// The post-production's colour grade, EMP table and timed effects
// (src/Render/PostProduction.h: RendDX9 0x10049900, 0x10048a30,
// 0x1004a120, 0x1004a2f0) and the RendDX9 console settings the renderer
// reads (src/Scene/PostProductionSettings.h, LightSettings.h).
#include "Render/PostProduction.h"
#include "Scene/LightSettings.h"
#include "Scene/PostProductionSettings.h"
#include "Console/Console.h"
#include "TestMain.h"

#include <cmath>
#include <string>

using namespace dice::hfe;

namespace {

bool near(float a, float b, float epsilon = 1e-4f)
{
	return std::fabs(a - b) < epsilon;
}

// out_r = sum_c rows[r][c] in_c.
std::array<float, 3> apply(const float rows[3][4], const std::array<float, 3>& in)
{
	std::array<float, 3> out {};
	for (int r = 0; r < 3; ++r) {
		out[static_cast<size_t>(r)] = rows[r][0] * in[0] + rows[r][1] * in[1] + rows[r][2] * in[2];
	}
	return out;
}

} // namespace

TEST(PostProduction_ColorGradeSaturationAndTint)
{
	float rows[3][4];
	// Saturation 1 and a white tint: the identity.
	render::colorGrade({1, 1, 1}, 1, rows);
	CHECK(near(rows[0][0], 1) && near(rows[0][1], 0) && near(rows[1][1], 1) && near(rows[2][2], 1) && near(rows[2][0], 0));
	// Saturation 0: every channel the luminance (0.3086, 0.6094, 0.0820).
	render::colorGrade({1, 1, 1}, 0, rows);
	const auto grey = apply(rows, {1, 0, 0});
	CHECK(near(grey[0], 0.3086f) && near(grey[1], 0.3086f) && near(grey[2], 0.3086f));
	const auto white = apply(rows, {1, 1, 1});
	CHECK(near(white[0], 1) && near(white[1], 1) && near(white[2], 1));
	// The tint scales each input channel before the saturation (M x T).
	render::colorGrade({2, 1, 0.5f}, 1, rows);
	const auto tinted = apply(rows, {0.25f, 0.5f, 1});
	CHECK(near(tinted[0], 0.5f) && near(tinted[1], 0.5f) && near(tinted[2], 0.5f));
	// Camp Gibraltar's: tint 1.2/1/0.8, saturation 1.1.
	render::colorGrade({1.2f, 1, 0.8f}, 1.1f, rows);
	const auto gib = apply(rows, {0.5f, 0.5f, 0.5f});
	const float lum = (0.3086f * 1.2f + 0.6094f + 0.0820f * 0.8f) * 0.5f;
	CHECK(near(gib[0], -0.1f * lum + 1.1f * 0.6f) && near(gib[2], -0.1f * lum + 1.1f * 0.4f));
}

TEST(PostProduction_EmpTableByLevel)
{
	scene::PostProductionSettings settings;
	// 0x10048950's table: level 0 nothing, 1-5 noise 8 and roll 0.02, no warp.
	const auto none = render::empParameters(0, settings);
	CHECK(none[0] == 0 && none[1] == 0 && none[2] == 0 && none[3] == 0);
	const auto two = render::empParameters(2, settings);
	CHECK(near(two[0], 8) && near(two[1], 0.02f) && two[2] == 0 && two[3] == 0);
	CHECK(near(render::empParameters(9, settings)[0], 8)); // past the table: its last
	// renderer.empWarpAmount / empVerticalRoll override.
	settings.empWarpAmount = 0.3f;
	settings.empVerticalRoll = 0.1f;
	const auto custom = render::empParameters(1, settings);
	CHECK(near(custom[1], 0.1f) && near(custom[2], 0.3f));
}

