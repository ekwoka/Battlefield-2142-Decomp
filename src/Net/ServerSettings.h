// A game server's configuration, as BF2142.exe keeps it for a dedicated
// (or menu-hosted) server: the server settings (`sv.*`, the object at
// DAT_00a201c0), the map list (`maplist.*`, MapList.cpp, DAT_00a21940),
// the reserved slots (`reservedSlots.*`) and the ban list (`admin.*`'s,
// Settings/banlist.con).
//
// Where the files come from (0x00409950, the command line): `+config <file>`
// is the settings' file (default <mod>/Settings/serversettings.con, or the
// overlay path's Settings/serversettings.con), `+mapList <file>` the map
// list's (default <mod>/Settings/maplist.con). Both are console scripts
// (sv.serverName "...", mapList.append minsk gpm_cq 32); `sv.save` writes
// every setting back in the order 0x00431880 writes them, `maplist.save`
// one `maplist.append "level" "mode" size` line per map (0x00440ef0).
//
// Nothing here touches the network or the console: HostedGame reads the
// settings, ServerAdmin (src/Main) binds them to the console.
#pragma once

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace dice::hfe::net {

// Every `sv.*` setting (the defaults are the shipped ServerSettings.con's).
struct ServerSettings {
	std::string configFile;  // sv.configFile: where load/save go
	std::string serverName = "Battlefield 2142";
	std::string password;
	bool internet = false;
	int bandwidthChoke = 0;
	int maxConnectionType = 0; // sv.maxConnectionType (an index into the rate table; not saved)
	std::string serverIP;
	int serverPort = 17567;
	std::string welcomeMessage;
	bool punkBuster = false;
	bool allowFreeCam = false;
	bool allowExternalViews = true;
	bool allowNoseCam = true;
	bool hitIndicator = true;
	int maxPlayers = 64;
	int numPlayersNeededToStart = 1;
	float notEnoughPlayersRestartDelay = 15;
	int startDelay = 15;
	int endDelay = 15;
	float spawnTime = 15;
	float manDownTime = 15;
	int endOfRoundDelay = 3;
	int ticketRatio = 100;
	int roundsPerMap = 3;
	int timeLimit = 0;   // minutes
	int scoreLimit = 0;
	int soldierFriendlyFire = 100;
	int vehicleFriendlyFire = 100;
	int soldierSplashFriendlyFire = 100;
	int vehicleSplashFriendlyFire = 100;
	bool tkPunishEnabled = true;
	int tkNumPunishToKick = 3;
	bool tkPunishByDefault = false;
	bool votingEnabled = true;
	int voteTime = 90;
	int minPlayersForVoting = 2;
	int gameSpyPort = 29900;
	bool allowNATNegotiation = false;
	std::string interfaceIP;
	bool autoRecord = false;
	std::string demoIndexURL = "http://";
	std::string demoDownloadURL = "http://";
	std::string autoDemoHook = "adminutils/demo/rotate_demo.exe";
	int demoQuality = 1;
	std::string adminScript = "default";
	float timeBeforeRestartMap = 30;
	bool autoBalanceTeam = false;
	float teamRatioPercent = 100;
	bool voipEnabled = true;
	int voipQuality = 3;
	bool voipServerRemote = false;
	std::string voipServerRemoteIP;
	int voipServerPort = 55125;
	int voipBFClientPort = 55123;
	int voipBFServerPort = 55124;
	std::string voipSharedPassword;
	bool useGlobalRank = true;
	bool useGlobalUnlocks = true;
	bool ranked = false;
	int minUnlockLevel = 0;
	int maxUnlockLevel = -1;
	std::string sponsorText;
	std::string sponsorLogoURL;
	std::string communityLogoURL;
	std::string customMapURL;
	int radioSpamInterval = 6;
	int radioMaxSpamFlagCount = 6;
	int radioBlockedDurationTime = 30;
	int numReservedSlots = 0;
	int maxRank = 0;
	bool friendlyFireWithMines = true;
	float botSkill = 0.5f;
	bool allowSpectators = false;
	bool allowTitanMovement = true;

	// What a ranked server (sv.ranked, +0x26d) runs with whatever was set:
	// spawnTime and manDownTime 15 (0x00430f60, 0x00430f80), ticketRatio
	// 100 (0x00430fa0), teamRatioPercent 100 (0x00430fc0). The console's
	// getters read these (0x0041cd19 ...), sv.save the values set.
	float spawnTimeInForce() const { return ranked ? 15.0f : spawnTime; }
	float manDownTimeInForce() const { return ranked ? 15.0f : manDownTime; }
	int ticketRatioInForce() const { return ranked ? 100 : ticketRatio; }
	float teamRatioInForce() const { return ranked ? 100.0f : teamRatioPercent; }
	// The pause after a round's end (0x00430610): 15 in gpm_ti (the mode's
	// name without case), else sv.endOfRoundDelay, which its setter keeps
	// within 3..30 (0x00431640; the constructor's 15, the shipped file's 3).
	int endOfRoundDelayInForce(const std::string& gameMode) const;
	// The countdown from a round's end to the next round or map (+0x284,
	// set by 0x004ba250, counted down by 0x004bd390): the end-of-round
	// delay, and on the map's last round (rounds played == roundsPerMap
	// - 1) sv.endDelay on top.
	float roundEndCountdown(const std::string& gameMode, bool lastRoundOnMap) const
	{
		return static_cast<float>(endOfRoundDelayInForce(gameMode) + (lastRoundOnMap ? endDelay : 0));
	}
	// A ranked game (0x00430a00): gpm_cq, gpm_ti, gpm_sl, gpm_nv or gpm_ca,
	// an internet server (+0x54) with GameSpyRanked (the GameSpy back end's
	// word on the server, which no LAN or offline host has) and sv.ranked;
	// with AIDLL loaded also its interface's slot 0x20 (not modelled).
	bool rankedGame(const std::string& gameMode, bool gameSpyRanked) const;
	// Its minimums: maxPlayers at least 16 (0x00431690), the players needed
	// to start at least 8 over 32 players, else 6 (0x004316b0).
	int maxPlayersInForce(bool rankedGame) const { return rankedGame && maxPlayers < 17 ? 16 : maxPlayers; }
	int playersNeededToStartInForce(bool rankedGame) const
	{
		if (!rankedGame) {
			return numPlayersNeededToStart;
		}
		return std::max(numPlayersNeededToStart, maxPlayers > 32 ? 8 : 6);
	}

	// The settings as console lines (0x00431880's order and quoting).
	std::string save() const;

	// Each setting by its console name ("serverName"): reads it as the
	// console prints it, writes it from a console argument. False for a
	// name that isn't a setting.
	bool get(const std::string& name, std::string& value) const;
	bool set(const std::string& name, const std::string& value);
	// The settings' console names, in save order (configFile and
	// maxConnectionType last, as they aren't saved).
	static const std::vector<std::string>& names();
};

// The map list (MapList.cpp): the maps the server runs in turn. `current`
// is the map running (+0x1c), `next` the one after it (admin.nextLevel).
struct MapListEntry {
	std::string level;    // as written ("minsk"; the level folder is matched without case)
	std::string gameMode; // "gpm_cq"
	int size = 0;         // 16, 32 or 64; 0 for the mode's default
};

class MapList {
public:
	std::string configFile;

	// Whether a map can be added: the level exists and has the mode (0x00441090's
	// "could not add to maplist. map not found:" / "gameMode not found:").
	// Without one, anything is taken.
	std::function<bool(const MapListEntry& entry, std::string& error)> validate;

	bool append(const MapListEntry& entry, std::string* error = nullptr);
	bool insert(int index, const MapListEntry& entry, std::string* error = nullptr);
	int remove(int index); // the new count, -1 for a bad index
	void clear();
	// "0: \"minsk\" \"gpm_cq\" 32\n" per map.
	std::string list() const;
	std::string save() const;

	const std::vector<MapListEntry>& entries() const { return m_entries; }
	int count() const { return static_cast<int>(m_entries.size()); }
	int current() const { return m_current; }
	void setCurrent(int index);
	// The map after the current one (admin.nextLevel), wrapping round.
	int next() const;
	void setNext(int index);
	// Moves on to the next map (the round ended `roundsPerMap` times, or
	// admin.runNextLevel): current becomes next, next the one after.
	void advance();

private:
	std::vector<MapListEntry> m_entries;
	int m_current = 0;
	int m_next = -1; // -1: the one after current
};

// The ban list (admin.addAddressToBanList / addKeyToBanList; saved to
// Settings/banlist.con by 0x00507570). A ban lasts for good ("perm" or
// nothing), for the round ("round"), or a number of seconds (0x00505db0
// reads the period).
struct Ban {
	std::string what;   // an address ("1.2.3.4") or a CD-key hash
	enum class Kind { Permanent, Round, Until } kind = Kind::Permanent;
	std::time_t until = 0; // Kind::Until: when it ends
	std::string period() const; // as saved: "perm", "round" or the seconds left
};

class BanList {
public:
	// False for a period that isn't one ("perm", "round", "", or seconds).
	static bool parsePeriod(const std::string& text, Ban& out, std::time_t now);
	bool addAddress(const std::string& address, const std::string& period, std::time_t now = std::time(nullptr));
	bool addKey(const std::string& key, const std::string& period, std::time_t now = std::time(nullptr));
	void removeAddress(const std::string& address);
	void removeKey(const std::string& key);
	void clear();
	// The address ("a.b.c.d", a port after a colon is ignored) or key is banned now.
	bool bannedAddress(const std::string& address, std::time_t now = std::time(nullptr)) const;
	bool bannedKey(const std::string& key, std::time_t now = std::time(nullptr)) const;
	// A new round: the round bans end.
	void endRound();
	std::string listAddresses(std::time_t now = std::time(nullptr)) const;
	std::string listKeys(std::time_t now = std::time(nullptr)) const;
	std::string save(std::time_t now = std::time(nullptr)) const;
	const std::vector<Ban>& addresses() const { return m_addresses; }
	const std::vector<Ban>& keys() const { return m_keys; }

private:
	std::vector<Ban> m_addresses, m_keys;
};

// The reserved slots: nicks that may join a full server.
class ReservedSlots {
public:
	std::string configFile;
	void addNick(const std::string& nick);
	void removeNick(const std::string& nick);
	void clear() { m_nicks.clear(); }
	bool has(const std::string& nick) const;
	std::string list() const; // one nick a line
	std::string save() const; // reservedSlots.addNick lines
	const std::vector<std::string>& nicks() const { return m_nicks; }

private:
	std::vector<std::string> m_nicks;
};

} // namespace dice::hfe::net
