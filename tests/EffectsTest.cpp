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
	flame->settings()["gravity"] = "0"; // the sprite template's default is -9.82
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
	// The first particle comes one interval (1/30 s) in, in 0.01 s steps.
	manager.update(1 / 30.0f);
	manager.update(1 / 30.0f);
	const auto before = sprites(manager);
	CHECK(!before.empty());
	// The emitter moves on; what it emitted stays where it was (a trail).
	manager.setAnchor(1, {50, 0, 0}, {});
	const auto after = sprites(manager);
	CHECK(after.size() == before.size() && std::fabs(after[0].position.x - before[0].position.x) < 1e-4f);
	manager.update(1 / 30.0f);
	manager.update(1 / 30.0f);
	const auto trail = sprites(manager);
	// (The sprites come back to front from the camera, 0x1012f800: the
	// newest, at the emitter's new place, is the farthest from the origin.)
	CHECK(!trail.empty() && trail.front().position.x > 40);
	// Releasing the anchor stops the emission.
	manager.releaseAnchor(1);
	CHECK(!manager.emitting(1));
}

TEST(Effects_ViewsPickTheirSystems)
{
	world::ObjectTemplateManager templates;
	addFlame(templates, false, "0", "1"); // a third-person-only system (like glow3p)
	// showInZoomedMode 0 too, as the one shipped template with
	// showInFirstPerson 0 has: in first person either flag shows a system
	// (0x0061a1c0's flags & 5).
	templates.find("flame")->settings()["showInZoomedMode"] = "0";
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

// A bundle under a bone part (boneIndex, 0x0063d470: the Talon's engine
// pods) sits where the bone puts the part, its template place replaced, as
// the renderer draws the part.
TEST(Effects_BundlesFollowBoneParts)
{
	world::ObjectTemplateManager templates;
	addFlame(templates, true);
	world::ObjectTemplate* jet = templates.create("EffectBundle", "e_jet");
	jet->children().push_back({"flame", {}, {}});
	world::ObjectTemplate* pod = templates.create("Bundle", "pod");
	pod->children().push_back({"e_jet", {0, 0, -1}, {}});
	world::ObjectTemplate* ship = templates.create("PlayerControlObject", "ship");
	ship->children().push_back({"pod", {5, 0, 0}, {}});
	// The bone: pitched 90 degrees (z down to -y) at (1, 2, 3).
	const effects::ObjectEffects::PartBone bone = [](uint32_t object, const std::string& part,
													  std::array<float, 16>& m) {
		if (object != 7 || part != "pod") {
			return false;
		}
		m = {1, 0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 1, 2, 3, 1};
		return true;
	};
	const auto unboned = effects::ObjectEffects::bundlesOf(templates, *ship, 7);
	CHECK(unboned.size() == 1 && std::fabs(unboned[0].position.x - 5) < 1e-5f
		&& std::fabs(unboned[0].position.z + 1) < 1e-5f);
	const auto boned = effects::ObjectEffects::bundlesOf(templates, *ship, 7, {}, nullptr, bone);
	CHECK(boned.size() == 1);
	const physics::Vec3 at = boned[0].position, z = boned[0].rotation.transform({0, 0, 1});
	CHECK(std::fabs(at.x - 1) < 1e-5f && std::fabs(at.y - 3) < 1e-5f && std::fabs(at.z - 3) < 1e-5f);
	CHECK(std::fabs(z.y + 1) < 1e-5f); // its z turned down with the pod
}

TEST(Effects_GenericProjectileStartsAndCarriesItsFireBundle)
{
	world::ObjectTemplateManager templates;
	addFlame(templates, true);
	world::ObjectTemplate* trail = templates.create("EffectBundle", "e_rocket_trail");
	trail->settings()["setStartOnEffects"] = "0";
	trail->settings()["startAtCreation"] = "0";
	trail->children().push_back({"flame", {0, 0, -0.1f}, {}});
	world::ObjectTemplate* rocket = templates.create("GenericProjectile", "rocket");
	rocket->children().push_back({"e_rocket_trail", {}, {}});

	effects::EffectManager manager(templates);
	effects::ObjectEffects objects(templates);
	effects::ObjectEffects::ObjectState state;
	state.id = 0x80000001u; // projectile effect anchor, separate from game object ids
	state.objectTemplate = rocket;
	state.velocity = {0, 0, 50};
	state.startEvent = true; // the round's engine start (0x005b2c10): event 0 to its own children
	objects.update({state}, manager);
	state.startEvent = false;
	CHECK(manager.emitting(state.id, 1)); // the projectile's fire event starts the trail
	manager.update(1 / 30.0f);
	manager.update(1 / 30.0f); // emission advances in fixed 10 ms steps
	CHECK(!sprites(manager).empty());
	state.position = {0, 0, 2};
	objects.update({state}, manager);
	manager.update(1 / 30.0f);
	const auto particles = sprites(manager);
	CHECK(!particles.empty() && std::fabs(particles.front().position.z - 1.9f) < 1e-3f);
	objects.update({}, manager);
	CHECK(!manager.emitting(state.id) && objects.tracked() == 0);
	for (int i = 0; i < 40; ++i) manager.update(1 / 30.0f);
	CHECK(manager.emitters() == 0 && sprites(manager).empty());
}

// A bundle with minDistanceUnderWaterSurface >= 0 (the amphibious APCs'
// wake, the soldiers' e_sold_swim) runs only while its depth under the
// water is within [min, max], whatever started it; a seated soldier's
// stays off (0x0061a760).
TEST(Effects_DepthGatedBundlesFollowTheWater)
{
	world::ObjectTemplateManager templates;
	addFlame(templates, true);
	world::ObjectTemplate* wake = templates.create("EffectBundle", "e_amphib_wake");
	wake->settings()["setStartOnEffects"] = "32";
	wake->settings()["minDistanceUnderwaterSurface"] = "0";
	wake->settings()["maxDistanceUnderwaterSurface"] = "5";
	wake->children().push_back({"flame", {}, {}});
	world::ObjectTemplate* apc = templates.create("PlayerControlObject", "as_apc_NP");
	apc->children().push_back({"e_amphib_wake", {0, -1, 0}, {}});
	effects::EffectManager manager(templates);
	manager.setWater(true, 10);
	effects::ObjectEffects objects(templates);
	effects::ObjectEffects::ObjectState state;
	state.id = 4;
	state.objectTemplate = apc;
	state.position = {0, 20, 0}; // on land: entering doesn't keep it on
	state.occupied = true;
	objects.update({state}, manager);
	CHECK(!manager.emitting(4, 1));
	state.position = {0, 9.5f, 0}; // the bundle 1.5 m under the surface
	objects.update({state}, manager);
	CHECK(manager.emitting(4, 1));
	state.position = {0, 3, 0}; // 8 m under: past the maximum
	objects.update({state}, manager);
	CHECK(!manager.emitting(4, 1));
	state.position = {0, 9.5f, 0};
	state.seated = true;
	objects.update({state}, manager);
	CHECK(!manager.emitting(4, 1));
	manager.setWater(false, 0);
	state.seated = false;
	objects.update({state}, manager);
	CHECK(!manager.emitting(4, 1));
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
	light->settings()["isDynamic"] = "1";
	light->settings()["color"] = "4/1/0.5";
	light->settings()["speccolor"] = "0.5/0.5/0.5";
	light->settings()["intensity"] = "2"; // the lightmap generator's only (RendDX9 0x1006a310)
	light->settings()["attenuationRange1"] = "2";
	light->settings()["attenuationRange2"] = "5";
	world::ObjectTemplate* off = templates.create("LightSource", "off");
	off->settings()["isDynamic"] = "1";
	off->settings()["enabled"] = "0";
	// isDynamic 0 (the template's default, 0x00616d60): the lightmaps' only.
	world::ObjectTemplate* baked = templates.create("LightSource", "baked");
	world::ObjectTemplate* bundle = templates.create("EffectBundle", "e_boom");
	bundle->children().push_back({"mp_spike", {}, {}});
	bundle->children().push_back({"flash", {0, 1, 0}, {}});
	bundle->children().push_back({"off", {}, {}});
	bundle->children().push_back({"baked", {}, {}});
	CHECK(manager.spawn("e_boom", {10, 0, 0}));
	CHECK(manager.lights().size() == 1);
	const effects::PointLight lit = manager.lights()[0];
	CHECK(lit.position.x == 10 && lit.position.y == 1 && lit.color.x == 4 && lit.range1 == 2 && lit.range2 == 5);
	CHECK(lit.specColor.x == 0.5f && lit.supportsEmitter);
	CHECK(!effects::lightSourceOf(*baked).dynamic && effects::lightSourceOf(*baked).specColor.y == 1);
	// A spot's ConeAngle: coneAngle2 in radians, at most 180 degrees; the
	// ranges at most 100 (BF2142.exe 0x0064b170).
	world::ObjectTemplate* spot = templates.create("LightSource", "spot");
	spot->settings()["lightType"] = "Spot";
	spot->settings()["coneAngle2"] = "25";
	spot->settings()["attenuationRange2"] = "250";
	const effects::LightSourceLook spotLook = effects::lightSourceOf(*spot);
	CHECK(spotLook.spot && std::fabs(spotLook.cone - 25 * 3.14159265f / 180) < 1e-5f && spotLook.range2 == 100);
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
	ripples->settings()["gravity"] = "0"; // the template's default is -9.82 (RendDX9 0x10137290)
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
	CHECK(d.normalMap.empty());
	manager.dropDecals(42);
	CHECK(manager.decals().empty());
	// decalNormalMapName (0x0052cf97) rides with the decal (RendDX9 +0x34 /
	// +0x38), for the vehicles' normal-mapped technique.
	world::ObjectTemplate* armour = templates.create("Decal", "armour");
	armour->settings()["decalTextureName"] = "objects\\effects\\decals\\textures\\Decal_metalsolid_s01_c";
	armour->settings()["decalNormalMapName"] = "objects\\effects\\decals\\textures\\Decal_metalsolid_s01_b";
	CHECK(manager.spawnDecal("armour", {11, 1, 0}, {1, 0, 0}, 42, {10, 0, 0}, {90, 0, 0}));
	CHECK(manager.decals().back().normalMap == "objects/effects/decals/textures/Decal_metalsolid_s01_b");
	// Its owner unseen at the set's last ageing (+0x1a): refused, unless
	// the template isStatic (0x10166410).
	manager.decals().back().ownerDrawn = false;
	CHECK(!manager.spawnDecal("armour", {11, 1, 0}, {1, 0, 0}, 42, {10, 0, 0}, {90, 0, 0}));
	CHECK(manager.spawnDecal("armour", {11, 1, 0}, {1, 0, 0}, 7, {10, 0, 0}, {90, 0, 0})); // another object's set
	armour->settings()["isStatic"] = "1";
	CHECK(manager.spawnDecal("armour", {11, 1, 0}, {1, 0, 0}, 42, {10, 0, 0}, {90, 0, 0}));
}

namespace {

// One sprite system "puff" (no gravity by default here) in a bundle "e_puff".
world::ObjectTemplate* addPuff(world::ObjectTemplateManager& templates)
{
	templates.registerType(1, "EffectBundle");
	templates.registerType(2, "SpriteParticleSystem");
	world::ObjectTemplate* puff = templates.create("SpriteParticleSystem", "puff");
	puff->settings()["emitFrequency"] = "10";
	puff->settings()["emitTime"] = "1";
	puff->settings()["timeToLive"] = "5";
	puff->settings()["gravity"] = "0";
	puff->settings()["textureName"] = "puff";
	world::ObjectTemplate* bundle = templates.create("EffectBundle", "e_puff");
	bundle->children().push_back({"puff", {}, {}});
	return puff;
}

} // namespace

// RendDX9's emitter (0x101407c0 / 0x1013f0c0): the sprite template's
// defaults, quadratic drag towards the wind, pre-warm, the quality level,
// the view distance and the LOD fade.
TEST(Effects_EmitterAsRendDX9)
{
	{
		// The sprite template's defaults: 32 a second, 4 s, -9.82 (0x10138b40).
		world::ObjectTemplateManager templates;
		templates.registerType(2, "SpriteParticleSystem");
		world::ObjectTemplate* t = templates.create("SpriteParticleSystem", "plain");
		const auto p = effects::ParticleTemplate::fromTemplate(*t);
		CHECK(p.emitFrequency == 32 && p.timeToLive == 4 && std::fabs(p.gravity + 9.82f) < 1e-5f);
		t->settings()["LodDistance"] = "Low";
		t->settings()["qualityLevel"] = "High";
		t->settings()["emitPrio"] = "medium";
		t->settings()["particleShaderType"] = "1";
		const auto q = effects::ParticleTemplate::fromTemplate(*t);
		CHECK(q.lodDistance == 0 && q.qualityLevel == 2 && q.emitPrio == 1 && q.heatShimmer);
	}
	// Drag: a particle shot at 20 m/s into still air slows as -k|v|v.
	{
		world::ObjectTemplateManager templates;
		world::ObjectTemplate* puff = addPuff(templates);
		puff->settings()["emitSpeed"] = "20";
		puff->settings()["emitDirection"] = "1/0/0";
		puff->settings()["airResistance"] = "0.1";
		effects::EffectManager manager(templates);
		manager.spawn("e_puff", {});
		for (int i = 0; i < 6; ++i) {
			manager.update(1 / 30.0f);
		}
		const auto& particles = manager.emitterList()[0]->particles();
		CHECK(!particles.empty());
		// v' = -0.1 v^2: from 20, after t seconds v = 20 / (1 + 2 t).
		const effects::Particle& first = particles.front();
		const float expected = 20 / (1 + 2 * first.age);
		CHECK(std::fabs(first.velocity.x - expected) < 0.3f);
		// Wind (5 m/s along z) moves a wind-affected particle towards it.
		puff->settings()["windAffectionFactor"] = "1";
		puff->settings()["emitSpeed"] = "0";
		effects::EffectManager windy(templates);
		scene::WindSettings wind;
		wind.globalWindSpeed = 5;
		windy.setWind(&wind);
		windy.spawn("e_puff", {});
		for (int i = 0; i < 30; ++i) {
			windy.update(1 / 30.0f);
		}
		const auto& blown = windy.emitterList()[0]->particles();
		CHECK(!blown.empty() && blown.front().velocity.z > 0.5f && blown.front().velocity.z < 5.01f);
	}
	// Pre-warm: half a second run at once when the emission starts.
	{
		world::ObjectTemplateManager templates;
		world::ObjectTemplate* puff = addPuff(templates);
		puff->settings()["preWarmTime"] = "0.5";
		effects::EffectManager manager(templates);
		manager.spawn("e_puff", {});
		manager.update(1 / 30.0f);
		const auto& particles = manager.emitterList()[0]->particles();
		CHECK(particles.size() >= 4 && particles.front().age > 0.4f);
	}
	// The manager's quality level and view distance.
	{
		world::ObjectTemplateManager templates;
		world::ObjectTemplate* puff = addPuff(templates);
		puff->settings()["qualityLevel"] = "High";
		scene::ParticleSystemSettings settings;
		settings.qualityLevel = 1;
		effects::EffectManager low(templates);
		low.setSettings(&settings);
		low.spawn("e_puff", {});
		low.update(0.5f);
		CHECK(low.emitters() == 0); // not started (0x1013fb30)
		settings.qualityLevel = 2;
		effects::EffectManager far(templates);
		far.setSettings(&settings);
		far.setCamera({0, 0, 1000}, 0.5f); // beyond 500 m
		far.spawn("e_puff", {});
		far.update(0.5f);
		CHECK(far.emitters() == 0);
		effects::EffectManager near(templates);
		near.setSettings(&settings);
		near.setCamera({0, 0, 10}, 0.5f);
		near.spawn("e_puff", {});
		near.update(0.5f);
		CHECK(near.emitters() == 1 && !near.emitterList()[0]->particles().empty());
		CHECK(near.emitterList()[0]->lodScale() == 1.0f);
	}
	// The draw fade: past 0.85 of the class's distance (High: the view
	// distance), (1 - f)^2.
	{
		scene::ParticleSystemSettings settings;
		effects::ParticleView view;
		view.settings = &settings;
		CHECK(view.fade(100, 2) == 1.0f);
		CHECK(std::fabs(view.fade(0.85f * 500 + 0.5f * 0.15f * 500, 2) - 0.25f) < 1e-4f);
		CHECK(view.fade(600, 2) == 0.0f && view.fade(160, 0) == 0.0f);
	}
}

// Wheel and draft bundles: the ground's material picks the effect, the
// modifiers follow the speed and the altitude (0x0061bd20, 0x0061ce20,
// 0x0061d820).
TEST(Effects_GroundBundles)
{
	world::ObjectTemplateManager templates;
	addPuff(templates);
	templates.registerType(6, "AirDraftEffectBundle");
	templates.registerType(7, "PlayerControlObject");
	world::ObjectTemplate* draft = templates.create("AirDraftEffectBundle", "ade_test");
	draft->settings()["material"] = "62";
	draft->settings()["startDistance"] = "20";
	draft->settings()["AlphaIntensityRange"] = "0.5/1";
	draft->settings()["AlphaIntensityFromAltitude"] = "20/0";
	draft->settings()["EmitSpeedScaleRange"] = "0.3/1";
	draft->settings()["EmitSpeedScaleFromEngine"] = "0/1";
	const auto g = effects::ObjectEffects::GroundBundle::fromTemplate(*draft);
	CHECK(g.material == 62 && g.startDistance == 20);
	CHECK(std::fabs(g.value(0, 0, 0, 10, 0) - 0.75f) < 1e-5f); // halfway down: half the range
	CHECK(g.value(0, 0, 0, 25, 0) == 0.5f && g.value(2, 0, 0, 0, 1) == 1.0f && g.value(1, 0, 0, 0, 0) == 1.0f);
	world::ObjectTemplate* ship = templates.create("PlayerControlObject", "ship");
	ship->children().push_back({"ade_test", {0, 0, 0}, {}});

	effects::EffectManager manager(templates);
	effects::ObjectEffects objects(templates);
	int probes = 0;
	objects.setGround(
		[&probes](const effects::Vec3& from, const effects::Vec3& to, uint32_t, effects::Vec3& hit, uint32_t& material) {
			++probes;
			if (to.y > 0) {
				return false;
			}
			hit = {from.x, 0, from.z}; // flat ground at y 0, material 2
			material = 2;
			return true;
		},
		[](uint32_t bundle, uint32_t ground) { return bundle == 62 && ground == 2 ? std::string("e_puff") : std::string(); });
	effects::ObjectEffects::ObjectState state;
	state.id = 5;
	state.objectTemplate = ship;
	state.position = {0, 10, 0};
	objects.update({state}, manager);
	CHECK(probes == 0 && manager.emitters() == 0); // parked, engine off: no rays
	state.occupied = true;
	objects.update({state}, manager);
	CHECK(probes == 1 && manager.emitting(5, 1));
	CHECK(std::fabs(manager.emitterList()[0]->alphaIntensity - 0.75f) < 1e-4f);
	// Up beyond startDistance: the draft stops.
	state.position = {0, 30, 0};
	objects.update({state}, manager);
	CHECK(!manager.emitting(5, 1));
}

// The cold breath (0x00588830): a soldier's breathEffect while it breathes out.
TEST(Effects_ColdBreath)
{
	world::ObjectTemplateManager templates;
	addPuff(templates);
	templates.registerType(8, "Soldier");
	world::ObjectTemplate* soldier = templates.create("Soldier", "bp1_soldier");
	soldier->settings()["breathEffect"] = "e_puff";
	effects::EffectManager manager(templates);
	effects::ObjectEffects objects(templates);
	effects::ObjectEffects::ObjectState state;
	state.id = 3;
	state.objectTemplate = soldier;
	objects.update({state}, manager);
	CHECK(!manager.emitting(3));
	state.coldBreath = true;
	objects.update({state}, manager);
	CHECK(manager.emitting(3, effects::ObjectEffects::kBreathTag));
	state.coldBreath = false;
	objects.update({state}, manager);
	CHECK(!manager.emitting(3));
}

// VideoSettings' levels as RendDX9's switch applies them (0x10010040,
// 0x100991b0).
TEST(Effects_QualityLevels)
{
	scene::QualitySettings q;
	scene::ParticleSystemSettings s;
	q.applyEffects(s);
	CHECK(s.viewDistance == 500 && s.maxParticles == 1000 && s.particleSystemShaderQuality == 3 && s.qualityLevel == 2);
	q.effects = 1;
	q.terrain = 1;
	q.geometry = 2;
	q.texture = 1;
	q.dynamicLighting = 0;
	q.applyEffects(s);
	CHECK(s.viewDistance == 250 && s.maxParticles == 300 && s.particleSystemShaderQuality == 1 && s.qualityLevel == 0);
	CHECK(std::fabs(s.startFadePercentageOfViewDistance - 0.2f) < 1e-6f);
	CHECK(q.terrainLodSwitchDistance() == 64 && q.undergrowthViewDistanceScale() == 0.5f);
	CHECK(std::fabs(q.geometryLodFactor() - 0.8f) < 1e-6f && q.skipMipLevels() == 2);
	// The texture classes (0x10010040, 0x10017ca0): the menu never, the
	// undergrowth and overgrowth one less, at most one.
	CHECK(q.skipMipLevels(scene::TextureClass::BundledMesh) == 2 && q.skipMipLevels(scene::TextureClass::Interface) == 0);
	CHECK(q.skipMipLevels(scene::TextureClass::Undergrowth) == 1 && q.skipMipLevels(scene::TextureClass::Overgrowth) == 1);
	q.texture = 2;
	CHECK(q.skipMipLevels(scene::TextureClass::Default) == 1 && q.skipMipLevels(scene::TextureClass::Overgrowth) == 0);
	// The lighting level (renderer +0x6b4) and what it drops.
	CHECK(q.lightingLevel() == 0 && q.meshNormalMaps());
	q.lighting = 2;
	CHECK(q.lightingLevel() == 1 && !q.meshNormalMaps() && q.meshHemiAndShadows());
	q.lighting = 1;
	CHECK(q.lightingLevel() == 3 && !q.meshHemiAndShadows() && !q.staticMeshDirtAndCrack());
	CHECK(!q.dynamicLightsEnabled() && !q.decalsEnabled());
	q.viewDistanceScale = 0.1f;
	q.applyEffects(s);
	CHECK(s.viewDistance == 200); // at least 200
}

// A wheel bundle under a wheel's spring follows the wheel's own contact
// (0x0061bfc0), not a probe under itself.
TEST(Effects_WheelBundlesFollowTheirContact)
{
	world::ObjectTemplateManager templates;
	addPuff(templates);
	templates.registerType(5, "WheelEffectBundle");
	templates.registerType(7, "PlayerControlObject");
	templates.registerType(9, "Spring");
	world::ObjectTemplate* dust = templates.create("WheelEffectBundle", "wheel_dust");
	dust->settings()["material"] = "67";
	world::ObjectTemplate* spring = templates.create("Spring", "car_wheel_fl");
	spring->children().push_back({"wheel_dust", {0, 0, 0}, {}});
	world::ObjectTemplate* car = templates.create("PlayerControlObject", "car");
	car->children().push_back({"car_wheel_fl", {1, 0, 2}, {}});
	effects::EffectManager manager(templates);
	effects::ObjectEffects objects(templates);
	objects.setGround(
		[](const effects::Vec3& from, const effects::Vec3&, uint32_t, effects::Vec3& hit, uint32_t& material) {
			hit = from;
			material = 2;
			return true;
		},
		[](uint32_t bundle, uint32_t ground) { return bundle == 67 && ground == 2 ? std::string("e_puff") : std::string(); });
	effects::ObjectEffects::ObjectState state;
	state.id = 8;
	state.objectTemplate = car;
	state.occupied = true;
	state.wheels.push_back({"car_wheel_fl", false, {1, 0, 2}});
	objects.update({state}, manager);
	CHECK(!manager.emitting(8, 1)); // in the air
	state.wheels[0].contact = true;
	objects.update({state}, manager);
	CHECK(manager.emitting(8, 1));
	// The wheel's tick hands its effect's systems (1, 1, sign of the speed
	// along the vehicle's forward) (0x0061bd20 -> 0x00618c80), whatever
	// emitDirectionZFromSpeedDof says.
	const auto directionZ = [&manager] {
		float z = 0;
		for (const auto& emitter : manager.emitterList()) {
			if (emitter->anchor == 8 && emitter->tag == 1) {
				z = emitter->directionScale.z;
			}
		}
		return z;
	};
	dust->settings()["emitDirectionZFromSpeedDof"] = "0";
	state.velocity = {0, 0, 5};
	objects.update({state}, manager);
	CHECK(directionZ() == 1);
	state.velocity = {0, 0, -5};
	objects.update({state}, manager);
	CHECK(directionZ() == -1);
}

// The WindAffector winds (0x0064b5f0) and the WindManager's sum (0x00703b20).
TEST(Effects_LocalWinds)
{
	using Wind = scene::WindSettings::LocalWind;
	Wind rotor;
	rotor.type = Wind::Type::RadialDir;
	rotor.position = {0, 10, 0};
	rotor.direction = {0, -1, 0};
	rotor.speed = 20;
	rotor.falloff = 20;
	rotor.radialFalloff = 10;
	// On the axis 5 m down: (1 - 5/20) x 20 along it (bent 0.2 out, no radial part).
	auto w = rotor.windAt({0, 5, 0});
	CHECK(std::fabs(w[1] + 0.8f * 15) < 1e-3f);
	// Beyond the radial falloff or the reach: none.
	w = rotor.windAt({11, 9, 0});
	CHECK(w[0] == 0 && w[1] == 0);
	w = rotor.windAt({0, -15, 0});
	CHECK(w[1] == 0);
	Wind blast;
	blast.type = Wind::Type::Point;
	blast.speed = 10;
	blast.falloff = 10;
	w = blast.windAt({5, 0, 0});
	CHECK(std::fabs(w[0] - 7.5f) < 1e-4f);
	scene::WindSettings all;
	all.globalWindSpeed = 1;
	all.localWinds = {blast};
	w = all.windAt({5, 0, 0});
	CHECK(std::fabs(w[0] - 7.5f) < 1e-4f && std::fabs(w[2] - 1) < 1e-4f);

	// An object carrying a WindAffector in its tree hands its wind out,
	// turned with the object.
	world::ObjectTemplateManager templates;
	templates.registerType(7, "PlayerControlObject");
	templates.registerType(10, "Engine");
	world::ObjectTemplate* engine = templates.create("Engine", "heli_rotor");
	engine->components().push_back("WindAffector");
	engine->settings()["windType"] = "RadialDir";
	engine->settings()["windDirection"] = "0/0/-1";
	engine->settings()["windSpeed"] = "20";
	engine->settings()["windFalloff"] = "20";
	engine->settings()["windRadialFalloff"] = "10";
	world::ObjectTemplate* heli = templates.create("PlayerControlObject", "heli");
	heli->children().push_back({"heli_rotor", {0, 2, 0}, {}});
	effects::EffectManager manager(templates);
	effects::ObjectEffects objects(templates);
	effects::ObjectEffects::ObjectState state;
	state.id = 4;
	state.objectTemplate = heli;
	state.position = {10, 0, 0};
	objects.update({state}, manager);
	CHECK(objects.localWinds().size() == 1);
	const Wind& local = objects.localWinds()[0];
	CHECK(local.type == Wind::Type::RadialDir && std::fabs(local.position[0] - 10) < 1e-4f
		&& std::fabs(local.position[1] - 2) < 1e-4f && std::fabs(local.direction[2] + 1) < 1e-4f);

	// An explosion bundle spawned on its own blows its Point wind for
	// windBlowTime, at most as long as the bundle (0x0064b490 / 0x0064b4c0).
	templates.registerType(1, "EffectBundle");
	addFlame(templates, false);
	world::ObjectTemplate* explosion = templates.create("EffectBundle", "e_exp_small");
	explosion->components().push_back("WindAffector");
	explosion->settings()["windType"] = "Point";
	explosion->settings()["windSpeed"] = "20";
	explosion->settings()["windFalloff"] = "10";
	explosion->settings()["windBlowTime"] = "0.5";
	explosion->children().push_back({"flame", {}, {}});
	CHECK(manager.spawn("e_exp_small", {3, 0, 0}));
	std::vector<Wind> blowing;
	manager.appendLocalWinds(blowing);
	CHECK(blowing.size() == 1 && blowing[0].type == Wind::Type::Point && std::fabs(blowing[0].position[0] - 3) < 1e-4f);
	for (int i = 0; i < 18; ++i) {
		manager.update(1 / 30.0f);
	}
	blowing.clear();
	manager.appendLocalWinds(blowing);
	CHECK(blowing.empty());
}

// A system's particles inherit only the velocity speedFromPhysics sets
// (0x00618ae0); the systems' own moves give them none (the constructors
// zero it, sprite 0x10137290 / mesh 0x10146820): a rocket's sparks stay
// where they are born, they aren't thrown on at the rocket's speed.
TEST(Effects_MovingSystemGivesItsParticlesNoVelocity)
{
	world::ObjectTemplateManager templates;
	world::ObjectTemplate* spark = addPuff(templates);
	spark->settings()["emitFrequency"] = "30";
	spark->settings()["emitTime"] = "0";
	spark->settings()["timeToLive"] = "2";
	effects::EffectManager manager(templates);
	manager.setAnchor(4, {}, {});
	CHECK(manager.spawnAttached("e_puff", 4, {}, {}, effects::EffectManager::View::Any, 1));
	for (int i = 1; i <= 10; ++i) {
		manager.setAnchor(4, {100.0f * static_cast<float>(i) / 30.0f, 0, 0}, {}); // 100 m/s along x
		manager.update(1 / 30.0f);
	}
	const auto& particles = manager.emitterList()[0]->particles();
	CHECK(particles.size() >= 5);
	for (const effects::Particle& p : particles) {
		CHECK(std::fabs(p.velocity.x) < 1.0f);
	}
}

// alphaCull, pivotOffset, drawOrder and collisionEnable of a sprite system.
TEST(Effects_SpriteDrawAndCollision)
{
	world::ObjectTemplateManager templates;
	world::ObjectTemplate* puff = addPuff(templates);
	puff->settings()["alphaCull"] = "0.2";
	puff->settings()["pivotOffset"] = "0/-0.5";
	puff->settings()["drawOrder"] = "Front";
	puff->settings()["collisionEnable"] = "1";
	puff->settings()["collisionSpeedAlongNormal"] = "0.5";
	puff->settings()["gravity"] = "-10";
	const auto t = effects::ParticleTemplate::fromTemplate(*puff);
	CHECK(t.alphaCull == 0.2f && t.pivot[1] == -0.5f && !t.drawBack && t.collision
		&& t.collisionAlongNormal == 0.5f);
	effects::EffectManager manager(templates);
	manager.setGround([](float, float) { return 0.0f; });
	manager.spawn("e_puff", {0, 1, 0});
	for (int i = 0; i < 60; ++i) {
		manager.update(1 / 30.0f);
	}
	bool above = true;
	for (const effects::Particle& p : manager.emitterList()[0]->particles()) {
		above = above && p.position.y >= -1e-4f;
	}
	CHECK(above); // they stay on the ground
	int sprites = 0;
	manager.forEachGroup([&](const std::string&, bool, const std::vector<effects::Sprite>& s) {
		sprites += static_cast<int>(s.size());
		CHECK(s.empty() || s[0].pivot[1] == -0.5f);
	});
	CHECK(sprites > 0);
	puff->settings()["alphaCull"] = "2"; // above any alpha: nothing drawn
	effects::EffectManager culled(templates);
	culled.spawn("e_puff", {0, 1, 0});
	culled.update(0.5f);
	int drawn = 0;
	culled.forEachGroup([&](const std::string&, bool, const std::vector<effects::Sprite>& s) { drawn += static_cast<int>(s.size()); });
	CHECK(drawn == 0);
}

// RendDX9's collector (0x10130bd0) culls on the particle's alpha (+0x2c:
// its alpha intensity, +0x58, by the LOD fade); the transparency graph is
// the vertex shader's and never culls. The gunships' jet hotspot
// (em_rearjetHelo_hotspot: graph 0/-0.09/0/0.09, alphaCull 0.1) draws,
// also at a hover's alpha intensity of 0.25.
TEST(Effects_AlphaCullIgnoresTheTransparencyGraph)
{
	world::ObjectTemplateManager templates;
	world::ObjectTemplate* hotspot = addPuff(templates);
	hotspot->settings()["transparencyGraph"] = "0/-0.09/0/0.09";
	hotspot->settings()["alphaCull"] = "0.1";
	hotspot->settings()["emitFrequency"] = "60";
	hotspot->settings()["timeToLive"] = "0.1";
	hotspot->settings()["emitTime"] = "0";
	effects::EffectManager manager(templates);
	manager.setAnchor(3, {}, {});
	CHECK(manager.spawnAttached("e_puff", 3, {}, {}, effects::EffectManager::View::Any, 1));
	manager.setModifier(3, 1, 0, 0.25f);
	for (int i = 0; i < 6; ++i) {
		manager.update(1 / 30.0f);
	}
	const auto drawn = sprites(manager);
	CHECK(!drawn.empty());
	for (const auto& s : drawn) {
		CHECK(s.color[3] > 0 && s.color[3] <= 0.09f * 0.25f + 1e-4f); // drawn at graph x intensity
	}
	manager.setModifier(3, 1, 0, 0.05f); // below alphaCull: culled
	for (int i = 0; i < 6; ++i) {
		manager.update(1 / 30.0f);
	}
	CHECK(sprites(manager).empty());
}

// An emit time of 0 isn't run down (RendDX9 0x10140590): an attached
// system emits until its bundle stops (the jets' setStopOnEffects).
TEST(Effects_ZeroEmitTimeEmitsUntilStopped)
{
	world::ObjectTemplateManager templates;
	world::ObjectTemplate* jet = addPuff(templates);
	jet->settings()["emitFrequency"] = "60";
	jet->settings()["timeToLive"] = "0.1";
	jet->settings()["emitTime"] = "0";
	effects::EffectManager manager(templates);
	manager.setAnchor(3, {}, {});
	CHECK(manager.spawnAttached("e_puff", 3, {}, {}, effects::EffectManager::View::Any, 1));
	for (int i = 0; i < 90; ++i) {
		manager.update(1 / 30.0f);
	}
	CHECK(manager.emitting(3, 1) && !sprites(manager).empty()); // still going after 3 s
	manager.stopAttached(3, 1);
	for (int i = 0; i < 10; ++i) {
		manager.update(1 / 30.0f);
	}
	CHECK(manager.emitters() == 0);
}

// A bundle spawned on its own lives its timeToLive (sampled at creation,
// 0x00619700; 1 s by the template's constructor, 0x0061a9b0), an infinite
// one 0.5 s (no parent, 0x00618600); its emit-time-0 and looping systems
// emit until then (0x0061a760), its particles living on.
TEST(Effects_WorldBundleLivesItsTimeToLive)
{
	world::ObjectTemplateManager templates;
	world::ObjectTemplate* glow = addPuff(templates);
	glow->settings()["emitFrequency"] = "30";
	glow->settings()["timeToLive"] = "0.2";
	glow->settings()["emitTime"] = "0";
	world::ObjectTemplate* bundle = templates.find("e_puff");
	const auto emitsAt = [&](float seconds) {
		effects::EffectManager manager(templates);
		manager.spawn("e_puff", {0, 0, 0});
		for (float t = 0; t < seconds; t += 1 / 30.0f) {
			manager.update(1 / 30.0f);
		}
		return !manager.emitterList().empty() && !manager.emitterList()[0]->stopped();
	};
	// The Titan core's explosion (e_vexp_eu_titanCore): 50 s.
	bundle->settings()["timeToLive"] = "CRD_NONE/50/0/0";
	CHECK(emitsAt(49.5f) && !emitsAt(50.5f));
	// No timeToLive: 1 s.
	bundle->settings().erase("timeToLive");
	CHECK(emitsAt(0.9f) && !emitsAt(1.1f));
	// Infinite with no parent: 0.5 s.
	bundle->settings()["timeToLive"] = "CRD_NONE/-1/0/0";
	CHECK(emitsAt(0.4f) && !emitsAt(0.6f));
	// A looping system is held the same way; stopped, its particles live on.
	glow->settings()["emitTime"] = "0.1";
	glow->settings()["isLooping"] = "1";
	bundle->settings()["timeToLive"] = "CRD_NONE/3/0/0";
	CHECK(emitsAt(2.9f) && !emitsAt(3.1f));
	effects::EffectManager manager(templates);
	manager.spawn("e_puff", {0, 0, 0});
	for (int i = 0; i < 92; ++i) {
		manager.update(1 / 30.0f);
	}
	CHECK(!sprites(manager).empty()); // 3.07 s: stopped, the last ones still there
	for (int i = 0; i < 15; ++i) {
		manager.update(1 / 30.0f);
	}
	CHECK(manager.emitters() == 0);
}

// An EffectBundle's modifiers (0x00618e30): with a mapping set anywhere, the
// mapped ones follow their inputs; otherwise the Used ones take their min.
TEST(Effects_BundleModifiersAsTheExe)
{
	world::ObjectTemplateManager templates;
	addPuff(templates);
	world::ObjectTemplate* used = templates.create("EffectBundle", "e_used");
	used->children().push_back({"puff", {}, {}});
	used->settings()["AgeScaleFactorUsed"] = "1";
	used->settings()["AgeScaleFactorRange"] = "1.5/1";
	used->settings()["AlphaIntensityRange"] = "0.2/0.4"; // not used: left alone
	world::ObjectTemplate* mapped = templates.create("EffectBundle", "e_mapped");
	mapped->children().push_back({"puff", {}, {}});
	mapped->settings()["EmitScaleFactorRange"] = "0.5/1";
	mapped->settings()["EmitScaleFactorFromSpeed"] = "0/20";
	mapped->settings()["AgeScaleFactorUsed"] = "1"; // a mapping elsewhere: Used is not read
	mapped->settings()["AgeScaleFactorRange"] = "3/3";

	const auto u = effects::BundleModifiers::fromTemplate(*used);
	CHECK(!u.anyMapping && u.any() && u.modifiers[3].used);
	std::array<float, 6> values {};
	CHECK(u.evaluate({}, values) == 8u && values[3] == 1.5f);
	const auto m = effects::BundleModifiers::fromTemplate(*mapped);
	CHECK(m.anyMapping);
	effects::BundleModifiers::Inputs moving;
	moving.speed = 10;
	CHECK(m.evaluate(moving, values) == 2u && std::fabs(values[1] - 0.75f) < 1e-5f);
	moving.speed = 50;
	m.evaluate(moving, values);
	CHECK(values[1] == 1.0f);

	// Spawned on their own (no physics: speed 0).
	effects::EffectManager manager(templates);
	CHECK(manager.spawn("e_used", {0, 0, 0}));
	CHECK(manager.emitterList().back()->timeScale == 1.5f && manager.emitterList().back()->alphaIntensity == 1.0f);
	CHECK(manager.spawn("e_mapped", {0, 0, 0}));
	CHECK(manager.emitterList().back()->emitScale == 0.5f && manager.emitterList().back()->timeScale == 1.0f);
}

// A mesh particle system's directionalScale (RendDX9 0x101357c0): the
// particles follow their velocity and stretch along it by speed x scale.
TEST(Effects_MeshParticlesDirectionalScale)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "EffectBundle");
	templates.registerType(3, "MeshParticleSystem");
	world::ObjectTemplate* spark = templates.create("MeshParticleSystem", "mp_spark");
	spark->settings()["emitFrequency"] = "100";
	spark->settings()["emitTime"] = "0.1";
	spark->settings()["timeToLive"] = "2";
	spark->settings()["gravity"] = "0";
	spark->settings()["emitSpeed"] = "10";
	spark->settings()["randomRotation"] = "0/0/90";
	spark->settings()["directionalScale"] = "0.5";
	spark->setGeometry("mp_spark");
	effects::EffectManager manager(templates);
	CHECK(manager.spawn("mp_spark", {0, 0, 0}));
	for (int i = 0; i < 3; ++i) {
		manager.update(1 / 30.0f);
	}
	size_t checked = 0;
	manager.forEachMeshGroup([&](const std::string&, bool, const std::vector<effects::MeshSprite>& list) {
		for (const effects::MeshSprite& m : list) {
			// The particle's own roll dropped: z its direction of travel.
			const effects::Vec3 z = m.rotation.rows[2];
			CHECK(std::fabs(physics::lengthSquared(z) - 1) < 1e-4f && m.stretch > 1);
			CHECK(std::fabs(m.rotation.rows[0].y) < 1e-4f); // x level
			++checked;
		}
	});
	CHECK(checked > 0);
}

