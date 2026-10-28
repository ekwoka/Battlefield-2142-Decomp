// Kits on the ground.
//
// A soldier's kit is an object of its own (class 0x9496) that the soldier
// holds (+0x228). What the original does with it:
//
//  - The drop (0x00589890, Soldier.cpp): the kit's items go back into it
//    (slot 0xf0 for each weapon), the kit is let go by the soldier (kit
//    interface 0xc49c slot 0x14, 0x0056d0a0) and a ray from the soldier's
//    position 1000 m straight down (the scene's slot 0x68, flags 0x1000100,
//    the soldier itself left out) places it where it meets something
//    (slot 100); the Titan records are asked whether that is a Titan's
//    (0x00561b20: 0x005aa030 compares the hit object's root with each
//    team's Titan; 0x005ad2e0 then keeps {kit, kit position - Titan
//    position}). A soldier drops his kit when he dies and when he picks up
//    another (0x0058b680 calls the drop first).
//  - Its life on the ground: once let go it is enabled as a kit lying
//    about (0x0056cf90 for the game mode 0x1d4c1) and the collision
//    manager's timer (slot 0xd0, event 0x1d) removes it
//    timeToLiveAfterDeath (kit template +0x2b8; 30 s by default, the
//    template constructor 0x00570080; the shipped kits leave it) later.
//  - The pick-up (ServerGameLogic::pickUpClosestKit, 0x004bcd80): the
//    closest kit within the radius (the scene's sphere query slot 0x44
//    with flags 0x200, a kit without a soldier holding it) whose kit
//    interface's slot 0x24 matches the holder's slot 0x20 (the soldier
//    template's +0x2bc against the kit template's +0x2a4, neither set by a
//    shipped script: every kit matches) goes to the game logic's
//    handlePickup (slot 0xf0, 0x004bb350 -> the soldier's 0x0058c650 ->
//    0x0058b680): the soldier drops his own kit, takes the new one's
//    items and selects the first from slot 3 up (its primary), takes its
//    battery (+0x1bc) and the player's kit time stamp is set (+0x230). The
//    Titan record lets the kit go (0x00561b80 from the kit's 0x005718d0).
//    It is asked for:
//      * by the use key (the player's input flag 4 over 0.5, 0x004bda00):
//        when the soldier lives, no vehicle takes him (0x004bce30) and 2 s
//        have passed since the time stamp (player slot 0x234), within 3 m;
//      * by a revive (the soldier's event 0x19, 0x00587b90): within 10 m,
//        else a new kit of the player's choice (0x004bfcc0).
//  - The Titan's tick (0x00561e20; also 0x0043f170 -> 0x00561bb0) puts each
//    kit it carries at the Titan's position plus its offset (0x005aa5b0).
//
// The kits' contents are the player's `carried` list with each weapon's
// state (its rounds), as the original's items keep theirs.
#include "GameWorld.h"

#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dice::hfe::game {

namespace {

constexpr float kDropRay = 1000;        // 0x00589890: (0, -1000, 0)
constexpr float kDefaultKitLife = 30;   // 0x00570080: +0x2b8 = 30.0
constexpr float kUseRadius = 3;         // 0x004bda00: 3.0
constexpr double kUseInterval = 2;      // 0x004bda00: 2.0 s since the time stamp
constexpr float kReviveRadius = 10;     // 0x00587b90: 10.0

float templateNumber(const world::ObjectTemplate& t, const char* name, float fallback)
{
	const auto it = t.settings().find(name);
	return it == t.settings().end() ? fallback : static_cast<float>(std::atof(it->second.c_str()));
}

} // namespace

std::vector<uint32_t> GameWorld::droppedKits() const
{
	std::vector<uint32_t> list;
	for (const DroppedKit& kit : m_droppedKits) {
		list.push_back(kit.object);
	}
	return list;
}

std::vector<uint32_t> GameWorld::objectsOutOfWorld(float limit) const
{
	std::vector<uint32_t> list;
	for (const auto& owned : m_objects.objects()) {
		const world::Vec3& p = owned->position();
		if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || std::fabs(p.x) > limit
			|| std::fabs(p.y) > limit || std::fabs(p.z) > limit) {
			list.push_back(owned->id());
		}
	}
	return list;
}

bool GameWorld::isDroppedKit(uint32_t object) const
{
	return std::any_of(
		m_droppedKits.begin(), m_droppedKits.end(), [object](const DroppedKit& kit) { return kit.object == object; });
}

