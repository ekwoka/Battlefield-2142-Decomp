// The menu natives that work without an account or a live service, as
// SwiffPlayer.dll binds them (its native table, src/UI/Player/
// NativeTable.cpp, gives each object's constructor): the favourite servers
// by address, the direct IP, the browser's filters and join password, the
// map list's own natives and the current game (dice.bf2.Multiplay,
// 0x100475c0), the mods (dice.bf2.Mod, 0x100409a0), the cursor
// (dice.bf2.Cursor), dice.bf2.Singleplay, and Logic's and the settings
// objects' small natives. Most of them call SwiffHost (BF2142.exe, vtable
// 0x00924d50; SwiffPlayer reaches it through its game interface's slot
// 0x60) or GeneralSettings (vtable 0x00927450); the slots are named where
// the behaviour comes from.
#include "MenuBindings.h"

#include "EndOfRoundBindings.h"
#include "Hud/HudFont.h"
#include "IO/Localization/LocaleFormat.h"
#include "Input/ControlMap.h"
#include "Net/Demo.h"
#include "SettingsObjects.h"
#include "UI/Player/Natives.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dice::hfe::menu {

namespace avm1 = ui::avm1;
using avm1::Interpreter;
using avm1::Value;
using Args = std::vector<Value>;

namespace {

constexpr const char* kLogic = "dice.bf2.Logic";
constexpr const char* kMultiplay = "dice.bf2.Multiplay";
constexpr const char* kMod = "dice.bf2.Mod";
constexpr const char* kCursor = "dice.bf2.Cursor";
constexpr const char* kSingleplay = "dice.bf2.Singleplay";
constexpr const char* kGeneralSettings = "dice.bf2.GeneralSettings";
constexpr const char* kVideoSettings = "dice.bf2.VideoSettings";
constexpr const char* kAudioSettings = "dice.bf2.AudioSettings";
constexpr const char* kProfile = "dice.bf2.Profile";
constexpr const char* kServerInfo = "dice.bf2.ServerInfo";
constexpr const char* kControlSettings = "dice.bf2.ControlSettings";
constexpr const char* kBookmarkList = "LocalProfile.DemoBookmark"; // addDemoBookmark's entries

constexpr const char* kFavouriteList = "GeneralSettings.FavouriteServer"; // addFavouriteServer's entries
constexpr const char* kHistoryList = "GeneralSettings.ServerHistory";     // addServerHistory's
// 0x0046c030: GSConnectToIpPort's default.
constexpr int kDefaultConnectPort = 0x449f;

Value arg(const Args& args, size_t i) { return i < args.size() ? args[i] : Value(); }
std::string lowerText(std::string text)
{
	for (char& c : text) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return text;
}
int intArg(Interpreter& vm, const Args& args, size_t i) { return static_cast<int>(std::lround(vm.toNumber(arg(args, i)))); }

// "ip gameport queryport", as the server rows give serverAddress (the
// natives read it with sscanf "%s %d %d").
bool parseAddress(const std::string& text, std::string& ip, int& port, int& queryPort)
{
	std::istringstream words(text);
	ip.clear();
	port = queryPort = 0;
	words >> ip >> port >> queryPort;
	return !ip.empty();
}

// A console line's arguments: words, or text in quotes.
std::vector<std::string> splitArguments(const std::string& entry)
{
	std::vector<std::string> words;
	for (size_t i = 0; i < entry.size();) {
		if (std::isspace(static_cast<unsigned char>(entry[i]))) {
			++i;
		} else if (entry[i] == '"') {
			const size_t end = entry.find('"', i + 1);
			words.push_back(entry.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1));
			i = end == std::string::npos ? entry.size() : end + 1;
		} else {
			size_t end = i;
			while (end < entry.size() && !std::isspace(static_cast<unsigned char>(entry[end]))) {
				++end;
			}
			words.push_back(entry.substr(i, end - i));
			i = end;
		}
	}
	return words;
}

// One addFavouriteServer / addServerHistory entry as General.con has it:
// "ip" port "name" queryPort.
MenuBindings::FavouriteServer parseEntry(const std::string& entry)
{
	const std::vector<std::string> words = splitArguments(entry);
	MenuBindings::FavouriteServer server;
	server.ip = words.size() > 0 ? words[0] : std::string();
	server.port = words.size() > 1 ? std::atoi(words[1].c_str()) : 0;
	server.name = words.size() > 2 ? words[2] : std::string();
	server.queryPort = words.size() > 3 ? std::atoi(words[3].c_str()) : 0;
	return server;
}

std::string formatEntry(const MenuBindings::FavouriteServer& server)
{
	return "\"" + server.ip + "\" " + std::to_string(server.port) + " \"" + server.name + "\" " + std::to_string(server.queryPort);
}

// The natives compare entries as "%s %d" of the address and game port.
std::string key(const std::string& ip, int port) { return ip + " " + std::to_string(port); }

// SwiffHost slot 0x28c (0x0046b720): digits and exactly three dots, then
// four numbers 0..255 ("%d.%d.%d.%d").
bool validIp(const std::string& ip)
{
	int dots = 0;
	for (const char c : ip) {
		if (c == '.') {
			++dots;
		} else if (c < '0' || c > '9') {
			return false;
		}
	}
	int part[4];
	if (ip.empty() || dots != 3 || std::sscanf(ip.c_str(), "%d.%d.%d.%d", &part[0], &part[1], &part[2], &part[3]) != 4) {
		return false;
	}
	return std::all_of(std::begin(part), std::end(part), [](int v) { return v >= 0 && v < 256; });
}

std::string underscoresAsSpaces(std::string text)
{
	std::replace(text.begin(), text.end(), '_', ' ');
	return text;
}

std::string trim(const std::string& text)
{
	const size_t begin = text.find_first_not_of(" \t\r\n");
	const size_t end = text.find_last_not_of(" \t\r\n");
	return begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
}

std::string serverField(const net::ServerEntry& server, const char* name)
{
	const auto it = server.fields.find(name);
	return it == server.fields.end() ? std::string() : it->second;
}

// 0x10007aa0: a time_t as GetDateFormatA and GetTimeFormatA give it in the
// game's language (0x10007730's LCID; io::formatDateTime), the two joined by
// a space; "" for 0.
std::string demoDate(uint32_t time)
{
	return io::formatDateTime(static_cast<std::time_t>(time), io::languageLcid(hud::language()));
}

// 0x10007b70: the date alone (the bookmarks', 0x10064510).
std::string bookmarkDate(uint32_t time)
{
	return io::formatDate(static_cast<std::time_t>(time), io::languageLcid(hud::language()));
}

// 0x0046b8a0: a local network's address (10/8, 172.16/12, 192.168/16).
bool privateAddress(const std::string& ip)
{
	int a = 0, b = 0, c = 0, d = 0;
	if (std::sscanf(ip.c_str(), "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
		return false;
	}
	return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}

} // namespace

// --- Mods

MenuBindings::ModEntry MenuBindings::parseModDesc(const std::string& path, const std::string& text)
{
	ModEntry mod;
	mod.path = path;
	const auto tag = [&text](const char* name) {
		const std::string open = std::string("<") + name + ">", close = std::string("</") + name + ">";
		const size_t begin = text.find(open);
		if (begin == std::string::npos) {
			return std::string();
		}
		const size_t end = text.find(close, begin);
		return trim(text.substr(begin + open.size(), end == std::string::npos ? std::string::npos : end - begin - open.size()));
	};
	mod.title = tag("title");
	mod.desc = tag("desc");
	mod.url = tag("url");
	mod.logo = tag("logo");
	mod.icon = tag("icon");
	mod.music = tag("music");
	mod.version = tag("version");
	return mod;
}

std::vector<MenuBindings::ModEntry> MenuBindings::findMods(const std::string& gameDir)
{
	namespace fs = std::filesystem;
	std::vector<ModEntry> mods;
	std::error_code error;
	std::vector<fs::path> folders;
	for (const auto& entry : fs::directory_iterator(fs::path(gameDir) / "mods", error)) {
		if (entry.is_directory(error)) {
			folders.push_back(entry.path());
		}
	}
	std::sort(folders.begin(), folders.end());
	for (const fs::path& folder : folders) {
		for (const auto& file : fs::directory_iterator(folder, error)) {
			std::string name = file.path().filename().string();
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (name != "mod.desc") {
				continue;
			}
			std::ifstream in(file.path(), std::ios::binary);
			std::stringstream text;
			text << in.rdbuf();
			mods.push_back(parseModDesc("mods/" + folder.filename().string(), text.str()));
		}
	}
	return mods;
}

void MenuBindings::setMods(std::vector<ModEntry> mods, const std::string& currentModPath)
{
	m_mods = std::move(mods);
	m_currentModPath = currentModPath;
	if (m_selectedMod >= static_cast<int>(m_mods.size())) {
		m_selectedMod = -1;
	}
	// The "mods" list (its row class 0x10107d88: slot 4 0x10063dc0 counts
	// them through SwiffHost slot 0x1a4, slot 5 0x10063f60 writes a row
	// from the record of slot 0x1a8): the localized title, the version.
	ui::NativeRegistry::Rows rows;
	for (const ModEntry& mod : m_mods) {
		rows.push_back({m_natives.localize(mod.title), mod.version});
	}
	m_natives.setListRows("mods", rows);
}

