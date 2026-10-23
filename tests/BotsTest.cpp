// Bots: AIDLL.dll's behaviour selection, sensing, aiming and strategy
// (src/AI/Behaviour, src/AI/Strategy, src/Game/Bots), checked against
// values derived from the decompilation; and, where $BF2142_GAME_DIR is
// set, the game's own AI scripts and a round on Cerbere_Landing (gpm_sp).
#include "AI/Behaviour.h"
#include "AI/NavMesh.h"
#include "AI/PathFollower.h"
#include "AI/Strategy.h"
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/Bots.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Physics/CollisionTemplateManager.h"
#include "Terrain/Heightmap.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"
#include "World/TemplateFiles.h"
#include "World/WorldConsole.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace dice::hfe;

namespace {

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// The bot manager's frames for a while: a bot runs its plan on about half
// of them, thinks after 0.3 s (0.5 s with a plan) of them and senses when
// its turn comes (0x100c91c0, 0x100c95f0, 0x100c9980).
void settle(game::BotManager& bots, float seconds, game::GameWorld* world = nullptr)
{
	for (int tick = 0; tick < static_cast<int>(seconds * 30); ++tick) {
		bots.update(1 / 30.0f);
		if (world) {
			world->tick(1 / 30.0f);
		}
	}
}

// The Infantery set and modifier sets of AI/AIBehaviours.ai.
game::BotSettings scriptedSettings(Console& console)
{
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	std::string out;
	for (const char* line : {"aiSettings.createBehaviourModifiers UnitWeights", "aiSettings.setBehaviourModifier Idle 1.0",
			 "aiSettings.createBehaviourModifiers StandardWeights", "aiSettings.setBehaviourModifier Avoid 1.0",
			 "aiSettings.setBehaviourModifier MoveTo 1.0", "aiSettings.setBehaviourModifier Idle 0.1",
			 "aiSettings.setBehaviourModifier Fire 7.5", "aiSettings.setBehaviourModifier Change 1.9",
			 "aiSettings.setBehaviourModifier Revive 3.0", "aiSettings.createBehaviourModifiers AvoidInhibit",
			 "aiSettings.setBehaviourModifier MoveTo 0.3", "aiSettings.setBehaviourModifier MedicAssist 0.5",
			 "aiSettings.createUCConstant UCUnion 1.0", "aiSettings.createUCLinear UCFire -0.22 1.3",
			 "aiSettings.createUCXInverse UCScout 2.5 0.9 1.0 0.5",
			 "aiSettings.setVehicleBehaviour Infantery Avoid BBAvoid BBPAvoidCollisionInfantery 1 UCUnion AvoidInhibit",
			 "aiSettings.setVehicleBehaviour Infantery MoveTo BBMoveTo BBPGotoWaypointInfantery 4 UCUnion UnitWeights",
			 "aiSettings.setVehicleBehaviour Infantery Idle BBIdle BBPIdleInfantery 5 UCUnion UnitWeights",
			 "aiSettings.setVehicleBehaviour Infantery Fire BBFireInfantery BBPFireInfantery 4 UCFire UnitWeights",
			 "aiSettings.setVehicleDefaultBehaviour Infantery Idle", "aiSettings.setViewDistance 120"}) {
		console.execute(line, out);
	}
	return settings;
}

// scriptedSettings plus the Infantery set's assist behaviours and
// StandardWeights' MedicAssist 3 (AI/AIBehaviours.ai).
game::BotSettings assistSettings(Console& console)
{
	game::BotSettings settings = scriptedSettings(console);
	std::string out;
	for (const char* line : {"aiSettings.setVehicleBehaviour Infantery MedicAssist BBMedicAssist BBPMedicAssist 10 UCUnion UnitWeights",
			 "aiSettings.setVehicleBehaviour Infantery AmmoAssist BBAmmoAssist BBPAmmoAssist 12 UCUnion UnitWeights",
			 "aiSettings.setVehicleBehaviour Infantery RepairVehicle BBRepairVehicle BBPRepairVehicle 10 UCUnion UnitWeights",
			 "aiSettings.setVehicleBehaviour Infantery c4 BBC4 BBPC4 11 UCUnion UnitWeights"}) {
		console.execute(line, out);
	}
	settings.modifiers["StandardWeights"]["MedicAssist"] = 3;
	return settings;
}

// Flat ground, a control point per team 200 m apart (PAC's at +x), rifles
// that kill in three hits, and bots placed where a test wants them.
struct Field {
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	io::FileManager files;
	physics::CollisionTemplateManager collisionTemplates;
	std::unique_ptr<game::GameWorld> world;
	std::vector<std::string> events;

	Field()
	{
		templates.registerType(3, "Soldier");
		for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
			templates.create("Soldier", name)->settings()["armor.maxHitPoints"] = "100";
		}
		for (const char* name : {"eu_ar_rifle", "as_ar_rifle"}) {
			world::ObjectTemplate* rifle = templates.create("GenericFireArm", name);
			rifle->settings()["itemIndex"] = "3";
			rifle->settings()["projectileTemplate"] = "bullet";
			rifle->settings()["velocity"] = "300";
			rifle->settings()["fire.roundsPerMinute"] = "600";
			rifle->settings()["ammo.magSize"] = "30";
			rifle->settings()["ammo.nrOfMags"] = "50";
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
		world->autoRespawn = false;
		world->startRound();
	}
	int soldier(int team, bool ai, float x, float z)
	{
		const int id = world->addPlayer(team == 1 ? "Pac" : "Eu", team, ai);
		world->spawnSoldier(id);
		game::GameWorld::PlayerInfo info;
		world->playerInfo(id, info);
		world->setSoldierPosition(info.soldier, {x, 0, z});
		return id;
	}
	void move(int player, float x, float z)
	{
		game::GameWorld::PlayerInfo info;
		world->playerInfo(player, info);
		world->setSoldierPosition(info.soldier, {x, 0, z});
	}
	int fired(int player) { const auto r = world->playerRounds(player, false, false); return r.empty() ? 0 : r[0].count; }
	int hits(int player) { const auto r = world->playerRounds(player, true, false); return r.empty() ? 0 : r[0].count; }
};

} // namespace

TEST(Bots_UrgencyCurves)
{
	// aiSettings.createUCLinear UCFire -0.22 1.3 (0x10087ef0): max(0, -0.22 t + 1.3).
	const ai::UrgencyCurve fire {ai::UrgencyCurve::Kind::Linear, -0.22f, 1.3f, 0, 0};
	CHECK(near(fire.value(0), 1.3f) && near(fire.value(5), 0.2f) && fire.value(6) == 0);
	// createUCXInverse UCScout 2.5 0.9 1.0 0.5 (0x10087f50): 2.5 / (t + 0.9) + 0.5.
	const ai::UrgencyCurve scout {ai::UrgencyCurve::Kind::XInverse, 2.5f, 0.9f, 1.0f, 0.5f};
	CHECK(near(scout.value(0), 2.5f / 0.9f + 0.5f) && near(scout.value(4.1f), 1.0f));
	const ai::UrgencyCurve unit {ai::UrgencyCurve::Kind::Constant, 1.0f, 0, 0, 0};
	CHECK(unit.value(100) == 1);
}

TEST(Bots_FuzzyTable)
{
	// DAT_10139530 (built at 0x100cb6f0), read at round(x * 100).
	CHECK(ai::fuzzy(0) == 0 && ai::fuzzy(-1) == 0 && ai::fuzzy(2) == 1);
	CHECK(near(ai::fuzzy(0.1f), 0.292421f) && near(ai::fuzzy(0.18f), 0.5263579f));
	CHECK(near(ai::fuzzy(0.5f), 0.8947369f) && near(ai::fuzzy(1.0f), 0.9989368f)); // 100 reads entry 99
}

TEST(Bots_WeightsInhibitorsAndCurves)
{
	Console console;
	const game::BotSettings settings = scriptedSettings(console);
	const ai::VehicleSetup& infantry = settings.vehicle("Infantery");
	CHECK(infantry.behaviours.size() == 4 && infantry.defaultBehaviour == "Idle");
	const auto& personality = settings.modifiers.at("StandardWeights");
	ai::Arbitration state;
	// Before the first plan: the personality alone.
	std::vector<float> w = ai::behaviourWeights(infantry.behaviours, state, 0, personality, settings.modifiers, settings.curves);
	CHECK(w[0] == 1 && w[1] == 1 && near(w[2], 0.1f) && near(w[3], 7.5f));
	// Avoiding: AvoidInhibit holds MoveTo to 0.3.
	state.running = 0;
	state.replan = false;
	w = ai::behaviourWeights(infantry.behaviours, state, 0.1f, personality, settings.modifiers, settings.curves);
	CHECK(near(w[1], 0.3f) && near(w[3], 7.5f));
	// Firing: UCFire on the time since the last think (a tick).
	state.running = 3;
	w = ai::behaviourWeights(infantry.behaviours, state, 1 / 30.0f, personality, settings.modifiers, settings.curves);
	CHECK(near(w[3], 7.5f * (1.3f - 0.22f / 30)) && w[1] == 1);
}

TEST(Bots_ArbiterHysteresis)
{
	ai::Arbitration state;
	state.urgency = {0, 1.0f, 0.1f, 0};
	CHECK(ai::arbitrate(state, 2) == 1); // MoveTo
	// Within [0.87, 1.15] of the latched value nothing is picked anew.
	state.urgency = {0, 0.9f, 0.1f, 0};
	CHECK(ai::arbitrate(state, 2) == 1 && near(state.latched[1], 1.0f));
	state.urgency = {0, 0.9f, 1.02f, 0}; // Idle rises 10x: a new pick
	CHECK(ai::arbitrate(state, 2) == 2);
	// Fire appearing (0 -> 50) always picks again.
	state.urgency = {0, 0.9f, 1.02f, 50};
	CHECK(ai::arbitrate(state, 2) == 3);
	// Ties go to the first in the set; nothing above 0 runs the default.
	ai::Arbitration ties;
	ties.urgency = {0, 2, 2, 0};
	CHECK(ai::arbitrate(ties, 2) == 1);
	ai::Arbitration nothing;
	nothing.urgency = {0, 0, 0, 0};
	CHECK(ai::arbitrate(nothing, 2) == 2);
	CHECK(ai::urgencyChanged(1, 0.87f) == false && ai::urgencyChanged(1, 0.86f) && ai::urgencyChanged(1, 1.16f));
	CHECK(ai::urgencyChanged(0, 0) == false && ai::urgencyChanged(0, 0.1f) && ai::urgencyChanged(0.1f, 0));
}

