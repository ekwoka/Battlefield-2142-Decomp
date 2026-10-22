// The compiled terrain of a level (terraindata.raw, loaded by "terrain.load",
// RendDX9 0x100b3760).
//
// The file holds the terrain's render settings and its patches' meshes. The
// header is read here; the patch meshes and LOD morphs that follow are not
// (the renderer builds patches from the heightmap instead).
//
// Header (version 26 in BF2142):
//   u16 version, u16 flags
//   primary and secondary world scale (3 floats each), 3 x i32
//   i32 patch size (quads), u8 subdivide, i32 patches per side,
//   i32 colormap size, i32 low-detail map size
//   base names of colormaps, detail maps, low-detail maps and lightmaps,
//   each terminated by '\n' (patch files append "XXxYY")
//   far side tiling (2 floats), far top tiling high, [low (version >= 23)],
//   far y offset, sun color, GI color, water color (3 floats each)
//   i32 detail layer count; per layer: texture name ('\n'), u8 flag,
//   2 floats, float tiling, i32, u8
// RendDX9 stores the sun color scaled by 1/4 and the GI color by 1/2.
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
};

std::optional<TerrainData> parseTerrainData(const std::vector<uint8_t>& data, std::string* error = nullptr);

// "terrain.create" and "terrain.load <path>". `terrain` receives the data.
void registerTerrainDataConsoleMethods(Console& console, const io::FileManager& files,
	std::optional<TerrainData>& terrain);

} // namespace dice::hfe::terrain
