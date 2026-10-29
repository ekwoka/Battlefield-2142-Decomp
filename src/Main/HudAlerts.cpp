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
	// The played list (GeneralSettings' slots 0x174 / 0x178): the settings
	// object keeps and saves the lines and then calls this.
	bindMethod(console, "GeneralSettings.setPlayedVOHelp", access, [this](std::string key) { m_played.insert(unquoted(key)); });
}

// The help manager's messages (0x00645d40: the keys at +0x208 + i x 0x1c,
// the play-time limits at +0xa8 + i x 4, the event flags at +0x53 + i) and
// the tick's conditions (0x00646cb0's cases), as codes:
//   1 no menu (HUD manager slot 0x228 == 0) and on foot (+0x205);
//   2 that after 3 s of the round; 3 that and alive (player slot 0x74);
//   4 that after 20 s; 5 after 60 s; 6 in a squad (slot 0x120), not its
//   leader (slot 0x150); 7 a squad leader with no menu; 8 the commander
//   screen (slot 0x234); 9 man down; 10 alive with no menu; 11 the spawn
//   screen (menu state 1); 12 no menu; 13 alive in a vehicle;
//   0 not by the tick here: an event's (the flag), the template keys'
//   (handled by update()), or (the event-only ones) shown by the game's
//   events, which the rewrite doesn't raise yet.
// A voice "=" is the key itself (the cases ending at 0x00649c0e).
const HelpPopups::Tutorial HelpPopups::kTutorials[83] = {
	{"HUD_HELP_PLAYER_CONTROLS_moveMouseToLook", 0, false, 1, ""},
	{"HUD_HELP_PLAYER_CONTROLS_moveForwardsBackwards", 0, false, 2, "move_help"},
	{"HUD_HELP_PLAYER_CONTROLS_fireLeftMouse", 0, false, 3, "="},
	{"HUD_HELP_WEPAON_HANDHELD_GENERAL_reloadWeapon", 0, false, 0, ""},
	{"HUD_HELP_PLAYER_CONTROLS_zoomAltFire", 0, false, 3, "weapon_zoom"},
	{"HUD_HELP_PLAYER_CONTROLS_jump", 7200, false, 1, ""},
	{"HUD_HELP_PLAYER_CONTROLS_crouch", 18000, false, 1, ""},
	{"HUD_HELP_PLAYER_CONTROLS_lieDown", 0, true, 1, ""},
	{"HUD_HELP_PLAYER_CONTROLS_strafeLeftRight", 7200, false, 1, ""},
	{"HUD_HELP_SQUAD_squadJoined", 0, false, 6, ""},
	{"HUD_HELP_SPAWNSCREEN_SQUADTAB_createSquad", 36000, false, 14, "="},
	{"HUD_HELP_SQUAD_squadCreated", 0, true, 22, ""},
	{"HUD_HELP_SPAWNSCREEN_SQUADTAB_joinSquad", 0, true, 14, "="},
	{"HUD_HELP_PLAYER_CONTROLS_parachute", 0, false, 15, "parachute_help"},
	{"HUD_HELP_MENU_CONTROLS_callMedicWhenManDown", 0, false, 9, "man_down_help"},
	{"HUD_HELP_MENU_CONTROLS_COMMOROSE_slcommoRoseAvailable", 0, false, 7, "="},
	{"HUD_HELP_COMMANDER_placeArtillery", 0, true, 23, "place_artillery"},
	{"HUD_HELP_COMMANDER_placeBombardment", 0, true, 24, "place_orbitalStrike"},
	{"HUD_HELP_COMMANDER_placeSupplies", 0, true, 21, "place_supplies"},
	{"HUD_HELP_COMMANDER_place_UAV", 0, true, 0, ""},
	{"HUD_HELP_COMMANDER_artilleryFired", 0, true, 8, "artillery_fired"},
	{"HUD_HELP_COMMANDER_bombardmentFired", 0, true, 8, "orbitalStrike_fired"},
	{"HUD_HELP_COMMANDER_UAVplaced", 0, true, 0, ""},
	{"HUD_HELP_SQUAD_callForTarget", 0, true, 21, ""},
	{"HUD_HELP_COMMANDER_commanderApply", 0, true, 25, "="},
	{"HUD_HELP_SPAWNSCREEN_SQUADTAB_applyCommander", 72000, false, 16, "="},
	{"HUD_HELP_COMMANDER_SQUADLIST_squadList", 0, true, 8, "="},
	{"HUD_HELP_COMMANDER_SQUADLIST_clickSquadList", 0, false, 17, "="},
	{"HUD_HELP_COMMANDER_MINIMAP_description", 0, false, 0, ""},
	{"HUD_HELP_COMMANDER_MAP_QUICKMENU", 0, false, 18, "="},
	{"HUD_HELP_COMMANDER_MAP_ZOOM_inOut", 0, false, 8, "="},
	{"HUD_HELP_COMMANDER_SATELLITESCAN_description", 0, false, 8, "="},
	{"HUD_HELP_COMMANDER_UAV_description", 0, false, 8, "="},
	{"HUD_HELP_COMMANDER_ARTILLERY_description", 0, false, 8, "="},
	{"HUD_HELP_COMMANDER_BOMBARDMENT_description", 0, false, 8, "="},
	{"HUD_HELP_COMMANDER_SUPPLIES_description", 0, false, 8, "="},
	{"", 0, false, 0, ""},
	{"HUD_HELP_SQUAD_LEADER_VOIP_commanderChannel", 0, true, 21, "="},
	{"HUD_HELP_COMMANDER_VOIP_allSquadLeadersChannel", 0, true, 8, "="},
	{"HUD_HELP_COMMANDER_COMMUNICATION_VOIPTOGGLE", 0, true, 8, "="},
	{"HUD_HELP_COMMANDER_MAP_REQUESTS_received", 0, true, 0, ""},
	{"HUD_HELP_COMMANDER_SQUADLIST_orderIcon", 0, true, 8, "="},
	{"HUD_HELP_COMMANDER_MAP_SATELLITEVIEW_moveKeys", 0, true, 8, "="},
	{"HUD_HELP_COMMANDER_SQUADLIST_commoMenu", 0, false, 0, ""},
	{"HUD_HELP_COMMANDER_MAP_FILTERS", 0, false, 0, ""},
	{"HUD_HELP_SQUAD_LEADER_mainMapRightMouseMenu", 0, false, 18, "="},
	{"", 0, false, 0, ""}, // 0x2e: the vehicle's helpStringKey (update())
	{"", 0, false, 0, ""},
	{"HUD_HELP_KIT_MEDIC_NAMETAG_indicatorBar", 0, true, 0, ""},
	{"HUD_HELP_KIT_SUPPORT_NAMETAG_indicatorBar", 0, true, 0, ""},
	{"HUD_HELP_KIT_ENGINEER_NAMETAG_indicatorBar", 0, true, 0, ""},
	{"HUD_HELP_GAMEPLAY_captureFlag", 0, true, 21, "="},
	{"HUD_HELP_GAMEPLAY_exitToCapture", 0, true, 21, "="},
	{"HUD_HELP_MENU_CONTROLS_COMMOROSE_specificSpotted", 0, true, 21, "="},
	{"HUD_HELP_COMMANDER_uavDamaged", 0, true, 8, "="},
	{"HUD_HELP_COMMANDER_radarDamaged", 0, true, 8, "="},
	{"HUD_HELP_COMMANDER_artilleryDamaged", 0, true, 8, "="},
	{"HUD_HELP_COMMANDER_bombardmentDamaged", 0, true, 8, "="},
	{"", 0, false, 0, ""}, // 0x3a: low armour (update())
	{"", 0, false, 0, ""}, // 0x3b: low ammunition (update())
	{"HUD_HELP_SPAWNSCREEN_spawnOnSL", 0, true, 21, "="},
	{"HUD_HELP_MENU_CONTROLS_zoomMap", 0, false, 10, ""},
	{"HUD_HELP_SQUAD_recievedTarget", 0, true, 21, "place_artillery"},
	{"HUD_HELP_SPAWNSCREEN_KITTAB_pickArmyAndKit", 0, false, 11, "="},
	{"HUD_HELP_SPAWNSCREEN_KITTAB_pickSpawnPoint", 0, false, 11, "="},
	{"HUD_HELP_SPAWNSCREEN_KITTAB_useUnlock", 0, true, 0, "="},
	{"HUD_HELP_SPAWNSCREEN_KITTAB_trySquads", 21600, false, 11, "="},
	{"HUD_HELP_GAMEPLAY_climbLadder", 0, false, 19, "ladder_help"},
	{"HUD_HELP_PLAYER_CONTROLS_radioMessages", 7200, false, 12, "comm_rose"},
	{"HUD_HELP_MENU_COMMOROSE_slCommoRoseFirstUse", 0, true, 22, "="},
	{"", 7200, true, 0, "="},
	{"HUD_HELP_VEHICLE_GENERAL_CONTROLS_enterVehicle", 0, false, 20, "enter_Vehicle_help"},
	{"HUD_HELP_VEHICLE_CONTROLS_exitVehicle", 0, false, 13, "exit_Vehicle_help"},
	{"", 0, false, 0, ""}, // 0x49: the closeTo keys (update())
	{"HUD_HELP_PLAYER_CONTROLS_sprint", 0, true, 26, "HUD_HELP_PLAYER_CONTROLS_sprint"},
	{"HUD_HELP_PLAYER_CONTROLS_lieDown", 18000, false, 1, ""},
	{"HUD_HELP_MENU_CONTROLS_changeWeapon", 0, false, 4, "weapon_change"},
	{"HUD_HELP_MENU_CONTROLS_largeMap", 7200, false, 5, "open_map"},
	{"HUD_HELP_MENU_CONTROLS_scoreBoard", 18000, false, 1, ""},
	{"HUD_HELP_MENU_CONTROLS_spawnScreen", 0, false, 0, ""},
	{"", 0, false, 0, ""}, // 0x50: the weapon's help and alt help (update())
	{"", 0, false, 0, ""}, // 0x51: the vehicle's help (update())
	{"HUD_HELP_WORLD_PLAYER_Squadleader_inventory_selectunlock", 0, true, 22, "="},
};

