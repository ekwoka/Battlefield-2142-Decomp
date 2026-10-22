// Bots (AIDLL.dll's BotManager, src BF2/AI/AIDLL/AI/Main/BotManager.cpp):
// each AI player runs the behaviours of its equipment type
// (AI/AIBehaviours.ai: Infantery on foot, the vehicle's type at the wheel
// or a gun), chosen by urgency each think (src/AI/Behaviour: the weights
// from the bot's personality "StandardWeights", the running behaviour's
// urgency curve and inhibitors, the arbiter's 0.87/1.15 hysteresis). The
// urgencies are the BB classes' (BB vtable slot 9):
//   MoveTo  (BBMoveTo 0x10066a70): the goal's urgency (0.1..1, x2 for an
//           area the team does not hold, 0 once there) times the weight;
//   Idle    (BBIdle 0x10069f50): the weight (0.5 for a passenger); a
//           driver waits (30, or 70 when the comer faces the vehicle) up
//           to 10 s for a teammate on the way to a free seat;
//   Fire    (BBFire 0x10075fb0): 50 while some known enemy scores above 0
//           (the score: weapon strength against the target's type x
//           (1 - d / (1.2 maxRange)) x 5 x how fresh the sighting is);
//   Revive  (BBRevive 0x10082760): 100 while a downed teammate within the
//           defibrillator's range can be reached before it bleeds out;
//   Flee    (BBFlee 0x100859d0): 100 below 30 % health with the
//           camouflage over 70 % charged, 10 s after spawning (its plan
//           0x10085fc0: a locked run 50 m away with the camouflage on);
//   Change  (BBChange 0x1006c520): best seat / current seat x weight x 10
//           (seat values 0x100d3e70), less with distance.
//   MedicAssist (0x10070200), AmmoAssist (0x10071a10), RepairVehicle
//           (0x10073360): the best teammate (human players only for ammo)
//           or vehicle in reach by need over distance, x the squad
//           coordination factor (0x10089aa0);
//   c4      (0x10083750): 100 to place a charge in a point the team holds,
//           100 to set it off once that point is contested;
//   DefuseMines (0x10086870), RunOver (0x10087820), Avoid (0x10064850,
//           on foot only); BridgeRepair is 0 (no broken bridges here).
//
// Scheduling (0x100ca590): about half the bots run their plan a frame, at
// most four think (after 0.3 s / 0.5 s of plan time) and two sense.
// Moving goes through the path follower and the drivers of
// src/AI/PathFollower; orders run through the level's strategic waypoints
// (0x10060300). A level without a navigation mesh has no bots.
//
// Sensing (AIDLL 0x100ed700, 0x100ee430): enemies within
// viewDistance x cos(fov/2) inside a 100 degree frustum with a clear ray;
// 360 degree awareness for 5 s after a round passes within 10 m
// (0x1004ed30); "lost" after 2 s out of sight, forgotten after 60 s.
// Skill does not change sensing. Soldiers never hear (the original's
// hearing roll can't succeed for a listener that has a Mobile part).
//
// Aiming (MouseTurretAimAt 0x1008e940, controller 0x100e5820): the
// ControlInfo plugin's PID gains steer the look towards the lead point;
// the output is PIMouseLookX/Y, 5 degrees a 1/30 s tick per unit
// (BF2142.exe 0x0058a6d0); the trigger (held for a fire mode 2 weapon,
// tapped otherwise, 0x100777a0) goes while the aim is inside the weapon's
// allowedDeviation (0x100d9090) after a reaction time of
// (1 - skill)(1.5 + U[0,1]) s (0x100777a0); the rounds stray by the AI
// deviation (1 - acc)(1 - t/2) x deviation (BF2142.exe 0x00892b70).
// Weapons are chosen per target by weaponTemplate strength, the fighting
// distance kept within [minRange, 1.1 x preferred] (0x100795a0).
//
// Strategy (src/AI/Strategy): each side's SAI picks a strategy and gives
// squads attack and defend orders on strategic areas; bots go to a spot
// inside the ordered area (0x10060300). A team with bots and without a
// commander has AIDLL's AI commander (not a player): it shells its own
// points being taken with the artillery (0x1009c050; BotManager::command).
//
// The settings come from the game's own scripts (aiSettings.*, the
// weaponTemplate.* lines of Objects/**/ai/Weapons.ai, aiStrategy.* and
// aiStrategicArea.*, aiTemplate.*); without them the shipped values are
// assumed.
#pragma once

#include "AI/Behaviour.h"
#include "AI/NavMesh.h"
#include "AI/PathFollower.h"
#include "AI/Strategy.h"
#include "GameWorld.h"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
}

