// Bots: AIDLL.dll's behaviour selection, sensing, aiming and strategy
// (src/AI/Behaviour, src/AI/Strategy, src/Game/Bots), checked against
// values derived from the decompilation; and, where $BF2142_GAME_DIR is
// set, the game's own AI scripts and a round on Cerbere_Landing (gpm_sp).
#include "AI/Ballistics.h"
#include "AI/Behaviour.h"
#include "AI/Formation.h"
#include "AI/NavMesh.h"
#include "AI/PathFollower.h"
#include "AI/Random.h"
#include "AI/Strategy.h"
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/Bots.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "Physics/CollisionTemplateManager.h"
#include "Physics/MaterialManager.h"
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
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace dice::hfe;

namespace {

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
float horizontal2(const physics::Vec3& d) { return d.x * d.x + d.z * d.z; }

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
	physics::MaterialManager materials;

	Field() : Field(nullptr) {}
	// `setup` runs before the world is made (static objects, materials);
	// with one the world takes the field's materials.
	explicit Field(const std::function<void(Field&)>& setup)
	{
		templates.registerType(3, "Soldier");
		for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
			templates.create("Soldier", name)->settings()["armor.maxHitPoints"] = "100";
		}
		for (const char* name : {"eu_ar_rifle", "as_ar_rifle"}) {
			world::ObjectTemplate* rifle = templates.create("GenericFireArm", name);
			rifle->settings()["itemIndex"] = "3";
			rifle->settings()["aiTemplate"] = name; // the AI's weaponTemplate, where a test makes one
			rifle->settings()["projectileTemplate"] = "bullet";
			rifle->settings()["velocity"] = "300";
			rifle->settings()["fire.roundsPerMinute"] = "600";
			rifle->settings()["ammo.magSize"] = "30";
			rifle->settings()["ammo.nrOfMags"] = "50";
			// As the shipped rifles: a SoldierDeviationComp (no deviation of its
			// own here), which carries the bots' AI deviation.
			rifle->components().push_back("SoldierDeviationComp");
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
		if (setup) {
			setup(*this);
		}
		world = std::make_unique<game::GameWorld>(templates, objects);
		world->enablePhysics(files, collisionTemplates,
			{[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& normal) {
				 normal = {0, 1, 0};
				 return 0.0f;
			 }},
			setup ? &materials : nullptr);
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

// The ControlInfo plug-in (BF2142.exe 0x00891d80 and the setters after
// 0x008856e1): the constructor's gains, the inputs as c_PI indices with
// their masks, and the camera-relative limits in radians clamped to +-pi.
TEST(Bots_ControlInfoTuning)
{
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	std::string out;
	for (const char* line : {"aiTemplatePlugIn.create ControlInfo Plain_Ctrl", "aiTemplatePlugIn.create ControlInfo Gun_Ctrl",
			 "aiTemplatePlugIn.driveTurnControl PIYaw", "aiTemplatePlugIn.driveThrottleControl PIThrottle",
			 "aiTemplatePlugIn.aimHorizontalControl PIMouseLookX", "aiTemplatePlugIn.lookVerticalControl PIMouseLookY",
			 "aiTemplatePlugIn.lookHorizontalControl PIRoll", "aiTemplatePlugIn.proportionalConstant -4",
			 "aiTemplatePlugIn.derivativeConstant -1", "aiTemplatePlugIn.angleSpeedConstant 0.5",
			 "aiTemplatePlugIn.setCameraRelativeMinRotationDeg -193.5/-30/0",
			 "aiTemplatePlugIn.setCameraRelativeMaxRotationDeg 39.5/85/0",
			 "aiTemplatePlugIn.setCameraRelativeDofRotationOffsetDeg 90", "aiTemplate.create Plain",
			 "aiTemplate.addPlugIn Plain_Ctrl", "aiTemplate.create Gun", "aiTemplate.addPlugIn Gun_Ctrl"}) {
		console.execute(line, out);
	}
	const game::AiPlugin* plain = settings.plugin("Plain", "ControlInfo");
	CHECK(plain && plain->proportional == -7 && plain->derivative == -1 && near(plain->angleSpeed, 1.5f));
	CHECK(plain && plain->yawScale == 1 && plain->lookHorizontal == 0x40 && plain->driveMask == 0);
	CHECK(plain && near(plain->cameraMin[0], -3.1415927f) && near(plain->cameraMax[2], 3.1415927f));
	const game::AiPlugin* gun = settings.plugin("Gun", "ControlInfo");
	CHECK(gun && gun->proportional == -4 && gun->derivative == -1 && near(gun->angleSpeed, 0.5f));
	CHECK(gun && gun->driveTurn == 0 && gun->driveThrottle == 3 && gun->driveMask == 0x9);
	CHECK(gun && gun->aimHorizontal == 4 && gun->aimMask == 0x10 && gun->aimVertical == 0x40);
	CHECK(gun && gun->lookVertical == 5 && gun->lookHorizontal == 2 && gun->lookMask == 0x24);
	// -193.5 degrees clamps to -pi; 85 degrees is 1.4835 rad.
	CHECK(gun && near(gun->cameraMin[0], -3.1415927f) && near(gun->cameraMin[1], -0.5235988f));
	CHECK(gun && near(gun->cameraMax[0], 0.6894051f) && near(gun->cameraMax[1], 1.4835299f));
	CHECK(gun && near(gun->dofRotationOffset, 1.5707964f));
	CHECK(game::playerInputIndex("c_PIMouseLookY") == 5 && game::playerInputIndex("PIFire") == 0x40);
}

// The SAI's strengths (0x1005f210): the Unit plug-in's battle and strategic
// strengths, the Armament's isAntiAircraft and the kits' kitTemplate; the
// required strength from the enemies' strategic sum and the friendly share.
TEST(Bots_StrengthsAndKitTemplates)
{
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	std::string out;
	for (const char* line : {"aiTemplatePlugIn.create Unit SoldierUnit", "aiTemplatePlugIn.setBattleStrength Infantry 4.0",
			 "aiTemplatePlugIn.setBattleStrength LightArmour 2.0", "aiTemplatePlugIn.setStrategicStrength 0 1",
			 "aiTemplatePlugIn.setStrategicStrength 1 3", "aiTemplatePlugIn.create Armament SoldierArms",
			 "aiTemplatePlugIn.setIsAntiAircraft 2", "aiTemplate.create Soldier", "aiTemplate.addPlugIn SoldierUnit",
			 "aiTemplate.addPlugIn SoldierArms", "kitTemplate.create Anti-Vehicle AT", "kitTemplate.setStrategicStrength 0 2",
			 "kitTemplate.setStrategicStrength 1 2", "kitTemplate.setBattleStrength HeavyArmour 3"}) {
		console.execute(line, out);
	}
	const game::AiPlugin* unit = settings.plugin("Soldier", "Unit");
	CHECK(unit && unit->battleStrength[0] == 4 && unit->battleStrength[1] == 2 && unit->strategicStrength[1] == 3);
	const game::AiPlugin* arms = settings.plugin("Soldier", "Armament");
	CHECK(arms && arms->antiAircraft == 2);
	const auto kit = settings.kitTemplates.find("anti-vehicle");
	CHECK(kit != settings.kitTemplates.end() && kit->second.kitType == 3 && kit->second.strategic[0] == 2);
	CHECK(kit != settings.kitTemplates.end() && kit->second.battle[2] == 3);
	// Owned: 2 x max(sum, 1); not owned: (2 - S(share)) x max(sum, 1) x 1.25.
	CHECK(ai::requiredStrength(4, true, 0.9f) == 8);
	CHECK(ai::requiredStrength(4, false, 0) == 10 && ai::requiredStrength(4, false, 1) == 5);
	CHECK(ai::requiredStrength(4, false, 0.5f) == 8); // (2 - 0.5) x 4 x 1.25 = 7.5
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
		// Seed 2 of the AI's twister: a stream whose draws have the skill-1 bot
		// change its pose first, so its round waits out the 0.8 s.
		settings.random = ai::Random::seeded(2);
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
		} else if (bots.keepsToOrder(bot)) {
			// The target lies across the way to the goal's head (the goal
			// about +x, the target +z): the plan walks to that spot while it
			// fights (fireKeepsToOrder), standing, so no pose change waits.
			CHECK(first >= 0 && seconds < 1.0f);
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

// Boxed in ahead while facing the point (the forward rays short of
// spaceNeededTweak x radius, room behind): the car backs out of state 11
// (0x100e8170 asks for room ahead to go on), while the tank's and the
// mech's state 11 (0x100de8a0, 0x100e1f20) go back to 0 on the facing
// alone, so the mech turns on the spot instead of backing; and state 8's
// way back to 0 drives ahead, the mech's too.
TEST(Bots_DriveBoxedInAhead)
{
	ai::DriveSense sense;
	sense.goalDistance = 100;
	sense.remaining = 200;
	sense.error = 0.05f; // to the right, cos 0.999
	sense.freeFront = 1; // short of 0.94 x 2.5
	sense.freeBack = 20;
	for (const ai::Driver driver : {ai::Driver::Car, ai::Driver::Tank, ai::Driver::Mech}) {
		ai::DriveSettings s;
		s.driver = driver;
		ai::DriveState st;
		st.state = 11;
		const ai::DriveOutput out = ai::driveControl(s, st, sense);
		if (driver == ai::Driver::Car) {
			CHECK(st.state == 11 && out.direction == -1 && out.throttle < 0);
		} else {
			CHECK(st.state == 0);
			CHECK(out.direction == (driver == ai::Driver::Mech ? 0 : 1));
		}
	}
	// State 8 with the goal no longer behind: back to 0, ahead.
	ai::DriveSettings mech;
	mech.driver = ai::Driver::Mech;
	ai::DriveState st;
	st.state = 8;
	sense.freeFront = 20;
	const ai::DriveOutput out = ai::driveControl(mech, st, sense);
	CHECK(st.state == 0 && out.direction == 1 && out.throttle == 1);
}

// The room rays (0x100e8170) start at the vehicle's spot on the map, and
// the cost-class byte the diagonal and side rays share is written only
// by a ray that enters a polygon: a diagonal that leaves the mesh in its
// first polygon reads the class the ray before it ended in. Here the
// front-left diagonal ends entering class 4 and the front-right one
// leaves the mesh at 0.5 m: neither counts, so the car drives on ahead.
TEST(Bots_DriveRoomRaysShareTheClass)
{
	ai::DriveSense sense;
	sense.goalDistance = 100;
	sense.remaining = 200;
	sense.hasRayStart = true;
	sense.rayStart = {0, 1, 0};
	sense.position = {0, 2, 0.3f}; // the origin, over its spot
	bool fromSpot = true;
	sense.ray = [&](const ai::Vec3& from, const ai::Vec3& to) {
		fromSpot = fromSpot && from.x == 0 && from.y == 1 && from.z == 0;
		ai::DriveRay ray;
		const float x = to.x - from.x, z = to.z - from.z;
		if (z > 0.1f && x < -0.1f) {
			ray.clear = false; // front left: into class 4 at 10 m
			ray.distance = 10;
			ray.lastClass = 4;
		} else if (z > 0.1f && x > 0.1f) {
			ray.clear = false; // front right: off the mesh at once
			ray.distance = 0.5f;
		}
		return ray;
	};
	ai::DriveSettings s;
	ai::DriveState st;
	const ai::DriveOutput out = ai::driveControl(s, st, sense);
	CHECK(fromSpot);
	CHECK(st.state == 0 && out.direction == 1 && out.throttle > 0);
	// The front-right ray first: nothing carried, it counts.
	sense.ray = [&](const ai::Vec3& from, const ai::Vec3& to) {
		ai::DriveRay ray;
		const float x = to.x - from.x, z = to.z - from.z;
		if (z > 0.1f && x > 0.1f) {
			ray.clear = false;
			ray.distance = 0.5f;
		}
		return ray;
	};
	ai::DriveState again;
	const ai::DriveOutput blocked = ai::driveControl(s, again, sense);
	CHECK(again.state != 0 || blocked.direction != 1);
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
		// The SAI's order stays the AI's: no commander order in the game
		// (nothing for the HUD to show), but the bot leader taking it
		// orders its squad (the leader's map attack, 0x008968d0).
		const game::GameWorld::SquadOrder* order = bots.squadOrder(2, squad);
		CHECK(order && order->type == game::GameWorld::OrderType::Attack);
		CHECK(order && (near(order->position.z, 60) || near(order->position.x, 100)));
		CHECK(!field.world->squadOrder(2, squad));
		const game::GameWorld::SquadOrder* leader = field.world->squadLeaderOrder(2, squad);
		CHECK(leader && leader->type == game::GameWorld::OrderType::MapAttack && !leader->fromCommander);
		CHECK(leader && order && near(leader->position.x, order->position.x) && near(leader->position.z, order->position.z));
	}
	CHECK(bots.behaviour(team[0]) == "MoveTo");
	for (int tick = 0; tick < 30 * 12; ++tick) {
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
	}
	// Walking to the neutral point at z 60 or PAC's at x 100.
	int closer = 0;
	for (int id : team) {
		game::GameWorld::PlayerInfo info;
		field.world->playerInfo(id, info);
		// (The members walk to their slots beside the leader and trail it:
		// a member's slot is given again only as it idles there or is
		// re-goaled, 0x100c0890.)
		closer += info.position.z > 10 || info.position.x > 10;
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
			gun->settings()["aiTemplate"] = name; // as the shipped rifles: the AI knows them by it
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
			const game::GameWorld::SquadOrder* order = bots.squadOrder(team, squad);
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
	// Nothing thrown out of the world on the way (finite, within 100 km).
	CHECK(world.objectsOutOfWorld().empty());
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

// The grenade warning (AIDLL 0x1004eab0, 0x10092960) and BBAvoid's 1000
// (0x10064850): a teammate's thrown grenade within its explosion radius in
// front of a bot sends it away (15 m, 0x10065360) until 2.8 s after the
// thrower took the grenade in hand. A weapon that explodes on impact
// doesn't warn.
TEST(Bots_GrenadeWarning)
{
	for (bool onImpact : {false, true}) {
		Field field;
		world::ObjectTemplate* frag = field.templates.create("GenericFireArm", "test_frag");
		frag->settings()["itemIndex"] = "4";
		frag->settings()["aiTemplate"] = "test_frag";
		frag->settings()["projectileTemplate"] = "test_frag_shell";
		frag->settings()["velocity"] = "6";
		frag->settings()["ammo.magSize"] = "1";
		frag->settings()["ammo.nrOfMags"] = "3";
		world::ObjectTemplate* shell = field.templates.create("GenericProjectile", "test_frag_shell");
		shell->settings()["physicsType"] = "RotationalPoint";
		shell->settings()["collision.bouncing"] = "1";
		shell->settings()["timeToLive"] = "CRD_NONE/2.8/0/0";
		shell->settings()["detonation.explosionDamage"] = "1";
		shell->settings()["detonation.explosionRadius"] = "9";
		world::ObjectTemplate* kit = field.templates.create("Kit", "test_thrower_kit");
		kit->children().push_back({"eu_ar_rifle", {}, {}});
		kit->children().push_back({"test_frag", {}, {}});
		Console console;
		game::BotSettings settings = scriptedSettings(console);
		std::string out;
		for (const char* line : {"weaponTemplate.create test_frag", "weaponTemplate.isThrown 1",
				 "weaponTemplate.setExplosionRadius 9"}) {
			console.execute(line, out);
		}
		if (onImpact) {
			console.execute("weaponTemplate.explodesOnImpact 1", out);
		}
		CHECK(settings.weapon("test_frag").thrown && settings.weapon("test_frag").explodesOnImpact == onImpact);
		game::BotManager bots(*field.world, settings);
		const int bot = field.soldier(2, true, -90, 0);
		const int thrower = field.world->addPlayer("Thrower", 2, false);
		field.world->setKit(thrower, "test_thrower_kit", "us_light_soldier");
		field.world->spawnSoldier(thrower);
		settle(bots, 0.2f, field.world.get());
		game::GameWorld::PlayerInfo info;
		field.world->playerInfo(bot, info);
		const float yaw = info.yaw * 3.14159265f / 180;
		// Three metres in front of the bot, facing it.
		field.move(thrower, info.position.x + 3 * std::sin(yaw), info.position.z + 3 * std::cos(yaw));
		game::SoldierInput input;
		input.selectItem = 4;
		field.world->setInput(thrower, input);
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
		input = {};
		input.fire = true;
		field.world->setInput(thrower, input);
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
		field.world->setInput(thrower, {});
		bool avoided = false;
		int ticks = 0;
		for (; ticks < 30 * 2; ++ticks) {
			bots.update(1 / 30.0f);
			field.world->tick(1 / 30.0f);
			avoided = avoided || bots.behaviour(bot) == "Avoid";
		}
		CHECK(avoided != onImpact);
		settle(bots, 1.5f, field.world.get()); // past 2.8 s after taking it in hand
		CHECK(bots.behaviour(bot) != "Avoid");
		if (!onImpact) {
			game::GameWorld::PlayerInfo now;
			field.world->playerInfo(bot, now);
			std::printf("  the bot moved %.1f m\n",
				std::sqrt(physics::lengthSquared(now.position - info.position)));
		}
	}
}

// The horn's target (0x100fc660 -> 0x100a5c00): a teammate on foot ahead
// on the line of travel, within 15 m beyond both radii; not an enemy, not
// one behind, not one off to the side.
TEST(Bots_HornTarget)
{
	std::vector<game::GameWorld::PlayerInfo> players(4);
	for (int i = 0; i < 4; ++i) {
		players[static_cast<size_t>(i)].id = i;
		players[static_cast<size_t>(i)].alive = true;
		players[static_cast<size_t>(i)].team = 2;
	}
	players[0].position = {0, 0, 0}; // the driver (in the car)
	players[0].vehicle = 99;
	players[1].position = {0, -1.3f, 12}; // ahead, its chest at the centre's height
	players[2].position = {6, -1.3f, 8};  // off to the side
	players[3].position = {0, -1.3f, -8}; // behind
	const physics::Vec3 centre {0, 0, 0}, way {0, 0, 1};
	CHECK(game::hornTarget(centre, way, 2.5f, 2, 0, players) == 1);
	players[1].position.z = 19; // 19 - 3 = 16 m beyond the radii
	CHECK(game::hornTarget(centre, way, 2.5f, 2, 0, players) == -1);
	players[1].position.z = 12;
	players[1].team = 1; // an enemy
	CHECK(game::hornTarget(centre, way, 2.5f, 2, 0, players) == -1);
	players[1].team = 2;
	CHECK(game::hornTarget(centre, {0, 0, -1}, 2.5f, 2, 0, players) == 3); // backing: the one behind
}

// The soldier's AI radius (BF2142.exe 0x006f6b90 through the AI adaptor's
// vt+0xdc): the built-in Soldier_CollisionMesh's box (0x006e4880: x and z
// +-0.4, y -1..0.8), its farthest corner from the origin (0x006aeda0).
TEST(Bots_SoldierAiRadius)
{
	CHECK(near(game::soldierAiRadius(), std::sqrt(1.32f)));
	CHECK(near(game::soldierAiRadius(), 1.1489f, 1e-3f));
}

// KitType (0x0088f6b0), the AI radio messages' types (BF2142.exe's table
// in 0x0088f000) and their times to live (aiComm.addAIRadioMessage), and a
// seat's select key (aiTemplatePlugIn.setSelectKey).
TEST(Bots_KitTypesRadioAndSelectKeys)
{
	CHECK(game::kitTypeIndex("Recon") == 0 && game::kitTypeIndex("KTAssault") == 1 && game::kitTypeIndex("Support") == 2);
	CHECK(game::kitTypeIndex("AT") == 3 && game::kitTypeIndex("Engineer") == 4 && game::kitTypeIndex("5") == 5);
	CHECK(game::kitTypeIndex("Sniper") == 7 && game::kitTypeIndex("Anti-Vehicle") == -1);
	CHECK(game::radioMessageType("bail_out") == 0 && game::radioMessageType("medic") == 3);
	CHECK(game::radioMessageType("need_repair") == 4 && game::radioMessageType("need_ammo") == 10);
	CHECK(game::radioMessageType("req_new_order") == 14 && game::radioMessageType("move") == 17);
	CHECK(game::radioMessageType("COMMSL_COMRADIO_divide") == 21 && game::radioMessageType("hello") == -1);
	Console console;
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	CHECK(near(settings.radioLifeOf(3), 20) && near(settings.radioLifeOf(14), 1));
	std::string out;
	for (const char* line : {"aiComm.addAIRadioMessage \"medic\" 10 20 7", "aiTemplatePlugIn.create Unit Gunner",
			 "aiTemplatePlugIn.setSelectKey PIPositionSelect2"}) {
		console.execute(line, out);
	}
	CHECK(near(settings.radioLifeOf(3), 7));
	CHECK(settings.aiPlugins["gunner"].selectKey == 2);
}

// The explosive's lob (0x100fa440): the higher arc; a round thrown along it
// comes down on the target point.
TEST(Bots_LobDirection)
{
	const float g = -14.73f, v = 20;
	for (const physics::Vec3& d : {physics::Vec3 {15, 0, 5}, physics::Vec3 {-8, 3, 12}, physics::Vec3 {20, -4, 0}}) {
		physics::Vec3 dir;
		CHECK(game::lobDirection(d, v, g, dir));
		CHECK(near(physics::lengthSquared(dir), 1, 1e-4f));
		const float across = std::sqrt(d.x * d.x + d.z * d.z);
		const float horizontal = std::sqrt(dir.x * dir.x + dir.z * dir.z);
		CHECK(dir.y > horizontal * (across > 0 ? std::fabs(d.y) / across : 0)); // above the line of sight
		const float t = across / (v * horizontal);
		CHECK(near(v * dir.y * t + 0.5f * g * t * t, d.y, 0.02f));
		CHECK(near(dir.x / horizontal, d.x / across, 1e-4f) && near(dir.z / horizontal, d.z / across, 1e-4f));
		CHECK(std::atan2(dir.y, horizontal) > 3.14159265f / 4); // the high arc
	}
	physics::Vec3 dir;
	CHECK(!game::lobDirection({40, 0, 0}, v, g, dir)); // beyond v^2 / g = 27 m
}

// The explosive bonus of the weapon choice (0x10074ef0).
TEST(Bots_ExplosivePick)
{
	game::ExplosivePick base;
	base.explosionRadius = 9;
	base.thrown = true;
	base.maxRange = 30;
	base.distance = 20;
	base.now = 100;
	base.seen = true;
	base.secondHand = true;
	base.seenAt = 99;
	CHECK(game::explosivePick(base));
	game::ExplosivePick p = base;
	p.secondHand = false; // seen by the bot itself
	CHECK(!game::explosivePick(p));
	p.heightAbove = 31; // high, but thrown
	CHECK(!game::explosivePick(p));
	p.thrown = false; // a launcher: over 30 m higher counts (the range grows by 1.25 dh)
	CHECK(game::explosivePick(p));
	p = base;
	p.distance = 9; // within 10 m when not more than 3 m higher
	CHECK(!game::explosivePick(p));
	p.heightAbove = 3.5f; // 5 m then
	CHECK(game::explosivePick(p));
	p = base;
	p.lobbing = true;
	p.distance = 9.5f; // the lob widens it by 1 m
	CHECK(game::explosivePick(p));
	p = base;
	p.distance = 31;
	CHECK(!game::explosivePick(p));
	p = base;
	p.seenAt = 89; // seen more than 10 s ago
	CHECK(!game::explosivePick(p));
	p = base;
	p.conditionFailedAt = 95; // the plan's condition failed within 10 s
	CHECK(!game::explosivePick(p));
	p = base;
	p.explosiveShots[0] = 92; // its last explosive round within 10 s
	CHECK(!game::explosivePick(p));
	p.thrown = false;
	p.fireMode2 = true; // automatic: the third last counts
	CHECK(game::explosivePick(p));
	p = base;
	p.targetSoldier = false;
	CHECK(!game::explosivePick(p));
	// In a vehicle: minRange up, seen within 5 s, the vehicle's stamp.
	p = base;
	p.soldierBot = false;
	p.secondHand = false;
	p.minRange = 25;
	CHECK(!game::explosivePick(p));
	p.minRange = 2;
	p.distance = 3;
	CHECK(game::explosivePick(p));
	p.seenAt = 94;
	CHECK(!game::explosivePick(p));
	p.seenAt = 99;
	p.vehicleFailedAt = 96;
	CHECK(!game::explosivePick(p));
}

// A bot's fire plan tells its squad of the target (0x100795a0 ->
// 0x10089b70 -> 0x100c1ce0); a squadmate that can't see the target knows
// it then, from others (flag 8). Its grenade would be lobbed at it (the
// weapon choice's bonus 0x10074ef0, the high arc 0x100fa440, the start
// condition 0x100d8910), but here the fire plan keeps to the order.
TEST(Bots_SquadTargetAndGrenadeLob)
{
	Field field;
	world::ObjectTemplate* frag = field.templates.create("GenericFireArm", "test_frag");
	frag->settings()["itemIndex"] = "4";
	frag->settings()["aiTemplate"] = "test_frag";
	frag->settings()["projectileTemplate"] = "test_frag_shell";
	frag->settings()["velocity"] = "20";
	frag->settings()["ammo.magSize"] = "1";
	frag->settings()["ammo.nrOfMags"] = "3";
	world::ObjectTemplate* shell = field.templates.create("GenericProjectile", "test_frag_shell");
	shell->settings()["physicsType"] = "RotationalPoint";
	shell->settings()["timeToLive"] = "CRD_NONE/3/0/0";
	shell->settings()["collision.bouncing"] = "1";
	shell->settings()["detonation.explosionDamage"] = "1";
	shell->settings()["detonation.explosionRadius"] = "9";
	world::ObjectTemplate* kit = field.templates.create("Kit", "test_grenadier_kit");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"test_frag", {}, {}});
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	// Seed 2 of the AI's twister: a stream whose first weapon choice (before
	// the fire plan keeps to the order) doesn't draw the grenade.
	settings.random = ai::Random::seeded(2);
	std::string out;
	for (const char* line : {"aiSettings.setViewDistance 15", "weaponTemplate.create test_frag", "weaponTemplate.isThrown 1",
			 "weaponTemplate.setExplosionRadius 9", "weaponTemplate.maxRange 40", "weaponTemplate.setStrength Infantry 1",
			 "weaponTemplate.create eu_ar_rifle", "weaponTemplate.maxRange 100", "weaponTemplate.setStrength Infantry 1"}) {
		console.execute(line, out);
	}
	game::BotManager bots(*field.world, settings);
	const int spotter = field.world->addPlayer("Spotter", 2, true);
	const int grenadier = field.world->addPlayer("Grenadier", 2, true);
	field.world->setKit(grenadier, "test_grenadier_kit", "us_light_soldier");
	for (int id : {grenadier, spotter}) { // the grenadier leads: it heads for the order, not a slot
		field.world->joinSquad(id, 1);
		field.world->spawnSoldier(id);
	}
	const int enemy = field.soldier(1, false, 0, 50);
	game::GameWorld::PlayerInfo target;
	field.world->playerInfo(enemy, target);
	field.world->setHitPoints(target.soldier, 1e6f); // the spotter's rounds don't end the fight
	field.move(spotter, 0, 42);   // 8 m from the enemy: within the 9.6 m it sees
	field.move(grenadier, 0, 30); // 20 m: out of its sight
	bool knew = false, picked = false, threw = false;
	for (int tick = 0; tick < 30 * 12 && !threw; ++tick) {
		field.move(enemy, 0, 50);
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
		knew = knew || bots.knows(grenadier, enemy);
		picked = picked || bots.explosive(grenadier);
		for (const game::Projectile& p : field.world->projectiles()) {
			threw = threw || (p.player == grenadier && p.settings && p.settings->bounces);
		}
	}
	// The grenadier's order (EU's attack on cp_pac at 100/0) lies over 30 m
	// away, the enemy on the other side: its fire plan keeps to the order
	// (+0x248, 0x100795a0), which withholds the explosive bonus from the
	// next weapon choice on (the first, before the plan, may give it), so
	// it fights with the rifle on its way and throws nothing.
	CHECK(knew);
	CHECK(bots.keepsToOrder(grenadier));
	CHECK(!threw);
	(void)picked;
}

// MedicAssist and the radio (0x10070200): a human squadmate's "medic"
// (type 3) heard and the best a human within 50 m: urgency 100 (the squad
// factor 1 for the bot's own task). A squadmate with a Medic kit (KitType 5)
// is left to itself (0x100c28e0).
TEST(Bots_MedicRadioAndKitTypes)
{
	for (bool medicKit : {false, true}) {
		Field field;
		world::ObjectTemplate* hub = field.templates.create("GenericFireArm", "test_hub_medic");
		hub->settings()["itemIndex"] = "4";
		hub->settings()["ammo.replenishingType"] = "RTHeal";
		hub->settings()["projectileTemplate"] = "test_hub_object";
		field.templates.create("SimpleObject", "test_hub_object");
		world::ObjectTemplate* kit = field.templates.create("Kit", "test_medic_kit");
		kit->children().push_back({"eu_ar_rifle", {}, {}});
		kit->children().push_back({"test_hub_medic", {}, {}});
		world::ObjectTemplate* patientKit = field.templates.create("Kit", "test_patient_kit");
		patientKit->settings()["kitType"] = medicKit ? "Medic" : "Assault";
		patientKit->children().push_back({"eu_ar_rifle", {}, {}});
		Console console;
		game::BotSettings settings = assistSettings(console);
		std::string out;
		console.execute("weaponTemplate.create test_hub_medic", out);
		console.execute("weaponTemplate.weaponType Healing", out);
		console.execute("weaponTemplate.maxRange 70", out);
		game::BotManager bots(*field.world, settings);
		const int medic = field.world->addPlayer("Medic", 2, true);
		field.world->setKit(medic, "test_medic_kit", "us_light_soldier");
		const int patient = field.world->addPlayer("Patient", 2, false);
		field.world->setKit(patient, "test_patient_kit", "us_light_soldier");
		for (int id : {medic, patient}) {
			field.world->joinSquad(id, 1);
			field.world->spawnSoldier(id);
		}
		field.move(medic, -90, 0);
		field.move(patient, -90, 30);
		game::GameWorld::PlayerInfo info;
		field.world->playerInfo(patient, info);
		field.world->setHitPoints(info.soldier, 90);
		settle(bots, 1.0f);
		const float before = bots.lastUrgency(medic, "MedicAssist");
		bots.radio(patient, "medic");
		settle(bots, 1.0f);
		const float after = bots.lastUrgency(medic, "MedicAssist");
		if (medicKit) {
			CHECK(before == 0 && after == 0);
		} else {
			CHECK(before > 0 && before < 100);
			CHECK(near(after, 100, 1e-3f)); // the bot's own task by then: factor 1
		}
	}
}

// Changing seats inside a vehicle (BBChange 0x1006c520 in a vehicle, the
// plan 0x1006dac0): the driver alone weighs its seat x2 (9), the gunner's
// seat as another's x0.77 (an ITGround root with Mobile, the bot driving:
// 0x1006b910): 2 x 8 x 0.77 = 12.3, and presses that seat's select key
// (setSelectKey PIPositionSelect2).
TEST(Bots_ChangesSeatInsideAVehicle)
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
	templates.registerType(11, "RotationalBundle");
	templates.registerType(12, "GenericFireArm");
	templates.create("Soldier", "us_light_soldier")->settings()["armor.maxHitPoints"] = "100";
	world::ObjectTemplate* point = templates.create("ControlPoint", "cp_eu");
	point->settings()["controlPointId"] = "2";
	point->settings()["team"] = "2";
	point->settings()["unableToChangeTeam"] = "1";
	objects.create(point, {-100, 0, 0});
	objects.create(templates.create("SpawnPoint", "sp"), {0, 0, 0})->setControlPointId(2);
	collisionTemplates.create("as_fav", "Objects/Vehicles/as_fav/");
	world::ObjectTemplate* car = templates.create("PlayerControlObject", "car");
	car->settings()["hasMobilePhysics"] = "1";
	car->settings()["physicsType"] = "Mesh";
	car->settings()["collisionMesh"] = "as_fav";
	car->settings()["seatInformation"] = "car 0/0.5/0.4 0/0/0";
	car->settings()["mass"] = "1800";
	car->settings()["aiTemplate"] = "TestCar";
	car->children().push_back({"car_entry", {-1.5f, 0, 0}, {}});
	car->children().push_back({"car_second", {0, 0, -0.5f}, {}});
	world::ObjectTemplate* second = templates.create("PlayerControlObject", "car_second");
	second->settings()["seatInformation"] = "car_second 0/0.5/-0.5 0/0/0";
	second->settings()["aiTemplate"] = "TestGunner";
	second->children().push_back({"car_base", {0, 0.8f, 0}, {}});
	world::ObjectTemplate* base = templates.create("RotationalBundle", "car_base");
	base->settings()["inputToYaw"] = "PIMouseLookX";
	base->children().push_back({"car_firearm", {0, 0.1f, 0}, {}});
	templates.create("GenericFireArm", "car_firearm")->settings()["projectileTemplate"] = "bullet";
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
			 "aiTemplate.create TestCar", "aiTemplate.addType ITGround", "aiTemplate.addPlugIn TestCar_Mobile",
			 "aiTemplatePlugIn.create Unit TestGunner_Unit", "aiTemplatePlugIn.equipmentTypeName Fixed",
			 "aiTemplatePlugIn.setSelectKey PIPositionSelect2", "aiTemplate.create TestGunner",
			 "aiTemplate.addPlugIn TestGunner_Unit", "weaponTemplate.create car_firearm",
			 "weaponTemplate.setStrength Infantry 2", "weaponTemplate.setStrength LightArmour 2",
			 "weaponTemplate.setStrength HeavyArmour 2", "weaponTemplate.setStrength Helicopter 2",
			 "aiSettings.createBehaviourModifiers UnitWeights", "aiSettings.setBehaviourModifier Idle 1.0",
			 "aiSettings.createBehaviourModifiers StandardWeights", "aiSettings.setBehaviourModifier Idle 0.1",
			 "aiSettings.setBehaviourModifier Change 1.9", "aiSettings.createUCConstant UCUnion 1.0",
			 "aiSettings.setVehicleBehaviour Car Change BBChange BBPChange 3 UCUnion UnitWeights",
			 "aiSettings.setVehicleBehaviour Car Idle BBIdle BBPIdle2d 5 UCUnion UnitWeights",
			 "aiSettings.setVehicleDefaultBehaviour Car Idle",
			 "aiSettings.setVehicleBehaviour Fixed Idle BBIdle BBPIdle2d 5 UCUnion UnitWeights",
			 "aiSettings.setVehicleDefaultBehaviour Fixed Idle"}) {
		console.execute(line, out);
	}
	CHECK(near(settings.weapon("car_firearm").strength[0], 2));
	game::GameWorld world(templates, objects);
	world.enablePhysics(files, collisionTemplates,
		{[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& normal) {
			 normal = {0, 1, 0};
			 return 0.0f;
		 }});
	world.autoRespawn = false;
	world.startRound();
	const uint32_t vehicle = objects.create(car, {3, 1.2f, 0}, {30, 0, 0})->id();
	const int bot = world.addPlayer("Bot", 2, true);
	CHECK(world.spawnSoldier(bot));
	game::GameWorld::PlayerInfo info;
	world.playerInfo(bot, info);
	world.setSoldierPosition(info.soldier, {0, 0, 0});
	for (int tick = 0; tick < 90; ++tick) {
		world.tick(1 / 30.0f); // the car settles
	}
	game::SoldierInput input;
	input.use = true;
	world.setInput(bot, input);
	world.tick(1 / 30.0f);
	world.setInput(bot, {});
	world.playerInfo(bot, info);
	CHECK(info.vehicle == vehicle && info.seat == 0);
	CHECK(world.vehicleSeats(vehicle).size() == 2);
	game::BotManager bots(world, settings);
	bool changing = false;
	for (int tick = 0; tick < 30 * 5 && info.seat != 1; ++tick) {
		bots.update(1 / 30.0f);
		world.tick(1 / 30.0f);
		changing = changing || bots.behaviour(bot) == "Change";
		world.playerInfo(bot, info);
	}
	CHECK(changing);
	CHECK(info.vehicle == vehicle && info.seat == 1);
	CHECK(near(bots.seatValueFor(bot, vehicle, 0), 4.5f, 0.05f)); // the driver's seat: its move only
	// Gunning now: its own seat 2 x 16 x 0.77 (the driver's seat empty)
	// against the driver's seat's 4.5: it stays.
	for (int tick = 0; tick < 30 * 3; ++tick) {
		bots.update(1 / 30.0f);
		world.tick(1 / 30.0f);
	}
	world.playerInfo(bot, info);
	CHECK(info.seat == 1);
}

