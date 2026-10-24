// Collision meshes (.collisionmesh), loaded by BF2/Physics/CollisionMeshTemplate.
//
// A template's collision mesh has geometries (normal, destroyed, ...), each
// with sub-geometries (parts) and LODs for different users: in BF2-engine
// content LOD 0 is for projectiles, 1 for vehicles and 2 for soldiers.
//
// Layout (versions 8, 9 and 10): u32 0, u32 version, u32 geometry count; per
// geometry a u32 sub-geometry count; per sub-geometry a u32 LOD count; per LOD:
//   u32 collision type (version >= 9)
//   u32 face count, faces of 4 x u16 (three vertex indices and a material)
//   u32 vertex count, vertices of 3 floats, then a u16 per vertex
//   bounds (min, max), a byte, bounds again
//   BSP: u32 count + 16-byte nodes, u32 count + u16 face lists,
//        u32 count + u32 (version >= 10)
// After parsing, the file is trimmed as the loader trims it (see
// parseCollisionMesh): vehicles' files lead every geometry with an empty
// sub-geometry, which goes.
// Checked against all 3,765 collision meshes in BF2142: all but three parse
// to the last byte; those three are truncated or malformed in the shipped
// data and are rejected.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe::physics {

struct CollisionFace {
	std::array<uint16_t, 3> vertices;
	uint16_t material; // index into the template's mapMaterial table
};

struct CollisionLod {
	uint32_t collisionType = 0;
	std::vector<CollisionFace> faces;
	std::vector<std::array<float, 3>> vertices;
	// A material index per vertex (into the template's mapMaterial table,
	// like the faces'): the LOD's +0xd8, read by the mesh's slot 0x44
	// (0x006de710), the material of a vertex in the mobile mesh physics'
	// contacts (0x006bb050).
	std::vector<uint16_t> vertexMaterials;
	std::array<float, 3> boundsMin {};
	std::array<float, 3> boundsMax {};
	// The BSP that accelerates queries; kept as stored for now.
	std::vector<std::array<uint8_t, 16>> bspNodes;
	std::vector<uint16_t> bspFaceLists;
	std::vector<uint32_t> bspExtra;
};

struct CollisionSubGeometry {
	std::vector<CollisionLod> lods;
};

struct CollisionGeometry {
	std::vector<CollisionSubGeometry> subGeometries;
};

struct CollisionMeshFile {
	uint32_t version = 0;
	std::vector<CollisionGeometry> geometries;
};

// The LOD a user (0 projectiles, 1 vehicles, 2 soldiers, 3 AI navigation)
// collides with, or null when the part has none for it: the loader
// (0x006e4880) files each LOD under its collision type (version 9 on; in
// version 8 by order) and leaves the other types empty (-1), so a bush with
// only a projectile LOD does not stop soldiers or vehicles.
const CollisionLod* collisionLodFor(const CollisionMeshFile& file, const CollisionSubGeometry& part, uint32_t type);

// The sub-geometry an instance of `geometry` collides with in `state`
// (the instance's +0x14, 0x006e5d70: 0 normally, 1 for a wreck through
// slot 0x30, 0x006e5df0), or null. Every per-LOD query of a mesh instance
// (slots 10-28, 0x006e5b10 .. 0x006e5ab0) goes through this one
// sub-geometry; only the radius (0x006dedb0) and the box (0x006deeb0) look
// across them all. A part's collisionPart is the geometry (0x0063ccc0
// hands the template's +0x74 to the collision manager's slot 0x28,
// 0x006d8180, which picks the geometry by it).
const CollisionSubGeometry* collisionPart(const CollisionMeshFile& file, size_t geometry, size_t state = 0);

// The same as a range of zero or one sub-geometries, for loops.
struct CollisionPartRange {
	const CollisionSubGeometry* first = nullptr;
	const CollisionSubGeometry* begin() const { return first; }
	const CollisionSubGeometry* end() const { return first ? first + 1 : nullptr; }
};
inline CollisionPartRange collisionParts(const CollisionMeshFile& file, size_t geometry, size_t state = 0)
{
	return {collisionPart(file, geometry, state)};
}

// A LOD's radius (the mesh's slot 0x50, 0x006de7d0: the LOD's +0xc0,
// copied at load from its box's +0x18, which 0x006aeda0 sets): the
// farthest of the box's eight corners from the object's origin.
float boxRadius(const std::array<float, 3>& boundsMin, const std::array<float, 3>& boundsMax);

std::optional<CollisionMeshFile> parseCollisionMesh(const std::vector<uint8_t>& data, std::string* error = nullptr);

} // namespace dice::hfe::physics
