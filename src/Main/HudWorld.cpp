#include "HudWorld.h"

#include "Game/CombatAreas.h"
#include "Game/GameLogic.h"
#include "Game/Weapons.h"
#include "Net/RemoteWorld.h"
#include "Render/PostProduction.h"
#include "World/Object.h"
#include "World/ObjectTemplate.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dice::hfe::host {

namespace {

float templateNumber(const world::ObjectTemplate* t, const char* key, float otherwise)
{
	if (!t) {
		return otherwise;
	}
	const auto it = t->settings().find(key);
	return it != t->settings().end() ? static_cast<float>(std::atof(it->second.c_str())) : otherwise;
}

} // namespace

int HudWorld::playerTeam() const
{
	PlayerInfo me;
	return playerInfo(localPlayer(), me) ? me.team : 0;
}

// --- the local round

float LocalHudWorld::objectHealth(uint32_t object) const
{
	const float hp = m_round.hitPoints(object);
	const world::Object* owned = m_round.objects().find(object);
	const world::ObjectTemplate* t = owned ? owned->objectTemplate() : nullptr;
	const float most = templateNumber(t, "armor.maxHitPoints", templateNumber(t, "armor.hitPoints", 0));
	return hp >= 0 && most > 0 ? hp / most : -1;
}

// PlayerHealth (0x007a0f80): the soldier's armour's hit points (slot
// 0x3c) over its maxHitPoints (slot 0x8c).
float LocalHudWorld::health() const
{
	const world::Object* soldier = m_round.player();
	const float fraction = soldier ? objectHealth(soldier->id()) : -1;
	return fraction >= 0 ? std::clamp(fraction, 0.0f, 1.0f) : 1.0f;
}

HudWorld::Scores LocalHudWorld::scores(int player) const
{
	Scores s;
	PlayerInfo p;
	if (m_round.playerInfo(player, p)) {
		s.score = scoreOf ? scoreOf(p.id) : p.kills * 2 - p.teamKills * 4;
	}
	s.teamwork = 0;
	s.ping = p.ai ? -1 : 0;
	return s;
}

float LocalHudWorld::tickets(int team) const
{
	return m_logic.team(team).tickets;
}

HudWorld::Weapon LocalHudWorld::heldWeapon() const
{
	Weapon w;
	w.gun = m_round.playerWeapon();
	if (const world::Object* rifle = m_round.objects().find(m_round.playerWeaponObject())) {
		w.weapon = rifle->objectTemplate();
	}
	if (w.gun) {
		w.rounds = w.gun->roundsInMag();
		w.mags = w.gun->magsLeft() + 1;
	}
	return w;
}

HudWorld::Weapon LocalHudWorld::seatGroupWeapon(int group) const
{
	Weapon w;
	PlayerInfo me;
	if (!m_round.playerInfo(m_round.localPlayer(), me) || !me.alive || !me.vehicle || me.seat < 0) {
		return w;
	}
	const int index = m_round.seatWeaponIndex(me.vehicle, me.seat, group);
	const auto seats = m_round.vehicleSeats(me.vehicle);
	if (index < 0 || static_cast<size_t>(me.seat) >= seats.size()
		|| static_cast<size_t>(index) >= seats[static_cast<size_t>(me.seat)].weapons.size()) {
		return w;
	}
	w.weapon = m_round.templates().find(seats[static_cast<size_t>(me.seat)].weapons[static_cast<size_t>(index)].weapon);
	w.gun = m_round.seatFireArm(me.vehicle, me.seat, index);
	if (w.gun) {
		w.rounds = w.gun->roundsInMag();
		w.mags = w.gun->magsLeft() + 1;
	}
	return w;
}

float LocalHudWorld::combatAreaTimeLeft() const
{
	// The game runs: the round plays (game logic slot 0x164 = 1).
	if (m_logic.ended() || m_logic.roundState() != 3) {
		return -1;
	}
	return m_round.combatAreaTimeLeft(m_round.localPlayer());
}

// --- a joined game

uint32_t RemoteHudWorld::objectOf(uint16_t ghost) const
{
	const auto it = ghost ? m_ghostObjects.find(ghost) : m_ghostObjects.end();
	return it != m_ghostObjects.end() ? it->second : 0;
}

