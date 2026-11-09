// Titan mode on the shipped levels (needs $BF2142_GAME_DIR): Minsk's,
// Suez_Canal's and Minsk_Titan16's gpm_ti gameplay objects with every object
// template, terrain, collision meshes and materials. The Titans spawn with
// their parts and spawn points, the missile silos fire at the enemy Titan
// and wear its shield and then its hull down, the core's sequence (four
// consoles, the doors, the core) destroys a Titan, the commander's order
// moves it, and gpm_ti.py keeps the tickets and ends the round.
#include "Game/Bots.h"
#include "Hud/Hud.h"
#include "Main/HudMenus.h"
#include "Main/HudMenusVehicles.h"
#include "Main/PlayerCamera.h"
#include "Main/TitanShotTrace.h"
#include "Animation/AnimationSystem.h"
#include "TitanLevel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>

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
	// gpm_ti 48's GamePlayObjects hide their points (isVisible 0,
	// showOnMinimap 0: the scene's cull and the maps), which stay
	// capturable.
	int hidden = 0;
	for (const auto& point : level.world->controlPointList()) {
		hidden += !point.visible && !point.showOnMinimap;
		if (point.id == 503) {
			CHECK(!point.visible && point.capturable);
		}
	}
	std::fprintf(stderr, "  %d of %zu control points hidden\n", hidden, level.world->controlPointList().size());
	CHECK(hidden > 0);
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
// against veh_titan_armor, cell 1) less what passing the penetrable shield
// took (material 4112: its thickness x damageLoss 1000, at least 1000) off
// the shield; with the shield down (60 % and less) the next one comes off
// the hull whole, the forcefield is gone and the Titan stops.
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
	const float through = 6e6f - level.titan(2)->hitPoints;
	CHECK(through > 110000 - 5000 && through <= 110000 - 1000 + 1);
	CHECK(level.titan(2)->shield);
	CHECK(level.count("VehicleDamage") == 1);
	// The root's name tag (addNametagOnDestroy 1, healthActivationLevel 60):
	// re-pushed on the drop, its secondary icon from 60 % down.
	CHECK(level.titan(2)->nametagPushes == 1 && level.titan(2)->healthActivationLevel == 60);
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
	// (The wreck's drain has begun within the second: 1 % of its 3,000,000
	// wreckHitPoints a second.)
	CHECK(level.titan(2)->wreck && level.titan(2)->hitPoints <= 0 && level.titan(2)->hitPoints >= -30001);
	CHECK(level.destroyed(eu->object) == 1);
	// The core went with it (destroyOnParentDamageLevel 0).
	CHECK(level.titan(2)->coreDestroyed);
	CHECK(level.world->titanSpawnPoints(2).empty());
	// The crash (0x00652380, 0x00654730): the armour drains to
	// -wreckHitPoints over timeToStayAsWreck (100 s), the explosion groups
	// going off at their levels (-1 to -36 %), the core explosion event at
	// -38 %; then the wreck is deleted. It coasts to a stop at its height.
	const uint32_t wreck = eu->object;
	const float height = level.titan(2)->position.y;
	float exploded = -1, gone = -1;
	int lowest = 0, groups = 0, last = 0;
	for (second = 0; second < 110 && gone < 0; second += 0.5f) {
		level.run(0.5f);
		const auto t = level.titan(2);
		if (!t || t->object != wreck) {
			gone = second;
			break;
		}
		if (t->effectLevel != last) {
			++groups;
			last = t->effectLevel;
			lowest = std::min(lowest, last);
		}
		if (t->coreExplosion && exploded < 0) {
			exploded = second;
		}
		CHECK(std::fabs(t->position.y - height) < 0.01f);
	}
	std::fprintf(stderr, "  the wreck: %d explosion groups down to %d %%, the core explosion after %.1f s, gone after %.1f s\n",
		groups, lowest, exploded, gone);
	CHECK(lowest == -36 && groups >= 30);
	CHECK(exploded > 36 && exploded < 40);
	CHECK(gone > 98 && gone < 102);
	CHECK(!level.world->objects().find(wreck));
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
	// Each brought down into its wreck. 0x004c0e40 hands the doors their
	// share only for a wreck (state 1), but the killing blow is limited
	// (0x00656320: hit points + min(0.8 x wreckHitPoints, half the rest)),
	// so even an overkill leaves a console a wreck (-8 of its -10) and the
	// doors lose their 2.
	for (size_t i = 0; i < consoles.size(); ++i) {
		level.world->applyDamage(consoles[i], i == 0 ? 1e6f : 7005, attacker);
		CHECK(level.world->hitPoints(consoles[i]) <= 0 && level.world->hitPoints(consoles[i]) > -10);
		CHECK(i != 0 || std::fabs(level.world->hitPoints(consoles[i]) + 8) < 1e-3f);
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

// Selecting a Titan must use its own moving spawn points even when its
// control-point group also contains the level's ordinary ground spawns.
TEST(Titan_SuezDeploymentSelectsInteriorOnly)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Suez_Canal", 48);
	CHECK(level.loaded);
	if (!level.loaded) {
		return;
	}
	const game::GameWorld::TitanInfo* titan = level.titan(2);
	CHECK(titan != nullptr);
	if (!titan) {
		return;
	}
	const uint32_t ownTitan = titan->object;
	const int ownTeam = titan->team;
	const int titanControlPoint = titan->controlPoint;
	const std::vector<uint32_t> interiors = level.world->titanSpawnPoints(ownTeam);
	CHECK(interiors.size() == 7);
	int groundPoints = 0;
	for (const auto& object : level.objects.objects()) {
		const world::ObjectTemplate* templ = object->objectTemplate();
		if (!templ || templ->typeName() != "SpawnPoint") {
			continue;
		}
		int controlPoint = object->controlPointId();
		if (controlPoint < 0) {
			const auto setting = templ->settings().find("controlPointId");
			if (setting != templ->settings().end()) {
				controlPoint = std::atoi(setting->second.c_str());
			}
		}
		if (controlPoint == titanControlPoint
			&& std::find(interiors.begin(), interiors.end(), object->id()) == interiors.end()) {
			++groundPoints;
		}
	}
	CHECK(groundPoints > 0); // Suez's Titan CP group is genuinely mixed.

	const int bot = level.world->addPlayer("Titan deploy", ownTeam, true);
	level.world->setSpawnTitan(bot, ownTitan);
	CHECK(level.world->spawnChoice(bot) == game::GameWorld::kSpawnOnTitan);
	CHECK(level.world->spawnTitan(bot) == ownTitan);
	CHECK(level.world->spawnSoldier(bot));
	game::GameWorld::PlayerInfo info;
	CHECK(level.world->playerInfo(bot, info) && info.alive);
	float nearestInterior = std::numeric_limits<float>::max();
	for (uint32_t id : interiors) {
		const world::Object* point = level.objects.find(id);
		if (point) {
			nearestInterior = std::min(nearestInterior, std::sqrt(physics::lengthSquared(info.position - physics::Vec3 {point->position().x, point->position().y, point->position().z})));
		}
	}
	CHECK(nearestInterior < 2.0f);

	const int invalid = level.world->addPlayer("Invalid Titan deploy", ownTeam, true);
	level.world->setSpawnTitan(invalid, 0xffffffffu);
	CHECK(!level.world->spawnSoldier(invalid)); // no ground-spawn fallback for an invalid Titan choice
	const game::GameWorld::TitanInfo* enemyTitan = level.titan(1);
	const int enemy = level.world->addPlayer("Enemy Titan deploy", ownTeam, true);
	CHECK(enemyTitan != nullptr);
	if (enemyTitan) {
		level.world->setSpawnTitan(enemy, enemyTitan->object);
		CHECK(!level.world->spawnSoldier(enemy)); // enemy Titan choice is refused without a ground fallback
	}
	level.world->setGameMode("gpm_cq");
	const int conquest = level.world->addPlayer("Conquest Titan deploy", ownTeam, true);
	level.world->setSpawnTitan(conquest, ownTitan);
	CHECK(!level.world->spawnSoldier(conquest)); // Conquest does not gain Titan deployment
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

// A soldier at its Titan's pod array drops in a pod (eu_pod0, a rigid body
// falling at gravityModifier 1.5 with its drag) and steps out where it lands.
TEST(Titan_PodArrayDropsASoldier)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Minsk", 48);
	if (!level.loaded) {
		return;
	}
	const int bot = level.world->addPlayer("Pac", 1, false);
	level.world->setSpawnChoice(bot, 501);
	game::GameWorld::PlayerInfo info;
	physics::Vec3 entry;
	float radius = 0;
	for (int tries = 0; tries < 30 && !level.world->titanPodEntry(bot, entry, radius); ++tries) {
		if (level.world->playerInfo(bot, info) && info.alive) {
			level.world->setHitPoints(info.soldier, 0);
			level.run(16);
		} else {
			level.world->spawnSoldier(bot);
		}
		level.run(0.1f);
	}
	CHECK(level.world->titanPodEntry(bot, entry, radius));
	level.world->playerInfo(bot, info);
	CHECK(level.world->setSoldierPosition(info.soldier, entry));
	CHECK(level.world->launchPod(bot));
	CHECK(level.world->ridingPod(bot) && level.world->pods().size() == 1);
	float second = 0;
	// Its keepUp ForceObject's couple (0x0062b750) holds it near upright in
	// the air.
	float tilt = 0;
	for (; second < 60 && level.world->ridingPod(bot); second += 0.5f) {
		level.run(0.5f);
		for (const auto& pod : level.world->pods()) {
			if (const world::Object* o = level.world->objects().find(pod.object)) {
				tilt = std::max({tilt, std::fabs(o->rotation().y), std::fabs(o->rotation().z)});
			}
		}
	}
	level.world->playerInfo(bot, info);
	std::fprintf(stderr, "  the pod landed after %.1f s at %.0f/%.0f/%.0f (from %.0f/%.0f/%.0f), tilted at most %.1f degrees\n",
		second, info.position.x, info.position.y, info.position.z, entry.x, entry.y, entry.z, tilt);
	CHECK(tilt < 30);
	CHECK(!level.world->ridingPod(bot) && info.alive && info.vehicle == 0);
	CHECK(info.position.y < 200); // down from the Titan's 287 to the ground
}

// The Titans' gun seats: each gun hub (eu_groundcanone3*_hub,
// eu_aacannon*_hub) is a seat of the Titan's, entered at its console inside
// (the hub's own EntryPoint, setEntryRadius 5); the gunner turns the
// turret within its bundle's limits in the hub's frame and fires its gun;
// with the gun's armour out (disableInputOnDestroy) it stops, and the
// Titan's wreck puts the gunners out.
TEST(Titan_GunSeatsAreMannedAndFire)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Minsk", 48);
	if (!level.loaded) {
		return;
	}
	const uint32_t eu = level.world->titanOf(2);
	CHECK(eu != 0);
	const std::vector<game::GameWorld::SeatLayout> seats = level.world->vehicleSeats(eu);
	CHECK(seats.size() == 6);
	int ground = -1, aa = -1;
	for (size_t i = 0; i < seats.size(); ++i) {
		std::fprintf(stderr, "  seat %zu %s: gun %s, frame %.0f, yaw %.0f..%.0f, pitch %.0f..%.0f\n", i, seats[i].name.c_str(),
			seats[i].weapon.c_str(), seats[i].frameYaw, seats[i].minYaw, seats[i].maxYaw, seats[i].minPitch, seats[i].maxPitch);
		if (seats[i].name == "eu_groundcanone3_01_hub") {
			ground = static_cast<int>(i);
		}
		if (seats[i].name == "eu_aacannon_hub") {
			aa = static_cast<int>(i);
		}
	}
	CHECK(ground >= 0 && aa >= 0);
	CHECK(std::fabs(std::fabs(seats[static_cast<size_t>(ground)].frameYaw) - 180) < 1);
	CHECK(seats[0].minYaw == -45 && seats[0].maxYaw == 45); // EU_groundcanonE3_Base
	CHECK(seats[static_cast<size_t>(ground)].maxYaw < 180);
	const int gunner = level.world->addPlayer("Gunner", 2, false);
	CHECK(level.world->spawnSoldier(gunner));
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(gunner, info);
	// Into the Titan, at a console: the nearest free gun.
	const auto titan = level.titan(2);
	CHECK(level.world->setSoldierPosition(info.soldier, titan->position + physics::Vec3 {0, 11, -30}));
	physics::Vec3 entry;
	float radius = 0;
	int seat = -1;
	CHECK(level.world->titanGunEntry(gunner, entry, radius, seat));
	CHECK(radius == 5 && seat >= 0);
	CHECK(level.world->setSoldierPosition(info.soldier, entry - physics::Vec3 {0, 1, 0}));
	game::SoldierInput input;
	input.use = true;
	level.world->setInput(gunner, input);
	level.run(0.1f);
	level.world->playerInfo(gunner, info);
	CHECK(info.vehicle == eu && info.seat == seat);
	CHECK(!level.world->drivesSeat(eu, info.seat));
	// Over to the rear ground cannon; turn far right: the hub's limit holds.
	input = {};
	input.switchSeat = ground + 1;
	level.world->setInput(gunner, input);
	level.run(0.1f);
	input = {};
	input.turn = 500; // degrees this tick
	level.world->setInput(gunner, input);
	level.run(1);
	level.world->playerInfo(gunner, info);
	CHECK(info.seat == ground);
	float yaw = 0, pitch = 0;
	CHECK(level.world->gunAim(eu, ground, yaw, pitch));
	std::fprintf(stderr, "  rear ground cannon aim: yaw %.1f, pitch %.1f\n", yaw, pitch);
	CHECK(std::fabs(std::remainder(yaw - (seats[static_cast<size_t>(ground)].frameYaw + seats[static_cast<size_t>(ground)].maxYaw), 360.0f)) < 0.5f);
	input.turn = 0;
	input.aimPitch = -30;
	level.world->setInput(gunner, input);
	level.run(0.1f);
	CHECK(level.world->gunAim(eu, ground, yaw, pitch) && pitch == -30);
	// Fire: its rounds leave backwards and down.
	const size_t before = level.world->projectiles().size();
	input.turn = 0;
	input.fire = true;
	level.world->setInput(gunner, input);
	level.run(0.2f);
	CHECK(level.world->projectiles().size() > before);
	const game::Projectile& round = level.world->projectiles().back();
	CHECK(round.velocity.y < 0 && round.player == gunner);
	// Its armour out: no more rounds.
	for (const auto& part : level.world->titanParts(2)) {
		if (part.kind == game::GameWorld::TitanPartInfo::Gun && part.position.z < titan->position.z - 60) {
			level.world->applyDamage(part.id, 1e5f, -1);
		}
	}
	level.run(1.5f);
	size_t fresh = 0;
	for (const auto& p : level.world->projectiles()) {
		fresh += p.player == gunner && p.age < 0.4f ? 1 : 0;
	}
	CHECK(fresh == 0);
	// The Titan's wreck: the gunner is out.
	level.world->applyDamage(eu, 1e7f, -1);
	level.run(0.1f);
	level.world->playerInfo(gunner, info);
	CHECK(info.vehicle == 0);
}