void MenuBindings::bindMods()
{
	ui::NativeRegistry& n = m_natives;
	const auto selected = [this]() -> const ModEntry* {
		return m_selectedMod >= 0 && m_selectedMod < static_cast<int>(m_mods.size()) ? &m_mods[static_cast<size_t>(m_selectedMod)] : nullptr;
	};
	// 0x1003fc10: SwiffHost slot 0x1a4 (0x0046d530), ModInfo's count.
	n.setFunction(kMod, "getNumMods", [this](Interpreter&, const Args&) { return Value(static_cast<double>(m_mods.size())); });
	// 0x1003fde0: the row index the page passes (rowObject.rowIndex).
	n.setFunction(kMod, "selectMod", [this](Interpreter& vm, const Args& args) {
		m_selectedMod = intArg(vm, args, 0);
		return Value();
	});
	// 0x10040000 / 0x10040160: the title's and description's keys
	// localized; "" with none selected. 0x100402c0, 0x100405e0, 0x10040720,
	// 0x10040860: the folder, the web address, the logo and the version
	// as written.
	const auto text = [&n, selected](std::string ModEntry::*member, bool localized) {
		return [selected, member, localized, &n](Interpreter&, const Args&) {
			const ModEntry* mod = selected();
			if (!mod) {
				return Value("");
			}
			return Value(localized ? n.localize(mod->*member) : mod->*member);
		};
	};
	n.setFunction(kMod, "getModTitle", text(&ModEntry::title, true));
	n.setFunction(kMod, "getModDesc", text(&ModEntry::desc, true));
	n.setFunction(kMod, "getModPath", text(&ModEntry::path, false));
	n.setFunction(kMod, "getModUrl", text(&ModEntry::url, false));
	n.setFunction(kMod, "getModLogo", text(&ModEntry::logo, false));
	n.setFunction(kMod, "getModVersion", text(&ModEntry::version, false));
	// 0x10040400: the folder after its first '/' or '\' ("bf2142").
	n.setFunction(kMod, "getModPathShort", [selected](Interpreter&, const Args&) {
		const ModEntry* mod = selected();
		if (!mod) {
			return Value("");
		}
		const size_t slash = mod->path.find_first_of("/\\");
		return Value(slash == std::string::npos ? std::string() : mod->path.substr(slash + 1));
	});
	// 0x1003ff50: SwiffHost slot 0x39c (0x0046c430 -> 0x00576c70):
	// GSModDirectory after its first '/', lower-cased ("(unknown)" unset).
	n.setFunction(kMod, "getCurrentModName", [this](Interpreter&, const Args&) {
		std::string name = m_currentModPath.empty() ? std::string("(unknown)") : m_currentModPath;
		if (const size_t slash = name.find('/'); slash != std::string::npos) {
			name.erase(0, slash + 1);
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		}
		return Value(name);
	});
	// 0x1003fd30: with a mod selected, its record (slot 0x1a8) to
	// SwiffHost slot 0x1ac (0x00477fb0: host event 0xc), which restarts
	// the game on that mod (0x00403fa0: "+modPath <path> +multi 1", and
	// "+fullscreen 0" when windowed); nothing without one.
	n.setFunction(kMod, "activateMod", [this, selected](Interpreter&, const Args&) {
		if (const ModEntry* mod = selected()) {
			log("activate mod " + mod->path);
			if (onActivateMod) {
				onActivateMod(mod->path);
			}
		}
		return Value();
	});
	// 0x1003fe20: SwiffHost slot 0x3a0 (0x00469a60) only answers true.
	n.setFunction(kMod, "activateModAndJoinServer", [](Interpreter&, const Args&) { return Value(); });
}

// --- Favourites

std::vector<MenuBindings::FavouriteServer> MenuBindings::favouriteServers(bool history) const
{
	if (!m_objects) {
		return history ? m_history : m_favourites;
	}
	std::vector<FavouriteServer> servers;
	for (const std::string& entry : m_objects->list(history ? kHistoryList : kFavouriteList)) {
		const FavouriteServer server = parseEntry(entry);
		// The console adds each line as it comes; the list holds one entry
		// an address (0x0049ce90 updates the one it finds).
		const auto same = std::find_if(servers.begin(), servers.end(),
			[&](const FavouriteServer& s) { return s.ip == server.ip && s.port == server.port; });
		if (same == servers.end()) {
			servers.push_back(server);
		} else {
			*same = server;
		}
	}
	return servers;
}

void MenuBindings::storeFavourites(bool history, const std::vector<FavouriteServer>& servers)
{
	if (m_objects) {
		std::vector<std::string> entries;
		for (const FavouriteServer& server : servers) {
			entries.push_back(formatEntry(server));
		}
		m_objects->setList(history ? kHistoryList : kFavouriteList, std::move(entries));
		// The original writes General.con at its next profile save; here
		// at once, so a favourite outlives a crash.
		m_objects->save();
	} else {
		(history ? m_history : m_favourites) = servers;
	}
	refreshFavourites();
}

void MenuBindings::refreshFavourites()
{
	const auto rows = [this](const std::vector<FavouriteServer>& entries) {
		std::vector<Server> servers;
		for (const FavouriteServer& f : entries) {
			Server server;
			server.gamePort = static_cast<uint16_t>(std::clamp(f.port, 0, 0xffff));
			// The browser's own row when it lists the same server (the
			// original pings the favourites through its "favourites"
			// browser; this rewrite contacts no server for them).
			bool found = false;
			for (const auto* listed : {&m_lanServers, &m_servers}) {
				for (const Server& s : *listed) {
					if (!found && s.entry.ip == f.ip && s.gamePort == server.gamePort) {
						server.entry = s.entry;
						found = true;
					}
				}
			}
			if (!found) {
				server.entry.ip = f.ip;
				server.entry.port = static_cast<uint16_t>(std::clamp(f.queryPort, 0, 0xffff));
				server.entry.fields["hostname"] = f.name.empty() ? key(f.ip, f.port) : f.name;
				server.entry.fields["hostport"] = std::to_string(f.port);
			}
			servers.push_back(std::move(server));
		}
		return servers;
	};
	m_favouriteServers = rows(favouriteServers(false));
	m_historyServers = rows(favouriteServers(true));
	ui::NativeRegistry::Rows favourites, history;
	for (const Server& s : m_favouriteServers) {
		favourites.push_back({s.entry.fields.count("hostname") ? s.entry.fields.at("hostname") : s.entry.ip});
	}
	for (const Server& s : m_historyServers) {
		history.push_back({s.entry.fields.count("hostname") ? s.entry.fields.at("hostname") : s.entry.ip});
	}
	m_natives.setListRows("favouriteServers", favourites);
	m_natives.setListRows("serverHistory", history);
	showServers();
}

