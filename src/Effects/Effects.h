// Effects: effect bundles (EffectBundle templates) whose children are
// particle systems (SpriteParticleSystem, MeshParticleSystem, ...), light
// sources and sounds, as in Objects/Effects. Impacts, muzzle flashes and
// explosions are effect bundles.
//
// How a particle looks over its life is from the particle shaders
// (Shaders/Particles.fx): the size, colour blend and transparency "graphs"
// are cubic polynomials a*t^3 + b*t^2 + c*t + d of the particle's age
// fraction t, clamped to 1; size = graph * particleMaxSize + randomSize,
// colour = color1 blended towards color2, times the particle's intensity
// plus its random intensity (vsParticle: (color * i + ri) / 2, doubled by
// the pixel shader).
//
// The emitters are RendDX9's (the sprite system: its template class,
// vtable 0x101d8c48, constructor defaults 0x10138b40; its instance, vtable
// 0x101d9170, constructor 0x1013f6a0):
//  - start (0x1013fb30): not at all when the template's QualityLevel is
//    above the manager's, not emitting when the camera is beyond the
//    manager's view distance; emitDelay, emitTime and preWarmTime are
//    taken then;
//  - update (0x101407c0): the time is cut to the particles' longest life,
//    the particles move in fixed steps of 0.01 s (the remainder carried to
//    the next update), the delay runs down, then the pre-warm is run in
//    steps of 1/30 s, then the emission (0x101403b0);
//  - emission (0x101403b0): one particle every 1 / (emitFrequency x the
//    distance LOD scale x the bundle's emit scale) seconds, throttled by
//    emitPrio once the pool is loaded, at the system's place lerped
//    between its last two, then moved on for the rest of the step; at most
//    (timeToLive + randomTimeToLive) x emitFrequency + 10 alive;
//  - a particle's birth (0x1013fdc0): life ttl + u x randomTimeToLive,
//    size offset and speed share one random s in -1..1 (randomSize x s,
//    emitSpeed - s x randomSpeed, times emitSpeedGraph over the emit time
//    and the bundle's speed scale), intensity the system's plus
//    randomIntensity x (-1..1), rotation randomRotation x (-1..1), its
//    speed rotationSpeed + |randomRotationSpeed x (-1..1)| with that
//    random's sign; inheriting the system's velocity unless local;
//  - its motion (0x1013f0c0): per step dv = -k |v - w| (v - w) + y
//    gravity x clamp(gravityGraph, 0, 1) dt, k = airResistance x
//    clamp(airResistanceGraph, 0, 1) dt, w the wind at it times
//    windAffectionFactor (world systems only), x += (v + dv / 2) dt, tiny
//    speeds zeroed; forceToWaterSurface puts it on the water; its
//    rotation turns by its speed x clamp(rotationGraph, 0, 1) (0x10131f80).
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
#include "Scene/EffectSettings.h"
#include "Scene/LightSettings.h"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
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
	float emitFrequency = 0;         // particles per second (the sprite template's +0x10, default 32)
	float emitTime = 0;
	float emitDelay = 0;
	bool isLooping = false;
	float emitSpeed = 0;
	float randomSpeed = 0;
	Graph emitSpeedGraph {0, 0, 0, 1}; // over the emit time (0x1013fdc0)
	float preWarmTime = 0;             // run at the start in 1/30 s steps (0x101407c0)
	Vec3 emitDirection {0, 1, 0};
	Vec3 randomDirectionAngle;       // degrees
	float timeToLive = 1;     // the sprite template's default is 4 (0x10138b40)
	float randomTimeToLive = 0;
	float gravity = 0;        // the sprite template's default is -9.82 (+0x90)
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
	float randomRotationSpeed = 0;
	Graph rotationGraph {0, 0, 0, 1}; // the rotation speed over the life
	float windAffectionFactor = 0;    // +0xa8
	bool forceToWaterSurface = false; // +0xac: on the water (the manager's water, slot 0x94)
	float waterSurfaceOffset = 0;     // +0xb0
	float randomIntensity = 0;        // +0x1bc
	// Particles.fx calcParticleLighting: the light map offset (+0x148,
	// TemplateParameters[0].z); the light factor is the light manager's
	// defaultEffectLightAffectionFactor (+0x1fc stays -1: no script sets it,
	// 0x10139800).
	float lightMapIntensityOffset = 0;
	// LodDistance, QualityLevel, emitPrio: Low 0, Medium 1, High 2
	// (BF2142.exe's names 0x00a04284; +0x64, +0x68, +0x60).
	int lodDistance = 2;
	int qualityLevel = 0;
	int emitPrio = 2;
	float minEmitScale = 0.05f; // +0xc8: the distance LOD's floor
	// particleShaderType 1: ParticleHeatShimmer (the back buffer displaced
	// by ShimmerIntensity x a texel, in ShimmerPhases waves; 0x1012ed89).
	bool heatShimmer = false;
	bool additive = false;
	std::string texture; // without ".dds"
	bool mesh = false;   // a MeshParticleSystem: each particle a copy of `geometry`
	std::string geometry; // its MeshParticleMesh
	// Mesh particles turn in three axes (yaw/pitch/roll, degrees): a random
	// start up to randomRotation and rotationSpeed per second; with
	// alignRotationToSpeed their z axis follows their velocity.
	Vec3 randomRotation3;
	Vec3 rotationSpeed3;
	bool alignToSpeed = false;
	// NonScreenAlignedParticleSystem: quads lying across the system's y
	// axis instead of facing the camera (NonScreenAlignedParticles.fx
	// displaces each corner by world vectors the CPU gives, displaceCoords).
	bool nonScreenAligned = false;
	// TrailSystem (Trail.fx): the particles are the points of a ribbon
	// along the system's path, in numPlanes planes about it, each faded as
	// it turns edge-on (the square of the plane's normal against the eye),
	// the ends faded over fadeInFactor / fadeOutFactor of the life, the
	// texture tiled textureTileFactor times along it and slid by
	// textureOffsetVelocity; points only while the system moves faster than
	// minTrailVelocity.
	bool trail = false;
	float fadeIn = 0.05f, fadeOut = 0.05f;
	float tileFactor = 1, uvOffsetVelocity = 0;
	int planes = 1;
	float minTrailVelocity = 0;
	// The texture's frames (animationEnable): frameCount frames, framesAcross
	// a row, each frameWidth x frameHeight of it.
	int frameCount = 1, framesAcross = 1;
	float frameWidth = 1, frameHeight = 1;
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
	float rotationSpeed = 0; // radians a second (0x1013fdc0 +0x7c)
	float intensity = 1;     // the system's, at birth (+0xc)
	float randomIntensity = 0; // randomIntensity x (-1..1) (+0x10)
	float alpha = 1;         // the bundle's alpha intensity at birth (+0x2c)
	Vec3 angles; // mesh particles: yaw/pitch/roll at birth, degrees
};

