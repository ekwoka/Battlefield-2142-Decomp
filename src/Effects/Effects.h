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
// 0x101d9170, constructor 0x1013f6a0). The mesh system's (template vtable
// 0x101d8800, constructor 0x10136720, defaults 0x10135f30, its compile
// 0x10135e20, distance LOD 0x10136570; instance vtable 0x101d9348,
// constructor 0x101446b0) do the same in their own copies: start
// 0x10144ab0, update 0x10145bf0 (a pre-warm of 1/60 s steps, no
// isStaticSystem run), emission 0x10144d90 (its own pool, no emitPrio
// throttle), one particle now 0x10145660, motion 0x10144110, the
// system's place 0x10144040; so do the non-screen-aligned template's
// (vtable 0x101d89f8, constructor 0x10137fe0, defaults 0x10137290, compile
// 0x10137eb0, LOD 0x10137a90, texture 0x10137c60) and the trail
// template's (vtable 0x101d95a8, constructor 0x10147c70, defaults
// 0x10147180, compile 0x10147dd0, LOD 0x10147900, texture 0x10147ad0):
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
#include "Physics/CollisionWorld.h"
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
	// emitterType (BF2142.exe's names 0x00a04264, setter 0x00625de0 ->
	// RendDX9 template +0x268): where a particle is born and which way it
	// leaves (the sprites' 0x10140fa0 / 0x101410d0 / 0x101412c0, the same
	// for the mesh 0x10145fc0 / 0x101460f0 / 0x101462e0, the
	// non-screen-aligned 0x1013d840 and trail 0x1013acb0 systems):
	//  - Standard (0): in the ellipsoid of emitRadius (+0x18, slot 0x28:
	//    theta = 2 pi u, phi = pi u, the point (cos theta sin phi u rx,
	//    sin theta sin phi u ry, cos phi u rz)), along emitDirection;
	//  - RadialDirection (1) / Radial (2): on a ring about emitRadius's
	//    longest axis at radialDirection +- randomRadialDirection degrees,
	//    the axes' distances from emitRangeX/Y/Z; RadialDirection leaves
	//    along the ring's radius, Radial along emitDirection;
	//  - Box (3): at (emitRangeX, emitRangeY, emitRangeZ), along emitDirection.
	// randomDirectionAngle then turns the direction for each of them.
	enum class Emitter { Standard, RadialDirection, Radial, Box } emitter = Emitter::Standard;
	Vec3 emitRadius;
	game::RandomDistribution emitRange[3];
	float randomRadialDirection = 0; // degrees
	float radialDirection = 0;       // degrees about the ring's axis
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
	// start up to randomRotation (template +0x174) and a speed of
	// rotationSpeed (+0xb8) plus randomRotationSpeed (+0xac) x a random
	// with its sign, one random for the three axes each (the mesh
	// system's birth 0x10144d90 / 0x10145660), turned each update by that
	// speed x clamp(rotationGraph, 0, 1) (the draw's 0x101357c0); with
	// alignRotationToSpeed their z axis follows their velocity.
	Vec3 randomRotation3;
	Vec3 rotationSpeed3;
	Vec3 randomRotationSpeed3;
	bool alignToSpeed = false;
	// directionalScale (RendDX9's mesh particle template +0x1ec, slot
	// 0x188; 0): when not 0 the particles follow their velocity as with
	// alignRotationToSpeed (+0x1f0), and over 0 stretch along it by their
	// speed x it (0x101357c0). writeZ (+0x1f3, slot 0x1a8) only picks
	// another pass when on (0x10134450), and every shipped system has it 0,
	// the default; it isn't kept.
	float directionalScale = 0;
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
	// A sprite's flipbook (the sprite template's +0xd8 animationEnable,
	// +0xd4 animationSpeed 1, +0xd0 animationPlayOnce, +0xd1
	// animationRandomizedStartFrame; 0x10138b40's defaults): each particle
	// keeps a frame clock (+0x78), from 0 or floor((frames - 1) u) at birth
	// (0x1013fdc0), advanced by animationSpeed x clamp(rotationGraph, 0, 1)
	// a second when drawn (0x10131f80, the template's +0x100 graph); its
	// frame is the clock's whole part modulo the frames. animationPlayOnce
	// makes the speed frames / timeToLive (0x10138790, 0x101387f0): one
	// pass over a life. Particles.fx as shipped (LOW) draws the frame
	// alone, without blending in the next.
	bool animated = false;
	float animationSpeed = 1;
	bool animationPlayOnce = false;
	bool randomStartFrame = false;
	// selfShadowIntensity (+0xa4, slot 0x1d0, 1 by default): copied into
	// each particle (+0x8, 0x1013fdc0); neither the sprite collector
	// (0x10130bd0 / 0x10131f80) nor the shipped Particles.fx reads it.
	float selfShadowIntensity = 1;
	// showInZoomedMode (the exe's template +0xaa, 1 by default, 0x00626d40):
	// the bundle's start (0x00619f90) keeps it as bit 2 of the system's
	// flags (bit 0 showInFirstPerson, bit 1 showInThirdPerson). Each frame
	// (0x0061a1c0) a first-person bundle (0x006187c0: its holder the local
	// player in first person) hands RendDX9 the systems with bit 0 or 2
	// (a third-person one those with bit 1), each with its +8 set while the
	// holder is zoomed. So in first person a system shows with either flag,
	// zoomed or not: the flag never hides one (shipped, the one template
	// with showInFirstPerson 0 has showInZoomedMode 0 too). What +8 changes
	// is the view: RendDX9's first-person particle pass (0x10099860 ->
	// 0x100990b0 with param 0, then 1; the sprite list 0x10131680 and the
	// others draw only the systems whose +8 matches) draws the unzoomed
	// ones with the world's camera (renderer slot 0x5c, +0xf8) and the
	// zoomed ones with the first-person meshes' view (slot 0xdc
	// 0x1000d0d0, +0xfc: 0.82 rad).
	bool showZoomed = true;
	// nearCullingEnable (sprites; RendDX9 template +0x284, slots 0x248 /
	// 0x24c, 0x00623740): within 6 m past the near plane (+0x270) a
	// particle bigger than 0.15 m (+0x278) fades towards the camera
	// (0x101318e0, ParticleView::nearFade).
	bool nearCulling = false;
	// noRotationFromParent (RendDX9 template +0x222, slots 0x11c / 0x120):
	// the system takes only its parent's position, not its turn
	// (0x1013eff0: its local matrix plus the parent's position).
	bool noRotationFromParent = false;
	bool localSystem = false;    // isLocalSystem: particles in the system's space
	// isStaticSystem (setter 0x00626cf0 -> RendDX9 slot 0x190, +0x26d): on
	// its first update the system is run for its timeToLive (+0x88) in
	// steps of 0.1 s (0x101407c0), so a level's standing smoke or fire is
	// seen in its steady state, not starting.
	bool staticSystem = false;
	// alphaCull (the sprite template's +0xa0, slot 0x154): a particle whose
	// alpha intensity times its fade (+0x2c) is below it isn't drawn (the
	// sprites' collector 0x10130bd0); the transparency graph doesn't count.
	float alphaCull = 0;
	// pivotOffset (+0x1b0 / +0x1b4, slot 0x1c8): the quad's centre moved by
	// this many half-sizes off the particle (0/-0.5: standing on it).
	float pivot[2] = {0, 0};
	// drawOrder Front / Back (BF2142.exe 0x006232e0: the index in {Front,
	// Back} to RendDX9's template slot 0x1a8, the sprite template's +0x80,
	// 0x10139aa0): the sprite collector (0x10130bd0) files a Back system's
	// particles in the second batch list (+0xc20, 256 systems) and a Front
	// one's in the first (+0x20), and the frame (0x10131b70) draws the Back
	// list before the Front one. Each system's particles go back to front
	// by their squared distance to the camera (+0x14, the radix sort
	// 0x1012f800) whichever its order.
	bool drawBack = false;
	// collisionEnable (+0xf0) with collisionSpeedAlongNormal / AlongPlane:
	// world particles bounce off the ground (here the terrain under them,
	// taken as flat: the original's system ground planes not decoded),
	// keeping those shares of their speed across and along it.
	bool collision = false;
	float collisionAlongNormal = 0.7f, collisionAlongPlane = 0.8f;
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
	float alpha = 1;         // the bundle's alpha intensity at birth (+0x58; +0x2c is it faded)
	Vec3 angles; // mesh particles: yaw/pitch/roll, degrees (+0xa8)
	Vec3 spin;   // mesh particles: their speed, degrees a second (+0x9c)
	float frameClock = 0;    // the flipbook's (+0x78)
	float selfShadow = 1;    // the template's selfShadowIntensity (+0x8)
};

