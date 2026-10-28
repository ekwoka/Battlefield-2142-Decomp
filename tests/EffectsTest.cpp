// Particle systems: graphs, emission, life.
#include "Effects/Effects.h"
#include "TestMain.h"
#include "World/ObjectTemplateManager.h"

#include <cmath>

using namespace dice::hfe;

TEST(Effects_GraphsAreCubics)
{
	// sizeGraph 0/1/-2/1 = (1 - t)^2.
	const auto g = effects::Graph::parse("0/1/-2/1");
	CHECK(g.at(0) == 1 && std::fabs(g.at(0.5f) - 0.25f) < 1e-6f && std::fabs(g.at(1)) < 1e-6f);
}

TEST(Effects_EmitAndExpire)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "EffectBundle");
	templates.registerType(2, "SpriteParticleSystem");
	world::ObjectTemplate* sparks = templates.create("SpriteParticleSystem", "sparks");
	sparks->settings()["emitFrequency"] = "100";
	sparks->settings()["emitTime"] = "0.1";
	sparks->settings()["emitSpeed"] = "5";
	sparks->settings()["timeToLive"] = "0.5";
	sparks->settings()["gravity"] = "-10";
	sparks->settings()["textureName"] = "objects\\effects\\textures\\spark";
	sparks->settings()["particleType"] = "Additive";
	world::ObjectTemplate* bundle = templates.create("EffectBundle", "e_test");
	bundle->children().push_back({"sparks", {0, 1, 0}, {}});

	effects::EffectManager manager(templates);
	CHECK(manager.spawn("e_test", {10, 0, 0}));
	CHECK(!manager.spawn("missing", {}));
	CHECK(manager.emitters() == 1);
	for (int i = 0; i < 6; ++i) { // 0.2 s
		manager.update(1 / 30.0f);
	}
	size_t sprites = 0;
	std::string texture;
	bool additive = false;
	float highest = 0;
	manager.forEachGroup([&](const std::string& path, bool add, const std::vector<effects::Sprite>& list) {
		texture = path;
		additive = add;
		sprites += list.size();
		for (const auto& s : list) {
			highest = std::fmax(highest, s.position.y);
		}
	});
	// About 10 particles (100 a second for 0.1 s), from 1 m up, flying up.
	CHECK(sprites >= 9 && sprites <= 11);
	CHECK(texture == "objects/effects/textures/spark" && additive);
	CHECK(highest > 1.2f);
	for (int i = 0; i < 30; ++i) {
		manager.update(1 / 30.0f);
	}
	CHECK(manager.emitters() == 0);
}

namespace {

// A bundle "e_flame" with one looping system, local or not, shown in the
// views given.
void addFlame(world::ObjectTemplateManager& templates, bool local, const char* firstPerson = "1",
	const char* thirdPerson = "1")
{
	templates.registerType(1, "EffectBundle");
	templates.registerType(2, "SpriteParticleSystem");
	templates.registerType(3, "Sound");
	templates.registerType(4, "PlayerControlObject");
	world::ObjectTemplate* flame = templates.create("SpriteParticleSystem", "flame");
	flame->settings()["emitFrequency"] = "30";
	flame->settings()["emitTime"] = "0.1";
	flame->settings()["isLooping"] = "1";
	flame->settings()["timeToLive"] = "1";
	flame->settings()["textureName"] = "flame";
	flame->settings()["isLocalSystem"] = local ? "1" : "0";
	flame->settings()["showInFirstPerson"] = firstPerson;
	flame->settings()["showInThirdPerson"] = thirdPerson;
	templates.create("Sound", "S_e_flame_Start");
	world::ObjectTemplate* bundle = templates.create("EffectBundle", "e_flame");
	bundle->children().push_back({"S_e_flame_Start", {}, {}});
	bundle->children().push_back({"flame", {0, 0, 1}, {}});
}

std::vector<effects::Sprite> sprites(const effects::EffectManager& manager)
{
	std::vector<effects::Sprite> all;
	manager.forEachGroup([&](const std::string&, bool, const std::vector<effects::Sprite>& list) {
		all.insert(all.end(), list.begin(), list.end());
	});
	return all;
}

} // namespace

