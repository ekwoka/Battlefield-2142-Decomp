// The main menu's natives (Main/MenuBindings.h) with a fake account service
// and master server: the error mapping, the natives' states, and (with
// $BF2142_GAME_DIR) the shipped mainMenu.dfm driven from the login page to
// joining a server.
#include "Console/Console.h"
#include "IO/File/ZipArchive.h"
#include "IO/Localization/Localization.h"
#include "Input/ControlMap.h"
#include "Main/EndOfRoundBindings.h"
#include "Main/MenuBindings.h"
#include "Main/SettingsObjects.h"
#include "Net/Demo.h"
#include "TestMain.h"
#include "UI/Player/Player.h"

#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>

using namespace dice::hfe;
using namespace dice::hfe::ui;

namespace {

struct Fake {
	bool reachable = true;
	int logins = 0;
	int queries = 0;
	std::string joinedHost;
	uint16_t joinedPort = 0;

	menu::MenuServices services()
	{
		menu::MenuServices s;
		s.checkOnline = [this] { return reachable; };
		s.loginAccount = [this](const std::string& name, const std::string& password) {
			++logins;
			menu::AccountReply r;
			if (name == "nobody") {
				r.error = 101;
			} else if (password != "secret") {
				r.error = 122;
			} else {
				r.soldiers = {"Ghost", "Viper"};
			}
			return r;
		};
		s.loginSoldier = [](const std::string&) {
			menu::AccountReply r;
			r.profileId = 42;
			return r;
		};
		s.logoutAccount = [] {};
		s.fetchServers = [this](std::string*) {
			++queries;
			net::ServerList list;
			for (int k = 0; k < 12; ++k) {
				net::ServerEntry e;
				e.ip = "10.0.0." + std::to_string(k + 1);
				e.port = 29900;
				e.fields = {{"hostname", "Fake server " + std::to_string(k + 1)}, {"mapname", "camp_gibraltar"},
					{"gametype", "gpm_cq"}, {"numplayers", std::to_string(k)}, {"maxplayers", "64"},
					{"hostport", std::to_string(17567 + k)}, {"gamevariant", "bf2142"}, {"gamever", "1.10.112.0"}};
				list.servers.push_back(e);
			}
			return std::optional<net::ServerList>(list);
		};
		return s;
	}
};

avm1::Value call(avm1::Interpreter& vm, NativeRegistry& natives, const char* object, const char* member,
	std::vector<avm1::Value> args = {})
{
	return natives.call(vm, object, member, args);
}

} // namespace

TEST(MenuBindings_ErrorCodes)
{
	CHECK(menu::profileErrorFromFesl(0) == menu::kProfileErrorNone);
	CHECK(menu::profileErrorFromFesl(122) == menu::kProfileErrorPasswordWrong);
	CHECK(menu::profileErrorFromFesl(101) == menu::kProfileErrorAccountNotFound);
	CHECK(menu::profileErrorFromFesl(103) == menu::kProfileErrorAccountBanned);
	CHECK(menu::profileErrorFromFesl(12345) == menu::kProfileErrorUnhandled);
	CHECK(std::string(menu::profileErrorKey(8)) == "LOCERROR_passwordwrong");
	CHECK(std::string(menu::profileErrorKey(15)) == "LOCERROR_accountnotfound");
	CHECK(std::string(menu::profileErrorKey(39)) == "LOCERROR_unhandled");
}

// The natives' states without a movie: the polling protocols the pages use.
TEST(MenuBindings_Natives)
{
	Fake fake;
	NativeRegistry natives;
	natives.setLocalizer([](const std::string& key) { return key == "LOCERROR_passwordwrong" ? "Wrong password." : key; });
	menu::MenuBindings bindings(natives, fake.services());
	avm1::Interpreter vm;

	// Version check: over at once, no patch.
	call(vm, natives, "dice.bf2.Logic", "checkVersion", {avm1::Value(true)});
	CHECK(!avm1::toBoolean(call(vm, natives, "dice.bf2.Logic", "checkVersion", {avm1::Value(false)})));
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.Logic", "getVersionResult")) == 0);

	// Online check: started, then complete.
	call(vm, natives, "dice.bf2.Profile", "checkOnlineStatus", {avm1::Value(true)});
	bindings.finishJobs();
	CHECK(avm1::toBoolean(call(vm, natives, "dice.bf2.Profile", "checkOnlineStatus", {avm1::Value(false)})));
	CHECK(avm1::toBoolean(call(vm, natives, "dice.bf2.Profile", "isOnline")));

	// A wrong password: the game's text for error 8.
	call(vm, natives, "dice.bf2.Profile", "connectToMasterAccount",
		{avm1::Value("tester"), avm1::Value("wrong"), avm1::Value(false)});
	bindings.finishJobs();
	CHECK(avm1::toBoolean(call(vm, natives, "dice.bf2.Profile", "isTransactionComplete")));
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.Profile", "getLastError")) == 8);
	CHECK(vm.toString(call(vm, natives, "dice.bf2.Profile", "getLastErrorString")) == "Wrong password.");
	CHECK(!avm1::toBoolean(call(vm, natives, "dice.bf2.Profile", "isConnected")));

	// The right one: connected, the soldiers listed, one picked.
	call(vm, natives, "dice.bf2.Profile", "connectToMasterAccount",
		{avm1::Value("tester"), avm1::Value("secret"), avm1::Value(true)});
	bindings.finishJobs();
	CHECK(avm1::toBoolean(call(vm, natives, "dice.bf2.Profile", "isTransactionComplete")));
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.Profile", "getLastError")) == 0);
	CHECK(avm1::toBoolean(call(vm, natives, "dice.bf2.Profile", "isConnected")));
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.Profile", "getLoginStatus")) == 2);
	CHECK(natives.listRows("soldiers").size() == 2);
	CHECK(vm.toString(call(vm, natives, "dice.bf2.Profile", "getLastOnlineUser")) == "tester");
	const std::string remembered = vm.toString(call(vm, natives, "dice.bf2.Profile", "getEncryptedLogin"));
	CHECK(!remembered.empty() && remembered.find("secret") == std::string::npos);
	call(vm, natives, "dice.bf2.Profile", "connectToSubAccount", {avm1::Value("Viper")});
	bindings.finishJobs();
	CHECK(avm1::toBoolean(call(vm, natives, "dice.bf2.Profile", "isTransactionComplete")));
	CHECK(bindings.soldierName() == "Viper");

	// The remembered login works without the password.
	call(vm, natives, "dice.bf2.Profile", "logout");
	CHECK(!bindings.loggedIn());
	call(vm, natives, "dice.bf2.Profile", "connectToMasterAccountEncrypted", {avm1::Value(remembered), avm1::Value(true)});
	bindings.finishJobs();
	call(vm, natives, "dice.bf2.Profile", "isTransactionComplete");
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.Profile", "getLastError")) == 0);
	CHECK(fake.logins == 3);

	// Offline: an account never seen gets a soldier of its name.
	CHECK(avm1::toBoolean(call(vm, natives, "dice.bf2.Logic", "setOfflineMode", {avm1::Value("lonely")})));
	CHECK(bindings.offlineMode() && !bindings.online());
	CHECK(natives.listRows("soldiers").size() == 1 && natives.listRows("soldiers")[0][0] == "lonely");
	call(vm, natives, "dice.bf2.Profile", "connectToSubAccount", {avm1::Value("lonely")});
	CHECK(avm1::toBoolean(call(vm, natives, "dice.bf2.Profile", "isTransactionComplete")));
	CHECK(bindings.soldierName() == "lonely");

	// Messages: newest first.
	bindings.raiseMessage(3, "serverfull");
	call(vm, natives, "dice.MessageHandler", "raiseMessage", {avm1::Value("boom"), avm1::Value(1), avm1::Value(7)});
	CHECK(vm.toNumber(call(vm, natives, "dice.MessageHandler", "getNumMessages")) == 2);
	CHECK(vm.toString(call(vm, natives, "dice.MessageHandler", "getLastText")) == "boom");
	CHECK(vm.toNumber(call(vm, natives, "dice.MessageHandler", "getLastCode")) == 7);
	call(vm, natives, "dice.MessageHandler", "removeLast");
	CHECK(vm.toNumber(call(vm, natives, "dice.MessageHandler", "getLastType")) == 3);

	// Joining: "ip gameport queryport".
	CHECK(!avm1::toBoolean(call(vm, natives, "dice.bf2.Multiplay", "joinServer", {avm1::Value("")})));
	bindings.onJoin = [&fake](const std::string& host, uint16_t port) {
		fake.joinedHost = host;
		fake.joinedPort = port;
	};
	CHECK(avm1::toBoolean(call(vm, natives, "dice.bf2.Multiplay", "joinServer", {avm1::Value("10.0.0.2 17568 29900")})));
	CHECK(fake.joinedHost == "10.0.0.2" && fake.joinedPort == 17568);
}

