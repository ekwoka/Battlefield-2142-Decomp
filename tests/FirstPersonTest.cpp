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
#include <array>
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

TEST(FirstPerson_RemasterShotgunShiftAndReloadMessages)
{
	REMASTER_ONLY();
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) return;
	io::FileManager files;
	files.setRootPath(gameDir);
	files.addSearchPath("mods/Project_Remaster_v17_Final");
	files.addSearchPath("mods/bf2142");
	Console console(&files);
	bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
		[&files](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
	std::string out;
	CHECK(console.run("ServerArchives.con", {}, out));
	CHECK(console.run("ClientArchives.con", {}, out));
	anim::AnimationSystems systems([&files](const std::string& path, std::vector<uint8_t>& data) {
		return files.readFile(path, data);
	});
	anim::registerAnimationConsoleMethods(console, systems);
	CHECK(console.run("Objects/Soldiers/Common/Animations/ValueHolders.inc", {}, out));
	const auto load = [&](const std::string& path) { return systems.get(path, [&] { console.run(path, {}, out); }); };
	const auto* soldier = load("Objects/Soldiers/Common/Animations/AnimationSystem1p.inc");
	const auto* shotgun = load("Objects/Weapons/Handheld/EU_Jackhammer/AnimationSystem1p.inc");
	const auto* skeleton = systems.skeleton("Objects/Soldiers/Common/Animations/1p_setup.ske");
	CHECK(soldier && shotgun && skeleton);
	if (!soldier || !shotgun || !skeleton) return;
	anim::FirstPersonRig rig(systems, *skeleton, soldier);
	rig.setWeapon(shotgun);
	anim::AnimationInput input;
	for (int i = 0; i < 90; ++i) rig.update(input, 1 / 30.0f);
	const auto playing = [&](const char* name) {
		const auto names = rig.playing();
		return std::find(names.begin(), names.end(), name) != names.end();
	};
	input.pulses = 32;
	rig.update(input, 1 / 30.0f);
	CHECK(playing("shift"));
	input.pulses = 0;
	for (int i = 0; i < 60; ++i) rig.update(input, 1 / 30.0f);
	input.messages = 2;
	rig.update(input, 1 / 30.0f);
	CHECK(playing("reload"));
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
	// A reload begun zoomed: the zoom-out's transition (priority 10000)
	// stops what was queued with it, so the reload's message goes the
	// update after (PlayerCamera::tick) and the reload plays.
	{
		anim::AnimationInput dropped;
		rig.update(dropped, 1 / 30.0f); // the zoom message goes
		dropped.pulses = 2;
		rig.update(dropped, 1 / 30.0f); // the reload's, an update later
		dropped.pulses = 0;
		int ticks = 0;
		for (int i = 0; i < 150; ++i) {
			rig.update(dropped, 1 / 30.0f);
			ticks += playing("reload") ? 1 : 0;
		}
		CHECK(ticks > 90); // most of its 3.96 s
		input.messages = 4;
		for (int i = 0; i < 10; ++i) {
			rig.update(input, 1 / 30.0f);
		}
	}
	// The weapon's first mesh node sits in front of and below the camera
	// bone (the rifle at the right hip of the view).
	const int mesh1 = skeleton->find("mesh1");
	CHECK(mesh1 >= 0);
	if (mesh1 >= 0) {
		const anim::Matrix& m = rig.pose()[static_cast<size_t>(mesh1)];
		const anim::Matrix& c = rig.pose()[0];
		CHECK(m[14] > c[14] && m[13] < c[13] + 0.05f);
	}
	// Sprinting (1p_sprint from 5.5 m/s) moves the camera bone a few
	// centimetres off its setup pose: the view's bob (cameraBoneDelta).
	{
		anim::AnimationInput sprint;
		sprint.speed = 6;
		sprint.forward = 1;
		sprint.movementSeconds = 1 / 30.0f;
		float most = 0;
		for (int i = 0; i < 120; ++i) {
			rig.update(sprint, 1 / 30.0f);
			const anim::Matrix d = anim::cameraBoneDelta(*skeleton, rig.pose(), rig.cameraNode());
			most = std::max(most, std::sqrt(d[12] * d[12] + d[13] * d[13] + d[14] * d[14]));
		}
		CHECK(playing("sprint"));
		CHECK(most > 0.02f && most < 0.1f);
	}
}

