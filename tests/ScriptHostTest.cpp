// The game's Python scripts on the embedded interpreter: Python 2 code
// translated on import, the host module, and the conquest game mode
// capturing a control point (needs $BF2142_GAME_DIR for the scripts).
#include "Game/GameLogic.h"
#include "Game/GameWorld.h"
#include "Game/ScriptBridge.h"
#include "Python/ScriptHost.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace dice::hfe;

namespace {

struct Round {
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	game::GameLogic logic;
	std::unique_ptr<game::GameWorld> world;
	std::unique_ptr<game::ScriptBridge> bridge;
	world::Object* enemyPoint = nullptr;

	Round()
	{
		templates.registerType(1, "ControlPoint");
		templates.registerType(2, "SpawnPoint");
		templates.registerType(3, "Soldier");
		templates.registerType(4, "Kit");
		templates.create("Kit", "EU_Assault");
		templates.create("Kit", "Pac_Assault");
		templates.registerType(5, "GenericFireArm");
		templates.create("GenericFireArm", "eu_ar_rifle");
		templates.create("GenericFireArm", "as_ar_rifle");
		for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
			templates.create("Soldier", name)->settings()["armor.maxHitPoints"] = "100";
		}
		world::ObjectTemplate* home = templates.create("ControlPoint", "cp_eu_base");
		home->settings()["controlPointId"] = "1";
		home->settings()["team"] = "2";
		home->settings()["unableToChangeTeam"] = "1";
		home->settings()["radius"] = "10";
		home->settings()["areaValueTeam2"] = "50";
		world::ObjectTemplate* enemy = templates.create("ControlPoint", "cp_harbor");
		enemy->settings()["controlPointId"] = "2";
		enemy->settings()["team"] = "1";
		enemy->settings()["radius"] = "10";
		enemy->settings()["timeToGetControl"] = "2";
		enemy->settings()["timeToLoseControl"] = "2";
		enemy->settings()["areaValueTeam1"] = "50";
		enemy->settings()["areaValueTeam2"] = "50";
		world::ObjectTemplate* pacHome = templates.create("ControlPoint", "cp_pac_base");
		pacHome->settings()["controlPointId"] = "3";
		pacHome->settings()["team"] = "1";
		pacHome->settings()["unableToChangeTeam"] = "1";
		pacHome->settings()["radius"] = "10";
		world::ObjectTemplate* spawn = templates.create("SpawnPoint", "sp_harbor");
		objects.create(home, {-500, 0, 0});
		enemyPoint = objects.create(enemy, {0, 0, 0});
		objects.create(pacHome, {500, 0, 0});
		// An EU spawn point (of the EU base) right at the harbour.
		objects.create(spawn, {3, 0, 0})->setControlPointId(1);
		// A PAC one at its base.
		objects.create(spawn, {503, 0, 0})->setControlPointId(3);

		for (int team = 1; team <= 2; ++team) {
			logic.team(team).defaultTickets[32] = 100;
			logic.team(team).ticketLossPerMin = 30;
		}
		world = std::make_unique<game::GameWorld>(templates, objects);
		world->startRound();
		logic.startRound();
		game::ServerSettings settings;
		settings.mapName = "test";
		bridge = std::make_unique<game::ScriptBridge>(*world, logic, settings, "mods/bf2142");
	}
};

} // namespace

