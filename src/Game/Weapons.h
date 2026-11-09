// Firearms and their projectiles (GenericFireArm and GenericProjectile
// templates with their fire and ammo components).
//
// A firearm fires a round every 1800 / roundsPerMinute ticks while the fire
// input is held (SingleFireComp, defaults in 0x005f2ad0: 600 rounds per
// minute), from a magazine of magSize rounds; an empty magazine is replaced
// after reloadTime seconds while magazines last (DefaultAmmoComp, defaults
// in 0x005f4340: 30 rounds, 5 magazines, 2 s; nrOfMags -1 never runs out). Projectiles leave at the
// firearm's velocity and fly with point physics (node type 1: no damping,
// gravity times gravityModifier) until they hit something or their time to
// live runs out. Their damage falls off linearly from
// distToStartLoseDamage to distToMinDamage (0x005b2f70).
#pragma once

#include "Physics/RigidBody.h"
#include "Random.h"

#include <functional>
#include <array>
#include <string>
#include <utility>
#include <vector>

namespace dice::hfe::physics {
struct Material;
}

namespace dice::hfe::world {
class ObjectTemplate;
}

namespace dice::hfe::game {


struct ProjectileSettings {
	std::string name; // the projectile's template (its S_<name>_Looping flight sound)
	float damage = 0;
	// hitImpactModifier (GenericProjectile template +0x338, 1 by default;
	// executor 0x004f1292 -> 0x007e6c10; the round's interface 0x94c1 slot
	// 65 0x005b3170): handed to the soldier it hits (SoldierResponse
	// 0x006c1df0 -> armour slot 0xe8), it scales the death ragdoll's hit
	// push for a tick (GameWorld::startRagDoll).
	float hitImpactModifier = 1;
	// activeDefenseEnabled (+0x336, 25 shipped: the AA, AV and TV-guided
	// missiles and some guns' rounds): it goes off on an active defence's
	// shield (0x006c07a0, see stepProjectiles).
	bool activeDefenseEnabled = false;
	float minDamage = -1;
	float distToStartLoseDamage = 0;
	float distToMinDamage = 0;
	RandomDistribution timeToLive {RandomDistribution::Type::None, 1, 0, 0};
	float gravityModifier = 1;
	// Its Point / RotationalPoint node's drag and mass (template +0x78,
	// +0x94; 0 and 1 by default): shotgun pellets 18, C4 15, missiles 0.1-10.
	float drag = 0;
	float mass = 1;
	int material = 0;
	// Tracers (the GenericProjectile template 0x005b3500; saved by
	// 0x005b5850): tracerTemplate (+0x2d0, resolved at +0x2ec; its
	// geometry: the tracer's mesh), tracerInterval (+0x2f0, 1),
	// tracerScaler (+0x2c0, 0), tracerSizeModifier (+0x2c4, 1),
	// tracerConvergeDistance (+0x2f4, 100), maxTracerScaler (+0x2b8, 0.1)
	// and minTracerScaler (+0x2bc, 5; read by nothing). With a tracer
	// template the firing (0x005f2bb0) makes every tracerInterval-th round a
	// tracer round: a second projectile of the same template flagged a
	// tracer (projectile slot 0xb8, 0x005b32b0: its geometry the tracer
	// template's) tied to the round (slot 0xbc, +0x248). See TracerLook.
	std::string tracerTemplate;
	std::string tracerGeometry; // the tracer template's geometry (filled by the game world)
	int tracerInterval = 1;
	float tracerScaler = 0, tracerSizeModifier = 1, tracerConvergeDistance = 100, maxTracerScaler = 0.1f;
	// Detonation (detonation.*): splash damage round the impact. With a
	// DefaultDetonationComp the template's defaults (0x005e9df0) are
	// damage 500, radius 10, force 150, yMod 1 and material -1 (no blast:
	// 0x005a9090 and 0x005a7ab0 skip everything for material -1).
	bool detonates = false;
	float explosionDamage = 0;
	float explosionRadius = 0;
	int explosionMaterial = 0;
	float explosionForce = 0;
	float explosionYMod = 1; // the height difference counts times this
	std::string endEffect; // detonation.endEffectTemplate
	// collision.bouncing: it bounces off what it hits (grenades, mines, C4)
	// and detonates when its time to live runs out, not on impact.
	bool bounces = false;
	// StickyCollisionComp (component 0xc54b, its template's constructor
	// 0x005e8820; the response 0x005e83d0; 11 shipped: C4, the sonars, the
	// decoy, the mines, the explosive shotgun round, the hubs, the shield
	// and the beacon): a bouncing round that meets a face it may stick to
	// stays there, riding the vehicle it hit (see GameWorld::stickProjectile).
	// collision.maxStickAngle (+0x10, executor 0x004f27f1) is kept as the
	// cosine of the angle clamped to +-180 (0x005e8030; default -1: any
	// face); stickToTerrain (+0x14), stickToStaticObjects (+0x15) and
	// stickToVehicles (+0x16) are 1 by default (shipped 0 on the mines,
	// decoy, hubs, shield and beacon).
	bool sticky = false;
	float maxStickCos = -1;
	bool stickToTerrain = true, stickToStaticObjects = true, stickToVehicles = true;
	// The Ability component's radar (template class 0xc4b2, its
	// constructor 0x00631790; read through the runtime component
	// 0x00656640's block, 0x00658590): radarRadius (+0x18, 0), radialRadar
	// (+0x15, 1), showPlayers (+0x13, 1), showVehicles (+0x14, 1). A
	// projectile with a radius is a radar source (0x00656550 -> the set at
	// 0x00a79ff8) the minimap rings for its team (GameWorld::radarSources).
	float radarRadius = 0;
	bool radialRadar = true, radarShowPlayers = true, radarShowVehicles = true;
	bool damageMandown = false; // damageMandownSoldiers: it reaches downed soldiers (the defibrillator)
	// ResurrectCollisionComp (template 0x005e7d20): collision.resurrectDamage
	// (default 0) and collision.restoreHP (default 0.1), handed to the game
	// logic's revive 0x004c1960 by 0x005e7a70.
	bool resurrect = false;
	float resurrectDamage = 0;
	float restoreHitPoints = 0.1f;
	bool emp = false;           // detonation.isEMPWeapon: shuts vehicles down instead
	// Seeking (seek.*, follow.*): TTMagnet locks onto the closest enemy
	// vehicle within maxDistLock after trackingDelay and chases it (motion
	// mines); TTUnique follows the shooter's aim (the AV missile), turning
	// at most maxYaw/maxPitch degrees a tick (followStep).
	// TTBeacon (the missile silos' msilo_missile, FixedTargetComp): chases
	// the target object its launcher locked (the silo's TitanBeacon on the
	// enemy Titan) after trackingDelay, by the magnet's follow.
	// Closest: SeekClosestTargetComp (template 0x9540, ctor 0x005ec300) of
	// TTHeat / TTLaser rounds, see GameWorld::steerClosest.
	enum class Seek { None, Magnet, Aim, Beacon, Closest };
	int seekTargetType = -1;  // seek.targetType's index (TTNone 0 .. TTPod 7)
	int reLockTicks = 0;      // seek.reLockTime (+0x28) x 30, rounded (+0x2c)
	Seek seek = Seek::None;
	float seekDelay = 0;
	float lockDistance = 0;
	float loseDistance = 0;
	float maxYawRate = 0, maxPitchRate = 0;
	// DefaultFollowComp's time limit (template +0x30, setter 0x004f6496,
	// default 0 = none; read by the follow's step 0x005ed1a0): the follow
	// turns the round only until followTime seconds after its start, the
	// lock taken from none plus trackingDelay (followFrom). The AA
	// missiles' 10, the AG's 2.5, the mechs' 6 and 0.35. targetActivation
	// (+0x34, setter 0x004f5fba, read by 0x005ec7c0) starts the chase at
	// that lock (0x005b2c10) and stops it when the target goes (0x005b2ca0)
	// instead of at the launch (0x005b3990); both only switch the round's
	// child effects (events 0x44 / 0 and 1 / 0x45), since without a target
	// the follow doesn't turn it either way.
	float followTime = 0;
	bool targetActivation = false;
	// The magnet's chase (GameWorld::steerSeeker): armingDelay (template
	// +0x2fc), follow.armingSpeed, speedModifier, predictionFactor,
	// useTargetSpeed, magneticBehaviour; the projectile's acceleration
	// (+0x31c), deAcceleration and maxSpeed; seek.maxAngleLock (degrees)
	// and ignoreFriendly.
	float armingDelay = 0;
	float armingSpeed = 0, speedModifier = 1, predictionFactor = 0;
	bool useTargetSpeed = false, magneticBehaviour = false;
	float acceleration = 0, deAcceleration = 0, maxSpeed = 0;
	float maxAngleLock = 360;
	bool ignoreFriendly = true;
	// seek.directionBonus (the seek template's +0x30, executor 0x004f541e,
	// 0 by default; the AA missiles' 0.8): the candidate query
	// (0x004cf290) takes the target of the least angle x bonus x 500 +
	// (1 - bonus) x distance (the angle off the round's heading in radians),
	// not simply the nearest. The launchers' lock asks with 0 (0x005e1b30).
	float directionBonus = 0;
	// The engine (0x005b3e60 from 0x005b4370): startDelay (template +0x300)
	// seconds after the launch (0x005b3990 schedules event 0, 0x005b62a0 ->
	// 0x005b2c10 sets the round's +0x25f; at once without a delay), or with
	// follow.targetActivation the lock (0x005b2c10), the round is pushed by
	// acceleration (+0x31c) a second while under maxSpeed (+0x324): along
	// the follow's new heading (its slot 0x40, 0x005ed120) on a tick its
	// update (0x005ed1a0) steered it or is past followTime, else, with
	// hasDirection (+0x335), along its nose; otherwise not at all. A
	// magnet's follow pushes it itself (followMagnet). hasFollow: a
	// DefaultFollowComp.
	float startDelay = 0;
	bool hasDirection = false;
	bool hasFollow = false;
	// onlyDetonateOnMaterial (template +0x348, -1 none) for the first
	// onlyDetonateOnMaterialTimeLimit seconds (+0x34c; 0: always): until
	// then it sets off only on that material, and anything else it meets
	// that is no Titan the server destroys (3e10 damage) while it flies on
	// (0x005b4ff0).
	// detonateDelay (template +0x33c, 0) and dudMaterial (+0x340, -1;
	// executors 0x004f13bd / 0x004f14da): a hit within detonateDelay
	// seconds of the round's start (0x005b4ff0) is a dud: handed on with
	// dudMaterial for the round's own (its damage and effect that cell's),
	// and the round is then removed without going off (0x005b54e0: the
	// game logic's slot 0xa4). Shipped: the AT minigun rounds 0.001 (40),
	// static_aa_cannon 0.01 (29), the under-rifle rocket eu_rifle_missile
	// 0.5 (30).
	float detonateDelay = 0;
	int dudMaterial = -1;
	int onlyDetonateOnMaterial = -1;
	float onlyDetonateOnMaterialTime = 0;
	// DefaultPenetrateComp (class 0x953e, cached at the projectile's
	// +0x1ac by 0x005b2b20; 111 of the 114 shipped rounds create it):
	// allowSolidPenetration (+0xc) and allowLiquidPenetration (+0xd), both 1
	// by default (executors 0x004f2c86 / 0x004f2db8). See penetrates().
	bool penetrator = false;
	bool allowSolidPenetration = true;
	bool allowLiquidPenetration = true;
	// Mines: an enemy inside triggerRadius and triggerAngle (degrees either
	// side of the mine's facing) moving faster than triggerVictimMinSpeed
	// sets it off; the blast reaches explosionConeAngle either side of the
	// facing (0: all round).
	float triggerRadius = 0;
	float triggerAngle = 0; // +0x78: 0 or less the box form (GameWorld's radius trigger)
	float triggerMinSpeed = 0;
	// detonation.radiusDetonateWithAirVehicles (+0x5c): the box form also
	// for an object touching nothing (all three shipped vehicle mines).
	bool radiusDetonateWithAirVehicles = false;
	float coneAngle = 0;
	// detonation.triggerType (template +0x64, RadiusTriggerType): what sets
	// it off (0x005e8aa0): MTYNone 0 nothing (the default), MTYPersonnel 1
	// soldiers, MTYVehicle 2 anything but soldiers, MTYPco 3 both. Shipped:
	// the anti-personnel mine MTYPco, the EMP and motion mines MTYVehicle.
	int triggerType = 0;
	// detonation.allowFriendlyFireDetonation (+0x7c): see GameWorld's
	// mines (with the server's friendlyFireWithMines).
	bool allowFriendlyFireDetonation = false;
	// No trigger and no fuse to speak of (C4): set off by its owner.
	bool remote = false;
	// hasOnTimeEffect (template +0x328, 1 by default, 0x005b3500): its time
	// run out (events 0x18 / 0x1d), the round detonates (event 0x1f, the
	// detonation's slot 0x14), else it is only removed (0x20; 0x005b62a0).
	bool onTimeEffect = true;
	// Where a hit round goes off (0x005e9300): deeper under water than
	// detonation.maxDepthForExplosion (template +0x48, 2 by default,
	// 0x005e9df0; the node's depth, slot 0xd8) nothing happens; the end
	// effect stands on the face's normal with useCollisionNormal (+0x69, 1),
	// else along the round's forward (its matrix's z row) (0x005e8f50).
	float maxDepthForExplosion = 2;
	bool useCollisionNormal = true;
	// detonation.detonateDistanceToTarget (+0x50, executor 0x004f381d, 0 by
	// default; shipped 3 on eu_ifv_laserguided_mine): the detonation
	// component's update (0x005e9f60), every detonationTriggerFreq, sets the
	// round off (event 0x1f) while its target (+0x10: the seek's, handed on
	// each update by 0x005b3e60 through slot 9 0x005e9280) lies nearer
	// than this, before the radius trigger's test.
	float detonateDistanceToTarget = 0;
	// centerOfCollisionOffset (SimpleObject +0xa4, executor 0x0051e654) of a
	// RotationalPoint round: its node's contacts test the point the offset
	// from its origin (the ground's 0x006c0f90 and the objects' 0x006c1220
	// through the node's slot 0x124), so it comes to rest that far the
	// other way: the frag grenade (0/-0.05/0) 5 cm, the IFV's mine
	// (0/-0.12/0) 12 cm above where its origin would lie. Other node types
	// don't read it (Point, Basic: the decoy and EMP mine are Mesh).
	physics::Vec3 collisionOffset;
	// A ForceObject child that holds the round over the ground (the IFV's
	// mine, eu_ifv_laserguided_mine_FO; template defaults 0x0062d4f0; the
	// object's tick 0x0062c920 -> 0x0062b750; GameWorld::hoverProjectile).
	// enableOnStartMessage (+0x296): off until the round's start message
	// (event 0 to its children: 0x005b3990 at the launch, or with
	// follow.targetActivation 0x005b2c10 at the lock; its handler
	// 0x0062ab40), off again at the stop (event 1, 0x005b2ca0).
	// targetPosition (+0x288) y over what lies under it, found by a ray down
	// targetPosition y + regulationTolerance (+0x29c, 0.5) long with
	// relativeYCollisionGroupInverse (+0x298) other than 0 (1: the terrain
	// alone, any height under it); regulatePosY (a, b) the regulator.
	struct Hover {
		bool present = false;
		bool enableOnStartMessage = false;
		float height = 0;     // targetPosition y
		float tolerance = 0.5f;
		float a = 0, b = 0;   // regulatePosY
		unsigned relativeY = 0;
	} hover;