TEST(FirstPerson_RenderedPoseBlendsBetweenTicks)
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
	const float dt = 1 / 30.0f;
	anim::AnimationInput sprint;
	sprint.speed = 6;
	sprint.forward = 1;
	sprint.movementSeconds = dt;
	const auto camera = [](const anim::FirstPersonRig& r) {
		const anim::Matrix& m = r.pose()[static_cast<size_t>(r.cameraNode())];
		return std::array<float, 3> {m[12], m[13], m[14]};
	};
	// Halfway between two ticks the bone is halfway between its two poses.
	anim::FirstPersonRig rig(systems, *skeleton, soldier);
	rig.setWeapon(rifle);
	for (int i = 0; i < 40; ++i) {
		rig.update(sprint, dt);
	}
	float moved = 0;
	for (int i = 0; i < 20; ++i) {
		rig.update(sprint, dt);
		rig.blend(1);
		const auto now = camera(rig);
		rig.blend(0);
		const auto before = camera(rig);
		rig.blend(0.5f);
		const auto half = camera(rig);
		for (size_t k = 0; k < 3; ++k) {
			CHECK(near(half[k], (now[k] + before[k]) * 0.5f, 1e-5f));
			moved = std::max(moved, std::fabs(now[k] - before[k]));
		}
	}
	CHECK(moved > 1e-4f); // the bone moves between ticks, so the blend is not a no-op
	// A reset (a thrown weapon's reload) and a weapon change snap: no blend
	// from the old pose.
	rig.resetWeapon();
	rig.update(sprint, dt);
	rig.blend(0);
	const auto reset0 = camera(rig);
	rig.blend(1);
	const auto reset1 = camera(rig);
	for (size_t k = 0; k < 3; ++k) {
		CHECK(near(reset0[k], reset1[k], 1e-6f));
	}
	rig.setWeapon(nullptr);
	rig.setWeapon(rifle);
	rig.update(sprint, dt);
	rig.blend(0);
	const auto swap0 = camera(rig);
	rig.blend(1);
	const auto swap1 = camera(rig);
	for (size_t k = 0; k < 3; ++k) {
		CHECK(near(swap0[k], swap1[k], 1e-6f));
	}
	// Events come from update() only: drawing more frames per tick changes none.
	const auto run = [&](int framesPerTick) {
		anim::FirstPersonRig r(systems, *skeleton, soldier);
		r.setWeapon(rifle);
		std::vector<anim::FirstPersonRig::Event> events;
		for (int i = 0; i < 90; ++i) {
			anim::AnimationInput in = sprint;
			in.pulses = i % 15 == 0 ? 1 : (i == 40 ? 2 : 0);
			r.update(in, dt, &events);
			for (int f = 0; f < framesPerTick; ++f) {
				r.blend(static_cast<float>(f) / static_cast<float>(framesPerTick));
			}
		}
		return events;
	};
	const auto slow = run(1), fast = run(8);
	CHECK(slow.size() == fast.size());
	for (size_t i = 0; i < std::min(slow.size(), fast.size()); ++i) {
		CHECK(slow[i].id == fast[i].id && slow[i].type == fast[i].type && slow[i].weapon == fast[i].weapon);
	}
}

namespace {

// The shipped soldier's and rifle's first-person systems on the first-person
// skeleton (needs $BF2142_GAME_DIR).
struct FirstPersonRifle {
	io::FileManager files;
	Console console {&files};
	anim::AnimationSystems systems {[this](const std::string& path, std::vector<uint8_t>& data) {
		return files.readFile(path, data);
	}};
	const anim::AnimationSystemTemplate* soldier = nullptr;
	const anim::AnimationSystemTemplate* rifle = nullptr;
	const anim::Skeleton* skeleton = nullptr;

	explicit FirstPersonRifle(const char* gameDir)
	{
		files.setRootPath(gameDir);
		files.addSearchPath("mods/bf2142");
		bindMethod(console, "fileManager.mountArchive", ConsoleAccess::Anyone,
			[this](std::string archive, std::string mountPoint, bool) { files.mountArchive(archive, mountPoint); }, 2);
		std::string out;
		console.run("ServerArchives.con", {}, out);
		console.run("ClientArchives.con", {}, out);
		anim::registerAnimationConsoleMethods(console, systems);
		console.run("Objects/Soldiers/Common/Animations/ValueHolders.inc", {}, out);
		const auto load = [&](const std::string& path) { return systems.get(path, [&] { console.run(path, {}, out); }); };
		soldier = load("Objects/Soldiers/Common/Animations/AnimationSystem1p.inc");
		rifle = load("Objects/Weapons/Handheld/eu_ar_rifle/AnimationSystem1p.inc");
		skeleton = systems.skeleton("Objects/Soldiers/Common/Animations/1p_setup.ske");
	}
	bool loaded() const { return soldier && rifle && skeleton; }
};

anim::AnimationInput sprintInput()
{
	anim::AnimationInput sprint;
	sprint.speed = 6;
	sprint.forward = 1;
	sprint.movementSeconds = 1 / 30.0f;
	return sprint;
}

std::array<float, 3> cameraBonePosition(const anim::FirstPersonRig& rig)
{
	const anim::Matrix& m = rig.pose()[static_cast<size_t>(rig.cameraNode())];
	return {m[12], m[13], m[14]};
}

} // namespace

