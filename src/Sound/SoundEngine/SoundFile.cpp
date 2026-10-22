#include "SoundFile.h"

#include "OggStream.h"
#include "SoundServices.h"
#include "SoundTweaks.h"
#include "WavFileFormat.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace dice::hfe::sound {

namespace {

constexpr uint32_t kQualitySampleRates[] = {44100, 22050, 11025};
constexpr uint32_t kVoiceMinimumRate = 22050;

// A loop shorter than this can't be streamed between its start and loop blocks.
constexpr float kMinimumLoopSeconds = 0.5f;

bool hasExtension(const std::string& path, const char* extension)
{
	if (path.size() < 5) {
		return false;
	}
	for (size_t i = 0; i < 3; ++i) {
		if (std::tolower(static_cast<unsigned char>(path[path.size() - 3 + i])) != extension[i]) {
			return false;
		}
	}
	return true;
}

} // namespace

SoundFileStats& soundFileStats()
{
	static SoundFileStats stats;
	return stats;
}

SoundFile::SoundFile(std::string path) : m_path(std::move(path)) {}

SoundFile::~SoundFile()
{
	reset();
}

int SoundFile::release()
{
	if (--m_refCount <= 0) {
		delete this;
		return 0;
	}
	if (m_refCount == 1) {
		m_usageCount = 0;
	}
	return m_refCount;
}

bool SoundFile::inUse() const
{
	return soundTweaks().get(Tweak::SoundFileUseUsageRefCount) > 0.0f ? m_usageCount > 0 : m_refCount > 1;
}

void SoundFile::load(int qualityLevel)
{
	if (!enabledAt(qualityLevel) || m_loaded) {
		return;
	}
	if (!inUse()) {
		return;
	}

	bool parsed = false;
	if (hasExtension(m_path, "wav")) {
		parsed = loadWav(qualityLevel);
	}
	else if (hasExtension(m_path, "ogg")) {
		parsed = loadOgg();
	}
	if (!parsed) {
		if (m_failures == 0 && !m_path.empty()) {
			SND_ERROR("Failed to open/parse %s.", m_path.c_str());
			++m_failures;
		}
		return;
	}

	const int32_t loopEnd = m_loopEnd < 0 ? static_cast<int32_t>(m_frames) : m_loopEnd;
	const float rate = static_cast<float>(m_sampleRate);
	m_timing[0] = static_cast<float>(m_loopStart) / rate;
	m_timing[1] = static_cast<float>(loopEnd - m_loopStart) / rate;
	m_timing[2] = static_cast<float>(static_cast<int32_t>(m_frames) - loopEnd) / rate;
	if (m_timing[0] > 0.0f && m_timing[1] < kMinimumLoopSeconds) {
		SND_WARNING("%s has a loop-block which is shorter than %f seconds. This is not allowed if the sample has a "
		            "start-block (i.e. the first loop-point is > 0).",
			m_path.c_str(), kMinimumLoopSeconds);
		return;
	}

	++soundFileStats().loadedFiles;
	if (createBuffers(qualityLevel)) {
		m_loaded = true;
	}
	m_failures = 0;
}

bool SoundFile::loadWav(int qualityLevel)
{
	m_kind = Kind::Static;
	WavData wav;
	if (!readWavFile(m_path.c_str(), wav)) {
		return false;
	}
	m_data = std::move(wav.samples);
	m_sampleRate = wav.sampleRate;
	m_frames = wav.frames;
	m_bytesPerFrame = wav.blockAlign;
	m_channels = wav.channels;
	m_loopStart = wav.loopStart;
	m_loopEnd = wav.loopEnd;

	// Lower quality levels decimate by an integer factor. Stereo files keep
	// only their left channel.
	uint32_t limit = kQualitySampleRates[std::clamp(qualityLevel, 0, 2)];
	if (limit < kVoiceMinimumRate && (m_qualityFlags & KeepVoiceQuality)) {
		limit = kVoiceMinimumRate;
	}
	if (limit < m_sampleRate) {
		const uint32_t factor = m_sampleRate / limit;
		m_loopStart /= static_cast<int32_t>(factor);
		m_loopEnd /= static_cast<int32_t>(factor);
		const uint32_t frames = m_frames / factor;
		const size_t stride = factor * (m_channels == 2 ? 2 : 1);
		const auto* in = reinterpret_cast<const int16_t*>(m_data.data());
		std::vector<uint8_t> out(frames * sizeof(int16_t));
		auto* outSamples = reinterpret_cast<int16_t*>(out.data());
		for (uint32_t i = 0; i < frames; ++i) {
			outSamples[i] = in[i * stride];
		}
		m_data = std::move(out);
		m_frames = frames;
		m_channels = 1;
		m_bytesPerFrame = 2;
		m_sampleRate = limit;
	}
	return true;
}

