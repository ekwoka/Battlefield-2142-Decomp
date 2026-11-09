#include "EnhancedAudio.h"

#include "SoundServices.h"

#include <algorithm>

namespace dice::hfe::sound {

namespace {

// ALC_SOFT_HRTF and ALC_EXT_EFX (efx.h).
constexpr ALenum kAlcHrtfStatusSoft = 0x1993;
constexpr ALenum kAlDirectFilter = 0x20005;
constexpr ALenum kAlFilterType = 0x8001;
constexpr ALenum kAlFilterLowpass = 0x0001;
constexpr ALenum kAlLowpassGain = 0x0001;
constexpr ALenum kAlLowpassGainHF = 0x0002;

struct State {
	bool enabled = false;
	IEnhancedOcclusion* occlusion = nullptr;
	int hrtfStatus = -1;
	void (*genFilters)(ALsizei, ALuint*) = nullptr;
	void (*deleteFilters)(ALsizei, const ALuint*) = nullptr;
	void (*filteri)(ALuint, ALenum, ALint) = nullptr;
	void (*filterf)(ALuint, ALenum, ALfloat) = nullptr;
	ALuint filter = 0; // one lowpass, copied into each source on attach
	std::vector<EnhancedSource> tick;
};

State& state()
{
	static State s;
	return s;
}

const char* hrtfStatusName(int status)
{
	static const char* const names[] = {"disabled", "enabled", "denied", "required", "headphones detected",
		"unsupported format"};
	return status >= 0 && status < 6 ? names[status] : "unknown";
}

} // namespace

void setEnhancedAudio(bool enabled) { state().enabled = enabled; }
bool enhancedAudioEnabled() { return state().enabled; }
void setEnhancedOcclusion(IEnhancedOcclusion* occlusion) { state().occlusion = occlusion; }
IEnhancedOcclusion* enhancedOcclusion() { return state().occlusion; }
int enhancedHrtfStatus() { return state().hrtfStatus; }

void enhancedOpen(ALCdevice* device)
{
	State& s = state();
	if (!s.enabled) {
		return;
	}
	if (al().alcIsExtensionPresent(device, "ALC_SOFT_HRTF")) {
		ALint status = 0;
		al().alcGetIntegerv(device, kAlcHrtfStatusSoft, 1, &status);
		s.hrtfStatus = status;
		SND_INFO("Enhanced audio: HRTF %s", hrtfStatusName(status));
	}
	else {
		SND_INFO("Enhanced audio: no ALC_SOFT_HRTF");
	}
	if (!al().alcIsExtensionPresent(device, "ALC_EXT_EFX")) {
		SND_INFO("Enhanced audio: no ALC_EXT_EFX, no occlusion filters");
		return;
	}
	s.genFilters = reinterpret_cast<decltype(s.genFilters)>(al().alGetProcAddress("alGenFilters"));
	s.deleteFilters = reinterpret_cast<decltype(s.deleteFilters)>(al().alGetProcAddress("alDeleteFilters"));
	s.filteri = reinterpret_cast<decltype(s.filteri)>(al().alGetProcAddress("alFilteri"));
	s.filterf = reinterpret_cast<decltype(s.filterf)>(al().alGetProcAddress("alFilterf"));
	if (!s.genFilters || !s.deleteFilters || !s.filteri || !s.filterf) {
		s.genFilters = nullptr;
		return;
	}
	s.genFilters(1, &s.filter);
	s.filteri(s.filter, kAlFilterType, kAlFilterLowpass);
	if (!CHECK_AL_ERROR()) {
		s.filter = 0;
	}
}

void enhancedClose()
{
	State& s = state();
	if (s.filter) {
		s.deleteFilters(1, &s.filter);
	}
	s.filter = 0;
	s.genFilters = nullptr;
	s.hrtfStatus = -1;
	s.tick.clear();
}

bool enhancedOcclusionActive()
{
	const State& s = state();
	return s.filter != 0 && s.occlusion;
}

void enhancedSubmit(ALuint source, const Vec3& worldPosition)
{
	// Also without an occlusion (between levels), so the tick resets the filter.
	if (state().filter) {
		state().tick.push_back({source, worldPosition});
	}
}

void enhancedRelease(ALuint source)
{
	State& s = state();
	if (!s.filter) {
		return;
	}
	al().alSourcei(source, kAlDirectFilter, 0);
	s.tick.erase(std::remove_if(s.tick.begin(), s.tick.end(), [source](const EnhancedSource& e) { return e.source == source; }),
		s.tick.end());
}

void enhancedEndTick(const Vec3* listener)
{
	State& s = state();
	if (!s.filter) {
		return;
	}
	if (s.occlusion && listener && !s.tick.empty()) {
		s.occlusion->simulate(*listener, s.tick);
	}
	for (const EnhancedSource& e : s.tick) {
		s.filterf(s.filter, kAlLowpassGain, std::clamp(e.gain, 0.0f, 1.0f));
		s.filterf(s.filter, kAlLowpassGainHF, std::clamp(e.gainHF, 0.0f, 1.0f));
		al().alSourcei(e.source, kAlDirectFilter, static_cast<ALint>(s.filter));
	}
	s.tick.clear();
}

} // namespace dice::hfe::sound
