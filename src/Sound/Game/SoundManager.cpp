#include "SoundManager.h"

#include "SoundObjectTemplate.h"

#include "Console/Vars.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace dice::hfe::sound {

namespace {

constexpr int kMenuSoundBase = 0x1000000;

// Sounds some templates create for themselves, named
// "S_<template>_<event>" and added as children. Template classes do this
// when created (their vtable slot 6) and sound components when added with
// createComponent (component slot 9). Scripts then configure them with
// "ObjectTemplate.activeSafe Sound S_<template>_<event>".
} // namespace

struct DefaultSound {
	std::string event;
	bool is3d;
	int loopCount;           // 0 loops forever
	float minDistance = -1;  // < 0: engine default
	int stopType = -1;       // < 0: engine default
	int volumeGroup = -1;    // < 0: engine default
	float reverbLevel = -1;  // < 0: engine default
	bool generatedFlag = false; // also set template flag 0x400 (soldier sounds)
};

namespace {

// 0x00562d00 (Camera, RotationalBundle, FloatingBundle, Wing, LandingGear, Engine)
const DefaultSound kRotationRpm {"RotationRpm", true, 0, 0.5f};

std::vector<DefaultSound> classSounds(uint32_t classId)
{
	switch (classId) {
	case 0xc4c2: // PlayerControlObject (0x005c6dc0)
	case 0x9c46: // LadderContainer
	case 0xc5a8: // Parachute
		return {{"Ambient", false, 0}, {"AmbientDamaged", false, 0}};
	case 0x9474: // EffectBundle (0x006188d0)
		return {{"Start", true, 1}};
	case 0x9476: // Engine (0x00628070 -> 0x00562d00, 0x007fdd10)
		return {kRotationRpm, {"Idle", true, 0, 3, 2}, {"Rpm1", true, 0, 3, 0}, {"Rpm2", true, 0, 3, 0},
			{"Load", true, 1, 1, 1}};
	case 0x9481: // Spring (0x00629af0 -> 0x007fe820)
		return {{"Rotation", true, 0}};
	case 0x9472: // Camera
	case 0x947d: // RotationalBundle
	case 0x9478: // FloatingBundle
	case 0x9483: // Wing
	case 0x94ae: // LandingGear
		return {kRotationRpm};
	case 0x9493: { // Soldier (0x007fd4b0)
		std::vector<DefaultSound> sounds;
		for (const char* event : {"Death", "Revive", "Swim", "Swim_Still", "Swim_Sprint", "Crouch"}) {
			DefaultSound sound {event, true, 1};
			sound.generatedFlag = true;
			sounds.push_back(sound);
		}
		return sounds;
	}
	default:
		return {};
	}
}

// Weapon sounds (0x005db760). `flags` & 2 drops the outdoor variant, & 1
// the click sounds; both component kinds pass 0.
std::vector<DefaultSound> weaponSounds(int flags)
{
	std::vector<DefaultSound> sounds;
	DefaultSound fire1p {"Fire1P", false, 0};
	fire1p.volumeGroup = 4;
	sounds.push_back(fire1p);
	if (!(flags & 2)) {
		DefaultSound outdoor = fire1p;
		outdoor.event = "Fire1P_Outdoor";
		sounds.push_back(outdoor);
	}
	sounds.push_back({"Fire3P", true, 0, 2});
	if (!(flags & 1)) {
		DefaultSound bolt {"BoltClick", false, 1};
		bolt.reverbLevel = 0.5f;
		DefaultSound trigger {"TriggerClick", false, 1};
		trigger.reverbLevel = 0.3f;
		DefaultSound fireRate {"SwitchFireRate", false, 1};
		fireRate.reverbLevel = 0.3f;
		sounds.insert(sounds.end(), {bolt, trigger, fireRate});
	}
	DefaultSound reload1p {"Reload1P", false, 1};
	reload1p.volumeGroup = 4;
	sounds.push_back(reload1p);
	sounds.push_back({"Reload3P", true, 1, 0.5f});
	sounds.push_back({"Deploy1P", false, 1, -1, 0});
	sounds.push_back({"Deploy3P", true, 1, 0.5f, 0});
	sounds.push_back({"Zoom", false, 1});
	return sounds;
}

void applyDefaults(SoundObjectTemplate& created, const DefaultSound& sound)
{
	ISoundTemplate* engineSound = created.engineTemplate();
	if (!engineSound) {
		return;
	}
	engineSound->setIs3dSound(sound.is3d);
	engineSound->setLoopCount(sound.loopCount);
	if (sound.minDistance >= 0) {
		engineSound->setMinDistance(sound.minDistance);
	}
	if (sound.stopType >= 0) {
		engineSound->setStopType(sound.stopType);
	}
	if (sound.volumeGroup >= 0) {
		engineSound->setVolumeGroup(sound.volumeGroup);
	}
	if (sound.reverbLevel >= 0) {
		engineSound->setReverbLevel(sound.reverbLevel);
	}
}

std::vector<DefaultSound> componentSounds(const std::string& component)
{
	if (component == "DefaultSoundComp") { // 0x005dc350
		return weaponSounds(0);
	}
	if (component == "DefaultProjSoundComp") { // 0x005eaa40 -> 0x005ea930
		return {{"Looping", true, 0, 0.5f}};
	}
	if (component == "CountdownSoundComp") { // 0x005dc760
		std::vector<DefaultSound> sounds = weaponSounds(0);
		for (int team = 1; team < 3; ++team) {
			for (int index = 1; index < 6; ++index) {
				sounds.push_back(
					{"ReloadTimeTeam" + std::to_string(team) + "_" + std::to_string(index), true, 1, 0.0f});
			}
		}
		return sounds;
	}
	return {};
}

constexpr uint32_t kGeneratedSoundFlag = 0x400; // set on sounds created this way (0x005f6750)

std::vector<std::string> splitList(const std::string& list)
{
	std::vector<std::string> items;
	std::istringstream in(list);
	for (std::string item; std::getline(in, item, ',');) {
		items.push_back(item);
	}
	return items;
}

} // namespace

