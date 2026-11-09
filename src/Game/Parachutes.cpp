// The soldiers' parachutes (GameWorld). Every kit carries the
// ParachuteLauncher: a GenericFireArm at itemIndex 9 with a
// SpawnObjectFireComp (template 0x94ef, component vtable 0x00973f20) whose
// projectile is the Parachute "parachute" (class 0xc5a8, object vtable
// 0x0096e518 / 0x0096e818), velocity 0, fire.projectileStartPosition 0/2/0.
//
//  - Taking it (0x004d2a50): key 9 (input 22), or the jump key (input 9,
//    bit 0x200, over 0.5) while the soldier is in the air (its slot 0x78),
//    asks for item 9; it can be taken only while its fire test passes
//    (isSelectable 0x005c1150 -> the fire component's slot 7, 0x005e4750):
//    the holder is a soldier (0xc4d7); unless *0x00a6ef84 (0 as shipped)
//    allows it anywhere, he is at least 2 m over the terrain and the water
//    and a ray 2 m down meets nothing (0x005e4330), and he falls at 10 m/s
//    or more (the velocity's y under -10); and there is room for the
//    parachute (0x005e45e0: its collision against the objects within 15 m).
//    Taken, it opens: the round leaves at the soldier's frame plus the start
//    position (0x005e4460, slot 53: 2 m over him) and its enterOnSpawn puts
//    him in. (The press that lets that round go after the choice was not
//    traced; the bots' only press is input 22, AIDLL 0x10099b70, so the
//    choice opens it here.)
//  - Entering (0x005cfaf0 after the PCO's 0x005cdd60): a parachute that
//    isn't a pod (+0x4cc) is set upright about its heading (0x006ad2a0's
//    angles, 0x005cfa40). The rider hangs at its seatInformation, visible
//    (seatInformation names the parachute itself: 0x005cd4f0 finds it,
//    0x005c6670, so 0x005cdd60 seats him on it rather than disabling him)
//    and posed by 3p_parachute_animSys.
//  - Its node is a Mesh rigid body (mass 200, otherCollisionLod 2) under
//    gravity, carried by parachute_WingMiddle (wingLift / flapLift 120, its
//    pitch on PIThrottle within -1.5..2.5, automaticReset), pulled straight
//    down by parachute_ForceDown (constantForce 12, useAbsoluteDirection,
//    20 m below) and turned by parachute_RotateForceObject (rotationalForce
//    0.5/0/0 on PIYaw): Vehicle::fromTemplate's flightParts, ticked with the
//    body (Body::canopy).
//  - Landing (the collision handler 0x005cf750, server, once): a contact
//    with the ground, or with an object along a normal at least 0.3 up
//    (*0x00a0200c; any normal when the other is under a quarter of its
//    vehicle-LOD radius), starts the landing timer (+0x45c) at 0.05 s
//    (*0x00a02004 / *0x00a02008, water or not). Its update (0x005d01c0)
//    runs the timer down; at 0 (0x005cff20) the rider is put out
//    (DAT_00a1b5d8 slot 0xac), the object leaves the renderer's and the
//    collision's lists (updateFlags (0, 0x102)) and 0.1 s later (timer
//    event 0x21) the soldier takes the parachute's velocity (0x005cff90)
//    and the parachute is taken away (DAT_00a1b5d8 slot 0xa4).
//  - Its animation state (timer event 0x20, 0x005cfbb0) for the rider's and
//    the canopy's systems: 4 once landed, else 1 while 8 m or more
//    (*0x00a02000) over the terrain, 2 below.
#include "GameWorld.h"

#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dice::hfe::game {