uint16_t RemoteHudWorld::ghostOf(uint32_t object) const
{
	for (const auto& [ghost, id] : m_ghostObjects) {
		if (id == object) {
			return ghost;
		}
	}
	return 0;
}

int RemoteHudWorld::localPlayer() const
{
	return m_world.localPlayer();
}

void RemoteHudWorld::fill(const net::RemotePlayer& p, PlayerInfo& out) const
{
	out = {};
	out.id = p.id;
	out.name = p.name;
	out.team = p.team;
	out.ai = p.ai;
	out.alive = p.alive;
	out.hitIndicator = p.local ? hitIndicator : 0;
	out.kills = p.kills;
	out.deaths = p.deaths;
	out.teamKills = p.teamKills;
	out.squad = p.squad;
	out.squadLeader = p.squadLeader;
	out.commander = p.commander;
	out.rank = p.rank;
	out.manDown = p.manDown; // the player networkable's 0x20 (+0xe0); ours below from our own state
	out.firing = p.fire[0].on;
	out.kit = objectOf(p.kitGhost);
	// The seat (EnterVehicle's ghost, a PlayerControlObject of the vehicle's
	// tree) and its vehicle (the root's ghost); ours from the control-object
	// record.
	uint16_t seatGhost = p.vehicleGhost;
	uint16_t soldierGhost = p.soldierGhost;
	if (p.local && m_world.hasControlObject() && m_world.controlInLastBlock()) {
		const net::ControlObjectRecord& control = m_world.controlObject();
		seatGhost = control.vehicle ? control.ghost : 0;
		if (!control.vehicle && !soldierGhost) {
			soldierGhost = control.ghost;
		}
	}
	out.soldier = objectOf(soldierGhost);
	if (const net::RemoteObject* seat = seatGhost ? m_world.object(seatGhost) : nullptr) {
		out.vehicle = objectOf(seat->rootGhost ? seat->rootGhost : seat->ghost);
		out.seat = seat->part;
		out.driving = seat->part == 0;
	}
	const net::RemoteObject* body = m_world.object(soldierGhost);
	const net::RemoteObject* where = out.vehicle ? m_world.object(ghostOf(out.vehicle)) : body;
	if (where && where->hasPosition) {
		out.position = {where->position.x, where->position.y, where->position.z};
	}
	if (body && body->hasAngles) {
		out.yaw = body->yaw + body->pitch; // the body's and the twist ahead of it (+0x25c, +0x278)
		out.aimPitch = body->aimPitch;
	}
	if (body && !p.local) {
		out.pose = body->pose == 2 ? game::SoldierPose::Prone
			: body->pose == 1      ? game::SoldierPose::Crouch
			: body->pose == 3      ? game::SoldierPose::Swim
								   : game::SoldierPose::Stand;
	}
	if (body && body->weapon >= 0) {
		out.inHandSlot = body->weapon;
	}
	if (p.local) {
		physics::Vec3 feet;
		float yaw = 0, pitch = 0;
		if (ownLook && ownLook(feet, yaw, pitch)) {
			out.position = feet;
			out.yaw = yaw;
			out.aimPitch = pitch;
		} else if (m_world.hasControlObject() && m_world.controlInLastBlock()) {
			const net::Vec3f& at = m_world.controlObject().position;
			out.position = {at.x, at.y, at.z};
		}
		if (const net::ControlState* state = m_world.controlState(); state && state->hasOwn) {
			out.manDown = state->own.manDown;
		}
		// The kit's weapons by slot (their itemIndex), for the item selection.
		if (const world::Object* kit = m_round.objects().find(out.kit); kit && kit->objectTemplate()) {
			for (const world::ChildTemplate& child : kit->objectTemplate()->children()) {
				const world::ObjectTemplate* t = m_round.templates().find(child.name);
				if (t && t->typeName() == "GenericFireArm") {
					out.carried.push_back({static_cast<int>(templateNumber(t, "itemIndex", 0)), t->name()});
				}
			}
		}
	}
}

bool RemoteHudWorld::playerInfo(int player, PlayerInfo& out) const
{
	const net::RemotePlayer* p = player >= 0 ? m_world.player(player) : nullptr;
	if (!p) {
		return false;
	}
	fill(*p, out);
	return true;
}