// What an emitter's update sees: the wind, the water, the camera (the
// distance LOD), the manager's settings and the sprite pool.
struct EmitContext {
	const scene::ParticleSystemSettings* settings = nullptr;
	const scene::WindSettings* wind = nullptr;
	bool hasWater = false;
	float waterLevel = 0;
	bool hasCamera = false;
	Vec3 camera;
	float tanHalfFov = 0.5f;
	int* poolUsed = nullptr; // sprites alive in all systems
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
	void update(float seconds, game::Random& random, const EmitContext& context = {});
	// Moves the system (an attached one, each tick).
	void place(const Vec3& position, const Mat3& rotation);
	const Vec3& position() const { return m_position; }
	const Mat3& rotation() const { return m_rotation; }
	// Ends the emission (a looping system's too); the particles live on.
	void stop() { m_stopped = true; }
	bool stopped() const { return m_stopped; }

	// The bundle's modifiers (EffectBundle's alpha intensity, emit scale,
	// emit speed scale and age scale: the instance's +0x164, +0x138, +0x13c
	// and +0x12c, set by Wheel/AirDraftEffectBundle each tick, 0x00618bc0..).
	float alphaIntensity = 1, emitScale = 1, speedScale = 1, timeScale = 1;
	// Whether it emits at all (started within the view distance and of a
	// quality the manager draws).
	bool active() const { return m_active; }
	// The distance LOD's emission scale now (+0x134).
	float lodScale() const { return m_lodScale; }

