// Round simulation: control points and object spawners.
#include "Console/Console.h"
#include "Game/Bots.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Physics/CollisionTemplateManager.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
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
	for (int i = 0; i < 30 * 6 && range.world->playerWeapon()->magsLeft() == magsBefore; ++i) {
		range.world->tick(1 / 30.0f);
	}
	range.world->setPlayerInput({});
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
	CHECK(std::abs(info.position.z - 31) < 1.0f);

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
	// eu_av_missile: seek.targetType TTUnique, follow.maxYaw 0.45 rad/s.
	// Fired north, the shooter then turns 30 degrees right: the missile
	// turns after the aim, no faster than 0.45 rad a second.
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
		// About a second of turning at 0.45 rad/s, short of the 30 degrees (0.52).
		CHECK(heading > 0.35f && heading < 0.5f);
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