TEST(Effects_MeshParticlesSignedScale)
{
	for (const float scale : {-1.0f, 0.0f, 2.0f}) {
		world::ObjectTemplateManager templates;
		templates.registerType(3, "MeshParticleSystem");
		auto* spike = templates.create("MeshParticleSystem", "mp_spike");
		spike->setGeometry("mp_smokespike");
		spike->settings()["emitFrequency"] = "33";
		spike->settings()["emitTime"] = "2";
		spike->settings()["timeToLive"] = "3";
		spike->settings()["emitSpeed"] = "10";
		spike->settings()["gravity"] = "0";
		spike->settings()["particleMaxSize"] = std::to_string(scale);
		spike->settings()["randomSize"] = "0";
		spike->settings()["sizeGraph"] = "0/0/0/1";
		spike->settings()["transparencyGraph"] = "0/-0.25/0.25/0";
		spike->settings()["directionalScale"] = "3";
		spike->settings()["alignRotationToSpeed"] = "1";
		effects::EffectManager manager(templates);
		CHECK(manager.spawn("mp_spike", {}));
		manager.update(0.5f);
		size_t drawn = 0;
		manager.forEachMeshGroup([&](const std::string&, bool, const std::vector<effects::MeshSprite>& list) {
			for (const auto& mesh : list) {
				CHECK(mesh.size == scale);
				CHECK(mesh.color[3] > 0);
				CHECK(std::fabs(mesh.stretch - 30) < 1e-4f);
				++drawn;
			}
		});
		CHECK(!manager.emitterList().front()->particles().empty());
		CHECK(scale == 0 ? drawn == 0 : drawn > 0);
	}
}

