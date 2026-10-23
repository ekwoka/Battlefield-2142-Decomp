#include "SoldierControl.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::game {

namespace {

constexpr float kSpeeds[7] = {0.8f, 2.0f, 1.5f, 3.9f, 6.3f, 2.1f, 3.645f};
constexpr float kEaseIn = 0.1f;  // 0x00a68988
constexpr float kEaseOut = 0.2f; // 0x00a6898c
constexpr float kTwistLimit = 40;     // *0x00a68eb0 (static_init 0x008e05e0; +4 is the pitch's 85)
constexpr float kCatchUpPerTick = 4;  // *0x00a68ee0 (static_init 0x008e05c0)

float shape(float x)
{
	x = std::clamp(x, -1.0f, 1.0f);
	const float shaped = (x > 0 ? 1.0f : x < 0 ? -1.0f : 0.0f) * x * x;
	return std::fabs(shaped) < 0.001f ? 0.0f : shaped;
}

float ease(float current, float target)
{
	const float next = (target - current) * (target == 0 ? kEaseOut : kEaseIn) + current;
	return std::fabs(next) < 0.001f ? 0.0f : next;
}

} // namespace

float soldierSpeed(int index) { return kSpeeds[std::clamp(index, 0, 6)]; }

int soldierSpeedIndex(SoldierPose pose, bool sprinting)
{
	switch (pose) {
	case SoldierPose::Crouch:
		return 1;
	case SoldierPose::Prone:
		return 0;
	case SoldierPose::Swim:
		return sprinting ? 6 : 5;
	case SoldierPose::Stand:
	default:
		// (Index 2, 1.5 m/s, is for a slowed state not rewritten yet.)
		return sprinting ? 4 : 3;
	}
}

void SoldierControl::update(const SoldierInput& input, physics::SoldierBody& body)
{
	// The look (0x0058a6d0): a turn under 0.001 counts as none. Standing
	// (the shaped throttle within 0.01 of 0; strafing doesn't count), the
	// turn twists the head (+0x278) and the body stays while the twist is
	// within 40 degrees (*0x00a68eb0); at or past it the body turns with the
	// turn and catches the whole twist up (+0x280). Moving, any twist is
	// caught up. Catching up turns the body 4 degrees a call (*0x00a68ee0)
	// towards it and takes that off the twist. (Past the limit the body
	// also takes the turn itself while the twist keeps it, so that tick the
	// look moves twice the turn: as the original.)
	const float turn = std::fabs(input.turn) < 0.001f ? 0.0f : input.turn;
	float bodyTurn = turn; // local_2c
	if (std::fabs(shape(input.throttle)) <= 0.01f) {
		m_twist += turn;
		if (m_twist < kTwistLimit && m_twist > -kTwistLimit) {
			bodyTurn = 0;
		} else {
			m_catchUp = m_twist;
		}
	} else if (m_twist != 0) {
		m_catchUp = m_twist;
	}
	if (m_catchUp != 0) {
		float step = m_catchUp > 0 ? kCatchUpPerTick : -kCatchUpPerTick;
		if (step <= 0) {
			step = std::max(step, m_catchUp);
		} else {
			step = std::min(step, m_catchUp);
		}
		m_catchUp -= step;
		m_twist -= step;
		bodyTurn += step;
	}
	body.yawDegrees += bodyTurn;
	m_throttle = ease(m_throttle, shape(input.throttle));
	m_strafe = ease(m_strafe, shape(input.strafe));
	// Heading (+z forward when yaw is 0, +x to the right).
	const float yaw = body.yawDegrees * 3.14159265f / 180.0f;
	const physics::Vec3 forward {std::sin(yaw), 0, std::cos(yaw)};
	const physics::Vec3 right {std::cos(yaw), 0, -std::sin(yaw)};
	physics::Vec3 direction = forward * m_throttle + right * m_strafe;
	const float length = std::sqrt(m_throttle * m_throttle + m_strafe * m_strafe);
	if (length > 1.0f) {
		direction = direction * (1.0f / length);
	}
	body.setDesiredVelocity(direction * soldierSpeed(soldierSpeedIndex(input.pose, input.sprint)));
}

} // namespace dice::hfe::game
