#include "AI/NavMesh.h"
#include "Game/Bots.h"
#include "TitanLevel.h"
#include "World/TemplateFiles.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <set>
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

// enhanced.botTitan: bots aboard walk from the spawn room to half their
// Titan's guns, but never to a hub that names an auto controller (all of
// Project Remaster's), which the controller keeps.
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
			// Remaster's hubs all name an auto controller, which keeps them: no bot walks to one.
			CHECK(gunners == (std::strstr(mod, "Remaster") ? 0 : 3));
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

// The same ledge under its own Titan's deck edge (a defender drops onto it
// hunting a boarder; a drop is one way): it leaves too, where the gun, crew
// and pod walks would hold it there. A bot on the deck does not.
TEST(Titan_BotsLeaveOwnTitanHullLedge)
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
		int ids[2] = {};
		const physics::Vec3 at[2] = {{-30, 3.5f, -19.5f}, {0, 11.5f, -40}};
		for (int i = 0; i < 2; ++i) {
			ids[i] = level.world->addPlayer(i ? "OnDeck" : "OnLedge", 2, true);
			CHECK(level.world->spawnSoldier(ids[i]));
			game::GameWorld::PlayerInfo info;
			CHECK(level.world->playerInfo(ids[i], info));
			CHECK(level.world->setSoldierPosition(info.soldier, level.world->titanToWorld(object, at[i])));
		}
		bool left[2] = {};
		level.afterTick = [&] {
			bots.update(1 / 30.0f);
			for (int i = 0; i < 2; ++i) left[i] = left[i] || bots.behaviour(ids[i]) == "TitanLeave";
		};
		level.run(30);
		game::GameWorld::PlayerInfo info;
		CHECK(level.world->playerInfo(ids[0], info));
		const physics::Vec3 local = level.world->titanToLocal(object, info.position);
		std::fprintf(stderr, "  %s: ledge TitanLeave %d, now local %.1f %.1f %.1f alive %d; deck TitanLeave %d\n", mod, left[0], local.x,
			local.y, local.z, info.alive, left[1]);
		CHECK(left[0]);
		CHECK(!info.alive || local.y < -5 || std::abs(local.x) > 40);
		CHECK(!left[1]);
	}
}

// A bot on foot on the ground in gpm_ti, below the enemy Titan's control
// point. Retail (enhanced.botTitan off): with no strategy it goes to the
// nearest point not its team's (updateGoal's fallback), here the Titan's,
// which can't change hands. With enhanced.botTitan it heads for a silo it
// can capture instead.
TEST(Titan_BotSiloGoalOnlyWithEnhancedTitanPlay)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const bool titanPlay : {false, true}) {
		TitanLevel level(gameDir, "Bridge_at_Remagen", 48, true, "mods/bf2142");
		CHECK(level.loaded);
		if (!level.loaded) continue;
		game::BotSettings settings;
		const bool enhanced = true;
		settings.enhanced = &enhanced;
		settings.botTitan = &titanPlay;
		const auto* enemy = level.titan(2);
		CHECK(enemy != nullptr);
		if (!enemy) continue;
		const int enemyPoint = enemy->controlPoint;
		const auto points = level.world->controlPointList();
		const game::GameWorld::ControlPointInfo* titanPoint = nullptr;
		for (const auto& p : points) {
			titanPoint = p.id == enemyPoint ? &p : titanPoint;
		}
		CHECK(titanPoint != nullptr);
		if (!titanPoint) continue;
		const physics::Vec3 at {titanPoint->position.x,
			level.heightmaps.primary()->heightAt(titanPoint->position.x, titanPoint->position.z) + 1, titanPoint->position.z};
		// The nearest point not the bot's team's, as the fallback sees them.
		const game::GameWorld::ControlPointInfo* nearest = nullptr;
		for (const auto& p : points) {
			if (p.team != 1 && (!nearest || physics::lengthSquared(p.position - at) < physics::lengthSquared(nearest->position - at))) {
				nearest = &p;
			}
		}
		CHECK(nearest == titanPoint && !titanPoint->capturable);
		game::BotManager bots(*level.world, settings);
		const int id = level.world->addPlayer("Bot", 1, true);
		CHECK(level.world->spawnSoldier(id));
		game::GameWorld::PlayerInfo info;
		CHECK(level.world->playerInfo(id, info));
		CHECK(level.world->setSoldierPosition(info.soldier, at));
		bots.update(1 / 30.0f);
		const std::vector<physics::Vec3> route = bots.route(id);
		CHECK(!route.empty());
		if (route.empty()) continue;
		const physics::Vec3 goal = route.back();
		const auto within = [&](const game::GameWorld::ControlPointInfo& p) {
			const float dx = goal.x - p.position.x, dz = goal.z - p.position.z;
			return dx * dx + dz * dz <= std::max(p.radius, 10.0f) * std::max(p.radius, 10.0f);
		};
		bool silo = false;
		for (const auto& s : level.world->missileSilos()) {
			for (const auto& p : points) {
				silo = silo || (p.id == s.controlPoint && s.team != 1 && p.capturable && within(p));
			}
		}
		std::fprintf(stderr, "  botTitan %d: goal (%.0f, %.0f, %.0f), the enemy Titan's point (%.0f, %.0f, %.0f)\n", titanPlay,
			goal.x, goal.y, goal.z, titanPoint->position.x, titanPoint->position.y, titanPoint->position.z);
		CHECK(within(*titanPoint) == !titanPlay);
		CHECK(silo == titanPlay);
	}
}

