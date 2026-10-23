// Rigid-body physics: falling, resting on the ground, turning.
#include "Physics/RigidBody.h"
#include "TestMain.h"

#include <cmath>
#include <cstdio>

namespace physics = dice::hfe::physics;

namespace {

// A 4 x 2 x 6 box's corners.
std::vector<physics::Vec3> boxCorners()
{
	std::vector<physics::Vec3> points;
	for (float x : {-2.0f, 2.0f}) {
		for (float y : {-1.0f, 1.0f}) {
			for (float z : {-3.0f, 3.0f}) {
				points.push_back({x, y, z});
			}
		}
	}
	return points;
}

physics::Ground flatGround(float level)
{
	physics::Ground ground;
	ground.height = [level](float, float) { return level; };
	ground.heightAndNormal = [level](float, float, physics::Vec3& normal) {
		normal = {0, 1, 0};
		return level;
	};
	return ground;
}

void simulate(physics::RigidBody& body, int ticks, const physics::Ground& ground)
{
	physics::ContactResponse response;
	const auto points = boxCorners();
	for (int i = 0; i < ticks; ++i) {
		body.step(1 / 30.0f);
		physics::collideWithGround(body, response, points, ground, {});
		response.solve(body);
	}
}

} // namespace

TEST(RigidBody_FallsWithGravity)
{
	physics::RigidBody body;
	body.position = {0, 100, 0};
	for (int i = 0; i < 30; ++i) {
		body.step(1 / 30.0f);
	}
	// About a second of -14.73 m/s^2, less the damping.
	CHECK(body.velocity.y < -10 && body.velocity.y > -14.73f);
	CHECK(body.position.y < 95 && body.position.y > 90);
}

TEST(RigidBody_ComesToRestOnTheGround)
{
	physics::RigidBody body;
	body.setBounds({4, 2, 6});
	body.position = {0, 20, 0};
	simulate(body, 30 * 10, flatGround(5));
	// Resting with its lowest corners on the ground and staying there. (The
	// velocity does not reach 0: each tick's gravity meets a contact
	// impulse whose vertical part is halved, and the push out holds the
	// position; the original puts resting bodies to sleep.)
	CHECK(std::fabs(body.position.y - 6) < 0.3f);
	const float rested = body.position.y;
	simulate(body, 30, flatGround(5));
	CHECK(std::fabs(body.position.y - rested) < 0.01f);
	CHECK(body.rotation.rows[1].y > 0.99f);
}

TEST(RigidBody_TiltedLandingSettles)
{
	physics::RigidBody body;
	body.setBounds({4, 2, 6});
	body.position = {0, 12, 0};
	body.rotation = physics::Mat3::axisAngle({0, 0, 1}, 0.4f); // rolled
	simulate(body, 30 * 20, flatGround(0));
	// Falls flat onto its bottom and stays near where it landed.
	CHECK(std::fabs(body.position.x) < 5 && std::fabs(body.position.z) < 5);
	CHECK(body.position.y > 0.5f && body.position.y < 3);
	CHECK(std::isfinite(body.position.y) && lengthSquared(body.angularVelocity) < 1);
	std::printf("  settled at %.2f, up %.3f/%.3f/%.3f\n", body.position.y, body.rotation.rows[1].x,
		body.rotation.rows[1].y, body.rotation.rows[1].z);
}

TEST(RigidBody_YawPitchRollRoundTrips)
{
	const physics::Vec3 angles {30, -20, 10};
	const physics::Vec3 back = physics::Mat3::fromYawPitchRoll(angles).yawPitchRoll();
	CHECK(std::fabs(back.x - 30) < 1e-3f && std::fabs(back.y + 20) < 1e-3f && std::fabs(back.z - 10) < 1e-3f);
	// Yaw turns the forward axis (+z) towards +x.
	const physics::Mat3 turned = physics::Mat3::fromYawPitchRoll({90, 0, 0});
	CHECK(std::fabs(turned.transform({0, 0, 1}).x - 1) < 1e-5f);
}

