#include "Effects.h"

#include "Physics/CollisionMeshFile.h"

#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dice::hfe::effects {

namespace {

constexpr float kDegrees = 3.14159265f / 180.0f;
// Effects with more particles than this lose the rest (a guard, not a
// value from the original).
constexpr size_t kMaxParticlesPerEmitter = 512;

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

Vec3 vector(const world::ObjectTemplate& t, const char* name, Vec3 fallback)
{
	if (const std::string* value = setting(t, name)) {
		std::sscanf(value->c_str(), "%f/%f/%f", &fallback.x, &fallback.y, &fallback.z);
	}
	return fallback;
}

Graph graph(const world::ObjectTemplate& t, const char* name, Graph fallback)
{
	const std::string* value = setting(t, name);
	return value ? Graph::parse(*value) : fallback;
}

// Any unit vector perpendicular to `v`.
Vec3 perpendicular(const Vec3& v)
{
	const Vec3 other = std::fabs(v.x) < 0.9f ? Vec3 {1, 0, 0} : Vec3 {0, 0, 1};
	Vec3 p = physics::cross(v, other);
	return p * (1.0f / std::sqrt(physics::lengthSquared(p)));
}

Vec3 normalized(const Vec3& v)
{
	const float length = std::sqrt(physics::lengthSquared(v));
	return length > 1e-6f ? v * (1.0f / length) : Vec3 {0, 1, 0};
}

} // namespace

Graph Graph::parse(const std::string& text)
{
	Graph g;
	std::sscanf(text.c_str(), "%f/%f/%f/%f", &g.a, &g.b, &g.c, &g.d);
	return g;
}

namespace {

// Low, Medium, High (BF2142.exe 0x00a04284); some scripts give a number
// for LodDistance (51 "500"), which the exe's setter (0x00625f60) doesn't
// find among the names: the class stays as it was.
int lowMediumHigh(const world::ObjectTemplate& t, const char* name, int fallback)
{
	const std::string* value = setting(t, name);
	if (!value) {
		return fallback;
	}
	std::string v = *value;
	for (char& c : v) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return v == "low" ? 0 : v == "medium" ? 1 : v == "high" ? 2 : fallback;
}

} // namespace

