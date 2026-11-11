// The HUD's menus that the game drives from console methods: the spawn
// screen's squad menu (BF2142.exe's SquadMenu, 0x007a4000-0x007a8800,
// constructor 0x007a7620: the squad list, creating a squad, locking it,
// the leader's invite list) and
// the commander screen's tabs (CommanderMenu, 0x007a9de0-0x007a9eb0).
//
// The squad list (list data 5, "SquadMemberList") has the commander's row
// (0x007a4800), then a row per squad of the player's team (0x007a5f10),
// each followed by its members (0x007a65a0, the leader first) only while
// the squad is expanded (0x007a74d0). Each row carries its id in cell 13
// and its kind in cell 14 (0 the commander, 2 a squad, 4 a member;
// 0x007a5940 / 0x007a59d0 read them back). A click
// (SquadMenu.singleClickNew, 0x007a81b0) acts by the row's kind and the
// column under the pointer:
//   - a squad's column 10 (JOIN, or LEAVE on the player's own squad):
//     leaves it (0x007a55f0) or joins it (0x007a5410: only from no squad,
//     and not as the commander, "HUD_HUD_SQUAD_MUST_LEAVE_TO_JOIN"
//     otherwise);
//   - a squad's column 8: its leader locks or unlocks it (0x007a5a60);
//   - a squad's column 12: its arrow expands or collapses it (0x007a7290);
//   - the commander's column 10: APPLY, RESIGN or MUTINY; 12 its arrow;
//   - other columns select the squad or the member.
// The local player's squad opens when he joins it or leads it (the HUD's
// events 2/0x16 and 2/0x18) and closes when he leaves it (2/0x15).
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

#include "Game/GameWorld.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace dice::hfe {
class Console;
namespace game {
class GameWorld;
}
namespace host {
class HudWorld;
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
	// Commander application/resignation is routed through the same commands
	// as the rest of the HUD, including a joined server's event path.
	std::function<void(bool resign)> onCommanderAction;
	// The commander row's MUTINY (0x007a8334 → HUD slot 0x324).
	std::function<void()> onMutiny;
	// A member row's voice box (0x007a65a0 cell 12, while the player has
	// voice, slot 0x2e4): -1 none, 0 quiet, 1 talking (slot 0x2f4), 2 muted
	// (slot 0x2f0). Unset: none.
	std::function<int(int player)> voiceState;
	// A squad's members listed under it (the menu's bitset +0xa8, squads 0..9;
	// none at first: 0x007a7620), and the commander's under his row (+0xa3).
	bool expanded(int squad) const { return m_expanded.count(squad) != 0; }
	// The player as the menu sees him (the joined game's, else the round's).
	bool playerInfoFor(int player, game::GameWorld::PlayerInfo& out) const { return playerInfo(player, out); }
	bool commanderExpanded() const { return m_commanderExpanded; }

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

	// A joined game (HudWorld.h): the players as the server has them, and
	// what the menu changes sent as the original's events (a client's
	// squad manager, ClientSquadManager: JoinSquad 0x10 from 0x004c9bf0,
	// LeaveSquad 0x11 from 0x004c9c50, another player for the leader's
	// kick; ChangeSquadName 0x16 for a new squad, which the server makes and
	// joins the sender to, 0x004c6720; SetPrivateSquad 0x17 for the lock;
	// Invite 0x19). The server's players then show the change.
	struct Remote {
		const host::HudWorld* world = nullptr;
		std::function<void(int squad)> join;
		std::function<void(int squad)> leave;
		std::function<void(int squad, const std::string& name)> create;
		std::function<void(int squad, bool locked)> lock;
		std::function<void(int player, int squad)> invite;
		std::function<void(int player, int squad)> kick;
	};
	void setRemote(Remote remote) { m_remote = std::move(remote); }
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
	// The players: the joined game's, else the round's.
	bool playerInfo(int player, game::GameWorld::PlayerInfo& out) const;
	std::vector<game::GameWorld::PlayerInfo> playerList() const;
	int localPlayer() const;
	Remote m_remote;

