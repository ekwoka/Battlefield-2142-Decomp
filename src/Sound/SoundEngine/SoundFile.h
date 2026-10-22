// A sound asset (originally SoundFile.cpp).
//
// WAV files become static OpenAL buffers. Because BF2142 sounds can loop a
// middle section, a WAV is split into up to three buffers: a one-shot start
// block, the loop block, and a one-shot end block. Ogg files (music, long
// ambients, voice-over) stay compressed in memory and are decoded while
// playing by StreamingSample.
//
// Files load lazily: only while some sound template references them and only
// if they are enabled for the current quality level.
#pragma once

#include "BfOpenAL.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dice::hfe::sound {

class SoundFile {
public:
	enum class Kind { Unloaded = 0, Static = 1, Streaming = 2 };

	// Quality levels are 0 = high (44.1 kHz), 1 = medium (22.05 kHz), 2 = low (11.025 kHz).
	enum QualityFlags : uint32_t {
		EnabledAtHigh = 1u << 0,
		EnabledAtMedium = 1u << 1,
		EnabledAtLow = 1u << 2,
		KeepVoiceQuality = 1u << 16, // never drop below 22.05 kHz (voice-over)
	};

	explicit SoundFile(std::string path);
	~SoundFile();

	SoundFile(const SoundFile&) = delete;
	SoundFile& operator=(const SoundFile&) = delete;

	// The engine's file cache holds one reference and every template using
	// the file another; usage counts only sounds that actually played it.
	int addRef() { return ++m_refCount; }
	int release();
	void addUsage() { ++m_usageCount; }
	void resetUsage() { m_usageCount = 0; }
	bool inUse() const;

	const std::string& path() const { return m_path; }
	Kind kind() const { return m_kind; }
	bool loaded() const { return m_loaded; }
	uint32_t& qualityFlags() { return m_qualityFlags; }
	bool enabledAt(int qualityLevel) const { return m_qualityFlags & (1u << qualityLevel); }

	// Loads and uploads the file if it is referenced, enabled at the current
	// quality, and not loaded yet.
	void load(int qualityLevel);

	// Frees the data if it is unused (or `force`); `forceStreaming` applies
	// to Ogg files, which are cheap to keep.
	void unload(bool force, bool forceStreaming);

	// Sample layout.
	uint32_t sampleRate() const { return m_sampleRate; }
	uint32_t frames() const { return m_frames; }
	uint32_t bytesPerFrame() const { return m_bytesPerFrame; }
	int channels() const { return m_channels; }
	int32_t loopStart() const { return m_loopStart; }   // frames
	int32_t loopEnd() const { return m_loopEnd; }       // frames
	// Durations of the start, loop and end blocks in seconds.
	const float* timing() const { return m_timing; }

	// Static (WAV) buffers; 0 where the block doesn't exist.
	ALuint startBuffer() const { return m_startBuffer; }
	ALuint loopBuffer() const { return m_loopBuffer; }
	ALuint endBuffer() const { return m_endBuffer; }
	ALenum alFormat() const { return m_bytesPerFrame == 2 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16; }

	// Compressed Ogg data for streaming.
	const std::vector<uint8_t>& encodedData() const { return m_data; }

private:
	bool loadWav(int qualityLevel);
	bool loadOgg();
	bool createBuffers(int qualityLevel);
	void deleteBuffers();
	void reset();

	std::string m_path;
	int m_refCount = 0;
	int m_usageCount = 0;
	int m_failures = 0;
	bool m_loaded = false;
	Kind m_kind = Kind::Unloaded;
	uint32_t m_qualityFlags = 0;

	std::vector<uint8_t> m_data; // PCM until uploaded, or compressed Ogg
	uint32_t m_sampleRate = 0;
	uint32_t m_frames = 0;
	uint32_t m_bytesPerFrame = 0;
	int m_channels = 0;
	int32_t m_loopStart = 0;
	int32_t m_loopEnd = -1;
	float m_timing[3] = {};

	ALuint m_startBuffer = 0;
	ALuint m_loopBuffer = 0;
	ALuint m_endBuffer = 0;
	uint32_t m_uploadedBytes = 0;
};

// Memory statistics shown by the engine's debug overlay.
struct SoundFileStats {
	uint32_t staticBytes = 0;
	uint32_t streamingBytes = 0;
	uint32_t buffers = 0;
	uint32_t loadedFiles = 0;
};
SoundFileStats& soundFileStats();

} // namespace dice::hfe::sound