// enhanced.botTitan: engineers aboard their own Titan with no enemy aboard
// walk to its wrecked guns' control stations inside the hull and mend the
// guns with the repair tool (a gun's wreck back in service above 0 hit points). Consoles are left: the tool's
// material does nothing on theirs, whole or wrecked (BotManager::titanRepair).
TEST(Titan_BotEngineersRepairTitanGunsAndConsoles)
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
		level.afterTick = [&] { bots.update(1 / 30.0f); };
		std::map<int, std::vector<uint32_t>> guns;
		std::map<int, std::pair<uint32_t, uint32_t>> consoles; // damaged, wrecked
		std::map<int, int> repairers;
		for (int team : {1, 2}) {
			const auto* titan = level.titan(team);
			CHECK(titan != nullptr);
			if (!titan) continue;
			for (const auto& part : level.world->titanParts(team)) {
				if (part.kind == game::GameWorld::TitanPartInfo::Gun) {
					level.world->applyDamage(part.id, 1e6f);
					CHECK(level.world->hitPoints(part.id) <= 0);
					guns[team].push_back(part.id);
				} else if (part.kind == game::GameWorld::TitanPartInfo::Console) {
					auto& [damaged, wrecked] = consoles[team];
					if (!damaged) {
						damaged = part.id;
						level.world->applyDamage(part.id, part.maxHitPoints * 0.5f);
					} else if (!wrecked) {
						wrecked = part.id;
						level.world->applyDamage(part.id, 1e6f);
					}
				}
			}
			for (int i = 0; i < 2; ++i) {
				const int id = level.world->addPlayer("Engineer" + std::to_string(team) + std::to_string(i), team, true);
				level.world->setKit(id, team == 1 ? "Pac_Anti-Vehicle" : "EU_Anti-Vehicle", team == 1 ? "pac_light_soldier" : "us_light_soldier");
				CHECK(level.world->spawnSoldier(id));
				game::GameWorld::PlayerInfo info;
				CHECK(level.world->playerInfo(id, info));
				CHECK(level.world->setSoldierPosition(info.soldier,
					level.world->titanToWorld(titan->object, {static_cast<float>(i * 3) - 1.5f, 11.5f, -40})));
				repairers[id] = team;
			}
		}
		const auto damagedConsoleBefore = [&](int team) { return level.world->hitPoints(consoles[team].first); };
		std::map<int, float> consoleBefore {{1, damagedConsoleBefore(1)}, {2, damagedConsoleBefore(2)}};
		std::map<int, int> repairing;
		level.afterTick = [&] {
			bots.update(1 / 30.0f);
			for (const auto& [id, team] : repairers) {
				repairing[id] += bots.behaviour(id) == "TitanRepair";
			}
		};
		level.run(240);
		std::map<int, int> back;
		for (int team : {1, 2}) {
			for (uint32_t gun : guns[team]) {
				back[team] += level.world->hitPoints(gun) > 0;
			}
			const auto& [damaged, wrecked] = consoles[team];
			std::fprintf(stderr, "  %s team %d: %d of %zu wrecked guns back, console %.0f -> %.0f, wrecked console %.0f\n", mod, team,
				back[team], guns[team].size(), consoleBefore[team], level.world->hitPoints(damaged), level.world->hitPoints(wrecked));
			// The repair tool's material does nothing on the consoles' (84 against 4110: damageMod 0).
			CHECK(level.world->hitPoints(damaged) == consoleBefore[team]);
			CHECK(level.world->hitPoints(wrecked) <= 0);
		}
		// Each gun through its control station inside the hull (BotManager::titanRepair); four minutes for two engineers.
		CHECK(back[1] >= 4 && back[2] >= 4);
		for (const auto& [id, team] : repairers) {
			CHECK(repairing[id] > 0);
		}
	}
}

// enhanced.botTitan: a bot's repair tool takes its AI's target (the gun it
// mends, BotManager::titanRepair) only within the tool's reach (+3 m) of one
// of the part's meshes, and the target goes with the bot's life. An engineer
// that was mending a gun, its AI then stopped and the tool held up at the sky
// (the ray meeting nothing to mend), mends it on the spot; carried off to the
// ground far away, not; back on the spot, again; after its AI thought on, off
// the Titan with enhanced.botTitan off, not (the claim lasts a think,
// BotManager::update);
// killed, and back on the spot in its next life, not (GameWorld::applyAbility;
// spawnSoldier's fresh Player).
TEST(Titan_StaleRepairTargetMendsNothingFromAfar)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	const char* mod = "mods/bf2142";
	if (!test::runs(mod)) return;
	TitanLevel level(gameDir, "Bridge_at_Remagen", 48, true, mod);
	CHECK(level.loaded);
	if (!level.loaded) return;
	game::BotSettings settings;
	loadAircraftAi(level, settings);
	const bool enhanced = true;
	bool titanPlay = true;
	settings.enhanced = &enhanced;
	settings.botTitan = &titanPlay;
	game::BotManager bots(*level.world, settings);
	const int team = 1;
	const auto* found = level.titan(team);
	CHECK(found != nullptr);
	if (!found) return;
	const uint32_t titan = found->object;
	std::vector<uint32_t> guns;
	for (const auto& part : level.world->titanParts(team)) {
		if (part.kind == game::GameWorld::TitanPartInfo::Gun) {
			level.world->applyDamage(part.id, part.maxHitPoints * 0.5f);
			guns.push_back(part.id);
		}
	}
	CHECK(!guns.empty());
	const auto gunHitPoints = [&] {
		float sum = 0;
		for (uint32_t gun : guns) {
			sum += level.world->hitPoints(gun);
		}
		return sum;
	};
	const int id = level.world->addPlayer("Engineer", team, true);
	level.world->setKit(id, "Pac_Anti-Vehicle", "pac_light_soldier");
	CHECK(level.world->spawnSoldier(id));
	const auto info = [&] {
		game::GameWorld::PlayerInfo p;
		CHECK(level.world->playerInfo(id, p));
		return p;
	};
	CHECK(level.world->setSoldierPosition(info().soldier, level.world->titanToWorld(titan, {-1.5f, 11.5f, -40})));
	bool thinking = true;
	game::SoldierInput hold;
	level.afterTick = [&] {
		if (thinking) {
			bots.update(1 / 30.0f);
		} else {
			level.world->setInput(id, hold);
		}
	};
	// Mending: at a gun's station, the gun's hit points rising.
	uint32_t mended = 0;
	for (int s = 0; s < 150 && !mended; ++s) {
		std::vector<float> was;
		for (uint32_t gun : guns) {
			was.push_back(level.world->hitPoints(gun));
		}
		level.run(1);
		for (size_t i = 0; i < guns.size(); ++i) {
			mended = bots.behaviour(id) == "TitanRepair" && level.world->hitPoints(guns[i]) > was[i] ? guns[i] : mended;
		}
	}
	CHECK(mended != 0);
	if (!mended) return;
	const int toolSlot = info().inHandSlot;
	const physics::Vec3 spot = level.world->titanToLocal(titan, info().position);
	thinking = false;
	hold.fire = true;
	hold.aimPitch = 89;
	// Each 4 s after a 3 s settle (the tool's scan holds its targets up to 2 s).
	const auto mends = [&] {
		level.run(3);
		const float was = gunHitPoints();
		level.run(4);
		return gunHitPoints() - was;
	};
	const auto moveTo = [&](const physics::Vec3& at) { CHECK(level.world->setSoldierPosition(info().soldier, at)); };
	const float onSpot = mends();
	physics::Vec3 far = level.world->titanToWorld(titan, spot);
	far.x += 300;
	far.y = level.world->terrainHeight(far.x, far.z);
	moveTo(far);
	const float fromAfar = mends();
	moveTo(level.world->titanToWorld(titan, spot));
	const float back = mends();
	// (Hurt again: the gun mended may be whole by now.)
	for (const auto& part : level.world->titanParts(team, titan)) {
		if (part.kind == game::GameWorld::TitanPartInfo::Gun) {
			level.world->applyDamage(part.id, part.maxHitPoints * 0.3f);
		}
	}
	// On the spot with the tool drawn and held up at the sky.
	const auto ready = [&] {
		moveTo(level.world->titanToWorld(titan, spot));
		hold = {};
		hold.selectItem = toolSlot;
		level.run(0.1f);
		hold = {};
		level.run(2);
		CHECK(info().inHandSlot == toolSlot);
		hold.fire = true;
		hold.aimPitch = 89;
	};
	// The AI thinking a moment (a lone bot thinks every other frame) with enhanced.botTitan off, off the Titan
	// (on the spot, the ordinary AI takes the gun's seat at once): the claim and the target let go.
	moveTo(far);
	titanPlay = false;
	thinking = true;
	level.run(0.1f);
	thinking = false;
	ready();
	const float settingOff = mends();
	level.world->applyDamage(info().soldier, 1e6f);
	level.run(1);
	CHECK(!info().alive);
	CHECK(level.world->spawnSoldier(id) || info().alive);
	ready();
	const float nextLife = mends();
	// The next life's AI naming the gun again: mended.
	level.world->setAiAbilityTarget(id, mended);
	const float named = mends();
	std::fprintf(stderr,
		"  gun hit points mended in 4 s: on the spot %.1f, 300 m off %.1f, back %.1f, setting off %.1f, next life %.1f, named again %.1f\n",
		onSpot, fromAfar, back, settingOff, nextLife, named);
	CHECK(onSpot > 0 && back > 0 && named > 0);
	CHECK(fromAfar == 0 && settingOff == 0 && nextLife == 0);
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

