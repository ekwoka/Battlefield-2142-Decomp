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
	bindBool(console, "renderer.drawPostProduction", s.drawPostProduction); // 0x1004df10, 0x1004e000, 0x1004e060
	bindFloat(console, "renderer.bloomFilterContrast", s.bloomFilterContrast); // 0x10050780, 0x10050870
	bindFloat(console, "renderer.bloomSize", s.bloomSize); // 0x10050920, 0x10050a10, 0x10050a70
	bindFloat(console, "renderer.bloomWeightScale", s.bloomWeightScale); // 0x10050ac0, 0x10050bb0, 0x10050c10
	bindFloat(console, "renderer.bloomOverlayAmount", s.bloomOverlayAmount); // 0x10050c90, 0x10050d80, 0x10050de0
	bindBool(console, "renderer.showBloomOverlayTexture", s.showBloomOverlayTexture); // 0x10050e30, 0x10050f20
	bindVec3(console, "renderer.defaultColorTint", s.defaultColorTint); // 0x10050fd0, 0x10051170, 0x100511f0
	bindFloat(console, "renderer.defaultSaturation", s.defaultSaturation); // 0x10051270, 0x10051360, 0x100513c0
	bindVec3(console, "renderer.defaultContrast", s.defaultContrast); // 0x100517e0, 0x10051860
	bindInt(console, "renderer.empEffectLevel", s.empEffectLevel); // 0x1004fe80, 0x1004fee0
	// 0x10050080: any value other than 0 turns it on.
	bindProperty<float>(console, "renderer.empEnableSimpleShader", kAccess, // 0x1004ff30, 0x10050020
		[&s] { return s.empEnableSimpleShader ? 1.0f : 0.0f; },
		[&s](const float& v) { s.empEnableSimpleShader = v != 0; });
	bindFloat(console, "renderer.empStaticNoiseAmount", s.empStaticNoiseAmount); // 0x100505e0, 0x100506d0
	bindFloat(console, "renderer.empVerticalRoll", s.empVerticalRoll); // 0x10050100, 0x100501f0
	bindFloat(console, "renderer.empWarpAmount", s.empWarpAmount); // 0x100502a0, 0x10050390, 0x100503f0
	bindFloat(console, "renderer.empWarpFrequency", s.empWarpFrequency); // 0x10050440, 0x10050530, 0x10050590
	bindProperty<ConsoleVec2>(console, "renderer.camouflageOffset", kAccess, // 0x10051410, 0x10051500, 0x100515a0, 0x10051670
		[&s] { return ConsoleVec2 {s.camouflageOffset[0], s.camouflageOffset[1]}; },
		[&s](const ConsoleVec2& v) { s.camouflageOffset = {v.x, v.y}; });
	bindBool(console, "renderer.glowEnabled", s.glowEnabled); // 0x1004ebf0, 0x1004ecf0
	bindFloat(console, "renderer.glowStrength", s.glowStrength); // 0x1004ed90, 0x1004ee80
	bindFloat(console, "renderer.colorBrightness", s.colorBrightness); // 0x1004f940, 0x1004fa40, 0x1004fa50
	bindMethod(console, "VideoSettings.setUseBloom", kAccess, [&s](bool on) { s.useBloom = on; });
}