// The manager's ISoundOcclusion (+0x10, vtable 0x009bf138): each 3D
// sound's owner in parameter 0 and the last clear ray in parameter 1, as
// 0x007f9340 sets them up.
class SoundManager::Occlusion final : public ISoundOcclusion {
public:
	static constexpr int kRayBudget = 48;
	explicit Occlusion(SoundManager& manager) : m_manager(manager) {}
	float occlusion(ISound* sound, const Vec3* position) override
	{
		if (!sound || !position) {
			return 0.0f;
		}
		// The answer kept in parameter 2 (value x 65535, frames left in the
		// top 16 bits), as sound-obstruction-opt1 does. The rewrite always
		// keeps it, for round(e^(1 + dist / 100)) frames but at least 10
		// and at most 30, and casts at most kRayBudget rays an update
		// (the others keep their last answer): its line test is far
		// slower than the exe's.
		const unsigned cached = sound->parameter(2);
		const bool budget = m_manager.m_raysThisUpdate < kRayBudget;
		if (cached != 0 && ((cached >> 16) != 0 || !budget)) {
			if ((cached >> 16) != 0) {
				sound->setParameter(2, (((cached >> 16) - 1) << 16) | (cached & 0xffff));
			}
			return static_cast<float>(cached & 0xffff) * 1.5259022e-05f;
		}
		if (!budget) {
			return 0.0f;
		}
		ISoundTemplate* t = sound->soundTemplate();
		int last = static_cast<int>(sound->parameter(1));
		const float value = m_manager.obstruction(*position, t ? t->soundRadius() : 0.0f, sound->parameter(0), &last);
		sound->setParameter(1, static_cast<unsigned>(last));
		const float dist = std::sqrt((*position - m_manager.m_listenerPosition).lengthSquared());
		const unsigned frames = static_cast<unsigned>(std::clamp(std::lround(std::exp(1 + std::min(dist, 1000.0f) * 0.01f)), 10L, 30L));
		sound->setParameter(2, (frames << 16) | (static_cast<unsigned>(std::lround(value * 65535.0f)) & 0xffff) | (value == 0 ? 1u : 0u));
		return value;
	}

private:
	SoundManager& m_manager;
};

