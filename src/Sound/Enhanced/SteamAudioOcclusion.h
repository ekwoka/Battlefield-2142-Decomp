// Enhanced mode: occlusion and transmission of the 3D sources through a
// level's static meshes, by Steam Audio's direct simulation (not in the
// original game; Sound/SoundEngine/EnhancedAudio.h applies the result as
// EFX filters). libphonon is loaded at run time: without it build() fails
// and the sound stays retail.
#pragma once

#include "Sound/SoundEngine/EnhancedAudio.h"

#include <memory>
#include <vector>

namespace dice::hfe::sound {

class SteamAudioOcclusion final : public IEnhancedOcclusion {
public:
	SteamAudioOcclusion();
	~SteamAudioOcclusion() override; // also takes itself out of setEnhancedOcclusion

	// The scene from `corners` (three a triangle, game world space). False
	// when libphonon can't be loaded or the scene can't be made.
	bool build(const std::vector<Vec3>& corners);
	void simulate(const Vec3& listener, std::vector<EnhancedSource>& sources) override;

	struct Stats {
		size_t triangles = 0;
		double buildSeconds = 0;
		double simulateSeconds = 0;
		long ticks = 0;   // simulate() calls
		long sources = 0; // summed over the ticks
		long muffled = 0; // of those, below half gain
	};
	const Stats& stats() const { return m_stats; }

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
	Stats m_stats;
};

} // namespace dice::hfe::sound
