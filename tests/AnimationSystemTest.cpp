// Animation systems: building them with console scripts, choosing bundles
// by the object's state, and posing the soldier with the game's systems
// (the last part needs $BF2142_GAME_DIR).
#include "Animation/AnimationConsole.h"
#include "Animation/AnimationSystem.h"
#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "IO/File/FileManager.h"
#include "TestMain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace dice::hfe;

namespace {

bool contains(const std::vector<std::string>& names, const std::string& name)
{
	return std::find(names.begin(), names.end(), name) != names.end();
}

void run(Console& console, const std::string& line)
{
	std::string out;
	console.execute(line, out);
}

// A small system like the soldier's: stand / crouch by pose, still / walk /
// run by speed, and a zoom overlay on a message.
const char* kScript[] = {
	"AnimationSystem.createValueHolder walk",
	"AnimationValueHolder.values 0.1 1.5 1.7",
	"AnimationSystem.createValueHolder run",
	"AnimationValueHolder.values 0.5 3.9 5.8",
	"animationSystem.createBundle stand_still",
	"animationSystem.createBundle stand_walk",
	"animationSystem.createBundle stand_run",
	"animationSystem.createBundle crouch_still",
	"animationSystem.createBundle zoom",
	"animationBundle.isLooping 0",
	"animationSystem.createTrigger Trigger stand_still",
	"animationTrigger.addBundle stand_still",
	"animationSystem.createTrigger MovementTrigger stand_walk",
	"animationTrigger.addBundle stand_walk",
	"animationTrigger.valueHolder walk",
	"animationSystem.createTrigger MovementTrigger stand_run",
	"animationTrigger.addBundle stand_run",
	"animationTrigger.valueHolder run",
	"animationSystem.createTrigger MessageTrigger zoom",
	"animationTrigger.addBundle zoom",
	"animationTrigger.message 4",
	"animationSystem.createTrigger Trigger stand",
	"animationTrigger.addChild stand_still",
	"animationTrigger.addChild stand_walk",
	"animationTrigger.addChild stand_run",
	"animationTrigger.addChild zoom",
	"animationSystem.createTrigger Trigger crouch",
	"animationTrigger.addBundle crouch_still",
	"animationSystem.createTrigger PoseTrigger root",
	"animationTrigger.addChild stand",
	"animationTrigger.addChild crouch",
};

} // namespace

TEST(AnimationSystem_ValueHolderRanges)
{
	anim::ValueHolder run {0.5f, 5.8f, 3.9f};
	CHECK(!run.inRange(0.4f) && run.inRange(0.5f) && run.inRange(5.8f) && !run.inRange(6));
	anim::ValueHolder turnLeft {-0.05f, -1000, 1};
	CHECK(turnLeft.inRange(-1) && !turnLeft.inRange(0) && !turnLeft.inRange(1));
	anim::ValueHolder any {0, 0, 1};
	CHECK(any.inRange(-7) && any.inRange(7));
}

TEST(AnimationSystem_TriggersChooseBundles)
{
	Console console;
	anim::AnimationSystems systems(nullptr);
	anim::registerAnimationConsoleMethods(console, systems);
	const anim::AnimationSystemTemplate* system = systems.get("test.inc", [&] {
		for (const char* line : kScript) {
			run(console, line);
		}
	});
	CHECK(system->triggers().size() == 7);
	// Pre-order from the only top-level trigger: root 1, stand 2, its
	// children 3-6, crouch 7.
	CHECK(system->triggers()[static_cast<size_t>(system->findTrigger("root"))].priority == 1);
	CHECK(system->triggers()[static_cast<size_t>(system->findTrigger("stand_run"))].priority == 5);
	CHECK(system->triggers()[static_cast<size_t>(system->findTrigger("crouch"))].priority == 7);

	anim::AnimationState state(systems, *system);
	anim::AnimationInput input;
	state.update(input, 1 / 30.0f);
	CHECK(state.playing() == std::vector<std::string> {"stand_still"});

	// At 1 m/s both walk and run are in range: run, later in the tree, is
	// on top; still (no condition) stays below them as a base.
	input.speed = 1;
	input.forward = 1;
	state.update(input, 1 / 30.0f);
	CHECK(state.playing() == (std::vector<std::string> {"stand_still", "stand_walk", "stand_run"}));
	input.speed = 3;
	for (int i = 0; i < 15; ++i) {
		state.update(input, 1 / 30.0f);
	}
	CHECK(state.playing() == (std::vector<std::string> {"stand_still", "stand_run"})); // walk faded out
	const float rate = state.instances().back().rate;
	CHECK(std::fabs(rate - 3 / 3.9f) < 1e-4f);

	// A one-shot on a message stays over the looping bundle until it ends.
	input.messages = 4;
	state.update(input, 1 / 30.0f);
	input.messages = 0;
	state.update(input, 1 / 30.0f);
	CHECK(contains(state.playing(), "zoom"));

	// Crouching.
	input.pose = 1;
	input.speed = 0;
	for (int i = 0; i < 60; ++i) {
		state.update(input, 1 / 30.0f);
	}
	CHECK(state.playing() == std::vector<std::string> {"crouch_still"});
}

