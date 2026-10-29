// The renderer's GPU-free logic: the Titans' part culling (TitanParts.h),
// the view's fog (Fog.h), the shadows', hemi maps', sky's and normal maps'
// arithmetic (SceneMath.h), the overhead view's camera (OverheadView.h),
// the quality levels' mip skips and the undergrowth's simple shader, the
// geometry templates' Titan and material settings, and the sprites'
// flipbooks and the effect bundles' age, speed and rotation settings.
#include "Console/Console.h"
#include "Effects/Effects.h"
#include "Geometry/GeometryTemplateManager.h"
#include "Render/EnvMaps.h"
#include "Render/Fog.h"
#include "Render/OverheadView.h"
#include "Render/SceneMath.h"
#include "Render/TitanParts.h"
#include "Scene/EffectSettings.h"
#include "Scene/LightSettings.h"
#include "TestMain.h"
#include "World/ObjectTemplateManager.h"

#include <cmath>
#include <string>
#include <vector>

using namespace dice::hfe;

namespace {

bool near(float a, float b, float epsilon = 1e-4f)
{
	return std::fabs(a - b) < epsilon;
}

} // namespace

TEST(RenderScene_TitanPartsCulledAsRendDX9)
{
	using render::TitanDraw;
	const render::TitanView outside {false, false}, inside {true, false}, all {true, true};
	// Static meshes (0x100bdb00): the rooms only from inside, the rest only from outside.
	CHECK(render::titanStaticDraw(outside, true) == TitanDraw::Skip);
	CHECK(render::titanStaticDraw(outside, false) == TitanDraw::Normal);
	CHECK(render::titanStaticDraw(inside, true) == TitanDraw::Interior);
	CHECK(render::titanStaticDraw(inside, false) == TitanDraw::Skip);
	CHECK(render::titanStaticDraw(all, false) == TitanDraw::Normal); // showAllTitanParts
	// Bundled (0x100c5030): from inside no exterior parts and nothing past 53 m.
	CHECK(render::titanBundledDraw(outside, true, 500, 53) == TitanDraw::Normal);
	CHECK(render::titanBundledDraw(inside, true, 5, 53) == TitanDraw::Skip);
	CHECK(render::titanBundledDraw(inside, false, 60, 53) == TitanDraw::Skip);
	CHECK(render::titanBundledDraw(inside, false, 50, 53) == TitanDraw::Interior);
	// Skinned (0x10162070): nothing past indoorSkinnedMeshCullDistance.
	CHECK(render::titanSkinnedDraw(inside, 54, 53) == TitanDraw::Skip);
	CHECK(render::titanSkinnedDraw(inside, 53, 53) == TitanDraw::Interior);
	CHECK(render::titanSkinnedDraw(outside, 1000, 53) == TitanDraw::Normal);

	// The hull parts' LOD pairs (0x100baf40 / 0x100baf90 / 0x100bb470).
	render::TitanExteriorLods lods;
	CHECK(lods.resolve(1, 2, 3) == 0); // the pairs start at 0
	CHECK(lods.resolve(2, 1, 3) == 0);
	CHECK(lods.resolve(3, 3, 3) == 0);
	CHECK(lods.resolve(4, 2, 3) == 0);
	lods.endFrame();
	CHECK(lods.resolve(1, 2, 3) == 1); // min(2, 1)
	CHECK(lods.resolve(4, 2, 3) == 2); // min(3, 2)
	CHECK(lods.resolve(0, 0, 3) == 1); // part 0 reads the first pair, records nothing
	CHECK(lods.resolve(3, 3, 1) == 1); // capped at the count
}

