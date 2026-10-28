// The movie queue (bf2Engine.playMovie, Movie/MovieQueue.h) and, with
// FFmpeg and $BF2142_GAME_DIR, the Bink decoder on the shipped movies.
#include "TestMain.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Movie/MenuVideo.h"
#include "Movie/MovieDecoder.h"
#include "Movie/MovieQueue.h"

#include <cmath>
#include <cstdlib>
#include <string>

using namespace dice::hfe;

TEST(Movie_QueueAsInitConPlaysThem)
{
	// Init.con's lines through the console, as runGame binds them.
	movie::MovieQueue queue;
	queue.northAmerica = true;
	Console console;
	bindMethod(console, "bf2Engine.playMovie", ConsoleAccess::Anyone,
		[&queue](std::string file, int mode) { queue.playMovie(file, mode); });
	std::string output;
	console.executeScript("bf2Engine.playMovie Movies\\EA.bik 1\n"
						  "bf2Engine.playMovie Movies\\dice.bik 1\n"
						  "bf2Engine.playMovie Movies\\legal.bik 1\n"
						  "bf2Engine.playMovie Movies\\intro.bik 0\n",
		output);
	CHECK(queue.size() == 4);
	const movie::QueuedMovie ea = queue.takeNext();
	CHECK(ea.file == "Movies\\EA.bik" && !ea.skippable());
	queue.takeNext();
	const movie::QueuedMovie legal = queue.takeNext();
	CHECK(legal.file == "Movies\\legal_na.bik" && !legal.skippable()); // 0x00403230's locale
	const movie::QueuedMovie intro = queue.takeNext();
	CHECK(intro.file == "Movies\\intro.bik" && intro.skippable());
	CHECK(queue.empty() && queue.takeNext().file.empty());

	movie::MovieQueue elsewhere;
	elsewhere.playMovie("Movies\\legal.bik", 1);
	CHECK(elsewhere.takeNext().file == "Movies\\legal.bik");
	movie::MovieQueue server;
	server.dedicated = true; // "These should be noops in server & editor"
	server.playMovie("Movies\\intro.bik", 0);
	CHECK(server.empty());
}

TEST(Movie_DecodesTheShippedBinkMovies)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir || !movie::MovieDecoder::available()) {
		return;
	}
	const std::string movies = std::string(gameDir) + "/mods/bf2142/Movies/";
	movie::MovieDecoder dice;
	std::string error;
	CHECK(dice.open(movies + "Dice.bik", &error));
	CHECK(dice.width() == 640 && dice.height() == 480 && std::abs(dice.frameRate() - 30) < 0.01);
	CHECK(dice.hasAudio() && dice.sampleRate() == 44100);
	movie::VideoFrame frame;
	int frames = 0;
	size_t samples = 0;
	double last = -1;
	bool ordered = true, lit = false;
	while (dice.nextFrame(frame)) {
		ordered = ordered && frame.time > last;
		last = frame.time;
		++frames;
		samples += dice.takeAudio().size() / 2;
		if (frames == 90) {
			CHECK(frame.width == 640 && frame.height == 480 && frame.rgba.size() == 640u * 480u * 4u);
			for (size_t i = 0; i < frame.rgba.size(); i += 4) {
				lit = lit || frame.rgba[i] + frame.rgba[i + 1] + frame.rgba[i + 2] > 60;
			}
		}
	}
	samples += dice.takeAudio().size() / 2;
	CHECK(ordered && lit);
	// 6.23 s at 30 fps, and its sound as long.
	CHECK(frames >= 180 && frames <= 190);
	CHECK(samples > 44100 * 6 && samples < 44100 * 7);
	// The legal screen has no sound; a missing movie doesn't open.
	movie::MovieDecoder legal;
	CHECK(legal.open(movies + "Legal.bik") && !legal.hasAudio() && legal.width() == 800 && legal.height() == 600);
	movie::MovieDecoder missing;
	CHECK(!missing.open(movies + "health.bik", &error) && !error.empty());
}

// The menu's movies in clips (Movie/MenuVideo.h): the URL's file, and the
// shipped Titan tutorial's pictures by time.
TEST(Movie_MenuVideo)
{
	CHECK(movie::menuVideoFile("?titan_tutorial.bik") == "Movies/titan_tutorial.bik");
	CHECK(movie::menuVideoFile("?/Levels/Suez_Canal/Info/gpm_cq_32_menuMovie.bik") ==
		"Levels/Suez_Canal/Info/gpm_cq_32_menuMovie.bik");
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir || !movie::MovieDecoder::available()) {
		return;
	}
	movie::MenuVideo video;
	CHECK(video.open(std::string(gameDir) + "/mods/bf2142/" + movie::menuVideoFile("?titan_tutorial.bik")));
	ui::MenuImage frame;
	CHECK(video.advance(0, frame) && frame.width > 0 && frame.rgba.size() == frame.width * frame.height * 4u);
	CHECK(!video.advance(0.001, frame)); // the next not due yet
	CHECK(video.advance(0.5, frame));
	CHECK(!video.finished());
	for (int i = 0; i < 2000 && !video.finished(); ++i) {
		video.advance(1.0, frame);
	}
	CHECK(video.finished());
}
