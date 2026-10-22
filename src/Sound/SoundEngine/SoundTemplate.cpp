#include "SoundTemplate.h"

#include "BfSoundEngine.h"
#include "Sound.h"
#include "SoundServices.h"
#include "SoundTweaks.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::sound {

namespace {

// Converts a template's "half volume distance" into an OpenAL rolloff factor
// for the inverse-distance-clamped model, then derives the distance at which
// the sound drops below max-distance-cutoff and can be culled.
constexpr float kHalfVolumeGain = 0.511872f;

std::unique_ptr<SoundEnvelope> makeEnvelope(float minX, float maxX, float minY, float maxY, bool randomized,
	int pointCount, const float* points)
{
	if (pointCount == 0) {
		return nullptr;
	}
	return std::make_unique<SoundEnvelope>(minX, maxX, minY, maxY, randomized,
		reinterpret_cast<const EnvelopePoint*>(points), pointCount);
}

const float* describeEnvelope(const SoundEnvelope* envelope, float* minX, float* maxX, float* minY, float* maxY,
	bool* randomized, int* pointCount)
{
	if (!envelope) {
		return nullptr;
	}
	if (minX) *minX = envelope->minX();
	if (maxX) *maxX = envelope->maxX();
	if (minY) *minY = envelope->minY();
	if (maxY) *maxY = envelope->maxY();
	if (randomized) *randomized = envelope->randomized();
	if (pointCount) *pointCount = static_cast<int>(envelope->points().size());
	return envelope->points().empty() ? nullptr : &envelope->points()[0].x;
}

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

} // namespace

SoundTemplate::SoundTemplate()
{
	updateRolloff();
}

SoundTemplate::~SoundTemplate()
{
	clearSoundFiles();
}

int SoundTemplate::release()
{
	if (--m_refCount < 1) {
		destroy(1);
		return 0;
	}
	return m_refCount;
}

void* SoundTemplate::destroy(unsigned flags)
{
	if (flags & 1) {
		delete this;
		return nullptr;
	}
	this->~SoundTemplate();
	return this;
}

void SoundTemplate::setSoundFilename(const char* path)
{
	if (path) {
		setSoundFilenames(&path, 1);
	}
	else {
		setSoundFilenames(nullptr, 0);
	}
}

const char* SoundTemplate::soundFilename()
{
	return m_filenamePointers.empty() ? "" : m_filenamePointers[0];
}

void SoundTemplate::setSoundFilenames(const char* const* paths, int count)
{
	clearSoundFiles();
	for (int i = 0; i < count; ++i) {
		SoundFile* file = nullptr;
		if (paths[i] && *paths[i]) {
			file = soundEngine().acquireSoundFile(paths[i]);
			if (!file) {
				SND_WARNING("Failed to create soundFile for: %s", paths[i]);
			}
			else {
				// lowSamples/mediumSamples limit how many variations load per quality.
				if (i < m_sampleCounts[0]) file->qualityFlags() |= SoundFile::EnabledAtHigh;
				if (i < m_sampleCounts[1]) file->qualityFlags() |= SoundFile::EnabledAtMedium;
				if (i < m_sampleCounts[2]) file->qualityFlags() |= SoundFile::EnabledAtLow;
				if (m_flags != 0) file->qualityFlags() |= SoundFile::KeepVoiceQuality;
			}
		}
		m_files.push_back(file);
		m_filenames.emplace_back(file ? file->path() : std::string());
	}
	m_filenamePointers.clear();
	for (size_t i = 0; i < m_files.size(); ++i) {
		m_filenamePointers.push_back(m_files[i] ? m_filenames[i].c_str() : nullptr);
	}
}

const char* const* SoundTemplate::soundFilenames(int* count)
{
	if (count) {
		*count = static_cast<int>(m_filenamePointers.size());
	}
	return m_filenamePointers.empty() ? nullptr : m_filenamePointers.data();
}

void SoundTemplate::clearSoundFiles()
{
	for (SoundFile* file : m_files) {
		if (file) {
			file->release();
		}
	}
	m_files.clear();
	m_filenames.clear();
	m_filenamePointers.clear();
}

void SoundTemplate::setVolume(float volume)
{
	m_volume = clamp01(volume);
	updateRolloff();
}

void SoundTemplate::setVolumeEnvelope(float minX, float maxX, float minY, float maxY, bool randomized,
	int pointCount, const float* points)
{
	m_volumeEnvelope = makeEnvelope(minX, maxX, minY, maxY, randomized, pointCount, points);
}

const float* SoundTemplate::volumeEnvelope(float* minX, float* maxX, float* minY, float* maxY, bool* randomized,
	int* pointCount)
{
	return describeEnvelope(m_volumeEnvelope.get(), minX, maxX, minY, maxY, randomized, pointCount);
}

