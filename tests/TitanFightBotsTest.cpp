// The bots' fight round the Titans' guns in gpm_ti: gunships on the guns,
// engineers mending them, the attention a gun killing many draws, and the
// gunships' attack runs and evasion.
#include "AI/NavMesh.h"
#include "Game/Bots.h"
#include "TitanLevel.h"
#include "World/Object.h"
#include "World/TemplateFiles.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace dice::hfe;
using titantest::TitanLevel;

namespace {

void loadAi(TitanLevel& level, game::BotSettings& settings)
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
	settings.viewDistance = 300; // No level fog is loaded by this fixture.
}

float flat(const physics::Vec3& d) { return std::sqrt(d.x * d.x + d.z * d.z); }

// A vehicle of `name` for `team` at `at`, made after the round started (the world takes it up on its next tick).
uint32_t place(TitanLevel& level, const char* name, int team, const physics::Vec3& at, float yaw = 0)
{
	world::ObjectTemplate* t = level.templates.find(name);
	CHECK(t != nullptr);
	if (!t) return 0;
	world::Object* made = level.objects.create(t, {at.x, at.y, at.z}, {yaw, 0, 0});
	made->setTeam(team);
	level.run(0.2f);
	return made->id();
}

// Into a vehicle's seat 0: the player tried round it until an entry point takes him.
bool seat(TitanLevel& level, int player, uint32_t vehicle)
{
	game::GameWorld::PlayerInfo info;
	for (float r = 0; r <= 8; r += 1.0f) {
		for (int k = 0; k < 8; ++k) {
			physics::Vec3 at;
			for (const auto& v : level.world->vehicleList()) {
				at = v.object == vehicle ? v.position : at;
			}
			level.world->playerInfo(player, info);
			level.world->setSoldierPosition(info.soldier, {at.x + r * std::sin(k * 0.785f), at.y - 1, at.z + r * std::cos(k * 0.785f)});
			game::SoldierInput use;
			use.use = true;
			level.world->setInput(player, use);
			level.world->tick(1 / 30.0f);
			level.world->setInput(player, {});
			level.world->tick(1 / 30.0f);
			level.world->playerInfo(player, info);
			if (info.vehicle == vehicle) {
				return true;
			}
		}
	}
	return false;
}

// Into a free gun of its own Titan, `skip` the seats not to take: on the deck, then at the gun's entry with the
// use key. The seat, -1 for none.
int manGun(TitanLevel& level, int player, int team, const std::vector<int>& skip = {})
{
	const uint32_t titan = level.titan(team)->object;
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(player, info);
	level.world->setSoldierPosition(info.soldier, level.world->titanToWorld(titan, {0, 11.5f, -40}));
	level.run(0.2f);
	physics::Vec3 entry;
	float radius = 0;
	int seat = -1;
	if (!level.world->titanGunEntry(player, entry, radius, seat, skip)) return -1;
	level.world->setSoldierPosition(info.soldier, entry - physics::Vec3 {0, 1.2f, 0});
	for (int i = 0; i < 6; ++i) {
		game::SoldierInput use;
		use.use = i % 2 == 0;
		level.world->setInput(player, use);
		level.world->tick(1 / 30.0f);
	}
	level.world->setInput(player, {});
	level.world->playerInfo(player, info);
	return info.vehicle == titan ? info.seat : -1;
}

struct Settings {
	game::BotSettings bots;
	bool on = true;
	explicit Settings(TitanLevel& level)
	{
		loadAi(level, bots);
		bots.enhanced = &on;
		bots.botTitan = &on;
		bots.botPilots = &on;
	}
};

} // namespace

