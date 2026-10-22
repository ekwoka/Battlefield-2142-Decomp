#include "OpenALProvider.h"

#include "OpenALSample.h"
#include "Reverb.h"
#include "SoundMath.h"
#include "SoundServices.h"
#include "SoundTemplate.h"
#include "SoundTweaks.h"

namespace dice::hfe::sound {

namespace {

// The OpenAL library for a provider. BF2142 ships BF2OpenAL.dll (Creative's
// DirectSound-backed router); "System OpenAL" uses the installed driver.
// Elsewhere both map to OpenAL Soft.
const char* const* libraryCandidates(bool systemOpenAL)
{
#if defined(_WIN32)
	static const char* const bundled[] = {"BF2OpenAL.dll", nullptr};
	static const char* const system[] = {"OpenAL32.dll", nullptr};
	return systemOpenAL ? system : bundled;
#elif defined(__APPLE__)
	(void)systemOpenAL;
	static const char* const candidates[] = {"libopenal.1.dylib", "@executable_path/../Frameworks/libopenal.1.dylib",
		"/opt/homebrew/opt/openal-soft/lib/libopenal.1.dylib", "/usr/local/opt/openal-soft/lib/libopenal.1.dylib",
		nullptr};
	return candidates;
#else
	(void)systemOpenAL;
	static const char* const candidates[] = {"libopenal.so.1", "libopenal.so", nullptr};
	return candidates;
#endif
}

bool loadProviderLibrary(bool systemOpenAL)
{
	for (const char* const* name = libraryCandidates(systemOpenAL); *name; ++name) {
		if (loadOpenAL(*name)) {
			return true;
		}
	}
	return false;
}

// Above this the listener is moving implausibly fast (300 m/s), most likely
// from a teleport, and the Doppler shift would be absurd.
constexpr float kMaxListenerSpeed = 300.0f;

} // namespace

bool eaxAvailable() { return eax().active; }

OpenALProvider::OpenALProvider()
{
	// The DirectSound names are what BF2OpenAL.dll calls its devices. Safe Mode
	// selects the waveOut ("MMSYSTEM") device and disables source pooling.
	m_providers = {
		{"DirectSound", "Software Audio + EAX", false, true, true, false},
		{"DirectSound3D", "Hardware Audio", false, false, true, false},
		{"DirectSound3D", "Hardware Audio + EAX", false, true, true, false},
		{"Default Device", "System OpenAL", true, false, true, false},
		{"Default Device", "System OpenAL + EAX", true, true, true, false},
		{"MMSYSTEM", "Safe Mode", false, false, false, false},
		{"DirectSound", "Software Audio", false, false, true, false},
	};
}

OpenALProvider::~OpenALProvider()
{
	close();
}

std::vector<std::string> OpenALProvider::providerNames() const
{
	std::vector<std::string> names;
	for (const ProviderInfo& p : m_providers) {
		names.push_back(p.displayName);
	}
	return names;
}

ProviderOpenResult OpenALProvider::open(const std::string& name)
{
	if (m_device) {
		if (m_openProvider == name) {
			return ProviderOpenResult::Ok;
		}
		close();
	}

	const ProviderInfo* info = nullptr;
	for (const ProviderInfo& p : m_providers) {
		if (p.displayName == name) {
			info = &p;
			break;
		}
	}
	if (!info) {
		return ProviderOpenResult::UnknownProvider;
	}

	if (!loadProviderLibrary(info->systemOpenAL)) {
		if (info->systemOpenAL) {
			SND_WARNING("OpenAL error: Could not find OpenAL32.dll");
		}
		else {
			SND_WARNING("OpenAL error: Could not find %s.", "BF2OpenAL.dll");
		}
		close();
		return ProviderOpenResult::LibraryMissing;
	}

#ifdef _WIN32
	const char* device = info->systemOpenAL ? nullptr : info->deviceName.c_str();
#else
	const char* device = nullptr; // OpenAL Soft's default output
#endif
	m_device = al().alcOpenDevice(device);
	setProviderFlags(info->systemOpenAL ? providerFlags() | ProviderUsesSystemOpenAL
	                                    : providerFlags() & ~ProviderUsesSystemOpenAL);
	if (!m_device) {
		SND_WARNING("OpenAL error: Failed to open device %s", info->deviceName.c_str());
		close();
		return ProviderOpenResult::DeviceFailed;
	}

	const ALint attributes[] = {ALC_FREQUENCY, 44100, 0, 0};
	m_context = al().alcCreateContext(m_device, attributes);
	if (al().alcGetError(m_device) != 0) {
		close();
		return ProviderOpenResult::DeviceFailed;
	}
	m_openProvider = name;
	al().alcMakeContextCurrent(m_context);
	if (al().alcGetError(m_device) != 0) {
		close();
		return ProviderOpenResult::DeviceFailed;
	}

	// The system driver is only trusted with EAX 5 (i.e. a Creative card),
	// unless the tweak says otherwise.
	if ((providerFlags() & ProviderUsesSystemOpenAL) && !soundTweaks().enabled(Tweak::AllowAnySystemOpenAL) &&
		!detectEax5()) {
		SND_WARNING("OpenAL error: Could not use system OpenAL, since it does not support EAX5.0");
		close();
		return ProviderOpenResult::NoEax5;
	}

	if (!countVoices()) {
		close();
		return ProviderOpenResult::DeviceFailed;
	}
	if (info->eax) {
		detectEax3();
	}
	clearALError();
	setDopplerFactor(m_dopplerFactor);
	al().alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
	const ALfloat orientation[] = {0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f};
	al().alListenerfv(AL_ORIENTATION, orientation);

	const char* deviceName = al().alcGetString(m_device, ALC_DEVICE_SPECIFIER);
	SND_INFO("Sound 3D provider: \"%s\" (%s)", name.c_str(), deviceName ? deviceName : "");
	SND_INFO("  EAX version: %d", m_eaxVersion);
	m_description = info->displayName + " (" + (deviceName ? deviceName : "") + ")";

	soundTweaks().set(Tweak::UseSourcePool, info->useSourcePool ? 1.0f : 0.0f);
	soundTweaks().set(Tweak::Use2dOcclusion, info->use2dOcclusion ? 1.0f : 0.0f);

	if (!CHECK_AL_ERROR()) {
		close();
		return ProviderOpenResult::DeviceFailed;
	}
	return ProviderOpenResult::Ok;
}

bool OpenALProvider::countVoices()
{
	// Allocate sources until the device refuses (or 128), then give them back.
	constexpr size_t kMaxProbe = 128;
	std::vector<ALuint> sources;
	sources.reserve(kMaxProbe);
	clearALError();
	while (sources.size() < kMaxProbe) {
		ALuint source = 0;
		al().alGenSources(1, &source);
		if (source == 0 || al().alGetError() != AL_NONE) {
			break;
		}
		sources.push_back(source);
	}
	SND_INFO("Max number of voices: %u", static_cast<unsigned>(sources.size()));
	al().alDeleteSources(static_cast<ALsizei>(sources.size()), sources.data());
	m_maxVoices = static_cast<int>(sources.size());
	clearALError();
	return true;
}

void OpenALProvider::detectEax3()
{
	EaxApi& api = eax();
	api.active = al().alIsExtensionPresent("EAX3.0") || al().alIsExtensionPresent("EAX3.0Emulated");
	if (api.active) {
		api.set = reinterpret_cast<decltype(api.set)>(al().alGetProcAddress("EAXSet"));
		api.get = reinterpret_cast<decltype(api.get)>(al().alGetProcAddress("EAXGet"));
		api.active = api.set && api.get;
	}
	m_eaxVersion = api.active ? 3 : 0;
	SND_INFO("%s available: %s", "EAX3.0", api.active ? "yes" : "no");
}

void OpenALProvider::close()
{
	if (openALLoaded()) {
		purgeStreamingBufferPool();
		purgeSourcePool();
	}
	eax() = {};
	if (m_context) {
		al().alcDestroyContext(m_context);
	}
	m_context = nullptr;
	if (m_device) {
		al().alcCloseDevice(m_device);
	}
	m_device = nullptr;
	m_maxVoices = 0;
	m_eaxVersion = 0;
	m_description.clear();
	m_openProvider.clear();
	unloadOpenAL();
}

std::unique_ptr<Sample> OpenALProvider::createSample(SoundTemplate& soundTemplate, int sampleIndex,
	const Vec3* positionOverride)
{
	SoundFile* file = soundTemplate.soundFile(sampleIndex);
	if (!file) {
		return nullptr;
	}
	switch (file->kind()) {
	case SoundFile::Kind::Static:
		return std::make_unique<StaticSample>(positionOverride);
	case SoundFile::Kind::Streaming:
		return std::make_unique<StreamingSample>(positionOverride);
	default:
		return nullptr;
	}
}

void OpenALProvider::setListenerVelocity(const Vec3& velocity)
{
	Vec3 v = velocity;
	if (v.lengthSquared() > kMaxListenerSpeed * kMaxListenerSpeed) {
		v = normalized(v) * kMaxListenerSpeed;
	}
	// The game is left-handed; OpenAL is right-handed.
	al().alListener3f(AL_VELOCITY, v.x, v.y, -v.z);
}

void OpenALProvider::setMasterGain(float gain)
{
	al().alListenerf(AL_GAIN, gain);
}

void OpenALProvider::setReverbProperties(const EaxListenerProperties* properties)
{
	if (!eax().active || !m_reverbEnabled) {
		return;
	}
	if (eax().set(&kEax30ListenerProperties, EaxListenerAllParameters, 0, const_cast<EaxListenerProperties*>(properties),
			sizeof(EaxListenerProperties)) != 0) {
		CHECK_AL_ERROR();
		SND_ERROR("Failed to update reverb. Disabling reverb from now on.");
		m_reverbEnabled = false;
	}
}

void OpenALProvider::setDopplerFactor(float factor)
{
	m_dopplerFactor = factor;
	if (m_device) {
		al().alDopplerFactor(factor);
	}
}

} // namespace dice::hfe::sound
