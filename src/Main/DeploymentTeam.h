#pragma once

#include "Game/GameLogic.h"
#include "Game/GameWorld.h"

namespace dice::main {

// Change an undeployed local player, retaining the selected kit index and
// discarding the old team's spawn choice. A living player stays on their team.
inline bool selectDeploymentTeam(hfe::game::GameWorld& round, const hfe::game::GameLogic& logic, int team, int& kitIndex)
{
	if ((team != 1 && team != 2) || team == round.playerTeam()) return false;
	const int player = round.localPlayer();
	if (!round.setPlayerTeam(player, team)) return false;
	const auto& kits = logic.team(team).kits;
	if (!kits.empty()) {
		if (kitIndex < 0 || static_cast<size_t>(kitIndex) >= kits.size()) kitIndex = 0;
		round.setKit(player, kits[static_cast<size_t>(kitIndex)].name, kits[static_cast<size_t>(kitIndex)].soldier);
	}
	const auto points = round.spawnablePoints(team);
	round.setSpawnChoice(player, points.empty() ? -1 : points.front());
	return true;
}

} // namespace dice::main
