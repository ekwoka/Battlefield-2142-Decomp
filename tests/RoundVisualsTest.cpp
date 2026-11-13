// A round's own look (client::RoundVisuals): its mesh's place and frame,
// its bundles started and stopped by the engine's events 0 and 1
// (0x005b2c10, 0x005b2ca0 -> EffectBundle 0x0061a4e0).
#include "Effects/Effects.h"
#include "Game/Weapons.h"
#include "Main/RoundVisuals.h"
#include "TestMain.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace dice::hfe;

namespace {

world::ObjectTemplate* trailSystem(world::ObjectTemplateManager& templates, const char* name)
{
	world::ObjectTemplate* t = templates.create("SpriteParticleSystem", name);
	t->settings()["emitFrequency"] = "100";
	t->settings()["emitTime"] = "5";
	t->settings()["timeToLive"] = "1";
	t->settings()["textureName"] = "smoke";
	return t;
}

bool near(const physics::Vec3& a, const physics::Vec3& b, float e = 1e-4f)
{
	return std::fabs(a.x - b.x) < e && std::fabs(a.y - b.y) < e && std::fabs(a.z - b.z) < e;
}

} // namespace

// The frame a round is drawn with: along its nose in flight (the heading
// the follow keeps, else the velocity), its facing lying, the face's normal
// as its up once stuck (0x005e83d0).
TEST(RoundVisuals_FrameFollowsNoseRestAndStick)
{
	game::ProjectileSettings settings;
	game::Projectile round;
	round.settings = &settings;
	round.velocity = {0, 0, 10};
	physics::Mat3 frame = client::roundFrame(round);
	CHECK(near(frame.rows[2], {0, 0, 1}) && near(frame.rows[1], {0, 1, 0}));
	// Climbing at 45 degrees towards +x.
	round.velocity = {10, 10, 0};
	frame = client::roundFrame(round);
	const float h = std::sqrt(0.5f);
	CHECK(near(frame.rows[2], {h, h, 0}, 1e-3f));
	CHECK(std::fabs(frame.rows[1].y - h) < 1e-3f); // unrolled: its up leans back
	// A heading wins over the velocity (a round launched at rest).
	round.heading = {-1, 0, 0};
	round.velocity = {};
	frame = client::roundFrame(round);
	CHECK(near(frame.rows[2], {-1, 0, 0}, 1e-3f));
	// Lying: its facing (yaw 90: +x), upright, whatever the heading.
	round.resting = true;
	round.yaw = 90;
	frame = client::roundFrame(round);
	CHECK(near(frame.rows[2], {1, 0, 0}, 1e-3f) && near(frame.rows[1], {0, 1, 0}, 1e-3f));
	// Stuck on a wall facing -x: up along the normal, x kept.
	round.stuck = true;
	round.stuckNormal = {-1, 0, 0};
	frame = client::roundFrame(round);
	CHECK(near(frame.rows[1], {-1, 0, 0}, 1e-3f) && near(frame.rows[0], {0, 0, -1}, 1e-3f));
}

// A thrown RotationalPoint round (0x005e5720, 0x006c6860, 0x006c0d00): its
// frame is the throw's turned by projectileStartRotation (the grenades'
// 0/0/45: rolled 45 degrees); randomRotationSpeed spins it about the world's
// y and z by whole radians a second times the power, seeded by its id; its
// node turns it each tick without damping; a bounce reverses the spin and
// scales it by the new speed's square (stopping it under 0.1); it is drawn
// with its own frame in flight and lying.
TEST(RoundVisuals_ThrownRoundTumbles)
{
	game::ProjectileSettings settings;
	settings.rotationalPoint = true;
	game::Projectile frag;
	frag.settings = &settings;
	frag.velocity = {0, 0, 15};
	game::throwFrame(frag, {0, 0, 1}, {0, 0, 45}, 0, 1, 7);
	const float h = std::sqrt(0.5f);
	CHECK(frag.turns && near(frag.spin, {}));
	CHECK(near(frag.rotation.rows[2], {0, 0, 1}, 1e-4f) && std::fabs(std::fabs(frag.rotation.rows[0].y) - h) < 1e-4f);
	const physics::Mat3 thrown = frag.rotation;
	game::moveProjectile(frag, 1 / 30.0f, -14.73f);
	CHECK(near(frag.rotation.rows[0], thrown.rows[0]) && near(client::roundFrame(frag).rows[1], thrown.rows[1]));
	// The smoke grenade's spin 4 at full power: (0, -n1, n2), n in 1..4.
	game::Projectile smoke;
	smoke.settings = &settings;
	smoke.velocity = {0, 0, 15};
	game::throwFrame(smoke, {0, 0, 1}, {0, 0, 45}, 4, 1, 7);
	std::fprintf(stderr, "  smoke grenade spin (%.0f, %.0f, %.0f)\n", smoke.spin.x, smoke.spin.y, smoke.spin.z);
	CHECK(smoke.spin.x == 0 && smoke.spin.y <= -1 && smoke.spin.y >= -4 && smoke.spin.z >= 1 && smoke.spin.z <= 4);
	game::Projectile half = smoke;
	game::throwFrame(half, {0, 0, 1}, {0, 0, 45}, 4, 0.5f, 7);
	CHECK(near(half.spin, smoke.spin * 0.5f));
	const physics::Mat3 before = smoke.rotation;
	const physics::Vec3 spin = smoke.spin;
	for (int i = 0; i < 30; ++i) {
		game::moveProjectile(smoke, 1 / 30.0f, -14.73f);
	}
	// A second's turn by |spin| radians about the spin's axis.
	const float angle = std::acos(std::clamp(physics::dot(before.rows[2], smoke.rotation.rows[2]), -1.0f, 1.0f));
	CHECK(near(smoke.spin, spin) && angle > 0.5f);
	const physics::Mat3 drawn = client::roundFrame(smoke);
	CHECK(near(drawn.rows[0], smoke.rotation.rows[0]) && near(drawn.rows[2], smoke.rotation.rows[2]));
	// Bounces.
	CHECK(near(game::bounceSpin({0, -2, 3}, {0, 0, 4}), {0, 2, -3}));
	CHECK(near(game::bounceSpin({0, -2, 3}, {0, 0.5f, 0.5f}), {0, 1, -1.5f}));
	CHECK(near(game::bounceSpin({0, -2, 3}, {0, 0.3f, 0}), {}));
	// Lying, it keeps the frame it came to rest with.
	smoke.resting = true;
	CHECK(near(client::roundFrame(smoke).rows[1], smoke.rotation.rows[1]));
}

