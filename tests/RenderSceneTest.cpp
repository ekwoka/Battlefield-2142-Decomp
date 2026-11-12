// The renderer's GPU-free logic: the Titans' part culling (TitanParts.h),
// the view's fog (Fog.h), the shadows', hemi maps', sky's and normal maps'
// arithmetic (SceneMath.h), the overhead view's camera (OverheadView.h),
// the quality levels' mip skips and the undergrowth's simple shader, the
// geometry templates' Titan and material settings, and the sprites'
// flipbooks and the effect bundles' age, speed and rotation settings.
#include "Console/Console.h"
#include "Effects/Effects.h"
#include "Geometry/GeometryTemplateManager.h"
#include "Render/AnimatedUV.h"
#include "Render/DistanceFade.h"
#include "Render/MeshLights.h"
#include "Render/OvergrowthLod.h"
#include "Render/EnvMaps.h"
#include "Render/Fog.h"
#include "Render/OverheadView.h"
#include "Render/RoadStatics.h"
#include "Render/SceneMath.h"
#include "Render/SimpleShadowmap.h"
#include "Render/StaticLightmaps.h"
#include "Render/TextureSuffix.h"
#include "Render/TitanParts.h"
#include "Scene/EffectSettings.h"
#include "Scene/LightSettings.h"
#include "Scene/OvergrowthSettings.h"
#include "TestMain.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <array>
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
	// A zoomed first person keeps it (showInZoomedMode 0, showInFirstPerson
	// 1: 0x0061a1c0's flags & 5 still hold; zoomed only picks the view).
	effects::ParticleView zoomed;
	zoomed.zoomed = true;
	size_t shown = 0;
	manager.forEachGroup(zoomed, [&](const std::string&, bool, const std::vector<effects::Sprite>& s) { shown += s.size(); });
	CHECK(shown > 0);
	// In first person a system shows with either flag; neither: not started.
	world::ObjectTemplate* neither = templates.find("anim");
	neither->settings()["showInFirstPerson"] = "0";
	effects::EffectManager firstPerson(templates);
	CHECK(firstPerson.spawn("e_anim", {}, {}, effects::EffectManager::View::FirstPerson));
	CHECK(firstPerson.emitters() == 0);
	neither->settings()["showInZoomedMode"] = "1"; // showInZoomedMode alone: started
	effects::EffectManager zoomedOnly(templates);
	CHECK(zoomedOnly.spawn("e_anim", {}, {}, effects::EffectManager::View::FirstPerson));
	CHECK(zoomedOnly.emitters() == 1);
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

