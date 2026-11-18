// gpm_ti.py on Minsk (needs $BF2142_GAME_DIR): the Titans' spawn sets the
// tickets from their armour (VehicleSpawn), each hit takes them down
// (VehicleDamage: ceil(100 x hit points / maxHitPoints)), and a Titan's
// destruction (VehicleDestroyed) ends the round for the other team.
#include "Game/GameLogic.h"
#include "Game/ScriptBridge.h"
#include "Main/HudMenus.h"
#include "Python/ScriptHost.h"
#include "TitanLevel.h"
#include <algorithm>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace dice::hfe;

// A walker driver beside Port Bavaria's first silo (704, a 6 m sphere):
// its root is 6.6 m away in 3D (3 m up), but the original's trigger query
// takes an object whose sphere about its root touches the trigger's, so
// the walker is inside and captures, and the HUD's uplink is real.
TEST(ScriptHost_PortBavariaWalkerSiloCapture)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	titantest::TitanLevel level(gameDir, "Port_Bavaria", 48, false, nullptr, true);
	CHECK(level.loaded);
	if (!level.loaded) return;
	game::GameLogic logic;
	logic.mapSize = 48;
	game::ServerSettings settings;
	settings.gameMode = "gpm_ti";
	settings.mapName = "Port_Bavaria";
	game::ScriptBridge bridge(*level.world, logic, settings, "mods/bf2142");
	std::string errors;
	script::ScriptHost host(bridge, {std::string(gameDir) + "/python", std::string(gameDir) + "/mods/bf2142/python"},
		[&errors](const std::string& line) {
			if (line.compare(0, 7, "stderr:") == 0) errors += line + "\n";
		});
	level.sink = [&host](const std::string& event, const game::ScriptArgs& args) { host.raise(event, args); };
	logic.setEventSink(level.sink);
	level.afterTick = [&] { logic.tick(1.0f / 30); host.update(); };
	level.world->startRound();
	logic.startRound();
	CHECK(host.startGameMode("gpm_ti"));
	host.setGameStatus(game::GameStatus::PreGame);
	host.setGameStatus(game::GameStatus::Playing);
	level.run(0.5f);
	const auto silos = level.world->missileSilos();
	CHECK(!silos.empty());
	if (silos.empty()) return;
	game::ControlPoint* cp = nullptr;
	for (const auto& point : level.world->controlPoints()) {
		if (point.id == silos.front().controlPoint) cp = level.world->controlPointOf(point.object->id());
	}
	CHECK(cp);
	auto* walkerTemplate = level.templates.find("us_heavy_mech");
	CHECK(walkerTemplate);
	if (!cp || !walkerTemplate) return;
	const auto pos = cp->object->position();
	const float x = pos.x + 4, z = pos.z + 4;
	const float ground = level.world->terrainHeight(x, z);
	auto* walker = level.objects.create(walkerTemplate, {x, ground + 6, z});
	walker->setTeam(2);
	level.run(3);
	auto* soldier = level.world->spawnPlayer(2);
	CHECK(soldier);
	if (!soldier) return;
	const auto at = walker->position();
	CHECK(level.world->setSoldierPosition(soldier->id(), {at.x + 1.5f, ground, at.z}));
	game::SoldierInput use;
	use.use = true;
	level.world->setPlayerInput(use);
	level.world->tick(1.0f / 30);
	level.world->setPlayerInput({});
	game::GameWorld::PlayerInfo info;
	CHECK(level.world->playerInfo(level.world->localPlayer(), info));
	CHECK(info.vehicle == walker->id() && info.seat == 0);
	const auto occupants = bridge.objectCall(walker->id(), "getOccupyingPlayers", {});
	CHECK(occupants.kind == game::ScriptValue::Kind::List && occupants.list.size() == 1);
	level.run(21);
	CHECK(level.world->playerInfo(level.world->localPlayer(), info));
	const auto p = walker->position();
	const float distance = std::sqrt((p.x - pos.x) * (p.x - pos.x) + (p.y - pos.y) * (p.y - pos.y) + (p.z - pos.z) * (p.z - pos.z));
	const float walkerRadius = level.world->objectRadius(walker->id());
	const auto bar = menu::captureBar(level.world->controlPointList(), info.position, level.world->playerInsideControlPoint);
	menu::UplinkHud uplink;
	uplink.update(0.1f, bar.enabled, bar.value);
	std::fprintf(stderr, "  cp=%d hemi=%lld walker 3d=%.3f radius=%.2f inside=%d HUD=%d uplink=%d team=%d flag=%.3f\n", cp->id,
		static_cast<long long>(bridge.objectCall(cp->object->id(), "cp_getParam", {game::ScriptValue::string("isHemisphere")}).asInt()),
		distance, walkerRadius, level.world->playerInsideControlPoint, bar.enabled, uplink.show, cp->team, cp->flagHeight);
	CHECK(info.vehicle == walker->id() && distance > 6 && distance < 6 + walkerRadius);
	CHECK(level.world->playerInsideControlPoint && bar.enabled && uplink.show);
	CHECK(cp->team == 2);
	if (!errors.empty()) std::fprintf(stderr, "%s", errors.c_str());
	CHECK(errors.empty());
}

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
	std::vector<std::pair<int, int>> states; // the scripts' setTicketState: the shield/hull announcements
	logic.onTicketState = [&states](int team, int state) { states.push_back({team, state}); };
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
	for (const auto& [team, state] : states) {
		std::fprintf(stderr, "  ticket state: team %d -> %d\n", team, state);
	}
	// 99 (the shield's first damage) .. 48 (the hull at 75 %): states 12 .. 5.
	CHECK(std::find(states.begin(), states.end(), std::make_pair(2, 12)) != states.end());
	CHECK(!states.empty() && states.back() == std::make_pair(2, 5));
	CHECK(!logic.ended());
	// A player killed before the end: its handler asks every player's
	// vehicle (getClosestTitan(p.getVehicle(), ...) calls isValid on it);
	// the host's handle keeps the dead soldier (man down, then the corpse),
	// as BF2142.exe's weak handle does, so the script doesn't raise.
	const int dead = level.world->addPlayer("Dead", 2, true);
	CHECK(level.world->spawnSoldier(dead));
	game::GameWorld::PlayerInfo info;
	CHECK(level.world->playerInfo(dead, info));
	const uint32_t deadSoldier = info.soldier;
	level.world->applyDamage(deadSoldier, 1e5f, pac);
	level.run(0.1f);
	CHECK(level.world->playerInfo(dead, info) && !info.alive);
	CHECK(info.heldVehicle == deadSoldier && info.heldSoldier == 0);
	// The rest: VehicleDestroyed; the scripts end the round, PAC winning
	// (sgl_endGame(winner, 3, 25)) after the 25 s of the Titan's explosion:
	// the winner is known at once, the round goes on until then (0x004bbe30).
	level.world->applyDamage(eu, 1e7f, pac);
	level.run(0.1f);
	CHECK(logic.team(2).tickets == 0);
	CHECK(!logic.ended() && logic.endPending() && winner == -1);
	CHECK(logic.param("winner", 0, 0).asInt() == 1);
	level.run(24.5f);
	CHECK(!logic.ended());
	level.run(0.6f);
	std::fprintf(stderr, "  EU's Titan destroyed: tickets %.0f, the round %s, winner %d, victory type %d\n",
		logic.team(2).tickets, logic.ended() ? "over" : "running", winner, victory);
	CHECK(logic.ended() && winner == 1 && victory == 3);
	if (!errors.empty()) {
		std::fprintf(stderr, "%s", errors.c_str());
	}
	CHECK(errors.empty());
}

// A removed object (a dead soldier leaving a control point's trigger): the
// list methods the shipped gpm_cq/gpm_ti scripts iterate give an empty list,
// not None (their "for p in o.getOccupyingPlayers()" raised TypeError).
TEST(ScriptBridge_RemovedObjectListsAreEmpty)
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
	game::ServerSettings settings;
	game::ScriptBridge bridge(*level.world, logic, settings, "mods/bf2142");
	const uint32_t gone = 0x7ffffff0;
	CHECK(!level.world->objects().find(gone));
	for (const char* method : {"getOccupyingPlayers", "getChildren"}) {
		const game::ScriptValue v = bridge.objectCall(gone, method, {});
		CHECK(v.kind == game::ScriptValue::Kind::List && v.list.empty());
	}
	CHECK(bridge.objectCall(gone, "getTeam", {}).kind == game::ScriptValue::Kind::None);
}
