// Tunable sound-engine parameters. Sound/AudioTweak.con sets them by name
// (e.g. "prio-k-distance 1.0") through ISoundEngine::setTweak, and the engine
// reads them every frame. All values are floats; booleans use >= 0.5.
#pragma once

#include <array>
#include <string>

namespace dice::hfe::sound {

enum class Tweak {
	AmbientFadeTime,                // seconds
	AmbientForcedFadeTime,
	AmbientIndoorObstruction,
	AmbientIndoorObstructionSpeed,
	DistanceDelayThreshold,         // metres before the speed-of-sound delay kicks in
	DistanceDelayFactor,
	MaxDistanceCutoff,
	PrioKFrontBack,                 // priority weights used when voices run out
	PrioKRightLeft,
	PrioKAge,
	PrioAgeOnlyDistance,
	PrioKDistance,
	Balance2d3dDefault,             // 2D/3D mix for "balance" sounds
	Balance2d3dEax,
	AirAbsorptionMinDistance,
	AirAbsorptionMaxDistance,
	AirAbsorptionMax,
	AirAbsorptionEaxMinDistance,
	AirAbsorptionEaxMaxDistance,
	AirAbsorptionEaxDbPerMeter,
	SoundFileUseUsageRefCount,
	Debug0,
	Debug1,
	Debug2,
	Debug3,
	UseSourcePool,                  // recycle OpenAL sources instead of deleting them
	RequiredBufferSize,
	Use2dOcclusion,
	Listener3dObstruction,
	Listener3dObstructionFactor,
	AllowAnySystemOpenAL,           // accept a system OpenAL without EAX 5
	EnableCulling,
	MinFreeSources,
	Count,
};

class SoundTweaks {
public:
	SoundTweaks();

	float get(Tweak tweak) const { return m_values[static_cast<size_t>(tweak)]; }
	bool enabled(Tweak tweak) const { return get(tweak) >= 0.5f; }

	// Clamps where the original did: AmbientFadeTime to [0, 100] and the
	// obstruction factors to [0, 1].
	void set(Tweak tweak, float value);

	// Lookup by the names used in AudioTweak.con. Unknown names log a warning.
	void set(const std::string& name, float value);
	float get(const std::string& name) const;

private:
	std::array<float, static_cast<size_t>(Tweak::Count)> m_values {};
};

SoundTweaks& soundTweaks();

} // namespace dice::hfe::sound
