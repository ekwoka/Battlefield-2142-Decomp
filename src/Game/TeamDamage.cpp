// Team damage (GameWorld): the armour's damage lists and the game logic's
// team damage test, ECGameLogicGLTeamDamage (category 0, id 0xb: the name
// the game logic's constructor 0x004d69d0 registers for it).
//
//  - The armour's loss (0x00656320) ends in 0x006557a0 with the attacker
//    (a player not man down, his slot 0x6c): {attacker, time, amount} goes
//    into the armour's list +0x220 (slot 0x120). For a soldier (class
//    0x9493) an attacker not yet in +0x238 (slot 0x128) whose sum there
//    reaches the soldier's maximum (slot 0x8c) x teamDamageScoreLimit %
//    (the score manager DAT_00a209d8 slot 0x44, +0x54: 50 from its
//    constructor 0x00560e00, scoringCommon.py's 50) joins +0x238. For
//    anything else with a PlayerControlObject up its tree (0x0042b2f0),
//    while the root's control object has a player in a seat or a team
//    (slot 0x98) the entry also goes into +0x22c (slot 0x124); unmanned
//    and of no team, +0x22c is emptied.
//  - The game logic's damage (0x004c0e40), after the hit: for a soldier
//    not down (armour slot 0x58), an attacker in +0x238 of the soldier's
//    team (slot 0xf8) is taken out of it and his entries out of +0x220;
//    for a vehicle with +0x22c, the attacker of the team of its first
//    occupant (else of the vehicle's, slot 0x98) whose +0x22c sum reaches
//    its maximum x teamVehicleDamageScoreLimit % (slot 0x4c, +0x58: 25)
//    when, without damagePointOnDestroy (armour slot 0x16c), the armour
//    still stands, or with it, this hit destroyed it, has his entries taken
//    out of +0x22c. Either raises PlayerTeamDamagePoint (Python event 0x24:
//    attacker, the hit object) and sends event 0/0xb {kind (0 a soldier, 1
//    a vehicle), the victim (the soldier's player, the first occupant or
//    -1), the attacker} to the attacker and the victim (DAT_00a1b5d8 slot
//    0x88; the server's own player's at once, 0x005099f0).
//  - A heal (0x00655a00) adds its hit points to +0x264; past 50 hit points,
//    or with the armour back to full, +0x220 is emptied and +0x264 cleared.
//  - The HUD (0x00759bf0 case 0xb), for the local player: see
//    teamDamageNotice.
#include "GameWorld.h"

#include "World/Object.h"
#include "World/ObjectTemplate.h"

#include <algorithm>

