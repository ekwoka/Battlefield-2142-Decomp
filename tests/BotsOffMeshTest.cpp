#include "AI/NavMesh.h"
#include "Game/Bots.h"
#include "TitanLevel.h"
#include "World/TemplateFiles.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace dice::hfe;
using titantest::TitanLevel;

namespace {

// The calling thread's CPU time: a bot update's own work, without the time other tests running
// beside it in the suite take the core from it (or keep the low-priority workers from a job it waits on).
double threadSeconds()
{
	timespec t {};
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_nsec) * 1e-9;
}

// A level's layout with the bots' scripts and both meshes, as main loads them (the level's ai/AI.ai,
// the default strategies, the layout's StrategicAreas.ai when it has one); botTitan with `titan`.
struct LevelBots {
	TitanLevel level;
	game::BotSettings settings;
	bool enhanced = true, offMesh = true, titan = false, asyncPaths = true;
	std::optional<ai::NavMesh> mesh, vehicleMesh;
	std::optional<game::BotManager> bots;
	std::vector<int> players;
	double botSeconds = 0, worstTick = 0;
	std::vector<double> ticks; // each update's time
	int botUpdates = 0, worstAt = 0;
	LevelBots(const char* gameDir, const std::string& name, const char* mode, int size, bool walkOffMesh, bool titanPlay = false)
		: level(gameDir, name, size, true, "mods/bf2142", true, nullptr, mode), offMesh(walkOffMesh), titan(titanPlay)
	{
		CHECK(level.loaded);
		if (!level.loaded) return;
		game::registerBotConsoleMethods(level.console, settings);
		std::string out;
		CHECK(level.console.run("AI/AIBehaviours.ai", {}, out));
		CHECK(level.console.run("AI/AIDefault.ai", {}, out));
		for (const std::string& script : world::templateScripts(level.files, "Objects/", true)) {
			if (script.size() > 3 && script.compare(script.size() - 3, 3, ".ai") == 0) {
				level.console.run(script, {}, out);
			}
		}
		const std::string path = "Levels/" + name + "/";
		level.console.run(path + "ai/AI.ai", {}, out);
		level.console.run("AI/AIDefaultStrategies.ai", {}, out);
		level.console.run("AI/AIDefaultStrategiesAdd.ai", {}, out);
		const std::string areas = path + "GameModes/" + mode + "/" + std::to_string(size) + "/ai/StrategicAreas.ai";
		if (level.files.exists(areas)) {
			level.console.run(areas, {}, out);
		}
		settings.viewDistance = 300;
		settings.maxBots = 64; // as main with enhanced.singleplayBotCount: AIDefault.ai's 16 doesn't cap them
		settings.enhanced = &enhanced;
		settings.botOffMesh = &offMesh;
		settings.botTitan = &titan;
		settings.botAsyncPaths = &asyncPaths;
		bots.emplace(*level.world, settings);
		for (const char* map : {"Infantry", "Vehicle"}) {
			std::vector<uint8_t> qtr, clb;
			const std::string base = path + "AIPathFinding/" + map;
			CHECK(level.files.readFile(base + ".qtr", qtr) && level.files.readFile(base + ".clb", clb));
			(map[0] == 'I' ? mesh : vehicleMesh) = ai::parseNavMesh(qtr, clb);
		}
		CHECK(mesh.has_value() && vehicleMesh.has_value());
		bots->setNavMesh(mesh ? &*mesh : nullptr);
		bots->setVehicleNavMesh(vehicleMesh ? &*vehicleMesh : nullptr);
		level.afterTick = [this] {
			const double t0 = threadSeconds();
			bots->update(1 / 30.0f);
			const double took = threadSeconds() - t0;
			botSeconds += took;
			ticks.push_back(took);
			if (botUpdates >= 150 && took > worstTick) {
				worstTick = took; // after the first 5 s (botTitan makes its Titans' grids then)
				worstAt = botUpdates;
			}
			++botUpdates;
		};
	}
	void addBots(int perTeam, int lastTeam = 2)
	{
		for (int i = 0; i < perTeam; ++i) {
			for (int team = 1; team <= lastTeam; ++team) {
				const int id = bots->addBot("", team);
				CHECK(id >= 0);
				if (id >= 0) {
					CHECK(level.world->spawnSoldier(id));
					players.push_back(id);
				}
			}
		}
	}
	bool onMesh(const physics::Vec3& p) const { return mesh->findPolygon({p.x, p.y, p.z}) >= 0; }
};

} // namespace