	// Attached: the anchor (0 for none), the mount (where the bundle sits
	// in the anchor's space) and the system's place in the bundle.
	uint32_t anchor = 0;
	uint32_t tag = 0;
	Vec3 mountPosition, localPosition;
	Mat3 mountRotation, localRotation;

private:
	void emit(game::Random& random, const Vec3& at, float age, const EmitContext& context);
	void simulate(Particle& p, float step, int steps, const EmitContext& context) const;
	void start(const EmitContext& context);
	void step(float seconds, game::Random& random, const EmitContext& context);

	const ParticleTemplate* m_template;
	bool m_started = false, m_active = true, m_prewarming = false;
	float m_delay = 0;     // emitDelay left (+0xe4)
	float m_emitLeft = 0;  // emitTime left (+0xe0)
	float m_preWarm = 0;   // pre-warm left (+0xf8)
	float m_clock = 0, m_emitClock = 0, m_carry = 0; // +0xe8, +0xf0, +0xf4
	float m_lodScale = 1;
	Vec3 m_previous;       // where it was at the last update (+0xc0)
	Vec3 m_velocity;       // its own velocity (inherited by world particles)
	Vec3 m_position;
	Mat3 m_rotation;
	float m_age = 0;
	float m_pending = 0; // fractional particles not emitted yet
	bool m_stopped = false;
	size_t m_capacity = 0; // (ttl + random ttl) x frequency + 10
	Vec3 m_lastPosition;  // a trail's: where it was last tick
	bool m_placed = false;
	float m_speed = 0;
	std::vector<Particle> m_particles;
};

// A vertex of the triangles drawn in world space (non-screen-aligned
// particles' quads, trails' ribbons): three per triangle.
struct WorldVertex {
	Vec3 position;
	float u = 0, v = 0;
	std::array<float, 4> color; // rgb, alpha
	float lightMapOffset = 0;   // the template's lightMapIntensityOffset (calcParticleLighting)
};

// What the renderer draws: one camera-facing quad per particle.
struct Sprite {
	Vec3 position;
	float size;
	float rotation;
	std::array<float, 4> color; // rgb, alpha
	float lightMapOffset = 0;   // the template's lightMapIntensityOffset (calcParticleLighting)
};

// The view the particles are drawn from: their fade by distance per LOD
// class (startLodPercentageOfLodDistance of the class's distance to it;
// 0x10132e90), with the manager's settings.
struct ParticleView {
	Vec3 eye;
	Vec3 forward {0, 0, 1};
	const scene::ParticleSystemSettings* settings = nullptr;
	// The alpha a particle keeps at view depth `depth` in LOD class `lod`.
	float fade(float depth, int lod) const;
};

// A mesh particle: its mesh scaled by `size`, turned and placed
// (MeshParticleMesh.fx: one bone per particle, the mesh times globalScale,
// coloured by the colour graph and faded by the transparency graph).
struct MeshSprite {
	Vec3 position;
	Mat3 rotation;
	float size;
	std::array<float, 4> color;
};

// "decal" templates (ObjectTemplate.create decal): what a material cell's
// decal template (MaterialManager.setDecalTemplate) leaves where a
// projectile hits: a square of `size` (+ up to randomSize) turned by
// rotation + up to randomRotation degrees about the surface's normal,
// showing one frame of an animationFrameCount-frame atlas
// (animationFrameCountX across, each setAnimationFrameWidth/HeightRelative
// of it; animationRandomizedStartFrame picks one at random).
struct DecalTemplate {
	std::string texture; // decalTextureName, without ".dds"
	float size = 1;
	float randomSize = 0;
	Vec3 color {1, 1, 1};
	float rotation = 0;
	float randomRotation = 0;
	int frameCount = 1;
	int framesAcross = 1;
	float frameWidth = 1;
	float frameHeight = 1;
	bool randomStartFrame = false;

	static DecalTemplate fromTemplate(const world::ObjectTemplate& t);
};

struct Decal {
	std::string texture;
	Vec3 position;
	Vec3 normal;
	Vec3 tangent; // the square's u axis
	float size = 1;
	std::array<float, 4> frame {0, 0, 1, 1}; // u0, v0, u1, v1
	Vec3 color {1, 1, 1};
	// On a moving object (its id; 0 for the world): position, normal and
	// tangent are in the object's space (BF2142.exe gives objects whose
	// template asks for it a decal set of their own: 0x0063c000 asks
	// DecalManager slot 0x14, RendDX9 0x100e6b70, kept at object +0x164 and
	// released with it, slot 0x20).
	uint32_t object = 0;
};