TEST(Effects_AttachedBundlesFollowTheirAnchor)
{
	world::ObjectTemplateManager templates;
	addFlame(templates, true);
	effects::EffectManager manager(templates);
	manager.setAnchor(7, {10, 0, 0}, {});
	// Mounted 2 m up the anchor; the system sits 1 m along the bundle's z.
	CHECK(manager.spawnAttached("e_flame", 7, {0, 2, 0}));
	const auto sounds = manager.takeSounds();
	CHECK(sounds.size() == 1 && sounds[0].name == "S_e_flame_Start" && sounds[0].position.x == 10 &&
		sounds[0].position.y == 2);
	CHECK(manager.takeSounds().empty());
	for (int i = 0; i < 3; ++i) {
		manager.update(1 / 30.0f);
	}
	auto list = sprites(manager);
	CHECK(!list.empty());
	CHECK(std::fabs(list[0].position.x - 10) < 1e-4f && std::fabs(list[0].position.y - 2) < 1e-4f
		&& std::fabs(list[0].position.z - 1) < 1e-4f);
	// The anchor moves and turns a quarter round (yaw 90: +z goes to +x): a
	// local system's particles go with it.
	manager.setAnchor(7, {0, 0, 5}, physics::Mat3::fromYawPitchRoll({90, 0, 0}));
	list = sprites(manager);
	CHECK(std::fabs(list[0].position.x - 1) < 1e-4f && std::fabs(list[0].position.y - 2) < 1e-4f
		&& std::fabs(list[0].position.z - 5) < 1e-4f);
	// A looping attached system keeps emitting until it's stopped.
	for (int i = 0; i < 60; ++i) {
		manager.update(1 / 30.0f);
	}
	CHECK(manager.emitting(7) && manager.emitters() == 1);
	manager.stopAttached(7);
	CHECK(!manager.emitting(7));
	for (int i = 0; i < 40; ++i) {
		manager.update(1 / 30.0f);
	}
	CHECK(manager.emitters() == 0);
}

TEST(Effects_WorldSystemsLeaveTheirParticles)
{
	world::ObjectTemplateManager templates;
	addFlame(templates, false);
	effects::EffectManager manager(templates);
	manager.setAnchor(1, {0, 0, 0}, {});
	manager.spawnAttached("e_flame", 1, {});
	manager.update(1 / 30.0f);
	const auto before = sprites(manager);
	CHECK(!before.empty());
	// The emitter moves on; what it emitted stays where it was (a trail).
	manager.setAnchor(1, {50, 0, 0}, {});
	const auto after = sprites(manager);
	CHECK(after.size() == before.size() && std::fabs(after[0].position.x - before[0].position.x) < 1e-4f);
	manager.update(1 / 30.0f);
	const auto trail = sprites(manager);
	CHECK(trail.back().position.x > 40);
	// Releasing the anchor stops the emission.
	manager.releaseAnchor(1);
	CHECK(!manager.emitting(1));
}

TEST(Effects_ViewsPickTheirSystems)
{
	world::ObjectTemplateManager templates;
	addFlame(templates, false, "0", "1"); // a third-person-only system (like glow3p)
	effects::EffectManager manager(templates);
	CHECK(manager.spawn("e_flame", {}, {}, effects::EffectManager::View::FirstPerson));
	CHECK(manager.emitters() == 0);
	manager.spawn("e_flame", {}, {}, effects::EffectManager::View::ThirdPerson);
	CHECK(manager.emitters() == 1);
	manager.spawn("e_flame", {});
	CHECK(manager.emitters() == 2);
}

