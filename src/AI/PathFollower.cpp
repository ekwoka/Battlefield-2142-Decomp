#include "PathFollower.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::ai {

namespace {

constexpr float kPi = 3.14159265f;

float cross2(float ax, float az, float bx, float bz) { return ax * bz - az * bx; }

float distance2XZ(const Vec3& a, const Vec3& b) { return (a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z); }

float lengthXZ(const Vec3& a, const Vec3& b) { return std::sqrt(distance2XZ(a, b)); }

Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

// Where the line p0->p1 crosses the line a->b, as the parameter along a->b
// (0x100a8300); false for parallel lines.
bool crossing(const Vec3& p0, const Vec3& p1, const Vec3& a, const Vec3& b, float& t)
{
	const float dx = p1.x - p0.x, dz = p1.z - p0.z;
	const float ex = b.x - a.x, ez = b.z - a.z;
	const float denom = cross2(dx, dz, ex, ez);
	if (std::fabs(denom) < 1e-9f) {
		return false;
	}
	t = cross2(dx, dz, p0.x - a.x, p0.z - a.z) / denom;
	return true;
}

// The signed angle (radians, + clockwise seen from above: to the right of
// a heading measured as atan2(x, z)) from `forward` to `direction`.
float signedAngle(const Vec3& forward, const Vec3& direction)
{
	const float a = std::atan2(forward.x, forward.z);
	const float b = std::atan2(direction.x, direction.z);
	return std::remainder(b - a, 2 * kPi);
}

} // namespace

bool meshRaycast(const NavMesh& mesh, const Vec3& from, const Vec3& to, const CostProfile* profile, int maxPolygons,
	float* reached, int32_t startPolygon)
{
	if (reached) {
		*reached = 0;
	}
	int32_t current = startPolygon >= 0 ? startPolygon : mesh.findPolygon(from, 5.0f, profile);
	if (current < 0) {
		return false;
	}
	const float length = lengthXZ(from, to);
	const float dx = to.x - from.x, dz = to.z - from.z;
	float t = 0;
	for (int step = 0; step <= maxPolygons; ++step) {
		if (mesh.containsXZ(static_cast<uint32_t>(current), to)) {
			if (reached) {
				*reached = length;
			}
			return true;
		}
		if (step == maxPolygons) {
			break;
		}
		// The edge the segment leaves the polygon through: the crossing
		// furthest along it.
		const Polygon& polygon = mesh.polygons[static_cast<size_t>(current)];
		float best = -1;
		int exit = -1;
		for (uint32_t k = 0; k < polygon.vertexCount; ++k) {
			const Vec3& a = mesh.vertex(static_cast<uint32_t>(current), k);
			const Vec3& b = mesh.vertex(static_cast<uint32_t>(current), (k + 1) % polygon.vertexCount);
			const float ex = b.x - a.x, ez = b.z - a.z;
			const float denom = cross2(dx, dz, ex, ez);
			if (std::fabs(denom) < 1e-12f) {
				continue;
			}
			const float along = cross2(a.x - from.x, a.z - from.z, ex, ez) / denom;
			const float onEdge = cross2(a.x - from.x, a.z - from.z, dx, dz) / denom;
			if (onEdge >= -1e-4f && onEdge <= 1 + 1e-4f && along > best) {
				best = along;
				exit = static_cast<int>(k);
			}
		}
		if (exit < 0 || best < t - 1e-4f) {
			break;
		}
		t = std::max(t, best);
		if (t >= 1) {
			if (reached) {
				*reached = length;
			}
			return true; // the end lies on the edge
		}
		const int32_t next = mesh.neighbour(static_cast<uint32_t>(current), static_cast<uint32_t>(exit));
		if (next < 0 || (profile && profile->cost(mesh.polygons[static_cast<size_t>(next)].costClass) < 0)) {
			break;
		}
		current = next;
	}
	if (reached) {
		*reached = std::clamp(t, 0.0f, 1.0f) * length;
	}
	return false;
}

int32_t nearestPolygon(const NavMesh& mesh, const Vec3& point, float reach, const CostProfile* profile, Vec3* at)
{
	int32_t polygon = mesh.findPolygon(point, 5.0f, profile);
	if (polygon >= 0 || reach <= 0) {
		if (at) {
			*at = point;
		}
		return polygon;
	}
	for (float r = 1; r <= reach; r += 1) {
		for (int k = 0; k < 16; ++k) {
			const float a = static_cast<float>(k) * kPi / 8;
			const Vec3 p {point.x + std::sin(a) * r, point.y, point.z + std::cos(a) * r};
			polygon = mesh.findPolygon(p, 5.0f, profile);
			if (polygon >= 0) {
				if (at) {
					*at = p;
				}
				return polygon;
			}
		}
	}
	return -1;
}

