// The player's controls: control maps as the profile's Controls/*.con
// scripts build them (the engine's "ControlMap" console object):
//   ControlMap.create defaultPlayerInputControlMap
//   ControlMap.addKeysToAxisMapping c_PIThrottle IDFKeyboard IDKey_W IDKey_S
//   ControlMap.addKeyToTriggerMapping c_PIUse IDFKeyboard IDKey_E c_CMPushAndHold
//   ControlMap.addButtonToTriggerMapping c_PIFire IDFMouse IDButton_0 c_CMPushAndHold
//   ControlMap.addAxisToTriggerMapping c_PINextItem c_PIPrevItem IDFMouse IDAxis_2
//   ControlMap.addAxisToAxisMapping c_PIMouseLookX IDFMouse IDAxis_0 0
// 2142's own Settings/Controls.con ends the trigger mappings with a repeat
// delay and the binding's slot (0 primary, 1 secondary) instead of BF2's
// c_CMPushAndHold / c_CMNonRepetive:
//   ControlMap.addKeyToTriggerMapping c_PIUse IDFKeyboard IDKey_E 0 0
//   ControlMap.addKeyToTriggerMapping c_PIReload IDFKeyboard IDKey_R 10000 0
// (10000: once per press, 0: while held). Each map binds the player inputs
// (c_PI...) or game inputs (c_GI...) to keys ("IDKey_W"), mouse buttons
// ("IDButton_0") and mouse axes ("IDAxis_2", the wheel). The game switches
// the player map with what the player controls (on foot
// InfantryPlayerInputControlMap, LandPlayerInputControlMap,
// AirPlayerInputControlMap, HelicopterPlayerInputControlMap,
// SeaPlayerInputControlMap); defaultPlayerInputControlMap holds the player
// inputs common to all, defaultGameControlMap the game's (menu, map,
// scoreboard, chat, the commander screen c_GILeader).
//
// Devices are named, not read here: the window answers whether a named
// key or button is down. Joysticks and gamepads are IDFGameController_0..7
// (GameControllers.h): their buttons bind as triggers and key pairs'
// halves, their axes to axis inputs
//   ControlMap.addAxisToAxisMapping c_PIThrottle IDFGameController_0 IDAxis_1 1 0
// (input, device, axis, inverted, slot; read by 0x006802c0: the device's
// axis, negated when inverted), and add up with the keys' values.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
}

namespace dice::hfe::input {

// One binding of a player input.
struct Binding {
	enum class Kind { Key, Button, Axis } kind = Kind::Key;
	std::string device;  // "IDFKeyboard", "IDFMouse"
	std::string control; // "IDKey_W", "IDButton_0", "IDAxis_2"
	float sign = 1;      // a key pair's halves: +1 the first key, -1 the second
	bool hold = true;    // while held (c_CMPushAndHold, delay 0), else once per press
	int slot = 0;        // 0 primary, 1 secondary
};

class ControlMaps {
public:
	// Maps by name, each from a player input (c_PIThrottle) to its bindings.
	using Map = std::map<std::string, std::vector<Binding>>;

	Map& create(const std::string& name);
	const Map* find(const std::string& name) const;
	const std::map<std::string, Map>& maps() const { return m_maps; }
	// The map the next mappings go to (the last created, or setActive's).
	Map* current() { return m_current; }
	void setActive(const std::string& name);
	const std::string& currentName() const { return m_currentName; }

	// A map's own settings (the control map object's +0x4c..+0x6c, written
	// by 0x00680f90 when not at their defaults): ControlMap.invertMouse,
	// mouseSensitivity (0..10, 0x006802a0), keyboardSensitivity (the
	// keys-to-axis ramp), setYawFactor and setPitchFactor (the mouse's X
	// and Y scaled by their ratio, 0x006802c0).
	struct MapSettings {
		bool invertMouse = false;
		float mouseSensitivity = 1;
		float keyboardSensitivity = 0.1f;
		float yawFactor = 1, pitchFactor = 1;
	};
	MapSettings& settings(const std::string& map) { return m_settings[map]; }
	MapSettings settingsOf(const std::string& map) const
	{
		const auto it = m_settings.find(map);
		return it == m_settings.end() ? MapSettings() : it->second;
	}

