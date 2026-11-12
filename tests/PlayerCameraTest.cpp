// The local player's camera (src/Main/PlayerCamera) in a joined game's
// seat: the seat's camera placed by the client's copy of the vehicle and
// the seat gun's zoom (DefaultZoomComp) on the client.
#include "Animation/AnimationSystem.h"
#include "Game/GameWorld.h"
#include "Main/PlayerCamera.h"
#include "Main/MissileViewSmoothing.h"
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <cmath>

using namespace dice::hfe;

TEST(PlayerCamera_JoinedSeatZooms)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	world::ObjectTemplate* tank = templates.create("PlayerControlObject", "tank");
	tank->settings()["seatInformation"] = "tank 0/1/0 0/0/0";
	tank->children().push_back({"tank_cam", {0, 3, 0}, {}});
	tank->children().push_back({"tank_gun", {0, 2, 1}, {}});
	world::ObjectTemplate* camera = templates.create("Camera", "tank_cam");
	camera->settings()["insideFOV"] = "1.2";
	world::ObjectTemplate* gun = templates.create("GenericFireArm", "tank_gun");
	gun->components().push_back("DefaultZoomComp");
	gun->settings()["zoom.addZoomFactor.list"] = "0 0.5";
	gun->settings()["zoom.zoomDelay"] = "0";
	world::ObjectTemplate* soldier = templates.create("Soldier", "us_soldier");
	const uint32_t vehicle = objects.create(tank, {10, 0, 20})->id();
	const uint32_t rider = objects.create(soldier, {10, 0, 20})->id();
	game::GameWorld round(templates, objects);
	anim::AnimationSystems systems(nullptr);
	PlayerCamera cam(round, systems, nullptr);
	cam.setRemote([&](PlayerCamera::Remote& me) {
		me.alive = true;
		me.soldier = rider;
		me.soldierTemplate = soldier;
		me.vehicle = vehicle;
		me.seat = 0;
		me.feet = {10, 0, 20};
		return true;
	});
	cam.tick(1 / 30.0f);
	PlayerCamera::View eye;
	CHECK(cam.view(eye, 4.0f / 3.0f));
	CHECK(std::fabs(eye.position[0] - 10) < 1e-4f && std::fabs(eye.position[1] - 3) < 1e-4f
		&& std::fabs(eye.position[2] - 20) < 1e-4f);
	const float wide = eye.fovY;
	cam.toggleMode();
	CHECK(cam.mode() == game::camera::Mode::Chase);
	CHECK(cam.selectView(2));
	CHECK(cam.mode() == game::camera::Mode::Chase && !cam.firstPerson);
	CHECK(cam.selectView(1));
	CHECK(cam.mode() == game::camera::Mode::Inside && cam.firstPerson);
	CHECK(!cam.selectView(5));
	CHECK(cam.seatZoomLevel() == 0);
	// The alternate fire's press steps the seat gun's zoom.
	cam.setAltFire(true);
	cam.tick(1 / 30.0f);
	cam.setAltFire(false);
	for (int i = 0; i < 5; ++i) {
		cam.tick(1 / 30.0f);
	}
	CHECK(cam.seatZoomLevel() == 1 && cam.zoomLevel() == 1);
	CHECK(cam.view(eye, 4.0f / 3.0f) && eye.fovY < wide);
}

TEST(PlayerCamera_ImplicitSeatsKeepTheirOwnCameras)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	auto* root = templates.create("PlayerControlObject", "walker");
	auto* gunner = templates.create("PlayerControlObject", "walker_gunner");
	// As in the walkers: neither seat has seatInformation. The gunner's
	// subtree precedes the driver's camera in depth-first order.
	root->children().push_back({"WALKER_GUNNER", {0, 5, 0}, {}});
	root->children().push_back({"driver_cam", {0, 2, 1}, {}});
	gunner->children().push_back({"gunner_cam", {0, 3, 0}, {}});
	templates.create("Camera", "driver_cam");
	templates.create("Camera", "gunner_cam");
	auto* soldier = templates.create("Soldier", "us_soldier");
	const auto vehicle = objects.create(root, {10, 0, 20})->id();
	const auto rider = objects.create(soldier, {})->id();
	game::GameWorld round(templates, objects);
	anim::AnimationSystems systems(nullptr);
	PlayerCamera camera(round, systems, nullptr);
	int seat = 0;
	camera.setRemote([&](PlayerCamera::Remote& me) {
		me.alive = true; me.soldier = rider; me.soldierTemplate = soldier;
		me.vehicle = vehicle; me.seat = seat;
		return true;
	});
	PlayerCamera::View view;
	camera.tick(1 / 30.0f);
	CHECK(camera.view(view, 16.0f / 9.0f));
	CHECK(std::fabs(view.position[1] - 2) < 1e-4f);
	seat = 1;
	camera.tick(1 / 30.0f);
	CHECK(camera.view(view, 16.0f / 9.0f));
	CHECK(std::fabs(view.position[1] - 8) < 1e-4f);
}

