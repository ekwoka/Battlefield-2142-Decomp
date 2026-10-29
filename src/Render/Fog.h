// The fog the view is in (GPU-free, for the tests).
//
// RendDX9's fog object (DAT_101fb808, constructed by 0x1004a590) keeps two
// fogs: the level's (renderer.fogColor / fogStartEndAndBase) and the
// water's (renderer.waterFogColor / waterFogStartEndAndBase). Each frame
// the renderer asks it for the colour (0x100497a0) and the start, end and
// base (0x10049850): with the camera at or below the heightmap cluster's
// sea level (slot 0x60; -1000 without a cluster) the water's, else the
// level's. The sky dome then draws its SkyDomeUnderWater technique, the
// water's fog colour (0x100fd2f0: below the sea; the dome at exactly sea
// level stays the sky).
//
// The end and the fourth value scale by the view distance, VideoSettings'
// setViewDistanceScale clamped to 0.5..1 (0x10028020 reads the settings
// object's +0x108 that slot 0x28, 0x1000f0c0, keeps) before the fog's
// constants are made (0x100355c0: RaCommon.fx calcFog's FogRange).
#pragma once

#include <algorithm>
#include <array>

namespace dice::hfe::render {

struct ViewFog {
	std::array<float, 3> color {0.5f, 0.5f, 0.5f}; // 0..1
	std::array<float, 4> startEndAndBase {0, 1400, 1, 0}; // the end and w scaled
	bool underwater = false;
	bool underwaterSky = false; // the sky dome draws as the water's fog colour
};

// `fogColor` / `waterFogColor` as the scripts give them (0..255);
// `hasSea` false: never under water.
inline ViewFog viewFog(const std::array<float, 3>& fogColor, const std::array<float, 4>& fogStartEndAndBase,
	const std::array<float, 3>& waterFogColor, const std::array<float, 4>& waterFogStartEndAndBase, bool hasSea,
	float seaLevel, float cameraY, float viewDistanceScale)
{
	ViewFog fog;
	const float water = hasSea ? seaLevel : -1000.0f;
	fog.underwater = cameraY <= water;
	fog.underwaterSky = cameraY < water;
	const auto& color = fog.underwater ? waterFogColor : fogColor;
	const auto& range = fog.underwater ? waterFogStartEndAndBase : fogStartEndAndBase;
	for (int i = 0; i < 3; ++i) {
		fog.color[static_cast<size_t>(i)] = color[static_cast<size_t>(i)] / 255.0f;
	}
	const float scale = std::clamp(viewDistanceScale, 0.5f, 1.0f);
	fog.startEndAndBase = {range[0], range[1] * scale, range[2], range[3] * scale};
	return fog;
}

// RaCommon.fx's FogRange as 0x100355c0 makes it from the start, end and
// base: (1 / (e - s), -base / (e - s), -s / (e - s), e / (e - s)). The
// shaders evaluate it at their clip-space w (calcFog(Out.Pos.w)): the view
// depth in metres, the same units the scripts give (Cerbere Landing fogs
// out at 140 m, its GameLogic.MaximumLevelViewDistance; the exe's AI view
// distance, BF2142.exe 0x008959f0, reads the same values as metres).
inline std::array<float, 4> fogRange(const std::array<float, 4>& startEndAndBase)
{
	const float start = startEndAndBase[0], end = startEndAndBase[1];
	const float inverse = 1.0f / std::max(end - start, 1e-3f);
	return {inverse, -startEndAndBase[2] * inverse, -start * inverse, end * inverse};
}

// RaCommon.fx calcFog: how much of the surface shows at view depth `w`
// (1 none of the fog, 0 all of it) from FogRange and FogColor.w (the fog's
// fourth value): max((e - base w) / (e - s), w4) - ((w - s) / (e - s))^3,
// saturated as the fog factor is (fog.glsl).
inline float fogVisibility(const std::array<float, 4>& startEndAndBase, float w)
{
	const auto range = fogRange(startEndAndBase);
	const float far = std::max(w * range[0] + range[2], 0.0f);
	const float close = std::max(w * range[1] + range[3], startEndAndBase[3]);
	return std::clamp(close - far * far * far, 0.0f, 1.0f);
}

} // namespace dice::hfe::render
