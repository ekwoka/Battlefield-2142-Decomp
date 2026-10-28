// A soldier's standing kept on this machine: experience, rank, the round
// totals and the unlocks chosen. The game asked the stats server for all
// of this (BF2142.exe 0x00450000..: getplayerinfo.aspx, getunlocksinfo.aspx
// 0x0045bb..., selectunlock.aspx, getawardsinfo.aspx, getrankinfo.aspx at
// stella.prod.gamespy.com); there is no account to ask here, so the store
// below plays the server's part and offline play works on its own.
//
// Ranks (Settings/RankSettings.con, read at start-up by 0x0043ac80 from
// <mod>/settings/ranksettings.con; "rankSettings.setRank <rank> <points>",
// written back by rankSettings.save 0x0043afa0): the points each rank
// starts at, ranks 0..40. Ranks 41..43 are listed with 0 points: they are
// not reached by points (the leaderboard's top ranks) and are skipped here.
//
// Unlocks: the ids the menu uses are <kit><branch><level>:
//   kit    1 Recon, 2 Assault, 3 Engineer (Anti-Vehicle), 4 Support (the
//          Kit templates' unlockIndex + 1), 5 for every kit (the player and
//          squad leader upgrades);
//   branch 1 and 2 (kits 1..4: the ItemContainers' unlockBranch UB_First
//          and UB_Second; kit 5: UB_Player and UB_SquadLeader);
//   level  1.. (the ItemContainers' unlockLevel + 1): 5 per kit branch,
//          6 player upgrades, 4 squad leader ones.
// 0x0045c030 turns the server's list into the menu's table: in each branch
// the levels up to the highest one owned are taken (1), the next one is
// available (2) and the rest locked (0), so a branch is bought in order.
// 0x0045be30 turns it into the game's: a mask per kit and branch, the 5xx
// ones the same for every kit. The Northern Strike ones (each kit branch's
// level 5, 516 and 524) need the booster (SwiffPlayer 0x1004e000,
// 0x1004b250 answers 302 without it). One unlock credit per rank: the menu
// shows the server's count of unspent credits (+0x18 of the unlock info,
// getUnlocksCountUnselected 0x10049a40), which the client only reads
// (the 12th token of getunlocksinfo.aspx, see parseUnlocksInfo) and never
// works out. The rule lived in GameSpy's stats back end (the .aspx pages
// at stella.prod.gamespy.com), which was never shipped, so the published
// game's rule (a credit per rank reached) stands in for it.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
}
namespace dice::hfe::world {
class ObjectTemplateManager;
}

namespace dice::hfe::stats {

// --- Ranks

class RankTable {
public:
	// The table of the shipped RankSettings.con until setRank changes it.
	RankTable();
	void setRank(int rank, int points);
	void clear() { m_points.clear(); }
	// The points the rank starts at (0 for rank 0 and below; ranks past
	// the table's last need its last value).
	int points(int rank) const;
	// The highest rank `points` reach (ranks listed with 0 points past
	// rank 0 are not reached by points).
	int rankFor(int points) const;
	// The highest rank reached by points (40).
	int maxRank() const;
	// The way from the rank's points to the next rank's, 0..1 (1 at the top).
	float progress(int points) const;

private:
	std::map<int, int> m_points;
};

// The table the game uses (rankSettings.setRank changes this one).
RankTable& ranks();
// rankSettings.setRank <rank> <points> into `table`.
void registerRankConsoleMethods(Console& console, RankTable& table = ranks());

// --- Unlocks

enum UnlockStatus : int {
	kUnlockLocked = 0,
	kUnlockTaken = 1,
	kUnlockAvailable = 2,
};

// selectUnlock's answers (the menu's selectUnlockDone): 0 done, 1 not
// available, 302 invalid (or Northern Strike without the booster), 303
// selected already, 304 no credits left. -1 would be "still asking".
enum SelectResult : int {
	kSelectDone = 0,
	kSelectUnavailable = 1,
	kSelectInvalid = 302,
	kSelectAlreadySelected = 303,
	kSelectNoCredits = 304,
};

int unlockKit(int id);    // 1..5
int unlockBranch(int id); // 1..2
int unlockLevel(int id);  // 1..
// The levels of a kit's branch (5, or 6 and 4 for kit 5).
int branchLength(int kit, int branch);
bool isUnlockId(int id);
// Northern Strike's ten (0x1004e000: x15, x25 of kits 1..4, 516 and 524).
bool isBoosterUnlock(int id);
// Every id, in the menu's order (kit, branch, level); 40, or 50 with the booster.
std::vector<int> unlockIds(bool booster);

// --- A soldier

struct SoldierStats {
	std::string name;
	int experience = 0; // the global score
	int kills = 0;
	int deaths = 0;
	int rounds = 0;
	int wins = 0;
	int losses = 0;
	double timePlayed = 0; // seconds
	int bestRoundScore = 0;
	std::vector<int> unlocks; // chosen, in order
};

struct RoundResult {
	int score = 0; // the experience earned (negative counts as 0)
	int kills = 0;
	int deaths = 0;
	float seconds = 0;
	int outcome = 0; // 1 won, -1 lost, 0 a draw
};

// What the menu asks of a soldier (the stats server's answers, here
// from the soldier's own record).
class Soldier {
public:
	Soldier(SoldierStats& stats, const RankTable& ranks = stats::ranks(), bool booster = true)
		: m_stats(stats), m_ranks(ranks), m_booster(booster) {}