TEST(Bots_GotoUrgency)
{
	// 0x1008aaa0: r = max(ceil(radius), bot radius + 1); 0 inside r; else
	// clamp(d2 / (1.5 r^2), 0.1, 1) x the area's factor.
	bool arrived = false;
	const ai::GoalArea plain;
	CHECK(ai::gotoUrgency(1.0f, 0, 0.5f, plain, arrived) == 0 && arrived);
	CHECK(near(ai::gotoUrgency(2.5f, 0, 0.5f, plain, arrived), 2.5f / 3.375f) && !arrived);
	CHECK(near(ai::gotoUrgency(2.3f, 0, 0.5f, plain, arrived), 2.3f / 3.375f));
	CHECK(ai::gotoUrgency(400, 0, 0.5f, plain, arrived) == 1);
	CHECK(near(ai::gotoUrgency(2501, 50, 0.5f, plain, arrived), 2501 / 3750.0f)); // just outside a 50 m order
	ai::GoalArea attack;
	attack.exists = true;
	attack.teamHolds = false;
	CHECK(ai::gotoUrgency(400, 0, 0.5f, attack, arrived) == 2);
	ai::GoalArea held;
	held.exists = true;
	held.inside = true;
	held.radius = 50;
	CHECK(ai::gotoUrgency(400, 0, 0.5f, held, arrived) == 0);
	held.inside = false;
	CHECK(near(ai::gotoUrgency(60 * 60, 0, 0.5f, held, arrived), 1.0f));
	CHECK(near(ai::gotoUrgency(2500.2f, 0, 0.5f, held, arrived), 0.1f)); // d2 - 50^2 is next to nothing
}

TEST(Bots_StrategyConditionsAndValues)
{
	ai::StrategySettings s;
	s.addDefaults();
	ai::SideStatus mine, theirs;
	// basicAttackEnemy: +-20 for each of "the enemy holds a point" and
	// "an area is neutral" (Crisp, Advisory).
	theirs.controlPoints = 1;
	mine.neutralAreas = 1;
	CHECK(ai::prerequisiteValue(s, "basicEnemyPrereq", mine, theirs) == 40);
	mine.neutralAreas = 0;
	CHECK(ai::prerequisiteValue(s, "basicEnemyPrereq", mine, theirs) == 0);
	theirs.controlPoints = 0;
	CHECK(ai::prerequisiteValue(s, "basicEnemyPrereq", mine, theirs) == -40);
	// basicGuard: 4 only with no enemy point and no neutral area (Required).
	CHECK(ai::prerequisiteValue(s, "guardPrereq", mine, theirs) == 4);
	theirs.controlPoints = 2;
	CHECK(ai::prerequisiteValue(s, "guardPrereq", mine, theirs) == 0);
	// Fuzzy conditions give the difference, FuzzySqr its signed square.
	ai::StrategyCondition fuzzy;
	fuzzy.conditional = ai::StrategyCondition::Conditional::FuzzySqr;
	fuzzy.compare = ai::StrategyCondition::Compare::EqualGreater;
	fuzzy.side = 1;
	fuzzy.type = 14;
	fuzzy.constant = 5;
	CHECK(ai::conditionValue(fuzzy, mine, theirs) == -9);
	// The pick sticks while no weight moves by 17 % down or 20 % up.
	CHECK(!ai::strategyWeightsChanged({40, 0}, {40, 0}) && !ai::strategyWeightsChanged({40, 0}, {35, 0}));
	CHECK(ai::strategyWeightsChanged({40, 0}, {48.5f, 0}) && ai::strategyWeightsChanged({40, 0}, {0, 4}));
	// Required strength: 2 x max(enemies, 1), x1.25 for areas not owned.
	CHECK(ai::requiredStrength(0, true) == 2 && ai::requiredStrength(3, true) == 6 && ai::requiredStrength(0, false) == 3);
	// basicAttackEnemy's modifiers: hostile points x10, neutral x12.
	const ai::Strategy& attack = s.strategies.at("basicAttackEnemy");
	CHECK(ai::modifiedTemperature(attack, 50, ai::AreaStatus::Hostile, true, false) == 500);
	CHECK(ai::modifiedTemperature(attack, 50, ai::AreaStatus::Neutral, true, false) == 600);
	CHECK(ai::modifiedTemperature(attack, 50, ai::AreaStatus::Owned, true, true) == 50);
	// Attack value: 100 v / d - 1.5 (frac + 1) x the enemies' strategic strength.
	ai::AreaState area;
	area.status = ai::AreaStatus::Hostile;
	area.temperature = 500;
	area.ownedNeighbour = area.hostileNeighbour = true;
	area.squadDistance = 100;
	CHECK(near(ai::attackValue(area), 500 - 1.5f));
	area.hostileNeighbour = false;
	area.neutralNeighbour = true;
	CHECK(near(ai::attackValue(area), 750 - 1.5f));
	area.ownedNeighbour = false;
	CHECK(ai::attackValue(area) == 0);
	ai::AreaState own;
	own.status = ai::AreaStatus::Owned;
	own.temperature = 500;
	own.enemyStrength = 8;
	own.hostileNeighbour = true;
	CHECK(near(ai::defenceValue(own), (8 + 500) * 2 * 1.5f));
	// The best n, then up to 2n with 0.8 of the n-th's value.
	const std::vector<int> picked = ai::selectAreas({{10, 0}, {100, 1}, {85, 2}, {50, 3}, {60, 4}}, 2, 0.8f);
	CHECK((picked == std::vector<int> {1, 2}));
	const std::vector<int> more = ai::selectAreas({{10, 0}, {100, 1}, {85, 2}, {70, 3}, {69, 4}}, 2, 0.8f);
	CHECK((more == std::vector<int> {1, 2, 3, 4}));
}

TEST(Bots_ScriptsFillTheSettings)
{
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	std::string out;
	for (const char* line : {"weaponTemplate.create eu_sni", "weaponTemplate.weaponType Sniper", "weaponTemplate.minRange 30.0",
			 "weaponTemplate.maxRange 125.0", "weaponTemplate.optimalRangePercentage 90", "weaponTemplate.setFiringPose Lying",
			 "weaponTemplate.setStrength Infantry 6.0", "aiStrategy.createConstantCondition c Fuzzy EqualGreater Enemy Ticket 3",
			 "aiStrategy.setConditionStrength AdvisoryPositive", "aiStrategy.createPrerequisite p", "aiStrategy.addCondition c 2",
			 "aiStrategy.createStrategy hold", "aiStrategy.NumberOfDefences 2", "aiStrategy.setStrategicObjectsModifier ControlPoint 10.0 Owned",
			 "aiStrategy.setStrategicObjectsModifier Front 3", "aiStrategy.setPrerequisite p", "ai.addSAIStrategy 2 hold",
			 "aiStrategicArea.createFromControlPoint A 301 50", "aiStrategicArea.layer 4",
			 "aiStrategicArea.createFromControlPoint B 302 50", "aiStrategicArea.setActive A",
			 "AIStrategicArea.addNeighbour B", "aiStrategicArea.addObjectTypeFlag ControlPoint",
			 "AIStrategicArea.setOrderPosition Infantry 19.7761/106.928/189.227", "aiStrategicArea.setSide 0",
			 "aiStrategicArea.addWayPoint -26.8967/95.053/148.488 Infantry A B"}) {
		console.execute(line, out);
	}
	const game::WeaponAi sniper = settings.weapon("EU_SNI");
	CHECK(sniper.type == game::WeaponAi::Sniper && sniper.minRange == 30 && sniper.maxRange == 125);
	CHECK(near(sniper.optimal, 0.9f) && sniper.firingPose == 3 && sniper.strength[0] == 6 && sniper.deviation == 14);
	CHECK(settings.weapon("Unl_Defibrillator").type == game::WeaponAi::Revive);
	const ai::StrategySettings& s = settings.strategy;
	CHECK(s.conditions.at("c").conditional == ai::StrategyCondition::Conditional::Fuzzy);
	CHECK(s.conditions.at("c").strength == ai::StrategyCondition::Strength::AdvisoryPositive && s.conditions.at("c").type == 13);
	const ai::Strategy& hold = s.strategies.at("hold");
	CHECK(hold.defences == 2 && hold.modifiers[0][ai::kTypeControlPoint] == 10 && hold.modifiers[1][ai::kTypeControlPoint] == 1);
	CHECK(hold.modifiers[0][ai::kTypeFront] == 3 && hold.modifiers[2][ai::kTypeFront] == 3 && hold.prerequisite == "p");
	CHECK(s.sideStrategies.at(2) == std::vector<std::string> {"hold"});
	CHECK(s.areas.size() == 2 && s.areas[0].layer == 4 && s.areas[0].neighbours == std::vector<std::string> {"B"});
	CHECK(s.areas[0].controlPointFlag && s.areas[0].hasOrderPosition[0] && near(s.areas[0].orderPosition[0][2], 189.227f, 1e-3f));
	CHECK(s.waypoints.size() == 1 && s.waypoints[0].from == "A" && s.waypoints[0].to == "B");
}

