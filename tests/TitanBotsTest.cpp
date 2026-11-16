#include "AI/NavMesh.h"
#include "Game/Bots.h"
#include "TitanLevel.h"
#include "World/TemplateFiles.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <vector>

using namespace dice::hfe;
using titantest::TitanLevel;

namespace {

void loadAircraftAi(TitanLevel& level, game::BotSettings& settings)
{
	game::registerBotConsoleMethods(level.console, settings);
	std::string out;
	CHECK(level.console.run("AI/AIBehaviours.ai", {}, out));
	CHECK(level.console.run("AI/AIDefault.ai", {}, out));
	for (const std::string& script : world::templateScripts(level.files, "Objects/", true)) {
		if (script.size() > 3 && script.compare(script.size() - 3, 3, ".ai") == 0) {
			level.console.run(script, {}, out);
		}
	}
}

} // namespace

TEST(Titan_BotsReachGunConsolesFromSpawnRoom)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) continue;
		TitanLevel level(gameDir, "Bridge_at_Remagen", 48, true, mod);
		CHECK(level.loaded);
		if (!level.loaded) continue;
		game::BotSettings settings;
		const bool enhanced = true;
		settings.enhanced = &enhanced;
		settings.botTitan = &enhanced;
		game::BotManager bots(*level.world, settings);
		level.afterTick = [&] { bots.update(1 / 30.0f); };
		std::map<int, std::vector<int>> crews;
		for (int team : {1, 2}) {
			const auto* titan = level.titan(team);
			CHECK(titan != nullptr);
			if (!titan) continue;
			for (int i = 0; i < 6; ++i) {
				const int id = level.world->addPlayer("Crew" + std::to_string(team) + std::to_string(i), team, true);
				CHECK(level.world->spawnSoldier(id));
				game::GameWorld::PlayerInfo info;
				CHECK(level.world->playerInfo(id, info));
				CHECK(level.world->setSoldierPosition(info.soldier,
					level.world->titanToWorld(titan->object, {static_cast<float>(i * 3) - 7.5f, 11.5f, -40})));
				crews[team].push_back(id);
			}
		}
		level.run(40);
		for (const auto& [team, crew] : crews) {
			const uint32_t titan = level.world->titanOf(team);
			int gunners = 0;
			for (int id : crew) {
				game::GameWorld::PlayerInfo info;
				CHECK(level.world->playerInfo(id, info));
				gunners += info.vehicle == titan;
			}
			std::fprintf(stderr, "  %s team %d: %d of 6 bots manning guns\n", mod, team, gunners);
			CHECK(gunners == 3);
		}
	}
}

