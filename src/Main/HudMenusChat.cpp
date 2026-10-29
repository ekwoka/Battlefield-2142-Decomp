#include "HudMenusChat.h"

#include <cmath>

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
	// HUD_HUD_KILLEDBY_MYSELF names the one who died as #PLAYERNAME#
	// (0x007518d0, 0x00759bf0 fill it with the player's name).
	line = replaced(line, "#PLAYERNAME#", victim);
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


// --- Votes (0x005740e0, 0x00573ae0, 0x005725e0) ---------------------------

void VoteMessages::clear()
{
	// 0x00572570: the vote forgotten, the players' vote states reset.
	m_kind = 0;
	m_target = 0;
	m_starter = -1;
	m_needed = 0;
	m_targetName.clear();
	m_starterName.clear();
	m_left = m_statTimer = m_startTimer = 0;
	m_voted = false;
	m_states.clear();
}

int VoteMessages::yesVotes() const
{
	int n = 0;
	for (const auto& [player, state] : m_states) {
		n += state == 1 || state == 2 ? 1 : 0;
	}
	return n;
}

std::string VoteMessages::targetLabel() const
{
	if (m_kind != 1) {
		return m_targetName;
	}
	std::string name = mapName ? mapName(m_target) : std::to_string(m_target);
	std::replace(name.begin(), name.end(), '_', ' ');
	return name;
}

void VoteMessages::line(int which, int choice)
{
	const auto minutes = [](float seconds) { return std::to_string(static_cast<int>(std::ceil(std::max(seconds, 0.0f) / 60))); };
	const char* key = nullptr;
	std::string out;
	const bool player = m_kind == 2, mutiny = m_kind == 3;
	switch (which) {
	case kUnable:
		out = replaced(text("HUD_HUD_KICKVOTE_UNABLE_TO_ISSUE"), "#MINUTES#", minutes(m_wait));
		break;
	case kInProgress: key = "HUD_HUD_KICKVOTE_IN_PROGRESS"; break;
	case kSelf: key = "HUD_HUD_KICKVOTE_CANNOT_KICK_SELF"; break;
	case kNotIn: key = "HUD_HUD_KICKVOTE_PLAYER_NOT_IN"; break;
	case kHost: key = "HUD_HUD_KICKVOTE_CANNOT_KICK_HOST"; break;
	case kTooFew: key = "HUD_HUD_KICKVOTE_UNABLE_TO_START_VOTE"; break;
	case kVoted: key = choice == 0 ? "HUD_HUD_KICKVOTE_VOTE_YES" : "HUD_HUD_KICKVOTE_VOTE_NO"; break;
	case kStart: {
		// Local when the local player started it or (a kick, a mutiny) is
		// its target: no keys to press, the time it runs.
		const std::string me = playerName ? playerName(localPlayer) : std::string();
		const bool local = (!me.empty() && me == m_starterName) || (m_kind != 1 && !me.empty() && me == m_targetName);
		if (m_kind == 1) {
			out = text(local ? "HUD_HUD_KICKVOTE_MAP_START_LOCAL" : "HUD_HUD_KICKVOTE_MAP_START_REMOTE");
			out = replaced(out, "#PLAYERNAME#", m_starterName);
			out = replaced(out, "#MAPNAME#", targetLabel());
		} else {
			out = text(player ? (local ? "HUD_HUD_KICKVOTE_PLAYER_START_LOCAL" : "HUD_HUD_KICKVOTE_PLAYER_START_REMOTE")
							  : (local ? "HUD_HUD_KICKVOTE_COMMANDER_START_LOCAL" : "HUD_HUD_KICKVOTE_COMMANDER_START_REMOTE"));
			out = replaced(out, "#PLAYERNAME1#", m_starterName);
			out = replaced(out, "#PLAYERNAME2#", m_targetName);
		}
		out = replaced(out, "#MINUTES#", minutes(m_left));
		break;
	}
	case kStat: case kPassed: case kFailed: {
		static const char* const keys[3][3] = {
			{"HUD_HUD_KICKVOTE_MAP_STAT", "HUD_HUD_KICKVOTE_MAP_CHANGE", "HUD_HUD_KICKVOTE_MAP_STAYS"},
			{"HUD_HUD_KICKVOTE_PLAYER_STAT", "HUD_HUD_KICKVOTE_PLAYER_KICKED", "HUD_HUD_KICKVOTE_PLAYER_STAYS"},
			{"HUD_HUD_KICKVOTE_COMMANDER_STAT", "HUD_HUD_KICKVOTE_COMMANDER_KICKED", "HUD_HUD_KICKVOTE_COMMANDER_STAYS"}};
		out = text(keys[player ? 1 : mutiny ? 2 : 0][which - kStat]);
		out = replaced(replaced(out, "#PLAYERNAME#", m_targetName), "#MAPNAME#", targetLabel());
		out = replaced(out, "#YESVOTES#", std::to_string(yesVotes()));
		out = replaced(out, "#NEEDED#", std::to_string(m_needed));
		break;
	}
	case kLeft: out = replaced(text("HUD_HUD_KICKVOTE_PLAYER_NOT_IN_GAME"), "#PLAYERNAME#", m_targetName); break;
	case kResigned: out = replaced(text("HUD_HUD_KICKVOTE_COMMANDER_NOT_IN_GAME"), "#PLAYERNAME#", m_targetName); break;
	default: return;
	}
	if (key) {
		out = text(key);
	}
	if (onLine && !out.empty()) {
		onLine(out);
	}
}

