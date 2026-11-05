// AIDLL.dll's squads: the squad object (0x100c4080 .. 0x100c4905), its
// members (0x100bf3d0 .. 0x100c0ee2), its formation (0x100f4350 ..
// 0x100f6e5c, the pure parts in src/AI/Formation) and the squad manager's
// round robin (0x1008a910).
//
// The squad manager (0x1008a720) makes nine squads a side up front and
// each AIMain frame (0x100571c0 -> 0x1008a910) updates one squad of one
// side, the sides taking turns: a squad every 18 frames. A squad's update
// (0x100c4240):
//   - an order under way (state 2), with no human commander, ends on an
//     attack whose area the side now holds, or on a defence of a held area
//     while the enemy holds an area whose point can change hands (the
//     SAI's slot 0x74, 0x1009c660 over IAIGame slot 0x24): state 0, kind 4.
//   - each member's place in the formation (0x100bfb60), the slots
//     swapped to the nearest members (0x100f69a0), each member's update
//     (0x100c0bb0), the mode (0x100c4010), the formation's update
//     (0x100f6dc0), the squad's spot (0x100c32b0), the danger list
//     (0x100c1b60), the repair marker's 60 s (+0xd4, 0x100c0fa0: the
//     bridge repair, BBBridgeRepair 0x10084620; no broken bridges), the
//     request for a new order (0x100c30f0) and the radio (0x100c2ee0).
#include "Bots.h"

#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace dice::hfe::game {

namespace {

constexpr float kSoldierAiRadius = 1.1489125f; // Bots.cpp's kSoldierRadius (the soldier's AI radius)
constexpr float kStrikeRadius = 40; // the squad manager's +0xb8 (0x10087f80)
constexpr float kStrikeLife = 25;   // +0xc0

float distance2(const physics::Vec3& a, const physics::Vec3& b)
{
	const physics::Vec3 d = a - b;
	return d.x * d.x + d.y * d.y + d.z * d.z;
}

float horizontalDistance2(const physics::Vec3& a, const physics::Vec3& b)
{
	return (a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z);
}

ai::Vec3 aiOf(const physics::Vec3& v) { return {v.x, v.y, v.z}; }
physics::Vec3 physicsOf(const ai::Vec3& v) { return {v.x, v.y, v.z}; }

// The AI's order kinds (0 attack, 1 defend, 2 move) from the game's.
int kindOf(GameWorld::OrderType type)
{
	using T = GameWorld::OrderType;
	switch (type) {
	case T::Attack:
	case T::MapAttack: return 0;
	case T::Defend:
	case T::MapDefend: return 1;
	case T::Move:
	case T::MapMove: return 2;
	default: return 4;
	}
}

} // namespace

const BotManager::Squad* BotManager::squad(int team, int id) const
{
	const auto it = m_squads.find({team, id});
	return it == m_squads.end() ? nullptr : &it->second;
}

bool BotManager::mayAdvance(int player) const
{
	const auto it = m_bots.find(player);
	return it == m_bots.end() || it->second.mayAdvance;
}

bool BotManager::formationGoal(int player, physics::Vec3& at) const
{
	const auto it = m_bots.find(player);
	ai::Vec3 head;
	if (it == m_bots.end() || !it->second.squadGoal || !ai::goalPoint(it->second.formation, head)) {
		return false;
	}
	at = physicsOf(head);
	return true;
}

void BotManager::updateSquadNow(int team, int id)
{
	m_players = m_world.playerList();
	m_playerIndex.clear();
	for (size_t i = 0; i < m_players.size(); ++i) {
		m_playerIndex[m_players[i].id] = i;
	}
	Squad& squad = m_squads[{team, id}];
	squad.team = team;
	squad.id = id;
	updateSquad(squad);
}

bool BotManager::humanCommands(int team) const
{
	// The SAI's slot 0x4c (0x1009bd50: +0x241, a human commander).
	const int boss = m_world.commander(team);
	const GameWorld::PlayerInfo* p = boss >= 0 ? playerOf(boss) : nullptr;
	return p && !p->ai;
}

const GameWorld::PlayerInfo* BotManager::squadLeaderOf(const Squad& squad) const
{
	if (squad.leader < 0 || static_cast<size_t>(squad.leader) >= squad.members.size()) {
		return nullptr;
	}
	return playerOf(squad.members[static_cast<size_t>(squad.leader)].player);
}

BotManager::SquadMember* BotManager::memberOf(Squad& squad, int player)
{
	// 0x100c27b0 / 0x100c27d0: a member by id.
	for (SquadMember& m : squad.members) {
		if (m.player == player) {
			return &m;
		}
	}
	return nullptr;
}

// The pathfinding courier's slots 0x38 and 0x2c (0x100c1300): the point
// on the member's map and on its island (the shipped meshes have one).
bool BotManager::reachable(const physics::Vec3& at) const
{
	return !m_navMesh || m_navMesh->findPolygon(aiOf(at)) >= 0;
}

// The strategic area for a point and side (0x10034590 -> 0x100349b0): of
// the areas, the one scoring most by its slot 0x68: its radius (+0x98 +
// 4 side) over max(1, d^2) to its position for the side (slot 8: the
// infantry order position, else its control point's). Its position and
// radius out.
int BotManager::areaAt(const physics::Vec3& at, int team, physics::Vec3* centre, float* radius) const
{
	(void)team;
	const auto& areas = m_settings.strategy.areas;
	int chosen = -1;
	float best = -1;
	for (const GameWorld::ControlPointInfo& point : m_world.controlPointList()) {
		const int index = areaOfControlPoint(point.id);
		if (index < 0) {
			continue;
		}
		const ai::StrategicArea& a = areas[static_cast<size_t>(index)];
		const physics::Vec3 c = a.hasOrderPosition[0] ? physics::Vec3 {a.orderPosition[0][0], a.orderPosition[0][1], a.orderPosition[0][2]}
													  : point.position;
		const float d2 = horizontalDistance2(at, c);
		const float value = a.temperature / std::max(1.0f, d2);
		if (value > best) {
			best = value;
			chosen = index;
			if (centre) {
				*centre = c;
			}
			if (radius) {
				*radius = a.temperature;
			}
		}
	}
	return chosen;
}

// An area's status for a side (+0x70 + 4 side, 0x1005f210): its control
// point's: Owned (Neutral while its flag is below 0.9), Neutral, Hostile.
ai::AreaStatus BotManager::areaStatus(int area, int team) const
{
	physics::Vec3 at;
	float radius = 0;
	int owner = 0;
	if (!areaPosition(area, at, radius, owner)) {
		return ai::AreaStatus::Neutral;
	}
	if (owner == team) {
		const int cpId = m_settings.strategy.areas[static_cast<size_t>(area)].controlPoint;
		for (const ControlPoint& cp : m_world.controlPoints()) {
			if (cp.id == cpId && cp.flag == team && cp.flagHeight < 0.9f) {
				return ai::AreaStatus::Neutral;
			}
		}
		return ai::AreaStatus::Owned;
	}
	return owner == 0 ? ai::AreaStatus::Neutral : ai::AreaStatus::Hostile;
}

// The squad manager (0x1008a910): the sides take turns, one squad each,
// the cursor moving on after the second side (+0x20, +0x24).
void BotManager::updateSquads()
{
	if (m_squads.empty()) {
		for (int team = 1; team <= 2; ++team) {
			for (int id = 1; id <= 9; ++id) {
				Squad& s = m_squads[{team, id}];
				s.team = team;
				s.id = id;
				s.requestedAt = m_time; // 0x100c4080: +0x100 the clock
				// The formation's constructor (0x100f67d0): the speed
				// (0x100f5120) and the shape's offsets, no slot used yet.
				ai::layOutFormation(s.formation, [](int) { return 0.0f; });
				// The SAI's reset at the round's start (0x1009d190 ->
				// 0x10089d80 -> 0x100c1700) resets the squads and their
				// members (0x100bf640: +0x4d set until their bot's next
				// Idle plan ends, 0x100d28a0).
				syncSquadMembers(s);
				for (SquadMember& m : s.members) {
					m.fresh = true;
					m.regoal = true;
				}
			}
		}
	}
	// The commanders' strikes reach the squad manager as they are called
	// (0x10088040): the AI commander's from its own artillery (0x1009c050,
	// below; its answers to requests 0x1009e020 / 0x1009e200, Bots.cpp's
	// answerRequests), a human commander's orbital strike and
	// artillery through IAIEventCourier slot 0x5c (0x1004e690, from
	// BF2142.exe 0x007d7d80 / 0x007d88d0 as the tool is used). Here: a
	// strike new in the world's list since the last tick.
	std::vector<std::pair<int, physics::Vec3>> strikes;
	for (const GameWorld::Strike& strike : m_world.strikes()) {
		if (strike.ability != GameWorld::CommanderAbility::OrbitalStrike && strike.ability != GameWorld::CommanderAbility::Artillery) {
			continue;
		}
		strikes.emplace_back(strike.team, strike.target);
		const bool seen = std::any_of(m_strikesSeen.begin(), m_strikesSeen.end(), [&](const auto& s) {
			return s.first == strike.team && distance2(s.second, strike.target) < 1e-4f;
		});
		if (!seen) {
			noteStrike(strike.team, strike.target);
		}
	}
	m_strikesSeen = std::move(strikes);
	updateStrikeSpots();
	m_squadSide = m_squadSide == 0 ? 1 : 0;
	updateSquad(m_squads[{m_squadSide + 1, m_squadCursor + 1}]);
	if (m_squadSide == 1) {
		if (m_squadCursor >= 8) {
			m_squadCursor = 0;
			trackHumans(); // 0x1008a3b0, once the cursor wraps
		} else {
			++m_squadCursor;
		}
	}
}