// The weapon choice (0x10074ef0): the strength against the target's type,
// the range and ammo weights, the guidance kind and the bot's +0x248 and
// area guard holding the explosive bonus back, close combat taken at once,
// EMP weapons by the target's EMP level, C4 not twice.
TEST(Bots_WeaponChoice)
{
	game::WeaponChoice c;
	c.distance = 50;
	c.pick.now = 100;
	c.pick.seen = true;
	c.pick.secondHand = true;
	c.pick.seenAt = 99;
	game::WeaponOption rifle;
	rifle.strength = 1;
	rifle.minRange = 0;
	rifle.maxRange = 100;
	rifle.rounds = 300;
	rifle.inMag = 30;
	game::WeaponOption pistol = rifle;
	pistol.strength = 0.8f;
	pistol.maxRange = 30; // 20 m short: x (1 - 0.9) = 0.08
	bool explosive = true;
	CHECK(game::chooseWeapon(c, {pistol, rifle}, explosive) == 1 && !explosive);
	// The rifle 10 m beyond its maxRange: x0.1 < the pistol's 0.8 in range.
	c.distance = 25;
	rifle.maxRange = 15;
	CHECK(game::chooseWeapon(c, {pistol, rifle}, explosive) == 0);
	// 1 m beyond: x (1 - 0.9 x 0.2) = 0.82 > 0.8.
	c.distance = 16;
	CHECK(game::chooseWeapon(c, {pistol, rifle}, explosive) == 1);
	// Below minRange: x0.1 (just under) to x0.01.
	game::WeaponOption mortar = rifle;
	mortar.strength = 5;
	mortar.minRange = 30;
	mortar.maxRange = 100;
	CHECK(game::chooseWeapon(c, {pistol, mortar}, explosive) == 0); // 5 x 0.01 = 0.05
	c.distance = 29.9f;
	CHECK(game::chooseWeapon(c, {pistol, mortar}, explosive) == 0); // 5 x 0.0982 = 0.49
	mortar.strength = 9;
	CHECK(game::chooseWeapon(c, {pistol, mortar}, explosive) == 1); // 0.88
	// An empty magazine x0.33; one that reloads by itself in over 1.9 s: 0.1 (then x0.33).
	c.distance = 10;
	rifle.maxRange = 100;
	game::WeaponOption empty = rifle;
	empty.strength = 2.5f;
	empty.inMag = 0;
	CHECK(game::chooseWeapon(c, {pistol, empty}, explosive) == 1); // 0.825 > 0.8
	empty.autoReload = true;
	empty.reloadTime = 3;
	CHECK(game::chooseWeapon(c, {pistol, empty}, explosive) == 0); // 0.033
	empty.rounds = 0; // nothing left at all: dropped
	CHECK(game::chooseWeapon(c, {empty}, explosive) == -1);
	empty.rounds = -1; // no magazines: counts 0x10000
	CHECK(game::chooseWeapon(c, {empty}, explosive) == 0);
	// The explosive bonus x100 (and +0x249): unguided only, not with +0x248
	// or the area guard.
	game::WeaponOption launcher = rifle;
	launcher.strength = 0.1f;
	launcher.explosionRadius = 6;
	launcher.maxRange = 60;
	c.distance = 20;
	CHECK(game::chooseWeapon(c, {pistol, launcher}, explosive) == 1 && explosive);
	launcher.guidance = 3; // wire guided
	CHECK(game::chooseWeapon(c, {pistol, launcher}, explosive) == 0 && !explosive);
	launcher.locked = true; // with a target: x10 = 1 > 0.8
	CHECK(game::chooseWeapon(c, {pistol, launcher}, explosive) == 1 && !explosive);
	launcher.guidance = 0;
	launcher.locked = false;
	c.orderMove = true;
	CHECK(game::chooseWeapon(c, {pistol, launcher}, explosive) == 0 && !explosive);
	c.orderMove = false;
	c.targetInArea = true;
	CHECK(game::chooseWeapon(c, {pistol, launcher}, explosive) == 0 && !explosive);
	c.targetInArea = false;
	// From a vehicle the guards don't count.
	c.soldierBot = false;
	c.areaHold = true;
	CHECK(game::chooseWeapon(c, {pistol, launcher}, explosive) == 1 && explosive);
	c.soldierBot = true;
	c.areaHold = false;
	// Close combat: soldier to soldier within 3 m, taken at once when the
	// mesh ray reaches the target, else dropped beyond 3 m.
	game::WeaponOption knife;
	knife.type = game::WeaponAi::CloseCombat;
	knife.strength = 0.5f;
	knife.maxRange = 2;
	knife.meleeRay = true;
	c.distance = 2.5f;
	CHECK(game::chooseWeapon(c, {pistol, knife}, explosive) == 1);
	knife.meleeRay = false; // then weighed: 0.5 x 0.82 < 0.8
	CHECK(game::chooseWeapon(c, {pistol, knife}, explosive) == 0);
	knife.meleeRay = true;
	knife.meleeReachable = false; // the target off the mesh or in a vehicle's room
	CHECK(game::chooseWeapon(c, {pistol, knife}, explosive) == 0);
	knife.meleeReachable = true;
	c.distance = 5;
	CHECK(game::chooseWeapon(c, {knife}, explosive) == -1);
	// Going in: a human running at the bot with an empty magazine, the
	// bot's own empty, within 20 m.
	c.targetMotion = 3;
	c.targetInHandEmpty = true;
	c.inHandEmpty = true;
	c.targetTargetsMe = true;
	CHECK(game::chooseWeapon(c, {pistol, knife}, explosive) == 1);
	c.targetHuman = false; // never against a bot
	CHECK(game::chooseWeapon(c, {pistol, knife}, explosive) == 0);
	c.targetHuman = true;
	c.targetMotion = 0;
	CHECK(game::chooseWeapon(c, {pistol, knife}, explosive) == 0);
	// EMP: x (3 - level).
	game::WeaponOption emp = rifle;
	emp.emp = true;
	emp.strength = 0.4f;
	c.distance = 10;
	CHECK(game::chooseWeapon(c, {pistol, emp}, explosive) == 1); // 1.2
	c.targetEmpLevel = 1;
	CHECK(game::chooseWeapon(c, {pistol, emp}, explosive) == 0); // 0.8: not above
	// C4: not with the bot's own charge out.
	game::WeaponOption c4 = rifle;
	c4.type = game::WeaponAi::C4;
	c4.strength = 5;
	CHECK(game::chooseWeapon(c, {pistol, c4}, explosive) == 1);
	c.c4Placed = true;
	CHECK(game::chooseWeapon(c, {pistol, c4}, explosive) == 0);
	// A weapon that can't fire through transparent wants a clear sighting.
	rifle.firesThroughTransparent = false;
	c.clearSighting = false;
	CHECK(game::chooseWeapon(c, {pistol, rifle}, explosive) == 0);
}

// The guidance kind (BF2142.exe 0x00892be0 -> 0x008923a0): the target
// component's targetSystem, TSParabellic and none 0; the shipped AV is
// wire guided, the AA heat seeking.
TEST(Bots_GuidanceKind)
{
	world::ObjectTemplateManager templates;
	world::ObjectTemplate* rocket = templates.create("GenericFireArm", "test_rocket");
	CHECK(game::guidanceKind(rocket) == 0);
	CHECK(game::guidanceKind(nullptr) == 0);
	const std::pair<const char*, int> kinds[] = {{"TSNone", 0}, {"TSHeatSeeking", 1}, {"TSLaserGuided", 2},
		{"TSWireGuided", 3}, {"TSTVGuided", 4}, {"TSParabellic", 0}};
	for (const auto& [name, kind] : kinds) {
		rocket->settings()["target.targetSystem"] = name;
		CHECK(game::guidanceKind(rocket) == kind);
	}
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/bf2142");
	files.setMountingAllowed(true);
	CHECK(files.mountArchive("Objects/Weapons_server.zip", "Objects\\Weapons"));
	Console console {&files};
	world::ObjectTemplateManager shipped;
	world::ObjectManager objects;
	world::registerWorldConsoleMethods(console, shipped, objects);
	world::registerUntypedTemplateProperties(console, shipped, {"target.targetSystem"});
	std::string out;
	for (const char* file : {"Objects/Weapons/Handheld/EU_AV/eu_av.con", "Objects/Weapons/Handheld/EU_AV/eu_av.tweak",
			 "Objects/Weapons/Handheld/EU_AA/eu_aa.con", "Objects/Weapons/Handheld/EU_AA/eu_aa.tweak"}) {
		CHECK(console.run(file, {}, out));
	}
	CHECK(game::guidanceKind(shipped.find("eu_av")) == 3);
	CHECK(game::guidanceKind(shipped.find("eu_aa")) == 1);
}

// The explosive bonus's area guard (0x10034a10, 0x10074ef0): the area
// not held with the most radius / d^2; the bot within 40 m of it pushing
// its controls, or the target inside its radius.
TEST(Bots_StrategicAreaGuard)
{
	game::GuardArea mine, near, far;
	mine.held = true;
	mine.centre = {0, 0, 0};
	mine.radius = 50;
	near.held = false;
	near.centre = {30, 0, 0};
	near.spotX = 30;
	near.radius = 20;
	far.held = false;
	far.centre = {300, 0, 0};
	far.spotX = 300;
	far.radius = 100;
	bool hold = false, inside = false;
	game::strategicAreaGuard({mine, near, far}, {0, 0, 0}, true, {35, 0, 5}, hold, inside);
	CHECK(hold && inside); // 30 m from the near one's centre, moving; the target 7 m from its spot
	game::strategicAreaGuard({mine, near, far}, {0, 0, 0}, false, {35, 0, 5}, hold, inside);
	CHECK(!hold && inside); // standing still
	game::strategicAreaGuard({mine, near, far}, {0, 0, 0}, true, {60, 0, 0}, hold, inside);
	CHECK(hold && !inside);
	// Far from both: the near one still scores more (20/900 > 100/90000),
	// but the bot is over 40 m from it.
	game::strategicAreaGuard({mine, near, far}, {-50, 0, 0}, true, {300, 0, 0}, hold, inside);
	CHECK(!hold && !inside);
	// The far one wins next to it: the target inside its 100 m.
	game::strategicAreaGuard({mine, near, far}, {290, 0, 0}, true, {320, 0, 0}, hold, inside);
	CHECK(hold && inside);
	// Held areas don't count.
	game::strategicAreaGuard({mine}, {0, 0, 0}, true, {0, 0, 0}, hold, inside);
	CHECK(!hold && !inside);
}

