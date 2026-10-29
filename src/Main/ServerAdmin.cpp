#include "ServerAdmin.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace dice::hfe::host {

namespace {

std::string lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
	return text;
}

bool readText(const std::string& path, std::string& text)
{
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return false;
	}
	std::ostringstream all;
	all << in.rdbuf();
	text = all.str();
	return true;
}

bool writeText(const std::string& path, const std::string& text)
{
	if (path.empty()) {
		return false;
	}
	std::error_code error;
	const std::filesystem::path p(path);
	if (p.has_parent_path()) {
		std::filesystem::create_directories(p.parent_path(), error);
	}
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << text;
	return static_cast<bool>(out);
}

bool runFile(Console& console, const std::string& path)
{
	std::string text;
	if (!readText(findFileNoCase(path), text)) {
		return false;
	}
	std::string output;
	console.executeScript(text, output);
	return true;
}

// The menu's GameServerSettings setters and the sv setting each sets
// (0x0041655b..0x00419112 register them).
struct MenuSetter {
	const char* method;
	const char* setting;
};
constexpr MenuSetter kMenuSetters[] = {
	{"setServerName", "serverName"},
	{"setPassword", "password"},
	{"setInternet", "internet"},
	{"setBandwidthChoke", "bandwidthChoke"},
	{"setMaxPlayers", "maxPlayers"},
	{"setStartDelay", "startDelay"},
	{"setEndDelay", "endDelay"},
	{"setSpawnTime", "spawnTime"},
	{"setManDownTime", "manDownTime"},
	{"setTicketRatio", "ticketRatio"},
	{"setRoundsPerMap", "roundsPerMap"},
	{"setTimeLimit", "timeLimit"},
	{"setScoreLimit", "scoreLimit"},
	{"setSoldierFF", "soldierFriendlyFire"},
	{"setVehicleFF", "vehicleFriendlyFire"},
	{"setSoldierSplashFF", "soldierSplashFriendlyFire"},
	{"setVehicleSplashFF", "vehicleSplashFriendlyFire"},
	{"setPunishTeamKills", "tkPunishEnabled"},
	{"setVoteTime", "voteTime"},
	{"setMinPlayersForVoting", "minPlayersForVoting"},
	{"setVoipEnabled", "voipEnabled"},
	{"setVoipQuality", "voipQuality"},
	{"setVoipServerRemote", "voipServerRemote"},
	{"setVoipServerRemoteIP", "voipServerRemoteIP"},
	{"setVoipServerPort", "voipServerPort"},
	{"setVoipBFClientPort", "voipBFClientPort"},
	{"setVoipBFServerPort", "voipBFServerPort"},
	{"setVoipSharedPassword", "voipSharedPassword"},
	{"setGameSpyPort", "gameSpyPort"},
	{"setAllowNATNegotation", "allowNATNegotiation"}, // sic
	{"setInterfaceIP", "interfaceIP"},
	{"setAutoRecord", "autoRecord"},
	{"setTimeBeforeRestartMap", "timeBeforeRestartMap"},
	{"setDemoIndexURL", "demoIndexURL"},
	{"setDemoDownloadURL", "demoDownloadURL"},
	{"setDemoHook", "autoDemoHook"},
	{"setAdminScript", "adminScript"},
	{"setSponsorText", "sponsorText"},
	{"setSponsorLogoURL", "sponsorLogoURL"},
	{"setCommunityLogoURL", "communityLogoURL"},
	{"setSvPunkBuster", "punkBuster"},
	{"setAutoBalanceTeam", "autoBalanceTeam"},
	{"setTeamRatio", "teamRatioPercent"},
};

} // namespace

std::string findFileNoCase(const std::string& path)
{
	std::error_code error;
	if (path.empty() || std::filesystem::exists(path, error)) {
		return path;
	}
	// Walk down from the first part that exists, matching each name without case.
	const std::filesystem::path wanted(path);
	std::filesystem::path have = wanted.root_path();
	for (const std::filesystem::path& part : wanted.relative_path()) {
		const std::filesystem::path exact = have / part;
		if (std::filesystem::exists(exact, error)) {
			have = exact;
			continue;
		}
		bool found = false;
		const std::string name = lower(part.string());
		for (const auto& entry : std::filesystem::directory_iterator(have.empty() ? "." : have, error)) {
			if (lower(entry.path().filename().string()) == name) {
				have = have.empty() ? entry.path().filename() : have / entry.path().filename();
				found = true;
				break;
			}
		}
		if (!found) {
			return path;
		}
	}
	return have.string();
}

