// The comm roses (BF2142.exe's HUD manager's radio interfaces, class
// vtable 0x009b6528, built by 0x007b88e0): the radio menu (Q, c_GIRadioComm)
// on foot (RadioInterface) and in a vehicle (RadioVehicleInterface), the
// tactical roses (T, c_GITacticalComm: SquadInterface, SquadLeaderInterface,
// CommanderInterface) and the cameras'. The HUD manager names each and
// gives its entry count after creating them (slot 0, 0x007b7070, from
// 0x0075e1f0): Radio 11, RadioVehicle 11, SquadLeader 11, Squad 11,
// Lonewolf 0, Commander 11, DemoCamera 3, DemoRecorder 9, Camera 3.
//
// Menu/HUD/HudSetup/Communication/<Rose>.con fills a rose with
//   <name>Interface.setStringMap Interface_1:<n> <text id> "<command>"
// (slot 0x20, 0x007b8310: the text localised through the HUD manager's
// slot 0x38 into the text map at +8, the command into the map at +0x14)
// and setVariableMap (slot 0x24, 0x007b8520: a value per key at +0x20).
// A rose's level is its path (+0x2c, "1" at the start: keys are
// "Interface_<path>:<n>").
//
// - Opening (slot 0x14, 0x007b7130): the path back to "1", each entry's
//   text and value into the HUD variables <name><n>String and
//   <name><n>Variable, Radio<n>MouseOver cleared.
// - RadioInterface.mouseOver <n> (slot 0x10, 0x007b70b0): Radio<n>MouseOver
//   1 for that entry, 0 for the others (every rose's buttons call it).
// - <name>Interface.selectOrder <n> (slot 0xc, 0x007b73d0): when the level
//   below the entry ("Interface_<path>:<n>:<i>") has texts, they replace
//   the entries' (the others blank) and the path goes down to it; else
//   the entry's command runs through the console with a new line (the HUD
//   manager's slot 0x420) and the rose closes (+4).
//
// The Radio.* methods behind the entries (the HUD manager's slots):
// callForMedic (0x198 -> 0x007c7c10), callForAmmo (0x1a0 -> 0x007c7c70)
// and callForRepair (0x1a4 -> 0x007c7ca0) hand the game's slot 0xd8
// (0x0042d220) the codes 0x33, 0x34 and 0x35, which say "medic" on
// TCLocalTeamMedic, "need_ammo" and "need_repair" on TCTeam;
// sendSpottedMessage <bool> <vehicle type> (0x17c) and
// setSpottedMenuActive (0x268, the middle button's alternate command)
// open the spotted rose (SpottedInterfaceShow, HudElementsSpottedComm.con,
// whose buttons say "game.radioMessage TCTeam <kind>_spotted").
// game.radioMessage <TargetChannel> <voice> (0x00423e61) is the local
// player's message on that channel.
#pragma once

#include <cmath>
#include <cstdint>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
namespace hud {
class HudManager;
}
namespace host {
class HudWorld;
}
namespace game {
class GameWorld;
}
namespace world {
class ObjectTemplate;
}
} // namespace dice::hfe

namespace dice::hfe::menu {

class CommRose {
public:
	CommRose(hud::HudManager& hud, std::string name, int entries);
	const std::string& name() const { return m_name; }
	int entries() const { return m_entries; }

	void setLocalizer(std::function<std::string(const std::string&)> localize) { m_localize = std::move(localize); }
	// slot 0x20 / 0x24.
	void setStringMap(const std::string& key, const std::string& text, const std::string& command);
	void setVariableMap(const std::string& key, const std::string& value);
	// slot 0x14: back to the first level, the entries' texts shown.
	void reset();
	// slot 0xc: the command the entry runs ("" when it went a level down
	// or has none). Closes the rose when it runs one.
	std::string selectOrder(const std::string& entry);
	const std::string& path() const { return m_path; }
	bool open() const { return m_open; }
	void setOpen(bool open) { m_open = open; } // slot 0x30 (+4)

