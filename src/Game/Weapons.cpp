#include "Weapons.h"

#include "Physics/MaterialManager.h"
#include "World/ObjectTemplate.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <functional>

namespace dice::hfe::game {

namespace {

float number(const world::ObjectTemplate& t, const char* name, float fallback)
{
	const auto it = t.settings().find(name);
	return it == t.settings().end() ? fallback : static_cast<float>(std::atof(it->second.c_str()));
}

// A Vec3 row ("x/y/z"); zeros when unset.
physics::Vec3 vector3(const world::ObjectTemplate& t, const char* name)
{
	physics::Vec3 v;
	const auto it = t.settings().find(name);
	if (it != t.settings().end()) {
		std::sscanf(it->second.c_str(), "%f/%f/%f", &v.x, &v.y, &v.z);
	}
	return v;
}

bool sameText(const std::string& a, const char* b)
{
	const size_t n = std::strlen(b);
	return a.size() == n && std::equal(a.begin(), a.end(), b, [](char x, char y) {
		return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
	});
}

} // namespace

ProjectileSettings::Hover projectileHover(const world::ObjectTemplate& forceObject)
{
	ProjectileSettings::Hover h;
	h.present = true;
	h.enableOnStartMessage = number(forceObject, "enableOnStartMessage", 0) != 0;
	h.height = vector3(forceObject, "targetPosition").y;
	h.tolerance = number(forceObject, "regulationTolerance", 0.5f);
	const physics::Vec3 y = vector3(forceObject, "regulatePosY");
	h.a = y.x;
	h.b = y.y;
	h.relativeY = static_cast<unsigned>(number(forceObject, "relativeYCollisionGroupInverse", 0));
	return h;
}

float seekCost(float angle, float distance, float bonus)
{
	return angle * bonus * 500.0f + (1.0f - bonus) * distance;
}

float hoverVelocity(const ProjectileSettings::Hover& hover, float y, float vy, float goal, float seconds)
{
	const float a = hover.a, b = hover.b;
	return vy + 30.0f * (2 * a * vy + (a * a + b * b) * (goal - y)) * seconds;
}

Penetration penetration(const ProjectileSettings& round, const physics::Material& material, bool object)
{
	if (!round.penetrator || (!object && !material.hasWaterPhysics)) {
		return Penetration::No;
	}
	if (!material.overrideNeverPenetrate) {
		if (material.hasWaterPhysics) {
			return round.allowLiquidPenetration ? Penetration::Free : Penetration::No;
		}
		if (!round.allowSolidPenetration) {
			return Penetration::No;
		}
	}
	return material.damageLoss == -1.0f ? Penetration::No : Penetration::Loss;
}

float penetrationLoss(const physics::Material& material, bool wayOut, float thickness)
{
	if (material.isOneSided) {
		return material.minDamageLoss;
	}
	if (!wayOut) {
		return material.maxDamageLoss;
	}
	return std::clamp(thickness * material.damageLoss, material.minDamageLoss, material.maxDamageLoss);
}

bool radiusTriggerShape(const ProjectileSettings& mine, const physics::Vec3& minePosition, const physics::Mat3& mineFrame,
	const TriggerCandidate& c)
{
	if (mine.triggerAngle <= 0) {
		if (!c.soldier) {
			const physics::Vec3 d = c.origin - minePosition;
			const float distance = std::sqrt(physics::lengthSquared(d));
			// 0x004bb060: the shorter of the distance and the radius.
			const physics::Vec3 point =
				distance > 1e-6f ? minePosition + d * (std::min(distance, mine.triggerRadius) / distance) : minePosition;
			const physics::Vec3 local = c.rotation.untransform(point - c.origin);
			const bool inside = local.x >= c.boxMin.x && local.x <= c.boxMax.x && local.y >= c.boxMin.y
				&& local.y <= c.boxMax.y && local.z >= c.boxMin.z && local.z <= c.boxMax.z;
			if (!inside || (!mine.radiusDetonateWithAirVehicles && c.inAir)) {
				return false;
			}
		}
	} else {
		const physics::Vec3 local = mineFrame.untransform(c.origin - minePosition);
		if (local.z < 0) {
			return false; // behind it
		}
		const float reach = std::fabs(std::tan(mine.triggerAngle * 0.017453292f) * local.z); // DAT_00a02d70
		if (std::fabs(local.x) - (c.boxMax.x - c.boxMin.x) * 0.5f >= reach
			|| std::fabs(local.y) - (c.boxMax.y - c.boxMin.y) * 0.5f >= reach) {
			return false;
		}
	}
	return std::sqrt(physics::lengthSquared(c.velocity)) >= mine.triggerMinSpeed; // the node's slot 100
}

physics::Mat3 stuckFrame(const physics::Mat3& before, const physics::Vec3& normal)
{
	const auto unit = [](const physics::Vec3& v) { // 0x004481a0, on the squared length
		const float l2 = physics::lengthSquared(v);
		if (std::fabs(l2 - 1.0f) < 1.1920929e-07f) {
			return v;
		}
		return std::fabs(l2) < 1.1920929e-07f ? physics::Vec3 {} : v * (1.0f / std::sqrt(l2));
	};
	physics::Mat3 m;
	physics::Vec3 x = unit(before.rows[0]);
	const physics::Vec3 up = unit(normal);
	const physics::Vec3 z = unit(physics::cross(x, up));
	x = unit(physics::cross(up, z));
	m.rows = {x, unit(physics::cross(z, x)), z};
	return m;
}

ProjectileSettings ProjectileSettings::fromTemplate(const world::ObjectTemplate& t)
{
	ProjectileSettings s;
	s.name = t.name();
	s.damage = number(t, "damage", s.damage);
	s.hitImpactModifier = number(t, "hitImpactModifier", s.hitImpactModifier);
	s.activeDefenseEnabled = number(t, "activeDefenseEnabled", 0) != 0;
	s.minDamage = number(t, "minDamage", s.minDamage);
	s.distToStartLoseDamage = number(t, "distToStartLoseDamage", s.distToStartLoseDamage);
	s.distToMinDamage = number(t, "distToMinDamage", s.distToMinDamage);
	s.gravityModifier = number(t, "gravityModifier", s.gravityModifier);
	s.drag = number(t, "drag", s.drag);
	s.mass = std::max(number(t, "mass", s.mass), 0.001f);
	s.material = static_cast<int>(number(t, "material", 0));
	if (const auto tracer = t.settings().find("tracerTemplate"); tracer != t.settings().end()) {
		s.tracerTemplate = tracer->second;
	}
	s.tracerInterval = static_cast<int>(number(t, "tracerInterval", 1));
	s.tracerScaler = number(t, "tracerScaler", 0);
	s.tracerSizeModifier = number(t, "tracerSizeModifier", 1);
	s.tracerConvergeDistance = number(t, "tracerConvergeDistance", 100);
	s.maxTracerScaler = number(t, "maxTracerScaler", 0.1f);
	// A detonation component (DefaultDetonationComp, template 0x953b) with
	// its constructor's defaults (0x005e9df0): explosionMaterial -1,
	// explosionForce 150, explosionYMod 1, explosionRadius 10,
	// explosionDamage 500 (fields 0x30, 0x34, 0x38, 0x3c, 0x44).
	const auto& components = t.components();
	s.detonates = std::any_of(components.begin(), components.end(), [](const std::string& c) {
		return c == "DefaultDetonationComp" || c == "ReplenishDetonationComp";
	}) || std::any_of(t.settings().begin(), t.settings().end(), [](const auto& setting) {
		return setting.first.rfind("detonation.", 0) == 0;
	});
	if (s.detonates) {
		s.explosionDamage = number(t, "detonation.explosionDamage", 500);
		s.explosionRadius = number(t, "detonation.explosionRadius", 10);
		s.explosionMaterial = static_cast<int>(number(t, "detonation.explosionMaterial", -1));
		s.explosionForce = number(t, "detonation.explosionForce", 150);
		s.explosionYMod = number(t, "detonation.explosionYMod", 1);
	}
	// collision.bouncing (the collision template's +0xd, 0 by default): a
	// hit that doesn't go through (the game logic's hit 0x005a39d0 ->
	// projectile slot 51, 0x005b5390) leaves the round in the world when
	// it bounces (collision slot 17), else the round goes off (event 0x1f,
	// 0x005b62a0). Shipped 1: the frag and radar grenades, C4, the mines,
	// hubs, sonars, decoy, beacon, shield, eu_ifv_laserguided_mine; the EMP
	// grenade (unset) and the smoke grenade (0) go off where they land.
	s.bounces = number(t, "collision.bouncing", 0) != 0;
	s.sticky = std::find(components.begin(), components.end(), "StickyCollisionComp") != components.end();
	if (s.sticky) {
		// 0x005e8030: the angle clamped to +-180 degrees, kept as its cosine.
		const float angle = std::clamp(number(t, "collision.maxStickAngle", 180), -180.0f, 180.0f);
		s.maxStickCos = std::cos(angle * 3.14159265f / 180.0f);
		s.stickToTerrain = number(t, "collision.stickToTerrain", 1) != 0;
		s.stickToStaticObjects = number(t, "collision.stickToStaticObjects", 1) != 0;
		s.stickToVehicles = number(t, "collision.stickToVehicles", 1) != 0;
	}
	s.radarRadius = std::max(number(t, "ability.radarRadius", 0), 0.0f);
	s.radialRadar = number(t, "ability.radialRadar", 1) != 0;
	s.radarShowPlayers = number(t, "ability.showPlayers", 1) != 0;
	s.radarShowVehicles = number(t, "ability.showVehicles", 1) != 0;
	s.detonateDelay = number(t, "detonateDelay", 0);
	s.dudMaterial = static_cast<int>(number(t, "dudMaterial", -1));
	s.damageMandown = number(t, "damageMandownSoldiers", 0) != 0;
	s.resurrect = t.settings().count("collision.resurrectDamage") || t.settings().count("collision.restoreHP");
	s.resurrectDamage = number(t, "collision.resurrectDamage", 0);
	s.restoreHitPoints = number(t, "collision.restoreHP", 0.1f);
	s.emp = number(t, "detonation.isEMPWeapon", 0) != 0;
	if (const auto seek = t.settings().find("seek.targetType"); seek != t.settings().end()) {
		s.seek = seek->second == "TTMagnet" ? ProjectileSettings::Seek::Magnet
			: seek->second == "TTUnique"    ? ProjectileSettings::Seek::Aim
			: seek->second == "TTBeacon"    ? ProjectileSettings::Seek::Beacon
			: seek->second == "TTHeat" || seek->second == "TTLaser" ? ProjectileSettings::Seek::Closest
											: ProjectileSettings::Seek::None;
		// TargetType's names (the argument reader 0x004dce2e).
		static const char* const kTypes[] = {"TTNone", "TTLaser", "TTHeat", "TTMagnet", "TTUnique", "TTBeacon", "TTDecoy", "TTPod"};
		for (int i = 0; i < 8; ++i) {
			if (seek->second == kTypes[i]) {
				s.seekTargetType = i;
			}
		}
		s.reLockTicks = static_cast<int>(std::nearbyint(number(t, "seek.reLockTime", 0) * 30));
		s.seekDelay = number(t, "seek.trackingDelay", 0);
		s.lockDistance = number(t, "seek.maxDistLock", 0);
		s.loseDistance = number(t, "seek.distLoseLock", s.lockDistance);
		s.maxYawRate = number(t, "follow.maxYaw", 1);
		s.maxPitchRate = number(t, "follow.maxPitch", 1);
		s.followTime = number(t, "follow.followTime", 0);
		s.targetActivation = number(t, "follow.targetActivation", 0) != 0;
		s.armingSpeed = number(t, "follow.armingSpeed", 0);
		s.speedModifier = number(t, "follow.speedModifier", 1);
		s.predictionFactor = number(t, "follow.predictionFactor", 0.3f); // 0x005ed3b0's default
		s.useTargetSpeed = number(t, "follow.useTargetSpeed", 0) != 0;
		s.magneticBehaviour = number(t, "follow.magneticBehaviour", 0) != 0;
		s.maxAngleLock = number(t, "seek.maxAngleLock", 360);
		if (s.seek == ProjectileSettings::Seek::Closest) {
			// SeekClosestTargetComp's defaults (0x005ec300): maxAngleLock
			// pi/4, maxDistLock 9999, distLoseLock 0.
			s.maxAngleLock = number(t, "seek.maxAngleLock", 45);
			s.lockDistance = number(t, "seek.maxDistLock", 9999);
			s.loseDistance = number(t, "seek.distLoseLock", 0);
		}
		s.ignoreFriendly = number(t, "seek.ignoreFriendly", 1) != 0;
		s.directionBonus = number(t, "seek.directionBonus", 0);
	}
	// The GenericProjectile's own (+0x31c..+0x324, +0x335): rounds without a
	// seek have a motor too (Unl_AV_Rifle_Projectile, eu_rifle_missile).
	s.acceleration = number(t, "acceleration", 0);
	s.deAcceleration = number(t, "deAcceleration", 0);
	s.maxSpeed = number(t, "maxSpeed", 0);
	s.hasDirection = number(t, "hasDirection", 1) != 0;
	s.startDelay = number(t, "startDelay", 0);
	// armingDelay (the GenericProjectile's +0x2fc, its ticks): the seek and
	// the detonation component wait for it (0x005b3e60).
	s.armingDelay = number(t, "armingDelay", 0);
	s.onlyDetonateOnMaterial = static_cast<int>(number(t, "onlyDetonateOnMaterial", -1));
	s.onlyDetonateOnMaterialTime = number(t, "onlyDetonateOnMaterialTimeLimit", 0);
	s.penetrator = std::any_of(components.begin(), components.end(), [](const std::string& c) {
		return c == "DefaultPenetrateComp";
	}) || t.settings().count("penetrate.allowSolidPenetration") || t.settings().count("penetrate.allowLiquidPenetration");
	s.allowSolidPenetration = number(t, "penetrate.allowSolidPenetration", 1) != 0;
	s.allowLiquidPenetration = number(t, "penetrate.allowLiquidPenetration", 1) != 0;
	s.triggerRadius = number(t, "detonation.triggerRadius", 0);
	s.triggerAngle = number(t, "detonation.triggerAngle", 0); // 0x005e9df0
	s.radiusDetonateWithAirVehicles = number(t, "detonation.radiusDetonateWithAirVehicles", 0) != 0;
	s.triggerMinSpeed = number(t, "detonation.triggerVictimMinSpeed", 0);
	if (const auto type = t.settings().find("detonation.triggerType"); type != t.settings().end()) {
		static const char* const kTypes[] = {"MTYNone", "MTYPersonnel", "MTYVehicle", "MTYPco"};
		for (int i = 0; i < 4; ++i) {
			if (type->second == kTypes[i]) {
				s.triggerType = i;
			}
		}
	}
	s.allowFriendlyFireDetonation = number(t, "detonation.allowFriendlyFireDetonation", 0) != 0;
	s.coneAngle = number(t, "detonation.explosionConeAngle", 0);
	if (const auto effect = t.settings().find("detonation.endEffectTemplate"); effect != t.settings().end()) {
		s.endEffect = effect->second;
	}
	s.useMMOnEndEffect = number(t, "detonation.useMMOnEndEffect", 0) != 0;
	const auto ttl = t.settings().find("timeToLive");
	if (ttl != t.settings().end()) {
		s.timeToLive = RandomDistribution::parse(ttl->second);
	}
	s.onTimeEffect = number(t, "hasOnTimeEffect", 1) != 0;
	s.maxDepthForExplosion = number(t, "detonation.maxDepthForExplosion", 2);
	s.useCollisionNormal = number(t, "detonation.useCollisionNormal", 1) != 0;
	s.detonateDistanceToTarget = number(t, "detonation.detonateDistanceToTarget", 0);
	// The node's type (physicsType RotationalPoint, as every shipped round
	// that sets the offset names it).
	if (const auto type = t.settings().find("physicsType");
		type != t.settings().end() && sameText(type->second, "RotationalPoint")) {
		s.collisionOffset = vector3(t, "centerOfCollisionOffset");
	}
	s.remote = s.explosionMaterial >= 0 && s.explosionDamage > 0 && s.triggerRadius <= 0 && s.timeToLive.a > 1000; // CRD_NONE/9999
	return s;
}

std::string tracerGeometryFor(const world::ObjectTemplateManager& templates, const ProjectileSettings& settings)
{
	const world::ObjectTemplate* tracer = settings.tracerTemplate.empty() ? nullptr : templates.find(settings.tracerTemplate);
	return tracer ? tracer->geometry() : std::string();
}

float ProjectileSettings::damageAt(float distance) const
{
	const float least = minDamage == -1 ? damage : minDamage;
	if (distToStartLoseDamage <= 0 || distance <= distToStartLoseDamage) {
		return damage;
	}
	if (distance >= distToMinDamage) {
		return least;
	}
	return damage - (distance - distToStartLoseDamage) * ((damage - least) / (distToMinDamage - distToStartLoseDamage));
}

float explosionFalloff(float radius, float distance)
{
	const float inner = radius * 0.2f;
	if (distance < inner) {
		return 1;
	}
	return std::clamp(1 - (distance - inner) / (radius - inner), 0.0f, 1.0f);
}

namespace {

physics::Vec3 unitOf(physics::Vec3 v) // 0x004481a0
{
	const float squared = physics::lengthSquared(v);
	if (std::fabs(squared - 1.0f) < 1.1920929e-07f) {
		return v;
	}
	return squared < 1.1920929e-07f ? physics::Vec3 {} : v * (1.0f / std::sqrt(squared));
}

// The object's axis nearest the line, signed towards the object (y only
// when strictly nearest, else x unless z is nearer).
physics::Vec3 nearestAxis(const BlastPush& blast)
{
	const physics::Vec3& x = blast.axes.rows[0];
	const physics::Vec3& y = blast.axes.rows[1];
	const physics::Vec3& z = blast.axes.rows[2];
	const float alongY = physics::dot(blast.line, y), alongX = physics::dot(blast.line, x),
				alongZ = physics::dot(blast.line, z);
	if (std::fabs(alongY) > std::fabs(alongX) && std::fabs(alongY) > std::fabs(alongZ)) {
		return alongY < 0 ? -y : y;
	}
	if (std::fabs(alongZ) <= std::fabs(alongX)) {
		return alongX < 0 ? -x : x;
	}
	return alongZ < 0 ? -z : z;
}

} // namespace

float blastPushForce(const BlastPush& blast)
{
	// A twist: 1 x 20 instead of the falloff x forceMod (bVar5).
	float force = blast.twist ? 20.0f * blast.explosionForce * blast.share
							  : blast.falloff * blast.forceMod * blast.explosionForce * blast.share;
	if (blast.inWater && blast.soldier) {
		force *= 0.1f;
	}
	force = std::min(force, blast.forceMax);
	if (blast.soldier) {
		force *= blast.soldierRandom;
	}
	if (blast.forceMax < force + blast.speed) {
		force = blast.forceMax - blast.speed;
	}
	return force;
}

physics::Vec3 blastPush(const BlastPush& blast)
{
	const float force = blastPushForce(blast);
	physics::Vec3 way = unitOf(nearestAxis(blast));
	if (!blast.twist) {
		physics::Vec3 line = unitOf(blast.line);
		if (blast.soldier) {
			line.y += 5; // *0x00a6ab78 (0x00a6aac0)
		}
		way = way + line; // 0x0043dbd0, skipped for a twist
	}
	physics::Vec3 push = unitOf(way) * force;
	for (float* c : {&push.x, &push.y, &push.z}) {
		if (std::fabs(*c) < 0.001f) {
			*c = 0;
		}
	}
	return push;
}

std::array<BlastForce, 3> blastTwist(const physics::Mat3& axes, const physics::Vec3& line, float force, float width)
{
	const physics::Vec3& x = axes.rows[0];
	const physics::Vec3 a = physics::dot(line, x) >= 0 ? -x : x; // 0x00562dd0 negates
	const float f = force / 2.9f * std::max(width, 2.0f);          // *0x00a6aa94, *0x00a6aa98
	return {BlastForce {a, {0, f, 0}}, BlastForce {-a, {0, -0.5f * f, 0}}, // *0x00a6aa8c
		BlastForce {{}, a * (f * -0.5f)}};                                // *0x00a6aa90
}

float addEmpDamage(float level, float amount, float stepSize, int numSteps)
{
	return std::min(static_cast<float>(numSteps), std::ceil(level + amount / stepSize));
}

bool tiltAllowsFire(const physics::Mat3& root, float maxTilt)
{
	if (maxTilt == 0) {
		return true;
	}
	const float y = std::clamp(root.rows[1].y, -1.0f, 1.0f);
	return std::acos(y) * 57.29578f < maxTilt;
}

FireArmSettings FireArmSettings::fromTemplate(const world::ObjectTemplate& t)
{
	FireArmSettings s;
	s.roundsPerMinute = static_cast<int>(number(t, "fire.roundsPerMinute", static_cast<float>(s.roundsPerMinute)));
	s.magSize = static_cast<int>(number(t, "ammo.magSize", static_cast<float>(s.magSize)));
	s.nrOfMags = static_cast<int>(number(t, "ammo.nrOfMags", static_cast<float>(s.nrOfMags)));
	s.reloadTime = number(t, "ammo.reloadTime", s.reloadTime);
	// Camera-guided launchers author a minimum reload interval. Limit this
	// interpretation to those launchers: other weapons also use the field,
	// but its retail semantics there have not been established.
	if (number(t, "zoom.useProjectileCamera", 0) != 0) {
		s.reloadTime = std::max(s.reloadTime, number(t, "ammo.minimumTimeUntilReload", 0));
	}
	s.reloadWithoutPlayer = number(t, "ammo.reloadWithoutPlayer", 1) != 0;
	s.delayToUseTicks = static_cast<int>(std::nearbyint(number(t, "delayToUse", 0) * 30));
	s.autoReload = number(t, "ammo.autoReload", 0) != 0;
	s.instantReloadOnEnable = number(t, "ammo.instantReloadOnEnable", 0) != 0;
	s.toggleWhenNoAmmo = number(t, "ammo.toggleWhenNoAmmo", 0) != 0;
	s.changeMagAt = number(t, "ammo.changeMagAt", s.changeMagAt);
	s.busyUntilButtonRelease = number(t, "fire.busyUntilButtonRelease", 0) != 0;
	if (const auto delay = t.settings().find("fire.fireStartDelay"); delay != t.settings().end()) {
		s.fireStartDelay = RandomDistribution::parse(delay->second);
	}
	s.maximumVehicleTiltForFire = number(t, "fire.maximumVehicleTiltForFire", 0);
	s.keepProjectiles = number(t, "keepProjectiles", s.keepProjectiles);
	s.velocity = number(t, "velocity", s.velocity);
	s.heatAddWhenFire = number(t, "heatAddWhenFire", 0);
	s.coolDownPerSec = number(t, "coolDownPerSec", 0);
	s.overheatPenalty = number(t, "overheatPenalty", 0);
	const auto projectile = t.settings().find("projectileTemplate");
	if (projectile != t.settings().end()) {
		s.projectileTemplate = projectile->second;
	}
	// fire.addFireRate keeps each value in order ("fire.addFireRate.list" =
	// "0 2").
	if (const auto rates = t.settings().find("fire.addFireRate.list"); rates != t.settings().end()) {
		const char* p = rates->second.c_str();
		char* end = nullptr;
		for (long rate = std::strtol(p, &end, 10); end != p; rate = std::strtol(p, &end, 10)) {
			s.fireRates.push_back(static_cast<int>(rate));
			p = end;
		}
	}
	s.burstSize = static_cast<int>(number(t, "fire.burstSize", static_cast<float>(s.burstSize)));
	for (const std::string& c : t.components()) {
		if (c.size() == 18 && std::equal(c.begin(), c.end(), "PulseMeterAmmoComp", [](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			})) {
			s.pulseScan = true;
		}
		if (c.size() == 13 && std::equal(c.begin(), c.end(), "MultiFireComp", [](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			})) {
			s.multiFire = true;
		}
		if (c.size() == 14 && std::equal(c.begin(), c.end(), "ThrownFireComp", [](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			})) {
			s.thrown = true;
		}
	}
	if (s.thrown) {
		s.pullbackTicks = static_cast<int>(std::nearbyint(number(t, "fire.pullbackTime", 0.2f) * 30));
		s.launchDelayTicks = static_cast<int>(std::nearbyint(number(t, "fire.fireLaunchDelay", 0) * 30));
		const auto alt = t.settings().find("fire.altFireInput");
		s.softThrow = alt != t.settings().end() && !alt->second.empty();
		s.showAfterReloadTicks = static_cast<int>(std::nearbyint(number(t, "fire.showWeaponAfterReloadDelay", 0) * 30));
	}
	s.allowReloadWithFullMag = number(t, "ammo.allowReloadWithFullMag", 0) != 0;
	if (const auto link = t.settings().find("ammo.magLinkWeapon"); link != t.settings().end()) {
		s.magLinkWeapon = link->second;
	}
	s.allowZoomOnROF = number(t, "fire.allowZoomOnROF", 0) != 0;
	s.replenishing = std::any_of(t.components().begin(), t.components().end(), [](const std::string& c) {
		return c.size() == 20 && std::equal(c.begin(), c.end(), "ReplenishingAmmoComp", [](char a, char b) {
			return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
		});
	});
	s.abilityCost = s.replenishing ? number(t, "ammo.abilityCost", 0) : 0;
	return s;
}

FireArm::FireArm(FireArmSettings settings)
	: m_settings(std::move(settings)), m_rounds(m_settings.magSize), m_mags(m_settings.nrOfMags)
{
	m_mode = m_settings.fireRates.empty() ? kFireAutomatic : m_settings.fireRates.front();
}

bool FireArm::toggleFireMode()
{
	const std::vector<int>& rates = m_settings.fireRates;
	if (rates.size() < 2) {
		return false;
	}
	const auto at = std::find(rates.begin(), rates.end(), m_mode);
	m_mode = at == rates.end() || at + 1 == rates.end() ? rates.front() : *(at + 1);
	m_pending = false;
	m_burstLeft = 0;
	return true;
}

void FireArm::enable(bool soldier)
{
	m_readyIn = m_settings.delayToUseTicks;
	if (soldier || !m_settings.reloadWithoutPlayer) {
		m_reloadLeft = 0;
	}
	// canReload (0x005f4140): not full (allowReloadWithFullMag aside) and
	// a magazine to take, or magazines without end; a ReplenishingAmmoComp's
	// (0x005d79a0) the battery too.
	if (m_reloadLeft <= 0 && m_settings.autoReload && m_settings.instantReloadOnEnable && m_settings.magSize > 0
		&& m_rounds == 0 && (m_mags > 0 || m_settings.nrOfMags < 0) && batteryAllowsReload()) {
		m_rounds = m_settings.magSize;
		if (m_settings.nrOfMags >= 0) {
			--m_mags;
		}
	}
	m_pending = false;
	m_burstLeft = 0;
	m_hidden = false; // drawn (0x005c1650 sets flag 2)
	m_showIn = -1;
}

// A ReplenishingAmmoComp's "a magazine can be taken" (0x005d79a0, its
// slot 16 in place of 0x005f4140): the magazine empty (slot 0xcc,
// 0x005f41a0: rounds 0) and the holder's battery (slot 0x28, 0x005d8160)
// at least abilityCost. The magazines still count in the fill
// (0x005f46f0), so the callers keep that test.
bool FireArm::batteryAllowsReload() const
{
	if (!m_settings.replenishing || m_settings.abilityCost <= 0) {
		return true;
	}
	return m_rounds == 0 && m_battery >= m_settings.abilityCost;
}

void FireArm::reloadStarted()
{
	if (m_settings.thrown) {
		m_showIn = m_settings.showAfterReloadTicks; // 0x005e49d0
	}
}

void FireArm::shareMagazine(const FireArm& from, bool mags)
{
	if (mags && m_settings.nrOfMags >= 0 && from.m_mags <= m_settings.nrOfMags) {
		m_mags = from.m_mags; // slot 0xac (0x005c4900): refused above its limit
	}
	m_rounds = m_settings.magSize >= 0 ? std::min(from.m_rounds, m_settings.magSize) : from.m_rounds; // slot 0x50
}

bool FireArm::selectable() const
{
	return !(m_settings.toggleWhenNoAmmo && m_settings.magSize >= 0 && m_rounds <= 0 && m_reloadLeft <= 0
		&& !(m_mags > 0 || m_settings.nrOfMags < 0));
}

bool FireArm::startReload()
{
	if (m_readyIn > 0 || m_reloadLeft > 0 || m_settings.magSize < 0
		|| (m_rounds >= m_settings.magSize && !m_settings.allowReloadWithFullMag)
		|| (m_mags <= 0 && m_settings.nrOfMags >= 0) || m_settings.reloadTime <= 0 || !batteryAllowsReload()) {
		return false;
	}
	reloadStarted();
	m_reloadLeft = m_settings.reloadTime;
	m_reloadFilled = false;
	m_pending = false;
	m_burstLeft = 0;
	return true;
}

bool FireArm::takeTracer(int interval)
{
	if (--m_tracerCount >= 1) {
		return false;
	}
	m_tracerCount = interval;
	return true;
}

void CameraRecoil::start(float recoilSpeed, float recoilSize)
{
	speed = recoilSpeed;
	size = recoilSize;
	phase = 0;
	on = true;
}

float CameraRecoil::update(float seconds)
{
	if (!on) {
		phase = 0;
		return 0;
	}
	phase += seconds * speed * 1.0f; // *0x00a739c4
	if (phase > 1) {
		phase = 0;
		on = false;
	}
	return std::sin(phase * 6.2831855f) * size * 0.6f; // *0x00a739c0
}

GunRecoil GunRecoil::fromTemplate(const world::ObjectTemplate& t)
{
	GunRecoil r;
	const auto& c = t.components();
	if (std::find(c.begin(), c.end(), "WeaponBasedRecoilComp") == c.end()) {
		return r;
	}
	r.force = number(t, "recoil.recoilForce", 0);
	r.speed = number(t, "recoil.recoilSpeed", 0);
	r.speedGB = number(t, "recoil.recoilSpeedGB", 0);
	r.size = number(t, "recoil.recoilSize", 0);
	return r;
}

void GunRecoil::fired(uint32_t tick)
{
	// 0x005ddfc0: ROUND(+0x1c x -30 x 0.5) and ROUND(+0x20 x -30), the
	// x87's round to nearest.
	const float kick = speed != 0 ? 3.14f / speed : 0;      // +0x1c
	const float back = speedGB != 0 ? 1.57f / speedGB : 0;  // +0x20
	const long a = std::lrint(kick * -30.0f * 0.5f);
	const long b = std::lrint(back * -30.0f);
	kickEnd = static_cast<uint32_t>(static_cast<long>(tick) - a);
	returnEnd = static_cast<uint32_t>(static_cast<long>(kickEnd) - b);
}

float GunRecoil::slide(uint32_t tick) const
{
	if (!(speed > 0) || kickEnd == 0) {
		return 0;
	}
	const float kick = 3.14f / speed;
	if (tick <= kickEnd) {
		const float t = static_cast<float>(kickEnd - tick) * 0.033333335f;
		return size * 0.05f * 0.5f * std::sin((kick * 0.5f + t) * speed);
	}
	if (tick <= returnEnd) {
		const float t = static_cast<float>(returnEnd - tick) * 0.033333335f;
		const float back = speedGB != 0 ? 1.57f / speedGB : 0;
		return back > 0 ? size * 0.05f * 0.5f * (t / back) : 0;
	}
	return 0; // at rest (0x005ddd80 clears +0x5c)
}

TracerLook tracerLook(const ProjectileSettings& settings, const physics::Vec3& round, const physics::Vec3& velocity,
	const physics::Vec3& origin, const physics::Vec3& start, const physics::Vec3& offset, bool hit)
{
	TracerLook look;
	const float travelled = std::sqrt(physics::lengthSquared(round - origin));
	look.position = round;
	const float converge = settings.tracerConvergeDistance;
	if (!hit && converge > 0 && travelled <= converge) {
		look.position = round + offset * (1 - std::clamp(travelled / converge, 0.0f, 1.0f));
	}
	const float speed2 = physics::lengthSquared(velocity);
	if (speed2 <= 0.025f) {
		return look; // 0x005b3e60 keeps the last frame
	}
	const physics::Vec3 z = velocity * (1 / std::sqrt(speed2));
	physics::Vec3 x, y;
	if (std::fabs(z.x) >= 0.0001f || std::fabs(z.z) >= 0.0001f) {
		x = physics::cross({0, 1, 0}, z);
		x = x * (1 / std::sqrt(physics::lengthSquared(x)));
		y = physics::cross(z, x);
	} else {
		x = {1, 0, 0};
		y = {0, 0, z.y > 0 ? -1.0f : 1.0f};
	}
	const float s = settings.tracerSizeModifier;
	const float along = std::max(0.0f, physics::dot(round - start, z));
	const float length = std::min(settings.tracerScaler * s, along);
	if (length <= 0 || s <= 0) {
		return look;
	}
	look.axes.rows = {x * s, y * s, z * length};
	look.shown = true;
	return look;
}

std::vector<PlacedTracerLook> placedTracerLooks(const std::vector<Projectile>& projectiles)
{
	std::vector<PlacedTracerLook> out;
	for (const Projectile& projectile : projectiles) {
		if (!projectile.tracer || !projectile.alive || !projectile.settings
			|| projectile.settings->tracerGeometry.empty()) {
			continue;
		}
		TracerLook look = tracerLook(*projectile.settings, projectile.position, projectile.velocity, projectile.origin,
			projectile.tracerStart, projectile.tracerOffset, projectile.tracerHit);
		if (look.shown) {
			out.push_back({projectile.settings->tracerGeometry, look});
		}
	}
	return out;
}

std::vector<FireBundle> fireBundles(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& weapon,
	bool zoom)
{
	std::vector<FireBundle> out;
	for (const world::ChildTemplate& child : weapon.children()) {
		const world::ObjectTemplate* bundle = templates.find(child.name);
		if (!bundle || bundle->typeName() != "EffectBundle") {
			continue;
		}
		const bool zoomNamed = child.name.find("zoom") != std::string::npos || child.name.find("Zoom") != std::string::npos;
		auto start = bundle->settings().find("setStartOnEffects");
		if (start == bundle->settings().end()) {
			start = bundle->settings().find("startOnEffects");
		}
		const unsigned long startOnEffects = start != bundle->settings().end() ? std::strtoul(start->second.c_str(), nullptr, 10) : 0;
		if (zoomNamed == zoom && startOnEffects == 0) {
			out.push_back({child.name, {child.position.x, child.position.y, child.position.z},
				physics::Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z})});
		}
	}
	return out;
}