void MenuBindings::bindFavourites()
{
	ui::NativeRegistry& n = m_natives;
	// GeneralSettings slot 0x17c (0x0049ce90): a server already there
	// takes the new name (and query port when one is given); else it is
	// added, its query port from the history's entry when it has one.
	const auto add = [this](const FavouriteServer& server) {
		std::vector<FavouriteServer> servers = favouriteServers(false);
		const auto it = std::find_if(servers.begin(), servers.end(),
			[&](const FavouriteServer& s) { return s.ip == server.ip && s.port == server.port; });
		if (it != servers.end()) {
			it->name = server.name;
			if (server.queryPort) {
				it->queryPort = server.queryPort;
			}
		} else {
			FavouriteServer added = server;
			for (const FavouriteServer& h : favouriteServers(true)) {
				if (h.ip == server.ip && h.port == server.port) {
					added.queryPort = h.queryPort;
				}
			}
			servers.push_back(added);
		}
		storeFavourites(false, servers);
	};
	const auto findIn = [this](bool history, const std::string& entryKey) {
		const std::vector<FavouriteServer> servers = favouriteServers(history);
		return std::find_if(servers.begin(), servers.end(), [&](const FavouriteServer& s) { return key(s.ip, s.port) == entryKey; })
			- servers.begin();
	};
	const auto removeAt = [this](bool history, size_t index) {
		std::vector<FavouriteServer> servers = favouriteServers(history);
		if (index >= servers.size()) {
			return false;
		}
		servers.erase(servers.begin() + static_cast<long>(index));
		storeFavourites(history, servers);
		return true;
	};
	// 0x10044440, a server row's star (rowObject.serverAddress): false when
	// it is a favourite already (slot 0x194) or the browser doesn't list it
	// (the browser's slot 0x3c); else added with the browser's name, no
	// query port.
	n.setFunction(kMultiplay, "addFavouriteServer", [this, add, findIn](Interpreter& vm, const Args& args) {
		std::string ip;
		int port = 0, queryPort = 0;
		if (!parseAddress(vm.toString(arg(args, 0)), ip, port, queryPort)
			|| findIn(false, key(ip, port)) < static_cast<long>(favouriteServers(false).size())) {
			return Value(false);
		}
		for (const auto* listed : {&m_servers, &m_lanServers, &m_historyServers}) {
			for (const Server& s : *listed) {
				if (s.entry.ip == ip && s.gamePort == port) {
					const auto name = s.entry.fields.find("hostname");
					add({ip, port, name == s.entry.fields.end() ? std::string() : name->second, 0});
					log("favourite added: " + key(ip, port));
					return Value(true);
				}
			}
		}
		return Value(false);
	});
	// 0x10044620: slot 0x190 (0x0049ca90) removes the address and game
	// port's entry; true when there was one.
	n.setFunction(kMultiplay, "removeFavouriteServer", [findIn, removeAt](Interpreter& vm, const Args& args) {
		std::string ip;
		int port = 0, queryPort = 0;
		parseAddress(vm.toString(arg(args, 0)), ip, port, queryPort);
		return Value(removeAt(false, static_cast<size_t>(findIn(false, key(ip, port)))));
	});
	// 0x10042d20: addFavourite(name, ip, port), a server by its address.
	// A bad address raises LOCERROR_invalidip, one there already
	// LOCERROR_serveralreadyfavorite (type 3 messages, SwiffHost slot
	// 0x218); else it is added without a query port.
	n.setFunction(kMultiplay, "addFavourite", [this, add, findIn](Interpreter& vm, const Args& args) {
		const std::string name = vm.toString(arg(args, 0)), ip = vm.toString(arg(args, 1));
		const int port = intArg(vm, args, 2);
		if (!validIp(ip)) {
			raiseMessage(3, "invalidip");
			return Value(false);
		}
		if (findIn(false, key(ip, port)) < static_cast<long>(favouriteServers(false).size())) {
			raiseMessage(3, "serveralreadyfavorite");
			return Value(false);
		}
		add({ip, port, name, 0});
		log("favourite added: " + key(ip, port));
		return Value(true);
	});
	// 0x10043110: by "ip port" (slot 0x18c, 0x0049b600, then slot 0x190).
	n.setFunction(kMultiplay, "removeFavourite", [findIn, removeAt](Interpreter& vm, const Args& args) {
		return Value(removeAt(false, static_cast<size_t>(findIn(false, vm.toString(arg(args, 0))))));
	});
	// 0x100432f0: the history's entry for "ip port" made a favourite;
	// "Server allready in favourites" (a type 1 message) when it is one.
	n.setFunction(kMultiplay, "addHistoryToFavourites", [this, add, findIn](Interpreter& vm, const Args& args) {
		const std::string entryKey = vm.toString(arg(args, 0));
		const std::vector<FavouriteServer> history = favouriteServers(true);
		const long index = findIn(true, entryKey);
		if (index >= static_cast<long>(history.size())) {
			return Value(false);
		}
		if (findIn(false, entryKey) < static_cast<long>(favouriteServers(false).size())) {
			raiseMessage(1, "Server allready in favourites");
			return Value(false);
		}
		const FavouriteServer& h = history[static_cast<size_t>(index)];
		add({h.ip, h.port, h.name, 0});
		return Value(true);
	});
	// 0x10043610 (slot 0x1a8) and 0x10041440 (slot 0x1b4): the history.
	n.setFunction(kMultiplay, "removeHistory", [findIn, removeAt](Interpreter& vm, const Args& args) {
		return Value(removeAt(true, static_cast<size_t>(findIn(true, vm.toString(arg(args, 0))))));
	});
	n.setFunction(kMultiplay, "clearHistory", [this](Interpreter&, const Args&) {
		storeFavourites(true, {});
		return Value();
	});
	// GeneralSettings.removeServer (0x10031410), the history page's: the
	// address's favourite (slot 0x194, then 0x190), else its history
	// entry (slots 0x1b0 / 0x1ac).
	n.setFunction(kGeneralSettings, "removeServer", [findIn, removeAt](Interpreter& vm, const Args& args) {
		std::string ip;
		int port = 0, queryPort = 0;
		parseAddress(vm.toString(arg(args, 0)), ip, port, queryPort);
		if (!removeAt(false, static_cast<size_t>(findIn(false, key(ip, port))))) {
			removeAt(true, static_cast<size_t>(findIn(true, key(ip, port))));
		}
		return Value();
	});
	// 0x100429f0: SwiffHost slot 0x28c.
	n.setFunction(kMultiplay, "validateIp", [](Interpreter& vm, const Args& args) { return Value(validIp(vm.toString(arg(args, 0)))); });
	// 0x100447e0 parses the address and answers false whatever it is.
	n.setFunction(kMultiplay, "isServerOnline", [](Interpreter&, const Args&) { return Value(false); });

	// The direct IP: SwiffHost slots 0x33c / 0x340 (0x0046c030 /
	// 0x0046c080, GSConnectToIpPort, 17567 unset) and 0x344 / 0x348
	// (0x0046c0c0 / 0x0046c120, GSConnectToIpHost, "" unset).
	n.setFunction(kMultiplay, "getConnectToIpHost", [this](Interpreter&, const Args&) {
		return Value(m_objects ? m_objects->gameSetting("GSConnectToIpHost", "") : m_connectHost);
	});
	n.setFunction(kMultiplay, "setConnectToIpHost", [this](Interpreter& vm, const Args& args) {
		m_connectHost = vm.toString(arg(args, 0));
		if (m_objects) {
			m_objects->setGameSetting("GSConnectToIpHost", "string", m_connectHost);
			m_objects->saveUserSettings();
		}
		return Value();
	});
	n.setFunction(kMultiplay, "getConnectToIpPort", [this](Interpreter&, const Args&) {
		if (m_objects) {
			return Value(std::atof(m_objects->gameSetting("GSConnectToIpPort", std::to_string(kDefaultConnectPort)).c_str()));
		}
		return Value(m_connectPort);
	});
	n.setFunction(kMultiplay, "setConnectToIpPort", [this](Interpreter& vm, const Args& args) {
		m_connectPort = intArg(vm, args, 0);
		if (m_objects) {
			m_objects->setGameSetting("GSConnectToIpPort", "int", std::to_string(m_connectPort));
			m_objects->saveUserSettings();
		}
		return Value();
	});

	// The browser's check boxes (0x10042a90 / 0x10046dd0): GeneralSettings
	// slots 0x168 / 0x170 (0x0049a230 / 0x0049a250) test and set the bit
	// in the toggle filters (+0xf8, setToggleFilters, 0x4c382 by default).
	// setToggleFilter(bit, on, query) also keeps the bit's query text for
	// the master server's filter (an empty or "%%" one clears the bit and
	// the text); the LAN search filters nothing.
	const auto filters = [this]() {
		const auto it = m_settings.find("GeneralSettings.ToggleFilters");
		return static_cast<uint32_t>(it == m_settings.end() || !it->second.isNumber() ? 0x4c382 : it->second.asNumber());
	};
	n.setFunction(kMultiplay, "getToggleFilter", [filters](Interpreter& vm, const Args& args) {
		return Value((filters() & static_cast<uint32_t>(intArg(vm, args, 0))) != 0);
	});
	n.setFunction(kMultiplay, "setToggleFilter", [this, filters](Interpreter& vm, const Args& args) {
		const uint32_t bit = static_cast<uint32_t>(intArg(vm, args, 0));
		const std::string query = vm.toString(arg(args, 2));
		const bool on = avm1::toBoolean(arg(args, 1)) && !query.empty() && query != "%%";
		const uint32_t value = on ? filters() | bit : filters() & ~bit;
		if (on) {
			m_toggleFilterValues[static_cast<int>(bit)] = query;
		} else {
			m_toggleFilterValues.erase(static_cast<int>(bit));
		}
		m_settings["GeneralSettings.ToggleFilters"] = m_appliedSettings["GeneralSettings.ToggleFilters"] =
			Value(static_cast<double>(value));
		if (m_objects) {
			m_objects->set("GeneralSettings.ToggleFilters", std::to_string(value));
		}
		return Value();
	});
	// 0x10041b10 / 0x10042b50: SwiffHost slots 0x18c / 0x188, the password
	// the next join sends.
	n.setFunction(kMultiplay, "setJoinPassword", [this](Interpreter& vm, const Args& args) {
		m_joinPassword = vm.toString(arg(args, 0));
		return Value();
	});
	n.setFunction(kMultiplay, "getJoinPassword", [this](Interpreter&, const Args&) { return Value(m_joinPassword); });
}

// --- The rest

void MenuBindings::setCurrentGame(const MenuMap& map, int maxPlayers)
{
	m_currentGame = CurrentGame {map, maxPlayers};
}

void MenuBindings::resetGeneralValues(const std::vector<std::pair<std::string, Value>>& values)
{
	for (const auto& [name, value] : values) {
		m_settings[name] = m_appliedSettings[name] = value;
	}
	saveSettings();
	exportSettings("GeneralSettings.");
	if (onSettingsApplied) {
		onSettingsApplied();
	}
}

