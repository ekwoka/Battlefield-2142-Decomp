// The loading and end-of-round screen's way into the game: the natives of
// SwiffPlayer.dll that endOfRound.dfm calls (dice.bf2.EndOfRound, made at
// 0x1001fd50, and parts of Logic, Client, ServerInfo and VideoSettings),
// answered from a RoundSummary the game fills.
//
// The movie polls. Its root (frame 1, loadProgress, every frame) asks:
//   Logic.getIsLoading() (0x1003c9e0): true while a level loads, false
//     while the finished round's screens wait ("END OF ROUND. LOADING NEW
//     MAP...", the outcome page).
//   Logic.isFirstTimeLoading() (0x1003c970) and EndOfRound.isValidEOR()
//     (0x1001bdd0): a first load, or one without a finished round, shows
//     the briefing (the loading screen: the map's picture and description,
//     the game mode's, a game tip and the server); otherwise the finished
//     round's pages take turns, 8 s each, while the next level loads.
//   Client.getLoadProgress() (0x100147b0): 0..1, the loading bar. From 0.95
//     on it says "LOADING COMPLETE. VERIFYING CLIENT DATA." and waits.
//   It is ready when it is not loading, the progress is under 0.95 and
//     Logic.getNumRoundsLeft() > 0 (or, never met, a first load at 1.0):
//     the tabs and JOIN GAME (and SPECTATOR when the server allows it) show;
//     they call Logic.readyToPlay(asSpectator) (0x1003dba0): onReadyToPlay.
//
// The finished round (EndOfRound.*, from the game's end-of-round record,
// SwiffHost slot 0x180): teams are 0 and 1 for the movie (the game's 1 and
// 2); getMyTeam and getWinningTeam (-1 for a draw) use them, getTeamName
// gives the team's name, which the movie lower-cases for its images and
// keys ("pac", "eu"). Pictures: images/eor/<played level>/outcome_
// [gpm_ti_]<win|loss|draw>_<team>.png (getBackgroundImage, 0x1001d9c0),
// images/eor/header_outcome_<outcome>_<language>.png
// (getHeaderImageOutcome, 0x1001db80), and for the map the movie asks for
// "$/Levels/<level>/Info/<mode>_<size>_menuMap.png": "$/" is the game's
// root, which the host's file reader has to resolve.
//
// Lists (dice.bf2.List; the rows are clips "row0", "row1", ...):
//   eorPlayerList "0" (the player's team) or "1" (0x10066c40): "N. name",
//     score, team score, kills, deaths, score per minute.
//   eorEarnedAwards "<page>" (0x10066190, 0x10066540): four awards a page.
//   eorSquadScore "<0|1> <place>" (0x100678b0): a best squad's leader,
//     members and totals.
//   eorDebriefing "<category> <place>": the best by kit, weapon and
//     vehicle (no records yet: empty).
#pragma once

#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dice::hfe::ui {
class NativeRegistry;
class NativeList;
namespace avm1 {
class Interpreter;
}
} // namespace dice::hfe::ui

namespace dice::hfe::menu {

// A player of the finished round.
struct EndOfRoundPlayer {
	std::string name;
	int team = 1; // 1 or 2 (the game's team numbers)
	int score = 0;
	int kills = 0;
	int deaths = 0;
	int teamwork = 0;      // the team score
	float scorePerMinute = 0;
	int squad = 0;         // 0: none, else the squad's number in the team
	bool squadLeader = false;
	bool commander = false;
	bool buddy = false;    // the buddy icon
};

// A squad's name (players give their squad by number).
struct EndOfRoundSquad {
	int team = 1;
	int number = 1;
	std::string name; // "Alpha"
};

// An award earned in the round: the award's id (1xx badges, 2xx medals,
// 3xx ribbons, 4xx pins), its level (badges) and how often.
struct EndOfRoundAward {
	int id = 0;
	int level = 0;
	int count = 1;
};

struct RoundSummary {
	// --- The movie's state
	bool loading = false;      // a level loads (Logic.getIsLoading)
	bool levelLoaded = false;  // the level is up and the player may join (see setLevelLoaded)
	bool firstLoad = true;     // the first level since joining (Logic.isFirstTimeLoading)
	float loadProgress = 0;    // 0..1 (Client.getLoadProgress)
	int roundsLeft = 999;      // on this map (Logic.getNumRoundsLeft): 999 for a game started here (0x004d3450)
	bool optimizingShaders = false;

	// --- The level loading or played next
	std::string mapPath;       // the level's folder name ("Minsk")
	std::string mapName;       // shown ("" = mapPath with spaces for underscores)
	std::string gameMode = "gpm_cq";
	int mapSize = 32;
	int maxPlayers = 32;
	std::string mapDescriptionKey;      // the .desc's briefing locid
	std::string gameModeDescriptionKey; // its maptype's locid ("GAMEMODE_DESCRIPTION_assault")
	bool friendlyFire = true;
	bool autoBalance = true;
	int roundsPerMap = 1;

	// --- The server
	std::string serverName;
	std::string serverAddress; // "ip:port"
	std::string welcomeMessage;
	bool allowSpectators = false;
	std::string sponsorLogo, communityLogo; // image paths ("" for none)