TEST(RenderScene_GeometryTemplateTitanAndMaterialSettings)
{
	Console console;
	geom::GeometryTemplateManager geometries;
	geom::registerGeometryConsoleMethods(console, geometries);
	std::string out;
	CHECK(console.execute("GeometryTemplate.create StaticMesh eu_titan_exterior_main_front", out) == ConsoleResult::Ok);
	CHECK(console.execute("GeometryTemplate.isTitanInterior 1", out) == ConsoleResult::Ok);
	CHECK(console.execute("GeometryTemplate.isTitanExterior 1", out) == ConsoleResult::Ok);
	CHECK(console.execute("GeometryTemplate.titanExteriorPartIndex 1", out) == ConsoleResult::Ok);
	CHECK(console.execute("GeometryTemplate.setMaterialParallaxHeightScale 0 0 3 25", out) == ConsoleResult::Ok);
	CHECK(console.execute("GeometryTemplate.setSpecularStaticGloss 0 0 0 0.5", out) == ConsoleResult::Ok);
	const geom::GeometryTemplate* hull = geometries.find("eu_titan_exterior_main_front");
	// One excludes the other (0x10148670 / 0x10148690).
	CHECK(hull && hull->titanExterior && !hull->titanInterior && hull->titanExteriorPart == 1);
	CHECK(hull && hull->parallaxHeightScale.at(geom::GeometryTemplate::materialKey(0, 0, 3)) == 25);
	CHECK(hull && hull->staticGloss.empty()); // bundled templates only (0x10073eb9)
	CHECK(console.execute("GeometryTemplate.create BundledMesh unl_shotgun_02", out) == ConsoleResult::Ok);
	CHECK(console.execute("GeometryTemplate.setSpecularStaticGloss 1 0 0 0", out) == ConsoleResult::Ok);
	CHECK(console.execute("GeometryTemplate.setMaterialReflectionScale 1 0 2 0.25", out) == ConsoleResult::Ok);
	CHECK(console.execute("GeometryTemplate.nrOfAnimatedUVMatrix 6", out) == ConsoleResult::Ok);
	const geom::GeometryTemplate* gun = geometries.find("unl_shotgun_02");
	CHECK(gun && gun->staticGloss.at(geom::GeometryTemplate::materialKey(1, 0, 0)) == 0);
	CHECK(gun && gun->reflectionScale.at(geom::GeometryTemplate::materialKey(1, 0, 2)) == 0.25f);
	CHECK(gun && gun->animatedUVMatrices == 6);
	CHECK(geom::GeometryTemplate::materialKey(1, 2, 3) == 0x10203u);
}

TEST(RenderScene_ViewFogUnderTheSea)
{
	// Cerbere Landing's Sky.con, the sea at 34 m.
	const std::array<float, 3> fogColor {110, 123, 132}, waterColor {62, 77, 94};
	const std::array<float, 4> fog {0, 140, 1, 0.5f}, water {0, 10, 2, 0};
	const render::ViewFog above = render::viewFog(fogColor, fog, waterColor, water, true, 34, 40, 1);
	CHECK(!above.underwater && !above.underwaterSky && near(above.color[0], 110 / 255.0f));
	CHECK(above.startEndAndBase[1] == 140);
	// At the surface the water's fog, but the sky dome still the sky.
	const render::ViewFog surface = render::viewFog(fogColor, fog, waterColor, water, true, 34, 34, 1);
	CHECK(surface.underwater && !surface.underwaterSky);
	const render::ViewFog below = render::viewFog(fogColor, fog, waterColor, water, true, 34, 30, 0.25f);
	CHECK(below.underwater && below.underwaterSky && near(below.color[2], 94 / 255.0f));
	// The end and the fourth value by the view distance, at least 0.5 (0x10028020).
	CHECK(near(below.startEndAndBase[1], 5) && below.startEndAndBase[2] == 2);
	// Without a sea (-1000) never under water.
	CHECK(!render::viewFog(fogColor, fog, waterColor, water, false, 34, -10, 1).underwater);
	// RaCommon.fx calcFog: clear at the start, gone at the end.
	CHECK(near(render::fogVisibility(fog, 0), 1));
	CHECK(near(render::fogVisibility(fog, 70), 0.375f)); // max(0.5, 0.5) - 0.125
	CHECK(near(render::fogVisibility(fog, 140), 0));
	// FogRange as 0x100355c0 makes it, in metres (Suez Canal: -10 / 355 / 0.2 / 0).
	const auto range = render::fogRange({-10, 355, 0.2f, 0});
	CHECK(near(range[0], 1 / 365.0f) && near(range[1], -0.2f / 365) && near(range[2], 10 / 365.0f) &&
		near(range[3], 355 / 365.0f));

	// The console names (0x1004f4a0 / 0x1004f740, terrain.water*).
	Console console;
	scene::WaterSettings settings;
	scene::registerWaterConsoleMethods(console, settings);
	std::string out;
	CHECK(console.execute("renderer.waterFogColor 62.00/77.00/94.00", out) == ConsoleResult::Ok);
	CHECK(console.execute("renderer.waterFogStartEndAndBase 0.00/10.00/2.00/0.00", out) == ConsoleResult::Ok);
	CHECK(console.execute("terrain.waterSunIntensity 0.8", out) == ConsoleResult::Ok);
	CHECK(console.execute("terrain.waterSpecularPower 12", out) == ConsoleResult::Ok);
	CHECK(settings.fogColor[1] == 77 && settings.fogStartEndAndBase[1] == 10 && near(settings.sunIntensity, 0.8f));
	CHECK(settings.specularPower == 12);
}