SoundManager::SoundManager(ISoundEngine* engine, world::ObjectTemplateManager& templates)
	: m_engine(engine), m_templates(templates), m_occlusion(std::make_unique<Occlusion>(*this))
{
}

SoundManager::~SoundManager()
{
	setInteriorAmbient(nullptr, 0);
	if (m_wind) {
		m_wind->stopImmediately();
		m_wind->setVolumeEnvelope(nullptr, nullptr);
		m_wind->setPitchEnvelope(nullptr, nullptr);
		m_wind->release();
	}
	for (ISound* sound : m_gameSounds) {
		if (sound) {
			sound->release();
		}
	}
	for (ISound* sound : m_menuSounds) {
		if (sound) {
			sound->release();
		}
	}
}

void SoundManager::registerTemplateType()
{
	ISoundEngine* engine = m_engine;
	m_templates.registerType(SoundObjectTemplate::kClassId, "Sound",
		[engine](uint32_t classId, const std::string& typeName, const std::string& name, uint32_t id) {
			return std::make_unique<SoundObjectTemplate>(engine, classId, typeName, name, id);
		});
	m_templates.addCreationListener([this](world::ObjectTemplate& created) {
		createDefaultSounds(created, classSounds(created.classId()));
	});
	m_templates.addComponentListener([this](world::ObjectTemplate& owner, const std::string& component) {
		createDefaultSounds(owner, componentSounds(component));
	});
}

// 0x0060e910: "S_<owner>_<event>" is created (or reused) as a Sound template
// flagged 0x400 and added to the owner's children.
void SoundManager::createDefaultSounds(world::ObjectTemplate& owner, const std::vector<DefaultSound>& sounds)
{
	const std::string ownerName = owner.name();
	for (const DefaultSound& sound : sounds) {
		const std::string name = "S_" + ownerName + "_" + sound.event;
		auto* created = dynamic_cast<SoundObjectTemplate*>(m_templates.create(SoundObjectTemplate::kClassId, name));
		if (!created) {
			continue;
		}
		created->setFlags(created->flags() | kGeneratedSoundFlag);
		if (world::ObjectTemplate* parent = m_templates.find(ownerName)) {
			parent->children().push_back({name, {}, {}});
		}
		applyDefaults(*created, sound);
	}
}

void SoundManager::applyEventDefaults(world::ObjectTemplate& sound)
{
	auto* created = dynamic_cast<SoundObjectTemplate*>(&sound);
	for (const DefaultSound& event : weaponSounds(0)) {
		const std::string suffix = "_" + event.event;
		const std::string& name = sound.name();
		if (created && name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
			applyDefaults(*created, event);
			return;
		}
	}
}

int SoundManager::initialize(int quality, const std::string& provider, int maxVoices)
{
	m_provider = provider;
	if (!m_engine) {
		return 0;
	}
	return m_engine->initialize(std::clamp(quality, 0, 2), m_provider.c_str(), maxVoices);
}

