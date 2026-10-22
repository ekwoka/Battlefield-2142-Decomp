#include "BfOpenAL.h"

#include "Platform/Platform.h"
#include "SoundServices.h"

namespace dice::hfe::sound {

namespace {

OpenALApi g_al {};
void* g_library = nullptr;
bool g_loaded = false;
uint32_t g_providerFlags = 0;

// Without EAX-RAM, buffers are created with no storage hint.
ALboolean noBufferMode(ALsizei, const ALuint*, ALint) { return 1; }

template <typename Fn>
void bind(Fn& slot, const char* name, bool& complete)
{
	slot = reinterpret_cast<Fn>(platform::findSymbol(g_library, name));
	if (!slot) {
		complete = false;
	}
}

} // namespace

const OpenALApi& al() { return g_al; }
bool openALLoaded() { return g_loaded; }

bool loadOpenAL(const char* library)
{
	g_library = platform::loadLibrary(library);
	if (!g_library) {
		return false;
	}

	bool complete = true;
	bind(g_al.alGetInteger, "alGetInteger", complete);
	bind(g_al.alGetString, "alGetString", complete);
	bind(g_al.alGetError, "alGetError", complete);
	bind(g_al.alIsExtensionPresent, "alIsExtensionPresent", complete);
	bind(g_al.alGetProcAddress, "alGetProcAddress", complete);
	bind(g_al.alGetEnumValue, "alGetEnumValue", complete);
	bind(g_al.alListenerf, "alListenerf", complete);
	bind(g_al.alListenerfv, "alListenerfv", complete);
	bind(g_al.alListener3f, "alListener3f", complete);
	bind(g_al.alGetListenerfv, "alGetListenerfv", complete);
	bind(g_al.alGenSources, "alGenSources", complete);
	bind(g_al.alDeleteSources, "alDeleteSources", complete);
	bind(g_al.alSourcei, "alSourcei", complete);
	bind(g_al.alSourcef, "alSourcef", complete);
	bind(g_al.alSourcefv, "alSourcefv", complete);
	bind(g_al.alGetSourcei, "alGetSourcei", complete);
	bind(g_al.alGetSourcef, "alGetSourcef", complete);
	bind(g_al.alGetSourcefv, "alGetSourcefv", complete);
	bind(g_al.alSourcePlay, "alSourcePlay", complete);
	bind(g_al.alSourceStop, "alSourceStop", complete);
	bind(g_al.alSourceRewind, "alSourceRewind", complete);
	bind(g_al.alGenBuffers, "alGenBuffers", complete);
	bind(g_al.alDeleteBuffers, "alDeleteBuffers", complete);
	bind(g_al.alBufferData, "alBufferData", complete);
	bind(g_al.alGetBufferi, "alGetBufferi", complete);
	bind(g_al.alSourceUnqueueBuffers, "alSourceUnqueueBuffers", complete);
	bind(g_al.alSourceQueueBuffers, "alSourceQueueBuffers", complete);
	bind(g_al.alDopplerFactor, "alDopplerFactor", complete);
	bind(g_al.alDistanceModel, "alDistanceModel", complete);
	bind(g_al.alcCreateContext, "alcCreateContext", complete);
	bind(g_al.alcMakeContextCurrent, "alcMakeContextCurrent", complete);
	bind(g_al.alcDestroyContext, "alcDestroyContext", complete);
	bind(g_al.alcGetError, "alcGetError", complete);
	bind(g_al.alcOpenDevice, "alcOpenDevice", complete);
	bind(g_al.alcCloseDevice, "alcCloseDevice", complete);
	bind(g_al.alcIsExtensionPresent, "alcIsExtensionPresent", complete);
	bind(g_al.alcGetString, "alcGetString", complete);
	bind(g_al.alcGetIntegerv, "alcGetIntegerv", complete);
	g_al.eaxSetBufferMode = noBufferMode;

	g_loaded = complete;
	if (!complete) {
		unloadOpenAL();
	}
	return complete;
}

void unloadOpenAL()
{
	platform::freeLibrary(g_library);
	g_library = nullptr;
	g_al = {};
	g_loaded = false;
}

bool checkALError(const char* file, int line)
{
	const ALenum error = g_al.alGetError();
	if (error == AL_NONE) {
		return true;
	}
	soundLog().warning(formatSoundMessage("OpenAL Error: %s: %s", g_al.alGetString(error), file), file, line);
	return false;
}

void clearALError()
{
	g_al.alGetError();
}

bool detectEax5()
{
	if (!g_al.alIsExtensionPresent || !g_al.alIsExtensionPresent("EAX5.0")) {
		return false;
	}
	SND_INFO("Detected OpenAL extension EAX5.0");
	if (g_al.eaxSetBufferMode == noBufferMode && g_al.alIsExtensionPresent("EAX-RAM") == 1) {
		SND_INFO("Detected OpenAL extension EAX-RAM");
		g_al.eaxSetBufferMode =
			reinterpret_cast<decltype(g_al.eaxSetBufferMode)>(g_al.alGetProcAddress("EAXSetBufferMode"));
		g_al.eaxStorageAutomatic = g_al.alGetEnumValue("AL_STORAGE_AUTOMATIC");
		g_al.eaxStorageAccessible = g_al.alGetEnumValue("AL_STORAGE_ACCESSIBLE");
	}
	return true;
}

void genStaticBuffers(ALsizei count, ALuint* buffers)
{
	g_al.alGenBuffers(count, buffers);
	g_al.eaxSetBufferMode(count, buffers, g_al.eaxStorageAutomatic);
}

void genStreamingBuffers(ALsizei count, ALuint* buffers)
{
	g_al.alGenBuffers(count, buffers);
	g_al.eaxSetBufferMode(count, buffers, g_al.eaxStorageAccessible);
}

EaxApi& eax()
{
	static EaxApi api;
	return api;
}

uint32_t providerFlags() { return g_providerFlags; }
void setProviderFlags(uint32_t flags) { g_providerFlags = flags; }

} // namespace dice::hfe::sound
