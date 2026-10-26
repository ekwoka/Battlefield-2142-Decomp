// The level's combat areas (the "CombatArea" and "CombatAreaManager"
// console objects of BF2142.exe, 0x00428af8-0x00429aa9; the manager is
// DAT_00a25134, its areas at +8..+0xc). The game mode's GamePlayObjects.con
// creates them:
//   CombatAreaManager.use 1
//   CombatArea.create CombatArea_141_Titans
//   CombatArea.min 0/0                 (a box when it has no points)
//   CombatArea.max 0/0
//   CombatArea.team 0
//   CombatArea.vehicles 4              (which vehicles it binds)
//   CombatArea.layer 6
//   CombatArea.addAreaPoint 704/251    (a polygon from the first point on)
//
// The rewrite uses them for the Titans' bounds (0x005ab740, Titans.cpp);
// the soldiers' and vehicles' out-of-bounds timer is not rewritten.
#pragma once

#include <string>
#include <vector>

namespace dice::hfe {
class Console;
}

namespace dice::hfe::game {

struct CombatArea {
	std::string name;
	bool polygon = false; // area +0 (0x004d9c70 tests the box, 0x004d9f00 the polygon)
	float minX = 0, minZ = 0, maxX = 0, maxZ = 0;
	std::vector<std::pair<float, float>> points; // x, z
	int team = 0;
	int vehicles = 0; // +0x14
	int layer = 0;
	bool usedByPathFinding = false;
	bool inverted = false; // +0x38: an area cut out of the others (no console property sets it)

	// 0x004d9cc0: whether the area binds a vehicle of category `category`:
	// vehicles 4 binds everything, 5 categories 2 and 3, otherwise only its own.
	bool binds(int category) const
	{
		return vehicles == 4 || vehicles == category || (vehicles == 5 && (category == 2 || category == 3));
	}
	bool contains(float x, float z) const;
	// The distance from a point to the area's border and the unit direction
	// from the point towards the nearest border point (x, z).
	float borderDistance(float x, float z, float& dirX, float& dirZ) const;
};

class CombatAreas {
public:
	bool use = false; // CombatAreaManager.use
	std::vector<CombatArea> areas;

	// 0x004ca7d0 over the areas binding `category`: inside one of them (or
	// anywhere when none binds it) and inside no cut-out.
	bool inside(float x, float z, int category) const;
	// 0x004cad50: the nearest border of those areas within `radius` of the
	// point (false when none is that near).
	bool nearestBorder(float x, float z, float radius, int category, float& distance, float& dirX, float& dirZ) const;
	bool any(int category) const;
};

// CombatArea.* and CombatAreaManager.* console methods.
void registerCombatAreaConsoleMethods(Console& console, CombatAreas& areas);

} // namespace dice::hfe::game
