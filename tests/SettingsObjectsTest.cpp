// The settings and profile console objects (Main/SettingsObjects.h): the
// console calls, the files in the writers' order and format, loading them
// back, a system's own method still called, SettingsManager, and the menu's
// pages starting from and writing back to them.
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Main/MenuBindings.h"
#include "Main/SettingsObjects.h"
#include "TestMain.h"
#include "UI/Player/Player.h"

#include <cmath>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace dice::hfe;

namespace {

std::string run(Console& console, const std::string& line)
{
	std::string out;
	console.execute(line, out, ConsoleAccess::Anyone);
	return out;
}

std::string readFile(const std::filesystem::path& path)
{
	std::ifstream in(path);
	std::stringstream text;
	text << in.rdbuf();
	return text.str();
}

} // namespace

TEST(SettingsObjects_Console)
{
	Console console;
	bool helpPopups = true;
	// A system's own method registered first keeps being called.
	bindMethod(console, "GeneralSettings.setHelpPopups", ConsoleAccess::Anyone, [&helpPopups](bool on) { helpPopups = on; });
	menu::SettingsObjects settings;
	settings.registerConsole(console);
	std::string changed;
	settings.onChange = [&changed](const std::string& key) { changed = key; };
	run(console, "GeneralSettings.setMaxBots 24");
	CHECK(settings.get("GeneralSettings.MaxBots") == "24");
	CHECK(changed == "GeneralSettings.MaxBots");
	run(console, "GeneralSettings.setHelpPopups 0");
	CHECK(!helpPopups);
	CHECK(settings.get("GeneralSettings.HelpPopups") == "0");
	run(console, "GeneralSettings.setCrosshairColor 255 128 0 200");
	CHECK(settings.get("GeneralSettings.CrosshairColor") == "255 128 0 200");
	run(console, "GeneralSettings.addFavouriteServer \"1.2.3.4\" 17567 \"My server\" 0");
	// AudioSettings are properties: set by name or setX, read with no argument.
	run(console, "AudioSettings.effectsVolume 0.25");
	run(console, "AudioSettings.setMusicVolume 0.5");
	CHECK(run(console, "AudioSettings.effectsVolume") == "0.25");
	CHECK(settings.number("AudioSettings.MusicVolume", 1) == 0.5);
	run(console, "VideoSettings.setResolution 1024x768@60Hz");
	run(console, "GlobalSettings.setDefaultUser \"Tester\"");
	run(console, "LocalProfile.setName \"Tester\"");
	run(console, "LocalProfile.setRank 7");
	// SettingsManager's typed GS settings.
	run(console, "SettingsManager.boolSet GSCameraShake 1");
	run(console, "SettingsManager.stringSet GSPlayerName \"Someone\"");
	CHECK(run(console, "SettingsManager.boolGet GSCameraShake") == "1");
	CHECK(run(console, "SettingsManager.stringGet GSPlayerName") == "Someone");
	CHECK(run(console, "SettingsManager.intGet GSUnknown") == "0");

	// The files, as the writers order and spell them.
	const std::string general = settings.generalScript();
	CHECK(general.find("GeneralSettings.setSortOrder 0\n") == 0);
	CHECK(general.find("GeneralSettings.addFavouriteServer \"1.2.3.4\" 17567 \"My server\" 0\n") != std::string::npos);
	CHECK(general.find("GeneralSettings.setCrosshairColor 255 128 0 200\n") != std::string::npos);
	CHECK(general.find("GeneralSettings.setMaxBots 24\n") != std::string::npos);
	CHECK(general.find("GeneralSettings.setViewIntroMovie 1\n") != std::string::npos);
	const std::string audio = settings.audioScript();
	CHECK(audio.find("AudioSettings.setVoipEnabled 0\n") == 0);
	CHECK(audio.find("AudioSettings.setProvider \"software\"\n") != std::string::npos);
	CHECK(audio.find("AudioSettings.setEffectsVolume 0.25\n") != std::string::npos);
	CHECK(settings.videoScript().find("VideoSettings.setResolution 1024x768@60Hz\n") != std::string::npos);
	CHECK(settings.globalScript().find("GlobalSettings.setDefaultUser \"Tester\"\n") == 0);
	CHECK(settings.profileScript().find("LocalProfile.setRank 7\n") != std::string::npos);
	CHECK(settings.userSettingsScript().find("SettingsManager.stringSet GSPlayerName \"Someone\"\n") != std::string::npos);

	// Saved and read back by a new game.
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / "bf2142_settings_test";
	std::filesystem::remove_all(dir);
	settings.setDirectory(dir.string());
	CHECK(settings.save());
	CHECK(std::filesystem::exists(dir / "0001" / "General.con"));
	CHECK(std::filesystem::exists(dir / "Global.con"));
	CHECK(readFile(dir / "0001" / "Audio.con") == audio);
	Console again;
	menu::SettingsObjects loaded;
	loaded.registerConsole(again);
	loaded.setDirectory(dir.string());
	CHECK(loaded.load(again));
	CHECK(loaded.get("GeneralSettings.MaxBots") == "24");
	CHECK(loaded.get("AudioSettings.EffectsVolume") == "0.25");
	CHECK(loaded.get("LocalProfile.Name") == "Tester");
	CHECK(loaded.generalScript() == general);
	CHECK(loaded.gameSettings().count("GSCameraShake") == 1);
	std::filesystem::remove_all(dir);
}