void SoundManager::addMenuSound(const std::string& name, const std::string& file, float volume, bool loop,
	bool interface)
{
	if (!m_engine) {
		return;
	}
	ISoundTemplate* sound = m_engine->createTemplate();
	if (!sound) {
		return;
	}
	const std::vector<std::string> files = splitList(file);
	std::vector<const char*> pointers;
	for (const std::string& f : files) {
		pointers.push_back(f.c_str());
	}
	sound->setSoundFilenames(pointers.data(), static_cast<int>(pointers.size()));
	sound->setVolume(volume);
	sound->setIs3dSound(false);
	sound->setLoopCount(loop ? 0 : 1);
	sound->setStopType(0);
	sound->setFlags(sound->flags() | 1);
	sound->markSoundFilesUsed();
	sound->setVolumeGroup(interface ? 1 : 3);
	ISound* instance = sound->createSound();
	sound->release();

	const auto it = m_soundIds.find(name);
	if (it == m_soundIds.end()) {
		m_soundIds[name] = kMenuSoundBase + static_cast<int>(m_menuSounds.size());
		m_menuSounds.push_back(instance);
		return;
	}
	// Re-adding a name replaces the sound (it must have been a menu sound).
	ISound*& slot = m_menuSounds[static_cast<size_t>(it->second - kMenuSoundBase)];
	if (slot) {
		slot->release();
	}
	slot = instance;
}

void SoundManager::addSound(const std::string& name)
{
	ISound* instance = nullptr;
	world::ObjectTemplate* objectTemplate = m_templates.create(SoundObjectTemplate::kClassId, "S_Game_" + name);
	if (const auto* sound = dynamic_cast<const SoundObjectTemplate*>(objectTemplate)) {
		if (sound->engineTemplate()) {
			instance = sound->engineTemplate()->createSound();
		}
	}
	const auto it = m_soundIds.find(name);
	if (it == m_soundIds.end()) {
		m_soundIds[name] = static_cast<int>(m_gameSounds.size());
		m_gameSounds.push_back(instance);
		return;
	}
	ISound*& slot = m_gameSounds[static_cast<size_t>(it->second)];
	if (slot) {
		slot->release();
	}
	slot = instance;
}

ISoundTemplate* SoundManager::soundTemplate(const std::string& templateName) const
{
	const auto* sound = dynamic_cast<const SoundObjectTemplate*>(m_templates.find(templateName));
	return sound ? sound->engineTemplate() : nullptr;
}

bool SoundManager::hasSound(const std::string& templateName) const
{
	const auto* sound = dynamic_cast<const SoundObjectTemplate*>(m_templates.find(templateName));
	return sound && sound->hasFiles();
}

void SoundManager::addTrigger(const std::string& templateName)
{
	if (!m_engine) {
		return;
	}
	if (m_engine->qualityLevel() == 2 && m_engine->ambientEmitterCount() > 0) {
		return;
	}
	if (ISoundTemplate* sound = soundTemplate(templateName)) {
		sound->markSoundFilesUsed();
		m_engine->addAmbientEmitter(sound);
	}
}

ISound* SoundManager::findSound(const std::string& name) const
{
	const auto it = m_soundIds.find(name);
	if (it == m_soundIds.end()) {
		return nullptr;
	}
	const int id = it->second;
	if (id >= kMenuSoundBase) {
		const size_t index = static_cast<size_t>(id - kMenuSoundBase);
		return index < m_menuSounds.size() ? m_menuSounds[index] : nullptr;
	}
	return static_cast<size_t>(id) < m_gameSounds.size() ? m_gameSounds[static_cast<size_t>(id)] : nullptr;
}

void SoundManager::playSound(const std::string& name)
{
	if (ISound* sound = findSound(name)) {
		sound->play();
	}
}

void SoundManager::playSoundAt(const std::string& name, float x, float y, float z)
{
	if (ISound* sound = findSound(name)) {
		const Vec3 at {x, y, z};
		sound->playAt(&at);
	}
}

void SoundManager::stopSound(const std::string& name)
{
	if (ISound* sound = findSound(name)) {
		sound->stop();
	}
}

void SoundManager::fadeOutSound(const std::string& name, int ticks)
{
	ISound* sound = findSound(name);
	if (!sound) {
		return;
	}
	if (m_engine && ticks > 0) {
		m_engine->registerFadingSound(sound);
	}
	sound->fadeOut(ticks);
}

void SoundManager::setProperty(const std::string& name, float value)
{
	if (m_engine) {
		m_engine->setTweak(name.c_str(), value);
	}
}