// AIMain's population (vtable 0x10114d30). A human joining (slot 7,
// 0x10056110 with a false "bot" flag; a bot joining only counts, +0xfc)
// is counted (+0x100); with maxBotsIncludeHumans (IAISettings slot 3,
// +0x37), bots about and the server's maximum (DAT_10139454 slot 4) under
// humans plus bots, one bot goes (IAIGameEffector slots 0x24 / 0x14): the
// last bot in the player list of the team with more players (team 2 on a
// tie), of the other team when that one has none. A human leaving (slot
// 8, 0x10056d10) leaves its squad (0x10089860) and, with
// maxBotsIncludeHumans and bots plus humans under the maximum, the fill
// (slot 20, 0x10056680) runs: with autoSpawnBots (IAISettings slot 40,
// +0x36: aiSettings.autoSpawnBots, which the shipped scripts don't use;
// their setAutoSpawnBots is no command) and the game's slot 0x5c, as many
// bots as setMaxNBots less the humans (with maxBotsIncludeHumans) less
// the bots there are, through the AI player factory (slot 0x18: here
// addBot, on the side with fewer players). The original counts the
// join and leave events; the rewrite sees the human players come and go.
void BotManager::population()
{
	std::set<int> humans;
	int bots = 0;
	for (const GameWorld::PlayerInfo& p : m_world.playerList()) {
		if (p.ai) {
			++bots;
		} else {
			humans.insert(p.id);
		}
	}
	std::set<int> counted;
	for (int id : m_humansSeen) {
		if (humans.count(id)) {
			counted.insert(id);
		}
	}
	const bool leaving = counted.size() < m_humansSeen.size();
	for (int id : humans) {
		if (counted.count(id)) {
			continue;
		}
		counted.insert(id);
		const int total = static_cast<int>(counted.size());
		if (!m_settings.maxBotsIncludeHumans || bots <= 0 || m_serverMaxPlayers <= 0 || m_serverMaxPlayers >= total + bots) {
			continue;
		}
		int count[3] = {0, 0, 0};
		int last[3] = {-1, -1, -1};
		for (const GameWorld::PlayerInfo& p : m_world.playerList()) {
			if (p.team == 1 || p.team == 2) {
				++count[p.team];
				if (p.ai) {
					last[p.team] = p.id;
				}
			}
		}
		int team = count[2] < count[1] ? 1 : 2;
		if (last[team] == -1) {
			team = team == 1 ? 2 : 1;
		}
		if (last[team] != -1 && m_world.removePlayer(last[team])) {
			m_bots.erase(last[team]);
			--bots;
		}
	}
	m_humansSeen = counted;
	if (leaving && m_settings.maxBotsIncludeHumans && m_serverMaxPlayers > 0 && bots + static_cast<int>(counted.size()) < m_serverMaxPlayers
		&& m_settings.autoSpawnBots) {
		int wanted = m_settings.maxBots - (m_settings.maxBotsIncludeHumans ? static_cast<int>(counted.size()) : 0) - bots;
		for (; wanted > 0; --wanted) {
			int count[3] = {0, 0, 0};
			for (const GameWorld::PlayerInfo& p : m_world.playerList()) {
				count[p.team == 1 || p.team == 2 ? p.team : 0] += 1;
			}
			addBot("", count[1] <= count[2] ? 1 : 2);
		}
	}
}

// 0x10088040: a strike for `team`'s side at `at` (the AI's height there:
// the terrain's, DAT_10139450 slot 0xc; here the strike's target). The
// other side learns of it (U + 1) x 12.5 s later (half of +0xc0's 25 s);
// both sides' escape points are forgotten.
void BotManager::noteStrike(int team, const physics::Vec3& at)
{
	if (team < 1 || team > 2) {
		return;
	}
	StrikeSpot& s = m_strikeSpots[team - 1];
	s.active = true;
	s.at = m_time;
	s.spot = at;
	s.enemyDelay = (random() + 1.0f) * (kStrikeLife * 0.5f);
	for (StrikeSpot& side : m_strikeSpots) {
		side.escape[0] = side.escape[1] = false;
	}
}

// 0x10088f20 (the squad manager's update, first): a spot 25 s old goes,
// and every member of every squad is given its goal again (0x100c1ca0 ->
// 0x100bf4e0(0): +0x35 0, +0x4e 1).
void BotManager::updateStrikeSpots()
{
	for (StrikeSpot& s : m_strikeSpots) {
		if (!s.active || !(s.at + kStrikeLife < m_time)) {
			continue;
		}
		s.active = false;
		for (auto& [key, squad] : m_squads) {
			for (SquadMember& m : squad.members) {
				m.goalSet = false;
				m.regoal = true;
			}
		}
	}
}

// 0x100880e0: whether `at` lies within 40 m (+0xb8) of the member's own
// side's strike spot, or of the other side's once that side's delay is
// over; `side` the spot's.
bool BotManager::strikeSpotHit(int team, const physics::Vec3& at, int& side) const
{
	if (team < 1 || team > 2) {
		return false;
	}
	const float r2 = kStrikeRadius * kStrikeRadius;
	const StrikeSpot& own = m_strikeSpots[team - 1];
	if (own.active && distance2(own.spot, at) < r2) {
		side = team;
		return true;
	}
	const StrikeSpot& other = m_strikeSpots[2 - team];
	if (other.active && distance2(other.spot, at) < r2 && m_time > other.at + other.enemyDelay) {
		side = 3 - team;
		return true;
	}
	return false;
}

// The member's map (0x100c1300's): the vehicle mesh for a bot aboard a
// vehicle, the infantry mesh on foot.
bool BotManager::onMeshFor(const GameWorld::PlayerInfo& self, const physics::Vec3& at) const
{
	const ai::NavMesh* mesh = self.vehicle && m_vehicleNavMesh ? m_vehicleNavMesh : m_navMesh;
	return !mesh || mesh->findPolygon(aiOf(at)) >= 0;
}

// 0x100bfcb0: a bot leaving a strike spot. Already leaving (+0x4f): it
// keeps at it. Else from the spot through the bot at 45 m (the radius + 5)
// a point on its map, tried five times, the way turned each time about
// the vertical (0x100a82c0) from the first: an angle a (0 at first) going
// 3a + (0.6 + 0.2 U) when a <= 0, -a - (0.6 + 0.2 U) otherwise. None: the
// point an earlier member of the bot's side found for its kind (soldier,
// its AI info's flag 0x200, or vehicle: 0x10088240), else nothing to do.
// Found: a waypoint goal there (0x1008aff0: reach 0, area 60), +0x35 and
// +0x4f set; a point it searched out is kept for its side (0x10088310).
bool BotManager::escapeStrike(Squad& squad, SquadMember& member, Bot& bot, const GameWorld::PlayerInfo& self, int side)
{
	if (member.escaping) {
		return true;
	}
	const StrikeSpot& spot = m_strikeSpots[side - 1];
	physics::Vec3 base = member.at - spot.spot;
	const float l = std::sqrt(base.x * base.x + base.y * base.y + base.z * base.z);
	if (l > 0) {
		base = base * (1.0f / l);
	}
	physics::Vec3 way = base;
	float turn = 0;
	bool found = false;
	physics::Vec3 point;
	for (int i = 0; i < 5; ++i) {
		point = spot.spot + way * (kStrikeRadius + 5.0f);
		if (onMeshFor(self, point)) {
			found = true;
			break;
		}
		const float step = 0.6f + 0.2f * random();
		turn = turn <= 0 ? 3 * turn + step : -turn - step;
		way = {std::cos(turn) * base.x - std::sin(turn) * base.z, 0, std::cos(turn) * base.z + std::sin(turn) * base.x};
	}
	const int kind = self.vehicle ? 1 : 0;
	StrikeSpot& own = m_strikeSpots[squad.team - 1];
	if (!found) {
		if (!own.escape[kind]) {
			return false;
		}
		point = own.escapeAt[kind];
	}
	member.goalSet = true;
	member.escaping = true;
	bot.squadGoal = true;
	bot.formation = ai::moveToGoal(0, aiOf(point));
	if (found) {
		own.escape[kind] = true;
		own.escapeAt[kind] = point;
	}
	return true;
}

