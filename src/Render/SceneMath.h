// The renderer's GPU-free arithmetic for the dynamic shadows, the hemi
// maps, the sky's clouds and the normal maps (header-only, for the tests;
// LevelRenderer.cpp and LevelRendererPost.cpp use them).
#pragma once

#include "RenderMath.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace dice::hfe::render {

// ---- the dynamic shadows ----------------------------------------------------

// The shadow region's half side by quality (VideoSettings 1 low, 2 medium,
// 3 high) over shadowManager.shadowMapDepthDistance, at least 10 m.
inline float shadowRegionRadius(int quality, float depthDistance)
{
	const float scale = quality >= 3 ? 2.0f : quality == 2 ? 1.5f : 1.0f;
	return std::max(depthDistance, 10.0f) * scale;
}

// The shadow map's texels across: shadowManager.shadowMapSize, doubled at
// the high quality, within 256..4096.
inline uint32_t shadowMapTexels(int quality, int mapSize)
{
	const int size = quality >= 3 ? mapSize * 2 : mapSize;
	return static_cast<uint32_t>(std::clamp(size, 256, 4096));
}

// The region's centre: half the region ahead of the camera along its
// heading, snapped to a quarter of the region (half the radius) so the
// projection holds still while the camera moves within a step; its height
// the camera's, snapped the same.
inline Vec3f shadowRegionCenter(const Vec3f& camera, const Vec3f& forward, float radius)
{
	const float flat = std::sqrt(forward.x * forward.x + forward.z * forward.z);
	const Vec3f ahead = flat > 1e-6f ? Vec3f {forward.x / flat, 0, forward.z / flat} : Vec3f {0, 0, 1};
	const Vec3f wanted = camera + ahead * (radius * 0.5f);
	const float step = radius * 0.5f;
	return {std::round(wanted.x / step) * step, std::round(camera.y / step) * step, std::round(wanted.z / step) * step};
}

// ---- the hemi maps -----------------------------------------------------------

// A BC3 (DXT5) block's 16 alpha values, row by row.
inline void decodeBc3Alpha(const uint8_t block[8], uint8_t out[16])
{
	uint8_t table[8] = {block[0], block[1]};
	if (block[0] > block[1]) {
		for (int i = 1; i < 7; ++i) {
			table[i + 1] = static_cast<uint8_t>(((7 - i) * block[0] + i * block[1]) / 7);
		}
	} else {
		for (int i = 1; i < 5; ++i) {
			table[i + 1] = static_cast<uint8_t>(((5 - i) * block[0] + i * block[1]) / 5);
		}
		table[6] = 0;
		table[7] = 255;
	}
	uint64_t bits = 0;
	for (int i = 0; i < 6; ++i) {
		bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
	}
	for (int p = 0; p < 16; ++p) {
		out[p] = table[(bits >> (3 * p)) & 7];
	}
}

// The base hemi map's texel over a world point (Particles.fx vsParticle's
// HemiMapInfo: uv = ((p + size / 2).xz - corner.xz) / size, v flipped), of a
// map `texels` across, clamped to it.
inline void hemiMapTexel(float x, float z, float cornerX, float cornerZ, float size, int texels, int& px, int& py)
{
	const float u = (x + size / 2 - cornerX) / size;
	const float v = 1 - (z + size / 2 - cornerZ) / size;
	px = std::clamp(static_cast<int>(u * static_cast<float>(texels)), 0, texels - 1);
	py = std::clamp(static_cast<int>(v * static_cast<float>(texels)), 0, texels - 1);
}

// A point in a Titan hemi map's space: from the Titan's origin along its
// axes (its rotation's rows) less the map's offset (0x100eda70).
inline Vec3f titanMapLocal(const Vec3f& origin, const Vec3f axes[3], const float offset[3], const Vec3f& point)
{
	const Vec3f d = point - origin;
	return {dot(d, axes[0]) - offset[0], dot(d, axes[1]) - offset[1], dot(d, axes[2]) - offset[2]};
}

// Whether a map-space point is over the map (above its floor, within its
// square; 0x100edce0).
inline bool overTitanMap(const Vec3f& local, float size)
{
	return local.y > 0 && std::fabs(local.x) < size / 2 && std::fabs(local.z) < size / 2;
}

// A Titan hemi map's colour at a map-space point, bilinear, 1 texel a metre
// (0x100ecb10: u = size / 2 - x, v = size / 2 + z, scaled to the texels,
// clamped one short of the edge); `rgba` the top level, RGBA8.
inline void titanHemiSample(const uint8_t* rgba, uint32_t width, uint32_t height, float size, const Vec3f& local,
	float out[3])
{
	const float scaleU = static_cast<float>(width) / size;
	const float scaleV = static_cast<float>(height) / size;
	const float u = std::clamp((size / 2 - local.x) * scaleU, 0.0f, static_cast<float>(width - 2));
	const float v = std::clamp((size / 2 + local.z) * scaleV, 0.0f, static_cast<float>(height - 2));
	const uint32_t x = static_cast<uint32_t>(u), y = static_cast<uint32_t>(v);
	const float fx = u - static_cast<float>(x), fy = v - static_cast<float>(y);
	for (int c = 0; c < 3; ++c) {
		const auto at = [&](uint32_t tx, uint32_t ty) {
			return static_cast<float>(rgba[(static_cast<size_t>(ty) * width + tx) * 4 + static_cast<size_t>(c)]) / 255.0f;
		};
		const float top = at(x, y) * (1 - fx) + at(x + 1, y) * fx;
		const float bottom = at(x, y + 1) * (1 - fx) + at(x + 1, y + 1) * fx;
		out[c] = top * (1 - fy) + bottom * fy;
	}
}

// HeightOverTerrain (BM 0x100c2b70, SM 0x10160e90): the height over what a
// ray 4 radii down meets (`below`, negative or past 4 radii for nothing),
// less a radius, in radii (at least 2 m).
inline float heightOverTerrain(float below, float radius)
{
	radius = radius > 0 ? radius : 1.0f;
	const float height = below >= 0 && below < 4 * radius ? below : 4 * radius;
	return std::max(height - radius, 0.0f) / std::max(radius, 2.0f);
}

// ---- the sky -----------------------------------------------------------------

// A cloud layer's texture offset after `seconds` (TEXOFFSET, 0x100fd2f0's
// +0x130 / +0x138: scrollDirection a second, kept within one tile).
inline float cloudOffset(float offset, float scroll, float seconds)
{
	return std::fmod(offset + scroll * seconds, 1.0f);
}

// ---- the normal maps ---------------------------------------------------------

// Which normal map a bundled or skinned material's second map is
// (RaShaderBM/SM _HASNORMALMAP_): 1 tangent space, 2 object space
// (_OBJSPACENORMALMAP_), 0 none. A bundled mesh's is the second map ending
// "_b.dds" (+0x35, 0x101499b0); a skinned mesh's is its second map unless
// the SpecularLUT, in tangent space when the technique names "tangent"
// (+0x100, 0x100d4160). `secondMap` and `technique` in lower case.
inline int normalMapKind(bool bundled, bool skinned, const std::string& secondMap, const std::string& technique)
{
	if (bundled) {
		const bool bump = secondMap.size() >= 6 && secondMap.compare(secondMap.size() - 6, 6, "_b.dds") == 0;
		return bump ? 1 : 0;
	}
	if (skinned) {
		if (secondMap.empty() || secondMap.find("specularlut") != std::string::npos) {
			return 0;
		}
		return technique.find("tangent") != std::string::npos ? 1 : 2;
	}
	return 0;
}

} // namespace dice::hfe::render