// enhanced.botPilots: a bot gunship pilot with the enemy Titan's guns in
// reach (no other enemy about) flies its attack runs at them, straight in
// from outside the hull a little under the hub, and wrecks one within two
// minutes (the old runs, from 0.2 stand-offs below, hit with too few rounds
// as the nose lagged: 750 hit points in two minutes, no gun down).
TEST(Titan_GunshipAttacksTitanGunInReach)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	TitanLevel level(gameDir, "Bridge_at_Remagen", 48, true, "mods/bf2142");
	CHECK(level.loaded);
	if (!level.loaded) return;
	Settings s(level);
	const auto* enemy = level.titan(2);
	CHECK(enemy != nullptr);
	if (!enemy) return;
	const physics::Vec3 titanAt = enemy->position;
	// 400 m off the enemy's Titan, on the ground.
	physics::Vec3 at = titanAt + physics::Vec3 {400, 0, 0};
	at.y = level.world->terrainHeight(at.x, at.z) + 3;
	const uint32_t gunship = place(level, "as_ag", 1, at, -90);
	const int pilot = level.world->addPlayer("Pilot", 1, true);
	CHECK(level.world->spawnSoldier(pilot));
	CHECK(seat(level, pilot, gunship));
	std::map<uint32_t, float> before;
	for (const auto& part : level.world->titanParts(2)) {
		if (part.kind == game::GameWorld::TitanPartInfo::Gun) {
			before[part.id] = part.hitPoints;
		}
	}
	game::BotManager bots(*level.world, s.bots);
	std::map<std::string, int> states;
	level.afterTick = [&] {
		bots.update(1 / 30.0f);
		++states[bots.flyState(pilot)];
	};
	const auto wrecked = [&] {
		int n = 0;
		for (const auto& part : level.world->titanParts(2)) {
			n += before.count(part.id) && part.hitPoints <= 0;
		}
		return n;
	};
	const auto lost = [&] {
		float sum = 0;
		for (const auto& part : level.world->titanParts(2)) {
			if (const auto it = before.find(part.id); it != before.end()) {
				sum += std::max(0.0f, it->second - part.hitPoints);
			}
		}
		return sum;
	};
	const float ran = level.runUntil(120, [&] { return wrecked() >= 1; });
	for (const auto& [state, ticks] : states) {
		std::fprintf(stderr, "  %-24s %6.1f s\n", state.c_str(), ticks / 30.0f);
	}
	std::fprintf(stderr, "  the enemy Titan's guns lost %.0f hit points in %.0f s, %d wrecked\n", lost(), ran, wrecked());
	CHECK(wrecked() >= 1);
}

// enhanced.botPilots: a hunting gunship hit hard (a fifth of its hit points
// at once, from an enemy soldier below) breaks away within two seconds; left
// under a third of them it flies to a supply object that mends it (its
// Titan's deck point) and gets its hit points back, then hunts again.
TEST(Titan_DamagedGunshipEvadesAndGoesForRepair)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	TitanLevel level(gameDir, "Bridge_at_Remagen", 48, true, "mods/bf2142");
	CHECK(level.loaded);
	if (!level.loaded) return;
	Settings s(level);
	const auto* enemy = level.titan(2);
	CHECK(enemy != nullptr);
	if (!enemy) return;
	physics::Vec3 at = enemy->position + physics::Vec3 {400, 0, 0};
	at.y = level.world->terrainHeight(at.x, at.z) + 3;
	const uint32_t gunship = place(level, "as_ag", 1, at, -90);
	const int pilot = level.world->addPlayer("Pilot", 1, true);
	CHECK(level.world->spawnSoldier(pilot));
	CHECK(seat(level, pilot, gunship));
	const int shooter = level.world->addPlayer("Shooter", 2, false);
	CHECK(level.world->spawnSoldier(shooter));
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(shooter, info);
	physics::Vec3 below = at + physics::Vec3 {-150, 0, 60};
	below.y = level.world->terrainHeight(below.x, below.z) + 0.5f;
	CHECK(level.world->setSoldierPosition(info.soldier, below));
	game::BotManager bots(*level.world, s.bots);
	level.afterTick = [&] { bots.update(1 / 30.0f); };
	const float most = level.world->hitPoints(gunship);
	// Up and on a run first.
	level.runUntil(40, [&] { return bots.flyState(pilot).rfind("hunt", 0) == 0; }, 0.1f);
	CHECK(bots.flyState(pilot).rfind("hunt", 0) == 0);
	level.run(3);
	level.world->applyDamage(gunship, most * 0.2f, shooter);
	bool evaded = false;
	level.runUntil(2, [&] { return evaded = evaded || bots.flyState(pilot) == "evade"; }, 1 / 30.0f);
	std::fprintf(stderr, "  hit for a fifth: %s\n", evaded ? "evaded" : bots.flyState(pilot).c_str());
	CHECK(evaded);
	level.run(4);
	CHECK(bots.flyState(pilot).rfind("hunt", 0) == 0); // back at it
	CHECK(!level.world->vehicleSupplies(gunship).empty()); // its Titan's deck point
	// Down to a quarter: off to be mended.
	level.world->applyDamage(gunship, level.world->hitPoints(gunship) - most * 0.25f, shooter);
	bool retreated = false, mended = false;
	const float ran = level.runUntil(90, [&] {
		retreated = retreated || bots.flyState(pilot).rfind("retreat", 0) == 0;
		mended = level.world->hitPoints(gunship) > most * 0.9f;
		return mended;
	}, 0.5f);
	std::fprintf(stderr, "  at a quarter: retreated %d, %.0f of %.0f hit points after %.0f s, then %s\n", retreated,
		level.world->hitPoints(gunship), most, ran, bots.flyState(pilot).c_str());
	CHECK(retreated);
	CHECK(mended);
	level.run(3);
	CHECK(bots.flyState(pilot).rfind("retreat", 0) != 0);
}

