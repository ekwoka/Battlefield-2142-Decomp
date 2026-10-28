#include "LightSettings.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"

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
	bindFloat("renderer.globalStaticMeshLodDistanceScale", settings.staticMeshLodDistanceScale);
	bindFloat("renderer.globalBundleMeshLodDistanceScale", settings.bundleMeshLodDistanceScale);
	bindFloat("renderer.globalSkinnedMeshLodDistanceScale", settings.skinnedMeshLodDistanceScale);
	bindProperty<int>(console, "terrainCuller.lodSwitchDistance", access,
		[&settings] { return settings.lodSwitchDistance; },
		[&settings](const int& v) {
			if (v >= settings.baseCellSize && v <= 1000) { // 0x1009c9b0
				settings.lodSwitchDistance = v;
				settings.lodSwitchDistanceSet = true;
			}
		});
	bindFloat("terrainCuller.lod0SwitchStart", settings.lod0SwitchStart);
	bindFloat("terrainCuller.lod0SwitchStop", settings.lod0SwitchStop);
	bindFloat("terrainCuller.lod1SwitchStart", settings.lod1SwitchStart);
	bindFloat("terrainCuller.lod1SwitchStop", settings.lod1SwitchStop);
	bindFloat("terrainCuller.lod2Start", settings.lod2Start);
	bindProperty<int>(console, "terrainCuller.baseCellSize", access, [&settings] { return settings.baseCellSize; },
		[&settings](const int& v) {
			// 0x1009cf40: a power of two from 16 to 512, at least the
			// smallest cell (32); the patch limit is the renderer's.
			if (v >= 32 && v <= 512 && (v & (v - 1)) == 0) {
				settings.baseCellSize = v;
			}
		});
}

