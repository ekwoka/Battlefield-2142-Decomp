#pragma once

#include "Game/GameWorld.h"
#include "Hud/Hud.h"

namespace dice::hfe::menu {

inline hud::HudManager::MapMarker playerMapMarker(const game::GameWorld::PlayerInfo& player)
{
	return {"Ingame/Player/Icons/Minimap/mini_Soldier.dds", player.position.x, player.position.z, 16, player.yaw};
}

} // namespace dice::hfe::menu