std::vector<ProjectileRenderState> projectileRenderStates(const world::ObjectTemplateManager& templates,
	const std::vector<Projectile>& projectiles, uint32_t hiddenSerial)
{
	std::vector<ProjectileRenderState> out;
	for (const Projectile& projectile : projectiles) {
		if (!projectile.alive || !projectile.settings || (hiddenSerial && projectile.serial == hiddenSerial)) {
			continue;
		}
		const world::ObjectTemplate* objectTemplate = templates.find(projectile.settings->name);
		if (!objectTemplate || objectTemplate->geometry().empty()) {
			continue;
		}
		physics::Mat3 rotation = physics::Mat3::fromYawPitchRoll({projectile.yaw, 0, 0});
		if (physics::lengthSquared(projectile.velocity) > 0.025f) {
			rotation = physics::lookAtRotation(projectile.velocity, rotation);
		} else if (projectile.stuck) {
			// Sticky rounds retain their contact normal; keep their local up on
			// that face while preserving the launch heading around its normal.
			physics::Vec3 up = projectile.stuckNormal;
			const float upLength = std::sqrt(physics::lengthSquared(up));
			if (upLength > 1e-5f) {
				up = up * (1 / upLength);
				physics::Vec3 forward {std::sin(projectile.yaw * 0.01745329252f), 0,
					std::cos(projectile.yaw * 0.01745329252f)};
				forward = forward - up * physics::dot(forward, up);
				const float forwardLength = std::sqrt(physics::lengthSquared(forward));
				if (forwardLength > 1e-5f) {
					forward = forward * (1 / forwardLength);
					physics::Vec3 right = physics::cross(up, forward);
					const float rightLength = std::sqrt(physics::lengthSquared(right));
					if (rightLength > 1e-5f) {
						right = right * (1 / rightLength);
						rotation.rows = {right, physics::cross(forward, right), forward};
					}
				}
			}
		}
		out.push_back({objectTemplate, projectile.position, rotation});
	}
	return out;
}

