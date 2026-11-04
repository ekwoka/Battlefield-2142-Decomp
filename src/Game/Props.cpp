// Destroyable props: the DestroyableObject templates (class 0xc4d0,
// template constructor 0x005bf100): barrel_big_dest_c and crate01_dest_c
// (40 hit points, an armour explosion of 150 over 7 m), the rail bars
// train_rail_bar_des / jbtrain_rail_bar_des (1) and the bridge piece
// bp1_bridge_industrial_01_c_sjbvfindes (1000). Static objects without
// mobile physics, broken through the generic Armor (docs/engine.md,
// "Debris, emitter types, level ambience and template behaviours").
#include "Game/GameWorld.h"

#include "Physics/CollisionTemplateManager.h"
#include "World/Object.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::game {

namespace {

float settingOr(const world::ObjectTemplate& t, const char* name, float fallback)
{
	const auto it = t.settings().find(name);
	return it == t.settings().end() ? fallback : static_cast<float>(std::atof(it->second.c_str()));
}

} // namespace

bool GameWorld::isProp(uint32_t object) const
{
	const world::Object* o = m_objects.find(object);
	const world::ObjectTemplate* t = o ? o->objectTemplate() : nullptr;
	return t && t->typeName() == "DestroyableObject" && t->settings().count("armor.maxHitPoints") != 0;
}

const std::vector<uint32_t>& GameWorld::props()
{
	if (m_propsScanned != m_objects.objects().size()) {
		m_propsScanned = m_objects.objects().size();
		m_props.clear();
		for (const auto& owned : m_objects.objects()) {
			if (owned && isProp(owned->id())) {
				m_props.push_back(owned->id());
			}
		}
	}
	return m_props;
}

bool GameWorld::propWrecked(uint32_t object) const
{
	return std::any_of(m_brokenProps.begin(), m_brokenProps.end(), [&](const BrokenProp& p) { return p.object == object; });
}

int GameWorld::propGeometry(uint32_t object) const
{
	for (const BrokenProp& p : m_brokenProps) {
		if (p.object == object) {
			return p.geometry;
		}
	}
	return -1;
}

// The armour's destruction (0x00654120, hit points crossing 0): the wreck
// flags, then at once (wreckDelay at most 0.01, not a soldier, no
// dontUseWreck) the wreck's look (0x00651c60: the physics body goes (slot
// 0xd4 -> 0xc); a render object of two geometries or more shows its
// second (slot 0xcc(1)), one of a single geometry takes the flag of slot
// 0x54(0, 2), which the rewrite reads as hiding it; the children the same)
// and 0x006539f0. A later wreckDelay (the bridge's 0.05) posts event 0x27
// for the same. The armour's explosion (explosionDamage above 0: the game
// logic's slot 0xe8 -> 0x005a9090 at the object, its own object left out,
// the giver +0x24 the attacker) and the armour effects of level 0
// (0x00654730 / 0x006549a0: left in the world where it stands). The wreck
// drains (0x00652380) only with canBeDestroyed and a timeToStayAsWreck
// other than -1 (the rail bars: 20 s, then removed, 0x00651db0); the
// barrels', crates' and bridge's stay until the round's end. A level's
// props are spawned once by the ObjectSpawner 0x004d2250 makes of each
// (respawnDelay +0x29c, 0; nrOfObjectToSpawn +0x2a0, 1): a broken prop
// never comes back within the round, and the next round has them whole.
bool GameWorld::breakProp(uint32_t object, int attacker)
{
	world::Object* o = m_objects.find(object);
	if (!o || !isProp(object) || propWrecked(object)) {
		return false;
	}
	const world::ObjectTemplate& t = *o->objectTemplate();
	BrokenProp broken;
	broken.object = object;
	const float stay = settingOr(t, "armor.timeToStayAsWreck", 20);
	const float after = std::max(0.0f, settingOr(t, "armor.timeToStayAfterDestroyed", 0));
	const bool drains = settingOr(t, "armor.canBeDestroyed", 1) != 0 && stay != -1;
	broken.removeAt = drains ? m_time + std::max(stay, 0.0f) + after : 1e30;
	const bool wreckLook = settingOr(t, "armor.dontUseWreck", 0) == 0;
	if (wreckLook) {
		m_collision.removeOwner(object);
		m_projectileCollision.removeOwner(object);
		removeStaticParts(object);
		// The render object's geometries: the collision mesh's stand in for
		// them (the shipped barrel has two of each, the others one).
		const physics::CollisionMeshFile* mesh = nullptr;
		if (const auto name = t.settings().find("collisionMesh"); name != t.settings().end() && m_collisionTemplates && m_files) {
			mesh = m_collisionTemplates->mesh(name->second, *m_files);
		}
		if (mesh && mesh->geometries.size() >= 2) {
			broken.geometry = 1;
		} else {
			o->setHidden(true);
		}
	}
	o->setDestroyed(true);
	m_brokenProps.push_back(broken);
	const physics::Vec3 at {o->position().x, o->position().y, o->position().z};
	const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({o->rotation().x, o->rotation().y, o->rotation().z});
	for (const ArmorEffectList::Effect& effect : armorEffectList(t).effects) {
		if (effect.level == 0 && !effect.spectacular) {
			m_effects.spawn(effect.effect, at + turn.transform(effect.position), effect.rotation * turn);
		}
	}
	ProjectileSettings blast;
	blast.explosionDamage = settingOr(t, "armor.explosionDamage", 0);
	blast.explosionRadius = settingOr(t, "armor.explosionRadius", 0);
	blast.explosionMaterial = static_cast<int>(settingOr(t, "armor.explosionMaterial", 0));
	blast.explosionForce = settingOr(t, "armor.explosionForce", 0);
	if (const auto effect = t.settings().find("armor.explosionEffect"); effect != t.settings().end()) {
		blast.endEffect = effect->second;
	}
	if (blast.explosionDamage > 0 && blast.explosionRadius > 0) {
		explode(at, blast, attacker, 0, object);
	}
	return true;
}

void GameWorld::stepProps()
{
	for (auto it = m_brokenProps.begin(); it != m_brokenProps.end();) {
		if (it->removeAt <= m_time) {
			if (world::Object* o = m_objects.find(it->object)) {
				o->setHidden(true); // drained: removed (0x00651db0)
			}
			it->removeAt = 1e30;
		}
		++it;
	}
}

// A new round: the props stand again (their spawners' reset, 0x005ae580).
void GameWorld::resetProps()
{
	for (const BrokenProp& broken : m_brokenProps) {
		if (world::Object* o = m_objects.find(broken.object)) {
			o->setDestroyed(false);
			o->setHidden(false);
		}
		m_hitPoints.erase(broken.object);
	}
	m_brokenProps.clear();
}

} // namespace dice::hfe::game