// A LightSource of an effect bundle (an explosion's flash, a muzzle
// flash): a point light of color * intensity, full up to
// attenuationRange1 and gone at attenuationRange2; or a placed object's
// (the levels' light bundles). The LightSource template has no lifetime of
// its own: a bundle's light lives as long as its bundle's longest sibling
// particle system (emit delay + time + life), fading out over that.
struct PointLight {
	Vec3 position;
	Vec3 color;   // at this moment (faded)
	float range1 = 0;
	float range2 = 1;
	// lightType Spot: lit within the cone about `direction` (world space),
	// full inside cosInner, gone outside cosOuter.
	bool spot = false;
	Vec3 direction {0, 0, 1};
	float cosInner = 1, cosOuter = 1;
	// affectLightmappedObjects: whether lightmapped surfaces take it too
	// (the static lights of the levels leave them to their lightmaps).
	bool affectsLightmapped = true;
};

// What a LightSource template sets (BF2142.exe's LightSource template,
// class 0x94c7, as its writer 0x00617030 lists the fields): lightType
// (+0x70: Directional, Point, Spot ...), isDynamic (+0x64),
// affectLightmappedObjects (+0x6d), affectingType (+0x90),
// attenuationRange1 / 2 (+0xa0 / +0xa4), color (+0xa8), coneAngle1 / 2
// (+0xd0 / +0xd4), enabled, intensity; flicker (+0xe4, clamped to 0..1 by
// 0x00616cf0) is kept but not applied. The template has no lifetime of its
// own (none of its setters, 0x00528f5a..0x0052b6ea, is one).
struct LightSourceLook {
	bool enabled = true;
	bool spot = false;
	bool directional = false;
	bool dynamic = true;
	bool affectsLightmapped = true;
	int affectingType = 0;
	Vec3 color {1, 1, 1}; // times intensity
	float range1 = 0, range2 = 1;
	Vec3 direction {0, 0, 1}; // in the light's space
	float cosInner = 1, cosOuter = 1;
	float flicker = 0;
};
LightSourceLook lightSourceOf(const world::ObjectTemplate& t);

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
	// The tags of an anchor's emitters and lights still alive (mounts to keep placed).
	std::set<uint32_t> mountedTags(uint32_t anchor) const;
	// The anchor's object is gone: its emitters stop where they are.
	void releaseAnchor(uint32_t anchor);
	void update(float seconds);

	// What the emitters read (null: the defaults): the manager's settings,
	// the wind, the water level (forceToWaterSurface) and the camera (the
	// view distance and the distance LOD; without one every system emits
	// at full rate). The world around the effects, not their state: so
	// const, for the owners that hand the manager out const (GameWorld).
	void setSettings(const scene::ParticleSystemSettings* settings) const { m_settings = settings; }
	void setWind(const scene::WindSettings* wind) const { m_wind = wind; }
	void setWater(bool has, float level) const
	{
		m_context.hasWater = has;
		m_context.waterLevel = level;
	}
	void setCamera(const Vec3& position, float tanHalfFov) const
	{
		m_context.hasCamera = true;
		m_context.camera = position;
		m_context.tanHalfFov = tanHalfFov;
	}
	// The bundle modifiers of an anchor's emitters spawned with `tag`
	// (Wheel/AirDraftEffectBundle, 0x0061bd20 / 0x0061ce20).
	void setModifiers(uint32_t anchor, uint32_t tag, float alphaIntensity, float emitScale, float speedScale,
		float timeScale = 1);
	// Sprites alive in all the sprite systems (the pool's use).
	int poolUsed() const { return m_poolUsed; }

	// The Sound children of the bundles started since the last call (the
	// bundle's start sound, S_<bundle>_Start), where they start.
	struct StartedSound {
		std::string name;
		Vec3 position;
	};
	std::vector<StartedSound> takeSounds() { return std::exchange(m_startedSounds, {}); }

	size_t emitters() const { return m_emitters.size(); }

	// Leaves a decal template's decal at a surface point (the oldest go
	// beyond kMaxDecals); false if there is no such decal template.
	bool spawnDecal(const std::string& name, const Vec3& position, const Vec3& normal);
	// The same on a moving object placed at `objectPosition`, turned by
	// `objectRotation` (yaw / pitch / roll degrees): kept in its space so it
	// moves with it.
	bool spawnDecal(const std::string& name, const Vec3& position, const Vec3& normal, uint32_t object,
		const Vec3& objectPosition, const Vec3& objectRotation);
	// Drops an object's decals (the object is gone).
	void dropDecals(uint32_t object);
	static constexpr size_t kMaxDecals = 256;
	const std::vector<Decal>& decals() const { return m_decals; }
	// The bundles' lights alive now.
	std::vector<PointLight> lights() const;

	// Calls `draw` for each mesh particle system's particles: its mesh, its
	// blending and the particles.
	template <class Draw>
	void forEachMeshGroup(Draw draw) const
	{
		for (const auto& emitter : m_emitters) {
			const ParticleTemplate& t = emitter->particleTemplate();
			if (!t.mesh || t.geometry.empty()) {
				continue;
			}
			m_meshSprites.clear();
			collectMeshes(*emitter, m_meshSprites);
			if (!m_meshSprites.empty()) {
				draw(t.geometry, t.additive, m_meshSprites);
			}
		}
	}
	// Calls `draw` for each group of sprites sharing a texture and blending
	// (the heat shimmer's with `shimmer`: forEachShimmerGroup), faded by
	// distance from `view` when it has settings.
	template <class Draw>
	void forEachGroup(Draw draw) const
	{
		forEachGroup(ParticleView {}, draw);
	}
	template <class Draw>
	void forEachGroup(const ParticleView& view, Draw draw, bool shimmer = false) const
	{
		for (const auto& emitter : m_emitters) {
			const ParticleTemplate& t = emitter->particleTemplate();
			if (t.mesh || t.trail || t.nonScreenAligned || t.heatShimmer != shimmer) {
				continue; // forEachMeshGroup, forEachWorldGroup
			}
			m_sprites.clear();
			collect(*emitter, m_sprites, view);
			if (!m_sprites.empty()) {
				draw(emitter->particleTemplate().texture, emitter->particleTemplate().additive, m_sprites);
			}
		}
	}
	// The heat shimmer's sprites (particleShaderType 1).
	template <class Draw>
	void forEachShimmerGroup(const ParticleView& view, Draw draw) const
	{
		forEachGroup(view, draw, true);
	}

	// Calls `draw` for each non-screen-aligned or trail system: its texture,
	// blending and triangles in world space, seen from `eye`.
	template <class Draw>
	void forEachWorldGroup(const Vec3& eye, Draw draw) const
	{
		ParticleView view;
		view.eye = eye;
		forEachViewedWorldGroup(view, draw);
	}
	template <class Draw>
	void forEachViewedWorldGroup(const ParticleView& view, Draw draw) const
	{
		for (const auto& emitter : m_emitters) {
			const ParticleTemplate& t = emitter->particleTemplate();
			if (!t.trail && !t.nonScreenAligned) {
				continue;
			}
			m_worldVertices.clear();
			if (t.trail) {
				collectTrail(*emitter, view.eye, m_worldVertices);
			} else {
				collectAligned(*emitter, m_worldVertices);
			}
			if (view.settings) {
				for (WorldVertex& v : m_worldVertices) {
					v.color[3] *= view.fade(physics::dot(v.position - view.eye, view.forward), t.lodDistance);
				}
			}
			if (!m_worldVertices.empty()) {
				draw(t.texture, t.additive, m_worldVertices);
			}
		}
	}
	static void collectTrail(const Emitter& emitter, const Vec3& eye, std::vector<WorldVertex>& out);
	static void collectAligned(const Emitter& emitter, std::vector<WorldVertex>& out);

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
		std::vector<Emitter*>* spawned, std::vector<size_t>* spawnedLights = nullptr);
	static void collect(const Emitter& emitter, std::vector<Sprite>& out, const ParticleView& view = {});
	static void collectMeshes(const Emitter& emitter, std::vector<MeshSprite>& out);
	struct LiveLight {
		Vec3 color;
		float range1 = 0, range2 = 1;
		LightSourceLook look;
		Vec3 direction {0, 0, 1}, localDirection {0, 0, 1};
		float age = 0, life = 1; // life below 0: until stopped (an attached looping bundle)
		bool stopped = false;
		// Attached as the emitters are.
		uint32_t anchor = 0, tag = 0;
		Vec3 position, mountPosition, localPosition;
		Mat3 mountRotation;
	};
	void placeLight(LiveLight& light) const;
	std::vector<LiveLight> m_lights;
	std::map<const world::ObjectTemplate*, DecalTemplate> m_decalTemplates;
	std::vector<Decal> m_decals;
	game::Random m_decalRandom {0x2d5c1e37};
	mutable std::vector<MeshSprite> m_meshSprites;
	mutable std::vector<WorldVertex> m_worldVertices;

	const world::ObjectTemplateManager& m_templates;
	game::Random m_random;
	std::map<const world::ObjectTemplate*, std::unique_ptr<ParticleTemplate>> m_particleTemplates;
	std::vector<std::unique_ptr<Emitter>> m_emitters;
	std::map<uint32_t, Anchor> m_anchors;
	std::vector<StartedSound> m_startedSounds;
	mutable std::vector<Sprite> m_sprites;
	mutable const scene::ParticleSystemSettings* m_settings = nullptr;
	mutable const scene::WindSettings* m_wind = nullptr;
	mutable EmitContext m_context;
	int m_poolUsed = 0;
};