namespace dice::hfe::game {

// What a weaponTemplate tells the AI (BF2142.exe's AI weapon template,
// constructor 0x0088a7b0 for the defaults).
struct WeaponAi {
	enum Type { FireArm, Healing, Ammo, Revive, Repair, C4, Camouflage, CloseCombat, Sniper, FireAndForget, TitanWeapon };
	int type = FireArm;
	float minRange = 0, maxRange = 0;
	float optimal = 0.35f;   // optimalRangePercentage / 100
	int firingPose = 1;      // NotAvailable 0, Standing 1, Crouching 2, Lying 3
	// By CombatStrengthType: Infantry, LightArmour, HeavyArmour,
	// NavalArmour, Helicopter, AirPlane, Titan, Missiles.
	std::array<float, 8> strength {};
	float deviation = 14;
	float allowedDeviation = 0.5f;
	float explosionRadius = 0;
	bool indirect = false, thrown = false;
};

// An AI template plug-in (aiTemplatePlugIn.create <kind> <name>; BF2142.exe
// 0x0088317b and the properties after it): what the bots read of the
// objects they are and drive.
struct AiPlugin {
	std::string kind; // Unit, Physical, Mobile, ControlInfo, Armament
	// Unit: the equipment type whose behaviours a bot in it runs.
	std::string equipmentType;
	// Physical: its CombatStrengthType (setStrType), -1 for none.
	int strengthType = -1;
	// Mobile: the path follower's settings.
	std::string pathfindingMap; // Infantry, Vehicle
	float maxSpeed = 0;
	float spaceNeededTweak = 1, speedAtZeroSpace = 0, backingSpeed = 0;
	float brakeDistance = 0, maxBrakePercentage = 0;
	float lookAheadTime = 0, minSpeedForLookAhead = 0;
	float minAngleDiffForSlowDown = 0, angleSlowDownScale = 0, maxAngleDiffSlowDown = 0;
	bool floating = false;
	// Unit: setHasExposedSoldier (+0x16; the seat value's armour bonus).
	bool exposedSoldier = false;
	// ControlInfo.
	float proportional = 0, derivative = 0, angleSpeed = 0, yawScale = 1;
	bool hasGains = false;
};

// aiTemplate.create <name> with its plug-ins (aiTemplate.addPlugIn).
struct AiTemplate {
	std::vector<std::string> plugins;
	float basicTemp = 0;
	bool secondary = false;
};

struct BotSettings {
	float skill = 0.4f;   // aiSettings.setBotSkill (0..1)
	int maxBots = 16;     // aiSettings.setMaxNBots
	std::vector<std::string> behaviours; // by id (aiSettings.setBehaviour)
	// Named modifier sets (createBehaviourModifiers + setBehaviourModifier).
	std::map<std::string, std::map<std::string, float>> modifiers;
	std::string botModifiers = "StandardWeights"; // what ai.addBot gives bots
	std::string editing; // the set setBehaviourModifier fills
	// The equipment types' behaviours (aiSettings.setVehicle*,
	// setVehicleBehaviour, ...) and the urgency curves (createUC*).
	std::map<std::string, ai::VehicleSetup> vehicles;
	std::map<std::string, ai::UrgencyCurve> curves;
	// aiSettings.setViewDistance (AIDLL's settings object +0x1c, -1 at
	// first). One not set by the scripts takes the game's when the AI
	// starts on a level (AIDLL 0x10056b20: IAIGame vt+0x58,
	// BF2142.exe 0x008959f0, see gameViewDistance) from the level's
	// Renderer.fogStartEndAndBase (the caller hands the level's over; the
	// default is scene::LightSettings') and GameLogic.maximumLevelViewDistance.
	float viewDistance = -1;
	std::array<float, 4> levelFog {10, 100, 1, 0};
	float levelViewDistance = 300;
	float fieldOfView = 100; // degrees (AIDLL table 0x1012b074, mode 0)
	float discoverCloakedDistance = 3; // aiSettings.setDiscoverCloakedEnemiesDistance
	// weaponTemplate.* by lower-case name.
	std::map<std::string, WeaponAi> weapons;
	std::string editingWeapon;
	// aiStrategy.*, aiStrategicArea.*, ai.addSAIStrategy.
	ai::StrategySettings strategy;
	// aiTemplate.* and aiTemplatePlugIn.* (Objects/**/ai/Objects.ai), by
	// lower-case name.
	std::map<std::string, AiTemplate> aiTemplates;
	std::map<std::string, AiPlugin> aiPlugins;
	std::string editingTemplate, editingPlugin;
	// The plug-in of a kind an AI template holds (null for none).
	const AiPlugin* plugin(const std::string& aiTemplate, const std::string& kind) const;