void HelpPopups::startEventFlags()
{
	if (!m_eventFlagsSet) {
		m_eventFlagsSet = true;
		for (size_t i = 0; i < m_eventOnly.size(); ++i) {
			m_eventOnly[i] = kTutorials[i].eventOnly;
		}
	}
}

void HelpPopups::enableTutorial(int index)
{
	// 0x00751540 -> player slot 0x24c -> 0x00645c40(index, 0), unless done
	// (slot 0x254).
	startEventFlags();
	if (index >= 0 && index < 83 && !m_tutorialDone[static_cast<size_t>(index)]) {
		m_eventOnly[static_cast<size_t>(index)] = false;
	}
}

void HelpPopups::disableTutorial(int index)
{
	// 0x00751580 -> slot 0x250 -> 0x00645c40(index, 1).
	startEventFlags();
	if (index >= 0 && index < 83 && !m_tutorialDone[static_cast<size_t>(index)]) {
		m_eventOnly[static_cast<size_t>(index)] = true;
	}
}

bool HelpPopups::tutorialHolds(int index, const Context& c) const
{
	const bool noMenu = !c.menuOpen && !c.spawnScreen;
	switch (kTutorials[index].condition) {
	case 1: return noMenu && c.onFoot;
	case 2: return c.roundTime > 3 && noMenu && c.onFoot;
	case 3: return noMenu && c.onFoot && c.alive;
	case 4: return c.roundTime > 20 && noMenu && c.onFoot && c.alive;
	case 5: return c.roundTime > 60;
	case 6: return c.squad != 0 && !c.squadLeader;
	case 7: return c.squadLeader && noMenu;
	case 8: return c.commanderScreen;
	case 9: return c.manDown;
	case 10: return c.alive && noMenu;
	case 11: return c.spawnScreen;
	case 12: return noMenu;
	case 13: return c.alive && c.inVehicle;
	// The menu states (0x00646cb0's cases 0x0a, 0x19, 0x1d / 0x2d; the
	// spawn screen's halves set by SpawnManager.toggleMembers, 0x007b8b80:
	// 1 the kits, the members 0x12 for a squad leader and 0x13 for the
	// rest), the parachute (case 0x0d: the HUD manager's slot 0xb0 object's
	// slot 0x78), a ladder (0x43: a Ladder, class 40000, within 3 m,
	// 0x00646880), a vehicle to enter (0x47: an entry within 3 m,
	// 0x005a98e0), the commander screen with the team's squads (0x1b).
	case 14: return c.menuState == 0x13 && c.squad == 0;
	case 15: return c.onFoot && c.parachute;
	case 16: return c.menuState == 0x13 || c.menuState == 0x12;
	case 17: return c.commanderScreen && c.teamSquads > 0;
	case 18: return c.menuState == 0x12;
	case 19: return c.onFoot && c.ladderNear;
	case 20: return c.onFoot && c.vehicleNear;
	// The event-only messages' own cases (enabled by the HUD manager's slot
	// 0x128, 0x00751540): unconditional, a squad leader (slot 0x150), the
	// commander (0x128), the artillery's and the orbital strike's
	// placement (menu states 0x17, 0x22), on foot (+0x205).
	case 21: return true;
	case 22: return c.squadLeader;
	case 23: return c.menuState == 0x17;
	case 24: return c.menuState == 0x22;
	case 25: return c.commander;
	case 26: return c.onFoot;
	default: return false;
	}
}