// enhanced.botCommander: with no commander a bot team's Titan is moved
// towards a silo, to a spot that leaves every friendly silo's missile path
// (the climb, then straight at the enemy's middle) clear of its box, and
// beside the enemy's Titan once that one's shield is down; the UAV goes up
// over the focus. With the switch off nobody orders it (the server's first
// order at its own place stands).
TEST(Titan_BotCommanderMovesTheTitans)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) continue;
		for (const bool commander : {false, true}) {
			TitanLevel level(gameDir, "Suez_Canal", 48, true, mod);
			CHECK(level.loaded);
			if (!level.loaded) continue;
			game::BotSettings settings;
			const bool enhanced = true;
			settings.enhanced = &enhanced;
			settings.botTitan = &enhanced;
			settings.botCommander = &commander;
			game::BotManager bots(*level.world, settings);
			bool uav[3] = {}, strike[3] = {};
			level.afterTick = [&] {
				bots.update(1 / 30.0f);
				for (int team : {1, 2}) {
					// Titan mode's commander screen shows the Titan's move where the artillery strike would be
					// (HudElementsCommanderComm.con, GameMode 1): no strike for the virtual commander.
					const auto artillery = level.world->abilityState(team, game::GameWorld::CommanderAbility::Artillery);
					strike[team] = strike[team] || (artillery.available && !artillery.ready) || artillery.active;
					physics::Vec3 at;
					float radius = 0;
					uav[team] = uav[team] || level.world->uavArea(team, at, radius);
				}
			};
			std::map<int, physics::Vec3> start;
			std::vector<int> crew;
			const auto addBots = [&] {
				for (int team : {1, 2}) {
					for (int i = 0; i < 2; ++i) {
						crew.push_back(level.world->addPlayer("Bot" + std::to_string(team) + std::to_string(i), team, true));
						CHECK(level.world->spawnSoldier(crew.back()));
					}
				}
			};
			for (int team : {1, 2}) {
				start[team] = level.titan(team)->position;
			}
			addBots();
			// The missile paths as the commander reads them (titanBlocksSilos: its climb and box clearance, the
			// team's silos and the focus silo): the Titan at `at` (moved there; it never turns) is in none.
			const auto blocks = [&](int team, const physics::Vec3& at) { return bots.titanOrderBlocksSilos(team, at); };
			level.run(150);
			for (int team : {1, 2}) {
				// (level.titan's answer is one static, overwritten by the next call: copied.)
				const auto* found = level.titan(team);
				CHECK(found && found->hasTarget);
				if (!found || !found->hasTarget) continue;
				const auto info = *found;
				const auto* titan = &info;
				const float ordered = std::hypot(titan->target.x - start[team].x, titan->target.z - start[team].z);
				const float moved = std::hypot(titan->position.x - start[team].x, titan->position.z - start[team].z);
				float silo = 1e9f;
				for (const auto& s : level.world->missileSilos()) {
					silo = std::min(silo, std::hypot(titan->target.x - s.position.x, titan->target.z - s.position.z));
				}
				std::fprintf(stderr, "  %s commander %d team %d: order %.0f m from the start, moved %.0f m, %.0f m from a silo, blocks %d, uav %d\n",
					mod, commander, team, ordered, moved, silo, blocks(team, titan->target), uav[team]);
				if (!commander) {
					CHECK(ordered < 5 && moved < 5 && !uav[team]);
					continue;
				}
				CHECK(ordered > 30 && moved > 100 && silo <= 125);
				CHECK(!blocks(team, titan->target));
				CHECK(uav[team]);
				CHECK(!strike[team]);
			}
			if (!commander) continue;
			// The silo each Titan went to, taken: its missile reaches the enemy, not its own Titan above it. (No bots
			// meanwhile: none takes the silos back, and the Titans hold their orders.)
			for (int id : crew) {
				level.world->removePlayer(id);
			}
			crew.clear();
			std::map<int, float> before;
			for (int team : {1, 2}) {
				const auto info = *level.titan(team);
				const auto* titan = &info;
				const auto silos = level.world->missileSilos();
				const game::GameWorld::SiloInfo* nearest = nullptr;
				for (const auto& s : silos) {
					if (!nearest || std::hypot(titan->target.x - s.position.x, titan->target.z - s.position.z)
							< std::hypot(titan->target.x - nearest->position.x, titan->target.z - nearest->position.z)) {
						nearest = &s;
					}
				}
				CHECK(nearest != nullptr);
				if (nearest) level.world->setControlPointTeam(nearest->controlPoint, team);
			}
			for (int team : {1, 2}) {
				before[team] = level.titan(team)->hitPoints;
			}
			level.run(175);
			for (int team : {1, 2}) {
				const float own = level.titan(team)->hitPoints, enemy = level.titan(3 - team)->hitPoints;
				std::fprintf(stderr, "  %s team %d: own Titan %.0f -> %.0f, enemy's %.0f -> %.0f\n", mod, team, before[team], own, before[3 - team], enemy);
				CHECK(enemy < before[3 - team]);
			}
			// The EU's shield down: the PAC Titan goes beside it.
			addBots();
			level.world->setHitPoints(level.titan(2)->object, level.titan(2)->maxHitPoints * 0.59f);
			level.run(25);
			const auto pacInfo = *level.titan(1);
			const auto euInfo = *level.titan(2);
			const auto* pac = &pacInfo;
			const auto* eu = &euInfo;
			const float apart = pac->radius + eu->radius;
			const float beside = std::hypot(pac->target.x - eu->position.x, pac->target.z - eu->position.z);
			std::fprintf(stderr, "  %s: PAC order %.0f m from the EU Titan (radii %.0f), blocks %d\n", mod, beside, apart, blocks(1, pac->target));
			CHECK(beside >= apart + 20 && beside <= apart + 90); // the ring: radii + 30 m (or 50 m further)
			CHECK(!blocks(1, pac->target));
		}
	}
}

