// The squad leaders' requests to the commander (GameWorld::SquadRequest):
// BF2142.exe's squad manager slot 0x50 (0x004c5720) and the squad's record
// (0x004c3f20), slot 0x24 to take one off. The HUD's side (sending, the
// commander's answer) is src/Main/HudMenus (SquadRequests).
#include "GameWorld.h"

namespace dice::hfe::game {

bool GameWorld::storeSquadRequest(int team, int squad, int kind, const physics::Vec3& at)
{
	// 0x004c5720: nothing for squad 0 or a squad the manager doesn't have
	// (slot 0x2c); the squad's record keeps one request a kind (0x004c3f20:
	// its flag +0x2b + kind, the point at +0x68 + 0x30 kind).
	if ((team != 1 && team != 2) || squad <= 0 || kind < 0 || kind >= kRequestKinds) {
		return false;
	}
	bool led = false;
	for (const auto& p : m_players) {
		led = led || (p && p->present && p->team == team && p->squad == squad && p->squadLeader);
	}
	if (!led) {
		return false;
	}
	m_squadRequests[{team, squad}][static_cast<size_t>(kind)] = SquadRequest {team, squad, kind, at, static_cast<float>(m_time)};
	return true;
}

void GameWorld::removeSquadRequest(int team, int squad, int kind)
{
	const auto it = m_squadRequests.find({team, squad});
	if (it != m_squadRequests.end() && kind >= 0 && kind < kRequestKinds) {
		it->second[static_cast<size_t>(kind)].reset();
	}
}

const GameWorld::SquadRequest* GameWorld::squadRequest(int team, int squad, int kind) const
{
	const auto it = m_squadRequests.find({team, squad});
	if (it == m_squadRequests.end() || kind < 0 || kind >= kRequestKinds || !it->second[static_cast<size_t>(kind)]) {
		return nullptr;
	}
	return &*it->second[static_cast<size_t>(kind)];
}

std::vector<GameWorld::SquadRequest> GameWorld::squadRequests(int team) const
{
	// A squad without a leader has no record to hold them.
	std::vector<SquadRequest> out;
	for (const auto& [key, kinds] : m_squadRequests) {
		if (key.first != team) {
			continue;
		}
		bool led = false;
		for (const auto& p : m_players) {
			led = led || (p && p->present && p->team == team && p->squad == key.second && p->squadLeader);
		}
		for (const auto& request : kinds) {
			if (request && led) {
				out.push_back(*request);
			}
		}
	}
	return out;
}

} // namespace dice::hfe::game
