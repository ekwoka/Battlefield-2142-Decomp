// The 3D name tags' rules (src/Main/Nametags: RendDX9's NametagManager,
// 0x100f0e60, 0x100f0310, 0x100f0be0, 0x100ef210) and their font.
#include "Main/Nametags.h"
#include "IO/File/FileManager.h"
#include "TestMain.h"

#include <cmath>
#include <cstdlib>
#include <string>

using namespace dice::hfe;

namespace {

float run(const nametags::Settings& s, const nametags::PlayerTagInput& in, nametags::Fade& fade, float seconds)
{
	float alpha = 0;
	for (int i = 0; i < static_cast<int>(seconds * 30 + 0.5f); ++i) {
		alpha = nametags::playerTagAlpha(s, in, fade, 1 / 30.0f);
	}
	return alpha;
}

} // namespace

// Friends are named to 80 m, fading out by 120 (maxFriendlyDistance);
// aimed at, out to 500 m.
TEST(Nametags_FriendlyRange)
{
	const nametags::Settings s;
	nametags::PlayerTagInput in;
	in.friendly = true;
	nametags::Fade fade;
	in.distance = 50;
	CHECK(std::fabs(run(s, in, fade, 0.1f) - 1) < 1e-4f);
	in.distance = 100;
	fade = {};
	CHECK(std::fabs(run(s, in, fade, 0.1f) - 0.5f) < 1e-3f);
	in.distance = 130;
	fade = {};
	CHECK(run(s, in, fade, 0.1f) == 0);
	// Aimed at: faded in over 0.7 s, full to 333 m.
	in.aimedAt = true;
	in.distance = 300;
	fade = {};
	const float half = run(s, in, fade, 0.35f);
	CHECK(half > 0.4f && half < 0.6f);
	CHECK(std::fabs(run(s, in, fade, 1) - 1) < 1e-4f);
	in.distance = 600;
	CHECK(run(s, in, fade, 0.1f) == 0);
	// Down: gone over 0.6 s, or 14 s for a medic.
	in = {};
	in.friendly = true;
	in.distance = 10;
	in.manDown = true;
	fade = {};
	fade.alpha = 1;
	CHECK(run(s, in, fade, 0.7f) == 0);
	in.localMedic = true;
	fade.alpha = 1;
	CHECK(run(s, in, fade, 7) > 0.4f);
}

// Enemies only while aimed at, after a delay of their distance's share of
// 300 m, and gone past 285 m.
TEST(Nametags_EnemiesWhenAimedAt)
{
	const nametags::Settings s;
	nametags::PlayerTagInput in;
	in.distance = 30;
	nametags::Fade fade;
	CHECK(run(s, in, fade, 2) == 0);
	in.aimedAt = true;
	CHECK(run(s, in, fade, 0.05f) == 0); // the 0.1 s delay
	CHECK(run(s, in, fade, 0.3f) > 0.9f);
	CHECK(nametags::playerTagColour(in) == nametags::Enemy);
	in.manDown = true;
	CHECK(run(s, in, fade, 0.1f) == 0);
	in = {};
	in.aimedAt = true;
	in.distance = 290;
	fade = {};
	CHECK(run(s, in, fade, 5) == 0);
	in.friendly = true;
	in.sameSquad = true;
	CHECK(nametags::playerTagColour(in) == nametags::Squad);
}

TEST(Nametags_ShownDistanceSteps)
{
	CHECK(nametags::shownDistance(-1, 20.4f) == 20);
	CHECK(nametags::shownDistance(20, 21.2f) == 21);
	CHECK(nametags::shownDistance(100, 101.6f) == 100);
	CHECK(nametags::shownDistance(100, 103.4f) == 103);
	CHECK(nametags::shownDistance(300, 304) == 300);
	CHECK(nametags::shownDistance(500, 509) == 500);
	CHECK(nametags::shownDistance(500, 510) == 510);
}

// The projection into the 800 x 600 screen, and glueToEdge's circle.
TEST(Nametags_ProjectionAndEdge)
{
	nametags::Nametags::View view;
	view.fovY = 1;
	view.aspect = 4.0f / 3.0f;
	float x = 0, y = 0;
	CHECK(nametags::Nametags::project(view, {0, 0, 10}, x, y, false, 0.65f));
	CHECK(std::fabs(x - 400) < 1e-3f && std::fabs(y - 300) < 1e-3f);
	CHECK(nametags::Nametags::project(view, {1, 1, 10}, x, y, false, 0.65f));
	CHECK(x > 400 && y < 300);
	CHECK(!nametags::Nametags::project(view, {0, 0, -10}, x, y, false, 0.65f));
	bool clamped = false;
	CHECK(nametags::Nametags::project(view, {0, 0, -10}, x, y, true, 0.65f, &clamped));
	CHECK(clamped && std::fabs(std::hypot((x - 400) / 400, (y - 300) / 300) - 0.65f) < 1e-3f);
}

// The tags' font, Fonts/BF1942.font.
TEST(Nametags_Font)
{
	nametags::TagFont font;
	CHECK(font.parse("Texture = Fonts/BF1942\nTextureWidth = 128\nTextureHeight = 128\nBetweenWidth = 1\nHeight = 11\n"
					 "-------\n  0 0 0\n! 0 0 1\nA 31 12 36\n"));
	CHECK(font.texture == "Fonts/BF1942.dds" && font.height == 11);
	CHECK(font.width("A") == 7);
	CHECK(font.width("A A") == 16);
	std::vector<hud::HudQuad> quads;
	const float white[4] = {1, 1, 1, 1};
	font.write("A A", 0, 0, white, quads);
	CHECK(quads.size() == 2 && quads[1].x0 == 9);
	const char* dir = std::getenv("BF2142_GAME_DIR");
	if (!dir) {
		return;
	}
	io::FileManager files;
	files.setRootPath(dir);
	files.addSearchPath("mods/bf2142");
	std::vector<uint8_t> data;
	files.setMountingAllowed(true);
	files.mountArchive("Fonts_client.zip", "Fonts/");
	CHECK(files.readFile("Fonts/BF1942.font", data));
	if (!data.empty()) {
		nametags::TagFont shipped;
		CHECK(shipped.parse(std::string(data.begin(), data.end())));
		CHECK(shipped.present['B'] && shipped.cells['B'].x0 == 37 && shipped.cells['B'].y == 12);
	}
}
