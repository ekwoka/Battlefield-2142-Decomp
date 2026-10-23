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
//           on foot only: wrecks, and 1000 for a grenade warning,
//           0x1004eab0); BridgeRepair is 0 (no broken bridges here).
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
	float explosionRadius = 0;  // +0x20
	bool indirect = false, thrown = false; // thrown: +2 (0x0088a650)
	bool explodesOnImpact = false; // +0x4e
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
	// Unit: setSelectKey PIPositionSelectN (read through 0x100aa5f0): the
	// position key that moves a bot to this seat, N; 0 for none.
	int selectKey = 0;
	// ControlInfo.
	float proportional = 0, derivative = 0, angleSpeed = 0, yawScale = 1;
	bool hasGains = false;
};

// aiTemplate.create <name> with its plug-ins (aiTemplate.addPlugIn).
struct AiTemplate {
	std::vector<std::string> plugins;
	float basicTemp = 0;
	bool secondary = false;
	uint32_t types = 0; // aiTemplate.addType: 1 << ai::InformationType
};

// ai::InformationType (BF2142.exe 0x0088fbd0): the bits of an AI object's
// flags (+8) its template's aiTemplate.addType lines set. ITSoldier is also
// set for a soldier (0x0088d070: an object with interface 0xc4d7).
enum InformationType : uint32_t {
	kITUnit = 1u << 0,
	kITAir = 1u << 1,
	kITHelicopter = 1u << 2,
	kITGround = 1u << 3,
	kITNaval = 1u << 4,
	kITControlpoint = 1u << 5,
	kITTransportation = 1u << 6,
	kITMobile = 1u << 7,
	kITStructure = 1u << 8,
	kITSoldier = 1u << 9,
	kITObstructedView = 1u << 10,
	kITNoTemperature = 1u << 11,
	kITHasWreck = 1u << 12,
	kITObstacle = 1u << 13,
	kITHasCarHorn = 1u << 14,
};
// The factor on a vehicle's seats other than the root's in BBChange
// (0x1006b910, for a vehicle the bot isn't in): 1 when its driver's seat
// is taken; else, for a root with a Mobile part that isn't ITNaval, 0.77
// for ITGround and 0.5 for ITAir; 1 otherwise.
float otherSeatFactor(bool driverTaken, bool rootMobile, uint32_t rootTypes);
// The distances of 0x100d3730 (squared): whether the bot's order is close
// for a vehicle (see BotManager::orderClose), and `noMove` (its param_5).
// `own`: the bot is in the vehicle.
bool transportClose(bool own, float botToInfantrySpot2, float vehicleToVehicleSpot2, bool& noMove);

// The flag of an InformationType name, 0 for none (ITBiological, used by
// some shipped templates, isn't one: the original's parser answers -1 and
// sets nothing).
uint32_t informationTypeFlag(const std::string& name);

// The soldier's AI radius (the AI adaptor's vt+0xdc -> BF2142.exe 0x008a3d40
// -> the soldier object's vt+0x90, 0x006f6b90): its collision mesh's radius
// (interface 0xcaa32 at +0x4c, the instance 0x006e5d70 whose slot 0x74,
// 0x006e5ad0 -> 0x006dedb0, reads its first LOD's +0xc0), plus the object's
// vt+0x94 (0x005b6b80: 0) and at least each child's radius plus its offset
// (the soldier's children have no collision mesh: 0). Every soldier template
// names the built-in Soldier_CollisionMesh (0x006d8740 -> 0x006e4880 with no
// file), whose points span x and z +-0.4 and y -1..0.8: the farthest corner
// of that box (0x006aeda0), sqrt(0.4^2 + 1 + 0.4^2) = 1.149 m.
float soldierAiRadius();

// KitType (BF2142.exe 0x0088f6b0: KTRecon/Recon/0, KTAssault 1, KTSupport 2,
// KTAT 3, KTEngineer 4, KTMedic 5, KTSpecops 6, KTSniper 7, KTExtended1 8,
// KTExtended2 9), -1 for none. The player's +0x58 (player vt+0x1a4, read
// by the AI player adaptor's slot 0x40, 0x008a5020) holds its kit's.
int kitTypeIndex(const std::string& name);

