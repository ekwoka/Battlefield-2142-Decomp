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

TEST(Viewer_MouseCaptureChangesOnlyWithGameplayOwnership)
{
	// Window shown/restored/fullscreen/display-size changes don't alter ownership.
	CHECK(gameplayMouseCaptureWanted(true, false, false, true));
	CHECK(gameplayMouseCaptureWanted(true, false, false, true));
	// Focus loss releases; focus return reacquires once wanted changes back.
	CHECK(!gameplayMouseCaptureWanted(true, false, false, false));
	CHECK(gameplayMouseCaptureWanted(true, false, false, true));
	// Opening a pointer screen releases relative mode while it remains focused.
	CHECK(!gameplayMouseCaptureWanted(true, true, false, true));
	CHECK(gameplayMouseCaptureWanted(true, false, false, true));
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

// The comm rose and other in-game pointer screens confine the cursor;
// menus at which the game yields the mouse leave it free.
TEST(MouseCapture_InGamePointerStaysInTheWindow)
{
	using namespace dice::hfe;
	CHECK(gameplayPointerConfined(true, true, false, true, "comm-rose"));
	CHECK(gameplayPointerConfined(true, true, false, true, "deployment"));
	CHECK(!gameplayPointerConfined(true, true, false, true, "esc-menu"));
	CHECK(!gameplayPointerConfined(true, true, false, true, "end-of-round"));
	CHECK(!gameplayPointerConfined(true, true, false, false, "comm-rose"));
	CHECK(!gameplayPointerConfined(true, true, true, true, "comm-rose"));
	CHECK(!gameplayMouseCaptureWanted(true, true, false, true)); // relative mode is off on the rose
	CHECK(!gameplayPointerConfined(true, true, false, false, "deployment")); // focus loss frees it
}

// The commander screen draws the game's pointer, so the system cursor hides
// there (and on the rose) while confined; other pointer screens keep it.
TEST(MouseCapture_CommanderScreenHidesSystemCursor)
{
	using namespace dice::hfe;
	CHECK(gameplaySystemCursorHidden(true, "commander-screen"));
	CHECK(gameplaySystemCursorHidden(true, "comm-rose"));
	CHECK(!gameplaySystemCursorHidden(true, "deployment"));
	CHECK(!gameplaySystemCursorHidden(false, "commander-screen")); // unfocused or Esc menu
	CHECK(!gameplaySystemCursorHidden(true, nullptr));
}