void MenuBindings::bindOffline()
{
	ui::NativeRegistry& n = m_natives;
	bindFavourites();
	bindMods();
	bindCommandLine();
	bindDemos();
	bindMapTexts();

	// --- The map list's own natives (the pages use the index ones).
	// 0x100414c0 setCurrentMap and 0x100414d0 setMapList do nothing.
	n.setFunction(kMultiplay, "setCurrentMap", [](Interpreter&, const Args&) { return Value(); });
	n.setFunction(kMultiplay, "setMapList", [](Interpreter&, const Args&) { return Value(); });
	// 0x10041e60: (name, game mode, size) to SwiffHost slot 0x104
	// (0x00479970, appended), unless the name is empty.
	n.setFunction(kMultiplay, "addMapToMapList", [this](Interpreter& vm, const Args& args) {
		const std::string level = vm.toString(arg(args, 0));
		if (!level.empty()) {
			m_mapList.push_back({level, vm.toString(arg(args, 1)), intArg(vm, args, 2)});
			showMapLists();
		}
		return Value();
	});
	// 0x100448c0: (index, up) to slot 0x10c (0x00479a70): swapped with the
	// one before (up) or after, not past the ends; true.
	n.setFunction(kMultiplay, "moveMapInList", [this](Interpreter& vm, const Args& args) {
		const int index = intArg(vm, args, 0);
		const bool up = avm1::toBoolean(arg(args, 1));
		const int other = up ? index - 1 : index + 1;
		const int count = static_cast<int>(m_mapList.size());
		if (count > 1 && index >= 0 && index < count && other >= 0 && other < count) {
			std::swap(m_mapList[static_cast<size_t>(index)], m_mapList[static_cast<size_t>(other)]);
			showMapLists();
		}
		return Value(true);
	});
	// 0x10047090: the chosen list's level (slot 0x11c) with spaces for
	// underscores, "" past its end.
	n.setFunction(kMultiplay, "getMapName", [this](Interpreter& vm, const Args& args) {
		const int index = intArg(vm, args, 0);
		if (index < 0 || index >= static_cast<int>(m_mapList.size())) {
			return Value("");
		}
		return Value(underscoresAsSpaces(m_mapList[static_cast<size_t>(index)].level));
	});
	// 0x10044cc0..0x10044e60: SwiffHost slots 0x70..0x7c (0x0046a480..
	// 0x0046a520) ask the game logic; "$noCurrentGame" and 0 without one.
	n.setFunction(kMultiplay, "getCurrentMapName", [this](Interpreter&, const Args&) {
		return Value(m_currentGame ? underscoresAsSpaces(m_currentGame->map.level) : std::string("$noCurrentGame"));
	});
	n.setFunction(kMultiplay, "getCurrentMapGameMode", [this](Interpreter&, const Args&) {
		return Value(m_currentGame ? m_currentGame->map.gameMode : std::string("$noCurrentGame"));
	});
	n.setFunction(kMultiplay, "getCurrentMapMaxPlayers", [this](Interpreter&, const Args&) {
		return Value(m_currentGame ? m_currentGame->maxPlayers : 0);
	});
	n.setFunction(kMultiplay, "getCurrentMapSize", [this](Interpreter&, const Args&) {
		return Value(m_currentGame ? m_currentGame->map.size : 0);
	});

	// --- dice.bf2.Singleplay (0x10059c20): chooseMission (0x10059bb0) is
	// a bare return; getBotSkills / setBotSkills (0x10059bc0 / 0x10059bd0)
	// fetch SwiffHost and return. The shipped menus call none of them (the
	// singleplayer page sets GeneralSettings.setBotSkill).
	for (const char* member : {"chooseMission", "getBotSkills", "setBotSkills"}) {
		n.setFunction(kSingleplay, member, [](Interpreter&, const Args&) { return Value(); });
	}

	// --- dice.bf2.Cursor (0x1001b6e0). The shipped menus call none of these.
	// setCursor (0x1001b520) is SwiffHost slot 0x80 (0x00469c60): the
	// "SwiffPlayer" object's slot 0x64 (0x10008ed0) asks its SwiffRender
	// (the player's +0xc, given by slot 0x24) to set the cursor, slot 50,
	// which RendDX9 leaves a stub answering true (0x101177b0): the name is
	// dropped. On that true the window object (0x0056c1d0, DAT_00a6779c)
	// gets +0x3b cleared, the flag its WM_SETCURSOR answer (0x0056bc40)
	// reads to hide the cursor (SetCursor(NULL)); so setCursor, whatever the
	// name, shows the window's cursor. The rewrite's menu never hides it
	// (ViewerOptions::menuCursor), so that is already the case; cursorShown()
	// keeps the flag. getCursor (0x1001b5d0, slots 0x88 -> 0x68 -> 51, the
	// stub 0x101177c0) answers true without filling the name: "" (it is
	// "undefined" only with no SwiffRender).
	n.setFunction(kCursor, "setCursor", [this](Interpreter&, const Args&) {
		m_cursorShown = true;
		return Value();
	});
	n.setFunction(kCursor, "getCursor", [](Interpreter&, const Args&) { return Value(std::string()); });
	// enableCursor (0x1001b580) is SwiffHost slot 0x8c (0x00469e50: +0x48).
	// Its only reader, slot 0x90 (0x00469e60), is reached through the
	// player's second interface (vtable 0x10101bd0, slot 30 0x10008e40),
	// which neither BF2142.exe nor SwiffPlayer.dll calls: the switch is kept
	// and reaches nothing, as in the original.
	n.setFunction(kCursor, "enableCursor", [this](Interpreter&, const Args& args) {
		m_cursorEnabled = avm1::toBoolean(arg(args, 0));
		return Value();
	});

	// --- The credits' images (no shipped menu calls these).
	// Logic.cacheCredits (0x1003b0e0) is SwiffRender's slot 0xf4 (the
	// player's +0xc; RendDX9 0x10119670): when it holds none yet, it loads
	// images/credits/1.png .. 113.png (0x71), keeping each that loads
	// (+0x45c). getRandomCreditsTexture (0x1003c440, slot 0x100,
	// 0x10118080) gives one of them at random (0x101aa890: rand() over
	// [0, count - 1], seeded with time() once), "" with none (0x101181e0).
	// The game ships no credits images.
	n.setFunction(kLogic, "cacheCredits", [this](Interpreter&, const Args&) {
		if (m_creditTextures.empty() && readFile) {
			for (int i = 1; i <= 0x71; ++i) {
				const std::string path = "images/credits/" + std::to_string(i) + ".png";
				std::string data;
				// Beside the movie, as the menu's other images.
				if (readFile("Menu/External/FlashMenu/" + path, data)) {
					m_creditTextures.push_back(path);
				}
			}
		}
		return Value();
	});
	n.setFunction(kLogic, "getRandomCreditsTexture", [this](Interpreter&, const Args&) {
		static const bool seeded = (std::srand(static_cast<unsigned>(std::time(nullptr))), true);
		(void)seeded;
		const int count = static_cast<int>(m_creditTextures.size());
		// min - round(rand() * (max - min) * -1/32767), MSVC's 15-bit rand().
		const int index = static_cast<int>(std::lround((std::rand() & 0x7fff) * (count - 1) / 32767.0));
		return Value(index >= 0 && index < count ? m_creditTextures[static_cast<size_t>(index)] : std::string());
	});

	// --- Profile's equipment (MenuTeamManager's kits; no shipped menu
	// calls these). getEquipmentName / getEquipmentDescription (0x1004e670
	// / 0x1004e7a0): the equipment record of that template in any team's
	// kit (0x1004e510, without regard to case), its name (+0x38, else the
	// template +0x1c) or description (+0x54) localized (0x10049770) as a
	// String; an empty String for none. setSelectedEquipment /
	// setSelectedUnlockEquipment (0x1004af70 / 0x1004afa0) keep a template
	// name (0x1012bd48 / 0x1012bcb4) that nothing in SwiffPlayer reads.
	// getSprint(team, kit) (0x1004e410): 1 - the kit's
	// sprintStaminaDissipationFactor (MenuTeamManager slot 0x2c's team,
	// +0x40 its kits, +0x58).
	n.setFunction(kProfile, "getEquipmentName", [this](Interpreter& vm, const Args& args) {
		const MenuEquipment* e = findEquipment(vm.toString(arg(args, 0)));
		return Value(e ? m_natives.localize(e->name.empty() ? e->templateName : e->name) : std::string());
	});
	n.setFunction(kProfile, "getEquipmentDescription", [this](Interpreter& vm, const Args& args) {
		const MenuEquipment* e = findEquipment(vm.toString(arg(args, 0)));
		return Value(e ? m_natives.localize(e->description) : std::string());
	});
	n.setFunction(kProfile, "setSelectedEquipment", [this](Interpreter& vm, const Args& args) {
		m_selectedEquipment = vm.toString(arg(args, 0));
		return Value();
	});
	n.setFunction(kProfile, "setSelectedUnlockEquipment", [this](Interpreter& vm, const Args& args) {
		m_selectedUnlockEquipment = vm.toString(arg(args, 0));
		return Value();
	});
	n.setFunction(kProfile, "getSprint", [this](Interpreter& vm, const Args& args) {
		findEquipment(std::string()); // reads the kits once
		const int team = intArg(vm, args, 0), kit = intArg(vm, args, 1);
		if (team < 0 || team >= static_cast<int>(m_menuKits.size()) || kit < 0
			|| kit >= static_cast<int>(m_menuKits[static_cast<size_t>(team)].size())) {
			return Value(); // the original reads past its records
		}
		return Value(1.0 - m_menuKits[static_cast<size_t>(team)][static_cast<size_t>(kit)].sprintStaminaDissipationFactor);
	});

	// --- The hardware probes (no shipped menu calls these), answered as the
	// original does on hardware it supports.
	// VideoSettings.detectVideoScheme (0x1005c4c0) is VideoSettings slot
	// 0x2c (0x00497090): the renderer's adapter object slot 0x40 (RendDX9
	// 0x1000fa50), 1 without a renderer. That rates the machine 0 low,
	// 1 medium, 2 high: by the CPU (below 2350 or under 1 GB of memory 0,
	// below 2950 1, else 2), 0 for pixel shader 1.x, then capped by the
	// card's row in the NVIDIA (0x101f35a0) and ATI (0x101f29e0) tables (a
	// Radeon 9600 SE 0); a card in neither stays at 1 unless it is an ATI
	// card with shader model 3. A current machine with a listed card rates
	// 2; the rewrite has no adapter tables and answers that.
	n.setFunction(kVideoSettings, "detectVideoScheme", [](Interpreter&, const Args&) { return Value(2); });
	// isRunningWithUnsupportedDrivers (0x1005d0d0, SwiffHost slot 0x35c
	// 0x0046c220): SwiffRender's driver version (slots 0xe8 / 0xe4) below
	// 0xa1e0b. False with a current driver, and the rewrite has no Direct3D
	// driver to ask.
	n.setFunction(kVideoSettings, "isRunningWithUnsupportedDrivers", [](Interpreter&, const Args&) { return Value(false); });
	// AudioSettings.isInCriticalState (0x10010750): AudioSettings slot 0x34
	// (0x006da520, +8) is 1, which applying the settings (0x00496830) sets
	// when the sound engine's provider fails to open (noprovider,
	// noproviderdll, openaldenied, openalerror; 0 when it opens). False with
	// a working sound device.
	n.setFunction(kAudioSettings, "isInCriticalState", [](Interpreter&, const Args&) { return Value(false); });

	// --- dice.bf2.Logic's small ones.
	// 0x1003b7b0 / 0x1003b930: SwiffHost slots 0xd8 / 0xd4 (0x0046adc0 /
	// 0x0046adb0), why the menu came up (0 started normally).
	n.setFunction(kLogic, "setActivateReason", [this](Interpreter& vm, const Args& args) {
		m_activateReason = intArg(vm, args, 0);
		return Value();
	});
	n.setFunction(kLogic, "getActivateReason", [this](Interpreter&, const Args&) { return Value(m_activateReason); });
	// 0x1003b810 / 0x1003b9b0: slots 0xe0 / 0xdc (0x0046afe0 / 0x0046afc0),
	// the page to come back to.
	n.setFunction(kLogic, "setFrameLabel", [this](Interpreter& vm, const Args& args) {
		m_frameLabel = vm.toString(arg(args, 0));
		return Value();
	});
	n.setFunction(kLogic, "getFrameLabel", [this](Interpreter&, const Args&) { return Value(m_frameLabel); });
	// 0x1003bf40 / 0x1003b110: slots 0x2e8 / 0x2ec (0x0046be30 /
	// 0x0046be60), the cursor while a page waits.
	n.setFunction(kLogic, "beginWaitState", [this](Interpreter& vm, const Args& args) {
		m_waitCursor = intArg(vm, args, 0);
		return Value();
	});
	n.setFunction(kLogic, "endWaitState", [this](Interpreter&, const Args&) {
		m_waitCursor = 0;
		return Value();
	});
	// 0x1003c6a0: setDecimals(value, n) is the value as "%.<n>f" (a float).
	n.setFunction(kLogic, "setDecimals", [](Interpreter& vm, const Args& args) {
		const float value = static_cast<float>(vm.toNumber(arg(args, 0)));
		char text[64];
		std::snprintf(text, sizeof text, "%.*f", std::clamp(intArg(vm, args, 1), 0, 20), static_cast<double>(value));
		return Value(text);
	});

	// 0x1003c300 / 0x1003be00: the player's own flag (SwiffPlayer's player
	// object slots 0x80 / 0x84, +0x48, false at 0x10009c00). The lists set
	// it when flushed and read it while they update; nothing else does.
	n.setFunction(kLogic, "getAsync", [this](Interpreter&, const Args&) { return Value(m_async); });
	n.setFunction(kLogic, "setAsync", [this](Interpreter&, const Args& args) {
		m_async = avm1::toBoolean(arg(args, 0));
		return Value();
	});
	// 0x1003bfa0 / 0x1003c9e0: SwiffHost slots 0xe8 / 0xe4 (0x0046b250 /
	// 0x0046b240), the loading flag +0x7b4 (the loading screen's own
	// bindings answer it while a level loads).
	n.setFunction(kLogic, "setIsLoading", [this](Interpreter&, const Args& args) {
		m_isLoading = avm1::toBoolean(arg(args, 0));
		return Value();
	});
	n.setFunction(kLogic, "getIsLoading", [this](Interpreter&, const Args&) { return Value(m_isLoading); });
	// 0x1003c540: GetKeyState(VK_CAPITAL) & 1, the Caps Lock light.
	n.setFunction(kLogic, "isCapsLockPressed", [this](Interpreter&, const Args&) { return Value(capsLock && capsLock()); });
	// 0x10049930: a bare "ret 0xc".
	n.setFunction(kProfile, "deleteCurrentProfile", [](Interpreter&, const Args&) { return Value(); });
	// 0x10016390: ControlSettings slot 0x24 (the game's controls again:
	// Settings/Controls.con), then the current profile saved
	// (LocalProfileManager slots 0x38 / 0x34).
	n.setFunction(kControlSettings, "resetDefault", [this](Interpreter&, const Args&) {
		if (m_profiles && m_profiles->onResetControls) {
			m_profiles->onResetControls();
		}
		if (m_profiles && m_profiles->current() >= 0) {
			m_profiles->save();
		}
		return Value();
	});

	// --- The settings objects.
	// 0x1002fec0: GeneralSettings slot 0x24 (0x0049bdf0), the HUD page's
	// values: transparencies 255, the crosshair white, the mini-map
	// rotating at 170, help pop-ups and camera shake on, colour-blind off;
	// then the profile saved (LocalProfileManager slots 0x38 / 0x34).
	n.setFunction(kGeneralSettings, "resetHud", [this](Interpreter&, const Args&) {
		resetGeneralValues({{"GeneralSettings.HUDTransparency", Value(255)}, {"GeneralSettings.MapIconTransparency", Value(255)},
			{"GeneralSettings.CrosshairColor", Value("255 255 255 255")}, {"GeneralSettings.MinimapTransparency", Value(170)},
			{"GeneralSettings.HelpPopUps", Value(1)}, {"GeneralSettings.MinimapRotate", Value(true)},
			{"GeneralSettings.CameraShake", Value(1)}, {"GeneralSettings.ColorBlindFriendly", Value(0)}});
		return Value();
	});
	// 0x1002ff40: slot 0x28 (0x0049a170): out of voting and ignoring buddy
	// requests off, the item selection reversed and auto reload on, the
	// connection type 5.
	n.setFunction(kGeneralSettings, "resetGeneral", [this](Interpreter&, const Args&) {
		resetGeneralValues({{"GeneralSettings.OutOfVoting", Value(false)}, {"GeneralSettings.IgnoreBuddyRequests", Value(0)},
			{"GeneralSettings.ItemSelectionReverseItems", Value(true)}, {"GeneralSettings.ConnectionType", Value(5)},
			{"GeneralSettings.AutoReload", Value(true)}});
		return Value();
	});
	// 0x1002ffc0: slot 0x2c (0x0049a1a0): the kill, radio and chat message
	// filters 1.
	n.setFunction(kGeneralSettings, "resetMessage", [this](Interpreter&, const Args&) {
		resetGeneralValues({{"GeneralSettings.KillMessagesFilter", Value(1)}, {"GeneralSettings.RadioMessagesFilter", Value(1)},
			{"GeneralSettings.ChatMessagesFilter", Value(1)}});
		return Value();
	});
	// 0x10032900: GeneralSettings slot 0x178 (0x0049b4f0): true when help
	// pop-ups are off or the voice-over help was played
	// (setPlayedVOHelp's list, matched without regard to case).
	n.setFunction(kGeneralSettings, "checkPlayedVOHelp", [this](Interpreter& vm, const Args& args) {
		const auto popups = m_settings.find("GeneralSettings.HelpPopUps");
		if (popups != m_settings.end() && vm.toNumber(popups->second) == 0) {
			return Value(true);
		}
		const auto lower = [](std::string text) {
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		};
		std::string name = lower(vm.toString(arg(args, 0)));
		if (m_objects) {
			for (std::string played : m_objects->list("GeneralSettings.PlayedVOHelp")) {
				played.erase(std::remove(played.begin(), played.end(), '"'), played.end());
				if (lower(played) == name) {
					return Value(true);
				}
			}
		}
		return Value(false);
	});
	// 0x1004c580: the current local profile's count (LocalProfileManager
	// slot 0x9c; LocalProfile.setNumTimesLoggedIn), 0 without one.
	n.setFunction("dice.bf2.Profile", "getNumTimesLoggedIn", [this](Interpreter&, const Args&) {
		return Value(m_objects ? m_objects->number("LocalProfile.NumTimesLoggedIn", 0) : 0.0);
	});
	// 0x1005d240: VideoSettings slot 0x3c, set when a change waits for a
	// restart; the rewrite applies the video page at once.
	n.setFunction(kVideoSettings, "needsReboot", [](Interpreter&, const Args&) { return Value(false); });
	// 0x10011130 / 0x100111c0: AudioSettings slots 0x2c / 0x30, the sound
	// card's EAX and Creative Zenith checks: OpenAL Soft has neither.
	n.setFunction(kAudioSettings, "hasEax", [](Interpreter&, const Args&) { return Value(false); });
	n.setFunction(kAudioSettings, "hasZenithBoard", [](Interpreter&, const Args&) { return Value(false); });
}

