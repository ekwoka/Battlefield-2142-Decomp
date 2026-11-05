// texturemanager.customTextureSuffix: a level's texture variants.
//
// The row's executor (RendDX9 0x10053d80) stores the value in the texture
// manager (DAT_101f8e5c +0xf8) and raises its reload flag (+0x114). At the
// end of the level's load (0x1000deb0, after "Loading Textures..") the
// manager's reload pass (0x10018ca0) walks every loaded texture: with a
// suffix set it builds "<name up to its last '.'>_<suffix>.dds" (the base
// name 0x1019f4c0) and loads that instead (0x10017f20 with "don't fall back
// to wantu.dds" set, so a missing variant fails), marking the entry (+0x7c)
// as swapped; where no variant exists, or the suffix is empty, a swapped
// entry is loaded from its own name again. Names starting with "rad_" (IGA's
// ads, which 0x10017f20 loads from the GSRadialLevelIndex cache) are left
// alone.
//
// The shipped variants: the Northern Strike snow kits and vehicles
// ("Snow": eu_kits_b_snow.dds, as_apc_b_snow.dds, us_heavy_mech_c_snow.dds),
// Wake_Island_2142's billboards ("wake": billboard_color01_wake.dds), and
// the community Titan levels' "HEAT" soldiers, kits and handguns in their
// own archives.
#pragma once

#include <string>

namespace dice::hfe::render {

// The variant name 0x10018ca0 tries for `path`, or "" when it tries none
// (no suffix, or a "rad_" name).
inline std::string customSuffixedTexturePath(const std::string& path, const std::string& suffix)
{
	if (suffix.empty() || path.compare(0, 4, "rad_") == 0) {
		return {};
	}
	const size_t dot = path.rfind('.');
	const std::string base = dot == std::string::npos ? path : path.substr(0, dot);
	return base + "_" + suffix + ".dds";
}

} // namespace dice::hfe::render
