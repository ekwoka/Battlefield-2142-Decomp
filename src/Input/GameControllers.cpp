#include "GameControllers.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dice::hfe::input {

GameControllers::GameControllers()
{
	for (Device& d : m_devices) {
		d.sensitivity.fill(1.0f); // the constructor 0x006ab590
	}
}

int GameControllers::add(uint64_t id, const std::string& name, int axes, int buttons, int povs)
{
	if (const int existing = slotOf(id); existing >= 0) {
		return existing;
	}
	for (int slot = 0; slot < kMaxDevices; ++slot) {
		Device& d = m_devices[static_cast<size_t>(slot)];
		if (d.connected) {
			continue;
		}
		// A slot keeps its sensitivities, offsets and inversions across
		// unplugging, as the profile's settings for that device number.
		const auto sensitivity = d.sensitivity;
		const auto offset = d.offset;
		const auto inverted = d.inverted;
		d = Device();
		d.sensitivity = sensitivity;
		d.offset = offset;
		d.inverted = inverted;
		d.connected = true;
		d.id = id;
		d.name = name;
		d.axisCount = std::clamp(axes, 0, kMaxAxes);
		d.povCount = std::clamp(povs, 0, 4);
		// DIJOYSTATE2 has 128 buttons; the control maps' masks take 32,
		// the hats' four each after the buttons.
		d.buttonCount = std::clamp(buttons, 0, kMaxButtons - 4 * d.povCount);
		return slot;
	}
	return -1;
}

void GameControllers::remove(uint64_t id)
{
	const int slot = slotOf(id);
	if (slot < 0) {
		return;
	}
	Device& d = m_devices[static_cast<size_t>(slot)];
	// Unacquired (0x006aac80): nothing held any more.
	d.connected = false;
	d.id = 0;
	d.axis.fill(0);
	d.pov.fill(-1);
	d.rawButtons = d.buttons = d.previous = 0;
}

int GameControllers::slotOf(uint64_t id) const
{
	for (int slot = 0; slot < kMaxDevices; ++slot) {
		if (m_devices[static_cast<size_t>(slot)].connected && m_devices[static_cast<size_t>(slot)].id == id) {
			return slot;
		}
	}
	return -1;
}

int GameControllers::count() const
{
	return static_cast<int>(std::count_if(m_devices.begin(), m_devices.end(), [](const Device& d) { return d.connected; }));
}

void GameControllers::setAxis(int slot, int axis, int raw)
{
	if (slot < 0 || slot >= kMaxDevices || axis < 0 || axis >= kMaxAxes) {
		return;
	}
	Device& d = m_devices[static_cast<size_t>(slot)];
	raw = std::clamp(raw, -1000, 1000);
	d.axis[static_cast<size_t>(axis)] = static_cast<float>(raw) * 0.001f * d.sensitivity[static_cast<size_t>(axis)];
}

void GameControllers::setButton(int slot, int button, bool down)
{
	if (slot < 0 || slot >= kMaxDevices || button < 0) {
		return;
	}
	Device& d = m_devices[static_cast<size_t>(slot)];
	if (button >= d.buttonCount) {
		return;
	}
	d.rawButtons = down ? d.rawButtons | (1u << button) : d.rawButtons & ~(1u << button);
	updateButtons(d);
}

void GameControllers::setPov(int slot, int pov, int hundredths)
{
	if (slot < 0 || slot >= kMaxDevices || pov < 0 || pov >= 4) {
		return;
	}
	Device& d = m_devices[static_cast<size_t>(slot)];
	d.pov[static_cast<size_t>(pov)] = hundredths;
	updateButtons(d);
}

// 0x006aaef0: the buttons, then for each hat four bits from button count
// + 4 * hat: up (0), right (9000), down (18000), left (27000).
void GameControllers::updateButtons(Device& d)
{
	d.buttons = d.rawButtons;
	for (int pov = 0; pov < d.povCount; ++pov) {
		const int base = d.buttonCount + 4 * pov;
		int bit = -1;
		switch (d.pov[static_cast<size_t>(pov)]) {
		case 0: bit = base; break;
		case 9000: bit = base + 1; break;
		case 18000: bit = base + 2; break;
		case 27000: bit = base + 3; break;
		default: break;
		}
		if (bit >= 0 && bit < kMaxButtons) {
			d.buttons |= 1u << bit;
		}
	}
}