// The objects' AI templates (Objects/**/ai/Objects.ai): plug-ins made
// first, then the template that lists them; a seat's PlayerControlObject
// names its template (ObjectTemplate.aiTemplate).
TEST(Bots_AiTemplatesFillTheSettings)
{
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	std::string out;
	for (const char* line : {"aiTemplatePlugIn.create Mobile AS_FAV_Mobile", "aiTemplatePlugIn.setPathfindingMap Vehicle",
			 "aiTemplatePlugIn.maxSpeed 30.0", "aiTemplatePlugIn.speedAtZeroSpace 3", "aiTemplatePlugIn.backingSpeed 7",
			 "aiTemplatePlugIn.brakeDistance 25", "aiTemplatePlugIn.lookAheadTime 1.5",
			 "aiTemplatePlugIn.angleSlowDownScale 1.8", "aiTemplatePlugIn.create Physical AS_FAV_Physical",
			 "aiTemplatePlugIn.setStrType LightArmour", "aiTemplatePlugIn.create Unit AS_FAV_Unit",
			 "aiTemplatePlugIn.equipmentTypeName Car", "aiTemplatePlugIn.create ControlInfo AS_FAV_Ctrl",
			 "aiTemplatePlugIn.yawScale 2.0", "aiTemplate.create AS_FAV", "aiTemplate.addType ITGround",
			 "aiTemplate.basicTemp 8", "aiTemplate.addPlugIn AS_FAV_Mobile", "aiTemplate.addPlugIn AS_FAV_Physical",
			 "aiTemplate.addPlugIn AS_FAV_Ctrl", "aiTemplate.addPlugIn AS_FAV_Unit"}) {
		console.execute(line, out);
	}
	const game::AiPlugin* unit = settings.plugin("as_fav", "Unit");
	CHECK(unit && unit->equipmentType == "Car");
	const game::AiPlugin* mobile = settings.plugin("AS_FAV", "mobile");
	CHECK(mobile && mobile->maxSpeed == 30 && mobile->speedAtZeroSpace == 3 && mobile->backingSpeed == 7);
	CHECK(mobile && mobile->brakeDistance == 25 && near(mobile->lookAheadTime, 1.5f) && near(mobile->angleSlowDownScale, 1.8f));
	CHECK(mobile && mobile->pathfindingMap == "Vehicle");
	const game::AiPlugin* physical = settings.plugin("AS_FAV", "Physical");
	CHECK(physical && physical->strengthType == 1);
	const game::AiPlugin* control = settings.plugin("AS_FAV", "ControlInfo");
	CHECK(control && near(control->yawScale, 2.0f));
	CHECK(settings.aiTemplates.at("as_fav").basicTemp == 8 && !settings.plugin("AS_FAV", "Armament"));
}

// The view distance (AIDLL 0x10056b20, BF2142.exe 0x008959f0): set by the
// scripts, or the game's from the level's fog (where it reaches 0.8),
// within the level's view distance and 300 m.
TEST(Bots_ViewDistanceFromTheFog)
{
	// 2142_Shipment: fog 10/40/2/0.5 (0.5 + t^3 = 0.8 at t = 0.67), 250 m.
	CHECK(near(game::gameViewDistance({10, 40, 2, 0.5f}, 250), 10 + 30 * 0.67f, 0.05f));
	// Sidi_Power_Plant: 0/345/0/0 (t^3 = 0.8 at t = 0.93): 321 m, capped at 300.
	CHECK(near(game::gameViewDistance({0, 345, 0, 0}, 345), 300));
	CHECK(near(game::gameViewDistance({0, 345, 0, 0}, 200), 200));
	Field field;
	Console console;
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	std::string out;
	console.execute("GameLogic.MaximumLevelViewDistance 250", out);
	settings.levelFog = {10, 40, 2, 0.5f};
	game::BotManager bots(*field.world, settings);
	const int bot = field.soldier(2, true, 0, 0);
	const int near_ = field.soldier(1, false, 0, 20); // 30 cos 50 = 19.4 m: just out
	settle(bots, 1.0f);
	CHECK(!bots.knows(bot, near_));
	field.move(near_, 0, 18);
	settle(bots, 1.0f);
	CHECK(bots.knows(bot, near_));
}

// Sensing (0x100ed700, 0x100ee430): seen within viewDistance x cos(50)
// in the 100 degree frustum; remembered 60 s out of sight, then forgotten.
TEST(Bots_SensingAndForgetting)
{
	Field field;
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	game::BotManager bots(*field.world, settings);
	const int bot = field.soldier(2, true, 0, 0); // facing +z
	const int enemy = field.soldier(1, false, 0, 70);
	settle(bots, 1.5f);
	CHECK(bots.knows(bot, enemy) && bots.behaviour(bot) == "Fire" && bots.target(bot) == enemy);
	// 120 cos 50 = 77 m: not at 80 m, nor behind, nor 60 degrees aside.
	game::BotManager fresh(*field.world, settings);
	field.move(enemy, 0, 80);
	settle(fresh, 0.5f);
	CHECK(!fresh.knows(bot, enemy));
	field.move(enemy, 0, -20);
	settle(fresh, 0.5f);
	CHECK(!fresh.knows(bot, enemy));
	field.move(enemy, 20 * std::sin(1.05f), 20 * std::cos(1.05f));
	settle(fresh, 0.5f);
	CHECK(!fresh.knows(bot, enemy));
	field.move(enemy, 20 * std::sin(0.8f), 20 * std::cos(0.8f)); // 46 degrees: inside
	settle(fresh, 0.5f);
	CHECK(fresh.knows(bot, enemy));
	// Out of sight (300 m off): still known for 60 s, then forgotten.
	field.move(enemy, 0, 300);
	for (int i = 0; i < 59; ++i) {
		bots.update(1.0f);
	}
	CHECK(bots.knows(bot, enemy));
	for (int i = 0; i < 8; ++i) {
		bots.update(1.0f); // sensed, then a think drops the target (a lone bot runs every other frame)
	}
	CHECK(!bots.knows(bot, enemy) && bots.target(bot) == -1);
	// Cloaked enemies only within discoverCloakedEnemiesDistance (3 m) is
	// covered by the GameWorld camouflage test; skill does not change sensing.
}

// The reaction time (0x100777a0): (1 - skill)(1.5 + U[0,1]) s before the
// first round at a new target; and no round within 0.8 s of a pose
// change (Trigger 0x1008f1e0): the bot stands from its idle crouch.
TEST(Bots_ReactionTimeBySkill)
{
	for (const float skill : {0.4f, 1.0f}) {
		Field field;
		Console console;
		game::BotSettings settings = scriptedSettings(console);
		settings.skill = skill;
		game::BotManager bots(*field.world, settings);
		const int bot = field.soldier(2, true, 0, 0);
		const int enemy = field.soldier(1, false, 0, 20);
		game::GameWorld::PlayerInfo info;
		field.world->playerInfo(enemy, info);
		field.world->setHitPoints(info.soldier, 1e6f);
		// From the think that takes the target to the first round.
		int first = -1, acquired = -1;
		for (int tick = 0; tick < 30 * 5 && first < 0; ++tick) {
			bots.update(1 / 30.0f);
			field.world->tick(1 / 30.0f);
			if (acquired < 0 && bots.target(bot) == enemy) {
				acquired = tick;
			}
			if (field.fired(bot) > 0) {
				first = tick;
			}
		}
		CHECK(acquired >= 0);
		first -= acquired;
		const float seconds = static_cast<float>(first) / 30;
		std::printf("  skill %.1f: first round %.2f s after the target\n", skill, seconds);
		if (skill < 1) {
			CHECK(first > 0 && seconds >= (1 - skill) * 1.5f - 0.05f && seconds <= (1 - skill) * 2.5f + 0.5f);
		} else {
			CHECK(first >= 0 && seconds >= 0.75f && seconds < 1.0f);
		}
	}
}

// fuzzy (0x100cbce0) truncates x * 100 (the FPU set to chop) and takes
// the table's entry; scurve (0x100cc930) interpolates its own table.
TEST(Bots_FuzzyTruncatesAndSCurve)
{
	CHECK(near(ai::fuzzy(0.019f), ai::fuzzy(0.01f)) && near(ai::fuzzy(0.01f), 0.02924211f));
	CHECK(near(ai::fuzzy(0.999f), ai::fuzzy(0.99f)));
	CHECK(near(ai::scurve(0.5f), 0.5f) && near(ai::scurve(0.1f), 0.0171289f) && near(ai::scurve(0.75f), 0.912744f, 1e-5f));
	CHECK(near(ai::scurve(0.105f), 0.5f * (0.0171289f + 0.0196096f), 1e-5f));
	CHECK(ai::scurve(-1) == 0 && ai::scurve(2) == 1 && ai::scurve(std::nanf("")) == 0);
}

// The string-pull (0x10093610): a waypoint 5 m or more from the bot moves
// onto its portal where the line from the previous point to the next
// crosses it, kept max(1, 0.8 radius) from the portal's ends; then a last
// pass without the margin where that line crosses the portal itself.
TEST(Bots_PathSmoothingOnPortals)
{
	// A path bending round a corner: portals across the corridor x 0..4 at
	// z 10, then x 0..4 at z 20, the goal off to the side at (30, 0, 30).
	std::vector<ai::Waypoint> path(3);
	path[0].a = {0, 0, 10};
	path[0].b = {4, 0, 10};
	path[0].p = {2, 0, 10};
	path[0].portal = true;
	path[1].a = {0, 0, 20};
	path[1].b = {4, 0, 20};
	path[1].p = {2, 0, 20};
	path[1].portal = true;
	path[2].p = {30, 0, 30};
	ai::smoothWaypoints(path, {2, 0, 0}, 0.5f);
	// The second portal: the line from the first point to the goal crosses
	// beyond its end; kept 1 m from it, then the last pass (which only
	// moves points whose line crosses inside the portal) leaves it there.
	CHECK(near(path[1].p.x, 3, 1e-3f) && near(path[1].p.z, 20));
	// The first: on the line from the bot to the second point.
	CHECK(near(path[0].p.z, 10) && near(path[0].p.x, 2.5f, 1e-3f));
	// Near the bot (within 5 m) a waypoint isn't moved by the margin passes.
	std::vector<ai::Waypoint> close(2);
	close[0].a = {0, 0, 3};
	close[0].b = {4, 0, 3};
	close[0].p = {2, 0, 3};
	close[0].portal = true;
	close[1].p = {40, 0, 3.5f};
	ai::smoothWaypoints(close, {2, 0, 0}, 0.5f);
	CHECK(near(close[0].p.x, 2) && near(close[0].p.z, 3));
}

