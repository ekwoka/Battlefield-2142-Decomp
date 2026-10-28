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
// The pose (0x008b8330) takes, per bone, the first queued animation that
// moves it and slerps towards each later one by its weight; the result is
// then slerped from the bone's previous pose by the system's weight, which
// is below 1 while a trigger with a fadeInTime fades the system in
// (0x008b9720), times the switch fade of directional bundles. Movement
// bundles of four clips (forward, backward, strafe right, strafe left, as
// addAnimation lists them in reverse) blend a strafe clip with the forward
// or backward one by asin(|forward|) * 2 / pi (0x008c46d0), the backward
// clip half the strafe clip's length ahead (anim_offsetBackwardTime); a
// movement bundle starts where the last one was in its step (the system's
// step phase, 0x4c). LookAroundTrigger blends two clips by one look value
// and plays to a time by another (0x008c3d00); SlopeTrigger leans a
// five-clip bundle to the side by the ground's slope (0x008c4f00).
// CountTrigger takes its first child until the bundle on top has played
// through once, then its second for good (0x008c42b0).
//
// Bundles with useNewSystem (0x008c30a0) fade linearly over their fade
// times and, once no longer queued, play on to the next of their
// forceStopPoints before fading out; while one with stop points plays
// without fading, bundles of other priorities that aren't fading out hold
// still. Their speed key points give a factor the object multiplies into
// its movement (0x008ba420).
//
// A bundle's clips are kept as the original's list: each added first, then
// moved by its start time (0x008bf1f0, 0x008bf280). A bundle with a
// looping clip (0x34) plays its clips one after another: while queued the
// looping clip repeats, and once no longer queued abruptPlayback skips to
// that clip's end (0x008c3360); each clip is drawn until the next has
// faded in (0x008c2e50). Events ("sound:N", "effect:N" at a time in the
// bundle, 0x008bde00) fire as the bundle's time passes them (0x008c3030)
// and go to the object's event handler (slot 0x100) as 0x36 + N or
// 0x2c + N. abruptPlayback restarts a one-shot queued again while it plays
// (0x008c29d0); jumpToLastAnimationAtStop plays the bundle at its own speed
// whatever the trigger's rate (0x008c2950).
//
// A system may have a camera spring (animationSystem.cameraSpring.*,
// 0x008b9670), which the soldier runs for the first-person view
// (0x005ee980: CameraSpringState).
#pragma once

#include "AnimationFiles.h"
#include "Pose.h"
#include "RagDoll.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
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
	// 0x008bde00: "effect:N" (type 2) or "sound:N" (type 1), N read by
	// sscanf after the prefix ("sound:0_first" is sound 0); anything else
	// is type 0 and fires nothing. Names are kept lower case (0x0065b190).
	struct Event {
		std::string name;
		int type = 0;
		int id = 0;
		float startTime = 0;
	};
	enum EventType { NoEvent = 0, SoundEvent = 1, EffectEvent = 2 };
	std::string name;
	// The original's list (0x4): addClip puts a clip first, setClipStart
	// moves it before the first one starting at or after it, so the list is
	// by start time and a bundle's clip index (0x008be470) counts from the
	// front: the last one added is 0.
	std::vector<Entry> clips;
	std::vector<Event> events; // 0x10, by start time the same way
	bool hasLoopingClip = false; // 0x34: set by finish
	float length = 1;         // 0x1c: computed from the clips
	float fadeInTime = 0.2f;  // 0x20
	float fadeOutTime = 0.1f; // 0x24
	float stickAround = 0;    // 0x28
	bool useNewSystem = false;   // 0x2c
	bool blockInput = false;     // 0x2d
	bool jumpToLastAnimationAtStop = false; // 0x2e
	bool useSpeedAsTime = false; // 0x2f
	bool abruptPlayback = false; // 0x30
	bool isLooping = true;       // 0x31
	bool playBackward = false;   // 0x32
	bool playForever = false;    // 0x33

	// The new system's key points: a list of numbers separated by ';'
	// (0x008bffe0 and the three after it), kept with the text it was set
	// from.
	struct KeyPoints {
		std::string text;
		std::vector<float> values;
	};
	KeyPoints forceStopPoints;       // 0x38 / 0x54: fractions of the length
	KeyPoints speedKeyPoints;        // 0x64 / 0xb8: over the length
	KeyPoints fadeOutSpeedKeyPoints; // 0x80 / 0xc8: over the fade out
	KeyPoints fadeInSpeedKeyPoints;  // 0x9c / 0xd8: over the fade in

	void addClip(size_t clip);                       // 0x008bf1f0
	void setClipStart(size_t clip, float startTime); // 0x008bf280
	void addEvent(const std::string& name);          // 0x008bf370
	void removeEvent(const std::string& name);       // 0x008bf420
	void setEventStart(const std::string& name, float startTime); // 0x008bf4d0
	static Event parseEvent(const std::string& name); // 0x008bde00
};

