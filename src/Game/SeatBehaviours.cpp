// The vehicles' and seats' template behaviours with a state of their own,
// stepped once a tick after the players (GameWorld::stepSeatBehaviours):
//   - automaticYawStabilization (RotationalBundle +0x308; its matrix update
//     0x00564eb0): the bundle's yaw is posed less its parent's heading
//     against the root's, so a gun on a turret keeps the hull's heading
//     while the turret turns (the tanks' cupolas, the walkers' AA cannons);
//   - the rolling-surface loop (groundContactVolume, PlayerControlObject
//     +0x474; the contact sound object 0x005c64a0 stepped by the PCO's
//     update 0x005c8e70);
//   - the heavy IFV's shield cells (armor.mirrorParentDamageLevel,
//     hudRepairCellNo: the armour's update 0x00654d80, the HUD's
//     0x007a1e50).
#include "GameWorld.h"

#include "Physics/MaterialManager.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>

namespace dice::hfe::game {

namespace {

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

bool hasComponent(const world::ObjectTemplate& t, const char* component)
{
	const auto& components = t.components();
	return std::any_of(components.begin(), components.end(), [&](const std::string& c) { return sameName(c, component); });
}

constexpr float kEpsilon = 1.1920929e-07f; // FLT_EPSILON, the sound object's tests

} // namespace

void GameWorld::stepSeatBehaviours(float seconds)
{
	for (Body& body : m_bodies) {
		if (!m_objects.find(body.object)) {
			continue;
		}
		stabilizeGunYaws(body);
		if (body.vehicle) {
			stepRollingSound(body);
			stepShieldCells(body, seconds);
		}
	}
	for (auto it = m_rolling.begin(); it != m_rolling.end();) {
		it = findBody(it->first) ? std::next(it) : m_rolling.erase(it);
	}
}

// 0x00564eb0: with automaticYawStabilization the bundle's matrix is made
// from its angles less the heading of its parent against the root's (the
// asin of the root's right on the parent's forward, 180 less that when the
// forwards point apart): the gunner's input keeps turning the bundle's own
// angle (+0x188), the turn the parent made is taken off what is posed. The
// rewrite keeps the posed angle (Body::Gun::yaw, which the posing, the aim
// and the fire read): each tick it takes off what the parent's heading
// (the seat's gun mount: the turret it rides, the walker's torso bone)
// moved since the last, on manned and empty seats alike, as the matrix
// update runs for every bundle.
void GameWorld::stabilizeGunYaws(Body& body)
{
	if (body.guns.empty()) {
		return;
	}
	const world::Object* object = m_objects.find(body.object);
	if (!object || !object->objectTemplate()) {
		return;
	}
	const VehicleLayout& layout = vehicleLayout(*object->objectTemplate());
	for (size_t i = 0; i < layout.seats.size() && i < body.guns.size(); ++i) {
		const SeatLayout& seat = layout.seats[i];
		if (!seat.yawStabilized) {
			continue;
		}
		Body::Gun& gun = body.guns[i];
		const float heading = seatMount(body, layout, seat.gunAnchor).turn.yawPitchRoll().x;
		const float moved = std::remainder(heading - gun.stabilizedHeading, 360.0f);
		gun.yaw = std::remainder(gun.yaw - moved, 360.0f);
		gun.stabilizedHeading = heading;
	}
}

// The rolling loop of one vehicle (0x005c8e70's sound part, on a client:
// the object made by 0x005cd4f0 for a root PlayerControlObject with
// groundContactVolume over FLT_EPSILON, kept at +0x30c; the volume field
// it keeps at +0x14 is read by nothing found, so the shipped 1 only turns
// it on):
//   - the root's speed, (|v| - 0.1) / 30 within 0..1 (+0x304, which the
//     interior loops read too);
//   - the first WheelEffectBundle's ground material (its +0x188, -1 while
//     its wheel is off the ground), or the second's when that is under 1
//     (0x006bf900; -2 without a second): on the ground at -1 or more; a
//     new material or bundle material while on it marks the object changed;
//   - the volume wanted is the speed's on the ground, else 0, and the one
//     played eases to it by 0.08 a tick ("pco-gcm-factor", 0x006aff70: by
//     no more than the gap, 0 under 0.0001);
//   - 0x005c65d0: after a choice it waits 20 ticks (*0x00a6c9a4); then,
//     changed and audible, it chooses the sound again (0x005c6510: the
//     material cell (the bundle's, the ground's) sound 0, else (bundle's,
//     0)'s, through MaterialManager slot 0x70 = 0x0060a670; a new one
//     replaces the old, its envelopes on the eased volume) and plays it;
//     silent, it pauses; audible and not playing, it plays.
void GameWorld::stepRollingSound(Body& body)
{
	const world::Object* object = m_objects.find(body.object);
	const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr;
	if (!t || !sameName(t->typeName(), "PlayerControlObject") || number(*t, "groundContactVolume", 0) <= kEpsilon) {
		return;
	}
	auto [it, made] = m_rolling.try_emplace(body.object);
	Rolling& r = it->second;
	if (made) {
		// The first two WheelEffectBundles of the tree, depth first, and
		// the wheel each hangs under (its spring among its ancestors).
		const auto& wheels = body.vehicle->wheels();
		std::function<void(const world::ObjectTemplate&, int, int)> find = [&](const world::ObjectTemplate& node, int wheel,
																		  int depth) {
			for (const world::ChildTemplate& child : node.children()) {
				const world::ObjectTemplate* c = m_templates.find(child.name);
				if (!c || depth > 8 || r.bundles.size() >= 2) {
					continue;
				}
				int under = wheel;
				for (size_t w = 0; w < wheels.size(); ++w) {
					if (!wheels[w].spring.empty() && sameName(wheels[w].spring, c->name())) {
						under = static_cast<int>(w);
					}
				}
				if (sameName(c->typeName(), "WheelEffectBundle")) {
					r.bundles.push_back({static_cast<uint32_t>(std::max(0.0f, number(*c, "material", 0))), under});
				}
				find(*c, under, depth + 1);
			}
		};
		find(*t, -1, 0);
	}
	// The ground under a bundle's wheel while it touches (its +0x188).
	const auto groundOf = [&](size_t index) -> int {
		if (index >= r.bundles.size()) {
			return -2;
		}
		const Rolling::Bundle& b = r.bundles[index];
		const auto& wheels = body.vehicle->wheels();
		if (b.wheel < 0 || static_cast<size_t>(b.wheel) >= wheels.size() || !wheels[static_cast<size_t>(b.wheel)].contact) {
			return -1;
		}
		const physics::Vec3 at = body.body.position + body.body.rotation.transform(wheels[static_cast<size_t>(b.wheel)].mount);
		return m_ground.material ? static_cast<int>(m_ground.material(at.x, at.z)) : 0;
	};
	const float speed = std::clamp((std::sqrt(physics::lengthSquared(body.body.velocity)) - 0.1f) * (1.0f / 30.0f), 0.0f, 1.0f);
	int ground = groundOf(0);
	size_t from = 0;
	if (ground < 1) {
		ground = groundOf(1);
		from = 1;
	}
	r.contact = ground > -1;
	const uint32_t own = from < r.bundles.size() ? r.bundles[from].material : 0;
	if (r.contact && (r.ground != ground || r.own != own)) {
		r.ground = ground;
		r.own = own;
		r.changed = true;
	}
	r.wanted = r.contact ? speed : 0.0f;
	// 0x006aff70 with a step of 1.
	const float gap = r.wanted - r.eased;
	if (std::fabs(gap) >= 1e-5f) {
		r.eased += 0.08f * gap;
		if (std::fabs(r.eased) < 0.0001f) {
			r.eased = 0;
		}
	}
	// 0x005c65d0.
	if (r.state == 0) {
		if (++r.ticks >= 20) {
			r.state = 1;
		}
		return;
	}
	if (r.eased > kEpsilon && r.changed) {
		r.changed = false;
		r.state = 0;
		r.ticks = 0;
		std::string sound;
		if (m_materials && r.ground >= 0) {
			for (const uint32_t other : {static_cast<uint32_t>(r.ground), 0u}) {
				const physics::MaterialCell* cell = m_materials->cell(r.own, other);
				if (cell && !cell->soundTemplates.empty() && !cell->soundTemplates[0].empty()) {
					sound = cell->soundTemplates[0];
					break;
				}
			}
		}
		r.sound = sound;
		r.playing = !r.sound.empty();
		return;
	}
	if (r.eased <= kEpsilon) {
		r.playing = false;
	} else if (!r.playing) {
		r.playing = !r.sound.empty();
	}
}

std::vector<GameWorld::RollingSound> GameWorld::rollingSounds() const
{
	std::vector<RollingSound> out;
	for (const auto& [id, r] : m_rolling) {
		if (!r.playing || r.sound.empty()) {
			continue;
		}
		for (const Body& body : m_bodies) {
			if (body.object == id) {
				out.push_back({id, r.sound, r.eased, body.body.position, body.body.velocity});
				break;
			}
		}
	}
	return out;
}

// The cells (the armour's update 0x00654d80 on the server, for a child's
// armour while it stands, +0x15 off): every 0.1 s (+0x288 counting down
// from +0x28c) one with mirrorParentDamageLevel compares its percent with
// its root's (slot 0x40, 0x00651f90: the hit points over maxHitPoints, or
// over wreckHitPoints below 0, x 100): above it, it loses the difference
// of its maxHitPoints (slot 0x44, 0x00656320); below it, it heals the
// difference (slot 0x48, 0x00655a00: canBeRepaired, the cells' 1). So the
// cells show the IFV's damage level, and a hull brought under 0 takes them
// all down. A cell brought to 0 or under is a wreck (0x00654120) and its
// Python component's onDestroyEvent goes with the vehicle (the cells' 56,
// VehicleShieldDestroyed, scored in scoringCommon.py to an enemy
// attacker: the mirror's loss has none). Not rewritten: the cells' own
// collision (their meshes aren't part of the vehicle's), so a shot can't
// strike one, and with it damagePointOnDestroy (0x004c0e40, armour slot
// 0x16c: the damage-assist award, given only on the hit that destroys the
// cell), which the rewrite has no award for; the cells' armour effects.
void GameWorld::stepShieldCells(Body& body, float seconds)
{
	const world::Object* object = m_objects.find(body.object);
	const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr;
	if (!t) {
		return;
	}
	if (!body.cellsBuilt) {
		body.cellsBuilt = true;
		std::function<void(const world::ObjectTemplate&, int)> find = [&](const world::ObjectTemplate& node, int depth) {
			for (const world::ChildTemplate& child : node.children()) {
				const world::ObjectTemplate* c = m_templates.find(child.name);
				if (!c || depth > 8) {
					continue;
				}
				if (hasComponent(*c, "Armor")) {
					Body::Cell cell;
					cell.t = c;
					cell.mirror = number(*c, "armor.mirrorParentDamageLevel", 0) != 0;
					cell.hudCell = static_cast<int>(number(*c, "armor.hudRepairCellNo", -1));
					cell.maxHitPoints = number(*c, "armor.maxHitPoints", 10);
					cell.hitPoints = number(*c, "armor.hitPoints", cell.maxHitPoints);
					if (cell.mirror || cell.hudCell != -1) {
						body.cells.push_back(cell);
					}
				}
				find(*c, depth + 1);
			}
		};
		find(*t, 0);
	}
	if (body.cells.empty()) {
		return;
	}
	const float rootMax = number(*t, "armor.maxHitPoints", 10);
	const float rootWreck = number(*t, "armor.wreckHitPoints", 10);
	const float left = hitPoints(body.object);
	const float rootPercent = left >= 0 ? left / rootMax * 100.0f : left / rootWreck * 100.0f;
	for (Body::Cell& cell : body.cells) {
		if (cell.wreck || !cell.mirror) {
			continue;
		}
		if (cell.timer > 0) {
			cell.timer -= seconds;
			continue;
		}
		cell.timer = 0.1f;
		const float own = cell.hitPoints / cell.maxHitPoints * 100.0f;
		if (own > rootPercent) {
			cell.hitPoints -= cell.maxHitPoints * (own - rootPercent) * 0.01f;
		} else if (own < rootPercent && number(*cell.t, "armor.canBeRepaired", 1) != 0) {
			cell.hitPoints = std::min(cell.maxHitPoints, cell.hitPoints + (rootPercent - own) * 0.01f * cell.maxHitPoints);
		}
		if (cell.hitPoints <= 0) {
			cell.wreck = true;
			const int event = static_cast<int>(number(*cell.t, "Python.onDestroyEvent", 64));
			if (event != 64) {
				raiseTitanEvent(event, body.object, -1); // the scripts' handler takes the root
			}
		}
	}
}

std::vector<bool> GameWorld::repairCells(uint32_t vehicle) const
{
	std::vector<bool> out;
	for (const Body& body : m_bodies) {
		if (body.object != vehicle) {
			continue;
		}
		for (const Body::Cell& cell : body.cells) {
			if (cell.hudCell != -1) {
				out.push_back(!cell.wreck);
			}
		}
	}
	return out;
}

} // namespace dice::hfe::game
