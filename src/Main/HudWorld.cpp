#include "HudWorld.h"

#include "Game/GameLogic.h"
#include "Game/Weapons.h"
#include "Net/RemoteWorld.h"
#include "World/Object.h"
#include "World/ObjectTemplate.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
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
	out.kills = p.kills;
	out.deaths = p.deaths;
	out.teamKills = p.teamKills;
	out.squad = p.squad;
	out.squadLeader = p.squadLeader;
	out.commander = p.commander;
	out.rank = p.rank;
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
		out.yaw = body->yaw;
		out.aimPitch = body->aimPitch;
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
			w.weapon = m_round.templates().find(seats[static_cast<size_t>(me.seat)].weapon);
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

} // namespace dice::hfe::host