std::vector<Waypoint> pathWaypoints(const NavMesh& mesh, const std::vector<PathStep>& steps, const Vec3& to)
{
	std::vector<Waypoint> waypoints;
	for (size_t i = 1; i < steps.size(); ++i) {
		Waypoint w;
		w.p = steps[i].position;
		w.polygon = static_cast<int32_t>(steps[i].polygon);
		const uint32_t previous = steps[i - 1].polygon;
		const Polygon& polygon = mesh.polygons[previous];
		for (uint32_t k = 0; k < polygon.vertexCount; ++k) {
			if (mesh.neighbour(previous, k) == static_cast<int32_t>(steps[i].polygon)) {
				w.a = mesh.vertex(previous, k);
				w.b = mesh.vertex(previous, (k + 1) % polygon.vertexCount);
				w.portal = true;
				break;
			}
		}
		waypoints.push_back(w);
	}
	Waypoint goal;
	goal.p = to;
	goal.polygon = steps.empty() ? -1 : static_cast<int32_t>(steps.back().polygon);
	waypoints.push_back(goal);
	return waypoints;
}

void smoothWaypoints(std::vector<Waypoint>& waypoints, const Vec3& bot, float radius)
{
	const size_t n = waypoints.size();
	if (n < 2) {
		return;
	}
	Vec3 previous = bot;
	for (int pass = 0; pass < 2; ++pass) {
		for (size_t i = 0; i + 1 < n; ++i) {
			Waypoint& w = waypoints[i];
			float t = 0;
			if (w.portal && distance2XZ(bot, w.p) >= 25 && crossing(previous, waypoints[i + 1].p, w.a, w.b, t)) {
				const float length = lengthXZ(w.a, w.b);
				if (length > 1e-4f) {
					const float s = length * t;
					const float margin = std::min(std::max(1.0f, 0.8f * radius), 0.5f * length);
					if (s >= length * 0.5f) {
						if (s > length - margin) {
							t = 1 - margin / length;
						}
					} else if (s < margin) {
						t = margin / length;
					}
					w.p = lerp(w.a, w.b, std::min(t, 1.0f));
				}
			}
			previous = w.p;
		}
	}
	previous = bot;
	for (size_t i = 0; i + 1 < n; ++i) {
		Waypoint& w = waypoints[i];
		float t = 0;
		if (w.portal && crossing(previous, waypoints[i + 1].p, w.a, w.b, t) && t >= 0 && t <= 1) {
			w.p = lerp(w.a, w.b, t);
		}
		previous = w.p;
	}
}

void PathFollower::reset()
{
	m_planned = false;
	m_failed = false;
	m_waypoints.clear();
	m_current = 0;
	m_blocked = false;
	m_jump = false;
}

void PathFollower::request(const NavMesh& mesh, const Vec3& from, const Vec3& to, const FollowerSettings& settings, float now)
{
	if (m_planned && !m_failed && distance2XZ(to, m_goal) < 1e-4f && std::fabs(to.y - m_goal.y) < 1e-2f) {
		return; // the same goal: keep following (0x10095fc0)
	}
	m_settings = settings;
	m_goal = to;
	plan(mesh, from, now);
}