	game::GameWorld& m_round;
	hud::HudManager& m_hud;
	std::function<std::string(const std::string&)> m_localize;
	bool m_createShow = false;   // +0xec
	bool m_createLocked = false; // +0xee
	bool m_inviteShow = false;   // +0xa5
	std::set<int> m_selected;    // +0x104: invited, or members kept
	std::set<int> m_unselected;  // +0x110: members to remove
	std::set<int> m_expanded;    // +0xa8
	bool m_commanderExpanded = false; // +0xa3
	int m_lastSquad = 0;              // the local player's, for the squad events
	bool m_lastLeader = false;
	std::set<std::pair<int, int>> m_locked;
	std::map<std::pair<int, int>, std::string> m_names; // squads named in the create box
};

// The commander screen's tabs (CommanderMenu.toggleSquadTab, toggleMapTab,
// toggleCommanderToolsTab: 0x007a9de0, 0x007a9e50, 0x007a9eb0 flip the
// menu's +0x1c0, +0x1c3 and +0x1c4, the HUD variables
// CommanderSquadTabShowFull, CommanderMapTabShowFull and
// CommanderToolsTabShowFull; closing the squad tab also clears +0x1c1,
// ShowSquadMemberList). The menu's constructor (0x007ac090, vtable
// 0x009b5860, the HUD's +0xe4) opens the squad, map and tools tabs
// (+0x1c0 / +0x1c3 / +0x1c4 = 1) and closes the members, the leader's
// stats and the order picture (+0x1c1 / +0x1c2 / +0x1c5 = 0).
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

// The commander rose's rows (main.cpp binds them to the console):
//   - Commander.zoomCommander (0x0077168e -> HUD slot 0x39c, 0x007d7be0):
//     the map's zoom index steps (0x0077e850(-1, 1): up while below the
//     last, 2 the overhead view, then back to 0); the map's cursor becomes
//     the world's point (slot 0x3a0, 0x007d8ec0) and CommanderCam goes
//     there (slot 0x398).
int commanderZoomStep(int zoom);
// Wheel zoom stops at the whole-map and live overhead endpoints.
int commanderWheelZoom(int zoom, int steps);
//   - Commander.detailedView (0x0077161d -> slot 0x394, 0x007d8db0): the
//     overhead camera's point kept (+0x2f8..+0x300): the view's own, else
//     the player's; the map at zoom 2 and the screen held open.
std::pair<float, float> detailedViewPoint(const std::pair<float, float>* view, const game::GameWorld::PlayerInfo* me);

// The commander's overhead camera in a local round (the server's
// OverheadCamera object, MultiPlayerOverheadCamera: a local game is its
// own server, so 0x005f8410 / 0x005f8170 / 0x005f8510 run for its
// commander as for a host's clients; HostedGame::moveOverheadCamera).
// - The point (0x005f8170): kept 80 m inside the terrain (its half
//   extents from the heightmap cluster's slot 0x2c, less 80, about the
//   centre); the height is the mean ground's plus the template's, which the
//   overhead view takes itself.
// - The pan (0x005f8510, each frame for the commander on the overhead
//   view): input 0 (PIYaw, A / D) moves it east and west, input 3
//   (PIThrottle, W / S) north and south: each axis's speed (+0x274 /
//   +0x27c) grows by the input x the acceleration (*0x00a7303c: the
//   "overheadCamera-acceleration" tweak, 250) x the frame's time, kept
//   within the template's speed (+0x2a0, 50) x the camera's +0x270 (1
//   from its constructor 0x005f8870, written nowhere else; 0x005f8120
//   reads the product too),
//   and without input falls back to 0 at the same rate; then the point
//   moves by speed x time (0x005f8170 with the speeds).
struct OverheadCamera {
	float x = 0, z = 0;
	float speedX = 0, speedZ = 0;
	float acceleration = 250, speed = 50;
	// The terrain (origin and size, metres) the point keeps 80 m inside.
	float originX = 0, originZ = 0, width = 0, depth = 0;
	void moveTo(float px, float pz); // 0x005f8170 without a time: the speeds cleared
	void pan(float yawInput, float throttleInput, float seconds);
};
//   - Commander.spottedMouseOver <n> (0x00771b67 -> slot 0x450,
//     0x00751f80): Spotted<i>MouseOver = (i == n) for i 1..11.
void spottedMouseOver(hud::HudManager& hud, int n);
// The squad leader rose's rows:
//   - SquadLeader.sendRequestForOrder (0x00770a7e -> slot 0x314,
//     0x007c7cd0, the rose's needorders): "req_new_order" to the commander
//     (channel 2) with the spam check, then the Request event of kind 1
//     (0x007c7d50; a joined game's IssueSquadOrder).
void sendRequestForOrder(const std::function<void(int channel, const std::string& voice)>& say,
	const std::function<void(int kind)>& request);
