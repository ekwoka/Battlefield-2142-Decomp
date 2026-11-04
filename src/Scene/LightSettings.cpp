#include "LightSettings.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"

#include <algorithm>

namespace dice::hfe::scene {

namespace {

void bindVec3(Console& console, const char* name, std::array<float, 3>& value)
{
	bindProperty<ConsoleVec3>(console, name, ConsoleAccess::Anyone,
		[&value] { return ConsoleVec3 {value[0], value[1], value[2]}; },
		[&value](const ConsoleVec3& v) { value = {v.x, v.y, v.z}; });
}

} // namespace

void registerRendererConsoleMethods(Console& console, RendererSettings& settings)
{
	const uint32_t access = ConsoleAccess::Anyone;
	const auto bindFloat = [&console, access](const char* name, float& value) {
		bindProperty<float>(console, name, access, [&value] { return value; }, [&value](const float& v) { value = v; });
	};
	bindFloat("renderer.globalStaticMeshLodDistanceScale", settings.staticMeshLodDistanceScale); // 0x10051e80, 0x10051f70, 0x10051fd0
	bindFloat("renderer.globalBundleMeshLodDistanceScale", settings.bundleMeshLodDistanceScale); // 0x10052040, 0x10052130, 0x10052190
	bindFloat("renderer.globalSkinnedMeshLodDistanceScale", settings.skinnedMeshLodDistanceScale); // 0x100521e0, 0x100522d0, 0x10052330
	bindProperty<int>(console, "terrainCuller.lodSwitchDistance", access, // 0x10076bd7, 0x10076c48, 0x10076c95
		[&settings] { return settings.lodSwitchDistance; },
		[&settings](const int& v) {
			if (v >= settings.baseCellSize && v <= 1000) { // 0x1009c9b0
				settings.lodSwitchDistance = v;
				settings.lodSwitchDistanceSet = true;
			}
		});
	bindFloat("terrainCuller.lod0SwitchStart", settings.lod0SwitchStart); // 0x10076cd7, 0x10076d48, 0x10076d95
	bindFloat("terrainCuller.lod0SwitchStop", settings.lod0SwitchStop); // 0x10076dda, 0x10076e4b, 0x10076e98
	bindFloat("terrainCuller.lod1SwitchStart", settings.lod1SwitchStart); // 0x10076edd, 0x10076f4e, 0x10076f9b
	bindFloat("terrainCuller.lod1SwitchStop", settings.lod1SwitchStop); // 0x10076fe0, 0x10077051, 0x1007709e
	bindFloat("terrainCuller.lod2Start", settings.lod2Start); // 0x100770e3, 0x100771b1, 0x100771fe
	bindProperty<int>(console, "terrainCuller.baseCellSize", access, [&settings] { return settings.baseCellSize; }, // 0x10077439, 0x100774aa, 0x100774f7
		[&settings](const int& v) {
			// 0x1009cf40: a power of two from 16 to 512, at least the
			// smallest cell (32); the patch limit is the renderer's.
			if (v >= 32 && v <= 512 && (v & (v - 1)) == 0) {
				settings.baseCellSize = v;
			}
		});
	bindFloat("renderer.indoorSkinnedMeshCullDistance", settings.indoorSkinnedMeshCullDistance); // 0x1004e250, 0x1004e3b0, 0x1004e410
	bindFloat("renderer.indoorBundledMeshCullDistance", settings.indoorBundledMeshCullDistance); // 0x1004e460, 0x1004e550, 0x1004e5b0
	const auto bindBool = [&console, access](const char* name, bool& value) {
		bindProperty<bool>(console, name, access, [&value] { return value; }, [&value](const bool& v) { value = v; });
	};
	bindBool("ObjectDrawer.isInsideTitanOverride", settings.insideTitanOverride); // 0x100759ee, 0x10075a3b
	bindBool("ObjectDrawer.showAllTitanParts", settings.showAllTitanParts); // 0x10075afb, 0x10075b48
	// The draw switches (RendererSettings; 0x1004c7a0..0x1004e0b0).
	bindBool("renderer.drawObjects", settings.drawObjects); // 0x1004c890, 0x1004c8f0
	bindBool("renderer.drawBundledMeshes", settings.drawBundledMeshes); // 0x1004c940, 0x1004ca30, 0x1004ca90
	bindBool("renderer.drawStaticMeshes", settings.drawStaticMeshes); // 0x1004ce40, 0x1004cf40
	bindBool("renderer.drawSkinnedMeshes", settings.drawSkinnedMeshes); // 0x1004d000, 0x1004d0f0, 0x1004d150
	bindBool("renderer.draw1pMeshes", settings.draw1pMeshes); // 0x1004cb00, 0x1004cbf0, 0x1004cc50
	bindBool("renderer.drawParticles", settings.drawParticles); // 0x1004cca0, 0x1004cd90, 0x1004cdf0
	bindBool("renderer.drawSkyDome", settings.drawSkyDome); // 0x1004d200, 0x1004d2f0, 0x1004d350
	bindBool("renderer.drawSunFlare", settings.drawSunFlare); // 0x1004d3a0, 0x1004d490, 0x1004d4f0
	bindBool("renderer.drawRoads", settings.drawRoads); // 0x1004d540, 0x1004d630, 0x1004d690
	bindBool("renderer.drawTerrain", settings.drawTerrain); // 0x1004d6f0, 0x1004d7e0, 0x1004d840
	bindBool("renderer.drawUndergrowth", settings.drawUndergrowth); // 0x1004d890, 0x1004d980, 0x1004d9e0
	bindBool("renderer.drawOvergrowth", settings.drawOvergrowth); // 0x1004da30, 0x1004db20, 0x1004db80
	bindBool("renderer.drawNametags", settings.drawNametags); // 0x1004dbd0, 0x1004dcc0, 0x1004dd20
	bindBool("renderer.drawTrees", settings.drawTrees); // 0x1004dd70, 0x1004de60, 0x1004dec0
	// renderer.drawFps (0x10053ae0; 0x10027570 makes any value over 0 a 1).
	bindProperty<int>(console, "renderer.drawFps", access, [&settings] { return settings.drawFps; }, // 0x10053bd0, 0x10053c30
		[&settings](const int& v) { settings.drawFps = v > 0 ? 1 : v; });
	bindProperty<float>(console, "renderer.tweakNearPlane", access, [&settings] { return settings.tweakNearPlane; }, // 0x10051c30, 0x10051c90
		[&settings](const float& v) {
			settings.tweakNearPlane = v;
			settings.tweakNearPlaneSet = true;
		});
	bindProperty<float>(console, "renderer.tweakNearPlane1p", access, [&settings] { return settings.tweakNearPlane1p; }, // 0x10051dd0, 0x10051e30
		[&settings](const float& v) {
			settings.tweakNearPlane1p = v;
			settings.tweakNearPlane1pSet = true;
		});
	bindFloat("renderer.minCullDistance", settings.minCullDistance); // 0x10051a80, 0x10051ae0
	bindBool("renderer.hud", settings.hud); // 0x1004fb80, 0x1004fc70, 0x1004fcd0
	bindBool("renderer.vsync", settings.vsync); // 0x1004be80, 0x1004bf70, 0x1004bfd0
	bindBool("renderer.presentAsync", settings.presentAsync); // 0x1004c030, 0x1004c190, 0x1004c1f0
	bindBool("renderer.useSunScaling", settings.useSunScaling); // 0x1004c250, 0x1004c340, 0x1004c3a0
	bindBool("renderer.alphaToMask", settings.alphaToMask); // 0x1004e600, 0x1004e6f0, 0x1004e750
	bindBool("renderer.enableHWOcclusion", settings.enableHWOcclusion); // 0x1004c5e0, 0x1004c6d0, 0x1004c6e0, 0x1004c740
	bindBool("renderer.drawGraphs", settings.drawGraphs); // 0x1004e1a0, 0x1004e200
	bindProperty<int>(console, "renderer.renderAheadLimit", access, [&settings] { return settings.renderAheadLimit; }, // 0x10052380, 0x10052470, 0x100524e0, 0x10052540
		[&settings](const int& v) { settings.renderAheadLimit = v; });
	bindFloat("Renderer.fieldOfView1p", settings.fieldOfView1p); // 0x10072704, 0x10072775, 0x100727c2
	bindFloat("Renderer.roadLodDistMod", settings.roadLodDistMod); // 0x10072b7f, 0x10072bcc
	bindFloat("Renderer.roadDepthBias", settings.roadDepthBias); // 0x100728f6, 0x10072967, 0x100729b4
	bindFloat("Renderer.roadSlopeScaleDepthBias", settings.roadSlopeScaleDepthBias); // 0x100729fc, 0x10072a6d, 0x10072aba
	bindBool("Renderer.disableFadeOnRoads", settings.disableFadeOnRoads); // 0x1007285f, 0x100728ac
	bindBool("StaticMeshRenderer.enableRendering", settings.drawStaticMeshes); // 0x10079e75, 0x10079ec2
	bindBool("StaticMeshRenderer.noLods", settings.staticNoLods); // 0x10079f88, 0x10079fd5
	bindBool("undergrowth.enabled", settings.undergrowthEnabled); // 0x1008613a, 0x10086187
	bindProperty<float>(console, "undergrowth.viewDistanceScale", access, // 0x100866ff, 0x10086770
		[&settings] { return settings.undergrowthViewDistanceScale; },
		[&settings](const float& v) {
			settings.undergrowthViewDistanceScale = std::clamp(v, 0.0f, 1.0f);
			settings.undergrowthViewDistanceScaleSet = true;
		});
	bindProperty<float>(console, "undergrowth.viewDistanceFade", access, // 0x10086806, 0x10086877
		[&settings] { return settings.undergrowthViewDistanceFade; },
		[&settings](const float& v) { settings.undergrowthViewDistanceFade = std::clamp(v, 0.0f, 1.0f); });
	bindProperty<int>(console, "undergrowth.zPassPatchCount", access, // 0x100863f6, 0x10086473, 0x100864c0
		[&settings] { return settings.undergrowthZPassPatchCount; },
		[&settings](const int& v) { settings.undergrowthZPassPatchCount = v; });
}

void registerLightConsoleMethods(Console& console, LightSettings& settings)
{
	bindVec3(console, "lightManager.sunDirection", settings.sunDirection); // 0x1007e0c6, 0x1007e150, 0x1007e1c3
	bindVec3(console, "lightManager.sunColor", settings.sunColor); // 0x1007e247, 0x1007e2cc, 0x1007e33f
	bindVec3(console, "lightManager.skyColor", settings.skyColor); // 0x1007c576, 0x1007c5fb, 0x1007c66e
	bindVec3(console, "lightManager.ambientColor", settings.ambientColor); // 0x1007c6de, 0x1007c763, 0x1007c7d6
	bindVec3(console, "lightManager.staticSunColor", settings.staticSunColor); // 0x1007cc8a, 0x1007cd0f, 0x1007cd82
	bindVec3(console, "lightManager.staticSkyColor", settings.staticSkyColor); // 0x1007cdf8, 0x1007ce7d, 0x1007cef0
	bindVec3(console, "terrain.sunColor", settings.terrainSunColor);
	bindVec3(console, "terrain.GIColor", settings.terrainGIColor);
	bindVec3(console, "renderer.fogColor", settings.fogColor); // 0x1004f230, 0x1004f330, 0x1004f3a0, 0x1004f420
	bindProperty<ConsoleVec4>(console, "renderer.fogStartEndAndBase", ConsoleAccess::Anyone, // 0x1004ef30, 0x1004f090, 0x1004f1b0
		[&settings] {
			const auto& f = settings.fogStartEndAndBase;
			return ConsoleVec4 {f[0], f[1], f[2], f[3]};
		},
		[&settings](const ConsoleVec4& v) { settings.fogStartEndAndBase = {v.x, v.y, v.z, v.w}; });
	const uint32_t access = ConsoleAccess::Anyone;
	const auto bindFloat = [&console, access](const char* name, float& value) {
		bindProperty<float>(console, name, access, [&value] { return value; }, [&value](const float& v) { value = v; });
	};
	const auto bindInt = [&console, access](const char* name, int& value) {
		bindProperty<int>(console, name, access, [&value] { return value; }, [&value](const int& v) { value = v; });
	};
	const auto bindBool = [&console, access](const char* name, bool& value) {
		bindProperty<bool>(console, name, access, [&value] { return value; }, [&value](const bool& v) { value = v; });
	};
	// lightManager.init (0x1007c509) resets nothing the scripts don't set next.
	bindMethod(console, "lightManager.init", access, [] {}); // 0x1007c55b
	bindVec3(console, "lightManager.treeAmbientColor", settings.treeAmbientColor); // 0x1007c846, 0x1007c8cb, 0x1007c93e
	bindVec3(console, "lightManager.treeSunColor", settings.treeSunColor); // 0x1007c9ae, 0x1007ca33, 0x1007caa6
	bindVec3(console, "lightManager.treeSkyColor", settings.treeSkyColor); // 0x1007cb1c, 0x1007cba1, 0x1007cc14
	bindVec3(console, "lightManager.staticSpecularColor", settings.staticSpecularColor); // 0x1007cf66, 0x1007cfeb, 0x1007d05e
	bindVec3(console, "lightManager.singlePointColor", settings.singlePointColor); // 0x1007d0d4, 0x1007d159, 0x1007d1cc
	bindVec3(console, "lightManager.effectSunColor", settings.effectSunColor); // 0x1007d242, 0x1007d31f, 0x1007d392
	bindVec3(console, "lightManager.effectShadowColor", settings.effectShadowColor); // 0x1007d408, 0x1007d48d, 0x1007d500
	bindVec3(console, "lightManager.sunSpecColor", settings.sunSpecColor); // 0x1007e3c3, 0x1007e448, 0x1007e4bb
	bindFloat("lightManager.defaultEffectLightAffectionFactor", settings.defaultEffectLightAffectionFactor); // 0x1007d576, 0x1007d5e7, 0x1007d634
	bindProperty<std::string>(console, "lightManager.groundHemi", access, [&settings] { return settings.groundHemi; }, // 0x1007d683, 0x1007d700, 0x1007d76a
		[&settings](const std::string& v) { settings.groundHemi = v; });
	bindFloat("lightManager.hemiLerpBias", settings.hemiLerpBias); // 0x1007d7df, 0x1007d850, 0x1007d89d
	bindFloat("lightManager.lightCullDistance", settings.lightCullDistance); // 0x1007d8e6, 0x1007d957, 0x1007d9a4
	bindInt("lightManager.maxLightsPerBundledMesh", settings.maxLightsPerBundledMesh); // 0x1007d9ed, 0x1007da5e, 0x1007daab
	bindInt("lightManager.maxLightsPerStaticMesh", settings.maxLightsPerStaticMesh); // 0x1007daf1, 0x1007dbbb, 0x1007dc08
	bindInt("lightManager.maxLightsPerSkinnedMesh", settings.maxLightsPerSkinnedMesh); // 0x1007dc4e, 0x1007dcbf, 0x1007dd0c
	bindInt("lightManager.maxLightsPerTerrainPatch", settings.maxLightsPerTerrainPatch); // 0x1007dd52, 0x1007ddc3, 0x1007de10
	bindFloat("lightManager.skinnedMeshShaderLodDistance", settings.skinnedMeshShaderLodDistance); // 0x1007de59, 0x1007deca, 0x1007df17
	bindProperty<ConsoleVec2>(console, "lightManager.lightMapAmbientAlphaCutoffRange", access, // 0x1007df66, 0x1007dff2, 0x1007e05d
		[&settings] {
			return ConsoleVec2 {settings.lightMapAmbientAlphaCutoffRange[0], settings.lightMapAmbientAlphaCutoffRange[1]};
		},
		[&settings](const ConsoleVec2& v) { settings.lightMapAmbientAlphaCutoffRange = {v.x, v.y}; });
	bindBool("lightManager.enableSun", settings.enableSun); // 0x1007e537, 0x1007e5a8, 0x1007e5f5
	bindBool("lightManager.enableStencilCullerReset", settings.enableStencilCullerReset); // 0x1007e6bd, 0x1007e70a
}

void registerWaterConsoleMethods(Console& console, WaterSettings& settings)
{
	const uint32_t access = ConsoleAccess::Anyone;
	bindVec3(console, "renderer.waterColor", settings.color); // 0x100530c0, 0x100531c0, 0x10053240
	bindProperty<ConsoleVec4>(console, "renderer.waterSpecularColor", access, // 0x10052df0, 0x10052ef0, 0x10052f80
		[&settings] {
			const auto& c = settings.specularColor;
			return ConsoleVec4 {c[0], c[1], c[2], c[3]};
		},
		[&settings](const ConsoleVec4& v) { settings.specularColor = {v.x, v.y, v.z, v.w}; });
	const auto bindFloat = [&console, access](const char* name, float& value) {
		bindProperty<float>(console, name, access, [&value] { return value; }, [&value](const float& v) { value = v; });
	};
	bindFloat("renderer.waterSpecularPower", settings.specularPower); // 0x100533d0, 0x100534c0, 0x10053520
	bindFloat("renderer.waterAlphaAdd", settings.alphaAdd); // 0x10053770, 0x10053860, 0x100538c0
	bindFloat("renderer.waterAlphaHeightMul", settings.alphaHeightMul); // 0x100535a0, 0x10053690, 0x100536f0
	bindFloat("renderer.waterAnimSpeed", settings.animSpeed); // 0x100528c0, 0x100529b0, 0x10052a10
	bindProperty<ConsoleVec2>(console, "renderer.waterScroll", access, // 0x10052590, 0x10052680, 0x10052710, 0x100527c0
		[&settings] { return ConsoleVec2 {settings.scroll[0], settings.scroll[1]}; },
		[&settings](const ConsoleVec2& v) { settings.scroll = {v.x, v.y}; });
	// The terrain's own names for the same water (0x10075eb4..0x10076929
	// call the terrain's slots renderer.water* call: 0x8c scroll, 0xac
	// specular power ...).
	bindVec3(console, "terrain.waterColor", settings.color); // 0x10076470, 0x100764f5, 0x10076568
	bindProperty<ConsoleVec4>(console, "terrain.waterSpecularColor", access, // 0x100762f3, 0x1007637d, 0x100763f8
		[&settings] {
			const auto& c = settings.specularColor;
			return ConsoleVec4 {c[0], c[1], c[2], c[3]};
		},
		[&settings](const ConsoleVec4& v) { settings.specularColor = {v.x, v.y, v.z, v.w}; });
	bindFloat("terrain.waterSpecularPower", settings.specularPower); // 0x100765d5, 0x10076646, 0x10076693
	bindFloat("terrain.waterAlphaAdd", settings.alphaAdd); // 0x1007699a, 0x100769e7
	bindFloat("terrain.waterAlphaHeightMul", settings.alphaHeightMul); // 0x1007681c, 0x1007688d, 0x100768da
	bindFloat("terrain.waterAnimSpeed", settings.animSpeed); // 0x10076067, 0x100760d8, 0x10076125
	bindProperty<ConsoleVec2>(console, "terrain.waterScroll", access, // 0x10075f8c, 0x10075ff7
		[&settings] { return ConsoleVec2 {settings.scroll[0], settings.scroll[1]}; },
		[&settings](const ConsoleVec2& v) { settings.scroll = {v.x, v.y}; });
	bindFloat("terrain.waterSunIntensity", settings.sunIntensity); // 0x10076753, 0x100767a0
	bindVec3(console, "renderer.waterFogColor", settings.fogColor); // 0x1004f740, 0x1004f840, 0x1004f8c0
	bindProperty<ConsoleVec4>(console, "renderer.waterFogStartEndAndBase", access, // 0x1004f620, 0x1004f6b0
		[&settings] {
			const auto& f = settings.fogStartEndAndBase;
			return ConsoleVec4 {f[0], f[1], f[2], f[3]};
		},
		[&settings](const ConsoleVec4& v) { settings.fogStartEndAndBase = {v.x, v.y, v.z, v.w}; });
}

void registerRainConsoleMethods(Console& console, RainSettings& settings)
{
	const uint32_t access = ConsoleAccess::Anyone;
	const auto bindFloat = [&console, access](const char* name, float& value) {
		bindProperty<float>(console, name, access, [&value] { return value; }, [&value](const float& v) { value = v; });
	};
	const auto bindBool = [&console, access](const char* name, bool& value) {
		bindProperty<bool>(console, name, access, [&value] { return value; }, [&value](const bool& v) { value = v; });
	};
	const auto bindVec4 = [&console, access](const char* name, std::array<float, 4>& value) {
		bindProperty<ConsoleVec4>(console, name, access,
			[&value] { return ConsoleVec4 {value[0], value[1], value[2], value[3]}; },
			[&value](const ConsoleVec4& v) { value = {v.x, v.y, v.z, v.w}; });
	};
	bindBool("rainManager.enabled", settings.enabled); // 0x1007e7de, 0x1007e82b
	bindProperty<int>(console, "rainManager.particleCount", access, [&settings] { return settings.particleCount; }, // 0x1007e87c, 0x1007e8ed, 0x1007e93a
		[&settings](const int& v) { settings.particleCount = v; });
	bindVec4("rainManager.pos", settings.pos); // 0x1007e980, 0x1007ea0a, 0x1007ea85
	bindVec3(console, "rainManager.offset", settings.offset); // 0x1007eae4, 0x1007eb69, 0x1007ebdc
	bindVec3(console, "rainManager.emitRadius", settings.emitRadius); // 0x1007ec4c, 0x1007ecd1, 0x1007ed44
	bindVec3(console, "rainManager.particleScale", settings.particleScale); // 0x1007edb4, 0x1007ee39, 0x1007eeac
	bindVec4("rainManager.particleColor", settings.particleColor); // 0x1007ef1c, 0x1007effe, 0x1007f079
	bindFloat("rainManager.particleSize", settings.particleSize); // 0x1007f0d8, 0x1007f149, 0x1007f196
	bindFloat("rainManager.particleMaxSize", settings.particleMaxSize); // 0x1007f1df, 0x1007f250, 0x1007f29d
	bindVec3(console, "rainManager.particleVelocity", settings.particleVelocity); // 0x1007f2e6, 0x1007f36b, 0x1007f3de
	bindFloat("rainManager.particleSpeed", settings.particleSpeed); // 0x1007f44e, 0x1007f4bf, 0x1007f50c
	bindFloat("rainManager.fadeOutRange", settings.fadeOutRange); // 0x1007f55b, 0x1007f5cc, 0x1007f619
	bindVec3(console, "rainManager.maxDeviation", settings.maxDeviation); // 0x1007f668, 0x1007f6ed, 0x1007f760
	bindProperty<ConsoleVec2>(console, "rainManager.angleSpeed", access, // 0x1007f7d6, 0x1007f856, 0x1007f8c1
		[&settings] { return ConsoleVec2 {settings.angleSpeed[0], settings.angleSpeed[1]}; },
		[&settings](const ConsoleVec2& v) { settings.angleSpeed = {v.x, v.y}; });
	// rainManager.randomness keeps only values over 0 (0x1011acf0).
	bindProperty<int>(console, "rainManager.randomness", access, [&settings] { return settings.randomness; }, // 0x1007f92a, 0x1007f99b, 0x1007f9e8
		[&settings](const int& v) {
			if (v > 0) {
				settings.randomness = v;
			}
		});
	bindFloat("rainManager.maxRandomChange", settings.maxRandomChange); // 0x1007fa34, 0x1007faa5, 0x1007faf2
	bindProperty<int>(console, "rainManager.randomDeviationAngleSpeed", access, // 0x1007fb41, 0x1007fbb2, 0x1007fbff
		[&settings] { return settings.randomDeviationAngleSpeed; },
		[&settings](const int& v) { settings.randomDeviationAngleSpeed = v; });
	bindBool("rainManager.lineDisplay", settings.lineDisplay); // 0x1007fe78, 0x1007fee9, 0x1007ff36
	bindBool("rainManager.freeze", settings.freeze); // 0x1007fcbf, 0x1007fd0c
	bindFloat("rainManager.lineLength", settings.lineLength); // 0x1007ff8d, 0x1007fffe, 0x1008004b
	bindProperty<std::string>(console, "rainManager.texture", access, [&settings] { return settings.texture; }, // 0x10080117, 0x1008018e
		[&settings](const std::string& v) { settings.texture = v; });
}

void registerSkyConsoleMethods(Console& console, SkySettings& settings)
{
	const uint32_t access = ConsoleAccess::Anyone;
	bindProperty<std::string>(console, "Skydome.skyTemplate", access, [&settings] { return settings.skyTemplate; }, // 0x100779b6, 0x10077a33, 0x10077a9d
		[&settings](const std::string& name) { settings.skyTemplate = name; });
	bindProperty<std::string>(console, "Skydome.skyTexture", access, [&settings] { return settings.skyTexture; }, // 0x10078193, 0x10078210, 0x1007827a
		[&settings](const std::string& name) { settings.skyTexture = name; });
	bindProperty<float>(console, "Skydome.domeRotation", access, [&settings] { return settings.domeRotation; }, // 0x100778af, 0x10077920, 0x1007796d
		[&settings](const float& degrees) { settings.domeRotation = degrees; });
	bindVec3(console, "Skydome.domePosition", settings.domePosition); // 0x10077640, 0x100776c5, 0x10077738
	const auto bindFloat = [&console, access](const char* name, float& value) {
		bindProperty<float>(console, name, access, [&value] { return value; }, [&value](const float& v) { value = v; });
	};
	const auto bindBool = [&console, access](const char* name, bool& value) {
		bindProperty<bool>(console, name, access, [&value] { return value; }, [&value](const bool& v) { value = v; });
	};
	const auto bindString = [&console, access](const char* name, std::string& value) {
		bindProperty<std::string>(console, name, access, [&value] { return value; },
			[&value](const std::string& v) { value = v; });
	};
	const auto bindVec2 = [&console, access](const char* name, std::array<float, 2>& value) {
		bindProperty<ConsoleVec2>(console, name, access, [&value] { return ConsoleVec2 {value[0], value[1]}; },
			[&value](const ConsoleVec2& v) { value = {v.x, v.y}; });
	};
	bindFloat("Skydome.domeSize", settings.domeSize); // 0x100777a8, 0x10077819, 0x10077866
	bindBool("Skydome.hasCloudLayer", settings.hasCloudLayer); // 0x100782ef, 0x10078360, 0x100783ad
	bindBool("Skydome.hasCloudLayer2", settings.hasCloudLayer2); // 0x100783fe, 0x1007846f, 0x100784bc
	bindString("Skydome.cloudTemplate", settings.cloudTemplate); // 0x1007850d, 0x1007858a, 0x100785f4
	bindString("Skydome.cloudTexture", settings.cloudTexture); // 0x10078669, 0x100786e6, 0x10078750
	bindString("Skydome.cloudTexture2", settings.cloudTexture2); // 0x100787c5, 0x10078842, 0x100788ac
	bindVec2("Skydome.scrollDirection", settings.scrollDirection); // 0x10078921, 0x100789a1, 0x10078a0c
	bindVec2("Skydome.scrollDirection2", settings.scrollDirection2); // 0x10078a6f, 0x10078aef, 0x10078b5a
	bindVec2("Skydome.fadeCloudsDistances", settings.fadeCloudsDistances); // 0x10078bbd, 0x10078c3d, 0x10078ca8
	bindVec2("Skydome.cloudLerpFactors", settings.cloudLerpFactors); // 0x10078d0b, 0x10078d97, 0x10078e02
	bindString("Skydome.flareTemplate", settings.flareTemplate); // 0x10077b12, 0x10077b8f, 0x10077c1e
	bindString("Skydome.flareTexture", settings.flareTexture); // 0x10077c93, 0x10077d10, 0x10077d80
	bindVec3(console, "Skydome.flareDirection", settings.flareDirection); // 0x10077df5, 0x10077e7a, 0x10077eed
	bindFloat("Skydome.flareFadeAdd", settings.flareFadeAdd); // 0x10077f8b, 0x10077ffc, 0x10078049
	bindFloat("Skydome.flareFadeMul", settings.flareFadeMul); // 0x1007808f, 0x10078100, 0x1007814d
}

} // namespace dice::hfe::scene
