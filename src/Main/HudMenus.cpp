#include "HudMenus.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameWorld.h"
#include "Hud/Hud.h"
#include "HudWorld.h"
#include "World/Object.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <vector>

namespace dice::hfe::menu {

namespace {

// The first of the 757 names at 0x00b3df00 (filled at start-up, 0x008f0000).
constexpr const char* kSquadNames[] = {"Unassigned", "Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Gamma",
	"Hotel", "India", "Juliet", "Kilo", "Lima", "Mike", "November", "Oscar", "Papa"};

constexpr int kSquadSize = 6;

// The squad list's cells (0x007a5f10): 0 the number's frame, 1 the number,
// 5 the name, 6 "members/6", 7 and 8 the lock in its frame, 9 and 10 the
// JOIN / LEAVE button, 12 the arrow; 13 and 14 the row's id and kind, not
// drawn. A row kind's actions by column: 0x007a81b0's tables 0x007a83c4.
constexpr int kCellId = 13, kCellKind = 14;
constexpr int kRowCommander = 0, kRowSquad = 2, kRowMember = 4;
constexpr int kColumnLock = 8, kColumnJoin = 10, kColumnExpand = 12;

// The menu's pictures (its constructor 0x007a7620: +0x14 .. +0x48).
constexpr const char* kFrame = "Ingame/Respawn/iconframe.tga";
constexpr const char* kLock = "Ingame/Respawn/icon_lock.tga";
constexpr const char* kButton = "Ingame/Respawn/button_longlight7.tga";
constexpr const char* kExpanded = "Ingame/Respawn/button_up.tga";
constexpr const char* kCollapsed = "Ingame/Respawn/button_down.tga";
constexpr const char* kCommanderIcon = "Ingame/Kits/Icons/kit_Commander.tga";

// The rows' colours (.data).
using Rgba = std::array<float, 4>;
constexpr Rgba kGrey {0.5f, 0.5f, 0.5f, 1};                          // 0x00a0bc60: text, an open lock
constexpr Rgba kBlack {0, 0, 0, 1};                                  // 0x00a0bb40 / 0x00a0bc70: a shut lock, the button's text
constexpr Rgba kOwn {0.141176477f, 0.501960814f, 0.258823544f, 1};   // 0x00a0bba0: the player's own squad, or command
constexpr Rgba kUnavailable {0.627451003f, 0.627451003f, 0.627451003f, 1}; // 0x00a0bca0: JOIN out of reach
constexpr Rgba kHeadFill {0.843137264f, 0.847058833f, 0.847058833f, 1};   // 0x00a0bc40: squad and commander rows
constexpr Rgba kMemberFill {0.650980413f, 0.650980413f, 0.650980413f, 1}; // 0x00a0bc30: odd member rows
constexpr Rgba kLocalFill {0.392156869f, 0.392156869f, 0.392156869f, 0.35f}; // 0x00a0bc50: the local player's row
constexpr Rgba kNoFill {0, 0, 0, 0};
// A member's state colour (0x007a65a0, via the theme table 0x008f2350):
// dead black (0x00b3bee0 ← 0x00a0bb40), his squad's (or the commander
// himself) green (0x00b3c070 ← 0x00a0beb0), the others blue (0x00b3c340 ←
// 0x00a0bec0); the leaders' squad icon (0x00b3c380 ← 0x00a0bb60); a
// talking voice yellow (0x00a0bc80).
constexpr Rgba kMateText {0.0313725509f, 0.466666669f, 0.00784313772f, 1};
constexpr Rgba kOtherText {0, 0.380392164f, 1, 1};
constexpr Rgba kLeaderIcon {0.400000006f, 1, 0, 1};
constexpr Rgba kTalking {1, 1, 0, 1};
constexpr const char* kBox = "Ingame/Respawn/iconframe_box.tga";
constexpr const char* kSpeaker = "Ingame/Respawn/icon_speaker.tga";
constexpr const char* kSpeakerMute = "Ingame/Respawn/icon_speakerMute.tga";

// The cells' settings (0x007e0e50 pictures, 0x007e0d60 / 0x007e0fc0 text;
// the text's alignment as the list draws it: the exe's 1 centre, 2 right).
hud::HudManager::ListCell pictureCell(float width, float height, float offset = 0, Rgba color = {-1, 0, 0, 0})
{
	return {offset, width, height, -1, color};
}
hud::HudManager::ListCell textCell(float offset, int alignment, Rgba color = {-1, 0, 0, 0})
{
	return {offset, 0, 0, alignment, color};
}
constexpr int kLeft = 0, kRight = 1, kCentre = 2;

// "§1" switches the list to its font 1 (squadListFont_8), "§0" back.
const std::string kFont1 = "\xa7"
						   "1",
				  kFont0 = "\xa7"
						   "0";

std::string upper(std::string s) // 0x0065b2c0
{
	for (char& c : s) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	return s;
}

} // namespace

const char* squadName(int squad)
{
	return squad >= 0 && squad < static_cast<int>(std::size(kSquadNames)) ? kSquadNames[squad] : "";
}

SquadMenu::SquadMenu(game::GameWorld& round, hud::HudManager& hud) : m_round(round), m_hud(hud)
{
	// The list's columns: the list data's own at 0 (0x007e0b20) and the
	// menu's (0x007a3e72, x from its +0xc0 .. +0xe0 set in 0x007a7620; a
	// fixed width or none, ending at the list's edge); two hidden ones
	// hold cells 13 and 14.
	m_hud.setListColumns(5, {{0, 0, 0}, {5, 0, 0}, {22, 0, 0}, {22, 0, 0}, {45, 166, 0}, {45, 69, 0}, {85, 30, 0},
								{116, 0, 0}, {116, 0, 0}, {133, 0, 0}, {133, 80, 0}, {216, 0, 0}, {216, 0, 0}});
	m_hud.setListColumns(8, {{6, 196, 0}});
}

bool SquadMenu::playerInfo(int player, game::GameWorld::PlayerInfo& out) const
{
	return m_remote.world ? m_remote.world->playerInfo(player, out) : m_round.playerInfo(player, out);
}

std::vector<game::GameWorld::PlayerInfo> SquadMenu::playerList() const
{
	return m_remote.world ? m_remote.world->playerList() : m_round.playerList();
}

int SquadMenu::localPlayer() const { return m_remote.world ? m_remote.world->localPlayer() : m_round.localPlayer(); }

void SquadMenu::bind(Console& console)
{
	onCommanderAction = [&console](bool resign) {
		std::string output;
		console.execute(resign ? "Commander.commanderResign" : "Commander.commanderApply", output, ConsoleAccess::Anyone);
	};
	onMutiny = [&console] {
		std::string output;
		console.execute("Commander.commanderMutiny", output, ConsoleAccess::Anyone);
	};
	const auto local = [this] { return localPlayer(); };
	bindMethod(console, "SquadMenu.setSquadCreateSelect", ConsoleAccess::Anyone,
		[this, local](bool select) { setCreateSelect(local(), select); });
	bindMethod(console, "SquadMenu.squadCreateLockToggle", ConsoleAccess::Anyone, [this] { toggleCreateLock(); });
	bindMethod(console, "SquadMenu.createSquad", ConsoleAccess::Anyone, [this, local] { createSquad(local()); });
	// The name box (an edit node) isn't typed into here: the name stays
	// the game's default (0x007a5300 focuses the box while it shows).
	bindMethod(console, "SquadMenu.setSquadNameSelect", ConsoleAccess::Anyone, [] {});
	bindMethod(console, "SquadMenu.setShowInviteList", ConsoleAccess::Anyone,
		[this, local](bool show) { setShowInviteList(local(), show); });
	bindMethod(console, "SquadMenu.applyInviteList", ConsoleAccess::Anyone, [this, local] { applyInviteList(local()); });
	bindMethod(console, "SquadMenu.singleClickNew", ConsoleAccess::Anyone, [this, local] {
		const auto& click = m_hud.lastListClick();
		if (click.data == 5) {
			clickSquadList(local(), click.row, click.column);
		}
	});
	bindMethod(console, "SquadMenu.singleClickInvite", ConsoleAccess::Anyone, [this, local] {
		const auto& click = m_hud.lastListClick();
		if (click.data == 8) {
			clickInviteList(local(), click.row);
		}
	});
	// The double clicks do nothing more than the single ones.
	bindMethod(console, "SquadMenu.doubleClickNew", ConsoleAccess::Anyone, [] {});
	bindMethod(console, "SquadMenu.doubleClickInvite", ConsoleAccess::Anyone, [] {});
}

int SquadMenu::freeSquad(int team) const
{
	std::set<int> taken;
	for (const auto& player : playerList()) {
		if (player.team == team && player.squad > 0) {
			taken.insert(player.squad);
		}
	}
	for (int squad = 1; squad <= 9; ++squad) {
		if (!taken.count(squad)) {
			return squad;
		}
	}
	return 0;
}

bool SquadMenu::setCreateSelect(int player, bool select)
{
	game::GameWorld::PlayerInfo me;
	if (!playerInfo(player, me)) {
		return false;
	}
	if ((me.squad == 0 && !me.commander) || !select) {
		int squad = freeSquad(me.team);
		if (me.squad > 0) {
			// A leader alone in his squad makes it again.
			int members = 0;
			for (const auto& other : playerList()) {
				members += other.team == me.team && other.squad == me.squad ? 1 : 0;
			}
			if (members == 1) {
				squad = me.squad;
			}
		}
		if (squad > 0) {
			m_createShow = select;
			return true;
		}
		m_createShow = false;
		message("HUD_HUD_SQUAD_MAX_REACHED");
		return false;
	}
	message(me.commander ? "HUD_HUD_SQUAD_COMMANDER_MUST_RESIGN" : "HUD_HUD_SQUAD_MUST_LEAVE_TO_CREATE");
	return false;
}

std::string SquadMenu::squadLabel(int team, int squad) const
{
	const auto it = m_names.find({team, squad});
	return it != m_names.end() ? it->second : squadName(squad);
}

bool SquadMenu::createSquad(int player)
{
	game::GameWorld::PlayerInfo me;
	if (!m_createShow || !playerInfo(player, me)) {
		return false;
	}
	const int squad = me.squad > 0 ? me.squad : freeSquad(me.team);
	bool made = false;
	if (m_remote.world) {
		// ChangeSquadName for the empty squad: the server makes it.
		made = squad > 0 && m_remote.create;
		if (made && me.squad != squad) {
			const std::string typed = typedName ? typedName() : std::string();
			m_remote.create(squad, typed.empty() ? squadName(squad) : typed);
			if (m_createLocked && m_remote.lock) {
				m_remote.lock(squad, true);
			}
		}
	} else {
		made = squad > 0 && (me.squad == squad || m_round.joinSquad(player, squad));
	}
	if (made) {
		const std::string typed = typedName ? typedName() : std::string();
		if (!typed.empty()) {
			m_names[{me.team, squad}] = typed;
		} else {
			m_names.erase({me.team, squad});
		}
		if (m_createLocked) {
			m_locked.insert({me.team, squad});
		} else {
			m_locked.erase({me.team, squad});
		}
	}
	setCreateSelect(player, false);
	return made;
}

bool SquadMenu::join(int player, int squad)
{
	game::GameWorld::PlayerInfo me;
	if (!playerInfo(player, me) || me.commander) {
		return false;
	}
	if (me.squad != 0) {
		message("HUD_HUD_SQUAD_MUST_LEAVE_TO_JOIN");
		return false;
	}
	if (locked(me.team, squad)) {
		return false;
	}
	if (m_remote.world) {
		if (m_remote.join) {
			m_remote.join(squad);
		}
		return true;
	}
	return m_round.joinSquad(player, squad);
}

void SquadMenu::leave(int player)
{
	game::GameWorld::PlayerInfo me;
	if (playerInfo(player, me) && me.squad > 0) {
		if (me.squadLeader) {
			m_locked.erase({me.team, me.squad});
		}
		if (m_remote.world) {
			if (m_remote.leave) {
				m_remote.leave(me.squad);
			}
			return;
		}
		m_round.joinSquad(player, 0);
	}
}

void SquadMenu::clickSquadList(int player, int row, int column)
{
	const int id = std::atoi(m_hud.listCell(5, row, kCellId).c_str());
	const int kind = std::atoi(m_hud.listCell(5, row, kCellKind).c_str());
	game::GameWorld::PlayerInfo me;
	if (kind == kRowCommander && playerInfo(player, me)) {
		if (column == kColumnExpand) {
			m_commanderExpanded = !m_commanderExpanded; // 0x007a83ab
		} else if (column == kColumnJoin) {
			// 0x007a8334: RESIGN as the commander, APPLY without one, else
			// MUTINY (the vote refuses one it can't start).
			if (me.commander || m_hud.listCell(5, row, kCellId) == "-1") {
				if (onCommanderAction) {
					onCommanderAction(me.commander);
				}
			} else if (onMutiny) {
				onMutiny();
			}
		}
		return;
	}
	if (kind != kRowSquad || !playerInfo(player, me)) {
		return; // selecting a member or a squad
	}
	if (column == kColumnExpand) {
		if (!m_expanded.erase(id)) {
			m_expanded.insert(id);
		}
	} else if (column == kColumnJoin) {
		if (id == me.squad) {
			leave(player);
		} else {
			join(player, id);
		}
	} else if (column == kColumnLock && id == me.squad && me.squadLeader) {
		if (!m_locked.erase({me.team, id})) {
			m_locked.insert({me.team, id});
		}
		if (m_remote.world && m_remote.lock) {
			m_remote.lock(id, locked(me.team, id));
		}
	}
}

void SquadMenu::setShowInviteList(int player, bool show)
{
	game::GameWorld::PlayerInfo me;
	if (!playerInfo(player, me) || !me.squadLeader) {
		return;
	}
	m_inviteShow = show;
	if (show) {
		m_selected.clear();
		m_unselected.clear();
		for (const auto& other : playerList()) {
			if (other.team == me.team && other.squad == me.squad && other.id != player) {
				m_selected.insert(other.id);
			}
		}
	}
}

void SquadMenu::clickInviteList(int player, int row)
{
	const int id = std::atoi(m_hud.listCell(8, row, 2).c_str());
	game::GameWorld::PlayerInfo me, other;
	if (!playerInfo(player, me) || !playerInfo(id, other) || other.commander || id == player) {
		return;
	}
	if (other.squad != me.squad) {
		if (m_selected.erase(id)) {
			return;
		}
		int members = 0;
		for (const auto& mate : playerList()) {
			members += mate.team == me.team && mate.squad == me.squad ? 1 : 0;
		}
		int invited = 0;
		for (const int selected : m_selected) {
			game::GameWorld::PlayerInfo info;
			invited += playerInfo(selected, info) && info.squad != me.squad ? 1 : 0;
		}
		if (invited < kSquadSize - members) {
			m_selected.insert(id);
		}
		return;
	}
	if (!m_unselected.erase(id)) {
		m_unselected.insert(id);
	}
}

void SquadMenu::applyInviteList(int player)
{
	game::GameWorld::PlayerInfo me;
	if (!playerInfo(player, me) || me.squad == 0) {
		return;
	}
	for (const int id : m_unselected) {
		game::GameWorld::PlayerInfo other;
		if (playerInfo(id, other) && other.team == me.team && other.squad == me.squad) {
			if (m_remote.world) {
				if (m_remote.kick) {
					m_remote.kick(id, me.squad); // LeaveSquad with the member (squad manager slot 0x38)
				}
			} else {
				m_round.joinSquad(id, 0);
			}
		}
	}
	for (const int id : m_selected) {
		game::GameWorld::PlayerInfo other;
		if (m_remote.world) {
			// The invitation (slot 0x30: Invite); the server's player answers it.
			if (!m_unselected.count(id) && playerInfo(id, other) && other.squad != me.squad && m_remote.invite) {
				m_remote.invite(id, me.squad);
			}
			continue;
		}
		if (!m_unselected.count(id) && playerInfo(id, other) && other.ai && other.squad == 0
			&& !locked(me.team, me.squad)) {
			m_round.joinSquad(id, me.squad);
		}
	}
	m_inviteShow = false;
}

void SquadMenu::update(int player)
{
	game::GameWorld::PlayerInfo me;
	if (!playerInfo(player, me)) {
		return;
	}
	const auto players = playerList();
	std::map<int, std::vector<const game::GameWorld::PlayerInfo*>> squads;
	int unassigned = 0;
	for (const auto& other : players) {
		if (other.team != me.team) {
			continue;
		}
		if (other.squad > 0) {
			squads[other.squad].push_back(&other);
		} else if (!other.commander) {
			++unassigned;
		}
	}
	if (!me.squadLeader) {
		m_inviteShow = false;
	}
	// The HUD manager's squad events (0x00756b60 → 0x007a5b80): joining a
	// squad (2/0x16) or taking its lead (2/0x18) opens it, leaving it
	// (2/0x15) closes it.
	if (me.squad != m_lastSquad) {
		if (m_lastSquad > 0) {
			m_expanded.erase(m_lastSquad);
		}
		if (me.squad > 0) {
			m_expanded.insert(me.squad);
		}
	} else if (me.squad > 0 && me.squadLeader && !m_lastLeader) {
		m_expanded.insert(me.squad);
	}
	m_lastSquad = me.squad;
	m_lastLeader = me.squadLeader;

	std::vector<std::vector<std::string>> rows;
	std::vector<std::vector<hud::HudManager::ListCell>> cells;
	std::vector<std::array<float, 4>> fills;
	const auto add = [&](std::vector<std::string> row, std::vector<hud::HudManager::ListCell> style, const Rgba& fill) {
		rows.push_back(std::move(row));
		cells.push_back(std::move(style));
		fills.push_back(fill);
	};
	// A member's row (0x007a65a0), in his state colour:
	//   0, 1  the vehicle he is in (HUD slot 0x60(player, 1): its
	//         vehicleHud.miniMapIcon, nothing on foot) in a box;
	//   2, 3  his kit (HUD slot 0x64, menu::scoreboardIcon) in a box;
	//   4     his name ("§1%s", 4 pixels left of the column);
	//   9     a leader's squad icon (mini_Squad_<n>_small, 16 pixels, the
	//         column's x plus 0xe0 - 0xd8 - 18);
	//   11, 12 his voice in a box, while he has voice.
	// The fill: the local player's row grey, else every odd row of the list.
	// Not drawn: a leader's squad-order icon in cells 7 and 8 (0x007a5c70).
	const auto member = [&](const game::GameWorld::PlayerInfo& who) {
		const bool mate = (me.squad > 0 && who.squad == me.squad) || (who.id == player && me.commander);
		const Rgba colour = !who.alive ? kBlack : mate ? kMateText : kOtherText;
		const auto templateOf = [&](uint32_t object) {
			const world::Object* o = object ? m_round.objects().find(object) : nullptr;
			return o ? o->objectTemplate() : nullptr;
		};
		std::vector<std::string> row(15);
		std::vector<hud::HudManager::ListCell> style(15);
		row[0] = kBox;
		style[0] = pictureCell(15, 15, 5);
		if (const world::ObjectTemplate* ride = who.vehicle != who.soldier ? templateOf(who.vehicle) : nullptr) {
			const auto icon = ride->settings().find("vehicleHud.miniMapIcon");
			if (icon != ride->settings().end()) {
				row[1] = icon->second;
				std::replace(row[1].begin(), row[1].end(), '\\', '/');
			}
		}
		style[1] = pictureCell(15, 15, 0, colour);
		row[2] = kBox;
		style[2] = pictureCell(15, 15);
		row[3] = scoreboardIcon(who, 0, templateOf(who.kit));
		style[3] = pictureCell(15, 15, 0, colour);
		row[4] = kFont1 + who.name;
		style[4] = textCell(-4, kLeft, colour);
		if (who.squadLeader && who.squad > 0) {
			row[9] = "Ingame/Player/Icons/Minimap/mini_Squad_" + std::to_string(who.squad) + "_small.tga";
			style[9] = pictureCell(16, 16, 216 - 133 - 18, kLeaderIcon);
		}
		row[11] = kBox;
		style[11] = pictureCell(15, 15);
		const int voice = voiceState ? voiceState(who.id) : -1;
		if (voice >= 0) {
			row[12] = voice == 2 ? kSpeakerMute : kSpeaker;
			style[12] = pictureCell(15, 15, 0, voice == 1 ? kTalking : colour);
		}
		row[kCellId] = std::to_string(who.id);
		row[kCellKind] = std::to_string(kRowMember);
		const size_t index = rows.size();
		add(std::move(row), std::move(style), who.id == player ? kLocalFill : index % 2 != 0 ? kMemberFill : kNoFill);
	};
	if (m_round.commanderEnabled()) {
		// The commander's row (0x007a4800), his own row under it while open
		// (0x007a73f0).
		const game::GameWorld::PlayerInfo* commander = nullptr;
		for (const auto& other : players) {
			if (other.team == me.team && other.commander) {
				commander = &other;
				break;
			}
		}
		const Rgba colour = me.commander ? kOwn : kGrey;
		std::vector<std::string> row(15);
		std::vector<hud::HudManager::ListCell> style(15);
		row[0] = kFrame;
		style[0] = pictureCell(15, 15, 5);
		row[1] = kCommanderIcon;
		style[1] = pictureCell(15, 15, 0, colour);
		row[5] = text("HUD_HUD_MENU_COMMANDER_HEADING");
		style[5] = textCell(-21, kLeft, colour);
		// APPLY without a commander, RESIGN as him, else MUTINY.
		const std::string button = !commander ? text("HUD_TEXT_MENU_SPAWN_SQUAD_APPLY")
			: me.commander                    ? text("HUD_TEXT_COMMANDER_RESIGN")
											  : text("HUD_TEXT_MENU_SPAWN_SQUAD_MUTINITY");
		row[9] = kButton;
		style[9] = pictureCell(80, 17);
		row[kColumnJoin] = button;
		style[kColumnJoin] = textCell(0, kCentre, kBlack);
		row[kColumnExpand] = !commander ? "" : m_commanderExpanded ? kExpanded : kCollapsed;
		style[kColumnExpand] = pictureCell(17, 17);
		row[kCellId] = commander ? std::to_string(commander->id) : "-1";
		row[kCellKind] = std::to_string(kRowCommander);
		add(std::move(row), std::move(style), kHeadFill);
		if (commander && m_commanderExpanded) {
			member(*commander);
		}
	}
	for (auto& [squad, members] : squads) {
		std::stable_sort(members.begin(), members.end(), [](const auto* a, const auto* b) { return a->squadLeader > b->squadLeader; });
		// The squad's row (0x007a5f10): green for the player's own.
		const Rgba colour = squad == me.squad ? kOwn : kGrey;
		// JOIN is out of reach from another squad or as the commander.
		const bool reachable = !me.commander && (me.squad <= 0 || me.squad == squad);
		std::vector<std::string> row(15);
		std::vector<hud::HudManager::ListCell> style(15);
		row[0] = kFrame;
		style[0] = pictureCell(15, 15, 5);
		row[1] = std::to_string(squad);
		style[1] = textCell(5, kLeft, colour);
		row[5] = kFont1 + upper(squadLabel(me.team, squad)) + kFont0;
		style[5] = textCell(-21, kLeft, colour);
		row[6] = kFont1 + std::to_string(members.size()) + "/" + std::to_string(kSquadSize) + kFont0;
		style[6] = textCell(0, kRight, colour);
		row[7] = kFrame;
		style[7] = pictureCell(15, 15);
		row[kColumnLock] = kLock;
		style[kColumnLock] = pictureCell(15, 15, 0, locked(me.team, squad) ? kBlack : kGrey);
		row[9] = kButton;
		style[9] = pictureCell(80, 17);
		row[kColumnJoin] =
			text(squad == me.squad && reachable ? "HUD_TEXT_MENU_SPAWN_SQUAD_LEAVE" : "HUD_TEXT_MENU_SPAWN_SQUAD_JOIN");
		style[kColumnJoin] = textCell(0, kCentre, reachable ? kBlack : kUnavailable);
		row[kColumnExpand] = expanded(squad) ? kExpanded : kCollapsed;
		style[kColumnExpand] = pictureCell(17, 17);
		row[kCellId] = std::to_string(squad);
		row[kCellKind] = std::to_string(kRowSquad);
		add(std::move(row), std::move(style), kHeadFill);
		if (expanded(squad)) {
			for (const auto* mate : members) {
				member(*mate);
			}
		}
	}
	m_hud.setList(5, std::move(rows));
	m_hud.setListCells(5, std::move(cells));
	m_hud.setListRowBackgrounds(5, std::move(fills));
	// The invite list (0x007a7ff0 with InviteListShow): a header row
	// (0x007a4680: HUD_SQUADTAB_SQUAD / HUD_SQUADTAB_KICK) over the
	// leader's members, then one (HUD_SQUADTAB_TEAM / HUD_SQUADTAB_INVITE)
	// over the team's players outside the squad (0x007a7450: not the
	// commander, not the leader himself), a row each (0x007a6e30: the name,
	// its icon cell, the player id in cell 2); the chosen ones highlighted
	// ("|c"). A header's id cell is "-1": a click on it does nothing.
	if (m_inviteShow) {
		std::vector<std::vector<std::string>> invite;
		const auto section = [&](const char* title, const char* action, bool members) {
			invite.push_back({text(title), text(action), "-1"});
			for (const auto& other : players) {
				if (other.team != me.team || other.id == player || other.commander || (other.squad == me.squad) != members) {
					continue;
				}
				const bool chosen = members ? !m_unselected.count(other.id) : m_selected.count(other.id) != 0;
				invite.push_back({(chosen ? "|c" : "") + other.name, "", std::to_string(other.id)});
			}
		};
		section("HUD_SQUADTAB_SQUAD", "HUD_SQUADTAB_KICK", true);
		section("HUD_SQUADTAB_TEAM", "HUD_SQUADTAB_INVITE", false);
		m_hud.setList(8, std::move(invite));
	}
	m_hud.set("SquadCreateShow", m_createShow ? 1 : 0);
	m_hud.set("SquadCreateLockedShow", m_createLocked ? 1 : 0);
	m_hud.setString("SquadCreateTagString", std::to_string(me.squad > 0 ? me.squad : freeSquad(me.team)));
	m_hud.set("NrOfSquads", static_cast<float>(squads.size()));
	m_hud.set("NrOfUnassigned", static_cast<float>(unassigned));
	m_hud.set("InviteListShow", m_inviteShow ? 1 : 0);
	m_hud.set("IsInSquad", me.squad > 0 ? 1 : 0);
	m_hud.set("IsSquadLeader", me.squadLeader ? 1 : 0);
}

void CommanderTabs::apply(hud::HudManager& hud) const
{
	hud.set("CommanderSquadTabShowFull", squad ? 1 : 0);
	hud.set("ShowSquadMemberList", squadMembers ? 1 : 0);
	hud.set("CommanderMapTabShowFull", map ? 1 : 0);
	hud.set("CommanderToolsTabShowFull", tools ? 1 : 0);
}

CommanderMenu::CommanderMenu(game::GameWorld& round, hud::HudManager& hud) : m_round(round), m_hud(hud)
{
	// Fitted to the 182-pixel list of HudElementsCommander.con.
	m_hud.setListColumns(15, {{4, 14, 2}, {20, 66, 0}, {106, 15, 2, 15}, {88, 15, 2, 13}, {124, 50, 2}});
	m_hud.setListColumns(14, {{4, 13, 2, 13}, {22, 190, 0}});
}

bool CommanderMenu::playerInfo(int player, game::GameWorld::PlayerInfo& out) const
{
	return m_world ? m_world->playerInfo(player, out) : m_round.playerInfo(player, out);
}

std::vector<game::GameWorld::PlayerInfo> CommanderMenu::playerList() const
{
	return m_world ? m_world->playerList() : m_round.playerList();
}

void CommanderMenu::bind(Console& console)
{
	const auto local = [this] { return m_world ? m_world->localPlayer() : m_round.localPlayer(); };
	bindMethod(console, "CommanderMenu.toggleSquadTab", ConsoleAccess::Anyone, [this] { tabs.toggleSquad(); });
	bindMethod(console, "CommanderMenu.toggleMapTab", ConsoleAccess::Anyone, [this] { tabs.map = !tabs.map; });
	bindMethod(console, "CommanderMenu.toggleCommanderToolsTab", ConsoleAccess::Anyone, [this] { tabs.tools = !tabs.tools; });
	bindMethod(console, "CommanderMenu.singleClick", ConsoleAccess::Anyone, [this, local] {
		const auto& click = m_hud.lastListClick();
		if (click.data == 15) {
			this->click(local(), click.row, click.column);
		}
	});
	bindMethod(console, "CommanderMenu.doubleClick", ConsoleAccess::Anyone, [] {});
	bindMethod(console, "CommanderMenu.rightClick", ConsoleAccess::Anyone, [] {});
	bindMethod(console, "CommanderMenu.deselect", ConsoleAccess::Anyone, [this] { deselect(); });
}

void CommanderMenu::sendRadioMessage(const std::function<void(int squad, bool first)>& say)
{
	bool first = true;
	for (const int squad : std::set<int>(m_selected)) {
		say(squad, first);
		first = false;
	}
	showSquad(-1);
	tabs.squadMembers = false;
}

int commanderWheelZoom(int zoom, int steps)
{
	return static_cast<int>(std::clamp(static_cast<long long>(zoom) + steps, 0LL, 2LL));
}

int commanderZoomStep(int zoom)
{
	return zoom < 2 ? zoom + 1 : 0;
}

std::pair<float, float> detailedViewPoint(const std::pair<float, float>* view, const game::GameWorld::PlayerInfo* me)
{
	if (view) {
		return *view;
	}
	return me ? std::make_pair(me->position.x, me->position.z) : std::make_pair(0.0f, 0.0f);
}

void OverheadCamera::moveTo(float px, float pz)
{
	x = px;
	z = pz;
	speedX = speedZ = 0;
	if (width > 160 && depth > 160) {
		x = std::clamp(x, originX + 80, originX + width - 80);
		z = std::clamp(z, originZ + 80, originZ + depth - 80);
	}
}

void OverheadCamera::pan(float yawInput, float throttleInput, float seconds)
{
	const auto axis = [this, seconds](float input, float& v) {
		input = std::clamp(input, -1.0f, 1.0f);
		if (input != 0) {
			v = std::clamp(v + input * acceleration * seconds, -speed, speed);
		} else if (v > 0) {
			v = std::max(v - seconds * acceleration, 0.0f);
		} else if (v < 0) {
			v = std::min(v + seconds * acceleration, 0.0f);
		}
	};
	axis(yawInput, speedX);
	axis(throttleInput, speedZ);
	if (speedX != 0 || speedZ != 0) {
		const float vx = speedX, vz = speedZ;
		moveTo(x + vx * seconds, z + vz * seconds);
		speedX = vx;
		speedZ = vz;
	}
}

void spottedMouseOver(hud::HudManager& hud, int n)
{
	for (int i = 1; i <= 11; ++i) {
		hud.set("Spotted" + std::to_string(i) + "MouseOver", i == n ? 1.0f : 0.0f);
	}
}

void sendRequestForOrder(const std::function<void(int channel, const std::string& voice)>& say,
	const std::function<void(int kind)>& request)
{
	say(2, "req_new_order");
	if (request) {
		request(1);
	}
}

bool issueSquadRequest(bool armed, const std::function<bool(float& x, float& z)>& rosePoint,
	const std::function<void(float x, float z)>& place)
{
	float x = 0, z = 0;
	if (!armed || !place || !rosePoint || !rosePoint(x, z)) {
		return false;
	}
	place(x, z);
	return true;
}

void CommanderMenu::deselect()
{
	// 0x007a9db0: from the members' state (0x11) back to 0x10.
	m_current = -1;
	tabs.squadMembers = false;
}

void CommanderMenu::click(int player, int row, int column)
{
	game::GameWorld::PlayerInfo me;
	if (!playerInfo(player, me) || !me.commander || m_hud.listCell(15, row, kCellKind) != "0") {
		return;
	}
	const int squad = std::atoi(m_hud.listCell(15, row, kCellId).c_str());
	if (column == 3) {
		// The leader's voice (player slot 0x300; VoipPlayerMute to the server).
		if (onMuteLeader) {
			for (const auto& other : playerList()) {
				if (other.team == me.team && other.squad == squad && other.squadLeader) {
					onMuteLeader(other.id);
				}
			}
		}
		return;
	}
	if (column == 4) {
		m_selected = {squad};
		m_current = squad;
		tabs.squadMembers = true;
	} else if (ctrlHeld && ctrlHeld()) {
		// Slot 0x20 (0x007aa9f0): this one toggled, the others kept.
		if (!m_selected.erase(squad)) {
			m_selected.insert(squad);
		}
	} else if (shiftHeld && shiftHeld()) {
		// Slot 0x30 (0x007a9d30): every squad from the anchor to this one.
		if (m_anchor < 0) {
			return;
		}
		for (int s = std::min(m_anchor, squad); s <= std::max(m_anchor, squad); ++s) {
			m_selected.insert(s);
		}
	} else {
		// Slot 0x1c (0x007ab2b0): the others cleared, this one toggled.
		const bool was = m_selected.count(squad) && m_selected.size() == 1;
		m_selected.clear();
		if (!was) {
			m_selected.insert(squad);
		}
	}
	m_anchor = m_selected.count(squad) ? squad : -1;
}

void CommanderMenu::update(int player)
{
	game::GameWorld::PlayerInfo me;
	if (!playerInfo(player, me)) {
		return;
	}
	std::map<int, std::vector<game::GameWorld::PlayerInfo>> squads;
	for (const auto& other : playerList()) {
		if (other.team == me.team && other.squad > 0) {
			squads[other.squad].push_back(other);
		}
	}
	for (auto it = m_selected.begin(); it != m_selected.end();) {
		it = squads.count(*it) ? std::next(it) : m_selected.erase(it);
	}
	if (m_current >= 0 && !squads.count(m_current)) {
		deselect();
	}
	std::vector<std::vector<std::string>> rows;
	int selectedRow = -1;
	for (const auto& [squad, members] : squads) {
		std::vector<std::string> row(15);
		row[0] = std::to_string(squad);
		row[1] = (m_selected.count(squad) ? "|c" : "") + std::string(squadName(squad));
		row[4] = std::to_string(members.size()) + "/" + std::to_string(kSquadSize);
		row[2] = orderPicture(me.team, squad);
		// The leader's voice (0x007aaf10).
		if (voice) {
			for (const auto& member : members) {
				if (!member.squadLeader) {
					continue;
				}
				switch (voice(member.id)) {
				case 1: row[3] = "Ingame/Commander/voip/voip_icon_blocked.tga"; break;
				case 2:
					row[3] = "Ingame/Commander/voip/voip_icon_transmitting.tga";
					if (onTutorial) {
						onTutorial(0x26);
						if (++m_transmitting > 1) {
							onTutorial(0x27);
						}
					}
					break;
				case 3: row[3] = "Ingame/Commander/voip/voip_icon_distorted.tga"; break;
				default: break;
				}
			}
		}
		row[kCellId] = std::to_string(squad);
		row[kCellKind] = "0";
		if (squad == m_current) {
			selectedRow = static_cast<int>(rows.size());
		}
		rows.push_back(std::move(row));
	}
	if (allSquads) {
		// 0x007ac8f0's row 11 (0xb) with the selected squad's background
		// (+0x1bc): " " + SELECTED_ALL_SQUADS.
		std::vector<std::string> row(15);
		row[1] = "|c " + (localize ? localize("SELECTED_ALL_SQUADS") : std::string("SELECTED_ALL_SQUADS"));
		row[kCellKind] = "1";
		selectedRow = static_cast<int>(rows.size());
		rows.push_back(std::move(row));
	}
	m_hud.setList(15, std::move(rows));
	m_hud.setListSelection(15, selectedRow);
	std::vector<std::vector<std::string>> memberRows;
	std::string leader, leaderKit;
	if (const auto it = squads.find(m_current); it != squads.end()) {
		for (const auto& member : it->second) {
			if (member.squadLeader) {
				leader = member.name;
				leaderKit = kitPicture(member.alive, member.manDown, member.kit);
			} else {
				// The members' rows (0x007ac8f0): the kit's icon (0x007aa0f0) and the name.
				memberRows.push_back({kitPicture(member.alive, member.manDown, member.kit), member.name});
			}
		}
	}
	m_hud.setList(14, std::move(memberRows));
	m_hud.setString("CurrentSquadName", m_current >= 0 ? squadName(m_current) : "");
	m_hud.setString("CurrentSquadLeaderName", leader);
	m_hud.set("ShowSquadLeaderStats", leader.empty() ? 0 : 1);
	// The squad shown (0x007ab790 with its row the current one): its
	// order's picture (+0x1c5 ShowSquadOrderPicture) and its leader's kit
	// (SquadLeaderKitPicturePathString, 0x007aa0f0).
	const std::string order = m_current >= 0 ? orderPicture(me.team, m_current) : std::string();
	m_hud.setString("SquadOrderPicturePathString", order);
	m_hud.set("ShowSquadOrderPicture", order.empty() ? 0 : 1);
	m_hud.setString("SquadLeaderKitPicturePathString", leaderKit);
	tabs.apply(m_hud);
}

std::string CommanderMenu::orderPicture(int team, int squad) const
{
	// 0x007ab790: the squad's requests first (orderNew, +0x1a0: any of its
	// four kinds, the squad's slot 0x18), else the commander's order by its
	// kind (+0x174 + 4 kind: none for 0..3 and 8.., Move, Attack, Defend,
	// Repair for 4..7, 0x007a8770's textures).
	for (int kind = 0; kind < game::GameWorld::kRequestKinds; ++kind) {
		if (m_world) {
			for (const auto& request : m_world->squadRequests(team)) {
				if (request.squad == squad) {
					return "Ingame/Commander/Orders/orderNew.tga";
				}
			}
		} else if (m_round.squadRequest(team, squad, kind)) {
			return "Ingame/Commander/Orders/orderNew.tga";
		}
	}
	const game::GameWorld::SquadOrder* order = m_round.squadOrder(team, squad);
	game::GameWorld::SquadOrder remote;
	if (m_world) {
		order = m_world->squadOrder(team, squad, true, remote) ? &remote : nullptr;
	}
	static const char* const kPictures[] = {"orderMove", "orderAttack", "orderDefend", "orderRepair"};
	const int kind = order ? static_cast<int>(order->type) : -1;
	return kind >= 4 && kind <= 7 ? std::string("Ingame/Commander/Orders/") + kPictures[kind - 4] + ".tga" : std::string();
}

std::string CommanderMenu::kitPicture(bool alive, bool manDown, uint32_t kitObject) const
{
	// 0x007aa0f0 (0x007aa000's kit type): alive and not down, the kit's
	// icon (1 assault, 2 support, 4 engineer, 7 recon), else
	// icon_kit_dead.
	std::string type;
	if (alive && !manDown) {
		if (const world::Object* kit = kitObject ? m_round.objects().find(kitObject) : nullptr; kit && kit->objectTemplate()) {
			const auto it = kit->objectTemplate()->settings().find("kitType");
			if (it != kit->objectTemplate()->settings().end()) {
				for (const char c : it->second) {
					type += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
			}
		}
	}
	const char* icon = type.find("assault") != std::string::npos ? "assault"
		: type.find("support") != std::string::npos               ? "support"
		: type.find("engineer") != std::string::npos              ? "engineer"
		: type.find("recon") != std::string::npos || type.find("sniper") != std::string::npos ? "recon"
																									: "dead";
	return std::string("Ingame/Commander/kits/icon_kit_") + icon + ".tga";
}

std::string scoreboardIcon(const game::GameWorld::PlayerInfo& player, int localTeam, const world::ObjectTemplate* kit)
{
	if (!player.alive) {
		return player.spectator ? "Ingame/Scoreboard/Icons/icon_Spectator.tga" : "Ingame/Scoreboard/Icons/icon_Death.tga";
	}
	if (localTeam != 0 && player.team != localTeam) {
		return "Ingame/Scoreboard/Icons/icon_Faded.tga";
	}
	if (!kit) {
		return {};
	}
	const auto icon = kit->settings().find("vehicleHud.vehicleIcon");
	std::string path = icon != kit->settings().end() ? icon->second : std::string();
	std::replace(path.begin(), path.end(), '\\', '/');
	return path;
}

ScoreboardStyle scoreboardStyle(const ScoreboardRow& row, int rowIndex)
{
	ScoreboardStyle style {{0, 0, 0, 1}, {0, 0, 0, 0}};
	if (row.alive && !row.hidden) {
		if ((row.sameSquad && row.friendlyList) || (row.local && row.localCommands)) {
			style.text = {0.0313726f, 0.466667f, 0.00784314f, 1}; // 0x00a0beb0
		} else if (row.friendlyList) {
			style.text = {0, 0.380392f, 1, 1}; // 0x00a0bec0
		} else {
			style.text = {0.992157f, 0, 0.00392157f, 1}; // 0x00a0bd50
		}
	}
	if (row.local) {
		style.background = {0.392157f, 0.392157f, 0.392157f, 0.35f}; // 0x00a0bc50
	} else if (rowIndex % 2 != 0) {
		style.background = {0.65098f, 0.65098f, 0.65098f, 0.8f}; // 0x00a0bc30
	}
	return style;
}

std::vector<ScoreboardLine> scoreboardBySquad(const std::vector<ScoreboardMember>& members)
{
	std::vector<ScoreboardLine> lines;
	for (size_t i = 0; i < members.size(); ++i) {
		if (members[i].commander) {
			if (lines.empty()) {
				lines.push_back({-1, -1, 0, 0, 0, 0, 0, false});
			}
			lines.push_back({members[i].index});
		}
	}
	const auto group = [&](int squad) {
		ScoreboardLine header {-1, squad, 0, 0, 0, 0, 0, true};
		std::vector<ScoreboardLine> rows;
		for (const ScoreboardMember& m : members) {
			if (!m.commander && m.squad == squad) {
				header.score += m.score;
				header.teamwork += m.teamwork;
				header.kills += m.kills;
				header.deaths += m.deaths;
				header.ping += m.ping;
				rows.push_back({m.index});
			}
		}
		if (rows.empty()) {
			return;
		}
		header.ping /= static_cast<int>(rows.size());
		lines.push_back(header);
		lines.insert(lines.end(), rows.begin(), rows.end());
	};
	for (int squad = 1; squad <= 9; ++squad) {
		group(squad);
	}
	group(0);
	return lines;
}


// --- Squad requests (0x007d6d20, 0x007c8f10, 0x007c7e20, 0x007c8a60) -------

const char* SquadRequests::toolName(int kind)
{
	switch (kind) {
	case 1: return "orbitalstrike";
	case 2: return "uav";
	case 3: return "supply";
	default: return "artillery";
	}
}

int SquadRequests::leaderOf(int team, int squad) const
{
	for (const auto& p : m_remote.world ? m_remote.world->playerList() : m_round.playerList()) {
		if (p.team == team && p.squad == squad && p.squadLeader) {
			return p.id;
		}
	}
	return -1;
}

bool SquadRequests::send(int player, int kind, float x, float z)
{
	game::GameWorld::PlayerInfo me;
	if (!m_round.playerInfo(player, me)) {
		return false;
	}
	if (!me.alive) {
		if (onPersonal) {
			onPersonal(text("HUD_HUD_SQUAD_ALIVE_TO_REQUEST"));
		}
		return false;
	}
	if (!count()) {
		return false;
	}
	const int commander = m_round.commander(me.team);
	if (!noCommanderNeeded && commander < 0) {
		m_round.hearRadio(player, 2, "no_commander_available");
		return false;
	}
	if (kind < 0 || kind >= game::GameWorld::kRequestKinds || !me.squadLeader
		|| !m_round.storeSquadRequest(me.team, me.squad, kind, {x, m_round.terrainHeight(x, z), z})) {
		return false;
	}
	// 2/0x1e to the leader: his line and his radio to the commander
	// (0x007c7690, TCPlayer 0xe with the commander as the value).
	static const char* const kVoices[] = {"req_artillery", "PLAYER_REQUEST_TM_requestforcestrike", "PLAYER_REQUEST_requestuav",
		"PLAYER_REQUEST_requestsupplies"};
	if (player == localPlayer && onPersonal) {
		onPersonal(text("HUD_HUD_SQUAD_REQUEST_TO_COMMANDER"));
	}
	if (commander >= 0) {
		m_round.radioMessage(player, 14, kVoices[kind], commander);
	}
	// 2/0x1d to the commander (0x007c8f10).
	if (commander >= 0 && commander == localPlayer) {
		received(me.team, me.squad, kind);
	}
	return true;
}

bool SquadRequests::count()
{
	if (m_sent >= maxRequests) {
		return false;
	}
	if (m_since < 0) {
		m_since = 0;
	}
	++m_sent;
	return true;
}

std::string SquadRequests::receivedLine(int squad, int kind) const
{
	static const char* const kNames[] = {"HUD_TEXT_COMMANDER_ARTILLERY", "HUD_TEXT_COMMANDER_ORBITALSTRIKE",
		"HUD_TEXT_COMMANDER_UAV", "HUD_TEXT_COMMANDER_SUPPLIES"};
	std::string line = text("HUD_HUD_REQUEST_RECIEVED_FROM_SQUAD");
	const auto put = [&line](const std::string& from, const std::string& to) {
		for (size_t at = line.find(from); at != std::string::npos; at = line.find(from, at + to.size())) {
			line.replace(at, from.size(), to);
		}
	};
	put("#REQUESTNAME#", text(kNames[kind >= 0 && kind < 4 ? kind : 0]));
	put("#SQUADNAME#", squadName(squad));
	return line;
}

void SquadRequests::received(int team, int squad, int kind)
{
	// 0x007c8f10: with the tool usable (0x007c7880) the line, else the 4 s
	// wait (0x007c8c00).
	const auto state = m_round.abilityState(team, game::GameWorld::requestAbility(kind));
	if (state.available && state.ready) {
		if (onInfo) {
			onInfo(receivedLine(squad, kind));
		}
	} else {
		m_pending.push_back({team, squad, kind, 4.0f}); // 0x40800000
	}
}

SquadRequests::Answer SquadRequests::answer(int commander, int squad, int kind, bool accept)
{
	game::GameWorld::PlayerInfo me;
	if (!m_round.playerInfo(commander, me) || !me.commander) {
		return Answer::None;
	}
	const game::GameWorld::SquadRequest* request = m_round.squadRequest(me.team, squad, kind);
	if (!request) {
		return Answer::None;
	}
	const std::string tool = toolName(kind);
	if (!accept) {
		m_round.removeSquadRequest(me.team, squad, kind);
		const int leader = leaderOf(me.team, squad);
		if (leader >= 0) {
			m_round.radioMessage(commander, 14, tool + "_deny", leader);
		}
		return Answer::Denied;
	}
	const physics::Vec3 at = request->position;
	if (!m_round.commanderAction(me.team, game::GameWorld::requestAbility(kind), at.x, at.z)) {
		m_round.hearRadio(commander, 2, tool + "_unavailable");
		return Answer::Unavailable;
	}
	for (int other = 1; other <= 9; ++other) {
		if (!m_round.squadRequest(me.team, other, kind)) {
			continue;
		}
		m_round.removeSquadRequest(me.team, other, kind);
		const int leader = leaderOf(me.team, other);
		if (leader >= 0) {
			m_round.radioMessage(commander, 14, tool + (other == squad ? "_accept" : "_deny"), leader);
		}
	}
	return Answer::Accepted;
}

void SquadRequests::tick(float seconds)
{
	// The count's reset (the HUD's update: blockedSeconds after the first).
	if (m_since >= 0) {
		m_since += seconds;
		if (m_since > blockedSeconds) {
			m_since = -1;
			m_sent = 0;
		}
	}
	for (auto it = m_pending.begin(); it != m_pending.end();) {
		it->left -= seconds;
		if (it->left > 0) {
			++it;
			continue;
		}
		// 0x007c8a60: taken off, and 0x007c7a00's line to the leader.
		const Pending p = *it;
		it = m_pending.erase(it);
		if (m_remote.world) {
			const auto standing = m_remote.world->squadRequests(p.team);
			if (std::none_of(standing.begin(), standing.end(),
					[&](const game::GameWorld::SquadRequest& r) { return r.squad == p.squad && r.kind == p.kind; })) {
				continue;
			}
			if (m_remote.takeOff) {
				m_remote.takeOff(p.squad, p.kind);
			}
		} else {
			if (!m_round.squadRequest(p.team, p.squad, p.kind)) {
				continue;
			}
			m_round.removeSquadRequest(p.team, p.squad, p.kind);
		}
		const auto state = m_round.abilityState(p.team, game::GameWorld::requestAbility(p.kind));
		std::string voice;
		switch (p.kind) {
		case 1: voice = state.available ? "orbitalstrike_auto_deny" : "orbitalstrike_unavailable"; break;
		case 2: voice = state.available ? "uav_auto_deny" : "uav_unavailable"; break;
		case 3: voice = "supplies_auto_deny"; break;
		default: voice = state.available ? "artillery_auto_deny" : "artillery_unavailable"; break;
		}
		const int leader = leaderOf(p.team, p.squad);
		if (leader >= 0 && localPlayer >= 0) {
			if (m_remote.world) {
				if (m_remote.say) {
					m_remote.say(leader, voice);
				}
			} else {
				m_round.radioMessage(localPlayer, 14, voice, leader);
			}
		}
	}
}

std::string commanderToolRefusal(int ability, bool available, bool ready)
{
	switch (ability) {
	case 0: return !available ? "orbitalstrike_unavailable" : !ready ? "orbitalstrike_in_use" : "";
	case 1: return !available ? "artillery_unavailable" : !ready ? "artillery_in_use" : "";
	case 2: return !available ? "uav_unavailable" : !ready ? "uav_in_use" : "";
	case 3: return !available ? "satellite_unavailable" : !ready ? "satellite_not_ready" : "";
	default: return !ready ? "supplies_in_use" : "";
	}
}


void updateSquadInfo(hud::HudManager& hud, game::GameWorld& round, int player, bool commanderScreen,
	const std::function<std::string(const std::string&)>& localize)
{
	game::GameWorld::PlayerInfo me;
	const bool known = round.playerInfo(player, me);
	updateSquadInfo(hud, known, me.commander, me.squad, me.squadLeader, commanderScreen, localize);
}

void updateSquadInfo(hud::HudManager& hud, bool known, bool commander, int squad, bool squadLeader, bool commanderScreen,
	const std::function<std::string(const std::string&)>& localize)
{
	struct {
		bool commander;
		int squad;
		bool squadLeader;
	} me {commander, squad, squadLeader};
	const auto text = [&localize](const char* id) { return localize ? localize(id) : std::string(id); };
	bool bar = false, commanderIcon = false, squadIcon = false;
	std::string line, icon;
	if (known && !commanderScreen && me.commander) {
		bar = commanderIcon = true;
		line = text("HUD_HUD_SQUADINFO_COMMANDER");
		icon = "Ingame/SquadInfo/icon_commander.tga";
	} else if (known && !commanderScreen && me.squad > 0) {
		bar = squadIcon = true;
		line = text(me.squadLeader ? "HUD_HUD_SQUADINFO_SQUADLEADER" : "HUD_HUD_SQUADINFO_SQUADMEMBER");
		icon = "Ingame/SquadInfo/icon_squad" + std::to_string(me.squad) + ".tga";
	}
	hud.set("SquadInfoBarShow", bar ? 1 : 0);
	hud.set("ShowCommanderIcon", commanderIcon ? 1 : 0);
	hud.set("ShowSquadIcon", squadIcon ? 1 : 0);
	hud.set("SquadOrderInfoBarShow", 0);
	hud.setString("SquadInfoText", line);
	hud.setString("SquadInfoIconPath", icon);
}

CaptureBar captureBar(const std::vector<game::GameWorld::ControlPointInfo>& points, const physics::Vec3& at)
{
	const game::GameWorld::ControlPointInfo* best = nullptr;
	float bestDistance = 0;
	for (const auto& point : points) {
		const float dx = point.position.x - at.x, dz = point.position.z - at.z;
		const float distance = std::sqrt(dx * dx + dz * dz);
		if (point.capturable && distance <= point.radius && (!best || distance < bestDistance)) {
			best = &point;
			bestDistance = distance;
		}
	}
	CaptureBar bar;
	if (!best) {
		return bar;
	}
	bar.enabled = true;
	bar.value = std::clamp(best->flagHeight, 0.0f, 1.0f);
	// A flag moving up is being taken, down is being neutralized; at rest
	// part-way, an owned point is going down and a neutral one up.
	const bool down = best->flagRate != 0 ? best->flagRate < 0 : best->team != 0;
	const bool moving = best->flagRate != 0 || (bar.value > 0 && bar.value < 1);
	bar.neutralShow = moving && down;
	bar.takenShow = moving && !down;
	return bar;
}

float titanHudHealth(const game::GameWorld::TitanInfo& titan)
{
	if (titan.maxHitPoints <= 0) {
		return 0;
	}
	// gpm_ti.py sets tickets to ceil(100 * remaining / maximum); keep that
	// integer convention, including zero for a wreck, for HudElementsTitans.
	return std::ceil(std::clamp(100.0f * titan.hitPoints / titan.maxHitPoints, 0.0f, 100.0f));
}

} // namespace dice::hfe::menu
