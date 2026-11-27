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
// it off, and fly its transport to a silo with riders who drop in pods. The
// same round runs for each half of the checks (a test each, below): the
// gunship's, and the transport's and its pods'.
static void botsFlyDeckAircraftFromTitan(const bool gunship)
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
		// Up to four minutes; done once both teams have reached everything this
		// half checks below.
		const float ran = level.runUntil(240, [&] {
			if (teams.size() < 2) return false;
			for (const auto& [team, t] : teams) {
				const bool done = gunship ? t.gunshipFlown
					: t.transportFlown && t.maxRiders >= 1 && t.drops >= 1 && t.normalFlight && t.supportStarted && t.supportHeld
						&& t.repositioned && t.gunnersRetained && t.gunShots > 0 && t.spawnedInFlight;
				if (!done) return false;
			}
			return true;
		});
		std::fprintf(stderr, "  %s: %.0f s\n", mod, ran);
		for (const auto& [team, t] : teams) {
			std::fprintf(stderr, "  %s team %d: gunship flown %d, transport flown %d with up to %d riders, %d pod drops, support %d, held %d (%d ticks), repositioned %d, retained gunners %d, %u gun rounds, restarted %d, mobile spawn %d\n",
				mod, team, t.gunshipFlown, t.transportFlown, t.maxRiders, t.drops, t.normalFlight, t.supportHeld, t.supportHoldTicks, t.repositioned, t.gunnersRetained, t.gunShots, t.restartedSortie, t.spawnedInFlight);
			if (gunship) {
				CHECK(t.gunshipFlown);
				continue;
			}
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

TEST(Titan_BotsFlyDeckAircraftFromTitan_Gunship) { botsFlyDeckAircraftFromTitan(true); }
TEST(Titan_BotsFlyDeckAircraftFromTitan_TransportAndPods) { botsFlyDeckAircraftFromTitan(false); }

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
		// Up to four minutes; done once the guns checked below are back in service
		// (four a team) and every engineer has been at it (the consoles are
		// checked after).
		const float ran = level.runUntil(240, [&] {
			for (int team : {1, 2}) {
				int up = 0;
				for (uint32_t gun : guns[team]) {
					up += level.world->hitPoints(gun) > 0;
				}
				if (up < 4) return false;
			}
			for (const auto& [id, team] : repairers) {
				if (repairing[id] <= 0) return false;
			}
			return true;
		});
		std::fprintf(stderr, "  %s: %.0f s\n", mod, ran);
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
	// Only team 1's consoles go down: only its Titan's grids (both teams' on
	// it) are built and compared. The other Titan's mesh stays as it was, so
	// none of its grids would be cast again.
	const auto* found = level.titan(1);
	CHECK(found != nullptr);
	if (!found) return;
	const uint32_t titan = found->object;
	const auto* other = level.titan(2);
	CHECK(other != nullptr);
	if (!other) return;
	const uint32_t otherTitan = other->object, otherVersion = level.world->titanMeshVersion(otherTitan);
	game::BotManager kept(*level.world, settings);
	kept.prepareTitanGrids(titan);
	const std::string before = kept.titanGridStats();
	CHECK(before.find("titan grid") != std::string::npos);
	const int attacker = level.world->addPlayer("Eu", 2, true);
	for (const auto& part : level.world->titanParts(1)) {
		if (part.kind == game::GameWorld::TitanPartInfo::Console) {
			level.world->applyDamage(part.id, 1e5f, attacker);
		}
	}
	level.run(1);
	CHECK(level.world->titanMeshVersion(otherTitan) == otherVersion);
	kept.prepareTitanGrids(titan);
	game::BotManager whole(*level.world, settings);
	whole.prepareTitanGrids(titan);
	std::fprintf(stderr, "  before:\n%s  after:\n%s", before.c_str(), kept.titanGridStats().c_str());
	CHECK(kept.titanGridStats() != before);
	CHECK(kept.titanGridStats() == whole.titanGridStats());
}

