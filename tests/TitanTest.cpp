// Titan mode on the shipped levels (needs $BF2142_GAME_DIR): Minsk's and
// Minsk_Titan16's gpm_ti gameplay objects with every object template, the
// terrain, the collision meshes and the materials. The Titans spawn with
// their parts and spawn points, the missile silos fire at the enemy Titan
// and wear its shield and then its hull down, the core's sequence (four
// consoles, the doors, the core) destroys a Titan, the commander's order
// moves it, and gpm_ti.py keeps the tickets and ends the round.
#include "TitanLevel.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace dice::hfe;
using titantest::TitanLevel;


// Minsk gpm_ti 48: each team's Titan from its spawner (as_titan_playtest,
// eu_titan_playtest), with the armour, parts and spawn points of its tree.
TEST(Titan_MinskTitansSpawn)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Minsk", 48);
	CHECK(level.loaded);
	if (!level.loaded) {
		return;
	}
	CHECK(level.world->titans().size() == 2);
	for (int team = 1; team <= 2; ++team) {
		const auto* titan = level.titan(team);
		CHECK(titan != nullptr);
		if (!titan) {
			continue;
		}
		CHECK(titan->maxHitPoints == 6e6f && titan->hitPoints == 6e6f);
		CHECK(titan->shield && titan->canMove && !titan->wreck);
		CHECK(titan->consoles == 4 && titan->consolesLeft == 4 && !titan->coreExposed && !titan->coreDestroyed);
		CHECK(titan->position.y > 280); // in the air at its spawner (286.7)
		CHECK(titan->radius > 20);
		CHECK(level.world->isTitan(titan->object));
		CHECK(level.world->titanOf(team) == titan->object);
		CHECK(titan->controlPoint == (team == 1 ? 501 : 502));
		// Seven spawn points inside, of the Titan's control point.
		const auto spawns = level.world->titanSpawnPoints(team);
		CHECK(spawns.size() == 7);
		for (uint32_t id : spawns) {
			const world::Object* point = level.objects.find(id);
			CHECK(point && point->controlPointId() == titan->controlPoint && point->position().y > titan->position.y);
		}
		// The parts: four consoles, the doors, the core, the guns, the forcefield.
		int consoles = 0, cores = 0, doors = 0, guns = 0, shields = 0;
		for (const auto& part : level.world->titanParts(team)) {
			consoles += part.kind == game::GameWorld::TitanPartInfo::Console;
			cores += part.kind == game::GameWorld::TitanPartInfo::Core;
			doors += part.kind == game::GameWorld::TitanPartInfo::CoreDoors;
			guns += part.kind == game::GameWorld::TitanPartInfo::Gun;
			shields += part.kind == game::GameWorld::TitanPartInfo::Shield;
			if (part.kind == game::GameWorld::TitanPartInfo::Console) {
				CHECK(part.maxHitPoints == 7000);
			}
			if (part.kind == game::GameWorld::TitanPartInfo::Core) {
				CHECK(part.maxHitPoints == 40000);
			}
		}
		std::fprintf(stderr, "  titan %d: %d consoles, %d core, %d doors, %d guns, %d shield, radius %.1f\n", team, consoles,
			cores, doors, guns, shields, titan->radius);
		CHECK(consoles == 4 && cores == 1 && doors == 1 && guns == 6 && shields == 1);
	}
	// VehicleSpawn for both (the Python component's onSpawnEvent 28).
	CHECK(level.count("VehicleSpawn") == 2);
	// The five silos, neutral and loaded.
	CHECK(level.world->missileSilos().size() == 5);
	// A soldier spawns in a Titan when its spawn points are chosen, and
	// stands on its floor.
	const int bot = level.world->addPlayer("Bot", 2, true);
	level.world->setSpawnChoice(bot, 502);
	bool inside = false;
	for (int tries = 0; tries < 20 && !inside; ++tries) {
		game::GameWorld::PlayerInfo info;
		if (!level.world->playerInfo(bot, info) || !info.alive) {
			CHECK(level.world->spawnSoldier(bot)); // else respawned on its own (autoRespawn)
		}
		level.world->playerInfo(bot, info);
		inside = info.position.y > 280;
		if (!inside) {
			level.world->setHitPoints(info.soldier, 0);
			level.run(16);
		}
	}
	CHECK(inside);
	level.run(2);
	game::GameWorld::PlayerInfo info;
	CHECK(level.world->playerInfo(bot, info) && info.alive);
	const auto* eu = level.titan(2);
	std::fprintf(stderr, "  soldier in the Titan at %.1f (Titan at %.1f)\n", info.position.y, eu ? eu->position.y : 0.0f);
	CHECK(eu && info.position.y > eu->position.y - 15 && info.position.y < eu->position.y + 25); // on its decks, not fallen through
}

