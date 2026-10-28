// SDL3's joysticks and gamepads into GameControllers (the window's side of
// IDFGameController_0..7).
//
// The original sees every game controller through DirectInput 8
// (0x006ab430). Here SDL3 opens each device as it is attached
// (SDL_EVENT_JOYSTICK_ADDED, also for the ones attached at start) and
// frees its slot when it goes (SDL_EVENT_JOYSTICK_REMOVED), so devices can
// come and go while the game runs.
//
//   A device SDL knows as a gamepad (SDL_IsGamepad) is read through the
//   gamepad API and laid out as DirectInput shows an Xbox 360 pad:
//   IDAxis_0/1 the left stick, IDAxis_2 the triggers together (left
//   positive, right negative), IDAxis_3/4 the right stick; IDButton_0..9
//   A, B, X, Y, the left and right shoulders, back, start, the left and
//   right stick clicks; the d-pad as the POV hat (IDButton_10..13 up,
//   right, down, left). This layout is the rewrite's choice, so that
//   bindings are the same on every platform.
//   Any other joystick: its own axes, buttons and hats in SDL's order.
//
// SDL's -32768..32767 axes become DirectInput's -1000..1000 range.
// Without hardware, SDL_AttachVirtualJoystick devices go the same way
// (the tests use them).
#pragma once

#include <cstdint>
#include <map>

union SDL_Event;

namespace dice::hfe::input {

class GameControllers;

class SdlGameControllers {
public:
	explicit SdlGameControllers(GameControllers& controllers);
	~SdlGameControllers();
	SdlGameControllers(const SdlGameControllers&) = delete;
	SdlGameControllers& operator=(const SdlGameControllers&) = delete;

	// Starts SDL's joystick and gamepad subsystems and opens the devices
	// already attached (false when SDL could not start them).
	bool start();
	// A window event: true when it was a joystick's or gamepad's.
	bool handle(const SDL_Event& event);

	// SDL's -32768..32767 as -1000..1000.
	static int rawAxis(int sdlValue);

private:
	void open(uint32_t instance);
	void close(uint32_t instance);
	GameControllers& m_controllers;
	struct Opened {
		void* joystick = nullptr; // SDL_Joystick
		void* gamepad = nullptr;  // SDL_Gamepad (when it is one)
		int lt = 0, rt = 0;       // the triggers, for IDAxis_2
		uint8_t dpad = 0;         // up 1, right 2, down 4, left 8
	};
	std::map<uint32_t, Opened> m_opened;
	bool m_started = false;
};

} // namespace dice::hfe::input