	// The entry's text and command at the current level (n from 1).
	std::string text(int entry) const;
	std::string command(int entry) const;

private:
	std::string key(const std::string& path, int entry) const;
	void showLevel(const std::string& path);

	hud::HudManager& m_hud;
	std::string m_name;
	int m_entries;
	std::string m_path = "1"; // +0x2c
	bool m_open = false;      // +4
	std::map<std::string, std::string> m_texts;     // +8
	std::map<std::string, std::string> m_commands;  // +0x14
	std::map<std::string, std::string> m_variables; // +0x20
	std::function<std::string(const std::string&)> m_localize;
};

// The roses of the HUD manager and their console methods.
class CommRoses {
public:
	explicit CommRoses(hud::HudManager& hud);
	// <name>Interface.setStringMap / setVariableMap / selectOrder and
	// RadioInterface.mouseOver; `run` executes a selected command line.
	void bind(Console& console, std::function<void(const std::string&)> run);
	void setLocalizer(const std::function<std::string(const std::string&)>& localize);
	CommRose* find(const std::string& name);

	// What the keys open (the HUD manager's menu states, 0x0079ab90 hides
	// them: 5/6 the radio, 7 spotted, 4 Squad, 8 SquadLeader, 9 Commander,
	// 3 Lonewolf): the radio rose (RadioVehicle aboard) or the tactical
	// one for the player's role, its <name>InterfaceShow 1 and the rest 0.
	void openRadio(bool inVehicle);
	void openTactical(bool commander, bool squadLeader, bool inSquad);
	void openSpotted();
	void close();
	// The rose shown ("" for none).
	const std::string& shown() const { return m_shown; }

private:
	void show(const std::string& name);