// Cerbere_Landing's one infantry mesh is made for its 16-player layout: in gpm_cq 32 every PAC spawn
// (the Harbor's, z -190..-250) lies south of it, as do the PAC Harbor and the EU Command Center points;
// the layout has no StrategicAreas.ai (the SAI's areas come from the control points, ensureAreas), so
// the PAC squads do get attack orders, but the follower finds no path from off the mesh and the
// soldiers stand (the playtest: every PAC bot idle at its spawn). enhanced.botOffMesh: they walk the
// terrain onto the mesh and on to their objective. With 16 bots a side most PAC bots leave their
// spawns and reach the mesh (where they meet EU's and fight); with PAC's alone one stands in a point
// PAC doesn't hold within two minutes. With the switch off they stay (retail).
TEST(Bots_OffMeshCerbere32PacLeaveSpawn)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (int run = 0; run < 4; ++run) {
		const int teams = run < 2 ? 2 : 1;
		const bool walk = run % 2 == 1;
		LevelBots round(gameDir, "Cerbere_Landing", "gpm_cq", 32, walk);
		if (!round.level.loaded || !round.mesh) return;
		auto& world = *round.level.world;
		CHECK(!round.level.files.exists("Levels/Cerbere_Landing/GameModes/gpm_cq/32/ai/StrategicAreas.ai"));
		round.addBots(16, teams);
		CHECK(round.players.size() == static_cast<size_t>(16 * teams));
		std::vector<physics::Vec3> start;
		int offStart = 0;
		for (const int id : round.players) {
			game::GameWorld::PlayerInfo info;
			world.playerInfo(id, info);
			start.push_back(info.position);
			offStart += info.team == 1 && !round.onMesh(info.position);
		}
		CHECK(offStart == 16); // every PAC spawn off the mesh
		// Every second: whether a PAC soldier on foot stands inside a capturable point PAC doesn't hold.
		const float ran = round.level.runUntil(120, [&] {
			for (const auto& p : world.controlPointList()) {
				if (!p.capturable || p.team == 1) continue;
				for (const int id : round.players) {
					game::GameWorld::PlayerInfo info;
					if (world.playerInfo(id, info) && info.alive && info.team == 1 && !info.vehicle
						&& std::sqrt(physics::lengthSquared(info.position - p.position)) <= p.radius) {
						return true;
					}
				}
			}
			return false;
		});
		const float reached = ran < 120 ? ran : -1;
		if (reached >= 0) round.level.run(120 - ran); // the same two minutes for the counts and the timing
		int moved = 0, onMesh = 0;
		for (size_t i = 0; i < round.players.size(); ++i) {
			game::GameWorld::PlayerInfo info;
			world.playerInfo(round.players[i], info);
			if (info.team != 1) continue;
			moved += !info.alive || info.vehicle || physics::lengthSquared(info.position - start[i]) > 20 * 20;
			onMesh += !info.alive || info.vehicle || round.onMesh(info.position);
		}
		const auto& stats = round.bots->groundStats();
		std::fprintf(stderr,
			"  %d bots, botOffMesh %d: %d of 16 PAC bots left their spawns, %d on the mesh; one in a point after %.0f s;"
			" bots %.3f ms a tick (at most %.1f); %d grid searches (%d none, %.2f ms each, at most %.2f, %.3f ms a tick), %d unfinished, %d reused, %d put off; %ld cells (at most %d), %ld steps checked\n",
			16 * teams, walk, moved, onMesh, reached, 1000.0 * round.botSeconds / round.botUpdates, 1000.0 * round.worstTick, stats.searches, stats.empty,
			stats.searches ? 1000.0 * stats.seconds / stats.searches : 0.0, 1000.0 * stats.maxSeconds,
			1000.0 * stats.seconds / round.botUpdates, stats.unfinished, stats.reused, stats.deferred, stats.cells, stats.maxCells, stats.steps);
		if (!walk) {
			CHECK(moved <= 2);
			CHECK(reached < 0);
			CHECK(stats.searches == 0);
			continue;
		}
		CHECK(moved >= 12);
		CHECK(onMesh >= 8);
		if (teams == 1) {
			CHECK(reached >= 0);
		}
	}
}