void PathFollower::plan(const NavMesh& mesh, const Vec3& from, float now)
{
	m_planned = true;
	m_failed = false;
	m_blocked = false;
	m_current = 0;
	m_waypoints.clear();
	// Timers (0x100923a0).
	m_lastInput = m_lastProgress = now;
	m_progressAt = from;
	m_bendAt = -100;
	// A straight walk first: under 200 m over at most five polygons (0x100952c0).
	if (distance2XZ(from, m_goal) < 200.0f * 200.0f && meshRaycast(mesh, from, m_goal, &m_settings.options.profile, 5)) {
		Waypoint w;
		w.p = m_goal;
		w.polygon = mesh.findPolygon(m_goal, m_settings.options.heightTolerance, &m_settings.options.profile);
		m_waypoints.push_back(w);
		m_startPolygon = mesh.findPolygon(from, m_settings.options.heightTolerance, &m_settings.options.profile);
		return;
	}
	Vec3 begin = from, end = m_goal;
	const float reach = m_settings.soldier ? 3.0f : 10.0f;
	const int32_t start = nearestPolygon(mesh, from, reach, &m_settings.options.profile, &begin);
	const int32_t goal = nearestPolygon(mesh, m_goal, reach, &m_settings.options.profile, &end);
	if (start < 0 || goal < 0) {
		m_failed = true;
		return;
	}
	m_startPolygon = start;
	std::vector<PathStep> steps;
	if (m_settings.options.useClusters && !mesh.clusters.empty()) {
		const std::vector<uint16_t> corridor = findClusterPath(mesh, static_cast<uint32_t>(start), begin,
			static_cast<uint32_t>(goal), end, m_settings.options);
		if (!corridor.empty()) {
			std::vector<bool> allowed(mesh.clusters.size(), false);
			for (uint16_t c : corridor) {
				allowed[c] = true;
			}
			steps = findPolygonPath(mesh, static_cast<uint32_t>(start), begin, static_cast<uint32_t>(goal), end,
				m_settings.options, &allowed);
		}
	}
	if (steps.empty()) {
		steps = findPolygonPath(mesh, static_cast<uint32_t>(start), begin, static_cast<uint32_t>(goal), end, m_settings.options);
	}
	if (steps.empty()) {
		m_failed = true;
		return;
	}
	m_waypoints = pathWaypoints(mesh, steps, end);
	smoothWaypoints(m_waypoints, from, m_settings.radius);
	// The look-ahead runs after each search (twice: one window and the next).
	lookAhead(mesh, from, now);
	lookAhead(mesh, from, now);
}

// Back onto the path when the bot strayed (0x10093a20): fine on the
// polygon of the waypoint before, of the current one or of any passed;
// else the first waypoint within the next 20 whose polygon it is on.
bool PathFollower::resync(const NavMesh& mesh, const Vec3& position)
{
	const int32_t polygon = mesh.findPolygon(position, m_settings.soldier ? 2.5f : 5.0f);
	if (polygon < 0) {
		return true; // off the mesh (a vehicle's jump, a soldier on a ledge): carry on
	}
	if (polygon == m_startPolygon) {
		return true; // the path's first entry: where it was planned from
	}
	for (size_t i = 0; i <= m_current && i < m_waypoints.size(); ++i) {
		if (m_waypoints[i].polygon == polygon) {
			return true;
		}
	}
	for (size_t j = m_current + 1; j < m_waypoints.size() && j <= m_current + 20; ++j) {
		if (m_waypoints[j].polygon == polygon) {
			m_current = j;
			return true;
		}
	}
	// The start polygon (before the first waypoint) counts as on the path.
	return m_current == 0;
}

// The look-ahead (0x10093420).
void PathFollower::lookAhead(const NavMesh& mesh, const Vec3& position, float now)
{
	if (m_current >= m_waypoints.size()) {
		return;
	}
	const float trigger = m_settings.soldier ? 10.0f : 20.0f;
	if (distance2XZ(position, m_waypoints[m_current].p) >= trigger * trigger) {
		return;
	}
	const int most = m_settings.soldier ? 1 : 4;
	// The ray's width (0x101287a8 0.5 on foot, 0x10139514 0 at the wheel):
	// here two more rays half a width to either side (provisional).
	const float width = m_settings.soldier ? 0.5f : 0.0f;
	const auto clear = [&](const Vec3& to) {
		if (!meshRaycast(mesh, position, to, &m_settings.options.profile, rayPolygons())) {
			return false;
		}
		if (width <= 0) {
			return true;
		}
		const float length = std::max(lengthXZ(position, to), 1e-3f);
		const Vec3 side {(to.z - position.z) / length * width * 0.5f, 0, -(to.x - position.x) / length * width * 0.5f};
		for (const Vec3& offset : {side, side * -1.0f}) {
			if (mesh.findPolygon(position + offset, 5.0f, &m_settings.options.profile) >= 0
				&& !meshRaycast(mesh, position + offset, to + offset, &m_settings.options.profile, rayPolygons())) {
				return false;
			}
		}
		return true;
	};
	int skipped = 0;
	while (skipped < most && m_current + 1 < m_waypoints.size() && clear(m_waypoints[m_current + 1].p)) {
		++m_current;
		++skipped;
	}
	const bool blocked = skipped == 0 && !clear(m_waypoints[m_current].p);
	if (blocked && !m_blocked) {
		m_blockedSince = now;
	}
	m_blocked = blocked;
}