// 0x1008a3b0: each human (the player's vt+0x84 false) alive and up on foot
// with a Mobile part whose position isn't on its map (the pathfinding
// courier's slot 0x30) goes into the list with the clock (0x10089e10, kept
// from the first time); one on the map, dead, down or gone leaves it.
void BotManager::trackHumans()
{
	std::map<int, float> kept;
	for (const GameWorld::PlayerInfo& p : m_players) {
		if (p.ai || !p.alive || p.manDown || p.vehicle) {
			continue;
		}
		if (!onMeshFor(p, p.position)) {
			const auto it = m_offMesh.find(p.id);
			kept[p.id] = it != m_offMesh.end() ? it->second : m_time;
		}
	}
	m_offMesh = std::move(kept);
}

// 0x100888d0: an enemy human of `team` off the mesh for over 60 s; its
// position.
bool BotManager::offMeshEnemy(int team, physics::Vec3& at) const
{
	for (const auto& [player, since] : m_offMesh) {
		const GameWorld::PlayerInfo* p = playerOf(player);
		if (p && p->team == (team == 1 ? 2 : 1) && since + 60 < m_time) {
			at = p->position;
			return true;
		}
	}
	return false;
}

// Who belongs: the game's squad, in the players' order (joining 0x100c3f10,
// leaving 0x100c3630 with 0x100c1de0's danger entries; the leader index
// from the game's leader 0x100c34d0).
void BotManager::syncSquadMembers(Squad& squad)
{
	std::vector<int> now;
	for (const GameWorld::PlayerInfo& p : m_players) {
		if (p.team == squad.team && p.squad == squad.id) {
			now.push_back(p.id);
		}
	}
	bool left = false;
	for (size_t i = 0; i < squad.members.size();) {
		const int id = squad.members[i].player;
		if (std::find(now.begin(), now.end(), id) == now.end()) {
			// 0x100c3630: its danger entries and its slot go (0x100c1de0,
			// 0x100f64a0), the slots are handed out again (0x100f57f0).
			squad.dangers.erase(std::remove_if(squad.dangers.begin(), squad.dangers.end(),
									[&](const SquadDanger& d) { return d.member == id; }),
				squad.dangers.end());
			if (ai::hasSlot(squad.formation, id)) {
				ai::clearSlot(squad.formation.slots[static_cast<size_t>(ai::slotOf(squad.formation, id))]);
			}
			if (auto bot = m_bots.find(id); bot != m_bots.end()) {
				bot->second.squadGoal = false;
			}
			squad.members.erase(squad.members.begin() + static_cast<std::ptrdiff_t>(i));
			left = true;
		} else {
			++i;
		}
	}
	for (int id : now) {
		if (memberOf(squad, id)) {
			continue;
		}
		// 0x100c3f10 -> 0x100bf550: a new member (+0x20 1, +0x50 900).
		SquadMember m;
		m.player = id;
		squad.members.push_back(m);
		// Without a human commander a squad led by a bot... (0x100c3f10: a
		// human joining with no human commander drops the order).
		const GameWorld::PlayerInfo* p = playerOf(id);
		if (p && !p->ai && !humanCommands(squad.team)) {
			squad.state = 0;
			squad.kind = 4;
		}
		if (inFormation(squad, squad.members.back())) {
			ai::takeSlot(squad.formation, id);
			ai::markMoved(squad.formation, true, m_time);
			squad.members.back().goalSet = false;
			squad.members.back().regoal = true;
		}
	}
	if (left) {
		// 0x100f57f0: every slot freed and handed out in member order (at
		// most the slot count less one members).
		for (ai::FormationSlot& slot : squad.formation.slots) {
			ai::clearSlot(slot);
		}
		size_t limit = std::min(squad.members.size(), squad.formation.slots.size() - 1);
		size_t next = 0;
		for (size_t i = 0; i < limit; ++i) {
			if (inFormation(squad, squad.members[i])) {
				squad.formation.slots[next].used = true;
				squad.formation.slots[next].member = squad.members[i].player;
				++next;
			}
		}
	}
	// 0x100c34d0: the first member the game makes leader.
	const int was = squad.leader;
	squad.leader = -1;
	for (size_t i = 0; i < squad.members.size(); ++i) {
		const GameWorld::PlayerInfo* p = playerOf(squad.members[i].player);
		if (p && p->squadLeader) {
			squad.leader = static_cast<int>(i);
			break;
		}
	}
	if (squad.leader >= 0 && was != -1 && was != squad.leader) {
		// A new leader: the formation lets go of a human's lead
		// (0x100f5320), the spot is the new leader's, everyone re-goals;
		// with more than one member the order is dropped unless a human
		// commands and the new leader is a bot.
		squad.formation.humanLeads = false;
		squad.formation.released = true;
		for (SquadMember& m : squad.members) {
			m.goalSet = false;
			m.regoal = true;
		}
		const GameWorld::PlayerInfo* leader = squadLeaderOf(squad);
		if (squad.members.size() > 1 && !(humanCommands(squad.team) && leader && m_bots.count(leader->id))) {
			squad.state = 0;
			squad.kind = 4;
		}
	}
}

// Whether a member stands in the formation (0x100bf950): alive and up, a
// soldier (its AI info's flag 0x200) with a Mobile part; not while an
// order under way has an area and the member is within 30 m (+0x50 900)
// of the order's spot for its map (0x100c1150).
bool BotManager::inFormation(const Squad& squad, const SquadMember& member) const
{
	const GameWorld::PlayerInfo* p = playerOf(member.player);
	if (!p || !p->alive || p->manDown || p->vehicle) {
		return false;
	}
	if (squad.state == 2 && squad.area >= 0 && member.placed && distance2(member.at, squad.orderAt) < 900.0f) {
		return false;
	}
	return true;
}

// 0x100c1eb0: an order reaching the squad (the game's event 0x10089480).
// A commander's (the SAI's go to the squads as the commander's) is taken
// when the squad has none and, without a human commander, no member is
// within 30 m of the old order's spot (0x100c1e50 over 0x100bf780): state
// 1 for 10 s (+0xa8). A squad leader's own (not from the commander) moves
// the formation's lead to a human leader (0x100f52d0) and re-goals.
void BotManager::takeOrder(Squad& squad, int kind, const physics::Vec3& at, bool fromCommander)
{
	if (!fromCommander) {
		const GameWorld::PlayerInfo* leader = squadLeaderOf(squad);
		if (!squad.members.empty() && leader && !m_bots.count(leader->id)) {
			squad.answerOrder = true;
			for (SquadMember& m : squad.members) {
				m.goalSet = false;
				m.regoal = true;
			}
			squad.formation.released = false;
			squad.formation.humanLeads = true;
			ai::markMoved(squad.formation, true, m_time);
		}
		return;
	}
	bool near = false;
	if (squad.area >= 0 && areaStatus(squad.area, squad.team) != ai::AreaStatus::Owned) {
		// 0x100bf780: within +0x50 (900) of the order's spot, on the
		// order's area, takeable by the side.
		for (const SquadMember& m : squad.members) {
			near = near || (squad.state == 2 && m.placed && distance2(squad.orderAt, m.at) < 900.0f
							   && m_settings.strategy.areas[static_cast<size_t>(squad.area)].takeable[static_cast<size_t>(squad.team)]);
		}
	}
	const bool commander = humanCommands(squad.team);
	if (((!commander && near) || squad.state != 0) && !commander) {
		return;
	}
	squad.state = 1;
	squad.takenUntil = m_time + 10;
	squad.takenAt = at;
	squad.takenFromCommander = fromCommander;
	squad.takenKind = kind;
}