bool FireArm::addMagazine()
{
	if (m_settings.magSize < 0 || m_mags >= m_settings.nrOfMags) {
		return false;
	}
	++m_mags;
	if (m_rounds <= 0 && m_reloadLeft <= 0) {
		m_rounds = m_settings.magSize;
		--m_mags;
	}
	return true;
}

bool FireArm::resupply(float percent, uint32_t now)
{
	const int size = m_settings.magSize;
	const int mags = m_settings.nrOfMags;
	const bool rounds = mags == 1 || mags == -1; // the load is the magazine's rounds
	// slot 0x9c (0x005f4520): magSize -1 takes nothing; the magazine not full,
	// or a spare missing (native counts spares, not the loaded magazine).
	if (size < 0 || (rounds ? m_rounds >= size : m_mags >= mags)) {
		return false;
	}
	if (m_resupplyTick + 30 < now) {
		m_resupplyCredit = 0;
	}
	const float delta = static_cast<float>(rounds ? size : mags) * percent * 0.01f;
	m_resupplyCredit += delta;
	if (m_resupplyCredit > 1) { // strictly above, then truncated
		const int n = static_cast<int>(m_resupplyCredit);
		m_resupplyCredit -= static_cast<float>(n);
		if (rounds) {
			m_rounds = std::min(size, m_rounds + n);
		} else {
			m_mags = std::min(mags, m_mags + n);
		}
	}
	if (delta > 0) {
		m_resupplyTick = now;
	}
	return true;
}

