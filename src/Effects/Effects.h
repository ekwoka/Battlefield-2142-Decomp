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
// What is inferred, until RendDX9's emitter update (0x100c3880) is
// decompiled: how particles are emitted (frequency over the emit time,
// radial and random directions, random speed and life) and moved (gravity
// and air resistance scaled by their graphs).
#pragma once

#include "Game/Random.h"
#include "Physics/RigidBody.h"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::world {
class ObjectTemplate;
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
	const std::vector<Particle>& particles() const { return m_particles; }
	// Still emitting or with particles alive.
	bool alive() const;
	void update(float seconds, game::Random& random);

private:
	void emit(game::Random& random);

	const ParticleTemplate* m_template;
	Vec3 m_position;
	Mat3 m_rotation;
	float m_age = 0;
	float m_pending = 0; // fractional particles not emitted yet
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

	// Starts an effect bundle or a single particle system by template name;
	// false if there is no such template.
	bool spawn(const std::string& name, const Vec3& position, const Mat3& rotation = {});
	void update(float seconds);

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

private:
	const ParticleTemplate* particleTemplate(const world::ObjectTemplate& t);
	void spawnTemplate(const world::ObjectTemplate& t, const Vec3& position, const Mat3& rotation, int depth);
	static void collect(const Emitter& emitter, std::vector<Sprite>& out);

	const world::ObjectTemplateManager& m_templates;
	game::Random m_random;
	std::map<const world::ObjectTemplate*, std::unique_ptr<ParticleTemplate>> m_particleTemplates;
	std::vector<std::unique_ptr<Emitter>> m_emitters;
	mutable std::vector<Sprite> m_sprites;
};

} // namespace dice::hfe::effects