	float modifier(const std::string& behaviour) const;
	// The behaviours of an equipment type: the scripts' or, without them,
	// AIBehaviours.ai's Infantery set.
	const ai::VehicleSetup& vehicle(const std::string& type) const;
	// A weapon's AI template, or what a firearm of that name would have.
	WeaponAi weapon(const std::string& name) const;
};

class BotManager {
public:
	BotManager(GameWorld& world, BotSettings settings);
	// With $BF2142_BOT_STATS set, prints how long the bots ran each
	// behaviour and how far they drove each vehicle.
	~BotManager();
	BotManager(const BotManager&) = delete;
	BotManager& operator=(const BotManager&) = delete;
	// Seconds the bots ran each behaviour (" (driving)", " (seated)" in vehicles).
	const std::map<std::string, float>& behaviourSeconds() const { return m_behaviourSeconds; }
	const BotSettings& settings() const { return m_settings; }

	// The level's infantry navigation mesh. A level without one
	// (AIPathFinding) has no bots in the original: its level AI/AI.ai with
	// "ai.init" doesn't exist, so AIMain never initiates and ai.addBot is
	// refused (0x10056b20, 0x100c84b0; BF2142.exe 0x004d3450). Setting a
	// null mesh does the same: the manager runs no bots (enabled() false).
	void setNavMesh(const ai::NavMesh* mesh)
	{
		m_navMesh = mesh;
		m_noNavMesh = mesh == nullptr;
	}
	bool enabled() const { return !m_noNavMesh; }
	// The vehicles' (AIPathFinding/Vehicle.qtr), for driving bots.
	void setVehicleNavMesh(const ai::NavMesh* mesh) { m_vehicleNavMesh = mesh; }
	// Thinks for every AI player and sets its input; call before the world ticks.
	void update(float seconds);