// A sprite's flipbook frame (0x10131f80): its texture rectangle, u0 v0 u1 v1.
std::array<float, 4> spriteFrame(const ParticleTemplate& t, float frameClock);

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
	// The ground's height at a point (collisionEnable); none if empty.
	std::function<float(float x, float z)> groundHeight;
	// The frames drawn so far (the renderer's count, DAT_101f8e58 slot
	// 0x60; EffectManager::markViewed counts them): 0 while none is drawn,
	// so a run that draws nothing never holds an emission.
	uint32_t frame = 0;
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
	float age() const { return m_age; }
	// Ends the emission (a looping system's too); the particles live on.
	void stop() { m_stopped = true; }
	bool stopped() const { return m_stopped; }

	// The bundle's modifiers (EffectBundle's alpha intensity, emit scale,
	// emit speed scale and age scale: the instance's +0x164, +0x138, +0x13c
	// and +0x12c, set by Wheel/AirDraftEffectBundle each tick, 0x00618bc0..).
	float alphaIntensity = 1, emitScale = 1, speedScale = 1, timeScale = 1;
	// The bundle's start age (modifier 4; BF2142.exe 0x00618d00 -> the
	// system's slot 0x80: RendDX9 0x1013ee80 sprite +0xd8, 0x10143ee0 mesh
	// +0xe0, 0x1013a2b0 trail +0xe0, 0x1013c690 non-screen-aligned +0xd8):
	// every particle or point born after it is set starts this many seconds
	// old (the births 0x1013fdc0, 0x10145660, 0x1013acb0, 0x1013d840: age
	// and age / life from it). Shipped: the land vehicles' damage smoke and
	// fire (StartAgeFromSpeed 0/8..0/12), so a moving wreck's smoke is born
	// older; the amphibious wake's range.
	float startAge = 0;
	// The emit direction's scale (the system's slot 0x70, RendDX9 0x1013e8b0
	// / 0x10143950 / 0x1013bfe0: +0x150 sprite, +0x148 mesh, +0x168 trail),
	// applied to emitDirection axis by axis (0x1013e930). Only the wheel
	// bundle's tick sets it (0x0061bd20 → 0x00618c80): (1, 1, -1) while its
	// vehicle runs backwards, so the spray flies the other way.
	Vec3 directionScale {1, 1, 1};
	// EffectBundle.speedFromPhysics (the exe's +0x45c, 0x00618ae0 each
	// tick): the velocity the particles inherit is set from outside, the
	// physics velocity's direction times the square root of its speed
	// (slots 0x40 / 0x4c, the instance's +0xcc / +0xdc), not taken from the
	// system's own moves.
	bool physicsVelocity = false;
	Vec3 inheritedVelocity;
	// Whether it emits at all (started within the view distance and of a
	// quality the manager draws).
	bool active() const { return m_active; }
	// The frame of its last draw (sprite +0x124, trail +0x120, mesh +0x124;
	// stamped by the start 0x1013fb30 / 0x10144ab0 / 0x1013aa00 and by the
	// draws 0x10131680 / 0x10133920 / 0x10134b90 / 0x1013d570 when its box
	// meets the view frustum). Its emission (0x101403b0, 0x1013b4b0,
	// 0x10144d90, 0x1013e070) does nothing once the renderer's frame count
	// (DAT_101f8e58 slot 0x60) is more than 60 past it: a system out of
	// sight for 60 frames stops emitting, its clock running on, so that
	// when it is seen again the missed time is emitted at once (up to its
	// pool), as if it had never stopped. EffectManager::markViewed stamps it.
	uint32_t lastSeenFrame = 0;
	static constexpr uint32_t kUnseenFrames = 60; // 0x3c
	// Whether it is out of sight long enough to hold its emission.
	bool unseen(uint32_t frame) const { return lastSeenFrame + kUnseenFrames < frame; }
	// The box its particles span (+0xfc..+0x110: their positions grown by
	// the template's particleMaxSize, the system's place when it has none;
	// 0x101407c0), as a centre and radius (0x1012ea80 / 0x1012ea30).
	void bounds(Vec3& centre, float& radius) const;
	// The distance LOD's emission scale now (+0x134).
	float lodScale() const { return m_lodScale; }

	// Attached: the anchor (0 for none), the mount (where the bundle sits
	// in the anchor's space) and the system's place in the bundle.
	uint32_t anchor = 0;
	uint32_t tag = 0;
	uint32_t bundle = 0; // the AttachedBundle it belongs to (0: none)
	// The life left of the world-spawned EffectBundle it came from (the
	// bundle's +0x194: its timeToLive sampled at creation, 0x00619700; an
	// unparented infinite one gets 0.5 s, 0x00618600; run down by the tick
	// while above 0 and the bundle stopped below 0, 0x0061a760). -1: none.
	float bundleLife = -1;
	// Its emission is held by an anchor or a living bundle: a looping or
	// emit-time-0 system emits until then.
	bool held() const { return anchor != 0 || bundleLife >= 0; }
	Vec3 mountPosition, localPosition;
	Mat3 mountRotation, localRotation;