TEST(SettingsObjects_MenuPages)
{
	Console console;
	menu::SettingsObjects settings;
	settings.registerConsole(console);
	run(console, "GeneralSettings.setMaxBots 8");
	run(console, "AudioSettings.helpVoiceVolume 0.3");
	run(console, "VideoSettings.setUseBloom 0");
	ui::NativeRegistry natives;
	menu::MenuServices services;
	services.checkOnline = [] { return false; };
	services.fetchServers = [](std::string* error) {
		*error = "offline";
		return std::optional<net::ServerList>();
	};
	menu::MenuBindings bindings(natives, services);
	bindings.setSettingsObjects(&settings);
	// The pages start from the objects' values.
	CHECK(bindings.setting("GeneralSettings.MaxBots", 0) == 8);
	CHECK(bindings.gameSetup().bots == 8);
	CHECK(std::fabs(bindings.setting("AudioSettings.VoiceOverVolume", 1) - 0.3) < 1e-6);
	CHECK(bindings.setting("VideoSettings.BloomEnabled", 1) == 0);
	// A page applied: its values back to the objects.
	ui::avm1::Interpreter vm;
	const auto call = [&](const char* object, const char* member, std::vector<ui::avm1::Value> args) {
		return natives.call(vm, object, member, args);
	};
	call("dice.bf2.AudioSettings", "setVoiceOverVolume", {ui::avm1::Value(0.75)});
	call("dice.bf2.AudioSettings", "apply", {});
	CHECK(settings.get("AudioSettings.HelpVoiceVolume") == "0.75");
	call("dice.bf2.GeneralSettings", "setMaxBots", {ui::avm1::Value(12)});
	call("dice.bf2.GeneralSettings", "apply", {});
	CHECK(settings.get("GeneralSettings.MaxBots") == "12");
	// The HUD page: transparencies against the object's alphas, the
	// crosshair as one ARGB number, camera shake inverted (SwiffPlayer
	// 0x100326b0, 0x10032310, 0x10030180 and their setters).
	run(console, "GeneralSettings.setMinimapTransparency 170");
	run(console, "GeneralSettings.setCrosshairColor 255 128 0 200");
	run(console, "GeneralSettings.setCameraShake 1");
	run(console, "GeneralSettings.setHelpPopups 1");
	run(console, "GeneralSettings.setMapIconAlphaTransparency 51");
	menu::MenuBindings page(natives, services);
	page.setSettingsObjects(&settings);
	const auto number = [&](const char* member) { return vm.toNumber(call("dice.bf2.GeneralSettings", member, {})); };
	CHECK(std::fabs(number("getMinimapTransparency") - (1 - 170.0 / 255)) < 1e-6);
	CHECK(std::fabs(number("getMapIconTransparency") - 0.8) < 1e-6);
	CHECK(number("getHUDTransparency") == 0);
	CHECK(number("getCrosshairColor") == static_cast<double>(200u << 24 | 255u << 16 | 128u << 8 | 0u));
	CHECK(number("getCameraShake") == 0 && number("getHelpPopUps") == 1);
	call("dice.bf2.GeneralSettings", "setHUDTransparency", {ui::avm1::Value(0.2)});
	call("dice.bf2.GeneralSettings", "setCrosshairColor",
		{ui::avm1::Value(10), ui::avm1::Value(20), ui::avm1::Value(30), ui::avm1::Value(40)});
	call("dice.bf2.GeneralSettings", "setCameraShake", {ui::avm1::Value(true)});
	call("dice.bf2.GeneralSettings", "setHelpPopUps", {ui::avm1::Value(false)});
	call("dice.bf2.GeneralSettings", "apply", {});
	CHECK(settings.get("GeneralSettings.HUDTransparency") == "204");
	CHECK(settings.get("GeneralSettings.CrosshairColor") == "10 20 30 40");
	CHECK(settings.get("GeneralSettings.CameraShake") == "0" && settings.get("GeneralSettings.HelpPopups") == "0");
	CHECK(settings.get("GeneralSettings.MinimapTransparency") == "170");
	bindings.finishJobs();
	page.finishJobs();
}