bool HelpPopups::tutorial(hud::HudManager& hud, const Context& context, float seconds)
{
	if (!m_tutorialStarted) {
		// 0x006467f0: the played keys and those past their play-time limit.
		m_tutorialStarted = true;
		startEventFlags();
		for (size_t i = 0; i < m_tutorialDone.size(); ++i) {
			const Tutorial& t = kTutorials[i];
			if (*t.key && (played(t.key) || (t.limit > 0 && context.playTime >= t.limit))) {
				m_tutorialDone[i] = true;
			}
		}
	}
	if (!m_enabled || !context.playing) {
		return false;
	}
	// 0x00649d80: 10 s after the last message, the first that holds.
	m_sinceTutorial += seconds;
	if (m_tutorialShowing) {
		if (m_sinceTutorial <= 10) {
			return false;
		}
		m_tutorialShowing = false;
		m_sinceTutorial = 0;
	}
	if (m_timer > 0) {
		return false; // a template key's help is up
	}
	for (int i = 0; i < 83; ++i) {
		const Tutorial& t = kTutorials[i];
		if (m_eventOnly[static_cast<size_t>(i)] || m_tutorialDone[static_cast<size_t>(i)] || !*t.key || !tutorialHolds(i, context)) {
			continue;
		}
		m_tutorialDone[static_cast<size_t>(i)] = true; // 0x006467a0
		// Case 0x1d shows message 0x2d's text (both cases share their code).
		const Tutorial& shown = i == 0x1d ? kTutorials[0x2d] : t;
		const std::string voice = std::string(shown.voice) == "=" ? std::string(shown.key) : std::string(shown.voice);
		if (!show(hud, shown.key, voice.empty() ? std::string(" ") : voice)) {
			continue;
		}
		m_tutorialShowing = true;
		m_sinceTutorial = 0;
		return true;
	}
	return false;
}

