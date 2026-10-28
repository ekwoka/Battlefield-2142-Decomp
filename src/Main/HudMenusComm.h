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

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
namespace hud {
class HudManager;
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

} // namespace dice::hfe::menu