float SoundManager::property(const std::string& name) const
{
	return m_engine ? m_engine->tweak(name.c_str()) : 0.0f;
}

void SoundManager::setReverb(const std::string& path)
{
	if (m_engine) {
		m_engine->loadReverbPreset(path.c_str());
	}
}

void SoundManager::setupTinnitus(const std::string& files, float attack, float sustain, float release, float level)
{
	if (!m_engine) {
		return;
	}
	const std::vector<std::string> names = splitList(files);
	std::vector<const char*> pointers;
	for (const std::string& file : names) {
		pointers.push_back(file.c_str());
	}
	m_engine->setupTinnitus(pointers.data(), static_cast<int>(pointers.size()));
	m_tinnitusAttack = attack;
	m_tinnitusSustain = sustain;
	m_tinnitusRelease = release;
	m_tinnitusLevel = level;
}

void SoundManager::startTinnitus()
{
	if (m_engine) {
		m_engine->startTinnitus(m_tinnitusAttack, m_tinnitusSustain, m_tinnitusRelease, m_tinnitusLevel, 0);
	}
}

void SoundManager::stopTinnitus(bool immediate)
{
	if (m_engine) {
		m_engine->stopTinnitus(immediate);
	}
}

void SoundManager::setMasterVolume(float volume)
{
	if (m_engine) {
		m_engine->setMasterVolume(volume);
	}
}

float SoundManager::masterVolume() const
{
	return m_engine ? m_engine->masterVolume() : 1.0f;
}

void SoundManager::setDopplerFactor(float factor)
{
	if (m_engine) {
		m_engine->setDopplerFactor(factor);
	}
}

float SoundManager::dopplerFactor() const
{
	return m_engine ? m_engine->dopplerFactor() : 1.0f;
}

void SoundManager::setVolumeForGroup(int group, float volume)
{
	if (group < 0 || group >= static_cast<int>(m_groupVolumes.size())) {
		return;
	}
	m_groupVolumes[static_cast<size_t>(group)] = std::clamp(volume, 0.0f, 1.0f);
	if (m_engine) {
		m_engine->setVolume(group, m_groupVolumes[static_cast<size_t>(group)] * m_duck[static_cast<size_t>(group)].current);
	}
}

// 0x007f9af0. The steps are the tweaks sound-mute-fadein-1p / -fadeout-1p
// (0x3eaa7efa, 0x3e2ab368) for reason 4, sound-mute-fadein-vo / -fadeout-vo
// (0x3d889a02) otherwise (0x008f0000's tweak table).
void SoundManager::setMuted(int reason, bool muted)
{
	if (reason < 0 || reason >= static_cast<int>(m_mute.size())) {
		return;
	}
	m_mute[static_cast<size_t>(reason)] = muted;
	Duck& effects = m_duck[0];
	const Vars& vars = Vars::global();
	if (reason == 4) {
		effects.riseStep = vars.number("sound-mute-fadein-1p", 0.333f);
		effects.fallStep = vars.number("sound-mute-fadeout-1p", 0.1667f);
	} else {
		effects.riseStep = vars.number("sound-mute-fadein-vo", 0.0667f);
		effects.fallStep = vars.number("sound-mute-fadeout-vo", 0.0667f);
	}
}

float SoundManager::duckedVolume(int group) const
{
	return group >= 0 && group < static_cast<int>(m_duck.size()) ? m_duck[static_cast<size_t>(group)].current : 1.0f;
}

