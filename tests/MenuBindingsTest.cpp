// The main menu's natives (Main/MenuBindings.h) with a fake account service
// and master server: the error mapping, the natives' states, and (with
// $BF2142_GAME_DIR) the shipped mainMenu.dfm driven from the login page to
// joining a server.
#include "Console/Console.h"
#include "IO/File/ZipArchive.h"
#include "IO/Localization/Localization.h"
#include "Input/ControlMap.h"
#include "Main/MenuBindings.h"
#include "TestMain.h"
#include "UI/Player/Player.h"

#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
	CHECK(vm.toNumber(call(vm, natives, "dice.bf2.ServerSettings", "getMaxPlayers")) == 16);
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
	CHECK(avm1::toBoolean(call(vm, natives, "dice.bf2.ControlSettings", "mapInput",
		{avm1::Value(fire), avm1::Value(true), avm1::Value(false), avm1::Value(false)})));
	CHECK(shown(fire, true, false) == "Q");
	CHECK(controls.pressed(input::ControlMaps::kInfantry, "c_PIFire",
		[](const std::string& control) { return control == "IDKey_Q"; }));
	// A gamepad's button as fire's secondary, its stick pushed up (negative)
	// as throttle's forward: an inverted axis mapping.
	bindings.setLastInput("IDFGameController_0", "IDButton_5", 1);
	CHECK(avm1::toBoolean(call(vm, natives, "dice.bf2.ControlSettings", "mapInput",
		{avm1::Value(fire), avm1::Value(true), avm1::Value(false), avm1::Value(false)})));
	CHECK(shown(fire, true, false) == "Joy Button 5");
	bindings.setLastInput("IDFGameController_0", "IDAxis_1", -1);
	CHECK(avm1::toBoolean(call(vm, natives, "dice.bf2.ControlSettings", "mapInput",
		{avm1::Value(throttle), avm1::Value(true), avm1::Value(false), avm1::Value(false)})));
	const input::Binding* axis = controls.binding(input::ControlMaps::kInfantry, "c_PIThrottle", 1, 1);
	CHECK(axis && axis->kind == input::Binding::Kind::Axis && axis->sign == -1 && axis->device == "IDFGameController_0");
	CHECK(shown(throttle, true, false) == "Joy Axis 1");
	CHECK(shown(throttle, false, false) == "W"); // the primary kept
	// With the game's strings: the localized CONTROLLER_ key.
	natives.setLocalizer([](const std::string& key) {
		return key == "CONTROLLER_IDFGameController_IDButton_5" ? std::string("Joy Button 5 (loc)") : key;
	});
	CHECK(shown(fire, true, false) == "Joy Button 5 (loc)");
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
