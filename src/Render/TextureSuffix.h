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
#include <vector>

namespace dice::hfe::render {

// The names a texture load tries in turn (RendDX9 0x10017340, from the 2D
// load 0x10017f20): a name with an extension as given, then, where that file
// is missing, its stem with ".dds" and the bare stem (the table 0x101f486c);
// a name without one, ".dds" first. So a HUD's "Soldier_021.tga" that only
// ships as Soldier_021.dds still loads.
inline std::vector<std::string> texturePathCandidates(const std::string& path)
{
	const size_t dot = path.rfind('.');
	if (dot == std::string::npos || path.find_first_of("/\\", dot) != std::string::npos) {
		return {path + ".dds", path};
	}
	const std::string stem = path.substr(0, dot);
	return {path, stem + ".dds", stem};
}

// Some mod meshes store bare texture names beside their Meshes directory.
// Resolve only those names; explicit virtual paths retain their meaning.
// The caller supplies existence checks so this also works with mounted ZIPs.
template <typename Exists>
inline std::string meshTexturePath(const std::string& path, std::string meshPath, Exists exists)
{
	if (path.empty() || path.find_first_of("/\\") != std::string::npos) return path;
	for (char& c : meshPath) {
		if (c == '\\') c = '/';
	}
	const size_t slash = meshPath.find_last_of('/');
	if (slash == std::string::npos) return path;
	const size_t parent = slash == 0 ? std::string::npos : meshPath.find_last_of('/', slash - 1);
	if (parent == std::string::npos) return path;
	std::string directory = meshPath.substr(parent + 1, slash - parent - 1);
	for (char& c : directory) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
	if (directory != "meshes") return path;
	// Preserve a directly resolvable name before trying the object's folder.
	for (const auto& candidate : texturePathCandidates(path)) if (exists(candidate)) return path;
	const std::string local = meshPath.substr(0, parent + 1) + "Textures/" + path;
	for (const auto& candidate : texturePathCandidates(local)) if (exists(candidate)) return local;
	return path;
}

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

// The texture load's name (0x10017f20 -> 0x10017340): the name as given
// when that file exists, else the name up to its extension (0x1019ee40)
// with ".dds", then with none. So the HUD's "mini_Medic.tga" (the map
// node's constructor, 0x00784cf0) loads mini_Medic.dds, the only one
// shipped. The fallback names to try after `path`, in order.
inline std::vector<std::string> textureFallbackPaths(const std::string& path)
{
	const size_t slash = path.find_last_of("/\\");
	const size_t dot = path.rfind('.');
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
		return {};
	}
	const std::string base = path.substr(0, dot);
	std::vector<std::string> out;
	if (path.size() - dot != 4 || (path.compare(dot, 4, ".dds") != 0 && path.compare(dot, 4, ".DDS") != 0)) {
		out.push_back(base + ".dds");
	}
	out.push_back(base);
	return out;
}

} // namespace dice::hfe::render
