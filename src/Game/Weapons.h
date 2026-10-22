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
	float chaseSpeed = 0; // follow.armingSpeed × speedModifier (see GameWorld::steerSeeker)
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
	int team = 0;         // the thrower's (mines spare their own side unless allowFriendlyFireDetonation)
	float yaw = 0;        // the facing it was thrown with (mines' cones)
	bool alive = true;
};

// One tick of point physics (0x006c6170 / 0x006c57a0) for a projectile.
void moveProjectile(Projectile& projectile, float seconds, float gravity = physics::tuning::kGravity);

} // namespace dice::hfe::game
