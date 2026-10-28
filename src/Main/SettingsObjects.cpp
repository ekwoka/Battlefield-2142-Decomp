#include "SettingsObjects.h"

#include "Console/Console.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dice::hfe::menu {

namespace {

// One console call of a settings object: its name, its arguments' types
// (i int, f float, b bool, s text written bare, q text written in
// quotes), its default (the constructors' values: GeneralSettings
// 0x0049bd20 / 0x0049bdf0; the others as the menu's pages start), and
// whether each call adds an entry (lists) instead of setting a value.
struct Field {
	const char* method; // "GeneralSettings.setMaxBots"
	const char* types;
	const char* fallback;
	bool list = false;
	bool property = false; // a console property (AudioSettings.voipEnabled)
};

// In each writer's order.
const Field kGeneral[] = {
	{"GeneralSettings.setPlayedVOHelp", "q", "", true},
	{"GeneralSettings.setSortOrder", "i", "0"},
	{"GeneralSettings.setSortKey", "q", ""},
	{"GeneralSettings.setNumRoundsPlayed", "i", "0"},
	{"GeneralSettings.setServerFilter", "q", ""},
	{"GeneralSettings.addFavouriteServer", "qiqi", "", true},
	{"GeneralSettings.addServerHistory", "qiqi", "", true},
	{"GeneralSettings.setHUDTransparency", "f", "255"},
	{"GeneralSettings.setCrosshairColor", "iiii", "255 255 255 255"},
	{"GeneralSettings.setBuddytagColor", "iii", "0 0 0"},
	{"GeneralSettings.setSquadtagColor", "iii", "0 255 0"},
	{"GeneralSettings.setMinimapRotate", "b", "1"},
	{"GeneralSettings.setMinimapTransparency", "f", "170"},
	{"GeneralSettings.setViewIntroMovie", "b", "1"},
	{"GeneralSettings.setOutOfVoting", "b", "0"},
	{"GeneralSettings.setBFTVSaveDirectory", "q", ""},
	{"GeneralSettings.setConfirmQuit", "b", "0"},
	{"GeneralSettings.setMapIconAlphaTransparency", "f", "255"},
	{"GeneralSettings.setMaxBots", "i", "16"},
	{"GeneralSettings.setMaxBotsIncludeHumans", "b", "1"},
	{"GeneralSettings.setBotSkill", "f", "0.5"},
	{"GeneralSettings.setAutoScreenshot", "i", "0"},
	{"GeneralSettings.setIgnoreBuddyRequests", "i", "0"},
	{"GeneralSettings.setCameraShake", "i", "1"},
	{"GeneralSettings.setColorBlindFriendly", "i", "0"},
	{"GeneralSettings.setHelpPopups", "i", "1"},
	{"GeneralSettings.setKillMessagesFilter", "i", "0"},
	{"GeneralSettings.setRadioMessagesFilter", "i", "0"},
	{"GeneralSettings.setChatMessagesFilter", "i", "0"},
	{"GeneralSettings.setLastAwardsCheckDate", "i", "0"},
	{"GeneralSettings.setAllowPunkBuster", "b", "1"},
	{"GeneralSettings.setItemSelectionReverseItems", "b", "0"},
	{"GeneralSettings.setToggleFilters", "i", "312194"}, // 0x4c382
	{"GeneralSettings.setAutoReload", "b", "0"},
	{"GeneralSettings.setConnectionType", "i", "0"},
	{"GeneralSettings.setLCDDisplayModes", "i", "0"},
	{"GeneralSettings.setConfirmLaunchEADownloader", "b", "0"},
	{"GeneralSettings.setConfirmLaunchBFWeb", "b", "0"},
	{"GeneralSettings.setConfirmDuplicateKey", "b", "0"},
	{"GeneralSettings.setConfirmLogout", "b", "0"},
	{"GeneralSettings.setConfirmDiscardBuddyMessages", "b", "0"},
	{"GeneralSettings.setConfirmInstantAction", "b", "0"},
	{"GeneralSettings.setConfirmReservedSlotsWarning", "b", "0"},
};

// Properties in the console (0x004112c0..0x00412254); the writer writes
// them as setX, which the console maps to property X.
const Field kAudio[] = {
	{"AudioSettings.voipEnabled", "b", "0", false, true},
	{"AudioSettings.voipPlaybackVolume", "f", "0.5", false, true},
	{"AudioSettings.voipCaptureVolume", "f", "0.5", false, true},
	{"AudioSettings.voipCaptureThreshold", "f", "0.5", false, true},
	{"AudioSettings.voipBoostEnabled", "b", "0", false, true},
	{"AudioSettings.voipUsePushToTalk", "b", "1", false, true},
	{"AudioSettings.provider", "q", "Software", false, true},
	{"AudioSettings.soundQuality", "q", "Medium", false, true},
	{"AudioSettings.effectsVolume", "f", "1", false, true},
	{"AudioSettings.musicVolume", "f", "1", false, true},
	{"AudioSettings.helpVoiceVolume", "f", "1", false, true},
	{"AudioSettings.englishOnlyVoices", "b", "0", false, true},
	{"AudioSettings.enableEAX", "b", "0", false, true},
	{"AudioSettings.alwaysAllowSystemOpenAL", "b", "0", false, true},
	{"AudioSettings.forceNumberOfVoices", "i", "32", false, true},
};

const Field kVideo[] = {
	{"VideoSettings.setTerrainQuality", "i", "3"},
	{"VideoSettings.setGeometryQuality", "i", "3"},
	{"VideoSettings.setLightingQuality", "i", "3"},
	{"VideoSettings.setDynamicLightingQuality", "i", "2"},
	{"VideoSettings.setDynamicShadowsQuality", "i", "2"},
	{"VideoSettings.setEffectsQuality", "i", "3"},
	{"VideoSettings.setTextureQuality", "i", "3"},
	{"VideoSettings.setTextureFilteringQuality", "i", "3"},
	{"VideoSettings.setResolution", "s", "1280x720@60Hz"},
	{"VideoSettings.setAntialiasing", "s", "Off"},
	{"VideoSettings.setViewDistanceScale", "f", "1"},
	{"VideoSettings.setUseBloom", "b", "1"},
	{"VideoSettings.setVideoOptionScheme", "i", "3"},
};

const Field kGlobal[] = {
	{"GlobalSettings.setDefaultUser", "q", ""},
	{"GlobalSettings.setLastOnlineUser", "q", ""},
	{"GlobalSettings.setEncryptedLogin", "q", ""},
	{"GlobalSettings.setNamePrefix", "q", ""},
};

// LocalProfile's are properties named set... in the console table.
const Field kProfile[] = {
	{"LocalProfile.setName", "q", "", false, true},
	{"LocalProfile.setGamespyNick", "q", "", false, true},
	{"LocalProfile.setEAOnlineMasterAccount", "q", "", false, true},
	{"LocalProfile.setEAOnlineSubAccount", "q", "", false, true},
	{"LocalProfile.setEmail", "q", "", false, true},
	{"LocalProfile.setTotalPlayedTime", "f", "0"},
	{"LocalProfile.setNumTimesLoggedIn", "i", "0", false, true},
	{"LocalProfile.setRank", "i", "0", false, true},
	{"LocalProfile.setCareerPoints", "i", "0", false, true},
	{"LocalProfile.setLastBaseUpdate", "i", "0", false, true},
	{"LocalProfile.setCurrentProfileLatestAwards", "qqq", "", true},
	{"LocalProfile.setCurrentProfileMuted", "i", "", true},
	{"LocalProfile.setCurrentProfileHeavyArmor", "iib", "", true},
	{"LocalProfile.setCurrentProfileKitSetting", "iiii", "", true},
	{"LocalProfile.addDemoBookmark", "qqqi", "", true},
};

// "GeneralSettings.setMaxBots" -> "GeneralSettings.MaxBots",
// "AudioSettings.voipEnabled" -> "AudioSettings.VoipEnabled".
std::string keyOf(const std::string& method)
{
	const size_t dot = method.find('.');
	std::string name = method.substr(dot + 1);
	if ((name.compare(0, 3, "set") == 0 || name.compare(0, 3, "add") == 0) && name.size() > 3
		&& std::isupper(static_cast<unsigned char>(name[3]))) {
		name = name.substr(3);
	}
	name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
	return method.substr(0, dot) + "." + name;
}

// The writer's spelling: properties as setX.
std::string writtenName(const Field& field)
{
	if (!field.property) {
		return field.method;
	}
	const std::string key = keyOf(field.method);
	const size_t dot = key.find('.');
	return key.substr(0, dot) + ".set" + key.substr(dot + 1);
}

// An argument as the writer puts it: numbers as typed, bools 0/1, quoted
// text in quotes.
std::string formatArgs(const char* types, const std::vector<std::string>& args)
{
	std::string out;
	for (size_t i = 0; types[i]; ++i) {
		const std::string arg = i < args.size() ? args[i] : std::string();
		if (i) {
			out += ' ';
		}
		switch (types[i]) {
		case 'q': out += "\"" + arg + "\""; break;
		case 'b': out += std::atoi(arg.c_str()) != 0 ? "1" : "0"; break;
		case 'i': out += std::to_string(std::atol(arg.c_str())); break;
		case 'f': {
			std::ostringstream number;
			number << std::atof(arg.c_str()); // ostream's float, as the writer's <<
			out += number.str();
			break;
		}
		default: out += arg; break;
		}
	}
	return out;
}

std::vector<std::string> splitValue(const std::string& value, size_t count)
{
	if (count <= 1) {
		return {value};
	}
	std::istringstream words(value);
	std::vector<std::string> out;
	for (std::string word; words >> word;) {
		out.push_back(word);
	}
	return out;
}

template <size_t N>
std::string writeFields(const SettingsObjects& self, const Field (&fields)[N],
	const std::map<std::string, std::vector<std::string>>& lists)
{
	std::string out;
	for (const Field& field : fields) {
		const std::string key = keyOf(field.method);
		const size_t count = std::char_traits<char>::length(field.types);
		if (field.list) {
			if (const auto it = lists.find(key); it != lists.end()) {
				for (const std::string& entry : it->second) {
					out += std::string(field.method) + " " + entry + "\n";
				}
			}
			continue;
		}
		out += writtenName(field) + " " + formatArgs(field.types, splitValue(self.get(key), count)) + "\n";
	}
	return out;
}

} // namespace

