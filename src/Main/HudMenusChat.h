// The HUD's message lists and text boxes (BF2142.exe's HUD manager,
// 0x009a6b50, and its message list class 0x007b6d80 / vtable 0x009b63d8):
//
// - ChatList (list data 3, HudElementsChat.con): chat lines, kill messages
//   and game information. A line is "§1" + text + "§0" (0x009b5ad4,
//   0x009b6358); chat reads "<channel prefix><name>: <text>" (0x007b4cd0,
//   "%s: " at 0x009b63d0; the prefixes HUD_TEXT_CHAT_TEAM / _SQUAD /
//   _COMMANDER and HUD_CHAT_DEADPREFIX), coloured by who said it (the
//   table at +0x178 picked by 0x007b4100): the commander (0.969, 0.827,
//   0.255), the own squad (0.4, 1, 0), the own team (0.012, 0.722, 0.949),
//   the enemy (1, 0.392, 0.392), nobody's (0.765, 0.729, 0.663). Lines
//   belong to nine categories (chat, kills and game info, ...) that each
//   keep 5 s from their latest line (+0x154..+0x174): when that runs out
//   the category's oldest line goes (0x007b4a40 → 0x007b4800).
// - Kill messages (0x00759bf0 → ChatList slot 0x28): HUD_HUD_KILLEDBY_KILLS
//   / _KILLED / _TEAMKILLS / _MYSELF with #PLAYERNAME1#, #WEAPON# (the
//   weapon's weaponHud.hudName, or the vehicle's vehicleHud.hudName) and
//   #PLAYERNAME2#, category 1; the victim's own "you were killed by" is
//   HUD_CHAT_YOUWEREKILLEDBY in DeathMessage (HudElementsKilled.con).
// - PersonalList (list data 13): personal notices (the chat spam block,
//   HUD_HUD_MENU_CHAT_SPAM_MESSAGE), 5 s each.
// - The say boxes (HudManager.enableSay{All,Team,Squad}ChatBox
//   0x007d93e0..0x007d95b0, opened by c_GISayAll / c_GISayTeam /
//   c_GISaySquad, 0x00755020): SayAllShow / SayTeamShow / SaySquadShow and
//   the edit box of data 1 (at most 150 characters); the same key again
//   closes it (0x007da200). Keys go through 0x007da320: printable
//   characters are added, Backspace takes one off, Enter sends
//   (0x007d9640..0x007d9e20) and closes. The squad's needs a squad.
// - The squad name box (SquadMenu.setSquadNameSelect, 0x007a5300 →
//   0x007d6420): edit data 2, at most 8 characters, sent with createSquad.
// - VoipSquadList (list data 11, 0x007be060): the team's commander, then
//   the squad's members (the squad leaders for the commander), those
//   talking: an icon (kit_Commander.tga in (0.4, 0.6, 1), or
//   mini_Squad_<n>_small.tga in the squad colour) at x 15 and the name at
//   x 30 (0x007bdd20); VoiceOverTalkIconShow while we send.
// - MapList (list data 12, LevelsList 0x00796ae0): a row per map of the
//   server's list (0x00796690): the vote button (kickbutton_up.tga, or
//   _down.tga while a vote runs), vote_square.tga on the map voted for,
//   the name with '_' as spaces. A click on the first two columns votes
//   for the row's map (0x00796a60 → 0x00573df0: a map vote, kind 1).
#pragma once

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dice::hfe::hud {
class HudManager;
}

namespace dice::hfe::menu {

class HudChat {
public:
	explicit HudChat(hud::HudManager& hud);
	void setLocalizer(std::function<std::string(const std::string&)> localize) { m_localize = std::move(localize); }

	// Who said a line, for its colour.
	enum class Speaker { Commander, Squad, Team, Enemy, Neutral };
	static std::array<float, 4> colorOf(Speaker speaker);
	// The chat channels as the say boxes send them (net::ChatChannel).
	// kPlayer: the whisper box (HudManager.enableSayPlayerChatBox, slot
	// 0xd8 → 0x007d95b0: SayPlayerShow, flag +0x3b), sent by 0x007d9e20 as
	// HUD_TEXT_CHAT_WHIPSER + the text to the HUD manager's player +0x48.
	enum Box { kNone = 0, kAll = 1, kTeam = 2, kSquad = 3, kPlayer = 4 };

	void addChat(const std::string& name, const std::string& text, int channel, Speaker speaker, bool dead = false);
	// A kill: HUD_HUD_KILLEDBY_* by who killed whom with what (the weapon's
	// text id or name), coloured as the killer.
	void addKill(const std::string& killer, const std::string& victim, const std::string& weapon, bool teamKill,
		Speaker color);
	void addInfo(const std::string& text, int category = 1);
	void addPersonal(const std::string& text);
	// The local player's death message (DeathMessage), empty when alive.
	void setKilledBy(const std::string& killer);

	// The say boxes.
	void open(Box box);
	void close();
	Box typing() const { return m_box; }
	void type(const std::string& text); // typed text (UTF-8; printable ASCII is kept)
	void key(int code);                 // 8 Backspace, 13 Enter, 27 Esc
	// A line to send: the box (kAll, kTeam, kSquad) and the text.
	std::function<void(Box box, const std::string& text)> onSay;

	// Each frame: the lists' timeouts and the variables.
	void update(float seconds);
	const std::vector<std::string>& chatRows() const { return m_rows; }

private:
	struct Line {
		std::string text;
		std::array<float, 4> color;
		int category;
		bool personal;
	};
	std::string text(const std::string& key) const { return m_localize ? m_localize(key) : key; }
	void push(Line line);

