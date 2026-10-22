// The sound engine (originally BfSoundEngine.cpp).
//
// Time advances in game ticks (30 per second). A played sound becomes a
// Sample that moves through these collections:
//
//   starting -> pending (heap ordered by start tick; start delayed by the
//               distance to the listener at the speed of sound)
//            -> virtual (due, waiting for a voice; sorted by priority)
//            -> playing (has an OpenAL source)
//
// When voices run out, the quietest playing samples are pushed back to
// pending and retried shortly; one-shots that wait too long are dropped.
#pragma once

#include "Ambience.h"
#include "ISoundEngine.h"
#include "SoundMath.h"

#include <list>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::sound {

class ISoundProvider;
class OpenALProvider;
class Reverb;
class Sample;
class Sound;
class SoundFile;

class BfSoundEngine final : public ISoundEngine {
public:
	BfSoundEngine();
	~BfSoundEngine();

	int initialize(int qualityLevel, const char* provider, int maxVoices) override;
	bool isInitialized() override { return m_initialized; }
	void shutdown() override;
	int qualityLevel() override { return m_qualityLevel; }
	ISoundTemplate* createTemplate() override;
	void reloadSoundFiles() override;
	void stopAllSounds() override;
	void setMasterVolume(float volume) override;
	float masterVolume() override { return m_masterVolume; }
	void setVolume(int group, float volume) override;
	float volume(int group) override;
	void setListenerPosition(const Vec3* position) override;
	const Vec3* listenerPosition() override { return m_listenerPosition; }
	void setListenerOrientation(const Vec3* forward, const Vec3* up) override;
	const Vec3* listenerForward() override { return m_listenerForward; }
	const Vec3* listenerUp() override { return m_listenerUp; }
	void setListenerVelocity(const Vec3* velocity) override;
	const Vec3* listenerVelocity() override { return m_listenerVelocity; }
	void setListenerInsideTitan(bool inside) override { m_insideTitan = inside; }
	bool listenerInsideTitan() override { return m_insideTitan; }
	void setIndoorLevel(float level) override;
	float indoorLevel() override { return m_indoorLevel; }
	void setDopplerFactor(float factor) override;
	float dopplerFactor() override;
	void update(int ticks) override;
	void setupTinnitus(const char* const* files, int count) override;
	void startTinnitus(float attack, float sustain, float release, float level, int sampleIndex) override;
	void stopTinnitus(bool immediate) override;
	void addAmbientEmitter(ISoundTemplate* emitter) override { m_emitters.add(emitter); }
	int ambientEmitterCount() override { return m_emitters.count(); }
	void setGlobalAmbient(ISound* sound) override { m_emitters.setGlobal(sound); }
	ISound* currentAmbientEmitter() override { return m_emitters.global(); }
	void removeAmbientEmitter(ISoundTemplate* emitter) override { m_emitters.remove(emitter); }
	void clearAmbience() override;
	void setTweak(const char* name, float value) override;
	float tweak(const char* name) override;
	void loadReverbPreset(const char* path) override;
	void setFileSystem(ISoundFileSystem* fileSystem) override;
	void setLogger(ISoundLogger* logger) override;
	int countLoadedSoundFiles(int* count) override;
	const char* const* providerNames(int* count) override;
	const SoundStatistics* statistics() override;
	void setAmbientSound(ISound* sound, float fadeSeconds) override { m_crossfader.setAmbient(sound, fadeSeconds); }
	void stopAmbientSound() override { m_crossfader.stop(); }
	void setEditorMode(bool enabled) override { m_editorMode = enabled; }
	bool editorMode() override { return m_editorMode; }
	int eaxVersion() override;
	int reverbInfo(int* out) override;
	void setDebugHook(void* hook) override { m_debugHook = hook; }
	void setMusic(const char* path) override;
	void startMusic() override;
	void stopMusic(float fadeSeconds) override;
	void registerFadingSound(ISound* sound) override { m_fading.push_back(sound); }
	void* destroy(unsigned flags) override;