TEST(AnimationSystem_SoldierAndRifle)
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
	CHECK(systems.findValueHolder("3p_stand_run") >= 0);

	const auto load = [&](const std::string& path) {
		return systems.get(path, [&] { console.run(path, {}, out); });
	};
	const anim::AnimationSystemTemplate* soldier = load("Objects/Soldiers/Common/Animations/AnimationSystem3p.inc");
	const anim::AnimationSystemTemplate* rifle = load("Objects/Weapons/Handheld/eu_ar_rifle/AnimationSystem3p.inc");
	std::fprintf(stderr, "  soldier: %zu clips, %zu bundles, %zu triggers; rifle: %zu, %zu, %zu\n",
		soldier->clips().size(), soldier->bundles().size(), soldier->triggers().size(), rifle->clips().size(),
		rifle->bundles().size(), rifle->triggers().size());
	CHECK(soldier->findTrigger("root") >= 0 && soldier->bundles().size() > 40);
	CHECK(rifle->findTrigger("completeTree") >= 0);
	// The jumps: start, loop and landing in sequence, the landing sound.
	const int jumpIndex = soldier->findBundle("stand_jumpForward");
	CHECK(jumpIndex >= 0);
	if (jumpIndex >= 0) {
		const anim::Bundle& jump = soldier->bundles()[static_cast<size_t>(jumpIndex)];
		CHECK(jump.hasLoopingClip && jump.abruptPlayback && jump.jumpToLastAnimationAtStop && jump.clips.size() == 3);
		CHECK(jump.events.size() == 1 && jump.events[0].type == anim::Bundle::SoundEvent && jump.events[0].id == 0);
		CHECK(jump.clips.size() == 3 && jump.clips[0].startTime == 0 && jump.clips[2].startTime > 2);
	}
	const int turnLeft = soldier->findTrigger("stand_turnLeft");
	CHECK(turnLeft >= 0 && soldier->triggers()[static_cast<size_t>(turnLeft)].type == anim::TriggerType::Turn);

	const anim::Skeleton* skeleton = systems.skeleton("Objects/Soldiers/Common/Animations/3p_setup.ske");
	CHECK(skeleton != nullptr);
	if (!skeleton) {
		return;
	}
	anim::AnimationState body(systems, *soldier);
	anim::AnimationState weapon(systems, *rifle);
	anim::AnimationInput input;
	input.speed = 3.5f;
	input.forward = 1;
	for (int i = 0; i < 30; ++i) {
		body.update(input, 1 / 30.0f);
		weapon.update(input, 1 / 30.0f);
	}
	std::string bodyPlaying, weaponPlaying;
	for (const std::string& name : body.playing()) {
		bodyPlaying += name + " ";
	}
	for (const std::string& name : weapon.playing()) {
		weaponPlaying += name + " ";
	}
	std::fprintf(stderr, "  running: body %s/ weapon %s\n", bodyPlaying.c_str(), weaponPlaying.c_str());
	CHECK(contains(body.playing(), "stand_run"));
	CHECK(contains(weapon.playing(), "stand_run"));

	anim::LocalPose pose = anim::setupPose(*skeleton);
	std::vector<bool> touched;
	body.apply(*skeleton, pose, touched);
	weapon.apply(*skeleton, pose, touched);
	const size_t moved = static_cast<size_t>(std::count(touched.begin(), touched.end(), true));
	std::fprintf(stderr, "  %zu of %zu bones posed\n", moved, skeleton->nodes.size());
	CHECK(moved > skeleton->nodes.size() / 2);
	const auto world = anim::worldMatrices(*skeleton, pose);
	bool finite = true;
	for (const auto& m : world) {
		for (float v : m) {
			finite = finite && std::isfinite(v);
		}
	}
	CHECK(finite);
}

namespace {

// Hand-built systems: bundles of placeholder clips (1 s each once
// finished, so a bundle with clips lasts 1 s; one without keeps length 1).
constexpr float kStep = 0.1f;

bool near(float a, float b, float epsilon = 1e-4f) { return std::fabs(a - b) < epsilon; }

size_t makeBundle(anim::AnimationSystemTemplate& system, const std::string& name, int clips = 0)
{
	const size_t bundle = system.addBundle(name);
	for (int i = 0; i < clips; ++i) {
		const size_t clip = system.addClip(name + std::to_string(i) + ".baf");
		system.bundles()[bundle].addClip(clip);
	}
	return bundle;
}

size_t makeTrigger(anim::AnimationSystemTemplate& system, anim::TriggerType type, const std::string& name,
	const std::vector<size_t>& bundles, const std::vector<size_t>& children = {})
{
	const size_t trigger = system.addTrigger(type, name);
	for (size_t bundle : bundles) {
		system.triggers()[trigger].bundles.push_back(bundle);
	}
	for (size_t child : children) {
		system.triggers()[trigger].children.push_back(child);
		system.triggers()[child].isChild = true;
	}
	return trigger;
}

void finishSystem(anim::AnimationSystemTemplate& system)
{
	system.finish([](const anim::Clip&) { return 1.0f; });
}

const anim::AnimationState::Instance* playingBundle(const anim::AnimationState& state,
	const anim::AnimationSystemTemplate& system, const std::string& name)
{
	for (const anim::AnimationState::Instance& instance : state.instances()) {
		if (system.bundles()[instance.bundle].name == name) {
			return &instance;
		}
	}
	return nullptr;
}

} // namespace

