// The voice codec: Speex narrowband at 8 kHz, 20 ms frames of 160 samples,
// as GameSpy Voice 2 runs it in BF2142.exe (0x00737b30-0x00737d80):
// speex_nb_mode (0x00a090a4: "narrowband", bit-stream version 4), the
// quality from speexQualityFor(), the decoder's perceptual enhancement on.
// Each frame is written as bitrate / 400 bytes (SPEEX_GET_BITRATE / 50
// frames / 8 bits, 0x00737b89), which cuts the last few bits of a frame at
// qualities 1, 2, 7 and 10; the decoder reads them as zeros. This keeps
// the frames the size the original clients send and expect.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dice::hfe::voip {

constexpr int kSampleRate = 8000;
constexpr int kFrameSamples = 160;
constexpr int kFramesPerSecond = kSampleRate / kFrameSamples; // 50

class SpeexEncoder {
public:
	explicit SpeexEncoder(int speexQuality = 4);
	~SpeexEncoder();
	SpeexEncoder(const SpeexEncoder&) = delete;
	SpeexEncoder& operator=(const SpeexEncoder&) = delete;

	int quality() const { return m_quality; }
	// The size of every encoded frame.
	size_t frameBytes() const { return m_frameBytes; }
	// Appends one frame's bytes for kFrameSamples samples.
	void encode(const int16_t* samples, std::vector<uint8_t>& out);

private:
	void* m_state = nullptr;
	void* m_bits = nullptr;
	int m_quality = 0;
	size_t m_frameBytes = 0;
};

class SpeexDecoder {
public:
	SpeexDecoder();
	~SpeexDecoder();
	SpeexDecoder(const SpeexDecoder&) = delete;
	SpeexDecoder& operator=(const SpeexDecoder&) = delete;

	// Decodes one frame into kFrameSamples samples; false when Speex
	// rejects it (the samples are then silence).
	bool decode(const uint8_t* frame, size_t bytes, int16_t* samples);
	// Fills in a lost frame (Speex's packet loss concealment).
	void conceal(int16_t* samples);

private:
	void* m_state = nullptr;
	void* m_bits = nullptr;
};

// The frame size for a Speex quality without making an encoder.
size_t speexFrameBytes(int speexQuality);

} // namespace dice::hfe::voip
