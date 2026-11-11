// Round simulation: control points and object spawners.
#include "Console/Console.h"
#include "Game/Bots.h"
#include "Animation/FirstPerson.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Physics/CollisionTemplateManager.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/WorldConsole.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace dice::hfe;

namespace {

struct Level {
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;

	Level()
	{
		templates.registerType(1, "ControlPoint");
		templates.registerType(2, "ObjectSpawner");
		templates.registerType(3, "PlayerControlObject");
		templates.create("PlayerControlObject", "tank_us");
		templates.create("PlayerControlObject", "tank_pac");
		world::ObjectTemplate* point = templates.create("ControlPoint", "cp_harbor");
		point->settings()["controlPointId"] = "7";
		point->settings()["team"] = "2";
		world::ObjectTemplate* spawner = templates.create("ObjectSpawner", "spawner_tank");
		spawner->settings()["controlPointId"] = "7";
		spawner->settings()["objectTemplate.team1"] = "tank_pac";
		spawner->settings()["objectTemplate.team2"] = "tank_us";
		spawner->settings()["minSpawnDelay"] = "10";
		spawner->settings()["maxSpawnDelay"] = "20";
		objects.create(point, {100, 0, 100});
		objects.create(spawner, {10, 5, 20}, {90, 0, 0});
	}

	size_t count(const char* name) const
	{
		size_t n = 0;
		for (const auto& object : objects.objects()) {
			n += object->objectTemplate()->name() == name;
		}
		return n;
	}
};

} // namespace

TEST(GameWorld_SpawnerSpawnsForItsControlPointsTeam)
{
	Level level;
	game::GameWorld world(level.templates, level.objects);
	world.startRound();
	CHECK(world.controlPoints().size() == 1);
	CHECK(world.spawners().size() == 1);
	CHECK(world.spawners()[0]->team() == 2);

	// Spawns on the first tick, at the spawner's place and heading.
	world.tick(1 / 30.0f);
	CHECK(level.count("tank_us") == 1);
	const world::Object* tank = level.objects.objects().back().get();
	CHECK(tank->position().x == 10 && tank->position().y == 5 && tank->position().z == 20);
	CHECK(tank->rotation().x == 90);
	// The next delay is drawn between the two spawn delays.
	const float delay = world.spawners()[0]->spawnCountdown();
	CHECK(delay >= 10 && delay <= 20);

	// The slot stays taken while the tank exists: no second tank.
	for (int i = 0; i < 30 * 30; ++i) {
		world.tick(1 / 30.0f);
	}
	CHECK(level.count("tank_us") == 1);
}

TEST(GameWorld_DestroyedObjectsRespawnAfterTheDelay)
{
	Level level;
	game::GameWorld world(level.templates, level.objects);
	world.startRound();
	world.tick(1 / 30.0f);
	world::Object* tank = level.objects.objects().back().get();
	const uint32_t first = tank->id();
	// Destroyed on the spot: the wreck is cleared once the delay is over.
	tank->setDestroyed(true);
	int ticks = 0;
	while (level.count("tank_us") == 1 && level.objects.find(first) && ticks < 30 * 30) {
		world.tick(1 / 30.0f);
		++ticks;
	}
	CHECK(ticks >= 10 * 30 - 2 && ticks <= 20 * 30 + 2);
	CHECK(level.objects.find(first) == nullptr);
	CHECK(level.count("tank_us") == 1);

	// Capturing the point switches what the spawner makes; the existing tank stays.
	world.setControlPointTeam(7, 1);
	CHECK(world.spawners()[0]->team() == 1);
	level.objects.objects().back()->setDestroyed(true);
	for (int i = 0; i < 25 * 30; ++i) {
		world.tick(1 / 30.0f);
	}
	CHECK(level.count("tank_pac") == 1);
}

// The countdown's reset of a hosted round (GameLogic::reset 0x004ce910):
// what the round made goes, the control points return to the level's
// teams and the spawners to their start, so the vehicles spawn anew.
TEST(GameWorld_ResetRoundRespawnsTheVehicles)
{
	Level level;
	game::GameWorld world(level.templates, level.objects);
	world.startRound();
	world.tick(1 / 30.0f);
	CHECK(level.count("tank_us") == 1);
	const uint32_t first = level.objects.objects().back()->id();
	// The point taken: the spawner's next tank would be PAC's.
	world.setControlPointTeam(7, 1);
	CHECK(world.spawners()[0]->team() == 1);
	world.resetRound();
	CHECK(level.objects.find(first) == nullptr);
	CHECK(world.controlPoints()[0].team == 2 && world.controlPoints()[0].object->team() == 2);
	CHECK(world.spawners()[0]->team() == 2);
	world.tick(1 / 30.0f);
	CHECK(level.count("tank_us") == 1 && level.count("tank_pac") == 0);
	CHECK(level.objects.objects().back()->id() != first);
}

// Two control points 200 m apart with a spawn point each, 20 m apart:
// EU's faces PAC's, PAC's looks sideways. Flat ground, rifles that kill in three hits.
struct Range {
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	io::FileManager files;
	physics::CollisionTemplateManager collisionTemplates;
	std::unique_ptr<game::GameWorld> world;
	std::vector<std::string> events;

	Range()
	{
		templates.registerType(1, "ControlPoint");
		templates.registerType(2, "SpawnPoint");
		templates.registerType(3, "Soldier");
		templates.registerType(4, "GenericFireArm");
		templates.registerType(5, "GenericProjectile");
		for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
			templates.create("Soldier", name)->settings()["armor.maxHitPoints"] = "100";
		}
		for (const char* name : {"eu_ar_rifle", "as_ar_rifle"}) {
			world::ObjectTemplate* rifle = templates.create("GenericFireArm", name);
			rifle->settings()["projectileTemplate"] = "bullet";
			rifle->settings()["velocity"] = "300";
			rifle->settings()["fire.roundsPerMinute"] = "600";
			rifle->settings()["ammo.magSize"] = "30";
		}
		templates.create("GenericProjectile", "bullet")->settings()["damage"] = "40";
		for (int team = 1; team <= 2; ++team) {
			world::ObjectTemplate* point = templates.create("ControlPoint", team == 1 ? "cp_pac" : "cp_eu");
			point->settings()["controlPointId"] = std::to_string(team);
			point->settings()["team"] = std::to_string(team);
			point->settings()["unableToChangeTeam"] = "1";
			objects.create(point, {team == 1 ? 100.0f : -100.0f, 0, 0});
		}
		world::ObjectTemplate* spawn = templates.create("SpawnPoint", "sp");
		objects.create(spawn, {0, 0, 20}, {90, 0, 0})->setControlPointId(1);
		objects.create(spawn, {0, 0, 0}, {0, 0, 0})->setControlPointId(2);
		world = std::make_unique<game::GameWorld>(templates, objects);
		world->enablePhysics(files, collisionTemplates,
			{[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& normal) {
				 normal = {0, 1, 0};
				 return 0.0f;
			 }});
		world->setEventSink([this](const std::string& event, const game::ScriptArgs&) { events.push_back(event); });
		world->startRound();
	}
	long count(const char* event) const { return std::count(events.begin(), events.end(), event); }
};

TEST(GameWorld_SoldiersShootAndDie)
{
	Range range;
	const int bot = range.world->addPlayer("Bot", 1, true);
	CHECK(range.world->spawnSoldier(bot));
	CHECK(range.world->spawnPlayer(2) != nullptr);
	range.world->setSpawnTime(2);

	// Three hits of 40 kill the bot.
	game::SoldierInput fire;
	fire.fire = true;
	range.world->setPlayerInput(fire);
	game::GameWorld::PlayerInfo info;
	int tick = 0;
	for (; tick < 90 && range.world->playerInfo(bot, info) && info.alive; ++tick) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(range.world->playerInfo(bot, info) && !info.alive && info.deaths == 1);
	game::GameWorld::PlayerInfo shooter;
	CHECK(range.world->playerInfo(range.world->localPlayer(), shooter) && shooter.kills == 1 && shooter.alive);
	CHECK(range.count("PlayerKilled") == 1);
	CHECK(info.manDown && range.count("PlayerDeath") == 0); // down, still revivable
	CHECK(tick > 6 && tick < 30); // about 0.2 s: three rounds at 600 a minute, 20 m

	// It comes back after the spawn time, the body gone (PlayerDeath first).
	range.world->setPlayerInput({});
	for (int i = 0; i < 70; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(range.world->playerInfo(bot, info) && info.alive && info.soldier != 0 && !info.manDown);
	CHECK(range.count("PlayerDeath") == 1 && range.count("PlayerSpawn") == 3);
}

// Team damage on a soldier (TeamDamage.cpp: 0x006557a0, 0x004c0e40): a
// teammate's damage reaching teamDamageScoreLimit (50 %) of the victim's
// maximum while he stands (the second 40 of 100) raises
// PlayerTeamDamagePoint and sends 0/0xb {0, the victim, the attacker} to
// both, once. The HUD's answers (0x00759bf0 case 0xb): the victim says
// AUTO_MOODGP_friendlyfire to the attacker; the attacker, the victim up,
// has the victim's voice say it; else the personal line. An enemy's
// damage is none.
TEST(GameWorld_TeamDamageOnASoldier)
{
	for (const int team : {2, 1}) {
		Range range;
		const int bot = range.world->addPlayer("Bot", team, true);
		CHECK(range.world->spawnSoldier(bot));
		CHECK(range.world->spawnPlayer(2) != nullptr);
		const int me = range.world->localPlayer();
		game::GameWorld::PlayerInfo info;
		range.world->playerInfo(bot, info);
		CHECK(range.world->setSoldierPosition(info.soldier, {0, 0, 20})); // in the line of fire
		game::SoldierInput fire;
		fire.fire = true;
		range.world->setPlayerInput(fire);
		std::vector<game::GameWorld::TeamDamage> mine;
		for (int tick = 0; tick < 90 && range.world->playerInfo(bot, info) && info.alive && !info.manDown && mine.empty(); ++tick) {
			range.world->tick(1 / 30.0f);
			mine = range.world->takeTeamDamage(me);
		}
		range.world->setPlayerInput({});
		std::fprintf(stderr, "  team %d: %.0f hit points left, %zu team damage\n", team, range.world->hitPoints(info.soldier), mine.size());
		if (team == 1) {
			CHECK(mine.empty() && range.count("PlayerTeamDamagePoint") == 0);
			continue;
		}
		CHECK(mine.size() == 1 && range.count("PlayerTeamDamagePoint") == 1);
		CHECK(std::fabs(range.world->hitPoints(info.soldier) - 20) < 0.5f);
		if (mine.size() != 1) {
			continue;
		}
		CHECK(mine[0].kind == 0 && mine[0].victim == bot && mine[0].attacker == me && mine[0].object == info.soldier);
		const auto theirs = range.world->takeTeamDamage(bot);
		CHECK(theirs.size() == 1 && theirs[0].attacker == me);
		CHECK(range.world->takeTeamDamage(me).empty() && range.world->takeTeamDamage(bot).empty());
		using Notice = game::GameWorld::TeamDamageNotice;
		CHECK(game::GameWorld::teamDamageNotice(mine[0], bot, true) == Notice::SayFriendlyFire);
		CHECK(game::GameWorld::teamDamageNotice(mine[0], me, true) == Notice::VictimSaysFriendlyFire);
		CHECK(game::GameWorld::teamDamageNotice(mine[0], me, false) == Notice::PlayerLine);
		game::GameWorld::TeamDamage vehicle = mine[0];
		vehicle.kind = 1;
		vehicle.victim = -1;
		CHECK(game::GameWorld::teamDamageNotice(vehicle, me, false) == Notice::VehicleLine);
	}
}

TEST(Bots_FightEachOther)
{
	// A bot of each team, 20 m apart: the EU one faces the PAC one, which
	// looks sideways and so does not see it (the view cone).
	Range range;
	Console console;
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	std::string out;
	for (const char* line : {"aiSettings.setBotSkill 0.8", "aiSettings.createBehaviourModifiers StandardWeights",
			 "aiSettings.setBehaviourModifier Idle 0.1", "aiSettings.setBehaviourModifier MoveTo 1.0",
			 "aiSettings.setBehaviourModifier Fire 7.5"}) {
		console.execute(line, out);
	}
	CHECK(settings.skill == 0.8f && settings.modifier("Fire") == 7.5f && settings.modifier("Idle") == 0.1f);
	game::BotManager bots(*range.world, settings);
	const int pac = range.world->addPlayer("PacBot", 1, true);
	const int eu = range.world->addPlayer("EuBot", 2, true);
	CHECK(range.world->spawnSoldier(pac) && range.world->spawnSoldier(eu));
	range.world->autoRespawn = false;
	// A second of bot frames: each runs its plan on about half of them and
	// thinks after 0.3 s of them (AIDLL 0x100c91c0, 0x100c95f0).
	for (int i = 0; i < 30; ++i) {
		bots.update(1 / 30.0f);
	}
	CHECK(bots.behaviour(pac) == "MoveTo" && bots.target(pac) == -1);
	CHECK(bots.behaviour(eu) == "Fire" && bots.target(eu) == pac);
	int tick = 0;
	for (; tick < 30 * 10 && range.count("PlayerKilled") == 0; ++tick) {
		bots.update(1 / 30.0f);
		range.world->tick(1 / 30.0f);
	}
	CHECK(range.count("PlayerKilled") == 1);
	game::GameWorld::PlayerInfo a, b;
	range.world->playerInfo(pac, a);
	range.world->playerInfo(eu, b);
	CHECK(!a.alive && b.alive && b.kills == 1);
	// At skill 0.8: a reaction time of 0.3 to 0.5 s and the crouch's 0.8 s
	// (AIDLL 0x100777a0, 0x1008f1e0), then rounds within +-1.4 degrees.
	CHECK(tick < 30 * 3);
	// With nobody left to shoot it heads for the enemy's control point (at +x).
	for (int i = 0; i < 30; ++i) {
		bots.update(1 / 30.0f);
	}
	CHECK(bots.behaviour(eu) == "MoveTo" && bots.target(eu) == -1);
	for (int i = 0; i < 60; ++i) {
		bots.update(1 / 30.0f);
		range.world->tick(1 / 30.0f);
	}
	range.world->playerInfo(eu, b);
	CHECK(b.position.x > 3);
}

TEST(GameWorld_VehicleSeatsFromTheTemplateTree)
{
	// As eu_fav: the root seats the driver, a child PlayerControlObject the
	// gunner, whose bundles aim a GenericFireArm; entry points ring it.
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "PlayerControlObject");
	templates.registerType(2, "RotationalBundle");
	templates.registerType(3, "GenericFireArm");
	templates.registerType(4, "EntryPoint");
	world::ObjectTemplate* car = templates.create("PlayerControlObject", "car");
	car->settings()["seatInformation"] = "car 0/0.5/1 0/0/0";
	car->settings()["soldierExitLocation"] = "-2/0/0 0/0/0";
	car->children().push_back({"car_entry", {0, 0, 1.25f}, {}});
	car->children().push_back({"car_second", {0, 0, -0.5f}, {}});
	templates.create("EntryPoint", "car_entry")->settings()["entryRadius"] = "3";
	world::ObjectTemplate* second = templates.create("PlayerControlObject", "car_second");
	second->settings()["seatInformation"] = "car_second 0.25/0.5/0.75 -90/0/0";
	second->children().push_back({"car_base", {0, 0.8f, 1.6f}, {}});
	world::ObjectTemplate* base = templates.create("RotationalBundle", "car_base");
	base->settings()["inputToYaw"] = "PIMouseLookX";
	base->children().push_back({"car_gun", {0, 0.1f, -0.1f}, {}});
	world::ObjectTemplate* gun = templates.create("RotationalBundle", "car_gun");
	gun->settings()["inputToPitch"] = "PIMouseLookY";
	gun->settings()["minRotation"] = "0/-30/0";
	gun->settings()["maxRotation"] = "0/15/0";
	gun->children().push_back({"car_firearm", {0, 0.125f, 0}, {}});
	templates.create("GenericFireArm", "car_firearm");
	const uint32_t id = objects.create(car, {10, 0, 10})->id();

	game::GameWorld world(templates, objects);
	const auto seats = world.vehicleSeats(id);
	CHECK(seats.size() == 2);
	if (seats.size() == 2) {
		CHECK(seats[0].name == "car" && seats[0].weapon.empty() && seats[0].hasExit && seats[0].exit.x == -2);
		CHECK(std::abs(seats[0].position.y - 0.5f) < 1e-6f && std::abs(seats[0].position.z - 1) < 1e-6f);
		CHECK(seats[1].name == "car_second" && std::abs(seats[1].position.z - 0.25f) < 1e-6f);
		CHECK(seats[1].weapon == "car_firearm" && seats[1].yawBundle == "car_base" && seats[1].pitchBundle == "car_gun");
		CHECK(std::abs(seats[1].weaponPosition.y - 1.025f) < 1e-5f && std::abs(seats[1].weaponPosition.z - 1.0f) < 1e-5f);
		CHECK(seats[1].minPitch == -30 && seats[1].maxPitch == 15);
	}
	CHECK(world.vehicleOccupants(id).empty()); // no rigid body: nobody can sit
}

TEST(GameWorld_SpawnChoice)
{
	Range range;
	CHECK(range.world->spawnablePoints(1) == std::vector<int> {1});
	CHECK(range.world->spawnablePoints(2) == std::vector<int> {2});
	const int bot = range.world->addPlayer("Bot", 2, true);
	// Choosing the other team's point falls back to the team's own.
	range.world->setSpawnChoice(bot, 1);
	CHECK(range.world->spawnChoice(bot) == 1);
	CHECK(range.world->spawnSoldier(bot));
	game::GameWorld::PlayerInfo info;
	range.world->playerInfo(bot, info);
	CHECK(info.alive && std::abs(info.position.z) < 0.5f); // EU's spawn point at the origin
}

TEST(GameWorld_KitsGiveTheirPrimaryWeapon)
{
	Range range;
	range.templates.registerType(6, "Kit");
	world::ObjectTemplate* sniper = range.templates.create("GenericFireArm", "eu_sni");
	sniper->settings()["itemIndex"] = "3";
	sniper->settings()["projectileTemplate"] = "bullet";
	range.templates.create("GenericFireArm", "knife")->settings()["itemIndex"] = "1";
	world::ObjectTemplate* recon = range.templates.create("Kit", "EU_Recon");
	recon->children().push_back({"knife", {}, {}});
	recon->children().push_back({"eu_sni", {}, {}});
	const int bot = range.world->addPlayer("Bot", 2, true);
	range.world->setKit(bot, "EU_Recon", "us_light_soldier");
	CHECK(range.world->spawnSoldier(bot));
	game::GameWorld::PlayerInfo info;
	range.world->playerInfo(bot, info);
	const world::Object* kit = range.objects.find(info.kit);
	const world::Object* weapon = range.objects.find(info.weapon);
	CHECK(kit && kit->objectTemplate()->name() == "EU_Recon");
	CHECK(weapon && weapon->objectTemplate()->name() == "eu_sni");
}

TEST(GameWorld_ExplosionsHurtAround)
{
	// A round without damage of its own but a 3 m blast, fired into the
	// ground a metre short of the PAC bot (20 m away): the blast kills it.
	Range range;
	world::ObjectTemplate* grenade = range.templates.find("bullet");
	grenade->settings()["damage"] = "0";
	grenade->settings()["detonation.explosionDamage"] = "200";
	grenade->settings()["detonation.explosionRadius"] = "3";
	grenade->settings()["detonation.explosionMaterial"] = "69";
	const int bot = range.world->addPlayer("Bot", 1, true);
	CHECK(range.world->spawnSoldier(bot));
	CHECK(range.world->spawnPlayer(2) != nullptr);
	game::SoldierInput fire;
	fire.fire = true;
	fire.aimPitch = -std::atan(1.6f / 19.0f) * 57.29578f;
	range.world->setPlayerInput(fire);
	range.world->tick(1 / 30.0f);
	range.world->setPlayerInput({});
	for (int i = 0; i < 30; ++i) {
		range.world->tick(1 / 30.0f);
	}
	game::GameWorld::PlayerInfo info;
	range.world->playerInfo(bot, info);
	CHECK(!info.alive && info.deaths == 1);
	CHECK(!range.world->impacts().empty() && range.world->impacts().back().object == 0); // it hit the ground
}

TEST(GameWorld_BlastsFallOffFromTheSoldiersOrigin)
{
	// A bare detonation (DefaultDetonationComp) with 100 damage over 10 m:
	// measured to the soldier's physics origin (a metre over its feet), the
	// damage is full within 2 m and half at 6 m (0x005a8900, 0x005a61a0);
	// all three bones in sight (0x005a7530). With the default material -1
	// it does nothing; an EMP hurts no one.
	Range range;
	world::ObjectTemplate* blast = range.templates.create("GenericProjectile", "blast");
	blast->components().push_back("DefaultDetonationComp");
	blast->settings()["detonation.explosionDamage"] = "100";
	blast->settings()["detonation.explosionMaterial"] = "69";
	world::ObjectTemplate* dud = range.templates.create("GenericProjectile", "dud");
	dud->components().push_back("DefaultDetonationComp");
	world::ObjectTemplate* emp = range.templates.create("GenericProjectile", "emp");
	emp->components().push_back("DefaultDetonationComp");
	emp->settings()["detonation.isEMPWeapon"] = "1";
	emp->settings()["detonation.explosionMaterial"] = "0";
	const int bot = range.world->addPlayer("Bot", 1, true);
	CHECK(range.world->spawnSoldier(bot));
	game::GameWorld::PlayerInfo info;
	range.world->playerInfo(bot, info);
	const physics::Vec3 origin = info.position + physics::Vec3 {0, 1, 0};
	CHECK(range.world->detonate("dud", origin) && range.world->hitPoints(info.soldier) == 100);
	CHECK(range.world->detonate("emp", origin) && range.world->hitPoints(info.soldier) == 100);
	CHECK(!range.world->detonate("bullet", origin));
	CHECK(range.world->detonate("blast", origin + physics::Vec3 {6, 0, 0}));
	CHECK(std::fabs(range.world->hitPoints(info.soldier) - 50) < 0.01f);
	CHECK(range.world->detonate("blast", origin + physics::Vec3 {0, 0, 11})); // out of reach
	CHECK(std::fabs(range.world->hitPoints(info.soldier) - 50) < 0.01f);
	CHECK(range.world->detonate("blast", origin + physics::Vec3 {0, 0, 1.5f}));
	CHECK(range.world->playerInfo(bot, info) && !info.alive);
}

TEST(GameWorld_WeaponSwitching)
{
	// A kit with a knife (slot 1), a handgun (2) and the rifle (3): the
	// soldier holds the rifle, switches to the handgun and back, and each
	// keeps its own magazine.
	Range range;
	range.templates.registerType(6, "Kit");
	world::ObjectTemplate* handgun = range.templates.create("GenericFireArm", "eu_handgun");
	handgun->settings()["itemIndex"] = "2";
	handgun->settings()["projectileTemplate"] = "bullet";
	handgun->settings()["ammo.magSize"] = "12";
	handgun->settings()["fire.roundsPerMinute"] = "300";
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	range.templates.create("GenericFireArm", "knife")->settings()["itemIndex"] = "1";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	for (const char* item : {"knife", "eu_handgun", "eu_ar_rifle"}) {
		kit->children().push_back({item, {}, {}});
	}
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const auto held = [&] { return range.objects.find(range.world->playerWeaponObject())->objectTemplate()->name(); };
	CHECK(held() == "eu_ar_rifle" && range.world->playerWeapon()->roundsInMag() == 30);
	// Fire two rounds of the rifle, then take the handgun.
	game::SoldierInput input;
	input.fire = true;
	input.aimPitch = 30; // over the bot's head: nothing to hit
	for (int i = 0; i < 4; ++i) {
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
	}
	const int rifleRounds = range.world->playerWeapon()->roundsInMag();
	CHECK(rifleRounds < 30);
	input = {};
	input.selectItem = 2;
	range.world->setPlayerInput(input);
	range.world->tick(1 / 30.0f);
	CHECK(held() == "eu_handgun" && range.world->playerWeapon()->roundsInMag() == 12);
	input.selectItem = 0;
	range.world->setPlayerInput(input);
	range.world->tick(1 / 30.0f);
	input.selectItem = 3;
	range.world->setPlayerInput(input);
	range.world->tick(1 / 30.0f);
	CHECK(held() == "eu_ar_rifle" && range.world->playerWeapon()->roundsInMag() == rifleRounds);
}