// A mesh particle's turn (RendDX9 0x10144d90, 0x101357c0): one random
// start for the three axes, a speed of rotationSpeed + randomRotationSpeed
// x a random with its sign, scaled by the rotation graph; isStaticSystem
// doesn't run a mesh system ahead (0x10145bf0).
TEST(Effects_MeshParticlesSpin)
{
	world::ObjectTemplateManager templates;
	templates.registerType(3, "MeshParticleSystem");
	world::ObjectTemplate* chunk = templates.create("MeshParticleSystem", "mp_chunk");
	chunk->settings()["emitFrequency"] = "100";
	chunk->settings()["emitTime"] = "0.05";
	chunk->settings()["timeToLive"] = "10";
	chunk->settings()["gravity"] = "0";
	chunk->settings()["randomRotation"] = "10/20/40";
	chunk->settings()["rotationSpeed"] = "100/0/0";
	chunk->settings()["randomRotationSpeed"] = "0/50/0";
	chunk->settings()["rotationGraph"] = "0/0/0/0.5";
	chunk->settings()["isStaticSystem"] = "1";
	chunk->setGeometry("mp_chunk");
	effects::EffectManager manager(templates);
	CHECK(manager.spawn("mp_chunk", {0, 0, 0}));
	manager.update(0.05f);
	const effects::Emitter& emitter = *manager.emitterList().back();
	CHECK(!emitter.particles().empty());
	std::vector<effects::Particle> before(emitter.particles().begin(), emitter.particles().end());
	for (const effects::Particle& p : before) {
		CHECK(p.age < 0.1f); // not run ahead
		// The start (less its turn since): one random x (10, 20, 40).
		const effects::Vec3 start = p.angles - p.spin * (0.5f * p.age);
		CHECK(std::fabs(start.y - 2 * start.x) < 0.1f && std::fabs(start.z - 4 * start.x) < 0.1f);
		// The speed: 100 with the random's sign on x, 50 x it on y.
		CHECK(std::fabs(std::fabs(p.spin.x) - 100) < 1e-3f && std::fabs(p.spin.y) <= 50 && p.spin.z == 0);
		CHECK((p.spin.x > 0) == (p.spin.y >= 0));
	}
	manager.update(1.0f);
	const effects::Particle& p = emitter.particles().front();
	const effects::Particle& q = before.front();
	// A second at half speed (the graph's 0.5).
	CHECK(std::fabs((p.angles.x - q.angles.x) - q.spin.x * 0.5f) < 1.0f);
}

