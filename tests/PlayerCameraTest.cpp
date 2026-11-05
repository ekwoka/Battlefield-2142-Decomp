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
