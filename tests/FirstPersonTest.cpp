// The first-person view's animation side: the zoom levels and the field of
// view they ask for, the eye's height by pose, the first-person skeleton's
// placement at the camera, and the rifle's first-person system on the
// first-person skeleton (the last part needs $BF2142_GAME_DIR).
#include "Animation/AnimationConsole.h"
#include "Animation/FirstPerson.h"
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "IO/File/FileManager.h"
#include "TestMain.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

using namespace dice::hfe;

namespace {

bool near(float a, float b, float tolerance = 1e-4f)
{
	return std::fabs(a - b) <= tolerance;
}

} // namespace

TEST(FirstPerson_ZoomLevelsAndFieldOfView)
{
	anim::ZoomSettings settings;
	settings.factors = {0, 0.484f};
	settings.delay = 0.15f;
	settings.lod = 1;
	anim::ZoomState zoom;
	anim::FieldOfView fov;
	fov.set(1.0f, 0);
	CHECK(settings.present() && settings.delayTicks() == 5);
	// The press asks for the level's field of view (absolute, radians) over
	// the delay; the zoomed LOD comes once the delay has passed.
	const float factor = zoom.cycle(settings);
	CHECK(zoom.level() == 1 && near(factor, 0.484f));
	fov.set(factor, settings.delay);
	for (int tick = 0; tick < 4; ++tick) {
		zoom.tick();
		fov.update(1 / 30.0f);
	}
	CHECK(zoom.lod(settings) == 0);
	CHECK(fov.current < 1.0f && fov.current > 0.484f);
	zoom.tick();
	fov.update(1 / 30.0f);
	CHECK(zoom.lod(settings) == 1);
	CHECK(near(fov.current, 0.484f, 1e-3f));
	fov.update(1 / 30.0f);
	CHECK(fov.current == 0.484f); // stops at the target
	// The next press wraps to level 0: the camera's own view, LOD 0 at once.
	CHECK(zoom.cycle(settings) == 0 && zoom.level() == 0 && zoom.lod(settings) == 0);
	// A weapon without levels doesn't zoom.
	anim::ZoomSettings none;
	anim::ZoomState other;
	CHECK(other.cycle(none) == 0 && other.level() == 0);
}

TEST(FirstPerson_EyeHeightByPose)
{
	anim::EyeHeight eye;
	eye.current = 4.1f; // the soldier camera's place in the template
	eye.tick(0);
	CHECK(near(eye.current, 4.1f + (0.47f - 4.1f) * 0.2f));
	for (int i = 0; i < 100; ++i) {
		eye.tick(0);
	}
	CHECK(near(eye.current, 0.47f, 1e-3f));
	eye.tick(2); // lying down: 0.2 of the way a tick
	CHECK(near(eye.current, 0.47f + (-0.63f - 0.47f) * 0.2f, 1e-3f));
}

TEST(FirstPerson_PlacementPutsTheCameraBoneAtTheEye)
{
	anim::Skeleton skeleton;
	skeleton.nodes.push_back({"Camerabone", -1, {0, 0, 0, 1}, {-0.024f, 1.504f, -0.111f}});
	skeleton.nodes.push_back({"torus", -1, {0, 0, 0, 1}, {0, 1.356f, -0.073f}});
	const std::vector<anim::Matrix> pose = anim::worldMatrices(skeleton, anim::setupPose(skeleton));
	const anim::Position eye {10, 20, 30};
	const float yaw = 0.7f, pitch = -0.3f;
	const anim::Matrix place = anim::firstPersonPlacement(skeleton, pose, 0, eye, yaw, pitch, {});
	const anim::Matrix camera = anim::multiply(pose[0], place);
	CHECK(near(camera[12], 10) && near(camera[13], 20) && near(camera[14], 30));
	// Its forward row is the view's.
	CHECK(near(camera[8], std::sin(yaw) * std::cos(pitch)) && near(camera[9], std::sin(pitch))
		&& near(camera[10], std::cos(yaw) * std::cos(pitch)));
	// The spring moves the whole rig along the view's axes.
	const anim::Matrix sprung = anim::multiply(pose[0], anim::firstPersonPlacement(skeleton, pose, 0, eye, 0, 0, {0.01f, 0.02f, -0.03f}));
	CHECK(near(sprung[12], 10.01f) && near(sprung[13], 20.02f) && near(sprung[14], 29.97f));
}

