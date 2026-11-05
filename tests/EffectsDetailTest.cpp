// Particle and debris emitter detail: emitterType and emitRadius,
// isStaticSystem, the debris Emitter and its Particle pieces; the 3P hit
// reactions' names.
#include "Effects/Effects.h"
#include "Game/GameWorld.h"
#include "TestMain.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>

using namespace dice::hfe;

namespace {

world::ObjectTemplate* sprites(world::ObjectTemplateManager& templates, const char* name, const char* type)
{
	world::ObjectTemplate* t = templates.create("SpriteParticleSystem", name);
	t->settings()["emitterType"] = type;
	t->settings()["emitFrequency"] = "200";
	t->settings()["emitTime"] = "0.5";
	t->settings()["timeToLive"] = "5";
	t->settings()["gravity"] = "0";
	t->settings()["emitSpeed"] = "0";
	t->settings()["textureName"] = "smoke";
	return t;
}

std::vector<effects::Vec3> particlesOf(const effects::EffectManager& manager)
{
	std::vector<effects::Vec3> at;
	for (const auto& emitter : manager.emitterList()) {
		for (const auto& p : emitter->particles()) {
			at.push_back(emitter->worldPosition(p));
		}
	}
	return at;
}

} // namespace

TEST(EffectsDetail_StandardEmitsInItsRadius)
{
	world::ObjectTemplateManager templates;
	world::ObjectTemplate* t = sprites(templates, "ring", "Standard");
	t->settings()["emitRadius"] = "4/0/2";
	t->settings()["emitRangeX"] = "CRD_UNIFORM/50/60/0"; // a Standard system ignores the ranges
	effects::EffectManager manager(templates);
	CHECK(manager.spawn("ring", {10, 0, 0}));
	for (int i = 0; i < 6; ++i) {
		manager.update(1 / 30.0f);
	}
	const auto at = particlesOf(manager);
	CHECK(at.size() > 20);
	float widest = 0;
	bool inside = true;
	for (const auto& p : at) {
		const float x = (p.x - 10) / 4, z = p.z / 2;
		inside = inside && x * x + z * z <= 1.0001f && std::fabs(p.y) < 1e-4f;
		widest = std::max(widest, std::fabs(p.x - 10));
	}
	CHECK(inside);         // in the ellipse of emitRadius (y radius 0)
	CHECK(widest > 1.0f);  // spread over it, not at the centre
}

TEST(EffectsDetail_BoxAndRadialTypes)
{
	world::ObjectTemplateManager templates;
	world::ObjectTemplate* box = sprites(templates, "box", "Box");
	box->settings()["emitRangeX"] = "CRD_UNIFORM/1/2/0";
	box->settings()["emitRangeY"] = "CRD_NONE/3/0/0";
	box->settings()["emitRangeZ"] = "CRD_UNIFORM/-1/1/0";
	// Radial: a ring about y (emitRadius's longest axis) of the ranges' radius.
	world::ObjectTemplate* radial = sprites(templates, "radial", "RadialDirection");
	radial->settings()["emitRadius"] = "0/1/0";
	radial->settings()["emitRangeX"] = "CRD_NONE/2/0/0";
	radial->settings()["emitRangeY"] = "CRD_NONE/0/0/0";
	radial->settings()["emitRangeZ"] = "CRD_NONE/2/0/0";
	radial->settings()["randomRadialDirection"] = "360";
	radial->settings()["emitSpeed"] = "1";
	{
		effects::EffectManager manager(templates);
		manager.spawn("box", {});
		manager.update(0.1f);
		bool inBox = true;
		for (const auto& p : particlesOf(manager)) {
			inBox = inBox && p.x >= 1 && p.x <= 2 && std::fabs(p.y - 3) < 1e-4f && std::fabs(p.z) <= 1;
		}
		CHECK(!particlesOf(manager).empty() && inBox);
	}
	{
		effects::EffectManager manager(templates);
		manager.spawn("radial", {});
		manager.update(0.1f);
		bool onRing = true, outward = true;
		for (const auto& emitter : manager.emitterList()) {
			for (const auto& p : emitter->particles()) {
				const float r = std::hypot(p.position.x, p.position.z);
				onRing = onRing && r >= 1.999f && r < 2.15f && std::fabs(p.position.y) < 1e-3f;
				// RadialDirection: leaving along the ring's radius.
				outward = outward && p.velocity.x * p.position.x + p.velocity.z * p.position.z > 0;
			}
		}
		CHECK(!particlesOf(manager).empty() && onRing && outward);
	}
}