TEST(GameWorld_GadgetsHealAndResupply)
{
	// unl_hub_medic: thrown out (slot 4), it heals the team round it, 3 per
	// cent of their maximum a second within 4 m (0x005d8ac0 -> 0x00655a00);
	// a second one replaces the first. An ammo hub hands out magazines,
	// strength per cent of one a second (0x005f5110).
	Range range;
	range.templates.registerType(6, "Kit");
	for (const char* name : {"unl_hub_medic", "unl_hub_ammo"}) {
		world::ObjectTemplate* hub = range.templates.create("GenericFireArm", name);
		const bool ammo = std::string(name) == "unl_hub_ammo";
		hub->settings()["itemIndex"] = ammo ? "5" : "4";
		hub->settings()["projectileTemplate"] = std::string(name) + "_stationary";
		hub->settings()["ammo.replenishingType"] = ammo ? "RTAmmo" : "RTHeal";
		hub->settings()["ammo.abilityRadius"] = "4";
		hub->settings()["ammo.abilityStrength"] = ammo ? "20" : "3";
		hub->settings()["ammo.magSize"] = "1";
		hub->settings()["fire.roundsPerMinute"] = "60";
		range.templates.create("GenericProjectile", std::string(name) + "_stationary")->settings()["armor.maxHitPoints"] = "100";
	}
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	range.templates.find("eu_ar_rifle")->settings()["ammo.nrOfMags"] = "5";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	for (const char* item : {"eu_ar_rifle", "unl_hub_medic", "unl_hub_ammo"}) {
		kit->children().push_back({item, {}, {}});
	}
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const uint32_t soldier = range.world->playerObject();
	range.world->setHitPoints(soldier, 40);
	const auto press = [&](game::SoldierInput input) {
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
		range.world->setPlayerInput({});
		range.world->tick(1 / 30.0f);
	};
	game::SoldierInput take;
	take.selectItem = 4;
	press(take);
	game::SoldierInput fire;
	fire.fire = true;
	press(fire);
	CHECK(range.world->supplyObjects().size() == 1);
	// The rifle in hand: only the thrown hub heals.
	game::SoldierInput rifle;
	rifle.selectItem = 3;
	press(rifle);
	const float before = range.world->hitPoints(soldier);
	for (int i = 0; i < 60; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(std::abs(range.world->hitPoints(soldier) - (before + 6)) < 0.5f); // 2 s at 3 a second
	// The hub in hand heals round its holder as well (0x005d8740): 6 a
	// second with the thrown one.
	press(take);
	const float holding = range.world->hitPoints(soldier);
	for (int i = 0; i < 30; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(std::abs(range.world->hitPoints(soldier) - (holding + 6)) < 0.5f);
	// Another throw replaces the hub.
	for (int i = 0; i < 40; ++i) {
		range.world->tick(1 / 30.0f);
	}
	press(fire);
	CHECK(range.world->supplyObjects().size() == 1);

	// Ammo: empty a magazine (into the air) and reload, then throw an ammo
	// hub (20 strength: a fifth of a magazine a second) to get it back.
	press(rifle);
	const int magsBefore = range.world->playerWeapon()->magsLeft();
	game::SoldierInput spray;
	spray.fire = true;
	spray.aimPitch = 45;
	range.world->setPlayerInput(spray);
	for (int i = 0; i < 30 * 6 && range.world->playerWeapon()->roundsInMag() > 0; ++i) {
		range.world->tick(1 / 30.0f);
	}
	range.world->setPlayerInput({});
	// The rifle has no ammo.autoReload: c_PIReload takes the next magazine.
	game::SoldierInput reload;
	reload.reload = true;
	press(reload);
	for (int i = 0; i < 30 * 3 && range.world->playerWeapon()->reloading(); ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(range.world->playerWeapon()->magsLeft() == magsBefore - 1);
	take.selectItem = 5;
	press(take);
	press(fire);
	CHECK(range.world->supplyObjects().size() == 2); // the medic hub stays
	press(rifle);
	for (int i = 0; i < 30 * 4; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(range.world->playerWeapon()->magsLeft() == magsBefore - 1); // 0.8 of it after 4 s
	for (int i = 0; i < 30 * 2; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(range.world->playerWeapon()->magsLeft() == magsBefore);
}

TEST(GameWorld_AbilityBattery)
{
	// The medic hub in hand heals only while its holder's battery holds
	// abilityDrain / 30 (0x005d8ac0), each target treated taking that much
	// (0x00586310); the battery starts empty with a fresh kit and fills by
	// the kit's abilityRestoreRate a second up to 1 (0x0058d350). An
	// unlock's increasedRepairAbility scales the strength (0x005d7a40).
	Range range;
	range.templates.registerType(6, "Kit");
	range.templates.registerType(7, "ItemContainer");
	world::ObjectTemplate* hub = range.templates.create("GenericFireArm", "unl_hub_medic");
	hub->settings()["itemIndex"] = "4";
	hub->settings()["ammo.replenishingType"] = "RTHeal";
	hub->settings()["ammo.abilityRadius"] = "4";
	hub->settings()["ammo.abilityStrength"] = "3";
	hub->settings()["ammo.abilityDrain"] = "0.5";
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	kit->settings()["abilityRestoreRate"] = "0.1";
	for (const char* item : {"eu_ar_rifle", "unl_hub_medic"}) {
		kit->children().push_back({item, {}, {}});
	}
	world::ObjectTemplate* unlock = range.templates.create("ItemContainer", "UnlockEU_AssaultMedic1");
	unlock->settings()["ability.increasedRepairAbility"] = "1.5";
	kit->children().push_back({"UnlockEU_AssaultMedic1", {}, {}});
	range.world->localPlayerUnlocks = {"UnlockEU_AssaultMedic1"};
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const int me = range.world->localPlayer();
	game::GameWorld::PlayerInfo info;
	CHECK(range.world->playerInfo(me, info) && info.battery == 0);
	const uint32_t soldier = range.world->playerObject();
	game::SoldierInput take;
	take.selectItem = 4;
	range.world->setPlayerInput(take);
	range.world->tick(1 / 30.0f);
	range.world->setPlayerInput({});
	const auto run = [&](float seconds) {
		for (int i = 0; i < static_cast<int>(seconds * 30 + 0.5f); ++i) {
			range.world->tick(1 / 30.0f);
		}
	};
	// Nobody to treat: the battery only fills, a tenth a second.
	run(1);
	CHECK(range.world->playerInfo(me, info) && std::abs(info.battery - 0.1f) < 0.01f);
	// Charged: 3 x 1.5 per cent a second, the battery down 0.5 - 0.1 a second.
	range.world->setHitPoints(soldier, 40);
	range.world->setBattery(me, 0.5f);
	run(1);
	CHECK(std::abs(range.world->hitPoints(soldier) - 44.5f) < 0.3f);
	CHECK(range.world->playerInfo(me, info) && std::abs(info.battery - 0.1f) < 0.02f);
	// Flat: a tick's treatment only now and then, as the battery creeps
	// over 0.5 / 30 (a fifth of the time).
	range.world->setBattery(me, 0);
	const float flat = range.world->hitPoints(soldier);
	run(1);
	CHECK(range.world->hitPoints(soldier) - flat > 0.5f && range.world->hitPoints(soldier) - flat < 1.3f);
}

TEST(GameWorld_AbilityBarTargetsAndGifts)
{
	// The ability bar's inputs (0x007c3af0): the medic hub in hand treats
	// its holder (a soldier of his team): the target's armour share, the
	// comp working, the battery; the heal stamps the giver (0x004bc410:
	// slot 0x258 (tick, 1, 0)).
	Range range;
	range.templates.registerType(6, "Kit");
	world::ObjectTemplate* hub = range.templates.create("GenericFireArm", "unl_hub_medic");
	hub->settings()["itemIndex"] = "4";
	hub->settings()["ammo.replenishingType"] = "RTHeal";
	hub->settings()["ammo.abilityRadius"] = "4";
	hub->settings()["ammo.abilityStrength"] = "3";
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	kit->components().push_back("Ability");
	kit->settings()["ability.hasHealingAbility"] = "1";
	for (const char* item : {"eu_ar_rifle", "unl_hub_medic"}) {
		kit->children().push_back({item, {}, {}});
	}
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const int me = range.world->localPlayer();
	const uint32_t soldier = range.world->playerObject();
	game::GameWorld::AbilityBarInfo info;
	CHECK(range.world->abilityBar(me, info) && info.alive && info.kitAbility && info.kitHealing && !info.kitAmmo);
	CHECK(info.replenishingType == 3 && !info.healFlag); // the rifle
	range.world->setHitPoints(soldier, 40);
	game::SoldierInput take;
	take.selectItem = 4;
	range.world->setPlayerInput(take);
	range.world->tick(1 / 30.0f);
	range.world->setPlayerInput({});
	for (int i = 0; i < 10; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(range.world->abilityBar(me, info) && info.replenishingType == 0 && info.active);
	CHECK(info.targets.size() == 1 && info.targets[0] > 0.4f && info.targets[0] < 0.45f);
	CHECK(info.healFlag && !info.repairFlag && info.sinceHeal == 0);
	// Put away, the hub treats no more: the flag stays, the ticks run on.
	game::SoldierInput rifle;
	rifle.selectItem = 3;
	range.world->setPlayerInput(rifle);
	range.world->tick(1 / 30.0f);
	range.world->setPlayerInput({});
	for (int i = 0; i < 20; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(range.world->abilityBar(me, info) && info.replenishingType == 3 && info.targets.empty());
	CHECK(info.healFlag && info.sinceHeal >= 15);
}

TEST(GameWorld_GrenadesBounceAndGoOff)
{
	// A frag grenade from an unlock (slot 7): thrown at the PAC bot 20 m
	// away, it lands, rolls to a stop, and its 9 m blast goes off after
	// the 2.8 s fuse.
	Range range;
	range.templates.registerType(6, "Kit");
	range.templates.registerType(7, "ItemContainer");
	world::ObjectTemplate* frag = range.templates.create("GenericFireArm", "unl_grenade_frag");
	frag->settings()["itemIndex"] = "7";
	frag->settings()["projectileTemplate"] = "frag_projectile";
	frag->settings()["velocity"] = "15";
	frag->settings()["ammo.magSize"] = "1";
	world::ObjectTemplate* shell = range.templates.create("GenericProjectile", "frag_projectile");
	shell->settings()["physicsType"] = "RotationalPoint";
	shell->settings()["timeToLive"] = "CRD_NONE/2.8/0/0";
	shell->settings()["collision.bouncing"] = "1";
	shell->settings()["detonation.explosionDamage"] = "140";
	shell->settings()["detonation.explosionRadius"] = "9";
	shell->settings()["detonation.explosionMaterial"] = "69"; // Unl_Grenade_Frag's
	shell->settings()["gravityModifier"] = "0.55";
	world::ObjectTemplate* container = range.templates.create("ItemContainer", "UnlockEU_AssaultPlayer0");
	container->children().push_back({"unl_grenade_frag", {}, {}});
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"UnlockEU_AssaultPlayer0", {}, {}});
	const int bot = range.world->addPlayer("Bot", 1, true);
	CHECK(range.world->spawnSoldier(bot));
	range.world->unlockAllByDefault = true;
	CHECK(range.world->spawnPlayer(2) != nullptr);
	game::SoldierInput input;
	input.selectItem = 7;
	range.world->setPlayerInput(input);
	range.world->tick(1 / 30.0f);
	CHECK(range.objects.find(range.world->playerWeaponObject())->objectTemplate()->name() == "unl_grenade_frag");
	input = {};
	input.fire = true;
	input.aimPitch = 20;
	range.world->setPlayerInput(input);
	range.world->tick(1 / 30.0f);
	range.world->setPlayerInput({});
	game::GameWorld::PlayerInfo info;
	for (int i = 0; i < 30 * 2; ++i) {
		range.world->tick(1 / 30.0f);
	}
	range.world->playerInfo(bot, info);
	CHECK(info.alive && range.world->projectilesInFlight() == 1); // still fused after 2 s
	for (int i = 0; i < 30; ++i) {
		range.world->tick(1 / 30.0f);
	}
	range.world->playerInfo(bot, info);
	CHECK(!info.alive && range.world->projectilesInFlight() == 0);
	// Without the unlock the kit has no grenade.
	const int other = range.world->addPlayer("Other", 2, true);
	range.world->setUnlocks(other, {}, false);
	range.world->setKit(other, "EU_Assault", "us_light_soldier");
	CHECK(range.world->spawnSoldier(other));
}

namespace {

// A kit gadget (slot 5) throwing `projectile`, for the mine and C4 tests.
void giveGadget(Range& range, const char* name, world::ObjectTemplate*& projectile)
{
	range.templates.registerType(6, "Kit");
	world::ObjectTemplate* gadget = range.templates.create("GenericFireArm", name);
	gadget->settings()["itemIndex"] = "5";
	gadget->settings()["projectileTemplate"] = std::string(name) + "_projectile";
	gadget->settings()["velocity"] = "5";
	gadget->settings()["ammo.magSize"] = "2";
	projectile = range.templates.create("GenericProjectile", std::string(name) + "_projectile");
	projectile->settings()["physicsType"] = "Mesh";
	projectile->settings()["collision.bouncing"] = "1"; // the mines', C4's: they lie where they land
	projectile->settings()["timeToLive"] = "CRD_NONE/9999/0/0";
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({name, {}, {}});
}

void throwGadget(Range& range)
{
	game::SoldierInput input;
	input.selectItem = 5;
	range.world->setPlayerInput(input);
	range.world->tick(1 / 30.0f);
	input = {};
	input.fire = true;
	input.aimPitch = -30;
	range.world->setPlayerInput(input);
	range.world->tick(1 / 30.0f);
	range.world->setPlayerInput({});
	for (int i = 0; i < 30; ++i) {
		range.world->tick(1 / 30.0f);
	}
}

} // namespace

// The use key's pick-up (0x004bce30): a placed round of a carried
// pickupEnabled item, within 1.3 m and in front, goes and the item gets
// the round back; out of reach, nothing.
TEST(GameWorld_PlacedRoundsArePickedUp)
{
	Range range;
	world::ObjectTemplate* c4 = nullptr;
	giveGadget(range, "Unl_c4", c4);
	range.templates.find("Unl_c4")->settings()["pickupEnabled"] = "1";
	CHECK(range.world->spawnPlayer(2) != nullptr);
	throwGadget(range);
	CHECK(range.world->projectilesInFlight() == 1);
	const auto inMag = [&] {
		for (const auto& w : range.world->weaponAmmo(range.world->localPlayer())) {
			if (w.weapon == "Unl_c4") {
				return w.inMag;
			}
		}
		return -9;
	};
	CHECK(inMag() == 1);
	const physics::Vec3 at = range.world->projectiles().front().position;
	game::GameWorld::PlayerInfo me;
	range.world->playerInfo(range.world->localPlayer(), me);
	const auto use = [&] {
		game::SoldierInput press;
		press.use = true;
		range.world->setPlayerInput(press);
		range.world->tick(1 / 30.0f);
		range.world->setPlayerInput({});
		range.world->tick(1 / 30.0f);
	};
	const float y = me.yaw * 3.14159265f / 180.0f;
	const physics::Vec3 ahead {std::sin(y), 0, std::cos(y)};
	std::fprintf(stderr, "  c4 at %.2f %.2f %.2f (resting %d), soldier at %.2f %.2f %.2f, yaw %.1f\n", at.x, at.y, at.z,
		range.world->projectiles().front().resting, me.position.x, me.position.y, me.position.z, me.yaw);
	// 3 m short of it: nothing.
	range.world->setSoldierPosition(me.soldier, at - ahead * 3.0f);
	use();
	CHECK(range.world->projectilesInFlight() == 1);
	// 1 m short of it, facing it: taken back.
	range.world->setSoldierPosition(me.soldier, at - ahead * 1.0f);
	use();
	CHECK(range.world->projectilesInFlight() == 0);
	CHECK(inMag() == 2);
}

TEST(GameWorld_MinesAndC4)
{
	// An anti-personnel mine at the EU soldier's feet facing PAC's spawn:
	// a PAC soldier walking slowly past is spared, running sets it off.
	{
		Range range;
		world::ObjectTemplate* mine = nullptr;
		giveGadget(range, "unl_mine_antipersonnel", mine);
		mine->settings()["detonation.explosionDamage"] = "200";
		mine->settings()["detonation.explosionRadius"] = "8";
		mine->settings()["detonation.explosionMaterial"] = "50"; // Unl_Mine_AntiPersonnel's
		mine->settings()["detonation.explosionConeAngle"] = "45";
		mine->settings()["detonation.triggerRadius"] = "3";
		mine->settings()["detonation.triggerType"] = "MTYPco";
		mine->settings()["detonation.triggerAngle"] = "30";
		mine->settings()["detonation.triggerVictimMinSpeed"] = "3.6";
		CHECK(range.world->spawnPlayer(2) != nullptr);
		throwGadget(range);
		CHECK(range.world->projectilesInFlight() == 1);
		// The PAC bot, standing 2 m in front of the mine: still, it is spared.
		const int bot = range.world->addPlayer("Bot", 1, true);
		CHECK(range.world->spawnSoldier(bot));
		game::GameWorld::PlayerInfo info;
		range.world->playerInfo(bot, info);
		game::GameWorld::PlayerInfo me;
		range.world->playerInfo(range.world->localPlayer(), me);
		range.world->setSoldierPosition(info.soldier, me.position + physics::Vec3 {0, 0, 2.5f});
		range.world->tick(1 / 30.0f);
		range.world->playerInfo(bot, info);
		CHECK(info.alive && range.world->projectilesInFlight() == 1);
		// Running at it from 15 m out sets it off; the EU soldier behind it
		// is spared by the cone.
		range.world->setSoldierPosition(info.soldier, me.position + physics::Vec3 {0, 0, 15});
		game::SoldierInput run;
		run.throttle = 1;
		run.sprint = true;
		run.turn = 90; // from facing +x to facing the mine (-z)
		for (int i = 0; i < 150 && range.world->projectilesInFlight() > 0; ++i) {
			range.world->setInput(bot, run);
			run.turn = 0;
			range.world->tick(1 / 30.0f);
		}
		range.world->playerInfo(bot, info);
		range.world->playerInfo(range.world->localPlayer(), me);
		CHECK(!info.alive && me.alive && range.world->projectilesInFlight() == 0);
	}
	// C4 lies still until its owner sets it off.
	{
		Range range;
		world::ObjectTemplate* c4 = nullptr;
		giveGadget(range, "unl_c4", c4);
		c4->settings()["detonation.explosionDamage"] = "1000";
		c4->settings()["detonation.explosionMaterial"] = "80"; // Unl_c4's; no radius: 10 by default
		CHECK(range.world->spawnPlayer(2) != nullptr);
		throwGadget(range);
		for (int i = 0; i < 90; ++i) {
			range.world->tick(1 / 30.0f);
		}
		CHECK(range.world->projectilesInFlight() == 1);
		game::SoldierInput detonate;
		detonate.altFire = true;
		range.world->setPlayerInput(detonate);
		range.world->tick(1 / 30.0f);
		CHECK(range.world->projectilesInFlight() == 0);
		game::GameWorld::PlayerInfo me;
		range.world->playerInfo(range.world->localPlayer(), me);
		CHECK(!me.alive); // standing on it
	}
}

// The radius trigger (0x005e8aa0): the anti-personnel mine (MTYPco,
// allowFriendlyFireDetonation 1) spares its owner's bots always and his
// human teammates only with the server's friendlyFireWithMines off; an
// EMP mine (MTYVehicle) no soldier sets off.
TEST(GameWorld_MinesFriendlyFireAndTriggerTypes)
{
	const auto setUp = [](Range& range, const char* type) {
		world::ObjectTemplate* mine = nullptr;
		giveGadget(range, "unl_mine_antipersonnel", mine);
		mine->settings()["detonation.explosionDamage"] = "200";
		mine->settings()["detonation.explosionRadius"] = "8";
		mine->settings()["detonation.triggerRadius"] = "3";
		mine->settings()["detonation.triggerType"] = type;
		mine->settings()["detonation.triggerAngle"] = "30"; // its owner stands behind it
		mine->settings()["detonation.allowFriendlyFireDetonation"] = "1";
		CHECK(range.world->spawnPlayer(2) != nullptr);
		throwGadget(range);
		CHECK(range.world->projectilesInFlight() == 1);
	};
	// Someone runs from 15 m in front at the mine for up to 5 s: whether
	// it went off.
	const auto runAt = [](Range& range, int who) {
		CHECK(range.world->spawnSoldier(who));
		game::GameWorld::PlayerInfo info, me;
		range.world->playerInfo(who, info);
		range.world->playerInfo(range.world->localPlayer(), me);
		range.world->setSoldierPosition(info.soldier, me.position + physics::Vec3 {0, 0, 15});
		game::SoldierInput run;
		run.throttle = 1;
		run.sprint = true;
		run.turn = 180 - info.yaw; // to face -z, the mine
		for (int i = 0; i < 150 && range.world->projectilesInFlight() > 0; ++i) {
			range.world->setInput(who, run);
			run.turn = 0;
			range.world->tick(1 / 30.0f);
		}
		return range.world->projectilesInFlight() == 0;
	};
	{
		Range range;
		setUp(range, "MTYPco");
		CHECK(!runAt(range, range.world->addPlayer("Bot", 2, true))); // a bot of the side: spared
	}
	{
		Range range;
		setUp(range, "MTYPco");
		CHECK(runAt(range, range.world->addPlayer("Mate", 2, false))); // a human: friendlyFireWithMines on
	}
	{
		Range range;
		range.world->setFriendlyFireWithMines(false);
		setUp(range, "MTYPco");
		CHECK(!runAt(range, range.world->addPlayer("Mate", 2, false)));
		CHECK(runAt(range, range.world->addPlayer("Enemy", 1, true)));
	}
	{
		Range range;
		setUp(range, "MTYVehicle");
		CHECK(!runAt(range, range.world->addPlayer("Enemy", 1, true))); // soldiers don't
	}
}

// The booster's motion mine bait (Bp1_Motion_Mine_Bait_Stationary and its
// MagneticBait child, a TTMagnet TargetObject): a magnet seeker takes the
// bait as it takes a vehicle's MagneticObject; overrideArmingSpeed arms it
// though the bait lies still, so it flies at maxSpeed (0x005ecb30,
// 0x005b3e60) and goes off at the bait. With switchPlayerOnLock the lock
// makes the mine the bait thrower's (0x005b3e60 -> 0x0058e420).
TEST(GameWorld_MotionMineBait)
{
	const auto setUp = [](Range& range, bool switchOnLock) {
		range.templates.registerType(8, "TargetObject");
		world::ObjectTemplate* mine = range.templates.create("GenericProjectile", "magnet_mine");
		mine->settings()["seek.targetType"] = "TTMagnet";
		mine->settings()["seek.maxDistLock"] = "50";
		mine->settings()["armingDelay"] = "0.2";
		mine->settings()["acceleration"] = "60";
		mine->settings()["deAcceleration"] = "50";
		mine->settings()["maxSpeed"] = "15";
		mine->settings()["follow.useTargetSpeed"] = "1";
		mine->settings()["follow.speedModifier"] = "2";
		mine->settings()["follow.armingSpeed"] = "7.2";
		mine->settings()["follow.magneticBehaviour"] = "1";
		mine->settings()["gravityModifier"] = "0";
		mine->settings()["timeToLive"] = "CRD_NONE/9999/0/0";
		mine->settings()["detonation.explosionDamage"] = "10";
		mine->settings()["detonation.explosionRadius"] = "1";
		world::ObjectTemplate* target = range.templates.create("TargetObject", "MagneticBait");
		target->settings()["targetType"] = "TTMagnet";
		target->settings()["overrideArmingSpeed"] = "1";
		target->settings()["switchPlayerOnLock"] = switchOnLock ? "1" : "0";
		world::ObjectTemplate* bait = range.templates.create("GenericProjectile", "bait_stationary");
		bait->settings()["gravityModifier"] = "0";
		bait->settings()["timeToLive"] = "CRD_NONE/9999/0/0";
		bait->children().push_back({"MagneticBait", {}, {}});
		CHECK(range.world->spawnPlayer(2) != nullptr);
		const int bot = range.world->addPlayer("Bot", 1, true);
		CHECK(range.world->spawnSoldier(bot));
		CHECK(range.world->launchProjectile("magnet_mine", {0, 3, 0}, {0, 0, 0}, range.world->localPlayer()));
		CHECK(range.world->launchProjectile("bait_stationary", {0, 3, 10}, {0, 0, 0}, bot));
		return bot;
	};
	const auto mineOf = [](Range& range) -> const game::Projectile* {
		for (const auto& p : range.world->projectiles()) {
			if (p.alive && p.settings && p.settings->name == "magnet_mine") {
				return &p;
			}
		}
		return nullptr;
	};
	{
		Range range;
		setUp(range, false);
		bool locked = false;
		for (int i = 0; i < 90 && mineOf(range); ++i) {
			range.world->tick(1 / 30.0f);
			locked = locked || (mineOf(range) && mineOf(range)->lockedBait != 0);
		}
		CHECK(locked && !mineOf(range)); // it chased the still bait and went off there
	}
	{
		Range range;
		const int bot = setUp(range, true);
		for (int i = 0; i < 15; ++i) {
			range.world->tick(1 / 30.0f);
		}
		const game::Projectile* mine = mineOf(range);
		CHECK(mine && mine->player == bot && mine->team == 1);
	}
}

// keepProjectiles (0x005f1cc0): a mine whose owner left the game stays
// the gadget's keepProjectiles seconds more, then goes without going off.
TEST(GameWorld_KeepProjectilesAfterTheOwnerLeaves)
{
	Range range;
	world::ObjectTemplate* mine = nullptr;
	giveGadget(range, "unl_mine_emp", mine);
	mine->settings()["detonation.explosionDamage"] = "200";
	mine->settings()["detonation.triggerRadius"] = "3";
	range.templates.find("unl_mine_emp")->settings()["keepProjectiles"] = "2";
	CHECK(range.world->spawnPlayer(2) != nullptr);
	throwGadget(range);
	CHECK(range.world->projectilesInFlight() == 1);
	const int owner = range.world->localPlayer();
	CHECK(range.world->removePlayer(owner));
	for (int i = 0; i < 50; ++i) { // 1.7 s
		range.world->tick(1 / 30.0f);
	}
	CHECK(range.world->projectilesInFlight() == 1);
	for (int i = 0; i < 15; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(range.world->projectilesInFlight() == 0);
}

TEST(GameWorld_DefibrillatorRevives)
{
	// A downed EU bot lies 1.5 m ahead; the EU soldier's defibrillator
	// (damageMandownSoldiers, short range) brings it back, and the
	// scripts hear PlayerRevived instead of PlayerDeath.
	Range range;
	range.templates.registerType(6, "Kit");
	world::ObjectTemplate* defib = range.templates.create("GenericFireArm", "unl_defibrillator");
	defib->settings()["itemIndex"] = "5";
	defib->settings()["projectileTemplate"] = "shock";
	defib->settings()["velocity"] = "60";
	defib->settings()["ammo.magSize"] = "1"; // one shock, then 0.6 s to recharge
	defib->settings()["ammo.reloadTime"] = "0.6";
	defib->settings()["ammo.nrOfMags"] = "4";
	defib->settings()["ammo.autoReload"] = "1";
	world::ObjectTemplate* shock = range.templates.create("GenericProjectile", "shock");
	shock->settings()["damage"] = "1000";
	shock->settings()["damageMandownSoldiers"] = "1";
	shock->settings()["collision.resurrectDamage"] = "20000"; // its ResurrectCollisionComp
	shock->settings()["collision.restoreHP"] = "120";
	shock->settings()["timeToLive"] = "CRD_NONE/0.05/0/0";
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"unl_defibrillator", {}, {}});
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const int bot = range.world->addPlayer("Mate", 2, true);
	CHECK(range.world->spawnSoldier(bot));
	game::GameWorld::PlayerInfo info, me;
	range.world->playerInfo(range.world->localPlayer(), me);
	range.world->playerInfo(bot, info);
	range.world->setSoldierPosition(info.soldier, me.position + physics::Vec3 {0, 0, 1.0f});
	range.world->setSpawnTime(30);
	range.world->setHitPoints(info.soldier, 0);
	range.world->playerInfo(bot, info);
	CHECK(!info.alive && info.manDown);
	game::SoldierInput input;
	input.selectItem = 5;
	range.world->setPlayerInput(input);
	range.world->tick(1 / 30.0f);
	input = {};
	input.fire = true;
	input.aimPitch = -60; // down at the body
	range.world->setPlayerInput(input);
	for (int i = 0; i < 5; ++i) {
		range.world->tick(1 / 30.0f);
	}
	range.world->playerInfo(bot, info);
	CHECK(info.alive && !info.manDown && range.count("PlayerRevived") == 1 && range.count("PlayerDeath") == 0);
	// Raised to the shock's restoreHP (0x004c1960).
	CHECK(std::abs(range.world->hitPoints(info.soldier) - 100) < 1e-3f);
	// Shocked again standing (the paddles' hit, 0x004ba5c0, goes on only
	// for a teammate on a downed body): no harm, friendly fire or not.
	game::GameWorld::FriendlyFire friendlyFire;
	friendlyFire.soldier = friendlyFire.vehicle = friendlyFire.soldierSplash = friendlyFire.vehicleSplash = 100;
	range.world->setFriendlyFire(friendlyFire);
	range.world->setPlayerInput({});
	for (int i = 0; i < 25; ++i) {
		range.world->tick(1 / 30.0f);
	}
	for (int aim : {-60, -30, -10}) {
		input = {};
		input.fire = true;
		input.aimPitch = static_cast<float>(aim);
		range.world->setPlayerInput(input);
		for (int i = 0; i < 3; ++i) {
			range.world->tick(1 / 30.0f);
		}
		range.world->setPlayerInput({});
		for (int i = 0; i < 25; ++i) {
			range.world->tick(1 / 30.0f);
		}
	}
	range.world->playerInfo(bot, info);
	CHECK(info.alive && std::abs(range.world->hitPoints(info.soldier) - 100) < 1e-3f);
	// Left down, the body goes after the man-down time.
	range.world->setPlayerInput({});
	range.world->setManDownTime(1);
	range.world->setHitPoints(info.soldier, 0);
	for (int i = 0; i < 35; ++i) {
		range.world->tick(1 / 30.0f);
	}
	range.world->playerInfo(bot, info);
	CHECK(!info.alive && !info.manDown && info.soldier == 0 && range.count("PlayerDeath") == 1);
}

// An enemy's paddles (0x004ba5c0's team test) leave a downed body down.
TEST(GameWorld_EnemyDefibrillatorDoesNothing)
{
	// A downed EU bot lies 1.5 m ahead; the EU soldier's defibrillator
	// (damageMandownSoldiers, short range) brings it back, and the
	// scripts hear PlayerRevived instead of PlayerDeath.
	Range range;
	range.templates.registerType(6, "Kit");
	world::ObjectTemplate* defib = range.templates.create("GenericFireArm", "unl_defibrillator");
	defib->settings()["itemIndex"] = "5";
	defib->settings()["projectileTemplate"] = "shock";
	defib->settings()["velocity"] = "60";
	defib->settings()["ammo.magSize"] = "1"; // one shock, then 0.6 s to recharge
	defib->settings()["ammo.reloadTime"] = "0.6";
	world::ObjectTemplate* shock = range.templates.create("GenericProjectile", "shock");
	shock->settings()["damage"] = "1000";
	shock->settings()["damageMandownSoldiers"] = "1";
	shock->settings()["collision.resurrectDamage"] = "20000"; // its ResurrectCollisionComp
	shock->settings()["collision.restoreHP"] = "120";
	shock->settings()["timeToLive"] = "CRD_NONE/0.05/0/0";
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"unl_defibrillator", {}, {}});
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const int bot = range.world->addPlayer("Foe", 1, true);
	CHECK(range.world->spawnSoldier(bot));
	game::GameWorld::PlayerInfo info, me;
	range.world->playerInfo(range.world->localPlayer(), me);
	range.world->playerInfo(bot, info);
	range.world->setSoldierPosition(info.soldier, me.position + physics::Vec3 {0, 0, 1.0f});
	range.world->setSpawnTime(30);
	range.world->setHitPoints(info.soldier, 0);
	range.world->playerInfo(bot, info);
	CHECK(!info.alive && info.manDown);
	game::SoldierInput input;
	input.selectItem = 5;
	range.world->setPlayerInput(input);
	range.world->tick(1 / 30.0f);
	input = {};
	input.fire = true;
	input.aimPitch = -60; // down at the body
	range.world->setPlayerInput(input);
	for (int i = 0; i < 5; ++i) {
		range.world->tick(1 / 30.0f);
	}
	range.world->playerInfo(bot, info);
	CHECK(!info.alive && info.manDown && range.count("PlayerRevived") == 0 && range.count("PlayerDeath") == 0);
}

TEST(Bots_ReviveTeammates)
{
	// An EU medic bot with a defibrillator; an EU teammate goes down 6 m
	// away: the bot walks over, takes the defibrillator out and revives it.
	Range range;
	range.templates.registerType(6, "Kit");
	world::ObjectTemplate* defib = range.templates.create("GenericFireArm", "unl_defibrillator");
	defib->settings()["itemIndex"] = "5";
	defib->settings()["projectileTemplate"] = "shock";
	defib->settings()["velocity"] = "60";
	defib->settings()["ammo.magSize"] = "1";
	defib->settings()["ammo.reloadTime"] = "0.6";
	world::ObjectTemplate* shock = range.templates.create("GenericProjectile", "shock");
	shock->settings()["damage"] = "1000";
	shock->settings()["damageMandownSoldiers"] = "1";
	shock->settings()["collision.resurrectDamage"] = "20000";
	shock->settings()["collision.restoreHP"] = "120";
	shock->settings()["timeToLive"] = "CRD_NONE/0.05/0/0";
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"unl_defibrillator", {}, {}});
	Console console;
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	std::string out;
	for (const char* line : {"aiSettings.createBehaviourModifiers StandardWeights", "aiSettings.setBehaviourModifier Idle 0.1",
			 "aiSettings.setBehaviourModifier MoveTo 1.0", "aiSettings.setBehaviourModifier Fire 7.5",
			 "aiSettings.setBehaviourModifier Revive 3.0"}) {
		console.execute(line, out);
	}
	game::BotManager bots(*range.world, settings);
	range.world->autoRespawn = false;
	const int medic = range.world->addPlayer("Medic", 2, true);
	const int mate = range.world->addPlayer("Mate", 2, true);
	// In a squad: Revive's urgency carries the squad factor (0x10089aa0),
	// 0 outside squads (the game puts every bot in one).
	range.world->joinSquad(medic, 1);
	range.world->joinSquad(mate, 1);
	CHECK(range.world->spawnSoldier(medic) && range.world->spawnSoldier(mate));
	game::GameWorld::PlayerInfo info;
	range.world->playerInfo(mate, info);
	range.world->setSoldierPosition(info.soldier, {6, 0, 3});
	range.world->setHitPoints(info.soldier, 0);
	for (int i = 0; i < 30; ++i) {
		bots.update(1 / 30.0f); // the next think (0x100c95f0)
	}
	CHECK(bots.behaviour(medic) == "Revive");
	for (int tick = 0; tick < 30 * 10 && range.world->playerInfo(mate, info) && !info.alive; ++tick) {
		bots.update(1 / 30.0f);
		range.world->tick(1 / 30.0f);
	}
	range.world->playerInfo(mate, info);
	CHECK(info.alive && range.count("PlayerRevived") == 1);
}

TEST(GameWorld_SquadsLeadersAndBeacons)
{
	Range range;
	range.templates.registerType(6, "Kit");
	world::ObjectTemplate* beacon = range.templates.create("GenericFireArm", "beacon");
	beacon->settings()["itemIndex"] = "8";
	beacon->settings()["projectileTemplate"] = "spawn_beacon";
	beacon->settings()["ammo.magSize"] = "1";
	range.templates.create("GenericProjectile", "spawn_beacon")->settings()["armor.holdsSlBeacon"] = "1";
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"beacon", {}, {}});

	// Six fit in a squad; the first leads; a seventh gets a new squad.
	std::vector<int> eu;
	for (int i = 0; i < 7; ++i) {
		eu.push_back(range.world->addPlayer("Eu" + std::to_string(i), 2, true));
		CHECK(range.world->joinSquad(eu.back(), range.world->openSquad(2)));
	}
	game::GameWorld::PlayerInfo info;
	range.world->playerInfo(eu[0], info);
	CHECK(info.squad == 1 && info.squadLeader);
	range.world->playerInfo(eu[5], info);
	CHECK(info.squad == 1 && !info.squadLeader);
	range.world->playerInfo(eu[6], info);
	CHECK(info.squad == 2 && info.squadLeader);
	CHECK(!range.world->joinSquad(eu[6], 1)); // full
	CHECK(range.count("PlayerChangedSquad") == 7);

	// The leader spawns and moves off; a member chooses to spawn on it.
	CHECK(range.world->spawnSoldier(eu[0]));
	range.world->playerInfo(eu[0], info);
	range.world->setSoldierPosition(info.soldier, {30, 0, 30});
	range.world->setSpawnChoice(eu[1], game::GameWorld::kSpawnOnLeader);
	CHECK(range.world->spawnSoldier(eu[1]));
	range.world->playerInfo(eu[1], info);
	// Scattered (0x005b2200): 3 to 9 m off (or half that), facing the leader.
	const float dx = info.position.x - 30, dz = info.position.z - 30;
	const float off = std::sqrt(dx * dx + dz * dz);
	std::printf("  spawned on the leader %.2f m off, facing %.0f\n", off, info.yaw);
	CHECK(off >= 1.5f - 1e-3f && off <= 9 + 1e-3f && std::abs(info.position.y) < 0.05f);
	CHECK(std::abs(std::remainder(info.yaw - std::atan2(-dx, -dz) * 57.29578f, 360.0f)) < 1);
	// A bot's tries must also be on the navigation mesh (0x005b2200's AI
	// flag -> IAIPathfindingCourier slot 0xc): with only the ground east of
	// the leader on it the bot lands there; with none it can't spawn.
	range.world->setAiSpawnTest([](const physics::Vec3& at) { return at.x > 30; });
	range.world->setSpawnChoice(eu[3], game::GameWorld::kSpawnOnLeader);
	CHECK(range.world->spawnSoldier(eu[3]));
	range.world->playerInfo(eu[3], info);
	CHECK(info.position.x > 30);
	range.world->setAiSpawnTest([](const physics::Vec3&) { return false; });
	range.world->setSpawnChoice(eu[4], game::GameWorld::kSpawnOnLeader);
	CHECK(!range.world->spawnSoldier(eu[4]));
	range.world->setAiSpawnTest({});

	// The leader sets down a beacon (slot 8) at (30, 31); a member spawns there.
	game::SoldierInput input;
	input.selectItem = 8;
	range.world->setInput(eu[0], input);
	range.world->tick(1 / 30.0f);
	input = {};
	input.fire = true;
	range.world->setInput(eu[0], input);
	range.world->tick(1 / 30.0f);
	range.world->setInput(eu[0], {});
	const uint32_t placed = range.world->squadBeacon(2, 1);
	CHECK(placed != 0);
	range.world->setSpawnChoice(eu[2], game::GameWorld::kSpawnOnBeacon);
	CHECK(range.world->spawnSoldier(eu[2]));
	range.world->playerInfo(eu[2], info);
	// Scattered round it as round a leader (the squad's spawn point, now
	// the beacon's child; no spawnInPod on this one).
	const world::Object* placedBeacon = range.objects.find(placed);
	CHECK(placedBeacon != nullptr);
	const float bx = info.position.x - placedBeacon->position().x, bz = info.position.z - placedBeacon->position().z;
	CHECK(std::sqrt(bx * bx + bz * bz) <= 9 + 1e-3f && std::abs(info.position.y) < 0.05f && info.vehicle == 0);

	// The leader leaves: the next member leads, the beacon goes.
	CHECK(range.world->joinSquad(eu[0], 0));
	range.world->playerInfo(eu[1], info);
	CHECK(info.squadLeader && range.world->squadBeacon(2, 1) == 0);
}

TEST(GameWorld_PortableShield)
{
	// unl_port_shield: its projectile (portable_shield) has mesh physics and
	// 600 hit points: it is set down ahead, a second replaces the first, and
	// shot to pieces it is gone.
	Range range;
	range.templates.registerType(6, "Kit");
	world::ObjectTemplate* gadget = range.templates.create("GenericFireArm", "unl_port_shield");
	gadget->settings()["itemIndex"] = "5";
	gadget->settings()["projectileTemplate"] = "portable_shield";
	gadget->settings()["ammo.magSize"] = "1";
	gadget->settings()["ammo.reloadTime"] = "0.1";
	world::ObjectTemplate* shield = range.templates.create("GenericProjectile", "portable_shield");
	shield->settings()["physicsType"] = "Mesh";
	shield->settings()["armor.maxHitPoints"] = "600";
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"unl_port_shield", {}, {}});
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const auto shields = [&] {
		std::vector<uint32_t> found;
		for (const auto& object : range.objects.objects()) {
			if (object->objectTemplate()->name() == "portable_shield") {
				found.push_back(object->id());
			}
		}
		return found;
	};
	const auto use = [&](game::SoldierInput input) {
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
		range.world->setPlayerInput({});
		for (int i = 0; i < 10; ++i) {
			range.world->tick(1 / 30.0f);
		}
	};
	game::SoldierInput take;
	take.selectItem = 5;
	use(take);
	game::SoldierInput fire;
	fire.fire = true;
	use(fire);
	CHECK(shields().size() == 1);
	CHECK(std::abs(range.objects.find(shields()[0])->position().z - 1) < 0.1f); // a metre ahead
	use(fire);
	CHECK(shields().size() == 1);
	range.world->setHitPoints(shields()[0], 0);
	CHECK(shields().empty());
}

