// The compiled terrain of a level (terraindata.raw, loaded by "terrain.load",
// RendDX9 0x100b3760).
//
// Header (version 26 in BF2142):
//   u16 version, u16 flags
//   primary and secondary world scale (3 floats each), a never-set i32
//   (0xcdcdcdcd), the highest and lowest height in metres (2 floats)
//   i32 patch size (quads), u8 subdivide, i32 patches per side,
//   i32 colormap size, i32 low-detail map size
//   base names of colormaps, detail maps, low-detail maps and lightmaps,
//   each terminated by '\n' (patch files append "XXxYY")
//   far side tiling (2 floats), far top tiling high, [low (version >= 23)],
//   far y offset, sun color, GI color, water color (3 floats each)
//   i32 detail layer count; per layer: texture name ('\n'), u8 flag,
//   2 floats, float tiling, i32, u8
// RendDX9 stores the sun color scaled by 1/4 and the GI color by 1/2.
//
// Then (decoded from the loader's callees; the whole file is walked to its
// end, see TerrainMeshes):
//   - one LOD index tree all patches share (0x100b00a0, version >= 0x1a):
//     85 quadtree nodes (1 + 4 + 16 + 64, over 128 / 64 / 32 / 16 quads),
//     3 LODs each as (i32 start, i32 count) ranges of one index buffer;
//     nodes 0-20 have, at LODs 1 and 2, nine ranges by stitch mask (in the
//     order 0, 1, 2, 4, 8, 5, 9, 6, 10: bit 1 the row z = 0, 2 the row z =
//     patch size, 4 the column x = 0, 8 the column x = patch size keep the
//     finer vertices towards a finer neighbour); LOD l takes every 2^l-th
//     vertex. Then u32 n and n u16 indices (row x (patch size + 1) + column),
//     then u8 baseGrid (0x40a: a separate grid stream and 20-byte vertices).
//   - per patch, row-major (0x100aed30 -> 0x1014be90): i32 x, z (grid index
//     from the centre), f32 highest, lowest, vec3 centre, f32 radius (its
//     bounding sphere), (patch size + 1)^2 vertices of 24 bytes (u8 edge
//     step, u8 row, u8 column, u8 0, f32 height in heightmap units (times
//     the primary scale's y), 3 x f32 geomorph deltas in those units, u8[4]
//     normal as a D3DCOLOR (B, G, R, A; (c - 127.5) / 127.5, y up = G)), then
//     detail-material index lists until an i32 -1 (0x1014bc80 -> 0x10178cd0:
//     i32 marker, i32 material, vec3 + f32 (zero), i32 index count, i32
//     triangle count, u32 4, i32 node count, node count x 3 (start, count),
//     the u16 indices into the patch's vertices; only LOD 0 filled).
//   - the surrounding terrains (0x100aedb0 -> 0x101506d0) until an i32 -1:
//     i32 k (0..7), f32 highest, lowest, vec3 centre, f32 radius, n heights
//     (f32, times the secondary scale's y) and n normals (vec3); n is not
//     stored: it is the vertex count of the mesh RendDX9 generates
//     (0x100b3fe0 / 0x100b3c70) for an edge (odd k) or a corner (even k).
//   - the culling quadtree (0x1009da80 / 0x1009cac0): i32 n, n x 44 bytes
//     (8 u16 neighbours, vec3 min, vec3 max, 3 u8 flags, u8 patch or 0xff).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
namespace io {
class FileManager;
}
} // namespace dice::hfe

namespace dice::hfe::terrain {

struct DetailLayer {
	std::string texture; // without ".dds"
	bool sideProjection = false; // the flag byte; set for rock layers
	std::array<float, 2> sideTiling {};
	float tiling = 1; // repeats across a patch
};

// A range of a shared index buffer (indices, not triangles).
struct IndexRange {
	int32_t start = 0;
	int32_t count = 0;
};

// One vertex of a patch's compiled mesh.
struct TerrainVertex {
	uint8_t edge = 0;          // on a border, the LOD step it drops out at
	uint8_t row = 0, column = 0;
	float height = 0;          // heightmap units
	std::array<float, 3> morph {}; // geomorph deltas, heightmap units
	std::array<uint8_t, 4> normal {}; // B, G, R, A
};

struct TerrainPatchMesh {
	int x = 0, z = 0;          // from the centre
	float highest = 0, lowest = 0;
	std::array<float, 3> centre {};
	float radius = 0;
	std::vector<TerrainVertex> vertices; // (patch size + 1)^2, row-major
	struct Material {
		int material = 0;
		std::vector<uint16_t> indices;
	};
	std::vector<Material> materials;
};

// The meshes after the header.
struct TerrainMeshes {
	// Per quadtree node, per LOD, by stitch mask (0..10; the masks not
	// stored are empty).
	std::vector<std::array<std::array<IndexRange, 11>, 3>> nodes;
	std::vector<uint16_t> indices; // the shared index buffer
	bool baseGrid = false;
	std::vector<TerrainPatchMesh> patches;
	int surroundingRecords = 0;
	int cullingNodes = 0;
};

struct TerrainData {
	int version = 0;
	std::array<float, 3> primaryWorldScale {};
	std::array<float, 3> secondaryWorldScale {};
	int patchSize = 0;
	int patchesPerSide = 0;
	int colormapSize = 0;
	int lowDetailmapSize = 0;
	std::string colormapBaseName;
	std::string detailmapBaseName;
	std::string lowDetailmapBaseName;
	std::string lightmapBaseName;
	std::array<float, 2> farSideTiling {};
	float farTopTilingHigh = 0;
	float farTopTilingLow = 0;
	float farYOffset = 0;
	std::array<float, 3> sunColor {};
	std::array<float, 3> giColor {};
	std::array<float, 3> waterColor {};
	std::vector<DetailLayer> layers;
	float highest = 0, lowest = 0; // metres
	// The patch meshes, when the file holds them in the known layout
	// (empty otherwise).
	std::optional<TerrainMeshes> meshes;
};

std::optional<TerrainData> parseTerrainData(const std::vector<uint8_t>& data, std::string* error = nullptr);

// "terrain.create" and "terrain.load <path>". `terrain` receives the data.
void registerTerrainDataConsoleMethods(Console& console, const io::FileManager& files,
	std::optional<TerrainData>& terrain);

} // namespace dice::hfe::terrain
