#include "AmbientTriggers.h"

#include "Game/Random.h"
#include "World/ObjectTemplateManager.h"
#include "World/Object.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

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

physics::Vec3 toPhysics(const world::Vec3& v) { return {v.x, v.y, v.z}; }

float distance(const physics::Vec3& a, const physics::Vec3& b) { return std::sqrt(physics::lengthSquared(a - b)); }

// The engine's (0, 1] and -1..1 draws (0x0042b230's top bits).
float unitOf(uint32_t r) { return static_cast<float>((static_cast<int32_t>(r | 0x80) >> 7) + 1) * 5.9604645e-08f; }
float signedOf(uint32_t r) { return static_cast<float>((static_cast<int32_t>(r | 0x80) >> 7) + 1) * 1.1920929e-07f - 1.0f; }

bool sameType(const world::ObjectTemplate& t, const char* type)
{
	const std::string& name = t.typeName();
	size_t n = 0;
	while (type[n] != 0) {
		++n;
	}
	if (name.size() != n) {
		return false;
	}
	for (size_t i = 0; i < n; ++i) {
		if (std::tolower(static_cast<unsigned char>(name[i])) != std::tolower(static_cast<unsigned char>(type[i]))) {
			return false;
		}
	}
	return true;
}

} // namespace

AmbientTriggers::TriggerSettings AmbientTriggers::TriggerSettings::fromTemplate(const world::ObjectTemplate& t)
{
	TriggerSettings s;
	s.triggerId = static_cast<int>(number(t, "triggerId", static_cast<float>(s.triggerId)));
	s.radius = number(t, "radius", s.radius);
	s.events = static_cast<uint32_t>(std::max(0.0f, number(t, "events", 0)));
	s.chance = std::clamp(number(t, "chance", s.chance), 0.0f, 1.0f);
	s.averageTimeBetweenTests = number(t, "averageTimeBetweenTests", s.averageTimeBetweenTests);
	s.minimumTimeBetweenTriggering = number(t, "minimumTimeBetweenTriggering", s.minimumTimeBetweenTriggering);
	s.forEachActivatingPlayer = number(t, "triggerForEachActivatingPlayer", 0) != 0;
	return s;
}

AmbientTriggers::AreaSettings AmbientTriggers::AreaSettings::fromTemplate(const world::ObjectTemplate& t)
{
	AreaSettings s;
	s.radius = number(t, "radius", s.radius);
	s.triggerId = static_cast<int>(number(t, "triggerId", static_cast<float>(s.triggerId)));
	s.playAtEventPosition = number(t, "playAtEventPosition", 0) != 0;
	s.numNetworkedEffects = static_cast<int>(number(t, "numNetworkedEffects", 0));
	if (const std::string* linked = setting(t, "linkedEffectContainer")) {
		s.linkedEffectContainer = *linked;
	}
	s.projectOnHeightmap = number(t, "projectOnHeightmap", 0) != 0;
	return s;
}

void AmbientTriggers::link(const world::ObjectManager& objects, const world::ObjectTemplateManager&)
{
	m_triggers.clear();
	m_areas.clear();
	m_cursor = 0;
	for (const auto& object : objects.objects()) {
		const world::ObjectTemplate* t = object->objectTemplate();
		if (!t) {
			continue;
		}
		if (sameType(*t, "Trigger")) {
			Trigger trigger;
			trigger.object = object->id();
			trigger.position = toPhysics(object->position());
			trigger.settings = TriggerSettings::fromTemplate(*t);
			m_triggers.push_back(trigger);
		} else if (sameType(*t, "AmbientEffectArea")) {
			Area area;
			area.object = object->id();
			area.position = toPhysics(object->position());
			area.settings = AreaSettings::fromTemplate(*t);
			m_areas.push_back(area);
		}
	}
	// 0x005d33d0 with 0x005ba390: each trigger's areas by triggerId; a
	// trigger without any is dropped (0x00514bd0 with 0x005d31a0).
	for (Trigger& trigger : m_triggers) {
		for (size_t i = 0; i < m_areas.size(); ++i) {
			if (m_areas[i].settings.triggerId == trigger.settings.triggerId) {
				trigger.areas.push_back(i);
			}
		}
	}
	m_triggers.erase(std::remove_if(m_triggers.begin(), m_triggers.end(), [](const Trigger& t) { return t.areas.empty(); }),
		m_triggers.end());
}

float AmbientTriggers::testChance(float chance, float averageTimeBetweenTests, double elapsed)
{
	const float average = std::max(averageTimeBetweenTests, 1.0f);
	const float t = std::clamp(static_cast<float>(elapsed), 0.0f, 100.0f);
	return 1.0f - std::pow(1.0f - chance / average, t);
}

bool AmbientTriggers::roll(uint32_t tick, float chance)
{
	if (chance >= 1.0f) {
		return true;
	}
	uint32_t state = (tick & 0x3ff) + 0x39e385;
	return unitOf(lehmerNext(state)) < chance;
}