// A frame that ran several ticks (below 30 fps) draws the pose between where
// the last frame left the skeleton and the last tick's, by the alpha that
// spans all the frame's ticks (0x0043f170), as the world's objects are drawn:
// not between the frame's last two ticks. alpha is clamped to -0.1 .. 1.1,
// as the objects' is.
TEST(FirstPerson_MultiTickFrameBlendsFromTheLastDraw)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	FirstPersonRifle data(gameDir);
	CHECK(data.loaded());
	if (!data.loaded()) {
		return;
	}
	const float dt = 1 / 30.0f;
	const anim::AnimationInput sprint = sprintInput();
	anim::FirstPersonRig rig(data.systems, *data.skeleton, data.soldier);
	rig.setWeapon(data.rifle);
	for (int i = 0; i < 40; ++i) {
		rig.update(sprint, dt);
		rig.blend(1);
	}
	float lagged = 0; // how far the last tick's start is from the last draw
	for (int frame = 0; frame < 10; ++frame) {
		rig.blend(1);
		const auto drawn = cameraBonePosition(rig); // where this frame starts
		std::array<float, 3> secondLast {};
		for (int tick = 0; tick < 3; ++tick) {
			secondLast = cameraBonePosition(rig);
			rig.update(sprint, dt);
		}
		const auto last = cameraBonePosition(rig);
		rig.blend(0);
		const auto from = cameraBonePosition(rig);
		rig.blend(1);
		const auto to = cameraBonePosition(rig);
		rig.blend(0.5f);
		const auto half = cameraBonePosition(rig);
		rig.blend(1.1f);
		const auto over = cameraBonePosition(rig);
		rig.blend(3.0f); // clamped to 1.1
		const auto clampedOver = cameraBonePosition(rig);
		rig.blend(-0.1f);
		const auto under = cameraBonePosition(rig);
		rig.blend(-3.0f); // clamped to -0.1
		const auto clampedUnder = cameraBonePosition(rig);
		for (size_t k = 0; k < 3; ++k) {
			CHECK(near(from[k], drawn[k], 1e-6f));
			CHECK(near(to[k], last[k], 1e-6f));
			CHECK(near(half[k], (drawn[k] + last[k]) * 0.5f, 1e-5f));
			CHECK(near(over[k], drawn[k] + (last[k] - drawn[k]) * 1.1f, 1e-5f));
			CHECK(near(clampedOver[k], over[k], 1e-6f));
			CHECK(near(under[k], drawn[k] - (last[k] - drawn[k]) * 0.1f, 1e-5f));
			CHECK(near(clampedUnder[k], under[k], 1e-6f));
			lagged = std::max(lagged, std::fabs(secondLast[k] - drawn[k]));
		}
	}
	CHECK(lagged > 1e-4f); // the frame's earlier ticks moved the bone: the check tells the two apart
}

// A weapon change (or holstering) draws the new weapon's start pose at once:
// a frame drawn before the next tick and the frame after it blend nothing
// from the old weapon's pose.
TEST(FirstPerson_WeaponChangeDrawsTheNewPose)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	FirstPersonRifle data(gameDir);
	CHECK(data.loaded());
	if (!data.loaded()) {
		return;
	}
	const float dt = 1 / 30.0f;
	const anim::AnimationInput sprint = sprintInput();
	anim::FirstPersonRig rig(data.systems, *data.skeleton, data.soldier);
	rig.setWeapon(data.rifle);
	for (int i = 0; i < 40; ++i) {
		rig.update(sprint, dt);
		rig.blend(0.5f);
	}
	const anim::LocalPose setup = anim::setupPose(*data.skeleton);
	const std::vector<anim::Matrix> setupWorld = anim::worldMatrices(*data.skeleton, setup);
	// Equal entries, a degenerate node's infinities and NaNs included (1p_setup.ske
	// has a node whose setup matrix isn't finite until a blend normalises it).
	const auto samePose = [](const std::vector<anim::Matrix>& a, const std::vector<anim::Matrix>& b) {
		if (a.size() != b.size()) {
			return false;
		}
		for (size_t i = 0; i < a.size(); ++i) {
			for (size_t k = 0; k < 16; ++k) {
				if (a[i][k] != b[i][k] && !(std::isnan(a[i][k]) && std::isnan(b[i][k])) && !near(a[i][k], b[i][k], 1e-5f)) {
					return false;
				}
			}
		}
		return true;
	};
	CHECK(!samePose(rig.pose(), setupWorld)); // sprinting: the bones are off their setup pose
	for (const anim::AnimationSystemTemplate* next : {static_cast<const anim::AnimationSystemTemplate*>(nullptr), data.rifle}) {
		rig.setWeapon(next);
		// Drawn before the next tick: the new start pose at any alpha.
		CHECK(samePose(rig.pose(), setupWorld));
		rig.blend(1);
		const std::vector<anim::Matrix> start = rig.pose();
		rig.blend(0);
		CHECK(samePose(rig.pose(), start));
		rig.blend(0.5f);
		CHECK(samePose(rig.pose(), start));
		// The first tick after it snaps: no blend from before the change.
		rig.update(sprint, dt);
		rig.blend(1);
		const std::vector<anim::Matrix> tick = rig.pose();
		rig.blend(0);
		CHECK(samePose(rig.pose(), tick));
		for (int i = 0; i < 10; ++i) {
			rig.update(sprint, dt);
			rig.blend(0.5f);
		}
	}
}

