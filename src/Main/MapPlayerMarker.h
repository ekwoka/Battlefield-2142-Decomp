#pragma once

#include "Game/GameWorld.h"
#include "Hud/Hud.h"

namespace dice::hfe::menu {

inline hud::HudManager::MapMarker playerMapMarker(const game::GameWorld::PlayerInfo& player)
{
	return {"Ingame/Player/Icons/Minimap/mini_Soldier.dds", player.position.x, player.position.z, 16, player.yaw};
}

// BF2142.exe 0x0078e510: the local camera's view indicator (+0xa24),
// separate from soldier/vehicle icons. 0x0078e53a sets 50 x 70;
// 0x0078e795 tints it (1, 247/255, 0, iconAlpha). The heading arrow
// (+0xa20) belongs to leader/commander player icons, not this layer.
inline hud::HudManager::MapMarker localViewMapMarker(const game::GameWorld::PlayerInfo& player, float viewYaw)
{
	hud::HudManager::MapMarker marker {
		"Ingame/Player/Icons/Minimap/mini_LocalArrow2.tga", player.position.x, player.position.z, 50, viewYaw};
	marker.height = 70;
	marker.color[1] = 247.0f / 255.0f;
	marker.color[2] = 0;
	marker.clamp = true;
	return marker;
}

} // namespace dice::hfe::menu