// The team forcefields (collisionGroups 0x100 the PAC Titan's own, 0x200
// the EU Titan's): an object passes its own team's and meets the other's.
// The Titan's object spawners
// (as_titan_ATSpawner / AGSpawner) and supply point are objects of their
// own carried with it.
TEST(Titan_ForcefieldsAndCarriedObjects)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Minsk", 48);
	if (!level.loaded) {
		return;
	}
	for (int team = 1; team <= 2; ++team) {
		const auto titan = *level.titan(team);
		const world::Object* object = level.objects.find(titan.object);
		const physics::Mat3 turn =
			physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y, object->rotation().z});
		// The spawn room's field's place in the Titan (its template tree).
		std::vector<physics::Vec3> fields;
		std::function<void(const world::ObjectTemplate&, const physics::Mat3&, const physics::Vec3&, int)> walk =
			[&](const world::ObjectTemplate& t, const physics::Mat3& r, const physics::Vec3& at, int depth) {
				const auto groups = t.settings().find("collisionGroups");
				if (depth > 0 && groups != t.settings().end() && (std::atoi(groups->second.c_str()) & 0x300)
					&& t.name().find("spawnroom") != std::string::npos) {
					fields.push_back(at);
				}
				for (const world::ChildTemplate& child : t.children()) {
					if (const world::ObjectTemplate* c = level.templates.find(child.name); c && depth < 8) {
						walk(*c, physics::Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z}) * r,
							r.transform({child.position.x, child.position.y, child.position.z}) + at, depth + 1);
					}
				}
			};
		walk(*object->objectTemplate(), physics::Mat3 {}, {}, 0);
		CHECK(fields.size() == 1);
		int blocked = 0;
		for (const physics::Vec3& offset : fields) {
			const physics::Vec3 at = titan.position + turn.transform(offset);
			for (const physics::Vec3 axis : {physics::Vec3 {1, 0, 0}, physics::Vec3 {0, 0, 1}}) {
				for (const float up : {0.3f, 1.0f, 1.6f}) {
					const physics::Vec3 a = at + turn.transform(axis * 3.0f + physics::Vec3 {0, up, 0});
					const physics::Vec3 b = at + turn.transform(axis * -3.0f + physics::Vec3 {0, up, 0});
					blocked += level.world->lineOfSight(a, b) ? 0 : 1;
				}
			}
		}
		std::fprintf(stderr, "  %s Titan's spawn room field: %d of 6 ways stop rounds\n",
			team == 1 ? "PAC" : "EU", blocked);
		CHECK(blocked > 0);
	}
	// The carried objects: two spawners and a supply point.
	const auto pac = *level.titan(1);
	std::vector<uint32_t> carried;
	for (const auto& object : level.objects.objects()) {
		const std::string& name = object->objectTemplate() ? object->objectTemplate()->name() : std::string();
		if (name == "as_titan_ATSpawner" || name == "as_titan_AGSpawner" || name == "as_titan_heliRepairRearm") {
			carried.push_back(object->id());
		}
	}
	std::fprintf(stderr, "  %zu objects carried by the PAC Titan\n", carried.size());
	CHECK(carried.size() == 3);
	std::vector<world::Vec3> before;
	for (const uint32_t id : carried) {
		before.push_back(level.objects.find(id)->position());
	}
	CHECK(level.world->moveTitan(1, 300, -200) == game::GameWorld::TitanMove::Accepted);
	level.run(20);
	const auto moved = *level.titan(1);
	const float dx = moved.position.x - pac.position.x, dz = moved.position.z - pac.position.z;
	CHECK(dx * dx + dz * dz > 4);
	for (size_t i = 0; i < carried.size(); ++i) {
		const world::Object* o = level.objects.find(carried[i]);
		CHECK(o && std::fabs(o->position().x - before[i].x - dx) < 0.01f && std::fabs(o->position().z - before[i].z - dz) < 0.01f);
	}
}

TEST(Titan_InteriorFieldCollisionUsesTeamAndRetailLod)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		TitanLevel level(gameDir, "Suez_Canal", 48, true, mod);
		CHECK(level.loaded);
		if (!level.loaded) continue;
		for (int team = 1; team <= 2; ++team) {
			const auto* titan = level.titan(team);
			CHECK(titan != nullptr);
			if (!titan) continue;
			const world::Object* root = level.objects.find(titan->object);
			const physics::Mat3 rootRotation = physics::Mat3::fromYawPitchRoll(
				{root->rotation().x, root->rotation().y, root->rotation().z});
			int fields = 0, verified = 0;
			struct Probe { uint32_t console; physics::Vec3 from, to; };
			std::vector<Probe> consoleProbes;
			const auto parts = level.world->titanParts(team);
			std::function<void(const world::ObjectTemplate&, const physics::Mat3&, const physics::Vec3&, int, uint32_t)> visit;
			visit = [&](const world::ObjectTemplate& t, const physics::Mat3& rotation, const physics::Vec3& position, int depth, uint32_t console) {
				if (depth > 8) return;
				for (const auto& part : parts) if (part.kind == game::GameWorld::TitanPartInfo::Console && part.name == t.name()) {
					console = part.id;
				}
				const auto groups = t.settings().find("collisionGroups");
				if (groups != t.settings().end() && (std::atoi(groups->second.c_str()) & 0x300)
					&& t.name().find("forcefield_corridor") != std::string::npos) {
					++fields;
					const std::string file = t.creationFile();
					const size_t slash = file.find_last_of("/\\");
					const auto* mesh = level.collisionTemplates.mesh(t.name(), level.files,
						slash == std::string::npos ? "" : file.substr(0, slash + 1));
					CHECK(mesh != nullptr);
					if (mesh) {
						const auto* sub = physics::collisionPart(*mesh, 0);
						const auto* lod = sub ? physics::collisionLodFor(*mesh, *sub, 2) : nullptr;
						CHECK(lod != nullptr);
						bool found = false;
						if (lod) for (const auto& face : lod->faces) {
							const auto& a = lod->vertices[face.vertices[0]];
							const auto& b = lod->vertices[face.vertices[1]];
							const auto& c = lod->vertices[face.vertices[2]];
							physics::Vec3 va {a[0], a[1], a[2]}, vb {b[0], b[1], b[2]}, vc {c[0], c[1], c[2]};
							physics::Vec3 normal = physics::cross(vc - vb, vb - va);
							const float len = std::sqrt(physics::lengthSquared(normal));
							if (len < 1e-5f) continue;
							normal = rootRotation.transform(rotation.transform(normal * (0.2f / len)));
							const physics::Vec3 center = titan->position + rootRotation.transform(position + rotation.transform((va + vb + vc) * (1.0f / 3)));
							const auto from = center - normal, to = center + normal;
							if (level.world->soldierPathClear(team, from, to)
								&& !level.world->soldierPathClear(3 - team, from, to)) {
								found = true;
								if (console) consoleProbes.push_back({console, from, to});
								break;
							}
						}
						if (found) ++verified;
					}
				}
				for (const auto& child : t.children()) if (const auto* c = level.templates.find(child.name)) {
					visit(*c, physics::Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z}) * rotation,
						position + rotation.transform({child.position.x, child.position.y, child.position.z}), depth + 1, console);
				}
			};
			visit(*root->objectTemplate(), {}, {}, 0, 0);
			std::fprintf(stderr, "  %s team %d corridor fields %d, team-filtered %d\n", mod, team, fields, verified);
			CHECK(fields > 0 && verified == fields);
			CHECK(!consoleProbes.empty());
			for (const auto& probe : consoleProbes) level.world->setHitPoints(probe.console, 0);
			level.run(0.2f);
			for (const auto& probe : consoleProbes) CHECK(level.world->soldierPathClear(3 - team, probe.from, probe.to));
		}
	}
}

// The Titan's outer shield (as_titan_forcefield / eu_forcefield) is death
// material 0x29a: an enemy soldier or gunship touching it while it stands
// dies; the Titan's own team passes it, and a wrecked shield is gone. (From
// the 8 Nov log: a pilot left a gunship that sat on the enemy shield and
// walked its dome.)
TEST(Titan_OuterShieldKillsEnemies)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (const int shieldDown : {0, 1}) {
			TitanLevel level(gameDir, "Suez_Canal", 48, true, mod);
			CHECK(level.loaded);
			if (!level.loaded) continue;
			const auto titan = *level.titan(1);
			const world::Object* object = level.objects.find(titan.object);
			const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y, object->rotation().z});
			const auto local = [&](float y) { return titan.position + turn.transform({0, y, -30}); };
			float dome = -1; // the shield's top over the rear deck, from the enemy's mesh
			for (float y = 40; y > 0; y -= 0.25f) {
				if (!level.world->soldierPathClear(2, local(y + 0.25f), local(y))) { dome = y; break; }
			}
			CHECK(dome > 0);
			if (shieldDown) {
				level.world->setHitPoints(titan.object, 6e6f * 0.59f);
				level.run(0.5f);
			}
			int enemy = -1, own = -1;
			for (const int team : {2, 1}) {
				const int id = level.world->addPlayer("Walker", team, true);
				CHECK(level.world->spawnSoldier(id));
				game::GameWorld::PlayerInfo info;
				level.world->playerInfo(id, info);
				// Set down on the dome, a half metre over it; the own team's is 10 m aside.
				CHECK(level.world->setSoldierPosition(info.soldier, local(dome + 0.5f) + (team == 1 ? turn.transform({10, 0, 0}) : physics::Vec3 {})));
				(team == 2 ? enemy : own) = id;
			}
			// An enemy gunship set down on the dome (retail 0x005c72f0: 1e10 on
			// the death material, unscaled by the material table).
			const physics::Vec3 aside = titan.position + turn.transform({-14, dome + 2, -30});
			world::ObjectTemplate* gunship = level.templates.find("us_ag");
			CHECK(gunship != nullptr);
			world::Object* craft = gunship ? level.objects.create(gunship, {aside.x, aside.y, aside.z}, {0, 0, 0}) : nullptr;
			const uint32_t craftId = craft ? craft->id() : 0;
			if (craft) craft->setTeam(2);
			level.run(2);
			const float craftHp = level.world->hitPoints(craftId);
			std::fprintf(stderr, "  %s shield %s: enemy gunship hp %.0f\n", mod, shieldDown ? "down" : "up", craftHp);
			CHECK(shieldDown ? craftHp > 0 : craftHp < 0);
			game::GameWorld::PlayerInfo e, o;
			level.world->playerInfo(enemy, e);
			level.world->playerInfo(own, o);
			std::fprintf(stderr, "  %s shield %s: enemy soldier %s, own soldier %s\n", mod, shieldDown ? "down" : "up",
				e.alive ? "alive" : "dead", o.alive ? "alive" : "dead");
			CHECK(shieldDown ? e.alive : !e.alive);
			CHECK(o.alive);
		}
	}
}

// A walker's leg sphere reaches below its hull box. That additional
// contact must still honor the outer shield's team filtering.
TEST(Titan_WalkerLegsRespectOuterShieldTeam)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		for (int team : {1, 2}) {
			TitanLevel level(gameDir, "Suez_Canal", 48, true, mod);
			CHECK(level.loaded);
			if (!level.loaded) continue;
			const auto titan = *level.titan(1);
			const auto* ship = level.objects.find(titan.object);
			const auto turn = physics::Mat3::fromYawPitchRoll({ship->rotation().x, ship->rotation().y, ship->rotation().z});
			const auto local = [&](float y) { return titan.position + turn.transform({0, y, -30}); };
			float dome = -1;
			for (float y = 40; y > 0; y -= 0.25f) {
				if (!level.world->soldierPathClear(2, local(y + 0.25f), local(y))) { dome = y; break; }
			}
			CHECK(dome > 0);
			const auto at = local(dome + 3.2f);
			auto* t = level.templates.find("us_heavy_mech");
			CHECK(t);
			if (!t || dome <= 0) continue;
			auto* walker = level.objects.create(t, {at.x, at.y, at.z});
			walker->setTeam(team);
			const auto id = walker->id();
			level.run(0.3f);
			const float hp = level.world->hitPoints(id);
			std::fprintf(stderr, "  %s walker team %d shield health %.0f\n", mod, team, hp);
			CHECK(team == 1 ? hp > 0 : hp < 0);
		}
	}
}