private:
	void emit(game::Random& random, const Vec3& at, float age, const EmitContext& context);
	void simulate(Particle& p, float step, int steps, const EmitContext& context) const;
	void start(const EmitContext& context);
	void step(float seconds, game::Random& random, const EmitContext& context);

	const ParticleTemplate* m_template;
	bool m_started = false, m_active = true, m_prewarming = false, m_warmedStatic = false;
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
	std::array<float, 2> pivot {0, 0}; // the template's pivotOffset
	std::array<float, 4> frame {0, 0, 1, 1}; // the flipbook frame's u0 v0 u1 v1
};

// The view the particles are drawn from: their fade by distance per LOD
// class (startLodPercentageOfLodDistance of the class's distance to it;
// 0x10132e90), with the manager's settings.
struct ParticleView {
	Vec3 eye;
	Vec3 forward {0, 0, 1};
	const scene::ParticleSystemSettings* settings = nullptr;
	// A zoomed first person (the systems' +8: drawn with the first-person
	// meshes' view, see showZoomed). It hides none of them.
	bool zoomed = false;
	// The alpha a particle keeps at view depth `depth` in LOD class `lod`.
	float fade(float depth, int lod) const;
	bool shows(const ParticleTemplate&) const { return true; }
	float nearPlane = 0; // the camera's
	// 0x101318e0's near fade of a particle of `size` at view depth `depth`
	// (the factor that replaces the distance fade): with d the depth past
	// the near plane under 6 m (+0x270) and the size over 0.15 (+0x278),
	// f = max(1 - (size - 0.15) x 4 (+0x27c), 0) and t = (6 - d) x 0.5
	// (+0x274): (1 - t) + t f while t <= 1, else f; below 0.01 (+0x280)
	// the particle isn't drawn. -1: no near fade here.
	static float nearFade(float depth, float nearPlane, float size)
	{
		const float d = depth - nearPlane;
		if (d >= 6.0f || size <= 0.15f) {
			return -1.0f;
		}
		const float s = (size - 0.15f) * 4.0f;
		const float f = s <= 1.0f ? 1.0f - s : 0.0f;
		const float t = (6.0f - d) * 0.5f;
		return t <= 1.0f ? (1.0f - t) + t * f : f;
	}
};

// A mesh particle: its mesh scaled by `size`, turned and placed
// (MeshParticleMesh.fx: one bone per particle, the mesh times globalScale,
// coloured by the colour graph and faded by the transparency graph).
struct MeshSprite {
	Vec3 position;
	Mat3 rotation;
	float size; // signed mesh scale (negative values mirror the geometry)
	std::array<float, 4> color;
	float stretch = 1; // along its z (directionalScale)
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
	// decalNormalMapName (the exe's row 0x0052cf1a, executor 0x0052cf97;
	// 8 shipped decals: the scorches, concrete, glass and armour), without
	// ".dds": RendDX9 keeps the map at the decal's +0x34 and flags it +0x38.
	std::string normalMap;
	// isStatic (exe 0x0052cd51 -> the RendDX9 template's slot 10,
	// 0x10167b70: its byte +0x20): such a decal is added to an object's set
	// even while its owner is out of view (DecalSet slot 4 0x10166410).
	bool isStatic = false;
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

// A vertex of a decal clipped to the surface it lies on (RendDX9 keeps
// float3 position + float2 uv, 20 bytes, at decal +0x70 / count +0x74).
struct DecalVertex {
	Vec3 position;
	float u = 0, v = 0;
};

struct Decal {
	std::string texture;
	std::string normalMap; // the template's decalNormalMapName (empty: none; decal +0x34 / +0x38)
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
	// On a part of that object (-1: the object itself): a collisionPart
	// child the round met, which in BF2142.exe is an object of its own
	// with its own decal set (every shipped one has hasCollisionPhysics,
	// 0x0063c000) and its own mesh instance (+0x4c, 0x0063ccc0), which the
	// add (0x005a36d0: the hit object's slot 0xd4 set, its +0x4c mesh)
	// clips the decal to and the set draws at the part's matrix: position,
	// normal and tangent are then in the part's space (its index among the
	// object's posed parts, GameWorld's raycastBody), and the decal turns
	// and moves with it.
	int part = -1;
	// A world decal's static object (0: the terrain): the exe's add
	// (0x005a36d0 -> DecalSet slot 4 0x10166410) passes the hit's mesh,
	// whose faces 0x10165f60 clips it to.
	uint32_t surface = 0;
	float age = 0; // seconds (RendDX9 0x10164f10; it goes at DecalManager.timeToLive)
	// A static decal's id in its set (+0x1d, 1..255; 0 for the others).
	// The add (DecalSet slot 4 0x10166410) puts a decal whose template
	// isStatic in the set's second list (+0x2c, 0x10164710) under the set's
	// next id (+0x30, back to 1 after 255) and returns it; that list is
	// never aged nor cut to the ring's size, and only slot 5 (0x10164e70)
	// takes one out, by its id. No shipped decal template sets isStatic.
	uint8_t staticId = 0;
	// The renderer's: the decal clipped to the terrain or its object's faces
	// (clipDecal; in the object's space on a moving one), made the first
	// time it is drawn; null until then.
	mutable std::shared_ptr<const std::vector<DecalVertex>> clipped;
	// Whether its owner object was drawn the last frame (the set's +0x19,
	// set by the ObjectDrawer 0x100b84b0 / 0x100b8380 / 0x100b71c0 for the
	// objects it draws and cleared by the decal draw 0x10164040): a set
	// whose owner wasn't ages twice as fast (0x10164f10); world decals
	// always count as drawn. The renderer keeps it.
	mutable bool ownerDrawn = true;
};

// RendDX9's decal clipping (DecalSet slot 4 0x10166410 -> 0x10165f60 when
// DecalManager.decalGeomClipAngle, +0x324c, is over 0 and there is terrain;
// else the flat square 0x10164940). The decal's square (corners centre +-
// tangent x half +- (normal x tangent) x half, the frame's coordinates at
// them) is cut, per surface triangle (0x101655c0), to the prism of the
// triangle's three edge planes (normalize(edge x n), 0x10165020,
// Sutherland-Hodgman with uv carried along); what is left (3 or more
// corners) is laid onto the triangle's plane (the flag 0x101f79d0),
// lifted by normalOffset along the decal's normal and fanned (0, i, i + 1).
// A triangle whose normal is more than maxAngleDegrees from the decal's is
// skipped (acos(n . normal) > angle x 0.0174533; the terrain path passes an
// angle of 0: none skipped). At most 128 vertices (0x7f). The original walks
// the triangles from the hit one (the hit's face, 0x101662f7) through their
// edge neighbours (mesh slot 0x38, 0x10165a02; the terrain's table
// 0x101f7880), at most 4 deep (0x101f79d4, 0x10165a60), each once (the set
// 0x1021e3b8), stopping where the square lies inside a face (0x101659c2), so
// it never meets a face of the mesh away from the hit. The rewrite takes the
// triangles it is given (the terrain's 3 x 3 cells about the hit,
// 0x10164d70; a mesh's faces in the grid column over the hit, any height)
// and stands in for the walk by skipping a triangle whose plane is further
// from the centre than the square's half diagonal: the hit face (distance 0)
// and every face folded off it inside the square stay, a parallel face above
// or below (a Titan's upper hull over the hit) doesn't get a copy.
// Triangles facing away from the decal are skipped. Empty if nothing is
// left: RendDX9 then frees the decal (0x10164660).
std::vector<DecalVertex> clipDecal(const Vec3& centre, const Vec3& normal, const Vec3& tangent, float halfSize,
	const std::array<float, 4>& frame, const std::vector<std::array<Vec3, 3>>& triangles, float maxAngleDegrees,
	float normalOffset);

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
	// lightType Spot: lit within the cone about `direction` (world space)
	// of width `cone` (LightSourceLook::cone).
	bool spot = false;
	Vec3 direction {0, 0, 1};
	float cone = 0.3927f;
	// affectLightmappedObjects: whether lightmapped surfaces take it too
	// (the static lights of the levels leave them to their lightmaps).
	bool affectsLightmapped = true;
	// A placed light's template range (its range2 flickers: 0x0064a920);
	// below 0 for the effects' lights.
	float templateRange2 = -1;
	// The template's speccolor: the skinned meshes' Light record's
	// specularColor (+0x28, template slot 0x170, 0x10162070).
	Vec3 specColor {1, 1, 1};
	// supportsEmitter: bundled and skinned meshes take only lights with it
	// (their filter 0x8b09's bit 0x8000, template slot 0x138).
	bool supportsEmitter = true;
};

