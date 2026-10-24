// Ground vehicles: the newCar2 tyre and spin functions, and a car on flat
// ground settling on its springs, driving, turning and braking.
#include "Console/Console.h"
#include "Game/Vehicle.h"
#include "IO/File/FileManager.h"
#include "Physics/CollisionMeshFile.h"
#include "Physics/CollisionWorld.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplate.h"
#include "World/ObjectTemplateManager.h"
#include "World/WorldConsole.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

// Off the ground the driven wheel still runs (0x006b8230 -> 0x006b7910
// with zero contact sums): no tyre force, but the engine spins the wheels
// up (no 0.99 decay) and their rpm still counts; with the throttle off
// the friction and engine brake torques slow them.
TEST(Vehicle_WheelsSpinInTheAir)
{
	Car car;
	if (!car.vehicle) {
		return;
	}
	car.ground.height = [](float, float) { return -100.0f; };
	car.ground.heightAndNormal = [](float, float, Vec3& normal) {
		normal = {0, 1, 0};
		return -100.0f;
	};
	car.body.gravityModifier = 0; // held in the air
	car.run(30, {1, 0});
	const float spun = car.vehicle->wheels()[2].spin;
	std::fprintf(stderr, "  in the air: spin %.2f rad/s, rpm %.2f, gear %d\n", spun, car.vehicle->rpm(), car.vehicle->gear());
	CHECK(!car.vehicle->wheels()[2].contact && spun > 1);
	CHECK(std::fabs(car.body.velocity.z) < 1e-3f); // no tyre force
	car.run(30, {0, 0});
	CHECK(car.vehicle->wheels()[2].spin < spun);
}

// The shipped FAVs' wheels and their collision parts. A Spring's
// collisionPart is a geometry of the vehicle's collision mesh (0x0063ccc0
// hands the template's +0x74 to the collision manager's slot 0x28,
// 0x006d8180, which picks the geometry by it); the mesh instance's +0x14
// is the sub-geometry, the state (0 normal, 1 wreck). The files lead every
// geometry with an empty sub-geometry, which the loader drops (0x006d8740
// → 0x006e5820), so the normal shape is sub-geometry 0 and the wreck 1, and
// the trailing geometries without LODs go.
TEST(Vehicle_ShippedWheelsResolveToTheirCollisionParts)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/bf2142");
	files.setMountingAllowed(true);
	CHECK(files.mountArchive("Objects/Vehicles_server.zip", "Objects\\Vehicles"));
	files.setMountingAllowed(false);
	Console console {&files};
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	world::registerWorldConsoleMethods(console, templates, objects);
	world::registerUntypedTemplateProperties(console, templates, {"collisionPart", "strength", "damping"});
	struct Case {
		const char* name;
		const char* script;
		const char* mesh;
		std::vector<int> parts; // the wheels' in the tree's order
		float radius;           // the contact face's depth below the wheel's centre
	};
	for (const Case& c : {Case {"as_fav", "Objects/Vehicles/as_fav/as_fav.con", "Objects/Vehicles/as_fav/meshes/as_fav.collisionmesh",
							  {8, 10, 11, 12}, 0.49f},
			 Case {"eu_fav", "Objects/Vehicles/eu_fav/eu_fav.con", "Objects/Vehicles/eu_fav/meshes/eu_fav.collisionmesh",
				 {8, 10, 11, 13}, 0.55f}}) {
		std::string out;
		console.run(c.script, {}, out);
		const world::ObjectTemplate* root = templates.find(c.name);
		CHECK(root != nullptr);
		std::vector<uint8_t> data;
		CHECK(files.readFile(c.mesh, data));
		const auto mesh = physics::parseCollisionMesh(data);
		CHECK(mesh.has_value());
		if (!root || !mesh) {
			continue;
		}
		CHECK(mesh->geometries.size() == 15); // 46 in the file, 31 empty at the end
		// Geometry 0: the hull and its wreck, the first sub-geometry gone.
		CHECK(mesh->geometries[0].subGeometries.size() == 2);
		const physics::CollisionSubGeometry* hull = physics::collisionPart(*mesh, 0);
		const physics::CollisionSubGeometry* wreck = physics::collisionPart(*mesh, 0, 1);
		CHECK(hull && wreck && physics::collisionLodFor(*mesh, *hull, 1) && physics::collisionLodFor(*mesh, *wreck, 1));
		CHECK(physics::collisionLodFor(*mesh, *hull, 2) && !physics::collisionLodFor(*mesh, *wreck, 2));
		// Only the hull collides: its vehicle LOD's faces, not the wreck's too.
		physics::CollisionWorld world;
		world.addMesh(*mesh, physics::CollisionUser::Vehicle, {}, {}, 1, {});
		CHECK(world.triangles() <= physics::collisionLodFor(*mesh, *hull, 1)->faces.size());
		CHECK(world.triangles() + 4 > physics::collisionLodFor(*mesh, *hull, 1)->faces.size());

		const auto vehicle = game::Vehicle::fromTemplate(*root, templates, &*mesh);
		CHECK(vehicle != nullptr);
		if (!vehicle) {
			continue;
		}
		CHECK(vehicle->wheels().size() == c.parts.size());
		for (size_t i = 0; i < vehicle->wheels().size() && i < c.parts.size(); ++i) {
			const game::Vehicle::Wheel& wheel = vehicle->wheels()[i];
			CHECK(wheel.collisionPart == c.parts[i]);
			// The part is the wheel: a projectile LOD round it and a vehicle
			// LOD of one face, the contact, under its centre.
			const physics::CollisionSubGeometry* part = physics::collisionPart(*mesh, static_cast<size_t>(wheel.collisionPart));
			CHECK(part != nullptr);
			if (!part) {
				continue;
			}
			const physics::CollisionLod* contact = physics::collisionLodFor(*mesh, *part, 1);
			CHECK(contact && contact->faces.size() == 1 && physics::collisionLodFor(*mesh, *part, 0));
			CHECK(near(wheel.radius, c.radius, 0.02f));
			CHECK(wheel.spinRadius > 0.9f && wheel.spinRadius < 1.1f);
			std::printf("  %s %s: part %d radius %.3f spin %.3f\n", c.name, wheel.spring.c_str(), wheel.collisionPart,
				wheel.radius, wheel.spinRadius);
		}
	}
}