// Bots aboard their Titan: while fewer than half its six guns are manned
// or claimed, a bot claims the nearest free one, walks to its console and
// takes it (the guns' AI templates are of equipment type Fixed, which the
// seat logic covers; the walk is the rewrite's, AIDLL has no Titan
// strategy); the rest make for the pod arrays.
TEST(Titan_BotsManHalfTheGuns)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Minsk", 48);
	if (!level.loaded) {
		return;
	}
	game::BotSettings settings;
	game::BotManager bots(*level.world, settings);
	level.afterTick = [&] { bots.update(1 / 30.0f); };
	const auto pac = *level.titan(1);
	const world::Object* titan = level.objects.find(pac.object);
	const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({titan->rotation().x, titan->rotation().y, titan->rotation().z});
	std::vector<int> crew;
	for (int i = 0; i < 6; ++i) {
		const int id = level.world->addPlayer("Crew" + std::to_string(i), 1, true);
		CHECK(level.world->spawnSoldier(id));
		game::GameWorld::PlayerInfo info;
		level.world->playerInfo(id, info);
		// Over the Titan's spawn room deck (10.6 m up at 40 m aft, in its
		// frame): they come down on it, falling too little for their
		// parachutes (BotManager::handleWeapon's input 22).
		CHECK(level.world->setSoldierPosition(info.soldier, pac.position + turn.transform({static_cast<float>(i) - 3, 11.5f, -40})));
		crew.push_back(id);
	}
	level.run(40);
	int gunners = 0, aboard = 0;
	for (const int id : crew) {
		game::GameWorld::PlayerInfo info;
		level.world->playerInfo(id, info);
		gunners += info.vehicle == pac.object ? 1 : 0;
		physics::Vec3 entry;
		float radius = 0;
		aboard += info.alive && !info.vehicle && level.world->titanPodEntry(id, entry, radius) ? 1 : 0;
	}
	std::fprintf(stderr, "  of 6 bots aboard: %d at the guns, %d on foot aboard\n", gunners, aboard);
	CHECK(gunners >= 1 && gunners <= 3);
}

// On Suez gpm_ti the generic SAI fallback also contains the two
// non-capturable Titan control points. An on-foot bot should be routed to a
// live silo instead of treating a Titan marker as a capture objective.
TEST(Titan_BotsChooseCapturableSilos)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Suez_Canal", 48);
	if (!level.loaded) {
		return;
	}
	for (const game::GameWorld::TitanInfo& titan : level.world->titans()) {
		CHECK(menu::titanHudHealth(titan) == 100);
	}
	std::vector<uint8_t> qtr, clb;
	CHECK(level.files.readFile("Levels/Suez_Canal/AIPathFinding/Infantry.qtr", qtr));
	CHECK(level.files.readFile("Levels/Suez_Canal/AIPathFinding/Infantry.clb", clb));
	const auto mesh = ai::parseNavMesh(qtr, clb);
	CHECK(mesh.has_value());
	if (!mesh) {
		return;
	}
	game::BotManager bots(*level.world, {});
	bots.setNavMesh(&*mesh);
	const int id = level.world->addPlayer("SiloBot", 2, true);
	CHECK(level.world->spawnSoldier(id));
	game::GameWorld::PlayerInfo self;
	CHECK(level.world->playerInfo(id, self) && self.alive && !self.vehicle);
	CHECK(level.world->setSoldierPosition(self.soldier,
		{357.9f, level.world->terrainHeight(357.9f, 49.4f) + 1.0f, 49.4f}));
	bots.update(1 / 30.0f);
	const std::vector<physics::Vec3> route = bots.route(id);
	CHECK(!route.empty());
	if (route.empty()) {
		return;
	}
	const physics::Vec3 target = route.back();
	bool siloTarget = false;
	for (const game::GameWorld::SiloInfo& silo : level.world->missileSilos()) {
		if (silo.team == 2) {
			continue;
		}
		for (const game::GameWorld::ControlPointInfo& point : level.world->controlPointList()) {
			const float dx = point.position.x - target.x, dz = point.position.z - target.z;
			if (point.id == silo.controlPoint && point.capturable && dx * dx + dz * dz <= point.radius * point.radius) {
				siloTarget = true;
			}
		}
	}
	CHECK(siloTarget);
}

// 0x004bbf60's tail / 0x006f2b00: a PAC soldier inside EU's Titan's
// coreroom (its type-5 hull) while the shield is up dies
// within 64 ticks with no attacker; an EU soldier there lives.
TEST(Titan_EnemyInsideTheHullDies)
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
	const auto* titan = level.titan(2);
	CHECK(titan != nullptr);
	if (!titan) {
		return;
	}
	// The coreroom (eu_titan_coreroom at 0/-0.081/-53.40 in the Titan, its
	// hull 48 x 13 x 79 m from z -21 to 58): the middle of it.
	const world::Object* object = level.objects.find(titan->object);
	const physics::Mat3 turn =
		physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y, object->rotation().z});
	const physics::Vec3 at = physics::Vec3 {object->position().x, object->position().y, object->position().z}
		+ turn.transform({0, -0.081f, -53.4f + 18});
	const int pac = level.world->addPlayer("Pac", 1, true);
	const int eu = level.world->addPlayer("Eu", 2, true);
	CHECK(level.world->spawnSoldier(pac) && level.world->spawnSoldier(eu));
	for (const int id : {pac, eu}) {
		game::GameWorld::PlayerInfo info;
		level.world->playerInfo(id, info);
		level.world->setSoldierPosition(info.soldier, {at.x, at.y, at.z});
	}
	for (int i = 0; i < 64; ++i) {
		level.world->tick(1 / 30.0f);
	}
	game::GameWorld::PlayerInfo a, b;
	level.world->playerInfo(pac, a);
	level.world->playerInfo(eu, b);
	std::fprintf(stderr, "  in EU's Titan: PAC %s, EU %s\n", a.alive ? "alive" : "dead", b.alive ? "alive" : "dead");
	CHECK(!a.alive && b.alive);
}

// python.onRepairEvent (0x004bc410 after the heal): a gun hub brought to
// its wreck raises TitanWeaponDestroyed (onDestroyEvent 50) and its seat
// stops working; repaired out of the wreck (canBeRepairedWhenWreck 1) it
// works again and raises TitanWeaponRepaired (51) once, with the Titan and
// the repairer; the next wreck and repair raise each again (the
// component's sent flags, +0x10 / +0x11). A console's wreck can't be
// repaired (canBeRepairedWhenWreck 0), so TitanPartRepaired never goes.
TEST(Titan_GunHubsRepairedOutOfTheWreck)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Minsk", 48);
	if (!level.loaded) {
		return;
	}
	const int engineer = level.world->addPlayer("Engineer", 2, true);
	const int enemy = level.world->addPlayer("Pac", 1, true);
	uint32_t gun = 0, console = 0;
	for (const auto& part : level.world->titanParts(2)) {
		if (!gun && part.kind == game::GameWorld::TitanPartInfo::Gun) {
			gun = part.id;
		}
		if (!console && part.kind == game::GameWorld::TitanPartInfo::Console) {
			console = part.id;
		}
	}
	CHECK(gun && console);
	if (!gun || !console) {
		return;
	}
	const auto repaired = [&] { return level.count("TitanWeaponRepaired"); };
	// Whole: nothing to repair.
	CHECK(!level.world->repair(gun, 10, engineer));
	level.world->applyDamage(gun, 1e6f, enemy);
	CHECK(level.world->hitPoints(gun) <= 0);
	CHECK(level.count("TitanWeaponDestroyed") == 1);
	// A little: still a wreck, no event.
	CHECK(level.world->repair(gun, 1, engineer));
	CHECK(repaired() == 0);
	int steps = 0;
	while (level.world->hitPoints(gun) <= 0 && steps++ < 1000) {
		level.world->repair(gun, 1, engineer);
	}
	CHECK(level.world->hitPoints(gun) > 0);
	CHECK(repaired() == 1);
	const auto it = std::find_if(level.events.rbegin(), level.events.rend(),
		[](const auto& e) { return e.first == "TitanWeaponRepaired"; });
	CHECK(it != level.events.rend() && it->second.size() == 2);
	if (it != level.events.rend() && it->second.size() == 2) {
		CHECK(static_cast<uint32_t>(it->second[0].i) == level.world->titanOf(2));
		CHECK(it->second[1].i == engineer);
	}
	// Up to full: no second event.
	while (level.world->repair(gun, 10, engineer)) {
	}
	CHECK(repaired() == 1);
	// Again: each once more.
	level.world->applyDamage(gun, 1e6f, enemy);
	CHECK(level.count("TitanWeaponDestroyed") == 2);
	steps = 0;
	while (level.world->hitPoints(gun) <= 0 && steps++ < 1000) {
		level.world->repair(gun, 5, engineer);
	}
	CHECK(repaired() == 2);
	// The console's wreck stays one.
	level.world->applyDamage(console, 1e6f, enemy);
	CHECK(level.world->hitPoints(console) <= 0);
	CHECK(!level.world->repair(console, 50, engineer));
	CHECK(level.count("TitanPartRepaired") == 0);
}

// The forcefield's Radio destroyedGameEvent 1 (eu_forcefield,
// as_titan_forcefield): its wreck (0x00654120's message 0x18, 0x0063c6a0)
// sends game event 1 to every spawner, which switches on the map's
// activateOnEvent 1 spawners (0x005ad470; Minsk's APC pods). The retail
// Titans start asleep (isSleeping, 0x005f6ba0).
TEST(Titan_ShieldDownSwitchesOnThePodSpawners)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Minsk", 48);
	if (!level.loaded) {
		return;
	}
	level.run(0.5f);
	const auto waiting = [&] {
		int on = 0, off = 0;
		for (const auto& spawner : level.world->spawners()) {
			if (spawner->settings().activateOnEvent == 1) {
				(spawner->active() ? on : off) += 1;
			}
		}
		return std::make_pair(on, off);
	};
	const auto before = waiting();
	std::fprintf(stderr, "  activateOnEvent 1 spawners: %d on, %d off\n", before.first, before.second);
	CHECK(before.first == 0 && before.second >= 5);
	for (int team = 1; team <= 2; ++team) {
		CHECK(level.titan(team) && level.world->bodyAsleep(level.titan(team)->object));
	}
	const auto* eu = level.titan(2);
	level.world->setHitPoints(eu->object, 6e6f * 0.59f);
	level.run(0.5f);
	const auto after = waiting();
	std::fprintf(stderr, "  after the EU shield: %d on, %d off\n", after.first, after.second);
	CHECK(after.first == before.second && after.second == 0);
}

// setIfTitanChangeToWeapon (pod_launcher_cq's "pod_launcher 250", the
// firearm's fire press 0x005c50e0): within 250 m of a Titan (across x and
// z) an APC's pod seat fires its pod_launcher (eu_pod, the Titan's
// boarding pod) instead; farther off its own pod_launcher_cq (eu_pod_cq).
TEST(Titan_PodSeatsChangeToTheTitanPodNearATitan)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Minsk", 48);
	if (!level.loaded || !level.templates.find("eu_apc_pods")) {
		return;
	}
	level.run(0.5f);
	const auto* eu = level.titan(2);
	CHECK(eu != nullptr);
	if (!eu) {
		return;
	}
	const physics::Vec3 titanAt = eu->position;
	const float along = std::sqrt(titanAt.x * titanAt.x + titanAt.z * titanAt.z);
	for (const float off : {150.0f, 450.0f}) {
		// Towards the map's middle from under the Titan.
		const float x = titanAt.x - titanAt.x / along * off, z = titanAt.z - titanAt.z / along * off;
		world::Object* apc =
			level.objects.create(level.templates.find("eu_apc_pods"), {x, level.world->terrainHeight(x, z) + 2.5f, z});
		apc->setTeam(2);
		level.run(2 / 30.0f);
		const int player = level.world->addPlayer(off < 250 ? "Near" : "Far", 2, false);
		level.world->setSpawnVehicle(player, apc->id());
		CHECK(level.world->spawnSoldier(player));
		// A pod seat (EU_APC_Podcannon1_pods..4).
		int podSeat = -1;
		const auto seats = level.world->vehicleSeats(apc->id());
		for (size_t i = 0; i < seats.size() && podSeat < 0; ++i) {
			std::string name = seats[i].name;
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			podSeat = name.find("podcannon") != std::string::npos ? static_cast<int>(i) : -1;
		}
		CHECK(podSeat > 0);
		game::SoldierInput input;
		input.switchSeat = podSeat + 1;
		level.world->setInput(player, input);
		level.run(1 / 30.0f);
		level.world->setInput(player, {});
		level.run(1 / 30.0f);
		game::GameWorld::PlayerInfo info;
		CHECK(level.world->playerInfo(player, info) && info.vehicle == apc->id() && info.seat == podSeat);
		CHECK(level.world->launchPod(player));
		std::string pod;
		for (const auto& p : level.world->pods()) {
			if (p.player == player) {
				const world::Object* o = level.objects.find(p.object);
				pod = o && o->objectTemplate() ? o->objectTemplate()->name() : "";
			}
		}
		std::fprintf(stderr, "  %.0f m from the Titan: %s\n", off, pod.c_str());
		CHECK(off < 250 ? pod == "eu_pod" : pod == "eu_pod_cq");
	}
}

