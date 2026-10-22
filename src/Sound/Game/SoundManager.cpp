#include "SoundManager.h"

#include "SoundObjectTemplate.h"

#include <algorithm>
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

SoundManager::SoundManager(ISoundEngine* engine, world::ObjectTemplateManager& templates)
	: m_engine(engine), m_templates(templates)
{
}

SoundManager::~SoundManager()
{
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
		ISoundTemplate* engineSound = created->engineTemplate();
		if (!engineSound) {
			continue;
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
	const world::ObjectTemplate* objectTemplate = m_templates.find("S_Game_" + name);
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
		m_engine->setVolume(group, m_groupVolumes[static_cast<size_t>(group)]);
	}
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

void SoundManager::update(int ticks)
{
	if (m_engine) {
		m_engine->update(ticks);
	}
}

} // namespace dice::hfe::sound