TEST(FirstPerson_RifleOnTheFirstPersonSkeleton)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/bf2142");
	Console console(&files);
	bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
		[&files](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
	std::string out;
	console.run("ServerArchives.con", {}, out);
	console.run("ClientArchives.con", {}, out);
	anim::AnimationSystems systems([&files](const std::string& path, std::vector<uint8_t>& data) {
		return files.readFile(path, data);
	});
	anim::registerAnimationConsoleMethods(console, systems);
	console.run("Objects/Soldiers/Common/Animations/ValueHolders.inc", {}, out);
	const auto load = [&](const std::string& path) { return systems.get(path, [&] { console.run(path, {}, out); }); };
	const anim::AnimationSystemTemplate* soldier = load("Objects/Soldiers/Common/Animations/AnimationSystem1p.inc");
	const anim::AnimationSystemTemplate* rifle = load("Objects/Weapons/Handheld/eu_ar_rifle/AnimationSystem1p.inc");
	const anim::Skeleton* skeleton = systems.skeleton("Objects/Soldiers/Common/Animations/1p_setup.ske");
	CHECK(soldier && rifle && skeleton);
	if (!soldier || !rifle || !skeleton) {
		return;
	}
	anim::FirstPersonRig rig(systems, *skeleton, soldier);
	CHECK(rig.cameraNode() == 0);
	rig.setWeapon(rifle);
	// The springs: the soldier's (amplitude 0.045, fire z 150) with the
	// rifle's parts over it (0.02, fire z 40).
	const anim::CameraSpring* spring = rig.spring();
	CHECK(spring && near(spring->amplitude.x, 0.02f) && near(spring->fire.z, 40) && near(spring->stiffness.x, 10));
	anim::AnimationInput input;
	rig.update(input, 1 / 30.0f);
	const auto playing = [&rig](const std::string& name) {
		const auto names = rig.playing();
		return std::find(names.begin(), names.end(), name) != names.end();
	};
	CHECK(playing("deploy")); // the startup trigger
	for (int i = 0; i < 60; ++i) {
		rig.update(input, 1 / 30.0f);
	}
	CHECK(playing("still"));
	// Zoomed (message 4): the zoomed stand.
	input.messages = 4;
	for (int i = 0; i < 10; ++i) {
		rig.update(input, 1 / 30.0f);
	}
	CHECK(playing("zoom_still"));
	// The weapon's first mesh node sits in front of and below the camera
	// bone (the rifle at the right hip of the view).
	const int mesh1 = skeleton->find("mesh1");
	CHECK(mesh1 >= 0);
	if (mesh1 >= 0) {
		const anim::Matrix& m = rig.pose()[static_cast<size_t>(mesh1)];
		const anim::Matrix& c = rig.pose()[0];
		CHECK(m[14] > c[14] && m[13] < c[13] + 0.05f);
	}
}

TEST(FirstPerson_RecoilGraphAndKick)
{
	// Without going back: n = 8 values -(i + 1)^2 x 2 / 204, adding up to -2.
	anim::RecoilSettings settings;
	settings.goBackOnRecoil = false;
	const std::vector<float> curve = settings.graph();
	CHECK(curve.size() == 8);
	float sum = 0;
	for (float v : curve) {
		sum += v;
	}
	CHECK(near(sum, -2.0f, 1e-5f) && near(curve[7], -128.0f / 204.0f));
	// The kick reads it backwards from the shot: the largest first, then
	// none from size - 1 ticks on.
	CHECK(near(anim::recoilAt(curve, 0, 1.0f), curve[7]) && near(anim::recoilAt(curve, 6, 0.5f), curve[1] * 0.5f));
	CHECK(anim::recoilAt(curve, 7, 1.0f) == 0 && anim::recoilAt(curve, -1, 1.0f) == 0);
	// Going back (the default): n + n/4 values that cancel out.
	settings.goBackOnRecoil = true;
	const std::vector<float> back = settings.graph();
	CHECK(back.size() == 10);
	float total = 0;
	for (float v : back) {
		total += v;
	}
	CHECK(near(total, 0.0f, 1e-5f));
	const float base = curve[7] * -0.5f;
	CHECK(near(back[9], curve[7] * 1.5f + base) && back[9] < 0); // the shot's tick
	CHECK(back[1] > 0);                                         // the way back
}