// enhanced.botTitan: bots aboard their Titan crew its deck gunship and fly
// it off, and fly its transport to a silo with riders who drop in pods.
TEST(Titan_BotsFlyDeckAircraftFromTitan)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) continue;
		TitanLevel level(gameDir, "Bridge_at_Remagen", 48, true, mod);
		CHECK(level.loaded);
		if (!level.loaded) continue;
		// The shipped AI scripts, as main runs them: the deck aircraft are
		// known as aircraft by their AI templates.
		game::BotSettings settings;
		loadAircraftAi(level, settings);
		settings.viewDistance = 300; // No level fog is loaded by this fixture.
		const bool enhanced = true;
		settings.enhanced = &enhanced;
		settings.botTitan = &enhanced;
		game::BotManager bots(*level.world, settings);
		struct Team {
			uint32_t gunship = 0, transport = 0;
			std::vector<int> crew;
			bool gunshipFlown = false, transportFlown = false, normalFlight = false, restartedSortie = false;
			bool spawnChecked = false, spawnedInFlight = false;
			int pilot = -1, normalTicks = 0, supportHoldTicks = 0;
			int maxRiders = 0, drops = 0;
			uint32_t gunShots = 0;
			physics::Vec3 supportAt;
			bool supportStarted = false, supportHeld = false, repositioned = false, gunnersRetained = false;
			std::map<int, bool> rode;
		};
		std::map<int, Team> teams;
		for (int team : {1, 2}) {
			const auto* titan = level.titan(team);
			CHECK(titan != nullptr);
			if (!titan) continue;
			Team& t = teams[team];
			for (const auto& v : level.world->vehicleList()) {
				const physics::Vec3 l = level.world->titanToLocal(titan->object, v.position);
				const auto* o = level.world->objects().find(v.object);
				if (!o || !o->objectTemplate() || std::abs(l.x) > 40 || std::abs(l.z) > 100 || std::abs(l.y) > 30) continue;
				const std::string& name = o->objectTemplate()->name();
				(name.find("_ag") != std::string::npos ? t.gunship : t.transport) = v.object;
			}
			CHECK(t.gunship && t.transport);
			for (int i = 0; i < 8; ++i) {
				const int id = level.world->addPlayer("Air" + std::to_string(team) + std::to_string(i), team, true);
				CHECK(level.world->spawnSoldier(id));
				game::GameWorld::PlayerInfo info;
				CHECK(level.world->playerInfo(id, info));
				CHECK(level.world->setSoldierPosition(info.soldier,
					level.world->titanToWorld(titan->object, {static_cast<float>(i * 2) - 7.5f, 11.5f, -40})));
				t.crew.push_back(id);
			}
		}
		for (const auto& silo : level.world->missileSilos()) {
			for (int team : {1, 2}) {
				const int id = level.world->addPlayer("Target" + std::to_string(team) + std::to_string(silo.controlPoint), team, false);
				CHECK(level.world->spawnSoldier(id));
				game::GameWorld::PlayerInfo info;
				CHECK(level.world->playerInfo(id, info));
				CHECK(level.world->setSoldierPosition(info.soldier, silo.position + physics::Vec3 {10.0f * team, 0, 0}));
			}
		}
		bool departed[3] = {};
		level.afterTick = [&] {
			bots.update(1 / 30.0f);
			for (auto& [team, t] : teams) {
				const auto* titan = level.titan(team);
				if (!titan) continue;
				int riders = 0;
				bool gunshipPiloted = false, transportPiloted = false;
				for (int id : t.crew) {
					game::GameWorld::PlayerInfo info;
					if (!level.world->playerInfo(id, info)) continue;
					gunshipPiloted = gunshipPiloted || (info.vehicle == t.gunship && info.seat == 0);
					if (info.vehicle == t.transport && info.seat == 0) {
						transportPiloted = true;
						if (t.pilot < 0) t.pilot = id;
					}
					riders += info.vehicle == t.transport && info.seat > 0;
					if (info.vehicle == t.transport && info.seat > 0) {
						t.gunShots = std::max(t.gunShots, info.gunShots);
					}
					t.rode[id] = t.rode[id] || (info.vehicle == t.transport && info.seat > 0);
					if (t.rode[id] && level.world->ridingPod(id) && departed[team]) {
						++t.drops;
						t.rode[id] = false;
					}
				}
				for (const auto& v : level.world->vehicleList()) {
					const float away = std::sqrt(std::pow(v.position.x - titan->position.x, 2.0f) + std::pow(v.position.z - titan->position.z, 2.0f));
					if (v.object == t.gunship && gunshipPiloted && away > 250) t.gunshipFlown = true;
					if (v.object == t.transport && transportPiloted && away > 250) {
						t.transportFlown = true;
						departed[team] = true;
					}
					if (v.object == t.transport && transportPiloted && departed[team] && t.drops > 0) {
						game::GameWorld::PlayerInfo pilot;
						if (level.world->playerInfo(t.pilot, pilot) && pilot.vehicle == t.transport && pilot.seat == 0) {
							const bool sortie = bots.behaviour(t.pilot) == "TitanSortie";
							t.normalTicks = sortie ? 0 : t.normalTicks + 1;
							if (t.normalTicks >= 30) t.normalFlight = true;
							if (t.normalFlight && sortie) t.restartedSortie = true;
							if (bots.behaviour(t.pilot) == "TitanSupport") {
								t.gunnersRetained = t.gunnersRetained || riders >= 2;
								if (!t.supportStarted) {
									t.supportStarted = true;
								}
								if (!t.supportHeld) {
									bool atStation = false;
									for (const auto& silo : level.world->missileSilos()) {
										for (int phase = 0; phase < 8; ++phase) {
											const float angle = phase * 0.78539816f;
											const physics::Vec3 station = silo.position
												+ physics::Vec3 {150 * std::sin(angle), 0, 150 * std::cos(angle)};
											const physics::Vec3 delta = v.position - station;
											atStation = atStation || delta.x * delta.x + delta.z * delta.z < 40 * 40;
										}
									}
									t.supportHoldTicks = atStation ? t.supportHoldTicks + 1 : 0;
									if (t.supportHoldTicks >= 90) {
										t.supportHeld = true;
										t.supportAt = v.position;
									}
								} else {
									const physics::Vec3 delta = v.position - t.supportAt;
									t.repositioned = t.repositioned || delta.x * delta.x + delta.z * delta.z > 150 * 150;
								}
							}
							if (t.normalFlight && !t.spawnChecked) {
								t.spawnChecked = true;
								const auto available = level.world->spawnVehicles(team);
								if (std::find(available.begin(), available.end(), t.transport) != available.end()) {
									const int reinforcement = level.world->addPlayer("Reinforcement" + std::to_string(team), team, false);
									level.world->setSpawnVehicle(reinforcement, t.transport);
									game::GameWorld::PlayerInfo spawned;
									t.spawnedInFlight = level.world->spawnSoldier(reinforcement)
										&& level.world->playerInfo(reinforcement, spawned) && spawned.vehicle == t.transport;
								}
							}
						}
					}
				}
				t.maxRiders = std::max(t.maxRiders, departed[team] ? t.maxRiders : riders);
			}
		};
		level.run(240);
		for (const auto& [team, t] : teams) {
			std::fprintf(stderr, "  %s team %d: gunship flown %d, transport flown %d with up to %d riders, %d pod drops, support %d, held %d (%d ticks), repositioned %d, retained gunners %d, %u gun rounds, restarted %d, mobile spawn %d\n",
				mod, team, t.gunshipFlown, t.transportFlown, t.maxRiders, t.drops, t.normalFlight, t.supportHeld, t.supportHoldTicks, t.repositioned, t.gunnersRetained, t.gunShots, t.restartedSortie, t.spawnedInFlight);
			CHECK(t.gunshipFlown);
			CHECK(t.transportFlown);
			CHECK(t.maxRiders >= 1);
			CHECK(t.drops >= 1);
			CHECK(t.normalFlight);
			CHECK(t.supportStarted && t.supportHeld && t.repositioned);
			CHECK(t.gunnersRetained);
			CHECK(t.gunShots > 0);
			CHECK(!t.restartedSortie);
			CHECK(t.spawnedInFlight);
		}
	}
}

