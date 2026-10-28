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
	if (p.mesh) {
		p.geometry = t.geometry();
		p.randomRotation3 = vector(t, "randomRotation", {});
		p.rotationSpeed3 = vector(t, "rotationSpeed", {});
		p.alignToSpeed = number(t, "alignRotationToSpeed", 0) != 0;
	}
	p.localSystem = number(t, "isLocalSystem", 0) != 0;
	p.showFirstPerson = number(t, "showInFirstPerson", 1) != 0;
	p.showThirdPerson = number(t, "showInThirdPerson", 1) != 0;
	return p;
}

Emitter::Emitter(const ParticleTemplate& t, const Vec3& position, const Mat3& rotation)
	: m_template(&t), m_position(position), m_rotation(rotation)
{
}

bool Emitter::alive() const
{
	const ParticleTemplate& t = *m_template;
	if (!m_particles.empty()) {
		return true;
	}
	// An attached looping system emits until it is stopped.
	return !m_stopped && (m_age < t.emitDelay + t.emitTime || (anchor != 0 && t.isLooping));
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

void Emitter::emit(game::Random& random)
{
	const ParticleTemplate& t = *m_template;
	Particle p;
	const Vec3 offset {t.emitRange[0].sample(random), t.emitRange[1].sample(random), t.emitRange[2].sample(random)};
	// A local system keeps its particles in its own space.
	p.position = t.localSystem ? offset : m_position + m_rotation.transform(offset);
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
	p.velocity = (t.localSystem ? direction : m_rotation.transform(direction)) * speed;
	p.life = std::max(0.01f, t.timeToLive + random.unit() * t.randomTimeToLive);
	p.sizeOffset = random.unit() * t.randomSize;
	p.rotation = random.unit() * t.randomRotation * 6.2831853f;
	if (t.mesh) {
		p.angles = {(random.unit() * 2 - 1) * t.randomRotation3.x, (random.unit() * 2 - 1) * t.randomRotation3.y,
			(random.unit() * 2 - 1) * t.randomRotation3.z};
	}
	if (m_particles.size() < kMaxParticlesPerEmitter) {
		m_particles.push_back(p);
	}
}

void Emitter::update(float seconds, game::Random& random)
{
	const ParticleTemplate& t = *m_template;
	m_age += seconds;
	const float active = m_age - t.emitDelay;
	if (!m_stopped && active > 0 && (t.isLooping || active - seconds < t.emitTime)) {
		const float span = t.isLooping ? seconds : std::min(seconds, t.emitTime - std::max(0.0f, active - seconds));
		m_pending += t.emitFrequency * std::max(0.0f, span);
		while (m_pending >= 1.0f) {
			emit(random);
			m_pending -= 1.0f;
		}
	}
	// Gravity pulls down the world's y, in a local system's space too.
	const Vec3 down = t.localSystem ? m_rotation.untransform({0, 1, 0}) : Vec3 {0, 1, 0};
	for (Particle& p : m_particles) {
		const float f = std::min(p.age / p.life, 1.0f);
		p.velocity += down * (t.gravity * t.gravityGraph.at(f) * seconds);
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

void EffectManager::spawnTemplate(const world::ObjectTemplate& t, const Vec3& position, const Mat3& rotation, int depth,
	View view, std::vector<Emitter*>* spawned, std::vector<size_t>* spawnedLights)
{
	if (depth > 4) {
		return;
	}
	const std::string& type = t.typeName();
	if (type == "SpriteParticleSystem" || type == "MeshParticleSystem" || type == "NonScreenAlignedParticleSystem") {
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
			|| childType == "NonScreenAlignedParticleSystem") {
			const ParticleTemplate* particles = particleTemplate(*childTemplate);
			looping = looping || particles->isLooping;
			span = std::max(span, particles->emitDelay + particles->emitTime + particles->timeToLive + particles->randomTimeToLive);
		}
	}
	for (const world::ChildTemplate& child : t.children()) {
		if (const world::ObjectTemplate* childTemplate = m_templates.find(child.name)) {
			const Mat3 local = Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z});
			const Vec3 at = position + rotation.transform({child.position.x, child.position.y, child.position.z});
			if (childTemplate->typeName() == "LightSource") {
				if (number(*childTemplate, "enabled", 1) == 0) {
					continue;
				}
				LiveLight light;
				const float intensity = number(*childTemplate, "intensity", 1);
				light.color = vector(*childTemplate, "color", {1, 1, 1}) * intensity;
				light.range1 = number(*childTemplate, "attenuationRange1", 0);
				light.range2 = std::max(number(*childTemplate, "attenuationRange2", 1), light.range1 + 0.01f);
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

std::vector<PointLight> EffectManager::lights() const
{
	std::vector<PointLight> out;
	for (const LiveLight& light : m_lights) {
		const float fade = light.life < 0 ? 1.0f : std::clamp(1.0f - light.age / light.life, 0.0f, 1.0f);
		if (fade > 0) {
			out.push_back({light.position, light.color * fade, light.range1, light.range2});
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
	// local, then the mount, then the anchor (row vectors: v * L * M * A).
	const Vec3 mounted = emitter.mountPosition + emitter.mountRotation.transform(emitter.localPosition);
	emitter.place(place.position + place.rotation.transform(mounted),
		emitter.localRotation * emitter.mountRotation * place.rotation);
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
	for (const auto& emitter : m_emitters) {
		emitter->update(seconds, m_random);
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
		out.push_back({emitter.worldPosition(p), size, p.rotation, {color.x, color.y, color.z, alpha}});
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

bool holdsBundles(const world::ObjectTemplateManager& templates, const world::ObjectTemplate& t,
	ObjectEffects::BundleCache& cache, int depth = 0)
{
	if (const auto found = cache.find(&t); found != cache.end()) {
		return found->second;
	}
	bool holds = false;
	if (t.typeName() == "EffectBundle") {
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
				if (c->typeName() == "EffectBundle") {
					Placed placed;
					placed.bundle = c;
					placed.path = path;
					placed.chain = chain;
					placed.position = childAt;
					placed.rotation = childTurn;
					placed.startOnEffects = flags(*c, "startOnEffects", "setStartOnEffects");
					placed.stopOnEffects = flags(*c, "stopOnEffects", "setStopOnEffects");
					placed.startAtCreation = number(*c, "startAtCreation", 0) != 0;
					out.push_back(std::move(placed));
				} else {
					walk(*c, childAt, childTurn, depth + 1);
				}
				path.pop_back();
				chain.pop_back();
			}
		};
	walk(root, {}, {}, 0);
	return out;
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
		if (tracked.bundles.empty()) {
			continue;
		}
		effects.setAnchor(object.id, object.position, object.rotation);
		// Turning parts carry their bundles along.
		if (partPose && !created) {
			for (size_t i = 0; i < tracked.bundles.size(); ++i) {
				Placed& b = tracked.bundles[i];
				place(b, object.id, partPose, b.position, b.rotation);
				effects.setMount(object.id, static_cast<uint32_t>(i + 1), b.position, b.rotation);
			}
		}
		for (size_t i = 0; i < tracked.bundles.size(); ++i) {
			const Placed& b = tracked.bundles[i];
			const uint32_t tag = static_cast<uint32_t>(i + 1);
			const bool start = (created && b.startAtCreation)
				|| (object.occupied && !tracked.occupied && (b.startOnEffects & kEntered) != 0);
			if (start && !effects.emitting(object.id, tag)) {
				effects.spawnAttached(b.bundle->name(), object.id, b.position, b.rotation, view, tag);
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

} // namespace dice::hfe::effects
