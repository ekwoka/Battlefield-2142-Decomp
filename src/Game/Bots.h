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
//           coordination factor (AIDLL 0x10089aa0);
//   c4      (0x10083750): 100 to place a charge in a point the team holds,
//           100 to set it off once that point is contested;
//   DefuseMines (0x10086870), RunOver (0x10087820), Avoid (0x10064850,
//           on foot only: wrecks, and 1000 for a grenade warning,
//           0x1004eab0); BridgeRepair is 0 (no broken bridges here).
//
// Pilots of Helicopter / TransportHelicopter seats fly with AIDLL's
// HeliMoveTo family (src/AI/Heli; BotManager::fly): BBPGotoWaypoint3d,
// BBPIdle3d and the gunship's BBPHeliFire3d.
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
// Weapons are chosen for the target by 0x10074ef0 (strength, range, ammo,
// guidance, the explosive bonus; chooseWeapon), the fighting
// distance kept within [minRange, 1.1 x preferred] (0x100795a0).
//
// Strategy (src/AI/Strategy): each side's SAI picks a strategy and gives
// squads attack and defend orders on strategic areas; bots go to a spot
// inside the ordered area (0x10060300). A team with bots and without a
// commander has AIDLL's AI commander (not a player): it shells its own
// points being taken with the artillery (0x1009c050; BotManager::command).
//
// Scripted bots (src/Game/BotsScript.cpp: ai.botMoveTo, botFlyTo,
// botSetWaypoints_Patrol/3d, deactivateBot): a goal object of AIDLL's
// (src/AI/Goals) in place of the SAI's orders.
//
// The settings come from the game's own scripts (aiSettings.*, the
// weaponTemplate.* lines of Objects/**/ai/Weapons.ai, aiStrategy.* and
// aiStrategicArea.*, aiTemplate.*); without them the shipped values are
// assumed.
#pragma once

#include "AI/Ballistics.h"
#include "AI/Behaviour.h"
#include "AI/Formation.h"
#include "AI/MineZones.h"
#include "AI/Goals.h"
#include "AI/Heli.h"
#include "AI/NavMesh.h"
#include "AI/PathFollower.h"
#include "AI/Strategy.h"
#include "GameWorld.h"

#include <functional>
#include <array>
#include <map>
#include <optional>
#include <set>
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
	// By CombatStrengthType (the names 0x008902f0 reads, "CSTInfantry",
	// "Infantry" or the misspelt "Infantery"): Infantry, LightArmour, HeavyArmour,
	// NavalArmour, Helicopter, AirPlane, Titan, Missiles.
	std::array<float, 8> strength {};
	float deviation = 14;
	float allowedDeviation = 0.5f;
	float explosionRadius = 0;  // +0x20
	bool indirect = false, thrown = false; // thrown: +2 (0x0088a650)
	bool explodesOnImpact = false; // +0x4e
	// +0x4c (setFiresThroughTransparent, 1 by default): without it the
	// weapon choice wants the target seen clear or known from others.
	bool firesThroughTransparent = true;
	bool emp = false; // +0x4d (isEmpWeapon): x (3 - the target's EMP level) in the weapon choice
	// +0x24 weaponTemplate.soundSphereRadius (BF2142.exe row 0x00887198,
	// executor 0x00887256; 50 m
	// by default, 0x0088a7b0), the AI weapon's slot 0x12c (0x00892880): how
	// far its firing is heard. The shipped Weapons.ai write
	// `weaponTemplate.setSoundSphereRadius`, which is no command (the
	// console has only the property), so every shipped weapon keeps 50 m.
	float soundSphere = 50;
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
	// Mobile (template constructor 0x008917b0): setSoundSphereRadius <min>
	// <max> (0x0088394e -> 0x00891840: +0x38 min 0, +0x34 max 30) and
	// setHearingProbability <near> <far> (0x00883a93 -> 0x00891860: +0x40
	// 0.01, +0x3c 0.1); the adaptor's slots 0x54 (0x008a1880: the sphere at
	// the unit's speed), 0x60 / 0x64.
	float soundSphereMin = 0, soundSphereMax = 30;
	float hearingNear = 0.01f, hearingFar = 0.1f;
	// Unit: cullDistance (0x00884818: +0x18 and its square +0x1c; 1000 by
	// default, 0x008919d0), the adaptor's slot 0x50 (0x008a1d60) read
	// through AIDLL 0x100aa5d0 by the memory pass 0x100ee430 alone.
	float cullDistance = 1000;
	// Unit: setHasExposedSoldier (+0x16; the seat value's armour bonus).
	bool exposedSoldier = false;
	// Unit: setUseNoPathfindingToGetToObject (executor 0x00884eaf, +0x15, 0
	// by default; 1 on the bipods and the ATs). AIDLL reads it through the
	// unit adaptor's slot 0x38 (0x008a1d10; AIDLL 0x100aa570) where it
	// weighs reaching the object: the vehicle choice 0x1006c520 and the
	// get-in plan 0x1006dac0 (with it, or without a Mobile part, the plans
	// without the infantry-map point's distance tests), MedicAssist
	// 0x10070200 / 0x10070da0, AmmoAssist 0x10071a10 / 0x100724e0, and
	// 0x1006bd70. Those tests read the Mobile's per-map point records, which
	// the rewrite doesn't keep (it counts every vehicle as reachable), so
	// the flag is kept and changes no decision here.
	bool noPathfindingToObject = false;
	// Mobile: vehicleNumber (executor 0x008834f9, +4, -1 by default and
	// shipped on the ATs): the adaptor's slot 0x24 (0x008a1770), read by
	// AIDLL 0x100a46c0 to pick the Mobile's per-map point record (+8 when
	// it is the pathfinder's slot 4, else +0x1c or +0x30): kept, unread
	// here for the same reason.
	int vehicleNumber = -1;
	// Unit (constructor 0x008919d0): setBattleStrength <CombatStrengthType>
	// <float> (0x00884b1e, +8: 8 floats) and setStrategicStrength <0|1>
	// <int> (0x00884c6f, +0xc: attack 0, defence 1). The SAI's area
	// strengths sum the battle ones, its required strength the strategic.
	std::array<float, 8> battleStrength {};
	std::array<int, 2> strategicStrength {};
	// Armament (type 0): setIsAntiAircraft (0x00884739, +8; 0 none, 1, 2).
	int antiAircraft = 0;
	// Unit: setSelectKey PIPositionSelectN (read through 0x100aa5f0): the
	// position key that moves a bot to this seat, N; 0 for none.
	int selectKey = 0;
	// ControlInfo (BF2142.exe's plug-in, constructor 0x00891d80; the AI
	// reads it through the adaptor vtable 0x009d6b98). The gains feed only
	// the aim controller (AIDLL 0x100e5820, from the adaptor's slots
	// 0x3c/0x40/0x44): +4 proportionalConstant (-7), +8 derivativeConstant
	// (-1), +0xc angleSpeedConstant (1.5); +0x10 yawScale (1) the mech's
	// and cars' steering.
	float proportional = -7, derivative = -1, angleSpeed = 1.5f, yawScale = 1;
	// The inputs a bot writes (io::PlayerInputMap: a c_PI index, 0x40 for
	// none; each setter also sets its bit in the group's mask, +0x28 drive,
	// +0x40 aim, +0x50 look). Drive: +0x14 throttle, +0x18 turn, +0x1c
	// pitch, +0x20 roll. Aim: +0x30 throttle, +0x34 vertical, +0x38
	// horizontal, +0x3c roll. Look: +0x48 vertical, +0x4c horizontal (the
	// aim controller's outputs, adaptor slots 0x30/0x34).
	int driveThrottle = 0x40, driveTurn = 0x40, drivePitch = 0x40, driveRoll = 0x40;
	int aimThrottle = 0x40, aimVertical = 0x40, aimHorizontal = 0x40, aimRoll = 0x40;
	int lookVertical = 0x40, lookHorizontal = 0x40;
	uint64_t driveMask = 0, aimMask = 0, lookMask = 0;
	// setCameraRelativeMin/MaxRotationDeg (0x00891fa0, 0x00892070: x pi /
	// 180, each clamped to +-pi; +0x64.., +0x70..; default -pi..pi) and
	// setCameraRelativeDofRotationOffsetDeg (+0x60, radians): the gunner's
	// scan limits (ScanForEnemies 0x100f9a90) and the turret's offset.
	std::array<float, 3> cameraMin {-3.1415927f, -3.1415927f, -3.1415927f};
	std::array<float, 3> cameraMax {3.1415927f, 3.1415927f, 3.1415927f};
	float dofRotationOffset = 0;
	// maxClimbAngle (+0x58) and maxRollAngle (+0x5c): only the plane
	// controller (0x100e6d00) reads them; no shipped template sets them.
	float maxClimbAngle = 0, maxRollAngle = 0;
};

