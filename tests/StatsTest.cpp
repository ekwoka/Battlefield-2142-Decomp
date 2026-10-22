// The soldiers' stats (Stats/Stats.h): ranks from RankSettings.con, the
// unlock rules, the store's file, the stats server's text format on
// made-up answers, the menu's Profile natives over the store and (with
// $BF2142_GAME_DIR) the kits' ItemContainers by unlock id.
#include "Console/Console.h"
#include "IO/File/FileManager.h"
#include "Main/EndOfRoundBindings.h"
#include "Main/MenuBindings.h"
#include "Stats/Stats.h"
#include "TestMain.h"
#include "UI/Player/Natives.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/WorldConsole.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace dice::hfe;

namespace {

bool contains(const std::vector<std::string>& list, const std::string& name)
{
	return std::find(list.begin(), list.end(), name) != list.end();
}

} // namespace

TEST(Stats_Ranks)
{
	stats::RankTable table;
	CHECK(table.points(0) == 0 && table.points(1) == 40 && table.points(28) == 22100);
	// Ranks 29 and up as RankSettings.con has them.
	CHECK(table.points(29) == 24200 && table.points(35) == 40200 && table.points(40) == 57700);
	CHECK(table.points(41) == 57700); // past the table: its last
	CHECK(table.maxRank() == 40);
	CHECK(table.rankFor(0) == 0 && table.rankFor(39) == 0 && table.rankFor(40) == 1 && table.rankFor(79) == 1);
	CHECK(table.rankFor(57700) == 40 && table.rankFor(1000000) == 40);
	CHECK(table.progress(60) == 0.5f && table.progress(57700) == 1.0f);

	// rankSettings.setRank, the zero-point leaderboard ranks skipped.
	Console console;
	stats::RankTable custom;
	custom.clear();
	stats::registerRankConsoleMethods(console, custom);
	std::string out;
	for (const char* line : {"rankSettings.setRank 0 0", "rankSettings.setRank 1 10", "rankSettings.setRank 2 30",
			 "rankSettings.setRank 3 0"}) {
		CHECK(console.execute(line, out, ConsoleAccess::Script) == ConsoleResult::Ok);
	}
	CHECK(custom.maxRank() == 2 && custom.rankFor(29) == 1 && custom.rankFor(5000) == 2);
	CHECK(custom.points(3) == 30);

	// The end-of-round movie uses the game's table.
	CHECK(menu::rankExperience(29) == 24200 && menu::rankExperience(40) == 57700);
}

TEST(Stats_UnlockIds)
{
	CHECK(stats::unlockIds(false).size() == 40 && stats::unlockIds(true).size() == 50);
	CHECK(stats::isUnlockId(111) && stats::isUnlockId(425) && stats::isUnlockId(516) && stats::isUnlockId(524));
	CHECK(!stats::isUnlockId(126) && !stats::isUnlockId(517) && !stats::isUnlockId(525) && !stats::isUnlockId(110)
		&& !stats::isUnlockId(611) && !stats::isUnlockId(131));
	CHECK(stats::isBoosterUnlock(115) && stats::isBoosterUnlock(425) && stats::isBoosterUnlock(516)
		&& stats::isBoosterUnlock(524));
	CHECK(!stats::isBoosterUnlock(114) && !stats::isBoosterUnlock(515) && !stats::isBoosterUnlock(523));
}

TEST(Stats_Unlocks)
{
	stats::SoldierStats record;
	stats::Soldier soldier(record);
	// A recruit: no credits, the first levels locked until there are some.
	CHECK(soldier.rank() == 0 && soldier.credits() == 0);
	CHECK(soldier.status(111) == stats::kUnlockLocked);
	CHECK(soldier.select(111) == stats::kSelectNoCredits);

	// Three rounds: rank 3 (120 points), three credits.
	CHECK(soldier.record({50, 5, 2, 600, 1}));
	CHECK(!soldier.record({20, 1, 4, 300, -1}));
	CHECK(soldier.record({60, 3, 3, 900, 0}));
	CHECK(record.experience == 130 && record.rounds == 3 && record.wins == 1 && record.losses == 1);
	CHECK(record.kills == 9 && record.deaths == 9 && record.timePlayed == 1800 && record.bestRoundScore == 60);
	CHECK(soldier.rank() == 3 && soldier.credits() == 3 && soldier.nextRankPoints() == 200);
	CHECK(soldier.status(111) == stats::kUnlockAvailable && soldier.status(112) == stats::kUnlockLocked);

	// A branch in order.
	CHECK(soldier.select(112) == stats::kSelectUnavailable);
	CHECK(soldier.select(111) == stats::kSelectDone);
	CHECK(soldier.select(111) == stats::kSelectAlreadySelected);
	CHECK(soldier.status(111) == stats::kUnlockTaken && soldier.status(112) == stats::kUnlockAvailable);
	CHECK(soldier.select(112) == stats::kSelectDone);
	CHECK(soldier.select(511) == stats::kSelectDone);
	CHECK(soldier.credits() == 0 && soldier.unlocksSelected() == 3);
	// No credits left: the next ones show as locked.
	CHECK(soldier.status(113) == stats::kUnlockLocked && soldier.select(113) == stats::kSelectNoCredits);
	CHECK(soldier.select(999) == stats::kSelectInvalid);

	// Northern Strike's need the booster.
	record.experience = 57700;
	record.unlocks = {111, 112, 113, 114};
	CHECK(soldier.status(115) == stats::kUnlockAvailable);
	stats::Soldier noBooster(record, stats::ranks(), false);
	CHECK(noBooster.unlocksTotal() == 40 && soldier.unlocksTotal() == 50);
	CHECK(noBooster.status(115) == stats::kUnlockLocked && noBooster.select(115) == stats::kSelectInvalid);
}

