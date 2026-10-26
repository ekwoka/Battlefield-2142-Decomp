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
