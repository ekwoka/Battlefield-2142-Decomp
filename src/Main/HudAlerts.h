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
// falls below 0.3 of its maximum (0x00646cb0 case 0x3a), the closeToHelpStringKey{Friendly,Enemy,
// Neutral} when the player comes within closeToDistance of an object that
// has them. Showing (the HUD manager's slot 0x94, 0x0074f2b0) puts the text
// in IngameHelpString, IngameHelpShow on and a timer (+0x238) that the HUD
// manager's update counts down (0x007539c0) before IngameHelpShow goes off.
// The voice: the template's matching help sound key (helpSoundKey,
// lowArmorHelpSoundKey, closeToHelpSoundKey*) goes to the voice hook, the
// sound layer's VoiceMessages::playHelp (gameLogic.messages.addHelpMessage
// <key> <file>, Sound/VoiceMessages_Help.con, keeps the Help_VO files
// there, 0x004d8e40 / 0x004d8280). GeneralSettings.setHelpPopups 0 turns
// the pop-ups off. Each key shows once a round, and once ever: a key shown
// goes to GeneralSettings' played list (0x00645c10 → GeneralSettings slot
// 0x174, GeneralSettings.setPlayedVOHelp, saved in General.con) and a key
// on it (slot 0x178, 0x00645cc0) never shows again.
//
// The help manager (BF2142.exe's per-player helper, the player's +700;
// built by 0x00645d40, ticked by 0x00649d80 from the player's update): 83
// messages (+0x208 + i x 0x1c: HUD_HELP_PLAYER_CONTROLS_*, the squads', the
// commander's tools...), each with a limit of the profile's play time (+0xa8
// + i x 4, seconds; LocalProfileManager slot 0xe0) past which it no longer
// applies and an "event only" flag (+0x53 + i: shown by the game's own
// events, not by the tick). Round start (0x006467f0) marks done those
// already played or past their limit. While the round plays (game logic
// slot 0x164 == 1) the tick waits until 10 s (+0x1f8) have passed since
// the last message, then tries the messages in order (each at most once a
// tick, 0x00645c70) and shows the first whose condition holds (0x00646cb0:
// the menu state, on foot, alive, squad, commander screen, round time...):
// the text for 9 s (HUD manager slot 0x94, 9.0) and its voice. The
// template help keys are among its messages: 0x3a low armour (the armour's
// share below 0.3), 0x3b low ammunition (a kit weapon's rounds below 0.3 of
// a magazine: helpHud.lowAmmoHelpStringKey / lowAmmoHelpSoundKey), 0x50 the
// weapon in hand's help while its HUD shows guiIndex and its alt help
// (altHelpStringKey / altHelpSoundKey) while it shows altGuiIndex (zoomed),
// 0x51 the vehicle's help.
//
// Supply sounds. abilityHud.healingSound, repairingSound and ammoSound
// (class id 0xc4f2, from 0x0055a41f) are the sounds the player's soldier
// or vehicle plays while it is healed, repaired or rearmed.
#pragma once

#include <array>
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
		float vehicleShare = 1;                         // its share of the maximum
		bool zoomed = false;                            // the weapon shows its altGuiIndex
		// The kit's weapons and their magazines' share (rounds / magSize).
		struct Ammo {
			const world::ObjectTemplate* weapon = nullptr;
			float share = 1;
		};
		std::vector<Ammo> ammo;
		// For the help manager's messages.
		bool playing = false;     // the round runs (game logic slot 0x164 == 1)
		bool onFoot = false;      // the player controls his soldier (+0x205)
		bool alive = false;       // player slot 0x74
		bool manDown = false;
		bool inVehicle = false;
		bool spawnScreen = false; // the HUD's menu state 1
		bool menuOpen = false;    // any other menu state (the scoreboard, the maps, the roses)
		bool commanderScreen = false; // HUD manager slot 0x234
		int squad = 0;            // player slot 0x120
		bool squadLeader = false; // slot 0x150
		bool commander = false;   // slot 0x128
		float roundTime = 0;      // 0x00666070
		float playTime = 0;       // the profile's (seconds)
		int menuState = 0;        // the HUD manager's (slot 0x228): 1 kits, 0x12 / 0x13 the members
		bool parachute = false;   // descending under a parachute
		bool ladderNear = false;  // a ladder within 3 m
		bool vehicleNear = false; // an empty vehicle to enter within reach
		int teamSquads = 0;       // the team's squads (the game manager's slot 0xa4)
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
	// IngameHelp.showIngameHelp <text> <seconds> (0x00777cf1 → the HUD
	// manager's slot 0x98 → 0x94, 0x0074f2b0): the text as given for that
	// long; empty text or no time hides it.
	void showText(hud::HudManager& hud, const std::string& text, float seconds);
	void reset()
	{
		m_shown.clear();
		m_tutorialDone.fill(false);
		m_tutorialStarted = false;
		m_eventFlagsSet = false;
	}
	static constexpr float kShowSeconds = 9; // the help manager's (0x00646cb0 hands slot 0x94 9.0)
	// GeneralSettings' played help keys (setPlayedVOHelp), and the hook
	// that records a new one (runs the console line).
	std::function<void(const std::string& key)> onPlayed;
	bool played(const std::string& key) const { return m_played.count(key) != 0; }
	// The help manager's messages (0x00645d40).
	struct Tutorial {
		const char* key;   // "" for the unused ones
		float limit;       // profile play time (s) past which it is done; 0 none
		bool eventOnly;    // +0x53 + i
		int condition;     // see HudAlerts.cpp (0 never by the tick)
		const char* voice; // its VO ("" the key's own)
	};
	static const Tutorial kTutorials[83];
	// The tick (0x00649d80); true when a message shows.
	bool tutorial(hud::HudManager& hud, const Context& context, float seconds);
	// The HUD manager's slots 0x128 / 0x12c (0x00751540 / 0x00751580): a
	// game event lets the tick show an event-only message (its flag
	// +0x53 + i cleared) or takes that back; nothing for a message done.
	void enableTutorial(int index);
	void disableTutorial(int index);

private:
	bool tutorialHolds(int index, const Context& context) const;
	void startEventFlags();
	std::array<bool, 83> m_eventOnly {}; // +0x53 + i
	bool m_eventFlagsSet = false;
	std::set<std::string> m_shown;
	std::set<std::string> m_played;
	std::array<bool, 83> m_tutorialDone {};
	bool m_tutorialStarted = false;
	bool m_tutorialShowing = false; // +500
	float m_sinceTutorial = 0;      // +0x1fc
	const world::ObjectTemplate* m_zoomedWeapon = nullptr;
	bool m_lowAmmoShown = false;
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