ParticleTemplate ParticleTemplate::fromTemplate(const world::ObjectTemplate& t)
{
	ParticleTemplate p;
	if (t.typeName() == "SpriteParticleSystem") {
		// The sprite template's constructor (0x10138b40).
		p.emitFrequency = 32;
		p.gravity = -9.82f;
		p.timeToLive = 4;
	} else if (t.typeName() == "MeshParticleSystem" || t.typeName() == "NonScreenAlignedParticleSystem") {
		// The mesh and non-screen-aligned templates' defaults (0x10135f30 /
		// 0x10137290; every shipped system sets both): 32 a second, -9.82.
		p.emitFrequency = 32;
		p.gravity = -9.82f;
	}
	if (const std::string* type = setting(t, "emitterType")) {
		// 0x00625de0: the names compared without case; another name leaves
		// the type as it was.
		std::string v = *type;
		for (char& c : v) {
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		p.emitter = v == "standard" ? Emitter::Standard
			: v == "radialdirection" ? Emitter::RadialDirection
			: v == "radial"          ? Emitter::Radial
			: v == "box"             ? Emitter::Box
									 : p.emitter;
	}
	p.emitRadius = vector(t, "emitRadius", p.emitRadius);
	const char* ranges[3] = {"emitRangeX", "emitRangeY", "emitRangeZ"};
	for (int i = 0; i < 3; ++i) {
		if (const std::string* value = setting(t, ranges[i])) {
			p.emitRange[i] = game::RandomDistribution::parse(*value);
		}
	}
	p.randomRadialDirection = number(t, "randomRadialDirection", p.randomRadialDirection);
	p.radialDirection = number(t, "radialDirection", p.radialDirection);
	p.emitFrequency = number(t, "emitFrequency", p.emitFrequency);
	p.emitTime = number(t, "emitTime", p.emitTime);
	p.emitDelay = number(t, "emitDelay", p.emitDelay);
	p.isLooping = number(t, "isLooping", 0) != 0;
	p.emitSpeed = number(t, "emitSpeed", p.emitSpeed);
	p.randomSpeed = number(t, "randomSpeed", p.randomSpeed);
	p.emitDirection = vector(t, "emitDirection", p.emitDirection);
	p.randomDirectionAngle = vector(t, "randomDirectionAngle", p.randomDirectionAngle);
	p.timeToLive = number(t, "timeToLive", p.timeToLive);
	p.randomTimeToLive = number(t, "randomTimeToLive", p.randomTimeToLive);
	p.gravity = number(t, "gravity", p.gravity);
	p.gravityGraph = graph(t, "gravityGraph", p.gravityGraph);
	p.airResistance = number(t, "airResistance", p.airResistance);
	p.airResistanceGraph = graph(t, "airResistanceGraph", p.airResistanceGraph);
	p.particleMaxSize = number(t, "particleMaxSize", p.particleMaxSize);
	p.randomSize = number(t, "randomSize", p.randomSize);
	p.sizeGraph = graph(t, "sizeGraph", p.sizeGraph);
	p.color1 = vector(t, "color1", p.color1);
	p.color2 = vector(t, "color2", p.color2);
	p.colorBlendGraph = graph(t, "colorBlendGraph", p.colorBlendGraph);
	p.transparencyGraph = graph(t, "transparencyGraph", p.transparencyGraph);
	p.randomRotation = number(t, "randomRotation", p.randomRotation);
	p.rotationSpeed = number(t, "rotationSpeed", p.rotationSpeed);
	p.randomRotationSpeed = number(t, "randomRotationSpeed", p.randomRotationSpeed);
	p.rotationGraph = graph(t, "rotationGraph", p.rotationGraph);
	p.emitSpeedGraph = graph(t, "emitSpeedGraph", p.emitSpeedGraph);
	p.preWarmTime = std::max(0.0f, number(t, "preWarmTime", p.preWarmTime));
	p.windAffectionFactor = number(t, "windAffectionFactor", p.windAffectionFactor);
	p.forceToWaterSurface = number(t, "forceToWaterSurface", 0) != 0;
	p.waterSurfaceOffset = number(t, "waterSurfaceOffset", p.waterSurfaceOffset);
	p.randomIntensity = number(t, "randomIntensity", p.randomIntensity);
	p.lightMapIntensityOffset = number(t, "lightMapIntensityOffset", p.lightMapIntensityOffset);
	p.alphaCull = number(t, "alphaCull", p.alphaCull);
	{
		const Vec3 pivot = vector(t, "pivotOffset", {0, 0, 0});
		p.pivot[0] = pivot.x;
		p.pivot[1] = pivot.y;
	}
	if (const std::string* order = setting(t, "drawOrder")) {
		p.drawBack = order->size() == 4 && std::tolower(static_cast<unsigned char>((*order)[0])) == 'b';
	}
	p.collision = number(t, "collisionEnable", 0) != 0;
	p.collisionAlongNormal = number(t, "collisionSpeedAlongNormal", p.collisionAlongNormal);
	p.collisionAlongPlane = number(t, "collisionSpeedAlongPlane", p.collisionAlongPlane);
	p.lodDistance = lowMediumHigh(t, "LodDistance", p.lodDistance);
	p.qualityLevel = lowMediumHigh(t, "QualityLevel", p.qualityLevel);
	p.emitPrio = lowMediumHigh(t, "emitPrio", p.emitPrio);
	p.heatShimmer = number(t, "particleShaderType", 0) == 1;
	const std::string* blend = setting(t, "particleType");
	if (!blend) {
		blend = setting(t, "blendMode");
	}
	p.additive = blend && *blend == "Additive";
	if (const std::string* texture = setting(t, "textureName")) {
		p.texture = *texture;
		for (char& c : p.texture) {
			if (c == '\\') {
				c = '/';
			}
		}
	}
	p.mesh = t.typeName() == "MeshParticleSystem";
	if (p.mesh) {
		p.geometry = t.geometry();
		p.randomRotation3 = vector(t, "randomRotation", {});
		p.rotationSpeed3 = vector(t, "rotationSpeed", {});
		p.randomRotationSpeed3 = vector(t, "randomRotationSpeed", {});
		p.alignToSpeed = number(t, "alignRotationToSpeed", 0) != 0;
		p.directionalScale = number(t, "directionalScale", 0);
	}
	p.nonScreenAligned = t.typeName() == "NonScreenAlignedParticleSystem";
	p.trail = t.typeName() == "TrailSystem";
	if (p.trail) {
		p.fadeIn = number(t, "fadeInFactor", p.fadeIn);
		p.fadeOut = number(t, "fadeOutFactor", p.fadeOut);
		p.tileFactor = number(t, "textureTileFactor", p.tileFactor);
		p.uvOffsetVelocity = number(t, "textureOffsetVelocity", p.uvOffsetVelocity);
		p.planes = std::clamp(static_cast<int>(number(t, "numPlanes", 1)), 1, 8);
		p.minTrailVelocity = number(t, "minTrailVelocity", p.minTrailVelocity);
	}
	if (number(t, "animationEnable", 0) != 0) {
		p.frameCount = std::max(1, static_cast<int>(number(t, "animationFrameCount", 1)));
		p.framesAcross = std::max(1, static_cast<int>(number(t, "animationFrameCountX", 1)));
		p.frameWidth = number(t, "setAnimationFrameWidthRelative", 1);
		p.frameHeight = number(t, "setAnimationFrameHeightRelative", 1);
		p.animated = t.typeName() == "SpriteParticleSystem";
	}
	p.animationSpeed = number(t, "animationSpeed", p.animationSpeed);
	p.animationPlayOnce = number(t, "animationPlayOnce", 0) != 0;
	p.randomStartFrame = number(t, "animationRandomizedStartFrame", 0) != 0;
	// animationPlayOnce (0x10138790) and the frame count's setter
	// (0x101387f0) make the speed frames / timeToLive (the template's base
	// life, +0x88) once both are known.
	if (p.animationPlayOnce && p.timeToLive > 0) {
		p.animationSpeed = static_cast<float>(p.frameCount) / p.timeToLive;
	}
	p.selfShadowIntensity = number(t, "selfShadowIntensity", p.selfShadowIntensity);
	p.showZoomed = number(t, "showInZoomedMode", 1) != 0;
	p.nearCulling = number(t, "nearCullingEnable", 0) != 0;
	p.noRotationFromParent = number(t, "noRotationFromParent", 0) != 0;
	p.localSystem = number(t, "isLocalSystem", 0) != 0;
	p.staticSystem = number(t, "isStaticSystem", 0) != 0;
	p.showFirstPerson = number(t, "showInFirstPerson", 1) != 0;
	p.showThirdPerson = number(t, "showInThirdPerson", 1) != 0;
	return p;
}

namespace {

// A non-screen-aligned system with forceToWaterSurface (template +0xa4,
// slot 0x124) takes an upright frame (0x1013c800 after composing its world
// matrix +0x74; 0x100989f0 with up (0, 1, 0) and the frame's z row +0x94):
// x = normalize(z x up), y = x x z, z kept as it is (not made level, nor
// normalised); with z within 0.9999 of up the rotation is the identity. So
// its rings lie across the world's up whatever the frame of the hit or the
// swimmer that started them. (x comes out mirrored against the identity:
// z = (0, 0, 1) gives x = (-1, 0, 0), as in the binary.)
Mat3 uprightFrame(const Mat3& frame)
{
	const Vec3 up {0, 1, 0};
	const Vec3 z = frame.rows[2];
	Mat3 out;
	if (std::fabs(physics::dot(up, z)) <= 0.9999f) {
		Vec3 x = physics::cross(z, up);
		const float length = std::sqrt(physics::lengthSquared(x));
		if (length > 0) {
			x = x * (1 / length);
		}
		out.rows = {x, physics::cross(x, z), z};
	}
	return out;
}

Mat3 frameFor(const ParticleTemplate& t, const Mat3& rotation)
{
	return t.nonScreenAligned && t.forceToWaterSurface ? uprightFrame(rotation) : rotation;
}

} // namespace

Emitter::Emitter(const ParticleTemplate& t, const Vec3& position, const Mat3& rotation)
	: m_template(&t), m_previous(position), m_position(position), m_rotation(frameFor(t, rotation))
{
	// The instance's pool (0x1013f6a0): (ttl + random ttl) x frequency + 10.
	const float span = t.timeToLive + t.randomTimeToLive;
	m_capacity = std::min(kMaxParticlesPerEmitter,
		static_cast<size_t>(std::max(0.0f, std::round(span * t.emitFrequency + 10.0f))));
}

bool Emitter::alive() const
{
	const ParticleTemplate& t = *m_template;
	if (!m_particles.empty()) {
		return true;
	}
	if (!m_active) {
		return false; // never started (its quality, or out of view)
	}
	// A trail draws its path as long as its anchor carries it.
	if (t.trail && anchor != 0 && !m_stopped) {
		return true;
	}
	// An attached looping system emits until it is stopped.
	return !m_stopped && (m_age < t.emitDelay + t.emitTime + t.preWarmTime || (anchor != 0 && t.isLooping));
}

void Emitter::place(const Vec3& position, const Mat3& rotation)
{
	m_position = position;
	m_rotation = frameFor(*m_template, rotation);
}

void Emitter::bounds(Vec3& centre, float& radius) const
{
	if (m_particles.empty()) {
		centre = m_position;
		radius = 0;
		return;
	}
	Vec3 lo = worldPosition(m_particles.front()), hi = lo;
	for (const Particle& p : m_particles) {
		const Vec3 at = worldPosition(p);
		lo = {std::min(lo.x, at.x), std::min(lo.y, at.y), std::min(lo.z, at.z)};
		hi = {std::max(hi.x, at.x), std::max(hi.y, at.y), std::max(hi.z, at.z)};
	}
	const float grow = m_template->particleMaxSize; // the template's +0x14c
	const Vec3 half = (hi - lo) * 0.5f + Vec3 {grow, grow, grow};
	centre = (lo + hi) * 0.5f;
	radius = std::sqrt(physics::lengthSquared(half));
}

Vec3 Emitter::worldPosition(const Particle& p) const
{
	return m_template->localSystem ? m_position + m_rotation.transform(p.position) : p.position;
}

namespace {

// RendDX9's random numbers in 0..1 and -1..1 (0x10025420: the mantissa's
// top bits, (r | 0x80) >> 7 + 1, over 2^24 or 2^23 less 1).
float unitRandom(game::Random& random)
{
	return random.unit();
}

float signedRandom(game::Random& random)
{
	return random.unit() * 2 - 1;
}

// An emitRange axis as RendDX9's emitters draw it (0x1013a080 over
// 0x1013a020): CRD_NONE a, CRD_UNIFORM a..b, CRD_EXPONENTIAL -ln(u) a,
// CRD_NORMAL Box-Muller x b + a. A set fourth field (+0xc) flips the
// result's sign with p = 0.5, its random drawn before the value's: so
// "CRD_UNIFORM/0.5/1/1" spawns on both sides of the system.
float emitRangeSample(const game::RandomDistribution& d, game::Random& random)
{
	float sign = 1;
	if (d.c != 0 && random.unit() <= 0.5f) {
		sign = -1;
	}
	if (d.type == game::RandomDistribution::Type::Exponential) {
		return sign * -std::log(random.unit()) * d.a;
	}
	return sign * d.sample(random);
}

// 0x1000f070.
float saturate(float v)
{
	return std::clamp(v, 0.0f, 1.0f);
}

constexpr float kStep = 0.01f;          // the particles' fixed step (DAT_101f7030)
constexpr float kPreWarmStep = 1 / 30.0f; // the pre-warm's (0x101407c0)
constexpr float kMeshPreWarmStep = 1 / 60.0f; // the mesh system's (0x10145bf0)
constexpr float kTwoPi = 6.2831853f;

} // namespace

// The start (0x1013fb30): of a quality the manager draws, within its view
// distance of the camera, the emit time and delay and the pre-warm taken.
void Emitter::start(const EmitContext& context)
{
	const ParticleTemplate& t = *m_template;
	m_started = true;
	m_delay = t.emitDelay;
	m_emitLeft = t.emitTime;
	m_preWarm = t.preWarmTime;
	m_clock = m_emitClock = m_carry = 0;
	m_previous = m_position;
	m_active = true;
	lastSeenFrame = context.frame; // the start stamps it (0x1013fb30)
	if (context.settings && t.qualityLevel > context.settings->qualityLevel) {
		m_active = false;
		return;
	}
	if (context.settings && context.hasCamera
		&& physics::lengthSquared(m_position - context.camera)
			> context.settings->viewDistance * context.settings->viewDistance) {
		m_active = false;
	}
}

// One particle's motion over `steps` steps of `step` seconds (0x1013f0c0).
void Emitter::simulate(Particle& p, float step, int steps, const EmitContext& context) const
{
	const ParticleTemplate& t = *m_template;
	const float f = std::min(p.age / p.life, 1.0f);
	Vec3 wind;
	if (!t.localSystem && t.windAffectionFactor > 0 && context.wind
		&& (!context.settings || context.settings->affectedByWind)) {
		// WindManager slot 0x3c at the particle: the global wind and the
		// local winds reaching it.
		const auto w = context.wind->localWinds.empty() ? context.wind->windAt()
														: context.wind->windAt({p.position.x, p.position.y, p.position.z});
		wind = Vec3 {w[0], w[1], w[2]} * t.windAffectionFactor;
	}
	const float drag = saturate(t.airResistanceGraph.at(f)) * t.airResistance * step;
	const float fall = saturate(t.gravityGraph.at(f)) * t.gravity * step;
	// Gravity pulls down the world's y, in a local system's space too.
	const Vec3 down = t.localSystem ? m_rotation.untransform({0, 1, 0}) : Vec3 {0, 1, 0};
	for (int i = 0; i < steps; ++i) {
		const Vec3 relative = p.velocity - wind;
		const float speed2 = physics::lengthSquared(relative);
		Vec3 dv;
		if (std::fabs(speed2) >= 1.1920929e-07f) {
			dv = relative * (-std::sqrt(speed2) * drag); // -k |r| r
		}
		dv += down * fall;
		p.position += (p.velocity + dv * 0.5f) * step;
		p.velocity += dv;
		p.age += step;
		// collisionEnable: off the ground, the speed across it reversed
		// and kept by collisionSpeedAlongNormal, along it by ...AlongPlane.
		// (The mesh system keeps the system's plane +0x200 every 1/3 s in a
		// ring of 10, 0x10144cf0, and a particle meets the one of its
		// birth, 0x10144110; the rewrite takes the terrain under it.)
		if (t.collision && !t.localSystem && context.groundHeight) {
			const float ground = context.groundHeight(p.position.x, p.position.z);
			if (p.position.y < ground) {
				p.position.y = ground;
				if (p.velocity.y < 0) {
					p.velocity.y = -p.velocity.y * t.collisionAlongNormal;
				}
				p.velocity.x *= t.collisionAlongPlane;
				p.velocity.z *= t.collisionAlongPlane;
			}
		}
	}
	if (std::fabs(p.velocity.x) < 0.001f) {
		p.velocity.x = 0;
	}
	if (std::fabs(p.velocity.y) < 0.001f) {
		p.velocity.y = 0;
	}
	if (std::fabs(p.velocity.z) < 0.001f) {
		p.velocity.z = 0;
	}
	if (t.forceToWaterSurface && context.hasWater && !t.localSystem) {
		p.position.y = context.waterLevel + t.waterSurfaceOffset;
	}
	// Its rotation (0x10131f80): its speed times the rotation graph; the
	// flipbook's clock by the same graph times animationSpeed.
	const float turn = saturate(t.rotationGraph.at(f)) * step * static_cast<float>(steps);
	p.rotation = std::fmod(p.rotation + turn * p.rotationSpeed, kTwoPi);
	if (t.mesh) {
		p.angles += p.spin * turn; // 0x101357c0
	}
	if (t.animated) {
		p.frameClock += turn * t.animationSpeed;
	}
}

std::array<float, 4> spriteFrame(const ParticleTemplate& t, float frameClock)
{
	if (!t.animated || t.frameCount <= 1) {
		return {0, 0, 1, 1};
	}
	// The clock's whole part modulo the frames (0x10131f80: an unsigned
	// floor; a clock below 0 never happens, the speeds being positive).
	const int whole = static_cast<int>(std::floor(std::max(frameClock, 0.0f)));
	const int frame = whole % t.frameCount;
	const float u0 = static_cast<float>(frame % t.framesAcross) * t.frameWidth;
	const float v0 = static_cast<float>(frame / t.framesAcross) * t.frameHeight;
	return {u0, v0, u0 + t.frameWidth, v0 + t.frameHeight};
}

// A particle's birth (0x1013fdc0) at `at` (world), `age` seconds old.
void Emitter::emit(game::Random& random, const Vec3& at, float age, const EmitContext& context)
{
	const ParticleTemplate& t = *m_template;
	if (m_particles.size() >= m_capacity) {
		return;
	}
	const bool sprite = !t.mesh && !t.trail && !t.nonScreenAligned;
	if (sprite && context.poolUsed && context.settings && *context.poolUsed >= context.settings->maxParticles) {
		return; // the sprite pool is empty (0x5efc)
	}
	Particle p;
	// A random start frame takes its random first (0x1013fdc0, +0xd1).
	if (t.animated && t.randomStartFrame && t.frameCount > 0) {
		p.frameClock = std::floor(static_cast<float>(t.frameCount - 1) * unitRandom(random));
	}
	p.selfShadow = t.selfShadowIntensity;
	p.life = std::max(0.01f, t.timeToLive + unitRandom(random) * t.randomTimeToLive);
	p.age = age + startAge; // the bundle's start age (+0xd8 / +0xe0)
	p.alpha = alphaIntensity;
	// Size and speed share one random (0x1013fdc0's local_8).
	const float shared = signedRandom(random);
	p.sizeOffset = shared * t.randomSize;
	p.randomIntensity = signedRandom(random) * t.randomIntensity;
	p.rotation = signedRandom(random) * t.randomRotation;
	const float spin = signedRandom(random) * t.randomRotationSpeed;
	p.rotationSpeed = spin < 0 ? spin - t.rotationSpeed : spin + t.rotationSpeed;
	// Where it is born and its direction, by emitterType (0x10140fa0,
	// 0x101410d0, 0x101412c0).
	Vec3 offset;
	// emitDirection x the bundle's direction scale (0x1013e930).
	Vec3 direction = normalized({t.emitDirection.x * directionScale.x, t.emitDirection.y * directionScale.y,
		t.emitDirection.z * directionScale.z});
	switch (t.emitter) {
	case ParticleTemplate::Emitter::Standard: {
		const float theta = unitRandom(random) * kTwoPi;
		const float phi = unitRandom(random) * (kTwoPi / 2);
		const float sinPhi = std::sin(phi);
		const float uz = unitRandom(random), uy = unitRandom(random), ux = unitRandom(random);
		offset = {std::cos(theta) * ux * t.emitRadius.x * sinPhi, std::sin(theta) * uy * t.emitRadius.y * sinPhi,
			std::cos(phi) * uz * t.emitRadius.z};
		break;
	}
	case ParticleTemplate::Emitter::RadialDirection:
	case ParticleTemplate::Emitter::Radial: {
		const float angle = (signedRandom(random) * t.randomRadialDirection + t.radialDirection) * kDegrees;
		const float c = std::cos(angle), s = std::sin(angle);
		// The ring's axis: emitRadius's longest (the first of equals).
		const float r[3] = {t.emitRadius.x, t.emitRadius.y, t.emitRadius.z};
		int axis = r[0] < r[1] ? 1 : 0;
		if (r[axis] < r[2]) {
			axis = 2;
		}
		const float fz = emitRangeSample(t.emitRange[2], random);
		const float fy = emitRangeSample(t.emitRange[1], random);
		const float fx = emitRangeSample(t.emitRange[0], random);
		Vec3 ring;
		if (axis == 0) {
			offset = {fx, fy * s, fz * c};
			ring = {0, offset.y, offset.z};
		} else if (axis == 1) {
			offset = {fx * s, fy, fz * c};
			ring = {offset.x, 0, offset.z};
		} else {
			offset = {fx * s, fy * c, fz};
			ring = {offset.x, offset.y, 0};
		}
		if (t.emitter == ParticleTemplate::Emitter::RadialDirection) {
			direction = physics::lengthSquared(ring) > 0 ? normalized(ring) : Vec3 {};
		}
		break;
	}
	case ParticleTemplate::Emitter::Box: {
		const float fz = emitRangeSample(t.emitRange[2], random);
		const float fy = emitRangeSample(t.emitRange[1], random);
		const float fx = emitRangeSample(t.emitRange[0], random);
		offset = {fx, fy, fz};
		break;
	}
	}
	// A local system keeps its particles in its own space.
	p.position = t.localSystem ? offset : at + m_rotation.transform(offset);
	if (t.randomDirectionAngle.x != 0 || t.randomDirectionAngle.y != 0 || t.randomDirectionAngle.z != 0) {
		const Vec3 angles {signedRandom(random) * t.randomDirectionAngle.x, signedRandom(random) * t.randomDirectionAngle.y,
			signedRandom(random) * t.randomDirectionAngle.z};
		direction = Mat3::fromYawPitchRoll({angles.y, angles.x, angles.z}).transform(direction);
	}
	// The speed: emitSpeed less the shared random's randomSpeed, times the
	// bundle's speed scale and emitSpeedGraph over the emit time.
	const float spent = t.emitTime > 0 ? 1 - m_emitLeft / t.emitTime : 0;
	const float speed = (t.emitSpeed - shared * t.randomSpeed) * speedScale * saturate(t.emitSpeedGraph.at(spent));
	p.velocity = (t.localSystem ? direction : m_rotation.transform(direction)) * speed;
	if (!t.localSystem) {
		p.velocity += m_velocity; // the system's own (0x1013fdc0: +0xcc x +0xdc)
	}
	if (t.mesh) {
		// One random for the three axes, another for the speed (0x10144d90).
		p.angles = t.randomRotation3 * signedRandom(random);
		const float r = signedRandom(random);
		p.spin = t.randomRotationSpeed3 * r + (r < 0 ? t.rotationSpeed3 * -1.0f : t.rotationSpeed3);
	}
	if (sprite && context.poolUsed) {
		++*context.poolUsed;
	}
	m_particles.push_back(p);
}

// One update of `seconds` (0x101407c0 with 0x101403b0).
void Emitter::step(float seconds, game::Random& random, const EmitContext& context)
{
	const ParticleTemplate& t = *m_template;
	seconds *= timeScale;
	if (seconds <= 0) {
		return;
	}
	m_age += seconds;
	// The time is cut to the longest life.
	const float span = t.timeToLive + t.randomTimeToLive;
	if (span > 0 && seconds > span) {
		seconds = span;
	}
	// The system's velocity, inherited by world particles born now (set
	// from outside with speedFromPhysics).
	if (physicsVelocity) {
		m_velocity = inheritedVelocity;
	} else if (!m_prewarming) {
		m_velocity = (m_position - m_previous) * (1 / seconds);
	}
	// The particles: fixed steps of 0.01 s, the remainder carried.
	const float total = seconds + m_carry;
	const int steps = static_cast<int>(std::lround(total * 100.0f));
	m_carry = total - static_cast<float>(steps) * kStep;
	const float before = m_clock;
	m_clock += static_cast<float>(steps) * kStep;
	const bool sprite = !t.mesh && !t.trail && !t.nonScreenAligned;
	for (Particle& p : m_particles) {
		simulate(p, kStep, steps, context);
	}
	const size_t alive = m_particles.size();
	m_particles.erase(std::remove_if(m_particles.begin(), m_particles.end(),
						  [](const Particle& p) { return p.age >= p.life; }),
		m_particles.end());
	if (sprite && context.poolUsed) {
		*context.poolUsed -= static_cast<int>(alive - m_particles.size());
	}
	// isStaticSystem: once, its life run at once in steps of 0.1 s (0x101407c0;
	// the mesh system's update 0x10145bf0 has no such run).
	if (t.staticSystem && !t.mesh && !m_warmedStatic && m_active && !m_prewarming) {
		m_warmedStatic = true;
		m_prewarming = true;
		for (float left = t.timeToLive; left > 0; left -= 0.1f) {
			step(std::min(left, 0.1f) / std::max(timeScale, 1e-3f), random, context);
		}
		m_prewarming = false;
	}
	// The delay.
	m_delay -= seconds;
	if (m_delay > 0) {
		m_emitClock = m_clock;
		m_previous = m_position;
		return;
	}
	// The pre-warm, once the delay is over: the time run at once in steps of 1/30 s.
	if (m_preWarm > 0 && !m_prewarming) {
		float left = m_preWarm;
		m_preWarm = 0;
		m_prewarming = true;
		const float warmStep = t.mesh ? kMeshPreWarmStep : kPreWarmStep;
		for (; left > 0; left -= warmStep) {
			step(std::min(left, warmStep) / std::max(timeScale, 1e-3f), random, context);
		}
		m_prewarming = false;
	}
	if (m_stopped || !m_active) {
		m_emitClock = m_clock;
		m_previous = m_position;
		return;
	}
	// Unseen for 60 frames (0x101403b0 / 0x10144d90): no emission, and the
	// emit clock (+0xf0) stays behind, so the missed time is emitted when
	// it is seen again.
	if (unseen(context.frame)) {
		m_previous = m_position;
		return;
	}
	// The emission (0x101403b0): the frequency times the distance LOD and
	// the bundle's scale, throttled by priority once the pool is loaded.
	float frequency = t.emitFrequency * m_lodScale * emitScale;
	if (sprite && context.settings && context.poolUsed && (t.isLooping || m_clock > 1.0f)
		&& static_cast<float>(*context.poolUsed)
			> std::round(static_cast<float>(context.settings->maxParticles) * context.settings->poolLoad)) {
		frequency *= context.settings->priorityScales[static_cast<size_t>(std::clamp(t.emitPrio, 0, 2))];
	}
	if (frequency <= 1e-4f) {
		m_emitClock = m_clock;
		m_previous = m_position;
		return;
	}
	const float interval = 1 / frequency;
	while (m_emitClock + interval <= m_clock + 1e-5f) {
		if (t.emitTime > 0 || !t.isLooping) {
			m_emitLeft -= interval;
			if (m_emitLeft <= 0) {
				if (t.isLooping) {
					m_emitLeft = t.emitTime; // the looping system starts over (slot 0xc)
				} else {
					m_stopped = true;
					break;
				}
			}
		}
		m_emitClock += interval;
		// One born so long ago that it would be dead by now (a catch-up
		// after the system was out of sight) isn't made: its update would
		// drop it before it is drawn. (The binary lerps such a birth's
		// place back along the system's last move, unclamped; here it
		// stays at the update's start.)
		if (span > 0 && m_clock - m_emitClock >= span) {
			continue;
		}
		// Its place lerped over the update, then moved on for the rest of it.
		const float share = m_clock > before ? std::clamp((m_emitClock - before) / (m_clock - before), 0.0f, 1.0f) : 1.0f;
		const Vec3 at = m_previous + (m_position - m_previous) * share;
		const size_t count = m_particles.size();
		emit(random, at, 0, context);
		if (m_particles.size() > count) {
			const int rest = static_cast<int>(std::lround((m_clock - m_emitClock) * 100.0f));
			if (rest > 0) {
				simulate(m_particles.back(), kStep, rest, context);
			}
		}
	}
	m_previous = m_position;
}

namespace {

// A trail point's motion (TrailSystem 0x1013a600, from its emission
// 0x1013b4b0 and update 0x1013b840), `steps` steps of 0.01 s: the wind
// (manager +0x8c10: strength slot 0x1c x direction slot 0x24) times
// windAffectionFactor (+0x8c), the drag -airResistance (+0x80) |r| r of the
// speed through it (no graph, unlike the sprites), gravity (+0x7c) along
// the world's y by the gravity graph at the point's age; speeds under
// 0.001 dropped; with forceToWaterSurface (+0x90) held at the water plus
// waterSurfaceOffset (+0x94) plus 0.1.
void moveTrailPoint(const ParticleTemplate& t, Particle& p, int steps, const EmitContext& context)
{
	constexpr float kStep = 0.01f;
	const float f = p.life > 0 ? std::min(p.age / p.life, 1.0f) : 1.0f;
	Vec3 wind;
	if (t.windAffectionFactor != 0 && context.wind && (!context.settings || context.settings->affectedByWind)) {
		const auto w = context.wind->localWinds.empty() ? context.wind->windAt()
														: context.wind->windAt({p.position.x, p.position.y, p.position.z});
		wind = Vec3 {w[0], w[1], w[2]} * t.windAffectionFactor;
	}
	const float drag = t.airResistance * kStep;
	const float fall = saturate(t.gravityGraph.at(f)) * t.gravity * kStep;
	for (int i = 0; i < steps; ++i) {
		const Vec3 relative = p.velocity - wind;
		const float speed2 = physics::lengthSquared(relative);
		Vec3 dv;
		if (std::fabs(speed2) >= 1.1920929e-07f) {
			dv = relative * (-std::sqrt(speed2) * drag);
		}
		dv.y += fall;
		p.position += (p.velocity + dv * 0.5f) * kStep;
		p.velocity += dv;
	}
	if (std::fabs(p.velocity.x) < 0.001f) {
		p.velocity.x = 0;
	}
	if (std::fabs(p.velocity.y) < 0.001f) {
		p.velocity.y = 0;
	}
	if (std::fabs(p.velocity.z) < 0.001f) {
		p.velocity.z = 0;
	}
	if (t.forceToWaterSurface && context.hasWater && !t.localSystem) {
		p.position.y = context.waterLevel + t.waterSurfaceOffset + 0.1f;
	}
}

} // namespace

void Emitter::update(float seconds, game::Random& random, const EmitContext& context)
{
	const ParticleTemplate& t = *m_template;
	if (!m_started) {
		start(context);
	}
	// The distance LOD (0x101393e0): (1 - f)^2 plus the particles' size on
	// screen, between the template's floor and 1.
	m_lodScale = 1;
	if (context.settings && context.hasCamera) {
		const scene::ParticleSystemSettings& s = *context.settings;
		const float end = s.lodFractions[static_cast<size_t>(std::clamp(t.lodDistance, 0, 2))] * s.viewDistance;
		const float fadeStart = s.startFadePercentageOfViewDistance * end;
		const float distance = std::sqrt(physics::lengthSquared(m_position - context.camera));
		float scale = 1;
		if (distance > fadeStart && end > fadeStart) {
			const float f = std::min((distance - fadeStart) / (end - fadeStart), 1.0f);
			scale = (1 - f) * (1 - f);
		}
		const float radius = t.particleMaxSize + t.randomSize;
		scale += radius / std::max(distance * context.tanHalfFov, 1e-3f);
		m_lodScale = std::clamp(scale, std::min(t.minEmitScale, 1.0f), 1.0f);
	}
	if (t.trail) {
		// A point at emitFrequency while the system moves faster than
		// minTrailVelocity, for as long as it is carried.
		m_age += seconds;
		const float active = m_age - t.emitDelay;
		m_speed = m_placed && seconds > 0 ? std::sqrt(physics::lengthSquared(m_position - m_lastPosition)) / seconds : 0.0f;
		// A new point adds the system's +0x1c..+0x24 to its speed
		// (0x1013b4b0); taken here as the velocity speedFromPhysics hands
		// the system, else none (which writer fills it isn't traced).
		m_velocity = physicsVelocity ? inheritedVelocity : Vec3 {};
		m_lastPosition = m_position;
		m_placed = true;
		const bool emitting = t.isLooping || anchor != 0 || active - seconds < t.emitTime;
		if (!m_stopped && m_active && active > 0 && emitting && m_speed >= t.minTrailVelocity) {
			m_pending += t.emitFrequency * emitScale * seconds;
			// Unseen for 60 frames (0x1013b4b0: the draw's frame +0x120):
			// the points wait, and come at once (up to the pool, +0x150)
			// when it is seen again.
			m_pending = std::min(m_pending, static_cast<float>(m_capacity) + 1.0f);
			while (!unseen(context.frame) && m_pending >= 1.0f) {
				emit(random, m_position, 0, context);
				m_pending -= 1.0f;
			}
		}
		// The points move and age in steps of 0.01 s, the rest carried.
		const float total = seconds + m_carry;
		const int steps = static_cast<int>(std::lround(total * 100.0f));
		m_carry = total - static_cast<float>(steps) * 0.01f;
		for (Particle& p : m_particles) {
			moveTrailPoint(t, p, steps, context);
			p.age += static_cast<float>(steps) * 0.01f;
		}
		m_particles.erase(std::remove_if(m_particles.begin(), m_particles.end(),
							  [](const Particle& p) { return p.age >= p.life; }),
			m_particles.end());
		return;
	}
	step(seconds, random, context);
}

EffectManager::EffectManager(const world::ObjectTemplateManager& templates, uint32_t seed)
	: m_templates(templates), m_random(seed)
{
}

EffectManager::~EffectManager() = default;

const ParticleTemplate* EffectManager::particleTemplate(const world::ObjectTemplate& t)
{
	auto& slot = m_particleTemplates[&t];
	if (!slot) {
		slot = std::make_unique<ParticleTemplate>(ParticleTemplate::fromTemplate(t));
	}
	return slot.get();
}

void EffectManager::spawnTemplate(const world::ObjectTemplate& t, const Vec3& position, const Mat3& rotation, int depth,
	View view, std::vector<Emitter*>* spawned, std::vector<size_t>* spawnedLights)
{
	if (depth > 4) {
		return;
	}
	const std::string& type = t.typeName();
	// ParticleSystemEmitter (class 0x94d2, the manager's kind 3: template
	// vtable 0x101d9420, constructor 0x10146820, defaults 0x101464c0;
	// instance vtable 0x101d9268, constructor 0x10142010, update
	// 0x10143540, emission 0x101426f0, motion 0x10141720), whose particles
	// each carry a particleSystemTemplate, isn't made: only levels without
	// an archive.md5 (BF2's leftovers) create one.
	if (type == "SpriteParticleSystem" || type == "MeshParticleSystem" || type == "NonScreenAlignedParticleSystem"
		|| type == "TrailSystem") {
		const ParticleTemplate* particles = particleTemplate(t);
		// First person: showInFirstPerson or showInZoomedMode (0x0061a1c0's
		// flags & 5); third person: showInThirdPerson (& 2).
		if ((view == View::FirstPerson && !particles->showFirstPerson && !particles->showZoomed)
			|| (view == View::ThirdPerson && !particles->showThirdPerson)) {
			return;
		}
		if (!particles->texture.empty() || (particles->mesh && !particles->geometry.empty())) {
			m_emitters.push_back(std::make_unique<Emitter>(*particles, position, rotation));
			if (spawned) {
				spawned->push_back(m_emitters.back().get());
			}
		}
		return;
	}
	if (type == "Sound") {
		if (depth > 0) {
			m_startedSounds.push_back({t.name(), position});
		}
		return;
	}
	if (type == "Emitter") {
		startDebrisEmitter(t, position, rotation, depth, view); // debris (0x0061f360)
		return;
	}
	if (type != "EffectBundle") {
		return; // light sources: placed by their bundle
	}
	// How long the bundle's particle systems last (its lights' life).
	float span = 0;
	bool looping = false;
	for (const world::ChildTemplate& child : t.children()) {
		const world::ObjectTemplate* childTemplate = m_templates.find(child.name);
		if (!childTemplate || childTemplate->typeName() == "LightSource" || childTemplate->typeName() == "Sound"
			|| childTemplate->typeName() == "EffectBundle") {
			continue;
		}
		const std::string& childType = childTemplate->typeName();
		if (childType == "SpriteParticleSystem" || childType == "MeshParticleSystem"
			|| childType == "NonScreenAlignedParticleSystem" || childType == "TrailSystem") {
			const ParticleTemplate* particles = particleTemplate(*childTemplate);
			looping = looping || particles->isLooping;
			span = std::max(span, particles->emitDelay + particles->emitTime + particles->timeToLive + particles->randomTimeToLive);
		}
	}
	// The bundle's randomAgeFactor (the exe's +0x410, sampled once a start,
	// 0x00619f90) scales each of its particle systems' age (their +0x12c,
	// slots 0x7c / 0x78); speedFromPhysics (+0x45c) has the exe set their
	// inherited velocity each tick (0x00618ae0: the root object's physics
	// velocity, none for a bundle on its own).
	float ageFactor = 1;
	if (const std::string* factor = setting(t, "randomAgeFactor")) {
		ageFactor = game::RandomDistribution::parse(*factor).sample(m_random);
	}
	const bool physicsSpeed = number(t, "speedFromPhysics", 0) != 0;
	// A WindAffector component on a bundle spawned on its own (the
	// explosions' Point winds): its instance registers with the WindManager
	// as the object is made (0x0064b490), counts windBlowTime down each
	// tick and leaves at 0 (0x0064b4c0; -1 never), or when the object goes
	// (slot 6, 0x0064b510), which the rewrite takes as the bundle's span.
	// The attached bundles' are ObjectEffects::localWinds.
	if (!spawnedLights && setting(t, "windType")) {
		LiveWind wind;
		wind.wind = ObjectEffects::localWindOf(t, position, rotation);
		const float blowTime = number(t, "windBlowTime", -1);
		wind.life = looping ? -1.0f : std::max(span, 0.1f);
		if (blowTime > 0) {
			wind.life = wind.life < 0 ? blowTime : std::min(wind.life, blowTime);
		}
		m_winds.push_back(wind);
	}
	// Its modifiers at the start (0x0061a1c0 -> 0x00618e30): a bundle on
	// its own has no physics to take speeds from, only its altitude.
	const BundleModifiers& modifiers = bundleModifiers(t);
	std::array<float, 6> modifierValues {};
	unsigned modifierMask = 0;
	if (modifiers.any()) {
		BundleModifiers::Inputs inputs;
		inputs.altitude = altitudeAt(position, modifiers.minUnderWater >= 0);
		modifierMask = modifiers.evaluate(inputs, modifierValues);
	}
	for (const world::ChildTemplate& child : t.children()) {
		if (const world::ObjectTemplate* childTemplate = m_templates.find(child.name)) {
			const Mat3 local = Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z});
			Vec3 at = position + rotation.transform({child.position.x, child.position.y, child.position.z});
			const std::string& childType = childTemplate->typeName();
			const bool system = childType == "SpriteParticleSystem" || childType == "MeshParticleSystem" ||
				childType == "NonScreenAlignedParticleSystem" || childType == "TrailSystem";
			if (system) {
				const ParticleTemplate* particles = particleTemplate(*childTemplate);
				const Mat3 turn = particles->noRotationFromParent ? local : local * rotation;
				if (particles->noRotationFromParent) {
					at = position + Vec3 {child.position.x, child.position.y, child.position.z};
				}
				std::vector<Emitter*> started;
				spawnTemplate(*childTemplate, at, turn, depth + 1, view, &started, spawnedLights);
				for (Emitter* emitter : started) {
					emitter->timeScale *= ageFactor;
					emitter->physicsVelocity = physicsSpeed;
					// 0..4 go to the systems (0x00618bc0..0x00618d00); the
					// sound's (5) is the bundle's.
					float* const targets[5] = {&emitter->alphaIntensity, &emitter->emitScale, &emitter->speedScale,
						&emitter->timeScale, &emitter->startAge};
					for (int m = 0; m < 5; ++m) {
						if ((modifierMask & (1u << m)) != 0) {
							*targets[m] = modifierValues[static_cast<size_t>(m)];
						}
					}
					if (spawned) {
						spawned->push_back(emitter);
					}
				}
				continue;
			}
			if (childTemplate->typeName() == "LightSource") {
				const LightSourceLook look = lightSourceOf(*childTemplate);
				if (!look.enabled || look.directional) {
					continue;
				}
				LiveLight light;
				light.look = look;
				light.color = look.color;
				light.range1 = look.range1;
				light.range2 = look.range2;
				light.localDirection = (local * rotation).transform(look.direction);
				light.direction = light.localDirection;
				light.life = looping ? -1.0f : std::max(span, 0.1f);
				light.position = at;
				light.localPosition = at;
				m_lights.push_back(light);
				if (spawnedLights) {
					spawnedLights->push_back(m_lights.size() - 1);
				}
				continue;
			}
			const size_t sounds = m_startedSounds.size();
			spawnTemplate(*childTemplate, at, local * rotation, depth + 1, view, spawned, spawnedLights);
			if (childType == "Sound" && m_startedSounds.size() > sounds) {
				// The bundle's start sound, its envelopes bound to the
				// bundle's sound intensity (0x00619700).
				StartedSound& sound = m_startedSounds.back();
				sound.bundle = &t;
				sound.intensity = (modifierMask & (1u << 5)) != 0 ? modifierValues[5] : 0.0f;
			}
		}
	}
}