// A server that does not answer the handshake: the menu hears of it.
TEST(MenuBindings_JoinFails)
{
	Fake fake;
	menu::MenuServices services = fake.services();
	services.connectServer = [](const std::string&, uint16_t) {
		net::ConnectResult r;
		r.outcome = net::ConnectResult::Outcome::Refused;
		r.reason = 0x1e;
		return r;
	};
	NativeRegistry natives;
	menu::MenuBindings bindings(natives, services);
	avm1::Interpreter vm;
	CHECK(avm1::toBoolean(call(vm, natives, "dice.bf2.Multiplay", "joinServer", {avm1::Value("10.0.0.9 17567 29900")})));
	bindings.finishJobs();
	CHECK(vm.toNumber(call(vm, natives, "dice.MessageHandler", "getNumMessages")) == 1);
	CHECK(vm.toNumber(call(vm, natives, "dice.MessageHandler", "getLastType")) == 3);
	CHECK(vm.toString(call(vm, natives, "dice.MessageHandler", "getLastText")) == "serverfull");
	CHECK(bindings.joinOutcome() == "refused: server full");
}

namespace {

struct Menu {
	Player player;
	std::shared_ptr<io::ZipArchive> client;
	std::shared_ptr<io::ZipArchive> fonts;
	io::Localization strings;

	// `withFonts`: the engine's fonts too ("/Fonts/..." from Fonts_client.zip),
	// so text fields lay out and draw.
	bool load(const std::string& gameDir, bool withFonts = false)
	{
		const std::string mod = gameDir + "/mods/bf2142/";
		const auto server = io::ZipArchive::open(mod + "Menu_server.zip");
		client = io::ZipArchive::open(mod + "Menu_client.zip");
		std::vector<uint8_t> dfm, csv;
		if (!server || !client || !server->read("External/FlashMenu/mainMenu.dfm", dfm)) {
			return false;
		}
		for (const char* file : {"strings.csv", "strings_patch.csv"}) {
			std::FILE* f = std::fopen((mod + "Localization/" + file).c_str(), "rb");
			if (f) {
				csv.clear();
				uint8_t buffer[65536];
				for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;) {
					csv.insert(csv.end(), buffer, buffer + n);
				}
				std::fclose(f);
				strings.load(csv);
			}
		}
		if (withFonts) {
			fonts = io::ZipArchive::open(mod + "Fonts_client.zip");
		}
		player.setFileReader([this](const std::string& path, std::vector<uint8_t>& out) {
			if (fonts && path.rfind("/Fonts/", 0) == 0) {
				return fonts->read(path.substr(7), out);
			}
			return client->read("External/FlashMenu/" + path, out);
		});
		player.natives().setLocalizer([this](const std::string& key) { return strings.text(key); });
		return player.loadDfm(dfm);
	}
	void run(double seconds)
	{
		for (int i = 0; i < static_cast<int>(seconds * 30); ++i) {
			player.tick();
		}
	}
	// Clicks the middle of a clickable object (by its target's end).
	bool click(const std::string& target)
	{
		for (const auto& c : player.clickables()) {
			if (c.target.size() >= target.size() && c.target.compare(c.target.size() - target.size(), target.size(), target) == 0) {
				player.mouseMove(c.x + c.width / 2, c.y + c.height / 2);
				player.tick();
				player.mouseDown();
				player.tick();
				player.mouseUp();
				return true;
			}
		}
		return false;
	}
	void type(const std::string& text)
	{
		for (char ch : text) {
			player.keyDown(std::toupper(static_cast<unsigned char>(ch)), static_cast<uint8_t>(ch));
			player.keyUp(std::toupper(static_cast<unsigned char>(ch)));
		}
	}
	std::string text(const std::string& path)
	{
		return player.interpreter()->toString(player.evaluate(path));
	}
};

} // namespace

// The shipped menu from the login page to a join, against the fakes.
TEST(MenuBindings_MainMenu)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Fake fake;
	Menu menu;
	menu::MenuBindings bindings(menu.player.natives(), fake.services());
	bindings.onJoin = [&fake](const std::string& host, uint16_t port) {
		fake.joinedHost = host;
		fake.joinedPort = port;
	};
	CHECK(menu.load(gameDir));
	menu.run(3);

	// A wrong password: the login error popup with the game's text.
	CHECK(menu.click("login.mcAccountName.mcInput.inputField"));
	menu.type("tester");
	CHECK(menu.click("login.mcPassword.mcInput.inputField"));
	menu.type("wrong");
	CHECK(menu.click("login.btnLogin"));
	menu.run(0.5);
	bindings.finishJobs();
	menu.run(1);
	CHECK(menu.text("sErrorLocTextHeader") == "HEADER_loginerror");
	CHECK(menu.text("sErrorLocText") == menu.strings.text("LOCERROR_passwordwrong"));
	CHECK(!bindings.loggedIn());

	// Closed, the right password: the soldier page lists the account's soldiers.
	CHECK(menu.click("MessageHandler.btnOk"));
	menu.run(1);
	CHECK(menu.click("login.mcPassword.mcInput.inputField"));
	for (int i = 0; i < 5; ++i) {
		menu.player.keyDown(8, 8);
		menu.player.keyUp(8);
	}
	menu.type("secret");
	CHECK(menu.click("login.btnLogin"));
	menu.run(0.5);
	bindings.finishJobs();
	menu.run(3);
	CHECK(bindings.loggedIn() && bindings.accountName() == "tester");
	const std::string rows = "login.ScrollTainer.m_mcContainer.contents.holder_list.holder_list.";
	CHECK(menu.text(rows + "row0.cell0.text") == "Ghost");
	CHECK(menu.text(rows + "row1.cell0.text") == "Viper");

	// The soldier (the page's SELECT path: the row handlers need the player
	// to resolve their variables through `this`, so the root's loginSoldier
	// is called as SELECT would).
	avm1::Value result;
	menu.player.interpreter()->callMethod(menu.player.root(), "loginSoldier", {avm1::Value("Ghost"), avm1::Value("")}, &result);
	menu.run(0.5);
	bindings.finishJobs();
	menu.run(5);
	CHECK(bindings.soldierName() == "Ghost");
	CHECK(avm1::toBoolean(menu.player.evaluate("start._visible")));

	// Multiplayer: the live servers under the list's header rows.
	CHECK(menu.click("FlashMenu.btnMultiplay"));
	menu.run(1);
	bindings.finishJobs();
	menu.run(4);
	CHECK(fake.queries == 1);
	const std::string servers = "multiplay.ScrollTainer.m_mcContainer.contents.holder_list.holder_list.";
	CHECK(menu.text(servers + "row2.cell0.text") == menu.strings.text("SERVERBROWSER_liveservers"));
	CHECK(menu.text(servers + "row3.cell0.text") == "Fake server 1");
	CHECK(menu.text(servers + "row4.cell1.text") == "1/64 (0)");
	CHECK(menu.text(servers + "row4.cell3.text") == "camp gibraltar");
	CHECK(menu.text(servers + "row4.cell4.text") == menu.strings.text("TEXT_gpm_cq"));

	// A server picked and joined.
	CHECK(menu.click(servers + "row4.mcClick"));
	menu.run(0.5);
	CHECK(menu.click("multiplay.btnJoinServer"));
	menu.run(1);
	CHECK(fake.joinedHost == "10.0.0.2");
	CHECK(fake.joinedPort == 17568);
	for (const std::string& e : menu.player.scriptErrors()) {
		std::fprintf(stderr, "mainMenu script error: %s\n", e.c_str());
	}
	CHECK(menu.player.scriptErrors().empty());
}

