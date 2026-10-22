#include "Effects.h"

#include "World/ObjectTemplateManager.h"

#include <algorithm>
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

ParticleTemplate ParticleTemplate::fromTemplate(const world::ObjectTemplate& t)
{
	ParticleTemplate p;
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
	return p;
}

Emitter::Emitter(const ParticleTemplate& t, const Vec3& position, const Mat3& rotation)
	: m_template(&t), m_position(position), m_rotation(rotation)
{
}

bool Emitter::alive() const
{
	const ParticleTemplate& t = *m_template;
	return !m_particles.empty() || m_age < t.emitDelay + t.emitTime;
}

void Emitter::emit(game::Random& random)
{
	const ParticleTemplate& t = *m_template;
	Particle p;
	const Vec3 offset {t.emitRange[0].sample(random), t.emitRange[1].sample(random), t.emitRange[2].sample(random)};
	p.position = m_position + m_rotation.transform(offset);
	const Vec3 axis = normalized(t.emitDirection);
	Vec3 direction = axis;
	if (t.emitter == ParticleTemplate::Emitter::RadialDirection) {
		// Around the axis, at radialDirection from the plane across it, give
		// or take randomRadialDirection.
		const float around = random.unit() * 6.2831853f;
		const float elevation = (t.radialDirection + (random.unit() * 2 - 1) * t.randomRadialDirection) * kDegrees;
		const Vec3 u = perpendicular(axis), v = physics::cross(axis, u);
		const Vec3 radial = u * std::cos(around) + v * std::sin(around);
		direction = axis * std::sin(elevation) + radial * std::cos(elevation);
	} else if (t.randomDirectionAngle.x != 0 || t.randomDirectionAngle.y != 0 || t.randomDirectionAngle.z != 0) {
		const Vec3 angles {(random.unit() * 2 - 1) * t.randomDirectionAngle.x,
			(random.unit() * 2 - 1) * t.randomDirectionAngle.y, (random.unit() * 2 - 1) * t.randomDirectionAngle.z};
		direction = Mat3::fromYawPitchRoll({angles.y, angles.x, angles.z}).transform(axis);
	}
	const float speed = t.emitSpeed + random.unit() * t.randomSpeed;
	p.velocity = m_rotation.transform(direction) * speed;
	p.life = std::max(0.01f, t.timeToLive + random.unit() * t.randomTimeToLive);
	p.sizeOffset = random.unit() * t.randomSize;
	p.rotation = random.unit() * t.randomRotation * 6.2831853f;
	if (m_particles.size() < kMaxParticlesPerEmitter) {
		m_particles.push_back(p);
	}
}

void Emitter::update(float seconds, game::Random& random)
{
	const ParticleTemplate& t = *m_template;
	m_age += seconds;
	const float active = m_age - t.emitDelay;
	if (active > 0 && (t.isLooping || active - seconds < t.emitTime)) {
		const float span = t.isLooping ? seconds : std::min(seconds, t.emitTime - std::max(0.0f, active - seconds));
		m_pending += t.emitFrequency * std::max(0.0f, span);
		while (m_pending >= 1.0f) {
			emit(random);
			m_pending -= 1.0f;
		}
	}
	for (Particle& p : m_particles) {
		const float f = std::min(p.age / p.life, 1.0f);
		p.velocity.y += t.gravity * t.gravityGraph.at(f) * seconds;
		const float drag = std::max(0.0f, 1.0f - t.airResistance * t.airResistanceGraph.at(f) * seconds);
		p.velocity = p.velocity * drag;
		p.position += p.velocity * seconds;
		p.rotation += t.rotationSpeed * seconds;
		p.age += seconds;
	}
	m_particles.erase(std::remove_if(m_particles.begin(), m_particles.end(),
						  [](const Particle& p) { return p.age >= p.life; }),
		m_particles.end());
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

void EffectManager::spawnTemplate(const world::ObjectTemplate& t, const Vec3& position, const Mat3& rotation, int depth)
{
	if (depth > 4) {
		return;
	}
	const std::string& type = t.typeName();
	if (type == "SpriteParticleSystem" || type == "MeshParticleSystem" || type == "NonScreenAlignedParticleSystem") {
		const ParticleTemplate* particles = particleTemplate(t);
		if (!particles->texture.empty()) { // mesh particles are not drawn yet
			m_emitters.push_back(std::make_unique<Emitter>(*particles, position, rotation));
		}
		return;
	}
	if (type != "EffectBundle") {
		return; // sounds, light sources: not drawn here
	}
	for (const world::ChildTemplate& child : t.children()) {
		if (const world::ObjectTemplate* childTemplate = m_templates.find(child.name)) {
			const Mat3 local = Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z});
			spawnTemplate(*childTemplate,
				position + rotation.transform({child.position.x, child.position.y, child.position.z}),
				local * rotation, depth + 1);
		}
	}
}

bool EffectManager::spawn(const std::string& name, const Vec3& position, const Mat3& rotation)
{
	const world::ObjectTemplate* t = m_templates.find(name);
	if (!t) {
		return false;
	}
	spawnTemplate(*t, position, rotation, 0);
	return true;
}

void EffectManager::update(float seconds)
{
	for (const auto& emitter : m_emitters) {
		emitter->update(seconds, m_random);
	}
	m_emitters.erase(std::remove_if(m_emitters.begin(), m_emitters.end(),
						 [](const std::unique_ptr<Emitter>& e) { return !e->alive(); }),
		m_emitters.end());
}

// The look of a particle at its age (Particles.fx vsParticle).
void EffectManager::collect(const Emitter& emitter, std::vector<Sprite>& out)
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
		out.push_back({p.position, size, p.rotation, {color.x, color.y, color.z, alpha}});
	}
}

} // namespace dice::hfe::effects