// animationSystem.cameraSpring.* (0x008bbfe0, 0x50 bytes): per axis of the
// view (x right, y up, z forward) how far the first-person rig may swing,
// how much a zoom damps it, how fast it settles, and what pushes it.
struct CameraSpring {
	struct V {
		float x = 0, y = 0, z = 0;
		bool zero() const { return x == 0 && y == 0 && z == 0; }
	};
	V amplitude;   // 0x00: metres at a full swing
	V zoomDamping; // 0x0c
	V stiffness;   // 0x18
	V move;        // 0x24: by the soldier's velocity
	V hit;         // 0x30: kept, not read by the soldier's spring
	V fire;        // 0x3c: by the weapon's recoil
	float look[2] = {0, 0}; // 0x48: by the turn and the pitch
	// 0x008bc390: a weapon's spring over the soldier's: each part the
	// weapon sets (non-zero) replaces the soldier's.
	void overrideBy(const CameraSpring& other);
};

// The soldier's first-person spring (0x005ee980, state at soldier +0x2f4):
// three values in -1..1 that relax by the stiffness (0x005edda0: by
// value^2 x stiffness x dt, at least anim-soldier-spring-clamp 0.001, to
// 0 under 0.0001), pushed by the look, the movement and the recoil. The
// first-person skeleton's root is moved by amplitude x value along the
// view's axes (0x006acfa0, 0x008b8880).
struct CameraSpringState {
	float value[3] = {0, 0, 0};
	struct Input {
		float yawDelta = 0;    // degrees turned this tick (soldier +0x300)
		float pitchDelta = 0;  // degrees pitched this tick (soldier +0x304)
		// The velocity relative to what the soldier stands on (zero under
		// 0.7 m/s when that moves) along the soldier's right, up and
		// forward.
		float right = 0, up = 0, forward = 0;
		float recoil = 0;      // the weapon's kick this tick (0x005861a0)
		// How far the view is zoomed: 0 not, 1 fully (the player's field of
		// view between the default and the weapon's zoomed one).
		float zoom = 0;
	};
	// One tick; returns the rig's offset in view space (x right, y up,
	// z forward).
	CameraSpring::V update(const CameraSpring& spring, const Input& input, float seconds);
};

// 0x008bffe0: split at ';' (empty pieces skipped), each piece read by atof.
Bundle::KeyPoints parseKeyPoints(const std::string& text);
// 0x008c2cd0: the key points as a polyline over 0..1 (evenly spaced);
// 1 with none, the only value with one.
float keyPointValue(const std::vector<float>& points, float fraction);

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
	uint32_t count = 0;        // CountTrigger 0x34 (default 1 there): not read
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
	// The camera spring (+0xc), null without cameraSpring.use 1.
	const CameraSpring* cameraSpring() const { return m_cameraSpring.get(); }
	CameraSpring* cameraSpring() { return m_cameraSpring.get(); }
	void useCameraSpring(bool use); // 0x008b9670

	// After loading (0x008bea30): whether each bundle has a looping clip,
	// bundle lengths from their clips and events (0x008be400, needs the
	// clips' lengths) and trigger priorities (0x008c0f50).
	void finish(const std::function<float(const Clip&)>& clipLength);