void SoundTemplate::setPitchEnvelope(float minX, float maxX, float minY, float maxY, bool randomized,
	int pointCount, const float* points)
{
	m_pitchEnvelope = makeEnvelope(minX, maxX, minY, maxY, randomized, pointCount, points);
}

const float* SoundTemplate::pitchEnvelope(float* minX, float* maxX, float* minY, float* maxY, bool* randomized,
	int* pointCount)
{
	return describeEnvelope(m_pitchEnvelope.get(), minX, maxX, minY, maxY, randomized, pointCount);
}

void SoundTemplate::setVolumeGroup(int group)
{
	if (group >= 0 && group < 8) {
		m_volumeGroup = group;
	}
}

void SoundTemplate::setPan(float pan) { m_pan = clamp01(pan); }
void SoundTemplate::setReverbLevel(float level) { m_reverbLevel = clamp01(level); }
void SoundTemplate::setStopType(int type) { m_stopType = std::clamp(type, 0, 2); }
void SoundTemplate::setConeInnerAngle(float degrees) { m_coneInnerAngle = std::clamp(degrees, 0.0f, 360.0f); }
void SoundTemplate::setConeOuterAngle(float degrees) { m_coneOuterAngle = std::clamp(degrees, 0.0f, 360.0f); }
void SoundTemplate::setConeOuterVolume(float volume) { m_coneOuterVolume = clamp01(volume); }
void SoundTemplate::setOcclusionFactor(float factor) { m_occlusionFactor = clamp01(factor); }

void SoundTemplate::setMinDistance(float distance)
{
	m_minDistance = distance;
	updateRolloff();
}

void SoundTemplate::setHalfVolumeDistance(float distance)
{
	m_halfVolumeDistance = distance;
	updateRolloff();
}

void SoundTemplate::setDirection(const Vec3* direction)
{
	m_direction = direction->lengthSquared() > 0.01f ? normalized(*direction) : Vec3();
}

void SoundTemplate::updateRolloff()
{
	if (m_minDistance < 0.01f) {
		m_minDistance = 0.01f;
	}
	m_rolloffFactor = 1.0f;
	if (m_halfVolumeDistance <= m_minDistance + 0.01f) {
		m_halfVolumeDistance = 0.0f;
	}
	else {
		const float gain = m_volume * kHalfVolumeGain;
		if (gain > 1.1920929e-07f) {
			m_rolloffFactor = (1.0f / gain - 1.0f) * (m_minDistance / (m_halfVolumeDistance - m_minDistance));
		}
	}
	const float cutoff = soundTweaks().get(Tweak::MaxDistanceCutoff);
	m_maxDistance = ((1.0f / (cutoff * m_rolloffFactor) - 1.0f / m_rolloffFactor) + 1.0f) * m_minDistance;
}

void SoundTemplate::markSoundFilesUsed()
{
	for (SoundFile* file : m_files) {
		if (file) {
			file->addUsage();
		}
	}
}

int SoundTemplate::usableSampleCount()
{
	const int quality = soundEngine().qualityLevel();
	return std::min(static_cast<int>(m_files.size()), m_sampleCounts[quality]);
}

SoundFile* SoundTemplate::soundFile(int index)
{
	if (index < 0 || index >= usableSampleCount()) {
		return nullptr;
	}
	return m_files[index];
}

void* SoundTemplate::loadedSoundFile(int index)
{
	if (soundEngine().editorMode() || (m_flags & LoadOnDemand)) {
		SoundFile* file = soundFile(index);
		if (!file) {
			return nullptr;
		}
		if (!file->loaded()) {
			file->load(soundEngine().qualityLevel());
			if (!file->loaded()) {
				return nullptr;
			}
		}
	}
	return soundFile(index);
}

int SoundTemplate::sampleDurationTicks(int index)
{
	SoundFile* file = m_files[index];
	if (!file) {
		return 0;
	}
	if (m_loopCount == 0) {
		return INT_MAX;
	}
	// The original scales by pitch rather than dividing by it.
	const float frames = static_cast<float>(file->frames()) * static_cast<float>(m_loopCount);
	return static_cast<int>(std::lround(frames * (30.0f / static_cast<float>(file->sampleRate())) * m_pitch));
}

void SoundTemplate::loadSoundFiles(int qualityLevel)
{
	for (SoundFile* file : m_files) {
		if (file) {
			file->load(qualityLevel);
		}
	}
}

void SoundTemplate::unloadSoundFiles(bool force, bool forceStreaming)
{
	for (SoundFile* file : m_files) {
		if (file) {
			file->unload(force, forceStreaming);
		}
	}
}

ISound* SoundTemplate::createSound()
{
	auto* sound = new Sound(this);
	sound->addRef();
	return sound;
}

} // namespace dice::hfe::sound
