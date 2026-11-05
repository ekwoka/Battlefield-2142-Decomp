// The light manager's level-wide sun occlusion (RendDX9's SimpleShadowmap,
// header-only for the tests; LevelRenderer uses it).
//
// Every level's server.zip holds SimpleShadowmap.raw and
// SimpleShadowmapIO.raw: N x N bytes, N half the heightmap cluster's
// heightmapSize (Cerbere_Landing: 1024 -> 512), a texel 2 m, the map
// centred on the world's origin, its first row the far (+z) edge. The
// lightmapper baked them: the sun's occlusion by the terrain and the static
// meshes (the hills' and the buildings' shade), and whether a spot is
// outdoors. The light manager loads them at the level's load (0x100aa290,
// its vtable 0x101d42e0 slot 155):
//   - the occlusion in 2-bit steps, q = floor(v x 3/255 + 0.5), four to a
//     byte (0x100a8f40), read back as q / 3 (0x100a84c0);
//   - the IO map as one bit a texel, set for a byte over 0x7f (outdoors;
//     0x100a9080 reads 1 or 0);
// each in a grid with a border (4 texels left and right, 1 above and below)
// that starts at 1 (unoccluded, outdoors). A missing file leaves its grid
// so. The lookups (slots 153 / 154, 0x100a8cb0 / 0x100a9160) clamp x and z
// to +-N, take fx = x / 2 - 0.5 and blend the four texels around bilinearly.
//
// The bundled meshes' draw (0x100c3880, the Titan interiors' 0x100c5030)
// and the skinned meshes' (0x10162070) scale the sun's colour and its
// specular colour by sunScale() at the object's origin (+0x30, +0x38 of its
// world matrix): see there.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dice::hfe::render {

class SimpleShadowmap {
public:
	// `n` texels a side (half the cluster's heightmapSize); `occlusion` and
	// `io` the two files' bytes (either may be empty: not found).
	void load(int n, const std::vector<uint8_t>& occlusion, const std::vector<uint8_t>& io)
	{
		m_n = std::max(n, 0);
		m_stride = m_n + 8;
		m_rows = m_n + 2;
		m_occlusion.assign(static_cast<size_t>(m_stride) * static_cast<size_t>(m_rows), 3);
		m_outdoors.assign(m_occlusion.size(), 1);
		m_loaded = m_n > 0 && occlusion.size() >= static_cast<size_t>(m_n) * static_cast<size_t>(m_n);
		if (!m_loaded) {
			return;
		}
		const bool withIO = io.size() >= static_cast<size_t>(m_n) * static_cast<size_t>(m_n);
		// The file's last row goes to the grid's row 1, its first to row N.
		for (int row = 1; row <= m_n; ++row) {
			const size_t from = static_cast<size_t>(m_n - row) * static_cast<size_t>(m_n);
			for (int i = 0; i < m_n; ++i) {
				const size_t to = static_cast<size_t>(row) * static_cast<size_t>(m_stride) + static_cast<size_t>(i + 4);
				m_occlusion[to] = quantize(occlusion[from + static_cast<size_t>(i)]);
				if (withIO) {
					m_outdoors[to] = io[from + static_cast<size_t>(i)] > 0x7f ? 1 : 0;
				}
			}
		}
	}

	bool loaded() const { return m_loaded; }
	int size() const { return m_n; }

	// 0x100a8f40: the byte in 2-bit steps.
	static uint8_t quantize(uint8_t value)
	{
		return static_cast<uint8_t>(std::floor(static_cast<float>(value) * 0.011764706f + 0.5f)) & 3;
	}

	// The sun's share at world x, z (0x100a8cb0): 0, 1/3, 2/3 or 1 blended.
	float occlusionAt(float x, float z) const
	{
		return bilinear(x, z, [this](int tx, int tz) { return static_cast<float>(texel(m_occlusion, tx, tz)) * 0.33333334f; });
	}