TEST(GameWorld_ActiveCamouflage)
{
	// unl_active_camo (CamouflageAmmoComp): cloaked while the trigger is
	// held, until its heat runs out (0.005 a tick-round, cooling 0.08 a
	// second); a bot facing the cloaked soldier 20 m off doesn't see it.
	Range range;
	range.templates.registerType(6, "Kit");
	world::ObjectTemplate* camo = range.templates.create("GenericFireArm", "unl_active_camo");
	camo->settings()["itemIndex"] = "5";
	camo->settings()["ammo.magSize"] = "-1";
	camo->settings()["fire.roundsPerMinute"] = "1800";
	camo->settings()["heatAddWhenFire"] = "0.02";
	camo->settings()["coolDownPerSec"] = "0.08";
	camo->settings()["overheatPenalty"] = "1.5";
	camo->components().push_back("CamouflageAmmoComp");
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	range.templates.find("as_ar_rifle")->settings()["itemIndex"] = "3";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"unl_active_camo", {}, {}});
	CHECK(range.world->spawnPlayer(2) != nullptr);
	game::SoldierInput input;
	input.selectItem = 5;
	range.world->setPlayerInput(input);
	range.world->tick(1 / 30.0f);
	input = {};
	input.fire = true;
	range.world->setPlayerInput(input);
	range.world->tick(1 / 30.0f);
	game::GameWorld::PlayerInfo me;
	range.world->playerInfo(range.world->localPlayer(), me);
	CHECK(me.cloaked && range.objects.find(me.soldier)->hidden());

	// A PAC bot 20 m ahead, facing the soldier: it doesn't see it.
	Console console;
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	std::string out;
	console.execute("aiSettings.setDiscoverCloakedEnemiesDistance 3.0", out);
	game::BotManager bots(*range.world, settings);
	const int bot = range.world->addPlayer("Bot", 1, true);
	CHECK(range.world->spawnSoldier(bot));
	game::GameWorld::PlayerInfo info;
	range.world->playerInfo(bot, info);
	range.world->setSoldierPosition(info.soldier, {0, 0, 20});
	bots.update(1 / 30.0f);
	CHECK(bots.target(bot) == -1);

	// The heat runs out after about 50 ticks: it shows again.
	for (int i = 0; i < 60; ++i) {
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
	}
	range.world->playerInfo(range.world->localPlayer(), me);
	CHECK(!me.cloaked && !range.objects.find(me.soldier)->hidden());
}

TEST(GameWorld_GuidedMissileFollowsTheAim)
{
	// eu_av_missile: seek.targetType TTUnique, follow.maxYaw 0.45. Fired
	// north, the shooter then turns 30 degrees right: the missile turns
	// after the aim, no faster than 0.45 degrees a tick (DefaultFollowComp,
	// 0x005ecb30: 13.5 degrees a second).
	Range range;
	world::ObjectTemplate* rifle = range.templates.find("eu_ar_rifle");
	rifle->settings()["projectileTemplate"] = "missile";
	rifle->settings()["velocity"] = "40";
	world::ObjectTemplate* missile = range.templates.create("GenericProjectile", "missile");
	missile->settings()["seek.targetType"] = "TTUnique";
	missile->settings()["seek.trackingDelay"] = "0.01";
	missile->settings()["follow.maxYaw"] = "0.45";
	missile->settings()["follow.maxPitch"] = "0.85";
	missile->settings()["gravityModifier"] = "0";
	missile->settings()["timeToLive"] = "CRD_NONE/10/0/0";
	missile->settings()["damage"] = "100";
	CHECK(range.world->spawnPlayer(2) != nullptr);
	game::SoldierInput input;
	input.fire = true;
	input.aimPitch = 3;
	range.world->setPlayerInput(input);
	range.world->tick(1 / 30.0f);
	input.fire = false;
	input.turn = 30;
	range.world->setPlayerInput(input);
	range.world->tick(1 / 30.0f);
	input.turn = 0;
	range.world->setPlayerInput(input);
	for (int i = 0; i < 30; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(range.world->projectiles().size() == 1);
	if (!range.world->projectiles().empty()) {
		const physics::Vec3 v = range.world->projectiles().front().velocity;
		const float heading = std::atan2(v.x, v.z);
		// About a second of turning at 0.45 degrees a tick (0.236 rad), short
		// of the 30 degrees (0.52).
		CHECK(heading > 0.22f && heading < 0.26f);
	}
}

// A deployed hub carrying a SupplyObject (unl_hub_medic_stationary's
// Unl_Hub_Medic_Supply: radius 5, healSpeed 4, sharedStorageSize 300)
// works as the supply crates do (0x005d1710, every 0.2 s): 4 per cent of
// the maximum a second, from its storage, not by the gadget's ability.
TEST(GameWorld_HubSupplyObject)
{
	Range range;
	range.templates.registerType(6, "Kit");
	range.templates.registerType(7, "SupplyObject");
	world::ObjectTemplate* hub = range.templates.create("GenericFireArm", "unl_hub_medic");
	hub->settings()["itemIndex"] = "4";
	hub->settings()["projectileTemplate"] = "unl_hub_medic_stationary";
	hub->settings()["ammo.replenishingType"] = "RTHeal";
	hub->settings()["ammo.abilityRadius"] = "4";
	hub->settings()["ammo.abilityStrength"] = "3";
	hub->settings()["ammo.magSize"] = "1";
	hub->settings()["fire.roundsPerMinute"] = "60";
	world::ObjectTemplate* stationary = range.templates.create("GenericProjectile", "unl_hub_medic_stationary");
	stationary->children().push_back({"Unl_Hub_Medic_Supply", {}, {}});
	world::ObjectTemplate* supply = range.templates.create("SupplyObject", "Unl_Hub_Medic_Supply");
	supply->settings()["radius"] = "5";
	supply->settings()["healSpeed"] = "4";
	supply->settings()["sharedStorageSize"] = "300";
	supply->settings()["destroyWhenEmptyStorage"] = "1";
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"unl_hub_medic", {}, {}});
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const uint32_t soldier = range.world->playerObject();
	range.world->setHitPoints(soldier, 40);
	const auto press = [&](game::SoldierInput input) {
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
		range.world->setPlayerInput({});
		range.world->tick(1 / 30.0f);
	};
	game::SoldierInput take;
	take.selectItem = 4;
	press(take);
	game::SoldierInput fire;
	fire.fire = true;
	press(fire);
	CHECK(range.world->supplyObjects().size() == 1);
	game::SoldierInput rifle;
	rifle.selectItem = 3;
	press(rifle);
	const float before = range.world->hitPoints(soldier);
	for (int i = 0; i < 60; ++i) {
		range.world->tick(1 / 30.0f);
	}
	std::printf("  hub supply: %.1f -> %.1f in 2 s\n", before, range.world->hitPoints(soldier));
	CHECK(std::abs(range.world->hitPoints(soldier) - (before + 8)) < 1.0f); // 2 s at 4 a second
}

// The driver's inputs (0x005ca2e0): x (1 - EMP level / empDamageNumStep),
// all gone above level 1 for an auto-controlled one; x
// criticalDamageScaleModifier below criticalDamage with
// scaleInputOnCriticalDamage.
TEST(GameWorld_DriverInputUnderEmp)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "PlayerControlObject");
	world::ObjectTemplate* car = templates.create("PlayerControlObject", "car");
	car->settings()["armor.takesEmpDamage"] = "1";
	car->settings()["armor.criticalDamage"] = "150";
	CHECK(game::GameWorld::driverInputScale(0, 500, *car) == 1);
	CHECK(game::GameWorld::driverInputScale(3, 500, *car) == 0);
	CHECK(std::abs(game::GameWorld::driverInputScale(1.5f, 500, *car) - 1.0f / 3) < 1e-6f); // level 2
	CHECK(std::abs(game::GameWorld::driverInputScale(0.2f, 500, *car) - 2.0f / 3) < 1e-6f);
	car->settings()["scaleInputOnCriticalDamage"] = "1";
	car->settings()["criticalDamageScaleModifier"] = "0.5";
	CHECK(game::GameWorld::driverInputScale(0, 100, *car) == 0.5f && game::GameWorld::driverInputScale(0, 200, *car) == 1);
	car->settings()["autoControllerTemplateName"] = "SentryGun";
	CHECK(game::GameWorld::driverInputScale(1, 500, *car) == 1 && game::GameWorld::driverInputScale(2, 500, *car) == 0);
}

// The abandoned vehicle's time to live (the spawn's PlayerControlObject
// slots 0xa4/0xb8/0xc0, its check 0x005c8ba0 every half second): by its
// spawner (within `distance`) the countdown stays full; away from it, it
// runs down, and then the vehicle loses 25 hit points every half second
// (0.5 x the spawner's 50 at +0x2f0) until it is destroyed.
TEST(GameWorld_AbandonedVehiclesTimeOut)
{
	Level level;
	world::ObjectTemplate* spawner = level.templates.find("spawner_tank");
	spawner->settings()["timeToLive"] = "2";
	spawner->settings()["distance"] = "5";
	level.templates.find("tank_us")->settings()["armor.maxHitPoints"] = "100";
	game::GameWorld world(level.templates, level.objects);
	world.startRound();
	world.tick(1 / 30.0f);
	world::Object* tank = level.objects.objects().back().get();
	CHECK(tank->objectTemplate()->name() == "tank_us");
	CHECK(world.timeToLiveLeft(tank->id()) == 2);
	for (int i = 0; i < 30 * 4; ++i) {
		world.tick(1 / 30.0f);
	}
	CHECK(world.timeToLiveLeft(tank->id()) == 2); // by the spawner
	tank->setPosition({60, 5, 20});
	for (int i = 0; i < 30 * 5 + 15; ++i) {
		world.tick(1 / 30.0f);
	}
	// Checked again within 5 s (the spawner's wait), then every half second:
	// four checks empty it, each one after takes 25.
	std::fprintf(stderr, "  left %.1f s, %.0f hit points\n", world.timeToLiveLeft(tank->id()), world.hitPoints(tank->id()));
	CHECK(world.timeToLiveLeft(tank->id()) <= 0);
	CHECK(world.hitPoints(tank->id()) < 100);
	for (int i = 0; i < 30 * 3; ++i) {
		world.tick(1 / 30.0f);
	}
	CHECK(!level.objects.find(tank->id()) || level.objects.find(tank->id())->destroyed());
}

// holdObject (the end of 0x005af230): the spawner holds its last spawn at
// the spot until its first Engine turns; without an Engine it lets go at once.
TEST(GameWorld_SpawnerHoldsItsObject)
{
	for (int engine = 0; engine < 2; ++engine) {
		Level level;
		level.templates.find("spawner_tank")->settings()["holdObject"] = "1";
		if (engine) {
			level.templates.registerType(4, "Engine");
			level.templates.create("Engine", "tank_engine");
			level.templates.find("tank_us")->children().push_back({"tank_engine", {}, {}});
		}
		game::GameWorld world(level.templates, level.objects);
		world.startRound();
		world.tick(1 / 30.0f);
		world::Object* tank = level.objects.objects().back().get();
		tank->setPosition({40, 5, 20});
		world.tick(1 / 30.0f);
		CHECK(world.spawners()[0]->holding() == (engine == 1));
		CHECK((tank->position().x == 10) == (engine == 1));
	}
}

namespace {

// A collision mesh of one part whose LODs of types 0, 1 and 2 (projectiles,
// vehicles, soldiers) are all `faces` over `vertices`, boxed by them.
physics::CollisionMeshFile panelMesh(std::vector<std::array<float, 3>> vertices, std::vector<physics::CollisionFace> faces)
{
	physics::CollisionSubGeometry part;
	for (uint32_t type = 0; type < 3; ++type) {
		physics::CollisionLod lod;
		lod.collisionType = type;
		lod.boundsMin = {1e9f, 1e9f, 1e9f};
		lod.boundsMax = {-1e9f, -1e9f, -1e9f};
		for (const auto& v : vertices) {
			for (size_t k = 0; k < 3; ++k) {
				lod.boundsMin[k] = std::min(lod.boundsMin[k], v[k]);
				lod.boundsMax[k] = std::max(lod.boundsMax[k], v[k]);
			}
		}
		lod.vertices = vertices;
		lod.faces = faces;
		part.lods.push_back(lod);
	}
	physics::CollisionGeometry geometry;
	geometry.subGeometries = {part};
	physics::CollisionMeshFile mesh;
	mesh.version = 10;
	mesh.geometries = {geometry};
	return mesh;
}

// A wall in the x-y plane, `half` wide each way and `height` up, faced both ways.
physics::CollisionMeshFile wallMesh(float half, float height)
{
	return panelMesh({{-half, 0, 0}, {half, 0, 0}, {half, height, 0}, {-half, height, 0}},
		{{{0, 1, 2}, 0}, {{0, 2, 3}, 0}, {{0, 2, 1}, 0}, {{0, 3, 2}, 0}});
}

// A closed box `half` each way round the origin (faces wound outwards).
physics::CollisionMeshFile boxMesh(float half)
{
	const float h = half;
	return panelMesh({{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h}, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}},
		{{{0, 2, 1}, 0}, {{0, 3, 2}, 0}, {{4, 5, 6}, 0}, {{4, 6, 7}, 0}, {{0, 1, 5}, 0}, {{0, 5, 4}, 0},
			{{3, 6, 2}, 0}, {{3, 7, 6}, 0}, {{0, 4, 7}, 0}, {{0, 7, 3}, 0}, {{1, 2, 6}, 0}, {{1, 6, 5}, 0}});
}

void makeMobile(world::ObjectTemplate& t, const char* mesh)
{
	t.settings()["collisionMesh"] = mesh;
	t.settings()["hasMobilePhysics"] = "1";
	t.settings()["physicsType"] = "Mesh";
}

} // namespace

// The squad beacon's pod (0x005e5fd0 -> 0x005b2200 -> 0x004bef90): the
// beacon's thrower has spawn.spawnInPod 1 and spawn.spawnVehicle
// eu_pod_drop, so a member spawning on it starts 150 m over a scattered
// spot (3 to 9 m off, or half), seated in a pod that drops and lets the
// soldier out by the beacon.
TEST(GameWorld_BeaconSpawnDropsInPod)
{
	Range range;
	range.templates.registerType(6, "Kit");
	range.templates.registerType(7, "Parachute");
	range.collisionTemplates.add("pod_mesh", boxMesh(1));
	world::ObjectTemplate* beacon = range.templates.create("GenericFireArm", "beacon");
	beacon->settings()["itemIndex"] = "8";
	beacon->settings()["projectileTemplate"] = "spawn_beacon";
	beacon->settings()["ammo.magSize"] = "1";
	beacon->settings()["spawn.spawnVehicle"] = "eu_pod_drop";
	beacon->settings()["spawn.spawnInPod"] = "1";
	range.templates.create("GenericProjectile", "spawn_beacon")->settings()["armor.holdsSlBeacon"] = "1";
	world::ObjectTemplate* pod = range.templates.create("Parachute", "eu_pod_drop");
	makeMobile(*pod, "pod_mesh");
	pod->settings()["mass"] = "250";
	pod->settings()["armor.maxHitPoints"] = "10000";
	pod->settings()["isPod"] = "1";
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"beacon", {}, {}});
	range.world->enablePhysics(range.files, range.collisionTemplates,
		{[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& normal) {
			 normal = {0, 1, 0};
			 return 0.0f;
		 }});
	range.world->startRound();
	const int leader = range.world->addPlayer("Leader", 2, true);
	const int member = range.world->addPlayer("Member", 2, true);
	CHECK(range.world->joinSquad(leader, range.world->openSquad(2)));
	CHECK(range.world->joinSquad(member, 1));
	CHECK(range.world->spawnSoldier(leader));
	game::GameWorld::PlayerInfo info;
	range.world->playerInfo(leader, info);
	range.world->setSoldierPosition(info.soldier, {30, 0, 30});
	game::SoldierInput input;
	input.selectItem = 8;
	range.world->setInput(leader, input);
	range.world->tick(1 / 30.0f);
	input = {};
	input.fire = true;
	range.world->setInput(leader, input);
	range.world->tick(1 / 30.0f);
	range.world->setInput(leader, {});
	const world::Object* placed = range.objects.find(range.world->squadBeacon(2, 1));
	CHECK(placed != nullptr);
	if (!placed) {
		return;
	}
	const physics::Vec3 at {placed->position().x, placed->position().y, placed->position().z};
	range.world->setSpawnChoice(member, game::GameWorld::kSpawnOnBeacon);
	CHECK(range.world->spawnSoldier(member));
	CHECK(range.world->ridingPod(member) && range.world->pods().size() == 1);
	// The spawn point's disablePodControls (1 by default, 0x00635590):
	// the pod takes no steering (0x0059d2a0 -> 0x005ca2e0).
	CHECK(!range.world->pods().empty() && range.world->pods()[0].controlsOff);
	range.world->playerInfo(member, info);
	const float dx = info.position.x - at.x, dz = info.position.z - at.z;
	const float off = std::sqrt(dx * dx + dz * dz);
	std::fprintf(stderr, "  the pod starts %.1f m up, %.2f m off the beacon\n", info.position.y - at.y, off);
	CHECK(info.vehicle != 0 && info.position.y - at.y > 140 && off >= 1.5f - 1e-3f && off <= 9 + 1e-3f);
	float second = 0;
	for (; second < 60 && range.world->ridingPod(member); second += 1 / 30.0f) {
		range.world->tick(1 / 30.0f);
	}
	range.world->playerInfo(member, info);
	std::fprintf(stderr, "  landed after %.1f s at %.1f/%.1f/%.1f\n", second, info.position.x, info.position.y,
		info.position.z);
	CHECK(!range.world->ridingPod(member) && info.alive && info.vehicle == 0 && info.position.y < 5);
}

// The spawn spot's test (0x005aeb80, 0x006f2e90): a PlayerControlObject
// that the template's mesh placed at the spot overlaps (a segment from the
// spot to a vertex of its vehicle LOD crossing the other's soldier LOD)
// holds the spawn back, a try a second; after five tries it is removed (no
// soldier within 7 m) and the next try spawns.
TEST(GameWorld_SpawnSpotKeptClear)
{
	for (int far = 0; far < 2; ++far) {
		Level level;
		io::FileManager files;
		physics::CollisionTemplateManager collisionTemplates;
		collisionTemplates.add("tank_mesh", boxMesh(2));
		collisionTemplates.add("jeep_mesh", boxMesh(1.5f));
		makeMobile(*level.templates.find("tank_us"), "tank_mesh");
		world::ObjectTemplate* jeep = level.templates.create("PlayerControlObject", "jeep");
		makeMobile(*jeep, "jeep_mesh");
		// On the spot (the spawner at 10/5/20), or 6 m off it (within the
		// 10 m search, but clear of the tank's box).
		const uint32_t blocker = level.objects.create(jeep, {far ? 16.0f : 10.5f, 5, 20})->id();
		game::GameWorld world(level.templates, level.objects);
		world.enablePhysics(files, collisionTemplates,
			{[](float, float) { return 3.5f; }, [](float, float, physics::Vec3& normal) {
				 normal = {0, 1, 0};
				 return 3.5f;
			 }});
		world.tick(1 / 30.0f); // the jeep's body
		world.startRound();
		const auto run = [&](float seconds) {
			for (int i = 0; i < static_cast<int>(seconds * 30); ++i) {
				world.tick(1 / 30.0f);
			}
		};
		run(0.1f);
		if (far) {
			CHECK(level.count("tank_us") == 1 && level.objects.find(blocker));
			continue;
		}
		CHECK(level.count("tank_us") == 0);
		run(4.5f);
		CHECK(level.count("tank_us") == 0 && level.objects.find(blocker));
		CHECK(world.spawners()[0]->blockedTries() == 5);
		run(2.5f);
		std::fprintf(stderr, "  blocked %d tries; the jeep %s, %zu tank(s)\n", world.spawners()[0]->blockedTries(),
			level.objects.find(blocker) ? "stays" : "removed", level.count("tank_us"));
		CHECK(!level.objects.find(blocker));
		CHECK(level.count("tank_us") == 1);
	}
}

// A blast hidden from an object by something else (0x005a9090) halves, or
// stops when the object's radius (slot 0x90: its mesh's, or a child's
// offset plus its own) is under a fifth of the blast's: a 0.4 m crate
// behind a wall takes nothing from a 10 m blast, the same crate with a
// child 3 m out half.
TEST(GameWorld_BlastBehindAWallByTheObjectsRadius)
{
	for (int child = 0; child < 2; ++child) {
		Range range;
		range.collisionTemplates.add("wall_mesh", wallMesh(2, 3));
		range.collisionTemplates.add("crate_mesh", boxMesh(0.4f));
		range.templates.registerType(6, "SimpleObject");
		range.templates.registerType(7, "PlayerControlObject");
		world::ObjectTemplate* wall = range.templates.create("SimpleObject", "wall");
		wall->settings()["collisionMesh"] = "wall_mesh";
		range.objects.create(wall, {0, 0, 12});
		world::ObjectTemplate* crate = range.templates.create("PlayerControlObject", "crate");
		makeMobile(*crate, "crate_mesh");
		crate->settings()["armor.maxHitPoints"] = "100";
		if (child) {
			range.templates.create("PlayerControlObject", "crate_arm")->settings()["collisionPart"] = "0";
			crate->children().push_back({"crate_arm", {0, 0, 3}, {}});
		}
		const uint32_t id = range.objects.create(crate, {0, 0.4f, 15})->id();
		world::ObjectTemplate* blast = range.templates.create("GenericProjectile", "blast");
		blast->components().push_back("DefaultDetonationComp");
		blast->settings()["detonation.explosionDamage"] = "100";
		blast->settings()["detonation.explosionMaterial"] = "69";
		range.world->startRound(); // the wall's collision
		range.world->tick(1 / 30.0f); // the crate's body
		CHECK(range.world->detonate("blast", {0, 1, 10}));
		const float left = range.world->hitPoints(id);
		// 5.1 m off a 10 m blast: falloff 1 - 3.1 / 8, times a half.
		const physics::Vec3 d = physics::Vec3 {0, 0.4f, 15} - physics::Vec3 {0, 1, 10};
		const float expected = 100 - 100 * (1 - (std::sqrt(physics::lengthSquared(d)) - 2) / 8) * 0.5f;
		std::fprintf(stderr, "  with%s a child: %.1f hit points left\n", child ? "" : "out", left);
		CHECK(child ? std::fabs(left - expected) < 0.5f : left == 100);
	}
}

// A soldier's bone hidden only by an object the same blast destroys counts
// half (0x005a7530 with 0x005a8900's list): a panel between the blast and
// the bot, with 10 hit points, lets half the blast through; with 1000 none.
TEST(GameWorld_BlastThroughWhatItDestroys)
{
	for (int weak = 0; weak < 2; ++weak) {
		Range range;
		range.collisionTemplates.add("panel_mesh", wallMesh(2, 3));
		range.templates.registerType(7, "PlayerControlObject");
		world::ObjectTemplate* panel = range.templates.create("PlayerControlObject", "panel");
		makeMobile(*panel, "panel_mesh");
		panel->settings()["armor.maxHitPoints"] = weak ? "10" : "1000";
		const uint32_t id = range.objects.create(panel, {0, 0, 17})->id();
		world::ObjectTemplate* blast = range.templates.create("GenericProjectile", "blast");
		blast->components().push_back("DefaultDetonationComp");
		blast->settings()["detonation.explosionDamage"] = "100";
		blast->settings()["detonation.explosionMaterial"] = "69";
		const int bot = range.world->addPlayer("Bot", 1, true);
		CHECK(range.world->spawnSoldier(bot));
		range.world->tick(1 / 30.0f); // the panel's body
		game::GameWorld::PlayerInfo info;
		range.world->playerInfo(bot, info);
		const physics::Vec3 origin = info.position + physics::Vec3 {0, 1, 0};
		CHECK(std::fabs(origin.z - 20) < 0.5f);
		CHECK(range.world->detonate("blast", {origin.x, 1, 14}));
		range.world->playerInfo(bot, info);
		const float left = range.world->hitPoints(info.soldier);
		const float falloff = 1 - (std::sqrt(physics::lengthSquared(origin - physics::Vec3 {origin.x, 1, 14})) - 2) / 8;
		std::fprintf(stderr, "  the panel %s; the bot has %.1f hit points\n",
			range.world->hitPoints(id) <= 0 ? "destroyed" : "stands", left);
		CHECK(weak ? std::fabs(left - (100 - 100 * falloff * 0.5f)) < 0.5f : left == 100);
	}
}

// Ladders (0x0059fd40, 0x0059f6b0, 0x0059ed10): a soldier facing a ladder
// and looking up gets on and sits at the LadderContainer's seat (0/1/0 on
// the ladder's axis); climbing stops under a ceiling the up ray (1.7 m over
// the container, 0.75 m back) meets; the jump key puts it off 0.75 m back.
TEST(GameWorld_LadderRaysAndJumpingOff)
{
	Range range;
	range.collisionTemplates.add("ladder_mesh",
		panelMesh({{-0.3f, 0, -0.05f}, {0.3f, 0, -0.05f}, {0.3f, 6, -0.05f}, {-0.3f, 6, -0.05f}},
			{{{0, 1, 2}, 0}, {{0, 2, 3}, 0}}));
	// A ceiling at 3.5 m from 0.7 m behind the ladder: over where the up ray
	// looks (0.75 m back), not where the bottom's ray does (0.6 m back).
	range.collisionTemplates.add("ceiling_mesh",
		panelMesh({{-1, 3.5f, -2}, {1, 3.5f, -2}, {1, 3.5f, -0.7f}, {-1, 3.5f, -0.7f}},
			{{{0, 1, 2}, 0}, {{0, 2, 3}, 0}, {{0, 2, 1}, 0}, {{0, 3, 2}, 0}}));
	range.templates.registerType(8, "Ladder");
	range.templates.registerType(6, "SimpleObject");
	world::ObjectTemplate* ladder = range.templates.create("Ladder", "ladder");
	ladder->settings()["collisionMesh"] = "ladder_mesh";
	range.objects.create(ladder, {0, 0, 40});
	world::ObjectTemplate* ceiling = range.templates.create("SimpleObject", "ceiling");
	ceiling->settings()["collisionMesh"] = "ceiling_mesh";
	range.objects.create(ceiling, {0, 0, 40});
	range.world->startRound(); // the static collision
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const int me = range.world->localPlayer();
	game::GameWorld::PlayerInfo info;
	range.world->playerInfo(me, info);
	range.world->setSoldierPosition(info.soldier, {0, 0, 39.5f});
	game::SoldierInput climb;
	climb.aimPitch = 20; // looking up
	climb.throttle = 1;
	climb.turn = -info.yaw; // face +z, the ladder's way
	bool on = false;
	for (int i = 0; i < 30 && !on; ++i) {
		range.world->setInput(me, climb);
		climb.turn = 0;
		range.world->tick(1 / 30.0f);
		on = range.world->onLadder(me);
	}
	CHECK(on);
	for (int i = 0; i < 150; ++i) {
		range.world->setInput(me, climb);
		range.world->tick(1 / 30.0f);
		range.world->playerInfo(me, info);
	}
	range.world->playerInfo(me, info);
	std::fprintf(stderr, "  on the ladder at %.2f/%.2f/%.2f\n", info.position.x, info.position.y, info.position.z);
	CHECK(range.world->onLadder(me));
	CHECK(std::fabs(info.position.z - 40) < 0.01f && std::fabs(info.position.x) < 0.01f); // the seat: on the axis
	CHECK(info.position.y < 3.5f - 1.9f + 0.1f && info.position.y > 1.0f); // held under the ceiling
	game::SoldierInput jump;
	jump.jump = true;
	range.world->setInput(me, jump);
	range.world->tick(1 / 30.0f);
	range.world->playerInfo(me, info);
	CHECK(!range.world->onLadder(me));
	CHECK(std::fabs(info.position.z - 39.25f) < 0.05f);
}

// The spawn spot's drop (0x005ad8b0): the template's vehicle-LOD box from
// its top down to its bottom along the spawner's up axis meets a static
// object (flags 0x1000100 without 8: mobile ones don't count) and sits on
// it; otherwise a bottom under the terrain is lifted onto it.
TEST(GameWorld_SpawnSpotDropsOntoStaticObjects)
{
	Range range;
	range.collisionTemplates.add("slab_mesh", boxMesh(1));
	range.collisionTemplates.add("car_mesh", boxMesh(1));
	range.templates.registerType(6, "SimpleObject");
	range.templates.registerType(7, "PlayerControlObject");
	world::ObjectTemplate* slab = range.templates.create("SimpleObject", "slab");
	slab->settings()["collisionMesh"] = "slab_mesh";
	range.objects.create(slab, {0, 0, 30}); // its top at y 1
	world::ObjectTemplate* car = range.templates.create("PlayerControlObject", "car");
	makeMobile(*car, "car_mesh");
	range.objects.create(car, {10, 1, 30}); // a mobile one, top at 2
	range.world->startRound();
	range.world->tick(1 / 30.0f);
	const physics::Vec3 up {0, 1, 0};
	// A box from -0.5 to 1.5 at y 0.2 over the slab: its top (1.7) is over
	// the slab's (1), so it sits there, bottom at 1 (the spot at 1.5).
	physics::Vec3 at = range.world->dropSpawnSpot({0, 0.2f, 30}, up, -0.5f, 1.5f);
	CHECK(std::fabs(at.y - 1.5f) < 1e-3f && at.x == 0 && at.z == 30);
	// Over the car: the ray passes it; the terrain (0) is under the bottom.
	at = range.world->dropSpawnSpot({10, 1.2f, 30}, up, -0.5f, 1.5f);
	CHECK(std::fabs(at.y - 1.2f) < 1e-6f);
	// A bottom under the terrain comes up onto it.
	at = range.world->dropSpawnSpot({20, 0.1f, 30}, up, -0.5f, 1.5f);
	CHECK(std::fabs(at.y - 0.5f) < 1e-6f);
	// High in the air: unchanged.
	at = range.world->dropSpawnSpot({0, 10, 30}, up, -0.5f, 1.5f);
	CHECK(std::fabs(at.y - 10) < 1e-6f);
}

// A walker's spawn (0x006f28e0): a template with MechLegs, once one has
// a body, keeps its object's box bottom (0x005cd4f0: the root's geometry
// and its legs' child, geometry 1 at its offset), and the spot's test also
// probes three segments down by it (at the spot and 1.7 m either side): a
// car under the spot, out of the hull's reach, holds the walker back.
TEST(GameWorld_WalkerSpawnProbesUnderItsLegs)
{
	for (int clear = 0; clear < 2; ++clear) {
		Level level;
		io::FileManager files;
		physics::CollisionTemplateManager collisionTemplates;
		physics::CollisionMeshFile walkerMesh = boxMesh(0.5f);
		walkerMesh.geometries.push_back(boxMesh(0.5f).geometries[0]); // the legs: half a metre round their origin
		collisionTemplates.add("walker_mesh", walkerMesh);
		collisionTemplates.add("car_mesh", boxMesh(0.5f));
		level.templates.registerType(4, "MechLegs");
		level.templates.registerType(5, "SimpleObject");
		world::ObjectTemplate* walker = level.templates.find("tank_us");
		makeMobile(*walker, "walker_mesh");
		walker->children().push_back({"walker_legs_part", {0, -4.5f, 0}, {}});
		walker->children().push_back({"walker_legs", {}, {}});
		level.templates.create("SimpleObject", "walker_legs_part")->settings()["collisionPart"] = "1";
		level.templates.create("MechLegs", "walker_legs");
		world::ObjectTemplate* car = level.templates.create("PlayerControlObject", "car");
		makeMobile(*car, "car_mesh");
		// A walker parked far off (its body gives the box bottom: -5), and
		// a car 3 m under the spawner's spot (10/5/20) or 3 m beside it.
		level.objects.create(walker, {200, 50, 200});
		const uint32_t blocker = level.objects.create(car, {clear ? 13.0f : 10.0f, 2, 20})->id();
		game::GameWorld world(level.templates, level.objects);
		world.enablePhysics(files, collisionTemplates,
			{[](float, float) { return -10.0f; }, [](float, float, physics::Vec3& normal) {
				 normal = {0, 1, 0};
				 return -10.0f;
			 }});
		world.tick(1 / 30.0f); // the bodies
		const game::ObjectSpawner::SpotTest test =
			world.testSpawnSpot(*walker, {10, 5, 20}, {0, 0, 0}, {10, 5, 20});
		std::fprintf(stderr, "  clear %d: %zu blockers\n", clear, test.blockers.size());
		CHECK(clear ? test.blockers.empty() : test.blockers.size() == 1 && test.blockers[0] == blocker);
	}
}

