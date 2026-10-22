// Squad leader drones, with the game's own templates (needs
// $BF2142_GAME_DIR): the recon drone (unl_drone_recon) and the sentry
// drone (unl_drone_sentry) and their detonators from Weapons_server.zip.
#include "Console/Console.h"
#include "Game/Bots.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Physics/CollisionTemplateManager.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/WorldConsole.h"

#include <cmath>
#include <cstdlib>
#include <memory>

using namespace dice::hfe;

namespace {

// Flat ground, a spawn point per team 20 m apart (EU's at the origin
// facing +z, PAC's at z 20), and the drones' scripts run as the game does.
struct Field {
	io::FileManager files;
	Console console {&files};
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	physics::CollisionTemplateManager collisionTemplates;
	std::unique_ptr<game::GameWorld> world;
	bool loaded = false;

	Field()
	{
		const char* gameDir = std::getenv("BF2142_GAME_DIR");
		if (!gameDir) {
			return;
		}
		files.setRootPath(gameDir);
		files.addSearchPath("mods/bf2142");
		if (!files.mountArchive("Objects/Weapons_server.zip", "Objects\\Weapons")) {
			return;
		}
		world::registerWorldConsoleMethods(console, templates, objects);
		// The template properties the drones use (kept as text).
		world::registerUntypedTemplateProperties(console, templates,
			{"regulatePosX", "regulatePosY", "regulatePosZ", "destroyWhenTargetLost", "positionToleranceXZ",
				"warningDistance", "destroyDistance", "minHeightAdjustDistance", "armor.maxHitPoints", "armor.hitPoints",
				"armor.defaultMaterial", "armor.takesEmpDamage", "physicsType", "hasMobilePhysics", "fire.roundsPerMinute",
				"fire.projectileStartPosition", "ammo.magSize", "ammo.nrOfMags", "ammo.reloadTime", "velocity",
				"projectileTemplate", "itemIndex", "pulse.fov", "pulse.viewDistance", "pulse.revealTime", "damage",
				"minDamage", "distToStartLoseDamage", "distToMinDamage", "timeToLive", "material", "deviation.minDev"});
		std::string out;
		for (const char* script : {"Objects/Weapons/Handheld/unl_drone_recon/unl_drone_recon.con",
				 "Objects/Weapons/Handheld/Unl_Drone_Recon_Detonator/Unl_Drone_Recon_Detonator.con",
				 "Objects/Weapons/Handheld/unl_drone_sentry/unl_drone_sentry.con",
				 "Objects/Weapons/Handheld/Unl_Drone_Sentry_Detonator/Unl_Drone_Sentry_Detonator.con"}) {
			console.run(script, {}, out);
		}
		templates.registerType(1, "ControlPoint");
		templates.registerType(2, "SpawnPoint");
		templates.registerType(3, "Soldier");
		templates.registerType(6, "Kit");
		for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
			templates.create("Soldier", name)->settings()["armor.maxHitPoints"] = "100";
		}
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
		world->startRound();
		loaded = templates.find("unl_drone_recon_detonator") && templates.find("unl_drone_sentry_detonator");
	}

	// The local player (EU) leading a squad, with a kit of one detonator.
	int spawnLeader(const char* detonator)
	{
		world::ObjectTemplate* kit = templates.create("Kit", "EU_Assault");
		kit->children().push_back({detonator, {}, {}});
		world->spawnPlayer(2);
		world->joinSquad(world->localPlayer(), world->openSquad(2));
		return world->localPlayer();
	}
	void run(float seconds, const game::SoldierInput& input = {})
	{
		for (int i = 0; i < static_cast<int>(seconds * 30 + 0.5f); ++i) {
			world->setPlayerInput(input);
			world->tick(1 / 30.0f);
		}
	}
	// Takes the detonator (slot 8) and launches the drone.
	void launch()
	{
		game::SoldierInput input;
		input.selectItem = 8;
		run(1 / 30.0f, input);
		run(1 / 30.0f);
		input = {};
		input.fire = true;
		run(1 / 30.0f, input);
		run(1 / 30.0f);
	}
	physics::Vec3 at(uint32_t object) const
	{
		const world::Object* found = objects.find(object);
		return found ? physics::Vec3 {found->position().x, found->position().y, found->position().z} : physics::Vec3 {};
	}
};