// The rain's 16 deviation groups (RendDX9 0x1011a790 / 0x1011a860).
TEST(Effects_RainDeviationGroups)
{
	scene::RainSettings s;
	s.enabled = true;
	s.particleCount = 32;
	s.maxDeviation = {0.5f, 0, 0.25f};
	s.angleSpeed = {90, 10};
	s.randomness = 1000000; // no speed changes in a few frames
	effects::Weather weather;
	weather.update(s, {}, 0);
	for (const effects::Weather::Deviation& d : weather.deviations()) {
		CHECK(d.size[0] >= 0 && d.size[0] < 0.5f && d.size[1] == 0 && d.size[2] < 0.25f);
		CHECK(d.angle[0] >= 0 && d.angle[0] < 360 && d.speed[0] == 90 && d.speed[1] == 10);
	}
	const effects::Weather::Deviation before = weather.deviations()[3];
	weather.update(s, {}, 0.5f);
	const effects::Weather::Deviation& after = weather.deviations()[3];
	// The offset from the angles before the turn; both turn by angleSpeed.x + their speed.
	CHECK(std::fabs(after.offset.x - std::cos(before.angle[0] * 0.017453292f) * before.size[0]) < 1e-5f);
	CHECK(std::fabs(after.offset.z - std::sin(before.angle[0] * 0.017453292f) * before.size[2]) < 1e-5f);
	CHECK(after.offset.y == 0);
	CHECK(std::fabs(std::fmod(before.angle[0] + 90, 360) - after.angle[0]) < 1e-3f);
	CHECK(std::fabs(std::fmod(before.angle[1] + 50, 360) - after.angle[1]) < 1e-3f);
	// randomness 1: every frame a change, to 0 x maxRandomChange (randomDeviationAngleSpeed 1).
	s.randomness = 1;
	weather.update(s, {}, 0.1f);
	CHECK(weather.deviations()[0].speed[0] == 0 && weather.deviations()[0].speed[1] == 0);
}

