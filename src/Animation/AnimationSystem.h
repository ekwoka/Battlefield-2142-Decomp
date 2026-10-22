// Animation systems: the state machines that choose and blend a skeleton's
// animations (BF2/Animation in DICE's tree; the code at 0x008b9000-0x008c5400
// of BF2142.exe). A soldier template names one for its body
// (animationSystem3P), and so does each weapon, whose animations move the
// arms and upper body over the soldier's.
//
// A system is built by a script (".inc") of console commands:
//   animationSystem.createAnimation <.baf>      a clip; animationManager.*
//                                                sets its looping, length...
//   animationSystem.createBundle <name>         clips played together;
//                                                animationBundle.*
//   animationSystem.createTrigger <type> <name> a node of the decision tree;
//                                                animationTrigger.*
// and value holders (Objects/Soldiers/Common/Animations/ValueHolders.inc)
// give the ranges in which triggers fire.
//
// Each update (0x008baed0) evaluates the trigger "startup" once, then "root"
// and "postRoot" with the object's state (pose, messages such as fire or
// zoom, movement direction and speed). Evaluating a trigger (vtable slot
// 0x48) checks its condition, evaluates its children and queues its bundles
// (slot 0x4c, 0x008bb770) at its priority: the trigger's place in a
// pre-order walk of its tree (0x008c0f50) unless overridePrio says
// otherwise. Queued bundle instances live in a list by priority; each
// update advances them (0x008c3360: time, looping, fade in and out as the
// square of a linear ramp; looping bundles not queued again fade out) and
// drops the finished ones, and idle animations (under an IdleTrigger) once
// the messages change or anything is queued above them.
//
// The pose (0x008b8530) takes, per bone, the first queued animation that
// moves it and slerps towards each later one by its weight. Movement
// bundles of four clips (forward, backward, strafe right, strafe left, as
// addAnimation lists them in reverse) blend a strafe clip with the forward
// or backward one by asin(|forward|) * 2 / pi (0x008c46d0).
//
// Not rewritten yet: the "new system" bundles (useNewSystem, 0x008c30a0:
// force stop points), animation events (sounds), camera springs, foot-phase
// matching between movement bundles, LookAroundTrigger's blend, the system
// weight while a trigger fades in, and CountTrigger.
#pragma once

#include "AnimationFiles.h"
#include "Pose.h"
#include "RagDoll.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::anim {

// AnimationManager.*: one clip (a .baf) and how it plays.
struct Clip {
	std::string path;
	bool looping = true;
	float length = 0;     // seconds; 0 plays at 24 frames per second
	float fadeInTime = 0;
	int noiseFrequency = 0;     // noiceFreq
	float noiseAmplitude = 0;   // noiceAmplitude
};

// AnimationValueHolder.*: a range of a value (speed, turn rate...), and
// the "normal" value at which bundles play at their own speed.
struct ValueHolder {
	float min = 0;    // 0x0
	float max = 0;    // 0x4
	float normal = 1; // 0x8
	uint32_t stopOnMessage = 0; // 0xc
	uint32_t passOnMessage = 0; // 0x10
	float clampMin = 0, clampMax = 0; // 0x14, 0x18
	bool hasClamp = false;

	// 0x008c3820: min <= v <= max (max <= v <= min for negative ranges);
	// always when min == max.
	bool inRange(float value) const;
};

// AnimationBundle.*: clips played together (template 0x008bf660).
struct Bundle {
	struct Entry {
		size_t clip;          // index into the system's clips
		float startTime = 0;
	};
	std::string name;
	std::vector<Entry> clips; // in script order
	float length = 1;         // 0x1c: computed from the clips
	float fadeInTime = 0.2f;  // 0x20
	float fadeOutTime = 0.1f; // 0x24
	float stickAround = 0;    // 0x28
	bool useNewSystem = false;
	bool useSpeedAsTime = false;
	bool isLooping = true;
	bool playBackward = false;
	bool playForever = false;
	bool abruptPlayback = false;
	bool jumpToLastAnimationAtStop = false;
	bool blockInput = false;
};

enum class TriggerType {
	Trigger,
	Pose,
	Idle,
	Count,
	Random,
	Message,
	SwitchMessage,
	Movement,
	Forward,
	Side,
	Up,
	Turn,
	LookAround,
	Slope,
};
bool parseTriggerType(const std::string& name, TriggerType& type);

struct Trigger {
	std::string name;
	TriggerType type = TriggerType::Trigger;
	std::vector<size_t> children; // trigger indices
	std::vector<size_t> bundles;
	int valueHolder = -1;
	float fadeInTime = 0;      // 0x2c
	int overridePrio = -1;     // 0x28
	int priority = 0;          // 0x24
	bool isChild = false;      // 0x30
	bool underIdle = false;    // 0x31: an IdleTrigger above it (set by addChild)
	uint32_t message = 0;      // 0x34 (message triggers)
	bool triggerOnAcceleration = false; // 0x38
	bool useDirection = false; // 0x39
	float idleMin = 5, idleMax = 10, idleNext = 5; // IdleTrigger 0x34-0x3c
	uint32_t count = 0;
	uint32_t switchId = 0;
};

// One system as its script built it.
class AnimationSystemTemplate {
public:
	explicit AnimationSystemTemplate(std::string path) : m_path(std::move(path)) {}
	const std::string& path() const { return m_path; }

	size_t addClip(const std::string& path);
	size_t addBundle(const std::string& name);
	size_t addTrigger(TriggerType type, const std::string& name);
	int findClip(const std::string& path) const;
	int findBundle(const std::string& name) const;
	int findTrigger(const std::string& name) const;