namespace dice::hfe::game {

namespace {

constexpr float kHealClears = 50.0f; // 0x00655a00: +0x264 over 50
constexpr size_t kPendingMost = 64;

} // namespace

void GameWorld::recordArmourDamage(uint32_t armour, uint32_t vehicle, float amount, int attacker, float most)
{
	const Player* from = attacker >= 0 ? findPlayer(attacker) : nullptr;
	if (!from || from->manDown || amount <= 0) {
		return;
	}
	ArmourDamage& damage = m_armourDamage[armour];
	damage.all[attacker] += amount;
	if (soldierPlayer(armour)) {
		if (!damage.over.count(attacker) && damage.all[attacker] >= most * m_teamDamageLimits[0] * 0.01f) {
			damage.over.insert(attacker);
		}
		return;
	}
	const Body* body = findBody(vehicle);
	if (!body || !body->controlObject) {
		return;
	}
	const bool manned = std::any_of(body->occupants.begin(), body->occupants.end(), [](int p) { return p >= 0; });
	const world::Object* object = m_objects.find(vehicle);
	if (!manned && !(object && object->team() != 0)) {
		damage.manned.clear();
		return;
	}
	damage.manned[attacker] += amount;
}

void GameWorld::checkTeamDamage(uint32_t armour, uint32_t vehicle, int attacker, bool destroyed, bool wasDestroyed,
	bool pointOnDestroy, float most)
{
	const Player* from = attacker >= 0 ? findPlayer(attacker) : nullptr;
	const auto found = m_armourDamage.find(armour);
	if (!from || found == m_armourDamage.end()) {
		return;
	}
	ArmourDamage& damage = found->second;
	TeamDamage event;
	event.attacker = attacker;
	if (const Player* victim = soldierPlayer(armour)) {
		if (destroyed || !damage.over.count(attacker) || victim->team != from->team) {
			return;
		}
		damage.over.erase(attacker);
		damage.all.erase(attacker);
		event.kind = 0;
		event.victim = victim->id;
		event.object = armour;
	} else {
		const auto sum = damage.manned.find(attacker);
		const Body* body = findBody(vehicle);
		if (sum == damage.manned.end() || !body || !body->controlObject) {
			return;
		}
		int driver = -1;
		int team = 0;
		for (const int occupant : body->occupants) {
			if (const Player* p = occupant >= 0 ? findPlayer(occupant) : nullptr) {
				driver = occupant;
				team = p->team;
				break;
			}
		}
		if (driver < 0) {
			const world::Object* object = m_objects.find(vehicle);
			team = object ? object->team() : 0;
		}
		if (team != from->team || sum->second < most * m_teamDamageLimits[1] * 0.01f) {
			return;
		}
		if (!((!destroyed && !pointOnDestroy) || (pointOnDestroy && destroyed && !wasDestroyed))) {
			return;
		}
		damage.manned.erase(sum);
		event.kind = 1;
		event.victim = driver;
		// The hit object (a shield cell's generator isn't a world object
		// here): the vehicle, which scoringCommon's handler types by name.
		event.object = vehicle;
	}
	raise("PlayerTeamDamagePoint", {ScriptValue::player(attacker), ScriptValue::object(event.object)});
	PendingTeamDamage pending;
	pending.event = event;
	pending.recipients.push_back(attacker);
	if (event.victim >= 0 && event.victim != attacker) {
		pending.recipients.push_back(event.victim);
	}
	m_teamDamage.push_back(std::move(pending));
	if (m_teamDamage.size() > kPendingMost) {
		m_teamDamage.erase(m_teamDamage.begin()); // the bots' never taken
	}
}

void GameWorld::armourHealed(uint32_t armour, float amount, bool full)
{
	const auto found = m_armourDamage.find(armour);
	if (found == m_armourDamage.end() || amount <= 0) {
		return;
	}
	ArmourDamage& damage = found->second;
	damage.healed += amount;
	if (damage.healed > kHealClears || full) {
		damage.all.clear();
		damage.healed = 0;
	}
}

std::vector<GameWorld::TeamDamage> GameWorld::takeTeamDamage(int recipient)
{
	std::vector<TeamDamage> taken;
	for (auto it = m_teamDamage.begin(); it != m_teamDamage.end();) {
		auto& to = it->recipients;
		if (const auto at = std::find(to.begin(), to.end(), recipient); at != to.end()) {
			to.erase(at);
			taken.push_back(it->event);
		}
		it = to.empty() ? m_teamDamage.erase(it) : it + 1;
	}
	return taken;
}

GameWorld::TeamDamageNotice GameWorld::teamDamageNotice(const TeamDamage& event, int local, bool victimUp)
{
	if (local < 0) {
		return TeamDamageNotice::None;
	}
	if (event.victim == local) {
		return TeamDamageNotice::SayFriendlyFire;
	}
	if (victimUp && event.attacker == local) {
		return TeamDamageNotice::VictimSaysFriendlyFire;
	}
	return event.kind == 0 ? TeamDamageNotice::PlayerLine
		: event.kind == 1  ? TeamDamageNotice::VehicleLine
						   : TeamDamageNotice::None;
}

} // namespace dice::hfe::game