DecalTemplate DecalTemplate::fromTemplate(const world::ObjectTemplate& t)
{
	DecalTemplate d;
	if (const std::string* texture = setting(t, "decalTextureName")) {
		d.texture = *texture;
		for (char& c : d.texture) {
			if (c == '\\') {
				c = '/';
			}
		}
	}
	if (const std::string* normalMap = setting(t, "decalNormalMapName")) {
		d.normalMap = *normalMap;
		for (char& c : d.normalMap) {
			if (c == '\\') {
				c = '/';
			}
		}
	}
	d.isStatic = number(t, "isStatic", 0) != 0;
	d.size = number(t, "size", d.size);
	d.randomSize = number(t, "randomSize", d.randomSize);
	d.color = vector(t, "color", d.color);
	d.rotation = number(t, "rotation", d.rotation);
	d.randomRotation = number(t, "randomRotation", d.randomRotation);
	if (number(t, "animationEnable", 0) != 0) {
		d.frameCount = std::max(1, static_cast<int>(number(t, "animationFrameCount", 1)));
		d.framesAcross = std::max(1, static_cast<int>(number(t, "animationFrameCountX", 1)));
		d.frameWidth = number(t, "setAnimationFrameWidthRelative", 1);
		d.frameHeight = number(t, "setAnimationFrameHeightRelative", 1);
		d.randomStartFrame = number(t, "animationRandomizedStartFrame", 0) != 0;
	}
	return d;
}

