// Ground vehicles: the newCar2 tyre and spin functions, and a car on flat
// ground settling on its springs, driving, turning and braking.
#include "Game/Vehicle.h"
#include "TestMain.h"
#include "World/ObjectTemplate.h"
#include "World/ObjectTemplateManager.h"

#include <cmath>
#include <cstdio>

using namespace dice::hfe;
using physics::Vec3;

namespace {

bool near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

physics::Ground flatGround()
{
	physics::Ground ground;
	ground.height = [](float, float) { return 0.0f; };
	ground.heightAndNormal = [](float, float, Vec3& normal) {
		normal = {0, 1, 0};
		return 0.0f;
	};
	return ground;
}

// A car like eu_fav: an engine with two steering front wheels and two rear
// wheels, all springs.
struct Car {
	world::ObjectTemplateManager templates;
	std::unique_ptr<game::Vehicle> vehicle;
	physics::RigidBody body;
	physics::ContactResponse response;
	std::vector<Vec3> chassis;
	physics::Ground ground = flatGround();

	Car()
	{
		templates.registerType(1, "PlayerControlObject");
		templates.registerType(2, "Engine");
		templates.registerType(3, "RotationalBundle");
		templates.registerType(4, "Spring");
		world::ObjectTemplate* root = templates.create("PlayerControlObject", "car");
		root->settings()["mass"] = "1800";
		world::ObjectTemplate* engine = templates.create("Engine", "car_engine");
		engine->settings()["engineType"] = "194";
		engine->settings()["torque"] = "100";
		engine->settings()["differential"] = "3";
		engine->settings()["numberOfGears"] = "4";
		engine->settings()["newCar2.maxRpm"] = "3250";
		engine->settings()["newCar2.minRpm"] = "500";
		engine->settings()["newCar2.brakeTorque"] = "300";
		engine->settings()["newCar2.engineFeedbackMod"] = "0.1";
		engine->settings()["newCar2.frictionTorque"] = "50";
		engine->settings()["newCar2.wheelLatScale"] = "0.05";
		engine->settings()["newCar2.wheelLatPeakAt"] = "0.3";
		engine->settings()["newCar2.wheelLatFrictionMod"] = "1.2";
		engine->settings()["newCar2.wheelLatDriveFrictionMod"] = "1.4";
		world::ObjectTemplate* steering = templates.create("RotationalBundle", "car_nav");
		steering->settings()["inputToYaw"] = "PIYaw";
		steering->settings()["maxRotation"] = "32/0/0";
		steering->settings()["minRotation"] = "-32/0/0";
		steering->settings()["maxSpeed"] = "1/0/0";
		steering->settings()["acceleration"] = "100/0/0";
		steering->settings()["automaticReset"] = "1";
		steering->settings()["snapToZeroOnNoInput"] = "1";
		world::ObjectTemplate* wheel = templates.create("Spring", "car_wheel");
		wheel->settings()["strength"] = "40";
		wheel->settings()["damping"] = "8";
		root->children().push_back({"car_engine", {0, -0.44f, 0}, {}});
		engine->children().push_back({"car_nav", {-0.95f, 0, 1.6f}, {}});
		engine->children().push_back({"car_nav", {0.95f, 0, 1.6f}, {}});
		engine->children().push_back({"car_wheel", {-0.95f, -0.15f, -1.4f}, {}});
		engine->children().push_back({"car_wheel", {0.95f, -0.15f, -1.4f}, {}});
		steering->children().push_back({"car_wheel", {0, -0.15f, 0}, {}});
		vehicle = game::Vehicle::fromTemplate(*root, templates, nullptr);

		body.position = {0, 1.5f, 0};
		body.setBounds({2.8f, 1.5f, 4.4f});
		body.gravityModifier = 2;
		body.centerOfMass = {0, 0, 0.5f};
		for (float x : {-1.2f, 1.2f}) {
			for (float z : {-2.0f, 2.2f}) {
				chassis.push_back({x, -0.5f, z});
				chassis.push_back({x, 0.9f, z});
			}
		}
	}

