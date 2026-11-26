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

bool GameWorld::requestNewOrder(int player, int team, int squad, int type)
{
	// 0x00512100: the sender is the squad's leader (slot 0x40) or the team's commander (slot 0x78).
	const Player* p = findPlayer(player);
	if (!p || !p->present || (team != 1 && team != 2) || squad <= 0 || type < 0 || type > 3) {
		return false;
	}
	const bool leader = p->team == team && p->squad == squad && p->squadLeader;
	if (!leader && commander(team) != player) {
		return false;
	}
	if (commander(team) == localPlayer()) {
		m_orderStates[{team, squad}] = type; // 0x00512100 posts 2/0x19 to a commander on this machine
	}
	if (type == 1 && (aiAnswersOrderRequests() || virtualCommander(team))) {
		m_newOrderRequests.push_back({team, squad});
	}
	return true;
}

int GameWorld::squadOrderState(int team, int squad) const
{
	const auto it = m_orderStates.find({team, squad});
	return it != m_orderStates.end() && !squadMembers(team, squad).empty() ? it->second : 0; // 0x0078f680 clears an empty squad's
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