// What a LightSource template sets (BF2142.exe's LightSource template,
// class 0x94c7, as its writer 0x00617030 lists the fields): lightType
// (+0x70: Directional, Point, Spot ...), isDynamic (+0x64),
// affectLightmappedObjects (+0x6d), affectingType (+0x90),
// attenuationRange1 / 2 (+0xa0 / +0xa4), color (+0xa8), speccolor (+0xb4),
// supportsEmitter (+0x6c), coneAngle1 / 2 (+0xd0 / +0xd4), enabled;
// flicker (+0xe4, clamped to 0..1 by 0x00616cf0). The defaults are the
// constructor's (0x00616d60: isDynamic 0, supportsEmitter and
// affectLightmappedObjects 1, both colours white). The colour is used as it
// is: every in-game reader takes the template's colour (slot 0x168) or the
// light object's copy of it (`+0x184`, slot 0x1a0), and `intensity` (slot
// 0x1e8) is read in RendDX9 only by the lightmap generator (0x1006a310).
// The template has no lifetime of its own (none of its setters,
// 0x00528f5a..0x0052b6ea, is one).
struct LightSourceLook {
	bool enabled = true;
	bool spot = false;
	bool directional = false;
	bool dynamic = true;
	bool affectsLightmapped = true;
	int affectingType = 0;
	Vec3 color {1, 1, 1};
	Vec3 specColor {1, 1, 1};
	bool supportsEmitter = true;
	float range1 = 0, range2 = 1;
	Vec3 direction {0, 0, 1}; // in the light's space
	// The shaders' ConeAngle: coneAngle2 in radians. BF2142.exe's light
	// object (0x0064b170) clamps coneAngle1 / 2 to 180 and keeps each in
	// degrees and radians (+0x1a0 / +0x1a4, +0x1a8 / +0x1ac); its slot
	// 0x1c8 (0x0064aad0) hands RendDX9 coneAngle2's radians, which go to
	// ConeAngle as they are (0x1006bf8a). coneAngle1 reaches no shader.
	float cone = 0.3927f; // 22.5 degrees, the template's default
	float flicker = 0;
};
LightSourceLook lightSourceOf(const world::ObjectTemplate& t);

// The debris emitters: BF2142.exe's own Emitter template (class 0x9475,
// vtable 0x0097d310, defaults 0x00620d50: delay 0, timeToLive 0.1,
// intensity 11) and its object's update (0x0061f360). For timeToLive
// seconds after its delay it makes one object of its `template` (or of one
// of alternateTemplate0..4: index floor(u (n + 1)), the main one past the
// last) every 1 / intensity seconds (0x0061ef80: intensity x its curve over
// the life, 1 unless scripted), each:
//  - at the emitter plus relativePositionIn{Right,Up,Dof} along its axes,
//    moved on by its velocity for its share of the tick;
//  - turned about the emitter's forward by startRotation + zRotation
//    rounded to zRotationSnap (radians, 0x005b1a70);
//  - leaving at positionalSpeedIn{Right,Up,Dof} along the emitter's axes
//    (plus the emitter's own velocity x emitterSpeedScale with
//    addEmitterSpeed), spinning at rotationalSpeedIn{Dof,Up,Right}
//    (the physics node's slot 0x6c), unless noPhysics.
// A Particle template (class 0x947b) object is a debris piece (its object
// 0x00621250 / update 0x00621480): it lives timeToLive (below 0: 0.5 s)
// seconds, drawn at size x sizeModifier (the axes above 0), its alpha the
// transparencyGraph at its age's share of the life (clamped, 0x00621210)
// times 1 - Transparency, its gravity times gravityModifier and drag. As
// the exe gives each piece its own physics node (physicsType 3, Mesh, for
// the shipped pieces with a collisionMesh; the object's 0x00621250 makes
// it through the template's hasMobilePhysics, 0x005f6ba0), the rewrite's
// piece is a rigid body (physics::RigidBody: Physics' -14.73 x
// gravityModifier, the 0.99-a-tick damping, the box inertia of its mesh)
// whose contact points, the corners of its collision mesh's vehicle LOD
// box (collisionPart), meet the terrain (collideWithGround, as a
// vehicle's points do) and the static objects (each point's motion in the
// tick against their faces, CollisionWorld::segmentContact), the contacts
// solved as ResponsePhysics does; it sleeps as a PhysicsNode does
// (physics::SleepState: a second still). Without the debris world
// (setDebrisWorld) it falls onto the height `setGround` gives. Another
// template made by an emitter (an effect bundle, a particle system) is
// started there instead.
struct DebrisEmitterTemplate {
	game::RandomDistribution delay, timeToLive, intensity;
	game::RandomDistribution relativePosition[3]; // right, up, dof
	game::RandomDistribution positionalSpeed[3];  // right, up, dof
	game::RandomDistribution rotationalSpeed[3];  // dof, up, right (as the exe hands them on)
	game::RandomDistribution startRotation, zRotation;
	float zRotationSnap = 1;
	bool addEmitterSpeed = false;
	float emitterSpeedScale = 1;
	bool noPhysics = false;
	std::string objectTemplate;
	std::vector<std::string> alternates;
	static DebrisEmitterTemplate fromTemplate(const world::ObjectTemplate& t);
};
struct DebrisPieceTemplate {
	std::string geometry;
	int part = 0; // geometryPart
	std::string collisionMesh;
	int collisionPart = 0;
	uint32_t material = 0;
	// The body's contact points (object space) and box, from the collision
	// mesh once it is looked up (a point at the origin and a 0.5 m box
	// without one).
	bool shapeResolved = false;
	std::vector<Vec3> points;
	Vec3 boxSize {0.5f, 0.5f, 0.5f};
	game::RandomDistribution timeToLive, size, transparency, gravityModifier, drag;
	Graph transparencyGraph {0, 0, 0, 1};
	Vec3 sizeModifier {1, 1, 1};
	static DebrisPieceTemplate fromTemplate(const world::ObjectTemplate& t);
};
struct DebrisPiece {
	const DebrisPieceTemplate* look = nullptr;
	Vec3 position, velocity, spin; // spin: radians a second about the world's axes
	Mat3 rotation;
	float age = 0, life = 10, size = 1, transparency = 0, gravity = 1, drag = 0;
	bool resting = false;
	physics::RigidBody body;   // its physics node (position, rotation, velocity, spin mirror it)
	physics::SleepState sleep;
	// What it is drawn with now.
	float alpha() const;
	Vec3 scale() const;
};

