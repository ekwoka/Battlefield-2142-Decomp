// Radio messages: who hears one (BF2142.exe's game server, 0x004a6f70)
// and the target channels the scripts name (0x0042d4a0).
//
// A radio message carries the message's index in the game logic's radio
// table, its target channel, its sender and, for some channels, a team,
// squad or player (the message's slot 0x10). The game client sends the
// local player's (Game.radioMessage, 0x00423e61 -> the client's slot 0x50,
// 0x004b2b60: the spam check, then RadioMessage event 0x14); the server
// hands it to each player 0x004a6f70 says hears it (0x004a7800 on the
// client's side), and the AI's own sends go through the AI game effector
// (0x00896270, vtable 0x009d6578 slot 1) with the AI's channels mapped to
// target channels.
//
// 0x004a6f70 (the game server's this: its +0x108, +0x10c, +0x110 and +0x114
// are squared distances, 10000, 225, 10000 and 10000, from its constructor
// 0x004b2330; 0x004421b0 is the squared length):
//   TCGlobal 0           everyone but the sender
//   TCTeam 1             the sender's team
//   TCTeamAndLocalEnemies 0x11  and anyone within 15 m
//   TCCommander 2        the team's commander and squad leaders (slots
//                        0x128, 0x150)
//   TCSquad 3            the sender's squad, not the commander (0x128),
//                        same team
//   TCSquadAndLocalEnemies 0x12  and anyone within 15 m
//   TCSquadLeader 4      the leader of the message's squad (+0x10 against
//                        the receiver's squad, slot 0x120; slot 0x150)
//   TCSquadId 5          the message's squad, same team
//   TCLocalTeamMedic 6 / TCLocalRevive 0xf   teammates within 100 m that
//                        carry the medic's means (see hearsRadio), else
//                        as TCAutoLocalRevive
//   TCLocal 9            anyone within 100 m
//   TCLocalTeam 10       teammates within 100 m, enemies within 15 m
//   TCAutoLocalRevive 0x10  anyone within 15 m
//   TCLocalGroundVehicle 7  anyone within 15 m; else teammates of the
//                        sender's squad, or riding a vehicle that isn't
//                        their soldier whose template's category (its
//                        interface 0xc4c4 slot 0x24: vehicleCategory) is
//                        VCLand within 100 m (+0x114) or VCAir /
//                        VCHelicopter within 100 m (+0x110)
//   TCLocalAirVehicle 8  the same without the 15 m
//   TCVehicle 0xb        those in the sender's vehicle (the same root)
//   TCServerMessage 0xc  everyone
//   TCServerTeamMessage 0xd  the message's team
//   TCPlayer 0xe         the message's player
// The distances are between the players' controlled objects (slot 0x4c:
// the vehicle aboard, else the soldier); neither may be missing.
#include "GameWorld.h"

#include "CombatAreas.h"

#include "World/Object.h"
#include "World/ObjectTemplate.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace dice::hfe::game {

int GameWorld::vehicleCategory(const world::ObjectTemplate& t)
{
	static const char* const names[] = {"VCLand", "VCSea", "VCAir", "VCHelicopter", "VCNoVehicleCategory"};
	const auto found = t.settings().find("vehicleCategory");
	if (found == t.settings().end()) {
		return 0; // VCLand
	}
	for (int i = 0; i < 5; ++i) {
		if (found->second == names[i]) {
			return i;
		}
	}
	return std::atoi(found->second.c_str());
}

int GameWorld::targetChannel(const std::string& name)
{
	static const std::pair<const char*, int> channels[] = {{"TCGlobal", 0}, {"TCTeam", 1}, {"TCCommander", 2},
		{"TCSquad", 3}, {"TCSquadLeader", 4}, {"TCSquadId", 5}, {"TCLocalTeamMedic", 6}, {"TCLocalGroundVehicle", 7},
		{"TCLocalAirVehicle", 8}, {"TCLocal", 9}, {"TCLocalTeam", 10}, {"TCVehicle", 11}, {"TCServerMessage", 12},
		{"TCServerTeamMessage", 13}, {"TCPlayer", 14}, {"TCLocalRevive", 15}, {"TCAutoLocalRevive", 16},
		{"TCTeamAndLocalEnemies", 17}, {"TCSquadAndLocalEnemies", 18}};
	for (const auto& [n, value] : channels) {
		if (name == n) {
			return value;
		}
	}
	return -1;
}