TEST(EffectsDetail_StaticSystemStartsWarm)
{
	world::ObjectTemplateManager templates;
	world::ObjectTemplate* plain = sprites(templates, "smoke", "Standard");
	plain->settings()["emitFrequency"] = "10";
	plain->settings()["isLooping"] = "1";
	plain->settings()["timeToLive"] = "3";
	world::ObjectTemplate* warm = sprites(templates, "standing_smoke", "Standard");
	warm->settings()["emitFrequency"] = "10";
	warm->settings()["isLooping"] = "1";
	warm->settings()["timeToLive"] = "3";
	warm->settings()["isStaticSystem"] = "1";
	effects::EffectManager a(templates), b(templates);
	a.setAnchor(1, {}, {});
	b.setAnchor(1, {}, {});
	a.spawnAttached("smoke", 1, {});
	b.spawnAttached("standing_smoke", 1, {});
	a.update(1 / 30.0f);
	b.update(1 / 30.0f);
	// Its life (3 s) run at once: about 30 particles alive at the start.
	CHECK(particlesOf(a).size() <= 1);
	CHECK(particlesOf(b).size() >= 25);
}

TEST(EffectsDetail_DebrisEmitterPieces)
{
	world::ObjectTemplateManager templates;
	world::ObjectTemplate* piece = templates.create("Particle", "p_piece");
	piece->setGeometry("wreck_parts");
	piece->settings()["geometryPart"] = "3";
	piece->settings()["timeToLive"] = "CRD_NONE/2/0/0";
	piece->settings()["sizeModifier"] = "1/2/0";
	piece->settings()["size"] = "CRD_NONE/1.5/0/0";
	piece->settings()["transparencyGraph"] = "0/0/-1/1"; // fading over its life
	world::ObjectTemplate* emitter = templates.create("Emitter", "em_parts");
	emitter->settings()["template"] = "p_piece";
	emitter->settings()["intensity"] = "CRD_NONE/10/0/0";
	emitter->settings()["timeToLive"] = "CRD_NONE/0.35/0/0";
	emitter->settings()["positionalSpeedInUp"] = "CRD_NONE/10/0/0";
	emitter->settings()["positionalSpeedInDof"] = "CRD_NONE/4/0/0";
	emitter->settings()["rotationalSpeedInDof"] = "CRD_NONE/3/0/0";
	world::ObjectTemplate* bundle = templates.create("EffectBundle", "e_wreck");
	bundle->children().push_back({"em_parts", {0, 1, 0}, {}});
	effects::EffectManager manager(templates);
	manager.setGround([](float, float) { return 0.0f; });
	CHECK(manager.spawn("e_wreck", {0, 0, 0}));
	CHECK(manager.debrisEmitters() == 1);
	for (int i = 0; i < 12; ++i) { // 0.4 s
		manager.update(1 / 30.0f);
	}
	// One every 0.1 s over 0.35 s: three, up and forward, spinning.
	CHECK(manager.debris().size() == 3);
	CHECK(manager.debrisEmitters() == 0);
	for (const auto& p : manager.debris()) {
		CHECK(p.look->geometry == "wreck_parts" && p.look->part == 3);
		CHECK(p.position.y > 1 && p.position.z > 0);
		const effects::Vec3 s = p.scale();
		CHECK(std::fabs(s.x - 1.5f) < 1e-5f && std::fabs(s.y - 3) < 1e-5f && s.z == 1); // the 0 axis keeps 1
		CHECK(p.alpha() < 1 && p.alpha() > 0.7f);
	}
	// They fall back, rest on the ground and go after their 2 s.
	for (int i = 0; i < 45; ++i) {
		manager.update(1 / 30.0f);
	}
	CHECK(manager.debris().size() == 3);
	bool landed = true;
	for (const auto& p : manager.debris()) {
		landed = landed && p.position.y >= 0 && p.position.y < 1.5f;
	}
	CHECK(landed);
	for (int i = 0; i < 20; ++i) {
		manager.update(1 / 30.0f);
	}
	CHECK(manager.debris().empty());
}