// Air transports away from the deck, even near a Titan, are not deck sorties.
TEST(Titan_BotsLeaveOtherTransportsToOrdinaryAi)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) continue;
		TitanLevel level(gameDir, "Bridge_at_Remagen", 48, true, mod);
		CHECK(level.loaded);
		if (!level.loaded) continue;
		game::BotSettings settings;
		loadAircraftAi(level, settings);
		const bool enhanced = true;
		settings.enhanced = &enhanced;
		settings.botTitan = &enhanced;
		game::BotManager bots(*level.world, settings);
		struct Crew { int team = 0, pilot = -1, rider = -1; bool nearTitan = false, pilotSortie = false, riderSortie = false; };
		std::vector<Crew> crews;
		for (int team : {1, 2}) {
			const auto* titan = level.titan(team);
			CHECK(titan != nullptr);
			if (!titan) continue;
			const float distance = std::sqrt(titan->position.x * titan->position.x + titan->position.z * titan->position.z);
			const float x = titan->position.x - titan->position.x / distance * 500;
			const float z = titan->position.z - titan->position.z / distance * 500;
			world::ObjectTemplate* transport = level.templates.find(team == 1 ? "as_at_spawn" : "eu_at_spawn");
			CHECK(transport != nullptr);
			if (!transport) continue;
			for (bool nearTitan : {false, true}) {
				const physics::Vec3 at = nearTitan ? level.world->titanToWorld(titan->object, {130, 25, 0})
					: physics::Vec3 {x, level.world->terrainHeight(x, z) + 3, z};
				if (nearTitan) {
					float fraction = 1;
					CHECK(!level.world->titanLocalRay(titan->object, team, {130, 26, 0}, {130, 13, 0}, fraction));
				}
				world::Object* vehicle = level.objects.create(transport, {at.x, at.y, at.z});
				CHECK(vehicle != nullptr);
				if (!vehicle) continue;
				vehicle->setTeam(team);
				level.run(1 / 30.0f);
				Crew& crew = crews.emplace_back();
				crew.team = team;
				crew.nearTitan = nearTitan;
				for (int seat = 0; seat < 2; ++seat) {
					const int id = level.world->addPlayer("Ground" + std::to_string(team) + std::to_string(nearTitan) + std::to_string(seat), team, true);
					level.world->setSpawnVehicle(id, vehicle->id());
					CHECK(level.world->spawnSoldier(id));
					game::GameWorld::PlayerInfo info;
					CHECK(level.world->playerInfo(id, info) && info.vehicle == vehicle->id() && info.seat == seat);
					(seat == 0 ? crew.pilot : crew.rider) = id;
				}
			}
		}
		level.afterTick = [&] {
			bots.update(1 / 30.0f);
			for (auto& crew : crews) {
				crew.pilotSortie = crew.pilotSortie || bots.behaviour(crew.pilot) == "TitanSortie";
				crew.riderSortie = crew.riderSortie || bots.behaviour(crew.rider) == "TitanSortie";
			}
		};
		level.run(3);
		for (const auto& crew : crews) {
			std::fprintf(stderr, "  %s team %d %s: pilot sortie %d, rider sortie %d\n",
				mod, crew.team, crew.nearTitan ? "near Titan" : "ground", crew.pilotSortie, crew.riderSortie);
			CHECK(!crew.pilotSortie);
			CHECK(!crew.riderSortie);
		}
	}
}