TEST(AnimationSystem_KeyPoints)
{
	const anim::Bundle::KeyPoints points = anim::parseKeyPoints("0.3;0.8;");
	CHECK(points.text == "0.3;0.8;");
	CHECK(points.values.size() == 2 && near(points.values[0], 0.3f) && near(points.values[1], 0.8f));
	CHECK(anim::parseKeyPoints(";;1;").values.size() == 1);
	CHECK(anim::parseKeyPoints("").values.empty());
	// A polyline over 0..1; 1 without points.
	CHECK(near(anim::keyPointValue({}, 0.5f), 1));
	CHECK(near(anim::keyPointValue({0.4f}, 0.9f), 0.4f));
	CHECK(near(anim::keyPointValue({1, 0.5f}, 0.5f), 0.75f));
	CHECK(near(anim::keyPointValue({1, 0.5f}, 1), 0.5f));
	CHECK(near(anim::keyPointValue({0, 1, 0}, 0.75f), 0.5f));

	// Set by the mechs' scripts like this.
	Console console;
	anim::AnimationSystems systems(nullptr);
	anim::registerAnimationConsoleMethods(console, systems);
	const anim::AnimationSystemTemplate* system = systems.get("mech.inc", [&] {
		run(console, "animationSystem.createBundle stand_walk");
		run(console, "animationBundle.useNewSystem 1");
		run(console, "animationBundle.forceStopPoints 0.3;0.8;");
		run(console, "animationBundle.fadeOutSpeedKeyPoints 1;0.5;");
		run(console, "animationBundle.fadeInSpeedKeyPoints 0.2;1;");
	});
	const anim::Bundle& walk = system->bundles()[0];
	CHECK(walk.useNewSystem);
	CHECK(walk.forceStopPoints.values == (std::vector<float> {0.3f, 0.8f}));
	CHECK(walk.fadeOutSpeedKeyPoints.values == (std::vector<float> {1, 0.5f}));
	CHECK(walk.fadeInSpeedKeyPoints.text == "0.2;1;");
	CHECK(walk.speedKeyPoints.values.empty());
}

TEST(AnimationSystem_NewSystemFadesAndForceStops)
{
	anim::AnimationSystems systems(nullptr);
	anim::AnimationSystemTemplate system("new.inc");
	const size_t stand = makeBundle(system, "stand");
	const size_t walk = makeBundle(system, "walk");
	{
		anim::Bundle& b = system.bundles()[walk];
		b.useNewSystem = true;
		b.fadeInTime = 0.2f;
		b.fadeOutTime = 0.2f;
		b.forceStopPoints = anim::parseKeyPoints("0.3;0.8;");
		b.fadeInSpeedKeyPoints = anim::parseKeyPoints("0.2;1;");
		b.fadeOutSpeedKeyPoints = anim::parseKeyPoints("1;0.5;");
	}
	const size_t standTrigger = makeTrigger(system, anim::TriggerType::Trigger, "stand", {stand});
	const size_t walkTrigger = makeTrigger(system, anim::TriggerType::Message, "walk", {walk});
	system.triggers()[walkTrigger].message = 1;
	makeTrigger(system, anim::TriggerType::Trigger, "root", {}, {standTrigger, walkTrigger});
	finishSystem(system);

	anim::AnimationState state(systems, system);
	anim::AnimationInput input;
	input.messages = 1;
	// Linear fade in over the bundle's own time; the weight lags one update.
	state.update(input, kStep);
	const anim::AnimationState::Instance* w = playingBundle(state, system, "walk");
	CHECK(w && near(w->weight, 0) && near(w->fadeInLeft, 0.1f) && near(w->time, 0.1f));
	// The fade-in key points over the fade in: 0.2 -> 1.
	CHECK(near(state.speedFactor(), 0.6f));
	state.update(input, kStep);
	w = playingBundle(state, system, "walk");
	CHECK(w && near(w->weight, 0.5f) && near(w->fadeInLeft, 0));
	state.update(input, kStep);
	w = playingBundle(state, system, "walk");
	CHECK(w && near(w->weight, 1) && near(w->time, 0.3f));
	CHECK(near(state.speedFactor(), 1)); // no speed key points

	// Released at 0.35 of its length: it plays on to the stop at 0.8 while
	// the stand bundle (another priority) is held still.
	state.update(input, 0.05f);
	input.messages = 0;
	const float standTime = playingBundle(state, system, "stand")->time;
	for (int i = 0; i < 4; ++i) {
		state.update(input, kStep);
	}
	w = playingBundle(state, system, "walk");
	CHECK(w && w->stopping && near(w->stopAt, 0.8f) && near(w->time, 0.75f) && near(w->weight, 1));
	CHECK(near(playingBundle(state, system, "stand")->time, standTime));
	// Past the stop it fades out (in its own time), and the others run again.
	state.update(input, kStep);
	w = playingBundle(state, system, "walk");
	CHECK(w && !w->stopping && near(w->fadeOutLeft, 0.2f) && w->loops == 1);
	CHECK(near(state.speedFactor(), 1)); // fade-out key points at 0
	state.update(input, kStep);
	w = playingBundle(state, system, "walk");
	CHECK(w && near(w->weight, 1) && near(w->fadeOutLeft, 0.1f));
	CHECK(near(state.speedFactor(), 0.75f));
	CHECK(playingBundle(state, system, "stand")->time > standTime);
	state.update(input, kStep); // the fade out runs out: dropped
	CHECK(state.playing() == std::vector<std::string> {"stand"});

	// Without force stop points a bundle no longer queued fades out at once.
	system.bundles()[walk].forceStopPoints = {};
	anim::AnimationState plain(systems, system);
	input.messages = 1;
	for (int i = 0; i < 4; ++i) {
		plain.update(input, kStep);
	}
	input.messages = 0;
	plain.update(input, kStep);
	w = playingBundle(plain, system, "walk");
	CHECK(w && near(w->fadeOutLeft, 0.2f) && near(w->time, 0.4f));
}