TEST(EffectsDetail_DebrisAlternatesAndBundles)
{
	world::ObjectTemplateManager templates;
	for (const char* name : {"p_a", "p_b"}) {
		world::ObjectTemplate* piece = templates.create("Particle", name);
		piece->setGeometry(name);
	}
	world::ObjectTemplate* sparks = sprites(templates, "sparks", "Standard");
	sparks->settings()["emitTime"] = "0.1";
	world::ObjectTemplate* emitter = templates.create("Emitter", "em_mixed");
	emitter->settings()["template"] = "p_a";
	emitter->settings()["alternateTemplate0"] = "p_b";
	emitter->settings()["alternateTemplate1"] = "sparks"; // not a Particle: started as a system
	emitter->settings()["alternateTemplate2"] = "";
	emitter->settings()["intensity"] = "CRD_NONE/100/0/0";
	emitter->settings()["timeToLive"] = "CRD_NONE/1/0/0";
	emitter->settings()["noPhysics"] = "1";
	effects::EffectManager manager(templates);
	manager.spawn("em_mixed", {});
	for (int i = 0; i < 30; ++i) {
		manager.update(1 / 30.0f);
	}
	int a = 0, b = 0;
	for (const auto& p : manager.debris()) {
		(p.look->geometry == "p_a" ? a : b) += 1;
		CHECK(p.resting); // noPhysics: no speed
	}
	// A third each (floor(u x 3): 0 p_b, 1 sparks, 2 the main p_a).
	CHECK(a > 20 && b > 20 && manager.emitters() > 20);
}

TEST(EffectsDetail_HitReactionNames)
{
	using game::GameWorld;
	const physics::Vec3 north {0, 0, 1};
	// The soldier facing north (yaw 0): a round flying north hits its back.
	CHECK(std::string(GameWorld::hitReaction(0x2f, north, 0)) == "hitBackHead");
	CHECK(std::string(GameWorld::hitReaction(0x2f, north * -1.0f, 0)) == "hitFrontHead");
	CHECK(std::string(GameWorld::hitReaction(0xf, north * -1.0f, 0)) == "hitFrontLeft");
	CHECK(std::string(GameWorld::hitReaction(0x11, north, 0)) == "hitBackLeft");
	CHECK(std::string(GameWorld::hitReaction(0x21, north * -1.0f, 0)) == "hitFrontRight");
	CHECK(std::string(GameWorld::hitReaction(3, north, 180)) == "hitFrontBody"); // turned round
	CHECK(std::string(GameWorld::hitReaction(-1, north, 0)) == "hitBackBody");
}

namespace {

physics::CollisionMeshFile debrisPanel(std::vector<std::array<float, 3>> vertices, std::vector<physics::CollisionFace> faces)
{
	physics::CollisionSubGeometry part;
	for (uint32_t type = 0; type < 3; ++type) {
		physics::CollisionLod lod;
		lod.collisionType = type;
		lod.boundsMin = {1e9f, 1e9f, 1e9f};
		lod.boundsMax = {-1e9f, -1e9f, -1e9f};
		for (const auto& v : vertices) {
			for (size_t k = 0; k < 3; ++k) {
				lod.boundsMin[k] = std::min(lod.boundsMin[k], v[k]);
				lod.boundsMax[k] = std::max(lod.boundsMax[k], v[k]);
			}
		}
		lod.vertices = vertices;
		lod.faces = faces;
		part.lods.push_back(lod);
	}
	physics::CollisionGeometry geometry;
	geometry.subGeometries = {part};
	physics::CollisionMeshFile mesh;
	mesh.version = 10;
	mesh.geometries = {geometry};
	return mesh;
}

} // namespace