TEST(PlayerCamera_LivingInfantryKeepsFirstPerson)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	auto* soldier = templates.create("Soldier", "infantry");
	const auto rider = objects.create(soldier, {})->id();
	game::GameWorld round(templates, objects);
	anim::AnimationSystems systems(nullptr);
	PlayerCamera camera(round, systems, nullptr);
	camera.setRemote([&](PlayerCamera::Remote& me) {
		me.alive = true; me.soldier = rider; me.soldierTemplate = soldier;
		return true;
	});
	camera.tick(1 / 30.0f);
	camera.toggleMode();
	CHECK(camera.firstPerson);
	CHECK(!camera.selectView(2));
	CHECK(!camera.selectView(3));
	CHECK(!camera.selectView(4));
	CHECK(camera.firstPerson);
	CHECK(camera.selectView(1));
}

TEST(PlayerCamera_RespawnAfterDyingInThirdPersonSeatIsFirstPerson)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	auto* soldier = templates.create("Soldier", "infantry");
	const auto rider = objects.create(soldier, {})->id();
	game::GameWorld round(templates, objects);
	anim::AnimationSystems systems(nullptr);
	PlayerCamera camera(round, systems, nullptr);
	bool alive = true;
	camera.setRemote([&](PlayerCamera::Remote& me) {
		me.alive = alive; me.soldier = rider; me.soldierTemplate = soldier;
		return true;
	});
	camera.tick(1 / 30.0f);
	camera.firstPerson = false; // a seat's chase view (toggleMode/selectView) when the soldier dies
	alive = false;
	camera.tick(1 / 30.0f);
	alive = true;
	camera.tick(1 / 30.0f);
	CHECK(camera.firstPerson);
	CHECK(camera.insideView());
}

TEST(VehicleHud_GeometryOnlyFirearmHasNoFireGroup)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	auto* tank = templates.create("PlayerControlObject", "tank");
	tank->children().push_back({"barrel_geometry", {}, {}});
	tank->children().push_back({"cannon", {}, {}});
	auto* geometry = templates.create("GenericFireArm", "barrel_geometry");
	geometry->settings()["geometryPart"] = "2";
	auto* cannon = templates.create("GenericFireArm", "cannon");
	cannon->components().push_back("SingleFireComp");
	cannon->settings()["ammo.magSize"] = "1";
	cannon->settings()["weaponHud.guiIndex"] = "68";
	const auto vehicle = objects.create(tank, {})->id();
	game::GameWorld round(templates, objects);
	const auto seats = round.vehicleSeats(vehicle);
	CHECK(seats.size() == 1);
	if (seats.size() != 1) return;
	CHECK(seats[0].weapons.size() == 2);
	if (seats[0].weapons.size() != 2) return;
	CHECK(seats[0].weapons[0].group == -1);
	CHECK(seats[0].weapons[1].group == 0);
}

// A seat camera fixed to a banked hull rolls the view with it (the cockpit
// stays put on screen); a level hull has no roll.
TEST(PlayerCamera_SeatCameraRollsWithTheHull)
{
	for (const float bank : {0.0f, 20.0f, -35.0f}) {
		world::ObjectTemplateManager templates;
		world::ObjectManager objects;
		auto* root = templates.create("PlayerControlObject", "gunship");
		root->children().push_back({"pilot_cam", {0, 2, 1}, {}});
		templates.create("Camera", "pilot_cam");
		auto* soldier = templates.create("Soldier", "us_soldier");
		const auto vehicle = objects.create(root, {10, 50, 20}, {30, 0, bank})->id();
		const auto rider = objects.create(soldier, {})->id();
		game::GameWorld round(templates, objects);
		anim::AnimationSystems systems(nullptr);
		PlayerCamera camera(round, systems, nullptr);
		camera.setRemote([&](PlayerCamera::Remote& me) {
			me.alive = true; me.soldier = rider; me.soldierTemplate = soldier;
			me.vehicle = vehicle; me.seat = 0;
			return true;
		});
		PlayerCamera::View view;
		camera.tick(1 / 30.0f);
		CHECK(camera.view(view, 16.0f / 9.0f));
		CHECK(std::fabs(std::fabs(view.roll) - std::fabs(bank) * 3.14159265f / 180) < 1e-3f);
		CHECK(std::fabs(view.yaw - 30 * 3.14159265f / 180) < 1e-3f);
	}
}