// The aim controller's gains for a control object (its ControlInfo
// plug-in's, the constructor's defaults without one).
struct AimGains {
	float proportional = -7, derivative = -1, angleSpeed = 1.5f;
	int lookVertical = 0x40, lookHorizontal = 0x40;
};
// A c_PI axis name of io::PlayerInputMap (PIYaw 0 .. PICameraY 7; the
// "c_" prefix optional), 0x40 (none) otherwise.
int playerInputIndex(const std::string& name);

// aiTemplate.create <name> with its plug-ins (aiTemplate.addPlugIn).
struct AiTemplate {
	std::vector<std::string> plugins;
	float basicTemp = 0;
	// +0xc degeneration (the knowledge's decay out of sight, AIDLL +0x50),
	// +0x10 allowedTimeDiff (the AI object's update throttle, +0xc), +0x18
	// commonKnowledge (per-team or shared AI objects, 0x0088d0d0).
	float degeneration = 0, allowedTimeDiff = 0;
	bool commonKnowledge = false;
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
// The last factor on a bot's AI deviation (TriggerContinously 0x1008ecd0 ->
// BF2142.exe 0x00892b70's param_4): 3 for a bot object that isn't ITAir,
// isn't ITNaval and isn't primary (the engine adaptor's slot 0xec, via
// 0x100a8c60: 0 for an aiTemplate.secondary object) at an ITAir target;
// 1 otherwise.
float aiDeviationFactor(uint32_t botTypes, bool botPrimary, uint32_t targetTypes);
// The fire plan's distance kept (0x100795a0) for a weapon, with roll the
// U[0, 1] draw and dh the target's height over the bot's eye.
float preferredRange(const WeaponAi& w, float skill, float roll, float dh, bool explosivePlan = false);
// The plan's far reach (maxRange + 1.25 dh over 5 m up) and near limit.
float fireReach(const WeaponAi& w, float dh);
float fireMinRange(const WeaponAi& w, float dh, bool explosivePlan);
// MoveToObjectFinding's back-off spot (0x100d00e0) for a mover that isn't
// ITSoldier: near + min(10, 0.2 (keep - near)) from the target along its
// horizontal forward, else its right (z, -x), else its left; the first
// `clear` passes.
physics::Vec3 backOffSpot(const physics::Vec3& target, physics::Vec3 forward, float near, float keep,
	const std::function<bool(const physics::Vec3&)>& clear);
// Hearing (AIDLL). A unit's sound sphere (the Mobile adaptor's slot 0x54,
// BF2142.exe 0x008a1880): max at maxSpeed or faster, else lerp(min, max,
// speed / maxSpeed). No Mobile part: 0.
float soundSphereRadius(const AiPlugin* mobile, float speed);
// The hearing dispatcher's reach for a source (0x100ca280): the weapon's
// sphere while its weapon was taken in hand under 3 s ago, the Mobile
// sphere if larger; heard only past 0.1 m.
float hearingReach(bool weaponWithin3s, float weaponSphere, float mobileSphere);
// The roll's odds (0x100ea820) for a listener `distance` from a source:
// r = min(d / weapon sphere while the weapon is under 5 s in hand and d
// is inside it, d / Mobile sphere inside that), 1 otherwise; nothing at r
// = 1. Then p = far r + near (1 - r) with the listener's Mobile part, its
// speed fraction q = max(1, speed / maxSpeed) (0x100317e0 is a max), and
// the roll succeeds when U[0, 1) <= (1 - q) p, so never in practice; a
// listener without a Mobile part has q = 0, p = 1. `p` is what a success
// hands on; the return value is the threshold.
float hearingOdds(float distance, bool weaponWithin5s, float weaponSphere, float mobileSphere,
	const AiPlugin* listenerMobile, float listenerSpeed, float& p);
// The side's knowledge after a unit is heard with certainty c
// (0x100a9bb0, slot 2 of vtbl 0x10118838): nothing when its certainty is
// at least c; else the position copied and the last sighting set to now -
// 1 / (c degeneration), i.e. lastSeen returned (c clamped to [0, 1]; 0:
// long ago).
float heardLastSeen(float now, float c, float degeneration);
// BBFire3d's unit-count clause (0x1007d6c0 against 0x100d2b30, the bot's
// +0x24, 1 for every bot, 0x1009ac30): a target whose units' summed
// battle strengths against the bot's own type are at most 1 scores x 0.01.
float fire3dUnitScale(float summedStrength);
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

// KitType (BF2142.exe 0x0088f6b0, read by 0x00424ff0: KTRecon/Recon/0, KTAssault 1, KTSupport 2,
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

// The guided weapons' plan's move on foot (AIDLL 0x10078570): an If (not
// forced) whose condition is the target, by the bot's memory of it, farther
// than half the weapon's maxRange (ObjectDistance 0x100d4480 with +0x16,
// "farther than") or within its minRange, over MoveToObjectFinding
// (0x100cff90, mode 2) keeping 0.85 maxRange with the near limit max(15,
// 1.1 minRange) (0x100317e0 is a max), done when the distance lies between
// the two. `engaged` is the If's running move; returns +1 to close in, -1
// to back off, 0 to hold.
int guidedMoveStep(bool& engaged, float distance, float minRange, float maxRange);

// The bots' zoom. The AI weapon's zoom input (BF2142.exe's adaptor slot
// 0x68, 0x00892800: the AI weapon template's +0x5c, which 0x00892be0 fills
// through 0x0088a8c0 with the zoom component's input, its slot 0xa8, when
// the item's zoom template, class 0x94e5, has a factor that isn't 0,
// 0x005da9a0; 0x40, PINone, otherwise): the weapon's DefaultZoomComp
// levels (zoom.addZoomFactor, as the player's camera reads them).
anim::ZoomSettings zoomSettingsOf(const world::ObjectTemplate* weapon);
// The fire plans' zoom statement (0x100d8250, vtable 0x1011a9f0, made by
// 0x100d8160; added by the fire statements 0x100777a0 and the guided ones
// 0x100781e0 for a soldier bot, the object's flag bit 9, whose weapon has
// a zoom input). Its key is pressed when the bot pushes PIYaw, PIThrottle
// or PISprint (its input mask +0x100 bits 0, 3, 13 with a value) while
// zoomed (the adaptor's slot 0x70, 0x00892820: the firearm's slot 0x12c,
// its zoom level not 0), or stands still unzoomed; either only with the
// weapon ready (the AI weapon's slot 0x18, the adaptor's 0x118 -> the
// firearm's slot 0x90, 0x005c4e90: past its next-fire tick, no component
// busy). Otherwise the key is let go.
bool zoomStatementPress(bool moving, bool zoomed, bool ready);

// The AI weapon's guidance kind (+0x6c; BF2142.exe 0x00892be0 from the
// weapon's slot 0x154 through 0x008923a0): GenericFireArm 0x005c1fa0 asks
// its target component (+0x2c) slot 0xb4 (0x005f5b80), its template's
// +0x80: ObjectTemplate.target.targetSystem (0x005f6020 writes it out,
// names 0x004db008, read back by 0x004dd315): TSNone 0, TSHeatSeeking 1, TSLaserGuided 2,
// TSWireGuided 3, TSTVGuided 4; TSParabellic (5) and no target component 0.
int guidanceKind(const world::ObjectTemplate* weapon);

// One weapon of the bot's object for the weapon choice (AIDLL 0x10074ef0;
// the AI weapon's slots through BF2142.exe's adaptor, vtable 0x009d5be8).
struct WeaponOption {
	int type = 0;                 // WeaponAi::Type (+0x64, slot 0xf4)
	float strength = 0;           // against the target's CombatStrengthType (slot 0xa8)
	float explosionRadius = 0;    // +0x20 (slot 0xac)
	bool thrown = false;          // slot 0xb0
	bool fireMode2 = false;       // slot 0x104 (automatic)
	float minRange = 0, maxRange = 0; // +0x18, +0x1c (slots 0x84, 0x88)
	int guidance = 0;             // +0x6c (slot 0xf8): guidanceKind
	bool firesThroughTransparent = true; // +0x4c (slot 0x98)
	bool emp = false;             // +0x4d (slot 0x9c)
	// The AI weapon's slot 0xc (0x100d2050/0x100abb90 -> GenericFireArm
	// slot 0x8c 0x005c1be0): the rounds in all its magazines, -1 without
	// magazines (taken as 0x10000).
	int rounds = -1;
	// Slot 0xd4(0) (-> GenericFireArm slot 0xa4 0x005c3b00: the ammo
	// component's first magazine): the rounds in the gun; -1 without.
	int inMag = -1;
	// Slot 0xe8 (-> the ammo component's 0x005f3d20): max(reloadTime,
	// minimumTimeUntilReload); slot 0xe4 (0x005f3a90): ammo.autoReload.
	float reloadTime = 2;
	bool autoReload = false;
	bool usable = true;           // the AI weapon's slot 0x1c (GenericFireArm slot 0x170, 0x005c1150)
	// Wire guided: its target component holds a target (slot 0xbc; the
	// LPTargetComp's 0x005e2410).
	bool locked = false;
	// Close combat and revive: the target stands on the bot's mesh (its
	// map spot within 1 m) outside every vehicle the bot's follower steps
	// round (0x100947b0); and the mesh ray from the bot reaches it (the
	// pathfinder's slot 0x84).
	bool meleeReachable = true;
	bool meleeRay = false;
};

// The weapon choice's view of the bot, the target and the moment.
struct WeaponChoice {
	bool soldierBot = true;       // the bot's object is ITSoldier
	bool targetSoldier = true;
	bool targetHuman = true;      // the target is no bot (the bot manager's slot 0x20 has none)
	bool targetScripted = false;  // a target bot's +0x264 (vt+0x27c): x0.5
	float distance = 0;
	float targetEmpLevel = 0;     // 0x10074c60's 3 - level, for EMP weapons
	bool inHandMelee = false;     // the weapon in hand is CloseCombat or Revive
	bool inHandEmpty = false;     // ... else its magazine is empty
	bool targetInHandEmpty = false; // the target's weapon in hand has an empty magazine
	bool targetTargetsMe = false; // the target's attention on the bot (see Bots.cpp)
	bool targetMobile = true;     // the target has a Mobile part (+0x3c)
	float targetMotion = 0;       // its Mobile slot 0x78: the target's sprint stamina, 0..1 (see Bots.cpp)
	bool c4Placed = false;        // the bot's +0x1b8 (vt+0x13c): a charge of its own is out
	// C4's guard on a moving target (0x10074ef0, type 5 with the target's
	// Mobile part): the target's velocity (its Mobile slot 0x18) along the
	// bot's facing (the bot's AI object's slot 0x3c, the direction the
	// strategy's 0.9 test at 0x10075fb0 also takes); over 4 m/s either way
	// no C4.
	float targetAlongFacing = 0;
	bool clearSighting = true;    // the memory item's flag 0x40 (seen clear) or 8
	bool orderMove = false;       // the bot's +0x248 (vt+0x258)
	bool areaHold = false, targetInArea = false; // strategicAreaGuard
	ExplosivePick pick;           // the bonus's inputs; the weapon's fields per option
};
// The weapon of `weapons` to fight the target with, -1 for none (the
// original keeps its last choice then); `explosive` is the bot's +0x249
// (the bonus given).
int chooseWeapon(const WeaponChoice& choice, const std::vector<WeaponOption>& weapons, bool& explosive);

// A strategic area for the explosive bonus's guard (0x10034a10 and
// 0x10074ef0): whether the bot's team holds it (+0x70 + 4 team, 0 held),
// its centre on the bot's map (vt+0x10), the team's spot (+0x7c + 8 team)
// and the team's radius (+0x98 + 4 team).
struct GuardArea {
	bool held = true;
	physics::Vec3 centre;
	float spotX = 0, spotZ = 0;
	float radius = 0;
};
// The guard: of the areas the team doesn't hold, the one with the most
// radius / max(1, d^2) for the bot (d across, from the team's spot; slot
// 0x68 = AIDLL 0x1005dd10 -> 0x1005dd50 -> 0x1005dd90); with the spot inside
// that radius of the centre, a bot within 40 m of the centre pushing its
// PIYaw, PIThrottle or PISprint (the bot's input +4, mask +0x100 bits 0, 3,
// 13) holds the bonus (`hold`); so does a target within the radius of the
// spot (`targetInside`).
void strategicAreaGuard(const std::vector<GuardArea>& areas, const physics::Vec3& bot, bool moving,
	const physics::Vec3& target, bool& hold, bool& targetInside);

class BotManager;

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
	// kitTemplate.create <name> <kitType> (0x0088cc40; class vtable
	// 0x009d5d68: +4 strategic int[2], +8 battle float[8]); a soldier takes
	// the strategic strengths of the one named as its kit's AI template
	// (0x0088c0d0), its battle ones only when it carries no weapon.
	struct KitTemplate {
		int kitType = -1;
		std::array<int, 2> strategic {};
		std::array<float, 8> battle {};
	};
	std::map<std::string, KitTemplate> kitTemplates; // by lower-case name
	std::string editingKit;
	std::string editingTemplate, editingPlugin;
	// aiComm.addAIRadioMessage <voice> <modifier> <max send period> <receive
	// time to live> (AI/AIRadioMessages.ai; the comm object's +0x54 per
	// type): the seconds a received message is kept, by type.
	std::map<int, float> radioLife;
	float radioLifeOf(int type) const;
	// The modifier column, by type: a received message's +8 (0x100a6fe0
	// stores the radio event's second word), 10 for every shipped one.
	std::map<int, float> radioModifier;
	float radioModifierOf(int type) const;
	// The max send period column, by type (the comm's +100): how long a
	// sent type waits before it may be queued again, and how long one
	// heard holds a send of the same type back (0x100a7af0).
	std::map<int, float> radioSendPeriod;
	float radioSendPeriodOf(int type) const;
	// The plug-in of a kind an AI template holds (null for none).
	const AiPlugin* plugin(const std::string& aiTemplate, const std::string& kind) const;
	// The template's ai::InformationType flags (aiTemplate.addType,
	// BF2142.exe 0x00882ff1 with the names of 0x0088fbd0), 0 for none.
	uint32_t typeFlags(const std::string& aiTemplate) const;
	// Whether an AI template's objects are primary (not aiTemplate.secondary:
	// the engine adaptor's slot 0xec, 1 from vtables 0x009d60a8 / 0x009d6220,
	// 0 from 0x009d5db8 / 0x009d5f30, chosen by 0x0088d0d0 from the
	// template's +0x19). No template: primary.
	bool primary(const std::string& aiTemplate) const;

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

