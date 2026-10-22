// The state a ghost update carries: the networkables of a replicated object
// (BF2142.exe "dice.bf2.Networkable.*", registered by 0x00518628 with the class ids at 0x00918d88; each
// class's vtable has the writer in slot 11 and the reader in slot 12, called
// by 0x0057f420 / 0x0057f670).
//
// How a ghost is laid out (0x0067c690, cached per template): the object
// tree is walked in pre-order (0x0067a5e0) and its networkables are
// grouped, identical non-unique ones sharing a group (0x00678070); every
// PlayerControlObject in the tree starts a ghost of its own with the next
// ghost id (0x0067a270). An update holds each group's state in turn (at
// most 12), and the subtree root's networkable is the first group, so the
// root's position and rotation can be read without the rest of the tree.
// Groups after the first are left undecoded.
//
// Which networkable an object gets is its class's (object vtable slot 94,
// e.g. 0x005c77f0): Soldier for soldiers, SimpleObject for most objects
// and for a root PlayerControlObject (a vehicle), PlayerControlObject for a
// PlayerControlObject with a parent, and so on. Players are networkables too
// (the player object +0xc, vtable 0x00966c80); CreatePlayer gives their
// ghost id.
//
// Ghost updates use mode 2 (0x0057fce0 passes 2; mode 3 is the
// control-object record's prediction state). Fields after something the
// reader takes from the client's own copy of the object (whether its
// physics moves, its armor's hit points, its weapon count...) need
// NetworkableHints; without them decoding stops there (`complete` false).
#pragma once

#include "BitStream.h"
#include "GameProtocol.h"

#include <array>
#include <memory>
#include <vector>
#include <cstdint>
#include <string>

