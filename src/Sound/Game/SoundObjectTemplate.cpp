#include "SoundObjectTemplate.h"

#include <cmath>
#include <limits>
#include <sstream>
#include <vector>

namespace dice::hfe::sound {

namespace {

// Splits "a,b,c" (0x008001f0).
std::vector<std::string> splitList(const std::string& list)
{
	std::vector<std::string> items;
	size_t start = 0;
	while (start <= list.size()) {
		const size_t comma = list.find(',', start);
		items.push_back(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
		if (comma == std::string::npos || comma + 1 == list.size()) {
			break;
		}
		start = comma + 1;
	}
	return items;
}

struct Envelope {
	float minX = 0, maxX = 0, minY = 0, maxY = 0;
	bool randomized = false;
	unsigned count = 0;
	std::vector<float> points; // x, yLow, yHigh per point
};

// 0x007fe9c0 and 0x00800430: numbers separated by any one character.
bool parseEnvelope(const std::string& text, Envelope& envelope)
{
	std::istringstream in(text);
	char separator;
	in >> envelope.minX >> separator >> envelope.maxX >> separator >> envelope.minY >> separator >> envelope.maxY >>
		separator >> envelope.randomized >> separator >> envelope.count >> separator;
	envelope.points.assign(static_cast<size_t>(envelope.count) * 3, 0.0f);
	for (float& value : envelope.points) {
		in >> value >> separator;
	}
	return !in.fail() || envelope.count == 0;
}

using SetEnvelope = void (ISoundTemplate::*)(float, float, float, float, bool, int, const float*);

void applyEnvelope(ISoundTemplate* sound, SetEnvelope set, const std::string& text)
{
	Envelope envelope;
	const bool ok = parseEnvelope(text, envelope);
	if (!sound || (!ok && envelope.count > 0)) {
		return; // the original drops envelopes whose points don't parse
	}
	(sound->*set)(envelope.minX, envelope.maxX, envelope.minY, envelope.maxY, envelope.randomized,
		static_cast<int>(envelope.count), envelope.points.empty() ? nullptr : envelope.points.data());
}

bool isZero(float v) { return std::fabs(v) <= std::numeric_limits<float>::epsilon(); }

} // namespace

SoundObjectTemplate::SoundObjectTemplate(ISoundEngine* engine, uint32_t classId, std::string typeName,
	std::string name, uint32_t id)
	: ObjectTemplate(classId, std::move(typeName), std::move(name), id)
{
	if (engine) {
		m_sound = engine->createTemplate();
	}
	if (m_sound) {
		m_volume = m_sound->volume();
		m_pitch = m_sound->pitch();
		m_minDistance = m_sound->minDistance();
		m_halfVolumeDistance = m_sound->halfVolumeDistance();
		m_reverbLevel = m_sound->reverbLevel();
		m_soundRadius = m_sound->soundRadius();
		m_titanAmbient = m_sound->isTitanInteriorAmbient();
	}
	setFlags(flags() | world::TemplateFlags::ClientOnly);
}

SoundObjectTemplate::~SoundObjectTemplate()
{
	if (m_sound) {
		m_sound->release();
	}
}

void SoundObjectTemplate::setSoundFilenames(const std::string& list)
{
	m_filenameText = list;
	if (!m_sound) {
		return;
	}
	const std::vector<std::string> files = splitList(list);
	std::vector<const char*> pointers;
	for (const std::string& file : files) {
		pointers.push_back(file.c_str());
	}
	m_sound->setSoundFilenames(pointers.data(), static_cast<int>(pointers.size()));
}

std::string SoundObjectTemplate::soundFilenames() const
{
	if (!m_sound) {
		return m_filenameText;
	}
	int count = 0;
	const char* const* files = m_sound->soundFilenames(&count);
	std::string list;
	for (int i = 0; i < count; ++i) {
		if (files[i]) {
			list += files[i];
		}
		if (i != count - 1) {
			list += ',';
		}
	}
	return list;
}

void SoundObjectTemplate::setVolumeEnvelope(const std::string& text)
{
	m_volumeEnvelopeText = text;
	applyEnvelope(m_sound, &ISoundTemplate::setVolumeEnvelope, text);
}

std::string SoundObjectTemplate::volumeEnvelope() const
{
	return m_volumeEnvelopeText;
}

void SoundObjectTemplate::setPitchEnvelope(const std::string& text)
{
	m_pitchEnvelopeText = text;
	applyEnvelope(m_sound, &ISoundTemplate::setPitchEnvelope, text);
}

std::string SoundObjectTemplate::pitchEnvelope() const
{
	return m_pitchEnvelopeText;
}

void SoundObjectTemplate::setVolume(float volume)
{
	m_volume = volume;
	if (m_sound) {
		m_sound->setVolume(volume * m_tweaks[VolumeTweak]);
	}
}

void SoundObjectTemplate::setPitch(float pitch)
{
	m_pitch = pitch;
	if (m_sound) {
		m_sound->setPitch(pitch * m_tweaks[PitchTweak]);
	}
}

void SoundObjectTemplate::setMinDistance(float distance)
{
	m_minDistance = distance;
	if (m_sound) {
		m_sound->setMinDistance(distance * m_tweaks[MinDistanceTweak]);
	}
}

void SoundObjectTemplate::setHalfVolumeDistance(float distance)
{
	m_halfVolumeDistance = distance;
	float applied = distance;
	if (isZero(distance)) {
		applied = 2 * m_minDistance * m_tweaks[MinDistanceTweak];
	}
	if (m_sound) {
		m_sound->setHalfVolumeDistance(applied * m_tweaks[HalfVolumeTweak]);
	}
}

void SoundObjectTemplate::setReverbLevel(float level)
{
	m_reverbLevel = level;
	if (m_sound) {
		m_sound->setReverbLevel(level * m_tweaks[ReverbTweak]);
	}
}

void SoundObjectTemplate::setSoundRadius(float radius)
{
	m_soundRadius = radius;
	if (m_sound) {
		m_sound->setSoundRadius(radius * m_tweaks[RadiusTweak]);
	}
}

void SoundObjectTemplate::setTitanAmbient(bool interior)
{
	m_titanAmbient = interior;
	if (m_sound) {
		m_sound->setTitanInteriorAmbient(interior);
	}
}

void SoundObjectTemplate::setTweaks(const std::array<float, 6>& multipliers)
{
	m_tweaks = multipliers;
	setVolume(m_volume);
	setPitch(m_pitch);
	setMinDistance(m_minDistance);
	setHalfVolumeDistance(m_halfVolumeDistance);
	setReverbLevel(m_reverbLevel);
	setSoundRadius(m_soundRadius);
}

} // namespace dice::hfe::sound