TEST(FirstPerson_FragComesBackInHandAfterTheThrow)
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
	const anim::AnimationSystemTemplate* frag = load("Objects/Weapons/Handheld/Unl_Grenade_Frag/AnimationSystem1p.inc");
	const anim::Skeleton* skeleton = systems.skeleton("Objects/Soldiers/Common/Animations/1p_setup.ske");
	CHECK(soldier && frag && skeleton);
	if (!soldier || !frag || !skeleton) {
		return;
	}
	anim::FirstPersonRig rig(systems, *skeleton, soldier);
	rig.setWeapon(frag);
	const auto playing = [&rig](const std::string& name) {
		const auto names = rig.playing();
		return std::find(names.begin(), names.end(), name) != names.end();
	};
	const auto run = [&](uint32_t messages, uint32_t pulses, int ticks) {
		anim::AnimationInput input;
		input.messages = messages;
		input.pulses = pulses;
		rig.update(input, 1 / 30.0f);
		input.pulses = 0;
		for (int i = 1; i < ticks; ++i) {
			rig.update(input, 1 / 30.0f);
		}
	};
	// FireArm::animationMessages: 8 while cocked (in hand, pin in).
	run(8, 0, 60);
	CHECK(playing("normal_still"));
	// The press: pin out (16), the throw (1) after the pull-back, the launch.
	run(0, 16, 30);
	run(0, 1, 12);
	CHECK(playing("uncocked_fire"));
	run(0, 0, 21);
	// The throw is held (playForever): only the reset gets rid of it.
	run(8, 0, 30);
	CHECK(playing("uncocked_fire"));
	// The reload (autoReload, 0x005e1410) resets the system, cocks the next
	// one and sends its pulse: a new grenade is drawn (deploy), then held.
	rig.resetWeapon();
	run(8, 2, 1);
	CHECK(playing("deploy"));
	CHECK(!playing("uncocked_fire"));
	run(8, 0, 60);
	CHECK(playing("normal_still"));
	CHECK(!playing("uncocked_fire"));
	// The engineer's motion mine: the same ThrownAnimationComp, its placing
	// held by playForever until the reload's reset.
	const anim::AnimationSystemTemplate* mine = load("Objects/Weapons/Handheld/Unl_Mine_Motion/AnimationSystem1p.inc");
	CHECK(mine != nullptr);
	if (!mine) {
		return;
	}
	rig.setWeapon(mine);
	run(8, 0, 60);
	CHECK(playing("still"));
	run(0, 17, 30);
	CHECK(playing("fire"));
	rig.resetWeapon();
	run(8, 2, 120);
	CHECK(!playing("fire"));
	CHECK(playing("still"));
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

// A seat gun between rounds without fire.allowZoomOnROF (0x005d3c40): the
// zoom press is dropped as when busy, but the cycle zooms nothing out (only
// a reload's start does, with zoomOutOnReload).
TEST(FirstPerson_SeatZoomWhileCycling)
{
	anim::ZoomSettings launcher;
	launcher.factors = {0, 0.7f};
	anim::SeatZoom av;
	av.reset(launcher, 1.1f);
	av.step(true, false, true, 1.1f, 1 / 30.0f, false, true);
	CHECK(av.state.level() == 0);
	av.step(true, false, true, 1.1f, 1 / 30.0f);
	CHECK(av.state.level() == 1);
	av.step(false, false, true, 1.1f, 1 / 30.0f, false, true);
	CHECK(av.state.level() == 1); // cycling isn't a reload's start
	launcher.zoomWhenBusy = true;
	anim::SeatZoom tank;
	tank.reset(launcher, 1.1f);
	tank.step(true, false, true, 1.1f, 1 / 30.0f, false, true);
	CHECK(tank.state.level() == 1);
}

