// A round's own look (client::RoundVisuals): its mesh's place and frame,
// its bundles started and stopped by the engine's events 0 and 1
// (0x005b2c10, 0x005b2ca0 -> EffectBundle 0x0061a4e0).
#include "Effects/Effects.h"
#include "Game/Weapons.h"
#include "Main/RoundVisuals.h"
#include "TestMain.h"
#include "World/ObjectTemplateManager.h"

#include <cmath>

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