namespace {

constexpr float kOpenHeight = 2.0f;    // 0x005e4330: over the terrain and the water, and the ray's length
constexpr float kOpenFallSpeed = -10;  // 0x005e4750: the velocity's y
constexpr float kStillHeight = 8.0f;   // *0x00a02000 (static_init 0x008e42e0)
constexpr float kLandingTime = 0.05f;  // *0x00a02004 / *0x00a02008 (0x008e42f0 / 0x008e4300)
constexpr float kLandingNormal = 0.3f; // *0x00a0200c (0x008e4310)
constexpr float kRemoveDelay = 0.1f;   // timer event 0x21's 0x3dcccccd (0x005cff20)

const std::string* setting(const world::ObjectTemplate& t, const char* name)
{
	const auto it = t.settings().find(name);
	return it == t.settings().end() ? nullptr : &it->second;
}

float number(const world::ObjectTemplate& t, const char* name, float fallback)
{
	const std::string* value = setting(t, name);
	return value ? static_cast<float>(std::atof(value->c_str())) : fallback;
}

bool sameName(const std::string& a, const std::string& b)
{
	const world::CaseInsensitiveLess less;
	return !less(a, b) && !less(b, a);
}

physics::Vec3 vec3Of(const std::string& text)
{
	physics::Vec3 v;
	std::sscanf(text.c_str(), "%f/%f/%f", &v.x, &v.y, &v.z);
	return v;
}

bool hasComponent(const world::ObjectTemplate& t, const char* component)
{
	const auto& components = t.components();
	return std::any_of(components.begin(), components.end(), [&](const std::string& c) { return sameName(c, component); });
}

// A soldier's parachute: a Parachute that isn't a drop pod.
bool soldierParachute(const world::ObjectTemplate& t)
{
	return sameName(t.typeName(), "Parachute") && number(t, "isPod", 0) == 0;
}

} // namespace

GameWorld::Parachute* GameWorld::findParachute(uint32_t object)
{
	for (Parachute& chute : m_parachutes) {
		if (chute.object == object) {
			return &chute;
		}
	}
	return nullptr;
}

bool GameWorld::ridingParachute(int player) const
{
	return std::any_of(m_parachutes.begin(), m_parachutes.end(), [player](const Parachute& c) { return c.player == player; });
}

bool GameWorld::soldierFalling(int player) const
{
	const Player* p = findPlayer(player);
	return p && p->alive && !p->manDown && !p->vehicle && p->body.velocity.y < kOpenFallSpeed;
}

std::vector<GameWorld::ParachuteInfo> GameWorld::parachutes() const
{
	std::vector<ParachuteInfo> list;
	for (const Parachute& chute : m_parachutes) {
		list.push_back({chute.object, chute.player, chute.state});
	}
	return list;
}

bool GameWorld::parachuteLauncher(const Player& player, std::string& projectile, physics::Vec3& start) const
{
	for (const Player::Carried& item : player.carried) {
		const world::ObjectTemplate* t = item.slot == 9 ? m_templates.find(item.name) : nullptr;
		if (!t || !hasComponent(*t, "SpawnObjectFireComp")) {
			continue;
		}
		const std::string* name = setting(*t, "projectileTemplate");
		const world::ObjectTemplate* round = name ? m_templates.find(*name) : nullptr;
		if (!round || !soldierParachute(*round)) {
			continue;
		}
		projectile = *name;
		const std::string* from = setting(*t, "fire.projectileStartPosition");
		start = from ? vec3Of(*from) : physics::Vec3 {};
		return true;
	}
	return false;
}

// What a soldier stands on: the static objects' and the bodies' soldier
// LODs (a Titan's decks, its team's forcefields), and what blocks a round
// (lineOfSight: the projectile LODs, the terrain).
bool GameWorld::soldierRayClear(int team, const physics::Vec3& from, const physics::Vec3& to, uint32_t ignore) const
{
	float fraction = 1;
	return !soldierRayHit(team, from, to, ignore, fraction) && lineOfSight(from, to, ignore);
}

// The nearest soldier face along the segment (the static objects' and the
// bodies' soldier LODs), its fraction 0..1.
bool GameWorld::soldierRayHit(int team, const physics::Vec3& from, const physics::Vec3& to, uint32_t ignore, float& fraction) const
{
	bool any = false;
	fraction = 1;
	physics::CollisionWorld::Hit hit {};
	if (m_collision.raycast(from, to, hit)) {
		any = true;
		fraction = std::min(fraction, hit.fraction);
	}
	for (const Body& body : m_bodies) {
		const physics::CollisionWorld* mesh = body.soldierMesh.get();
		if ((team == 1 || team == 2) && body.teamSoldierMesh[team - 1]) {
			mesh = body.teamSoldierMesh[team - 1].get();
		}
		if (!mesh || body.object == ignore) {
			continue;
		}
		const physics::Vec3 a = body.body.rotation.untransform(from - body.body.position);
		const physics::Vec3 b = body.body.rotation.untransform(to - body.body.position);
		if (mesh->raycast(a, b, hit)) {
			any = true;
			fraction = std::min(fraction, hit.fraction);
		}
	}
	return any;
}

