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
	CHECK(audio.find("AudioSettings.setProvider \"Software\"\n") != std::string::npos);
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
	bindings.finishJobs();
}
