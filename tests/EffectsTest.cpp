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
