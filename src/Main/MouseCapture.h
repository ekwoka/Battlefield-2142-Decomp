// The native viewer captures only while gameplay owns an active, focused
// window. Kept independent of SDL so the lifecycle policy can be regression
// tested without opening a window.
#pragma once

namespace dice::hfe {

constexpr bool gameplayMouseCaptureWanted(bool playing, bool pointer, bool menuUp, bool focused)
{
	return playing && !pointer && !menuUp && focused;
}

// A mouse press between two frames is kept for the frame's raw buttons, pointer screens
// included: the dead and man-down screens are pointer screens, and their right-click
// (open the spawn screen) or left-click (call a medic) must not be lost when press and
// release fall within one frame. The mapped game buttons still ignore it under a pointer.
constexpr bool latchMousePress(bool playing, bool menuUp)
{
	return playing && !menuUp;
}
// Why capture is not wanted, for the always-on log line ("none" when it is).
constexpr const char* mouseCaptureBlocker(bool playing, bool pointer, bool menuUp, bool focused)
{
	return !playing ? "not-playing" : pointer ? "pointer" : menuUp ? "menu" : !focused ? "unfocused" : "none";
}

// An in-game pointer screen (comm rose, deployment, commander screen) shows the
// cursor but keeps it inside the focused window; the Esc menu and the round's
// end let it go, as does losing focus.
constexpr bool sameText(const char* a, const char* b)
{
	return *a == *b && (*a == 0 || sameText(a + 1, b + 1));
}

constexpr bool gameplayPointerConfined(bool playing, bool pointer, bool menuUp, bool focused, const char* reason)
{
	return playing && pointer && !menuUp && focused && reason && !sameText(reason, "esc-menu")
		&& !sameText(reason, "end-of-round");
}

// Full screen the window is the whole display, so a free or confined cursor reaches
// the edges and corners where macOS fires hot corners, reveals the Dock and hands the
// pointer to another Mac (Universal Control). While focused, every screen (menus too)
// keeps it this many points inside; windowed play is unchanged.
constexpr int fullscreenEdgeInset(bool fullscreen, bool focused)
{
	return fullscreen && focused ? 4 : 0;
}

// Screens that draw the game's own pointer (the commander screen's
// pointerMinimap.tga, HudManager.setMouseTextureFull) or none (the comm rose,
// picked by pointing) hide the system cursor while it is confined there.
constexpr bool gameplaySystemCursorHidden(bool confined, const char* reason)
{
	return confined && reason && (sameText(reason, "comm-rose") || sameText(reason, "commander-screen"));
}

// Apply one visibility decision after capture and confinement are updated.
// Showing a pointer first, then hiding only on a rose transition, makes the
// system cursor reappear on the second frame of a held Q/T menu.
constexpr bool gameplaySystemCursorVisible(bool capture, bool confined, const char* reason)
{
	return !capture && !gameplaySystemCursorHidden(confined, reason);
}

// The soldier's aim pitch is clamped (85 degrees first person), but a driver's
// PIPitch/PIMouseLookY is the per-tick change of that pitch, so a clamp pinned
// at one end swallows the mouse in that direction. Driving only bounds the sum.
constexpr float clampLookPitch(float pitch, bool firstPerson, bool driving)
{
	const float low = driving ? -1000.0f : firstPerson ? -1.4835f : -1.2f;
	const float high = driving ? 1000.0f : firstPerson ? 1.4835f : 1.0f;
	return pitch < low ? low : pitch > high ? high : pitch;
}

} // namespace dice::hfe
