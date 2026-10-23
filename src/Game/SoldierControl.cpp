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

void SprintStamina::configure(float dissipationTime, float recoverTime, float limit, float useModifier,
	float rechargeModifier)
{
	m_configured = true;
	m_dissipationTime = dissipationTime;
	m_recoverTime = recoverTime;
	m_limit = limit;
	m_useModifier = useModifier;
	m_rechargeModifier = rechargeModifier;
}

void SprintStamina::request(bool held)
{
	m_requested = true;
	if (held && !m_sprinting) {
		m_requested = false;
	}
}

void SprintStamina::update(bool blocked, float recoverDelay, float seconds)
{
	// Blocked as well without a dissipation time (0x006457f0's first test).
	const bool cannot = !(m_dissipationTime > 1.1920929e-07f) || blocked;
	if (!m_sprinting) {
		if (m_recoverTime <= 0) {
			m_stamina = 1;
		} else if (recoverDelay <= 0) {
			m_stamina = std::min(1.0f, seconds / m_recoverTime * m_rechargeModifier + m_stamina);
		}
		if (m_requested && m_limit <= m_stamina && !cannot) {
			m_requested = false;
			m_sprinting = true; // event 0x2a
			return;
		}
	} else {
		m_stamina = std::max(0.0f, m_stamina - m_drain / m_dissipationTime * seconds * m_useModifier);
		if (!m_requested || m_stamina <= 0 || cannot) {
			m_sprinting = false; // event 0x2b
		}
	}
	m_requested = false;
}

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

void SoldierControl::update(const SoldierInput& input, physics::SoldierBody& body, float seconds)
{
	// The timers: the air's (0x0058a6d0's start) to -1 when they run out,
	// the others (0x00586710) down to 0.
	for (float* timer : {&m_slopeDelay, &m_airTime}) {
		if (*timer > 0) {
			*timer -= seconds;
			if (*timer <= 0) {
				*timer = -1;
			}
		}
	}
	for (float* timer : {&m_jumpTime, &m_noProne, &m_getUp, &m_recoverDelay}) {
		if (*timer > 0) {
			*timer -= seconds;
		}
	}
	// Up from prone (0x005889d0, pose 2 to 0 or 1): a second before a jump.
	if (m_lastPose == SoldierPose::Prone && input.pose != SoldierPose::Prone && input.pose != SoldierPose::Swim) {
		m_getUp = jump::kGetUpTime;
	}
	m_lastPose = input.pose;
	// The sprint (0x00645760 on the key's events, 0x006457f0 each tick from
	// 0x0058d350): crouching or prone it can't (in a vehicle neither, but
	// then this isn't called). Without the template's numbers, sprinting
	// is the key.
	if (input.sprint) {
		m_stamina.request(m_sprintHeld);
	}
	m_sprintHeld = input.sprint;
	m_stamina.update(input.pose == SoldierPose::Crouch || input.pose == SoldierPose::Prone, m_recoverDelay, seconds);
	const bool sprinting = m_stamina.configured() ? m_stamina.sprinting() : input.sprint;
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
	// Heading (+z forward when yaw is 0, +x to the right).
	const float yaw = body.yawDegrees * 3.14159265f / 180.0f;
	const physics::Vec3 forward {std::sin(yaw), 0, std::cos(yaw)};
	const physics::Vec3 right {std::cos(yaw), 0, -std::sin(yaw)};
	const float speed = soldierSpeed(soldierSpeedIndex(input.pose, sprinting)); // +0x310 (0x00586020)
	// The jump (0x0058a6d0): PIAction under 0.001 is none; none while the
	// get-up time runs or swimming, nor unless standing.
	m_jumped = false;
	const bool action = input.jump && m_getUp <= 0 && !body.swimming() && input.pose == SoldierPose::Stand;
	if (action && body.onGround()) {
		m_jumpTime = jump::kJumpTime;
		m_noProne = jump::kNoProneTime;
		m_recoverDelay = jump::kRecoverDelay;
		m_airTime = jump::kAirTime;
		if (body.groundNormal().y < jump::kSteep) {
			m_slopeDelay = jump::kSlopeDelay;
		}
		// The velocity along the heading and across it (y left out, each
		// direction normalised), x 0.98, at most the speed index's speed
		// (0x00586df0(7)) x 0.98; then 6 m/s up.
		const physics::Vec3& v = body.velocity;
		physics::Vec3 leap = forward * (physics::dot(forward, v) * jump::kCarry)
			+ right * (physics::dot(right, v) * jump::kCarry);
		const float length = std::sqrt(physics::lengthSquared(leap));
		const float most = speed * jump::kCarry;
		if (length > most) {
			leap = leap * (most / length);
		}
		leap.y = jump::kUpSpeed;
		body.jump(leap);
		m_stamina.lose(jumpLoss);
		m_jumped = true;
	}
	// 0x00588420: the inputs eased towards the shaped ones, their direction
	// at most 1 long.
	m_throttle = ease(m_throttle, shape(input.throttle));
	m_strafe = ease(m_strafe, shape(input.strafe));
	physics::Vec3 direction = forward * m_throttle + right * m_strafe;
	const float length = std::sqrt(m_throttle * m_throttle + m_strafe * m_strafe);
	if (length > 1.0f) {
		direction = direction * (1.0f / length);
	}
	if (body.onGround() || body.swimming()) {
		body.setDesiredVelocity(direction * speed); // x *0x00a68f08 (1)
		return;
	}
	// In the air: steering while the jump's air time runs (after the slope
	// delay): the direction x the time left x 0.5 x 2 m/s (*0x00a68eb4)
	// added to the velocity (node slot 0x60), the ground speed then held
	// to what it was if that was over the steering speed and grew.
	body.setDesiredVelocity({});
	if (m_slopeDelay < 0 && m_airTime > 0) {
		const float steer = m_airTime * 0.5f * jump::kAirSpeed;
		const physics::Vec3 before = body.velocity;
		const float was = std::sqrt(before.x * before.x + before.z * before.z);
		physics::Vec3 after = before + physics::Vec3 {direction.x * steer, 0, direction.z * steer};
		const float now = std::sqrt(after.x * after.x + after.z * after.z);
		if (steer < was && was < now) {
			after.x *= was / now;
			after.z *= was / now;
			after.y = before.y;
		}
		body.velocity = after;
	}
}

} // namespace dice::hfe::game
