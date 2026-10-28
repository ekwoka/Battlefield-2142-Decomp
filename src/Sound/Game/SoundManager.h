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
#include <functional>
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
	// Game sounds (slot 0x4c, 0x007fae20): creates the template
	// "S_Game_<name>" (left active, so the script's ObjectTemplate.* lines
	// that follow set it up, Sound/GameSounds.con) and its sound.
	void addSound(const std::string& name);
	// "Sound.addTrigger" (0x007f95a0): the template becomes an ambient
	// emitter of the engine (slot 0x70), the levels' AmbientObjects.con
	// sounds; at quality level 2 (low) only the first one.
	void addTrigger(const std::string& templateName);
	// The engine template of a Sound template, by name (null for none, or
	// without an engine).
	ISoundTemplate* soundTemplate(const std::string& templateName) const;
	// Whether there is a Sound template of that name with files to play
	// (with or without an engine).
	bool hasSound(const std::string& templateName) const;
	void playSound(const std::string& name);
	// The same at a point of the world (a 3-D sound's position).
	void playSoundAt(const std::string& name, float x, float y, float z);
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
	// Muting (slot 0xbc, 0x007f9af0): reason 3 while a voice-over plays
	// (0x004cdfb0), 4 while a first-person weapon whose sound.muteOnFire
	// is set fires (0x005dbdc0, until 0x005db6c0). Each update
	// (0x007f9f50) moves every group's volume a step towards its target:
	// group 0 (the world's effects) at 0.125 under a voice-over, and x
	// sound-mute-level-1p (0.4) while firing; group 4 (first-person
	// weapons) only under the voice-over. Reason 4 also sets group 0's
	// steps to sound-mute-fadein-1p / -fadeout-1p, the others to the -vo
	// ones.
	void setMuted(int reason, bool muted);
	bool muted(int reason) const { return reason >= 0 && reason < 5 && m_mute[static_cast<size_t>(reason)]; }
	// A group's volume as the ducking has it now (0..1, before the user's).
	float duckedVolume(int group) const;

	bool loadMusic(const std::string& path);
	bool playMusic();
	bool stopMusic(float fadeSeconds);

	// "Sound.tweakTemplate": see SoundObjectTemplate::setTweaks.
	void tweakTemplate(const std::string& name, const std::array<float, 6>& multipliers);
	// Names of all Sound templates, one per line.
	std::string allSoundTemplates() const;

	// Loads the sample data of all sounds created so far (slot 8, after loading).
	void reloadSoundFiles();
	// The camera, as the manager's update (0x007f9d50) is handed it each
	// frame: the engine keeps pointers to the copies (slots 11, 13, 16, 18).
	void setListener(const Vec3& position, const Vec3& forward, const Vec3& up, const Vec3& velocity,
		bool insideTitan = false);
	const Vec3& listenerPosition() const { return m_listenerPosition; }
	// The level's ray test for the indoor level (true, with the distance
	// along the segment, when it hits something).
	using RayTest = std::function<bool(const Vec3& from, const Vec3& to, float& distance)>;
	void setRayTest(RayTest test) { m_rayTest = std::move(test); }
	// 0x007fa100: how enclosed a point is, 0 in the open. Two rays up
	// (from +-0.4 x, +-0.3 z) and one down, sound-raylength (15) long;
	// all three hitting, the room's height h (the floor's depth plus the
	// ceilings' mean, at most 15) gives sound-maxreverb (0.5) x h /
	// (sound-reverb-steepness (0.98) x h + 0.02 x 15).
	float indoorLevel(const Vec3& at) const;
	// 0x007fa320 (slot 0x98): no ceiling and floor around.
	bool outdoors(const Vec3& at) const { return indoorLevel(at) == 0.0f; }
	// Once a frame (0x007f9d50): the muting's fades (0x007f9f50), the
	// listener's indoor level (engine slot 20) and the engine's update.
	void update(int ticks);

private:
	ISound* findSound(const std::string& name) const;
	void createDefaultSounds(world::ObjectTemplate& owner, const std::vector<DefaultSound>& sounds);
	void updateDucking();

	ISoundEngine* m_engine;
	world::ObjectTemplateManager& m_templates;
	// Sound handles by name: game sounds are indices into m_gameSounds,
	// menu sounds 0x1000000 + an index into m_menuSounds (0x007fac60).
	std::map<std::string, int, world::CaseInsensitiveLess> m_soundIds;
	std::vector<ISound*> m_gameSounds;
	std::vector<ISound*> m_menuSounds;
	std::array<float, 8> m_groupVolumes {1, 1, 1, 1, 1, 1, 1, 1};
	// The per-group fade (+0xe0 + group x 0x14, set up by the constructor
	// 0x007f9040): current, target and the rising and falling steps per
	// update (1/15 at first).
	struct Duck {
		float current = 1, target = 1, riseStep = 0.0667f, fallStep = 0.0667f;
	};
	std::array<Duck, 8> m_duck {};
	std::array<bool, 5> m_mute {};
	Vec3 m_listenerPosition;
	Vec3 m_listenerForward {0, 0, 1};
	Vec3 m_listenerUp {0, 1, 0};
	Vec3 m_listenerVelocity;
	bool m_listenerSet = false;
	RayTest m_rayTest;
	Vec3 m_indoorAt {1e30f, 0, 0}; // where the indoor level was last measured
	float m_indoorLevel = 0;
	int m_indoorAge = 0;
	float m_tinnitusAttack = 0.5f;
	float m_tinnitusSustain = 0;
	float m_tinnitusRelease = 0;
	float m_tinnitusLevel = 0;
	std::string m_provider;
};

} // namespace dice::hfe::sound
