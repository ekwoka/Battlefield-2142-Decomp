// The primary terrain's levels of detail as RendDX9's terrain culler and
// TerrainShader pick and morph them (no GPU code: shared with the tests).
//
// terraindata.raw holds every patch at full detail with three geomorph
// deltas per vertex and one index tree for all patches (TerrainData.h).
// Each frame the culler (0x100a07a0, cullerType 3: 0x1009f210) walks its
// quadtree down to cells of terrainCuller.baseCellSize metres and gives
// each a LOD by the horizontal distance from the camera to the cell's box
// plus the camera's height above the ground:
//   s = max(round(zoom x lodSwitchDistance x 0.75), baseCellSize)
//   LOD 0 up to s, LOD 1 up to s x lod2Start, LOD 2 beyond
// (100 x 0.75 = 75 m and 225 m by default), the zoom being RenderView's
// tan(first fov / 2) / tan(fov / 2). Then (0x1009cfa0) a LOD 2 cell with a
// LOD 0 cell among its eight neighbours becomes LOD 1, and a cell keeps the
// vertices of a visible neighbour one LOD finer along that edge (the
// stitch mask: 1 its -z edge, else 2 its +z edge; 4 its -x edge, else 8
// its +x edge).
//
// The vertex shader (TerrainShader_Shared.fx geoMorphPosition) moves a
// vertex by
//   y -= dot(selector, delta) x interp + dot(adder[edge step], delta)
//   interp = saturate(d^2 x limits.x - limits.y)
// with d^2 = dx^2 + (height above ground)^2 + dz^2, the limits the cell's
// LOD's (0x100af7e0: 1 / (stop - start)^2 and start^2 / (stop - start)^2,
// start and stop lod<n>SwitchStart / Stop x s; none at LOD 2), the
// selector picking the LOD's delta and the adders (0x1014d700) moving a
// stitched edge's extra vertices fully onto the coarser line. With the
// defaults a LOD 0 vertex is fully morphed from 48 m and LOD 0 ends at
// 75 m (shader distance at least 53 m there); LOD 1 morphs from 150 m to
// 158.9 m and ends at 225 m (at least 159.1 m): no pops, and no cracks but
// one the rewrite closes (TerrainMorph::next).
#pragma once

#include "RenderMath.h"
#include "Scene/LightSettings.h"

#include <cmath>
#include <vector>

