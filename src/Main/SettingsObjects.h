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
	const std::string& profile() const { return m_profile; }
	// The profile's objects (General, Audio, Video, LocalProfile) back to
	// their defaults, as before a profile's files run; Global's stay.
	void resetProfile();
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

// The local profiles (BF2142.exe's LocalProfileManager, vtable 0x00925968,
// singleton 0x00a0f7b4): one folder per profile under Profiles/, numbered
// "0001", "0002"... (and "Default", the settings before anyone logs in).
// Each holds Profile.con (LocalProfile.setName, setGamespyNick,
// setEAOnlineMasterAccount, setEAOnlineSubAccount...) and its own
// General.con, Audio.con, Video.con and Controls.con. The menu's offline
// accounts are profiles without an account (createOfflineAccount); an EA
// soldier gets the profile whose sub account is its name, made on its
// first login (master: the account, sub: the soldier). One profile is
// current while someone is logged in; its files are the settings
// objects' and the control maps'.
class LocalProfileManager {
public:
	struct Entry {
		std::string directory; // "0001"
		std::string name, gamespyNick, email, master, sub;
	};
	explicit LocalProfileManager(SettingsObjects& settings) : m_settings(settings) {}

	// Profiles/ ("" none).
	void setDirectory(const std::string& profilesDirectory) { m_directory = profilesDirectory; }
	const std::string& directory() const { return m_directory; }

	// Slot 15 (0x00491360) with validate: the folders holding a profile
	// (0x0048ccb0: Profile.con, General.con, Audio.con and Video.con; the
	// original also asks for ServerSettings.con, which the rewrite, keeping
	// the server pages' values in the menu, does not write), each read
	// from its Profile.con, in name order; Default left out.
	std::vector<Entry> list() const;
	// Slot 6 (0x0048fc30) at start-up: Default made when missing (slot 5,
	// 0x0048f2b0: the defaults, saved).
	void ensureDefault();
	// Slot 9 (0x004904d0): a new profile in the first free number
	// ("%.4d" from 1, gaps filled), made current and saved. `offline`: no
	// account (master and sub empty) and Global.con's default user;
	// otherwise master the last online user and sub `name`. `reset`: the
	// settings back to their defaults first. Refused for an offline one
	// while a profile is current, and for an empty name. Its index in
	// list(), or -1.
	int create(const std::string& name, bool offline, bool reset = true);
	// Slot 7 (0x004903d0): list()[index] made current and loaded (slot 12,
	// 0x0048f490: its folder the default user, Global.con saved, the
	// objects reset and its files run, numTimesLoggedIn counted). Refused
	// while one is current.
	bool select(int index);
	// Slot 10 (0x00490ca0): list()[index]'s folder deleted (no longer the
	// default user). Refused while one is current.
	bool remove(int index);
	// Slot 8 (0x0048f3a0): nobody's profile any more.
	void release();
	// Slot 13 (0x0048d940): the current profile's files (Default: only
	// Profile.con, Audio.con and Video.con).
	bool save();

	int current() const { return m_current; } // list() index, -1 none
	const std::string& currentDirectory() const { return m_currentDirectory; }
	// Slot 20 (0x00491030): the profile of a soldier (its sub account,
	// compared exactly), -1 none.
	int findBySub(const std::string& sub) const;
	// The first number free among `taken` (case-insensitive).
	static std::string nextDirectory(const std::vector<std::string>& taken);

	// Runs a profile's files through the console after the objects were
	// reset (Profile.con, General.con, Audio.con, Video.con, and the
	// controls from its Controls.con); and writes its controls.
	std::function<void(const std::string& folder)> onLoad;
	std::function<void(const std::string& folder)> onSaveControls;
	// Back to the game's controls (a new profile's reset).
	std::function<void()> onResetControls;

private:
	bool load(const std::string& folder);
	SettingsObjects& m_settings;
	std::string m_directory;
	int m_current = -1;
	std::string m_currentDirectory;
};

} // namespace dice::hfe::menu
