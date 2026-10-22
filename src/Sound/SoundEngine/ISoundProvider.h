// A 3D audio back end ("provider" in the game's audio options). The only
// implementation is OpenALProvider, which presents the classic DirectSound
// choices as OpenAL devices.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::sound {

struct EaxListenerProperties;
class SoundTemplate;
class Sample;
struct Vec3;

enum class ProviderOpenResult {
	Ok = 0,
	UnknownProvider = 1,
	LibraryMissing = 2,  // BF2OpenAL / system OpenAL not found
	NoEax5 = 3,          // system OpenAL without EAX 5 and allow-any-system-openal off
	DeviceFailed = 4,
};

class ISoundProvider {
public:
	virtual ~ISoundProvider() = default;

	// Creates a voice for sample `sampleIndex` of `soundTemplate`; null if the
	// template has no usable data there.
	virtual std::unique_ptr<Sample> createSample(SoundTemplate& soundTemplate, int sampleIndex,
		const Vec3* positionOverride) = 0;

	// Names shown in the options menu.
	virtual std::vector<std::string> providerNames() const = 0;

	virtual ProviderOpenResult open(const std::string& name) = 0;
	virtual void close() = 0;

	virtual void setListenerVelocity(const Vec3& velocity) = 0;
	virtual void setMasterGain(float gain) = 0;
	virtual void setReverbProperties(const EaxListenerProperties* properties) = 0;
	virtual void setDopplerFactor(float factor) = 0;
	virtual float dopplerFactor() const = 0;

	virtual int eaxVersion() const = 0;   // 0 or 3
	virtual int maxVoices() const = 0;
	virtual const char* description() const = 0;
};

} // namespace dice::hfe::sound