// A launcher's lob whose condition holds (0x100d8910 with +0x208 set and
// the weapon not thrown) sets +0x209 (vt+0x20c(1)): the bot's moves stop
// (0x100e6060) while the fire plan runs.
TEST(Bots_LobHoldsTheMove)
{
	Field field;
	world::ObjectTemplate* launcher = field.templates.create("GenericFireArm", "test_launcher");
	launcher->settings()["itemIndex"] = "4";
	launcher->settings()["aiTemplate"] = "test_launcher";
	launcher->settings()["projectileTemplate"] = "test_launcher_shell";
	launcher->settings()["velocity"] = "40";
	launcher->settings()["ammo.magSize"] = "1";
	launcher->settings()["ammo.nrOfMags"] = "5";
	world::ObjectTemplate* shell = field.templates.create("GenericProjectile", "test_launcher_shell");
	shell->settings()["detonation.explosionDamage"] = "1";
	shell->settings()["detonation.explosionRadius"] = "6";
	world::ObjectTemplate* kit = field.templates.create("Kit", "test_launcher_kit");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"test_launcher", {}, {}});
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	std::string out;
	for (const char* line : {"aiSettings.setViewDistance 15", "weaponTemplate.create test_launcher",
			 "weaponTemplate.setExplosionRadius 6", "weaponTemplate.maxRange 60", "weaponTemplate.setStrength Infantry 1",
			 "weaponTemplate.create eu_ar_rifle", "weaponTemplate.maxRange 100", "weaponTemplate.setStrength Infantry 1"}) {
		console.execute(line, out);
	}
	game::BotManager bots(*field.world, settings);
	const int spotter = field.world->addPlayer("Spotter", 2, true);
	const int gunner = field.world->addPlayer("Gunner", 2, true);
	field.world->setKit(gunner, "test_launcher_kit", "us_light_soldier");
	for (int id : {spotter, gunner}) {
		field.world->joinSquad(id, 1);
		field.world->spawnSoldier(id);
	}
	const int enemy = field.soldier(1, false, 0, 50);
	const auto place = [&](int player, float x, float y, float z) {
		game::GameWorld::PlayerInfo info;
		field.world->playerInfo(player, info);
		field.world->setSoldierPosition(info.soldier, {x, y, z});
	};
	place(gunner, 0, 0, 25);
	bool held = false;
	physics::Vec3 heldAt;
	float drift = 0;
	int heldTicks = 0;
	for (int tick = 0; tick < 30 * 15 && heldTicks < 45; ++tick) {
		place(enemy, 0, 12, 50);  // 12 m up: a launcher lobs
		place(spotter, 0, 12, 42); // it sees the enemy and tells the squad
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
		game::GameWorld::PlayerInfo info;
		field.world->playerInfo(gunner, info);
		if (bots.holdingForLob(gunner)) {
			if (!held) {
				heldAt = info.position;
			}
			held = true;
			++heldTicks;
			const float dx = info.position.x - heldAt.x, dz = info.position.z - heldAt.z;
			drift = std::max(drift, std::sqrt(dx * dx + dz * dz));
		}
	}
	CHECK(held);
	CHECK(heldTicks > 0 && drift < 0.3f);
}

// The follower's jump (0x10092bc0, 0x10095720): a soldier pushing its
// controls without moving for half a second asks to jump (PIAction, which
// BotManager::steer holds 0.4 s), not again within 2 s (the follower held
// at its start fails on its blocked look-ahead after 2 s).
TEST(Bots_FollowerJump)
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
	ai::PathFollower follower;
	ai::FollowerSettings settings;
	settings.soldier = true;
	follower.request(mesh, from, {19.7761f, 106.928f, 189.227f}, settings, 0);
	CHECK(follower.planned() && !follower.failed());
	std::vector<float> jumps;
	for (float t = 0; t < 2.0f; t += 0.1f) {
		follower.progress(from, 1, t);
		if (follower.steer(mesh, from, t).jump) {
			jumps.push_back(t);
		}
	}
	CHECK(jumps.size() == 1); // once, then not within 2 s
	CHECK(!jumps.empty() && jumps[0] > 0.5f && jumps[0] < 0.75f);
	// A soldier that isn't pushing never jumps.
	ai::PathFollower idle;
	idle.request(mesh, from, {19.7761f, 106.928f, 189.227f}, settings, 0);
	bool jumped = false;
	for (float t = 0; t < 2.0f; t += 0.1f) {
		idle.progress(from, 0, t);
		jumped = jumped || idle.steer(mesh, from, t).jump;
	}
	CHECK(!jumped);
}

namespace {

// Cars on as_fav's collision mesh (the game's files) on flat ground: a
// driver's seat (TestCar, ITGround with a Mobile part, maxSpeed 20) and a
// gunner's (TestGunner, Fixed, select key 2) with `gun`.
struct CarField {
	io::FileManager files;
	Console console {&files};
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	physics::CollisionTemplateManager collisionTemplates;
	game::BotSettings settings;
	std::unique_ptr<game::GameWorld> world;

	explicit CarField(const char* gameDir)
	{
		files.setRootPath(gameDir);
		files.addSearchPath("mods/bf2142");
		bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
			[this](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
		std::string out;
		console.run("ServerArchives.con", {}, out);
		templates.registerType(1, "ControlPoint");
		templates.registerType(2, "SpawnPoint");
		templates.registerType(3, "Soldier");
		templates.registerType(7, "PlayerControlObject");
		templates.registerType(8, "EntryPoint");
		templates.registerType(9, "Engine");
		templates.registerType(10, "Spring");
		templates.registerType(11, "RotationalBundle");
		templates.registerType(12, "GenericFireArm");
		templates.create("Soldier", "us_light_soldier")->settings()["armor.maxHitPoints"] = "100";
		world::ObjectTemplate* point = templates.create("ControlPoint", "cp_eu");
		point->settings()["controlPointId"] = "2";
		point->settings()["team"] = "2";
		point->settings()["unableToChangeTeam"] = "1";
		objects.create(point, {-100, 0, 0});
		objects.create(templates.create("SpawnPoint", "sp"), {0, 0, 0})->setControlPointId(2);
		// A far point and spawn for team 1, for the enemies some tests add.
		world::ObjectTemplate* pac = templates.create("ControlPoint", "cp_pac");
		pac->settings()["controlPointId"] = "1";
		pac->settings()["team"] = "1";
		pac->settings()["unableToChangeTeam"] = "1";
		objects.create(pac, {400, 0, 400});
		objects.create(templates.find("sp"), {400, 0, 380})->setControlPointId(1);
		collisionTemplates.create("as_fav", "Objects/Vehicles/as_fav/");
		templates.create("EntryPoint", "car_entry")->settings()["entryRadius"] = "3";
		world::ObjectTemplate* engine = templates.create("Engine", "car_engine");
		engine->settings()["engineType"] = "194";
		world::ObjectTemplate* wheel = templates.create("Spring", "car_wheel");
		wheel->settings()["strength"] = "40";
		wheel->settings()["damping"] = "8";
		for (float x : {-0.95f, 0.95f}) {
			for (float z : {-1.4f, 1.6f}) {
				engine->children().push_back({"car_wheel", {x, -0.15f, z}, {}});
			}
		}
		addCar("car", "car_firearm");
		addCar("car_b", "car_firearm_b");
		game::registerBotConsoleMethods(console, settings);
		for (const char* line : {"aiTemplatePlugIn.create Mobile TestCar_Mobile", "aiTemplatePlugIn.maxSpeed 20",
				 "aiTemplate.create TestCar", "aiTemplate.addType ITGround", "aiTemplate.addPlugIn TestCar_Mobile",
				 "aiTemplatePlugIn.create Unit TestGunner_Unit", "aiTemplatePlugIn.equipmentTypeName Fixed",
				 "aiTemplatePlugIn.setSelectKey PIPositionSelect2", "aiTemplate.create TestGunner",
				 "aiTemplate.addPlugIn TestGunner_Unit", "weaponTemplate.create car_firearm",
				 "weaponTemplate.setStrength Infantry 2", "weaponTemplate.setStrength LightArmour 2",
				 "weaponTemplate.setStrength HeavyArmour 2", "weaponTemplate.setStrength Helicopter 2",
				 "weaponTemplate.create car_firearm_b", "weaponTemplate.setStrength Infantry 10",
				 "weaponTemplate.setStrength LightArmour 10", "weaponTemplate.setStrength HeavyArmour 10",
				 "weaponTemplate.setStrength Helicopter 10", "aiSettings.createBehaviourModifiers UnitWeights",
				 "aiSettings.setBehaviourModifier Idle 1.0", "aiSettings.createBehaviourModifiers StandardWeights",
				 "aiSettings.setBehaviourModifier Idle 0.1", "aiSettings.setBehaviourModifier Change 1.9",
				 "aiSettings.setBehaviourModifier Avoid 1.0", "aiSettings.createUCConstant UCUnion 1.0",
				 "aiSettings.setVehicleBehaviour Car Change BBChange BBPChange 3 UCUnion UnitWeights",
				 "aiSettings.setVehicleBehaviour Car Idle BBIdle BBPIdle2d 5 UCUnion UnitWeights",
				 "aiSettings.setVehicleDefaultBehaviour Car Idle",
				 "aiSettings.setVehicleBehaviour Fixed Change BBChange BBPChange 3 UCUnion UnitWeights",
				 "aiSettings.setVehicleBehaviour Fixed Idle BBIdle BBPIdle2d 5 UCUnion UnitWeights",
				 "aiSettings.setVehicleDefaultBehaviour Fixed Idle",
				 "aiSettings.setVehicleBehaviour Infantery Avoid BBAvoid BBPAvoidCollisionInfantery 1 UCUnion UnitWeights",
				 "aiSettings.setVehicleBehaviour Infantery Change BBChange BBPChange 3 UCUnion UnitWeights",
				 "aiSettings.setVehicleBehaviour Infantery Idle BBIdle BBPIdleInfantery 5 UCUnion UnitWeights",
				 "aiSettings.setVehicleDefaultBehaviour Infantery Idle"}) {
			console.execute(line, out);
		}
		world = std::make_unique<game::GameWorld>(templates, objects);
		world->enablePhysics(files, collisionTemplates,
			{[](float, float) { return 0.0f; }, [](float, float, physics::Vec3& normal) {
				 normal = {0, 1, 0};
				 return 0.0f;
			 }});
		world->autoRespawn = false;
		world->startRound();
	}
	void addCar(const std::string& name, const std::string& gun)
	{
		world::ObjectTemplate* car = templates.create("PlayerControlObject", name);
		car->settings()["hasMobilePhysics"] = "1";
		car->settings()["physicsType"] = "Mesh";
		car->settings()["collisionMesh"] = "as_fav";
		car->settings()["seatInformation"] = name + " 0/0.5/0.4 0/0/0";
		car->settings()["mass"] = "1800";
		car->settings()["aiTemplate"] = "TestCar";
		car->settings()["armor.maxHitPoints"] = "100";
		car->children().push_back({"car_entry", {-1.5f, 0, 0}, {}});
		car->children().push_back({name + "_second", {0, 0, -0.5f}, {}});
		car->children().push_back({"car_engine", {0, -0.44f, 0}, {}});
		world::ObjectTemplate* second = templates.create("PlayerControlObject", name + "_second");
		second->settings()["seatInformation"] = name + "_second 0/0.5/-0.5 0/0/0";
		second->settings()["aiTemplate"] = "TestGunner";
		second->children().push_back({name + "_base", {0, 0.8f, 0}, {}});
		world::ObjectTemplate* base = templates.create("RotationalBundle", name + "_base");
		base->settings()["inputToYaw"] = "PIMouseLookX";
		base->children().push_back({gun, {0, 0.1f, 0}, {}});
		templates.create("GenericFireArm", gun)->settings()["projectileTemplate"] = "bullet";
	}
	uint32_t car(const std::string& name, float x, float z)
	{
		return objects.create(templates.find(name), {x, 1.2f, z}, {30, 0, 0})->id();
	}
	int soldier(const std::string& name, bool ai, float x, float z)
	{
		const int id = world->addPlayer(name, 2, ai);
		world->spawnSoldier(id);
		game::GameWorld::PlayerInfo info;
		world->playerInfo(id, info);
		world->setSoldierPosition(info.soldier, {x, 0, z});
		return id;
	}
	void ticks(int n, game::BotManager* bots = nullptr)
	{
		for (int i = 0; i < n; ++i) {
			if (bots) {
				bots->update(1 / 30.0f);
			}
			world->tick(1 / 30.0f);
		}
	}
	// Gets a player in (use) and, for seat 1, to the gunner's seat.
	void board(int player, int seat)
	{
		game::SoldierInput input;
		input.use = true;
		world->setInput(player, input);
		world->tick(1 / 30.0f);
		world->setInput(player, {});
		world->tick(1 / 30.0f);
		if (seat > 0) {
			input = {};
			input.switchSeat = seat + 1;
			world->setInput(player, input);
			world->tick(1 / 30.0f);
			world->setInput(player, {});
			world->tick(1 / 30.0f);
		}
	}
	game::GameWorld::PlayerInfo info(int player)
	{
		game::GameWorld::PlayerInfo i;
		world->playerInfo(player, i);
		return i;
	}
};

} // namespace

// The radio in BBChange (0x1006c520): a human's bail_out (type 0) makes a
// driving bot's way out x modifier x 100 and it gets out; a human's get_in
// (type 1) from a car makes a bot on foot weigh that car x the modifier.
TEST(Bots_ChangeHearsBailOutAndGetIn)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	{
		CarField field(gameDir);
		const uint32_t car = field.car("car", 3, 0);
		const int bot = field.soldier("Bot", true, 0, 0);
		const int human = field.soldier("Human", false, -10, 0);
		field.ticks(90);
		field.board(bot, 0);
		CHECK(field.info(bot).vehicle == car && field.info(bot).seat == 0);
		game::BotManager bots(*field.world, field.settings);
		field.ticks(60, &bots);
		CHECK(field.info(bot).vehicle == car); // its seat (2 x 4.5) beats the soldier's own
		bots.radio(human, "bail_out");
		bool out = false;
		for (int tick = 0; tick < 30 * 3 && !out; ++tick) {
			field.ticks(1, &bots);
			out = field.info(bot).vehicle == 0;
		}
		CHECK(out);
	}
	{
		CarField field(gameDir);
		const uint32_t car = field.car("car", 3, 0);
		const int human = field.soldier("Human", false, 0, 0);
		const int bot = field.soldier("Bot", true, -30, 0);
		field.ticks(90);
		field.board(human, 0);
		CHECK(field.info(human).vehicle == car);
		game::BotManager bots(*field.world, field.settings);
		field.ticks(30, &bots);
		const float before = bots.lastUrgency(bot, "Change");
		bots.radio(human, "get_in");
		field.ticks(30, &bots);
		const float after = bots.lastUrgency(bot, "Change");
		CHECK(before > 0);
		CHECK(after > 5 * before); // x10, the same distance
	}
}

// The entry search (BF2142.exe 0x005a98e0) takes every EntryPoint whose
// root is the vehicle, wherever it hangs (us_heavy_mech's under its pelvis
// and turret bundles); and a bot heading for a car on foot keeps walking
// at it and pressing use (the get-in plan 0x1006dac0: MoveToObjectFinding
// beside a Trigger of PIUse within the two radii + 1 m) until it is in.
TEST(Bots_GetInThroughNestedEntryPoints)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	{
		CarField field(gameDir);
		field.addCar("car_nested", "car_firearm_n");
		world::ObjectTemplate* nested = field.templates.find("car_nested");
		nested->children().erase(nested->children().begin()); // the root's own entry
		world::ObjectTemplate* bundle = field.templates.create("RotationalBundle", "car_nested_bundle");
		bundle->children().push_back({"car_entry", {-1.5f, 0, 0}, {}});
		nested->children().push_back({"car_nested_bundle", {0, 0.2f, 0}, {}});
		const uint32_t car = field.car("car_nested", 3, 0);
		const int human = field.soldier("Human", false, 1, 0);
		field.ticks(60);
		CHECK(field.world->vehicleInReach(human));
		field.board(human, 0);
		CHECK(field.info(human).vehicle == car);
	}
	{
		CarField field(gameDir);
		const uint32_t car = field.car("car", 3, 0);
		const int bot = field.soldier("Bot", true, -12, 0);
		field.ticks(60);
		game::BotManager bots(*field.world, field.settings);
		bool in = false;
		for (int tick = 0; tick < 30 * 20 && !in; ++tick) {
			field.ticks(1, &bots);
			in = field.info(bot).vehicle == car;
		}
		CHECK(in);
	}
}

// From a vehicle BBChange weighs the other vehicles' seats too (0x1006c520):
// a gunner whose own seat is worth 2 x 16 x 0.77 = 24.6 leaves for a car
// whose gun is worth 2 x 40 x 0.77 = 61.6 nearby.
TEST(Bots_ChangeWeighsOtherVehiclesFromInside)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	CarField field(gameDir);
	const uint32_t car = field.car("car", 3, 0);
	const uint32_t other = field.car("car_b", 3, 12);
	const int bot = field.soldier("Bot", true, 0, 0);
	field.ticks(90);
	field.board(bot, 1);
	CHECK(field.info(bot).vehicle == car && field.info(bot).seat == 1);
	game::BotManager bots(*field.world, field.settings);
	bool left = false, boarded = false;
	for (int tick = 0; tick < 30 * 20 && !boarded; ++tick) {
		field.ticks(1, &bots);
		const game::GameWorld::PlayerInfo i = field.info(bot);
		left = left || i.vehicle != car;
		boarded = i.vehicle == other;
	}
	CHECK(left);
	CHECK(boarded);
}

// Avoid (0x10064850): a wreck within reach adds 100, x2 in the level's
// first 5 s (the comm's +0x78 stamp at 0 against the game's clock).
TEST(Bots_AvoidDoubledInTheFirstFiveSeconds)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	CarField field(gameDir);
	const uint32_t car = field.car("car", 3, 0);
	const int bot = field.soldier("Bot", true, 0, 0);
	field.ticks(90);
	field.world->setHitPoints(car, 10); // below 30 %: avoided like a wreck
	game::BotManager bots(*field.world, field.settings);
	float early = 0, late = 0;
	for (int tick = 0; tick < 30 * 8; ++tick) {
		field.world->setSoldierPosition(field.info(bot).soldier, {0, 0, 0});
		field.ticks(1, &bots);
		const float u = bots.lastUrgency(bot, "Avoid");
		if (tick < 30 * 4) {
			early = std::max(early, u);
		} else if (tick > 30 * 6) {
			late = std::max(late, u);
		}
	}
	CHECK(near(early, 200, 1e-3f));
	CHECK(near(late, 100, 1e-3f));
}

namespace {
// Two teams' soldiers on flat ground, for the radio's hand-out.
struct RadioField {
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	std::unique_ptr<game::GameWorld> world;
	RadioField()
	{
		templates.registerType(1, "ControlPoint");
		templates.registerType(2, "SpawnPoint");
		templates.registerType(3, "Soldier");
		templates.create("Soldier", "us_light_soldier")->settings()["armor.maxHitPoints"] = "100";
		templates.create("Soldier", "pac_light_soldier")->settings()["armor.maxHitPoints"] = "100";
		for (int team = 1; team <= 2; ++team) {
			world::ObjectTemplate* point = templates.create("ControlPoint", "cp" + std::to_string(team));
			point->settings()["controlPointId"] = std::to_string(team);
			point->settings()["team"] = std::to_string(team);
			point->settings()["unableToChangeTeam"] = "1";
			objects.create(point, {team == 1 ? -300.0f : 300.0f, 0, 0});
			objects.create(templates.create("SpawnPoint", "sp" + std::to_string(team)), {team == 1 ? -10.0f : 10.0f, 0, 0})
				->setControlPointId(team);
		}
		world = std::make_unique<game::GameWorld>(templates, objects);
		world->autoRespawn = false;
		world->startRound();
	}
	int soldier(const std::string& name, int team, float x, float z, bool ai = false)
	{
		const int id = world->addPlayer(name, team, ai);
		world->spawnSoldier(id);
		game::GameWorld::PlayerInfo info;
		world->playerInfo(id, info);
		world->setSoldierPosition(info.soldier, {x, 0, z});
		world->tick(1 / 30.0f);
		return id;
	}
};
} // namespace

// Who hears a radio message (0x004a6f70): the team channels, the local
// ones by the server's distances (100 m, and 15 m for enemies and the
// revive channels), the squad's, the commander's, a player's.
TEST(Radio_TargetChannelsReachTheirHearers)
{
	CHECK(game::GameWorld::targetChannel("TCTeam") == 1 && game::GameWorld::targetChannel("TCLocalTeam") == 10
		&& game::GameWorld::targetChannel("TCTeamAndLocalEnemies") == 17 && game::GameWorld::targetChannel("TCVehicle") == 11
		&& game::GameWorld::targetChannel("TCLocalGroundVehicle") == 7 && game::GameWorld::targetChannel("nope") == -1);
	RadioField f;
	const int me = f.soldier("Me", 2, 0, 0);
	const int mate = f.soldier("Mate", 2, 50, 0);
	const int far = f.soldier("Far", 2, 150, 0);
	const int enemy = f.soldier("Enemy", 1, 10, 0);
	const int enemyFar = f.soldier("EnemyFar", 1, 60, 0);
	game::GameWorld& w = *f.world;
	CHECK(w.hearsRadio(1, me, mate) && w.hearsRadio(1, me, far) && !w.hearsRadio(1, me, enemy) && !w.hearsRadio(1, me, me));
	CHECK(w.hearsRadio(17, me, enemy) && !w.hearsRadio(17, me, enemyFar) && w.hearsRadio(17, me, far));
	CHECK(w.hearsRadio(10, me, mate) && !w.hearsRadio(10, me, far) && w.hearsRadio(10, me, enemy) && !w.hearsRadio(10, me, enemyFar));
	CHECK(w.hearsRadio(9, me, enemyFar) && !w.hearsRadio(9, me, far));
	CHECK(!w.hearsRadio(16, me, mate) && w.hearsRadio(16, me, enemy));
	CHECK(w.hearsRadio(0, me, enemyFar) && w.hearsRadio(14, me, enemy, enemy) && !w.hearsRadio(14, me, mate, enemy));
	// TCSquad compares the squad ids (slot 0x120): two players in no squad
	// share "squad 0".
	CHECK(w.hearsRadio(3, me, mate) && !w.hearsRadio(3, me, enemy));
	CHECK(!w.hearsRadio(11, me, mate)); // on foot: no vehicle to share
	// Ground vehicles' channel: 15 m for anyone, else teammates aboard.
	CHECK(w.hearsRadio(7, me, enemy) && !w.hearsRadio(7, me, mate) && !w.hearsRadio(8, me, enemy));
	// The hand-out: each hearer once, in player order.
	w.radioMessage(me, 10, "roger_that");
	const auto heard = w.takeRadioHeard();
	CHECK(heard.size() == 2 && heard[0].receiver == mate && heard[1].receiver == enemy && heard[0].voice == "roger_that");
	CHECK(w.takeRadioHeard().empty());
}