// A rain particle's deviation group is drawn as it is made, rand() & 15
// (RendDX9 0x1011b1b0; Rain.fx Data.y from 0x1011cf00), not its index.
TEST(Effects_RainParticleGroupsAreRandom)
{
	scene::RainSettings s;
	s.enabled = true;
	s.particleCount = 64;
	effects::Weather weather;
	weather.update(s, {}, 0);
	const std::vector<uint8_t>& groups = weather.groups();
	CHECK(groups.size() == 64);
	// MSVC's rand() seeded 1: 41, 18467, 6334, 26500 ...
	CHECK(groups[0] == (41 & 15) && groups[1] == (18467 & 15) && groups[2] == (6334 & 15) && groups[3] == (26500 & 15));
	int byIndex = 0;
	for (size_t i = 0; i < groups.size(); ++i) {
		CHECK(groups[i] < 16);
		byIndex += groups[i] == i % 16 ? 1 : 0;
	}
	CHECK(byIndex < 16);
	// Kept while the count is.
	const std::vector<uint8_t> before = groups;
	weather.update(s, {}, 0.1f);
	CHECK(weather.groups() == before);
}

// A vehicle's bundles seen from inside by its own occupant (0x006187c0:
// the local player in the inside or nose view under the same root, no
// neverDrawAs1p on the way; GameWorld::firstPersonEffects): their
// first-person systems (showInFirstPerson), not the third-person ones
// (0x0061a1c0); anyone else's vehicle, or a Titan's, the third-person ones.
TEST(Effects_ObjectBundlesFirstPersonForm)
{
	world::ObjectTemplateManager templates;
	addFlame(templates, true, "1", "0"); // a cockpit glow: first person only
	world::ObjectTemplate* glow = templates.create("EffectBundle", "e_cockpit");
	glow->settings()["startAtCreation"] = "1";
	glow->children().push_back({"flame", {}, {}});
	world::ObjectTemplate* car = templates.create("PlayerControlObject", "car");
	car->children().push_back({"e_cockpit", {0, 1, 0}, {}});
	effects::EffectManager manager(templates);
	effects::ObjectEffects objects(templates);
	effects::ObjectEffects::ObjectState theirs;
	theirs.id = 7;
	theirs.objectTemplate = car;
	objects.update({theirs}, manager);
	CHECK(manager.emitters() == 0); // the third-person form shows nothing of it
	effects::ObjectEffects::ObjectState mine = theirs;
	mine.id = 8;
	mine.occupied = true;
	mine.firstPerson = true;
	objects.update({theirs, mine}, manager);
	CHECK(manager.emitters() == 1 && manager.emitting(8, 1) && !manager.emitting(7, 1));
}