	// The rest of AIDLL's settings object (AISettings, constructor
	// 0x10051aa0; IAISettings vtbl 0x101148c8, IAISettingsModifier vtbl
	// 0x10114828, both of one object: offsets are the object's), set by the
	// aiSettings.* commands (0x1000ac80..0x10011570, src/Game/BotsScript.cpp).
	int maxAutoControllers = 0;      // setMaxNAutoControllers (+0xc; slot 1 is +8 plus +0xc)
	bool maxBotsIncludeHumans = true;// +0x37
	float ignoreCommanderDistance = 30; // setIgnoreCommanderDistance (+0x38)
	int maxBehaviours = 0;           // setMaxNumberOfBehaviours (+0x10, also DAT_10139520)
	float statsViewDistance = 300;   // setStatsViewDistance (+0x20)
	// The information grid it sizes (vtable 0x1011b934, constructor 0x100f95c0,
	// destructor 0x100f9690, cell range 0x100f7370; the circle query slot 6
	// 0x100f88d0, the other shapes slots 7-10 0x100f8b00, 0x100f9090,
	// 0x100f92c0, 0x100f93e0) only speeds up the object searches: the rewrite
	// scans the player and vehicle lists instead.
	int informationGridDimension = 32; // setInformationGridDimension (+0x2c; IAISettings slot 16 reads it, 0x100d2b40)
	int sides = 0;                   // setNSides (+0x30)
	float saiUpdateFrequency = 35;   // setSAIUpdateFrequency (+0x28): the SAI's gate (see updateStrategy)
	bool respawnAllowed = true;      // setRespawnAllowed (+0x34)
	bool autoSpawnBots = false;      // autoSpawnBots (+0x36; aiSettings.setAutoSpawnBots, which the
	                                 // shipped AIDefault.ai uses, is no command of AIDLL's)
	bool overrideMenuSettings = false; // +0x184
	// setVehicleClusterMapping <type> <type> (0x10050030): the first type
	// takes the second's cluster index (+0x28 of its record; read through
	// IAISettings slot 0x60).
	std::map<std::string, std::string> clusterMapping;
	std::string manDownEquipment;    // setManDownEquipment (+0x40)
	std::string basicBotWeights;     // setBasicBotWeights (0x10028da0: the modifier set, modifier slot 0x30)
	// setStandardPersonality <set> <side> (0x10027ff0 -> 0x100509d0, +0xf0
	// by side): the modifier set a side's bots get.
	std::map<int, std::string> standardPersonality;
	// Vehicle groups (createVehicleGroup, addVehicleToVehicleGroup).
	std::map<std::string, std::vector<std::string>> vehicleGroups;
	// setSensingAgentPatternName / setSensingAgentNullPattern (0x10026c50,
	// 0x10029d40): the sensing patterns' names.
	std::vector<std::string> sensingPatternNames;
	std::string sensingNullPattern;
	// addInterpreterEntry <equipment> <statement> (0x10029e10): the plan
	// statements an equipment type's interpreter runs (the implementations
	// by name, registered by the AI object's constructor 0x1002a400); removeInterpreterEntry
	// takes one out. The rewrite's patterns run their statements directly
	// (see "Action plans" in docs/engine.md); the table picks the driver
	// (moveInterpreter).
	std::map<std::string, std::vector<std::string>> interpreter;
	std::map<std::string, std::string> defaultInterpreter; // activateDefaultInterpreter <equipment> <type>
	// The equipment type's MoveTo statement (the driver it runs: CarMoveTo,
	// TankMoveTo, MechMoveTo, FloatingTankMoveTo, HeliMoveTo...; the
	// interpreter table's dispatch 0x100c4780, constructor 0x100c46f0,
	// destructors 0x100c4740 / 0x100c4620).
	std::string moveInterpreter(const std::string& equipment) const;
	// The bot manager's scheduling (vtbl 0x101196b0 on the bot manager,
	// DAT_10139504): setPlannedDecisionMakingThreshold (+0x6c, absolute),
	// setUnplannedDecisionMakingThreshold (+0x74), setDecisionMakingInterleave
	// <planned> <unplanned> (+0x60, +0x64). The quotients are shares of the
	// real-time budget (setSensingQuotient a b: a/(a+b) at +0x94;
	// setSystemQuotient d p s: +0x84, +0x8c, the rest), kept but not used
	// (see Scheduling). The defaults are the manager's reset's (0x100c9f40,
	// also its console slot 0x80 0x100ca0d0; it sets the system quotient
	// 30 40 30).
	double plannedThreshold = 0.5, unplannedThreshold = 0.3;
	int plannedInterleave = 2, unplannedInterleave = 2;
	std::array<unsigned, 2> sensingQuotient {1, 1};
	std::array<unsigned, 3> systemQuotient {1, 1, 1};
	// autoControllerTemplate.* (AI/AutoControllers.ai, AIDLL 0x10002210..0x10004b80;
	// the template 0x100a8ed0, 0x3c bytes), by name as given.
	struct AutoControllerTemplate {
		bool sendFireWarning = false;  // +0
		bool operatesAlone = false;    // +1
		float scanningSpeed = 0.03f;   // +4
		float accuracy = 0.5f;         // +8
		float fov = 20;                // +0xc, degrees (the auto controller's frustum, 0x100e0..: x pi / 180)
		float viewDistance = 30;       // +0x10
		bool useSiren = false;         // +0x14
		float reactionTime = 0;        // +0x18
		float targetLostTimeout = 0;   // +0x1c
		bool neverAttackDeployer = false; // +0x20
		bool reportEnemiesToTeam = false; // +0x21
		bool allowTakeOver = false;    // +0x22
		float maxTrackingInput = 4;    // +0x24
		bool checkLineOfFire = false;  // +0x28
		float discoverCloakedEnemiesDistance = 0; // +0x2c
		float flipOutAtHealth = 0;     // +0x30
		float scanningInputScale = 1;  // +0x34
		bool reloadOnTeamChange = false; // +0x38
		bool onlyAttackNetworkBattlefieldObjects = false; // +0x39
		bool simpleFirePlan = false;   // +0x3a
	};
	std::map<std::string, AutoControllerTemplate> autoControllers;
	std::string editingAutoController;
	const AutoControllerTemplate* autoController(const std::string& name) const;