// A 16-player Conquest round without --enhanced, loaded as main loads it: Verdun's and
// Cerbere_Landing's gpm_cq 16 layouts have no strategic areas (Verdun's StrategicAreas.ai is a
// comment, Cerbere's is missing), so ensureAreas makes them from the control points. Their flags lie
// off the vehicle mesh (all five of Verdun's by 1-3 m; Cerbere's Command Center on a roof), and a
// driver heads for the area's order position itself: with the flag as that position the drivers'
// path requests failed and every bot sat in a vehicle at its base all round (Verdun: no round fired).
// The made-up areas' order positions lie on each map's mesh now: the vehicles leave their bases,
// the bots fire and stand in points to take them.
TEST(Bots_RetailConquest16Plays)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const char* name : {"Verdun", "Cerbere_Landing"}) {
		LevelBots round(gameDir, name, "gpm_cq", 16, false);
		if (!round.level.loaded || !round.mesh) return;
		round.enhanced = false; // retail's bots: no enhanced.* switch on
		auto& world = *round.level.world;
		round.addBots(8);
		CHECK(round.players.size() == 16);
		round.level.run(1);
		const auto& areas = round.bots->settings().strategy.areas;
		CHECK(areas.size() == world.controlPointList().size());
		int placed = 0;
		for (const ai::StrategicArea& area : areas) {
			for (int m = 0; m < 2; ++m) {
				const ai::NavMesh& mesh = m == 0 ? *round.mesh : *round.vehicleMesh;
				const auto& o = area.orderPosition[m];
				placed += area.hasOrderPosition[m] && mesh.findPolygon({o[0], o[1], o[2]}) >= 0;
			}
		}
		CHECK(placed == static_cast<int>(areas.size()) * 2); // each area's order position on both meshes
		std::vector<physics::Vec3> start;
		for (const int id : round.players) {
			game::GameWorld::PlayerInfo info;
			world.playerInfo(id, info);
			start.push_back(info.position);
		}
		std::map<uint32_t, physics::Vec3> vehicleStart;
		for (const auto& v : world.vehicleList()) vehicleStart[v.object] = v.position;
		// Every second: the bots inside a capturable point their team doesn't hold (the game mode's
		// script, which hands the point over, doesn't run here), and how far the vehicles got.
		int capturing = 0;
		float driven = 0;
		round.level.runUntil(180, [&] {
			for (const auto& p : world.controlPointList()) {
				if (!p.capturable) continue;
				for (const int id : round.players) {
					game::GameWorld::PlayerInfo info;
					capturing += world.playerInfo(id, info) && info.alive && info.team != p.team
						&& std::sqrt(physics::lengthSquared(info.position - p.position)) <= std::max(p.radius, 10.0f);
				}
			}
			for (const auto& v : world.vehicleList()) {
				if (const auto it = vehicleStart.find(v.object); it != vehicleStart.end()) {
					driven = std::max(driven, std::sqrt(physics::lengthSquared(v.position - it->second)));
				}
			}
			return false;
		});
		int moved = 0, fired = 0;
		for (size_t i = 0; i < round.players.size(); ++i) {
			game::GameWorld::PlayerInfo info;
			world.playerInfo(round.players[i], info);
			moved += !info.alive || physics::lengthSquared(info.position - start[i]) > 20 * 20;
			const auto rounds = world.playerRounds(round.players[i], false, false);
			fired += rounds.empty() ? 0 : rounds[0].count;
		}
		std::fprintf(stderr,
			"  %s gpm_cq 16: %d of 16 bots moved over 20 m, %d rounds fired, %d bot-seconds in points to take,"
			" a vehicle %.0f m from its spawn\n",
			name, moved, fired, capturing, driven);
		CHECK(moved >= 8);
		CHECK(fired > 0);
		CHECK(capturing >= 10);
		CHECK(driven > 50);
	}
}

