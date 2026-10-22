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
	// The ray's width (0x101287a8 0.5 on foot, 0x10139514 0 at the wheel)
	// goes to the pathfinder's ray (vslot 0x84), whose use of it isn't
	// decoded; here two more rays half a width to either side.
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

namespace {

float dot3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length3(const Vec3& v) { return std::sqrt(dot3(v, v)); }
float dot2(const Vec3& a, const Vec3& b) { return a.x * b.x + a.z * b.z; }
float sign(float v) { return v > 0 ? 1.0f : v < 0 ? -1.0f : 0.0f; }

// The signed angle from a to b across the ground (0x100a8710 and
// 0x100a87f0): asin of their cross product, folded past a right angle.
float angleTo(const Vec3& a, const Vec3& b)
{
	const float s = std::clamp(a.z * b.x - a.x * b.z, -1.0f, 1.0f);
	const float angle = std::asin(s);
	if (dot2(a, b) < 0) {
		return (angle > 0 ? kPi : angle < 0 ? -kPi : 0.0f) - angle;
	}
	return angle;
}

const DriveObstacle* findObstacleById(const DriveSense& in, int id)
{
	for (const DriveObstacle& o : in.obstacles) {
		if (o.id == id) {
			return &o;
		}
	}
	return nullptr;
}

// The bot's state change (0x100973b0): at most one per time stamp.
void setState(DriveState& st, int state, float now)
{
	if (now < st.changedAt) {
		st.changedAt = -1;
	}
	if (now - st.changedAt > 0) {
		st.changedAt = now;
		st.state = state;
	}
}

// The side to pass an obstacle on (0x10097410): true when it flips from
// one side to the other after a flip before; 0 clears it.
bool setSide(DriveState& st, int side)
{
	const bool flipped = st.sideFlipped && side != st.side && side != 0;
	if (side == 0) {
		st.sideFlipped = false;
		st.side = 0;
		return flipped;
	}
	if (side != st.side && st.side != 0) {
		st.sideFlipped = true;
	}
	st.side = side;
	return flipped;
}

// Avoiding on: the side is cleared (0x1009b2e0).
void setAvoiding(DriveState& st, bool on)
{
	st.avoiding = on;
	st.side = 0;
}

// 0x100fbdb0: shortens the free distances ahead of and behind `p` along
// `dir` to where the line meets another object's sphere (its centre
// radius about its origin), leaving out `skip`.
void clipByObstacles(const DriveSense& in, const Vec3& p, const Vec3& dir, float& ahead, float& behind, int skip)
{
	for (const DriveObstacle& o : in.obstacles) {
		if (o.id == skip) {
			continue;
		}
		const float r = o.centreRadius;
		const Vec3 d = o.position - p;
		const float along = dot3(d, dir);
		const float side = length3(d - dir * along);
		if (along <= 0) {
			if (r + behind < -along || r < side) {
				continue;
			}
			const float t = -along - std::sqrt(r * r - side * side);
			if (t <= behind) {
				behind = t;
			}
		} else {
			if (r + ahead < along || r < side) {
				continue;
			}
			const float t = along - std::sqrt(r * r - side * side);
			if (t <= ahead) {
				ahead = t;
			}
		}
	}
}

// 0x100fc660: the speed allowed behind another object close ahead (or
// behind, when backing): within 15 m of its sphere, the line of travel
// through it, nearly straight ahead (dot 0.75), or touching (2 m) and it
// moving over 2 m/s: its speed along the way less 1, at least 2. Closing
// at over 10 m/s from a standstill state: -20 (full brake) and the swerve
// (`swerve`: the error forced to -2). One that moves across the way
// (dot between -0.75 and 0.6) farther than 8 m, or the one being avoided
// while avoiding, does not count. Not below 2 m/s of the driver's own
// speed: 9999 (no cap).
float obstacleSpeedCap(const DriveSettings& s, DriveState& st, const DriveSense& in, float signedSpeed, bool& swerve)
{
	swerve = false;
	if (signedSpeed <= 2) {
		return 9999;
	}
	const Vec3 forward3 = in.forward;
	const float motion = sign(dot3(in.velocity, forward3));
	const Vec3 way = forward3 * motion;
	float best = 9999;
	int bestId = -1;
	for (const DriveObstacle& o : in.obstacles) {
		Vec3 d = o.centre - in.position;
		float dist = length3(d);
		const float r = s.centreRadius + o.centreRadius;
		if (dist > 1e-5f) {
			d = d * (1 / dist);
		}
		const float gap = dist - r;
		if (gap > 15) {
			continue;
		}
		const float otherSpeed = length3(o.velocity);
		const float f = dot3(d, way);
		const Vec3 nearest = in.position + way * (f * (r + gap));
		const Vec3 off = nearest - o.centre;
		if (dot3(off, off) > r * r) {
			continue;
		}
		if (!(f >= 0.75f || (gap < 2 && f > 0 && otherSpeed > 2))) {
			continue;
		}
		Vec3 v = o.velocity;
		const bool moving = otherSpeed > 0.5f;
		float cap = std::max(2.0f, dot3(v, way) - 1);
		if (moving) {
			v = v * (1 / otherSpeed);
		}
		const Vec3 relative = o.velocity - in.velocity;
		const bool closing = dot3(relative, relative) > 100;
		if (closing && st.state == 0) {
			cap = -20;
			swerve = true;
		}
		const float along = dot3(v, way);
		if ((gap <= 8 || closing) && (!moving || along >= 0.6f || along <= -0.75f || closing)
			&& (!st.avoiding || st.avoided != o.id || closing)) {
			if (cap < best) {
				best = cap;
				bestId = o.id;
			}
		}
	}
	st.brakingFor = bestId;
	return best;
}

// What 0x100fbff0 finds of the obstacle in the way.
struct ObstacleView {
	bool found = false;
	int id = -1;
	float ahead = 0;         // dot(direction to it, forward)
	float angle = 0;         // to it, from the forward axis (or the way, when nearly straight)
	bool clearAhead = false; // straight on misses it by more than sqrt(0.7) of its reach
	bool pathClear = false;  // the way to the steering point misses it (or it is behind)
	bool touching = false;   // within both radii
	float passAngle = 0;     // where to head to pass it on the side chosen
	bool listed = false;     // the one being avoided is still in the world list
};

// 0x100fbff0: the object in the way. Moving ahead (or standing): the
// vehicles within reach (forward speed + max(radius, 5) + both radii)
// that are nearer than the steering point or sit on it, ahead, and whose
// sphere (both radii) the forward line crosses, the most central first;
// one more than 60 degrees aside only when it is the one being avoided (or
// none is). Failing that, the one being avoided while it is still about.
ObstacleView findObstacle(const DriveSettings& s, DriveState& st, const DriveSense& in, const Vec3& dir,
	const Vec3& offset, float pointDistance, float motion, int side)
{
	ObstacleView v;
	const float k = 0.25f; // the velocity look-ahead (0.5 when called without the flag; CarMoveTo sets it)
	int chosen = -1;
	float best = 99999;
	for (const DriveObstacle& o : in.obstacles) {
		if (st.avoided != -1 && o.id == st.avoided) {
			v.listed = true;
		}
		if (motion < 0) {
			continue;
		}
		const float r = o.radius + s.radius;
		const Vec3 d = o.position - in.position;
		const float d2 = dot3(d, d);
		const float reach = dot3(in.forward, in.velocity) + std::max(s.radius, 5.0f) + r;
		const float ox = d.x - offset.x, oz = d.z - offset.z;
		if (d2 > reach * reach || (d2 > pointDistance * pointDistance && ox * ox + oz * oz > r * r)) {
			continue;
		}
		const float along = dot2(d, in.forward);
		if (along < 0) {
			continue;
		}
		const float px = d.x - along * in.forward.x, pz = d.z - along * in.forward.z;
		const float perp2 = px * px + pz * pz;
		if (perp2 <= r * r && (along / std::sqrt(d.x * d.x + d.z * d.z) >= 0.5f || o.id == st.avoided || st.avoided == -1)
			&& perp2 < best) {
			chosen = o.id;
			best = perp2;
		}
	}
	if (chosen == -1) {
		if (!v.listed) {
			return v;
		}
		chosen = st.avoided;
	}
	const DriveObstacle* o = findObstacleById(in, chosen);
	if (!o) {
		return v;
	}
	const float reach = o->radius * 0.8f + s.radius; // 0x1012b59c
	Vec3 u {o->position.x - in.position.x, 0, o->position.z - in.position.z};
	const float dist = std::sqrt(u.x * u.x + u.z * u.z);
	if (dist < 1e-5f) {
		return v; // on top of it (the original leaves its outputs unset here)
	}
	u = u * (1 / dist);
	v.ahead = dot2(u, in.forward);
	v.touching = dist < o->radius + s.radius;
	// Seen from a quarter of a second ahead.
	Vec3 w {o->position.x - (in.position.x + k * in.velocity.x), 0, o->position.z - (in.position.z + k * in.velocity.z)};
	const float wl = std::sqrt(w.x * w.x + w.z * w.z);
	w = w * (1 / wl);
	if (wl >= reach) {
		const float half = std::asin(std::clamp(reach / wl, -1.0f, 1.0f));
		const float toIt = angleTo(in.forward, w);
		v.passAngle = side == 2 ? toIt + half : toIt - half;
	} else {
		const Vec3 perp = side == 2 ? Vec3 {w.z, 0, -w.x} : Vec3 {-w.z, 0, w.x};
		v.passAngle = angleTo(in.forward, perp);
	}
	const float qx = in.position.x + v.ahead * in.forward.x * dist - o->position.x;
	const float qz = in.position.z + v.ahead * in.forward.z * dist - o->position.z;
	v.clearAhead = reach * reach * 0.7f < qx * qx + qz * qz; // 0x1012b598
	const float g = dot2(u, dir);
	const float gx = in.position.x + g * dir.x * dist - o->position.x;
	const float gz = in.position.z + g * dir.z * dist - o->position.z;
	v.pathClear = reach * reach < gx * gx + gz * gz || (g < -0.1f && motion > 0);
	if (g < 0 && dot2(dir, in.forward) > 0) {
		// Behind the way ahead: forgotten.
		st.avoided = -1;
		v.pathClear = true;
		v.clearAhead = true;
		return v;
	}
	v.angle = std::fabs(dot2(dir, in.forward)) <= 0.85f ? angleTo(in.forward, u) : angleTo(dir, u);
	v.id = chosen;
	v.found = true;
	return v;
}

// The free space round the vehicle (0x100e8170): mesh rays out to radius
// + 10 m ahead, along the box's diagonals ahead (forward x length +-
// right x width), behind and its diagonals, and to either side. The rays
// behind are cut short by other vehicles (0x100fbdb0).
void freeSpace(const DriveSettings& s, const DriveSense& in, float reach, float& front, float& back, float& left,
	float& right)
{
	if (!in.ray) {
		front = in.freeFront;
		back = in.freeBack;
		left = in.freeLeft;
		right = in.freeRight;
	} else {
		const Vec3 p = in.position;
		const auto along = [&](const Vec3& d) { return in.ray(p, p + d * reach); };
		const auto distance = [&](const DriveRay& r) { return r.clear ? reach : r.distance; };
		Vec3 frontLeft = in.forward * s.length - in.right * s.width;
		Vec3 frontRight = in.forward * s.length + in.right * s.width;
		frontLeft = frontLeft * (1 / std::max(length3(frontLeft), 1e-6f));
		frontRight = frontRight * (1 / std::max(length3(frontRight), 1e-6f));
		front = distance(along(in.forward));
		for (const Vec3& d : {frontLeft, frontRight}) {
			const DriveRay r = along(d);
			if (!r.clear) {
				front = std::min(front, r.distance);
			}
		}
		back = distance(along(in.forward * -1));
		for (const Vec3& d : {frontLeft * -1, frontRight * -1}) {
			const DriveRay r = along(d);
			if (!r.clear) {
				back = std::min(back, r.distance);
			}
		}
		right = distance(along(in.right));
		left = distance(along(in.right * -1));
	}
	float unused = 0;
	clipByObstacles(in, in.position, in.forward, unused, back, -1);
	Vec3 frontLeft = in.forward * s.length - in.right * s.width;
	Vec3 frontRight = in.forward * s.length + in.right * s.width;
	frontLeft = frontLeft * (1 / std::max(length3(frontLeft), 1e-6f));
	frontRight = frontRight * (1 / std::max(length3(frontRight), 1e-6f));
	clipByObstacles(in, in.position, frontLeft, unused, back, -1);
	clipByObstacles(in, in.position, frontRight, unused, back, -1);
}

} // namespace

