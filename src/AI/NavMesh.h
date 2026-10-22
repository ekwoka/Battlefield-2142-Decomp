// The bots' navigation mesh (AIDLL.dll, AIPathFinding/NavMesh): per level
// AIPathFinding/Infantry.qtr + .clb and Vehicle.qtr + .clb in server.zip.
//
// The QuadTreeManager (0x100ae4e0, QuadTreeManager.cpp) opens
// <level>/AIPathFinding/<name>.qtr and .clb, reads a u32 tree count from the
// .qtr ("m_treeCount < 2": more than one is reported as "More than one
// island in the navMesh!!!" but still loaded) and per tree builds
//   - a quad tree (0xe0 bytes, constructor 0x100ba850, load 0x100baa90)
//     from the .qtr: walkable polygons (triangles in shipped data), their
//     vertices, per-edge neighbours and a quad tree over the x/z plane, and
//   - a cluster map (0x3c bytes, constructor 0x100b3c40, load 0x100b1380)
//     from the .clb: the polygons grouped into clusters, the links ("portals")
//     between neighbouring clusters and, per cluster, the precomputed walking
//     distances between each pair of its links.
// All values are little-endian; every array is a u32 count then the
// elements, read straight into memory (the file readers 0x100fe050 = 4
// bytes, 0x100fdfe0 = n bytes).
//
// .qtr (one tree; the manager's count comes first):
//   u32 treeCount (1)
//   u32 version (must be 3)
//   f32 minX, minZ                      tree +0x4c
//   f32 maxX, maxZ                      tree +0x54
//   f32 rootSize                        tree +0x5c: edge of the root square
//   QuadNode[]        0x18 bytes        tree +0x64 count, +0x7c array
//   Polygon[]         0x14 bytes        tree +0x68, +0x80
//   u32 leafPolygons[]                  tree +0x6c, +0x84
//   Vec3 vertices[]   0x0c bytes        tree +0x70, +0x88
//   u32 edgeVertices[]                  tree +0x74, +0x8c (per polygon edge)
//   i32 edgeNeighbours[]                tree +0x78, +0x90 (-1 = border)
// .clb:
//   u32 version (must be 5)
//   Cluster[]         0x10 bytes        map +0x0c count, +0x08 array
//   u32 clusterPolygons[]               map +0x04, +0x00 (polygons by cluster)
//   ClusterLink[]     0x14 bytes        map +0x10, +0x14 (absent when 0)
//   f32 linkDistances[]                 map +0x28, +0x2c (absent when 0)
//
// Path finding is hierarchical: an A* over clusters (search space at
// 0x100bd010..0x100bdbb0, driven by the bot manager's "tryClusterPath",
// vtable 0x10114448 slot 9 = 0x10041920) and an A* over polygons (search
// space 0x100bc0f0..0x100bcef0, "tryPath", slot 8 = 0x10040410 /
// 0x10041800). Both run on a shared, time-sliced A* driver (0x10044880).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe::ai {

struct Vec3 {
	float x = 0, y = 0, z = 0;

	Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
	Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
	Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
	Vec3& operator+=(const Vec3& o) { return *this = *this + o; }
	bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
};

float distance(const Vec3& a, const Vec3& b);

// A quad tree node (0x18 bytes). Children cover the quarters of the node's
// square: [0] low x, low z; [1] low x, high z; [2] high x, low z; [3] high
// x, high z ("high" includes the midpoint). Index 0 means no child (the
// root is node 0). Leaves keep garbage (0xcdcdcdcd) in the child slots.
struct QuadNode {
	std::array<uint32_t, 4> children {}; // +0x00
	bool leaf = false;                   // +0x10
	uint8_t polygonCount = 0;            // +0x11 polygons in the leaf
	uint32_t firstPolygon = 0;           // +0x14 into leafPolygons
};

// A walkable polygon (0x14 bytes, element constructor 0x100b8050).
struct Polygon {
	uint8_t vertexCount = 0; // +0x00 (3 in shipped data)
	// +0x01: the cost class, an index into a CostProfile (0x100b8010 reads,
	// 0x100b8030 writes it; blocking an area at run time sets it to 0, which
	// costs -1 = impassable, 0x100b8dc0).
	uint8_t costClass = 0;
	uint16_t cluster = 0;    // +0x02 index into NavMesh::clusters
	uint32_t firstEdge = 0;  // +0x04 into edgeVertices/edgeNeighbours
	Vec3 normal;             // +0x08 unit plane normal (0x100b7f90)
};

