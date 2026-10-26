// gpm_ti.py on Minsk (needs $BF2142_GAME_DIR): the Titans' spawn sets the
// tickets from their armour (VehicleSpawn), each hit takes them down
// (VehicleDamage: ceil(100 x hit points / maxHitPoints)), and a Titan's
// destruction (VehicleDestroyed) ends the round for the other team.
#include "Game/GameLogic.h"
#include "Game/ScriptBridge.h"
#include "Python/ScriptHost.h"
#include "TitanLevel.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace dice::hfe;

TEST(ScriptHost_TitanModeTicketsAndTheEnd)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	titantest::TitanLevel level(gameDir, "Minsk", 48, false);
	CHECK(level.loaded);
	if (!level.loaded) {
		return;
	}
	game::GameLogic logic;
	logic.mapSize = 48;
	game::ServerSettings settings;
	settings.gameMode = "gpm_ti";
	settings.mapName = "Minsk";
	game::ScriptBridge bridge(*level.world, logic, settings, "mods/bf2142");
	std::string errors;
	script::ScriptHost host(bridge, {std::string(gameDir) + "/python", std::string(gameDir) + "/mods/bf2142/python"},
		[&errors](const std::string& line) {
			if (line.compare(0, 7, "stderr:") == 0) {
				errors += line + "\n";
			}
		});
	CHECK(host.ok());
	const auto raise = [&host](const std::string& event, const game::ScriptArgs& args) { host.raise(event, args); };
	level.sink = raise;
	logic.setEventSink(raise);
	int winner = -1, victory = -1;
	logic.onEndGame = [&](int team, int type) {
		winner = team;
		victory = type;
	};
	level.afterTick = [&] {
		logic.tick(1 / 30.0f);
		host.update();
	};
	level.world->startRound();
	logic.startRound();
	CHECK(host.startGameMode("gpm_ti"));
	host.setGameStatus(game::GameStatus::PreGame);
	host.setGameStatus(game::GameStatus::Playing);
	level.run(0.5f);
	CHECK(level.world->titans().size() == 2);
	std::fprintf(stderr, "  tickets after the Titans' spawn: %.0f / %.0f\n", logic.team(1).tickets, logic.team(2).tickets);
	CHECK(logic.team(1).tickets == 100 && logic.team(2).tickets == 100);

	// A silo missile's worth on EU's Titan: 98.17 % left, 99 tickets.
	const uint32_t eu = level.world->titanOf(2);
	const int pac = level.world->addPlayer("Pac", 1, true);
	CHECK(level.world->spawnSoldier(pac));
	level.world->applyDamage(eu, 110000, pac);
	level.run(0.1f);
	CHECK(logic.team(2).tickets == 99);
	CHECK(logic.team(1).tickets == 100);
	// Down to the hull's 40 %: 40 tickets (the HUD's hull bar spans 0-60).
	level.world->applyDamage(eu, level.world->hitPoints(eu) - 6e6f * 0.4f, pac);
	level.run(0.1f);
	CHECK(logic.team(2).tickets == 40);
	CHECK(!logic.ended());
	// The rest: VehicleDestroyed; the scripts end the round, PAC winning
	// (sgl_endGame(winner, 3, 25)).
	level.world->applyDamage(eu, 1e7f, pac);
	level.run(0.1f);
	std::fprintf(stderr, "  EU's Titan destroyed: tickets %.0f, the round %s, winner %d, victory type %d\n",
		logic.team(2).tickets, logic.ended() ? "over" : "running", winner, victory);
	CHECK(logic.team(2).tickets == 0);
	CHECK(logic.ended() && winner == 1 && victory == 3);
	if (!errors.empty()) {
		std::fprintf(stderr, "%s", errors.c_str());
	}
	CHECK(errors.empty());
}