// A real APC pod dropped onto the friendly Suez Titan's upper deck. The
// pod must eject on contact and disappear shortly afterwards, without waiting
// for world-space velocity to settle on the Titan.
TEST(Titan_ApcPodLandsAndDisappearsOnDeck)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		TitanLevel level(dir, "Suez_Canal", 48, true, mod);
		CHECK(level.loaded && level.titan(2) && level.templates.find("eu_apc_pods"));
		if (!level.loaded || !level.titan(2) || !level.templates.find("eu_apc_pods")) return;
		const auto titan = *level.titan(2);
		const world::Object* hull = level.objects.find(titan.object);
		const physics::Mat3 frame = physics::Mat3::fromYawPitchRoll({hull->rotation().x, hull->rotation().y, hull->rotation().z});
		const physics::Vec3 at = titan.position + frame.transform({0, 35, -20});
		world::Object* apc = level.objects.create(level.templates.find("eu_apc_pods"), {at.x, at.y, at.z});
		apc->setTeam(2);
		const uint32_t apcId = apc->id();
		level.run(1 / 30.0f);
		const int player = level.world->addPlayer("Pod rider", 2, false);
		level.world->setSpawnVehicle(player, apcId);
		CHECK(level.world->spawnSoldier(player));
		const auto seats = level.world->vehicleSeats(apcId);
		int seat = -1;
		for (size_t i = 0; i < seats.size(); ++i) {
			std::string name = seats[i].name;
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (name.find("podcannon") != std::string::npos) { seat = static_cast<int>(i); break; }
		}
		CHECK(seat > 0);
		game::SoldierInput input; input.switchSeat = seat + 1;
		level.world->setInput(player, input); level.run(1 / 30.0f);
		level.world->setInput(player, {}); level.run(1 / 30.0f);
		CHECK(level.world->launchPod(player));
		CHECK(!level.world->pods().empty());
		if (level.world->pods().empty()) return;
		const uint32_t pod = level.world->pods().back().object;
		CHECK(level.world->setBodyVelocity(pod, {0, -15, 0}));
		int ticks = 0;
		while (ticks < 180 && level.world->ridingPod(player)) { level.run(1 / 30.0f); ++ticks; }
		game::GameWorld::PlayerInfo info; level.world->playerInfo(player, info);
		std::fprintf(stderr, "  %s: rider left after %.2f s at y %.1f (Titan %.1f)\n", mod, ticks / 30.0f, info.position.y, titan.position.y);
		CHECK(!level.world->ridingPod(player) && info.alive && info.vehicle == 0);
		CHECK(info.position.y > titan.position.y + 3 && info.position.y < titan.position.y + 18);
		level.run(0.3f);
		CHECK(level.objects.find(pod) == nullptr);
	}
}

// A parachute opened inside a Titan (its hangar, 14 m over the deck) comes
// down on the deck: the parachute (otherCollisionLod 2) meets each of the
// Titan's parts at its soldier LOD (Titan::VehiclePart::lod), as the mobile
// mesh physics meets any mesh at the LOD the mover asks for (0x006bb050),
// lands on it (0x005cf750) and puts its rider out there, alive.
TEST(Titan_ParachuteComesDownOnTheDecks)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Minsk", 48);
	if (!level.loaded) {
		return;
	}
	const auto pac = *level.titan(1);
	const world::Object* object = level.objects.find(pac.object);
	const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y, object->rotation().z});
	const int id = level.world->addPlayer("Jumper", 1, true);
	CHECK(level.world->spawnSoldier(id));
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(id, info);
	level.world->setSoldierPosition(info.soldier, pac.position + turn.transform({0, 14, 15}));
	game::SoldierInput nine;
	nine.selectItem = 9; // held (0x004bba40): it opens once the fire test passes
	bool opened = false;
	float lowest = 1e9f;
	for (int i = 0; i < 30 * 4; ++i) {
		level.world->setInput(id, nine);
		level.world->tick(1 / 30.0f);
		opened = opened || !level.world->parachutes().empty();
		for (const auto& chute : level.world->parachutes()) {
			if (const world::Object* o = level.objects.find(chute.object)) {
				const physics::Vec3 at {o->position().x, o->position().y, o->position().z};
				lowest = std::min(lowest, turn.untransform(at - pac.position).y);
			}
		}
	}
	level.world->playerInfo(id, info);
	const float feet = turn.untransform(info.position - pac.position).y;
	std::fprintf(stderr, "  parachute down to %.1f, the soldier at %.1f over the Titan's origin\n", lowest, feet);
	CHECK(opened);
	CHECK(lowest > -1 && lowest < 5); // on the deck (-2.3), not through it
	CHECK(level.world->parachutes().empty() && !info.vehicle && info.alive);
	CHECK(feet > -3 && feet < -1.5f);
}

// A bot falling (AIDLL 0x10099b70: its node's velocity at most -1 m/s)
// holds input 22, so the parachute opens as soon as its fire test passes
// (2 m clear below, 10 m/s down): off the Titan's side, 170 m over the
// ground, it rides the parachute down and lands alive.
TEST(Titan_FallingBotOpensItsParachute)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Minsk", 48);
	if (!level.loaded) {
		return;
	}
	game::BotSettings settings;
	game::BotManager bots(*level.world, settings);
	level.afterTick = [&] { bots.update(1 / 30.0f); };
	const auto pac = *level.titan(1);
	const world::Object* titan = level.objects.find(pac.object);
	const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({titan->rotation().x, titan->rotation().y, titan->rotation().z});
	const int id = level.world->addPlayer("Faller", 1, true);
	CHECK(level.world->spawnSoldier(id));
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(id, info);
	CHECK(level.world->setSoldierPosition(info.soldier, pac.position + turn.transform({60, 9.5f, -3})));
	bool rode = false;
	for (int s = 0; s < 40 && !(rode && info.alive && !info.vehicle && std::fabs(info.velocity.y) < 1); ++s) {
		level.run(1);
		level.world->playerInfo(id, info);
		for (const auto& chute : level.world->parachutes()) {
			rode = rode || chute.player == id;
		}
	}
	std::fprintf(stderr, "  the bot rode a parachute: %d, %.1f m over the Titan at the end, alive %d\n", rode, info.position.y - pac.position.y, info.alive);
	CHECK(rode && info.alive && !info.vehicle);
	CHECK(info.position.y < pac.position.y - 100); // on the ground
}

// The gunship a Titan's AGSpawner holds (holdObject) is a stopped node, not a
// heavy moving object: a soldier running into it is neither squashed nor run
// over (1000 damage, no attacker: "is no more").
TEST(Titan_HeldGunshipDoesNotCrushSoldiers)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Suez_Canal", 48);
	if (!level.loaded) {
		return;
	}
	const int team = 2; // the human player's side in the reported session (--play 2)
	const auto titan = *level.titan(team);
	CHECK(level.world->moveTitan(team, 300, -200) == game::GameWorld::TitanMove::Accepted);
	level.run(5);
	uint32_t ag = 0;
	for (const auto& object : level.objects.objects()) {
		const std::string& name = object->objectTemplate() ? object->objectTemplate()->name() : std::string();
		if (name.size() == 5 && name.substr(2) == "_ag") {
			ag = object->id();
		}
	}
	CHECK(ag != 0);
	if (!ag) {
		return;
	}
	int deaths = 0, tries = 0;
	// Soldiers running at the held gunship from the -z side (yaw 0 faces +z).
	for (float dx = -6; dx <= 6; dx += 1.0f) {
		for (const bool sprint : {false, true}) {
			const int runner = level.world->addPlayer("Runner", titan.team, false);
			level.world->setSpawnTitan(runner, titan.object);
			if (!level.world->spawnSoldier(runner)) {
				continue;
			}
			game::GameWorld::PlayerInfo info;
			level.world->playerInfo(runner, info);
			const world::Vec3 at = level.objects.find(ag)->position();
			level.world->setSoldierPosition(info.soldier, {at.x + dx, 159.95f, at.z - 10});
			level.run(0.5f); // settles on the deck
			game::SoldierInput run;
			run.throttle = 1;
			run.sprint = sprint;
			level.world->setInput(runner, run);
			++tries;
			level.run(4);
			level.world->setInput(runner, {});
			level.world->playerInfo(runner, info);
			if (!info.alive) {
				++deaths;
				std::fprintf(stderr, "  died running at x offset %.1f%s\n", dx, sprint ? " sprinting" : "");
			}
		}
	}
	std::fprintf(stderr, "  %d of %d died\n", deaths, tries);
	CHECK(deaths == 0);
}

// A pilot in the held gunship of a moving Titan's AGSpawner starts the
// engine: the hold lets go at 0.1 rpm.
static void heldGunshipReleasesIntact(const char* mod)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Suez_Canal", 48, true, mod);
	if (!level.loaded) {
		return;
	}
	const int team = 2;
	const auto titan = *level.titan(team);
	CHECK(level.world->moveTitan(team, 300, -200) == game::GameWorld::TitanMove::Accepted);
	level.run(5);
	uint32_t ag = 0;
	for (const auto& object : level.objects.objects()) {
		const std::string& name = object->objectTemplate() ? object->objectTemplate()->name() : std::string();
		if (name.size() == 5 && name.substr(2) == "_ag") {
			ag = object->id();
		}
	}
	CHECK(ag != 0);
	if (!ag) {
		return;
	}
	const int pilot = level.world->addPlayer("Pilot", titan.team, false);
	level.world->setSpawnTitan(pilot, titan.object);
	CHECK(level.world->spawnSoldier(pilot));
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(pilot, info);
	const world::Vec3 at = level.objects.find(ag)->position();
	level.world->setSoldierPosition(info.soldier, {at.x, at.y, at.z - 4});
	level.run(0.5f);
	game::SoldierInput use;
	use.use = true;
	level.world->setInput(pilot, use);
	level.run(0.1f);
	level.world->setInput(pilot, {});
	level.run(0.5f);
	level.world->playerInfo(pilot, info);
	CHECK(info.vehicle == ag);
	const auto holding = [&] {
		for (const auto& s : level.world->spawners()) {
			if (s->lastSpawned() == ag) {
				return s->holding();
			}
		}
		return false;
	};
	CHECK(holding());
	const float y0 = level.objects.find(ag)->position().y;
	// The engine starts with the pilot aboard: the rotor reaches 0.1 rpm in
	// under a second and the hold lets go, the gunship at rest on the Titan
	// (its wheels' springs not kicking it off the deck).
	level.run(1);
	CHECK(!holding());
	CHECK(level.world->hitPoints(ag) > 900);
	// Full throttle: it lifts off the moving Titan.
	game::SoldierInput fly;
	fly.throttle = 1;
	level.world->setInput(pilot, fly);
	level.run(5);
	level.world->playerInfo(pilot, info);
	const float y1 = level.objects.find(ag)->position().y;
	std::fprintf(stderr, "  y %.2f (held at %.2f), pilot alive %d in vehicle %d, hp %.0f, rpm %.2f\n", y1, y0, info.alive,
		info.vehicle == ag, level.world->hitPoints(ag), level.world->vehicle(ag)->rpm());
	CHECK(info.alive && info.vehicle == ag);
	CHECK(level.world->hitPoints(ag) > 900);
	CHECK(y1 > y0 + 3);
}

TEST(Titan_HeldGunshipReleasesIntact)
{
	heldGunshipReleasesIntact(nullptr);
}

TEST(Titan_HeldGunshipReleasesIntactRemaster)
{
	heldGunshipReleasesIntact("mods/Project_Remaster_v17_Final");
}

// A walker crossing a Suez canal bridge. The legs' contacts with the deck are
// the legs' own record, solved without friction like the ground's (0x006ce740;
// the response's generic slot 0x2c is a bare return): through the generic
// friction a walker falling gravityModifier 4 onto the deck had 0.8 of its
// approach braked off its forward speed each tick (7.4 m/s to 1.5) and
// jittered, taking the bridge at a crawl. And the feet's probes (0x00650e90)
// meet the objects' meshes after the heightmap: over a deck both feet were
// "off the ground" and the walker was pushed sideways by 30 until it fell in.
struct BridgeCrossing {
	float x, z, yaw, deckY;
	float from = 55, to = 40; // metres before the middle to start at, after it to reach
	float lateral = 0;        // metres to the right of the bridge's axis (the lane)
};