TEST(AnimationSystem_CountTrigger)
{
	// The sentry gun's: open once, then the rest for good.
	anim::AnimationSystems systems(nullptr);
	anim::AnimationSystemTemplate system("count.inc");
	const size_t open = makeBundle(system, "open");
	system.bundles()[open].isLooping = false;
	system.bundles()[open].fadeInTime = 0;
	system.bundles()[open].fadeOutTime = 0;
	system.bundles()[open].stickAround = 0.1f;
	const size_t stand = makeBundle(system, "stand");
	const size_t openTrigger = makeTrigger(system, anim::TriggerType::Trigger, "open", {open});
	const size_t standTrigger = makeTrigger(system, anim::TriggerType::Trigger, "still", {stand});
	const size_t idle = makeTrigger(system, anim::TriggerType::Count, "idle", {}, {openTrigger, standTrigger});
	makeTrigger(system, anim::TriggerType::Trigger, "root", {}, {idle});
	finishSystem(system);

	anim::AnimationState state(systems, system);
	anim::AnimationInput input;
	for (int i = 0; i < 9; ++i) {
		state.update(input, 0.125f);
	}
	CHECK(state.playing() == std::vector<std::string> {"open"});
	CHECK(state.instances()[0].loops == 1); // ended, sticking around
	state.update(input, 0.125f);
	CHECK(state.playing() == std::vector<std::string> {"stand"});
	for (int i = 0; i < 20; ++i) {
		state.update(input, 0.125f);
	}
	CHECK(state.playing() == std::vector<std::string> {"stand"});
}

TEST(AnimationSystem_TriggerFadeInWeight)
{
	anim::AnimationSystems systems(nullptr);
	anim::AnimationSystemTemplate system("fade.inc");
	const size_t a = makeBundle(system, "a");
	const size_t b = makeBundle(system, "b");
	const size_t ta = makeTrigger(system, anim::TriggerType::Message, "a", {a});
	system.triggers()[ta].message = 1;
	system.triggers()[ta].fadeInTime = 1;
	const size_t tb = makeTrigger(system, anim::TriggerType::Message, "b", {b});
	system.triggers()[tb].message = 2;
	system.triggers()[tb].fadeInTime = 0.5f;
	makeTrigger(system, anim::TriggerType::Trigger, "root", {}, {ta, tb});
	finishSystem(system);

	anim::AnimationState state(systems, system);
	anim::AnimationInput input;
	CHECK(near(state.weight(), 1));
	input.messages = 1;
	state.update(input, 0.25f);
	CHECK(near(state.weight(), 0.25f));
	state.update(input, 0.25f);
	CHECK(near(state.weight(), 0.5f));
	// The same trigger again doesn't restart the fade; another one does.
	input.messages = 2;
	state.update(input, 0.25f);
	CHECK(near(state.weight(), 0.5f));
	state.update(input, 0.25f);
	CHECK(near(state.weight(), 1));
	input.messages = 1;
	state.update(input, 0.5f);
	CHECK(near(state.weight(), 0.5f));
}

TEST(AnimationSystem_FootPhaseAndDirections)
{
	anim::AnimationSystems systems(nullptr);
	anim::ValueHolder& walkHolder = systems.createValueHolder("walk");
	walkHolder.min = 0.1f;
	walkHolder.normal = 1;
	walkHolder.max = 2;
	anim::ValueHolder& runHolder = systems.createValueHolder("run");
	runHolder.min = 2;
	runHolder.normal = 4;
	runHolder.max = 10;
	anim::AnimationSystemTemplate system("feet.inc");
	const size_t leftBack = makeBundle(system, "leftFootBack");
	const size_t rightBack = makeBundle(system, "rightFootBack");
	const size_t walk = makeBundle(system, "walk", 4);
	const size_t run = makeBundle(system, "run", 4);
	system.bundles()[walk].fadeInTime = 0.5f;
	const size_t still = makeTrigger(system, anim::TriggerType::Movement, "still", {leftBack, rightBack});
	const size_t walkTrigger = makeTrigger(system, anim::TriggerType::Movement, "walk", {walk});
	system.triggers()[walkTrigger].valueHolder = systems.findValueHolder("walk");
	const size_t runTrigger = makeTrigger(system, anim::TriggerType::Movement, "run", {run});
	system.triggers()[runTrigger].valueHolder = systems.findValueHolder("run");
	const size_t moving = makeTrigger(system, anim::TriggerType::Trigger, "moving", {}, {walkTrigger, runTrigger});
	const size_t speedSwitch = makeTrigger(system, anim::TriggerType::SwitchMessage, "speed", {}, {moving, still});
	system.triggers()[speedSwitch].message = 1;
	makeTrigger(system, anim::TriggerType::Trigger, "root", {}, {speedSwitch});
	finishSystem(system);

	anim::AnimationState state(systems, system);
	anim::AnimationInput input;
	// Standing: no value holder and two bundles, the first while the step
	// phase is near 0 or 1, which then becomes anim_startLeftOffset.
	state.update(input, kStep);
	CHECK(state.playing() == std::vector<std::string> {"leftFootBack"});
	CHECK(near(state.stepPhase(), 0.2f));

	// Walking forward: the bundle starts at the step phase and keeps it.
	input.messages = 1;
	input.speed = 1;
	input.forward = 1;
	state.update(input, kStep);
	const anim::AnimationState::Instance* w = playingBundle(state, system, "walk");
	CHECK(w && near(w->time, 0.3f) && near(w->blend, 1) && w->strafe == 3 && w->forwardOrBack == 0);
	CHECK(near(state.stepPhase(), 0.2f)); // its time when queued
	state.update(input, kStep);
	CHECK(near(state.stepPhase(), 0.3f));

	// Backward while the forward clip dominates: the switch fades the
	// bundle's pose in again over its fadeInTime.
	CHECK(near(playingBundle(state, system, "walk")->switchFade, 1));
	input.forward = -1;
	state.update(input, kStep);
	w = playingBundle(state, system, "walk");
	CHECK(w && w->forwardOrBack == 1 && near(w->switchFade, 0.2f));

	// Running: the run bundle starts where walking left off in its step.
	input.forward = 1;
	state.update(input, kStep);
	const float phase = state.stepPhase();
	input.speed = 3;
	state.update(input, kStep);
	const anim::AnimationState::Instance* r = playingBundle(state, system, "run");
	CHECK(r && near(r->time, phase + kStep * 3 / 4));
	CHECK(near(state.stepPhase(), phase));
	// Strafing hard sets the phase to a start offset.
	input.side = 1;
	input.forward = 0;
	state.update(input, kStep);
	CHECK(near(state.stepPhase(), 0.2f));
	input.side = -1;
	state.update(input, kStep);
	CHECK(near(state.stepPhase(), 0.7f));

	// Stopping in the middle of a step: the other foot back.
	input.messages = 0;
	input.speed = 0;
	input.side = 0;
	state.update(input, kStep);
	CHECK(contains(state.playing(), "rightFootBack"));
	CHECK(near(state.stepPhase(), 0.7f));
}