// The bots hear through the world's hand-out and answer through their
// comm (0x100a7860 / 0x100a7af0): a get_in from a human's car makes a bot
// on foot head for it and say "roger_that" a second on (0x1006dac0); the
// low-health call says "medic" on TCLocalTeamMedic (0x100a7910), once per
// send period (20 s).
TEST(Bots_RadioHeardAndAnswered)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	{
		CarField field(gameDir);
		const uint32_t car = field.car("car", 3, 0);
		field.car("car_b", -30, 0); // out of the bot's reach (the get-in press, 0x1006dac0)
		const int human = field.soldier("Human", false, 0, 0);
		const int bot = field.soldier("Bot", true, -11, 0);
		field.ticks(90);
		field.board(human, 0);
		CHECK(field.info(human).vehicle == car);
		game::BotManager bots(*field.world, field.settings);
		field.ticks(30, &bots);
		field.world->takeRadioHeard();
		field.world->radioMessage(human, game::GameWorld::targetChannel("TCLocalTeam"), "get_in");
		field.ticks(60, &bots);
		const auto sent = bots.radioSent(bot);
		CHECK(!sent.empty() && sent.front().type == 6 && sent.front().channel == 6);
		// What the bot said reached the human (TCLocalTeam within 100 m).
		bool humanHeard = false;
		for (const auto& h : field.world->takeRadioHeard()) {
			humanHeard = humanHeard || (h.sender == bot && h.receiver == human && h.voice == "roger_that");
		}
		CHECK(humanHeard);
	}
	{
		CarField field(gameDir);
		const int bot = field.soldier("Bot", true, 0, 0);
		const int mate = field.soldier("Mate", false, 20, 0);
		(void)mate;
		field.ticks(30);
		game::BotManager bots(*field.world, field.settings);
		const uint32_t soldier = field.info(bot).soldier;
		field.world->setHitPoints(soldier, 30); // 0.3 of 100
		field.ticks(30, &bots);
		auto sent = bots.radioSent(bot);
		CHECK(sent.size() == 1 && sent[0].type == 3 && sent[0].channel == 0);
		field.ticks(30 * 5, &bots);
		CHECK(bots.radioSent(bot).size() == 1); // the 20 s send period
	}
}

// The ballistic lead (AIDLL 0x100fa060 / 0x100fa6f0): a round launched along
// the solved direction meets the target where the solver says, at the time
// it says, against gravity, drag and a moving target; the high arc for an
// indirect weapon; the time with drag (0x100fa100) against the flight
// (0x100fa200).
TEST(Bots_LeadSolverMeetsTheTarget)
{
	using ai::Vec3;
	const auto meets = [](const ai::RoundModel& round, Vec3 offset, Vec3 targetVelocity, bool high, float& angle,
						 int solves = 12) {
		ai::LeadSolver solver;
		Vec3 dir {0, 0, 1}, impact;
		double t = 0;
		bool ok = false;
		for (int i = 0; i < solves; ++i) { // the angle persists between solves, as each aim tick
			solver.setup(offset, targetVelocity, {}, {0, 0, 1}, 0, round);
			ok = solver.solve(0, 0.5f, high, dir, impact, &t);
		}
		angle = solver.angle();
		if (!ok) {
			return 1e9f;
		}
		Vec3 at;
		solver.roundAt(dir, static_cast<float>(t), at);
		const Vec3 target = offset + targetVelocity * static_cast<float>(t);
		const Vec3 d = at - target;
		return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	};
	float angle = 0;
	// Straight: no gravity, no drag, the target still.
	ai::RoundModel flat {300, 0, 0};
	CHECK(meets(flat, {0, 10, 200}, {}, false, angle) < 0.2f);
	CHECK(near(angle, std::atan2(10.0f, 200.0f), 0.002f));
	// Gravity: the arc above the line, the round on the target.
	ai::RoundModel rifle {300, 0, -14.73f};
	CHECK(meets(rifle, {0, 0, 400}, {}, false, angle) < 0.5f);
	CHECK(angle > 0.02f);
	// A moving target.
	CHECK(meets(rifle, {50, 0, 300}, {0, 0, 12}, false, angle) < 0.6f);
	// Drag.
	ai::RoundModel slow {120, 0.05f, -14.73f};
	CHECK(meets(slow, {0, 5, 250}, {}, false, angle) < 1.0f);
	// The high arc of an indirect weapon: found on the second solve (the
	// first only starts the search). Each solve pulls the angle halfway to
	// 1.508 before searching (0x100fa88e), so later solves wander round it
	// as the original's do.
	ai::RoundModel mortar {80, 0, -14.73f};
	CHECK(meets(mortar, {0, 0, 300}, {}, true, angle, 2) < 1.5f);
	CHECK(near(angle, 0.5f * (3.14159265f - std::asin(300 * 14.73f / 6400)), 0.01f));
	// The time with drag covers the distance the flight does.
	ai::LeadSolver solver;
	solver.setup({0, 0, 100}, {}, {}, {0, 0, 1}, 0, slow);
	const float t = solver.flightTime(100, 120);
	ai::RoundModel noGravity = slow;
	noGravity.gravity = 0;
	solver.setup({0, 0, 100}, {}, {}, {0, 0, 1}, 0, noGravity);
	CHECK(near(solver.flight({120, 0, 0}, t).x, 100, 1.5f));
	CHECK(solver.flightTime(1e6f, 1) == 1000);
	// The drag the AI weapon keeps (0x00892be0).
	CHECK(near(ai::roundDrag(10, 2, 1), 0.157f, 1e-3f));
	CHECK(ai::roundDrag(10, 2, 3) == 0.3f && ai::roundDrag(0, 2, 3) == 0);
	CHECK(ai::roundDrag(4, 2, 0) == 4);
}

// The on-target condition (0x100dcab0 / 0x100dcb00): within atan(0.0125 d)
// of the solution and a round fired now landing within d of the impact; a
// weapon that isn't automatic fires at the closest approach.
TEST(Bots_OnTargetWaitsForTheClosestApproach)
{
	using ai::Vec3;
	ai::RoundModel round {400, 0, 0};
	const auto aimed = [&](float yaw, ai::LeadSolver& solver, Vec3& dir, Vec3& impact, double& t) {
		solver.setup({0, 0, 200}, {}, {}, {std::sin(yaw), 0, std::cos(yaw)}, 0, round);
		dir = {0, 0, 1};
		return solver.solve(0, 0.5f, false, dir, impact, &t);
	};
	ai::LeadSolver solver;
	Vec3 dir, impact;
	double t = 0;
	ai::OnTargetTest automatic(0.5f, false);
	CHECK(near(automatic.maxAngle(), std::atan(0.00625f)));
	bool ok = aimed(0, solver, dir, impact, t);
	CHECK(automatic.test(ok, solver, dir, impact, t, 0));
	ok = aimed(0.01f, solver, dir, impact, t); // 2 m off at 200 m
	CHECK(!automatic.test(ok, solver, dir, impact, t, 0));
	CHECK(!automatic.test(false, solver, dir, impact, t, 0));
	// Sweeping across: held while closing, fired as the miss grows.
	ai::OnTargetTest single(0.5f, true);
	std::vector<bool> fired;
	for (float yaw : {0.004f, 0.002f, 0.0005f, -0.001f, -0.003f}) {
		ok = aimed(yaw, solver, dir, impact, t);
		fired.push_back(single.test(ok, solver, dir, impact, t, 0));
	}
	CHECK(!fired[0] && !fired[1] && !fired[2] && fired[3] && !fired[4]);
}

// The bail_out radio in BBChange (0x1006c520 -> 0x100a5990): a driver
// that takes the way out with its vehicle at or under the bot manager's
// floor + 0.1 (+0xdc 0.3, slot 0x68: 0.4) says bail_out (type 0) on the
// AI's channel 7 (TCVehicle).
TEST(Bots_BailOutRadioLeavingAWreck)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	CarField field(gameDir);
	const uint32_t car = field.car("car", 3, 0);
	const int bot = field.soldier("Bot", true, 0, 0);
	field.ticks(90);
	field.board(bot, 0);
	CHECK(field.info(bot).vehicle == car);
	game::BotManager bots(*field.world, field.settings);
	field.world->setHitPoints(car, 5);
	bool out = false;
	for (int tick = 0; tick < 30 * 3 && !out; ++tick) {
		field.ticks(1, &bots);
		out = field.info(bot).vehicle == 0;
	}
	CHECK(out);
	bool said = false;
	for (const game::BotManager::RadioSent& r : bots.radioSent(bot)) {
		said = said || (r.type == 0 && r.channel == 7);
	}
	CHECK(said);
}

// BBChangeLandingCraft (0x1006f460) inside its vehicle: a passenger whose
// driver's seat is free weighs that seat + 100 (a root with a Mobile part)
// and moves to it.
TEST(Bots_LandingCraftTakesTheFreeDriversSeat)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	CarField field(gameDir);
	std::string out;
	field.console.execute("aiSettings.setVehicleBehaviour Fixed Change BBChangeLandingCraft BBPChange 3 UCUnion UnitWeights", out);
	const uint32_t car = field.car("car", 3, 0);
	const int bot = field.soldier("Bot", true, 0, 0);
	field.ticks(90);
	field.board(bot, 1);
	CHECK(field.info(bot).vehicle == car && field.info(bot).seat == 1);
	game::BotManager bots(*field.world, field.settings);
	bool driving = false;
	float urgency = 0;
	for (int tick = 0; tick < 30 * 5 && !driving; ++tick) {
		field.ticks(1, &bots);
		urgency = std::max(urgency, bots.lastUrgency(bot, "Change"));
		driving = field.info(bot).vehicle == car && field.info(bot).seat == 0;
	}
	CHECK(urgency > 0);
	CHECK(driving);
}

// BBFire3d (0x1007e1b0 / 0x1007d6c0): a bot aboard with a Fire3d set
// scores a known enemy soldier and the urgency is fuzzy(2 best) x the
// weight (7.5 here, the fuzzy table saturated); a teammate scores nothing.
TEST(Bots_Fire3dScoresKnownEnemies)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	CarField field(gameDir);
	std::string out;
	for (const char* line : {"aiSettings.createUCConstant UCFire 1.0", "aiSettings.setBehaviourModifier Fire 7.5",
			 "aiSettings.setVehicleBehaviour Car Fire BBFire3d BBPHeliFire3d 2 UCFire UnitWeights",
			 "aiSettings.setVehicleBehaviour Fixed Fire BBFire3d BBPHeliFire3d 2 UCFire UnitWeights",
			 "aiSettings.createBehaviourModifiers UnitWeights", "aiSettings.setBehaviourModifier Idle 1.0",
			 "aiSettings.setBehaviourModifier Fire 1.0", "aiSettings.setViewDistance 200"}) {
		field.console.execute(line, out);
	}
	const uint32_t car = field.car("car", 3, 0);
	const int bot = field.soldier("Bot", true, 0, 0);
	const int mate = field.soldier("Mate", false, 3, 15);
	field.ticks(90);
	field.board(bot, 0);
	CHECK(field.info(bot).vehicle == car);
	game::BotManager bots(*field.world, field.settings);
	field.ticks(60, &bots);
	CHECK(bots.lastUrgency(bot, "Fire") == 0); // a teammate only
	field.templates.create("Soldier", "pac_light_soldier")->settings()["armor.maxHitPoints"] = "100";
	const int enemy = field.world->addPlayer("Enemy", 1, false);
	field.world->spawnSoldier(enemy);
	game::GameWorld::PlayerInfo e;
	field.world->playerInfo(enemy, e);
	const game::GameWorld::PlayerInfo me = field.info(bot);
	field.world->setSoldierPosition(e.soldier,
		{me.position.x + 4, 0, me.position.z + 1}); // close: seen whatever the view
	float urgency = 0;
	for (int tick = 0; tick < 30 * 3; ++tick) {
		field.ticks(1, &bots);
		urgency = std::max(urgency, bots.lastUrgency(bot, "Fire"));
	}
	(void)mate;
	CHECK(bots.knows(bot, enemy));
	CHECK(urgency > 0 && bots.target(bot) == enemy);
}

// The goal objects (src/AI/Goals): a waypoint goal is done within its
// reach (0x1008aaa0); a patrol puts the head to the back within its reach
// (0x1008b6c0, at least 5 m); Patrol3d compares the bot's height with the
// point's x (0x1008b920, as the original does); a fly-to of several points
// moves on within both reaches or once the bot gets farther (0x1008b230).
TEST(Bots_ScriptedGoals)
{
	ai::Goal move = ai::moveToGoal(3, {10, 0, 0});
	CHECK(move.kind == ai::Goal::Kind::Waypoints && move.areaRadius == 60);
	CHECK(ai::goalUrgency(move, {0, 0, 0}, 100, 1.15f) > 0 && !move.done);
	CHECK(ai::goalUrgency(move, {9, 0, 0}, 1, 1.15f) == 0 && move.done);

	ai::Goal patrol = ai::patrolGoal(2, {{0, 0, 0}, {20, 0, 0}});
	CHECK(patrol.radius == 5); // raised to 5
	CHECK(ai::goalUrgency(patrol, {3, 50, 0}, 0, 1) == 1); // within 5 m horizontally
	ai::Vec3 head;
	CHECK(ai::goalPoint(patrol, head) && head.x == 20 && patrol.points.size() == 2);
	CHECK(ai::goalUrgency(patrol, {10, 0, 0}, 0, 1) == 1 && ai::goalPoint(patrol, head) && head.x == 20);

	ai::Goal ring = ai::patrol3dGoal(4, {{30, 0, 0}, {0, 0, 0}});
	ai::goalUrgency(ring, {30, 0, 0}, 0, 1); // y 0 against x 30: no
	CHECK(ai::goalPoint(ring, head) && head.x == 30);
	ai::goalUrgency(ring, {30, 25, 0}, 0, 1); // |25 - 30| under 10
	CHECK(ai::goalPoint(ring, head) && head.x == 0);

	ai::Goal fly = ai::flyToGoal(10, 5, {0, 100, 0});
	CHECK(fly.kind == ai::Goal::Kind::Fly);
	CHECK(ai::goalUrgency(fly, {100, 100, 0}, 0, 1) == 1);
	CHECK(near(ai::goalUrgency(fly, {3, 90, 4}, 0, 1), (100 + 25) / (25 + 100.0f))); // inside both reaches: (dy² + d²) / (h² + r²)
	CHECK(ai::goalUrgency(fly, {3, 90, 4}, 0, 1, true) == 0); // holding within 50 m
	ai::Goal route = ai::flyToGoal(10, 5, {0, 100, 0});
	route.points.push_back({0, 100, 500});
	ai::goalUrgency(route, {0, 100, -300}, 0, 1);
	ai::goalUrgency(route, {0, 100, -200}, 0, 1); // closer: kept
	CHECK(route.points.size() == 2);
	ai::goalUrgency(route, {0, 100, 300}, 0, 1); // farther than at the last look: passed
	CHECK(route.points.size() == 1 && route.newPoint);
}

// A side's knowledge of a unit (0x100a9b60, 0x100a9b90): 1 while sensed,
// then 1 - S(t / degeneration); a degeneration of 0 keeps it at 1. The
// strategy's time (0x100a0130): at once the first time, then after
// timeLimit, 1.5 timeLimit...
TEST(Bots_KnowledgeCertaintyAndStrategyTime)
{
	CHECK(ai::knowledgeCertainty(true, 100, 10) == 1);
	CHECK(near(ai::knowledgeCertainty(false, 5, 10), 1 - ai::scurve(0.5f)));
	CHECK(ai::knowledgeCertainty(false, 20, 10) == 0);
	CHECK(ai::knowledgeCertainty(false, 20, 0) == 1);
	CHECK(ai::strategyTimeUp(0, 0, 100));
	CHECK(!ai::strategyTimeUp(99, 1, 100) && ai::strategyTimeUp(100, 1, 100));
	CHECK(!ai::strategyTimeUp(149, 2, 100) && ai::strategyTimeUp(150, 2, 100));
	CHECK(!ai::strategyTimeUp(1e8f, 5, 1e9f));
}

// The rest of AIDLL's settings commands (src/Game/BotsScript.cpp): the
// bot names (a random order, the first six towards the front; the head
// goes to the back), the auto controllers' templates over their defaults
// (0x100a8ed0), the interpreter table, the scheduling.
TEST(Bots_SettingsCommands)
{
	Console console;
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	std::string out;
	for (const char* line : {"aiSettings.addBotName S. Wallberg", "aiSettings.addBotName T. Karlsson",
			 "aiSettings.setNSides 2", "aiSettings.setMaxNAutoControllers 256", "aiSettings.maxBotsIncludeHumans 0",
			 "aiSettings.setInformationGridDimension 16", "aiSettings.setVehicleClusterMapping Car Tank",
			 "aiSettings.setManDownEquipment ManDown", "aiSettings.addInterpreterEntry Tank TankMoveTo",
			 "aiSettings.addInterpreterEntry Tank Sense", "aiSettings.removeInterpreterEntry Tank Sense",
			 "autoControllerTemplate.create SentryGun", "autoControllerTemplate.fov 30",
			 "autoControllerTemplate.viewDistance 63", "autoControllerTemplate.useSiren 1",
			 "aiBotManager.setDecisionMakingInterleave 3 1", "aiBotManager.setPlannedDecisionMakingThreshold -0.25"}) {
		CHECK(console.execute(line, out) == ConsoleResult::Ok);
	}
	CHECK(settings.sides == 2 && settings.maxAutoControllers == 256 && !settings.maxBotsIncludeHumans);
	CHECK(settings.informationGridDimension == 16 && settings.clusterMapping.at("Car") == "Tank");
	CHECK(settings.manDownEquipment == "ManDown");
	CHECK(settings.interpreter.at("Tank") == std::vector<std::string> {"TankMoveTo"});
	CHECK(settings.moveInterpreter("Tank") == "TankMoveTo" && settings.moveInterpreter("Mech") == "MechMoveTo"); // the table, the shipped one
	const game::BotSettings::AutoControllerTemplate* sentry = settings.autoController("sentrygun");
	CHECK(sentry && sentry->fov == 30 && sentry->viewDistance == 63 && sentry->useSiren);
	CHECK(sentry && near(sentry->scanningSpeed, 0.03f) && sentry->accuracy == 0.5f && sentry->maxTrackingInput == 4);
	CHECK(settings.plannedInterleave == 3 && settings.unplannedInterleave == 1 && settings.plannedThreshold == 0.25);
	const std::string first = settings.nextBotName();
	const std::string second = settings.nextBotName();
	CHECK(first != second && (first == "S. Wallberg" || first == "T. Karlsson"));
	CHECK(settings.nextBotName() == first);
	CHECK(console.execute("ai.addUnspawnedBot Someone 1 x", out) == ConsoleResult::Ok && out == "-1");
}

// The scripted-bot commands on a running manager: ai.botMoveTo <radius>
// <x/y/z> <bot> takes the place of the SAI's orders until the bot is
// there (0x100284a0); ai.botSetWaypoints_Patrol walks a ring (0x10028680);
// ai.deactivateBot takes the bot out of the AI (0x10055f80).
TEST(Bots_ScriptedMoveToAndPatrol)
{
	Field field;
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	game::BotManager bots(*field.world, settings);
	bots.attach(settings);
	const int bot = field.soldier(2, true, -100, 0);
	field.world->joinSquad(bot, 1);
	std::string out;
	console.execute("ai.botMoveTo 3 -100/0/50 " + std::to_string(bot), out);
	CHECK(bots.scriptedGoal(bot) != nullptr);
	game::GameWorld::PlayerInfo info;
	float closest = 1e9f;
	for (int tick = 0; tick < 30 * 20; ++tick) {
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
		field.world->playerInfo(bot, info);
		closest = std::min(closest, horizontal2(info.position - physics::Vec3 {-100, 0, 50}));
	}
	CHECK(closest < 4 * 4);
	CHECK(bots.scriptedGoal(bot) == nullptr); // done: the SAI's orders again

	console.execute("ai.botSetWaypoints_Patrol " + std::to_string(bot) + " 5 2 -100/0/30 -70/0/30", out);
	float nearA = 1e9f, nearB = 1e9f;
	int laps = 0;
	bool wasB = false;
	for (int tick = 0; tick < 30 * 40; ++tick) {
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
		field.world->playerInfo(bot, info);
		const float a = horizontal2(info.position - physics::Vec3 {-100, 0, 30});
		const float b = horizontal2(info.position - physics::Vec3 {-70, 0, 30});
		nearA = std::min(nearA, a);
		nearB = std::min(nearB, b);
		if (b < 36 && !wasB) {
			++laps;
		}
		wasB = b < 36;
	}
	CHECK(nearA < 36 && nearB < 36 && laps >= 2);
	CHECK(console.execute("ai.deactivateBot " + std::to_string(bot), out) == ConsoleResult::Ok && out == "1");
	CHECK(bots.deactivated(bot) && bots.behaviour(bot).empty());
	CHECK(console.execute("ai.getNumberOfDeactivedBots", out) == ConsoleResult::Ok && out == "1");
}

// aiStrategy.forceStrategy <name> <side> (0x1003a720): the side runs it at
// every update; an unknown name clears it.
TEST(Bots_ForceStrategy)
{
	Field field;
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	std::string out;
	for (const char* line : {"aiStrategy.createStrategy attack", "aiStrategy.NumberOfAttacks 1", "aiStrategy.createStrategy hold",
			 "aiStrategy.NumberOfDefences 1", "ai.addSAIStrategy 2 attack", "ai.addSAIStrategy 2 hold"}) {
		console.execute(line, out);
	}
	game::BotManager bots(*field.world, settings);
	bots.attach(settings);
	const int bot = field.soldier(2, true, -100, 0);
	field.world->joinSquad(bot, 1);
	settle(bots, 2, field.world.get());
	CHECK(console.execute("aiStrategy.forceStrategy hold 2", out) == ConsoleResult::Ok && out == "Strategy is activated : hold");
	settle(bots, 2, field.world.get());
	CHECK(bots.strategy(2) == "hold");
	CHECK(console.execute("aiStrategy.forceStrategy hold 3", out) == ConsoleResult::Ok
		&& out.rfind("Side must be 1 or 2", 0) == 0);
	console.execute("aiStrategy.forceStrategy nothing 2", out);
	CHECK(out == "No active strategy chosen : nothing" && bots.liveSettings().strategy.forced[2].empty());
}

