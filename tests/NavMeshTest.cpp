// The bots' navigation mesh: .qtr/.clb parsing, locating points and path
// finding on a small synthetic mesh, then a real level's Infantry mesh when
// $BF2142_GAME_DIR is set.
#include "AI/NavMesh.h"
#include "AI/PathFollower.h"
#include "IO/File/ZipArchive.h"
#include "TestMain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <string>

using namespace dice::hfe;
using ai::Vec3;

namespace {

void put32(std::vector<uint8_t>& out, uint32_t v)
{
	for (int i = 0; i < 4; ++i) {
		out.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}
}

void put16(std::vector<uint8_t>& out, uint16_t v)
{
	out.push_back(static_cast<uint8_t>(v));
	out.push_back(static_cast<uint8_t>(v >> 8));
}

void putF(std::vector<uint8_t>& out, float f)
{
	uint32_t v;
	std::memcpy(&v, &f, 4);
	put32(out, v);
}

void putVec3(std::vector<uint8_t>& out, const Vec3& v)
{
	putF(out, v.x);
	putF(out, v.y);
	putF(out, v.z);
}

// Writes a mesh the way the level tool did.
std::vector<uint8_t> writeQtr(const ai::NavMesh& m)
{
	std::vector<uint8_t> out;
	put32(out, 1);
	put32(out, 3);
	putF(out, m.minX);
	putF(out, m.minZ);
	putF(out, m.maxX);
	putF(out, m.maxZ);
	putF(out, m.rootSize);
	put32(out, static_cast<uint32_t>(m.nodes.size()));
	for (const ai::QuadNode& n : m.nodes) {
		for (uint32_t c : n.children) {
			put32(out, n.leaf ? 0xcdcdcdcd : c);
		}
		out.push_back(n.leaf);
		out.push_back(n.polygonCount);
		out.push_back(0xcd);
		out.push_back(0xcd);
		put32(out, n.firstPolygon);
	}
	put32(out, static_cast<uint32_t>(m.polygons.size()));
	for (const ai::Polygon& p : m.polygons) {
		out.push_back(p.vertexCount);
		out.push_back(p.costClass);
		put16(out, p.cluster);
		put32(out, p.firstEdge);
		putVec3(out, p.normal);
	}
	put32(out, static_cast<uint32_t>(m.leafPolygons.size()));
	for (uint32_t v : m.leafPolygons) {
		put32(out, v);
	}
	put32(out, static_cast<uint32_t>(m.vertices.size()));
	for (const Vec3& v : m.vertices) {
		putVec3(out, v);
	}
	put32(out, static_cast<uint32_t>(m.edgeVertices.size()));
	for (uint32_t v : m.edgeVertices) {
		put32(out, v);
	}
	put32(out, static_cast<uint32_t>(m.edgeNeighbours.size()));
	for (int32_t v : m.edgeNeighbours) {
		put32(out, static_cast<uint32_t>(v));
	}
	return out;
}

std::vector<uint8_t> writeClb(const ai::NavMesh& m)
{
	std::vector<uint8_t> out;
	put32(out, 5);
	put32(out, static_cast<uint32_t>(m.clusters.size()));
	for (const ai::Cluster& c : m.clusters) {
		put32(out, c.firstPolygon);
		put32(out, c.polygonCount);
		put16(out, c.firstLink);
		put16(out, c.linkCount);
		put16(out, c.firstDistance);
		out.push_back(0xcd); // "open", left uninitialised by the tool
		out.push_back(0xcd);
	}
	put32(out, static_cast<uint32_t>(m.clusterPolygons.size()));
	for (uint32_t v : m.clusterPolygons) {
		put32(out, v);
	}
	put32(out, static_cast<uint32_t>(m.links.size()));
	for (const ai::ClusterLink& l : m.links) {
		putVec3(out, l.position);
		put16(out, l.targetCluster);
		put16(out, 0xcdcd);
		put32(out, l.polygon);
	}
	put32(out, static_cast<uint32_t>(m.linkDistances.size()));
	for (float d : m.linkDistances) {
		putF(out, d);
	}
	return out;
}

constexpr int kWidth = 9, kDepth = 5;

// A flat 9 x 5 m floor at y = 0 of two triangles per square metre, in
// three clusters of three columns each. Column 4 is blocked (cost class 0)
// except its top row, so paths along z = 0.5 must go round.
ai::NavMesh buildGrid()
{
	ai::NavMesh m;
	m.minX = 0;
	m.minZ = 0;
	m.maxX = kWidth;
	m.maxZ = kDepth;
	m.rootSize = kWidth;
	for (int z = 0; z <= kDepth; ++z) {
		for (int x = 0; x <= kWidth; ++x) {
			m.vertices.push_back({float(x), 0, float(z)});
		}
	}
	auto vid = [](int x, int z) { return static_cast<uint32_t>(z * (kWidth + 1) + x); };
	std::vector<std::array<uint32_t, 3>> triangles;
	for (int z = 0; z < kDepth; ++z) {
		for (int x = 0; x < kWidth; ++x) {
			// Clockwise seen from above, like the shipped data.
			triangles.push_back({vid(x, z), vid(x, z + 1), vid(x + 1, z + 1)});
			triangles.push_back({vid(x, z), vid(x + 1, z + 1), vid(x + 1, z)});
			for (int t = 0; t < 2; ++t) {
				ai::Polygon p;
				p.vertexCount = 3;
				p.costClass = (x == 4 && z < kDepth - 1) ? 0 : 1;
				p.cluster = static_cast<uint16_t>(x / 3);
				p.firstEdge = static_cast<uint32_t>(m.edgeVertices.size() + t * 3);
				p.normal = {0, 1, 0};
				m.polygons.push_back(p);
			}
			for (int t = 0; t < 2; ++t) {
				for (uint32_t v : triangles[triangles.size() - 2 + t]) {
					m.edgeVertices.push_back(v);
				}
			}
		}
	}
	// Neighbours over shared edges.
	std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> edges;
	for (uint32_t p = 0; p < triangles.size(); ++p) {
		for (int k = 0; k < 3; ++k) {
			const uint32_t a = triangles[p][k], b = triangles[p][(k + 1) % 3];
			edges[{std::min(a, b), std::max(a, b)}].push_back(p);
		}
	}
	for (uint32_t p = 0; p < triangles.size(); ++p) {
		for (int k = 0; k < 3; ++k) {
			const uint32_t a = triangles[p][k], b = triangles[p][(k + 1) % 3];
			int32_t other = -1;
			for (uint32_t q : edges[{std::min(a, b), std::max(a, b)}]) {
				if (q != p) {
					other = static_cast<int32_t>(q);
				}
			}
			m.edgeNeighbours.push_back(other);
		}
	}
	// A root with four leaf quarters; each leaf lists every polygon (more
	// than needed, which the lookup tolerates).
	m.nodes.resize(5);
	m.nodes[0].children = {1, 2, 3, 4};
	for (int i = 1; i <= 4; ++i) {
		m.nodes[i].leaf = true;
		m.nodes[i].polygonCount = static_cast<uint8_t>(m.polygons.size());
		m.nodes[i].firstPolygon = static_cast<uint32_t>(m.leafPolygons.size());
		for (uint32_t p = 0; p < m.polygons.size(); ++p) {
			m.leafPolygons.push_back(p);
		}
	}
	// Clusters: polygons by cluster, a link each way at every cluster border
	// (on the top row, z = 4..5, the way round the blocked column).
	for (uint16_t c = 0; c < 3; ++c) {
		ai::Cluster cl;
		cl.firstPolygon = static_cast<uint32_t>(m.clusterPolygons.size());
		for (uint32_t p = 0; p < m.polygons.size(); ++p) {
			if (m.polygons[p].cluster == c) {
				m.clusterPolygons.push_back(p);
			}
		}
		cl.polygonCount = static_cast<uint32_t>(m.clusterPolygons.size()) - cl.firstPolygon;
		cl.firstLink = static_cast<uint16_t>(m.links.size());
		cl.firstDistance = static_cast<uint16_t>(m.linkDistances.size());
		// The border polygon: the lower-right triangle of the square left of
		// the border, top row.
		auto border = [&](int x) { return static_cast<uint32_t>(((kDepth - 1) * kWidth + x) * 2 + 1); };
		if (c > 0) {
			m.links.push_back({{float(c * 3), 0, kDepth - 0.5f}, uint16_t(c - 1), border(c * 3 - 1)});
		}
		if (c < 2) {
			m.links.push_back({{float(c * 3 + 3), 0, kDepth - 0.5f}, uint16_t(c + 1), border(c * 3 + 2)});
		}
		cl.linkCount = static_cast<uint16_t>(m.links.size()) - cl.firstLink;
		if (cl.linkCount == 2) {
			m.linkDistances.push_back(3.0f);
		}
		m.clusters.push_back(cl);
	}
	return m;
}

const std::filesystem::path* gameDir()
{
	static std::filesystem::path dir;
	const char* env = std::getenv("BF2142_GAME_DIR");
	if (!env) {
		return nullptr;
	}
	dir = env;
	return &dir;
}

} // namespace

