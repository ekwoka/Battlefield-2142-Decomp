// The cameras' geometry (src/Game/Cameras.h): the vehicle camera's view
// modes (0x00612eb0, 0x006108e0, 0x00610800, 0x00612bb0, 0x00610d20) and
// the player camera's death, follow and free views (0x00615970,
// 0x00614d00, 0x00615290).
#include "TestMain.h"

#include "Game/Cameras.h"

#include <cmath>

using namespace dice::hfe::game::camera;

namespace {

bool near(float a, float b, float e = 1e-3f) { return std::abs(a - b) <= e; }
bool near(const Vec3& a, const Vec3& b, float e = 1e-3f) { return near(a.x, b.x, e) && near(a.y, b.y, e) && near(a.z, b.z, e); }

Ground flat(float h)
{
	Ground g;
	g.terrain = [h](float, float) { return h; };
	return g;
}

} // namespace

// c_PIToggleCameraMode's walk (0x004b9e10) over what setMode takes: by
// default Inside, Chase, FrontChase, FlyBy; Nose with CVMNose and
// sv.allowNoseCam; none of the external ones without sv.allowExternalViews.
TEST(Camera_ModesCycleAsTheTemplateAllows)
{
	ModeRules rules;
	CHECK(nextMode(Mode::Inside, rules) == Mode::Chase);
	CHECK(nextMode(Mode::Chase, rules) == Mode::FrontChase);
	CHECK(nextMode(Mode::FrontChase, rules) == Mode::FlyBy);
	CHECK(nextMode(Mode::FlyBy, rules) == Mode::Inside); // 6, 7 refused, 0 is Inside
	// Backwards from Inside meets 0 first, which setMode takes as Inside.
	CHECK(nextMode(Mode::Inside, rules, true) == Mode::Inside);
	CHECK(nextMode(Mode::Chase, rules, true) == Mode::Inside);
	rules.nose = true;
	CHECK(nextMode(Mode::Inside, rules) == Mode::Nose);
	rules.allowNoseCam = false;
	CHECK(nextMode(Mode::Inside, rules) == Mode::Chase);
	rules.allowExternalViews = false;
	CHECK(nextMode(Mode::Inside, rules) == Mode::Inside);
	rules.trace = true; // Trace doesn't ask for the external views
	CHECK(nextMode(Mode::Inside, rules) == Mode::Trace);
	CHECK(!modeAllowed(Mode::ExternTrace, {}));
}

// Chase (0x006108e0): chaseDistance -1 is the root's radius x 2 (at least
// 2 x 2), behind and up by chaseAngle; FrontChase ahead; the eye a metre over
// the ground; the direction and distance ease by followStiffness x seconds.
TEST(Camera_ChaseSitsBehindAndEases)
{
	Frame root; // at the origin facing +z
	ChaseState state;
	ChaseSettings settings;
	const Look look = chase(state, settings, root, root.position, 3, false, 1.0f / 30, flat(-100));
	const float l = std::sqrt(1 + 0.09f);
	CHECK(near(look.eye, {0, 0.3f / l * 6, -1 / l * 6}));
	CHECK(near(look.at, {0, 0, 0}));
	ChaseState front;
	const Look ahead = chase(front, settings, root, root.position, 1, true, 1.0f / 30, flat(-100));
	CHECK(near(ahead.eye, {0, 0.3f / l * 4, 1 / l * 4})); // radius under 2: 2 x 2
	// The ground holds it up.
	ChaseState low;
	settings.angle = 0;
	const Look grounded = chase(low, settings, root, root.position, 3, false, 1.0f / 30, flat(5));
	CHECK(near(grounded.eye.y, 6));
	// Turned 90 degrees: a third of a second at stiffness 1 goes a share of
	// the way, not all of it.
	settings.angle = 0.3f;
	Frame turned;
	turned.rotation = Mat3::fromYawPitchRoll({90, 0, 0});
	const Look eased = chase(state, settings, turned, turned.position, 3, false, 1.0f / 3, flat(-100));
	CHECK(eased.eye.z < -0.5f && eased.eye.x < -0.5f);
	for (int i = 0; i < 300; ++i) {
		chase(state, settings, turned, turned.position, 3, false, 1.0f / 30, flat(-100));
	}
	const Look settled = chase(state, settings, turned, turned.position, 3, false, 1.0f / 30, flat(-100));
	CHECK(near(settled.eye, {-1 / l * 6, 0.3f / l * 6, 0}, 1e-2f));
	// A fixed distance and chaseOffset in the parent's frame.
	ChaseState fixed;
	ChaseSettings tank;
	tank.distance = 12;
	tank.angle = 0.15f;
	tank.offset = {0, 3, 2.5f};
	const Look t = chase(fixed, tank, root, root.position, 3, false, 1.0f / 30, flat(-100));
	CHECK(near(t.at, {0, 3, 2.5f}));
	CHECK(near(std::sqrt((t.eye.x - t.at.x) * (t.eye.x - t.at.x) + (t.eye.y - t.at.y) * (t.eye.y - t.at.y) +
				   (t.eye.z - t.at.z) * (t.eye.z - t.at.z)),
		12));
}

