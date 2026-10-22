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
