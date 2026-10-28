// Joysticks and gamepads (Input/GameControllers.h): slots and hot-plugging,
// axes with sensitivity, offset and inversion, POV hats as buttons, the
// controls page's last input, control maps bound to controller buttons
// and axes (and written back), and, with SDL3, virtual SDL joysticks and
// gamepads fed through SdlGameControllers.
#include "Console/Console.h"
#include "Input/ControlMap.h"
#include "Input/GameControllers.h"
#include "TestMain.h"

#if BF2142_HAS_SDL_INPUT
#include "Input/SdlGameControllers.h"
#include <SDL3/SDL.h>
#endif

#include <cmath>
#include <string>

using namespace dice::hfe;

namespace {

bool near(float a, float b)
{
	return std::fabs(a - b) < 1e-4f;
}

} // namespace

TEST(GameControllers_Slots)
{
	input::GameControllers pads;
	CHECK(pads.add(11, "stick", 3, 8, 1) == 0);
	CHECK(pads.add(12, "pad", 5, 10, 1) == 1);
	CHECK(pads.add(11, "stick", 3, 8, 1) == 0); // already there
	CHECK(pads.count() == 2);
	pads.remove(11);
	CHECK(pads.count() == 1);
	CHECK(pads.slotOf(11) == -1);
	// The lowest free slot.
	CHECK(pads.add(13, "wheel", 2, 4, 0) == 0);
	for (uint64_t id = 20; id < 26; ++id) {
		CHECK(pads.add(id, "more", 2, 2, 0) >= 0);
	}
	CHECK(pads.count() == 8);
	CHECK(pads.add(99, "ninth", 2, 2, 0) == -1);
	CHECK(input::GameControllers::slotNamed("IDFGameController_7") == 7);
	CHECK(input::GameControllers::slotNamed("IDFMouse") == -1);
	CHECK(input::GameControllers::deviceName(3) == "IDFGameController_3");
}

TEST(GameControllers_AxesButtonsHats)
{
	input::GameControllers pads;
	const int slot = pads.add(1, "stick", 4, 6, 1);
	// -1000..1000 times 0.001 (0x006aaef0).
	pads.setAxis(slot, 1, -500);
	CHECK(near(pads.axis(slot, 1), -0.5f));
	pads.setAxis(slot, 1, 4000);
	CHECK(near(pads.axis(slot, 1), 1.0f));
	// Sensitivity scales the raw value; offset and inversion apply on
	// reading (0x006aadc0).
	pads.setSensitivity(slot, 0, 2);
	pads.setAxis(slot, 0, 250);
	CHECK(near(pads.axis(slot, 0), 0.5f));
	pads.setOffset(slot, 0, 0.1f);
	pads.setInverted(slot, 0, true);
	CHECK(near(pads.axis(slot, 0), 0.1f - 0.5f));
	// Buttons, pressed and released by frame.
	pads.beginFrame();
	pads.setButton(slot, 2, true);
	CHECK(pads.down(slot, 2));
	CHECK(pads.pressed(slot, 2));
	pads.beginFrame();
	CHECK(!pads.pressed(slot, 2));
	pads.setButton(slot, 2, false);
	CHECK(pads.released(slot, 2));
	// The hat's four directions after the six buttons; diagonals none.
	CHECK(pads.buttonCount(slot) == 10);
	pads.setPov(slot, 0, 9000);
	CHECK(pads.down(slot, 7));
	pads.setPov(slot, 0, 27000);
	CHECK(pads.down(slot, 9) && !pads.down(slot, 7));
	pads.setPov(slot, 0, 4500);
	for (int b = 6; b < 10; ++b) {
		CHECK(!pads.down(slot, b));
	}
	// Unplugged: nothing held.
	pads.setButton(slot, 1, true);
	pads.remove(1);
	CHECK(!pads.down(slot, 1));
	CHECK(pads.axis(slot, 1) == 0);
}