// A cluster (0x10 bytes, .clb).
struct Cluster {
	uint32_t firstPolygon = 0;  // +0x00 into clusterPolygons
	uint32_t polygonCount = 0;  // +0x04
	uint16_t firstLink = 0;     // +0x08 into links
	uint16_t linkCount = 0;     // +0x0a
	uint16_t firstDistance = 0; // +0x0c into linkDistances
	// +0x0e: whether the cluster may be entered (0x100bd270 skips links into
	// a cluster whose byte is 0; 0x100b8dc0 clears it when the cluster is
	// blocked). The tool left it uninitialised (0xcd) in the files, which
	// reads as open.
	bool open = true;
};

// A link from one cluster into a neighbouring one (0x14 bytes).
struct ClusterLink {
	Vec3 position;              // +0x00 a point on the shared border
	uint16_t targetCluster = 0; // +0x0c (>= cluster count: unusable, 0x100bd270)
	uint32_t polygon = 0;       // +0x10 the border polygon (on either side)
};

// Per-cost-class multipliers (0x100bdfb0 builds the default: nine classes,
// all 1 except class 0 = -1 (impassable) and classes 5 and 7 = 1000).
// Scripts change them per bot kind (0x1004fcf0). Negative = impassable.
struct CostProfile {
	std::array<float, 9> costs {-1, 1, 1, 1, 1, 1000, 1, 1000, 1};

	// Classes past the table are treated as plain ground (the original would
	// read past its vector).
	float cost(uint8_t costClass) const { return costClass < costs.size() ? costs[costClass] : 1.0f; }
};

class NavMesh {
public:
	// .qtr
	uint32_t treeCount = 0;
	uint32_t version = 0;
	float minX = 0, minZ = 0, maxX = 0, maxZ = 0;
	float rootSize = 0;
	std::vector<QuadNode> nodes;
	std::vector<Polygon> polygons;
	std::vector<uint32_t> leafPolygons;
	std::vector<Vec3> vertices;
	std::vector<uint32_t> edgeVertices;
	std::vector<int32_t> edgeNeighbours;
	// .clb
	uint32_t clusterVersion = 0;
	std::vector<Cluster> clusters;
	std::vector<uint32_t> clusterPolygons;
	std::vector<ClusterLink> links;
	std::vector<float> linkDistances;

	// Edge k of a polygon runs from vertex k to vertex k+1 (wrapping).
	const Vec3& vertex(uint32_t polygon, uint32_t k) const; // 0x100b7e80 / 0x100b8260
	// The polygon across edge k, -1 at the mesh border (0x100b7df0).
	int32_t neighbour(uint32_t polygon, uint32_t k) const;
	// The average of the polygon's vertices (0x100b7ef0).
	Vec3 centroid(uint32_t polygon) const;
	// The midpoint of edge k (0x100bc5f0): where paths cross into the
	// neighbour.
	Vec3 edgeMidpoint(uint32_t polygon, uint32_t k) const;
	// Whether x/z lies inside the polygon seen from above, 1 mm of slack
	// (0x100b8060).
	bool containsXZ(uint32_t polygon, const Vec3& point) const;
	// The height of the polygon's plane at the point's x/z.
	float planeHeight(uint32_t polygon, const Vec3& point) const;
	// Whether the point stands on this polygon (0x100b87b0): inside it from
	// above, with the surface at most `heightTolerance` below the point or
	// at most 2 m above it. `heightOffset` gets surface y - point y.
	bool standsOn(uint32_t polygon, const Vec3& point, float heightTolerance, float* heightOffset = nullptr) const;

	// The polygon a point stands on (0x100b7d70 -> quad tree walk 0x100f40e0),
	// or -1. Inside the bounds, descend to the leaf holding the x/z, then
	// among its upward-facing polygons the profile can walk on, pick the
	// one whose surface is vertically closest. The bot manager uses a
	// tolerance of 2.5 for one bot kind (3) and 5 for the others
	// (0x1003f2d0). A null profile skips the cost check.
	int32_t findPolygon(const Vec3& point, float heightTolerance = 5.0f, const CostProfile* profile = nullptr,
		float* heightOffset = nullptr) const;
	// The quad tree leaf holding x/z, or -1.
	int32_t findLeaf(float x, float z) const;

