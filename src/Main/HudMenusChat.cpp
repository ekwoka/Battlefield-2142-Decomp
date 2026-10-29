#include "HudMenusChat.h"

#include "Hud/Hud.h"

#include <algorithm>

namespace dice::hfe::menu {

namespace {

constexpr float kLineSeconds = 5.0f; // each category's lifetime (+0x154..+0x174)
constexpr size_t kMaxTyped = 150;    // setEditNodeMaxLength 150 (HudElementsChat.con)

std::string replaced(std::string text, const std::string& from, const std::string& to)
{
	for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
		text.replace(at, from.size(), to);
	}
	return text;
}

std::string printable(const std::string& text)
{
	// 0x007da320 takes 0x20..0x7f.
	std::string out;
	for (const char c : text) {
		if (c >= 0x20 && c < 0x7f) {
			out += c;
		}
	}
	return out;
}

} // namespace

HudChat::HudChat(hud::HudManager& hud) : m_hud(hud)
{
	m_hud.setListColumns(3, {{0, 0, 0}});
	m_hud.setListColumns(13, {{0, 0, 0}});
}

std::array<float, 4> HudChat::colorOf(Speaker speaker)
{
	switch (speaker) {
	case Speaker::Commander: return {0.969f, 0.827f, 0.255f, 1};
	case Speaker::Squad: return {0.4f, 1, 0, 1};
	case Speaker::Team: return {0.012f, 0.722f, 0.949f, 1};
	case Speaker::Enemy: return {1, 0.392f, 0.392f, 1};
	default: return {0.765f, 0.729f, 0.663f, 1};
	}
}

void HudChat::push(Line line)
{
	m_left[static_cast<size_t>(std::clamp(line.category, 0, 8))] = kLineSeconds;
	m_lines.push_back(std::move(line));
}

void HudChat::addChat(const std::string& name, const std::string& message, int channel, Speaker speaker, bool dead)
{
	// The channel's prefix (0x007b4cd0): team, squad, commander.
	std::string prefix;
	if (channel == 1) {
		prefix = text("HUD_TEXT_CHAT_TEAM");
	} else if (channel == 3 || channel == 5) {
		prefix = text("HUD_TEXT_CHAT_SQUAD");
	} else if (channel == 2) {
		prefix = text("HUD_TEXT_CHAT_COMMANDER");
	}
	if (dead) {
		prefix = text("HUD_CHAT_DEADPREFIX") + prefix;
	}
	push({"\xa7" "1" + prefix + name + ": " + message + "\xa7" "0", colorOf(speaker), 0, false});
}

void HudChat::addKill(const std::string& killer, const std::string& victim, const std::string& weapon, bool teamKill,
	Speaker color)
{
	const char* key = killer.empty() || killer == victim ? "HUD_HUD_KILLEDBY_MYSELF"
		: teamKill                                     ? "HUD_HUD_KILLEDBY_TEAMKILLS"
													   : "HUD_HUD_KILLEDBY_KILLS";
	std::string line = text(key);
	line = replaced(line, "#PLAYERNAME1#", killer.empty() ? victim : killer);
	line = replaced(line, "#WEAPON#", weapon.empty() ? std::string() : text(weapon));
	line = replaced(line, "#PLAYERNAME2#", victim);
	push({"\xa7" "1" + line + "\xa7" "0", colorOf(color), 1, false});
}

void HudChat::addInfo(const std::string& message, int category)
{
	push({"\xa7" "1" + message + "\xa7" "0", colorOf(Speaker::Neutral), category, false});
}

void HudChat::addPersonal(const std::string& message)
{
	push({message, {1, 1, 1, 1}, 7, true});
}

void HudChat::setKilledBy(const std::string& killer)
{
	m_hud.setString("DeathMessage", killer.empty() ? std::string() : replaced(text("HUD_CHAT_YOUWEREKILLEDBY"), "#PLAYERNAME#", killer));
}

void HudChat::open(Box box)
{
	// enableSay*ChatBox: the same box again closes it.
	if (m_box == box) {
		close();
		return;
	}
	m_box = box;
	m_typed.clear();
	m_hud.setEditFocus(1);
}

void HudChat::close()
{
	m_box = kNone;
	m_typed.clear();
	if (m_hud.editFocus() == 1) {
		m_hud.setEditFocus(-1);
	}
}

void HudChat::type(const std::string& typed)
{
	if (m_box == kNone) {
		return;
	}
	m_typed += printable(typed);
	const int most = m_hud.editMaxLength(1);
	m_typed.resize(std::min(m_typed.size(), most > 0 ? static_cast<size_t>(most) : kMaxTyped));
}

void HudChat::key(int code)
{
	if (m_box == kNone) {
		return;
	}
	if (code == 8 && !m_typed.empty()) {
		m_typed.pop_back();
	} else if (code == 13) {
		const Box box = m_box;
		const std::string line = m_typed;
		close();
		if (!line.empty() && onSay) {
			onSay(box, line);
		}
	} else if (code == 27) {
		close();
	}
}

