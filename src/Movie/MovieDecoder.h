// A Bink movie's frames and sound (the game's Movies/*.bik: Bink video
// "BIKi" at 640x480 or 800x600, 30 fps, Bink DCT audio at 44.1 or 48 kHz).
//
// BF2142.exe plays them through binkw32.dll (RendDX9's FSMoviePlayer);
// the rewrite decodes them with FFmpeg's Bink decoders (libavformat's bink
// demuxer, libavcodec's binkvideo and binkaudio_dct), converted to RGBA
// and interleaved float samples. Without FFmpeg (BF2142_MOVIES off, or not
// found) open() always fails and the movies are skipped, as the original
// skips a movie that won't open.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::movie {

struct VideoFrame {
	int width = 0, height = 0;
	std::vector<uint8_t> rgba; // width * height * 4, top row first
	double time = 0;           // seconds from the start
};

class MovieDecoder {
public:
	MovieDecoder();
	~MovieDecoder();
	MovieDecoder(const MovieDecoder&) = delete;
	MovieDecoder& operator=(const MovieDecoder&) = delete;

	// Whether this build decodes movies at all.
	static bool available();

	bool open(const std::string& path, std::string* error = nullptr);
	void close();
	bool isOpen() const;
	int width() const;
	int height() const;
	double frameRate() const;
	double duration() const; // seconds, 0 when unknown
	bool hasAudio() const;
	int sampleRate() const;  // of takeAudio's samples
	int channels() const { return 2; }

	// The next video frame (false at the end). Audio decoded on the way
	// is kept for takeAudio.
	bool nextFrame(VideoFrame& out);
	// The audio decoded so far, interleaved stereo floats.
	std::vector<float> takeAudio();

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace dice::hfe::movie
