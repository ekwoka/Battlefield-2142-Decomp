#include "Reverb.h"

#include "ISoundProvider.h"
#include "SoundServices.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace dice::hfe::sound {

namespace {

// Preset files are tiny; the original refused anything above 2 KiB.
constexpr long kMaxPresetSize = 0x800;

class LineReader {
public:
	explicit LineReader(std::string text) : m_text(std::move(text)) {}

	const char* next()
	{
		while (m_pos < m_text.size() && m_text[m_pos] == '\n') {
			++m_pos;
		}
		if (m_pos >= m_text.size()) {
			return "";
		}
		const size_t end = std::min(m_text.find('\n', m_pos), m_text.size());
		m_line = m_text.substr(m_pos, end - m_pos);
		m_pos = end;
		return m_line.c_str();
	}
	void skip() { next(); }
	int32_t integer() { return static_cast<int32_t>(std::atol(next())); }
	float real() { return static_cast<float>(std::atof(next())); }

private:
	std::string m_text;
	std::string m_line;
	size_t m_pos = 0;
};

} // namespace

float gainToMillibels(float gain)
{
	return gain > 1e-5f ? 2000.0f * std::log10(gain) : -10000.0f;
}

bool Reverb::load(const char* presetFile)
{
	ISoundFileSystem& files = soundFiles();
	void* file = files.open(presetFile);
	if (!file) {
		SND_WARNING("Reverb preset file %s not found.", presetFile);
		return false;
	}
	const long size = files.seek(file, 0, SEEK_END);
	files.seek(file, 0, SEEK_SET);
	if (size < 0 || size >= kMaxPresetSize) {
		files.close(file);
		return false;
	}
	std::string text(static_cast<size_t>(size), '\0');
	files.read(file, text.data(), text.size());
	files.close(file);
	text += '\n';

	// Line 1 is the title; every value is preceded by its label.
	LineReader lines(text);
	EaxListenerProperties& p = m_preset;
	lines.skip();
	lines.skip(); p.environment = static_cast<uint32_t>(lines.integer());
	lines.skip(); p.environmentSize = lines.real();
	lines.skip(); p.environmentDiffusion = lines.real();
	lines.skip(); p.room = lines.integer();
	lines.skip(); p.roomHF = lines.integer();
	lines.skip(); p.roomLF = lines.integer();
	lines.skip(); p.decayTime = lines.real();
	lines.skip(); p.decayHFRatio = lines.real();
	lines.skip(); p.decayLFRatio = lines.real();
	lines.skip(); p.reflections = lines.integer();
	lines.skip(); p.reflectionsDelay = lines.real();
	lines.skip();
	p.reflectionsPan[0] = lines.real();
	p.reflectionsPan[1] = lines.real();
	p.reflectionsPan[2] = lines.real();
	lines.skip(); p.reverb = lines.integer();
	lines.skip(); p.reverbDelay = lines.real();
	lines.skip();
	p.reverbPan[0] = lines.real();
	p.reverbPan[1] = lines.real();
	p.reverbPan[2] = lines.real();
	lines.skip(); p.echoTime = lines.real();
	lines.skip(); p.echoDepth = lines.real();
	lines.skip(); p.modulationTime = lines.real();
	lines.skip(); p.modulationDepth = lines.real();
	lines.skip(); p.airAbsorptionHF = lines.real();
	lines.skip(); p.hfReference = lines.real();
	lines.skip(); p.lfReference = lines.real();
	lines.skip(); p.roomRolloffFactor = lines.real();
	lines.skip(); p.flags = static_cast<uint32_t>(lines.integer());

	m_current = m_preset;
	m_dirty = true;
	return true;
}

void Reverb::setRoomGain(float gain)
{
	const int32_t room =
		std::clamp(static_cast<int32_t>(std::lround(gainToMillibels(gain))) + m_preset.room, -10000, 0);
	if (room != m_current.room) {
		m_current.room = room;
		m_dirty = true;
	}
}

void Reverb::apply(ISoundProvider& provider)
{
	if (m_dirty) {
		provider.setReverbProperties(&m_current);
		m_dirty = false;
	}
}

} // namespace dice::hfe::sound