void HudChat::update(float seconds)
{
	// Each category's oldest line goes when its time runs out.
	for (int category = 0; category < 9; ++category) {
		float& left = m_left[static_cast<size_t>(category)];
		if (left <= 0) {
			continue;
		}
		left -= seconds;
		if (left > 0) {
			continue;
		}
		const auto oldest = std::find_if(m_lines.begin(), m_lines.end(), [category](const Line& l) { return l.category == category; });
		if (oldest != m_lines.end()) {
			m_lines.erase(oldest);
		}
		if (std::any_of(m_lines.begin(), m_lines.end(), [category](const Line& l) { return l.category == category; })) {
			left = kLineSeconds;
		}
	}
	std::vector<std::vector<std::string>> chat, personal;
	std::vector<std::array<float, 4>> chatColors, personalColors;
	m_rows.clear();
	for (const Line& line : m_lines) {
		(line.personal ? personal : chat).push_back({line.text});
		(line.personal ? personalColors : chatColors).push_back(line.color);
		if (!line.personal) {
			m_rows.push_back(line.text);
		}
	}
	m_hud.setList(3, std::move(chat));
	m_hud.setListRowColors(3, std::move(chatColors));
	m_hud.setList(13, std::move(personal));
	m_hud.setListRowColors(13, std::move(personalColors));
	m_hud.set("ChatListShow", m_lines.empty() ? 0 : 1);
	m_hud.set("SayAllShow", m_box == kAll ? 1 : 0);
	m_hud.set("SayTeamShow", m_box == kTeam ? 1 : 0);
	m_hud.set("SaySquadShow", m_box == kSquad ? 1 : 0);
	m_hud.set("SayPlayerShow", m_box == kPlayer ? 1 : 0);
	m_hud.setEditText(1, m_typed);
}

void SquadNameBox::select()
{
	m_typing = true;
	m_name.clear();
	m_hud.setEditFocus(2);
	m_hud.setEditText(2, m_name);
}

void SquadNameBox::type(const std::string& typed)
{
	if (!m_typing) {
		return;
	}
	m_name += printable(typed);
	const int most = m_hud.editMaxLength(2);
	m_name.resize(std::min(m_name.size(), static_cast<size_t>(most > 0 ? most : 8)));
	m_hud.setEditText(2, m_name);
}

void SquadNameBox::key(int code)
{
	if (!m_typing) {
		return;
	}
	if (code == 8 && !m_name.empty()) {
		m_name.pop_back();
	} else if (code == 13 || code == 27) {
		m_typing = false;
		if (m_hud.editFocus() == 2) {
			m_hud.setEditFocus(-1);
		}
	}
	m_hud.setEditText(2, m_name);
}

std::string SquadNameBox::take()
{
	std::string name = m_name;
	m_name.clear();
	m_typing = false;
	if (m_hud.editFocus() == 2) {
		m_hud.setEditFocus(-1);
	}
	m_hud.setEditText(2, m_name);
	return name;
}

MapVoteList::MapVoteList(hud::HudManager& hud) : m_hud(hud)
{
	// The vote button, the voted mark and the name (0x00796580).
	m_hud.setListColumns(12, {{4, 0, 0, 15}, {22, 0, 0, 19}, {46, 0, 0}});
}

void MapVoteList::setShow(bool show)
{
	m_shown = show;
}

void MapVoteList::click(int row, int column)
{
	// 0x00796a60: the first two columns vote.
	if (column > 1 || row < 0 || static_cast<size_t>(row) >= m_maps.size()) {
		return;
	}
	m_voted = row;
	if (onVote) {
		onVote(row);
	}
}

void MapVoteList::update()
{
	std::vector<std::vector<std::string>> rows;
	for (size_t i = 0; i < m_maps.size(); ++i) {
		std::string name = m_maps[i];
		std::replace(name.begin(), name.end(), '_', ' ');
		rows.push_back({m_voted >= 0 ? "Ingame/Scoreboard/Icons/kickbutton_down.tga" : "Ingame/Scoreboard/Icons/kickbutton_up.tga",
			static_cast<int>(i) == m_voted ? "Ingame/Scoreboard/Icons/vote_square.tga" : "", name});
	}
	m_hud.setList(12, std::move(rows));
	m_hud.set("VoteMapSelected", m_shown ? 1 : 0);
}

void updateVoipList(hud::HudManager& hud, const std::vector<VoipSpeaker>& talking, bool sending, bool commander)
{
	// 0x007be120 / 0x007bdd20: the icon at 15, the name at 30.
	hud.setListColumns(11, {{15, 0, 0, 15}, {30, 0, 0}});
	std::vector<std::vector<std::string>> rows;
	std::vector<std::array<float, 4>> colors;
	for (const VoipSpeaker& s : talking) {
		if (rows.size() >= 10) {
			break;
		}
		rows.push_back({s.commander ? "Ingame/Kits/Icons/kit_Commander.tga"
									: "Ingame/Player/Icons/Minimap/mini_Squad_" + std::to_string(std::max(1, s.squad)) + "_small.tga",
			s.name});
		colors.push_back(s.commander ? std::array<float, 4> {0.4f, 0.6f, 1, 1} : HudChat::colorOf(HudChat::Speaker::Squad));
	}
	hud.set("VoipListShow", rows.empty() ? 0 : 1);
	hud.setList(11, std::move(rows));
	hud.setListRowColors(11, std::move(colors));
	hud.set("VoiceOverIconShow", sending ? 1 : 0);
	hud.set("VoiceOverTalkIconShow", sending && !commander ? 1 : 0);
	hud.set("VoiceOverCommanderIconShow", sending && commander ? 1 : 0);
}

} // namespace dice::hfe::menu
