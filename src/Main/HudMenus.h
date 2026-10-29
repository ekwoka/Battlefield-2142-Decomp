// The HUD's menus that the game drives from console methods: the spawn
// screen's squad menu (BF2142.exe's SquadMenu, 0x007a4000-0x007a8800: the
// squad list, creating a squad, locking it, the leader's invite list) and
// the commander screen's tabs (CommanderMenu, 0x007a9de0-0x007a9eb0).
//
// The squad list (list data 5, "SquadMemberList", built by 0x007a5f10 and
// 0x007a65a0) has a row per squad of the player's team followed by its
// members. Each row carries its id in cell 13 and its kind in cell 14
// (2 a squad, 4 a member; 0x007a5940 / 0x007a59d0 read them back). A
// click (SquadMenu.singleClickNew, 0x007a81b0) acts by the row's kind and
// the column under the pointer:
//   - a squad's column 10 (JOIN, or LEAVE on the player's own squad):
//     leaves it (0x007a55f0) or joins it (0x007a5410: only from no squad,
//     and not as the commander, "HUD_HUD_SQUAD_MUST_LEAVE_TO_JOIN"
//     otherwise);
//   - a squad's column 8: its leader locks or unlocks it (0x007a5a60);
//   - other columns select the squad or the member.
// Creating (SquadMenu.setSquadCreateSelect 1, 0x007a5700) opens the create
// box with the first free squad id (SquadCreateTagString) unless the player
// is in a squad ("HUD_HUD_SQUAD_MUST_LEAVE_TO_CREATE"), commands
// ("HUD_HUD_SQUAD_COMMANDER_MUST_RESIGN") or every id is taken
// ("HUD_HUD_SQUAD_MAX_REACHED"); a leader alone in his squad re-creates
// it. SquadMenu.squadCreateLockToggle flips the box's lock and
// SquadMenu.createSquad (0x007a5ad0) makes the squad with the first of the
// game's names (0x008f0000's table at 0x00b3df00: "Alpha", "Bravo", ...)
// no other squad of the team has, then closes the box.
//
// The invite list (list data 8, SquadMenu.setShowInviteList 1, a leader's
// only, 0x007a7dc0) lists the team's players, the leader's members
// selected (0x007a7ac0). A click (SquadMenu.singleClickInvite, 0x007a8420
// → 0x007a7e20, the player id in cell 2) selects a player outside the
// squad while there is room (six less the members) or unselects a member;
// SquadMenu.applyInviteList (0x007a84a0) removes the unselected members
// (squad manager slot 0x38) and invites the selected players (slot 0x30).
// Offline there is nobody to answer an invite: a bot in no squad takes it
// at once (a stand-in for the invite's answer).
#pragma once

#include <array>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace dice::hfe {
class Console;
namespace game {
class GameWorld;
}
namespace hud {
class HudManager;
}
} // namespace dice::hfe

namespace dice::hfe::menu {

// The game's squad names (0x00b3df00; 0 is "Unassigned").
const char* squadName(int squad);

class SquadMenu {
public:
	SquadMenu(game::GameWorld& round, hud::HudManager& hud);

	// SquadMenu.* console methods, for the local player.
	void bind(Console& console);
	// The HUD's text ids to the language's text.
	void setLocalizer(std::function<std::string(const std::string&)> localize) { m_localize = std::move(localize); }
	// Messages the menu shows (HUD_HUD_SQUAD_*, text ids).
	std::function<void(const std::string&)> onMessage;

	// Each frame: the lists and the variables (SquadCreateShow,
	// SquadCreateLockedShow, SquadCreateTagString, NrOfSquads,
	// NrOfUnassigned, InviteListShow, IsInSquad, IsSquadLeader).
	void update(int player);

	// What the console methods do, for `player`.
	bool setCreateSelect(int player, bool select);
	void toggleCreateLock() { m_createLocked = !m_createLocked; }
	bool createSquad(int player);
	void setShowInviteList(int player, bool show);
	void applyInviteList(int player);
	void clickSquadList(int player, int row, int column);
	void clickInviteList(int player, int row);

	// The name typed in the create box (SquadMenu.setSquadNameSelect, edit
	// data 2): the squad createSquad makes takes it (ChangeSquadName, 0x16).
	std::function<std::string()> typedName;
	std::string squadLabel(int team, int squad) const;
	// A squad's lock (its leader's; the squad manager's slot 0x20).
	bool locked(int team, int squad) const { return m_locked.count({team, squad}) != 0; }
	bool createShown() const { return m_createShow; }
	bool inviteListShown() const { return m_inviteShow; }
	// The first squad id with no members, 0 when all nine are taken
	// (HUD manager slot 0x310).
	int freeSquad(int team) const;

private:
	std::string text(const std::string& key) const { return m_localize ? m_localize(key) : key; }
	void message(const std::string& key)
	{
		if (onMessage) {
			onMessage(key);
		}
	}
	bool join(int player, int squad);
	void leave(int player);

