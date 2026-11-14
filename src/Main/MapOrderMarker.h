#pragma once

#include "Hud/Hud.h"

#include <optional>
#include <string>

namespace dice::hfe::main {

// The map-order callback at BF2142.exe 0x0078b110 indexes the ordinary or
// commander texture table by the placed order kind. Kinds 4..9 are move,
// attack, defend, repair, destroy and mine; 10+ cancels the armed order.
inline std::optional<hud::HudManager::MapMarker> mapOrderMarker(int kind, bool fromCommander, float x, float z)
{
	const char* name = nullptr;
	switch (kind) {
	case 4: name = "Move"; break;
	case 5: name = "Attack"; break;
	case 6: name = "Defend"; break;
	case 7: name = "Repair"; break;
	case 8: name = "Destroy"; break;
	case 9: name = "Mine"; break;
	default: return std::nullopt;
	}

	hud::HudManager::MapMarker marker {
		std::string("Ingame/Minimap/Icons/order") + name + (fromCommander ? "Commander.tga" : ".tga"), x, z, 15};
	// The retail leader and commander textures are 15x31 and 15x47. The
	// commander callback supplies 15x47 HUD units; the leader texture uses
	// its native 15x31 dimensions. Both are centered on the projected point.
	marker.height = fromCommander ? 47 : 31;
	return marker;
}

} // namespace dice::hfe::main