TEST(ScriptHost_ConquestCapturesAControlPoint)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Round round;
	std::string errors;
	script::ScriptHost host(*round.bridge,
		{std::string(gameDir) + "/python", std::string(gameDir) + "/mods/bf2142/python"},
		[&errors](const std::string& line) {
			if (line.compare(0, 7, "stderr:") == 0) {
				errors += line + "\n";
			}
		});
	CHECK(host.ok());
	const auto raise = [&host](const std::string& event, const game::ScriptArgs& args) { host.raise(event, args); };
	round.world->setEventSink(raise);
	round.logic.setEventSink(raise);
	CHECK(host.startGameMode("gpm_cq"));
	host.setGameStatus(game::GameStatus::PreGame);
	host.setGameStatus(game::GameStatus::Playing);
	CHECK(round.logic.team(1).tickets == 100 && round.logic.team(2).tickets == 100);

	// The EU soldier appears inside PAC's harbour point: its flag comes down
	// (2 s), the point turns neutral, then EU's flag goes up (2 s).
	CHECK(round.world->spawnPlayer(2) != nullptr);
	int neutralAt = -1, capturedAt = -1;
	for (int tick = 0; tick < 30 * 8; ++tick) {
		round.world->tick(1 / 30.0f);
		round.logic.tick(1 / 30.0f);
		host.update();
		const int team = round.enemyPoint->team();
		if (team == 0 && neutralAt < 0) {
			neutralAt = tick;
		}
		if (team == 2 && capturedAt < 0) {
			capturedAt = tick;
		}
	}
	std::fprintf(stderr, "  neutral after %.2f s, captured after %.2f s; tickets %.1f / %.1f\n", neutralAt / 30.0f,
		capturedAt / 30.0f, round.logic.team(1).tickets, round.logic.team(2).tickets);
	if (!errors.empty()) {
		std::fprintf(stderr, "%s", errors.c_str());
	}
	CHECK(errors.empty());
	CHECK(neutralAt > 50 && neutralAt < 70);   // about 2 s
	CHECK(capturedAt > 110 && capturedAt < 130); // about 4 s
	// EU holds more area now: PAC's tickets bleed.
	CHECK(round.logic.team(1).tickets < 100);
	CHECK(round.logic.team(2).tickets == 100);

	// A PAC bot joins and spawns; killing its soldier runs the scripts'
	// death handlers (scoring, a ticket for the death), and it comes back
	// after the spawn time.
	const int bot = round.world->addPlayer("Bot", 1, true);
	CHECK(round.world->spawnSoldier(bot));
	game::GameWorld::PlayerInfo info;
	CHECK(round.world->playerInfo(bot, info) && info.alive && info.soldier != 0 && info.ai);
	const float ticketsBefore = round.logic.team(1).tickets;
	round.world->setSpawnTime(1);
	round.world->setHitPoints(info.soldier, 0);
	CHECK(round.world->playerInfo(bot, info) && !info.alive && info.deaths == 1 && info.respawnIn == 1 && info.manDown);
	host.update();
	CHECK(round.logic.team(1).tickets == ticketsBefore); // down, not dead yet
	for (int tick = 0; tick < 40; ++tick) {
		round.world->tick(1 / 30.0f);
		round.logic.tick(1 / 30.0f);
		host.update();
	}
	CHECK(round.logic.team(1).tickets < ticketsBefore); // PlayerDeath on respawning
	CHECK(round.world->playerInfo(bot, info) && info.alive && info.soldier != 0);
	CHECK(round.world->playerList().size() == 2);
	if (!errors.empty()) {
		std::fprintf(stderr, "%s", errors.c_str());
	}
	CHECK(errors.empty());
}

// Conquest's ticket limit 1 is 0 (bf2.GameLogic.setTicketLimit(team, id,
// value)): a team out of tickets ends the round, the other team winning.
TEST(ScriptHost_ConquestEndsWithoutTickets)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Round round;
	script::ScriptHost host(*round.bridge,
		{std::string(gameDir) + "/python", std::string(gameDir) + "/mods/bf2142/python"}, [](const std::string&) {});
	const auto raise = [&host](const std::string& event, const game::ScriptArgs& args) { host.raise(event, args); };
	round.world->setEventSink(raise);
	round.logic.setEventSink(raise);
	int winner = -1;
	round.logic.onEndGame = [&winner](int team, int) { winner = team; };
	CHECK(host.startGameMode("gpm_cq"));
	host.setGameStatus(game::GameStatus::PreGame);
	host.setGameStatus(game::GameStatus::Playing);
	CHECK(round.logic.team(1).ticketLimits.count(1) == 1 && round.logic.team(1).ticketLimits[1] == 0);
	round.logic.setTickets(1, 0);
	host.update();
	CHECK(round.logic.ended());
	CHECK(winner == 2);
}