// An EffectBundle's modifiers (the template's block at +0x298, vtable
// 0x00979278; the setters 0x0053cbb9..0x0053f0e0 forward to its slots
// 0x18..0x168): six of them, 0 alpha intensity, 1 emit scale, 2 emit speed
// scale, 3 age scale, 4 start age, 5 sound intensity; each a Range (min,
// max; +4 + 8i), a Used flag (+0x154 + i) and six "From" mappings a/b
// (+0x34 + 8 (6i + j)): j 0 FromSpeed, 1 FromRotationalSpeed, 2
// FromAcceleration, 3 FromAltitude, 4 FromEngine, 5 FromSpeedInDof.
//
// The bundle's update (0x00618e30, at its start 0x0061a1c0 and each tick
// 0x0061a760): when any mapping of any modifier is set (0x0064be90), each
// modifier with one set becomes min + (max - min) x the product of its
// mappings (0 at a, 1 at b, clamped) of: the speed of the root object's
// physics at the bundle (slot 0xac; none for a bundle on its own), the
// rotational part of it (that less the body's velocity, slot 0x58), the
// size of slot 0xb0 at the bundle, the altitude (minDistanceUnderWater
// Surface +0x3f4 below 0: the height over the terrain, DAT_00a83e58 slot
// 0x68; else the depth under the water, slot 0x94, at least 0), the
// throttle of the Engine the bundle sits on (template 0x9476 with +0x24c,
// 0x0064c3b0) and the speed along the bundle's z (at least 0). Otherwise
// each modifier with Used on is set to its min. The values are set on the
// systems (0x00618bc0..0x00618d00: their slots 0x58 / 0x60 / 0x68 / 0x78 /
// 0x80, RendDX9 +0x164 / +0x138 / +0x13c / +0x12c / +0xd8), replacing the
// age scale randomAgeFactor gave; the sound's (5) is the bundle's +0x1d8,
// clamped to 0..1.
struct BundleModifiers {
	struct Inputs {
		float speed = 0, rotationalSpeed = 0, acceleration = 0, altitude = 0, engine = 0, speedInDof = 0;
	};
	struct Modifier {
		float min = 1, max = 1;
		bool used = false;
		std::array<std::array<float, 2>, 6> from {}; // by input j
		bool mapped() const;
	};
	std::array<Modifier, 6> modifiers;
	bool anyMapping = false;           // 0x0064be90
	float minUnderWater = -0.25f;      // minDistanceUnderWaterSurface (+0x3f4)
	// maxDistanceUnderWaterSurface (+0x3f8, -1 by default, 0x0061a9b0).
	// With the minimum at 0 or more an attached bundle runs only while its
	// depth under the water lies within [min, max] (0x0061a760): it starts
	// on entering the range (unless running, +0x18e) and stops on leaving;
	// on a soldier only while he isn't seated (the parent's +0x34). Shipped:
	// the amphibious APCs' wakes and bow spray, the soldiers' e_sold_swim,
	// e_sinking_vehicle, the web wheels' water spray.
	float maxUnderWater = -1;
	bool depthGated() const { return minUnderWater >= 0; }
	bool inDepth(float depth) const { return depth >= 0 && depth >= minUnderWater && depth <= maxUnderWater; }
	static BundleModifiers fromTemplate(const world::ObjectTemplate& t);
	// The modifiers the update sets: bit i for modifier i, its value in out[i].
	unsigned evaluate(const Inputs& inputs, std::array<float, 6>& out) const;
	bool any() const;
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
	// in the anchor's space, `tag` names it for stopAttached. Unless
	// `driven` is false (ObjectEffects drives its own), a bundle with
	// speed mappings or speedFromPhysics follows the anchor's motion each
	// update (stepAttachedBundles).
	bool spawnAttached(const std::string& name, uint32_t anchor, const Vec3& position, const Mat3& rotation = {},
		View view = View::Any, uint32_t tag = 0, bool driven = true);
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
	// The ground the colliding particles bounce off (collisionEnable).
	void setGround(std::function<float(float x, float z)> height) const { m_context.groundHeight = std::move(height); }
	// What the debris pieces' bodies collide with: the terrain (height and
	// normal; its material), the static objects' faces (world space), the
	// pieces' collision meshes by name and the contacts' materials (the
	// piece's, the surface's).
	struct DebrisWorld {
		physics::Ground ground;
		const physics::CollisionWorld* statics = nullptr;
		std::function<const physics::CollisionMeshFile*(const std::string& name)> collisionMesh;
		std::function<physics::ContactMaterial(uint32_t piece, uint32_t surface)> material;
	};
	void setDebrisWorld(DebrisWorld world) { m_debrisWorld = std::move(world); }
	// After a frame's view is known: one more frame drawn, and every system
	// whose box `inView` (centre, radius) accepts is stamped as seen
	// (Emitter::lastSeenFrame; RendDX9's draws 0x10131680, 0x10133920,
	// 0x10134b90, 0x1013d570 stamp the systems they draw that meet the
	// view frustum, 0x101a4720). Const as setCamera: the draw's bookkeeping.
	void markViewed(const std::function<bool(const Vec3& centre, float radius)>& inView) const;
	uint32_t framesViewed() const { return m_context.frame; }
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
	// One of them (BundleModifiers' 0..5) of those emitters (5, the sound
	// intensity, of the bundle's held start sound: bundleLoops).
	void setModifier(uint32_t anchor, uint32_t tag, int modifier, float value);
	// Their emit direction's scale (Emitter::directionScale).
	void setDirectionScale(uint32_t anchor, uint32_t tag, const Vec3& scale);
	// A bundle's modifiers (read once per template).
	const BundleModifiers& bundleModifiers(const world::ObjectTemplate& bundle);
	// BundleModifiers' altitude at a point: over the ground (the
	// collisionEnable ground), or under the water when `underWater`.
	float altitudeAt(const Vec3& at, bool underWater) const;
	// The WindAffectors of the bundles spawned on their own (explosions)
	// still blowing, appended to `out` (the WindManager's local winds).
	void appendLocalWinds(std::vector<scene::WindSettings::LocalWind>& out) const;
	// The water's height over a point (below 0 above the water or with
	// none): the depth gate's (BundleModifiers::maxUnderWater).
	float waterDepthAt(const Vec3& at) const;
	// Sprites alive in all the sprite systems (the pool's use).
	int poolUsed() const { return m_poolUsed; }
	// EffectBundle.speedFromPhysics: the physics velocity (world) at an
	// anchor's bundles spawned with `tag` (0x00618ae0).
	void setPhysicsVelocity(uint32_t anchor, uint32_t tag, const Vec3& velocity);