	hud::HudManager& m_hud;
	std::function<std::string(const std::string&)> m_localize;
	std::deque<Line> m_lines;
	std::array<float, 9> m_left {}; // each category's time left
	std::vector<std::string> m_rows;
	Box m_box = kNone;
	std::string m_typed;
};

// The squad name box (edit data 2).
class SquadNameBox {
public:
	explicit SquadNameBox(hud::HudManager& hud) : m_hud(hud) {}
	void select();          // SquadMenu.setSquadNameSelect
	bool typing() const { return m_typing; }
	void type(const std::string& text);
	void key(int code);
	std::string take();     // the name typed (createSquad), the box emptied
private:
	hud::HudManager& m_hud;
	bool m_typing = false;
	std::string m_name;
};

// The scoreboard's map vote list (LevelsList).
class MapVoteList {
public:
	explicit MapVoteList(hud::HudManager& hud);
	void setMaps(std::vector<std::string> maps) { m_maps = std::move(maps); }
	void setShow(bool show);              // levelsList.setVoteMapShow
	void click(int row, int column);      // levelsList.singleClick
	std::function<void(int map)> onVote;  // the map list index voted for
	int voted() const { return m_voted; }
	void update();

private:
	hud::HudManager& m_hud;
	std::vector<std::string> m_maps;
	bool m_shown = false;
	int m_voted = -1;
};

// The client's view of the server's votes and its HUD lines (BF2142.exe's
// client vote object: the Vote event's handler 0x005740e0, its tick
// 0x00573ae0, the lines 0x005725e0, the start checks 0x00573cb0 /
// 0x00573df0 / 0x00573f20). The event's phase (game::Votes::Phase) picks
// the line (0x005725e0's case):
//   - 0 start: the vote's kind (+4), target (+8, a player's name +0xc), the
//     starter (+0x28, name +0x2c), its time (+0x54), the yes votes it needs
//     (+0x50); the starter waits 300 s before another (+0x58); line 7;
//   - 1 failed: 10 (PLAYER_STAYS / COMMANDER_STAYS / MAP_STAYS);
//   - 2 cancelled: 0xd for a mutiny (COMMANDER_NOT_IN_GAME), else 0xc
//     (PLAYER_NOT_IN_GAME);
//   - 3 passed: 9 (PLAYER_KICKED / COMMANDER_KICKED / MAP_CHANGE);
//   - 4 a vote cast: the voter's state (player slot 0x30c: 2 yes, 3 no);
//     the local player's own is 0xb (VOTE_YES / VOTE_NO);
//   - 5 a target not in the game: 3 (PLAYER_NOT_IN), 6 another vote
//     running: 1 (IN_PROGRESS), 7 too few players: 6
//     (UNABLE_TO_START_VOTE), to the starter only.
// While a vote runs, the start line comes again every 5 s until the local
// player has voted (+1, +0x4c) and the count (line 8, *_STAT) every 30 s
// (+0x48). Before sending a start the client refuses a vote against itself
// (2, CANNOT_KICK_SELF), the host (4, CANNOT_KICK_HOST) and a start inside
// its 300 s wait (0, UNABLE_TO_ISSUE with #MINUTES#). The start lines go
// to the HUD's slot 0x174, the others to 0x170 (both the PersonalList,
// +0xf0, 0x007c7490 / 0x007c7520). #MINUTES# is ceil (0x00448120) of the
// minutes left; #YESVOTES# counts the players whose vote state is 1 (the
// starter) or 2 (yes) (0x005724b0); #NEEDED# is the start's +0x50; a
// map's name has '_' as ' '.
class VoteMessages {
public:
	struct Event {
		int kind = 0, phase = 0, target = 0, voter = -1, choice = 0;
		int needed = 0;
		float seconds = 0;
	};
	std::function<std::string(const std::string&)> localize;
	std::function<std::string(int player)> playerName; // "" for none
	std::function<std::string(int map)> mapName;       // the map list's name
	std::function<void(const std::string& line)> onLine;
	int localPlayer = -1;

	void handle(const Event& event);
	void tick(float seconds);
	// The client's checks before it sends a start (true: send it).
	bool mayStart(int kind, int target, bool targetIsHost);
	bool running() const { return m_kind != 0; }

private:
	enum Line { kUnable = 0, kInProgress = 1, kSelf = 2, kNotIn = 3, kHost = 4, kTooFew = 6, kStart = 7, kStat = 8,
		kPassed = 9, kFailed = 10, kVoted = 11, kLeft = 12, kResigned = 13 };
	void line(int which, int choice = 0);
	std::string text(const std::string& key) const { return localize ? localize(key) : key; }
	std::string targetLabel() const;
	void clear();
	int yesVotes() const;

	int m_kind = 0, m_target = 0, m_starter = -1, m_needed = 0;
	std::string m_targetName, m_starterName;
	float m_left = 0, m_wait = 0, m_statTimer = 0, m_startTimer = 0;
	bool m_voted = false;
	std::map<int, int> m_states; // player -> 1 starter, 2 yes, 3 no
};

// The VOIP list and icon (VoipSquadList, VoiceOverTalkIconShow).
struct VoipSpeaker {
	std::string name;
	bool commander = false;
	int squad = 0;
};
void updateVoipList(hud::HudManager& hud, const std::vector<VoipSpeaker>& talking, bool sending, bool commander);

} // namespace dice::hfe::menu