//   - SquadLeader.issueSquadRequest (0x00770b5f -> slot 0x3b0,
//     0x007d6820): the armed order (+0x284) placed at the rose's point
//     (+0x28c) through sendOrder's handler; nothing without an armed order
//     or a point. True when placed.
bool issueSquadRequest(bool armed, const std::function<bool(float& x, float& z)>& rosePoint,
	const std::function<void(float x, float z)>& place);

// The commander screen's squad list (list data 15, "CommanderSquadList";
// CommanderMenu.singleClick, 0x007ab380): a row per squad of the team
// with members, its id in cell 13 and its kind (0) in cell 14 (0x007a9c90,
// 0x007a9c00). A click on column 4 makes that squad the only one selected
// and shows its members (0x007a9f10: menu state 0x11, list data 14 and
// CurrentSquadName, CurrentSquadLeaderName); a click elsewhere selects it
// alone, or deselects it when it was the only one (slot 0x1c, 0x007ab2b0);
// with Ctrl (HUD +0x235: c_GILeftCtrl / c_GIRightCtrl) slot 0x20
// (0x007aa9f0) toggles it among the others, with Shift (+0x236) slot 0x30
// (0x007a9d30) adds every squad from the anchor (+0xcc, the last one
// clicked into the selection; nothing without one) to it. Column 3 mutes
// or unmutes the squad leader's voice (his player slot 0x300 and
// VoipPlayerMute 0x26 to the server).
// The commander's orders go to the selected squads (the order handler 0x007d6ab0
// sends one to each squad slot 0x34 reports selected; 0x007a9f60 is the
// radio messages', CommanderMenu.sendRadioMessage).
class CommanderMenu {
public:
	CommanderMenu(game::GameWorld& round, hud::HudManager& hud);
	// CommanderMenu.* console methods for the local player.
	void bind(Console& console);
	void update(int player);
	void click(int player, int row, int column);
	void deselect();
	const std::set<int>& selected() const { return m_selected; }
	// The modifiers held, and the leader's voice toggled (column 3).
	std::function<bool()> ctrlHeld, shiftHeld;
	std::function<void(int leader)> onMuteLeader;
	// The leaders' voice icons in column 3 (0x007aaf10, the panel's
	// textures of 0x007ac090): a leader with voice (player slot 0x2e4)
	// shows voip_icon_blocked when the commander mutes him (+0x298's list,
	// +0x124) or he is blocked (slot 0x2f0, +0x128), voip_icon_transmitting
	// while he talks (slot 0x2f4, +0x12c; each such frame counts +0xb0 and
	// lets the help message 0x26 through, from the second 0x27 too),
	// voip_icon_distorted otherwise (+0x130); without voice the cell is
	// empty. `voice` gives a player's: 0 none, 1 muted, 2 talking, 3 idle.
	std::function<int(int player)> voice;
	std::function<void(int tutorial)> onTutorial;
	// The tactical rose's all-squads row (0x007ac8f0 with the panel's +0x1e4:
	// set by 0x00754290 as the commander, or a player with a squad, opens
	// the rose with c_GITacticalComm, cleared each frame the key isn't
	// held): an extra row, selected, "SELECTED_ALL_SQUADS".
	bool allSquads = false;
	std::function<std::string(const std::string&)> localize;
	// The squad whose members show (+0xac; -1 none).
	void showSquad(int squad) { m_current = squad; }
	// CommanderMenu.sendRadioMessage (0x00771fe2 -> HUD slot 0x2c4 ->
	// 0x007a9f60): `say` for each selected squad (the spam check on the
	// first only), then no squad shows (+0xac) and the members' state 0x11
	// goes back to 0x10.
	void sendRadioMessage(const std::function<void(int squad, bool first)>& say);
	CommanderTabs tabs;
	// A joined game's players and requests (the server's, HudWorld.h).
	void setWorld(const host::HudWorld* world) { m_world = world; }

private:
	const host::HudWorld* m_world = nullptr;
	bool playerInfo(int player, game::GameWorld::PlayerInfo& out) const;
	std::vector<game::GameWorld::PlayerInfo> playerList() const;
	// The squad's order picture in the list (cell 2) and the panel.
	std::string orderPicture(int team, int squad) const;
	std::string kitPicture(bool alive, bool manDown, uint32_t kit) const;
	game::GameWorld& m_round;
	hud::HudManager& m_hud;
	std::set<int> m_selected;
	int m_current = -1; // +0xac: the squad whose members show
	int m_anchor = -1;  // +0xcc: the range's start
	int m_transmitting = 0; // +0xb0
};