// 0x100c21b0: the leader (or the member asking) accepts an order taken.
// Without a human commander an attack on an area the side holds is
// dropped; the order already under way is kept. An area at the spot whose
// radius (+ aiSettings.setIgnoreCommanderDistance, IAISettings slot 0x10)
// holds it, while it isn't the side's, takes its position; else a spot
// within 30 m on the mesh (0x100c14a0: the spot itself, then 8 tries
// spot + U(-30, 30) on x and z). None: a bot says "negative" to the
// commander (type 7, channel 4, 2 s), the order is cancelled (IBFSquad
// slots 0x3c / 0x40 / 0x44 by kind) and everyone re-goals. Accepted: a
// bot leader says "roger_that" to a human commander (6, channel 4, 2.5 s)
// and the order to the squad (attack_position 16, defend 15, move 17 on
// channel 3, 2 s; IBFSquad 0x1c / 0x24 / 0x20 mark it and 0x30 / 0x34 /
// 0x38 take it for the leader); everyone re-goals.
bool BotManager::acceptOrder(Squad& squad, int by)
{
	const bool commander = humanCommands(squad.team);
	physics::Vec3 centre;
	float radius = 0;
	const int area = areaAt(squad.takenAt, squad.team, &centre, &radius);
	if (!commander && area >= 0 && squad.takenKind == 0 && areaStatus(area, squad.team) == ai::AreaStatus::Owned) {
		squad.state = 0;
		squad.kind = 4;
		return false;
	}
	if (!commander && squad.kind == squad.takenKind && squad.area == area && squad.state == 2) {
		return true;
	}
	bool accepted = false;
	squad.area = -1;
	if (area >= 0) {
		const float reach = radius + m_settings.ignoreCommanderDistance;
		const bool inside = horizontalDistance2(squad.takenAt, centre) <= radius * radius;
		if (inside || (std::sqrt(distance2(squad.takenAt, centre)) < reach && areaStatus(area, squad.team) != ai::AreaStatus::Owned)) {
			squad.takenAt = centre;
			squad.orderAt = centre;
			squad.area = area;
			accepted = true;
		}
	}
	Bot* leaderBot = nullptr;
	if (const GameWorld::PlayerInfo* leader = squadLeaderOf(squad)) {
		const auto it = m_bots.find(leader->id);
		leaderBot = it != m_bots.end() ? &it->second : nullptr;
	}
	if (squad.area < 0) {
		physics::Vec3 spot = squad.takenAt;
		bool found = reachable(spot);
		for (int i = 0; i < 8 && !found; ++i) {
			const physics::Vec3 tryAt {squad.takenAt.x - 30 + random() * 60, squad.takenAt.y, squad.takenAt.z - 30 + random() * 60};
			found = reachable(tryAt);
			spot = found ? tryAt : spot;
		}
		if (!found) {
			auto byBot = m_bots.find(by);
			if (byBot != m_bots.end()) {
				queueRadio(byBot->second, 7, 4, 2.0f);
				squad.state = 0;
				squad.kind = 4;
				for (SquadMember& m : squad.members) {
					m.goalSet = false;
					m.regoal = true;
				}
			}
			return false;
		}
		squad.takenAt = spot;
		squad.orderAt = spot;
		accepted = true;
	}
	if (accepted) {
		squad.state = 2;
		squad.kind = squad.takenKind;
		squad.fromCommander = squad.takenFromCommander;
	}
	if (leaderBot) {
		if (commander) {
			queueRadio(*leaderBot, 6, 4, 2.5f);
		}
		static const int orderVoice[3] = {16, 15, 17};
		if (squad.kind >= 0 && squad.kind <= 2) {
			queueRadio(*leaderBot, orderVoice[squad.kind], 3, 2.0f);
		}
	}
	for (SquadMember& m : squad.members) {
		m.goalSet = false;
		m.regoal = true;
	}
	return true;
}

// The SAI letting go of an area (0x100893c0): its squads ordered there with
// no member near the spot (0x100c1e50) drop the order (0x100c0f00).
void BotManager::releaseSquad(int team, int id, int area)
{
	const auto it = m_squads.find({team, id});
	if (it == m_squads.end() || it->second.area != area) {
		return;
	}
	Squad& squad = it->second;
	for (const SquadMember& m : squad.members) {
		if (squad.state == 2 && m.placed && distance2(squad.orderAt, m.at) < 900.0f && area >= 0
			&& areaStatus(area, team) != ai::AreaStatus::Owned) {
			return;
		}
	}
	squad.state = 0;
	squad.kind = 4;
}

// A member's slot point (0x100f5c70): the squad's spot plus its slot's
// offset turned to the way; a slot off the mesh or on cost class 4
// (0x100c0ff0, the pathfinder's slot 0x58) takes its trail point, else the
// first free one it can reach (0x100f5950), else the order's spot (an
// order under way, 0x100c1120) or the leader's slot member's position.
bool BotManager::slotPoint(Squad& squad, int player, physics::Vec3& at)
{
	ai::Formation& f = squad.formation;
	if (squad.at.x * squad.at.x + squad.at.y * squad.at.y + squad.at.z * squad.at.z < 0.001f) {
		const GameWorld::PlayerInfo* leader = squadLeaderOf(squad);
		if (!leader) {
			return false;
		}
		at = leader->position;
		return true;
	}
	const int k = ai::slotOf(f, player);
	if (k < 0) {
		return true;
	}
	ai::FormationSlot& slot = f.slots[static_cast<size_t>(k)];
	at = squad.at + physicsOf(ai::turnOffset(f, slot.offset));
	if (k == 0) {
		return true;
	}
	bool bad = false;
	if (m_navMesh) {
		const int32_t polygon = m_navMesh->findPolygon(aiOf(at));
		bad = polygon < 0 || m_navMesh->polygons[static_cast<size_t>(polygon)].costClass == 4;
	}
	if (!bad) {
		slot.crumb = -1;
		return true;
	}
	if (slot.crumb != -1) {
		at = physicsOf(f.crumbs[static_cast<size_t>(slot.crumb)].at);
		return true;
	}
	for (size_t c = 0; c < f.crumbs.size(); ++c) {
		bool taken = false;
		for (const ai::FormationSlot& s : f.slots) {
			taken = taken || s.crumb == static_cast<int>(c);
		}
		if (f.crumbs[c].valid && !taken && reachable(physicsOf(f.crumbs[c].at))) {
			slot.crumb = static_cast<int>(c);
			at = physicsOf(f.crumbs[c].at);
			return true;
		}
	}
	if (squad.state == 2) {
		if (squad.kind == 4) {
			return false;
		}
		at = squad.orderAt;
		return true;
	}
	if (const SquadMember* first = f.slots[0].used ? memberOf(squad, f.slots[0].member) : nullptr; first && first->placed) {
		at = first->at;
		return true;
	}
	return false;
}

