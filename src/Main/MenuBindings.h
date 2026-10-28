// The main menu's way into the game: the natives of SwiffPlayer.dll
// (Natives.h) that the login, soldier, multiplayer and join pages of
// mainMenu.dfm call, bound to the account service (Net/Fesl.h), the
// master server (Net/ServerBrowser.h) and the game connection.
//
// The menu polls: a script starts a job with one call and then asks every
// frame (its updateManager callbacks) until the job is done, so the
// blocking network parts run on worker threads here and the natives only
// look at their state.
//
//   Version check (Logic, 0x1003e0c0): checkVersion(true) starts it,
//     checkVersion(false) is true while it runs (0x1003b540);
//     getVersionResult() is -1 when a newer patch exists, else 0
//     (0x1003b5a0). There is no patch server: done, no patch.
//   Online check (Profile, 0x100516b0): checkOnlineStatus(true) starts it,
//     (false) is true once it is over (0x1004b430); isOnline() is its result
//     unless offline mode was forced (0x10049ab0).
//   Account login: connectToMasterAccount(name, password, remember)
//     (0x1004bc20) and connectToSubAccount(soldier) (0x100510c0) start
//     transactions; isTransactionComplete() (0x10049bf0), getLastError()
//     (0x10049f40, the ProfileManager's error index, kProfileError*) and
//     getLastErrorString() (0x1004d8e0, its localized text: BF2142.exe
//     0x004a0980) report them. The login page shows the text with
//     HEADER_loginerror, except for 2, 3 and 38 (EA Online unreachable: it
//     offers offline mode), 20 and 21.
//   Offline mode: Logic.setOfflineMode(account) (0x1003dc70) picks the
//     soldiers kept locally for an account (the game caches them from
//     online logins); with no name it leaves offline mode.
//   Messages (dice.MessageHandler, 0x1003f7d0): a queue the menu empties
//     (checkForMessages): type 1 error text, 2 warning text, 3 the key of
//     a LOCERROR_ string (type 4, the anti-cheat's, never comes: the
//     rewrite has none).
//   Server lists (dice.bf2.List): fillList(true)/refreshList(true) start a
//     query, (false) is true while it runs; rows (0x10079770) are cell0
//     name, cell1 "players/max (queue)", cell2 the ping bar (a clip),
//     cell3 map, cell4 mode. The quick and advanced lists start with three
//     rows: "Favorites", a gap and "Live servers" (so the menu counts
//     rows - 3). selectRow(n) returns the row's server (0x100743xx):
//     serverAddress "ip gameport queryport", password, sameMod, version,
//     punkbuster, maxRank, hasReservedSlots, isBoosterPackMap,
//     isCustomPackMap, customMapLink.
//   Joining: Multiplay.joinServer(serverAddress) (0x10043ed0) is false
//     when the address cannot be used (the menu then shows
//     TEXT_servererror). Here it calls onJoin and tries the server's
//     handshake; a refusal or no answer comes back as a type 3 message
//     (LOCERROR_serverfull, ..._connectionfailed) that replaces the menu's
//     "joining server" popup.
#pragma once

#include "Net/GameConnection.h"
#include "Net/ServerBrowser.h"
#include "Stats/Stats.h"
#include "UI/Avm1/Value.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe::ui {
class NativeRegistry;
}
namespace dice::hfe::input {
class ControlMaps;
}