bool FireArm::tick(float seconds, bool trigger, bool altTrigger)
{
	const bool altPressed = altTrigger && !m_altWas;
	m_altWas = altTrigger;
	// The pulse meter's scan runs on (0x005d99e0, after the ammo's update).
	if (m_scanning) {
		m_scan += seconds * kPulseScanStep;
		if (m_scan >= 1) {
			m_scan = 0;
			m_scanning = false;
		}
	}
	bool fired = false;
	// 0x005e5610: shown again once the ticks from the reload's start are past.
	if (m_showIn >= 0 && m_showIn-- == 0) {
		m_hidden = false;
	}
	if (m_launchIn >= 0 && m_soft) {
		// The soft throw held: the power grows, the timer waits.
		fire(seconds, false);
		if (altTrigger) {
			m_softTicks = std::min(m_softTicks + 1, 30);
			m_power = static_cast<float>(m_softTicks) / 30;
		} else {
			m_soft = false;
		}
	} else if (m_launchIn >= 0) {
		// 0x005e4c80: the thrown round leaves when its waits are through;
		// meanwhile the component is busy (no new round).
		fire(seconds, false);
		if (m_launchIn == 0) {
			m_launchIn = -1;
			fired = true;
		} else {
			--m_launchIn;
		}
	} else {
		const bool soft = m_settings.softThrow && altPressed && !trigger;
		fired = fire(seconds, trigger || soft);
		if (fired && m_settings.thrown) {
			m_power = 1;
			if (soft) {
				m_soft = true;
				m_softTicks = 0;
				m_power = 0;
				m_launchIn = std::max(m_settings.pullbackTicks + m_settings.launchDelayTicks - 1, 0);
				fired = false;
			} else if (m_settings.pullbackTicks + m_settings.launchDelayTicks > 0) {
				m_launchIn = m_settings.pullbackTicks + m_settings.launchDelayTicks - 1;
				fired = false;
			}
		}
	}
	if (fired && m_settings.pulseScan) {
		m_scanning = true; // 0x005d9cf0
	}
	if (fired && m_settings.thrown) {
		m_hidden = true; // the launch (0x005e5720) puts the firearm away
	}
	m_firedThisTick = fired;
	return fired;
}

