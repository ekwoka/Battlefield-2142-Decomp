// Runs real BF2142 sound files through the engine. Needs extracted game
// sounds in $BF2142_SOUND_TEST_DIR (skipped otherwise) and plays them on
// OpenAL Soft's silent "null" driver.
#include "Sound/SoundEngine/ISoundEngine.h"
#include "Sound/SoundEngine/Music.h"
#include "Sound/SoundEngine/SoundEnvelope.h"
#include "Sound/SoundEngine/OpenALSample.h"
#include "Sound/SoundEngine/Sound.h"
#include "Sound/SoundEngine/SoundFile.h"
#include "Sound/SoundEngine/WavFileFormat.h"
#include "TestMain.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace snd = dice::hfe::sound;

namespace {

const char* testDir() { return std::getenv("BF2142_SOUND_TEST_DIR"); }

std::string path(const char* relative) { return std::string(testDir()) + "/" + relative; }

} // namespace

TEST(Music_ClearingTrackCannotRestartPreviousSong)
{
	if (!testDir()) {
		return;
	}
#ifdef _WIN32
	_putenv_s("ALSOFT_DRIVERS", "null");
#else
	setenv("ALSOFT_DRIVERS", "null", 1);
#endif
	auto* engine = snd::getSoundEngine();
	CHECK(engine->initialize(0, "Software Audio", 16) == 0);
	const std::string song = path("Music/2142MenuMusic.ogg");
	CHECK(snd::music::playTrack(song.c_str()));
	CHECK(snd::music::start());
	CHECK(snd::music::playTrack(nullptr));
	CHECK(!snd::music::start());
	snd::music::stop(0);
	engine->shutdown();
	snd::shutdownSoundEngine();
}

TEST(Music_ReplacingTrackStopsPreviousPlayer)
{
	if (!testDir()) {
		return;
	}
#ifdef _WIN32
	_putenv_s("ALSOFT_DRIVERS", "null");
#else
	setenv("ALSOFT_DRIVERS", "null", 1);
#endif
	auto* engine = snd::getSoundEngine();
	CHECK(engine->initialize(0, "Software Audio", 16) == 0);
	CHECK(snd::music::playTrack(path("Music/2142MenuMusic.ogg").c_str()));
	CHECK(snd::music::start());
	CHECK(snd::music::setTrack(path("Ambient/Cerbere.ogg").c_str()));
	CHECK(!snd::music::stop(0)); // selecting another track must have joined the old player
	CHECK(snd::music::start());
	CHECK(snd::music::playTrack(nullptr));
	engine->shutdown();
	snd::shutdownSoundEngine();
}

TEST(Wav_LoopPointsFromCueAndLabel)
{
	if (!testDir()) {
		return;
	}
	snd::WavData wav;
	CHECK(snd::readWavFile(path("Effects/Grenade_fuse_loop.wav").c_str(), wav));
	CHECK(wav.channels >= 1 && wav.sampleRate > 0);
	CHECK(wav.hasLoop);
	CHECK(wav.loopStart >= 0 && wav.loopStart < wav.loopEnd);
	CHECK(wav.loopEnd <= static_cast<int32_t>(wav.frames) + 1);

	snd::WavData plain;
	CHECK(snd::readWavFile(path("Cockpits/Missile_Warning.wav").c_str(), plain));
	CHECK(!plain.hasLoop && plain.loopEnd == static_cast<int32_t>(plain.frames));
}