// The seat cameras' shakes (0x00610240): none below cameraShakeStartSpeed,
// half a degree at most at cameraShakeMaxSpeed, swinging 40 radians a
// second; a hit's shake starts and ends at 0 over 0.6 s, 2 x the hit at most.
TEST(FirstPerson_CameraShakes)
{
	CHECK(anim::cameraSpeedShake(1, 5, 10, 30) == 0);
	CHECK(anim::cameraSpeedShake(1, 5, 0, 30) == 0); // no start speed: off
	const float full = anim::cameraSpeedShake(3.14159265f / 80, 40, 10, 30); // sin(pi / 2)
	CHECK(std::fabs(full - 0.5f) < 1e-4f);
	CHECK(std::fabs(anim::cameraSpeedShake(3.14159265f / 80, 20, 10, 30) - 0.25f) < 1e-4f);
	CHECK(std::fabs(anim::cameraHitShake(anim::kCameraHitShakeTime, 1)) < 1e-6f);
	float most = 0;
	for (float left = 0.6f; left > 0; left -= 1 / 300.0f) {
		most = std::max(most, std::fabs(anim::cameraHitShake(left, 0.5f)));
	}
	CHECK(most > 0.5f && most < 1.0f);
	CHECK(anim::cameraHitShake(0, 1) == 0);
}

// The sniper scopes' fine tuning (0x005da670, 0x005da820, 0x005da890):
// factors 0, 0.312, 0.106 with 12 steps: the toggle knows two levels, the
// wheel moves the field of view by (0.312 - 0.106) / 12 between them,
// only with the zoom upgrade when the unlock is asked for.
TEST(FirstPerson_ZoomFineTuning)
{
	anim::ZoomSettings settings;
	settings.factors = {0, 0.312f, 0.106f};
	settings.useFineTuning = true;
	settings.fineTuneSteps = 12;
	anim::ZoomState zoom;
	CHECK(std::fabs(zoom.cycle(settings) - 0.312f) < 1e-6f);
	CHECK(zoom.cycle(settings) == 0 && zoom.level() == 0); // two levels only
	zoom.cycle(settings);
	CHECK(!zoom.fineTune(settings, 1, false)); // needs the unlock
	CHECK(zoom.fineTune(settings, 1, true));
	CHECK(std::fabs(zoom.factor(settings) - (0.312f - 0.206f / 12)) < 1e-5f);
	for (int i = 0; i < 20; ++i) {
		zoom.fineTune(settings, 1, true);
	}
	CHECK(std::fabs(zoom.factor(settings) - 0.106f) < 1e-5f && !zoom.fineTune(settings, 1, true));
	// Zoomed out and in again: the offset is kept (0x005da530).
	zoom.cycle(settings);
	CHECK(std::fabs(zoom.cycle(settings) - 0.106f) < 1e-5f);
	for (int i = 0; i < 20; ++i) {
		zoom.fineTune(settings, -1, true);
	}
	CHECK(std::fabs(zoom.factor(settings) - 0.312f) < 1e-5f && !zoom.fineTune(settings, -1, true));
	settings.fineTuningNeedsUnlock = false;
	CHECK(zoom.fineTune(settings, 1, false));
	// Without fine tuning: every level, no offset.
	anim::ZoomSettings plain;
	plain.factors = {0, 0.5f, 0.3f};
	anim::ZoomState other;
	other.cycle(plain);
	CHECK(other.cycle(plain) == 0.3f && !other.fineTune(plain, 1, true));
}