std::vector<DecalVertex> clipDecal(const Vec3& centre, const Vec3& normal, const Vec3& tangent, float halfSize,
	const std::array<float, 4>& frame, const std::vector<std::array<Vec3, 3>>& triangles, float maxAngleDegrees,
	float normalOffset)
{
	constexpr size_t kMaxVertices = 128; // 0x7f
	const Vec3 n0 = normalized(normal);
	const Vec3 u = normalized(tangent) * halfSize;
	const Vec3 v = physics::cross(n0, normalized(tangent)) * halfSize;
	const auto dotv = [](const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
	// The square, as the flat path lays it out (LevelRenderer's corners).
	const DecalVertex square[4] = {
		{centre - u - v, frame[0], frame[3]},
		{centre + u - v, frame[2], frame[3]},
		{centre + u + v, frame[2], frame[1]},
		{centre - u + v, frame[0], frame[1]},
	};
	const float cosLimit = maxAngleDegrees > 0 ? std::cos(maxAngleDegrees * 0.0174533f) : -2.0f;
	std::vector<DecalVertex> out;
	for (const auto& tri : triangles) {
		Vec3 a = tri[0], b = tri[1], c = tri[2];
		Vec3 n = physics::cross(b - a, c - a);
		const float length = std::sqrt(physics::lengthSquared(n));
		if (length < 1e-8f) {
			continue;
		}
		n = n * (1.0f / length);
		if (dotv(n, n0) < 0) { // wound the other way: face the decal
			std::swap(b, c);
			n = n * -1.0f;
		}
		const float facing = dotv(n, n0);
		if (facing <= 1e-4f || facing < cosLimit) {
			continue;
		}
		std::vector<DecalVertex> polygon(square, square + 4);
		const Vec3 corners[3] = {a, b, c};
		for (int e = 0; e < 3 && polygon.size() >= 3; ++e) {
			const Vec3& start = corners[e];
			const Vec3 edge = corners[(e + 1) % 3] - start;
			const Vec3 plane = normalized(physics::cross(edge, n)); // outward
			const auto side = [&](const DecalVertex& p) { return dotv(p.position - start, plane); };
			std::vector<DecalVertex> kept;
			for (size_t i = 0; i < polygon.size(); ++i) {
				const DecalVertex& p = polygon[i];
				const DecalVertex& q = polygon[(i + 1) % polygon.size()];
				const float sp = side(p), sq = side(q);
				if (sp <= 0) {
					kept.push_back(p);
				}
				if ((sp <= 0) != (sq <= 0)) {
					const float denominator = sp - sq;
					if (std::fabs(denominator) < 1.19e-7f) {
						continue;
					}
					const float t = sp / denominator;
					kept.push_back({p.position + (q.position - p.position) * t, p.u + (q.u - p.u) * t, p.v + (q.v - p.v) * t});
				}
			}
			polygon = std::move(kept);
		}
		if (polygon.size() < 3) {
			continue;
		}
		for (DecalVertex& p : polygon) {
			p.position = p.position - n * dotv(p.position - a, n) + n0 * normalOffset;
		}
		for (size_t i = 1; i + 1 < polygon.size(); ++i) {
			if (out.size() + 3 > kMaxVertices) {
				return out;
			}
			out.push_back(polygon[0]);
			out.push_back(polygon[i]);
			out.push_back(polygon[i + 1]);
		}
	}
	return out;
}

bool EffectManager::spawnDecal(const std::string& name, const Vec3& position, const Vec3& normal, uint32_t surface)
{
	return addDecal(name, position, normal, surface, 0);
}

bool EffectManager::addDecal(const std::string& name, const Vec3& position, const Vec3& normal, uint32_t surface,
	uint32_t set)
{
	const world::ObjectTemplate* t = m_templates.find(name);
	if (!t || (t->typeName() != "Decal" && t->typeName() != "decal")) {
		return false;
	}
	auto found = m_decalTemplates.find(t);
	if (found == m_decalTemplates.end()) {
		found = m_decalTemplates.emplace(t, DecalTemplate::fromTemplate(*t)).first;
	}
	const DecalTemplate& look = found->second;
	if (look.texture.empty()) {
		return false;
	}
	Decal decal;
	decal.texture = look.texture;
	decal.normalMap = look.normalMap;
	decal.normal = normalized(normal);
	decal.position = position;
	decal.surface = surface;
	decal.size = look.size + m_decalRandom.unit() * look.randomSize;
	decal.color = look.color;
	// Turned about the normal from any perpendicular axis.
	const float angle = (look.rotation + m_decalRandom.unit() * look.randomRotation) * kDegrees;
	const Vec3 u = perpendicular(decal.normal), v = physics::cross(decal.normal, u);
	decal.tangent = u * std::cos(angle) + v * std::sin(angle);
	if (look.frameCount > 1) {
		const int frame = look.randomStartFrame
			? std::min(static_cast<int>(m_decalRandom.unit() * look.frameCount), look.frameCount - 1)
			: 0;
		const float u0 = (frame % look.framesAcross) * look.frameWidth;
		const float v0 = (frame / look.framesAcross) * look.frameHeight;
		decal.frame = {u0, v0, u0 + look.frameWidth, v0 + look.frameHeight};
	}
	m_lastDecalId = 0;
	if (look.isStatic) {
		// The set's second list (0x10166410 -> 0x10164710): the next id,
		// past 255 back to 1.
		m_decals.push_back(decal);
		m_decals.back().staticId = m_lastDecalId = nextStaticDecalId(set);
		return true;
	}
	// The ring of aged decals (+0x28): the oldest goes when it is full.
	if (std::count_if(m_decals.begin(), m_decals.end(), [](const Decal& d) { return d.staticId == 0; })
		>= static_cast<std::ptrdiff_t>(kMaxDecals)) {
		m_decals.erase(std::find_if(m_decals.begin(), m_decals.end(), [](const Decal& d) { return d.staticId == 0; }));
	}
	m_decals.push_back(decal);
	return true;
}

uint8_t EffectManager::nextStaticDecalId(uint32_t object)
{
	uint32_t& id = m_staticDecalIds[object];
	if (id > 0xfe) {
		id = 0;
	}
	return static_cast<uint8_t>(++id);
}

bool EffectManager::removeStaticDecal(uint32_t object, uint8_t id)
{
	if (id == 0) {
		return false; // 0x10164e70 does nothing for 0
	}
	const auto found = std::find_if(m_decals.begin(), m_decals.end(),
		[object, id](const Decal& d) { return d.object == object && d.staticId == id; });
	if (found == m_decals.end()) {
		return false;
	}
	m_decals.erase(found);
	return true;
}

bool EffectManager::spawnDecal(const std::string& name, const Vec3& position, const Vec3& normal, uint32_t object,
	const Vec3& objectPosition, const Vec3& objectRotation)
{
	if (object != 0) {
		const bool unseen = std::any_of(m_decals.begin(), m_decals.end(),
			[object](const Decal& d) { return d.object == object && !d.ownerDrawn; });
		const world::ObjectTemplate* t = unseen ? m_templates.find(name) : nullptr;
		if (t && !DecalTemplate::fromTemplate(*t).isStatic) {
			return false;
		}
	}
	if (!addDecal(name, position, normal, 0, object)) {
		return false;
	}
	if (object != 0) {
		Decal& decal = m_decals.back();
		const Mat3 turn = Mat3::fromYawPitchRoll(objectRotation);
		decal.object = object;
		decal.position = turn.untransform(decal.position - objectPosition);
		decal.normal = turn.untransform(decal.normal);
		decal.tangent = turn.untransform(decal.tangent);
	}
	return true;
}

void EffectManager::dropDecals(uint32_t object)
{
	m_staticDecalIds.erase(object);
	m_decals.erase(std::remove_if(m_decals.begin(), m_decals.end(),
					   [object](const Decal& decal) { return decal.object == object; }),
		m_decals.end());
}

std::vector<PointLight> EffectManager::lights() const
{
	std::vector<PointLight> out;
	for (const LiveLight& light : m_lights) {
		if (!light.look.dynamic) {
			continue; // every light filter asks for isDynamic (RendDX9 0x100a7f00, bit 8)
		}
		const float fade = light.life < 0 ? 1.0f : std::clamp(1.0f - light.age / light.life, 0.0f, 1.0f);
		if (fade > 0) {
			PointLight lit {light.position, light.color * fade, light.range1, light.range2};
			lit.spot = light.look.spot;
			lit.direction = light.direction;
			lit.cone = light.look.cone;
			lit.affectsLightmapped = light.look.affectsLightmapped;
			lit.specColor = light.look.specColor * fade;
			lit.supportsEmitter = light.look.supportsEmitter;
			out.push_back(lit);
		}
	}
	return out;
}

void EffectManager::placeLight(LiveLight& light) const
{
	const auto at = m_anchors.find(light.anchor);
	const Anchor place = at != m_anchors.end() ? at->second : Anchor {};
	const Vec3 mounted = light.mountPosition + light.mountRotation.transform(light.localPosition);
	light.position = place.position + place.rotation.transform(mounted);
	light.direction = place.rotation.transform(light.mountRotation.transform(light.localDirection));
}

LightSourceLook lightSourceOf(const world::ObjectTemplate& t)
{
	LightSourceLook look;
	look.enabled = number(t, "enabled", 1) != 0;
	const std::string* type = setting(t, "lightType");
	const auto is = [type](const char* name) {
		if (!type || type->size() != std::char_traits<char>::length(name)) {
			return false;
		}
		for (size_t i = 0; i < type->size(); ++i) {
			if (std::tolower(static_cast<unsigned char>((*type)[i])) != std::tolower(static_cast<unsigned char>(name[i]))) {
				return false;
			}
		}
		return true;
	};
	look.spot = is("Spot");
	look.directional = is("Directional");
	look.dynamic = number(t, "isDynamic", 0) != 0;
	look.affectsLightmapped = number(t, "affectLightmappedObjects", 1) != 0;
	look.affectingType = static_cast<int>(number(t, "affectingType", 0));
	look.color = vector(t, "color", {1, 1, 1});
	look.specColor = vector(t, "speccolor", {1, 1, 1});
	look.supportsEmitter = number(t, "supportsEmitter", 1) != 0;
	// The light object clamps both ranges to 100 (BF2142.exe 0x0064b170,
	// +0x17c / +0x180; RendDX9 reads the second through slot 0x198).
	look.range1 = std::min(number(t, "attenuationRange1", 0), 100.0f);
	look.range2 = std::max(std::min(number(t, "attenuationRange2", 1), 100.0f), look.range1 + 0.01f);
	look.direction = normalized(vector(t, "direction", {0, 0, 1}));
	// The cone: coneAngle2 (22.5 by default, +0xdc), at most 180 degrees,
	// in radians (Effects.h).
	look.cone = std::min(number(t, "coneAngle2", 22.5f), 180.0f) * kDegrees;
	look.flicker = std::clamp(number(t, "flicker", 0), 0.0f, 1.0f);
	return look;
}

bool EffectManager::spawn(const std::string& name, const Vec3& position, const Mat3& rotation, View view)
{
	const world::ObjectTemplate* t = m_templates.find(name);
	if (!t) {
		return false;
	}
	spawnTemplate(*t, position, rotation, 0, view, nullptr);
	return true;
}

bool EffectManager::spawnAttached(const std::string& name, uint32_t anchor, const Vec3& position, const Mat3& rotation,
	View view, uint32_t tag, bool driven)
{
	const world::ObjectTemplate* t = m_templates.find(name);
	if (!t) {
		return false;
	}
	// Spawned in the bundle's own space, then mounted on the anchor.
	std::vector<Emitter*> spawned;
	std::vector<size_t> spawnedLights;
	const size_t sounds = m_startedSounds.size();
	spawnTemplate(*t, {}, {}, 0, view, &spawned, &spawnedLights);
	for (size_t index : spawnedLights) {
		LiveLight& light = m_lights[index];
		light.anchor = anchor;
		light.tag = tag;
		light.mountPosition = position;
		light.mountRotation = rotation;
		placeLight(light);
	}
	// The looping start sounds of bundles that set their sound intensity
	// (SoundIntensityUsed or a mapping) stay with the bundle (bundleLoops),
	// their envelopes at that intensity: modifier 5 as the start sets it
	// (0x0061a1c0 -> 0x00618e30), with no motion yet.
	std::vector<HeldSound> held;
	for (size_t i = sounds; i < m_startedSounds.size();) {
		const StartedSound& started = m_startedSounds[i];
		const world::ObjectTemplate* sound = m_templates.find(started.name);
		const std::string* loops = sound ? setting(*sound, "loopCount") : nullptr;
		const bool intensity = started.bundle
			&& (bundleModifiers(*started.bundle).modifiers[5].used || bundleModifiers(*started.bundle).modifiers[5].mapped());
		if (!intensity || !loops || std::atoi(loops->c_str()) != 0) {
			++i;
			continue;
		}
		HeldSound h;
		h.key = ++m_nextHeldSound;
		h.anchor = anchor;
		h.tag = tag;
		h.name = m_startedSounds[i].name;
		h.localPosition = m_startedSounds[i].position;
		h.mountPosition = position;
		h.mountRotation = rotation;
		h.intensity = started.intensity;
		held.push_back(std::move(h));
		m_startedSounds.erase(m_startedSounds.begin() + static_cast<std::ptrdiff_t>(i));
	}
	uint32_t bundle = 0;
	if (driven && (!spawned.empty() || !held.empty())
		&& (bundleModifiers(*t).anyMapping || number(*t, "speedFromPhysics", 0) != 0)) {
		AttachedBundle b;
		b.id = bundle = ++m_nextBundle;
		b.anchor = anchor;
		b.tag = tag;
		b.bundle = t;
		b.mountPosition = position;
		b.mountRotation = rotation;
		b.speedFromPhysics = number(*t, "speedFromPhysics", 0) != 0;
		m_attachedBundles.push_back(b);
	}
	for (HeldSound& h : held) {
		h.bundle = bundle;
		m_heldSounds.push_back(std::move(h));
	}
	for (Emitter* emitter : spawned) {
		emitter->anchor = anchor;
		emitter->tag = tag;
		emitter->bundle = bundle;
		emitter->localPosition = emitter->position();
		emitter->localRotation = emitter->rotation();
		emitter->mountPosition = position;
		emitter->mountRotation = rotation;
		placeAttached(*emitter);
	}
	const auto at = m_anchors.find(anchor);
	const Anchor place = at != m_anchors.end() ? at->second : Anchor {};
	for (size_t i = sounds; i < m_startedSounds.size(); ++i) {
		const Vec3 mounted = position + rotation.transform(m_startedSounds[i].position);
		m_startedSounds[i].position = place.position + place.rotation.transform(mounted);
	}
	return true;
}

void EffectManager::placeAttached(Emitter& emitter) const
{
	const auto at = m_anchors.find(emitter.anchor);
	const Anchor place = at != m_anchors.end() ? at->second : Anchor {};
	if (emitter.particleTemplate().noRotationFromParent) {
		// Only the bundle's position (0x1013eff0).
		emitter.place(place.position + place.rotation.transform(emitter.mountPosition) + emitter.localPosition,
			emitter.localRotation);
		return;
	}
	// local, then the mount, then the anchor (row vectors: v * L * M * A).
	const Vec3 mounted = emitter.mountPosition + emitter.mountRotation.transform(emitter.localPosition);
	emitter.place(place.position + place.rotation.transform(mounted),
		emitter.localRotation * emitter.mountRotation * place.rotation);
}

void EffectManager::setPhysicsVelocity(uint32_t anchor, uint32_t tag, const Vec3& velocity)
{
	// 0x00618ae0: the direction times the square root of the speed.
	const float speed = std::sqrt(physics::lengthSquared(velocity));
	const Vec3 inherited = speed > 1e-6f ? velocity * (std::sqrt(speed) / speed) : Vec3 {};
	for (const auto& emitter : m_emitters) {
		if (emitter->anchor == anchor && emitter->tag == tag && emitter->physicsVelocity) {
			emitter->inheritedVelocity = inherited;
		}
	}
}

void EffectManager::setAnchor(uint32_t anchor, const Vec3& position, const Mat3& rotation)
{
	m_anchors[anchor] = {position, rotation};
	for (const auto& emitter : m_emitters) {
		if (emitter->anchor == anchor) {
			placeAttached(*emitter);
		}
	}
	for (LiveLight& light : m_lights) {
		if (light.anchor == anchor) {
			placeLight(light);
		}
	}
}

void EffectManager::setMount(uint32_t anchor, uint32_t tag, const Vec3& position, const Mat3& rotation)
{
	for (const auto& emitter : m_emitters) {
		if (emitter->anchor == anchor && emitter->tag == tag) {
			emitter->mountPosition = position;
			emitter->mountRotation = rotation;
			placeAttached(*emitter);
		}
	}
	for (LiveLight& light : m_lights) {
		if (light.anchor == anchor && light.tag == tag) {
			light.mountPosition = position;
			light.mountRotation = rotation;
			placeLight(light);
		}
	}
	for (HeldSound& h : m_heldSounds) {
		if (h.anchor == anchor && h.tag == tag) {
			h.mountPosition = position;
			h.mountRotation = rotation;
		}
	}
}

void EffectManager::stopAttached(uint32_t anchor, uint32_t tag)
{
	for (const auto& emitter : m_emitters) {
		if (emitter->anchor == anchor && (tag == 0 || emitter->tag == tag)) {
			emitter->stop();
		}
	}
	for (LiveLight& light : m_lights) {
		if (light.anchor == anchor && (tag == 0 || light.tag == tag)) {
			light.stopped = true;
		}
	}
	m_heldSounds.erase(std::remove_if(m_heldSounds.begin(), m_heldSounds.end(),
						   [&](const HeldSound& h) { return h.anchor == anchor && (tag == 0 || h.tag == tag); }),
		m_heldSounds.end());
}

std::set<uint32_t> EffectManager::mountedTags(uint32_t anchor) const
{
	std::set<uint32_t> tags;
	for (const auto& emitter : m_emitters) {
		if (emitter->anchor == anchor) {
			tags.insert(emitter->tag);
		}
	}
	for (const LiveLight& light : m_lights) {
		if (light.anchor == anchor) {
			tags.insert(light.tag);
		}
	}
	for (const HeldSound& h : m_heldSounds) {
		if (h.anchor == anchor) {
			tags.insert(h.tag);
		}
	}
	return tags;
}

bool EffectManager::emitting(uint32_t anchor, uint32_t tag) const
{
	for (const auto& emitter : m_emitters) {
		if (emitter->anchor == anchor && (tag == 0 || emitter->tag == tag) && !emitter->stopped()) {
			return true;
		}
	}
	// A bundle with only a looping sound runs while the sound is held.
	return std::any_of(m_heldSounds.begin(), m_heldSounds.end(),
		[&](const HeldSound& h) { return h.anchor == anchor && (tag == 0 || h.tag == tag); });
}

std::vector<EffectManager::BundleLoop> EffectManager::bundleLoops() const
{
	std::vector<BundleLoop> out;
	out.reserve(m_heldSounds.size());
	for (const HeldSound& h : m_heldSounds) {
		const auto at = m_anchors.find(h.anchor);
		if (at == m_anchors.end()) {
			continue;
		}
		const Vec3 mounted = h.mountPosition + h.mountRotation.transform(h.localPosition);
		out.push_back({h.key, h.name, at->second.position + at->second.rotation.transform(mounted), h.intensity});
	}
	return out;
}

void EffectManager::releaseAnchor(uint32_t anchor)
{
	for (const auto& emitter : m_emitters) {
		if (emitter->anchor == anchor) {
			emitter->stop();
			emitter->anchor = 0;
		}
	}
	for (LiveLight& light : m_lights) {
		if (light.anchor == anchor) {
			light.stopped = true;
		}
	}
	m_heldSounds.erase(std::remove_if(m_heldSounds.begin(), m_heldSounds.end(),
						   [anchor](const HeldSound& h) { return h.anchor == anchor; }),
		m_heldSounds.end());
	m_anchors.erase(anchor);
}

void EffectManager::update(float seconds)
{
	// The decals age and go at the decal manager's timeToLive (0x10164f10).
	const float decalLife = m_settings ? m_settings->decalTimeToLive : scene::ParticleSystemSettings {}.decalTimeToLive;
	// A set whose owner wasn't drawn ages twice (0x10164f10 adds the
	// frame's seconds once more while +0x19 is clear; world sets force it
	// set): Decal::ownerDrawn.
	// The static list (+0x2c) isn't aged.
	for (Decal& decal : m_decals) {
		if (decal.staticId == 0) {
			decal.age += decal.object != 0 && !decal.ownerDrawn ? 2 * seconds : seconds;
		}
	}
	m_decals.erase(std::remove_if(m_decals.begin(), m_decals.end(),
					   [decalLife](const Decal& decal) { return decal.staticId == 0 && decal.age >= decalLife; }),
		m_decals.end());
	// Emitters attached to nothing that is still known end.
	for (const auto& emitter : m_emitters) {
		if (emitter->anchor != 0 && !m_anchors.count(emitter->anchor)) {
			emitter->stop();
		}
	}
	m_heldSounds.erase(std::remove_if(m_heldSounds.begin(), m_heldSounds.end(),
						   [this](const HeldSound& h) { return !m_anchors.count(h.anchor); }),
		m_heldSounds.end());
	stepAttachedBundles(seconds);
	EmitContext context = m_context;
	context.settings = m_settings;
	context.wind = m_wind;
	// The sprite pool's use, counted afresh (systems that ended took theirs).
	m_poolUsed = 0;
	for (const auto& emitter : m_emitters) {
		const ParticleTemplate& t = emitter->particleTemplate();
		if (!t.mesh && !t.trail && !t.nonScreenAligned) {
			m_poolUsed += static_cast<int>(emitter->particles().size());
		}
	}
	context.poolUsed = &m_poolUsed;
	for (const auto& emitter : m_emitters) {
		emitter->update(seconds, m_random, context);
	}
	m_emitters.erase(std::remove_if(m_emitters.begin(), m_emitters.end(),
						 [](const std::unique_ptr<Emitter>& e) { return !e->alive(); }),
		m_emitters.end());
	for (LiveLight& light : m_lights) {
		light.age += seconds;
		if (light.anchor != 0 && !m_anchors.count(light.anchor)) {
			light.stopped = true;
		}
	}
	m_lights.erase(std::remove_if(m_lights.begin(), m_lights.end(),
					   [](const LiveLight& l) { return l.stopped || (l.life >= 0 && l.age >= l.life); }),
		m_lights.end());
	for (LiveWind& wind : m_winds) {
		wind.age += seconds;
	}
	m_winds.erase(std::remove_if(m_winds.begin(), m_winds.end(),
					  [](const LiveWind& w) { return w.life >= 0 && w.age >= w.life; }),
		m_winds.end());
	stepDebris(seconds);
}

// The attached bundles' tick (0x0061a760): the speeds at the bundle from
// the anchor's moves over the update, as ObjectEffects takes them from an
// object's (the anchor's own velocity standing in for the root physics';
// no Engine input: an armour effect sits on the vehicle's root).
void EffectManager::stepAttachedBundles(float seconds)
{
	m_attachedBundles.erase(std::remove_if(m_attachedBundles.begin(), m_attachedBundles.end(),
								[this](const AttachedBundle& b) {
									if (!m_anchors.count(b.anchor)) {
										return true;
									}
									return std::none_of(m_emitters.begin(), m_emitters.end(),
											   [&b](const std::unique_ptr<Emitter>& e) { return e->bundle == b.id && !e->stopped(); })
										&& std::none_of(m_heldSounds.begin(), m_heldSounds.end(),
											[&b](const HeldSound& h) { return h.bundle == b.id; });
								}),
		m_attachedBundles.end());
	if (seconds <= 0) {
		return;
	}
	for (AttachedBundle& b : m_attachedBundles) {
		const Anchor& anchor = m_anchors.at(b.anchor);
		const Vec3 point = anchor.position + anchor.rotation.transform(b.mountPosition);
		if (!b.hasLast) {
			b.lastPoint = point;
			b.lastOrigin = anchor.position;
			b.hasLast = true;
			continue;
		}
		const Vec3 velocity = (anchor.position - b.lastOrigin) * (1 / seconds);
		const Vec3 pointVelocity = (point - b.lastPoint) * (1 / seconds);
		const auto each = [&](const std::function<void(Emitter&)>& f) {
			for (const auto& emitter : m_emitters) {
				if (emitter->bundle == b.id) {
					f(*emitter);
				}
			}
		};
		if (b.speedFromPhysics) {
			const float speed = std::sqrt(physics::lengthSquared(velocity));
			const Vec3 inherited = speed > 1e-6f ? velocity * (std::sqrt(speed) / speed) : Vec3 {};
			each([&](Emitter& e) {
				if (e.physicsVelocity) {
					e.inheritedVelocity = inherited;
				}
			});
		}
		const BundleModifiers& modifiers = bundleModifiers(*b.bundle);
		if (modifiers.anyMapping) {
			BundleModifiers::Inputs inputs;
			inputs.speed = std::sqrt(physics::lengthSquared(pointVelocity));
			inputs.rotationalSpeed = std::sqrt(physics::lengthSquared(pointVelocity - velocity));
			inputs.acceleration = std::sqrt(physics::lengthSquared((pointVelocity - b.lastPointVelocity) * (1 / seconds)));
			inputs.altitude = altitudeAt(point, modifiers.minUnderWater >= 0);
			inputs.speedInDof = physics::dot(pointVelocity, (b.mountRotation * anchor.rotation).transform({0, 0, 1}));
			std::array<float, 6> values {};
			const unsigned mask = modifiers.evaluate(inputs, values);
			each([&](Emitter& e) {
				float* const targets[5] = {&e.alphaIntensity, &e.emitScale, &e.speedScale, &e.timeScale, &e.startAge};
				for (int m = 0; m < 5; ++m) {
					if ((mask & (1u << m)) != 0) {
						*targets[m] = values[static_cast<size_t>(m)];
					}
				}
			});
			// The sound intensity (modifier 5, +0x1d8): its start sound's
			// envelopes' input.
			if ((mask & (1u << 5)) != 0) {
				for (HeldSound& h : m_heldSounds) {
					if (h.bundle == b.id) {
						h.intensity = values[5];
					}
				}
			}
		}
		b.lastPoint = point;
		b.lastOrigin = anchor.position;
		b.lastPointVelocity = pointVelocity;
	}
}

void EffectManager::markViewed(const std::function<bool(const Vec3& centre, float radius)>& inView) const
{
	const uint32_t frame = ++m_context.frame;
	for (const auto& emitter : m_emitters) {
		Vec3 centre;
		float radius = 0;
		emitter->bounds(centre, radius);
		if (!inView || inView(centre, radius)) {
			emitter->lastSeenFrame = frame;
		}
	}
}

void EffectManager::appendLocalWinds(std::vector<scene::WindSettings::LocalWind>& out) const
{
	for (const LiveWind& wind : m_winds) {
		out.push_back(wind.wind);
	}
}

namespace {

game::RandomDistribution distribution(const world::ObjectTemplate& t, const char* name, float fallback)
{
	if (const std::string* value = setting(t, name)) {
		return game::RandomDistribution::parse(*value);
	}
	game::RandomDistribution d;
	d.a = fallback;
	return d;
}

} // namespace

DebrisEmitterTemplate DebrisEmitterTemplate::fromTemplate(const world::ObjectTemplate& t)
{
	DebrisEmitterTemplate d;
	d.delay = distribution(t, "delay", 0);
	d.timeToLive = distribution(t, "timeToLive", 0.1f);
	d.intensity = distribution(t, "intensity", 11);
	const char* relative[3] = {"relativePositionInRight", "relativePositionInUp", "relativePositionInDof"};
	const char* positional[3] = {"positionalSpeedInRight", "positionalSpeedInUp", "positionalSpeedInDof"};
	const char* rotational[3] = {"rotationalSpeedInDof", "rotationalSpeedInUp", "rotationalSpeedInRight"};
	for (int i = 0; i < 3; ++i) {
		d.relativePosition[i] = distribution(t, relative[i], 0);
		d.positionalSpeed[i] = distribution(t, positional[i], 0);
		d.rotationalSpeed[i] = distribution(t, rotational[i], 0);
	}
	d.startRotation = distribution(t, "startRotation", 0);
	d.zRotation = distribution(t, "zRotation", 0);
	d.zRotationSnap = number(t, "zRotationSnap", 1);
	d.addEmitterSpeed = number(t, "addEmitterSpeed", 0) != 0;
	d.emitterSpeedScale = number(t, "emitterSpeedScale", 1);
	d.noPhysics = number(t, "noPhysics", 0) != 0;
	if (const std::string* name = setting(t, "template")) {
		d.objectTemplate = *name;
	}
	for (int i = 0; i < 5; ++i) {
		const std::string key = "alternateTemplate" + std::to_string(i);
		if (const std::string* name = setting(t, key.c_str()); name && !name->empty()) {
			d.alternates.push_back(*name);
		}
	}
	return d;
}

DebrisPieceTemplate DebrisPieceTemplate::fromTemplate(const world::ObjectTemplate& t)
{
	// The Particle template's defaults (0x00622960): timeToLive 10, size 1,
	// Transparency 0, gravityModifier 1, drag 0, sizeModifier 1/1/1.
	DebrisPieceTemplate d;
	d.geometry = t.geometry();
	d.part = static_cast<int>(number(t, "geometryPart", 0));
	if (const auto it = t.settings().find("collisionMesh"); it != t.settings().end()) {
		d.collisionMesh = it->second;
	}
	d.collisionPart = static_cast<int>(number(t, "collisionPart", 0));
	d.material = static_cast<uint32_t>(std::max(0.0f, number(t, "material", 0)));
	d.timeToLive = distribution(t, "timeToLive", 10);
	d.size = distribution(t, "size", 1);
	d.transparency = distribution(t, "transparency", 0);
	d.gravityModifier = distribution(t, "gravityModifier", 1);
	d.drag = distribution(t, "drag", 0);
	d.transparencyGraph = graph(t, "transparencyGraph", d.transparencyGraph);
	d.sizeModifier = vector(t, "sizeModifier", d.sizeModifier);
	return d;
}

float DebrisPiece::alpha() const
{
	const float f = life > 0 ? std::min(age / life, 1.0f) : 0.0f;
	return std::clamp(look->transparencyGraph.at(f), 0.0f, 1.0f) * (1 - transparency);
}

Vec3 DebrisPiece::scale() const
{
	const Vec3& m = look->sizeModifier;
	if (m.x == 0 && m.y == 0 && m.z == 0) {
		return {1, 1, 1};
	}
	return {m.x > 0 ? size * m.x : 1.0f, m.y > 0 ? size * m.y : 1.0f, m.z > 0 ? size * m.z : 1.0f};
}

void EffectManager::startDebrisEmitter(const world::ObjectTemplate& t, const Vec3& position, const Mat3& rotation,
	int depth, View view)
{
	auto& slot = m_debrisEmitterTemplates[&t];
	if (!slot) {
		slot = std::make_unique<DebrisEmitterTemplate>(DebrisEmitterTemplate::fromTemplate(t));
	}
	LiveDebrisEmitter e;
	e.look = slot.get();
	e.position = position;
	e.rotation = rotation;
	e.view = view;
	e.depth = depth;
	e.delay = std::max(0.0f, slot->delay.sample(m_random));
	e.timeToLive = std::max(slot->timeToLive.sample(m_random), 0.01f);
	e.intensity = slot->intensity.sample(m_random);
	// 0x0061ef80: the interval 1 / |intensity|, 100 s for none.
	const float rate = std::fabs(e.intensity);
	e.next = rate < 1.1920929e-07f || !std::isfinite(rate) ? 100.0f : 1.0f / rate;
	m_debrisEmitters.push_back(e);
}

void EffectManager::emitDebris(LiveDebrisEmitter& e, float lead)
{
	const DebrisEmitterTemplate& d = *e.look;
	// The template: one of the alternates or the main one.
	std::string name = d.objectTemplate;
	if (!d.alternates.empty()) {
		const size_t n = d.alternates.size();
		const size_t index = static_cast<size_t>(std::floor(m_random.unit() * static_cast<float>(n + 1)));
		if (index < n) {
			name = d.alternates[index];
		}
	}
	const world::ObjectTemplate* t = m_templates.find(name);
	if (!t) {
		return;
	}
	const Vec3 right = e.rotation.transform({1, 0, 0}), up = e.rotation.transform({0, 1, 0}),
			   dof = e.rotation.transform({0, 0, 1});
	Vec3 velocity = right * d.positionalSpeed[0].sample(m_random) + up * d.positionalSpeed[1].sample(m_random)
		+ dof * d.positionalSpeed[2].sample(m_random);
	const Vec3 at = e.position + right * d.relativePosition[0].sample(m_random)
		+ up * d.relativePosition[1].sample(m_random) + dof * d.relativePosition[2].sample(m_random) + velocity * lead;
	const float snap = d.zRotationSnap != 0 ? d.zRotationSnap : 1.0f;
	const float turn = std::round(d.zRotation.sample(m_random) / snap) * snap + d.startRotation.sample(m_random);
	Mat3 rotation = e.rotation;
	if (std::fabs(turn) > 1.1920929e-07f) {
		rotation = rotation * Mat3::axisAngle(normalized(dof), turn);
	}
	if (t->typeName() != "Particle") {
		spawnTemplate(*t, at, rotation, e.depth + 1, e.view, nullptr);
		return;
	}
	auto& slot = m_debrisPieceTemplates[t];
	if (!slot) {
		slot = std::make_unique<DebrisPieceTemplate>(DebrisPieceTemplate::fromTemplate(*t));
	}
	if (slot->geometry.empty()) {
		return;
	}
	DebrisPiece piece;
	piece.look = slot.get();
	piece.position = at;
	piece.rotation = rotation;
	piece.life = slot->timeToLive.sample(m_random);
	if (piece.life < 0) {
		piece.life = 0.5f; // 0x00621250
	}
	piece.size = slot->size.sample(m_random);
	piece.transparency = slot->transparency.sample(m_random);
	piece.gravity = slot->gravityModifier.sample(m_random);
	piece.drag = slot->drag.sample(m_random);
	if (!d.noPhysics) {
		piece.velocity = velocity;
		piece.spin = {d.rotationalSpeed[0].sample(m_random), d.rotationalSpeed[1].sample(m_random),
			d.rotationalSpeed[2].sample(m_random)};
	} else {
		piece.resting = true;
	}
	resolveShape(*slot);
	piece.body.position = piece.position;
	piece.body.rotation = piece.rotation;
	piece.body.velocity = piece.velocity;
	piece.body.angularVelocity = piece.spin; // the node's slot 0x6c
	piece.body.gravityModifier = piece.gravity;
	piece.body.setBounds(slot->boxSize);
	if (m_debris.size() >= kMaxDebris) {
		m_debris.erase(m_debris.begin());
	}
	m_debris.push_back(piece);
}

void EffectManager::resolveShape(DebrisPieceTemplate& t)
{
	if (t.shapeResolved) {
		return;
	}
	t.shapeResolved = true;
	t.points = {{0, 0, 0}};
	const physics::CollisionMeshFile* mesh =
		!t.collisionMesh.empty() && m_debrisWorld.collisionMesh ? m_debrisWorld.collisionMesh(t.collisionMesh) : nullptr;
	if (!mesh || mesh->geometries.empty()) {
		return;
	}
	const auto& parts = mesh->geometries[0].subGeometries;
	const size_t index = static_cast<size_t>(std::max(t.collisionPart, 0));
	if (index >= parts.size()) {
		return;
	}
	// The vehicle LOD (the mobile mesh physics' own), its box's corners.
	const physics::CollisionLod* lod = physics::collisionLodFor(*mesh, parts[index], 1);
	if (!lod || lod->vertices.empty()) {
		return;
	}
	const Vec3 low {lod->boundsMin[0], lod->boundsMin[1], lod->boundsMin[2]};
	const Vec3 high {lod->boundsMax[0], lod->boundsMax[1], lod->boundsMax[2]};
	t.boxSize = {std::max(high.x - low.x, 0.05f), std::max(high.y - low.y, 0.05f), std::max(high.z - low.z, 0.05f)};
	t.points.clear();
	for (int corner = 0; corner < 8; ++corner) {
		t.points.push_back({corner & 1 ? high.x : low.x, corner & 2 ? high.y : low.y, corner & 4 ? high.z : low.z});
	}
}

void EffectManager::stepPiece(DebrisPiece& p, float seconds)
{
	physics::RigidBody& body = p.body;
	const std::vector<Vec3>& points = p.look->points; // resolveShape gave at least the origin
	// The points where they were (their motion this tick meets the statics).
	Vec3 before[8];
	const size_t tested = std::min<size_t>(points.size(), 8);
	for (size_t i = 0; i < tested; ++i) {
		before[i] = body.position + body.rotation.transform(points[i]);
	}
	if (p.drag > 0) {
		body.velocity = body.velocity * std::max(0.0f, 1.0f - p.drag * seconds);
	}
	body.step(seconds);
	physics::ContactResponse response;
	const DebrisWorld& w = m_debrisWorld;
	const uint32_t own = p.look->material;
	const auto materialOf = [&](uint32_t surface) {
		return w.material ? w.material(own, surface) : physics::ContactMaterial {};
	};
	if (w.ground.height && w.ground.heightAndNormal) {
		const uint32_t surface = w.ground.material ? w.ground.material(body.position.x, body.position.z) : 0;
		physics::collideWithGround(body, response, points, w.ground, materialOf(surface));
	} else if (m_context.groundHeight) {
		physics::Ground flat;
		flat.height = m_context.groundHeight;
		flat.heightAndNormal = [this](float x, float z, Vec3& normal) {
			normal = {0, 1, 0};
			return m_context.groundHeight(x, z);
		};
		physics::collideWithGround(body, response, points, flat, materialOf(0));
	}
	if (w.statics && w.statics->triangles() > 0) {
		for (size_t i = 0; i < tested; ++i) {
			const Vec3 now = body.position + body.rotation.transform(points[i]);
			physics::SphereContact contact;
			if (w.statics->segmentContact(before[i], now, contact)) {
				response.addContact(contact.point - body.position, body.pointVelocity(contact.point), contact.normal,
					contact.depth, materialOf(contact.material));
			}
		}
	}
	response.solve(body);
	p.sleep.sample(body.position, body.angularVelocity, seconds);
	if (p.sleep.asleep()) {
		body.velocity = body.angularVelocity = {};
		p.resting = true;
	}
	p.position = body.position;
	p.rotation = body.rotation;
	p.velocity = body.velocity;
	p.spin = body.angularVelocity;
}

void EffectManager::stepDebris(float seconds)
{
	if (seconds <= 0) {
		return;
	}
	// The emitters (0x0061f360): the delay, then a piece each interval
	// over the emitter's life.
	for (LiveDebrisEmitter& e : m_debrisEmitters) {
		float dt = seconds;
		if (e.delay > 0) {
			e.delay -= dt;
			if (e.delay >= 0) {
				continue;
			}
			dt = -e.delay;
			e.delay = 0;
		}
		const float start = e.elapsed;
		e.elapsed += dt;
		const float rate = std::fabs(e.intensity);
		const float interval = rate < 1.1920929e-07f || !std::isfinite(rate) ? 100.0f : 1.0f / rate;
		int guard = 0;
		while (e.next <= e.elapsed && e.next <= e.timeToLive && guard++ < 256) {
			emitDebris(e, e.elapsed - std::max(e.next, start));
			e.next += interval;
		}
	}
	m_debrisEmitters.erase(std::remove_if(m_debrisEmitters.begin(), m_debrisEmitters.end(),
							   [](const LiveDebrisEmitter& e) { return e.delay <= 0 && e.elapsed >= e.timeToLive; }),
		m_debrisEmitters.end());
	// The pieces.
	for (DebrisPiece& p : m_debris) {
		p.age += seconds;
		if (!p.resting) {
			stepPiece(p, seconds);
		}
	}
	m_debris.erase(std::remove_if(m_debris.begin(), m_debris.end(), [](const DebrisPiece& p) { return p.age >= p.life; }),
		m_debris.end());
}

// A mesh particle at its age (MeshParticleMesh.fx vsDiffuse): the colour
// and transparency graphs as the sprites', the size graph scaling the mesh.
void EffectManager::collectMeshes(const Emitter& emitter, std::vector<MeshSprite>& out)
{
	const ParticleTemplate& t = emitter.particleTemplate();
	for (const Particle& p : emitter.particles()) {
		const float f = std::min(p.age / p.life, 1.0f);
		const float blend = std::min(t.colorBlendGraph.at(f), 1.0f);
		const float alpha = std::clamp(std::min(t.transparencyGraph.at(f), 1.0f), 0.0f, 1.0f);
		const float size = std::min(t.sizeGraph.at(f), 1.0f) * t.particleMaxSize + p.sizeOffset;
		if (size <= 0 || alpha <= 0) {
			continue;
		}
		const Vec3 color = t.color1 * (1 - blend) + t.color2 * blend;
		Mat3 turn = Mat3::fromYawPitchRoll({p.angles.x, p.angles.y, p.angles.z});
		const Mat3 system = t.localSystem ? emitter.rotation() : Mat3 {};
		float stretch = 1;
		if (t.alignToSpeed || t.directionalScale != 0) {
			// 0x101357c0: z along the velocity, y as near the world's up as
			// it goes (straight up or down: x the world's, y its -z / z);
			// the particle's own turn is dropped, and none at all at rest.
			const Vec3 v = t.localSystem ? emitter.rotation().transform(p.velocity) : p.velocity;
			const float speed = std::sqrt(physics::lengthSquared(v));
			turn = Mat3 {};
			if (speed >= 1.1920929e-07f) {
				const Vec3 z = v * (1 / speed);
				Vec3 x {1, 0, 0};
				if (std::fabs(z.x) >= 0.0001f || std::fabs(z.z) >= 0.0001f) {
					x = normalized(physics::cross({0, 1, 0}, z));
				}
				turn.rows = {x, physics::cross(z, x), z};
				if (t.directionalScale > 0) {
					stretch = speed * t.directionalScale;
				}
			}
		} else {
			turn = turn * (t.localSystem ? system : emitter.rotation());
		}
		out.push_back({emitter.worldPosition(p), turn, size, {color.x, color.y, color.z, alpha}, stretch});
	}
}

float ParticleView::fade(float depth, int lod) const
{
	if (!settings) {
		return 1;
	}
	// 0x10132e90: past startLod of the class's distance, (1 - min((d - s) / (end - s), 1))^2.
	const float end = settings->lodFractions[static_cast<size_t>(std::clamp(lod, 0, 2))] * settings->viewDistance;
	const float start = settings->startLodPercentageOfLodDistance * end;
	if (depth <= start || end <= start) {
		return depth <= end || end <= start ? 1.0f : 0.0f;
	}
	const float f = std::min((depth - start) / (end - start), 1.0f);
	return (1 - f) * (1 - f);
}

void EffectManager::setDirectionScale(uint32_t anchor, uint32_t tag, const Vec3& scale)
{
	for (const auto& emitter : m_emitters) {
		if (emitter->anchor == anchor && emitter->tag == tag) {
			emitter->directionScale = scale;
		}
	}
}

void EffectManager::setModifier(uint32_t anchor, uint32_t tag, int modifier, float value)
{
	for (const auto& emitter : m_emitters) {
		if (emitter->anchor != anchor || emitter->tag != tag) {
			continue;
		}
		switch (modifier) {
		case 0: emitter->alphaIntensity = value; break;
		case 1: emitter->emitScale = value; break;
		case 2: emitter->speedScale = value; break;
		case 3: emitter->timeScale = value; break;
		case 4: emitter->startAge = value; break; // 0x00618d00 -> slot 0x80
		default: break;
		}
	}
	if (modifier == 5) { // the sound intensity, +0x1d8 (bundleLoops)
		for (HeldSound& h : m_heldSounds) {
			if (h.anchor == anchor && h.tag == tag) {
				h.intensity = value;
			}
		}
	}
}

const BundleModifiers& EffectManager::bundleModifiers(const world::ObjectTemplate& bundle)
{
	auto known = m_bundleModifiers.find(&bundle);
	if (known == m_bundleModifiers.end()) {
		known = m_bundleModifiers.emplace(&bundle, BundleModifiers::fromTemplate(bundle)).first;
	}
	return known->second;
}

float EffectManager::waterDepthAt(const Vec3& at) const
{
	return m_context.hasWater ? m_context.waterLevel - at.y : -1.0f;
}

float EffectManager::altitudeAt(const Vec3& at, bool underWater) const
{
	if (underWater) {
		return m_context.hasWater ? std::max(m_context.waterLevel - at.y, 0.0f) : 0.0f;
	}
	return m_context.groundHeight ? at.y - m_context.groundHeight(at.x, at.z) : at.y;
}

void EffectManager::setModifiers(uint32_t anchor, uint32_t tag, float alphaIntensity, float emitScale, float speedScale,
	float timeScale)
{
	for (const auto& emitter : m_emitters) {
		if (emitter->anchor == anchor && emitter->tag == tag) {
			emitter->alphaIntensity = alphaIntensity;
			emitter->emitScale = emitScale;
			emitter->speedScale = speedScale;
			emitter->timeScale = timeScale;
		}
	}
}

// The look of a particle at its age (Particles.fx vsParticle): the colour
// blend times its intensity plus its random intensity, the transparency
// graph times its alpha, faded by its depth in the view per LOD class.
void EffectManager::collect(const Emitter& emitter, std::vector<Sprite>& out, const ParticleView& view)
{
	const ParticleTemplate& t = emitter.particleTemplate();
	const auto& particles = emitter.particles();
	// Back to front by the squared distance to the camera (0x10130bd0's
	// +0x14, sorted by 0x1012f800).
	std::vector<size_t> order(particles.size());
	std::vector<float> distances(particles.size());
	for (size_t i = 0; i < particles.size(); ++i) {
		order[i] = i;
		distances[i] = physics::lengthSquared(emitter.worldPosition(particles[i]) - view.eye);
	}
	std::stable_sort(order.begin(), order.end(), [&distances](size_t a, size_t b) { return distances[a] > distances[b]; });
	for (const size_t index : order) {
		const Particle& p = particles[index];
		const float f = std::min(p.age / p.life, 1.0f);
		const float blend = std::min(t.colorBlendGraph.at(f), 1.0f);
		float alpha = std::clamp(std::min(t.transparencyGraph.at(f), 1.0f) * p.alpha, 0.0f, 1.0f);
		const float size = std::min(t.sizeGraph.at(f), 1.0f) * t.particleMaxSize + p.sizeOffset;
		if (size <= 0 || alpha <= 0 || alpha < t.alphaCull) {
			continue;
		}
		const Vec3 at = emitter.worldPosition(p);
		if (view.settings) {
			const float depth = physics::dot(at - view.eye, view.forward);
			const float near = t.nearCulling ? ParticleView::nearFade(depth, view.nearPlane, size) : -1.0f;
			if (near >= 0) {
				if (near < 0.01f) {
					continue;
				}
				alpha *= near;
			} else {
				alpha *= view.fade(depth, t.lodDistance);
			}
			if (alpha <= 0) {
				continue;
			}
		}
		const Vec3 color = (t.color1 * (1 - blend) + t.color2 * blend) * p.intensity
			+ Vec3 {p.randomIntensity, p.randomIntensity, p.randomIntensity};
		out.push_back({at, size, p.rotation, {color.x, color.y, color.z, alpha}, t.lightMapIntensityOffset,
			{t.pivot[0], t.pivot[1]}, spriteFrame(t, p.frameClock)});
	}
}

namespace {

// The texture rectangle of a particle's frame (frames over its life).
std::array<float, 4> frameOf(const ParticleTemplate& t, float lifeFraction)
{
	if (t.frameCount <= 1) {
		return {0, 0, 1, 1};
	}
	const int frame = std::min(static_cast<int>(lifeFraction * static_cast<float>(t.frameCount)), t.frameCount - 1);
	const float u0 = static_cast<float>(frame % t.framesAcross) * t.frameWidth;
	const float v0 = static_cast<float>(frame / t.framesAcross) * t.frameHeight;
	return {u0, v0, u0 + t.frameWidth, v0 + t.frameHeight};
}

} // namespace

// NonScreenAlignedParticles.fx: each particle a square of its size about
// its centre in the plane across the system's y axis (its x and z axes;
// the ripples and rings on the ground and water), turned by its rotation.
// RendDX9's draw (0x1012e920, "NSAParticleSystem") fills four vertices a
// particle (0x1012d720: its position and the corner offsets it keeps,
// the age and the template's slot) and batches up to 10 templates' graphs
// as shader constants (0x1012d660, 0x1012d5b0); the colour's / 2 there and
// the pixel shader's x 2 cancel. Here the graphs are read on the CPU.
void EffectManager::collectAligned(const Emitter& emitter, std::vector<WorldVertex>& out)
{
	const ParticleTemplate& t = emitter.particleTemplate();
	const Mat3& turn = emitter.rotation();
	const Vec3 axisU = turn.transform({1, 0, 0}), axisV = turn.transform({0, 0, 1});
	for (const Particle& p : emitter.particles()) {
		const float f = std::min(p.age / p.life, 1.0f);
		const float blend = std::min(t.colorBlendGraph.at(f), 1.0f);
		const float alpha = std::clamp(std::min(t.transparencyGraph.at(f), 1.0f), 0.0f, 1.0f);
		const float size = std::min(t.sizeGraph.at(f), 1.0f) * t.particleMaxSize + p.sizeOffset;
		if (size <= 0 || alpha <= 0) {
			continue;
		}
		const Vec3 color = (t.color1 * (1 - blend) + t.color2 * blend) * p.intensity
			+ Vec3 {p.randomIntensity, p.randomIntensity, p.randomIntensity};
		const std::array<float, 4> rgba {color.x, color.y, color.z, alpha * p.alpha};
		const float c = std::cos(p.rotation), s = std::sin(p.rotation);
		const Vec3 u = (axisU * c + axisV * s) * size, v = (axisV * c - axisU * s) * size;
		const Vec3 centre = emitter.worldPosition(p);
		const std::array<float, 4> frame = frameOf(t, f);
		const float offset = t.lightMapIntensityOffset;
		const WorldVertex corners[4] = {
			{centre - u - v, frame[0], frame[3], rgba, offset},
			{centre + u - v, frame[2], frame[3], rgba, offset},
			{centre + u + v, frame[2], frame[1], rgba, offset},
			{centre - u + v, frame[0], frame[1], rgba, offset},
		};
		for (int i : {0, 1, 2, 0, 2, 3}) {
			out.push_back(corners[i]);
		}
	}
}

// Trail.fx: the points in the order they were left, joined into strips of
// width 2 x size in numPlanes planes about the path; each vertex faded by
// the square of its plane's normal against the eye and by the ends'
// fade-in and fade-out over the point's life.
void EffectManager::collectTrail(const Emitter& emitter, const Vec3& eye, std::vector<WorldVertex>& out)
{
	const ParticleTemplate& t = emitter.particleTemplate();
	const std::vector<Particle>& points = emitter.particles();
	if (points.size() < 2) {
		return;
	}
	struct Point {
		Vec3 position;
		float size;
		std::array<float, 4> color;
		float u;
		float age;
	};
	std::vector<Point> placed;
	placed.reserve(points.size());
	float along = 0;
	for (size_t i = 0; i < points.size(); ++i) {
		const Particle& p = points[i];
		const float f = std::min(p.age / p.life, 1.0f);
		const float blend = std::min(t.colorBlendGraph.at(f), 1.0f);
		const float fade = std::clamp(f / std::max(t.fadeIn, 1e-3f), 0.0f, 1.0f)
			* std::clamp((1 - f) / std::max(t.fadeOut, 1e-3f), 0.0f, 1.0f);
		const float alpha = std::clamp(std::min(t.transparencyGraph.at(f), 1.0f), 0.0f, 1.0f) * fade;
		const Vec3 color = t.color1 * (1 - blend) + t.color2 * blend;
		const Vec3 at = emitter.worldPosition(p);
		if (i > 0) {
			along += std::sqrt(physics::lengthSquared(at - placed.back().position));
		}
		placed.push_back({at, std::max(0.0f, std::min(t.sizeGraph.at(f), 1.0f) * t.particleMaxSize),
			{color.x, color.y, color.z, alpha}, along, p.age});
	}
	if (along <= 1e-3f) {
		return;
	}
	const std::array<float, 4> frame = frameOf(t, 0);
	const float slide = (placed.back().age) * t.uvOffsetVelocity;
	for (size_t i = 0; i + 1 < placed.size(); ++i) {
		const Point& a = placed[i];
		const Point& b = placed[i + 1];
		const Vec3 tangent = normalized(b.position - a.position);
		const Vec3 base = perpendicular(tangent);
		for (int plane = 0; plane < t.planes; ++plane) {
			const float angle = 3.14159265f * static_cast<float>(plane) / static_cast<float>(t.planes);
			const Vec3 side = base * std::cos(angle) + physics::cross(tangent, base) * std::sin(angle);
			const Vec3 normal = physics::cross(tangent, side * -1.0f);
			const auto vertex = [&](const Point& p, float across) {
				Vec3 toEye = eye - p.position;
				toEye = toEye - tangent * physics::dot(toEye, tangent);
				const float facing = physics::dot(normalized(toEye), normal);
				WorldVertex v;
				v.position = p.position + side * (p.size * across);
				v.u = (p.u / along) * t.tileFactor + slide;
				v.v = frame[1] + (frame[3] - frame[1]) * (across * 0.5f + 0.5f);
				v.color = p.color;
				v.color[3] *= facing * facing;
				v.lightMapOffset = t.lightMapIntensityOffset;
				return v;
			};
			const WorldVertex a0 = vertex(a, -1), a1 = vertex(a, 1), b0 = vertex(b, -1), b1 = vertex(b, 1);
			for (const WorldVertex* v : {&a0, &b0, &b1, &a0, &b1, &a1}) {
				out.push_back(*v);
			}
		}
	}
}

} // namespace dice::hfe::effects