bool FireArm::fire(float seconds, bool trigger)
{
	const bool pressed = trigger && !m_triggerWas;
	m_triggerWas = trigger;
	const int mode = m_useModes ? m_mode : kFireAutomatic;
	if (mode == kFireSingle && pressed) {
		m_pending = true;
	} else if (mode == kFireBurst && pressed && m_burstLeft <= 0) {
		m_burstLeft = std::max(m_settings.burstSize, 1);
	}
	// The heat cools only while the fire component is idle: the weapon update
	// (0x005c5d5e) calls the cooling (0x005c1fe0) for fire state 0 only, and
	// state 4 (a round's interval) still holds on the tick that ends it, before
	// the component's update. So a held trigger never cools.
	const bool inRoundInterval = m_cooldown > 0;
	m_sinceFire += seconds;
	if (m_cooldown > 0) {
		--m_cooldown;
	}
	if (m_reloadLeft > 0 || overheated()) {
		m_pending = false;
		m_burstLeft = 0;
	}
	if (mode == kFireSingle) {
		trigger = m_pending;
	} else if (mode == kFireBurst) {
		trigger = m_burstLeft > 0;
	}
	// busyUntilButtonRelease (SingleFireComp template +0x9c; the rocket,
	// AA and AV launchers): after a round the weapon stays busy (the
	// update 0x005d4800 pushes weapon +0x228 on by wp-delay-ticks) until the
	// trigger is released (slot 12, 0x005d3c20, sets +0x164): no next
	// round, no reload.
	if (m_busyUntilRelease) {
		if (m_triggerWas) {
			return false;
		}
		m_busyUntilRelease = false;
	}
	if (m_readyIn > 0) {
		--m_readyIn; // delayToUse: busy (0x005c14f0)
		m_pending = false;
		m_burstLeft = 0;
		if (m_reloadLeft <= 0) {
			return false;
		}
	}
	if (m_reloadLeft > 0) {
		m_reloadLeft -= seconds;
		// The swap at changeMagAt of the reload (0x005f4830 / 0x005f3c40),
		// busy on to its end.
		if (!m_reloadFilled && m_reloadLeft <= m_settings.reloadTime * (1 - m_settings.changeMagAt) + 1e-4f) {
			m_reloadFilled = true;
			m_rounds = m_settings.magSize;
			if (m_settings.nrOfMags >= 0) {
				--m_mags;
			}
		}
		if (m_reloadLeft <= 0) {
			m_reloadLeft = 0;
		}
		return false;
	}
	// heat = the heat at the last round - the time since it x coolDownPerSec.
	if (!inRoundInterval) {
		m_heat = std::max(0.0f, m_heatAtFire - m_settings.coolDownPerSec * m_sinceFire);
	}
	if (overheated()) {
		return false;
	}
	// A magazine size of -1: no magazine (vehicle guns that heat up instead).
	const bool unlimited = m_settings.magSize < 0;
	if (!unlimited && m_rounds <= 0) {
		m_pending = false;
		m_burstLeft = 0;
		// The fire attempt (0x005c2ee0) on a press: the ammo component's
		// slot 0x34 (0x005f37c0: a round in the magazine, not state 2)
		// refuses, the trigger is let go (0x005c1010) and the sound
		// component's click plays (0x005dc160: TriggerClick, first person).
		if (pressed) {
			m_clicked = true;
		}
		// With ammo.autoReload the ammo's update (0x005f3c40) starts a
		// reload of an empty magazine by itself once the firearm isn't busy
		// (0x005c14f0) and a magazine can be taken (0x005f4140): +0x43, which
		// the firearm's update (0x005c58a0, slot 0xc8) turns into the
		// reload (0x005c0f40). A magazine given to an empty gun (a resupply)
		// is so taken in.
		if (m_settings.autoReload && m_cooldown <= 0 && (m_mags > 0 || m_settings.nrOfMags < 0)
			&& m_settings.reloadTime > 0 && batteryAllowsReload()) {
			reloadStarted();
			m_reloadLeft = m_settings.reloadTime;
			m_reloadFilled = false;
			return false;
		}
	}
	// fireStartDelay: the spin-up (see FireArmSettings).
	if (m_startDelay >= 0) {
		if (!m_triggerWas) {
			m_startDelay = -1; // released: dropped (0x005f1630)
			m_pending = false;
			m_burstLeft = 0;
			return false;
		}
		if (m_startDelay > 0) {
			--m_startDelay;
			return false;
		}
		m_startDelay = -1; // through: the round goes (0x005f18b0)
	} else if (pressed && trigger && m_settings.fireStartDelay.a != 0 && !m_settings.multiFire && m_cooldown <= 0
		&& (unlimited || m_rounds > 0) && (mode != kFireBurst || m_burstLeft >= std::max(m_settings.burstSize, 1))) {
		const int ticks = static_cast<int>(std::nearbyint(30 * std::max(0.0f, m_settings.fireStartDelay.sample(m_startRandom))));
		if (ticks > 0) {
			m_startDelay = ticks - 1;
			return false;
		}
	}
	if (!trigger || m_cooldown > 0 || (!unlimited && m_rounds <= 0)) {
		return false;
	}
	m_pending = false;
	if (m_burstLeft > 0) {
		--m_burstLeft;
	}
	m_cooldown = m_settings.ticksBetweenShots();
	m_busyUntilRelease = m_settings.busyUntilButtonRelease;
	if (m_settings.heatAddWhenFire > 0) {
		// The batch fire (0x005c3cb0): over 1 the heat becomes overheatPenalty
		// and cools back down to 1 (the gun stops while it is 1 or more).
		m_heat += m_settings.heatAddWhenFire;
		if (m_heat > 1) {
			m_heat = m_settings.overheatPenalty;
		}
		m_heatAtFire = m_heat;
		m_sinceFire = 0;
	}
	if (unlimited) {
		return true;
	}
	--m_rounds;
	// A ReplenishingAmmoComp's round (0x005d81d0): the battery pays
	// abilityCost for it.
	if (m_settings.replenishing && m_settings.abilityCost > 0) {
		m_batteryCost += m_settings.abilityCost;
		m_battery = std::max(0.0f, m_battery - m_settings.abilityCost);
	}
	// The round's ammo use (0x005f45e0, slot 0x38): an emptied magazine sets
	// +0x41, and +0x44 only with ammo.autoReload (+0x28); after the round
	// (0x005f4a40, slot 0x3c) +0x44 with a magazine to take (0x005f4140)
	// starts the reload (state 2, 0x005f4830). Without autoReload (the
	// rifles, handguns and launchers) nothing starts: the magazine stays
	// empty, the trigger clicks (0x005c2ee0), until c_PIReload (the
	// firearm's action 10). ammo.nrOfMags -1: magazines without end (the
	// drones' detonators).
	if (m_rounds == 0 && m_settings.autoReload && (m_mags > 0 || m_settings.nrOfMags < 0) && batteryAllowsReload()) {
		reloadStarted();
		m_reloadLeft = m_settings.reloadTime;
		m_reloadFilled = false;
	}
	return true;
}

