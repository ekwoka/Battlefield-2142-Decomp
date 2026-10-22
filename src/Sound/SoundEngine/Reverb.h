// EAX reverb presets (originally Reverb.cpp).
//
// A preset file is plain text: a title line, then alternating label and value
// lines in the order of EAX 3.0's listener properties (vector values take
// three value lines). The level designer's preset is the base; the engine
// scales its room level by the ambient reverb gain each frame.
#pragma once

#include <cstdint>

namespace dice::hfe::sound {

class ISoundProvider;

// Layout of EAXLISTENERPROPERTIES (EAX 3.0); passed verbatim to EAXSet.
struct EaxListenerProperties {
	uint32_t environment;
	float environmentSize;
	float environmentDiffusion;
	int32_t room;
	int32_t roomHF;
	int32_t roomLF;
	float decayTime;
	float decayHFRatio;
	float decayLFRatio;
	int32_t reflections;
	float reflectionsDelay;
	float reflectionsPan[3];
	int32_t reverb;
	float reverbDelay;
	float reverbPan[3];
	float echoTime;
	float echoDepth;
	float modulationTime;
	float modulationDepth;
	float airAbsorptionHF;
	float hfReference;
	float lfReference;
	float roomRolloffFactor;
	uint32_t flags;
};
static_assert(sizeof(EaxListenerProperties) == 0x70);

class Reverb {
public:
	bool load(const char* presetFile);

	// Scales the room level by `gain` (linear) relative to the preset.
	void setRoomGain(float gain);

	// Sends the properties to the provider if they changed.
	void apply(ISoundProvider& provider);

	// Forces the next apply() to resend, e.g. after the provider reopened.
	void invalidate() { m_dirty = true; }

private:
	EaxListenerProperties m_preset {};
	EaxListenerProperties m_current {};
	bool m_dirty = true;
};

// Linear gain to EAX millibels: 2000 * log10(gain), -10000 for silence.
float gainToMillibels(float gain);

} // namespace dice::hfe::sound