namespace dice::hfe::render {

struct TerrainLodCell {
	Vec3f boundsMin, boundsMax;
	int lod = 0;
	int mask = 0;
	bool visible = false;
};

// The LOD 0 distance (the culler's +0, 0x100a07a0).
inline float terrainLodSwitch(float zoom, const scene::RendererSettings& settings)
{
	return std::fmax(static_cast<float>(std::lround(zoom * static_cast<float>(settings.lodSwitchDistance) * 0.75f)),
		static_cast<float>(settings.baseCellSize));
}

// LODs and stitch masks of a square grid of cells (row-major, +x then +z),
// their visibility set.
inline void pickTerrainLods(std::vector<TerrainLodCell>& cells, int perSide, float cameraX, float cameraZ,
	float heightAboveGround, float lodSwitch, float lod2Start)
{
	for (TerrainLodCell& cell : cells) {
		const float dx = cameraX < cell.boundsMin.x ? cell.boundsMin.x - cameraX
			: cameraX > cell.boundsMax.x            ? cameraX - cell.boundsMax.x
													: 0.0f;
		const float dz = cameraZ < cell.boundsMin.z ? cell.boundsMin.z - cameraZ
			: cameraZ > cell.boundsMax.z            ? cameraZ - cell.boundsMax.z
													: 0.0f;
		const float d = std::sqrt(dx * dx + dz * dz) + heightAboveGround;
		cell.lod = d <= lodSwitch ? 0 : d <= lodSwitch * lod2Start ? 1 : 2;
		cell.mask = 0;
	}
	const auto at = [&cells, perSide](int x, int z) -> TerrainLodCell* {
		return x < 0 || z < 0 || x >= perSide || z >= perSide ? nullptr : &cells[static_cast<size_t>(z) * perSide + x];
	};
	// RendDX9 forces and stitches in one pass over the visible cells, so a
	// cell forced late can miss a stitch; the rewrite takes all LODs first.
	for (int z = 0; z < perSide; ++z) {
		for (int x = 0; x < perSide; ++x) {
			TerrainLodCell& cell = *at(x, z);
			for (int j = -1; j <= 1 && cell.lod == 2; ++j) {
				for (int i = -1; i <= 1; ++i) {
					const TerrainLodCell* other = at(x + i, z + j);
					if (other && other != &cell && other->lod == 0) {
						cell.lod = 1;
					}
				}
			}
		}
	}
	for (int z = 0; z < perSide; ++z) {
		for (int x = 0; x < perSide; ++x) {
			TerrainLodCell& cell = *at(x, z);
			if (!cell.visible || cell.lod == 0) {
				continue;
			}
			const auto finer = [&cell](const TerrainLodCell* other) {
				return other && other->visible && other->lod == cell.lod - 1;
			};
			cell.mask = (finer(at(x, z - 1)) ? 1 : finer(at(x, z + 1)) ? 2 : 0) |
				(finer(at(x - 1, z)) ? 4 : finer(at(x + 1, z)) ? 8 : 0);
		}
	}
}

// A LOD's morph constants, the deltas taken in metres (the selector and
// adders over RendDX9's y scale).
struct TerrainMorph {
	float camera[4] {};   // x, unused, z, height above the ground
	float limits[4] {};   // NEARFARMORPHLIMITS, 1 to morph, 1 to fade the detail by it (LOD 0)
	float selector[4] {}; // MORPHDELTASELECTOR
	float adder[3][4] {}; // MORPHDELTAADDER by edge step
	// The rewrite's own: LOD 1's limits and the weight of the LOD 2 delta for
	// a LOD 0 cell. RendDX9 morphs a LOD 0 cell only towards LOD 1, but such a
	// cell reaches up to 75 + 64 x sqrt 2 m away while its LOD 1 neighbours
	// start morphing towards LOD 2 at 150 m, so their shared edge parts by
	// up to a decimetre there (a low camera beside the cell); morphing LOD 0's
	// far vertices on towards LOD 2 too keeps the edge shut.
	float next[4] {};
};

inline TerrainMorph terrainMorph(int lod, bool morph, float lodSwitch, const scene::RendererSettings& settings,
	float cameraX, float cameraZ, float heightAboveGround)
{
	TerrainMorph u;
	u.camera[0] = cameraX;
	u.camera[2] = cameraZ;
	u.camera[3] = heightAboveGround;
	if (lod < 2) {
		const float start = lodSwitch * (lod == 0 ? settings.lod0SwitchStart : settings.lod1SwitchStart);
		const float stop = lodSwitch * (lod == 0 ? settings.lod0SwitchStop : settings.lod1SwitchStop);
		const float inverse = stop > start ? 1.0f / (stop - start) : 0.0f;
		u.limits[0] = inverse * inverse;
		u.limits[1] = start * inverse * start * inverse;
	}
	u.limits[2] = morph ? 1.0f : 0.0f;
	u.limits[3] = lod == 0 ? 1.0f : 0.0f;
	if (lod >= 0 && lod < 3) {
		u.selector[lod] = 1;
	}
	for (auto& adder : u.adder) {
		adder[2] = 1; // the third delta (0 in every shipped file)
	}
	if (lod == 0) {
		const float start = lodSwitch * settings.lod1SwitchStart, stop = lodSwitch * settings.lod1SwitchStop;
		const float inverse = stop > start ? 1.0f / (stop - start) : 0.0f;
		u.next[0] = inverse * inverse;
		u.next[1] = start * inverse * start * inverse;
		u.next[2] = 1;
	}
	if (lod == 1) {
		u.adder[1][0] = 1;
		u.adder[1][2] = 0;
	} else if (lod == 2) {
		u.adder[2][1] = 1;
		u.adder[2][2] = 0;
	}
	return u;
}

// terrain.vert's morph on the CPU: the height a vertex is drawn at.
inline float terrainMorphedHeight(const TerrainMorph& u, float x, float y, float z, const float delta[3], int edge)
{
	if (u.limits[2] < 0.5f || edge < 0) {
		return y;
	}
	const float dx = u.camera[0] - x, dz = u.camera[2] - z, dy = u.camera[3];
	const float d2 = dx * dx + dy * dy + dz * dz;
	const float interp = std::fmin(std::fmax(d2 * u.limits[0] - u.limits[1], 0.0f), 1.0f);
	const float further = std::fmin(std::fmax(d2 * u.next[0] - u.next[1], 0.0f), 1.0f) * u.next[2];
	const int e = edge < 0 ? 0 : edge > 2 ? 2 : edge;
	float selected = 0, added = 0;
	for (int i = 0; i < 3; ++i) {
		selected += u.selector[i] * delta[i];
		added += u.adder[e][i] * delta[i];
	}
	return y - selected * interp - added - delta[1] * further;
}

} // namespace dice::hfe::render
