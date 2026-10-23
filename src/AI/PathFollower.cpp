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

namespace {

// Where a ray leaves a polygon (0x100b82a0), on the x/z plane: the line
// from `origin` along `dir` (always from the ray's origin, not from where
// it entered) against each edge but the one back into `previous` (none
// skipped for -1). A crossing ahead (t >= 0) within the edge is taken when
// clear of its ends (0.001 < s < 0.999), or near an end when the ray heads
// out of the polygon (dot(dir, (e.z, -e.x)) < 0, e = b - a). Otherwise the
// edge missed by the least (|t| behind, -s or s - 1 off its ends) under
// 0.001 (0x10128e30) gives the point, with no edge to leave by
// (`edge` = the vertex count). `clearance` gets min(s, 1 - s) x the edge's
// length (x/z): the room beside the crossing point.
bool exitPoint(const NavMesh& mesh, uint32_t polygon, int32_t previous, const Vec3& origin, const Vec3& dir, Vec3& exit,
	uint32_t& edge, float& clearance)
{
	const Polygon& p = mesh.polygons[polygon];
	const Vec3 end {origin.x + dir.x, origin.y, origin.z + dir.z};
	float bestMiss = 9999; // 0x47c34f80
	bool nearMiss = false;
	Vec3 nearA, nearB;
	float nearS = 0;
	for (uint32_t k = 0; k < p.vertexCount; ++k) {
		if (previous != -1 && mesh.neighbour(polygon, k) == previous) {
			continue;
		}
		const Vec3& a = mesh.vertex(polygon, k);
		const Vec3& b = mesh.vertex(polygon, (k + 1) % p.vertexCount);
		// 0x100a8300: t along the ray, s along the edge.
		const float dx = end.x - origin.x, dz = end.z - origin.z;
		const float ex = b.x - a.x, ez = b.z - a.z;
		const float denom = cross2(dx, dz, ex, ez);
		if (std::fabs(denom) < 1e-12f) {
			continue;
		}
		const float t = cross2(a.x - origin.x, a.z - origin.z, ex, ez) / denom;
		const float sOn = cross2(a.x - origin.x, a.z - origin.z, dx, dz) / denom;
		if (t >= 0 && sOn >= 0 && sOn <= 1) {
			const bool inside = sOn > 0.001f && sOn < 0.999f;
			if (inside || dir.x * ez + dir.z * -ex < 0) {
				exit = lerp(a, b, sOn);
				edge = k;
				clearance = std::min(sOn, 1 - sOn) * std::sqrt(ex * ex + ez * ez);
				return true;
			}
			continue;
		}
		float miss = t < 0 ? std::fabs(t) : 9999.0f;
		if (sOn < 0) {
			miss = std::min(miss, -sOn);
		} else if (sOn > 1) {
			miss = std::min(miss, sOn - 1);
		}
		if (miss < 0.001f && miss < bestMiss) {
			bestMiss = miss;
			nearMiss = true;
			nearA = a;
			nearB = b;
			nearS = sOn;
		}
	}
	if (nearMiss) {
		exit = lerp(nearA, nearB, std::clamp(nearS, 0.0f, 1.0f));
		edge = p.vertexCount;
	}
	return false;
}

} // namespace

bool steepAgainst(const Vec3& normal, const Vec3& dir, float maxSlope)
{
	// 0x1003c220: the way across the ground (0x10026a20), against the normal.
	float fx = dir.x, fz = dir.z;
	const float l = std::sqrt(fx * fx + fz * fz);
	if (l > 0) {
		fx /= l;
		fz /= l;
	}
	if (normal.x * fx + normal.z * fz >= 0) {
		return false; // level, or downhill
	}
	if (normal.y < 0) {
		return true;
	}
	return std::acos(std::clamp(normal.y, -1.0f, 1.0f)) > maxSlope;
}