	// What a bot does now (for tests and debugging): the running
	// behaviour's name (MoveTo, Idle, Fire, Change, Revive, Flee, ...).
	std::string behaviour(int player) const;
	// A bot's value of a seat (0x100d3e70; `vehicle` 0 for its soldier), as
	// BBChange weighs it; 0 for no such bot.
	float seatValueFor(int player, uint32_t vehicle, int seat) const;
	int target(int player) const; // the enemy player it fights, -1 for none
	// The equipment type whose behaviours it runs (Infantery, Tank, ...).
	std::string equipment(int player) const;
	// Whether it knows of an enemy (seen, or still remembered).
	bool knows(int player, int enemy) const;
	// The points MoveTo passes on the way to its goal (strategic waypoints
	// and areas), then the goal.
	std::vector<physics::Vec3> route(int player) const;
	// The strategy a team's SAI runs ("" before the first update).
	std::string strategy(int team) const;

private:
	// An enemy a bot knows of (the sensing agent's memory item).
	struct Known {
		int player = -1;
		physics::Vec3 position; // where it was last seen
		float seen = -1e9f;     // when it was last seen
		float lostAt = -1;      // when it went out of sight, -1 while in sight
		float alertAt = -1e9f;  // a round passed by (360 degree awareness for 5 s)
		bool everSeen = false;
	};
	struct Bot {
		ai::Arbitration arbitration;
		std::string equipment;
		std::string behaviour = "Idle";
		float lastThink = -1;
		float spawnedAt = 0;
		std::vector<Known> known;
		// Fire.
		int target = -1;
		int weaponSlot = 0;
		float targetAcquired = 0;
		float triggerAt = 0;      // when the trigger may first be pulled
		float preferredRange = 0; // the fighting distance kept
		physics::Vec3 lastAimPoint;
		float yawError = 0, pitchError = 0;
		std::array<float, 10> pitchErrors {};
		int pitchErrorCount = 0;
		float pitch = 0;          // the look's pitch, degrees
		float devYaw = 0, devPitch = 0; // the aim deviation applied (degrees)
		bool tapped = false;      // Trigger: pressed last tick
		bool heatStop = false;
		bool reportedFiring = false; // the fire report's last state    // the heat condition (0x100d9040) in its cooling state
		float poseRequested = -10;
		SoldierPose pose = SoldierPose::Stand;
		// Goal (MoveTo).
		int goalOrder = -1;       // the area the goal lies in, -1 for none
		physics::Vec3 goal;
		bool hasGoal = false;
		float goalAt = -1e9f;
		bool goalReached = false;
		physics::Vec3 orderPosition;
		// Change.
		uint32_t vehicleTarget = 0;
		uint32_t lastVehicle = 0;
		float leftVehicleAt = -1e9f;
		// Flee (BBPFlee's plan 0x10085fc0).
		physics::Vec3 fleeDirection;
		bool planLocked = false;     // bot+0x7c: the plan runs to its end, no arbitration
		bool fleePlanned = false;
		bool fleeRun = false;        // a point was found: running to it
		physics::Vec3 fleePoint;
		float fleeCamoFrom = -1;     // the camouflage's 4 s, from when it is in hand
		// Movement: the path follower on foot and at the wheel.
		ai::PathFollower follower;
		int followId = -1;           // what it follows (a goal id)
		physics::Vec3 followTo;
		float followAt = -100;
		float unstickUntil = 0;      // the jump's stand-in (see steer)
		float unstickSide = 1;
		ai::DriveState drive;
		float lastYaw = 0;           // the vehicle's, for its turn rate
		float lastYawAt = -1;
		physics::Vec3 lastVelocity;  // the hover tank's, for its acceleration (FloatingTankMoveTo +0x3c)
		// Strategic waypoints and areas on the way to the goal (0x10060300).
		struct Via {
			physics::Vec3 position;
			int area = -1; // -1: an aiStrategicArea.addWayPoint point
		};
		std::vector<Via> via;
		int goalMap = 0;             // Infantry 0, Vehicle 1
		int lastArea = -1;           // the area it last headed for
		// Scheduling (BotManager entries, 0x100c91c0).
		std::string pattern;         // the running behaviour's BBP
		float lastAct = -1;
		float sinceThink = 0;        // entry +0x18
		int thinkAge = 0, senseAge = 0; // entry +8, +0x10
		int skips = 0;               // bot +0x1b4
		SoldierInput lastInput;
		// MedicAssist, AmmoAssist, RepairVehicle: whom (a player, or a
		// vehicle's object for RepairVehicle).
		int assistTarget = -1;
		uint32_t repairTarget = 0;
		// c4 (the bot's charge memory, bot+0x1b8): placed where, at which
		// area, when (it lapses after 240 s); and the plan's step.
		bool c4Placed = false;
		physics::Vec3 c4At;
		int c4Area = -1;
		float c4PlacedAt = 0;
		int c4Mode = 0;           // 0 place, 1 set off
		float c4InHandAt = -1;
		// DefuseMines.
		physics::Vec3 mineAt, mineStand;
		// RunOver.
		int runOverTarget = -1;
		// Avoid.
		physics::Vec3 avoidFrom, avoidVelocity;
		bool avoidStatic = false;
		float avoidFor = 0;
		physics::Vec3 avoidGoal;
		float avoidUntil = -1;
		float waitingSince = -1;  // Idle: a driver waiting for a passenger
		bool usedLast = false;    // presses: use and the defibrillator want a release between
		bool firedLast = false;
	};
	// The side's strategic AI.
	struct Side {
		std::string strategy;
		std::vector<float> weights;
		float startedAt = 0;
		float nextUpdate = 0;
		std::map<int, int> squadArea; // squad -> area index it attacks/defends
		std::vector<int> attacks, defences;
	};