TEST(NavMesh_ParsesWhatItWrites)
{
	const ai::NavMesh grid = buildGrid();
	std::string error;
	const auto mesh = ai::parseNavMesh(writeQtr(grid), writeClb(grid), &error);
	CHECK(mesh.has_value());
	if (!mesh) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return;
	}
	CHECK(mesh->treeCount == 1 && mesh->version == 3 && mesh->clusterVersion == 5);
	CHECK(mesh->polygons.size() == kWidth * kDepth * 2);
	CHECK(mesh->nodes.size() == 5 && !mesh->nodes[0].leaf && mesh->nodes[1].leaf);
	CHECK(mesh->nodes[0].children[3] == 4);
	CHECK(mesh->clusters.size() == 3 && mesh->clusters[1].linkCount == 2 && mesh->clusters[1].open);
	CHECK(mesh->links[0].targetCluster == 1 && mesh->links[1].targetCluster == 0);
	CHECK(mesh->polygons[8].costClass == 0 && mesh->polygons[8].cluster == 1);
	CHECK(mesh->rootSize == kWidth);
}

TEST(NavMesh_RejectsBadFiles)
{
	const ai::NavMesh grid = buildGrid();
	std::vector<uint8_t> qtr = writeQtr(grid), clb = writeClb(grid);
	std::string error;

	std::vector<uint8_t> wrongVersion = qtr;
	wrongVersion[4] = 2;
	CHECK(!ai::parseNavMesh(wrongVersion, clb, &error));
	CHECK(error.find("version") != std::string::npos);

	std::vector<uint8_t> truncated(qtr.begin(), qtr.end() - 1);
	CHECK(!ai::parseNavMesh(truncated, clb, &error));

	std::vector<uint8_t> badClb = clb;
	badClb[0] = 4;
	CHECK(!ai::parseNavMesh(qtr, badClb, &error));

	// A neighbour index past the polygons.
	ai::NavMesh broken = grid;
	broken.edgeNeighbours[0] = 100000;
	CHECK(!ai::parseNavMesh(writeQtr(broken), clb, &error));
}