// Even without riders, a deck transport can deploy as a mobile spawn point.
TEST(Titan_BotsDeployEmptyDeckTransport)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) continue;
		TitanLevel level(gameDir, "Bridge_at_Remagen", 48, true, mod);
		CHECK(level.loaded);
		if (!level.loaded) continue;
		game::BotSettings settings;
		loadAircraftAi(level, settings);
		const bool enhanced = true;
		settings.enhanced = &enhanced;
		settings.botTitan = &enhanced;
		game::BotManager bots(*level.world, settings);
		struct Flight {
			uint32_t transport = 0;
			int pilot = -1, normalAfterRestoreTicks = 0;
			bool departed = false, shieldLowered = false, boardingFlight = false, shieldRestored = false;
			bool shieldRaised = false, resumedSortie = false;
		};
		std::map<int, Flight> flights;
		for (int team : {1, 2}) {
			const auto* titan = level.titan(team);
			CHECK(titan != nullptr);
			if (!titan) continue;
			Flight& flight = flights[team];
			for (const auto& v : level.world->vehicleList()) {
				world::Object* object = level.objects.find(v.object);
				if (!object || !object->objectTemplate()) continue;
				const std::string& name = object->objectTemplate()->name();
				const physics::Vec3 local = level.world->titanToLocal(titan->object, v.position);
				if ((name == "as_at_spawn" || name == "eu_at_spawn") && std::abs(local.x) < 40
					&& std::abs(local.z) < 100 && std::abs(local.y) < 30) {
					flight.transport = v.object;
					object->setTeam(team);
					break;
				}
			}
			CHECK(flight.transport != 0);
			if (!flight.transport) continue;
			flight.pilot = level.world->addPlayer("EmptyPilot" + std::to_string(team), team, true);
			level.world->setSpawnVehicle(flight.pilot, flight.transport);
			CHECK(level.world->spawnSoldier(flight.pilot));
			game::GameWorld::PlayerInfo info;
			CHECK(level.world->playerInfo(flight.pilot, info) && info.vehicle == flight.transport && info.seat == 0);
		}
		level.afterTick = [&] {
			bots.update(1 / 30.0f);
			for (auto& [team, flight] : flights) {
				game::GameWorld::PlayerInfo pilot;
				const auto* titan = level.titan(team);
				if (!titan || !level.world->playerInfo(flight.pilot, pilot) || pilot.vehicle != flight.transport || pilot.seat != 0) continue;
				const physics::Vec3 delta = pilot.position - titan->position;
				const float distance2 = delta.x * delta.x + delta.z * delta.z;
				flight.departed = flight.departed || distance2 > 250 * 250;
				if (distance2 > 150 * 150 && bots.behaviour(flight.pilot) == "TitanSortie" && !flight.shieldLowered) {
					const auto* enemy = level.titan(3 - team);
					if (enemy) {
						level.world->setHitPoints(enemy->object, enemy->maxHitPoints * 0.59f);
						flight.shieldLowered = true;
					}
				}
				flight.boardingFlight = flight.boardingFlight
					|| (flight.shieldLowered && bots.behaviour(flight.pilot) == "TitanFly");
				if (flight.boardingFlight && !flight.shieldRestored) {
					const auto* enemy = level.titan(3 - team);
					if (enemy) {
						level.world->setHitPoints(enemy->object, enemy->maxHitPoints);
						flight.shieldRestored = true;
					}
				}
				const auto* enemy = level.titan(3 - team);
				if (flight.shieldRestored && enemy && enemy->shield) {
					flight.shieldRaised = true;
					const bool sortie = bots.behaviour(flight.pilot) == "TitanSortie";
					flight.resumedSortie = flight.resumedSortie || sortie;
					flight.normalAfterRestoreTicks = sortie ? 0 : flight.normalAfterRestoreTicks + 1;
				}
			}
		};
		level.run(120);
		for (const auto& [team, flight] : flights) {
			std::fprintf(stderr, "  %s team %d: empty transport departed %d, shield-down boarding %d, resumed old sortie %d\n",
				mod, team, flight.departed, flight.boardingFlight, flight.resumedSortie);
			CHECK(flight.departed);
			CHECK(flight.boardingFlight);
			CHECK(flight.shieldRaised && flight.normalAfterRestoreTicks >= 30 && !flight.resumedSortie);
		}
	}
}