SettingsObjects::SettingsObjects()
{
	const auto defaults = [this](const auto& fields) {
		for (const Field& field : fields) {
			if (!field.list && *field.fallback) {
				m_values[keyOf(field.method)] = field.fallback;
			} else if (!field.list) {
				m_values[keyOf(field.method)] = "";
			}
		}
	};
	defaults(kGeneral);
	defaults(kAudio);
	defaults(kVideo);
	defaults(kGlobal);
	defaults(kProfile);
}

void SettingsObjects::registerConsole(Console& console)
{
	const auto add = [this, &console](const Field& field) {
		const std::string key = keyOf(field.method);
		const size_t count = std::char_traits<char>::length(field.types);
		const bool list = field.list;
		const std::string types = field.types;
		ConsoleMethod method;
		method.name = field.method;
		method.access = ConsoleAccess::Anyone;
		method.kind = field.property ? ConsoleMethodKind::ReadWriteProperty : ConsoleMethodKind::Method;
		method.minArgs = field.property ? 0 : static_cast<int>(count);
		method.maxArgs = static_cast<int>(count);
		// A system's own method of the same name runs after this one.
		std::function<std::string(const std::vector<std::string>&)> previous;
		if (const ConsoleMethod* existing = console.findMethod(key.substr(0, key.find('.')), method.name.substr(method.name.find('.') + 1));
			existing && !existing->stub) {
			previous = existing->invoke;
			method.replaces = true;
		}
		method.invoke = [this, key, count, list, types, previous](const std::vector<std::string>& args) -> std::string {
			if (args.empty() && count > 0) {
				return get(key);
			}
			if (list) {
				m_lists[key].push_back(formatArgs(types.c_str(), args));
			} else {
				std::string value;
				for (size_t i = 0; i < args.size() && i < count; ++i) {
					value += (i ? " " : "") + args[i];
				}
				m_values[key] = value;
			}
			if (onChange) {
				onChange(key);
			}
			return previous ? previous(args) : std::string();
		};
		console.registerMethod(std::move(method));
	};
	for (const Field& field : kGeneral) {
		add(field);
	}
	for (const Field& field : kAudio) {
		add(field);
	}
	for (const Field& field : kVideo) {
		add(field);
	}
	for (const Field& field : kGlobal) {
		add(field);
	}
	for (const Field& field : kProfile) {
		add(field);
	}
	// LocalProfile.save (0x00410af9): Profile.con.
	ConsoleMethod save;
	save.name = "LocalProfile.save";
	save.access = ConsoleAccess::Anyone;
	save.invoke = [this](const std::vector<std::string>&) { return std::string(saveProfile() ? "1" : "0"); };
	console.registerMethod(std::move(save));

	// SettingsManager (0x0066cee1..0x0066df35): typed GS* settings.
	for (const char* type : {"string", "bool", "int", "float", "U32"}) {
		ConsoleMethod set;
		set.name = std::string("SettingsManager.") + type + "Set";
		set.access = ConsoleAccess::Anyone;
		set.minArgs = set.maxArgs = 2;
		set.invoke = [this, type = std::string(type)](const std::vector<std::string>& args) {
			std::string value = args[1];
			if (type == "bool") {
				value = std::atoi(value.c_str()) != 0 ? "1" : "0";
			}
			m_game[args[0]] = {type, value};
			return std::string("1");
		};
		console.registerMethod(std::move(set));
		ConsoleMethod get;
		get.name = std::string("SettingsManager.") + type + "Get";
		get.access = ConsoleAccess::Anyone;
		get.minArgs = get.maxArgs = 1;
		get.invoke = [this, type = std::string(type)](const std::vector<std::string>& args) {
			const auto it = m_game.find(args[0]);
			if (it == m_game.end()) {
				return std::string(type == "string" ? "" : "0");
			}
			return it->second.value;
		};
		console.registerMethod(std::move(get));
	}
}