// enhanced.botPilots: with targets about (the enemy Titan's guns, a group of
// soldiers on the ground) a gunship flies attack runs, one after another,
// rather than hanging in the air: over two minutes at least five runs in,
// and under 15 % of its airborne time slower than 4 m/s (turning about at
// a run's end; a run in at a hub used to crawl to a stop under it, its
// nose held up by HeliAimAt).
TEST(Titan_GunshipFliesAttackRunsRatherThanHovering)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	TitanLevel level(gameDir, "Bridge_at_Remagen", 48, true, "mods/bf2142");
	CHECK(level.loaded);
	if (!level.loaded) return;
	Settings s(level);
	physics::Vec3 at = level.titan(2)->position + physics::Vec3 {400, 0, 0};
	at.y = level.world->terrainHeight(at.x, at.z) + 3;
	const uint32_t gunship = place(level, "as_ag", 1, at, -90);
	const int pilot = level.world->addPlayer("Pilot", 1, true);
	CHECK(level.world->spawnSoldier(pilot));
	CHECK(seat(level, pilot, gunship));
	for (int i = 0; i < 3; ++i) {
		const int e = level.world->addPlayer("Enemy" + std::to_string(i), 2, false);
		CHECK(level.world->spawnSoldier(e));
		game::GameWorld::PlayerInfo info;
		level.world->playerInfo(e, info);
		physics::Vec3 p = at + physics::Vec3 {-100.0f + 4 * i, 0, 250};
		p.y = level.world->terrainHeight(p.x, p.z) + 0.5f;
		level.world->setSoldierPosition(info.soldier, p);
	}
	game::BotManager bots(*level.world, s.bots);
	int runs = 0, airborne = 0, slow = 0;
	std::map<std::string, int> slowBy;
	std::string last;
	level.afterTick = [&] {
		bots.update(1 / 30.0f);
		const std::string state = bots.flyState(pilot);
		runs += state == "hunt in" && last != "hunt in";
		last = state;
		for (const auto& v : level.world->vehicleList()) {
			if (v.object == gunship && v.position.y - level.world->terrainHeight(v.position.x, v.position.z) > 10) {
				++airborne;
				slow += flat(v.velocity) < 4;
				if (flat(v.velocity) < 4) ++slowBy[state];
			}
		}
	};
	level.run(120);
	std::fprintf(stderr, "  %d runs in, %.0f s airborne, %.0f s of it under 4 m/s\n", runs, airborne / 30.0f, slow / 30.0f);
	for (const auto& [state, n] : slowBy) std::fprintf(stderr, "    slow in %s: %.1f s\n", state.c_str(), n / 30.0f);
	CHECK(runs >= 5);
	CHECK(airborne > 30 * 60);
	CHECK(slow * 100 < airborne * 15);
}