TEST(RenderScene_AnimatedUVFollowsThePartsTravel)
{
	// EU_TANK_tracks_left: v scrolls by the distance / 8, wrapped into [0, 0.285].
	render::AnimatedUVSettings tracks;
	tracks.translation = true;
	tracks.max[0] = 0;
	tracks.max[1] = 0.285f;
	tracks.size[0] = 0;
	tracks.size[1] = 8;
	tracks.translationIndex = 5;
	std::array<render::UVMatrix, render::kAnimatedUVMatrices> matrices {};
	render::AnimatedUVState state;
	const float z[3] = {0, 0, 1};
	float at[3] = {10, 0, 20};
	render::updateAnimatedUV(tracks, state, at, z, matrices); // the first update moves nothing
	CHECK(state.uv[1] == 0 && matrices[5].r2[1] == 0);
	at[2] = 19; // a metre backwards along z: +1 / 8
	render::updateAnimatedUV(tracks, state, at, z, matrices);
	CHECK(near(state.uv[1], 0.125f) && near(matrices[5].r2[1], 0.125f) && matrices[5].r2[0] == 0);
	at[2] = 17; // two more: 0.375 wraps to 0.09
	render::updateAnimatedUV(tracks, state, at, z, matrices);
	CHECK(near(state.uv[1], 0.375f - 0.285f));
	at[2] = 25; // forwards: -1, back into [0, 0.285]
	render::updateAnimatedUV(tracks, state, at, z, matrices);
	CHECK(state.uv[1] >= 0 && state.uv[1] <= 0.285f && near(state.uv[1], std::fmod(0.09f - 1.0f + 4 * 0.285f, 0.285f), 1e-3f));
	// Reverse subtracts; sideways travel does nothing.
	render::AnimatedUVSettings reverse = tracks;
	reverse.translationReverse = true;
	render::AnimatedUVState r;
	float p[3] = {0, 0, 0};
	render::updateAnimatedUV(reverse, r, p, z, matrices);
	p[2] = -0.8f;
	render::updateAnimatedUV(reverse, r, p, z, matrices);
	CHECK(near(r.uv[1], 0.285f - 0.1f));
	p[0] = 5;
	render::updateAnimatedUV(reverse, r, p, z, matrices);
	CHECK(near(r.uv[1], 0.185f));

	// EU_TANK_Rwheel_06's turn: angle = distance / radius 0.3, the matrix
	// rows (cos sx, -sin sy), (sin sx, cos sy), row 2 zero.
	render::AnimatedUVSettings wheel;
	wheel.rotation = true;
	wheel.radius = 0.3f;
	wheel.scale[0] = 0.5f;
	wheel.scale[1] = 1;
	wheel.rotationIndex = 3;
	render::AnimatedUVState w;
	float c[3] = {0, 0, 0};
	matrices[3].r2[0] = 7;
	render::updateAnimatedUV(wheel, w, c, z, matrices);
	c[2] = -0.3f; // one radian
	render::updateAnimatedUV(wheel, w, c, z, matrices);
	CHECK(near(w.angle, 1.0f, 1e-4f));
	CHECK(near(matrices[3].r0[0], std::cos(1.0f) * 0.5f) && near(matrices[3].r0[1], -std::sin(1.0f)));
	CHECK(near(matrices[3].r1[0], std::sin(1.0f) * 0.5f) && near(matrices[3].r1[1], std::cos(1.0f)));
	CHECK(matrices[3].r2[0] == 0 && matrices[3].r2[1] == 0);
	c[2] = 0.3f; // two radians back: wrapped into [0, 2 pi]
	render::updateAnimatedUV(wheel, w, c, z, matrices);
	CHECK(near(w.angle, 6.2831855f - 1.0f, 1e-4f));

	float v[2] = {0, 0};
	render::parseAnimatedUVVec2("0.2/0.285", v);
	CHECK(near(v[0], 0.2f) && near(v[1], 0.285f));
}

// RendDX9's decal clipping (0x10165f60 / 0x101655c0 / 0x10165020): a decal
// over a fold of the ground is cut to the faces it lies on and laid onto
// each; faces turned too far from the decal are left out by the clip
// angle; a decal off every face leaves nothing.
TEST(RenderScene_DecalClippedToTheSurface)
{
	using effects::Vec3;
	// Two faces meeting along x = 0: flat for x < 0, sloping up 45 degrees for x > 0.
	const std::vector<std::array<Vec3, 3>> fold = {
		{Vec3 {-4, 0, -4}, Vec3 {0, 0, -4}, Vec3 {-4, 0, 4}},
		{Vec3 {-4, 0, 4}, Vec3 {0, 0, -4}, Vec3 {0, 0, 4}},
		{Vec3 {0, 0, -4}, Vec3 {4, 4, -4}, Vec3 {0, 0, 4}},
		{Vec3 {0, 0, 4}, Vec3 {4, 4, -4}, Vec3 {4, 4, 4}},
	};
	const std::array<float, 4> frame {0, 0, 1, 1};
	const auto pieces = effects::clipDecal({0, 0, 0}, {0, 1, 0}, {1, 0, 0}, 1, frame, fold, 0, 0.01f);
	CHECK(!pieces.empty() && pieces.size() % 3 == 0 && pieces.size() <= 128);
	bool onSurface = true, inFrame = true, rises = false;
	for (const auto& p : pieces) {
		const float ground = p.position.x > 0 ? p.position.x : 0.0f;
		onSurface = onSurface && std::fabs(p.position.y - ground - 0.01f) < 1e-3f;
		inFrame = inFrame && p.u >= -1e-4f && p.u <= 1.0001f && p.v >= -1e-4f && p.v <= 1.0001f;
		rises = rises || p.position.y > 0.5f;
	}
	CHECK(onSurface && inFrame && rises);
	// decalGeomClipAngle 30: the 45-degree faces are left out.
	const auto flatOnly = effects::clipDecal({0, 0, 0}, {0, 1, 0}, {1, 0, 0}, 1, frame, fold, 30, 0.01f);
	bool flat = !flatOnly.empty();
	for (const auto& p : flatOnly) {
		flat = flat && p.position.x <= 1e-3f;
	}
	CHECK(flat);
	// Nowhere near the faces.
	CHECK(effects::clipDecal({50, 0, 50}, {0, 1, 0}, {1, 0, 0}, 1, frame, fold, 0, 0.01f).empty());
}