// The vehicles' driving (0x100e9d40, 0x100e0540, 0x100e8170): steering
// towards the error less the turn rate, the throttle towards the speed
// wanted; slowing for bends (angleSlowDownScale) and the path's end
// (brakeDistance); a stationary tank does not steer.
TEST(Bots_DriveControl)
{
	ai::DriveSettings car; // the FAV's Mobile plug-in, maxSpeed 15
	car.yawScale = 2;
	ai::DriveState state;
	ai::DriveSense sense;
	sense.error = 0.1f;
	sense.goalDistance = 100;
	sense.speed = 10;
	sense.remaining = 200;
	ai::DriveOutput out = ai::driveControl(car, state, sense);
	CHECK(near(out.turn, 0.6f, 1e-4f) && out.direction == 1 && state.state == 0);
	CHECK(near(out.targetSpeed, 15) && out.throttle > 0.9f);
	sense.yawRate = 0.5f; // already turning: less
	CHECK(near(ai::driveControl(car, state, sense).turn, 0.1f, 1e-4f));
	// A bend of 0.6 rad ahead: 1 - 0.8 min(1, 1.8 (0.6 - 0.2)) = 0.424 of top speed.
	sense.yawRate = 0;
	sense.maxBend = 0.6f;
	CHECK(near(ai::driveControl(car, state, sense).targetSpeed, 0.424f * 15, 1e-3f));
	// 10 m of path left at 10 m/s: space 10 - 2.5 - 0.5 = 7 m of 25:
	// max(0.2, 0.0784)^2 = 0.04.
	sense.maxBend = 0;
	sense.remaining = 10;
	CHECK(near(ai::driveControl(car, state, sense).targetSpeed, 0.04f * 15, 1e-4f));
	// A tank at a standstill does not steer.
	ai::DriveSettings tank = car;
	tank.driver = ai::Driver::Tank;
	ai::DriveState ahead;
	sense.speed = 0;
	sense.error = 0.5f;
	sense.remaining = 200;
	CHECK(ai::driveControl(tank, ahead, sense).turn == 0);
}

// MechMoveTo (0x1008d250, machine 0x100e1f20, controller 0x100e3c00):
// the walker turns on the spot (15/16) until facing within cos 0.9, then
// walks at full throttle, steering at full lock; at its top speed it
// sprints. Stopping lets go of everything (0x100e3f00).
TEST(Bots_MechDrive)
{
	ai::DriveSettings mech; // US_HeavyMech's Mobile plug-in
	mech.driver = ai::Driver::Mech;
	mech.maxSpeed = 8;
	mech.spaceNeededTweak = 0.91f;
	mech.backingSpeed = 6;
	mech.brakeDistance = 7;
	mech.maxBrakePercentage = 0;
	mech.minSpeedForLookAhead = 3;
	mech.minAngleDiffForSlowDown = 0.35f;
	mech.angleSlowDownScale = 2.5f;
	mech.maxAngleDiffSlowDown = 0.7f;
	mech.yawScale = 15;
	ai::DriveState state;
	ai::DriveSense sense;
	sense.goalDistance = 100;
	sense.remaining = 200;
	sense.error = 1.0f; // to the right, cos 0.54: not facing it
	ai::DriveOutput out = ai::driveControl(mech, state, sense);
	CHECK(state.state == 16 && out.direction == 0 && out.throttle == 0 && out.turn == 1);
	sense.now = 0.1f;
	out = ai::driveControl(mech, state, sense);
	CHECK(state.state == 16 && out.direction == 0 && out.throttle == 0 && out.turn == 1);
	// Nearly facing it: back to 0, then ahead at full throttle and lock.
	sense.error = 0.05f;
	sense.now = 0.2f;
	ai::driveControl(mech, state, sense);
	CHECK(state.state == 0);
	sense.now = 0.3f;
	out = ai::driveControl(mech, state, sense);
	CHECK(out.direction == 1 && out.throttle == 1 && out.turn == 1 && !out.sprint);
	// At 7 m/s on a straight: maxSpeed wanted, over 0.7 of it: sprint.
	sense.error = 0;
	sense.speed = 7;
	sense.now = 0.4f;
	out = ai::driveControl(mech, state, sense);
	CHECK(out.sprint && out.throttle == 1 && out.turn == 0 && near(out.targetSpeed, 8));
}

// FloatingTankMoveTo (0x1008c7c0, controller 0x100de350): the hover tank
// strafes. From a standstill it pushes 10 (clamped to 1) along the way on
// both axes; at speed the loop is 3 x (wanted - current - acceleration).
// The speed wanted is at least backingSpeed. The tank's stop (0x100e0930).
TEST(Bots_FloatingTankDrive)
{
	ai::DriveSettings hover; // as_tank's Mobile plug-in
	hover.maxSpeed = 15;
	hover.backingSpeed = 4;
	hover.brakeDistance = 14;
	hover.maxBrakePercentage = 80;
	hover.lookAheadTime = 2.5f;
	hover.minSpeedForLookAhead = 3;
	hover.minAngleDiffForSlowDown = 0.35f;
	hover.angleSlowDownScale = 2;
	hover.maxAngleDiffSlowDown = 0.6f;
	ai::DriveSense sense;
	sense.remaining = 200;
	const float h = std::sqrt(0.5f);
	ai::FloatingTankOutput out = ai::floatingTankControl(hover, sense, {h, 0, h}, {});
	CHECK(out.throttle == 1 && out.strafe == 1 && near(out.targetSpeed, 15));
	// Moving at 14 m/s straight ahead, the way straight ahead: 3 x (15 - 14).
	sense.velocity = {0, 0, 14};
	sense.speed = 14;
	out = ai::floatingTankControl(hover, sense, {0, 0, 1}, {});
	CHECK(out.throttle == 1 && out.strafe == 0);
	sense.velocity = {0, 0, 14.9f};
	out = ai::floatingTankControl(hover, sense, {0, 0, 1}, {});
	CHECK(near(out.throttle, 0.3f, 1e-4f));
	// 1 m of path left: max(0.2, 0)^2 x 15 = 0.6, floored at backingSpeed 4.
	sense.remaining = 1;
	CHECK(near(ai::floatingTankControl(hover, sense, {0, 0, 1}, {}).targetSpeed, 4));
	CHECK(ai::tankStopThrottle(0.03f) == 0 && ai::tankStopThrottle(0.2f) == -0.4f && near(ai::tankStopThrottle(0.5f), -0.5f)
		&& near(ai::tankStopThrottle(5), -0.7f));
}

// The state machine (0x100e8170): a goal far behind with room to turn:
// 11 (the point on the right), backing at the opposite lock; close
// behind (within 10 radii): 8, backing towards it, the error mirrored
// (pi - |e|); behind in a narrow spot: 12, backing straight out. One
// change per time stamp (0x100973b0).
TEST(Bots_DriveStates)
{
	ai::DriveSettings car;
	car.yawScale = 2;
	ai::DriveSense sense;
	sense.goalDistance = 100;
	sense.remaining = 200;
	sense.error = 3.0f;
	ai::DriveState far;
	ai::DriveOutput out = ai::driveControl(car, far, sense);
	CHECK(far.state == 11 && out.direction == 1);
	sense.now = 0.1f;
	out = ai::driveControl(car, far, sense);
	CHECK(far.state == 11 && out.direction == -1 && out.turn == -1 && out.throttle < 0);
	// Close behind.
	ai::DriveState close;
	sense.now = 1;
	sense.remaining = 20;
	ai::driveControl(car, close, sense);
	CHECK(close.state == 8);
	ai::driveControl(car, close, sense); // the same time stamp: no change
	sense.now = 1.1f;
	out = ai::driveControl(car, close, sense);
	CHECK(close.state == 8 && out.direction == -1);
	CHECK(near(out.turn, 3 * 2 * (3.14159265f - 3.0f), 1e-4f));
	// Narrow: 1 m either side.
	ai::DriveState narrow;
	sense.remaining = 200;
	sense.freeLeft = sense.freeRight = 1;
	sense.now = 2;
	ai::driveControl(car, narrow, sense);
	CHECK(narrow.state == 12);
	sense.now = 2.1f;
	out = ai::driveControl(car, narrow, sense);
	CHECK(narrow.state == 12 && out.direction == -1);
	// Facing the goal again: ahead.
	sense.error = 0;
	sense.now = 2.2f;
	ai::driveControl(car, narrow, sense);
	CHECK(narrow.state == 0);
}

// Other vehicles (0x100fbff0, 0x100fc660): one standing 8 m ahead, a
// little to the right, is passed (state 6) on the left: heading for the
// tangent of its sphere (0.8 of its radius + the driver's, seen from a
// quarter of a second ahead); and caps the speed behind it at 2 m/s (its
// speed along the way less 1, at least 2), so the driver brakes.
TEST(Bots_DriveAroundObstacles)
{
	ai::DriveSettings car;
	ai::DriveSense sense;
	sense.goalDistance = 100;
	sense.remaining = 200;
	sense.goal = {0, 0, 100};
	ai::DriveObstacle other;
	other.id = 7;
	other.position = other.centre = {0.5f, 0, 8};
	other.radius = other.centreRadius = 2.5f;
	sense.obstacles.push_back(other);
	ai::DriveState state;
	ai::DriveOutput out = ai::driveControl(car, state, sense);
	CHECK(state.state == 6 && state.avoided == 7 && state.side == 1);
	sense.now = 0.1f;
	out = ai::driveControl(car, state, sense);
	const float toIt = std::atan2(0.5f, 8.0f);
	const float half = std::asin(4.5f / std::sqrt(0.25f + 64));
	CHECK(state.state == 6 && near(out.turn, std::clamp(3 * (toIt - half), -1.0f, 1.0f), 1e-3f));
	// Moving at 10 m/s with it 10 m ahead: capped at 2 m/s.
	ai::DriveState moving;
	sense.obstacles[0].position = sense.obstacles[0].centre = {0, 0, 10};
	sense.velocity = {0, 0, 10};
	sense.speed = 10;
	out = ai::driveControl(car, moving, sense);
	CHECK(moving.brakingFor == 7 && out.throttle == -1);
	sense.obstacles.clear();
	ai::DriveState clear;
	CHECK(ai::driveControl(car, clear, sense).throttle == 1);
	// Something on the goal next to the driver: the move ends.
	other.position = other.centre = {0, 0, 6};
	sense.obstacles.push_back(other);
	sense.goal = {0, 0, 6.5f};
	sense.goalDistance = 6.5f;
	ai::DriveState there;
	CHECK(ai::driveControl(car, there, sense).stop);
}

