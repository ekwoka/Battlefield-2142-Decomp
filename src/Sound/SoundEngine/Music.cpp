#include "Music.h"

#include "BfOpenAL.h"
#include "BfSoundEngine.h"
#include "OggStream.h"
#include "OpenALSample.h"
#include "SoundFile.h"
#include "SoundServices.h"

#include <atomic>
#include <chrono>
#include <climits>
#include <list>
#include <mutex>
#include <string>
#include <thread>

namespace dice::hfe::sound::music {

namespace {

constexpr int kBufferCount = 16;
constexpr int kVolumeGroupMusic = 1;

SoundFile* g_track = nullptr;
bool g_running = false;

struct Player {
	std::thread thread;
	std::atomic<bool> stopRequested {false};
};
Player* g_player = nullptr;

// Shared with the thread, in thousandths.
std::atomic<int> g_volumeMilli {1000};
std::atomic<int> g_fadeMilliseconds {0};

// OpenAL errors raised on the music thread, reported from the main thread.
std::mutex g_errorsMutex;
std::list<std::string> g_errors;

void recordALError(const char* operation)
{
	const ALenum error = al().alGetError();
	if (error != AL_NONE) {
		std::lock_guard<std::mutex> lock(g_errorsMutex);
		g_errors.push_back(std::string("Music thread OpenAL Error: ") + operation + ": " + al().alGetString(error));
	}
}

void reportErrors()
{
	std::lock_guard<std::mutex> lock(g_errorsMutex);
	if (g_errors.empty()) {
		return;
	}
	for (const std::string& error : g_errors) {
		SND_WARNING("%s", error.c_str());
	}
	SND_ERROR("At least one error caused by music player (see log entries above)");
	g_errors.clear();
}

void streamTrack(const std::atomic<bool>& stopRequested, const SoundFile& track)
{
	uint8_t scratch[kStreamChunkBytes];
	ALuint source = 0;
	ALuint buffers[kBufferCount] = {};

	al().alGenSources(1, &source);
	recordALError("alGenSources");
	genStreamingBuffers(kBufferCount, buffers);
	recordALError("alGenStreamingBuffers");

	// Music loops until stopped.
	std::unique_ptr<OggDecoder> decoder;
	int loopsRemaining = INT_MAX;
	for (ALuint buffer : buffers) {
		startOggStream(decoder, track, scratch, loopsRemaining, -1, source, buffer, false);
	}
	al().alSourcef(source, AL_GAIN, static_cast<float>(g_volumeMilli.load()) * 0.001f);
	recordALError("alSourcef (AL_GAIN)");
	al().alSourcePlay(source);
	recordALError("alSourcePlay");

	bool fading = false;
	float fadeLevel = 1.0f;
	float fadeSeconds = 0.0f;
	while (!stopRequested.load()) {
		if (!fading) {
			if (const int fadeMs = g_fadeMilliseconds.load(); fadeMs > 0) {
				fading = true;
				fadeSeconds = static_cast<float>(fadeMs) * 0.001f;
			}
		}
		else {
			fadeLevel -= 0.01f / fadeSeconds; // one 10 ms step
			if (fadeLevel <= 0.0f) {
				break;
			}
		}
		const float gain = static_cast<float>(g_volumeMilli.load()) * 0.001f * fadeLevel;
		al().alSourcef(source, AL_GAIN, gain);
		if (gain > 0.0f) {
			for (int i = 0; i < 4; ++i) {
				updateOggStream(decoder.get(), track, scratch, loopsRemaining, source, 0, false);
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	al().alSourceStop(source);
	recordALError("alSourceStop");
	for (int i = 0; i < kBufferCount; ++i) {
		ALuint buffer;
		al().alSourceUnqueueBuffers(source, 1, &buffer);
		recordALError(("alSourceUnqueueBuffers " + std::to_string(i)).c_str());
	}
	al().alDeleteBuffers(kBufferCount, buffers);
	recordALError("alDeleteBuffers");
	al().alDeleteSources(1, &source);
	recordALError("alDeleteSources");
}

} // namespace

bool setTrack(const char* path)
{
	if (g_track && g_track->path() == path) {
		return true;
	}
	delete g_track;
	// Music doesn't go through the file cache: two references and a usage
	// mark keep it loadable, and it is enabled at every quality level.
	g_track = new SoundFile(path);
	g_track->addUsage();
	g_track->addRef();
	g_track->addRef();
	g_track->qualityFlags() |= SoundFile::EnabledAtHigh | SoundFile::EnabledAtMedium | SoundFile::EnabledAtLow;
	return true;
}

bool playTrack(const char* path)
{
	stop(0.0f);
	if (g_track) {
		g_track->unload(true, true);
	}
	if (path) {
		return setTrack(path);
	}
	g_running = false;
	return true;
}

bool start()
{
	if (!g_track) {
		return false;
	}
	stop(0.0f);
	if (soundEngine().isInitialized()) {
		{
			std::lock_guard<std::mutex> lock(g_errorsMutex);
			g_errors.clear();
		}
		g_track->load(soundEngine().qualityLevel());
		if (g_track->loaded()) {
			g_running = true;
			updateVolume();
			g_player = new Player;
			const SoundFile* track = g_track;
			g_player->thread = std::thread([player = g_player, track] { streamTrack(player->stopRequested, *track); });
			SND_INFO("Music thread was started");
		}
	}
	return g_running;
}

bool stop(float fadeSeconds)
{
	const bool wasRunning = g_running;
	int fadeMs = 0;
	if (g_player) {
		if (fadeSeconds > 0.0f) {
			SND_INFO("Music thread fading out");
			fadeMs = static_cast<int>(fadeSeconds * 1000.0f);
		}
		else {
			g_player->stopRequested = true;
			g_player->thread.join();
			delete g_player;
			g_player = nullptr;
			SND_INFO("Music thread was stopped");
			CHECK_AL_ERROR();
			reportErrors();
		}
		g_fadeMilliseconds = fadeMs;
	}
	g_running = false;
	return wasRunning;
}

void updateVolume()
{
	if (g_running) {
		g_volumeMilli = static_cast<int>(soundEngine().volume(kVolumeGroupMusic) * 1000.0f);
	}
}

} // namespace dice::hfe::sound::music