// A silo taken by PAC reloads (120 s, reloadOnTeamChange), fires its
// missile at EU's Titan, which hits it: 110000 damage (msilo_missile
// against veh_titan_armor, cell 1) off the shield; with the shield down
// (60 % and less) the next one comes off the hull, the forcefield is gone
// and the Titan stops.
TEST(Titan_SilosWearTheShieldThenTheHull)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Minsk", 48);
	if (!level.loaded) {
		return;
	}
	level.world->setControlPointTeam(503, 1); // Front Line's silo to PAC
	level.run(0.5f);
	auto silos = level.world->missileSilos();
	const auto silo = std::find_if(silos.begin(), silos.end(), [](const auto& s) { return s.controlPoint == 503; });
	CHECK(silo != silos.end() && silo->team == 1 && !silo->loaded && silo->reloadLeft > 118);
	const auto* eu = level.titan(2);
	CHECK(eu && eu->hitPoints == 6e6f);
	// Two minutes of reload, the launch delay, and the flight.
	float hitAt = -1;
	for (int second = 0; second < 180 && hitAt < 0; ++second) {
		level.run(1);
		if (level.titan(2)->hitPoints < 6e6f) {
			hitAt = static_cast<float>(second);
		}
	}
	silos = level.world->missileSilos();
	const auto fired = std::find_if(silos.begin(), silos.end(), [](const auto& s) { return s.controlPoint == 503; });
	std::fprintf(stderr, "  missile hit after %.0f s; EU Titan %.0f hit points (%.2f %%), %d launched\n", hitAt,
		level.titan(2)->hitPoints, level.titan(2)->percent, fired != silos.end() ? fired->launched : -1);
	CHECK(hitAt > 120 && hitAt < 170);
	CHECK(std::fabs(level.titan(2)->hitPoints - (6e6f - 110000)) < 1);
	CHECK(level.titan(2)->shield);
	CHECK(level.count("VehicleDamage") == 1);
	CHECK(fired != silos.end() && fired->launched == 1 && !fired->loaded);

	// The shield down: at 61 % and below the forcefield is destroyed
	// (destroyOnParentDamageLevel 61), at 60 % the Titan no longer moves.
	level.world->setHitPoints(eu->object, 6e6f * 0.59f);
	level.run(0.5f);
	const auto* hull = level.titan(2);
	CHECK(!hull->shield && !hull->canMove);
	bool forcefieldDown = false;
	for (const auto& part : level.world->titanParts(2)) {
		if (part.kind == game::GameWorld::TitanPartInfo::Shield) {
			forcefieldDown = part.wreck;
		}
	}
	CHECK(forcefieldDown);
	const float before = hull->hitPoints;
	float second = 0;
	for (; second < 180 && level.titan(2)->hitPoints >= before; ++second) {
		level.run(1);
	}
	std::fprintf(stderr, "  the next one off the hull after %.0f s: %.0f -> %.0f\n", second, before, level.titan(2)->hitPoints);
	CHECK(std::fabs(level.titan(2)->hitPoints - (before - 110000)) < 1);
	// Down to 0 by missiles alone (onlyDamageFromChildrenLevel 0 caps it
	// there): the Titan is a wreck, VehicleDestroyed.
	level.world->setHitPoints(eu->object, 50000);
	for (second = 0; second < 180 && !level.titan(2)->wreck; ++second) {
		level.run(1);
	}
	CHECK(level.titan(2)->wreck && level.titan(2)->hitPoints == 0);
	CHECK(level.destroyed(eu->object) == 1);
	// The core went with it (destroyOnParentDamageLevel 0).
	CHECK(level.titan(2)->coreDestroyed);
	CHECK(level.world->titanSpawnPoints(2).empty());
}

