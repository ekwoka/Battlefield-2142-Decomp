#include "AI/Heli.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::ai {

namespace {

float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float clamp1(float x) { return std::clamp(x, -1.0f, 1.0f); }
float sign(float x) { return x > 0 ? 1.0f : x < 0 ? -1.0f : 0.0f; }
// The holdHeight terms' shape: sign(e) x min(1, e^2).
float signedSquare(float e) { return sign(e) * std::min(1.0f, e * e); }
constexpr float kDegrees = 57.29578f;

// 0x100ffda0: a direction's heading, degrees (the look-at 0x101003e0 puts
// the direction in the matrix's row 2).
float heading(const Vec3& d) { return std::atan2(-d.x, d.z) * kDegrees; }

Vec3 flat(const Vec3& v)
{
	const float l = std::sqrt(v.x * v.x + v.z * v.z);
	return l > 1e-5f ? Vec3 {v.x / l, 0, v.z / l} : Vec3 {0, 0, 1};
}

} // namespace

HeliPilot::HeliPilot(HeliState& state, const HeliSense& sense) : m_state(state), m_sense(sense)
{
	const Vec3& w = sense.angularVelocity;
	m_rate = {dot(sense.right, w), dot(sense.up, w), dot(sense.forward, w)};
}

float HeliPilot::heightAboveGround(const Vec3& p) const
{
	return m_sense.terrain ? p.y - m_sense.terrain(p.x, p.z) : p.y;
}

float HeliPilot::heightAboveGround() const { return heightAboveGround(m_sense.position); }

// 0x100e4440. Axis 0 (pitch): the nose's height forward.y, its rate the
// pitch rate; axis 2 (roll): the right wing's right.y and the roll rate.
//   e = clamp((target - current) / 0.02 [0x1012ad44]) x scale, negated for
//       the pitch
//   out = clamp((e - bias - rate) / 0.5 [0x1012ad40]) x gain
// PIPitch = out; PIRoll = -out.
void HeliPilot::attitude(int axis, float target, float gain, float scale, float bias)
{
	const float current = axis == 0 ? m_sense.forward.y : m_sense.right.y;
	const float rate = axis == 0 ? m_rate.x : m_rate.z;
	float e = clamp1((target - current) / 0.02f) * scale;
	if (axis == 0) {
		e = -e;
	}
	const float out = clamp1((e - bias - rate) / 0.5f) * gain;
	if (axis == 0) {
		m_state.controls.pitch = out;
	} else {
		m_state.controls.roll = -out;
	}
}

// 0x100e4960: only at |v| >= deadband; k = min(1, (|v| - deadband) x 0.2),
// the nose held at (deadband 0: 0.5 [0x1012ad5c], else 0.3 [0x1012ad54])
// x sign(v) x k^2, gain 1 [0x1012ad58] (the bias global 0x10139548 is 0).
void HeliPilot::brake(float v, float deadband, float scale)
{
	if (std::fabs(v) < deadband) {
		return;
	}
	const float k = std::min(1.0f, (std::fabs(v) - deadband) * 0.2f);
	const float target = (deadband == 0 ? 0.5f : 0.3f) * sign(v) * k * k;
	attitude(0, target, 1.0f, scale);
}

// 0x100e4550: diff = wrap180(heading(dir) - heading(forward)); PIYaw =
// clamp((-1.5 [0x1012ad50] x clamp(diff / 30) - yaw rate) / 0.1
// [0x1012ad4c]) x 1 [0x1012ad48]. (Its debug flag 0x10139544 is never set.)
float HeliPilot::yawTo(const Vec3& dir)
{
	const float diff = std::remainder(heading(dir) - heading(m_sense.forward), 360.0f);
	m_state.controls.yaw = clamp1((-1.5f * clamp1(diff / 30.0f) - m_rate.y) / 0.1f);
	return std::fabs(diff);
}

// 0x100e4130:
//   airborne once over 5 m above the ground (bot vt 0x17c)
//   wantVy = 5 [0x1012ad38] x clamp((wantY - y) / 5 [0x1012ad3c])
//   PIThrottle = clamp((wantVy - v.y) / 2 [0x1012ad34], 0, 1), 1 while it
//                should climb and falls with the hull upright
//   PIPitch = sq((0.5 [0x1012ad2c] x clamp(forward.y / 0.3 [0x1012ad30]) -
//             pitch rate) / 0.6 [0x1012ad28])
//   PIRoll = -sq((-0.5 x clamp(right.y / 0.3) - roll rate) / 0.6)
//   PIYaw = 0 (the bias global 0x10139540 is 0)
void HeliPilot::holdHeight(float wantY)
{
	if (!m_state.airborne && heightAboveGround() > 5) {
		m_state.airborne = true;
	}
	const float wantVy = 5.0f * clamp1((wantY - m_sense.position.y) / 5.0f);
	float throttle = std::clamp(clamp1((wantVy - m_sense.velocity.y) / 2.0f), 0.0f, 1.0f);
	if (wantVy > 0 && m_sense.velocity.y < 0 && m_sense.up.y > 0) {
		throttle = 1;
	}
	HeliControls& c = m_state.controls;
	c.throttle = throttle;
	c.pitch = signedSquare((0.5f * clamp1(m_sense.forward.y / 0.3f) - m_rate.x) / 0.6f);
	c.roll = -signedSquare((-0.5f * clamp1(m_sense.right.y / 0.3f) - m_rate.z) / 0.6f);
	c.yaw = 0;
}