// Several local profiles (LocalProfileManager): Default made at start-up,
// numbered folders with gaps filled, each with its own settings and
// controls, selected, released and deleted as the original does; and the
// menu's offline accounts and soldiers on them.
TEST(SettingsObjects_LocalProfiles)
{
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / "bf2142-profiles-test";
	std::filesystem::remove_all(dir);
	Console console;
	menu::SettingsObjects settings;
	settings.registerConsole(console);
	menu::LocalProfileManager profiles(settings);
	profiles.setDirectory(dir.string());
	std::string controls = "game";
	profiles.onLoad = [&](const std::string& folder) {
		settings.load(console);
		std::ifstream in(dir / folder / "Controls.con");
		std::getline(in, controls);
	};
	profiles.onSaveControls = [&](const std::string& folder) { std::ofstream(dir / folder / "Controls.con") << controls; };
	profiles.onResetControls = [&] { controls = "game"; };
	profiles.ensureDefault();
	CHECK(std::filesystem::exists(dir / "Default" / "Profile.con"));
	CHECK(!std::filesystem::exists(dir / "Default" / "General.con")); // Default: Profile, Audio, Video
	CHECK(profiles.list().empty());                                  // Default is not listed
	CHECK(menu::LocalProfileManager::nextDirectory({"0001", "0003", "Default"}) == "0002");

	// Two offline profiles, each with its own bots and keys.
	CHECK(profiles.create("Alpha", true) == 0);
	CHECK(profiles.currentDirectory() == "0001" && settings.get("GlobalSettings.DefaultUser") == "0001");
	CHECK(profiles.create("Bravo", true) < 0); // one is current
	run(console, "GeneralSettings.setMaxBots 4");
	controls = "alpha keys";
	CHECK(profiles.save());
	profiles.release();
	CHECK(profiles.create("Bravo", true) == 1);
	CHECK(settings.number("GeneralSettings.MaxBots", 0) == 16); // reset to the defaults
	CHECK(controls == "game");
	run(console, "GeneralSettings.setMaxBots 12");
	controls = "bravo keys";
	profiles.save();
	profiles.release();
	const auto entries = profiles.list();
	CHECK(entries.size() == 2 && entries[0].name == "Alpha" && entries[1].directory == "0002" && entries[1].master.empty());
	// Selecting one loads its own.
	CHECK(profiles.select(0));
	CHECK(settings.number("GeneralSettings.MaxBots", 0) == 4 && controls == "alpha keys");
	CHECK(settings.number("LocalProfile.NumTimesLoggedIn", 0) == 1);
	CHECK(!profiles.remove(1)); // not while one is current
	profiles.release();
	CHECK(profiles.select(1));
	CHECK(settings.number("GeneralSettings.MaxBots", 0) == 12 && controls == "bravo keys");
	profiles.release();
	// Deleting the first: its folder goes, and the next one takes its number.
	CHECK(profiles.remove(0));
	CHECK(!std::filesystem::exists(dir / "0001") && profiles.list().size() == 1);
	CHECK(profiles.create("Charlie", true) == 0 && profiles.currentDirectory() == "0001");
	profiles.release();

	// The menu: offline accounts are the profiles without an account.
	ui::NativeRegistry natives;
	menu::MenuServices services;
	services.checkOnline = [] { return false; };
	menu::MenuBindings bindings(natives, services);
	bindings.setSettingsObjects(&settings);
	bindings.setLocalProfiles(&profiles);
	ui::avm1::Interpreter vm;
	const auto call = [&](const char* object, const char* member, std::vector<ui::avm1::Value> args) {
		return natives.call(vm, object, member, args);
	};
	CHECK(natives.listRows("localProfiles").size() == 2);
	CHECK(vm.toNumber(call("dice.bf2.Profile", "getNumLocalProfiles", {})) == 2);
	CHECK(ui::avm1::toBoolean(call("dice.bf2.Profile", "createOfflineAccount", {ui::avm1::Value("Delta"), ui::avm1::Value("Delta")})));
	CHECK(bindings.loggedIn() && bindings.soldierName() == "Delta" && profiles.currentDirectory() == "0003");
	CHECK(natives.listRows("localProfiles").size() == 3);
	call("dice.bf2.Profile", "logout", {});
	CHECK(profiles.current() < 0);
	CHECK(ui::avm1::toBoolean(call("dice.bf2.Profile", "loginOfflineAccount", {ui::avm1::Value(1)})));
	CHECK(bindings.soldierName() == "Bravo" && bindings.setting("GeneralSettings.MaxBots", 0) == 12);
	call("dice.bf2.Profile", "logout", {});
	// A soldier of an account gets its own profile (master the account).
	CHECK(ui::avm1::toBoolean(call("dice.bf2.Logic", "setOfflineMode", {ui::avm1::Value("acct")})));
	call("dice.bf2.Profile", "connectToSubAccount", {ui::avm1::Value("acct")});
	CHECK(profiles.current() >= 0);
	const auto soldier = profiles.list()[static_cast<size_t>(profiles.current())];
	CHECK(soldier.master == "acct" && soldier.sub == "acct" && soldier.directory == "0004");
	CHECK(natives.listRows("localProfiles").size() == 3); // not an offline account
	call("dice.bf2.Profile", "disconnectFromSubAccount", {});
	// Again: the same profile.
	call("dice.bf2.Profile", "connectToSubAccount", {ui::avm1::Value("acct")});
	CHECK(profiles.currentDirectory() == "0004" && profiles.list().size() == 4);
	std::filesystem::remove_all(dir);
}