// A gun hangs under the hull; its control station inside (an unarmoured
// child of the gun's bundle, eu_controlstation_v2_g) is the gun's to the
// repair tool: 0x004cc380 walks a hit node up (+0x34) to the nearest armour
// (+0x48). The gun's wreck puts its whole subtree in its wreck state, by
// each node's own mesh and nothing else: the look (0x00651c60: three
// geometries, so the wreck geometry, not hidden) and the collision
// (0x006539f0 -> 0x006e5df0: the station's second sub-geometry, the same
// shape as its first), so a player's tool aimed at the station brings the
// gun back (canBeRepairedWhenWreck 1).
TEST(Titan_RepairToolOnGunStationMendsTheGun)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	TitanLevel level(gameDir, "Bridge_at_Remagen", 48, true, "mods/bf2142");
	CHECK(level.loaded);
	if (!level.loaded) return;
	const int team = 2;
	const int id = level.world->addPlayer("Engineer", team, false);
	level.world->setKit(id, "EU_Anti-Vehicle", "us_light_soldier");
	CHECK(level.world->spawnSoldier(id));
	int mended = 0, tried = 0;
	const uint32_t titan = level.titan(team)->object;
	for (const auto& part : level.world->titanParts(team)) {
		if (part.kind != game::GameWorld::TitanPartInfo::Gun) continue;
		const bool aa = part.name.find("aacannon") != std::string::npos;
		const std::string station = part.name + "_hub/" + part.name + (aa ? "/eu_controlstation_v2_aa" : "/eu_controlstation_v2_g");
		CHECK(level.world->titanPartLook(titan, station) == 0);
		level.world->applyDamage(part.id, 1e6f);
		CHECK(level.world->hitPoints(part.id) <= 0);
		level.run(0.1f);
		CHECK(level.world->titanPartLook(titan, station) == 1); // its wreck geometry
		CHECK(level.world->titanPartLook(titan, part.name + "_hub/" + part.name) == 1);
		const auto meshes = level.world->titanPartMeshes(part.id);
		CHECK(meshes.size() >= 2); // the gun's own and its station's
		bool done = false;
		// Any of its meshes a soldier can stand by (the ground cannons': only the station).
		for (int k = 0; k < 16 * static_cast<int>(meshes.size()) && !done; ++k) {
			const physics::Vec3 station = meshes[static_cast<size_t>(k / 16)];
			const float a = static_cast<float>(k % 8) * 6.2831853f / 8, r = k % 16 < 8 ? 1.5f : 2.2f;
			game::GameWorld::PlayerInfo info;
			CHECK(level.world->playerInfo(id, info));
			if (!level.world->setSoldierPosition(info.soldier, station + physics::Vec3 {std::sin(a) * r, 0.2f, std::cos(a) * r})) continue;
			level.run(0.5f);
			level.world->playerInfo(id, info);
			if (std::fabs(info.position.y - station.y) > 1.5f) continue;
			++tried;
			const float before = level.world->hitPoints(part.id);
			for (int tick = 0; tick < 90; ++tick) {
				level.world->playerInfo(id, info);
				const physics::Vec3 d = station + physics::Vec3 {0, 0.8f, 0} - (info.position + physics::Vec3 {0, 1.6f, 0});
				game::SoldierInput input;
				input.turn = std::remainder(std::atan2(d.x, d.z) * 57.29578f - info.yaw, 360.0f);
				input.aimPitch = std::atan2(d.y, std::hypot(d.x, d.z)) * 57.29578f;
				input.hasPitch = true;
				input.selectItem = tick == 0 ? 4 : 0;
				input.fire = tick > 30;
				level.world->setInput(id, input);
				level.run(1 / 30.0f);
			}
			done = level.world->hitPoints(part.id) > before;
			if (done) {
				// The ability bar reads the gun's own armour (0x007c3af0: hit points
				// over maxHitPoints), below 0 while it is a wreck.
				game::GameWorld::AbilityBarInfo bar;
				const float share = level.world->hitPoints(part.id) / 1000.0f;
				CHECK(level.world->abilityBar(id, bar) && bar.targets.size() == 1);
				CHECK(!bar.targets.empty() && share < 0 && std::fabs(bar.targets[0] - share) < 1e-3f);
				if (mended == 0) {
					// Working again (above 0): the share fills the ring.
					level.world->setHitPoints(part.id, 400);
					for (int tick = 0; tick < 3; ++tick) {
						level.run(1 / 30.0f);
					}
					CHECK(level.world->abilityBar(id, bar) && bar.targets.size() == 1);
					CHECK(!bar.targets.empty() && bar.targets[0] > 0.39f && bar.targets[0] < 0.5f);
				}
			}
		}
		mended += done;
		std::fprintf(stderr, "  %s: %s\n", part.name.c_str(), done ? "mended" : "not mended");
	}
	CHECK(tried > 0);
	CHECK(mended == 6);
}

// Camp Gibraltar's chain-link fence (Chicken_Wire_Thin, a 3 mm pair of
// faces) lets rounds through: the way out (slot 0x7c, 0x005eb094) is the far
// face met from its front, found even when the hit lies on the pair.
TEST(CampGibraltar_BulletsPassTheChainLinkFence)
{
	REMASTER_ONLY();
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	TitanLevel level(gameDir, "Camp_Gibraltar", 32, true, "mods/Project_Remaster_v17_Final", true, nullptr, "gpm_cq");
	if (!level.loaded) return;
	for (const auto& object : level.objects.objects()) {
		if (object->objectTemplate()->name() != "fence_industry_16m") continue;
		const auto p = object->position();
		const float yaw = object->rotation().x * 3.14159265f / 180.0f;
		const physics::Vec3 across {std::cos(yaw), 0, -std::sin(yaw)};
		const physics::Vec3 from = physics::Vec3 {p.x, p.y + 1.0f, p.z} + across * -5.0f + physics::Vec3 {std::sin(yaw), 0, std::cos(yaw)} * 0.3f;
		level.world->launchProjectile("eu_smg_Projectile", from, across * 400.0f);
		for (int i = 0; i < 4; ++i) level.world->tick(1 / 30.0f);
		bool fence = false;
		for (const auto& impact : level.world->impacts()) fence = fence || impact.object == object->id();
		CHECK(fence);
		CHECK(level.world->impacts().size() > 1 || !level.world->projectiles().empty());
		return;
	}
	CHECK(false);
}