// The core's sequence through the game logic's damage: the four consoles
// (7000 each) destroyed take 2 each off the doors' 10 (parentDamageWreck
// 20 % of the doors' maxHitPoints); at 2, under their criticalDamage 3,
// the doors burn down at 1 a second and open (their wreck: 1 % of the
// core's 40000); the core's damage takes the Titan's share with it
// (parentDamageAlive 1), and its end is the Titan's end.
TEST(Titan_ConsolesDoorsAndCoreDestroyTheTitan)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Minsk", 48);
	if (!level.loaded) {
		return;
	}
	const auto* pac = level.titan(1);
	CHECK(pac != nullptr);
	if (!pac) {
		return;
	}
	const int attacker = level.world->addPlayer("Eu", 2, true);
	const int own = level.world->addPlayer("Pac", 1, true);
	uint32_t core = 0, doors = 0;
	std::vector<uint32_t> consoles;
	for (const auto& part : level.world->titanParts(1)) {
		if (part.kind == game::GameWorld::TitanPartInfo::Console) {
			consoles.push_back(part.id);
		} else if (part.kind == game::GameWorld::TitanPartInfo::Core) {
			core = part.id;
		} else if (part.kind == game::GameWorld::TitanPartInfo::CoreDoors) {
			doors = part.id;
		}
	}
	CHECK(consoles.size() == 4 && core && doors);
	if (consoles.size() != 4 || !core || !doors) {
		return;
	}
	// The Titan's own team does it no harm (takeDamageFromOwnTeam 0).
	level.world->applyDamage(consoles[0], 5000, own);
	CHECK(level.world->hitPoints(consoles[0]) == 7000);
	// Each brought down into its wreck (0 to -10: a hit past -wreckHitPoints
	// leaves it destroyed, state 2, and the doors get nothing, as
	// 0x004c0e40's check of the state 1 has it).
	for (size_t i = 0; i < consoles.size(); ++i) {
		level.world->applyDamage(consoles[i], 7005, attacker);
		CHECK(level.world->hitPoints(consoles[i]) <= 0);
		CHECK(std::fabs(level.world->hitPoints(doors) - (10.0f - 2.0f * static_cast<float>(i + 1))) < 1e-3f);
	}
	CHECK(level.titan(1)->consolesLeft == 0);
	CHECK(level.count("TitanPartDestroyed") == 4);
	CHECK(!level.titan(1)->coreExposed);
	level.run(1.1f);
	CHECK(!level.titan(1)->coreExposed && std::fabs(level.world->hitPoints(doors) - 1) < 1e-3f);
	level.run(1.1f);
	CHECK(level.titan(1)->coreExposed);
	// The doors' wreck took 1 % of the core (400); the core passed on its
	// share (400 of 40000: 1 %) to the Titan.
	CHECK(std::fabs(level.world->hitPoints(core) - 39600) < 1);
	CHECK(std::fabs(level.titan(1)->hitPoints - 6e6f * 0.99f) < 2);
	// A quarter of what the core has left: the Titan loses a quarter too.
	const float titanBefore = level.titan(1)->hitPoints;
	level.world->applyDamage(core, 9900, attacker);
	CHECK(std::fabs(level.titan(1)->hitPoints - titanBefore * 0.75f) < 2);
	CHECK(level.titan(1)->percent < 75 && !level.titan(1)->wreck);
	// The rest: the core and the Titan go together.
	level.world->applyDamage(core, 1e6f, attacker);
	CHECK(level.titan(1)->coreDestroyed);
	CHECK(level.titan(1)->wreck && level.titan(1)->hitPoints <= 0);
	std::fprintf(stderr, "  core destroyed: %zu TitanCoreDestroyed, %zu VehicleDestroyed of the Titan\n",
		level.count("TitanCoreDestroyed"), level.destroyed(pac->object));
	CHECK(level.count("TitanCoreDestroyed") == 1 && level.destroyed(pac->object) == 1);
	// A dead Titan takes no more (0x004c0e40).
	const float wreck = level.titan(1)->hitPoints;
	level.world->applyDamage(pac->object, 1000, attacker);
	CHECK(level.titan(1)->hitPoints == wreck);
	// Shots straight at the hull stop at 0 (onlyDamageFromChildrenLevel 0):
	// the other Titan, hit for more than it has.
	const auto* eu = level.titan(2);
	level.world->applyDamage(eu->object, 7e6f, own);
	CHECK(level.titan(2)->hitPoints == 0 && level.titan(2)->wreck);
}