// The doubled ageing (0x10164f10): a decal set whose owner wasn't drawn the
// last frame ages twice as fast; world decals don't.
TEST(RenderScene_DecalSetsAgeTwiceUnseen)
{
	world::ObjectTemplateManager templates;
	templates.registerType(5, "Decal");
	world::ObjectTemplate* hole = templates.create("Decal", "hole");
	hole->settings()["decalTextureName"] = "hole";
	effects::EffectManager manager(templates);
	CHECK(manager.spawnDecal("hole", {0, 0, 0}, {0, 1, 0}));
	CHECK(manager.spawnDecal("hole", {11, 1, 0}, {1, 0, 0}, 42, {10, 0, 0}, {0, 0, 0}));
	CHECK(manager.spawnDecal("hole", {11, 1, 0}, {1, 0, 0}, 43, {10, 0, 0}, {0, 0, 0}));
	manager.decals()[0].ownerDrawn = false; // ignored for the world's set
	manager.decals()[2].ownerDrawn = false;
	manager.update(1);
	CHECK(std::fabs(manager.decals()[0].age - 1) < 1e-5f);
	CHECK(std::fabs(manager.decals()[1].age - 1) < 1e-5f);
	CHECK(std::fabs(manager.decals()[2].age - 2) < 1e-5f);
}

// rainManager.freeze (+0x65, 0x1011ab20 / 0x1011d250): the box stops
// following the camera.
TEST(RenderScene_RainFreezeHoldsTheBox)
{
	scene::RainSettings rain;
	rain.enabled = true;
	rain.particleCount = 200;
	rain.emitRadius = {10, 10, 10};
	rain.offset = {0, 0, 0};
	effects::Weather weather;
	weather.update(rain, {0, 0, 0}, 0);
	rain.freeze = true;
	weather.update(rain, {500, 0, 0}, 0.1f);
	bool held = !weather.drops().empty();
	for (const auto& drop : weather.drops()) {
		held = held && std::fabs(drop.position.x) <= 11.0f;
	}
	CHECK(held);
	rain.freeze = false;
	weather.update(rain, {500, 0, 0}, 0.1f);
	bool moved = !weather.drops().empty();
	for (const auto& drop : weather.drops()) {
		moved = moved && std::fabs(drop.position.x - 500) <= 11.0f;
	}
	CHECK(moved);
}

// The renderer's console switches (renderer.draw*, 0x1004c7a0..0x1004e0b0;
// drawFps 0x10053ae0; the road renderer's Renderer.road*; undergrowth.*).
TEST(RenderScene_RendererConsoleSwitches)
{
	Console console;
	scene::RendererSettings settings;
	scene::registerRendererConsoleMethods(console, settings);
	std::string out;
	CHECK(settings.drawTerrain && settings.drawSkyDome && settings.drawFps == 0);
	console.execute("renderer.drawTerrain 0", out);
	console.execute("renderer.drawFps 7", out);
	console.execute("Renderer.roadLodDistMod 2", out);
	console.execute("StaticMeshRenderer.enableRendering 0", out);
	console.execute("undergrowth.viewDistanceFade 3", out);
	console.execute("renderer.tweakNearPlane1p 0.1", out);
	CHECK(!settings.drawTerrain && settings.drawFps == 1 && settings.roadLodDistMod == 2);
	CHECK(!settings.drawStaticMeshes && settings.undergrowthViewDistanceFade == 1);
	CHECK(settings.tweakNearPlane1pSet && std::fabs(settings.tweakNearPlane1p - 0.1f) < 1e-6f);
	CHECK(!settings.tweakNearPlaneSet);
}