// --- The command line

int MenuBindings::commandLineActivation(const CommandLine& commandLine, bool internet)
{
	// 0x00404780: GSIsServer picks hosting, 4 with the server settings'
	// internet flag (+0x54) and 5 without; a joining client 3 for an
	// address on the local network (slot 0x288), 2 otherwise.
	if (commandLine.hostServer) {
		return internet ? 4 : 5;
	}
	if (commandLine.joinAddress.empty()) {
		return 0;
	}
	return privateAddress(commandLine.joinAddress) ? 3 : 2;
}

void MenuBindings::setCommandLine(const CommandLine& commandLine)
{
	m_commandLine = commandLine;
	const auto internet = m_settings.find("ServerSettings.Internet");
	m_commandLineType = commandLineActivation(commandLine, internet != m_settings.end() && avm1::toBoolean(internet->second));
}

void MenuBindings::bindCommandLine()
{
	ui::NativeRegistry& n = m_natives;
	// 0x1003c5a0 / 0x1003bee0: SwiffHost slots 0x2f4 / 0x2f8 (+0xd94), the
	// activation type. The main page asks for it as "getCommandline"; the
	// natives' names, like the movies' members, match without regard to
	// case (NativeRegistry::memberNamed).
	n.setFunction(kLogic, "getCommandLine", [this](Interpreter&, const Args&) { return Value(m_commandLineType); });
	n.setFunction(kLogic, "setCommandLine", [this](Interpreter& vm, const Args& args) {
		m_commandLineType = intArg(vm, args, 0);
		return Value();
	});
	// 0x1003c870 / 0x1003c8f0: slots 0xf4 / 0xf8 (0x0046b610 / 0x0046b6c0),
	// GSJoinAddress and GSPassword ("" unset).
	n.setFunction(kLogic, "getCommandlineServer", [this](Interpreter&, const Args&) { return Value(m_commandLine.joinAddress); });
	n.setFunction(kLogic, "getCommandlineServerPassword", [this](Interpreter&, const Args&) {
		return Value(m_commandLine.password);
	});
	// 0x1003c7a0: slot 0x124 (joinServer) with GSJoinAddress and slot 0xfc's
	// GSPort (0x0046b670, 17567 unset); its answer.
	n.setFunction(kLogic, "joinCommandlineServer", [this](Interpreter&, const Args&) {
		if (!m_commandLine.password.empty()) {
			m_joinPassword = m_commandLine.password;
		}
		return Value(joinServer(m_commandLine.joinAddress, m_commandLine.port));
	});
	// 0x1003b130: slot 0x140 (0x00469f90) gives the server settings' port
	// and query port; the info server becomes 127.0.0.1 on them (slot
	// 0x130) and slot 0x128 creates the server: the command line's level
	// when it names one (MNCommandLine, GSLoadLevel and GSGameMode,
	// cleared after), else the map list. Then the arrow cursor and
	// endWaitState (slot 0x2ec).
	n.setFunction(kLogic, "startCommandlineServer", [this](Interpreter&, const Args&) {
		m_currentServerHost = "127.0.0.1";
		m_currentServerPort = static_cast<int>(setting("ServerSettings.Port", 0x449f));
		m_currentServerQueryPort = static_cast<int>(setting("ServerSettings.GameSpyPort", 29900));
		if (!m_commandLine.level.empty()) {
			// 0x00431690: the size is the server's players, 16 at least on a
			// ranked server.
			m_mapList = {{m_commandLine.level, m_commandLine.gameMode, static_cast<int>(setting("ServerSettings.MaxPlayers", 64))}};
			m_commandLine.level.clear();
			showMapLists();
		}
		createServer();
		m_waitCursor = 0;
		return Value();
	});
}

