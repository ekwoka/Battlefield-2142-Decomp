// Which point lights light which mesh, as RendDX9's light manager
// (DAT_101ffc84, vtable 0x101d42e0, constructor 0x100aafe0) picks them (no
// GPU code: shared with the tests).
//
// Each frame (0x100a93f0, slot 0x10 from the frame functions 0x10028ae0 /
// 0x10028bb0 / 0x1002cef0) it lists the enabled lights whose sphere (the
// position, radius = the light object's attenuationRange2, 0x0064aa10, 0
// when hidden) meets the camera's frustum with its far plane moved to
// lightCullDistance (+0xa0; none at all when that is 0 or less). Each mesh
// then asks (slot 0x20, 0x100a81a0 -> 0x100a9f80) with its object's origin
// and its radius: the listed lights within light radius + mesh radius
// that pass its filter (0x100a7f00), nearest first by the squared distance
// from the light to the origin (0x100ad8c0 with 0x100abbe0), cut to the
// mesh kind's maximum (0x100ad040). The filters always ask for isDynamic:
//   static meshes (0x100bdb00): Point or Spot (0x1b09), and with a
//     lightmap also affectLightmappedObjects (0x11b09); maxLightsPerStaticMesh;
//   bundled (0x100c3880) and skinned (0x10162070) meshes: Point only and
//     supportsEmitter (0x8b09); maxLightsPerBundledMesh / ...SkinnedMesh.
// A spot reaches a static mesh as a point light (the Light record's
// direction, +0xc, isn't filled for meshes). The shaders' point-light
// passes add one light each (RaShaderSTM / SM / BM "_pointlight_"). The
// filter's bits (0x100a7f00; set = must hold, the second mask = must not)
// ask the light object (slot 0x1d0, enabled) and its template (+0x160):
// 1 enabled, 8 isDynamic (slot 0xf8), 4 / 0x10 castsStaticShadow (0x100),
// 0x20 castsDynamicShadow (0x108), 0x80 selfLights (0x110), 0x40
// selfShadows (0x118), 0x400 / 0x800 / 0x1000 / 0x2000 lightType 0..3
// (Directional, Point, Spot, slot 0xf0), 0x4000 supportsGlow (0x130), 0x8000
// supportsEmitter (0x138), 0x10000 affectLightmappedObjects (0x140). Every
// shipped LightSource sets supportsEmitter 1 (the constructor's default,
// 0x00616d60), so 0x8000 refuses none of them.
//
// Two more callers list lights without a mesh's maximum:
//   the terrain (0x100b4d80): every listed light passing 0x909 (dynamic
//     Point lights, lightmapped or not; slot 0x18, 0x100a9630), each drawn
//     as one more additive pass over the patches its sphere meets
//     (0x1014e080: Hi_Terrain's PerPixelPointlight, pass 13, or pointlight,
//     pass 1): POINTLIGHT = (position, 1 / range2^2, colour) and
//     TerrainShader's calcPVPointTerrain x 0.5;
//   the undergrowth (0x1010e270): per patch, the nearest 4 within reach of
//     its bounding sphere passing 0xb09 (slot 0x1c), PointLightPosAtten /
//     PointLightColor, technique t0_l<n> (Undergrowth.fx: per vertex
//     saturate(1 - d^2 / R^2) x colour, no N.L, added to the sun term).
// lightManager.maxLightsPerTerrainPatch (+0xac, 7 from the constructor)
// limits neither: its getter (slot 0x78) is called only by the renderer's
// state save and restore (0x10028760 / 0x10028590, which keep min(n, 1)),
// and the undergrowth's 4 is a literal.
//
// The dynamic shadows' quality (VideoSettings, RendDX9 settings +0x3c,
// 0x10010040) sets the three maxima and lightCullDistance together: 0 / 0
// m, 1 / 50 m, 2 / 100 m, 3 / 200 m (slots 0x6c, 0x64, 0x74, 0x5c).
//
// Flicker (BF2142.exe's LightSource slot 0xa4, 0x0064a920): with flicker
// over 0 each update draws f = (rand() / 32767 - 1) x flicker + 1 (MSVC's
// rand) and makes the light's ranges the template's x f (then x its scale
// +0x1c8, 1); its colour stays. Static meshes are lit with that range,
// bundled and skinned ones with the template's (their attenuation reads
// the template, 0x100c3880 / 0x10162070).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <utility>
#include <vector>