static void walkerCrossesBridge(const char* mod, const char* tmpl, const BridgeCrossing& bridge, bool sprint, bool steering = false, bool survey = false, bool crowded = false, bool requireSmooth = false)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Suez_Canal", 48, true, mod, true);
	if (!level.loaded || !level.templates.find(tmpl)) {
		return;
	}
	const float rad = bridge.yaw * 3.14159265f / 180;
	const float dx = std::sin(rad), dz = std::cos(rad);
	const float from = bridge.from; // the approach road, short of the bridge
	const float x = bridge.x - dx * from, z = bridge.z - dz * from;
	world::Object* walker = level.objects.create(level.templates.find(tmpl), {x, level.world->terrainHeight(x, z) + 6, z}, {bridge.yaw, 0, 0});
	walker->setTeam(2);
	if (crowded) {
		// An occupied-looking obstruction in the lane: this tests collision
		// response, not the bot path planner or the screenshot's exact state.
		world::Object* blocker = level.objects.create(level.templates.find("us_heavy_mech"),
			{bridge.x, bridge.deckY + 4, bridge.z}, {bridge.yaw, 0, 0});
		blocker->setTeam(2);
	}
	level.run(2);
	const int player = level.world->addPlayer("Walker", 2, false);
	CHECK(level.world->spawnSoldier(player));
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(player, info);
	const world::Vec3 at = walker->position();
	level.world->setSoldierPosition(info.soldier, {at.x + dx * 1.5f, level.world->terrainHeight(at.x, at.z), at.z + dz * 1.5f});
	level.run(0.5f);
	game::SoldierInput use;
	use.use = true;
	level.world->setInput(player, use);
	level.run(0.2f);
	level.world->setInput(player, {});
	level.run(0.5f);
	level.world->playerInfo(player, info);
	CHECK(info.vehicle == walker->id());
	game::SoldierInput drive;
	drive.throttle = 1;
	drive.sprint = sprint;
	float lowestOnDeck = 1e9f, highestOnDeck = -1e9f, maxTickRise = 0, maxTickFall = 0, along = -from;
	float previousY = walker->position().y, riseAt = 0, fallAt = 0;
	float maxYawDelta = 0;
	const float initialYaw = walker->rotation().x;
	int samples = 0;
	const float hp = level.world->hitPoints(walker->id());
	for (int i = 0; i < static_cast<int>(30 * (from + bridge.to) / 5) && along < bridge.to; ++i) {
		// Small alternating steering pulses exercise seams without a long
		// open-loop turn carrying the vehicle into a railing.
		drive.strafe = steering ? ((i / 6) % 2 == 0 ? 0.2f : -0.2f) : 0;
		level.world->setInput(player, drive);
		level.world->tick(1 / 30.0f);
		const world::Vec3 p = walker->position();
		along = (p.x - bridge.x) * dx + (p.z - bridge.z) * dz;
		if (std::getenv("BF2142_BRIDGE_TRACE") && mod && bridge.x == 65 && !sprint && !steering && !crowded && std::fabs(along) < 5) {
			float vy = 0;
			for (const auto& v : level.world->vehicleList()) if (v.object == walker->id()) vy = v.velocity.y;
			std::fprintf(stderr, "  BRIDGETICK tick=%d along=%.3f y=%.4f dy=%+.4f vy=%+.4f hp=%.1f\n",
				i, along, p.y, p.y - previousY, vy, level.world->hitPoints(walker->id()));

		}
		if (std::fabs(along) < 15 || (bridge.from > 100 && !requireSmooth)) {
			lowestOnDeck = std::min(lowestOnDeck, p.y);
			highestOnDeck = std::max(highestOnDeck, p.y);
			if (p.y - previousY > maxTickRise) { maxTickRise = p.y - previousY; riseAt = along; }
			if (previousY - p.y > maxTickFall) { maxTickFall = previousY - p.y; fallAt = along; }
			maxYawDelta = std::max(maxYawDelta, std::fabs(std::remainder(walker->rotation().x - initialYaw, 360.0f)));
			++samples;
		}
		previousY = p.y;
	}
	std::fprintf(stderr, "  %s %s steering=%d crowded=%d at %.0f %.0f: along %.1f y-range %.3f..%.3f peak tick rise %.3f at %.1f fall %.3f at %.1f yaw-change %.2f samples %d hp %.0f/%.0f\n", mod ? "remaster" : "vanilla",
		tmpl, steering, crowded, bridge.x, bridge.z, along, lowestOnDeck, highestOnDeck, maxTickRise, riseAt, maxTickFall, fallAt, maxYawDelta, samples, level.world->hitPoints(walker->id()), hp);
	CHECK(samples > 0);
	if (requireSmooth) {
		// More than 5 cm in a 1/30-second tick is a visible jump on this
		// level deck. This catches the 16 cm alternating contact dropout.
		CHECK(maxTickRise < 0.05f);
		CHECK(maxTickFall < 0.05f);
	}
	if (!survey) {
		CHECK(along >= bridge.to);
		CHECK(lowestOnDeck > bridge.deckY + 2.0f);
		CHECK(level.world->hitPoints(walker->id()) >= hp);
	}
}

struct WalkResult {
	bool entered = false;
	float along = 0, lowest = 1e9f, hp = 0, hp0 = 0;
	float stuckAlong = 1e9f, stuckLateral = 0; // where it stopped for over a second short of the far side
	world::Vec3 stuckAt;
};

// One walker driven over `bridge` on its lane in an already loaded level; the
// walker is removed after, so the level serves the whole sweep.
static WalkResult walkBridge(TitanLevel& level, const char* tmpl, const BridgeCrossing& bridge, bool sprint)
{
	WalkResult r;
	const float rad = bridge.yaw * 3.14159265f / 180;
	const float dx = std::sin(rad), dz = std::cos(rad);
	const float from = bridge.from; // the approach road, short of the bridge
	const float x = bridge.x - dx * from + dz * bridge.lateral, z = bridge.z - dz * from - dx * bridge.lateral;
	world::Object* walker = level.objects.create(level.templates.find(tmpl), {x, level.world->terrainHeight(x, z) + 6, z}, {bridge.yaw, 0, 0});
	walker->setTeam(2);
	level.run(2);
	const int player = level.world->addPlayer("Walker", 2, false);
	CHECK(level.world->spawnSoldier(player));
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(player, info);
	const world::Vec3 at = walker->position();
	level.world->setSoldierPosition(info.soldier, {at.x + dx * 1.5f, level.world->terrainHeight(at.x, at.z), at.z + dz * 1.5f});
	level.run(0.5f);
	game::SoldierInput use;
	use.use = true;
	level.world->setInput(player, use);
	level.run(0.2f);
	level.world->setInput(player, {});
	level.run(0.5f);
	level.world->playerInfo(player, info);
	r.entered = info.vehicle == walker->id();
	game::SoldierInput drive;
	drive.throttle = 1;
	drive.sprint = sprint;
	r.along = -from;
	r.hp0 = level.world->hitPoints(walker->id());
	world::Vec3 window[30] = {};
	for (int i = 0; i < static_cast<int>(30 * (from + bridge.to) / 5) && r.along < bridge.to; ++i) {
		level.world->setInput(player, drive);
		level.world->tick(1 / 30.0f);
		const world::Vec3 p = walker->position();
		r.along = (p.x - bridge.x) * dx + (p.z - bridge.z) * dz;
		if (std::fabs(r.along) < 15 || bridge.from > 100) {
			r.lowest = std::min(r.lowest, p.y); // the heightmap is the canal bed far below
		}
		// Stopped: under 1 m moved in the last second, short of the far side.
		const world::Vec3 old = window[i % 30];
		window[i % 30] = p;
		if (i >= 90 && r.stuckAlong > 1e8f && r.along < bridge.to && std::hypot(p.x - old.x, p.z - old.z) < 1.0f) {
			r.stuckAlong = r.along;
			r.stuckAt = p;
		}
	}
	r.hp = level.world->hitPoints(walker->id());
	level.world->setInput(player, {});
	level.objects.remove(walker->id());
	level.run(0.2f);
	return r;
}

// The sweep: the Suez bridges both ways across the deck's lanes (the 12 m deck
// of bridge_industrial_01_c and the big road's two spans, whose joint at x -245
// was where a walker stopped: the hull's points against the rails and the
// seam's end faces braked it, which retail's walker response never takes,
// 0x00854170). Lanes within 6 m of the axis; the vanilla mech in vanilla, the
// Remaster's in Remaster. WALKER_SWEEP=1 logs every lane to 9 m, both mechs,
// walking and sprinting, and where each stopped.
TEST(Titan_WalkerSweepsTheSuezBridges)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	const bool all = std::getenv("WALKER_SWEEP") != nullptr;
	struct Span {
		float x, z, yaw, deckY, from, to;
	};
	const Span spans[] = {{65, -36, 90, 47.2f, 55, 40}, {-512, -36, 90, 47.2f, 55, 40}, {-99, -128, 0, 47.2f, 55, 40},
		{-245, -32, 90, 50.8f, 175, 130}};
	for (const char* mod : {static_cast<const char*>(nullptr), "mods/Project_Remaster_v17_Final"}) {
		TitanLevel level(gameDir, "Suez_Canal", 48, true, mod, true);
		if (!level.loaded) {
			continue;
		}
		for (const char* tmpl : {"us_heavy_mech", "as_heavy_mech"}) {
			if (!level.templates.find(tmpl) || (!all && (tmpl[0] == 'u') != (mod == nullptr))) {
				continue;
			}
			for (const Span& span : spans) {
				for (const bool back : {false, true}) {
					int index = 0;
					for (const float lane : {-9.0f, -6.0f, -3.0f, 0.0f, 3.0f, 6.0f, 9.0f}) {
						++index;
						for (const bool sprint : {false, true}) {
							if (!all && (std::fabs(lane) > 6 || sprint != (index & 1))) {
								continue;
							}
							BridgeCrossing b {span.x, span.z, span.yaw + (back ? 180.0f : 0.0f), span.deckY, span.from, span.to, lane};
							const WalkResult r = walkBridge(level, tmpl, b, sprint);
							const bool crossed = r.along >= span.to && r.lowest > span.deckY + 2.0f;
							if (all) {
								std::fprintf(stderr, "SWEEP %s %s span(%.0f,%.0f) %s lane %+.0f %s: along %.1f lowest %.1f %s", mod ? "rem" : "van",
									tmpl, span.x, span.z, back ? "back" : "fwd", lane, sprint ? "sprint" : "walk", r.along, r.lowest,
									crossed ? "CROSSED" : "SHORT");
								if (r.stuckAlong < 1e8f) {
									std::fprintf(stderr, " stuck at along %.1f pos %.1f %.1f %.1f", r.stuckAlong, r.stuckAt.x, r.stuckAt.y, r.stuckAt.z);
								}
								std::fprintf(stderr, "\n");
							} else {
								if (!crossed) {
									std::fprintf(stderr, "  %s %s span(%.0f,%.0f) %s lane %+.0f: along %.1f lowest %.1f stuck at %.1f %.1f %.1f\n",
										mod ? "rem" : "van", tmpl, span.x, span.z, back ? "back" : "fwd", lane, r.along, r.lowest,
										r.stuckAt.x, r.stuckAt.y, r.stuckAt.z);
								}
								CHECK(crossed);
							}
						}
					}
				}
			}
		}
	}
}

// A walker against a prop on level ground: how far along it gets in 8 s.
static float walkerAgainstProp(const char* prop, float yaw, float sink = 0)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return 1e9f;
	}
	TitanLevel level(gameDir, "Suez_Canal", 48, false, nullptr, false);
	if (!level.loaded || !level.templates.find("us_heavy_mech") || !level.templates.find(prop)) {
		return 1e9f;
	}
	// A flat 60 m run along x on the canal's west plain.
	float fx = 0, fz = 0;
	bool found = false;
	for (float z = -400; z < 400 && !found; z += 10) {
		for (float x = -700; x < 700 && !found; x += 10) {
			float lo = 1e9f, hi = -1e9f;
			for (float d = 0; d <= 60; d += 2) {
				for (float w = -6; w <= 6; w += 6) {
					const float h = level.world->terrainHeight(x + d, z + w);
					lo = std::min(lo, h);
					hi = std::max(hi, h);
				}
			}
			if (hi - lo < 0.05f && lo > 5) {
				fx = x, fz = z, found = true;
			}
		}
	}
	CHECK(found);
	if (!found) {
		return 1e9f;
	}
	const float y = level.world->terrainHeight(fx + 30, fz);
	level.objects.create(level.templates.find(prop), {fx + 30, y - sink, fz}, {yaw, 0, 0});
	world::Object* walker = level.objects.create(level.templates.find("us_heavy_mech"), {fx + 2, y + 6, fz}, {90, 0, 0});
	walker->setTeam(2);
	level.world->startRound();
	level.run(2);
	const int player = level.world->addPlayer("Walker", 2, false);
	CHECK(level.world->spawnSoldier(player));
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(player, info);
	level.world->setSoldierPosition(info.soldier, {fx + 3.5f, y, fz});
	level.run(0.5f);
	game::SoldierInput use;
	use.use = true;
	level.world->setInput(player, use);
	level.run(0.2f);
	level.world->setInput(player, {});
	level.run(0.5f);
	level.world->playerInfo(player, info);
	CHECK(info.vehicle == walker->id());
	game::SoldierInput drive;
	drive.throttle = 1;
	for (int i = 0; i < 240; ++i) {
		level.world->setInput(player, drive);
		level.world->tick(1 / 30.0f);
	}
	return walker->position().x - (fx + 30);
}

// The legs' sphere (1.5 m at the foot, 0.0x006cec60) decides what a walker meets
// on an object: a ledge low under the legs is climbed (the contact's push is
// up), a wall at their height stops it. container_seald_b is 2.6 m of box,
// sunk 2.2 m into the plain it leaves a 0.4 m ledge (under stepHeight 0.5).
TEST(Titan_WalkerClimbsALedgeAndStopsAtAWall)
{
	const float ledge = walkerAgainstProp("container_seald_b", 0, 2.2f);
	const float wall = walkerAgainstProp("container_seald_b", 0, 0);
	std::fprintf(stderr, "  ledge: %.1f m past it, wall: %.1f m\n", ledge, wall);
	if (ledge < 1e8f) {
		CHECK(ledge > 20); // over the ledge and on at the free pace (29.7 m)
		CHECK(wall < 0);   // held short of the wall (it stands at 0)
	}
}

