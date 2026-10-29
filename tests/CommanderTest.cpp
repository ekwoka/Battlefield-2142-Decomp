// The commander, with the game's own templates where $BF2142_GAME_DIR is
// set (Weapons_server.zip: the commander's assets commander_emp_strike,
// commander_orbital_strike_64, commander_uav_drone_control and
// commander_movement_analyst, their RemoteControlledObjects, and the
// supply_crate); the assignment and the orders run without them.
#include "Console/Console.h"
#include "Game/Bots.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Main/HudMenus.h"
#include "Physics/CollisionTemplateManager.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/WorldConsole.h"

#include <algorithm>
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
					{"rcType", "minTimeBetweenActivation", "uavVehicleTemplate", "uavVehicleSpeed", "uavVehicleRadius",
						"uavVehicleFlightHeight", "uavVehicleSampleResolution", "armor.maxHitPoints", "armor.hitPoints",
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
	// Applications wait out the apply period (15 s), then the
	// highest-ranked applicant is offered command and answers.
	world.joinSquad(a, world.openSquad(2));
	const int d = world.addPlayer("D", 2, false);
	world.setPlayerRank(d, 3);
	world.setPlayerRank(a, 3);
	world.applyForCommander(a);
	field.run(5);
	world.applyForCommander(d);
	world.applyForCommander(b);
	field.run(9.8f);
	CHECK(world.commander(2) == -1 && world.commanderOffer(2) == -1);
	field.run(0.3f);
	CHECK(world.commanderOffer(2) == a && world.commander(2) == -1); // the first of the highest
	// Declined: the next best at once.
	world.answerCommanderOffer(a, false);
	CHECK(world.commanderOffer(2) == d);
	world.answerCommanderOffer(b, true); // not offered: nothing
	CHECK(world.commander(2) == -1);
	world.answerCommanderOffer(d, true);
	CHECK(world.commander(2) == d && world.commander(1) == -1 && world.commanderOffer(2) == -1);
	game::GameWorld::PlayerInfo info;
	CHECK(world.playerInfo(d, info) && info.commander);
	CHECK(!field.events.empty() && field.events.back().first == "ChangedCommander");
	CHECK(field.events.back().second.size() == 3 && field.events.back().second[0].asInt() == 2
		&& field.events.back().second[1].kind == game::ScriptValue::Kind::None
		&& field.events.back().second[2].kind == game::ScriptValue::Kind::Player && field.events.back().second[2].i == d);
	// The commander leaves its squad.
	world.setCommander(2, a);
	CHECK(world.playerInfo(a, info) && info.commander && info.squad == 0);
	CHECK(!world.joinSquad(a, 1));
	// Not across teams.
	CHECK(!world.setCommander(1, a) && world.commander(1) == -1);
	CHECK(world.setCommander(1, c) && world.commander(1) == c);
	// Taking command drops the applicants (POSFILLED); after a
	// resignation the next applicant is offered command 10 s later.
	world.applyForCommander(b);
	world.resignCommander(a);
	CHECK(world.commander(2) == -1 && world.playerInfo(a, info) && !info.commander);
	field.run(9.9f);
	CHECK(world.commanderOffer(2) == -1);
	field.run(0.2f);
	CHECK(world.commanderOffer(2) == b);
	world.answerCommanderOffer(b, true);
	CHECK(world.commander(2) == b);
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
	// (Beyond the order's 80 m, orderRadius, so they set off.)
	CHECK(!world.issueOrder(2, 1, game::GameWorld::OrderType::MapDefend, 0, 160, leader));
	CHECK(!world.issueOrder(2, 4, game::GameWorld::OrderType::MapDefend, 0, 160, boss));
	CHECK(world.issueOrder(2, 1, game::GameWorld::OrderType::MapDefend, 0, 160, boss));
	const game::GameWorld::SquadOrder* order = world.squadOrder(2, 1);
	CHECK(order && order->type == game::GameWorld::OrderType::MapDefend && order->position.z == 160 && !order->accepted
		&& order->radius == 80);
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

TEST(Commander_BotsNeverApply)
{
	Field field;
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	world.addPlayer("Bot", 1, true);
	const int other = world.addPlayer("Other", 1, true);
	world.joinSquad(other, 1);
	Console console;
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	game::BotManager bots(world, settings);
	for (int i = 0; i < 30 * 20; ++i) {
		bots.update(1 / 30.0f);
		world.tick(1 / 30.0f);
	}
	// AIDLL's commander is the team's AI, not a player.
	CHECK(world.commander(1) == -1 && world.commander(2) == -1 && world.commanderOffer(1) == -1);
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
	// Its projectile (damage 3000) kills the soldier it lands on; its blast
	// is an EMP (detonation.isEMPWeapon: the detonation hands it to the
	// game logic's EMP, 0x005a7ab0), which hurts nobody.
	CHECK(!field.alive(near) && field.alive(edge) && field.alive(far) && field.alive(mate));
	game::GameWorld::PlayerInfo info;
	CHECK(world.playerInfo(boss, info) && info.kills == 1); // the commander's kill
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
	// Each shell strays (SimpleDeviationComp, 0x005f5540): subProjectileDev
	// 45 x 0.01 x the firearm's velocity 10 = 4.5 m/s, one standard
	// deviation on each axis across its fall (the main one by the last
	// sub's); the barrage lands round the point.
	game::GameWorld::PlayerInfo info;
	field.world->playerInfo(target, info);
	const uint32_t soldier = info.soldier;
	field.run(20);
	std::vector<float> landed; // the last 64 (the world keeps that many)
	for (const auto& impact : world.impacts()) {
		landed.push_back(std::sqrt(impact.point.x * impact.point.x + (impact.point.z + 60) * (impact.point.z + 60)));
	}
	std::sort(landed.begin(), landed.end());
	const float median = landed.empty() ? -1 : landed[landed.size() / 2];
	std::printf("  the last %zu shells landed a median %.1f m from the point, the nearest %.1f m\n", landed.size(), median,
		landed.empty() ? -1.0f : landed.front());
	CHECK(world.strikes().empty() && landed.size() == 64);
	CHECK(median > 3 && median < 60);
	CHECK(!field.alive(target) || world.hitPoints(soldier) < 100);
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
	if (!field.templates.find("eu_drone")) {
		field.templates.registerType(9, "UAVVehicle");
		field.templates.create("UAVVehicle", "eu_drone")->settings()["armor.maxHitPoints"] = "1000";
	}
	CHECK(world.commanderAction(2, Ability::Uav, 300, 300));
	// UAVControlObject: eu_drone at x + 40 (uavVehicleRadius), 70 m up
	// (uavVehicleFlightHeight over flat ground), circling at 14 m/s with
	// asin(14 / 1200) x 57.29578 x 0.5263158 = 0.3518 rad/s: 39.8 m round.
	const uint32_t drone = world.uavDrone(2);
	const world::Object* flying = field.objects.find(drone);
	CHECK(drone != 0 && flying && std::fabs(flying->position().x - 340) < 1e-3f && std::fabs(flying->position().y - 70) < 1e-3f);
	field.run(5);
	flying = field.objects.find(drone);
	if (flying) {
		const float rate = std::asin(14.0f / 1200) * 57.29578f * 0.5263158f;
		const float circle = 14 / rate;
		const float dx = flying->position().x - (340 - circle), dz = flying->position().z - 300;
		CHECK(std::fabs(std::sqrt(dx * dx + dz * dz) - circle) < 0.01f && std::fabs(circle - 39.79f) < 0.05f);
		CHECK(dz > 0); // round from +x towards +z
		CHECK(std::fabs(std::atan2(dz, dx) - rate * 5.0f) < 0.02f);
	}
	physics::Vec3 at;
	float radius = 0;
	CHECK(world.uavArea(2, at, radius) && at.x == 300 && radius == 100);
	CHECK(world.isRevealed(inside, 2) && !world.isRevealed(outside, 2) && !world.isRevealed(inside, 1));
	// It stays 30 s and is ready again 5 s later.
	field.run(24);
	CHECK(world.isRevealed(inside, 2) && world.uavDrone(2) == drone);
	field.run(1.5f);
	CHECK(!world.isRevealed(inside, 2) && !world.uavArea(2, at, radius) && !world.abilityReady(2, Ability::Uav));
	CHECK(world.uavDrone(2) == 0 && !field.objects.find(drone)); // gone with the UAV's time
	field.run(5);
	CHECK(world.abilityReady(2, Ability::Uav));
	// The satellite scan (commander_movement_analyst) shows everyone for
	// 11 s, to the commander only.
	field.place("commander_movement_analyst", 2, -220, -200);
	const int mate = field.soldier(2, -300, -300);
	CHECK(world.commanderAction(2, Ability::Satellite, 0, 0));
	field.run(0.1f);
	const std::vector<int> seen = world.scannedPlayers(world.commander(2));
	CHECK(seen.size() == 2 && world.scannedPlayers(mate).empty() && !world.isRevealed(outside, 2));
	field.run(11);
	CHECK(world.scannedPlayers(world.commander(2)).empty());
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

TEST(Commander_OrderKindsAndLeaderOrders)
{
	using Order = game::GameWorld::OrderType;
	// The engine's kinds (HudManager slot 0x3f4): 0-3 from the issue-order
	// menu, 4-7 placed on the map, 10 and up drop the armed order.
	CHECK(game::GameWorld::orderTypeFromKind(0) == Order::Move && game::GameWorld::orderTypeFromKind(1) == Order::Attack
		&& game::GameWorld::orderTypeFromKind(2) == Order::Defend && game::GameWorld::orderTypeFromKind(7) == Order::MapRepair
		&& game::GameWorld::orderTypeFromKind(10) == Order::None);
	// Only 5 and 6 count as attack or defend for the scripts.
	CHECK(game::GameWorld::isAttackOrDefend(Order::MapAttack) && game::GameWorld::isAttackOrDefend(Order::MapDefend)
		&& !game::GameWorld::isAttackOrDefend(Order::Attack) && !game::GameWorld::isAttackOrDefend(Order::MapMove));
	Field field;
	game::GameWorld& world = *field.world;
	const int leader = world.addPlayer("Lead", 2, false);
	const int member = world.addPlayer("Member", 2, true);
	world.joinSquad(leader, 1);
	world.joinSquad(member, 1);
	// A squad leader orders his own squad, accepted at once; a member can't.
	CHECK(!world.issueSquadLeaderOrder(member, Order::MapAttack, 10, 20));
	CHECK(world.issueSquadLeaderOrder(leader, Order::MapAttack, 10, 20));
	const game::GameWorld::SquadOrder* order = world.squadLeaderOrder(2, 1);
	CHECK(order && order->type == Order::MapAttack && order->accepted && !order->fromCommander && order->position.z == 20);
	CHECK(world.squadOrder(2, 1) == nullptr); // apart from the commander's
	CHECK(world.issueSquadLeaderOrder(leader, Order::None, 0, 0) && world.squadLeaderOrder(2, 1) == nullptr);
}

TEST(Commander_AiUsesToolsWithoutCommander)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	field.place("commander_orbital_strike_64", 1, 200, 200);
	// Nobody commands: a player's request is refused, the game's own
	// (IBFCommander, no sender) goes through.
	CHECK(world.commander(1) == -1 && !world.commanderAction(1, Ability::Artillery, 0, -60));
	CHECK(world.commanderAction(1, Ability::Artillery, 0, -60, true) && world.strikes().size() == 1);
}

// The squad leaders' requests (SquadLeader.sendRequest → 0x007d6d20, the
// store 0x004c5720) and the commander's answers (0x007c7e20), the 4 s
// auto-deny of a tool that can't be used (0x007c8a60 / 0x007c7a00).
TEST(Commander_SquadRequests)
{
	Field field;
	if (!field.loaded) {
		return;
	}
	game::GameWorld& world = *field.world;
	world.setCommanderEnabled(true);
	const int boss = world.addPlayer("Boss", 2, false);
	const int leader = field.soldier(2, 10, 10);
	const int other = field.soldier(2, 30, 10);
	world.joinSquad(leader, 1);
	world.joinSquad(other, 2);
	menu::SquadRequests requests(world);
	std::vector<std::string> personal, info;
	requests.onPersonal = [&](const std::string& s) { personal.push_back(s); };
	requests.onInfo = [&](const std::string& s) { info.push_back(s); };
	requests.localPlayer = boss;
	// Without a commander: refused, the leader alone hears why.
	world.takeRadioHeard();
	CHECK(!requests.send(leader, 3, 40, 41));
	auto heard = world.takeRadioHeard();
	CHECK(heard.size() == 1 && heard[0].voice == "no_commander_available" && heard[0].receiver == leader);
	world.setCommander(2, boss);
	// Supplies (kind 3): stored, the leader's radio to the commander, the
	// commander's line (the tool is ready).
	CHECK(requests.send(leader, 3, 40, 41) && world.squadRequest(2, 1, 3));
	heard = world.takeRadioHeard();
	CHECK(!heard.empty() && heard[0].voice == "PLAYER_REQUEST_requestsupplies" && heard[0].receiver == boss);
	CHECK(info.size() == 1 && info[0] == "HUD_HUD_REQUEST_RECIEVED_FROM_SQUAD");
	CHECK(requests.send(other, 3, 30, 31) && world.squadRequests(2).size() == 2);
	world.takeRadioHeard();
	// Accepted: the drop at the request's point, "supply_accept" to its
	// leader, "supply_deny" to the other squad's; both taken off.
	CHECK(requests.answer(boss, 1, 3, true) == menu::SquadRequests::Answer::Accepted);
	CHECK(world.supplyCrates().size() == 1 && world.squadRequests(2).empty());
	heard = world.takeRadioHeard();
	bool accepted = false, denied = false;
	for (const auto& h : heard) {
		accepted = accepted || (h.voice == "supply_accept" && h.receiver == leader);
		denied = denied || (h.voice == "supply_deny" && h.receiver == other);
	}
	CHECK(accepted && denied);
	// Again: the drop reloads, so accepting fails and the request stays.
	CHECK(requests.send(leader, 3, 40, 41));
	world.takeRadioHeard();
	CHECK(requests.answer(boss, 1, 3, true) == menu::SquadRequests::Answer::Unavailable && world.squadRequest(2, 1, 3));
	heard = world.takeRadioHeard();
	CHECK(heard.size() == 1 && heard[0].voice == "supply_unavailable" && heard[0].receiver == boss);
	CHECK(requests.answer(boss, 1, 3, false) == menu::SquadRequests::Answer::Denied && !world.squadRequest(2, 1, 3));
	// Artillery with no asset: no line for the commander, and 4 s later
	// it goes by itself with "artillery_unavailable" to the leader.
	info.clear();
	CHECK(requests.send(leader, 0, 40, 41) && info.empty());
	world.takeRadioHeard();
	requests.tick(3.9f);
	CHECK(world.squadRequest(2, 1, 0));
	requests.tick(0.2f);
	heard = world.takeRadioHeard();
	CHECK(!world.squadRequest(2, 1, 0) && heard.size() == 1 && heard[0].voice == "artillery_unavailable"
		&& heard[0].receiver == leader);
	// At most six requests until 30 s after the first (sv.radio* defaults);
	// eight sent so far would be refused had they not reset: reset now.
	requests.tick(31);
	int stored = 0;
	for (int i = 0; i < 8; ++i) {
		stored += requests.send(leader, 2, 40, 41) ? 1 : 0;
	}
	CHECK(stored == 6);
	// A dead leader can't request.
	game::GameWorld::PlayerInfo me;
	world.playerInfo(leader, me);
	world.setHitPoints(me.soldier, 0);
	field.run(0.1f);
	requests.tick(31);
	personal.clear();
	CHECK(!requests.send(leader, 2, 40, 41) && personal.size() == 1 && personal[0] == "HUD_HUD_SQUAD_ALIVE_TO_REQUEST");
}