TEST(RenderScene_ShadowRegionAndHemiMaps)
{
	CHECK(render::shadowRegionRadius(3, 40) == 80 && render::shadowRegionRadius(2, 40) == 60);
	CHECK(render::shadowRegionRadius(1, 4) == 10);
	CHECK(render::shadowMapTexels(3, 1024) == 2048 && render::shadowMapTexels(1, 100) == 256);
	CHECK(render::shadowMapTexels(3, 4096) == 4096);
	// Half the region ahead, snapped to half the radius: still while the
	// camera moves within a step.
	const render::Vec3f a = render::shadowRegionCenter({1, 3, 2}, {0, -0.5f, 1}, 40);
	const render::Vec3f b = render::shadowRegionCenter({4, 3, 5}, {0, 0, 1}, 40);
	CHECK(a.x == 0 && a.y == 0 && a.z == 20);
	CHECK(b.x == 0 && b.z == 20);

	// A BC3 alpha block: 8 levels between the ends, then 6 with 0 and 255.
	const uint8_t eight[8] = {255, 0, 0b10001000, 0, 0, 0, 0, 0}; // texels' indices 0, 1, 2, 0
	uint8_t alpha[16];
	render::decodeBc3Alpha(eight, alpha);
	CHECK(alpha[0] == 255 && alpha[1] == 0 && alpha[2] == 218 && alpha[3] == 255); // 2: (6 x 255 + 0) / 7
	const uint8_t six[8] = {0, 100, 0b00111110, 0, 0, 0, 0, 0}; // texel 0: 6 -> 0, texel 1: 7 -> 255
	render::decodeBc3Alpha(six, alpha);
	CHECK(alpha[0] == 0 && alpha[1] == 255 && alpha[2] == 0);

	// The base map's texel (vsParticle's HemiMapInfo, v flipped).
	int px = 0, py = 0;
	render::hemiMapTexel(0, 0, 0, 0, 2048, 512, px, py);
	CHECK(px == 256 && py == 256);
	render::hemiMapTexel(-1024, -1024, 0, 0, 2048, 512, px, py);
	CHECK(px == 0 && py == 511);
	render::hemiMapTexel(5000, 5000, 0, 0, 2048, 512, px, py);
	CHECK(px == 511 && py == 0);

	// A Titan map: over its square and above its floor, bilinear.
	const render::Vec3f axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	const float offset[3] = {2, -10, -33};
	const render::Vec3f local = render::titanMapLocal({100, 150, 0}, axes, offset, {102, 145, -33});
	CHECK(local.x == 0 && local.y == 5 && local.z == 0);
	CHECK(render::overTitanMap(local, 128) && !render::overTitanMap({0, -1, 0}, 128) && !render::overTitanMap({70, 1, 0}, 128));
	std::vector<uint8_t> rgba(4 * 4 * 4, 0);
	rgba[(1 * 4 + 1) * 4] = 255; // texel (1, 1) red
	float colour[3];
	render::titanHemiSample(rgba.data(), 4, 4, 4, {1, 1, -1}, colour); // u = 1, v = 1
	CHECK(near(colour[0], 1) && near(colour[1], 0));
	render::titanHemiSample(rgba.data(), 4, 4, 4, {0.5f, 1, -1}, colour); // u = 1.5
	CHECK(near(colour[0], 0.5f));

	// HeightOverTerrain: in radii over the ground less one, at least 2 m radii.
	CHECK(near(render::heightOverTerrain(3, 1), 1));     // (3 - 1) / 2
	CHECK(near(render::heightOverTerrain(6, 1), 1.5f));  // past 4 radii: (4 - 1) / 2
	CHECK(near(render::heightOverTerrain(-1, 3), 3));    // nothing hit: 4 radii
	CHECK(near(render::heightOverTerrain(1, 3), 0));
	// The clouds wrap within a tile.
	CHECK(near(render::cloudOffset(0.9f, 0.5f, 0.4f), 0.1f));
}

