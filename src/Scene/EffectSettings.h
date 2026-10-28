// The effect-side settings the renderer and the particle systems read:
// RendDX9's ParticleSystemManager ("ParticleSystemManager.*", 0x1007aa09..
// 0x1007bcb7; the object DAT_101ffc70, constructed by 0x1009a8c0), the
// game's WindManager ("windManager.*", BF2142.exe 0x006ebec1 / 0x006ebfc8;
// the object 0x00704400, vtable 0x0099ad60) and the video quality levels
// ("VideoSettings.set*Quality", BF2142.exe 0x0041236a..0x00412ea3) that
// RendDX9's quality switch (0x10010040) applies to the renderer's objects.
#pragma once

#include <array>
#include <string>

namespace dice::hfe {
class Console;
}

namespace dice::hfe::scene {

// The WindManager (0x00704400): a global wind of globalWindSpeed (+0x2c,
// default 0) along globalWindDirection (+0x30, default 0/0/1); slot 0x3c
// (0x00703b20) gives the wind at a point as speed x direction plus the
// local winds of the WindAffector components near it (slot 0x18,
// 0x007040d0: those whose radius reaches the point), slot 0x30
// (0x007042c0) its length. Five shipped levels set a global wind (Aegis
// Station and Operation Nightshift 10, Gulf of Oman and Arctic Crossing
// 5, The Hell 2). RendDX9 updates it each frame (0x10028bb0: slot 0x10).
struct WindSettings {
	float globalWindSpeed = 0;
	std::array<float, 3> globalWindDirection {0, 0, 1};
	// The wind at a point: the global wind (the local WindAffector winds
	// are not modelled: see engine.md).
	std::array<float, 3> windAt() const
	{
		return {globalWindDirection[0] * globalWindSpeed, globalWindDirection[1] * globalWindSpeed,
			globalWindDirection[2] * globalWindSpeed};
	}
	float speedAt() const;
};

// The particle systems' manager (0x1009a8c0's defaults; the effects
// quality row, 0x100991b0, overwrites most of them).
struct ParticleSystemSettings {
	bool drawStats = false;
	bool drawBoundingBoxEnable = false;
	// The systems' view distance (+0x8ca4): GSViewDistance (500) x the view
	// distance scale x the quality row's scale (+0x8cac), at least 200
	// (0x100991b0).
	float viewDistance = 500;
	// The particles' own fade (draw time, 0x10132e90 and the like): each
	// LodDistance class (Low, Medium, High: +0x8bb0..+0x8bb8 = 0.3, 0.6, 1 of
	// the view distance) is drawn to its distance, a particle's alpha times
	// (1 - min((d - s) / (end - s), 1))^2 past s = startLodPercentage (0.85,
	// +0x8bfc) of it, d its depth in the view.
	float startLodPercentageOfLodDistance = 0.85f;
	// A system's emission scale (+0x134, 0x101393e0): (1 - f)^2 + its size
	// on screen, at most 1 and at least the template's minimum (0.05), with
	// f = saturate((dist - p end) / ((1 - p) end)), p = startFadePercentage
	// (0.7, +0x8be0).
	float startFadePercentageOfViewDistance = 0.7f;
	std::array<float, 3> lodFractions {0.3f, 0.6f, 1.0f};
	// The emission throttle (0x101403b0): once the pool is more than
	// poolLoad (0.75, +0x8c0c) full, an emitter's frequency is scaled by its
	// emitPrio's (Low, Medium, High) factor (+0x8c00..+0x8c08).
	std::array<float, 3> priorityScales {0.4f, 0.55f, 0.7f};
	float poolLoad = 0.75f;
	// The sprite particles' pool (+0x5f00, the row's [1]): emission stops
	// when it is empty.
	int maxParticles = 1000;
	bool affectedByWind = true;  // +0x8c14
	// The particle shader's technique (+0x8cb0): 0 the overdraw view, 1 Low
	// (unlit), 2 Medium (lit, no hemi map), 3 High (Particles.fx
	// psParticleHigh: lit by the hemi map's shadow).
	int particleSystemShaderQuality = 3;
	float overdrawScale = 10;    // +0x8ca8
	// A system whose template's QualityLevel (Low 0, Medium 1, High 2) is
	// above this does not start (0x1013fb30; +0x8cb4).
	int qualityLevel = 2;
	// The heat shimmer (Particles.fx / NonScreenAlignedParticles.fx /
	// Trail.fx ParticleHeatShimmer; the sprite manager's +0x1dcc..+0x1dd4):
	// FracTime = frac(t x speed) (0x10132de0).
	float shimmerIntensity = 0.75f, shimmerPhases = 20, shimmerSpeed = 10;
	float nsaShimmerIntensity = 0.75f, nsaShimmerPhases = 20, nsaShimmerSpeed = 10;
	float trailShimmerIntensity = 0.75f, trailShimmerPhases = 20, trailShimmerSpeed = 10;
};

// VideoSettings' levels (the menu's Video page; SettingsObjects keeps
// them in Video.con) as RendDX9's settings object holds them (+0x30
// terrain .. +0x4c filtering) and its switch (0x10010040) applies them:
//
//   terrain   1/2/3: the terrain culler's lodSwitchDistance 64/80/100
//             (0x1009c9b0), undergrowth.simpleShaderEnable 1/1/0 and
//             undergrowth.viewDistanceScale 0.5/0.75/1 (slots 0x40, 0x68),
//             renderer +0x693 0/1/1, the road renderer's slot 0x18 and
//             the sky dome's slot 0x94 (0/1/2).
//   geometry  1/2/3: renderer +0x974 (the mesh LOD distances' factor)
//             0.5/0.8/1, +0x910 (maxSkip3pLods) 1/0/0, +0x6a8/+0x6ac/
//             +0x6b0 6/6/40, 7/8/60, 8/10/80, Overgrowth +0x110
//             (viewDistanceScale) 0.4/0.7/1.
//   lighting  1/2/3: renderer +0x6b4 3/1/0, DAT_101fa9ac +0x40 1/0/0.
//   dynamic shadows 0..3: the light manager's slots 0x6c, 0x64, 0x74 and
//             the shadow distance 0/50/100/200 (slot 0x5c).
//   dynamic lighting 0..3: the light manager's slot 0xf4 (0 off, else 1).
//   effects   0..4: the particle manager's row DAT_101f45a8 + 0x3c x level
//             (0x100991b0: view-distance scale, pool sizes, priority
//             scales, LOD fractions, fade/LOD starts, shader quality,
//             quality level), DecalManager +0x322e = level > 1.
//   texture   1/2/3: GSSkipMipLevels 2/1/0 (the texture manager's mip
//             skips, at most 1 per texture class).
struct QualitySettings {
	int terrain = 3;
	int geometry = 3;
	int lighting = 3;
	int dynamicLighting = 2;
	int effects = 3;
	int texture = 3;
	int textureFiltering = 3;
	float viewDistanceScale = 1; // VideoSettings.setViewDistanceScale
	bool changed = true;         // set by the setters; the owner clears it once applied