namespace dice::hfe::menu {

struct RoundSummary;
class SettingsObjects;

// The ProfileManager's error indices (BF2142.exe 0x004a0980's table).
enum ProfileError : int {
	kProfileErrorNone = 0,
	kProfileErrorNoConnection = 2,
	kProfileErrorTimeout = 3,
	kProfileErrorPasswordWrong = 8,
	kProfileErrorAccountNotFound = 15,
	kProfileErrorAccountDisabled = 20,
	kProfileErrorAccountBanned = 21,
	kProfileErrorSystem = 38,
	kProfileErrorUnhandled = 39,
};
// The index for a FESL errorCode (0x004a1730).
int profileErrorFromFesl(long code);
// The localization key of an index ("" for none).
const char* profileErrorKey(int error);

// What the account service answered. `reached` is false when it could not
// be reached at all; `error` is FESL's errorCode (0 for success).
struct AccountReply {
	bool reached = true;
	long error = 0;
	std::vector<std::string> soldiers; // the account's soldiers (login)
	long profileId = 0;                // the soldier's profile (soldier login)
};

// A level the menu can start: its folder name, a game mode it has and a
// size (Levels/<level>/server.zip has GameModes/<gameMode>/<size>/).
struct MenuMap {
	std::string level;
	std::string gameMode;
	int size = 0;
};

// What Multiplay.createServer starts: the map list and the settings.
struct GameSetup {
	std::vector<MenuMap> maps; // the first is played first
	int maxPlayers = 16;
	int bots = 16;               // GeneralSettings.setMaxBots
	bool botsIncludeHumans = true; // the bots fill up to `bots` players
	float botSkill = 0.5f;       // 0..1
	int ticketRatio = 100;       // percent
	int spawnTime = 15;          // seconds
	int timeLimit = 0;           // minutes, 0 for none
	int scoreLimit = 0;
	bool friendlyFire = true;
	int roundsPerMap = 1;
};

// The network behind the menu, replaceable (tests use fakes). Each
// function blocks and runs on a worker thread.
struct MenuServices {
	// Whether the account service can be reached.
	std::function<bool()> checkOnline;
	// Logs in to an EA account and lists its soldiers.
	std::function<AccountReply(const std::string& name, const std::string& password)> loginAccount;
	// Picks a soldier of the account logged in last.
	std::function<AccountReply(const std::string& soldier)> loginSoldier;
	std::function<void()> logoutAccount;
	// The master server's list.
	std::function<std::optional<net::ServerList>(std::string* error)> fetchServers;
	// The LOCAL page's search (serverListLAN; the "lan" browser,
	// 0x00467d40 -> 0x00715900): the servers on the local network that
	// answer the query port probe (net/ServerQuery.h).
	std::function<std::vector<net::ServerEntry>()> browseLan;
	// Tries a game server's connect handshake (empty: not tried).
	std::function<net::ConnectResult(const std::string& host, uint16_t port)> connectServer;

	// OpenSpy: FESL at fesl.openspy.net, the master at
	// stella.ms5.openspy.net, the handshake with GameConnection.
	static MenuServices openSpy();
};

class MenuBindings {
public:
	// Binds the natives in `natives` (they stay bound to this object:
	// keep it alive while the menu runs).
	MenuBindings(ui::NativeRegistry& natives, MenuServices services = MenuServices::openSpy());
	~MenuBindings();
	MenuBindings(const MenuBindings&) = delete;
	MenuBindings& operator=(const MenuBindings&) = delete;

	// The award definitions (Stats.h, from awards.con).
	void setAwards(std::vector<stats::AwardDefinition> awards) { m_awards = std::move(awards); }
	const std::vector<stats::AwardDefinition>& awards() const { return m_awards; }
	// The awardProgress list's rows for an award id (its fillList).
	void fillAwardProgress(const std::string& id);

	// Called when the menu joins a server (host and game port). The game
	// takes over from here; the handshake probe (connectServer) runs after.
	std::function<void(const std::string& host, uint16_t port)> onJoin;
	// Logic.quit.
	std::function<void()> onQuit;
	// Multiplay.createServer: the game takes over with this setup.
	std::function<void(const GameSetup&)> onStartGame;
	// Logic.disconnectGame: leave the running game for the menu.
	std::function<void()> onDisconnect;
	// Whether a game runs under the menu (Logic.isGameRunning).
	void setGameRunning(bool running) { m_gameRunning = running; }
	// Told what happens (logins, lists, joins), one line each.
	std::function<void(const std::string&)> onLog;

	// Where accounts used offline are kept between runs ("" keeps them in
	// memory only). Read at once, written on changes.
	void setProfileFile(const std::string& path);
	// The default file: <config dir>/bf2142-recomp/profiles.txt.
	static std::string defaultProfileFile();

