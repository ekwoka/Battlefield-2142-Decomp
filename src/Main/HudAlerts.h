// The HUD's alerts: the vehicles' missile and mine warnings (the WarningHud
// template component), the help pop-ups (HelpHud, GameLogic.messages.
// addHelpMessage, GeneralSettings.setHelpPopups) and the supply sounds
// (AbilityHud).
//
// Warnings. ObjectTemplate.warningHud.setWarningType <TargetType> <bool>,
// setWarningIcon <TargetType> <texture>, setFirstWarningSound and
// setSecondWarningSound <TargetType> <sound> (the WarningHud component,
// class id 0xc4f6; its console methods from 0x0055a940, 8 target types of
// 0x1c bytes each, written back by 0x00639a40) say what a vehicle warns of.
// The HUD's event 0x2f (0x007a2100, case 0x2f) is a guided projectile's lock
// on the player's vehicle: for a target type the vehicle warns of (TTUnique
// also when TTLaser is on), the component's warning sound (slot 0x3c: 1 the
// first, 2 the second) starts, WarningIconShow comes on and WarningIconPath
// is that type's icon; the projectiles warning are kept in a set (+0x2c0),
// and when the last one lets go (its event's id -1) the sound stops (0) and
// WarningIconShow goes off. Here the game's projectiles locked on the
// vehicle (GameWorld::projectiles, lockedOn) make those events each frame:
// the first warning while a seeker has locked but is not yet chasing (its
// tracking delay), the second once it chases.
//
// Help. A HelpHud's helpStringKey (helpHud.*, class id 0xc4fa, 15
// properties from 0x0055b355) shows when the player takes the weapon or
// vehicle that has it, its lowArmorHelpStringKey when that vehicle's armour
// falls to its critical damage, the closeToHelpStringKey{Friendly,Enemy,
// Neutral} when the player comes within closeToDistance of an object that
// has them. Showing (the HUD manager's slot 0x94, 0x0074f2b0) puts the text
// in IngameHelpString, IngameHelpShow on and a timer (+0x238) that the HUD
// manager's update counts down (0x007539c0) before IngameHelpShow goes off.
// The voice: the template's matching help sound key (helpSoundKey,
// lowArmorHelpSoundKey, closeToHelpSoundKey*) goes to the voice hook, the
// sound layer's VoiceMessages::playHelp (gameLogic.messages.addHelpMessage
// <key> <file>, Sound/VoiceMessages_Help.con, keeps the Help_VO files
// there, 0x004d8e40 / 0x004d8280). GeneralSettings.setHelpPopups 0 turns
// the pop-ups off. Each key shows once a round.
//
// Supply sounds. abilityHud.healingSound, repairingSound and ammoSound
// (class id 0xc4f2, from 0x0055a41f) are the sounds the player's soldier
// or vehicle plays while it is healed, repaired or rearmed.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
namespace hud {
class HudManager;
}
namespace world {
class ObjectTemplate;
class ObjectTemplateManager;
} // namespace world
} // namespace dice::hfe

namespace dice::hfe::menu {

// TargetType (0x004daf00): TTNone 0, TTLaser 1, TTHeat 2, TTMagnet 3,
// TTUnique 4, TTBeacon 5, TTDecoy 6, TTPod 7; -1 for none of them.
int targetTypeIndex(const std::string& name);
const char* targetTypeName(int index);

// ObjectTemplate.warningHud.* (kept on the template's settings as
// "warningHud.<TargetType>.warn" / ".icon" / ".firstSound" /
// ".secondSound").
void registerWarningHudMethods(Console& console, world::ObjectTemplateManager& templates);

// A vehicle template's warning for a target type (WarningHud's 0x1c
// bytes a type).
struct Warning {
	bool warns = false;
	std::string icon;
	std::string firstSound, secondSound;
};
Warning warningFor(const world::ObjectTemplate& vehicle, int targetType);

class WarningHud {
public:
	struct Seeker {
		uint32_t id = 0;       // the projectile
		int targetType = 0;    // its seek's TargetType
		bool chasing = false;  // past its tracking delay: the second warning
	};
	// The warning sounds: start (true) or stop (false) a sound by name.
	using SoundHook = std::function<void(const std::string& sound, bool start)>;
	void setSoundHook(SoundHook hook) { m_sound = std::move(hook); }

	// One frame: the player's vehicle's template (nullptr on foot) and the
	// projectiles locked on it.
	void update(hud::HudManager& hud, const world::ObjectTemplate* vehicle, const std::vector<Seeker>& seekers);
	bool warning() const { return !m_warning.empty(); }
	const std::string& playing() const { return m_playing; }

private:
	void play(const std::string& sound);
	std::set<uint32_t> m_warning; // +0x2c0
	std::string m_playing;
	SoundHook m_sound;
};

class HelpPopups {
public:
	// GeneralSettings.setHelpPopups.
	void bind(Console& console);
	void setEnabled(bool enabled) { m_enabled = enabled; }
	bool enabled() const { return m_enabled; }
	void setLocalizer(std::function<std::string(const std::string&)> localize) { m_localize = std::move(localize); }
	// The help voice: its sound key (for the sound layer's playHelp).
	void setVoiceHook(std::function<void(const std::string& key)> hook) { m_voiceHook = std::move(hook); }

	// What the player has this frame.
	struct Context {
		const world::ObjectTemplate* vehicle = nullptr; // entered
		const world::ObjectTemplate* weapon = nullptr;  // in hand (on foot)
		float vehicleHitPoints = 0;                     // the vehicle's armour left
		// The objects near the player with closeTo keys: their template, the
		// distance and whose (1 the player's team, 2 the enemy, 0 neutral).
		struct Near {
			const world::ObjectTemplate* object = nullptr;
			float distance = 0;
			int side = 0;
		};
		std::vector<Near> nearby;
	};
	void update(hud::HudManager& hud, const Context& context, float seconds);
	// Shows a help key's text (and plays its voice, `sound`, or the key
	// itself) unless help is off or it was shown already; true when it
	// shows.
	bool show(hud::HudManager& hud, const std::string& key, const std::string& sound = {});
	void reset() { m_shown.clear(); }
	static constexpr float kShowSeconds = 6; // the timer the help manager hands 0x0074f2b0 (approximate)

private:
	std::set<std::string> m_shown;
	bool m_enabled = true;
	float m_timer = 0;
	const world::ObjectTemplate* m_vehicle = nullptr;
	const world::ObjectTemplate* m_weapon = nullptr;
	bool m_lowArmorShown = false;
	std::function<std::string(const std::string&)> m_localize;
	std::function<void(const std::string&)> m_voiceHook;
};

// AbilityHud's sound for what the player receives (0 heal, 1 repair, 2
// ammo); "" when the template has none.
std::string abilitySound(const world::ObjectTemplate& receiver, int what);

} // namespace dice::hfe::menu
