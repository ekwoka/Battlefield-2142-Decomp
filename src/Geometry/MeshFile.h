// BF2-engine mesh files: .staticmesh, .bundledmesh and .skinnedmesh
// ("dice.hfe.geom.GeometryTemplate.StaticMesh" etc. in RendDX9.dll).
//
// Layout (little-endian; versions 4, 6, 10 and 11 occur in BF2142):
//   header      5 x i32 (second is the version), 1 byte
//   geometries  i32 count, then i32 LOD count per geometry
//   vertex declaration  i32 count, then {u16 stream, u16 offset, u16 type, u16 usage}
//               (Direct3D 9 D3DDECLTYPE/D3DDECLUSAGE values; stream 0xff ends it)
//   vertices    i32 element size (4), i32 stride, i32 count, data
//   indices     i32 count, u16 data
//   i32         (not in skinned meshes)
//   per LOD     bounds min/max (+ pivot for version <= 6);
//               skinned: i32 rig count, per rig i32 bone count and {i32 id, 4x4 matrix} per bone;
//               static:  i32 node count and a 4x4 matrix per node;
//               bundled: i32 node (part) count only
//   per LOD     i32 material count; per material:
//               i32 alpha mode (not skinned), string effect, string technique,
//               i32 map count + strings, i32 vertex start, index start, index count,
//               vertex count, 2 x i32, bounds min/max (version 11 static/bundled only)
// Strings are an i32 length followed by the characters.
//
// The layout follows the BF2 modding community's documentation and was
// checked against every mesh shipped with BF2142 (all parse to the last byte).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe::geom {

// Road: a compiled road template (RoadFile.h), not a mesh file.
enum class MeshKind { Static, Bundled, Skinned, Road };

// Direct3D 9 vertex element types and usages that the meshes use.
enum class VertexType : uint16_t { Float1 = 0, Float2 = 1, Float3 = 2, Float4 = 3, D3DColor = 4 };
enum class VertexUsage : uint8_t {
	Position = 0,
	BlendWeight = 1,
	BlendIndices = 2,
	Normal = 3,
	TexCoord = 5,
	Tangent = 6,
};

struct VertexElement {
	uint16_t offset;
	VertexType type;
	VertexUsage usage;
	uint8_t usageIndex; // e.g. TexCoord 1
};

struct Bounds {
	std::array<float, 3> min {};
	std::array<float, 3> max {};
};

struct MeshMaterial {
	uint32_t alphaMode = 0; // 0 opaque, 1 blend, 2 alpha test
	std::string effect;     // e.g. "StaticMesh.fx"
	std::string technique;  // e.g. "Base", "BaseDetailCrack"
	std::vector<std::string> maps; // texture paths, meaning depends on the technique
	uint32_t vertexStart = 0;
	uint32_t indexStart = 0;
	uint32_t indexCount = 0;
	uint32_t vertexCount = 0;
};

struct Bone {
	int32_t id;
	std::array<float, 16> matrix;
};

struct MeshLod {
	Bounds bounds;
	std::vector<std::array<float, 16>> nodes;  // static meshes
	uint32_t partCount = 0;                     // bundled meshes
	std::vector<std::vector<Bone>> rigs;        // skinned meshes
	std::vector<MeshMaterial> materials;
};

struct MeshGeometry {
	std::vector<MeshLod> lods;
};

struct MeshFile {
	MeshKind kind = MeshKind::Static;
	uint32_t version = 0;
	std::vector<VertexElement> vertexElements;
	uint32_t vertexStride = 0;
	uint32_t vertexCount = 0;
	std::vector<uint8_t> vertices;
	std::vector<uint16_t> indices;
	std::vector<MeshGeometry> geometries;

	const VertexElement* element(VertexUsage usage, uint8_t usageIndex = 0) const;
	// Reads a float attribute of a vertex (Float1..Float4) into `out`.
	bool readFloats(uint32_t vertex, const VertexElement& element, float* out, int count) const;
};

// Parses a mesh; `kind` comes from the file extension. Returns nothing on
// malformed data, with the reason in `error`.
std::optional<MeshFile> parseMesh(const std::vector<uint8_t>& data, MeshKind kind, std::string* error = nullptr);

// .staticmesh / .bundledmesh / .skinnedmesh (any case); nothing otherwise.
std::optional<MeshKind> meshKindFromExtension(const std::string& path);

} // namespace dice::hfe::geom
