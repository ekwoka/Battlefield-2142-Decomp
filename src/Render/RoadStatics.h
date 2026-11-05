// Road static meshes: static templates under objects/roads/ drawn as roads.
//
// The static template's loader (RendDX9 0x100de6d0) flags a template by its
// geometry's path with a case-blind find (0x1019eb80): "objects/roads/"
// (+0x2a0) and, for such a road, "noBlend" (+0x2a1). The static mesh's draw
// (0x100bdb00) draws a road template only with renderer.drawRoads (+0x693),
// whatever drawStaticMeshes says, and gives each material the technique key
// (registered with its RaShader*.fx by 0x100db190): no detail map (the
// material's +0xe0) is Road (0xf1000000), one with it RoadDetail
// (0xf2000000), or RoadDetailNoBlend (0xf4000000) with +0x2a1. The material
// goes to the road queue with the world matrix and the screen-space light
// buffer as its "LightMap" (the light manager's slot 0x5c).
//
// Shipped: the 3 objects/roads/statics/ intersections (Road) and the 8
// objects/roads/staticsnoblend/ concrete blocks under the airstrips and
// helipads (RoadDetailNoBlend), on Wake_Island_2142, Strike_at_Karkand,
// tgw_sharqi_peninsula and delhi_river.
#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace dice::hfe::render {

inline bool pathHas(const std::string& path, const char* part)
{
	std::string lowered = path;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	std::replace(lowered.begin(), lowered.end(), '\\', '/');
	return lowered.find(part) != std::string::npos;
}

// road.frag's technique (RoadUniforms::road[3]).
enum class RoadTechnique { Compiled = 0, Road = 1, RoadDetail = 2, RoadDetailNoBlend = 3 };

// The template's +0x2a0 and +0x2a1 from its geometry's path.
inline bool roadStaticPath(const std::string& path) { return pathHas(path, "objects/roads/"); }
inline bool roadStaticNoBlend(const std::string& path) { return roadStaticPath(path) && pathHas(path, "noblend"); }

// 0x100bdb00's key for a road template's material.
inline RoadTechnique roadStaticTechnique(bool hasDetailMap, bool noBlend)
{
	if (!hasDetailMap) {
		return RoadTechnique::Road;
	}
	return noBlend ? RoadTechnique::RoadDetailNoBlend : RoadTechnique::RoadDetail;
}

} // namespace dice::hfe::render
