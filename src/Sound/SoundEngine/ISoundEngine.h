// Public interface of the sound engine (BF2142Audio.dll).
//
// The game talks to the engine only through these abstract classes, obtained
// from getSoundEngine(). Their virtual method order mirrors the original DLL
// slot for slot, because the retail executable calls them by vtable offset;
// do not reorder. Slot numbers are noted where it helps cross-referencing.
//
// Two conventions follow from that binary interface:
//  - Objects are reference counted (addRef/release) and the last slot of each
//    interface is the original "scalar deleting destructor" (destroy), which
//    frees the object when `flags & 1`.
//  - The engine stores pointers to game-owned vectors (listener position and
//    orientation) and reads them every update, rather than copying.
#pragma once

#include "SoundEnvelope.h"
#include "SoundMath.h"

namespace dice::hfe::sound {

class ISoundFileSystem;
class ISoundLogger;
class ISound;

// Supplies a playing sound's world transform; implemented by game objects.
class ISoundTransform {
public:
	virtual const Matrix4* worldTransform() = 0;  // slot 0
	virtual const Vec3* velocity() = 0;            // slot 1

protected:
	~ISoundTransform() = default;
};

// Optional per-sound occlusion test (the game ray-casts against the level).
class ISoundOcclusion {
public:
	// Returns how occluded a sound emitted at `position` is (0 clear, 1 blocked).
	virtual float occlusion(ISound* sound, const Vec3* position) = 0;

protected:
	~ISoundOcclusion() = default;
};

// Shared, data-driven description of a sound ("ObjectTemplate.create Sound").
class ISoundTemplate {
public:
	virtual int addRef() = 0;                                             // 0
	virtual int release() = 0;                                            // 1
	virtual void setSoundFilename(const char* path) = 0;                  // 2
	virtual const char* soundFilename() = 0;                              // 3
	virtual void setSoundFilenames(const char* const* paths, int count) = 0; // 4
	virtual const char* const* soundFilenames(int* count) = 0;            // 5
	virtual void setLoopCount(int count) = 0;                             // 6 (0 = loop forever)
	virtual int loopCount() = 0;                                          // 7
	virtual void setVolume(float volume) = 0;                             // 8
	virtual float volume() = 0;                                           // 9
	virtual void setVolumeEnvelope(float minX, float maxX, float minY, float maxY, bool randomized,
		int pointCount, const float* points) = 0;                         // 10
	virtual const float* volumeEnvelope(float* minX, float* maxX, float* minY, float* maxY, bool* randomized,
		int* pointCount) = 0;                                             // 11
	virtual ISoundEnvelope* volumeEnvelopeObject() = 0;                    // 12
	virtual void setPitch(float pitch) = 0;                               // 13
	virtual float pitch() = 0;                                            // 14
	virtual void setPitchEnvelope(float minX, float maxX, float minY, float maxY, bool randomized,
		int pointCount, const float* points) = 0;                         // 15
	virtual const float* pitchEnvelope(float* minX, float* maxX, float* minY, float* maxY, bool* randomized,
		int* pointCount) = 0;                                             // 16
	virtual ISoundEnvelope* pitchEnvelopeObject() = 0;                     // 17
	virtual void setVolumeGroup(int group) = 0;                           // 18 (0-7, see ISoundEngine::setVolume)
	virtual int volumeGroup() = 0;                                        // 19
	virtual void setPan(float pan) = 0;                                   // 20 (2D sounds, 0.5 = centre)
	virtual float pan() = 0;                                              // 21
	virtual void setMinDistance(float distance) = 0;                      // 22
	virtual float minDistance() = 0;                                      // 23
	virtual void setHalfVolumeDistance(float distance) = 0;               // 24
	virtual float halfVolumeDistance() = 0;                               // 25
	virtual void setIs3dSound(bool is3d) = 0;                             // 26
	virtual bool is3dSound() = 0;                                         // 27
	virtual void setStopType(int type) = 0;                               // 28 (0-2)
	virtual int stopType() = 0;                                           // 29
	virtual void setReverbLevel(float level) = 0;                         // 30
	virtual float reverbLevel() = 0;                                      // 31
	virtual void setPosition(const Vec3* position) = 0;                   // 32
	virtual const Vec3* position() = 0;                                   // 33
	virtual void setDirection(const Vec3* direction) = 0;                 // 34
	virtual const Vec3* direction() = 0;                                  // 35
	virtual void setSoundRadius(float radius) = 0;                        // 36
	virtual float soundRadius() = 0;                                      // 37
	virtual void setTitanInteriorAmbient(bool interior) = 0;                       // 38
	virtual bool isTitanInteriorAmbient() = 0;                                   // 39
	virtual void setConeInnerAngle(float degrees) = 0;                    // 40
	virtual float coneInnerAngle() = 0;                                   // 41
	virtual void setConeOuterAngle(float degrees) = 0;                    // 42
	virtual float coneOuterAngle() = 0;                                   // 43
	virtual void setConeOuterVolume(float volume) = 0;                    // 44
	virtual float coneOuterVolume() = 0;                                  // 45
	// How strongly occlusion and the tinnitus effect muffle this sound (0-1).
	virtual void setOcclusionFactor(float factor) = 0;                    // 46
	virtual float occlusionFactor() = 0;                                  // 47
	virtual void markSoundFilesUsed() = 0;                                // 48
	virtual void setSampleCount(int qualityLevel, int count) = 0;         // 49 (lowSamples/mediumSamples)
	virtual int sampleCount(int qualityLevel) = 0;                        // 50
	virtual void setRandomMode(unsigned char mode) = 0;                   // 51
	virtual unsigned char randomMode() = 0;                               // 52
	virtual void setFlags(unsigned flags) = 0;                            // 53
	virtual unsigned flags() = 0;                                         // 54
	virtual void* loadedSoundFile(int index) = 0;                         // 55
	virtual ISound* createSound() = 0;                                    // 56
	virtual void* destroy(unsigned flags) = 0;                            // 57

protected:
	~ISoundTemplate() = default;
};

// A playable instance of a template, owned by a game object.
class ISound {
public:
	virtual int addRef() = 0;                                             // 0
	virtual int release() = 0;                                            // 1
	virtual void setPlayFlags(unsigned flags) = 0;                        // 2
	virtual unsigned playFlags() = 0;                                     // 3
	// The play variants were overloads in the original. MSVC lays out
	// overloaded virtuals in reverse declaration order and other compilers
	// don't, so they have distinct names here to pin the slot order.
	virtual void playLooped(int loopCount) = 0;                           // 4
	virtual void play() = 0;                                              // 5
	virtual void playAtLooped(const Vec3* position, int loopCount) = 0;   // 6
	virtual void playAt(const Vec3* position) = 0;                        // 7
	virtual bool playSampleLooped(int index, int loopCount) = 0;          // 8
	virtual bool playSample(int index) = 0;                               // 9
	virtual bool playSampleAtLooped(const Vec3* position, int index, int loopCount) = 0; // 10
	virtual bool playSampleAt(const Vec3* position, int index) = 0;       // 11
	virtual void fadeOut(int ticks) = 0;                                  // 12 (0 = stop now)
	virtual void stop() = 0;                                              // 13
	virtual void stopImmediately() = 0;                                   // 14
	virtual bool isPlaying() = 0;                                         // 15
	virtual void setTransform(ISoundTransform* transform) = 0;            // 16
	virtual ISoundTransform* transform() = 0;                             // 17
	virtual void setVolumeEnvelope(ISoundEnvelope* envelope, const float* input) = 0; // 18
	virtual ISoundEnvelope* volumeEnvelope() = 0;                          // 19
	virtual void setPitchEnvelope(ISoundEnvelope* envelope, const float* input) = 0;  // 20
	virtual ISoundEnvelope* pitchEnvelope() = 0;                           // 21
	virtual void setOcclusion(ISoundOcclusion* occlusion) = 0;            // 22
	virtual ISoundOcclusion* occlusion() = 0;                             // 23
	// Head-relative sounds play at the listener (first-person weapons, UI).
	virtual void setHeadRelative(bool relative) = 0;                      // 24
	virtual bool headRelative() = 0;                                      // 25
	virtual void setParameter(int index, unsigned value) = 0;             // 26
	virtual unsigned parameter(int index) = 0;                            // 27
	virtual const float* sampleTiming(int index) = 0;                     // 28 (start, loop, end seconds)
	virtual ISoundTemplate* soundTemplate() = 0;                          // 29
	virtual void setFading(bool fading) = 0;                              // 30
	virtual bool isFading() = 0;                                          // 31
	virtual bool updateFade(int now) = 0;                                 // 32
	virtual void* destroy(unsigned flags) = 0;                            // 33

protected:
	~ISound() = default;
};

// Counters shown by the game's sound debug overlay (field order is binary
// interface: the game reads the struct the engine returns).
struct SoundStatistics {
	const char* providerDescription;
	int playingSamples;
	int virtualSamples;     // waiting for a voice
	int pendingSamples;     // scheduled for a later tick
	int staticBufferBytes;
	int streamingFileBytes;
	int streamingBufferBytes;
	int staticBuffers;
	int streamingBuffers;
	int loadedFiles;
	int soundsAlive;
	int samplesAlive;
	int sources;
	int maxVoices;
};
static_assert(sizeof(SoundStatistics) == 14 * sizeof(int) || sizeof(void*) != 4);

class ISoundEngine {
public:
	virtual int initialize(int qualityLevel, const char* provider, int maxVoices) = 0; // 0
	virtual bool isInitialized() = 0;                                     // 1
	virtual void shutdown() = 0;                                          // 2
	virtual int qualityLevel() = 0;                                       // 3
	virtual ISoundTemplate* createTemplate() = 0;                         // 4
	virtual void reloadSoundFiles() = 0;                                  // 5
	virtual void stopAllSounds() = 0;                                     // 6
	virtual void setMasterVolume(float volume) = 0;                       // 7
	virtual float masterVolume() = 0;                                     // 8
	virtual void setVolume(int group, float volume) = 0;                  // 9 (0 effects, 1 music, ...)
	virtual float volume(int group) = 0;                                  // 10
	virtual void setListenerPosition(const Vec3* position) = 0;           // 11
	virtual const Vec3* listenerPosition() = 0;                           // 12
	virtual void setListenerOrientation(const Vec3* forward, const Vec3* up) = 0; // 13
	virtual const Vec3* listenerForward() = 0;                            // 14
	virtual const Vec3* listenerUp() = 0;                                 // 15
	virtual void setListenerVelocity(const Vec3* velocity) = 0;           // 16
	virtual const Vec3* listenerVelocity() = 0;                           // 17
	virtual void setListenerInsideTitan(bool inside) = 0;                 // 18
	virtual bool listenerInsideTitan() = 0;                               // 19
	virtual void setIndoorLevel(float level) = 0;                         // 20
	virtual float indoorLevel() = 0;                                      // 21
	virtual void setDopplerFactor(float factor) = 0;                      // 22
	virtual float dopplerFactor() = 0;                                    // 23
	virtual void update(int ticks) = 0;                                   // 24 (30 ticks per second)
	virtual void setupTinnitus(const char* const* files, int count) = 0;  // 25
	virtual void startTinnitus(float attack, float sustain, float release, float level, int loopCount) = 0; // 26
	virtual void stopTinnitus(bool immediate) = 0;                        // 27
	virtual void addAmbientEmitter(ISoundTemplate* emitter) = 0;          // 28
	virtual int ambientEmitterCount() = 0;                                // 29
	virtual void setGlobalAmbient(ISound* sound) = 0;                     // 30
	virtual ISound* currentAmbientEmitter() = 0;                          // 31
	virtual void removeAmbientEmitter(ISoundTemplate* emitter) = 0;       // 32
	virtual void clearAmbience() = 0;                                     // 33
	virtual void setTweak(const char* name, float value) = 0;             // 34
	virtual float tweak(const char* name) = 0;                            // 35
	virtual void loadReverbPreset(const char* path) = 0;                  // 36
	virtual void setFileSystem(ISoundFileSystem* fileSystem) = 0;         // 37
	virtual void setLogger(ISoundLogger* logger) = 0;                     // 38
	virtual int countLoadedSoundFiles(int* count) = 0;                    // 39
	virtual const char* const* providerNames(int* count) = 0;             // 40
	virtual const SoundStatistics* statistics() = 0;                      // 41
	virtual void setAmbientSound(ISound* sound, float fadeSeconds) = 0;   // 42
	virtual void stopAmbientSound() = 0;                                  // 43
	virtual void setEditorMode(bool enabled) = 0;                         // 44
	virtual bool editorMode() = 0;                                        // 45
	virtual int eaxVersion() = 0;                                         // 46
	virtual int reverbInfo(int* out) = 0;                                 // 47 (unused stub)
	virtual void setDebugHook(void* hook) = 0;                            // 48 (stored, never read)
	virtual void setMusic(const char* path) = 0;                          // 49
	virtual void startMusic() = 0;                                        // 50
	virtual void stopMusic(float fadeSeconds) = 0;                        // 51
	virtual void registerFadingSound(ISound* sound) = 0;                  // 52
	virtual void* destroy(unsigned flags) = 0;                            // 53

protected:
	~ISoundEngine() = default;
};

// Entry points (the DLL's two exports).
ISoundEngine* getSoundEngine();
void shutdownSoundEngine();

} // namespace dice::hfe::sound
