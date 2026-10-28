// RendDX9 settings for the post-production, the dynamic shadows and the
// hemispheric ambient: what the console sets ("renderer.bloom*",
// "renderer.default*", "renderer.emp*", "shadowManager.*",
// "HemiMapManager.*", "VideoSettings.setDynamicShadowsQuality") and the
// renderer reads each frame.
#pragma once

#include <array>
#include <string>

namespace dice::hfe {
class Console;
}

namespace dice::hfe::scene {

// The post-production object (RendDX9 0x1004a590 constructs it; the
// console's setters 0x1004fd20..0x100516e0 write it). Every level's Sky.con
// sets the bloom and the colour grade.
struct PostProductionSettings {
	bool drawPostProduction = true; // renderer.drawPostProduction (renderer +0x6a5)
	bool useBloom = true;           // VideoSettings.setUseBloom (the menu's BloomEnabled)
	// The bloom (FSQuadDrawer's contrast and large-bloom filters, then
	// PostProduction's Soften): +0x104, +0x108, +0x10c, +0x110, +0x114.
	float bloomFilterContrast = 1;
	float bloomSize = 1;
	float bloomWeightScale = 1;
	float bloomOverlayAmount = 0.2f;
	bool showBloomOverlayTexture = false;
	// The colour grade (ColorTransform): +0x1b8 tint, +0x1c4 saturation,
	// +0x1c8 the contrast polynomial a x + b x^2 + c.
	std::array<float, 3> defaultColorTint {1, 1, 1};
	float defaultSaturation = 1;
	std::array<float, 3> defaultContrast {1, 0, 0};
	// The EMP static (renderer.emp*: +0xf4 noise, +0xf8 roll, +0xfc warp
	// amount, +0x100 warp frequency; empEffectLevel loads them from the
	// level table, 0x10048a30). empEnableSimpleShader is +0xf0.
	int empEffectLevel = 0;
	bool empEnableSimpleShader = false;
	float empStaticNoiseAmount = 0;
	float empVerticalRoll = 0;
	float empWarpAmount = 0;
	float empWarpFrequency = 0;
	// The active camouflage's refraction (FSQuadDrawer CAMOUFLAGEOFFSET).
	std::array<float, 2> camouflageOffset {0.01f, 0.01f};
	// The glow materials (GlowMaterial, Glow): renderer.glowEnabled, glowStrength.
	bool glowEnabled = false;
	float glowStrength = 1;
	// renderer.colorBrightness (stored; RendDX9 hands it to the gamma ramp).
	float colorBrightness = 1;
};

// The dynamic shadows: the light manager's shadow settings (its
// constructor 0x100aafe0, setters at vtable 0x101d42e0 slots 41..94, the
// shadowManager.* console names 0x100802eb..0x100821b3).
struct ShadowSettings {
	int shadowMapSize = 1024;      // +0x168
	int shadowMapObjectSize = 512; // +0x16c (at most the map's)
	int shadowMapBitDepth = 24;    // +0x170 (D3DFMT_D24S8 0x4d; 16 0x50, 32 0x72)
	float shadowMapDepthDistance = 40; // +0x178
	bool shadowMapIncludeStaticMeshes = true; // +0x1cc
	bool shadowShowFrustums = false;           // +0x6e4
	float shadowIntensityBias = 0;  // +0x1d0 (SHADOWINTENSITYBIAS)
	float lightmapIntensityBias = 0; // +0x1d4 (LIGHTMAPINTENSITYBIAS)
	float dropShadowClipHeightOffset = 0; // +0x1d8
	bool dynamicShadowsEnabled = true;           // +0x748
	bool dynamic1pShadowsEnabled = true;         // +0x749
	bool dynamicStaticMeshShadowsEnabled = true; // +0x74a
	bool allowAlphaShadows = false;  // +0x74b
	float alphaShadowThreshold = 0.2f; // +0x74c (SHADOWALPHATHRESHOLD)
	float shadowUpdateFactor = 1;    // +0x750
	bool shadowUpdatesEnabled = true; // +0x754
	float shadowMapDepthBias = 0;    // +0x760
	float shadowMapSlopeBias = 0;    // +0x764
	float shadowOccluderOversizeFactor = 1.5f; // +0x768
	bool useLSSMOptimizations = true;  // +0x755
	bool usePSMOptimizations = false;  // +0x75c
	bool shadowPSMUseSlideback = false; // +0x75d
	bool shadowPSMUseUnitCubeClipping = true; // +0x75e
	float shadowPSMZNearOffset = 0;
	float shadowPSMZFarOffset = 0;
	float shadowPSMZMinInfinityZ = 1.5f;
	float shadowPSMFovY = 60;          // +0x76c
	float shadowTSMFocus = 0.52f;      // +0x770
	float shadow80Line = -0.6f;        // +0x774
	bool shadowMergeDynamicShadowFrustums = true; // +0x778
	// VideoSettings.setDynamicShadowsQuality: 0 off, 1 low, 2 medium, 3 high
	// (the menu's default 2).
	int quality = 2;
	// undergrowth.dynamicShadowsEnable (RendDX9 0x100861d8): the
	// undergrowth receives them.
	bool undergrowthShadows = true;
};

// The hemispheric ambient (HemiMapManager.*, RendDX9 0x10085535..0x10085e3c):
// a texture over the level, the ground's bounced colour in rgb and the
// sun's occlusion in alpha (roofs, bridges), looked up under each dynamic
// object (RaShaderBM.fx / RaShaderSM.fx calcGroundUVAndLerp).
struct HemiMapSettings {
	std::string baseHemiMap;           // without ".dds"
	std::array<float, 3> offset {};    // the map's corner, world x/y/z
	float size = 0;                    // metres across (HemiMapConstants.z)
	float height = 0;                  // its fourth argument (the heights it covers)
	float hemiLerpBias = 1;
	bool useCurrentSetting = false;
	struct TitanHemiMap {
		std::string path;
		std::array<float, 3> position {};
		float size = 0;
		int team = 0;
	};
	std::array<TitanHemiMap, 3> titans; // HemiMapManager.loadTitanHemiMap, by team
};

void registerPostProductionConsoleMethods(Console& console, PostProductionSettings& settings);
void registerShadowConsoleMethods(Console& console, ShadowSettings& settings);
void registerHemiMapConsoleMethods(Console& console, HemiMapSettings& settings);

} // namespace dice::hfe::scene