// The pieces' own bodies (as the exe's physics nodes): thrown at a wall
// they stop at it and fall, land on the terrain on their mesh's corners
// (not on their centre) and go to sleep.
TEST(EffectsDetail_DebrisBodiesMeetTheWorld)
{
	world::ObjectTemplateManager templates;
	world::ObjectTemplate* piece = templates.create("Particle", "p_chunk");
	piece->setGeometry("chunk");
	piece->settings()["collisionMesh"] = "chunk_mesh";
	piece->settings()["timeToLive"] = "CRD_NONE/10/0/0";
	world::ObjectTemplate* emitter = templates.create("Emitter", "em_chunks");
	emitter->settings()["template"] = "p_chunk";
	emitter->settings()["intensity"] = "CRD_NONE/10/0/0";
	emitter->settings()["timeToLive"] = "CRD_NONE/0.25/0/0";
	emitter->settings()["positionalSpeedInDof"] = "CRD_NONE/12/0/0";
	emitter->settings()["positionalSpeedInUp"] = "CRD_NONE/2/0/0";
	// A cube of 0.4 m, and a wall across z = 4 (faced both ways).
	const float h = 0.2f;
	const physics::CollisionMeshFile chunk = debrisPanel(
		{{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h}, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}},
		{{{0, 2, 1}, 0}, {{0, 3, 2}, 0}, {{4, 5, 6}, 0}, {{4, 6, 7}, 0}});
	const physics::CollisionMeshFile wall = debrisPanel({{-20, -1, 0}, {20, -1, 0}, {20, 20, 0}, {-20, 20, 0}},
		{{{0, 1, 2}, 0}, {{0, 2, 3}, 0}, {{0, 2, 1}, 0}, {{0, 3, 2}, 0}});
	physics::CollisionWorld statics;
	statics.addMesh(wall, physics::CollisionUser::Soldier, {}, {0, 0, 4});
	effects::EffectManager manager(templates);
	effects::EffectManager::DebrisWorld world;
	world.ground.height = [](float, float) { return 0.0f; };
	world.ground.heightAndNormal = [](float, float, physics::Vec3& n) {
		n = {0, 1, 0};
		return 0.0f;
	};
	world.statics = &statics;
	world.collisionMesh = [&chunk](const std::string& name) { return name == "chunk_mesh" ? &chunk : nullptr; };
	manager.setDebrisWorld(world);
	CHECK(manager.spawn("em_chunks", {0, 1, 0}));
	for (int i = 0; i < 30 * 5; ++i) {
		manager.update(1 / 30.0f);
	}
	CHECK(manager.debris().size() == 2);
	for (const auto& p : manager.debris()) {
		CHECK(p.look->points.size() == 8);
		CHECK(p.position.z < 4); // the wall held them
		CHECK(p.position.y > 0.1f && p.position.y < 0.5f); // on their corners
		CHECK(p.resting); // asleep after a second still
	}
}

// The bundle's start age (modifier 4, 0x00618d00 -> the systems' slot 0x80:
// a particle is born that old, 0x1013fdc0), set each tick from the motion
// of an anchored bundle as the armour's damage smoke has it
// (StartAgeFromSpeed 0/12: 0 standing, 1 s at 12 m/s and over).
TEST(EffectsDetail_StartAgeFollowsTheAnchorsSpeed)
{
	world::ObjectTemplateManager templates;
	templates.registerType(1, "EffectBundle");
	templates.registerType(2, "SpriteParticleSystem");
	sprites(templates, "smoke", "Standard")->settings()["emitTime"] = "0"; // looping below
	templates.find("smoke")->settings()["isLooping"] = "1";
	world::ObjectTemplate* bundle = templates.create("EffectBundle", "e_dstate_smoke");
	bundle->children().push_back({"smoke", {}, {}});
	bundle->settings()["startAgeRange"] = "0/1";
	bundle->settings()["startAgeFromSpeed"] = "0/12";
	// The last particle born (they are kept in birth order).
	const auto newest = [](const effects::EffectManager& manager) {
		const auto& particles = manager.emitterList().front()->particles();
		return particles.empty() ? -1.0f : particles.back().age;
	};
	for (const bool driven : {true, false}) {
		effects::EffectManager manager(templates);
		manager.setAnchor(1, {}, {});
		CHECK(manager.spawnAttached("e_dstate_smoke", 1, {}, {}, effects::EffectManager::View::Any, 9, driven));
		for (int i = 0; i < 10; ++i) {
			manager.update(1 / 30.0f);
		}
		CHECK(manager.emitterList().front()->startAge == 0);
		CHECK(newest(manager) < 0.05f);
		// Driving at 15 m/s: the start age reaches its maximum, 1 s.
		for (int i = 0; i < 10; ++i) {
			manager.setAnchor(1, {0, 0, 0.5f * static_cast<float>(i + 1)}, {});
			manager.update(1 / 30.0f);
		}
		const float startAge = manager.emitterList().front()->startAge;
		if (driven) {
			CHECK(std::fabs(startAge - 1) < 1e-3f);
			CHECK(newest(manager) >= 1.0f);
		} else {
			CHECK(startAge == 0); // ObjectEffects drives its own bundles
		}
	}
}