// The grid walk's cost where it searches most: Verdun in Titan mode with 47 bots (botTitan and
// botOffMesh), whose Titan-mode ground lies largely off the Conquest mesh and some of whose goals
// are out of the grid's reach. The bot update's mean and worst tick and the searches' numbers, for
// the report; it checks the searches stay spread out (at most one a tick) and that the bots move.
TEST(Bots_OffMeshVerdunTitanCost)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	LevelBots round(gameDir, "Verdun", "gpm_ti", 48, true, true);
	if (!round.level.loaded || !round.mesh) return;
	round.addBots(23);
	round.addBots(1, 1); // 24 PAC, 23 EU: 47 bots and a player's place
	round.bots->prepareTitanGrids(); // as main does at the level's load (else the first boarder's tick makes them)
	auto& world = *round.level.world;
	std::vector<physics::Vec3> start;
	for (const int id : round.players) {
		game::GameWorld::PlayerInfo info;
		world.playerInfo(id, info);
		start.push_back(info.position);
	}
	// A console of each Titan goes down at 30 s and another at 60 s: new grids and fields mid-round
	// (enhanced.botAsyncPaths: made off the main thread while the bots walk the last ones).
	const auto wreckConsole = [&] {
		for (int team = 1; team <= 2; ++team) {
			for (const auto& part : world.titanParts(team)) {
				if (part.kind == game::GameWorld::TitanPartInfo::Console && !part.wreck) {
					world.applyDamage(part.id, 1e6f, -1);
					break;
				}
			}
		}
	};
	round.level.run(30);
	wreckConsole();
	round.level.run(30);
	wreckConsole();
	round.level.run(30);
	int moved = 0;
	for (size_t i = 0; i < round.players.size(); ++i) {
		game::GameWorld::PlayerInfo info;
		world.playerInfo(round.players[i], info);
		moved += !info.alive || info.vehicle || physics::lengthSquared(info.position - start[i]) > 20 * 20;
	}
	const auto& stats = round.bots->groundStats();
	std::fprintf(stderr,
		"  Verdun gpm_ti, %zu bots, 90 s: %d moved; bots %.3f ms a tick (at most %.1f, tick %d); %d grid searches (%d none, %.2f ms each,"
		" at most %.2f, %.3f ms a tick), %d unfinished, %d reused, %d put off; %ld cells (at most %d), %ld steps checked\n",
		round.players.size(), moved, 1000.0 * round.botSeconds / round.botUpdates, 1000.0 * round.worstTick, round.worstAt, stats.searches,
		stats.empty, stats.searches ? 1000.0 * stats.seconds / stats.searches : 0.0, 1000.0 * stats.maxSeconds,
		1000.0 * stats.seconds / round.botUpdates, stats.unfinished, stats.reused, stats.deferred, stats.cells, stats.maxCells,
		stats.steps);
	std::fprintf(stderr, "    vehicle-mesh searches %d, %.2f ms each, at most %.1f\n", stats.vehicleSearches,
		stats.vehicleSearches ? 1000.0 * stats.vehicleSeconds / stats.vehicleSearches : 0.0, 1000.0 * stats.vehicleMaxSeconds);
	CHECK(round.players.size() == 47);
	CHECK(moved >= static_cast<int>(round.players.size()) / 2);
	// No tick-long hitch: enhanced.botAsyncPaths makes the Titans' fields at the load and anew off the main
	// thread when the consoles go down (each field some 40 ms of rays; a part change cost 0.5 s at once
	// before), and spreads the vehicle-mesh search over ticks (50 ms for one that finds nothing).