	// An input's value in a map, else in the common maps (player inputs
	// in defaultPlayerInputControlMap, game inputs in
	// defaultGameControlMap): an axis from
	// its keys (-1..1), a trigger 1 while one of its keys/buttons is down.
	// `down` tells whether a named key or button is pressed.
	// Only keyboard and mouse bindings are asked (`down` names no device).
	using IsDown = std::function<bool(const std::string& control)>;
	float value(const std::string& map, const std::string& input, const IsDown& down) const;
	bool pressed(const std::string& map, const std::string& input, const IsDown& down) const
	{
		return value(map, input, down) != 0;
	}
	// Every device: `down` for keys and buttons, `axis` for an axis
	// binding's value (-1..1, before its inversion; mouse axes are the
	// window's own and may answer 0).
	struct Source {
		std::function<bool(const Binding&)> down;
		std::function<float(const Binding&)> axis;
	};
	float value(const std::string& map, const std::string& input, const Source& source) const;
	bool pressed(const std::string& map, const std::string& input, const Source& source) const
	{
		return value(map, input, source) != 0;
	}

	// The control a player input is bound to first ("IDKey_E"; "" for
	// none), for the controls page (ControlSettings.getMappedString).
	std::string firstControl(const std::string& map, const std::string& input) const;
	// Rebinds a trigger input's primary binding to one key or button.
	void rebind(const std::string& map, const std::string& input, const std::string& control)
	{
		bind(map, input, control, 0, 1);
	}
	// Binds a key or button to an input's slot (0 primary, 1 secondary) and
	// side (a key pair's -1 half), replacing what was there
	// (ControlSettings.mapInput).
	void bind(const std::string& map, const std::string& input, const std::string& control, int slot, float sign);
	// The same on a named device: a game controller's button
	// ("IDFGameController_0", "IDButton_3") or axis ("IDAxis_1", bound as
	// an axis mapping, inverted when `sign` is -1).
	void bind(const std::string& map, const std::string& input, const std::string& device, const std::string& control,
		int slot, float sign);
	// The localization key the controls page shows for a binding
	// (0x00498750: "CONTROLLER_IDKey_W", "CONTROLLER_IDFMouse_IDButton_0",
	// "CONTROLLER_IDFGameController_IDAxis_1").
	static std::string displayKey(const Binding& binding);
	// All maps as the scripts that build them (2142's format), to keep
	// rebound keys between runs.
	std::string script() const;

	// The binding in a slot and side, if any (an axis binding answers
	// either side).
	const Binding* binding(const std::string& map, const std::string& input, int slot, float sign) const;

	static constexpr const char* kGame = "defaultGameControlMap";
	static constexpr const char* kPlayer = "defaultPlayerInputControlMap";
	static constexpr const char* kInfantry = "InfantryPlayerInputControlMap";
	static constexpr const char* kLand = "LandPlayerInputControlMap";
	static constexpr const char* kAir = "AirPlayerInputControlMap";
	static constexpr const char* kHelicopter = "HelicopterPlayerInputControlMap";
	static constexpr const char* kSea = "SeaPlayerInputControlMap";

private:
	const std::vector<Binding>* bindings(const std::string& map, const std::string& input) const;
	std::map<std::string, Map> m_maps;
	std::map<std::string, MapSettings> m_settings;
	Map* m_current = nullptr;
	std::string m_currentName;
};

// The engine's number of an input constant ("c_PIFire"; -1 for none),
// and the player (c_PI) or game (c_GI) input of a number.
int inputNumber(const std::string& name);
std::string inputNamed(int number, bool player);

// ControlMap.create and the ControlMap.add*Mapping methods into `maps`.
void registerControlMapConsoleMethods(Console& console, ControlMaps& maps);

} // namespace dice::hfe::input