// A seat's zoom (anim::SeatZoom; 0x005c50e0 -> 0x005c0da0 -> 0x005da670,
// 0x005da3f0, 0x005da5c0, 0x005da630): eu_tank's gun (factors 0, 0.25,
// zoomDelay 0.55, zoomWhenBusy 1, zoomOutOnReload 0) under a camera of
// insideFOV 1.1: the alternate fire narrows the view over the delay, a
// second press widens it back to the camera's; busy, the press counts only
// with zoomWhenBusy; leaving the inside or nose view zooms out at once.
TEST(FirstPerson_SeatZoom)
{
	anim::ZoomSettings gun;
	gun.factors = {0, 0.25f};
	gun.delay = 0.55f;
	gun.zoomWhenBusy = true;
	gun.zoomOutOnReload = false;
	anim::SeatZoom zoom;
	zoom.reset(gun, 1.1f);
	CHECK(zoom.fov.current == 1.1f && zoom.state.level() == 0);
	zoom.step(true, false, true, 1.1f, 1 / 30.0f);
	CHECK(zoom.state.level() == 1 && zoom.fov.target == 0.25f && !zoom.zoomed());
	for (int i = 0; i < 17; ++i) {
		zoom.step(false, false, true, 1.1f, 1 / 30.0f);
	}
	CHECK(zoom.zoomed() && zoom.fov.current == 0.25f); // 0.55 s
	// Reloading doesn't zoom out (zoomOutOnReload 0), and a busy press
	// still steps it (zoomWhenBusy).
	zoom.step(false, true, true, 1.1f, 1 / 30.0f);
	CHECK(zoom.state.level() == 1);
	zoom.step(true, true, true, 1.1f, 1 / 30.0f);
	CHECK(zoom.state.level() == 0 && zoom.fov.target == 1.1f);
	// Without zoomWhenBusy the busy press is dropped; with
	// zoomOutOnReload a reload's start zooms out.
	anim::ZoomSettings launcher;
	launcher.factors = {0, 0.7f};
	launcher.delay = 0.62f;
	anim::SeatZoom av;
	av.reset(launcher, 1.1f);
	av.step(true, true, true, 1.1f, 1 / 30.0f);
	CHECK(av.state.level() == 0);
	av.step(true, false, true, 1.1f, 1 / 30.0f);
	CHECK(av.state.level() == 1);
	av.step(false, true, true, 1.1f, 1 / 30.0f);
	CHECK(av.state.level() == 0 && av.fov.target == 1.1f);
	// A chase view zooms out at once, and presses do nothing there.
	av.step(false, false, true, 1.1f, 1 / 30.0f);
	av.step(true, false, true, 1.1f, 1 / 30.0f);
	CHECK(av.state.level() == 1);
	av.step(false, false, false, 1.1f, 1 / 30.0f);
	CHECK(av.state.level() == 0 && av.fov.current == 1.1f);
	av.step(true, false, false, 1.1f, 1 / 30.0f);
	CHECK(av.state.level() == 0);
	// Level 0 follows the camera's own field of view (0x005da290).
	av.step(false, false, true, 0.9f, 1 / 30.0f);
	CHECK(av.fov.current == 0.9f);
	// A gun without a zoom never zooms.
	anim::SeatZoom none;
	none.reset({}, 1.1f);
	none.step(true, false, true, 1.1f, 1 / 30.0f);
	CHECK(none.state.level() == 0 && none.fov.current == 1.1f);
}

// allowZoomOnHeat (GenericFireArm +0x2d0, 0x005c50e0's busy test): an
// overheated gun drops the zoom press unless it allows zooming on heat or
// zooms when busy; with allowZoomOnHeat the overheat lets the press through
// even when it is reloading too, but a reload alone still blocks it.
TEST(FirstPerson_AllowZoomOnHeat)
{
	anim::ZoomSettings plain;
	plain.factors = {0, 0.59f};
	CHECK(plain.takesPress(false, false));
	CHECK(!plain.takesPress(false, true));
	CHECK(!plain.takesPress(true, false));
	anim::ZoomSettings heat = plain;
	heat.allowZoomOnHeat = true;
	CHECK(heat.takesPress(false, true));
	CHECK(heat.takesPress(true, true));
	CHECK(!heat.takesPress(true, false));
	anim::ZoomSettings busy = plain;
	busy.zoomWhenBusy = true;
	CHECK(busy.takesPress(true, false) && busy.takesPress(false, true));
	anim::SeatZoom seat;
	seat.reset(plain, 1.1f);
	seat.step(true, false, true, 1.1f, 1 / 30.0f, true);
	CHECK(seat.state.level() == 0);
	seat.reset(heat, 1.1f);
	seat.step(true, false, true, 1.1f, 1 / 30.0f, true);
	CHECK(seat.state.level() == 1);
}
