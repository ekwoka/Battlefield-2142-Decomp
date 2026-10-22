#include "NavMesh.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>
#include <unordered_map>

namespace dice::hfe::ai {

namespace {

constexpr uint32_t kQuadTreeVersion = 3; // 0x100baa90: "2 < version < 4"
constexpr uint32_t kClusterVersion = 5;  // 0x100b1380: "4 < version < 6"
constexpr uint16_t kNoLink = 0xffff;

// Sequential little-endian reads of the raw arrays.
class Reader {
public:
	Reader(const std::vector<uint8_t>& data, const char* name) : m_data(data), m_name(name) {}

	bool u32(uint32_t& out) { return raw(&out, 4); }
	bool f32(float& out) { return raw(&out, 4); }
	bool raw(void* out, size_t size)
	{
		if (m_data.size() - m_pos < size) {
			return false;
		}
		std::memcpy(out, m_data.data() + m_pos, size);
		m_pos += size;
		return true;
	}
	// A u32 count then `count` elements of `size` bytes; returns the start.
	const uint8_t* array(uint32_t& count, size_t size)
	{
		if (!u32(count) || (m_data.size() - m_pos) / size < count) {
			return nullptr;
		}
		const uint8_t* start = m_data.data() + m_pos;
		m_pos += count * size;
		return start;
	}
	const char* name() const { return m_name; }

private:
	const std::vector<uint8_t>& m_data;
	const char* m_name;
	size_t m_pos = 0;
};

uint32_t get32(const uint8_t* p)
{
	uint32_t v;
	std::memcpy(&v, p, 4);
	return v;
}

uint16_t get16(const uint8_t* p)
{
	uint16_t v;
	std::memcpy(&v, p, 2);
	return v;
}

float getF(const uint8_t* p)
{
	float v;
	std::memcpy(&v, p, 4);
	return v;
}

Vec3 getVec3(const uint8_t* p)
{
	return {getF(p), getF(p + 4), getF(p + 8)};
}

bool fail(std::string* error, const std::string& message)
{
	if (error) {
		*error = message;
	}
	return false;
}

bool parseQuadTree(Reader& in, NavMesh& mesh, std::string* error)
{
	if (!in.u32(mesh.treeCount) || mesh.treeCount == 0) {
		return fail(error, "qtr: no tree");
	}
	if (!in.u32(mesh.version) || mesh.version != kQuadTreeVersion) {
		return fail(error, "qtr: unsupported version " + std::to_string(mesh.version));
	}
	if (!in.f32(mesh.minX) || !in.f32(mesh.minZ) || !in.f32(mesh.maxX) || !in.f32(mesh.maxZ) || !in.f32(mesh.rootSize)) {
		return fail(error, "qtr: truncated header");
	}

	uint32_t count = 0;
	const uint8_t* p = in.array(count, 0x18);
	if (!p) {
		return fail(error, "qtr: truncated quad tree nodes");
	}
	mesh.nodes.resize(count);
	for (uint32_t i = 0; i < count; ++i, p += 0x18) {
		QuadNode& n = mesh.nodes[i];
		for (int c = 0; c < 4; ++c) {
			n.children[c] = get32(p + c * 4);
		}
		n.leaf = p[0x10] != 0;
		n.polygonCount = p[0x11];
		n.firstPolygon = get32(p + 0x14);
	}

	if (!(p = in.array(count, 0x14))) {
		return fail(error, "qtr: truncated polygons");
	}
	mesh.polygons.resize(count);
	for (uint32_t i = 0; i < count; ++i, p += 0x14) {
		Polygon& poly = mesh.polygons[i];
		poly.vertexCount = p[0];
		poly.costClass = p[1];
		poly.cluster = get16(p + 2);
		poly.firstEdge = get32(p + 4);
		poly.normal = getVec3(p + 8);
	}

	if (!(p = in.array(count, 4))) {
		return fail(error, "qtr: truncated leaf polygon lists");
	}
	mesh.leafPolygons.resize(count);
	for (uint32_t i = 0; i < count; ++i) {
		mesh.leafPolygons[i] = get32(p + i * 4);
	}

	if (!(p = in.array(count, 0xc))) {
		return fail(error, "qtr: truncated vertices");
	}
	mesh.vertices.resize(count);
	for (uint32_t i = 0; i < count; ++i) {
		mesh.vertices[i] = getVec3(p + i * 0xc);
	}

	if (!(p = in.array(count, 4))) {
		return fail(error, "qtr: truncated edge vertices");
	}
	mesh.edgeVertices.resize(count);
	for (uint32_t i = 0; i < count; ++i) {
		mesh.edgeVertices[i] = get32(p + i * 4);
	}

	if (!(p = in.array(count, 4))) {
		return fail(error, "qtr: truncated edge neighbours");
	}
	mesh.edgeNeighbours.resize(count);
	for (uint32_t i = 0; i < count; ++i) {
		mesh.edgeNeighbours[i] = static_cast<int32_t>(get32(p + i * 4));
	}
	return true;
}

bool parseClusters(Reader& in, NavMesh& mesh, std::string* error)
{
	if (!in.u32(mesh.clusterVersion) || mesh.clusterVersion != kClusterVersion) {
		return fail(error, "clb: unsupported version " + std::to_string(mesh.clusterVersion));
	}
	uint32_t count = 0;
	const uint8_t* p = in.array(count, 0x10);
	if (!p) {
		return fail(error, "clb: truncated clusters");
	}
	mesh.clusters.resize(count);
	for (uint32_t i = 0; i < count; ++i, p += 0x10) {
		Cluster& c = mesh.clusters[i];
		c.firstPolygon = get32(p);
		c.polygonCount = get32(p + 4);
		c.firstLink = get16(p + 8);
		c.linkCount = get16(p + 0xa);
		c.firstDistance = get16(p + 0xc);
		c.open = p[0xe] != 0;
	}

	if (!(p = in.array(count, 4))) {
		return fail(error, "clb: truncated cluster polygons");
	}
	mesh.clusterPolygons.resize(count);
	for (uint32_t i = 0; i < count; ++i) {
		mesh.clusterPolygons[i] = get32(p + i * 4);
	}

	// The link and distance arrays are only read when their count isn't 0
	// (the same thing for a well-formed file).
	if (!(p = in.array(count, 0x14))) {
		return fail(error, "clb: truncated links");
	}
	mesh.links.resize(count);
	for (uint32_t i = 0; i < count; ++i, p += 0x14) {
		ClusterLink& l = mesh.links[i];
		l.position = getVec3(p);
		l.targetCluster = get16(p + 0xc);
		l.polygon = get32(p + 0x10);
	}

	if (!(p = in.array(count, 4))) {
		return fail(error, "clb: truncated link distances");
	}
	mesh.linkDistances.resize(count);
	for (uint32_t i = 0; i < count; ++i) {
		mesh.linkDistances[i] = getF(p + i * 4);
	}
	return true;
}

size_t triangle(size_t n)
{
	return n * (n - 1) / 2;
}

// The original indexes the arrays without checks; reject files that would
// make it read out of bounds.
bool validate(const NavMesh& mesh, std::string* error)
{
	const size_t polygons = mesh.polygons.size();
	for (const QuadNode& n : mesh.nodes) {
		if (n.leaf) {
			if (size_t(n.firstPolygon) + n.polygonCount > mesh.leafPolygons.size()) {
				return fail(error, "qtr: leaf polygon list out of range");
			}
		} else {
			for (uint32_t c : n.children) {
				if (c >= mesh.nodes.size()) {
					return fail(error, "qtr: quad tree child out of range");
				}
			}
		}
	}
	for (uint32_t p : mesh.leafPolygons) {
		if (p >= polygons) {
			return fail(error, "qtr: leaf polygon out of range");
		}
	}
	for (const Polygon& p : mesh.polygons) {
		if (p.vertexCount < 3 || size_t(p.firstEdge) + p.vertexCount > mesh.edgeVertices.size()) {
			return fail(error, "qtr: polygon edges out of range");
		}
		if (!mesh.clusters.empty() && p.cluster >= mesh.clusters.size()) {
			return fail(error, "qtr: polygon cluster out of range");
		}
	}
	if (mesh.edgeNeighbours.size() != mesh.edgeVertices.size()) {
		return fail(error, "qtr: edge arrays differ in length");
	}
	for (uint32_t v : mesh.edgeVertices) {
		if (v >= mesh.vertices.size()) {
			return fail(error, "qtr: edge vertex out of range");
		}
	}
	for (int32_t n : mesh.edgeNeighbours) {
		if (n < -1 || n >= int64_t(polygons)) {
			return fail(error, "qtr: edge neighbour out of range");
		}
	}
	for (const Cluster& c : mesh.clusters) {
		if (size_t(c.firstPolygon) + c.polygonCount > mesh.clusterPolygons.size() ||
			size_t(c.firstLink) + c.linkCount > mesh.links.size() ||
			(c.linkCount > 1 && size_t(c.firstDistance) + triangle(c.linkCount) > mesh.linkDistances.size())) {
			return fail(error, "clb: cluster ranges out of range");
		}
	}
	for (uint32_t p : mesh.clusterPolygons) {
		if (p >= polygons) {
			return fail(error, "clb: cluster polygon out of range");
		}
	}
	for (const ClusterLink& l : mesh.links) {
		if (l.polygon >= polygons) {
			return fail(error, "clb: link polygon out of range");
		}
	}
	return true;
}

} // namespace

float distance(const Vec3& a, const Vec3& b)
{
	const Vec3 d = a - b;
	return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

std::optional<NavMesh> parseNavMesh(const std::vector<uint8_t>& qtr, const std::vector<uint8_t>& clb, std::string* error)
{
	NavMesh mesh;
	Reader q(qtr, "qtr");
	Reader c(clb, "clb");
	if (!parseQuadTree(q, mesh, error) || !parseClusters(c, mesh, error) || !validate(mesh, error)) {
		return std::nullopt;
	}
	return mesh;
}

const Vec3& NavMesh::vertex(uint32_t polygon, uint32_t k) const
{
	const Polygon& p = polygons[polygon];
	return vertices[edgeVertices[p.firstEdge + k % p.vertexCount]];
}

int32_t NavMesh::neighbour(uint32_t polygon, uint32_t k) const
{
	const Polygon& p = polygons[polygon];
	return k < p.vertexCount ? edgeNeighbours[p.firstEdge + k] : -1;
}

Vec3 NavMesh::centroid(uint32_t polygon) const
{
	const Polygon& p = polygons[polygon];
	Vec3 sum;
	for (uint32_t k = 0; k < p.vertexCount; ++k) {
		sum += vertex(polygon, k);
	}
	return sum * (1.0f / p.vertexCount);
}

Vec3 NavMesh::edgeMidpoint(uint32_t polygon, uint32_t k) const
{
	return (vertex(polygon, k) + vertex(polygon, k + 1)) * 0.5f;
}

bool NavMesh::containsXZ(uint32_t polygon, const Vec3& point) const
{
	// The polygons wind clockwise seen from above (+y), so the inside is
	// to the right of every edge.
	const Polygon& p = polygons[polygon];
	for (uint32_t k = 0; k < p.vertexCount; ++k) {
		const Vec3& a = vertex(polygon, k);
		const Vec3& b = vertex(polygon, k + 1);
		const float side = (point.x - a.x) * (b.z - a.z) - (point.z - a.z) * (b.x - a.x);
		if (side < -0.001f) {
			return false;
		}
	}
	return true;
}

float NavMesh::planeHeight(uint32_t polygon, const Vec3& point) const
{
	// The plane n.v + d = 0 through vertex 0, met by a vertical line
	// (0x10100660 with the point and the point 1 m lower).
	const Vec3& n = polygons[polygon].normal;
	const Vec3& v = vertex(polygon, 0);
	const float d = -(n.x * v.x + n.y * v.y + n.z * v.z);
	return -(d + n.x * point.x + n.z * point.z) / n.y;
}

bool NavMesh::standsOn(uint32_t polygon, const Vec3& point, float heightTolerance, float* heightOffset) const
{
	if (polygons[polygon].normal.y == 0 || !containsXZ(polygon, point)) {
		return false;
	}
	const float surface = planeHeight(polygon, point);
	// The point may hover up to the tolerance above the surface, or be up
	// to 2 m under it (_DAT_10128e34).
	const float limit = surface < point.y ? heightTolerance : 2.0f;
	const float dy = surface - point.y;
	if (dy * dy > limit * limit) {
		return false;
	}
	if (heightOffset) {
		*heightOffset = dy;
	}
	return true;
}

int32_t NavMesh::findLeaf(float x, float z) const
{
	if (nodes.empty() || x < minX || x > maxX || z < minZ || z > maxZ) {
		return -1;
	}
	uint32_t node = 0;
	float originX = minX, originZ = minZ, size = rootSize;
	while (!nodes[node].leaf) {
		size *= 0.5f;
		const bool highX = originX + size <= x;
		const bool highZ = originZ + size <= z;
		const uint32_t child = nodes[node].children[(highX ? 2 : 0) + (highZ ? 1 : 0)];
		if (child == 0) {
			return -1;
		}
		originX += highX ? size : 0;
		originZ += highZ ? size : 0;
		node = child;
	}
	return static_cast<int32_t>(node);
}

int32_t NavMesh::findPolygon(const Vec3& point, float heightTolerance, const CostProfile* profile, float* heightOffset) const
{
	const int32_t leaf = findLeaf(point.x, point.z);
	if (leaf < 0) {
		return -1;
	}
	const QuadNode& node = nodes[leaf];
	int32_t best = -1;
	float bestDistance = 1e8f; // 0x4cbebc20
	for (uint32_t i = 0; i < node.polygonCount; ++i) {
		const uint32_t polygon = leafPolygons[node.firstPolygon + i];
		const Polygon& p = polygons[polygon];
		// Only floors (normal pointing up) the bot may walk on.
		if (!(p.normal.y > 0) || (profile && profile->cost(p.costClass) < 0)) {
			continue;
		}
		float offset = 0;
		if (standsOn(polygon, point, heightTolerance, &offset) && offset * offset < bestDistance) {
			bestDistance = offset * offset;
			best = static_cast<int32_t>(polygon);
			if (heightOffset) {
				*heightOffset = offset;
			}
		}
	}
	return best;
}

float NavMesh::linkDistance(uint16_t cluster, uint16_t a, uint16_t b) const
{
	if (a == b) {
		return 0; // the original would read the previous pair's entry
	}
	const Cluster& c = clusters[cluster];
	const uint32_t low = std::min(a, b), high = std::max(a, b);
	const uint32_t n = c.linkCount;
	const uint32_t rest = n - low;
	const uint32_t index = triangle(n) - triangle(rest) + (high - low - 1);
	return linkDistances[c.firstDistance + index];
}

uint16_t NavMesh::reverseLink(uint32_t link) const
{
	const ClusterLink& l = links[link];
	if (l.targetCluster >= clusters.size()) {
		return kNoLink;
	}
	const Cluster& target = clusters[l.targetCluster];
	for (uint16_t i = 0; i < target.linkCount; ++i) {
		const ClusterLink& back = links[target.firstLink + i];
		const Vec3 d = back.position - l.position;
		if (back.polygon == l.polygon && d.x * d.x + d.y * d.y + d.z * d.z < 0.01f) {
			return i;
		}
	}
	return kNoLink;
}

namespace {

// The A* bookkeeping shared by both searches (the driver 0x10044880 with
// its open list 0x100401d0 and the spaces' hash of visited nodes, 0x407
// buckets keyed by polygon / cluster). Nodes live in a vector; the open
// list is a heap with stale entries skipped.
struct OpenEntry {
	float f;
	uint32_t node;
	uint32_t serial;
	bool operator>(const OpenEntry& o) const { return f > o.f; }
};

using OpenList = std::priority_queue<OpenEntry, std::vector<OpenEntry>, std::greater<OpenEntry>>;

constexpr uint32_t kNone = std::numeric_limits<uint32_t>::max();

struct PolygonNode {
	uint32_t polygon = 0; // +0x10
	uint32_t parent = kNone; // +0x08
	float g = 0;           // +0x14
	float f = 0;           // +0x18
	uint32_t depth = 0;    // +0x1c
	Vec3 position;         // +0x20: where the path enters the polygon
	bool open = true;      // hash entry byte 0 (byte 1 = closed)
	uint32_t serial = 0;
};

// The cost of walking from `a` at `from` into `b` at `to` (0x100bc300),
// -1 when `a`'s cost class is impassable.
float stepCost(const NavMesh& mesh, const PathOptions& options, uint32_t a, const Vec3& from, uint32_t b, const Vec3& to)
{
	float multiplier = options.profile.cost(mesh.polygons[a].costClass);
	if (multiplier < 0) {
		return -1;
	}
	const float length = distance(from, to);
	if (!options.vehicle) {
		// On foot: uphill into a slope costs up to twice the distance, by
		// the slope's angle (2/pi * acos(normal.y)).
		if (length > 0.35f) {
			Vec3 dir {to.x - from.x, 0, to.z - from.z};
			const float len = std::sqrt(dir.x * dir.x + dir.z * dir.z);
			if (len >= 1.1920929e-07f) {
				dir = dir * (1.0f / len);
			}
			const Vec3& n = mesh.polygons[b].normal;
			if (dir.x * n.x + dir.y * n.y + dir.z * n.z < 0) {
				if (n.y < 0) {
					return 2.0f * length * multiplier;
				}
				const float slope = std::acos(std::clamp(n.y, -1.0f, 1.0f)) * 0.63661975f;
				return (slope + 1.0f) * length * multiplier;
			}
		}
		return length * multiplier;
	}
	// Vehicles: steep ground costs up to 16 times (normal.y from 1 down to
	// 0.8), and every bend between the two polygons adds 1 to 4.
	float bend = 0;
	if (length >= 0.25f) {
		const Vec3& na = mesh.polygons[a].normal;
		const Vec3& nb = mesh.polygons[b].normal;
		float steep = std::max(na.y * 5.0f - 4.0f, 0.0f);
		steep = 4.0f - steep * steep * 3.0f;
		float flat = std::max((na.x * nb.x + na.y * nb.y + na.z * nb.z) * 5.0f - 4.0f, 0.0f);
		flat = 2.0f - flat * flat;
		bend = flat * flat;
		multiplier *= steep * steep;
	}
	return length * multiplier + bend;
}

// The cost so far of a node entered from `parent` (0x100bc6f0): a
// non-positive step is returned as it is, without the parent's cost.
float pathCost(const NavMesh& mesh, const PathOptions& options, const PolygonNode& parent, uint32_t polygon, const Vec3& at)
{
	const float step = stepCost(mesh, options, parent.polygon, parent.position, polygon, at);
	return step > 0 ? step + parent.g : step;
}

} // namespace

std::vector<PathStep> findPolygonPath(const NavMesh& mesh, uint32_t startPolygon, const Vec3& from, uint32_t goalPolygon,
	const Vec3& to, const PathOptions& options, const std::vector<bool>* allowedClusters)
{
	if (startPolygon >= mesh.polygons.size() || goalPolygon >= mesh.polygons.size()) {
		return {};
	}
	std::vector<PolygonNode> nodes;
	std::unordered_map<uint32_t, uint32_t> visited; // polygon -> node
	OpenList open;

	auto build = [&](uint32_t last) {
		std::vector<PathStep> path;
		for (uint32_t n = last; n != kNone; n = nodes[n].parent) {
			path.push_back({nodes[n].polygon, nodes[n].position});
		}
		std::reverse(path.begin(), path.end());
		return path;
	};

	nodes.push_back({startPolygon, kNone, 0, distance(from, to), 0, from});
	// 0x100415f0: start and goal in one polygon is a path at once.
	if (startPolygon == goalPolygon) {
		return build(0);
	}
	visited[startPolygon] = 0;
	open.push({nodes[0].f, 0, 0});

	uint32_t expansions = 0;
	while (!open.empty()) {
		const OpenEntry top = open.top();
		open.pop();
		if (!nodes[top.node].open || nodes[top.node].serial != top.serial) {
			continue;
		}
		const uint32_t current = top.node;
		const Polygon& poly = mesh.polygons[nodes[current].polygon];

		// The successors (0x100bcbb0, one per edge with a neighbour).
		for (uint32_t k = 0; k < poly.vertexCount; ++k) {
			const int32_t next = mesh.neighbour(nodes[current].polygon, k);
			if (next < 0) {
				continue;
			}
			const uint32_t q = static_cast<uint32_t>(next);
			// The driver drops a step straight back to the parent.
			const uint32_t parent = nodes[current].parent;
			if (parent != kNone && nodes[parent].polygon == q) {
				continue;
			}
			const Polygon& qp = mesh.polygons[q];
			if (options.profile.cost(qp.costClass) < 0) {
				continue;
			}
			if (allowedClusters && (qp.cluster >= allowedClusters->size() || !(*allowedClusters)[qp.cluster])) {
				continue;
			}
			const Vec3 at = mesh.edgeMidpoint(nodes[current].polygon, k);

			const auto seen = visited.find(q);
			if (seen != visited.end()) {
				// Open or closed: take the cheaper way in (0x100bc790; a
				// closed node goes back on the open list).
				PolygonNode& e = nodes[seen->second];
				const uint32_t depth = nodes[current].depth + 1;
				if (options.maxDepth != 0 && depth == options.maxDepth) {
					continue;
				}
				if (nodes[current].parent == seen->second || e.parent == kNone) {
					continue;
				}
				const float g = pathCost(mesh, options, nodes[current], q, at);
				const float f = distance(to, at) + g;
				if (0.5f + f < e.f) {
					e.parent = current;
					e.position = at;
					e.g = g;
					e.f = f;
					e.depth = depth;
					e.open = true;
					open.push({e.f, seen->second, ++e.serial});
				}
				continue;
			}

			PolygonNode n;
			n.polygon = q;
			n.parent = current;
			n.depth = nodes[current].depth + 1;
			n.position = at;
			n.g = pathCost(mesh, options, nodes[current], q, at);
			nodes.push_back(n);
			const uint32_t index = static_cast<uint32_t>(nodes.size() - 1);
			if (q == goalPolygon) {
				// The goal is reached when generated, not when expanded.
				nodes[index].f = nodes[index].g + stepCost(mesh, options, nodes[current].polygon, at, q, to);
				return build(index);
			}
			nodes[index].f = distance(to, at) + nodes[index].g; // 0x100bc590
			visited[q] = index;
			open.push({nodes[index].f, index, 0});
		}

		nodes[current].open = false;
		++expansions;
		if ((options.maxExpansions != 0 && expansions == options.maxExpansions) ||
			(options.maxDepth != 0 && nodes[current].depth == options.maxDepth)) {
			return {};
		}
	}
	return {};
}

namespace {

struct ClusterNode {
	uint16_t cluster = 0;       // +0x14
	uint16_t entry = kNoLink;   // +0x16 link index within the cluster
	uint32_t parent = kNone;    // +0x08
	float g = 0;                // +0x0c
	float f = 0;                // +0x10
	uint32_t depth = 0;         // +0x18
	bool open = true;
	uint32_t serial = 0;
};

// The cheapest way from a node over to `target` (0x100bd270): among the
// node's links into the target, the one closest to where the node was
// entered (the start point, or the precomputed in-cluster distance from
// the entry link). Gives the cost so far (or -1) and the entry link in the
// target.
float crossCost(const NavMesh& mesh, const ClusterNode& node, uint16_t target, const Vec3& from, uint16_t& entry)
{
	entry = kNoLink;
	if (target >= mesh.clusters.size() || !mesh.clusters[target].open) {
		return -1;
	}
	const Cluster& c = mesh.clusters[node.cluster];
	float best = 1e9f;
	for (uint16_t j = 0; j < c.linkCount; ++j) {
		const uint32_t link = c.firstLink + j;
		if (mesh.links[link].targetCluster != target) {
			continue;
		}
		// 0x100bd130
		float cost = node.entry == kNoLink ? distance(mesh.links[link].position, from)
										   : mesh.linkDistance(node.cluster, node.entry, j);
		if (cost < 0) {
			continue;
		}
		cost += node.g;
		if (cost < best) {
			best = cost;
			entry = mesh.reverseLink(link);
		}
	}
	return entry == kNoLink ? -1 : best;
}

// The heuristic (0x100bd1f0): straight from the node's entry point (the
// start point for the start node) to the goal.
float clusterEstimate(const NavMesh& mesh, const ClusterNode& node, const Vec3& from, const Vec3& to)
{
	const Vec3& at =
		node.parent == kNone ? from : mesh.links[mesh.clusters[node.cluster].firstLink + node.entry].position;
	return distance(to, at) + node.g;
}

} // namespace

std::vector<uint16_t> findClusterPath(const NavMesh& mesh, uint32_t startPolygon, const Vec3& from, uint32_t goalPolygon,
	const Vec3& to, const PathOptions& options)
{
	std::vector<uint16_t> clusters;
	for (const ClusterStep& step : findClusterRoute(mesh, startPolygon, from, goalPolygon, to, options)) {
		clusters.push_back(step.cluster);
	}
	return clusters;
}

std::vector<ClusterStep> findClusterRoute(const NavMesh& mesh, uint32_t startPolygon, const Vec3& from,
	uint32_t goalPolygon, const Vec3& to, const PathOptions& options)
{
	if (startPolygon >= mesh.polygons.size() || goalPolygon >= mesh.polygons.size() || mesh.clusters.empty()) {
		return {};
	}
	const uint16_t startCluster = mesh.polygons[startPolygon].cluster;
	const uint16_t goalCluster = mesh.polygons[goalPolygon].cluster;
	std::vector<ClusterNode> nodes;
	std::unordered_map<uint16_t, uint32_t> visited;
	OpenList open;

	auto build = [&](uint32_t last) {
		std::vector<ClusterStep> path;
		for (uint32_t n = last; n != kNone; n = nodes[n].parent) {
			path.push_back({nodes[n].cluster, nodes[n].entry});
		}
		std::reverse(path.begin(), path.end());
		return path;
	};

	ClusterNode start;
	start.cluster = startCluster;
	start.f = distance(from, to);
	nodes.push_back(start);
	// 0x10041700: the same cluster is a path at once.
	if (startCluster == goalCluster) {
		return build(0);
	}
	visited[startCluster] = 0;
	open.push({start.f, 0, 0});

	uint32_t expansions = 0;
	std::vector<uint16_t> targets;
	while (!open.empty()) {
		const OpenEntry top = open.top();
		open.pop();
		if (!nodes[top.node].open || nodes[top.node].serial != top.serial) {
			continue;
		}
		const uint32_t current = top.node;

		// The successors (0x100bd630): each neighbouring cluster once.
		const Cluster& c = mesh.clusters[nodes[current].cluster];
		targets.clear();
		for (uint16_t i = 0; i < c.linkCount; ++i) {
			const uint16_t t = mesh.links[c.firstLink + i].targetCluster;
			if (std::find(targets.begin(), targets.end(), t) == targets.end()) {
				targets.push_back(t);
			}
		}
		for (const uint16_t target : targets) {
			const uint32_t parent = nodes[current].parent;
			if (parent != kNone && nodes[parent].cluster == target) {
				continue;
			}
			uint16_t entry = kNoLink;
			const float g = crossCost(mesh, nodes[current], target, from, entry);
			if (g < 0) {
				continue;
			}

			const auto seen = visited.find(target);
			if (seen != visited.end()) {
				// 0x100bd850: re-enter over a cheaper link.
				ClusterNode& e = nodes[seen->second];
				const uint32_t depth = nodes[current].depth + 1;
				if ((options.maxDepth != 0 && depth == options.maxDepth) || e.parent == kNone) {
					continue;
				}
				ClusterNode candidate = e;
				candidate.entry = entry;
				candidate.g = g;
				candidate.parent = current;
				const float f = clusterEstimate(mesh, candidate, from, to);
				if (f < e.f) {
					e.parent = current;
					e.entry = entry;
					e.g = g;
					e.f = f;
					e.depth = depth;
					e.open = true;
					open.push({e.f, seen->second, ++e.serial});
				}
				continue;
			}

			ClusterNode n;
			n.cluster = target;
			n.entry = entry;
			n.parent = current;
			n.g = g;
			n.depth = nodes[current].depth + 1;
			nodes.push_back(n);
			const uint32_t index = static_cast<uint32_t>(nodes.size() - 1);
			if (target == goalCluster) {
				return build(index);
			}
			nodes[index].f = clusterEstimate(mesh, nodes[index], from, to);
			visited[target] = index;
			open.push({nodes[index].f, index, 0});
		}

		nodes[current].open = false;
		++expansions;
		if ((options.maxExpansions != 0 && expansions == options.maxExpansions) ||
			(options.maxDepth != 0 && nodes[current].depth == options.maxDepth)) {
			return {};
		}
	}
	return {};
}

const ClusterLink* entryLink(const NavMesh& mesh, const ClusterStep& step)
{
	if (step.cluster >= mesh.clusters.size() || step.entry == 0xffff) {
		return nullptr;
	}
	const size_t link = mesh.clusters[step.cluster].firstLink + static_cast<size_t>(step.entry);
	return link < mesh.links.size() ? &mesh.links[link] : nullptr;
}

std::vector<Vec3> findPath(const NavMesh& mesh, const Vec3& from, const Vec3& to, const PathOptions& options)
{
	const int32_t start = mesh.findPolygon(from, options.heightTolerance, &options.profile);
	const int32_t goal = mesh.findPolygon(to, options.heightTolerance, &options.profile);
	if (start < 0 || goal < 0) {
		return {};
	}
	std::vector<PathStep> steps;
	if (options.useClusters && !mesh.clusters.empty()) {
		const std::vector<uint16_t> corridor = findClusterPath(mesh, start, from, goal, to, options);
		if (!corridor.empty()) {
			std::vector<bool> allowed(mesh.clusters.size(), false);
			for (uint16_t c : corridor) {
				allowed[c] = true;
			}
			steps = findPolygonPath(mesh, start, from, goal, to, options, &allowed);
		}
	}
	if (steps.empty()) {
		steps = findPolygonPath(mesh, start, from, goal, to, options);
	}
	if (steps.empty()) {
		return {};
	}
	std::vector<Vec3> points;
	points.reserve(steps.size() + 1);
	for (const PathStep& s : steps) {
		points.push_back(s.position);
	}
	points.push_back(to);
	return points;
}

} // namespace dice::hfe::ai
