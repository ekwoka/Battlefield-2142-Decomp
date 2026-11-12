// The local player's camera (src/Main/PlayerCamera) in a joined game's
// seat: the seat's camera placed by the client's copy of the vehicle and
// the seat gun's zoom (DefaultZoomComp) on the client.
#include "Animation/AnimationSystem.h"
#include "Game/GameWorld.h"
#include "Main/PlayerCamera.h"
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