TEST(NavMesh_FindsThePolygonUnderAPoint)
{
	const ai::NavMesh m = buildGrid();
	// (0.3, 0.6) is above the diagonal of the first square: its first triangle.
	CHECK(m.findPolygon({0.3f, 0, 0.6f}) == 0);
	CHECK(m.findPolygon({0.6f, 0, 0.3f}) == 1);
	CHECK(m.findLeaf(8.9f, 4.9f) == 4);
	CHECK(m.findLeaf(1.0f, 4.9f) == 2);
	// Up to the tolerance above the floor, up to 2 m under it.
	float offset = 0;
	CHECK(m.findPolygon({0.3f, 3.0f, 0.6f}, 5.0f, nullptr, &offset) == 0);
	CHECK(std::fabs(offset + 3.0f) < 1e-5f);
	CHECK(m.findPolygon({0.3f, 6.0f, 0.6f}) == -1);
	CHECK(m.findPolygon({0.3f, 3.0f, 0.6f}, 2.5f) == -1);
	CHECK(m.findPolygon({0.3f, -1.5f, 0.6f}) == 0);
	CHECK(m.findPolygon({0.3f, -2.5f, 0.6f}) == -1);
	// Outside the bounds.
	CHECK(m.findPolygon({-0.5f, 0, 0.5f}) == -1);
	// A blocked polygon isn't found for a profile that can't walk on it.
	const ai::CostProfile profile;
	CHECK(m.findPolygon({4.3f, 0, 0.6f}) >= 0);
	CHECK(m.findPolygon({4.3f, 0, 0.6f}, 5.0f, &profile) == -1);
}

TEST(NavMesh_LinkDistancesAreAnUpperTriangle)
{
	ai::NavMesh m;
	ai::Cluster c;
	c.linkCount = 4;
	c.firstDistance = 1;
	m.clusters.push_back(c);
	m.linkDistances = {-9, 0, 1, 2, 3, 4, 5};
	CHECK(m.linkDistance(0, 0, 1) == 0);
	CHECK(m.linkDistance(0, 0, 3) == 2);
	CHECK(m.linkDistance(0, 2, 1) == 3);
	CHECK(m.linkDistance(0, 1, 3) == 4);
	CHECK(m.linkDistance(0, 3, 2) == 5);
	CHECK(m.linkDistance(0, 2, 2) == 0);
}

