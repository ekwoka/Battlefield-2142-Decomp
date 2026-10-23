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
