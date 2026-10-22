// Undergrowth: grass and ground clutter scattered over the terrain near the
// camera (RendDX9 "UndergrowthSystem", init 0x101100d0).
//
// A level has three files:
//   Undergrowth.cfg  view distances, patch subdivision, sway, lighting
//   Undergrowth.dat  per material the types to scatter, each with its mesh
//                    baked in (loader 0x1010fd90)
//   Undergrowth.raw  one material byte per heightmap sample
//
// The terrain is split into patches (terrain patches subdivided
// PatchSubdivide times per side). A patch is generated when it comes into
// view (0x1010ba40): each type lays a jittered grid of sqrt(density) points
// per metre over the patch, and a point keeps an instance where the material
// map holds the type's material. A Lehmer random generator seeded with the
// patch index jitters the points and picks each instance's scale, rotation
// and sway, so a patch always looks the same.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe::terrain {

struct UndergrowthConfig {
	float viewDistance = 30;
	float viewDistanceFadeScale = 0.5f;
	float viewDistanceHeightScale = 0.4f;
	float viewDistanceHeight2Scale = 0.4f;
	float viewDistanceStreamingScale = 1;
	int patchSubdivide = 15;
	float swayScale = 0.15f;
	float lightingScale = 1.2f;
	float alphaRef = 0.3f;
};

// Only the top-level settings; the material and type blocks are the
// editor's source of Undergrowth.dat.
UndergrowthConfig parseUndergrowthConfig(const std::string& text);

struct UndergrowthVertex {
	std::array<float, 3> position; // mesh space
	std::array<uint16_t, 2> texCoord; // into the undergrowth atlas, / 32767
	uint8_t flags;            // byte 0x10, always 0 in the shipped files
	uint8_t terrainColorMix;  // (1 - TerrainColorScale) * 255: how much the terrain color is ignored
	float sway;               // TypeSwayScale on the vertices that sway
};

struct UndergrowthType {
	std::string name;
	std::string mesh;
	std::string texture;
	std::array<float, 2> crossSize {1, 1}; // CrossSize; [1] also scales the sway byte and Skew
	std::array<float, 2> sizeRange {1, 1};  // RandomSizeScale
	std::array<float, 2> normalYRange {0, 1}; // instances only where the terrain normal's y is in range
	float density = 0;          // instances per square metre
	float variation = 0;        // jitter, in grid cells; negative scatters freely
	float terrainColorScale = 1;
	float swayScale = 0;        // TypeSwayScale (version 2)
	bool skew = false;          // Skew: stand the first two quads' tops above their bottoms
	std::vector<UndergrowthVertex> vertices;
	std::vector<uint16_t> indices;
	std::vector<uint32_t> extra; // unknown; empty in the shipped files
};

struct UndergrowthMaterial {
	std::string name;
	uint32_t index = 0; // the value in Undergrowth.raw
	float generalHeight = 0;
	std::vector<UndergrowthType> types;
};

// Undergrowth.dat (version 2; version 1 lacks TypeSwayScale):
//   u32 version, u32 material count; per material:
//     string name, u32 index, f32 general height, u32 type count; per type:
//       string name, mesh, texture
//       f32[2] cross size, f32[2] size range, f32[2] normal y range,
//       f32 density, f32 variation (clamped to -1..1 by the loader),
//       f32 terrain color scale, [f32 sway scale], u8 skew
//       u32 vertex count, 24 bytes each: f32[3] position, u16[2] uv,
//           u8 flags, u8 terrain color mix, u16 padding, f32 sway
//       u32 index count, u16 each; u32 count, u32 each
//   (strings are a u32 length and the characters)
std::optional<std::vector<UndergrowthMaterial>> parseUndergrowthData(const std::vector<uint8_t>& data,
	std::string* error = nullptr);

// The generator's random numbers (0x10025420): the engine's Lehmer
// generator (Game/Random.h).
uint32_t undergrowthRandom(uint32_t& state);

// A generated vertex in world space.
struct UndergrowthPatchVertex {
	std::array<float, 3> position; // y is the ground height under the instance
	float height;                  // the vertex's height above that ground
	std::array<float, 2> texCoord;
	uint8_t swayIndex;  // one of 16 sway offsets
	uint8_t sway;       // how far the vertex sways, 0..255
	uint8_t terrainColorMix;
	const UndergrowthType* type;
};

struct UndergrowthPatch {
	std::vector<UndergrowthPatchVertex> vertices;
	std::vector<uint32_t> indices;
	size_t instances = 0;
};

class UndergrowthGenerator {
public:
	struct Terrain {
		// The material map covers the heightmap: one byte per sample, rows
		// along z, centred on the origin.
		std::vector<uint8_t> materialMap;
		int mapWidth = 0;
		int mapHeight = 0;
		std::array<float, 2> sampleSpacing {1, 1}; // heightmap scale x, z
		int patchesPerSide = 0; // terrain patches per side * PatchSubdivide
		float patchSize = 0;    // metres
		std::function<float(float x, float z)> height;
		std::function<std::array<float, 3>(float x, float z)> normal;
	};

	UndergrowthGenerator(std::vector<UndergrowthMaterial> materials, Terrain terrain);

	const std::vector<UndergrowthMaterial>& materials() const { return m_materials; }
	int patchesPerSide() const { return m_terrain.patchesPerSide; }
	float patchSize() const { return m_terrain.patchSize; }
	// The patch's corner with the lowest x and z.
	std::array<float, 2> patchOrigin(int patch) const;
	// Vertex and index budget per patch: what the densest material could
	// produce, plus a tenth (0x101100d0).
	size_t vertexCapacity() const { return m_vertexCapacity; }
	size_t indexCapacity() const { return m_indexCapacity; }

	// The material at a world position (0 outside the map).
	uint8_t materialAt(float x, float z) const;
	// The fraction of the patch's map samples that hold each material 0-15 (0x1010b240).
	std::array<float, 16> materialPresence(int patch) const;

	UndergrowthPatch generate(int patch) const;

private:
	void mapSample(float x, float z, int& sx, int& sz) const;

	std::vector<UndergrowthMaterial> m_materials;
	Terrain m_terrain;
	size_t m_vertexCapacity = 0;
	size_t m_indexCapacity = 0;
};

} // namespace dice::hfe::terrain
