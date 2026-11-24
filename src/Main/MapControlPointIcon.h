#pragma once

#include "Game/GameWorld.h"

#include <string>

namespace dice::hfe::main {

// A control point's map flag texture (the map's vt+0x13c, BF2142.exe
// 0x0077dbf0). Outside Assault Lines a point that can't change hands is a
// base. In Assault Lines (0x0078ce50 when the mode name holds "sl") a point
// the other team may not take (allowCaptureByTeam off) shows the Locked flag
// (UnLocked for a base); a point whose template lists supply groups for one
// team only shows them the other way round when the other team may take it.
// Neutral points show the plain neutral flag.
inline std::string mapControlPointIcon(const game::GameWorld::ControlPointInfo& point, const std::string& teamFolder,
	bool assaultLines)
{
	const std::string prefix = "Ingame/Flags/Icons/Minimap/";
	const bool base = !point.capturable;
	if (assaultLines && point.team == 0) {
		return prefix + "Neutral/miniMap_CP.tga";
	}
	// shortcut: a point listing supply groups for both teams takes 0x0078d955's
	// plain flag only when it is in the map's supply-line sets (+0xca8/+0xcb4);
	// no shipped Assault Lines layer has one, so it is treated as listing none.
	const bool locked = assaultLines && !point.allowCapture[point.team == 1 ? 2 : 1];
	const bool oneSided = assaultLines && point.needsSupply[1] != point.needsSupply[2];
	if (locked || oneSided) {
		return prefix + teamFolder + (locked != base ? "/miniMap_CPLocked.tga" : "/miniMap_CPUnLocked.tga");
	}
	return prefix + teamFolder + (base ? "/miniMap_CPBase.tga" : "/miniMap_CP.tga");
}

} // namespace dice::hfe::main