TEST(NavMesh_ClusterPath)
{
	const ai::NavMesh m = buildGrid();
	const Vec3 from {0.6f, 0, 0.3f}, to {8.6f, 0, 0.3f};
	const int32_t start = m.findPolygon(from), goal = m.findPolygon(to);
	CHECK(m.reverseLink(1) == 0); // cluster 0's link into 1 <-> cluster 1's first
	const auto clusters = ai::findClusterPath(m, start, from, goal, to);
	CHECK(clusters == (std::vector<uint16_t> {0, 1, 2}));
	CHECK(ai::findClusterPath(m, start, from, start, from) == (std::vector<uint16_t> {0}));
	// Closing the middle cluster cuts the way.
	ai::NavMesh closed = m;
	closed.clusters[1].open = false;
	CHECK(ai::findClusterPath(closed, start, from, goal, to).empty());
}

TEST(NavMesh_PathGoesRoundTheBlockedColumn)
{
	const ai::NavMesh m = buildGrid();
	const Vec3 from {0.6f, 0, 0.3f}, to {8.6f, 0, 0.3f};
	const int32_t start = m.findPolygon(from), goal = m.findPolygon(to);
	const auto steps = ai::findPolygonPath(m, start, from, goal, to);
	CHECK(steps.size() > 2);
	if (steps.empty()) {
		return;
	}
	CHECK(steps.front().polygon == uint32_t(start) && steps.front().position == from);
	CHECK(steps.back().polygon == uint32_t(goal));
	bool adjacent = true, walkable = true, crossesTopRow = false;
	for (size_t i = 1; i < steps.size(); ++i) {
		bool found = false;
		for (uint32_t k = 0; k < 3; ++k) {
			if (m.neighbour(steps[i - 1].polygon, k) == int32_t(steps[i].polygon)) {
				found = true;
				const Vec3 mid = m.edgeMidpoint(steps[i - 1].polygon, k);
				adjacent = adjacent && mid == steps[i].position;
			}
		}
		adjacent = adjacent && found;
		walkable = walkable && m.polygons[steps[i].polygon].costClass != 0;
		const Vec3& p = steps[i].position;
		crossesTopRow = crossesTopRow || (p.x > 4 && p.x < 5 && p.z > kDepth - 1);
	}
	CHECK(adjacent);
	CHECK(walkable);
	CHECK(crossesTopRow);

	const auto points = ai::findPath(m, from, to);
	CHECK(points.size() == steps.size() + 1);
	CHECK(!points.empty() && points.front() == from && points.back() == to);
	// The detour climbs diagonally to the gap at (4.5, 4.5) and back down:
	// about 14 m against 8 m straight.
	float length = 0;
	for (size_t i = 1; i < points.size(); ++i) {
		length += ai::distance(points[i - 1], points[i]);
	}
	CHECK(length > 12 && length < 18);

	// Without the gap there is no way.
	ai::NavMesh walled = m;
	for (uint32_t i = 0; i < walled.polygons.size(); ++i) {
		if ((i / 2) % kWidth == 4) {
			walled.polygons[i].costClass = 0;
		}
	}
	CHECK(ai::findPath(walled, from, to).empty());
	// Off the mesh.
	CHECK(ai::findPath(m, {-5, 0, 0}, to).empty());
}

TEST(NavMesh_CostProfileAndSlopes)
{
	const ai::NavMesh m = buildGrid();
	const ai::CostProfile profile;
	CHECK(profile.cost(0) < 0 && profile.cost(1) == 1 && profile.cost(5) == 1000 && profile.cost(7) == 1000);
	// Making class 1 costly doesn't change reachability, only cost.
	ai::PathOptions options;
	options.profile.costs[1] = 3;
	CHECK(!ai::findPath(m, {0.6f, 0, 0.3f}, {8.6f, 0, 0.3f}, options).empty());
	options.vehicle = true;
	CHECK(!ai::findPath(m, {0.6f, 0, 0.3f}, {8.6f, 0, 0.3f}, options).empty());
	// The expansion budget ends a search.
	ai::PathOptions tight;
	tight.maxExpansions = 3;
	tight.useClusters = false;
	CHECK(ai::findPath(m, {0.6f, 0, 0.3f}, {8.6f, 0, 0.3f}, tight).empty());
}

