#include "Game/Bots.h"
#include "TitanLevel.h"
#include "World/TemplateFiles.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
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