bool SoundFile::loadOgg()
{
	ISoundFileSystem& files = soundFiles();
	void* file = files.open(m_path.c_str());
	if (!file) {
		return false;
	}
	const long size = files.seek(file, 0, SEEK_END);
	files.seek(file, 0, SEEK_SET);
	bool ok = false;
	if (size > 0) {
		std::vector<uint8_t> data(static_cast<size_t>(size));
		if (files.read(file, data.data(), data.size()) == data.size()) {
			OggInfo info;
			if (probeOggVorbis(data, m_path.c_str(), info)) {
				m_kind = Kind::Streaming;
				m_data = std::move(data);
				m_sampleRate = info.sampleRate;
				m_channels = info.channels;
				m_bytesPerFrame = static_cast<uint32_t>(info.channels) * 2;
				m_frames = info.frames;
				m_loopStart = 0;
				m_loopEnd = static_cast<int32_t>(info.frames);
				soundFileStats().streamingBytes += static_cast<uint32_t>(m_data.size());
				ok = true;
			}
		}
	}
	files.close(file);
	return ok;
}

bool SoundFile::createBuffers(int qualityLevel)
{
	if (m_kind == Kind::Streaming) {
		return true;
	}
	const uint32_t startBytes = static_cast<uint32_t>(m_loopStart) * m_bytesPerFrame;
	const uint32_t loopEndBytes = static_cast<uint32_t>(m_loopEnd) * m_bytesPerFrame;
	const uint32_t totalBytes = static_cast<uint32_t>(m_data.size());

	// At low quality, sounds with a start/end block keep only their loop.
	const bool split = !(qualityLevel == 2 && startBytes > 0 && loopEndBytes != totalBytes);

	genStaticBuffers(1, &m_loopBuffer);
	++soundFileStats().buffers;
	if (split) {
		if (startBytes > 0) {
			genStaticBuffers(1, &m_startBuffer);
			++soundFileStats().buffers;
		}
		if (loopEndBytes != totalBytes) {
			genStaticBuffers(1, &m_endBuffer);
			++soundFileStats().buffers;
		}
	}

	m_uploadedBytes = 0;
	const ALenum format = alFormat();
	const auto upload = [&](ALuint buffer, uint32_t from, uint32_t to) {
		al().alBufferData(buffer, format, m_data.data() + from, static_cast<ALsizei>(to - from),
			static_cast<ALsizei>(m_sampleRate));
		if (!CHECK_AL_ERROR()) {
			return false;
		}
		m_uploadedBytes += to - from;
		return true;
	};
	if (m_startBuffer && !upload(m_startBuffer, 0, startBytes)) {
		return false;
	}
	if (m_loopBuffer && !upload(m_loopBuffer, startBytes, loopEndBytes)) {
		return false;
	}
	if (m_endBuffer && !upload(m_endBuffer, loopEndBytes, totalBytes)) {
		return false;
	}
	soundFileStats().staticBytes += m_uploadedBytes;
	m_data.clear();
	m_data.shrink_to_fit();
	return true;
}

void SoundFile::deleteBuffers()
{
	if (m_kind == Kind::Streaming) {
		return;
	}
	for (ALuint* buffer : {&m_startBuffer, &m_loopBuffer, &m_endBuffer}) {
		if (*buffer) {
			al().alDeleteBuffers(1, buffer);
			--soundFileStats().buffers;
			*buffer = 0;
		}
	}
	if (m_loaded) {
		soundFileStats().staticBytes -= m_uploadedBytes;
		m_uploadedBytes = 0;
	}
}

void SoundFile::unload(bool force, bool forceStreaming)
{
	if (!m_loaded) {
		return;
	}
	if (m_kind == Kind::Streaming) {
		force = forceStreaming;
	}
	if (!inUse() || force) {
		--soundFileStats().loadedFiles;
		reset();
	}
}

void SoundFile::reset()
{
	if (m_kind == Kind::Streaming && !m_data.empty()) {
		soundFileStats().streamingBytes -= static_cast<uint32_t>(m_data.size());
	}
	m_data.clear();
	if (openALLoaded()) {
		deleteBuffers();
	}
	m_loaded = false;
	m_kind = Kind::Unloaded;
	m_sampleRate = m_frames = m_bytesPerFrame = 0;
	m_channels = 0;
	m_loopStart = 0;
	m_loopEnd = -1;
}

} // namespace dice::hfe::sound