// The footstep set's material (0x007fda00): the other material of the
// soldier's last collision event (0x00586a90's +0x32c), so on an object
// its face's material and on the terrain the terrain's.
TEST(GameWorld_FootstepsOnObjectsUseTheirMaterial)
{
	for (int onSlab = 0; onSlab < 2; ++onSlab) {
		Range range;
		range.templates.registerType(6, "SimpleObject");
		if (onSlab) {
			range.collisionTemplates.add("slab_mesh", panelMesh({{-10, 0, -10}, {10, 0, -10}, {10, 0, 10}, {-10, 0, 10}},
														  {{{0, 2, 3}, 0}, {{0, 1, 2}, 0}})); // facing up
			world::ObjectTemplate* slab = range.templates.create("SimpleObject", "slab");
			slab->settings()["collisionMesh"] = "slab_mesh";
			slab->settings()["mapMaterial.0"] = "42";
			range.objects.create(slab, {0, 0, 0}); // at the spawn point, over the terrain
		}
		physics::Ground ground;
		const float height = onSlab ? -5.0f : 0.0f;
		ground.height = [height](float, float) { return height; };
		ground.heightAndNormal = [height](float, float, physics::Vec3& normal) {
			normal = {0, 1, 0};
			return height;
		};
		ground.material = [](float, float) { return 7u; };
		range.world->enablePhysics(range.files, range.collisionTemplates, ground);
		range.world->startRound();
		CHECK(range.world->spawnPlayer(2) != nullptr);
		const int me = range.world->localPlayer();
		CHECK(range.world->footstepMaterial(me) == 0xffffffffu);
		if (onSlab) {
			CHECK(range.world->setSoldierPosition(range.world->playerObject(), {0, 0.02f, 0}));
		}
		game::SoldierInput walk;
		walk.throttle = 1;
		range.world->setPlayerInput(walk);
		for (int i = 0; i < 30; ++i) {
			range.world->tick(1 / 30.0f);
		}
		std::fprintf(stderr, "  on the %s: material %u, y %.2f\n", onSlab ? "slab" : "terrain", range.world->footstepMaterial(me), range.objects.find(range.world->playerObject())->position().y);
		CHECK(range.world->footstepMaterial(me) == (onSlab ? 42u : 7u));
	}
}

// The held weapon's effect event N (0x2c + N) reaches its EffectBundle
// children (0x0063ca60 down the tree; 0x0061a4e0): those whose
// startOnEffects has bit 9 + N start (e_muzz_eu_av_reload: 512, effect 0).
TEST(GameWorld_WeaponEffectEventsStartTheirBundles)
{
	Range range;
	range.templates.registerType(6, "EffectBundle");
	range.templates.registerType(7, "SpriteParticleSystem");
	world::ObjectTemplate* sparks = range.templates.create("SpriteParticleSystem", "reload_sparks");
	sparks->settings()["emitFrequency"] = "100";
	sparks->settings()["emitTime"] = "0.1";
	sparks->settings()["timeToLive"] = "0.5";
	sparks->settings()["textureName"] = "objects\\effects\\textures\\spark";
	world::ObjectTemplate* reload = range.templates.create("EffectBundle", "e_reload");
	reload->settings()["setStartOnEffects"] = "512";
	reload->children().push_back({"reload_sparks", {}, {}});
	for (const char* rifle : {"eu_ar_rifle", "as_ar_rifle"}) {
		range.templates.find(rifle)->children().push_back({"e_reload", {0, 0, 0.3f}, {}});
	}
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const int me = range.world->localPlayer();
	const size_t before = range.world->effects().emitters();
	range.world->weaponEffectEvent(me, 1); // not its bit
	CHECK(range.world->effects().emitters() == before);
	range.world->weaponEffectEvent(me, 0);
	CHECK(range.world->effects().emitters() == before + 1);
}

// The aim's recoil kick (SoldierBasedRecoilComp; 0x0058a6d0 adds
// 0x005861a0's value to the soldier's pitch and 0x00586140's to its turn):
// k ticks after a shot the graph's value times the forces drawn, so over
// the graph's frames the aim rises by recoilGraphTotalMovement x
// recoilForceUp; with goBackOnRecoil the second graph brings it back. The
// viewer takes the kick into its own pitch (takeAimKick).
TEST(GameWorld_RecoilKicksTheAim)
{
	for (const bool goBack : {false, true}) {
		Range range;
		world::ObjectTemplate* rifle = range.templates.find("eu_ar_rifle");
		rifle->components().push_back("SoldierBasedRecoilComp");
		rifle->settings()["recoil.hasRecoilForce"] = "1";
		rifle->settings()["recoil.goBackOnRecoil"] = goBack ? "1" : "0";
		rifle->settings()["recoil.recoilForceUp"] = "CRD_NONE/1.5/0/0";
		rifle->settings()["recoil.recoilForceLeftRight"] = "CRD_NONE/1/0/0";
		rifle->settings()["recoil.recoilGraphFrameCount"] = "8";
		rifle->settings()["recoil.recoilGraphTotalMovement"] = "2";
		CHECK(range.world->spawnPlayer(2) != nullptr);
		const int me = range.world->localPlayer();
		game::GameWorld::PlayerInfo info;
		range.world->playerInfo(me, info);
		const float yaw = info.yaw;
		game::SoldierInput input;
		input.fire = true;
		input.aimPitch = 5;
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
		input.fire = false;
		range.world->setPlayerInput(input);
		float highest = 0;
		for (int i = 0; i < 20; ++i) {
			range.world->tick(1 / 30.0f);
			highest = std::max(highest, range.world->aimPitchOf(me) - 5);
		}
		const float kicked = range.world->aimPitchOf(me) - 5;
		range.world->playerInfo(me, info);
		const float turned = std::remainder(info.yaw - yaw, 360.0f);
		std::fprintf(stderr, "  goBackOnRecoil %d: highest %.2f, after 20 ticks %.2f up, %.2f turned\n", goBack ? 1 : 0, highest,
			kicked, turned);
		// The soldier's update reads the kick before the weapon's tick fires,
		// so a shot's own tick (k = 0) passes: the graph's values for k = 1 on
		// (the left-right one twists the look: lookYaw).
		anim::RecoilSettings recoil;
		recoil.hasRecoilForce = true;
		recoil.goBackOnRecoil = goBack;
		recoil.graphFrameCount = 8;
		recoil.graphTotalMovement = 2;
		float expected = 0;
		for (int k = 1; k <= 20; ++k) {
			expected -= anim::recoilAt(recoil.graph(), k, 1.5f);
		}
		CHECK(std::fabs(kicked - expected) < 0.01f);
		CHECK(goBack ? highest > 1 && kicked < highest - 1 : kicked > 1.9f);
		// The viewer takes it: the aim is the input's again.
		CHECK(std::fabs(range.world->takeAimKick(me) - kicked) < 1e-4f);
		CHECK(std::fabs(range.world->aimPitchOf(me) - 5) < 1e-4f);
	}
}

TEST(GameWorld_DroppedKitsArePickedUpAndTimeOut)
{
	// A dead soldier's kit falls where he lay (0x00589890: the ray straight
	// down) with its weapons; the use key picks the closest within 3 m up
	// (0x004bda00 -> 0x004bcd80), dropping the taker's own; a kit lies
	// timeToLiveAfterDeath (30 s by default, 0x00570080) and goes.
	Range range;
	range.templates.registerType(6, "Kit");
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	world::ObjectTemplate* lmg = range.templates.create("GenericFireArm", "eu_lmg");
	lmg->settings()["itemIndex"] = "3";
	lmg->settings()["projectileTemplate"] = "bullet";
	lmg->settings()["ammo.magSize"] = "100";
	range.templates.create("Kit", "EU_Assault")->children().push_back({"eu_ar_rifle", {}, {}});
	world::ObjectTemplate* support = range.templates.create("Kit", "EU_Support");
	support->children().push_back({"eu_lmg", {}, {}});
	support->settings()["timeToLiveAfterDeath"] = "5";
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const int bot = range.world->addPlayer("Mate", 2, true);
	range.world->setKit(bot, "EU_Support", "");
	CHECK(range.world->spawnSoldier(bot));
	game::GameWorld::PlayerInfo info, me;
	range.world->playerInfo(range.world->localPlayer(), me);
	range.world->setSoldierPosition(range.world->playerInfo(bot, info) ? info.soldier : 0,
		me.position + physics::Vec3 {0, 0, 2.0f});
	range.world->playerInfo(bot, info);
	const uint32_t botKit = info.kit;
	CHECK(botKit != 0 && range.objects.find(botKit)->hidden());
	range.world->setSpawnTime(60);
	range.world->setHitPoints(info.soldier, 0);
	range.world->tick(1 / 30.0f);
	// On the ground under him, visible, with the scripts told.
	CHECK(range.world->droppedKits() == std::vector<uint32_t> {botKit} && range.world->isDroppedKit(botKit));
	CHECK(!range.objects.find(botKit)->hidden() && std::abs(range.objects.find(botKit)->position().y) < 1e-3f);
	CHECK(range.count("DropKit") == 1);
	// Use: no vehicle about, so the kit 2 m off is taken; the assault kit
	// goes down in its place, and the light machine gun is in hand.
	const auto held = [&] { return range.objects.find(range.world->playerWeaponObject())->objectTemplate()->name(); };
	CHECK(held() == "eu_ar_rifle");
	range.world->playerInfo(range.world->localPlayer(), me);
	const uint32_t ownKit = me.kit;
	game::SoldierInput use;
	use.use = true;
	range.world->setPlayerInput(use);
	range.world->tick(1 / 30.0f);
	range.world->setPlayerInput({});
	range.world->tick(1 / 30.0f);
	range.world->playerInfo(range.world->localPlayer(), me);
	CHECK(me.kit == botKit && held() == "eu_lmg" && range.world->playerWeapon()->roundsInMag() == 100);
	CHECK(range.objects.find(botKit)->hidden() && range.count("PickupKit") >= 3 && range.count("DropKit") == 2);
	CHECK(range.world->droppedKits() == std::vector<uint32_t> {ownKit});
	// Within 2 s of a pick-up, use takes nothing (the time stamp).
	range.world->setPlayerInput(use);
	range.world->tick(1 / 30.0f);
	range.world->setPlayerInput({});
	range.world->playerInfo(range.world->localPlayer(), me);
	CHECK(me.kit == botKit);
	// The assault kit (30 s) is gone after its time on the ground.
	for (int i = 0; i < 30 * 31; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(range.world->droppedKits().empty() && !range.objects.find(ownKit));
}

// SpawnPoint options (0x005b24f0, 0x005b2200): onlyForHuman / onlyForAI
// keep players of the other kind off a point; the offset is added to the
// point (the soldier's origin, (0, 1.02, 0) by default); a scattering
// point puts the soldier 3 to 9 m out, facing the point.
TEST(GameWorld_SpawnPointOptions)
{
	Range range;
	world::ObjectTemplate* sp = range.templates.find("sp");
	sp->settings()["setOnlyForAI"] = "1";
	world::ObjectTemplate* human = range.templates.create("SpawnPoint", "sp_human");
	human->settings()["setOnlyForHuman"] = "1";
	human->settings()["setSpawnPositionOffset"] = "0/1.25/0";
	range.objects.create(human, {30, 0, 0}, {0, 0, 0})->setControlPointId(2);
	game::GameWorld::PlayerInfo info;
	// The human takes the human-only point, with its offset.
	CHECK(range.world->spawnPlayer(2) != nullptr);
	range.world->playerInfo(range.world->localPlayer(), info);
	CHECK(std::abs(info.position.x - 30) < 1e-3f && std::abs(info.position.y - 0.25f) < 1e-3f);
	// The bot the AI-only one, at the default offset.
	const int bot = range.world->addPlayer("Bot", 2, true);
	CHECK(range.world->spawnSoldier(bot));
	range.world->playerInfo(bot, info);
	CHECK(std::abs(info.position.x) < 1e-3f && std::abs(info.position.y - 0.02f) < 1e-3f);
	// A human-only point alone: no bot spawns.
	sp->settings()["setOnlyForAI"] = "0";
	sp->settings()["setOnlyForHuman"] = "1";
	const int other = range.world->addPlayer("Other", 1, true);
	CHECK(!range.world->spawnSoldier(other));

	// Scattering: round the point, facing it.
	sp->settings()["setOnlyForHuman"] = "0";
	sp->settings()["setScatterSpawnPositions"] = "1";
	CHECK(range.world->spawnSoldier(other));
	range.world->playerInfo(other, info);
	const float dx = info.position.x, dz = info.position.z - 20;
	const float out = std::sqrt(dx * dx + dz * dz);
	std::fprintf(stderr, "  scattered %.2f m out\n", out);
	CHECK(out >= 1.5f - 1e-3f && out <= 9 + 1e-3f);
}

// The local soldier's own EMP level (its armour, with takesEmpDamage:
// level = ceil(amount / empDamageStepSize), wearing off a step every
// empLevelTime) and the tinnitus of an explosion within 0.6 x its radius
// (0x005a6e70, event 0xd), both for the post-production.
TEST(GameWorld_SoldierEmpAndTinnitus)
{
	Range range;
	range.templates.find("us_light_soldier")->settings()["armor.takesEmpDamage"] = "1";
	world::ObjectTemplate* blast = range.templates.create("GenericProjectile", "blast");
	blast->components().push_back("DefaultDetonationComp");
	blast->settings()["detonation.explosionDamage"] = "1";
	blast->settings()["detonation.explosionMaterial"] = "69";
	world::ObjectTemplate* emp = range.templates.create("GenericProjectile", "emp");
	emp->components().push_back("DefaultDetonationComp");
	emp->settings()["detonation.isEMPWeapon"] = "1";
	emp->settings()["detonation.explosionMaterial"] = "0";
	emp->settings()["detonation.explosionDamage"] = "300";
	CHECK(range.world->spawnPlayer(2) != nullptr);
	game::GameWorld::PlayerInfo info;
	range.world->playerInfo(range.world->localPlayer(), info);
	const physics::Vec3 origin = info.position + physics::Vec3 {0, 1, 0};
	CHECK(info.empLevel == 0);
	CHECK(range.world->detonate("emp", origin));
	range.world->playerInfo(range.world->localPlayer(), info);
	CHECK(info.empLevel == 3); // ceil(300 / 120), at most 3
	for (int i = 0; i < 30 * 6; ++i) {
		range.world->tick(1 / 30.0f);
	}
	range.world->playerInfo(range.world->localPlayer(), info);
	CHECK(info.empLevel == 2);
	CHECK(range.world->takeTinnitus() == 0); // an EMP rings no ears
	CHECK(range.world->takeCameraShake() == 0); // nor shakes the view (0x005a7ab0)
	// The default radius 10: 0.6 x 10 = 6 m.
	CHECK(range.world->detonate("blast", origin + physics::Vec3 {0, 0, 7}));
	CHECK(range.world->takeTinnitus() == 0);
	CHECK(range.world->detonate("blast", origin + physics::Vec3 {0, 0, 5}));
	CHECK(range.world->takeTinnitus() == 7);
	CHECK(range.world->takeTinnitus() == 0); // taken once
}

// A blast's camera shake (0x005a6e70): min(damage / 150, 1) x min(force /
// 20, 1) x 4, x 0.1 without an instigator, x (1 - d / 3r) off one's own
// object; nothing past 3r; and an EffectBundle's own (cameraShake, the
// mechs' footsteps: radius and damage from cameraShakeRadius / Force).
TEST(GameWorld_ExplosionCameraShake)
{
	Range range;
	CHECK(range.world->spawnPlayer(2) != nullptr);
	game::GameWorld::PlayerInfo info;
	range.world->playerInfo(range.world->localPlayer(), info);
	const physics::Vec3 origin = info.position + physics::Vec3 {0, 1, 0}; // the body's
	game::GameWorld::ShakeSource s;
	s.at = origin + physics::Vec3 {0, 0, 5};
	s.radius = 10;
	s.damage = 300;
	s.force = 150;
	s.material = -1;
	s.instigated = true;
	range.world->cameraShake(s);
	float shake = range.world->takeCameraShake();
	std::fprintf(stderr, "  shake at 5 m of 10: %.3f\n", shake);
	CHECK(std::fabs(shake - 4 * (1 - 5 / 30.0f)) < 1e-3f);
	CHECK(range.world->takeCameraShake() == 0); // taken once
	CHECK(range.world->takeTinnitus() == 0);   // material -1
	s.instigated = false;
	s.damage = 75; // half of 150
	range.world->cameraShake(s);
	CHECK(std::fabs(range.world->takeCameraShake() - 0.1f * 0.5f * 4 * (1 - 5 / 30.0f)) < 1e-4f);
	s.at = origin + physics::Vec3 {0, 0, 31};
	range.world->cameraShake(s);
	CHECK(range.world->takeCameraShake() == 0);
	// On the soldier itself: no falloff.
	s.at = origin + physics::Vec3 {0, 0, 29};
	s.hitObject = info.soldier;
	s.instigated = true;
	s.damage = 300;
	range.world->cameraShake(s);
	CHECK(std::fabs(range.world->takeCameraShake() - 4) < 1e-4f);
	// A gun's round (material 0, no damage): no shake, the tinnitus within 0.6r.
	s = {};
	s.at = origin + physics::Vec3 {3, 0, 0};
	s.material = 0;
	s.radius = 12;
	s.force = 90;
	range.world->cameraShake(s);
	CHECK(range.world->takeCameraShake() == 0 && range.world->takeTinnitus() == 7);
	// The mech's footstep bundle at 3 m: 0.6 x 1 x 4 x 0.1 x (1 - 3 / 30).
	range.templates.registerType(9, "EffectBundle");
	world::ObjectTemplate* step = range.templates.create("EffectBundle", "us_heavy_mech_FootStepEffectLeft");
	step->settings()["cameraShake"] = "1";
	step->settings()["cameraShakeRadius"] = "10";
	step->settings()["cameraShakeForce"] = "90";
	range.world->bundleCameraShake("us_heavy_mech_FootStepEffectLeft", origin + physics::Vec3 {0, 0, 3});
	CHECK(std::fabs(range.world->takeCameraShake() - 0.6f * 4 * 0.1f * 0.9f) < 1e-4f);
	CHECK(range.world->takeTinnitus() == 0);
	CHECK(range.world->takeSentShake(range.world->localPlayer()) == 0);
}

// Friendly fire (0x005b7290): a teammate's blast does
// soldierSplashFriendlyFire percent of an enemy's; under 1 none.
TEST(GameWorld_FriendlyFireScaling)
{
	const auto lost = [](float percent, bool teammate) {
		Range range;
		world::ObjectTemplate* blast = range.templates.create("GenericProjectile", "blast");
		blast->components().push_back("DefaultDetonationComp");
		blast->settings()["detonation.explosionDamage"] = "40";
		blast->settings()["detonation.explosionMaterial"] = "69";
		game::GameWorld::FriendlyFire ff;
		ff.soldierSplash = percent;
		range.world->setFriendlyFire(ff);
		const int victim = range.world->addPlayer("Victim", 2, true);
		const int attacker = range.world->addPlayer("Attacker", teammate ? 2 : 1, true);
		CHECK(range.world->spawnSoldier(victim) && range.world->spawnSoldier(attacker));
		game::GameWorld::PlayerInfo info;
		range.world->playerInfo(victim, info);
		CHECK(range.world->detonate("blast", info.position + physics::Vec3 {0, 1, 2}, attacker));
		return 100 - range.world->hitPoints(info.soldier);
	};
	const float enemy = lost(50, false), half = lost(50, true), none = lost(0.5f, true);
	std::fprintf(stderr, "  blast: %.2f from an enemy, %.2f from a teammate at 50%%, %.2f at 0.5%%\n", enemy, half, none);
	CHECK(enemy > 1 && std::fabs(half - enemy * 0.5f) < 0.01f && none == 0);
}

// hasOnTimeEffect (0x005b62a0): a fused round whose time runs out goes off
// (event 0x1f) with it, and is only removed (0x20) without.
TEST(GameWorld_OnTimeEffect)
{
	for (const bool effect : {true, false}) {
		Range range;
		world::ObjectTemplate* grenade = range.templates.create("GenericProjectile", "grenade");
		grenade->components().push_back("DefaultDetonationComp");
		grenade->settings()["detonation.explosionDamage"] = "60";
		grenade->settings()["detonation.explosionMaterial"] = "69";
		grenade->settings()["detonation.explosionRadius"] = "8";
		grenade->settings()["timeToLive"] = "0.2";
		grenade->settings()["hasOnTimeEffect"] = effect ? "1" : "0";
		grenade->settings()["physicsType"] = "RotationalPoint";
		grenade->settings()["collision.bouncing"] = "1";
		range.templates.find("eu_ar_rifle")->settings()["projectileTemplate"] = "grenade";
		range.templates.find("eu_ar_rifle")->settings()["velocity"] = "1";
		CHECK(range.world->spawnPlayer(2) != nullptr);
		game::GameWorld::PlayerInfo me;
		range.world->playerInfo(range.world->localPlayer(), me);
		game::SoldierInput fire;
		fire.fire = true;
		range.world->setPlayerInput(fire);
		range.world->tick(1 / 30.0f);
		range.world->setPlayerInput({});
		for (int i = 0; i < 30; ++i) {
			range.world->tick(1 / 30.0f);
		}
		CHECK(range.world->projectilesInFlight() == 0);
		const float hp = range.world->hitPoints(me.soldier);
		std::fprintf(stderr, "  hasOnTimeEffect %d: %.1f hit points left\n", effect ? 1 : 0, hp);
		CHECK(effect ? hp < 100 : hp == 100);
	}
}

// The dog-tag knife (0x00570ab0): the kit's isDogtagReplacement container
// takes the place of the knife for a player with totalDogtagCount 50 or
// more, unlocked or not.
TEST(GameWorld_DogtagReplacement)
{
	for (const int tags : {49, 50}) {
		Range range;
		range.templates.registerType(6, "Kit");
		range.templates.registerType(7, "ItemContainer");
		for (const char* name : {"knife", "knife_unlock"}) {
			world::ObjectTemplate* knife = range.templates.create("GenericFireArm", name);
			knife->settings()["itemIndex"] = "1";
		}
		range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
		world::ObjectTemplate* tag = range.templates.create("ItemContainer", "UnlockEU_AssaultDogTagReplacement");
		tag->settings()["isDogtagReplacement"] = "1";
		tag->settings()["replaceItem.list"] = "knife;";
		tag->children().push_back({"knife_unlock", {}, {}});
		world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
		kit->children().push_back({"knife", {}, {}});
		kit->children().push_back({"eu_ar_rifle", {}, {}});
		kit->children().push_back({"UnlockEU_AssaultDogTagReplacement", {}, {}});
		const int me = range.world->addPlayer("Me", 2, true);
		range.world->setKit(me, "EU_Assault", "us_light_soldier");
		range.world->setDogtagCount(me, tags);
		CHECK(range.world->spawnSoldier(me));
		game::GameWorld::PlayerInfo info;
		range.world->playerInfo(me, info);
		std::string first;
		for (const auto& [slot, name] : info.carried) {
			if (slot == 1) {
				first = name;
			}
		}
		std::fprintf(stderr, "  %d dog tags: %s\n", tags, first.c_str());
		CHECK(first == (tags >= 50 ? "knife_unlock" : "knife"));
	}
}

// Object.disableChildren (0x006f6fd0): a placed object's children leave
// the scene, so a gas tank's ladder can't be climbed.
TEST(GameWorld_DisabledChildrenHaveNoLadder)
{
	for (const bool disabled : {false, true}) {
		Range range;
		range.collisionTemplates.add("ladder_mesh",
			panelMesh({{-0.3f, 0, -0.05f}, {0.3f, 0, -0.05f}, {0.3f, 6, -0.05f}, {-0.3f, 6, -0.05f}},
				{{{0, 1, 2}, 0}, {{0, 2, 3}, 0}}));
		range.templates.registerType(8, "Ladder");
		range.templates.registerType(6, "SimpleObject");
		world::ObjectTemplate* ladder = range.templates.create("Ladder", "ladder_5m_c");
		ladder->settings()["collisionMesh"] = "ladder_mesh";
		world::ObjectTemplate* tank = range.templates.create("SimpleObject", "gastank_c");
		tank->children().push_back({"ladder_5m_c", {}, {}});
		Console console;
		world::registerWorldConsoleMethods(console, range.templates, range.objects);
		std::string out;
		console.execute("Object.create gastank_c", out);
		console.execute("Object.absolutePosition 0/0/40", out);
		if (disabled) {
			console.execute("Object.disableChildren 1", out);
		}
		console.execute("Object.disableChildren", out);
		CHECK(out == (disabled ? "1" : "0"));
		range.world->startRound();
		CHECK(range.world->spawnPlayer(2) != nullptr);
		const int me = range.world->localPlayer();
		game::GameWorld::PlayerInfo info;
		range.world->playerInfo(me, info);
		range.world->setSoldierPosition(info.soldier, {0, 0, 39.5f});
		game::SoldierInput climb;
		climb.aimPitch = 20;
		climb.throttle = 1;
		climb.turn = -info.yaw;
		bool on = false;
		for (int i = 0; i < 30 && !on; ++i) {
			range.world->setInput(me, climb);
			climb.turn = 0;
			range.world->tick(1 / 30.0f);
			on = range.world->onLadder(me);
		}
		CHECK(on == !disabled);
	}
}

// The unlocks' sprint modifiers (0x005706b0 -> the stamina's 0x00645790):
// sprintUseModifier slows the drain, sprintRechargeModifier speeds the
// recovery; setOverheatMod scales the named gadget's heat per round; and a
// squad member spawning on the beacon waits sv.spawnTime x its leader's
// beaconSpawnTimeMod (0x004bb220 -> player slot 0x108, 0x005913c0).
TEST(GameWorld_UnlockSprintHeatAndBeaconModifiers)
{
	const auto run = [](bool unlocked, float& drained, float& recovered, float& heat) {
		Range range;
		range.templates.registerType(6, "Kit");
		range.templates.registerType(7, "ItemContainer");
		world::ObjectTemplate* soldier = range.templates.find("us_light_soldier");
		soldier->settings()["sprintDissipationTime"] = "10";
		soldier->settings()["sprintRecoverTime"] = "20";
		soldier->settings()["sprintLimit"] = "0.1";
		world::ObjectTemplate* unlock = range.templates.create("ItemContainer", "UnlockEU_AssaultPlayer1");
		unlock->settings()["ability.sprintUseModifier"] = "0.5";
		unlock->settings()["ability.sprintRechargeModifier"] = "1.5";
		unlock->settings()["ability.setOverheatMod.list"] = "unl_active_camo 0.5;";
		world::ObjectTemplate* camo = range.templates.create("GenericFireArm", "unl_active_camo");
		camo->settings()["itemIndex"] = "5";
		camo->settings()["ammo.magSize"] = "-1";
		camo->settings()["fire.roundsPerMinute"] = "1800";
		camo->settings()["heatAddWhenFire"] = "0.02";
		camo->components().push_back("CamouflageAmmoComp");
		range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
		world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
		kit->children().push_back({"eu_ar_rifle", {}, {}});
		kit->children().push_back({"unl_active_camo", {}, {}});
		kit->children().push_back({"UnlockEU_AssaultPlayer1", {}, {}});
		const int me = range.world->addPlayer("Me", 2, true);
		range.world->setKit(me, "EU_Assault", "us_light_soldier");
		if (unlocked) {
			range.world->setUnlocks(me, {"UnlockEU_AssaultPlayer1"});
		}
		CHECK(range.world->spawnSoldier(me));
		game::SoldierInput input;
		input.throttle = 1;
		input.sprint = true;
		for (int i = 0; i < 120; ++i) { // four seconds' sprint
			range.world->setInput(me, input);
			range.world->tick(1 / 30.0f);
		}
		game::GameWorld::PlayerInfo info;
		range.world->playerInfo(me, info);
		drained = 1 - info.stamina;
		const float low = info.stamina;
		for (int i = 0; i < 45; ++i) { // a second and a half standing
			range.world->setInput(me, {});
			range.world->tick(1 / 30.0f);
		}
		range.world->playerInfo(me, info);
		recovered = info.stamina - low;
		input = {};
		input.selectItem = 5;
		range.world->setInput(me, input);
		range.world->tick(1 / 30.0f);
		input = {};
		input.fire = true;
		range.world->setInput(me, input);
		range.world->tick(1 / 30.0f);
		range.world->playerInfo(me, info);
		heat = info.heat;
	};
	float drained[2], recovered[2], heat[2];
	run(false, drained[0], recovered[0], heat[0]);
	run(true, drained[1], recovered[1], heat[1]);
	std::fprintf(stderr, "  drained %.3f / %.3f, recovered %.3f / %.3f, heat %.4f / %.4f\n", drained[0], drained[1],
		recovered[0], recovered[1], heat[0], heat[1]);
	CHECK(drained[0] > 0.1f && std::fabs(drained[1] - drained[0] * 0.5f) < 0.01f);
	CHECK(recovered[0] > 0.05f && std::fabs(recovered[1] - recovered[0] * 1.5f) < 0.01f);
	CHECK(heat[0] > 0 && std::fabs(heat[1] - heat[0] * 0.5f) < 1e-4f);

	// The beacon: a member who chose it waits 15 s x the leader's 0.8.
	Range range;
	range.templates.registerType(6, "Kit");
	range.templates.registerType(7, "ItemContainer");
	range.templates.create("ItemContainer", "UnlockEU_AssaultSquadLeader3")->settings()["ability.beaconSpawnTimeMod"] =
		"0.8";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"UnlockEU_AssaultSquadLeader3", {}, {}});
	const int leader = range.world->addPlayer("Leader", 2, true);
	const int member = range.world->addPlayer("Member", 2, true);
	CHECK(range.world->joinSquad(leader, range.world->openSquad(2)));
	CHECK(range.world->joinSquad(member, 1));
	for (const int id : {leader, member}) {
		range.world->setKit(id, "EU_Assault", "us_light_soldier");
	}
	range.world->setUnlocks(leader, {"UnlockEU_AssaultSquadLeader3"});
	CHECK(range.world->spawnSoldier(leader) && range.world->spawnSoldier(member));
	const auto waitAfterDeath = [&](int choice) {
		range.world->setSpawnChoice(member, choice);
		game::GameWorld::PlayerInfo info;
		range.world->playerInfo(member, info);
		range.world->setHitPoints(info.soldier, 0);
		range.world->tick(1 / 30.0f);
		range.world->playerInfo(member, info);
		const float wait = info.respawnIn;
		range.world->spawnSoldier(member);
		return wait;
	};
	const float plain = waitAfterDeath(-1), onBeacon = waitAfterDeath(game::GameWorld::kSpawnOnBeacon);
	std::fprintf(stderr, "  respawn wait %.2f, on the beacon %.2f\n", plain, onBeacon);
	CHECK(plain > 1 && std::fabs(onBeacon - plain * 0.8f) < 0.05f);
}

// An unlock's Ability lists: ability.addExtraAmmoItem gives the rifle
// two more magazines, ability.setMaterialDamageMod cuts the damage of the
// rifle's projectile material (69 here) to the soldier by a quarter.
TEST(GameWorld_UnlockAbilityLists)
{
	Range range;
	range.templates.registerType(6, "Kit");
	range.templates.registerType(7, "ItemContainer");
	world::ObjectTemplate* armour = range.templates.create("ItemContainer", "UnlockEU_AssaultPlayer5");
	armour->settings()["ability.setMaterialDamageMod.list"] = "69 0.75;";
	armour->settings()["ability.addExtraAmmoItem.list"] = "eu_ar_rifle 2 0 0;";
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"UnlockEU_AssaultPlayer5", {}, {}});
	const int plain = range.world->addPlayer("Plain", 2, true);
	range.world->setKit(plain, "EU_Assault", "us_light_soldier");
	CHECK(range.world->spawnSoldier(plain));
	const int armoured = range.world->addPlayer("Armoured", 2, true);
	range.world->setKit(armoured, "EU_Assault", "us_light_soldier");
	range.world->setUnlocks(armoured, {"UnlockEU_AssaultPlayer5"});
	CHECK(range.world->spawnSoldier(armoured));
	const auto mags = [&](int player) {
		const auto ammo = range.world->weaponAmmo(player);
		return ammo.empty() ? -1 : ammo.front().magazines;
	};
	std::fprintf(stderr, "  magazines: %d plain, %d with the unlock\n", mags(plain), mags(armoured));
	CHECK(mags(armoured) == mags(plain) + 2);
	game::GameWorld::PlayerInfo a, b;
	range.world->playerInfo(plain, a);
	range.world->playerInfo(armoured, b);
	world::ObjectTemplate* blast = range.templates.create("GenericProjectile", "blast");
	blast->components().push_back("DefaultDetonationComp");
	blast->settings()["detonation.explosionDamage"] = "40";
	blast->settings()["detonation.explosionMaterial"] = "69";
	CHECK(range.world->detonate("blast", (a.position + b.position) * 0.5f + physics::Vec3 {0, 1, 0}));
	const float lostPlain = 100 - range.world->hitPoints(a.soldier);
	const float lostArmoured = 100 - range.world->hitPoints(b.soldier);
	std::fprintf(stderr, "  blast: %.2f lost plain, %.2f with the unlock\n", lostPlain, lostArmoured);
	CHECK(lostPlain > 1 && std::fabs(lostArmoured - 0.75f * lostPlain) < 0.01f);
}