// 0x007f9f50, once an update: the targets from the reasons (reason 0 a
// one-update pulse at 0.25, 1 all of groups 0-3 silent, 2 groups 0 and 2
// silent, 3 a voice-over: group 0 at 0.125, 4 firing: group 0 x 0.4 on
// top), every group a step towards its target, the engine's group volume
// the user's x that.
void SoundManager::updateDucking()
{
	constexpr float kPulse = 0.25f;   // +0x188
	constexpr float kVoice = 0.125f;  // +0x194
	const float kFiring = Vars::global().number("sound-mute-level-1p", 0.4f); // DAT_00b44f14
	const bool off = m_mute[1];
	const bool off02 = m_mute[2] || off;
	const float pulse = m_mute[0] ? kPulse : 1.0f;
	float effects = off02 ? 0.0f : m_mute[3] ? kVoice : pulse;
	m_duck[1].target = off ? 0.0f : pulse;
	m_duck[2].target = off02 ? 0.0f : pulse;
	m_duck[3].target = off ? 0.0f : pulse;
	m_duck[4].target = effects;
	if (m_mute[4]) {
		effects *= kFiring;
	}
	m_duck[0].target = effects;
	for (size_t group = 0; group < m_duck.size(); ++group) {
		Duck& d = m_duck[group];
		if (d.current < d.target) {
			d.current = std::min(d.current + d.riseStep, d.target);
		} else if (d.current > d.target) {
			d.current = std::max(d.current - d.fallStep, d.target);
		}
		if (m_engine) {
			m_engine->setVolume(static_cast<int>(group), m_groupVolumes[group] * d.current);
		}
	}
	m_mute[0] = false;
	m_mute[3] = false;
}

float SoundManager::volumeForGroup(int group) const
{
	if (group < 0 || group >= static_cast<int>(m_groupVolumes.size())) {
		return 1.0f;
	}
	return m_groupVolumes[static_cast<size_t>(group)];
}

bool SoundManager::loadMusic(const std::string& path)
{
	if (!m_engine) {
		return false;
	}
	m_engine->setMusic(path.c_str());
	return true;
}

bool SoundManager::playMusic()
{
	if (!m_engine) {
		return false;
	}
	m_engine->startMusic();
	return true;
}

bool SoundManager::stopMusic(float fadeSeconds)
{
	if (!m_engine) {
		return false;
	}
	m_engine->stopMusic(fadeSeconds);
	return true;
}

void SoundManager::tweakTemplate(const std::string& name, const std::array<float, 6>& multipliers)
{
	if (auto* sound = dynamic_cast<SoundObjectTemplate*>(m_templates.find(name))) {
		sound->setTweaks(multipliers);
	}
}

std::string SoundManager::allSoundTemplates() const
{
	std::string names;
	for (const auto& objectTemplate : m_templates.templates()) {
		if (objectTemplate->classId() == SoundObjectTemplate::kClassId) {
			names += objectTemplate->name() + "\n";
		}
	}
	return names;
}

void SoundManager::reloadSoundFiles()
{
	if (m_engine) {
		m_engine->reloadSoundFiles();
	}
}

void SoundManager::setListener(const Vec3& position, const Vec3& forward, const Vec3& up, const Vec3& velocity,
	bool insideTitan)
{
	m_listenerPosition = position;
	m_listenerForward = forward;
	m_listenerUp = up;
	m_listenerVelocity = velocity;
	if (m_engine && !m_listenerSet) {
		m_engine->setListenerPosition(&m_listenerPosition);
		m_engine->setListenerOrientation(&m_listenerForward, &m_listenerUp);
		m_engine->setListenerVelocity(&m_listenerVelocity);
	}
	m_listenerSet = true;
	m_insideTitan = insideTitan;
	if (m_engine) {
		m_engine->setListenerInsideTitan(insideTitan);
	}
}