float across(const physics::Vec3& a, const physics::Vec3& b)
{
	return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z));
}

} // namespace

TEST(Drones_HoverByTheirOwner)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	const int leader = field.spawnLeader("unl_drone_recon_detonator");
	game::GameWorld::PlayerInfo me;
	CHECK(field.world->playerInfo(leader, me) && me.squadLeader);
	field.launch();
	const uint32_t drone = field.world->drone(leader);
	CHECK(drone != 0);
	CHECK(field.objects.find(drone)->objectTemplate()->name() == "unl_drone_recon");

	// It settles within positionToleranceXZ (1.5 m) across the ground, at
	// the height of its target's position (the soldier's physics origin, a
	// metre over the feet), held against gravity a little below it.
	field.run(6);
	field.world->playerInfo(leader, me);
	physics::Vec3 hover = field.at(drone);
	CHECK(across(hover, me.position) < 1.6f);
	CHECK(std::abs(hover.y - (me.position.y + 1.0f)) < 0.3f);
	CHECK(!field.world->droneWarning(leader));

	// It follows its owner 8 m off (still inside warningDistance 10).
	field.world->setSoldierPosition(me.soldier, {8, 0, 0});
	field.run(6);
	field.world->playerInfo(leader, me);
	hover = field.at(drone);
	CHECK(across(hover, me.position) < 1.6f);

	// Moved 30 m at once, the owner is past destroyDistance (12 m): gone.
	field.world->setSoldierPosition(me.soldier, {38, 0, 0});
	field.run(0.1f);
	CHECK(field.world->drone(leader) == 0 && field.objects.find(drone) == nullptr);

	// A new one (the detonator has magazines without end); a second
	// launch replaces it: one per owner.
	field.run(1);
	field.launch();
	const uint32_t first = field.world->drone(leader);
	CHECK(first != 0);
	field.run(1);
	field.launch();
	CHECK(field.world->drone(leader) != 0 && field.world->drone(leader) != first && field.objects.find(first) == nullptr);

	// Shot down through its armour (120 hit points).
	CHECK(field.world->hitPoints(field.world->drone(leader)) == 120);
	field.world->setHitPoints(field.world->drone(leader), 0);
	CHECK(field.world->drone(leader) == 0);

	// Its owner dies: it is lost with its target.
	field.run(1);
	field.launch();
	CHECK(field.world->drone(leader) != 0);
	field.world->playerInfo(leader, me);
	field.world->setHitPoints(me.soldier, 0);
	field.run(0.1f);
	CHECK(field.world->drone(leader) == 0);
}

TEST(Drones_EmpDropsThem)
{
	// The EMP grenade's blast (500 at its centre, 120 a step, 3 steps for
	// the drone's armour): above level 1 the ForceObject stops holding the
	// drone (0x0062b750) and it falls; a step goes every 5 s
	// (empLevelTime's default), and below level 2 it climbs back.
	Field field;
	if (!field.loaded) {
		return;
	}
	world::ObjectTemplate* emp = field.templates.create("GenericProjectile", "emp_grenade");
	emp->components().push_back("DefaultDetonationComp");
	emp->settings()["detonation.isEMPWeapon"] = "1";
	emp->settings()["detonation.explosionRadius"] = "9";
	emp->settings()["detonation.explosionMaterial"] = "0";
	const int leader = field.spawnLeader("unl_drone_recon_detonator");
	field.launch();
	const uint32_t drone = field.world->drone(leader);
	field.run(6);
	const float held = field.at(drone).y;
	CHECK(field.world->detonate("emp_grenade", field.at(drone)));
	CHECK(field.world->empLevel(drone) == 3);
	field.run(1);
	CHECK(field.at(drone).y < held - 0.3f && field.world->drone(leader) == drone);
	field.run(9.5f); // level 3 - 10.5 / 5 = 0.9: held again
	CHECK(field.world->empLevel(drone) == 1);
	field.run(4);
	CHECK(std::fabs(field.at(drone).y - held) < 0.3f);
}