// RendDX9's emissions stop for a system not drawn for 60 frames
// (0x101403b0: the draw's frame +0x124 against the renderer's count) and
// make up the missed time when it is seen again; nothing holds while no
// frame is drawn at all.
TEST(EffectsDetail_UnseenSystemsHoldTheirEmission)
{
	world::ObjectTemplateManager templates;
	templates.registerType(2, "SpriteParticleSystem");
	world::ObjectTemplate* smoke = sprites(templates, "smoke", "Standard");
	smoke->settings()["emitFrequency"] = "30";
	smoke->settings()["emitTime"] = "100";
	smoke->settings()["timeToLive"] = "2";
	effects::EffectManager manager(templates);
	CHECK(manager.spawn("smoke", {}));
	const auto count = [&manager] { return particlesOf(manager).size(); };
	for (int i = 0; i < 30; ++i) {
		manager.update(1 / 30.0f); // no frame drawn: it emits
	}
	CHECK(count() >= 28 && count() <= 31);
	const auto hidden = [](const effects::Vec3&, float) { return false; };
	const auto shown = [](const effects::Vec3&, float) { return true; };
	for (int i = 0; i < 60; ++i) {
		manager.markViewed(hidden);
		manager.update(1 / 30.0f);
	}
	const size_t atSixty = count(); // still emitting up to the 60th frame
	CHECK(atSixty >= 55);
	for (int i = 0; i < 90; ++i) {
		manager.markViewed(hidden);
		manager.update(1 / 30.0f);
	}
	CHECK(count() == 0); // three seconds unseen: the last ones have died
	CHECK(manager.emitters() == 1);
	// Seen again: the last two seconds' worth comes at once.
	manager.markViewed(shown);
	manager.update(1 / 30.0f);
	CHECK(count() >= 55 && count() <= 62);
	CHECK(manager.framesViewed() == 151);
}

// forceToWaterSurface on a non-screen-aligned system: its frame stood
// upright (0x1013c800 -> 0x100989f0), its rings level whatever the hit's
// frame; others keep theirs.
TEST(EffectsDetail_WaterRingsStandUpright)
{
	world::ObjectTemplateManager templates;
	templates.registerType(3, "NonScreenAlignedParticleSystem");
	for (const char* name : {"rings", "tilted"}) {
		world::ObjectTemplate* t = templates.create("NonScreenAlignedParticleSystem", name);
		t->settings()["emitFrequency"] = "30";
		t->settings()["timeToLive"] = "1";
		t->settings()["textureName"] = "ring";
	}
	templates.find("rings")->settings()["forceToWaterSurface"] = "1";
	effects::EffectManager manager(templates);
	const physics::Mat3 rolled = physics::Mat3::fromYawPitchRoll({0, 0, 30});
	CHECK(manager.spawn("rings", {0, 1, 0}, rolled));
	CHECK(manager.spawn("tilted", {0, 1, 0}, rolled));
	const physics::Mat3& upright = manager.emitterList()[0]->rotation();
	CHECK(std::fabs(upright.rows[0].x + 1) < 1e-4f); // z x up: mirrored as the binary
	CHECK(std::fabs(upright.rows[1].y - 1) < 1e-4f);
	CHECK(std::fabs(upright.rows[2].z - 1) < 1e-4f);
	CHECK(std::fabs(manager.emitterList()[1]->rotation().rows[1].y - 1) > 0.1f);
	// Straight up (|z . up| > 0.9999): the identity.
	CHECK(manager.spawn("rings", {0, 1, 0}, physics::Mat3::fromYawPitchRoll({0, 90, 0})));
	const physics::Mat3& level = manager.emitterList()[2]->rotation();
	CHECK(level.rows[0].x == 1 && level.rows[1].y == 1 && level.rows[2].z == 1);
}

// isStatic decals go to their set's second list (0x10166410 -> 0x10164710):
// numbered 1..255 per set, never aged, taken out by id (slot 5, 0x10164e70).
TEST(EffectsDetail_StaticDecalsSecondList)
{
	world::ObjectTemplateManager templates;
	templates.registerType(5, "Decal");
	world::ObjectTemplate* mark = templates.create("Decal", "mark");
	mark->settings()["decalTextureName"] = "mark";
	mark->settings()["isStatic"] = "1";
	world::ObjectTemplate* hole = templates.create("Decal", "hole");
	hole->settings()["decalTextureName"] = "hole";
	effects::EffectManager manager(templates);
	CHECK(manager.spawnDecal("mark", {0, 0, 0}, {0, 1, 0}) && manager.lastDecalId() == 1);
	CHECK(manager.spawnDecal("mark", {1, 0, 0}, {0, 1, 0}) && manager.lastDecalId() == 2);
	CHECK(manager.spawnDecal("hole", {2, 0, 0}, {0, 1, 0}) && manager.lastDecalId() == 0);
	// An object's set counts on its own.
	CHECK(manager.spawnDecal("mark", {11, 1, 0}, {1, 0, 0}, 42, {10, 0, 0}, {0, 0, 0}) && manager.lastDecalId() == 1);
	for (int i = 0; i < 30; ++i) {
		manager.update(1.0f); // past DecalManager.timeToLive (20 s)
	}
	CHECK(manager.decals().size() == 3); // the hole went, the static ones stay
	CHECK(manager.removeStaticDecal(0, 1));
	CHECK(!manager.removeStaticDecal(0, 1));
	CHECK(manager.decals().size() == 2);
	// After 255 the ids start again at 1.
	for (int i = 0; i < 253; ++i) {
		manager.spawnDecal("mark", {0, 0, 0}, {0, 1, 0});
	}
	CHECK(manager.lastDecalId() == 255);
	manager.spawnDecal("mark", {0, 0, 0}, {0, 1, 0});
	CHECK(manager.lastDecalId() == 1);
}