TEST(AnimationSystem_LookAroundAndSlope)
{
	anim::AnimationSystems systems(nullptr);
	anim::ValueHolder& holder = systems.createValueHolder("mech_stand_walk");
	holder.min = 0;
	holder.normal = 6.5f;
	holder.max = 50;
	holder.clampMin = 0.5f;
	holder.clampMax = 2;
	anim::AnimationSystemTemplate system("seat.inc");
	const size_t look = makeBundle(system, "lookAround", 2);
	system.bundles()[look].isLooping = false;
	system.bundles()[look].useSpeedAsTime = true;
	system.bundles()[look].fadeInTime = 0;
	system.bundles()[look].fadeOutTime = 0;
	const size_t slope = makeBundle(system, "slope", 5);
	system.bundles()[slope].useNewSystem = true;
	system.bundles()[slope].fadeInTime = 0;
	const size_t lookTrigger = makeTrigger(system, anim::TriggerType::LookAround, "look", {look});
	const size_t slopeTrigger = makeTrigger(system, anim::TriggerType::Slope, "slope", {slope});
	system.triggers()[slopeTrigger].valueHolder = systems.findValueHolder("mech_stand_walk");
	makeTrigger(system, anim::TriggerType::Trigger, "root", {}, {lookTrigger, slopeTrigger});
	finishSystem(system);

	anim::AnimationState state(systems, system);
	anim::AnimationInput input;
	input.lookAroundX = 1;
	input.lookAroundY = -0.5f;
	input.slopeSide = -1;
	input.speed = 1;
	state.update(input, kStep);
	const anim::AnimationState::Instance* l = playingBundle(state, system, "lookAround");
	CHECK(l && near(l->blend, 1) && l->strafe == 0 && l->forwardOrBack == 1);
	CHECK(near(l->time, 0.25f)); // useSpeedAsTime: the rate is the time
	// Slope: the rate clamped to the holder's clamp, the level clip and the
	// side lean by the smoothed slope.
	const anim::AnimationState::Instance* s = playingBundle(state, system, "slope");
	CHECK(s && near(s->rate, 0.5f));
	CHECK(s->layers.size() == 2 && s->layers[0].first == 0 && near(s->layers[0].second, 1));
	CHECK(s->layers[1].first == 2 && near(s->layers[1].second, 0.2f));
	input.speed = 26;
	state.update(input, kStep);
	s = playingBundle(state, system, "slope");
	CHECK(near(s->rate, 2) && near(s->layers[1].second, 0.36f));
}

TEST(AnimationSystem_MechWalksToAStop)
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
	const std::string path = "Objects/Vehicles/us_heavy_mech/AnimationSystem.inc";
	const anim::AnimationSystemTemplate* mech = systems.get(path, [&] { console.run(path, {}, out); });
	const int walkIndex = mech->findBundle("stand_walk");
	CHECK(walkIndex >= 0);
	if (walkIndex < 0) {
		return;
	}
	const anim::Bundle& walk = mech->bundles()[static_cast<size_t>(walkIndex)];
	CHECK(walk.useNewSystem && walk.forceStopPoints.values.size() == 2 && walk.clips.size() == 5);

	// Occupied (2048), standing, walking (128) at its normal speed.
	anim::AnimationState state(systems, *mech);
	anim::AnimationInput input;
	input.messages = 2048 | 128;
	input.speed = 6.5f;
	input.forward = 1;
	state.update(input, 0.05f);
	CHECK(contains(state.playing(), "stand_walk"));
	const float starting = state.speedFactor();
	for (int i = 0; i < 10; ++i) {
		state.update(input, 0.05f);
	}
	std::fprintf(stderr, "  mech walking: speed factor %.3f fading in, %.3f walking\n", starting, state.speedFactor());
	CHECK(starting < 1 && near(state.speedFactor(), 1));
	// Let go: the walk plays on to its next stop point, then fades out.
	input.messages = 2048;
	int updates = 0;
	while (contains(state.playing(), "stand_walk") && updates < 200) {
		state.update(input, 0.05f);
		++updates;
	}
	std::string playing;
	for (const std::string& name : state.playing()) {
		playing += name + " ";
	}
	std::fprintf(stderr, "  walk gone after %d updates; playing %s\n", updates, playing.c_str());
	CHECK(updates < 200 && contains(state.playing(), "stand_slope"));
}

