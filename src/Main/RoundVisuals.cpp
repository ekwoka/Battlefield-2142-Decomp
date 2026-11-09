#include "Main/RoundVisuals.h"

#include <cmath>

namespace dice::hfe::client {

physics::Mat3 roundFrame(const game::Projectile& round)
{
	constexpr float kDegrees = 57.29578f;
	// The facing it was thrown with (a mine's yaw, GameWorld's radius
	// trigger frame).
	physics::Mat3 frame = physics::Mat3::fromYawPitchRoll({round.yaw, 0, 0});
	if (round.stuck) {
		return game::stuckFrame(frame, round.stuckNormal);
	}
	if (round.resting) {
		return frame;
	}
	const physics::Vec3 nose = game::noseOf(round);
	if (physics::lengthSquared(nose) < 0.25f) {
		return frame;
	}
	return physics::Mat3::fromYawPitchRoll({std::atan2(nose.x, nose.z) * kDegrees,
		-std::atan2(nose.y, std::sqrt(nose.x * nose.x + nose.z * nose.z)) * kDegrees, 0});
}

bool roundEngineOn(const game::Projectile& round)
{
	const game::ProjectileSettings& s = *round.settings;
	if (s.targetActivation) {
		return round.lockedOn != 0 || round.lockedBait != 0; // 0x005b3e60: 0x005b2c10 / 0x005b2ca0
	}
	return round.age >= s.startDelay; // 0x005b3990: at once, or event 0 after startDelay (0x005b62a0)
}

const RoundVisuals::Look& RoundVisuals::lookOf(const game::Projectile& round)
{
	auto found = m_looks.find(round.settings);
	if (found == m_looks.end()) {
		Look look;
		if (round.settings) {
			look.objectTemplate = m_templates.find(round.settings->name);
		}
		if (look.objectTemplate) {
			look.geometry = !look.objectTemplate->geometry().empty();
			look.bundles = !effects::ObjectEffects::bundlesOf(m_templates, *look.objectTemplate, 0, {}, &m_bundleCache).empty();
		}
		found = m_looks.emplace(round.settings, look).first;
	}
	return found->second;
}

void RoundVisuals::appendEffectStates(const std::vector<game::Projectile>& rounds,
	std::vector<effects::ObjectEffects::ObjectState>& states)
{
	std::map<uint32_t, bool> engines;
	for (const game::Projectile& round : rounds) {
		// A server's projectile ghost's round: its object carries the bundles.
		if (!round.alive || !round.settings || round.serial == 0 || round.ghostObject != 0) {
			continue;
		}
		const Look& look = lookOf(round);
		if (!look.objectTemplate || !look.bundles) {
			continue;
		}
		effects::ObjectEffects::ObjectState state;
		state.id = kRoundAnchor | (round.serial & 0x0fffffffu);
		state.objectTemplate = look.objectTemplate;
		state.position = round.position;
		state.rotation = roundFrame(round);
		state.velocity = round.velocity;
		const bool on = roundEngineOn(round);
		const auto before = m_engines.find(round.serial);
		const bool was = before != m_engines.end() && before->second;
		state.startEvent = on && !was;
		state.stopEvent = !on && was;
		engines[round.serial] = on;
		states.push_back(state);
	}
	m_engines = std::move(engines);
}

void RoundVisuals::meshes(const std::vector<game::Projectile>& rounds, std::vector<Mesh>& out)
{
	for (const game::Projectile& round : rounds) {
		if (!round.alive || !round.settings || round.ghostObject != 0) {
			continue;
		}
		const Look& look = lookOf(round);
		if (!look.geometry) {
			continue;
		}
		out.push_back({look.objectTemplate, round.position, roundFrame(round)});
	}
}

} // namespace dice::hfe::client