TEST(GameControllers_LastInput)
{
	input::GameControllers pads;
	const int slot = pads.add(1, "pad", 5, 10, 1);
	// An axis counts once it was at rest (under 0.2) and then went past 0.95.
	pads.setAxis(slot, 1, 990);
	CHECK(!pads.lastInput()); // not at rest first
	pads.setAxis(slot, 1, 0);
	CHECK(!pads.lastInput());
	pads.setAxis(slot, 1, -990);
	auto last = pads.lastInput();
	CHECK(last && last->device == "IDFGameController_0" && last->control == "IDAxis_1" && last->direction == -1);
	CHECK(!pads.lastInput()); // once
	// A button newly pressed comes first.
	pads.beginFrame();
	pads.setButton(slot, 3, true);
	last = pads.lastInput();
	CHECK(last && last->control == "IDButton_3");
}

TEST(GameControllers_ControlMaps)
{
	Console console;
	input::ControlMaps maps;
	input::registerControlMapConsoleMethods(console, maps);
	std::string out;
	for (const char* line : {
			 "ControlMap.create InfantryPlayerInputControlMap",
			 "ControlMap.addKeysToAxisMapping c_PIThrottle IDFKeyboard IDKey_W IDKey_S 0",
			 "ControlMap.addAxisToAxisMapping c_PIThrottle IDFGameController_0 IDAxis_1 1 1",
			 "ControlMap.addButtonToTriggerMapping c_PIFire IDFGameController_0 IDButton_5 0 1",
			 "ControlMap.addButtonsToAxisMapping c_PIYaw IDFGameController_1 IDButton_1 IDButton_2 1",
			 "ControlMap.mouseSensitivity 1.5",
			 "ControlMap.setYawFactor 0.8",
			 "ControlMap.InvertMouse 1",
		 }) {
		CHECK(console.execute(line, out, ConsoleAccess::Anyone) != ConsoleResult::Error);
	}
	input::GameControllers pads;
	pads.add(1, "pad", 5, 10, 1);
	pads.add(2, "stick", 2, 4, 0);
	input::ControlMaps::Source source;
	source.down = [&pads](const input::Binding& b) {
		const int slot = input::GameControllers::slotNamed(b.device);
		return slot >= 0 && pads.down(slot, std::atoi(b.control.c_str() + 9));
	};
	source.axis = [&pads](const input::Binding& b) {
		const int slot = input::GameControllers::slotNamed(b.device);
		return slot >= 0 ? pads.axis(slot, std::atoi(b.control.c_str() + 7)) : 0.0f;
	};
	const std::string inf = input::ControlMaps::kInfantry;
	// The stick pushed forward (Y negative), inverted: throttle forward.
	pads.setAxis(0, 1, -600);
	CHECK(near(maps.value(inf, "c_PIThrottle", source), 0.6f));
	CHECK(!maps.pressed(inf, "c_PIFire", source));
	pads.setButton(0, 5, true);
	CHECK(maps.pressed(inf, "c_PIFire", source));
	pads.setButton(1, 2, true);
	CHECK(maps.value(inf, "c_PIYaw", source) == -1);
	// A keyboard-only reader ignores the controllers' buttons of the same name.
	CHECK(!maps.pressed(inf, "c_PIFire", [](const std::string& c) { return c == "IDButton_5"; }));
	// The map's own settings.
	const auto settings = maps.settingsOf(inf);
	CHECK(near(settings.mouseSensitivity, 1.5f) && near(settings.yawFactor, 0.8f) && settings.invertMouse);
	// Written back and read again: the same bindings.
	const std::string script = maps.script();
	CHECK(script.find("ControlMap.addAxisToAxisMapping c_PIThrottle IDFGameController_0 IDAxis_1 1 1") != std::string::npos);
	CHECK(script.find("ControlMap.addButtonsToAxisMapping c_PIYaw IDFGameController_1 IDButton_1 IDButton_2 1") != std::string::npos);
	CHECK(script.find("ControlMap.mouseSensitivity 1.5") != std::string::npos);
	Console again;
	input::ControlMaps reread;
	input::registerControlMapConsoleMethods(again, reread);
	again.executeScript(script, out);
	CHECK(reread.script() == script);
	// Rebinding an axis replaces the slot's keys, a key the slot's axis.
	maps.bind(inf, "c_PIThrottle", "IDFGameController_1", "IDAxis_0", 0, -1);
	const input::Binding* b = maps.binding(inf, "c_PIThrottle", 0, 1);
	CHECK(b && b->kind == input::Binding::Kind::Axis && b->sign == -1 && b->device == "IDFGameController_1");
	CHECK(input::ControlMaps::displayKey(*b) == "CONTROLLER_IDFGameController_IDAxis_0");
	maps.bind(inf, "c_PIThrottle", "IDKey_Up", 0, 1);
	b = maps.binding(inf, "c_PIThrottle", 0, 1);
	CHECK(b && b->kind == input::Binding::Kind::Key && b->control == "IDKey_Up");
	CHECK(input::ControlMaps::displayKey(*b) == "CONTROLLER_IDKey_Up");
}

