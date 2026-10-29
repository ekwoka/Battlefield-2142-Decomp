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
#include <vector>

namespace dice::hfe::world {
class ObjectTemplate;
}

namespace dice::hfe::game {


struct ProjectileSettings {
	std::string name; // the projectile's template (its S_<name>_Looping flight sound)
	float damage = 0;
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
	// physicsType RotationalPoint or Mesh: it bounces off what it hits
	// (grenades) and detonates when its time to live runs out, not on impact.
	bool bounces = false;
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
	enum class Seek { None, Magnet, Aim, Beacon };
	Seek seek = Seek::None;
	float seekDelay = 0;
	float lockDistance = 0;
	float loseDistance = 0;
	float maxYawRate = 0, maxPitchRate = 0;
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
	// startDelay (the GenericProjectile's): seconds before its acceleration
	// pushes it along its flight.
	float startDelay = 0;
	// onlyDetonateOnMaterial (template +0x348, -1 none) for the first
	// onlyDetonateOnMaterialTimeLimit seconds (+0x34c; 0: always): until
	// then it sets off only on that material, and anything else it meets
	// that is no Titan the server destroys (3e10 damage) while it flies on
	// (0x005b4ff0).
	int onlyDetonateOnMaterial = -1;
	float onlyDetonateOnMaterialTime = 0;
	// Mines: an enemy inside triggerRadius and triggerAngle (degrees either
	// side of the mine's facing) moving faster than triggerVictimMinSpeed
	// sets it off; the blast reaches explosionConeAngle either side of the
	// facing (0: all round).
	float triggerRadius = 0;
	float triggerAngle = 180;
	float triggerMinSpeed = 0;
	float coneAngle = 0;
	// No trigger and no fuse to speak of (C4): set off by its owner.
	bool remote = false;

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

struct FireArmSettings {
	int roundsPerMinute = 600;
	int magSize = 30;
	int nrOfMags = 5;
	float reloadTime = 2;
	float velocity = 0;
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
	float reloadLeft() const { return m_reloadLeft; } // seconds (the HUD's reload bar)
	float heat() const { return m_heat; } // 0..1
	// Resupply (ammo hubs): adds magazines up to the kit's number; false
	// when full.
	bool addMagazine();
	bool overheated() const { return m_overheatLeft > 0; }

	// One tick; true when a round leaves the barrel. `trigger` is the fire
	// input held. In the automatic mode a round goes every
	// ticksBetweenShots while it is held; single fire takes one round a
	// press, burst burstSize rounds a press, the burst running on without
	// the trigger until it is out (the fire component's trigger 0x005f1370
	// keeps the press at +0x6c; the round's count 0x005f1820 ends the burst
	// at burstSize - 1, +0x24). A press that finds the gun busy (between
	// rounds) waits for it; one that finds it reloading, overheated or
	// empty is dropped.
	bool tick(float seconds, bool trigger);
	// The fire mode now (the fire component's +0x28; kFireAutomatic for a
	// gun without fire.addFireRate) and the toggle (c_PIToggleFireRate, the
	// firearm's action 0xb in 0x005c50e0 -> fire component slot 0x94,
	// 0x005f1da0): the next of the template's rates, back to the first
	// after the last; false (nothing changes) with fewer than two.
	int fireMode() const { return m_mode; }
	bool toggleFireMode();
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

private:
	FireArmSettings m_settings;
	int m_rounds;
	int m_mags;
	int m_cooldown = 0;
	float m_reloadLeft = 0;
	float m_heat = 0;
	float m_overheatLeft = 0;
	int m_mode = kFireAutomatic;
	bool m_useModes = true;
	bool m_triggerWas = false;
	bool m_pending = false; // a press waiting for the gun
	int m_burstLeft = 0;
	int m_tracerCount = 0; // +0x78
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
// (Above 5 the force also starts 0x005a6e70, the explosions' camera shake
// for the players near, which the rewrite leaves out.)
struct GunRecoil {
	float force = 0, speed = 0, speedGB = 0, size = 0;
	uint32_t kickEnd = 0, returnEnd = 0; // ticks (0: at rest)
	static GunRecoil fromTemplate(const world::ObjectTemplate& firearm);
	bool any() const { return force > 0 || (speed > 0 && size != 0); }
	void fired(uint32_t tick);
	// The slide back along the gun's z this tick (metres, >= 0 back).
	float slide(uint32_t tick) const;
};

struct Projectile {
	physics::Vec3 origin;
	physics::Vec3 position;
	physics::Vec3 velocity;
	physics::Vec3 acceleration; // gravity, applied the next tick
	float timeLeft = 1;
	const ProjectileSettings* settings = nullptr;
	uint32_t shooter = 0;
	int player = -1; // who fired it (-1 for none)
	bool resting = false; // a grenade lying still
	float age = 0;
	uint32_t lockedOn = 0; // the vehicle a seeker chases
	bool newTarget = false; // the follow's +0x94: its target set since its last turn
	float damageScale = 1;  // what is left of its damage after penetrating (DefaultPenetrateComp)
	// A magnet's follow state (0x005ecb30): chasing from when (the seek's
	// trackingDelay after the lock), the fastest the target has gone
	// (+0x90) and whether that armed it (+0x95).
	float followFrom = -1;
	float targetTopSpeed = 0;
	bool armed = false;
	int team = 0;         // the thrower's (mines spare their own side unless allowFriendlyFireDetonation)
	float yaw = 0;        // the facing it was thrown with (mines' cones)
	bool alive = true;
	// A tracer round's tracer (ProjectileSettings' tracer*): it shows from
	// `tracerFrom` (the muzzle as drawn) and closes on the round
	// (tracerOffset, what it lies off the round's own start), and its
	// length grows from where it (re)started (tracerStart: the start, or a
	// bounce's point, 0x005b4980 restarting the growth at +0x1dc).
	bool tracer = false;
	physics::Vec3 tracerOffset;
	physics::Vec3 tracerStart;
	bool tracerHit = false; // +0x23c: it hit something (the tracer sits on the round from then on)
};

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
void followMagnet(Projectile& projectile, const physics::Vec3& aim, const physics::Vec3& targetVelocity, float seconds);

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
// (`newTarget`). follow.changeYaw, changePitch and minDist are stored and
// saved but never read. False when it didn't turn.
bool followStep(Projectile& projectile, const physics::Vec3& target, const physics::Vec3& targetVelocity, float seconds,
	bool newTarget = false);

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