// The effect bundles a world object carries in its template tree, started
// and stopped as BF2142.exe's EffectBundle (vtable 0x0097b658) handles its
// events (0x0061a4e0): startAtCreation 1 starts it with the object (the
// engines' flames, a rocket's trail); event 4, sent to a vehicle when a
// player enters it (Player 0x00593e30), starts those whose startOnEffects
// has 0x20 (the exhaust's startup) and event 5 (the PlayerControlObject's
// exit, 0x005cea00) stops those whose stopOnEffects has 0x40. The bundles
// follow the object, turned with its parts as the game poses them.
//
// Two bundle classes pick their effect from the ground (the template's own
// material against the ground's: MaterialManager's cell effect 0, slot
// 0x68): WheelEffectBundle (0x0061bfc0: while its wheel touches, at the
// contact) and AirDraftEffectBundle (0x0061d820: a ray along its -y,
// startDistance + 5 long, against the terrain, the water and the objects;
// while it hits closer than startDistance, at the hit). Each tick their
// modifiers (alpha intensity, emit scale, emit speed scale, age scale) are
// min + (max - min) x the product of the "From" mappings that are set
// (0x0061bd20, 0x0061ce20): FromSpeed the speed of the vehicle at the
// bundle, FromSpeedInDof its forward part (at least 0), FromAltitude the
// draft's ray length, FromEngine the engine's throttle; a mapping a/b
// gives 0 at a and 1 at b, clamped.
//
// A soldier's breathEffect (Soldier +0x3a0, created with it, 0x00588xxx)
// is shown while gameLogic.useColdBreath is on and the kit doesn't
// disableBreathEffect, unless it is the local player's own soldier in
// first person (0x00588830, every few seconds).
class ObjectEffects {
public:
	struct ObjectState {
		uint32_t id = 0;
		const world::ObjectTemplate* objectTemplate = nullptr;
		Vec3 position;
		Mat3 rotation;
		bool occupied = false; // a player is in it
		// Effect events the object's own animation fired this update (bit
		// 9 + N for "effect:N"): those bundles whose startOnEffects has
		// one start (0x0061a4e0), as a walker's foot steps.
		uint32_t effects = 0;
		// Its velocity (m/s, world) and its engine's throttle (0..1): the
		// wheel and draft bundles' modifiers.
		Vec3 velocity;
		float engine = 0;
		// Soldiers: breathing out in the cold (useColdBreath, not the
		// kit's disableBreathEffect, not the local player in first person).
		bool coldBreath = false;
	};
	// The ground under a point (wheel and draft bundles): the first hit from
	// `from` to `to` against the terrain, the water and the objects (not
	// `ignore`), its point and material.
	using GroundProbe = std::function<bool(const Vec3& from, const Vec3& to, uint32_t ignore, Vec3& hit,
		uint32_t& material)>;
	// The effect template of a pair of materials (MaterialManager's cell
	// effect 0), "" for none.
	using MaterialEffect = std::function<std::string(uint32_t bundleMaterial, uint32_t groundMaterial)>;
	void setGround(GroundProbe probe, MaterialEffect effect)
	{
		m_probe = std::move(probe);
		m_materialEffect = std::move(effect);
	}
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
		// WheelEffectBundle / AirDraftEffectBundle: the ground picks the effect.
		enum class Kind { Plain, Wheel, AirDraft } kind = Kind::Plain;
	};
	// A Wheel/AirDraft bundle's template values (its effectProperties part).
	struct GroundBundle {
		uint32_t material = 0;
		float startDistance = 0, maxDistance = 0; // AirDraft (+0x3b8, +0x3bc)
		float engineInfluence = 0;                // +0x3b4
		// The modifiers: 0 alpha intensity, 1 emit scale, 2 emit speed
		// scale, 3 age scale. Each its range and its mappings (0/0 unset).
		struct Modifier {
			float min = 1, max = 1;
			std::array<float, 2> fromSpeed {0, 0}, fromSpeedInDof {0, 0}, fromAltitude {0, 0}, fromEngine {0, 0};
		};
		std::array<Modifier, 4> modifiers;
		static GroundBundle fromTemplate(const world::ObjectTemplate& t);
		// A modifier's value for the inputs (0x0061bd20 / 0x0061ce20).
		float value(int modifier, float speed, float speedInDof, float altitude, float engine) const;
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
		// The ground bundles' current effect (by bundle), "" while off.
		std::vector<std::string> groundEffects;
		std::string breath; // the breathEffect bundle, "" for none
		bool breathing = false;
		Vec3 lastPosition, velocity; // its velocity from its moves (one tick, 1/30 s)
		bool moved = false;
	};
	void updateGround(const ObjectState& object, const Vec3& velocity, Tracked& tracked, size_t index, EffectManager& effects,
		const PartPose& partPose, EffectManager::View view);
	GroundProbe m_probe;
	MaterialEffect m_materialEffect;
	std::map<const world::ObjectTemplate*, GroundBundle> m_groundBundles;
	const world::ObjectTemplateManager& m_templates;
	std::map<uint32_t, Tracked> m_objects;
	BundleCache m_hasBundles;
	// A bundle's place along its chain under the parts' current poses.
	static void place(const Placed& bundle, uint32_t object, const PartPose& partPose, Vec3& position, Mat3& rotation);
