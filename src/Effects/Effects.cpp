#include "Effects.h"

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
	}
	if (const std::string* type = setting(t, "emitterType")) {
		p.emitter = *type == "RadialDirection" ? Emitter::RadialDirection : Emitter::Standard;
	}
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
		p.drawNewestFirst = order->size() == 4 && std::tolower(static_cast<unsigned char>((*order)[0])) == 'b';
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
		p.alignToSpeed = number(t, "alignRotationToSpeed", 0) != 0;
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
	p.noRotationFromParent = number(t, "noRotationFromParent", 0) != 0;
	p.localSystem = number(t, "isLocalSystem", 0) != 0;
	p.showFirstPerson = number(t, "showInFirstPerson", 1) != 0;
	p.showThirdPerson = number(t, "showInThirdPerson", 1) != 0;
	return p;
}

Emitter::Emitter(const ParticleTemplate& t, const Vec3& position, const Mat3& rotation)
	: m_template(&t), m_position(position), m_rotation(rotation), m_previous(position)
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
	m_rotation = rotation;
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

// 0x1000f070.
float saturate(float v)
{
	return std::clamp(v, 0.0f, 1.0f);
}

constexpr float kStep = 0.01f;          // the particles' fixed step (DAT_101f7030)
constexpr float kPreWarmStep = 1 / 30.0f; // the pre-warm's (0x101407c0)
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
	p.age = age;
	p.alpha = alphaIntensity;
	// Size and speed share one random (0x1013fdc0's local_8).
	const float shared = signedRandom(random);
	p.sizeOffset = shared * t.randomSize;
	p.randomIntensity = signedRandom(random) * t.randomIntensity;
	p.rotation = signedRandom(random) * t.randomRotation;
	const float spin = signedRandom(random) * t.randomRotationSpeed;
	p.rotationSpeed = spin < 0 ? spin - t.rotationSpeed : spin + t.rotationSpeed;
	const Vec3 offset {t.emitRange[0].sample(random), t.emitRange[1].sample(random), t.emitRange[2].sample(random)};
	// A local system keeps its particles in its own space.
	p.position = t.localSystem ? offset : at + m_rotation.transform(offset);
	const Vec3 axis = normalized(t.emitDirection);
	Vec3 direction = axis;
	if (t.emitter == ParticleTemplate::Emitter::RadialDirection) {
		// Around the axis, at radialDirection from the plane across it, give
		// or take randomRadialDirection.
		const float around = random.unit() * kTwoPi;
		const float elevation = (t.radialDirection + signedRandom(random) * t.randomRadialDirection) * kDegrees;
		const Vec3 u = perpendicular(axis), v = physics::cross(axis, u);
		const Vec3 radial = u * std::cos(around) + v * std::sin(around);
		direction = axis * std::sin(elevation) + radial * std::cos(elevation);
	} else if (t.randomDirectionAngle.x != 0 || t.randomDirectionAngle.y != 0 || t.randomDirectionAngle.z != 0) {
		const Vec3 angles {signedRandom(random) * t.randomDirectionAngle.x, signedRandom(random) * t.randomDirectionAngle.y,
			signedRandom(random) * t.randomDirectionAngle.z};
		direction = Mat3::fromYawPitchRoll({angles.y, angles.x, angles.z}).transform(axis);
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
		p.angles = {signedRandom(random) * t.randomRotation3.x, signedRandom(random) * t.randomRotation3.y,
			signedRandom(random) * t.randomRotation3.z};
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
		for (; left > 0; left -= kPreWarmStep) {
			step(std::min(left, kPreWarmStep) / std::max(timeScale, 1e-3f), random, context);
		}
		m_prewarming = false;
	}
	if (m_stopped || !m_active) {
		m_emitClock = m_clock;
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
		m_lastPosition = m_position;
		m_placed = true;
		const bool emitting = t.isLooping || anchor != 0 || active - seconds < t.emitTime;
		if (!m_stopped && m_active && active > 0 && emitting && m_speed >= t.minTrailVelocity) {
			m_pending += t.emitFrequency * emitScale * seconds;
			while (m_pending >= 1.0f) {
				emit(random, m_position, 0, context);
				m_pending -= 1.0f;
			}
		}
		for (Particle& p : m_particles) {
			p.age += seconds;
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
	if (type == "SpriteParticleSystem" || type == "MeshParticleSystem" || type == "NonScreenAlignedParticleSystem"
		|| type == "TrailSystem") {
		const ParticleTemplate* particles = particleTemplate(t);
		if ((view == View::FirstPerson && !particles->showFirstPerson)
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
	if (type != "EffectBundle") {
		return; // light sources, mesh emitters: not drawn here
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
			spawnTemplate(*childTemplate, at, local * rotation, depth + 1, view, spawned, spawnedLights);
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

bool EffectManager::spawnDecal(const std::string& name, const Vec3& position, const Vec3& normal)
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
	decal.normal = normalized(normal);
	decal.position = position;
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
	if (m_decals.size() >= kMaxDecals) {
		m_decals.erase(m_decals.begin());
	}
	m_decals.push_back(decal);
	return true;
}

bool EffectManager::spawnDecal(const std::string& name, const Vec3& position, const Vec3& normal, uint32_t object,
	const Vec3& objectPosition, const Vec3& objectRotation)
{
	if (!spawnDecal(name, position, normal)) {
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
	m_decals.erase(std::remove_if(m_decals.begin(), m_decals.end(),
					   [object](const Decal& decal) { return decal.object == object; }),
		m_decals.end());
}

std::vector<PointLight> EffectManager::lights() const
{
	std::vector<PointLight> out;
	for (const LiveLight& light : m_lights) {
		const float fade = light.life < 0 ? 1.0f : std::clamp(1.0f - light.age / light.life, 0.0f, 1.0f);
		if (fade > 0) {
			PointLight lit {light.position, light.color * fade, light.range1, light.range2};
			lit.spot = light.look.spot;
			lit.direction = light.direction;
			lit.cosInner = light.look.cosInner;
			lit.cosOuter = light.look.cosOuter;
			lit.affectsLightmapped = light.look.affectsLightmapped;
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
	look.dynamic = number(t, "isDynamic", 1) != 0;
	look.affectsLightmapped = number(t, "affectLightmappedObjects", 1) != 0;
	look.affectingType = static_cast<int>(number(t, "affectingType", 0));
	look.color = vector(t, "color", {1, 1, 1}) * number(t, "intensity", 1);
	look.range1 = number(t, "attenuationRange1", 0);
	look.range2 = std::max(number(t, "attenuationRange2", 1), look.range1 + 0.01f);
	look.direction = normalized(vector(t, "direction", {0, 0, 1}));
	// The cone's angles taken as half angles in degrees, inner and outer
	// (the original looks the falloff up in a table, LightManager's
	// conical attenuation texture over 1 - cos^2; not read here).
	const float inner = number(t, "coneAngle1", 20), outer = std::max(number(t, "coneAngle2", 22.5f), inner);
	look.cosInner = std::cos(inner * kDegrees);
	look.cosOuter = std::cos(outer * kDegrees);
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
	View view, uint32_t tag)
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
	for (Emitter* emitter : spawned) {
		emitter->anchor = anchor;
		emitter->tag = tag;
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
	return tags;
}

bool EffectManager::emitting(uint32_t anchor, uint32_t tag) const
{
	for (const auto& emitter : m_emitters) {
		if (emitter->anchor == anchor && (tag == 0 || emitter->tag == tag) && !emitter->stopped()) {
			return true;
		}
	}
	return false;
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
	m_anchors.erase(anchor);
}

void EffectManager::update(float seconds)
{
	// Emitters attached to nothing that is still known end.
	for (const auto& emitter : m_emitters) {
		if (emitter->anchor != 0 && !m_anchors.count(emitter->anchor)) {
			emitter->stop();
		}
	}
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
		const Vec3 angles = p.angles + t.rotationSpeed3 * p.age;
		Mat3 turn = Mat3::fromYawPitchRoll({angles.x, angles.y, angles.z});
		const Mat3 system = t.localSystem ? emitter.rotation() : Mat3 {};
		if (t.alignToSpeed && physics::lengthSquared(p.velocity) > 1e-6f) {
			// z along the velocity, y as near the world's up as it goes.
			const Vec3 z = normalized(t.localSystem ? emitter.rotation().transform(p.velocity) : p.velocity);
			const Vec3 x = std::fabs(z.y) < 0.99f ? normalized(physics::cross({0, 1, 0}, z)) : Vec3 {1, 0, 0};
			Mat3 aligned;
			aligned.rows = {x, physics::cross(z, x), z};
			turn = turn * aligned;
		} else {
			turn = turn * (t.localSystem ? system : emitter.rotation());
		}
		out.push_back({emitter.worldPosition(p), turn, size, {color.x, color.y, color.z, alpha}});
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
	for (size_t i = 0; i < particles.size(); ++i) {
		const Particle& p = particles[t.drawNewestFirst ? particles.size() - 1 - i : i];
		const float f = std::min(p.age / p.life, 1.0f);
		const float blend = std::min(t.colorBlendGraph.at(f), 1.0f);
		float alpha = std::clamp(std::min(t.transparencyGraph.at(f), 1.0f) * p.alpha, 0.0f, 1.0f);
		const float size = std::min(t.sizeGraph.at(f), 1.0f) * t.particleMaxSize + p.sizeOffset;
		if (size <= 0 || alpha <= 0 || alpha < t.alphaCull) {
			continue;
		}
		const Vec3 at = emitter.worldPosition(p);
		if (view.settings) {
			alpha *= view.fade(physics::dot(at - view.eye, view.forward), t.lodDistance);
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
		|| hasWindAffector(t)) {
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
				if (type == "EffectBundle" || type == "WheelEffectBundle" || type == "AirDraftEffectBundle") {
					Placed placed;
					placed.kind = type == "WheelEffectBundle" ? Placed::Kind::Wheel
						: type == "AirDraftEffectBundle"      ? Placed::Kind::AirDraft
															  : Placed::Kind::Plain;
					placed.bundle = c;
					placed.path = path;
					placed.chain = chain;
					placed.position = childAt;
					placed.rotation = childTurn;
					placed.startOnEffects = flags(*c, "startOnEffects", "setStartOnEffects");
					placed.stopOnEffects = flags(*c, "stopOnEffects", "setStopOnEffects");
					placed.startAtCreation = number(*c, "startAtCreation", 0) != 0;
					placed.speedFromPhysics = number(*c, "speedFromPhysics", 0) != 0;
					out.push_back(std::move(placed));
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
				effects.spawnAttached(tracked.breath, object.id, {0, 1.6f, 0.15f}, {}, view, kBreathTag);
			} else {
				effects.stopAttached(object.id, kBreathTag);
			}
			tracked.breathing = object.coldBreath;
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
			if (b.kind != Placed::Kind::Plain) {
				updateGround(object, tracked.velocity, tracked, i, effects, partPose, view);
				continue;
			}
			const bool start = (created && b.startAtCreation)
				|| (object.occupied && !tracked.occupied && (b.startOnEffects & kEntered) != 0)
				|| (object.effects & b.startOnEffects) != 0;
			if (start && !effects.emitting(object.id, tag)) {
				if (partPose && !created) {
					place(b, object.id, partPose, b.position, b.rotation);
				}
				effects.spawnAttached(b.bundle->name(), object.id, b.position, b.rotation, view, tag);
			}
			if (b.speedFromPhysics) {
				effects.setPhysicsVelocity(object.id, tag, object.velocity); // 0x00618ae0, each tick
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

bool equalsIgnoreCase(const std::string& a, const std::string& b)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
		return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
	});
}

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
			effects.spawnAttached(effect, object.id, {}, {}, view, tag);
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
	effects.setModifiers(object.id, tag, g.value(0, speed, inDof, altitude, engine), g.value(1, speed, inDof, altitude, engine),
		g.value(2, speed, inDof, altitude, engine), g.value(3, speed, inDof, altitude, engine));
}

} // namespace dice::hfe::effects

namespace dice::hfe::effects {

void Weather::update(const scene::RainSettings& s, const Vec3& camera, float seconds)
{
	m_drops.clear();
	const int count = s.enabled ? std::clamp(s.particleCount, 0, kMaxParticles) : 0;
	const Vec3 radius {std::max(s.emitRadius[0], 0.1f), std::max(s.emitRadius[1], 0.1f), std::max(s.emitRadius[2], 0.1f)};
	const Vec3 centre = camera + Vec3 {s.offset[0], s.offset[1], s.offset[2]};
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
		// Its group's sway (16 groups, phases spread round the circle).
		const float phase = static_cast<float>(i % 16) * 0.3926991f;
		const float a = phase + m_time * s.angleSpeed[0] * kDegrees, b = phase + m_time * s.angleSpeed[1] * kDegrees;
		const Vec3 sway {s.maxDeviation[0] * std::sin(a), s.maxDeviation[1] * std::sin(b), s.maxDeviation[2] * std::cos(a)};
		Drop drop;
		drop.position = p + sway;
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
