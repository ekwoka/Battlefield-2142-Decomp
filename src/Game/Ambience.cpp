// The game's side of the level ambience (AmbientTriggers.h): the players
// that set the triggers off and the effects the areas play.
#include "GameWorld.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <cctype>
#include <cmath>

namespace dice::hfe::game {

namespace {

bool isBundle(const std::string& type)
{
	static const char kBundle[] = "bundle";
	if (type.size() != 6) {
		return false;
	}
	for (size_t i = 0; i < 6; ++i) {
		if (std::tolower(static_cast<unsigned char>(type[i])) != kBundle[i]) {
			return false;
		}
	}
	return true;
}

} // namespace

void GameWorld::linkAmbience()
{
	m_ambience.link(m_objects, m_templates);
	m_ambiencePlaces.clear();
	m_ambience.groundHeight = [this](float x, float z) { return terrainHeight(x, z); };
	// The ground the effects' colliding particles and debris pieces meet.
	m_effects.setGround([this](float x, float z) { return terrainHeight(x, z); });
	m_ambience.bundleChildren = [this](const std::string& name) {
		// A Bundle's children (an EffectBundle is played itself).
		std::vector<std::string> children;
		if (const world::ObjectTemplate* t = m_templates.find(name); t && isBundle(t->typeName())) {
			for (const world::ChildTemplate& child : t->children()) {
				children.push_back(child.name);
			}
		}
		return children;
	};
	m_ambience.bundleTimeToLive = [this](const std::string& name) {
		// The EffectBundle's timeToLive (+0x3fc), sampled.
		const world::ObjectTemplate* t = m_templates.find(name);
		if (!t) {
			return 0.0f;
		}
		const auto it = t->settings().find("timeToLive");
		return it == t->settings().end() ? 0.0f : RandomDistribution::parse(it->second).sample(m_random);
	};
	m_ambience.liveEffects = [this](uint32_t area) { return m_effects.mountedTags(area).size(); };
}

void GameWorld::stepAmbience(float seconds)
{
	if (m_ambience.triggers() == 0) {
		return;
	}
	std::vector<AmbientTriggers::Activator> activators;
	for (const auto& player : m_players) {
		if (!player || !player->alive) {
			continue;
		}
		const uint32_t id = player->vehicle != 0 ? player->vehicle : player->object;
		const world::Object* object = m_objects.find(id);
		if (!object) {
			continue;
		}
		AmbientTriggers::Activator a;
		a.player = player->id;
		a.object = id;
		a.position = {object->position().x, object->position().y, object->position().z};
		auto [last, fresh] = m_ambiencePlaces.try_emplace(player->id, a.position);
		if (!fresh && seconds > 0) {
			a.speed = std::sqrt(physics::lengthSquared(a.position - last->second)) / seconds;
		}
		last->second = a.position;
		activators.push_back(a);
	}
	m_ambience.update(m_time, m_ticks, activators);
	for (const AmbientTriggers::Effect& effect : m_ambience.takeEffects()) {
		const world::Object* area = m_objects.find(effect.area);
		const physics::Vec3 at = area ? physics::Vec3 {area->position().x, area->position().y, area->position().z}
									  : physics::Vec3 {};
		m_effects.setAnchor(effect.area, at, {});
		const size_t mark = m_effects.soundsStarted();
		m_effects.spawnAttached(effect.bundle, effect.area, effect.position - at, {}, effects::EffectManager::View::Any,
			++m_ambienceTag);
		for (const auto& sound : m_effects.takeSoundsFrom(mark)) {
			m_sounds.push_back({sound.name, sound.position});
		}
	}
}

void GameWorld::ambientShot(const Player& player)
{
	if (m_ambience.triggers() == 0) {
		return;
	}
	const uint32_t id = player.vehicle != 0 ? player.vehicle : player.object;
	if (const world::Object* object = m_objects.find(id)) {
		AmbientTriggers::Activator a;
		a.player = player.id;
		a.object = id;
		a.position = {object->position().x, object->position().y, object->position().z};
		m_ambience.weaponFired(m_time, m_ticks, a);
	}
}

void GameWorld::ambientExplosion(const physics::Vec3& at)
{
	if (m_ambience.triggers() != 0) {
		m_ambience.explosion(m_time, m_ticks, at);
	}
}

} // namespace dice::hfe::game