public:
	// The tag the breath bundle is spawned with on its soldier.
	static constexpr uint32_t kBreathTag = 0xb7ea7;
};

// Rain and snow (RendDX9's rain manager, Shaders/Rain.fx): particleCount
// particles in a box of emitRadius about the camera plus offset, falling
// along particleVelocity x particleSpeed and wrapped round the box (the
// original moves 6 x 6 x 6 cells of 0.4 x emitRadius with the particles
// fixed in them, 0x1011b2a0, wrapping the cells round the box, 0x1011b720,
// and draws them 32 at a time, CELLPOSITIONS), swaying by maxDeviation in 16
// groups (DEVIATIONGROUPS; the original's walk of them, randomness and
// maxRandomChange, isn't the rewrite's sine at angleSpeed), faded from
// fadeOutRange of the box to its edge (vsPoint's sysDelta), drawn as points
// of particleSize x sqrt(1 / (scale.x + scale.y x distance)) pixels, at most
// particleMaxSize, or as lines lineLength long. All particleCount of them:
// RendDX9 sizes its buffer by it (0x1011d5b0; Aegis Station asks for
// 80,000), kMaxParticles only guards against a runaway script.
class Weather {
public:
	static constexpr int kMaxParticles = 200000;
	struct Drop {
		Vec3 position;
		Vec3 tail;    // lines: the other end
		float pixels; // points: the size on screen
		float alpha;  // the fade (times particleColor's)
	};
	void update(const scene::RainSettings& settings, const Vec3& camera, float seconds);
	const std::vector<Drop>& drops() const { return m_drops; }

private:
	std::vector<Vec3> m_positions; // without the sway
	std::vector<Drop> m_drops;
	float m_time = 0;
	game::Random m_random {0x5eed4a1};
};

} // namespace dice::hfe::effects