// A side knows an enemy soldier only once its bots saw it (the side's AI
// object of it, 0x008a2f00 -> 0x1003b900), fully while one sees it, then
// fading over the template's degeneration (0x100a9b90).
TEST(Bots_SideKnowledge)
{
	Field field;
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	std::string out;
	for (const char* line : {"aiTemplate.create SoldierAi", "aiTemplate.degeneration 10"}) {
		console.execute(line, out);
	}
	for (const char* name : {"us_light_soldier", "pac_light_soldier"}) {
		field.templates.find(name)->settings()["aiTemplate"] = "SoldierAi";
	}
	game::BotManager bots(*field.world, settings);
	const int bot = field.soldier(2, true, -100, 0);
	const int enemy = field.soldier(1, false, -100, 300); // far out of sight
	settle(bots, 2, field.world.get());
	CHECK(bots.certainty(2, enemy) == 0 && bots.certainty(1, bot) == 0 && bots.certainty(2, bot) == 1);
	game::GameWorld::PlayerInfo me;
	field.world->playerInfo(bot, me);
	field.move(enemy, me.position.x + 4, me.position.z + 1); // close: seen whatever the view
	settle(bots, 1, field.world.get());
	CHECK(bots.knows(bot, enemy) && bots.certainty(2, enemy) == 1);
	field.move(enemy, -100, 400);
	settle(bots, 6, field.world.get());
	const float fading = bots.certainty(2, enemy);
	CHECK(fading > 0 && fading < 1);
	settle(bots, 10, field.world.get());
	CHECK(bots.certainty(2, enemy) == 0);
}

// The shipped AI scripts give the settings these commands keep: the bot
// names (BotNames.ai), the interpreter table and cluster mappings
// (AIBehaviours.ai), the auto controllers (AutoControllers.ai).
TEST(Bots_ShippedSettingsScripts)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/bf2142");
	Console console {&files};
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	std::string out;
	CHECK(console.run("AI/AIBehaviours.ai", {}, out));
	CHECK(console.run("AI/AIDefault.ai", {}, out));
	CHECK(settings.botNames.size() >= 50);
	CHECK(settings.interpreter.count("Tank") && settings.interpreter.at("Tank").front() == "InfoWrapper");
	CHECK(settings.clusterMapping.count("Car") && settings.clusterMapping.at("Car") == "Tank");
	CHECK(settings.moveInterpreter("ArmedCar") == "CarMoveTo" && settings.moveInterpreter("FloatingTank") == "FloatingTankMoveTo");
	CHECK(settings.moveInterpreter("TransportHelicopter") == "HeliMoveTo");
	CHECK(settings.manDownEquipment == "ManDown" && settings.maxBehaviours == 14 && settings.sides == 2);
	const game::BotSettings::AutoControllerTemplate* uav = settings.autoController("UAVDrone");
	CHECK(uav && uav->fov == 90 && uav->viewDistance == 100 && near(uav->scanningSpeed, 0.13f) && uav->neverAttackDeployer);
	CHECK(uav && uav->onlyAttackNetworkBattlefieldObjects && uav->discoverCloakedEnemiesDistance == 8);
	const game::BotSettings::AutoControllerTemplate* cannon = settings.autoController("CP_CANNON");
	CHECK(cannon && cannon->fov == 179 && cannon->reloadOnTeamChange && cannon->maxTrackingInput == 0.5f);
	CHECK(!settings.autoSpawnBots); // aiSettings.setAutoSpawnBots is no command of AIDLL's
}

// A bot against a stationary enemy 30 m off, with the game's AI scripts'
// weapon set-up: the rifle has an aiTemplate naming its weaponTemplate and
// no ObjectTemplate.velocity (as the shipped SMGs and handguns: 600 m/s,
// the GenericFireArm template's default, 0x005c2800); the kit also carries
// a ParachuteLauncher-like item (no aiTemplate, no magazine limit, 0 m/s),
// which AIDLL has no weapon object for and never picks (0x10074ef0). At the
// default skill the AI deviation spreads the rounds by 8.4 x 0.01 rad, yet
// the enemy goes down within 30 s, each hit taking the round's damage.
TEST(Bots_KillsAStationaryEnemy)
{
	Field field;
	for (const char* name : {"eu_ar_rifle", "as_ar_rifle"}) {
		field.templates.find(name)->settings().erase("velocity");
	}
	world::ObjectTemplate* chute = field.templates.create("GenericFireArm", "test_parachute");
	chute->settings()["itemIndex"] = "9";
	chute->settings()["projectileTemplate"] = "bullet";
	chute->settings()["velocity"] = "0";
	chute->settings()["ammo.magSize"] = "-1";
	chute->settings()["ammo.nrOfMags"] = "-1";
	world::ObjectTemplate* kit = field.templates.create("Kit", "test_kit");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"test_parachute", {}, {}});
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	std::string out;
	for (const char* line : {"weaponTemplate.create eu_ar_rifle", "weaponTemplate.maxRange 72",
			 "weaponTemplate.setStrength Infantry 5"}) {
		console.execute(line, out);
	}
	CHECK(settings.skill == 0.4f && settings.weapons.at("eu_ar_rifle").deviation == 14);
	game::BotManager bots(*field.world, settings);
	const int bot = field.world->addPlayer("Bot", 2, true);
	field.world->setKit(bot, "test_kit", "us_light_soldier");
	field.world->spawnSoldier(bot);
	field.move(bot, 0, 0);
	const int enemy = field.soldier(1, false, 0, 30);
	game::GameWorld::PlayerInfo target;
	field.world->playerInfo(enemy, target);
	CHECK(field.world->hitPoints(target.soldier) == 100);
	bool parachute = false, killed = false;
	float firstHit = 0;
	int tick = 0;
	for (; tick < 30 * 30 && !killed; ++tick) {
		field.move(enemy, 0, 30);
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
		game::GameWorld::PlayerInfo self, them;
		field.world->playerInfo(bot, self);
		field.world->playerInfo(enemy, them);
		parachute = parachute || self.inHandSlot == 9;
		if (them.alive && firstHit == 0 && field.world->hitPoints(them.soldier) < 100) {
			firstHit = 100 - field.world->hitPoints(them.soldier);
		}
		killed = !them.alive;
	}
	std::printf("  killed after %.1f s: %d rounds, %d hits\n", tick / 30.0f, field.fired(bot), field.hits(bot));
	CHECK(killed);
	CHECK(!parachute);
	CHECK(near(firstHit, 40)); // the bullet's damage, one hit's worth
	CHECK(field.hits(bot) >= 3 && field.fired(bot) > field.hits(bot));
}

// A seat gun's round leaves from where the bots aim it from
// (GameWorld::gunFrame: the seat's next barrel, turned with the gun and the
// hull), not from over the hull's centre.
TEST(Bots_GunFrameIsWhereTheRoundStarts)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	CarField field(gameDir);
	field.templates.find("car_firearm")->settings()["velocity"] = "100";
	field.templates.create("GenericProjectile", "bullet")->settings()["damage"] = "1";
	const uint32_t car = field.car("car", 3, 0);
	const int gunner = field.soldier("Gunner", false, 0, 0);
	field.ticks(90);
	field.board(gunner, 1);
	CHECK(field.info(gunner).vehicle == car && field.info(gunner).seat == 1);
	game::SoldierInput input;
	input.turn = 40;
	field.world->setInput(gunner, input);
	field.world->tick(1 / 30.0f);
	physics::Vec3 origin;
	physics::Mat3 hull;
	CHECK(field.world->gunFrame(car, 1, origin, hull));
	const size_t before = field.world->projectiles().size();
	input = {};
	input.fire = true;
	field.world->setInput(gunner, input);
	field.world->tick(1 / 30.0f);
	CHECK(field.world->projectiles().size() > before);
	if (field.world->projectiles().size() > before) {
		const game::Projectile& p = field.world->projectiles().back();
		CHECK(physics::lengthSquared(p.origin - origin) < 0.05f * 0.05f);
		float yaw = 0, pitch = 0;
		CHECK(field.world->gunAim(car, 1, yaw, pitch));
		// Its heading is the gun's in the hull's frame.
		const physics::Vec3 local = hull.untransform(p.velocity * (1 / std::sqrt(physics::lengthSquared(p.velocity))));
		CHECK(std::fabs(std::remainder(std::atan2(local.x, local.z) * 57.29578f - yaw, 360.0f)) < 1.0f);
	}
}

// The x3 on the AI deviation (TriggerContinously 0x1008ecd0's last
// argument to BF2142.exe 0x00892b70): a bot object that isn't ITAir or
// ITNaval and isn't primary (aiTemplate.secondary 1: the engine adaptor's
// slot 0xec is 0, vtables 0x009d5db8 / 0x009d5f30) at an ITAir target.
TEST(Bots_DeviationTripledForSecondaryAtAircraft)
{
	using game::aiDeviationFactor;
	const uint32_t ground = game::kITUnit | game::kITGround, air = game::kITAir | game::kITUnit;
	CHECK(aiDeviationFactor(ground, false, air) == 3);
	CHECK(aiDeviationFactor(ground, true, air) == 1);              // a primary object: a driver, a soldier
	CHECK(aiDeviationFactor(ground, false, ground) == 1);          // not at an aircraft
	CHECK(aiDeviationFactor(air, false, air) == 1);                // from the air
	CHECK(aiDeviationFactor(ground | game::kITNaval, false, air) == 1);
	CHECK(aiDeviationFactor(0, false, air) == 3);                  // no types (a pod's ITUnit-less template)
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	std::string out;
	for (const char* line : {"aiTemplate.create AS_APC_Podcannon1", "aiTemplate.addType ITUnit", "aiTemplate.addType ITGround",
			 "aiTemplate.secondary 1", "aiTemplate.create AS_APC", "aiTemplate.addType ITGround"}) {
		console.execute(line, out);
	}
	CHECK(!settings.primary("AS_APC_Podcannon1") && settings.primary("as_apc") && settings.primary("no_such_template"));
}

// The distance kept is the fire plan's (0x100795a0), made again when the
// weapon the choice settles on changes (0x1007cf10 keeps a plan only for
// the same target and weapon): a bot that switches to the knife keeps the
// knife's range, not the rifle's.
TEST(Bots_PreferredRangeFollowsTheWeapon)
{
	game::WeaponAi knife;
	knife.type = game::WeaponAi::CloseCombat;
	knife.maxRange = 1.375f;
	knife.optimal = 0.5f;
	CHECK(near(game::preferredRange(knife, 0.4f, 0.9f, -0.3f), 0.6875f)); // optimal alone for melee
	game::WeaponAi rifle;
	rifle.maxRange = 72;
	rifle.optimal = 0.35f;
	CHECK(near(game::preferredRange(rifle, 0.4f, 0.5f, 0), 72 * 0.35f * 0.4f)); // x max(0.2, skill) x U[0.8, 1.2]
	CHECK(near(game::preferredRange(rifle, 0.1f, 0.5f, 0), 72 * 0.35f * 0.2f));
	// + 0.25 dh above only in the explosive plan (0x100795a0 local_60).
	CHECK(near(game::preferredRange(rifle, 0.4f, 0.5f, 8), (72 + 1.25f * 8) * 0.35f * 0.4f));
	CHECK(near(game::preferredRange(rifle, 0.4f, 0.5f, 8, true), (72 + 1.25f * 8) * 0.35f * 0.4f + 0.25f * 8));
	// A thrown weapon in the explosive plan at a target over 3 m up keeps
	// 5 m at least (and no 1.25 dh or 0.25 dh).
	game::WeaponAi grenade;
	grenade.maxRange = 30;
	grenade.minRange = 1;
	grenade.optimal = 0.5f;
	grenade.thrown = true;
	CHECK(near(game::fireMinRange(grenade, 4, true), 5) && near(game::fireMinRange(grenade, 4, false), 1));
	CHECK(near(game::preferredRange(grenade, 0.4f, 0.5f, 8, true), 5 + 25 * 0.5f * 0.4f));
	CHECK(near(game::fireReach(rifle, 8), 82) && near(game::fireReach(grenade, 8), 30));

	Field field;
	world::ObjectTemplate* blade = field.templates.create("GenericFireArm", "test_knife");
	blade->settings()["itemIndex"] = "1";
	blade->settings()["aiTemplate"] = "test_knife";
	world::ObjectTemplate* kit = field.templates.create("Kit", "test_kit");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	kit->children().push_back({"test_knife", {}, {}});
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	std::string out;
	for (const char* line : {"weaponTemplate.create eu_ar_rifle", "weaponTemplate.maxRange 72",
			 "weaponTemplate.setStrength Infantry 5", "weaponTemplate.create test_knife", "weaponTemplate.weaponType CloseCombat",
			 "weaponTemplate.maxRange 1.375", "weaponTemplate.optimalRangePercentage 50", "weaponTemplate.setStrength Infantry 1"}) {
		console.execute(line, out);
	}
	game::BotManager bots(*field.world, settings);
	const int bot = field.world->addPlayer("Bot", 2, true);
	field.world->setKit(bot, "test_kit", "us_light_soldier");
	field.world->spawnSoldier(bot);
	field.move(bot, 0, 0);
	const int enemy = field.soldier(1, false, 0, 30);
	bool rifleFirst = false;
	for (int tick = 0; tick < 30 * 10 && !rifleFirst; ++tick) {
		field.move(enemy, 0, 30);
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
		rifleFirst = bots.planSlot(bot) == 3;
	}
	CHECK(rifleFirst);
	CHECK(bots.preferredRange(bot) > 5);
	// Close by, the choice takes the knife (soldier to soldier within 3 m):
	// the plan, and the distance, are made again for it.
	bool knifeNext = false;
	for (int tick = 0; tick < 30 * 5 && !knifeNext; ++tick) {
		game::GameWorld::PlayerInfo self;
		field.world->playerInfo(bot, self);
		field.move(enemy, self.position.x, self.position.z + 1.5f);
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
		knifeNext = bots.planSlot(bot) == 1;
	}
	CHECK(knifeNext);
	CHECK(bots.preferredRange(bot) <= 1.375f);
}

// Hearing (AIDLL 0x100ca280 -> 0x100ec100 -> 0x100ea820): the source's
// sphere by speed (BF2142.exe 0x008a1880), the dispatcher's reach, the
// roll's odds (a listener with a Mobile part never hears: 0x100317e0 is a
// max), the side's knowledge after a hearing (0x100a9bb0), and BBFire3d's
// unit-count scale (0x1007d6c0 against 0x100d2b30).
TEST(Bots_HearingAndUnitScale)
{
	game::AiPlugin mobile;
	mobile.maxSpeed = 5;
	mobile.soundSphereMin = 0;
	mobile.soundSphereMax = 15; // the soldiers' (Common/ai/Objects.ai)
	CHECK(near(game::soundSphereRadius(&mobile, 0), 0) && near(game::soundSphereRadius(&mobile, 2.5f), 7.5f));
	CHECK(near(game::soundSphereRadius(&mobile, 9), 15) && game::soundSphereRadius(nullptr, 3) == 0);
	game::AiPlugin defaults; // the Mobile constructor's (0x008917b0): 0 .. 30 m, 0.01 .. 0.1
	CHECK(defaults.soundSphereMin == 0 && defaults.soundSphereMax == 30 && near(defaults.hearingNear, 0.01f)
		&& near(defaults.hearingFar, 0.1f) && defaults.cullDistance == 1000);
	CHECK(game::WeaponAi().soundSphere == 50); // weaponTemplate +0x24 (0x0088a7b0)
	// The reach: the weapon's 50 m only within 3 s of taking it in hand.
	CHECK(game::hearingReach(true, 50, 10) == 50 && game::hearingReach(false, 50, 10) == 10);
	CHECK(game::hearingReach(false, 50, 0.05f) == 0);
	// The odds: a listener with no Mobile part (a gun seat) hears for sure
	// inside a sphere; one with a Mobile part never (the speed fraction is
	// at least 1).
	float p = 0;
	CHECK(game::hearingOdds(20, false, 50, 30, nullptr, 0, p) == 1 && p == 1);
	CHECK(game::hearingOdds(40, false, 50, 30, nullptr, 0, p) == 0); // outside both
	CHECK(game::hearingOdds(40, true, 50, 30, nullptr, 0, p) == 1);  // the weapon's 50 m
	CHECK(game::hearingOdds(10, true, 50, 30, &mobile, 0, p) <= 0 && near(p, 0.1f * 0.2f + 0.01f * 0.8f)); // r = 10 / 50
	CHECK(game::hearingOdds(10, true, 50, 30, &mobile, 20, p) <= 0);
	// Heard with certainty c: the last sighting set back 1 / (c deg).
	CHECK(near(game::heardLastSeen(100, 1, 15), 100 - 1 / 15.0f) && near(game::heardLastSeen(100, 0.1f, 10), 99));
	CHECK(game::heardLastSeen(100, 1, 0) == 100 && game::heardLastSeen(100, 0, 10) < -1e8f);
	CHECK(near(ai::knowledgeCertainty(false, 1 / 15.0f, 15), 1 - ai::scurve(1 / 225.0f)));
	// BBFire3d: summed battle strengths against the bot's type of 1 or less: x 0.01.
	CHECK(game::fire3dUnitScale(0) == 0.01f && game::fire3dUnitScale(1) == 0.01f && game::fire3dUnitScale(1.5f) == 1);
	// The commands: the plug-in's sphere and odds, its cull distance; the
	// weapon's sphere is weaponTemplate.soundSphereRadius (the shipped
	// scripts' setSoundSphereRadius is no command).
	Console console;
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	std::string out;
	for (const char* line : {"aiTemplatePlugIn.create Mobile M", "aiTemplatePlugIn.setSoundSphereRadius 0.0 75.0",
			 "aiTemplatePlugIn.setHearingProbability 0.01 0.3", "aiTemplatePlugIn.create Unit U",
			 "aiTemplatePlugIn.cullDistance 250", "weaponTemplate.create W", "weaponTemplate.soundSphereRadius 120"}) {
		console.execute(line, out);
	}
	CHECK(settings.aiPlugins.at("m").soundSphereMax == 75 && near(settings.aiPlugins.at("m").hearingFar, 0.3f));
	CHECK(settings.aiPlugins.at("u").cullDistance == 250 && settings.weapons.at("w").soundSphere == 120);
	console.execute("weaponTemplate.setSoundSphereRadius 30", out);
	CHECK(settings.weapons.at("w").soundSphere == 120);
}

// Vehicles are known per side (AIDLL's per-side AI objects, BF2142.exe
// 0x008a2f00): an enemy car no bot of the side has seen counts for
// nothing; seen it is certain, and 2 s after the bots lose it (0x100ee430
// counting the sensor down) its certainty decays by the template's
// degeneration from where the side last placed it (0x100a9b20).
TEST(Bots_VehicleKnowledgePerSide)
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
	objects.create(spawn, {0, 0, 0}, {0, 0, 0})->setControlPointId(2);
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
			 "aiTemplate.create TestCar", "aiTemplate.addType ITGround", "aiTemplate.addPlugIn TestCar_Mobile",
			 "aiTemplate.degeneration 10"}) {
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
	world::Object* made = objects.create(car, {0, 1.2f, 400}, {0, 0, 0});
	made->setTeam(1);
	const uint32_t vehicle = made->id();
	const int bot = world.addPlayer("Bot", 2, true);
	CHECK(world.spawnSoldier(bot));
	game::GameWorld::PlayerInfo info;
	world.playerInfo(bot, info);
	world.setSoldierPosition(info.soldier, {0, 0, 0});
	game::BotManager bots(world, settings);
	const auto run = [&](float seconds) {
		for (int tick = 0; tick < static_cast<int>(seconds * 30); ++tick) {
			bots.update(1 / 30.0f);
			world.tick(1 / 30.0f);
		}
	};
	run(2);
	CHECK(bots.vehicleTeam(vehicle) == 1 && bots.vehicleCertainty(2, vehicle) == 0 && bots.vehicleCertainty(1, vehicle) == 1);
	world.playerInfo(bot, info);
	world.setSoldierPosition(info.soldier, {0, 0, 393}); // 7 m off: seen whatever the view
	run(1);
	CHECK(bots.vehicleCertainty(2, vehicle) == 1);
	world.playerInfo(bot, info);
	world.setSoldierPosition(info.soldier, {0, 0, 0});
	run(1.5f);
	CHECK(bots.vehicleCertainty(2, vehicle) == 1); // still sensed: 2 s after losing it
	run(4);
	const float fading = bots.vehicleCertainty(2, vehicle);
	CHECK(fading > 0 && fading < 1);
	run(12);
	CHECK(bots.vehicleCertainty(2, vehicle) == 0);
	// A soldier never hears (its Mobile part): the car is never "heard".
	CHECK(bots.heardVehicles(bot).empty());
}

// ai.botGiveOrder <bot> <behaviour> <modifier set> (AIDLL 0x100065e0 ->
// 0x10028dd0 -> the bot's slot 0x40, 0x10098b80): refused while the bot's
// +0x50 isn't set (no writer of it was found), an unknown behaviour or set
// before that. The fire plan's keep-to-the-order uses the goal object's
// head, so a bot without a goal never keeps to one.
TEST(Bots_GiveOrderRefused)
{
	Field field;
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	settings.behaviours = {"Idle", "MoveTo", "Fire"}; // aiSettings.setBehaviour <id> <name>
	game::BotManager bots(*field.world, settings);
	const int bot = field.soldier(2, true, 0, 0);
	settle(bots, 1, field.world.get());
	CHECK(!bots.giveOrder(bot, "Fire", "StandardWeights") && bots.lastOrder(bot) == "Fire StandardWeights");
	CHECK(!bots.giveOrder(bot, "NoSuchBehaviour", "StandardWeights") && bots.lastOrder(bot) == "Fire StandardWeights");
	CHECK(!bots.giveOrder(bot, "Idle", "NoSuchSet") && bots.lastOrder(bot) == "Fire StandardWeights");
	CHECK(!bots.keepsToOrder(bot));
}

// The weapon handling (AIDLL 0x10099b70, after each execution's plan): out
// of the fire plan (bot +0x188, set by 0x100d4da0) a bot with its primary
// (c_PIWeaponSelect3) in hand presses c_PIReload while the magazine isn't
// full (rounds / magazine size, an integer division, below 0.8): the
// rifle, which has no ammo.autoReload, is topped up once the fight is over.
TEST(Bots_ReloadAfterTheFight)
{
	Field field;
	Console console;
	// A kit carrying the rifle in slot 3 (its itemIndex): the primary.
	field.templates.registerType(60, "Kit");
	field.templates.create("Kit", "EU_Assault")->children().push_back({"eu_ar_rifle", {}, {}});
	game::BotManager bots(*field.world, scriptedSettings(console));
	const int bot = field.soldier(2, true, 0, 0);
	const int enemy = field.soldier(1, false, 0, 25);
	const auto rifle = [&] {
		for (const auto& ammo : field.world->weaponAmmo(bot)) {
			if (ammo.weapon == "eu_ar_rifle") {
				return ammo;
			}
		}
		return game::GameWorld::WeaponAmmo {};
	};
	const int roundsBefore = rifle().rounds; // in the magazine and the spares
	for (int i = 0; i < 30 * 6 && field.fired(bot) < 5; ++i) {
		settle(bots, 1 / 30.0f + 1e-4f, field.world.get());
	}
	CHECK(field.fired(bot) >= 5);
	game::GameWorld::PlayerInfo info;
	field.world->playerInfo(enemy, info);
	field.world->setHitPoints(info.soldier, 0);
	field.move(enemy, 0, 3000);
	for (int i = 0; i < 30 * 8 && rifle().inMag < rifle().magSize; ++i) {
		settle(bots, 1 / 30.0f + 1e-4f, field.world.get());
	}
	std::fprintf(stderr, "  %d rounds fired; the rifle %d/%d, %d -> %d rounds in all (%s)\n", field.fired(bot), rifle().inMag,
		rifle().magSize, roundsBefore, rifle().rounds, bots.behaviour(bot).c_str());
	// Topped up: a fresh magazine in, the partial one's rounds gone with it.
	CHECK(rifle().inMag == rifle().magSize && rifle().rounds < roundsBefore - field.fired(bot));
}