TEST(MenuBindings_StartGame)
{
	Fake fake;
	NativeRegistry natives;
	menu::MenuBindings bindings(natives, fake.services());
	std::optional<menu::GameSetup> started;
	bindings.onStartGame = [&started](const menu::GameSetup& setup) { started = setup; };
	bindings.setMaps({{"Minsk", "gpm_cq", 32}, {"Belgrade", "gpm_sp", 16}, {"Camp_Gibraltar_Coop", "gpm_sp", 32}});
	avm1::Interpreter vm;

	// The singleplayer page's maps: "Name - size" for its mode only.
	call(vm, natives, "dice.bf2.Multiplay", "setFilterGameMode", {avm1::Value("gpm_sp")});
	const auto& available = natives.listRows("mapListAvailable");
	CHECK(available.size() == 2);
	CHECK(!available.empty() && available[0][0] == "Belgrade - 16");
	CHECK(available.size() == 2 && available[1][0] == "Camp Gibraltar Coop - 32");
	CHECK(vm.toString(call(vm, natives, "dice.bf2.Multiplay", "getMapNameFromId", {avm1::Value("Camp_Gibraltar_Coop")}))
		== "Camp Gibraltar Coop");

	// Nothing chosen: nothing starts.
	CHECK(!avm1::toBoolean(call(vm, natives, "dice.bf2.Multiplay", "createServer")));
	CHECK(!started);

	// Settings kept by name, with the game's defaults before.
	// The host page starts as ServerSettings.con's (0x00499430): 64
	// players, 3 rounds a map, no autobalance, "Battlefield 2142".
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.ServerSettings", "getMaxPlayers")) == 64);
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.ServerSettings", "getRoundsPerMap")) == 3);
	CHECK(!avm1::toBoolean(call(vm, natives, "dice.bf2.ServerSettings", "getAutoBalanceTeam")));
	CHECK(vm.toString(call(vm, natives, "dice.bf2.ServerSettings", "getServerName")) == "Battlefield 2142");
	call(vm, natives, "dice.bf2.ServerSettings", "setTicketRatio", {avm1::Value(200)});
	call(vm, natives, "dice.bf2.GeneralSettings", "setMaxBots", {avm1::Value(8)});
	call(vm, natives, "dice.bf2.GeneralSettings", "setBotSkill", {avm1::Value(0.75)});
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.ServerSettings", "getTicketRatio")) == 200);

	// The second map chosen, then the first, the first removed again.
	call(vm, natives, "dice.bf2.Multiplay", "addMapIndexToMapList", {avm1::Value(1)});
	call(vm, natives, "dice.bf2.Multiplay", "addMapIndexToMapList", {avm1::Value(0)});
	CHECK(natives.listRows("mapListChosen").size() == 2);
	call(vm, natives, "dice.bf2.Multiplay", "removeMapfromMapList", {avm1::Value(1)});
	CHECK(natives.listRows("mapListChosen").size() == 1);
	CHECK(avm1::toBoolean(call(vm, natives, "dice.bf2.Multiplay", "createServer")));
	CHECK(started && started->maps.size() == 1);
	if (started && !started->maps.empty()) {
		CHECK(started->maps[0].level == "Camp_Gibraltar_Coop" && started->maps[0].gameMode == "gpm_sp"
			&& started->maps[0].size == 32);
		CHECK(started->ticketRatio == 200 && started->bots == 8 && started->botSkill == 0.75f);
	}
}

// The shipped menu after an offline login, drawn with the engine's fonts:
// the frames carry text, and a frame drawn twice is the same. With
// $BF2142_MENU_BENCH set, prints the time Player::frame() takes.
TEST(MenuBindings_FrameText)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Fake fake;
	fake.reachable = false;
	Menu menu;
	menu::MenuBindings bindings(menu.player.natives(), fake.services());
	CHECK(menu.load(gameDir, true));
	menu.run(3);
	CHECK(menu.click("login.mcAccountName.mcInput.inputField"));
	menu.type("tester");
	CHECK(menu.click("login.btnLogin"));
	menu.run(0.5);
	bindings.finishJobs();
	menu.run(2);
	CHECK(menu.click("MessageHandler.btnOk"));
	menu.run(2);
	CHECK(menu.click("login.ScrollTainer.m_mcContainer.contents.holder_list.holder_list.row0"));
	CHECK(menu.click("login.btnLogin"));
	menu.run(5);
	CHECK(bindings.soldierName() == "tester");

	// A digest of a frame's batches (FNV-1a over textures and vertices).
	const auto digest = [](const MenuFrame& frame, size_t& textQuads) {
		uint64_t h = 1469598103934665603ull;
		const auto mix = [&h](const void* p, size_t n) {
			for (size_t i = 0; i < n; ++i) {
				h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
			}
		};
		textQuads = 0;
		for (const MenuBatch& b : frame.batches) {
			mix(b.texture.data(), b.texture.size());
			mix(b.vertices.data(), b.vertices.size() * sizeof(MenuVertex));
			if (b.texture.rfind("Fonts/", 0) == 0) {
				textQuads += b.vertices.size() / 6;
			}
		}
		return h;
	};
	size_t quads = 0, quadsAgain = 0;
	const uint64_t first = digest(menu.player.frame(), quads);
	const uint64_t again = digest(menu.player.frame(), quadsAgain);
	CHECK(quads > 100);
	CHECK(first == again && quads == quadsAgain);
	if (std::getenv("BF2142_MENU_BENCH")) {
		const int frames = 300;
		const auto start = std::chrono::steady_clock::now();
		for (int i = 0; i < frames; ++i) {
			menu.player.frame();
		}
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		std::fprintf(stderr, "menu frame: %.3f ms (%zu text quads, digest %016llx)\n", ms / frames, quads,
			static_cast<unsigned long long>(first));
	}
}

// The shipped menu: offline login, the singleplayer page's instant action.
TEST(MenuBindings_InstantAction)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Fake fake;
	fake.reachable = false;
	Menu menu;
	menu::MenuBindings bindings(menu.player.natives(), fake.services());
	bindings.setMaps({{"Belgrade", "gpm_sp", 16}, {"Minsk", "gpm_cq", 32}});
	std::optional<menu::GameSetup> started;
	bindings.onStartGame = [&started](const menu::GameSetup& setup) { started = setup; };
	CHECK(menu.load(gameDir));
	menu.run(3);
	CHECK(menu.click("login.mcAccountName.mcInput.inputField"));
	menu.type("tester");
	CHECK(menu.click("login.btnLogin"));
	menu.run(0.5);
	bindings.finishJobs();
	menu.run(2);
	// "No internet connection": OK plays offline.
	CHECK(menu.click("MessageHandler.btnOk"));
	menu.run(2);
	CHECK(bindings.offlineMode());
	CHECK(menu.click("login.ScrollTainer.m_mcContainer.contents.holder_list.holder_list.row0"));
	CHECK(menu.click("login.btnLogin"));
	menu.run(5);
	CHECK(bindings.soldierName() == "tester");

	CHECK(menu.click("FlashMenu.btnSingleplay"));
	menu.run(3);
	const std::string rows = "singleplay.mc_singleplayer.mcMapsAvailable.m_mcContainer.contents.holder_list.holder_list.";
	CHECK(menu.text(rows + "row0.cell0.text") == "Belgrade - 16");
	CHECK(menu.click("singleplay.mc_singleplayer.btnInstantAction"));
	menu.run(1);
	CHECK(menu.click("MessageHandler.btnOk"));
	menu.run(1);
	CHECK(started && !started->maps.empty() && started->maps[0].level == "Belgrade");
	if (started) {
		CHECK(started->bots == 16 && started->botsIncludeHumans && started->ticketRatio == 200 && started->spawnTime == 15);
	}
	for (const std::string& e : menu.player.scriptErrors()) {
		std::fprintf(stderr, "mainMenu script error: %s\n", e.c_str());
	}
	CHECK(menu.player.scriptErrors().empty());
}