// UpTrigger and TurnTrigger as the factory (0x008c1170) builds them: Up
// reads the direction's y (or its acceleration) with useDirection's
// absolute value and signed rate (the ladders' climb), Turn the turn value
// and restarts the step at the left foot.
TEST(AnimationSystem_UpAndTurnTriggers)
{
	anim::AnimationSystems systems(nullptr);
	anim::ValueHolder& climb = systems.createValueHolder("climb");
	climb.min = 0.01f;
	climb.normal = 1;
	climb.max = 10;
	anim::ValueHolder& turnLeft = systems.createValueHolder("turnLeft");
	turnLeft.min = -0.05f;
	turnLeft.normal = 1;
	turnLeft.max = -1000;
	anim::ValueHolder& braking = systems.createValueHolder("braking");
	braking.min = -3;
	braking.normal = 1;
	braking.max = -5;
	anim::AnimationSystemTemplate system("up.inc");
	const size_t climbBundle = makeBundle(system, "climb");
	const size_t turnBundle = makeBundle(system, "turnLeft");
	const size_t brakeBundle = makeBundle(system, "brake");
	const size_t up = makeTrigger(system, anim::TriggerType::Up, "climb", {climbBundle});
	system.triggers()[up].valueHolder = systems.findValueHolder("climb");
	system.triggers()[up].useDirection = true;
	const size_t turn = makeTrigger(system, anim::TriggerType::Turn, "turnLeft", {turnBundle});
	system.triggers()[turn].valueHolder = systems.findValueHolder("turnLeft");
	const size_t brake = makeTrigger(system, anim::TriggerType::Forward, "brake", {brakeBundle});
	system.triggers()[brake].valueHolder = systems.findValueHolder("braking");
	system.triggers()[brake].triggerOnAcceleration = true;
	makeTrigger(system, anim::TriggerType::Trigger, "root", {}, {up, turn, brake});
	finishSystem(system);

	anim::AnimationState state(systems, system);
	anim::AnimationInput input;
	input.upSpeed = -0.5f; // climbing down: |y| in range, played backwards
	input.turn = 5;        // a right turn: not TurnLeft's, and not Up's value
	state.update(input, kStep);
	const anim::AnimationState::Instance* climbing = playingBundle(state, system, "climb");
	CHECK(climbing && near(climbing->rate, -0.5f));
	CHECK(!playingBundle(state, system, "turnLeft"));
	// Turning left at 90 degrees a second (0.9 after the soldier's 0.01).
	input.upSpeed = 0;
	input.turn = -0.9f;
	state.update(input, kStep);
	const anim::AnimationState::Instance* turning = playingBundle(state, system, "turnLeft");
	CHECK(turning && near(turning->rate, 0.9f));
	CHECK(near(state.stepPhase(), 0.2f)); // anim_startLeftOffset
	// Accelerations: the old speed x direction less the new, per second.
	input.turn = 0;
	input.forward = 1;
	input.speed = 2;
	input.movementSeconds = 0.5f;
	state.update(input, kStep);
	CHECK(near(state.acceleration(2), -4));
	CHECK(playingBundle(state, system, "brake") != nullptr);
}

// 0x008b9ae0: with a time the direction eases towards the new one by
// anim_smoothSet, except when it swaps sign past 0.9.
TEST(AnimationSystem_MovementSmoothing)
{
	anim::AnimationSystems systems(nullptr);
	anim::AnimationSystemTemplate system("smooth.inc");
	makeTrigger(system, anim::TriggerType::Trigger, "root", {makeBundle(system, "still")});
	finishSystem(system);
	anim::AnimationState state(systems, system);
	anim::AnimationInput input;
	input.speed = 2;
	input.forward = 1;
	input.movementSeconds = 0.05f;
	state.update(input, 0.05f);
	CHECK(near(state.direction(2), 1) && near(state.direction(0), 0));
	input.side = 0.6f;
	input.forward = 0.8f;
	state.update(input, 0.05f); // halfway: (0.3, 0.9), normalised
	const float length = std::sqrt(0.3f * 0.3f + 0.9f * 0.9f);
	CHECK(near(state.direction(0), 0.3f / length) && near(state.direction(2), 0.9f / length));
	input.side = 0;
	input.forward = -1; // reversing: at once
	state.update(input, 0.05f);
	CHECK(near(state.direction(2), -1) && near(state.direction(0), 0));
	input.movementSeconds = 0; // no time: as given
	input.side = 1;
	input.forward = 0;
	state.update(input, 0.05f);
	CHECK(near(state.direction(0), 1));
}