	static ProjectileSettings fromTemplate(const world::ObjectTemplate& projectile);
	// The damage after travelling `distance` metres.
	float damageAt(float distance) const;
};

// A blast's falloff with distance (0x005a61a0): 1 within 0.2 of the
// radius (0x00a6aab4), then falling linearly to 0 at the radius.
float explosionFalloff(float radius, float distance);
// A blast's push on an object (0x005a66e0), before it is spread over the
// distance: `line` runs from the blast to the object's position (its height
// already times explosionYMod), `axes` are the object's rotation (rows: its
// x, y and z). The force is falloff x forceMod x explosionForce x share
// (the armour's explosionForceMod, or wreckExplosionForceMod for a wreck;
// without armour 10000 / max(mass, 100)), times 0.1 for a soldier whose
// node is under water (slot 0x144, +0x5c, above 0), at most forceMax
// (explosionForceMax, wreckExplosionForceMax; 500 without armour). It points
// along the object's axis nearest the line (y only when strictly nearest,
// else x unless z is nearer), signed towards the object, plus the line made
// a unit; a soldier's line gets 5 more upwards (*0x00a6ab78) and its force
// times `soldierRandom` (0.8 + 0.005 x (rand % 100)). The object's speed
// counts against forceMax; components under 0.001 are dropped (0x005a6310).
// The caller applies it when at least 0.01 long.
// A blast whose projectile has a detonation component (0xc54d, the
// projectile's slot 0x50) with explosionDamage below 0 (its slot 0x4c: the
// template's +0x44) is `twist` (0x005a66e0's bVar5): the force is 20 x
// explosionForce x share (no falloff, no forceMod), it points along the
// nearest axis alone (the line is not added), and the caller gives it as
// the torque pair (blastTwist). No shipped detonation has a negative
// explosionDamage.
struct BlastPush {
	physics::Vec3 line;
	physics::Mat3 axes;
	float falloff = 1;
	float forceMod = 1, forceMax = 10;
	float explosionForce = 0;
	float share = 1;
	bool soldier = false;
	float soldierRandom = 1;
	bool inWater = false;
	float speed = 0;
	bool twist = false;
};
physics::Vec3 blastPush(const BlastPush& blast);
// The push's strength (blastPush's length before the small components are
// dropped).
float blastPushForce(const BlastPush& blast);
// The torque pair of a `twist` blast (0x005a66e0's end, through node slot
// 0xa4 three times, not spread over the distance): with a the object's x
// axis (axes row 0) turned to the blast's side (its negative when the line
// from the blast runs along +x, 0x00562dd0) and f = force / 2.9
// (*0x00a6aa94) x max(the object's box width across x, 2) (slot 0x9c's
// +0x24, *0x00a6aa98): (0, f, 0) at a, (0, -0.5 f, 0) at -a (*0x00a6aa8c)
// and a x f x -0.5 at the centre (*0x00a6aa90, the point 0x006ac600).
struct BlastForce {
	physics::Vec3 at;    // relative to the object's position
	physics::Vec3 force;
};
std::array<BlastForce, 3> blastTwist(const physics::Mat3& axes, const physics::Vec3& line, float force, float width);
// An armour's EMP level after `amount` of EMP damage (0x00652980): it goes
// up in whole steps of `stepSize`, at most `numSteps`.
float addEmpDamage(float level, float amount, float stepSize, int numSteps);
// The candidate query's cost (0x004cf290): angle (radians off the
// heading) x bonus x 500 + (1 - bonus) x distance; the least below 9999
// wins (seek.directionBonus).
float seekCost(float angle, float distance, float bonus);
// A projectile's hovering ForceObject child (ProjectileSettings::Hover)
// from its template.
ProjectileSettings::Hover projectileHover(const world::ObjectTemplate& forceObject);
// The hover's vertical step (0x0062b750's regulated y through the node's
// slot 0xa0, x 30 / mass; the regulator 0x00667710: 2a v + (a^2 + b^2)
// (goal - y)): the round's new vertical velocity after `seconds`.
float hoverVelocity(const ProjectileSettings::Hover& hover, float y, float vy, float goal, float seconds);

// fire.maximumVehicleTiltForFire (0x005f1780, the press of every fire
// component): 0 lets any tilt through; otherwise the root object's up axis
// (`root`'s second row) must lie less than that many degrees off the
// world's (acos of its y, clamped).
bool tiltAllowsFire(const physics::Mat3& root, float maxTilt);

struct FireArmSettings {
	int roundsPerMinute = 600;
	int magSize = 30;
	int nrOfMags = 5;
	float reloadTime = 2;
	// The muzzle speed (ObjectTemplate.velocity, template +0x2f0): 600 m/s
	// unless set (the GenericFireArm template's constructor 0x005c2800);
	// the SMGs and handguns don't set it.
	float velocity = 600;
	std::string projectileTemplate;
	// Heat (guns without magazines): each round adds heatAddWhenFire, it
	// cools by coolDownPerSec, and at 1 the gun stops for overheatPenalty
	// seconds.
	float heatAddWhenFire = 0;
	float coolDownPerSec = 0;
	float overheatPenalty = 0;
	// The fire modes (fire.addFireRate, in order; the template's list at
	// +0x30): 0 single, 1 burst, 2 automatic. The fire component starts in
	// the first (2 without one, 0x005f28a0). A burst is fire.burstSize
	// rounds (template +0x4c, 3 by default, 0x005f2ad0).
	std::vector<int> fireRates;
	int burstSize = 3;
	// A PulseMeterAmmoComp (the handheld pulse meter): each round starts its
	// scan (0x005d9cf0 sets +0x58), which runs pulse-meter-scan-step (1, a
	// tweakable of 0x008e..: DAT_00a6e194) a second until it is through
	// (0x005d99e0, then both cleared): the HUD's HudScanProgress through
	// the ammo component's 0xc4f2 slots 0xf8 / 0xfc (0x007be380).
	bool pulseScan = false;
	// ammo.reloadWithoutPlayer (the ammo template's +0x41): a gun not held
	// by a soldier (a vehicle's, a stationary one) finishes a reload
	// begun while nobody uses it (0x005f4bb0); without it the reload is
	// dropped when its user leaves (0x005f3a40: +0x3c, +0x34, +0x20 cleared).
	bool reloadWithoutPlayer = true; // 1 by default (the ammo template's constructor 0x005f4340)
	// delayToUse (the GenericFireArm template's +0x2b8; its setter
	// 0x005c2270 keeps round(x 30) ticks at +0x2bc): the firearm's enable
	// (0x005c47d0, interface slot 105) sets its ready tick (+0x208) that far
	// on; until then the busy test (0x005c14f0) holds: no round
	// (SingleFireComp 0x005d4800), no fire input to the fire components
	// (0x005c50e0), no reload (the ammo's update 0x005f3c40 via 0x005c26a0).
	int delayToUseTicks = 0;
	// ammo.autoReload (+0x28) and ammo.instantReloadOnEnable (+0x29): the
	// ammo's on-enable (0x005f4bb0, slot 0x60) with both and an empty
	// magazine that can be reloaded (0x005f4140) fills it at once
	// (0x005f46f0): a magazine taken, no reload time.
	bool autoReload = false;
	bool instantReloadOnEnable = false;
	// ammo.toggleWhenNoAmmo (+0x40): with it, an empty magazine that can't
	// be reloaded makes the weapon unselectable (0x005f39c0, slot 0x1c,
	// asked by the firearm's 0x005c1150) and, after its last round, its
	// soldier switches away (0x005f4a40 -> firearm slot 0xe0, the pick
	// 0x005883d0: the item before).
	bool toggleWhenNoAmmo = false;
	// ammo.changeMagAt (+0x24, 0.7 by default): the reload start (0x005f4830)
	// puts the magazine's swap at round(reloadTicks x changeMagAt) and the
	// end at reloadTicks; the update (0x005f3c40) fills at the swap and is
	// busy until the end.
	float changeMagAt = 0.7f;
	// fire.busyUntilButtonRelease (SingleFireComp +0x9c, setter 0x004e3f33):
	// see FireArm::fire.
	bool busyUntilButtonRelease = false;
	// fire.fireStartDelay (the fire template's RandomDistribution at
	// +0x08, its value +0x0c; 0 by default): a spin-up before the first
	// round of a press. SingleFireComp / SpawnObjectFireComp (slot 36,
	// 0x005d4d80) and ExplosivesFireComp (0x005e6bc0) on a fresh press
	// (+0x6c, the press 0x005f1370) at the burst's first round (+0x24 0)
	// with a value other than 0 start it (+0x60; 0x005f1650: round(30 x
	// max(0, sample)) ticks) and fire nothing; the update (0x005f18b0, slot
	// 5) lets the round go once the ticks are past; a release (slot 12,
	// 0x005f1630) drops it. MultiFireComp's slot 36 (0x005d6e10) never
	// starts it (the heavy mech's minigun's 0.5 does nothing). Shipped:
	// EU_AAcannon_Firearm_Barrel 0.6, the AT's miniguns 0.5,
	// US_AG_Cannon_fireArm 0.227.
	RandomDistribution fireStartDelay;
	bool multiFire = false; // a MultiFireComp
	// fire.maximumVehicleTiltForFire (the fire template's +0x7c, 0 none):
	// the press (slot 13 of each fire component, 0x005f1780) is refused
	// while the root object's up axis is that many degrees or more off the
	// world's (the pod launchers: 30, the air transports' 70); a thrown
	// one's second click of a double click isn't tested (0x005e4d80).
	float maximumVehicleTiltForFire = 0;
	// keepProjectiles (the firearm template's +0x318, 5 by default,
	// 0x005c2800): when the firearm goes, its rounds still in the world
	// (its list +0x18) are removed this many seconds later (0x005f1cc0:
	// the object manager's slot 0xd0 with 0x20, a projectile's removal
	// without its effect, else 0x21): the mines, hubs, sonars and
	// beacons a soldier left (30 or 60 s for those, 0 for the AP mine).
	float keepProjectiles = 5;
	// A ThrownFireComp (the grenades, mines, hubs, sonars, the pod
	// launchers): a round decided (slot 36, 0x005e5a10: state 2 from now,
	// +0x15c) waits fire.pullbackTime (template +0x94, ticks +0x98, 0.2 s
	// by default; the setter 0x005e4c40) and then fire.fireLaunchDelay
	// (+0x1c, ticks +0x20: state 3, +0x160) before it leaves (state 4; the
	// update, slot 5 0x005e4c80). Ticks are the times x 30, rounded.
	bool thrown = false;
	int pullbackTicks = 0, launchDelayTicks = 0;
	// The underarm (soft) throw: a ThrownFireComp with fire.altFireInput
	// (PIAltFire on the grenades): the alternate press (0x005e4d80, slot 13)
	// starts the throw with the soft flag (+0x178), under which the
	// pull-back / launch-delay timer (0x005e4c80) waits; while it is held
	// the power (+0x168) is min(1, ticks held / 30) (0x005e4eb0, the
	// constructor's 1/30 at +0x90); the release lets the timer run and the
	// round leaves at the weapon's speed x the power (0x005e5720). A
	// normal throw's power is 1 (0x005e4b40).
	bool softThrow = false;
	// fire.showWeaponAfterReloadDelay (ThrownFireComp +0xa4, ticks +0xa8:
	// round(s x 30), the setter 0x005e4c00, executor 0x004e4d1e; 0 by
	// default, constructor 0x005e5b10): the launch (0x005e5720's end)
	// hides the firearm (object flag 2 cleared, 0x006f68f0); the reload
	// start (0x005c0f40 -> the component's slot 18, 0x005e49d0) notes the
	// time (+0x164, +0x169) and the update (slot 6, 0x005e5610) shows it
	// again once that many ticks have passed (FireArm::hidden). Shipped
	// 0.05 (frag, EMP, the sonars, the hubs) and 0.2 (smoke, the radar
	// grenade, the motion mine bait). fire.fireLaunchDelaySoft (+0x9c,
	// ticks +0xa0, 0x005e64b0; 0.8 on five grenades) delays no round: its
	// only reader (slot 11, 0x005e4e70) adds max(fireLaunchDelay, soft) +
	// this to the busy time a reload start sets (0x005c2da0), which the
	// rewrite's reload doesn't model.
	int showAfterReloadTicks = 0;
	// ammo.allowReloadWithFullMag (+0x43, executor 0x004e6b77): canReload
	// (0x005f4140) lets a full magazine be reloaded (the missile silos).
	bool allowReloadWithFullMag = false;
	// ammo.magLinkWeapon (+0x48, executor 0x004e6528): the sibling firearm
	// (the parent's children, 0x005f3aa0, by name without case) whose
	// magazine this one shares: each round used (0x005f45e0) sets its
	// rounds in the magazine to ours (slot 0x50), a reload or added ammo
	// (0x005f46f0, 0x005f4ef0) its magazines too (slot 0xac, 0x005c4900).
	// The under-rifle shotgun and rocket link each other (not the rifle).
	std::string magLinkWeapon;
	// fire.allowZoomOnROF (the fire template's +0xb4, executor 0x004e44d3;
	// shipped 1 on the three sniper rifles and static_av): the fire
	// component's busy query (slot 27, 0x005d3c40), which the firearm's
	// event handler (0x005c50e0) asks before it takes any press, holds while
	// the component is between rounds (its state, slot 0xd8 +0x150, above 1:
	// the spin-up 3, a round's cycle 4) unless this is set; with it the
	// component's own press (slot 13, 0x005d3b00) refuses instead, so only
	// the fire press waits and the zoom's press goes through
	// (FireArm::zoomBusy).
	bool allowZoomOnROF = false;
	// A ReplenishingAmmoComp (the hubs, the repair tool, the defibrillator)
	// and its ammo.abilityCost (+0x78, executor 0x004e6efb, 0 by default):
	// each round taken (slot 0x38, 0x005d81d0) costs the holder's battery
	// rounds x abilityCost (soldier component 0xc4d7 slot 0x98, through
	// 0x005d7fe0; nothing for a magazine without end, rounds -1), and a
	// magazine can be taken (0x005d79a0, in place of the default ammo's
	// 0x005f4140) only into an empty magazine while the battery holds at
	// least abilityCost: a hub (cost 1) comes back with a full battery.
	// Shipped: the ammo and medic hubs 1, the repair tool 0.3 (its
	// magazine has no end), the defibrillator 0.
	bool replenishing = false;
	float abilityCost = 0;