PathFollower::Steer PathFollower::steer(const NavMesh& mesh, const Vec3& position, float now)
{
	Steer out;
	if (!m_planned || m_failed || m_waypoints.empty()) {
		return out;
	}
	if (!resync(mesh, position)) {
		plan(mesh, position, now);
		if (m_failed) {
			return out;
		}
	}
	// Reached (0x10094050).
	const Waypoint& w = m_waypoints[m_current];
	const bool last = m_current + 1 == m_waypoints.size();
	float r = 0.5f; // 0x101287ac
	if (!m_settings.soldier) {
		r = std::max(1.0f, m_settings.approach);
		if (last) {
			r = std::max(r, m_settings.radius);
		}
	}
	const float height = m_settings.soldier ? 3.0f : 5.0f;
	if (std::fabs(position.y - w.p.y) <= height && distance2XZ(position, w.p) < r * r) {
		if (last) {
			out.ok = true;
			out.arrived = true;
			out.point = w.p;
			m_lastInput = m_lastProgress = now;
			return out;
		}
		++m_current;
	}
	lookAhead(mesh, position, now);
	// Blocked too long: a new path from here (0x10095470).
	if (m_blocked && now - m_blockedSince > (m_settings.soldier ? 2.0f : 0.5f)) {
		plan(mesh, position, now);
		if (m_failed) {
			return out;
		}
	}
	out.ok = true;
	out.point = m_waypoints[m_current].p;
	out.jump = m_jump;
	m_jump = false;
	return out;
}

void PathFollower::progress(const Vec3& position, float input, float now)
{
	if (!m_planned || m_failed || m_waypoints.empty()) {
		return;
	}
	if (input <= 0.1f) {
		if (now - m_lastInput > (m_settings.soldier ? 0.1f : 0.5f)) {
			m_lastProgress = now; // not trying to move: not stuck
			return;
		}
	} else {
		m_lastInput = now;
	}
	const float r = m_settings.soldier ? 2.0f : 4.0f;
	if (distance2XZ(position, m_progressAt) + (position.y - m_progressAt.y) * (position.y - m_progressAt.y) > r * r) {
		m_progressAt = position;
		m_lastProgress = now;
		return;
	}
	if (m_settings.soldier && now - m_lastProgress > 0.5f && now - m_jumpAt > 2.0f) {
		const Vec3 d = position - m_sample;
		if (d.x * d.x + d.y * d.y + d.z * d.z < 0.001f) {
			m_jump = true;
			m_jumpAt = now;
		}
		m_sample = position;
	}
	if (now - m_lastProgress >= (m_settings.soldier ? 10.0f : 20.0f)) {
		m_failed = true;
	}
}

float PathFollower::remaining(const Vec3& position) const
{
	if (m_waypoints.empty() || m_current >= m_waypoints.size()) {
		return 0;
	}
	float total = lengthXZ(position, m_waypoints[m_current].p);
	for (size_t i = m_current + 1; i < m_waypoints.size(); ++i) {
		total += lengthXZ(m_waypoints[i - 1].p, m_waypoints[i].p);
	}
	return total;
}

float PathFollower::maxBend(const Vec3& position, const Vec3& forward, float speed, float lookAheadTime, float now)
{
	if (now - m_bendAt < 0.5f) {
		return m_bend;
	}
	m_bendAt = now;
	m_bend = 0;
	const float reach = std::max(3.0f, std::fabs(speed) * lookAheadTime);
	float covered = 0;
	Vec3 from = position;
	for (size_t i = m_current; i < m_waypoints.size() && covered < reach; ++i) {
		const Vec3& to = m_waypoints[i].p;
		const float length = lengthXZ(from, to);
		if (length > 0.01f) {
			const float angle = signedAngle(forward, to - from);
			if (std::fabs(angle) > std::fabs(m_bend)) {
				m_bend = angle;
			}
		}
		covered += length;
		from = to;
	}
	return m_bend;
}