// A level without AIPathFinding has no bots in the original (no ai.init,
// ai.addBot refused): with a null mesh the manager runs none.
TEST(Bots_NoNavigationMeshNoBots)
{
	Field field;
	Console console;
	game::BotManager bots(*field.world, scriptedSettings(console));
	bots.setNavMesh(nullptr);
	const int bot = field.soldier(2, true, 0, 0);
	field.soldier(1, false, 0, 30);
	settle(bots, 2, field.world.get());
	CHECK(!bots.enabled() && bots.behaviour(bot).empty() && field.fired(bot) == 0);
}

// MedicAssist (0x10070200, plan 0x10070da0): a medic with the hub near a
// wounded human teammate of its squad goes to them and takes the hub,
// which heals round its holder; AIDLL never throws it.
TEST(Bots_MedicAssistHealsATeammate)
{
	Field field;
	world::ObjectTemplate* hub = field.templates.create("GenericFireArm", "test_hub_medic");
	hub->settings()["itemIndex"] = "4";
	hub->settings()["ammo.replenishingType"] = "RTHeal";
	hub->settings()["ammo.abilityRadius"] = "4";
	hub->settings()["ammo.abilityStrength"] = "20";
	hub->settings()["projectileTemplate"] = "test_hub_object";
	hub->settings()["fire.roundsPerMinute"] = "60";
	field.templates.create("SimpleObject", "test_hub_object");
	world::ObjectTemplate* kit = field.templates.create("Kit", "test_medic_kit");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"test_hub_medic", {}, {}});
	Console console;
	game::BotManager bots(*field.world, assistSettings(console));
	const int medic = field.world->addPlayer("Medic", 2, true);
	field.world->setKit(medic, "test_medic_kit", "us_light_soldier");
	field.world->joinSquad(medic, 1);
	field.world->spawnSoldier(medic);
	field.move(medic, -90, 0);
	const int patient = field.world->addPlayer("Patient", 2, false);
	field.world->joinSquad(patient, 1);
	field.world->spawnSoldier(patient);
	field.move(patient, -90, 10);
	game::GameWorld::PlayerInfo info;
	field.world->playerInfo(patient, info);
	field.world->setHitPoints(info.soldier, 40);
	bool assisted = false;
	for (int tick = 0; tick < 30 * 15; ++tick) {
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
		assisted = assisted || bots.behaviour(medic) == "MedicAssist";
	}
	CHECK(assisted);
	CHECK(field.world->supplyObjects().empty());
	CHECK(field.world->hitPoints(info.soldier) > 60);
}

// c4 (0x10083750, plan 0x10083ae0): with a charge in the kit, standing in
// a control point its team holds uncontested: 100, the charge in hand,
// 0.7 s, one press of fire; then the bot carries on.
TEST(Bots_C4InItsOwnControlPoint)
{
	Field field;
	world::ObjectTemplate* c4 = field.templates.create("GenericFireArm", "test_c4");
	c4->settings()["itemIndex"] = "5";
	c4->settings()["projectileTemplate"] = "bullet";
	c4->settings()["ammo.magSize"] = "2";
	c4->settings()["ammo.nrOfMags"] = "1";
	world::ObjectTemplate* kit = field.templates.create("Kit", "test_recon_kit");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"test_c4", {}, {}});
	Console console;
	game::BotManager bots(*field.world, assistSettings(console));
	const int recon = field.world->addPlayer("Recon", 2, true);
	field.world->setKit(recon, "test_recon_kit", "us_light_soldier");
	field.world->joinSquad(recon, 1);
	field.world->spawnSoldier(recon);
	field.move(recon, -100, 3); // EU's point at x -100, radius 10
	bool placing = false;
	int placedAt = -1;
	for (int tick = 0; tick < 30 * 6 && placedAt < 0; ++tick) {
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
		placing = placing || bots.behaviour(recon) == "c4";
		const auto rounds = field.world->playerRounds(recon, false, false);
		for (const auto& r : rounds) {
			if (r.weapon == "test_c4" && r.count > 0) {
				placedAt = tick;
			}
		}
	}
	CHECK(placing && placedAt > 0);
	settle(bots, 2, field.world.get());
	CHECK(bots.behaviour(recon) != "c4");
}

// The goal list of an order (0x10060300): from the area the bot is in to
// the target over the areas' neighbours, a waypoint of the pair
// (aiStrategicArea.addWayPoint) when it lies ahead and closer to the
// target; then a point in each area on the way, then the goal.
TEST(Bots_StrategicWaypointsOnTheWay)
{
	Field field;
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	std::string out;
	for (const char* line : {"aiStrategicArea.createFromControlPoint pac 1 50", "aiStrategicArea.addNeighbour eu",
			 "aiStrategicArea.addObjectTypeFlag ControlPoint", "aiStrategicArea.createFromControlPoint eu 2 50",
			 "aiStrategicArea.addNeighbour pac", "aiStrategicArea.addObjectTypeFlag ControlPoint",
			 "aiStrategicArea.addWayPoint 0/0/40 Infantry pac eu", "aiStrategicArea.addWayPoint 0/0/-40 Vehicle eu pac"}) {
		console.execute(line, out);
	}
	game::BotManager bots(*field.world, settings);
	const int bot = field.world->addPlayer("Eu", 2, true);
	field.world->joinSquad(bot, 1);
	field.world->spawnSoldier(bot);
	field.move(bot, -100, 0); // in EU's area, ordered to PAC's at x 100
	settle(bots, 1, field.world.get());
	const std::vector<physics::Vec3> route = bots.route(bot);
	CHECK(route.size() == 2 && near(route[0].x, 0) && near(route[0].z, 40)); // the infantry waypoint, either direction
	CHECK(route.size() == 2 && route[1].x > 85);
}

// The AI deviation (BF2142.exe 0x00892b70): (1 - skill) x 14 at a new
// target. A perfect bot hits nearly every round at 40 m, an unskilled one
// rarely.
TEST(Bots_AimErrorBySkill)
{
	float ratio[2] = {};
	int index = 0;
	for (const float skill : {1.0f, 0.0f}) {
		Field field;
		Console console;
		game::BotSettings settings = scriptedSettings(console);
		settings.skill = skill;
		game::BotManager bots(*field.world, settings);
		const int bot = field.soldier(2, true, 0, 0);
		const int enemy = field.soldier(1, false, 0, 40);
		game::GameWorld::PlayerInfo info;
		field.world->playerInfo(enemy, info);
		field.world->setHitPoints(info.soldier, 1e6f);
		for (int tick = 0; tick < 30 * 6; ++tick) {
			bots.update(1 / 30.0f);
			field.world->tick(1 / 30.0f);
		}
		const int fired = field.fired(bot);
		CHECK(fired > 10);
		ratio[index++] = fired > 0 ? static_cast<float>(field.hits(bot)) / static_cast<float>(fired) : 0;
	}
	CHECK(ratio[0] > 0.8f && ratio[1] < 0.35f);
}

// MoveTo and the SAI: two EU squads on a field where PAC holds a point and
// one is neutral. The side runs basicAttackEnemy (enemy point and a
// neutral area: 40), orders its squads to attack, and they walk there
// (urgency 2: an area the team does not hold).
TEST(Bots_StrategyOrdersSquadsToAttack)
{
	Field field;
	world::ObjectTemplate* neutral = field.templates.create("ControlPoint", "cp_mid");
	neutral->settings()["controlPointId"] = "3";
	neutral->settings()["team"] = "0";
	neutral->settings()["radius"] = "10";
	field.objects.create(neutral, {0, 0, 60});
	field.world = std::make_unique<game::GameWorld>(field.templates, field.objects);
	field.world->enablePhysics(field.files, field.collisionTemplates,
		{[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& normal) {
			 normal = {0, 1, 0};
			 return 0.0f;
		 }});
	field.world->autoRespawn = false;
	field.world->startRound();
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	game::BotManager bots(*field.world, settings);
	std::vector<int> team;
	for (int i = 0; i < 4; ++i) {
		const int id = field.world->addPlayer("Eu", 2, true);
		field.world->joinSquad(id, i < 2 ? 1 : 2);
		field.world->spawnSoldier(id);
		team.push_back(id);
	}
	settle(bots, 1);
	CHECK(bots.strategy(2) == "basicAttackEnemy");
	for (int squad = 1; squad <= 2; ++squad) {
		const game::GameWorld::SquadOrder* order = field.world->squadOrder(2, squad);
		CHECK(order && order->type == game::GameWorld::OrderType::Attack && order->accepted);
		CHECK(order && (near(order->position.z, 60) || near(order->position.x, 100)));
	}
	CHECK(bots.behaviour(team[0]) == "MoveTo");
	for (int tick = 0; tick < 30 * 8; ++tick) {
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
	}
	// Walking to the neutral point at z 60 or PAC's at x 100.
	int closer = 0;
	for (int id : team) {
		game::GameWorld::PlayerInfo info;
		field.world->playerInfo(id, info);
		closer += info.position.z > 15 || info.position.x > 15;
	}
	CHECK(closer == 4);
}

