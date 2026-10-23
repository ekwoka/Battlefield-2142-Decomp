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

#include <array>
#include <string>

namespace dice::hfe::world {
class ObjectTemplate;
}

namespace dice::hfe::game {


struct ProjectileSettings {
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
	// at most maxYaw/maxPitch radians a second.
	enum class Seek { None, Magnet, Aim };
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

	static FireArmSettings fromTemplate(const world::ObjectTemplate& firearm);
	int ticksBetweenShots() const { return roundsPerMinute > 0 ? 1800 / roundsPerMinute : 1; }
};

class FireArm {
public:
	explicit FireArm(FireArmSettings settings);

	const FireArmSettings& settings() const { return m_settings; }
	int roundsInMag() const { return m_rounds; }
	int magsLeft() const { return m_mags; }
	bool reloading() const { return m_reloadLeft > 0; }
	float heat() const { return m_heat; } // 0..1
	// Resupply (ammo hubs): adds magazines up to the kit's number; false
	// when full.
	bool addMagazine();
	bool overheated() const { return m_overheatLeft > 0; }

	// One tick; true when a round leaves the barrel.
	bool tick(float seconds, bool trigger);

private:
	FireArmSettings m_settings;
	int m_rounds;
	int m_mags;
	int m_cooldown = 0;
	float m_reloadLeft = 0;
	float m_heat = 0;
	float m_overheatLeft = 0;
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
	// A magnet's follow state (0x005ecb30): chasing from when (the seek's
	// trackingDelay after the lock), the fastest the target has gone
	// (+0x90) and whether that armed it (+0x95).
	float followFrom = -1;
	float targetTopSpeed = 0;
	bool armed = false;
	int team = 0;         // the thrower's (mines spare their own side unless allowFriendlyFireDetonation)
	float yaw = 0;        // the facing it was thrown with (mines' cones)
	bool alive = true;
};

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