// RendDX9's emit ranges (0x1013a080): a set fourth field flips the value's
// sign half the time, so a Box emitter spawns on both sides.
TEST(Effects_EmitRangeSignFlip)
{
	world::ObjectTemplateManager templates;
	world::ObjectTemplate* puff = addPuff(templates);
	puff->settings()["emitterType"] = "Box";
	puff->settings()["emitFrequency"] = "100";
	puff->settings()["emitRangeX"] = "CRD_UNIFORM/1/2/1";
	puff->settings()["emitRangeY"] = "CRD_UNIFORM/1/2/0";
	effects::EffectManager manager(templates);
	manager.spawn("e_puff", {});
	for (int i = 0; i < 15; ++i) {
		manager.update(1 / 30.0f);
	}
	int negativeX = 0, positiveX = 0, negativeY = 0;
	for (const effects::Particle& p : manager.emitterList()[0]->particles()) {
		(p.position.x < 0 ? negativeX : positiveX) += 1;
		negativeY += p.position.y < 0 ? 1 : 0;
		CHECK(std::fabs(p.position.x) >= 0.99f && std::fabs(p.position.x) <= 2.01f);
	}
	CHECK(negativeX > 5 && positiveX > 5);
	CHECK(negativeY == 0);
}

// A trail's points move after they are left (0x1013a600): emitSpeed up and
// gravity down, in steps of 0.01 s.
TEST(Effects_TrailPointsMove)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "EffectBundle");
	templates.registerType(2, "TrailSystem");
	world::ObjectTemplate* trail = templates.create("TrailSystem", "trail");
	trail->settings()["emitFrequency"] = "10";
	trail->settings()["timeToLive"] = "5";
	trail->settings()["isLooping"] = "1";
	trail->settings()["minTrailVelocity"] = "0";
	trail->settings()["emitSpeed"] = "2";
	trail->settings()["emitDirection"] = "0/1/0";
	trail->settings()["gravity"] = "0";
	trail->settings()["textureName"] = "trail";
	world::ObjectTemplate* bundle = templates.create("EffectBundle", "e_trail");
	bundle->children().push_back({"trail", {}, {}});
	effects::EffectManager manager(templates);
	manager.setAnchor(3, {0, 0, 0}, {});
	CHECK(manager.spawnAttached("e_trail", 3, {}));
	for (int i = 0; i < 30; ++i) {
		manager.update(1 / 30.0f);
	}
	CHECK(!manager.emitterList().empty());
	const auto& points = manager.emitterList()[0]->particles();
	CHECK(!points.empty());
	if (!points.empty()) {
		// The oldest point has risen at 2 m/s for its age.
		const effects::Particle& first = points.front();
		CHECK(first.age > 0.5f);
		CHECK(std::fabs(first.position.y - 2 * first.age) < 0.1f);
		CHECK(std::fabs(first.velocity.y - 2) < 1e-4f);
	}
}