// A round leaves the eye 0.5 m along the aim; a soldier beside a thin solid
// (the Titans' door shield, the portable shield's slab, a fence) starts it
// past the surface, which a ray from there never crosses: it starts at the
// eye then.
TEST(CampGibraltar_ShotsAtAnAdjacentFenceHit)
{
	REMASTER_ONLY();
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	TitanLevel level(gameDir, "Camp_Gibraltar", 32, true, "mods/Project_Remaster_v17_Final", true, nullptr, "gpm_cq");
	if (!level.loaded) return;
	world::Object* soldier = level.world->spawnPlayer(2);
	CHECK(soldier != nullptr);
	if (!soldier) return;
	for (int i = 0; i < 60; ++i) level.world->tick(1 / 30.0f); // the weapon is up
	const float yaw = soldier->rotation().x * 3.14159265f / 180.0f;
	const physics::Vec3 f {std::sin(yaw), 0, std::cos(yaw)};
	for (const auto& object : level.objects.objects()) {
		if (object->objectTemplate()->name() != "fence_industry_16m") continue;
		const auto p = object->position();
		const physics::Vec3 centre {p.x, p.y + 1.0f, p.z};
		physics::CollisionWorld::Hit face {};
		if (!level.world->staticCollision().raycast(centre - f * 20.0f, centre, face) || face.owner != object->id()) continue;
		const physics::Vec3 feet = face.point - f * 0.15f - physics::Vec3 {0, 1.08f, 0}; // the crouched eye is 1.08 m up
		game::SoldierInput input;
		input.pose = game::SoldierPose::Crouch;
		input.fire = true;
		const size_t before = level.world->impacts().size();
		for (int i = 0; i < 4; ++i) {
			if (!level.world->setSoldierPosition(soldier->id(), feet)) break; // held at the fence
			level.world->setPlayerInput(input);
			level.world->tick(1 / 30.0f);
		}
		bool hit = false;
		for (size_t i = before; i < level.world->impacts().size(); ++i) hit = hit || level.world->impacts()[i].object == object->id();
		CHECK(hit);
		return;
	}
	CHECK(false);
}

// enhanced.botTitan: boarders set down on the enemy Titan's deck with its
// shield down shoot its consoles from spots their rounds reach (not through
// a window's glass), and with the consoles gone, from inside the core room, the core.
TEST(Titan_BotBoardersDestroyConsolesAndCore)
{
	REMASTER_ONLY();
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	const char* mod = "mods/Project_Remaster_v17_Final";
	TitanLevel level(gameDir, "Suez_Canal", 48, true, mod);
	CHECK(level.loaded);
	if (!level.loaded) return;
	game::BotSettings settings;
	loadAircraftAi(level, settings);
	const bool enhanced = true;
	settings.enhanced = &enhanced;
	settings.botTitan = &enhanced;
	game::BotManager bots(*level.world, settings);
	const auto* found = level.titan(2);
	CHECK(found != nullptr);
	if (!found) return;
	const uint32_t eu = found->object;
	level.world->setHitPoints(eu, found->maxHitPoints * 0.59f);
	level.run(1);
	const std::vector<physics::Vec3> spots = bots.titanLandingSpots(2, 1);
	CHECK(!spots.empty());
	if (spots.empty()) return;
	std::vector<int> boarders;
	for (int i = 0; i < 6; ++i) {
		const int id = level.world->addPlayer("Boarder" + std::to_string(i), 1, true);
		CHECK(level.world->spawnSoldier(id));
		game::GameWorld::PlayerInfo info;
		CHECK(level.world->playerInfo(id, info));
		CHECK(level.world->setSoldierPosition(info.soldier, spots[(i * spots.size()) / 6] + physics::Vec3 {0, 1, 0}));
		boarders.push_back(id);
	}
	float t = 0, consolesAt = -1, coreAt = -1, coreLeft = 1e9f;
	size_t respawns = 0;
	const bool diag = std::getenv("BF2142_TITAN_DIAG") != nullptr;
	if (diag) {
		for (size_t i = 0; i < 6; ++i) {
			const physics::Vec3 l = level.world->titanToLocal(eu, spots[(i * spots.size()) / 6]);
			std::fprintf(stderr, "  spot %.1f %.1f %.1f of %zu\n", l.x, l.y, l.z, spots.size());
		}
		level.sink = [&](const std::string& event, const game::ScriptArgs&) {
			if (event == "PlayerKilled") std::fprintf(stderr, "  %.1f s: %s\n", t, event.c_str());
		};
	}
	level.afterTick = [&] {
		bots.update(1 / 30.0f);
		t += 1 / 30.0f;
		const auto parts = level.world->titanParts(2, eu);
		int consoles = 0;
		bool core = false;
		for (const auto& p : parts) {
			consoles += p.kind == game::GameWorld::TitanPartInfo::Console && !p.wreck;
			core = core || (p.kind == game::GameWorld::TitanPartInfo::Core && p.wreck);
		}
		if (consoles == 0 && consolesAt < 0) consolesAt = t;
		if (core && coreAt < 0) coreAt = t;
		for (const auto& p : parts) {
			if (p.kind == game::GameWorld::TitanPartInfo::Core) coreLeft = std::min(coreLeft, p.hitPoints);
		}
		// Kept alive (the test is the boarders' way and aim, not their fights); one that died or leaves (its
		// rounds spent) comes back with a new kit on the next landing spot, as a respawn on a transport would.
		for (int id : boarders) {
			game::GameWorld::PlayerInfo info;
			if (!level.world->playerInfo(id, info)) continue;
			if (info.alive && bots.behaviour(id) == "TitanLeave") {
				level.world->setHitPoints(info.soldier, 0);
				continue;
			}
			if (info.alive) {
				level.world->setHitPoints(info.soldier, 100);
			} else if (level.world->spawnSoldier(id) && level.world->playerInfo(id, info)) {
				level.world->setSoldierPosition(info.soldier, spots[(++respawns * 7919) % spots.size()] + physics::Vec3 {0, 1, 0});
			}
		}
		const int tick = static_cast<int>(std::lround(t * 30));
		if (diag && tick % (30 * 15) == 0) {
			std::fprintf(stderr, "  %.0f s: consoles", t);
			for (const auto& p : parts) {
				if (p.kind == game::GameWorld::TitanPartInfo::Console || p.kind == game::GameWorld::TitanPartInfo::Core) {
					std::fprintf(stderr, " %s %.0f/%.0f", p.kind == game::GameWorld::TitanPartInfo::Core ? "core" : "c", p.hitPoints, p.maxHitPoints);
				}
			}
			std::fprintf(stderr, "\n");
			for (const auto& p : parts) {
				physics::Vec3 aim;
				if (p.kind == game::GameWorld::TitanPartInfo::Console && level.world->titanPartAimPoint(p.id, aim)) {
					const physics::Vec3 l = level.world->titanToLocal(eu, p.position);
					std::fprintf(stderr, "    console %x %s at %.1f %.1f %.1f aim %.1f %.1f %.1f\n", p.id, p.name.c_str(), l.x, l.y, l.z, aim.x, aim.y, aim.z);
				}
			}
			for (int id : boarders) {
				game::GameWorld::PlayerInfo info;
				level.world->playerInfo(id, info);
				const physics::Vec3 l = level.world->titanToLocal(eu, info.position);
				std::string ammo;
				for (const auto& w : level.world->weaponAmmo(id)) {
					ammo += " " + w.weapon + (w.inHand ? "*" : "") + " " + std::to_string(w.inMag) + "/" + std::to_string(w.rounds);
				}
				std::fprintf(stderr, "    %d %s alive %d local %.1f %.1f %.1f fire %d mag %d:%s\n", id, bots.behaviour(id).c_str(), info.alive, l.x, l.y, l.z,
					bots.lastInputFor(id).fire, info.roundsInMag, ammo.c_str());
			}
		}
	};
	level.run(600);
	std::fprintf(stderr, "  consoles down at %.0f s, core at %.0f s, %zu respawns\n", consolesAt, coreAt, respawns);
	CHECK(consolesAt > 0);
	// The core is 40000 hit points to small arms: the boarders reach it and hurt it (the whole of it takes a
	// long while).
	std::fprintf(stderr, "  core down to %.0f\n", coreLeft);
	CHECK(coreLeft < 38000);
}