// enhanced.botAsyncPaths: the consoles' and the core's fields are made at the
// load; when a part goes down the bots keep the last grid and its fields
// while the new grid is made off the main thread, take the new grid (with
// the fields carried over to its cells) kTitanJobDelay (30) updates on, its
// fields made anew 30 after that and the landing cells 30 after those. The
// result matches a grid and fields made from scratch on the changed Titan,
// with worker threads or without.
TEST(Titan_BotAsyncFieldsStaleThenSwap)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const int threads : {2, 0}) {
		TitanLevel level(gameDir, "Bridge_at_Remagen", 48, true, "mods/bf2142");
		CHECK(level.loaded);
		if (!level.loaded) return;
		game::BotSettings settings;
		const bool enhanced = true;
		settings.enhanced = &enhanced;
		settings.botTitan = &enhanced;
		settings.botAsyncPaths = &enhanced;
		const auto* found = level.titan(1);
		CHECK(found != nullptr);
		if (!found) return;
		const uint32_t titan = found->object;
		uint32_t core = 0, console = 0;
		for (const auto& part : level.world->titanParts(1, titan)) {
			core = part.kind == game::GameWorld::TitanPartInfo::Core ? part.id : core;
			console = part.kind == game::GameWorld::TitanPartInfo::Console && !console ? part.id : console;
		}
		CHECK(core != 0 && console != 0);
		game::BotManager bots(*level.world, settings);
		bots.setPathWorkers(threads);
		bots.prepareTitanGrids(titan);
		const auto made = bots.titanGridState(titan, 2, core);
		CHECK(made.exists && made.hasField && made.fields >= 2 && made.pending == 0 && !made.rebuilding);
		CHECK(made.landingDone && !made.landingPending);
		CHECK(made.reached > 0);
		const uint32_t before = level.world->titanMeshVersion(titan);
		CHECK(made.version == before);
		const int attacker = level.world->addPlayer("Eu", 2, true);
		level.world->applyDamage(console, 1e5f, attacker); // one console down: its mesh goes to its wreck's
		level.run(1);
		const uint32_t after = level.world->titanMeshVersion(titan);
		CHECK(after != before);
		// Asked for (the pods' landing spots): the new grid is being made, the last one is kept meanwhile.
		const size_t spots = bots.titanLandingSpots(1, 2).size();
		auto now = bots.titanGridState(titan, 2, core);
		CHECK(now.version == before && now.rebuilding && now.fieldSum == made.fieldSum);
		CHECK(spots == made.landingCells);
		int updates = 0;
		while (now.rebuilding && updates < 100) {
			bots.update(1 / 30.0f);
			++updates;
			now = bots.titanGridState(titan, 2, core);
			if (now.rebuilding) {
				CHECK(now.version == before && now.fieldSum == made.fieldSum); // the stale grid and field, unchanged
			}
		}
		CHECK(updates == 30);
		// Taken: the new grid with the core's field carried over (the wrecked console's is dropped), made anew.
		CHECK(now.version == after && now.hasField && now.pending > 0 && now.fields == made.fields - 1);
		CHECK(now.landingPending && now.landingDone);
		updates = 0;
		while ((now.pending > 0 || now.landingPending) && updates < 200) {
			bots.update(1 / 30.0f);
			++updates;
			now = bots.titanGridState(titan, 2, core);
		}
		CHECK(updates == 60); // the fields 30 on, the landing cells 30 after them
		CHECK(now.pending == 0 && !now.landingPending);
		// The same as made from scratch on the changed Titan.
		game::BotManager whole(*level.world, settings);
		whole.setPathWorkers(threads);
		whole.prepareTitanGrids(titan);
		const auto fresh = whole.titanGridState(titan, 2, core);
		std::fprintf(stderr, "  %d threads: %zu cells, core field %zu cells (%.0f) before, %zu (%.0f) after, %zu (%.0f) made whole; landing %zu / %zu\n",
			threads, now.cells, made.reached, made.fieldSum, now.reached, now.fieldSum, fresh.reached, fresh.fieldSum, now.landingCells,
			fresh.landingCells);
		CHECK(fresh.cells == now.cells);
		CHECK(fresh.reached == now.reached && fresh.fieldSum == now.fieldSum);
		CHECK(fresh.landingCells == now.landingCells);
	}
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
// order at its own place stands). A test per setting below.
static void botCommanderMovesTheTitans(const bool commander)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* mod : {"mods/bf2142", "mods/Project_Remaster_v17_Final"}) {
		if (!test::runs(mod)) continue;
		{
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
			// Commander on: up to 150 s, done once both Titans meet every check below (the order and the UAV
			// come at the first think, the 100 m at about 95 s). Off: the switch is read first thing in
			// titanCommand, which runs every 10 s; switched on, the order (over 30 m off) and the UAV are there
			// within 1 s and the Titan is 5 m on by 6 s, so 30 s (three thinks, five times that) shows none.
			float ran = 0;
			if (commander) {
				ran = level.runUntil(150, [&] {
					for (int team : {1, 2}) {
						const auto* found = level.titan(team);
						if (!found || !found->hasTarget) return false;
						const auto t = *found;
						float silo = 1e9f;
						for (const auto& s : level.world->missileSilos()) {
							silo = std::min(silo, std::hypot(t.target.x - s.position.x, t.target.z - s.position.z));
						}
						if (!(std::hypot(t.target.x - start[team].x, t.target.z - start[team].z) > 30
								&& std::hypot(t.position.x - start[team].x, t.position.z - start[team].z) > 100 && silo <= 125
								&& !blocks(team, t.target) && uav[team])) {
							return false;
						}
					}
					return true;
				});
			} else {
				ran = 30;
				level.run(ran);
			}
			std::fprintf(stderr, "  %s commander %d: %.0f s\n", mod, commander, ran);
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
			// Up to 175 s, done once each Titan's missile has hurt the enemy's.
			const float hit = level.runUntil(175, [&] {
				return level.titan(2)->hitPoints < before[2] && level.titan(1)->hitPoints < before[1];
			});
			std::fprintf(stderr, "  %s: missiles hit after %.0f s\n", mod, hit);
			for (int team : {1, 2}) {
				const float own = level.titan(team)->hitPoints, enemy = level.titan(3 - team)->hitPoints;
				std::fprintf(stderr, "  %s team %d: own Titan %.0f -> %.0f, enemy's %.0f -> %.0f\n", mod, team, before[team], own, before[3 - team], enemy);
				CHECK(enemy < before[3 - team]);
			}
			// The EU's shield down: the PAC Titan goes beside it.
			addBots();
			level.world->setHitPoints(level.titan(2)->object, level.titan(2)->maxHitPoints * 0.59f);
			// Up to 25 s, done once the PAC order is in the ring and clear of the missile paths.
			const float ordered = level.runUntil(25, [&] {
				const auto pac = *level.titan(1);
				const auto eu = *level.titan(2);
				const float apart = pac.radius + eu.radius;
				const float beside = std::hypot(pac.target.x - eu.position.x, pac.target.z - eu.position.z);
				return beside >= apart + 20 && beside <= apart + 90 && !blocks(1, pac.target);
			});
			std::fprintf(stderr, "  %s: PAC ordered beside after %.0f s\n", mod, ordered);
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

TEST(Titan_BotCommanderMovesTheTitans_Off) { botCommanderMovesTheTitans(false); }
TEST(Titan_BotCommanderMovesTheTitans_On) { botCommanderMovesTheTitans(true); }

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

namespace {

// Suez_Canal in gpm_ti with the bots' scripts and both meshes, as main loads them.
struct SuezBots {
	TitanLevel level;
	game::BotSettings settings;
	bool enhanced = true, titanPlay = true;
	std::optional<ai::NavMesh> mesh, vehicleMesh;
	std::optional<game::BotManager> bots;
	SuezBots(const char* gameDir, bool titan) : level(gameDir, "Suez_Canal", 48, true, "mods/bf2142", true), titanPlay(titan)
	{
		CHECK(level.loaded);
		if (!level.loaded) return;
		loadAircraftAi(level, settings);
		std::string out;
		level.console.run("AI/AIDefaultStrategies.ai", {}, out);
		level.console.run("AI/AIDefaultStrategiesAdd.ai", {}, out);
		level.console.run("Levels/Suez_Canal/ai/AI.ai", {}, out);
		settings.viewDistance = 300;
		settings.enhanced = &enhanced;
		settings.botTitan = &titanPlay;
		bots.emplace(*level.world, settings);
		for (const char* map : {"Infantry", "Vehicle"}) {
			std::vector<uint8_t> qtr, clb;
			const std::string base = std::string("Levels/Suez_Canal/AIPathFinding/") + map;
			CHECK(level.files.readFile(base + ".qtr", qtr) && level.files.readFile(base + ".clb", clb));
			(map[0] == 'I' ? mesh : vehicleMesh) = ai::parseNavMesh(qtr, clb);
		}
		CHECK(mesh.has_value() && vehicleMesh.has_value());
		bots->setNavMesh(mesh ? &*mesh : nullptr);
		bots->setVehicleNavMesh(vehicleMesh ? &*vehicleMesh : nullptr);
		level.afterTick = [this] { bots->update(1 / 30.0f); };
	}
};

} // namespace

// Suez_Canal's infantry mesh is its Conquest one (x -712..260): in Titan mode the PAC ground base (the
// Outpost, its spawns at x 550..620) and silos 901, 905 and 906 lie off it, 45 m and more from its edge.
// Retail's follower finds no path there and the soldier stands (the playtest: PAC's bots took no silo).
// enhanced.botTitan: a bot on foot at the PAC base walks the terrain to each of those silos, into its
// capture sphere (radius 6, gpm_ti.py's radius trigger); with the switch off it stays where it is.
TEST(Titan_BotsWalkOffMeshToSuezSilos)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const bool titanPlay : {false, true}) {
		for (const int target : {906, 901, 905}) {
			if (!titanPlay && target != 906) continue;
			SuezBots suez(gameDir, titanPlay);
			if (!suez.level.loaded || !suez.mesh) return;
			auto& world = *suez.level.world;
			physics::Vec3 centre;
			float radius = 0;
			for (const auto& p : world.controlPointList()) {
				if (p.id == target) {
					centre = p.position;
					radius = p.radius;
				} else if (p.capturable) {
					world.setControlPointTeam(p.id, 1); // the others PAC's: this one the objective
				}
			}
			CHECK(radius == 6);
			CHECK(suez.mesh->findPolygon({centre.x, centre.y, centre.z}) < 0);
			const int id = world.addPlayer("Walker", 1, true);
			CHECK(world.spawnSoldier(id));
			game::GameWorld::PlayerInfo info;
			CHECK(world.playerInfo(id, info));
			const physics::Vec3 base {566, world.terrainHeight(566, -314), -314}; // a PAC ground spawn at the Outpost
			CHECK(suez.mesh->findPolygon({base.x, base.y, base.z}) < 0);
			CHECK(world.setSoldierPosition(info.soldier, base));
			bool inside = false;
			const float ran = suez.level.runUntil(titanPlay ? 320 : 20, [&] {
				game::GameWorld::PlayerInfo now;
				inside = world.playerInfo(id, now) && std::sqrt(physics::lengthSquared(now.position - centre)) <= radius;
				return inside;
			});
			CHECK(world.playerInfo(id, info));
			const float moved = std::hypot(info.position.x - base.x, info.position.z - base.z);
			std::fprintf(stderr, "  botTitan %d silo %d: %.0f s, at %.0f %.0f %.0f, %.1f m from its point, %.0f m from the base\n",
				titanPlay, target, ran, info.position.x, info.position.y, info.position.z,
				std::sqrt(physics::lengthSquared(info.position - centre)), moved);
			if (!titanPlay) {
				CHECK(moved < 5);
				continue;
			}
			CHECK(inside);
			// It stays for the capture (timeToGetControl 20 s; nobody takes the point here, so it is still the objective).
			float held = 0;
			suez.level.afterTick = [&] {
				suez.bots->update(1 / 30.0f);
				game::GameWorld::PlayerInfo now;
				held += world.playerInfo(id, now) && std::sqrt(physics::lengthSquared(now.position - centre)) <= radius ? 1 / 30.0f : 0;
			};
			suez.level.run(25);
			std::fprintf(stderr, "    then %.1f s of 25 inside\n", held);
			CHECK(held >= 20);
		}
	}
}