std::vector<HudWorld::PlayerInfo> RemoteHudWorld::playerList() const
{
	std::vector<PlayerInfo> out;
	for (const net::RemotePlayer& p : m_world.playerList()) {
		out.emplace_back();
		fill(p, out.back());
	}
	return out;
}

HudWorld::Scores RemoteHudWorld::scores(int player) const
{
	Scores s;
	if (const net::RemotePlayer* p = m_world.player(player)) {
		s.score = p->score;
		s.teamwork = p->teamwork;
		s.ping = p->ai ? -1 : std::max(p->ping, 0);
	}
	return s;
}

float RemoteHudWorld::tickets(int team) const
{
	return static_cast<float>(std::max(m_world.tickets(team), 0));
}

bool RemoteHudWorld::overheadCamera(int team, float& x, float& z) const
{
	net::Vec3f at;
	if (!m_world.overheadCamera(team, at)) {
		return false;
	}
	x = at.x;
	z = at.z;
	return true;
}

int RemoteHudWorld::commander(int team) const
{
	for (const net::RemotePlayer& p : m_world.playerList()) {
		if (p.commander && p.team == team) {
			return p.id;
		}
	}
	return -1;
}

std::vector<HudWorld::ControlPointInfo> RemoteHudWorld::controlPointList() const
{
	// The control points' ghosts (ControlPoint networkables: team, flag);
	// the id of a spawn group at one names it (the id the spawn screen
	// asks for), else 1000 + the ghost.
	std::vector<ControlPointInfo> out;
	for (const net::RemoteObject& o : m_world.objectList()) {
		const world::Object* object = m_round.objects().find(objectOf(o.ghost));
		const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr;
		const bool point = o.kind == net::NetworkableKind::ControlPoint || (t && t->typeName() == "ControlPoint");
		if (!point || !o.hasPosition) {
			continue;
		}
		ControlPointInfo cp {1000 + o.ghost, o.team, {o.position.x, o.position.y, o.position.z},
			templateNumber(t, "radius", 10), templateNumber(t, "unableToChangeTeam", 0) == 0};
		for (const auto& [id, group] : m_world.spawnGroups()) {
			if (group.ghost == o.ghost) {
				cp.id = id;
			}
		}
		out.push_back(cp);
	}
	return out;
}

std::vector<int> RemoteHudWorld::spawnablePoints(int team) const
{
	std::vector<int> out;
	for (const auto& [id, group] : m_world.spawnGroups()) {
		if (group.team == team) {
			out.push_back(id);
		}
	}
	return out;
}

std::vector<uint32_t> RemoteHudWorld::spawnVehicles(int team) const
{
	std::vector<uint32_t> out;
	for (const auto& [id, group] : m_world.spawnGroups()) {
		const world::Object* object = group.team == team ? m_round.objects().find(objectOf(group.ghost)) : nullptr;
		if (object && object->objectTemplate() && object->objectTemplate()->typeName() == "PlayerControlObject") {
			out.push_back(object->id());
		}
	}
	return out;
}

int RemoteHudWorld::spawnGroupAt(uint32_t object) const
{
	for (const auto& [id, group] : m_world.spawnGroups()) {
		if (object != 0 && objectOf(group.ghost) == object) {
			return id;
		}
	}
	return -1;
}

std::vector<HudWorld::VehicleInfo> RemoteHudWorld::vehicleList() const
{
	std::vector<VehicleInfo> out;
	for (const net::RemoteObject& o : m_world.objectList()) {
		if (o.part != 0 || !o.hasPosition) {
			continue;
		}
		const world::Object* object = m_round.objects().find(objectOf(o.ghost));
		const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr;
		if (!t || t->typeName() != "PlayerControlObject") {
			continue;
		}
		VehicleInfo v {};
		v.object = object->id();
		v.position = {o.position.x, o.position.y, o.position.z};
		v.yaw = object->rotation().x;
		v.working = o.health != 0;
		if (o.hasVelocity) {
			v.velocity = {o.velocity.x, o.velocity.y, o.velocity.z};
		}
		const auto occupants = vehicleOccupants(v.object);
		for (size_t i = 0; i < occupants.size(); ++i) {
			v.freeSeats += occupants[i] < 0 ? 1 : 0;
		}
		v.driverFree = occupants.empty() || occupants[0] < 0;
		out.push_back(v);
	}
	return out;
}

