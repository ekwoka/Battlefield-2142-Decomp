// The commander, with the game's own templates where $BF2142_GAME_DIR is
// set (Weapons_server.zip: the commander's assets commander_emp_strike,
// commander_orbital_strike_64, commander_uav_drone_control and
// commander_movement_analyst, their RemoteControlledObjects, and the
// supply_crate); the assignment and the orders run without them.
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
#include <string>
#include <vector>

using namespace dice::hfe;

namespace {

using Ability = game::GameWorld::CommanderAbility;

// Flat ground, a control point and a spawn point per team (PAC's at z 20,
// EU's at the origin), and the commander's templates as the game loads
// them.
struct Field {
	io::FileManager files;
	Console console {&files};
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	physics::CollisionTemplateManager collisionTemplates;
	std::unique_ptr<game::GameWorld> world;
	std::vector<std::pair<std::string, game::ScriptArgs>> events;
	bool loaded = false;

	Field()
	{
		if (const char* gameDir = std::getenv("BF2142_GAME_DIR")) {
			files.setRootPath(gameDir);
			files.addSearchPath("mods/bf2142");
			if (files.mountArchive("Objects/Weapons_server.zip", "Objects\\Weapons")) {
				world::registerWorldConsoleMethods(console, templates, objects);
				world::registerUntypedTemplateProperties(console, templates,
					{"rcType", "minTimeBetweenActivation", "uavVehicleTemplate", "armor.maxHitPoints", "armor.hitPoints",
						"armor.defaultMaterial", "armor.takesEmpDamage", "physicsType", "hasMobilePhysics",
						"fire.roundsPerMinute", "fire.burstSize", "fire.batchSize", "fire.fireLaunchDelay",
						"deviation.subProjectileDev", "ammo.magSize", "ammo.nrOfMags", "velocity", "projectileTemplate",
						"detonation.explosionDamage", "detonation.explosionRadius", "detonation.explosionMaterial",
						"detonation.isEMPWeapon", "damage", "minDamage", "timeToLive", "gravityModifier", "material",
						"seek.targetType", "radius", "healSpeed", "refillAmmoSpeed", "sharedStorageSize",
						"healStorageSize", "ammoStorageSize", "destroyWhenEmptyStorage", "workOnVehicles"});
				std::string out;
				const std::string base = "Objects/Weapons/";
				for (const char* script : {"common/RemoteControlledObjects/OrbitalStrikeControlObjectUS.con",
						 "common/RemoteControlledObjects/ArtilleryControlObjectUS.con",
						 "common/RemoteControlledObjects/UAVControlObject.con",
						 "common/RemoteControlledObjects/SatelliteControlObject.con",
						 "stationary/commander_EMP_strike/commander_EMP_strike.con",
						 "stationary/commander_orbital_Strike/commander_orbital_Strike.con",
						 "stationary/commander_orbital_Strike/commander_orbital_strike_64.con",
						 "stationary/commander_uav_drone_control/commander_uav_drone_control.con",
						 "stationary/commander_movement_analyst/commander_movement_analyst.con",
						 "stationary/commander_supplybox/supply_crate.con"}) {
					console.run(base + script, {}, out);
				}
				loaded = templates.find("commander_emp_strike") && templates.find("commander_orbital_strike_64")
					&& templates.find("commander_uav_drone_control") && templates.find("commander_movement_analyst")
					&& templates.find("supply_crate") && templates.find("OrbitalStrikeControlObjectUS");
			}
		}
		templates.registerType(1, "ControlPoint");
		templates.registerType(2, "SpawnPoint");
		templates.registerType(3, "Soldier");
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
		if (loaded) {
			world->enablePhysics(files, collisionTemplates,
				{[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& normal) {
					 normal = {0, 1, 0};
					 return 0.0f;
				 }});
		}
		world->setEventSink([this](const std::string& event, const game::ScriptArgs& args) {
			events.emplace_back(event, args);
		});
		world->startRound();
	}