// The medic's means for TCLocalTeamMedic: 0x004a6f70 asks the receiver's
// soldier for its interface 0xc4d7 (the soldier's +0x18c, 0x00585af0),
// whose slot 0x58 (0x005afc00) is its kit (+0x228), and the kit's +0x74's
// slot 0x38 for a flag; that flag's setter wasn't traced. The rewrite
// takes a kit carrying a healing gadget (ammo.replenishingType RTHeal: the
// medic hub, the defibrillator's kit) as having it.
bool GameWorld::carriesHealing(const Player& player) const
{
	for (const Player::Carried& item : player.carried) {
		const world::ObjectTemplate* t = m_templates.find(item.name);
		Ability ability;
		bool whileFiring = false;
		if (t && abilityOf(*t, ability, whileFiring) && !ability.ammo) {
			return true;
		}
	}
	return false;
}

bool GameWorld::hearsRadio(int channel, int senderId, int receiverId, int value) const
{
	constexpr float kFar = 10000;  // +0x108, +0x110, +0x114: 100 m squared
	constexpr float kNear = 225;   // +0x10c: 15 m squared
	const Player* sender = findPlayer(senderId);
	const Player* receiver = findPlayer(receiverId);
	if (!receiver) {
		return false;
	}
	// The controlled objects' positions (slot 0x4c: the vehicle aboard).
	PlayerInfo from, to;
	const bool haveFrom = sender && playerInfo(sender->id, from) && from.alive;
	const bool haveTo = playerInfo(receiver->id, to) && to.alive;
	const auto distance2 = [&] { return physics::lengthSquared(to.position - from.position); };
	const auto sameTeam = [&] { return sender && sender->team == receiver->team; };
	switch (channel) {
	case 0:
		return sender != receiver;
	case 2:
		if (sender == receiver || (commander(receiver->team) != receiver->id && !receiver->squadLeader)) {
			return false;
		}
		return !sender || sameTeam();
	case 3:
	case 18:
		if (!sender || sender == receiver || !haveFrom || !haveTo) {
			return false;
		}
		if (channel == 18 && distance2() <= kNear) {
			return true;
		}
		if (commander(receiver->team) == receiver->id || sender->squad != receiver->squad) {
			return false;
		}
		return sameTeam();
	case 1:
	case 17:
		if (!sender || sender == receiver || !haveFrom || !haveTo) {
			return false;
		}
		if (channel == 17 && distance2() <= kNear) {
			return true;
		}
		return sameTeam();
	case 4:
		return sender != receiver && receiver->squadLeader && receiver->squad == value && sameTeam();
	case 5:
		return sender != receiver && receiver->squad == value && sameTeam();
	case 6:
	case 15:
		if (!sender || sender == receiver || !haveFrom || !haveTo) {
			return false;
		}
		if (sameTeam() && carriesHealing(*receiver) && distance2() <= kFar) {
			return true;
		}
		[[fallthrough]];
	case 9:
	case 10:
	case 16: {
		if (!sender || sender == receiver || !haveFrom || !haveTo) {
			return false;
		}
		const bool team = channel != 10 || sameTeam();
		const float reach = channel == 6 || channel == 15 || channel == 16 ? kNear : kFar;
		const float d2 = distance2();
		if (channel == 10 && !team && d2 <= kNear) {
			return true;
		}
		return d2 <= reach && team;
	}
	case 7:
	case 8: {
		if (!sender || sender == receiver || !haveFrom || !haveTo) {
			return false;
		}
		const float d2 = distance2();
		if (channel == 7 && d2 <= kNear) {
			return true;
		}
		if (!sameTeam()) {
			return false;
		}
		if (sender->squad != 0 && sender->squad == receiver->squad) {
			return true;
		}
		if (!to.vehicle) {
			return false; // on foot: the controlled object is the soldier itself
		}
		const world::Object* vehicle = m_objects.find(to.vehicle);
		const world::ObjectTemplate* t = vehicle ? vehicle->objectTemplate() : nullptr;
		const int category = t ? vehicleCategory(*t) : -1;
		if (category == 0) {
			return d2 <= kFar; // +0x114
		}
		return (category == 2 || category == 3) && d2 <= kFar; // +0x110
	}
	case 11:
		return sender && sender != receiver && haveFrom && haveTo && from.vehicle && from.vehicle == to.vehicle;
	case 12:
		return true;
	case 13:
		return receiver->team == value;
	case 14:
		return receiver->id == value;
	default:
		return false;
	}
}