std::vector<int> RemoteHudWorld::vehicleOccupants(uint32_t vehicle) const
{
	// The vehicle's ghosts in pre-order (the root's, then its seats'), as
	// GameWorld::vehicleSeats lists its seats.
	std::vector<int> out;
	const net::RemoteObject* root = m_world.object(ghostOf(vehicle));
	if (!root) {
		return out;
	}
	out.push_back(root->occupant);
	for (const uint16_t seat : root->seats) {
		const net::RemoteObject* s = m_world.object(seat);
		out.push_back(s ? s->occupant : -1);
	}
	return out;
}

float RemoteHudWorld::hitPoints(uint32_t object) const
{
	const net::RemoteObject* o = m_world.object(ghostOf(object));
	return o && o->health >= 0 ? o->health : 0;
}

float RemoteHudWorld::objectHealth(uint32_t object) const
{
	// A soldier's networked health is a fraction already (0x005fcdb0); the
	// others' are hit points.
	const net::RemoteObject* o = m_world.object(ghostOf(object));
	if (!o || o->health < 0) {
		return -1;
	}
	if (o->kind == net::NetworkableKind::Soldier) {
		return o->health;
	}
	const world::Object* owned = m_round.objects().find(object);
	const float most = templateNumber(owned ? owned->objectTemplate() : nullptr, "armor.maxHitPoints", 0);
	return most > 0 ? o->health / most : -1;
}

std::vector<bool> RemoteHudWorld::repairCells(uint32_t vehicle) const
{
	const net::RemoteObject* o = m_world.object(ghostOf(vehicle));
	return o ? o->repairCells : std::vector<bool> {};
}

namespace {

// A bundle's turn as its ghost has it: the yaw (x) or the pitch (the y's
// negative: partPose's pitch is down positive); 0 without one.
float ghostBundle(const net::RemoteWorld& world, uint16_t ghost, const std::string& part, bool yaw, bool* found = nullptr)
{
	net::Vec3f offset, rotation;
	if (part.empty() || !ghost || !world.partPose(ghost, part, offset, rotation)) {
		return 0;
	}
	if (found) {
		*found = true;
	}
	return yaw ? rotation.x : -rotation.y;
}

} // namespace

bool RemoteHudWorld::gunTurn(uint32_t vehicle, int seat, float& yaw, float& pitch) const
{
	const std::vector<game::GameWorld::SeatLayout> seats = m_round.vehicleSeats(vehicle);
	if (seat < 0 || static_cast<size_t>(seat) >= seats.size() || seats[static_cast<size_t>(seat)].weapon.empty()) {
		return false;
	}
	const game::GameWorld::SeatLayout& s = seats[static_cast<size_t>(seat)];
	const uint16_t ghost = ghostOf(vehicle);
	float turn = s.frameYaw + ghostBundle(m_world, ghost, s.yawBundle, true);
	// The turret it rides (seatMount: the carrier seat's yaw bundle).
	if (s.gunAnchor.yawSeat >= 0 && static_cast<size_t>(s.gunAnchor.yawSeat) < seats.size()
		&& s.gunAnchor.yawSeat != seat) {
		turn += ghostBundle(m_world, ghost, seats[static_cast<size_t>(s.gunAnchor.yawSeat)].yawBundle, true);
	}
	yaw = std::remainder(turn, 360.0f);
	pitch = ghostBundle(m_world, ghost, s.pitchBundle, false);
	return true;
}

bool RemoteHudWorld::seatYaw(uint32_t vehicle, int seat, float& yaw) const
{
	const std::vector<game::GameWorld::SeatLayout> seats = m_round.vehicleSeats(vehicle);
	if (seat < 0 || static_cast<size_t>(seat) >= seats.size() || seats[static_cast<size_t>(seat)].yawBundle.empty()) {
		return false;
	}
	bool found = false;
	const float turn = ghostBundle(m_world, ghostOf(vehicle), seats[static_cast<size_t>(seat)].yawBundle, true, &found);
	if (found) {
		yaw = turn;
	}
	return found;
}