// The AI radio messages (BF2142.exe 0x0088f000's table, the ids the comm
// object files them under, AIDLL 0x100a6fe0): bail_out 0, get_in 1,
// req_pickup 2, medic 3, need_repair 4, roger_that 6, negative 7,
// PLAYER_CONFIRM_thankyou 8, PLAYER_CONFIRM_sorry 9, need_ammo 10,
// PLAYER_TACTICS_gogogo 11, PLAYER_TACTICS_followme 12, laserpaint 13,
// req_new_order 14, defend 15, attack_position 16, move 17,
// COMMSL_COMRADIO_commend 18, rebuke 19, followorders 20, divide 21; -1 for
// another name.
int radioMessageType(const std::string& voiceName);

// The lob (AIDLL 0x100fa440, the explosive aim of 0x100dbd10): the launch
// direction that carries a round of `speed` along `d` (target - muzzle)
// under `gravity` (negative: down), the higher of the two arcs; false when
// out of reach.
bool lobDirection(const physics::Vec3& d, float speed, float gravity, physics::Vec3& direction);

// The explosive bonus of the weapon choice (AIDLL 0x10074ef0, x100 on the
// weapon's score and the bot's +0x249 set, vt+0x260): see BotManager::fireUrgency.
struct ExplosivePick {
	bool soldierBot = true;      // the bot's object is ITSoldier
	bool targetSoldier = true;   // the target's is
	float explosionRadius = 0;   // the weapon's +0x20 (0 for none)
	bool thrown = false, sniper = false;
	bool fireMode2 = false;      // aiWeapon slot 0x104 (automatic)
	float minRange = 0, maxRange = 0;
	float distance = 0;          // to the target
	float heightAbove = 0;       // the target's height over the bot
	bool lobbing = false;        // the bot's +0x208 (the aim's lob, vt+0x208)
	float now = 0;
	float conditionFailedAt = -1e9f; // +0x1f8 (soldier; 0x100d8910's failure)
	float vehicleFailedAt = -1e9f;   // +0x250 (vehicle; 0x100d8450's failure)
	bool seen = false;           // the memory item's flag 2
	bool secondHand = false;     // its flag 8: known from others, not seen by the bot itself
	float seenAt = -1e9f;        // its +0x58
	float explosiveShots[3] = {-1e9f, -1e9f, -1e9f}; // the bot's +0x1e0.. (newest first)
};
bool explosivePick(const ExplosivePick& pick);

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
	// aiComm.addAIRadioMessage <voice> <modifier> <max send period> <receive
	// time to live> (AI/AIRadioMessages.ai; the comm object's +0x54 per
	// type): the seconds a received message is kept, by type.
	std::map<int, float> radioLife;
	float radioLifeOf(int type) const;
	// The plug-in of a kind an AI template holds (null for none).
	const AiPlugin* plugin(const std::string& aiTemplate, const std::string& kind) const;
	// The template's ai::InformationType flags (aiTemplate.addType,
	// BF2142.exe 0x00882ff1 with the names of 0x0088fbd0), 0 for none.
	uint32_t typeFlags(const std::string& aiTemplate) const;

	// The pathfinding maps (aiPathfinding.createMap in AI/AIPathfinding.ai:
	// Infantry is map 0, Vehicle map 1; aiPathfinding.setActiveMap and
	// aiPathfinding.map.* in the level's AI/AI.ai). The bots read their
	// mesh rays' climb test from them (AIDLL map +0x24, 0x100b0440).
	struct PathfindingMap {
		std::string name;
		float maxSlope = 90;                 // degrees (+0xc holds its cosine, 0 at first)
		float allowedThresholdHeight = 0.35f; // +0x18
		float headClearance = 2;             // +0x14
	};
	std::vector<PathfindingMap> pathfindingMaps;
	int activeMap = -1;
	// A map's climb limits (map 0's rays always climb 0.35 m, 0x1003f930).
	ai::ClimbLimits climbLimits(int map) const;

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
	// Whether a driving bot holds its vehicle's horn (PIFire; 0x100a5c00).
	bool honking(int player) const;
	// Whether the bot's weapon choice gave the explosive bonus (its +0x249).
	bool explosive(int player) const;
	// A behaviour's urgency at the bot's last think (0 for none).
	float lastUrgency(int player, const std::string& behaviour) const;
	// A radio message (the game's AI event 0x1004e140 -> 0x10030b20 ->
	// 0x100a6fe0): `voiceName` from AI/AIRadioMessages.ai ("medic",
	// "need_ammo", ...) said by `sender`, heard by `receiver`, or with -1 by
	// every bot of the sender's team (who hears the radio is the game's
	// matter, not traced). A bot doesn't hear itself.
	void radio(int sender, const std::string& voiceName, int receiver = -1);