	void run(float seconds)
	{
		for (int i = 0; i < static_cast<int>(seconds * 30 + 0.5f); ++i) {
			world->tick(1 / 30.0f);
		}
	}
	// An asset of the team, set down as its spawner would (teamOnVehicle).
	uint32_t place(const char* name, int team, float x, float z)
	{
		world::Object* asset = objects.create(templates.find(name), {x, 0, z});
		asset->setTeam(team);
		run(1.1f); // looked through once a second
		return asset->id();
	}
	// A soldier of the team at a place (its feet).
	int soldier(int team, float x, float z)
	{
		const int id = world->addPlayer(team == 1 ? "Pac" : "Eu", team, true);
		world->spawnSoldier(id);
		game::GameWorld::PlayerInfo info;
		world->playerInfo(id, info);
		world->setSoldierPosition(info.soldier, {x, 0, z});
		return id;
	}
	bool alive(int player) const
	{
		game::GameWorld::PlayerInfo info;
		return world->playerInfo(player, info) && info.alive;
	}
};

} // namespace

TEST(Commander_OnePerTeam)
{
	Field field;
	game::GameWorld& world = *field.world;
	const int a = world.addPlayer("A", 2, false);
	const int b = world.addPlayer("B", 2, true);
	const int c = world.addPlayer("C", 1, true);
	// Only once the game mode enables it (sh_setEnableCommander).
	CHECK(!world.setCommander(2, a) && world.commander(2) == -1);
	world.setCommanderEnabled(true);
	// The first applicant takes over; the second waits.
	world.joinSquad(a, world.openSquad(2));
	world.applyForCommander(a);
	world.applyForCommander(b);
	CHECK(world.commander(2) == a && world.commander(1) == -1);
	game::GameWorld::PlayerInfo info;
	CHECK(world.playerInfo(a, info) && info.commander && info.squad == 0); // it left its squad
	CHECK(!world.joinSquad(a, 1));
	CHECK(!field.events.empty() && field.events.back().first == "ChangedCommander");
	CHECK(field.events.back().second.size() == 3 && field.events.back().second[0].asInt() == 2
		&& field.events.back().second[1].kind == game::ScriptValue::Kind::None
		&& field.events.back().second[2].kind == game::ScriptValue::Kind::Player && field.events.back().second[2].i == a);
	// Not across teams.
	CHECK(!world.setCommander(1, a) && world.commander(1) == -1);
	CHECK(world.setCommander(1, c) && world.commander(1) == c);
	// Resigning hands it to the next applicant.
	world.resignCommander(a);
	CHECK(world.commander(2) == b);
	CHECK(world.playerInfo(a, info) && !info.commander);
	// A human applying takes over from a bot (provisional).
	world.applyForCommander(a);
	CHECK(world.commander(2) == a);
	// Disabled, nobody commands.
	world.setCommanderEnabled(false);
	CHECK(world.commander(1) == -1 && world.commander(2) == -1);
}

TEST(Commander_OrdersAndBots)
{
	Field field;
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	const int boss = world.addPlayer("Boss", 2, false);
	world.setCommander(2, boss);
	const int leader = world.addPlayer("Lead", 2, true);
	const int member = world.addPlayer("Member", 2, true);
	world.joinSquad(leader, 1);
	world.joinSquad(member, 1);
	CHECK(world.spawnSoldier(leader) && world.spawnSoldier(member));
	// Only the commander orders, and only squads that exist.
	CHECK(!world.issueOrder(2, 1, game::GameWorld::OrderType::Defend, 0, 60, leader));
	CHECK(!world.issueOrder(2, 4, game::GameWorld::OrderType::Defend, 0, 60, boss));
	CHECK(world.issueOrder(2, 1, game::GameWorld::OrderType::Defend, 0, 60, boss));
	const game::GameWorld::SquadOrder* order = world.squadOrder(2, 1);
	CHECK(order && order->type == game::GameWorld::OrderType::Defend && order->position.z == 60 && !order->accepted);
	world.acceptOrder(member, true); // not the leader's
	CHECK(!world.squadOrder(2, 1)->accepted);
	world.acceptOrder(leader, true);
	CHECK(world.squadOrder(2, 1)->accepted);
	// The squad's bots make for the order rather than the enemy's point
	// (100 m the other way).
	Console console;
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	game::BotManager bots(world, settings);
	for (int i = 0; i < 30 * 4; ++i) {
		bots.update(1 / 30.0f);
		world.tick(1 / 30.0f);
	}
	game::GameWorld::PlayerInfo info;
	CHECK(world.playerInfo(member, info) && info.position.z > 5 && std::fabs(info.position.x) < 5);
	// Turned down, it goes.
	world.acceptOrder(leader, false);
	CHECK(world.squadOrder(2, 1) == nullptr);
}