TEST(PostProduction_FrameModes)
{
	scene::PostProductionSettings settings;
	settings.bloomOverlayAmount = 0.1f;
	render::PostProductionState state;
	render::PostEffects effects;
	effects.frameSeconds = 1.0f / 30;
	render::PostUniforms u = state.update(effects, settings, false);
	int mode = static_cast<int>(u.contrast[3]);
	CHECK((mode & render::kPostBloom) && (mode & render::kPostGrade) && !(mode & render::kPostEmp));
	CHECK(near(u.bloom[0], 0.1f));
	// The menu's bloom off: no Soften.
	settings.useBloom = false;
	mode = static_cast<int>(state.update(effects, settings, false).contrast[3]);
	CHECK(!(mode & render::kPostBloom));
	settings.useBloom = true;
	// A vehicle's EMP: the static and a neutral second grade; with the
	// view's colour, saturation 0 and its tint.
	effects.vehicleEmpLevel = 2;
	effects.viewTinted = true;
	effects.viewTint = {1, 0.5f, 0.5f};
	u = state.update(effects, settings, false);
	mode = static_cast<int>(u.contrast[3]);
	CHECK((mode & render::kPostEmp) && near(u.emp[0], 8));
	CHECK(near(u.empColorRow[0][0], 0.3086f) && near(u.empColorRow[0][1], 0.6094f * 0.5f));
	// No EMP but the view's colour: the second grade alone.
	effects.vehicleEmpLevel = 0;
	mode = static_cast<int>(state.update(effects, settings, false).contrast[3]);
	CHECK(!(mode & render::kPostEmp) && (mode & render::kPostViewGrade));
	effects.viewTinted = false;
	// A soldier's: level + 3 (5 for 3), the saturation swinging from 0.
	effects.soldierEmpLevel = 1;
	u = state.update(effects, settings, false);
	CHECK((static_cast<int>(u.contrast[3]) & render::kPostEmp) && near(u.emp[0], 8));
	CHECK(near(u.empColorRow[1][1], 0.6094f * 0.99f + 0.01f, 1e-3f)); // one frame's 0.01
	effects.soldierEmpLevel = 0;
	// The camera effect takes the grade's place; the camouflage needs drawn cloaks.
	effects.cameraEffect = true;
	mode = static_cast<int>(state.update(effects, settings, true).contrast[3]);
	CHECK((mode & render::kPostCameraEffect) && !(mode & render::kPostGrade) && (mode & render::kPostCamo));
}

TEST(PostProduction_TinnitusTimeline)
{
	scene::PostProductionSettings settings;
	render::PostProductionState state;
	render::PostEffects effects;
	effects.frameSeconds = 0.5f;
	effects.tinnitusSeconds = 7; // Sound.tinnitusSetup's 1 + 2 + 4
	render::PostUniforms u = state.update(effects, settings, false);
	int mode = static_cast<int>(u.contrast[3]);
	// It takes the bloom's place, starting from the old frames (bias 0).
	CHECK((mode & render::kPostTinnitus) && !(mode & render::kPostBloom) && near(u.tinnitus[2], 0));
	effects.tinnitusSeconds = 0;
	for (int i = 0; i < 5; ++i) {
		u = state.update(effects, settings, false); // 0.5 .. 2.5 s in
	}
	// Under half way the progress is halved: 2.5 / 7 / 2.
	CHECK(near(u.tinnitus[2], 2.5f / 7 * 0.5f, 1e-3f));
	for (int i = 0; i < 4; ++i) {
		u = state.update(effects, settings, false);
	}
	CHECK(near(u.tinnitus[2], 4.5f / 7, 1e-3f));
	for (int i = 0; i < 5; ++i) {
		state.update(effects, settings, false);
	}
	CHECK(!state.tinnitusActive());
	mode = static_cast<int>(state.update(effects, settings, false).contrast[3]);
	CHECK(!(mode & render::kPostTinnitus) && (mode & render::kPostBloom));
}

TEST(PostProduction_TitanFlash)
{
	scene::PostProductionSettings settings;
	render::PostProductionState state;
	render::PostEffects effects;
	effects.frameSeconds = 0.5f;
	effects.titanExplosion = true;
	state.update(effects, settings, false); // t = 0
	effects.titanExplosion = false;
	// 1 s in: the tint 1 + 20 t, the contrast's constant t / 2.
	state.update(effects, settings, false);
	render::PostUniforms u = state.update(effects, settings, false);
	CHECK(near(u.colorRow[0][0], 21) && near(u.contrast[2], 0.5f));
	// Then white (tint 40, saturation 0, + 1) until 12 s.
	for (int i = 0; i < 4; ++i) {
		u = state.update(effects, settings, false);
	}
	CHECK(near(u.colorRow[0][1], 40 * 0.6094f) && near(u.contrast[2], 1));
	for (int i = 0; i < 20; ++i) {
		u = state.update(effects, settings, false);
	}
	CHECK(!state.flashActive() && near(u.colorRow[0][0], 1) && near(u.contrast[2], 0));
}