// 0x1003f930 -> 0x1003ce50 (with the climb test) or 0x1003d1c0 (without),
// with the edge step 0x100b82a0.
bool meshRaycast(const NavMesh& mesh, const Vec3& from, const Vec3& to, const CostProfile* profile, int maxPolygons,
	float* reached, int32_t startPolygon, float width, uint8_t stopClass, const ClimbLimits* climb, RayHit* hit)
{
	if (reached) {
		*reached = 0;
	}
	if (hit) {
		*hit = RayHit();
	}
	int32_t current = startPolygon >= 0 ? startPolygon : mesh.findPolygon(from, 5.0f, profile);
	if (current < 0) {
		return false;
	}
	if (maxPolygons == -1) {
		maxPolygons = 1000;
	}
	// 0x1003ce50 first puts the start on the polygon's surface: the offset
	// 0x100b88e0 measures is zeroed when the start stands on it within
	// 2.5 m, and otherwise the original adds a local it never wrote (the
	// offset is only written on success); the start is kept here either way.
	const Vec3 d = to - from;
	const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	const float flat = lengthXZ(from, to);
	if (hit) {
		hit->started = true;
		hit->point = from;
	}
	if (length <= 1e-6f) { // 0x10114118
		if (hit) {
			hit->point = to;
		}
		return true;
	}
	const Vec3 dir = d * (1 / length);
	Vec3 out = from;
	const auto done = [&](bool clear) {
		if (clear) {
			out = to;
		}
		if (reached) {
			*reached = clear ? flat : lengthXZ(from, out);
		}
		if (hit) {
			hit->point = out;
		}
		return clear;
	};
	int32_t previous = -1;
	bool climbing = false; // bVar1: on a run of steep polygons
	float climbFrom = 0;   // local_1c: the height the run began at
	for (int n = 0;; ++n) {
		if (n >= maxPolygons) {
			return done(false);
		}
		const bool steep = climb && steepAgainst(mesh.polygons[static_cast<size_t>(current)].normal, dir, climb->maxSlope);
		if (!steep) {
			climbing = false;
		} else if (!climbing) {
			climbFrom = out.y;
			climbing = true;
		}
		Vec3 exit = out;
		uint32_t edge = 0;
		float clearance = 1e9f;
		const bool ok = exitPoint(mesh, static_cast<uint32_t>(current), previous, from, dir, exit, edge, clearance);
		if (climbing && ok && exit.y - climbFrom > climb->stepHeight) {
			return done(false); // too steep a climb: the point stays where the run began
		}
		out = exit;
		if (!ok) {
			return done(previous == -1); // the end inside the first polygon
		}
		previous = current;
		const Vec3 e = exit - from;
		if (std::sqrt(e.x * e.x + e.y * e.y + e.z * e.z) + 0.05f >= length // 0x1011390c
			|| distance2XZ(to, exit) <= 0.025f) {                             // 0x10114110
			return done(true);
		}
		// The room beside the crossing (the original compares 2 x clearance
		// with a local it never sets as well; the edge's length is meant,
		// which always passes).
		if (width != 0 && clearance < width) {
			return done(false);
		}
		const int32_t next = mesh.neighbour(static_cast<uint32_t>(current), edge);
		if (next < 0) {
			return done(false);
		}
		const uint8_t costClass = mesh.polygons[static_cast<size_t>(next)].costClass;
		if (hit) {
			hit->lastClass = costClass;
		}
		if (profile && profile->cost(costClass) < 0) {
			return done(false);
		}
		if (stopClass != 0 && (costClass == stopClass || ((stopClass == 5 || stopClass == 6) && costClass == 7))) {
			return done(false);
		}
		current = next;
	}
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
	m_clusterPath.clear();
	m_cursor = 0;
	// Timers (0x100923a0).
	m_lastInput = m_lastProgress = now;
	m_progressAt = from;
	m_bendAt = -100;
	m_lastClear = now;
	const Vec3 d = m_goal - from;
	const float distance2 = d.x * d.x + d.y * d.y + d.z * d.z;
	const CostProfile* profile = &m_settings.options.profile;
	// A straight walk first (0x100952c0): under 200 m (0x101287b0), the
	// mesh ray to the goal not entering cost class 5 (or 7).
	if (distance2 < 40000 && meshRaycast(mesh, from, m_goal, profile, -1, nullptr, -1, 0, 5, &m_settings.climb)) {
		Waypoint w;
		w.p = m_goal;
		w.polygon = mesh.findPolygon(m_goal, m_settings.options.heightTolerance, profile);
		m_waypoints.push_back(w);
		m_startPolygon = mesh.findPolygon(from, m_settings.options.heightTolerance, profile);
		return;
	}
	// The start and the goal on the mesh (vslot 0x38, 0x1003f2d0: the
	// polygon each stands on; none fails the request, 0x100966c0).
	const int32_t start = mesh.findPolygon(from, m_settings.options.heightTolerance, profile);
	const int32_t goal = mesh.findPolygon(m_goal, m_settings.options.heightTolerance, profile);
	if (start < 0 || goal < 0) {
		m_failed = true;
		return;
	}
	m_startPolygon = start;
	m_goalPolygon = goal;
	m_requestFrom = from;
	// Over 100 m (+0x8c): the cluster search first (0x10095620).
	if (distance2 > 10000 && m_settings.options.useClusters && !mesh.clusters.empty()) {
		m_clusterPath = findClusterRoute(mesh, static_cast<uint32_t>(start), from, static_cast<uint32_t>(goal), m_goal,
			m_settings.options);
		if (m_clusterPath.empty()) {
			m_failed = true;
			return;
		}
	}
	planSegment(mesh, from, now);
}

