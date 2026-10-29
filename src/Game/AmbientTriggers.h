// Level ambience: Trigger and AmbientEffectArea objects (the levels'
// AmbientObjects.con), as BF2142.exe runs them.
//
// A Trigger (template class 0x9c45, vtable 0x0096fb68; its object 0x005d36b0)
// watches a sphere of `radius` for players. Its `events` bits say what
// sets it off: 1 a player in the sphere, 4 a player moving in it (faster
// than 0.1 m/s), 2 a weapon fired in it (0x005c4500 -> 0x005149d0), 8 an
// explosion in it (0x005a6e70 -> 0x005149d0). The trigger manager
// (DAT_00a3fc48, update 0x00514fc0) looks at three of the triggers no one
// is near each tick, round robin, and every tick at those with players in
// them (0x005d31f0 finds the objects in the sphere); a trigger with bits 1
// or 4 tests (0x005d3890) once minimumTimeBetweenTriggering (1 s by
// default, 0x005d32c0) has passed since it last fired: for the first
// player in it (each of them with triggerForEachActivatingPlayer) it fires
// with the chance 1 - (1 - chance / max(averageTimeBetweenTests, 1))^t, t
// the seconds since its last test clamped to 0..100 (0x005d3600); the
// events 2 and 8 fire it with `chance` (0x005d3540), past the same minimum
// time. Its random number is drawn from a generator reseeded with the tick
// count each time (0x005d2e60).
//
// Firing (0x005d3450) sets off every AmbientEffectArea (class 0x9c44,
// object 0x005ba780) with the trigger's triggerId (0x005ba390, linked at
// the level's start, 0x005d33d0). An area (0x005ba9c0) plays its
// linkedEffectContainer, an EffectBundle, or one of the children of a
// Bundle picked at random: at a random point of the disc of its radius
// about it (x = r u, z = r (-1..1) sqrt(1 - u^2), u in -1..1), or with
// playAtEventPosition at the activator (or the explosion), its height on
// the terrain with projectOnHeightmap; at most 10 of its effects alive at
// once, and no bundle whose timeToLive comes out -1 or 30 (0x005ba490).
// numNetworkedEffects above 0 makes the area the server's (it would send
// the effect to the clients, 0x00510270): a hosted game is the server.
#pragma once

#include "Physics/RigidBody.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dice::hfe::world {
class ObjectManager;
class ObjectTemplateManager;
class ObjectTemplate;
} // namespace dice::hfe::world

namespace dice::hfe::game {

class AmbientTriggers {
public:
	// A player in the world: where the object it controls is (its soldier
	// or vehicle) and how fast that moves.
	struct Activator {
		int player = -1;
		uint32_t object = 0;
		physics::Vec3 position;
		float speed = 0;
	};
	// An effect to start: the bundle, where, and the area it belongs to
	// (the anchor its effects are counted by).
	struct Effect {
		std::string bundle;
		physics::Vec3 position;
		uint32_t area = 0;
	};
	// The trigger's and the area's template values.
	struct TriggerSettings {
		int triggerId = -1; // +0x2a4
		float radius = 0;   // +0x2a0
		uint32_t events = 0; // +0x29c
		float chance = 0;    // +0x2a8, clamped to 0..1 (0x005d2ef0)
		float averageTimeBetweenTests = -1; // +0x2ac
		float minimumTimeBetweenTriggering = 1; // +0x2b0
		bool forEachActivatingPlayer = false;   // +0x2b4
		static TriggerSettings fromTemplate(const world::ObjectTemplate& t);
	};
	struct AreaSettings {
		float radius = 0;         // +0x29c
		int triggerId = -1;       // +0x2a0
		bool playAtEventPosition = false; // +0x2a4
		int numNetworkedEffects = 0;      // +0x2a8
		std::string linkedEffectContainer; // +0x2ac
		bool projectOnHeightmap = false;  // +0x2c8
		static AreaSettings fromTemplate(const world::ObjectTemplate& t);
	};

	// The level's triggers and areas (at the round's start).
	void link(const world::ObjectManager& objects, const world::ObjectTemplateManager& templates);
	size_t triggers() const { return m_triggers.size(); }
	size_t areas() const { return m_areas.size(); }
	// One tick at `now` seconds (`tick` the tick count, the random's seed):
	// the players there are.
	void update(double now, uint32_t tick, const std::vector<Activator>& activators);
	// The events 2 and 8: a weapon fired by `player` at `position`, an
	// explosion at `position`.
	void weaponFired(double now, uint32_t tick, const Activator& shooter);
	void explosion(double now, uint32_t tick, const physics::Vec3& position);
	// The effects to start since the last call.
	std::vector<Effect> takeEffects() { return std::exchange(m_effects, {}); }

	// The terrain's height (projectOnHeightmap); the bundles' sampled
	// timeToLive (-1 or 30 aren't played) and a Bundle's children; how
	// many of an area's effects are alive.
	std::function<float(float x, float z)> groundHeight;
	std::function<float(const std::string& bundle)> bundleTimeToLive;
	std::function<std::vector<std::string>(const std::string& bundle)> bundleChildren;
	std::function<size_t(uint32_t area)> liveEffects;
	// The chance a trigger fires after `elapsed` seconds without a test (0x005d3600).
	static float testChance(float chance, float averageTimeBetweenTests, double elapsed);

private:
	struct Trigger {
		uint32_t object = 0;
		physics::Vec3 position;
		TriggerSettings settings;
		std::vector<size_t> areas;
		double lastTest = -1;   // +0x18c (0: not yet)
		double lastTrigger = -1e9; // +0x190
		bool active = false;
	};
	struct Area {
		uint32_t object = 0;
		physics::Vec3 position;
		AreaSettings settings;
		uint32_t random = 0x759dc150; // +0x198
	};
	bool roll(uint32_t tick, float chance); // 0x005d2e60
	void test(Trigger& trigger, double now, uint32_t tick, const std::vector<const Activator*>& inside);
	void fire(Trigger& trigger, double now, const Activator* by, const physics::Vec3* at);
	void play(Area& area, const Activator* by, const physics::Vec3* at);
	std::vector<Trigger> m_triggers;
	std::vector<Area> m_areas;
	size_t m_cursor = 0;
	uint32_t m_shared = 0x759dc150;  // the game's generator (0x00a05b30)
	std::vector<Effect> m_effects;
};

} // namespace dice::hfe::game
