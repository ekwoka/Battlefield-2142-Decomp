// Voices (originally OpenALSample.cpp).
//
// A Sample is one playback of one sound file by a Sound. The engine creates
// samples when a sound plays, schedules them (start delay for the speed of
// sound, priority culling when voices run out) and updates the playing ones
// every frame. OpenALSample maps a sample onto an OpenAL source; the two
// concrete kinds queue either static buffers (WAV, with start/loop/end
// blocks) or a stream of decoded Ogg chunks.
//
// OpenAL sources and streaming buffers are pooled, since creating them is
// slow on some drivers.
#pragma once

#include "BfOpenAL.h"
#include "SoundMath.h"

#include <cstdint>
#include <memory>

namespace dice::hfe::sound {

class Sound;
class SoundFile;
class OggDecoder;

class Sample {
public:
	enum class State { None = 0, Finished = 2, Playing = 4 };

	enum class StartResult { Started = 0, NoVoice = 1, Failed = 2 };

	Sample();
	virtual ~Sample();

	// Silences the voice and returns it to the pool.
	virtual void releaseVoice() = 0;
	virtual State state() = 0;
	virtual Vec3 worldPosition() = 0;
	virtual const Vec3& positionOffset() const = 0;
	virtual StartResult start(int startFrame, int loopCount) = 0;
	virtual void update() = 0;
	// Stops according to the template's stop type. Returns true once the
	// sample is finished; false while an end block still plays.
	virtual bool stop(bool immediate) = 0;
	virtual bool stopRequested() const = 0;
	virtual bool isStreaming() const { return false; }

	Sound* sound() const { return m_sound; }
	void setSound(Sound* sound) { m_sound = sound; }
	SoundFile* soundFile() const;
	bool is3d() const;
	// How long the sample plays, in engine ticks (INT_MAX when looping forever).
	int durationTicks() const;

	int startTime = 0;      // engine tick the sample was scheduled to start
	int scheduledTime = 0;
	float priority = 0.0f;  // higher plays first when voices run out
	uint8_t pitchSeed = 0;  // pick envelope variants; re-rolled every loop
	uint8_t volumeSeed = 0;
	int16_t sampleIndex = 0;
	bool started = false;
	bool voiceAssigned = false;
	float loopProgress = 0.0f;

	// Updates loopProgress from elapsed ticks; a wrap re-rolls the seeds so
	// every loop gets a new random variation.
	float updateLoopProgress(int playOffset, int elapsedTicks);

private:
	Sound* m_sound = nullptr;
};

class OpenALSample : public Sample {
public:
	explicit OpenALSample(const Vec3* positionOffset);
	~OpenALSample() override;

	void releaseVoice() override;
	State state() override;
	Vec3 worldPosition() override;
	const Vec3& positionOffset() const override { return m_offset; }
	StartResult start(int startFrame, int loopCount) override;
	void update() override;
	bool stop(bool immediate) override;
	bool stopRequested() const override { return m_stopRequested; }

protected:
	// Current play position in frames, for loop tracking.
	virtual int playOffset() = 0;
	virtual bool queueBuffers(int startFrame, int loopCount) = 0;
	virtual void updateQueue() = 0;
	virtual bool queueEndBlock() = 0;
	virtual bool finishedLooping() = 0;
	int unqueueAll();

	void applyDistanceModel();
	float computeOcclusionGain();
	void setOcclusion(float occlusion);
	void setTinnitusMuffle(float amount);

	// EAX 3.0 per-source properties (EAXBUFFERPROPERTIES).
	struct EaxBufferProperties {
		int32_t direct = 0;
		int32_t directHF = 0;
		int32_t room = 0;
		int32_t roomHF = 0;
		int32_t obstruction = 0;
		float obstructionLFRatio = 0.0f;
		int32_t occlusion = 0;
		float occlusionLFRatio = 0.25f;
		float occlusionRoomRatio = 1.5f;
		float occlusionDirectRatio = 1.0f;
		int32_t exclusion = 0;
		float exclusionLFRatio = 1.0f;
		int32_t outsideVolumeHF = 0;
		float dopplerFactor = 0.0f;
		float rolloffFactor = 0.0f;
		float roomRolloffFactor = 0.0f;
		float airAbsorptionFactor = 1.0f;
		uint32_t flags = 7;
	};
	static_assert(sizeof(EaxBufferProperties) == 0x48);

	Vec3 m_offset;
	ALuint m_source = 0;
	bool m_stopRequested = false;
	float m_occlusion = 0.0f; // smoothed
	EaxBufferProperties m_eax;
};

class StaticSample final : public OpenALSample {
public:
	using OpenALSample::OpenALSample;
	~StaticSample() override;

protected:
	int playOffset() override;
	bool queueBuffers(int startFrame, int loopCount) override;
	void updateQueue() override;
	bool queueEndBlock() override;
	bool finishedLooping() override { return true; }

private:
	// Set while the one-shot start block plays before an infinite loop: the
	// source switches to looping once the start block is consumed.
	bool m_loopAfterStart = false;
};

class StreamingSample final : public OpenALSample {
public:
	explicit StreamingSample(const Vec3* positionOffset);
	~StreamingSample() override;
	bool isStreaming() const override { return true; }

protected:
	int playOffset() override { return 0; }
	bool queueBuffers(int startFrame, int loopCount) override;
	void updateQueue() override;
	bool queueEndBlock() override { return false; }
	bool finishedLooping() override { return m_loopsRemaining < 1; }

private:
	static constexpr int kBufferCount = 4;

	std::unique_ptr<OggDecoder> m_decoder;
	ALuint m_buffers[kBufferCount] = {};
	int m_loopsRemaining = 0;
	int m_primed = 0; // buffers queued so far
};

// Streaming helpers shared with the music player.
bool startOggStream(std::unique_ptr<OggDecoder>& decoder, const SoundFile& file, uint8_t* scratch,
	int& loopsRemaining, int startFrame, ALuint source, ALuint buffer, bool logErrors);
bool updateOggStream(OggDecoder* decoder, const SoundFile& file, uint8_t* scratch, int& loopsRemaining,
	ALuint source, ALuint primeBuffer, bool logErrors);
constexpr int kStreamChunkBytes = 0x2000;

// Pools (cleared when the provider closes).
void setMaxSources(int count);
void purgeSourcePool();
void purgeStreamingBufferPool();

struct SamplePoolStats {
	int samples = 0;
	int sources = 0;
	int streamingBuffers = 0;
	int streamingBytes = 0;
};
SamplePoolStats& samplePoolStats();

} // namespace dice::hfe::sound
