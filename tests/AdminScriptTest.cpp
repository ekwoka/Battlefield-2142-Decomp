// The install's admin scripts on the embedded Python (Main/AdminScript.h):
// admin/default.py's TCP remote console on 127.0.0.1 with the seeded
// login, "exec" through host.rcon_invoke to the server's console, and
// standard_admin's settings through host.ss_getParam (needs
// $BF2142_GAME_DIR).
#include "TestMain.h"

#include "Console/Console.h"
#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "Main/AdminScript.h"
#include "Main/ServerAdmin.h"
#include "Main/ServerScriptBridge.h"
#include "Net/RemoteConsole.h"
#include "Python/ScriptHost.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace dice::hfe;

TEST(AdminScript_RemoteConsoleOverTcp)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
#ifdef _WIN32
	const int pid = 1;
#else
	const int pid = static_cast<int>(getpid());
#endif
	const uint16_t port = static_cast<uint16_t>(30000 + pid % 20000);
	// The overlay (sgl_getOverlayDirectory) with admin/default.cfg.
	const std::filesystem::path overlay =
		std::filesystem::temp_directory_path() / ("bf2142-admin-test-" + std::to_string(pid));
	std::filesystem::create_directories(overlay / "admin");
	{
		std::ofstream cfg(overlay / "admin" / "default.cfg");
		cfg << "port=" << port << "\npassword=loopback\n";
	}

	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	game::GameWorld round(templates, objects);
	game::GameLogic logic;
	round.startRound();
	logic.startRound();
	host::ServerAdmin admin;
	admin.settings.serverName = "Admin Py";
	admin.settings.autoBalanceTeam = true;
	admin.settings.teamRatioPercent = 80;
	CHECK(admin.maps.append({"minsk", "gpm_cq", 32}));
	std::atomic<bool> nextLevel {false};
	admin.hooks.runNextLevel = [&nextLevel] { nextLevel = true; };
	Console console;
	admin.registerConsole(console);
	game::ServerSettings scriptSettings;
	scriptSettings.mapName = "minsk";
	host::ServerScriptBridge bridge(round, logic, scriptSettings, "mods/bf2142", &admin, &console);
	bridge.overlay = overlay.string() + "/";
	std::string errors;
	script::ScriptHost scripts(bridge, {std::string(gameDir) + "/python", std::string(gameDir) + "/mods/bf2142/python"},
		[&errors](const std::string& line) {
			if (line.compare(0, 7, "stderr:") == 0) {
				errors += line + "\n";
			}
		});
	CHECK(scripts.ok());
	host::AdminScript adminScript;
	CHECK(adminScript.start(scripts, gameDir, "default", true));
	// standard_admin loaded, and its settings come from the sv settings.
	CHECK(scripts.run("import sys, host\n"
					  "assert 'admin.standard_admin.tk_punish' in sys.modules\n"
					  "assert host.ss_getParam('autoBalance') == 1\n"
					  "assert abs(host.ss_getParam('teamRatioPct') - 80.0) < 1e-6\n"
					  "assert host.ss_getParam('tkpNeeded') == 3 and host.ss_getParam('tkpEnabled') == 1\n"
					  "assert host.ss_getParam('ranked') == 0\n"));

	std::atomic<bool> done {false};
	bool connected = false, refused = false, loggedIn = false;
	std::string seed, name, maps, users, unknown;
	std::thread client([&] {
		net::RemoteConsoleClient wrong;
		if (wrong.connect("127.0.0.1", port, 5)) {
			std::string reply;
			refused = !wrong.login("not the password", &reply) && reply.find("Authentication failed") != std::string::npos;
			std::string denied;
			wrong.exec("sv.serverName", denied);
			refused = refused && denied.find("not authenticated") != std::string::npos;
			wrong.close();
		}
		net::RemoteConsoleClient rcon;
		connected = rcon.connect("127.0.0.1", port, 5);
		seed = rcon.seed();
		loggedIn = connected && rcon.login("loopback");
		if (loggedIn) {
			rcon.exec("sv.serverName", name);
			rcon.exec("maplist.list", maps);
			std::string ignored;
			rcon.exec("admin.runNextLevel", ignored);
			rcon.request("users", users);
			rcon.request("frobnicate", unknown);
		}
		done = true;
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (!done && std::chrono::steady_clock::now() < deadline) {
		adminScript.update(scripts);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	client.join();
	CHECK(refused);
	CHECK(connected && seed.size() == 16);
	CHECK(loggedIn);
	CHECK(name == "Admin Py\n");
	CHECK(maps == "0: \"minsk\" \"gpm_cq\" 32\n");
	CHECK(nextLevel);
	CHECK(users.find("active rcon users:") != std::string::npos && users.find("tcp: 127.0.0.1:") != std::string::npos);
	CHECK(unknown == "unknown command: 'frobnicate'\n");
	adminScript.shutdown(scripts);
	if (!errors.empty()) {
		std::fprintf(stderr, "%s", errors.c_str());
	}
	CHECK(errors.empty());
	std::error_code ignored;
	std::filesystem::remove_all(overlay, ignored);
}
