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

#include <cstdio>
#include <cstdlib>
#include <string>

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