// A driving bot fires its vehicle's guns (the Tank set's Fire is
// BBFireInfantery, whose plan aims with the driver's ControlInfo,
// MouseTurretAimAt on PIMouseLookX / PIMouseLookY, and triggers the gun
// as a gunner's): the turret turns at an enemy off the hull's line, the
// gun's rounds go out from the tank, and the vehicle's plan doesn't drive
// it while the target is within the gun's reach (0x100795a0's
// ObjectDistance 0x100d44c0).
TEST(Bots_DriverAimsAndFiresTheTurret)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	CarField field(gameDir);
	field.templates.create("GenericProjectile", "bullet")->settings()["damage"] = "1";
	world::ObjectTemplate* tank = field.templates.create("PlayerControlObject", "tank");
	for (const auto& [key, value] : field.templates.find("car")->settings()) {
		tank->settings()[key] = value;
	}
	tank->settings()["seatInformation"] = "tank 0/0.5/0.4 0/0/0";
	tank->settings()["aiTemplate"] = "TestTank";
	tank->children().push_back({"car_entry", {-1.5f, 0, 0}, {}});
	tank->children().push_back({"car_engine", {0, -0.44f, 0}, {}});
	tank->children().push_back({"tank_turret", {0, 0.9f, 0}, {}});
	world::ObjectTemplate* turret = field.templates.create("RotationalBundle", "tank_turret");
	turret->settings()["inputToYaw"] = "PIMouseLookX";
	turret->children().push_back({"tank_barrel", {0, 0.2f, 0.5f}, {}});
	world::ObjectTemplate* barrel = field.templates.create("RotationalBundle", "tank_barrel");
	barrel->settings()["inputToPitch"] = "PIMouseLookY";
	barrel->settings()["minRotation"] = "0/-20/0";
	barrel->settings()["maxRotation"] = "0/10/0";
	barrel->children().push_back({"tank_gun", {0, 0, 1}, {}});
	world::ObjectTemplate* gun = field.templates.create("GenericFireArm", "tank_gun");
	gun->settings()["projectileTemplate"] = "bullet";
	gun->settings()["velocity"] = "300";
	gun->settings()["aiTemplate"] = "tank_gun";
	gun->settings()["fire.roundsPerMinute"] = "120";
	std::string out;
	for (const char* line : {"aiTemplatePlugIn.create Mobile TestTank_Mobile", "aiTemplatePlugIn.maxSpeed 12",
			 "aiTemplatePlugIn.create Unit TestTank_Unit", "aiTemplatePlugIn.equipmentTypeName Tank",
			 "aiTemplate.create TestTank", "aiTemplate.addType ITGround", "aiTemplate.addPlugIn TestTank_Mobile",
			 "aiTemplate.addPlugIn TestTank_Unit", "weaponTemplate.create tank_gun", "weaponTemplate.setStrength Infantry 5",
			 "weaponTemplate.maxRange 150", "weaponTemplate.weaponType FireArm",
			 "weaponTemplate.allowedDeviation 2", "aiSettings.setBehaviourModifier Fire 7.5",
			 "aiSettings.createBehaviourModifiers UnitWeights", "aiSettings.setBehaviourModifier Idle 1.0",
			 "aiSettings.setBehaviourModifier Fire 1.0",
			 "aiSettings.setVehicleBehaviour Tank Idle BBIdle BBPIdle2d 5 UCUnion UnitWeights",
			 "aiSettings.setVehicleBehaviour Tank Fire BBFireInfantery BBPFireInfantery 4 UCUnion UnitWeights",
			 "aiSettings.setVehicleDefaultBehaviour Tank Idle", "aiSettings.setViewDistance 200"}) {
		field.console.execute(line, out);
	}
	const uint32_t id = field.objects.create(tank, {3, 1.2f, 0}, {0, 0, 0})->id();
	const int bot = field.soldier("Bot", true, 0, 0);
	field.ticks(90);
	field.board(bot, 0);
	CHECK(field.info(bot).vehicle == id && field.info(bot).seat == 0);
	CHECK(field.world->seatTurnsGun(id, 0));
	// An enemy 30 m off, 35 degrees right of the hull's line.
	field.templates.create("Soldier", "pac_light_soldier")->settings()["armor.maxHitPoints"] = "100";
	const int enemy = field.world->addPlayer("Enemy", 1, false);
	field.world->spawnSoldier(enemy);
	const float a = 35 * 3.14159265f / 180;
	const physics::Vec3 at = field.info(bot).position;
	field.world->setSoldierPosition(field.info(enemy).soldier, {at.x + 30 * std::sin(a), 0, at.z + 30 * std::cos(a)});
	game::BotManager bots(*field.world, field.settings);
	const physics::Vec3 start = field.info(bot).position;
	size_t fromTank = 0;
	for (int tick = 0; tick < 30 * 8; ++tick) {
		const size_t before = field.world->projectiles().size();
		field.ticks(1, &bots);
		for (size_t i = before; i < field.world->projectiles().size(); ++i) {
			fromTank += field.world->projectiles()[i].shooter == id;
		}
	}
	float yaw = 0, pitch = 0;
	CHECK(field.world->gunAim(id, 0, yaw, pitch));
	const float hull = field.objects.find(id)->rotation().x;
	const physics::Vec3 now = field.info(bot).position;
	std::printf("  behaviour %s, knows %d, fire urgency %.2f, enemy alive %d\n", bots.behaviour(bot).c_str(),
		bots.knows(bot, enemy), bots.lastUrgency(bot, "Fire"), field.info(enemy).alive);
	std::printf("  the driver bot's turret at %.1f off the hull (the enemy %.1f), %zu rounds from the tank, moved %.1f m\n",
		yaw, std::remainder(35 - hull, 360.0f), fromTank, std::sqrt(physics::lengthSquared(now - start)));
	CHECK(fromTank >= 2);
	CHECK(std::fabs(std::remainder(yaw + hull - 35, 360.0f)) < 8);
	CHECK(physics::lengthSquared(now - start) < 3.0f * 3.0f);
}

// The vehicle plan's move (0x100795a0: If NOT ObjectDistance [minRange,
// 1.1 D] then MoveToObjectFinding 0x100cff90): a target nearer than the
// move's near limit min(1.1 minRange, D) sends the driver off to a spot
// ahead of the target (0x100d00e0: its forward, the first of three spots
// clear) rather than at it.
TEST(Bots_DriverBacksOffInsideMinRange)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	CarField field(gameDir);
	field.templates.create("GenericProjectile", "bullet")->settings()["damage"] = "1";
	world::ObjectTemplate* tank = field.templates.create("PlayerControlObject", "tank");
	for (const auto& [key, value] : field.templates.find("car")->settings()) {
		tank->settings()[key] = value;
	}
	tank->settings()["seatInformation"] = "tank 0/0.5/0.4 0/0/0";
	tank->settings()["aiTemplate"] = "TestTank";
	tank->children().push_back({"car_entry", {-1.5f, 0, 0}, {}});
	tank->children().push_back({"car_engine", {0, -0.44f, 0}, {}});
	tank->children().push_back({"tank_turret", {0, 0.9f, 0}, {}});
	world::ObjectTemplate* turret = field.templates.create("RotationalBundle", "tank_turret");
	turret->settings()["inputToYaw"] = "PIMouseLookX";
	turret->children().push_back({"tank_barrel", {0, 0.2f, 0.5f}, {}});
	world::ObjectTemplate* barrel = field.templates.create("RotationalBundle", "tank_barrel");
	barrel->settings()["inputToPitch"] = "PIMouseLookY";
	barrel->settings()["minRotation"] = "0/-20/0";
	barrel->settings()["maxRotation"] = "0/10/0";
	barrel->children().push_back({"tank_gun", {0, 0, 1}, {}});
	world::ObjectTemplate* gun = field.templates.create("GenericFireArm", "tank_gun");
	gun->settings()["projectileTemplate"] = "bullet";
	gun->settings()["velocity"] = "300";
	gun->settings()["aiTemplate"] = "tank_gun";
	gun->settings()["fire.roundsPerMinute"] = "120";
	std::string out;
	for (const char* line : {"aiTemplatePlugIn.create Mobile TestTank_Mobile", "aiTemplatePlugIn.maxSpeed 12",
			 "aiTemplatePlugIn.create Unit TestTank_Unit", "aiTemplatePlugIn.equipmentTypeName Tank",
			 "aiTemplate.create TestTank", "aiTemplate.addType ITGround", "aiTemplate.addPlugIn TestTank_Mobile",
			 "aiTemplate.addPlugIn TestTank_Unit", "weaponTemplate.create tank_gun", "weaponTemplate.setStrength Infantry 5",
			 "weaponTemplate.maxRange 150", "weaponTemplate.minRange 25", "weaponTemplate.weaponType FireArm",
			 "weaponTemplate.allowedDeviation 2", "aiSettings.setBehaviourModifier Fire 7.5",
			 "aiSettings.createBehaviourModifiers UnitWeights", "aiSettings.setBehaviourModifier Idle 1.0",
			 "aiSettings.setBehaviourModifier Fire 1.0",
			 "aiSettings.setVehicleBehaviour Tank Idle BBIdle BBPIdle2d 5 UCUnion UnitWeights",
			 "aiSettings.setVehicleBehaviour Tank Fire BBFireInfantery BBPFireInfantery 4 UCUnion UnitWeights",
			 "aiSettings.setVehicleDefaultBehaviour Tank Idle", "aiSettings.setViewDistance 200"}) {
		field.console.execute(line, out);
	}
	const uint32_t id = field.objects.create(tank, {3, 1.2f, 0}, {0, 0, 0})->id();
	const int bot = field.soldier("Bot", true, 0, 0);
	field.ticks(90);
	field.board(bot, 0);
	CHECK(field.info(bot).vehicle == id && field.info(bot).seat == 0);
	CHECK(field.world->seatTurnsGun(id, 0));
	// An enemy 8 m off, 35 degrees right, facing +z (yaw 0): inside
	// minRange. The spot is ~30 m ahead of the enemy; the tank drives there.
	field.templates.create("Soldier", "pac_light_soldier")->settings()["armor.maxHitPoints"] = "100";
	const int enemy = field.world->addPlayer("Enemy", 1, false);
	field.world->spawnSoldier(enemy);
	const physics::Vec3 at = field.info(bot).position;
	field.world->setSoldierPosition(field.info(enemy).soldier, {at.x + 8 * std::sin(0.6108652f), 0, at.z + 8 * std::cos(0.6108652f)});
	game::BotManager bots(*field.world, field.settings);
	const physics::Vec3 start = field.info(bot).position;
	size_t fromTank = 0;
	for (int tick = 0; tick < 30 * 30; ++tick) {
		const size_t before = field.world->projectiles().size();
		field.ticks(1, &bots);
		for (size_t i = before; i < field.world->projectiles().size(); ++i) {
			fromTank += field.world->projectiles()[i].shooter == id;
		}
	}
	const physics::Vec3 enemyAt = field.info(enemy).position;
	const physics::Vec3 spot = game::backOffSpot(enemyAt, {0, 0, 1}, 25 * 1.1f, bots.preferredRange(bot), [](const physics::Vec3&) { return true; });
	const float before = std::sqrt(horizontal2(start - spot)), after = std::sqrt(horizontal2(field.info(bot).position - spot));
	std::printf("  the driver %.1f m from its back-off spot (%.1f m ahead of an enemy inside minRange 25), then %.1f m\n", before,
		std::sqrt(horizontal2(spot - enemyAt)), after);
	CHECK(after < before - 2);
	// The spot: 1.1 minRange + min(10, 0.2 (D - 1.1 minRange)) along the
	// target's forward, else its right, else its left.
	const physics::Vec3 o {10, 0, 10};
	const auto same = [](const physics::Vec3& a, const physics::Vec3& b) { return std::fabs(a.x - b.x) < 1e-3f && std::fabs(a.z - b.z) < 1e-3f; };
	CHECK(same(game::backOffSpot(o, {0, 0, 2}, 20, 30, [](const physics::Vec3&) { return true; }), {10, 0, 32}));
	CHECK(same(game::backOffSpot(o, {0, 0, 1}, 20, 200, [](const physics::Vec3&) { return true; }), {10, 0, 40}));
	CHECK(same(game::backOffSpot(o, {1, 0, 0}, 20, 30, [](const physics::Vec3& p) { return p.x < 15; }), {10, 0, -12}));
}

// BBRevive leaves out a body in deep water (0x10082760): under 0.5 m over
// the surface where it is more than 0.75 m deep.
TEST(Bots_ReviveSkipsDrownedBodies)
{
	CHECK(game::bodyUnderWater(-1.5f, 0, -2));   // lying 2 m deep
	CHECK(!game::bodyUnderWater(0.5f, 0, -2));   // on a deck 0.5 m over it
	CHECK(!game::bodyUnderWater(-0.5f, 0, -0.7f)); // a shallow ford
	CHECK(game::bodyUnderWater(0.4f, 0, -0.8f));
}

// The assist behaviours' upright test (0x10070200 / 0x10071a10 /
// 0x10073360): the target's up within 45 degrees of the terrain's normal.
TEST(Bots_AssistTargetsStandUpright)
{
	const auto unit = [](const physics::Vec3& v) { return v * (1 / std::sqrt(physics::lengthSquared(v))); };
	CHECK(game::uprightOnGround({0, 1, 0}, {0, 1, 0}));
	CHECK(game::uprightOnGround({0, 1, 0}, unit({0, 1, 0.99f}))); // a 44.7-degree slope
	CHECK(!game::uprightOnGround({0, 1, 0}, unit({0, 1, 1.02f}))); // past 45 degrees
	CHECK(!game::uprightOnGround({1, 0, 0}, {0, 1, 0}));  // a vehicle on its side
	CHECK(!game::uprightOnGround({0, -1, 0}, {0, 1, 0})); // on its roof
}

// BBChange's room ahead of a fixed-wing seat (0x1006c3e0, 0x100ff690):
// +-0.6 of the width and height, from 0.6 lengths behind to 12 ahead, level
// on the heading; a sphere counts by its radius.
TEST(Bots_FixedWingSeatNeedsRoomAhead)
{
	const physics::Vec3 o {0, 0, 0}, ahead {0, 0.5f, 1}; // nose up: the box stays level
	CHECK(game::sphereInRoomBox(o, ahead, 10, 4, 20, {0, 0, 239}, 0));
	CHECK(!game::sphereInRoomBox(o, ahead, 10, 4, 20, {0, 0, 241}, 0));
	CHECK(game::sphereInRoomBox(o, ahead, 10, 4, 20, {0, 0, 241}, 2));
	CHECK(game::sphereInRoomBox(o, ahead, 10, 4, 20, {0, 0, -11.9f}, 0));
	CHECK(!game::sphereInRoomBox(o, ahead, 10, 4, 20, {0, 0, -12.1f}, 0));
	CHECK(!game::sphereInRoomBox(o, ahead, 10, 4, 20, {6.1f, 0, 50}, 0));
	CHECK(!game::sphereInRoomBox(o, ahead, 10, 4, 20, {0, 2.5f, 50}, 0));
	// Turned to +x: the box runs along x.
	CHECK(game::sphereInRoomBox(o, {1, 0, 0}, 10, 4, 20, {200, 0, 0}, 0));
	CHECK(!game::sphereInRoomBox(o, {1, 0, 0}, 10, 4, 20, {0, 0, 200}, 0));
}

// The AI's clock seed (0x100309a0 with the word 0): the game clock's
// seconds x 1e6, rounded, the low 32 bits; the tests' word repeats.
TEST(Bots_SeedFromTheClock)
{
	CHECK(game::clockSeedWord(0) == 0);
	CHECK(game::clockSeedWord(1.5) == 1500000u);
	CHECK(game::clockSeedWord(0.0000004) == 0 && game::clockSeedWord(0.0000006) == 1);
	CHECK(game::clockSeedWord(5000.0) == static_cast<uint32_t>(5000000000ull)); // past 2^32: the low word
	CHECK(game::botRandomSeed(false) == game::botRandomSeed(false));
}

// BF2142_BOT_SEED (Bots.cpp, BotManager's constructor) starts the bots' own
// draws elsewhere: the same seed plays the same round twice over, tick for
// tick; another seed plays a different one.
TEST(Bots_SeedRepeatsTheRound)
{
	auto setSeed = [](const char* seed) {
#ifdef _WIN32
		_putenv_s("BF2142_BOT_SEED", seed);
#else
		setenv("BF2142_BOT_SEED", seed, 1);
#endif
	};
	// Bots_KillsAStationaryEnemy's fight, two bots against a soldier who
	// stands 30 m off: their moves, looks, rounds and hits each tick.
	auto play = [&](const char* seed) {
		setSeed(seed);
		Field field;
		world::ObjectTemplate* kit = field.templates.create("Kit", "test_kit");
		kit->children().push_back({"eu_ar_rifle", {}, {}});
		Console console;
		game::BotSettings settings = scriptedSettings(console);
		std::string out;
		for (const char* line : {"weaponTemplate.create eu_ar_rifle", "weaponTemplate.maxRange 72",
				 "weaponTemplate.setStrength Infantry 5"}) {
			console.execute(line, out);
		}
		game::BotManager bots(*field.world, settings);
		std::vector<int> ids;
		for (const float x : {-3.0f, 3.0f}) {
			const int id = field.world->addPlayer("Bot", 2, true);
			field.world->setKit(id, "test_kit", "us_light_soldier");
			field.world->spawnSoldier(id);
			field.move(id, x, 0);
			ids.push_back(id);
		}
		const int enemy = field.soldier(1, false, 0, 30);
		std::vector<float> trace;
		for (int tick = 0; tick < 10 * 30; ++tick) {
			field.move(enemy, 0, 30);
			bots.update(1 / 30.0f);
			field.world->tick(1 / 30.0f);
			for (const int id : ids) {
				game::GameWorld::PlayerInfo info;
				field.world->playerInfo(id, info);
				trace.push_back(info.position.x);
				trace.push_back(info.position.z);
				trace.push_back(info.yaw);
				trace.push_back(static_cast<float>(field.fired(id)));
				trace.push_back(static_cast<float>(field.hits(id)));
			}
			game::GameWorld::PlayerInfo them;
			field.world->playerInfo(enemy, them);
			trace.push_back(them.alive ? field.world->hitPoints(them.soldier) : -1.0f);
		}
		setSeed(""); // empty: the default sequence
		return trace;
	};
	const std::vector<float> first = play("7"), again = play("7"), other = play("8");
	CHECK(first.size() == again.size());
	CHECK(first == again);
	CHECK(first != other);
	// Something happened: rounds were fired.
	CHECK(first.size() > 11 && (first[first.size() - 8] > 0 || first[first.size() - 3] > 0));
}

// The SAI's status rule for an area without a control point (0x1005f210):
// a side's units alone take it when it is takeable for that side, the
// enemy's alone make it hostile when takeable for them; mixed or empty
// keeps the last status.
TEST(Bots_BoxAreaStatusRule)
{
	using ai::AreaStatus;
	CHECK(ai::boxAreaStatus(AreaStatus::Neutral, 2, 0, true, true) == AreaStatus::Owned);
	CHECK(ai::boxAreaStatus(AreaStatus::Neutral, 2, 0, false, true) == AreaStatus::Neutral);
	CHECK(ai::boxAreaStatus(AreaStatus::Owned, 0, 1, true, true) == AreaStatus::Hostile);
	CHECK(ai::boxAreaStatus(AreaStatus::Owned, 0, 1, true, false) == AreaStatus::Owned);
	CHECK(ai::boxAreaStatus(AreaStatus::Hostile, 1, 1, true, true) == AreaStatus::Hostile);
	CHECK(ai::boxAreaStatus(AreaStatus::Owned, 0, 0, true, true) == AreaStatus::Owned);
}

// The guided weapons' plan on foot (AIDLL 0x10078570, its fire statements
// 0x100781e0, the half-reach distance condition 0x100d4480): the move holds
// between minRange and half the reach, closes to 0.85 maxRange from
// farther and backs off to max(15, 1.1 minRange) from within minRange; a
// soldier bot with a wire-guided launcher fires at a target within [0.9
// minRange, 1.1 maxRange]; a heat-seeking one waits for a lock it can't
// get on a soldier.
TEST(Bots_GuidedPlanOnFoot)
{
	bool engaged = false;
	CHECK(game::guidedMoveStep(engaged, 35, 20, 100) == 0 && !engaged); // holds
	CHECK(game::guidedMoveStep(engaged, 95, 20, 100) == 1 && engaged);  // closes in...
	CHECK(game::guidedMoveStep(engaged, 86, 20, 100) == 1);
	CHECK(game::guidedMoveStep(engaged, 84, 20, 100) == 0 && !engaged); // ...to 85
	CHECK(game::guidedMoveStep(engaged, 60, 20, 100) == 0 && !engaged); // past half the reach but within the band
	CHECK(game::guidedMoveStep(engaged, 90, 20, 100) == 1 && engaged);
	CHECK(game::guidedMoveStep(engaged, 80, 20, 100) == 0 && !engaged);
	CHECK(game::guidedMoveStep(engaged, 12, 20, 100) == -1 && engaged); // within minRange: back
	CHECK(game::guidedMoveStep(engaged, 21, 20, 100) == -1);            // to max(15, 22)
	CHECK(game::guidedMoveStep(engaged, 22.5f, 20, 100) == 0 && !engaged);
	CHECK(game::guidedMoveStep(engaged, 4, 5, 100) == -1 && game::guidedMoveStep(engaged, 14, 5, 100) == -1); // 15 m at least
	CHECK(game::guidedMoveStep(engaged, 15.5f, 5, 100) == 0);

	for (const char* system : {"TSWireGuided", "TSHeatSeeking"}) {
		Field field;
		world::ObjectTemplate* launcher = field.templates.find("eu_ar_rifle");
		launcher->settings()["target.targetSystem"] = system;
		world::ObjectTemplate* kit = field.templates.create("Kit", "test_kit");
		kit->children().push_back({"eu_ar_rifle", {}, {}});
		Console console;
		game::BotSettings settings = scriptedSettings(console);
		std::string out;
		for (const char* line : {"weaponTemplate.create eu_ar_rifle", "weaponTemplate.minRange 20", "weaponTemplate.maxRange 100",
				 "weaponTemplate.setStrength Infantry 5"}) {
			console.execute(line, out);
		}
		game::BotManager bots(*field.world, settings);
		const int bot = field.world->addPlayer("Bot", 2, true);
		field.world->setKit(bot, "test_kit", "us_light_soldier");
		field.world->spawnSoldier(bot);
		field.move(bot, 0, 0);
		const int enemy = field.soldier(1, false, 0, 35);
		int nearRounds = 0;
		for (int tick = 0; tick < 30 * 8; ++tick) {
			// The target 35 m ahead of the bot, then 10 m (within 0.9 minRange).
			game::GameWorld::PlayerInfo self;
			field.world->playerInfo(bot, self);
			const bool close = tick >= 30 * 6;
			field.move(enemy, self.position.x, self.position.z + (close ? 10.0f : 35.0f));
			const int before = field.fired(bot);
			bots.update(1 / 30.0f);
			field.world->tick(1 / 30.0f);
			if (close && tick > 30 * 6 + 5) {
				nearRounds += field.fired(bot) - before;
			}
		}
		const bool wire = std::string(system) == "TSWireGuided";
		std::printf("  %s: %d rounds, %d of them within 0.9 minRange\n", system, field.fired(bot), nearRounds);
		CHECK(wire ? field.fired(bot) > 0 : field.fired(bot) == 0);
		CHECK(nearRounds == 0);
	}
}