// Retail keeps a soldier under his parachute (a seat without a camera) in
// first person: no view key reaches the external modes.
TEST(PlayerCamera_ParachuteStaysFirstPerson)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	world::ObjectTemplate* chute = templates.create("Parachute", "parachute");
	chute->settings()["seatInformation"] = "Parachute 0/-2/0 0/0/0";
	world::ObjectTemplate* soldier = templates.create("Soldier", "infantry");
	const uint32_t vehicle = objects.create(chute, {0, 100, 0})->id();
	const uint32_t rider = objects.create(soldier, {0, 100, 0})->id();
	game::GameWorld round(templates, objects);
	anim::AnimationSystems systems(nullptr);
	PlayerCamera camera(round, systems, nullptr);
	camera.setRemote([&](PlayerCamera::Remote& me) {
		me.alive = true; me.soldier = rider; me.soldierTemplate = soldier;
		me.vehicle = vehicle; me.seat = 0;
		me.feet = {0, 100, 0};
		return true;
	});
	camera.tick(1 / 30.0f);
	PlayerCamera::View eye;
	CHECK(camera.insideView() && camera.view(eye, 4.0f / 3.0f)); // else the renderer falls back to third person
	CHECK(eye.position[1] > 100);
	camera.toggleMode();
	CHECK(camera.firstPerson);
	CHECK(!camera.selectView(2));
	CHECK(!camera.selectView(3));
	CHECK(!camera.selectView(4));
	camera.tick(1 / 30.0f);
	CHECK(camera.firstPerson && camera.mode() == game::camera::Mode::Inside);
}

TEST(PlayerCamera_MissileViewInterpolation)
{
	MissileViewSmoothing smooth;
	MissileViewSmoothing::Pose first {{0, 0, 0}, 3.10f, 0};
	MissileViewSmoothing::Pose next {{10, 4, 2}, -3.10f, -0.4f};
	auto p = smooth.sample(1, true, 0, first);
	CHECK(p.position[0] == 0);
	p = smooth.sample(1, true, 0.5f, next);
	CHECK(std::fabs(p.position[0] - 5) < 0.001f && std::fabs(p.pitch + 0.2f) < 0.001f);
	CHECK(std::fabs(p.yaw - 3.14159265f) < 0.01f);
	p = smooth.sample(2, true, 0, next);
	CHECK(p.position[0] == 10 && p.yaw == next.yaw);
}

// The cockpit geometry's reach (0x00612880): the seat's own mesh and its
// children's; a Titan gun seat must leave the other guns whole.
TEST(PlayerCamera_CockpitCoversOnlyTheSeat)
{
	PlayerCamera::Cockpit gun;
	gun.seat = "eu_groundcanone3_01_hub";
	gun.seatHasGeometry = false;
	CHECK(PlayerCamera::cockpitCovers(gun, "eu_groundcanone3_01_hub/eu_groundcanone3_01"));
	CHECK(PlayerCamera::cockpitCovers(gun, "EU_groundcanone3_01_Hub/eu_controlstation_v2_g"));
	CHECK(!PlayerCamera::cockpitCovers(gun, "eu_groundcanone3_02_hub/eu_groundcanone3_02"));
	CHECK(!PlayerCamera::cockpitCovers(gun, "eu_groundcanone3_01_hub/eu_groundcanone3_01/eu_groundcanone3_base_01"));
	CHECK(PlayerCamera::cockpitCovers(gun, "")); // a seat with no mesh: the root's
	gun.seatHasGeometry = true;
	CHECK(!PlayerCamera::cockpitCovers(gun, ""));
	PlayerCamera::Cockpit driver; // the root's seat: the hull and its children
	CHECK(PlayerCamera::cockpitCovers(driver, ""));
	CHECK(PlayerCamera::cockpitCovers(driver, "us_tnk_turret"));
	CHECK(!PlayerCamera::cockpitCovers(driver, "us_tnk_turret/us_tnk_barrel"));
}