	// What the levels give (0x10010040, 0x100991b0).
	int terrainLodSwitchDistance() const;
	float undergrowthViewDistanceScale() const;
	bool undergrowthSimpleShader() const;
	float geometryLodFactor() const;
	float overgrowthViewDistanceScale() const;
	int maxSkip3pLods() const;
	int skipMipLevels() const;
	bool dynamicLightsEnabled() const;
	bool decalsEnabled() const;
	// The effects row: applies it to the manager's settings.
	void applyEffects(ParticleSystemSettings& settings) const;
};

// GameLogic.useColdBreath (BF2142.exe 0x00427d38, the game logic's slots
// 0xd8 / 0xdc): soldiers show their breathEffect (0x00588830).
struct ColdBreathSettings {
	bool useColdBreath = false;
};

void registerWindConsoleMethods(Console& console, WindSettings& settings);
void registerColdBreathConsoleMethods(Console& console, ColdBreathSettings& settings);
void registerParticleSystemConsoleMethods(Console& console, ParticleSystemSettings& settings);
// VideoSettings.setTerrainQuality / setGeometryQuality / setLightingQuality /
// setDynamicLightingQuality / setEffectsQuality / setTextureQuality /
// setTextureFilteringQuality / setViewDistanceScale (setUseBloom and
// setDynamicShadowsQuality are the post-production's).
void registerQualityConsoleMethods(Console& console, QualitySettings& settings);

} // namespace dice::hfe::scene