// A synthetic WAV with a cue point and an "adtl ltxt" region: 1000 frames of
// start block, a 2000 frame loop block, 3000 frames of end block.
TEST(Wav_LoopRegionSplitsStartLoopAndEndBlocks)
{
	const auto u32 = [](std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); };
	const auto tag = [](std::vector<uint8_t>& v, const char* t) { v.insert(v.end(), t, t + 4); };
	std::vector<uint8_t> riff;
	tag(riff, "WAVE");
	tag(riff, "fmt "); u32(riff, 16);
	riff.insert(riff.end(), {1, 0, 1, 0}); u32(riff, 44100); u32(riff, 88200); riff.insert(riff.end(), {2, 0, 16, 0});
	tag(riff, "cue "); u32(riff, 28); u32(riff, 1); u32(riff, 1); u32(riff, 1000); tag(riff, "data"); u32(riff, 0); u32(riff, 0); u32(riff, 1000);
	tag(riff, "LIST"); u32(riff, 32); tag(riff, "adtl"); tag(riff, "ltxt"); u32(riff, 20); u32(riff, 1); u32(riff, 2000);
	tag(riff, "rgn "); u32(riff, 0); u32(riff, 0);
	tag(riff, "data"); u32(riff, 6000 * 2); riff.resize(riff.size() + 6000 * 2);
	std::vector<uint8_t> file;
	tag(file, "RIFF"); u32(file, uint32_t(riff.size())); file.insert(file.end(), riff.begin(), riff.end());
	const std::string tmp = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/bf2142_loop_region.wav";
	FILE* f = std::fopen(tmp.c_str(), "wb");
	CHECK(f != nullptr);
	if (!f) {
		return;
	}
	std::fwrite(file.data(), 1, file.size(), f);
	std::fclose(f);
	snd::WavData wav;
	CHECK(snd::readWavFile(tmp.c_str(), wav));
	std::remove(tmp.c_str());
	CHECK(wav.hasLoop && wav.loopStart == 1000 && wav.loopEnd == 3000 && wav.frames == 6000);
}