// replaceItem: an equipped unlock's rifle takes the place of the kit's
// own (the console method keeps the list), and is the one in hand.
TEST(GameWorld_UnlockReplacesItem)
{
	Range range;
	range.templates.registerType(6, "Kit");
	range.templates.registerType(7, "ItemContainer");
	Console console;
	world::registerWorldConsoleMethods(console, range.templates, range.objects);
	world::ObjectTemplate* better = range.templates.create("GenericFireArm", "unl_har_rifle");
	better->settings()["itemIndex"] = "3";
	better->settings()["projectileTemplate"] = "bullet";
	world::ObjectTemplate* unlock = range.templates.create("ItemContainer", "UnlockEU_AssaultLight3");
	unlock->children().push_back({"unl_har_rifle", {}, {}});
	std::string out;
	CHECK(console.execute("ObjectTemplate.activeSafe ItemContainer UnlockEU_AssaultLight3", out) == ConsoleResult::Ok);
	CHECK(console.execute("ObjectTemplate.replaceItem eu_ar_rifle", out) == ConsoleResult::Ok);
	CHECK(console.execute("ObjectTemplate.replaceItem unl_best_buy_rifle", out) == ConsoleResult::Ok);
	CHECK(unlock->settings()["replaceItem.list"] == "eu_ar_rifle;unl_best_buy_rifle;");
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"UnlockEU_AssaultLight3", {}, {}});
	const int plain = range.world->addPlayer("Plain", 2, true);
	range.world->setKit(plain, "EU_Assault", "us_light_soldier");
	CHECK(range.world->spawnSoldier(plain));
	const int equipped = range.world->addPlayer("Equipped", 2, true);
	range.world->setKit(equipped, "EU_Assault", "us_light_soldier");
	range.world->setUnlocks(equipped, {"UnlockEU_AssaultLight3"});
	CHECK(range.world->spawnSoldier(equipped));
	const auto carried = [&](int player) {
		std::vector<std::string> names;
		for (const auto& w : range.world->weaponAmmo(player)) {
			names.push_back(w.weapon);
		}
		return names;
	};
	CHECK(carried(plain) == std::vector<std::string> {"eu_ar_rifle"});
	CHECK(carried(equipped) == std::vector<std::string> {"unl_har_rifle"});
}

// The kit build (0x00570ab0): a firearm replacement (isFirearmReplacement,
// firearmReplacement unl_har_rifle) applies by itself once its rifle is
// the player's, putting its shotgun in place of the base one; a
// replaceWithoutEquip container (the ...Player0 frag grenade) applies when
// owned, chosen or not.
TEST(GameWorld_FirearmReplacementAndOwnedUnlocks)
{
	Range range;
	range.templates.registerType(6, "Kit");
	range.templates.registerType(7, "ItemContainer");
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	const auto firearm = [&](const char* name, int slot) {
		world::ObjectTemplate* t = range.templates.create("GenericFireArm", name);
		t->settings()["itemIndex"] = std::to_string(slot);
		t->settings()["projectileTemplate"] = "bullet";
	};
	firearm("as_ar_shotgun", 5);
	firearm("unl_har_rifle", 3);
	firearm("unl_har_shotgun", 5);
	firearm("unl_grenade_frag", 7);
	world::ObjectTemplate* har = range.templates.create("ItemContainer", "UnlockHAR");
	har->children().push_back({"unl_har_rifle", {}, {}});
	har->settings()["replaceItem.list"] = "eu_ar_rifle;";
	world::ObjectTemplate* add = range.templates.create("ItemContainer", "UnlockHARShotgun");
	add->children().push_back({"unl_har_shotgun", {}, {}});
	add->settings()["isFirearmReplacement"] = "1";
	add->settings()["firearmReplacement"] = "unl_har_rifle";
	add->settings()["replaceItem.list"] = "as_ar_shotgun;";
	world::ObjectTemplate* frag = range.templates.create("ItemContainer", "UnlockPlayer0");
	frag->children().push_back({"unl_grenade_frag", {}, {}});
	frag->settings()["replaceWithoutEquip"] = "1";
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	for (const char* child : {"eu_ar_rifle", "as_ar_shotgun", "UnlockHAR", "UnlockHARShotgun", "UnlockPlayer0"}) {
		kit->children().push_back({child, {}, {}});
	}
	const auto carried = [&](int player) {
		game::GameWorld::PlayerInfo info;
		range.world->playerInfo(player, info);
		std::vector<std::pair<int, std::string>> slots(info.carried.begin(), info.carried.end());
		std::sort(slots.begin(), slots.end());
		std::string list;
		for (const auto& [slot, name] : slots) {
			list += std::to_string(slot) + ":" + name + " ";
		}
		return list;
	};
	const int plain = range.world->addPlayer("Plain", 2, true);
	range.world->setKit(plain, "EU_Assault", "us_light_soldier");
	CHECK(range.world->spawnSoldier(plain));
	CHECK(carried(plain) == "3:eu_ar_rifle 5:as_ar_shotgun ");
	const int har2 = range.world->addPlayer("HAR", 2, true);
	range.world->setKit(har2, "EU_Assault", "us_light_soldier");
	range.world->setUnlocks(har2, {"UnlockHAR"});
	range.world->setOwnedUnlocks(har2, {"UnlockPlayer0"});
	CHECK(range.world->spawnSoldier(har2));
	std::fprintf(stderr, "  %s\n", carried(har2).c_str());
	CHECK(carried(har2) == "3:unl_har_rifle 5:unl_har_shotgun 7:unl_grenade_frag ");
	// The local player's owned set comes from its profile
	// (localPlayerOwned, main.cpp: the soldier's stats unlocks), apart from
	// the chosen ones the kit menu equips.
	range.world->localPlayerUnlocks = {};
	range.world->localPlayerOwned = {"UnlockPlayer0"};
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const int local = range.world->localPlayer();
	range.world->setKit(local, "EU_Assault", "us_light_soldier");
	range.world->setHitPoints(range.world->playerObject(), 0);
	for (int i = 0; i < 30 * 30 && !range.world->spawnPlayer(2); ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(carried(local) == "3:eu_ar_rifle 5:as_ar_shotgun 7:unl_grenade_frag ");
}

// altItemIndex (template +0x2e4): the kit's add-item (0x00589510) puts an
// unlock whose itemIndex is held in its altItemIndex when that is free
// (unl_shotgun_02: 5 -> 6); with no free alternative it isn't added.
TEST(GameWorld_UnlockTakesItsAlternativeSlot)
{
	Range range;
	range.templates.registerType(6, "Kit");
	range.templates.registerType(7, "ItemContainer");
	range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
	world::ObjectTemplate* mine = range.templates.create("GenericFireArm", "unl_mine");
	mine->settings()["itemIndex"] = "5";
	world::ObjectTemplate* shotgun = range.templates.create("GenericFireArm", "unl_shotgun_02");
	shotgun->settings()["itemIndex"] = "5";
	shotgun->settings()["altItemIndex"] = "6";
	world::ObjectTemplate* other = range.templates.create("GenericFireArm", "unl_other");
	other->settings()["itemIndex"] = "5";
	world::ObjectTemplate* unlock = range.templates.create("ItemContainer", "UnlockShotgun");
	unlock->children().push_back({"unl_shotgun_02", {}, {}});
	unlock->children().push_back({"unl_other", {}, {}});
	world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"unl_mine", {}, {}});
	kit->children().push_back({"UnlockShotgun", {}, {}});
	const int player = range.world->addPlayer("P", 2, true);
	range.world->setKit(player, "EU_Assault", "us_light_soldier");
	range.world->setUnlocks(player, {"UnlockShotgun"});
	CHECK(range.world->spawnSoldier(player));
	game::GameWorld::PlayerInfo info;
	CHECK(range.world->playerInfo(player, info));
	std::vector<std::pair<int, std::string>> slots(info.carried.begin(), info.carried.end());
	std::sort(slots.begin(), slots.end());
	CHECK(slots.size() == 3);
	CHECK(slots.size() == 3 && slots[1] == std::make_pair(5, std::string("unl_mine")));
	CHECK(slots.size() == 3 && slots[2] == std::make_pair(6, std::string("unl_shotgun_02")));
}

// A lock-on weapon (target.targetSystem TSHeatSeeking): an enemy carrying
// a TTHeat target object in the lock cone is locking at once and locked
// after lockDelay; turned away, the lock holds for loseLockDelay, then
// goes.
TEST(GameWorld_WeaponLock)
{
	Range range;
	range.templates.registerType(8, "SimpleObject");
	world::ObjectTemplate* launcher = range.templates.find("eu_ar_rifle");
	launcher->settings()["target.targetSystem"] = "TSHeatSeeking";
	launcher->settings()["target.maxDistance"] = "450";
	launcher->settings()["target.lockAngle"] = "10";
	launcher->settings()["target.lockDelay"] = "1.5";
	launcher->settings()["target.loseLockDelay"] = "0.35";
	world::ObjectTemplate* jet = range.templates.create("SimpleObject", "jet");
	jet->children().push_back({"HeatObject", {}, {}});
	range.templates.create("SimpleObject", "HeatObject")->settings()["targetType"] = "TTHeat";
	CHECK(range.world->spawnPlayer(2) != nullptr);
	game::GameWorld::PlayerInfo me;
	range.world->playerInfo(range.world->localPlayer(), me);
	world::Object* enemy = range.objects.create(jet, {me.position.x, me.position.y + 1.6f, me.position.z + 60});
	enemy->setTeam(1);
	range.world->tick(1 / 30.0f);
	game::GameWorld::WeaponLockInfo lock = range.world->weaponLock(range.world->localPlayer());
	CHECK(lock.target == enemy->id() && lock.state == 1 && lock.maxDistance == 450);
	for (int i = 0; i < 46; ++i) {
		range.world->tick(1 / 30.0f);
	}
	lock = range.world->weaponLock(range.world->localPlayer());
	CHECK(lock.target == enemy->id() && lock.state == 2);
	// Turned a quarter round: kept for 0.35 s, then lost.
	game::SoldierInput turn;
	turn.turn = 90;
	range.world->setPlayerInput(turn);
	range.world->tick(1 / 30.0f);
	range.world->setPlayerInput({});
	CHECK(range.world->weaponLock(range.world->localPlayer()).target == enemy->id());
	for (int i = 0; i < 12; ++i) {
		range.world->tick(1 / 30.0f);
	}
	lock = range.world->weaponLock(range.world->localPlayer());
	CHECK(lock.target == 0 && lock.state == 0);
}