// A bundle like the soldier's jumps: the start, a looping middle and the
// landing by start time, an effect at once and the landing sound late.
// While queued the middle repeats; once not, abruptPlayback skips past it.
TEST(AnimationSystem_EventsAndClipSequence)
{
	Console console;
	anim::AnimationSystems systems(nullptr);
	anim::registerAnimationConsoleMethods(console, systems);
	anim::ValueHolder& jumpHolder = systems.createValueHolder("jump");
	jumpHolder.min = 0.25f;
	jumpHolder.normal = 2.5f;
	jumpHolder.max = 8;
	anim::AnimationSystemTemplate* system = systems.get("jump.inc", [&] {
		run(console, "animationSystem.createAnimation end.baf");
		run(console, "animationManager.looping 0");
		run(console, "animationManager.length 0.6");
		run(console, "animationManager.fadeInTime 0.2");
		run(console, "animationSystem.createAnimation loop.baf");
		run(console, "animationManager.length 1");
		run(console, "animationSystem.createAnimation start.baf");
		run(console, "animationManager.looping 0");
		run(console, "animationManager.length 0.5");
		run(console, "animationSystem.createBundle jump");
		run(console, "animationBundle.isLooping 0");
		run(console, "animationBundle.abruptPlayback 1");
		run(console, "animationBundle.jumpToLastAnimationAtStop 1");
		run(console, "animationBundle.addAnimation end.baf");
		run(console, "animationBundle.setAnimationStartTime end.baf 2");
		run(console, "animationBundle.addAnimation loop.baf");
		run(console, "animationBundle.setAnimationStartTime loop.baf 0.25");
		run(console, "animationBundle.addAnimation start.baf");
		run(console, "animationBundle.addEvent Sound:0_first");
		run(console, "animationBundle.setEventStartTime sound:0_first 2.4");
		run(console, "animationBundle.addEvent effect:3");
		run(console, "animationSystem.createTrigger UpTrigger root");
		run(console, "animationTrigger.addBundle jump");
		run(console, "animationTrigger.valueHolder jump");
	});
	const anim::Bundle& jump = system->bundles()[0];
	CHECK(jump.clips.size() == 3);
	CHECK(system->clips()[jump.clips[0].clip].path == "start.baf");
	CHECK(system->clips()[jump.clips[1].clip].path == "loop.baf" && near(jump.clips[1].startTime, 0.25f));
	CHECK(system->clips()[jump.clips[2].clip].path == "end.baf" && near(jump.clips[2].startTime, 2));
	CHECK(jump.hasLoopingClip && near(jump.length, 2.6f));
	CHECK(jump.events.size() == 2);
	CHECK(jump.events[0].name == "effect:3" && jump.events[0].type == anim::Bundle::EffectEvent && jump.events[0].id == 3);
	CHECK(jump.events[1].name == "sound:0_first" && jump.events[1].type == anim::Bundle::SoundEvent
		&& jump.events[1].id == 0 && near(jump.events[1].startTime, 2.4f));
	CHECK(anim::Bundle::parseEvent("x sound:2").type == anim::Bundle::SoundEvent);
	CHECK(anim::Bundle::parseEvent("x sound:2").id == 0); // sscanf from the start
	CHECK(anim::Bundle::parseEvent("footstep").type == anim::Bundle::NoEvent);

	anim::AnimationState state(systems, *system);
	anim::AnimationInput input;
	input.upSpeed = 1; // in the air
	float highest = 0;
	for (int i = 0; i < 20; ++i) {
		state.update(input, kStep);
		highest = std::max(highest, state.instances().at(0).time);
	}
	CHECK(near(state.instances().at(0).rate, 1)); // not 1 / 2.5: jumpToLastAnimationAtStop
	CHECK(highest <= 1.25f + 1e-4f);              // the loop repeats from 0.25 to 1.25
	std::vector<anim::AnimationState::FiredEvent> events = state.takeEvents();
	CHECK(events.size() == 1 && events[0].type == anim::Bundle::EffectEvent && events[0].id == 3);
	const float before = state.instances().at(0).time;
	input.upSpeed = 0; // landed: not queued
	state.update(input, kStep);
	CHECK(near(state.instances().at(0).time, 1.25f));
	CHECK(near(state.instances().at(0).jumpOffset, 1.25f - (before + kStep), 1e-3f));
	int updates = 0;
	while (!state.instances().empty() && updates < 40) {
		state.update(input, kStep);
		++updates;
	}
	events = state.takeEvents();
	CHECK(state.instances().empty());
	CHECK(events.size() == 1 && events[0].type == anim::Bundle::SoundEvent && events[0].id == 0);
}

// abruptPlayback: a one-shot queued again while it plays starts over.
TEST(AnimationSystem_AbruptPlayback)
{
	anim::AnimationSystems systems(nullptr);
	anim::AnimationSystemTemplate system("fire.inc");
	const size_t abrupt = makeBundle(system, "abrupt");
	const size_t smooth = makeBundle(system, "smooth");
	for (size_t b : {abrupt, smooth}) {
		system.bundles()[b].isLooping = false;
		system.bundles()[b].fadeInTime = 0;
	}
	system.bundles()[abrupt].abruptPlayback = true;
	const size_t fire = makeTrigger(system, anim::TriggerType::Message, "fire", {abrupt, smooth});
	system.triggers()[fire].message = 1;
	makeTrigger(system, anim::TriggerType::Trigger, "root", {}, {fire});
	finishSystem(system);
	anim::AnimationState state(systems, system);
	anim::AnimationInput input;
	input.pulses = 1;
	state.update(input, kStep);
	input.pulses = 0;
	state.update(input, kStep);
	state.update(input, kStep);
	input.pulses = 1;
	state.update(input, kStep);
	CHECK(near(playingBundle(state, system, "abrupt")->time, kStep));
	CHECK(near(playingBundle(state, system, "smooth")->time, 4 * kStep));
}