// GroundEffectBundle (0x0061e090 -> 0x0061de40): the weapon's fire (event 0
// to its children) starts its muzzle bundle and, within maxTriggerDistance
// of the ground under the bundle, the ground's material effect there; an
// EffectBundle's start sets off the ground bundles under it (0x0061a1c0).
TEST(EffectsDetail_GroundBundlesUnderGunsAndFeet)
{
	world::ObjectTemplateManager templates;
	sprites(templates, "dust", "Standard");
	sprites(templates, "flash", "Standard");
	world::ObjectTemplate* dust = templates.create("EffectBundle", "e_geb_dirt");
	dust->children().push_back({"dust", {}, {}});
	world::ObjectTemplate* muzzle = templates.create("EffectBundle", "e_muzz_gun");
	muzzle->children().push_back({"flash", {}, {}});
	world::ObjectTemplate* geb = templates.create("GroundEffectBundle", "geb_tank");
	geb->settings()["material"] = "66";
	geb->settings()["maxTriggerDistance"] = "3";
	world::ObjectTemplate* gun = templates.create("GenericFireArm", "tank_Barrel");
	gun->children().push_back({"e_muzz_gun", {0, 0, 3}, {}});
	gun->children().push_back({"Geb_tank", {0, 0, 2.5f}, {}});
	world::ObjectTemplate* foot = templates.create("EffectBundle", "mech_FootStep");
	foot->settings()["setStartOnEffects"] = "512"; // effect:0 of the walker's animation
	foot->children().push_back({"flash", {}, {}});
	foot->children().push_back({"geb_tank", {}, {}});
	world::ObjectTemplate* tank = templates.create("PlayerControlObject", "tank");
	tank->children().push_back({"tank_Barrel", {0, 2, 0}, {}});
	tank->children().push_back({"mech_FootStep", {1, 0, 0}, {}});

	const auto placed = effects::ObjectEffects::bundlesOf(templates, *tank);
	CHECK(placed.size() == 4);
	if (!(placed.size() == 4)) {
		return;
	}
	CHECK(placed[1].kind == effects::ObjectEffects::Placed::Kind::GroundTrigger && placed[1].parent == -1);
	CHECK(placed[1].material == 66 && placed[1].maxTriggerDistance == 3);
	CHECK(placed[3].kind == effects::ObjectEffects::Placed::Kind::GroundTrigger && placed[3].parent == 2);

	effects::EffectManager manager(templates);
	effects::ObjectEffects objects(templates);
	std::vector<std::pair<effects::Vec3, effects::Vec3>> rays;
	objects.setGround(
		[&rays](const effects::Vec3& from, const effects::Vec3& to, uint32_t, effects::Vec3& hit, uint32_t& material) {
			rays.push_back({from, to});
			if (to.y > 0) {
				return false;
			}
			hit = {from.x, 0, from.z}; // flat ground at y 0, material 8
			material = 8;
			return true;
		},
		[](uint32_t bundle, uint32_t ground) { return bundle == 66 && ground == 8 ? std::string("e_geb_dirt") : std::string(); });
	const auto worldEmitters = [&manager](float x, float z) {
		int count = 0;
		for (const auto& emitter : manager.emitterList()) {
			const effects::Vec3 p = emitter->position();
			if (emitter->anchor == 0 && std::fabs(p.x - x) < 1e-3f && std::fabs(p.y) < 1e-3f && std::fabs(p.z - z) < 1e-3f) {
				++count;
			}
		}
		return count;
	};
	effects::ObjectEffects::ObjectState state;
	state.id = 5;
	state.objectTemplate = tank;
	state.position = {0, 0.5f, 0};
	objects.update({state}, manager);
	CHECK(rays.empty() && manager.emitters() == 0);
	// A round: the muzzle bundle (startOnEffects 0) and the dust under the
	// barrel, 2.5 m over the ground.
	state.firedWeapons = {"TANK_BARREL"};
	objects.update({state}, manager);
	CHECK(manager.emitting(5, 1));
	CHECK(rays.size() == 1);
	if (!(rays.size() == 1)) {
		return;
	}
	CHECK(std::fabs(rays[0].first.y - 2.5f) < 1e-4f && std::fabs(rays[0].second.y + 0.5f) < 1e-4f);
	CHECK(worldEmitters(0, 2.5f) == 1);
	// Again: the muzzle restarts each round (minDistanceUnderwaterSurface
	// below 0), the dust too.
	objects.update({state}, manager);
	CHECK(worldEmitters(0, 2.5f) == 2);
	// Lifted 5 m: the ground is beyond the 3 m.
	state.position = {0, 5, 0};
	objects.update({state}, manager);
	CHECK(rays.size() == 3 && worldEmitters(0, 2.5f) == 2);
	// A foot step: its bundle starts and stamps the ground under it.
	state.firedWeapons.clear();
	state.position = {0, 1, 0};
	state.effects = 512;
	objects.update({state}, manager);
	CHECK(manager.emitting(5, 3));
	CHECK(worldEmitters(1, 0) == 1);
}

