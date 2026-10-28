// A server's configuration and administration: the sv settings, the map
// list and its rotation, the bans, the admin.* console objects and
// rcon_invoke (Net/ServerSettings.h, Main/ServerAdmin.h), against a hosted
// game and the rewrite's own client over UDP on 127.0.0.1.
#include "TestMain.h"

#include "Console/Console.h"
#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "Main/HostedGame.h"
#include "Main/ServerAdmin.h"
#include "Net/GameClient.h"
#include "Net/GameProtocol.h"
#include "Net/RemoteConsole.h"
#include "Net/ServerSettings.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

namespace net = dice::hfe::net;
namespace host = dice::hfe::host;
using dice::hfe::Console;
namespace ConsoleAccess = dice::hfe::ConsoleAccess;

namespace {

bool pumpUntil(const std::function<void()>& step, const std::function<bool()>& done, int milliseconds = 4000)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	while (std::chrono::steady_clock::now() < deadline) {
		step();
		if (done()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return false;
}

std::string run(Console& console, const std::string& line)
{
	std::string output;
	console.execute(line, output, ConsoleAccess::Anyone);
	return output;
}

} // namespace

TEST(ServerAdmin_SettingsAndSave)
{
	net::ServerSettings sv;
	CHECK(sv.serverName == "Battlefield 2142" && sv.maxPlayers == 64 && sv.roundsPerMap == 3 && sv.adminScript == "default");
	CHECK(sv.set("SERVERNAME", "Loopback Test") && sv.serverName == "Loopback Test");
	CHECK(sv.set("teamRatioPercent", "75.5") && sv.teamRatioPercent == 75.5f);
	CHECK(sv.set("autoBalanceTeam", "1") && sv.autoBalanceTeam);
	CHECK(sv.set("maxPlayers", "not a number") && sv.maxPlayers == 64); // kept
	CHECK(!sv.set("noSuchSetting", "1"));
	std::string value;
	CHECK(sv.get("botSkill", value) && value == "0.5");
	// 0x00431880's order and quoting; sv.ranked only on a ranked server.
	const std::string saved = sv.save();
	CHECK(saved.rfind("sv.serverName \"Loopback Test\"\nsv.password \"\"\nsv.internet 0\n", 0) == 0);
	CHECK(saved.find("sv.interfaceIP \n") != std::string::npos);
	CHECK(saved.find("sv.adminScript \"default\"\n") != std::string::npos);
	CHECK(saved.find("sv.ranked") == std::string::npos && saved.find("sv.configFile") == std::string::npos);
	const std::string last = "sv.allowTitanMovement 1\n";
	CHECK(saved.size() > last.size() && saved.compare(saved.size() - last.size(), last.size(), last) == 0);
	sv.ranked = true;
	CHECK(sv.save().find("sv.useGlobalUnlocks 1\nsv.ranked 1\nsv.minUnlockLevel 0\n") != std::string::npos);
}

TEST(ServerAdmin_MapListRotationAndBans)
{
	net::MapList maps;
	maps.validate = [](const net::MapListEntry& e, std::string& error) {
		error = "map not found: " + e.level;
		return e.level != "nowhere";
	};
	CHECK(maps.append({"minsk", "gpm_cq", 32}));
	CHECK(maps.append({"Camp_Gibraltar", "gpm_ti", 64}));
	CHECK(!maps.append({"nowhere", "gpm_cq", 32}));
	CHECK(maps.insert(1, {"Suez_Canal", "gpm_cq", 16}));
	CHECK(maps.list() == "0: \"minsk\" \"gpm_cq\" 32\n1: \"Suez_Canal\" \"gpm_cq\" 16\n2: \"Camp_Gibraltar\" \"gpm_ti\" 64\n");
	CHECK(maps.save().rfind("maplist.append \"minsk\" \"gpm_cq\" 32\n", 0) == 0);
	CHECK(maps.current() == 0 && maps.next() == 1);

	host::ServerAdmin admin;
	admin.maps = maps;
	admin.settings.roundsPerMap = 2;
	CHECK(admin.nextRound().level == "minsk");          // round 2 of 2 on minsk
	CHECK(admin.nextRound().level == "Suez_Canal");     // on to the next
	admin.request = host::ServerAdmin::Request::Restart;
	CHECK(admin.nextRound().level == "Suez_Canal");
	admin.maps.setNext(0);                              // admin.nextLevel 0
	admin.request = host::ServerAdmin::Request::NextLevel;
	CHECK(admin.nextRound().level == "minsk" && admin.maps.current() == 0);
	admin.request = host::ServerAdmin::Request::RunLevel;
	admin.requestedLevel = {"Shuhia_Taiba", "gpm_cq", 32};
	CHECK(admin.nextRound().level == "Shuhia_Taiba" && admin.maps.current() == 0);
	admin.settings.roundsPerMap = 1;
	CHECK(admin.nextRound().level == "Suez_Canal");
	CHECK(admin.nextRound().level == "Camp_Gibraltar");
	CHECK(admin.nextRound().level == "minsk"); // round and round
	CHECK(maps.remove(5) == -1 && maps.remove(0) == 2 && maps.current() == 0);

	// 0x00505db0's periods: "perm" (or nothing), "round", seconds.
	net::BanList bans;
	const std::time_t now = 1000000000;
	CHECK(bans.addAddress("10.0.0.1:12345", "perm", now));
	CHECK(bans.addAddress("10.0.0.2", "round", now));
	CHECK(bans.addKey("abcdef", "60", now));
	CHECK(!bans.addKey("x", "-5", now));
	CHECK(bans.bannedAddress("10.0.0.1:999", now) && bans.bannedAddress("10.0.0.2", now));
	CHECK(bans.bannedKey("abcdef", now + 59) && !bans.bannedKey("abcdef", now + 61));
	CHECK(bans.save(now) == "admin.addKeyToBanList abcdef 1000000060\nadmin.addAddressToBanList 10.0.0.1 perm\n");
	bans.endRound();
	CHECK(bans.bannedAddress("10.0.0.1", now) && !bans.bannedAddress("10.0.0.2", now));
	// A saved ban reads back with its end time.
	net::BanList again;
	CHECK(again.addKey("abcdef", "1000000060", now) && again.bannedKey("abcdef", now + 30) && !again.bannedKey("abcdef", now + 90));
}

TEST(ServerAdmin_ConsoleAndRconInvoke)
{
	host::ServerAdmin admin;
	Console console;
	admin.registerConsole(console);
	CHECK(run(console, "sv.serverName \"Recomp Test\"").empty() && admin.settings.serverName == "Recomp Test");
	CHECK(run(console, "sv.serverName") == "Recomp Test");
	CHECK(run(console, "sv.roundsPerMap 2").empty() && admin.settings.roundsPerMap == 2);
	CHECK(run(console, "GameServerSettings.setTeamRatio 50").empty() && admin.settings.teamRatioPercent == 50);
	CHECK(run(console, "GameServerSettings.setPunishTeamKills 0").empty() && !admin.settings.tkPunishEnabled);
	CHECK(run(console, "maplist.append minsk gpm_cq 32") == "1");
	CHECK(run(console, "mapList.append Camp_Gibraltar gpm_cq") == "1"); // the size optional; any case
	CHECK(run(console, "maplist.mapCount") == "2" && run(console, "maplist.currentMap") == "0");
	CHECK(run(console, "admin.nextLevel") == "1");
	CHECK(run(console, "reservedSlots.addNick Somebody").empty() && admin.reserved.has("Somebody"));

	// admin.* through the hooks.
	int kicked = -1;
	bool banned = false;
	std::string said;
	bool nextLevel = false;
	admin.hooks.players = [] {
		host::AdminHooks::Player local, remote, bot;
		local.id = 0, local.name = "Host";
		remote.id = 1, remote.name = "Joiner", remote.remote = true, remote.address = "127.0.0.1:50000",
		remote.cdKeyHash = "0123456789abcdef0123456789abcdef";
		bot.id = 2, bot.name = "Bot", bot.ai = true;
		return std::vector<host::AdminHooks::Player> {local, remote, bot};
	};
	admin.hooks.kick = [&](int player, bool ban) {
		kicked = player;
		banned = ban;
		return true;
	};
	admin.hooks.sayAll = [&](const std::string& text) { said = text; };
	admin.hooks.runNextLevel = [&] { nextLevel = true; };
	CHECK(run(console, "admin.listPlayers")
		== "Id:  0 - Host is local.\nId:  1 - Joiner is remote ip: 127.0.0.1:50000 ->\n         CD-key hash: "
		   "0123456789abcdef0123456789abcdef \nId:  2 - Bot is local. is an AI bot.\n");
	CHECK(run(console, "admin.kickPlayer 7") == "Player not found.");
	CHECK(run(console, "admin.kickPlayer 1").empty() && kicked == 1 && !banned);
	CHECK(run(console, "admin.banPlayer 1 round").empty() && kicked == 1 && banned);
	CHECK(admin.bans.bannedAddress("127.0.0.1"));
	CHECK(run(console, "admin.listBannedAddresses") == "127.0.0.1 round\n");
	CHECK(run(console, "Game.sayAll \"hello server\"").empty() && said == "hello server");
	CHECK(run(console, "admin.runNextLevel").empty() && nextLevel);

	// rcon_invoke (0x004427b0): the output ends with a newline; nothing
	// for nothing; a ranked server refuses vars.set.
	CHECK(admin.rconInvoke(console, "maplist.list") == "0: \"minsk\" \"gpm_cq\" 32\n1: \"Camp_Gibraltar\" \"gpm_cq\" 0\n");
	CHECK(admin.rconInvoke(console, "sv.maxPlayers") == "64\n");
	CHECK(admin.rconInvoke(console, "").empty());
	CHECK(admin.rconInvoke(console, "sv.maxPlayers 32\nsv.maxPlayers") == "32\n");
	admin.settings.ranked = true;
	CHECK(admin.rconInvoke(console, "  vars.set x 1") == "Ranked server blocks vars.set from remote console\n");

	// The shipped ServerSettings.con through sv.* (with the game).
	if (const char* dir = std::getenv("BF2142_GAME_DIR")) {
		host::ServerAdmin shipped;
		shipped.setDefaultFiles(std::string(dir) + "/mods/bf2142");
		Console other;
		shipped.registerConsole(other);
		CHECK(shipped.loadSettings(other));
		CHECK(shipped.settings.serverName == "Battlefield 2142" && shipped.settings.maxPlayers == 64
			&& shipped.settings.voipServerPort == 55125 && shipped.settings.autoDemoHook == "adminutils/demo/rotate_demo.exe");
		CHECK(shipped.loadMapList(other) && shipped.maps.count() == 1 && shipped.maps.entries()[0].level == "minsk");
	}
}

// A hosted game with a server configuration: the settings and the whole
// map list go to the joining client, admin.listPlayers shows it, a ban
// kicks it and refuses it after; the in-game remote console (6/0x12 and
// 6/0x13) and a server message.
TEST(ServerAdmin_HostedKickBanAndMessages)
{
	dice::hfe::world::ObjectTemplateManager templates;
	dice::hfe::world::ObjectManager objects;
	dice::hfe::game::GameWorld round(templates, objects);
	dice::hfe::game::GameLogic logic;
	round.startRound();
	logic.startRound();

	host::ServerAdmin admin;
	admin.settings.serverName = "Admin Test";
	admin.settings.welcomeMessage = "welcome";
	CHECK(admin.maps.append({"Camp_Gibraltar", "gpm_cq", 32}));
	CHECK(admin.maps.append({"Minsk", "gpm_cq", 32}));
	host::HostSetup setup;
	setup.serverName = admin.settings.serverName;
	setup.level = "Camp_Gibraltar";
	setup.gameMode = "gpm_cq";
	setup.mapSize = 32;
	setup.gamePort = 0;
	setup.queryPort = 0;
	setup.settings = &admin.settings;
	setup.maps = &admin.maps;
	setup.bans = &admin.bans;
	host::HostedGame hosted(round, logic, templates, setup);
	CHECK(hosted.start());
	std::vector<std::string> commands;
	hosted.onRemoteCommand = [&](int player, const std::string& command) {
		commands.push_back(command);
		hosted.sendRemoteFeedback(player, "Authentication successful, rcon ready.\n");
	};
	Console console;
	admin.registerConsole(console);
	admin.hooks.players = [&] {
		std::vector<host::AdminHooks::Player> out;
		for (const host::HostedGame::ClientPlayer& c : hosted.clientPlayers()) {
			host::AdminHooks::Player p;
			p.id = c.player, p.name = c.name, p.remote = true, p.address = c.address, p.cdKeyHash = c.cdKeyHash;
			out.push_back(p);
		}
		return out;
	};
	admin.hooks.kick = [&](int player, bool ban) { return hosted.kickPlayer(player, ban); };
	admin.hooks.sayAll = [&](const std::string& text) { hosted.sayAll(text); };

	net::GameClient client;
	net::GameClientOptions options;
	options.playerName = "Joiner";
	CHECK(client.connect("127.0.0.1", hosted.gamePort(), options));
	const auto step = [&] {
		client.update();
		hosted.tick();
		round.tick(1.0f / 30);
	};
	CHECK(pumpUntil(step, [&] { return client.world().gameState() == 1 && client.world().roundStarted(); }));
	CHECK(client.world().serverName() == "Admin Test" && client.world().mapList().size() == 2);
	CHECK(client.world().settings().welcome.size() == 1 && client.world().settings().welcome[0] == "welcome");
	CHECK(pumpUntil(step, [&] {
		const net::RemotePlayer* me = client.world().player(client.world().localPlayer());
		return me && me->name == "Joiner";
	}));
	const int me = client.world().localPlayer();
	const std::string list = run(console, "admin.listPlayers");
	CHECK(list.find("Joiner is remote ip: 127.0.0.1:") != std::string::npos);

	// In-game rcon: the line to the server, its answer back.
	client.remoteCommand("login secret");
	bool answered = false;
	CHECK(pumpUntil(step, [&] {
		for (const net::PostRemoteInfo& m : client.world().takeMessages()) {
			if (m.category == net::kMsgNetworkCategory && m.id == net::kNeRemoteConsoleFeedback && m.payloadInt() == -1
				&& m.payload.find("rcon: Authentication successful") != std::string::npos) {
				answered = true;
			}
		}
		return answered;
	}));
	CHECK(commands.size() == 1 && commands[0] == "login secret");

	// A server message (Game.sayAll).
	CHECK(run(console, "Game.sayAll \"server here\"").empty());
	bool heard = false;
	CHECK(pumpUntil(step, [&] {
		for (const net::ChatLine& line : client.world().takeChat()) {
			heard = heard || (line.text == "server here" && line.channel == net::kChatServerMessage);
		}
		return heard;
	}));

	// admin.banPlayer: kicked (KickBan) and refused (0x16) after.
	CHECK(run(console, "admin.banPlayer " + std::to_string(me)).empty());
	CHECK(pumpUntil(step, [&] { return client.state() != net::GameClient::State::Connected; }));
	CHECK(admin.bans.bannedAddress("127.0.0.1"));
	net::GameClient again;
	CHECK(again.connect("127.0.0.1", hosted.gamePort(), options));
	CHECK(pumpUntil(
		[&] {
			again.update();
			hosted.tick();
		},
		[&] { return again.state() == net::GameClient::State::Refused; }));
	CHECK(again.connectResult().reason == 0x16);
	CHECK(run(console, "admin.removeAddressFromBanList 127.0.0.1").empty() && !admin.bans.bannedAddress("127.0.0.1"));
}