// The overgrowth's cells, cross-fade and lodTimer (OvergrowthLod.h,
// 0x1008da30 / 0x10120ef0 / 0x10121410) and its console rows.
TEST(RenderScene_OvergrowthCrossFadeAndCells)
{
	using render::OvergrowthBlend;
	using render::OvergrowthCells;
	// 500 / 4 = 125, fitted to a 1024 m terrain: 8 cells of 128.
	CHECK(near(OvergrowthCells::cellSize(500, 1024), 128));
	CHECK(near(OvergrowthCells::cellSize(140, 0), 35));
	// Kept up to 70% at 1, to 0 at the view distance, gone beyond.
	CHECK(near(OvergrowthCells::distanceFade(300, 500), 1));
	CHECK(near(OvergrowthCells::distanceFade(425, 500), 0.5f));
	CHECK(OvergrowthCells::distanceFade(500, 500) < 0);
	// The cell about a camera on the ground fades to 0.4 at its centre.
	CHECK(near(OvergrowthCells::nearFade(0, 0), 0.4f));
	CHECK(near(OvergrowthCells::nearFade(6, 0), 0.5f));
	CHECK(near(OvergrowthCells::nearFade(6, 10), 1));
	// The type factor: (radius / 5)^2 within 0.3..1.
	CHECK(near(OvergrowthBlend::typeFactor(1), 0.3f));
	CHECK(near(OvergrowthBlend::typeFactor(4), 0.64f));
	CHECK(near(OvergrowthBlend::typeFactor(10), 1));
	// w: 1 within 70% of R (factor 1, closeDistFade 0.3), 0 at R.
	CHECK(near(OvergrowthBlend::weight(50, 100, 1, 0.3f), 1));
	CHECK(near(OvergrowthBlend::weight(85, 100, 1, 0.3f), 0.5f));
	CHECK(near(OvergrowthBlend::weight(100, 100, 1, 0.3f), 0));
	// Both meshes at full alpha in the middle, one of them fading at the ends.
	CHECK(near(OvergrowthBlend::fullAlpha(0.5f), 1) && near(OvergrowthBlend::lodAlpha(0.5f), 1));
	CHECK(near(OvergrowthBlend::fullAlpha(0.25f), 0.5f) && near(OvergrowthBlend::lodAlpha(0.25f), 1));
	CHECK(near(OvergrowthBlend::lodAlpha(1), 0) && near(OvergrowthBlend::fullAlpha(0), 0));
	// lodTimer 0: w as it is; 2 s: eased at 0.5 a second.
	float t = 0;
	CHECK(near(OvergrowthBlend::eased(0.3f, t, 0.1f, 0), 0.3f));
	t = 0;
	CHECK(near(OvergrowthBlend::eased(0.4f, t, 0.1f, 2), 0.8f)); // at least 1 - 2w = 0.2
	CHECK(near(OvergrowthBlend::eased(0.4f, t, 0.2f, 2), 0.7f)); // 0.2 + 0.1
	CHECK(near(OvergrowthBlend::eased(0.9f, t, 0.2f, 2), 0.8f)); // at most 2 - 2w = 0.2
	CHECK(near(OvergrowthBlend::eased(1, t, 0.2f, 2), 1) && t == 0);

	Console console;
	std::string out;
	scene::OvergrowthSettings og;
	scene::registerOvergrowthConsoleMethods(console, og);
	console.execute("Overgrowth.viewDistance 350", out);
	console.execute("Overgrowth.closeDistPercentage 0.15", out);
	console.execute("Overgrowth.addMaterial wood 7", out);
	console.execute("Overgrowth.setActiveMaterial wood", out);
	console.execute("Overgrowth.addType spruce", out);
	console.execute("Overgrowth.setActiveType spruce", out);
	console.execute("OvergrowthType.geometry eu_spruce_b01", out);
	console.execute("OvergrowthType.density 50", out);
	console.execute("Overgrowth.preLoad", out);
	CHECK(og.configured && og.preLoaded && near(og.viewDistance, 350) && near(og.closeDistPercentage, 0.15f));
	CHECK(og.materials.size() == 1 && og.materials[0].index == 7 && og.activeMaterial == 0);
	CHECK(!og.addMaterial("other", 7)); // the index is taken (0x10092840)
	const scene::OvergrowthSettings::Type* spruce = og.typeForGeometry("EU_Spruce_b01");
	CHECK(spruce && near(spruce->density, 50) && near(spruce->randomScale[0], 1));
	console.execute("Overgrowth.getActiveMaterialName", out);
	CHECK(out == "wood");
}

