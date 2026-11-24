#pragma once

#include "GameLogic.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

namespace dice::hfe::game {

// The retail Unlock event carries one byte per team, kit and branch.
using UnlockMasks = std::array<uint8_t, 2 * 7 * 4>;

inline int unlockBranch(const std::string& value)
{
	if (value == "UB_Second" || value == "1") return 1;
	if (value == "UB_Player" || value == "2") return 2;
	if (value == "UB_SquadLeader" || value == "3") return 3;
	return 0;
}

inline int unlockBranch(const world::ObjectTemplate& container)
{
	const auto at = container.settings().find("unlockBranch");
	return at == container.settings().end() ? 0 : unlockBranch(at->second);
}

inline int unlockLevel(const world::ObjectTemplate& container)
{
	const auto at = container.settings().find("unlockLevel");
	return at == container.settings().end() ? 0 : std::atoi(at->second.c_str());
}

inline size_t unlockMaskIndex(int team, size_t kit, int branch)
{
	return ((team - 1) * 7 + kit) * 4 + branch;
}

template<class Visit>
void visitKitUnlocks(const world::ObjectTemplateManager& templates, const Team& team, size_t kit, Visit visit)
{
	const auto* root = kit < team.kits.size() ? templates.find(team.kits[kit].name) : nullptr;
	if (!root) return;
	for (const auto& child : root->children()) {
		const auto* container = templates.find(child.name);
		if (!container || container->typeName() != "ItemContainer" || container->children().empty()) continue;
		const int level = unlockLevel(*container);
		if (level >= 0 && level < 8) visit(child.name, unlockBranch(*container), level);
	}
}

inline UnlockMasks kitUnlockMasks(const world::ObjectTemplateManager& templates, const GameLogic& logic,
	const std::vector<std::string>& names, bool all = false)
{
	UnlockMasks masks {};
	for (int team : {1, 2}) {
		for (size_t kit = 0; kit < 7; ++kit) {
			visitKitUnlocks(templates, logic.team(team), kit, [&](const std::string& name, int branch, int level) {
				if (all || std::find(names.begin(), names.end(), name) != names.end()) {
					masks[unlockMaskIndex(team, kit, branch)] |= 1u << level;
				}
			});
		}
	}
	return masks;
}

inline std::vector<std::string> kitUnlockNames(const world::ObjectTemplateManager& templates, const GameLogic& logic,
	const UnlockMasks& masks)
{
	std::vector<std::string> names;
	for (int team : {1, 2}) {
		for (size_t kit = 0; kit < 7; ++kit) {
			visitKitUnlocks(templates, logic.team(team), kit, [&](const std::string& name, int branch, int level) {
				if ((masks[unlockMaskIndex(team, kit, branch)] & (1u << level)) != 0 &&
					std::find(names.begin(), names.end(), name) == names.end()) {
					names.push_back(name);
				}
			});
		}
	}
	return names;
}

} // namespace dice::hfe::game