TEST(Effects_ObjectBundlesStartAndStopAsTheOriginal)
{
	world::ObjectTemplateManager templates;
	addFlame(templates, true);
	// An engine flame that runs from the start, an exhaust that starts when
	// a player enters (startOnEffects 0x20) and stops when they leave (0x40).
	world::ObjectTemplate* engine = templates.create("EffectBundle", "e_engine");
	engine->settings()["startAtCreation"] = "1";
	engine->children().push_back({"flame", {}, {}});
	world::ObjectTemplate* exhaust = templates.create("EffectBundle", "e_exhaust");
	exhaust->settings()["setStartOnEffects"] = "32";
	exhaust->settings()["setStopOnEffects"] = "64";
	exhaust->children().push_back({"flame", {}, {}});
	world::ObjectTemplate* car = templates.create("PlayerControlObject", "car");
	car->children().push_back({"e_engine", {0, 1, -2}, {}});
	car->children().push_back({"e_exhaust", {0, 0.5f, -2.5f}, {}});
	const auto placed = effects::ObjectEffects::bundlesOf(templates, *car);
	CHECK(placed.size() == 2 && placed[0].startAtCreation && placed[1].startOnEffects == 32 &&
		placed[1].stopOnEffects == 64 && placed[1].position.z == -2.5f);

	effects::EffectManager manager(templates);
	effects::ObjectEffects objects(templates);
	effects::ObjectEffects::ObjectState state;
	state.id = 9;
	state.objectTemplate = car;
	state.position = {100, 0, 0};
	objects.update({state}, manager);
	CHECK(manager.emitting(9, 1) && !manager.emitting(9, 2));
	manager.update(1 / 30.0f);
	auto list = sprites(manager);
	CHECK(!list.empty() && std::fabs(list[0].position.x - 100) < 1e-4f && std::fabs(list[0].position.z + 2) < 1e-4f);
	// Entered: the exhaust starts; left: it stops, the engine keeps going.
	state.occupied = true;
	objects.update({state}, manager);
	CHECK(manager.emitting(9, 2));
	state.occupied = false;
	objects.update({state}, manager);
	CHECK(!manager.emitting(9, 2) && manager.emitting(9, 1));
	// The object goes: so does the emission.
	objects.update({}, manager);
	CHECK(!manager.emitting(9) && objects.tracked() == 0);
}

TEST(Effects_DecalsLightsAndMeshParticles)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "EffectBundle");
	templates.registerType(2, "SpriteParticleSystem");
	templates.registerType(3, "MeshParticleSystem");
	templates.registerType(4, "LightSource");
	templates.registerType(5, "Decal");
	// A decal template: a 2 x 2 atlas, one of its four frames at random.
	world::ObjectTemplate* hole = templates.create("Decal", "S_Decal_Concreate");
	hole->settings()["size"] = "0.175";
	hole->settings()["randomSize"] = "0";
	hole->settings()["decalTextureName"] = "objects\\effects\\decals\\textures\\Decal_concreate01_c";
	hole->settings()["randomRotation"] = "360";
	hole->settings()["animationEnable"] = "1";
	hole->settings()["animationFrameCount"] = "4";
	hole->settings()["animationFrameCountX"] = "2";
	hole->settings()["setAnimationFrameWidthRelative"] = "0.5";
	hole->settings()["setAnimationFrameHeightRelative"] = "0.5";
	hole->settings()["animationRandomizedStartFrame"] = "1";
	effects::EffectManager manager(templates);
	CHECK(manager.spawnDecal("S_Decal_Concreate", {1, 2, 3}, {0, 0, -2}));
	CHECK(!manager.spawnDecal("missing", {}, {0, 1, 0}));
	CHECK(manager.decals().size() == 1);
	const effects::Decal& decal = manager.decals()[0];
	CHECK(decal.texture == "objects/effects/decals/textures/Decal_concreate01_c" && decal.size == 0.175f);
	CHECK(decal.normal.z == -1 && std::fabs(physics::dot(decal.tangent, decal.normal)) < 1e-5f);
	CHECK(decal.frame[2] - decal.frame[0] == 0.5f && decal.frame[3] - decal.frame[1] == 0.5f);
	for (size_t i = 0; i < effects::EffectManager::kMaxDecals + 10; ++i) {
		manager.spawnDecal("S_Decal_Concreate", {static_cast<float>(i), 0, 0}, {0, 1, 0});
	}
	CHECK(manager.decals().size() == effects::EffectManager::kMaxDecals);
	CHECK(manager.decals().back().position.x == effects::EffectManager::kMaxDecals + 9); // the oldest went

	// A bundle with a mesh particle system and a light: the light lives as
	// long as the bundle's systems, fading.
	world::ObjectTemplate* spike = templates.create("MeshParticleSystem", "mp_spike");
	spike->settings()["emitFrequency"] = "100";
	spike->settings()["emitTime"] = "0.1";
	spike->settings()["timeToLive"] = "0.5";
	spike->settings()["randomRotation"] = "0/0/90";
	spike->setGeometry("mp_smokespike");
	world::ObjectTemplate* light = templates.create("LightSource", "flash");
	light->settings()["color"] = "2/1/0.5";
	light->settings()["intensity"] = "2";
	light->settings()["attenuationRange1"] = "2";
	light->settings()["attenuationRange2"] = "5";
	world::ObjectTemplate* off = templates.create("LightSource", "off");
	off->settings()["enabled"] = "0";
	world::ObjectTemplate* bundle = templates.create("EffectBundle", "e_boom");
	bundle->children().push_back({"mp_spike", {}, {}});
	bundle->children().push_back({"flash", {0, 1, 0}, {}});
	bundle->children().push_back({"off", {}, {}});
	CHECK(manager.spawn("e_boom", {10, 0, 0}));
	CHECK(manager.lights().size() == 1);
	const effects::PointLight lit = manager.lights()[0];
	CHECK(lit.position.x == 10 && lit.position.y == 1 && lit.color.x == 4 && lit.range1 == 2 && lit.range2 == 5);
	for (int i = 0; i < 6; ++i) {
		manager.update(1 / 30.0f);
	}
	size_t meshes = 0, sprites = 0;
	std::string geometry;
	manager.forEachMeshGroup([&](const std::string& name, bool, const std::vector<effects::MeshSprite>& list) {
		geometry = name;
		meshes += list.size();
	});
	manager.forEachGroup([&](const std::string&, bool, const std::vector<effects::Sprite>& list) { sprites += list.size(); });
	CHECK(geometry == "mp_smokespike" && meshes >= 9 && meshes <= 11 && sprites == 0);
	CHECK(manager.lights().size() == 1 && manager.lights()[0].color.x < 4 && manager.lights()[0].color.x > 1);
	for (int i = 0; i < 30; ++i) {
		manager.update(1 / 30.0f);
	}
	CHECK(manager.lights().empty() && manager.emitters() == 0);
}