// The squad leaders' requests and the commander's answers (BF2142.exe's
// HUD manager; the store is GameWorld's squad requests, the squad
// manager's slot 0x50 0x004c5720):
//   - SquadLeader.sendRequest <kind> <add> (0x007708d4's executor
//     0x007709f8 → HUD manager slot 0x3f8, 0x007d6d20): kind 0 artillery,
//     1 orbital strike, 2 UAV, 3 supplies (SquadLeader.con's rose:
//     "sendRequest 3 0" supplies, 2 the UAV, 1 the orbital strike, 0 the
//     artillery; the map menus "<kind> 1"). Only alive
//     ("HUD_HUD_SQUAD_ALIVE_TO_REQUEST" otherwise), at most
//     sv.radioMaxSpamFlagCount (DAT_00a3fcc0 +0x48, 6) requests until
//     sv.radioBlockedDurationTime (+0x4c, 30 s) has passed since the first
//     (+0x378 the count, +0x37c the time, reset by the HUD's update
//     0x00750xxx), and outside sp1 / gpm_sp / gpm_coop only with a
//     commander ("no_commander_available" said to the leader alone
//     otherwise). The request's point is the order record's (+0x28c): the
//     rose's aim (0x00753b40, up to 1000 m) or the map's point with add
//     (0x00780820). Stored (0x004c5720) it goes to the squad leader (PostRemote 2/0x1e:
//     "HUD_HUD_SQUAD_REQUEST_TO_COMMANDER" and his radio to the commander
//     on TCPlayer, 0x007c7690: "req_artillery",
//     "PLAYER_REQUEST_TM_requestforcestrike", "PLAYER_REQUEST_requestuav",
//     "PLAYER_REQUEST_requestsupplies") and to the commander (2/0x1d,
//     0x007c8f10): with the tool usable (0x007c7880) the line
//     "HUD_HUD_REQUEST_RECIEVED_FROM_SQUAD" (#REQUESTNAME# the tool's
//     HUD_TEXT_COMMANDER_*, 0x007c7970, #SQUADNAME#), else it waits 4 s
//     (0x007c8c00 → 0x007c8a60) and is turned down by itself: taken off
//     and "<tool>_auto_deny" (the asset standing) or "<tool>_unavailable"
//     (0x007c7a00) said to the squad's leader (0x007c7810: TCPlayer).
//   - The commander's answer (Commander.commRoseAccept on a selected
//     request tag, slot 0x390 0x007d8d30 → 0x007c7e20): deny says
//     "<tool>_deny" to the leader; accept uses the tool at the request's
//     point quietly (the tools' slots 0x32c / 0x348 / 0x35c / 0x358 with
//     1): done, "<tool>_accept" to the leader and "<tool>_deny" to every
//     other squad with a request of that kind (each taken off); not done,
//     "<tool>_unavailable" to the commander alone and the request stays.
// The tools are named artillery, orbitalstrike, uav and supply.
class SquadRequests {
public:
	explicit SquadRequests(game::GameWorld& round) : m_round(round) {}
	std::function<std::string(const std::string&)> localize;
	std::function<void(const std::string&)> onPersonal; // slot 0x170 (PersonalList)
	std::function<void(const std::string&)> onInfo;     // slot 0x15c (the chat's game line)
	int localPlayer = -1;  // whose HUD (the commander's lines)
	bool noCommanderNeeded = false; // sp1, gpm_sp, gpm_coop
	int maxRequests = 6;            // sv.radioMaxSpamFlagCount
	float blockedSeconds = 30;      // sv.radioBlockedDurationTime