namespace dice::hfe::net {

struct Quat {
	float x = 0, y = 0, z = 0, w = 1;
};

enum class NetworkableKind : uint8_t {
	Unknown,
	SimpleObject,             // 0x9858, vtable 0x009403b0: write 0x005ff5d0, read 0x005ffec0
	RotationalBundle,         // 0x9859, 0x00940410: 0x006015a0 / 0x00602070
	FreeCamera,               // 0x985a, 0x00940470
	OverheadCamera,           // 0x9871, 0x009404d0
	Item,                     // 0x985d, 0x00940530
	DestroyableObject,        // 0x9876, 0x00940578: 0x006044e0 / 0x006048c0
	DestroyableWindowsBundle, // 0x9877, 0x009405c0
	Spring,                   // 0x985c, 0x00940608
	PlayerControlObject,      // 0x985f, 0x00940650 (only for a PCO with a parent)
	Kit,                      // 0x985e, 0x00940698: 0x00605f90 / 0x00606030
	GenericProjectile,        // 0x986d, 0x009406f8: 0x006067c0 / 0x00606f20
	ControlPoint,             // 0x9862, 0x00940740: 0x006082b0 / 0x006084b0
	Engine,                   // 0x9863, 0x009407a0
	TargetObject,             // 0x986a, 0x00940800
	MechLegs,                 // 0x9872, 0x00940848
	Soldier,                  // 0x985b, 0x00978430: 0x005fb8c0 / 0x005fcdb0
	Player,                   // the player's networkable, 0x00966c80: 0x00596100 / 0x005981a0
};
const char* networkableKindName(NetworkableKind kind);
// The networkable an object of a template type gets as the root of a ghost
// (provisional for the types the object-class table wasn't traced for).
NetworkableKind networkableKindForTemplateType(const std::string& templateType, bool hasParent = false);

// What the reader takes from the client's copy of the object (its
// template's settings: see ghostTemplateFrom in GhostLayout.h).
struct NetworkableHints {
	NetworkableKind kind = NetworkableKind::Unknown;
	bool mobile = false;       // SimpleObject: a physics node that moves (vtable 0x164; provisional: hasMobilePhysics)
	bool hasArmor = false;     // an armor component (object +0x48)
	int maxHitPoints = 0;      // armor.maxHitPoints, truncated (armor vtable 0x8c)
	int wreckHitPoints = 0;    // armor.wreckHitPoints, truncated (vtable 0x90)
	int soldierItemSlots = -1; // Soldier mask 0x2000: the soldier's item count (template +0x298 slot 4); -1 unknown
	// RotationalBundle: maxSpeed scales the rotation speeds (template +0x2bc).
	Vec3f maxSpeed;
	// Engine (0x00609340): numberOfGears (+0x520) sizes the gear; engineType
	// (+0x36c; 0x20 helicopter, 0x80 newCar2) adds fields; the rest scale.
	int engineGears = 0;
	int engineType = 0;
	Vec3f minRotation, maxRotation;
	float maxAngleOfAttack = 0;
	float minRpm = 0, maxRpm = 0; // newCar2.minRpm / maxRpm
	// Spring (0x00604f10): an extra field when its engine is a newCar2 one
	// and the physics node has flag 8 (provisional: assumed with newCar2).
	bool springExtra = false;
	// PlayerControlObject with a parent (a seat, 0x00605b00): its weapon
	// count (PCO interface +0x2c); 0 carries the occupant soldier's state.
	int seatWeapons = -1;
	// DestroyableWindowsBundle: ObjectTemplate.numWindows (+0x2a4).
	int windows = -1;
	// SimpleObject: a TeamChange component (object +0x5c, mask 0x40); a
	// PlayerControlObject on the parent chain (0x00600560: the vehicle
	// fields), and whether the object is a Parachute.
	bool hasTeamChange = false;
	bool inVehicle = false;
	bool parachute = false;
	// SimpleObject mask 0x800000 and DestroyableObject mask 4: the armored
	// objects below the root (object vtable 0x14c, filled by 0x0060ec00;
	// provisional order: post-order), each with its armor.fullChildSync.
	std::vector<bool> armoredChildren;
	bool armoredChildrenKnown = false;
	// OverheadCamera mask 1: the template's speed (+0x2a0; times a runtime
	// factor at object +0x270, taken as 1).
	float overheadRange = 0;
};

// Player networkable stats (0x005981a0; mask 8 fields go to the player's
// stats, the same struct the scripts read: score, rplScore, deaths, kills, TKs).
struct PlayerNetState {
	bool hasAlive = false;
	bool alive = false;         // mask 1: player vtable 0x70 (provisional meaning)
	int spawnGroup = -1;        // mask 2: 0-255, player +0xe8 (vtable 0x188/0x18c; the spawn code reads it)
	bool hasStats = false;      // mask 8
	int score = 0, deaths = 0, kills = 0, teamwork = 0, teamKills = 0, value1c = 0;
	int ping = -1;              // mask 0x100: 0-1023, player +0xf0 (provisional: ping in ms)
	int team = -1;              // mask 0x200: player vtable 0xf4
	int squad = -1;             // mask 0x1000: 0-15, vtable 0x1a0 (provisional)
	int rank = -1;              // mask 0x800: 0-255 (provisional)
	std::array<uint32_t, 4> raw {}; // a few unnamed fields (provisional)
};

struct NetworkableState {
	NetworkableKind kind = NetworkableKind::Unknown;
	uint32_t mask = 0;
	bool flag = false; // SimpleObject: the bit after the mask (0x00561d90; skips the physics fields)