// The bots' zoom (0x100d8250 and the moves' 0x100e5f70): a soldier bot
// with a scoped rifle zooms in standing to fire, the world hears it
// (setZoomed), and walking zooms it out again.
TEST(Bots_ZoomScopedWeapon)
{
	CHECK(game::zoomStatementPress(false, false, true));  // still, unzoomed: in
	CHECK(!game::zoomStatementPress(false, true, true));  // still, zoomed: let go
	CHECK(game::zoomStatementPress(true, true, true));    // moving, zoomed: out
	CHECK(!game::zoomStatementPress(true, false, true));
	CHECK(!game::zoomStatementPress(false, false, false)); // not ready
	CHECK(!game::zoomStatementPress(true, true, false));

	Field field;
	world::ObjectTemplate* rifle = field.templates.find("eu_ar_rifle");
	CHECK(!game::zoomSettingsOf(rifle).present()); // no DefaultZoomComp
	rifle->components().push_back("DefaultZoomComp");
	rifle->settings()["zoom.addZoomFactor.list"] = "0 0.4 0.4";
	rifle->settings()["zoom.zoomDelay"] = "0.2";
	const anim::ZoomSettings zoom = game::zoomSettingsOf(rifle);
	CHECK(zoom.present() && zoom.factors.size() == 2 && zoom.factors[1] == 0.4f && zoom.delay == 0.2f);
	world::ObjectTemplate* kit = field.templates.create("Kit", "test_kit");
	kit->children().push_back({"eu_ar_rifle", {}, {}});
	Console console;
	game::BotSettings settings = scriptedSettings(console);
	std::string out;
	for (const char* line : {"weaponTemplate.create eu_ar_rifle", "weaponTemplate.minRange 0", "weaponTemplate.maxRange 100",
			 "weaponTemplate.setStrength Infantry 5"}) {
		console.execute(line, out);
	}
	game::BotManager bots(*field.world, settings);
	const int bot = field.world->addPlayer("Bot", 2, true);
	field.world->setKit(bot, "test_kit", "us_light_soldier");
	field.world->spawnSoldier(bot);
	field.move(bot, 0, 0);
	const int enemy = field.soldier(1, false, 15, 0); // towards its goal (PAC's point at +x)
	bool zoomedFiring = false;
	for (int tick = 0; tick < 30 * 6; ++tick) {
		game::GameWorld::PlayerInfo self;
		field.world->playerInfo(bot, self);
		field.move(enemy, self.position.x + 15, self.position.z);
		const int before = field.fired(bot);
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
		zoomedFiring = zoomedFiring || (field.fired(bot) > before && bots.zoomLevel(bot) > 0);
	}
	std::printf("  zoom level %d, rounds %d\n", bots.zoomLevel(bot), field.fired(bot));
	CHECK(zoomedFiring);
	// The target far off: the plan walks after it and zooms out.
	bool walkedOut = false;
	for (int tick = 0; tick < 30 * 4; ++tick) {
		game::GameWorld::PlayerInfo self;
		field.world->playerInfo(bot, self);
		field.move(enemy, self.position.x + 95, self.position.z);
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
		walkedOut = walkedOut || bots.zoomLevel(bot) == 0;
	}
	CHECK(walkedOut);
}

namespace {

// A wall across z = 10 (both faces) of mesh material 0, 40 m wide.
physics::CollisionMeshFile wallMesh()
{
	physics::CollisionSubGeometry part;
	for (uint32_t type = 0; type < 3; ++type) {
		physics::CollisionLod lod;
		lod.collisionType = type;
		lod.vertices = {{-20, -1, 0}, {20, -1, 0}, {20, 20, 0}, {-20, 20, 0}};
		lod.boundsMin = {-20, -1, 0};
		lod.boundsMax = {20, 20, 0};
		lod.faces = {{{0, 1, 2}, 0}, {{0, 2, 3}, 0}, {{0, 2, 1}, 0}, {{0, 3, 2}, 0}};
		part.lods.push_back(lod);
	}
	physics::CollisionGeometry geometry;
	geometry.subGeometries = {part};
	physics::CollisionMeshFile mesh;
	mesh.version = 10;
	mesh.geometries = {geometry};
	return mesh;
}

} // namespace

// The sight ray passes see-through materials (BF2142.exe 0x00898040 with
// the filter 0x00897d50) and says so; the sighting then isn't clear (flag
// 0x40), and a weapon without setFiresThroughTransparent isn't chosen.
TEST(Bots_SightThroughSeeThrough)
{
	for (const bool glass : {false, true}) {
		for (const bool throughTransparent : {true, false}) {
			Field field([glass](Field& f) {
				f.materials.define(7).isSeeThrough = glass;
				f.collisionTemplates.add("wall_mesh", wallMesh());
				world::ObjectTemplate* wall = f.templates.create("SimpleObject", "test_wall");
				wall->settings()["collisionMesh"] = "wall_mesh";
				wall->settings()["mapMaterial.0"] = "7";
				f.objects.create(wall, {0, 0, 10});
			});
			bool through = false;
			const bool clear = field.world->lineOfSight({0, 1.5f, 0}, {0, 1.5f, 20}, 0, through);
			CHECK(clear == glass && through == glass);
			CHECK(field.world->lineOfSight({0, 1.5f, 0}, {0, 1.5f, 8}, 0, through) && !through);
			world::ObjectTemplate* kit = field.templates.create("Kit", "test_kit");
			kit->children().push_back({"eu_ar_rifle", {}, {}});
			Console console;
			game::BotSettings settings = scriptedSettings(console);
			// Seed 2 of the AI's twister: a stream whose draws let the
			// sighting's rounds go (some streams' aim never fires here).
			settings.random = ai::Random::seeded(2);
			std::string out;
			for (const char* line : {"weaponTemplate.create eu_ar_rifle", "weaponTemplate.minRange 0", "weaponTemplate.maxRange 100",
					 "weaponTemplate.setStrength Infantry 5"}) {
				console.execute(line, out);
			}
			console.execute(throughTransparent ? "weaponTemplate.setFiresThroughTransparent 1" : "weaponTemplate.setFiresThroughTransparent 0", out);
			game::BotManager bots(*field.world, settings);
			const int bot = field.world->addPlayer("Bot", 2, true);
			field.world->setKit(bot, "test_kit", "us_light_soldier");
			field.world->spawnSoldier(bot);
			field.move(bot, 0, 0);
			const int enemy = field.soldier(1, false, 0, 20);
			for (int tick = 0; tick < 30 * 4; ++tick) {
				field.move(bot, 0, 0);
				field.move(enemy, 0, 20);
				bots.update(1 / 30.0f);
				field.world->tick(1 / 30.0f);
			}
			std::printf("  glass %d, firesThroughTransparent %d: knows %d, rounds %d\n", glass, throughTransparent,
				bots.knows(bot, enemy), field.fired(bot));
			CHECK(bots.knows(bot, enemy) == glass);
			// The weapon choice leaves the rifle out without the transparent
			// flag but keeps the targets' loop's choice (none comes back).
			CHECK((field.fired(bot) > 0) == glass);
		}
	}
	// The choice itself (0x10074ef0: the memory item's flags 0x48).
	game::WeaponChoice c;
	c.distance = 20;
	game::WeaponOption o;
	o.usable = true;
	o.strength = 5;
	o.maxRange = 100;
	o.firesThroughTransparent = false;
	bool explosive = false;
	c.clearSighting = false;
	CHECK(game::chooseWeapon(c, {o}, explosive) == -1);
	c.clearSighting = true;
	CHECK(game::chooseWeapon(c, {o}, explosive) == 0);
	o.firesThroughTransparent = true;
	c.clearSighting = false;
	CHECK(game::chooseWeapon(c, {o}, explosive) == 0);
	// C4 against a target moving along the bot's facing at over 4 m/s.
	game::WeaponOption charge = o;
	charge.type = game::WeaponAi::C4;
	c.distance = 2;
	c.targetAlongFacing = -4.5f;
	CHECK(game::chooseWeapon(c, {charge}, explosive) == -1);
	c.targetAlongFacing = 3.5f;
	CHECK(game::chooseWeapon(c, {charge}, explosive) == 0);
}

// The bots' spotting (0x100a5850 -> 0x10030fb0): a first own sighting near
// a strategic area (30 m across for soldiers) is said on TCTeam and
// reported; the team's entry for the area holds the next infantry spot
// for 60 s; far from every area nothing is spotted.
TEST(Bots_SpotSightingsNearAreas)
{
	for (const bool nearArea : {true, false}) {
		Field field;
		Console console;
		game::BotSettings settings = scriptedSettings(console);
		std::string out;
		console.execute(nearArea ? "aiStrategicArea.create spot 10/10 30/30 0 50" : "aiStrategicArea.create spot 200/200 220/220 0 50", out);
		game::BotManager bots(*field.world, settings);
		const int bot = field.soldier(2, true, 0, 0);
		field.soldier(2, false, -5, -5); // a teammate to hear it
		const int first = field.soldier(1, false, 20, 20);
		const int second = field.soldier(1, false, 22, 18);
		field.world->takeRadioHeard();
		int spots = 0;
		bool secondKnown = false;
		for (int tick = 0; tick < 30 * 4; ++tick) {
			field.move(bot, 0, 0);
			field.move(first, 20, 20);
			field.move(second, 22, 18);
			bots.update(1 / 30.0f);
			field.world->tick(1 / 30.0f);
			std::set<std::string> said;
			for (const auto& heard : field.world->takeRadioHeard()) {
				if (heard.sender == bot && heard.channel == 1 && heard.voice.find("spotted") != std::string::npos) {
					said.insert(heard.voice);
				}
			}
			spots += static_cast<int>(!said.empty());
			secondKnown = secondKnown || bots.knows(bot, second);
		}
		std::printf("  near an area %d: knows %d %d, spots %d\n", nearArea, bots.knows(bot, first), secondKnown, spots);
		CHECK(bots.knows(bot, first) && secondKnown);
		CHECK(spots == (nearArea ? 1 : 0));
	}
}

// A squadmate hit by an enemy tells the squad's bots of the attacker
// (IAICollisionHandler slot 0x18, 0x1004f020 -> 0x10089b70), even one far
// out of their sight; a teammate's hit doesn't.
TEST(Bots_HitSharedWithSquad)
{
	for (const bool enemy : {true, false}) {
		Field field;
		Console console;
		game::BotSettings settings = scriptedSettings(console);
		game::BotManager bots(*field.world, settings);
		const int mate = field.world->addPlayer("Mate", 2, true);
		field.world->joinSquad(mate, 1);
		field.world->spawnSoldier(mate);
		field.move(mate, 0, 0);
		const int victim = field.world->addPlayer("Victim", 2, false);
		field.world->joinSquad(victim, 1);
		field.world->spawnSoldier(victim);
		field.move(victim, 5, 0);
		const int attacker = field.soldier(enemy ? 1 : 2, false, 0, 900);
		bots.update(1 / 30.0f);
		CHECK(!bots.knows(mate, attacker));
		game::GameWorld::PlayerInfo info;
		field.world->playerInfo(victim, info);
		field.world->applyDamage(info.soldier, 10, attacker);
		bots.update(1 / 30.0f);
		std::printf("  enemy %d: the mate knows the attacker %d\n", enemy, bots.knows(mate, attacker));
		CHECK(bots.knows(mate, attacker) == enemy);
	}
}

// The two AITemplatePlugIn rows the bipods and ATs set: the Unit's
// setUseNoPathfindingToGetToObject (+0x15, 0x00884eaf; AIDLL 0x100aa570)
// and the Mobile's vehicleNumber (+4, 0x008834f9; AIDLL 0x100a46c0), kept
// on the plug-ins (no longer stubs).
TEST(Bots_PlugInNoPathfindingAndVehicleNumber)
{
	Console console;
	game::BotSettings settings;
	game::registerBotConsoleMethods(console, settings);
	std::string out;
	CHECK(console.execute("aiTemplatePlugIn.create Unit hmg_bipod_unit", out) == ConsoleResult::Ok);
	CHECK(!settings.aiPlugins["hmg_bipod_unit"].noPathfindingToObject);
	CHECK(console.execute("aiTemplatePlugIn.setUseNoPathfindingToGetToObject 1", out) == ConsoleResult::Ok);
	CHECK(settings.aiPlugins["hmg_bipod_unit"].noPathfindingToObject);
	CHECK(console.execute("aiTemplatePlugIn.create Mobile as_at_mobile", out) == ConsoleResult::Ok);
	CHECK(settings.aiPlugins["as_at_mobile"].vehicleNumber == -1);
	CHECK(console.execute("aiTemplatePlugIn.vehicleNumber 2", out) == ConsoleResult::Ok);
	CHECK(settings.aiPlugins["as_at_mobile"].vehicleNumber == 2);
	CHECK(console.execute("aiTemplatePlugIn.vehicleNumber", out) == ConsoleResult::Ok && out.find('2') != std::string::npos);
}

// The squad formation's pure parts (AIDLL 0x100f58c0 / 0x100f53a0,
// 0x100f5c70, 0x100f4d30): a line abreast whose slots alternate right and
// left of their parents at 3 (r + r') apart, turned to the leader's way,
// and the trail dropped every 6 m.
TEST(Bots_FormationLineAndTrail)
{
	ai::Formation f;
	for (int i = 0; i < ai::Formation::kSlots; ++i) {
		ai::takeSlot(f, 10 + i);
	}
	CHECK(f.slots[5].used && f.slots[5].member == 15);
	ai::layOutFormation(f, [](int) { return 1.0f; });
	const float expect[6] = {0, 6, -6, 12, -12, 18};
	const int parents[6] = {0, 0, 0, 1, 2, 3};
	for (int i = 0; i < 6; ++i) {
		CHECK(near(f.slots[static_cast<size_t>(i)].offset.x, expect[i]) && near(f.slots[static_cast<size_t>(i)].offset.z, 0));
		CHECK(f.slots[static_cast<size_t>(i)].parent == parents[i]);
	}
	// Facing +x, the right is -z.
	f.dirX = 1;
	f.dirZ = 0;
	const ai::Vec3 turned = ai::turnOffset(f, {6, 0, 0});
	CHECK(near(turned.x, 0) && near(turned.z, -6));
	const ai::Vec3 ahead = ai::turnOffset(f, {0, 0, 2});
	CHECK(near(ahead.x, 2) && near(ahead.z, 0));
	// The wedge steps back 0.6 per 0.8 aside.
	f.shape = 1;
	ai::layOutFormation(f, [](int) { return 1.0f; });
	CHECK(near(f.slots[1].offset.x, 4.8f) && near(f.slots[1].offset.z, -3.6f));
	// The trail: the first spot is kept, a point dropped past 6 m.
	f.centre = {1, 0, 0};
	ai::dropCrumb(f);
	CHECK(!f.crumbs[0].valid && near(f.lastCrumb.x, 1));
	f.centre = {6, 0, 0};
	ai::dropCrumb(f);
	CHECK(!f.crumbs[0].valid);
	f.centre = {8, 0, 0};
	ai::dropCrumb(f);
	CHECK(f.crumbs[0].valid && near(f.crumbs[0].at.x, 1) && near(f.lastCrumb.x, 8));
	// Moved: forced, or 2 m on after 0.2 s.
	ai::markMoved(f, true, 0);
	CHECK(f.moved && near(f.movedFrom.x, 8));
	f.centre = {9, 0, 0};
	ai::markMoved(f, false, 0.1);
	CHECK(!f.moved);
	f.centre = {11, 0, 0};
	ai::markMoved(f, false, 0.3);
	CHECK(f.moved);
}

// The squad taking an order (0x100c1eb0, 0x100c21b0) and its formation
// (0x100c0890): three EU bots in a squad take the SAI's attack order (on
// PAC's point); the leader accepts it, says attack_position (16) on the
// squad's channel (3) and orders its squad in the game (the leader's map
// attack: the SAI's own order never reaches the game); the leader heads for
// the order, the members for their slots in the line beside its spot
// (3 (r + r) = 6.89 m aside). A second order is ignored while this one is
// under way and no member is near it.
TEST(Bots_SquadTakesAnOrderInFormation)
{
	Field field;
	game::BotSettings settings;
	game::BotManager bots(*field.world, settings);
	std::vector<int> team;
	for (int i = 0; i < 3; ++i) {
		const int id = field.world->addPlayer("Eu", 2, true);
		field.world->joinSquad(id, 1);
		field.world->spawnSoldier(id);
		field.move(id, static_cast<float>(i) * 3, 0);
		team.push_back(id);
	}
	// Over six of the squad's looks (one every 18 frames): the offsets are
	// laid out for the members on the sixth (0x100f6dc0).
	for (int tick = 0; tick < 18 * 7; ++tick) {
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
	}
	const game::BotManager::Squad* squad = bots.squad(2, 1);
	const game::GameWorld::SquadOrder* order = bots.squadOrder(2, 1);
	CHECK(squad && order && squad->members.size() == 3 && squad->state == 2 && squad->kind == 0);
	CHECK(squad && order && std::fabs(squad->orderAt.x - order->position.x) < 1
		&& std::fabs(squad->orderAt.z - order->position.z) < 1);
	const game::GameWorld::SquadOrder* leaderOrder = field.world->squadLeaderOrder(2, 1);
	CHECK(squad && leaderOrder && leaderOrder->type == game::GameWorld::OrderType::MapAttack
		&& std::fabs(squad->orderAt.x - leaderOrder->position.x) < 1 && std::fabs(squad->orderAt.z - leaderOrder->position.z) < 1);
	bool said = false;
	for (const auto& r : bots.radioSent(team[0])) {
		said = said || (r.type == 16 && r.channel == 3);
	}
	CHECK(said);
	physics::Vec3 slot;
	CHECK(!bots.formationGoal(team[0], slot)); // the leader's goal is the order's
	bots.updateSquadNow(2, 1);
	for (int i = 1; i < 3; ++i) {
		// A slot goal, about a spot the squad had at a re-goal (the slot
		// moves with the spot, the goal only when given again).
		CHECK(bots.formationGoal(team[static_cast<size_t>(i)], slot));
		// The line abreast: 3 (r + r') right and left of slot 0.
		const ai::FormationSlot& s = squad->formation.slots[static_cast<size_t>(i)];
		CHECK(s.used && std::fabs(std::fabs(s.offset.x) - 6.8934751f) < 0.01f && s.offset.z == 0);
	}
	CHECK(squad->formation.slots[1].offset.x * squad->formation.slots[2].offset.x < 0);
	CHECK(bots.mayAdvance(team[1]));
	const physics::Vec3 was = squad->orderAt;
	CHECK(field.world->issueOrder(2, 1, game::GameWorld::OrderType::Attack, 0, 100, -1));
	bots.updateSquadNow(2, 1);
	CHECK(squad->state == 2 && std::fabs(squad->orderAt.x - was.x) < 0.01f);
}

// A member whose bot idles (+0x4d, set by the Idle statement 0x100d2830
// and cleared by 0x100d28a0) is given its slot again at every look
// (0x100c0890 -> 0x100bff90), so it keeps up with a leader on the move.
TEST(Bots_IdleMemberFollowsItsSlot)
{
	Field field;
	game::BotSettings settings;
	game::BotManager bots(*field.world, settings);
	std::vector<int> team;
	for (int i = 0; i < 2; ++i) {
		const int id = field.world->addPlayer("Eu", 2, true);
		field.world->joinSquad(id, 1);
		field.world->spawnSoldier(id);
		field.move(id, static_cast<float>(i) * 3, 0);
		team.push_back(id);
	}
	int idleLooks = 0;
	for (int look = 0; look < 60; ++look) {
		for (int tick = 0; tick < 17; ++tick) {
			bots.update(1 / 30.0f);
			field.world->tick(1 / 30.0f);
		}
		const bool idle = bots.behaviour(team[1]) == "Idle";
		bots.updateSquadNow(2, 1);
		const game::BotManager::Squad* squad = bots.squad(2, 1);
		physics::Vec3 slot;
		if (idle && squad && squad->formation.slots[0].member == team[0] && bots.formationGoal(team[1], slot)) {
			// The goal is the slot about the spot the squad has now (the
			// look sees the spot of the update before).
			const float aside = std::sqrt(horizontal2(slot - squad->at));
			if (look >= 6) {
				// Past the sixth look the offsets are laid out for the
				// members (3 x (r + r'), 6.89 m).
				CHECK(aside > 6.4f && aside < 7.6f);
				++idleLooks;
			}
		}
	}
	CHECK(idleLooks >= 3);
}

// The sensing agent's look-around (0x100d7840): a bot walking to PAC's
// point with nobody about looks along its walk most of the time and
// glances aside now and then (the sector weights growing 0.0125 off the
// walk), the walk going on by throttle and strafe against the look
// (0x100cd3b0).
TEST(Bots_LookAroundWhileWalking)
{
	Field field;
	game::BotSettings settings;
	game::BotManager bots(*field.world, settings);
	const int id = field.world->addPlayer("Eu", 2, true);
	field.world->joinSquad(id, 1);
	field.world->spawnSoldier(id);
	field.move(id, -60, 0);
	int aside = 0, ticks = 0;
	std::set<int> sectors;
	for (int tick = 0; tick < 30 * 40; ++tick) {
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
		game::GameWorld::PlayerInfo info;
		field.world->playerInfo(id, info);
		if (bots.lookPlanned(id)) {
			sectors.insert(bots.lookSector(id));
		}
		if (bots.behaviour(id) == "MoveTo" && info.velocity.x * info.velocity.x + info.velocity.z * info.velocity.z > 1) {
			++ticks;
			const float walk = std::atan2(info.velocity.x, info.velocity.z) * 57.29578f;
			aside += std::fabs(std::remainder(info.yaw - walk, 360.0f)) > 30;
		}
	}
	game::GameWorld::PlayerInfo info;
	field.world->playerInfo(id, info);
	CHECK(info.position.x > -40); // it made its way east
	CHECK(ticks > 30 * 10);
	CHECK(aside > 0 && aside < ticks / 2);
	CHECK(sectors.size() >= 2);
}