	// The Sound children of the bundles started since the last call (the
	// bundle's start sound, S_<bundle>_Start), where they start.
	struct StartedSound {
		std::string name;
		Vec3 position;
		// The bundle it is the start sound of, and that bundle's sound
		// intensity at the start (modifier 5; 0 unless it sets one).
		const world::ObjectTemplate* bundle = nullptr;
		float intensity = 0;
	};
	std::vector<StartedSound> takeSounds() { return std::exchange(m_startedSounds, {}); }
	// Those started since `mark` (soundsStarted() before a spawn), taken.
	size_t soundsStarted() const { return m_startedSounds.size(); }
	std::vector<StartedSound> takeSoundsFrom(size_t mark)
	{
		std::vector<StartedSound> taken;
		if (mark < m_startedSounds.size()) {
			taken.assign(m_startedSounds.begin() + static_cast<std::ptrdiff_t>(mark), m_startedSounds.end());
			m_startedSounds.resize(mark);
		}
		return taken;
	}

	// The looping Sound children (loopCount 0) of the bundles started on
	// anchors that set their sound intensity, held while their bundle runs. The bundle's "Start" sound
	// (made by the bundle's constructor 0x00619700) has its volume and pitch
	// envelopes bound (its slots 0x48 / 0x50) to the bundle's sound
	// intensity (+0x1d8: modifier 5, set by 0x00618e30 when
	// SoundIntensityUsed or a mapping is; 0 otherwise, as the constructor
	// leaves it). Each with a key stable while it plays, where it is now and
	// that intensity: the caller keeps the loop on while it is listed
	// (GameSounds::loop). A one-shot detached loop would stop at once
	// (Sound::release), so these are never in takeSounds.
	struct BundleLoop {
		uint64_t key = 0;
		std::string name;
		Vec3 position;
		float intensity = 0;
	};
	std::vector<BundleLoop> bundleLoops() const;

	size_t emitters() const { return m_emitters.size(); }