void registerShadowConsoleMethods(Console& console, ShadowSettings& s)
{
	// 0x1014a580: the map's size (the device must accept it, a power of two here).
	bindProperty<int>(console, "shadowManager.shadowMapSize", kAccess, [&s] { return s.shadowMapSize; }, // 0x1008035c, 0x100803a9
		[&s](const int& v) {
			if (v >= 64 && v <= 4096 && (v & (v - 1)) == 0) {
				s.shadowMapSize = v;
				s.shadowMapObjectSize = std::min(s.shadowMapObjectSize, v);
			}
		});
	// 0x1014a5d0: at most the map's size.
	bindProperty<int>(console, "shadowManager.shadowMapObjectSize", kAccess, [&s] { return s.shadowMapObjectSize; }, // 0x100803f5, 0x10080466, 0x100804b3
		[&s](const int& v) {
			if (v <= s.shadowMapSize) {
				s.shadowMapObjectSize = v;
			}
		});
	// 0x1014a600: 16, 24 or 32 bits.
	bindProperty<int>(console, "shadowManager.shadowMapBitDepth", kAccess, [&s] { return s.shadowMapBitDepth; }, // 0x100804ff, 0x10080570, 0x100805bd
		[&s](const int& v) {
			if (v == 16 || v == 24 || v == 32) {
				s.shadowMapBitDepth = v;
			}
		});
	bindFloat(console, "shadowManager.shadowMapDepthDistance", s.shadowMapDepthDistance); // 0x10080609, 0x1008067a, 0x100806c7
	bindBool(console, "shadowManager.shadowMapIncludeStaticMeshes", s.shadowMapIncludeStaticMeshes); // 0x10080716, 0x10080787, 0x100807d4
	bindBool(console, "shadowManager.shadowShowFrustums", s.shadowShowFrustums); // 0x1008082b, 0x1008089c, 0x100808e9
	bindFloat(console, "shadowManager.shadowIntensityBias", s.shadowIntensityBias); // 0x10080940, 0x100809b1, 0x100809fe
	bindFloat(console, "shadowManager.lightmapIntensityBias", s.lightmapIntensityBias); // 0x10080a4d, 0x10080abe, 0x10080b0b
	bindFloat(console, "shadowManager.dropShadowClipHeightOffset", s.dropShadowClipHeightOffset); // 0x10080b5a, 0x10080bcb, 0x10080c18
	bindBool(console, "shadowManager.dynamicShadowsEnabled", s.dynamicShadowsEnabled); // 0x10080c67, 0x10080cd8, 0x10080d25
	bindBool(console, "shadowManager.dynamicStaticMeshShadowsEnabled", s.dynamicStaticMeshShadowsEnabled); // 0x10080d7c, 0x10080ded, 0x10080e3a
	bindBool(console, "shadowManager.dynamic1pShadowsEnabled", s.dynamic1pShadowsEnabled); // 0x10080e91, 0x10080f02, 0x10080f4f
	bindBool(console, "shadowManager.allowAlphaShadows", s.allowAlphaShadows); // 0x10080fa6, 0x10081017, 0x10081064
	bindFloat(console, "shadowManager.alphaShadowThreshold", s.alphaShadowThreshold); // 0x100810bb, 0x1008112c, 0x10081179
	bindFloat(console, "shadowManager.shadowUpdateFactor", s.shadowUpdateFactor); // 0x100811c8, 0x10081239, 0x10081286
	bindBool(console, "shadowManager.shadowUpdatesEnabled", s.shadowUpdatesEnabled); // 0x100812d5, 0x10081346, 0x10081393
	bindFloat(console, "shadowManager.shadowMapDepthBias", s.shadowMapDepthBias); // 0x100813ea, 0x1008145b, 0x100814a8
	bindFloat(console, "shadowManager.shadowMapSlopeBias", s.shadowMapSlopeBias); // 0x100814f7, 0x10081568, 0x100815b5
	bindFloat(console, "shadowManager.shadowOccluderOversizeFactor", s.shadowOccluderOversizeFactor); // 0x10081604, 0x10081675, 0x100816c2
	bindBool(console, "shadowManager.useLSSMOptimizations", s.useLSSMOptimizations); // 0x10081711, 0x10081782, 0x100817cf
	bindBool(console, "shadowManager.usePSMOptimizations", s.usePSMOptimizations); // 0x10081826, 0x10081897, 0x100818e4
	bindBool(console, "shadowManager.shadowPSMUseSlideback", s.shadowPSMUseSlideback); // 0x1008193b, 0x100819ac, 0x100819f9
	bindBool(console, "shadowManager.shadowPSMUseUnitCubeClipping", s.shadowPSMUseUnitCubeClipping); // 0x10081a50, 0x10081ac1, 0x10081b0e
	bindFloat(console, "shadowManager.shadowPSMZNearOffset", s.shadowPSMZNearOffset); // 0x10081b65, 0x10081bd6, 0x10081c23
	bindFloat(console, "shadowManager.shadowPSMZFarOffset", s.shadowPSMZFarOffset); // 0x10081c72, 0x10081ce3, 0x10081d30
	bindFloat(console, "shadowManager.shadowPSMZMinInfinityZ", s.shadowPSMZMinInfinityZ); // 0x10081d7f, 0x10081df0, 0x10081e3d
	bindFloat(console, "shadowManager.shadowPSMFovY", s.shadowPSMFovY); // 0x10081e8c, 0x10081efd, 0x10081f4a
	bindFloat(console, "shadowManager.shadowTSMFocus", s.shadowTSMFocus); // 0x10081f99, 0x1008200a, 0x10082057
	bindFloat(console, "shadowManager.shadow80Line", s.shadow80Line); // 0x100820a6, 0x10082117, 0x10082164
	bindBool(console, "shadowManager.shadowMergeDynamicShadowFrustums", s.shadowMergeDynamicShadowFrustums); // 0x10082224, 0x10082271
	bindMethod(console, "VideoSettings.setDynamicShadowsQuality", kAccess,
		[&s](int quality) { s.quality = std::clamp(quality, 0, 3); });
	bindBool(console, "undergrowth.dynamicShadowsEnable", s.undergrowthShadows); // 0x10086249, 0x10086296
}

void registerHemiMapConsoleMethods(Console& console, HemiMapSettings& s)
{
	// HemiMapManager.setBaseHemiMap <texture> <corner> <size> [<height>]
	// (0x100857b8): the levels' Sky.con, e.g. "Levels/Camp_Gibraltar/groundhemi
	// 0/0/0 2048 500".
	bindMethod(console, "HemiMapManager.setBaseHemiMap", kAccess, // 0x1008589b, 0x1008594c
		[&s](std::string path, ConsoleVec3 corner, float size, float height) {
			s.baseHemiMap = path;
			s.offset = {corner.x, corner.y, corner.z};
			s.size = size;
			s.height = height;
			return true;
		},
		3);
	// HemiMapManager.loadTitanHemiMap <texture> <position> <size> <team> (0x10085535).
	bindMethod(console, "HemiMapManager.loadTitanHemiMap", kAccess, // 0x10085673
		[&s](std::string path, ConsoleVec3 position, float size, int team) {
			HemiMapSettings::TitanHemiMap& map = s.titans[static_cast<size_t>(std::clamp(team, 0, 2))];
			map = {path, {position.x, position.y, position.z}, size, team};
		});
	bindFloat(console, "HemiMapManager.hemiLerpBias", s.hemiLerpBias); // 0x10085d35, 0x10085da6, 0x10085df3
	bindBool(console, "HemiMapManager.useCurrentSetting", s.useCurrentSetting); // 0x10085ead, 0x10085efa
}

} // namespace dice::hfe::scene
