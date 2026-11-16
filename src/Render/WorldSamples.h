// The world target's sample count and the pipelines drawn into it, kept in
// agreement (no GPU code: shared with the tests). GpuContext holds one.
//
// The antialiasing (VideoSettings.setAntialiasing) multisamples the world's
// passes: the most samples at or below the request that both target formats
// take (RendDX9 lists the device's multisample types, 0x1000fb60). The world
// target, its depth, the camouflage's and every pipeline drawn into them
// take that count; alpha to coverage only exists while it is above one.
//
// Not retail: the enhanced mode's lit variants (the GTAO, the cascades;
// Enhanced/LevelRendererEnhanced.cpp) are made single-sampled only, so a
// frame that draws them has a single-sampled world (the antialiasing's
// setting waits, TAA being the enhanced mode's own). The choice is made
// once, before the frame's targets: the variants can be turned off inside
// the frame (the retail single-sampled set then draws), never on while the
// world is multisampled. With the enhanced mode off the count is retail's.
#pragma once

#include <cstdint>
#include <vector>

namespace dice::hfe::render {

// The world's sample count: 1 with the enhanced lit variants, else the most
// of `supported` (the counts above one, ascending) at or below `requested`.
inline uint32_t worldSampleCount(uint32_t requested, bool enhancedLit, const std::vector<uint32_t>& supported)
{
	if (enhancedLit) {
		return 1;
	}
	uint32_t samples = 1;
	for (const uint32_t count : supported) {
		if (count <= requested) {
			samples = count;
		}
	}
	return samples;
}

class WorldSamples {
public:
	// A frame's (or a load's) setting. Returns the count in effect.
	uint32_t set(uint32_t requested, bool enhancedLit, const std::vector<uint32_t>& supported)
	{
		m_samples = worldSampleCount(requested, enhancedLit, supported);
		m_enhancedLit = enhancedLit && m_samples <= 1;
		return m_samples;
	}
	// The multisampled pipelines or targets couldn't be made: single-sampled.
	void fallBackToSingle() { m_samples = 1; }
	// Off at any time; on only while the world is single-sampled.
	void setEnhancedLit(bool on) { m_enhancedLit = on && m_samples <= 1; }
	uint32_t samples() const { return m_samples; }
	bool enhancedLit() const { return m_enhancedLit; }
	// The pipelines drawn into the world: the multisampled set (with alpha
	// to coverage) above one sample, else the single-sampled one or, lit,
	// the enhanced variants.
	bool multisampled() const { return m_samples > 1; }
	bool alphaToCoverage() const { return multisampled(); }

private:
	uint32_t m_samples = 1;
	bool m_enhancedLit = false;
};

} // namespace dice::hfe::render
