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
		game::registerBotConsoleMethods(level.console, settings);
		std::string out;
		CHECK(level.console.run("AI/AIBehaviours.ai", {}, out));
		CHECK(level.console.run("AI/AIDefault.ai", {}, out));
		for (const std::string& script : world::templateScripts(level.files, "Objects/", true)) {
			if (script.size() > 3 && script.compare(script.size() - 3, 3, ".ai") == 0) {
				level.console.run(script, {}, out);
			}
		}
		const bool enhanced = true;
		settings.enhanced = &enhanced;
		settings.botTitan = &enhanced;
		game::BotManager bots(*level.world, settings);
		struct Team {
			uint32_t titan = 0, gunship = 0, transport = 0;
			std::vector<int> crew;
			bool gunshipFlown = false, transportFlown = false, landedBack = false;
			int riders = 0, maxRiders = 0, drops = 0;
			std::map<int, bool> rode;
		};
		std::map<int, Team> teams;
		for (int team : {1, 2}) {
			const auto* titan = level.titan(team);
			CHECK(titan != nullptr);
			if (!titan) continue;
			Team& t = teams[team];
			t.titan = titan->object;
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
					transportPiloted = transportPiloted || (info.vehicle == t.transport && info.seat == 0);
					riders += info.vehicle == t.transport && info.seat > 0;
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
					if (v.object == t.transport && transportPiloted && departed[team] && away < 60 && t.drops > 0
						&& std::sqrt(physics::lengthSquared(v.velocity - titan->velocity)) < 1) t.landedBack = true;
				}
				t.maxRiders = std::max(t.maxRiders, departed[team] ? t.maxRiders : riders);
			}
		};
		level.run(240);
		for (const auto& [team, t] : teams) {
			std::fprintf(stderr, "  %s team %d: gunship flown %d, transport flown %d with up to %d riders, %d pod drops, landed back %d\n",
				mod, team, t.gunshipFlown, t.transportFlown, t.maxRiders, t.drops, t.landedBack);
			CHECK(t.gunshipFlown);
			CHECK(t.transportFlown);
			CHECK(t.maxRiders >= 1);
			CHECK(t.drops >= 1);
			CHECK(t.landedBack);
		}
	}
}