// A boarder on a ledge of the enemy's hull with no way to a console (the
// side skirt below the EU Titan's deck edge) leaves the Titan instead of
// standing there under the ordinary AI.
TEST(Titan_BotsLeaveStrandedHullLedge)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) continue;
		TitanLevel level(gameDir, "Bridge_at_Remagen", 48, true, mod);
		CHECK(level.loaded);
		if (!level.loaded) continue;
		game::BotSettings settings;
		const bool enhanced = true;
		settings.enhanced = &enhanced;
		settings.botTitan = &enhanced;
		game::BotManager bots(*level.world, settings);
		const auto* titan = level.titan(2);
		CHECK(titan != nullptr);
		if (!titan) continue;
		const uint32_t object = titan->object;
		const int id = level.world->addPlayer("Stranded", 1, true);
		CHECK(level.world->spawnSoldier(id));
		game::GameWorld::PlayerInfo info;
		CHECK(level.world->playerInfo(id, info));
		CHECK(level.world->setSoldierPosition(info.soldier, level.world->titanToWorld(object, {-30, 3.5f, -19.5f})));
		bool left = false;
		level.afterTick = [&] {
			bots.update(1 / 30.0f);
			left = left || bots.behaviour(id) == "TitanLeave";
		};
		level.run(30);
		CHECK(level.world->playerInfo(id, info));
		const physics::Vec3 local = level.world->titanToLocal(object, info.position);
		std::fprintf(stderr, "  %s: TitanLeave %d, now local %.1f %.1f %.1f alive %d\n", mod, left, local.x, local.y, local.z, info.alive);
		CHECK(left);
		CHECK(!info.alive || local.y < -5 || std::abs(local.x) > 40);
	}
}