namespace dice::hfe::effects {

namespace {

uint32_t flags(const world::ObjectTemplate& t, const char* name, const char* other)
{
	const std::string* value = setting(t, name);
	if (!value) {
		value = setting(t, other);
	}
	return value ? static_cast<uint32_t>(std::strtoul(value->c_str(), nullptr, 10)) : 0u;
}

// Moves `at`/`turn` from a parent's space to its child's, after the pose
// the game gives the part (as the renderer's collectParts).
void placeChild(const world::ChildTemplate& child, uint32_t object, const ObjectEffects::PartPose& partPose, Vec3& at,
	Mat3& turn)
{
	Mat3 childTurn = Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z});
	Vec3 childAt = at + turn.transform({child.position.x, child.position.y, child.position.z});
	Vec3 offset, rotation;
	if (partPose && object != 0 && partPose(object, child.name, offset, rotation)) {
		childAt = childAt + (childTurn * turn).transform(offset);
		childTurn = Mat3::fromYawPitchRoll(rotation) * childTurn;
	}
	at = childAt;
	turn = childTurn * turn;
}

bool hasWindAffector(const world::ObjectTemplate& t)
{
	for (const std::string& component : t.components()) {
		if (component.size() == 12 && std::equal(component.begin(), component.end(), "WindAffector", [](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			})) {
			return true;
		}
	}
	return false;
}