// The antialiasing setting (VideoSettings.setAntialiasing, RendDX9
// 0x1000fb60's "<n>Samples" names) and the alpha-to-coverage path's alphas
// (0x1008da30, RaShaderLeaf with HASALPHA2MASK, 0x1010e270).
TEST(RenderScene_AntialiasingAndAlphaToCoverage)
{
	CHECK(scene::antialiasingSampleCount("Off") == 1);
	CHECK(scene::antialiasingSampleCount("2Samples") == 2);
	CHECK(scene::antialiasingSampleCount("4Samples") == 4);
	CHECK(scene::antialiasingSampleCount("8Samples") == 8);
	CHECK(scene::antialiasingSampleCount("1Samples") == 1);
	CHECK(scene::antialiasingSampleCount("Samples") == 1);
	CHECK(scene::antialiasingSampleCount("4Foo") == 1);
	CHECK(scene::antialiasingSampleCount("x4Samples") == 1);
	CHECK(scene::antialiasingSampleCount("") == 1);

	Console console;
	std::string out;
	scene::QualitySettings video;
	scene::registerQualityConsoleMethods(console, video);
	CHECK(video.antialiasingSamples == 1);
	console.execute("VideoSettings.setAntialiasing 4Samples", out);
	CHECK(video.antialiasingSamples == 4);
	console.execute("VideoSettings.setAntialiasing Off", out);
	CHECK(video.antialiasingSamples == 1);

	using render::AlphaToCoverage;
	// The near pass doubles both alphas only antialiased.
	CHECK(near(AlphaToCoverage::nearAlpha(0.5f, false), 0.5f));
	CHECK(near(AlphaToCoverage::nearAlpha(0.5f, true), 1));
	CHECK(near(AlphaToCoverage::nearAlpha(1, true), 2));
	// The leaves' coverage: 4 x a Transparency held to 1 (COLOR0).
	CHECK(near(AlphaToCoverage::leafAlphaScale(1), 4));
	CHECK(near(AlphaToCoverage::leafAlphaScale(2), 4));
	CHECK(near(AlphaToCoverage::leafAlphaScale(0.25f), 1));
	// The undergrowth's: over the cfg's AlphaRef, else as it is.
	CHECK(near(AlphaToCoverage::undergrowthAlphaScale(0.2f), 5));
	CHECK(near(AlphaToCoverage::undergrowthAlphaScale(0), 1));
}

// The meshes' point lights (MeshLights.h, 0x100a9f80 / 0x100a7f00) and
// the flicker (0x0064a920).
TEST(RenderScene_MeshLightsPickedPerMesh)
{
	using render::LightedMesh;
	render::LightLimits limits = render::LightLimits::forShadowQuality(2);
	CHECK(limits.staticMesh == 2 && near(limits.cullDistance, 100));
	CHECK(render::LightLimits::forShadowQuality(0).maxFor(LightedMesh::Bundled) == 0);
	render::ListedLight lights[4];
	const float at[4][3] = {{10, 0, 0}, {2, 0, 0}, {5, 0, 0}, {50, 0, 0}};
	for (int i = 0; i < 4; ++i) {
		std::copy(at[i], at[i] + 3, lights[i].position);
		lights[i].radius = 8;
	}
	lights[2].spot = true;
	lights[1].affectsLightmapped = false;
	const float origin[3] = {0, 0, 0};
	int picked[8];
	// Within light radius + mesh radius, nearest first, at most 2.
	int n = render::pickMeshLights(lights, 4, origin, 3, LightedMesh::Static, false, limits, picked, 8);
	CHECK(n == 2 && picked[0] == 1 && picked[1] == 2);
	// Bundled meshes take no spots; lightmapped statics only affectLightmappedObjects.
	n = render::pickMeshLights(lights, 4, origin, 3, LightedMesh::Bundled, false, limits, picked, 8);
	CHECK(n == 2 && picked[0] == 1 && picked[1] == 0);
	n = render::pickMeshLights(lights, 4, origin, 3, LightedMesh::Static, true, limits, picked, 8);
	CHECK(n == 2 && picked[0] == 2 && picked[1] == 0);
	// Bundled and skinned meshes only supportsEmitter lights (0x8b09's bit
	// 0x8000); static meshes any.
	lights[1].supportsEmitter = false;
	n = render::pickMeshLights(lights, 4, origin, 3, LightedMesh::Skinned, false, limits, picked, 8);
	CHECK(n == 1 && picked[0] == 0);
	n = render::pickMeshLights(lights, 4, origin, 3, LightedMesh::Static, false, limits, picked, 8);
	CHECK(n == 2 && picked[0] == 1 && picked[1] == 2);
	// An undergrowth patch (0x1010e270): Point lights only (0xb09), the
	// nearest 4 about its sphere, whatever the emitter flag.
	n = render::pickUndergrowthLights(lights, 4, origin, 3, picked);
	CHECK(n == 2 && picked[0] == 1 && picked[1] == 0);
	n = render::pickUndergrowthLights(lights, 4, origin, 50, picked);
	CHECK(n == 3 && picked[2] == 3);
	CHECK(render::terrainTakesLight(lights[0]) && !render::terrainTakesLight(lights[2]));
	// The flicker's factor lies in 1 - flicker .. 1; MSVC's rand from 1: 41.
	render::MsvcRand rand;
	CHECK(rand.next() == 41);
	CHECK(near(render::flickerFactor(0.5f, 32767), 1) && near(render::flickerFactor(0.5f, 0), 0.5f));
	CHECK(near(render::flickerFactor(0, 0), 1));
}