std::vector<HudWorld::SquadRequest> RemoteHudWorld::squadRequests(int team) const
{
	std::vector<SquadRequest> out;
	for (const net::RemoteWorld::SquadRequest& r : m_world.squadRequests(team)) {
		out.push_back({r.team, r.squad, r.kind, {r.point.x, r.point.y, r.point.z}, 0});
	}
	return out;
}

bool RemoteHudWorld::squadOrder(int team, int squad, bool fromCommander, SquadOrder& out) const
{
	// The server's IssueSquadOrder (store 1 the commander's), its kind the
	// order type's number; on an object, at the object's ghost.
	const net::RemoteWorld::SquadOrder* o = m_world.squadOrder(team, squad, fromCommander ? 1 : 0);
	if (!o || game::GameWorld::orderTypeFromKind(o->kind) == game::GameWorld::OrderType::None) {
		return false;
	}
	out = {};
	out.type = game::GameWorld::orderTypeFromKind(o->kind);
	out.position = {o->point.x, o->point.y, o->point.z};
	if (const net::RemoteObject* on = o->object ? m_world.object(o->object) : nullptr; on && on->hasPosition) {
		out.position = {on->position.x, on->position.y, on->position.z};
	}
	out.fromCommander = fromCommander;
	out.accepted = o->accepted;
	return true;
}

const world::Object* RemoteHudWorld::soldier() const
{
	PlayerInfo me;
	if (!playerInfo(localPlayer(), me) || !me.alive || !m_world.hasControlObject() || !m_world.controlInLastBlock()) {
		return nullptr;
	}
	return m_round.objects().find(me.soldier);
}

const world::Object* RemoteHudWorld::vehicle() const
{
	if (!m_world.hasControlObject() || !m_world.controlInLastBlock() || !m_world.controlObject().vehicle) {
		return nullptr;
	}
	return m_round.objects().find(objectOf(m_world.controlObject().vehicle));
}

float RemoteHudWorld::health() const
{
	// Our player's own state (its soldier's hit points over maxHitPoints),
	// else the soldier ghost's fraction.
	if (const net::ControlState* state = m_world.controlState(); state && state->hasOwn) {
		return std::clamp(state->own.health, 0.0f, 1.0f);
	}
	const net::RemotePlayer* me = m_world.player(localPlayer());
	const net::RemoteObject* body = me ? m_world.object(me->soldierGhost) : nullptr;
	return body && body->health >= 0 ? std::clamp(body->health, 0.0f, 1.0f) : 1.0f;
}

HudWorld::Weapon RemoteHudWorld::heldWeapon() const
{
	Weapon w;
	PlayerInfo me;
	if (!playerInfo(localPlayer(), me) || !me.alive) {
		return w;
	}
	if (me.vehicle) {
		// A seat's gun (its template's weapon).
		const auto seats = m_round.vehicleSeats(me.vehicle);
		if (me.seat >= 0 && static_cast<size_t>(me.seat) < seats.size()) {
			const auto& seat = seats[static_cast<size_t>(me.seat)];
			w.weapon = m_round.templates().find(seat.weapon);
			// The server's PIFire group's active weapon (its place in the group).
			const net::ControlState* state = m_world.controlState();
			if (state && state->hasOwn && state->own.weapons[0].index >= 0) {
				int place = 0;
				for (const auto& candidate : seat.weapons) {
					if (candidate.group == 0 && place++ == state->own.weapons[0].index) {
						w.weapon = m_round.templates().find(candidate.weapon);
						break;
					}
				}
			}
		}
	} else if (const world::Object* kit = m_round.objects().find(me.kit); kit && kit->objectTemplate()) {
		// The kit's firearm in the selected slot (the soldier's mask 0x2000;
		// itemIndex, 3 the primary), as the first-person view takes it.
		const int wanted = me.inHandSlot > 0 ? me.inHandSlot : 3;
		const world::ObjectTemplate* first = nullptr;
		for (const world::ChildTemplate& child : kit->objectTemplate()->children()) {
			const world::ObjectTemplate* t = m_round.templates().find(child.name);
			if (!t || t->typeName() != "GenericFireArm") {
				continue;
			}
			if (static_cast<int>(templateNumber(t, "itemIndex", -1)) == wanted) {
				w.weapon = t;
			}
			first = first ? first : t;
		}
		w.weapon = w.weapon ? w.weapon : first;
	}
	// The rounds and magazines of the active weapon group (PlayerOwnState).
	if (const net::ControlState* state = m_world.controlState(); state && state->hasOwn) {
		for (const auto& g : state->own.weapons) {
			if (g.active && g.ammo >= 0) {
				w.rounds = g.ammo;
				w.mags = g.magazines;
				break;
			}
		}
	}
	return w;
}