	void run(int ticks, game::VehicleInput input = {})
	{
		vehicle->setInput(input);
		for (int i = 0; i < ticks; ++i) {
			vehicle->tick(body, ground, 1 / 30.0f, 0.8f);
			body.step(1 / 30.0f);
			physics::collideWithGround(body, response, chassis, ground, {});
			response.solve(body);
		}
	}

	float forwardSpeed() const { return physics::dot(body.velocity, body.rotation.rows[2]); }
};

} // namespace

TEST(Vehicle_TyreCurve)
{
	CHECK(near(game::tyreCurve(0.05f, 0.1f, 0.7f), 0.5f));
	CHECK(near(game::tyreCurve(0.1f, 0.1f, 0.7f), 1.0f));
	CHECK(near(game::tyreCurve(1.0f, 0.1f, 0.7f), 0.7f));
	CHECK(near(game::tyreCurve(0.55f, 0.1f, 0.7f), 0.85f));
	CHECK(near(game::tyreCurve(-0.55f, 0.1f, 0.7f), -0.85f));
	CHECK(near(game::tyreCurve(5.0f, 0.1f, 0.7f), 0.7f)); // no lower than minDynamic
	CHECK(game::tyreCurve(0, 0.1f, 0.7f) == 0);
}

TEST(Vehicle_WheelSpinStep)
{
	CHECK(near(game::wheelSpinStep(0, 24.5f, 0, 24.5f, 1), 1));
	CHECK(near(game::wheelSpinStep(2, 0, 24.5f, 24.5f, 1), 1));
	CHECK(game::wheelSpinStep(0.5f, 0, 49, 24.5f, 1) == 0); // friction stops, not reverses
	CHECK(near(game::wheelSpinStep(-2, 0, 24.5f, 24.5f, 1), -1));
}

TEST(Vehicle_RotationalBundleAxis)
{
	// eu_fav's steering (EU_FAV_nav_LF): automaticReset, so the angle heads
	// for input x maxRotation at |acceleration| (100) degrees a second.
	game::RotationAxis nav;
	nav.minRotation = -32;
	nav.maxRotation = 32;
	nav.maxSpeed = 1;
	nav.acceleration = 100;
	nav.automaticReset = true;
	nav.snapToZero = true;
	float angle = 0, speed = 0;
	game::rotationAxisStep(nav, 1, 0.1f, angle, speed);
	CHECK(near(angle, 10));
	for (int i = 0; i < 5; ++i) {
		game::rotationAxisStep(nav, 1, 0.1f, angle, speed);
	}
	CHECK(near(angle, 32)); // at the limit
	game::rotationAxisStep(nav, 0.5f, 0.1f, angle, speed);
	CHECK(near(angle, 22)); // back towards half the lock
	game::rotationAxisStep(nav, 0, 0.1f, angle, speed);
	CHECK(angle == 0); // snapToZeroOnNoInput
	game::rotationAxisStep(nav, -1, 0.1f, angle, speed);
	CHECK(near(angle, -10));
	game::rotationAxisStep(nav, 1, 0.1f, angle, speed);
	CHECK(near(angle, 10)); // the other way: snapped to 0 first
	// Without automaticReset (a turret): the speed heads for input x
	// maxSpeed at |acceleration|, and the angle moves by it.
	game::RotationAxis turret;
	turret.maxSpeed = 90;
	turret.acceleration = 450;
	angle = speed = 0;
	game::rotationAxisStep(turret, 1, 0.1f, angle, speed);
	CHECK(near(speed, 45) && near(angle, 4.5f));
	game::rotationAxisStep(turret, 1, 0.1f, angle, speed);
	game::rotationAxisStep(turret, 1, 0.1f, angle, speed);
	CHECK(near(speed, 90) && near(angle, 4.5f + 9 + 9));
}

