// Titan mode on the shipped levels (needs $BF2142_GAME_DIR): Minsk's and
// Minsk_Titan16's gpm_ti gameplay objects with every object template, the
// terrain, the collision meshes and the materials. The Titans spawn with
// their parts and spawn points, the missile silos fire at the enemy Titan
// and wear its shield and then its hull down, the core's sequence (four
// consoles, the doors, the core) destroys a Titan, the commander's order
// moves it, and gpm_ti.py keeps the tickets and ends the round.
#include "Game/Bots.h"
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
	for (; second < 60 && level.world->ridingPod(bot); second += 0.5f) {
		level.run(0.5f);
	}
	level.world->playerInfo(bot, info);
	std::fprintf(stderr, "  the pod landed after %.1f s at %.0f/%.0f/%.0f (from %.0f/%.0f/%.0f)\n", second, info.position.x,
		info.position.y, info.position.z, entry.x, entry.y, entry.z);
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
// the EU Titan's): an object passes its own team's and meets the other's,
// but the shipped fields' meshes carry no soldier LOD, so soldiers of both
// teams walk through (a mesh without a soldier LOD stops no soldier) while
// rounds meet them (their projectile LOD); the Titan's object spawners
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
		int blocked = 0, walked = 0;
		for (const physics::Vec3& offset : fields) {
			const physics::Vec3 at = titan.position + turn.transform(offset);
			for (const physics::Vec3 axis : {physics::Vec3 {1, 0, 0}, physics::Vec3 {0, 0, 1}}) {
				for (const float up : {0.3f, 1.0f, 1.6f}) {
					const physics::Vec3 a = at + turn.transform(axis * 3.0f + physics::Vec3 {0, up, 0});
					const physics::Vec3 b = at + turn.transform(axis * -3.0f + physics::Vec3 {0, up, 0});
					blocked += level.world->lineOfSight(a, b) ? 0 : 1;
					walked += level.world->soldierPathClear(1, a, b) == level.world->soldierPathClear(2, a, b) ? 1 : 0;
				}
			}
		}
		std::fprintf(stderr, "  %s Titan's spawn room field: %d of 6 ways stop rounds, %d the same for both teams' soldiers\n",
			team == 1 ? "PAC" : "EU", blocked, walked);
		CHECK(blocked > 0 && walked == 6);
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
	std::vector<int> crew;
	for (int i = 0; i < 6; ++i) {
		const int id = level.world->addPlayer("Crew" + std::to_string(i), 1, true);
		CHECK(level.world->spawnSoldier(id));
		game::GameWorld::PlayerInfo info;
		level.world->playerInfo(id, info);
		// In the Titan's spawn room deck.
		CHECK(level.world->setSoldierPosition(info.soldier, pac.position + physics::Vec3 {static_cast<float>(i) - 3, 9.5f, -40}));
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