TEST(Stats_Store)
{
	const std::filesystem::path file = std::filesystem::temp_directory_path() / "bf2142_stats_test" / "stats.txt";
	std::filesystem::remove(file);
	{
		stats::StatsStore store;
		store.setFile(file.string());
		CHECK(store.find("Ghost") == nullptr);
		stats::SoldierStats& ghost = store.soldier("Ghost");
		ghost.experience = 1234;
		ghost.kills = 50;
		ghost.timePlayed = 3600;
		ghost.unlocks = {111, 211, 511};
		store.soldier("Some Body").rounds = 2; // names with spaces
		CHECK(store.save());
	}
	stats::StatsStore store;
	store.setFile(file.string());
	const stats::SoldierStats* ghost = store.find("Ghost");
	CHECK(ghost && ghost->experience == 1234 && ghost->kills == 50 && ghost->timePlayed == 3600);
	CHECK(ghost && ghost->unlocks == std::vector<int>({111, 211, 511}));
	CHECK(store.find("Some Body") && store.find("Some Body")->rounds == 2);
	std::filesystem::remove_all(file.parent_path());
}

TEST(Stats_ServerResponse)
{
	// A made-up getunlocksinfo.aspx answer in the BF2 text format.
	const std::string text = "O\nH\tpid\tnick\tasof\nD\t12345\tGhost\t1190000000\nH\tAvcred\nD\t2\n"
							 "H\tUnlockID\nD\t111\nD\t112\nD\t211\nD\t999\n$\t68\t$\n";
	const stats::StatsResponse response = stats::parseStatsResponse(text);
	CHECK(response.ok && response.blocks.size() == 3);
	CHECK(response.blocks[0].value(0, "nick") == "Ghost" && response.blocks[0].value(0, "pid") == "12345");
	CHECK(response.blocks[0].value(0, "nothing").empty() && response.blocks[0].value(1, "pid").empty());
	CHECK(response.declaredLength == 68 && response.complete());
	CHECK(response.tokens.size() > 12 && response.tokens[6] == "12345" && response.tokens[12] == "2");
	const stats::UnlocksInfo info = stats::parseUnlocksInfo(response);
	CHECK(info.credits == 2 && info.unlocks == std::vector<int>({111, 112, 211}));

	const stats::StatsResponse error = stats::parseStatsResponse("E\t104\nH\tasof\terr\nD\t1\tNo player\n$\t20\t$\n");
	CHECK(!error.ok && error.errorCode == 104);
	CHECK(!stats::parseStatsResponse("<html>").ok);
	CHECK(!stats::parseStatsResponse("O\nH\tpid\nD\t1\n").complete()); // cut off
	// The length after "$" is not checked (0x005752b0), and runs of tabs
	// and newlines make no empty fields.
	CHECK(stats::parseStatsResponse("O\n\nH\tpid\t\tnick\nD\t7\tX\n$\t999\t$\n").complete());
	CHECK(stats::parseStatsResponse("O\n\nH\tpid\t\tnick\nD\t7\tX\n$\t999\t$\n").blocks[0].value(0, "nick") == "X");
	// The unlocks are read by position (0x0045c030): the 12th token is the
	// credits whatever its key, and a cut-off answer gives nothing.
	const stats::UnlocksInfo renamed = stats::parseUnlocksInfo(stats::parseStatsResponse(
		"O\nH\tpid\tnick\tasof\nD\t5\tGhost\t1\nH\tcredits\nD\t4\nH\tid\nD\t311\n$\t1\t$\n"));
	CHECK(renamed.credits == 4 && renamed.unlocks == std::vector<int>({311}));
	CHECK(stats::parseUnlocksInfo(stats::parseStatsResponse(text.substr(0, text.size() - 7))).unlocks.empty());
}