float Gaussian::next(Random& random)
{
	if (m_has) {
		m_has = false;
		return m_kept;
	}
	float u = 0, v = 0, s = 0;
	do {
		u = random.unit() * 2 - 1;
		v = random.unit() * 2 - 1;
		s = u * u + v * v;
	} while (s > 1 || s == 0);
	const float f = std::sqrt(-2 * std::log(s) / s);
	m_kept = f * v;
	m_has = true;
	return f * u;
}

DeviationBuildUp DeviationBuildUp::fromTemplate(const world::ObjectTemplate& t)
{
	DeviationBuildUp d;
	if (const auto it = t.settings().find("deviation.setFireDev"); it != t.settings().end()) {
		std::sscanf(it->second.c_str(), "%f %f %f", &d.max, &d.perShot, &d.decay);
	}
	if (const auto it = t.settings().find("deviation.minDev"); it != t.settings().end()) {
		d.minDev = std::strtof(it->second.c_str(), nullptr);
	}
	return d;
}

void DeviationBuildUp::update(bool firing)
{
	if (!firing) {
		value -= decay;
		if (value < 0) {
			value = 0;
		}
	}
}

void DeviationBuildUp::updateSoldier(bool firing)
{
	if (!firing) {
		value -= decay;
		if ((perShot > 0 && value < 0) || (perShot < 0 && value > 0)) {
			value = 0;
		}
	}
}

void DeviationBuildUp::fired()
{
	value += perShot;
	if (perShot > 0 && value > max) {
		value = max;
	} else if (perShot < 0 && value < max) {
		value = max;
	}
}