// The commander's EMP strike (commander_EMP_strike_GenericFireArm, moved
// over the target by 0x005a0bf0): its fire's event 0 reaches
// GEB_emp_strike there (material 28, 2000 m), whose ray down finds the
// ground under the target; the firearm's pitch 90 in the asset and the
// GEB's -90 under it leave the GEB upright, so it looks straight down.
TEST(EffectsDetail_EmpStrikeGroundBundle)
{
	world::ObjectTemplateManager templates;
	sprites(templates, "rings", "Standard");
	world::ObjectTemplate* dirt = templates.create("EffectBundle", "e_geb_emp_strike_dirt");
	dirt->children().push_back({"rings", {}, {}});
	world::ObjectTemplate* geb = templates.create("GroundEffectBundle", "GEB_emp_strike");
	geb->settings()["material"] = "28";
	geb->settings()["maxTriggerDistance"] = "2000";
	world::ObjectTemplate* gun = templates.create("GenericFireArm", "commander_EMP_strike_GenericFireArm");
	gun->children().push_back({"GEB_emp_strike", {}, {0, -90, 0}});
	world::ObjectTemplate* asset = templates.create("PlayerControlObject", "commander_emp_strike");
	asset->children().push_back({"commander_EMP_strike_GenericFireArm", {}, {0, 90, 0}});

	effects::EffectManager manager(templates);
	effects::ObjectEffects objects(templates);
	std::vector<std::pair<effects::Vec3, effects::Vec3>> rays;
	objects.setGround(
		[&rays](const effects::Vec3& from, const effects::Vec3& to, uint32_t, effects::Vec3& hit, uint32_t& material) {
			rays.push_back({from, to});
			if (to.y > 10) {
				return false;
			}
			hit = {from.x, 10, from.z}; // ground at y 10, material 3
			material = 3;
			return true;
		},
		[](uint32_t bundle, uint32_t ground) {
			return bundle == 28 && ground == 3 ? std::string("e_geb_emp_strike_dirt") : std::string();
		});
	// The firearm's world turn: its child rotation in the asset, the asset yawed.
	const effects::Mat3 assetTurn = physics::Mat3::fromYawPitchRoll({30, 0, 0});
	const effects::Mat3 turn = physics::Mat3::fromYawPitchRoll({0, 90, 0}) * assetTurn;
	objects.weaponFiredAt(*gun, {100, 510, -40}, turn, manager);
	CHECK(rays.size() == 1);
	if (!(rays.size() == 1)) {
		return;
	}
	CHECK(std::fabs(rays[0].first.x - 100) < 1e-3f && std::fabs(rays[0].first.y - 510) < 1e-3f);
	CHECK(std::fabs(rays[0].second.x - 100) < 1e-2f && std::fabs(rays[0].second.z + 40) < 1e-2f);
	CHECK(std::fabs(rays[0].second.y - (510 - 2000)) < 1e-2f);
	int found = 0;
	for (const auto& emitter : manager.emitterList()) {
		const effects::Vec3 p = emitter->position();
		found += std::fabs(p.x - 100) < 1e-2f && std::fabs(p.y - 10) < 1e-2f && std::fabs(p.z + 40) < 1e-2f;
	}
	CHECK(found == 1);
	// The other strike firearms carry no bundle: nothing happens.
	world::ObjectTemplate* orbital = templates.create("GenericFireArm", "commander_orbital_strike_GenericFireArm");
	objects.weaponFiredAt(*orbital, {0, 500, 0}, turn, manager);
	CHECK(rays.size() == 1);
}