// enhanced.botTitan: with the consoles down (the core's doors open) the
// walking grids, of which only the changed box is cast again, come out as
// grids made whole.
TEST(Titan_BotGridRebuiltAroundOpenedCoreDoors)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	TitanLevel level(gameDir, "Bridge_at_Remagen", 48, true, "mods/bf2142");
	CHECK(level.loaded);
	if (!level.loaded) return;
	game::BotSettings settings;
	const bool enhanced = true;
	settings.enhanced = &enhanced;
	settings.botTitan = &enhanced;
	game::BotManager kept(*level.world, settings);
	kept.prepareTitanGrids();
	const std::string before = kept.titanGridStats();
	CHECK(before.find("titan grid") != std::string::npos);
	const int attacker = level.world->addPlayer("Eu", 2, true);
	for (const auto& part : level.world->titanParts(1)) {
		if (part.kind == game::GameWorld::TitanPartInfo::Console) {
			level.world->applyDamage(part.id, 1e5f, attacker);
		}
	}
	level.run(1);
	kept.prepareTitanGrids();
	game::BotManager whole(*level.world, settings);
	whole.prepareTitanGrids();
	std::fprintf(stderr, "  before:\n%s  after:\n%s", before.c_str(), kept.titanGridStats().c_str());
	CHECK(kept.titanGridStats() != before);
	CHECK(kept.titanGridStats() == whole.titanGridStats());
}

// enhanced.botTitan: bots aboard their own Titan drop by pod; they aim the
// array and steer the pod at dry, walkable ground towards their objective.
// Each landing, with whether the infantry mesh is underfoot there (within
// 5 m, at its feet's height). A bot that lands is put back aboard to drop
// again, for `seconds`.
struct PodDrop {
	int team = 0;
	bool wet = false, walksOut = false;
	physics::Vec3 from, at;
	float nearestPoint = 1e9f;
};