	// Where the soldiers' stats (Stats/Stats.h: rank, unlocks, totals) are
	// kept ("" in memory only). Read at once, written on changes.
	void setStatsFile(const std::string& path) { m_stats.setFile(path); }
	// <config dir>/bf2142-recomp/stats.txt.
	static std::string defaultStatsFile() { return stats::StatsStore::defaultFile(); }
	stats::StatsStore& stats() { return m_stats; }
	// Whether the Northern Strike unlocks can be chosen (on: the booster
	// ships with 1.51).
	void setBoosterPack(bool owned) { m_booster = owned; }
	// The soldier logged in (nullptr when none).
	stats::SoldierStats* soldierStats();
	// The unlocks the soldier chose (none without one).
	std::vector<int> soldierUnlocks();
	// The round just over, added to the soldier's record (saved): the
	// summary's rank fields (roundExp, totalExp, rank, rankedUp) follow.
	// Nothing is recorded without a soldier.
	void recordRound(RoundSummary& summary);

	// Where the settings pages' applied values are kept ("" in memory
	// only). Read at once, written on apply.
	void setSettingsFile(const std::string& path);
	// <config dir>/bf2142-recomp/settings.txt.
	static std::string defaultSettingsFile();
	// A settings page applied its values (read them with setting()).
	std::function<void()> onSettingsApplied;
	// A setting by "Object.Name" ("AudioSettings.EffectsVolume") as a number.
	double setting(const std::string& name, double fallback) const;
	// The same as text ("" when unset or not text).
	std::string text(const std::string& name) const;

	// The controls page's maps (ControlSettings), and the key or button the
	// player pressed last ("IDKey_Q"), which mapInput binds.
	void setControls(input::ControlMaps* controls) { m_controls = controls; }
	void setLastInput(const std::string& control)
	{
		m_lastInput = control;
		m_lastDevice = control.compare(0, 9, "IDButton_") == 0 ? "IDFMouse" : "IDFKeyboard";
		m_lastDirection = 1;
	}
	// A game controller's (GameControllers::lastInput): "IDFGameController_0"
	// with "IDButton_3", or "IDAxis_1" and the way it was pushed (-1, +1).
	void setLastInput(const std::string& device, const std::string& control, float direction)
	{
		m_lastDevice = device;
		m_lastInput = control;
		m_lastDirection = direction;
	}

	// The game's settings objects (SettingsObjects.h): their values (read
	// from the profile's .con files) become the pages' starting values, and
	// each page's apply() writes its values back to them and saves the
	// files. The names match but for a few the pages spell differently
	// (AudioSettings.VoiceOverVolume is helpVoiceVolume, NumberOfVoices
	// forceNumberOfVoices, VideoSettings.BloomEnabled setUseBloom).
	void setSettingsObjects(SettingsObjects* objects);

	// The display modes the video page offers ("1280x720@60Hz").
	void setDisplayModes(const std::vector<std::string>& modes);

	// The levels the map lists offer (all game modes; the menu filters).
	void setMaps(std::vector<MenuMap> maps);
	// The setup createServer would start now.
	GameSetup gameSetup() const;

	// A message for the menu to show (dice.MessageHandler).
	void raiseMessage(int type, const std::string& text, int code = 0);

	// --- State (for the game and tests)

	bool online() const;
	bool loggedIn() const { return m_account.loggedIn; }
	bool offlineMode() const { return m_offline; }
	const std::string& accountName() const { return m_account.name; }
	const std::string& soldierName() const { return m_soldier; }
	size_t serverCount() const { return m_servers.size(); }
	// The last server the menu joined ("" before).
	const std::string& joinedHost() const { return m_joinHost; }
	uint16_t joinedPort() const { return m_joinPort; }
	// Its map as the server browser listed it (empty level when unknown).
	const MenuMap& joinedMap() const { return m_joinMap; }
	// The outcome of the last handshake probe ("" while none finished).
	std::string joinOutcome() const;
	// Waits for the jobs running now (tests).
	void finishJobs();

private:
	struct Job;
	struct LocalAccount {
		std::string name;
		std::vector<std::string> soldiers;
		std::string remembered; // the password, scrambled, when "remember me" was ticked
	};
	struct Server {
		net::ServerEntry entry;
		uint16_t gamePort = 0;
	};

