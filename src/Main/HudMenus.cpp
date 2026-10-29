#include "HudMenus.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameWorld.h"
#include "Hud/Hud.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <vector>

namespace dice::hfe::menu {

namespace {

// The first of the 757 names at 0x00b3df00 (filled at start-up, 0x008f0000).
constexpr const char* kSquadNames[] = {"Unassigned", "Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Gamma",
	"Hotel", "India", "Juliet", "Kilo", "Lima", "Mike", "November", "Oscar", "Papa"};

constexpr int kSquadSize = 6;

// The squad list's cells (0x007a5f10): 1 the id, 5 the name, 6 "members/6",
// 10 join or leave; 13 and 14 the row's id and kind, not drawn.
constexpr int kCellId = 13, kCellKind = 14;
constexpr int kRowSquad = 2, kRowMember = 4;
constexpr int kColumnJoin = 10, kColumnLock = 8;

} // namespace

const char* squadName(int squad)
{
	return squad >= 0 && squad < static_cast<int>(std::size(kSquadNames)) ? kSquadNames[squad] : "";
}

SquadMenu::SquadMenu(game::GameWorld& round, hud::HudManager& hud) : m_round(round), m_hud(hud)
{
	// The list's columns (0x007a4100 sets them from the menu's widths; the
	// positions here are fitted to the 246-pixel list of
	// HudElementsSquadsNew.con).
	std::vector<hud::HudManager::ListColumn> columns(11, {-1, 0, 0});
	columns[1] = {6, 14, 2};
	columns[5] = {24, 116, 0};
	columns[6] = {140, 34, 2};
	columns[kColumnLock] = {174, 12, 2};
	columns[kColumnJoin] = {186, 60, 2};
	m_hud.setListColumns(5, columns);
	m_hud.setListColumns(8, {{6, 196, 0}});
}

void SquadMenu::bind(Console& console)
{
	const auto local = [this] { return m_round.localPlayer(); };
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
	for (const auto& player : m_round.playerList()) {
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
	if (!m_round.playerInfo(player, me)) {
		return false;
	}
	if ((me.squad == 0 && !me.commander) || !select) {
		int squad = freeSquad(me.team);
		if (me.squad > 0) {
			// A leader alone in his squad makes it again.
			int members = 0;
			for (const auto& other : m_round.playerList()) {
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
	if (!m_createShow || !m_round.playerInfo(player, me)) {
		return false;
	}
	const int squad = me.squad > 0 ? me.squad : freeSquad(me.team);
	bool made = squad > 0 && (me.squad == squad || m_round.joinSquad(player, squad));
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
	if (!m_round.playerInfo(player, me) || me.commander) {
		return false;
	}
	if (me.squad != 0) {
		message("HUD_HUD_SQUAD_MUST_LEAVE_TO_JOIN");
		return false;
	}
	if (locked(me.team, squad)) {
		return false;
	}
	return m_round.joinSquad(player, squad);
}

void SquadMenu::leave(int player)
{
	game::GameWorld::PlayerInfo me;
	if (m_round.playerInfo(player, me) && me.squad > 0) {
		if (me.squadLeader) {
			m_locked.erase({me.team, me.squad});
		}
		m_round.joinSquad(player, 0);
	}
}

void SquadMenu::clickSquadList(int player, int row, int column)
{
	const int id = std::atoi(m_hud.listCell(5, row, kCellId).c_str());
	const int kind = std::atoi(m_hud.listCell(5, row, kCellKind).c_str());
	game::GameWorld::PlayerInfo me;
	if (kind != kRowSquad || !m_round.playerInfo(player, me)) {
		return; // selecting a member or a squad
	}
	if (column == kColumnJoin) {
		if (id == me.squad) {
			leave(player);
		} else {
			join(player, id);
		}
	} else if (column == kColumnLock && id == me.squad && me.squadLeader) {
		if (!m_locked.erase({me.team, id})) {
			m_locked.insert({me.team, id});
		}
	}
}

void SquadMenu::setShowInviteList(int player, bool show)
{
	game::GameWorld::PlayerInfo me;
	if (!m_round.playerInfo(player, me) || !me.squadLeader) {
		return;
	}
	m_inviteShow = show;
	if (show) {
		m_selected.clear();
		m_unselected.clear();
		for (const auto& other : m_round.playerList()) {
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
	if (!m_round.playerInfo(player, me) || !m_round.playerInfo(id, other) || other.commander || id == player) {
		return;
	}
	if (other.squad != me.squad) {
		if (m_selected.erase(id)) {
			return;
		}
		int members = 0;
		for (const auto& mate : m_round.playerList()) {
			members += mate.team == me.team && mate.squad == me.squad ? 1 : 0;
		}
		int invited = 0;
		for (const int selected : m_selected) {
			game::GameWorld::PlayerInfo info;
			invited += m_round.playerInfo(selected, info) && info.squad != me.squad ? 1 : 0;
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
	if (!m_round.playerInfo(player, me) || me.squad == 0) {
		return;
	}
	for (const int id : m_unselected) {
		game::GameWorld::PlayerInfo other;
		if (m_round.playerInfo(id, other) && other.team == me.team && other.squad == me.squad) {
			m_round.joinSquad(id, 0);
		}
	}
	for (const int id : m_selected) {
		game::GameWorld::PlayerInfo other;
		if (!m_unselected.count(id) && m_round.playerInfo(id, other) && other.ai && other.squad == 0
			&& !locked(me.team, me.squad)) {
			m_round.joinSquad(id, me.squad);
		}
	}
	m_inviteShow = false;
}

void SquadMenu::update(int player)
{
	game::GameWorld::PlayerInfo me;
	if (!m_round.playerInfo(player, me)) {
		return;
	}
	const auto players = m_round.playerList();
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
	// The squads, each followed by its members (the leader first).
	std::vector<std::vector<std::string>> rows;
	for (auto& [squad, members] : squads) {
		std::stable_sort(members.begin(), members.end(), [](const auto* a, const auto* b) { return a->squadLeader > b->squadLeader; });
		std::vector<std::string> row(15);
		row[1] = std::to_string(squad);
		row[5] = squadLabel(me.team, squad);
		row[6] = std::to_string(members.size()) + "/" + std::to_string(kSquadSize);
		row[kColumnLock] = locked(me.team, squad) ? "L" : "";
		row[kColumnJoin] = text(squad == me.squad ? "HUD_TEXT_MENU_SPAWN_SQUAD_LEAVE" : "HUD_TEXT_MENU_SPAWN_SQUAD_JOIN");
		row[kCellId] = std::to_string(squad);
		row[kCellKind] = std::to_string(kRowSquad);
		rows.push_back(std::move(row));
		for (const auto* member : members) {
			std::vector<std::string> line(15);
			line[5] = "  " + member->name;
			line[kCellId] = std::to_string(member->id);
			line[kCellKind] = std::to_string(kRowMember);
			rows.push_back(std::move(line));
		}
	}
	m_hud.setList(5, std::move(rows));
	// The invite list: the team's players but the leader, selected ones
	// highlighted ("|c").
	if (m_inviteShow) {
		std::vector<std::vector<std::string>> invite;
		for (const auto& other : players) {
			if (other.team != me.team || other.id == player || other.commander) {
				continue;
			}
			const bool chosen = other.squad == me.squad ? !m_unselected.count(other.id) : m_selected.count(other.id) != 0;
			invite.push_back({(chosen ? "|c" : "") + other.name, "", std::to_string(other.id)});
		}
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
	m_hud.setListColumns(15, {{4, 14, 2}, {20, 100, 0}, {-1, 0, 0}, {-1, 0, 0}, {124, 50, 2}});
	m_hud.setListColumns(14, {{6, 200, 0}});
}

void CommanderMenu::bind(Console& console)
{
	const auto local = [this] { return m_round.localPlayer(); };
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

void CommanderMenu::deselect()
{
	// 0x007a9db0: from the members' state (0x11) back to 0x10.
	m_current = -1;
	tabs.squadMembers = false;
}

void CommanderMenu::click(int player, int row, int column)
{
	game::GameWorld::PlayerInfo me;
	if (!m_round.playerInfo(player, me) || !me.commander || m_hud.listCell(15, row, kCellKind) != "0") {
		return;
	}
	const int squad = std::atoi(m_hud.listCell(15, row, kCellId).c_str());
	if (column == 3) {
		return; // the leader's voice
	}
	m_selected = {squad};
	if (column == 4) {
		m_current = squad;
		tabs.squadMembers = true;
	}
}

void CommanderMenu::update(int player)
{
	game::GameWorld::PlayerInfo me;
	if (!m_round.playerInfo(player, me)) {
		return;
	}
	std::map<int, std::vector<game::GameWorld::PlayerInfo>> squads;
	for (const auto& other : m_round.playerList()) {
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
		row[kCellId] = std::to_string(squad);
		row[kCellKind] = "0";
		if (squad == m_current) {
			selectedRow = static_cast<int>(rows.size());
		}
		rows.push_back(std::move(row));
	}
	m_hud.setList(15, std::move(rows));
	m_hud.setListSelection(15, selectedRow);
	std::vector<std::vector<std::string>> memberRows;
	std::string leader;
	if (const auto it = squads.find(m_current); it != squads.end()) {
		for (const auto& member : it->second) {
			if (member.squadLeader) {
				leader = member.name;
			} else {
				memberRows.push_back({member.name});
			}
		}
	}
	m_hud.setList(14, std::move(memberRows));
	m_hud.setString("CurrentSquadName", m_current >= 0 ? squadName(m_current) : "");
	m_hud.setString("CurrentSquadLeaderName", leader);
	m_hud.set("ShowSquadLeaderStats", leader.empty() ? 0 : 1);
	tabs.apply(m_hud);
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

} // namespace dice::hfe::menu