// A TrailSystem carried by its anchor leaves points while it moves faster
// than minTrailVelocity and draws them as a ribbon in numPlanes planes
// (Trail.fx); a NonScreenAlignedParticleSystem's quads lie across the
// system's y axis (NonScreenAlignedParticles.fx).
TEST(Effects_TrailsAndNonScreenAlignedQuads)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "EffectBundle");
	templates.registerType(2, "TrailSystem");
	templates.registerType(3, "NonScreenAlignedParticleSystem");
	world::ObjectTemplate* trail = templates.create("TrailSystem", "smoke_trail");
	trail->settings()["emitFrequency"] = "20";
	trail->settings()["timeToLive"] = "2";
	trail->settings()["particleMaxSize"] = "1";
	trail->settings()["sizeGraph"] = "0/0/0/1";
	trail->settings()["transparencyGraph"] = "0/0/0/1";
	trail->settings()["numPlanes"] = "3";
	trail->settings()["minTrailVelocity"] = "2";
	trail->settings()["fadeInFactor"] = "0.05";
	trail->settings()["fadeOutFactor"] = "0.05";
	trail->settings()["textureName"] = "objects\\effects\\textures\\trail";
	world::ObjectTemplate* bundle = templates.create("EffectBundle", "e_trail");
	bundle->children().push_back({"smoke_trail", {}, {}});
	effects::EffectManager manager(templates);
	manager.setAnchor(3, {0, 10, 0}, {});
	CHECK(manager.spawnAttached("e_trail", 3, {}));
	const auto triangles = [&manager] {
		size_t count = 0;
		manager.forEachWorldGroup({50, 10, 5}, [&](const std::string&, bool, const std::vector<effects::WorldVertex>& list) {
			count += list.size();
		});
		return count;
	};
	// Still: no points.
	for (int i = 0; i < 15; ++i) {
		manager.update(1 / 30.0f);
	}
	CHECK(manager.emitters() == 1 && triangles() == 0);
	// Flying along x at 30 m/s for half a second: about ten points, nine
	// segments of three planes of two triangles.
	for (int i = 0; i < 15; ++i) {
		manager.setAnchor(3, {static_cast<float>(i + 1), 10, 0}, {});
		manager.update(1 / 30.0f);
	}
	const size_t drawn = triangles();
	CHECK(drawn >= 8 * 3 * 6 && drawn <= 10 * 3 * 6 && drawn % 18 == 0);
	// Sprites leave it out.
	size_t sprites = 0;
	manager.forEachGroup([&](const std::string&, bool, const std::vector<effects::Sprite>& list) { sprites += list.size(); });
	CHECK(sprites == 0);
	// Stopped, its points fade and it goes.
	manager.stopAttached(3);
	for (int i = 0; i < 75; ++i) {
		manager.update(1 / 30.0f);
	}
	CHECK(manager.emitters() == 0);

	world::ObjectTemplate* ripples = templates.create("NonScreenAlignedParticleSystem", "ripples");
	ripples->settings()["emitFrequency"] = "30";
	ripples->settings()["emitTime"] = "0.1";
	ripples->settings()["timeToLive"] = "1";
	ripples->settings()["particleMaxSize"] = "2";
	ripples->settings()["sizeGraph"] = "0/0/0/1";
	ripples->settings()["transparencyGraph"] = "0/0/0/1";
	ripples->settings()["textureName"] = "objects\\effects\\textures\\ring";
	CHECK(manager.spawn("ripples", {0, 1, 0}));
	for (int i = 0; i < 3; ++i) {
		manager.update(1 / 30.0f);
	}
	bool flat = true;
	size_t corners = 0;
	manager.forEachWorldGroup({0, 20, 0}, [&](const std::string&, bool, const std::vector<effects::WorldVertex>& list) {
		for (const auto& v : list) {
			flat = flat && std::fabs(v.position.y - 1) < 1e-4f;
			++corners;
		}
	});
	CHECK(corners > 0 && corners % 6 == 0 && flat);
}

