// Ogg Vorbis decoding from memory (originally statically linked libVorbis
// 1.0.1 with custom read callbacks in OpenALSample.cpp/SoundFile.cpp).
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace dice::hfe::sound {

struct OggInfo {
	uint32_t sampleRate = 0;
	int channels = 0;
	uint32_t frames = 0;
};

// Validates an Ogg file. Like the original, files must have been encoded with
// libVorbis 1.0.1 ("Xiph.Org libVorbis I 20030909"), the version DICE shipped.
bool probeOggVorbis(const std::vector<uint8_t>& data, const char* path, OggInfo& info);

// Streams 16-bit PCM out of an in-memory Ogg file.
class OggDecoder {
public:
	OggDecoder();
	~OggDecoder();
	OggDecoder(const OggDecoder&) = delete;
	OggDecoder& operator=(const OggDecoder&) = delete;

	// `data` must outlive the decoder. Returns the vorbisfile error code (0 = ok).
	int open(const std::vector<uint8_t>& data, bool readComments);

	// Fills up to `bytes` of interleaved little-endian int16. Returns bytes
	// read, 0 at end of stream, or a negative vorbisfile error.
	long read(void* buffer, int bytes);

	// Seeks to a PCM frame. Returns 0 on success.
	int seekFrame(int64_t frame);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

const char* oggErrorName(int code);

} // namespace dice::hfe::sound