DriveOutput driveControl(const DriveSettings& s, DriveState& state, const DriveSense& in)
{
	DriveOutput out;
	const float r = s.radius;
	const float probe = r + 10;
	const float needed = s.spaceNeededTweak * r;
	const auto fraction = [&](float free) { return std::clamp((free - needed) / std::max(probe - needed, 0.01f), 0.0f, 1.0f); };
	const float frontFraction = fraction(in.front);
	const float backFraction = fraction(in.back);
	float error = in.error;
	const float c = std::cos(error);
	int direction = 1;
	// The state machine (0x100e8170; the transitions are provisional).
	switch (state.state) {
	case 8: // backing towards a goal behind
		if (c >= -0.2f || in.back <= needed) {
			state.state = 0;
		}
		break;
	case 9:  // turning on the spot: ahead at full lock
	case 11: // and back at the opposite lock
		if (c > 0.9f) {
			state.state = 0;
		} else if (state.state == 9 && in.front < needed) {
			state.state = 11;
		} else if (state.state == 11 && in.back < needed) {
			state.state = 9;
		}
		break;
	default:
		if (!(c >= -0.2f && in.front >= needed)) {
			if (in.goalDistance > 10 * r && c < -0.6f && in.back > needed) {
				state.state = 8;
			} else if (c < 0.9f) {
				state.state = in.front >= needed ? 9 : 11;
			}
		}
		break;
	}
	float cap = 0;
	if (state.state == 8) {
		direction = -1;
		if (std::fabs(error) > kPi / 2) {
			error = (kPi - std::fabs(error)) * (error > 0 ? 1.0f : -1.0f);
		}
	} else if (state.state == 9) {
		error = error >= 0 ? 2.0f : -2.0f;
	} else if (state.state == 11) {
		direction = -1;
		error = error >= 0 ? -2.0f : 2.0f;
	}
	if (direction > 0) {
		cap = frontFraction >= 0.95f ? s.maxSpeed - (1 - frontFraction) * (s.maxSpeed - s.speedAtZeroSpace)
									 : s.backingSpeed - (1 - frontFraction) * (s.backingSpeed - s.speedAtZeroSpace);
	} else {
		cap = s.backingSpeed - (1 - backFraction) * (s.backingSpeed - s.speedAtZeroSpace);
	}
	if (in.front <= 0.01f && in.back <= 0.01f) {
		direction = 1; // boxed in
		cap = 0.5f * s.maxSpeed;
	}
	// The speed wanted: braking for the end of the path, slowing for bends.
	const float speed = std::fabs(in.speed);
	float brake = 1;
	if (speed > s.backingSpeed && s.brakeDistance > 0) {
		const float space = std::max(0.0f, in.remaining - r - 0.5f);
		if (space * space < s.brakeDistance * s.brakeDistance) {
			const float ratio = space * space / (s.brakeDistance * s.brakeDistance);
			if (s.tank) {
				brake = std::max(1 - s.maxBrakePercentage, ratio); // TankMoveTo, as decompiled
			} else {
				const float f = std::max(1 - s.maxBrakePercentage * 0.01f, ratio);
				brake = f * f;
			}
		}
	}
	float angle = 1;
	if (speed > s.minSpeedForLookAhead) {
		const float a = s.tank ? std::fabs(in.maxBend) : std::max(std::fabs(in.maxBend), std::fabs(in.error));
		angle = 1 - s.maxAngleDiffSlowDown * std::min(1.0f, s.angleSlowDownScale * std::max(a - s.minAngleDiffForSlowDown, 0.0f));
	}
	const float targetSpeed = std::min(brake, angle) * s.maxSpeed;
	out.direction = direction;
	out.targetSpeed = targetSpeed;
	const float motion = in.speed > 0 ? 1.0f : in.speed < 0 ? -1.0f : 0.0f;
	float target = targetSpeed * static_cast<float>(direction);
	if (in.tilt > 0.25f) {
		target *= 1 - 0.5f * std::min(1.0f, (in.tilt - 0.25f) / 0.7f);
	}
	target = std::clamp(target, -cap, cap);
	float throttle = 0;
	if (!s.tank) {
		// The car's controller (0x100e9d40).
		if (speed > 1 && motion != 0 && static_cast<float>(direction) != motion) {
			error = -error;
		}
		out.turn = std::clamp(3 * s.yawScale * error - static_cast<float>(direction) * in.yawRate, -1.0f, 1.0f);
		throttle = std::clamp(target / 15 + std::min(1.0f, (target - in.speed) / 3), -1.0f, 1.0f);
		if (targetSpeed >= s.maxSpeed && speed > 0.8f * s.maxSpeed) {
			throttle = 1;
		}
	} else {
		// The tank's (0x100e0540): no turning below 1 m/s, less below 2 m/s.
		float e = 0;
		if (speed >= 1) {
			const float k = std::min(1.0f, speed / 2.0f);
			e = error * k * k;
		}
		if (static_cast<float>(direction) != motion && motion != 0) {
			e = -e;
		}
		out.turn = std::clamp(3 * e, -1.0f, 1.0f);
		const float x = (target - in.speed) / 3;
		const float m = std::min(1.0f, std::fabs(x));
		throttle = std::clamp(target / 15 + (x > 0 ? 1.0f : x < 0 ? -1.0f : 0.0f) * m * m, -1.0f, 1.0f);
	}
	if (speed < 1.5f && throttle != 0) {
		throttle = throttle > 0 ? 1.0f : -1.0f; // a kick off the line
	}
	out.throttle = throttle;
	return out;
}

} // namespace dice::hfe::ai