// 0x005e4750 with 0x005e4330 and 0x005e45e0.
bool GameWorld::mayOpenParachute(const Player& player, const std::string& projectile, const physics::Vec3& start) const
{
	if (!player.alive || player.manDown || player.vehicle || player.ladder >= 0 || !m_ground.height) {
		return false;
	}
	const world::Object* soldier = m_objects.find(player.object);
	if (!soldier) {
		return false;
	}
	const world::Vec3 at = soldier->position();
	float below = m_ground.height(at.x, at.z);
	if (m_ground.waterLevel) {
		below = std::max(below, m_ground.waterLevel(at.x, at.z));
	}
	if (at.y < below + kOpenHeight || player.body.velocity.y > kOpenFallSpeed) {
		return false;
	}
	// The ray 2 m down (the scene's slot 0x68 with the soldier's own root
	// left out): the static objects, the vehicles and the Titans' decks.
	const physics::Vec3 feet {at.x, at.y, at.z};
	const auto clear = [&](const physics::Vec3& from, const physics::Vec3& to) {
		return soldierRayClear(player.team, from, to, player.object);
	};
	if (!clear(feet, feet - physics::Vec3 {0, kOpenHeight, 0})) {
		return false;
	}
	return parachuteRoom(player, projectile, start);
}

// 0x005e45e0: without the round's collision mesh (0x1a4 cleared once the
// manager has none) there is room. Else the objects within 15 m of the
// soldier (the scene's slot 0x44, radius 15, with flag 0x100, 0x006f4b60,
// the soldier left out; the canopy's few metres stay inside it), projectiles (0x9493) aside, are asked 0x006f2e90 with the
// mesh in the round's frame (the fire comp's slot 0xd4, 0x005e4460: the
// soldier's heading at his feet plus projectileStartPosition, where
// openParachute makes it): one with a mesh response (0xc42d, 0xc430,
// 0xc431) and a soldier LOD (slot 0xc4(2)) that a segment from that point
// to any vertex of the mesh's vehicle LOD (type 1) crosses (0x006f2760, slot
// 0x58 both sides of the faces) leaves no room. The legs' probe
// (0x006f28e0) isn't asked (its argument is 0). The static objects' and
// the bodies' soldier LODs answer here (soldierRayHit, team 0: the plain
// soldier LOD).
bool GameWorld::parachuteRoom(const Player& player, const std::string& projectile, const physics::Vec3& start) const
{
	const world::ObjectTemplate* round = m_templates.find(projectile);
	const world::Object* soldier = m_objects.find(player.object);
	if (!round || !soldier) {
		return true;
	}
	const std::vector<physics::Vec3> vertices = vehicleLodVertices(*round);
	const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({player.body.yawDegrees, 0, 0});
	const physics::Vec3 feet {soldier->position().x, soldier->position().y, soldier->position().z};
	return canopyClear(feet + turn.transform(start), turn, vertices, [&](const physics::Vec3& a, const physics::Vec3& b) {
		float fraction = 1;
		return soldierRayHit(0, a, b, player.object, fraction);
	});
}

bool canopyClear(const physics::Vec3& from, const physics::Mat3& turn, const std::vector<physics::Vec3>& vertices,
	const std::function<bool(const physics::Vec3&, const physics::Vec3&)>& crosses)
{
	return std::none_of(vertices.begin(), vertices.end(), [&](const physics::Vec3& v) { return crosses(from, from + turn.transform(v)); });
}