TEST(AnimationSystem_CameraSpring)
{
	Console console;
	anim::AnimationSystems systems(nullptr);
	anim::registerAnimationConsoleMethods(console, systems);
	anim::AnimationSystemTemplate* system = systems.get("1p.inc", [&] {
		run(console, "animationSystem.cameraSpring.amplitude 1/1/1"); // no spring yet: ignored
		run(console, "animationSystem.cameraSpring.use 1");
		run(console, "animationSystem.cameraSpring.amplitude 0.02/0.02/0.02");
		run(console, "animationSystem.cameraSpring.zoomDamping 2.5/2.5/0");
		run(console, "animationSystem.cameraSpring.stiffness 10/10/10");
		run(console, "animationSystem.cameraSpring.look 1/1");
		run(console, "animationSystem.cameraSpring.move 2/2/2");
		run(console, "animationSystem.cameraSpring.fire 0/0/150");
	});
	const anim::CameraSpring* spring = system->cameraSpring();
	CHECK(spring != nullptr);
	if (!spring) {
		return;
	}
	CHECK(near(spring->amplitude.x, 0.02f) && near(spring->zoomDamping.x, 2.5f) && near(spring->stiffness.z, 10));
	CHECK(near(spring->look[1], 1) && near(spring->move.y, 2) && near(spring->fire.z, 150) && spring->hit.zero());

	const float dt = 1 / 30.0f;
	anim::CameraSpringState state;
	anim::CameraSpringState::Input kick;
	kick.recoil = 1; // 150 / 30 = 5: clamped
	anim::CameraSpring::V offset = state.update(*spring, kick, dt);
	CHECK(near(state.value[2], 1) && near(offset.z, 0.02f) && near(offset.x, 0));
	// It settles by value^2 x stiffness x dt.
	offset = state.update(*spring, {}, dt);
	CHECK(near(state.value[2], 1 - 10 * dt, 1e-4f));
	// Turning left 3 degrees in a tick swings it right... by -look x turn x dt.
	anim::CameraSpringState looking;
	anim::CameraSpringState::Input turn;
	turn.yawDelta = 3;
	looking.update(*spring, turn, dt);
	CHECK(near(looking.value[0], -0.1f));
	// Strafing right at 3 m/s: x by move.x, and y dips by 0.3 of it.
	anim::CameraSpringState strafing;
	anim::CameraSpringState::Input strafe;
	strafe.right = 3;
	strafing.update(*spring, strafe, dt);
	CHECK(near(strafing.value[0], 0.2f) && near(strafing.value[1], -0.06f));
	// Zoomed in fully: amplitude / (zoomDamping x 6); 0 damping drops it.
	anim::CameraSpringState::Input zoomed = kick;
	zoomed.zoom = 1;
	anim::CameraSpringState fresh;
	offset = fresh.update(*spring, zoomed, dt);
	CHECK(near(offset.z, 0));
	fresh.value[0] = 1;
	offset = fresh.update(*spring, zoomed, 0);
	CHECK(near(offset.x, 0.02f / 15));
	// A weapon's spring replaces only what it sets.
	anim::CameraSpring weapon;
	weapon.fire = {0, 0, 20};
	anim::CameraSpring merged = *spring;
	merged.overrideBy(weapon);
	CHECK(near(merged.fire.z, 20) && near(merged.amplitude.x, 0.02f) && near(merged.look[0], 1));
}

// 0x005eef80: the soldier's up value through a jump.
TEST(AnimationSystem_SoldierJumpValue)
{
	anim::SoldierJumpAnimation jump;
	using Sound = anim::SoldierJumpAnimation::Sound;
	Sound sound;
	bool groundNear = false;
	float lastWay[3] = {0, 0, 0};
	const auto ground = [&](const float way[3]) {
		std::copy(way, way + 3, lastWay);
		return groundNear;
	};
	const float standing[3] = {0, 0, 0};
	CHECK(jump.update(true, standing, ground, sound) == 0 && sound == Sound::None);
	const float slowFall[3] = {0, -0.3f, 0};
	CHECK(near(jump.update(false, slowFall, ground, sound), -0.3f)); // a step down
	const float takeOff[3] = {0, 3, 1};
	CHECK(jump.update(false, takeOff, ground, sound) == 1 && sound == Sound::Jump && jump.jumping);
	const float top[3] = {0, 0.2f, 1};
	CHECK(jump.update(false, top, ground, sound) == 1 && sound == Sound::None);
	const float falling[3] = {0, -5, 0};
	CHECK(jump.update(false, falling, ground, sound) == 1);
	CHECK(near(lastWay[1], -1) && near(lastWay[0], 0)); // a metre along the velocity
	const float slowDrift[3] = {0.5f, -0.6f, 0};
	jump.update(false, slowDrift, ground, sound);
	const float length = std::sqrt(0.25f + 1);
	CHECK(near(lastWay[0], 0.5f / length) && near(lastWay[1], -1 / length)); // under 1 m/s: down
	groundNear = true;
	CHECK(jump.update(false, falling, ground, sound) == 0 && sound == Sound::Land && jump.landing);
	CHECK(jump.update(false, falling, ground, sound) == -5); // landing: the vertical speed
	CHECK(jump.update(true, falling, ground, sound) == 0 && !jump.landing && !jump.jumping);
}
