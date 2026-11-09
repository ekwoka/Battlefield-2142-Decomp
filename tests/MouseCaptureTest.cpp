#include <string>
#include "Main/MouseCapture.h"
#include "TestMain.h"

using namespace dice::hfe;

TEST(Viewer_MouseCaptureFollowsFocusAndMenuOwnership)
{
	// A gameplay window opened without focus acquires capture when focused.
	CHECK(!gameplayMouseCaptureWanted(true, false, false, false));
	CHECK(gameplayMouseCaptureWanted(true, false, false, true));

	// Focus loss and pointer/menu ownership release it; returning to focused
	// gameplay reacquires it without requiring another pointer transition.
	CHECK(!gameplayMouseCaptureWanted(true, false, false, false));
	CHECK(!gameplayMouseCaptureWanted(true, true, false, true));
	CHECK(!gameplayMouseCaptureWanted(true, false, true, true));
	CHECK(gameplayMouseCaptureWanted(true, false, false, true));
	CHECK(!gameplayMouseCaptureWanted(false, false, false, true));
}

TEST(Viewer_MouseCaptureRefreshesWhenSdlStateIsStale)
{
	// Wanted and SDL already agrees: leave it alone unless a refresh event arrived.
	CHECK(!mouseCaptureNeedsRelease(true, true, true, false));
	CHECK(!mouseCaptureNeedsAcquire(true, true, true));
	// Focus/fullscreen events cycle capture even though SDL still reports it on.
	CHECK(mouseCaptureNeedsRelease(true, true, true, true));
	CHECK(mouseCaptureNeedsAcquire(true, false, false));
	// Not wanted: release whatever is on; nothing to release when already off.
	CHECK(mouseCaptureNeedsRelease(false, true, false, false));
	CHECK(mouseCaptureNeedsRelease(false, false, true, false));
	CHECK(!mouseCaptureNeedsRelease(false, false, false, true));
	CHECK(!mouseCaptureNeedsAcquire(false, false, false));
	// Half-applied state (relative on, grab off) is completed.
	CHECK(mouseCaptureNeedsAcquire(true, true, false));
}

TEST(Viewer_MouseCaptureBlockerNamesTheFlag)
{
	CHECK(std::string(mouseCaptureBlocker(true, false, false, true)) == "none");
	CHECK(std::string(mouseCaptureBlocker(false, false, false, true)) == "not-playing");
	CHECK(std::string(mouseCaptureBlocker(true, true, true, false)) == "pointer");
	CHECK(std::string(mouseCaptureBlocker(true, false, true, false)) == "menu");
	CHECK(std::string(mouseCaptureBlocker(true, false, false, false)) == "unfocused");
}

TEST(MouseCapture_DriverPitchIsNotPinnedBySoldierLimit)
{
	using dice::hfe::clampLookPitch;
	CHECK(clampLookPitch(5.0f, false, false) == 1.0f && clampLookPitch(-5.0f, false, false) == -1.2f);
	// Holding mouse-up far past the soldier limit and then reversing still moves the aim.
	float pitch = clampLookPitch(5.0f, false, true);
	CHECK(pitch == 5.0f);
	CHECK(clampLookPitch(pitch + 0.1f, false, true) - pitch > 0.09f);
	CHECK(clampLookPitch(pitch - 0.1f, false, true) - pitch < -0.09f);
}

// The comm rose shows a pointer: the cursor stays grabbed in the window
// (relative mode off); the Esc menu and an unfocused window let it go.
TEST(MouseCapture_InGamePointerStaysInTheWindow)
{
	using namespace dice::hfe;
	CHECK(gameplayPointerConfined(true, true, false, true, "comm-rose"));
	CHECK(gameplayPointerConfined(true, true, false, true, "deployment"));
	CHECK(!gameplayPointerConfined(true, true, false, true, "esc-menu"));
	CHECK(!gameplayPointerConfined(true, true, false, true, "end-of-round"));
	CHECK(!gameplayPointerConfined(true, true, false, false, "comm-rose"));
	CHECK(!gameplayPointerConfined(true, true, true, true, "comm-rose"));
	// Opening the rose from capture: relative goes, the grab stays.
	CHECK(mouseCaptureNeedsRelease(false, true, true, false, true)); // relative on must go
	CHECK(!mouseCaptureNeedsRelease(false, false, true, false, true)); // grab alone is kept
	CHECK(mouseCaptureNeedsRelease(false, false, true, false, false)); // Esc menu: released
}
