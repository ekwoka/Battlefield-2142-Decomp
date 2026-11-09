// Enhanced mode's Steam Audio occlusion (Sound/Enhanced/SteamAudioOcclusion.h)
// on a hand-made scene: a wall between listener and source muffles it, a wall
// off to the side doesn't, whichever way the wall's faces wind.
#include "Sound/Enhanced/SteamAudioOcclusion.h"
#include "TestMain.h"

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
		CHECK(behind.gain < 0.5f);
		CHECK(behind.gainHF < 1.0f); // transmission keeps less of the highs
		const EnhancedSource clear = heard(wall(100, 10, flipped));
		CHECK(clear.gain > 0.95f);
		CHECK(clear.gainHF > 0.95f);
	}
}