private:
	std::string m_path;
	std::vector<Clip> m_clips;
	std::vector<Bundle> m_bundles;
	std::vector<Trigger> m_triggers;
	std::unique_ptr<CameraSpring> m_cameraSpring;
};

// What triggers look at (the object's state, 0x008b9..: pose at 0x4,
// messages at 0x8; from the movement setter 0x008b9ae0 (slot 0x38) the
// direction at 0x14 (x side, y up, z forward), the speed at 0x20, the
// accelerations at 0x2c and the last vector at 0x38).
struct AnimationInput {
	int pose = 0;            // 0 stand, 1 crouch, 2 prone, 3 swim
	uint32_t messages = 0;   // held: 1 fire, 2 reload, 4 zoom...
	uint32_t pulses = 0;     // for one update only
	float side = 0;          // movement direction (0x14), right positive
	float forward = 0;       // movement direction (0x1c), forward positive
	float speed = 0;         // m/s (0x20)
	// The direction's y (0x18), which UpTrigger reads (0x008c4980): not
	// part of the normalised direction. The soldier passes its jump state
	// (0x005eef80, SoldierJumpAnimation): 1 rising in a jump, its vertical
	// speed falling, 0 landing.
	float upSpeed = 0;
	// The time the object passes with its movement (0x008b9ae0's first
	// argument). Over 0 the direction's x and z are smoothed by
	// anim_smoothSet (10 per second, unless one of them swaps sign over
	// 0.9) and the accelerations are taken; 0 sets the direction as given.
	float movementSeconds = 0;
	// The vector an object passes last to the movement setter (0x38, 0x3c,
	// 0x40). TurnTrigger reads the second part (0x008c49e0): the soldier
	// passes its rotational velocity, degrees per second (at most 1499,
	// 0x0058a6d0), times 0.01 (0x005ef250), right positive.
	// LookAroundTrigger blends by the first and plays to a time by the
	// second (0x008c3d00); seats and stationary weapons use it for where
	// the occupant looks (what they pass isn't decoded).
	float turn = 0;
	float lookAroundX = 0;
	float lookAroundY = 0;
	// The ground's slope under the object (slot 0x3c, 0x008b9970, kept at
	// 0x24 and 0x28), which the system smooths by anim_surfaceInterpolate;
	// SlopeTrigger leans its five-clip bundles by it (0x008c4f00).
	float slopeForward = 0;
	float slopeSide = 0;
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

	// 0x008ba420 (slot 0x54): the product of the playing bundles' speed
	// key points; vehicles multiply their movement by it (0x00650e90) and
	// divide the speed they give the system by it (0x0063ce30).
	float speedFactor() const;
	// 0x008b9e00 (slot 0x50): whether a playing bundle has blockInput
	// (0x2d); the walker's legs hold their state while one does
	// (0x00650680).
	bool blocksInput() const;
	// 0x78: the weight of the whole system's pose over what was there
	// before it, below 1 while a trigger with a fadeInTime fades in.
	float weight() const { return m_weight; }
	// 0x4c: where in its step the last movement bundle was (0..1), which
	// the next one starts from (0x008c46d0).
	float stepPhase() const { return m_phase; }

	// The events the bundles passed since the last call (0x008c3030), in
	// order: what the original hands the object's event handler (slot
	// 0x100) as 0x36 + id for sounds and 0x2c + id for effects.
	struct FiredEvent {
		int type = 0; // Bundle::SoundEvent or Bundle::EffectEvent
		int id = 0;
		size_t bundle = 0;
	};
	std::vector<FiredEvent> takeEvents() { return std::exchange(m_events, {}); }
	// The direction the triggers see after smoothing (0x14, 0x18, 0x1c)
	// and the accelerations (0x2c, 0x30, 0x34).
	float direction(int axis) const { return m_direction[axis]; }
	float acceleration(int axis) const { return m_acceleration[axis]; }

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
		float switchFade = 1;    // 0x3c: after the directional clips change
		int loops = 0;           // 0x5c: times it wrapped or ended
		// The new system (useNewSystem).
		bool stopping = false;   // 0x0: playing on to a force stop point
		float stopAt = 0;        // 0x4: that point, a fraction of the length
		float fadeOutLeft = 0;   // 0x8
		float fadeInLeft = 0;    // 0xc
		bool still = false;      // 0x65: advanced by no time this update
		// 0x68: clips by index with their weights, drawn instead of the
		// bundle's clips (SlopeTrigger's layers).
		std::vector<std::pair<int, float>> layers;
		size_t nextEvent = 0;    // 0x14: the first event not yet fired
		float jumpOffset = 0;    // 0x40: how far abruptPlayback skipped
	};
	const std::vector<Instance>& instances() const { return m_instances; }
	// Bundle names playing, by priority (for checks and debugging).
	std::vector<std::string> playing() const;

