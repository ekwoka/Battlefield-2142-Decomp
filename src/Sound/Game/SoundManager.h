// The game side of sound (vtable 0x009bf168, created by 0x007fcd50).
//
// The executable loads BF2142Audio.dll, takes its engine from
// getSoundEngine(), and wraps it in this manager, which keeps named menu
// and game sounds, the listener, tinnitus and volume groups, and serves the
// "Sound.*" console methods. Dedicated servers and "+opt-nosound" get a
// manager without an engine, whose methods do nothing.
#pragma once

#include "Sound/SoundEngine/ISoundEngine.h"
#include "World/ObjectTemplateManager.h"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace dice::hfe::sound {

struct DefaultSound; // a sound a template creates for itself (SoundManager.cpp)

class SoundManager {
public:
	// `engine` may be null.
	SoundManager(ISoundEngine* engine, world::ObjectTemplateManager& templates);
	~SoundManager();

	ISoundEngine* engine() const { return m_engine; }
	world::ObjectTemplateManager& templates() const { return m_templates; }

	// Registers the "Sound" template type so templates of it own engine sounds.
	void registerTemplateType();

	// quality 0..2; returns the engine's result.
	int initialize(int quality, const std::string& provider, int maxVoices);

	// Menu sounds (slot 0x50): a 2D sound from a file. A looping sound plays
	// until stopped; interface sounds use volume group 1, others group 3.
	void addMenuSound(const std::string& name, const std::string& file, float volume = 1, bool loop = false,
		bool interface = false);
	// Game sounds (slot 0x4c): from the template "S_Game_<name>".
	void addSound(const std::string& name);
	void playSound(const std::string& name);
	void stopSound(const std::string& name);
	void fadeOutSound(const std::string& name, int ticks);

	void setProperty(const std::string& name, float value);
	float property(const std::string& name) const;
	void setReverb(const std::string& path);

	// Tinnitus (the ringing after explosions): files are a comma-separated list.
	void setupTinnitus(const std::string& files, float attack, float sustain, float release, float level);
	void startTinnitus();
	void stopTinnitus(bool immediate);

	void setMasterVolume(float volume);
	float masterVolume() const;
	void setDopplerFactor(float factor);
	float dopplerFactor() const;
	void setVolumeForGroup(int group, float volume); // groups 0..7, volume clamped to 0..1
	float volumeForGroup(int group) const;

	bool loadMusic(const std::string& path);
	bool playMusic();
	bool stopMusic(float fadeSeconds);

	// "Sound.tweakTemplate": see SoundObjectTemplate::setTweaks.
	void tweakTemplate(const std::string& name, const std::array<float, 6>& multipliers);
	// Names of all Sound templates, one per line.
	std::string allSoundTemplates() const;

	// Loads the sample data of all sounds created so far (slot 8, after loading).
	void reloadSoundFiles();
	void update(int ticks);

private:
	ISound* findSound(const std::string& name) const;
	void createDefaultSounds(world::ObjectTemplate& owner, const std::vector<DefaultSound>& sounds);

	ISoundEngine* m_engine;
	world::ObjectTemplateManager& m_templates;
	// Sound handles by name: game sounds are indices into m_gameSounds,
	// menu sounds 0x1000000 + an index into m_menuSounds (0x007fac60).
	std::map<std::string, int, world::CaseInsensitiveLess> m_soundIds;
	std::vector<ISound*> m_gameSounds;
	std::vector<ISound*> m_menuSounds;
	std::array<float, 8> m_groupVolumes {1, 1, 1, 1, 1, 1, 1, 1};
	float m_tinnitusAttack = 0.5f;
	float m_tinnitusSustain = 0;
	float m_tinnitusRelease = 0;
	float m_tinnitusLevel = 0;
	std::string m_provider;
};

} // namespace dice::hfe::sound