// Assault Lines (gpm_sl.py): a control point's supply groups
// (supplyGroupNeeded <team> <group> <optional>) gate its capture. The
// harbour needs team 2 (EU) to hold group 3 (PAC's base) first, so the EU
// soldier inside it captures nothing; once its need is met, it does.
TEST(ScriptHost_SupplyLinesGateCapture)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	for (const bool supplied : {false, true}) {
		Round round;
		world::ObjectTemplate* harbour = round.templates.find("cp_harbor");
		harbour->settings()["supplyGroupId"] = "2";
		harbour->settings()["supplyGroupNeeded.list"] = supplied ? "2 1 0;1 3 1;" : "2 3 0;";
		round.templates.find("cp_eu_base")->settings()["supplyGroupId"] = "1";
		round.templates.find("cp_pac_base")->settings()["supplyGroupId"] = "3";
		round.world->startRound();
		std::string errors;
		script::ScriptHost host(*round.bridge,
			{std::string(gameDir) + "/python", std::string(gameDir) + "/mods/bf2142/python"},
			[&errors](const std::string& line) {
				if (line.compare(0, 7, "stderr:") == 0) {
					errors += line + "\n";
				}
			});
		const auto raise = [&host](const std::string& event, const game::ScriptArgs& args) { host.raise(event, args); };
		round.world->setEventSink(raise);
		round.logic.setEventSink(raise);
		CHECK(host.startGameMode("gpm_sl"));
		host.setGameStatus(game::GameStatus::PreGame);
		host.setGameStatus(game::GameStatus::Playing);
		// What the script sees (0x0056a010, 0x00568890).
		CHECK(host.run(std::string("import host, bf2\n"
								   "cp = [o for o in bf2.objectManager.getObjectsOfType('dice.hfe.world.ObjectTemplate.ControlPoint')"
								   " if o.templateName == 'cp_harbor'][0]\n"
								   "assert cp.cp_getParam('supplyGroupId') == 2 and cp.getSupplyGroupId() == 2\n"
								   "assert [o.templateName for o in host.sgl_getControlPointsInGroup(3)] == ['cp_pac_base']\n"
								   "assert len(host.sgl_getControlPointsInGroup(7)) == 0\n")
			+ (supplied ? "assert list(cp.cp_getParam('supplyGroupsNeeded', 2)) == [[1, 0]]\n"
						  "assert list(cp.cp_getParam('supplyGroupsNeeded', 1)) == [[3, 1]]\n"
						  "assert cp.cp_getParam('allowCaptureByTeam', 2) == 1\n"
						: "assert list(cp.cp_getParam('supplyGroupsNeeded', 2)) == [[3, 0]]\n"
						  "assert cp.cp_getParam('allowCaptureByTeam', 2) == 0\n"
						  "assert cp.cp_getParam('allowCaptureByTeam', 1) == 1\n")));
		CHECK(round.world->spawnPlayer(2) != nullptr);
		for (int tick = 0; tick < 30 * 8; ++tick) {
			round.world->tick(1 / 30.0f);
			round.logic.tick(1 / 30.0f);
			host.update();
		}
		std::fprintf(stderr, "  supplied %d: harbour team %d\n", supplied ? 1 : 0, round.enemyPoint->team());
		CHECK(round.enemyPoint->team() == (supplied ? 2 : 1));
		if (!errors.empty()) {
			std::fprintf(stderr, "%s", errors.c_str());
		}
		CHECK(errors.empty());
	}
}

// The squad and score hooks: pmgr_getSquadLeader / getPlayersInSquad
// (0x0056a570, 0x0056adb0), pmgr_enableScoreEvents raising PlayerScore
// (0x0056ad00, 0x0056ae60), pers_plrGiveFieldUnlock (0x00566850),
// gl_sendEndOfRoundData (0x00568930), and the stats server calls failing
// as with no server.
TEST(ScriptHost_SquadAndStatsHooks)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Round round;
	script::ScriptHost host(*round.bridge,
		{std::string(gameDir) + "/python", std::string(gameDir) + "/mods/bf2142/python"}, [](const std::string&) {});
	std::vector<std::string> events;
	const auto raise = [&](const std::string& event, const game::ScriptArgs& args) {
		events.push_back(event);
		host.raise(event, args);
	};
	round.world->setEventSink(raise);
	round.logic.setEventSink(raise);
	round.bridge->events = raise;
	CHECK(host.startGameMode("gpm_cq"));
	const int a = round.world->addPlayer("A", 2, true);
	const int b = round.world->addPlayer("B", 2, true);
	const int c = round.world->addPlayer("C", 1, true);
	CHECK(round.world->joinSquad(a, 3) && round.world->joinSquad(b, 3) && round.world->joinSquad(c, 3));
	CHECK(host.run("import host, bf2\n"
				   "assert host.pmgr_getSquadLeader(2, 3) == " + std::to_string(a) + "\n"
				   "assert host.pmgr_getSquadLeader(2, 4) == -1\n"
				   "assert sorted(host.pmgr_getPlayersInSquad(2, 3)) == sorted([" + std::to_string(a) + ", " + std::to_string(b) + "])\n"
				   "assert [p.index for p in bf2.playerManager.getPlayersInSquad(1, 3)] == [" + std::to_string(c) + "]\n"
				   "assert bf2.playerManager.getSquadLeader(2, 3).index == " + std::to_string(a) + "\n"
				   "host.pers_plrGiveFieldUnlock(" + std::to_string(b) + ")\n"
				   "assert host.pers_plrRequestStats(" + std::to_string(b) + ", 1, '&mode=base', 0) == 0\n"
				   "try:\n  host.pers_getStatsKeyVal('rnk', 0)\n  assert False\nexcept RuntimeError:\n  pass\n"
				   "host.pmgr_enableScoreEvents(1)\n"
				   "host.pmgr_setScore(" + std::to_string(b) + ", 'score', 5)\n"
				   "host.gl_sendEndOfRoundData('\\\\tp0\\\\B')\n"));
	CHECK(round.world->fieldUnlocks(b) == 1 && round.world->fieldUnlocks(a) == 0);
	CHECK(std::count(events.begin(), events.end(), "PlayerScore") == 1);
	CHECK(round.logic.endOfRoundData() == "\\tp0\\B" && round.logic.endOfRoundSerial() == 1);
}