	// aiSettings.addBotName <first> <last> (0x10050720): "first last" goes
	// into the name list at a random place (the list is kept by a key drawn
	// U[0, 0.4) for the first six names, U[0, 1) after, ascending); the
	// game names a bot added without a name by nextBotName (0x10050a20,
	// from BF2142.exe's AI player factory 0x0089e7a0): the head, which then
	// goes to the back.
	struct BotName {
		float key = 0;
		std::string name;
	};
	std::vector<BotName> botNames;
	uint32_t botNameSeed = 0x9e3779b9u; // the keys' U[0, 1) (the original's Mersenne twister, DAT_101394cc)
	void addBotName(const std::string& first, const std::string& last, float u);
	std::string nextBotName();

	bool initiated = false; // ai.init (the level's AI/AI.ai)
	// The running bot manager, for the scripted-bot commands (ai.botMoveTo
	// and the rest); BotManager::attach sets it and the manager's
	// destructor clears it.
	BotManager* manager = nullptr;

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
	// It is also the bots' spawn test (GameWorld::setAiSpawnTest: the
	// IAIPathfindingCourier, AIDLL vtable 0x10114b50 slot 0xc 0x10054ff0).
	void setNavMesh(const ai::NavMesh* mesh);
	bool enabled() const { return !m_noNavMesh; }
	// The vehicles' (AIPathFinding/Vehicle.qtr), for driving bots.
	void setVehicleNavMesh(const ai::NavMesh* mesh) { m_vehicleNavMesh = mesh; }
	// The server's maximum of players (sv.maxPlayers; AIDLL reads the game's
	// through DAT_10139454 slot 4) for AIMain's kick on a human's join and
	// fill on a leave (BotsSquad.cpp's population); 0 for none.
	void setServerMaxPlayers(int players) { m_serverMaxPlayers = players; }
	// A commander's strike for `team` at `at` reaching the squad manager
	// (0x10088040; IAIEventCourier slot 0x5c's way in): the side's bots in
	// squads leave 40 m round it at once, the other side's after 12.5..25
	// s, for 25 s. update() notes the world's new strikes itself.
	void noteStrike(int team, const physics::Vec3& at);
	// Thinks for every AI player and sets its input; call before the world ticks.
	void update(float seconds);

	// What a bot does now (for tests and debugging): the running
	// behaviour's name (MoveTo, Idle, Fire, Change, Revive, Flee, ...).
	std::string behaviour(int player) const;
	// A bot's value of a seat (0x100d3e70; `vehicle` 0 for its soldier), as
	// BBChange weighs it; 0 for no such bot.
	float seatValueFor(int player, uint32_t vehicle, int seat) const;
	int target(int player) const; // the enemy player it fights, -1 for none
	// The fire plan's distance kept and the carried slot it was made for
	// (planFire); 0 / -1 for no such bot.
	float preferredRange(int player) const;
	int planSlot(int player) const;
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
	// Whether a lob that went off holds its moves (its +0x209).
	bool holdingForLob(int player) const;
	// Whether its fire plan keeps to its order's spot (+0x248, 0x100795a0).
	bool keepsToOrder(int player) const;
	// The bot's weapon zoom level (0: unzoomed; see stepZoom).
	int zoomLevel(int player) const;
	// A behaviour's urgency at the bot's last think (0 for none).
	float lastUrgency(int player, const std::string& behaviour) const;
	// A radio message (the game's AI event 0x1004e140 -> AIDLL 0x10030b20 ->
	// 0x100a6fe0): `voiceName` from AI/AIRadioMessages.ai ("medic",
	// "need_ammo", ...) said by `sender`, heard by `receiver`, or with -1 by
	// every bot of the sender's team (the game's hand-out, 0x004a6f70, is
	// GameWorld::hearsRadio; update takes what the world heard). A bot
	// doesn't hear itself.
	void radio(int sender, const std::string& voiceName, int receiver = -1);
	// A bot's own radio (the comm object's sends, 0x100a7860, dispatched
	// by 0x100a7af0): what it has queued and what it said, oldest first.
	struct RadioSent {
		int type = -1;
		int channel = 0; // the AI's channel (0x00896270 maps it to a target channel)
		int target = -1;
		float at = 0;
	};
	std::vector<RadioSent> radioSent(int player) const;