TEST(Commander_BotTakesCommand)
{
	Field field;
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	const int bot = world.addPlayer("Bot", 1, true);
	const int other = world.addPlayer("Other", 1, true);
	world.joinSquad(other, 1);
	Console console;
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	game::BotManager bots(world, settings);
	bots.update(1 / 30.0f);
	CHECK(world.commander(1) == bot && world.commander(2) == -1);
	// It sends its squad at the point it doesn't hold.
	const game::GameWorld::SquadOrder* order = world.squadOrder(1, 1);
	CHECK(order && order->type == game::GameWorld::OrderType::Attack && order->position.x == -100 && order->accepted);
}

TEST(Commander_OrbitalStrike)
{
	Field field;
	CHECK(field.loaded || !std::getenv("BF2142_GAME_DIR"));
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	const int boss = world.addPlayer("Boss", 2, false);
	world.setCommander(2, boss);
	// No asset, no strike.
	CHECK(!world.abilityState(2, Ability::OrbitalStrike).available);
	CHECK(!world.commanderAction(2, Ability::OrbitalStrike, 0, 50));
	const uint32_t station = field.place("commander_emp_strike", 2, -200, -200);
	game::GameWorld::CommanderAbilityState state = world.abilityState(2, Ability::OrbitalStrike);
	CHECK(state.available && state.ready && state.asset == station && state.armor == 1);
	CHECK(!world.abilityState(1, Ability::OrbitalStrike).available); // EU's

	const int near = field.soldier(1, 0, 50);
	const int edge = field.soldier(1, 10, 50);   // within explosionRadius 15
	const int far = field.soldier(1, 0, 80);
	const int mate = field.soldier(2, 0, 120);
	CHECK(world.commanderAction(2, Ability::OrbitalStrike, 0, 50));
	CHECK(world.strikes().size() == 1);
	// Recharging: minTimeBetweenActivation 115 s.
	state = world.abilityState(2, Ability::OrbitalStrike);
	CHECK(!state.ready && std::fabs(state.timeUntilReady - 115) < 0.1f && state.active);
	CHECK(!world.commanderAction(2, Ability::OrbitalStrike, 0, 80));
	// The launch delay (fireLaunchDelay 1.5, 2 whole seconds) and the fall
	// from orbitalBombardmentHeight (500 m at 100 m/s, five times gravity):
	// nothing yet after 3 s, the blast by 6.
	field.run(3);
	CHECK(field.alive(near) && field.alive(edge));
	field.run(3);
	CHECK(!field.alive(near) && !field.alive(edge));
	CHECK(field.alive(far) && field.alive(mate));
	game::GameWorld::PlayerInfo info;
	CHECK(world.playerInfo(boss, info) && info.kills == 2); // the commander's kills
	CHECK(world.strikes().empty());
	// Ready again after 115 s, not before.
	field.run(105);
	CHECK(!world.commanderAction(2, Ability::OrbitalStrike, 0, 80));
	field.run(5);
	CHECK(world.abilityReady(2, Ability::OrbitalStrike));
	// A wrecked station disables it (armour 1000, canBeDestroyed 0: a
	// wreck until repaired).
	world.setHitPoints(station, 0);
	CHECK(field.objects.find(station) && !world.abilityState(2, Ability::OrbitalStrike).available);
	CHECK(!world.commanderAction(2, Ability::OrbitalStrike, 0, 80));
	world.setHitPoints(station, 500);
	CHECK(world.abilityState(2, Ability::OrbitalStrike).available && world.abilityState(2, Ability::OrbitalStrike).armor == 0.5f);
}