void AmbientTriggers::update(double now, uint32_t tick, const std::vector<Activator>& activators)
{
	if (m_triggers.empty()) {
		return;
	}
	std::vector<const Activator*> inside;
	const auto playersIn = [&](const Trigger& trigger) {
		inside.clear();
		for (const Activator& a : activators) {
			if (a.player >= 0 && distance(a.position, trigger.position) < trigger.settings.radius) {
				inside.push_back(&a);
			}
		}
		return !inside.empty();
	};
	const auto periodic = [](const Trigger& trigger) { return (trigger.settings.events & 5) != 0; };
	// Those already active: tested while someone is in them, else let go.
	std::vector<Trigger*> started;
	for (Trigger& trigger : m_triggers) {
		if (!trigger.active) {
			continue;
		}
		if (!playersIn(trigger)) {
			trigger.active = false;
		} else if (periodic(trigger)) {
			test(trigger, now, tick, inside);
		}
	}
	// Three of the others a tick, round robin.
	for (int n = 0; n < 3 && n < static_cast<int>(m_triggers.size()); ++n) {
		Trigger& trigger = m_triggers[m_cursor];
		m_cursor = (m_cursor + 1) % m_triggers.size();
		if (trigger.active || !playersIn(trigger)) {
			continue;
		}
		trigger.active = true;
		if (periodic(trigger)) {
			test(trigger, now, tick, inside);
		}
	}
}

void AmbientTriggers::test(Trigger& trigger, double now, uint32_t tick, const std::vector<const Activator*>& inside)
{
	// 0x005d3890.
	if (trigger.lastTest < 0) {
		trigger.lastTest = now;
	}
	if (now - trigger.lastTrigger < trigger.settings.minimumTimeBetweenTriggering) {
		return;
	}
	for (const Activator* a : inside) {
		if ((trigger.settings.events & 4) != 0 && a->speed <= 0.1f) {
			continue; // moving players only
		}
		// 0x005d3600: the chance over the time since the last test.
		const double elapsed = now - trigger.lastTest;
		if (elapsed > 0
			&& roll(tick, testChance(trigger.settings.chance, trigger.settings.averageTimeBetweenTests, elapsed))) {
			fire(trigger, now, a, &a->position);
		}
		if (!trigger.settings.forEachActivatingPlayer) {
			break;
		}
	}
	trigger.lastTest = now;
}

void AmbientTriggers::weaponFired(double now, uint32_t tick, const Activator& shooter)
{
	// 0x005149d0 event 2: the triggers with bit 2 the weapon is in.
	for (Trigger& trigger : m_triggers) {
		if ((trigger.settings.events & 2) != 0 && distance(shooter.position, trigger.position) < trigger.settings.radius
			&& now - trigger.lastTrigger >= trigger.settings.minimumTimeBetweenTriggering
			&& roll(tick, trigger.settings.chance)) {
			const physics::Vec3 none;
			fire(trigger, now, &shooter, &none); // 0x005d3540: no position for a shot
		}
	}
}

void AmbientTriggers::explosion(double now, uint32_t tick, const physics::Vec3& position)
{
	// Event 8 (0x005a6e70): the triggers with bit 8 it is in.
	for (Trigger& trigger : m_triggers) {
		if ((trigger.settings.events & 8) != 0 && distance(position, trigger.position) < trigger.settings.radius
			&& now - trigger.lastTrigger >= trigger.settings.minimumTimeBetweenTriggering
			&& roll(tick, trigger.settings.chance)) {
			fire(trigger, now, nullptr, &position);
		}
	}
}

void AmbientTriggers::fire(Trigger& trigger, double now, const Activator* by, const physics::Vec3* at)
{
	// 0x005d3450: each of its areas, then the time it fired.
	for (size_t index : trigger.areas) {
		play(m_areas[index], by, at);
	}
	trigger.lastTrigger = now;
}

void AmbientTriggers::play(Area& area, const Activator* by, const physics::Vec3* at)
{
	// 0x005ba9c0.
	const AreaSettings& s = area.settings;
	if (s.linkedEffectContainer.empty()) {
		return;
	}
	std::string bundle = s.linkedEffectContainer;
	if (bundleChildren) {
		// A Bundle: one of its children (the list holds the bundle itself
		// first, 0x006f2f70; a lone bundle plays nothing).
		const std::vector<std::string> children = bundleChildren(bundle);
		if (!children.empty()) {
			const size_t count = children.size() + 1;
			const float pick = (static_cast<float>(count) - 1.0f) * unitOf(lehmerNext(m_shared)) + 1.0f;
			const size_t index = std::min(static_cast<size_t>(std::lround(pick)), count - 1);
			bundle = children[index - 1];
		}
	}
	physics::Vec3 offset;
	if (by == nullptr || !s.playAtEventPosition) {
		if (!s.playAtEventPosition) {
			const float u = std::clamp(signedOf(lehmerNext(area.random)), -1.0f, 1.0f);
			const float half = std::sin(std::acos(u));
			const float v = unitOf(lehmerNext(area.random));
			offset = {u * s.radius, 0, ((half + half) * v - half) * s.radius};
		} else if (at) {
			offset = *at - area.position;
		}
	} else {
		offset = by->position - area.position;
	}
	if (s.projectOnHeightmap && groundHeight) {
		offset.y = groundHeight(area.position.x + offset.x, area.position.z + offset.z) - area.position.y;
	}
	// 0x005ba490: at most ten alive, none that would never end.
	if (liveEffects && liveEffects(area.object) > 10) {
		return;
	}
	if (bundleTimeToLive) {
		const float life = bundleTimeToLive(bundle);
		if (life == -1.0f || life == 30.0f) {
			return;
		}
	}
	m_effects.push_back({bundle, area.position + offset, area.object});
}

} // namespace dice::hfe::game