TEST(RenderScene_NormalMapsAndMipSkips)
{
	CHECK(render::normalMapKind(true, false, "vehicles/a/textures/hull_b.dds", "base") == 1);
	CHECK(render::normalMapKind(true, false, "vehicles/a/textures/hull_c.dds", "base") == 0);
	CHECK(render::normalMapKind(false, true, "soldiers/a_b_os.dds", "humanskin") == 2);
	CHECK(render::normalMapKind(false, true, "soldiers/a_b.dds", "humanskintangent") == 1);
	CHECK(render::normalMapKind(false, true, "shaders/specularlut_pow36.dds", "") == 0);
	CHECK(render::normalMapKind(false, false, "x_b.dds", "") == 0);

	// GSSkipMipLevels by the texture quality, and its classes (0x10010040).
	scene::QualitySettings q;
	q.texture = 1;
	CHECK(q.skipMipLevels(scene::TextureClass::StaticMesh) == 2);
	CHECK(q.skipMipLevels(scene::TextureClass::Undergrowth) == 1);
	CHECK(q.skipMipLevels(scene::TextureClass::Interface) == 0);
	q.texture = 3;
	CHECK(q.skipMipLevels(scene::TextureClass::Overgrowth) == 0);
	// The lighting level's normal maps (renderer +0x6b4).
	q.lighting = 2;
	CHECK(!q.meshNormalMaps() && q.meshHemiAndShadows());

	// undergrowth.simpleShaderEnable holds until the settings are applied again.
	Console console;
	scene::QualitySettings video;
	scene::registerQualityConsoleMethods(console, video);
	std::string out;
	CHECK(video.undergrowthSimpleShader() == false); // the high terrain quality
	CHECK(console.execute("undergrowth.simpleShaderEnable 1", out) == ConsoleResult::Ok);
	CHECK(video.undergrowthSimpleShader());
	CHECK(console.execute("VideoSettings.setTerrainQuality 3", out) == ConsoleResult::Ok);
	CHECK(!video.undergrowthSimpleShader());
	CHECK(console.execute("VideoSettings.setTerrainQuality 1", out) == ConsoleResult::Ok);
	CHECK(video.undergrowthSimpleShader());
}

TEST(RenderScene_OverheadCameraPlace)
{
	render::OverheadView view;
	view.x = 10;
	view.z = 2000;
	view.height = 260;
	const float origin[2] = {-1024, -1024};
	float xs[5], zs[5];
	render::overheadGroundPoints(view, origin, 2048, xs, zs);
	CHECK(xs[0] == 10 && zs[0] == 944 && zs[1] == 946 && xs[3] == 12); // 80 m inside the edge
	const float heights[5] = {10, 20, 30, 40, 50};
	const render::Vec3f at = render::overheadCameraPosition(view, origin, 2048, heights, -1e9f);
	CHECK(at.x == 10 && at.z == 944 && near(at.y, 290));
	const render::Vec3f sea = render::overheadCameraPosition(view, origin, 2048, heights, 100);
	CHECK(near(sea.y, 360)); // the sea's height when higher
	const render::Vec3f small = render::overheadCameraPosition(view, origin, 100, heights, -1e9f);
	CHECK(small.z == 2000); // a small terrain: no clamp
}

namespace {

void addSprite(world::ObjectTemplateManager& templates, const char* name)
{
	world::ObjectTemplate* sprite = templates.create("SpriteParticleSystem", name);
	sprite->settings()["emitFrequency"] = "10";
	sprite->settings()["emitTime"] = "0.2";
	sprite->settings()["timeToLive"] = "2";
	sprite->settings()["gravity"] = "0";
	sprite->settings()["textureName"] = "flipbook";
}

} // namespace

TEST(RenderScene_SpriteFlipbooks)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "EffectBundle");
	templates.registerType(2, "SpriteParticleSystem");
	addSprite(templates, "anim");
	world::ObjectTemplate* anim = templates.find("anim");
	anim->settings()["animationEnable"] = "1";
	anim->settings()["animationFrameCount"] = "4";
	anim->settings()["animationFrameCountX"] = "2";
	anim->settings()["setAnimationFrameWidthRelative"] = "0.5";
	anim->settings()["setAnimationFrameHeightRelative"] = "0.5";
	anim->settings()["animationSpeed"] = "1.73913";
	anim->settings()["animationPlayOnce"] = "1";
	anim->settings()["showInZoomedMode"] = "0";
	const effects::ParticleTemplate t = effects::ParticleTemplate::fromTemplate(*anim);
	// animationPlayOnce: frames / timeToLive (0x10138790).
	CHECK(t.animated && near(t.animationSpeed, 2) && !t.showZoomed && t.selfShadowIntensity == 1);
	// The frame: the clock's whole part modulo the frames, 2 across (0x10131f80).
	auto frame = effects::spriteFrame(t, 0.5f);
	CHECK(frame[0] == 0 && frame[1] == 0 && frame[2] == 0.5f && frame[3] == 0.5f);
	frame = effects::spriteFrame(t, 3.2f);
	CHECK(frame[0] == 0.5f && frame[1] == 0.5f);
	frame = effects::spriteFrame(t, 5.0f); // wraps
	CHECK(frame[0] == 0.5f && frame[1] == 0);

	world::ObjectTemplate* bundle = templates.create("EffectBundle", "e_anim");
	bundle->children().push_back({"anim", {}, {}});
	effects::EffectManager manager(templates);
	CHECK(manager.spawn("e_anim", {}));
	for (int i = 0; i < 30; ++i) { // 1 s: the first particles two frames on
		manager.update(1 / 30.0f);
	}
	bool advanced = false;
	manager.forEachGroup([&](const std::string&, bool, const std::vector<effects::Sprite>& sprites) {
		for (const effects::Sprite& s : sprites) {
			advanced = advanced || s.frame[0] > 0;
		}
	});
	CHECK(advanced);
	// A zoomed first person leaves it out.
	effects::ParticleView zoomed;
	zoomed.zoomed = true;
	size_t shown = 0;
	manager.forEachGroup(zoomed, [&](const std::string&, bool, const std::vector<effects::Sprite>& s) { shown += s.size(); });
	CHECK(shown == 0);
}