// 0x100c0bb0: a member's look. Its position (0x100bf670); every 9 s a goal
// unchanged since the last look is given again when the bot's goal object
// says it is reached (urgency below 0.5) or the bot stands still over 4 m
// from it (0x100c0cbc). A taken order goes to the leader alive and up, or
// to this member, to accept (0x100c21b0; the game's IBFSquad slot 0x28
// takes it, 0x2c refuses). Then the member out of the formation heads for
// the order (0x100c0a90), the one in it for its slot (0x100c0890).
// Before the order, a bot in a strike spot leaves it (0x100880e0,
// 0x100bfcb0: escapeStrike).
void BotManager::updateMember(Squad& squad, SquadMember& member)
{
	const GameWorld::PlayerInfo* p = playerOf(member.player);
	if (!p) {
		return;
	}
	const physics::Vec3 was = member.at;
	member.moving = false;
	member.placed = p->alive;
	if (member.placed) {
		member.at = p->position; // its AI object's root (0x100a8c70)
		member.moving = distance2(member.at, was) > 0.01f;
	}
	auto botIt = m_bots.find(member.player);
	Bot* bot = botIt != m_bots.end() && p->ai ? &botIt->second : nullptr;
	// +0x4d follows the bot's Idle plans: set as one begins (its Idle
	// statement 0x100d2830), cleared as one ends (0x100d28a0); a member
	// starts with it clear (0x100bf550) whatever its bot runs.
	if (bot) {
		if (!member.idleSynced) {
			member.idleSynced = true;
		} else if (bot->idleStarts != member.idleStartsSeen || bot->idleEnds != member.idleEndsSeen) {
			member.fresh = bot->pattern.rfind("BBPIdle", 0) == 0;
		}
		member.idleStartsSeen = bot->idleStarts;
		member.idleEndsSeen = bot->idleEnds;
	}
	if (m_time > member.checkedAt + 9) {
		if (distance2(member.lastGoal, member.goal) <= 0.01f) {
			if (bot && member.placed) {
				const float u = goalObjectUrgency(*bot, *p);
				if (u < 0.5f || (!member.moving && distance2(member.at, member.goal) > 16.0f)) {
					member.goalSet = false;
					member.regoal = true;
				}
			}
		} else {
			member.goalSet = false;
			member.regoal = true;
			member.lastGoal = member.goal;
		}
		member.checkedAt = m_time;
	}
	// A bot alive and up (0x100bfa70) standing in a strike spot
	// (0x100880e0) leaves it (0x100bfcb0) and does nothing else at this
	// look; out of every spot it is no longer leaving (+0x4f 0).
	if (bot && member.placed) {
		int side = 0;
		if (!p->manDown && strikeSpotHit(squad.team, member.at, side)) {
			if (escapeStrike(squad, member, *bot, *p, side)) {
				return;
			}
		} else {
			member.escaping = false;
		}
	}
	if (squad.state == 1) {
		const GameWorld::PlayerInfo* leader = squadLeaderOf(squad);
		int by = -1;
		if (leader && leader->alive && !leader->manDown) {
			by = leader->id;
		} else if (p->alive && !p->manDown) {
			by = p->id;
		}
		if (by >= 0) {
			const bool accepted = acceptOrder(squad, by);
			if (leader && leader->ai && m_world.squadOrder(squad.team, squad.id)) {
				m_world.acceptOrder(leader->id, accepted);
			}
		}
	}
	// 0x100c0890's first part, for every member with a slot, humans too:
	// the slot's squared distance over the member's top speed (+0x20; the
	// Mobile part's 0x100a4350, a soldier's 5), read by the lag test.
	physics::Vec3 slot;
	const bool slotted = ai::hasSlot(squad.formation, member.player) && slotPoint(squad, member.player, slot);
	if (slotted) {
		member.eta = distance2(slot, member.at) / 5.0f;
	}
	if (!bot || !p->alive) {
		return;
	}
	bot->walkFactor = 1;
	const bool commander = humanCommands(squad.team);
	if (!ai::hasSlot(squad.formation, member.player)) {
		// 0x100c0a90: the order's goal (0x100c0130) once the member isn't
		// given one, over sqrt(8) m from the spot of an order under way, or
		// with a human commander.
		bot->squadGoal = false;
		if (member.goalSet) {
			return;
		}
		if ((squad.state == 2 && distance2(member.at, squad.orderAt) > 8.0f) || commander) {
			member.goalSet = true;
			member.goal = squad.orderAt;
			bot->orderRegoal = true;
		}
		return;
	}
	// 0x100c0890.
	if (!slotted) {
		return;
	}
	if (distance2(slot, member.at) <= 10.0f) {
		if (!member.atSlot) {
			member.goalSet = false;
		}
		member.atSlot = true;
	} else {
		member.atSlot = false;
	}
	if (squad.formation.slots[0].member == member.player) {
		bot->squadGoal = false;
		if (squad.state == 2 && squad.formation.together) {
			// The squad's speed (0x100f4740) over the bot's own top speed.
			bot->walkFactor = squad.formation.speed / 5.0f;
			if (!member.goalSet && distance2(member.at, squad.orderAt) > 8.0f) {
				member.goalSet = true;
				member.goal = squad.orderAt;
				bot->orderRegoal = true;
			}
		} else if (!member.goalSet && commander) {
			member.goalSet = true;
			member.goal = squad.orderAt;
			bot->orderRegoal = true;
		}
		return;
	}
	if (member.fresh || member.regoal) {
		// 0x100bff90: a waypoint goal at the slot (reach 0, area 60),
		// at every look while the bot idles (+0x4d: it reached the last
		// slot point, so it follows the slot as it moves), else when
		// re-goaled (+0x4e, cleared here).
		member.regoal = false;
		member.goalSet = true;
		member.goal = slot;
		bot->squadGoal = true;
		bot->formation = ai::moveToGoal(0, aiOf(slot));
	}
}

// 0x100f6020: the trail starts again from slot 0's slot point (+0x4c,
// +0x78; with slot 0 used and the squad's spot known, 0x100c10c0), its
// points and the slots' marks dropped.
void BotManager::restartTrail(Squad& squad)
{
	ai::Formation& f = squad.formation;
	physics::Vec3 c;
	if (f.slots[0].used && (squad.at.x != 0 || squad.at.y != 0 || squad.at.z != 0) && slotPoint(squad, f.slots[0].member, c)) {
		f.centre = aiOf(c);
	}
	f.lastCrumb = f.centre;
	for (auto& crumb : f.crumbs) {
		crumb.valid = false;
	}
	for (auto& slot : f.slots) {
		slot.crumb = -1;
	}
}

// The formation's update (0x100f6dc0): the leader's slot point is the
// spot (+0x4c), the moved mark (0x100f4c70), the speed (0x100f5120), the
// leader's slot (0x100f60a0), human members lagging (0x100f4f30), the way
// (0x100f51f0), the trail (0x100f4d30), the trail points handed out by
// distance (0x100f6c10); every sixth tick the offsets are laid out again
// (0x100f58c0), the shape set back to 0 on the third.
void BotManager::updateFormation(Squad& squad)
{
	ai::Formation& f = squad.formation;
	if (f.slots[0].used && (squad.at.x != 0 || squad.at.y != 0 || squad.at.z != 0)) {
		physics::Vec3 c;
		if (slotPoint(squad, f.slots[0].member, c)) {
			f.centre = aiOf(c);
		}
	}
	ai::markMoved(f, false, m_time);
	// 0x100f5120: the slowest member's top speed (the Mobile part's
	// 0x100a4350; the soldiers' 5), x 0.9 with more than one; 5 for none.
	int used = 0;
	f.speed = 3.4028235e38f;
	for (const ai::FormationSlot& s : f.slots) {
		if (s.used) {
			++used;
			f.speed = std::min(f.speed, 5.0f);
		}
	}
	f.speed = used == 0 ? 5.0f : (used > 1 ? f.speed * 0.9f : f.speed);
	// 0x100f60a0: the game's leader and slot 0.
	//   - Not led by a human's order (+0x58 clear), the leader in a slot
	//     past 0 swaps into slot 0 unless it stands 50 m or more from the
	//     spot while not released (+0x59); slot 0 is used after, the slot
	//     it left takes slot 0's used flag and member, both lag times are
	//     cleared, the leader and the member moved out are re-goaled
	//     (0x100bf4e0(0): +0x35 0, +0x4e 1) and the trail starts again
	//     (0x100f6020).
	//   - Led by a human's order (+0x58), the leader in slot 0 swaps with
	//     the last used slot (both lags cleared, the trail started again,
	//     both re-goaled): a member leads the formation; in a slot past 0
	//     it stays there.
	//   - A leader with no slot: a human (bot) leader's slot 0 held by a
	//     bot (human) goes to the first member past it of the leader's
	//     kind; lags cleared, the member now in slot 0 re-goaled, the
	//     trail started again.
	const GameWorld::PlayerInfo* leader = squadLeaderOf(squad);
	if (leader) {
		const auto clearLag = [](ai::FormationSlot& s) {
			s.lagging = false;
			s.lagUntil = 0;
		};
		const auto regoal = [&](int player) {
			if (SquadMember* m = memberOf(squad, player)) {
				m->goalSet = false;
				m->regoal = true;
			}
		};
		int k = -1;
		for (size_t i = 0; i < f.slots.size() && k < 0; ++i) {
			k = f.slots[i].used && f.slots[i].member == leader->id ? static_cast<int>(i) : -1;
		}
		if (!f.humanLeads && k > 0) {
			const SquadMember* m = memberOf(squad, leader->id);
			const physics::Vec3 at = m ? m->at : physics::Vec3 {}; // 0x100bf930: +0x40, placed or not
			if (distance2(at, squad.at) < 2500.0f || f.released) {
				f.released = false;
				ai::FormationSlot& first = f.slots[0];
				ai::FormationSlot& left = f.slots[static_cast<size_t>(k)];
				const bool firstUsed = first.used;
				const int firstMember = first.member;
				first.used = true;
				first.member = leader->id;
				clearLag(first);
				regoal(leader->id);
				left.used = firstUsed;
				left.member = firstMember;
				clearLag(left);
				regoal(firstMember);
				restartTrail(squad);
			}
		} else if (f.humanLeads && k == 0) {
			size_t last = f.slots.size() - 1;
			while (last > 0 && !f.slots[last].used) {
				--last;
			}
			if (last > 0) {
				std::swap(f.slots[0].member, f.slots[last].member);
				clearLag(f.slots[0]);
				clearLag(f.slots[last]);
				restartTrail(squad);
				regoal(f.slots[0].member);
				regoal(f.slots[last].member);
			}
		} else if (k < 0 && f.slots[0].used) {
			const bool leaderBot = m_bots.count(leader->id) != 0;
			const bool firstBot = m_bots.count(f.slots[0].member) != 0;
			if (firstBot != leaderBot) {
				for (size_t i = 1; i < f.slots.size(); ++i) {
					if (f.slots[i].used && (m_bots.count(f.slots[i].member) != 0) == leaderBot) {
						std::swap(f.slots[0].member, f.slots[i].member);
						clearLag(f.slots[0]);
						clearLag(f.slots[i]);
						regoal(f.slots[0].member);
						restartTrail(squad);
						break;
					}
				}
			}
		}
	}
	// 0x100f4f30: human members lagging. A slot past the leader's is
	// cleared (+0x18, +0x1c) for a bot, for the leader a human leads by,
	// for a member not placed, whose slot time (+0x20, 0x1004f4e0: the
	// squared distance over its speed) is outside (150, 1500), or who is no
	// farther from the order's spot (+0x2c) than the squad's spot (+0x8c).
	// Else the human lags: the first time for 10 + 10 U s, the formation
	// not together (+0x28) while it lasts; once over, the slot isn't
	// lagging but keeps its time, so it lags again only after a clear.
	f.together = true;
	const GameWorld::PlayerInfo* gameLeader = squadLeaderOf(squad);
	for (size_t i = 1; i < f.slots.size(); ++i) {
		ai::FormationSlot& slot = f.slots[i];
		if (!slot.used) {
			continue;
		}
		const SquadMember* m = memberOf(squad, slot.member);
		const bool leadingHuman = f.humanLeads && gameLeader && gameLeader->id == slot.member;
		bool clear = leadingHuman || m_bots.count(slot.member) || !m || !m->placed || m->eta >= 1500.0f || m->eta <= 150.0f;
		clear = clear || distance2(squad.orderAt, m->at) <= distance2(squad.orderAt, squad.at);
		if (clear) {
			slot.lagging = false;
			slot.lagUntil = 0;
		} else if (slot.lagging) {
			if (m_time <= slot.lagUntil) {
				f.together = false;
			} else {
				slot.lagging = false;
			}
		} else if (slot.lagUntil < 0.01f) {
			f.together = false;
			slot.lagging = true;
			slot.lagUntil = m_time + random() * 10.0f + 10.0f;
		}
	}
	// 0x100f51f0: the way the leader goes, over 0.5 m/s.
	if (f.slots[0].used) {
		if (const GameWorld::PlayerInfo* first = playerOf(f.slots[0].member)) {
			const physics::Vec3 v = first->velocity;
			if (v.x * v.x + v.y * v.y + v.z * v.z > 0.25f) {
				const float flat = std::sqrt(v.x * v.x + v.z * v.z);
				if (flat > 0) {
					f.dirX = v.x / flat;
					f.dirZ = v.z / flat;
				}
			}
		}
	}
	ai::dropCrumb(f);
	// 0x100f6c10: the trail points in use, nearest the spot first.
	std::vector<std::pair<float, size_t>> users;
	for (size_t i = 0; i < f.slots.size(); ++i) {
		if (f.slots[i].crumb != -1 && f.slots[i].used) {
			const SquadMember* m = memberOf(squad, f.slots[i].member);
			users.push_back({m ? distance2(m->at, physicsOf(f.centre)) : 0, i});
		}
	}
	if (!users.empty()) {
		std::stable_sort(users.begin(), users.end());
		for (auto& s : f.slots) {
			s.crumb = -1;
		}
		for (size_t rank = 0; rank < users.size(); ++rank) {
			f.slots[users[rank].second].crumb = static_cast<int>(rank);
		}
	}
	if (++f.ticks > 5) {
		f.ticks = 0;
		ai::layOutFormation(f, [](int member) { return member >= 0 ? kSoldierAiRadius : 0.0f; });
	} else if (f.ticks == 3) {
		f.shape = 0;
	}
}