	bool hasPosition = false;
	Vec3f position;
	bool hasVelocity = false;
	Vec3f velocity;
	bool hasAngularVelocity = false;
	Vec3f angularVelocity;
	bool hasOrientation = false; // SimpleObject, GenericProjectile: a quaternion
	Quat orientation;
	bool hasAngles = false;      // Soldier: degrees
	float yaw = 0, pitch = 0, roll = 0, aimPitch = 0;
	bool hasHealth = false;      // Soldier: a fraction of maxHitPoints; others: hit points
	float health = 0;
	int team = -1;               // ControlPoint mask 1
	int weaponZoom = -1;         // Soldier: the weapon's zoom level 0-3 (DefaultZoomComp +0xc)
	int weaponFireState = -1;    // Soldier: its fire comp's state 0-1 (provisional: fire mode)
	int weapon = -1;             // Soldier mask 0x2000: the active item's index (-1 none)
	float soldierLean[2] {};     // Soldier 0x800, 0x1000: ±50 (+0x264, +0x260)
	float stamina = -1;          // Soldier 0x8000: 0..1 (provisional: sprint)
	int ladder = -1;             // Soldier 0x20000: the ladder's id (0 none)
	float ladderPosition = 0;    // and the position on it, 0..1
	int soldierValues[3] {};     // Soldier 0x20 (+0x268, +0x26c, 0..4), 0x4000 (+0x284)
	uint32_t soldierBits = 0;    // Soldier: bit 0 armor +0x21c, bits 1-3 +0x328, +0x30c, +0x3e9
	size_t ragdollBits = 0;      // Soldier 0x10000: dead, a ragdoll's state (its first particle in `position`)
	int empLevel = -1;           // SimpleObject 0x20, Soldier 0x80: the armor's EMP level 0-15
	int changePoints = -1;       // SimpleObject 0x40: TeamChange changePoints (rounded up)
	int spawnTeam = -1;          // SimpleObject 0x1000: object +0x168 (provisional: its spawner's team)
	bool sleeping = false;       // SimpleObject 0x200: the physics node sleeps
	bool supplyFlag = false;     // SimpleObject 0x20000: SupplyObject +0x198
	bool responseFlag = false;   // SimpleObject 0x1000000, Soldier: ResponsePhysics
	int parachuteState = -1;     // SimpleObject 0x8000 on a Parachute: 0-7
	int vehicleValue = -1;       // SimpleObject 0x40000, Soldier 0x100000: PCO +0x190 (0-15)
	bool vehicleActive = false;  // SimpleObject 0x100000: PCO +0x18c (provisional: engine on)
	bool vehicleFlag = false;    // SimpleObject 0x200000: PCO +0x2be
	std::vector<int> childDamage; // SimpleObject 0x800000, DestroyableObject 4: per armored child, eighths of its hit points (8 intact, 0 wrecked)

	// ControlPoint (0x006084b0; the interface at cp +0x184). `team` is mask 1.
	bool cpFlag = false;         // mask 2: cp +0x1a0 (provisional: contested)
	float cpValues[2] {};        // mask 4: the flag's height (capture progress 0..1, cp +0x19c) and its rate per second (+0x194, vtable 0x4c); ±1.25 in 16 bits
	bool cpTeam1Flag = false, cpTeam2Flag = false; // masks 8, 0x10: cp +0x1b4 / +0x1b5 (provisional: capturable by team 1 / 2)
	int cpValue = 0;             // mask 0x20: whose flag is raised, 0-2 (cp +0x1b0)

	PlayerNetState player;

	// RotationalBundle (0x00602070): rotation yaw/pitch/roll in degrees
	// (masks 1, 2, 4; object +0x188), rotation speeds (8, 0x10, 0x20; +0x194),
	// 0x40 a value of ±41 (+0x1b0; provisional: the remembered excess
	// input), and with a PlayerControlObject above it (object vtable 0xd8)
	// the ignore-parent rotation: angles (0x200-0x800) and a quaternion (0x100).
	float rotation[3] {};
	float rotationSpeed[3] {};
	float excessInput = 0;
	float parentRotation[3] {};
	Quat parentOrientation;

	// Engine (0x00609340).
	int gear = -1;               // mask 2 (object +0x280)
	float engineRotation[2] {};  // masks 8, 4: the rotation speed x, z (vtable 0x184)
	float engineTarget = 0;      // mask 0x10 (vtable 0x18c, z)
	float engineInputs[3] {};    // masks 0x20, 0x40, 0x80: ±1.25 (+0x25c, +0x26c, +0x260; provisional: throttle and inputs)
	float angleOfAttack = 0;     // mask 0x100, helicopters (+0x270)
	int engineDirection = 0;     // mask 0x200: -1, 0, 1 (+0x29c)
	float rpm = 0;               // mask 0x400, newCar2: signed rpm
	float clutch = 0;            // its 8-bit 0..1 (+0x27c)
	int carGear = 0;             // and the gear (vtable 0x1b0)

