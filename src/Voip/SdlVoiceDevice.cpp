// The microphone and speakers through SDL3's audio streams, 8 kHz mono
// 16-bit (SDL converts from the devices' own formats). The original opens
// DirectSound capture and playback buffers in the same format (0x00720910,
// 0x00721400, 0x007214c0).
#include "VoiceChat.h"

#include <SDL3/SDL.h>

namespace dice::hfe::voip {

namespace {

class SdlVoiceDevice final : public VoiceDevice {
public:
	~SdlVoiceDevice() override
	{
		if (m_capture) {
			SDL_DestroyAudioStream(m_capture);
		}
		if (m_playback) {
			SDL_DestroyAudioStream(m_playback);
		}
		if (m_started) {
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
		}
	}

	bool open(std::string* error)
	{
		if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
			if (error) {
				*error = SDL_GetError();
			}
			return false;
		}
		m_started = true;
		const SDL_AudioSpec spec {SDL_AUDIO_S16, 1, kSampleRate};
		m_playback = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
		if (!m_playback) {
			if (error) {
				*error = SDL_GetError();
			}
			return false;
		}
		SDL_ResumeAudioStreamDevice(m_playback);
		// Without a microphone (or permission for it) voice still plays.
		m_capture = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
		if (m_capture) {
			SDL_ResumeAudioStreamDevice(m_capture);
		} else if (error) {
			*error = SDL_GetError();
		}
		return true;
	}

	size_t readCapture(int16_t* out, size_t max) override
	{
		if (!m_capture || max == 0) {
			return 0;
		}
		const int got = SDL_GetAudioStreamData(m_capture, out, static_cast<int>(max * sizeof(int16_t)));
		return got > 0 ? static_cast<size_t>(got) / sizeof(int16_t) : 0;
	}

	void writePlayback(const int16_t* samples, size_t count) override
	{
		if (m_playback && count) {
			SDL_PutAudioStreamData(m_playback, samples, static_cast<int>(count * sizeof(int16_t)));
		}
	}

	size_t playbackQueued() const override
	{
		const int queued = m_playback ? SDL_GetAudioStreamQueued(m_playback) : 0;
		return queued > 0 ? static_cast<size_t>(queued) / sizeof(int16_t) : 0;
	}

private:
	bool m_started = false;
	SDL_AudioStream* m_capture = nullptr;
	SDL_AudioStream* m_playback = nullptr;
};

} // namespace

std::unique_ptr<VoiceDevice> openVoiceDevice(std::string* error)
{
	auto device = std::make_unique<SdlVoiceDevice>();
	if (!device->open(error)) {
		return nullptr;
	}
	return device;
}

bool voiceDeviceAvailable()
{
	return true;
}

} // namespace dice::hfe::voip