// The commander's order: accepted inside the Titans' combat area, the Titan
// sets off towards it two seconds on, at about a metre a second (0.33
// pushed, damped 0.99 a tick); not again within minTimeBetweenActivation
// (5 s); refused outside the area; with the shield down it drifts to a stop.
TEST(Titan_MovesToTheCommandersTarget)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Minsk", 48);
	if (!level.loaded) {
		return;
	}
	const auto start = level.titan(1)->position;
	CHECK(level.world->moveTitan(1, 5000, 5000) == game::GameWorld::TitanMove::Unable); // outside CombatArea_141_Titans
	CHECK(level.world->moveTitan(1, 300, -200) == game::GameWorld::TitanMove::Accepted);
	CHECK(level.world->moveTitan(1, 300, -100) == game::GameWorld::TitanMove::TooSoon);
	level.run(1.9f);
	CHECK(physics::lengthSquared(level.titan(1)->position - start) < 1e-4f);
	level.run(60);
	const auto moved = level.titan(1)->position;
	const physics::Vec3 way {300 - start.x, 0, -200 - start.z};
	const float along = physics::dot(moved - start, way) / std::sqrt(physics::lengthSquared(way));
	const float speed = std::sqrt(physics::lengthSquared(level.titan(1)->velocity));
	std::fprintf(stderr, "  after a minute: %.1f m towards the target, %.2f m/s, height %.2f -> %.2f\n", along, speed, start.y,
		moved.y);
	CHECK(along > 10 && along < 70);
	CHECK(speed > 0.3f && speed < 1.2f);
	CHECK(std::fabs(moved.y - start.y) < 1e-3f);
	// The spawn points go with it.
	for (uint32_t id : level.world->titanSpawnPoints(1)) {
		CHECK(level.objects.find(id)->position().x > start.x - 300);
	}
	// The shield down: no more push, the damping stops it.
	level.world->setHitPoints(level.titan(1)->object, 6e6f * 0.5f);
	level.run(30);
	CHECK(std::sqrt(physics::lengthSquared(level.titan(1)->velocity)) < 0.05f);
	CHECK(!level.titan(1)->canMove);
}

// Minsk_Titan16 (gpm_ti 16): its own archives' Titans and silos
// (missile_silo_v22) work the same way.
TEST(Titan_MinskTitan16)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Minsk_Titan16", 16);
	if (!level.loaded) {
		return;
	}
	CHECK(level.world->titans().size() == 2);
	const auto silos = level.world->missileSilos();
	CHECK(!silos.empty());
	if (silos.empty() || !level.titan(1) || !level.titan(2)) {
		return;
	}
	level.world->setControlPointTeam(silos[0].controlPoint, 2);
	const float before = level.titan(1)->hitPoints;
	float second = 0;
	for (; second < 200 && level.titan(1)->hitPoints >= before; ++second) {
		level.run(1);
	}
	std::fprintf(stderr, "  Minsk_Titan16: %zu silos; PAC's Titan hit after %.0f s: %.0f\n", silos.size(), second,
		level.titan(1)->hitPoints);
	CHECK(level.titan(1)->hitPoints < before);
}