void VoteMessages::handle(const Event& e)
{
	switch (e.phase) {
	case 0: // a start (0x005721e0: a kind 1..3)
		if (e.kind < 1 || e.kind > 3) {
			return;
		}
		clear();
		m_kind = e.kind;
		m_target = e.target;
		m_left = e.seconds;
		m_starter = e.voter;
		m_needed = e.needed;
		if (m_kind != 1 && playerName) {
			m_targetName = playerName(m_target);
		}
		if (m_starter == localPlayer) {
			m_wait = 300; // +0x58 (0x43960000)
			m_voted = true;
		}
		m_starterName = playerName ? playerName(m_starter) : std::string();
		m_states[m_starter] = 1;
		line(kStart);
		return;
	case 1: line(kFailed); clear(); return;
	case 2: line(m_kind == 3 ? kResigned : kLeft); clear(); return;
	case 3: line(kPassed); clear(); return;
	case 4:
		if (e.choice == 2) {
			return;
		}
		m_states[e.voter] = e.choice == 0 ? 2 : 3;
		if (e.voter == localPlayer) {
			m_voted = true;
			line(kVoted, e.choice);
		}
		return;
	case 5: if (e.voter == localPlayer) line(kNotIn); return;
	case 6: if (e.voter == localPlayer) line(kInProgress); return;
	case 7: if (e.voter == localPlayer) line(kTooFew); return;
	default: return;
	}
}

void VoteMessages::tick(float seconds)
{
	m_wait = std::max(0.0f, m_wait - seconds);
	if (m_kind == 0) {
		return;
	}
	m_left -= seconds;
	m_statTimer += seconds;
	m_startTimer = m_voted ? 0 : m_startTimer + seconds;
	if (m_startTimer >= 5) {
		m_startTimer = 0;
		line(kStart);
	}
	if (m_statTimer >= 30) {
		m_statTimer = 0;
		line(kStat);
	}
}

bool VoteMessages::mayStart(int kind, int target, bool targetIsHost)
{
	// 0x00573f20 (a kick or a mutiny): not against oneself, not the host;
	// every start (also 0x00573cb0 / 0x00573df0): not inside the wait.
	if (kind != 1 && target == localPlayer) {
		line(kSelf);
		return false;
	}
	if (kind != 1 && targetIsHost) {
		line(kHost);
		return false;
	}
	if (m_wait > 0) {
		line(kUnable);
		return false;
	}
	return true;
}

} // namespace dice::hfe::menu
