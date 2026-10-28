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
	// A trail draws its path as long as its anchor carries it.
	if (t.trail && anchor != 0 && !m_stopped) {
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
	if (t.trail) {
		// A point at emitFrequency while the system moves faster than
		// minTrailVelocity, for as long as it is carried.
		m_speed = m_placed && seconds > 0 ? std::sqrt(physics::lengthSquared(m_position - m_lastPosition)) / seconds : 0.0f;
		m_lastPosition = m_position;
		m_placed = true;
		const bool emitting = t.isLooping || anchor != 0 || active - seconds < t.emitTime;
		if (!m_stopped && active > 0 && emitting && m_speed >= t.minTrailVelocity) {
			m_pending += t.emitFrequency * seconds;
			while (m_pending >= 1.0f) {
				emit(random);
				m_pending -= 1.0f;
			}
		}
	} else if (!m_stopped && active > 0 && (t.isLooping || active - seconds < t.emitTime)) {
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
	for (const world::ChildTemplate& child : t.children()) {
		if (const world::ObjectTemplate* childTemplate = m_templates.find(child.name)) {
			const Mat3 local = Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z});
			const Vec3 at = position + rotation.transform({child.position.x, child.position.y, child.position.z});
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
		const Vec3 color = t.color1 * (1 - blend) + t.color2 * blend;
		const std::array<float, 4> rgba {color.x, color.y, color.z, alpha};
		const float c = std::cos(p.rotation), s = std::sin(p.rotation);
		const Vec3 u = (axisU * c + axisV * s) * size, v = (axisV * c - axisU * s) * size;
		const Vec3 centre = emitter.worldPosition(p);
		const std::array<float, 4> frame = frameOf(t, f);
		const WorldVertex corners[4] = {
			{centre - u - v, frame[0], frame[3], rgba},
			{centre + u - v, frame[2], frame[3], rgba},
			{centre + u + v, frame[2], frame[1], rgba},
			{centre - u + v, frame[0], frame[1], rgba},
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