// One polygon search (0x100962e0, its result taken by 0x10095b20): from
// the last waypoint (the request's start for the first) to the link into
// the cluster two steps on along the cluster path (0x10092250), or to the
// goal when there is none (the last segment). The search may only enter
// the start's cluster, the cluster path's next three from the cursor and
// the segment goal's cluster (+0xb4: the successors 0x100bcbb0 refuse any
// other). A trip of 100 m or less has no cluster path: only the start's
// and the goal's clusters. Each node of the path becomes a waypoint (the
// first replacing the last one kept); the goal is added on the last
// segment. Then the smoothing, the look-ahead twice and the cursor on.
bool PathFollower::planSegment(const NavMesh& mesh, const Vec3& position, float now)
{
	Vec3 from = m_requestFrom;
	int32_t fromPolygon = m_startPolygon;
	if (!m_waypoints.empty()) {
		from = m_waypoints.back().p;
		fromPolygon = m_waypoints.back().polygon;
	}
	Vec3 to = m_goal;
	int32_t toPolygon = m_goalPolygon;
	bool last = true;
	if (m_cursor + 2 < m_clusterPath.size()) {
		if (const ClusterLink* link = entryLink(mesh, m_clusterPath[m_cursor + 2])) {
			to = link->position;
			toPolygon = static_cast<int32_t>(link->polygon);
			last = false;
		}
	}
	if (fromPolygon < 0 || toPolygon < 0) {
		m_failed = true;
		return false;
	}
	std::vector<bool> allowed(mesh.clusters.size(), false);
	const auto allow = [&](size_t cluster) {
		if (cluster < allowed.size()) {
			allowed[cluster] = true;
		}
	};
	allow(mesh.polygons[static_cast<size_t>(fromPolygon)].cluster);
	for (size_t i = 0; i < 3 && m_cursor + i < m_clusterPath.size(); ++i) {
		allow(m_clusterPath[m_cursor + i].cluster);
	}
	allow(mesh.polygons[static_cast<size_t>(toPolygon)].cluster);
	std::vector<PathStep> steps = findPolygonPath(mesh, static_cast<uint32_t>(fromPolygon), from,
		static_cast<uint32_t>(toPolygon), to, m_settings.options, mesh.clusters.empty() ? nullptr : &allowed);
	if (steps.empty() && !mesh.clusters.empty()) {
		// The original fails the move here (the search's state 3/4 makes
		// getSteer 0x10095720 answer false), which a trip of 100 m or less
		// (no cluster path: only the start's and the goal's clusters,
		// 0x100962e0 putting 0xffff for the path's steps) that the mesh
		// ray can't walk straight always does when it needs a third
		// cluster. On 2142's meshes (clusters 7 to 26 m across) that is
		// most short trips: of random 20-100 m trips, 312 of 321 on
		// Cerbere_Landing, 351 of 393 on Camp Gibraltar, 255 of 397 on
		// Minsk, all of which have a path. Failing them would leave the
		// bots replanning in place, so the rewrite deviates and searches
		// the whole mesh.
		steps = findPolygonPath(mesh, static_cast<uint32_t>(fromPolygon), from, static_cast<uint32_t>(toPolygon), to,
			m_settings.options);
	}
	if (steps.empty()) {
		m_failed = true;
		return false;
	}
	std::vector<Waypoint> segment = pathWaypoints(mesh, steps, to);
	segment.pop_back(); // the segment's end point: the goal only on the last
	Waypoint first;
	first.p = steps.front().position;
	first.a = first.b = first.p;
	first.polygon = static_cast<int32_t>(steps.front().polygon);
	if (m_waypoints.empty()) {
		m_waypoints.push_back(first);
	} else {
		m_waypoints.back() = first;
	}
	m_waypoints.insert(m_waypoints.end(), segment.begin(), segment.end());
	if (last) {
		Waypoint goal;
		goal.p = m_goal;
		goal.polygon = m_goalPolygon;
		m_waypoints.push_back(goal);
		m_clusterPath.clear();
		m_cursor = 0;
	} else {
		++m_cursor;
	}
	smoothWaypoints(m_waypoints, position, m_settings.radius);
	lookAhead(mesh, position, now);
	lookAhead(mesh, position, now);
	return true;
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

// The look-ahead (0x10093420): within 10 m (20 m at the wheel) of the
// waypoint, skip to the next while the mesh ray to it is clear (8 polygons
// on foot, 30 at the wheel, not into cost class 5 or 7); when none was
// skipped, the ray to the waypoint itself (1000 polygons) says whether the
// bot is blocked. On foot the rays need 0.5 m (0x101287a8) beside each
// edge they cross; at the wheel the width (0x10139514, never set) is 0.
// Both take the map's climb test (their last argument, 1).
void PathFollower::lookAhead(const NavMesh& mesh, const Vec3& position, float now)
{
	if (m_current >= m_waypoints.size()) {
		return;
	}
	const float trigger = m_settings.soldier ? 10.0f : 20.0f;
	const int most = m_settings.soldier ? 1 : 4;
	const float width = m_settings.soldier ? 0.5f : 0.0f;
	const CostProfile* profile = &m_settings.options.profile;
	int skipped = 0;
	if (distance2XZ(position, m_waypoints[m_current].p) < trigger * trigger) {
		while (skipped < most && m_current + 1 < m_waypoints.size()
			&& meshRaycast(mesh, position, m_waypoints[m_current + 1].p, profile, rayPolygons(), nullptr, -1, width, 5,
				&m_settings.climb)) {
			++m_current;
			++skipped;
		}
	}
	if (skipped == 0
		&& !meshRaycast(mesh, position, m_waypoints[m_current].p, profile, -1, nullptr, -1, width, 0, &m_settings.climb)) {
		m_blocked = true; // +0xda: until the path is planned again
	} else {
		m_lastClear = now;
	}
}

PathFollower::Steer PathFollower::steer(const NavMesh& mesh, const Vec3& position, float now)
{
	Steer out;
	if (!m_planned || m_failed || m_waypoints.empty()) {
		return out;
	}
	// Back onto the path (0x10093a20). getSteer ignores what it answers:
	// a bot off its path keeps steering for the current waypoint, and the
	// blocked test below takes over when it can't be reached.
	resync(mesh, position);
	// The next segment while the cluster path goes on (0x10095720): the
	// waypoints cut to ten past the current one and the search from the
	// last (at once here; the original's searches are time-sliced). Not
	// while 30 m or more of path (+0x14c, 3-D from the bot, 0x10093e50) and
	// more than ten waypoints are left, unless the current waypoint lies in
	// the cluster at the cursor (or, as the original compares a cluster id
	// with the cursor's index too, one numbered as the cursor): the
	// segments follow the bot, so the cluster path is still there when it
	// is blocked.
	if (!m_clusterPath.empty() && m_cursor + 1 <= m_clusterPath.size()) {
		bool wait = false;
		if (pathLeft(position) >= 30 && m_current + 10 < m_waypoints.size()) {
			const int32_t polygon = m_waypoints[m_current].polygon;
			const uint32_t cluster = polygon >= 0 ? mesh.polygons[static_cast<size_t>(polygon)].cluster : 0xffffu;
			const uint32_t atCursor = m_cursor < m_clusterPath.size() ? m_clusterPath[m_cursor].cluster : 0xffffu;
			wait = cluster != m_cursor && cluster != atCursor;
		}
		if (!wait) {
			m_waypoints.resize(std::min(m_waypoints.size(), m_current + 10));
			if (!planSegment(mesh, position, now)) {
				return out;
			}
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
	// Blocked too long (0x10095720 -> 0x10095470): the search goes on from
	// the bot's own cluster on the cluster path, or else the move fails.
	if (m_blocked && now - m_lastClear > (m_settings.soldier ? 2.0f : 0.5f)) {
		m_blocked = false;
		m_lastClear = now;
		if (!reroute(mesh, position, now)) {
			m_failed = true;
			return out;
		}
	}
	out.ok = true;
	out.point = m_waypoints[m_current].p;
	out.jump = m_jump;
	m_jump = false;
	return out;
}

// 0x10095470. With more than one step on the cluster path: the waypoints
// dropped, the start moved to where the bot is (its polygon and position,
// 0x100a46c0), and the cursor on the first of the path's steps (all but
// the last) that is the bot's cluster, else on the first a link of the
// bot's cluster leads into (0x1003f7a0 -> 0x100b0810); the next segment's
// search then runs from there (+0xdb, 0x10096780; at once here). Without
// a cluster path (a trip of 100 m or less, or once the last segment was
// planned and the path cleared), or with the bot's cluster neither on nor
// next to it, the follower is reset (0x10095120: +0xd9 cleared), so the
// move fails and the behaviour plans again.
bool PathFollower::reroute(const NavMesh& mesh, const Vec3& position, float now)
{
	const size_t count = m_clusterPath.size();
	if (count <= 1) {
		return false;
	}
	const int32_t polygon = mesh.findPolygon(position, m_settings.options.heightTolerance, &m_settings.options.profile);
	if (polygon < 0) {
		return false;
	}
	m_cursor = 0;
	m_waypoints.clear();
	m_current = 0;
	m_startPolygon = polygon;
	m_requestFrom = position;
	const uint16_t cluster = mesh.polygons[static_cast<size_t>(polygon)].cluster;
	const auto linked = [&](uint16_t to) {
		if (cluster >= mesh.clusters.size()) {
			return false;
		}
		const Cluster& c = mesh.clusters[cluster];
		for (uint32_t k = c.firstLink; k < static_cast<uint32_t>(c.firstLink) + c.linkCount && k < mesh.links.size(); ++k) {
			if (mesh.links[k].targetCluster == to) {
				return true;
			}
		}
		return false;
	};
	bool found = false;
	for (size_t i = 0; i + 1 < count && !found; ++i) {
		found = m_clusterPath[m_cursor].cluster == cluster;
		m_cursor += found ? 0 : 1;
	}
	if (!found) {
		m_cursor = 0;
		for (size_t i = 0; i + 1 < count && !found; ++i) {
			found = linked(m_clusterPath[m_cursor].cluster);
			m_cursor += found ? 0 : 1;
		}
	}
	if (!found) {
		return false;
	}
	return planSegment(mesh, position, now);
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

float PathFollower::pathLeft(const Vec3& position) const
{
	if (m_current >= m_waypoints.size()) {
		return 0;
	}
	float total = distance(position, m_waypoints[m_current].p);
	for (size_t i = m_current + 1; i < m_waypoints.size(); ++i) {
		total += distance(m_waypoints[i - 1].p, m_waypoints[i].p);
	}
	return total;
}

Vec3 PathFollower::pointAhead(const Vec3& position, float distance) const
{
	if (m_waypoints.empty()) {
		return position;
	}
	for (size_t i = m_current; i < m_waypoints.size(); ++i) {
		if (distance * distance < distance2XZ(position, m_waypoints[i].p)) {
			return m_waypoints[i].p;
		}
	}
	return m_waypoints.back().p;
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
	// The velocity look-ahead: 0.25 s with the flag CarMoveTo passes, 0.5 s
	// without it (TankMoveTo, MechMoveTo).
	const float k = s.driver == Driver::Car ? 0.25f : 0.5f;
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
// behind are cut short by other vehicles (0x100fbdb0). The mech's
// (0x100e1f20) diagonals go along forward x (length + 1) +- right x
// (width - 1), and its side rays' results are never stored.
void freeSpace(const DriveSettings& s, const DriveSense& in, float reach, float& front, float& back, float& left,
	float& right)
{
	const bool mech = s.driver == Driver::Mech;
	const float length = mech ? s.length + 1 : s.length;
	const float width = mech ? s.width - 1 : s.width;
	if (!in.ray) {
		front = in.freeFront;
		back = in.freeBack;
		left = in.freeLeft;
		right = in.freeRight;
	} else {
		const Vec3 p = in.position;
		const auto along = [&](const Vec3& d) { return in.ray(p, p + d * reach); };
		const auto distance = [&](const DriveRay& r) { return r.clear ? reach : r.distance; };
		Vec3 frontLeft = in.forward * length - in.right * width;
		Vec3 frontRight = in.forward * length + in.right * width;
		frontLeft = frontLeft * (1 / std::max(length3(frontLeft), 1e-6f));
		frontRight = frontRight * (1 / std::max(length3(frontRight), 1e-6f));
		// The diagonal rays don't count where they end entering cost class
		// 4, and a side ray that does gives the whole reach.
		front = distance(along(in.forward));
		for (const Vec3& d : {frontLeft, frontRight}) {
			const DriveRay r = along(d);
			if (!r.clear && r.lastClass != 4) {
				front = std::min(front, r.distance);
			}
		}
		back = distance(along(in.forward * -1));
		for (const Vec3& d : {frontLeft * -1, frontRight * -1}) {
			const DriveRay r = along(d);
			if (!r.clear && r.lastClass != 4) {
				back = std::min(back, r.distance);
			}
		}
		const DriveRay toRight = along(in.right), toLeft = along(in.right * -1);
		right = toRight.lastClass == 4 ? reach : distance(toRight);
		left = toLeft.lastClass == 4 ? reach : distance(toLeft);
	}
	float unused = 0;
	clipByObstacles(in, in.position, in.forward, unused, back, -1);
	Vec3 frontLeft = in.forward * length - in.right * width;
	Vec3 frontRight = in.forward * length + in.right * width;
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
	const bool car = s.driver == Driver::Car;
	const bool mech = s.driver == Driver::Mech;
	// How close behind a goal must be to back towards it: 10 radii for the
	// car, 5 for the tank and the mech (0x100de8a0, 0x100e1f20).
	const float behind = (car ? 10.0f : 5.0f) * r;
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
	// The mech's machine starts each tick at 0 (turn on the spot) where the
	// car's and the tank's start at 1 (ahead).
	int direction = mech ? 0 : 1;
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
	// Round an obstacle (4/5, 13/14): the car steers at +-2; the tank and
	// the mech at 0.2 x min(1, speed), and they keep going ahead while
	// touching it as long as it is not straight ahead (dot 0.8).
	const float roundError = car ? 2.0f : 0.2f * std::min(1.0f, std::fabs(in.speed));
	switch (switchState) {
	case 1:
	case 7:
	case 10:
		break;
	case 2: // ahead (the mech: on the spot), turning right
		if (pointLeft) {
			set(c <= 0 ? 9 : 3);
		} else if (c <= 0.9f) {
			if (front >= needed && (view.ahead <= 0.5f || !have || view.clearAhead || st.avoiding)) {
				error = std::fabs(error);
				direction = mech ? 0 : 1;
			} else {
				set(11);
			}
		} else {
			set(0);
		}
		break;
	case 3: // ahead (the mech: on the spot), turning left
		if (pointLeft) {
			if (c > 0.9f) {
				set(0);
			} else if (front >= needed && (view.ahead <= 0.5f || !have || view.clearAhead || st.avoiding)) {
				error = -std::fabs(error);
				direction = mech ? 0 : 1;
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
		} else if (front >= needed && (!view.touching || (!car && view.ahead <= 0.8f))) {
			direction = 1;
			error = -roundError;
		} else {
			set(13);
		}
		break;
	case 5:
		if (have && (!view.clearAhead || !angleLeft)) {
			if (front < needed || (view.touching && (car || view.ahead > 0.8f))) {
				set(14);
			} else {
				direction = 1;
				error = roundError;
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
		} else if (c > -0.5f || remaining > behind + 5 || back < needed) {
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
	case 12: // backing out of a narrow spot (not the mech's)
		if (mech) {
			break;
		}
		if (c > 0) {
			set(0);
		} else if (remaining <= behind && c < -0.6f && back > needed) {
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
		error = roundError;
		break;
	case 14:
		if (!have || (view.clearAhead && angleLeft)) {
			set(6);
		} else if (back < needed) {
			set(5);
		}
		direction = -1;
		error = -roundError;
		break;
	case 15: // the mech: turning left on the spot
	case 16: // and right
		if (!mech) {
			break;
		}
		if (c <= 0.9f) {
			direction = 0;
			error = switchState == 15 ? -std::fabs(error) : std::fabs(error);
		} else {
			set(0);
		}
		break;
	default:
		if (switchState > 16) {
			break;
		}
		if (boxed) {
			direction = 1;
			cap = s.maxSpeed * 0.5f;
		} else if (mech) {
			// 0x100e1f20: drives on only nearly facing the point (cos 0.6),
			// else turns on the spot (15/16) or backs (9/11).
			if (found && !st.avoiding && c >= 0) {
				set(6);
			} else if (c >= 0.6f && front >= needed) {
				direction = 1;
			} else if (c < 0 && remaining <= behind && c < -0.6f && back > needed) {
				set(8);
			} else if (!pointLeft) {
				set(front >= needed ? 16 : 11);
			} else {
				set(front >= needed ? 15 : 9);
			}
		} else if (c >= (car ? -0.2f : 0.0f) && front >= needed) {
			if (found && !st.avoiding) {
				set(6);
			}
		} else if (remaining <= behind && c < -0.6f && back > needed) {
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
			if (!car) {
				// TankMoveTo and MechMoveTo, as decompiled: the percentage
				// not scaled, nothing squared.
				brake = std::max(1 - s.maxBrakePercentage, ratio);
			} else {
				const float f = std::max(1 - s.maxBrakePercentage * 0.01f, ratio);
				brake = f * f;
			}
		}
	}
	float angle = 1;
	if (speed > s.minSpeedForLookAhead) {
		// The tank looks at the bend alone; the car at the heading error as
		// well, the mech at the error its machine left (0x1008d250).
		const float a = s.driver == Driver::Tank ? std::fabs(in.maxBend)
			: std::max(std::fabs(in.maxBend), std::fabs(mech ? error : in.error));
		angle = 1 - s.maxAngleDiffSlowDown * std::min(1.0f, s.angleSlowDownScale * std::max(a - s.minAngleDiffForSlowDown, 0.0f));
	}
	const float targetSpeed = std::min(brake, angle) * s.maxSpeed;
	out.direction = direction;
	out.targetSpeed = targetSpeed;
	// The controllers: the error mirrored when moving against the way wanted
	// (over 1 m/s); the speed wanted signed by the way, capped (keeping the
	// cap's sign: a negative cap brakes).
	const float speed2 = hasVelocity ? std::sqrt(in.velocity.x * in.velocity.x + in.velocity.z * in.velocity.z) : speed;
	if (direction != 0 && static_cast<float>(direction) != motion && speed2 > 1) {
		error = -error;
	}
	float target = targetSpeed * static_cast<float>(direction);
	float throttle = 0;
	if (mech) {
		// The mech's controller (0x100e3c00): full lock (yawScale x error,
		// raised to at least 1; turning on the spot, yawScale x its sign)
		// and the throttle at the direction, no speed regulation and no cap:
		// the speed wanted only says when to sprint (at maxSpeed and over 0.7
		// of it). The original writes yawScale (15 for the mechs) unclamped;
		// the rewrite's inputs are clamped to +-1.
		if (direction == 0) {
			out.turn = std::clamp(s.yawScale * sign(error), -1.0f, 1.0f);
			throttle = 0;
		} else {
			float y = s.yawScale * error;
			if (y != 0 && std::fabs(y) < 1) {
				y = sign(y);
			}
			out.turn = std::clamp(y, -1.0f, 1.0f);
			throttle = static_cast<float>(direction);
			if (targetSpeed == s.maxSpeed && in.speed > 0.7f * s.maxSpeed) {
				out.sprint = true;
				throttle = 1;
			}
		}
	} else if (car) {
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
			out.sprint = true; // PISprint (input 13)
		} else if (throttle != 0 && speed < 1.5f) {
			throttle = sign(throttle); // a kick off the line
		}
	} else {
		// The tank's (0x100e0540): no turning below 1 m/s across the ground,
		// less below 2 m/s.
		float e = 0;
		if (speed2 >= 1) {
			const float k = std::min(1.0f, speed2 / 2.0f);
			e = error * k * k;
		}
		out.turn = std::clamp(3 * e, -1.0f, 1.0f);
		if (cap < std::fabs(target)) {
			target = sign(target) * cap;
		}
		const float x = std::min(1.0f, (target - speed2 * motion) / 3);
		throttle = std::clamp(target / 15 + sign(x) * x * x, -1.0f, 1.0f);
		if (throttle != 0 && speed2 < 1.5f) {
			throttle = sign(throttle);
		}
	}
	out.throttle = throttle;
	return out;
}

namespace {

const SidestepObstacle* findSidestepObstacle(const SidestepSense& in, int id)
{
	for (const SidestepObstacle& o : in.obstacles) {
		if (o.id == id && id != -1) {
			return &o;
		}
	}
	return nullptr;
}

// x/z unit vector (0x100643c0: zero when too short).
Vec3 unit2(float x, float z)
{
	const float l2 = x * x + z * z;
	if (l2 < 1e-12f) {
		return {};
	}
	const float l = std::sqrt(l2);
	return {x / l, 0, z / l};
}

// 0x100e63c0: the nearest object ahead on the way to `ahead` (along the
// way, more than 0.1 of its distance ahead, within 2 m of the bot's height
// and passing within its room + 1 m of the line; the bot itself, the move's
// object unless it is to be stepped round, soldiers, and objects with
// neither a Mobile part nor ITObstacle left out). Its room: its centre
// radius, or for one that is fixed or below the wreck health (unless the
// bot's orders head for it) max(r, min(10, r + 4)) x 1.3. Mesh rays 15 m
// to either side of it across the way (from its own polygon, the original's
// AI position on the map for a moving one; a fixed one off the mesh ends the
// move: true) measure the room there; the bot passes on the side away from
// it when that has more than room + 1 m (else on the roomier side), at
// min(that room, its room) from it.
bool findSidestep(SidestepState& st, const SidestepSense& in)
{
	const float px = in.position.x, pz = in.position.z;
	const float wx = in.ahead.x - px, wz = in.ahead.z - pz;
	const float wl = std::sqrt(wx * wx + wz * wz);
	if (wl < 1e-4f) { // 0x10128c90
		return false;
	}
	const float dx = wx / wl, dz = wz / wl;
	const float rx = dz, rz = -dx; // 0x100a8290
	st.distance = 0;               // *param_7
	float best = 99999;
	const SidestepObstacle* chosen = nullptr;
	for (const SidestepObstacle& o : in.obstacles) {
		if (o.id == in.self || (o.id == in.target && !in.avoidTarget) || o.soldier || (!o.mobile && !o.fixed)) {
			continue;
		}
		const bool related = o.id == in.related[0] || o.id == in.related[1];
		bool still = false;
		float r = o.radius; // +0x40 vt+0x30
		if (o.fixed || o.health < in.wreckHealth) {
			still = true;
			if (!related) {
				r = std::max(o.radius, std::min(10.0f, o.radius + 4)) * 1.3f;
			}
		}
		if (std::fabs(o.position.y - in.position.y) > 2) {
			continue;
		}
		const float ox = o.position.x - px, oz = o.position.z - pz;
		const float along = ox * dx + oz * dz;
		const float d2 = ox * ox + oz * oz;
		const float reach = r + 1;
		if (reach * reach < d2 || (d2 > 0 && along / std::sqrt(d2) < 0.1f) || reach <= std::fabs(ox * rx + oz * rz)
			|| best <= along) {
			continue;
		}
		best = along;
		chosen = &o;
		st.radius = r;            // *param_9
		st.obstacle = o.id;       // *param_6
		st.at = {o.position.x, 0, o.position.z}; // *param_8
		st.still = still;         // *param_10
	}
	if (!chosen) {
		return false;
	}
	const Vec3 at = chosen->position;
	int32_t start = -1;
	if (in.mesh) {
		start = in.mesh->findPolygon(at, in.heightTolerance, in.profile);
		if (!chosen->mobile && start < 0) {
			return true; // a fixed obstacle off the mesh (vt+0x30 failed)
		}
	}
	const auto room = [&](float sign) {
		if (!in.mesh) {
			return 0.0f;
		}
		RayHit hit;
		meshRaycast(*in.mesh, at, at + Vec3 {rx, 0, rz} * (15 * sign), in.profile, -1, nullptr, start, 0, 0, nullptr, &hit);
		if (!hit.started || (std::fabs(hit.point.x) <= 1.1920929e-07f && std::fabs(hit.point.y) <= 1.1920929e-07f
								&& std::fabs(hit.point.z) <= 1.1920929e-07f)) {
			return 0.0f; // the output left as the original zeroed it
		}
		const Vec3 d = hit.point - at;
		return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	};
	const float right = room(1);  // param_12
	const float left = room(-1);  // fVar19
	st.point = st.at;
	const float offset = (st.at.x - px) * rx + (st.at.z - pz) * rz;
	bool negative = false;
	float space = right;
	if (offset <= 0) {
		if (!(st.radius + 1 < right || right > left)) {
			negative = true;
			space = left;
		}
	} else if (st.radius + 1 < left || right < left) {
		negative = true;
		space = left;
	}
	space = std::min(space, st.radius);
	const float s = negative ? -space : space;
	st.point = {st.at.x + rx * s, 0, st.at.z + rz * s};
	st.distance = space;
	return false;
}

} // namespace

// 0x100ce860 (its time stamp cache, +0x88, only answers a clock that went
// back; the vt+0x1c branch, which hands the steering point to the bot's
// look, runs once the move's +0x18 is set, and MoveToFinding never sets it
// before that branch: neither is here).
bool sidestepSteer(SidestepState& st, const SidestepSense& in, Vec3& direction)
{
	const float px = in.position.x, pz = in.position.z;
	// A new sidestep when there is none, or the obstacle no longer has the
	// point on its rim (0x100e5eb0: the point within [room, room + 1.5] of
	// where it stands now).
	bool search = st.obstacle == -1;
	if (!search) {
		if (const SidestepObstacle* o = findSidestepObstacle(in, st.obstacle)) {
			const float ex = st.point.x - o->position.x, ez = st.point.z - o->position.z;
			const float e2 = ex * ex + ez * ez;
			search = !(st.radius * st.radius <= e2 && e2 <= (st.radius + 1.5f) * (st.radius + 1.5f));
		}
	}
	if (search && findSidestep(st, in)) {
		return true;
	}
	float wx = in.steer.x - px, wz = in.steer.z - pz;
	float length = std::sqrt(wx * wx + wz * wz);
	bool passed = false; // param_1._3_1_: pass it by (go straight for the point)
	const SidestepObstacle* o = findSidestepObstacle(in, st.obstacle);
	if (!o) {
		st.obstacle = -1;
	} else {
		const float tx = st.at.x - px, tz = st.at.z - pz;
		const float d2 = tx * tx + tz * tz;
		if ((st.radius + 1) * (st.radius + 1) < d2) {
			st.obstacle = -1; // out of its reach
		}
		const bool related = st.obstacle != -1 && (st.obstacle == in.related[0] || st.obstacle == in.related[1]);
		passed = related;
		if (st.obstacle != -1) {
			// Behind on the way to the point, or (to an object) farther than the point.
			if (!related && !st.still && (tx * wx + tz * wz < 0 || (in.target != -1 && length * length <= d2))) {
				passed = true;
			}
			const float mx = st.at.x - o->position.x, mz = st.at.z - o->position.z;
			if (mx * mx + mz * mz >= 0.25f) {
				st.obstacle = -1; // it moved half a metre
			} else {
				// The goal inside its room and the bot about as close as it gets.
				const float gx = o->position.x - in.goal.x, gz = o->position.z - in.goal.z;
				const float goalOff = std::sqrt(gx * gx + gz * gz);
				const float limit = (st.radius - goalOff + 0.8f) * (st.radius - goalOff + 0.8f);
				const float toGoal2 = (in.goal.x - px) * (in.goal.x - px) + (in.goal.z - pz) * (in.goal.z - pz);
				const float reach2 = (st.radius + 1) * (st.radius + 1);
				if (st.still && goalOff < st.radius && (toGoal2 < limit || (st.distance < st.radius && d2 <= reach2))) {
					direction = {wx, 0, wz};
					return true;
				}
				if ((!passed || related) && goalOff < st.radius && toGoal2 < limit) {
					direction = {wx, 0, wz};
					return true;
				}
				wx = st.point.x - px;
				wz = st.point.z - pz;
				length = std::sqrt(wx * wx + wz * wz);
			}
		}
	}
	if (st.obstacle != -1) {
		if (length < 0.3f) {
			st.obstacle = -1; // at the point
		}
		if (st.obstacle != -1 && !passed && length > 0.5f) {
			const Vec3 a = unit2(st.point.x - px, st.point.z - pz);
			const Vec3 b = unit2(st.at.x - px, st.at.z - pz);
			if (b.x * a.x + b.z * a.z <= 0.05f) {
				direction = {wx / length, 0, wz / length};
			} else {
				// Round it: towards its rim at the sidestep distance, on the
				// side a metre towards the point lies.
				const Vec3 c = unit2(a.x + px - st.at.x, a.z + pz - st.at.z);
				direction = unit2(c.x * st.distance + st.at.x - px, c.z * st.distance + st.at.z - pz);
			}
			return false;
		}
	}
	direction = length <= 0.001f ? Vec3 {1, 0, 0} : Vec3 {wx / length, 0, wz / length};
	return false;
}

float tankStopThrottle(float forwardSpeed)
{
	const float t = -std::clamp(forwardSpeed, -0.7f, 0.7f); // 0x1012abf8
	if (std::fabs(t) <= 0.05f) {
		return 0;
	}
	return std::fabs(t) <= 0.4f ? 0.4f * sign(t) : t;
}

// 0x1008c7c0 and its controller 0x100de350. The speed wanted: the bend's
// slowing as the car's (|maxBend| only, above minSpeedForLookAhead), the
// brake for the path's end as the car's (the percentage x 0.01, squared)
// but at any speed, and backingSpeed as the floor. The loop runs in the
// frame the tank will have half a second on (turning over 0.5 rad/s):
// along its forward and right axes, (wanted - current) x 1 - acceleration,
// x 3; 10 towards the way wanted while moving against it (the sign of 0
// is 0). The original writes them unclamped; the rewrite's inputs are
// clamped to +-1.
FloatingTankOutput floatingTankControl(const DriveSettings& s, const DriveSense& in, const Vec3& direction,
	const Vec3& acceleration)
{
	FloatingTankOutput out;
	float bend = 1;
	if (std::fabs(in.speed) > s.minSpeedForLookAhead) {
		bend = 1 - s.maxAngleDiffSlowDown
				* std::min(1.0f, s.angleSlowDownScale * std::max(std::fabs(in.maxBend) - s.minAngleDiffForSlowDown, 0.0f));
	}
	float brake = 1;
	const float space = std::max(0.0f, in.remaining - s.centreRadius - 0.5f);
	if (s.brakeDistance > 0 && space * space < s.brakeDistance * s.brakeDistance) {
		brake = std::max(1 - 0.01f * s.maxBrakePercentage, space * space / (s.brakeDistance * s.brakeDistance));
	}
	const float want = std::max(std::min(brake * brake, bend) * s.maxSpeed, s.backingSpeed);
	out.targetSpeed = want;
	Vec3 f = in.forward;
	Vec3 v = in.velocity;
	Vec3 a = acceleration;
	if (std::fabs(in.yawRate) > 0.5f) {
		const float turn = 0.5f * in.yawRate; // 0x1012ab98: half a second
		const float cs = std::cos(turn), sn = std::sin(turn);
		const auto rotate = [&](const Vec3& p) { return Vec3 {p.x * cs + p.z * sn, p.y, -p.x * sn + p.z * cs}; };
		f = rotate(f);
		v = rotate(v);
		a = rotate(a);
	}
	const Vec3 r {f.z, 0, -f.x}; // 0x100a8290
	const float wantF = dot2(f, direction) * want, wantR = dot2(r, direction) * want;
	const float curF = dot2(f, v), curR = dot2(r, v);
	float throttle = ((wantF - curF) * 1.0f - dot2(f, a)) * 3.0f; // 0x1012ab94, 0x1012ab90
	float strafe = ((wantR - curR) * 1.0f - dot2(r, a)) * 3.0f;
	if (sign(curF) != sign(wantF)) {
		throttle = 10 * sign(wantF);
	}
	if (sign(curR) != sign(wantR)) {
		strafe = 10 * sign(wantR);
	}
	out.throttle = std::clamp(throttle, -1.0f, 1.0f);
	out.strafe = std::clamp(strafe, -1.0f, 1.0f);
	return out;
}

} // namespace dice::hfe::ai