	static FireArmSettings fromTemplate(const world::ObjectTemplate& firearm);
	int ticksBetweenShots() const { return roundsPerMinute > 0 ? 1800 / roundsPerMinute : 1; }
};

// The fire modes (the fire component's +0x28, FireArm::fireMode).
enum FireMode { kFireSingle = 0, kFireBurst = 1, kFireAutomatic = 2 };

class FireArm {
public:
	explicit FireArm(FireArmSettings settings);

	const FireArmSettings& settings() const { return m_settings; }
	int roundsInMag() const { return m_rounds; }
	int magsLeft() const { return m_mags; }
	bool reloading() const { return m_reloadLeft > 0; }
	// A thrown round on its way out (ThrownFireComp states 2 and 3).
	bool throwing() const { return m_launchIn >= 0; }
	// Put away after a throw until showAfterReloadTicks past the reload's
	// start (FireArmSettings::showAfterReloadTicks); drawn again by enable.
	bool hidden() const { return m_hidden; }
	// magLinkWeapon: take the linked firearm's magazine (its rounds, and
	// with `mags` its magazines too, at most our nrOfMags).
	void shareMagazine(const FireArm& from, bool mags);
	// Without a user (reloadWithoutPlayer): a reload under way runs on and
	// fills the magazine; dropReload forgets one (the magazine stays as it is).
	void runReload(float seconds)
	{
		if (m_reloadLeft > 0) {
			fire(seconds, false);
		}
	}
	void dropReload() { m_reloadLeft = 0; }
	// The firearm's enable (0x005c47d0: drawn): delayToUse starts, and the
	// ammo's on-enable runs (0x005f4bb0): held by a soldier (or without
	// reloadWithoutPlayer) a reload under way is dropped; instantReloadOnEnable.
	void enable(bool soldier);
	bool readying() const { return m_readyIn > 0; }
	// A placed round picked up again (the firearm's slot 0x84(1, ...), from
	// 0x004bce30): one round back, into the magazine if there is room,
	// else a magazine's worth on the spares.
	void giveRoundBack()
	{
		if (m_settings.magSize >= 0 && m_rounds < m_settings.magSize) {
			++m_rounds;
		} else if (m_settings.nrOfMags >= 0) {
			++m_mags;
		}
	}
	// toggleWhenNoAmmo's test (0x005f39c0): false with an empty magazine
	// that can't be reloaded.
	bool selectable() const;
	float reloadLeft() const { return m_reloadLeft; } // seconds (the HUD's reload bar)
	float heat() const { return m_heat; } // 0..1
	// Resupply (ammo hubs): adds magazines up to the kit's number; false
	// when full.
	bool addMagazine();
	bool overheated() const { return m_overheatLeft > 0; }
	// Between rounds: the fire component's state above 1 (a round's cycle,
	// state 4 until its ticksBetweenShots are past, or the spin-up, state 3).
	bool cycling() const { return m_cooldown > 0 || m_startDelay >= 0; }
	// The firearm busy for a zoom press (0x005c50e0's test): reloading, or
	// cycling without fire.allowZoomOnROF (0x005d3c40). The press is then
	// taken only with zoomWhenBusy (anim::ZoomSettings::takesPress).
	bool zoomBusy() const { return reloading() || (cycling() && !m_settings.allowZoomOnROF); }
	// The holder's ability battery (0..1) for a ReplenishingAmmoComp's
	// abilityCost (FireArmSettings::replenishing), set before each tick and
	// enable; and what the rounds since the last call cost it.
	void setBattery(float battery) { m_battery = battery; }
	float takeBatteryCost() { return std::exchange(m_batteryCost, 0.0f); }

