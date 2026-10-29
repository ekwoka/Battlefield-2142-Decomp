#include "Votes.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::game {

int Votes::Vote::yes() const
{
	return static_cast<int>(std::count_if(choices.begin(), choices.end(), [](const auto& c) { return c.second == 0; }));
}

int Votes::Vote::no() const
{
	return static_cast<int>(std::count_if(choices.begin(), choices.end(), [](const auto& c) { return c.second == 1; }));
}

Votes::Vote* Votes::slot(int kind, int team)
{
	if (m_votes.size() < 4) {
		m_votes.resize(4);
	}
	switch (kind) {
	case kMap: return &m_votes[0];
	case kKick: return &m_votes[1];
	case kMutiny: return team == 1 || team == 2 ? &m_votes[static_cast<size_t>(1 + team)] : nullptr;
	default: return nullptr;
	}
}

bool Votes::running(int kind, int team) const
{
	for (const Vote& v : m_votes) {
		if (v.on && v.kind == kind && (kind != kMutiny || team == 0 || v.team == team)) {
			return true;
		}
	}
	return false;
}

int Votes::eligibleCount(int kind, int target, int team, std::set<int>* out) const
{
	// 0x0058f220: the humans who haven't opted out; a kick's target and a
	// mutiny's other team (and its target) aren't asked.
	int count = 0;
	for (const Voter& p : players ? players() : std::vector<Voter> {}) {
		if (!p.human || m_optedOut.count(p.id)) {
			continue;
		}
		if ((kind == kKick || kind == kMutiny) && p.id == target) {
			continue;
		}
		if (kind == kMutiny && p.team != team) {
			continue;
		}
		++count;
		if (out) {
			out->insert(p.id);
		}
	}
	return count;
}

Votes::Reply Votes::replyOf(const Vote& vote, int phase, int voter, int choice) const
{
	Reply r;
	r.kind = vote.kind;
	r.phase = phase;
	r.target = vote.target;
	r.voter = voter;
	r.choice = choice;
	// The tally (0x00645a90: counts[choice]++): every event carries the
	// yes and no counts.
	for (const auto& [who, c] : vote.choices) {
		r.yesCount += c == 0 ? 1 : 0;
		r.noCount += c == 1 ? 1 : 0;
	}
	r.needed = vote.needed;
	return r;
}

std::vector<Votes::Reply> Votes::handle(int player, int kind, int phase, int target, int choice)
{
	std::vector<Reply> out;
	if (!settings.enabled) {
		return out;
	}
	if (m_votes.size() < 4) {
		m_votes.resize(4);
	}
	int team = 0;
	for (const Voter& p : players ? players() : std::vector<Voter> {}) {
		if (p.id == player) {
			team = p.team;
		}
	}
	const auto only = [&](int ph) {
		Reply r;
		r.kind = kind;
		r.phase = ph;
		r.target = target;
		r.voter = player;
		r.to = player;
		out.push_back(r);
	};
	if (phase == kOptOut) {
		if (choice != 0) {
			m_optedOut.insert(player);
		} else {
			m_optedOut.erase(player);
		}
		return out;
	}
	if (phase == kState) {
		for (const Vote& v : m_votes) {
			if (v.on) {
				Reply r = replyOf(v, kCast, v.starter, 0);
				r.to = player;
				out.push_back(r);
			}
		}
		return out;
	}
	if (phase == kStart) {
		const int voteTeam = kind == kMutiny ? team : 0;
		const int eligible = eligibleCount(kind, target, voteTeam, nullptr);
		// Too few to vote (phase 7); a lone player's map vote (the game by
		// itself) goes on.
		int humans = 0;
		for (const Voter& p : players ? players() : std::vector<Voter> {}) {
			humans += p.human ? 1 : 0;
		}
		if (eligible < settings.minPlayers && !(kind == kMap && humans == 1)) {
			only(kTooFewPlayers);
			return out;
		}
		// Another vote running (phase 6).
		if (running(kMap) || running(kKick) || (kind == kMutiny ? running(kMutiny, team) : running(kMutiny))) {
			only(kRefused);
			return out;
		}
		if (kind == kMap && (target < 0 || target >= maps || target == currentMap)) {
			return out;
		}
		if (kind == kKick && (!kickable || !kickable(target))) {
			only(kBadTarget);
			return out;
		}
		if (kind == kMutiny && (!commander || commander(team) != target || target == player)) {
			return out;
		}
		Vote* v = slot(kind, voteTeam);
		if (!v) {
			return out;
		}
		*v = {};
		v->on = true;
		v->kind = kind;
		v->target = target;
		v->team = voteTeam;
		v->starter = player;
		eligibleCount(kind, target, voteTeam, &v->eligible);
		v->needed = static_cast<int>(std::ceil(settings.required * static_cast<float>(v->eligible.size())));
		v->left = settings.voteTime;
		if (v->eligible.count(player)) {
			v->choices[player] = 0; // the starter's own
		}
		Reply r = replyOf(*v, kStart, player, 0);
		r.seconds = v->left;
		out.push_back(r);
		return out;
	}
	if (phase == kCast) {
		Vote* v = slot(kind, kind == kMutiny ? team : 0);
		if (!v || !v->on || !v->eligible.count(player) || v->choices.count(player) || choice < 0 || choice > 2) {
			return out; // one vote a player; 0..2 only (0x00645950)
		}
		v->choices[player] = choice;
		out.push_back(replyOf(*v, kCast, player, choice));
	}
	return out;
}

std::vector<Votes::Result> Votes::tick(float seconds, std::vector<Reply>& replies)
{
	std::vector<Result> results;
	for (Vote& v : m_votes) {
		if (!v.on) {
			continue;
		}
		v.left -= seconds;
		const int yes = v.yes();
		if (yes >= v.needed) {
			v.on = false;
			replies.push_back(replyOf(v, kPassed, v.starter, 0));
			results.push_back({v.kind, v.target, v.team});
		} else if (v.yes() + v.no() >= static_cast<int>(v.eligible.size()) || v.left <= 0) {
			// 0x00645bb0: every eligible voter said yes or no (a 2 counts
			// for neither), or the time is out.
			v.on = false;
			Reply r = replyOf(v, kFailed, v.starter, 0);
			r.needed = v.needed - yes; // the yes votes it lacked
			replies.push_back(r);
		}
	}
	return results;
}

std::vector<Votes::Reply> Votes::playerLeft(int player)
{
	std::vector<Reply> out;
	for (Vote& v : m_votes) {
		if (v.on && (v.kind == kKick || v.kind == kMutiny) && v.target == player) {
			v.on = false;
			out.push_back(replyOf(v, kCancelled, v.starter, 0));
		}
		v.eligible.erase(player);
		v.choices.erase(player);
	}
	m_optedOut.erase(player);
	return out;
}

} // namespace dice::hfe::game