	void bindLogic();
	void bindProfile();
	void bindProfileStats();
	void bindMessages();
	void bindLists();
	// The BFHQ details page's rows for a category (statsTypes' order).
	std::vector<std::vector<std::string>> statsRows(int category);
	void bindMultiplay();
	void bindServerSettings();
	void showMapLists();
	std::vector<MenuMap> availableMaps() const;

	void startAccountLogin(const std::string& name, const std::string& password, bool remember);
	void finishAccountLogin();
	void finishSoldierLogin();
	void startServerQuery();
	bool finishServerQuery();
	void showServers();
	LocalAccount* localAccount(const std::string& name);
	void loadProfiles();
	void saveProfiles();
	static std::string loginToken(const LocalAccount& local);
	void pollJoin();
	void setSoldierRows(const std::vector<std::string>& soldiers);
	void log(const std::string& line) const;

	ui::NativeRegistry& m_natives;
	MenuServices m_services;

	// Startup checks.
	std::shared_ptr<Job> m_onlineCheck;
	bool m_online = false;
	bool m_offline = false; // forced offline mode (Logic.setOfflineMode)

	// The account: an EA login (online) or a local one (offline).
	struct Account {
		std::string name;
		std::vector<std::string> soldiers;
		bool loggedIn = false;
		bool connected = false; // to the account service
	} m_account;
	std::string m_soldier, m_namePrefix;
	std::shared_ptr<Job> m_transaction; // the running login
	enum class Transaction { None, Account, Soldier } m_transactionKind = Transaction::None;
	bool m_transactionRemember = false;
	std::string m_transactionName, m_transactionPassword;
	int m_lastError = 0;
	long m_lastFeslError = 0;

	std::vector<LocalAccount> m_local;
	std::string m_lastUser;
	std::string m_profileFile;

	// The soldiers' stats.
	stats::StatsStore m_stats;
	bool m_booster = true;
	int m_rankNotification = 0; // PlayerStats.getRankNotification: 1 up, 2 down

	struct Message {
		int type = 0;
		int code = 0;
		std::string text;
	};
	std::vector<Message> m_messages;

	// Servers.
	std::shared_ptr<Job> m_query;
	std::vector<Server> m_servers;
	// Awards (awards.con): none is held offline (the stats server granted
	// them); the pages show each locked, with its criteria.
	std::vector<stats::AwardDefinition> m_awards;
	std::string m_selectedAward;
	struct AwardRow {
		bool check = false;    // mcIconCheck shown
		float progress = -1;   // mcProgress.setValue, -1 none
	};
	std::vector<AwardRow> m_awardRows;
	std::shared_ptr<Job> m_lanQuery;
	std::vector<Server> m_lanServers; // the LOCAL page's
	void startLanQuery();
	bool finishLanQuery();
	void showLanServers();
	// The servers a list shows (nullptr: not a server list we fill).
	const std::vector<Server>* serversOf(const std::string& listId) const;

	std::string m_infoHost; // the server info popup's server

	// The controls page.
	void bindControls();
	input::ControlMaps* m_controls = nullptr;
	std::string m_lastInput, m_lastDevice;
	float m_lastDirection = 1;
	int m_controlMap = 0;

	// Starting a game.
	bool m_gameRunning = false;
	std::vector<MenuMap> m_maps;
	std::string m_filterGameMode = "gpm_sp";
	std::vector<MenuMap> m_mapList;
	// ServerSettings and GeneralSettings values by name ("MaxPlayers").
	std::map<std::string, ui::avm1::Value> m_settings;
	std::map<std::string, ui::avm1::Value> m_appliedSettings;
	std::string m_settingsFile;
	void saveSettings() const;
	SettingsObjects* m_objects = nullptr;
	void exportSettings(const std::string& prefix);

	std::shared_ptr<Job> m_joinProbe;
	bool m_joinReported = false;
	std::string m_joinHost;
	uint16_t m_joinPort = 0;
	MenuMap m_joinMap;
};

} // namespace dice::hfe::menu