// The soldier's view rides the animated camera bone (0x005edeb0): the delta
// is the identity at the setup pose; a bone rolled and lifted in its own
// frame rolls and lifts the view the same way about and along the view's
// axes, whatever the yaw and pitch.
TEST(FirstPerson_ViewRidesTheCameraBone)
{
	anim::Skeleton skeleton;
	skeleton.nodes.push_back({"Camerabone", -1, {0, 0, 0, 1}, {-0.024f, 1.504f, -0.111f}});
	const std::vector<anim::Matrix> setup = anim::worldMatrices(skeleton, anim::setupPose(skeleton));
	const anim::Matrix identity = anim::cameraBoneDelta(skeleton, setup, 0);
	for (int i = 0; i < 16; ++i) {
		CHECK(near(identity[static_cast<size_t>(i)], i % 5 == 0 ? 1.0f : 0.0f));
	}
	const float yaw0 = 0.7f, pitch0 = -0.3f;
	{
		anim::Position eye {10, 20, 30};
		float yaw = yaw0, pitch = pitch0, roll = 0;
		anim::rideCameraBone(identity, eye, yaw, pitch, roll);
		CHECK(near(eye[0], 10) && near(eye[1], 20) && near(eye[2], 30) && near(yaw, yaw0) && near(pitch, pitch0) && near(roll, 0));
	}
	// The bone rolled 2 degrees about its forward (z) and 3 cm up its y.
	const float angle = 2 * 3.14159265f / 180;
	const float h = angle / 2;
	anim::LocalPose pose = anim::setupPose(skeleton);
	pose.rotations[0] = {0, 0, std::sin(h), std::cos(h)};
	pose.positions[0][1] += 0.03f;
	const anim::Matrix delta = anim::cameraBoneDelta(skeleton, anim::worldMatrices(skeleton, pose), 0);
	anim::Position eye {10, 20, 30};
	float yaw = yaw0, pitch = pitch0, roll = 0;
	anim::rideCameraBone(delta, eye, yaw, pitch, roll);
	CHECK(near(yaw, yaw0) && near(pitch, pitch0));
	CHECK(near(std::fabs(roll), angle));
	// Lifted along the view's up (-sy sp, cp, -cy sp).
	const float sy = std::sin(yaw0), cy = std::cos(yaw0), sp = std::sin(pitch0), cp = std::cos(pitch0);
	CHECK(near(eye[0], 10 - 0.03f * sy * sp) && near(eye[1], 20 + 0.03f * cp) && near(eye[2], 30 - 0.03f * cy * sp));
	// The rolled up, rebuilt as render::Camera does from the roll (Rodrigues
	// of the world up about the view axis), is the bone's.
	const float k[3] = {sy * cp, sp, cy * cp};
	const float c = std::cos(roll), s = std::sin(roll);
	float up[3] = {-k[2] * s + k[0] * k[1] * (1 - c), c + k[1] * k[1] * (1 - c), k[0] * s + k[2] * k[1] * (1 - c)};
	float dotK = up[0] * k[0] + up[1] * k[1] + up[2] * k[2];
	for (int i = 0; i < 3; ++i) up[i] -= dotK * k[i];
	const float len = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
	const float axes[3][3] = {{cy, 0, -sy}, {-sy * sp, cp, -cy * sp}, {sy * cp, sp, cy * cp}};
	for (int j = 0; j < 3; ++j) {
		float want = 0;
		for (int r = 0; r < 3; ++r) want += delta[static_cast<size_t>(4 + r)] * axes[r][j];
		CHECK(near(up[j] / len, want, 1e-3f));
	}
}

// The soldier camera's rumble: sin(phase x 2 pi) x 0.3 x nearness degrees.
TEST(FirstPerson_CameraProximityShake)
{
	CHECK(near(anim::cameraProximityShake(0.25f, 1), 0.3f));
	CHECK(near(anim::cameraProximityShake(0.25f, 0.5f), 0.15f));
	CHECK(near(anim::cameraProximityShake(0.75f, 1), -0.3f));
	CHECK(anim::cameraProximityShake(0.4f, 0) == 0);
	CHECK(anim::kCameraProximityRate == 10.0f);
}
