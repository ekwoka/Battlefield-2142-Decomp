// The local player's camera (src/Main/PlayerCamera) in a joined game's
// seat: the seat's camera placed by the client's copy of the vehicle and
// the seat gun's zoom (DefaultZoomComp) on the client.
#include "Animation/AnimationSystem.h"
#include "Game/GameWorld.h"
#include "Main/PlayerCamera.h"
#include "Main/Viewer.h"
#include "Main/MissileViewSmoothing.h"
#ifdef BF2142_RENDER_TESTS
#include "Render/LevelRenderer.h"
#endif
#include "TestMain.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <cmath>

using namespace dice::hfe;

TEST(PlayerCamera_ExitAdoptsLastVehicleViewPitch)
{
	const float seatedPitch = 10.0f * 3.14159265f / 180.0f;
	const float oldAimPitch = -75.0f * 3.14159265f / 180.0f;
	CHECK(infantryPitchAfterVehicleExit(oldAimPitch, true, false, seatedPitch) == seatedPitch);
	CHECK(infantryPitchAfterVehicleExit(oldAimPitch, true, false, std::nullopt) == oldAimPitch);
	CHECK(infantryPitchAfterVehicleExit(oldAimPitch, true, true, seatedPitch) == oldAimPitch);
	CHECK(infantryPitchAfterVehicleExit(oldAimPitch, false, false, seatedPitch) == oldAimPitch);
}

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

// A soldier spawned between ticks (the deployment's Done): the camera's
// zero-time update taken then puts it in first person for that frame.
TEST(PlayerCamera_SpawnFrameIsFirstPerson)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	auto* soldier = templates.create("Soldier", "infantry");
	const auto rider = objects.create(soldier, {})->id();
	game::GameWorld round(templates, objects);
	anim::AnimationSystems systems(nullptr);
	PlayerCamera camera(round, systems, nullptr);
	bool alive = false;
	camera.setRemote([&](PlayerCamera::Remote& me) {
		me.alive = alive;
		me.soldier = alive ? rider : 0;
		me.soldierTemplate = alive ? soldier : nullptr;
		return true;
	});
	camera.tick(1 / 30.0f); // dead: the spawn screen
	alive = true;           // Done
	CHECK(!camera.insideView());
	camera.tick(0);
	CHECK(camera.insideView() && camera.hides(rider));
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

#ifdef BF2142_RENDER_TESTS
TEST(PlayerCamera_PodCameraKeepsTheHullScreenAxes)
{
	for (const world::Vec3 turn : {world::Vec3 {35, -40, 0}, world::Vec3 {-120, 65, 110},
			 world::Vec3 {170, -70, -160}}) {
		world::ObjectTemplateManager templates;
		world::ObjectManager objects;
		auto* pod = templates.create("Parachute", "test_pod");
		pod->settings()["isPod"] = "1";
		pod->children().push_back({"pod_camera_mount", {0, 1, 0}, {0, -30, 0}});
		auto* mount = templates.create("RotationalBundle", "pod_camera_mount");
		mount->children().push_back({"pod_camera"});
		templates.create("Camera", "pod_camera");
		auto* soldier = templates.create("Soldier", "us_soldier");
		const auto vehicle = objects.create(pod, {10, 50, 20}, turn)->id();
		const auto rider = objects.create(soldier, {})->id();
		game::GameWorld round(templates, objects);
		anim::AnimationSystems systems(nullptr);
		PlayerCamera camera(round, systems, nullptr);
		camera.setRemote([&](PlayerCamera::Remote& me) {
			me.alive = true; me.soldier = rider; me.soldierTemplate = soldier;
			me.vehicle = vehicle; me.seat = 0;
			return true;
		});
		camera.tick(1 / 30.0f);
		PlayerCamera::View eye;
		CHECK(camera.view(eye, 16.0f / 9.0f));
		if (!camera.view(eye, 16.0f / 9.0f)) return;
		render::Camera rendered;
		rendered.position = {eye.position[0], eye.position[1], eye.position[2]};
		rendered.yaw = eye.yaw;
		rendered.pitch = eye.pitch;
		rendered.roll = eye.roll;
		const auto screen = rendered.view();
		const auto hull = render::Mat4::yawPitchRoll(turn.x, turn.y, turn.z);
		const float right = hull.at(0, 0) * screen.at(0, 0)
			+ hull.at(0, 1) * screen.at(1, 0) + hull.at(0, 2) * screen.at(2, 0);
		CHECK(right > 0.999f); // A's negative hull-right force must remain screen-left.
	}
}
#endif

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