#if BF2142_HAS_SDL_INPUT
// Virtual SDL devices: attached, moved, pressed and detached, no hardware.
TEST(GameControllers_SdlVirtual)
{
	SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
	input::GameControllers pads;
	{
		input::SdlGameControllers sdl(pads);
		if (!sdl.start()) {
			std::fprintf(stderr, "  (SDL joysticks unavailable: skipped)\n");
			return;
		}
		const auto pump = [&sdl] {
			SDL_UpdateJoysticks();
			SDL_PumpEvents();
			SDL_Event event;
			while (SDL_PollEvent(&event)) {
				sdl.handle(event);
			}
		};
		pump();
		const int before = pads.count();
		// A plain joystick: 3 axes, 4 buttons, a hat.
		SDL_VirtualJoystickDesc desc;
		SDL_INIT_INTERFACE(&desc);
		desc.type = SDL_JOYSTICK_TYPE_FLIGHT_STICK;
		desc.naxes = 3;
		desc.nbuttons = 4;
		desc.nhats = 1;
		desc.name = "virtual stick";
		const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
		CHECK(id != 0);
		pump();
		CHECK(pads.count() == before + 1);
		const int slot = pads.slotOf(id);
		CHECK(slot >= 0);
		SDL_Joystick* stick = SDL_GetJoystickFromID(id);
		CHECK(stick != nullptr);
		SDL_SetJoystickVirtualAxis(stick, 1, -32767);
		SDL_SetJoystickVirtualButton(stick, 2, true);
		SDL_SetJoystickVirtualHat(stick, 0, SDL_HAT_DOWN);
		pump();
		CHECK(near(pads.axis(slot, 1), -1.0f));
		CHECK(pads.down(slot, 2));
		CHECK(pads.down(slot, 4 + 2)); // the hat's down after the 4 buttons
		SDL_DetachVirtualJoystick(id);
		pump();
		CHECK(pads.count() == before);

		// A gamepad: the Xbox 360 layout (left stick IDAxis_0/1, A IDButton_0,
		// the d-pad as the hat after ten buttons).
		SDL_VirtualJoystickDesc padDesc;
		SDL_INIT_INTERFACE(&padDesc);
		padDesc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
		padDesc.naxes = SDL_GAMEPAD_AXIS_COUNT;
		padDesc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
		padDesc.name = "virtual pad";
		const SDL_JoystickID padId = SDL_AttachVirtualJoystick(&padDesc);
		pump();
		const int padSlot = pads.slotOf(padId);
		CHECK(padSlot >= 0);
		if (padSlot >= 0 && SDL_IsGamepad(padId)) {
			SDL_Joystick* pad = SDL_GetJoystickFromID(padId);
			SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFTX, 16384);
			SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
			SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
			SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT, true);
			pump();
			CHECK(near(pads.axis(padSlot, 0), 0.5f));
			CHECK(near(pads.axis(padSlot, 2), -1.0f));
			CHECK(pads.down(padSlot, 0));
			CHECK(pads.down(padSlot, 13));
		}
		SDL_DetachVirtualJoystick(padId);
		pump();
	}
	CHECK(pads.count() == 0);
}
#endif
