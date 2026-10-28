#include "Cameras.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::game::camera {

namespace {

float length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// The camera never goes under the terrain (or the sea) plus `clearance`.
void keepAbove(Vec3& eye, float floor)
{
	eye.y = std::max(eye.y, floor);
}

} // namespace

Vec3 normalized(const Vec3& v)
{
	const float l = length(v);
	return l > 0 ? v * (1.0f / l) : v;
}

bool modeAllowed(Mode mode, const ModeRules& rules)
{
	switch (mode) {
	case Mode::FirstPerson:
	case Mode::Inside: return rules.inside;
	case Mode::Nose: return rules.nose && rules.allowNoseCam;
	case Mode::Chase: return rules.chase && rules.allowExternalViews;
	case Mode::FrontChase: return rules.frontChase && rules.allowExternalViews;
	case Mode::FlyBy: return rules.flyBy && rules.allowExternalViews;
	case Mode::Trace: return rules.trace;
	case Mode::ExternTrace: return rules.externTrace && rules.hasExternTarget;
	}
	return false;
}

Mode nextMode(Mode current, const ModeRules& rules, bool backwards)
{
	const int start = static_cast<int>(current);
	int mode = start;
	do {
		mode += backwards ? -1 : 1;
		if (mode >= 8) {
			mode = 0;
		} else if (mode < 0) {
			mode = 7;
		}
		if (modeAllowed(static_cast<Mode>(mode), rules)) {
			return mode == 0 ? Mode::Inside : static_cast<Mode>(mode);
		}
	} while (mode != start);
	return current;
}

Dir4 slerp(const Dir4& a, const Dir4& b, float t)
{
	// 0x006ac980: the shorter way round; nearly parallel ones lerp.
	float cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	float sign = 1;
	if (cosine < 0) {
		cosine = -cosine;
		sign = -1;
	}
	float wa, wb;
	if (1.0f - cosine <= 1e-6f) {
		wa = 1.0f - t;
		wb = t;
	} else {
		const float angle = std::acos(std::clamp(cosine, -1.0f, 1.0f));
		const float inv = 1.0f / std::sin(angle);
		wa = std::sin((1.0f - t) * angle) * inv;
		wb = std::sin(angle * t) * inv;
	}
	wb *= sign;
	return {wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z, wa * a.w + wb * b.w};
}

Look chase(ChaseState& state, const ChaseSettings& settings, const Frame& parent, const Vec3& root, float radius,
	bool front, float seconds, const Ground& ground)
{
	// The point looked at: the root, or chaseOffset in the parent's frame.
	Vec3 at = root;
	if (length(settings.offset) != 0) {
		at = parent.rotation.transform(settings.offset) + parent.position;
	}
	float distance = settings.distance, angle = settings.angle;
	if (settings.distance == -1 || settings.soldierParent) {
		distance = std::max(radius, 2.0f) * 2.0f; // *0x00a739ac
		if (settings.soldierParent) {
			angle = 0.3f;
		}
	}
	// Behind (or, for FrontChase, ahead), leaning up by the angle.
	const float side = front ? -1.0f : 1.0f;
	const Vec3 wanted = normalized(parent.forward() * -side + parent.up() * angle);
	const float t = std::clamp(settings.stiffness * seconds, 0.0f, 1.0f);
	if (!state.direction.zero()) {
		Dir4 d = slerp(state.direction, {wanted.x, wanted.y, wanted.z, 1}, t);
		const Vec3 n = normalized(d.xyz());
		state.direction = {n.x, n.y, n.z, d.w};
	} else {
		state.direction = {wanted.x, wanted.y, wanted.z, 1};
	}
	// The distance eases the same way (its velocity term, *0x00a739b0, is 0).
	state.distance = state.distance > 0 ? t * distance + (1 - t) * state.distance : distance;
	Look look;
	look.at = at;
	look.eye = at + state.direction.xyz() * state.distance;
	keepAbove(look.eye, std::max(ground.terrainAt(look.eye.x, look.eye.z), ground.seaAt(look.eye.x, look.eye.z)) + 1.0f);
	return look;
}

Look nose(const Frame& camera, const Mat3& parent, const Vec3& nosePos)
{
	Look look;
	look.eye = camera.position + parent.transform(nosePos);
	look.at = look.eye + camera.forward();
	look.up = camera.up();
	return look;
}

Look flyBy(FlyByState& state, const Vec3& cameraPosition, const Frame& root, const Vec3& rootVelocity, float radius,
	const Ground& ground)
{
	// The next point: ahead along the root's forward and its way, x 8 x 0.4
	// (the object's +0x254, 8 from 0x006131e0), a metre over the sea.
	const float scale = 8.0f * 0.4f;
	Vec3 ahead = root.forward() * (radius * 1.2f) + rootVelocity * 0.3f;
	Vec3 point = cameraPosition + ahead * scale;
	point.y = std::max(point.y, ground.seaAt(point.x, point.z) + 1.0f);
	if (state.points.empty() || state.points.front().time + 8.0f * 60.0f <= state.clock) {
		state.points.push_back({point, state.clock});
	}
	// The list's point by the clock (0x006123a0): the passed ones go.
	while (state.points.size() > 1 && state.clock > state.points[1].time) {
		state.points.pop_front();
	}
	Vec3 eye = state.points.front().position;
	if (state.points.size() > 1) {
		const auto& a = state.points[0];
		const auto& b = state.points[1];
		const float u = std::max(0.0f, (state.clock - a.time) / (b.time - a.time));
		eye = a.position * (1 - u) + b.position * u;
	}
	Look look;
	look.eye = eye;
	look.at = cameraPosition;
	keepAbove(look.eye, ground.terrainAt(eye.x, eye.z) + 0.5f);
	return look;
}