// A vehicle that names no seat (eu_ifv: each PlayerControlObject of its tree
// is a seat, GameWorld::vehicleLayout) puts a gunner at his own position's
// camera: the camera map of the seat's subtree (0x0058bdb0), not the
// driver's camera, nor the seated soldier's eye inside the hull.
TEST(PlayerCamera_UnnamedSeatsUseTheirOwnCameras)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	world::ObjectTemplate* ifv = templates.create("PlayerControlObject", "ifv");
	ifv->children().push_back({"ifv_cam", {0, 3, 0}, {}});
	ifv->children().push_back({"ifv_side", {-2, 1, 1}, {}});
	templates.create("Camera", "ifv_cam");
	world::ObjectTemplate* side = templates.create("PlayerControlObject", "ifv_side");
	side->children().push_back({"ifv_side_cam", {-0.5f, 1, 0}, {}});
	templates.create("Camera", "ifv_side_cam");
	world::ObjectTemplate* soldier = templates.create("Soldier", "us_soldier");
	const uint32_t vehicle = objects.create(ifv, {10, 0, 20})->id();
	const uint32_t rider = objects.create(soldier, {10, 0, 20})->id();
	game::GameWorld round(templates, objects);
	CHECK(round.vehicleSeats(vehicle).size() == 2);
	anim::AnimationSystems systems(nullptr);
	PlayerCamera cam(round, systems, nullptr);
	int seat = 0;
	cam.setRemote([&](PlayerCamera::Remote& me) {
		me.alive = true;
		me.soldier = rider;
		me.soldierTemplate = soldier;
		me.vehicle = vehicle;
		me.seat = seat;
		me.feet = {10, 0, 20};
		return true;
	});
	PlayerCamera::View eye;
	cam.tick(1 / 30.0f);
	CHECK(cam.view(eye, 4.0f / 3.0f));
	CHECK(std::fabs(eye.position[0] - 10) < 1e-4f && std::fabs(eye.position[1] - 3) < 1e-4f);
	seat = 1;
	cam.tick(1 / 30.0f);
	CHECK(cam.view(eye, 4.0f / 3.0f));
	CHECK(std::fabs(eye.position[0] - 7.5f) < 1e-4f && std::fabs(eye.position[1] - 2) < 1e-4f
		&& std::fabs(eye.position[2] - 21) < 1e-4f);
	// The seat camera's colour (ICamera slots 0x84 / 0x7c / 0x80): only
	// with useColorTransform; saturation 1 by default (0x00610fd0).
	std::array<float, 3> tint {};
	float saturation = -1;
	CHECK(!cam.viewColor(tint, saturation));
	templates.find("ifv_side_cam")->settings()["useColorTransform"] = "1";
	templates.find("ifv_side_cam")->settings()["colorTransform"] = "0.5/1/0.25";
	CHECK(cam.viewColor(tint, saturation) && tint[0] == 0.5f && tint[1] == 1 && tint[2] == 0.25f && saturation == 1);
	templates.find("ifv_side_cam")->settings()["saturation"] = "0.2";
	CHECK(cam.viewColor(tint, saturation) && std::fabs(saturation - 0.2f) < 1e-6f);
	seat = 0;
	CHECK(!cam.viewColor(tint, saturation));
}

// A seat placed turned (eu_fav_openPosition: addTemplate ... setRotation
// 180/0/0, its seatInformation's own rotation 0/0/0) faces its placement's
// way: the seat's matrix is seatInformation's in the PlayerControlObject's
// frame (0x00587a30), so the FAV's kneeling passenger looks out over the
// tail, not into the back of the driver's seat.
TEST(PlayerCamera_TurnedSeatFacesItsPlacement)
{
	world::ObjectTemplateManager templates;
	world::ObjectManager objects;
	world::ObjectTemplate* fav = templates.create("PlayerControlObject", "fav");
	fav->settings()["seatInformation"] = "fav 0/0.53/1.1 0/0/0";
	fav->children().push_back({"fav_open", {0, 0.12f, -1.22f}, {180, 0, 0}});
	world::ObjectTemplate* open = templates.create("PlayerControlObject", "fav_open");
	open->settings()["seatInformation"] = "fav_open 0/0.8/0.2 0/0/0";
	const uint32_t vehicle = objects.create(fav, {0, 0, 0})->id();
	game::GameWorld round(templates, objects);
	const auto seats = round.vehicleSeats(vehicle);
	CHECK(seats.size() == 2);
	if (seats.size() == 2) {
		CHECK(std::fabs(seats[0].rotation.x) < 1e-3f);
		CHECK(std::fabs(std::fabs(seats[1].rotation.x) - 180) < 1e-3f);
		CHECK(std::fabs(seats[1].position.y - 0.92f) < 1e-4f && std::fabs(seats[1].position.z + 1.42f) < 1e-4f);
	}
}