// drawOrder (0x006232e0 -> RendDX9 0x10139aa0, the collector 0x10130bd0,
// the frame 0x10131b70): the Back systems' particles are drawn before the
// Front ones', and each system's go back to front from the camera (0x1012f800).
TEST(Effects_DrawOrderAndDepthSort)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "EffectBundle");
	templates.registerType(2, "SpriteParticleSystem");
	const auto system = [&](const char* name, const char* order, const char* texture) {
		world::ObjectTemplate* t = templates.create("SpriteParticleSystem", name);
		t->settings()["emitFrequency"] = "30";
		t->settings()["emitTime"] = "1";
		t->settings()["emitSpeed"] = "4";
		t->settings()["emitDirection"] = "0/0/1";
		t->settings()["timeToLive"] = "5";
		t->settings()["gravity"] = "0";
		t->settings()["textureName"] = texture;
		t->settings()["drawOrder"] = order;
	};
	system("front", "Front", "front");
	system("back", "Back", "back");
	world::ObjectTemplate* bundle = templates.create("EffectBundle", "e_two");
	bundle->children().push_back({"front", {}, {}});
	bundle->children().push_back({"back", {}, {}});
	effects::EffectManager manager(templates);
	CHECK(manager.spawn("e_two", {}));
	for (int i = 0; i < 15; ++i) {
		manager.update(1 / 30.0f);
	}
	effects::ParticleView view;
	view.eye = {0, 0, -10}; // behind the start: the newest are the nearest
	std::vector<std::string> order;
	bool farFirst = true;
	manager.forEachGroup(view, [&](const std::string& texture, bool, const std::vector<effects::Sprite>& list) {
		order.push_back(texture);
		for (size_t i = 1; i < list.size(); ++i) {
			farFirst = farFirst && list[i - 1].position.z >= list[i].position.z;
		}
	});
	CHECK(order.size() == 2 && order[0] == "back" && order[1] == "front");
	CHECK(farFirst);
}