// The terrain's point-light passes (Hi_Terrain's PerPixelPointlight,
// 0x1014e080; LevelRenderer::applyTerrainLights): every listed Point light
// (filter 0x909, no spots), the shader's 8 nearest the camera by distance
// less radius, each adding colour x N.L x saturate(1 - d^2 / R^2) x 0.5.
TEST(RenderScene_TerrainPointLightPasses)
{
	render::ListedLight lights[11];
	for (int i = 0; i < 11; ++i) {
		lights[i].position[0] = 10.0f * static_cast<float>(i + 1); // 10 .. 110 m along x
		lights[i].radius = 5;
	}
	lights[0].spot = true;    // a spot: not the terrain's
	lights[10].radius = 200;  // far, but reaching over the camera: first
	const float camera[3] = {0, 0, 0};
	int picked[8];
	const int n = render::pickTerrainLights(lights, 11, camera, 8, picked);
	CHECK(n == 8);
	CHECK(picked[0] == 10 && picked[1] == 1 && picked[2] == 2 && picked[7] == 7);
	for (int i = 0; i < n; ++i) {
		CHECK(!lights[picked[i]].spot);
	}
	CHECK(render::pickTerrainLights(lights, 1, camera, 8, picked) == 0); // the spot alone
	CHECK(near(render::pointLightInverseRange2(10), 0.01f) && near(render::pointLightInverseRange2(0), 10000));
	// One light's term over a flat patch (normal up), R = 10 m.
	const float up[3] = {0, 1, 0};
	const float inv = render::pointLightInverseRange2(10);
	const float above[3] = {0, 5, 0};    // straight up, 5 of 10 m: 1 - 0.25
	CHECK(near(render::terrainPointLight(above, inv, up), 0.375f));
	const float aside[3] = {5, 5, 0};    // 45 degrees, d^2 50: 0.7071 x 0.5 x 0.5
	CHECK(near(render::terrainPointLight(aside, inv, up), 0.70710678f * 0.5f * 0.5f));
	const float below[3] = {0, -5, 0};   // behind the surface: none
	CHECK(render::terrainPointLight(below, inv, up) == 0);
	const float beyond[3] = {0, 11, 0};  // past R: none
	CHECK(render::terrainPointLight(beyond, inv, up) == 0);
}

// The object manager's distance fade (0x006ff110 / 0x006fb280 / 0x006fb320):
// at high geometry quality (8 / 10 / 80) an object of R = 1 is whole within
// 0.8 x 6400 m^2 of its edge, eases out to 6400 and is gone past 1.2 x 6400;
// a vehicle's area takes +0x144's 10.
TEST(RenderScene_ObjectDistanceFade)
{
	using namespace dice::hfe::render;
	const ObjectCullScales high = objectCullScales(3);
	CHECK(high.area == 8 && high.actorArea == 10 && high.minimum == 80);
	CHECK(objectCullScales(1).minimum == 40 && objectCullScales(2).actorArea == 8);
	CHECK(viewFarDistance(330.4f, 1) == 330 && viewFarDistance(5000, 0.5f) == 1500 && viewFarDistance(5, 1) == 20);
	// The overhead view draws with the scale at 1 (0x00779680).
	CHECK(frameViewDistanceScale(0.5f, false) == 0.5f && frameViewDistanceScale(0.5f, true) == 1.0f);
	const CullRecord prop = cullRecord(high, 1, 1000, false, 0.1f);
	CHECK(prop.minArea == 6400 && prop.areaScale == 640);
	// R = 1: pi x 640 < 6400, so A = 6400.
	CHECK(stepDistanceFade(0, 1, 1 + 5000, prop) == 1);                  // d^2 5000 <= 5120
	CHECK(stepDistanceFade(1, 1, 1 + 6000, prop) == 1);                  // whole stays whole in the near band
	const float in = stepDistanceFade(0, 1, 1 + 6000, prop);             // 1 - 880 / 1200 = 0.2667 vs 0.4
	CHECK(std::fabs(in - 0.4f) < 1e-5f);
	const float out = stepDistanceFade(1, 1, 1 + 7000, prop);            // 1 - 600 / 1400 = 0.571 vs 0.6
	CHECK(std::fabs(out - (1 - 600.0f / 1400.0f)) < 1e-5f);
	CHECK(stepDistanceFade(0, 1, 1 + 7000, prop) == 0);                  // gone stays gone in the far band
	CHECK(stepDistanceFade(1, 1, 1 + 7700, prop) == 0);                  // past 1.2 A
	// The view distance cuts first: (R + p1)^2.
	const CullRecord shortView = cullRecord(high, 1, 50, false, 0.1f);
	CHECK(stepDistanceFade(1, 1, 1 + 2700, shortView) == 0);
	// A big object (R = 10): pi x 100 x 640 = 201062 m^2, whole to ~401 m.
	CHECK(stepDistanceFade(0, 10, 100 + 400 * 400, prop) == 1);
	// A vehicle of R = 5 at 260 m: pi x 25 x 1000 = 78540; 0.8 of it is 62832.
	const CullRecord vehicle = cullRecord(high, 1, 1000, true, 0.1f);
	CHECK(stepDistanceFade(0, 5, 25 + 240 * 240, vehicle) == 1);
	CHECK(stepDistanceFade(0, 5, 25 + 240 * 240, prop) < 1); // as a prop: 50265, past 0.8 A
	// Zoomed 4x: p0 and p2 grow with it.
	const CullRecord zoomed = cullRecord(high, 4, 1000, false, 0.1f);
	CHECK(stepDistanceFade(0, 1, 1 + 15000, zoomed) == 1);
}