namespace {

// A round on Cerbere_Landing (gpm_sp, 16) with the game's own AI scripts:
// the real terrain, navigation mesh, control points and strategic areas;
// four bots a side in squads. With `staticObjects`, also the level's static
// objects (all of Objects/' templates, then StaticObjects.con) for soldiers
// to collide with, as `bf2142 +level` loads them.
struct CerbereLanding {
	io::FileManager files;
	Console console {&files};
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	terrain::HeightmapCluster heightmaps;
	game::BotSettings settings;
	physics::CollisionTemplateManager collisionTemplates;
	std::unique_ptr<game::GameWorld> world;
	std::optional<ai::NavMesh> mesh;
	std::unique_ptr<game::BotManager> bots;
	std::vector<int> players;
	size_t weaponScripts = 0;
	bool loaded = false;

	CerbereLanding(const char* gameDir, bool staticObjects)
	{
		files.setRootPath(gameDir);
		files.addSearchPath("mods/bf2142");
		files.setMountingAllowed(true);
		const std::string level = "Levels/Cerbere_Landing/";
		CHECK(files.mountArchive(level + "server.zip", level));
		std::string out;
		if (staticObjects) {
			bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
				[this](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
			CHECK(console.run("ServerArchives.con", {}, out));
		} else {
			CHECK(files.mountArchive("Objects/Weapons_server.zip", "Objects\\Weapons"));
		}
		files.setMountingAllowed(false);
		world::registerWorldConsoleMethods(console, templates, objects);
		world::registerUntypedTemplateProperties(console, templates,
			{"controlPointId", "team", "radius", "unableToChangeTeam", "setControlPointId", "timeToGetControl",
				"timeToLoseControl", "areaValueTeam1", "areaValueTeam2", "collisionMesh", "hasMobilePhysics"});
		terrain::registerTerrainConsoleMethods(console, heightmaps, files);
		physics::registerCollisionConsoleMethods(console, collisionTemplates);
		game::registerBotConsoleMethods(console, settings);
		for (const std::string& script : {std::string("AI/AIBehaviours.ai"), std::string("AI/AIDefaultStrategies.ai"),
				 std::string("AI/AIDefaultStrategiesAdd.ai"), level + "GameModes/gpm_sp/16/ai/StrategicAreas.ai"}) {
			CHECK(console.run(script, {}, out));
		}
		for (const std::string& script : world::templateScripts(files, "Objects/Weapons/", true)) {
			if (script.size() > 3 && script.compare(script.size() - 3, 3, ".ai") == 0) {
				console.run(script, {}, out);
				++weaponScripts;
			}
		}
		if (staticObjects) {
			templates.setLoading(true);
			world::loadObjectTemplates(console, files, "Objects/", false);
			templates.setLoading(false);
			console.run(level + "StaticObjects.con", {}, out);
		}

		// The level: terrain, control points and spawn points.
		console.run(level + "Heightdata.con", {}, out);
		console.run(level + "GameModes/gpm_sp/16/GamePlayObjects.con", {"host"}, out);
		const terrain::Heightmap* ground = heightmaps.primary();
		CHECK(ground && ground->loaded());
		if (!ground || !ground->loaded()) {
			return;
		}
		for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
			world::ObjectTemplate* soldier = templates.find(name) ? templates.find(name) : templates.create("Soldier", name);
			soldier->settings()["armor.maxHitPoints"] = "100";
		}
		for (const char* name : {"eu_ar_rifle", "as_ar_rifle"}) {
			world::ObjectTemplate* gun = templates.find(name) ? templates.find(name) : templates.create("GenericFireArm", name);
			gun->settings()["projectileTemplate"] = "test_bullet";
			gun->settings()["velocity"] = "300";
			gun->settings()["fire.roundsPerMinute"] = "600";
			gun->settings()["ammo.magSize"] = "30";
			gun->settings()["ammo.nrOfMags"] = "50";
		}
		templates.create("GenericProjectile", "test_bullet")->settings()["damage"] = "34";
		world = std::make_unique<game::GameWorld>(templates, objects);
		physics::Ground surface;
		surface.height = [ground](float x, float z) { return ground->heightAt(x, z); };
		surface.heightAndNormal = [ground](float x, float z, physics::Vec3& normal) {
			const float step = ground->scale()[0];
			const float dx = ground->heightAt(x + step, z) - ground->heightAt(x - step, z);
			const float dz = ground->heightAt(x, z + step) - ground->heightAt(x, z - step);
			const float length = std::sqrt(dx * dx + 4 * step * step + dz * dz);
			normal = {-dx / length, 2 * step / length, -dz / length};
			return ground->heightAt(x, z);
		};
		world->enablePhysics(files, collisionTemplates, std::move(surface));
		world->startRound();
		std::vector<uint8_t> qtr, clb;
		if (files.readFile(level + "AIPathFinding/Infantry.qtr", qtr) && files.readFile(level + "AIPathFinding/Infantry.clb", clb)) {
			mesh = ai::parseNavMesh(qtr, clb);
		}
		CHECK(mesh.has_value());
		bots = std::make_unique<game::BotManager>(*world, settings);
		bots->setNavMesh(mesh ? &*mesh : nullptr);
		for (int team = 1; team <= 2; ++team) {
			for (int i = 0; i < 4; ++i) {
				const int id = world->addPlayer(team == 1 ? "Pac" : "Eu", team, true);
				world->joinSquad(id, world->openSquad(team));
				CHECK(world->spawnSoldier(id));
				players.push_back(id);
			}
		}
		loaded = true;
	}

	physics::Vec3 position(int id) const
	{
		game::GameWorld::PlayerInfo info;
		world->playerInfo(id, info);
		return info.position;
	}
	int fired(int id) const
	{
		const auto rounds = world->playerRounds(id, false, false);
		return rounds.empty() ? 0 : rounds[0].count;
	}
	void run(float seconds)
	{
		for (int tick = 0; tick < static_cast<int>(seconds * 30); ++tick) {
			bots->update(1 / 30.0f);
			world->tick(1 / 30.0f);
		}
	}
};

} // namespace

// The scripts' settings, the first orders, and a minute: the bots head off
// and meet.
TEST(Bots_CerbereLanding)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	CerbereLanding round(gameDir, false);
	game::BotSettings& settings = round.settings;
	// AIBehaviours.ai: 13 infantry behaviours; Fire on UCFire.
	const ai::VehicleSetup& infantry = settings.vehicle("Infantery");
	CHECK(infantry.behaviours.size() == 13 && settings.modifier("Fire") == 7.5f && settings.modifier("Idle") == 0.1f);
	CHECK(settings.curves.count("UCFire") && near(settings.curves.at("UCFire").a, -0.22f));
	CHECK(settings.vehicle("Tank").behaviours.size() == 7 && settings.vehicle("Car").behaviours.size() == 7);
	// Weapons.ai: the rifles, the sniper rifle and the defibrillator.
	CHECK(round.weaponScripts > 40);
	const game::WeaponAi rifle = settings.weapon("eu_ar_rifle");
	CHECK(rifle.maxRange == 72 && near(rifle.optimal, 0.35f) && rifle.strength[0] == 5 && rifle.firingPose == 2);
	CHECK(settings.weapon("Unl_Defibrillator").type == game::WeaponAi::Revive && settings.weapon("Unl_Defibrillator").maxRange == 60);
	// The strategies and the level's areas.
	CHECK(settings.strategy.strategies.count("basicAttackEnemy") && settings.strategy.strategies.count("basicGuard"));
	CHECK((settings.strategy.sideStrategies[1] == std::vector<std::string> {"basicAttackEnemy", "basicGuard"}));
	CHECK(settings.strategy.areas.size() == 4 && settings.strategy.waypoints.size() == 12);
	if (!round.loaded) {
		return;
	}
	game::GameWorld& world = *round.world;
	game::BotManager& bots = *round.bots;
	CHECK(world.controlPointList().size() == 4);
	bots.update(1 / 30.0f);
	// Both sides attack (the enemy holds points, none is neutral: weight 0
	// for both strategies, the first added runs). PAC (South Offices, 301)
	// goes for its neighbours Roadblock Area or South Town Center; EU for
	// South Offices, the one area next to its own that it doesn't hold.
	CHECK(bots.strategy(1) == "basicAttackEnemy" && bots.strategy(2) == "basicAttackEnemy");
	const auto orderedTo = [&](int team, float x, float z) {
		for (int squad = 1; squad <= 9; ++squad) {
			const game::GameWorld::SquadOrder* order = world.squadOrder(team, squad);
			if (order && order->type == game::GameWorld::OrderType::Attack && near(order->position.x, x, 0.01f)
				&& near(order->position.z, z, 0.01f)) {
				return true;
			}
		}
		return false;
	};
	CHECK(orderedTo(1, 19.5134f, 83.7731f) || orderedTo(1, 6.79578f, -38.4976f));
	CHECK(orderedTo(2, 58.1218f, -122.23f));
	CHECK(!orderedTo(1, 19.7761f, 189.227f)); // the Command Center has no PAC neighbour
	// A minute: they head off and meet.
	std::vector<physics::Vec3> start;
	for (int id : round.players) {
		start.push_back(round.position(id));
	}
	round.run(60);
	int moved = 0, fired = 0;
	for (size_t i = 0; i < round.players.size(); ++i) {
		game::GameWorld::PlayerInfo info;
		world.playerInfo(round.players[i], info);
		moved += !info.alive || physics::lengthSquared(info.position - start[i]) > 20 * 20;
		fired += round.fired(round.players[i]);
	}
	CHECK(moved >= 6);
	CHECK(fired > 0);
}