// --- Local demos and the bookmarks

std::vector<MenuBindings::LocalDemo> MenuBindings::findDemos(const std::string& directory)
{
	namespace fs = std::filesystem;
	std::vector<LocalDemo> demos;
	std::error_code error;
	std::vector<fs::path> files;
	for (const auto& entry : fs::directory_iterator(directory, error)) {
		if (entry.is_regular_file(error) && entry.path().extension() == ".bf2142demo") {
			files.push_back(entry.path());
		}
	}
	std::sort(files.begin(), files.end());
	for (const fs::path& file : files) {
		net::DemoHeader header;
		std::vector<net::DemoRecord> records;
		if (!net::readDemo(file.string(), header, records)) {
			continue; // 0x005793b0 refused the header
		}
		LocalDemo demo;
		demo.path = file.string();
		demo.name = file.stem().string(); // 0x0065bb10
		demo.serverName = header.serverName;
		demo.started = header.started;
		demo.level = header.level;
		demo.gameMode = header.gameMode;
		demos.push_back(demo);
	}
	return demos;
}

void MenuBindings::setDemoDirectory(const std::string& directory)
{
	m_demoDirectory = directory;
	m_demos = directory.empty() ? std::vector<LocalDemo>() : findDemos(directory);
	showDemos();
}

void MenuBindings::showDemos()
{
	// The "demos" list (0x10064930 counts the library through slot 0x18;
	// 0x10064a50 writes each row): cell0 the date (0x10007aa0), cell1 the
	// file's name, cell2 the map's name (ModInfo slot 0x38), cell3 the
	// mode's TEXT_<mode>, else the mod's name of it, else the mode.
	ui::NativeRegistry::Rows rows;
	for (const LocalDemo& demo : m_demos) {
		std::string mode = m_natives.localize("TEXT_" + demo.gameMode);
		if (mode == "TEXT_" + demo.gameMode) {
			const std::string name = gameModeName ? gameModeName(demo.gameMode) : std::string("N/A");
			mode = name == "N/A" || name.empty() ? demo.gameMode : name;
		}
		rows.push_back({demoDate(demo.started), demo.name, underscoresAsSpaces(demo.level), mode});
	}
	m_natives.setListRows("demos", rows);
}

std::vector<std::string> MenuBindings::demoBookmarks() const
{
	return m_objects ? m_objects->list(kBookmarkList) : std::vector<std::string>();
}

void MenuBindings::showDemoBookmarks()
{
	// The "demobm" list (0x100643f0 counts through LocalProfileManager slot
	// 0xd4, 0x10064510 writes a row from slot 0xd8's record): cell0 the
	// date (+0x54, 0x10007b70: no time), cell1 the first text (+0x04), cell2 the map's name of
	// the second (+0x1c). The entries are addDemoBookmark's "qqqi".
	ui::NativeRegistry::Rows rows;
	for (const std::string& entry : demoBookmarks()) {
		const std::vector<std::string> words = splitArguments(entry);
		const auto word = [&words](size_t i) { return i < words.size() ? words[i] : std::string(); };
		rows.push_back({bookmarkDate(static_cast<uint32_t>(std::strtoul(word(3).c_str(), nullptr, 10))), word(0),
			underscoresAsSpaces(word(1))});
	}
	m_natives.setListRows("demobm", rows);
}

void MenuBindings::bindDemos()
{
	ui::NativeRegistry& n = m_natives;
	// 0x10046f40: the library's record (slot 0x1c); its file goes to host
	// event 0xd (SwiffHost slot 0x30). The arrow cursor and endWaitState
	// either way.
	n.setFunction(kMultiplay, "playDemo", [this](Interpreter& vm, const Args& args) {
		const int index = intArg(vm, args, 0);
		if (index >= 0 && index < static_cast<int>(m_demos.size())) {
			log("play demo " + m_demos[static_cast<size_t>(index)].path);
			if (onPlayDemo) {
				onPlayDemo(m_demos[static_cast<size_t>(index)].path);
			}
		}
		m_waitCursor = 0;
		return Value();
	});
	// 0x100421c0: slot 0x20 (0x0049dde0): the file deleted (FileSystem slot
	// 0x20) and the record removed.
	n.setFunction(kMultiplay, "deleteDemo", [this](Interpreter& vm, const Args& args) {
		const int index = intArg(vm, args, 0);
		if (index >= 0 && index < static_cast<int>(m_demos.size())) {
			std::error_code error;
			std::filesystem::remove(m_demos[static_cast<size_t>(index)].path, error);
			m_demos.erase(m_demos.begin() + index);
			showDemos();
		}
		return Value();
	});
	// 0x10043940: LocalProfileManager slot 0xd4, the bookmarks' count.
	n.setFunction(kMultiplay, "getNumDemoBookmarks", [this](Interpreter&, const Args&) {
		return Value(static_cast<double>(demoBookmarks().size()));
	});
	// 0x10043dc0: slot 0xdc (the bookmark removed), then the profile saved
	// (slot 0x34) when it was; its answer. The arrow cursor and
	// endWaitState.
	n.setFunction(kMultiplay, "deleteDemoBookmark", [this](Interpreter& vm, const Args& args) {
		const int index = intArg(vm, args, 0);
		std::vector<std::string> entries = demoBookmarks();
		const bool removed = m_objects && index >= 0 && index < static_cast<int>(entries.size());
		if (removed) {
			entries.erase(entries.begin() + index);
			m_objects->setList(kBookmarkList, entries);
			if (m_profiles) {
				m_profiles->save();
			}
			showDemoBookmarks();
		}
		m_waitCursor = 0;
		return Value(removed);
	});
	// 0x10042bd0 / 0x10041b70: the library's flag (slots 0x40 / 0x44, +0xb0).
	n.setFunction(kMultiplay, "getHasNewBookmark", [this](Interpreter&, const Args&) { return Value(m_hasNewBookmark); });
	n.setFunction(kMultiplay, "setHasNewBookmark", [this](Interpreter&, const Args& args) {
		m_hasNewBookmark = avm1::toBoolean(arg(args, 0));
		return Value();
	});
}

// --- The map texts and the server info popup

const MenuBindings::LevelText* MenuBindings::levelText(const std::string& level) const
{
	auto it = m_levelTexts.find(level);
	if (it == m_levelTexts.end()) {
		std::optional<LevelText> text;
		std::string xml;
		if (readFile && !level.empty() && readFile("Levels/" + level + "/Info/" + level + ".desc", xml)) {
			// 0x00462750: <name> (+0x1c), <briefing> (+0x38) and its locid
			// (+0x54).
			const LevelDescription desc = parseLevelDescription(xml, "", 0);
			text = LevelText {desc.name, desc.briefing, desc.briefingKey, desc.modes};
		}
		it = m_levelTexts.emplace(level, text).first;
	}
	return it->second ? &*it->second : nullptr;
}

