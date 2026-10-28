#include "SdlGameControllers.h"

#include "GameControllers.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>

namespace dice::hfe::input {

namespace {

// The hat's directions in hundredths of a degree (DirectInput's POV:
// clockwise from up), -1 centred.
int povFromHat(uint8_t hat)
{
	switch (hat) {
	case SDL_HAT_UP: return 0;
	case SDL_HAT_RIGHTUP: return 4500;
	case SDL_HAT_RIGHT: return 9000;
	case SDL_HAT_RIGHTDOWN: return 13500;
	case SDL_HAT_DOWN: return 18000;
	case SDL_HAT_LEFTDOWN: return 22500;
	case SDL_HAT_LEFT: return 27000;
	case SDL_HAT_LEFTUP: return 31500;
	default: return -1;
	}
}

// The Xbox 360 pad's DirectInput buttons (see the header).
int gamepadButton(int button)
{
	switch (button) {
	case SDL_GAMEPAD_BUTTON_SOUTH: return 0;
	case SDL_GAMEPAD_BUTTON_EAST: return 1;
	case SDL_GAMEPAD_BUTTON_WEST: return 2;
	case SDL_GAMEPAD_BUTTON_NORTH: return 3;
	case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return 4;
	case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return 5;
	case SDL_GAMEPAD_BUTTON_BACK: return 6;
	case SDL_GAMEPAD_BUTTON_START: return 7;
	case SDL_GAMEPAD_BUTTON_LEFT_STICK: return 8;
	case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return 9;
	default: return -1;
	}
}

uint8_t dpadBit(int button)
{
	switch (button) {
	case SDL_GAMEPAD_BUTTON_DPAD_UP: return SDL_HAT_UP;
	case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return SDL_HAT_RIGHT;
	case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return SDL_HAT_DOWN;
	case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return SDL_HAT_LEFT;
	default: return 0;
	}
}

} // namespace

SdlGameControllers::SdlGameControllers(GameControllers& controllers) : m_controllers(controllers) {}

SdlGameControllers::~SdlGameControllers()
{
	for (auto& [instance, opened] : m_opened) {
		if (opened.gamepad) {
			SDL_CloseGamepad(static_cast<SDL_Gamepad*>(opened.gamepad));
		} else if (opened.joystick) {
			SDL_CloseJoystick(static_cast<SDL_Joystick*>(opened.joystick));
		}
		m_controllers.remove(instance);
	}
	if (m_started) {
		SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
	}
}

bool SdlGameControllers::start()
{
	if (!m_started) {
		if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
			std::fprintf(stderr, "game controllers: %s\n", SDL_GetError());
			return false;
		}
		m_started = true;
	}
	int count = 0;
	if (SDL_JoystickID* ids = SDL_GetJoysticks(&count)) {
		for (int i = 0; i < count; ++i) {
			open(ids[i]);
		}
		SDL_free(ids);
	}
	return true;
}

int SdlGameControllers::rawAxis(int sdlValue)
{
	return std::clamp(sdlValue * 1000 / 32767, -1000, 1000);
}

void SdlGameControllers::open(uint32_t instance)
{
	if (m_opened.count(instance)) {
		return;
	}
	Opened opened;
	int axes = 0, buttons = 0, hats = 0;
	std::string name;
	if (SDL_IsGamepad(instance)) {
		SDL_Gamepad* pad = SDL_OpenGamepad(instance);
		if (!pad) {
			return;
		}
		opened.gamepad = pad;
		opened.joystick = SDL_GetGamepadJoystick(pad);
		axes = 5;
		buttons = 10;
		hats = 1;
		const char* text = SDL_GetGamepadName(pad);
		name = text ? text : "gamepad";
	} else {
		SDL_Joystick* stick = SDL_OpenJoystick(instance);
		if (!stick) {
			return;
		}
		opened.joystick = stick;
		axes = SDL_GetNumJoystickAxes(stick);
		buttons = SDL_GetNumJoystickButtons(stick);
		hats = SDL_GetNumJoystickHats(stick);
		const char* text = SDL_GetJoystickName(stick);
		name = text ? text : "joystick";
	}
	const int slot = m_controllers.add(instance, name, axes, buttons, hats);
	if (slot < 0) {
		// All eight taken: the original also stops at eight devices.
		if (opened.gamepad) {
			SDL_CloseGamepad(static_cast<SDL_Gamepad*>(opened.gamepad));
		} else {
			SDL_CloseJoystick(static_cast<SDL_Joystick*>(opened.joystick));
		}
		return;
	}
	m_opened[instance] = opened;
	std::printf("game controller %s: %s (%d axes, %d buttons, %d hats)\n", GameControllers::deviceName(slot).c_str(),
		name.c_str(), axes, buttons, hats);
}

