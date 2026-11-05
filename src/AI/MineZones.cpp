#include "AI/MineZones.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <set>

namespace dice::hfe::ai {

namespace {

float length2(const Vec3& v) { return v.x * v.x + v.y * v.y + v.z * v.z; }

bool circleTouches(const NavMesh& mesh, uint32_t polygon, const Vec3& centre, float radius)
{
	const uint32_t n = mesh.polygons[polygon].vertexCount;
	const float r2 = radius * radius;
	for (uint32_t k = 0; k < n; ++k) {
		if (length2(mesh.vertex(polygon, k) - centre) <= r2) {
			return true;
		}
	}
	for (uint32_t k = 0; k < n; ++k) {
		const Vec3& a = mesh.vertex(polygon, k);
		const Vec3& b = mesh.vertex(polygon, (k + n - 1) % n);
		const Vec3 e = b - a;
		const float len = std::sqrt(length2(e));
		if (len <= 0) {
			continue;
		}
		const Vec3 u = e * (1 / len);
		const Vec3 c = centre - a;
		const float t = c.x * u.x + c.y * u.y + c.z * u.z;
		if (t >= 0 && t <= len && length2(c - u * t) <= r2) {
			return true;
		}
	}
	return false;
}

float cross2(float ax, float az, float bx, float bz) { return ax * bz - az * bx; }

// 0x100a88d0: the point inside the triangle on x/z.
bool insideTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c)
{
	const float d1 = cross2(b.x - a.x, b.z - a.z, p.x - a.x, p.z - a.z);
	const float d2 = cross2(c.x - b.x, c.z - b.z, p.x - b.x, p.z - b.z);
	const float d3 = cross2(a.x - c.x, a.z - c.z, p.x - c.x, p.z - c.z);
	const bool negative = d1 < 0 || d2 < 0 || d3 < 0, positive = d1 > 0 || d2 > 0 || d3 > 0;
	return !(negative && positive);
}

// 0x100a8300: the segments cross with both parameters in [0, 1).
bool segmentsCross(const Vec3& p, const Vec3& p2, const Vec3& q, const Vec3& q2)
{
	const float rx = p2.x - p.x, rz = p2.z - p.z, sx = q2.x - q.x, sz = q2.z - q.z;
	const float d = cross2(rx, rz, sx, sz);
	if (std::fabs(d) < 1e-9f) {
		return false;
	}
	const float t = cross2(q.x - p.x, q.z - p.z, sx, sz) / d;
	const float u = cross2(q.x - p.x, q.z - p.z, rx, rz) / d;
	return t >= 0 && t < 1 && u >= 0 && u < 1;
}

bool triangleTouches(const NavMesh& mesh, uint32_t polygon, const Vec3& p0, const Vec3& p1, const Vec3& p2)
{
	const uint32_t n = mesh.polygons[polygon].vertexCount;
	for (uint32_t k = 0; k < n; ++k) {
		if (std::fabs(mesh.vertex(polygon, k).y - p0.y) > 2.0f) {
			return false;
		}
	}
	for (uint32_t k = 0; k < n; ++k) {
		if (insideTriangle(mesh.vertex(polygon, k), p0, p1, p2)) {
			return true;
		}
	}
	const Vec3* tri[3] = {&p0, &p1, &p2};
	for (uint32_t k = 0; k < n; ++k) {
		const Vec3& a = mesh.vertex(polygon, k);
		const Vec3& b = mesh.vertex(polygon, (k + 1) % n);
		for (int j = 0; j < 3; ++j) {
			if (segmentsCross(a, b, *tri[j], *tri[(j + 1) % 3])) {
				return true;
			}
		}
	}
	return false;
}

template <typename Test> std::vector<uint32_t> flood(const NavMesh& mesh, uint32_t start, Test touches)
{
	std::vector<uint32_t> out {start};
	std::set<uint32_t> visited {start};
	std::deque<uint32_t> open {start};
	while (!open.empty()) {
		const uint32_t p = open.front();
		open.pop_front();
		for (uint32_t k = 0; k < mesh.polygons[p].vertexCount; ++k) {
			const int32_t next = mesh.neighbour(p, k);
			if (next < 0 || !visited.insert(static_cast<uint32_t>(next)).second) {
				continue;
			}
			if (touches(static_cast<uint32_t>(next))) {
				out.push_back(static_cast<uint32_t>(next));
				open.push_back(static_cast<uint32_t>(next));
			}
		}
	}
	return out;
}

} // namespace

std::vector<uint32_t> floodCircle(const NavMesh& mesh, uint32_t start, const Vec3& centre, float radius)
{
	return flood(mesh, start, [&](uint32_t p) { return circleTouches(mesh, p, centre, radius); });
}

std::vector<uint32_t> floodTriangle(const NavMesh& mesh, uint32_t start, const Vec3& p0, const Vec3& p1, const Vec3& p2)
{
	return flood(mesh, start, [&](uint32_t p) { return triangleTouches(mesh, p, p0, p1, p2); });
}

void MineMarks::mark(NavMesh& navMesh, int mesh, uint32_t polygon, bool counterB)
{
	const auto key = std::make_pair(mesh, polygon);
	const auto it = m_records.find(key);
	if (it == m_records.end()) {
		m_records[key] = counterB ? std::make_pair(0, 1) : std::make_pair(1, 0);
		return;
	}
	int& counter = counterB ? it->second.second : it->second.first;
	if (counter == 0 && polygon < navMesh.polygons.size()) {
		navMesh.polygons[polygon].costClass = 7;
	}
	++counter;
}

void MineMarks::unmark(int mesh, uint32_t polygon, bool counterB)
{
	const auto it = m_records.find({mesh, polygon});
	if (it == m_records.end()) {
		return;
	}
	int& counter = counterB ? it->second.second : it->second.first;
	counter = std::max(0, counter - 1);
	if (it->second.first == 0 && it->second.second == 0) {
		m_records.erase(it);
	}
}

} // namespace dice::hfe::ai