// The engine: on from the launch after startDelay; with targetActivation
// only while a target is held (0x005b3990, 0x005b2c10, 0x005b2ca0).
TEST(RoundVisuals_EngineEvents)
{
	game::ProjectileSettings settings;
	settings.startDelay = 0.2f;
	game::Projectile round;
	round.settings = &settings;
	round.age = 0.1f;
	CHECK(!client::roundEngineOn(round));
	round.age = 0.2f;
	CHECK(client::roundEngineOn(round));
	settings.targetActivation = true;
	CHECK(!client::roundEngineOn(round));
	round.lockedOn = 7;
	CHECK(client::roundEngineOn(round));
}

// A missile's tree: its trail with startAtCreation starts with it, a
// flame bundle (startOnEffects 0) at the engine's start (event 0), stopped
// by event 1 when the target is lost; the bullet without a geometry draws
// nothing, the rocket its geometry; a server ghost's round is left to its
// object. The round's end lets its bundles go where they are.
TEST(RoundVisuals_BundlesAndMeshes)
{
	world::ObjectTemplateManager templates;
	trailSystem(templates, "smoke");
	trailSystem(templates, "flame");
	world::ObjectTemplate* trail = templates.create("EffectBundle", "e_trail");
	trail->settings()["startAtCreation"] = "1";
	trail->settings()["startOnEffects"] = "1"; // not on event 0
	trail->settings()["stopOnEffects"] = "1";  // nor stopped by event 1
	trail->children().push_back({"smoke", {}, {}});
	world::ObjectTemplate* flame = templates.create("EffectBundle", "e_flame");
	flame->children().push_back({"flame", {}, {}});
	world::ObjectTemplate* missile = templates.create("GenericProjectile", "test_missile");
	missile->setGeometry("test_missile");
	missile->children().push_back({"S_test_missile_Looping", {}, {}});
	missile->children().push_back({"e_trail", {0, 0, -0.2f}, {}});
	missile->children().push_back({"e_flame", {}, {}});
	templates.create("GenericProjectile", "test_bullet");

	game::ProjectileSettings missileSettings, bulletSettings;
	missileSettings.name = "test_missile";
	missileSettings.targetActivation = true;
	bulletSettings.name = "test_bullet";
	game::Projectile rocket;
	rocket.settings = &missileSettings;
	rocket.serial = 3;
	rocket.position = {1, 2, 3};
	rocket.velocity = {0, 0, 40};
	game::Projectile bullet;
	bullet.settings = &bulletSettings;
	bullet.serial = 4;
	bullet.velocity = {0, 0, 400};
	game::Projectile ghosted = rocket;
	ghosted.serial = 5;
	ghosted.ghostObject = 77;
	std::vector<game::Projectile> rounds {rocket, bullet, ghosted};

	client::RoundVisuals visuals(templates);
	std::vector<client::RoundVisuals::Mesh> meshes;
	visuals.meshes(rounds, meshes);
	CHECK(meshes.size() == 1);
	if (meshes.size() == 1) {
		CHECK(meshes[0].objectTemplate == missile);
		CHECK(near(meshes[0].position, {1, 2, 3}) && near(meshes[0].rotation.rows[2], {0, 0, 1}));
	}
	// Seen through its own camera, the rocket isn't drawn (from inside its
	// nose the original culls every face of it).
	meshes.clear();
	visuals.meshes(rounds, meshes, rocket.serial);
	CHECK(meshes.empty());

	effects::EffectManager manager(templates);
	effects::ObjectEffects objects(templates);
	const uint32_t anchor = client::kRoundAnchor | 3;
	const auto tick = [&] {
		std::vector<effects::ObjectEffects::ObjectState> states;
		visuals.appendEffectStates(rounds, states);
		objects.update(states, manager);
		manager.update(1.0f / 30.0f);
		return states.size();
	};
	CHECK(tick() == 1); // only the rocket: the bullet has no bundles, the ghost its object
	CHECK(manager.emitting(anchor, 1));  // the trail, at creation
	CHECK(!manager.emitting(anchor, 2)); // the flame waits for the engine
	rounds[0].lockedOn = 9;              // a target: event 0
	tick();
	CHECK(manager.emitting(anchor, 1) && manager.emitting(anchor, 2));
	rounds[0].lockedOn = 0;              // lost: event 1
	tick();
	CHECK(manager.emitting(anchor, 1) && !manager.emitting(anchor, 2));
	rounds[0].lockedOn = 9;              // again
	tick();
	CHECK(manager.emitting(anchor, 2));
	rounds[0].alive = false;             // its end: the bundles let go
	tick();
	CHECK(!manager.emitting(anchor, 1) && !manager.emitting(anchor, 2));
}