bool GameWorld::openParachute(Player& player)
{
	std::string projectile;
	physics::Vec3 start;
	if (!m_files || !parachuteLauncher(player, projectile, start) || !mayOpenParachute(player, projectile, start)) {
		return false;
	}
	world::ObjectTemplate* t = m_templates.find(projectile);
	world::Object* soldier = m_objects.find(player.object);
	if (!t || !soldier) {
		return false;
	}
	// 0x005e4460: the soldier's frame (his heading) plus the start position.
	const float yaw = player.body.yawDegrees;
	const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({yaw, 0, 0});
	const physics::Vec3 feet {soldier->position().x, soldier->position().y, soldier->position().z};
	const physics::Vec3 from = feet + turn.transform(start);
	world::Object* made = m_objects.create(t, {from.x, from.y, from.z}, {yaw, 0, 0});
	if (!made) {
		return false;
	}
	made->setTeam(player.team);
	addBodies();
	Body* body = findBody(made->id());
	if (!body) {
		m_objects.remove(made->id());
		return false;
	}
	// The round takes its holder's velocity (velocity 0 of its own), as the
	// pods take their launcher's.
	body->body.velocity = player.body.velocity;
	wake(*body);
	Parachute chute;
	chute.object = made->id();
	chute.player = player.id;
	m_parachutes.push_back(chute);
	// enterOnSpawn: seat 0 (0x005cfaf0 -> 0x005cdd60).
	body->occupants.assign(1, player.id);
	player.vehicle = made->id();
	player.seat = 0;
	player.kitGeometry = -1;
	player.seatBodyYaw = player.seatTwist = 0;
	player.lastLookPitch = player.input.aimPitch;
	if (world::Object* held = player.weaponObject ? m_objects.find(player.weaponObject) : nullptr) {
		held->setHidden(true);
	}
	raise("EnterVehicle", {ScriptValue::player(player.id), ScriptValue::object(made->id())});
	return true;
}

// The rider: his input steers the canopy (the parachute's PIThrottle wing,
// its PIYaw turn), he hangs at the seat, posed by the seat's system.
void GameWorld::rideParachute(Player& player, world::Object& soldier, Body& body, float seconds)
{
	const world::Object* object = m_objects.find(body.object);
	const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr;
	if (body.canopy) {
		body.canopy->setInput(driverInput(player.input, 1.0f, player.lastLookPitch));
	}
	player.lastLookPitch = player.input.aimPitch;
	SeatLayout seat;
	if (t) {
		if (const std::string* info = setting(*t, "seatInformation")) {
			char name[128] = {}, position[64] = {}, rotation[64] = {};
			std::sscanf(info->c_str(), "%127s %63s %63s", name, position, rotation);
			seat.position = vec3Of(position);
			seat.rotation = vec3Of(rotation);
			// "Parachute": the parachute itself (0x005cd4f0's search by
			// template name), so he is shown under it.
			seat.seatParent = name;
			seat.seatShown = sameName(seat.seatParent, t->name());
		}
		if (const std::string* system = setting(*t, "seatAnimationSystem")) {
			seat.animationSystem = *system;
		}
	}
	const physics::Vec3 at = body.body.position + body.body.rotation.transform(seat.position);
	player.body.position = at + physics::Vec3 {0, 1, 0};
	player.body.velocity = body.body.velocity;
	player.body.yawDegrees = body.body.rotation.yawPitchRoll().x;
	const Parachute* chute = findParachute(body.object);
	player.seatMessages = chute ? static_cast<uint32_t>(chute->state) : 0u;
	poseSeated(player, soldier, body, seat, seconds);
}

// 0x005cf750's landing test.
void GameWorld::parachuteCollision(uint32_t self, uint32_t other, const physics::Vec3& normal, uint32_t otherMaterial)
{
	(void)otherMaterial;
	Parachute* chute = findParachute(self);
	Pod* pod = nullptr;
	for (Pod& candidate : m_pods) {
		if (candidate.object == self) {
			pod = &candidate;
			break;
		}
	}
	float* landing = chute ? &chute->landing : pod ? &pod->landing : nullptr;
	const float removeIn = chute ? chute->removeIn : pod ? pod->removeIn : -1;
	if (!landing || *landing >= 0 || removeIn >= 0) {
		return;
	}
	if (other != 0) {
		// The normal's height, or 1 when the other is under a quarter of
		// its vehicle-LOD radius (slot 0x50(1)); the radii here are the
		// bodies' (Body::objectRadius), 1 for an object without one.
		const Body* own = findBody(self);
		const Body* theirs = findBody(other);
		float up = normal.y;
		if (own && 4 * (theirs ? theirs->objectRadius : 1.0f) < own->objectRadius) {
			up = 1;
		}
		if (up < kLandingNormal) {
			return;
		}
		// A moving object (its root's flag 8 with a node over 5 m/s, or
		// without one) doesn't land it.
		if (theirs && physics::lengthSquared(theirs->body.velocity) > 25) {
			return;
		}
	}
	if (pod) {
		pod->landingNormal = other ? normal : physics::Vec3 {0, 1, 0};
		const Body* surface = other ? findBody(other) : nullptr;
		pod->landingSurfaceVelocity = surface ? surface->body.velocity : physics::Vec3 {};
	}
	*landing = kLandingTime;
}