	hud::HudManager& m_hud;
	std::vector<CommRose> m_roses;
	std::string m_shown;
	std::function<void(const std::string&)> m_run;
};

// The radio's spam check (player slot 0x340, 0x00591d40; its reset in
// the player's tick, 0x0059bd80), before a player's radio message goes out
// (the client's game slot 0x50, 0x004b2b60; a server's 0x004a5a80) and a
// spot (0x0042d110). The limits are the server's (data block 0):
// sv.radioSpamInterval (DAT_00a3fcc0 +0x44, 6 s), radioMaxSpamFlagCount
// (+0x48, 6) and radioBlockedDurationTime (+0x4c, 30 s); a commander
// (player slot 0x128) may send twice as many and is blocked half as long.
// - check (count): with the count (+0x17c, slots 0x344 / 0x348) at the
//   limit the message is refused: the first refusal starts the block's
//   clock (+0x184, slot 0x354) and the refusals are counted (+0x180,
//   slots 0x34c / 0x350); else, when `count` (the callers pass whether
//   to count it), the first message starts the clock and the count goes up.
// - tick: under the limit, the count goes (all three cleared, the clock
//   -1) once radioSpamInterval has passed since the clock started; at the
//   limit, once radioBlockedDurationTime has.
// A refused message plays the local "spam" voice and the personal line
// HUD_HUD_MENU_CHAT_SPAM_MESSAGE with #SEC# the seconds left: clock +
// radioBlockedDurationTime + 1 - now, rounded (0x004b2b60).
struct RadioSpam {
	int interval = 6, maxCount = 6, blockedTime = 30;
	int count = 0, refused = 0;
	double clock = -1;
	bool check(bool countIt, bool commander, double now)
	{
		const int limit = commander ? maxCount * 2 : maxCount;
		if (limit <= count) {
			if (refused == 0) {
				clock = now;
			}
			++refused;
			return true;
		}
		if (countIt) {
			if (clock < 0) {
				clock = now;
			}
			++count;
		}
		return false;
	}
	void tick(bool commander, double now)
	{
		const int limit = commander ? maxCount * 2 : maxCount;
		const int blocked = commander ? blockedTime / 2 : blockedTime;
		const double elapsed = now - clock;
		const bool reset = count < limit ? clock != now && !(static_cast<double>(interval) > elapsed)
										 : !(static_cast<double>(blocked) > elapsed);
		if (reset) {
			count = refused = 0;
			clock = -1;
		}
	}
	int secondsLeft(double now) const
	{
		return static_cast<int>(std::lround(clock + blockedTime + 1.0 - now));
	}
};

// What Radio.sendSpottedMessage spots and says (0x007c8160).
// - The crosshair object (0x0075be30 with the HUD's ray 0x0075bfe0): the
//   ray meets an object; when it and its root are canBeSpotted (vehicleHud
//   block +0xc2, 1 by default) the root is taken; the last player in the
//   seats of its first seat holder up the parents (0x0042b2f0, component
//   0xc4c5), when of the other team, makes it the spotted object, which
//   becomes that player's soldier (slot 0xb4) when the object's template
//   has usePlayerIcon (+0xb8). Its network id goes to the event (+0x1f0).
//   An empty vehicle is never spotted. Here the vehicle is the root (the
//   rewrite's vehicles are whole objects); one that isn't canBeSpotted
//   leaves its rider's soldier spotted.
// - The voice (with the method's first argument; the comm rose's "Radio.
//   sendSpottedMessage 1 VTNoVehicleType"): the game's slot 0xd0
//   (0x0042e9d0) names it, then slot 0x50 says it on channel 1 (TCTeam,
//   with its own spam check). The name is the radio.spottedMessage
//   (comp 0xc4b7 +0x74, executor 0x0054fb6d) of the spotted object's
//   Radio component's owner: a soldier's kit (Pac_Recon "sniper_spotted",
//   the other kits "infantry_spotted"), a vehicle's own template
//   (apc_spotted, titan_spotted, heli_spotted ...; 40 templates); an object
//   without one with usePlayerIcon takes its occupants' soldiers' kits';
//   empty or nothing spotted, "spotted". The voice is said even when the
//   spot itself was refused (no object and no point, or the spam check).
struct SpotChoice {
	int player = -1;     // the enemy reported (-1: none)
	uint32_t object = 0; // the spotted object: the vehicle or the soldier
	std::string voice = "spotted";
};
// For the enemy player `enemy` under the crosshair (-1 for none).
SpotChoice spotChoice(const host::HudWorld& world, game::GameWorld& round, int enemy);
// The radio.spottedMessage of a template (its quotes taken off), "" without one.
std::string spottedMessage(const world::ObjectTemplate* t);
// The crosshair's enemy of `viewer` as a joined client sees it (GameWorld::
// crosshairTarget's test over the HudWorld's players: a soldier as a 0.6 m
// sphere about its middle, a vehicle as a 2.5 m one about its origin, the
// nearest along the ray from `eye` along the unit `dir` within `reach`
// with nothing in between); -1 for none.
int crosshairEnemy(const host::HudWorld& world, game::GameWorld& round, int viewer, const float eye[3], const float dir[3],
	float reach = 300);

// Where a joined game's left click goes (the HUD's input, 0x00754290: the
// pointer's buttons for the HUD's own nodes while playing). With a
// soldier: the HUD (HudManager::click: the roses' entries, the scoreboard,
// the map's menus); without one, the spawn screen: a spawn icon picks
// that point, elsewhere any point (the client's round has no control
// points of its own).
enum class JoinedClick { Hud, SpawnAt, AnySpawn };
inline JoinedClick joinedClick(bool hasControlObject, int marker)
{
	if (hasControlObject) {
		return JoinedClick::Hud;
	}
	return marker >= 0 ? JoinedClick::SpawnAt : JoinedClick::AnySpawn;
}

} // namespace dice::hfe::menu
