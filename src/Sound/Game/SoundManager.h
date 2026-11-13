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
#include <memory>
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
	// Once a frame (0x007f9d50): the wind (0x007f9ea0), the muting's fades
	// (0x007f9f50), the listener's indoor level (engine slot 20) and the
	// engine's update.
	void update(int ticks);

	// --- Obstruction (the manager's ISoundOcclusion at +0x10, vtable
	// 0x009bf138 -> 0x007fa350). Every 3D sound the manager makes (slot 7,
	// 0x007f9340: parameter 0 its owner, 1 and 2 cleared) asks it each
	// engine update how obstructed it is from the listener:
	//   - nothing with sound-obstruction-disable set;
	//   - d = the sound less the listener; its template's soundRadius r
	//     (0.05 when not over 0); dist = |d| - r - sound-listener-radius
	//     (0.2); nothing when dist <= 0;
	//   - up to three segments (one beyond 40 m): from the listener pushed
	//     out by the listener radius along d's direction n to the sound
	//     pulled back by r, and the same from the listener and the sound
	//     both moved sideways along a = n x (0,1,0) (x when n is nearly
	//     vertical) and b = n x a, by the listener radius and r. The first
	//     segment that meets nothing (sound-obstruction-opt0 starts with the
	//     last clear one, parameter 1) gives 0;
	//   - all blocked: nothing when the first meets the sound's owner's
	//     root and that is under 50 m across; else, h the first segment's
	//     distance to its hit and R sound-obstruction-radius (2), m
	//     sound-obstruction-max (0.8):
	//       o(R, m) = clamp((1 - (atan((dist - h) / R) + atan(h / R)) / pi) x m)
	//     o(R, m) outside a Titan or nearer than sound-obstruction-titan-inner
	//     (8); inside one (the player's slot 0x390 or sound-inside-titan),
	//     1 from sound-obstruction-titan-outer (55) on, between them
	//     w o(2, 0.8) + (1 - w) o(sound-obstruction-radius-titan 500,
	//     sound-obstruction-max-titan 0.99) with w = (1 - t)^(2^exp), t the
	//     place between inner and outer, exp sound-obstruction-titan-exp (0);
	//   - sound-obstruction-opt1 keeps the answer in parameter 2 (value x
	//     65535 and the frames left in the top 16 bits) for round(e^(1 +
	//     dist / 100)) + sound-obstruction-reuse-frames frames, at most
	//     sound-obstruction-max-reuse-frames.
	// The segments go through the world's line test (filter vtable
	// 0x009bf278, 0x007fabf0): soldiers (class 0x9493), objects without
	// collision (+0x15 bit 0) and the listener's own root when it is under
	// 50 m across don't count.
	struct RayHit {
		float fraction = 1;     // along the segment, 0..1
		uint32_t root = 0;      // the object it met (its root), 0 the terrain or none
		bool large = false;     // that root over 50 m across (slot 0x90: a Titan)
	};
	using ObstructionTest = std::function<bool(const Vec3& from, const Vec3& to, uint32_t ignore, RayHit& hit)>;
	void setObstructionTest(ObstructionTest test) { m_obstructionTest = std::move(test); }
	// The listener's root (the update 0x007f9bf0 keeps the local player's
	// object's root at +0xdc), 0 for none; `large` over 50 m across.
	void setListenerRoot(uint32_t root, bool large)
	{
		m_listenerRoot = root;
		m_listenerRootLarge = large;
	}
	// How obstructed a sound at `at` is (0x007fa350), `radius` its
	// template's soundRadius, `owner` the root of its owner (0 none).
	float obstruction(const Vec3& at, float radius, uint32_t owner, int* lastClear = nullptr) const;
	// The hook 3D sounds are given (setOcclusion), reading their owner
	// from parameter 0 and the last clear ray from parameter 1.
	ISoundOcclusion* occlusionHook();
	bool listenerInsideTitan() const { return m_insideTitan; }

	// --- Wind (0x007f9840, Sound.windSetup <template> <speed>, Settings/
	// Sound.con: S_Wind 30): a sound of the template, its volume and pitch
	// envelopes read at +0xd4 = clamp(|listener velocity| / speed, 0, 1)
	// (speed at least 1). startWind (slot 0x90, 0x007f9900, unless the
	// engine runs at quality level 2) and stopWind (slot 0x94, 0x007f9920)
	// say whether it should play; each update (0x007f9ea0) plays or stops
	// it when that changed, and stops it while `allowed` is false (no
	// first person, or inside a Titan: 0x007f9d50).
	void windSetup(const std::string& templateName, float speed);
	void startWind();
	void stopWind() { m_windWanted = false; }
	void setWindAllowed(bool allowed) { m_windAllowed = allowed; }
	bool windPlaying() const { return m_windPlaying; }
	float windVolume() const { return m_windInput; }

	// The engine's global ambient (slot 30: a PlayerControlObject's
	// interior loop the local player hears, 0x005c7dc0) and the
	// listener-3d-obstruction tweak (its listenerObstruction) together;
	// null and 0 to clear.
	void setInteriorAmbient(ISound* sound, float listenerObstruction);
	// A Sound the data configured but never created (enhanced audio, WorldConsole's activeSafe): the
	// defaults of the weapon event its name ends in, as DefaultSoundComp's own sounds have.
	void applyEventDefaults(world::ObjectTemplate& sound);

private:
	ISound* findSound(const std::string& name) const;
	void createDefaultSounds(world::ObjectTemplate& owner, const std::vector<DefaultSound>& sounds);
	void updateDucking();
	void updateWind();
	class Occlusion;

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
	ObstructionTest m_obstructionTest;
	uint32_t m_listenerRoot = 0;
	bool m_listenerRootLarge = false;
	bool m_insideTitan = false;
	std::unique_ptr<Occlusion> m_occlusion;
	mutable int m_raysThisUpdate = 0; // the obstruction rays cast since the last update
	ISound* m_wind = nullptr;
	float m_windSpeed = 1;
	float m_windInput = 0; // +0xd4, the envelopes' input
	bool m_windWanted = false, m_windPlaying = false, m_windAllowed = false;
	ISound* m_interiorAmbient = nullptr;
	float m_listenerObstruction = 0;
	float m_tinnitusAttack = 0.5f;
	float m_tinnitusSustain = 0;
	float m_tinnitusRelease = 0;
	float m_tinnitusLevel = 0;
	std::string m_provider;
};

} // namespace dice::hfe::sound
