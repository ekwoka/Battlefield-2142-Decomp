// Enhanced mode's Steam Audio occlusion (Sound/Enhanced/SteamAudioOcclusion.h)
// on a hand-made scene: a wall between listener and source muffles it, a wall
// off to the side doesn't, whichever way the wall's faces wind; a muffled
// source keeps half its level and highs, and the filter fades 0.1 an update.
#include "Sound/Enhanced/SteamAudioOcclusion.h"
#include "TestMain.h"

#include <cmath>

using namespace dice::hfe::sound;

namespace {

// A 20 x 20 m wall across z = `z`, centred on x = `x`, as two triangles.
std::vector<Vec3> wall(float x, float z, bool flipped)
{
	const Vec3 a {x - 10, -10, z}, b {x + 10, -10, z}, c {x + 10, 10, z}, d {x - 10, 10, z};
	return flipped ? std::vector<Vec3> {a, c, b, a, d, c} : std::vector<Vec3> {a, b, c, a, c, d};
}

EnhancedSource heard(const std::vector<Vec3>& scene)
{
	SteamAudioOcclusion occlusion;
	CHECK(occlusion.build(scene));
	std::vector<EnhancedSource> sources {{1, {0, 0, 20}}};
	occlusion.simulate({0, 0, 0}, sources);
	CHECK(occlusion.stats().ticks == 1 && occlusion.stats().sources == 1);
	return sources[0];
}

} // namespace

TEST(SteamAudio_WallBetweenMuffles)
{
	for (bool flipped : {false, true}) {
		const EnhancedSource behind = heard(wall(0, 10, flipped));
		CHECK(behind.gain < 0.6f);
		CHECK(behind.gainHF < 0.9f); // transmission keeps less of the highs
		// but never as little as the bare direct path's -20 dB: no diffraction there
		CHECK(behind.gain >= 0.5f);
		CHECK(behind.gainHF >= 0.5f);
		const EnhancedSource clear = heard(wall(100, 10, flipped));
		CHECK(clear.gain > 0.95f);
		CHECK(clear.gainHF > 0.95f);
	}
}

TEST(SteamAudio_TwoWallsStillAudible)
{
	std::vector<Vec3> scene = wall(0, 5, false);
	const std::vector<Vec3> second = wall(0, 15, false);
	scene.insert(scene.end(), second.begin(), second.end());
	const EnhancedSource behind = heard(scene);
	CHECK(behind.gain >= 0.5f && behind.gain < 0.6f);
	CHECK(behind.gainHF >= 0.5f);
}

TEST(SteamAudio_FilterFadesAcrossACorner)
{
	const auto near = [](float a, float b) { return std::fabs(a - b) < 1e-5f; };
	CHECK(near(enhancedSmoothed(1.0f, 0.5f), 0.9f));
	CHECK(near(enhancedSmoothed(0.5f, 1.0f), 0.6f));
	CHECK(near(enhancedSmoothed(0.55f, 0.6f), 0.6f)); // within a step it arrives
	float g = 1.0f;
	int updates = 0;
	while (g > 0.5f + 1e-4f && updates < 100) {
		g = enhancedSmoothed(g, 0.5f);
		++updates;
	}
	CHECK(updates == 5); // a sixth of a second at 30 updates a second
}
