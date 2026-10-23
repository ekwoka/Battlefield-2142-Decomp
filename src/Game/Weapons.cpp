#include "Weapons.h"

#include "World/ObjectTemplate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dice::hfe::game {

namespace {

float number(const world::ObjectTemplate& t, const char* name, float fallback)
{
	const auto it = t.settings().find(name);
	return it == t.settings().end() ? fallback : static_cast<float>(std::atof(it->second.c_str()));
}

} // namespace

ProjectileSettings ProjectileSettings::fromTemplate(const world::ObjectTemplate& t)
{
	ProjectileSettings s;
	s.damage = number(t, "damage", s.damage);
	s.minDamage = number(t, "minDamage", s.minDamage);
	s.distToStartLoseDamage = number(t, "distToStartLoseDamage", s.distToStartLoseDamage);
	s.distToMinDamage = number(t, "distToMinDamage", s.distToMinDamage);
	s.gravityModifier = number(t, "gravityModifier", s.gravityModifier);
	s.drag = number(t, "drag", s.drag);
	s.mass = std::max(number(t, "mass", s.mass), 0.001f);
	s.material = static_cast<int>(number(t, "material", 0));
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
	// Thrown objects (grenades, mines, C4, hubs) have physicsType
	// RotationalPoint or Mesh and a collision mesh of their own; rounds and
	// missiles are Points (they set hasCollisionPhysics too).
	if (const auto type = t.settings().find("physicsType"); type != t.settings().end()) {
		s.bounces = type->second == "RotationalPoint" || type->second == "Mesh";
	}
	s.damageMandown = number(t, "damageMandownSoldiers", 0) != 0;
	s.resurrect = t.settings().count("collision.resurrectDamage") || t.settings().count("collision.restoreHP");
	s.resurrectDamage = number(t, "collision.resurrectDamage", 0);
	s.restoreHitPoints = number(t, "collision.restoreHP", 0.1f);
	s.emp = number(t, "detonation.isEMPWeapon", 0) != 0;
	if (const auto seek = t.settings().find("seek.targetType"); seek != t.settings().end()) {
		s.seek = seek->second == "TTMagnet" ? ProjectileSettings::Seek::Magnet
			: seek->second == "TTUnique"    ? ProjectileSettings::Seek::Aim
											: ProjectileSettings::Seek::None;
		s.seekDelay = number(t, "seek.trackingDelay", 0);
		s.lockDistance = number(t, "seek.maxDistLock", 0);
		s.loseDistance = number(t, "seek.distLoseLock", s.lockDistance);
		s.maxYawRate = number(t, "follow.maxYaw", 1);
		s.maxPitchRate = number(t, "follow.maxPitch", 1);
		s.armingDelay = number(t, "armingDelay", 0);
		s.armingSpeed = number(t, "follow.armingSpeed", 0);
		s.speedModifier = number(t, "follow.speedModifier", 1);
		s.predictionFactor = number(t, "follow.predictionFactor", 0);
		s.useTargetSpeed = number(t, "follow.useTargetSpeed", 0) != 0;
		s.magneticBehaviour = number(t, "follow.magneticBehaviour", 0) != 0;
		s.acceleration = number(t, "acceleration", 0);
		s.deAcceleration = number(t, "deAcceleration", 0);
		s.maxSpeed = number(t, "maxSpeed", 0);
		s.maxAngleLock = number(t, "seek.maxAngleLock", 360);
		s.ignoreFriendly = number(t, "seek.ignoreFriendly", 1) != 0;
	}
	s.triggerRadius = number(t, "detonation.triggerRadius", 0);
	s.triggerAngle = number(t, "detonation.triggerAngle", 180);
	s.triggerMinSpeed = number(t, "detonation.triggerVictimMinSpeed", 0);
	s.coneAngle = number(t, "detonation.explosionConeAngle", 0);
	if (const auto effect = t.settings().find("detonation.endEffectTemplate"); effect != t.settings().end()) {
		s.endEffect = effect->second;
	}
	const auto ttl = t.settings().find("timeToLive");
	if (ttl != t.settings().end()) {
		s.timeToLive = RandomDistribution::parse(ttl->second);
	}
	s.remote = s.explosionMaterial >= 0 && s.explosionDamage > 0 && s.triggerRadius <= 0 && s.timeToLive.a > 1000; // CRD_NONE/9999
	return s;
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

physics::Vec3 blastPush(const BlastPush& blast)
{
	const auto unit = [](physics::Vec3 v) { // 0x004481a0
		const float squared = physics::lengthSquared(v);
		if (std::fabs(squared - 1.0f) < 1.1920929e-07f) {
			return v;
		}
		return squared < 1.1920929e-07f ? physics::Vec3 {} : v * (1.0f / std::sqrt(squared));
	};
	float force = blast.falloff * blast.forceMod * blast.explosionForce * blast.share;
	if (blast.inWater && blast.soldier) {
		force *= 0.1f;
	}
	force = std::min(force, blast.forceMax);
	const physics::Vec3& x = blast.axes.rows[0];
	const physics::Vec3& y = blast.axes.rows[1];
	const physics::Vec3& z = blast.axes.rows[2];
	const float alongY = physics::dot(blast.line, y), alongX = physics::dot(blast.line, x),
				alongZ = physics::dot(blast.line, z);
	physics::Vec3 way;
	if (std::fabs(alongY) > std::fabs(alongX) && std::fabs(alongY) > std::fabs(alongZ)) {
		way = alongY < 0 ? -y : y;
	} else if (std::fabs(alongZ) <= std::fabs(alongX)) {
		way = alongX < 0 ? -x : x;
	} else {
		way = alongZ < 0 ? -z : z;
	}
	way = unit(way);
	physics::Vec3 line = unit(blast.line);
	if (blast.soldier) {
		line.y += 5; // *0x00a6ab78 (0x00a6aac0)
		force *= blast.soldierRandom;
	}
	if (blast.forceMax < force + blast.speed) {
		force = blast.forceMax - blast.speed;
	}
	physics::Vec3 push = unit(way + line) * force;
	for (float* c : {&push.x, &push.y, &push.z}) {
		if (std::fabs(*c) < 0.001f) {
			*c = 0;
		}
	}
	return push;
}

float addEmpDamage(float level, float amount, float stepSize, int numSteps)
{
	return std::min(static_cast<float>(numSteps), std::ceil(level + amount / stepSize));
}

FireArmSettings FireArmSettings::fromTemplate(const world::ObjectTemplate& t)
{
	FireArmSettings s;
	s.roundsPerMinute = static_cast<int>(number(t, "fire.roundsPerMinute", static_cast<float>(s.roundsPerMinute)));
	s.magSize = static_cast<int>(number(t, "ammo.magSize", static_cast<float>(s.magSize)));
	s.nrOfMags = static_cast<int>(number(t, "ammo.nrOfMags", static_cast<float>(s.nrOfMags)));
	s.reloadTime = number(t, "ammo.reloadTime", s.reloadTime);
	s.velocity = number(t, "velocity", s.velocity);
	s.heatAddWhenFire = number(t, "heatAddWhenFire", 0);
	s.coolDownPerSec = number(t, "coolDownPerSec", 0);
	s.overheatPenalty = number(t, "overheatPenalty", 0);
	const auto projectile = t.settings().find("projectileTemplate");
	if (projectile != t.settings().end()) {
		s.projectileTemplate = projectile->second;
	}
	return s;
}

FireArm::FireArm(FireArmSettings settings)
	: m_settings(std::move(settings)), m_rounds(m_settings.magSize), m_mags(m_settings.nrOfMags)
{
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

bool FireArm::tick(float seconds, bool trigger)
{
	if (m_cooldown > 0) {
		--m_cooldown;
	}
	if (m_reloadLeft > 0) {
		m_reloadLeft -= seconds;
		if (m_reloadLeft <= 0) {
			m_reloadLeft = 0;
			m_rounds = m_settings.magSize;
			if (m_settings.nrOfMags >= 0) {
				--m_mags;
			}
		}
		return false;
	}
	m_heat = std::max(0.0f, m_heat - m_settings.coolDownPerSec * seconds);
	if (m_overheatLeft > 0) {
		m_overheatLeft = std::max(0.0f, m_overheatLeft - seconds);
		return false;
	}
	// A magazine size of -1: no magazine (vehicle guns that heat up instead).
	const bool unlimited = m_settings.magSize < 0;
	if (!trigger || m_cooldown > 0 || (!unlimited && m_rounds <= 0)) {
		return false;
	}
	m_cooldown = m_settings.ticksBetweenShots();
	if (m_settings.heatAddWhenFire > 0) {
		m_heat += m_settings.heatAddWhenFire;
		if (m_heat >= 1) {
			m_heat = 1;
			m_overheatLeft = m_settings.overheatPenalty;
		}
	}
	if (unlimited) {
		return true;
	}
	--m_rounds;
	// ammo.nrOfMags -1: magazines without end (the drones' detonators).
	if (m_rounds == 0 && (m_mags > 0 || m_settings.nrOfMags < 0)) {
		m_reloadLeft = m_settings.reloadTime;
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

void followMagnet(Projectile& p, const physics::Vec3& aim, const physics::Vec3& targetVelocity, float seconds)
{
	const ProjectileSettings& s = *p.settings;
	const float targetSpeed = std::sqrt(physics::lengthSquared(targetVelocity));
	if (targetSpeed > p.targetTopSpeed || (targetSpeed < s.armingSpeed && !p.armed)) {
		p.targetTopSpeed = targetSpeed;
	}
	p.armed = p.targetTopSpeed > s.armingSpeed;
	const float ownSpeed = std::sqrt(physics::lengthSquared(p.velocity));
	physics::Vec3 point = aim;
	if (ownSpeed > 0) {
		point = point + targetVelocity * (std::sqrt(physics::lengthSquared(aim - p.position)) / ownSpeed * s.predictionFactor);
	}
	physics::Vec3 direction = point - p.position;
	const float length = std::sqrt(physics::lengthSquared(direction));
	direction = length > 1e-4f ? direction * (1 / length) : physics::Vec3 {};
	float cap = s.maxSpeed;
	if (s.useTargetSpeed) {
		cap = (p.armed ? s.speedModifier : 1.0f) * p.targetTopSpeed; // 0x005ec770, 0x005ec7a0
	}
	if (ownSpeed >= cap) {
		if (ownSpeed > 0.01f) {
			p.velocity = p.velocity - p.velocity * (std::min(s.deAcceleration * seconds, ownSpeed) / ownSpeed);
		}
	} else {
		p.velocity = p.velocity + direction * (s.acceleration * seconds);
		p.resting = false;
	}
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