#ifdef NDEBUG
	constexpr double kWorst = 0.1;
#else
	constexpr double kWorst = 1.0; // (a sanitizer build)
#endif
	std::vector<double> sorted(round.ticks.begin() + std::min<size_t>(150, round.ticks.size()), round.ticks.end());
	std::sort(sorted.begin(), sorted.end());
	const double p99 = sorted.empty() ? 0 : sorted[sorted.size() * 99 / 100];
	std::fprintf(stderr, "    bot update p99 %.1f ms, worst %.1f ms\n", 1000.0 * p99, 1000.0 * round.worstTick);
	CHECK(round.worstTick < kWorst);
	CHECK(stats.searches <= round.botUpdates);
}

// enhanced.botAsyncPaths: the Titans' fields and grids made on worker threads are taken at fixed updates
// (and waited for then if late), so the round plays the same with two threads, with none (each job made
// on the main thread when it is due) and with two again: Verdun gpm_ti with 47 bots and a console of each
// Titan going down at 15 s and 30 s, every bot's place and behaviour and the world's fingerprint at 45 s.
TEST(Bots_AsyncPathsVerdunTitanSameRound)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	struct Outcome {
		uint64_t fingerprint = 0;
		std::vector<physics::Vec3> places;
		std::vector<std::string> behaviours;
	};
	const auto play = [&](int threads) {
		Outcome out;
		LevelBots round(gameDir, "Verdun", "gpm_ti", 48, true, true);
		if (!round.level.loaded || !round.mesh) return out;
		round.bots->setPathWorkers(threads);
		round.addBots(23);
		round.addBots(1, 1);
		round.bots->prepareTitanGrids();
		auto& world = *round.level.world;
		const auto wreckConsole = [&] {
			for (int team = 1; team <= 2; ++team) {
				for (const auto& part : world.titanParts(team)) {
					if (part.kind == game::GameWorld::TitanPartInfo::Console && !part.wreck) {
						world.applyDamage(part.id, 1e6f, -1);
						break;
					}
				}
			}
		};
		round.level.run(15);
		wreckConsole();
		round.level.run(15);
		wreckConsole();
		round.level.run(15);
		out.fingerprint = world.stateFingerprint();
		for (const int id : round.players) {
			game::GameWorld::PlayerInfo info;
			world.playerInfo(id, info);
			out.places.push_back(info.position);
			out.behaviours.push_back(round.bots->behaviour(id));
		}
		return out;
	};
	const Outcome two = play(2), none = play(0), again = play(2);
	if (two.places.empty()) return;
	std::fprintf(stderr, "  fingerprints %016llx %016llx %016llx\n", static_cast<unsigned long long>(two.fingerprint),
		static_cast<unsigned long long>(none.fingerprint), static_cast<unsigned long long>(again.fingerprint));
	CHECK(two.fingerprint == none.fingerprint);
	CHECK(two.fingerprint == again.fingerprint);
	CHECK(two.places.size() == none.places.size() && two.places.size() == again.places.size());
	for (size_t i = 0; i < two.places.size() && i < none.places.size() && i < again.places.size(); ++i) {
		CHECK(two.places[i].x == none.places[i].x && two.places[i].y == none.places[i].y && two.places[i].z == none.places[i].z);
		CHECK(two.places[i].x == again.places[i].x && two.places[i].z == again.places[i].z);
		CHECK(two.behaviours[i] == none.behaviours[i] && two.behaviours[i] == again.behaviours[i]);
	}
}
