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
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>
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
// whose slot 0x58 (0x005afc00) is its kit (+0x228), and the kit's +0x74
// (the object's component of interface 0xc4b1, kept by 0x006f99e0: the
// Ability component, vtable 0x00988e50) for its slot 0x38 (0x00658590:
// the template's +0x10), ability.hasHealingAbility (+0x10 of hasHealing,
// hasRepairing, hasAmmo, showPlayers, showVehicles, radialRadar, then
// radarRadius +0x18, which the component's constructor 0x00656550 reads).
// Shipped: EU_Assault and Pac_Assault.
bool GameWorld::carriesHealing(const Player& player) const
{
	const world::Object* worn = player.kitObject ? m_objects.find(player.kitObject) : nullptr;
	const world::ObjectTemplate* kit = worn && worn->objectTemplate() ? worn->objectTemplate()
		: player.kitTemplate.empty()                                   ? nullptr
																		: m_templates.find(player.kitTemplate);
	if (!kit) {
		return false;
	}
	const auto& components = kit->components();
	const bool ability = std::any_of(components.begin(), components.end(), [](const std::string& c) {
		return c.size() == 7 && std::equal(c.begin(), c.end(), "ability", [](char a, char b) {
			return std::tolower(static_cast<unsigned char>(a)) == b;
		});
	});
	const auto healing = kit->settings().find("ability.hasHealingAbility");
	return ability && healing != kit->settings().end() && std::atoi(healing->second.c_str()) != 0;
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
	// The controlled objects (slot 0x4c, 0x00593df0: the player's handle
	// +0x80, the vehicle aboard, else the soldier, kept through man down
	// and as a corpse until the body is removed; PlayerInfo::heldVehicle)
	// and their positions (slot 0x60).
	PlayerInfo from, to;
	const world::Object* fromObject = nullptr;
	const world::Object* toObject = nullptr;
	if (sender && playerInfo(sender->id, from) && from.heldVehicle) {
		fromObject = m_objects.find(from.heldVehicle);
	}
	if (playerInfo(receiver->id, to) && to.heldVehicle) {
		toObject = m_objects.find(to.heldVehicle);
	}
	const bool haveFrom = fromObject != nullptr;
	const bool haveTo = toObject != nullptr;
	const auto distance2 = [&] {
		const auto& a = fromObject->position();
		const auto& b = toObject->position();
		return physics::lengthSquared(physics::Vec3 {b.x - a.x, b.y - a.y, b.z - a.z});
	};
	const auto sameTeam = [&] { return sender && sender->team == receiver->team; };
	const auto downApart = [&] { return sender && !sender->alive && receiver->alive; };
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
	// 4, 5 and 0xe first take the chat's down rule again (a dead or
	// man-down sender, slots 0x74 / 0x114, only to the dead and the downed;
	// the rewrite's man down isn't alive).
	case 4:
		return sender != receiver && !downApart() && receiver->squadLeader && receiver->squad == value && sameTeam();
	case 5:
		return sender != receiver && !downApart() && receiver->squad == value && sameTeam();
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
		if (to.heldVehicle == to.heldSoldier) {
			return false; // the controlled object is the default vehicle (slot 0xb4): on foot
		}
		const world::ObjectTemplate* t = toObject->objectTemplate();
		const int category = t ? vehicleCategory(*t) : -1;
		if (category == 0) {
			return d2 <= kFar; // +0x114
		}
		return (category == 2 || category == 3) && d2 <= kFar; // +0x110
	}
	case 11:
		// The two controlled objects' roots (0x006f1d70) the same.
		return sender && sender != receiver && haveFrom && haveTo && from.heldVehicle == to.heldVehicle;
	case 12:
		return true;
	case 13:
		return receiver->team == value;
	case 14:
		return sender != receiver && !downApart() && receiver->id == value;
	default:
		return false;
	}
}

bool GameWorld::hearsChat(int channel, int senderId, int receiverId, int value) const
{
	if (channel != 12 && channel != 13) {
		PlayerInfo from, to;
		if (!playerInfo(senderId, from) || !playerInfo(receiverId, to)) {
			return false;
		}
		// Down: not alive (player slot 0x74, +0xd5) or man down (slot
		// 0x114, +0xe0), a man-down sender counting alive on 0xf / 0x10.
		// The original's man down is alive with +0xe0 set; the rewrite's
		// man down is not alive, so it is told apart here.
		const bool senderDown = from.manDown ? channel != 15 && channel != 16 : !from.alive;
		const bool receiverDown = !to.alive || to.manDown;
		if (senderDown && senderDown != receiverDown) {
			return false;
		}
	}
	return hearsRadio(channel, senderId, receiverId, value);
}