std::string SettingsObjects::get(const std::string& key) const
{
	const auto it = m_values.find(key);
	return it == m_values.end() ? std::string() : it->second;
}

double SettingsObjects::number(const std::string& key, double fallback) const
{
	const auto it = m_values.find(key);
	if (it == m_values.end() || it->second.empty()) {
		return fallback;
	}
	char* end = nullptr;
	const double value = std::strtod(it->second.c_str(), &end);
	return end == it->second.c_str() ? fallback : value;
}

bool SettingsObjects::has(const std::string& key) const
{
	return m_values.count(key) != 0;
}

void SettingsObjects::set(const std::string& key, const std::string& value)
{
	m_values[key] = value;
}

std::string SettingsObjects::generalScript() const
{
	return writeFields(*this, kGeneral, m_lists);
}

std::string SettingsObjects::audioScript() const
{
	return writeFields(*this, kAudio, m_lists);
}

std::string SettingsObjects::videoScript() const
{
	return writeFields(*this, kVideo, m_lists);
}

std::string SettingsObjects::globalScript() const
{
	return writeFields(*this, kGlobal, m_lists);
}

std::string SettingsObjects::profileScript() const
{
	return writeFields(*this, kProfile, m_lists);
}

// The format of the shipped Settings/Usersettings.con.
std::string SettingsObjects::userSettingsScript() const
{
	std::string out = "rem\nrem Settingsfile automatically saved by bf2142\nrem\n";
	for (const auto& [name, typed] : m_game) {
		out += "SettingsManager." + typed.type + "Set " + name + " " +
			(typed.type == "string" ? "\"" + typed.value + "\"" : typed.value) + "\n";
	}
	return out;
}

