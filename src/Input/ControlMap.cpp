#include "ControlMap.h"
#include "GameControllers.h"

#include "Console/Console.h"

#include <algorithm>
#include <cstdlib>

namespace dice::hfe::input {

ControlMaps::Map& ControlMaps::create(const std::string& name)
{
	// Creating a map again adds to it (the profile's Common.con and
	// Infantry.con both create defaultPlayerInputControlMap).
	m_current = &m_maps[name];
	m_currentName = name;
	return *m_current;
}

void ControlMaps::setActive(const std::string& name)
{
	// 0x0066e1db: the map the following lines configure.
	if (m_maps.count(name)) {
		m_current = &m_maps[name];
		m_currentName = name;
	}
}

const ControlMaps::Map* ControlMaps::find(const std::string& name) const
{
	const auto it = m_maps.find(name);
	return it == m_maps.end() ? nullptr : &it->second;
}

const std::vector<Binding>* ControlMaps::bindings(const std::string& map, const std::string& input) const
{
	const std::string common = input.compare(0, 4, "c_GI") == 0 ? kGame : kPlayer;
	for (const std::string& name : {map, common}) {
		if (const Map* m = find(name)) {
			if (const auto it = m->find(input); it != m->end() && !it->second.empty()) {
				return &it->second;
			}
		}
	}
	return nullptr;
}

float ControlMaps::value(const std::string& map, const std::string& input, const IsDown& down) const
{
	Source source;
	source.down = [&down](const Binding& b) { return GameControllers::slotNamed(b.device) < 0 && down(b.control); };
	return value(map, input, source);
}

// 0x00680b00: each mapping's value by its kind (axis 0x006802c0, keys or
// buttons as an axis 0x00680990, triggers 0x006803f0), summed here over
// the input's primary and secondary bindings.
float ControlMaps::value(const std::string& map, const std::string& input, const Source& source) const
{
	const std::vector<Binding>* list = bindings(map, input);
	if (!list) {
		return 0;
	}
	float value = 0;
	for (const Binding& binding : *list) {
		if (binding.kind == Binding::Kind::Axis) {
			// Axis to axis (hold); axis to trigger is the mouse wheel's,
			// counted by the window.
			if (binding.hold && source.axis) {
				value += source.axis(binding) * binding.sign;
			}
		} else if (source.down && source.down(binding)) {
			value += binding.sign;
		}
	}
	return std::clamp(value, -1.0f, 1.0f);
}

std::string ControlMaps::firstControl(const std::string& map, const std::string& input) const
{
	const std::vector<Binding>* list = bindings(map, input);
	if (!list) {
		return std::string();
	}
	for (const Binding& binding : *list) {
		if (binding.slot == 0) {
			return binding.control;
		}
	}
	return list->front().control;
}

void ControlMaps::bind(const std::string& map, const std::string& input, const std::string& control, int slot, float sign)
{
	const bool button = control.compare(0, 9, "IDButton_") == 0;
	bind(map, input, button ? "IDFMouse" : "IDFKeyboard", control, slot, sign);
}

void ControlMaps::bind(const std::string& map, const std::string& input, const std::string& device, const std::string& control,
	int slot, float sign)
{
	std::vector<Binding>& list = m_maps[map][input];
	Binding binding;
	binding.kind = control.compare(0, 7, "IDAxis_") == 0 ? Binding::Kind::Axis
		: control.compare(0, 9, "IDButton_") == 0      ? Binding::Kind::Button
													   : Binding::Kind::Key;
	binding.device = device;
	binding.control = control;
	binding.sign = sign;
	binding.slot = slot;
	if (binding.kind == Binding::Kind::Axis) {
		// An axis mapping covers both sides of its slot.
		binding.hold = true;
		list.erase(std::remove_if(list.begin(), list.end(), [slot](const Binding& b) { return b.slot == slot; }), list.end());
		list.insert(slot == 0 ? list.begin() : list.end(), binding);
		return;
	}
	if (!list.empty()) {
		const auto trigger = std::find_if(list.begin(), list.end(), [](const Binding& b) { return b.kind != Binding::Kind::Axis; });
		binding.hold = trigger != list.end() ? trigger->hold : true;
	}
	// A key replaces an axis mapping in its slot.
	list.erase(std::remove_if(list.begin(), list.end(),
				   [slot](const Binding& b) { return b.slot == slot && b.kind == Binding::Kind::Axis && b.hold; }),
		list.end());
	const auto same = [slot, sign](const Binding& b) { return b.slot == slot && b.sign == sign && b.kind != Binding::Kind::Axis; };
	const auto at = std::find_if(list.begin(), list.end(), same);
	if (at != list.end()) {
		*at = binding;
	} else {
		list.insert(slot == 0 ? list.begin() : list.end(), binding);
	}
}

std::string ControlMaps::displayKey(const Binding& binding)
{
	if (binding.device == "IDFKeyboard") {
		return "CONTROLLER_" + binding.control;
	}
	if (binding.device == "IDFMouse") {
		return "CONTROLLER_IDFMouse_" + binding.control;
	}
	if (GameControllers::slotNamed(binding.device) >= 0) {
		return "CONTROLLER_IDFGameController_" + binding.control;
	}
	return "CONTROLLER_" + binding.control;
}

std::string ControlMaps::script() const
{
	std::string out;
	for (const auto& [name, map] : m_maps) {
		out += "ControlMap.create " + name + "\n";
		for (const auto& [input, list] : map) {
			// Key pairs: the + key and its - partner in the same slot.
			std::vector<bool> written(list.size(), false);
			for (size_t i = 0; i < list.size(); ++i) {
				const Binding& b = list[i];
				if (written[i]) {
					continue;
				}
				written[i] = true;
				const std::string slot = std::to_string(b.slot);
				if (b.kind == Binding::Kind::Axis) {
					if (b.hold) {
						out += "ControlMap.addAxisToAxisMapping " + input + " " + b.device + " " + b.control + " " +
							(b.sign < 0 ? "1" : "0") + " " + slot + "\n";
					} else if (b.sign > 0) {
						out += "ControlMap.addAxisToTriggerMapping " + input + " -1 " + b.device + " " + b.control + " " + slot + "\n";
					} else {
						out += "ControlMap.addAxisToTriggerMapping -1 " + input + " " + b.device + " " + b.control + " " + slot + "\n";
					}
					continue;
				}
				size_t partner = list.size();
				for (size_t j = i + 1; j < list.size(); ++j) {
					if (!written[j] && list[j].slot == b.slot && list[j].sign == -b.sign && list[j].kind == b.kind
						&& list[j].device == b.device) {
						partner = j;
						break;
					}
				}
				if (partner < list.size()) {
					written[partner] = true;
					const Binding& plus = b.sign > 0 ? b : list[partner];
					const Binding& minus = b.sign > 0 ? list[partner] : b;
					out += std::string(b.kind == Binding::Kind::Button ? "ControlMap.addButtonsToAxisMapping " : "ControlMap.addKeysToAxisMapping ") + input + " " + b.device + " " + plus.control + " " + minus.control +
						" " + slot + "\n";
				} else {
					out += std::string("ControlMap.") +
						(b.kind == Binding::Kind::Button ? "addButtonToTriggerMapping " : "addKeyToTriggerMapping ") + input +
						" " + b.device + " " + b.control + " " + (b.hold ? "0" : "10000") + " " + slot + "\n";
				}
			}
		}
		// 0x00680f90: the settings not at their defaults.
		const MapSettings set = settingsOf(name);
		const auto number = [](float v) {
			std::string text = std::to_string(v);
			text.erase(text.find_last_not_of('0') + 1);
			if (!text.empty() && text.back() == '.') {
				text.pop_back();
			}
			return text;
		};
		if (set.invertMouse) {
			out += "ControlMap.invertMouse 1\n";
		}
		if (set.mouseSensitivity != 1) {
			out += "ControlMap.mouseSensitivity " + number(set.mouseSensitivity) + "\n";
		}
		if (set.keyboardSensitivity != 0.1f) {
			out += "ControlMap.keyboardSensitivity " + number(set.keyboardSensitivity) + "\n";
		}
		if (set.yawFactor != 1) {
			out += "ControlMap.setYawFactor " + number(set.yawFactor) + "\n";
		}
		if (set.pitchFactor != 1) {
			out += "ControlMap.setPitchFactor " + number(set.pitchFactor) + "\n";
		}
	}
	return out;
}

const Binding* ControlMaps::binding(const std::string& map, const std::string& input, int slot, float sign) const
{
	const std::vector<Binding>* list = bindings(map, input);
	if (!list) {
		return nullptr;
	}
	for (const Binding& b : *list) {
		if (b.slot == slot && b.sign == sign && b.kind != Binding::Kind::Axis) {
			return &b;
		}
	}
	for (const Binding& b : *list) {
		if (b.slot == slot && b.kind == Binding::Kind::Axis && b.hold) {
			return &b;
		}
	}
	return nullptr;
}

namespace {

// The trailing arguments of a trigger mapping: BF2's c_CM... word, or
// 2142's repeat delay (10000 once per press, 0 while held) and slot.
void readTriggerTail(const std::vector<std::string>& args, size_t at, Binding& binding)
{
	if (args.size() > at && args[at].compare(0, 4, "c_CM") == 0) {
		binding.hold = args[at] == "c_CMPushAndHold";
		return;
	}
	if (args.size() > at) {
		binding.hold = std::atoi(args[at].c_str()) == 0;
	}
	if (args.size() > at + 1) {
		binding.slot = std::atoi(args[at + 1].c_str());
	}
}

// The engine's input constants and their numbers (0x00669baf registers
// them: the player inputs c_PI... and the game inputs c_GI... count from 0
// each). The scripts name them; here each stands for its own name, and
// the menu's controls page asks by number.
constexpr std::pair<const char*, int> kInputs[] = {
	{"c_noAccess", 0}, {"c_endUserAccess", 1}, {"c_fullAccess", 2}, {"c_totalAccess", 7}, {"c_GIMenu", 0},
	{"c_GIToggleConsole", 1}, {"c_GIEscape", 2}, {"c_GIUp", 3}, {"c_GIDown", 4}, {"c_GILeft", 5},
	{"c_GIRight", 6}, {"c_GIDelete", 30}, {"c_GIBack", 31}, {"c_GIPageUp", 7}, {"c_GIPageDown", 8},
	{"c_GIRightShift", 9}, {"c_GILeftShift", 10}, {"c_GILeftCtrl", 11}, {"c_GIRightCtrl", 12},
	{"c_GILeftAlt", 13}, {"c_GIRightAlt", 14}, {"c_GIOk", 15}, {"c_GIAltOk", 16}, {"c_GITab", 28},
	{"c_GIEnter", 29}, {"c_GICancel", 17}, {"c_GIMenuSelect0", 37}, {"c_GIMenuSelect1", 38},
	{"c_GIMenuSelect2", 39}, {"c_GIMenuSelect3", 40}, {"c_GIMenuSelect4", 41}, {"c_GIMenuSelect5", 42},
	{"c_GIMenuSelect6", 43}, {"c_GIMenuSelect7", 44}, {"c_GIMenuSelect8", 45}, {"c_GIMenuSelect9", 46},
	{"c_GIQuit", 27}, {"c_GIMouseLookX", 20}, {"c_GIMouseLookY", 21}, {"c_GIMouseWheelDown", 26},
	{"c_GIMouseWheelUp", 25}, {"c_GISayAll", 22}, {"c_GISayTeam", 23}, {"c_GISaySquad", 24},
	{"c_GIVoipPushToTalk", 61}, {"c_GIVoipUseLeaderChannel", 56}, {"c_GIScreenShot", 18},
	{"c_GITogglePause", 19}, {"c_GITacticalComm", 33}, {"c_GIRadioComm", 34}, {"c_GIMapZoom", 36},
	{"c_GIMapSize", 35}, {"c_GI3dMap", 57}, {"c_GIYes", 47}, {"c_GINo", 48}, {"c_GIAutoAccept", 49},
	{"c_GICreateSquad", 50}, {"c_GIJoinSquad", 51}, {"c_GISelectSquad", 52}, {"c_GIHotRankUp", 54},
	{"c_GIShowScoreboard", 55}, {"c_GIDeselectAll", 59}, {"c_GISelectAll", 58}, {"c_GILeader", 60},
	{"c_GIShowUnlock", 62}, {"c_GINone", 75}, {"c_PIShowScoreBoard", 63}, {"c_PILie", 37}, {"c_PICrouch", 38},
	{"c_PIYaw", 0}, {"c_PIPitch", 1}, {"c_PIRoll", 2}, {"c_PIThrottle", 3}, {"c_PIMouseLookX", 4},
	{"c_PIMouseLookY", 5}, {"c_PICameraX", 6}, {"c_PICameraY", 7}, {"c_PINumInput", 64}, {"c_PIFire", 8},
	{"c_PIAltFire", 31}, {"c_PIFlareFire", 47}, {"c_PIReload", 32}, {"c_PISelectFunc", 33}, {"c_PIAction", 9},
	{"c_PIUse", 10}, {"c_PIMouseLook", 11}, {"c_PIDrop", 34}, {"c_PINextItem", 48}, {"c_PIPrevItem", 49},
	{"c_PICommunication", 62}, {"c_PIAltSprint", 12}, {"c_PISprint", 13}, {"c_PICameraMode1", 39},
	{"c_PICameraMode2", 40}, {"c_PICameraMode3", 41}, {"c_PICameraMode4", 42}, {"c_PIToggleCamera", 36},
	{"c_PINone", 64}, {"c_PIWeaponSelect1", 14}, {"c_PIWeaponSelect2", 15}, {"c_PIWeaponSelect3", 16},
	{"c_PIWeaponSelect4", 17}, {"c_PIWeaponSelect5", 18}, {"c_PIWeaponSelect6", 19}, {"c_PIWeaponSelect7", 20},
	{"c_PIWeaponSelect8", 21}, {"c_PIWeaponSelect9", 22}, {"c_PIPositionSelect1", 23},
	{"c_PIPositionSelect2", 24}, {"c_PIPositionSelect3", 25}, {"c_PIPositionSelect4", 26},
	{"c_PIPositionSelect5", 27}, {"c_PIPositionSelect6", 28}, {"c_PIPositionSelect7", 29},
	{"c_PIPositionSelect8", 30}, {"c_PIRadio1", 50}, {"c_PIRadio2", 51}, {"c_PIRadio3", 52}, {"c_PIRadio4", 53},
	{"c_PIRadio5", 54}, {"c_PIRadio6", 55}, {"c_PIRadio7", 56}, {"c_PIRadio8", 57}, {"c_PIScreenShot", 58},
	{"c_PIToolTip", 59}, {"c_PISayAll", 60}, {"c_PISayTeam", 61}, {"c_PISelectSecWeapon", 44},
	{"c_PIToggleWeapon", 45}, {"c_PIToggleFireRate", 46}
};
constexpr const char* kModes[] = {"c_CMDoubleTap", "c_CMMinFallTime", "c_CMNonRepetive", "c_CMPushAndHold"};

} // namespace

int inputNumber(const std::string& name)
{
	for (const auto& [input, number] : kInputs) {
		if (name == input) {
			return number;
		}
	}
	return -1;
}

std::string inputNamed(int number, bool player)
{
	for (const auto& [input, n] : kInputs) {
		if (n == number && std::string(input).compare(0, 4, player ? "c_PI" : "c_GI") == 0) {
			return input;
		}
	}
	return std::string();
}

void registerControlMapConsoleMethods(Console& console, ControlMaps& maps)
{
	for (const auto& [input, number] : kInputs) {
		console.registerConstant(input, input);
	}
	for (const char* mode : kModes) {
		console.registerConstant(mode, mode);
	}
	const auto add = [&console](const std::string& name, int minArgs, int maxArgs,
						 std::function<void(const std::vector<std::string>&)> run) {
		ConsoleMethod method;
		method.name = "ControlMap." + name;
		method.access = ConsoleAccess::Anyone;
		method.minArgs = minArgs;
		method.maxArgs = maxArgs;
		method.invoke = [run = std::move(run)](const std::vector<std::string>& args) {
			run(args);
			return std::string();
		};
		console.registerMethod(std::move(method));
	};
	add("create", 1, 1, [&maps](const std::vector<std::string>& args) { maps.create(args[0]); });
	const auto into = [&maps](const std::string& input) -> std::vector<Binding>* {
		ControlMaps::Map* map = maps.current();
		return map ? &(*map)[input] : nullptr;
	};
	// input device keyPositive keyNegative [slot]
	add("addKeysToAxisMapping", 4, 5, [into](const std::vector<std::string>& args) {
		if (std::vector<Binding>* list = into(args[0])) {
			const int slot = args.size() > 4 ? std::atoi(args[4].c_str()) : 0;
			list->push_back({Binding::Kind::Key, args[1], args[2], 1, true, slot});
			list->push_back({Binding::Kind::Key, args[1], args[3], -1, true, slot});
		}
	});
	// input device buttonPositive buttonNegative [slot] (the profile writer's,
	// 0x00680f90, for a controller's buttons on an axis)
	add("addButtonsToAxisMapping", 4, 5, [into](const std::vector<std::string>& args) {
		if (std::vector<Binding>* list = into(args[0])) {
			const int slot = args.size() > 4 ? std::atoi(args[4].c_str()) : 0;
			list->push_back({Binding::Kind::Button, args[1], args[2], 1, true, slot});
			list->push_back({Binding::Kind::Button, args[1], args[3], -1, true, slot});
		}
	});
	// input device key [c_CM... | delay slot]
	for (const char* name : {"addKeyToTriggerMapping", "addButtonToTriggerMapping"}) {
		const bool button = std::string(name).find("Button") != std::string::npos;
		add(name, 3, 5, [into, button](const std::vector<std::string>& args) {
			if (std::vector<Binding>* list = into(args[0])) {
				Binding binding {button ? Binding::Kind::Button : Binding::Kind::Key, args[1], args[2]};
				readTriggerTail(args, 3, binding);
				list->push_back(binding);
			}
		});
	}
	// inputUp inputDown device axis [slot] ("-1" for no input)
	add("addAxisToTriggerMapping", 4, 5, [into](const std::vector<std::string>& args) {
		for (int half = 0; half < 2; ++half) {
			if (args[half] == "-1") {
				continue;
			}
			if (std::vector<Binding>* list = into(args[half])) {
				list->push_back({Binding::Kind::Axis, args[2], args[3], half == 0 ? 1.0f : -1.0f, false});
			}
		}
	});
	// input device axis [inverted slot]
	add("addAxisToAxisMapping", 3, 5, [into](const std::vector<std::string>& args) {
		if (std::vector<Binding>* list = into(args[0])) {
			const bool inverted = args.size() > 3 && std::atoi(args[3].c_str()) != 0;
			list->push_back({Binding::Kind::Axis, args[1], args[2], inverted ? -1.0f : 1.0f, true,
				args.size() > 4 ? std::atoi(args[4].c_str()) : 0});
		}
	});
	add("setActive", 1, 1, [&maps](const std::vector<std::string>& args) { maps.setActive(args[0]); });
	// The current map's own settings (0x0066edb6..0x0066f475).
	const auto settings = [&maps]() -> ControlMaps::MapSettings* {
		return maps.current() ? &maps.settings(maps.currentName()) : nullptr;
	};
	add("mouseSensitivity", 1, 2, [settings](const std::vector<std::string>& args) {
		if (auto* s = settings()) {
			s->mouseSensitivity = std::clamp(static_cast<float>(std::atof(args[0].c_str())), 0.0f, 10.0f);
		}
	});
	add("keyboardSensitivity", 1, 1, [settings](const std::vector<std::string>& args) {
		if (auto* s = settings()) {
			s->keyboardSensitivity = static_cast<float>(std::atof(args[0].c_str()));
		}
	});
	add("invertMouse", 1, 1, [settings](const std::vector<std::string>& args) {
		if (auto* s = settings()) {
			s->invertMouse = std::atoi(args[0].c_str()) != 0;
		}
	});
	add("setYawFactor", 1, 1, [settings](const std::vector<std::string>& args) {
		if (auto* s = settings()) {
			s->yawFactor = static_cast<float>(std::atof(args[0].c_str()));
		}
	});
	add("setPitchFactor", 1, 1, [settings](const std::vector<std::string>& args) {
		if (auto* s = settings()) {
			s->pitchFactor = static_cast<float>(std::atof(args[0].c_str()));
		}
	});
}

} // namespace dice::hfe::input