TEST(Titan_WalkerCrossesTheSuezBridges)
{
	// The canal bridges of StaticObjects.con: bridge_industrial_01_c (65, -36),
	// (-512, -36) and (-99, -128), bc_big_bridge_a (-185, -32) and (-303, -32).
	// Walking and sprinting, with the vanilla and the Remaster mech.
	// The two bc_big_bridge_a spans are one road across the canal, driven on
	// the lane at z -32 from the west bank to the east bank.
	const BridgeCrossing bridges[] = {{65, -36, 90, 47.2f}, {-512, -36, 90, 47.2f}, {-99, -128, 0, 47.2f}, {-245, -32, 90, 50.8f, 175, 130}};
	for (const char* mod : {static_cast<const char*>(nullptr), "mods/Project_Remaster_v17_Final"}) {
		for (const BridgeCrossing& bridge : bridges) {
			for (const bool sprint : {false, true}) {
				walkerCrossesBridge(mod, "us_heavy_mech", bridge, sprint);
			}
		}
	}
}

// Both shipped walker models must stay in contact with Suez bridge decks,
// rather than alternate between the hull box and leg sphere.
TEST(Titan_WalkerBridgeContactDoesNotOscillate)
{
	for (const char* mod : {static_cast<const char*>(nullptr), "mods/Project_Remaster_v17_Final"}) {
		for (const char* vehicle : {"us_heavy_mech", "as_heavy_mech"}) {
			for (const bool sprint : {false, true}) {
				for (const BridgeCrossing& bridge : {BridgeCrossing {65, -36, 90, 47.2f},
						 BridgeCrossing {-512, -36, 90, 47.2f}, BridgeCrossing {-99, -128, 0, 47.2f},
						 BridgeCrossing {-245, -32, 90, 50.8f, 175, 130}})
					walkerCrossesBridge(mod, vehicle, bridge, sprint, false, false, false, true);
			}
		}
	}
}

// Record vertical movement and progress with more actual vehicles and small
// steering inputs. This survey records evidence; it is not a retail-motion
// acceptance threshold. The long bridge has joints, ramps and props.
TEST(Titan_SuezBridgeMotionSurvey)
{
    const BridgeCrossing bridge {-245, -32, 90, 50.8f, 175, 130};
    for (const char* mod : {static_cast<const char*>(nullptr), "mods/Project_Remaster_v17_Final"}) {
        for (const char* vehicle : {"us_heavy_mech", "as_heavy_mech", "eu_tank", "eu_apc"}) {
            for (bool steering : {false, true})
                walkerCrossesBridge(mod, vehicle, bridge, false, steering, true);
        }
    }
}

// Controlled collision survey with a second walker in the same lane.
// It is not a substitute for bot routing through the pictured underpass.
TEST(Titan_SuezBridgeCrowdingSurvey)
{
    for (const BridgeCrossing& bridge : {BridgeCrossing {65, -36, 90, 47.2f},
                                         BridgeCrossing {-512, -36, 90, 47.2f},
                                         BridgeCrossing {-245, -32, 90, 50.8f, 175, 130}})
        walkerCrossesBridge("mods/Project_Remaster_v17_Final", "us_heavy_mech", bridge, false, false, true, true);
}

// Suez's vanilla infantry mesh has no polygon near silos 901, 905 and 906, so a bot sent there cannot plan and walks
// straight into the first wall. A foot bot standing 60 m from silo 906 (the nearest) must prefer one it can plan to.
TEST(Titan_BotsPreferSilosOnTheNavMesh)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	TitanLevel level(gameDir, "Suez_Canal", 48, true, nullptr, true);
	std::vector<uint8_t> qtr, clb;
	if (!level.files.readFile("Levels/Suez_Canal/AIPathFinding/Infantry.qtr", qtr)
		|| !level.files.readFile("Levels/Suez_Canal/AIPathFinding/Infantry.clb", clb)) {
		return;
	}
	const auto mesh = ai::parseNavMesh(qtr, clb);
	CHECK(mesh.has_value());
	if (!mesh) return;
	const auto points = level.world->controlPointList();
	const auto cp = std::find_if(points.begin(), points.end(), [](const auto& p) { return p.id == 906; });
	CHECK(cp != points.end());
	if (cp == points.end()) return;
	CHECK(mesh->findPolygon({cp->position.x, cp->position.y, cp->position.z}) < 0); // the premise: 906 is off the mesh
	physics::Vec3 from;
	bool found = false;
	for (float x = -80; x <= 80 && !found; x += 2) {
		for (float z = -80; z <= 80 && !found; z += 2) {
			if (std::fabs(std::hypot(x, z) - 60) > 1.5f) continue;
			physics::Vec3 at = cp->position + physics::Vec3 {x, 0, z};
			at.y = level.world->terrainHeight(at.x, at.z) + 1;
			if (mesh->findPolygon({at.x, at.y, at.z}) >= 0) { from = at; found = true; }
		}
	}
	CHECK(found);
	if (!found) return;
	game::BotManager bots(*level.world, {});
	bots.setNavMesh(&*mesh);
	const int id = level.world->addPlayer("Chooser", 2, true);
	CHECK(level.world->spawnSoldier(id));
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(id, info);
	level.world->setSoldierPosition(info.soldier, from);
	for (int tick = 0; tick < 30 * 5; ++tick) {
		bots.update(1 / 30.0f);
		level.world->tick(1 / 30.0f);
	}
	const auto route = bots.route(id);
	CHECK(!route.empty());
	if (route.empty()) return;
	const float toOffMesh = std::hypot(route.back().x - cp->position.x, route.back().z - cp->position.z);
	std::fprintf(stderr, "  bot's goal is %.0f m from the off-mesh silo 906\n", toOffMesh);
	CHECK(toOffMesh > 100.0f);
}

// Rounds from above onto each part of a Titan (guns, consoles): which take
// the hit, and whether the shooter's hitmarker rises.
TEST(Titan_RoundsHitItsPartsAndRaiseTheHitmarker)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Minsk", 48);
	if (!level.loaded) {
		return;
	}
	level.run(1);
	const int shooter = level.world->addPlayer("Shooter", 1, true);
	const world::ObjectTemplate* rifle = level.templates.find("Unl_AV_Rifle");
	const std::string round = rifle && rifle->settings().count("projectileTemplate") ? rifle->settings().at("projectileTemplate") : "";
	CHECK(!round.empty());
	int tried = 0, struck = 0, marked = 0;
	for (const auto& part : level.world->titanParts(2)) {
		if (part.kind != game::GameWorld::TitanPartInfo::Gun && part.kind != game::GameWorld::TitanPartInfo::Console) {
			continue;
		}
		// From outside along the line from the Titan's middle through the gun.
		int covered = 0;
		physics::Vec3 out = part.position - level.titan(2)->position;
		out.y = 0;
		out = out * (1.0f / std::max(std::sqrt(physics::lengthSquared(out)), 1e-3f));
		const physics::Vec3 across {out.z, 0, -out.x};
		for (float dy = -3; dy <= 3; dy += 1.5f) {
			for (float side = -3; side <= 3; side += 1.5f) {
				const float was = level.world->hitPoints(part.id);
				level.world->launchProjectile(round, part.position + out * 25.0f + across * side + physics::Vec3 {0, dy, 0}, out * -300.0f, shooter);
				level.run(0.15f);
				covered += level.world->hitPoints(part.id) < was ? 1 : 0;
			}
		}
		std::fprintf(stderr, "  %s: %d of 25 rounds from outside hurt the part itself\n", part.name.c_str(), covered);
		float f = 1;
		uint32_t object = 0;
		const physics::Vec3 top = part.position + physics::Vec3 {0, 4, 0};
		const bool seg = level.world->segmentHit(top, part.position - physics::Vec3 {0, 1, 0}, 0, f, object);
		const float before = level.world->hitPoints(part.id);
		level.world->launchProjectile(round, top, {0, -300, 0}, shooter);
		level.run(0.2f);
		const bool hurt = level.world->hitPoints(part.id) < before;
		const bool mark = level.world->hitIndicator(shooter) > 0;
		const auto& im = level.world->impacts();
		std::fprintf(stderr, "  %s kind %d at %.0f %.0f %.0f: ray hit %d object %u (part %u) hurt %d marker %d impact %.0f %.0f %.0f obj %u\n",
			part.name.c_str(), part.kind, part.position.x, part.position.y, part.position.z, seg, object, part.id, hurt, mark,
			im.empty() ? 0.f : im.back().point.x, im.empty() ? 0.f : im.back().point.y, im.empty() ? 0.f : im.back().point.z,
			im.empty() ? 0u : im.back().object);
		++tried;
		struck += covered > 0 ? 1 : 0;
		(void)hurt;
		marked += mark;
		level.run(3);
	}
	CHECK(tried > 0);
	CHECK(struck >= 3); // the AA and ground cannons take rounds (their children's geometries)
	CHECK(marked > 0);
}

// Where a pod dropped over the enemy Titan comes to rest, shield up or down,
// at several speeds (the pod array's launch is 150 m/s).
TEST(Titan_PodsLandOnTheEnemyTitansRoof)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	for (const bool down : {false, true}) {
		TitanLevel level(dir, "Minsk", 48);
		if (!level.loaded) {
			return;
		}
		const auto pac = *level.titan(1);
		if (down) {
			level.world->setHitPoints(pac.object, pac.hitPoints * 0.5f);
			level.run(1);
		}
		const world::Object* object = level.objects.find(pac.object);
		const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y, object->rotation().z});
		world::ObjectTemplate* podTemplate = level.templates.find("eu_pod");
		if (!podTemplate) {
			return;
		}
		for (const float speed : {0.0f, 50.0f, 100.0f, 150.0f}) {
			for (const physics::Vec3 spot : {physics::Vec3 {-12, 0, -3}, {0, 0, -40}, {12, 0, -55}}) {
				const physics::Vec3 at = pac.position + turn.transform({spot.x, 150, spot.z});
				world::Object* pod = level.objects.create(podTemplate, {at.x, at.y, at.z}, {0, 0, 0});
				level.run(1 / 30.0f);
				level.world->setBodyVelocity(pod->id(), {0, -speed, 0});
				level.run(15);
				const physics::Vec3 rest = {pod->position().x, pod->position().y, pod->position().z};
				const physics::Vec3 rel = turn.untransform(rest - pac.position);
				std::fprintf(stderr, "  shield %s, %3.0f m/s down over %.0f/%.0f: rest at %.1f/%.1f/%.1f\n", down ? "down" : "up", speed, spot.x, spot.z,
					rel.x, rel.y, rel.z);
			}
		}
	}
}

// A grid of pods dropped over the PAC Titan with its shield down: where each rests.
TEST(Titan_PodGridOverTheEnemyTitan)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	TitanLevel level(dir, "Minsk", 48);
	if (!level.loaded) {
		return;
	}
	const auto pac = *level.titan(1);
	level.world->setHitPoints(pac.object, pac.hitPoints * 0.55f);
	level.run(1);
	const world::Object* object = level.objects.find(pac.object);
	const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y, object->rotation().z});
	std::fprintf(stderr, "  PAC yaw %.1f\n", object->rotation().x);
	world::ObjectTemplate* podTemplate = level.templates.find("eu_pod");
	if (!podTemplate) {
		return;
	}
	std::vector<std::pair<world::Object*, physics::Vec3>> pods;
	for (float x = -25; x <= 30; x += 5) {
		for (float z = -85; z <= 25; z += 5) {
			const physics::Vec3 at = pac.position + turn.transform({x, 120, z});
			pods.push_back({level.objects.create(podTemplate, {at.x, at.y, at.z}, {0, 0, 0}), {x, 0, z}});
		}
	}
	level.run(2 / 30.0f);
	for (auto& [pod, spot] : pods) {
		level.world->setBodyVelocity(pod->id(), {0, -40, 0});
	}
	level.run(14);
	for (float z = 25; z >= -85; z -= 5) {
		std::string row;
		for (auto& [pod, spot] : pods) {
			if (spot.z != z) {
				continue;
			}
			const physics::Vec3 rel = turn.untransform(physics::Vec3 {pod->position().x, pod->position().y, pod->position().z} - pac.position);
			char cell[16];
			std::snprintf(cell, sizeof cell, "%4.0f", rel.y < -99 ? -99.0f : rel.y);
			row += cell;
		}
		std::fprintf(stderr, "  z %4.0f:%s\n", z, row.c_str());
	}
	int inside = 0;
	for (auto& [pod, spot] : pods) {
		const physics::Vec3 rel = turn.untransform(physics::Vec3 {pod->position().x, pod->position().y, pod->position().z} - pac.position);
		if (rel.y > -20 && rel.y < 6.5f) {
			++inside;
			std::fprintf(stderr, "  pod over %.0f/%.0f rests inside at %.1f/%.1f/%.1f\n", spot.x, spot.z, rel.x, rel.y, rel.z);
		}
	}
	std::fprintf(stderr, "  %d of %zu pods came to rest below the roof line\n", inside, pods.size());
}