TEST(Drones_ReconPulseRevealsEnemies)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	const int leader = field.spawnLeader("unl_drone_recon_detonator");
	const int enemy = field.world->addPlayer("Pac", 1, true);
	CHECK(field.world->spawnSoldier(enemy)); // 20 m off
	CHECK(!field.world->isRevealed(enemy, 2));
	// The first pulse goes at launch: the enemy within 60 m shows to EU.
	field.launch();
	CHECK(field.world->isRevealed(enemy, 2) && !field.world->isRevealed(enemy, 1));
	CHECK(field.world->revealedPlayers(2) == std::vector<int> {enemy});
	// Out of reach, it shows for revealTime (3 s) and no longer.
	game::GameWorld::PlayerInfo info;
	field.world->playerInfo(enemy, info);
	field.world->setSoldierPosition(info.soldier, {0, 0, 150});
	field.run(2.5f);
	CHECK(field.world->isRevealed(enemy, 2));
	field.run(1);
	CHECK(!field.world->isRevealed(enemy, 2));
	// Back in reach, the next pulse (10 a minute: every 6 s) finds it again.
	field.world->setSoldierPosition(info.soldier, {0, 0, 30});
	field.run(3);
	CHECK(field.world->isRevealed(enemy, 2));
}

static game::GameWorld::PlayerInfo me(Field& field, int player)
{
	game::GameWorld::PlayerInfo info;
	field.world->playerInfo(player, info);
	return info;
}

TEST(Drones_SentryShootsEnemies)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	const int leader = field.spawnLeader("unl_drone_sentry_detonator");
	const int enemy = field.world->addPlayer("Pac", 1, true);
	CHECK(field.world->spawnSoldier(enemy)); // 20 m off
	const int mate = field.world->addPlayer("Eu", 2, true);
	CHECK(field.world->spawnSoldier(mate));
	field.launch();
	CHECK(field.world->drone(leader) != 0);
	// onlyAttackNetworkBattlefieldObjects (AIDLL 0x100ea470): the leader's
	// squad must have had the enemy reported in the last 3 s.
	game::GameWorld::PlayerInfo info;
	field.run(1.5f);
	CHECK(field.world->playerInfo(enemy, info) && info.deaths == 0 && field.world->hitPoints(info.soldier) == 100);
	field.world->reportBattlefieldObjects(leader, {enemy});
	CHECK(field.world->squadKnows(2, me(field, leader).squad, enemy));
	// 600 a minute at 10 (15 m) to 5 (40 m) a round: 100 hit points go
	// within a couple of seconds; the kill is the owner's.
	field.run(2.5f);
	CHECK(field.world->playerInfo(enemy, info) && info.deaths == 1);
	game::GameWorld::PlayerInfo me;
	CHECK(field.world->playerInfo(leader, me) && me.kills == 1);
	CHECK(field.world->playerInfo(mate, info) && info.alive && field.world->hitPoints(info.soldier) == 100);
}

TEST(Drones_BotLeadersSendOutSentries)
{
	// A bot leading a squad, with a rifle and the sentry drone's detonator,
	// sees an enemy 30 m off: it sends out the drone, then takes its rifle.
	// The detonator (SentryDroneDetonator: FireAndForget, strength 20
	// against infantry) outscores the rifle while the bot faces the enemy;
	// the trigger waits the reaction time, (1 - 0.4)(1.5 + U[0,1]) s
	// (AIDLL 0x100777a0); with the drone out it scores nothing.
	Field field;
	if (!field.loaded) {
		return;
	}
	field.templates.registerType(4, "GenericFireArm");
	world::ObjectTemplate* rifle = field.templates.create("GenericFireArm", "eu_ar_rifle");
	rifle->settings()["itemIndex"] = "3";
	world::ObjectTemplate* kit = field.templates.create("Kit", "EU_Assault");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"unl_drone_sentry_detonator", {}, {}});
	Console console;
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	game::BotManager bots(*field.world, settings);
	const int bot = field.world->addPlayer("Eu", 2, true);
	field.world->joinSquad(bot, field.world->openSquad(2));
	CHECK(field.world->spawnSoldier(bot));
	const int enemy = field.world->addPlayer("Pac", 1, false);
	CHECK(field.world->spawnSoldier(enemy));
	game::GameWorld::PlayerInfo info;
	field.world->playerInfo(enemy, info);
	field.world->setSoldierPosition(info.soldier, {0, 0, 30});
	for (int i = 0; i < 30 * 3; ++i) {
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
	}
	CHECK(field.world->drone(bot) != 0);
	CHECK(field.world->playerInfo(bot, info) && info.inHandSlot == 3);
}