// The shares of a contact between two nodes (0x006b6cc0).
TEST(RigidBody_MassShares)
{
	using physics::massShares;
	const physics::ShareNode car {1800, false, false}, crate {200, false, false};
	// By mass, the lighter taking more, kept within 0.05..0.95.
	physics::MassShares s = massShares(car, crate, -2, false, false, false);
	CHECK(std::fabs(s.velocityA - 0.1f) < 1e-6f && std::fabs(s.velocityB + 0.9f) < 1e-6f);
	CHECK(s.depthA == s.velocityA && s.depthB == s.velocityB);
	s = massShares({1, false, false}, {1000, false, false}, -2, false, false, false);
	CHECK(std::fabs(s.velocityA - 0.95f) < 1e-6f && std::fabs(s.velocityB + 0.05f) < 1e-6f);
	// A Particle's shares are not limited.
	s = massShares({1, false, false}, {1000, false, false}, -2, true, false, false);
	CHECK(s.velocityA > 0.99f);
	// A hard contact (over 10 m/s along the normal) halves the push out each way.
	s = massShares(car, crate, -12, false, false, false);
	CHECK(std::fabs(s.velocityA - 0.1f) < 1e-6f && s.depthA == 0.5f && s.depthB == -0.5f);
	// A static object takes nothing; the mobile one everything.
	const physics::ShareNode wall {1, false, true};
	s = massShares(car, wall, -12, false, false, true);
	CHECK(s.velocityA == 1 && s.velocityB == 0 && s.depthA == 1 && s.depthB == 0);
	s = massShares(wall, car, -12, false, true, false);
	CHECK(s.velocityA == 0 && s.velocityB == -1 && s.depthA == 0 && s.depthB == -1);
	// A major actor (isMajorActor) is not moved by a minor one.
	s = massShares({1800, true, false}, crate, -2, false, false, false);
	CHECK(s.velocityA == 0 && s.velocityB == -1);
	s = massShares(crate, {1800, true, false}, -2, false, false, false);
	CHECK(s.velocityA == 1 && s.velocityB == 0);
	// A sleeping node hit hard enough (the other's momentum along the
	// normal over its mass) wakes and takes its share; else it stays put.
	s = massShares({200, false, true}, car, -0.5f, false, false, false);
	CHECK(s.wakeA && std::fabs(s.velocityA - 0.9f) < 1e-6f && s.depthA == 0);
	s = massShares({200000, false, true}, car, -0.5f, false, false, false);
	CHECK(!s.wakeA && s.velocityA == 0 && s.velocityB == -1);
}

// The broad phase's sphere-against-box (0x006aecc0) and box-against-box
// (0x006aef70) tests.
TEST(RigidBody_BroadPhaseTests)
{
	const physics::Mat3 turned = physics::Mat3::fromYawPitchRoll({45, 0, 0});
	CHECK(physics::sphereTouchesBox({0, 0, 2.4f}, 0.5f, {-1, -1, -2}, {1, 1, 2}, {}, {}));
	CHECK(!physics::sphereTouchesBox({0, 0, 2.6f}, 0.5f, {-1, -1, -2}, {1, 1, 2}, {}, {}));
	// The box turned 45 degrees: its corner reaches out to 1.41 along x.
	CHECK(physics::sphereTouchesBox({1.6f, 0, 0}, 0.25f, {-1, -1, -1}, {1, 1, 1}, turned, {}));
	CHECK(!physics::sphereTouchesBox({1.8f, 0, 0}, 0.25f, {-1, -1, -1}, {1, 1, 1}, turned, {}));
	CHECK(physics::boxesOverlap({-1, -1, -1}, {1, 1, 1}, {}, {}, {-1, -1, -1}, {1, 1, 1}, turned, {2.3f, 0, 0}));
	CHECK(!physics::boxesOverlap({-1, -1, -1}, {1, 1, 1}, {}, {}, {-1, -1, -1}, {1, 1, 1}, turned, {2.5f, 0, 0}));
	CHECK(!physics::boxesOverlap({-1, -1, -1}, {1, 1, 1}, {}, {}, {-1, -1, -1}, {1, 1, 1}, {}, {0, 2.1f, 0}));
}

// Sleeping (0x006c9060): a node whose mean motion over three ticks stays
// under 0.5 m/s (and its turning over five under 0.5 rad/s) counts down
// from 30 and sleeps at 0; moving again (or slot 0x160) wakes it.
TEST(RigidBody_SleepsWhenStill)
{
	physics::SleepState sleep;
	CHECK(!sleep.asleep() && sleep.counter == 30);
	physics::Vec3 position;
	int ticks = 0;
	while (!sleep.asleep() && ticks < 100) {
		position.x += 0.1f / 30; // 0.1 m/s
		sleep.sample(position, {}, 1 / 30.0f);
		++ticks;
	}
	CHECK(ticks == 30);
	position.x += 1.0f / 30 * 3; // 3 m/s: the three-tick mean is 1 m/s
	CHECK(sleep.sample(position, {}, 1 / 30.0f) && !sleep.asleep());
	// Turning wakes it as well.
	physics::SleepState spin;
	for (int i = 0; i < 4; ++i) {
		CHECK(!spin.sample({}, {0, 0.6f, 0}, 1 / 30.0f) || i >= 4);
	}
	CHECK(spin.counter < 30);
	CHECK(spin.sample({}, {0, 0.6f, 0}, 1 / 30.0f) && spin.counter == 30);
}

