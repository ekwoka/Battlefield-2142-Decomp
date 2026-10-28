#include "HudAlerts.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Hud/Hud.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace dice::hfe::menu {

namespace {

const char* const kTargetTypes[] = {"TTNone", "TTLaser", "TTHeat", "TTMagnet", "TTUnique", "TTBeacon", "TTDecoy", "TTPod"};

std::string unquoted(std::string s)
{
	s.erase(std::remove(s.begin(), s.end(), '"'), s.end());
	return s;
}

std::string setting(const world::ObjectTemplate& t, const std::string& key)
{
	const auto it = t.settings().find(key);
	return it == t.settings().end() ? std::string() : unquoted(it->second);
}

} // namespace

int targetTypeIndex(const std::string& name)
{
	for (int i = 0; i < 8; ++i) {
		if (name.size() == std::char_traits<char>::length(kTargetTypes[i])
			&& std::equal(name.begin(), name.end(), kTargetTypes[i], [](char a, char b) {
				   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			   })) {
			return i;
		}
	}
	return -1;
}

const char* targetTypeName(int index) { return index >= 0 && index < 8 ? kTargetTypes[index] : ""; }

void registerWarningHudMethods(Console& console, world::ObjectTemplateManager& templates)
{
	const uint32_t access = ConsoleAccess::Anyone;
	world::ObjectTemplateManager* t = &templates;
	const auto keep = [&console, t](const char* method, const char* field) {
		bindMethod(console, std::string("ObjectTemplate.warningHud.") + method, access,
			[t, field](std::string type, std::string value) {
				const int index = targetTypeIndex(type);
				if (!t->active() || index < 0) {
					return;
				}
				t->active()->settings()[std::string("warningHud.") + kTargetTypes[index] + "." + field] = unquoted(value);
			});
	};
	keep("setWarningType", "warn");
	keep("setWarningIcon", "icon");
	keep("setFirstWarningSound", "firstSound");
	keep("setSecondWarningSound", "secondSound");
}

Warning warningFor(const world::ObjectTemplate& vehicle, int targetType)
{
	Warning w;
	if (targetType < 0 || targetType >= 8) {
		return w;
	}
	const std::string prefix = std::string("warningHud.") + kTargetTypes[targetType] + ".";
	w.warns = std::atoi(setting(vehicle, prefix + "warn").c_str()) != 0;
	w.icon = setting(vehicle, prefix + "icon");
	w.firstSound = setting(vehicle, prefix + "firstSound");
	w.secondSound = setting(vehicle, prefix + "secondSound");
	return w;
}

void WarningHud::play(const std::string& sound)
{
	if (sound == m_playing) {
		return;
	}
	if (m_sound && !m_playing.empty()) {
		m_sound(m_playing, false);
	}
	m_playing = sound;
	if (m_sound && !m_playing.empty()) {
		m_sound(m_playing, true);
	}
}

// 0x007a2100's case 0x2f, an event per seeker each frame.
void WarningHud::update(hud::HudManager& hud, const world::ObjectTemplate* vehicle, const std::vector<Seeker>& seekers)
{
	std::set<uint32_t> now;
	std::string icon, sound;
	if (vehicle) {
		for (const Seeker& seeker : seekers) {
			Warning w = warningFor(*vehicle, seeker.targetType);
			// TTUnique warns when the vehicle warns of TTLaser too (+0x1c1).
			if (!w.warns && seeker.targetType == 4) {
				const Warning laser = warningFor(*vehicle, 1);
				if (laser.warns) {
					w.warns = true;
				}
			}
			if (!w.warns) {
				continue;
			}
			now.insert(seeker.id);
			if (icon.empty() || seeker.chasing) {
				icon = w.icon;
				sound = seeker.chasing ? w.secondSound : w.firstSound;
			}
		}
	}
	m_warning = std::move(now);
	if (m_warning.empty()) {
		play("");
		hud.set("WarningIconShow", 0);
		return;
	}
	play(sound);
	hud.set("WarningIconShow", 1);
	hud.setString("WarningIconPath", icon);
}

void HelpPopups::bind(Console& console)
{
	const uint32_t access = ConsoleAccess::Anyone;
	bindMethod(console, "GeneralSettings.setHelpPopups", access, [this](bool on) { setEnabled(on); });
}

bool HelpPopups::show(hud::HudManager& hud, const std::string& rawKey, const std::string& sound)
{
	const std::string key = unquoted(rawKey);
	if (!m_enabled || key.empty() || m_shown.count(key)) {
		return false;
	}
	m_shown.insert(key);
	hud.setString("IngameHelpString", m_localize ? m_localize(key) : key);
	hud.set("IngameHelpShow", 1);
	m_timer = kShowSeconds;
	if (m_voiceHook) {
		m_voiceHook(sound.empty() ? key : unquoted(sound));
	}
	return true;
}

void HelpPopups::update(hud::HudManager& hud, const Context& context, float seconds)
{
	// The HUD manager's count down (0x007539c0).
	if (m_timer > 0) {
		m_timer -= seconds;
		if (m_timer <= 0) {
			hud.set("IngameHelpShow", 0);
		}
	}
	if (context.vehicle != m_vehicle) {
		m_vehicle = context.vehicle;
		m_lowArmorShown = false;
		if (m_vehicle) {
			show(hud, setting(*m_vehicle, "helpHud.helpStringKey"), setting(*m_vehicle, "helpHud.helpSoundKey"));
		}
	}
	if (context.weapon != m_weapon) {
		m_weapon = context.weapon;
		if (m_weapon && !m_vehicle) {
			show(hud, setting(*m_weapon, "helpHud.helpStringKey"), setting(*m_weapon, "helpHud.helpSoundKey"));
		}
	}
	if (m_vehicle && !m_lowArmorShown) {
		const std::string key = setting(*m_vehicle, "helpHud.lowArmorHelpStringKey");
		const float critical = static_cast<float>(std::atof(setting(*m_vehicle, "armor.criticalDamage").c_str()));
		if (!key.empty() && critical > 0 && context.vehicleHitPoints > 0 && context.vehicleHitPoints <= critical) {
			m_lowArmorShown = true;
			show(hud, key, setting(*m_vehicle, "helpHud.lowArmorHelpSoundKey"));
		}
	}
	for (const Context::Near& near : context.nearby) {
		if (!near.object) {
			continue;
		}
		const float reach = static_cast<float>(std::atof(setting(*near.object, "helpHud.closeToDistance").c_str()));
		if (reach <= 0 || near.distance > reach) {
			continue;
		}
		const char* side = near.side == 1 ? "Friendly" : near.side == 2 ? "Enemy" : "Neutral";
		if (show(hud, setting(*near.object, std::string("helpHud.closeToHelpStringKey") + side),
				setting(*near.object, std::string("helpHud.closeToHelpSoundKey") + side))) {
			break;
		}
	}
}

std::string abilitySound(const world::ObjectTemplate& receiver, int what)
{
	static const char* const kKeys[] = {"abilityHud.healingSound", "abilityHud.repairingSound", "abilityHud.ammoSound"};
	return what >= 0 && what < 3 ? setting(receiver, kKeys[what]) : std::string();
}

} // namespace dice::hfe::menu