TEST(PostProduction_ConsoleSettings)
{
	Console console;
	scene::PostProductionSettings post;
	scene::ShadowSettings shadows;
	scene::HemiMapSettings hemi;
	scene::LightSettings lights;
	scene::SkySettings sky;
	scene::registerPostProductionConsoleMethods(console, post);
	scene::registerShadowConsoleMethods(console, shadows);
	scene::registerHemiMapConsoleMethods(console, hemi);
	scene::registerLightConsoleMethods(console, lights);
	scene::registerSkyConsoleMethods(console, sky);
	std::string out;
	// Camp Gibraltar's Sky.con.
	CHECK(console.execute("Renderer.BloomFilterContrast 0.2", out) == ConsoleResult::Ok);
	CHECK(console.execute("Renderer.DefaultColorTint 1.20/1.00/0.80", out) == ConsoleResult::Ok);
	CHECK(console.execute("Renderer.DefaultContrast 2.10/-1.00/-0.04", out) == ConsoleResult::Ok);
	CHECK(console.execute("hemiMapManager.setBaseHemiMap Levels/Camp_Gibraltar/groundhemi 0.000000/0.000000/0.000000 2048.000000 500.000000", out) == ConsoleResult::Ok);
	CHECK(console.execute("Lightmanager.treeSkyColor 2.32/2.56/3.07", out) == ConsoleResult::Ok);
	CHECK(console.execute("Lightmanager.enableSun 1", out) == ConsoleResult::Ok);
	CHECK(console.execute("Skydome.hasCloudLayer 1", out) == ConsoleResult::Ok);
	CHECK(console.execute("Skydome.scrolldirection 0.003/0.003", out) == ConsoleResult::Ok);
	CHECK(console.execute("Skydome.flareDirection -0.32/-0.89/-0.32", out) == ConsoleResult::Ok);
	CHECK(near(post.bloomFilterContrast, 0.2f) && near(post.defaultColorTint[0], 1.2f) && near(post.defaultContrast[1], -1));
	CHECK(hemi.baseHemiMap == "Levels/Camp_Gibraltar/groundhemi" && near(hemi.size, 2048) && near(hemi.height, 500));
	CHECK(near(lights.treeSkyColor[2], 3.07f) && lights.enableSun);
	CHECK(sky.hasCloudLayer && near(sky.scrollDirection[1], 0.003f) && near(sky.flareDirection[1], -0.89f));
	// shadowManager.*: its defaults (0x100aafe0) and the setters' checks.
	CHECK(shadows.shadowMapSize == 1024 && near(shadows.shadowMapDepthDistance, 40) && !shadows.allowAlphaShadows);
	CHECK(console.execute("shadowManager.shadowMapSize 1000", out) == ConsoleResult::Ok); // not a power of two
	CHECK(shadows.shadowMapSize == 1024);
	CHECK(console.execute("shadowManager.shadowMapSize 2048", out) == ConsoleResult::Ok);
	CHECK(console.execute("shadowManager.shadowMapObjectSize 4096", out) == ConsoleResult::Ok); // over the map's
	CHECK(shadows.shadowMapSize == 2048 && shadows.shadowMapObjectSize == 512);
	CHECK(console.execute("shadowManager.shadowMapBitDepth 32", out) == ConsoleResult::Ok);
	CHECK(shadows.shadowMapBitDepth == 32);
	CHECK(console.execute("VideoSettings.setDynamicShadowsQuality 0", out) == ConsoleResult::Ok);
	CHECK(console.execute("undergrowth.dynamicShadowsEnable 0", out) == ConsoleResult::Ok);
	CHECK(shadows.quality == 0 && !shadows.undergrowthShadows);
	CHECK(console.execute("renderer.drawPostProduction 0", out) == ConsoleResult::Ok);
	CHECK(!post.drawPostProduction);
}

// The commander's overhead view: its fov over the HUD's map rectangle,
// the screen around it going on (render::overheadProjection).
#include "Render/OverheadView.h"

TEST(PostProduction_OverheadProjection)
{
	render::OverheadView view;
	view.fovY = 0.3f;
	view.rect[0] = 100;
	view.rect[1] = 0;
	view.rect[2] = 700;
	view.rect[3] = 600;
	// A 1280 x 720 target: the 600 x 600 square is 960 x 720 pixels.
	const render::Mat4 p = render::overheadProjection(view, 1280, 720, 1, 6000);
	const auto ndc = [&p](float x, float y, float z) {
		const float cx = x * p.at(0, 0) + z * p.at(2, 0), cy = y * p.at(1, 1) + z * p.at(2, 1), w = z;
		return std::array<float, 2> {cx / w, cy / w};
	};
	const float t = std::tan(0.15f);
	// The top-left and bottom-right corners of the view (aspect 960 / 720)
	// at the rectangle's corners (NDC x -0.75 .. 0.75, y 1 .. -1).
	const auto a = ndc(-t * 960 / 720, t, 1), b = ndc(t * 960 / 720, -t, 1);
	CHECK(near(a[0], -0.75f) && near(a[1], 1) && near(b[0], 0.75f) && near(b[1], -1));
	// The ground width it spans: 2 tan(0.15) 260 = 78.6 m.
	CHECK(near(render::overheadGroundWidth(0.3f, 260), 2 * t * 260));
	// Depth 0 at the near plane, 1 at the far one.
	CHECK(near((1 * p.at(2, 2) + p.at(3, 2)) / 1, 0) && near((6000 * p.at(2, 2) + p.at(3, 2)) / 6000, 1));
}