	// Outdoors (1) or indoors (0) at world x, z (0x100a9160), blended.
	float outdoorsAt(float x, float z) const
	{
		return bilinear(x, z, [this](int tx, int tz) { return static_cast<float>(texel(m_outdoors, tx, tz)); });
	}

private:
	// Texel (tx, tz) about the origin: the grid's column tx + (N + 8) / 2
	// (0x100a84c0's +0x110 bytes of four), row tz + (N + 2) / 2 (+0x114).
	uint8_t texel(const std::vector<uint8_t>& grid, int tx, int tz) const
	{
		if (m_n <= 0) {
			return 1; // no map: 1 (0x100a84c0 / 0x100a9080 with nothing loaded)
		}
		const int col = tx + (m_stride / 4 / 2) * 4;
		const int row = tz + m_rows / 2;
		if (col < 0 || col >= m_stride || row < 0 || row >= m_rows) {
			return &grid == &m_occlusion ? 3 : 1;
		}
		return grid[static_cast<size_t>(row) * static_cast<size_t>(m_stride) + static_cast<size_t>(col)];
	}

	template <class Texel>
	float bilinear(float x, float z, Texel at) const
	{
		if (m_n <= 0) {
			return 1;
		}
		const float limit = static_cast<float>(m_n);
		x = std::clamp(x, -limit, limit);
		z = std::clamp(z, -limit, limit);
		const float fx = x * 0.5f - 0.5f, fz = z * 0.5f - 0.5f;
		const float x0 = std::floor(fx), z0 = std::floor(fz);
		const int ix = static_cast<int>(x0), iz = static_cast<int>(z0);
		const float tx = fx - x0, tz = fz - z0;
		const float nearRow = tx * at(ix + 1, iz) + (1 - tx) * at(ix, iz);
		const float farRow = tx * at(ix + 1, iz + 1) + (1 - tx) * at(ix, iz + 1);
		return farRow * tz + (1 - tz) * nearRow;
	}

	int m_n = 0;
	int m_stride = 0;
	int m_rows = 0;
	bool m_loaded = false;
	std::vector<uint8_t> m_occlusion; // 0..3 a texel
	std::vector<uint8_t> m_outdoors;  // 0 or 1 a texel
};

// The light manager's settings for it (the constructor 0x100aafe0; no
// console row sets them): the exponent (+0xcc, slot 0x248), the heights
// over the ground across which the map fades out (+0xd8 / +0xdc, slot
// 0x260) and the upward ray's length (+0xc8, slot 0x240).
constexpr float kSimpleShadowExponent = 1.0f;
constexpr float kSimpleShadowFadeLow = 3.0f;
constexpr float kSimpleShadowFadeHigh = 6.0f;
constexpr float kSimpleShadowUpRay = 150.0f;

// The factor on a mesh's sun and specular colour (bundled 0x100c3880 /
// 0x100c5030, skinned 0x10162070). `occlusion` and `outdoors` are the maps
// at the object's origin, `upHit` whether a ray kUpRay up from it met
// anything (+600 / +0x54, 0x100c2b70's third ray), `height` its height over
// what a ray kFadeHigh down meets (kFadeHigh when nothing: +0x254 / +0x50).
//   - Nothing above: outdoors, the map faded to 1 by the height across
//     kFadeLow..kFadeHigh; indoors 1.
//   - Something above: indoors the map, outdoors 1.
// A skinned mesh with renderer.useSunScaling (device +0x41, slot 0xbc) is
// taken as indoors in both. Then pow(f, kExponent).
inline float simpleShadowSunScale(float occlusion, float outdoors, bool upHit, float height,
	bool useSunScaling = false)
{
	const bool indoors = outdoors < 0.5f;
	float f = 1;
	if (!upHit) {
		if (!indoors || useSunScaling) {
			const float t = std::clamp((height - kSimpleShadowFadeLow) / (kSimpleShadowFadeHigh - kSimpleShadowFadeLow), 0.0f, 1.0f);
			f = (1 - t) * occlusion + t;
		}
	} else if (indoors || useSunScaling) {
		f = occlusion;
	}
	return std::pow(f, kSimpleShadowExponent);
}

// The downward ray's answer from a height over the terrain (negative: the
// terrain is above the origin, which a downward ray doesn't meet).
inline float simpleShadowHeight(float overTerrain)
{
	return overTerrain >= 0 && overTerrain <= kSimpleShadowFadeHigh ? overTerrain : kSimpleShadowFadeHigh;
}

} // namespace dice::hfe::render
