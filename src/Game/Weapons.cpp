#include "Weapons.h"

#include <algorithm>

#include "World/ObjectTemplate.h"

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
	s.material = static_cast<int>(number(t, "material", 0));
	s.explosionDamage = number(t, "detonation.explosionDamage", 0);
	s.explosionRadius = number(t, "detonation.explosionRadius", 0);
	s.explosionMaterial = static_cast<int>(number(t, "detonation.explosionMaterial", 0));
	// Thrown objects (grenades, mines, C4, hubs) have physicsType
	// RotationalPoint or Mesh and a collision mesh of their own; rounds and
	// missiles are Points (they set hasCollisionPhysics too).
	if (const auto type = t.settings().find("physicsType"); type != t.settings().end()) {
		s.bounces = type->second == "RotationalPoint" || type->second == "Mesh";
	}
	s.damageMandown = number(t, "damageMandownSoldiers", 0) != 0;
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
		s.chaseSpeed = number(t, "follow.armingSpeed", 7) * number(t, "follow.speedModifier", 1);
	}
	s.triggerRadius = number(t, "detonation.triggerRadius", 0);
	s.triggerAngle = number(t, "detonation.triggerAngle", 180);
	s.triggerMinSpeed = number(t, "detonation.triggerVictimMinSpeed", 0);
	s.coneAngle = number(t, "detonation.explosionConeAngle", 0);
	if (s.explosionDamage > 0 && s.explosionRadius <= 0 && !s.emp) {
		s.explosionRadius = 10; // C4 sets none (provisional: the engine's default is not traced)
	}
	if (const auto effect = t.settings().find("detonation.endEffectTemplate"); effect != t.settings().end()) {
		s.endEffect = effect->second;
	}
	const auto ttl = t.settings().find("timeToLive");
	if (ttl != t.settings().end()) {
		s.timeToLive = RandomDistribution::parse(ttl->second);
	}
	s.remote = s.explosionDamage > 0 && s.triggerRadius <= 0 && s.timeToLive.a > 1000; // CRD_NONE/9999
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

void moveProjectile(Projectile& p, float seconds, float gravity)
{
	const physics::Vec3 previous = p.velocity;
	p.velocity += p.acceleration * seconds;
	p.position += (previous + p.velocity) * (0.5f * seconds);
	p.acceleration = {0, gravity * (p.settings ? p.settings->gravityModifier : 1.0f), 0};
	p.timeLeft -= seconds;
	if (p.timeLeft <= 0) {
		p.alive = false;
	}
}

} // namespace dice::hfe::game