const MenuBindings::Server* MenuBindings::infoServer() const
{
	// 0x10053860: in a game the joined server (the "serverInfo" browser's,
	// slot 0x44), otherwise the one SwiffHost +0xd08 / +0xd24 name (slots
	// 0x290 / 0x298, set with slot 0x130 by the popup's lists, 0x10072f90);
	// none for "0.0.0.0" or port 0.
	const std::string& host = m_gameRunning && !m_joinHost.empty() ? m_joinHost : m_infoHost;
	if (host.empty() || host == "0.0.0.0") {
		return nullptr;
	}
	for (const auto* servers : {&m_servers, &m_lanServers, &m_favouriteServers}) {
		for (const Server& server : *servers) {
			if (server.entry.ip == host) {
				return &server;
			}
		}
	}
	return nullptr;
}

// The server info popup's rules (the "serverExtendedInfo" list, 0x10068150;
// rows drawn by 0x10068380): eight rows, each a localized label (cell0, the
// table 0x1012c354 the first list fills) and the server record's value
// (cell1). The record is the browser's (SwiffHost's "serverInfo" object,
// slot 0x3c with the address and game port), its fields read from the
// server's query keys by 0x004661a0. Without the server only the labels
// show.
int MenuBindings::serverInfoRows(const std::string& host, int port)
{
	static const char* const kLabels[] = {"SERVERINFO_friendlyfire", "SERVERINFO_roundspermap", "SERVERINFO_spawntime",
		"SERVERINFO_timelimit", "SERVERINFO_ticketratio", "SERVERINFO_teamratio", "SERVERINFO_autobalance",
		"HEADER_allowspectator"};
	const Server* found = nullptr;
	for (const auto* servers : {&m_servers, &m_lanServers, &m_favouriteServers}) {
		for (const Server& server : *servers) {
			if (!found && server.entry.ip == host && (port <= 0 || server.gamePort == port)) {
				found = &server;
			}
		}
	}
	const auto key = [&](const char* name) -> std::string {
		if (!found) {
			return {};
		}
		const auto it = found->entry.fields.find(name);
		return it == found->entry.fields.end() ? std::string() : it->second;
	};
	const auto number = [&](const char* name) { return std::atoi(key(name).c_str()); };
	// The two float fields (spawn time +0x1ac, team ratio +0x20c), rounded
	// as ROUND() does (to nearest).
	const auto rounded = [&](const char* name) { return static_cast<int>(std::lround(std::atof(key(name).c_str()))); };
	const auto onOff = [this](bool on) { return m_natives.localize(on ? "HEADER_on" : "HEADER_off"); };
	const std::string values[] = {
		onOff(number("bf2142_friendlyfire") != 0),         // +0xfc
		std::to_string(number("roundtime")),               // +0x1b4 (rounds per map)
		std::to_string(rounded("bf2142_spawntime")),       // +0x1ac
		std::to_string(number("timelimit") / 60),          // +0x1b0, seconds, shown in minutes
		std::to_string(number("bf2142_ticketratio")),      // +0x210
		std::to_string(rounded("bf2142_teamratio")),       // +0x20c
		onOff(number("bf2142_autobalanced") == 1),         // +0xfd
		onOff(number("bf2142_allow_spectators") == 1),     // +0x228
	};
	ui::NativeRegistry::Rows rows;
	for (size_t i = 0; i < std::size(kLabels); ++i) {
		rows.push_back({m_natives.localize(kLabels[i]), found ? values[i] : std::string()});
	}
	m_natives.setListRows("serverExtendedInfo", rows);
	return found ? static_cast<int>(rows.size()) : 0;
}

// 0x00492dc0: a kit file's lines, split at spaces, tabs and quotes.
MenuBindings::MenuKit MenuBindings::parseMenuKit(const std::string& text,
	const std::function<bool(const std::string&, std::string&)>& readFile)
{
	// A Weapon.desc element's locid, else its text ("" without the element).
	const auto element = [](const std::string& xml, const std::string& tag) {
		std::string lower = xml;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		const size_t open = lower.find("<" + tag);
		if (open == std::string::npos) {
			return std::string();
		}
		const size_t close = lower.find('>', open);
		if (close == std::string::npos) {
			return std::string();
		}
		if (const size_t at = lower.find("locid=\"", open); at != std::string::npos && at < close) {
			const size_t end = xml.find('"', at + 7);
			if (end != std::string::npos && end > at + 7) {
				return xml.substr(at + 7, end - at - 7);
			}
		}
		const size_t end = lower.find("</" + tag, close);
		return end == std::string::npos ? std::string() : xml.substr(close + 1, end - close - 1);
	};
	MenuKit kit;
	std::istringstream lines(text);
	for (std::string line; std::getline(lines, line);) {
		std::vector<std::string> words;
		std::string word;
		for (const char c : line) {
			if (c == ' ' || c == '\t' || c == '"' || c == '\r') {
				if (!word.empty()) {
					words.push_back(word);
					word.clear();
				}
			} else {
				word += c;
			}
		}
		if (!word.empty()) {
			words.push_back(word);
		}
		if (words.size() < 2) {
			continue;
		}
		if (words[0] == "ObjectTemplate.addTemplate") {
			MenuEquipment e;
			e.templateName = e.name = words[1];
			std::string desc;
			if (readFile && readFile("objects/weapons/handheld/" + words[1] + "/Weapon.desc", desc)) {
				if (std::string name = element(desc, "name"); !name.empty()) {
					e.name = name;
				}
				e.description = element(desc, "description");
			}
			kit.equipment.push_back(std::move(e));
		} else if (words[0] == "ObjectTemplate.create" && words.size() >= 3 && lowerText(words[1]) == "kit") {
			kit.name = words[2];
		} else if (words[0] == "ObjectTemplate.sprintStaminaDissipationFactor") {
			kit.sprintStaminaDissipationFactor = static_cast<float>(std::atof(words[1].c_str()));
		}
	}
	return kit;
}

// 0x1004e510: the teams in order, each kit, each record, the template
// compared without regard to case. The kits are read on first use, as
// addKit (0x00494da0) read them: <team folder>/<kit file>.
const MenuBindings::MenuEquipment* MenuBindings::findEquipment(const std::string& templateName)
{
	if (!m_menuKitsRead && menuHost) {
		m_menuKitsRead = true;
		for (const MenuHost::Team& team : menuHost->teams) {
			std::vector<MenuKit> kits;
			for (const std::string& file : team.kits) {
				std::string text;
				if (readFile && readFile(team.path + "/" + file, text)) {
					kits.push_back(parseMenuKit(text, readFile));
				}
			}
			m_menuKits.push_back(std::move(kits));
		}
	}
	const std::string wanted = lowerText(templateName);
	for (const auto& kits : m_menuKits) {
		for (const MenuKit& kit : kits) {
			for (const MenuEquipment& e : kit.equipment) {
				if (lowerText(e.templateName) == wanted) {
					return &e;
				}
			}
		}
	}
	return nullptr;
}