bool holdsBundles(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& t,
	ObjectEffects::BundleCache& cache, int depth = 0)
{
	if (const auto found = cache.find(&t); found != cache.end()) {
		return found->second;
	}
	bool holds = false;
	if (t.typeName() == "EffectBundle" || t.typeName() == "WheelEffectBundle" || t.typeName() == "AirDraftEffectBundle"
		|| t.typeName() == "GroundEffectBundle" || hasWindAffector(t)) {
		holds = true;
	} else if (depth <= 10) {
		for (const world::ChildTemplate& child : t.children()) {
			const world::ObjectTemplate* c = templates.find(child.name);
			if (c && holdsBundles(templates, *c, cache, depth + 1)) {
				holds = true;
				break;
			}
		}
	}
	cache[&t] = holds;
	return holds;
}

bool equalsIgnoreCase(const std::string& a, const std::string& b)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
		return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
	});
}

// A GroundEffectBundle (class 0x9486) in a template tree.
ObjectEffects::Placed groundTriggerOf(const world::ObjectTemplate& t, const std::vector<std::string>& path,
	const std::vector<const world::ChildTemplate*>& chain, const Vec3& position, const Mat3& rotation, int parent)
{
	ObjectEffects::Placed placed;
	placed.kind = ObjectEffects::Placed::Kind::GroundTrigger;
	placed.bundle = &t;
	placed.path = path;
	placed.chain = chain;
	placed.position = position;
	placed.rotation = rotation;
	placed.parent = parent;
	placed.material = static_cast<uint32_t>(number(t, "material", 0));
	placed.maxTriggerDistance = number(t, "maxTriggerDistance", 3); // +0x23c, 0x0061e0b0
	return placed;
}

} // namespace