void GameWorld::dropKit(Player& player)
{
	world::Object* kit = player.kitObject ? m_objects.find(player.kitObject) : nullptr;
	const world::Object* soldier = m_objects.find(player.object);
	if (!kit) {
		player.kitObject = 0;
		return;
	}
	raise("DropKit", {ScriptValue::player(player.id), ScriptValue::object(player.kitObject)});
	DroppedKit dropped;
	dropped.object = player.kitObject;
	dropped.team = player.team;
	// Its items with what they hold: the one in hand goes back in first.
	if (player.inHand >= 0 && static_cast<size_t>(player.inHand) < player.carried.size() && player.weapon) {
		player.carried[static_cast<size_t>(player.inHand)].weapon = std::move(player.weapon);
	}
	dropped.carried = std::move(player.carried);
	dropped.inHand = player.inHand;
	dropped.battery = player.battery;
	dropped.abilityModifier = player.abilityModifier;
	const world::ObjectTemplate* t = kit->objectTemplate();
	dropped.removeAt = m_time + (t ? templateNumber(*t, "timeToLiveAfterDeath", kDefaultKitLife) : kDefaultKitLife);
	player.carried.clear();
	player.inHand = -1;
	player.weapon.reset();
	player.weaponTemplate.clear();
	player.battery = -1;
	if (player.weaponObject) {
		m_effects.releaseAnchor(player.weaponObject);
		m_objects.remove(player.weaponObject);
		player.weaponObject = 0;
	}
	player.kitObject = 0;

	// Straight down from the soldier: the nearest of the static objects',
	// the bodies' (a Titan's decks among them) and the terrain.
	const world::Vec3 at = soldier ? soldier->position() : kit->position();
	const physics::Vec3 from {at.x, at.y, at.z};
	const physics::Vec3 to {at.x, at.y - kDropRay, at.z};
	float nearest = 2;
	uint32_t hitObject = 0;
	physics::CollisionWorld::Hit hit {};
	if (m_projectileCollision.raycast(from, to, hit)) {
		nearest = hit.fraction;
	}
	for (const Body& body : m_bodies) {
		if (!body.projectileMesh || body.object == player.object) {
			continue;
		}
		const physics::Vec3 localFrom = body.body.rotation.untransform(from - body.body.position);
		const physics::Vec3 localTo = body.body.rotation.untransform(to - body.body.position);
		if (body.projectileMesh->raycast(localFrom, localTo, hit) && hit.fraction < nearest) {
			nearest = hit.fraction;
			hitObject = body.object;
		}
	}
	if (m_ground.height) {
		const float ground = (at.y - m_ground.height(at.x, at.z)) / kDropRay;
		if (ground >= 0 && ground < nearest) {
			nearest = ground;
			hitObject = 0;
		}
	}
	if (nearest <= 1) {
		kit->setPosition({at.x, at.y - kDropRay * nearest, at.z});
		// 0x00561b20: on a Titan, it rides with it.
		if (const Titan* titan = hitObject ? titanByObject(hitObject) : nullptr) {
			if (const world::Object* root = m_objects.find(titan->object)) {
				dropped.titan = titan->object;
				dropped.offset = {kit->position().x - root->position().x, kit->position().y - root->position().y,
					kit->position().z - root->position().z};
			}
		}
	}
	kit->setHidden(false);
	m_droppedKits.push_back(std::move(dropped));
}

bool GameWorld::pickUpClosestKit(Player& player, float radius)
{
	const world::Object* soldier = m_objects.find(player.object);
	if (!soldier || !player.alive) {
		return false;
	}
	const world::Vec3& p = soldier->position();
	auto closest = m_droppedKits.end();
	float best = radius * radius;
	for (auto it = m_droppedKits.begin(); it != m_droppedKits.end(); ++it) {
		const world::Object* kit = m_objects.find(it->object);
		if (!kit) {
			continue;
		}
		const world::Vec3& k = kit->position();
		const float d = (k.x - p.x) * (k.x - p.x) + (k.y - p.y) * (k.y - p.y) + (k.z - p.z) * (k.z - p.z);
		if (d < best) {
			best = d;
			closest = it;
		}
	}
	if (closest == m_droppedKits.end()) {
		return false;
	}
	DroppedKit taken = std::move(*closest);
	m_droppedKits.erase(closest);
	// 0x0058b680: his own kit goes first (0x00589890).
	if (player.kitObject) {
		dropKit(player);
	}
	player.kitObject = taken.object;
	if (world::Object* kit = m_objects.find(taken.object)) {
		kit->setHidden(true);
		kit->setPosition(soldier->position());
	}
	player.carried = std::move(taken.carried);
	player.inHand = -1;
	player.battery = taken.battery;
	player.abilityModifier = taken.abilityModifier;
	// The first item from slot 3 up (the primary), else the first.
	int slot = -1;
	for (const Player::Carried& c : player.carried) {
		if (c.slot >= 3 && (slot < 0 || c.slot < slot)) {
			slot = c.slot;
		}
	}
	if (slot < 0 && !player.carried.empty()) {
		slot = player.carried.front().slot;
	}
	if (slot >= 0) {
		equip(player, slot);
	}
	player.kitTimeStamp = m_time;
	raise("PickupKit", {ScriptValue::player(player.id), ScriptValue::object(player.kitObject)});
	return true;
}

// Each tick: kits whose time is up go, and those on a Titan ride with it.
void GameWorld::stepDroppedKits()
{
	for (auto it = m_droppedKits.begin(); it != m_droppedKits.end();) {
		world::Object* kit = m_objects.find(it->object);
		if (!kit || m_time >= it->removeAt) {
			if (kit) {
				m_hitPoints.erase(it->object);
				m_objects.remove(it->object);
			}
			it = m_droppedKits.erase(it);
			continue;
		}
		if (it->titan) {
			if (const world::Object* titan = m_objects.find(it->titan)) {
				const world::Vec3& at = titan->position();
				kit->setPosition({at.x + it->offset.x, at.y + it->offset.y, at.z + it->offset.z});
			} else {
				it->titan = 0;
			}
		}
		++it;
	}
}

} // namespace dice::hfe::game
