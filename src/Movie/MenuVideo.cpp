#include "MenuVideo.h"

namespace dice::hfe::movie {

std::string menuVideoFile(const std::string& url)
{
	std::string file = url.empty() || url[0] != '?' ? url : url.substr(1);
	for (char& c : file) {
		if (c == '\\') {
			c = '/';
		}
	}
	if (!file.empty() && file[0] == '/') {
		return file.substr(1);
	}
	return "Movies/" + file;
}

MenuVideo::~MenuVideo()
{
	if (m_sink.stop) {
		m_sink.stop();
	}
}

void MenuVideo::playAudio()
{
	std::vector<float> samples = m_decoder.takeAudio();
	if (!samples.empty() && m_sink.play) {
		m_sink.play(samples, m_decoder.sampleRate());
	}
}

bool MenuVideo::open(const std::string& path, std::string* error)
{
	if (!m_decoder.open(path, error)) {
		return false;
	}
	m_haveNext = m_decoder.nextFrame(m_next);
	m_finished = !m_haveNext;
	return m_haveNext;
}

bool MenuVideo::advance(double seconds, ui::MenuImage& frame)
{
	if (!m_haveNext) {
		m_finished = true;
		return false;
	}
	if (m_started) {
		m_time += seconds;
	}
	bool changed = false;
	// The newest picture due by now (the first at once).
	while (m_haveNext && (!m_started || m_next.time <= m_time)) {
		m_started = true;
		frame.width = static_cast<uint32_t>(m_next.width);
		frame.height = static_cast<uint32_t>(m_next.height);
		frame.rgba = m_next.rgba;
		changed = true;
		m_haveNext = m_decoder.nextFrame(m_next);
		playAudio();
	}
	if (!m_haveNext) {
		m_finished = true;
	}
	return changed;
}

} // namespace dice::hfe::movie