// Rain and snow (rainManager.*, Rain.fx): the particles stay in the box
// about the camera as they fall, fade towards its edge and shrink with
// distance.
TEST(Effects_WeatherBoxFollowsTheCamera)
{
	scene::RainSettings rain;
	rain.enabled = true;
	rain.particleCount = 500;
	rain.offset = {0, 10, 0};
	rain.emitRadius = {20, 10, 20};
	rain.particleVelocity = {0, -1, 0};
	rain.particleSpeed = 5;
	rain.particleSize = 8;
	rain.particleMaxSize = 12;
	rain.particleScale = {0, 0.1f, 0};
	rain.fadeOutRange = 0.6f;
	effects::Weather weather;
	weather.update(rain, {100, 0, 100}, 0);
	CHECK(!weather.drops().empty() && weather.drops().size() <= 500);
	for (int i = 0; i < 60; ++i) {
		weather.update(rain, {100.0f + static_cast<float>(i), 0, 100}, 1 / 30.0f);
	}
	bool inside = true, sized = true;
	for (const auto& drop : weather.drops()) {
		inside = inside && std::fabs(drop.position.x - 159) <= 20.01f && std::fabs(drop.position.y - 10) <= 10.01f
			&& drop.alpha > 0 && drop.alpha <= 1;
		sized = sized && drop.pixels > 0 && drop.pixels <= 12;
	}
	CHECK(inside && sized);
	rain.enabled = false;
	weather.update(rain, {}, 1 / 30.0f);
	CHECK(weather.drops().empty());
}

// A decal on a moving object is kept in its space (the object's own
// decal set, BF2142.exe 0x0063c000 / RendDX9 0x100e6b70).
TEST(Effects_DecalsOnMovingObjects)
{
	world::ObjectTemplateManager templates;
	templates.registerType(5, "Decal");
	world::ObjectTemplate* hole = templates.create("Decal", "hole");
	hole->settings()["decalTextureName"] = "objects\\effects\\textures\\hole";
	effects::EffectManager manager(templates);
	// The object at (10, 0, 0) turned a quarter round (yaw 90: its +z is
	// the world's +x); hit at (11, 1, 0) on a face looking along +x.
	CHECK(manager.spawnDecal("hole", {11, 1, 0}, {1, 0, 0}, 42, {10, 0, 0}, {90, 0, 0}));
	CHECK(manager.decals().size() == 1);
	const effects::Decal& d = manager.decals()[0];
	CHECK(d.object == 42);
	CHECK(std::fabs(d.position.x) < 1e-4f && std::fabs(d.position.y - 1) < 1e-4f && std::fabs(d.position.z - 1) < 1e-4f);
	CHECK(std::fabs(d.normal.z - 1) < 1e-4f);
	manager.dropDecals(42);
	CHECK(manager.decals().empty());
}