void ServerAdmin::setDefaultFiles(const std::string& modDirectory, const std::string& overlay)
{
	// 0x00409950: <GSModDirectory>/Settings/serversettings.con and
	// .../maplist.con, or <GSOverlayPath>Settings/... with an overlay;
	// 0x00507570: <overlay>Settings/banlist.con.
	const std::string settingsDir = overlay.empty() ? modDirectory + "/Settings/" : overlay + "Settings/";
	if (settings.configFile.empty()) {
		settings.configFile = findFileNoCase(settingsDir + "serversettings.con");
	}
	if (maps.configFile.empty()) {
		maps.configFile = findFileNoCase(settingsDir + "maplist.con");
	}
	if (banListFile.empty()) {
		banListFile = findFileNoCase(settingsDir + "banlist.con");
	}
	if (reserved.configFile.empty()) {
		reserved.configFile = findFileNoCase(settingsDir + "reservedslots.con");
	}
}

// sv.load (0x0041b003, 0x0041b056, 0x0041b078) and sv.save (0x0041b0a1, 0x0041b0f4).
bool ServerAdmin::loadSettings(Console& console) { return runFile(console, settings.configFile); }
bool ServerAdmin::loadMapList(Console& console) { return runFile(console, maps.configFile); }
bool ServerAdmin::loadBanList(Console& console) { return runFile(console, banListFile); }
bool ServerAdmin::saveSettings() const { return writeText(settings.configFile, settings.save()); }
bool ServerAdmin::saveMapList() const { return writeText(maps.configFile, maps.save()); }
bool ServerAdmin::saveBanList() const { return writeText(banListFile, bans.save()); }

std::string ServerAdmin::listPlayers() const
{
	// 0x004a86b0
	std::string out;
	if (!hooks.players) {
		return out;
	}
	for (const AdminHooks::Player& p : hooks.players()) {
		char id[16];
		std::snprintf(id, sizeof id, "Id:%3d - ", p.id);
		out += id + p.name;
		if (!p.remote) {
			out += " is local.";
		} else {
			out += " is remote ";
			if (!p.address.empty()) {
				out += "ip: " + p.address;
			}
			if (!p.cdKeyHash.empty()) {
				out += " ->\n         CD-key hash: " + p.cdKeyHash + " ";
			}
		}
		if (p.ai) {
			out += " is an AI bot.";
		}
		out += "\n";
	}
	return out;
}

net::MapListEntry ServerAdmin::nextRound()
{
	bans.endRound();
	const Request asked = std::exchange(request, Request::None);
	switch (asked) {
	case Request::RunLevel:
		roundsOnMap = 0;
		return requestedLevel;
	case Request::Restart:
		roundsOnMap = 0;
		break;
	case Request::NextLevel:
		roundsOnMap = 0;
		maps.advance();
		break;
	case Request::None:
		if (++roundsOnMap >= std::max(settings.roundsPerMap, 1)) {
			roundsOnMap = 0;
			maps.advance();
		}
		break;
	}
	if (maps.count() == 0) {
		return {};
	}
	return maps.entries()[static_cast<size_t>(maps.current())];
}

std::string ServerAdmin::rconInvoke(Console& console, const std::string& command) const
{
	// 0x004427b0: nothing for nothing; a ranked server refuses vars.set;
	// the console's executeString (0x00674430) runs it, line by line, and
	// the output ends with a newline.
	if (command.empty()) {
		return {};
	}
	if (settings.ranked) {
		const size_t start = command.find_first_not_of(" \t\r\n");
		if (start != std::string::npos && lower(command.substr(start, 8)) == "vars.set") {
			return "Ranked server blocks vars.set from remote console\n";
		}
	}
	std::string output;
	std::istringstream lines(command);
	for (std::string line; std::getline(lines, line);) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.empty()) {
			continue;
		}
		std::string result;
		console.execute(line, result, ConsoleAccess::Anyone);
		output += result;
	}
	if (output.empty() || output.back() != '\n') {
		output += "\n";
	}
	return output;
}

