// Effects: effect bundles (EffectBundle templates) whose children are
// particle systems (SpriteParticleSystem, MeshParticleSystem, ...), light
// sources and sounds, as in Objects/Effects. Impacts, muzzle flashes and
// explosions are effect bundles.
//
// What is from the original: the parameters and their script names, and
// how a particle looks over its life, from the particle shaders
// (Shaders/Particles.fx): the size, colour blend and transparency "graphs"
// are cubic polynomials a*t^3 + b*t^2 + c*t + d of the particle's age
// fraction t, clamped to 1; size = graph * particleMaxSize + randomSize,
// colour = color1 blended towards color2.
//
// What is inferred, until RendDX9's emitter update is decompiled: how
// particles are emitted (frequency over the emit time, radial and random
// directions, random speed and life) and moved (gravity and air resistance
// scaled by their graphs).
//
// Attached effects: a bundle that is a child of an object (a weapon's
// muzzle flash, a vehicle's exhaust and engine flames, a rocket's trail)
// follows its parent. The emitters spawned against an anchor (spawnAttached)
// keep their place in it, and the anchor is moved each tick (setAnchor);
// a system with isLocalSystem 1 keeps its particles in its own space, so
// they move with it, the others leave theirs in the world (trails, smoke).
// showInFirstPerson / showInThirdPerson pick the systems a view shows (the
// 1P muzzle's glow1p, the 3P one's glow3p). The bundle's EffectBundle events
// (0x0061a4e0) are the owners' to send (ObjectEffects below; the first-person
// view's PlayerCamera).
#pragma once

#include "Game/Random.h"
#include "Physics/RigidBody.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dice::hfe::world {
class ObjectTemplate;
struct ChildTemplate;
class ObjectTemplateManager;
} // namespace dice::hfe::world

namespace dice::hfe::effects {

using physics::Mat3;
using physics::Vec3;

// a*t^3 + b*t^2 + c*t + d.
struct Graph {
	float a = 0, b = 0, c = 0, d = 1;
	static Graph parse(const std::string& text);
	float at(float t) const { return ((a * t + b) * t + c) * t + d; }
};

struct ParticleTemplate {
	enum class Emitter { Standard, RadialDirection } emitter = Emitter::Standard;
	game::RandomDistribution emitRange[3];
	float randomRadialDirection = 0; // degrees
	float radialDirection = 0;       // degrees from the plane across emitDirection
	float emitFrequency = 0;         // particles per second
	float emitTime = 0;
	float emitDelay = 0;
	bool isLooping = false;
	float emitSpeed = 0;
	float randomSpeed = 0;
	Vec3 emitDirection {0, 1, 0};
	Vec3 randomDirectionAngle;       // degrees
	float timeToLive = 1;
	float randomTimeToLive = 0;
	float gravity = 0;
	Graph gravityGraph;
	float airResistance = 0;
	Graph airResistanceGraph;
	float particleMaxSize = 1;
	float randomSize = 0;
	Graph sizeGraph;
	Vec3 color1 {1, 1, 1};
	Vec3 color2 {1, 1, 1};
	Graph colorBlendGraph {0, 0, 0, 0};
	Graph transparencyGraph;
	float randomRotation = 0;
	float rotationSpeed = 0;
	bool additive = false;
	std::string texture; // without ".dds"
	bool mesh = false;   // a MeshParticleSystem (drawn as sprites for now)
	bool localSystem = false;    // isLocalSystem: particles in the system's space
	bool showFirstPerson = true; // showInFirstPerson
	bool showThirdPerson = true; // showInThirdPerson

	static ParticleTemplate fromTemplate(const world::ObjectTemplate& t);
};

struct Particle {
	Vec3 position;
	Vec3 velocity;
	float age = 0;
	float life = 1;
	float sizeOffset = 0;
	float rotation = 0;
};

// A particle system placed in the world.
class Emitter {
public:
	Emitter(const ParticleTemplate& t, const Vec3& position, const Mat3& rotation);

	const ParticleTemplate& particleTemplate() const { return *m_template; }
	// The particles: in the world, or in the system's space for a local
	// system (worldPosition gives where they are).
	const std::vector<Particle>& particles() const { return m_particles; }
	Vec3 worldPosition(const Particle& p) const;
	// Still emitting or with particles alive.
	bool alive() const;
	void update(float seconds, game::Random& random);
	// Moves the system (an attached one, each tick).
	void place(const Vec3& position, const Mat3& rotation);
	const Vec3& position() const { return m_position; }
	const Mat3& rotation() const { return m_rotation; }
	// Ends the emission (a looping system's too); the particles live on.
	void stop() { m_stopped = true; }
	bool stopped() const { return m_stopped; }

	// Attached: the anchor (0 for none), the mount (where the bundle sits
	// in the anchor's space) and the system's place in the bundle.
	uint32_t anchor = 0;
	uint32_t tag = 0;
	Vec3 mountPosition, localPosition;
	Mat3 mountRotation, localRotation;

private:
	void emit(game::Random& random);

	const ParticleTemplate* m_template;
	Vec3 m_position;
	Mat3 m_rotation;
	float m_age = 0;
	float m_pending = 0; // fractional particles not emitted yet
	bool m_stopped = false;
	std::vector<Particle> m_particles;
};

// What the renderer draws: one camera-facing quad per particle.
struct Sprite {
	Vec3 position;
	float size;
	float rotation;
	std::array<float, 4> color; // rgb, alpha
};

class EffectManager {
public:
	explicit EffectManager(const world::ObjectTemplateManager& templates, uint32_t seed = 0x759dc150);
	~EffectManager();

	// The views a system shows in (showInFirstPerson, showInThirdPerson).
	enum class View { Any, FirstPerson, ThirdPerson };