// The light manager's SimpleShadowmap (RendDX9 0x100aa290, 0x100a8cb0 /
// 0x100a9160) and the meshes' sun factor (0x100c3880 / 0x10162070).
TEST(RenderScene_SimpleShadowmap)
{
	using namespace dice::hfe::render;
	// 0x100a8f40: 2-bit steps of the byte.
	CHECK(SimpleShadowmap::quantize(0) == 0 && SimpleShadowmap::quantize(42) == 0 && SimpleShadowmap::quantize(43) == 1);
	CHECK(SimpleShadowmap::quantize(128) == 2 && SimpleShadowmap::quantize(212) == 2 && SimpleShadowmap::quantize(213) == 3);
	CHECK(SimpleShadowmap::quantize(255) == 3);
	// An 8 x 8 map (16 m across, 2 m a texel, about the origin): all lit
	// but file column 5, row 1 (x 2..4, z 4..6: the first row is +z), and
	// that texel indoors.
	const int n = 8;
	std::vector<uint8_t> occlusion(n * n, 255), io(n * n, 255);
	occlusion[1 * n + 5] = 0;
	io[1 * n + 5] = 0;
	SimpleShadowmap map;
	CHECK(map.occlusionAt(0, 0) == 1); // nothing loaded
	map.load(n, occlusion, io);
	CHECK(map.loaded() && map.size() == n);
	CHECK(std::fabs(map.occlusionAt(3, 5)) < 1e-6f);   // the texel's centre
	CHECK(std::fabs(map.outdoorsAt(3, 5)) < 1e-6f);
	CHECK(std::fabs(map.occlusionAt(4, 5) - 0.5f) < 1e-5f); // half way to the next texel
	CHECK(std::fabs(map.occlusionAt(3, 3) - 1) < 1e-6f);    // the row below it (z 2..4)
	CHECK(std::fabs(map.occlusionAt(-3, 5) - 1) < 1e-6f);
	// Past the map: clamped to +-N, the border is lit and outdoors.
	CHECK(map.occlusionAt(100, -100) == 1 && map.outdoorsAt(-100, 100) == 1);
	// 1/3 steps.
	occlusion[1 * n + 5] = 90; // q 1
	map.load(n, occlusion, {});
	CHECK(std::fabs(map.occlusionAt(3, 5) - 1.0f / 3.0f) < 1e-5f);
	CHECK(map.outdoorsAt(3, 5) == 1); // no IO file: outdoors
	// A short file loads nothing.
	map.load(n, std::vector<uint8_t>(10, 0), {});
	CHECK(!map.loaded() && map.occlusionAt(3, 5) == 1);

	// The factor: outdoors with nothing above, the map faded out by the
	// height over the ground across 3..6 m; indoors with a roof, the map.
	CHECK(std::fabs(simpleShadowSunScale(0.25f, 1, false, 1) - 0.25f) < 1e-6f);
	CHECK(std::fabs(simpleShadowSunScale(0.25f, 1, false, 4.5f) - 0.625f) < 1e-6f);
	CHECK(simpleShadowSunScale(0.25f, 1, false, 6) == 1);
	CHECK(simpleShadowSunScale(0.25f, 0, false, 1) == 1); // indoors, open above
	CHECK(simpleShadowSunScale(0.25f, 0, true, 1) == 0.25f);
	CHECK(simpleShadowSunScale(0.25f, 1, true, 1) == 1); // outdoors under something
	// renderer.useSunScaling (skinned meshes): taken as indoors.
	CHECK(simpleShadowSunScale(0.25f, 1, true, 1, true) == 0.25f);
	CHECK(std::fabs(simpleShadowSunScale(0.25f, 0, false, 1, true) - 0.25f) < 1e-6f);
	// The downward ray: 6 m when it meets nothing.
	CHECK(simpleShadowHeight(2) == 2 && simpleShadowHeight(7) == 6 && simpleShadowHeight(-1) == 6);
}


