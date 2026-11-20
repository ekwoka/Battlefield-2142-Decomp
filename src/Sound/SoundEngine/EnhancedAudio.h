// Enhanced-mode audio (Scene/EnhancedSettings.h): not in the original game.
// With enhanced.enabled set when the provider opens, OpenAL Soft is asked
// for HRTF and the 3D sources get an EFX direct filter (gain + low-pass)
// from an occlusion module (Sound/Enhanced/SteamAudioOcclusion.h), which
// replaces the retail obstruction gain for those sources, and a source's
// Doppler speed is held below the speed of sound (enhancedDopplerVelocity).
// Off, none of this runs and the provider opens exactly as before.
// Client-side only: nothing here feeds the simulation.
#pragma once

#include "BfOpenAL.h"
#include "SoundMath.h"

#include <algorithm>
#include <vector>

namespace dice::hfe::sound {

// A playing 3D source this tick, where it is (game world space), and what
// reaches the listener through the scene (each 0..1, set by the occlusion).
struct EnhancedSource {
	ALuint source = 0;
	Vec3 position;
	float gain = 1.0f;   // the low band
	float gainHF = 1.0f; // the high band, relative to `gain` (AL_LOWPASS_GAINHF)
};

class IEnhancedOcclusion {
public:
	virtual ~IEnhancedOcclusion() = default;
	virtual void simulate(const Vec3& listener, std::vector<EnhancedSource>& sources) = 0;
};

// A source's filter moves towards the occlusion's answer by at most 0.1 an
// update, as retail's obstruction does (OpenALSample::computeOcclusionGain),
// so walking past a corner fades instead of switching.
inline float enhancedSmoothed(float current, float target)
{
	return current + std::max(-0.1f, std::min(0.1f, target - current));
}

// The Doppler: OpenAL's pitch is (SS - DF vls) / (SS - DF vss), vss the
// source's speed towards the listener. Retail passes the game's velocity,
// dropped only when an axis reaches 300 (OpenALSample::update), so a round
// diving at the listener (an orbital strike's shells) still nears SS / DF and
// its pitch shoots up just before the impact. Enhanced mode holds a source's
// speed to a fraction of SS / DF: approaching, at most an octave up.
inline constexpr float kOpenAlSpeedOfSound = 343.3f; // AL_SPEED_OF_SOUND's default; nothing sets it
inline constexpr float kEnhancedDopplerMaxSpeedFraction = 0.5f;

inline Vec3 enhancedDopplerVelocity(const Vec3& velocity, float dopplerFactor)
{
	if (!(dopplerFactor > 0.0f)) {
		return velocity;
	}
	const float maxSpeed = kEnhancedDopplerMaxSpeedFraction * kOpenAlSpeedOfSound / dopplerFactor;
	const float speedSquared = velocity.lengthSquared();
	if (speedSquared <= maxSpeed * maxSpeed) {
		return velocity;
	}
	return velocity * (maxSpeed / std::sqrt(speedSquared));
}

// Read when the provider opens (main.cpp sets it from enhanced.enabled).
void setEnhancedAudio(bool enabled);
bool enhancedAudioEnabled();
// The occlusion for the current level, or null.
void setEnhancedOcclusion(IEnhancedOcclusion* occlusion);
IEnhancedOcclusion* enhancedOcclusion();
// ALC_HRTF_STATUS_SOFT after the context opened, -1 when not asked for.
int enhancedHrtfStatus();

// The provider's and the samples' side.
inline constexpr ALenum kAlcHrtfSoft = 0x1992;
void enhancedOpen(ALCdevice* device); // context current
void enhancedClose();                 // context still current
bool enhancedOcclusionActive();       // filters on and an occlusion set
void enhancedSubmit(ALuint source, const Vec3& worldPosition); // no-op unless filters are on
void enhancedRelease(ALuint source); // before the source goes back to the pool
void enhancedEndTick(const Vec3* listener);

} // namespace dice::hfe::sound
