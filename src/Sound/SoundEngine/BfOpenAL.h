// Runtime-loaded OpenAL entry points (originally BfOpenAL.cpp).
//
// The engine never links OpenAL directly: the selected provider decides which
// library to load (the bundled BF2OpenAL.dll, the system OpenAL32.dll, or
// OpenAL Soft on macOS/Linux), and every call goes through this table.
#pragma once

#include <cstdint>

namespace dice::hfe::sound {

using ALboolean = char;
using ALchar = char;
using ALint = int;
using ALuint = unsigned int;
using ALsizei = int;
using ALenum = int;
using ALfloat = float;
struct ALCdevice;
struct ALCcontext;

// The subset of AL/ALC constants the engine uses.
enum : ALenum {
	AL_NONE = 0,
	AL_SOURCE_RELATIVE = 0x202,
	AL_CONE_INNER_ANGLE = 0x1001,
	AL_CONE_OUTER_ANGLE = 0x1002,
	AL_PITCH = 0x1003,
	AL_POSITION = 0x1004,
	AL_DIRECTION = 0x1005,
	AL_VELOCITY = 0x1006,
	AL_LOOPING = 0x1007,
	AL_BUFFER = 0x1009,
	AL_GAIN = 0x100A,
	AL_ORIENTATION = 0x100F,
	AL_SOURCE_STATE = 0x1010,
	AL_INITIAL = 0x1011,
	AL_PLAYING = 0x1012,
	AL_PAUSED = 0x1013,
	AL_STOPPED = 0x1014,
	AL_BUFFERS_QUEUED = 0x1015,
	AL_BUFFERS_PROCESSED = 0x1016,
	AL_REFERENCE_DISTANCE = 0x1020,
	AL_ROLLOFF_FACTOR = 0x1021,
	AL_CONE_OUTER_GAIN = 0x1022,
	AL_FORMAT_MONO16 = 0x1101,
	AL_FORMAT_STEREO16 = 0x1103,
	AL_INVERSE_DISTANCE_CLAMPED = 0xD002,
	ALC_FREQUENCY = 0x1007,
	ALC_DEVICE_SPECIFIER = 0x1005,
};

struct OpenALApi {
	ALenum (*alGetInteger)(ALenum);
	const ALchar* (*alGetString)(ALenum);
	ALenum (*alGetError)();
	ALboolean (*alIsExtensionPresent)(const ALchar*);
	void* (*alGetProcAddress)(const ALchar*);
	ALenum (*alGetEnumValue)(const ALchar*);
	void (*alListenerf)(ALenum, ALfloat);
	void (*alListenerfv)(ALenum, const ALfloat*);
	void (*alListener3f)(ALenum, ALfloat, ALfloat, ALfloat);
	void (*alGetListenerfv)(ALenum, ALfloat*);
	void (*alGenSources)(ALsizei, ALuint*);
	void (*alDeleteSources)(ALsizei, const ALuint*);
	void (*alSourcei)(ALuint, ALenum, ALint);
	void (*alSourcef)(ALuint, ALenum, ALfloat);
	void (*alSourcefv)(ALuint, ALenum, const ALfloat*);
	void (*alGetSourcei)(ALuint, ALenum, ALint*);
	void (*alGetSourcef)(ALuint, ALenum, ALfloat*);
	void (*alGetSourcefv)(ALuint, ALenum, ALfloat*);
	void (*alSourcePlay)(ALuint);
	void (*alSourceStop)(ALuint);
	void (*alSourceRewind)(ALuint);
	void (*alGenBuffers)(ALsizei, ALuint*);
	void (*alDeleteBuffers)(ALsizei, const ALuint*);
	void (*alBufferData)(ALuint, ALenum, const void*, ALsizei, ALsizei);
	void (*alGetBufferi)(ALuint, ALenum, ALint*);
	void (*alSourceUnqueueBuffers)(ALuint, ALsizei, ALuint*);
	void (*alSourceQueueBuffers)(ALuint, ALsizei, const ALuint*);
	void (*alDopplerFactor)(ALfloat);
	void (*alDistanceModel)(ALenum);
	ALCcontext* (*alcCreateContext)(ALCdevice*, const ALint*);
	ALboolean (*alcMakeContextCurrent)(ALCcontext*);
	void (*alcDestroyContext)(ALCcontext*);
	ALenum (*alcGetError)(ALCdevice*);
	ALCdevice* (*alcOpenDevice)(const ALchar*);
	ALboolean (*alcCloseDevice)(ALCdevice*);
	ALboolean (*alcIsExtensionPresent)(ALCdevice*, const ALchar*);
	const ALchar* (*alcGetString)(ALCdevice*, ALenum);
	void (*alcGetIntegerv)(ALCdevice*, ALenum, ALsizei, ALint*);

	// EAX-RAM: chooses where buffer memory lives on X-Fi hardware.
	ALboolean (*eaxSetBufferMode)(ALsizei, const ALuint*, ALint);
	ALint eaxStorageAutomatic;
	ALint eaxStorageAccessible;
};

// The loaded table; only valid while openALLoaded() is true.
const OpenALApi& al();
bool openALLoaded();

// Loads every entry point from `library`. Fails (and unloads) if any is missing.
bool loadOpenAL(const char* library);
void unloadOpenAL();

// Logs and clears any pending AL error. Returns true when there was none.
bool checkALError(const char* file, int line);
void clearALError();

// Detects EAX 5 and, when present, EAX-RAM buffer modes.
bool detectEax5();

// Buffers for static samples may live in hardware memory; streaming buffers
// must stay CPU-accessible because they are refilled.
void genStaticBuffers(ALsizei count, ALuint* buffers);
void genStreamingBuffers(ALsizei count, ALuint* buffers);

// EAX 3.0 (Creative hardware or the "EAX3.0Emulated" software path).
struct EaxGuid {
	uint32_t data1;
	uint16_t data2;
	uint16_t data3;
	uint8_t data4[8];
};
inline constexpr EaxGuid kEax30ListenerProperties {0xa8fa6882, 0xb476, 0x11d3, {0xbd, 0xb9, 0x00, 0xc0, 0xf0, 0x2d, 0xdf, 0x87}};
inline constexpr EaxGuid kEax30BufferProperties {0xa8fa6881, 0xb476, 0x11d3, {0xbd, 0xb9, 0x00, 0xc0, 0xf0, 0x2d, 0xdf, 0x87}};
enum : ALuint {
	EaxListenerAllParameters = 1,
	EaxBufferAllParameters = 1,
};

struct EaxApi {
	ALenum (*set)(const EaxGuid*, ALuint property, ALuint source, void* value, ALuint size) = nullptr;
	ALenum (*get)(const EaxGuid*, ALuint property, ALuint source, void* value, ALuint size) = nullptr;
	bool active = false;
};
EaxApi& eax();

enum ProviderFlags : uint32_t {
	ProviderUsesSystemOpenAL = 1,
};
uint32_t providerFlags();
void setProviderFlags(uint32_t flags);

} // namespace dice::hfe::sound

#define CHECK_AL_ERROR() ::dice::hfe::sound::checkALError(__FILE__, __LINE__)