	// The walking distance inside a cluster between two of its links, by
	// their index within the cluster (0x100b0730: the upper triangle of the
	// link-by-link matrix, row by row). Equal indices give 0.
	float linkDistance(uint16_t cluster, uint16_t a, uint16_t b) const;
	// The link in the target cluster that leads back over the same border
	// (0x100b0920: same polygon, within 0.1 m), as an index within that
	// cluster, or 0xffff.
	uint16_t reverseLink(uint32_t link) const;
};

// Parses a tree from a .qtr and its .clb. Fails (with a message) on a
// version other than 3/5, a short file or an index out of range. Only the
// first tree is read when the .qtr claims several (the shipped levels have
// one; the original's later trees would follow in both files).
std::optional<NavMesh> parseNavMesh(const std::vector<uint8_t>& qtr, const std::vector<uint8_t>& clb,
	std::string* error = nullptr);

struct PathOptions {
	CostProfile profile;
	// The vehicle cost (poly search space +0x34, set through vtable slot 46
	// 0x10040700): slope and bend penalties instead of the uphill factor.
	bool vehicle = false;
	// Locating the start and goal (see NavMesh::findPolygon).
	float heightTolerance = 5.0f;
	// Search a cluster corridor first and keep the polygon search inside it
	// (falling back to the whole mesh). See findPath.
	bool useClusters = true;
	// The A* driver gives up after this many expanded nodes, or when the
	// node it expands is this deep (search +0x08, +0x0c, set by the caller
	// through slots 42/43). 0 = no limit. The defaults are ours.
	uint32_t maxExpansions = 200000;
	uint32_t maxDepth = 0;
};

// One step of a polygon path: the polygon and where the path enters it
// (the start point for the first, then edge midpoints).
struct PathStep {
	uint32_t polygon = 0;
	Vec3 position;
};

// The A* over clusters (0x10041920 + the cluster search space): the
// clusters from `from`'s to `to`'s, empty when there is no way. Each node
// is a cluster entered over a link; its successors are the neighbouring
// open clusters, reached over the cheapest link to each (cost from the
// start point, or the precomputed distance from the entry link), the
// heuristic is the straight line from the entry link to the goal, and the
// search ends when the goal cluster is generated.
std::vector<uint16_t> findClusterPath(const NavMesh& mesh, uint32_t startPolygon, const Vec3& from,
	uint32_t goalPolygon, const Vec3& to, const PathOptions& options = {});

// The A* over polygons (0x10041800 + the polygon search space). Neighbours
// are crossed at edge midpoints; the cost is distance times the cost
// class's multiplier and a slope factor (0x100bc300); the heuristic is the
// straight line to the goal point (0x100bc590); the search ends when the
// goal polygon is generated. `allowedClusters` (search +0x30), when not
// null, restricts the search to polygons whose cluster is flagged in it.
// Empty when there is no path.
std::vector<PathStep> findPolygonPath(const NavMesh& mesh, uint32_t startPolygon, const Vec3& from,
	uint32_t goalPolygon, const Vec3& to, const PathOptions& options = {},
	const std::vector<bool>* allowedClusters = nullptr);

// The waypoints from `from` to `to`: the start point, the midpoint of each
// polygon edge crossed, the goal point. Empty when either point is off the
// mesh or there is no path.
//
// PROVISIONAL composition: the original runs the cluster and polygon
// searches as separate bot-manager requests ("tryClusterPath", "tryPath")
// and the bots' code that combines them (what it puts in the polygon
// search's goal set +0x2c and allowed-cluster set +0x30) isn't rewritten
// yet. Here a cluster corridor (when useClusters) bounds one polygon
// search to the goal, retried on the whole mesh if that fails. The
// original's path list also stops at the midpoint into the goal polygon;
// the goal point is appended here.
std::vector<Vec3> findPath(const NavMesh& mesh, const Vec3& from, const Vec3& to, const PathOptions& options = {});

} // namespace dice::hfe::ai