// The server's hand-out (0x004ab600): to each client's player and its own
// (0x004a93b0, 0x004a7800) with the chat's rule, hearsChat; to the bots
// (0x004a8a00, the players with the bot bit, slot 0x68 +0xd4) only from a
// living sender (slot 0x74; man down counts alive) to a living bot, then
// 0x004a6f70.
void GameWorld::radioMessage(int sender, int channel, const std::string& voice, int value)
{
	if (channel < 0 || voice.empty()) {
		return;
	}
	const Player* from = sender >= 0 ? findPlayer(sender) : nullptr;
	const bool senderLiving = from && (from->alive || from->manDown);
	for (const auto& player : m_players) {
		const bool hears = player->ai
			? senderLiving && (player->alive || player->manDown) && hearsRadio(channel, sender, player->id, value)
			: hearsChat(channel, sender, player->id, value);
		if (hears) {
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
	const Player* player = findPlayer(playerId);
	return player && insideCombatArea(*player);
}

bool GameWorld::insideCombatArea(const Player& player) const
{
	if (!player.present || !player.alive) {
		return false;
	}
	if (!m_combatAreas || !m_combatAreas->use) {
		return true;
	}
	// 0x004ca5d0 passes the category 4 to 0x004d9d50, which then takes the
	// controlled object's own: through the vehicle-category interface, and an
	// object without it, a soldier on foot, reads 0 (VCLand, 0x004d9dba). The
	// 4 is that "ask the object" marker, not a category.
	int category = 0;
	physics::Vec3 at = player.body.position;
	if (player.vehicle) {
		const world::Object* vehicle = m_objects.find(player.vehicle);
		category = vehicle && vehicle->objectTemplate() ? vehicleCategory(*vehicle->objectTemplate()) : 0;
		if (vehicle) {
			at = {vehicle->position().x, vehicle->position().y, vehicle->position().z};
		}
	}
	struct {
		int team;
		physics::Vec3 position;
	} info {player.team, at};
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

float GameWorld::outsideCombatArea(int id) const
{
	const Player* player = findPlayer(id);
	return player && player->outsideTime > 0 ? player->outsideTime : -1;
}

float GameWorld::combatAreaTimeLeft(int id) const
{
	const Player* player = findPlayer(id);
	if (!player || player->outsideTime <= 0 || !m_combatAreas || !m_combatAreas->use) {
		return -1;
	}
	int category = 4;
	if (player->vehicle) {
		const world::Object* vehicle = m_objects.find(player->vehicle);
		category = vehicle && vehicle->objectTemplate() ? vehicleCategory(*vehicle->objectTemplate()) : 4;
	}
	// 0x0044c7b0: what is left of the controlled object's timer.
	return std::max(0.0f, m_combatAreas->timerFor(category, !player->vehicle) - player->outsideTime);
}

void GameWorld::stepCombatAreas(float seconds)
{
	if (!m_combatAreas) {
		return;
	}
	const CombatAreas& areas = *m_combatAreas;
	// The players (0x004bbf60, every one, every seat): dead or inside, the
	// time outside is 0; outside it runs, and past the controlled object's
	// timer the soldier takes the manager's damage each tick (not by the
	// second; 100 kills at once).
	for (const auto& owned : m_players) {
		Player& player = *owned;
		if (!player.present) {
			continue;
		}
		if (!player.alive || insideCombatArea(player)) {
			player.outsideTime = 0;
			continue;
		}
		const bool first = player.outsideTime <= 0;
		player.outsideTime += seconds;
		if (first && &player == m_player && !player.ai) {
			// The HUD's first frame of the warning (HUD +0x24f): the
			// commander's "leaving_the_combat_area" (0x0044c7b0).
			m_radioHeard.push_back({-1, player.id, 13, "leaving_the_combat_area"});
		}
		int category = 4;
		if (player.vehicle) {
			const world::Object* vehicle = m_objects.find(player.vehicle);
			category = vehicle && vehicle->objectTemplate() ? vehicleCategory(*vehicle->objectTemplate()) : 4;
		}
		if (player.outsideTime > areas.timerFor(category, !player.vehicle) && player.object) {
			loseHitPoints(player.object, areas.damage, -1);
		}
	}
	// Abandoned vehicles that destroyWhenOutsideCombatArea (0x004bed00):
	// outside the areas binding every vehicle (0x004cb120, any team), after
	// 15 s (0x0091f2c0) they lose damage(category) x floor(t / timer)^2 a
	// second; taken, they start over.
	std::vector<std::pair<uint32_t, float>> losses; // applied after the walk (a wreck may go)
	for (const Body& body : m_bodies) {
		const world::Object* object = body.vehicle ? m_objects.find(body.object) : nullptr;
		const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr;
		if (!t) {
			continue;
		}
		const auto flag = t->settings().find("destroyWhenOutsideCombatArea");
		if (flag == t->settings().end() || std::atoi(flag->second.c_str()) == 0) {
			continue;
		}
		if (playerOf(body.object) >= 0) {
			m_outsideVehicles.erase(body.object);
			continue;
		}
		bool inside = true;
		if (areas.use) {
			bool any = false, in = false;
			for (const CombatArea& area : areas.areas) {
				if (area.vehicles == 4 && !area.inverted) {
					any = true;
					in = in || area.contains(object->position().x, object->position().z);
				}
			}
			inside = !any || in;
		}
		if (inside) {
			m_outsideVehicles.erase(body.object);
			continue;
		}
		float& t2 = m_outsideVehicles[body.object];
		t2 += seconds;
		if (t2 > 15.0f) {
			const int category = vehicleCategory(*t);
			const float timer = std::max(areas.timerFor(category, false), 1e-3f);
			const float steps = std::floor(t2 / timer);
			losses.push_back({body.object, areas.damageFor(category) * steps * steps * seconds});
		}
	}
	for (const auto& [object, amount] : losses) {
		loseHitPoints(object, amount, -1);
	}
}

physics::Vec3 GameWorld::viewForward(int id) const
{
	const Player* me = findPlayer(id);
	if (!me) {
		return {};
	}
	const auto along = [](float yawDegrees, float pitchDegrees) {
		const float yaw = yawDegrees * 3.14159265f / 180.0f;
		const float pitch = pitchDegrees * 3.14159265f / 180.0f;
		return physics::Vec3 {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
	};
	if (const Body* body = me->vehicle ? findBody(me->vehicle) : nullptr) {
		float yaw = 0, pitch = 0;
		if (!gunAim(body->object, me->seat, yaw, pitch) && me->seat >= 0
			&& static_cast<size_t>(me->seat) < body->looks.size()) {
			yaw = body->looks[static_cast<size_t>(me->seat)].yaw;
			pitch = body->looks[static_cast<size_t>(me->seat)].pitch;
		}
		return body->body.rotation.transform(along(yaw, pitch));
	}
	return along(lookYaw(*me), me->input.aimPitch);
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
		radioSpot(player, target);
	}
	return target;
}

namespace {

int radioInt(const world::ObjectTemplate& t, const char* name)
{
	const auto it = t.settings().find(name);
	return it == t.settings().end() ? 0 : std::atoi(it->second.c_str());
}

} // namespace

// The close-to indications (GameWorld.h, CloseToEvent): each living
// player's scan every 0.2 s (0x0059a6c0; its countdown +0x178 restarts
// at 0.2 after a scan), a dead one's list cleared (0x0059a5f0).
void GameWorld::stepCloseTo(float seconds)
{
	// Events nobody took (bots, players without a client) don't pile up.
	const uint64_t now = m_ticks;
	m_closeToEvents.erase(std::remove_if(m_closeToEvents.begin(), m_closeToEvents.end(),
							  [now](const CloseToEvent& e) { return now - e.tick > 60; }),
		m_closeToEvents.end());
	for (const auto& owned : m_players) {
		if (!owned || !owned->present) {
			continue;
		}
		Player& player = *owned;
		CloseToScan& scan = m_closeTo[player.id];
		if (!player.alive || player.manDown) {
			clearCloseTo(player.id, scan);
			continue;
		}
		scan.countdown -= seconds;
		if (scan.countdown <= 0) {
			scanCloseTo(player, scan);
			scan.countdown = 0.2f;
		}
	}
	for (auto it = m_closeTo.begin(); it != m_closeTo.end();) {
		it = findPlayer(it->first) ? std::next(it) : m_closeTo.erase(it);
	}
}

void GameWorld::clearCloseTo(int player, CloseToScan& scan)
{
	if (!scan.entries.empty()) {
		scan.entries.clear();
		m_closeToEvents.push_back({player, -1, false, m_ticks}); // 0x0059a5f0: type -1
	}
}

std::vector<GameWorld::CloseToEvent> GameWorld::takeCloseToEvents(int player)
{
	std::vector<CloseToEvent> mine;
	auto keep = m_closeToEvents.begin();
	for (const CloseToEvent& e : m_closeToEvents) {
		if (e.player == player) {
			mine.push_back(e);
		} else {
			*keep++ = e;
		}
	}
	m_closeToEvents.erase(keep, m_closeToEvents.end());
	return mine;
}

void GameWorld::scanCloseTo(Player& player, CloseToScan& scan)
{
	// The player's controlled object (+0x80: the vehicle it drives or
	// rides, else its soldier), its position and radius (slot 0x90).
	const uint32_t controlled = player.vehicle ? player.vehicle : player.object;
	const world::Object* self = m_objects.find(controlled);
	if (!self) {
		clearCloseTo(player.id, scan);
		return;
	}
	const physics::Vec3 at = player.vehicle
		? physics::Vec3 {self->position().x, self->position().y, self->position().z}
		: player.body.position;
	const float radius = player.vehicle ? objectRadius(controlled) : std::max(objectRadius(controlled), 0.5f);
	// One candidate: its Radio rows, then the distance and team tests.
	bool any = false;
	const auto consider = [&](uint64_t key, const world::ObjectTemplate* t, const physics::Vec3& p, int team,
							  float ownRadius) {
		if (!t) {
			return;
		}
		auto rows = m_closeToRows.find(t);
		if (rows == m_closeToRows.end()) {
			CloseToRows r;
			r.distance = radioInt(*t, "radio.closeToIndicationDist"); // +0x200
			r.type = radioInt(*t, "radio.closeToIndicationType");     // +0x204
			r.flat = radioInt(*t, "radio.indicationDistance2D") != 0; // +0x1fc
			rows = m_closeToRows.emplace(t, r).first;
		}
		const int distance = rows->second.distance;
		if (distance <= 0) {
			return;
		}
		const bool flat = rows->second.flat;
		// Gathered within the radius + 200 (the sphere query meets the
		// object's own bounds), then measured from centre to centre.
		const physics::Vec3 d {p.x - at.x, flat ? 0.0f : p.y - at.y, p.z - at.z};
		const float length = std::sqrt(physics::lengthSquared(d));
		const physics::Vec3 full = p - at;
		if (std::sqrt(physics::lengthSquared(full)) - ownRadius > radius + 200) {
			return;
		}
		if (length > static_cast<float>(distance) + radius) {
			return;
		}
		// Only the player's team's (the objects with a team; the ability
		// flag that would lift this is never set).
		if (team != 0 && team != player.team) {
			return;
		}
		any = true;
		const int type = rows->second.type;
		for (CloseToEntry& e : scan.entries) {
			if (e.key == key) {
				e.state = 1;
				return;
			}
		}
		scan.entries.push_back({key, type, 0});
	};
	for (const auto& owned : m_objects.objects()) {
		const world::Object& o = *owned;
		if (o.id() == controlled || o.destroyed()) {
			continue;
		}
		consider(o.id(), o.objectTemplate(), {o.position().x, o.position().y, o.position().z}, o.team(),
			objectRadius(o.id()));
	}
	for (const Projectile& q : m_projectiles) {
		if (!q.alive || q.visual || !q.settings) {
			continue;
		}
		consider((uint64_t {1} << 32) | q.serial, m_templates.find(q.settings->name), q.position, q.team, 0);
	}
	if (scan.entries.empty() || !any) {
		clearCloseTo(player.id, scan);
		return;
	}
	// The list's walk: a new one posts 0x3e and is kept (state 2), one
	// seen again is kept, one not seen goes, posting 0x3f unless another
	// of its type is still in the list.
	std::vector<CloseToEntry> kept;
	std::set<int> gone; // a type's leave goes once, when its last one goes
	for (CloseToEntry& e : scan.entries) {
		if (e.state == 0) {
			m_closeToEvents.push_back({player.id, e.type, true, m_ticks});
		}
		if (e.state != 2) {
			e.state = 2;
			kept.push_back(e);
		} else {
			gone.insert(e.type);
		}
	}
	scan.entries = std::move(kept);
	for (const int type : gone) {
		const bool still = std::any_of(scan.entries.begin(), scan.entries.end(),
			[type](const CloseToEntry& e) { return e.type == type; });
		if (!still) {
			m_closeToEvents.push_back({player.id, type, false, m_ticks});
		}
	}
}

} // namespace dice::hfe::game