// The controls page: keys shown by the engine's input number, and a key
// pressed bound in a slot.
TEST(MenuBindings_Controls)
{
	Fake fake;
	NativeRegistry natives;
	menu::MenuBindings bindings(natives, fake.services());
	input::ControlMaps controls;
	input::ControlMaps::Map& infantry = controls.create(input::ControlMaps::kInfantry);
	infantry["c_PIThrottle"] = {{input::Binding::Kind::Key, "IDFKeyboard", "IDKey_W", 1},
		{input::Binding::Kind::Key, "IDFKeyboard", "IDKey_S", -1}};
	infantry["c_PIFire"] = {{input::Binding::Kind::Button, "IDFMouse", "IDButton_0"}};
	bindings.setControls(&controls);
	avm1::Interpreter vm;
	call(vm, natives, "dice.bf2.ControlSettings", "setActiveControlMap", {avm1::Value("0")});
	const double throttle = input::inputNumber("c_PIThrottle"), fire = input::inputNumber("c_PIFire");
	CHECK(throttle == 3);
	const auto shown = [&](double id, bool secondary, bool negative) {
		return vm.toString(call(vm, natives, "dice.bf2.ControlSettings", "getMappedString",
			{avm1::Value(id), avm1::Value(secondary), avm1::Value(negative)}));
	};
	CHECK(shown(throttle, false, false) == "W");
	CHECK(shown(throttle, false, true) == "S");
	CHECK(shown(fire, false, false) == "Mouse 1");
	CHECK(shown(fire, true, false).empty());
	// Q pressed, then bound as fire's secondary.
	bindings.setLastInput("IDKey_Q");
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.ControlSettings", "mapInput",
		{avm1::Value(fire), avm1::Value(true), avm1::Value(false), avm1::Value(false)})) == 0); // 0: bound
	CHECK(shown(fire, true, false) == "Q");
	CHECK(controls.pressed(input::ControlMaps::kInfantry, "c_PIFire",
		[](const std::string& control) { return control == "IDKey_Q"; }));
	// A gamepad's button as fire's secondary, its stick pushed up (negative)
	// as throttle's forward: an inverted axis mapping.
	bindings.setLastInput("IDFGameController_0", "IDButton_5", 1);
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.ControlSettings", "mapInput",
		{avm1::Value(fire), avm1::Value(true), avm1::Value(false), avm1::Value(false)})) == 0); // 0: bound
	CHECK(shown(fire, true, false) == "Joy Button 5");
	bindings.setLastInput("IDFGameController_0", "IDAxis_1", -1);
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.ControlSettings", "mapInput",
		{avm1::Value(throttle), avm1::Value(true), avm1::Value(false), avm1::Value(false)})) == 0);
	const input::Binding* axis = controls.binding(input::ControlMaps::kInfantry, "c_PIThrottle", 1, 1);
	CHECK(axis && axis->kind == input::Binding::Kind::Axis && axis->sign == -1 && axis->device == "IDFGameController_0");
	CHECK(shown(throttle, true, false) == "Joy Axis 1");
	CHECK(shown(throttle, false, false) == "W"); // the primary kept
	// With the game's strings: the localized CONTROLLER_ key.
	natives.setLocalizer([](const std::string& key) {
		return key == "CONTROLLER_IDFGameController_IDButton_5" ? std::string("Joy Button 5 (loc)") : key;
	});
	CHECK(shown(fire, true, false) == "Joy Button 5 (loc)");

	// A key another input has (W, the throttle's): error 1, nothing bound
	// until forceSetLastInput, which takes it off the throttle. The prompt
	// names the throttle as the original's table does.
	natives.setLocalizer([](const std::string& key) { return key == "HEADER_forward" ? std::string("Forward") : key; });
	bindings.setLastInput("IDKey_W");
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.ControlSettings", "mapInput",
		{avm1::Value(fire), avm1::Value(false), avm1::Value(false), avm1::Value(false)})) == 1);
	CHECK(vm.toString(call(vm, natives, "dice.bf2.ControlSettings", "getLastDuplicateInput")) == "Forward");
	CHECK(shown(fire, false, false) == "Mouse 1");
	call(vm, natives, "dice.bf2.ControlSettings", "forceSetLastInput");
	CHECK(shown(fire, false, false) == "W");
	CHECK(shown(throttle, false, false).empty());
	CHECK(menu::MenuBindings::controlHeader(input::ControlMaps::kInfantry, "c_PIThrottle", true) == "HEADER_backwark");

	// The game map (5) numbers its inputs as c_GI: 55 the scoreboard.
	input::ControlMaps::Map& game = controls.create(input::ControlMaps::kGame);
	game["c_GIShowScoreboard"] = {{input::Binding::Kind::Key, "IDFKeyboard", "IDKey_Tab", 1}};
	call(vm, natives, "dice.bf2.ControlSettings", "setActiveControlMap", {avm1::Value("5")});
	CHECK(shown(55, false, false) == "Tab");
	CHECK(menu::MenuBindings::controlHeader(input::ControlMaps::kGame, "c_GIShowScoreboard", false) == "HEADER_scoreboard");
}

// The audio page's radios (0x100108c0, 0x100109c0): the provider and the
// sound quality by name, what the page set coming back.
TEST(MenuBindings_AudioRadios)
{
	Fake fake;
	NativeRegistry natives;
	menu::MenuBindings bindings(natives, fake.services());
	avm1::Interpreter vm;
	const auto get = [&](const char* member) { return vm.toString(call(vm, natives, "dice.bf2.AudioSettings", member)); };
	CHECK(get("getSelectedProvider") == "software" && get("getSelectedSoundQuality") == "Medium");
	call(vm, natives, "dice.bf2.AudioSettings", "setProvider", {avm1::Value("Hardware")});
	call(vm, natives, "dice.bf2.AudioSettings", "setSoundQuality", {avm1::Value("UltraHigh")});
	CHECK(get("getSelectedProvider") == "hardware" && get("getSelectedSoundQuality") == "UltraHigh");
}

