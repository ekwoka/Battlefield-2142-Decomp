#pragma once

#include "Game/GameWorld.h"
#include "Hud/Hud.h"
#include "World/Object.h"
#include "World/ObjectTemplate.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace dice::hfe::main {

struct SpawnGroupMarker {
	int id = -1;
	float x = 0, z = 0;
	bool selected = false;
	bool active = false;
};

inline std::vector<SpawnGroupMarker> spawnGroupMarkers(const std::vector<game::GameWorld::SpawnGroupInfo>& groups,
	int selected)
{
	std::vector<SpawnGroupMarker> markers;
	markers.reserve(groups.size());
	for (const auto& group : groups) {
		markers.push_back({group.id, group.position.x, group.position.z, group.id == selected, group.available});
	}
	return markers;
}

inline std::string spawnGroupControlPointLabel(const std::string& cpName)
{
	return cpName;
}

inline std::string spawnGroupVehicleLabel(const world::ObjectTemplate* vehicle)
{
	if (!vehicle) return {};
	const auto hudName = vehicle->settings().find("vehicleHud.hudName");
	std::string label = hudName == vehicle->settings().end() ? vehicle->name() : hudName->second;
	if (label.size() >= 2 && label.front() == '"' && label.back() == '"') {
		label = label.substr(1, label.size() - 2);
	}
	return label;
}

inline std::string spawnGroupTitanLabel(int team, const std::string& localizedCpName,
	const std::function<std::string(const std::string&)>& localize)
{
	const std::string titan = team == 1 ? "PAC Titan" : "EU Titan";
	return hud::titanSpawnLabel(titan, localize ? localize(localizedCpName) : localizedCpName);
}

inline std::string spawnGroupLabel(const game::GameWorld::SpawnGroupInfo& group,
	const std::vector<game::GameWorld::ControlPointInfo>& controlPoints,
	const std::vector<game::GameWorld::TitanInfo>& titans, const world::ObjectManager& objects,
	const std::function<std::string(const std::string&)>& localize)
{
	if (!group.mobileObject) {
		const auto cp = std::find_if(controlPoints.begin(), controlPoints.end(), [&group](const auto& point) {
			return point.id == group.controlPoint;
		});
		return cp == controlPoints.end() ? std::string() : spawnGroupControlPointLabel(cp->name);
	}
	const auto titan = std::find_if(titans.begin(), titans.end(), [&group](const auto& info) {
		return info.object == group.mobileObject;
	});
	if (titan != titans.end()) {
		const auto cp = std::find_if(controlPoints.begin(), controlPoints.end(), [titan](const auto& point) {
			return point.id == titan->controlPoint;
		});
		const std::string cpName = cp == controlPoints.end() ? std::string() : cp->name;
		return spawnGroupTitanLabel(titan->team, cpName, localize);
	}
	const world::Object* vehicle = objects.find(group.mobileObject);
	return spawnGroupVehicleLabel(vehicle ? vehicle->objectTemplate() : nullptr);
}

} // namespace dice::hfe::main
