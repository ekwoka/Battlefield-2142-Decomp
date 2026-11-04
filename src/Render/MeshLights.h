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
// passes add one light each (RaShaderSTM / SM / BM "_pointlight_").
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
};

// Picks at most kind's maximum of `lights` (count of them) for a mesh at
// `origin` with `meshRadius`, nearest first, into `out` (indices); returns
// how many.
inline int pickMeshLights(const ListedLight* lights, size_t count, const float origin[3], float meshRadius,
	LightedMesh kind, bool lightmapped, const LightLimits& limits, int* out, int capacity)
{
	const int max = std::min(limits.maxFor(kind), capacity);
	if (max <= 0) {
		return 0;
	}
	float best[16];
	int n = 0;
	for (size_t i = 0; i < count; ++i) {
		const ListedLight& l = lights[i];
		if (l.spot && kind != LightedMesh::Static) {
			continue; // Point only (0x8b09)
		}
		if (lightmapped && !l.affectsLightmapped) {
			continue; // 0x11b09
		}
		const float dx = l.position[0] - origin[0], dy = l.position[1] - origin[1], dz = l.position[2] - origin[2];
		const float d2 = dx * dx + dy * dy + dz * dz;
		const float reach = l.radius + meshRadius;
		if (d2 >= reach * reach) {
			continue;
		}
		// Insert by distance, keeping the nearest `max`.
		int at = n;
		while (at > 0 && best[at - 1] > d2) {
			--at;
		}
		if (at >= max) {
			continue;
		}
		const int last = std::min(n, max - 1);
		for (int j = last; j > at; --j) {
			best[j] = best[j - 1];
			out[j] = out[j - 1];
		}
		best[at] = d2;
		out[at] = static_cast<int>(i);
		n = std::min(n + 1, max);
	}
	return n;
}

} // namespace dice::hfe::render