// enhanced.botTitan: a bot manning a Titan's ground cannon fights an enemy on
// the ground in the gun's arc. The shipped guns' firearms name no
// weaponTemplate (retail bots never man them), so without the AI weapon made
// from the round (titanGunWeaponAi) the gunner scored no target and sat in
// Idle; and the guns hang far over the ground, past the level's view, so the
// sight reaches as far as the gun (sense). Bots.cpp: seatArmament,
// titanGunReaches.
TEST(Titan_BotGunnerFiresAtEnemyInRange)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	TitanLevel level(gameDir, "Minsk", 48);
	CHECK(level.loaded);
	if (!level.loaded) return;
	game::BotSettings settings;
	loadAircraftAi(level, settings);
	settings.viewDistance = 280; // about Verdun's
	const bool enhanced = true;
	settings.enhanced = &enhanced;
	settings.botTitan = &enhanced;
	const uint32_t eu = level.world->titanOf(2);
	CHECK(eu != 0);
	const std::vector<game::GameWorld::SeatLayout> seats = level.world->vehicleSeats(eu);
	int ground = -1;
	for (size_t i = 0; i < seats.size(); ++i) {
		ground = seats[i].name == "eu_groundcanone3_01_hub" ? static_cast<int>(i) : ground;
	}
	CHECK(ground >= 0);
	if (ground < 0) return;
	// The bot into a gun at its console, then over to the rear ground cannon.
	const int gunner = level.world->addPlayer("Gunner", 2, true);
	CHECK(level.world->spawnSoldier(gunner));
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(gunner, info);
	const auto titan = level.titan(2);
	CHECK(level.world->setSoldierPosition(info.soldier, titan->position + physics::Vec3 {0, 11, -30}));
	physics::Vec3 entry;
	float radius = 0;
	int seat = -1;
	CHECK(level.world->titanGunEntry(gunner, entry, radius, seat));
	CHECK(level.world->setSoldierPosition(info.soldier, entry - physics::Vec3 {0, 1, 0}));
	game::SoldierInput input;
	input.use = true;
	level.world->setInput(gunner, input);
	level.run(0.1f);
	input = {};
	input.switchSeat = ground + 1;
	level.world->setInput(gunner, input);
	level.run(0.1f);
	level.world->setInput(gunner, {});
	level.world->playerInfo(gunner, info);
	CHECK(info.vehicle == eu && info.seat == ground);
	// An enemy on the ground behind the Titan, in the rear cannon's arc, about
	// as far off as the gun is high (45 degrees down).
	physics::Vec3 origin;
	physics::Mat3 hull;
	CHECK(level.world->gunFrame(eu, ground, origin, hull));
	const float frame = seats[static_cast<size_t>(ground)].frameYaw * 3.14159265f / 180;
	physics::Vec3 way = hull.transform({std::sin(frame), 0, std::cos(frame)});
	way.y = 0;
	way = way * (1 / std::sqrt(physics::lengthSquared(way)));
	const float height = origin.y - level.world->terrainHeight(origin.x, origin.z);
	physics::Vec3 at = origin + way * std::clamp(height, 120.0f, 300.0f);
	at.y = level.world->terrainHeight(at.x, at.z);
	const int enemy = level.world->addPlayer("Enemy", 1, false);
	CHECK(level.world->spawnSoldier(enemy));
	game::GameWorld::PlayerInfo target;
	level.world->playerInfo(enemy, target);
	CHECK(level.world->setSoldierPosition(target.soldier, at));
	std::fprintf(stderr, "  gun %.0f m over the ground, the enemy %.0f m off\n", height, std::sqrt(physics::lengthSquared(at - origin)));
	game::BotManager bots(*level.world, settings);
	level.afterTick = [&] { bots.update(1 / 30.0f); };
	bool targeted = false;
	const float ran = level.runUntil(30, [&] {
		level.world->playerInfo(gunner, info);
		targeted = targeted || bots.target(gunner) == enemy;
		return info.gunShots > 0;
	}, 0.5f);
	level.world->playerInfo(gunner, info);
	std::fprintf(stderr, "  after %.1f s: behaviour %s, target %d, %u rounds\n", ran, bots.behaviour(gunner).c_str(), bots.target(gunner), info.gunShots);
	CHECK(info.vehicle == eu && info.seat == ground);
	CHECK(targeted);
	CHECK(info.gunShots > 0);
	// The setting off (retail's bots): the gun scores no target.
	const bool off = false;
	game::BotSettings retail = settings;
	retail.botTitan = &off;
	game::BotManager plain(*level.world, retail);
	level.afterTick = [&] { plain.update(1 / 30.0f); };
	level.run(8);
	CHECK(plain.target(gunner) != enemy);
}