TEST(RenderScene_BundleAgeSpeedAndRotation)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "EffectBundle");
	templates.registerType(2, "SpriteParticleSystem");
	addSprite(templates, "plain");
	addSprite(templates, "upright");
	templates.find("upright")->settings()["noRotationFromParent"] = "1";
	world::ObjectTemplate* bundle = templates.create("EffectBundle", "e_bundle");
	bundle->settings()["randomAgeFactor"] = "CRD_NONE/0.5/0/0";
	bundle->settings()["speedFromPhysics"] = "1";
	bundle->children().push_back({"plain", {0, 0, 2}, {}});
	bundle->children().push_back({"upright", {0, 0, 2}, {}});
	effects::EffectManager manager(templates);
	// Turned 90 degrees about y: the plain system's offset turns with it.
	const effects::Mat3 turn = effects::Mat3::fromYawPitchRoll({90, 0, 0});
	CHECK(manager.spawn("e_bundle", {10, 0, 0}, turn));
	const auto& emitters = manager.emitterList();
	CHECK(emitters.size() == 2);
	if (emitters.size() == 2) {
		const effects::Emitter& plain = *emitters[0];
		const effects::Emitter& upright = *emitters[1];
		// randomAgeFactor scales both systems' age (0x00619f90).
		CHECK(plain.timeScale == 0.5f && upright.timeScale == 0.5f);
		CHECK(plain.physicsVelocity && upright.physicsVelocity);
		// noRotationFromParent: the offset unturned, its own rotation.
		CHECK(near(upright.position().x, 10) && near(upright.position().z, 2));
		CHECK(!(near(plain.position().x, 10) && near(plain.position().z, 2)));
	}
	// speedFromPhysics: the direction times the square root of the speed (0x00618ae0).
	CHECK(manager.spawnAttached("e_bundle", 7, {}, {}, effects::EffectManager::View::Any, 3));
	manager.setPhysicsVelocity(7, 3, {0, 0, 16});
	bool set = false;
	for (const auto& e : manager.emitterList()) {
		if (e->anchor == 7) {
			set = near(e->inheritedVelocity.z, 4) && near(e->inheritedVelocity.x, 0);
		}
	}
	CHECK(set);
}

TEST(RenderScene_EnvMapsAndNearFade)
{
	// Belgrade's EnvironmentMapInfo.emi (0x10064770) and the nearest map (0x10064f30).
	const auto maps = render::parseEnvMapInfo(
		"# EnvironmentMapInformation file\n# Version 0\n#\nEnvMap0 -88,54,66,1 \r\nEnvMap1 100,20,-5,0\n");
	CHECK(maps.size() == 2 && maps[0].name == "EnvMap0" && maps[0].flag && near(maps[0].position[2], 66));
	CHECK(render::nearestEnvMap(maps, {90, 0, 0}) == 1 && render::nearestEnvMap(maps, {-80, 50, 60}) == 0);
	CHECK(render::nearestEnvMap({}, {0, 0, 0}) == -1);
	// nearCullingEnable (0x101318e0): beyond 6 m past the near plane or
	// 0.15 m and smaller, none; a big particle at the near plane, gone.
	CHECK(effects::ParticleView::nearFade(10, 0.1f, 1) < 0);
	CHECK(effects::ParticleView::nearFade(1, 0.1f, 0.1f) < 0);
	CHECK(near(effects::ParticleView::nearFade(0.1f, 0.1f, 1), 0));
	// 5 m past it: t = 0.5, f = 1 - 0.05 x 4 = 0.8: 0.5 + 0.5 x 0.8.
	CHECK(near(effects::ParticleView::nearFade(5.1f, 0.1f, 0.2f), 0.9f));
}