// The bundle's sound intensity (modifier 5, +0x1d8): its looping start
// sound is held with the bundle (bundleLoops) with its envelopes read at the
// intensity; a one-shot start sound still plays once (takeSounds).
TEST(EffectsDetail_BundleSoundLoopsAtTheirIntensity)
{
	world::ObjectTemplateManager templates;
	world::ObjectTemplate* loop = templates.create("Sound", "S_e_wake_sound_Start");
	loop->settings()["loopCount"] = "0";
	templates.create("Sound", "S_e_click_Start");
	world::ObjectTemplate* wake = templates.create("EffectBundle", "e_wake_sound");
	wake->settings()["SoundIntensityRange"] = "0/1";
	wake->settings()["SoundIntensityUsed"] = "0";
	wake->settings()["SoundIntensityFromSpeed"] = "0/5";
	wake->settings()["setStartOnEffects"] = "32";
	wake->settings()["setStopOnEffects"] = "64";
	wake->children().push_back({"S_e_wake_sound_Start", {0, 0, 1}, {}});
	world::ObjectTemplate* click = templates.create("EffectBundle", "e_click");
	click->children().push_back({"S_e_click_Start", {}, {}});
	world::ObjectTemplate* apc = templates.create("PlayerControlObject", "apc");
	apc->children().push_back({"e_wake_sound", {}, {}});

	effects::EffectManager manager(templates);
	effects::ObjectEffects objects(templates);
	effects::ObjectEffects::ObjectState state;
	state.id = 4;
	state.objectTemplate = apc;
	state.position = {10, 0, 0};
	objects.update({state}, manager);
	CHECK(manager.bundleLoops().empty());
	state.occupied = true; // event 4: the bundle starts
	objects.update({state}, manager);
	CHECK(manager.takeSounds().empty());
	CHECK(manager.bundleLoops().size() == 1);
	if (!(manager.bundleLoops().size() == 1)) {
		return;
	}
	CHECK(manager.emitting(4, 1)); // a sound-only bundle runs while its loop does
	const auto first = manager.bundleLoops()[0];
	CHECK(first.name == "S_e_wake_sound_Start" && first.intensity == 0);
	CHECK(std::fabs(first.position.x - 10) < 1e-4f && std::fabs(first.position.z - 1) < 1e-4f);
	// Moving at 2.5 m/s (1/12 m a tick): half way along 0..5.
	state.position = {10, 0, 1.0f / 12.0f};
	objects.update({state}, manager);
	CHECK(manager.bundleLoops().size() == 1);
	if (!(manager.bundleLoops().size() == 1)) {
		return;
	}
	CHECK(manager.bundleLoops()[0].key == first.key);
	CHECK(std::fabs(manager.bundleLoops()[0].intensity - 0.5f) < 1e-3f);
	// Event 5 (the player left): stopped.
	state.occupied = false;
	objects.update({state}, manager);
	CHECK(manager.bundleLoops().empty() && !manager.emitting(4, 1));

	// A one-shot start sound plays once.
	manager.setAnchor(9, {}, {});
	manager.spawnAttached("e_click", 9, {});
	CHECK(manager.bundleLoops().empty() && manager.takeSounds().size() == 1);
	// A driven bundle on a moving anchor (stepAttachedBundles) sets it too.
	manager.spawnAttached("e_wake_sound", 9, {});
	CHECK(manager.bundleLoops().size() == 1);
	if (!(manager.bundleLoops().size() == 1)) {
		return;
	}
	manager.update(0.1f);
	manager.setAnchor(9, {0, 0, 0.5f}, {});
	manager.update(0.1f); // 5 m/s
	CHECK(std::fabs(manager.bundleLoops()[0].intensity - 1) < 1e-3f);
	manager.releaseAnchor(9);
	CHECK(manager.bundleLoops().empty());
}
