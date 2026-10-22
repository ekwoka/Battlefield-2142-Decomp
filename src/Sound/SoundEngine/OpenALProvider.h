// OpenAL back end (originally OpenALProvider.cpp).
#pragma once

#include "BfOpenAL.h"
#include "ISoundProvider.h"

namespace dice::hfe::sound {

class OpenALProvider final : public ISoundProvider {
public:
	OpenALProvider();
	~OpenALProvider() override;

	std::unique_ptr<Sample> createSample(SoundTemplate& soundTemplate, int sampleIndex,
		const Vec3* positionOverride) override;
	std::vector<std::string> providerNames() const override;
	ProviderOpenResult open(const std::string& name) override;
	void close() override;

	void setListenerVelocity(const Vec3& velocity) override;
	void setMasterGain(float gain) override;
	void setReverbProperties(const EaxListenerProperties* properties) override;
	void setDopplerFactor(float factor) override;
	float dopplerFactor() const override { return m_dopplerFactor; }

	int eaxVersion() const override { return m_eaxVersion; }
	int maxVoices() const override { return m_maxVoices; }
	const char* description() const override { return m_description.c_str(); }

private:
	struct ProviderInfo {
		std::string deviceName;   // OpenAL device on Windows (BF2OpenAL wraps DirectSound)
		std::string displayName;  // what the options menu and settings file use
		bool systemOpenAL;        // use the system library instead of BF2OpenAL
		bool eax;
		bool useSourcePool;
		bool use2dOcclusion;
	};

	bool countVoices();
	void detectEax3();

	std::vector<ProviderInfo> m_providers;
	ALCdevice* m_device = nullptr;
	ALCcontext* m_context = nullptr;
	std::string m_openProvider;
	std::string m_description;
	float m_dopplerFactor = 1.0f;
	bool m_reverbEnabled = true;
	int m_maxVoices = 0;
	int m_eaxVersion = 0;
};

// EAX 3 property setter, available while an EAX provider is open.
bool eaxAvailable();

} // namespace dice::hfe::sound
