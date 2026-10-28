// The game's settings and profile console objects, as BF2142.exe keeps
// them in a player's profile folder and reads them back with the console:
//
//   Profiles/Global.con         GlobalSettings.*  (0x00495d50 writes it)
//   Profiles/0001/Profile.con   LocalProfile.*    (0x0048d940)
//   Profiles/0001/General.con   GeneralSettings.* (0x0049aad0)
//   Profiles/0001/Audio.con     AudioSettings.*   (0x004961a0)
//   Profiles/0001/Video.con     VideoSettings.*   (0x00497560)
//   Settings/Usersettings.con   SettingsManager.* (the GS* game settings)
//
// Each file is console lines ("GeneralSettings.setMaxBots 16",
// "AudioSettings.setEffectsVolume 0.8", strings in quotes) in the
// writer's order; the game runs them at start-up and writes them again
// when a settings page is applied. Here the values are kept by
// "Object.Name" ("GeneralSettings.MaxBots", "AudioSettings.EffectsVolume")
// as the console text of their arguments; the menu's settings pages
// (MenuBindings) read and write the same names, and the game applies the
// ones it has systems for (MenuBindings::onSettingsApplied in main.cpp:
// volumes, bots, the crosshair colour, help pop-ups...). VideoSettings'
// qualities are stored and exposed only; the renderer reads them.
//
// The rewrite's profile folder is <config dir>/bf2142-recomp/Profiles/
// (the original's My Documents/Battlefield 2142/Profiles/), one profile
// "0001".
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
}

namespace dice::hfe::menu {

class SettingsObjects {
public:
	SettingsObjects();

	// GeneralSettings (and the three the writer adds: setBotSkill,
	// setLastAwardsCheckDate, setLCDDisplayModes), AudioSettings,
	// VideoSettings, GlobalSettings, LocalProfile, SettingsManager. A
	// method already registered by a system that applies it (HudAlerts'
	// GeneralSettings.setHelpPopups, PostProduction's
	// VideoSettings.setUseBloom...) keeps working: this one stores the
	// value and then calls it.
	void registerConsole(Console& console);

	// A value by "Object.Name" ("" when unknown), as console text
	// ("16", "0.8", "1", "Software", "255 255 0 255").
	std::string get(const std::string& key) const;
	double number(const std::string& key, double fallback) const;
	bool has(const std::string& key) const;
	void set(const std::string& key, const std::string& value);
	// Every scalar value (not the lists: favourites, history, played help).
	const std::map<std::string, std::string>& values() const { return m_values; }

	// The files' text, as the original writes them.
	std::string generalScript() const;
	std::string audioScript() const;
	std::string videoScript() const;
	std::string globalScript() const;
	std::string profileScript() const;
	std::string userSettingsScript() const;

	// Where the files are kept ("" none) and the profile's folder name.
	void setDirectory(const std::string& profilesDirectory, const std::string& profile = "0001");
	const std::string& directory() const { return m_directory; }
	// <config dir>/bf2142-recomp/Profiles.
	static std::string defaultDirectory();
	// Runs the saved files through the console (missing ones are skipped);
	// true when any was there.
	bool load(Console& console);
	// Writes them all (LocalProfile.save writes Profile.con alone).
	bool save() const;
	bool saveProfile() const;
	bool saveUserSettings() const;

	// Told after a console line changed a value (its key).
	std::function<void(const std::string& key)> onChange;

	// SettingsManager's typed settings (GSPlayerName, GSCameraShake...).
	struct Typed {
		std::string type; // "string", "bool", "int", "float", "U32"
		std::string value;
	};
	const std::map<std::string, Typed>& gameSettings() const { return m_game; }

private:
	std::string file(const std::string& name, bool global = false) const;
	bool write(const std::string& path, const std::string& text) const;
	std::map<std::string, std::string> m_values;
	// The list entries: "GeneralSettings.FavouriteServer" -> each call's
	// arguments as written.
	std::map<std::string, std::vector<std::string>> m_lists;
	std::map<std::string, Typed> m_game;
	std::string m_directory, m_profile = "0001";
};

} // namespace dice::hfe::menu