private:
	bool evaluate(size_t trigger);
	void queueBundles(size_t trigger);
	Instance& queue(size_t bundle, size_t trigger, int priority, float weightTarget, float rate);
	bool advance(Instance& instance, float seconds);
	bool advanceNew(Instance& instance, float seconds);
	void setDirection(Instance& instance, float blend, int strafe, int forwardOrBack) const;
	void setMovement(const AnimationInput& input); // 0x008b9ae0
	void fireEvents(Instance& instance);           // 0x008c3030
	void setFadingTrigger(size_t trigger);
	const Clip* bundleClip(const Bundle& bundle, int index) const;
	int priorityOf(const Trigger& trigger) const;
	float value(const Trigger& trigger) const;

	AnimationSystems& m_systems;
	const AnimationSystemTemplate& m_system;
	std::vector<Instance> m_instances; // by priority
	AnimationInput m_input;
	uint32_t m_previousMessages = 0;
	bool m_started = false;
	bool m_startupPlaying = false; // 0x63
	float m_weight = 1;      // 0x78
	float m_fadeLeft = 0;    // 0x74
	int m_fadingTrigger = -1; // 0x7c
	float m_phase = 0;       // 0x4c
	float m_direction[3] = {0, 0, 0};    // 0x14
	float m_speed = 0;                   // 0x20
	float m_acceleration[3] = {0, 0, 0}; // 0x2c
	std::vector<FiredEvent> m_events;
	float m_slopeForward = 0, m_slopeSide = 0; // 0x24, 0x28
	std::map<uint32_t, bool> m_switches; // 0x98 (CountTrigger: key 1)
	float m_idleTime = 0;    // 0x44: time with one bundle playing
	std::map<size_t, float> m_idleNext; // IdleTrigger 0x3c, per trigger
	uint32_t m_random = 0x759dc150;
	uint32_t nextRandom();
};

// The soldier's up value for its system (0x005ef250 with 0x005eef80; the
// state at soldier +0x2e8 and +0x2e9). Standing on something (the
// response's +0xf9) clears it and gives 0. In the air: rising at
// anim_startJumpAtSpeed (2.5 m/s) or more starts a jump (the jump sound),
// which reads 1 until the landing is seen; falling faster than
// anim_startLandAtSpeed (-0.5 m/s) a ray from the feet (a metre under the
// origin) along the velocity, anim_startLandAtHeight (1 m) long (straight
// down that far when slower than 1 m/s), looks for the ground (objects,
// then the heightmap); found, the landing starts (the land sound) and from
// then on the value is the vertical speed. Otherwise the value is the
// vertical speed while falling, else 0.
struct SoldierJumpAnimation {
	static constexpr float kStartJumpAtSpeed = 2.5f;  // anim_startJumpAtSpeed
	static constexpr float kStartLandAtSpeed = -0.5f; // anim_startLandAtSpeed
	static constexpr float kStartLandAtHeight = 1.0f; // anim_startLandAtHeight
	bool landing = false; // +0x2e8
	bool jumping = false; // +0x2e9
	enum class Sound { None, Jump, Land };
	// `ground(way)`: whether the segment from the feet along `way` meets
	// the ground. `sound`: what the soldier's sound set plays (both are
	// 0x007fd970's sound; the landing's only for 0x007fdcd0's player check).
	float update(bool onGround, const float velocity[3], const std::function<bool(const float way[3])>& ground,
		Sound& sound);
};

} // namespace dice::hfe::anim