// The walker cannon (as_heavy_mech main cannon, loopCount 0, stopType 1): a
// 0.15 s loop block followed by a 1.4 s tail. Held fire loops the block
// (the whole file lasts 1.5 s) and the release plays the tail out.
TEST(SoundEngine_WalkerCannonLoopsBlockAndPlaysTailOnRelease)
{
	if (!testDir()) {
		return;
	}
	snd::WavData wav;
	CHECK(snd::readWavFile(path("Weapons/300rpm_medium_cannon/300rpm_medium_cannon_3p.wav").c_str(), wav));
	CHECK(wav.hasLoop && wav.loopStart == 0 && wav.loopEnd > 0 && wav.loopEnd < int32_t(wav.frames) / 4);

	setenv("ALSOFT_DRIVERS", "null", 1);
	snd::ISoundEngine* engine = snd::getSoundEngine();
	CHECK(engine->initialize(0, "Software Audio", 16) == 0);
	snd::ISoundTemplate* templ = engine->createTemplate();
	templ->setSoundFilename(path("Weapons/300rpm_medium_cannon/300rpm_medium_cannon_1p.wav").c_str());
	templ->setIs3dSound(false);
	templ->setLoopCount(0);
	templ->setStopType(1);
	snd::ISound* gun = templ->createSound();
	engine->reloadSoundFiles();
	const auto run = [&](int ms) {
		for (int t = 0; t < ms; t += 20) {
			engine->update(1);
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
	};
	gun->playLooped(0);
	run(2200); // longer than the whole file
	CHECK(gun->isPlaying());
	gun->stop();
	run(400);
	CHECK(gun->isPlaying()); // the tail, not a cut
	run(2400);
	CHECK(!gun->isPlaying());
	gun->release();
	templ->release();
	engine->shutdown();
	snd::shutdownSoundEngine();
}

TEST(SoundEngine_PlaysWavAndOgg)
{
	if (!testDir()) {
		return;
	}
#ifdef _WIN32
	_putenv_s("ALSOFT_DRIVERS", "null");
#else
	setenv("ALSOFT_DRIVERS", "null", 1);
#endif
	snd::ISoundEngine* engine = snd::getSoundEngine();
	CHECK(engine->initialize(0, "Software Audio", 16) == 0);
	CHECK(engine->isInitialized());

	const snd::Vec3 listener {0, 0, 0};
	const snd::Vec3 forward {0, 0, 1};
	const snd::Vec3 up {0, 1, 0};
	engine->setListenerPosition(&listener);
	engine->setListenerOrientation(&forward, &up);

	// A 3D one-shot, a looping WAV with start/loop/end blocks, and a 2D Ogg stream.
	const std::string oneShotFile = path("Cockpits/Missile_Warning.wav");
	snd::ISoundTemplate* oneShot = engine->createTemplate();
	oneShot->setSoundFilename(oneShotFile.c_str());
	oneShot->setMinDistance(2.0f);
	oneShot->setHalfVolumeDistance(10.0f);

	const std::string loopFile = path("Effects/Grenade_fuse_loop.wav");
	snd::ISoundTemplate* loop = engine->createTemplate();
	loop->setSoundFilename(loopFile.c_str());
	loop->setLoopCount(0);

	const std::string streamFile = path("Ambient/Cerbere.ogg");
	snd::ISoundTemplate* stream = engine->createTemplate();
	stream->setSoundFilename(streamFile.c_str());
	stream->setIs3dSound(false);
	stream->setLoopCount(0);

	snd::ISound* a = oneShot->createSound();
	snd::ISound* b = loop->createSound();
	snd::ISound* c = stream->createSound();
	engine->reloadSoundFiles();

	const snd::Vec3 nearby {0, 0, 3};
	a->playAt(&nearby);
	b->play();
	c->play();
	int maxPlaying = 0;
	for (int frame = 0; frame < 30; ++frame) {
		engine->update(1);
		maxPlaying = std::max(maxPlaying, engine->statistics()->playingSamples);
	}
	CHECK(maxPlaying == 3);
	CHECK(b->isPlaying() && c->isPlaying());
	CHECK(engine->statistics()->streamingBuffers > 0);

	b->stop();
	c->stopImmediately();
	for (int frame = 0; frame < 120; ++frame) {
		engine->update(1);
	}
	CHECK(!c->isPlaying());

	a->release();
	b->release();
	c->release();
	oneShot->release();
	loop->release();
	stream->release();
	engine->shutdown();
	CHECK(!engine->isInitialized());
	snd::shutdownSoundEngine();
}

TEST(SoundEngine_RandomEnvelopeVariantsStayWithinAuthoredBounds)
{
	const snd::EnvelopePoint point {0.0f, 0.99f, 1.02f};
	snd::SoundEnvelope envelope(0, 1, 0.9f, 1.1f, true, &point, 1);
	for (unsigned seed = 0; seed < snd::SoundEnvelope::VariantCount; ++seed) {
		const float value = envelope.evaluate(0, seed);
		CHECK(value >= 0.99f && value <= 1.02f);
	}
}

// Actual source offsets, not a simulated tick clock: the Shepherd gun has a
// long loop and pitch 0.8. At medium quality it must advance in 22050-Hz
// source frames and reroll only after a real loop, not four times too fast.
TEST(SoundEngine_StaticLoopTracksActualPitchedPlayback)
{
	if (!testDir()) return;
	setenv("ALSOFT_DRIVERS", "null", 1);
	auto* engine = snd::getSoundEngine();
	CHECK(engine->initialize(1, "Software Audio", 16) == 0);
	auto* templ = engine->createTemplate();
	templ->setSoundFilename(path("Weapons/Miniguns/Drone_gun_mono.wav").c_str());
	templ->setIs3dSound(false);
	templ->setPitch(0.8f);
	auto* sound = static_cast<snd::Sound*>(templ->createSound());
	sound->setPlayFlags(snd::Sound::SkipStartBlock);
	engine->reloadSoundFiles();
	{
		snd::StaticSample sample(nullptr);
		sample.setSound(sound);
		auto* file = sample.soundFile();
		CHECK(file && file->loaded() && file->sampleRate() == 22050);
		if (file && file->loaded()) {
			CHECK(sample.start(file->loopStart(), 0) == snd::Sample::StartResult::Started);
			const auto seed = sample.pitchSeed;
			std::this_thread::sleep_for(std::chrono::milliseconds(220));
			sample.update();
			const int advanced = sample.playOffset() - file->loopStart();
			CHECK(advanced > 2000 && advanced < 6000);
			CHECK(sample.pitchSeed == seed);
			for (int i = 0; i < 80; ++i) {
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				sample.update();
			}
			CHECK(sample.pitchSeed > seed);
		}
	}
	sound->release();
	templ->release();
	engine->shutdown();
	snd::shutdownSoundEngine();
}