// The same round among the level's static objects (houses, walls, rocks,
// bushes): the bots must still get through. They used to wedge a few metres
// from their spawns: soldiers collided with collision LODs that are not
// theirs (a bush's projectile-only LOD; 0x006e4880 files LODs by collision
// type), and spheres whose centre lay behind a face were pushed through it
// (0x006c3890 drops those contacts).
TEST(Bots_CerbereLandingAmongStaticObjects)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	CerbereLanding round(gameDir, true);
	CHECK(round.loaded);
	if (!round.loaded) {
		return;
	}
	CHECK(round.world->staticCollision().triangles() > 10000);
	std::vector<physics::Vec3> start;
	for (int id : round.players) {
		start.push_back(round.position(id));
	}
	// Every 10 s, how far each bot walking to its goal got in the last 10 s:
	// tens of metres when it walks; one wedged in geometry gets nowhere.
	std::vector<physics::Vec3> last = start;
	int walking = 0, stuck = 0;
	for (int interval = 0; interval < 6; ++interval) {
		round.run(10);
		for (size_t i = 0; i < round.players.size(); ++i) {
			game::GameWorld::PlayerInfo info;
			round.world->playerInfo(round.players[i], info);
			if (info.alive && !info.vehicle && round.bots->behaviour(info.id) == "MoveTo") {
				++walking;
				stuck += physics::lengthSquared(info.position - last[i]) < 6 * 6;
			}
			last[i] = info.position;
		}
	}
	int moved = 0, fired = 0;
	for (size_t i = 0; i < round.players.size(); ++i) {
		game::GameWorld::PlayerInfo info;
		round.world->playerInfo(round.players[i], info);
		moved += !info.alive || info.vehicle || physics::lengthSquared(info.position - start[i]) > 20 * 20;
		fired += round.fired(round.players[i]);
	}
	std::printf("  %d of %d bots moved over 20 m, %d of %d walking samples stuck, %d rounds fired\n", moved,
		static_cast<int>(round.players.size()), stuck, walking, fired);
	CHECK(moved >= 6);
	CHECK(stuck * 4 <= walking);
	CHECK(fired > 0);
}

// The path follower (0x10095fc0, 0x10095720, 0x10092bc0) on Cerbere_Landing's
// infantry mesh: a path to the Command Center whose waypoints lie on the
// portals they cross; a walker at 5 m/s reaching each within 0.5 m gets
// there; one that pushes and doesn't move fails after 10 s.
TEST(Bots_PathFollowerOnCerbereLanding)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	CerbereLanding round(gameDir, false);
	CHECK(round.loaded && round.mesh.has_value());
	if (!round.loaded || !round.mesh) {
		return;
	}
	const ai::NavMesh& mesh = *round.mesh;
	const physics::Vec3 start = round.position(round.players[0]);
	const ai::Vec3 from {start.x, start.y, start.z};
	const ai::Vec3 to {19.7761f, 106.928f, 189.227f}; // the Command Center's order position
	ai::PathFollower follower;
	ai::FollowerSettings settings;
	follower.request(mesh, from, to, settings, 0);
	CHECK(follower.planned() && !follower.failed() && follower.waypoints().size() > 3);
	int onPortal = 0, portals = 0;
	for (const ai::Waypoint& w : follower.waypoints()) {
		if (!w.portal) {
			continue;
		}
		++portals;
		const float ex = w.b.x - w.a.x, ez = w.b.z - w.a.z;
		const float cross = std::fabs(ex * (w.p.z - w.a.z) - ez * (w.p.x - w.a.x)) / std::max(std::sqrt(ex * ex + ez * ez), 1e-4f);
		const float t = ((w.p.x - w.a.x) * ex + (w.p.z - w.a.z) * ez) / std::max(ex * ex + ez * ez, 1e-6f);
		onPortal += cross < 0.01f && t >= -1e-3f && t <= 1.001f;
	}
	CHECK(portals > 0 && onPortal == portals);
	// Walk it: 1/6 m a tick, the height from the mesh.
	ai::Vec3 at = from;
	bool arrived = false;
	float now = 0;
	for (int tick = 0; tick < 30 * 120 && !arrived; ++tick) {
		now += 1 / 30.0f;
		const ai::PathFollower::Steer steer = follower.steer(mesh, at, now);
		CHECK(steer.ok);
		if (!steer.ok) {
			break;
		}
		arrived = steer.arrived;
		const float dx = steer.point.x - at.x, dz = steer.point.z - at.z;
		const float d = std::sqrt(dx * dx + dz * dz);
		const float step = std::min(d, 5.0f / 30);
		if (d > 1e-4f) {
			at.x += dx / d * step;
			at.z += dz / d * step;
		}
		const int32_t polygon = mesh.findPolygon(at, 5.0f);
		if (polygon >= 0) {
			at.y = mesh.planeHeight(static_cast<uint32_t>(polygon), at);
		}
		follower.progress(at, 1, now);
	}
	CHECK(arrived);
	CHECK(std::sqrt((at.x - to.x) * (at.x - to.x) + (at.z - to.z) * (at.z - to.z)) < 1);
	// Stuck: pushing without moving fails after 10 s, not before.
	ai::PathFollower stuck;
	stuck.request(mesh, from, to, settings, 0);
	for (float t = 0; t < 9.5f; t += 0.1f) {
		stuck.progress(from, 1, t);
	}
	CHECK(!stuck.failed());
	for (float t = 9.5f; t < 10.5f; t += 0.1f) {
		stuck.progress(from, 1, t);
	}
	CHECK(stuck.failed() && !stuck.steer(mesh, from, 10.5f).ok);
}

// A constant condition (0x100a9620, 0x1005cce0, 0x1005ce20): the friendly
// value for Friendly, the enemy's otherwise (Both too), minus the
// constant (or over it for the Quotient compares) against 0.
TEST(Bots_StrategyConditions)
{
	using C = ai::StrategyCondition;
	ai::SideStatus friendly, enemy;
	friendly.controlPoints = 3;
	enemy.controlPoints = 1;
	C c;
	c.type = 14;
	c.constant = 2;
	c.compare = C::Compare::EqualGreater;
	CHECK(ai::conditionValue(c, friendly, enemy) == 1);
	c.side = 2; // Both: the enemy's
	CHECK(ai::conditionValue(c, friendly, enemy) == -1);
	c.side = 0;
	c.compare = C::Compare::QuotientSmaller; // 3 / 2 <= 0
	CHECK(ai::conditionValue(c, friendly, enemy) == -1);
	c.constant = 0;
	c.compare = C::Compare::QuotientGreater; // no constant: 1
	CHECK(ai::conditionValue(c, friendly, enemy) == 1);
	c.conditional = C::Conditional::Fuzzy;
	CHECK(ai::conditionValue(c, friendly, enemy) == 1000);
	c.constant = 1;
	c.compare = C::Compare::DifferenceSmaller;
	c.conditional = C::Conditional::FuzzySqr; // -(3 - 1) squared, signed
	CHECK(ai::conditionValue(c, friendly, enemy) == -4);
}

// BBPFlee's plan (0x10085fc0, 0x10085de0): under 30 % health with the
// camouflage, the bot takes it in hand, runs to a point 50 m (+-20 m)
// away from the enemy while holding the camouflage for 4 s, and the plan
// is locked until both are done.
TEST(Bots_FleeRunsFiftyMetresAway)
{
	Field field;
	world::ObjectTemplate* camo = field.templates.create("GenericFireArm", "test_camo");
	camo->settings()["itemIndex"] = "5";
	world::ObjectTemplate* kit = field.templates.create("Kit", "test_recon_kit");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"test_camo", {}, {}});
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	std::string out;
	console.execute("aiSettings.setVehicleBehaviour Infantery Flee BBFlee BBPFlee 7 UCUnion UnitWeights", out);
	settings.modifiers["StandardWeights"]["Flee"] = 1;
	game::BotManager bots(*field.world, settings);
	const int bot = field.world->addPlayer("Recon", 2, true);
	field.world->setKit(bot, "test_recon_kit", "us_light_soldier");
	field.world->spawnSoldier(bot);
	field.move(bot, 0, 0);
	const int enemy = field.soldier(1, false, 0, 400);
	settle(bots, 11, field.world.get()); // more than 10 s since spawning
	game::GameWorld::PlayerInfo info;
	field.world->playerInfo(bot, info);
	field.world->setHitPoints(info.soldier, 20);
	game::GameWorld::PlayerInfo enemyInfo;
	field.world->playerInfo(enemy, enemyInfo);
	field.world->setHitPoints(enemyInfo.soldier, 1e6f);
	field.move(enemy, 25, 0); // ahead: the bot walks to the control point at +x
	field.move(bot, 0, 0);
	bool fled = false, camoOn = false;
	for (int tick = 0; tick < 30 * 8; ++tick) {
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
		fled = fled || bots.behaviour(bot) == "Flee";
		game::GameWorld::PlayerInfo now;
		field.world->playerInfo(bot, now);
		camoOn = camoOn || now.cloaked;
	}
	field.world->playerInfo(bot, info);
	std::printf("  fled to (%.1f, %.1f), camouflage %s\n", info.position.x, info.position.z, camoOn ? "on" : "off");
	CHECK(fled);
	CHECK(info.position.x < -15); // away from the enemy
}

// Tap or hold (0x100777a0): the trigger is held only for a weapon whose
// fire mode is 2, the first fire.addFireRate (kept in order as
// fire.addFireRate.list); as_ar_rifle's [2, 0] holds, a single-shot [0]
// taps.
TEST(Bots_FireRateListKeptInOrder)
{
	Console console;
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	world::registerWorldConsoleMethods(console, templates, objects);
	world::registerUntypedTemplateProperties(console, templates, {"fire.addFireRate", "fire.roundsPerMinute"});
	std::string out;
	for (const char* line : {"ObjectTemplate.create GenericFireArm test_rifle", "ObjectTemplate.fire.addFireRate 2",
			 "ObjectTemplate.fire.addFireRate 0", "ObjectTemplate.fire.roundsPerMinute 600"}) {
		console.execute(line, out);
	}
	const world::ObjectTemplate* rifle = templates.find("test_rifle");
	CHECK(rifle && rifle->settings().at("fire.addFireRate.list") == "2 0");
	CHECK(rifle && rifle->settings().count("fire.roundsPerMinute.list") == 0);
}