physics::Vec3 deviateVelocity(const physics::Vec3& velocity, const physics::Vec3& right, const physics::Vec3& up, float speed,
	float deviation, float alongUp, float alongRight)
{
	if (deviation <= 0.01f) { // 0x0091f9e0, also the scale
		return velocity;
	}
	const float scale = speed * 0.01f * deviation;
	return velocity + up * (scale * alongUp) + right * (scale * alongRight);
}

void armFollow(Projectile& p, float targetSpeed, bool overrideArming)
{
	const ProjectileSettings& s = *p.settings;
	if (targetSpeed > p.targetTopSpeed || (targetSpeed < s.armingSpeed && !p.armed)) {
		p.targetTopSpeed = targetSpeed;
	}
	p.armed = p.targetTopSpeed > s.armingSpeed || overrideArming;
}

float followSpeedCap(const Projectile& p, bool overrideArming)
{
	const ProjectileSettings& s = *p.settings;
	return s.useTargetSpeed && !overrideArming ? (p.armed ? s.speedModifier : 1.0f) * p.targetTopSpeed : s.maxSpeed;
}

void followMagnet(Projectile& p, const physics::Vec3& aim, const physics::Vec3& targetVelocity, float seconds, bool overrideArming)
{
	const ProjectileSettings& s = *p.settings;
	armFollow(p, std::sqrt(physics::lengthSquared(targetVelocity)), overrideArming);
	const float ownSpeed = std::sqrt(physics::lengthSquared(p.velocity));
	physics::Vec3 point = aim;
	if (ownSpeed > 0) {
		point = point + targetVelocity * (std::sqrt(physics::lengthSquared(aim - p.position)) / ownSpeed * s.predictionFactor);
	}
	physics::Vec3 direction = point - p.position;
	const float length = std::sqrt(physics::lengthSquared(direction));
	direction = length > 1e-4f ? direction * (1 / length) : physics::Vec3 {};
	const float cap = followSpeedCap(p, overrideArming);
	if (ownSpeed >= cap) {
		if (ownSpeed > 0.01f) {
			p.velocity = p.velocity - p.velocity * (std::min(s.deAcceleration * seconds, ownSpeed) / ownSpeed);
		}
	} else {
		p.velocity = p.velocity + direction * (s.acceleration * seconds);
		p.resting = false;
	}
}

bool followActive(const Projectile& p)
{
	const ProjectileSettings& s = *p.settings;
	const float from = p.followFrom >= 0 ? p.followFrom : s.seekDelay;
	return p.age >= from && (s.followTime <= 0 || p.age <= from + s.followTime);
}

bool followStep(Projectile& p, const physics::Vec3& target, const physics::Vec3& targetVelocity, float seconds, bool newTarget)
{
	const ProjectileSettings& s = *p.settings;
	const float speed = std::sqrt(physics::lengthSquared(p.velocity));
	physics::Vec3 aim = target;
	if (speed > 0 && s.predictionFactor != 0) {
		aim = aim + targetVelocity * (std::sqrt(physics::lengthSquared(target - p.position)) / speed * s.predictionFactor);
	}
	physics::Vec3 d = aim - p.position;
	const float length = std::sqrt(physics::lengthSquared(d));
	if (length < 1e-4f || speed < 1e-4f) {
		return false;
	}
	d = d * (1 / length);
	const physics::Vec3 forward = p.velocity * (1 / speed);
	physics::Vec3 right {forward.z, 0, -forward.x}; // (0, 1, 0) x forward
	const float r = std::sqrt(physics::lengthSquared(right));
	right = r > 1e-5f ? right * (1 / r) : physics::Vec3 {1, 0, 0};
	const physics::Vec3 up {forward.y * right.z - forward.z * right.y, forward.z * right.x - forward.x * right.z,
		forward.x * right.y - forward.y * right.x}; // forward x right
	const float x = physics::dot(d, right), y = physics::dot(d, up), z = physics::dot(d, forward);
	const float ticks = seconds * 30.0f;
	const float maxYaw = s.maxYawRate * ticks, maxPitch = s.maxPitchRate * ticks;
	constexpr float kDegrees = 57.29578f;
	float yaw = 0, pitch = 0; // degrees: right positive, up positive
	if (z > 0) {
		const bool uncapped = s.magneticBehaviour && newTarget;
		yaw = std::fabs(std::asin(std::clamp(x, -1.0f, 1.0f))) * kDegrees;
		pitch = std::fabs(std::asin(std::clamp(y, -1.0f, 1.0f))) * kDegrees;
		if (!uncapped) {
			yaw = std::min(yaw, maxYaw);
			pitch = std::min(pitch, maxPitch);
		}
	} else if (s.magneticBehaviour) {
		yaw = maxYaw;
		pitch = maxPitch;
	} else {
		return false;
	}
	yaw = x < 0 ? -yaw : yaw;
	pitch = y < 0 ? -pitch : pitch;
	// About its right (the pitch), then about its up as turned (the yaw).
	const float a = pitch / kDegrees, b = yaw / kDegrees;
	const physics::Vec3 pitched = forward * std::cos(a) + up * std::sin(a);
	const physics::Vec3 turned = pitched * std::cos(b) + right * std::sin(b);
	const float t = std::sqrt(physics::lengthSquared(turned));
	p.velocity = turned * (speed / (t > 0 ? t : 1));
	return true;
}

physics::Vec3 bounceVelocity(const physics::Vec3& velocity, const physics::Vec3& normal, float elasticity, float friction)
{
	const physics::Vec3 normalPart = normal * physics::dot(velocity, normal);
	return (velocity - normalPart) * (1 - std::min(friction, 1.0f)) - normalPart * elasticity;
}

void moveProjectile(Projectile& p, float seconds, float gravity)
{
	// The Point node's tick (0x006c6170 / 0x006c57a0, physics::PointBody):
	// its drag, then the mean of the old and new velocity moves it; gravity
	// for the next tick.
	physics::PointBody node;
	node.position = p.position;
	node.velocity = p.velocity;
	node.acceleration = p.acceleration;
	if (p.settings) {
		node.drag = p.settings->drag;
		node.mass = p.settings->mass;
		node.gravityModifier = p.settings->gravityModifier;
	}
	node.step(seconds, gravity);
	p.position = node.position;
	p.velocity = node.velocity;
	p.acceleration = node.acceleration;
	p.timeLeft -= seconds;
	if (p.timeLeft <= 0) {
		p.alive = false;
	}
}

} // namespace dice::hfe::game