	// The console's settings (registerBotConsoleMethods'): the scripted-bot
	// commands reach this manager through them, and the run-time setters
	// (forceStrategy, the scheduling) change both copies. The destructor
	// lets go.
	void attach(BotSettings& source);
	BotSettings& liveSettings() { return m_settings; }
	// AIDLL's scripted-bot commands (ai.*; 0x10005a30..0x10007a00): a goal
	// for a bot (bot vt+0x38; its MoveTo, or a pilot's GotoWaypoint3d,
	// follows it in place of the SAI's orders until it is done or the bot
	// dies; see BotsScript.cpp). False when the player is no bot.
	bool scriptGoal(int player, ai::Goal goal);
	// ai.botMoveTo <radius> <x/y/z> <bot> (0x100284a0): a waypoint goal, or
	// a fly-to with the radius as both reaches for a bot whose vehicle is
	// ITAir.
	bool scriptMoveTo(int player, float radius, const physics::Vec3& at);
	// ai.botGiveOrder <bot> <behaviour> <modifier set> (execute 0x100065e0
	// -> 0x10028dd0): with the behaviour named (its index, 0x10025350) and
	// the set known, the set's weights and the index go to the bot (slot
	// 0x40, 0x10098b80), which takes them only while its +0x50 holds; then
	// they replace +0x44 (multiplying the personality in 0x10096a50) and
	// name the hinted behaviour (+0x9c, slots 0x68 / 0x44). No writer of
	// the bot's +0x50 was found (its constructor 0x1009ac30 leaves it), so
	// the order is refused (returns false) and only kept for inspection.
	bool giveOrder(int player, const std::string& behaviour, const std::string& modifierSet);
	std::string lastOrder(int player) const;
	// ai.botFlyTo <bot> <radius> <height> <unused> <x/y/z> (0x100285a0):
	// only a bot whose vehicle is ITAir.
	bool scriptFlyTo(int player, float radius, float height, const physics::Vec3& at);
	const ai::Goal* scriptedGoal(int player) const;
	// ai.deactivateBot <bot> (AIMain slot 5, 0x10055f80): the bot leaves the
	// AI (its side's SAI and the bot manager) and stops steering; the
	// player stays. ai.updateBot <bot> (0x10026f30, bot vt+0x24): it thinks
	// at its next plan run.
	bool deactivate(int player);
	bool deactivated(int player) const { return m_deactivated.count(player) != 0; }
	void thinkSoon(int player);
	// The bots of the manager (ai.getBotCount, getNumberOfActiveBots,
	// getNumberOfDeactivedBots: the bot manager's slots 2, 0x74).
	int botCount() const;
	float time() const { return m_time; } // the bot manager's clock (its updates' sum)
	// ai.addBot: a bot player of the team in an open squad (it spawns as
	// the round spawns bots); its id, -1 for none.
	int addBot(const std::string& name, int team);
	int deactivatedCount() const { return static_cast<int>(m_deactivated.size()); }
	// Whether an AI player was refused for aiSettings.setMaxNBots.
	bool refused(int player) const { return m_refused.count(player) != 0; }
	// Whether a mine's zone marks a polygon of a mesh (0 infantry, 1 vehicles).
	bool mineMarked(int mesh, uint32_t polygon) const;
	// A unit's (player's) certainty for a side's SAI (ai::knowledgeCertainty):
	// 1 for its own and the bots' current sightings, decaying from the last
	// sighting by its template's degeneration, 0 for one the side's bots
	// never saw.
	float certainty(int team, int player) const;
	// The same for a vehicle (its root AI object): enemy vehicles too are
	// known per side (sensed, heard), neutral and own ones certain.
	float vehicleCertainty(int team, uint32_t vehicle) const;
	// Where the side places a unit (vtbl 0x10118838 slot 0x2c, 0x100a9b20):
	// live while sensed, else the remembered position; false for none.
	bool knownPosition(int team, int player, physics::Vec3& at) const;
	// The vehicles a bot heard (0x100ec100 / 0x100ec930's flag-1 items).
	std::vector<uint32_t> heardVehicles(int player) const;
	// A vehicle's team as the AI sees it (its Unit's: those aboard, else
	// the object's team).
	int vehicleTeam(uint32_t vehicle) const;