void GameWorld::radioMessage(int sender, int channel, const std::string& voice, int value)
{
	if (channel < 0 || voice.empty()) {
		return;
	}
	for (const auto& player : m_players) {
		if (hearsRadio(channel, sender, player->id, value)) {
			m_radioHeard.push_back({sender, player->id, channel, voice});
		}
	}
	if (m_radioHeard.size() > 256) {
		m_radioHeard.erase(m_radioHeard.begin(), m_radioHeard.end() - 256);
	}
}

std::vector<GameWorld::RadioHeard> GameWorld::takeRadioHeard(const std::function<bool(int receiver)>& receiver)
{
	std::vector<RadioHeard> taken;
	const auto keep = std::stable_partition(m_radioHeard.begin(), m_radioHeard.end(),
		[&](const RadioHeard& heard) { return !receiver(heard.receiver); });
	taken.assign(keep, m_radioHeard.end());
	m_radioHeard.erase(keep, m_radioHeard.end());
	return taken;
}

// 0x004ca5d0 (the combat area manager, DAT_00a25134): the areas that
// apply to the player (0x004d9d50: its team or no team on either side, and
// binding the controlled object's vehicleCategory, 0x004d9cc0); with the
// manager in use and one of them a plain area, inside one of the plain
// ones and in none of the cut-outs (+0x38). Otherwise the heightmap's
// bounds (its cluster's slot 0x2c), which the rewrite's ground doesn't
// give: inside.
bool GameWorld::insideCombatArea(int playerId) const
{
	PlayerInfo info;
	if (!playerInfo(playerId, info) || !info.alive) {
		return false;
	}
	if (!m_combatAreas || !m_combatAreas->use) {
		return true;
	}
	int category = 4; // a soldier's template has no category: VCNoVehicleCategory
	if (info.vehicle) {
		const world::Object* vehicle = m_objects.find(info.vehicle);
		category = vehicle && vehicle->objectTemplate() ? vehicleCategory(*vehicle->objectTemplate()) : 4;
	}
	const auto applies = [&](const CombatArea& area) {
		return (info.team == 0 || area.team == 0 || area.team == info.team) && area.binds(category);
	};
	bool anyPlain = false, inside = false;
	for (const CombatArea& area : m_combatAreas->areas) {
		if (!area.inverted && applies(area)) {
			anyPlain = true;
			inside = inside || area.contains(info.position.x, info.position.z);
		}
	}
	if (!anyPlain) {
		return true;
	}
	for (const CombatArea& area : m_combatAreas->areas) {
		if (area.inverted && applies(area) && area.contains(info.position.x, info.position.z)) {
			inside = false;
		}
	}
	return inside;
}

int GameWorld::crosshairTarget(int id, float reach, physics::Vec3* point) const
{
	const Player* me = findPlayer(id);
	if (!me || !me->alive) {
		return -1;
	}
	const float yaw = lookYaw(*me) * 3.14159265f / 180.0f;
	const float pitch = me->input.aimPitch * 3.14159265f / 180.0f;
	const physics::Vec3 dir {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
	const physics::Vec3 eye = me->body.position + physics::Vec3 {0, 0.6f, 0};
	// The nearest enemy along the ray with nothing in between (the static
	// objects, the bodies and the terrain: lineOfSight).
	int found = -1;
	float best = reach;
	for (const auto& owned : m_players) {
		const Player& p = *owned;
		if (!p.present || !p.alive || p.id == id || p.team == me->team) {
			continue;
		}
		// A soldier as a 0.6 m sphere about its middle, a vehicle as a
		// 2.5 m one about its origin (as the name tags' ray).
		physics::Vec3 centre = p.body.position;
		float radius = 0.6f;
		if (p.vehicle) {
			if (const world::Object* v = m_objects.find(p.vehicle)) {
				centre = {v->position().x, v->position().y, v->position().z};
				radius = 2.5f;
			}
		}
		const physics::Vec3 d = centre - eye;
		const float along = physics::dot(d, dir);
		if (along <= 0 || along >= best || physics::lengthSquared(d) - along * along > radius * radius) {
			continue;
		}
		if (!lineOfSight(eye, centre, p.vehicle ? p.vehicle : me->vehicle)) {
			continue;
		}
		best = along;
		found = p.id;
	}
	if (found >= 0 && point) {
		*point = eye + dir * best;
	}
	return found;
}

int GameWorld::spotCrosshair(int player)
{
	const int target = crosshairTarget(player);
	if (target >= 0) {
		reportBattlefieldObjects(player, {target});
	}
	return target;
}

} // namespace dice::hfe::game