// The real thing: minsk_coop's infantry mesh.
TEST(NavMesh_LoadsALevelAndFindsAPath)
{
	const std::filesystem::path* dir = gameDir();
	if (!dir) {
		return;
	}
	auto zip = io::ZipArchive::open((*dir / "mods/bf2142/Levels/minsk_coop/server.zip").string());
	CHECK(zip != nullptr);
	if (!zip) {
		return;
	}
	std::vector<uint8_t> qtr, clb;
	CHECK(zip->read("AIPathFinding/Infantry.qtr", qtr));
	CHECK(zip->read("AIPathFinding/Infantry.clb", clb));
	std::string error;
	const auto mesh = ai::parseNavMesh(qtr, clb, &error);
	CHECK(mesh.has_value());
	if (!mesh) {
		std::fprintf(stderr, "%s\n", error.c_str());
		return;
	}
	const ai::NavMesh& m = *mesh;
	CHECK(m.treeCount == 1);
	CHECK(m.polygons.size() > 10000 && m.vertices.size() > 10000 && m.nodes.size() > 1000);
	CHECK(m.clusters.size() > 100 && m.links.size() > 100);
	CHECK(m.minX < m.maxX && m.minZ < m.maxZ && m.rootSize >= m.maxX - m.minX);
	CHECK(m.clusterPolygons.size() == m.polygons.size());
	// Every cluster's polygons say they belong to it; links come in pairs.
	bool clustersAgree = true;
	for (uint32_t c = 0; c < m.clusters.size(); ++c) {
		for (uint32_t i = 0; i < m.clusters[c].polygonCount; ++i) {
			clustersAgree = clustersAgree && m.polygons[m.clusterPolygons[m.clusters[c].firstPolygon + i]].cluster == c;
		}
	}
	CHECK(clustersAgree);
	size_t paired = 0;
	for (uint32_t l = 0; l < m.links.size(); ++l) {
		paired += m.reverseLink(l) != 0xffff;
	}
	CHECK(paired * 100 > m.links.size() * 95);
	// Neighbours are mutual.
	bool mutual = true;
	for (uint32_t p = 0; p < m.polygons.size() && mutual; ++p) {
		for (uint32_t k = 0; k < m.polygons[p].vertexCount; ++k) {
			const int32_t q = m.neighbour(p, k);
			if (q < 0) {
				continue;
			}
			bool back = false;
			for (uint32_t j = 0; j < m.polygons[q].vertexCount; ++j) {
				back = back || m.neighbour(q, j) == int32_t(p);
			}
			mutual = mutual && back;
		}
	}
	CHECK(mutual);

	// Two walkable points far apart in one connected area: the last polygon
	// a breadth-first walk from polygon 0 reaches.
	const ai::CostProfile profile;
	uint32_t first = 0;
	while (first < m.polygons.size() && !(m.polygons[first].normal.y > 0.5f)) {
		++first;
	}
	std::vector<bool> seen(m.polygons.size(), false);
	std::deque<uint32_t> queue {first};
	seen[first] = true;
	uint32_t last = first;
	while (!queue.empty()) {
		const uint32_t p = queue.front();
		queue.pop_front();
		last = p;
		for (uint32_t k = 0; k < m.polygons[p].vertexCount; ++k) {
			const int32_t q = m.neighbour(p, k);
			if (q >= 0 && !seen[q] && profile.cost(m.polygons[q].costClass) >= 0 && m.polygons[q].normal.y > 0.5f) {
				seen[q] = true;
				queue.push_back(static_cast<uint32_t>(q));
			}
		}
	}
	const Vec3 from = m.centroid(first), to = m.centroid(last);
	const int32_t start = m.findPolygon(from, 5.0f, &profile);
	const int32_t goal = m.findPolygon(to, 5.0f, &profile);
	CHECK(start >= 0 && goal >= 0);
	CHECK(ai::distance(from, to) > 50);
	if (start < 0 || goal < 0) {
		return;
	}
	CHECK(!ai::findClusterPath(m, start, from, goal, to).empty());

	const auto steps = ai::findPolygonPath(m, start, from, goal, to);
	CHECK(!steps.empty());
	bool continuous = true;
	for (size_t i = 1; i < steps.size(); ++i) {
		bool found = false;
		for (uint32_t k = 0; k < m.polygons[steps[i - 1].polygon].vertexCount; ++k) {
			found = found || m.neighbour(steps[i - 1].polygon, k) == int32_t(steps[i].polygon);
		}
		continuous = continuous && found;
	}
	CHECK(continuous);

	const auto points = ai::findPath(m, from, to);
	CHECK(points.size() >= 2);
	if (points.size() < 2) {
		return;
	}
	CHECK(points.front() == from && points.back() == to);
	float length = 0, longest = 0;
	for (size_t i = 1; i < points.size(); ++i) {
		const float d = ai::distance(points[i - 1], points[i]);
		length += d;
		longest = std::max(longest, d);
	}
	// No jumps: consecutive waypoints lie on one polygon, and the route is
	// at least the straight line and not absurdly longer.
	CHECK(longest < 100);
	CHECK(length >= ai::distance(from, to) - 0.01f);
	CHECK(length < ai::distance(from, to) * 10);
	std::printf("NavMesh: minsk_coop infantry %zu polygons, %zu clusters; path of %zu points, %.0f m (straight %.0f m)\n",
		m.polygons.size(), m.clusters.size(), points.size(), length, ai::distance(from, to));
}