// The Shepherd landed on the enemy Titan's deck: E walks out on the deck, no
// pod from under the hull.
TEST(Titan_ShepherdOnTheDeckLeavesOnFoot)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	TitanLevel level(dir, "Minsk", 48);
	world::ObjectTemplate* shepherd = level.loaded ? level.templates.find("eu_at") : nullptr;
	if (!shepherd) {
		return;
	}
	const auto pac = *level.titan(1);
	const world::Object* object = level.objects.find(pac.object);
	const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({object->rotation().x, object->rotation().y, object->rotation().z});
	level.world->setHitPoints(pac.object, 6e6f * 0.59f);
	level.run(0.5f);
	const physics::Vec3 at = pac.position + turn.transform({0, 19.5f, -30}); // just over the rear deck, clear of the vehicle bay shield
	world::Object* vehicle = level.objects.create(shepherd, {at.x, at.y, at.z}, {object->rotation().x, 0, 0});
	vehicle->setTeam(2);
	level.run(6);
	const int me = level.world->addPlayer("Me", 2, false);
	const bool spawned = level.world->spawnSoldier(me);
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(me, info);
	const physics::Vec3 rest {vehicle->position().x, vehicle->position().y, vehicle->position().z};
	level.world->setSoldierPosition(info.soldier, {rest.x + 2.0f, rest.y - 1.0f, rest.z});
	game::SoldierInput use;
	use.use = true;
	level.world->setInput(me, use);
	level.run(0.1f);
	level.world->setInput(me, {});
	level.run(2);
	level.world->playerInfo(me, info);
	const uint32_t in = info.vehicle;
	level.world->setInput(me, use);
	level.run(0.1f);
	level.world->setInput(me, {});
	level.run(3);
	level.world->playerInfo(me, info);
	const physics::Vec3 rel = turn.untransform(info.position - pac.position);
	std::fprintf(stderr, "  spawned %d in %u, Shepherd rests at %.0f above the Titan origin; after E: vehicle %u, pods %zu, soldier at %.1f/%.1f/%.1f\n",
		spawned, in, turn.untransform(rest - pac.position).y, info.vehicle, level.world->pods().size(), rel.x, rel.y, rel.z);
	CHECK(spawned && in == vehicle->id());
	CHECK(level.world->pods().empty());
	CHECK(rel.y > 8); // out on the deck, not inside the hull
}

// Dropping off the PAC Titan's rear deck onto the catwalk below (Remaster's
// Minsk, as played): where the soldier ends up and whether he lives.
TEST(Titan_DropToTheRearCatwalk)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	for (const bool down : {false, true}) {
		TitanLevel level(dir, "Minsk", 48, true, "mods/Project_Remaster_v17_Final");
		if (!level.loaded) {
			return;
		}
		const auto pac = *level.titan(1);
		if (down) {
			level.world->setHitPoints(pac.object, pac.hitPoints * 0.55f);
			level.run(1);
		}
		const int me = level.world->addPlayer("Me", 2, false);
		CHECK(level.world->spawnSoldier(me));
		game::GameWorld::PlayerInfo info;
		level.world->playerInfo(me, info);
		// The log's last deck sample: world offsets -62.9 / 10.6 / -7.0 from the Titan's origin.
		for (const float edge : {-61.5f, -62.5f, -64.0f}) {
			level.world->playerInfo(me, info);
			if (!info.alive) {
				level.world->spawnSoldier(me);
				level.world->playerInfo(me, info);
			}
			level.world->setSoldierPosition(info.soldier, pac.position + physics::Vec3 {edge, 11.0f, 0.1f});
			float lowest = 1e9f, last = 0;
			for (int tick = 0; tick < 90; ++tick) {
				level.run(1 / 30.0f);
				level.world->playerInfo(me, info);
				lowest = std::min(lowest, info.position.y - pac.position.y);
				last = info.position.y - pac.position.y;
				if (!info.alive) {
					std::fprintf(stderr, "  shield %s, dropped at x %.0f: DIED at tick %d, y %.1f (above the origin), vy %.1f\n", down ? "down" : "up", edge, tick,
						last, info.velocity.y);
					break;
				}
			}
			if (info.alive) {
				std::fprintf(stderr, "  shield %s, dropped at x %.0f: alive, settled at y %.1f (lowest %.1f), hull %u\n", down ? "down" : "up", edge, last, lowest,
					level.world->titanHolding(info.position) ? 1u : 0u);
			}
		}
	}
}

// The coreroom hull test's footprint over the PAC Titan's rear (world offsets
// from its origin): '#' inside, '.' outside, at the catwalk's height and the deck's.
TEST(Titan_HullZoneAtTheRearCatwalk)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	TitanLevel level(dir, "Minsk", 48, true, "mods/Project_Remaster_v17_Final");
	if (!level.loaded) {
		return;
	}
	const auto pac = *level.titan(1);
	for (const float y : {5.2f, 10.6f}) {
		std::fprintf(stderr, "  y %.1f (rows z from +30 to -30, columns x from -80 to -20 step 4)\n", y);
		for (float z = 30; z >= -30; z -= 5) {
			std::string row;
			for (float x = -80; x <= -20; x += 4) {
				row += level.world->titanHolding(pac.position + physics::Vec3 {x, y, z}) ? '#' : '.';
			}
			std::fprintf(stderr, "   z %4.0f %s\n", z, row.c_str());
		}
	}
}

TEST(Titan_SuezTitanPositions)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	TitanLevel level(dir, "Suez_Canal", 48);
	if (!level.loaded) {
		return;
	}
	for (int team : {1, 2}) {
		if (const auto* t = level.titan(team)) {
			std::fprintf(stderr, "  team %d Titan at %.0f/%.0f/%.0f radius %.0f\n", team, t->position.x, t->position.y, t->position.z, t->radius);
		}
	}
	std::fprintf(stderr, "  terrain under it: %.0f\n", level.world->terrainHeight(level.titan(2)->position.x, level.titan(2)->position.z));
}

// The 8 Nov Suez log: the local soldier stood at 8.0/8.6/-18.0 from the enemy
// Titan's origin (shield up), stepped to 8.9/-18.7 and fell through to y 4.0
// and past it (127 damage). Drop soldiers over a grid there: where do they stand?
TEST(Titan_SuezInteriorFloorProbe)
{
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	TitanLevel level(dir, "Suez_Canal", 48, true, "mods/Project_Remaster_v17_Final");
	if (!level.loaded || !level.titan(2)) {
		return;
	}
	const auto ti = *level.titan(2);
	const int me = level.world->addPlayer("Me", 1, false);
	for (float z = -17.0f; z >= -19.0f; z -= 0.5f) {
		std::string row;
		for (float x = 7.0f; x <= 10.0f; x += 0.5f) {
			game::GameWorld::PlayerInfo info;
			level.world->playerInfo(me, info);
			if (!info.alive) {
				level.world->spawnSoldier(me);
				level.world->playerInfo(me, info);
			}
			level.world->setSoldierPosition(info.soldier, ti.position + physics::Vec3 {x, 9.0f, z});
			float fastest = 0;
			for (int tick = 0; tick < 60 && info.alive; ++tick) {
				level.run(1 / 30.0f);
				level.world->playerInfo(me, info);
				fastest = std::min(fastest, info.velocity.y);
			}
			CHECK(info.alive); // landing in the standing Titan's pit is not a road kill
			char cell[48];
			std::snprintf(cell, sizeof cell, " %s%4.1f(%3.0f v%.0f h%u)", info.alive ? "" : "X", info.position.y - ti.position.y,
				level.world->hitPoints(info.soldier), fastest, level.world->titanHolding(info.position) ? 1u : 0u);
			row += cell;
		}
		std::fprintf(stderr, "  z %5.1f:%s\n", z, row.c_str());
	}
}

// Both Titan AA positions share the firearm but own independent aim and fire.
TEST(Titan_AASeatAimAndFireOwnership)
{
 const char* dir = std::getenv("BF2142_GAME_DIR");
 if (!dir) return;
 for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
  TitanLevel level(dir, "Minsk", 48, true, mod);
  CHECK(level.loaded);
  if (!level.loaded) continue;
  const int player = level.world->joinPlayer(2);
  CHECK(level.world->spawnSoldier(player));
  anim::AnimationSystems systems(nullptr);
  PlayerCamera camera(*level.world, systems, nullptr);
  game::GameWorld::PlayerInfo info;
  CHECK(level.world->playerInfo(player, info));
  const auto* titan = level.titan(2);
  CHECK(titan != nullptr);
  if (!titan) continue;
  CHECK(level.world->setSoldierPosition(info.soldier, titan->position + physics::Vec3 {0, 11, -30}));
  physics::Vec3 entry; float radius = 0; int seat = -1;
  CHECK(level.world->titanGunEntry(player, entry, radius, seat));
  CHECK(level.world->setSoldierPosition(info.soldier, entry - physics::Vec3 {0, 1, 0}));
  game::SoldierInput input; input.use = true;
  level.world->setInput(player, input); level.run(0.1f);
  const uint32_t id = level.world->titanOf(2);
  const auto seats = level.world->vehicleSeats(id);
  int count = 0;
  for (size_t i = 0; i < seats.size(); ++i) {
   if (seats[i].name.find("aacannon") == std::string::npos) continue;
   ++count;
   input = {}; input.switchSeat = static_cast<int>(i) + 1;
   level.world->setInput(player, input); level.run(0.1f);
   input = {}; input.aimPitch = 30;
   level.world->setInput(player, input); level.run(0.1f);
   float yaw = 0, pitch = 0;
   CHECK(level.world->gunAim(id, static_cast<int>(i), yaw, pitch));
   CHECK(std::fabs(pitch - 30) < 0.5f);
   world::Vec3 off, rotation;
   CHECK(level.world->partPose(id, seats[i].pitchBundle, off, rotation));
   CHECK(std::fabs(rotation.y + 30) < 0.5f);
   camera.tick(1 / 30.0f);
   PlayerCamera::View view;
   CHECK(camera.view(view, 16.0f / 9.0f));
   CHECK(std::fabs(view.pitch - 30.0f * 3.14159265f / 180.0f) < 0.03f);
   level.world->takeObjectWeaponFires();
   input.fire = true; level.world->setInput(player, input); level.run(1.2f);
   const auto events = level.world->takeObjectWeaponFires();
   const auto found = events.find(id);
   CHECK(found != events.end());
   if (found != events.end()) {
    for (const auto& event : found->second) CHECK(event.seat == seats[i].name);
   }
   input = {}; level.world->setInput(player, input); level.run(0.2f);
  }
  CHECK(count == 2);
 }
}

TEST(Titan_GroundGunCameraAndProjectileFollowEachSeat)
{
 const char* dir = std::getenv("BF2142_GAME_DIR");
 if (!dir) return;
 for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
 for (int team : {1, 2}) {
  TitanLevel level(dir, "Minsk", 48, true, mod);
  CHECK(level.loaded);
  if (!level.loaded) continue;
  const int player = level.world->joinPlayer(team);
  CHECK(level.world->spawnSoldier(player));
  game::GameWorld::PlayerInfo info;
  CHECK(level.world->playerInfo(player, info));
  const auto* titan = level.titan(team);
  if (!titan) { CHECK(titan); continue; }
  CHECK(level.world->setSoldierPosition(info.soldier, titan->position + physics::Vec3 {0, 11, -30}));
  physics::Vec3 entry; float radius = 0; int seat = -1;
  CHECK(level.world->titanGunEntry(player, entry, radius, seat));
  CHECK(level.world->setSoldierPosition(info.soldier, entry - physics::Vec3 {0, 1, 0}));
  game::SoldierInput input; input.use = true;
  level.world->setInput(player, input); level.run(0.1f);
  const uint32_t id = level.world->titanOf(team);
  const auto seats = level.world->vehicleSeats(id);
  anim::AnimationSystems systems(nullptr);
  PlayerCamera camera(*level.world, systems, nullptr);
  int count = 0;
  for (size_t i = 0; i < seats.size(); ++i) {
   if (seats[i].name.find("groundcanone3") == std::string::npos) continue;
   ++count;
   input = {}; input.switchSeat = static_cast<int>(i) + 1;
   level.world->setInput(player, input); level.run(0.1f);
   input = {}; input.turn = 15;
   level.world->setInput(player, input); level.run(1 / 30.0f);
   for (const float aim : {-15.0f, -40.0f}) {
    input = {}; input.aimPitch = aim;
    level.world->setInput(player, input); level.run(0.1f);
    camera.tick(1 / 30.0f);
    PlayerCamera::View view;
    CHECK(camera.view(view, 16.0f / 9.0f));
    std::fprintf(stderr, "ground %s team %d seat %zu aim %.1f view %.1f yaw %.1f\n", mod, team, i, aim, view.pitch * 180 / 3.14159265f, view.yaw * 180 / 3.14159265f);
    CHECK(std::fabs(view.pitch - aim * 3.14159265f / 180.0f) < 0.04f);
   }
   input = {}; input.aimPitch = -40; input.fire = true;
   level.world->setInput(player, input); level.run(0.2f);
   const auto& projectiles = level.world->projectiles();
   const game::Projectile* shot = nullptr;
   for (const auto& p : projectiles) if (p.player == player && p.age < 0.25f) shot = &p;
   CHECK(shot != nullptr);
   if (shot) {
    PlayerCamera::View view; CHECK(camera.view(view, 16.0f / 9.0f));
    const float len = std::sqrt(physics::lengthSquared(shot->velocity));
    const float shotPitch = std::asin(shot->velocity.y / len);
    CHECK(std::fabs(shotPitch - view.pitch) < 0.12f);
    const float shotYaw = std::atan2(shot->velocity.x, shot->velocity.z);
    std::fprintf(stderr, "shot yaw %.1f view %.1f age %.2f\n", shotYaw * 180 / 3.14159265f, view.yaw * 180 / 3.14159265f, shot->age);
    CHECK(std::fabs(std::remainder(shotYaw - view.yaw, 2 * 3.14159265f)) < 0.12f);
   }
   input = {}; level.world->setInput(player, input); level.run(0.3f);
  }
  CHECK(count == 4);
 }
 }
}

