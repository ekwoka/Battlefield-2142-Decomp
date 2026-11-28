#pragma once

#include "Game/GameWorld.h"
#include "Hud/Hud.h"
#include "Main/HudMenusVehicles.h"
#include "Main/HudWorld.h"
#include "World/Object.h"

#include <algorithm>
#include <array>
#include <unordered_set>
#include <vector>

namespace dice::hfe::main {

inline bool appendCommanderUavContacts(hud::HudManager& hud, const host::HudWorld& world, game::GameWorld& round, int team)
{
	physics::Vec3 center {};
	float radius = 0;
	const bool active = round.uavArea(team, center, radius);
	std::vector<hud::HudManager::UavTarget> targets;
	if ((team == 1 || team == 2) && (active || hud.hasUavContacts())) {
		std::unordered_set<uint32_t> seen;
		const int enemyTeam = team == 1 ? 2 : 1;
		for (const auto& player : world.playerList()) {
			if (!player.alive || player.manDown || player.team != enemyTeam) {
				continue;
			}
			const uint32_t id = player.vehicle ? player.vehicle : player.soldier;
			const world::Object* object = round.objects().find(id);
			if (!object || object->destroyed() || !seen.insert(id).second) {
				continue;
			}
			const world::ObjectTemplate* t = object->objectTemplate();
			if (menu::hudNumber(t, "vehicleHud.alwaysShowOnMinimap", 0) != 0
				|| menu::hudNumber(t, "vehicleHud.canBeSpotted", 1) == 0) {
				continue;
			}
			if (const auto marker = menu::radarContactMarker(round, player)) {
				targets.push_back({id, {marker->texture, marker->x, marker->z, marker->size, marker->yaw}});
			}
		}
	}
	return hud.appendUavContacts(team, round.roundTime(), center.x, center.z, radius, targets);
}

inline void appendCommanderMapMarkers(std::vector<hud::HudManager::MapMarker>& markers, game::GameWorld& round, int team)
{
	if (team != 1 && team != 2) {
		return;
	}

	physics::Vec3 uav;
	float uavRadius = 0;
	if (round.uavArea(team, uav, uavRadius)) {
		// 0x0078f060: a white (0x00a0bfc0) ring over the UAV's whole area, not its icon.
		markers.push_back({"Ingame/Minimap/icon_uav_circle.tga", uav.x, uav.z, 0});
		markers.back().worldSize = 2 * uavRadius;
		markers.back().radar = true;
		markers.back().upright = true;
	}

	// Retail uses the same tint on each tool's pulsing ring and 15 x 15 icon.
	// Both strike warnings use red (0x0078eb20 / 0x0078eea0, 0x00b3c440);
	// the friendly supply drop uses team blue (0x0078ece0, 0x00b3c280).
	const auto place = [&markers](const char* ring, float size, const char* icon, float x, float z,
						   const std::array<float, 4>& color) {
		hud::HudManager::MapMarker area {ring, x, z, size};
		area.upright = true;
		area.pulse = true;
		std::copy(color.begin(), color.end(), area.color);
		markers.push_back(area);
		hud::HudManager::MapMarker symbol {icon, x, z, 15};
		symbol.upright = true;
		std::copy(color.begin(), color.end(), symbol.color);
		markers.push_back(symbol);
	};

	constexpr std::array<float, 4> strikeRed {1, 0, 0, 1};
	for (const auto& strike : round.strikes()) {
		if (strike.team != team) {
			continue;
		}
		if (strike.ability == game::GameWorld::CommanderAbility::OrbitalStrike) {
			place("Ingame/Minimap/Icons/placedAirstrikeLarge.tga", 116, "Ingame/Commander/tools/orbit_strike.tga",
				strike.target.x, strike.target.z, strikeRed);
		} else {
			place("Ingame/Minimap/Icons/placedAirstrike.tga", 58, "Ingame/Commander/tools/force_strike.tga",
				strike.target.x, strike.target.z, strikeRed);
		}
	}

	// Only the dropping team receives HUD event 0x53 (0x0043d1f0 -> 0x0043cf80).
	const auto supplyBlue = menu::mapRelationColor(menu::MapRelation::Friendly);
	for (const uint32_t crate : round.supplyCrates()) {
		const float age = round.supplyCrateAge(crate);
		if (const world::Object* object = round.objects().find(crate); object && object->team() == team && age >= 0 && age < 25) {
			place("Ingame/Minimap/Icons/placedAirstrike.tga", 58, "Ingame/Minimap/Icons/icon_supplies.tga",
				object->position().x, object->position().z, supplyBlue);
		}
	}
}

} // namespace dice::hfe::main