// The players' ranks (stats +0x20, -1 from 0x005a5fc0): stats.py gives
// each 0 on PlayerConnect through pmgr_setScore "rank" (0x0056a9d0 →
// 0x005a53b0); rank.py asks the stats server (pers_plrRequestStats
// 0x00566930) for a player with a profile id above 2000 and sets the
// PlayerStatsResponse's "rnk" (pers_getStatsKeyVal 0x00566b90).
TEST(ScriptHost_RanksFromTheStatsServer)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Round round;
	script::ScriptHost host(*round.bridge,
		{std::string(gameDir) + "/python", std::string(gameDir) + "/mods/bf2142/python"}, [](const std::string&) {});
	const auto raise = [&](const std::string& event, const game::ScriptArgs& args) { host.raise(event, args); };
	round.world->setEventSink(raise);
	round.logic.setEventSink(raise);
	round.bridge->events = raise;
	// The local soldier's profile (asked inside addPlayer, as it connects).
	round.bridge->profileOf = [&round](int player) -> int64_t {
		game::GameWorld::PlayerInfo p;
		return round.world->playerInfo(player, p) && p.name == "Me" ? 0x10000123 : 0;
	};
	round.bridge->statsServer = [](int64_t profile, game::ScriptBridge::StatsRecord& record) {
		if (profile != 0x10000123) {
			return false;
		}
		record["rnk"] = "7";
		return true;
	};
	CHECK(host.startGameMode("gpm_cq"));
	const int bot = round.world->addPlayer("Bot", 1, true);
	const int me = round.world->addPlayer("Me", 2, false);
	game::GameWorld::PlayerInfo info;
	CHECK(round.world->playerInfo(bot, info) && info.rank == 0); // stats.py
	CHECK(round.world->playerInfo(me, info) && info.rank == 0);  // until the answer comes
	round.bridge->deliverStatsResponses();
	CHECK(round.world->playerInfo(me, info) && info.rank == 7);
	CHECK(round.world->playerInfo(bot, info) && info.rank == 0);
	CHECK(host.run("import host\nassert host.pmgr_getScore(" + std::to_string(me) + ", 'rank') == 7\n"
				   "assert host.pers_getStatsKeyVal('rnk', 0x10000123) == '7'\n"));
}

// Python 2's "/" inside a class (py2compat's division helper): a name
// with two leading underscores would be mangled there (ObjectStat's
// accuracy, bf2/stats/stats.py, which the end of round reads).
TEST(ScriptHost_DivisionInAClass)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	Round round;
	script::ScriptHost host(*round.bridge,
		{std::string(gameDir) + "/python", std::string(gameDir) + "/mods/bf2142/python"}, [](const std::string&) {});
	CHECK(host.startGameMode("gpm_cq"));
	CHECK(host.run("import bf2.stats.stats as stats\n"
				   "o = stats.ObjectStat(0)\n"
				   "o.bulletsFired = 4\n"
				   "o.bulletsHit = 1\n"
				   "assert abs(o.accuracy - 0.25) < 1e-9\n"
				   // Classic instances order (by address) as Python 2's do: the
				   // end of round sorts (score, PlayerStat) pairs.
				   "pairs = [((0, 0), stats.ObjectStat(1)), ((0, 0), stats.ObjectStat(2)), ((1, 0), o)]\n"
				   "pairs.sort()\n"
				   "assert pairs[-1][1] is o\n"
				   // A classic class's __cmp__ orders it.
				   "import py2compat\n"
				   "g = {'__builtins__': py2compat.PY2_BUILTINS, '__name__': 't'}\n"
				   "exec(compile(py2compat.translate('class Old:\\n"
				   "  def __init__(self, v): self.v = v\\n"
				   "  def __cmp__(self, other): return cmp(self.v, other.v)\\n"
				   "first = sorted([Old(3), Old(1), Old(2)])[0].v\\n'), 't', 'exec'), g)\n"
				   "assert g['first'] == 1\n"));
}