void traceStart(TraceState& state, const Vec3& cameraPosition, const Frame& parent)
{
	state.point = cameraPosition + parent.forward() * 20.0f;
	state.last = cameraPosition;
}

Look trace(TraceState& state, const Vec3& cameraPosition, const Frame& parent, const Vec3* target)
{
	// The point moves with the camera...
	state.point += cameraPosition - state.last;
	if (target && length(*target - cameraPosition) > 2.0f) {
		// ... and goes 0.2 of the way to the target a frame,
		state.point = state.point + (*target - state.point) * 0.2f;
	} else {
		// or back in front: eased while it is off the forward by more than
		// acos 0.9, put there once it is within.
		const Vec3 ahead = cameraPosition + parent.forward() * 20.0f;
		const Vec3 towards = normalized(state.point - cameraPosition);
		if (std::abs(physics::dot(towards, parent.forward())) <= 0.9f) {
			state.point = state.point + (ahead - state.point) * 0.1f;
		} else {
			state.point = ahead;
		}
	}
	state.last = cameraPosition;
	Look look;
	look.eye = cameraPosition;
	look.at = state.point;
	look.up = parent.up();
	return look;
}

void deathStart(DeathState& state, const Vec3& position, const Vec3& forward, float radius)
{
	state = {};
	state.wanted = std::max(3.0f, radius * 3.0f); // 0x00615f10: +0x188 -> +0x30c
	state.target = position;
	state.facing = forward;
	state.direction = {0, 0, 0, 1};
}

Look death(DeathState& state, const Vec3* body, const Vec3* killer, float seconds, const Ground& ground)
{
	if (body) {
		state.target = *body; // the object while it is there (+0x31c)
	}
	// Turned away from the killer once the camera is most of the way out.
	if (killer && state.wanted * 0.8f < state.distance) {
		Vec3 away {killer->x - state.target.x, 0, killer->z - state.target.z};
		state.facing = normalized(away);
	}
	const Vec3 wanted = normalized({-state.facing.x, -state.facing.y + 0.3f, -state.facing.z});
	const float t = std::clamp(seconds * kCamStiffness, 0.0f, 1.0f);
	if (!state.direction.zero()) {
		Dir4 d = slerp(state.direction, {wanted.x, wanted.y, wanted.z, 1}, t);
		const Vec3 n = normalized(d.xyz());
		state.direction = {n.x, n.y, n.z, d.w};
	} else {
		// The first frame: a tenth of the way back, half a metre out.
		state.direction.x = -state.facing.x * 0.1f;
		state.direction.y = -state.facing.y * 0.1f;
		state.direction.z = -state.facing.z * 0.1f;
		state.distance = 0.5f;
		state.smoothed = state.target;
	}
	const float follow = std::min(seconds * kCamInterpolateAttach, 1.0f);
	state.smoothed = state.smoothed + (state.target - state.smoothed) * follow;
	state.distance = t * state.wanted + (1 - t) * state.distance;
	Look look;
	look.eye = state.smoothed + state.direction.xyz() * state.distance;
	keepAbove(look.eye, ground.terrainAt(look.eye.x, look.eye.z) + kDeathTerrainOffset);
	look.at = state.target;
	return look;
}

Look follow(FollowState& state, const Frame& camera, float radius, const Vec3* velocity, const Ground& ground)
{
	const Vec3 wanted = camera.forward() * -radius + camera.up() * (radius * 0.3f);
	Vec3 delta = wanted - state.offset;
	if (velocity) {
		delta = delta - *velocity * 0.6f;
	}
	const float share = 1.0f - std::exp(-0.06666667f); // the clamp's constant -1/15
	Look look;
	look.at = camera.position;
	look.eye = camera.position + state.offset + delta * share;
	keepAbove(look.eye, ground.terrainAt(look.eye.x, look.eye.z) + 1.0f);
	state.offset = look.eye - camera.position;
	return look;
}

void freeMove(FreeState& state, const FreeInput& input, float seconds, const Ground& ground)
{
	// The speed (+0x280): slow while sprint is held, fast with the alternate
	// sprint or once fast and still going forward, else normal.
	if (input.slow) {
		state.speed = kCamSlow;
	} else if (input.fast || (1.0f < state.speed && input.throttle > 0)) {
		state.speed = kCamFast;
	} else {
		state.speed = kCamNormal;
	}
	state.yaw = std::remainder(state.yaw + input.turnDegrees, 360.0f);
	state.pitch = std::clamp(state.pitch + input.pitchDegrees, -89.0f, 89.0f);
	const Mat3 r = Mat3::fromYawPitchRoll({state.yaw, -state.pitch, 0});
	const float step = state.speed * seconds * 20.0f;
	state.position += r.rows[0] * (input.strafe * step);
	state.position += r.rows[2] * (input.throttle * step);
	state.position.y += ((input.up ? 1.0f : 0.0f) - (input.down ? 1.0f : 0.0f)) * step;
	keepAbove(state.position, ground.terrainAt(state.position.x, state.position.z) + 0.2f);
}

Look freeLook(const FreeState& state)
{
	const Mat3 r = Mat3::fromYawPitchRoll({state.yaw, -state.pitch, 0});
	return {state.position, state.position + r.rows[2], {0, 1, 0}};
}

void anglesOf(const Look& look, float& yaw, float& pitch)
{
	const Vec3 f = normalized(look.at - look.eye);
	yaw = std::atan2(f.x, f.z);
	pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
}

} // namespace dice::hfe::game::camera