	void sense(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players);
	void noticeRounds(Bot& bot, const GameWorld::PlayerInfo& self);
	void think(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players);
	void act(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds);
	// Urgencies (BB slot 9) of the behaviours in the set.
	float urgency(Bot& bot, const ai::BehaviourSetup& behaviour, float weight, const GameWorld::PlayerInfo& self,
		const std::vector<GameWorld::PlayerInfo>& players);
	float fireUrgency(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players);
	float moveUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight);
	float reviveUrgency(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players);
	float changeUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight);
	// A seat's value (0x100d3e70): `vehicle` 0 for the bot's own soldier.
	float seatValue(const Bot& bot, const GameWorld::PlayerInfo& self, uint32_t vehicle, int seat, bool other) const;
	float idleUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight, const std::vector<GameWorld::PlayerInfo>& players);
	float fleeUrgency(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players);
	float medicUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight, bool ammo);
	float repairUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight);
	float c4Urgency(Bot& bot, const GameWorld::PlayerInfo& self);
	float defuseUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight);
	float runOverUrgency(Bot& bot, const GameWorld::PlayerInfo& self);
	float avoidUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight);
	// The squad coordination factor (0x10089aa0): whether this bot should
	// take the task (Medic 1, Repair 2, Ammo 4) on a target.
	float coordination(const GameWorld::PlayerInfo& self, int type, int64_t target, int targetPlayer,
		const physics::Vec3& at) const;
	// Plans.
	void assist(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds, bool ammo);
	void repair(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds);
	void placeC4(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input);
	void defuse(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds);
	void runOver(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input);
	void avoid(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds);
	// The kit slot holding a weapon of a type (0 for none).
	int kitSlot(const GameWorld::PlayerInfo& self, int type) const;
	float basicTemp(const std::string& objectTemplate, float fallback) const;
	// Plans (what the chosen behaviour does).
	void fire(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds);
	void aim(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& point, const physics::Vec3& pointVelocity,
		bool hasTarget, float projectileSpeed, SoldierInput& input, float seconds);
	// Walks (InfanteryMoveTo) or drives (CarMoveTo/TankMoveTo) along a path
	// to `to`; false once it has arrived. `radius` is the goal's.
	bool steer(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& to, int goalId, float seconds,
		SoldierInput& input, float radius = 0);
	bool drive(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& to, int goalId, float seconds,
		SoldierInput& input, float radius = 0);
	// The point MoveTo heads for now (the next strategic waypoint or area
	// on the way, else the goal), popping those reached (0x1008aaa0).
	physics::Vec3 moveTarget(Bot& bot, const GameWorld::PlayerInfo& self);
	bool follow(Bot& bot, const ai::NavMesh& mesh, const physics::Vec3& from, const physics::Vec3& to, int goalId,
		const ai::FollowerSettings& settings);
	ai::DriveSettings driveSettings(const GameWorld::PlayerInfo& self) const;
	std::vector<int> areaRoute(int from, int to, int team) const;
	void routeVia(Bot& bot, const GameWorld::PlayerInfo& self, int target, int map);
	bool areaPosition(int area, physics::Vec3& at, float& radius, int& team) const;
	// The goal a bot heads for (its squad's order, else the SAI's areas).
	void updateGoal(Bot& bot, const GameWorld::PlayerInfo& self);
	void updateStrategy(float seconds);
	// A bot commanding its team (AIDLL 0x1009c050; see Bots.cpp).
	void command(float seconds);

	// Helpers.
	float healthOf(uint32_t object) const;
	std::vector<std::pair<int, std::string>> carried(const GameWorld::PlayerInfo& self) const;
	std::string weaponName(const GameWorld::PlayerInfo& self, int slot) const;
	float projectileSpeed(const std::string& weapon) const;
	std::string equipmentOf(const GameWorld::PlayerInfo& self) const;
	std::string aiTemplateOf(const std::string& objectTemplate) const;
	WeaponAi weaponAi(const std::string& name) const;
	int strengthTypeOf(const GameWorld::PlayerInfo& player) const;
	int areaOfControlPoint(int controlPoint) const;
	void ensureAreas();

	const ai::NavMesh* m_navMesh = nullptr;
	const ai::NavMesh* m_vehicleNavMesh = nullptr;
	bool m_noNavMesh = false;
	bool m_oddFrame = false;
	size_t m_cursor = 0; // plan execution's round robin (+0xfc)
	float m_commandIn = 0; // seconds to the commander's next look
	float m_firstStrikeAt = 120;
	float m_lastStrike[3] = {-1e9f, -1e9f, -1e9f};
	bool m_humanCommander[3] = {}; // the team had a commander at the last look (AIDLL's +0x241)
	GameWorld& m_world;
	BotSettings m_settings;
	std::vector<GameWorld::PlayerInfo> m_players;   // as of this update
	std::vector<GameWorld::VehicleInfo> m_vehicles; // as of this update
	std::map<int, Bot> m_bots;
	Side m_sides[3];
	// Squad tasks (squad member +task, 0x100bf470): bot -> (type, target).
	std::map<int, std::pair<int, int64_t>> m_tasks;
	std::map<std::string, float> m_behaviourSeconds;
	std::map<uint32_t, float> m_driven;
	float m_time = 0;
	uint32_t m_random = 0x2545f491;
	float random(); // 0..1
	void flee(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds);
	void idle(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input);
};

// aiSettings.* (skill, bots, behaviours and modifiers, the equipment types'
// behaviour sets), weaponTemplate.*, aiStrategy.*, aiStrategicArea.* and
// ai.addSAIStrategy into `settings`.
void registerBotConsoleMethods(Console& console, BotSettings& settings);

// The game's view distance for the bots (IAIGame vt+0x58, BF2142.exe
// 0x008959f0): the distance into the fog where its density reaches 0.8
// (t from 0 in steps of 0.01 while density(t) < 0.8, density(t) = t x base
// below fog.w, else t^3 + fog.w; start + (end - start) t), at most the
// level's view distance and 300 m.
float gameViewDistance(const std::array<float, 4>& fogStartEndAndBase, float levelViewDistance);

} // namespace dice::hfe::game
