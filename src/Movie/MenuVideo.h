// A Bink movie shown inside a Flash menu clip (ui::VideoSource): the
// menu's Titan tutorial ("?titan_tutorial.bik", loaded by the LAUNCH button
// of the Titan tips page into mcTutorial with loadImg) and the map movies
// (Multiplay.getMapMovie). SwiffPlayer draws such a movie as a video
// texture (RendDX9's Video/VideoTarget); here its pictures replace the
// clip's image as time passes, in step with the movie's own clock. Its
// sound goes to the host's sink as it is decoded (Bink plays a menu movie's
// sound itself, RendDX9 0x1011e0d0: BinkOpen(path, 0)), and what is still
// queued stops when the clip lets the movie go (unloadMovie).
#pragma once

#include "Movie/MovieDecoder.h"
#include "UI/MenuDraw.h"

#include <functional>
#include <string>
#include <vector>

namespace dice::hfe::movie {

// The file a menu URL names, relative to the mod's folder: "?name.bik" is
// in Movies/ (titan_tutorial.bik ships there), "?/path" is from the mod's
// root ("?/Levels/<level>/Info/<mode>_<size>_menuMovie.bik").
std::string menuVideoFile(const std::string& url);

class MenuVideo : public ui::VideoSource {
public:
	// Where the movie's sound goes: interleaved stereo floats at a rate,
	// and `stop` when the movie is closed.
	struct AudioSink {
		std::function<void(const std::vector<float>& samples, int rate)> play;
		std::function<void()> stop;
	};
	MenuVideo() = default;
	explicit MenuVideo(AudioSink sink) : m_sink(std::move(sink)) {}
	~MenuVideo() override;
	MenuVideo(const MenuVideo&) = delete;
	MenuVideo& operator=(const MenuVideo&) = delete;

	bool open(const std::string& path, std::string* error = nullptr);
	bool advance(double seconds, ui::MenuImage& frame) override;
	bool finished() const override { return m_finished; }

private:
	void playAudio();
	AudioSink m_sink;
	MovieDecoder m_decoder;
	VideoFrame m_next;
	bool m_haveNext = false, m_finished = false, m_started = false;
	double m_time = 0;
};

} // namespace dice::hfe::movie
