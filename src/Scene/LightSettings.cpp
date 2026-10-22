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
}

} // namespace dice::hfe::scene
