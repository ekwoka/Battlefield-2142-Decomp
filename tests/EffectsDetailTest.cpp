// Particle and debris emitter detail: emitterType and emitRadius,
// isStaticSystem, the debris Emitter and its Particle pieces; the 3P hit
// reactions' names.
#include "Effects/Effects.h"
#include "Game/GameWorld.h"
#include "TestMain.h"
#include "World/ObjectTemplateManager.h"

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