	// --- The finished round (validRound false: none)
	bool validRound = false;
	std::string previousMapPath; // the level played ("Minsk")
	std::string previousGameMode = "gpm_cq";
	int previousMapSize = 32;
	std::array<std::string, 2> teamNames {"PAC", "EU"}; // teams 1 and 2
	int myTeam = 1;      // 1 or 2
	int winningTeam = 0; // 1, 2, or 0 for a draw
	std::array<int, 2> victories {0, 0}; // rounds won on this map
	bool ranked = false;

	// The local player's round.
	int score = 0;
	int kills = 0;
	int deaths = 0;
	int dogTags = 0;
	float scorePerMinute = 0;
	int awayBonus = 0;
	int squadMemberBonus = 0;
	int squadLeaderBonus = 0;
	int commanderBonus = 0;
	float roundSeconds = 0; // how long the round lasted
	// The soldier's standing (MenuBindings::recordRound, from the local
	// stats store; a recruit's without a soldier).
	int roundExp = 0;   // points earned for the rank
	int totalExp = 0;   // after the round
	int rank = 0;       // after the round (RANK_<n>)
	bool rankedUp = false;

	std::vector<EndOfRoundPlayer> players; // both teams
	int localPlayer = -1;                  // index into players
	std::vector<EndOfRoundSquad> squads;
	std::vector<EndOfRoundAward> awards;
};

// The points a rank starts at (rank 0, a recruit: 0): RankSettings.con's
// table (stats::ranks()); ranks past the table's end need its last value.
int rankExperience(int rank);

// The parts of a level's Info/<Level>.desc for a game mode and size: the
// name, the briefing's key and the maptype's description key.
struct LevelDescription {
	std::string name;
	std::string briefingKey;
	std::string gameModeKey;
	std::vector<int> sizes; // of the game mode
};
LevelDescription parseLevelDescription(const std::string& xml, const std::string& gameMode, int size);

class EndOfRoundBindings {
public:
	// Binds the natives in `natives` (they stay bound to this object: keep
	// it alive while the movie runs). It replaces the lists' fillList and
	// refreshList; lists other than the end-of-round ones keep the plain
	// behaviour (rows from setListRows), so give the movie its own registry.
	explicit EndOfRoundBindings(ui::NativeRegistry& natives);
	EndOfRoundBindings(const EndOfRoundBindings&) = delete;
	EndOfRoundBindings& operator=(const EndOfRoundBindings&) = delete;

	// What the movie shows from now on (it polls).
	void setSummary(RoundSummary summary);
	const RoundSummary& summary() const { return m_summary; }
	// Shortcuts for the loading screen: a level loads (progress 0..1), or
	// it is up (not loading, progress back to 0, as 0x004cbea0 leaves the
	// host). The movie shows JOIN GAME only when it is not loading, its
	// progress is under 0.95 and Logic.getNumRoundsLeft() > 0 (root frame
	// 1, loadProgress).
	void setLoading(float progress);
	void setLevelLoaded();

	// A game mode's name from the mod (SwiffHost.addGameMode into ModInfo,
	// its slot 0x48 0x00460ff0: "N/A" when unknown), behind the
	// HEADER_<mode> text (0x1001beb0, 0x1001bfd0).
	std::function<std::string(const std::string& mode)> gameModeName;

	// Logic.readyToPlay: the player pressed JOIN GAME (false) or SPECTATOR (true).
	std::function<void(bool asSpectator)> onReadyToPlay;

	// Logic.get/setStorageString: what the movie keeps between runs (the
	// slideshow's page, "EOR_lastpage").
	const std::map<std::string, std::string>& storage() const { return m_storage; }

private:
	void bindEndOfRound();
	std::string modeName(const std::string& mode) const;
	void bindLogic();
	void bindServer();
	void bindLists();
	struct SquadScore {
		const EndOfRoundSquad* squad = nullptr;
		int number = 0;
		std::vector<const EndOfRoundPlayer*> members; // the leader first
		int score = 0, teamwork = 0;
	};
	// A team's squads by score.
	std::vector<SquadScore> teamSquads(int team) const;
	static std::string squadName(const SquadScore& squad);
	// Every player's place by score (0 the best), for the medals.
	int overallPlace(const EndOfRoundPlayer* player) const;
	void fillPlayers(ui::avm1::Interpreter& vm, ui::NativeList& list);
	void fillAwards(ui::avm1::Interpreter& vm, ui::NativeList& list);
	void fillSquads(ui::avm1::Interpreter& vm, ui::NativeList& list);
	void fillDebriefing(ui::avm1::Interpreter& vm, ui::NativeList& list);

	// A team's players (the game's team 1 or 2) by score.
	std::vector<const EndOfRoundPlayer*> teamPlayers(int team) const;
	std::string outcome() const;
	std::string mapName() const;
	std::string localized(const std::string& key) const;

	ui::NativeRegistry& m_natives;
	RoundSummary m_summary;
	std::map<std::string, std::string> m_storage;
	int m_lastTip = 0;
	unsigned m_random = 12345;
};

} // namespace dice::hfe::menu