bool HelpPopups::show(hud::HudManager& hud, const std::string& rawKey, const std::string& sound)
{
	const std::string key = unquoted(rawKey);
	if (!m_enabled || key.empty() || m_shown.count(key) || played(key)) {
		return false;
	}
	m_shown.insert(key);
	hud.setString("IngameHelpString", m_localize ? m_localize(key) : key);
	hud.set("IngameHelpShow", 1);
	m_timer = kShowSeconds;
	// A voice of " ": the message has none (0x00646cb0's text-only cases).
	if (m_voiceHook && sound != " ") {
		m_voiceHook(sound.empty() ? key : unquoted(sound));
	}
	// 0x00645c10: recorded as played (GeneralSettings slot 0x174).
	m_played.insert(key);
	if (onPlayed) {
		onPlayed(key);
	}
	return true;
}

void HelpPopups::showText(hud::HudManager& hud, const std::string& text, float seconds)
{
	if (text.empty() || seconds <= 0) {
		hud.set("IngameHelpShow", 0);
		m_timer = 0;
		return;
	}
	hud.setString("IngameHelpString", text);
	hud.set("IngameHelpShow", 1);
	m_timer = seconds; // +0x238
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
	// Low armour (0x00646cb0 case 0x3a): the vehicle's armour below 0.3
	// of its maximum.
	if (m_vehicle && !m_lowArmorShown) {
		const std::string key = setting(*m_vehicle, "helpHud.lowArmorHelpStringKey");
		if (!key.empty() && context.vehicleHitPoints > 0 && context.vehicleShare < 0.3f) {
			m_lowArmorShown = true;
			show(hud, key, setting(*m_vehicle, "helpHud.lowArmorHelpSoundKey"));
		}
	}
	// The alt help (case 0x50): the weapon in hand showing its altGuiIndex.
	if (context.zoomed && m_weapon && !m_vehicle && m_zoomedWeapon != m_weapon) {
		m_zoomedWeapon = m_weapon;
		show(hud, setting(*m_weapon, "helpHud.altHelpStringKey"), setting(*m_weapon, "helpHud.altHelpSoundKey"));
	}
	// Low ammunition (case 0x3b): a kit weapon's magazine below 0.3 full.
	if (!m_vehicle && !m_lowAmmoShown) {
		for (const Context::Ammo& a : context.ammo) {
			if (a.weapon && a.share < 0.3f) {
				const std::string key = setting(*a.weapon, "helpHud.lowAmmoHelpStringKey");
				if (!key.empty() && show(hud, key, setting(*a.weapon, "helpHud.lowAmmoHelpSoundKey"))) {
					m_lowAmmoShown = true;
					break;
				}
			}
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