float SoundManager::obstruction(const Vec3& at, float radius, uint32_t owner, int* lastClear) const
{
	const Vars& vars = Vars::global();
	if (!m_obstructionTest || vars.number("sound-obstruction-disable", 0) != 0) {
		return 0.0f;
	}
	const float listenerRadius = vars.number("sound-listener-radius", 0.2f);
	const Vec3 d = at - m_listenerPosition;
	const float r = radius > 0 ? radius : 0.05f;
	const float length = std::sqrt(d.lengthSquared());
	const float dist = length - r - listenerRadius;
	if (dist <= 0 || length <= 0) {
		return 0.0f;
	}
	const Vec3 n = d * (1.0f / length);
	const auto cross = [](const Vec3& a, const Vec3& b) {
		return Vec3 {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
	};
	const auto unit = [](const Vec3& v) {
		const float l = std::sqrt(v.lengthSquared());
		return l > 0 ? v * (1.0f / l) : v;
	};
	const Vec3 up = std::fabs(n.y) >= 0.99f ? Vec3 {1, 0, 0} : Vec3 {0, 1, 0};
	const Vec3 a = unit(cross(up, n));
	const Vec3 b = unit(cross(n, a));
	const Vec3 from[3] = {m_listenerPosition + n * listenerRadius, m_listenerPosition + a * listenerRadius,
		m_listenerPosition + b * listenerRadius};
	const Vec3 to[3] = {at - n * r, at + a * r, at + b * r};
	// Three rays within 40 m, one beyond; sound-obstruction-opt0 starts
	// with the one last found clear.
	const int rays = dist <= 40.0f ? 3 : 1;
	const bool opt0 = vars.number("sound-obstruction-opt0", 0) != 0;
	const int first = rays > 1 && opt0 && lastClear ? std::max(0, *lastClear) : 0;
	const uint32_t ignore = m_listenerRootLarge ? 0 : m_listenerRoot;
	RayHit hits[3];
	int tried = first;
	for (; tried < first + rays; ++tried) {
		const int ray = tried % rays;
		++m_raysThisUpdate;
		if (!m_obstructionTest(from[ray], to[ray], ignore, hits[ray])) {
			if (lastClear) {
				*lastClear = ray;
			}
			return 0.0f;
		}
	}
	// Every ray blocked. The first meeting the sound's own (small) root:
	// nothing in the way.
	if (owner != 0 && hits[0].root == owner && !hits[0].large) {
		return 0.0f;
	}
	const float h = std::sqrt((to[0] - from[0]).lengthSquared()) * hits[0].fraction;
	constexpr float kInvPi = 0.31830987f;
	const auto shape = [&](float rad, float max) {
		const float value = (1.0f - (std::atan((dist - h) / rad) + std::atan(h / rad)) * kInvPi) * max;
		return std::clamp(value, 0.0f, 1.0f);
	};
	const float obstructionRadius = vars.number("sound-obstruction-radius", 2.0f);
	const float obstructionMax = vars.number("sound-obstruction-max", 0.8f);
	const bool inside = m_insideTitan || vars.number("sound-inside-titan", 0) != 0;
	const float inner = vars.number("sound-obstruction-titan-inner", 8.0f);
	const float outer = vars.number("sound-obstruction-titan-outer", 55.0f);
	if (!inside || dist < inner) {
		return shape(obstructionRadius, obstructionMax);
	}
	if (dist >= outer) {
		return 1.0f;
	}
	const float near = shape(obstructionRadius, obstructionMax);
	const float far = shape(vars.number("sound-obstruction-radius-titan", 500.0f),
		vars.number("sound-obstruction-max-titan", 0.99f));
	float w = 1.0f - std::clamp((dist - inner) / (outer - inner), 0.0f, 1.0f);
	for (int i = static_cast<int>(vars.number("sound-obstruction-titan-exp", 0)); i > 0; --i) {
		w *= w;
	}
	return w * near + (1.0f - w) * far;
}

ISoundOcclusion* SoundManager::occlusionHook() { return m_occlusion.get(); }

void SoundManager::windSetup(const std::string& templateName, float speed)
{
	// 0x007f9840: the speed at least 1; a sound of the template replaces
	// the last, its envelopes read at the wind's volume.
	ISoundTemplate* t = soundTemplate(templateName);
	if (!t) {
		return;
	}
	if (m_wind) {
		m_wind->stopImmediately();
		m_wind->setVolumeEnvelope(nullptr, nullptr);
		m_wind->setPitchEnvelope(nullptr, nullptr);
		m_wind->release();
		m_wind = nullptr;
	}
	m_wind = t->createSound();
	m_windInput = 0;
	if (m_wind) {
		m_wind->setVolumeEnvelope(nullptr, &m_windInput);
		m_wind->setPitchEnvelope(nullptr, &m_windInput);
	}
	m_windSpeed = std::max(speed, 1.0f);
	m_windWanted = false;
	m_windPlaying = false;
}

void SoundManager::startWind()
{
	// 0x007f9900: not at the lowest quality.
	if (!m_engine || m_engine->qualityLevel() != 2) {
		m_windWanted = true;
	}
}

void SoundManager::updateWind()
{
	// 0x007f9ea0 (with no wind sound set up, nothing).
	if (!m_wind) {
		return;
	}
	m_windInput = std::clamp(std::sqrt(m_listenerVelocity.lengthSquared()) / m_windSpeed, 0.0f, 1.0f);
	if (!m_windAllowed) {
		if (m_windPlaying) {
			m_wind->stop();
		}
		m_windPlaying = false;
	} else if (m_windWanted != m_windPlaying) {
		if (m_windWanted) {
			m_wind->play();
		} else {
			m_wind->stop();
		}
		m_windPlaying = m_windWanted;
	}
}

void SoundManager::setInteriorAmbient(ISound* sound, float listenerObstruction)
{
	if (sound == m_interiorAmbient && listenerObstruction == m_listenerObstruction) {
		return;
	}
	m_interiorAmbient = sound;
	m_listenerObstruction = listenerObstruction;
	if (m_engine) {
		m_engine->setTweak("listener-3d-obstruction", listenerObstruction);
		m_engine->setGlobalAmbient(sound);
	}
}

float SoundManager::indoorLevel(const Vec3& at) const
{
	if (!m_rayTest) {
		return 0.0f;
	}
	// The settings as 0x007fa100 reads them each time (Settings/Sound.con
	// sets 17, 0 and 0.55: retail's listener is never "indoors").
	const Vars& vars = Vars::global();
	const float kRayLength = vars.number("sound-raylength", 15.0f);
	const float kMaxReverb = std::clamp(vars.number("sound-maxreverb", 0.5f), 0.0f, 1.0f);
	const float kSteepness = std::clamp(vars.number("sound-reverb-steepness", 0.98f), 0.0f, 1.0f);
	if (kRayLength <= 0) {
		return 0.0f;
	}
	float up1 = 0, up2 = 0, down = 0;
	const Vec3 a {at.x + 0.4f, at.y, at.z + 0.3f};
	const Vec3 b {at.x - 0.4f, at.y, at.z - 0.3f};
	if (!m_rayTest(a, a + Vec3 {0, kRayLength, 0}, up1) || !m_rayTest(b, b + Vec3 {0, kRayLength, 0}, up2)
		|| !m_rayTest(at, at - Vec3 {0, kRayLength, 0}, down)) {
		return 0.0f;
	}
	const float height = std::clamp(down + (up1 + up2) * 0.5f, 0.0f, kRayLength);
	return kMaxReverb * height / (kSteepness * height + (1.0f - kSteepness) * kRayLength);
}

void SoundManager::update(int ticks)
{
	m_raysThisUpdate = 0;
	updateWind();
	updateDucking();
	if (m_engine && m_listenerSet && m_rayTest) {
		// The rays again once the listener has moved half a metre, or every
		// 15 frames (the original casts them each frame).
		if ((m_listenerPosition - m_indoorAt).lengthSquared() > 0.25f || ++m_indoorAge >= 15) {
			m_indoorAt = m_listenerPosition;
			m_indoorAge = 0;
			m_indoorLevel = indoorLevel(m_listenerPosition);
			m_engine->setIndoorLevel(m_indoorLevel);
		}
	}
	if (m_engine) {
		m_engine->update(ticks);
	}
}

} // namespace dice::hfe::sound