void SettingsObjects::setDirectory(const std::string& profilesDirectory, const std::string& profile)
{
	m_directory = profilesDirectory;
	m_profile = profile;
}

std::string SettingsObjects::defaultDirectory()
{
	const char* home = std::getenv("HOME");
#ifdef _WIN32
	if (const char* appData = std::getenv("APPDATA")) {
		home = appData;
	}
#endif
	if (!home || !*home) {
		return std::string();
	}
#ifdef __APPLE__
	const std::filesystem::path base = std::filesystem::path(home) / "Library" / "Application Support";
#elif defined(_WIN32)
	const std::filesystem::path base = home;
#else
	const char* xdg = std::getenv("XDG_CONFIG_HOME");
	const std::filesystem::path base = xdg && *xdg ? std::filesystem::path(xdg) : std::filesystem::path(home) / ".config";
#endif
	return (base / "bf2142-recomp" / "Profiles").string();
}

std::string SettingsObjects::file(const std::string& name, bool global) const
{
	if (m_directory.empty()) {
		return std::string();
	}
	const std::filesystem::path dir = global ? std::filesystem::path(m_directory) : std::filesystem::path(m_directory) / m_profile;
	return (dir / name).string();
}

bool SettingsObjects::load(Console& console)
{
	bool any = false;
	for (const auto& [name, global] : std::vector<std::pair<const char*, bool>> {{"Global.con", true}, {"Usersettings.con", true},
			 {"Profile.con", false}, {"General.con", false}, {"Audio.con", false}, {"Video.con", false}}) {
		const std::string path = file(name, global);
		std::ifstream in(path);
		if (path.empty() || !in) {
			continue;
		}
		any = true;
		std::string output;
		for (std::string line; std::getline(in, line);) {
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			console.execute(line, output, ConsoleAccess::Anyone);
		}
	}
	return any;
}

bool SettingsObjects::write(const std::string& path, const std::string& text) const
{
	if (path.empty()) {
		return false;
	}
	std::error_code error;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
	std::ofstream out(path, std::ios::binary);
	out << text;
	return static_cast<bool>(out);
}

bool SettingsObjects::save() const
{
	bool ok = write(file("Global.con", true), globalScript());
	ok = write(file("General.con"), generalScript()) && ok;
	ok = write(file("Audio.con"), audioScript()) && ok;
	ok = write(file("Video.con"), videoScript()) && ok;
	ok = saveProfile() && ok;
	if (!m_game.empty()) {
		ok = saveUserSettings() && ok;
	}
	return ok;
}

bool SettingsObjects::saveProfile() const
{
	return write(file("Profile.con"), profileScript());
}

bool SettingsObjects::saveUserSettings() const
{
	return write(file("Usersettings.con", true), userSettingsScript());
}

} // namespace dice::hfe::menu