// The mode (0x100c4010 over 0x100c1940): bounding (1) with two or more of
// the squad's bots alive with a target on foot (the target's AI flags 2,
// 4 and 0x10 clear) within 90 m of the spot; flanking (2) for an order
// from the commander under way whose spot is 50..173 m off and whose area
// is the enemy's; else every bot may advance (vt+0x128 1).
//   Bounding (0x100c3af0, every 4 s): all may advance; then by phase the
// first half (phase 0, next look in 1.5 s), nobody (1, U*2 + 1 s), the
// second half (2, 1.5 s), nobody (3): of those, a bot facing its target
// (0.85) more than 40 m off holds (vt+0x128 0).
//   Flanking (0x100c3390): all may advance; once, with the formation's
// slot 0 used and an area, 65 % of the time the area's two flank spots
// (0x1005e820) reachable for slot 0's member would mark the squad flanking
// (+0xc0) and re-goal that member; the area never has flank spots (below).
// The bots' +400 is read by the sensing agent's look-around (0x100d7840,
// vt+0x12c): a bot told to hold looks along its walk, not about it
// (BotsLook.cpp).
void BotManager::squadMode(Squad& squad)
{
	std::vector<std::pair<Bot*, const GameWorld::PlayerInfo*>> fighting;
	for (const SquadMember& m : squad.members) {
		const auto it = m_bots.find(m.player);
		const GameWorld::PlayerInfo* p = playerOf(m.player);
		if (it == m_bots.end() || !p || !p->alive) {
			continue;
		}
		const GameWorld::PlayerInfo* target = it->second.target >= 0 ? playerOf(it->second.target) : nullptr;
		if (target && !target->vehicle && m.placed && distance2(m.at, squad.at) < 8100.0f) {
			fighting.push_back({&it->second, p});
		}
	}
	int mode = 0;
	if (fighting.size() > 1) {
		mode = 1;
	} else if (squad.state == 2) {
		const float d2 = distance2(squad.orderAt, squad.at);
		if (d2 < 30000.0f && d2 > 2500.0f && squad.fromCommander && squad.area >= 0
			&& areaStatus(squad.area, squad.team) == ai::AreaStatus::Hostile) {
			if (squad.mode != 2) {
				squad.flanking = false;
				squad.flankTried = false;
			}
			mode = 2;
		}
	}
	squad.mode = mode;
	for (const SquadMember& m : squad.members) {
		if (auto it = m_bots.find(m.player); it != m_bots.end()) {
			it->second.mayAdvance = true;
		}
	}
	if (mode == 1) {
		if (squad.boundNext >= m_time) {
			return;
		}
		squad.boundNext = m_time + 4;
		size_t from = 0, to = 0;
		const size_t n = fighting.size();
		switch (squad.boundPhase) {
		case 0: to = n / 2; squad.boundNext = m_time + 1.5f; squad.boundPhase = 1; break;
		case 1: squad.boundNext = m_time + random() * 2 + 1; squad.boundPhase = 2; break;
		case 2: from = n / 2; to = n; squad.boundNext = m_time + 1.5f; squad.boundPhase = 3; break;
		case 3: squad.boundNext = m_time + random() * 2 + 1; squad.boundPhase = 0; break;
		}
		for (size_t i = from; i < to; ++i) {
			Bot& bot = *fighting[i].first;
			const GameWorld::PlayerInfo& self = *fighting[i].second;
			const GameWorld::PlayerInfo* target = playerOf(bot.target);
			if (!target) {
				continue;
			}
			physics::Vec3 d = target->position - self.position;
			const float length = std::sqrt(physics::lengthSquared(d));
			if (length <= 0) {
				continue;
			}
			d = d * (1 / length);
			const float yaw = self.yaw * 3.14159265f / 180;
			const float facing = d.x * std::sin(yaw) + d.z * std::cos(yaw);
			if (facing > 0.85f && length * length > 1600.0f) {
				bot.mayAdvance = false;
			}
		}
	} else if (mode == 2 && !squad.flankTried && squad.state == 2 && squad.formation.slots[0].used && squad.area >= 0) {
		squad.flankTried = true;
		// The draw is made (0x10046770 against 0.65), but the flank spots
		// are never found: 0x1005e820 takes the record of the area's list
		// (+0x1c4 / +0x1c8: 0x20-byte nodes, a point at +8 and a second at
		// +0x14) nearest the squad's spot, and nothing in AIDLL 1.51 adds to
		// that list. Its only writers are the area's constructors
		// (0x1005ffe0, 0x10062e10: an empty head) and its destructor; its
		// node maker 0x1005ebc0 is reached only through the list copy
		// 0x1005f0d0 / 0x1005ff40 / 0x10061240, whose sources are such lists;
		// its readers are 0x1005e820 and the SAI debug printer
		// (0x1002eff0 via 0x10061440, which draws each pair). So +0xc0 is
		// never set, no member is re-goaled, and +0xc8 stays at zero.
		(void)random();
	}
}