std::vector<ObjectEffects::Placed> ObjectEffects::bundlesOf(const world::ObjectTemplateManager& templates,
	const world::ObjectTemplate& root, uint32_t object, const PartPose& partPose, BundleCache* cache)
{
	std::vector<Placed> out;
	std::vector<std::string> path;
	std::vector<const world::ChildTemplate*> chain;
	const std::function<void(const world::ObjectTemplate&, const Vec3&, const Mat3&, int)> walk =
		[&](const world::ObjectTemplate& t, const Vec3& at, const Mat3& turn, int depth) {
			if (depth > 10) {
				return;
			}
			for (const world::ChildTemplate& child : t.children()) {
				const world::ObjectTemplate* c = templates.find(child.name);
				if (!c || (cache && !holdsBundles(templates, *c, *cache))) {
					continue;
				}
				Vec3 childAt = at;
				Mat3 childTurn = turn;
				placeChild(child, object, partPose, childAt, childTurn);
				path.push_back(child.name);
				chain.push_back(&child);
				const std::string& type = c->typeName();
				if (type == "GroundEffectBundle") {
					out.push_back(groundTriggerOf(*c, path, chain, childAt, childTurn, -1));
				} else if (type == "EffectBundle" || type == "WheelEffectBundle" || type == "AirDraftEffectBundle") {
					Placed placed;
					placed.kind = type == "WheelEffectBundle" ? Placed::Kind::Wheel
						: type == "AirDraftEffectBundle"      ? Placed::Kind::AirDraft
															  : Placed::Kind::Plain;
					placed.bundle = c;
					placed.path = path;
					placed.chain = chain;
					placed.position = childAt;
					placed.rotation = childTurn;
					placed.onEngine = t.typeName() == "Engine";
					placed.startOnEffects = flags(*c, "startOnEffects", "setStartOnEffects");
					placed.stopOnEffects = flags(*c, "stopOnEffects", "setStopOnEffects");
					placed.startAtCreation = number(*c, "startAtCreation", 0) != 0;
					placed.speedFromPhysics = number(*c, "speedFromPhysics", 0) != 0;
					out.push_back(std::move(placed));
					// The GroundEffectBundles under it: the bundle's start
					// sends its children event 0 (0x0061a1c0, slot 0x100).
					const int parent = static_cast<int>(out.size() - 1);
					for (const world::ChildTemplate& grand : c->children()) {
						const world::ObjectTemplate* g = templates.find(grand.name);
						if (!g || g->typeName() != "GroundEffectBundle") {
							continue;
						}
						Vec3 groundAt = childAt;
						Mat3 groundTurn = childTurn;
						placeChild(grand, object, partPose, groundAt, groundTurn);
						path.push_back(grand.name);
						chain.push_back(&grand);
						out.push_back(groundTriggerOf(*g, path, chain, groundAt, groundTurn, parent));
						path.pop_back();
						chain.pop_back();
					}
				} else {
					if (hasWindAffector(*c)) {
						Placed placed;
						placed.kind = Placed::Kind::Wind;
						placed.bundle = c;
						placed.path = path;
						placed.chain = chain;
						placed.position = childAt;
						placed.rotation = childTurn;
						out.push_back(std::move(placed));
					}
					walk(*c, childAt, childTurn, depth + 1);
				}
				path.pop_back();
				chain.pop_back();
			}
		};
	walk(root, {}, {}, 0);
	return out;
}

scene::WindSettings::LocalWind ObjectEffects::localWindOf(const world::ObjectTemplate& t, const Vec3& position,
	const Mat3& rotation)
{
	using Wind = scene::WindSettings::LocalWind;
	Wind wind;
	const std::string* type = setting(t, "windType");
	std::string name = type ? *type : std::string();
	for (char& c : name) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	wind.type = name == "point" ? Wind::Type::Point
		: name == "radialdir"   ? Wind::Type::RadialDir
		: name == "turbulencefield" ? Wind::Type::Turbulence
									: Wind::Type::Directional;
	const Vec3 d = normalized(rotation.transform(vector(t, "windDirection", {0, 0, 1})));
	wind.position = {position.x, position.y, position.z};
	wind.direction = {d.x, d.y, d.z};
	wind.speed = number(t, "windSpeed", 0);
	wind.falloff = number(t, "windFalloff", 0);
	wind.radialFalloff = number(t, "windRadialFalloff", 0);
	return wind;
}

void ObjectEffects::place(const Placed& bundle, uint32_t object, const PartPose& partPose, Vec3& position,
	Mat3& rotation)
{
	position = {};
	rotation = {};
	for (const world::ChildTemplate* child : bundle.chain) {
		placeChild(*child, object, partPose, position, rotation);
	}
}

void ObjectEffects::startBundle(const ObjectState& object, Tracked& tracked, size_t index, EffectManager& effects,
	const PartPose& partPose, EffectManager::View view, bool created)
{
	Placed& b = tracked.bundles[index];
	if (partPose && !created) {
		place(b, object.id, partPose, b.position, b.rotation);
	}
	effects.spawnAttached(b.bundle->name(), object.id, b.position, b.rotation, view, static_cast<uint32_t>(index + 1), false);
	// The start sends its children event 0 (0x0061a1c0).
	for (Placed& ground : tracked.bundles) {
		if (ground.kind == Placed::Kind::GroundTrigger && ground.parent == static_cast<int>(index)) {
			groundTrigger(object, ground, effects, partPose, created);
		}
	}
}

// GroundEffectBundle's event 0 (0x0061e090 -> 0x0061de40): a ray from the
// bundle along its own -y, maxTriggerDistance long, against the objects
// (the collision manager, slot 0x68) and then the terrain (slot 0x8c); the
// hit's material, or the water's (1) where the hit lies more than 0.1 m
// under the water's surface. Within maxTriggerDistance the MaterialManager's
// cell for the bundle's material and that one (slot 0x68) gives the effect,
// started at the hit (the effect manager's slot 0x118) facing the hit's
// normal and the bundle's forward (its matrix's row 2). The tanks' main
// guns blow dust off the ground under the barrel (geb_tank, material 66,
// 3 m), the walkers' feet stamp it (geb_mech under the foot step bundles,
// 127). The rewrite's ground probe gives no normal: the effect stands up.
void ObjectEffects::groundTrigger(const ObjectState& object, Placed& g, EffectManager& effects, const PartPose& partPose,
	bool created)
{
	if (!m_probe || !m_materialEffect) {
		return;
	}
	if (partPose && !created) {
		place(g, object.id, partPose, g.position, g.rotation);
	}
	groundTriggerAt(object.position + object.rotation.transform(g.position), g.rotation * object.rotation, g, object.id,
		effects);
}

void ObjectEffects::groundTriggerAt(const Vec3& at, const Mat3& turn, const Placed& g, uint32_t ignore, EffectManager& effects)
{
	if (!m_probe || !m_materialEffect) {
		return;
	}
	Vec3 hit;
	uint32_t material = 0;
	if (!m_probe(at, at + turn.transform({0, -1, 0}) * g.maxTriggerDistance, ignore, hit, material)
		|| std::sqrt(physics::lengthSquared(hit - at)) > g.maxTriggerDistance) {
		return;
	}
	const std::string effect = m_materialEffect(g.material, material);
	if (effect.empty()) {
		return;
	}
	const Vec3 up {0, 1, 0};
	Vec3 forward = turn.transform({0, 0, 1});
	forward.y = 0;
	forward = physics::lengthSquared(forward) > 1e-6f ? normalized(forward) : Vec3 {0, 0, 1};
	Mat3 frame;
	frame.rows = {physics::cross(up, forward), up, forward};
	effects.spawn(effect, hit, frame);
}

// A firearm that fires away from its place in its object's tree (the
// commanders' strike firearms, which 0x005a0bf0 moves over the target):
// FireComp's fire (0x005f31d0) sends event 0 to the firearm's children
// where it is. Its EffectBundles whose startOnEffects is 0 start there
// (0x0061a4e0, as one-shots: nothing moves the firearm after), its
// GroundEffectBundles look down their -y (0x0061e090 -> 0x0061de40).
void ObjectEffects::weaponFiredAt(const world::ObjectTemplate& weapon, const Vec3& at, const Mat3& turn,
	EffectManager& effects, EffectManager::View view)
{
	for (const Placed& b : bundlesOf(m_templates, weapon, 0, {}, &m_hasBundles)) {
		if (b.path.size() != 1) {
			continue; // only the firearm's own children get the event
		}
		const Vec3 point = at + turn.transform(b.position);
		const Mat3 frame = b.rotation * turn;
		if (b.kind == Placed::Kind::GroundTrigger) {
			groundTriggerAt(point, frame, b, 0, effects);
		} else if (b.kind == Placed::Kind::Plain && b.startOnEffects == 0) {
			effects.spawn(b.bundle->name(), point, frame, view);
		}
	}
}

void ObjectEffects::update(const std::vector<ObjectState>& objects, EffectManager& effects, const PartPose& partPose,
	EffectManager::View view)
{
	constexpr uint32_t kEntered = 0x20; // event 4: a player entered (0x00593e30)
	constexpr uint32_t kExited = 0x40;  // event 5: the player left (0x005cea00)
	std::map<uint32_t, bool> seen;
	m_localWinds.clear();
	for (const ObjectState& object : objects) {
		if (!object.objectTemplate || object.id == 0) {
			continue;
		}
		seen[object.id] = true;
		auto found = m_objects.find(object.id);
		const bool created = found == m_objects.end() || found->second.objectTemplate != object.objectTemplate;
		if (created) {
			if (found != m_objects.end()) {
				effects.releaseAnchor(object.id);
			}
			Tracked tracked;
			tracked.objectTemplate = object.objectTemplate;
			tracked.bundles = bundlesOf(m_templates, *object.objectTemplate, object.id, partPose, &m_hasBundles);
			found = m_objects.insert_or_assign(object.id, std::move(tracked)).first;
		}
		Tracked& tracked = found->second;
		// The local player's own vehicle seen from inside: its bundles'
		// first-person form (0x006187c0).
		const EffectManager::View bundleView = object.firstPerson ? EffectManager::View::FirstPerson : view;
		if (created) {
			tracked.groundEffects.assign(tracked.bundles.size(), std::string());
			// A soldier's breathEffect (Soldier +0x3a0).
			if (const std::string* breath = setting(*object.objectTemplate, "breathEffect")) {
				tracked.breath = *breath;
			}
		}
		if (tracked.bundles.empty() && tracked.breath.empty()) {
			continue;
		}
		// Its velocity: the state's, else from its move over the tick.
		tracked.velocity = object.velocity;
		if (physics::lengthSquared(object.velocity) == 0 && tracked.moved) {
			tracked.velocity = (object.position - tracked.lastPosition) * 30.0f;
		}
		tracked.lastPosition = object.position;
		tracked.moved = true;
		effects.setAnchor(object.id, object.position, object.rotation);
		// Turning parts carry their bundles along: those with live
		// particles or lights now, a starting one before it spawns.
		if (partPose && !created) {
			for (const uint32_t tag : effects.mountedTags(object.id)) {
				if (tag == 0 || tag > tracked.bundles.size()) {
					continue;
				}
				Placed& b = tracked.bundles[tag - 1];
				place(b, object.id, partPose, b.position, b.rotation);
				effects.setMount(object.id, tag, b.position, b.rotation);
			}
		}
		// The cold breath (0x00588830): shown while the soldier breathes out
		// in the cold, on its head (the bundle's own place in the soldier).
		if (!tracked.breath.empty() && object.coldBreath != tracked.breathing) {
			if (object.coldBreath) {
				effects.spawnAttached(tracked.breath, object.id, {0, 1.6f, 0.15f}, {}, view, kBreathTag, false);
			} else {
				effects.stopAttached(object.id, kBreathTag);
			}
			tracked.breathing = object.coldBreath;
		}
		// A weapon's fire: event 0 to the weapon's children (FireComp).
		// 0x0061a4e0 starts the bundles whose startOnEffects is 0 (the
		// muzzle's flash and smoke), again on each round unless its
		// minDistanceUnderWaterSurface (+0x3f4) is 0 or more and it still
		// runs; 0x0061e090 sets off the GroundEffectBundles.
		for (const std::string& weapon : object.firedWeapons) {
			for (size_t i = 0; i < tracked.bundles.size(); ++i) {
				Placed& b = tracked.bundles[i];
				if (b.path.size() < 2 || !equalsIgnoreCase(b.path[b.path.size() - 2], weapon)) {
					continue;
				}
				if (b.kind == Placed::Kind::GroundTrigger && b.parent < 0) {
					groundTrigger(object, b, effects, partPose, created);
				} else if (b.kind == Placed::Kind::Plain && b.startOnEffects == 0
					&& (!effects.bundleModifiers(*b.bundle).depthGated()
						|| !effects.emitting(object.id, static_cast<uint32_t>(i + 1)))) {
					startBundle(object, tracked, i, effects, partPose, bundleView, created);
				}
			}
		}
		for (size_t i = 0; i < tracked.bundles.size(); ++i) {
			Placed& b = tracked.bundles[i];
			const uint32_t tag = static_cast<uint32_t>(i + 1);
			if (b.kind == Placed::Kind::Wind) {
				// The affector's wind, placed with its part (its object's
				// position and matrix, slots 0x60 / 0x78).
				if (partPose) {
					place(b, object.id, partPose, b.position, b.rotation);
				}
				m_localWinds.push_back(localWindOf(*b.bundle, object.position + object.rotation.transform(b.position),
					b.rotation * object.rotation));
				continue;
			}
			if (b.kind == Placed::Kind::GroundTrigger) {
				continue; // set off by its bundle's start or its weapon's fire
			}
			if (b.kind != Placed::Kind::Plain) {
				updateGround(object, tracked.velocity, tracked, i, effects, partPose, bundleView);
				continue;
			}
			const bool start = (created && b.startAtCreation)
				|| (object.occupied && !tracked.occupied && (b.startOnEffects & kEntered) != 0)
				|| (object.effects & b.startOnEffects) != 0;
			if (start && !effects.emitting(object.id, tag)) {
				startBundle(object, tracked, i, effects, partPose, bundleView, created);
			}
			// The depth gate (0x0061a760, BundleModifiers::maxUnderWater):
			// the water's height at the bundle less its own.
			const BundleModifiers& modifiers = effects.bundleModifiers(*b.bundle);
			if (modifiers.depthGated()) {
				const Vec3 point = object.position + object.rotation.transform(b.position);
				const bool in = !object.seated && modifiers.inDepth(effects.waterDepthAt(point));
				const bool running = effects.emitting(object.id, tag);
				if (in && !running) {
					startBundle(object, tracked, i, effects, partPose, bundleView, created);
				} else if (!in && running) {
					effects.stopAttached(object.id, tag);
				}
			}
			if (b.speedFromPhysics) {
				effects.setPhysicsVelocity(object.id, tag, object.velocity); // 0x00618ae0, each tick
			}
			// Its modifiers each tick (0x0061a760 -> 0x00618e30) from the
			// object's motion at the bundle; the rewrite takes the speeds
			// from the bundle's moves over the tick (1/30 s).
			if (modifiers.anyMapping && effects.emitting(object.id, tag)) {
				const Vec3 point = object.position + object.rotation.transform(b.position);
				const Vec3 pointVelocity = b.hasLastPoint ? (point - b.lastPoint) * 30.0f : tracked.velocity;
				BundleModifiers::Inputs inputs;
				inputs.speed = std::sqrt(physics::lengthSquared(pointVelocity));
				inputs.rotationalSpeed = std::sqrt(physics::lengthSquared(pointVelocity - tracked.velocity));
				inputs.acceleration = b.hasLastPoint ? std::sqrt(physics::lengthSquared((pointVelocity - b.lastPointVelocity) * 30.0f)) : 0;
				inputs.altitude = effects.altitudeAt(point, modifiers.minUnderWater >= 0);
				inputs.engine = b.onEngine ? object.engine : 0;
				inputs.speedInDof = physics::dot(pointVelocity, (b.rotation * object.rotation).transform({0, 0, 1}));
				std::array<float, 6> values {};
				const unsigned mask = modifiers.evaluate(inputs, values);
				for (int m = 0; m < 6; ++m) {
					if ((mask & (1u << m)) != 0) {
						effects.setModifier(object.id, tag, m, values[static_cast<size_t>(m)]);
					}
				}
				b.lastPoint = point;
				b.lastPointVelocity = pointVelocity;
				b.hasLastPoint = true;
			}
			if (!object.occupied && tracked.occupied && (b.stopOnEffects & kExited) != 0) {
				effects.stopAttached(object.id, tag);
			}
		}
		tracked.occupied = object.occupied;
	}
	for (auto it = m_objects.begin(); it != m_objects.end();) {
		if (!seen.count(it->first)) {
			effects.releaseAnchor(it->first);
			it = m_objects.erase(it);
		} else {
			++it;
		}
	}
}