// 0x100e4a70 (its debug flag 0x1013954c is 0):
//   tilted = |right.y| > 0.2 [0x1012ad84] or |forward.y| > 0.6
//   s = max(4, forward . v); untilted, the probes every 5 m ahead, n = 2 +
//   round(0.01 s^2), at least 5 (under 6: 5), give the highest terrain and
//   the least clearance; nothing to do unless that is under 5 m at s > 4.
// Taking over with the least clearance -10 [0x1012ad7c] or more, or
// tilted: level the wings, brake (deadband 3 [0x1012ad64], scale 0.3
// [0x1012ad68] x min(1, |fs| / 10 [0x1012ad6c]) going forward + 0.1
// [0x1012ad60]) and with the nose up full collective. Otherwise (a wall
// ahead): PIYaw 0.5, bank to the side with more room (0x100e0be0: 12 terrain
// samples every 22 m along 0.7 forward +- 0.1 right each side, the larger
// summed clearance) by attitude(2, -+0.2 [0x1012ad70], 0.5 [0x1012ad74],
// 0.5 [0x1012ad78]); with that side's least clearance over -10, brake
// (5, 0.3) and full collective.
bool HeliPilot::terrainCheck(float& maxTerrain)
{
	maxTerrain = -99999;
	const Vec3& f = m_sense.forward;
	const bool tilted = std::fabs(m_sense.right.y) > 0.2f || std::fabs(f.y) > 0.6f;
	const float s = std::max(4.0f, dot(f, m_sense.velocity));
	float minClear = 1e9f;
	if (!tilted) {
		int n = 2 + static_cast<int>(std::lround(0.01f * s * s));
		if (n < 6) {
			n = 5;
		}
		for (int i = 0; i < n; ++i) {
			const Vec3 p = m_sense.position + f * (5.0f * static_cast<float>(i));
			const float t = m_sense.terrain ? m_sense.terrain(p.x, p.z) : 0.0f;
			maxTerrain = std::max(maxTerrain, t);
			minClear = std::min(minClear, p.y - t);
		}
		if (!(minClear < 5 && s > 4)) {
			return false;
		}
	}
	HeliControls& c = m_state.controls;
	const float fs = dot(f, m_sense.velocity);
	if (tilted || minClear >= -10) {
		c.roll = -clamp1((0.5f * clamp1(-m_sense.right.y / 0.02f) - m_rate.z) / 0.5f);
		brake(fs, 3, 0.3f * (fs > 0.1f ? std::min(1.0f, std::fabs(fs) / 10.0f) : 0.0f) + 0.1f);
		if (f.y > 0) {
			c.throttle = 1;
		}
		return true;
	}
	c.yaw = 0.5f;
	float sum[2] = {}, least[2] = {1e9f, 1e9f};
	for (int side = 0; side < 2; ++side) {
		Vec3 d = f * 0.7f + m_sense.right * (side == 0 ? 0.1f : -0.1f);
		d.y = 0;
		const Vec3 step = flat(d) * 22.0f;
		for (int i = 1; i <= 12; ++i) {
			const Vec3 p = m_sense.position + step * static_cast<float>(i);
			const float clear = p.y - (m_sense.terrain ? m_sense.terrain(p.x, p.z) : 0.0f);
			sum[side] += clear;
			least[side] = std::min(least[side], clear);
		}
	}
	const int side = sum[0] >= sum[1] ? 0 : 1; // 0: right
	attitude(2, side == 0 ? -0.2f : 0.2f, 0.5f, 0.5f);
	if (least[side] > -10) {
		brake(fs, 5, 0.3f);
		c.throttle = 1;
	}
	return true;
}

// 0x100e4fa0: d = goal - position; attitude(2, -0.25 [0x1012ad88] x
// clamp(d . right / 20 [0x1012ad90]), 0.15 [0x1012ad8c], 0.2) and
// attitude(0, -0.25 x clamp(d . forward / 20), 0.15, 0.2).
void HeliPilot::hoverAt(const Vec3& goal)
{
	const Vec3 d = goal - m_sense.position;
	attitude(2, -0.25f * clamp1(dot(d, m_sense.right) / 20.0f), 0.15f, 0.2f);
	attitude(0, -0.25f * clamp1(dot(d, m_sense.forward) / 20.0f), 0.15f, 0.2f);
}