namespace dice::hfe::render {

enum class LightedMesh { Static = 0, Bundled = 1, Skinned = 2 };

struct LightLimits {
	float cullDistance = 300; // 0x100aafe0
	int staticMesh = 3, bundledMesh = 3, skinnedMesh = 3;
	int maxFor(LightedMesh kind) const
	{
		return kind == LightedMesh::Static ? staticMesh : kind == LightedMesh::Bundled ? bundledMesh : skinnedMesh;
	}
	// 0x10010040's table by the dynamic shadows' quality.
	static LightLimits forShadowQuality(int quality)
	{
		static const float kCull[4] = {0, 50, 100, 200};
		const int q = std::clamp(quality, 0, 3);
		LightLimits l;
		l.cullDistance = kCull[q];
		l.staticMesh = l.bundledMesh = l.skinnedMesh = q;
		return l;
	}
};

// MSVC's rand (214013 / 2531011, bits 16..30), the flicker's draws.
struct MsvcRand {
	uint32_t seed = 1;
	int next()
	{
		seed = seed * 214013u + 2531011u;
		return static_cast<int>((seed >> 16) & 0x7fff);
	}
};

// 0x0064a920's factor for a draw r (0..32767).
inline float flickerFactor(float flicker, int r)
{
	return flicker > 0 ? (static_cast<float>(r) * (1.0f / 32767.0f) - 1.0f) * flicker + 1.0f : 1.0f;
}

struct ListedLight {
	float position[3] {};
	float radius = 0; // the (flickered) attenuationRange2
	bool spot = false;
	bool affectsLightmapped = true;
	bool supportsEmitter = true;
	int owner = -1; // its parent object's group (pickOwnedLights), -1 none
};

// The terrain's filter 0x909 and the undergrowth's 0xb09: Point lights
// (isDynamic and enabled are the list's already).
inline bool terrainTakesLight(const ListedLight& l)
{
	return !l.spot;
}

// The terrain's lights (Hi_Terrain's PerPixelPointlight passes, one per
// listed Point light, 0x1014e080): the rewrite's shader takes `max` of
// them in one pass, the Point ones (0x909) nearest the camera by their
// distance less their radius (a light reaching over the camera first),
// in that order (ties as listed); into `out`, returns how many.
inline int pickTerrainLights(const ListedLight* lights, size_t count, const float camera[3], int max, int* out)
{
	std::vector<std::pair<float, int>> byCamera;
	for (size_t i = 0; i < count; ++i) {
		const ListedLight& l = lights[i];
		const float dx = l.position[0] - camera[0], dy = l.position[1] - camera[1], dz = l.position[2] - camera[2];
		byCamera.push_back({std::sqrt(dx * dx + dy * dy + dz * dz) - l.radius, static_cast<int>(i)});
	}
	std::stable_sort(byCamera.begin(), byCamera.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	int n = 0;
	for (size_t i = 0; i < byCamera.size() && n < max; ++i) {
		if (terrainTakesLight(lights[static_cast<size_t>(byCamera[i].second)])) {
			out[n++] = byCamera[i].second;
		}
	}
	return n;
}

// PointLightPosAtten's w for the terrain and the undergrowth: 1 / R^2,
// R the light's (flickered) attenuationRange2, at least 0.01.
inline float pointLightInverseRange2(float range)
{
	const float r = std::max(range, 0.01f);
	return 1.0f / (r * r);
}

// What one Point light adds to a terrain pixel (terrain.frag; the
// original's calcPVPointTerrain x 0.5 in an additive pass): its colour x
// N.L x saturate(1 - d^2 / R^2) x 0.5, not times the terrain's colour.
inline float terrainPointLight(const float toLight[3], float inverseRange2, const float normal[3])
{
	const float d2 = toLight[0] * toLight[0] + toLight[1] * toLight[1] + toLight[2] * toLight[2];
	const float attenuation = std::clamp(1.0f - d2 * inverseRange2, 0.0f, 1.0f);
	const float d = std::sqrt(d2);
	const float nl = d > 0 ? std::max((toLight[0] * normal[0] + toLight[1] * normal[1] + toLight[2] * normal[2]) / d, 0.0f)
						   : 0.0f;
	return nl * attenuation * 0.5f;
}

// The undergrowth's per-patch pick (0x1010e270): the nearest 4 Point lights
// within light radius + the patch sphere's radius, nearest first.
constexpr int kUndergrowthPatchLights = 4;

// The nearest `max` (at most 16) of `lights` that `accept` takes and whose
// sphere meets the one at `origin` with `radius`, nearest first by the
// squared distance to `origin` (0x100a9f80: 0x100ad8c0, 0x100ad040), into
// `out` (indices); returns how many.
template<class Accept>
int pickNearestLights(const ListedLight* lights, size_t count, const float origin[3], float radius, int max,
	Accept accept, int* out, bool byReach = false)
{
	max = std::min(max, 16);
	if (max <= 0) {
		return 0;
	}
	float best[16];
	int n = 0;
	for (size_t i = 0; i < count; ++i) {
		const ListedLight& l = lights[i];
		if (!accept(l)) {
			continue;
		}
		const float dx = l.position[0] - origin[0], dy = l.position[1] - origin[1], dz = l.position[2] - origin[2];
		const float d2 = dx * dx + dy * dy + dz * dz;
		const float reach = l.radius + radius;
		if (d2 >= reach * reach) {
			continue;
		}
		// Not retail (enhanced.lightRank): the rank is d^2 / R^2, so a 1 m flash
		// far from the origin ranks behind a 22 m light that reaches it.
		const float key = byReach ? d2 / std::max(l.radius * l.radius, 1e-4f) : d2;
		// Insert by distance, keeping the nearest `max`.
		int at = n;
		while (at > 0 && best[at - 1] > key) {
			--at;
		}
		if (at >= max) {
			continue;
		}
		for (int j = std::min(n, max - 1); j > at; --j) {
			best[j] = best[j - 1];
			out[j] = out[j - 1];
		}
		best[at] = key;
		out[at] = static_cast<int>(i);
		n = std::min(n + 1, max);
	}
	return n;
}

// Picks at most kind's maximum of `lights` (count of them) for a mesh at
// `origin` with `meshRadius`, nearest first, into `out` (indices); returns
// how many.
inline int pickMeshLights(const ListedLight* lights, size_t count, const float origin[3], float meshRadius,
	LightedMesh kind, bool lightmapped, const LightLimits& limits, int* out, int capacity, bool byReach = false)
{
	return pickNearestLights(lights, count, origin, meshRadius, std::min(limits.maxFor(kind), capacity),
		[kind, lightmapped](const ListedLight& l) {
			if (kind != LightedMesh::Static && (l.spot || !l.supportsEmitter)) {
				return false; // Point only, supportsEmitter (0x8b09)
			}
			return !(lightmapped && !l.affectsLightmapped); // 0x11b09
		},
		out, byReach);
}

// A Titan-interior static's pick (0x100bd0a0 -> slot 0x24, 0x100a81f0 ->
// 0x100aa0d0, when the renderable's owner +0x324, slot 0xa0, is set): from
// every registered light (the manager's map +0x120, not the frustum's list
// +0x138, so no camera test), those within reach that pass 0x11b09, nearest
// first by d^2 to the origin (0x100ad8c0), then only the ones whose parent
// (+0x34) is the owner, cut to maxLightsPerStaticMesh (0x100ad040). A
// corridor piece is lit by its own LightSource children alone.
inline int pickOwnedLights(const ListedLight* lights, size_t count, int owner, const float origin[3], float radius,
	int max, int* out)
{
	return pickNearestLights(lights, count, origin, radius, max,
		[owner](const ListedLight& l) { return owner >= 0 && l.owner == owner && l.affectsLightmapped; }, out);
}

// The undergrowth patch's pick (0x1010e270): filter 0xb09, at most 4.
inline int pickUndergrowthLights(const ListedLight* lights, size_t count, const float centre[3], float radius,
	int* out)
{
	return pickNearestLights(lights, count, centre, radius, kUndergrowthPatchLights, terrainTakesLight, out);
}

} // namespace dice::hfe::render