	// slot 0x3f8 for `player` at (x, z); true when stored.
	bool send(int player, int kind, float x, float z);
	enum class Answer { None, Accepted, Denied, Unavailable };
	// 0x007c7e20 by the team's commander.
	Answer answer(int commander, int squad, int kind, bool accept);
	// The 4 s waits (0x007c8a60) and the count's reset.
	void tick(float seconds);
	// The request count's limit (+0x378 against sv.radioMaxSpamFlagCount,
	// +0x37c's sv.radioBlockedDurationTime): false when no more may go.
	bool count();
	// 2/0x1d for the commander's HUD: a squad's request stored (the line,
	// or the 4 s wait and the turn-down when the tool can't be used), and
	// the line itself ("HUD_HUD_REQUEST_RECIEVED_FROM_SQUAD").
	void received(int team, int squad, int kind);
	std::string receivedLine(int squad, int kind) const;
	static const char* toolName(int kind); // "artillery", "orbitalstrike", "uav", "supply"
	// A joined game's commander (the server's requests and players,
	// HudWorld.h): the 4 s turn-down takes the request off with the squad
	// manager's slot 0x24 (SetAcceptOrder to the server) and says its line
	// to the leader (0x007c7810: TCPlayer through the client's radio).
	struct Remote {
		const host::HudWorld* world = nullptr;
		std::function<void(int squad, int kind)> takeOff;
		std::function<void(int leader, const std::string& voice)> say;
	};
	void setRemote(Remote remote) { m_remote = std::move(remote); }

private:
	Remote m_remote;
	std::string text(const std::string& key) const { return localize ? localize(key) : key; }
	int leaderOf(int team, int squad) const;
	game::GameWorld& m_round;
	int m_sent = 0;      // +0x378
	float m_since = -1;  // seconds since the first (+0x37c), -1 none
	struct Pending {
		int team, squad, kind;
		float left;
	};
	std::vector<Pending> m_pending; // the commander's +0x354
};

// The squad and commander info bar (0x007a0f80, HudElementsPlayer.con's
// SquadInfo): the commander, outside the commander screen (HUD slot
// 0x234), sees "HUD_HUD_SQUADINFO_COMMANDER" with
// Ingame/SquadInfo/icon_commander.tga (ShowCommanderIcon +0x2ff); a
// squad's leader or member "HUD_HUD_SQUADINFO_SQUADLEADER" /
// "_SQUADMEMBER" with icon_squad<n>.tga (ShowSquadIcon +0x300); anyone
// else nothing (SquadInfoBarShow +0x2fe 0). SquadInfoText +0x304,
// SquadInfoIconPath +0x308. The squad order points bar
// (SquadOrderInfoBarShow +0x2fd, SquadInfoValue +0x30c: +0x310 over
// +0x318, from the field-unlock progress the server grants) stays off.
void updateSquadInfo(hud::HudManager& hud, game::GameWorld& round, int player, bool commanderScreen,
	const std::function<std::string(const std::string&)>& localize);
// The same for a player as the HUD's world has it (a joined game's: the
// server's player): whether there is one, commands, its squad and lead.
void updateSquadInfo(hud::HudManager& hud, bool known, bool commander, int squad, bool squadLeader, bool commanderScreen,
	const std::function<std::string(const std::string&)>& localize);

