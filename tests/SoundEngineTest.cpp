// Runs real BF2142 sound files through the engine. Needs extracted game
// sounds in $BF2142_SOUND_TEST_DIR (skipped otherwise) and plays them on
// OpenAL Soft's silent "null" driver.
#include "Sound/SoundEngine/ISoundEngine.h"
#include "Sound/SoundEngine/WavFileFormat.h"
#include "TestMain.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace snd = dice::hfe::sound;

namespace {

const char* testDir() { return std::getenv("BF2142_SOUND_TEST_DIR"); }

std::string path(const char* relative) { return std::string(testDir()) + "/" + relative; }

} // namespace

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