	// Leaves a decal template's decal at a surface point (the oldest go
	// beyond kMaxDecals); false if there is no such decal template.
	// `surface` is the static object the round hit (0: the terrain), whose
	// collision faces the renderer clips the decal to.
	bool spawnDecal(const std::string& name, const Vec3& position, const Vec3& normal, uint32_t surface = 0);
	// The same on a moving object placed at `objectPosition`, turned by
	// `objectRotation` (yaw / pitch / roll degrees): kept in its space so it
	// moves with it. Refused (false) while the object's set saw its owner
	// unseen at its last ageing (the set's +0x1a, copied from +0x19 by
	// 0x10164f10; 0x10166410 refuses a decal there unless its template
	// isStatic): the rewrite's set is the object's decals' ownerDrawn.
	bool spawnDecal(const std::string& name, const Vec3& position, const Vec3& normal, uint32_t object,
		const Vec3& objectPosition, const Vec3& objectRotation);
	// The same on `object`'s part `part` (Decal::part) whose frame in the
	// world is now `partRotation` (rows x, y, z) at `partPosition`.
	bool spawnPartDecal(const std::string& name, const Vec3& position, const Vec3& normal, uint32_t object, int part,
		const Vec3& partPosition, const Mat3& partRotation);
	// The id the last spawnDecal gave its decal (0x10166410's result: a
	// static decal's id in its set, else 0).
	uint8_t lastDecalId() const { return m_lastDecalId; }
	// Takes a static decal out of an object's set (0: the world's) by its
	// id (DecalSet slot 5, 0x10164e70); false if there is none.
	bool removeStaticDecal(uint32_t object, uint8_t id);
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
		for (const auto& [geometry, sprites] : m_placedMeshes) {
			if (!sprites.empty()) {
				draw(geometry, true, sprites);
			}
		}
	}
	// Meshes placed from outside, replaced each tick (the projectiles'
	// tracers, GameWorld::placeTracers): drawn by forEachMeshGroup after the
	// mesh systems', added. A sprite's rotation rows may carry a scale.
	void setPlacedMeshes(std::vector<std::pair<std::string, std::vector<MeshSprite>>> groups)
	{
		m_placedMeshes = std::move(groups);
	}
	const std::vector<std::pair<std::string, std::vector<MeshSprite>>>& placedMeshes() const { return m_placedMeshes; }
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
		// The Back systems first, then the Front ones (0x10131b70).
		for (const bool back : {true, false}) {
			for (const auto& emitter : m_emitters) {
				const ParticleTemplate& t = emitter->particleTemplate();
				if (t.mesh || t.trail || t.nonScreenAligned || t.heatShimmer != shimmer || !view.shows(t) || t.drawBack != back) {
					continue; // forEachMeshGroup, forEachWorldGroup
				}
				m_sprites.clear();
				collect(*emitter, m_sprites, view);
				if (!m_sprites.empty()) {
					draw(emitter->particleTemplate().texture, emitter->particleTemplate().additive, m_sprites);
				}
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
			if ((!t.trail && !t.nonScreenAligned) || !view.shows(t)) {
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
	// The debris pieces alive (DebrisPiece) and the debris emitters still
	// emitting; at most kMaxDebris pieces (the oldest go first: a guard of
	// the rewrite's, the exe's GSDynamicObjectsLimit is read and dropped).
	const std::vector<DebrisPiece>& debris() const { return m_debris; }
	size_t debrisEmitters() const { return m_debrisEmitters.size(); }
	static constexpr size_t kMaxDebris = 384;

private:
	struct Anchor {
		Vec3 position;
		Mat3 rotation;
	};
	void placeAttached(Emitter& emitter) const;
	std::map<const world::ObjectTemplate*, BundleModifiers> m_bundleModifiers;
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
	struct LiveWind {
		scene::WindSettings::LocalWind wind;
		float age = 0, life = -1; // life below 0: as long as the manager
	};
	std::vector<LiveWind> m_winds;
	struct LiveDebrisEmitter {
		const DebrisEmitterTemplate* look = nullptr;
		Vec3 position;
		Mat3 rotation;
		View view = View::Any;
		int depth = 0;
		float delay = 0, timeToLive = 0.1f, elapsed = 0, next = 0, intensity = 11;
	};
	std::vector<LiveDebrisEmitter> m_debrisEmitters;
	std::vector<DebrisPiece> m_debris;
	DebrisWorld m_debrisWorld;
	void resolveShape(DebrisPieceTemplate& t);
	void stepPiece(DebrisPiece& p, float seconds);
	std::map<const world::ObjectTemplate*, std::unique_ptr<DebrisEmitterTemplate>> m_debrisEmitterTemplates;
	std::map<const world::ObjectTemplate*, std::unique_ptr<DebrisPieceTemplate>> m_debrisPieceTemplates;
	void startDebrisEmitter(const world::ObjectTemplate& t, const Vec3& position, const Mat3& rotation, int depth, View view);
	void stepDebris(float seconds);
	void emitDebris(LiveDebrisEmitter& emitter, float lead);
	std::map<const world::ObjectTemplate*, DecalTemplate> m_decalTemplates;
	std::vector<Decal> m_decals;
	std::map<uint32_t, uint32_t> m_staticDecalIds; // each set's +0x30, by object (0: the world's)
	uint8_t m_lastDecalId = 0;
	game::Random m_decalRandom {0x2d5c1e37};
	mutable std::vector<MeshSprite> m_meshSprites;
	std::vector<std::pair<std::string, std::vector<MeshSprite>>> m_placedMeshes;
	mutable std::vector<WorldVertex> m_worldVertices;

	const world::ObjectTemplateManager& m_templates;
	game::Random m_random;
	std::map<const world::ObjectTemplate*, std::unique_ptr<ParticleTemplate>> m_particleTemplates;
	std::vector<std::unique_ptr<Emitter>> m_emitters;
	std::map<uint32_t, Anchor> m_anchors;
	// A bundle spawned on an anchor whose tick (BF2142.exe 0x0061a760) sets
	// its systems from its motion: the modifiers (0x00618e30) and the
	// speedFromPhysics velocity (0x00618ae0). The armour's damage smoke and
	// fire (armor.addArmorEffect) are such bundles: their alpha, emission,
	// speed, age scale and start age grow with the vehicle's speed.
	struct AttachedBundle {
		uint32_t id = 0, anchor = 0, tag = 0;
		const world::ObjectTemplate* bundle = nullptr;
		Vec3 mountPosition;
		Mat3 mountRotation;
		bool speedFromPhysics = false;
		bool hasLast = false;
		Vec3 lastPoint, lastPointVelocity, lastOrigin;
	};
	std::vector<AttachedBundle> m_attachedBundles;
	uint32_t m_nextBundle = 0;
	// A bundle's looping start sound (bundleLoops): its place in the
	// bundle (local), the bundle's mount on the anchor, the AttachedBundle
	// driving it (0 none) and the intensity last set.
	struct HeldSound {
		uint64_t key = 0;
		uint32_t anchor = 0, tag = 0, bundle = 0;
		std::string name;
		Vec3 localPosition, mountPosition;
		Mat3 mountRotation;
		float intensity = 0;
	};
	std::vector<HeldSound> m_heldSounds;
	uint64_t m_nextHeldSound = 0;
	void stepAttachedBundles(float seconds);
	// spawnDecal into the set of `set` (an object's id, 0 the world's).
	bool addDecal(const std::string& name, const Vec3& position, const Vec3& normal, uint32_t surface, uint32_t set);
	uint8_t nextStaticDecalId(uint32_t object);
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
// A GroundEffectBundle (0x9486) shows nothing itself: on event 0 (its
// parent EffectBundle's start, 0x0061a1c0, or its weapon's fire) it looks
// maxTriggerDistance down its -y and starts the ground's material effect
// there (0x0061de40; ObjectEffects::groundTrigger).
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
		// kit's disableBreathEffect, not in a vehicle, not the local player
		// in first person).
		bool coldBreath = false;
		// Soldiers: seated in a vehicle (their depth-gated bundles stay off).
		bool seated = false;
		// Its bundles start in their first-person form (the EffectBundle's
		// test 0x006187c0: the local player looks through the inside or nose
		// view, player slot 0x1bc, sits under the same root, and neither his
		// control object's nearest PlayerControlObject nor the bundle's is
		// neverDrawAs1p, slot 0x14c; GameWorld::firstPersonEffects): their
		// systems with showInFirstPerson or showInZoomedMode, else those
		// with showInThirdPerson (0x0061a1c0). Taken as each bundle starts.
		bool firstPerson = false;
		// Its wheels' contact this tick (the vehicle's springs, by template
		// name): a WheelEffectBundle under a wheel's spring is on while it
		// touches, at its contact point (world). Empty: the bundle probes
		// the ground under itself.
		struct WheelContact {
			std::string spring;
			bool contact = false;
			Vec3 point;
		};
		std::vector<WheelContact> wheels;
		// The weapons in its tree that fired a round this tick (template
		// names): the fire's event 0 to each weapon's children, which starts
		// the bundles whose startOnEffects is 0 (0x0061a4e0: the muzzle
		// flash and smoke) and sets off the GroundEffectBundles (0x0061e090).
		struct WeaponFire { std::string weapon, seat; };
		std::vector<WeaponFire> firedWeapons;
		// Events 0 and 1 to the object's own children this update (a
		// round's: its engine's start, 0x005b2c10, sends 0x44 then 0; its
		// stop, 0x005b2ca0 at a lost target or the round's end 0x005b4cb0,
		// sends 1 then 0x45). 0x0061a4e0 starts on event 0 the bundles whose
		// startOnEffects is 0 and stops on event 1 those whose
		// stopOnEffects is 0; 0x44 and 0x45 do nothing to a bundle.
		bool startEvent = false;
		bool stopEvent = false;
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
	// A bone part's local matrix (boneIndex, 0x0063d470: row-major, row
	// vectors, the translation in 12..14), which replaces the part's own
	// place in its parent; the game's partBone, as the renderer draws it.
	using PartBone = std::function<bool(uint32_t object, const std::string& part, std::array<float, 16>& local)>;
	void setPartBone(PartBone bone) { m_partBone = std::move(bone); }

	explicit ObjectEffects(const world::ObjectTemplateManager& templates) : m_templates(templates) {}
	// Once a tick: the objects there are now. Starts, moves and stops their
	// bundles in `effects` (as seen from `view`).
	void update(const std::vector<ObjectState>& objects, EffectManager& effects, const PartPose& partPose = {},
		EffectManager::View view = EffectManager::View::ThirdPerson);
	size_t tracked() const { return m_objects.size(); }
	// A firearm's fire away from its object (a commander's strike firearm,
	// moved over the target by 0x005a0bf0): event 0 to its own children at
	// `at` / `turn` (the firearm's world place): the bundles whose
	// startOnEffects is 0 start there, the GroundEffectBundles set off the
	// ground's effect below (GEB_emp_strike).
	void weaponFiredAt(const world::ObjectTemplate& weapon, const Vec3& at, const Mat3& turn, EffectManager& effects,
		EffectManager::View view = EffectManager::View::ThirdPerson);

	// A bundle of a template tree: its place in the root's space.
	struct Placed {
		const world::ObjectTemplate* bundle;
		std::vector<std::string> path; // the children from the root to it (for part poses)
		std::vector<const world::ChildTemplate*> chain; // the same children's links, to place it again
		Vec3 position;
		Mat3 rotation;
		uint32_t startOnEffects = 0, stopOnEffects = 0;
		bool startAtCreation = false;
		bool speedFromPhysics = false; // EffectBundle.speedFromPhysics (+0x45c)
		bool onEngine = false; // its parent is an Engine (BundleModifiers' FromEngine)
		// Its place in the world and its velocity there last tick (the
		// modifiers' rotational speed and acceleration).
		Vec3 lastPoint, lastPointVelocity;
		bool hasLastPoint = false;
		// WheelEffectBundle / AirDraftEffectBundle: the ground picks the effect.
		enum class Kind { Plain, Wheel, AirDraft, Wind, GroundTrigger } kind = Kind::Plain;
		// GroundTrigger (a GroundEffectBundle, class 0x9486): the bundle
		// whose start sets it off (its parent EffectBundle's index here,
		// -1 when a weapon's fire does), its material (slot 0xbc) and
		// maxTriggerDistance (+0x23c, 3 by the constructor 0x0061e0b0).
		int parent = -1;
		uint32_t material = 0;
		float maxTriggerDistance = 3;
	};
	// The local winds of the objects' WindAffector components this tick
	// (a template in the tree with the component, placed with its parts):
	// what the WindManager holds (scene::WindSettings::localWinds).
	const std::vector<scene::WindSettings::LocalWind>& localWinds() const { return m_localWinds; }
	// A WindAffector template's wind, at `position` turned by `rotation`.
	static scene::WindSettings::LocalWind localWindOf(const world::ObjectTemplate& t, const Vec3& position,
		const Mat3& rotation);
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
		uint32_t object = 0, const PartPose& partPose = {}, BundleCache* cache = nullptr, const PartBone& partBone = {});

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
	// A GroundEffectBundle's event 0 (0x0061de40): the ground below it.
	void groundTrigger(const ObjectState& object, Placed& bundle, EffectManager& effects, const PartPose& partPose,
		bool created);
	// The same from a world point and frame (`ignore`: the object the ray passes).
	void groundTriggerAt(const Vec3& at, const Mat3& turn, const Placed& bundle, uint32_t ignore, EffectManager& effects);
	// Starts a plain bundle (its event), then sets off its GroundEffectBundles.
	void startBundle(const ObjectState& object, Tracked& tracked, size_t index, EffectManager& effects,
		const PartPose& partPose, EffectManager::View view, bool created);
	GroundProbe m_probe;
	MaterialEffect m_materialEffect;
	std::map<const world::ObjectTemplate*, GroundBundle> m_groundBundles;
	const world::ObjectTemplateManager& m_templates;
	std::map<uint32_t, Tracked> m_objects;
	BundleCache m_hasBundles;
	PartBone m_partBone;
	std::vector<scene::WindSettings::LocalWind> m_localWinds;
	// A bundle's place along its chain under the parts' current poses.
	static void place(const Placed& bundle, uint32_t object, const PartPose& partPose, Vec3& position, Mat3& rotation,
		const PartBone& partBone = {});
public:
	// The tag the breath bundle is spawned with on its soldier.
	static constexpr uint32_t kBreathTag = 0xb7ea7;
};

// Rain and snow (RendDX9's rain manager, Shaders/Rain.fx): particleCount
// particles in a box of emitRadius about the camera plus offset, falling
// along particleVelocity x particleSpeed and wrapped round the box (the
// original moves 6 x 6 x 6 cells of 0.4 x emitRadius with the particles
// fixed in them, 0x1011b2a0, wrapping the cells round the box, 0x1011b720,
// and draws them 32 at a time, CELLPOSITIONS), swaying by one of 16
// deviation groups (DEVIATIONGROUPS, Weather::Deviation), faded from
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

	// A deviation group (RendDX9's rain manager, 28 bytes each from
	// +0x63a4): made afresh when angleSpeed or maxDeviation change
	// (0x1011c6a0 / 0x1011b880 flag a rebuild, 0x1011a790): two angles at
	// a random 0..359.9 degrees, its speeds angleSpeed x and y, its sizes a
	// random 0..0.999 of maxDeviation's (0 where that is 0). Each frame
	// (0x1011a860): with randomness's chance its speeds become a random
	// step (RainSettings), then its offset is (cos a x size.x, sin b x
	// size.y, sin a x size.z) and a and b turn by (angleSpeed.x + their
	// speed) x the frame's seconds (both by angleSpeed's x), wrapped past
	// 360. A particle's group (Rain.fx Data.y, the vertex's byte 0xd from
	// its record's +0x10, 0x1011cf00) is drawn once as it is made: rand()
	// & 15 after the three rand()s of its place (0x1011b1b0, 0x1011b0b0).
	struct Deviation {
		float angle[2] {};
		float size[3] {};
		float speed[2] {};
		Vec3 offset;
	};
	const std::array<Deviation, 16>& deviations() const { return m_deviations; }
	const std::vector<uint8_t>& groups() const { return m_groups; } // each particle's deviation group

private:
	void rebuildDeviations(const scene::RainSettings& settings);
	int rand(); // MSVC's rand() (the original's CRT), seeded 1
	std::array<Deviation, 16> m_deviations {};
	std::array<float, 2> m_deviationSpeed {};
	std::array<float, 3> m_deviationSize {};
	bool m_deviationsBuilt = false;
	uint32_t m_randState = 1;
	std::vector<Vec3> m_positions; // without the sway
	std::vector<uint8_t> m_groups; // their deviation groups, 0..15
	Vec3 m_centre;          // the box's centre (held while frozen)
	bool m_centreSet = false;
	std::vector<Drop> m_drops;
	float m_time = 0;
	game::Random m_random {0x5eed4a1};
};

} // namespace dice::hfe::effects
