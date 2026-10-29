// Joysticks and gamepads: the control maps' IDFGameController_0..7
// devices (BF2142.exe's DirectInput 8 game controller,
// dice.hfe.io.InputDeviceManager.DX8's devices with vtable 0x00990f60).
//
// The original enumerates the attached game controllers (EnumDevices
// class 4, 0x006ab430), sets each axis' range to -1000..1000 (0x006ab320)
// and polls DIJOYSTATE2 each frame (0x006aaef0):
//   axis i = raw * 0.001 * sensitivity[i]     (X, Y, Z, Rx, Ry, Rz and two
//                                              sliders, the ones present)
//   buttons: one bit each, then four per POV hat after them (up, right,
//            down, left; only the four exact directions, no diagonals)
// and the control maps read
//   getAxis(i) = offset[i] + axis i, or offset[i] - axis i when inverted
//                                             (0x006aadc0)
// No dead zone is set (DIPROP_DEADZONE is never written, so DirectInput's
// default 0 applies). Per-axis sensitivity (default 1), offset (0) and
// inversion (off) are the device's slots 15-20 (0x006aad00..0x006aad60).
//
// The controls page's "press the input to bind" (0x006aadf0) takes the
// first button held, else an axis pushed past 0.95 after it was back under
// 0.2, with its direction.
//
// This class is the devices' state only; the window (SDL3's joystick
// API, SdlGameControllers.h) feeds it, and tests can feed it directly.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dice::hfe::input {

class GameControllers {
public:
	static constexpr int kMaxDevices = 8; // IDFGameController_0..7
	static constexpr int kMaxAxes = 8;    // DIJOYSTATE2's lX..rglSlider[1]
	static constexpr int kMaxButtons = 32;

	struct Device {
		bool connected = false;
		std::string name;
		uint64_t id = 0; // the platform's handle (SDL_JoystickID)
		int axisCount = 0, buttonCount = 0, povCount = 0;
		std::array<float, kMaxAxes> axis {};        // raw * 0.001 * sensitivity
		std::array<float, kMaxAxes> sensitivity {}; // 1
		std::array<float, kMaxAxes> offset {};      // 0
		std::array<bool, kMaxAxes> inverted {};
		std::array<int, 4> pov {{-1, -1, -1, -1}}; // hundredths of a degree, -1 centred
		uint32_t rawButtons = 0;   // the buttons alone
		uint32_t buttons = 0;      // with the POV hats' four each
		uint32_t previous = 0;     // last frame's
		std::array<bool, kMaxAxes> rested {}; // back under 0.2 since its last pick
	};

	GameControllers();

	// Hot-plugging: a device takes the lowest free slot (its
	// IDFGameController_N), -1 when all eight are taken; removing frees it.
	int add(uint64_t id, const std::string& name, int axes, int buttons, int povs);
	void remove(uint64_t id);
	int slotOf(uint64_t id) const;
	int count() const;
	const Device& device(int slot) const { return m_devices[static_cast<size_t>(slot)]; }
	Device& device(int slot) { return m_devices[static_cast<size_t>(slot)]; }

	// Raw state as the driver reports it: an axis -1000..1000, a button,
	// a hat in hundredths of a degree (-1 centred).
	void setAxis(int slot, int axis, int raw);
	void setButton(int slot, int button, bool down);
	void setPov(int slot, int pov, int hundredths);
	// A new frame: the buttons pressed last frame become the previous ones.
	void beginFrame();

	// What the control maps read.
	float axis(int slot, int axis) const; // 0x006aadc0
	// A hat's angle in hundredths of a degree, -1 centred (slot 0x6c, read
	// by the POV-to-axis mapping 0x006807f0).
	int pov(int slot, int pov) const;
	bool down(int slot, int button) const;
	bool pressed(int slot, int button) const;  // down now, not last frame
	bool released(int slot, int button) const; // the other way
	int buttonCount(int slot) const;           // 0x006ab270: buttons + 4 per hat

	void setSensitivity(int slot, int axis, float value);
	void setOffset(int slot, int axis, float value);
	void setInverted(int slot, int axis, bool inverted);

	// The controls page's last input (0x006aadf0): "IDFGameController_N"
	// with "IDButton_K", or "IDAxis_K" and its direction (-1 / +1).
	struct Input {
		std::string device, control;
		float direction = 1;
	};
	std::optional<Input> lastInput();

	// "IDFGameController_3" -> 3 (-1 for other names).
	static int slotNamed(const std::string& device);
	static std::string deviceName(int slot);

private:
	void updateButtons(Device& d);
	std::array<Device, kMaxDevices> m_devices;
};

} // namespace dice::hfe::input
