#include "WavFileFormat.h"

#include "SoundServices.h"

#include <cstring>

namespace dice::hfe::sound {

namespace {

constexpr uint32_t fourcc(const char (&s)[5])
{
	return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
		uint32_t(uint8_t(s[3])) << 24;
}

uint32_t readU32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
uint16_t readU16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

class FileHandle {
public:
	explicit FileHandle(const char* path) : m_file(soundFiles().open(path)) {}
	~FileHandle()
	{
		if (m_file) {
			soundFiles().close(m_file);
		}
	}
	explicit operator bool() const { return m_file != nullptr; }
	bool read(void* buffer, size_t bytes) { return soundFiles().read(m_file, buffer, bytes) == bytes; }
	bool read(std::vector<uint8_t>& buffer, size_t bytes)
	{
		buffer.resize(bytes);
		return read(buffer.data(), bytes);
	}
	void skip(long bytes) { soundFiles().seek(m_file, bytes, SEEK_CUR); }

private:
	void* m_file;
};

// cue chunk: count, then 24-byte cue points {id, position, chunk, chunkStart, blockStart, sampleOffset}.
struct CuePoint {
	uint32_t id;
	uint32_t sampleOffset;
};

} // namespace

bool readWavFile(const char* path, WavData& out)
{
	FileHandle file(path);
	if (!file) {
		return false;
	}

	uint8_t riff[12];
	if (!file.read(riff, sizeof(riff)) || readU32(riff) != fourcc("RIFF") || readU32(riff + 8) != fourcc("WAVE")) {
		return false;
	}

	bool haveCue = false;
	CuePoint cue {};
	bool havePlaylist = false;
	uint32_t playlistCueId = 0;
	uint32_t playlistLength = 0;
	bool haveLabel = false;
	uint32_t labelCueId = 0;
	uint32_t labelLength = 0;

	uint8_t header[8];
	while (file.read(header, sizeof(header))) {
		const uint32_t id = readU32(header);
		const uint32_t size = readU32(header + 4);
		std::vector<uint8_t> chunk;

		if (id == fourcc("fmt ")) {
			if (!file.read(chunk, size) || size < 16) {
				SND_WARNING("Error reading format chunk of file: %s", path);
				return false;
			}
			out.formatTag = readU16(&chunk[0]);
			out.channels = readU16(&chunk[2]);
			out.sampleRate = readU32(&chunk[4]);
			out.blockAlign = readU16(&chunk[12]);
			out.bitsPerSample = readU16(&chunk[14]);
		}
		else if (id == fourcc("cue ")) {
			if (size != 0x1c) {
				SND_WARNING("%s has multiple cue points in a cue list. Ignoring all but the first cue point.", path);
			}
			if (!file.read(chunk, size) || size < 0x1c) {
				SND_ERROR("Wavefile cue list segment is corrupt (redo the loop points): %s", path);
				continue;
			}
			cue = {readU32(&chunk[4]), readU32(&chunk[4 + 20])};
			haveCue = true;
		}
		else if (id == fourcc("plst")) {
			// count, then {cueId, length, loops}
			if (!file.read(chunk, size) || size < 12) {
				SND_ERROR("Wavefile playlist chunk is corrupt (redo the loop points): %s", path);
				continue;
			}
			playlistCueId = readU32(&chunk[4]);
			playlistLength = readU32(&chunk[8]);
			havePlaylist = true;
		}
		else if (id == fourcc("LIST") && !haveLabel) {
			if (!file.read(chunk, size)) {
				SND_ERROR("Wavefile list chunk is corrupt (redo the loop points): %s", path);
				continue;
			}
			// "adtl" followed by an "ltxt" sub-chunk: {id, size, cueId, sampleLength, ...}
			if (size >= 20 && readU32(&chunk[0]) == fourcc("adtl") && readU32(&chunk[4]) == fourcc("ltxt")) {
				labelCueId = readU32(&chunk[12]);
				labelLength = readU32(&chunk[16]);
				haveLabel = true;
			}
		}
		else if (id == fourcc("data")) {
			if (!file.read(out.samples, size)) {
				SND_ERROR("Wavefile DATA chunk is corrupt: %s", path);
				return false;
			}
		}
		else {
			file.skip(size);
		}
	}

	out.frames = out.blockAlign ? static_cast<uint32_t>(out.samples.size() / out.blockAlign) : 0;

	// The playlist end is inclusive, the labelled region's isn't.
	if (haveCue && havePlaylist && playlistCueId == cue.id) {
		out.loopStart = static_cast<int32_t>(cue.sampleOffset);
		out.loopEnd = static_cast<int32_t>(cue.sampleOffset + playlistLength + 1);
		out.hasLoop = true;
	}
	else if (haveCue && haveLabel && labelCueId == cue.id) {
		out.loopStart = static_cast<int32_t>(cue.sampleOffset);
		out.loopEnd = static_cast<int32_t>(cue.sampleOffset + labelLength);
		out.hasLoop = true;
	}
	else {
		out.loopStart = 0;
		out.loopEnd = static_cast<int32_t>(out.frames);
		out.hasLoop = false;
	}
	return !out.samples.empty();
}

} // namespace dice::hfe::sound