void GameWorld::leaveParachute(Player& player, Parachute& chute)
{
	chute.player = -1;
	Body* body = findBody(chute.object);
	world::Object* soldier = m_objects.find(player.object);
	if (body) {
		for (int& occupant : body->occupants) {
			occupant = occupant == player.id ? -1 : occupant;
		}
		if (body->canopy) {
			body->canopy->setInput({});
		}
	}
	if (player.vehicle != chute.object) {
		return;
	}
	player.vehicle = 0;
	player.seat = -1;
	player.kitGeometry = -1;
	player.seatMessages = 0;
	player.seatAnimation.reset();
	player.seatAnimationPath.clear();
	if (player.skeleton) {
		player.pose = anim::setupPose(*player.skeleton);
	}
	player.control.reset();
	if (soldier) {
		soldier->setHidden(false);
	}
	if (world::Object* held = player.weaponObject ? m_objects.find(player.weaponObject) : nullptr) {
		held->setHidden(false);
	}
	if (!body) {
		return;
	}
	// setSoldierExitLocation (0.044/-1.607/0.261 under the canopy), on the
	// ground at least; his velocity the parachute's (0x005cff90).
	const world::Object* object = m_objects.find(chute.object);
	physics::Vec3 exit {0, -1.6f, 0};
	if (const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr) {
		if (const std::string* location = setting(*t, "soldierExitLocation")) {
			exit = vec3Of(*location);
		} else if (const std::string* position = setting(*t, "soldierExitPosition")) {
			exit = vec3Of(*position);
		}
	}
	physics::Vec3 at = body->body.position + body->body.rotation.transform(exit);
	at.y = std::max(at.y, m_ground.height ? m_ground.height(at.x, at.z) : at.y);
	// A floor between the canopy and the place (a Titan's deck the
	// parachute came down on) holds him on it: the rewrite's soldier
	// doesn't climb out of a floor he is put under.
	const physics::Vec3 top {at.x, body->body.position.y, at.z};
	float fraction = 1;
	if (top.y > at.y && soldierRayHit(player.team, top, at, player.object, fraction)) {
		at.y = top.y + (at.y - top.y) * fraction;
	}
	at.y += 1.0f;
	player.body.position = at;
	player.body.velocity = body->body.velocity;
	player.body.yawDegrees = body->body.rotation.yawPitchRoll().x;
	if (soldier) {
		soldier->setPosition({at.x, at.y - 1.0f, at.z});
		soldier->setRotation({player.body.yawDegrees, 0, 0});
	}
	raise("ExitVehicle", {ScriptValue::player(player.id), ScriptValue::object(chute.object)});
}

void GameWorld::stepParachutes(float seconds)
{
	for (auto it = m_parachutes.begin(); it != m_parachutes.end();) {
		Parachute& chute = *it;
		Body* body = findBody(chute.object);
		Player* rider = chute.player >= 0 ? findPlayer(chute.player) : nullptr;
		if (rider && (!rider->alive || rider->vehicle != chute.object)) {
			leaveParachute(*rider, chute); // dead: the canopy comes down empty
			rider = nullptr;
		}
		if (!body) {
			if (rider) {
				leaveParachute(*rider, chute);
			}
			it = m_parachutes.erase(it);
			continue;
		}
		// 0x005cfbb0: the state, by the height over the terrain.
		const physics::Vec3 p = body->body.position;
		const float ground = m_ground.height ? m_ground.height(p.x, p.z) : 0.0f;
		chute.state = chute.landing >= 0 || chute.removeIn >= 0 ? 4 : p.y - ground >= kStillHeight ? 1 : 2;
		// 0x005d01c0: the landing timer; at 0 the rider is out (0x005cff20).
		if (chute.landing > 0) {
			chute.landing -= seconds;
			if (chute.landing <= 0) {
				chute.landing = -1;
				if (rider) {
					leaveParachute(*rider, chute);
				}
				chute.removeIn = kRemoveDelay;
			}
		}
		// The timer event 0x21: gone.
		if (chute.removeIn >= 0) {
			chute.removeIn -= seconds;
			if (chute.removeIn <= 0) {
				m_objects.remove(chute.object);
				it = m_parachutes.erase(it);
				continue;
			}
		}
		++it;
	}
}

} // namespace dice::hfe::game