	// Starts an effect bundle or a single particle system by template name;
	// false if there is no such template.
	bool spawn(const std::string& name, const Vec3& position, const Mat3& rotation = {}, View view = View::Any);
	// The same following an anchor: `position` and `rotation` are its place
	// in the anchor's space, `tag` names it for stopAttached.
	bool spawnAttached(const std::string& name, uint32_t anchor, const Vec3& position, const Mat3& rotation = {},
		View view = View::Any, uint32_t tag = 0);
	// Moves an anchor (and what is attached to it).
	void setAnchor(uint32_t anchor, const Vec3& position, const Mat3& rotation);
	// Moves the bundles spawned with `tag` within their anchor (a turning part).
	void setMount(uint32_t anchor, uint32_t tag, const Vec3& position, const Mat3& rotation);
	// Stops the emitters attached to an anchor (with `tag`, those spawned
	// with it); their particles live on.
	void stopAttached(uint32_t anchor, uint32_t tag = 0);
	// Whether an anchor has emitters still emitting (with `tag`: its).
	bool emitting(uint32_t anchor, uint32_t tag = 0) const;
	// The anchor's object is gone: its emitters stop where they are.
	void releaseAnchor(uint32_t anchor);
	void update(float seconds);

	// The Sound children of the bundles started since the last call (the
	// bundle's start sound, S_<bundle>_Start), where they start.
	struct StartedSound {
		std::string name;
		Vec3 position;
	};
	std::vector<StartedSound> takeSounds() { return std::exchange(m_startedSounds, {}); }

	size_t emitters() const { return m_emitters.size(); }
	// Calls `draw` for each group of sprites sharing a texture and blending.
	template <class Draw>
	void forEachGroup(Draw draw) const
	{
		for (const auto& emitter : m_emitters) {
			m_sprites.clear();
			collect(*emitter, m_sprites);
			if (!m_sprites.empty()) {
				draw(emitter->particleTemplate().texture, emitter->particleTemplate().additive, m_sprites);
			}
		}
	}

	// The emitters (checks).
	const std::vector<std::unique_ptr<Emitter>>& emitterList() const { return m_emitters; }

private:
	struct Anchor {
		Vec3 position;
		Mat3 rotation;
	};
	void placeAttached(Emitter& emitter) const;
	const ParticleTemplate* particleTemplate(const world::ObjectTemplate& t);
	void spawnTemplate(const world::ObjectTemplate& t, const Vec3& position, const Mat3& rotation, int depth, View view,
		std::vector<Emitter*>* spawned);
	static void collect(const Emitter& emitter, std::vector<Sprite>& out);

	const world::ObjectTemplateManager& m_templates;
	game::Random m_random;
	std::map<const world::ObjectTemplate*, std::unique_ptr<ParticleTemplate>> m_particleTemplates;
	std::vector<std::unique_ptr<Emitter>> m_emitters;
	std::map<uint32_t, Anchor> m_anchors;
	std::vector<StartedSound> m_startedSounds;
	mutable std::vector<Sprite> m_sprites;
};

// The effect bundles a world object carries in its template tree, started
// and stopped as BF2142.exe's EffectBundle (vtable 0x0097b658) handles its
// events (0x0061a4e0): startAtCreation 1 starts it with the object (the
// engines' flames, a rocket's trail); event 4, sent to a vehicle when a
// player enters it (Player 0x00593e30), starts those whose startOnEffects
// has 0x20 (the exhaust's startup) and event 5 (the PlayerControlObject's
// exit, 0x005cea00) stops those whose stopOnEffects has 0x40. The bundles
// follow the object, turned with its parts as the game poses them.
class ObjectEffects {
public:
	struct ObjectState {
		uint32_t id = 0;
		const world::ObjectTemplate* objectTemplate = nullptr;
		Vec3 position;
		Mat3 rotation;
		bool occupied = false; // a player is in it
	};
	// A part's pose in its parent (the game's partPose); may be empty.
	using PartPose = std::function<bool(uint32_t object, const std::string& part, Vec3& offset, Vec3& rotation)>;

	explicit ObjectEffects(const world::ObjectTemplateManager& templates) : m_templates(templates) {}
	// Once a tick: the objects there are now. Starts, moves and stops their
	// bundles in `effects` (as seen from `view`).
	void update(const std::vector<ObjectState>& objects, EffectManager& effects, const PartPose& partPose = {},
		EffectManager::View view = EffectManager::View::ThirdPerson);
	size_t tracked() const { return m_objects.size(); }

	// A bundle of a template tree: its place in the root's space.
	struct Placed {
		const world::ObjectTemplate* bundle;
		std::vector<std::string> path; // the children from the root to it (for part poses)
		std::vector<const world::ChildTemplate*> chain; // the same children's links, to place it again
		Vec3 position;
		Mat3 rotation;
		uint32_t startOnEffects = 0, stopOnEffects = 0;
		bool startAtCreation = false;
	};
	// Whether each template's tree holds a bundle, kept between calls so
	// the walk skips the branches without one (a Titan's tree is large).
	using BundleCache = std::map<const world::ObjectTemplate*, bool>;
	static std::vector<Placed> bundlesOf(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& root,
		uint32_t object = 0, const PartPose& partPose = {}, BundleCache* cache = nullptr);

private:
	struct Tracked {
		const world::ObjectTemplate* objectTemplate = nullptr;
		std::vector<Placed> bundles;
		bool occupied = false;
	};
	const world::ObjectTemplateManager& m_templates;
	std::map<uint32_t, Tracked> m_objects;
	BundleCache m_hasBundles;
	// A bundle's place along its chain under the parts' current poses.
	static void place(const Placed& bundle, uint32_t object, const PartPose& partPose, Vec3& position, Mat3& rotation);
};

} // namespace dice::hfe::effects
