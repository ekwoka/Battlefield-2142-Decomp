#include "PostProductionSettings.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"

#include <algorithm>

namespace dice::hfe::scene {

namespace {

constexpr uint32_t kAccess = ConsoleAccess::Anyone;

void bindFloat(Console& console, const char* name, float& value)
{
	bindProperty<float>(console, name, kAccess, [&value] { return value; }, [&value](const float& v) { value = v; });
}

void bindBool(Console& console, const char* name, bool& value)
{
	bindProperty<bool>(console, name, kAccess, [&value] { return value; }, [&value](const bool& v) { value = v; });
}

void bindInt(Console& console, const char* name, int& value)
{
	bindProperty<int>(console, name, kAccess, [&value] { return value; }, [&value](const int& v) { value = v; });
}

void bindVec3(Console& console, const char* name, std::array<float, 3>& value)
{
	bindProperty<ConsoleVec3>(console, name, kAccess, [&value] { return ConsoleVec3 {value[0], value[1], value[2]}; },
		[&value](const ConsoleVec3& v) { value = {v.x, v.y, v.z}; });
}

} // namespace

void registerPostProductionConsoleMethods(Console& console, PostProductionSettings& s)
{
	bindBool(console, "renderer.drawPostProduction", s.drawPostProduction);
	bindFloat(console, "renderer.bloomFilterContrast", s.bloomFilterContrast);
	bindFloat(console, "renderer.bloomSize", s.bloomSize);
	bindFloat(console, "renderer.bloomWeightScale", s.bloomWeightScale);
	bindFloat(console, "renderer.bloomOverlayAmount", s.bloomOverlayAmount);
	bindBool(console, "renderer.showBloomOverlayTexture", s.showBloomOverlayTexture);
	bindVec3(console, "renderer.defaultColorTint", s.defaultColorTint);
	bindFloat(console, "renderer.defaultSaturation", s.defaultSaturation);
	bindVec3(console, "renderer.defaultContrast", s.defaultContrast);
	bindInt(console, "renderer.empEffectLevel", s.empEffectLevel);
	// 0x10050080: any value other than 0 turns it on.
	bindProperty<float>(console, "renderer.empEnableSimpleShader", kAccess,
		[&s] { return s.empEnableSimpleShader ? 1.0f : 0.0f; },
		[&s](const float& v) { s.empEnableSimpleShader = v != 0; });
	bindFloat(console, "renderer.empStaticNoiseAmount", s.empStaticNoiseAmount);
	bindFloat(console, "renderer.empVerticalRoll", s.empVerticalRoll);
	bindFloat(console, "renderer.empWarpAmount", s.empWarpAmount);
	bindFloat(console, "renderer.empWarpFrequency", s.empWarpFrequency);
	bindProperty<ConsoleVec2>(console, "renderer.camouflageOffset", kAccess,
		[&s] { return ConsoleVec2 {s.camouflageOffset[0], s.camouflageOffset[1]}; },
		[&s](const ConsoleVec2& v) { s.camouflageOffset = {v.x, v.y}; });
	bindBool(console, "renderer.glowEnabled", s.glowEnabled);
	bindFloat(console, "renderer.glowStrength", s.glowStrength);
	bindFloat(console, "renderer.colorBrightness", s.colorBrightness);
	bindMethod(console, "VideoSettings.setUseBloom", kAccess, [&s](bool on) { s.useBloom = on; });
}

void registerShadowConsoleMethods(Console& console, ShadowSettings& s)
{
	// 0x1014a580: the map's size (the device must accept it, a power of two here).
	bindProperty<int>(console, "shadowManager.shadowMapSize", kAccess, [&s] { return s.shadowMapSize; },
		[&s](const int& v) {
			if (v >= 64 && v <= 4096 && (v & (v - 1)) == 0) {
				s.shadowMapSize = v;
				s.shadowMapObjectSize = std::min(s.shadowMapObjectSize, v);
			}
		});
	// 0x1014a5d0: at most the map's size.
	bindProperty<int>(console, "shadowManager.shadowMapObjectSize", kAccess, [&s] { return s.shadowMapObjectSize; },
		[&s](const int& v) {
			if (v <= s.shadowMapSize) {
				s.shadowMapObjectSize = v;
			}
		});
	// 0x1014a600: 16, 24 or 32 bits.
	bindProperty<int>(console, "shadowManager.shadowMapBitDepth", kAccess, [&s] { return s.shadowMapBitDepth; },
		[&s](const int& v) {
			if (v == 16 || v == 24 || v == 32) {
				s.shadowMapBitDepth = v;
			}
		});
	bindFloat(console, "shadowManager.shadowMapDepthDistance", s.shadowMapDepthDistance);
	bindBool(console, "shadowManager.shadowMapIncludeStaticMeshes", s.shadowMapIncludeStaticMeshes);
	bindBool(console, "shadowManager.shadowShowFrustums", s.shadowShowFrustums);
	bindFloat(console, "shadowManager.shadowIntensityBias", s.shadowIntensityBias);
	bindFloat(console, "shadowManager.lightmapIntensityBias", s.lightmapIntensityBias);
	bindFloat(console, "shadowManager.dropShadowClipHeightOffset", s.dropShadowClipHeightOffset);
	bindBool(console, "shadowManager.dynamicShadowsEnabled", s.dynamicShadowsEnabled);
	bindBool(console, "shadowManager.dynamicStaticMeshShadowsEnabled", s.dynamicStaticMeshShadowsEnabled);
	bindBool(console, "shadowManager.dynamic1pShadowsEnabled", s.dynamic1pShadowsEnabled);
	bindBool(console, "shadowManager.allowAlphaShadows", s.allowAlphaShadows);
	bindFloat(console, "shadowManager.alphaShadowThreshold", s.alphaShadowThreshold);
	bindFloat(console, "shadowManager.shadowUpdateFactor", s.shadowUpdateFactor);
	bindBool(console, "shadowManager.shadowUpdatesEnabled", s.shadowUpdatesEnabled);
	bindFloat(console, "shadowManager.shadowMapDepthBias", s.shadowMapDepthBias);
	bindFloat(console, "shadowManager.shadowMapSlopeBias", s.shadowMapSlopeBias);
	bindFloat(console, "shadowManager.shadowOccluderOversizeFactor", s.shadowOccluderOversizeFactor);
	bindBool(console, "shadowManager.useLSSMOptimizations", s.useLSSMOptimizations);
	bindBool(console, "shadowManager.usePSMOptimizations", s.usePSMOptimizations);
	bindBool(console, "shadowManager.shadowPSMUseSlideback", s.shadowPSMUseSlideback);
	bindBool(console, "shadowManager.shadowPSMUseUnitCubeClipping", s.shadowPSMUseUnitCubeClipping);
	bindFloat(console, "shadowManager.shadowPSMZNearOffset", s.shadowPSMZNearOffset);
	bindFloat(console, "shadowManager.shadowPSMZFarOffset", s.shadowPSMZFarOffset);
	bindFloat(console, "shadowManager.shadowPSMZMinInfinityZ", s.shadowPSMZMinInfinityZ);
	bindFloat(console, "shadowManager.shadowPSMFovY", s.shadowPSMFovY);
	bindFloat(console, "shadowManager.shadowTSMFocus", s.shadowTSMFocus);
	bindFloat(console, "shadowManager.shadow80Line", s.shadow80Line);
	bindBool(console, "shadowManager.shadowMergeDynamicShadowFrustums", s.shadowMergeDynamicShadowFrustums);
	bindMethod(console, "VideoSettings.setDynamicShadowsQuality", kAccess,
		[&s](int quality) { s.quality = std::clamp(quality, 0, 3); });
	bindBool(console, "undergrowth.dynamicShadowsEnable", s.undergrowthShadows);
}

void registerHemiMapConsoleMethods(Console& console, HemiMapSettings& s)
{
	// HemiMapManager.setBaseHemiMap <texture> <corner> <size> [<height>]
	// (0x100857b8): the levels' Sky.con, e.g. "Levels/Camp_Gibraltar/groundhemi
	// 0/0/0 2048 500".
	bindMethod(console, "HemiMapManager.setBaseHemiMap", kAccess,
		[&s](std::string path, ConsoleVec3 corner, float size, float height) {
			s.baseHemiMap = path;
			s.offset = {corner.x, corner.y, corner.z};
			s.size = size;
			s.height = height;
			return true;
		},
		3);
	// HemiMapManager.loadTitanHemiMap <texture> <position> <size> <team> (0x10085535).
	bindMethod(console, "HemiMapManager.loadTitanHemiMap", kAccess,
		[&s](std::string path, ConsoleVec3 position, float size, int team) {
			HemiMapSettings::TitanHemiMap& map = s.titans[static_cast<size_t>(std::clamp(team, 0, 2))];
			map = {path, {position.x, position.y, position.z}, size, team};
		});
	bindFloat(console, "HemiMapManager.hemiLerpBias", s.hemiLerpBias);
	bindBool(console, "HemiMapManager.useCurrentSetting", s.useCurrentSetting);
}

} // namespace dice::hfe::scene