DriveOutput driveControl(const DriveSettings& s, DriveState& st, const DriveSense& in)
{
	DriveOutput out;
	const float r = s.radius;
	const float probe = r + 10;
	const float needed = s.spaceNeededTweak * r;
	float error = in.error;
	const float c = std::cos(error);  // local_44: dot(forward, way to the point)
	const bool pointLeft = error < 0; // bVar3
	const Vec3 dir = in.forward * c + in.right * std::sin(error); // unit, to the steering point
	// The motion's sign (+1 when standing) and speed; without a velocity
	// (the tests) the speed along the heading stands for it.
	const bool hasVelocity = dot3(in.velocity, in.velocity) > 0;
	const float motion = (hasVelocity ? dot3(in.forward, in.velocity) : in.speed) < 0 ? -1.0f : 1.0f;
	const float speed3 = hasVelocity ? length3(in.velocity) : std::fabs(in.speed);
	bool swerve = false;
	const float obstacleCap = obstacleSpeedCap(s, st, in, speed3 * motion, swerve);
	int direction = 1;
	float front = 0, back = 0, left = 0, right = 0;
	freeSpace(s, in, probe, front, back, left, right);
	const float remaining = in.remaining;

	// The obstacle in the way, kept while it stays where it was (within
	// 1 m, 2.5 m while avoiding).
	const int side = st.side;
	int sideWanted = side; // local_58: the side as read, as chosen, or 0 without an obstacle
	ObstacleView view = findObstacle(s, st, in, dir, dir * in.goalDistance, in.goalDistance, motion, side);
	bool found = view.found;
	int avoided = st.avoided;
	const auto reset = [&] {
		st.avoided = -1;
		view.id = -1;
		avoided = -1;
	};
	if (avoided == -1 || view.listed) {
		if (!found) {
			reset();
		} else if (const DriveObstacle* o = findObstacleById(in, view.id)) {
			const float limit = st.avoiding ? 6.25f : 1.0f;
			if (view.id == avoided && avoided != -1) {
				const Vec3 moved = o->position - st.avoidedAt;
				if (limit < dot3(moved, moved)) {
					found = false;
					reset();
				}
			} else {
				st.avoidedAt = o->position;
			}
		}
	} else {
		found = false;
		reset();
	}
	bool noRoom = false; // bVar4: no room on either side of it
	int switchState = st.state;
	if (!found) {
		setSide(st, 0);
	} else {
		st.avoided = view.id;
		avoided = view.id;
		if (side == 0) {
			// Choose a side: the one with more room beside the obstacle (mesh
			// rays from it along the driver's right axis, half its centre
			// radius + the driver's radius + 0.5 m, cut short by other
			// vehicles), or when both have room, the side of the way it is on.
			bool leftward = true;
			const DriveObstacle* o = findObstacleById(in, avoided);
			const float half = o->centreRadius * 0.5f + r;
			const float reach = half + 0.5f;
			float rightFree = reach, leftFree = reach;
			if (in.ray) {
				const auto free = [&](const Vec3& to) {
					const DriveRay ray = in.ray(o->position, to);
					return ray.clear ? length3(to - o->position) : ray.distance;
				};
				rightFree = free(o->position + in.right * reach);
				leftFree = free(o->position - in.right * reach);
			}
			leftward = rightFree < leftFree;
			clipByObstacles(in, o->position, in.right, rightFree, leftFree, avoided);
			const float least = std::min(leftFree, rightFree), most = std::max(leftFree, rightFree);
			noRoom = most < half;
			if (least <= half) {
				if (most >= half * 0.7f) {
					leftward = leftFree > rightFree;
				}
			} else {
				const Vec3 perp {dir.z, 0, -dir.x};
				leftward = dot2(o->position - in.position, perp) > 0;
			}
			sideWanted = leftward ? 1 : 2;
			if (setSide(st, sideWanted) && !st.avoiding) {
				switchState = 0;
				setState(st, 0, in.now);
				setAvoiding(st, true);
			}
		}
	}
	bool have = avoided != -1;
	if (have) {
		const DriveObstacle* o = findObstacleById(in, avoided);
		if (!o) {
			have = false;
			avoided = -1;
		} else {
			// It sits on the goal and the driver is next to it: stop there.
			const float gx = in.goal.x - o->position.x, gz = in.goal.z - o->position.z;
			if (gx * gx + gz * gz < o->radius * o->radius) {
				const float mx = o->position.x - in.position.x, mz = o->position.z - in.position.z;
				const float near = o->radius + r + 3;
				if (mx * mx + mz * mz < near * near) {
					out.stop = true;
					out.direction = 1;
					return out;
				}
			}
		}
	} else {
		setSide(st, 0);
		sideWanted = 0;
	}
	// Room to turn (2.7 and 3 radii across and lengthwise).
	const bool roomy = left + right > 2.7f * r && front + back > 2.7f * r; // local_61
	const bool wide = left + right > 3 * r && front + back > 3 * r;        // local_62
	const float frontFraction = (front - needed) / (probe - needed);
	const float backFraction = (back - needed) / (probe - needed);
	const bool boxed = front == 0 && back == 0;
	if (boxed) {
		switchState = c <= 0 ? 8 : 0;
	}
	float cap = 0;
	if (st.state > 7) {
		cap = s.backingSpeed - (1 - backFraction) * (s.backingSpeed - s.speedAtZeroSpace);
	} else if (frontFraction >= 0.95f) {
		cap = s.maxSpeed - (1 - frontFraction) * (s.maxSpeed - s.speedAtZeroSpace);
	} else {
		cap = s.backingSpeed - (1 - frontFraction) * (s.backingSpeed - s.speedAtZeroSpace);
	}
	if (noRoom) {
		cap = std::min(cap, s.backingSpeed);
	}
	if (!have) {
		setAvoiding(st, false);
		setSide(st, 0);
	}
	// While avoiding (the side flipped), the obstacle states give way to
	// driving on. (The original also tells the other vehicle's driver,
	// 0x100e8170's calls through the other's +0x28, and stops avoiding
	// when that one is not a player's and stands; not rewritten.)
	if (st.avoiding && (switchState == 4 || switchState == 5 || switchState == 6 || switchState == 13 || switchState == 14)) {
		setState(st, 0, in.now);
		switchState = 0;
	}
	const auto set = [&](int state) { setState(st, state, in.now); };
	const auto fold = [&] {
		direction = -1;
		if (std::fabs(error) > kPi / 2) {
			error = (kPi - std::fabs(error)) * sign(error);
		}
	};
	const bool angleLeft = view.angle < 0; // bVar5
	switch (switchState) {
	case 1:
	case 7:
	case 10:
		break;
	case 2: // ahead, turning right
		if (pointLeft) {
			set(c <= 0 ? 9 : 3);
		} else if (c <= 0.9f) {
			if (front >= needed && (view.ahead <= 0.5f || !have || view.clearAhead || st.avoiding)) {
				error = std::fabs(error);
			} else {
				set(11);
			}
		} else {
			set(0);
		}
		break;
	case 3: // ahead, turning left
		if (pointLeft) {
			if (c > 0.9f) {
				set(0);
			} else if (front >= needed && (view.ahead <= 0.5f || !have || view.clearAhead || st.avoiding)) {
				error = -std::fabs(error);
			} else {
				set(9);
			}
		} else {
			set(c > 0 ? 2 : 11);
		}
		break;
	case 4:
		if (!have || (view.clearAhead && !angleLeft)) {
			set(6);
		} else if (front >= needed && !view.touching) {
			error = -2;
		} else {
			set(13);
		}
		break;
	case 5:
		if (have && (!view.clearAhead || !angleLeft)) {
			if (front < needed || view.touching) {
				set(14);
			} else {
				error = 2;
			}
		} else {
			set(6);
		}
		break;
	case 6: // passing the obstacle
		if (!have || (view.pathClear && view.ahead < 0.3f)) {
			set(error >= 0 ? 2 : 3);
		} else if (!view.touching || view.clearAhead || view.ahead <= 0) {
			if (front >= needed) {
				error = view.passAngle;
			} else if (sideWanted == 2) {
				if (setSide(st, 1) && !st.avoiding) {
					set(0);
					setAvoiding(st, true);
				} else {
					set(13);
				}
			} else if (setSide(st, 2) && !st.avoiding) {
				set(0);
				setAvoiding(st, true);
			} else {
				set(14);
			}
		} else {
			set(sideWanted != 2 ? 13 : 14);
		}
		break;
	case 8: // backing towards a goal close behind
		if (boxed) {
			error = -error;
			fold();
		} else if (c > -0.5f || remaining > 10 * r + 5 || back < needed) {
			set(0);
		} else if (view.ahead >= -0.6f || !have) {
			fold();
		} else {
			set(angleLeft ? 3 : 2);
		}
		break;
	case 9: // backing, the point on the left
		if (!pointLeft) {
			set(c > 0 ? 2 : 11);
		} else if (c > 0.9f && front > needed) {
			set(0);
		} else if (back >= needed) {
			direction = -1;
			error = std::fabs(error);
		} else {
			set(3);
		}
		break;
	case 11: // backing, the point on the right
		if (pointLeft) {
			set(c > 0 ? 3 : 9);
		} else if (c > 0.9f && front > needed) {
			set(0);
		} else if (back >= needed) {
			direction = -1;
			error = -std::fabs(error);
		} else {
			set(2);
		}
		break;
	case 12: // backing out of a narrow spot
		if (c > 0) {
			set(0);
		} else if (remaining <= 10 * r && c < -0.6f && back > needed) {
			set(8);
		} else if (!wide) {
			fold();
		} else {
			set(pointLeft ? 9 : 11);
		}
		break;
	case 13:
		if (have && (!view.clearAhead || angleLeft)) {
			if (back < needed) {
				set(4);
			}
		} else {
			set(6);
		}
		direction = -1;
		error = 2;
		break;
	case 14:
		if (!have || (view.clearAhead && angleLeft)) {
			set(6);
		} else if (back < needed) {
			set(5);
		}
		direction = -1;
		error = -2;
		break;
	default:
		if (switchState > 14) {
			break;
		}
		if (boxed) {
			cap = s.maxSpeed * 0.5f;
		} else if (c >= -0.2f && front >= needed) {
			if (found && !st.avoiding) {
				set(6);
			}
		} else if (remaining <= 10 * r && c < -0.6f && back > needed) {
			set(8);
		} else if (c < 0 && !roomy) {
			set(12);
		} else {
			set(pointLeft ? 9 : 11);
		}
		break;
	}
	cap = std::min(cap, obstacleCap);
	if (swerve) {
		error = -2;
	}

	// The speed wanted (CarMoveTo / TankMoveTo): braking for the end of the
	// path, slowing for bends.
	const float speed = std::fabs(in.speed);
	float brake = 1;
	if (speed > s.backingSpeed && s.brakeDistance > 0) {
		const float space = std::max(0.0f, remaining - s.centreRadius - 0.5f);
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
	// The controllers: the error mirrored when moving against the way wanted
	// (over 1 m/s); the speed wanted signed by the way, capped (keeping the
	// cap's sign: a negative cap brakes).
	const float speed2 = hasVelocity ? std::sqrt(in.velocity.x * in.velocity.x + in.velocity.z * in.velocity.z) : speed;
	if (static_cast<float>(direction) != motion && speed2 > 1) {
		error = -error;
	}
	float target = targetSpeed * static_cast<float>(direction);
	float throttle = 0;
	if (!s.tank) {
		// The car's controller (0x100e9d40).
		out.turn = std::clamp(3 * s.yawScale * error - static_cast<float>(direction) * in.yawRate, -1.0f, 1.0f);
		if (in.tilt > 0.25f) {
			target *= 1 - 0.5f * std::min(1.0f, (in.tilt - 0.25f) / 0.7f);
		}
		if (cap < std::fabs(target)) {
			target = sign(target) * cap;
		}
		throttle = std::clamp(target / 15 + std::min(1.0f, (target - in.speed) / 3), -1.0f, 1.0f);
		if (target == s.maxSpeed && speed > 0.8f * s.maxSpeed) {
			throttle = 1;
		} else if (throttle != 0 && speed < 1.5f) {
			throttle = sign(throttle); // a kick off the line
		}
	} else {
		// The tank's (0x100e0540): no turning below 1 m/s, less below 2 m/s.
		float e = 0;
		if (speed >= 1) {
			const float k = std::min(1.0f, speed / 2.0f);
			e = error * k * k;
		}
		out.turn = std::clamp(3 * e, -1.0f, 1.0f);
		if (cap < std::fabs(target)) {
			target = sign(target) * cap;
		}
		const float x = std::min(1.0f, (target - in.speed) / 3);
		throttle = std::clamp(target / 15 + sign(x) * x * x, -1.0f, 1.0f);
		if (throttle != 0 && speed < 1.5f) {
			throttle = sign(throttle);
		}
	}
	out.throttle = throttle;
	return out;
}

} // namespace dice::hfe::ai