// 0x100e50b0. The cruise is a fixed 35 m/s [0x1012ada8], not the Mobile
// maxSpeed; at 5 m or lower over the ground it only climbs, level.
void HeliPilot::flyTo(const Vec3& goal, float margin)
{
	const Vec3 fwdFlat = flat(m_sense.forward);
	Vec3 toGoal = goal - m_sense.position;
	toGoal.y = 0;
	const float dist = std::sqrt(toGoal.x * toGoal.x + toGoal.z * toGoal.z);
	const Vec3 dir = flat(toGoal);
	const float fs = dot(fwdFlat, m_sense.velocity);
	const float fa = dot(fwdFlat, m_sense.acceleration);
	float maxTerrain = 0;
	if (terrainCheck(maxTerrain)) {
		return;
	}
	const float wantY = std::max(goal.y, maxTerrain + margin);
	holdHeight(wantY);
	const float below = wantY - m_sense.position.y;
	const float k = below < 5 ? 0.0f : std::min(1.0f, (below - 5) / 10.0f); // [0x1012adc4] 5, [0x1012adc0] 15
	if (dist < 20 && k < 0.1f) { // [0x1012adbc]
		const float deadband = dist < 5 ? 3.0f : 3.0f + 2.0f * (dist - 5) / 15.0f; // [0x1012adb8], [0x1012adb4], [0x1012adb0]
		if (std::fabs(fs) <= deadband) {
			hoverAt(goal);
		} else {
			brake(fs, deadband, 0.3f);
		}
		return;
	}
	if (heightAboveGround() > 5) {
		if (yawTo(dir) > 45) {
			brake(fs, 5, 0.3f);
			return;
		}
		const float wantV = (1 - 0.9f * k) * 35.0f * std::min(1.0f, 0.02f * dist); // [0x1012adac] 0.9
		const float a = clamp1((wantV - fs) / 10.0f);                              // [0x1012ada4]
		const float b = std::clamp((15.0f * a - fa) / 6.0f, -0.1f, 1.0f);         // [0x1012ada0], [0x1012ad9c]
		const float p = -clamp1((-0.4f * b - m_sense.forward.y) / 0.2f) - std::clamp(4 * m_rate.x, -1.2f, 1.2f); // [0x1012ad98]
		m_state.controls.pitch = clamp1(p);
	}
}

// 0x100e5590 (HeliAimAt's controller; the margin +0x1b0, 40). Too steep
// (dir.y at most -0.6 [0x1012add0] or 0.35 [0x1012adcc] or more) it flies
// on 150 m to come back; lined up within 10 degrees and turning under 0.1
// rad/s, the nose follows the aim: attitude(0, dir.y, 1, 0.2 [0x1012adc8],
// feed-forward clamp(asin(dir.y - last dir.y) / dt, +-0.1)).
bool HeliPilot::aimAt(const Vec3& direction, float margin, float lastDirY, float seconds)
{
	float maxTerrain = 0;
	if (terrainCheck(maxTerrain)) {
		const float speed = std::sqrt(dot(m_sense.velocity, m_sense.velocity));
		if (m_sense.forward.y > -0.15f && speed > 2) {
			m_state.controls.throttle = 1;
		}
		return false;
	}
	holdHeight(maxTerrain + margin);
	if (direction.y <= -0.6f || direction.y >= 0.35f) {
		flyTo(m_sense.position + flat(m_sense.forward) * 150.0f, margin);
		return false;
	}
	if (yawTo(direction) < 10 && std::fabs(m_rate.y) < 0.1f) {
		const float ff = seconds > 0
			? std::clamp(std::asin(std::clamp(direction.y - lastDirY, -1.0f, 1.0f)) / seconds, -0.1f, 0.1f)
			: 0.0f;
		attitude(0, direction.y, 1.0f, 0.2f, ff);
		return true;
	}
	return false;
}

// 0x100674f0: the highest of the goal's height and 100 terrain samples
// round it, 2..30 m away at any angle (AIDLL's own random stream; a fixed
// spiral here).
float heliHoverHeight(const Vec3& goal, const std::function<float(float x, float z)>& terrain)
{
	float highest = goal.y;
	if (!terrain) {
		return highest;
	}
	for (int i = 0; i < 100; ++i) {
		const float r = 2.0f + 28.0f * static_cast<float>(i % 10) / 9.0f;
		const float a = static_cast<float>(i) * 2.39996323f; // the golden angle
		highest = std::max(highest, terrain(goal.x + r * std::sin(a), goal.z + r * std::cos(a)));
	}
	return highest;
}

} // namespace dice::hfe::ai