void GameControllers::beginFrame()
{
	for (Device& d : m_devices) {
		d.previous = d.buttons;
	}
}

float GameControllers::axis(int slot, int axis) const
{
	if (slot < 0 || slot >= kMaxDevices || axis < 0 || axis >= kMaxAxes) {
		return 0;
	}
	const Device& d = m_devices[static_cast<size_t>(slot)];
	const auto i = static_cast<size_t>(axis);
	return d.inverted[i] ? d.offset[i] - d.axis[i] : d.offset[i] + d.axis[i];
}

bool GameControllers::down(int slot, int button) const
{
	if (slot < 0 || slot >= kMaxDevices || button < 0 || button >= kMaxButtons) {
		return false;
	}
	return (m_devices[static_cast<size_t>(slot)].buttons & (1u << button)) != 0; // 0x006aaca0
}

bool GameControllers::pressed(int slot, int button) const
{
	if (slot < 0 || slot >= kMaxDevices || button < 0 || button >= kMaxButtons) {
		return false;
	}
	const Device& d = m_devices[static_cast<size_t>(slot)];
	return (d.previous & (1u << button)) == 0 && (d.buttons & (1u << button)) != 0; // 0x006aacc0
}

bool GameControllers::released(int slot, int button) const
{
	if (slot < 0 || slot >= kMaxDevices || button < 0 || button >= kMaxButtons) {
		return false;
	}
	const Device& d = m_devices[static_cast<size_t>(slot)];
	return (d.previous & (1u << button)) != 0 && (d.buttons & (1u << button)) == 0; // 0x006aace0
}

int GameControllers::buttonCount(int slot) const
{
	const Device& d = m_devices[static_cast<size_t>(std::clamp(slot, 0, kMaxDevices - 1))];
	return d.buttonCount + 4 * d.povCount;
}

void GameControllers::setSensitivity(int slot, int axis, float value)
{
	if (slot >= 0 && slot < kMaxDevices && axis >= 0 && axis < kMaxAxes) {
		m_devices[static_cast<size_t>(slot)].sensitivity[static_cast<size_t>(axis)] = value; // 0x006aad80
	}
}

void GameControllers::setOffset(int slot, int axis, float value)
{
	if (slot >= 0 && slot < kMaxDevices && axis >= 0 && axis < kMaxAxes) {
		m_devices[static_cast<size_t>(slot)].offset[static_cast<size_t>(axis)] = value; // 0x006aad40
	}
}

void GameControllers::setInverted(int slot, int axis, bool inverted)
{
	if (slot >= 0 && slot < kMaxDevices && axis >= 0 && axis < kMaxAxes) {
		m_devices[static_cast<size_t>(slot)].inverted[static_cast<size_t>(axis)] = inverted; // 0x006aad00
	}
}

std::optional<GameControllers::Input> GameControllers::lastInput()
{
	for (int slot = 0; slot < kMaxDevices; ++slot) {
		Device& d = m_devices[static_cast<size_t>(slot)];
		if (!d.connected) {
			continue;
		}
		for (int button = 0; button < buttonCount(slot); ++button) {
			if (pressed(slot, button)) {
				return Input {deviceName(slot), "IDButton_" + std::to_string(button), 1};
			}
		}
		for (int a = 0; a < d.axisCount; ++a) {
			const float v = d.axis[static_cast<size_t>(a)];
			if (std::fabs(v) < 0.2f) {
				d.rested[static_cast<size_t>(a)] = true;
			} else if (d.rested[static_cast<size_t>(a)] && std::fabs(v) > 0.95f) {
				d.rested[static_cast<size_t>(a)] = false;
				return Input {deviceName(slot), "IDAxis_" + std::to_string(a), v < 0 ? -1.0f : 1.0f};
			}
		}
	}
	return std::nullopt;
}

int GameControllers::slotNamed(const std::string& device)
{
	static const std::string prefix = "IDFGameController_";
	if (device.compare(0, prefix.size(), prefix) != 0 || device.size() != prefix.size() + 1) {
		return -1;
	}
	const int slot = device.back() - '0';
	return slot >= 0 && slot < kMaxDevices ? slot : -1;
}

std::string GameControllers::deviceName(int slot)
{
	return "IDFGameController_" + std::to_string(slot);
}

} // namespace dice::hfe::input