TEST(Stats_ProfileNatives)
{
	ui::NativeRegistry natives;
	natives.setLocalizer([](const std::string& key) { return key; });
	menu::MenuServices services;
	menu::MenuBindings bindings(natives, services);
	ui::avm1::Interpreter vm;
	const auto call = [&](const char* member, std::vector<ui::avm1::Value> args = {}) {
		return natives.call(vm, "dice.bf2.Profile", member, args);
	};
	// No soldier: a recruit, no unlock info.
	CHECK(vm.toNumber(call("getCurrentRankId")) == 0 && vm.toNumber(call("getNextRankExp")) == 40);
	CHECK(vm.toNumber(call("getUnlockInfo", {ui::avm1::Value(111)})) == -1);

	// An offline soldier with 130 points (rank 3).
	CHECK(ui::avm1::toBoolean(natives.call(vm, "dice.bf2.Logic", "setOfflineMode", {ui::avm1::Value("Ghost")})));
	call("connectToSubAccount", {ui::avm1::Value("Ghost")});
	CHECK(bindings.soldierName() == "Ghost");
	menu::RoundSummary summary;
	summary.validRound = true;
	summary.localPlayer = 0;
	summary.myTeam = 2;
	summary.winningTeam = 2;
	summary.score = 130;
	summary.kills = 12;
	summary.roundSeconds = 1200;
	bindings.recordRound(summary);
	CHECK(summary.rank == 3 && summary.rankedUp && summary.totalExp == 130 && summary.roundExp == 130);
	CHECK(bindings.stats().find("Ghost")->wins == 1);
	CHECK(vm.toNumber(natives.call(vm, "dice.bf2.PlayerStats", "getRankNotification", {})) == 1);
	CHECK(vm.toNumber(natives.call(vm, "dice.bf2.PlayerStats", "getRankNotification", {})) == 0);
	CHECK(vm.toNumber(call("getCurrentRankId")) == 3 && vm.toNumber(call("getCurrentExp")) == 130);
	CHECK(vm.toNumber(call("getNextRankExp")) == 200 && vm.toNumber(call("getRankProgress")) == 10.0 / 80.0);
	CHECK(vm.toString(call("getCurrentRankString")) == "RANK_3" && vm.toString(call("getNextRankString")) == "RANK_4");
	CHECK(vm.toNumber(call("getUnlocksCountUnselected")) == 3 && vm.toNumber(call("getUnlocksCountTotal")) == 50);

	// The unlock page: select (newRequest true) answers at once.
	CHECK(vm.toNumber(call("getUnlockInfo", {ui::avm1::Value(211)})) == 2);
	CHECK(vm.toNumber(call("selectUnlock", {ui::avm1::Value("211"), ui::avm1::Value(true)})) == 0);
	CHECK(vm.toNumber(call("selectUnlock", {ui::avm1::Value("211"), ui::avm1::Value(true)})) == 303);
	CHECK(vm.toNumber(call("selectUnlock", {ui::avm1::Value("213"), ui::avm1::Value(true)})) == 1);
	CHECK(vm.toNumber(call("getUnlockInfo", {ui::avm1::Value(211)})) == 1);
	CHECK(vm.toNumber(call("getUnlocksCountSelected")) == 1 && vm.toNumber(call("getUnlocksCountUnselected")) == 2);
	CHECK(vm.toNumber(call("getUnlocksProgress")) == 1.0 / 50.0);
	CHECK(bindings.soldierUnlocks() == std::vector<int>({211}));
}

TEST(Stats_KitContainers)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/bf2142");
	CHECK(files.mountArchive("Objects/Kits_server.zip", "Objects\\Kits"));
	Console console(&files);
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(6, "Kit");
	templates.registerType(7, "ItemContainer");
	world::registerWorldConsoleMethods(console, templates, objects);
	world::registerUntypedTemplateProperties(console, templates, {"unlockIndex", "unlockLevel", "unlockBranch"});
	std::string out;
	for (const char* army : {"EU", "Pac"}) {
		for (const char* kit : {"Recon", "Assault", "Anti-Vehicle", "Support"}) {
			console.run(std::string("Objects/Kits/") + army + "/" + army + "_" + kit + ".con", {}, out);
		}
	}
	const std::vector<stats::UnlockContainer> all = stats::unlockContainers(templates);
	// Every id in every kit of both armies: 4 kits x 10 own + 10 shared, twice.
	CHECK(all.size() == 2 * 4 * 20);
	for (const int id : stats::unlockIds(true)) {
		const auto count = std::count_if(all.begin(), all.end(), [id](const stats::UnlockContainer& c) { return c.id == id; });
		CHECK(count == (id >= 500 ? 8 : 2));
	}
	const std::vector<std::string> chosen = stats::containersFor(all, {111, 112, 221, 511, 524});
	CHECK(contains(chosen, "UnlockEU_ReconSni0") && contains(chosen, "UnlockEU_ReconSni1"));
	CHECK(contains(chosen, "UnlockEU_AssaultMedic0"));
	CHECK(contains(chosen, "UnlockEU_SupportPlayer0") && contains(chosen, "UnlockEU_Anti-VehicleSquadLeader3"));
	CHECK(!contains(chosen, "UnlockEU_ReconSpecOp0") && !contains(chosen, "UnlockEU_ReconDogTagReplacement"));
	CHECK(chosen.size() == 2 * (2 + 1 + 4 + 4));
	CHECK(std::any_of(chosen.begin(), chosen.end(), [](const std::string& c) { return c.rfind("UnlockPAC", 0) == 0 || c.rfind("UnlockPac", 0) == 0; }));
}