namespace dice::hfe::game {

namespace {

float settingOr(const world::ObjectTemplate& t, const char* name, float fallback)
{
	const auto it = t.settings().find(name);
	return it == t.settings().end() ? fallback : std::strtof(it->second.c_str(), nullptr);
}

bool hasComponent(const world::ObjectTemplate& t, const char* name)
{
	for (const std::string& c : t.components()) {
		if (c.size() == std::strlen(name)) {
			bool same = true;
			for (size_t i = 0; i < c.size() && same; ++i) {
				same = std::tolower(static_cast<unsigned char>(c[i])) == std::tolower(static_cast<unsigned char>(name[i]));
			}
			if (same) {
				return true;
			}
		}
	}
	return false;
}

// A held value's step (0x005de980's pattern): `add` while active, held at
// `max` (decaying back to it when above); else decaying to 0.
void accumulate(float& value, bool active, float add, float decay, float max)
{
	if (!active) {
		value -= decay;
		if (value < 0) {
			value = 0;
		}
	} else if (max <= value) {
		value -= decay;
		if (value < max) {
			value = max;
		}
	} else {
		value += add;
		if (max < value) {
			value = max;
		}
	}
}

} // namespace

SoldierDeviation SoldierDeviation::fromTemplate(const world::ObjectTemplate& t)
{
	SoldierDeviation d;
	d.present = hasComponent(t, "SoldierDeviationComp");
	d.fire = DeviationBuildUp::fromTemplate(t);
	if (const auto it = t.settings().find("deviation.setTurnDev"); it != t.settings().end()) {
		std::sscanf(it->second.c_str(), "%f %f %f %f", &d.turnMax, &d.turnPitch, &d.turnYaw, &d.turnDecay);
	}
	if (const auto it = t.settings().find("deviation.setSpeedDev"); it != t.settings().end()) {
		std::sscanf(it->second.c_str(), "%f %f %f %f", &d.speedMax, &d.speedThrottle, &d.speedStrafe, &d.speedDecay);
	}
	if (const auto it = t.settings().find("deviation.setMiscDev"); it != t.settings().end()) {
		std::sscanf(it->second.c_str(), "%f %f %f", &d.miscMax, &d.miscAdd, &d.miscDecay);
	}
	d.modStand = settingOr(t, "deviation.devModStand", 1);
	d.modCrouch = settingOr(t, "deviation.devModCrouch", 1);
	d.modLie = settingOr(t, "deviation.devModLie", 1);
	d.modZoom = settingOr(t, "deviation.devModZoom", 1);
	d.minModZoom = settingOr(t, "deviation.minDevModZoom", 1);
	return d;
}

void SoldierDeviation::update(float throttle, float strafe, float lookX, float lookY, bool inMisc, bool firing,
	int stance, bool zoomed)
{
	// The speed's part (+0x28): a step for moving, one for strafing.
	float moving = 0;
	if (std::fabs(throttle) > 0.01f) {
		moving = speedThrottle;
	}
	if (std::fabs(strafe) > 0.01f) {
		moving += speedStrafe;
	}
	accumulate(speed, moving != 0, moving, speedDecay, speedMax);
	// The turn's part (+0x24): by how much the look turns, less the decay.
	float turning = 0;
	if (std::fabs(lookY) > 0.01f) {
		turning = std::fabs(lookY) * turnPitch;
	}
	if (std::fabs(lookX) > 0.01f) {
		turning += std::fabs(lookX) * turnYaw;
	}
	accumulate(turn, turning != 0, turning - turnDecay, turnDecay, turnMax);
	// The misc part (+0x2c).
	accumulate(misc, inMisc && miscAdd != 0, miscAdd - miscDecay, miscDecay, miscMax);
	fire.updateSoldier(firing);
	const float stanceMod = stance == 1 ? modCrouch : stance == 2 ? modLie : modStand;
	const float zoom = zoomed ? modZoom : 1;
	const float minZoom = zoomed ? minModZoom : 1;
	total = (fire.minDev * minZoom + misc + speed + turn + fire.value) * zoom * stanceMod;
}

GunSway GunSway::fromTemplate(const world::ObjectTemplate& t)
{
	GunSway s;
	s.present = hasComponent(t, "DefaultGunSwayComp");
	s.useZoom = settingOr(t, "sway.gunSwayUseZoom", 0) != 0;
	s.zoomModifier = settingOr(t, "sway.gunSwayZoomModifier", 1);
	s.recoverTime = settingOr(t, "sway.stabilityRecoverTime", 0);
	s.stabilityTime = settingOr(t, "sway.stabilityTime", 0);
	s.duration = settingOr(t, "sway.stabilityDuration", 0);
	s.durationRecoverTime = settingOr(t, "sway.stabilityDurationRecoverTime", 0);
	for (const char* axis : {"Pitch", "Lr"}) {
		Axis& a = std::string(axis) == "Pitch" ? s.pitch : s.lr;
		const std::string p = std::string("sway.gunSway") + axis;
		a.steps = settingOr(t, (p + "Steps").c_str(), 0);
		a.factor = settingOr(t, (p + "Factor").c_str(), 0);
		a.max = settingOr(t, (p + "Max").c_str(), 0);
		a.updateRate = settingOr(t, (p + "UpdateRate").c_str(), 0);
		a.dirModifier = settingOr(t, (p + "DirModifier").c_str(), 0);
		a.dirThreshold = settingOr(t, (p + "DirThreshold").c_str(), 0);
		a.randomMin = settingOr(t, (p + "RandomMin").c_str(), 0);
		a.randomMax = settingOr(t, (p + "RandomMax").c_str(), 0);
		a.smoothFactor = settingOr(t, (p + "SmoothFactor").c_str(), 0);
		a.smoothMax = settingOr(t, (p + "SmoothMax").c_str(), 0);
		a.smoothThreshold = settingOr(t, (p + "SmoothThreshold").c_str(), 0);
	}
	return s;
}

namespace {

// One axis's step (0x005e09a0 / 0x005e0ba0). `peakFlag` is the flag the
// turn at the swing's peak sets: the pitch's own, and (as BF2142.exe has
// it, 0x005e0ba0 writing +0x58) the pitch's for the left-right axis too.
void swayAxis(GunSway::Axis& a, bool& peakFlag, float seconds, float limit, float stability, float zoomModifier,
	float& input, const std::function<float()>& random01)
{
	if (a.max <= 0) {
		a.swing = 0;
		return;
	}
	const float step = a.max / a.steps - a.smooth;
	if (a.updateRate <= a.timer) {
		a.timer = 0;
		const float amount = stability * a.direction * a.factor * zoomModifier * step * seconds;
		if (std::fabs(input) < std::fabs(amount)) {
			input += amount;
		}
		a.swing += step;
		a.total += step;
	}
	if (a.max + a.extra <= a.swing && a.total < limit - step) {
		a.swing = 0;
		a.direction = -a.direction;
		if (!a.keepGoing) {
			peakFlag = random01() * a.dirModifier < a.dirThreshold; // 0x0042b2c0(0, DirModifier)
		}
	}
	if (limit < a.total) {
		a.swing = 0;
		a.total = 0;
		if (!a.keepGoing) {
			a.direction *= a.dirThreshold <= random01() * a.dirModifier ? 1.0f : -1.0f;
		} else {
			a.direction = -a.direction;
			a.smooth = a.smoothMax;
		}
		a.extra = (a.randomMax - a.randomMin) * random01() + a.randomMin;
	}
}

// The smoothing near a swing's ends (0x005dfc20).
void swaySmooth(GunSway::Axis& a, float limit)
{
	const float f = a.smoothFactor, x = a.total, t = a.smoothThreshold;
	const float peak = a.max + a.extra;
	if (f != 0 && (x <= t || (peak - t < x && x < peak + t) || limit - t <= x)) {
		if (x < t) {
			a.smooth -= f;
			if (a.smooth < f) {
				a.smooth = f;
			}
			if (!a.keepGoing) {
				a.smooth = 0;
			}
		}
		if (peak - t < x && x < peak) {
			a.smooth = std::min(a.smooth + f, a.smoothMax);
			a.keepGoing = false;
		}
		if (peak < x && x < peak + t) {
			a.smooth -= f;
			if (a.smooth < f) {
				a.smooth = f;
			}
		}
		if (x <= limit - t || limit <= x) {
			return;
		}
		a.smooth = std::min(a.smooth + f, a.smoothMax);
		if (a.keepGoing) {
			return;
		}
	}
	a.smooth = 0;
}

} // namespace

void GunSway::update(float seconds, bool zoomed, bool sprintHeld, bool hasStabilizer, float& lookX, float& lookY,
	const std::function<float()>& random01)
{
	if (!present) {
		return;
	}
	// The stability (0x005e0750).
	if (!steadying) {
		stability = recoverTime <= 0 ? 1.0f : std::min(1.0f, stability + seconds / recoverTime);
		used = durationRecoverTime > 0 ? std::max(0.0f, used - seconds / durationRecoverTime) : 0.0f;
	} else {
		stability = stabilityTime > 0 ? std::max(0.0f, stability - seconds / stabilityTime) : 0.0f;
		used = duration > 0 ? std::min(duration, used + seconds / duration) : duration;
	}
	if (useZoom && !zoomed) {
		return;
	}
	// Steadying (0x005e0da0): the sprint key, with the scope stabilizer.
	if (hasStabilizer) {
		if (sprintHeld && !latched && used == 0) {
			steadying = latched = true;
		}
		if (!sprintHeld) {
			steadying = false;
			if (used == 0) {
				latched = false;
			}
		}
		if (steadying && duration <= used) {
			steadying = false;
		}
	}
	pitch.timer += seconds;
	const float pitchLimit = 2 * (pitch.extra + pitch.max);
	swayAxis(pitch, pitch.keepGoing, seconds, pitchLimit, stability, zoomModifier, lookY, random01);
	lr.timer += seconds;
	const float lrLimit = 2 * (lr.max + lr.extra);
	swayAxis(lr, pitch.keepGoing, seconds, lrLimit, stability, zoomModifier, lookX, random01);
	swaySmooth(pitch, pitchLimit);
	swaySmooth(lr, lrLimit);
}

} // namespace dice::hfe::game