// The commander's own tools' refusals (said to him alone, game slot 0x60):
// arming (0x007d73a0 artillery, 0x007d74e0 the UAV, 0x007d7640 supplies)
// and using (0x007d88d0, 0x007d7d80 the orbital strike, 0x007d8370,
// 0x007d81d0; 0x007d8660 the scan): "<tool>_unavailable" without a
// standing asset (slot 0x3c8) with armour (0x3d8), "<tool>_in_use" while it
// reloads or runs ("supplies_in_use"; the scan's "satellite_unavailable" /
// "satellite_not_ready"); "" when it may be used. A use that goes ahead
// plays the HUD's "mapAction" sound (DAT_00b43878 slot 0x2c) and sends the
// commander event (0x007d7c20, 0x28 bytes).
// ability: GameWorld::CommanderAbility as an int (0 orbital strike, 1
// artillery, 2 UAV, 3 satellite, 4 supplies).
std::string commanderToolRefusal(int ability, bool available, bool ready);

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
// A scoreboard row's kit icon (the HUD manager's slot 0x64, 0x0074e940,
// taken by the row filler 0x007bb540 and drawn 15 pixels square): a player
// not alive (slot 0x74; or the HUD manager's slot 0x410 test) shows
// Ingame/Scoreboard/Icons/icon_Spectator.tga when a spectator (+0x404,
// slot 0x3bc), else icon_Death.tga; a living enemy of the local player
// (teams by slot 0xf8, `localTeam` 0 for none) icon_Faded.tga; a living
// teammate his kit's HUD icon (the kit, slot 0xb4, its HUD component 0xc4d7
// slot 0x58's texture 0: vehicleHud.vehicleIcon, as the kit buttons show
// it); "" without one. Textures cached by the HUD manager (+0x1ec, +0x138,
// +0x134).
std::string scoreboardIcon(const game::GameWorld::PlayerInfo& player, int localTeam, const world::ObjectTemplate* kit);

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

// The capture bar's variables for a soldier at `at`: the closest capturable
// control point whose radius holds him (the scripts' getOccupyingCP), and
// the flag's height as the bar value.
struct CaptureBar {
	bool enabled = false, neutralShow = false, takenShow = false;
	float value = 0;
};
CaptureBar captureBar(const std::vector<game::GameWorld::ControlPointInfo>& points, const physics::Vec3& at);

// The Titan HUD's "uplink" state, a per-frame state machine the original HUD manager keeps itself (BF2142.exe
// 0x0044a470, the variables bound in 0x00450780): while the capture bar's CPInterfaceEnabled is on,
// TimePreUplinkComplete rises 0.5 a second and ShowTimePreUplink stays on until it reaches 1 (then it holds
// at 1, ShowTimePreUplink drops and the lost timer arms at 1); off, the progress and the show flag reset, and
// while the armed timer is above 0 ShowTimePreUplinkLost is on as it falls 0.5 a second. CaptureComplete is
// TimePreCPIsTaken above 0.99. HudElementsTitans.con's nodes read these.
struct UplinkHud {
	float complete = 0;      // TimePreUplinkComplete (+0xb0)
	float lostTimer = 0;     // +0xb4
	bool show = false;       // ShowTimePreUplink (+0xad)
	bool showLost = false;   // ShowTimePreUplinkLost (+0xae)
	bool captureComplete = false; // CaptureComplete (+0xda)
	void update(float seconds, bool enabled, float takenValue)
	{
		captureComplete = takenValue > 0.99f;
		if (lostTimer > 0 && !enabled) {
			showLost = true;
			lostTimer -= seconds * 0.5f;
		} else {
			showLost = false;
		}
		if (!enabled) {
			complete = 0;
			show = false;
			return;
		}
		complete += seconds * 0.5f;
		if (complete >= 1.0f) {
			lostTimer = 1.0f;
			complete = 1.0f;
			show = false;
		} else {
			show = true;
		}
	}
};

// Titan mode's ticket bars span the two parts of a Titan's armour: hull at
// 0..60 and shield at 60..100. The game-mode script mirrors this value into
// tickets; use the native Titan armour as the HUD source so the bars remain
// tied to the live mode state even if a script callback is absent.
float titanHudHealth(const game::GameWorld::TitanInfo& titan);

// MiddleTicketsShow (BF2142.exe 0x00449930, field +0xa88 bound in 0x00451820): the Titan HUD's top block is shown
// while either team's ticket value is 100 or less (Titan mode's tickets are the Titans' armour, 0..100; Conquest's
// start in the hundreds). Without it HudElementsTitans.con's whole ticket, health and console block stays hidden.
inline bool middleTicketsShown(float friendlyTickets, float enemyTickets) { return friendlyTickets <= 100 || enemyTickets <= 100; }

} // namespace dice::hfe::menu