TEST(Vehicle_GearRatios)
{
	game::EngineSettings engine;
	CHECK(engine.gearRatio(1) == 1); // without setGearRatios every gear is 1
	engine.numberOfGears = 5;
	engine.setGearRatios({4, 3, 2, 1.5f, 1});
	CHECK(near(engine.gearRatio(1), 4));
	CHECK(near(engine.gearRatio(3), 2));
	CHECK(near(engine.gearRatio(5), 1));
	CHECK(near(engine.gearRatio(0), 4.8f)); // reverse
	engine.numberOfGears = 4; // gear 1 at 0.25: between the keys at 0.2 and 0.4
	CHECK(near(engine.gearRatio(1), 3.75f));
}

TEST(Vehicle_TyreForceOpposesSliding)
{
	game::EngineSettings engine;
	game::TyreContact contact;
	float along = 0, across = 0;
	// Sliding right over the ground: slip (wheel less ground) points left.
	const Vec3 force = game::tyreForce(engine, contact, {-0.5f, 0, 0}, {0, 0, 1}, {1, 0, 0}, &along, &across);
	CHECK(force.x < 0 && near(force.z, 0));
	CHECK(near(across, -1.0f)); // -0.5 * wheelLatScale 0.1 = half the peak, times wheelLatFrictionMod 2
	// Tilted ground grips less: the cube of the normal's height.
	contact.normalY = 0.5f;
	const Vec3 tilted = game::tyreForce(engine, contact, {-0.5f, 0, 0}, {0, 0, 1}, {1, 0, 0});
	CHECK(near(tilted.x, force.x * 0.125f));
}

TEST(Vehicle_CarSettlesDrivesTurnsAndBrakes)
{
	Car car;
	CHECK(car.vehicle != nullptr);
	if (!car.vehicle) {
		return;
	}
	CHECK(car.vehicle->wheels().size() == 4);
	CHECK(car.vehicle->wheels()[0].maxSteer == 32 && car.vehicle->wheels()[2].maxSteer == 0);

	// Resting on its springs: the chassis clear of the ground.
	car.run(90);
	const float rest = car.body.position.y;
	std::fprintf(stderr, "  at rest at %.3f (wheel compression %.3f)\n", rest, car.vehicle->wheels()[0].compression);
	CHECK(rest > 0.6f && rest < 1.5f);
	CHECK(std::fabs(car.body.velocity.y) < 0.05f);
	CHECK(std::fabs(car.body.position.x) < 0.01f && std::fabs(car.body.position.z) < 0.05f);

	// Standing with no throttle it holds the brake (0x0064e810), and the
	// clutch, which starts out, stays out while braking.
	CHECK(car.vehicle->braking() && car.vehicle->clutch() == 0 && car.vehicle->gear() == 1);
	// Throttle: the clutch comes in at 2 / gearChangeTime a second.
	car.run(1, {1, 0});
	CHECK(!car.vehicle->braking() && car.vehicle->clutch() > 0 && car.vehicle->clutch() < 0.2f);

	// Full throttle for three seconds.
	car.run(89, {1, 0});
	const float speed = car.forwardSpeed();
	std::fprintf(stderr, "  after 3 s: %.1f m/s, at z %.1f, gear %d, rpm %.2f\n", speed, car.body.position.z,
		car.vehicle->gear(), car.vehicle->rpm());
	CHECK(speed > 5);
	CHECK(car.body.position.z > 5);
	CHECK(std::fabs(car.body.position.x) < 1);

	// Steering right turns the heading right (yaw grows).
	const float yaw = car.body.rotation.yawPitchRoll().x;
	car.run(30, {1, 1});
	const float turned = car.body.rotation.yawPitchRoll().x - yaw;
	std::fprintf(stderr, "  turned %.1f degrees in 1 s\n", turned);
	CHECK(turned > 5);

	// Throttle back brakes.
	const float before = car.forwardSpeed();
	car.run(30, {-1, 0});
	std::fprintf(stderr, "  braking: %.1f -> %.1f m/s\n", before, car.forwardSpeed());
	CHECK(car.forwardSpeed() < before - 2);
	CHECK(car.vehicle->braking() || car.vehicle->gear() == 0);
}