// The squad's radio. 0x100c30f0: with a human commander, no member in the
// order's area (0x100c1820) and an attack, a move or none, a bot leader
// alive and up within 15 m of the order's spot or with its area held asks
// for a new order every 20 s (IBFSquad slot 0x48; "req_new_order" 14 to
// the commander, channel 4, 1 s).
// 0x100c2ee0: a human leader's "followme" (12) heard by the first bot
// member alive releases the formation (0x100f5320), that bot says
// roger_that (6, channel 3, 2 s) and everyone re-goals; a bot leader under
// a human commander answers the commander's commend (18) with thankyou
// (8), rebuke (19) and followorders (20) with sorry (9), divide (21) with
// negative (7): channel 4, 2 s.
void BotManager::squadRadio(Squad& squad)
{
	const bool commander = humanCommands(squad.team);
	const GameWorld::PlayerInfo* leader = squadLeaderOf(squad);
	Bot* leaderBot = nullptr;
	if (leader) {
		const auto it = m_bots.find(leader->id);
		leaderBot = it != m_bots.end() ? &it->second : nullptr;
	}
	bool inArea = false;
	if (squad.area >= 0 && areaStatus(squad.area, squad.team) != ai::AreaStatus::Owned) {
		physics::Vec3 at;
		float radius = 0;
		int owner = 0;
		if (areaPosition(squad.area, at, radius, owner)) {
			for (const SquadMember& m : squad.members) {
				const GameWorld::PlayerInfo* p = playerOf(m.player);
				inArea = inArea || (p && p->alive && !p->manDown && m.placed && distance2(m.at, at) < radius * radius);
			}
		}
	}
	bool asked = false;
	if (commander && !inArea && (squad.kind == 0 || squad.kind == 2 || squad.kind == 4) && leaderBot && leader->alive
		&& !leader->manDown) {
		const bool close = distance2(leader->position, squad.orderAt) < 225.0f
			|| (squad.area >= 0 && areaStatus(squad.area, squad.team) == ai::AreaStatus::Owned);
		if (close) {
			if (squad.requestedAt + 20 < m_time) {
				queueRadio(*leaderBot, 14, 4, 1.0f);
				squad.requestedAt = m_time;
			}
			asked = true;
		}
	}
	if (!asked) {
		squad.requestedAt = m_time;
	}
	if (!leader) {
		return;
	}
	if (!leaderBot && squad.formation.humanLeads) {
		for (const SquadMember& m : squad.members) {
			const auto it = m_bots.find(m.player);
			const GameWorld::PlayerInfo* p = playerOf(m.player);
			if (it == m_bots.end() || !p || !p->alive) {
				continue;
			}
			const Bot::Radio* followMe = nullptr;
			for (const Bot::Radio& r : it->second.radio) {
				followMe = !followMe && r.type == 12 ? &r : followMe;
			}
			if (followMe && followMe->sender == leader->id) {
				squad.formation.humanLeads = false;
				squad.formation.released = true;
				ai::markMoved(squad.formation, true, m_time);
				queueRadio(it->second, 6, 3, 2.0f);
				for (SquadMember& each : squad.members) {
					each.goalSet = false;
					each.regoal = true;
				}
			}
			break;
		}
	}
	if (leaderBot && commander) {
		static const std::pair<int, int> answers[] = {{18, 8}, {19, 9}, {20, 9}, {21, 7}};
		for (const auto& [heard, answer] : answers) {
			if (answerRadio(*leaderBot, heard)) {
				queueRadio(*leaderBot, answer, 4, 2.0f);
			}
		}
	}
}

void BotManager::updateSquad(Squad& squad)
{
	syncSquadMembers(squad);
	const bool commander = humanCommands(squad.team);
	// The world's order changing is the game's order event (0x10089480).
	const GameWorld::SquadOrder* order = m_world.squadOrder(squad.team, squad.id);
	const GameWorld::OrderType type = order ? order->type : GameWorld::OrderType::None;
	const physics::Vec3 orderAt = order ? order->position : physics::Vec3 {};
	if (order && (type != squad.seenType || distance2(orderAt, squad.seenAt) > 1.0f)) {
		const int kind = kindOf(type);
		if (kind != 4) {
			takeOrder(squad, kind, orderAt, true);
		}
	}
	squad.seenType = type;
	squad.seenAt = orderAt;
	if (squad.state == 1 && m_time > squad.takenUntil) {
		// +0xa8: the order taken lapses unanswered (the squad manager's
		// 0x10088f20 re-goals; the rewrite lets it go).
		squad.state = 0;
	}
	if (squad.state == 2 && !commander && squad.area >= 0 && areaStatus(squad.area, squad.team) == ai::AreaStatus::Owned) {
		if (squad.kind == 0) {
			squad.state = 0;
			squad.kind = 4;
		}
		// A defence ends while the enemy holds an area whose point can
		// change hands (0x1009c660: +0x70 + 4 side 0 and IAIGame slot 0x24's
		// flag, BF2142.exe 0x00895d90: not unableToChangeTeam).
		if (squad.kind == 1) {
			for (const GameWorld::ControlPointInfo& cp : m_world.controlPointList()) {
				if (cp.team == 3 - squad.team && cp.capturable && areaOfControlPoint(cp.id) >= 0) {
					squad.state = 0;
					squad.kind = 4;
					break;
				}
			}
		}
	}
	if (squad.members.empty()) {
		return;
	}
	// 0x100bfb60: places in the formation given up and taken.
	for (SquadMember& m : squad.members) {
		const bool in = inFormation(squad, m);
		const bool has = ai::hasSlot(squad.formation, m.player);
		if (has && !in) {
			ai::Formation& f = squad.formation;
			const size_t hole = static_cast<size_t>(ai::slotOf(f, m.player));
			ai::clearSlot(f.slots[hole]);
			// 0x100f64a0: the last used slot moves into the hole; the
			// leader's slot takes the bot member nearest the order's spot
			// that isn't the leader.
			size_t last = f.slots.size() - 1;
			while (last > hole && !f.slots[last].used) {
				--last;
			}
			if (last > hole && hole + 1 < f.slots.size() && f.slots[hole + 1].used) {
				size_t from = last;
				if (hole == 0 && last >= 2) {
					float best = 3.4028235e38f;
					for (size_t j = last; j > 0; --j) {
						const SquadMember* c = f.slots[j].used ? memberOf(squad, f.slots[j].member) : nullptr;
						const GameWorld::PlayerInfo* cp = c ? playerOf(c->player) : nullptr;
						if (c && cp && !cp->squadLeader && m_bots.count(c->player) && c->placed) {
							const float d = distance2(c->at, squad.orderAt);
							if (d < best) {
								best = d;
								from = j;
							}
						}
					}
				}
				// The move copies the used flag, member, trail point and lag
				// (+0, +4, +0x14, +0x18, +0x1c), never the parent (+0x20);
				// the offset (+8..+0x10) only into the leader's hole, so a
				// hole past it keeps its own place until the next layout.
				const auto moveSlot = [](ai::FormationSlot& to, const ai::FormationSlot& from, bool offset) {
					to.used = from.used;
					to.member = from.member;
					to.crumb = from.crumb;
					to.lagging = from.lagging;
					to.lagUntil = from.lagUntil;
					if (offset) {
						to.offset = from.offset;
					}
				};
				moveSlot(f.slots[hole], f.slots[from], hole == 0 && last >= 2);
				ai::clearSlot(f.slots[from]);
				if (SquadMember* moved = memberOf(squad, f.slots[hole].member)) {
					moved->goalSet = false;
					moved->regoal = true;
				}
				// The leader's hole filled from the middle: that slot's hole
				// takes the last used one in turn (0x100f64a0's second move),
				// so the used slots stay packed from slot 0.
				if (hole == 0 && from != last && from + 1 < f.slots.size() && f.slots[from + 1].used) {
					size_t tail = f.slots.size() - 1;
					while (tail > from && !f.slots[tail].used) {
						--tail;
					}
					if (tail > from) {
						moveSlot(f.slots[from], f.slots[tail], true);
						ai::clearSlot(f.slots[tail]);
						if (SquadMember* moved = memberOf(squad, f.slots[from].member)) {
							moved->goalSet = false;
							moved->regoal = true;
						}
					}
				}
			}
			if (hole == 0) {
				restartTrail(squad);
			}
			ai::markMoved(f, true, m_time);
			m.goalSet = false;
			if (auto bot = m_bots.find(m.player); bot != m_bots.end()) {
				bot->second.squadGoal = false;
			}
		} else if (!has && in) {
			ai::takeSlot(squad.formation, m.player);
			ai::markMoved(squad.formation, true, m_time);
			m.goalSet = false;
			m.regoal = true;
		}
	}
	// 0x100f69a0: each slot past the leader's to the member nearest it.
	ai::Formation& f = squad.formation;
	for (size_t i = 1; i < f.slots.size(); ++i) {
		physics::Vec3 point;
		if (!f.slots[i].used || !slotPoint(squad, f.slots[i].member, point)) {
			continue;
		}
		size_t nearest = i;
		float best = 3.4028235e38f;
		for (size_t j = 1; j < f.slots.size(); ++j) {
			const SquadMember* m = f.slots[j].used ? memberOf(squad, f.slots[j].member) : nullptr;
			if (m && m->placed && distance2(m->at, point) < best) {
				best = distance2(m->at, point);
				nearest = j;
			}
		}
		if (nearest != i) {
			std::swap(f.slots[i].member, f.slots[nearest].member);
			std::swap(f.slots[i].lagging, f.slots[nearest].lagging);
			std::swap(f.slots[i].lagUntil, f.slots[nearest].lagUntil);
		}
	}
	for (SquadMember& m : squad.members) {
		updateMember(squad, m);
	}
	squadMode(squad);
	updateFormation(squad);
	// 0x100c32b0: the spot is slot 0's member's (or the leader's), or the
	// first member alive's.
	{
		const SquadMember* first = f.slots[0].used ? memberOf(squad, f.slots[0].member) : nullptr;
		if (!first && squad.leader >= 0) {
			first = &squad.members[static_cast<size_t>(squad.leader)];
		}
		const GameWorld::PlayerInfo* fp = first ? playerOf(first->player) : nullptr;
		if (!fp || !fp->alive) {
			first = nullptr;
			for (const SquadMember& m : squad.members) {
				const GameWorld::PlayerInfo* p = playerOf(m.player);
				if (p && p->alive) {
					first = &m;
					break;
				}
			}
		}
		if (first && first->placed) {
			squad.at = first->at;
		}
	}
	// 0x100c1b60: a danger entry goes when its member is gone, its time is
	// up or the member moved 5 m from it; the last two re-goal everyone and
	// mark the formation moved. (No writer of the list was found among
	// AIDLL's functions: it stays empty in the shipped game.)
	for (auto it = squad.dangers.begin(); it != squad.dangers.end();) {
		const SquadMember* m = memberOf(squad, it->member);
		if (!m || !m->placed) {
			it = squad.dangers.erase(it);
		} else if (it->until < m_time || distance2(it->at, m->at) > 25.0f) {
			it = squad.dangers.erase(it);
			for (SquadMember& each : squad.members) {
				each.goalSet = false;
				each.regoal = true;
			}
			ai::markMoved(f, true, m_time);
		} else {
			++it;
		}
	}
	squadRadio(squad);
}