	// --- Used by sounds, samples and ambience ---
	SoundFile* acquireSoundFile(const char* path);
	void scheduleSample(Sound& sound, int sampleIndex, const Vec3* position);
	void stopSound(Sound* sound, bool immediate);
	void stopSample(Sample* sample, bool immediate);
	int now() const { return m_now; }
	int frame() const { return m_frame; }
	int outputRate() const { return m_outputRate; }
	void toListenerSpace(Sound& sound, Vec3& position, Vec3& velocity) const;
	const Matrix4& listenerMatrix() const { return m_listenerMatrix; }
	float airAbsorption(float distance) const;
	float gain2d() const { return m_gain2d; }
	float gain3d() const { return m_gain3d; }
	float tinnitusLevel() const { return m_tinnitusLevel; }
	AmbientEmitters& ambientEmitters() { return m_emitters; }

private:
	int openProvider(const std::string& name);
	bool refreshProviders();
	void setMaxVoices(int count);
	void requeuePlaying();
	void unloadSoundFiles(bool force, bool forceStreaming);
	void reloadFiles();
	void updateBalance();
	void updateTinnitus();
	void updateListener();
	void queueStartingSample(Sample* sample);
	void updateSamples(int ticks);
	void startNextPending();
	bool startVirtualSample();
	void stealVoice();
	void computePriorities(std::list<Sample*>& samples);
	bool shouldCull(Sound& sound, const Vec3* position);
	void updateFades();
	void processStops();
	void pushPending(Sample* sample);
	Sample* popPending();
	bool stopInList(std::list<Sample*>& list, Sample* sample, bool immediate);
	void stopSoundInList(std::list<Sample*>& list, Sound* sound, bool immediate);
	void deleteAll(std::list<Sample*>& list);

	bool m_initialized = false;
	int m_qualityLevel = 0;
	std::vector<SoundFile*> m_fileCache;
	std::vector<std::string> m_providerNames;
	std::vector<const char*> m_providerNamePointers;
	ISoundProvider* m_provider = nullptr;
	std::unique_ptr<OpenALProvider> m_openAL;

	std::vector<Sample*> m_starting;
	std::list<Sample*> m_playing;
	std::list<Sample*> m_virtual;
	std::vector<Sample*> m_pending; // min-heap on scheduledTime
	std::vector<Sample*> m_stopping;
	std::list<ISound*> m_fading;

	int m_outputRate = 44100;
	float m_masterVolume = 1.0f;
	float m_volumes[8] = {1, 1, 1, 1, 1, 1, 1, 1};
	float m_gain2d = 1.0f;
	float m_gain3d = 1.0f;
	const Vec3* m_listenerPosition;
	const Vec3* m_listenerForward;
	const Vec3* m_listenerUp;
	const Vec3* m_listenerVelocity;
	bool m_insideTitan = false;
	Matrix4 m_listenerMatrix; // world to listener space
	Vec3 m_listenerVelocityLocal;
	int m_now = 0;
	int m_frame = 0;
	SoundStatistics m_statistics {};
	int m_maxVoicesInUse = 16;
	float m_indoorLevel = 0.0f;

	// Tinnitus (ringing ears after explosions): an envelope-driven 2D loop
	// that also muffles other sounds while it plays.
	ISound* m_tinnitus = nullptr;
	int m_tinnitusStart = 0;
	int m_tinnitusEnd = -1;
	float m_tinnitusMaxLevel = 0.0f;
	float m_tinnitusEnvelopeInput = 0.0f;
	float m_tinnitusLevel = 0.0f;
	float m_tinnitusHoldPoint = -1.0f; // envelope position to hold at until released

	AmbientCrossfader m_crossfader;
	AmbientEmitters m_emitters;
	bool m_editorMode = false;
	std::unique_ptr<Reverb> m_reverb;
	void* m_debugHook = nullptr;

	// Last initialize() arguments, to make re-initialisation a no-op.
	int m_initMaxVoices = 0;
	std::string m_initProvider;
	int m_initQuality = 0;
};

// The engine singleton (created by getSoundEngine()).
BfSoundEngine& soundEngine();

} // namespace dice::hfe::sound