namespace {

// A "From" mapping a/b (0x0061bd20's slot 8): 0 at a, 1 at b, clamped.
float mapping(const std::array<float, 2>& from, float x)
{
	if (from[0] == from[1]) {
		return x >= from[1] ? 1.0f : 0.0f;
	}
	return std::clamp((x - from[0]) / (from[1] - from[0]), 0.0f, 1.0f);
}

std::array<float, 2> vector2(const world::ObjectTemplate& t, const char* name, std::array<float, 2> fallback)
{
	if (const std::string* value = setting(t, name)) {
		std::sscanf(value->c_str(), "%f/%f", &fallback[0], &fallback[1]);
	}
	return fallback;
}

} // namespace

bool BundleModifiers::Modifier::mapped() const
{
	for (const auto& f : from) {
		if (f[0] != 0 || f[1] != 0) {
			return true;
		}
	}
	return false;
}

BundleModifiers BundleModifiers::fromTemplate(const world::ObjectTemplate& t)
{
	BundleModifiers b;
	static const char* const kNames[6] = {"alphaIntensity", "emitScaleFactor", "emitSpeedScale", "AgeScaleFactor",
		"startAge", "soundIntensity"};
	static const char* const kInputs[6] = {"FromSpeed", "FromRotationalSpeed", "FromAcceleration", "FromAltitude",
		"FromEngine", "FromSpeedInDof"};
	for (int i = 0; i < 6; ++i) {
		Modifier& m = b.modifiers[static_cast<size_t>(i)];
		const std::string base = kNames[i];
		const auto range = vector2(t, (base + "Range").c_str(), {1, 1});
		m.min = range[0];
		m.max = range[1];
		m.used = number(t, (base + "Used").c_str(), 0) != 0;
		for (int j = 0; j < 6; ++j) {
			m.from[static_cast<size_t>(j)] = vector2(t, (base + kInputs[j]).c_str(), {0, 0});
		}
		b.anyMapping = b.anyMapping || m.mapped();
	}
	b.minUnderWater = number(t, "minDistanceUnderWaterSurface", b.minUnderWater);
	b.maxUnderWater = number(t, "maxDistanceUnderWaterSurface", b.maxUnderWater);
	return b;
}

bool BundleModifiers::any() const
{
	if (anyMapping) {
		return true;
	}
	for (const Modifier& m : modifiers) {
		if (m.used) {
			return true;
		}
	}
	return false;
}

unsigned BundleModifiers::evaluate(const Inputs& inputs, std::array<float, 6>& out) const
{
	unsigned mask = 0;
	const float x[6] = {inputs.speed, inputs.rotationalSpeed, inputs.acceleration, inputs.altitude, inputs.engine,
		std::max(inputs.speedInDof, 0.0f)};
	for (int i = 0; i < 6; ++i) {
		const Modifier& m = modifiers[static_cast<size_t>(i)];
		float value = 0;
		bool set = false;
		if (anyMapping) {
			float f = 1;
			for (int j = 0; j < 6; ++j) {
				const auto& from = m.from[static_cast<size_t>(j)];
				if (from[0] != 0 || from[1] != 0) {
					f *= mapping(from, x[j]);
					set = true;
				}
			}
			value = (m.max - m.min) * f + m.min;
		} else if (m.used) {
			value = m.min;
			set = true;
		}
		if (set) {
			out[static_cast<size_t>(i)] = i == 5 ? std::clamp(value, 0.0f, 1.0f) : value;
			mask |= 1u << i;
		}
	}
	return mask;
}

ObjectEffects::GroundBundle ObjectEffects::GroundBundle::fromTemplate(const world::ObjectTemplate& t)
{
	GroundBundle g;
	g.material = static_cast<uint32_t>(number(t, "material", 0));
	g.startDistance = number(t, "startDistance", 0);
	g.maxDistance = number(t, "maxDistance", 0);
	g.engineInfluence = number(t, "engineInfluence", 0);
	static const char* const kNames[4] = {"alphaIntensity", "emitScaleFactor", "emitSpeedScale", "AgeScaleFactor"};
	for (int i = 0; i < 4; ++i) {
		Modifier& m = g.modifiers[static_cast<size_t>(i)];
		const std::string base = kNames[i];
		const auto range = vector2(t, (base + "Range").c_str(), {1, 1});
		m.min = range[0];
		m.max = range[1];
		m.fromSpeed = vector2(t, (base + "FromSpeed").c_str(), {0, 0});
		m.fromSpeedInDof = vector2(t, (base + "FromSpeedInDof").c_str(), {0, 0});
		m.fromAltitude = vector2(t, (base + "FromAltitude").c_str(), {0, 0});
		m.fromEngine = vector2(t, (base + "FromEngine").c_str(), {0, 0});
	}
	return g;
}

float ObjectEffects::GroundBundle::value(int modifier, float speed, float speedInDof, float altitude, float engine) const
{
	const Modifier& m = modifiers[static_cast<size_t>(std::clamp(modifier, 0, 3))];
	const auto set = [](const std::array<float, 2>& from) { return from[0] != 0 || from[1] != 0; };
	float f = 1;
	if (set(m.fromSpeed)) {
		f *= mapping(m.fromSpeed, speed);
	}
	if (set(m.fromSpeedInDof)) {
		f *= mapping(m.fromSpeedInDof, std::max(speedInDof, 0.0f));
	}
	if (set(m.fromAltitude)) {
		f *= mapping(m.fromAltitude, altitude);
	}
	if (set(m.fromEngine)) {
		f *= mapping(m.fromEngine, engine);
	}
	return m.min + (m.max - m.min) * f;
}

// A wheel's or a draft's effect this tick (0x0061bfc0, 0x0061d820): the
// ground under it picks it (stopping the last when it changes), it is put
// on the ground, upright, and its modifiers follow the vehicle.
void ObjectEffects::updateGround(const ObjectState& object, const Vec3& velocity, Tracked& tracked, size_t index,
	EffectManager& effects, const PartPose& partPose, EffectManager::View view)
{
	Placed& b = tracked.bundles[index];
	const uint32_t tag = static_cast<uint32_t>(index + 1);
	auto known = m_groundBundles.find(b.bundle);
	if (known == m_groundBundles.end()) {
		known = m_groundBundles.emplace(b.bundle, GroundBundle::fromTemplate(*b.bundle)).first;
	}
	const GroundBundle& g = known->second;
	const float speed = std::sqrt(physics::lengthSquared(velocity));
	// Idle and parked: nothing to show (a wheel's modifiers go with the
	// speed, a draft's with the engine), and no rays to cast.
	const bool idle = !object.occupied && object.engine <= 0 && speed < 0.5f;
	if (partPose && !idle) {
		place(b, object.id, partPose, b.position, b.rotation);
	}
	const Vec3 at = object.position + object.rotation.transform(b.position);
	std::string effect;
	Vec3 hit;
	uint32_t material = 0;
	float altitude = 0;
	// A wheel's own contact (the vehicle's wheel list, 0x0061bfc0: on while
	// its wheel touches, at the contact; the ground material from just
	// above and below it).
	const ObjectState::WheelContact* wheel = nullptr;
	if (b.kind == Placed::Kind::Wheel) {
		for (const ObjectState::WheelContact& w : object.wheels) {
			for (const std::string& step : b.path) {
				if (!w.spring.empty() && equalsIgnoreCase(step, w.spring)) {
					wheel = &w;
				}
			}
		}
	}
	if (wheel && m_probe && m_materialEffect && !idle) {
		if (wheel->contact && m_probe(wheel->point + Vec3 {0, 0.5f, 0}, wheel->point - Vec3 {0, 0.5f, 0}, object.id, hit, material)) {
			hit = wheel->point;
			effect = m_materialEffect(g.material, material);
		}
	} else if (m_probe && m_materialEffect && !idle) {
		const bool draft = b.kind == Placed::Kind::AirDraft;
		// The draft looks along its -y for startDistance + 5; a wheel just
		// below it (its contact; the rewrite has no wheel contact list here).
		const Vec3 down = draft ? (b.rotation * object.rotation).transform({0, -1, 0}) : Vec3 {0, -1, 0};
		const float reach = draft ? g.startDistance + 5 : 1.5f;
		const bool engineOn = !draft || object.engine > 0 || object.occupied;
		if (engineOn && m_probe(at, at + down * reach, object.id, hit, material)) {
			altitude = std::sqrt(physics::lengthSquared(hit - at));
			if (!draft || altitude < g.startDistance) {
				effect = m_materialEffect(g.material, material);
			}
		}
	}
	std::string& current = tracked.groundEffects[index];
	if (effect != current) {
		if (!current.empty()) {
			effects.stopAttached(object.id, tag);
		}
		current = effect;
		if (!effect.empty()) {
			effects.spawnAttached(effect, object.id, {}, {}, view, tag, false);
		}
	}
	if (current.empty()) {
		return;
	}
	// Placed on the ground, upright in the world: the mount is the hit in
	// the object's space, its turn the object's undone.
	const auto& m = object.rotation.rows;
	Mat3 upright;
	upright.rows = {Vec3 {m[0].x, m[1].x, m[2].x}, Vec3 {m[0].y, m[1].y, m[2].y}, Vec3 {m[0].z, m[1].z, m[2].z}};
	effects.setMount(object.id, tag, object.rotation.untransform(hit - object.position), upright);
	const float inDof = physics::dot(velocity, object.rotation.transform({0, 0, 1}));
	const float engine = object.engine > 0 ? object.engine : object.occupied ? 1.0f : 0.0f;
	if (b.kind == Placed::Kind::Wheel) {
		// 0x0061bd20: every system of the wheel's effect takes (1, 1, sign
		// of the speed along the vehicle's forward) through slot 0x70
		// (0x00618c80), whatever emitDirectionZFromSpeedDof says: that
		// flag (the template's +0x398, 1 from 0x0061cb80, slots 0x100 /
		// 0x104) is read only by its console row and the write-back.
		effects.setDirectionScale(object.id, tag, {1, 1, inDof < 0 ? -1.0f : 1.0f});
	}
	// Only the modifiers with a mapping set are set (0x0061bd20's bVar1).
	for (int i = 0; i < 4; ++i) {
		const GroundBundle::Modifier& m = g.modifiers[static_cast<size_t>(i)];
		const auto set = [](const std::array<float, 2>& from) { return from[0] != 0 || from[1] != 0; };
		if (set(m.fromSpeed) || set(m.fromSpeedInDof) || set(m.fromAltitude) || set(m.fromEngine)) {
			effects.setModifier(object.id, tag, i, g.value(i, speed, inDof, altitude, engine));
		}
	}
}

} // namespace dice::hfe::effects

namespace dice::hfe::effects {

int Weather::rand()
{
	m_randState = m_randState * 214013u + 2531011u;
	return static_cast<int>((m_randState >> 16) & 0x7fff);
}

void Weather::rebuildDeviations(const scene::RainSettings& s)
{
	for (Deviation& d : m_deviations) {
		d.angle[0] = static_cast<float>(rand() % 3600) * 0.1f;
		d.angle[1] = static_cast<float>(rand() % 3600) * 0.1f;
		d.speed[0] = s.angleSpeed[0];
		d.speed[1] = s.angleSpeed[1];
		for (int k = 0; k < 3; ++k) {
			d.size[k] = s.maxDeviation[static_cast<size_t>(k)] == 0.0f ? 0.0f
				: static_cast<float>(rand() % 1000) / (1000.0f / s.maxDeviation[static_cast<size_t>(k)]);
		}
	}
	m_deviationSpeed = s.angleSpeed;
	m_deviationSize = s.maxDeviation;
	m_deviationsBuilt = true;
}

void Weather::update(const scene::RainSettings& s, const Vec3& camera, float seconds)
{
	m_drops.clear();
	const int count = s.enabled ? std::clamp(s.particleCount, 0, kMaxParticles) : 0;
	const Vec3 radius {std::max(s.emitRadius[0], 0.1f), std::max(s.emitRadius[1], 0.1f), std::max(s.emitRadius[2], 0.1f)};
	// rainManager.freeze (+0x65, slot 0xa0 0x1011ab20): the box stops
	// following the camera (0x1011d250 moves its centre, +0x70, to the
	// camera plus offset only while it is clear).
	if (!s.freeze || !m_centreSet) {
		m_centre = camera + Vec3 {s.offset[0], s.offset[1], s.offset[2]};
		m_centreSet = true;
	}
	const Vec3 centre = m_centre;
	if (static_cast<int>(m_positions.size()) != count) {
		m_positions.resize(static_cast<size_t>(count));
		for (Vec3& p : m_positions) {
			p = centre + Vec3 {(m_random.unit() * 2 - 1) * radius.x, (m_random.unit() * 2 - 1) * radius.y,
				(m_random.unit() * 2 - 1) * radius.z};
		}
	}
	if (count == 0) {
		return;
	}
	if (!m_deviationsBuilt || m_deviationSpeed != s.angleSpeed || m_deviationSize != s.maxDeviation) {
		rebuildDeviations(s);
	}
	// 0x1011a860, once a frame.
	constexpr float kRadians = 0.017453292f; // DAT_101f6a98
	for (Deviation& d : m_deviations) {
		const int roll = s.randomness != 0 ? rand() % s.randomness : 0;
		if (roll == s.randomness - 1) {
			for (float& speed : d.speed) {
				speed = s.randomDeviationAngleSpeed != 0
					? static_cast<float>(rand() % s.randomDeviationAngleSpeed) * s.maxRandomChange : 0.0f;
			}
		}
		d.offset = {std::cos(kRadians * d.angle[0]) * d.size[0], std::sin(kRadians * d.angle[1]) * d.size[1],
			std::sin(kRadians * d.angle[0]) * d.size[2]};
		for (int k = 0; k < 2; ++k) {
			d.angle[k] += (s.angleSpeed[0] + d.speed[k]) * seconds;
			if (360.0f < d.angle[k]) {
				d.angle[k] -= 360.0f;
			}
		}
	}
	m_time += seconds;
	const Vec3 velocity = Vec3 {s.particleVelocity[0], s.particleVelocity[1], s.particleVelocity[2]} * s.particleSpeed;
	const Vec3 direction = normalized(velocity);
	const auto wrap = [](float v, float r) {
		const float span = 2 * r;
		v = std::fmod(v + r, span);
		return (v < 0 ? v + span : v) - r;
	};
	const float fadeStart = std::clamp(s.fadeOutRange, 0.0f, 0.999f);
	m_drops.reserve(m_positions.size());
	for (size_t i = 0; i < m_positions.size(); ++i) {
		Vec3& p = m_positions[i];
		p += velocity * seconds;
		Vec3 d = p - centre;
		d = {wrap(d.x, radius.x), wrap(d.y, radius.y), wrap(d.z, radius.z)};
		p = centre + d;
		// Its deviation group's offset.
		Drop drop;
		drop.position = p + m_deviations[i % 16].offset;
		// vsPoint: 1 - |saturate((|delta| - range) / (radius - range))|.
		const Vec3 over {std::clamp((std::fabs(d.x) - radius.x * fadeStart) / (radius.x * (1 - fadeStart)), 0.0f, 1.0f),
			std::clamp((std::fabs(d.y) - radius.y * fadeStart) / (radius.y * (1 - fadeStart)), 0.0f, 1.0f),
			std::clamp((std::fabs(d.z) - radius.z * fadeStart) / (radius.z * (1 - fadeStart)), 0.0f, 1.0f)};
		drop.alpha = std::max(0.0f, 1 - std::sqrt(physics::lengthSquared(over)));
		if (drop.alpha <= 0) {
			continue;
		}
		const float distance = std::sqrt(physics::lengthSquared(drop.position - camera));
		const float scale = s.particleScale[0] + s.particleScale[1] * distance;
		drop.pixels = scale > 0 ? std::min(s.particleSize * std::sqrt(1 / scale), s.particleMaxSize) : s.particleMaxSize;
		drop.tail = drop.position - direction * s.lineLength;
		m_drops.push_back(drop);
	}
}

} // namespace dice::hfe::effects