// A round that has gone through the fence goes on to the next thin solid: the
// railing behind it is hit as well (each way out costs its own loss and turns
// the heading a little, so the railing's thin bars are swept).
TEST(CampGibraltar_ShotsThroughTheFenceHitARailingBehindIt)
{
	REMASTER_ONLY();
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	TitanLevel level(gameDir, "Camp_Gibraltar", 32, true, "mods/Project_Remaster_v17_Final", true, nullptr, "gpm_cq");
	if (!level.loaded) return;
	world::ObjectTemplate* railing = level.templates.find("pedestrian_railing_8m_c");
	CHECK(railing != nullptr);
	for (const auto& object : level.objects.objects()) {
		if (!railing || object->objectTemplate()->name() != "fence_industry_16m") continue;
		const auto p = object->position();
		const float yaw = object->rotation().x * 3.14159265f / 180.0f;
		const physics::Vec3 across {std::cos(yaw), 0, -std::sin(yaw)};
		const physics::Vec3 forward {std::sin(yaw), 0, std::cos(yaw)};
		const physics::Vec3 behind = physics::Vec3 {p.x, p.y, p.z} + across * 2.0f;
		world::Object* rail = level.objects.create(railing, {behind.x, behind.y, behind.z});
		rail->setRotation({object->rotation().x, 0, 0});
		for (int i = 0; i < 3; ++i) level.world->tick(1 / 30.0f);
		int viaFence = 0, alone = 0;
		bool fence = false;
		// The railing's bars are thin, and each way out turns the heading a little: sweep the heights.
		for (int k = 0; k < 24; ++k) {
			const float h = 0.05f * (k + 1);
			for (const bool through : {true, false}) {
				const size_t before = level.world->impactsTotal();
				const physics::Vec3 from = physics::Vec3 {p.x, p.y + h, p.z} + across * (through ? -5.0f : 0.8f);
				level.world->launchProjectile("eu_smg_Projectile", from, across * 400.0f);
				for (int i = 0; i < 6; ++i) level.world->tick(1 / 30.0f);
				const size_t fresh = std::min<size_t>(level.world->impactsTotal() - before, level.world->impacts().size());
				for (size_t n = level.world->impacts().size() - fresh; n < level.world->impacts().size(); ++n) {
					const auto& impact = level.world->impacts()[n];
					fence = fence || impact.object == object->id();
					if (impact.object == rail->id()) ++(through ? viaFence : alone);
				}
			}
		}
		CHECK(alone > 0);
		const bool back = viaFence > 0;
		CHECK(fence);
		CHECK(back);
		return;
	}
	CHECK(false);
}

