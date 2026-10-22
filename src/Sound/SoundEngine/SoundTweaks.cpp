#include "SoundTweaks.h"

#include "SoundServices.h"

#include <algorithm>

namespace dice::hfe::sound {

namespace {

struct TweakInfo {
	const char* name;
	float defaultValue;
};

// Order matches the Tweak enum.
constexpr TweakInfo kTweaks[] = {
	{"ambient-fade-time", 4.0f},
	{"ambient-forced-fade-time", 1.0f},
	{"ambient-indoor-obstruction", 0.98f},
	{"ambient-indoor-obstruction-speed", 0.08f},
	{"distance-delay-threshold", 5.0f},
	{"distance-delay-factor", 1.0f},
	{"max-distance-cutoff", 0.02f},
	{"prio-k-frontback", 0.0f},
	{"prio-k-rightleft", 0.0f},
	{"prio-k-age", 0.5f},
	{"prio-age-only-distance", 5.0f},
	{"prio-k-distance", 1.0f},
	{"balance2d3d-default", 0.5f},
	{"balance2d3d-eax", 0.5f},
	{"airabsorption-mindistance", 15.0f},
	{"airabsorption-maxdistance", 30.0f},
	{"airabsorption-max", 0.9f},
	{"airabsorption-eax-mindistance", 30.0f},
	{"airabsorption-eax-maxdistance", 60.0f},
	{"airabsorption-eax-dbpermeter", -5.0f},
	{"soundfile-use-usagerefcount", 0.0f},
	{"dbg0", 0.0f},
	{"dbg1", 0.0f},
	{"dbg2", 0.0f},
	{"dbg3", 0.0f},
	{"use-source-pool", 0.0f},
	{"required-buffer-size", 31.0f},
	{"use-2d-occlusion", 0.0f},
	{"listener-3d-obstruction", 0.0f},
	{"listener-3d-obstruction-factor", 0.85f},
	{"allow-any-system-openal", 0.0f},
	{"enable-culling", 1.0f},
	{"min-free-sources", 4.0f},
};
static_assert(std::size(kTweaks) == static_cast<size_t>(Tweak::Count));

const TweakInfo* findTweak(const std::string& name, size_t& index)
{
	for (index = 0; index < std::size(kTweaks); ++index) {
		if (name == kTweaks[index].name) {
			return &kTweaks[index];
		}
	}
	return nullptr;
}

} // namespace

SoundTweaks::SoundTweaks()
{
	for (size_t i = 0; i < std::size(kTweaks); ++i) {
		m_values[i] = kTweaks[i].defaultValue;
	}
}

void SoundTweaks::set(Tweak tweak, float value)
{
	switch (tweak) {
	case Tweak::AmbientFadeTime:
		value = std::clamp(value, 0.0f, 100.0f);
		break;
	case Tweak::AmbientIndoorObstruction:
	case Tweak::AmbientIndoorObstructionSpeed:
		value = std::clamp(value, 0.0f, 1.0f);
		break;
	default:
		break;
	}
	m_values[static_cast<size_t>(tweak)] = value;
}

void SoundTweaks::set(const std::string& name, float value)
{
	size_t index;
	if (findTweak(name, index)) {
		set(static_cast<Tweak>(index), value);
	}
	else {
		SND_WARNING("Cannot set unknown property %s", name.c_str());
	}
}

float SoundTweaks::get(const std::string& name) const
{
	size_t index;
	if (findTweak(name, index)) {
		return m_values[index];
	}
	SND_WARNING("Cannot get unknown property %s", name.c_str());
	return 0.0f;
}

SoundTweaks& soundTweaks()
{
	static SoundTweaks tweaks;
	return tweaks;
}

} // namespace dice::hfe::sound
