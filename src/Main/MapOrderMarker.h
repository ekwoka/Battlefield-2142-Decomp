#pragma once

#include "Hud/Hud.h"

#include <algorithm>
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
	// 0x0077c610 turns an icon by (+0x28 - turn), then with the map by +turn:
	// on screen it shows +0x28, which 0x0078b110 never sets.
	marker.upright = true;
	return marker;
}

// The order's line (layer 8): map_Line.tga 8 wide (0x0078797f: +0x6f4) from the squad's soldier to the order's
// point, drawn by 0x00791090 under the pin. 0x0078b110 colours it: the record's accepted byte (+0x2c, the leader's
// answer, 0x004c2f60) set takes the kind's colour from the table 0x00b3c7b8 (static init 0x008f3dd0) at the map's icon
// alpha, unset (0.7, 0.7, 0.7) at a fixed 0.8.
inline std::optional<hud::HudManager::MapMarker> mapOrderLine(int kind, bool accepted, float fromX, float fromZ, float toX, float toZ)
{
	static const float kColors[6][3] = {{1, 0.611f, 0}, {0.792f, 0.2f, 0.949f}, {0.792f, 0.2f, 0.949f}, {1, 0.611f, 0},
		{1, 0.611f, 0}, {0.757f, 0.2196f, 0.196f}};
	if (kind < 4 || kind > 9) {
		return std::nullopt;
	}
	hud::HudManager::MapMarker line {"Ingame/Minimap/map_Line.tga", fromX, fromZ, 8};
	line.line = true;
	line.toX = toX;
	line.toZ = toZ;
	if (accepted) {
		std::copy(kColors[kind - 4], kColors[kind - 4] + 3, line.color);
	} else {
		line.color[0] = line.color[1] = line.color[2] = 0.7f;
		line.color[3] = 0.8f;
		line.iconAlpha = false;
	}
	return line;
}

} // namespace dice::hfe::main