// The seat value (0x100d3e70) of the bot's own soldier: health x (its
// weapons' strengths against the objects it knows + 0.3 x the soldier's
// maxSpeed 5 x 1.5 x 0.5). The rifle (strength 5 against infantry)
// counts once an enemy soldier is known.
TEST(Bots_SeatValueOfTheSoldier)
{
	Field field;
	Console console;
	game::BotManager bots(*field.world, scriptedSettings(console));
	world::ObjectTemplate* kit = field.templates.create("Kit", "test_rifle_kit");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	const int bot = field.world->addPlayer("Rifleman", 2, true);
	field.world->setKit(bot, "test_rifle_kit", "us_light_soldier");
	field.world->spawnSoldier(bot);
	field.move(bot, 0, 0); // facing +z
	settle(bots, 0.5f);
	CHECK(near(bots.seatValueFor(bot, 0, 0), 1.125f));
	const int enemy = field.soldier(1, false, 0, 20);
	settle(bots, 1.0f);
	CHECK(bots.knows(bot, enemy));
	CHECK(near(bots.seatValueFor(bot, 0, 0), 5 + 1.125f));
	game::GameWorld::PlayerInfo info;
	field.world->playerInfo(bot, info);
	field.world->setHitPoints(info.soldier, 50);
	CHECK(near(bots.seatValueFor(bot, 0, 0), 0.5f * (5 + 1.125f)));
}

// The level's AI/AI.ai pathfinding settings (aiPathfinding.createMap in
// AI/AIPathfinding.ai, then setActiveMap and map.*): the maps' climb
// limits; the infantry map's step is always 0.35 (0x1003f930). And the AI
// templates' InformationTypes (aiTemplate.addType, BF2142.exe 0x0088fbd0).
TEST(Bots_PathfindingMapsAndInformationTypes)
{
	Console console;
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	std::string out;
	// No scripts: 90 degrees (cos 0) and 0.35.
	CHECK(near(settings.climbLimits(1).maxSlope, 1.5707964f) && near(settings.climbLimits(1).stepHeight, 0.35f));
	for (const char* line : {"aiPathfinding.createMap Infantry", "aiPathfinding.createMap Vehicle",
			 "aiPathfinding.setActiveMap Infantry", "aiPathfinding.map.maxSlope 35",
			 "aiPathfinding.map.allowedThresholdHeight 1.0", "aiPathfinding.setActiveMap Vehicle",
			 "aiPathfinding.map.maxSlope 24", "aiPathfinding.map.allowedThresholdHeight 0.1",
			 "aiTemplate.create AS_APC", "aiTemplate.addType ITGround", "aiTemplate.addType ITMobile",
			 "aiTemplate.addType ITBiological", "aiTemplate.create Wall", "aiTemplate.addType ITObstacle"}) {
		console.execute(line, out);
	}
	CHECK(settings.pathfindingMaps.size() == 2);
	const ai::ClimbLimits infantry = settings.climbLimits(0), vehicle = settings.climbLimits(1);
	CHECK(near(infantry.maxSlope, 35 * 3.14159265f / 180) && near(infantry.stepHeight, 0.35f));
	CHECK(near(vehicle.maxSlope, 24 * 3.14159265f / 180) && near(vehicle.stepHeight, 0.1f));
	CHECK(settings.typeFlags("as_apc") == (game::kITGround | game::kITMobile)); // ITBiological is none
	CHECK(settings.typeFlags("Wall") == game::kITObstacle && settings.typeFlags("nothing") == 0);
	CHECK(game::informationTypeFlag("ITHelicopter") == 4 && game::informationTypeFlag("ITSoldier") == 0x200);
}

// BBChange's factor on another vehicle's other seats (0x1006b910) and the
// seat value's transport distances (0x100d3730).
TEST(Bots_SeatFactorsAndTransport)
{
	using game::otherSeatFactor;
	CHECK(otherSeatFactor(false, true, game::kITGround) == 0.77f);
	CHECK(otherSeatFactor(false, true, game::kITAir | game::kITHelicopter) == 0.5f);
	CHECK(otherSeatFactor(false, true, game::kITGround | game::kITAir) == 0.77f); // ITGround first
	CHECK(otherSeatFactor(true, true, game::kITGround) == 1);                    // someone drives it
	CHECK(otherSeatFactor(false, false, game::kITGround) == 1);                  // no Mobile part
	CHECK(otherSeatFactor(false, true, game::kITGround | game::kITNaval) == 1);
	bool noMove = false;
	// Far: the bot 80 m from the area's spot, the vehicle 60 m from its own.
	CHECK(!game::transportClose(false, 80 * 80, 60 * 60, noMove) && !noMove);
	// The bot within 50 m of the spot: close (the move doesn't count).
	CHECK(game::transportClose(false, 40 * 40, 60 * 60, noMove) && noMove);
	// The bot 60 m away and the vehicle 65 m: farther than the bot, and the
	// bot under 70 m: close.
	CHECK(game::transportClose(false, 60 * 60, 65 * 65, noMove));
	CHECK(!game::transportClose(false, 60 * 60, 55 * 55, noMove)); // the vehicle nearer
	// In it: close within 5 m of its spot.
	noMove = false;
	CHECK(game::transportClose(true, 1e6f, 4 * 4, noMove) && !noMove);
	CHECK(!game::transportClose(true, 0, 6 * 6, noMove));
}

// The 7 s after a human player of the team gets out of a vehicle (BF2142.exe
// 0x00894860 stamps the unit's per-team time, vt+0x64 0x008a1b10; the seat
// value 0x100d3e70 reads it through vt+0x60): its root seat is worth
// nothing to the team's bots; another team's bots don't care.
TEST(Bots_HumanGettingOutKeepsBotsOffForSevenSeconds)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/bf2142");
	Console console {&files};
	bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
		[&files](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
	std::string out;
	console.run("ServerArchives.con", {}, out);
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	physics::CollisionTemplateManager collisionTemplates;
	templates.registerType(1, "ControlPoint");
	templates.registerType(2, "SpawnPoint");
	templates.registerType(3, "Soldier");
	templates.registerType(7, "PlayerControlObject");
	templates.registerType(8, "EntryPoint");
	templates.registerType(9, "Engine");
	templates.registerType(10, "Spring");
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
	// A car on as_fav's collision mesh, with an AI template that has a
	// Mobile part (its root seat: 20 x 1.5 x 0.5 x 0.3 = 4.5).
	collisionTemplates.create("as_fav", "Objects/Vehicles/as_fav/");
	world::ObjectTemplate* car = templates.create("PlayerControlObject", "car");
	car->settings()["hasMobilePhysics"] = "1";
	car->settings()["physicsType"] = "Mesh";
	car->settings()["collisionMesh"] = "as_fav";
	car->settings()["seatInformation"] = "car 0/0.5/0.4 0/0/0";
	car->settings()["mass"] = "1800";
	car->settings()["aiTemplate"] = "TestCar";
	car->children().push_back({"car_entry", {-1.5f, 0, 0}, {}});
	world::ObjectTemplate* engine = templates.create("Engine", "car_engine");
	engine->settings()["engineType"] = "194";
	world::ObjectTemplate* wheel = templates.create("Spring", "car_wheel");
	wheel->settings()["strength"] = "40";
	wheel->settings()["damping"] = "8";
	car->children().push_back({"car_engine", {0, -0.44f, 0}, {}});
	for (float x : {-0.95f, 0.95f}) {
		for (float z : {-1.4f, 1.6f}) {
			engine->children().push_back({"car_wheel", {x, -0.15f, z}, {}});
		}
	}
	templates.create("EntryPoint", "car_entry")->settings()["entryRadius"] = "3";
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	for (const char* line : {"aiTemplatePlugIn.create Mobile TestCar_Mobile", "aiTemplatePlugIn.maxSpeed 20",
			 "aiTemplate.create TestCar", "aiTemplate.addType ITGround", "aiTemplate.addPlugIn TestCar_Mobile"}) {
		console.execute(line, out);
	}
	game::GameWorld world(templates, objects);
	world.enablePhysics(files, collisionTemplates,
		{[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& normal) {
			 normal = {0, 1, 0};
			 return 0.0f;
		 }});
	world.autoRespawn = false;
	world.startRound();
	const uint32_t vehicle = objects.create(car, {3, 1.2f, 0}, {30, 0, 0})->id();
	const int human = world.addPlayer("Human", 2, false);
	const int bot = world.addPlayer("Bot", 2, true);
	const int enemyBot = world.addPlayer("EnemyBot", 1, true);
	CHECK(world.spawnSoldier(human)); // the bots stay unspawned: they only weigh the seat
	game::GameWorld::PlayerInfo info;
	world.playerInfo(human, info);
	world.setSoldierPosition(info.soldier, {0, 0, 0});
	game::BotManager bots(world, settings);
	const auto run = [&](float seconds) {
		for (int tick = 0; tick < static_cast<int>(seconds * 30); ++tick) {
			bots.update(1 / 30.0f);
			world.tick(1 / 30.0f);
		}
	};
	const auto press = [&] {
		game::SoldierInput input;
		input.use = true;
		world.setInput(human, input);
		bots.update(1 / 30.0f);
		world.tick(1 / 30.0f);
		world.setInput(human, {});
	};
	run(3); // the car settles
	CHECK(near(bots.seatValueFor(bot, vehicle, 0), 4.5f, 0.05f));
	press();
	run(0.5f);
	world.playerInfo(human, info);
	CHECK(info.vehicle == vehicle);
	press(); // out
	run(0.5f);
	world.playerInfo(human, info);
	CHECK(info.vehicle == 0);
	CHECK(bots.seatValueFor(bot, vehicle, 0) == 0);
	// The other team's stamp isn't set (its bots would still refuse a car
	// with an enemy aboard, but it is empty).
	CHECK(near(bots.seatValueFor(enemyBot, vehicle, 0), 4.5f, 0.05f));
	run(6.0f);
	CHECK(bots.seatValueFor(bot, vehicle, 0) == 0); // 6.5 s after
	run(1.0f);
	CHECK(near(bots.seatValueFor(bot, vehicle, 0), 4.5f, 0.05f)); // 7.5 s after
}