namespace {

// Each polygon's unit normal from its vertices, facing up.
void computeNormals(ai::NavMesh& m)
{
	for (uint32_t p = 0; p < m.polygons.size(); ++p) {
		const Vec3 a = m.vertex(p, 0), b = m.vertex(p, 1), c = m.vertex(p, 2);
		const Vec3 u = b - a, v = c - a;
		Vec3 n {u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
		const float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
		n = n * ((n.y < 0 ? -1.0f : 1.0f) / l);
		m.polygons[p].normal = n;
	}
}

// A corridor of `columns` x 2 squares of `size` m, two triangles a square,
// clusters of four columns linked along the upper row; the lower row of
// column `wall` is blocked (cost class 0).
ai::NavMesh buildCorridor(int columns, float size, int wall)
{
	ai::NavMesh m;
	const int rows = 2;
	m.minX = 0;
	m.minZ = 0;
	m.maxX = columns * size;
	m.maxZ = rows * size;
	m.rootSize = m.maxX;
	for (int z = 0; z <= rows; ++z) {
		for (int x = 0; x <= columns; ++x) {
			m.vertices.push_back({x * size, 0, z * size});
		}
	}
	auto vid = [columns](int x, int z) { return static_cast<uint32_t>(z * (columns + 1) + x); };
	std::vector<std::array<uint32_t, 3>> triangles;
	for (int z = 0; z < rows; ++z) {
		for (int x = 0; x < columns; ++x) {
			triangles.push_back({vid(x, z), vid(x, z + 1), vid(x + 1, z + 1)});
			triangles.push_back({vid(x, z), vid(x + 1, z + 1), vid(x + 1, z)});
			for (int t = 0; t < 2; ++t) {
				ai::Polygon p;
				p.vertexCount = 3;
				p.costClass = x == wall && z == 0 ? 0 : 1;
				p.cluster = static_cast<uint16_t>(x / 4);
				p.firstEdge = static_cast<uint32_t>(m.edgeVertices.size() + t * 3);
				p.normal = {0, 1, 0};
				m.polygons.push_back(p);
			}
			for (int t = 0; t < 2; ++t) {
				for (uint32_t v : triangles[triangles.size() - 2 + t]) {
					m.edgeVertices.push_back(v);
				}
			}
		}
	}
	std::map<std::pair<uint32_t, uint32_t>, std::vector<uint32_t>> edges;
	for (uint32_t p = 0; p < triangles.size(); ++p) {
		for (int k = 0; k < 3; ++k) {
			const uint32_t a = triangles[p][k], b = triangles[p][(k + 1) % 3];
			edges[{std::min(a, b), std::max(a, b)}].push_back(p);
		}
	}
	for (uint32_t p = 0; p < triangles.size(); ++p) {
		for (int k = 0; k < 3; ++k) {
			const uint32_t a = triangles[p][k], b = triangles[p][(k + 1) % 3];
			int32_t other = -1;
			for (uint32_t q : edges[{std::min(a, b), std::max(a, b)}]) {
				other = q != p ? static_cast<int32_t>(q) : other;
			}
			m.edgeNeighbours.push_back(other);
		}
	}
	// A root with four leaf quarters, each listing every polygon (as buildGrid).
	m.nodes.resize(5);
	m.nodes[0].children = {1, 2, 3, 4};
	for (int i = 1; i <= 4; ++i) {
		m.nodes[i].leaf = true;
		m.nodes[i].polygonCount = static_cast<uint8_t>(std::min<size_t>(m.polygons.size(), 255));
		m.nodes[i].firstPolygon = static_cast<uint32_t>(m.leafPolygons.size());
		for (uint32_t p = 0; p < m.polygons.size(); ++p) {
			m.leafPolygons.push_back(p);
		}
	}
	const int clusters = (columns + 3) / 4;
	for (int c = 0; c < clusters; ++c) {
		ai::Cluster cl;
		cl.firstPolygon = static_cast<uint32_t>(m.clusterPolygons.size());
		for (uint32_t p = 0; p < m.polygons.size(); ++p) {
			if (m.polygons[p].cluster == c) {
				m.clusterPolygons.push_back(p);
			}
		}
		cl.polygonCount = static_cast<uint32_t>(m.clusterPolygons.size()) - cl.firstPolygon;
		cl.firstLink = static_cast<uint16_t>(m.links.size());
		cl.firstDistance = static_cast<uint16_t>(m.linkDistances.size());
		// The upper row's lower-right triangle left of a border.
		auto border = [&](int x) { return static_cast<uint32_t>((columns + x) * 2 + 1); };
		if (c > 0) {
			m.links.push_back({{c * 4 * size, 0, 1.5f * size}, uint16_t(c - 1), border(c * 4 - 1)});
		}
		if (c < clusters - 1) {
			m.links.push_back({{(c * 4 + 4) * size, 0, 1.5f * size}, uint16_t(c + 1), border(c * 4 + 3)});
		}
		cl.linkCount = static_cast<uint16_t>(m.links.size()) - cl.firstLink;
		if (cl.linkCount == 2) {
			m.linkDistances.push_back(4 * size);
		}
		m.clusters.push_back(cl);
	}
	return m;
}

} // namespace

// The climb test of the mesh ray (0x1003ce50 with the map's maxSlope and
// step, 0x1003c220): a 45 degree ramp up from x = 5 on the grid's top row.
TEST(PathFollower_MeshRayRefusesASteepClimb)
{
	ai::NavMesh m = buildGrid();
	for (Vec3& v : m.vertices) {
		v.y = std::max(0.0f, v.x - 5);
	}
	for (ai::Polygon& p : m.polygons) {
		p.costClass = 1;
	}
	computeNormals(m);
	const Vec3 low {1.5f, 0, 4.5f}, high {8.5f, 3.5f, 4.5f};
	const ai::ClimbLimits steep {35 * 3.14159265f / 180, 0.35f}, gentle {50 * 3.14159265f / 180, 0.35f};
	// The slope's normal faces the way up across the ground, 45 degrees from up.
	const int32_t ramp = m.findPolygon({6.5f, 1.5f, 4.5f});
	CHECK(ramp >= 0);
	if (ramp < 0) {
		return;
	}
	CHECK(ai::steepAgainst(m.polygons[ramp].normal, high - low, steep.maxSlope));
	CHECK(!ai::steepAgainst(m.polygons[ramp].normal, low - high, steep.maxSlope)); // downhill
	CHECK(!ai::steepAgainst(m.polygons[ramp].normal, high - low, gentle.maxSlope));
	CHECK(!ai::steepAgainst({0, 1, 0}, high - low, 0.1f));
	CHECK(ai::steepAgainst({0.6f, -0.8f, 0}, {-1, 0, 0}, 1.5f)); // facing down: always
	// Without the test, or with a gentler limit, the ray climbs; with 35
	// degrees it stops where the climb begins, unless the step allows 3 m.
	CHECK(ai::meshRaycast(m, low, high, nullptr, -1));
	CHECK(ai::meshRaycast(m, low, high, nullptr, -1, nullptr, -1, 0, 0, &gentle));
	float reached = 0;
	ai::RayHit hit;
	CHECK(!ai::meshRaycast(m, low, high, nullptr, -1, &reached, -1, 0, 0, &steep, &hit));
	CHECK(hit.started && hit.point.x <= 5.5f && reached < 4.1f);
	const ai::ClimbLimits tall {steep.maxSlope, 4};
	CHECK(ai::meshRaycast(m, low, high, nullptr, -1, nullptr, -1, 0, 0, &tall));
	CHECK(ai::meshRaycast(m, high, low, nullptr, -1, nullptr, -1, 0, 0, &steep)); // down is fine
}

// Blocked for 2 s (0x10095720 -> 0x10095470): with a cluster path the
// search goes on from the bot's cluster (no new trip from the start, no
// failure); without one (a trip of 100 m or less) the move fails.
TEST(PathFollower_BlockedGoesOnFromItsCluster)
{
	const ai::NavMesh m = buildCorridor(40, 4, 20); // 160 x 8 m, ten clusters, the wall at x 80-84
	const Vec3 from {2, 0, 2}, to {158, 0, 2};
	ai::PathFollower follower;
	ai::FollowerSettings settings;
	follower.request(m, from, to, settings, 0);
	CHECK(follower.planned() && !follower.failed());
	// The segments follow the bot: with it at the start, more than ten
	// waypoints and 30 m ahead, the next one waits.
	float now = 0;
	for (int i = 0; i < 3; ++i) {
		now += 0.1f;
		CHECK(follower.steer(m, from, now).ok);
	}
	const size_t planned = follower.waypoints().size();
	CHECK(planned > 10 && follower.waypoints().back().p.x < 60);
	// Beyond the wall, out of sight of the current waypoint, in cluster 6.
	const Vec3 beyond {100, 0, 2};
	ai::PathFollower::Steer steer;
	for (int i = 0; i < 25; ++i) {
		now += 0.1f;
		steer = follower.steer(m, beyond, now);
	}
	CHECK(steer.ok && !follower.failed());
	CHECK(!follower.waypoints().empty());
	bool onward = !follower.waypoints().empty();
	for (const ai::Waypoint& w : follower.waypoints()) {
		onward = onward && w.p.x > 90; // from the bot's cluster on, not back to the start
	}
	CHECK(onward);
	// A short trip has no cluster path: blocked, the move fails.
	ai::PathFollower shortTrip;
	shortTrip.request(m, from, {40, 0, 2}, settings, 0);
	CHECK(shortTrip.planned() && !shortTrip.failed());
	const Vec3 behind {90, 0, 2}; // past the wall: the waypoints can't be seen
	float t = 0;
	for (int i = 0; i < 25; ++i) {
		t += 0.1f;
		shortTrip.steer(m, behind, t);
	}
	CHECK(shortTrip.failed());
}

// The infantry steering's sidestep (0x100ce860, 0x100e63c0) round a
// vehicle standing on the way: the point on its rim, on the side away from
// it, and the way bent round it; a wreck keeps more room; a goal inside
// the room ends the move once the bot is close.
TEST(PathFollower_SidestepRoundAVehicle)
{
	ai::NavMesh m = buildCorridor(40, 4, -1);
	for (Vec3& v : m.vertices) {
		v.z *= 6; // 48 m across
	}
	m.maxZ *= 6;
	ai::SidestepSense sense;
	sense.position = {20, 0, 24};
	sense.steer = sense.ahead = {60, 0, 24};
	sense.goal = {150, 0, 24};
	sense.mesh = &m;
	ai::SidestepObstacle car;
	car.id = 7;
	car.position = {22.5f, 0, 24.5f};
	car.radius = 2;
	sense.obstacles.push_back(car);
	ai::SidestepState state;
	Vec3 way;
	CHECK(!ai::sidestepSteer(state, sense, way));
	CHECK(state.obstacle == 7 && std::fabs(state.radius - 2) < 1e-5f && std::fabs(state.distance - 2) < 1e-4f);
	// It stands a little to the left (+z) of the way: passed on the right.
	CHECK(std::fabs(state.point.x - 22.5f) < 1e-4f && std::fabs(state.point.z - 22.5f) < 1e-4f);
	CHECK(way.x > 0.5f && way.z < -0.3f && std::fabs(way.x * way.x + way.z * way.z - 1) < 1e-4f);
	// Nothing in the way: straight for the point.
	ai::SidestepState clear;
	ai::SidestepSense open = sense;
	open.obstacles.clear();
	CHECK(!ai::sidestepSteer(clear, open, way) && clear.obstacle == -1 && std::fabs(way.x - 1) < 1e-5f);
	// A wreck (below 0.3 of its health): max(r, min(10, r + 4)) x 1.3 of room.
	ai::SidestepSense wreck = sense;
	wreck.obstacles[0].health = 0.2f;
	ai::SidestepState wrecked;
	ai::sidestepSteer(wrecked, wreck, way);
	CHECK(wrecked.obstacle == 7 && wrecked.still && std::fabs(wrecked.radius - 7.8f) < 1e-4f);
	// The goal beside the car: the move ends once the bot is close.
	ai::SidestepSense near = sense;
	near.goal = near.steer = {23, 0, 24.5f};
	ai::SidestepState s2;
	CHECK(!ai::sidestepSteer(s2, near, way)); // 3 m from the goal
	near.position = {21, 0, 24};
	CHECK(ai::sidestepSteer(s2, near, way));
}
