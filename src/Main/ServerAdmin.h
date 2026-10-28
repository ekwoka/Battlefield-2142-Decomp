// A server's administration: the console objects BF2142.exe registers
// for its server settings and map list and the remote admin's commands,
// on the configuration (net/ServerSettings.h) that lasts from map to map.
//
//   sv.*                 every server setting (0x0041b003..0x004207a8 register
//                        them; the object is DAT_00a201c0), sv.load/save/configFile
//   GameServerSettings.* the menu's host page's setters (0x0041655b..), onto
//                        the same settings
//   maplist.*            the map list (0x0042154a..0x00421d50; MapList.cpp)
//   reservedSlots.*      the reserved slots' nicks (0x0041aa21..)
//   admin.*              (0x00429bba..0x0042adfc) the map rotation, kicks,
//                        bans and the player list, through AdminHooks
//   Game.sayAll/sayTeam  a server message to everyone or a team
//
// rconInvoke is host.rcon_invoke (0x004427b0): a console command run
// for the remote console, its output ending with a newline.
#pragma once

#include "Net/ServerSettings.h"

#include <functional>
#include <string>

namespace dice::hfe {
class Console;
}

namespace dice::hfe::host {

// What admin.* asks of the game running now (main sets these each round).
struct AdminHooks {
	struct Player {
		int id = -1;
		std::string name;
		bool remote = false;     // a client's (else the server's own, or a bot)
		bool ai = false;
		std::string address;     // "a.b.c.d:port" for a client
		std::string cdKeyHash;
	};
	std::function<std::vector<Player>()> players;
	std::function<bool(int player, bool ban)> kick;
	std::function<void()> runNextLevel;   // end this map, start maplist's next
	std::function<void()> restartMap;     // the same map again
	std::function<void(const net::MapListEntry& map)> runLevel;
	std::function<void(const std::string& text)> sayAll;
	std::function<void(int team, const std::string& text)> sayTeam;
};

class ServerAdmin {
public:
	net::ServerSettings settings;
	net::MapList maps;
	net::BanList bans;
	net::ReservedSlots reserved;
	AdminHooks hooks;
	std::string banListFile; // Settings/banlist.con (0x00507570 saves it after each ban)
	int autoRecordRounds = 0; // GameServerSettings.setAutoRecordRounds
	std::string menuGameMode; // GameServerSettings.setGameMode

	// The default files under the mod's Settings folder (the overlay's when
	// `overlay` is given): ServerSettings.con, maplist.con, banlist.con.
	void setDefaultFiles(const std::string& modDirectory, const std::string& overlay = {});
	// Registers the console objects above (on each map's console: the
	// configuration stays here). Methods another module registered first
	// (Game.sayAll) are left to it.
	void registerConsole(Console& console);
	// Runs the settings' (sv.configFile) and the map list's files; false
	// when the file isn't there.
	bool loadSettings(Console& console);
	bool loadMapList(Console& console);
	bool loadBanList(Console& console);
	bool saveSettings() const;
	bool saveMapList() const;
	bool saveBanList() const;

	// host.rcon_invoke (0x004427b0).
	std::string rconInvoke(Console& console, const std::string& command) const;
	// admin.listPlayers' text (0x004a86b0).
	std::string listPlayers() const;

	// The map rotation, asked after each round: what admin.runNextLevel,
	// restartMap or runLevel asked for (the hooks set `request`), else
	// sv.roundsPerMap rounds on each map of the list in turn. Returns the
	// map to run (an empty level without one) and moves the list on; the
	// round bans end.
	enum class Request { None, NextLevel, Restart, RunLevel };
	Request request = Request::None;
	net::MapListEntry requestedLevel; // Request::RunLevel's
	int roundsOnMap = 0;
	net::MapListEntry nextRound();
};

// A file's path with its name matched without case (the exe opens
// "serversettings.con"; the install has "ServerSettings.con"): the
// existing file's, else `path` as given.
std::string findFileNoCase(const std::string& path);

} // namespace dice::hfe::host