	int rank() const;
	int nextRankPoints() const;
	float rankProgress() const;
	// One credit per rank, less the unlocks chosen.
	int credits() const;
	int unlocksTotal() const;
	int unlocksSelected() const { return static_cast<int>(m_stats.unlocks.size()); }
	// The level owned in a branch (0 for none).
	int branchLevel(int kit, int branch) const;
	// 0x0045c030's table, then 0x1004e000: an available one shows as
	// locked without credits, Northern Strike ones without the booster.
	UnlockStatus status(int id) const;
	SelectResult select(int id);
	// Adds a round; true when the soldier reached a new rank.
	bool record(const RoundResult& round);

private:
	SoldierStats& m_stats;
	const RankTable& m_ranks;
	bool m_booster;
};

// --- The store: one file of every soldier's record.

class StatsStore {
public:
	// Reads `path` ("" keeps everything in memory only); written on save.
	void setFile(const std::string& path);
	const std::string& file() const { return m_file; }
	// <config dir>/bf2142-recomp/stats.txt (next to profiles.txt).
	static std::string defaultFile();
	// The soldier's record, a recruit's when new.
	SoldierStats& soldier(const std::string& name);
	const SoldierStats* find(const std::string& name) const;
	bool save() const;
	// The text form (one "soldier <name>" line, then "<field> <value>" lines).
	std::string write() const;
	void read(const std::string& text);

private:
	std::string m_file;
	std::map<std::string, SoldierStats> m_soldiers;
};

// --- Unlocks in the game: the kits' ItemContainers by unlock id.

struct UnlockContainer {
	int id = 0;
	std::string kit;       // the Kit template
	std::string container; // the ItemContainer template
};
// Every kit (a Kit template with unlockIndex) and its ItemContainer
// children with an unlockLevel the menu has (the level-15 replacements
// are not chosen).
std::vector<UnlockContainer> unlockContainers(const world::ObjectTemplateManager& templates);
// The ItemContainers the chosen unlocks give (all kits, both armies), for
// GameWorld::setUnlocks.
std::vector<std::string> containersFor(const std::vector<UnlockContainer>& all, const std::vector<int>& unlocks);

// --- The stats server's answers (BF2-style text), for when one can be
// asked. Not used: there is no account to ask with. 0x005752b0 reads them
// as tokens split at tabs and newlines:
//   O                       ok (anything else failed)
//   H <tab> key <tab> ...   a header
//   D <tab> value ...       its rows
//   $ <tab> <length> <tab> $  the end; without it the answer failed. The
//                           length is not checked by the client.
struct StatsResponse {
	bool ok = false;
	int errorCode = 0; // an "E <code>" answer's (the client ignores it)
	struct Block {
		std::vector<std::string> keys;
		std::vector<std::vector<std::string>> rows;
		// A row's value by key ("" when absent).
		std::string value(size_t row, const std::string& key) const;
	};
	std::vector<Block> blocks;
	std::vector<std::string> tokens; // all of them, for the position-based readers
	bool terminated = false;         // the "$" came
	int declaredLength = -1;         // the "$" line's, -1 without one
	bool complete() const { return terminated; }
};
StatsResponse parseStatsResponse(const std::string& text);
// getunlocksinfo.aspx: the unspent credits (Avcred, the 12th token) and
// the ids owned (each row's first value after it), read by position as
// 0x0045c030 does.
struct UnlocksInfo {
	int credits = 0;
	std::vector<int> unlocks;
};
UnlocksInfo parseUnlocksInfo(const StatsResponse& response);

// --- Awards (Menu/External/FlashMenu/awards.con, run at start-up by
// 0x0046a0c0; "awards.setData <id> <criterion>..." parsed by 0x004a4e50).
// 126 entries: badges 100-121 in levels _1.._3, medals 200-219, ribbons
// 300-323, pins 400-416. Each criterion is "<flags>,<critId>,<keys>,<target>":
// flag 1 a career (global) criterion, else one within a round; 4 shown as a
// check mark rather than a bar; 8 whole numbers ("1" for a target of 0);
// the keys the stats server's getplayerinfo.aspx keys whose values add up
// ("wlr" wins over losses; an award id: 1 when held); its text is
// AWARD_CRITERIA_<critId>.
struct AwardCriterion {
	bool global = false;
	bool check = false;
	bool whole = false;
	int textId = 0;
	std::vector<std::string> keys;
	float target = 0;
};
struct AwardDefinition {
	std::string id;   // "100_1", "200"
	int number = 0;   // 100..416
	int level = 0;    // badges 1..3, else 0
	std::vector<AwardCriterion> criteria;
	// 0 medal, 1 badge, 2 ribbon, 3 pin (SwiffPlayer's record +0, by the number)
	int kind() const { return number < 200 ? 1 : number < 300 ? 0 : number < 400 ? 2 : 3; }
};
std::vector<AwardDefinition> parseAwards(const std::string& script);
// A stat key's value from the local record: the getplayerinfo.aspx keys it
// keeps (klls, dths, gsco, wins, lose, tt, and wlr); 0 for the rest
// (per kit, weapon, vehicle and map stats aren't kept).
double awardStatValue(const SoldierStats& soldier, const std::string& key);

} // namespace dice::hfe::stats