void SdlGameControllers::close(uint32_t instance)
{
	const auto it = m_opened.find(instance);
	if (it == m_opened.end()) {
		return;
	}
	if (it->second.gamepad) {
		SDL_CloseGamepad(static_cast<SDL_Gamepad*>(it->second.gamepad));
	} else if (it->second.joystick) {
		SDL_CloseJoystick(static_cast<SDL_Joystick*>(it->second.joystick));
	}
	const int slot = m_controllers.slotOf(instance);
	if (slot >= 0) {
		std::printf("game controller %s removed\n", GameControllers::deviceName(slot).c_str());
	}
	m_controllers.remove(instance);
	m_opened.erase(it);
}

bool SdlGameControllers::handle(const SDL_Event& event)
{
	switch (event.type) {
	case SDL_EVENT_JOYSTICK_ADDED:
		open(event.jdevice.which);
		return true;
	case SDL_EVENT_JOYSTICK_REMOVED:
		close(event.jdevice.which);
		return true;
	case SDL_EVENT_GAMEPAD_ADDED:
	case SDL_EVENT_GAMEPAD_REMOVED:
	case SDL_EVENT_GAMEPAD_REMAPPED:
		return true; // the joystick events open and close it
	default:
		break;
	}
	// The device's own events: a gamepad's through the gamepad API only.
	const auto opened = [this](uint32_t instance) -> Opened* {
		const auto it = m_opened.find(instance);
		return it == m_opened.end() ? nullptr : &it->second;
	};
	switch (event.type) {
	case SDL_EVENT_JOYSTICK_AXIS_MOTION:
		if (Opened* o = opened(event.jaxis.which); o && !o->gamepad) {
			m_controllers.setAxis(m_controllers.slotOf(event.jaxis.which), event.jaxis.axis, rawAxis(event.jaxis.value));
		}
		return true;
	case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
	case SDL_EVENT_JOYSTICK_BUTTON_UP:
		if (Opened* o = opened(event.jbutton.which); o && !o->gamepad) {
			m_controllers.setButton(m_controllers.slotOf(event.jbutton.which), event.jbutton.button, event.jbutton.down);
		}
		return true;
	case SDL_EVENT_JOYSTICK_HAT_MOTION:
		if (Opened* o = opened(event.jhat.which); o && !o->gamepad) {
			m_controllers.setPov(m_controllers.slotOf(event.jhat.which), event.jhat.hat, povFromHat(event.jhat.value));
		}
		return true;
	case SDL_EVENT_JOYSTICK_BALL_MOTION:
	case SDL_EVENT_JOYSTICK_BATTERY_UPDATED:
	case SDL_EVENT_JOYSTICK_UPDATE_COMPLETE:
		return true;
	case SDL_EVENT_GAMEPAD_AXIS_MOTION:
		if (Opened* o = opened(event.gaxis.which); o && o->gamepad) {
			const int slot = m_controllers.slotOf(event.gaxis.which);
			switch (event.gaxis.axis) {
			case SDL_GAMEPAD_AXIS_LEFTX: m_controllers.setAxis(slot, 0, rawAxis(event.gaxis.value)); break;
			case SDL_GAMEPAD_AXIS_LEFTY: m_controllers.setAxis(slot, 1, rawAxis(event.gaxis.value)); break;
			case SDL_GAMEPAD_AXIS_RIGHTX: m_controllers.setAxis(slot, 3, rawAxis(event.gaxis.value)); break;
			case SDL_GAMEPAD_AXIS_RIGHTY: m_controllers.setAxis(slot, 4, rawAxis(event.gaxis.value)); break;
			case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
			case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
				(event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? o->lt : o->rt) = event.gaxis.value;
				m_controllers.setAxis(slot, 2, rawAxis(o->lt - o->rt));
				break;
			default: break;
			}
		}
		return true;
	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
	case SDL_EVENT_GAMEPAD_BUTTON_UP:
		if (Opened* o = opened(event.gbutton.which); o && o->gamepad) {
			const int slot = m_controllers.slotOf(event.gbutton.which);
			if (const uint8_t bit = dpadBit(event.gbutton.button)) {
				o->dpad = event.gbutton.down ? o->dpad | bit : o->dpad & ~bit;
				m_controllers.setPov(slot, 0, povFromHat(o->dpad));
			} else if (const int button = gamepadButton(event.gbutton.button); button >= 0) {
				m_controllers.setButton(slot, button, event.gbutton.down);
			}
		}
		return true;
	case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
	case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
	case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
	case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
	case SDL_EVENT_GAMEPAD_UPDATE_COMPLETE:
	case SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED:
		return true;
	default:
		return false;
	}
}

} // namespace dice::hfe::input