private:
	// An enemy a bot knows of (the sensing agent's memory item).
	struct Known {
		int player = -1;
		physics::Vec3 position; // where it was last seen
		float seen = -1e9f;     // when it was last seen
		float lostAt = -1;      // when it went out of sight, -1 while in sight
		float alertAt = -1e9f;  // a round passed by (360 degree awareness for 5 s)
		bool everSeen = false;  // the memory item's flag 2
		// Flag 8 (0x100ed2c0): learnt from another (a squadmate's fire plan,
		// someone in the same vehicle) and not seen by the bot itself since.
		bool secondHand = false;
		float seenAt = -1e9f;   // +0x58: when anyone last saw it
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
		bool reportedFiring = false;
		// Explosives (0x10074ef0, 0x100777a0, 0x100d8910, 0x100d8450).
		bool explosive = false;          // +0x249: the weapon choice's bonus applied
		bool lobbing = false;            // +0x208: the aim lobs (0x100dbd10)
		float conditionFailedAt = -1e9f; // +0x1f8
		float vehicleFailedAt = -1e9f;   // +0x250
		float lobFailedAt = -1e9f;       // +0x200: a lob's condition failed
		float explosiveShots[3] = {-1e9f, -1e9f, -1e9f}; // +0x1e0, +0x1e8, +0x1f0
		int sharedTarget = -1;           // the target the fire plan told the squad of // the fire report's last state    // the heat condition (0x100d9040) in its cooling state
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
		int seatTarget = -1;         // a seat of the bot's own vehicle (-1: any seat of vehicleTarget)
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
		float jumpUntil = -1;        // PIAction held (the follower's jump, 0x10095720 / 0x10092bc0)
		ai::DriveState drive;
		ai::SidestepState sidestep;  // the move's (0x100ce860 +0x64..+0x80)
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
		// The grenade warning (the path follower's +0x11c position and
		// +0x128 stamp, 0x10092920; read by 0x10092960): where, and when
		// the thrower took the weapon in hand.
		physics::Vec3 grenadeAt;
		float grenadeStamp = -1e9f;
		bool avoidGrenade = false; // the avoided object is -1: a grenade (0x10065360)
		float hornAt = -1e9f;      // the comm object's +0x80 (0x100a5c00)
		bool hornHeld = false;
		float waitingSince = -1;  // Idle: a driver waiting for a passenger
		bool usedLast = false;    // presses: use and the defibrillator want a release between
		bool switchedLast = false;
		// The comm object's received radio messages (bot vt+0xa4, +0xc: a
		// list per type, 0x100a6fe0), each kept for its lifetime (0x100a7130).
		struct Radio {
			int type = -1;
			int sender = -1;
			float at = 0;
		};
		std::vector<Radio> radio;
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
	void pickExplosive(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players);
	float moveUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight);
	float reviveUrgency(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players);
	float changeUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight);
	// A seat's value (0x100d3e70): `vehicle` 0 for the bot's own soldier.
	float seatValue(const Bot& bot, const GameWorld::PlayerInfo& self, uint32_t vehicle, int seat, bool other) const;
	// The seat value's transport test (0x100d3730): true when the bot's
	// order is close (see Bots.cpp).
	bool orderClose(const Bot& bot, const GameWorld::PlayerInfo& self, const GameWorld::VehicleInfo& vehicle,
		const std::string& rootAi, bool& inArea, bool& noMove) const;
	// Whether a known enemy's weapons can hurt the bot (the sensing agent's
	// +0x7c above 0, 0x100ebdd0; the bot's vt+0x9c).
	bool threatened(const Bot& bot, const GameWorld::PlayerInfo& self) const;
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
	// The fire plan's word to the squad (0x10089b70 -> 0x100c1ce0).
	void shareTarget(const GameWorld::PlayerInfo& self, const GameWorld::PlayerInfo& target);
	// A player's kit's KitType (kitTypeIndex), -1 for none.
	int kitTypeOf(const GameWorld::PlayerInfo& player) const;
	// Whether a radio message of `type` from `sender` is held (0x100a6160).
	bool heard(const Bot& bot, int type, int sender) const;
	// The explosive start conditions of the fire plan (0x100d8910 and
	// 0x100d8450; see Bots.cpp): true when the trigger may go.
	bool explosiveClear(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& muzzle,
		const physics::Vec3& aimDirection, const physics::Vec3& targetPoint, const WeaponAi& weapon,
		const std::string& weaponTemplate, bool soldierBot);
	// The first point the segment from..to is blocked at (bisecting
	// GameWorld::lineOfSight), false when it is clear.
	bool firstBlocked(const physics::Vec3& from, const physics::Vec3& to, uint32_t ignore, physics::Vec3& hit) const;
	// A thrown or fired round's launch speed and gravity (the AI weapon's
	// +0x28 and +0x2c x the world's gravity).
	void ballistics(const std::string& weaponTemplate, float& speed, float& gravity) const;
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
	// `object`: a MoveToObject's object (a vehicle to enter or repair),
	// which the sidestep doesn't step round.
	bool steer(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& to, int goalId, float seconds,
		SoldierInput& input, float radius = 0, uint32_t object = 0);
	bool drive(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& to, int goalId, float seconds,
		SoldierInput& input, float radius = 0);
	// The point MoveTo heads for now (the next strategic waypoint or area
	// on the way, else the goal), popping those reached (0x1008aaa0).
	physics::Vec3 moveTarget(Bot& bot, const GameWorld::PlayerInfo& self);
	bool follow(Bot& bot, const ai::NavMesh& mesh, const physics::Vec3& from, const physics::Vec3& to, int goalId,
		const ai::FollowerSettings& settings);
	ai::DriveSettings driveSettings(const GameWorld::PlayerInfo& self) const;
	// What the infantry steering's sidestep (0x100ce860, 0x100e63c0) sees:
	// the vehicles of the world list round the bot's controlled object
	// `own` at `at`.
	ai::SidestepSense sidestepSense(const Bot& bot, const physics::Vec3& at, uint32_t own, const ai::NavMesh& mesh,
		const ai::FollowerSettings& settings, const physics::Vec3& steerPoint, const physics::Vec3& goal,
		uint32_t object) const;
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
	// When a human player of each team last got out of a vehicle (the unit
	// component's per-team stamp, BF2142.exe vt+0x64 0x008a1b10 set by the
	// exit event 0x00894860, read through vt+0x60 by the seat value), and
	// the vehicle each human player was in at the last update.
	std::map<uint32_t, std::array<float, 3>> m_humanLeftAt;
	std::map<int, uint32_t> m_humanVehicle;
	// When each player took its weapon in hand: the AI weapon's +0x20
	// (BF2142.exe 0x008a6d80 stamps the newly selected weapon with the
	// clock, slot 0xfc 0x008924d0; read through slot 0x100 0x008924e0).
	struct InHand {
		int slot = -1;
		std::string weapon;
		float since = 0;
	};
	std::map<int, InHand> m_inHand;
	// The grenade warning (0x1004eab0) and the horn (0x100a5c00).
	void warnOfGrenades(Bot& bot, const GameWorld::PlayerInfo& self);
	void horn(Bot& bot, const GameWorld::PlayerInfo& self, const GameWorld::VehicleInfo& vehicle, SoldierInput& input);
	float m_time = 0;
	uint32_t m_random = 0x2545f491;
	float random(); // 0..1
	void flee(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds);
	void idle(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input);
};

// The soldier a driver would sound the horn at (AIDLL 0x100fc660's scan;
// see BotManager::horn): of `team` (not `self`), alive and on foot, ahead
// along `way` (the unit motion direction) within 15 m beyond both radii,
// on the line of travel; the last one in `players` order, -1 for none.
int hornTarget(const physics::Vec3& centre, const physics::Vec3& way, float ownRadius, int team, int self,
	const std::vector<GameWorld::PlayerInfo>& players);

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