static std::vector<PodDrop> runPodDrops(const char* gameDir, const std::string& map, int size, const char* mode, float seconds)
{
	std::vector<PodDrop> drops;
	// With the static objects: a pod comes down on a roof (Molokai's command centre, 36 m up, off the mesh).
	TitanLevel level(gameDir, map, size, true, "mods/Project_Remaster_v17_Final", true, nullptr, mode);
	CHECK(level.loaded);
	if (!level.loaded) return drops;
	const bool sea = level.heightmaps.hasSea();
	if (sea) level.world->setWaterLevel(level.heightmaps.seaWaterLevel());
	game::BotSettings settings;
	loadAircraftAi(level, settings);
	std::string out;
	level.console.run("AI/AIDefaultStrategies.ai", {}, out);
	level.console.run("AI/AIDefaultStrategiesAdd.ai", {}, out);
	const std::string base = "Levels/" + map + "/";
	const std::string areas = base + "GameModes/" + mode + "/" + std::to_string(size) + "/ai/StrategicAreas.ai";
	if (level.files.exists(areas)) CHECK(level.console.run(areas, {}, out)); // Titan mode ships none
	settings.viewDistance = 300;
	const bool enhanced = true;
	settings.enhanced = &enhanced;
	settings.botTitan = &enhanced;
	game::BotManager bots(*level.world, settings);
	// The infantry navigation mesh, as main loads it: landings must be walkable.
	std::optional<ai::NavMesh> mesh;
	{
		std::vector<uint8_t> qtr, clb;
		CHECK(level.files.readFile(base + "AIPathFinding/Infantry.qtr", qtr) && level.files.readFile(base + "AIPathFinding/Infantry.clb", clb));
		mesh = ai::parseNavMesh(qtr, clb);
		CHECK(mesh.has_value());
		bots.setNavMesh(mesh ? &*mesh : nullptr);
	}
	struct Crew { int team = 0, slot = 0; bool riding = false; physics::Vec3 from; };
	std::map<int, Crew> crew;
	const auto aboard = [&](int id) {
		const Crew& c = crew[id];
		const auto* titan = level.titan(c.team);
		game::GameWorld::PlayerInfo info;
		return titan && level.world->playerInfo(id, info) && !info.vehicle
			&& level.world->setSoldierPosition(info.soldier, level.world->titanToWorld(titan->object, {static_cast<float>(c.slot * 2) - 7.5f, 11.5f, -40}));
	};
	for (int team : {1, 2}) {
		CHECK(level.titan(team) != nullptr);
		// Spawn points that put a soldier straight into a vehicle are not this
		// test's: only those on foot board.
		for (int i = 0, slot = 0; slot < 10 && i < 40; ++i) {
			const int id = level.world->addPlayer("Pod" + std::to_string(team) + std::to_string(i), team, true);
			CHECK(level.world->spawnSoldier(id));
			game::GameWorld::PlayerInfo info;
			CHECK(level.world->playerInfo(id, info));
			if (info.vehicle) continue;
			crew[id] = {team, slot};
			CHECK(aboard(id));
			++slot;
		}
	}
	level.afterTick = [&] {
		bots.update(1 / 30.0f);
		for (auto& [id, c] : crew) {
			const bool riding = level.world->ridingPod(id);
			game::GameWorld::PlayerInfo info;
			if (!level.world->playerInfo(id, info)) continue;
			if (riding && !c.riding) c.from = info.position;
			if (!riding && c.riding && info.alive) {
				PodDrop d;
				d.team = c.team;
				d.from = c.from;
				d.at = info.position;
				drops.push_back(d);
				aboard(id);
			}
			c.riding = riding;
		}
	};
	level.run(seconds);
	const float water = sea ? level.heightmaps.seaWaterLevel() : -1e9f;
	// Walkable from where it stands: the mesh within 5 m, at its feet's height (not a roof or ledge over
	// it, nor a ledge of it overhead). The soldier's position is a metre over its feet.
	const auto underfoot = [&](const physics::Vec3& p) {
		for (float r = 0; mesh && r <= 5; r += 2.5f) {
			for (int k = 0; k < (r > 0 ? 8 : 1); ++k) {
				if (mesh->findPolygon({p.x + r * std::sin(k * 0.785f), p.y - 1.0f, p.z + r * std::cos(k * 0.785f)}, 1.5f) >= 0) return true;
			}
		}
		return false;
	};
	for (PodDrop& d : drops) {
		d.wet = level.world->terrainHeight(d.at.x, d.at.z) < water;
		for (const auto& cp : level.world->controlPointList()) {
			d.nearestPoint = std::min(d.nearestPoint, std::hypot(cp.position.x - d.at.x, cp.position.z - d.at.z));
		}
		d.walksOut = underfoot(d.at);
		if (!d.walksOut || d.wet) {
			std::fprintf(stderr, "  %s team %d: pod from %.0f %.0f to %.0f %.0f %.0f, %.0f m from a point,%s%s\n", map.c_str(), d.team,
				d.from.x, d.from.z, d.at.x, d.at.y, d.at.z, d.nearestPoint, d.wet ? " SEA" : "", d.walksOut ? "" : " NO WAY OUT");
		}
	}
	return drops;
}

// Wake Island 2142: both Titans park off the coast; drops land ashore, on the mesh, beside a control point.
TEST(Titan_BotsPodToLandOnWakeCoop)
{
	REMASTER_ONLY();
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	int landed = 0, wet = 0;
	for (const PodDrop& d : runPodDrops(gameDir, "Wake_Island_2142", 64, "gpm_coop", 90)) {
		++landed;
		wet += d.wet;
		CHECK(d.nearestPoint < 60); // Wake's Titans park 100..250 m off the nearest control point
		CHECK(d.walksOut);
	}
	std::fprintf(stderr, "  wake coop: %d pod landings, %d in the sea\n", landed, wet);
	CHECK(landed >= 40);
	CHECK(wet == 0);
}

// Molokai, Titan mode: the Titans fly over the island's buildings (the command
// centre's roof, 36 m up, has no infantry mesh); a drop from its own Titan
// comes down where the mesh is underfoot.
TEST(Titan_BotsPodToWalkableGroundOnMolokaiTitan)
{
	REMASTER_ONLY();
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	int landed = 0, stuck = 0;
	for (const PodDrop& d : runPodDrops(gameDir, "Molokai", 48, "gpm_ti", 240)) {
		++landed;
		stuck += !d.walksOut;
		CHECK(!d.wet);
	}
	std::fprintf(stderr, "  molokai titan: %d pod landings, %d off the mesh\n", landed, stuck);
	CHECK(landed >= 4);
	CHECK(stuck == 0);
}