TEST(RenderScene_StaticLightmapsByLightingQuality)
{
	using namespace dice::hfe::render;
	// 0x100df010: low lighting (1) drops the statics' lightmaps but Titan
	// parts' and objects/water/ meshes'; medium (2) and high (3) keep all.
	CHECK(!staticKeepsLightmap(false, false, 1));
	CHECK(staticKeepsLightmap(true, false, 1));
	CHECK(staticKeepsLightmap(false, true, 1));
	CHECK(staticKeepsLightmap(false, false, 2));
	CHECK(staticKeepsLightmap(false, false, 3));
}

TEST(RenderScene_CustomTextureSuffix)
{
	using namespace dice::hfe::render;
	// 0x10018ca0: "<name up to its last '.'>_<suffix>.dds" (0x1019f4c0).
	CHECK(customSuffixedTexturePath("objects/vehicles/eu_fav/textures/eu_fav_c.dds", "Snow") ==
		"objects/vehicles/eu_fav/textures/eu_fav_c_Snow.dds");
	CHECK(customSuffixedTexturePath("textures/billboard_color01.tga", "wake") == "textures/billboard_color01_wake.dds");
	CHECK(customSuffixedTexturePath("noextension", "HEAT") == "noextension_HEAT.dds");
	// None without a suffix, nor for IGA's "rad_" textures.
	CHECK(customSuffixedTexturePath("objects/x.dds", "").empty());
	CHECK(customSuffixedTexturePath("rad_2041100.dds", "HEAT").empty());
}

// The texture load's name (0x10017f20 -> 0x10017340): as given, else
// with ".dds", else bare. The map node's "mini_Medic.tga" ships only as
// mini_Medic.dds.
TEST(RenderScene_TextureFallbackNames)
{
	using namespace dice::hfe::render;
	const auto medic = textureFallbackPaths("Menu/HUD/Texture/Ingame/Player/Icons/Minimap/mini_Medic.tga");
	CHECK(medic.size() == 2);
	if (medic.size() == 2) {
		CHECK(medic[0] == "Menu/HUD/Texture/Ingame/Player/Icons/Minimap/mini_Medic.dds");
		CHECK(medic[1] == "Menu/HUD/Texture/Ingame/Player/Icons/Minimap/mini_Medic");
	}
	const auto dds = textureFallbackPaths("textures/x.dds");
	CHECK(dds.size() == 1 && dds[0] == "textures/x");
	CHECK(textureFallbackPaths("levels/a.b/noextension").empty());
}

TEST(RenderScene_RoadStaticMeshTechniques)
{
	using namespace dice::hfe::render;
	// 0x100de6d0's flags (a case-blind find) and 0x100bdb00's keys.
	CHECK(roadStaticPath("Objects/roads/statics/t_intersection/Meshes/t_intersection.staticMesh"));
	CHECK(!roadStaticNoBlend("Objects/roads/statics/t_intersection/Meshes/t_intersection.staticMesh"));
	CHECK(roadStaticNoBlend("Objects/Roads/staticsnoblend/concreteblock_helipad/Meshes/concreteblock_helipad.staticMesh"));
	CHECK(!roadStaticPath("Objects/StaticObjects/roads_sign.staticMesh"));
	CHECK(!roadStaticNoBlend("Objects/StaticObjects/noBlend/x.staticMesh"));
	CHECK(roadStaticTechnique(false, false) == RoadTechnique::Road);
	CHECK(roadStaticTechnique(false, true) == RoadTechnique::Road); // no detail map: Road even under noBlend
	CHECK(roadStaticTechnique(true, false) == RoadTechnique::RoadDetail);
	CHECK(roadStaticTechnique(true, true) == RoadTechnique::RoadDetailNoBlend);
}