void MenuBindings::bindMapTexts()
{
	ui::NativeRegistry& n = m_natives;
	// The level record's description: its briefing's locid (+0x54)
	// localized (SwiffHost slot 0xbc), else the briefing's text (+0x38); ""
	// for no such level.
	const auto description = [this](const LevelText* text) {
		if (!text) {
			return std::string();
		}
		return text->briefingKey.empty() ? m_natives.localize(text->briefing) : m_natives.localize(text->briefingKey);
	};
	// The levels the menu knows, once each.
	const auto levels = [this] {
		std::vector<std::string> out;
		for (const MenuMap& map : m_maps) {
			if (std::find(out.begin(), out.end(), map.level) == out.end()) {
				out.push_back(map.level);
			}
		}
		return out;
	};
	// 0x10045be0: the level whose folder (+0x00) is the text up to its
	// first space, in the current mod (SwiffHost slot 0x144, ModInfo slot
	// 0x1c's levels +0x100).
	n.setFunction(kMultiplay, "getMapDescriptionFromId", [this, description](Interpreter& vm, const Args& args) {
		std::string id = vm.toString(arg(args, 0));
		id = id.substr(0, id.find(' '));
		return Value(description(levelText(id)));
	});
	// 0x10045ef0: the level whose shown name (+0x1c) is the first word.
	n.setFunction(kMultiplay, "getMapDescriptionFromText", [this, description, levels](Interpreter& vm, const Args& args) {
		std::istringstream words(vm.toString(arg(args, 0)));
		std::string name;
		words >> name;
		for (const std::string& level : levels()) {
			const LevelText* text = levelText(level);
			if (text && text->name == name) {
				return Value(description(text));
			}
		}
		return Value("");
	});
	// 0x10046440: every mod's levels (ModInfo slot 0x14) for the one whose
	// shown name is the argument: its folder; the argument itself otherwise.
	n.setFunction(kMultiplay, "getMapPathFromName", [this, levels](Interpreter& vm, const Args& args) {
		const std::string name = vm.toString(arg(args, 0));
		for (const std::string& level : levels()) {
			const LevelText* text = levelText(level);
			if (text && text->name == name) {
				return Value(level);
			}
		}
		return Value(name);
	});
	// 0x100437f0 calls ModInfo slot 0x44 (0x00461740, addGameMode's
	// setter) with (id, "") and answers that "": an original bug that also
	// blanks the mode's name in the mod's map. The rewrite answers "" and
	// leaves the name.
	n.setFunction(kMultiplay, "getGameModeFromId", [](Interpreter&, const Args&) { return Value(""); });
	// 0x100452f0 / 0x100451a0: the current mod's first game mode (its list
	// +0x110, which the level scan 0x00462750 fills with each <mode type>
	// not yet in it) by its name (ModInfo slot 0x48); 0x10045430 the id
	// itself; "" without one.
	const auto firstMode = [this, levels] {
		std::vector<std::string> sorted = levels();
		std::sort(sorted.begin(), sorted.end());
		for (const std::string& level : sorted) {
			const LevelText* text = levelText(level);
			if (text && !text->modes.empty()) {
				return text->modes.front();
			}
		}
		return std::string();
	};
	n.setFunction(kMultiplay, "getSelectedFilterGameModeId", [firstMode](Interpreter&, const Args&) { return Value(firstMode()); });
	for (const char* member : {"getSelectedFilterGameMode", "getSelectedServerFilterGameMode"}) {
		n.setFunction(kMultiplay, member, [this, firstMode](Interpreter&, const Args&) {
			const std::string mode = firstMode();
			if (mode.empty()) {
				return Value("");
			}
			return Value(gameModeName ? gameModeName(mode) : std::string("N/A"));
		});
	}
	// 0x10042890: the info server's team names (0x1012bc3c + team * 0x1c),
	// which General.cacheExtendedServerInfo (0x10026770, online) sets; ""
	// until then.
	n.setFunction(kMultiplay, "getTeamName", [this](Interpreter& vm, const Args& args) {
		const int team = intArg(vm, args, 0);
		return Value(team >= 0 && team < 2 ? m_teamNames[static_cast<size_t>(team)] : std::string());
	});
	// 0x10041d70: "ip port queryPort" to SwiffHost slot 0x130 (0x00469d60:
	// kept, and the "serverInfo" browser asks that server), then both info
	// revisions (slots 0x138 / 0x13c) counted up. 0x10041530: "0.0.0.0", 0, 0.
	n.setFunction(kMultiplay, "setCurrentServerId", [this](Interpreter& vm, const Args& args) {
		std::string ip;
		int port = 0, queryPort = 0;
		parseAddress(vm.toString(arg(args, 0)), ip, port, queryPort);
		m_currentServerHost = ip;
		m_currentServerPort = port;
		m_currentServerQueryPort = queryPort;
		++m_serverInfoRevision[0];
		++m_serverInfoRevision[1];
		return Value();
	});
	n.setFunction(kMultiplay, "clearServerId", [this](Interpreter&, const Args&) {
		m_currentServerHost = "0.0.0.0";
		m_currentServerPort = m_currentServerQueryPort = 0;
		return Value();
	});

	// --- dice.bf2.ServerInfo's map texts (0x10053c20 ...): the info
	// server's record (0x10053860): getMapName its map (+0xa0),
	// getMaxPlayers its players (+0x68); getGameModeTitle,
	// getGameModeDescription, getBackgroundTitle and
	// getBackgroundDescription all read the same field, its game type
	// (+0x7c). "" and 0 without a server.
	const auto text = [this](const char* name) {
		return [this, name](Interpreter&, const Args&) {
			const Server* server = infoServer();
			return Value(server ? serverField(server->entry, name) : std::string());
		};
	};
	n.setFunction(kServerInfo, "getMapName", text("mapname"));
	n.setFunction(kServerInfo, "getMaxPlayers", [this](Interpreter&, const Args&) {
		const Server* server = infoServer();
		return Value(server ? std::atoi(serverField(server->entry, "maxplayers").c_str()) : 0);
	});
	for (const char* member : {"getGameModeTitle", "getGameModeDescription", "getBackgroundTitle", "getBackgroundDescription"}) {
		n.setFunction(kServerInfo, member, text("gametype"));
	}
}

// --- The controls page's duplicate prompt

std::string MenuBindings::controlHeader(const std::string& map, const std::string& input, bool negative)
{
	struct Entry {
		int keyId;
		bool negative;
		const char* action; // after "HEADER_"
	};
	// 0x10014ef0 fills it once (0x10016250): per map, (keyId, negative) to
	// the action's HEADER_ text, as the shipped table spells them
	// ("backwark"). Its five maps' names are strings made at start-up; by
	// their actions they are the infantry, land and air maps, the common
	// player map and the game's. The game's keyIds are c_GI numbers.
	struct MapTable {
		const char* map;
		std::vector<Entry> entries;
	};
	static const MapTable kTables[] = {
		{input::ControlMaps::kInfantry, {{3, false, "forward"}, {3, true, "backwark"}, {0, false, "straferight"}, {0, true, "strafeleft"}, {37, false, "prone"}, {9, false, "jump"}, {13, false, "sprint"}, {34, false, "pickupkit"}, {8, false, "fire"}, {31, false, "altfire"}, {45, false, "cycleweapons"}, {14, false, "weapon1"}, {15, false, "weapon2"}, {16, false, "weapon3"}, {17, false, "weapon4"}, {18, false, "weapon5"}, {19, false, "weapon6"}, {20, false, "weapon7"}, {21, false, "weapon8"}, {22, false, "parachute"}, {23, false, "position1"}, {24, false, "position2"}, {25, false, "position3"}, {26, false, "position4"}, {27, false, "position5"}, {28, false, "position6"}, {29, false, "position7"}, {30, false, "position8"}}},
		{input::ControlMaps::kLand, {{3, false, "accelerate"}, {3, true, "decelerate"}, {1, false, "lookdown"}, {1, true, "lookup"}, {0, false, "steerright"}, {0, true, "steerleft"}, {2, false, "turnright"}, {2, true, "turnleft"}, {13, false, "boost"}, {47, false, "countermeasures"}, {38, false, "crouch"}, {8, false, "fire"}, {31, false, "altfire"}, {45, false, "cycleweapons"}, {14, false, "weapon1"}, {15, false, "weapon2"}, {16, false, "weapon3"}, {17, false, "weapon4"}, {18, false, "weapon5"}, {19, false, "weapon6"}, {20, false, "weapon7"}, {21, false, "weapon8"}, {22, false, "weapon9"}, {39, false, "firstperson"}, {40, false, "behind"}, {41, false, "infront"}, {42, false, "static"}, {11, false, "mouselook"}, {23, false, "position1"}, {24, false, "position2"}, {25, false, "position3"}, {26, false, "position4"}, {27, false, "position5"}, {28, false, "position6"}, {29, false, "position7"}, {30, false, "position8"}}},
		{input::ControlMaps::kAir, {{3, false, "accelerate"}, {3, true, "decelerate"}, {0, false, "steerright"}, {0, true, "steerleft"}, {1, false, "pitchforward"}, {1, true, "pitchbackward"}, {2, false, "rollright"}, {2, true, "rollleft"}, {13, false, "afterburner"}, {47, false, "countermeasures"}, {8, false, "fire"}, {31, false, "altfire"}, {45, false, "cycleweapons"}, {14, false, "weapon1"}, {15, false, "weapon2"}, {16, false, "weapon3"}, {17, false, "weapon4"}, {18, false, "weapon5"}, {19, false, "weapon6"}, {20, false, "weapon7"}, {21, false, "weapon8"}, {22, false, "weapon9"}, {39, false, "firstperson"}, {40, false, "behind"}, {41, false, "infront"}, {42, false, "static"}, {11, false, "mouselook"}, {23, false, "position1"}, {24, false, "position2"}, {25, false, "position3"}, {26, false, "position4"}, {27, false, "position5"}, {28, false, "position6"}, {29, false, "position7"}, {30, false, "position8"}}},
		{input::ControlMaps::kPlayer, {{4, false, "turnright"}, {4, true, "turnleft"}, {5, false, "lookdown"}, {5, true, "lookup"}, {10, false, "enterexit"}, {32, false, "reload"}, {35, false, "cyclecameras"}, {38, false, "crouch"}, {48, false, "nextweaponzoomout"}, {49, false, "previousweaponzoomin"}}},
		{input::ControlMaps::kGame, {{61, false, "pushtotalk"}, {22, false, "sayall"}, {23, false, "sayteam"}, {24, false, "saysquad"}, {34, false, "mainradio"}, {33, false, "secradio"}, {56, false, "comchannel"}, {47, false, "voteyes"}, {48, false, "voteno"}, {50, false, "createsquad"}, {36, false, "zoommap"}, {35, false, "map"}, {55, false, "scoreboard"}, {29, false, "spawnscreen"}, {60, false, "squadscreen"}, {19, false, "pause"}, {18, false, "screenshot"}, {57, false, "3dmap"}, {1, false, "console"}}},
	};
	const bool game = map == input::ControlMaps::kGame;
	const int keyId = input::inputNumber(input);
	for (const MapTable& table : kTables) {
		if (map != table.map || keyId < 0 || input.compare(0, 4, game ? "c_GI" : "c_PI") != 0) {
			continue;
		}
		for (const Entry& e : table.entries) {
			if (e.keyId == keyId && e.negative == negative) {
				return std::string("HEADER_") + e.action;
			}
		}
	}
	return std::string();
}

void MenuBindings::bindPendingInput()
{
	if (!m_controls || !m_pendingInput) {
		return;
	}
	const PendingInput p = *m_pendingInput;
	m_pendingInput.reset();
	if (!p.duplicateInput.empty()) {
		// The key off the input that had it (ControlMaps::create adds to
		// the map; the map the console builds next is kept).
		const std::string current = m_controls->currentName();
		input::ControlMaps::Map& map = m_controls->create(p.duplicateMap);
		if (const auto it = map.find(p.duplicateInput); it != map.end()) {
			auto& bindings = it->second;
			bindings.erase(std::remove_if(bindings.begin(), bindings.end(),
							   [&](const input::Binding& b) { return b.device == p.device && b.control == p.control; }),
				bindings.end());
		}
		if (!current.empty()) {
			m_controls->setActive(current);
		}
	}
	m_controls->bind(p.map, p.input, p.device, p.control, p.slot, p.sign);
	log("controls: " + p.input + " on " + p.device + " " + p.control);
}

} // namespace dice::hfe::menu