// enhanced.botThreat: two enemy Titan guns manned, the hunting gunship pilot
// on one of them; the other gunner kills four soldiers, and within ten
// seconds the pilot hunts his gun. With the switch off it keeps to its own.
TEST(Titan_BusyGunDrawsTheGunship)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	for (const bool weighs : {true, false}) {
		TitanLevel level(gameDir, "Bridge_at_Remagen", 48, true, "mods/bf2142");
		CHECK(level.loaded);
		if (!level.loaded) return;
		Settings s(level);
		const bool threat = weighs;
		s.bots.botThreat = &threat;
		const uint32_t enemyTitan = level.titan(2)->object;
		physics::Vec3 at = level.titan(2)->position + physics::Vec3 {400, 0, 0};
		at.y = level.world->terrainHeight(at.x, at.z) + 3;
		const uint32_t gunship = place(level, "as_ag", 1, at, -90);
		const int pilot = level.world->addPlayer("Pilot", 1, true);
		CHECK(level.world->spawnSoldier(pilot));
		CHECK(seat(level, pilot, gunship));
		std::vector<int> gunners, seats;
		for (int i = 0; i < 2; ++i) {
			const int g = level.world->addPlayer("Gunner" + std::to_string(i), 2, false);
			CHECK(level.world->spawnSoldier(g));
			const int seat = manGun(level, g, 2, seats);
			CHECK(seat >= 0);
			gunners.push_back(g);
			seats.push_back(seat);
		}
		game::BotManager bots(*level.world, s.bots);
		level.afterTick = [&] { bots.update(1 / 30.0f); };
		level.runUntil(60, [&] { return bots.huntedPart(pilot) != 0; }, 0.5f);
		const uint32_t first = bots.huntedPart(pilot);
		CHECK(first != 0);
		// The gunner whose hub isn't hunted (either, when neither is) kills four of the pilot's side.
		int busy = -1;
		uint32_t busyHub = 0;
		for (size_t i = 0; i < gunners.size(); ++i) {
			uint32_t hub = 0;
			physics::Vec3 where;
			bool wreck = false;
			if (busy < 0 && level.world->titanSeatHub(enemyTitan, seats[i], hub, where, wreck) && hub != first) {
				busy = gunners[i];
				busyHub = hub;
			}
		}
		CHECK(busy >= 0);
		for (int k = 0; k < 4; ++k) {
			const int victim = level.world->addPlayer("Victim" + std::to_string(k), 1, false);
			CHECK(level.world->spawnSoldier(victim));
			game::GameWorld::PlayerInfo v;
			level.world->playerInfo(victim, v);
			level.world->applyDamage(v.soldier, 1e6f, busy);
		}
		game::GameWorld::PlayerInfo b;
		level.world->playerInfo(busy, b);
		CHECK(b.kills == 4);
		bool switched = false;
		level.runUntil(10, [&] { return switched = bots.huntedPart(pilot) == busyHub; }, 0.2f);
		std::fprintf(stderr, "  threat %d: hunted %u first, the busy gun's hub %u %s\n", weighs, first, busyHub,
			switched ? "hunted within 10 s" : "not hunted");
		CHECK(switched == weighs);
	}
}