	// AIDLL's squads (the squad object, 0x110 bytes: constructor 0x100c4080,
	// reset 0x100c1700, destructor 0x100c38f0; nine a side, made up front
	// by the squad manager 0x1008a720 from IAIGame slot 0x50, BF2142.exe
	// 0x00895850: 10, squads 1..9). See BotsSquad.cpp.
	struct SquadMember {
		int player = -1;            // +0x04 (its AI player +0x10, its bot +0x14)
		bool placed = false;        // +0x4c: its AI object's position known
		bool moving = false;        // +0x54: moved over 0.1 m since the last look
		physics::Vec3 at;           // +0x40
		bool atSlot = false;        // +0x34: within sqrt(10) m of its slot
		bool goalSet = false;       // +0x35
		bool fresh = false;         // +0x4d (set by the squad's reset 0x100bf640, never cleared)
		bool regoal = false;        // +0x4e
		physics::Vec3 goal;         // +0x64: the goal it was last given
		physics::Vec3 lastGoal;     // +0x58: the goal at the last 9 s look
		float checkedAt = 0;        // +0x28
		float speed = 0;            // +0x30: the speed it walks at (0x100e6060 reads it)
		float eta = 1;              // +0x20: squared distance to its slot over its speed
		bool escaping = false;      // +0x4f: leaving a strike spot (0x100bfcb0)
	};
	struct SquadDanger {            // a node of the list at +0x14 (pruned by 0x100c1b60)
		float until = 0;            // +0x00
		int member = -1;            // +0x24: whose position is watched
		physics::Vec3 at;           // +0x34: where it was
	};
	struct Squad {
		int team = 0, id = 0;       // +0x20 (side - 1), +0x1c (squad - 1)
		std::vector<SquadMember> members; // +0x04
		int leader = -1;            // +0x78: the index of the game's squad leader
		// The order: +0x98 (0 none, 1 taken but not accepted, 2 under
		// way), its kind +0x38 (0 attack, 1 defend, 2 move, 4 none), spot
		// +0x2c, strategic area +0x28, commander's +0x48; the order taken
		// (0x100c1eb0): spot +0x54, kind +0x60, commander's +0x70, until
		// +0xa8.
		int state = 0;
		int kind = 4;
		physics::Vec3 orderAt;
		int area = -1;
		bool fromCommander = false;
		physics::Vec3 takenAt;
		int takenKind = 4;
		bool takenFromCommander = false;
		float takenUntil = 0;
		physics::Vec3 at;           // +0x8c: the leader's spot (0x100c32b0)
		ai::Formation formation;    // +0x7c
		int mode = 0;               // +0xb8: 0, 1 bounding, 2 flanking (0x100c1940)
		int boundPhase = 0;         // +0xbc
		float boundNext = 0;        // +0xb0
		bool flanking = false;      // +0xc0
		bool flankTried = false;    // +0xc1
		physics::Vec3 flank[2];     // +0xc8
		std::vector<SquadDanger> dangers; // +0x14
		float requestedAt = 0;      // +0x100: the last req_new_order
		bool answerOrder = false;   // +0x108: the members say roger_that / negative on their goal
		// The world's order this squad last saw (the game's event 0x10089480
		// reaching 0x100c1eb0 when it changes).
		GameWorld::OrderType seenType = GameWorld::OrderType::None;
		physics::Vec3 seenAt;
		bool updated = false;
	};
	const Squad* squad(int team, int id) const;
	// The team's kits by kit index (the game's Team::kits), for the SAI's
	// kit choice at respawn (0x100f04a0); without them the bots keep theirs.
	void setTeamKits(int team, std::vector<std::pair<std::string, std::string>> kits);
	// The kit the SAI chose at the bot's last death (-1 none).
	int respawnKit(int player) const;
	int respawnPoint(int player) const;
	// The sensing agent's chosen sector (-1 none) and whether a look plan runs.
	int lookSector(int player) const;
	bool lookPlanned(int player) const;
	// Whether a bot may advance (bot +400, vt+0x128 / 0x12c: set by the
	// squad's bounding and flanking; read by the look-around 0x100d7840,
	// which without it looks along the walk).
	bool mayAdvance(int player) const;
	// A bot's squad goal: its formation slot, false for none.
	bool formationGoal(int player, physics::Vec3& at) const;
	// The squad update (0x100c4240) of one squad now, outside the round robin
	// (for tests).
	void updateSquadNow(int team, int id);
private:
	// An enemy a bot knows of (the sensing agent's memory item; the agent's
	// constructor 0x100ecee0, its memory cleared on the bot's reset 0x100ed040).
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
		// Flag 0x40: the last sighting was clear, not through a see-through
		// material (the sight ray's out flag, BF2142.exe 0x00898040: set by
		// a new item's sighting 0x100ed2c0, set or cleared by a kept one's
		// 0x100ee430).
		bool seenClear = false;
		// +0x68: the item was offered to the spotting report (once, on the
		// bot's own first sighting, 0x100ed2c0).
		bool spotted = false;
	};
	// An enemy vehicle in a bot's memory (its root AI object's item):
	// when it was last seen, lost (-1 in sight) and heard (flag 1, -1e9
	// none; dropped 30 s after, 0x100ec930).
	struct KnownVehicle {
		float seen = -1e9f, lostAt = -1, heardAt = -1e9f;
	};
	// A bot (AIDLL's bot object, vtbl 0x10118050: constructor 0x1009ac30 with
	// its parts 0x10097470; bound to its player's input block by 0x100978d0
	// -> 0x100974d0, which marks the inputs the AI drives; destructor
	// 0x1009b620, deleting 0x1009b8c0).
	struct Bot {
		ai::Arbitration arbitration;
		std::string equipment;
		std::string behaviour = "Idle";
		float lastThink = -1;
		float spawnedAt = 0;
		std::vector<Known> known;
		std::map<uint32_t, KnownVehicle> knownVehicles;
		float firedAt = -1e9f; // +0xc0: the bot's last round (slot 0x314, 0x10097a60); hearing waits 1.5 s
		// Fire.
		int target = -1;
		int weaponSlot = 0;
		// At a seat: the chosen one of the seat's weapons (an index into its
		// SeatLayout::weapons, -1 the active one).
		int seatWeapon = -1;
		bool seatSelectTap = false; // its selection key pressed last execution
		float targetAcquired = 0;
		float triggerAt = 0;      // when the trigger may first be pulled
		float preferredRange = 0; // the fighting distance kept
		// The driving plan's manoeuvre (0x100795a0's If over
		// MoveToObjectFinding 0x100cff90): running until it arrives, the
		// back-off spot and when it was last chosen (a repath each second).
		bool manoeuvre = false;
		bool chase = false; // the lost target's chase (then forgotten)
		// The guided plan's move on foot (0x10078570's If, not forced, over
		// MoveToObjectFinding): running until the bot is within its band.
		bool guidedMove = false;
		physics::Vec3 backOff;
		float backOffAt = -1e9f;
		// The fire plan's target and weapon (0x1007cf10 keeps the plan, and
		// its distance, while both hold).
		int planTarget = -1;
		int planSlot = 0;
		physics::Vec3 lastAimPoint;
		// The aim's ballistic lead (the aim object's solver record,
		// 0x100d9a60 -> 0x100fa6f0).
		ai::LeadSolver lead;
		float yawError = 0, pitchError = 0;
		// The aim controller's gains, from the control object's ControlInfo
		// (the seat's, or the soldier's on foot): looked up once per
		// control object (vehicle, seat), not per tick.
		AimGains gains;
		uint32_t gainsVehicle = 0;
		int gainsSeat = -2;
		std::array<float, 10> pitchErrors {};
		int pitchErrorCount = 0;
		float pitch = 0;          // the look's pitch, degrees
		float devYaw = 0, devPitch = 0; // the aim deviation applied (degrees)
		bool tapped = false;      // Trigger: pressed last tick
		bool heatStop = false;
		bool reportedFiring = false;
		// Explosives (0x10074ef0, 0x100777a0, 0x100d8910, 0x100d8450).
		bool explosive = false;          // +0x249: the weapon choice's bonus applied
		bool lobbing = false;            // +0x208: the aim lobs (AIDLL 0x100dbd10)
		bool lobHold = false;            // +0x209: a lob's condition held (vt+0x20c; read by 0x100e6060)
		bool orderMove = false;          // +0x248: the fire plan moves to the order's spot (vt+0x254)
		physics::Vec3 orderSpot;         // that spot: the goal object's head (bot +0x80 slot 8)
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
		float jumpUntil = -1;        // PIAction held (the follower's jump, 0x10095720 / AIDLL 0x10092bc0)
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
		bool avoidGrenade = false; // the avoided object is -1: a grenade (AIDLL 0x10065360)
		float hornAt = -1e9f;      // the comm object's +0x80 (0x100a5c00)
		bool hornHeld = false;
		float waitingSince = -1;  // Idle: a driver waiting for a passenger
		bool usedLast = false;    // presses: use and the defibrillator want a release between
		int titanGun = -1;        // Titan mode: the gun seat of its Titan it goes to man (claimed)
		bool switchedLast = false;
		bool reloadedLast = false; // c_PIReload pressed at the last execution (a release between presses)
		// The comm object's received radio messages (bot vt+0xa4, +0xc: a
		// list per type, 0x100a6fe0), each kept for its lifetime (0x100a7130).
		struct Radio {
			int type = -1;
			int sender = -1;
			float at = 0;
			float modifier = 10; // +8
			bool answered = false; // +0x18 (0x100a61c0 marks it)
		};
		std::vector<Radio> radio;
		// The comm's sends (0x100a7860): the queue by send time (+0x28,
		// a heap), the next time each type may be queued (+0x3c), and what
		// went out (for checks).
		std::vector<RadioSent> radioQueue;
		std::map<int, float> radioNext;
		std::vector<RadioSent> radioSent;
		bool firedLast = false;
		// The weapon's zoom as the bot's key steps it (stepZoom): the zoom
		// component's level (+0x1c) and the player's field of view moving
		// towards it, the weapon they belong to, the key's press this
		// execution and at the last, the reload last time.
		anim::ZoomSettings zoomSettings;
		anim::ZoomState zoom;
		anim::FieldOfView fov;
		std::string zoomWeapon;
		bool zoomPress = false, zoomKey = false, zoomReloadWas = false;
		// A helicopter's pilot (src/AI/Heli): the controller's inputs and
		// airborne flag (bot +0x140), the plan's hover point and its take-off
		// wait (BBPGotoWaypoint3d 0x10067690), the last velocity and aim
		// (HeliAimAt), the gunship's attack run (BBPHeliFire3d 0x1007f9d0).
		ai::HeliState heli;
		uint32_t heliVehicle = 0;
		physics::Vec3 heliGoal, heliHover;
		bool heliPlanned = false;
		std::string heliPattern;
		float heliWaitUntil = -1;
		physics::Vec3 heliLastVelocity;
		float heliLastAt = -1;
		float heliLastDirY = 0;
		int heliRun = 0;        // 0 approach, 1 away, 2 back
		float heliTapAt = -1e9f;
		// When the bot last took a seat (BBChangeLandingCraft's x6 within 15 s).
		float seatedAt = -1e9f;
		uint32_t seatedVehicle = 0;
		int seatedSeat = -1;
		// HeliAimAt's lead (0x100dcf10) and the attack run's on-target
		// condition (0x100dcab0), made with the run's plan.
		ai::LeadSolver heliLead;
		physics::Vec3 heliLeadDirection {0, 0, 1};
		std::optional<ai::OnTargetTest> heliOnTarget;
		// The squad's goal for a member in its formation (0x100bff90: a
		// waypoint goal, reach 0, area 60, at the slot) and the re-goal the
		// squad asks of one heading for the order (0x100c0130).
		bool squadGoal = false;
		ai::Goal formation;
		bool orderRegoal = false;
		int idlePassenger = -1;      // BBPIdle2d's +0xc: whom the driver waits for (0x1006b1f0)
		bool pickupAsked = false;    // the get-in plan's +0x14: req_pickup said
		int pilotWeapon = -1;        // BBPHeliFire3d's +0x14: the pilot's weapon (0x100dd4a0)
		bool reviveShock = false;    // the crouch-then-fire's press made (0x100dda20 +0x18)
		bool mayAdvance = true;      // +400 (the constructor's 1, 0x1009ac30)
		// The sensing agent's look-around (bot +0xdc, AIDLL 0x100d7210; see
		// BotsLook.cpp).
		struct Look {
			std::array<float, 8> sectorW {}; // +0x3c
			int sector = -1;                 // +0x20
			float sectorWeight = 0;          // +0x24
			physics::Vec3 sectorPoint;       // +0x28
			bool reached = false;            // +0x34
			float moveX = 0, moveZ = 0;      // +0x5c, +0x60 (0x1008f8e0)
			bool requested = false;          // +0x48: the Turn's request (0x100d58b0)
			physics::Vec3 request;           // +0x4c
			float requestWeight = 0;         // +0x58
			float decidedAt = 0;             // +0x80
			bool force = false, forceLatch = false; // +0x88, +0x89
			bool paused = false;             // +0x8a (0x100d51c0)
			float pausedAt = 0;              // +0x90
			bool aheadOfMove = false;        // +0x99
			bool plan = false;               // +0x9c: a look plan runs
			bool needPlan = true;            // +0xa0
			bool sectorPlan = true;          // +0xa8
			physics::Vec3 offset;            // the plan's look offset (0x100de2c0's)
			bool offsetFromRoot = false;     // a stimulus's: from the soldier's root, else a way
			float duration = 0.75f;          // +0xac
			bool started = false;            // the reset (0x100d6b90) done
			bool quiet = false;              // the aim called by the walk or the look: no pause
			std::string pattern;             // the plan the last execution ran (the Turn's request on a new walk)
		};
		Look look;
		// The rounds that passed the bot (the perception's list +0x6c, round
		// records 0x100c4500): the shooter, when, where it hit.
		struct RoundSeen {
			int shooter = -1;
			float at = 0;
			physics::Vec3 impact;
			bool friendly = false;
		};
		std::vector<RoundSeen> roundsSeen;
		float walkFactor = 1;        // the member's +0x30 over the soldier's top speed (0x100e6060)
	};
	// The side's strategic AI.
	struct Side {
		std::string strategy;
		std::vector<float> weights;
		float startedAt = 0;
		float nextUpdate = 0;
		std::map<int, int> squadArea; // squad -> area index it attacks/defends
		std::vector<int> attacks, defences;
		std::map<std::string, int> picks; // the strategy records' +0xc (ai::strategyTimeUp)
		std::map<int, bool> lonerAttacks;  // steps 10 / 11: a bot outside squads -> attack (else defend)
		std::vector<float> temperature;    // per area, the side's (+0xa0 + 4 side) at the last update
	};

	void sense(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players);
	// The bots' spotting (0x100a5850 -> 0x10030fb0): a first sighting
	// near a strategic area reported to the team's minimap and said aloud,
	// throttled per team and area. Whether it was spotted.
	bool spotSighting(const GameWorld::PlayerInfo& self, const GameWorld::PlayerInfo& other);
	// The throttle (DAT_101394d0, vtable 0x10113b30; reset 0x10031560): per
	// team (+0x1c + 0x10 (team - 1)) and area, the kind last spotted and
	// when ({8, -1000} at the start), and one stamp for aircraft (+0x38).
	struct SpotEntry {
		int kind = 8;
		float at = -1000;
	};
	std::array<std::vector<SpotEntry>, 2> m_spots;
	float m_airSpotAt = -1000;
	void hitBy(int victim, int attacker);
	void noticeRounds(Bot& bot, const GameWorld::PlayerInfo& self);
	void think(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players);
	void act(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds);
	// Urgencies (BB slot 9) of the behaviours in the set.
	float urgency(Bot& bot, const ai::BehaviourSetup& behaviour, float weight, const GameWorld::PlayerInfo& self,
		const std::vector<GameWorld::PlayerInfo>& players);
	float fireUrgency(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players);
	// The weapon choice for the bot's target (0x10074ef0): sets the slot
	// and the explosive flag.
	void pickWeapon(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players);
	void planFire(Bot& bot, const GameWorld::PlayerInfo& self, bool gunning);
	// The first message of a type a bot holds from a human alive and not
	// down (the comm's list for the type, its first entry; the sender's
	// slots 0x84, 0x20, 0x28), null for none.
	const Bot::Radio* heardFromHuman(const Bot& bot, int type) const;
	float moveUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight);
	float reviveUrgency(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players);
	float changeUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight);
	// The comm object's send (0x100a7860): `type` on the AI's `channel`
	// `delay` s from now, unless the type was queued within its send
	// period; and its update (0x100a7af0): the low-health call
	// (0x100a7910), then what is due goes out (0x100a57c0).
	void queueRadio(Bot& bot, int type, int channel, float delay, int target = -1);
	void sendRadioNow(Bot& bot, const GameWorld::PlayerInfo& self, int type, int channel, int target = -1);
	void updateComm(Bot& bot, const GameWorld::PlayerInfo& self);
	// Marks the bot's unanswered messages of a type answered (0x100a61c0):
	// true when there was one.
	bool answerRadio(Bot& bot, int type);
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
	// The squad coordination factor (AIDLL 0x10089aa0): whether this bot should
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
	// What the SAI reads of an object template, cached per template: its AI
	// template's basicTemp (+0x14), its Unit plug-in (nullptr for none),
	// for a kit the kitTemplate its AI template names, and for a vehicle
	// the basicTemp of its seats (every seat's AI object counts).
	struct UnitAi {
		float basicTemp = 0, seatsTemp = 0;
		const AiPlugin* unit = nullptr;
		const BotSettings::KitTemplate* kit = nullptr;
		bool seatsKnown = false;
	};
	const UnitAi& unitAiOf(const world::ObjectTemplate* t) const;
	mutable std::map<const world::ObjectTemplate*, UnitAi> m_unitAi;
	// A unit's strengths in the SAI (AIDLL 0x100aa740): its Unit's battle
	// and strategic strengths x the capability factor 0.25 + 0.5 S(ammo) +
	// 0.25 S(health) (0x100aa420; ammo counts as full here). A soldier's
	// battle strengths are the most of its weapons' (their setStrength), its
	// strategic ones its kit's kitTemplate's (0x008a1e80).
	// `health` < 0: the unit's own now; else the side's kept value (the
	// knowledge's capability, refreshed under allowedTimeDiff).
	void unitStrengths(const GameWorld::PlayerInfo& p, float& battle, std::array<float, 2>& strategic, float health = -1) const;
	// The unit's battle strengths by CombatStrengthType, capability scaled
	// (the Unit part's +8, 0x100aa740): a soldier's, or (vehicle) its
	// root's and every seat's Unit summed.
	std::array<float, 8> battleArray(const GameWorld::PlayerInfo& p) const;
	// Plans (what the chosen behaviour does).
	void fire(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds);
	// `leadWeapon`: the weapon whose round the aim leads the point for
	// (empty: no lead).
	void aim(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& point, const physics::Vec3& pointVelocity,
		bool hasTarget, const std::string& leadWeapon, SoldierInput& input, float seconds);
	// The round the AI weapon template describes for the solver (0x00892be0):
	// the muzzle speed, the drag and the gravity; `indirect` its
	// weaponTemplate.indirect (+0).
	ai::RoundModel roundModel(const std::string& weaponTemplate, bool* indirect = nullptr) const;
	// The fire plan's +0x248 (0x100795a0): fight on the way to the order's spot.
	float landingCraftUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight);
	float fire3dUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight,
		const std::vector<GameWorld::PlayerInfo>& players);
	// The goal object's urgency for the bot where it stands (its slot 1,
	// as BBMoveTo asks it; bot +0x80, set by vt+0x38): no side effects.
	float goalObjectUrgency(const Bot& bot, const GameWorld::PlayerInfo& self) const;
	// The guided weapons' plan on foot (0x10078570): whether its trigger is
	// held (see Bots.cpp); `guidance` the weapon's guidanceKind.
	bool guidedFire(const Bot& bot, const GameWorld::PlayerInfo& self, const WeaponAi& weapon, int guidance,
		const Known& known, bool inSight, const physics::Vec3& look, const physics::Vec3& toTarget) const;
	// The guided plan's order spot (0x10078570; see Bots.cpp): whether it
	// walks there while it fights, and the spot.
	bool guidedOrderSpot(const GameWorld::PlayerInfo& self, physics::Vec3& spot);
	bool fireKeepsToOrder(const Bot& bot, const GameWorld::PlayerInfo& self, const GameWorld::PlayerInfo& target,
		physics::Vec3* spot = nullptr) const;
	// Walks (InfanteryMoveTo) or drives (CarMoveTo/TankMoveTo) along a path
	// to `to`; false once it has arrived. `radius` is the goal's.
	// `object`: a MoveToObject's object (a vehicle to enter or repair),
	// which the sidestep doesn't step round.
	bool steer(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& to, int goalId, float seconds,
		SoldierInput& input, float radius = 0, uint32_t object = 0);
	bool drive(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& to, int goalId, float seconds,
		SoldierInput& input, float radius = 0);
	// The fire plan's moves for a bot at the wheel (0x100795a0 for an object
	// with a Mobile part); true when the lost target was chased down and
	// forgotten.
	bool driveFirePlan(Bot& bot, const GameWorld::PlayerInfo& self, const GameWorld::PlayerInfo& target, const Known& known,
		bool inSight, const WeaponAi& weapon, bool explosivePlan, float seconds, SoldierInput& input);
	// The point MoveTo heads for now (the next strategic waypoint or area
	// on the way, else the goal), popping those reached (0x1008aaa0).
	physics::Vec3 moveTarget(Bot& bot, const GameWorld::PlayerInfo& self);
	bool follow(Bot& bot, const ai::NavMesh& mesh, const physics::Vec3& from, const physics::Vec3& to, int goalId,
		const ai::FollowerSettings& settings);
	ai::DriveSettings driveSettings(const GameWorld::PlayerInfo& self) const;
	// What the infantry steering's sidestep (AIDLL 0x100ce860, 0x100e63c0) sees:
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
	// The sensing agent's look-around (BotsLook.cpp): its update and plan
	// (0x100d7840) after the main plan, the faced sectors (0x100d6d60) at
	// each sensing pass.
	void lookAround(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds);
	void faceSectors(Bot& bot, float yaw);
	// Whether the bot's seat runs the Turret sensing pattern's look-around
	// (0x1008c120: the vehicle's equipment type names Turret, the seat's
	// controlled object has its ControlInfo and the seat aims a gun).
	bool turretLooks(const GameWorld::PlayerInfo& self) const;
	// The squads (BotsSquad.cpp): the manager's round robin (0x1008a910),
	// one squad's update (0x100c4240) and its parts.
	void updateSquads();
	void updateSquad(Squad& squad);
	void syncSquadMembers(Squad& squad);
	void takeOrder(Squad& squad, int kind, const physics::Vec3& at, bool fromCommander);
	bool acceptOrder(Squad& squad, int by);
	void updateMember(Squad& squad, SquadMember& member);
	bool inFormation(const Squad& squad, const SquadMember& member) const;
	bool slotPoint(Squad& squad, int player, physics::Vec3& at);
	void updateFormation(Squad& squad);
	void squadMode(Squad& squad);
	void squadRadio(Squad& squad);
	void releaseSquad(int team, int id, int area);
	bool reachable(const physics::Vec3& at) const;
	int areaAt(const physics::Vec3& at, int team, physics::Vec3* centre = nullptr, float* radius = nullptr) const;
	ai::AreaStatus areaStatus(int area, int team) const;
	SquadMember* memberOf(Squad& squad, int player);
	const GameWorld::PlayerInfo* squadLeaderOf(const Squad& squad) const;
	bool humanCommands(int team) const;
	std::map<std::pair<int, int>, Squad> m_squads; // (team, squad id)
	// The SAI's respawn (0x100f0590): per bot, whether this death's choice is
	// made, and the control point of the last spawn point chosen (the
	// player's vt+0xf4, kept 20 % of the time).
	struct Respawn {
		bool chosen = false;
		int controlPoint = -1;
		int kit = -1;
	};
	std::map<int, Respawn> m_respawn;
	std::array<std::vector<std::pair<std::string, std::string>>, 3> m_teamKits; // (kit, soldier) by kit index
	void chooseRespawn(const GameWorld::PlayerInfo& self);
	int m_squadCursor = 0; // +0x20
	int m_squadSide = 0;   // +0x24
	// The squad manager's strike spots, one record a side (+0x28 + 0x48
	// (side - 1), set by 0x10088040): where that side's commander shelled,
	// when, and how long the other side takes to learn of it (+0x48); the
	// escape points found for that side's soldiers (+0x50 / +0x54) and
	// vehicles (+0x51 / +0x60). Radius +0xb8 40 m, life +0xc0 25 s.
	struct StrikeSpot {
		bool active = false;
		float at = 0;
		physics::Vec3 spot;
		float enemyDelay = 0;
		bool escape[2] = {false, false};
		physics::Vec3 escapeAt[2];
	};
	StrikeSpot m_strikeSpots[2];
	int m_serverMaxPlayers = 0;
	std::set<int> m_humansSeen; // AIMain's +0x100: the humans it was told of
	void population();
	std::set<int> m_downBots;           // bots man down (for the revive's event 7)
	std::map<int, int> m_teamKillsSeen; // each bot's team kills at the last tick (event 0xb)
	std::vector<std::pair<int, physics::Vec3>> m_strikesSeen; // the world's strikes on their way last tick
	// The humans off the navigation mesh, by player, since when (+0xd4,
	// 0x1008a3b0): the AI commander's second artillery target (0x100888d0).
	std::map<int, float> m_offMesh;
	void updateStrikeSpots();
	bool strikeSpotHit(int team, const physics::Vec3& at, int& side) const;
	bool escapeStrike(Squad& squad, SquadMember& member, Bot& bot, const GameWorld::PlayerInfo& self, int side);
	bool onMeshFor(const GameWorld::PlayerInfo& self, const physics::Vec3& at) const;
	void trackHumans();
	bool offMeshEnemy(int team, physics::Vec3& at) const;
	// A bot commanding its team (AIDLL 0x1009c050; see Bots.cpp).
	void command(float seconds);

	// Helpers.
	float healthOf(uint32_t object) const;
	std::vector<std::pair<int, std::string>> carried(const GameWorld::PlayerInfo& self) const;
	// What of that the AI has a weapon for: an item whose object names an
	// aiTemplate that is a weaponTemplate (with the AI scripts loaded; all
	// of it without them, the tests' built-in rifle stand-in).
	std::vector<std::pair<int, std::string>> armament(const GameWorld::PlayerInfo& self) const;
	// A seat's armament (the Armament list at 0x10075170 over the bot's
	// object's weapons): its weapons whose aiTemplate names a
	// weaponTemplate, by index into the seat's SeatLayout::weapons; without
	// the AI scripts (the tests) the seat's active weapon alone.
	std::vector<std::pair<int, std::string>> seatArmament(const GameWorld::PlayerInfo& self) const;
	// Whether the bot works a seat's gun (its aim the gun's, its weapons
	// the seat's): a gunner's (`gunner`, the site's own test), or a ground
	// driver's whose look turns the root's turret or barrel
	// (GameWorld::seatTurnsGun: the driver's ControlInfo aims with
	// PIMouseLookX / PIMouseLookY, the hover tank's with PIRoll / PIPitch;
	// the Tank, FloatingTank, Mech and ArmedCar sets' Fire is
	// BBFireInfantery, whose plan aims and triggers the driver's weapons
	// as a gunner's, MouseTurretAimAt and Trigger among their interpreter
	// entries).
	bool drivesGun(const GameWorld::PlayerInfo& self) const;
	std::string seatWeaponName(const GameWorld::PlayerInfo& self, int index) const; // -1: the active one
	std::string weaponName(const GameWorld::PlayerInfo& self, int slot) const;
	float projectileSpeed(const std::string& weapon) const;
	// The ControlInfo gains of a player's control object (its seat's
	// aiTemplate, or its soldier's on foot), the plug-in's defaults without one.
	AimGains aimGainsOf(const GameWorld::PlayerInfo& self) const;
	std::string equipmentOf(const GameWorld::PlayerInfo& self) const;
	std::string aiTemplateOf(const std::string& objectTemplate) const;
	WeaponAi weaponAi(const std::string& name) const;
	int strengthTypeOf(const GameWorld::PlayerInfo& player) const;
	int areaOfControlPoint(int controlPoint) const;
	void ensureAreas();

	const ai::NavMesh* m_navMesh = nullptr;
	const ai::NavMesh* m_vehicleNavMesh = nullptr;
	BotSettings* m_source = nullptr;
	std::map<int, ai::Goal> m_goals; // the scripted goals by player
	std::map<int, std::string> m_orders; // ai.botGiveOrder's refused orders, "behaviour set"
	std::map<int, bool> m_deactivated;
	// The mine manager (BotsMines.cpp): the mines by projectile serial, their
	// zones' marks, the 0.5 s clock.
	struct MineZone {
		struct Mark {
			int mesh;
			uint32_t polygon;
			bool counterB;
		};
		int type = 0;
		bool active = false;
		std::vector<Mark> marks;
	};
	std::map<uint32_t, MineZone> m_mines;
	ai::MineMarks m_mineMarks;
	float m_minesNext = 0;
	void updateMines();
	std::set<int> m_refused; // AI players over aiSettings.setMaxNBots (0x100581a0): never activated
	// The sides' knowledge of the enemy players (the AI objects of
	// ai::knowledgeCertainty): when its bots last saw each, and whether one
	// sees it now; filled by sense().
	// Per side and unit (players by id, vehicles by -(object) - 1), the
	// side's AI object (vtbl 0x10118838): +0x48 the last sighting, +0xa8
	// the sensors' count (sensed), +0x54 the position kept while unsensed,
	// +0x20 the last full update (allowedTimeDiff's throttle, 0x100a9e70)
	// and what it refreshed (+0x44's capability: here the health).
	struct Knowledge {
		float lastSeen = -1e9f;
		bool sensed = false;
		physics::Vec3 position;
		bool hasPosition = false;
		float updatedAt = -1e9f;
		float health = 1;
	};
	std::map<int64_t, Knowledge> m_knowledge[3];
	// This tick's players by id and vehicles' teams (from m_players).
	std::map<int, size_t> m_playerIndex;
	std::map<uint32_t, int> m_vehicleTeams;
	const GameWorld::PlayerInfo* playerOf(int id) const;
	static int64_t vehicleKey(uint32_t vehicle) { return -static_cast<int64_t>(vehicle) - 1; }
	// A sighting or hearing reaching the side (slot 3, 0x100a9e70): within
	// allowedTimeDiff of the last full update only the stamp moves.
	void refreshKnowledge(Knowledge& k, float allowedTimeDiff, float health, const physics::Vec3& live, bool force);
	// Hearing: the dispatcher (0x100ca280) and the roll (0x100ec100 ->
	// 0x100ea820).
	void hear();
	float knowledgeCertainty(int team, int64_t key, const std::string& aiTemplate) const;
	void updateKnowledge();
	float degenerationOf(const GameWorld::PlayerInfo& player) const;
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
	std::map<uint32_t, std::string> m_drivenName; // the template, kept past the vehicle's removal
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
	// A xorshift in place of the AI's Mersenne twister (DAT_101394cc:
	// constructor 0x100308a0, destructor 0x100308d0, seeded from the clock
	// x 1e6 through the Park-Miller step 0x10030810 by 0x100309a0, the
	// 624-word refill 0x100308f0).
	uint32_t m_random = 0x2545f491;
	float random(); // 0..1
	void flee(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds);
	void idle(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input);
	// The weapon handling after each execution's plan (AIDLL 0x10099b70):
	// the primary back in hand and its reload.
	std::vector<GameWorld::WeaponAmmo> seatAmmo(const GameWorld::PlayerInfo& self,
		const std::vector<std::pair<int, std::string>>& weapons) const;
	void handleWeapon(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input);
	// The zoom key's press (Bot::zoomPress, set by the plans' statements)
	// on the weapon in hand: a press steps the level (0x005da670; not while
	// reloading), a weapon change, a seat and a reload with zoomOutOnReload
	// (0x005da630) zoom out; the world hears the zoom (setZoomed: the
	// deviation's devModZoom, the walk's speed).
	void stepZoom(Bot& bot, const GameWorld::PlayerInfo& self, float seconds);
	// A Helicopter / TransportHelicopter pilot's plans (BBPGotoWaypoint3d,
	// BBPIdle3d, BBPHeliFire3d) with the flight controllers of src/AI/Heli;
	// false for a behaviour they leave to the others (Change).
	bool fly(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds);
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
// The rest of AIDLL's commands (src/Game/BotsScript.cpp); called by
// registerBotConsoleMethods.
void registerBotScriptMethods(Console& console, BotSettings& settings);

// The game's view distance for the bots (IAIGame vt+0x58, BF2142.exe
// 0x008959f0): the distance into the fog where its density reaches 0.8
// (t from 0 in steps of 0.01 while density(t) < 0.8, density(t) = t x base
// below fog.w, else t^3 + fog.w; start + (end - start) t), at most the
// level's view distance and 300 m.
float gameViewDistance(const std::array<float, 4>& fogStartEndAndBase, float levelViewDistance);

} // namespace dice::hfe::game