void BotManager::setTeamKits(int team, std::vector<std::pair<std::string, std::string>> kits)
{
	if (team >= 1 && team <= 2) {
		m_teamKits[static_cast<size_t>(team)] = std::move(kits);
	}
}

int BotManager::respawnKit(int player) const
{
	const auto it = m_respawn.find(player);
	return it == m_respawn.end() ? -1 : it->second.kit;
}

int BotManager::respawnPoint(int player) const
{
	const auto it = m_respawn.find(player);
	return it == m_respawn.end() ? -1 : it->second.controlPoint;
}

// The SAI's respawn of a dead bot (step 4 0x100a1a80 -> 0x100f0590, once
// the death hook 0x100a1270 queued it; the game's timer decides when it
// spawns, the SAI where and with what):
//   - a spawn point already chosen stays (vt+0xb0 slot 0x3c);
//   - 20 % of the time (U > 0.8) the last one is kept while the team may
//     still use it (IAIGame slot 0x40), no new kit;
//   - else the chooser (0x100eff20): unless (U <= 0.3 and the squad's
//     leader is a bot), with the leader alive and up, the usable spawn
//     point nearest the squad's spot (on the infantry mesh); else a
//     roulette over the usable points in a strategic area, each weighing
//     max(1, (1 + 0.033 E) T - 0.1 D): T the area's temperature for the
//     side (+0xa0 + 4 side), E the basicTemp of the empty vehicles within
//     50 m of the point (IAIInformationEnvironment slot 0x20, filter
//     0x100ef6a0), D the 2-D distance from the area to the squad's spot;
//   - the kit (0x100f04a0): floor(U x 4), the four kits alike (the SAI's
//     per-kit counts are passed and not read); the unlock levels one over
//     the best human's, at most 4 (vt+0xa0; 1 with no humans: the bots'
//     kits stay unlock-free here).
// The rewrite's spawn choice is a control point (GameWorld::setSpawnChoice):
// the point chosen gives its control point, whose spawn points the game
// draws from. Usable: a SpawnPoint of a control point the team holds.
void BotManager::chooseRespawn(const GameWorld::PlayerInfo& self)
{
	Respawn& r = m_respawn[self.id];
	if (r.chosen || (self.team != 1 && self.team != 2)) {
		return;
	}
	r.chosen = true;
	ensureAreas();
	struct Spawn {
		physics::Vec3 at;
		int controlPoint;
	};
	std::vector<Spawn> spawns;
	const std::vector<GameWorld::ControlPointInfo> points = m_world.controlPointList();
	for (const auto& owned : m_world.objects().objects()) {
		const world::ObjectTemplate* t = owned->objectTemplate();
		if (!t || t->typeName() != "SpawnPoint") {
			continue;
		}
		int cp = owned->controlPointId();
		if (cp == -1) {
			const auto it = t->settings().find("controlPointId");
			cp = it != t->settings().end() ? std::atoi(it->second.c_str()) : -1;
		}
		for (const GameWorld::ControlPointInfo& p : points) {
			if (p.id == cp && p.team == self.team) {
				spawns.push_back({{owned->position().x, owned->position().y, owned->position().z}, cp});
			}
		}
	}
	if (spawns.empty()) {
		m_world.setSpawnChoice(self.id, -1); // vt+0x104(-1, 0xe)
		return;
	}
	if (random() > 0.8f) {
		for (const Spawn& sp : spawns) {
			if (sp.controlPoint == r.controlPoint) {
				m_world.setSpawnChoice(self.id, r.controlPoint);
				return;
			}
		}
	}
	// The chooser (0x100eff20).
	const Squad* squad = self.squad ? this->squad(self.team, self.squad) : nullptr;
	const physics::Vec3 spot = squad ? squad->at : self.position;
	const GameWorld::PlayerInfo* leader = squad ? squadLeaderOf(*squad) : nullptr;
	int chosen = -1;
	const bool skipNear = random() <= 0.3f && leader && leader->ai;
	if (!skipNear && leader && leader->alive && !leader->manDown) {
		float best = 3.4028235e38f;
		for (size_t i = 0; i < spawns.size(); ++i) {
			const float d = distance2(spawns[i].at, spot);
			if (d < best && reachable(spawns[i].at)) {
				best = d;
				chosen = static_cast<int>(i);
			}
		}
	}
	if (chosen < 0) {
		const Side& side = m_sides[self.team];
		std::vector<std::pair<float, int>> wheel;
		float total = 0;
		for (size_t i = 0; i < spawns.size(); ++i) {
			physics::Vec3 centre;
			const int area = areaAt(spawns[i].at, self.team, &centre);
			if (area < 0) {
				continue;
			}
			const float temperature = static_cast<size_t>(area) < side.temperature.size() ? side.temperature[static_cast<size_t>(area)] : 0;
			float empty = 0;
			for (const GameWorld::VehicleInfo& v : m_vehicles) {
				if (m_vehicleTeams.count(v.object) || !v.working || distance2(v.position, spawns[i].at) > 2500.0f) {
					continue;
				}
				const world::Object* o = m_world.objects().find(v.object);
				if (o && o->objectTemplate()) {
					const UnitAi& u = unitAiOf(o->objectTemplate());
					empty += u.basicTemp;
				}
			}
			const float d = std::sqrt(horizontalDistance2(centre, spot));
			const float w = std::max(1.0f, (1 + 0.033f * empty) * temperature - 0.1f * d);
			wheel.push_back({w, static_cast<int>(i)});
			total += w;
		}
		if (!wheel.empty()) {
			float spin = random() * total;
			chosen = wheel.back().second;
			for (const auto& [w, i] : wheel) {
				if ((spin -= w) <= 0) {
					chosen = i;
					break;
				}
			}
		}
	}
	if (chosen < 0) {
		m_world.setSpawnChoice(self.id, -1);
		return;
	}
	r.controlPoint = spawns[static_cast<size_t>(chosen)].controlPoint;
	// The kit (0x100f04a0).
	const auto& kits = m_teamKits[static_cast<size_t>(self.team)];
	r.kit = static_cast<int>(std::floor(random() * 4));
	if (static_cast<size_t>(r.kit) < kits.size()) {
		m_world.setKit(self.id, kits[static_cast<size_t>(r.kit)].first, kits[static_cast<size_t>(r.kit)].second);
		// Its unlocks (0x100efce0 -> the bot's player vt+0xb0 -> vt+0xa0).
		m_world.chooseBotUnlocks(self.id, [this](const std::string& aiWeapon) {
			std::string key = aiWeapon;
			std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			const auto w = m_settings.weapons.find(key);
			return w != m_settings.weapons.end() && w->second.antiAir;
		});
	}
	m_world.setSpawnChoice(self.id, r.controlPoint);
}

} // namespace dice::hfe::game