	// Spring (0x00605320): the offset (vtable 0xe0) and velocity, 2 mm steps.
	bool springPresent = false;
	Vec3f springOffset, springVelocity;
	float springExtra = 0;       // newCar2: ±256 (physics +0x27c)

	// MechLegs (0x0060a100; provisional meanings).
	int legState = -1;           // mask 1: 4 bits (+0x1bc)
	bool legFlag1 = false, legFlag2 = false; // masks 2 (+0x1c0), 4 (+0x19c)

	// TargetObject (0x00609d50): position (1), active (2), a byte (4; +0x194).
	bool targetActive = false;
	int targetValue = 0;

	// Item (0x00604120): the item interface's value (2; +8), active (8;
	// vtable 0x28/0x2c), the unlock (0x10; +0x198 with unlockBranch).
	int itemValue = 0;
	bool itemActive = false;
	int itemUnlock = 0;

	// FreeCamera (0x00602c50) and OverheadCamera (0x00603980).
	int cameraValue = 0;         // FreeCamera 4 (+0x220), OverheadCamera 4 (+0x29c)
	float cameraPan[2] {};       // OverheadCamera 1 (provisional: pan speed)
	bool cameraFlag = false;     // OverheadCamera 8 (a global, 0x00a032ac)

	// DestroyableWindowsBundle (0x00604e00): one bit per window (provisional: set = intact).
	uint32_t windowBits = 0;

	// DestroyableObject (0x006048c0): mask 2's value 0..100 (armor vtable 0xe8).
	float destroyValue = 0;

	// PlayerControlObject with a parent (0x00605b00). A seat with weapons:
	// the active weapon of groups 0-2 (masks 1, 2, 4; -1 none; a vehicle
	// root's SimpleObject has them at 0x2000, 0x4000, 0x80000) and a flag
	// (8; vtable 0x12c, object +0x310; provisional: the alternate mode).
	// Without: the occupant soldier's state (an 11-bit length, then the
	// Soldier networkable's bits), decoded into `occupant` when it can be.
	int seatWeapon[3] {-1, -1, -1};
	bool seatFlag = false;
	size_t occupantBits = 0;
	std::shared_ptr<NetworkableState> occupant;

	bool complete = false;       // read to the end of this networkable's state
};

// Reads the first group of a ghost update for a networkable of `hints.kind`.
// `reference` is the stream's reference position (the control-object
// record's position in this ghost block, else the origin).
bool decodeNetworkable(BitReader& in, const NetworkableHints& hints, const Vec3f& reference, NetworkableState& out);
// Writes one as the original writer does for the fields NetworkableState
// holds (others present in the mask are written as zero), for tests and the
// loopback server. `reference` nullptr: positions go as raw floats.
void encodeNetworkable(BitWriter& out, const NetworkableHints& hints, const Vec3f* reference, const NetworkableState& state);

// --- the quantizations, shared with the tests

// 2-bit mode, then three floats or three signed values of the table's bits
// times `scale` from `reference` (0x006876c0 / 0x00687510: 20, 16, 12 bits;
// 0x00687920 / 0x00687770: 12, 10, 8 bits).
enum class DeltaTable { Wide, Narrow };
Vec3f readDelta(BitReader& in, DeltaTable table, float scale, const Vec3f& reference);
void writeDelta(BitWriter& out, DeltaTable table, float scale, const Vec3f& value, const Vec3f* reference);
// 3-bit mode (0x00687c20 / 0x006879d0): 0 floats, 1-6 values of 24, 20, 16,
// 12, 10 or 8 bits, 7 the reference itself.
Vec3f readDelta3(BitReader& in, float scale, const Vec3f& reference);
void writeDelta3(BitWriter& out, float scale, const Vec3f& value, const Vec3f& reference);
// A unit quaternion as w, x, y, z of `bits` each ((v + 1) / 2 in 0..1,
// truncated; 0x00687dc0 / 0x00687f40), normalised when read.
Quat readQuaternion(BitReader& in, unsigned bits);
void writeQuaternion(BitWriter& out, const Quat& q, unsigned bits);

} // namespace dice::hfe::net
