// The minimap's combat-area layer (Bf2MenuMapNode, BF2142.exe 0x0078b490
// and its draw 0x0077d200).
//
// The map node loads Ingame/Minimap/map_CombatArea32.dds (constructor
// 0x00784cf0: +0x9e8 / +0x9e4; 512 x 512 A8R8G8B8, an even orange,
// (255, 163, 1) at alpha 0xc0) and each frame (0x00795620) asks
// 0x0078b490(1) whether the areas that apply to the local player changed:
// a bit per area of the manager's list (0x005a9ea0) that applies to him
// (0x004d9d50: his controlled object has seats, the area's team is 0, his
// own or he has none, and it binds his controlled object's vehicle
// category, 0 without one: CombatArea::binds), against the last mask
// (DAT_00b3c4a0). On a change it frames the map on them (the same call
// with 1: the bounds of the applying areas that don't bind category 0,
// else of all the areas that do, in the map's 512 pixels; the HUD's
// setMapArea) and redraws the layer (0x0078b490(0)) through RendDX9's
// texture operations (0x10187e70):
//   - 0x500: every texel's alpha 0;
//   - per applying area that doesn't bind category 0: its polygon (or box)
//     rasterised at 0x80 (0x0078a160 -> slot 0x70, in the map's
//     coordinates: x over the heightmap cluster's width, z flipped), then
//     0x402 ORs that into the alpha;
//   - per area that binds category 0 (whatever its team), the same at 0xff;
//   - 0x703: the alpha becomes (255 - alpha) x the texture's own (the
//     loaded alpha, kept by the texture: buffer 3) / 256. The colour stays.
// So the layer is the texture's orange where no area is, clear inside the
// areas every soldier must keep to, and half as strong inside areas only
// for the player's vehicles. 0x0077d200 draws it over the map at the map's
// alpha x the node's, under the markers (0x00795620: after the map's own
// layers, with the node's layer 1 shown, slot 0x104, and the layer built,
// +0xc24).
//
// Here the layer covers the whole map (the exe's covers the framed square
// at 512 x 512), rebuilt only when the mask changes, as a texture kept in
// the file manager's memory (a path per distinct mask), which the HUD
// draws with the map.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dice::hfe {
namespace game {
class CombatAreas;
}
namespace io {
class FileManager;
}
namespace hud {
class HudManager;
}
} // namespace dice::hfe

namespace dice::hfe::menu {

// The areas that apply to a player of `team` (0 without one) whose
// controlled object is of vehicle category `category` (0 on foot), a bit
// each (the first 32; DAT_00b3c4a0's mask; 0x004d9d50 per area).
uint32_t combatAreaMask(const game::CombatAreas& areas, int team, int category);
// The framed bounds (world x / z): false when no area frames it.
bool combatAreaBounds(const game::CombatAreas& areas, int team, int category, float& minX, float& minZ, float& maxX,
	float& maxZ);
// The 0..255 coverage per texel of a `size` x `size` grid over the map
// (origin, width, depth in metres; row 0 at the north edge, max z): 0x80
// in the applying areas that don't bind category 0, 0xff in those that do,
// ORed.
std::vector<uint8_t> combatAreaCoverage(const game::CombatAreas& areas, int team, int category, float originX,
	float originZ, float width, float depth, int size);
// The layer's DDS: the shipped texture (`dds`, uncompressed 32-bit) with
// each texel's alpha (255 - coverage) x its own / 256 (0x703), the
// coverage sampled over the texture's size. Empty when `dds` isn't one.
std::vector<uint8_t> combatAreaTexture(const std::vector<uint8_t>& dds, const std::vector<uint8_t>& coverage, int size);

// The layer for a level: kept up to date each frame, cheaply (the mask).
class CombatAreaLayer {
public:
	// The level's map (as HudManager::setMap) and a name for its textures.
	void setLevel(const std::string& level, float originX, float originZ, float width, float depth);
	// The frame: the local player's team and category; sets the HUD's map
	// area and layer when the mask changed. Without areas in use
	// (CombatAreaManager.use 0, or none) the layer and framing are off.
	void update(io::FileManager& files, const game::CombatAreas& areas, int team, int category, hud::HudManager& hud);
	const std::string& texture() const { return m_texture; }

private:
	std::string m_level;
	float m_origin[2] = {0, 0}, m_size[2] = {1, 1};
	bool m_built = false;
	uint32_t m_mask = 0;
	int m_team = -1, m_category = -1;
	size_t m_areaCount = 0;
	std::string m_texture;
	std::vector<uint8_t> m_shipped;
	bool m_shippedRead = false;
};

} // namespace dice::hfe::menu