	game::GameWorld& m_round;
	hud::HudManager& m_hud;
	std::function<std::string(const std::string&)> m_localize;
	bool m_createShow = false;   // +0xec
	bool m_createLocked = false; // +0xee
	bool m_inviteShow = false;   // +0xa5
	std::set<int> m_selected;    // +0x104: invited, or members kept
	std::set<int> m_unselected;  // +0x110: members to remove
	std::set<std::pair<int, int>> m_locked;
	std::map<std::pair<int, int>, std::string> m_names; // squads named in the create box
};

// The commander screen's tabs (CommanderMenu.toggleSquadTab, toggleMapTab,
// toggleCommanderToolsTab: 0x007a9de0, 0x007a9e50, 0x007a9eb0 flip the
// menu's +0x1c0, +0x1c3 and +0x1c4, the HUD variables
// CommanderSquadTabShowFull, CommanderMapTabShowFull and
// CommanderToolsTabShowFull; closing the squad tab also clears +0x1c1,
// ShowSquadMemberList). Their first state isn't known (the menu's
// constructor wasn't found): open.
struct CommanderTabs {
	bool squad = true, squadMembers = false, map = true, tools = true;
	void toggleSquad()
	{
		squad = !squad;
		if (!squad) {
			squadMembers = false;
		}
	}
	void apply(hud::HudManager& hud) const;
};

// The commander screen's squad list (list data 15, "CommanderSquadList";
// CommanderMenu.singleClick, 0x007ab380): a row per squad of the team
// with members, its id in cell 13 and its kind (0) in cell 14 (0x007a9c90,
// 0x007a9c00). A click on column 4 makes that squad the only one selected
// and shows its members (0x007a9f10: menu state 0x11, list data 14 and
// CurrentSquadName, CurrentSquadLeaderName); a click elsewhere selects it
// alone too (slot 0x1c; with Shift slot 0x20 adds it, not done here).
// Column 3 mutes the squad leader's voice (slot 0x300), not done here.
// The commander's orders go to the selected squads (0x007a9f60 sends one
// to each squad slot 0x34 reports selected).
class CommanderMenu {
public:
	CommanderMenu(game::GameWorld& round, hud::HudManager& hud);
	// CommanderMenu.* console methods for the local player.
	void bind(Console& console);
	void update(int player);
	void click(int player, int row, int column);
	void deselect();
	const std::set<int>& selected() const { return m_selected; }
	CommanderTabs tabs;

private:
	game::GameWorld& m_round;
	hud::HudManager& m_hud;
	std::set<int> m_selected;
	int m_current = -1; // +0xac: the squad whose members show
};

// A scoreboard player row's look (the scoreboard's row filler 0x007bb540,
// reached through the HUD manager's list data, slot 0x214 → 0x007c6d70:
// data 2 and 1 are the scoreboard's +0x38c friendly and +0x394 enemy
// lists, 10 and 9 its totals +0x390 / +0x398, made by 0x007b9aa0). The
// name and the numbers take one colour from three static tables
// (0x008f24b0, 0x008f2350, 0x008f2400), indexed 2 on the local player's
// team's list and 1 on the other:
//   - a player not alive (player slot 0x74, +0xd5) is black (0x00b3bec0:
//     (0, 0, 0, 1) at both indices); so is one the HUD manager's slot 0x410
//     test or the player's +0x404 (slot 0x3bc) flags;
//   - a member of the local player's squad (slot 0x120, squad > 0) on his
//     team's list, and the local player himself while he commands (slot
//     0x128, scoreboard +0x25), are green (0x00b3c050: (0.031, 0.467,
//     0.008));
//   - other players are blue on the friendly list (0x00b3c320 index 2:
//     (0, 0.38, 1)) and red on the enemy one (index 1: (0.992, 0, 0.004)).
// The rows' backgrounds (0x007dd5d0): the local player's row (0.392,
// 0.392, 0.392, 0.35) (0x00a0bc50), else every odd row (0.651, 0.651,
// 0.651, 0.8) (0x00a0bc30, alpha 0x3f4ccccd), over the list's (0.686)
// grey.
struct ScoreboardRow {
	bool alive = false;
	bool friendlyList = false; // on the local player's team's list
	bool sameSquad = false;    // in the local player's squad (squad > 0)
	bool local = false;        // the local player
	bool localCommands = false; // the local player is the commander
	bool hidden = false;       // the HUD manager's 0x410 or the player's +0x404
};
struct ScoreboardStyle {
	std::array<float, 4> text;
	std::array<float, 4> background; // alpha 0: none
};
ScoreboardStyle scoreboardStyle(const ScoreboardRow& row, int rowIndex);

// The scoreboard by squads (the "Squads" tab, scoreboard.setToggleShow 1):
// 0x007bad00's commander header ("HUD_HUD_MENU_COMMANDER_HEADING", no
// totals) and the commander, then per squad 0x007baed0's header row (the
// squad's number and name, squad 0 "HUD_TEXT_MENU_SPAWN_SQUAD_UNASSIGNED"
// last) whose cells 0x007ba570 fills with the squad's score, teamwork,
// kills and deaths and its average ping, then its members by score. The
// headers' text (0.5, 0.5, 0.5) (DAT_00a0bc60), the local player's squad's
// (0.141, 0.502, 0.259) (DAT_00a0bba0), over (0.843, 0.847, 0.847, 0.9)
// (DAT_00a0bc40, alpha 0x3f666666).
struct ScoreboardMember {
	int index = 0;   // into the caller's rows (sorted by score)
	int squad = 0;
	bool commander = false;
	int score = 0, teamwork = 0, kills = 0, deaths = 0, ping = 0;
};
struct ScoreboardLine {
	int member = -1;      // a member's index, or -1 for a header
	int squad = 0;        // a header's squad (-1 the commander's)
	int score = 0, teamwork = 0, kills = 0, deaths = 0, ping = 0; // a squad header's totals
	bool totals = false;  // the cells carry them (not the commander's)
};
std::vector<ScoreboardLine> scoreboardBySquad(const std::vector<ScoreboardMember>& members);

} // namespace dice::hfe::menu
