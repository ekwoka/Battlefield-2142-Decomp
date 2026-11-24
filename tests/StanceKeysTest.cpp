#include "Game/StanceKeys.h"
#include "TestMain.h"

using namespace dice::hfe;

TEST(Viewer_LieKeyTogglesProneAndCrouchIsHeld)
{
	ProneToggle s;
	// A Z press lies down and stays down after the release (0x005889d0).
	s.update(true, false, false);
	CHECK(s.prone);
	s.update(false, false, false);
	CHECK(s.prone);
	// The next press gets up.
	s.update(true, false, false);
	CHECK(!s.prone);
	s.update(false, false, false);
	CHECK(!s.prone);

	// A crouch press from prone gets up; a crouch held from before the lie press doesn't.
	s.update(true, false, false);
	s.update(false, false, false);
	s.update(false, true, false);
	CHECK(!s.prone);
	s.update(true, true, false);
	CHECK(s.prone);
	s.update(false, true, false);
	CHECK(s.prone);

	// A jump press from prone gets up; Space held on doesn't stop lying down again.
	s.update(false, false, true);
	CHECK(!s.prone);
	s.update(true, false, true);
	s.update(false, false, true);
	CHECK(s.prone);
}