void ServerAdmin::registerConsole(Console& console)
{
	constexpr uint32_t access = ConsoleAccess::Anyone;
	ServerAdmin* a = this;

	// --- sv.*
	for (const std::string& name : net::ServerSettings::names()) {
		bindProperty<std::string>(
			console, "sv." + name, access,
			[a, name]() {
				std::string value;
				a->settings.get(name, value);
				return value;
			},
			[a, name](const std::string& value) { a->settings.set(name, value); });
	}
	bindMethod(console, "sv.load", access, [a, &console]() { return a->loadSettings(console); });
	bindMethod(console, "sv.save", access, [a]() { return a->saveSettings(); });

	// --- GameServerSettings.* (the menu's host page)
	for (const MenuSetter& setter : kMenuSetters) {
		const std::string setting = setter.setting;
		bindMethod(console, std::string("GameServerSettings.") + setter.method, access,
			[a, setting](std::string value) { a->settings.set(setting, value); });
	}
	bindMethod(console, "GameServerSettings.setGameMode", access, [a](std::string mode) { a->menuGameMode = mode; });
	bindMethod(console, "GameServerSettings.setAutoRecordRounds", access, [a](int rounds) { a->autoRecordRounds = rounds; });

	// --- maplist.*
	bindMethod(console, "maplist.load", access, [a, &console]() {
		a->maps.clear();
		return a->loadMapList(console);
	});
	bindMethod(console, "maplist.save", access, [a]() { return a->saveMapList(); });
	bindProperty<std::string>(
		console, "maplist.configFile", access, [a]() { return a->maps.configFile; },
		[a](const std::string& file) { a->maps.configFile = file; });
	bindMethod(console, "maplist.mapCount", access, [a]() { return a->maps.count(); });
	bindMethod(console, "maplist.currentMap", access, [a]() { return a->maps.current(); });
	bindMethod(console, "maplist.clear", access, [a]() {
		a->maps.clear();
		return true;
	});
	bindMethod(console, "maplist.remove", access, [a](int index) { return a->maps.remove(index); });
	bindMethod(console, "maplist.list", access, [a]() { return a->maps.list(); });
	bindMethod(
		console, "maplist.append", access,
		[a](std::string level, std::string mode, int size) {
			std::string error;
			const bool ok = a->maps.append({level, mode, size}, &error);
			if (!ok) {
				std::printf("could not add to maplist. %s\n", error.c_str());
			}
			return ok;
		},
		2);
	bindMethod(
		console, "maplist.insert", access,
		[a](int index, std::string level, std::string mode, int size) {
			std::string error;
			const bool ok = a->maps.insert(index, {level, mode, size}, &error);
			if (!ok) {
				std::printf("could not add to maplist. %s\n", error.c_str());
			}
			return ok;
		},
		3);

	// --- reservedSlots.*
	bindMethod(console, "reservedSlots.load", access, [a, &console]() {
		a->reserved.clear();
		return runFile(console, a->reserved.configFile);
	});
	bindMethod(console, "reservedSlots.save", access, [a]() { return writeText(a->reserved.configFile, a->reserved.save()); });
	bindProperty<std::string>(
		console, "reservedSlots.configFile", access, [a]() { return a->reserved.configFile; },
		[a](const std::string& file) { a->reserved.configFile = file; });
	bindMethod(console, "reservedSlots.addNick", access, [a](std::string nick) { a->reserved.addNick(nick); });
	bindMethod(console, "reservedSlots.removeNick", access, [a](std::string nick) { a->reserved.removeNick(nick); });
	bindMethod(console, "reservedSlots.clear", access, [a]() { a->reserved.clear(); });
	bindMethod(console, "reservedSlots.list", access, [a]() { return a->reserved.list(); });

	// --- admin.* (the rows 0x00429bba..0x0042af3b; the ban list is the
	// game server's, 0x00506210..0x00508300: each call below names its
	// functions, and every change to the list saves it, 0x00508100 ->
	// 0x00507570's banlist.con writer)
	bindMethod(console, "admin.runNextLevel", access, [a]() { // 0x00429bba: the game server's slot 0x2c
		if (a->hooks.runNextLevel) {
			a->hooks.runNextLevel();
		}
	});
	bindMethod(console, "admin.currentLevel", access, [a]() { return a->maps.current(); }); // 0x00429c28 -> 0x004baed0
	bindProperty<int>( // 0x00429ca4 -> 0x004bae80
		console, "admin.nextLevel", access, [a]() { return a->maps.next(); }, [a](const int& index) { a->maps.setNext(index); });
	bindMethod(console, "admin.restartMap", access, [a]() { // 0x00429dab -> 0x004ba0c0
		if (a->hooks.restartMap) {
			a->hooks.restartMap();
		}
	});
	bindMethod(console, "admin.runLevel", access, [a](std::string level, std::string mode, int size) { // 0x00429e19 -> 0x004baee0
		if (a->hooks.runLevel) {
			a->hooks.runLevel({level, mode, size});
		}
	});
	const auto findPlayer = [a](const std::string& id, AdminHooks::Player& out) {
		if (!a->hooks.players || id.empty() || id.find_first_not_of("0123456789") != std::string::npos) {
			return false;
		}
		const int wanted = std::atoi(id.c_str());
		for (const AdminHooks::Player& p : a->hooks.players()) {
			if (p.id == wanted) {
				out = p;
				return true;
			}
		}
		return false;
	};
	// 0x00507e10 / 0x00507e80 (from the rows 0x0042a04a / 0x0042a27d, the
	// period parsed by 0x00507f70 / 0x00507fb0): ban the player's address
	// (key), save the list, then the kick.
	bindMethod(
		console, "admin.banPlayer", access,
		[a, findPlayer](std::string id, std::string period) -> std::string {
			AdminHooks::Player p;
			if (!findPlayer(id, p) || !p.remote) {
				return "Player not found.";
			}
			a->bans.addAddress(p.address, period);
			a->saveBanList();
			if (a->hooks.kick) {
				a->hooks.kick(p.id, true);
			}
			return "";
		},
		1);
	bindMethod(
		console, "admin.banPlayerKey", access,
		[a, findPlayer](std::string id, std::string period) -> std::string {
			AdminHooks::Player p;
			if (!findPlayer(id, p) || !p.remote) {
				return "Player not found.";
			}
			a->bans.addKey(p.cdKeyHash, period);
			a->saveBanList();
			if (a->hooks.kick) {
				a->hooks.kick(p.id, true);
			}
			return "";
		},
		1);
	bindMethod( // 0x0042a4b0 -> 0x00507ef0 (the period 0x00507ff0)
		console, "admin.addAddressToBanList", access,
		[a](std::string address, std::string period) { a->bans.addAddress(address, period); }, 1);
	bindMethod( // 0x0042a6a0 -> 0x00507f20 (the period 0x00508030)
		console, "admin.addKeyToBanList", access, [a](std::string key, std::string period) { a->bans.addKey(key, period); },
		1);
	bindMethod(console, "admin.removeAddressFromBanList", access, [a](std::string address) { // 0x0042a890 -> 0x005077b0
		a->bans.removeAddress(address);
		a->saveBanList();
	});
	bindMethod(console, "admin.removeKeyFromBanList", access, [a](std::string key) { // 0x0042a9b6 -> 0x005077d0
		a->bans.removeKey(key);
		a->saveBanList();
	});
	bindMethod(console, "admin.clearBanList", access, [a]() { // 0x0042aadc -> 0x005077f0
		a->bans.clear();
		a->saveBanList();
	});
	bindMethod(console, "admin.listBannedAddresses", access, [a]() { return a->bans.listAddresses(); }); // 0x0042ab4a -> 0x00507800
	bindMethod(console, "admin.listBannedKeys", access, [a]() { return a->bans.listKeys(); }); // 0x0042abee -> 0x005078d0
	// 0x0042ac9e -> 0x004a6090
	bindMethod(console, "admin.kickPlayer", access, [a, findPlayer](std::string id) -> std::string {
		AdminHooks::Player p;
		if (!findPlayer(id, p) || !a->hooks.kick || !a->hooks.kick(p.id, false)) {
			return "Player not found.";
		}
		return "";
	});
	bindMethod(console, "admin.listPlayers", access, [a]() { return a->listPlayers(); }); // 0x0042adfc

	// --- Game.sayAll / sayTeam (0x0042407c, 0x004241be)
	if (!console.hasMethod("Game.sayAll")) {
		bindMethod(console, "Game.sayAll", access, [a](std::string text) {
			if (a->hooks.sayAll) {
				a->hooks.sayAll(text);
			}
		});
	}
	if (!console.hasMethod("Game.sayTeam")) {
		bindMethod(console, "Game.sayTeam", access, [a](int team, std::string text) {
			if (a->hooks.sayTeam) {
				a->hooks.sayTeam(team, text);
			}
		});
	}
}

} // namespace dice::hfe::host