// Nose (0x00610800): the seat camera moved by nosePos in its parent's turn.
TEST(Camera_NoseMovesTheSeatCamera)
{
	Frame camera;
	camera.position = {10, 2, 0};
	camera.rotation = Mat3::fromYawPitchRoll({90, 0, 0});
	const Look look = nose(camera, camera.rotation, {0, -1, 2});
	CHECK(near(look.eye, {12, 1, 0}));
	CHECK(near(look.at, {13, 1, 0}));
}

// FlyBy (0x00612bb0): a point ahead on the way, kept; the camera looks from
// it at the vehicle going by.
TEST(Camera_FlyByWaitsAhead)
{
	FlyByState state;
	Frame root;
	const Look first = flyBy(state, {0, 0, 0}, root, {0, 0, 10}, 2, flat(-100));
	// (2 x 1.2 + 10 x 0.3) x 3.2 along +z.
	CHECK(near(first.eye, {0, 0, (2.4f + 3) * 3.2f}));
	state.clock += 1;
	const Look later = flyBy(state, {0, 0, 10}, root, {0, 0, 10}, 2, flat(-100));
	CHECK(near(later.eye, first.eye));
	CHECK(near(later.at, {0, 0, 10}));
	CHECK(state.points.size() == 1);
	// The sea holds the point a metre up; the eye stays half a metre over
	// the terrain.
	FlyByState sea;
	Ground g = flat(3);
	g.sea = [](float, float) { return 1.0f; };
	const Look wet = flyBy(sea, {0, -5, 0}, root, {}, 1, g);
	CHECK(near(wet.eye.y, 3.5f));
}

// Trace (0x00610d20): from the camera at a point 20 m ahead that follows.
TEST(Camera_TraceLooksAhead)
{
	TraceState state;
	Frame parent;
	traceStart(state, {0, 1, 0}, parent);
	CHECK(near(state.point, {0, 1, 20}));
	Look look = trace(state, {0, 1, 5}, parent, nullptr);
	CHECK(near(look.eye, {0, 1, 5}));
	CHECK(near(look.at, {0, 1, 25}));
	// A target over 2 m off: 0.2 of the way a frame.
	const Vec3 target {10, 1, 5};
	look = trace(state, {0, 1, 5}, parent, &target);
	CHECK(near(look.at, {2, 1, 21}));
}

// The death camera (0x00615970): out from the body to max(3, radius x 3),
// leaning 0.3 up and back from where it faced, then away from the killer.
TEST(Camera_DeathCameraBacksAwayFromTheKiller)
{
	DeathState state;
	deathStart(state, {0, 0, 0}, {0, 0, 1}, 0.5f);
	CHECK(near(state.wanted, 3));
	const Vec3 body {0, 0, 0};
	Look look = death(state, &body, nullptr, 1.0f / 30, flat(-100));
	CHECK(look.eye.z < 0);
	for (int i = 0; i < 200; ++i) {
		look = death(state, &body, nullptr, 1.0f / 30, flat(-100));
	}
	const float l = std::sqrt(1 + 0.09f);
	CHECK(near(look.eye, {0, 0.3f / l * 3, -1 / l * 3}, 1e-2f));
	CHECK(near(look.at, body));
	// A killer to the east: the camera goes round to the west of the body.
	const Vec3 killer {20, 0, 0};
	for (int i = 0; i < 300; ++i) {
		look = death(state, &body, &killer, 1.0f / 30, flat(-100));
	}
	CHECK(look.eye.x < -2.5f && std::abs(look.eye.z) < 0.1f);
	// The terrain under it plus 0.8.
	look = death(state, &body, &killer, 1.0f / 30, flat(2));
	CHECK(near(look.eye.y, 2.8f));
}

// Following a player (0x00614d00): behind and above its camera by its
// radius, easing in.
TEST(Camera_FollowSettlesBehind)
{
	FollowState state;
	Frame camera;
	camera.position = {0, 2, 0};
	Look look;
	for (int i = 0; i < 400; ++i) {
		look = follow(state, camera, 2, nullptr, flat(-100));
	}
	CHECK(near(look.eye, {0, 2.6f, -2}, 1e-2f));
	CHECK(near(look.at, camera.position));
}

// The free camera (0x00615290): 20 m a second at cam-normal, x 10 fast,
// x 0.1 slow, never under the terrain + 0.2.
TEST(Camera_FreeCameraFlies)
{
	FreeState state;
	state.position = {0, 10, 0};
	FreeInput input;
	input.throttle = 1;
	freeMove(state, input, 1, flat(-100));
	CHECK(near(state.position, {0, 10, 20}));
	input.fast = true;
	freeMove(state, input, 0.1f, flat(-100));
	CHECK(near(state.position.z, 40));
	input = {};
	input.slow = true;
	input.strafe = 1;
	freeMove(state, input, 1, flat(-100));
	CHECK(near(state.position.x, 2));
	input = {};
	input.down = true;
	freeMove(state, input, 10, flat(0));
	CHECK(near(state.position.y, 0.2f));
	input = {};
	input.turnDegrees = 90;
	freeMove(state, input, 0, flat(-100));
	float yaw = 0, pitch = 0;
	anglesOf(freeLook(state), yaw, pitch);
	CHECK(near(yaw, 3.14159265f / 2));
	input = {};
	input.pitchDegrees = 30;
	freeMove(state, input, 0, flat(-100));
	anglesOf(freeLook(state), yaw, pitch);
	CHECK(near(pitch, 3.14159265f / 6));
}