TEST(Commander_Artillery)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	world.setCommander(1, world.addPlayer("Boss", 1, false));
	field.place("commander_orbital_strike_64", 1, 200, 200);
	CHECK(world.abilityReady(1, Ability::Artillery) && !world.abilityState(1, Ability::OrbitalStrike).available);
	const int target = field.soldier(2, 0, -60);
	CHECK(world.commanderAction(1, Ability::Artillery, 0, -60));
	// 16 rounds of 7 shells (fire.batchSize) a second (60 a minute), no
	// launch delay.
	CHECK(world.strikes().size() == 1 && world.strikes()[0].salvosLeft == 16);
	field.run(0.1f);
	CHECK(world.strikes()[0].salvosLeft == 15 && world.projectilesInFlight() == 7);
	field.run(20);
	CHECK(world.strikes().empty() && !field.alive(target));
	CHECK(std::fabs(world.abilityState(1, Ability::Artillery).timeUntilReady - (85 - 20.1f)) < 0.2f);
}

TEST(Commander_UavAndScanReveal)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	world.setCommander(2, world.addPlayer("Boss", 2, false));
	const int inside = field.soldier(1, 300, 330);
	const int outside = field.soldier(1, 300, 450);
	CHECK(!world.commanderAction(2, Ability::Uav, 300, 300)); // no UAV trailer
	field.place("commander_uav_drone_control", 2, -200, -200);
	CHECK(world.commanderAction(2, Ability::Uav, 300, 300));
	field.run(0.1f);
	physics::Vec3 at;
	float radius = 0;
	CHECK(world.uavArea(2, at, radius) && at.x == 300 && radius == 100);
	CHECK(world.isRevealed(inside, 2) && !world.isRevealed(outside, 2) && !world.isRevealed(inside, 1));
	// It stays 30 s and is ready again 5 s later.
	field.run(29);
	CHECK(world.isRevealed(inside, 2));
	field.run(1.5f);
	CHECK(!world.isRevealed(inside, 2) && !world.uavArea(2, at, radius) && !world.abilityReady(2, Ability::Uav));
	field.run(5);
	CHECK(world.abilityReady(2, Ability::Uav));
	// The satellite scan (commander_movement_analyst) shows everyone for 11 s.
	field.place("commander_movement_analyst", 2, -220, -200);
	CHECK(world.commanderAction(2, Ability::Satellite, 0, 0));
	field.run(0.1f);
	CHECK(world.isRevealed(inside, 2) && world.isRevealed(outside, 2));
	field.run(11);
	CHECK(!world.isRevealed(outside, 2));
	CHECK(std::fabs(world.abilityState(2, Ability::Satellite).timeUntilReady - (27 - 11.1f)) < 0.2f);
}

TEST(Commander_SupplyDrop)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	world.setCommander(2, world.addPlayer("Boss", 2, false));
	// No asset needed.
	CHECK(world.abilityReady(2, Ability::Supplies));
	const int hurt = field.soldier(2, 40, 40);
	const int enemy = field.soldier(1, 41, 40);
	game::GameWorld::PlayerInfo info;
	world.playerInfo(hurt, info);
	world.setHitPoints(info.soldier, 40);
	world.playerInfo(enemy, info);
	world.setHitPoints(info.soldier, 40);
	CHECK(world.commanderAction(2, Ability::Supplies, 40, 41));
	CHECK(!world.commanderAction(2, Ability::Supplies, 40, 41)); // 60 s to the next
	CHECK(world.supplyCrates().size() == 1);
	const uint32_t crate = world.supplyCrates()[0];
	CHECK(field.objects.find(crate)->position().y == 50); // supplyDropHeight
	// It comes down, then heals its team (healSpeed 3 a second) within 5 m.
	field.run(12);
	CHECK(field.objects.find(crate)->position().y < 1.0f);
	world.playerInfo(hurt, info);
	const float before = world.hitPoints(info.soldier);
	field.run(5);
	const float after = world.hitPoints(info.soldier);
	CHECK(before > 40 && after > before + 14 && after < before + 16);
	world.playerInfo(enemy, info);
	CHECK(world.hitPoints(info.soldier) == 40);
	field.run(45);
	CHECK(world.abilityReady(2, Ability::Supplies));
	// Gone after supplyDropNumSecsToLive (300 s).
	field.run(240);
	CHECK(world.supplyCrates().empty() && field.objects.find(crate) == nullptr);
}