// enhanced.botTitanRepair: a bot respawning while its Titan has a gun down
// and no engineer aboard takes the kit with the repair tool, spawns aboard
// and mends the gun from its station inside. (Retail's SAI draws the kit
// at random and spawns by the areas' temperatures; its bots never board a
// Titan.)
TEST(Titan_EngineerRespawnsAboardToMendADownedGun)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	TitanLevel level(gameDir, "Bridge_at_Remagen", 48, true, "mods/bf2142");
	CHECK(level.loaded);
	if (!level.loaded) return;
	Settings s(level);
	s.bots.botTitanRepair = &s.on;
	game::BotManager bots(*level.world, s.bots);
	bots.setTeamKits(1, {{"Pac_Assault", "pac_light_soldier"}, {"Pac_Anti-Vehicle", "pac_light_soldier"},
							{"Pac_Recon", "pac_light_soldier"}, {"Pac_Support", "pac_light_soldier"}});
	level.afterTick = [&] { bots.update(1 / 30.0f); };
	uint32_t gun = 0;
	for (const auto& part : level.world->titanParts(1)) {
		gun = !gun && part.kind == game::GameWorld::TitanPartInfo::Gun ? part.id : gun;
	}
	CHECK(gun != 0);
	level.world->applyDamage(gun, 1e6f);
	CHECK(level.world->hitPoints(gun) <= 0);
	const int id = level.world->addPlayer("Bot", 1, true);
	level.world->setKit(id, "Pac_Assault", "pac_light_soldier");
	CHECK(level.world->spawnSoldier(id));
	level.run(0.5f);
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(id, info);
	level.world->setHitPoints(info.soldier, 0);
	level.run(0.5f);
	level.world->playerInfo(id, info);
	CHECK(!info.alive);
	std::fprintf(stderr, "  respawn: kit %d, spawn choice %d\n", bots.respawnKit(id), level.world->spawnChoice(id));
	CHECK(bots.respawnKit(id) == 1);
	CHECK(level.world->spawnChoice(id) == game::GameWorld::kSpawnOnTitan);
	CHECK(level.world->spawnSoldier(id));
	level.world->playerInfo(id, info);
	CHECK(flat(info.position - level.titan(1)->position) < 160); // aboard
	bool repairing = false;
	const float ran = level.runUntil(150, [&] {
		repairing = repairing || bots.behaviour(id) == "TitanRepair";
		return level.world->hitPoints(gun) > 0;
	}, 0.5f);
	std::fprintf(stderr, "  the gun back at %.0f hit points after %.0f s (repairing %d)\n", level.world->hitPoints(gun), ran, repairing);
	CHECK(repairing);
	CHECK(level.world->hitPoints(gun) > 0);
}

// enhanced.botTitanRepair: a bot manning a Titan gun that is wrecked under
// it gets out (the gun fires nothing until mended) and, an engineer, mends
// it.
TEST(Titan_GunnerLeavesAWreckedGunAndMendsIt)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	TitanLevel level(gameDir, "Bridge_at_Remagen", 48, true, "mods/bf2142");
	CHECK(level.loaded);
	if (!level.loaded) return;
	Settings s(level);
	s.bots.botTitanRepair = &s.on;
	game::BotManager bots(*level.world, s.bots);
	level.afterTick = [&] { bots.update(1 / 30.0f); };
	const uint32_t titan = level.titan(1)->object;
	const int id = level.world->addPlayer("Engineer", 1, true);
	level.world->setKit(id, "Pac_Anti-Vehicle", "pac_light_soldier");
	CHECK(level.world->spawnSoldier(id));
	game::GameWorld::PlayerInfo info;
	level.world->playerInfo(id, info);
	CHECK(level.world->setSoldierPosition(info.soldier, level.world->titanToWorld(titan, {0, 11.5f, -40})));
	level.runUntil(60, [&] { return level.world->playerInfo(id, info) && info.vehicle == titan; }, 0.5f);
	CHECK(info.vehicle == titan);
	if (info.vehicle != titan) return;
	uint32_t hub = 0;
	physics::Vec3 at;
	bool wreck = false;
	CHECK(level.world->titanSeatHub(titan, info.seat, hub, at, wreck) && !wreck);
	level.world->applyDamage(hub, 1e6f);
	CHECK(level.world->hitPoints(hub) <= 0);
	level.runUntil(3, [&] { return level.world->playerInfo(id, info) && info.vehicle != titan; }, 0.1f);
	std::fprintf(stderr, "  out of the wrecked gun: %d\n", info.vehicle != titan);
	CHECK(info.vehicle != titan);
	const float ran = level.runUntil(150, [&] { return level.world->hitPoints(hub) > 0; }, 0.5f);
	std::fprintf(stderr, "  mended after %.0f s: %.0f hit points\n", ran, level.world->hitPoints(hub));
	CHECK(level.world->hitPoints(hub) > 0);
}