// Project Remaster's APC pod seats (equipment Passenger, their Armament
// plug-in commented out) count none of their machine gun (AIDLL 0x100d3b80
// reads the AI object's Armament only; 0x100d3e70's x2 needs it too): the
// seat is worth its move only, 20 x 0.5 x 0.3 behind a teammate driving
// (not of the bot's squad), nothing without a driver.
TEST(Bots_RemasterApcPodSeatsCountNoGun)
{
	REMASTER_ONLY();
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	TitanLevel level(gameDir, "Backstab", 16, true, "mods/Project_Remaster_v17_Final", false, nullptr, "gpm_coop");
	CHECK(level.loaded);
	if (!level.loaded) {
		return;
	}
	game::BotSettings settings;
	loadAircraftAi(level, settings);
	float x = 0;
	for (const char* name : {"eu_apc", "as_apc"}) {
		for (const bool driven : {false, true}) {
			x += 40;
			const float z = 0;
			world::Object* apc = level.objects.create(level.templates.find(name), {x, level.world->terrainHeight(x, z) + 2.5f, z});
			CHECK(apc != nullptr);
			if (!apc) {
				return;
			}
			apc->setTeam(2);
			level.run(1.0f);
			// Aboard with the use key from beside the hull (the first free seat, the driver's first).
			const auto board = [&](const char* who, bool ai) {
				const int id = level.world->addPlayer(who, 2, ai);
				CHECK(level.world->spawnSoldier(id));
				game::GameWorld::PlayerInfo p;
				level.world->playerInfo(id, p);
				const auto at = apc->position();
				level.world->setSoldierPosition(p.soldier, {at.x, at.y, at.z});
				game::SoldierInput use;
				use.use = true;
				level.world->setInput(id, use);
				level.run(1 / 30.0f);
				level.world->setInput(id, {});
				level.run(1 / 30.0f);
				level.world->playerInfo(id, p);
				CHECK(p.vehicle == apc->id());
				return id;
			};
			if (driven) {
				board("Driver", false);
			}
			const int bot = board("Bot", true);
			int podSeat = -1;
			const auto seats = level.world->vehicleSeats(apc->id());
			for (size_t i = 0; i < seats.size() && podSeat < 0; ++i) {
				std::string seat = seats[i].name;
				std::transform(seat.begin(), seat.end(), seat.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				podSeat = seat.find("podcannon") != std::string::npos ? static_cast<int>(i) : -1;
			}
			CHECK(podSeat > 1);
			game::SoldierInput input;
			input.switchSeat = podSeat + 1;
			level.world->setInput(bot, input);
			level.run(1 / 30.0f);
			level.world->setInput(bot, {});
			level.run(1 / 30.0f);
			game::GameWorld::PlayerInfo info;
			CHECK(level.world->playerInfo(bot, info) && info.vehicle == apc->id() && info.seat == podSeat);
			game::BotManager bots(*level.world, settings);
			bots.update(1 / 30.0f); // the bot known, before its input reaches the world
			CHECK(bots.equipment(bot) == "Passenger");
			const float value = bots.seatValueFor(bot, apc->id(), podSeat);
			std::printf("  %s pod seat %d %s: %.3f\n", name, podSeat, driven ? "driven" : "no driver", value);
			if (driven) {
				CHECK(std::fabs(value - 3.0f) < 0.05f);
			} else {
				CHECK(value == 0);
			}
			level.world->removePlayer(bot);
		}
	}
}

// Project Remaster's Titan gun hubs name an auto controller
// (setAutoControllerTemplateName SentryGunAApac / SentryGunAA / Co_Auto /
// SentryGunCannon, AI/AutoControllers.ai): with no one at the console, each
// Titan's AA gun turns on an enemy transport hovering in reach and fires at
// it (the AIDLL bot the manager puts in the seat, GameWorld::
// stepSeatControllers), stops while a person holds the seat and starts again
// once they leave; a ground cannon fires at an enemy soldier below and its
// shells, slow and dropping, land on him (the AI's ballistic lead), and
// holds its fire at one past its reach (the fire plan's on-target test).
TEST(Titan_RemasterAAAutoControllerFiresAtTransport)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	const char* mod = "mods/Project_Remaster_v17_Final";
	if (!gameDir || !test::runs(mod)) return;
	for (const int team : {1, 2}) {
		TitanLevel level(gameDir, "Highway_Tampa", 48, false, mod);
		CHECK(level.loaded);
		if (!level.loaded) return;
		game::BotSettings settings;
		loadAircraftAi(level, settings);
		CHECK(settings.autoController(team == 1 ? "SentryGunAApac" : "SentryGunAA") != nullptr);
		game::BotManager bots(*level.world, settings); // before the spawn: the manager takes the hubs then
		level.world->startRound();
		level.run(0.5f);
		const int enemy = 3 - team;
		const uint32_t titan = level.world->titanOf(team);
		CHECK(titan != 0);
		if (!titan) return;
		const auto seats = level.world->vehicleSeats(titan);
		int aa = -1, ground = -1;
		for (size_t i = 0; i < seats.size(); ++i) {
			if (aa < 0 && seats[i].name.find("aacannon") != std::string::npos) aa = static_cast<int>(i);
			if (ground < 0 && seats[i].name.find("groundcanon") != std::string::npos) ground = static_cast<int>(i);
		}
		CHECK(aa >= 0 && ground >= 0);
		if (aa < 0 || ground < 0) return;
		std::set<uint32_t> counted;
		const auto rounds = [&](const physics::Vec3* near) {
			int n = 0;
			for (const auto& p : level.world->projectiles()) {
				if (p.shooter == titan && p.player == -1 && (!near || physics::lengthSquared(p.origin - *near) < 8.0f * 8.0f)
					&& counted.insert(p.serial).second) {
					++n;
				}
			}
			return n;
		};
		// No enemy about: the guns hold their fire.
		int fired = 0;
		level.afterTick = [&] { fired += rounds(nullptr); };
		level.run(2);
		CHECK(fired == 0);
		// An enemy transport 300 m off the hub, 15 degrees up and off its aim.
		physics::Vec3 eye;
		physics::Mat3 hull;
		float yaw = 0, pitch = 0;
		CHECK(level.world->gunFrame(titan, aa, eye, hull) && level.world->gunAim(titan, aa, yaw, pitch));
		const game::GameWorld::SeatLayout& seat = seats[static_cast<size_t>(aa)];
		const float bearing = std::isfinite(seat.minYaw) ? seat.frameYaw + (seat.minYaw + seat.maxYaw) / 2 + 10 : yaw + 40;
		const float up = 15 * 3.14159265f / 180, round = bearing * 3.14159265f / 180;
		const physics::Vec3 at = eye + hull.transform({std::sin(round) * std::cos(up), std::sin(up), std::cos(round) * std::cos(up)}) * 300.0f;
		world::Object* transport = level.objects.create(level.templates.find(team == 1 ? "eu_at" : "as_at"), {at.x, at.y, at.z});
		CHECK(transport != nullptr);
		if (!transport) return;
		transport->setTeam(enemy);
		const uint32_t transportId = transport->id();
		level.run(2 / 30.0f);
		level.world->setHitPoints(transportId, 1e9f); // it outlives the rounds
		const int pilot = level.world->addPlayer("Pilot", enemy, false);
		CHECK(level.world->spawnSoldier(pilot));
		physics::Vec3 entry;
		CHECK(level.world->entryPoint(transportId, entry));
		game::GameWorld::PlayerInfo info;
		level.world->playerInfo(pilot, info);
		level.world->setSoldierPosition(info.soldier, {entry.x, entry.y - 1.0f, entry.z});
		game::SoldierInput use;
		use.use = true;
		level.world->setInput(pilot, use);
		level.run(1 / 30.0f);
		level.world->setInput(pilot, {});
		const auto pilotAboard = [&] {
			game::GameWorld::PlayerInfo p;
			return level.world->playerInfo(pilot, p) && p.alive && p.vehicle == transportId && level.objects.find(transportId);
		};
		CHECK(pilotAboard());
		// The transport held where it hovers; the gun's own rounds counted.
		float aimError = 1e9f;
		level.afterTick = [&] {
			level.world->setBodyVelocity(transportId, {});
			physics::Vec3 from;
			physics::Mat3 frame;
			level.world->gunFrame(titan, aa, from, frame);
			fired += rounds(&from);
			for (const auto& p : level.world->projectiles()) {
				if (p.shooter == titan && p.player == -1 && p.age < 0.1f && physics::lengthSquared(p.origin - from) < 8.0f * 8.0f) {
					const physics::Vec3 to = physics::Vec3 {transport->position().x, transport->position().y, transport->position().z} - p.origin;
					const float c = physics::dot(to, p.velocity) / std::sqrt(physics::lengthSquared(to) * physics::lengthSquared(p.velocity));
					aimError = std::min(aimError, std::acos(std::clamp(c, -1.0f, 1.0f)) * 57.29578f);
				}
			}
		};
		fired = 0;
		level.run(10);
		CHECK(pilotAboard());
		if (!pilotAboard()) return;
		level.world->gunAim(titan, aa, yaw, pitch);
		level.world->gunFrame(titan, aa, eye, hull);
		const physics::Vec3 aim = hull.untransform(physics::Vec3 {transport->position().x, transport->position().y, transport->position().z} - eye);
		const float wantYaw = std::atan2(aim.x, aim.z) * 57.29578f;
		const float wantPitch = std::atan2(aim.y, std::sqrt(aim.x * aim.x + aim.z * aim.z)) * 57.29578f;
		std::fprintf(stderr, "  %s: gun %.1f/%.1f, transport at %.1f/%.1f (%.0f m), %d rounds, nearest %.1f degrees off\n",
			seat.name.c_str(), yaw, pitch + seat.restPitch, wantYaw, wantPitch, std::sqrt(physics::lengthSquared(aim)), fired, aimError);
		CHECK(std::fabs(std::remainder(yaw - wantYaw, 360.0f)) < 3);
		CHECK(std::fabs(pitch + seat.restPitch - wantPitch) < 3);
		CHECK(fired > 0);
		CHECK(aimError < 3);
		// A gunner of the Titan takes the console: the auto controller stops.
		const int gunner = level.world->addPlayer("Gunner", team, false);
		CHECK(level.world->spawnSoldier(gunner));
		level.world->playerInfo(gunner, info);
		CHECK(level.world->setSoldierPosition(info.soldier, level.titan(team)->position + physics::Vec3 {0, 11, -30}));
		float radius = 0;
		int first = -1;
		CHECK(level.world->titanGunEntry(gunner, entry, radius, first));
		CHECK(level.world->setSoldierPosition(info.soldier, entry - physics::Vec3 {0, 1, 0}));
		level.world->setInput(gunner, use);
		level.run(0.1f);
		game::SoldierInput input;
		input.switchSeat = aa + 1;
		level.world->setInput(gunner, input);
		level.run(0.1f);
		level.world->setInput(gunner, {});
		CHECK(level.world->playerInfo(gunner, info) && info.vehicle == titan && info.seat == aa);
		fired = 0;
		level.run(3);
		CHECK(fired == 0);
		CHECK(pilotAboard());
		// Out again: the auto controller takes the seat back and fires on.
		level.world->setInput(gunner, use);
		level.run(1 / 30.0f);
		level.world->setInput(gunner, {});
		CHECK(level.world->playerInfo(gunner, info) && info.vehicle != titan);
		fired = 0;
		level.run(3);
		std::fprintf(stderr, "  after the gunner left: %d rounds\n", fired);
		CHECK(fired > 0);
		// A ground cannon and an enemy soldier on the ground in front of it.
		level.world->removePlayer(pilot);
		level.run(1);
		CHECK(level.world->gunFrame(titan, ground, eye, hull) && level.world->gunAim(titan, ground, yaw, pitch));
		const game::GameWorld::SeatLayout& cannon = seats[static_cast<size_t>(ground)];
		const float side = std::isfinite(cannon.minYaw) ? cannon.frameYaw + (cannon.minYaw + cannon.maxYaw) / 2 : yaw;
		const physics::Vec3 out = hull.transform({std::sin(side * 3.14159265f / 180), 0, std::cos(side * 3.14159265f / 180)});
		const float across = std::sqrt(out.x * out.x + out.z * out.z);
		// The barrel pitches 5..75 degrees down only: from the deck its 75 m/s
		// shells reach some 290 m out over flat ground. At 400 m the solution
		// lies above the barrel's reach and the gun holds its fire.
		const float farX = eye.x + out.x / across * 400, farZ = eye.z + out.z / across * 400;
		const int soldier = level.world->addPlayer("Infantry", enemy, false);
		CHECK(level.world->spawnSoldier(soldier));
		level.world->playerInfo(soldier, info);
		CHECK(level.world->setSoldierPosition(info.soldier, {farX, level.world->terrainHeight(farX, farZ) + 0.1f, farZ}));
		fired = 0;
		level.afterTick = [&] {
			physics::Vec3 from;
			physics::Mat3 frame;
			level.world->gunFrame(titan, ground, from, frame);
			fired += rounds(&from);
		};
		level.run(8);
		std::fprintf(stderr, "  %s at a soldier 400 m out: %d rounds\n", cannon.name.c_str(), fired);
		CHECK(fired == 0);
		const float x = eye.x + out.x / across * 200, z = eye.z + out.z / across * 200;
		CHECK(level.world->setSoldierPosition(info.soldier, {x, level.world->terrainHeight(x, z) + 0.1f, z}));
		const float health = level.world->hitPoints(info.soldier);
		fired = 0;
		level.run(8);
		game::GameWorld::PlayerInfo after;
		level.world->playerInfo(soldier, after);
		const bool hurt = !after.alive || after.soldier != info.soldier || level.world->hitPoints(info.soldier) < health;
		std::fprintf(stderr, "  %s at a soldier 200 m out (%.0f m below): %d rounds, %s\n", cannon.name.c_str(),
			eye.y - level.world->terrainHeight(x, z), fired, hurt ? "hurt" : "unhurt");
		CHECK(fired > 0);
		CHECK(hurt);
	}
}

