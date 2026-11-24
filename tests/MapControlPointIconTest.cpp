#include "Main/MapControlPointIcon.h"
#include "TestMain.h"

using namespace dice::hfe;

TEST(Map_AssaultLinesHomeBaseShowsLockUntilEnemyHasSupply)
{
	const std::string dir = "Ingame/Flags/Icons/Minimap/";
	// Highway Tampa's gpm_sl base 201: team 1's, needs team 2's supply only.
	game::GameWorld::ControlPointInfo base {};
	base.team = 1;
	base.needsSupply[2] = true;
	base.allowCapture[2] = false;
	CHECK(main::mapControlPointIcon(base, "EU", true) == dir + "EU/miniMap_CPLocked.tga");
	// Team 2 holds every group it needs (gpm_sl.py sets allowCaptureByTeam 2).
	base.allowCapture[2] = true;
	CHECK(main::mapControlPointIcon(base, "EU", true) == dir + "EU/miniMap_CPUnLocked.tga");
	// Conquest keeps the plain flags.
	base.allowCapture[2] = false;
	CHECK(main::mapControlPointIcon(base, "EU", false) == dir + "EU/miniMap_CP.tga");

	// A middle point needing no supply: the plain flag; neutral: the neutral flag.
	game::GameWorld::ControlPointInfo middle {};
	middle.team = 2;
	CHECK(main::mapControlPointIcon(middle, "Pac", true) == dir + "Pac/miniMap_CP.tga");
	middle.capturable = false;
	CHECK(main::mapControlPointIcon(middle, "Pac", true) == dir + "Pac/miniMap_CPBase.tga");
	middle.allowCapture[1] = false;
	CHECK(main::mapControlPointIcon(middle, "Pac", true) == dir + "Pac/miniMap_CPUnLocked.tga");
	middle.team = 0;
	CHECK(main::mapControlPointIcon(middle, "Neutral", true) == dir + "Neutral/miniMap_CP.tga");
	CHECK(main::mapControlPointIcon(middle, "Neutral", false) == dir + "Neutral/miniMap_CPBase.tga");
}
