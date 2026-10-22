#include "OggStream.h"

#include "SoundServices.h"

#include <vorbis/vorbisfile.h>

#include <algorithm>
#include <cstring>

namespace dice::hfe::sound {

namespace {

constexpr const char kRequiredVendor[] = "Xiph.Org libVorbis I 20030909";

struct MemoryFile {
	const uint8_t* data = nullptr;
	size_t size = 0;
	size_t position = 0;
};

size_t memoryRead(void* buffer, size_t size, size_t count, void* source)
{
	auto* file = static_cast<MemoryFile*>(source);
	if (file->position >= file->size) {
		return 0;
	}
	const size_t bytes = std::min(size * count, file->size - file->position);
	std::memcpy(buffer, file->data + file->position, bytes);
	file->position += bytes;
	return bytes;
}

int memorySeek(void* source, ogg_int64_t offset, int origin)
{
	auto* file = static_cast<MemoryFile*>(source);
	ogg_int64_t target;
	switch (origin) {
	case SEEK_SET:
		target = offset;
		break;
	case SEEK_CUR:
		target = static_cast<ogg_int64_t>(file->position) + offset;
		break;
	case SEEK_END:
		target = static_cast<ogg_int64_t>(file->size) - offset;
		break;
	default:
		return 0;
	}
	if (target < 0 || target > static_cast<ogg_int64_t>(file->size)) {
		return -1;
	}
	file->position = static_cast<size_t>(target);
	return 0;
}

int memoryClose(void*) { return 0; }

long memoryTell(void* source) { return static_cast<long>(static_cast<MemoryFile*>(source)->position); }

constexpr ov_callbacks kMemoryCallbacks = {memoryRead, memorySeek, memoryClose, memoryTell};

} // namespace

struct OggDecoder::Impl {
	OggVorbis_File vorbis {};
	MemoryFile file;
	bool open = false;
};

OggDecoder::OggDecoder() : m_impl(std::make_unique<Impl>()) {}

OggDecoder::~OggDecoder()
{
	if (m_impl->open) {
		ov_clear(&m_impl->vorbis);
	}
}

int OggDecoder::open(const std::vector<uint8_t>& data, bool)
{
	m_impl->file = {data.data(), data.size(), 0};
	const int result = ov_open_callbacks(&m_impl->file, &m_impl->vorbis, nullptr, 0, kMemoryCallbacks);
	m_impl->open = result == 0;
	return result;
}

long OggDecoder::read(void* buffer, int bytes)
{
	int section = 0;
	return ov_read(&m_impl->vorbis, static_cast<char*>(buffer), bytes, 0, 2, 1, &section);
}

int OggDecoder::seekFrame(int64_t frame)
{
	return ov_pcm_seek(&m_impl->vorbis, frame);
}

bool probeOggVorbis(const std::vector<uint8_t>& data, const char* path, OggInfo& info)
{
	MemoryFile file {data.data(), data.size(), 0};
	OggVorbis_File vorbis {};
	const int result = ov_open_callbacks(&file, &vorbis, nullptr, 0, kMemoryCallbacks);
	if (result != 0) {
		SND_WARNING("Failed to open vorbis file: %s", oggErrorName(result));
		return false;
	}
	bool ok = false;
	vorbis_comment* comment = ov_comment(&vorbis, -1);
	if (!comment) {
		SND_WARNING("Failed to get vorbis file comment: %s", oggErrorName(0));
	}
	else if (std::strncmp(comment->vendor, kRequiredVendor, sizeof(kRequiredVendor)) != 0) {
		SND_ERROR("Invalid ogg-file vendor: %s for file %s. Must be: %s", comment->vendor, path, kRequiredVendor);
	}
	else {
		vorbis_info* vi = ov_info(&vorbis, -1);
		info.sampleRate = static_cast<uint32_t>(vi->rate);
		info.channels = vi->channels;
		info.frames = static_cast<uint32_t>(ov_pcm_total(&vorbis, -1));
		ok = true;
	}
	ov_clear(&vorbis);
	return ok;
}

const char* oggErrorName(int code)
{
	switch (code) {
	case OV_FALSE: return "OV_FALSE";
	case OV_EOF: return "OV_EOF";
	case OV_HOLE: return "OV_HOLE";
	case OV_EREAD: return "OV_EREAD";
	case OV_EFAULT: return "OV_EFAULT";
	case OV_EIMPL: return "OV_EIMPL";
	case OV_EINVAL: return "OV_EINVAL";
	case OV_ENOTVORBIS: return "OV_ENOTVORBIS";
	case OV_EBADHEADER: return "OV_EBADHEADER";
	case OV_EVERSION: return "OV_EVERSION";
	case OV_ENOTAUDIO: return "OV_ENOTAUDIO";
	case OV_EBADPACKET: return "OV_EBADPACKET";
	case OV_EBADLINK: return "OV_EBADLINK";
	case OV_ENOSEEK: return "OV_ENOSEEK";
	default: return "unknown";
	}
}

} // namespace dice::hfe::sound