// A server without bots runs the AI's multiplayer settings in place of
// AIDefault.ai (BF2142.exe 0x004d3450: AIDefaultMultiplayer.ai, no bots,
// AutoControllers.ai) and the objects' AI templates, as main does then: the
// PAC Titan's unmanned AA gun still fires at an enemy transport.
TEST(Titan_RemasterAAAutoControllerWithoutBots)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	const char* mod = "mods/Project_Remaster_v17_Final";
	if (!gameDir || !test::runs(mod)) return;
	TitanLevel level(gameDir, "Highway_Tampa", 48, false, mod);
	CHECK(level.loaded);
	if (!level.loaded) return;
	game::BotSettings settings;
	game::registerBotConsoleMethods(level.console, settings);
	std::string out;
	CHECK(level.console.run("AI/AIDefaultMultiplayer.ai", {}, out));
	for (const std::string& script : world::templateScripts(level.files, "Objects/", true)) {
		if (script.size() > 3 && script.compare(script.size() - 3, 3, ".ai") == 0) {
			level.console.run(script, {}, out);
		}
	}
	CHECK(settings.maxBots == 0 && settings.autoController("SentryGunAApac") != nullptr);
	game::BotManager bots(*level.world, settings);
	level.afterTick = [&] { bots.update(1 / 30.0f); };
	level.world->startRound();
	level.run(0.5f);
	const uint32_t titan = level.world->titanOf(1);
	const auto seats = level.world->vehicleSeats(titan);
	int aa = -1;
	for (size_t i = 0; i < seats.size() && aa < 0; ++i) {
		aa = seats[i].name.find("aacannon") != std::string::npos ? static_cast<int>(i) : -1;
	}
	physics::Vec3 eye;
	physics::Mat3 hull;
	float yaw = 0, pitch = 0;
	CHECK(aa >= 0 && level.world->gunFrame(titan, aa, eye, hull) && level.world->gunAim(titan, aa, yaw, pitch));
	if (aa < 0) return;
	const float up = 15 * 3.14159265f / 180, round = (yaw + 40) * 3.14159265f / 180;
	const physics::Vec3 at = eye + hull.transform({std::sin(round) * std::cos(up), std::sin(up), std::cos(round) * std::cos(up)}) * 300.0f;
	world::Object* transport = level.objects.create(level.templates.find("eu_at"), {at.x, at.y, at.z});
	CHECK(transport != nullptr);
	if (!transport) return;
	transport->setTeam(2);
	const uint32_t transportId = transport->id();
	level.run(2 / 30.0f);
	level.world->setHitPoints(transportId, 1e9f);
	const int pilot = level.world->addPlayer("Pilot", 2, false);
	CHECK(level.world->spawnSoldier(pilot));
	physics::Vec3 entry;
	CHECK(level.world->entryPoint(transportId, entry));
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(pilot, info);
	level.world->setSoldierPosition(info.soldier, {entry.x, entry.y - 1.0f, entry.z});
	game::SoldierInput use;
	use.use = true;
	level.world->setInput(pilot, use);
	level.run(1 / 30.0f);
	level.world->setInput(pilot, {});
	CHECK(level.world->playerInfo(pilot, info) && info.vehicle == transportId);
	std::set<uint32_t> rounds;
	level.afterTick = [&] {
		bots.update(1 / 30.0f);
		level.world->setBodyVelocity(transportId, {});
		for (const auto& p : level.world->projectiles()) {
			if (p.shooter == titan && p.player == -1 && physics::lengthSquared(p.origin - eye) < 8.0f * 8.0f) rounds.insert(p.serial);
		}
	};
	level.run(8);
	std::fprintf(stderr, "  no bots: %zu players, %zu AA rounds at the transport\n", level.world->playerList().size(), rounds.size());
	CHECK(level.world->playerList().size() == 1); // the pilot alone: no bot came
	CHECK(!rounds.empty());
}