TEST(Titan_VehicleSeatHudUsesAuthoredIconOffsets)
{
 const char* dir = std::getenv("BF2142_GAME_DIR");
 if (!dir) return;
 for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
  TitanLevel level(dir, "Minsk", 48, true, mod);
  CHECK(level.loaded);
  if (!level.loaded) continue;
  hud::HudManager hud([&](const std::string& path, std::string& text) {
   std::vector<uint8_t> bytes;
   if (!level.files.readFile(path, bytes)) return false;
   text.assign(bytes.begin(), bytes.end()); return true;
  });
  hud::registerHudConsoleMethods(level.console, hud);
  level.console.setScriptOutput(false, false);
  std::string out;
  CHECK(level.console.run("Menu/HUD/HudSetup/HudSetupMain.con", {}, out));
  hud.set("ShowIngameHud", 1); hud.set("ShowSoldierHud", 1); hud.set("ShowVehicleHud", 1);
  hud.set("VehicleIconShow", 1); hud.set("MenuBackgroundAlpha", 1);
  hud.setString("VehicleIconPathString", "test_vehicle_silhouette.tga");
  for (const char* name : {"eu_titan_playtest", "as_titan_playtest", "eu_tank", "eu_apc", "as_fav", "us_ag"}) {
   auto* t = level.templates.find(name);
   CHECK(t != nullptr);
   if (!t) continue;
   auto* o = level.objects.create(t, {});
   const auto seats = level.world->vehicleSeats(o->id());
   menu::VehicleHudInput input; input.vehicle = t;
   for (const auto& seat : seats) {
    menu::SeatDot dot;
    const auto* st = level.templates.find(seat.name);
    const std::string pos = menu::hudSetting(st, "vehicleHud.vehicleIconPos");
    if (std::sscanf(pos.c_str(), "%f/%f", &dot.x, &dot.y) == 2) dot.state = 4;
    input.seats.push_back(dot);
   }
   menu::VehicleHud vehicleHud;
   vehicleHud.update(hud, input);
   const auto quads = hud.frame(0);
   const hud::HudQuad* icon = nullptr;
   std::vector<const hud::HudQuad*> dots;
   for (const auto& q : quads) {
    if (q.texture.find("test_vehicle_silhouette") != std::string::npos) icon = &q;
    if (q.texture.find("vehiclePositionEmpty") != std::string::npos) dots.push_back(&q);
   }
   CHECK(icon != nullptr);
   size_t expected = 0;
   for (const auto& dot : input.seats) {
    if (!dot.state || (dot.x == 0 && dot.y == 0)) continue;
    CHECK(expected < dots.size());
    if (icon && expected < dots.size()) {
     CHECK(std::fabs(dots[expected]->x0 - (icon->x0 - 1 + dot.x)) < 0.01f);
     CHECK(std::fabs(dots[expected]->y0 - (icon->y0 + dot.y)) < 0.01f);
     CHECK(std::fabs(dots[expected]->x1 - dots[expected]->x0 - 8) < 0.01f);
     CHECK(std::fabs(dots[expected]->y1 - dots[expected]->y0 - 8) < 0.01f);
    }
    ++expected;
   }
   std::fprintf(stderr, "seat HUD %s %s: %zu authored seats, %zu drawn dots\n", mod, name, expected, dots.size());
   CHECK(expected >= 2 && expected == dots.size());
  }
 }
}

// Collision and damage are separate: friendly fire settings must not remove
// the Titan's projectile faces. Exercise the installed stationary AA rounds
// against both teams' actual Titans, including the approach from below.
TEST(Titan_StationaryAARoundsMeetFriendlyAndEnemyTitan)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		TitanLevel level(gameDir, "Sidi_Power_Plant", 48, true, mod);
		CHECK(level.loaded);
		if (!level.loaded) continue;
		for (int team = 1; team <= 2; ++team) {
			const auto* found = level.titan(team);
			CHECK(found != nullptr);
			if (!found) continue;
			const auto titan = *found;
			const auto parts = level.world->titanParts(team);
			for (int shooterTeam : {team, 3 - team}) {
				const int shooter = level.world->addPlayer("AA test", shooterTeam, true);
				for (const char* round : {"static_aa_cannon_Projectile", "static_aa_missile_projectile"}) {
					for (int direction : {-1, 1}) {
						const auto from = titan.position + physics::Vec3 {0, direction * 60.0f, 0};
						const size_t before = level.world->impacts().size();
						CHECK(level.world->launchProjectile(round, from, {0, -direction * 300.0f, 0}, shooter));
						level.run(0.4f);
						const auto& impacts = level.world->impacts();
						bool hit = false;
						for (size_t i = before; i < impacts.size(); ++i) {
							const uint32_t object = impacts[i].object;
							hit |= object == titan.object || std::any_of(parts.begin(), parts.end(),
								[object](const auto& part) { return part.id == object; });
						}
						std::fprintf(stderr, "  %s titan=%d shooter=%d round=%s approach=%s impact=%d\n",
							mod, team, shooterTeam, round, direction < 0 ? "below" : "above", hit);
						CHECK(hit);
					}
				}
			}
		}
	}
}

TEST(Titan_FriendlyStationaryAAGunFiresAtTitan)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (bool missile : {false, true}) {
		TitanLevel level(gameDir, "Sidi_Power_Plant", 48, true, "mods/Project_Remaster_v17_Final");
		CHECK(level.loaded);
		if (!level.loaded) return;
		const auto titan = *level.titan(2);
		auto* aaTemplate = level.templates.find("static_aa");
		CHECK(aaTemplate != nullptr);
		if (!aaTemplate) return;
		const auto pos = titan.position + physics::Vec3 {0, -40, -80};
		auto* gun = level.objects.create(aaTemplate, {pos.x, pos.y, pos.z});
		gun->setTeam(2);
		const uint32_t gunId = gun->id();
		level.run(0.2f);
		const int player = level.world->addPlayer("AA gunner", 2, false);
		CHECK(level.world->spawnSoldier(player));
		game::GameWorld::PlayerInfo info;
		level.world->playerInfo(player, info);
		bool entered = false;
		for (float r : {0.0f, 1.5f, 3.0f, 4.5f}) {
			for (int k = 0; k < 8 && !entered; ++k) {
				const float a = k * 0.785398f;
				level.world->setSoldierPosition(info.soldier, pos + physics::Vec3 {r * std::sin(a), 0, r * std::cos(a)});
				game::SoldierInput input;
				input.use = true;
				level.world->setInput(player, input);
				level.run(0.067f);
				level.world->setInput(player, {});
				level.run(0.067f);
				level.world->playerInfo(player, info);
				entered = info.vehicle == gunId;
			}
			if (entered) break;
		}
		CHECK(entered);
		if (!entered) return;
		game::SoldierInput input;
		input.aimPitch = 27;
		level.world->setInput(player, input);
		level.run(2.0f);
		{
			const auto before = level.world->impacts().size();
			input.fire = !missile;
			input.altFire = missile;
			level.world->setInput(player, input);
			level.run(1.0f);
			level.world->setInput(player, {});
			level.run(2.0f);
			bool hit = false;
			const auto parts = level.world->titanParts(2);
			const auto& impacts = level.world->impacts();
			for (size_t i = before; i < impacts.size(); ++i) {
				const auto object = impacts[i].object;
				hit |= object == titan.object || std::any_of(parts.begin(), parts.end(), [object](const auto& part) { return part.id == object; });
			}
			std::fprintf(stderr, "  stationary AA %s Titan impact=%d\n", missile ? "missile" : "cannon", hit);
			CHECK(hit);
		}
	}
}

TEST(Titan_FriendlyWalkerAAGunOnSuez)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	const bool crowded = std::getenv("BF2142_TITAN_CROWD_TEST") != nullptr;
	for (bool alt : {false, true}) for (bool down : {false, true}) {
		geom::GeometryTemplateManager geometries;
		TitanLevel level(gameDir, "Suez_Canal", 48, true, "mods/Project_Remaster_v17_Final", crowded, &geometries);
		CHECK(level.loaded);
		if (!level.loaded) return;
		const auto titan = *level.titan(2);
		if (down) { level.world->setHitPoints(titan.object, titan.maxHitPoints * 0.59f); level.run(0.5f); }
		auto* vehicle = level.templates.find("us_heavy_mech");
		CHECK(vehicle != nullptr);
		if (!vehicle) return;
		physics::Vec3 pos = titan.position + physics::Vec3 {0, 0, -100};
		pos.y = level.world->terrainHeight(pos.x, pos.z) + 3;
		auto* made = level.objects.create(vehicle, {pos.x, pos.y, pos.z});
		made->setTeam(2);
		const uint32_t walker = made->id();
		level.run(2);
		game::BotManager bots(*level.world, {});
		if (crowded) {
			for (int i = 0; i < 43; ++i) {
				const int bot = level.world->addPlayer("Crowd", i % 2 + 1, true);
				CHECK(level.world->spawnSoldier(bot));
			}
			level.afterTick = [&bots] { bots.update(1.0f / 30); };
		}
		const int player = level.world->addPlayer("Walker gunner", 2, false);
		CHECK(level.world->spawnSoldier(player));
		game::GameWorld::PlayerInfo info;
		level.world->playerInfo(player, info);
		bool entered = false;
		for (float r : {0.0f, 2.0f, 4.0f, 6.0f}) {
			for (int k = 0; k < 8 && !entered; ++k) {
				const float a = k * 0.785398f;
				const auto p = level.objects.find(walker)->position();
				level.world->setSoldierPosition(info.soldier, {p.x + r * std::sin(a), p.y, p.z + r * std::cos(a)});
				game::SoldierInput input;
				input.use = true;
				level.world->setInput(player, input);
				level.run(0.067f);
				level.world->setInput(player, {});
				level.run(0.067f);
				level.world->playerInfo(player, info);
				entered = info.vehicle == walker;
			}
			if (entered) break;
		}
		CHECK(entered);
		if (!entered) continue;
		game::SoldierInput input;
		input.switchSeat = 2;
		level.world->setInput(player, input);
		level.run(0.067f);
		level.world->playerInfo(player, info);
		CHECK(info.seat == 1);
		input = {};
		const auto p = level.objects.find(walker)->position();
		input.aimPitch = std::atan2(titan.position.y - p.y, titan.position.z - p.z) * 57.29578f;
		level.world->setInput(player, input);
		level.run(2);
		input.fire = !alt;
		input.altFire = alt;
		level.world->setInput(player, input);
		level.run(1.0f / 30);
		uint64_t serial = 0;
		for (const auto& shot : level.world->projectiles()) {
			if (shot.player == player && shot.settings) {
				serial = shot.serial;
				CHECK(shot.settings->name == (alt ? "us_heavy_mech_AA_missile" : "us_heavy_mech_AAcannon_Projectile"));
			}
		}
		CHECK(serial != 0);
		level.world->setInput(player, {});
		const auto parts = level.world->titanParts(2);
		size_t previous = level.world->impacts().size();
		bool hit = false, removed = false;
		for (int tick = 0; tick < 90 && !hit; ++tick) {
			level.run(1.0f / 30);
			const auto& impacts = level.world->impacts();
			for (size_t i = previous; i < impacts.size(); ++i) {
				const auto object = impacts[i].object;
				hit |= object == titan.object || std::any_of(parts.begin(), parts.end(),
					[object](const auto& part) { return part.id == object; });
			}
			previous = impacts.size();
			if (hit) removed = std::none_of(level.world->projectiles().begin(), level.world->projectiles().end(),
				[serial](const auto& shot) { return shot.serial == serial; });
		}
		std::fprintf(stderr, "  walker %s shield=%s Titan impact=%d projectile removed=%d\n",
			alt ? "missile" : "cannon", down ? "down" : "up", hit, removed);
		CHECK(hit && removed);
	}
}

// The launcher trace must capture a real local projectile's lifecycle and its
// Titan transform; otherwise a player reproduction cannot diagnose crossing.
TEST(Titan_WalkerShotTraceRecordsLifecycle)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	TitanLevel level(gameDir, "Suez_Canal", 48, true, "mods/Project_Remaster_v17_Final");
	CHECK(level.loaded);
	if (!level.loaded) return;
	CHECK(level.world->spawnPlayer(2) != nullptr);
	const auto titan = *level.titan(2);
	const int player = level.world->localPlayer();
	std::FILE* out = std::tmpfile();
	CHECK(out != nullptr);
	if (!out) return;
	host::TitanShotTrace trace;
	CHECK(level.world->launchProjectile("us_heavy_mech_AA_missile", titan.position + physics::Vec3 {0, -60, 0}, {0, 300, 0}, player));
	trace.update(*level.world, level.objects, out);
	for (int i = 0; i < 30; ++i) { level.run(1.0f / 30); trace.update(*level.world, level.objects, out); }
	std::rewind(out);
	std::string output;
	char buffer[1024];
	while (const size_t n = std::fread(buffer, 1, sizeof(buffer), out)) output.append(buffer, n);
	std::fclose(out);
	CHECK(output.find("titan-shot: start") != std::string::npos);
	CHECK(output.find("titan-shot: step") != std::string::npos);
	CHECK(output.find("shield=1 mesh=1") != std::string::npos);
	CHECK(output.find("titan-shot: end") != std::string::npos);
	CHECK(output.find("titan-shot: recent-impact") != std::string::npos);
}
