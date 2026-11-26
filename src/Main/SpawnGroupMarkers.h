#pragma once

#include "Game/GameWorld.h"

#include <vector>

namespace dice::hfe::main {

struct SpawnGroupMarker {
	int id = -1;
	float x = 0, z = 0;
	bool selected = false;
};

inline std::vector<SpawnGroupMarker> spawnGroupMarkers(const std::vector<game::GameWorld::SpawnGroupInfo>& groups,
	int selected)
{
	std::vector<SpawnGroupMarker> markers;
	markers.reserve(groups.size());
	for (const auto& group : groups) {
		markers.push_back({group.id, group.position.x, group.position.z, group.id == selected});
	}
	return markers;
}

} // namespace dice::hfe::main