// The lock-on update's candidate (0x005e1b30 -> 0x004cf290): the nearest
// target object in the cone, not the one nearest the aim; an empty
// vehicle of the shooter's own side is a candidate too. Locked, another
// candidate replaces the target only after more than loseLockDelay ticks,
// unlocked.
TEST(GameWorld_WeaponLockTakesTheNearest)
{
	Range range;
	range.templates.registerType(8, "SimpleObject");
	world::ObjectTemplate* launcher = range.templates.find("eu_ar_rifle");
	launcher->settings()["target.targetSystem"] = "TSHeatSeeking";
	launcher->settings()["target.maxDistance"] = "450";
	launcher->settings()["target.lockAngle"] = "20";
	launcher->settings()["target.lockDelay"] = "0.5";
	launcher->settings()["target.loseLockDelay"] = "0.2";
	world::ObjectTemplate* jet = range.templates.create("SimpleObject", "jet");
	jet->children().push_back({"HeatObject", {}, {}});
	range.templates.create("SimpleObject", "HeatObject")->settings()["targetType"] = "TTHeat";
	CHECK(range.world->spawnPlayer(2) != nullptr);
	game::GameWorld::PlayerInfo me;
	range.world->playerInfo(range.world->localPlayer(), me);
	const float eye = me.position.y + 1.6f;
	world::Object* far = range.objects.create(jet, {me.position.x, eye, me.position.z + 200});
	far->setTeam(1);
	world::Object* nearer = range.objects.create(jet, {me.position.x + 20, eye, me.position.z + 80}); // ~14 degrees off
	nearer->setTeam(2); // an empty one of our own side
	range.world->tick(1 / 30.0f);
	game::GameWorld::WeaponLockInfo lock = range.world->weaponLock(range.world->localPlayer());
	CHECK(lock.target == nearer->id() && lock.state == 1);
	int ticks = 1;
	while (range.world->weaponLock(range.world->localPlayer()).state != 2 && ticks < 60) {
		range.world->tick(1 / 30.0f);
		++ticks;
	}
	std::fprintf(stderr, "  locked after %d ticks\n", ticks);
	CHECK(ticks == 17); // the 16th tick after the one it was taken on: strictly past lockDelay's 15
	// The nearer one goes: the far one replaces it after more than 6 ticks.
	nearer->setPosition({me.position.x - 200, eye, me.position.z});
	for (int i = 0; i < 6; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(range.world->weaponLock(range.world->localPlayer()).target == nearer->id());
	range.world->tick(1 / 30.0f);
	range.world->tick(1 / 30.0f);
	lock = range.world->weaponLock(range.world->localPlayer());
	CHECK(lock.target == far->id() && lock.state == 1);
}

// A vehicle's TargetObject switches itself off (0x0058eba0): with
// HeatObject's disableWhenEmptyVehicle an empty one, never sat in, can't
// be locked; without it, it can. (A wreck is out of the lock either way:
// it counts as destroyed.)
TEST(GameWorld_TargetObjectOffWhenEmpty)
{
	for (int off = 0; off < 2; ++off) {
		Range range;
		range.collisionTemplates.add("crate_mesh", boxMesh(1));
		range.templates.registerType(8, "SimpleObject");
		range.templates.registerType(20, "PlayerControlObject");
		range.templates.registerType(23, "Engine");
		world::ObjectTemplate* launcher = range.templates.find("eu_ar_rifle");
		launcher->settings()["target.targetSystem"] = "TSHeatSeeking";
		launcher->settings()["target.maxDistance"] = "450";
		launcher->settings()["target.lockAngle"] = "20";
		launcher->settings()["target.lockDelay"] = "0.1";
		range.templates.create("Engine", "crate_engine")->settings()["engineType"] = "c_ETCar";
		world::ObjectTemplate* heat = range.templates.create("SimpleObject", "HeatObject");
		heat->settings()["targetType"] = "TTHeat";
		heat->settings()["disableWhenEmptyVehicle"] = off ? "1" : "0";
		heat->settings()["disableWhenEmptyVehicleDelay"] = "5";
		world::ObjectTemplate* crate = range.templates.create("PlayerControlObject", "crate");
		makeMobile(*crate, "crate_mesh");
		crate->children().push_back({"crate_engine", {}, {}});
		crate->children().push_back({"HeatObject", {}, {}});
		CHECK(range.world->spawnPlayer(2) != nullptr);
		game::GameWorld::PlayerInfo me;
		range.world->playerInfo(range.world->localPlayer(), me);
		world::Object* enemy = range.objects.create(crate, {me.position.x, 1, me.position.z + 40});
		enemy->setTeam(1);
		for (int i = 0; i < 10; ++i) {
			range.world->tick(1 / 30.0f);
		}
		const game::GameWorld::WeaponLockInfo lock = range.world->weaponLock(range.world->localPlayer());
		CHECK(off ? lock.target == 0 : lock.target == enemy->id() && lock.state == 2);
	}
}

// A heat seeker's round (SeekClosestTargetComp, TTHeat): fired locked it
// has the launcher's target and turns after it (0x005e1ee0, 0x005ec260);
// fired unlocked with reLockTime 9999 it never takes one.
TEST(GameWorld_HeatSeekerChasesItsLock)
{
	for (const bool locked : {true, false}) {
		Range range;
		range.templates.registerType(8, "SimpleObject");
		world::ObjectTemplate* launcher = range.templates.find("eu_ar_rifle");
		launcher->settings()["target.targetSystem"] = "TSHeatSeeking";
		launcher->settings()["target.maxDistance"] = "450";
		launcher->settings()["target.lockAngle"] = "40";
		launcher->settings()["target.lockDelay"] = locked ? "0.2" : "30";
		launcher->settings()["projectileTemplate"] = "heat_missile";
		launcher->settings()["velocity"] = "40";
		world::ObjectTemplate* missile = range.templates.create("GenericProjectile", "heat_missile");
		missile->settings()["damage"] = "100";
		missile->settings()["gravityModifier"] = "0";
		missile->settings()["timeToLive"] = "CRD_NONE/10/0/0";
		missile->settings()["seek.targetType"] = "TTHeat";
		missile->settings()["seek.reLockTime"] = "9999";
		missile->settings()["seek.maxAngleLock"] = "40";
		missile->settings()["follow.maxYaw"] = "3";
		missile->settings()["follow.maxPitch"] = "3";
		world::ObjectTemplate* jet = range.templates.create("SimpleObject", "jet");
		jet->children().push_back({"HeatObject", {}, {}});
		range.templates.create("SimpleObject", "HeatObject")->settings()["targetType"] = "TTHeat";
		CHECK(range.world->spawnPlayer(2) != nullptr);
		game::GameWorld::PlayerInfo me;
		range.world->playerInfo(range.world->localPlayer(), me);
		world::Object* enemy = range.objects.create(jet, {me.position.x + 40, me.position.y + 1.6f, me.position.z + 80});
		enemy->setTeam(1);
		for (int i = 0; i < 10; ++i) {
			range.world->tick(1 / 30.0f);
		}
		CHECK((range.world->weaponLock(range.world->localPlayer()).state == 2) == locked);
		game::SoldierInput fire;
		fire.fire = true;
		range.world->setPlayerInput(fire);
		range.world->tick(1 / 30.0f);
		range.world->setPlayerInput({});
		CHECK(range.world->projectiles().size() == 1);
		for (int i = 0; i < 30 && !range.world->projectiles().empty(); ++i) {
			range.world->tick(1 / 30.0f);
		}
		CHECK(!range.world->projectiles().empty());
		if (range.world->projectiles().empty()) {
			continue;
		}
		const auto& round = range.world->projectiles().front();
		std::fprintf(stderr, "  %s: after 1 s at x %.1f, target %u\n", locked ? "locked" : "unlocked", round.position.x - me.position.x,
			round.lockedOn);
		CHECK((round.lockedOn == enemy->id()) == locked);
		CHECK(locked ? round.position.x - me.position.x > 5 : std::fabs(round.position.x - me.position.x) < 1);
	}
}

// The lock's feedback (0x005e1b30): a TSLaserGuided lock is announced
// once locked, 35 ticks after the target was taken ("target_aquired" to
// the local player), and its destroyed target is "target_lost".
TEST(GameWorld_LockFeedback)
{
	Range range;
	range.templates.registerType(8, "SimpleObject");
	world::ObjectTemplate* launcher = range.templates.find("eu_ar_rifle");
	launcher->settings()["target.targetSystem"] = "TSLaserGuided";
	launcher->settings()["target.maxDistance"] = "450";
	launcher->settings()["target.lockAngle"] = "10";
	launcher->settings()["target.lockDelay"] = "0.5";
	launcher->settings()["target.loseLockDelay"] = "0.2";
	world::ObjectTemplate* tank = range.templates.create("SimpleObject", "painted");
	tank->children().push_back({"LaserTarget", {}, {}});
	range.templates.create("SimpleObject", "LaserTarget")->settings()["targetType"] = "TTLaser";
	CHECK(range.world->spawnPlayer(2) != nullptr);
	game::GameWorld::PlayerInfo me;
	range.world->playerInfo(range.world->localPlayer(), me);
	world::Object* enemy = range.objects.create(tank, {me.position.x, me.position.y + 1.6f, me.position.z + 60});
	enemy->setTeam(1);
	range.world->takeRadioHeard();
	std::vector<game::GameWorld::LockEvent> events;
	std::vector<std::string> heard;
	const auto run = [&](int ticks) {
		for (int i = 0; i < ticks; ++i) {
			range.world->tick(1 / 30.0f);
			for (const auto& e : range.world->takeLockEvents()) {
				events.push_back(e);
			}
			for (const auto& h : range.world->takeRadioHeard()) {
				heard.push_back(h.voice);
			}
		}
	};
	run(20); // locked after 0.5 s, not yet said
	CHECK(range.world->weaponLock(range.world->localPlayer()).state == 2);
	CHECK(events.empty());
	run(20); // 35 ticks after the target was taken
	CHECK(events.size() == 1 && events[0].kind == game::GameWorld::LockEvent::Acquired && events[0].target == enemy->id()
		&& events[0].system == 2);
	CHECK(heard.size() == 1 && heard[0] == "target_aquired");
	enemy->setDestroyed(true);
	run(15);
	CHECK(events.size() == 2 && events[1].kind == game::GameWorld::LockEvent::Lost);
	CHECK(heard.size() == 2 && heard[1] == "target_lost");
}

// The armour's effect levels (0x00654730) and the spectacular hit
// (0x006525f0 with the generator 0x0042b230).
TEST(GameWorld_ArmorEffectLevels)
{
	using Step = game::GameWorld::ArmorEffectStep;
	const std::vector<int> levels {50, 50, 0, -1, -100};
	int group = 0;
	CHECK(game::GameWorld::armorEffectStep(levels, 100, 60, 0, group) == Step::Keep); // above every level
	CHECK(game::GameWorld::armorEffectStep(levels, 60, 40, 0, group) == Step::Start && group == 50);
	CHECK(game::GameWorld::armorEffectStep(levels, 40, 30, 50, group) == Step::Keep); // no level passed
	CHECK(game::GameWorld::armorEffectStep(levels, 30, -20, 50, group) == Step::Start && group == -1);
	CHECK(game::GameWorld::armorEffectStep(levels, 30, 0, 50, group) == Step::Start && group == 0);
	CHECK(game::GameWorld::armorEffectStep(levels, -20, -100, -1, group) == Step::Start && group == -100);
	CHECK(game::GameWorld::armorEffectStep(levels, 40, 80, 50, group) == Step::Stop); // repaired past them all
	CHECK(game::GameWorld::armorPercent(40, 100, 50) == 40 && game::GameWorld::armorPercent(-10, 100, 50) == -20);
	CHECK(game::GameWorld::armorPercent(0.5f, 100, 50) == 1);
	// Object 3 can die spectacularly (the generator gives 1891080048 for
	// it), 1 and 2 never; for 3 a hit of over 20 % of armor.hitPoints is,
	// or one leaving over 25 %.
	CHECK(game::GameWorld::spectacularHit(3, 10, -15, 100));
	CHECK(!game::GameWorld::spectacularHit(3, 10, -5, 100));
	CHECK(game::GameWorld::spectacularHit(3, 50, 40, 100));
	CHECK(!game::GameWorld::spectacularHit(1, 10, -50, 100) && !game::GameWorld::spectacularHit(2, 10, -50, 100));
}

namespace {

// An effect bundle of one sprite system `height` up (an emitter there to find).
void effectBundle(world::ObjectTemplateManager& templates, const std::string& name, float height)
{
	world::ObjectTemplate* sparks = templates.create("SpriteParticleSystem", name + "_sparks");
	sparks->settings()["emitFrequency"] = "10";
	sparks->settings()["emitTime"] = "10";
	sparks->settings()["timeToLive"] = "0.5";
	sparks->settings()["textureName"] = "spark";
	templates.create("EffectBundle", name)->children().push_back({name + "_sparks", {0, height, 0}, {}});
}

const effects::Emitter* emitterAt(const game::GameWorld& world, float height, uint32_t anchor, bool running)
{
	for (const auto& e : world.effects().emitterList()) {
		if (e->anchor == anchor && e->stopped() != running && std::fabs(e->position().y - height) < 0.6f) {
			return e.get();
		}
	}
	return nullptr;
}

} // namespace

// A vehicle's armour effects (0x006549a0): the level's group as its percent
// falls past it, attached and following it above 0, left in the world at 0
// and -100; a spectacular hit takes the level's spectacular group; the
// wreck's -100 group goes off as it goes.
TEST(GameWorld_VehicleArmorEffects)
{
	for (int spectacular = 0; spectacular < 2; ++spectacular) {
		Range range;
		range.collisionTemplates.add("crate_mesh", boxMesh(1));
		range.templates.registerType(20, "PlayerControlObject");
		range.templates.registerType(21, "EffectBundle");
		range.templates.registerType(22, "SpriteParticleSystem");
		effectBundle(range.templates, "e_smoke", 5);
		effectBundle(range.templates, "e_boom", 10);
		effectBundle(range.templates, "e_boom_spec", 20);
		effectBundle(range.templates, "e_final", 30);
		range.templates.registerType(23, "Engine");
		range.templates.registerType(24, "SimpleObject");
		range.templates.create("Engine", "crate_engine")->settings()["engineType"] = "c_ETCar";
		world::ObjectTemplate* crate = range.templates.create("PlayerControlObject", "crate");
		makeMobile(*crate, "crate_mesh");
		crate->children().push_back({"crate_engine", {}, {}});
		crate->settings()["armor.maxHitPoints"] = "100";
		crate->settings()["armor.wreckHitPoints"] = "50";
		crate->settings()["armor.timeToStayAsWreck"] = "1";
		crate->settings()["armor.addArmorEffect.list"] =
			"50 e_smoke 0/0/0 0/0/0;0 e_boom 0/0/0 0/0/0;-100 e_final 0/0/0 0/0/0;";
		crate->settings()["armor.addArmorEffectSpectacular.list"] = "0 e_boom_spec 0/0/0 0/0/0;";
		world::ObjectTemplate* blast = range.templates.create("GenericProjectile", "shell");
		blast->components().push_back("DefaultDetonationComp");
		blast->settings()["detonation.explosionDamage"] = "60";
		blast->settings()["detonation.explosionRadius"] = "10";
		blast->settings()["detonation.explosionMaterial"] = "0";
		// Fillers until the next object's id can die spectacularly (or can't).
		world::ObjectTemplate* filler = range.templates.create("SimpleObject", "filler");
		for (int i = 0; i < 20; ++i) {
			const uint32_t next = range.objects.create(filler, {500, 0, 500})->id() + 1;
			if (game::GameWorld::spectacularHit(next, 50, -50, 100) == (spectacular != 0)) {
				break;
			}
		}
		const uint32_t id = range.objects.create(crate, {0, 1, 40})->id();
		range.world->tick(1 / 30.0f); // its body
		const int me = range.world->localPlayer() >= 0 ? range.world->localPlayer() : range.world->addPlayer("Me", 2, false);
		CHECK(range.world->detonate("shell", {0, 1, 41}, me));
		range.world->tick(1 / 30.0f);
		const float left = range.world->hitPoints(id);
		CHECK(left < 50 && left > 0);
		CHECK(emitterAt(*range.world, 6, id, true) != nullptr); // e_smoke, following the crate
		CHECK(range.world->detonate("shell", {0, 1, 41}, me));
		range.world->tick(1 / 30.0f);
		CHECK(range.world->hitPoints(id) <= 0);
		CHECK(emitterAt(*range.world, 6, id, true) == nullptr); // the smoke's emitter stopped
		const bool spec = game::GameWorld::spectacularHit(id, left, range.world->hitPoints(id), 100);
		std::fprintf(stderr, "  crate %u: %.1f then %.1f hit points, spectacular %d\n", id, left, range.world->hitPoints(id), spec);
		CHECK(spec == (spectacular != 0));
		CHECK((emitterAt(*range.world, 21, 0, true) != nullptr) == spec);  // e_boom_spec, in the world
		CHECK((emitterAt(*range.world, 11, 0, true) != nullptr) == !spec); // e_boom
		CHECK(emitterAt(*range.world, 31, 0, true) == nullptr);
		for (int i = 0; i < 40 && range.objects.find(id); ++i) {
			range.world->tick(1 / 30.0f);
		}
		CHECK(!range.objects.find(id));
		CHECK(emitterAt(*range.world, 31, 0, true) != nullptr); // e_final as the wreck went
	}
}

// Tracer rounds and the fire modes for the player's rifle: one round a
// press in single fire, the toggle (c_PIToggleFireRate) to automatic with
// its SwitchFireRate sound, every second round a tracer the effects draw.
TEST(GameWorld_TracersAndFireModes)
{
	Range range;
	range.templates.registerType(20, "Particle");
	range.templates.create("Particle", "p_tracer")->setGeometry("p_tracer_mesh");
	world::ObjectTemplate* bullet = range.templates.find("bullet");
	bullet->settings()["tracerTemplate"] = "p_tracer";
	bullet->settings()["tracerInterval"] = "2";
	bullet->settings()["tracerScaler"] = "10";
	bullet->settings()["tracerSizeModifier"] = "0.5";
	for (const char* rifle : {"eu_ar_rifle", "as_ar_rifle"}) {
		range.templates.find(rifle)->settings()["fire.addFireRate.list"] = "0 2";
	}
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const auto run = [&](int ticks, const game::SoldierInput& input) {
		range.world->setPlayerInput(input);
		for (int i = 0; i < ticks; ++i) {
			range.world->tick(1 / 30.0f);
		}
	};
	game::SoldierInput fire;
	fire.fire = true;
	game::GameWorld::PlayerInfo me;
	const auto fired = [&] {
		return range.world->playerWeapon() ? 30 - range.world->playerWeapon()->roundsInMag() : -1;
	};
	CHECK(range.world->playerFireMode() == game::kFireSingle);
	run(1, fire);
	CHECK(fired() == 1);
	size_t tracers = 0;
	float longest = 0;
	std::string geometry;
	range.world->effects().forEachMeshGroup([&](const std::string& g, bool, const std::vector<effects::MeshSprite>& list) {
		geometry = g;
		tracers += list.size();
		for (const auto& s : list) {
			longest = std::fmax(longest, std::sqrt(physics::lengthSquared(s.rotation.rows[2])));
		}
	});
	CHECK(geometry == "p_tracer_mesh" && tracers == 1 && std::fabs(longest - 5) < 0.01f); // 10 m flown, 5 m long
	run(29, fire);
	CHECK(fired() == 1); // held: still one
	run(1, {});
	game::SoldierInput toggle;
	toggle.toggleFireRate = true;
	range.world->takeWeaponSounds();
	run(1, toggle);
	const auto sounds = range.world->takeWeaponSounds();
	CHECK(range.world->playerFireMode() == game::kFireAutomatic);
	CHECK(std::count_if(sounds.begin(), sounds.end(), [](const auto& s) {
		return s.kind == game::GameWorld::WeaponSoundEvent::SwitchFireRate && s.weapon == "eu_ar_rifle";
	}) == 1);
	run(1, {});
	run(30, fire);
	CHECK(fired() == 11);
	const auto& flying = range.world->projectiles();
	const long withTracer = std::count_if(flying.begin(), flying.end(), [](const game::Projectile& p) { return p.tracer; });
	std::fprintf(stderr, "  %zu rounds flying, %ld with tracers\n", flying.size(), withTracer);
	CHECK(withTracer * 2 >= static_cast<long>(flying.size()) - 1 && withTracer * 2 <= static_cast<long>(flying.size()) + 1);
}

// Leaving a seat with hasRestrictedExit (0x005c7840): the exit location,
// else the same mirrored across x, where nothing stands between; with
// every place blocked the soldier stays in.
TEST(GameWorld_RestrictedExit)
{
	for (int blocked = 0; blocked < 3; ++blocked) {
		Range range;
		range.collisionTemplates.add("jeep_mesh", boxMesh(1));
		range.collisionTemplates.add("wall_mesh", wallMesh(4, 4));
		range.templates.registerType(20, "PlayerControlObject");
		range.templates.registerType(21, "SimpleObject");
		range.templates.registerType(23, "Engine");
		range.templates.create("Engine", "jeep_engine")->settings()["engineType"] = "c_ETCar";
		world::ObjectTemplate* jeep = range.templates.create("PlayerControlObject", "jeep");
		makeMobile(*jeep, "jeep_mesh");
		jeep->children().push_back({"jeep_engine", {}, {}});
		jeep->settings()["soldierExitLocation"] = "-3/0/0";
		jeep->settings()["hasRestrictedExit"] = "1";
		jeep->settings()["exitSpeedMod"] = "0.5";
		world::ObjectTemplate* wall = range.templates.create("SimpleObject", "wall");
		wall->settings()["collisionMesh"] = "wall_mesh";
		// Walls along z beside the jeep: none, on its left (-x), on both sides.
		if (blocked >= 1) {
			range.objects.create(wall, {-2, -1, 3}, {90, 0, 0});
		}
		if (blocked == 2) {
			range.objects.create(wall, {2, -1, 3}, {90, 0, 0});
		}
		range.objects.create(jeep, {0, 1, 3});
		range.world->startRound(); // the walls' collision
		CHECK(range.world->spawnPlayer(2) != nullptr);
		range.world->tick(1 / 30.0f); // the jeep's body
		game::SoldierInput use;
		use.use = true;
		range.world->setPlayerInput(use);
		range.world->tick(1 / 30.0f);
		range.world->setPlayerInput({});
		range.world->tick(1 / 30.0f);
		game::GameWorld::PlayerInfo me;
		CHECK(range.world->playerInfo(range.world->localPlayer(), me) && me.vehicle != 0);
		range.world->setPlayerInput(use);
		range.world->tick(1 / 30.0f);
		range.world->setPlayerInput({});
		range.world->tick(1 / 30.0f);
		range.world->playerInfo(range.world->localPlayer(), me);
		std::fprintf(stderr, "  blocked %d: %s at x %.2f\n", blocked, me.vehicle ? "still in" : "out", me.position.x);
		if (blocked == 2) {
			CHECK(me.vehicle != 0);
		} else {
			CHECK(me.vehicle == 0);
			CHECK(blocked == 0 ? me.position.x < -2.5f : me.position.x > 2.5f);
		}
	}
}

namespace {

// A jeep (a 2 m box) at 0/1/3 with a restricted exit 3 m to its left, the
// local player spawned beside it; `use` presses c_PIUse for a tick.
struct ExitRange : Range {
	world::ObjectTemplate* jeep = nullptr;
	uint32_t id = 0;
	ExitRange()
	{
		collisionTemplates.add("jeep_mesh", boxMesh(1));
		collisionTemplates.add("plate_mesh", boxMesh(1));
		templates.registerType(20, "PlayerControlObject");
		templates.registerType(21, "SimpleObject");
		templates.registerType(23, "Engine");
		templates.create("Engine", "jeep_engine")->settings()["engineType"] = "c_ETCar";
		jeep = templates.create("PlayerControlObject", "jeep");
		makeMobile(*jeep, "jeep_mesh");
		jeep->children().push_back({"jeep_engine", {}, {}});
		jeep->settings()["soldierExitLocation"] = "-3/0/0";
		jeep->settings()["hasRestrictedExit"] = "1";
	}
	void use()
	{
		game::SoldierInput press;
		press.use = true;
		world->setPlayerInput(press);
		world->tick(1 / 30.0f);
		world->setPlayerInput({});
		world->tick(1 / 30.0f);
	}
	game::GameWorld::PlayerInfo me()
	{
		game::GameWorld::PlayerInfo info;
		world->playerInfo(world->localPlayer(), info);
		return info;
	}
};

} // namespace

// The restricted exit's place must hold a standing soldier (interface
// 0xc4d7 slot 0x14 -> 0x00585d00 -> physics::soldierFits): a slab 0.5 m
// over the left place leaves the way to it clear but no room to stand, so
// the soldier leaves by the mirrored place.
TEST(GameWorld_RestrictedExitNeedsRoomToStand)
{
	for (int slab = 0; slab < 2; ++slab) {
		ExitRange range;
		world::ObjectTemplate* plate = range.templates.create("SimpleObject", "plate");
		plate->settings()["collisionMesh"] = "plate_mesh";
		if (slab) {
			range.objects.create(plate, {-3, 3.0f, 3}); // its bottom at 2 m, the place at 1.5
		}
		range.id = range.objects.create(range.jeep, {0, 1, 3})->id();
		range.world->startRound();
		CHECK(range.world->spawnPlayer(2) != nullptr);
		range.world->tick(1 / 30.0f);
		range.use();
		CHECK(range.me().vehicle == range.id);
		range.use();
		const auto me = range.me();
		std::fprintf(stderr, "  slab %d: %s at x %.2f\n", slab, me.vehicle ? "still in" : "out", me.position.x);
		CHECK(me.vehicle == 0);
		CHECK(slab ? me.position.x > 2.5f : me.position.x < -2.5f);
	}
}

// dontClearTeamOnExit (template +0x34f, 0x005c95b0): the empty vehicle
// keeps the team of the soldier who left; without it, it goes back to its
// own (the default team, here neutral).
TEST(GameWorld_DontClearTeamOnExit)
{
	for (int keep = 0; keep < 2; ++keep) {
		ExitRange range;
		range.jeep->settings()["hasRestrictedExit"] = "0";
		range.jeep->settings()["dontClearTeamOnExit"] = keep ? "1" : "0";
		range.id = range.objects.create(range.jeep, {0, 1, 3})->id();
		CHECK(range.world->spawnPlayer(2) != nullptr);
		range.world->tick(1 / 30.0f);
		range.use();
		CHECK(range.me().vehicle == range.id && range.objects.find(range.id)->team() == 2);
		range.use();
		range.world->tick(1 / 30.0f);
		CHECK(range.me().vehicle == 0);
		CHECK(range.objects.find(range.id)->team() == (keep ? 2 : 0));
	}
}

// The help manager's reach tests (0x00646cb0 cases 0x43 and 0x47): a
// vehicle whose entry reaches the soldier on foot (not once he sits in it).
TEST(GameWorld_VehicleInReachForTheHelp)
{
	ExitRange range;
	range.id = range.objects.create(range.jeep, {0, 1, 3})->id();
	CHECK(range.world->spawnPlayer(2) != nullptr);
	range.world->tick(1 / 30.0f);
	const int me = range.world->localPlayer();
	CHECK(range.world->vehicleInReach(me));
	CHECK(!range.world->ladderInReach(me));
	range.use();
	CHECK(range.me().vehicle == range.id);
	CHECK(!range.world->vehicleInReach(me));
}

// The wreck (0x00652380): timeToStayAsWreck (20 by default) drains its
// armour to -wreckHitPoints; with the AI running three times as fast, so a
// 3 s wreck goes after 1 s.
TEST(GameWorld_WreckDrainsFasterWithBots)
{
	for (int ai = 0; ai < 2; ++ai) {
		ExitRange range;
		range.jeep->settings()["armor.maxHitPoints"] = "100";
		range.jeep->settings()["armor.wreckHitPoints"] = "50";
		range.jeep->settings()["armor.timeToStayAsWreck"] = "3";
		range.world->setAiRunning(ai != 0);
		range.id = range.objects.create(range.jeep, {0, 1, 40})->id();
		range.world->tick(1 / 30.0f);
		range.world->applyDamage(range.id, 1000, -1);
		int ticks = 0;
		while (range.objects.find(range.id) && ticks < 200) {
			range.world->tick(1 / 30.0f);
			++ticks;
		}
		std::fprintf(stderr, "  ai %d: the wreck went after %d ticks\n", ai, ticks);
		CHECK(ai ? (ticks >= 29 && ticks <= 32) : (ticks >= 89 && ticks <= 92));
	}
}

// The wreck's own blast (0x00654120): drained to -wreckHitPoints, a wreck
// with armor.wreckExplosionDamage goes off with its wreckExplosion radius
// and material, hurting a soldier near it.
TEST(GameWorld_WreckExplodes)
{
	for (const bool blast : {false, true}) {
		ExitRange range;
		range.jeep->settings()["armor.maxHitPoints"] = "100";
		range.jeep->settings()["armor.wreckHitPoints"] = "50";
		range.jeep->settings()["armor.timeToStayAsWreck"] = "1";
		if (blast) {
			range.jeep->settings()["armor.wreckExplosionDamage"] = "60";
			range.jeep->settings()["armor.wreckExplosionRadius"] = "8";
			range.jeep->settings()["armor.wreckExplosionMaterial"] = "69";
		}
		range.id = range.objects.create(range.jeep, {0, 1, 40})->id();
		range.world->tick(1 / 30.0f);
		const int bot = range.world->addPlayer("Bot", 2, true);
		CHECK(range.world->spawnSoldier(bot));
		game::GameWorld::PlayerInfo info;
		range.world->playerInfo(bot, info);
		range.world->setSoldierPosition(info.soldier, {3, 0, 40});
		range.world->applyDamage(range.id, 1000, -1);
		for (int i = 0; i < 60 && range.objects.find(range.id); ++i) {
			range.world->tick(1 / 30.0f);
		}
		CHECK(!range.objects.find(range.id));
		const float hp = range.world->hitPoints(info.soldier);
		std::fprintf(stderr, "  wreck blast %d: the soldier at 3 m has %.1f\n", blast ? 1 : 0, hp);
		CHECK(blast ? hp < 100 : hp == 100);
	}
}

// A spectacular death: destroyOnSpectacularDeath (+0x165) removes the
// vehicle at its armour's next update with the level's effects, no wreck;
// hideChildrenOnSpectacularDeath (+0x164) keeps the wreck but hides every
// mesh part after the first (0x00651c60: zero matrices).
TEST(GameWorld_SpectacularDeathDestroysOrHides)
{
	for (int mode = 0; mode < 2; ++mode) {
		ExitRange range;
		range.templates.registerType(24, "GenericProjectile");
		range.jeep->settings()["armor.maxHitPoints"] = "100";
		range.jeep->settings()["armor.timeToStayAsWreck"] = "10";
		range.jeep->settings()[mode ? "armor.hideChildrenOnSpectacularDeath" : "armor.destroyOnSpectacularDeath"] = "1";
		world::ObjectTemplate* blast = range.templates.create("GenericProjectile", "shell");
		blast->components().push_back("DefaultDetonationComp");
		blast->settings()["detonation.explosionDamage"] = "500";
		blast->settings()["detonation.explosionRadius"] = "10";
		blast->settings()["detonation.explosionMaterial"] = "0";
		world::ObjectTemplate* filler = range.templates.create("SimpleObject", "filler");
		for (int i = 0; i < 20; ++i) {
			const uint32_t next = range.objects.create(filler, {500, 0, 500})->id() + 1;
			if (game::GameWorld::spectacularHit(next, 100, -100, 100)) {
				break;
			}
		}
		range.id = range.objects.create(range.jeep, {0, 1, 40})->id();
		range.world->tick(1 / 30.0f);
		const int me = range.world->addPlayer("Me", 2, false);
		std::vector<anim::Matrix> parts(3, anim::Matrix {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
		CHECK(!range.world->partTransforms(range.id, parts));
		CHECK(range.world->detonate("shell", {0, 1, 41}, me));
		range.world->tick(1 / 30.0f);
		range.world->tick(1 / 30.0f);
		if (mode == 0) {
			CHECK(!range.objects.find(range.id)); // gone at once
		} else {
			CHECK(range.objects.find(range.id) != nullptr); // a wreck
			CHECK(range.world->partTransforms(range.id, parts));
			CHECK(parts[0][0] == 1 && parts[1] == anim::Matrix {} && parts[2] == anim::Matrix {});
		}
	}
}

// A placed object's SupplyObject child (helipad_m_SupplyObject: workOn-
// Vehicles, teamFromClosestCP) works as a supply object of its own at the
// child's place; each 0.2 s update (0x005d1710 -> 0x005d0f20) it takes the
// team of the nearest control point.
TEST(GameWorld_StaticSupplyTakesTheClosestPointsTeam)
{
	Range range;
	range.templates.registerType(20, "Bundle");
	range.templates.registerType(21, "SupplyObject");
	world::ObjectTemplate* pad = range.templates.create("Bundle", "helipad_m");
	world::ObjectTemplate* supply = range.templates.create("SupplyObject", "helipad_m_SupplyObject");
	supply->settings()["radius"] = "5";
	supply->settings()["workOnSoldiers"] = "0";
	supply->settings()["workOnVehicles"] = "1";
	supply->settings()["healSpeed"] = "8";
	supply->settings()["teamFromClosestCP"] = "1";
	pad->children().push_back({"helipad_m_SupplyObject", {0, 1, 2}, {}});
	range.objects.create(pad, {80, 0, 0}); // nearer cp_pac (team 1) at 100/0/0
	range.world->tick(1 / 30.0f);
	const world::Object* made = nullptr;
	for (const auto& object : range.objects.objects()) {
		if (object->objectTemplate() && object->objectTemplate()->name() == "helipad_m_SupplyObject") {
			CHECK(made == nullptr); // one, not one per tick
			made = object.get();
		}
	}
	CHECK(made != nullptr);
	for (int i = 0; i < 10; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(made && std::fabs(made->position().z - 2) < 1e-4f && std::fabs(made->position().y - 1) < 1e-4f);
	CHECK(made && made->team() == 1);
}

// A seated engineer's kit ability (0x00599f30): an EU_Anti-Vehicle kit
// (hasRepairingAbility, abilityInVehicleRadius 8, abilityInVehicleStrength
// 0.075 a tick) repairs the vehicle it sits in, 2.25 per cent a second; a
// kit without an Ability component does nothing.
TEST(GameWorld_KitAbilityInAVehicle)
{
	for (int repairs = 0; repairs < 2; ++repairs) {
		Range range;
		range.collisionTemplates.add("jeep_mesh", boxMesh(1));
		range.templates.registerType(6, "Kit");
		range.templates.registerType(20, "PlayerControlObject");
		range.templates.registerType(23, "Engine");
		range.templates.create("Engine", "jeep_engine")->settings()["engineType"] = "c_ETCar";
		world::ObjectTemplate* jeep = range.templates.create("PlayerControlObject", "jeep");
		makeMobile(*jeep, "jeep_mesh");
		jeep->children().push_back({"jeep_engine", {}, {}});
		jeep->settings()["armor.maxHitPoints"] = "100";
		range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
		// Named as team 2's default kit, the local player's.
		world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
		kit->children().push_back({"eu_ar_rifle", {}, {}});
		kit->settings()["abilityInVehicleRadius"] = "8";
		kit->settings()["abilityInVehicleStrength"] = "0.075";
		if (repairs) {
			kit->settings()["ability.hasRepairingAbility"] = "1";
		}
		const uint32_t car = range.objects.create(jeep, {0, 1, 3})->id();
		range.world->startRound();
		CHECK(range.world->spawnPlayer(2) != nullptr);
		const int me = range.world->localPlayer();
		range.world->tick(1 / 30.0f);
		game::SoldierInput use;
		use.use = true;
		range.world->setPlayerInput(use);
		range.world->tick(1 / 30.0f);
		range.world->setPlayerInput({});
		game::GameWorld::PlayerInfo info;
		CHECK(range.world->playerInfo(me, info) && info.vehicle == car);
		range.world->setHitPoints(car, 50);
		for (int i = 0; i < 30; ++i) {
			range.world->tick(1 / 30.0f);
		}
		const float now = range.world->hitPoints(car);
		std::fprintf(stderr, "  %s: %.2f after a second\n", repairs ? "repairing" : "no ability", now);
		CHECK(repairs ? std::fabs(now - 52.25f) < 0.1f : now == 50);
	}
}

// A seat gun's barrels and camera frame in the layout (0x005d6d20: the
// gun's children named by fire.addBarrelName, in order; 0x005d5fb0 /
// 0x005f1a20: fireInCameraDof's camera frame; projectileStartPosition).
TEST(GameWorld_SeatGunBarrelsAndCameraFrame)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "PlayerControlObject");
	templates.registerType(2, "RotationalBundle");
	templates.registerType(3, "GenericFireArm");
	templates.registerType(4, "Bundle");
	templates.registerType(5, "Camera");
	world::ObjectTemplate* mech = templates.create("PlayerControlObject", "mech");
	mech->settings()["seatInformation"] = "mech 0/2/0 0/0/0";
	mech->children().push_back({"mech_camera", {0, 3, 0.5f}, {}});
	mech->children().push_back({"mech_turret", {0, 2, 0}, {}});
	templates.create("Camera", "mech_camera");
	world::ObjectTemplate* turret = templates.create("RotationalBundle", "mech_turret");
	turret->settings()["inputToYaw"] = "PIMouseLookX";
	turret->children().push_back({"mech_minigun", {0, 0, 1}, {}});
	world::ObjectTemplate* gun = templates.create("GenericFireArm", "mech_minigun");
	gun->settings()["fire.addBarrelName.list"] = "mech_barrel_L mech_barrel_R";
	gun->settings()["fire.projectileStartPosition"] = "0/0/0.5";
	gun->children().push_back({"mech_barrel_R", {1, 0, 0}, {}});
	gun->children().push_back({"mech_barrel_L", {-1, 0, 0}, {}});
	gun->children().push_back({"mech_other", {0, 5, 0}, {}});
	templates.create("Bundle", "mech_barrel_L");
	templates.create("Bundle", "mech_barrel_R");
	templates.create("Bundle", "mech_other");
	const uint32_t id = objects.create(mech, {0, 0, 0})->id();
	game::GameWorld world(templates, objects);
	auto seats = world.vehicleSeats(id);
	CHECK(seats.size() == 1);
	if (seats.size() == 1) {
		const auto& seat = seats[0];
		CHECK(seat.barrels.size() == 2 && seat.cameras.size() == 1 && !seat.fireInCameraDof && seat.hasStartPosition);
		CHECK(seat.barrels.size() == 2 && seat.barrels[0].x == 1 && seat.barrels[1].x == -1 && seat.barrels[0].z == 1);
		CHECK(!seat.cameras.empty() && std::fabs(seat.cameras[0].at.y - 3) < 1e-6f && !seat.cameras[0].ridesGun);
		CHECK(std::fabs(seat.yawPivot.y - 2) < 1e-6f);
	}
	gun->settings()["fire.fireInCameraDof"] = "1";
	game::GameWorld again(templates, objects);
	seats = again.vehicleSeats(id);
	CHECK(seats.size() == 1 && seats[0].fireInCameraDof);
}

// The death ragdoll's hit push (0x005f0f50): the hit bone's particle, the
// direction levelled by rd_hitUp 0, rd_hitForce (9000) x the round's
// hitImpactModifier (template +0x338, 1 by default) while it is fresh.
TEST(GameWorld_RagDollHitPush)
{
	using dice::hfe::game::ragDollHitBone;
	using dice::hfe::game::ragDollHitForce;
	CHECK(ragDollHitBone(3) == 2 && ragDollHitBone(8) == 7 && ragDollHitBone(0x11) == 0x10 && ragDollHitBone(0x21) == 0x20);
	CHECK(ragDollHitBone(0xc) == 0x2f && ragDollHitBone(-1) == 0x2f && ragDollHitBone(40) == 0x2f && ragDollHitBone(6) == 6);
	const dice::hfe::physics::Vec3 f = ragDollHitForce({0.6f, 0.8f, 0}, 0, 9000, 3);
	CHECK(std::fabs(f.x - 27000) < 1 && f.y == 0 && f.z == 0); // levelled, x3
	const dice::hfe::physics::Vec3 down = ragDollHitForce({0, -1, 0}, 1, 9000, 1);
	CHECK(dice::hfe::physics::lengthSquared(down) == 0); // straight down: nothing left
	CHECK(dice::hfe::physics::lengthSquared(ragDollHitForce({1, 0, 0}, 0, 0, 1)) == 0);
	dice::hfe::world::ObjectTemplateManager templates;
	templates.registerType(9, "GenericProjectile");
	dice::hfe::world::ObjectTemplate* round = templates.create("GenericProjectile", "ifv_round");
	CHECK(dice::hfe::game::ProjectileSettings::fromTemplate(*round).hitImpactModifier == 1);
	round->settings()["hitImpactModifier"] = "4";
	CHECK(dice::hfe::game::ProjectileSettings::fromTemplate(*round).hitImpactModifier == 4);
}

// target.disableGuidanceOnReload (LPTargetComp +0xb1, as_av / eu_av): the
// reload's start (0x005c0f40 -> the component's slot 0x48, 0x005e23b0)
// lets the wire-guided missile in flight go unguided, so it no longer
// turns after the aim. Without the flag the missile keeps following.
TEST(GameWorld_GuidanceLostOnReload)
{
	for (const bool disable : {false, true}) {
		Range range;
		world::ObjectTemplate* launcher = range.templates.find("eu_ar_rifle");
		launcher->settings()["projectileTemplate"] = "missile";
		launcher->settings()["velocity"] = "40";
		launcher->settings()["ammo.magSize"] = "1";
		launcher->settings()["ammo.nrOfMags"] = "4";
		launcher->settings()["ammo.reloadTime"] = "3";
		if (disable) {
			launcher->settings()["target.disableGuidanceOnReload"] = "1";
		}
		world::ObjectTemplate* missile = range.templates.create("GenericProjectile", "missile");
		missile->settings()["seek.targetType"] = "TTUnique";
		missile->settings()["seek.trackingDelay"] = "0.01";
		missile->settings()["follow.maxYaw"] = "0.45";
		missile->settings()["follow.maxPitch"] = "0.85";
		missile->settings()["gravityModifier"] = "0";
		missile->settings()["timeToLive"] = "CRD_NONE/10/0/0";
		CHECK(range.world->spawnPlayer(2) != nullptr);
		game::SoldierInput input;
		input.fire = true;
		input.aimPitch = 3;
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
		input.fire = false;
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
		input.reload = true; // a press: the reload starts
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
		input.reload = false;
		input.turn = 30;
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
		input.turn = 0;
		range.world->setPlayerInput(input);
		for (int i = 0; i < 30; ++i) {
			range.world->tick(1 / 30.0f);
		}
		CHECK(range.world->projectiles().size() == 1);
		if (!range.world->projectiles().empty()) {
			const game::Projectile& round = range.world->projectiles().front();
			const float heading = std::atan2(round.velocity.x, round.velocity.z);
			CHECK(round.wireLost == disable);
			if (disable) {
				CHECK(std::fabs(heading) < 0.01f); // flies straight on
			} else {
				CHECK(heading > 0.2f); // turned after the aim
			}
		}
	}
}

// fire.maxProjectilesInWorld (FireComp +0x74, 0x005f2bb0): the firearm
// never refuses to fire; its oldest rounds go (without going off) so that
// no more than the limit are in the world. A new kit's firearms start a
// new count.
TEST(GameWorld_MaxProjectilesInWorld)
{
	Range range;
	world::ObjectTemplate* mines = range.templates.find("eu_ar_rifle");
	mines->settings()["projectileTemplate"] = "mine";
	mines->settings()["velocity"] = "10";
	mines->settings()["fire.roundsPerMinute"] = "60";
	mines->settings()["ammo.magSize"] = "10";
	mines->settings()["fire.maxProjectilesInWorld"] = "2";
	world::ObjectTemplate* mine = range.templates.create("GenericProjectile", "mine");
	mine->settings()["gravityModifier"] = "0";
	mine->settings()["timeToLive"] = "CRD_NONE/100/0/0";
	mine->settings()["damage"] = "0";
	CHECK(range.world->spawnPlayer(2) != nullptr);
	std::vector<uint32_t> fired;
	for (int shot = 0; shot < 4; ++shot) {
		game::SoldierInput input;
		input.fire = true;
		input.aimPitch = 30;
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
		input.fire = false;
		range.world->setPlayerInput(input);
		for (int i = 0; i < 40; ++i) {
			range.world->tick(1 / 30.0f);
		}
		CHECK(range.world->projectiles().size() == static_cast<size_t>(std::min(shot + 1, 2)));
		if (!range.world->projectiles().empty()) {
			fired.push_back(range.world->projectiles().back().serial);
		}
	}
	// The two newest are left.
	CHECK(fired.size() == 4);
	if (fired.size() == 4 && range.world->projectiles().size() == 2) {
		CHECK(range.world->projectiles()[0].serial == fired[2]);
		CHECK(range.world->projectiles()[1].serial == fired[3]);
	}
	// Without a limit every round stays.
	mines->settings().erase("fire.maxProjectilesInWorld");
	for (int shot = 0; shot < 2; ++shot) {
		game::SoldierInput input;
		input.fire = true;
		input.aimPitch = 30;
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
		input.fire = false;
		range.world->setPlayerInput(input);
		for (int i = 0; i < 40; ++i) {
			range.world->tick(1 / 30.0f);
		}
	}
	CHECK(range.world->projectiles().size() == 4);
}

// armor.canBeRepaired 0 (template +0x14c; the Titans' cores, doors and
// fields): neither the repair test 0x004cc380 nor the armour's 0x00655a00
// lets a repair through, so the seated engineer of
// GameWorld_KitAbilityInAVehicle mends nothing. The test itself
// (GameWorld::repairable) also refuses a wreck without
// canBeRepairedWhenWreck (+0x14d).
TEST(GameWorld_CanBeRepaired)
{
	for (int repairable = 0; repairable < 2; ++repairable) {
		Range range;
		range.collisionTemplates.add("jeep_mesh", boxMesh(1));
		range.templates.registerType(6, "Kit");
		range.templates.registerType(20, "PlayerControlObject");
		range.templates.registerType(23, "Engine");
		range.templates.create("Engine", "jeep_engine")->settings()["engineType"] = "c_ETCar";
		world::ObjectTemplate* jeep = range.templates.create("PlayerControlObject", "jeep");
		makeMobile(*jeep, "jeep_mesh");
		jeep->children().push_back({"jeep_engine", {}, {}});
		jeep->settings()["armor.maxHitPoints"] = "100";
		jeep->settings()["armor.canBeRepaired"] = repairable ? "1" : "0";
		range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
		world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
		kit->children().push_back({"eu_ar_rifle", {}, {}});
		kit->settings()["abilityInVehicleRadius"] = "8";
		kit->settings()["abilityInVehicleStrength"] = "0.075";
		kit->settings()["ability.hasRepairingAbility"] = "1";
		const uint32_t car = range.objects.create(jeep, {0, 1, 3})->id();
		range.world->startRound();
		CHECK(range.world->spawnPlayer(2) != nullptr);
		range.world->tick(1 / 30.0f);
		game::SoldierInput use;
		use.use = true;
		range.world->setPlayerInput(use);
		range.world->tick(1 / 30.0f);
		range.world->setPlayerInput({});
		CHECK(range.world->repairable(car) == (repairable != 0));
		range.world->setHitPoints(car, 50);
		for (int i = 0; i < 30; ++i) {
			range.world->tick(1 / 30.0f);
		}
		const float now = range.world->hitPoints(car);
		CHECK(repairable ? std::fabs(now - 52.25f) < 0.1f : now == 50);
	}
}

// Material 0x29a (666) under a PlayerControlObject or a Parachute (the
// pods): 0x005c72f0 / 0x005cf750 destroy it (1e10, no attacker). The
// Titans' force fields carry it on every face of their vehicle LOD, so an
// enemy pod or vehicle meeting a standing shield goes with its riders;
// here a car falls onto a slab of it, and onto one of another material.
TEST(GameWorld_DeathMaterialDestroysVehicles)
{
	for (const char* parent : {"PlayerControlObject", "Parachute"}) {
		for (int deadly = 0; deadly < 2; ++deadly) {
			Range range;
			range.templates.registerType(6, "SimpleObject");
			range.templates.registerType(7, "PlayerControlObject");
			range.templates.registerType(8, "Parachute");
			range.collisionTemplates.add("slab_mesh", panelMesh({{-10, 0, -10}, {10, 0, -10}, {10, 0, 10}, {-10, 0, 10}},
														  {{{0, 2, 3}, 0}, {{0, 1, 2}, 0}}));
			range.collisionTemplates.add("car_mesh", boxMesh(1));
			world::ObjectTemplate* slab = range.templates.create("SimpleObject", "slab");
			slab->settings()["collisionMesh"] = "slab_mesh";
			slab->settings()["mapMaterial.0"] = deadly ? "666" : "42";
			range.objects.create(slab, {0, 0, 50});
			world::ObjectTemplate* car = range.templates.create(parent, "car");
			makeMobile(*car, "car_mesh");
			car->settings()["armor.maxHitPoints"] = "10000";
			const uint32_t id = range.objects.create(car, {0, 2, 50})->id();
			physics::Ground ground;
			ground.height = [](float, float) { return -5.0f; };
			ground.heightAndNormal = [](float, float, physics::Vec3& normal) {
				normal = {0, 1, 0};
				return -5.0f;
			};
			range.world->enablePhysics(range.files, range.collisionTemplates, ground);
			range.world->startRound();
			for (int i = 0; i < 60; ++i) {
				range.world->tick(1 / 30.0f);
			}
			const world::Object* o = range.objects.find(id);
			const float left = range.world->hitPoints(id);
			std::fprintf(stderr, "  %s on material %s: %.0f hit points left, y %.2f\n", parent, deadly ? "666" : "42", left,
				o ? o->position().y : -99.0f);
			CHECK(deadly ? left <= 0 : left > 9000); // the fall itself hurts a little
		}
	}
}

// Destroyable props (DestroyableObject, Props.cpp): at 0 hit points the
// armour's 0x00654120 wrecks one (its physics gone, 0x00651c60: a mesh
// of two geometries shows its second, one of one is hidden), its armour
// explosion (barrel_big_dest_c: 150 over 7 m) goes off at it, setting off
// its neighbour and hurting a soldier; with canBeDestroyed 0 the wreck
// stays, a draining one (the rail bars: timeToStayAsWreck 20) is removed
// after it; a new round has them whole (spawned once a round).
TEST(GameWorld_DestroyablePropsBreak)
{
	Range range;
	range.templates.registerType(6, "DestroyableObject");
	physics::CollisionMeshFile twoGeometries = boxMesh(0.5f);
	twoGeometries.geometries.push_back(boxMesh(0.2f).geometries[0]);
	range.collisionTemplates.add("barrel_mesh", twoGeometries);
	range.collisionTemplates.add("bar_mesh", boxMesh(0.5f));
	world::ObjectTemplate* barrel = range.templates.create("DestroyableObject", "barrel_big_dest_c");
	barrel->settings()["collisionMesh"] = "barrel_mesh";
	barrel->settings()["armor.maxHitPoints"] = "40";
	barrel->settings()["armor.explosionDamage"] = "150";
	barrel->settings()["armor.explosionRadius"] = "7";
	barrel->settings()["armor.explosionForce"] = "10";
	barrel->settings()["armor.canBeDestroyed"] = "0";
	barrel->settings()["armor.timeToStayAsWreck"] = "100";
	world::ObjectTemplate* bar = range.templates.create("DestroyableObject", "train_rail_bar_des");
	bar->settings()["collisionMesh"] = "bar_mesh";
	bar->settings()["armor.maxHitPoints"] = "1";
	const uint32_t first = range.objects.create(barrel, {0, 0.5f, 40})->id();
	const uint32_t second = range.objects.create(barrel, {3, 0.5f, 40})->id();
	const uint32_t rail = range.objects.create(bar, {-3, 0.5f, 40})->id();
	const uint32_t far = range.objects.create(barrel, {30, 0.5f, 40})->id();
	range.world->startRound();
	CHECK(range.world->spawnPlayer(2) != nullptr);
	CHECK(range.world->setSoldierPosition(range.world->playerObject(), {0, 0, 36}));
	range.world->tick(1 / 30.0f);
	// A segment through the first barrel meets it.
	CHECK(!range.world->lineOfSight({0, 0.5f, 38}, {0, 0.5f, 42}));
	range.world->setHitPoints(first, 0);
	CHECK(range.world->propWrecked(first) && range.world->propWrecked(second) && range.world->propWrecked(rail));
	CHECK(!range.world->propWrecked(far) && range.world->hitPoints(far) == 40);
	CHECK(range.world->propGeometry(first) == 1 && !range.objects.find(first)->hidden());
	CHECK(range.world->propGeometry(rail) == -1 && range.objects.find(rail)->hidden());
	CHECK(range.world->lineOfSight({0, 0.5f, 38}, {0, 0.5f, 42})); // its physics went
	game::GameWorld::PlayerInfo me;
	range.world->playerInfo(range.world->localPlayer(), me);
	std::fprintf(stderr, "  the soldier 4 m off: %.1f\n", range.world->hitPoints(me.soldier));
	CHECK(range.world->hitPoints(me.soldier) < 100);
	// The rail bar drains: removed after its 20 s; the barrels stay.
	range.world->setHitPoints(me.soldier, 100);
	for (int i = 0; i < 21 * 30; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(range.objects.find(rail)->hidden() && range.world->propWrecked(second) && !range.objects.find(second)->hidden());
	// The next round: whole again.
	range.world->startRound();
	CHECK(!range.world->propWrecked(first) && range.world->hitPoints(first) == 40 && !range.objects.find(rail)->hidden());
	CHECK(!range.world->lineOfSight({0, 0.5f, 38}, {0, 0.5f, 42}));
}

// target.disableFireWhenActiveMissile (LPTargetComp +0xb0, 0x005e2430):
// while the guided round flies, a new press fires nothing.
TEST(GameWorld_NoFireWhileTheMissileFlies)
{
	for (const bool disable : {false, true}) {
		Range range;
		world::ObjectTemplate* launcher = range.templates.find("eu_ar_rifle");
		launcher->settings()["projectileTemplate"] = "missile";
		launcher->settings()["velocity"] = "40";
		if (disable) {
			launcher->settings()["target.disableFireWhenActiveMissile"] = "1";
		}
		world::ObjectTemplate* missile = range.templates.create("GenericProjectile", "missile");
		missile->settings()["seek.targetType"] = "TTUnique";
		missile->settings()["gravityModifier"] = "0";
		missile->settings()["timeToLive"] = "CRD_NONE/10/0/0";
		CHECK(range.world->spawnPlayer(2) != nullptr);
		for (int press = 0; press < 3; ++press) {
			game::SoldierInput input;
			input.fire = true;
			input.aimPitch = 3;
			range.world->setPlayerInput(input);
			range.world->tick(1 / 30.0f);
			input.fire = false;
			range.world->setPlayerInput(input);
			for (int i = 0; i < 10; ++i) {
				range.world->tick(1 / 30.0f);
			}
		}
		CHECK(range.world->projectiles().size() == (disable ? 1u : 3u));
	}
}

// A vehicle in water (the armour's 0x006521d0): over waterLevel a timer
// from waterDamageDelay (3 s by default), past deepWaterLevel another from
// deepWaterDamageDelay; each run out takes its whole hpLost at once and
// starts again at a second.
TEST(GameWorld_WaterDamageDelays)
{
	Range range;
	range.collisionTemplates.add("jeep_mesh", boxMesh(1));
	range.templates.registerType(20, "PlayerControlObject");
	range.templates.registerType(23, "Engine");
	range.templates.create("Engine", "jeep_engine")->settings()["engineType"] = "c_ETCar";
	world::ObjectTemplate* jeep = range.templates.create("PlayerControlObject", "jeep");
	makeMobile(*jeep, "jeep_mesh");
	jeep->children().push_back({"jeep_engine", {}, {}});
	jeep->settings()["armor.maxHitPoints"] = "100";
	jeep->settings()["armor.waterLevel"] = "0.5";
	jeep->settings()["armor.deepWaterLevel"] = "1.5";
	jeep->settings()["armor.hpLostWhileInWater"] = "5";
	jeep->settings()["armor.hpLostWhileInDeepWater"] = "20";
	jeep->settings()["armor.deepWaterDamageDelay"] = "0.5";
	const uint32_t car = range.objects.create(jeep, {0, 1, 3})->id();
	range.world->startRound();
	range.world->tick(1 / 30.0f);
	range.world->setWaterLevel(range.objects.find(car)->position().y + 2);
	const auto run = [&](float seconds) {
		for (int i = 0; i < static_cast<int>(seconds * 30 + 0.5f); ++i) {
			range.world->tick(1 / 30.0f);
		}
	};
	run(0.4f);
	CHECK(range.world->hitPoints(car) == 100);
	run(0.2f); // the deep timer's 0.5 s
	CHECK(range.world->hitPoints(car) == 80);
	run(2.3f); // 2.9 s: deep at 1.5 and 2.5
	CHECK(range.world->hitPoints(car) == 40);
	run(0.2f); // the water timer's 3 s
	std::fprintf(stderr, "  after 3.1 s: %.0f\n", range.world->hitPoints(car));
	CHECK(range.world->hitPoints(car) == 35);
}

// fire.useDistanceCounter (0x005d5440, 0x005d4d80, 0x005b3e60): zoomed
// in, the launcher's range is the view's (999 m with nothing in the way),
// stepped by distanceStep and never under minimumDistance; its round goes
// off that far from its start. Not zoomed: no range, the round flies on.
TEST(GameWorld_AirburstDistance)
{
	for (const bool zoomed : {false, true}) {
		Range range;
		world::ObjectTemplate* launcher = range.templates.find("eu_ar_rifle");
		launcher->settings()["projectileTemplate"] = "rocket";
		launcher->settings()["velocity"] = "40";
		launcher->settings()["fire.useDistanceCounter"] = "1";
		launcher->settings()["fire.distanceStep"] = "2";
		launcher->settings()["fire.minimumDistance"] = "20";
		world::ObjectTemplate* rocket = range.templates.create("GenericProjectile", "rocket");
		rocket->settings()["gravityModifier"] = "0";
		rocket->settings()["timeToLive"] = "CRD_NONE/10/0/0";
		rocket->settings()["detonation.explosionDamage"] = "10";
		rocket->settings()["detonation.explosionRadius"] = "3";
		rocket->settings()["detonation.explosionMaterial"] = "0";
		CHECK(range.world->spawnPlayer(2) != nullptr);
		const int me = range.world->localPlayer();
		range.world->setZoomed(me, zoomed);
		game::SoldierInput input;
		input.aimPitch = 10;
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
		CHECK(range.world->burstDistance(me) == (zoomed ? 999 : 0));
		range.world->stepBurstDistance(me, -1000);
		CHECK(range.world->burstDistance(me) == (zoomed ? 20 : 0));
		range.world->stepBurstDistance(me, 1);
		CHECK(range.world->burstDistance(me) == (zoomed ? 22 : 0));
		// The wheel's PINextItem / PIPrevItem (0x005d5440): a step each.
		input.itemStep = 1;
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
		CHECK(range.world->burstDistance(me) == (zoomed ? 24 : 0));
		input.itemStep = -1;
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
		input.itemStep = 0;
		CHECK(range.world->burstDistance(me) == (zoomed ? 22 : 0));
		input.fire = true;
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
		input.fire = false;
		range.world->setPlayerInput(input);
		for (int i = 0; i < 15; ++i) {
			range.world->tick(1 / 30.0f);
		}
		CHECK(range.world->projectiles().size() == 1); // 20 m in half a second
		for (int i = 0; i < 10; ++i) {
			range.world->tick(1 / 30.0f);
		}
		CHECK(range.world->projectiles().size() == (zoomed ? 0u : 1u));
	}
}

// The ladder's camera (LadderContainerCamera: 0/1.5/-0.4 off the
// container, the mouse's yaw within -130..140 and the pitch within 60
// degrees of the ladder; 0x0059e730 pushes it on over the top).
TEST(GameWorld_LadderCamera)
{
	Range range;
	range.collisionTemplates.add("ladder_mesh",
		panelMesh({{-0.3f, 0, -0.05f}, {0.3f, 0, -0.05f}, {0.3f, 6, -0.05f}, {-0.3f, 6, -0.05f}},
			{{{0, 1, 2}, 0}, {{0, 2, 3}, 0}}));
	range.templates.registerType(8, "Ladder");
	world::ObjectTemplate* ladder = range.templates.create("Ladder", "ladder_5m_c");
	ladder->settings()["collisionMesh"] = "ladder_mesh";
	range.objects.create(ladder, {0, 0, 40});
	range.world->startRound();
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const int me = range.world->localPlayer();
	game::GameWorld::PlayerInfo info;
	range.world->playerInfo(me, info);
	range.world->setSoldierPosition(info.soldier, {0, 0, 39.5f});
	game::GameWorld::LadderView view;
	CHECK(!range.world->ladderView(me, view));
	game::SoldierInput climb;
	climb.aimPitch = 20;
	climb.throttle = 1;
	climb.turn = -info.yaw;
	bool on = false;
	for (int i = 0; i < 30 && !on; ++i) {
		range.world->setInput(me, climb);
		climb.turn = 0;
		range.world->tick(1 / 30.0f);
		on = range.world->onLadder(me);
	}
	CHECK(on);
	CHECK(range.world->ladderView(me, view));
	range.world->playerInfo(me, info);
	// 1.5 m over the container (the feet, a metre under the origin), 0.4 m
	// behind it along the ladder's facing (+z here).
	std::fprintf(stderr, "  camera %.2f/%.2f/%.2f, soldier %.2f/%.2f/%.2f, yaw %.1f\n", view.position.x, view.position.y,
		view.position.z, info.position.x, info.position.y, info.position.z, view.yaw);
	CHECK(std::fabs(view.position.y - (info.position.y + 1.5f)) < 0.05f);
	CHECK(std::fabs(view.pitch - 20) < 1e-4f);
	// The networked ladder (0x20000): its id (the first ladder, 1) and the
	// container's height over the bottom (0 here) over the 6 m length.
	CHECK(info.ladder == 1);
	CHECK(std::fabs(info.ladderPosition - info.position.y / 6.0f) < 0.02f);
	// The yaw: at most 250 degrees a second, within 140 to the right.
	climb.turn = 100;
	climb.throttle = 0;
	climb.aimPitch = 80;
	const float before = view.yaw;
	range.world->setInput(me, climb);
	range.world->tick(1 / 30.0f);
	range.world->ladderView(me, view);
	CHECK(std::fabs(std::remainder(view.yaw - before, 360.0f) - 250.0f / 30) < 0.01f);
	CHECK(view.pitch == 60);
	for (int i = 0; i < 30; ++i) {
		range.world->setInput(me, climb);
		range.world->tick(1 / 30.0f);
	}
	range.world->ladderView(me, view);
	CHECK(std::fabs(std::remainder(view.yaw - before, 360.0f) - 140) < 0.01f);
}

// An open seat's look (0x0058a6d0, 0x005890a0, 0x005c7cb0): the mouse
// turns the head up to 40 degrees, then the body within the seat's limits
// (seatLeftRotationLimit -90 / seatRightRotationLimit 90, the FAVs' open
// position); the seat has no camera, so the view is the seated soldier's
// own, along the seat plus the body's turn plus the twist.
TEST(GameWorld_OpenSeatLook)
{
	Range range;
	range.collisionTemplates.add("jeep_mesh", boxMesh(1));
	range.templates.registerType(20, "PlayerControlObject");
	range.templates.registerType(23, "Engine");
	range.templates.create("Engine", "jeep_engine")->settings()["engineType"] = "c_ETCar";
	world::ObjectTemplate* jeep = range.templates.create("PlayerControlObject", "jeep");
	makeMobile(*jeep, "jeep_mesh");
	jeep->settings()["seatInformation"] = "jeep 0/1/0 0/0/0";
	jeep->children().push_back({"jeep_engine", {}, {}});
	world::ObjectTemplate* open = range.templates.create("PlayerControlObject", "jeep_openPosition");
	open->settings()["seatInformation"] = "jeep_openPosition 0/1/-1 0/0/0";
	open->settings()["seatLeftRotationLimit"] = "-90";
	open->settings()["seatRightRotationLimit"] = "90";
	jeep->children().push_back({"jeep_openPosition", {0, 0, -1}, {}});
	const uint32_t car = range.objects.create(jeep, {0, 1, 3})->id();
	range.world->startRound();
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const int me = range.world->localPlayer();
	range.world->tick(1 / 30.0f);
	game::SoldierInput use;
	use.use = true;
	range.world->setPlayerInput(use);
	range.world->tick(1 / 30.0f);
	use = {};
	use.switchSeat = 2;
	range.world->setPlayerInput(use);
	range.world->tick(1 / 30.0f);
	game::GameWorld::PlayerInfo info;
	CHECK(range.world->playerInfo(me, info) && info.vehicle == car && info.seat == 1);
	const auto seats = range.world->vehicleSeats(car);
	CHECK(seats.size() == 2 && seats.size() == 2 && seats[1].leftLimit == -90 && seats[1].rightLimit == 90);
	game::GameWorld::LadderView view;
	CHECK(range.world->seatedSoldierView(me, view));
	const float ahead = view.yaw;
	const auto turnBy = [&](float degrees) {
		game::SoldierInput look;
		look.turn = degrees;
		range.world->setPlayerInput(look);
		range.world->tick(1 / 30.0f);
		range.world->seatedSoldierView(me, view);
		return std::remainder(view.yaw - ahead, 360.0f);
	};
	CHECK(std::fabs(turnBy(30) - 30) < 0.01f);  // the head
	CHECK(std::fabs(turnBy(30) - 70) < 0.01f);  // twist 40, the body 30
	CHECK(std::fabs(turnBy(100) - 130) < 0.01f); // the body at its 90
	CHECK(std::fabs(turnBy(-50) - 80) < 0.01f);  // the head alone back to -10, the body stays
	// What the soldier networkable carries seated (+0x25c, +0x278).
	CHECK(range.world->playerInfo(me, info) && std::fabs(info.seatBodyYaw - 90) < 0.01f
		&& std::fabs(info.seatTwist + 10) < 0.01f && info.ladder == 0);
	std::fprintf(stderr, "  open seat look ahead %.1f\n", ahead);
}

// The radar sources (0x00656550, the minimap's 0x00789f30 / 0x00791660):
// a sonar round of EU's lying 30 m from a PAC soldier: its expanding ring
// (radialRadar 0) passes him once a sweep; with showPlayers 0 never; the
// other team has no source.
TEST(GameWorld_RadarRingsPassEnemies)
{
	Range range;
	world::ObjectTemplate* sonar = range.templates.create("GenericProjectile", "sonar_projectile");
	sonar->settings()["ability.radarRadius"] = "40";
	sonar->settings()["ability.radialRadar"] = "0";
	sonar->settings()["collision.bouncing"] = "1";
	sonar->settings()["timeToLive"] = "CRD_NONE/9999/0/0";
	CHECK(range.world->spawnPlayer(2) != nullptr);
	const int me = range.world->localPlayer();
	const int bot = range.world->addPlayer("Bot", 1, true);
	CHECK(range.world->spawnSoldier(bot));
	game::GameWorld::PlayerInfo info;
	range.world->playerInfo(bot, info);
	const physics::Vec3 at = info.position + physics::Vec3 {30, 0, 0};
	CHECK(range.world->launchProjectile("sonar_projectile", at + physics::Vec3 {0, 0.5f, 0}, {0, -1, 0}, me));
	range.world->tick(1 / 30.0f);
	CHECK(range.world->radarSources(2).size() == 1 && range.world->radarSources(1).empty());
	CHECK(range.world->radarSources(2)[0].radius == 40 && !range.world->radarSources(2)[0].radial);
	int seen = 0;
	for (int i = 0; i < 48; ++i) { // 1.6 s: a sweep (2 pi / 4 rad/s)
		range.world->setInput(bot, {});
		range.world->tick(1 / 30.0f);
		for (const auto& contact : range.world->radarContacts(2)) {
			seen += contact.player == bot && !contact.vehicle;
		}
	}
	std::fprintf(stderr, "  the ring passed the bot on %d ticks\n", seen);
	CHECK(seen >= 1 && seen <= 7);
	CHECK(range.world->radarContacts(1).empty());
	// The pure test: radial and expanding.
	game::GameWorld::RadarSource ring;
	ring.radius = 40;
	ring.radial = false;
	CHECK(game::GameWorld::radarPasses(ring, {20, 0, 0}, 3.14159f));
	CHECK(!game::GameWorld::radarPasses(ring, {30, 0, 0}, 3.14159f));
	CHECK(!game::GameWorld::radarPasses(ring, {50, 0, 0}, 6.28f));
	ring.radial = true;
	CHECK(game::GameWorld::radarPasses(ring, {10, 0, 0}, 1.5708f)); // the bearing atan2(x, z)
	CHECK(!game::GameWorld::radarPasses(ring, {0, 0, 10}, 1.5708f));
	// showPlayers 0 (unl_sonar's): no soldier.
	sonar->settings()["ability.showPlayers"] = "0";
	Range other;
	other.templates.create("GenericProjectile", "sonar2")->settings() = sonar->settings();
	CHECK(other.world->spawnPlayer(2) != nullptr);
	const int bot2 = other.world->addPlayer("Bot", 1, true);
	CHECK(other.world->spawnSoldier(bot2));
	other.world->playerInfo(bot2, info);
	CHECK(other.world->launchProjectile("sonar2", info.position + physics::Vec3 {30, 0.5f, 0}, {0, -1, 0},
		other.world->localPlayer()));
	seen = 0;
	for (int i = 0; i < 48; ++i) {
		other.world->tick(1 / 30.0f);
		seen += static_cast<int>(other.world->radarContacts(2).size());
	}
	CHECK(seen == 0);
}

// detonateDelay / dudMaterial (0x005b4ff0, 0x005b54e0): a rocket hitting
// within its delay goes without its blast; later it goes off.
TEST(GameWorld_DudsDontGoOff)
{
	Range range;
	world::ObjectTemplate* rocket = range.templates.create("GenericProjectile", "rifle_rocket");
	rocket->settings()["damage"] = "1";
	rocket->settings()["detonation.explosionDamage"] = "500";
	rocket->settings()["detonation.explosionRadius"] = "6";
	rocket->settings()["detonation.explosionMaterial"] = "45";
	rocket->settings()["detonateDelay"] = "0.5";
	rocket->settings()["dudMaterial"] = "30";
	rocket->settings()["timeToLive"] = "CRD_NONE/10/0/0";
	rocket->settings()["gravityModifier"] = "0";
	const int bot = range.world->addPlayer("Bot", 1, true);
	CHECK(range.world->spawnSoldier(bot));
	game::GameWorld::PlayerInfo info;
	range.world->playerInfo(bot, info);
	// Fired into the ground 2 m from the bot: hit at once, a dud.
	CHECK(range.world->launchProjectile("rifle_rocket", info.position + physics::Vec3 {2, 0, 0}, {0, -60, 0}));
	for (int i = 0; i < 10; ++i) {
		range.world->tick(1 / 30.0f);
	}
	range.world->playerInfo(bot, info);
	CHECK(info.alive && range.world->projectilesInFlight() == 0);
	// From 40 m up at 60 m/s (0.65 s): past the delay, it goes off.
	CHECK(range.world->launchProjectile("rifle_rocket", info.position + physics::Vec3 {2, 40, 0}, {0, -60, 0}));
	for (int i = 0; i < 40; ++i) {
		range.world->tick(1 / 30.0f);
	}
	range.world->playerInfo(bot, info);
	CHECK(!info.alive);
}

// The lock tones (0x00657ce0 from the lock box 0x007e5a10): tracking
// starts weaponHud.firstLockSound, locked stops it and starts
// secondLockSound, the lock lost stops both.
TEST(GameWorld_LockTones)
{
	Range range;
	range.templates.registerType(8, "SimpleObject");
	world::ObjectTemplate* launcher = range.templates.find("eu_ar_rifle");
	launcher->settings()["target.targetSystem"] = "TSHeatSeeking";
	launcher->settings()["target.maxDistance"] = "450";
	launcher->settings()["target.lockAngle"] = "10";
	launcher->settings()["target.lockDelay"] = "1.5";
	launcher->settings()["target.loseLockDelay"] = "0.35";
	launcher->settings()["weaponHud.firstLockSound"] = "TargetTrack";
	launcher->settings()["weaponHud.secondLockSound"] = "TargetLock";
	world::ObjectTemplate* jet = range.templates.create("SimpleObject", "jet");
	jet->children().push_back({"HeatObject", {}, {}});
	range.templates.create("SimpleObject", "HeatObject")->settings()["targetType"] = "TTHeat";
	CHECK(range.world->spawnPlayer(2) != nullptr);
	game::GameWorld::PlayerInfo me;
	range.world->playerInfo(range.world->localPlayer(), me);
	world::Object* enemy = range.objects.create(jet, {me.position.x, me.position.y + 1.6f, me.position.z + 60});
	enemy->setTeam(1);
	const auto said = [&] {
		std::string out;
		for (const auto& tone : range.world->takeLockTones()) {
			out += (tone.play ? "+" : "-") + tone.sound + " ";
		}
		return out;
	};
	range.world->tick(1 / 30.0f);
	CHECK(said() == "-TargetLock +TargetTrack ");
	for (int i = 0; i < 46; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(said() == "-TargetTrack +TargetLock ");
	game::SoldierInput turn;
	turn.turn = 90;
	range.world->setPlayerInput(turn);
	range.world->tick(1 / 30.0f);
	range.world->setPlayerInput({});
	for (int i = 0; i < 12; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(said() == "-TargetTrack -TargetLock ");
	for (int i = 0; i < 10; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(said().empty());
}

// ObjectTemplate.setCheckObstruction (executor 0x004e236e; the pod
// launchers' -1.5 2 1) and its five rays (0x005c2310): the middle one from
// 1.5 m behind the gun to 2 m ahead, then the four moved by the radius on
// its right and up axes; anything met (here the ground, or a parked box
// past the gun's own) refuses the trigger (0x005c50e0, 0x005c2ee0).
TEST(GameWorld_FireObstruction)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "GenericFireArm");
	templates.registerType(2, "PlayerControlObject");
	templates.create("GenericFireArm", "pod_launcher");
	Console console;
	world::registerWorldConsoleMethods(console, templates, objects);
	std::string out;
	CHECK(console.execute("ObjectTemplate.activeSafe GenericFireArm pod_launcher", out) == ConsoleResult::Ok);
	CHECK(console.execute("ObjectTemplate.setCheckObstruction -1.5 2 1", out) == ConsoleResult::Ok);
	CHECK(templates.find("pod_launcher")->settings()["checkObstruction"] == "-1.5/2/1");

	io::FileManager files;
	physics::CollisionTemplateManager collisionTemplates;
	collisionTemplates.add("box_mesh", boxMesh(1));
	world::ObjectTemplate* box = templates.create("PlayerControlObject", "box");
	makeMobile(*box, "box_mesh");
	const uint32_t parked = objects.create(box, {0, 1, 10})->id();
	game::GameWorld world(templates, objects);
	world.enablePhysics(files, collisionTemplates,
		{[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& normal) {
			 normal = {0, 1, 0};
			 return 0.0f;
		 }});
	world.tick(1 / 30.0f); // the box's body
	const physics::Vec3 ahead {0, 0, 1}, right {1, 0, 0}, up {0, 1, 0};
	// Level at 1.5 m over flat ground, the sky clear: the lowest rays at 0.5 m.
	CHECK(!world.fireObstructed({0, 1.5f, 0}, ahead, right, up, -1.5f, 2, 1, 0));
	// At 0.8 m the lower corners' rays run under the ground.
	CHECK(world.fireObstructed({0, 0.8f, 0}, ahead, right, up, -1.5f, 2, 1, 0));
	// Aimed down: the middle ray meets the ground.
	CHECK(world.fireObstructed({0, 1.5f, 0}, {0, -1, 0}, right, {0, 0, 1}, -1.5f, 2, 1, 0));
	// A box 1.5 m ahead of the gun: met, unless it is the gun's own.
	CHECK(world.fireObstructed({0, 1.5f, 8.5f}, ahead, right, up, -1.5f, 2, 1, 0));
	CHECK(!world.fireObstructed({0, 1.5f, 8.5f}, ahead, right, up, -1.5f, 2, 1, parked));
}

// The close-to indications (0x0059a6c0, every 0.2 s on the server): a
// team object whose Radio has closeToIndicationDist within that plus the
// player's radius (on the plane with indicationDistance2D) posts 2/0x3e
// with its closeToIndicationType once, its going 2/0x3f; another team's
// counts for nothing; a mine (a projectile) of type 0 too; death posts
// 2/0x3f with -1.
TEST(GameWorld_CloseToIndication)
{
	Range range;
	range.templates.registerType(6, "SimpleObject");
	world::ObjectTemplate* titan = range.templates.create("SimpleObject", "eu_titan");
	titan->settings()["Radio.closeToIndicationDist"] = "250";
	titan->settings()["Radio.closeToIndicationType"] = "1";
	titan->settings()["Radio.indicationDistance2D"] = "1";
	const int me = range.world->addPlayer("Me", 2, false);
	CHECK(range.world->spawnSoldier(me));
	game::GameWorld::PlayerInfo info;
	range.world->playerInfo(me, info);
	// 150 m off and 100 m up: gathered (180 m, within the radius + 200)
	// and within 250 m on the plane.
	world::Object* ours = range.objects.create(titan, {info.position.x + 150, info.position.y + 100, info.position.z});
	ours->setTeam(2);
	world::Object* theirs = range.objects.create(titan, {info.position.x - 100, info.position.y, info.position.z});
	theirs->setTeam(1);
	const auto run = [&](float seconds) {
		std::vector<game::GameWorld::CloseToEvent> all;
		for (float t = 0; t < seconds; t += 1 / 30.0f) {
			range.world->tick(1 / 30.0f);
			for (const auto& e : range.world->takeCloseToEvents(me)) {
				all.push_back(e);
			}
		}
		return all;
	};
	auto events = run(0.5f);
	CHECK(events.size() == 1 && events[0].type == 1 && events[0].enter);
	CHECK(run(1).empty()); // still in: nothing more
	// Ours out of range: the type's last one goes (and nothing is left in
	// range at all: 0x0059a5f0's type -1).
	ours->setPosition({info.position.x + 600, info.position.y, info.position.z});
	events = run(0.5f);
	CHECK(events.size() == 1 && !events[0].enter && events[0].type == -1);
	// Two of type 1 in (each new one posts its 0x3e), one going: no leave
	// while the other stays.
	ours->setPosition({info.position.x + 100, info.position.y, info.position.z});
	world::Object* second = range.objects.create(titan, {info.position.x, info.position.y, info.position.z + 120});
	second->setTeam(2);
	events = run(0.5f);
	CHECK(events.size() == 2 && events[0].enter && events[1].enter && events[0].type == 1);
	second->setPosition({info.position.x, info.position.y, info.position.z + 900});
	CHECK(run(0.5f).empty());
	// Death: the list cleared with type -1.
	range.world->setHitPoints(info.soldier, 0);
	events = run(0.5f);
	CHECK(events.size() == 1 && !events[0].enter && events[0].type == -1);
}

// The bots' unlocks (AIDLL 0x100efce0 / 0x100f04a0 -> BF2142.exe's AI
// player adaptor slot 40, 0x008a5b90): the levels one over the best
// living human's owned mask for a kit of the same unlockIndex (at most
// 4); then two of the UB_First / UB_Second containers below those levels
// at random chosen and owned, the anti-air one kept aside (no air
// vehicle: not taken), UB_Player level 0 owned, a squad leader's one
// UB_SquadLeader container.
TEST(GameWorld_BotUnlocks)
{
	Range range;
	range.templates.registerType(6, "Kit");
	range.templates.registerType(7, "ItemContainer");
	for (const char* team : {"EU", "Pac"}) {
		world::ObjectTemplate* kit = range.templates.create("Kit", std::string(team) + "_Assault");
		kit->settings()["unlockIndex"] = "1";
		kit->children().push_back({"eu_ar_rifle", {}, {}});
		const auto add = [&](const std::string& name, const char* branch, int level, const char* item) {
			world::ObjectTemplate* c = range.templates.create("ItemContainer", name);
			if (branch) {
				c->settings()["unlockBranch"] = branch;
			}
			c->settings()["unlockLevel"] = std::to_string(level);
			c->children().push_back({item, {}, {}});
			kit->children().push_back({name, {}, {}});
		};
		const std::string base = std::string("Unlock") + team + "_Assault";
		for (int level = 0; level < 4; ++level) {
			add(base + "Light" + std::to_string(level), nullptr, level, "eu_ar_rifle");
		}
		add(base + "Medic0", "UB_Second", 0, "eu_ar_rifle");
		add(base + "Medic1", "UB_Second", 1, "aa_gun");
		add(base + "Player0", "UB_Player", 0, "eu_ar_rifle");
		add(base + "SquadLeader0", "UB_SquadLeader", 0, "eu_ar_rifle");
	}
	world::ObjectTemplate* aa = range.templates.create("GenericFireArm", "aa_gun");
	aa->settings()["aiTemplate"] = "eu_aa";
	// An item without an aiTemplate leaves its container out (0x008a57b0).
	range.templates.create("GenericFireArm", "eu_ar_rifle")->settings()["aiTemplate"] = "eu_ar_rifle";
	const auto antiAir = [](const std::string& ai) { return ai == "eu_aa"; };
	// A human of the other team owning Light0 and Light1 (mask 3) and
	// Medic0 (mask 1): the bot's levels are 4, 2, 1, 1.
	const int human = range.world->addPlayer("Human", 1, false);
	range.world->setOwnedUnlocks(human, {"UnlockPac_AssaultLight0", "UnlockPac_AssaultLight1", "UnlockPac_AssaultMedic0"});
	CHECK(range.world->spawnSoldier(human));
	const int bot = range.world->addPlayer("Bot", 2, true);
	range.world->setKit(bot, "EU_Assault", "us_light_soldier");
	const std::array<int, 4> levels = range.world->chooseBotUnlocks(bot, antiAir);
	CHECK(levels[0] == 4 && levels[1] == 2 && levels[2] == 1 && levels[3] == 1);
	std::vector<std::string> chosen = range.world->unlocksOf(bot);
	const std::vector<std::string> owned = range.world->unlocksOf(bot, true);
	std::fprintf(stderr, "  levels %d %d %d %d; chosen %s, %s\n", levels[0], levels[1], levels[2], levels[3],
		chosen.size() > 0 ? chosen[0].c_str() : "-", chosen.size() > 1 ? chosen[1].c_str() : "-");
	CHECK(chosen.size() == 2 && chosen[0] != chosen[1]);
	for (const std::string& c : chosen) {
		CHECK(c != "UnlockEU_AssaultMedic1" && c.find("SquadLeader") == std::string::npos);
	}
	CHECK(std::find(owned.begin(), owned.end(), "UnlockEU_AssaultPlayer0") != owned.end());
	CHECK(std::find(chosen.begin(), chosen.end(), "UnlockEU_AssaultPlayer0") == chosen.end());
	// No living human: level 1 each (only level 0 containers); a squad
	// leader also takes its squad leader container.
	range.world->setHitPoints(range.world->playerList()[0].soldier, 0);
	CHECK(range.world->joinSquad(bot, range.world->openSquad(2)));
	const std::array<int, 4> alone = range.world->chooseBotUnlocks(bot, antiAir);
	CHECK(alone[0] == 1 && alone[1] == 1 && alone[3] == 1);
	chosen = range.world->unlocksOf(bot);
	CHECK(std::find(chosen.begin(), chosen.end(), "UnlockEU_AssaultSquadLeader0") != chosen.end());
	CHECK(chosen.size() == 3); // Light0, Medic0 and the leader's
}

// The entry search's team test (0x005a98e0): an enemy's empty vehicle may
// be taken unless its template is lockToTeam (+0x350, the heavy IFV's);
// a neutral one always.
TEST(GameWorld_LockToTeamKeepsAnEnemysEmptyVehicleShut)
{
	struct Case {
		int team;
		bool locked;
		bool enters;
	};
	for (const Case c : {Case {1, false, true}, Case {1, true, false}, Case {0, true, true}, Case {2, true, true}}) {
		ExitRange range;
		range.jeep->settings()["hasRestrictedExit"] = "0";
		range.jeep->settings()["lockToTeam"] = c.locked ? "1" : "0";
		world::Object* jeep = range.objects.create(range.jeep, {0, 1, 3});
		jeep->setTeam(c.team);
		range.id = jeep->id();
		CHECK(range.world->spawnPlayer(2) != nullptr);
		range.world->tick(1 / 30.0f);
		CHECK(range.world->vehicleInReach(range.world->localPlayer()) == c.enters);
		range.use();
		CHECK((range.me().vehicle == range.id) == c.enters);
	}
}

// crewKitIndex (the enter 0x005cdd60 -> 0x0056cf50): the seat's index is
// the kit's geometry while its wearer sits there; the exit (0x00587540)
// puts the default back.
TEST(GameWorld_CrewKitIndexWhileSeated)
{
	ExitRange range;
	range.jeep->settings()["hasRestrictedExit"] = "0";
	range.jeep->settings()["crewKitIndex"] = "12";
	range.templates.registerType(25, "Kit");
	range.templates.create("Kit", "crew_kit");
	range.id = range.objects.create(range.jeep, {0, 1, 3})->id();
	const int crew = range.world->addPlayer("Crew", 2, false);
	range.world->setKit(crew, "crew_kit", "us_light_soldier");
	CHECK(range.world->spawnSoldier(crew));
	range.world->tick(1 / 30.0f);
	const auto info = [&] {
		game::GameWorld::PlayerInfo i;
		range.world->playerInfo(crew, i);
		return i;
	};
	const auto use = [&] {
		game::SoldierInput press;
		press.use = true;
		range.world->setInput(crew, press);
		range.world->tick(1 / 30.0f);
		range.world->setInput(crew, {});
		range.world->tick(1 / 30.0f);
	};
	const uint32_t kit = info().kit;
	CHECK(kit != 0);
	CHECK(range.world->kitGeometry(kit) == -1);
	use();
	CHECK(info().vehicle == range.id);
	CHECK(range.world->kitGeometry(kit) == 12);
	use();
	CHECK(info().vehicle == 0);
	CHECK(range.world->kitGeometry(kit) == -1);
}

// A PlayerControlObject's radius takes its boundingRadiusModifierWhenOccupied
// (slot 0x94 = 0x005c6be0 in 0x006f6b90); isSleeping (0x005f6ba0) puts its
// node to sleep from the start.
TEST(GameWorld_RadiusModifierAndSleepingStart)
{
	float radius[2] = {};
	for (int on = 0; on < 2; ++on) {
		ExitRange range;
		if (on) {
			range.jeep->settings()["boundingRadiusModifierWhenOccupied"] = "2";
			range.jeep->settings()["isSleeping"] = "1";
		}
		range.id = range.objects.create(range.jeep, {0, 1, 3})->id();
		range.world->tick(1 / 30.0f);
		radius[on] = range.world->objectRadius(range.id);
		CHECK(range.world->bodyAsleep(range.id) == (on == 1));
	}
	std::fprintf(stderr, "  radius %.3f, with the modifier %.3f\n", radius[0], radius[1]);
	CHECK(radius[0] > 1 && std::fabs(radius[1] - radius[0] - 2) < 1e-4f);
}

// The spawners' game events (0x005ad470): activateOnEvent holds a spawner
// off from the round's reset until its event, deActivateOnEvent switches
// one off; other events leave them be.
TEST(GameWorld_SpawnerGameEvents)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(2, "ObjectSpawner");
	world::ObjectTemplate* t = templates.create("ObjectSpawner", "pods");
	world::Object* object = objects.create(t, {});
	game::SpawnerSettings on;
	on.activateOnEvent = 1;
	game::SpawnerSettings off;
	off.deActivateOnEvent = 1;
	game::ObjectSpawner pods(*object, on), favs(*object, off);
	game::Random random;
	pods.reset(random);
	favs.reset(random);
	CHECK(!pods.active() && favs.active());
	pods.gameEvent(2);
	favs.gameEvent(2);
	CHECK(!pods.active() && favs.active());
	pods.gameEvent(1);
	favs.gameEvent(1);
	CHECK(pods.active() && !favs.active());
	t->settings()["deActivateOnEvent"] = "3";
	CHECK(game::SpawnerSettings::fromTemplate(*t).deActivateOnEvent == 3);
}

// A destroyed armoured object whose Radio has destroyedGameEvent 1
// (0x00654120 -> message 0x18 -> 0x0063c6a0) switches on the round's
// activateOnEvent 1 spawners.
TEST(GameWorld_DestroyedGameEventReachesTheSpawners)
{
	ExitRange range;
	range.templates.registerType(22, "ObjectSpawner");
	world::ObjectTemplate* spawner = range.templates.create("ObjectSpawner", "apc_pods_spawner");
	spawner->settings()["activateOnEvent"] = "1";
	range.objects.create(spawner, {50, 0, 50});
	range.jeep->settings()["armor.maxHitPoints"] = "100";
	range.jeep->settings()["Radio.destroyedGameEvent"] = "1";
	range.id = range.objects.create(range.jeep, {0, 1, 3})->id();
	range.world->startRound();
	range.world->tick(1 / 30.0f);
	CHECK(range.world->spawners().size() == 1 && !range.world->spawners()[0]->active());
	range.world->applyDamage(range.id, 1000); // the game logic's damage: a wreck (0x00654120)
	range.world->tick(1 / 30.0f);
	CHECK(range.world->spawners()[0]->active());
}

// toggleMouseLook (0x00611c80): the seat's camera turns on the mouse only
// while PIMouseLook is held, and springs back by 0.75 a tick without it.
TEST(GameWorld_ToggleMouseLookCamera)
{
	ExitRange range;
	range.jeep->settings()["hasRestrictedExit"] = "0";
	range.templates.registerType(23, "Camera");
	world::ObjectTemplate* camera = range.templates.create("Camera", "jeep_camera");
	camera->settings()["inputToYaw"] = "PIMouseLookX";
	camera->settings()["inputToPitch"] = "PIMouseLookY";
	camera->settings()["minRotation"] = "-90/-20/0";
	camera->settings()["maxRotation"] = "90/20/0";
	camera->settings()["maxSpeed"] = "90/90/0";
	camera->settings()["acceleration"] = "5000/5000/0";
	camera->settings()["toggleMouseLook"] = "1";
	range.jeep->children().push_back({"jeep_camera", {0, 1, 0}, {}});
	range.id = range.objects.create(range.jeep, {0, 1, 3})->id();
	CHECK(range.world->spawnPlayer(2) != nullptr);
	range.world->tick(1 / 30.0f);
	range.use();
	CHECK(range.me().vehicle == range.id);
	const auto yaw = [&] {
		world::Vec3 offset, rotation;
		return range.world->partPose(range.id, "jeep_camera", offset, rotation) ? rotation.x : -1000.0f;
	};
	game::SoldierInput look;
	look.turn = 5; // a full PIMouseLookX a tick
	for (int i = 0; i < 10; ++i) {
		range.world->setPlayerInput(look);
		range.world->tick(1 / 30.0f);
	}
	CHECK(std::fabs(yaw()) < 1e-4f);
	look.mouseLook = true;
	for (int i = 0; i < 10; ++i) {
		range.world->setPlayerInput(look);
		range.world->tick(1 / 30.0f);
	}
	const float turned = yaw();
	look.mouseLook = false;
	look.turn = 0;
	range.world->setPlayerInput(look);
	range.world->tick(1 / 30.0f);
	const float back = yaw();
	std::fprintf(stderr, "  free look %.2f, a tick after letting go %.2f\n", turned, back);
	CHECK(turned > 10);
	CHECK(std::fabs(back - turned * 0.75f) < 1e-3f);
}

// automaticYawStabilization (0x00564eb0): a gun on the driver's turret
// keeps the hull's heading while the turret turns.
TEST(GameWorld_AutomaticYawStabilization)
{
	ExitRange range;
	range.jeep->settings()["hasRestrictedExit"] = "0";
	range.jeep->settings()["seatInformation"] = "jeep 0/0.5/0 0/0/0";
	range.templates.registerType(24, "RotationalBundle");
	world::ObjectTemplate* turret = range.templates.create("RotationalBundle", "jeep_turret");
	turret->settings()["inputToYaw"] = "PIMouseLookX";
	range.jeep->children().push_back({"jeep_turret", {0, 1, 0}, {}});
	world::ObjectTemplate* cupola = range.templates.create("PlayerControlObject", "jeep_cupola");
	cupola->settings()["seatInformation"] = "jeep_cupola 0.5/1.5/-0.5 0/0/0";
	turret->children().push_back({"jeep_cupola", {0.5f, 0.5f, -0.5f}, {}});
	world::ObjectTemplate* mount = range.templates.create("RotationalBundle", "jeep_cupola_mount");
	mount->settings()["inputToYaw"] = "PIMouseLookX";
	mount->settings()["automaticYawStabilization"] = "1";
	cupola->children().push_back({"jeep_cupola_mount", {}, {}});
	range.id = range.objects.create(range.jeep, {0, 1, 3})->id();
	CHECK(range.world->spawnPlayer(2) != nullptr);
	range.world->tick(1 / 30.0f);
	range.use();
	CHECK(range.me().vehicle == range.id && range.me().seat == 0);
	const auto seats = range.world->vehicleSeats(range.id);
	int gunner = -1;
	for (size_t i = 0; i < seats.size(); ++i) {
		gunner = seats[i].name == "jeep_cupola" ? static_cast<int>(i) : gunner;
	}
	CHECK(gunner > 0);
	game::SoldierInput look;
	look.turn = 3;
	for (int i = 0; i < 10; ++i) {
		range.world->setPlayerInput(look);
		range.world->tick(1 / 30.0f);
	}
	float turretYaw = 0, cupolaYaw = 0;
	CHECK(range.world->seatYaw(range.id, 0, turretYaw) && range.world->seatYaw(range.id, gunner, cupolaYaw));
	std::fprintf(stderr, "  turret %.2f, cupola %.2f\n", turretYaw, cupolaYaw);
	CHECK(turretYaw > 20);
	CHECK(std::fabs(cupolaYaw + turretYaw) < 1e-3f);
}

// The heavy IFV's shield cells (0x00654d80): armoured children with
// mirrorParentDamageLevel follow the hull's damage level every 0.1 s; at
// none left they are wrecks and their onDestroyEvent (56,
// VehicleShieldDestroyed) goes; the HUD's cells (hudRepairCellNo,
// 0x007a1e50) show which stand.
TEST(GameWorld_ShieldCellsMirrorTheHull)
{
	ExitRange range;
	range.jeep->settings()["armor.maxHitPoints"] = "1000";
	range.jeep->settings()["armor.hitPoints"] = "1000";
	range.templates.registerType(26, "SupplyObject");
	world::ObjectTemplate* cell = range.templates.create("SupplyObject", "jeep_shield_generator");
	cell->components().push_back("Armor");
	cell->settings()["armor.maxHitPoints"] = "110";
	cell->settings()["armor.hitPoints"] = "110";
	cell->settings()["armor.mirrorParentDamageLevel"] = "1";
	cell->settings()["armor.hudRepairCellNo"] = "1";
	cell->settings()["Python.onDestroyEvent"] = "56";
	range.jeep->children().push_back({"jeep_shield_generator", {1, 0, 0}, {}});
	range.jeep->children().push_back({"jeep_shield_generator", {-1, 0, 0}, {}});
	range.id = range.objects.create(range.jeep, {0, 1, 3})->id();
	range.world->tick(1 / 30.0f);
	const auto cells = range.world->repairCells(range.id);
	CHECK(cells.size() == 2 && cells[0] && cells[1]);
	range.world->setHitPoints(range.id, 500);
	for (int i = 0; i < 9; ++i) {
		range.world->tick(1 / 30.0f);
	}
	CHECK(range.count("VehicleShieldDestroyed") == 0);
	range.world->applyDamage(range.id, 1000);
	for (int i = 0; i < 9; ++i) {
		range.world->tick(1 / 30.0f);
	}
	const auto down = range.world->repairCells(range.id);
	CHECK(down.size() == 2 && !down[0] && !down[1]);
	CHECK(range.count("VehicleShieldDestroyed") == 2);
}

// The detonation component's update waits for armingDelay (0x005b3e60
// calls 0x005e9f60 only past the template's +0x2fc ticks): an
// anti-personnel mine armed after 5 s spares an enemy standing at it
// until then (the shipped one arms after 4).
TEST(GameWorld_MinesArmBeforeTheyTrigger)
{
	Range range;
	world::ObjectTemplate* mine = nullptr;
	giveGadget(range, "unl_mine_antipersonnel", mine);
	mine->settings()["detonation.explosionDamage"] = "200";
	mine->settings()["detonation.explosionRadius"] = "8";
	mine->settings()["detonation.triggerRadius"] = "3";
	mine->settings()["detonation.triggerType"] = "MTYPco";
	mine->settings()["detonation.triggerAngle"] = "30";
	mine->settings()["detonation.explosionMaterial"] = "50";
	mine->settings()["armingDelay"] = "5";
	CHECK(range.world->spawnPlayer(2) != nullptr);
	throwGadget(range); // about 1.1 s
	CHECK(range.world->projectilesInFlight() == 1);
	for (int i = 0; i < 60 && !range.world->projectiles().front().resting; ++i) {
		range.world->tick(1 / 30.0f); // lying still: its facing is its frame
	}
	CHECK(range.world->projectiles().front().resting);
	const float age = range.world->projectiles().front().age;
	const int bot = range.world->addPlayer("Bot", 1, true);
	CHECK(range.world->spawnSoldier(bot));
	game::GameWorld::PlayerInfo info, me;
	range.world->playerInfo(bot, info);
	range.world->playerInfo(range.world->localPlayer(), me);
	// 1.5 m in front of where it lies (it faces the thrower's +z).
	const physics::Vec3 at = range.world->projectiles().front().position;
	range.world->setSoldierPosition(info.soldier, {at.x, me.position.y, at.z + 1.5f});
	while (range.world->projectilesInFlight() == 1 && range.world->projectiles().front().age < 4.9f) {
		range.world->tick(1 / 30.0f);
	}
	range.world->playerInfo(bot, info);
	CHECK(info.alive && range.world->projectilesInFlight() == 1);
	if (range.world->projectilesInFlight() == 1) {
		std::fprintf(stderr, "  lying from %.2f s, still there at %.2f s\n", age, range.world->projectiles().front().age);
	}
	int waited = 0;
	for (; waited < 150 && range.world->projectilesInFlight() == 1; ++waited) { // past 5 s: armed, and a trigger test since
		range.world->tick(1 / 30.0f);
	}
	std::fprintf(stderr, "  went off %d ticks after 4.9 s\n", waited);
	CHECK(waited >= 3 && waited <= 12); // armed at 5 s, its test 0.25 s on
	range.world->playerInfo(bot, info);
	CHECK(!info.alive && range.world->projectilesInFlight() == 0);
}

// detonation.detonateDistanceToTarget (0x005e9f60, the IFV's mine 3): a
// magnet round goes off as soon as its target lies nearer than that at a
// trigger test, short of it.
TEST(GameWorld_ProximityFuseOnTheTarget)
{
	for (const bool fused : {false, true}) {
		Range range;
		range.templates.registerType(8, "TargetObject");
		world::ObjectTemplate* mine = range.templates.create("GenericProjectile", "ifv_mine");
		mine->settings()["seek.targetType"] = "TTMagnet";
		mine->settings()["seek.maxDistLock"] = "50";
		mine->settings()["armingDelay"] = "0.2";
		mine->settings()["acceleration"] = "60";
		mine->settings()["deAcceleration"] = "50";
		mine->settings()["maxSpeed"] = "3";
		mine->settings()["follow.magneticBehaviour"] = "1";
		mine->settings()["gravityModifier"] = "0";
		mine->settings()["timeToLive"] = "CRD_NONE/9999/0/0";
		mine->settings()["detonation.explosionDamage"] = "10";
		mine->settings()["detonation.explosionRadius"] = "1";
		if (fused) {
			mine->settings()["detonation.detonateDistanceToTarget"] = "3";
		}
		world::ObjectTemplate* target = range.templates.create("TargetObject", "MagneticBait");
		target->settings()["targetType"] = "TTMagnet";
		target->settings()["overrideArmingSpeed"] = "1";
		world::ObjectTemplate* bait = range.templates.create("GenericProjectile", "bait_stationary");
		bait->settings()["gravityModifier"] = "0";
		bait->settings()["timeToLive"] = "CRD_NONE/9999/0/0";
		bait->children().push_back({"MagneticBait", {}, {}});
		CHECK(range.world->spawnPlayer(2) != nullptr);
		const int bot = range.world->addPlayer("Bot", 1, true);
		CHECK(range.world->spawnSoldier(bot));
		CHECK(range.world->launchProjectile("ifv_mine", {0, 3, 0}, {0, 0, 0}, range.world->localPlayer()));
		CHECK(range.world->launchProjectile("bait_stationary", {0, 3, 10}, {0, 0, 0}, bot));
		float last = -1;
		for (int i = 0; i < 300; ++i) {
			const game::Projectile* round = nullptr;
			for (const auto& p : range.world->projectiles()) {
				if (p.alive && p.settings && p.settings->name == "ifv_mine") {
					round = &p;
				}
			}
			if (!round) {
				break;
			}
			last = 10 - round->position.z;
			range.world->tick(1 / 30.0f);
		}
		std::fprintf(stderr, "  %s: went off %.2f m short of the bait\n", fused ? "fused" : "contact", last);
		CHECK(fused ? last > 1.5f && last < 3.2f : last >= 0 && last < 1.5f);
	}
}

// target.notifyProjectileOnLostTarget (0x005e1b30 with 0x005e1ee0): the
// lock's loss frees the last round fired (its target none, relock armed);
// without it the round keeps chasing.
TEST(GameWorld_LockLossFreesTheRound)
{
	for (const bool notify : {false, true}) {
		Range range;
		range.templates.registerType(8, "SimpleObject");
		world::ObjectTemplate* launcher = range.templates.find("eu_ar_rifle");
		launcher->settings()["target.targetSystem"] = "TSHeatSeeking";
		launcher->settings()["target.maxDistance"] = "450";
		launcher->settings()["target.lockAngle"] = "20";
		launcher->settings()["target.lockDelay"] = "0.2";
		launcher->settings()["target.loseLockDelay"] = "0.1";
		if (notify) {
			launcher->settings()["target.notifyProjectileOnLostTarget"] = "1";
		}
		launcher->settings()["projectileTemplate"] = "heat_missile";
		launcher->settings()["velocity"] = "20";
		world::ObjectTemplate* missile = range.templates.create("GenericProjectile", "heat_missile");
		missile->settings()["gravityModifier"] = "0";
		missile->settings()["timeToLive"] = "CRD_NONE/10/0/0";
		missile->settings()["seek.targetType"] = "TTHeat";
		missile->settings()["seek.reLockTime"] = "9999";
		missile->settings()["seek.maxAngleLock"] = "60";
		world::ObjectTemplate* jet = range.templates.create("SimpleObject", "jet");
		jet->children().push_back({"HeatObject", {}, {}});
		range.templates.create("SimpleObject", "HeatObject")->settings()["targetType"] = "TTHeat";
		CHECK(range.world->spawnPlayer(2) != nullptr);
		game::GameWorld::PlayerInfo me;
		range.world->playerInfo(range.world->localPlayer(), me);
		world::Object* enemy = range.objects.create(jet, {me.position.x, me.position.y + 1.6f, me.position.z + 150});
		enemy->setTeam(1);
		for (int i = 0; i < 10; ++i) {
			range.world->tick(1 / 30.0f);
		}
		CHECK(range.world->weaponLock(range.world->localPlayer()).state == 2);
		game::SoldierInput input;
		input.fire = true;
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
		CHECK(range.world->projectiles().size() == 1 && range.world->projectiles().front().lockedOn == enemy->id());
		input = {};
		input.turn = 90; // away: the lock goes after loseLockDelay
		range.world->setPlayerInput(input);
		range.world->tick(1 / 30.0f);
		range.world->setPlayerInput({});
		for (int i = 0; i < 10; ++i) {
			range.world->tick(1 / 30.0f);
		}
		CHECK(range.world->weaponLock(range.world->localPlayer()).state == 0);
		CHECK(!range.world->projectiles().empty());
		if (!range.world->projectiles().empty()) {
			const game::Projectile& round = range.world->projectiles().front();
			CHECK(notify ? round.lockedOn == 0 : round.lockedOn == enemy->id());
		}
	}
}

// seek.directionBonus (0x004cf290): with the AA missiles' 0.8 a round
// takes the target ahead of it over a nearer one off its heading; with 0
// the nearest.
TEST(GameWorld_DirectionBonusPrefersTheTargetAhead)
{
	for (const bool bonus : {false, true}) {
		Range range;
		range.templates.registerType(8, "SimpleObject");
		world::ObjectTemplate* missile = range.templates.create("GenericProjectile", "aa_missile");
		missile->settings()["gravityModifier"] = "0";
		missile->settings()["timeToLive"] = "CRD_NONE/10/0/0";
		missile->settings()["seek.targetType"] = "TTHeat";
		missile->settings()["seek.maxAngleLock"] = "40";
		if (bonus) {
			missile->settings()["seek.directionBonus"] = "0.8";
		}
		world::ObjectTemplate* jet = range.templates.create("SimpleObject", "jet");
		jet->children().push_back({"HeatObject", {}, {}});
		range.templates.create("SimpleObject", "HeatObject")->settings()["targetType"] = "TTHeat";
		world::Object* nearer = range.objects.create(jet, {10, 20, 25}); // about 22 degrees off, 27 m
		nearer->setTeam(1);
		world::Object* ahead = range.objects.create(jet, {0, 20, 150});
		ahead->setTeam(1);
		CHECK(range.world->launchProjectile("aa_missile", {0, 20, 0}, {0, 0, 40}));
		range.world->tick(1 / 30.0f);
		range.world->tick(1 / 30.0f);
		CHECK(!range.world->projectiles().empty());
		if (!range.world->projectiles().empty()) {
			const uint32_t taken = range.world->projectiles().front().lockedOn;
			CHECK(taken == (bonus ? ahead->id() : nearer->id()));
		}
	}
}

// blockInputOnRotBundle (0x005c4220, 0x00565180): while the seat's active
// weapon names the turret, the turret ignores the gunner's look.
TEST(GameWorld_BlockInputOnRotBundle)
{
	for (const bool blocked : {false, true}) {
		ExitRange range;
		range.jeep->settings()["hasRestrictedExit"] = "0";
		range.jeep->settings()["seatInformation"] = "jeep 0/0.5/0 0/0/0";
		range.templates.registerType(24, "RotationalBundle");
		world::ObjectTemplate* turret = range.templates.create("RotationalBundle", "jeep_turret");
		turret->settings()["inputToYaw"] = "PIMouseLookX";
		range.jeep->children().push_back({"jeep_turret", {0, 1, 0}, {}});
		world::ObjectTemplate* launcher = range.templates.create("GenericFireArm", "jeep_tv_launcher");
		if (blocked) {
			launcher->settings()["blockInputOnRotBundle"] = "JEEP_TURRET"; // names compare without case
		}
		turret->children().push_back({"jeep_tv_launcher", {0, 0, 1}, {}});
		range.id = range.objects.create(range.jeep, {0, 1, 3})->id();
		CHECK(range.world->spawnPlayer(2) != nullptr);
		range.world->tick(1 / 30.0f);
		range.use();
		CHECK(range.me().vehicle == range.id && range.me().seat == 0);
		game::SoldierInput look;
		look.turn = 3;
		for (int i = 0; i < 10; ++i) {
			range.world->setPlayerInput(look);
			range.world->tick(1 / 30.0f);
		}
		float yaw = 0;
		CHECK(range.world->seatYaw(range.id, 0, yaw));
		std::fprintf(stderr, "  %s: turret at %.1f\n", blocked ? "blocked" : "free", yaw);
		CHECK(blocked ? std::fabs(yaw) < 1e-4f : yaw > 20);
	}
}

// A LookAtObject (0x0064bc00): the part's matrix in its parent's frame at
// its template place, its z towards nameToLookAt's object at
// offsetToLookAt, its y = z x the parent's x.
TEST(GameWorld_LookAtObjectFacesItsTarget)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	templates.registerType(1, "PlayerControlObject");
	templates.registerType(2, "Spring");
	templates.registerType(3, "LookAtObject");
	world::ObjectTemplate* car = templates.create("PlayerControlObject", "car");
	car->children().push_back({"car_wheel", {-1, 0, -1}, {}});
	templates.create("Spring", "car_wheel")->children().push_back({"car_arm", {0.5f, 0, 0}, {10, 20, 0}});
	world::ObjectTemplate* arm = templates.create("LookAtObject", "car_arm");
	arm->settings()["nameToLookAt"] = "car";
	arm->settings()["offsetToLookAt"] = "-0.5/1/0";
	const uint32_t id = objects.create(car, {5, 0, 5}, {45, 0, 0})->id();
	game::GameWorld world(templates, objects);
	anim::Matrix local {};
	CHECK(world.partBone(id, "car_arm", local));
	const float h = std::sqrt(0.5f);
	// At (0.5, 0, 0) in the wheel's frame (its own 10/20 turn dropped); z
	// from (-0.5, 0, -1) to (-0.5, 1, 0).
	CHECK(std::fabs(local[12] - 0.5f) < 1e-5f && std::fabs(local[13]) < 1e-5f && std::fabs(local[14]) < 1e-5f);
	CHECK(std::fabs(local[8]) < 1e-5f && std::fabs(local[9] - h) < 1e-5f && std::fabs(local[10] - h) < 1e-5f);
	CHECK(std::fabs(local[0] - 1) < 1e-5f && std::fabs(local[5] - h) < 1e-5f && std::fabs(local[6] + h) < 1e-5f);
	CHECK(!world.partBone(id, "car_wheel", local)); // not a LookAtObject
	arm->settings()["nameToLookAt"] = "nothing";
	CHECK(!world.partBone(id, "car_arm", local));
}

// otherCollisionLod (0x006dc7b0, 0x006bb050): a crate asking for the
// soldiers' LOD (2) lands on a plate's LOD-2 faces, a metre above its
// vehicle LOD's, where one asking for nothing lands.
TEST(GameWorld_OtherCollisionLod)
{
	for (const bool soldierLod : {false, true}) {
		Range range;
		// A plate 2 m across at its origin, faced both ways.
		physics::CollisionMeshFile platform = panelMesh({{-2, 0, -2}, {2, 0, -2}, {2, 0, 2}, {-2, 0, 2}},
			{{{0, 1, 2}, 0}, {{0, 2, 3}, 0}, {{0, 2, 1}, 0}, {{0, 3, 2}, 0}});
		for (physics::CollisionLod& lod : platform.geometries[0].subGeometries[0].lods) {
			if (lod.collisionType == 2) {
				for (auto& v : lod.vertices) {
					v[1] += 1;
				}
				lod.boundsMin[1] += 1;
				lod.boundsMax[1] += 1;
			}
		}
		range.collisionTemplates.add("platform_mesh", platform);
		range.collisionTemplates.add("crate_mesh", boxMesh(0.4f));
		range.templates.registerType(6, "SimpleObject");
		range.templates.registerType(7, "PlayerControlObject");
		world::ObjectTemplate* base = range.templates.create("SimpleObject", "platform");
		base->settings()["collisionMesh"] = "platform_mesh";
		range.objects.create(base, {0, 1, 30});
		world::ObjectTemplate* crate = range.templates.create("PlayerControlObject", "crate");
		makeMobile(*crate, "crate_mesh");
		if (soldierLod) {
			crate->settings()["otherCollisionLod"] = "2";
		}
		const uint32_t id = range.objects.create(crate, {0, 5, 30})->id();
		range.world->startRound(); // the platform's collision
		for (int i = 0; i < 120; ++i) {
			range.world->tick(1 / 30.0f);
		}
		const world::Object* o = range.objects.find(id);
		CHECK(o != nullptr);
		if (o) {
			std::fprintf(stderr, "  otherCollisionLod %d: the crate rests at %.2f\n", soldierLod ? 2 : 1, o->position().y);
			CHECK(std::fabs(o->position().y - (soldierLod ? 2.4f : 1.4f)) < 0.2f);
		}
	}
}

// A world order marker's height (0x005b8b20): the higher of the terrain
// and the water under the point, plus half the marker's radius.
TEST(GameWorld_OrderMarkerHeight)
{
	io::FileManager files;
	physics::CollisionTemplateManager collisionTemplates;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	game::GameWorld world(templates, objects);
	world.enablePhysics(files, collisionTemplates,
		{[](float, float) { return 3.5f; }, [](float, float, physics::Vec3& normal) {
			 normal = {0, 1, 0};
			 return 3.5f;
		 }});
	CHECK(std::fabs(world.orderMarkerHeight({10, 3.5f, 20}, 2) - 4.5f) < 1e-4f);
	CHECK(std::fabs(world.orderMarkerHeight({10, 3.5f, 20}, 0) - 3.5f) < 1e-4f);
	world.setWaterLevel(6);
	CHECK(std::fabs(world.orderMarkerHeight({10, 3.5f, 20}, 2) - 7) < 1e-4f);
	world.setWaterLevel(1);
	CHECK(std::fabs(world.orderMarkerHeight({10, 3.5f, 20}, 1) - 4) < 1e-4f);
}

// The order's name tag (0x005b8b20 -> the manager's slot 0x2c): at the
// order's point with its marker 1.5 m or more off, else 1.5 m over the marker.
TEST(GameWorld_OrderTagByItsMarker)
{
	const auto same = [](const physics::Vec3& a, const physics::Vec3& b) {
		return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f && std::fabs(a.z - b.z) < 1e-4f;
	};
	CHECK(same(game::GameWorld::orderTagPoint({10, 3, 20}, 3.4f), {10, 4.9f, 20}));  // marker on the point
	CHECK(same(game::GameWorld::orderTagPoint({10, 3, 20}, 4.49f), {10, 5.99f, 20})); // just under 1.5 m
	CHECK(same(game::GameWorld::orderTagPoint({10, 3, 20}, 4.5f), {10, 3, 20}));     // a roof 1.5 m up
	CHECK(same(game::GameWorld::orderTagPoint({10, 3, 20}, 1.0f), {10, 3, 20}));     // the point over water
}

// The rocket's engine (0x005b3e60): the shipped eu_av fires eu_av_missile at
// 4 m/s; startDelay (0.2 s) later the engine pushes it by acceleration
// (100) along the wire-guided follow's heading up to maxSpeed (50). A
// round without a follow pushes only with hasDirection.
TEST(GameWorld_RocketEngine)
{
	for (const int kind : {0, 1, 2}) { // 0 the AV missile, 1 no follow, 2 no follow but hasDirection
		Range range;
		world::ObjectTemplate* av = range.templates.find("eu_ar_rifle");
		auto& s = av->settings();
		s.clear();
		for (const auto& [k, v] : std::vector<std::pair<const char*, const char*>> {{"fire.addFireRate", "0"},
				 {"fire.busyUntilButtonRelease", "1"}, {"ammo.magSize", "1"}, {"ammo.nrOfMags", "6"},
				 {"ammo.reloadTime", "5.7"}, {"target.maxDistance", "9999"}, {"target.setTargetAtMaxDistance", "1"},
				 {"target.targetSystem", "TSWireGuided"}, {"target.targetObjectTemplate", "UniqueTarget"},
				 {"projectileTemplate", "eu_av_missile"}, {"velocity", "4"}}) {
			s[k] = v;
		}
		world::ObjectTemplate* m = range.templates.create("GenericProjectile", "eu_av_missile");
		for (const auto& [k, v] : std::vector<std::pair<const char*, const char*>> {{"follow.maxYaw", "0.45"},
				 {"follow.maxPitch", "0.85"}, {"drag", "7"}, {"mass", "10"}, {"gravityModifier", "-0.1"},
				 {"timeToLive", "CRD_NONE/10/0/0"}, {"damage", "125"}, {"acceleration", "100"}, {"maxSpeed", "50"},
				 {"startDelay", "0.2"}}) {
			m->settings()[k] = v;
		}
		if (kind == 0) {
			m->settings()["seek.targetType"] = "TTUnique";
			m->settings()["seek.trackingDelay"] = "0.01";
			m->components().push_back("SeekClosestTargetComp");
			m->components().push_back("DefaultFollowComp");
		}
		if (kind == 2) {
			m->settings()["hasDirection"] = "1";
		}
		CHECK(range.world->spawnPlayer(2) != nullptr);
		game::SoldierInput input;
		input.fire = true;
		input.aimPitch = 5;
		float early = 0, late = 0;
		for (int i = 0; i < 45; ++i) {
			if (i == 2) {
				input.fire = false;
			}
			range.world->setPlayerInput(input);
			range.world->tick(1 / 30.0f);
			if (range.world->projectiles().empty()) {
				continue;
			}
			const float speed = std::sqrt(physics::lengthSquared(range.world->projectiles().front().velocity));
			if (i == 4) {
				early = speed;
			}
			late = speed;
		}
		CHECK(range.world->projectiles().size() == 1);
		CHECK(early < 5); // the start delay
		if (kind == 1) {
			CHECK(late < 5); // no follow, no hasDirection: no push
		} else {
			CHECK(late > 45 && late < 54); // up to maxSpeed
		}
	}
}

// fire.batchSize (FireComp 0x005f31d0, fire template +0x48): one round
// makes that many projectiles, the first the main one and the rest
// sub-projectiles straying by subProjectileDev (0x005c3cb0 -> 0x005f5540);
// the magazine loses one round. The shotguns fire 12 pellets, the EU AV 4
// rockets.
TEST(GameWorld_FireBatch)
{
	for (const int batch : {1, 12}) {
		Range range;
		world::ObjectTemplate* gun = range.templates.find("eu_ar_rifle");
		gun->settings()["fire.batchSize"] = std::to_string(batch);
		gun->settings()["deviation.subProjectileDev"] = "2";
		gun->components().push_back("SoldierDeviationComp");
		CHECK(range.world->spawnPlayer(2) != nullptr);
		game::SoldierInput fire;
		fire.fire = true;
		range.world->setPlayerInput(fire);
		range.world->tick(1 / 30.0f);
		range.world->setPlayerInput({});
		CHECK(static_cast<int>(range.world->projectiles().size()) == batch);
		CHECK(range.world->playerWeapon() && range.world->playerWeapon()->roundsInMag() == 29);
		// The pellets spread: not all along the same line.
		float least = 1, most = -1;
		for (const game::Projectile& p : range.world->projectiles()) {
			const float x = p.velocity.x / std::sqrt(physics::lengthSquared(p.velocity));
			least = std::min(least, x);
			most = std::max(most, x);
		}
		CHECK(batch == 1 ? most == least : most - least > 0.01f);
	}
}

// The repair tool (ammo.onlyActiveWhileFiring, 0x005d84a0): the first
// thing along the holder's look within abilityRadius, not a sphere round
// him: a big vehicle whose centre is out of reach is mended when its side
// is in front, and nothing behind the soldier is.
TEST(GameWorld_RepairToolRay)
{
	for (const int facing : {0, 180}) {
		Range range;
		range.collisionTemplates.add("hull_mesh", boxMesh(2));
		range.templates.registerType(6, "Kit");
		range.templates.registerType(20, "PlayerControlObject");
		world::ObjectTemplate* hull = range.templates.create("PlayerControlObject", "hull");
		makeMobile(*hull, "hull_mesh");
		hull->settings()["armor.maxHitPoints"] = "100";
		world::ObjectTemplate* tool = range.templates.create("GenericFireArm", "unl_repair");
		tool->settings()["itemIndex"] = "4";
		tool->settings()["ammo.magSize"] = "-1";
		tool->settings()["ammo.replenishingType"] = "RTHeal";
		tool->settings()["ammo.abilityRadius"] = "2.5";
		tool->settings()["ammo.abilityStrength"] = "2.5";
		tool->settings()["ammo.onlyActiveWhileFiring"] = "1";
		range.templates.find("eu_ar_rifle")->settings()["itemIndex"] = "3";
		world::ObjectTemplate* kit = range.templates.create("Kit", "EU_Assault");
		for (const char* item : {"eu_ar_rifle", "unl_repair"}) {
			kit->children().push_back({item, {}, {}});
		}
		// Its centre 4 m ahead: its side 2 m off, its centre past the
		// tool's 2.5 m.
		const uint32_t car = range.objects.create(hull, {0, 2, facing == 0 ? 4.0f : -4.0f})->id();
		range.world->startRound();
		CHECK(range.world->spawnPlayer(2) != nullptr);
		game::SoldierInput take;
		take.selectItem = 4;
		range.world->setPlayerInput(take);
		range.world->tick(1 / 30.0f);
		range.world->setHitPoints(car, 50);
		game::SoldierInput fire;
		fire.fire = true;
		fire.aimPitch = -10;
		for (int i = 0; i < 30; ++i) {
			range.world->setPlayerInput(fire);
			range.world->tick(1 / 30.0f);
		}
		const float now = range.world->hitPoints(car);
		CHECK(facing == 0 ? now > 51 : now == 50);
	}
}