void RemoteHudWorld::stepAbility(float seconds, bool fire)
{
	// The gun in hand's ReplenishingAmmoComp (0x007beac0's, on foot), as
	// GameWorld's players work it: a hub while in hand, the repair tool
	// (onlyActiveWhileFiring) while firing.
	PlayerInfo me;
	const world::ObjectTemplate* gun = nullptr;
	if (playerInfo(localPlayer(), me) && me.alive && !me.manDown && !me.vehicle) {
		gun = heldWeapon().weapon;
	}
	float radius = 0;
	bool ammo = false, whileFiring = false;
	const bool gadget = gun && game::GameWorld::gadgetScan(*gun, radius, ammo, whileFiring);
	if (!gadget || (whileFiring && !fire)) {
		m_abilityScan = {};
		return;
	}
	m_abilityScan.active = true;
	if (m_abilityScan.targets.empty() && m_abilityScan.timer == 0) {
		m_abilityScan.timer = 1; // the first scan at once
	}
	if (m_abilityScan.timer >= (m_abilityScan.targets.empty() ? 1.0f : 2.0f)) {
		m_abilityScan.targets = scanAbilityTargets(me, radius, ammo, whileFiring);
		m_abilityScan.timer = 0;
	}
	m_abilityScan.timer += seconds;
}