	std::vector<Clip>& clips() { return m_clips; }
	const std::vector<Clip>& clips() const { return m_clips; }
	std::vector<Bundle>& bundles() { return m_bundles; }
	const std::vector<Bundle>& bundles() const { return m_bundles; }
	std::vector<Trigger>& triggers() { return m_triggers; }
	const std::vector<Trigger>& triggers() const { return m_triggers; }

	// After loading: bundle lengths from their clips (0x008be400, needs the
	// clips' lengths) and trigger priorities (0x008c0f50).
	void finish(const std::function<float(const Clip&)>& clipLength);

private:
	std::string m_path;
	std::vector<Clip> m_clips;
	std::vector<Bundle> m_bundles;
	std::vector<Trigger> m_triggers;
};

// What triggers look at (the object's state, 0x008b9..: pose at 0x4,
// messages at 0x8, direction at 0x14, speed at 0x20, vertical speed at
// 0x3c).
struct AnimationInput {
	int pose = 0;            // 0 stand, 1 crouch, 2 prone, 3 swim
	uint32_t messages = 0;   // held: 1 fire, 2 reload, 4 zoom...
	uint32_t pulses = 0;     // for one update only
	float side = 0;          // movement direction, right positive
	float turn = 0;          // turning, radians per second, right positive
	float forward = 0;       // movement direction, forward positive
	float speed = 0;         // m/s
	float upSpeed = 0;       // m/s
};

// The system shared by every object that uses it, plus the global value
// holders and the clip files.
class AnimationSystems {
public:
	using Reader = std::function<bool(const std::string& path, std::vector<uint8_t>& data)>;
	explicit AnimationSystems(Reader reader) : m_reader(std::move(reader)) {}

	// The template of a script, built by `build` the first time (which runs
	// the script with this system active).
	AnimationSystemTemplate* get(const std::string& path, const std::function<void()>& build);
	AnimationSystemTemplate* active() { return m_active; }
	void setActive(AnimationSystemTemplate* system) { m_active = system; }

	ValueHolder& createValueHolder(const std::string& name);
	const ValueHolder* valueHolder(int index) const;
	int findValueHolder(const std::string& name) const;
	ValueHolder* activeValueHolder() { return m_activeHolder; }

	// Console state: what animationManager / animationBundle /
	// animationTrigger commands act on.
	int activeClip = -1;
	int activeBundle = -1;
	int activeTrigger = -1;

	const Animation* animation(const std::string& path);
	const Skeleton* skeleton(const std::string& path);
	// A clip's length in seconds.
	float clipLength(const Clip& clip);
	// The ragdoll built by a script (ragDollInit.con), loaded the first
	// time; null if it can't be.
	const RagDollTemplate* ragDoll(const std::string& path);

private:
	Reader m_reader;
	std::map<std::string, std::unique_ptr<AnimationSystemTemplate>> m_systems;
	AnimationSystemTemplate* m_active = nullptr;
	std::vector<std::pair<std::string, ValueHolder>> m_holders;
	ValueHolder* m_activeHolder = nullptr;
	std::map<std::string, std::unique_ptr<Animation>> m_animations;
	std::map<std::string, std::unique_ptr<Skeleton>> m_skeletons;
	std::map<std::string, std::unique_ptr<RagDollTemplate>> m_ragDolls;
};

// One object's playing system (the runtime state, 0x008bad..).
class AnimationState {
public:
	AnimationState(AnimationSystems& systems, const AnimationSystemTemplate& system);

	// One update of `seconds` with the object's state.
	void update(const AnimationInput& input, float seconds);
	// Plays a named trigger once (special moves, emotes: 0x008b9790).
	bool play(const std::string& trigger);

	// Applies the playing animations to `pose` (local transforms of
	// `skeleton`), after anything already applied.
	void apply(const Skeleton& skeleton, LocalPose& pose, std::vector<bool>& touched) const;

	struct Instance {
		size_t bundle;           // 0x18
		size_t trigger;          // 0x1c
		int priority;
		float time = 0;          // 0x20
		float rate = 1;          // 0x48
		float weightTarget = 1;  // 0x34
		float fade = 0;          // 0x2c
		float weight = 0;        // 0x38
		bool active = false;     // 0x44: queued in this update
		float stickLeft = 0;     // 0x60
		int strafe = -1;         // 0x54: directional clips
		int forwardOrBack = -1;  // 0x58
		float blend = 1;         // 0x50
	};
	const std::vector<Instance>& instances() const { return m_instances; }
	// Bundle names playing, by priority (for checks and debugging).
	std::vector<std::string> playing() const;

private:
	bool evaluate(size_t trigger);
	void queueBundles(size_t trigger);
	Instance& queue(size_t bundle, size_t trigger, int priority, float weightTarget, float rate);
	bool advance(Instance& instance, float seconds);
	int priorityOf(const Trigger& trigger) const;
	float value(const Trigger& trigger) const;

	AnimationSystems& m_systems;
	const AnimationSystemTemplate& m_system;
	std::vector<Instance> m_instances; // by priority
	AnimationInput m_input;
	uint32_t m_previousMessages = 0;
	bool m_started = false;
	float m_idleTime = 0;    // 0x44: time with one bundle playing
	std::map<size_t, float> m_idleNext; // IdleTrigger 0x3c, per trigger
	uint32_t m_random = 0x759dc150;
	uint32_t nextRandom();
};

} // namespace dice::hfe::anim