// SwiffHost's and MenuTeamManager's console objects (credits.con,
// gameModes.con, menuTeams.con) and the credits roll's slots: 40 lines 20 px
// apart from the stage's bottom, moving up at the speed (twips a second,
// 500..4000), recycled at the top and looping.
TEST(MenuBindings_MenuHostAndCredits)
{
	Console console;
	ui::CreditsScroller scroller;
	menu::MenuHost host;
	host.scroller = &scroller;
	menu::registerMenuHostConsole(console, host);
	std::string out;
	for (const char* line : {"swiffHost.setScrollFadeSpeed 200", "swiffHost.setScrollSpeed 800",
			 "swiffHost.setScrollerRect 200 195 1000 648", "swiffHost.addCredit \"DIGITAL ILLUSIONS CE\" 2",
			 "swiffHost.addCredit \"\" 0", "swiffHost.addCredit \"CREDITS_DICE_producer\" 1", "swiffHost.addGameMode \"gpm_cq\" \"Conquest\"",
			 "MenuTeamManager.addTeam \"EU\" \"objects/kits/eu\"", "MenuTeamManager.addKit \"EU_Recon.con\"",
			 "swiffHost.addAward 0 105 2 7"}) {
		CHECK(console.execute(line, out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	}
	CHECK(scroller.credits().size() == 3 && scroller.credits()[2].size == 1 && scroller.fadeSpeed() == 200);
	CHECK(scroller.speed() == 800);
	scroller.setSpeed(100);
	CHECK(scroller.speed() == 500);
	scroller.setSpeed(800);
	CHECK(host.gameModeName("GPM_CQ") == "Conquest" && host.gameModeName("gpm_ti") == "N/A");
	CHECK(host.teams.size() == 1 && host.teams[0].kits.size() == 1 && host.teams[0].path == "objects/kits/eu");
	CHECK(host.awards.size() == 1 && host.awards[0].kind == 1 && host.awards[0].nameKey == "AWARD_NAME_105_2");
	// On: 40 slots from 768 px, 20 px apart, lines in turn (3 lines loop).
	scroller.enable(true);
	CHECK(scroller.slots().size() == 40 && scroller.slots()[0].y == 15360 && scroller.slots()[1].y == 15760);
	CHECK(scroller.slots()[3].line == 0 && scroller.slots()[4].line == 1);
	// 20 s at 800 twips a second: 16000 twips up; the first slots gone.
	for (int i = 0; i < 200; ++i) {
		scroller.addTime(0.1);
	}
	CHECK(scroller.slots().size() == 40 && scroller.slots()[0].y >= -400 && scroller.slots()[0].y < 0);
	CHECK(std::fabs(scroller.slots()[1].y - scroller.slots()[0].y - 400) < 0.01f);
	CHECK(scroller.slots().back().line == (scroller.slots().front().line + 39) % 3);
	scroller.enable(false);
	CHECK(scroller.slots().empty());
}

// The favourites (Multiplay, GeneralSettings' list): a listed server's star,
// a server by its address, the quick list's "Favorites" rows, removal,
// and the profile's General.con lines.
TEST(MenuBindings_Favourites)
{
	Fake fake;
	NativeRegistry natives;
	menu::MenuBindings bindings(natives, fake.services());
	menu::SettingsObjects objects;
	bindings.setSettingsObjects(&objects);
	avm1::Interpreter vm;
	const auto mp = [&](const char* member, std::vector<avm1::Value> args = {}) {
		return call(vm, natives, "dice.bf2.Multiplay", member, std::move(args));
	};
	// The quick list before: Favorites, a gap, Live servers, then the 12.
	natives.call(vm, "dice.bf2.Multiplay", "startBrowse", {});
	bindings.finishJobs();
	mp("isBrowsing");
	auto quick = natives.listRows("serverListQuick");
	CHECK(quick.size() == 15);
	// A listed server's star: added with its name, refused the second time.
	CHECK(avm1::toBoolean(mp("addFavouriteServer", {avm1::Value("10.0.0.3 17569 29900")})));
	CHECK(!avm1::toBoolean(mp("addFavouriteServer", {avm1::Value("10.0.0.3 17569 29900")})));
	CHECK(!avm1::toBoolean(mp("addFavouriteServer", {avm1::Value("10.9.9.9 16567 29900")}))); // not listed
	// By address: a bad one and a duplicate raise the game's messages.
	CHECK(!avm1::toBoolean(mp("validateIp", {avm1::Value("10.0.0.256")})));
	CHECK(!avm1::toBoolean(mp("validateIp", {avm1::Value("10.0.0")})));
	CHECK(avm1::toBoolean(mp("validateIp", {avm1::Value("192.168.1.20")})));
	CHECK(!avm1::toBoolean(mp("addFavourite", {avm1::Value("Mine"), avm1::Value("1.2.3"), avm1::Value(16567)})));
	CHECK(vm.toString(call(vm, natives, "dice.MessageHandler", "getLastText")) == "invalidip");
	CHECK(avm1::toBoolean(mp("addFavourite", {avm1::Value("Mine"), avm1::Value("192.168.1.20"), avm1::Value(16567)})));
	CHECK(!avm1::toBoolean(mp("addFavourite", {avm1::Value("Again"), avm1::Value("192.168.1.20"), avm1::Value(16567)})));
	CHECK(vm.toString(call(vm, natives, "dice.MessageHandler", "getLastText")) == "serveralreadyfavorite");
	const auto favourites = bindings.favouriteServers();
	CHECK(favourites.size() == 2);
	CHECK(favourites.size() == 2 && favourites[0].name == "Fake server 3" && favourites[1].ip == "192.168.1.20");
	// General.con's lines, as the original writes them.
	CHECK(objects.generalScript().find("GeneralSettings.addFavouriteServer \"192.168.1.20\" 16567 \"Mine\" 0") != std::string::npos);
	// The quick list: Favorites, the two, a gap, Live servers, the 12.
	quick = natives.listRows("serverListQuick");
	CHECK(quick.size() == 17);
	CHECK(quick.size() == 17 && quick[1][0] == "Fake server 3" && quick[2][0] == "Mine" && quick[2][1].empty());
	CHECK(natives.listRows("favouriteServers").size() == 2);
	// Joining one by its address uses it like any other server.
	CHECK(avm1::toBoolean(mp("joinServer", {avm1::Value("192.168.1.20 16567 0")})));
	CHECK(bindings.joinedHost() == "192.168.1.20");
	// Removed by "ip port" and by a row's address.
	CHECK(avm1::toBoolean(mp("removeFavourite", {avm1::Value("192.168.1.20 16567")})));
	CHECK(!avm1::toBoolean(mp("removeFavourite", {avm1::Value("192.168.1.20 16567")})));
	CHECK(avm1::toBoolean(mp("removeFavouriteServer", {avm1::Value("10.0.0.3 17569 29900")})));
	CHECK(bindings.favouriteServers().empty() && natives.listRows("serverListQuick").size() == 15);
	CHECK(!avm1::toBoolean(mp("isServerOnline", {avm1::Value("10.0.0.3 17569 29900")})));

	// The history from the profile's file: made a favourite, removed.
	objects.setList("GeneralSettings.ServerHistory", {"\"10.1.1.1\" 16567 \"Old one\" 29901"});
	CHECK(avm1::toBoolean(mp("addHistoryToFavourites", {avm1::Value("10.1.1.1 16567")})));
	CHECK(bindings.favouriteServers().size() == 1 && bindings.favouriteServers()[0].name == "Old one"
		&& bindings.favouriteServers()[0].queryPort == 29901);
	call(vm, natives, "dice.bf2.GeneralSettings", "removeServer", {avm1::Value("10.1.1.1 16567 29901")});
	CHECK(bindings.favouriteServers().empty() && bindings.favouriteServers(true).size() == 1);
	mp("clearHistory");
	CHECK(bindings.favouriteServers(true).empty());

	// The direct IP (GSConnectToIpHost / Port, 17567 unset).
	CHECK(vm.toNumber(mp("getConnectToIpPort")) == 17567);
	mp("setConnectToIpHost", {avm1::Value("192.168.1.30")});
	mp("setConnectToIpPort", {avm1::Value(16568)});
	CHECK(vm.toString(mp("getConnectToIpHost")) == "192.168.1.30" && vm.toNumber(mp("getConnectToIpPort")) == 16568);
	CHECK(objects.userSettingsScript().find("SettingsManager.stringSet GSConnectToIpHost \"192.168.1.30\"") != std::string::npos);

	// The browser's check boxes: bits of the toggle filters (0x4c382).
	CHECK(avm1::toBoolean(mp("getToggleFilter", {avm1::Value(2)})));
	CHECK(!avm1::toBoolean(mp("getToggleFilter", {avm1::Value(1)})));
	mp("setToggleFilter", {avm1::Value(1), avm1::Value(true), avm1::Value("numplayers>0")});
	CHECK(avm1::toBoolean(mp("getToggleFilter", {avm1::Value(1)})));
	mp("setToggleFilter", {avm1::Value(2), avm1::Value(true), avm1::Value("%%")});
	CHECK(!avm1::toBoolean(mp("getToggleFilter", {avm1::Value(2)})));
	CHECK(objects.get("GeneralSettings.ToggleFilters") == std::to_string((0x4c382 | 1) & ~2));
	mp("setJoinPassword", {avm1::Value("secret")});
	CHECK(vm.toString(mp("getJoinPassword")) == "secret");
}

// The map list's own natives, the current game, Singleplay, Cursor and
// Logic's small ones, the settings resets.
TEST(MenuBindings_OfflineNatives)
{
	Fake fake;
	NativeRegistry natives;
	menu::MenuBindings bindings(natives, fake.services());
	bindings.setMaps({{"Minsk", "gpm_cq", 32}, {"Belgrade", "gpm_sp", 16}});
	avm1::Interpreter vm;
	const auto mp = [&](const char* member, std::vector<avm1::Value> args = {}) {
		return call(vm, natives, "dice.bf2.Multiplay", member, std::move(args));
	};
	mp("addMapToMapList", {avm1::Value("Camp_Gibraltar"), avm1::Value("gpm_ti"), avm1::Value(64)});
	mp("addMapToMapList", {avm1::Value(""), avm1::Value("gpm_ti"), avm1::Value(64)}); // no name: nothing
	mp("setFilterGameMode", {avm1::Value("gpm_sp")});
	mp("addMapIndexToMapList", {avm1::Value(0)});
	CHECK(natives.listRows("mapListChosen").size() == 2);
	CHECK(vm.toString(mp("getMapName", {avm1::Value(0)})) == "Camp Gibraltar");
	CHECK(vm.toString(mp("getMapName", {avm1::Value(5)})).empty());
	CHECK(avm1::toBoolean(mp("moveMapInList", {avm1::Value(1), avm1::Value(true)}))); // Belgrade up
	CHECK(vm.toString(mp("getMapName", {avm1::Value(0)})) == "Belgrade");
	mp("moveMapInList", {avm1::Value(1), avm1::Value(false)}); // the last can't go down
	CHECK(vm.toString(mp("getMapName", {avm1::Value(1)})) == "Camp Gibraltar");
	mp("setMapList", {avm1::Value("x")});
	CHECK(natives.listRows("mapListChosen").size() == 2);

	// The current game: none, then the one started, none after leaving.
	CHECK(vm.toString(mp("getCurrentMapName")) == "$noCurrentGame");
	CHECK(vm.toNumber(mp("getCurrentMapSize")) == 0);
	CHECK(avm1::toBoolean(mp("createServer")));
	CHECK(vm.toString(mp("getCurrentMapName")) == "Belgrade" && vm.toString(mp("getCurrentMapGameMode")) == "gpm_sp");
	CHECK(vm.toNumber(mp("getCurrentMapSize")) == 16 && vm.toNumber(mp("getCurrentMapMaxPlayers")) == 64);
	call(vm, natives, "dice.bf2.Logic", "disconnectGame");
	CHECK(vm.toString(mp("getCurrentMapName")) == "$noCurrentGame");

	// Singleplay's do nothing; the cursor keeps its name and switch.
	for (const char* member : {"chooseMission", "getBotSkills", "setBotSkills"}) {
		CHECK(natives.isBound("dice.bf2.Singleplay", member));
		CHECK(call(vm, natives, "dice.bf2.Singleplay", member).isUndefined());
	}
	CHECK(vm.toString(call(vm, natives, "dice.bf2.Cursor", "getCursor")) == "undefined");
	call(vm, natives, "dice.bf2.Cursor", "setCursor", {avm1::Value("hand")});
	call(vm, natives, "dice.bf2.Cursor", "enableCursor", {avm1::Value(false)});
	CHECK(vm.toString(call(vm, natives, "dice.bf2.Cursor", "getCursor")) == "hand" && !bindings.cursorEnabled());

	// Logic: the activate reason, the frame label, the wait cursor, decimals.
	call(vm, natives, "dice.bf2.Logic", "setActivateReason", {avm1::Value(2)});
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.Logic", "getActivateReason")) == 2);
	call(vm, natives, "dice.bf2.Logic", "setFrameLabel", {avm1::Value("options")});
	CHECK(vm.toString(call(vm, natives, "dice.bf2.Logic", "getFrameLabel")) == "options");
	call(vm, natives, "dice.bf2.Logic", "beginWaitState", {avm1::Value(1)});
	CHECK(bindings.waitCursor() == 1);
	call(vm, natives, "dice.bf2.Logic", "endWaitState");
	CHECK(bindings.waitCursor() == 0);
	CHECK(vm.toString(call(vm, natives, "dice.bf2.Logic", "setDecimals", {avm1::Value(3.14159), avm1::Value(2)})) == "3.14");

	// GeneralSettings' resets: the HUD page's values back to 0x0049bdf0's.
	call(vm, natives, "dice.bf2.GeneralSettings", "setHUDTransparency", {avm1::Value(0.5)});
	call(vm, natives, "dice.bf2.GeneralSettings", "setCrosshairColor",
		{avm1::Value(1), avm1::Value(2), avm1::Value(3), avm1::Value(4)});
	call(vm, natives, "dice.bf2.GeneralSettings", "resetHud");
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.GeneralSettings", "getHUDTransparency")) == 0);
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.GeneralSettings", "getMinimapTransparency")) == 1.0 - 170.0 / 255.0);
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.GeneralSettings", "getCrosshairColor")) == static_cast<double>(0xffffffffu));
	call(vm, natives, "dice.bf2.GeneralSettings", "resetMessage");
	CHECK(bindings.setting("GeneralSettings.ChatMessagesFilter", 0) == 1);
	CHECK(!avm1::toBoolean(call(vm, natives, "dice.bf2.VideoSettings", "needsReboot")));
}

// dice.bf2.Mod over the installed mods (each Mod.desc).
TEST(MenuBindings_Mods)
{
	const auto mod = menu::MenuBindings::parseModDesc("mods/bf2142",
		"<mod>\n\t<title> HEADER_MODTITLE </title>\n\t<desc> TEXT_MODDESC </desc>\n\t<url> http://www.battlefield.com</url>\n"
		"\t<logo> mod.png </logo>\n\t<music> sound/music/2142MenuMusic.ogg </music> \n\t<version> 1.51 </version>\n</mod>");
	CHECK(mod.title == "HEADER_MODTITLE" && mod.url == "http://www.battlefield.com" && mod.logo == "mod.png" && mod.version == "1.51");
	Fake fake;
	NativeRegistry natives;
	natives.setLocalizer([](const std::string& key) { return key == "HEADER_MODTITLE" ? std::string("Battlefield 2142") : key; });
	menu::MenuBindings bindings(natives, fake.services());
	std::string activated;
	bindings.onActivateMod = [&activated](const std::string& path) { activated = path; };
	std::vector<menu::MenuBindings::ModEntry> mods {mod};
	if (const char* dir = std::getenv("BF2142_GAME_DIR")) {
		mods = menu::MenuBindings::findMods(dir);
		CHECK(mods.size() == 1 && mods[0].path == "mods/bf2142" && mods[0].version == "1.51");
	}
	bindings.setMods(mods, "mods/bf2142");
	avm1::Interpreter vm;
	const auto m = [&](const char* member, std::vector<avm1::Value> args = {}) {
		return call(vm, natives, "dice.bf2.Mod", member, std::move(args));
	};
	CHECK(vm.toNumber(m("getNumMods")) == 1);
	CHECK(natives.listRows("mods").size() == 1 && natives.listRows("mods")[0][0] == "Battlefield 2142");
	CHECK(vm.toString(m("getCurrentModName")) == "bf2142");
	CHECK(vm.toString(m("getModTitle")).empty()); // none selected
	m("activateMod");
	CHECK(activated.empty());
	m("selectMod", {avm1::Value(0)});
	CHECK(vm.toString(m("getModTitle")) == "Battlefield 2142");
	CHECK(vm.toString(m("getModPath")) == "mods/bf2142" && vm.toString(m("getModPathShort")) == "bf2142");
	CHECK(vm.toString(m("getModLogo")) == "mod.png" && vm.toString(m("getModVersion")) == "1.51");
	m("activateMod");
	CHECK(activated == "mods/bf2142");
}

// Every native SwiffPlayer registers (NativeTable.cpp's 702) is bound by
// the menu bindings, or is one of these, each kind as docs/uncited.md's
// "Menu natives" table lists it. A binding added for one of them, or a
// native left in neither, fails here, so the table stays true.
namespace {

struct NativeKind {
	const char* kind;
	const char* object;
	std::vector<const char*> members;
};

const NativeKind kUnboundNatives[] = {
	// Bound outside MenuBindings: the player's built-ins (Player::installDefaults,
	// NativeList's methods and fields), main.cpp's sound, music and credits.
	{"elsewhere", "dice.bf2.Locale", {"getString", "getStringUppercase", "getLanguage", "getTextboxWidth", "convertToWide"}},
	{"elsewhere", "dice.bf2.Logic", {"active", "setScrollSpeed", "enableScroller"}},
	{"elsewhere", "dice.bf2.Mouse", {"insideMovieClip"}},
	{"elsewhere", "dice.bf2.List", {"fillList", "abortFill", "sortList", "refreshList", "scrollUp", "scrollDown", "pageUp",
		"pageDown", "setFirstIndex", "scrollPercent", "scrollToSelection", "selectRow", "setArgs", "m_firstIndex",
		"m_rowCount", "m_selectedIndex"}},
	{"elsewhere", "dice.bf2.Sound", {"playSound", "stopSound", "playMenuMusic", "stopMenuMusic"}},
	{"elsewhere", "dice.bf2.Multiplay", {"getScrollFadeSpeed"}},
	// Online only: the master server's revisions and query filters, HTTP,
	// news, the EA account and registration, BFHQ, buddies, leaderboards,
	// sponsors, demo and logo downloads.
	{"online", "dice.General", {"flushList", "getListRevision", "getListEntries", "getHostVersion", "setFilter",
		"setSortOrder", "cacheExtendedServerInfo", "setEquipmentIndex", "getHttpFile", "getHttpFileAsync", "isDownloading",
		"getDownloadedContent"}},
	{"online", "dice.bf2.Logic", {"getEANews", "getNextNewsTicker", "getBFWebsiteUrl", "useEreg2", "launchEreg2"}},
	{"online", "dice.bf2.Multiplay", {"setBrowser", "getBrowser", "setFilter", "sort",
		"updateFilter", "setHighestPing", "getSponsorText", "getSponsorLogoUrl", "isDownloadingDemo", "getDownloadingHostname",
		"getDownloadingUrl", "getDownloadingDate", "getDownloadingProgress", "downloadDemo", "stopDownloadDemo",
		"hasDemoDownloadFailed", "downloadServerLogo", "getSelectedLeaderboardCategoryId"}},
	{"online", "dice.bf2.Profile", {"createOnlineAccount", "loginOnlineAccount", "loginOnlineAccountWithEmail",
		"loginOnlineAccountWithNick", "searchUniquePlayer", "searchPlayer", "setComparePlayer", "clearComparePlayer",
		"setStatPlayer", "getStatPlayer", "getStatPlayerName", "isMyProfile", "setActivePlayer", "newGamespyProfile",
		"getFirstCountry", "prepareCreateMasterAccount", "getTermsOfService", "createMasterAccount", "createSubAccount",
		"disableSubAccount", "retrieveAccountName", "retrievePassword", "getAccountInfo", "getAccountInfoEmail",
		"getAccountBirthDate", "getAccountInfoParentalEmail", "getAccountInfoCountryIndex", "getAccountInfoAllowEAEmail",
		"getAccountInfoAllowThirdPartyEmail", "updateAccountInfo", "updatePassword", "hasRetailerWeapon",
		"hasRetailerRankup"}},
	{"online", "dice.bf2.BuddySystem", {"sendBuddyRequest", "authBuddyRequest", "denyBuddyRequest", "removeBuddy",
		"sendBuddyMessage", "getBuddiesOnlineString", "searchForNick", "getBuddyMessage", "muteBuddy", "getSystemMessage",
		"registerCallback", "hasMessages"}},
	{"online", "dice.bf2.PlayerStats", {"searchLeaderBoard", "getLeaderBoardPosition", "getLeaderBoardSelectRow"}},
	{"online", "dice.bf2.ServerInfo", {"getSponsorText", "getCustomMapURL"}},
	{"online", "dice.bf2.GeneralSettings", {"updateLastAwardsCheckDate"}},
	// Leave the game: a web browser, the EA downloader, another program,
	// a restart.
	{"external", "dice.General", {"executePath"}},
	{"external", "dice.bf2.Logic", {"launchEADownloader", "restartExe"}},
	{"external", "dice.bf2.Multiplay", {"browse"}},
	{"external", "dice.bf2.Mod", {"browseHomepage"}},
	// Development: the authoring tool's assert, the shader switch, the
	// context menu.
	{"dev", "dice.bf2.Logic", {"assert"}},
	{"dev", "dice.bf2.Shader", {"setShader"}},
	{"dev", "dice.bf2.Mouse", {"registerContextMenu"}},
	// Missing: would work offline, not bound yet (docs/uncited.md says why).
	{"missing", "dice.bf2.Logic", {"cacheCredits", "getRandomCreditsTexture"}},
	{"missing", "dice.bf2.Profile", {"getEquipmentName", "getEquipmentDescription", "setSelectedEquipment",
		"setSelectedUnlockEquipment", "getSprint"}},
	{"missing", "dice.bf2.VideoSettings", {"detectVideoScheme", "isRunningWithUnsupportedDrivers"}},
	{"missing", "dice.bf2.AudioSettings", {"isInCriticalState"}},
};

} // namespace

TEST(MenuBindings_NativeCoverage)
{
	Fake fake;
	NativeRegistry natives;
	menu::MenuBindings bindings(natives, fake.services());
	menu::EndOfRoundBindings endOfRound(natives);
	std::map<std::string, std::string> kinds;
	for (const NativeKind& k : kUnboundNatives) {
		for (const char* member : k.members) {
			const std::string name = std::string(k.object) + "." + member;
			CHECK(kinds.emplace(name, k.kind).second); // listed once
		}
	}
	size_t bound = 0;
	std::map<std::string, int> counts;
	for (size_t i = 0; i < kNativeSlotCount; ++i) {
		const NativeSlotSpec& slot = kNativeSlots[i];
		const std::string name = std::string(slot.object) + "." + slot.member;
		const bool isBound = natives.isBound(slot.object, slot.member);
		const auto kind = kinds.find(name);
		if (isBound) {
			++bound;
			if (kind != kinds.end()) {
				std::fprintf(stderr, "bound but listed as %s: %s\n", kind->second.c_str(), name.c_str());
			}
			CHECK(kind == kinds.end());
		} else {
			if (kind == kinds.end()) {
				std::fprintf(stderr, "unbound and not listed: %s\n", name.c_str());
			}
			CHECK(kind != kinds.end());
			if (kind != kinds.end()) {
				++counts[kind->second];
			}
		}
		if (const char* dump = std::getenv("BF2142_NATIVE_TABLE")) {
			static std::FILE* out = std::fopen(dump, "w");
			if (out) {
				std::fprintf(out, "%s\t%s\t%s\n", slot.object, slot.member,
					isBound ? "bound" : kind != kinds.end() ? kind->second.c_str() : "?");
				std::fflush(out);
			}
		}
	}
	CHECK(kNativeSlotCount == 702);
	CHECK(bound + counts["elsewhere"] + counts["online"] + counts["external"] + counts["dev"] + counts["missing"] == kNativeSlotCount);
}

// The command line's join and host (0x00404780's activation types, the
// Logic natives the main page calls), local demos and bookmarks, the map
// texts from the levels' .desc files, the server info popup's texts and
// the small Logic natives.
TEST(MenuBindings_CommandLineDemosAndTexts)
{
	using CommandLine = menu::MenuBindings::CommandLine;
	CommandLine join;
	join.joinAddress = "81.2.3.4";
	CHECK(menu::MenuBindings::commandLineActivation(join, false) == 2);
	join.joinAddress = "192.168.0.7";
	CHECK(menu::MenuBindings::commandLineActivation(join, false) == 3);
	join.joinAddress = "172.20.1.1";
	CHECK(menu::MenuBindings::commandLineActivation(join, false) == 3);
	CHECK(menu::MenuBindings::commandLineActivation(CommandLine {}, false) == 0);
	CommandLine host;
	host.hostServer = true;
	CHECK(menu::MenuBindings::commandLineActivation(host, true) == 4);
	CHECK(menu::MenuBindings::commandLineActivation(host, false) == 5);

	Fake fake;
	NativeRegistry natives;
	menu::MenuBindings bindings(natives, fake.services());
	avm1::Interpreter vm;
	const auto logic = [&](const char* member, std::vector<avm1::Value> args = {}) {
		return call(vm, natives, "dice.bf2.Logic", member, std::move(args));
	};
	const auto mp = [&](const char* member, std::vector<avm1::Value> args = {}) {
		return call(vm, natives, "dice.bf2.Multiplay", member, std::move(args));
	};
	// The main page spells it getCommandline: it reaches getCommandLine.
	CHECK(natives.memberNamed("dice.bf2.Logic", "getCommandline") == "getCommandLine");
	CHECK(vm.toNumber(logic("getCommandLine")) == 0);
	std::string joined;
	bindings.onJoin = [&](const std::string& h, uint16_t p) { joined = h + ":" + std::to_string(p); };
	CommandLine line;
	line.joinAddress = "10.0.0.5";
	line.port = 17571;
	line.password = "pw";
	bindings.setCommandLine(line);
	CHECK(vm.toNumber(logic("getCommandLine")) == 3);
	CHECK(vm.toString(logic("getCommandlineServer")) == "10.0.0.5");
	CHECK(vm.toString(logic("getCommandlineServerPassword")) == "pw");
	CHECK(avm1::toBoolean(logic("joinCommandlineServer")) && joined == "10.0.0.5:17571");
	CHECK(vm.toString(mp("getJoinPassword")) == "pw");
	logic("setCommandLine", {avm1::Value(0)});
	CHECK(bindings.commandLineType() == 0);
	// Hosting: the command line's level, else the map list.
	std::optional<menu::GameSetup> started;
	bindings.onStartGame = [&](const menu::GameSetup& setup) { started = setup; };
	CommandLine hostLine;
	hostLine.hostServer = true;
	hostLine.level = "Minsk";
	hostLine.gameMode = "gpm_ti";
	bindings.setCommandLine(hostLine);
	CHECK(bindings.commandLineType() == 5);
	logic("beginWaitState", {avm1::Value(1)});
	logic("startCommandlineServer");
	CHECK(started && started->maps.size() == 1 && started->maps[0].level == "Minsk" && started->maps[0].gameMode == "gpm_ti");
	CHECK(bindings.waitCursor() == 0);

	// The small ones: the asynchronous flag, the loading flag, Caps Lock,
	// deleteCurrentProfile (a bare return).
	CHECK(!avm1::toBoolean(logic("getAsync")));
	logic("setAsync", {avm1::Value(true)});
	CHECK(avm1::toBoolean(logic("getAsync")));
	logic("setIsLoading", {avm1::Value(true)});
	CHECK(avm1::toBoolean(logic("getIsLoading")));
	CHECK(!avm1::toBoolean(logic("isCapsLockPressed")));
	bindings.capsLock = [] { return true; };
	CHECK(avm1::toBoolean(logic("isCapsLockPressed")));
	CHECK(call(vm, natives, "dice.bf2.Profile", "deleteCurrentProfile").isUndefined());

	// Local demos: the folder's files with a readable header, their rows,
	// playing and deleting one.
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / "bf2142_menu_demos";
	fs::remove_all(dir);
	fs::create_directories(dir);
	{
		net::DemoHeader header;
		header.serverName = "Test server";
		header.level = "Camp_Gibraltar";
		header.gameMode = "gpm_cq";
		header.started = 0;
		net::DemoWriter writer;
		CHECK(writer.open((dir / "a.bf2142demo").string(), header));
		writer.close(0);
		std::ofstream((dir / "b.bf2142demo").string()) << "not a demo";
		std::ofstream((dir / "c.txt").string()) << "x";
	}
	bindings.gameModeName = [](const std::string& mode) { return mode == "gpm_cq" ? std::string("Conquest") : std::string("N/A"); };
	bindings.setDemoDirectory(dir.string());
	CHECK(bindings.demos().size() == 1);
	const auto demoRows = natives.listRows("demos");
	CHECK(demoRows.size() == 1 && demoRows[0].size() == 4);
	CHECK(demoRows.size() == 1 && demoRows[0][1] == "a" && demoRows[0][2] == "Camp Gibraltar" && demoRows[0][3] == "Conquest");
	std::string played;
	bindings.onPlayDemo = [&](const std::string& path) { played = path; };
	mp("playDemo", {avm1::Value(0)});
	CHECK(played == (dir / "a.bf2142demo").string());
	mp("deleteDemo", {avm1::Value(0)});
	CHECK(bindings.demos().empty() && natives.listRows("demos").empty() && !fs::exists(dir / "a.bf2142demo"));
	fs::remove_all(dir);

	// Bookmarks: LocalProfile.addDemoBookmark's entries.
	menu::SettingsObjects objects;
	bindings.setSettingsObjects(&objects);
	objects.setList("LocalProfile.DemoBookmark", {"\"Server one\" \"Minsk\" \"http://x/1\" 0", "\"Two\" \"Belgrade\" \"u\" 0"});
	CHECK(vm.toNumber(mp("getNumDemoBookmarks")) == 2);
	CHECK(avm1::toBoolean(mp("deleteDemoBookmark", {avm1::Value(0)})));
	CHECK(!avm1::toBoolean(mp("deleteDemoBookmark", {avm1::Value(5)})));
	CHECK(objects.list("LocalProfile.DemoBookmark").size() == 1);
	const auto bookmarkRows = natives.listRows("demobm");
	CHECK(bookmarkRows.size() == 1 && bookmarkRows[0][1] == "Two" && bookmarkRows[0][2] == "Belgrade");
	CHECK(!avm1::toBoolean(mp("getHasNewBookmark")));
	mp("setHasNewBookmark", {avm1::Value(true)});
	CHECK(avm1::toBoolean(mp("getHasNewBookmark")));

	// The map texts: a level's .desc (its name, briefing locid, modes).
	bindings.setMaps({{"Minsk", "gpm_ti", 32}, {"Minsk", "gpm_cq", 64}});
	bindings.readFile = [](const std::string& path, std::string& text) {
		if (path != "Levels/Minsk/Info/Minsk.desc") {
			return false;
		}
		text = "<map><name> Minsk </name><briefing locid=\"LEVEL_minsk\">Text</briefing><modes>"
			   "<mode type=\"gpm_ti\"><maptype players=\"32\" locid=\"GM_ti\">x</maptype></mode>"
			   "<mode type=\"gpm_cq\"><maptype players=\"64\" locid=\"GM_cq\">y</maptype></mode></modes></map>";
		return true;
	};
	CHECK(vm.toString(mp("getMapDescriptionFromId", {avm1::Value("Minsk 32")})) == "LEVEL_minsk");
	CHECK(vm.toString(mp("getMapDescriptionFromText", {avm1::Value("Minsk")})) == "LEVEL_minsk");
	CHECK(vm.toString(mp("getMapDescriptionFromId", {avm1::Value("Nowhere")})).empty());
	CHECK(vm.toString(mp("getMapPathFromName", {avm1::Value("Minsk")})) == "Minsk");
	CHECK(vm.toString(mp("getMapPathFromName", {avm1::Value("Elsewhere")})) == "Elsewhere");
	CHECK(vm.toString(mp("getSelectedFilterGameModeId")) == "gpm_ti");
	CHECK(vm.toString(mp("getGameModeFromId", {avm1::Value("gpm_cq")})).empty());
	CHECK(vm.toString(mp("getTeamName", {avm1::Value(0)})).empty());

	// The server info popup: the server its lists name.
	natives.call(vm, "dice.bf2.Multiplay", "startBrowse", {});
	bindings.finishJobs();
	mp("isBrowsing");
	CHECK(vm.toString(call(vm, natives, "dice.bf2.ServerInfo", "getMapName")).empty());
	auto info = std::make_shared<NativeList>(natives, nullptr);
	info->listId = "serverExtendedInfo";
	natives.callList(vm, *info, "setArgs", {avm1::Value("10.0.0.2 17568 29900")});
	CHECK(vm.toString(call(vm, natives, "dice.bf2.ServerInfo", "getMapName")) == "camp_gibraltar");
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.ServerInfo", "getMaxPlayers")) == 64);
	CHECK(vm.toString(call(vm, natives, "dice.bf2.ServerInfo", "getBackgroundTitle")) == "gpm_cq");
	mp("setCurrentServerId", {avm1::Value("10.0.0.2 17568 29900")});
	mp("clearServerId");
}

// The loading screen's Client.* and EndOfRound's level texts.
TEST(EndOfRoundBindings_ClientAndTexts)
{
	NativeRegistry natives;
	menu::EndOfRoundBindings eor(natives);
	avm1::Interpreter vm;
	const auto client = [&](const char* member, std::vector<avm1::Value> args = {}) {
		return call(vm, natives, "dice.bf2.Client", member, std::move(args));
	};
	menu::RoundSummary s;
	s.validRound = true;
	s.previousMapDescriptionKey = "LEVEL_x";
	s.previousGameModeDescriptionKey = "GM_x";
	s.eorType = 2;
	s.timeUntilRestart = 12.5f;
	s.currentLoadFile = "Levels/Minsk/client.zip";
	eor.setSummary(s);
	CHECK(vm.toNumber(client("getEORType")) == 0);
	CHECK(vm.toNumber(client("getTimeUntilRestart")) == 12.5);
	CHECK(vm.toNumber(client("getTotalTimeUntilRestart")) == 0);
	CHECK(vm.toString(client("getCurrentLoadFile")) == "Levels/Minsk/client.zip");
	CHECK(avm1::toBoolean(client("hasUserEscapedFromGame")));
	CHECK(vm.toString(client("getPlayerName", {avm1::Value("Default")})) == "Default");
	client("setPlayerName", {avm1::Value("Ghost")});
	CHECK(vm.toString(client("getPlayerName", {avm1::Value("Default")})) == "Ghost");
	client("setCommandlineOption", {avm1::Value("+joinServer 1.2.3.4")});
	CHECK(vm.toString(client("getCommandlineOption")) == "+joinServer 1.2.3.4");
	const auto e = [&](const char* member, std::vector<avm1::Value> args = {}) {
		return vm.toString(call(vm, natives, "dice.bf2.EndOfRound", member, std::move(args)));
	};
	CHECK(e("getPreviousMapDescription") == "LEVEL_x");
	CHECK(e("getPreviousMapGameModeDescription") == "GM_x");
	CHECK(e("getKitNameFromId", {avm1::Value(4)}) == "specops");
	CHECK(e("getKitNameFromId", {avm1::Value(7)}).empty());
	const menu::LevelDescription desc = menu::parseLevelDescription(
		"<map><name>M</name><briefing locid=\"\">Brief</briefing><modes><mode type=\"gpm_cq\">"
		"<maptype players=\"16\">Sixteen</maptype></mode><mode type=\"gpm_ti\"></mode></modes></map>",
		"gpm_cq", 16);
	CHECK(desc.briefing == "Brief" && desc.gameModeText == "Sixteen");
	CHECK(desc.modes.size() == 2 && desc.modes[1] == "gpm_ti");
}