// The SAI's respawn choice (0x100f0590): a dead EU bot gets EU's point
// (the only one it holds with a spawn point) and one of the four kits.
// AIMain refuses bots over aiSettings.setMaxNBots (0x100581a0).
TEST(Bots_RespawnChoiceAndMaxBots)
{
	Field field;
	game::BotSettings settings;
	settings.maxBots = 1;
	game::BotManager bots(*field.world, settings);
	bots.setTeamKits(2, {{"k0", "us_light_soldier"}, {"k1", "us_light_soldier"}, {"k2", "us_light_soldier"},
							{"k3", "us_light_soldier"}});
	const int id = field.world->addPlayer("Eu", 2, true);
	field.world->joinSquad(id, 1);
	field.world->spawnSoldier(id);
	const int other = field.world->addPlayer("Eu2", 2, true);
	field.world->joinSquad(other, 1);
	field.world->spawnSoldier(other);
	bots.update(1 / 30.0f);
	CHECK(!bots.refused(id) && bots.refused(other));
	game::GameWorld::PlayerInfo info;
	field.world->playerInfo(id, info);
	field.world->setHitPoints(info.soldier, 0);
	for (int tick = 0; tick < 5; ++tick) {
		field.world->tick(1 / 30.0f);
		bots.update(1 / 30.0f);
	}
	field.world->playerInfo(id, info);
	CHECK(!info.alive);
	CHECK(bots.respawnPoint(id) == 2 && field.world->spawnChoice(id) == 2);
	CHECK(bots.respawnKit(id) >= 0 && bots.respawnKit(id) <= 3);
}

// SAI steps 10 / 11 (0x100a0420, 0x100a09c0): the shares by Aggression and
// the running split.
TEST(Bots_AggressionSplit)
{
	float a = 0, d = 0;
	ai::aggressionShares(0.7f, 0, 2, 1, a, d);
	CHECK(a == 1 && d == 0); // nothing held: all attack
	ai::aggressionShares(0.7f, 3, 0, 0, a, d);
	CHECK(a == 0 && d == 1); // all held: all defend
	ai::aggressionShares(0.7f, 1, 1, 1, a, d);
	CHECK(near(a, 0.7f) && near(d, 0.3f));
	CHECK(ai::assignsAttack(0.7f, 0.3f, 0, 0));
	CHECK(!ai::assignsAttack(0.4f, 0.6f, 0, 0));
	CHECK(ai::assignsAttack(0.7f, 0.3f, 2, 1));  // 2/3 <= 0.7
	CHECK(!ai::assignsAttack(0.7f, 0.3f, 3, 1)); // 3/4 > 0.7
	CHECK(ai::assignsAttack(1, 0, 5, 0));
	CHECK(!ai::assignsAttack(0, 1, 0, 3));
}

// The squad manager's strike spots (0x10088040, 0x100880e0, 0x100bfcb0,
// 0x10088f20): a strike EU's commander calls on its own squad sends the
// bots 45 m out from it at once (the first way tried, straight out, on no
// mesh here); PAC's strike there reaches them only after its 12.5..25 s;
// after 25 s the spot goes and they get their goals again.
TEST(Bots_SquadLeavesAStrikeSpot)
{
	Field field;
	game::BotSettings settings;
	game::BotManager bots(*field.world, settings);
	std::vector<int> team;
	for (int i = 0; i < 3; ++i) {
		const int id = field.world->addPlayer("Eu", 2, true);
		field.world->joinSquad(id, 1);
		field.world->spawnSoldier(id);
		field.move(id, -10.0f + static_cast<float>(i) * 3, 0);
		team.push_back(id);
	}
	for (int tick = 0; tick < 18 * 7; ++tick) {
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
	}
	// PAC's strike on them: not known to EU yet.
	bots.noteStrike(1, {-5, 0, 0});
	bots.updateSquadNow(2, 1);
	physics::Vec3 at;
	for (int id : team) {
		CHECK(!bots.formationGoal(id, at) || std::fabs(std::sqrt(horizontal2(at - physics::Vec3 {-5, 0, 0})) - 45) > 1);
	}
	// EU's own: every bot heads 45 m out, along the way from the spot to it.
	bots.noteStrike(2, {-5, 0, 0});
	bots.updateSquadNow(2, 1);
	for (int id : team) {
		game::GameWorld::PlayerInfo info;
		field.world->playerInfo(id, info);
		CHECK(bots.formationGoal(id, at));
		CHECK(std::fabs(std::sqrt(physics::lengthSquared(at - physics::Vec3 {-5, 0, 0})) - 45) < 0.1f);
		const physics::Vec3 out = info.position - physics::Vec3 {-5, 0, 0};
		CHECK(out.x * (at.x + 5) + out.z * at.z > 0);
	}
	// 25 s on the spot has gone: given their goals again.
	for (int tick = 0; tick < 30 * 26; ++tick) {
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
	}
	const game::BotManager::Squad* squad = bots.squad(2, 1);
	CHECK(squad != nullptr);
	for (const auto& m : squad->members) {
		CHECK(!m.escaping || horizontal2(m.at - physics::Vec3 {-5, 0, 0}) < 40 * 40);
	}
}

// A human member lagging (0x100f4f30): a human of the squad 40 m behind
// the leader, farther from the order than the squad's spot, lags for
// 10..20 s and the formation isn't together; a bot there never lags.
TEST(Bots_HumanMemberLagsBehind)
{
	Field field;
	game::BotSettings settings;
	game::BotManager bots(*field.world, settings);
	const int leader = field.world->addPlayer("Eu", 2, true);
	field.world->joinSquad(leader, 1);
	field.world->spawnSoldier(leader);
	field.move(leader, -60, 0);
	const int mate = field.world->addPlayer("Eu2", 2, true);
	field.world->joinSquad(mate, 1);
	field.world->spawnSoldier(mate);
	field.move(mate, -57, 0);
	for (int tick = 0; tick < 18 * 7; ++tick) {
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
	}
	const game::BotManager::Squad* squad = bots.squad(2, 1);
	CHECK(squad && squad->state == 2);
	// The human joins and stands 40 m behind (the order is PAC's point, east).
	CHECK(field.world->spawnPlayer(2) != nullptr);
	const int human = field.world->localPlayer();
	field.world->joinSquad(human, 1);
	field.move(leader, -60, 0);
	field.move(mate, -57, 0);
	field.move(human, -100, 0);
	bots.update(1 / 30.0f);
	bots.updateSquadNow(2, 1);
	bots.updateSquadNow(2, 1);
	CHECK(squad->members.size() == 3);
	bool humanLags = false, botLags = false;
	float until = 0;
	for (const ai::FormationSlot& s : squad->formation.slots) {
		if (s.used && s.member == human) {
			humanLags = s.lagging;
			until = s.lagUntil;
		}
		botLags = botLags || (s.used && s.member == mate && s.lagging);
	}
	const game::BotManager::SquadMember* m = nullptr;
	for (const auto& member : squad->members) {
		m = member.player == human ? &member : m;
	}
	std::printf("  the human's slot time %.0f, lagging %d until %.1f s from now\n", m ? m->eta : -1.0f, humanLags,
		until - bots.time());
	CHECK(humanLags && !botLags && !squad->formation.together);
	CHECK(until - bots.time() >= 9.9f && until - bots.time() <= 20.1f);
	// Up with the others: no longer lagging, together again.
	field.move(human, -63, 2);
	bots.update(1 / 30.0f);
	bots.updateSquadNow(2, 1);
	bots.updateSquadNow(2, 1);
	for (const ai::FormationSlot& s : squad->formation.slots) {
		CHECK(!(s.used && s.member == human && s.lagging));
	}
	CHECK(squad->formation.together);
}

// The bot manager's slots 16 and 24 (game events 7 and 0xb): a bot whose
// teammate it killed says "sorry" (9, channel 6) at once (0x100c60c0).
TEST(Bots_TeamKillSaysSorry)
{
	Field field;
	game::BotSettings settings;
	game::BotManager bots(*field.world, settings);
	const int killer = field.soldier(2, true, -60, 0);
	const int victim = field.soldier(2, true, -55, 0);
	for (int tick = 0; tick < 10; ++tick) {
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
	}
	game::GameWorld::PlayerInfo info;
	field.world->playerInfo(victim, info);
	field.world->applyDamage(info.soldier, 1000, killer);
	bool sorry = false;
	for (int tick = 0; tick < 10; ++tick) {
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
	}
	for (const auto& r : bots.radioSent(killer)) {
		sorry = sorry || (r.type == 9 && r.channel == 6);
	}
	field.world->playerInfo(killer, info);
	CHECK(info.teamKills == 1);
	CHECK(sorry);
	for (const auto& r : bots.radioSent(victim)) {
		CHECK(r.type != 9);
	}
}

// Game event 7 (the soldier's revive, 0x00587b90 event 0x19 ->
// 0x100c5f00): the revived bot says "thankyou" (8, channel 6) a second
// later.
TEST(Bots_RevivedBotSaysThankYou)
{
	Field field([](Field& f) {
		f.templates.registerType(6, "Kit");
		world::ObjectTemplate* defib = f.templates.create("GenericFireArm", "unl_defibrillator");
		defib->settings()["itemIndex"] = "5";
		defib->settings()["projectileTemplate"] = "shock";
		defib->settings()["velocity"] = "60";
		defib->settings()["ammo.magSize"] = "1";
		defib->settings()["ammo.reloadTime"] = "0.6";
		world::ObjectTemplate* shock = f.templates.create("GenericProjectile", "shock");
		shock->settings()["damage"] = "1000";
		shock->settings()["damageMandownSoldiers"] = "1";
		shock->settings()["collision.resurrectDamage"] = "20000";
		shock->settings()["collision.restoreHP"] = "120";
		shock->settings()["timeToLive"] = "CRD_NONE/0.05/0/0";
		world::ObjectTemplate* kit = f.templates.create("Kit", "EU_Assault");
		kit->children().push_back({"eu_ar_rifle", {}, {}});
		kit->children().push_back({"unl_defibrillator", {}, {}});
	});
	game::BotSettings settings;
	game::BotManager bots(*field.world, settings);
	CHECK(field.world->spawnPlayer(2) != nullptr);
	const int bot = field.world->addPlayer("Mate", 2, true);
	CHECK(field.world->spawnSoldier(bot));
	game::GameWorld::PlayerInfo info, me;
	field.world->playerInfo(field.world->localPlayer(), me);
	field.world->playerInfo(bot, info);
	field.world->setSoldierPosition(info.soldier, me.position + physics::Vec3 {0, 0, 1.0f});
	field.world->setSpawnTime(30);
	bots.update(1 / 30.0f);
	field.world->setHitPoints(info.soldier, 0);
	bots.update(1 / 30.0f);
	game::SoldierInput input;
	input.selectItem = 5;
	field.world->setPlayerInput(input);
	field.world->tick(1 / 30.0f);
	input = {};
	input.fire = true;
	input.aimPitch = -60;
	field.world->setPlayerInput(input);
	for (int i = 0; i < 5; ++i) {
		field.world->tick(1 / 30.0f);
		bots.update(1 / 30.0f);
	}
	field.world->playerInfo(bot, info);
	CHECK(info.alive && !info.manDown);
	field.world->setPlayerInput({});
	bool thanks = false;
	float at = -1;
	for (int i = 0; i < 45; ++i) {
		field.world->tick(1 / 30.0f);
		bots.update(1 / 30.0f);
		for (const auto& r : bots.radioSent(bot)) {
			if (r.type == 8 && r.channel == 6 && !thanks) {
				thanks = true;
				at = r.at;
			}
		}
	}
	CHECK(thanks);
	std::printf("  thankyou at %.2f s\n", at);
}

// AIMain's kick on a human's join (slot 7, 0x10056110): over the server's
// maximum with maxBotsIncludeHumans, the last bot of the team with more
// players goes.
TEST(Bots_HumanJoiningKicksABot)
{
	Field field;
	game::BotSettings settings;
	game::BotManager bots(*field.world, settings);
	bots.setServerMaxPlayers(3);
	std::vector<int> ids;
	for (int i = 0; i < 3; ++i) {
		ids.push_back(field.soldier(i == 1 ? 1 : 2, true, -60.0f + static_cast<float>(i) * 3, 0));
	}
	bots.update(1 / 30.0f);
	CHECK(bots.botCount() == 3);
	CHECK(field.world->spawnPlayer(1) != nullptr); // a human joins PAC (2 a side now)
	bots.update(1 / 30.0f);
	CHECK(bots.botCount() == 2);
	game::GameWorld::PlayerInfo info;
	CHECK(!field.world->playerInfo(ids[2], info)); // EU's last bot (a tie: team 2)
	CHECK(field.world->playerInfo(ids[0], info) && field.world->playerInfo(ids[1], info));
	// Without maxBotsIncludeHumans nobody goes.
	Field other;
	game::BotSettings plain;
	plain.maxBotsIncludeHumans = false;
	game::BotManager more(*other.world, plain);
	more.setServerMaxPlayers(2);
	other.soldier(2, true, -60, 0);
	other.soldier(2, true, -57, 0);
	more.update(1 / 30.0f);
	CHECK(other.world->spawnPlayer(1) != nullptr);
	more.update(1 / 30.0f);
	CHECK(more.botCount() == 2);
}

// The Turret sensing pattern (0x1008c120): a tank's driver with nothing
// to fight looks about with the turret, by the look-around's sectors; with
// the Null pattern the turret stays where it is.
TEST(Bots_TurretLooksAround)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	for (const bool turretPattern : {true, false}) {
		CarField field(gameDir);
		world::ObjectTemplate* tank = field.templates.create("PlayerControlObject", "tank");
		for (const auto& [key, value] : field.templates.find("car")->settings()) {
			tank->settings()[key] = value;
		}
		tank->settings()["seatInformation"] = "tank 0/0.5/0.4 0/0/0";
		tank->settings()["aiTemplate"] = "TestTank";
		tank->children().push_back({"car_entry", {-1.5f, 0, 0}, {}});
		tank->children().push_back({"car_engine", {0, -0.44f, 0}, {}});
		tank->children().push_back({"tank_turret", {0, 0.9f, 0}, {}});
		world::ObjectTemplate* turret = field.templates.create("RotationalBundle", "tank_turret");
		turret->settings()["inputToYaw"] = "PIMouseLookX";
		turret->children().push_back({"tank_barrel", {0, 0.2f, 0.5f}, {}});
		world::ObjectTemplate* barrel = field.templates.create("RotationalBundle", "tank_barrel");
		barrel->settings()["inputToPitch"] = "PIMouseLookY";
		barrel->children().push_back({"tank_gun", {0, 0, 1}, {}});
		world::ObjectTemplate* gun = field.templates.create("GenericFireArm", "tank_gun");
		gun->settings()["velocity"] = "300";
		std::string out;
		for (const char* line : {"aiTemplatePlugIn.create Unit TestTank_Unit", "aiTemplatePlugIn.equipmentTypeName Tank",
				 "aiTemplatePlugIn.create ControlInfo TestTank_Ctrl", "aiTemplate.create TestTank",
				 "aiTemplate.addType ITGround", "aiTemplate.addPlugIn TestTank_Unit", "aiTemplate.addPlugIn TestTank_Ctrl",
				 "aiSettings.createBehaviourModifiers UnitWeights", "aiSettings.setBehaviourModifier Idle 1.0",
				 "aiSettings.setVehicleBehaviour Tank Idle BBIdle BBPIdle2d 5 UCUnion UnitWeights",
				 "aiSettings.setVehicleDefaultBehaviour Tank Idle"}) {
			field.console.execute(line, out);
		}
		field.console.execute(turretPattern ? "aiSettings.addSensingAgentPattern Tank Turret"
											: "aiSettings.addSensingAgentPattern Tank Null",
			out);
		const uint32_t id = field.objects.create(tank, {3, 1.2f, 0}, {0, 0, 0})->id();
		const int bot = field.soldier("Bot", true, 0, 0);
		field.ticks(90);
		field.board(bot, 0);
		CHECK(field.info(bot).vehicle == id);
		game::BotManager bots(*field.world, field.settings);
		float most = 0;
		bool planned = false;
		for (int tick = 0; tick < 30 * 20; ++tick) {
			field.ticks(1, &bots);
			float yaw = 0, pitch = 0;
			if (field.world->gunAim(id, 0, yaw, pitch)) {
				most = std::max(most, std::fabs(std::remainder(yaw, 360.0f)));
			}
			planned = planned || bots.lookPlanned(bot);
		}
		std::printf("  %s pattern: the turret turned up to %.1f degrees off the hull, looked %d\n",
			turretPattern ? "Turret" : "Null", most, planned);
		CHECK(planned == turretPattern);
		CHECK(turretPattern ? most > 20 : most < 1);
	}
}

// The leader's slot given up (0x100f64a0): the member nearest the order
// takes slot 0, and the last used slot moves into the hole it left, so the
// used slots stay packed from slot 0 (the earlier rewrite left the hole,
// and a respawned member took slot 0 back at its spawn, pulling the squad
// there).
TEST(Bots_FormationStaysPackedWhenTheLeaderFalls)
{
	Field field;
	game::BotSettings settings;
	game::BotManager bots(*field.world, settings);
	std::vector<int> team;
	for (int i = 0; i < 6; ++i) {
		const int id = field.world->addPlayer("Eu", 2, true);
		field.world->joinSquad(id, 1);
		field.world->spawnSoldier(id);
		field.move(id, -60.0f - static_cast<float>(i) * 3, static_cast<float>(i));
		team.push_back(id);
	}
	for (int tick = 0; tick < 18 * 7; ++tick) {
		bots.update(1 / 30.0f);
		field.world->tick(1 / 30.0f);
	}
	const game::BotManager::Squad* squad = bots.squad(2, 1);
	CHECK(squad != nullptr);
	// The member of slot 2 goes nearest the order (east) as the leader
	// falls, so it leaves a middle slot for slot 0.
	const int middle = squad->formation.slots[2].member;
	const int first = squad->formation.slots[0].member;
	CHECK(squad->formation.slots[4].used);
	field.move(middle, -30, 0);
	bots.updateSquadNow(2, 1);
	game::GameWorld::PlayerInfo info;
	CHECK(field.world->playerInfo(first, info));
	field.world->setHitPoints(info.soldier, 0);
	field.world->tick(1 / 30.0f);
	bots.updateSquadNow(2, 1);
	int used = 0;
	bool packed = true;
	for (size_t i = 0; i < squad->formation.slots.size(); ++i) {
		const bool u = squad->formation.slots[i].used;
		packed = packed && (u ? static_cast<int>(i) == used : true);
		used += u ? 1 : 0;
		CHECK(!(u && squad->formation.slots[i].member == first));
	}
	CHECK(used == 5 && packed);
}

// AIDLL's random numbers (AIRandom, DAT_101394cc): the Park-Miller step
// 0x10030810 is x 630360016 mod 2^31 - 1; the seed 0x100309a0 fills the
// 624 words with its run; the twister 0x100308f0 / 0x10046770 tempers with
// DICE's masks (0x56b64a << 7, 0xffffb716 << 15), so its words aren't
// MT19937's (whose first word for this state would differ).
TEST(Bots_AiRandomIsDicesTwister)
{
	uint32_t x = 12345;
	for (int i = 0; i < 1000; ++i) {
		const uint32_t want = static_cast<uint32_t>((static_cast<uint64_t>(x) * 630360016u) % 0x7fffffffu);
		CHECK(ai::parkMillerStep(x) == want);
		x = want;
	}
	CHECK(ai::parkMillerStep(0x7ffffffe) == static_cast<uint32_t>((0x7ffffffeull * 630360016u) % 0x7fffffffu));
	ai::Random random = ai::Random::seeded(1);
	CHECK(random.seedWord() == 1);
	CHECK(random.next() == 0x77f5ba93u);
	CHECK(random.next() == 0x171e5b46u);
	CHECK(random.next() == 0xc38f3094u);
	// aiRandom.seed writes the word only: the draws go on as before.
	ai::Random again = ai::Random::seeded(1);
	again.next();
	again.next();
	again.setSeedWord(99);
	CHECK(again.next() == 0xc38f3094u && again.seedWord() == 99);
	// U[0, 1) stays below 1 as a float.
	float top = 0;
	for (int i = 0; i < 100000; ++i) {
		top = std::max(top, random.uniform());
	}
	CHECK(top < 1.0f && top > 0.999f);
	CHECK(ai::wordToFloat(0xffffffffu) == 4294967296.0f && ai::wordToFloat(0x80000000u) == 2147483648.0f);
}

// The bots' unlock pools (BF2142.exe's AI player adaptor slot 40,
// 0x008a5b90, sorting by slot 39, 0x008a57b0): a container whose first
// item has no aiTemplate is left out; one whose item's itemIndex is the
// kit's primary slot (Kit +0x2c8, 3) goes to the primary pool, of which one
// is taken at floor((n + 0.999) U) when that is below n (about half the
// time with one); the rest to the main pool, two of which are taken.
TEST(Bots_UnlocksTakeAPrimaryFromItsOwnPool)
{
	Field field([](Field& f) {
		f.templates.registerType(20, "Kit");
		f.templates.registerType(21, "ItemContainer");
		world::ObjectTemplate* kit = f.templates.create("Kit", "EU_Assault");
		kit->settings()["unlockIndex"] = "1";
		const auto item = [&](const char* name, int index, bool ai) {
			world::ObjectTemplate* t = f.templates.create("GenericFireArm", name);
			t->settings()["itemIndex"] = std::to_string(index);
			if (ai) {
				t->settings()["aiTemplate"] = name;
			}
		};
		item("unl_shotgun", 5, true);
		item("unl_smoke", 5, true);
		item("unl_har_rifle", 3, true);
		item("ability_sprint", 0, false);
		const auto add = [&](const char* name, const char* branch, int level, const char* content) {
			world::ObjectTemplate* c = f.templates.create("ItemContainer", name);
			c->settings()["unlockBranch"] = branch;
			c->settings()["unlockLevel"] = std::to_string(level);
			c->children().push_back({content, {}, {}});
			kit->children().push_back({name, {}, {}});
		};
		add("Light0", "UB_First", 0, "unl_shotgun");
		add("Light1", "UB_First", 1, "ability_sprint");
		add("Light2", "UB_First", 2, "unl_har_rifle");
		add("Medic0", "UB_Second", 0, "unl_smoke");
	});
	// A human owning Light0 and Light1 (mask 3): UB_First level 4, so Light0
	// to Light2 are offered; UB_Second level 1 (Medic0).
	const int human = field.world->addPlayer("Human", 1, false);
	field.world->setOwnedUnlocks(human, {"Light0", "Light1"});
	CHECK(field.world->spawnSoldier(human));
	const int bot = field.world->addPlayer("Bot", 2, true);
	field.world->setKit(bot, "EU_Assault", "us_light_soldier");
	int primaries = 0;
	const int runs = 400;
	for (int run = 0; run < runs; ++run) {
		field.world->chooseBotUnlocks(bot);
		const std::vector<std::string> chosen = field.world->unlocksOf(bot);
		const auto has = [&](const char* name) { return std::find(chosen.begin(), chosen.end(), name) != chosen.end(); };
		CHECK(has("Light0") && has("Medic0")); // the main pool's two
		CHECK(!has("Light1"));                 // no aiTemplate: left out
		primaries += has("Light2") ? 1 : 0;
		CHECK(chosen.size() == (has("Light2") ? 3u : 2u));
	}
	std::fprintf(stderr, "  the primary taken %d of %d\n", primaries, runs);
	CHECK(primaries > runs * 3 / 10 && primaries < runs * 7 / 10);
}
