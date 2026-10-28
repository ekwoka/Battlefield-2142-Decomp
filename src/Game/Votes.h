// The server's votes (BF2142.exe's vote manager DAT_00a693f8: constructor
// 0x0058feb0, tick 0x0058fdc0, the Vote event 0x27's handler 0x00590230,
// a vote's start / cast 0x0058f470, its end 0x0058fb90). Three kinds: a map
// vote (target a map-list index, kind 1), a kick vote (a player, 2) and a
// mutiny (the team's commander, 3). Everything is skipped unless
// sv.votingEnabled.
//
// A vote counts the eligible voters when it starts (0x0058f220: the human
// players who haven't opted out; a kick's target and a mutiny's other team
// left out) and needs ceil(2/3 of them) (0x3f2aaa9f, 0x00645b20; 0x0058f340)
// yes votes; it ends once that many said yes, everyone voted, or
// sv.voteTime ran out (0x0058ef90). The starter's vote is the choice 0;
// the rewrite counts a cast's choice 0 as yes and 1 or 2 as no (the
// choice's meaning isn't settled).
#pragma once

#include <functional>
#include <map>
#include <set>
#include <vector>

namespace dice::hfe::game {

class Votes {
public:
	enum Kind { kMap = 1, kKick = 2, kMutiny = 3 };
	enum Phase {
		kStart = 0, kFailed = 1, kCancelled = 2, kPassed = 3, kCast = 4, kBadTarget = 5, kRefused = 6,
		kTooFewPlayers = 7, kOptOut = 8, kState = 9
	};
	struct Settings {
		bool enabled = true;     // sv.votingEnabled (+0x9c)
		float voteTime = 90;     // sv.voteTime (+0xa0), seconds
		int minPlayers = 0;      // sv.minPlayersForVoting (+0xa4)
		float required = 0.66666597f; // 0x3f2aaa9f
	};
	struct Voter {
		int id = -1;
		int team = 0;
		bool human = true;
	};
	// The Vote event's fields as the server sends them (0x005113c0's order).
	struct Reply {
		int kind = 0, phase = 0, target = 0, voter = 0, choice = 0;
		int topCount = 0, topChoice = 0, needed = 0;
		float seconds = 0; // a start's time to run
		int to = -1;       // -1: everyone, else that player only
	};
	struct Result {
		int kind = 0, target = 0, team = 0;
	};

	Settings settings;
	// The players now; the map list's size and the map running; whether a
	// player may be kicked (exists, not the host).
	std::function<std::vector<Voter>()> players;
	int maps = 0, currentMap = -1;
	std::function<bool(int player)> kickable;
	std::function<int(int team)> commander; // -1: none

	// A Vote event from `player` (0x00590230).
	std::vector<Reply> handle(int player, int kind, int phase, int target, int choice);
	// The running votes' ends (0x0058fdc0): what passed, and the replies.
	std::vector<Result> tick(float seconds, std::vector<Reply>& replies);
	bool running(int kind, int team = 0) const;
	// A player gone: the votes about it end (0x0058f6f0, phase 2).
	std::vector<Reply> playerLeft(int player);

private:
	struct Vote {
		bool on = false;
		int kind = 0, target = 0, team = 0, starter = -1;
		std::set<int> eligible;
		std::map<int, int> choices; // voter -> choice
		int needed = 0;
		float left = 0;
		int yes() const;
	};
	Vote* slot(int kind, int team);
	int eligibleCount(int kind, int target, int team, std::set<int>* out) const;
	Reply replyOf(const Vote& vote, int phase, int voter, int choice) const;
	std::vector<Vote> m_votes; // map, kick, mutiny 1, mutiny 2
	std::set<int> m_optedOut;  // phase 8 (GeneralSettings.setOutOfVoting)
};

} // namespace dice::hfe::game
