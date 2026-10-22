// Compiled roads: the road decals the editor bakes from splines.
//
// A level's CompiledRoads.con (run by its Init.con) places each road:
//     Object.create asphalt_sand
//     Object.geometry.loadMesh Levels\<level>\Roads\A1_compiled.mesh
//     Object.absolutePosition x/y/z
// where asphalt_sand is a "RoadCompiled" geometry template. Its look comes
// from <script directory>/<name>_compiled.dat (0x100fee00 in RendDX9):
//     texture path '\n' texture path '\n' (no extension)
//     f32 blend factor, f32 fade start, f32 fade end, u32 priority
//
// <road>_compiled.mesh, as the editor writes it (0x10100ce0):
//     u32 version (0x10004; 0x10003 and 0x10002 in older levels)
//     f32[3] position, f32 radius, f32[3] bounds min, f32[3] bounds max
//     u32 (0), u32 vertex count, vertices, u32 index count, u16 indices
//     version 4 only: u32 count, 24 bytes each (not used for drawing)
// Vertices are relative to the position:
//     versions 3, 4: f32[3] position, f32[2] uv0, f32[2] uv1, f32 alpha (32 bytes)
//     version 2:     f32[3] position, f32[3] normal, f32[2] uv0, f32[2] uv1,
//                    f32[4] unused (56 bytes; no alpha)
// Checked against every shipped road mesh: all versions 2-4 parse to the
// last byte; 9 files (versions 0 and garbage, BF2 leftovers) do not.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe::geom {

struct RoadVertex {
	std::array<float, 3> position; // relative to CompiledRoad::position
	std::array<float, 2> texCoord0; // along (v) and across (u) the road
	std::array<float, 2> texCoord1; // the second texture's
	float alpha = 1;                // 0 at the road's ends
};

struct CompiledRoad {
	uint32_t version = 0;
	std::array<float, 3> position {};
	float radius = 0;
	std::array<float, 3> boundsMin {};
	std::array<float, 3> boundsMax {};
	std::vector<RoadVertex> vertices;
	std::vector<uint16_t> indices; // triangle list
};

struct RoadSettings {
	std::string texture0; // primary, sampled with uv0
	std::string texture1; // detail, sampled with uv1
	float blendFactor = 1; // weight of texture0
	float fadeStart = 0;
	float fadeEnd = 0;
	uint32_t priority = 0;
};

std::optional<CompiledRoad> parseCompiledRoad(const std::vector<uint8_t>& data, std::string* error = nullptr);
std::optional<RoadSettings> parseRoadSettings(const std::vector<uint8_t>& data, std::string* error = nullptr);

} // namespace dice::hfe::geom