	// One tick; true when a round leaves the barrel. `trigger` is the fire
	// input held. In the automatic mode a round goes every
	// ticksBetweenShots while it is held; single fire takes one round a
	// press, burst burstSize rounds a press, the burst running on without
	// the trigger until it is out (the fire component's trigger 0x005f1370
	// keeps the press at +0x6c; the round's count 0x005f1820 ends the burst
	// at burstSize - 1, +0x24). A press that finds the gun busy (between
	// rounds) waits for it; one that finds it reloading, overheated or
	// empty is dropped.
	bool tick(float seconds, bool trigger, bool altTrigger = false);
	// The power of the thrown round that left last (1 unless thrown soft).
	float throwPower() const { return m_power; }
	// The fire mode now (the fire component's +0x28; kFireAutomatic for a
	// gun without fire.addFireRate) and the toggle (c_PIToggleFireRate, the
	// firearm's action 0xb in 0x005c50e0 -> fire component slot 0x94,
	// 0x005f1da0): the next of the template's rates, back to the first
	// after the last; false (nothing changes) with fewer than two.
	int fireMode() const { return m_mode; }
	bool toggleFireMode();
	// c_PIReload (the firearm's action 10 in 0x005c50e0: unless the ammo
	// component is reloading already (its state 2), and when it can
	// (0x005c0e70), 0x005c0f40 starts its reload): a magazine not full,
	// with one to spare, is replaced after reloadTime as an empty one is.
	// False when nothing starts.
	bool startReload();
	// A press of the trigger that found the magazine empty since the last
	// call (0x005c2ee0: the ammo's slot 0x34 refuses; the TriggerClick).
	bool takeClick() { return std::exchange(m_clicked, false); }
	// Off: the mode is ignored and the trigger fires as held. (The drones
	// and sentry guns keep the modes: their auto controllers, AIDLL bots,
	// hold the trigger for an automatic gun and tap it otherwise,
	// GameWorld::autoTrigger.)
	void setUseFireModes(bool use) { m_useModes = use; }
	// Whether the round fired now carries a tracer (0x005f2bb0, with a
	// tracer template: the fire component's +0x78 counts down a round at a
	// time and at below 1 a tracer goes with the round and the count starts
	// again at tracerInterval; the first round has one).
	bool takeTracer(int interval);
	// The pulse meter's scan (FireArmSettings::pulseScan): running, and its
	// progress 0..1.
	bool scanning() const { return m_scanning; }
	float scan() const { return m_scan; }
	static constexpr float kPulseScanStep = 1; // "pulse-meter-scan-step"

private:
	bool fire(float seconds, bool trigger); // tick's rounds
	FireArmSettings m_settings;
	int m_rounds;
	int m_mags;
	int m_cooldown = 0;
	float m_reloadLeft = 0;
	bool m_reloadFilled = false; // the magazine swapped (changeMagAt) in this reload
	int m_readyIn = 0;           // ticks of delayToUse left
	float m_heat = 0;
	float m_overheatLeft = 0;
	int m_mode = kFireAutomatic;
	bool m_useModes = true;
	bool m_triggerWas = false;
	bool m_busyUntilRelease = false; // a round fired with busyUntilButtonRelease, trigger still held
	int m_startDelay = -1;   // fireStartDelay's ticks left (-1: none pending)
	Random m_startRandom {0xcfb}; // 0x005f1650's draws (its seed: the owner's +0x214 or the time, + 0xcfb)
	bool m_pending = false; // a press waiting for the gun
	bool m_clicked = false; // a press on an empty magazine (takeClick)
	int m_burstLeft = 0;
	int m_launchIn = -1;   // a thrown round's ticks to go (states 2 and 3), -1 none
	bool m_soft = false;   // the soft throw's flag (+0x178): the alternate input still held
	bool m_altWas = false;
	int m_softTicks = 0;
	float m_power = 1;     // +0x168
	bool m_hidden = false; // the object's flag 2 cleared by the launch
	int m_showIn = -1;     // ticks to showing again (-1 none)
	void reloadStarted();
	// "A magazine can be taken" for a reload (0x005f4140, or a
	// ReplenishingAmmoComp's 0x005d79a0), the magazine count aside.
	bool batteryAllowsReload() const;
	float m_battery = 0;
	float m_batteryCost = 0;
	int m_tracerCount = 0; // +0x78
	bool m_scanning = false; // the ammo component's +0x58
	float m_scan = 0;        // +0x5c
};

// The camera's recoil (WeaponBasedRecoilComp's cameraRecoilSpeed and
// cameraRecoilSize, template +0x24 / +0x28: the vehicle guns'): each round
// the gun's component (0x005ddfc0's end) hands them to the camera of its
// control object (the first PlayerControlObject up its tree, interface
// 0xc4c5 slot 0x48) through 0x00610180, which takes them when the camera
// is in its inside or nose view (+0x250 1 or 2): a phase from 0. The
// camera's update (0x006101c0, for the local player's) moves the phase by
// seconds x speed x *0x00a739c4 (1) and, past 1, stops; the kick is
// sin(phase x 2 pi) x size x *0x00a739c0 (0.6) degrees, turned in with
// the camera's shakes (0x00610620 -> 0x005b1a70).
struct CameraRecoil {
	float speed = 0, size = 0;
	float phase = 0;
	bool on = false;
	void start(float recoilSpeed, float recoilSize);
	// One update; the kick in degrees.
	float update(float seconds);
};

// The gun's own recoil (WeaponBasedRecoilComp, template 0x005de1b0:
// recoilForce +0xc, recoilSpeed +0x10, recoilSpeedGB +0x14, recoilSize
// +0x18; their setters 0x005dda80 / 0x005ddac0 keep pi / recoilSpeed at
// +0x1c and (pi / 2) / recoilSpeedGB at +0x20). Each round (0x005ddfc0)
// stamps two tick counts: the kick ends round(+0x1c x 15) ticks on, the
// return round(+0x20 x 30) after that. The update (0x005ddd80, with
// recoilSpeed above 0) slides the gun's local matrix back along its own z:
// during the kick by size x 0.025 x sin((+0x1c / 2 + t) x recoilSpeed), t
// the seconds left of it (0 to the full slide); during the return by size x
// 0.025 x (t / +0x20), t the seconds left of it; then back at rest. With
// recoilForce above 0 the round also pushes the vehicle's root body by
// -forward x recoilForce at the gun (physics slot 0xa0, 0x006c7b10: added
// to the acceleration, its moment to the torque; slot 0x160 wakes it).
// Above 5 the force also starts 0x005a6e70 at the gun (GameWorld::cameraShake:
// radius min(force, 12), material 0, no damage: the tinnitus of a soldier
// on foot near it, no shake).
struct GunRecoil {
	float force = 0, speed = 0, speedGB = 0, size = 0;
	uint32_t kickEnd = 0, returnEnd = 0; // ticks (0: at rest)
	static GunRecoil fromTemplate(const world::ObjectTemplate& firearm);
	bool any() const { return force > 0 || (speed > 0 && size != 0); }
	void fired(uint32_t tick);
	// The slide back along the gun's z this tick (metres, >= 0 back).
	float slide(uint32_t tick) const;
};

// DefaultPenetrateComp's first tests (0x005eaca0) for a round meeting a
// face of `material`: No, Free (water with allowLiquidPenetration: no
// loss), or Loss (the damage loss decides). `object`: the face is an
// object's (not the terrain's).
enum class Penetration { No, Free, Loss };
Penetration penetration(const ProjectileSettings& round, const physics::Material& material, bool object);
// Its loss: minDamageLoss on a one-sided material; else with a way out
// thickness x damageLoss within minDamageLoss and maxDamageLoss, without
// one maxDamageLoss.
float penetrationLoss(const physics::Material& material, bool wayOut, float thickness);
// Whether the damage left goes on: at or above a tenth of the template's.
inline bool penetrationLeft(const ProjectileSettings& round, float left) { return left >= 0.1f * round.damage; }

struct Projectile {
	physics::Vec3 origin;
	physics::Vec3 position;
	physics::Vec3 velocity;
	// Its nose (a unit vector; zero for "along its flight"): the firing's
	// direction at the launch. A Point round's physics doesn't turn it; the
	// follow does (0x005ecb30 turns the object's matrix, not its velocity),
	// and the engine pushes along it, so a round launched at rest (the
	// gunship's TV missile, velocity 0) still flies off.
	physics::Vec3 heading;
	physics::Vec3 acceleration; // gravity, applied the next tick
	float timeLeft = 1;
	const ProjectileSettings* settings = nullptr;
	uint32_t shooter = 0;
	int player = -1; // who fired it (-1 for none)
	bool resting = false; // a grenade lying still
	// Stuck (StickyCollisionComp's +0x14, 0x005e83d0): it lies where it
	// met the face, its up along the face's normal (stuckNormal). On a
	// vehicle (stuckTo, the component's +0x10 handle) it keeps its place
	// and normal in the vehicle's frame (+0x18 / +0x48) and rides it.
	bool stuck = false;
	uint32_t stuckTo = 0;
	physics::Vec3 stuckLocal;  // its place in stuckTo's frame
	physics::Vec3 stuckLocalNormal;
	physics::Vec3 stuckNormal {0, 1, 0}; // in the world
	// The vehicle a round lies on (not stuck). In the original a resting
	// round is a ResponsePhysics body in contact with the vehicle; the
	// rewrite's rounds are points without one (no contact friction to
	// carry them), so a round lying on a vehicle falls again once the
	// vehicle moves from under it.
	uint32_t lyingOn = 0;
	float age = 0;
	uint32_t lockedOn = 0; // the vehicle a seeker chases
	uint32_t lockedBait = 0; // or the round (serial) carrying a TTMagnet target (the booster's MagneticBait)
	// Wire guidance (LPTargetComp, 0x005e2e20 -> 0x005e27a0) from its
	// firearm: target.wireGuidedLookAheadDistance (+0xb4, 20 by default),
	// target.maxDistance (+0x78), dontLooseGuidanceOnObstruction (+0xb8);
	// wireLost once guidance is gone (out of range, the line cut, or a
	// reload with disableGuidanceOnReload +0xb1).
	float wireLookAhead = 20;
	float wireMaxDistance = 0;
	bool wireKeepOnObstruction = false;
	bool wireLost = false;
	bool newTarget = false; // the follow's +0x94: its target set since its last turn
	bool followTurned = false; // the follow steered it this tick (0x005ed1a0's result)
	// SeekClosestTargetComp's relock: armed at each shot (0x005b2e70 ->
	// setTarget(t, 1)); a candidate held since pendingSince.
	bool relockArmed = false;
	uint32_t pending = 0;
	int64_t pendingSince = 0;
	float damageScale = 1;  // what is left of its damage after penetrating (DefaultPenetrateComp)
	float burstDistance = 0; // the airburst: off at this far from its origin (fire.useDistanceCounter, 0 none)
	// The follow's state (0x005ecb30): chasing from when (the seek's
	// trackingDelay after the lock taken from none, the follow's +0x18:
	// 0x005b2c10, 0x005b3e60), the fastest the target has gone
	// (+0x90) and whether that armed it (+0x95).
	float followFrom = -1;
	float targetTopSpeed = 0;
	bool armed = false;
	int team = 0;         // the thrower's
	float yaw = 0;        // the facing it was thrown with (mines' cones)
	float triggerIn = 0.25f; // the radius trigger's countdown (the component's +0x14, starting full)
	bool alive = true;
	// Its firearm went (its player left): removed, without its effect, when
	// this runs out (keepProjectiles, 0x005f1cc0); below 0 not counting.
	float keepLeft = -1;
	float keepProjectiles = 5; // the firing firearm's
	// A tracer round's tracer (ProjectileSettings' tracer*): it shows from
	// `tracerFrom` (the muzzle as drawn) and closes on the round
	// (tracerOffset, what it lies off the round's own start), and its
	// length grows from where it (re)started (tracerStart: the start, or a
	// bounce's point, 0x005b4980 restarting the growth at +0x1dc).
	bool tracer = false;
	physics::Vec3 tracerOffset;
	physics::Vec3 tracerStart;
	bool tracerHit = false; // +0x23c: it hit something (the tracer sits on the round from then on)
	// A joined client's round (GameWorld::fireRemoteRound, launchRemoteProjectile):
	// it flies, hits and shows its effects, tracer and decals as any, but
	// damages nothing: the client game logic (vtable 0x0092a530) has a
	// return (0x008c961e) in the hit slots 0xf8..0x104 where the server's
	// (0x009294e8) has 0x004ba440, 0x004ba500, 0x004ba5c0 and 0x004ba3f0.
	bool visual = false;
	uint32_t ghostObject = 0; // a server projectile's ghost object it follows (0: the client's own round)
	uint32_t serial = 0;      // a stable id (the server's projectile ghosts, HostedGame::trackProjectiles)
};

// The radius trigger's shape test (0x005e8aa0; GameWorld's radius trigger
// has the rest: the trigger types, the spared side, the line to it): the
// object's origin, frame and box (object space), whether it is a soldier
// and whether it touches nothing (its node's +0x249). With triggerAngle 0
// or less, all but soldiers need the point min(distance, triggerRadius)
// from the mine towards the origin inside the box (0x005a60a0), and one
// touching nothing radiusDetonateWithAirVehicles; above 0, the origin in
// the mine's frame `mineFrame` (rows x, y, z) ahead and within tan(angle)
// x z of its axis across and up, less half the box's width and height.
// Then its speed at least triggerVictimMinSpeed.
struct TriggerCandidate {
	physics::Vec3 origin;
	physics::Mat3 rotation;
	physics::Vec3 boxMin, boxMax;
	physics::Vec3 velocity;
	bool soldier = false;
	bool inAir = false;
};
bool radiusTriggerShape(const ProjectileSettings& mine, const physics::Vec3& minePosition, const physics::Mat3& mineFrame,
	const TriggerCandidate& candidate);

// A round's frame once it sticks (StickyCollisionComp, 0x005e83d0): its
// up the face's normal, its x axis as near the frame it had as that
// leaves (z = x x up, x = up x z, up = z x x, each normalised,
// 0x005a2b60 / 0x004481a0), set as the object's rotation (slot 0x80).
physics::Mat3 stuckFrame(const physics::Mat3& before, const physics::Vec3& normal);

// A tracer's place and size this frame (0x005b3e60, 0x005b3cb0): `round`
// the round's position, `velocity` its velocity. Its position is the
// round's, until the round has gone tracerConvergeDistance from its start
// (or hit something) moved by the muzzle offset x (1 - distance /
// tracerConvergeDistance); a distance of -1 puts it on the round at once.
// It points along the velocity (above 0.025 m^2/s^2: z forward, x = up x
// z, y = z x x; straight up or down the frame of 0x005b3e60's else). Its
// scale (interface 0xebc881fa slot 0x10) is (s, s, min(L, along)) with s
// = tracerSizeModifier and L = tracerScaler x s: the growth time
// (+0x264 = tracerScaler / speed x s at the start, 0x005b5300) has the
// length grow with the round's travel, so it never reaches back past its
// start; `along` is how far the round has gone along its heading from
// `start`. The tracer mesh's tail runs along -z from its origin (the
// p_tracer2 meshes 0..-0.98), so it trails the round.
struct TracerLook {
	physics::Vec3 position;
	physics::Mat3 axes; // rows x, y, z: scaled
	bool shown = false;
};
TracerLook tracerLook(const ProjectileSettings& settings, const physics::Vec3& round, const physics::Vec3& velocity,
	const physics::Vec3& origin, const physics::Vec3& start, const physics::Vec3& offset, bool hit);

// A RotationalPoint projectile's velocity after a contact (0x006c0d00):
// the part along the normal reversed times `elasticity` (the mean of the
// two materials'), the sliding part times 1 - `friction` (at most 1).
physics::Vec3 bounceVelocity(const physics::Vec3& velocity, const physics::Vec3& normal, float elasticity, float friction);

// A magnet's follow and thrust for a tick (0x005ecb30, 0x005b3e60): the
// target's top speed arms it past armingSpeed; it heads for `aim` plus
// the target's velocity x (distance / own speed) x predictionFactor,
// accelerating while under the cap (maxSpeed, or with useTargetSpeed the
// top speed, x speedModifier once armed) and braking along its velocity
// at it.
// A target with overrideArmingSpeed (the bait, TargetObject +0x2a7 through
// 0x0058e410: 0x005ecb30 arms the follow, and 0x005b3e60 through the
// follow's slot 0x44, 0x005ed310, keeps maxSpeed as the cap) arms it at
// once and lifts the useTargetSpeed cap.
void followMagnet(Projectile& projectile, const physics::Vec3& aim, const physics::Vec3& targetVelocity, float seconds,
	bool overrideArming = false);

// DefaultFollowComp's turn (0x005ed1a0 -> 0x005ecb30, once a 30 Hz tick:
// follow.maxYaw / maxPitch are degrees a tick): towards `target` plus its
// velocity x (distance / own speed) x predictionFactor, in the missile's
// own upright frame (forward its flight, right = up x forward): x, y, z the
// way's components, the yaw |asin x| degrees at most maxYaw (to the side of
// x), the pitch |asin y| at most maxPitch (to the side of y), turned about
// its right and then its up and rebuilt upright (0x005b1a70), the speed
// kept. With the target behind (z <= 0) it doesn't turn, unless
// magneticBehaviour, which turns it at the full rates; magneticBehaviour
// also lifts the caps while it is still or on the tick its target changed
// (`newTarget`). follow.changeYaw, changePitch and minDist (+0x14, +0x18,
// +0x1c; setters 0x004f59fc, 0x004f5b1f, 0x004f5c5a) are stored and saved
// but never read. False when it didn't turn.
bool followStep(Projectile& projectile, const physics::Vec3& target, const physics::Vec3& targetVelocity, float seconds,
	bool newTarget = false);

// The round's nose: its heading, else the direction of its flight (zero
// for a round at rest without one).
physics::Vec3 noseOf(const Projectile& projectile);

// The engine's push for a tick (see ProjectileSettings::startDelay).
void thrustProjectile(Projectile& projectile, float seconds);

// Whether the follow turns the round this tick (0x005ed1a0): from its
// start (followFrom, else trackingDelay after the launch) until
// follow.followTime seconds after it, when that is set.
bool followActive(const Projectile& projectile);

// A normal(0, 1) number (0x005b28d0: Marsaglia's polar method on the
// engine's generator, the second of each pair kept for the next call).
// The deviation components fill a table of 1024 of them once
// (0x005f5750) and read it at (Game slot 0x68 + the round's index) & 1023.
class Gaussian {
public:
	float next(Random& random);

private:
	bool m_has = false;
	float m_kept = 0;
};

// The firing's build-up of a deviation component (deviation.setFireDev
// max perShot decay: template +0x10, +0x8, +0xc by 0x005f5360, all 0 by
// default; deviation.minDev +0x14). Each round fired (0x005f5540) adds
// perShot, held at max (above it for a positive perShot, below it for a
// negative one: the machine guns' grow steadier); each update (0x005dedb0)
// takes off decay unless the firer holds the trigger (its input, the
// update's by-value argument), and a build-up below 0 is 0 then. The
// deviation is minDev plus the build-up (0 while zoomed: the component's
// +0x2c; the rewrite's users don't zoom).
struct DeviationBuildUp {
	float max = 0, perShot = 0, decay = 0;
	float minDev = 0;
	float value = 0; // +8
	static DeviationBuildUp fromTemplate(const world::ObjectTemplate& t);
	void update(bool firing);
	// The SoldierDeviationComp's own decay (0x005de980): the build-up
	// stops at 0 on the side it grows from, below 0 for a positive perShot
	// and above it for a negative one (the machine guns' steadiness,
	// setFireDev -1 -0.09 -0.009, decays back up to 0, not past it).
	void updateSoldier(bool firing);
	void fired();
	float deviation() const { return minDev + value; }
};

// A hand-held firearm's SoldierDeviationComp (template class 0x94e1; the
// object's update 0x005de980, once a tick with the holder's input). The
// template: setFireDev and minDev as DeviationBuildUp; setTurnDev max
// pitch yaw decay (0x005de3c0: +0x2c, +0x20, +0x24, +0x28), setSpeedDev
// max throttle strafe decay (0x005de3e0: +0x3c, +0x30, +0x34, +0x38),
// setMiscDev max add decay (0x005de400: +0x48, +0x40, +0x44); devModStand,
// devModCrouch, devModLie (+0x4c..+0x54, 1), devModZoom and
// minDevModZoom (+0x58, +0x5c, 1). Each update:
//   - the turn's part: |PIMouseLookY| x pitch + |PIMouseLookX| x yaw (the
//     look inputs, PIMouseLook units) less decay is added, held at max
//     (decaying back to it when above); without a turn it decays to 0;
//   - the speed's part: throttle (|PIThrottle| > 0.01) adds `throttle`,
//     strafing (|PIYaw|) `strafe`, held at max; without, decay;
//   - the misc part: while its condition holds (the update's flag; the
//     rewrite: in the air) add - decay up to max, else decay;
//   - the firing's build-up decays off the trigger (DeviationBuildUp);
//   - the stance's modifier (Stand, Crouch, Lie), and zoomed devModZoom
//     on the whole and minDevModZoom on minDev (else 1 and 1).
// The deviation = (minDev x minDevModZoom + misc + turn + speed + fire)
// x zoom x stance. (Its +0x44 extra, added on a condition of the holder's
// slot 0x1a0 not decoded, is left out.)
struct SoldierDeviation {
	DeviationBuildUp fire;
	float turnMax = 0, turnPitch = 0, turnYaw = 0, turnDecay = 0;
	float speedMax = 0, speedThrottle = 0, speedStrafe = 0, speedDecay = 0;
	float miscMax = 0, miscAdd = 0, miscDecay = 0;
	float modStand = 1, modCrouch = 1, modLie = 1, modZoom = 1, minModZoom = 1;
	bool present = false;
	float turn = 0, speed = 0, misc = 0; // +0x24, +0x28, +0x2c
	float total = 0;                     // +4
	static SoldierDeviation fromTemplate(const world::ObjectTemplate& t);
	// lookX / lookY in PIMouseLook units; stance 0 stand, 1 crouch, 2 lie.
	void update(float throttle, float strafe, float lookX, float lookY, bool misc, bool firing, int stance, bool zoomed);
	float deviation() const { return total; }
};

// DefaultGunSwayComp (template class 0x94f6, its 28 sway.* settings at
// +0xc..+0x78; object 0x005e0640, update 0x005e0da0 a tick): the aim
// wanders by nudging the holder's look inputs. With gunSwayUseZoom it
// runs only while zoomed. Per axis (pitch: 0x005e09a0 on PIMouseLookY;
// left-right: 0x005e0ba0 on PIMouseLookX), every UpdateRate seconds a
// step of Max / Steps less the smoothing is taken along the swing's
// direction: Factor x ZoomModifier x step x dt x the stability (below) x
// the direction (+-1) is put on the look input when the player's own is
// smaller. A swing turns back at Max + its random extra (RandomMin ..
// RandomMax, drawn at each end), keeping on (by DirModifier and
// DirThreshold's draw) past it until 2 x (Max + extra), where it starts
// again. The smoothing (0x005dfc20, SmoothFactor / SmoothMax /
// SmoothThreshold) slows the step near the swing's ends. Holding the
// sprint key while zoomed with the scope stabilizer (the holder's
// ability, hasScopeStabilizerAbility) steadies it (0x005e0750): the
// stability falls to 0 over stabilityTime and comes back over
// stabilityRecoverTime; it lasts at most stabilityDuration (its use
// recovering over stabilityDurationRecoverTime).
struct GunSway {
	struct Axis {
		float steps = 0, factor = 0, max = 0, updateRate = 0, dirModifier = 0, dirThreshold = 0;
		float randomMin = 0, randomMax = 0, smoothFactor = 0, smoothMax = 0, smoothThreshold = 0;
		// State: the step timer, this swing's way, its total, the
		// direction, the extra, the smoothing, the keep-going flag.
		float timer = 0, swing = 0, total = 0, direction = 1, extra = 0, smooth = 0;
		bool keepGoing = false;
	};
	bool present = false;
	bool useZoom = false;
	float zoomModifier = 1;
	float recoverTime = 0, stabilityTime = 0, duration = 0, durationRecoverTime = 0;
	Axis pitch, lr;
	float stability = 1, used = 0;
	bool steadying = false, latched = false;
	static GunSway fromTemplate(const world::ObjectTemplate& t);
	// One update: `lookX` / `lookY` the holder's look inputs this tick
	// (PIMouseLook units; changed in place). `random01` gives (0, 1].
	void update(float seconds, bool zoomed, bool sprintHeld, bool hasStabilizer, float& lookX, float& lookY,
		const std::function<float()>& random01);
};

// A round's deviation (the deviation components' apply, 0x005f5540): two
// normal(0, 1) numbers times `deviation` (minDev with the firing's
// build-up for the main round, subProjectileDev for each sub-projectile),
// each times 0.01 x the firearm's velocity, along the projectile's up and
// right axes. Not a cone in degrees: deviation 1 is 0.57 degrees, one
// standard deviation, on each axis.
physics::Vec3 deviateVelocity(const physics::Vec3& velocity, const physics::Vec3& right, const physics::Vec3& up, float speed,
	float deviation, float alongUp, float alongRight);

// One tick of point physics (0x006c6170 / 0x006c57a0) for a projectile.
void moveProjectile(Projectile& projectile, float seconds, float gravity = physics::tuning::kGravity);

} // namespace dice::hfe::game