std::vector<uint32_t> RemoteHudWorld::scanAbilityTargets(const PlayerInfo& me, float radius, bool ammo, bool whileFiring) const
{
	std::vector<uint32_t> found;
	constexpr float kDegrees = 3.14159265f / 180.0f;
	const float yaw = me.yaw * kDegrees;
	// 0x005d7d40: of our team or of none, and one a heal may mend
	// (GameWorld::repairable's test on the ghost's hit points).
	const auto ofTeam = [&](uint32_t object) {
		const world::Object* o = m_round.objects().find(object);
		return o && (o->team() == me.team || o->team() == 0);
	};
	const auto mendable = [&](uint32_t object) {
		const world::Object* o = m_round.objects().find(object);
		const world::ObjectTemplate* t = o ? o->objectTemplate() : nullptr;
		if (!t || m_round.isTitan(object) || templateNumber(t, "armor.maxHitPoints", 0) <= 0
			|| templateNumber(t, "armor.canBeRepaired", 1) == 0) {
			return false;
		}
		return hitPoints(object) > 0 || templateNumber(t, "armor.canBeRepairedWhenWreck", 0) != 0;
	};
	if (whileFiring) {
		// 0x005d84a0: from the eyes along the look, as far as abilityRadius.
		const float pitch = std::clamp(me.aimPitch, -89.0f, 89.0f) * kDegrees;
		const physics::Vec3 eye = me.position + physics::Vec3 {0, 1.6f, 0};
		const physics::Vec3 look {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
		const uint32_t hit = m_round.firstObjectHit(eye, eye + look * radius, me.soldier);
		if (hit && !ammo && ofTeam(hit) && mendable(hit)) {
			found.push_back(hit);
		}
		return found;
	}
	// 0x005d8740: round the point a metre ahead of the hands.
	const physics::Vec3 at = me.position + physics::Vec3 {std::sin(yaw), 0.5f, std::cos(yaw)};
	const auto inReach = [&](const physics::Vec3& p) { return physics::lengthSquared(p - at) <= radius * radius; };
	for (const PlayerInfo& p : playerList()) {
		if (p.alive && !p.vehicle && p.soldier && p.team == me.team && inReach(p.position)) {
			found.push_back(p.soldier);
		}
	}
	if (!ammo) {
		for (const net::RemoteObject& o : m_world.objectList()) {
			if (o.part != 0 || !o.hasPosition || o.kind == net::NetworkableKind::Soldier) {
				continue;
			}
			const uint32_t object = objectOf(o.ghost);
			if (object && ofTeam(object) && inReach({o.position.x, o.position.y, o.position.z}) && mendable(object)) {
				found.push_back(object);
			}
		}
	}
	return found;
}

bool RemoteHudWorld::abilityBar(game::GameWorld::AbilityBarInfo& out) const
{
	PlayerInfo me;
	if (!playerInfo(localPlayer(), me)) {
		return false;
	}
	out = {};
	out.alive = me.alive && !me.manDown; // slots 0x74 and 0x114
	const net::ControlState* state = m_world.controlState();
	if (state && state->hasOwn) {
		out.healFlag = state->own.flag168;
		out.repairFlag = state->own.flag169;
		out.ammoFlag = state->own.flag16a;
	}
	if (const world::Object* kit = m_round.objects().find(me.kit); kit && kit->objectTemplate()) {
		game::GameWorld::abilityBarKit(*kit->objectTemplate(), out);
	}
	const world::ObjectTemplate* gun = me.vehicle ? nullptr : heldWeapon().weapon;
	if (!gun || !game::GameWorld::abilityBarGadget(*gun, out)) {
		return true;
	}
	// The battery (slot 0x28, 0x005d8160: the holder's soldier's +0x338).
	float battery = 0;
	if (!(ownBattery && ownBattery(battery)) && state && !state->groups.empty()
		&& state->groups.front().kind == net::NetworkableKind::Soldier && (state->groups.front().mask & 0x40000)) {
		battery = state->groups.front().battery;
	}
	out.battery = std::max(0.0f, battery);
	out.active = m_abilityScan.active && !m_abilityScan.targets.empty();
	for (const uint32_t target : m_abilityScan.targets) {
		out.targets.push_back(out.kitAmmo ? 1.0f : objectHealth(target));
	}
	return true;
}

float RemoteHudWorld::combatAreaTimeLeft() const
{
	// The server's game status (game logic +0x40: 1 playing) and our
	// player's time outside (the networkable's 0x800 part, whole seconds;
	// the HUD's slot 0xf0 test needs more than 0).
	const net::RemotePlayer* me = m_world.player(localPlayer());
	const game::CombatAreas* areas = m_round.combatAreas();
	if (m_world.gameState() != 1 || !me || !me->alive || me->outsideSeconds <= 0 || !areas || !areas->use) {
		return -1;
	}
	// The controlled object's timer (0x004cac70): a vehicle's category's,
	// else timeAllowedOutside.
	const world::Object* car = vehicle();
	const int category = car && car->objectTemplate() ? game::GameWorld::vehicleCategory(*car->objectTemplate()) : 4;
	return std::max(0.0f, areas->timerFor(category, car == nullptr) - static_cast<float>(me->outsideSeconds));
}

HudWorld::Weapon RemoteHudWorld::seatGroupWeapon(int group) const
{
	Weapon w;
	PlayerInfo me;
	const net::ControlState* state = m_world.controlState();
	if (group < 0 || group > 2 || !playerInfo(localPlayer(), me) || !me.alive || !me.vehicle || me.seat < 0 || !state
		|| !state->hasOwn) {
		return w;
	}
	const auto& own = state->own.weapons[group];
	const auto seats = m_round.vehicleSeats(me.vehicle);
	if (own.index < 0 || static_cast<size_t>(me.seat) >= seats.size()) {
		return w;
	}
	int place = 0;
	for (const auto& candidate : seats[static_cast<size_t>(me.seat)].weapons) {
		if (candidate.group == group && place++ == own.index) {
			w.weapon = m_round.templates().find(candidate.weapon);
			break;
		}
	}
	if (w.weapon) {
		w.rounds = own.ammo;
		w.mags = own.magazines;
	}
	return w;
}

std::vector<int> RemoteHudWorld::revealedPlayers(int) const
{
	return m_world.radarPlayers();
}

std::vector<uint32_t> RemoteHudWorld::spottedVehicles() const
{
	std::vector<uint32_t> out;
	for (const uint16_t ghost : m_world.radarVehicles()) {
		if (const auto it = m_ghostObjects.find(ghost); it != m_ghostObjects.end()) {
			out.push_back(it->second);
		}
	}
	return out;
}

void roundCameraEffects(game::GameWorld& round, int player, render::PostEffects& effects)
{
	game::GameWorld::ProjectileCamera camera;
	effects.tv = round.projectileCamera(player, camera);
}

} // namespace dice::hfe::host