void registerLightConsoleMethods(Console& console, LightSettings& settings)
{
	bindVec3(console, "lightManager.sunDirection", settings.sunDirection);
	bindVec3(console, "lightManager.sunColor", settings.sunColor);
	bindVec3(console, "lightManager.skyColor", settings.skyColor);
	bindVec3(console, "lightManager.ambientColor", settings.ambientColor);
	bindVec3(console, "lightManager.staticSunColor", settings.staticSunColor);
	bindVec3(console, "lightManager.staticSkyColor", settings.staticSkyColor);
	bindVec3(console, "terrain.sunColor", settings.terrainSunColor);
	bindVec3(console, "terrain.GIColor", settings.terrainGIColor);
	bindVec3(console, "renderer.fogColor", settings.fogColor);
	bindProperty<ConsoleVec4>(console, "renderer.fogStartEndAndBase", ConsoleAccess::Anyone,
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
	bindMethod(console, "lightManager.init", access, [] {});
	bindVec3(console, "lightManager.treeAmbientColor", settings.treeAmbientColor);
	bindVec3(console, "lightManager.treeSunColor", settings.treeSunColor);
	bindVec3(console, "lightManager.treeSkyColor", settings.treeSkyColor);
	bindVec3(console, "lightManager.staticSpecularColor", settings.staticSpecularColor);
	bindVec3(console, "lightManager.singlePointColor", settings.singlePointColor);
	bindVec3(console, "lightManager.effectSunColor", settings.effectSunColor);
	bindVec3(console, "lightManager.effectShadowColor", settings.effectShadowColor);
	bindVec3(console, "lightManager.sunSpecColor", settings.sunSpecColor);
	bindFloat("lightManager.defaultEffectLightAffectionFactor", settings.defaultEffectLightAffectionFactor);
	bindProperty<std::string>(console, "lightManager.groundHemi", access, [&settings] { return settings.groundHemi; },
		[&settings](const std::string& v) { settings.groundHemi = v; });
	bindFloat("lightManager.hemiLerpBias", settings.hemiLerpBias);
	bindFloat("lightManager.lightCullDistance", settings.lightCullDistance);
	bindInt("lightManager.maxLightsPerBundledMesh", settings.maxLightsPerBundledMesh);
	bindInt("lightManager.maxLightsPerStaticMesh", settings.maxLightsPerStaticMesh);
	bindInt("lightManager.maxLightsPerSkinnedMesh", settings.maxLightsPerSkinnedMesh);
	bindInt("lightManager.maxLightsPerTerrainPatch", settings.maxLightsPerTerrainPatch);
	bindFloat("lightManager.skinnedMeshShaderLodDistance", settings.skinnedMeshShaderLodDistance);
	bindProperty<ConsoleVec2>(console, "lightManager.lightMapAmbientAlphaCutoffRange", access,
		[&settings] {
			return ConsoleVec2 {settings.lightMapAmbientAlphaCutoffRange[0], settings.lightMapAmbientAlphaCutoffRange[1]};
		},
		[&settings](const ConsoleVec2& v) { settings.lightMapAmbientAlphaCutoffRange = {v.x, v.y}; });
	bindBool("lightManager.enableSun", settings.enableSun);
	bindBool("lightManager.enableStencilCullerReset", settings.enableStencilCullerReset);
}

void registerWaterConsoleMethods(Console& console, WaterSettings& settings)
{
	const uint32_t access = ConsoleAccess::Anyone;
	bindVec3(console, "renderer.waterColor", settings.color);
	bindProperty<ConsoleVec4>(console, "renderer.waterSpecularColor", access,
		[&settings] {
			const auto& c = settings.specularColor;
			return ConsoleVec4 {c[0], c[1], c[2], c[3]};
		},
		[&settings](const ConsoleVec4& v) { settings.specularColor = {v.x, v.y, v.z, v.w}; });
	const auto bindFloat = [&console, access](const char* name, float& value) {
		bindProperty<float>(console, name, access, [&value] { return value; }, [&value](const float& v) { value = v; });
	};
	bindFloat("renderer.waterSpecularPower", settings.specularPower);
	bindFloat("renderer.waterAlphaAdd", settings.alphaAdd);
	bindFloat("renderer.waterAlphaHeightMul", settings.alphaHeightMul);
	bindFloat("renderer.waterAnimSpeed", settings.animSpeed);
	bindProperty<ConsoleVec2>(console, "renderer.waterScroll", access,
		[&settings] { return ConsoleVec2 {settings.scroll[0], settings.scroll[1]}; },
		[&settings](const ConsoleVec2& v) { settings.scroll = {v.x, v.y}; });
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
	bindBool("rainManager.enabled", settings.enabled);
	bindProperty<int>(console, "rainManager.particleCount", access, [&settings] { return settings.particleCount; },
		[&settings](const int& v) { settings.particleCount = v; });
	bindVec4("rainManager.pos", settings.pos);
	bindVec3(console, "rainManager.offset", settings.offset);
	bindVec3(console, "rainManager.emitRadius", settings.emitRadius);
	bindVec3(console, "rainManager.particleScale", settings.particleScale);
	bindVec4("rainManager.particleColor", settings.particleColor);
	bindFloat("rainManager.particleSize", settings.particleSize);
	bindFloat("rainManager.particleMaxSize", settings.particleMaxSize);
	bindVec3(console, "rainManager.particleVelocity", settings.particleVelocity);
	bindFloat("rainManager.particleSpeed", settings.particleSpeed);
	bindFloat("rainManager.fadeOutRange", settings.fadeOutRange);
	bindVec3(console, "rainManager.maxDeviation", settings.maxDeviation);
	bindProperty<ConsoleVec2>(console, "rainManager.angleSpeed", access,
		[&settings] { return ConsoleVec2 {settings.angleSpeed[0], settings.angleSpeed[1]}; },
		[&settings](const ConsoleVec2& v) { settings.angleSpeed = {v.x, v.y}; });
	bindBool("rainManager.lineDisplay", settings.lineDisplay);
	bindFloat("rainManager.lineLength", settings.lineLength);
	bindProperty<std::string>(console, "rainManager.texture", access, [&settings] { return settings.texture; },
		[&settings](const std::string& v) { settings.texture = v; });
}

void registerSkyConsoleMethods(Console& console, SkySettings& settings)
{
	const uint32_t access = ConsoleAccess::Anyone;
	bindProperty<std::string>(console, "Skydome.skyTemplate", access, [&settings] { return settings.skyTemplate; },
		[&settings](const std::string& name) { settings.skyTemplate = name; });
	bindProperty<std::string>(console, "Skydome.skyTexture", access, [&settings] { return settings.skyTexture; },
		[&settings](const std::string& name) { settings.skyTexture = name; });
	bindProperty<float>(console, "Skydome.domeRotation", access, [&settings] { return settings.domeRotation; },
		[&settings](const float& degrees) { settings.domeRotation = degrees; });
	bindVec3(console, "Skydome.domePosition", settings.domePosition);
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
	bindFloat("Skydome.domeSize", settings.domeSize);
	bindBool("Skydome.hasCloudLayer", settings.hasCloudLayer);
	bindBool("Skydome.hasCloudLayer2", settings.hasCloudLayer2);
	bindString("Skydome.cloudTemplate", settings.cloudTemplate);
	bindString("Skydome.cloudTexture", settings.cloudTexture);
	bindString("Skydome.cloudTexture2", settings.cloudTexture2);
	bindVec2("Skydome.scrollDirection", settings.scrollDirection);
	bindVec2("Skydome.scrollDirection2", settings.scrollDirection2);
	bindVec2("Skydome.fadeCloudsDistances", settings.fadeCloudsDistances);
	bindVec2("Skydome.cloudLerpFactors", settings.cloudLerpFactors);
	bindString("Skydome.flareTemplate", settings.flareTemplate);
	bindString("Skydome.flareTexture", settings.flareTexture);
	bindVec3(console, "Skydome.flareDirection", settings.flareDirection);
	bindFloat("Skydome.flareFadeAdd", settings.flareFadeAdd);
	bindFloat("Skydome.flareFadeMul", settings.flareFadeMul);
}

} // namespace dice::hfe::scene