// Drag (0x006c7e00): along each body axis by the area across it; never
// more than the velocity itself in a tick; 25 times as strong under water.
TEST(RigidBody_DragAndBuoyancy)
{
	physics::RigidBody body;
	body.velocity = {0, 0, 20};
	physics::applyDrag(body, 1, 1000, {1, 1, 1}, {2, 1, 4}, 0);
	// drag / mass x |v| x area (2 x 1 x 0.785) x v = 0.0628 a tick.
	CHECK(std::fabs(body.acceleration.z + 0.785f * 2 * 20 * 20 / 1000) < 1e-3f);
	physics::RigidBody wet;
	wet.velocity = {0, 0, 20};
	physics::applyDrag(wet, 1, 1000, {1, 1, 1}, {2, 1, 4}, 1);
	CHECK(std::fabs(wet.acceleration.z + 0.785f * 2 * 500 * 500 / 1000) < 1e-1f); // the velocity counts 25 times
	physics::RigidBody slow;
	slow.velocity = {0, 0, 0.5f};
	physics::applyDrag(slow, 1000, 1, {1, 1, 1}, {2, 1, 4}, 1);
	CHECK(std::fabs(slow.acceleration.z + 0.5f * 30) < 1e-3f); // no more than the velocity in a tick
	// Buoyancy (0x006b9260): a box wholly under the water with floaterMod 1
	// is lifted as hard as gravity pulls it; half in, by half.
	const std::vector<physics::Vec3> corners = boxCorners();
	physics::RigidBody under;
	under.position = {0, -5, 0};
	bool entered = false;
	const float depth = physics::applyBuoyancy(under, {-2, -1, -3}, {2, 1, 3}, corners, 0, 1, physics::tuning::kGravity,
		0, true, &entered);
	CHECK(depth == 1 && entered);
	CHECK(std::fabs(under.acceleration.y + physics::tuning::kGravity) < 1e-3f);
	physics::RigidBody half;
	half.position = {0, 0, 0};
	physics::applyBuoyancy(half, {-2, -1, -3}, {2, 1, 3}, corners, 0, 1, physics::tuning::kGravity, 0.5f, true);
	CHECK(std::fabs(half.acceleration.y + physics::tuning::kGravity * 0.5f) < 1e-3f);
	physics::RigidBody dry;
	dry.position = {0, 5, 0};
	CHECK(physics::applyBuoyancy(dry, {-2, -1, -3}, {2, 1, 3}, corners, 0, 1, physics::tuning::kGravity, 0, true) == 0);
}

// The point nodes (0x006c57a0, 0x006c6860): no damping, the mean of the
// old and new velocity moves them; a RotationalPoint turns by its torque
// over its mass x 150 about its own origin, without damping.
TEST(RigidBody_PointNodes)
{
	physics::PointBody point;
	point.velocity = {10, 0, 0};
	point.step(1 / 30.0f);
	CHECK(point.velocity.x == 10 && std::fabs(point.position.x - 10.0f / 30) < 1e-5f);
	point.step(1 / 30.0f);
	CHECK(std::fabs(point.velocity.y - physics::tuning::kGravity / 30) < 1e-5f);
	physics::PointBody slowed = {};
	slowed.drag = 18;
	slowed.velocity = {100, 0, 0};
	slowed.step(1 / 30.0f);
	slowed.step(1 / 30.0f);
	CHECK(slowed.velocity.x < 100 && slowed.velocity.x > 80);
	physics::PointBody spinner;
	spinner.rotational = true;
	spinner.mass = 2;
	spinner.torque = {0, 1, 0};
	spinner.step(1 / 30.0f);
	CHECK(std::fabs(spinner.angularVelocity.y - 1.0f / 30 * 150 / 2) < 1e-4f);
	spinner.step(1 / 30.0f);
	CHECK(std::fabs(spinner.angularVelocity.y - 2.5f) < 1e-4f); // kept
	CHECK(spinner.rotation.rows[0].x < 1);
}
