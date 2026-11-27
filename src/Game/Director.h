// --director: an automatic camera director for recording footage of a match
// of bots (not retail: a tool of the rewrite). The local player spectates;
// the director scores each living bot's interest from what the AI and the
// world know (its fire, hits, kills, a target in sight and range, vehicle
// and aircraft fights, control point captures, the Titan assault; idling,
// seats without a target and the spawn lower it), cuts between them with
// minimum and maximum shot lengths, hysteresis and variety, holds on a
// death, and places a smoothed camera: a chase, the bot's own eye, or a
// slow wide orbit. It records no video: an outside screen recorder does.
//
// Three layers, each testable without a window: Observer (the world and
// the bots to Samples), ShotDirector (Samples to cuts; pure) and
// DirectorCamera (a subject's pose to a camera Look; pure, on Cameras.h's
// follow and death views). Director runs them together each tick.
#pragma once

#include "Game/Cameras.h"
#include "Physics/RigidBody.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dice::hfe::game {
class GameWorld;
class BotManager;
}

namespace dice::hfe::game::director {

enum class CameraMode { Chase, FirstPerson, Wide };
const char* modeName(CameraMode mode);

constexpr float kNever = 1e9f;

// What the director knows of a player at a sample.
struct Sample {
	int player = -1;
	std::string name;
	int team = 0;
	bool alive = false;
	physics::Vec3 position; // the soldier's feet (in a vehicle, the vehicle's)
	physics::Vec3 focus;    // where its action is: between it and its target, else itself
	float speed = 0;        // metres a second over the last sample
	uint32_t soldier = 0, vehicle = 0;
	int seat = -1;
	bool flying = false;        // in an aircraft (VCAir, VCHelicopter)
	bool ownTitan = false;      // manning a gun of its own Titan (a fixed, far chase: worth less)
	bool hasTarget = false;     // the AI fights an enemy
	bool enemyInRange = false;  // that enemy is in sight and within its weapon's range
	bool idleBehaviour = false; // the AI's behaviour is Idle (or none)
	bool capturing = false;     // inside an enemy or neutral control point's radius
	bool titanAssault = false;  // gpm_ti: on or close to the enemy Titan
	bool titanCore = false;     // inside the enemy Titan with its core exposed
	bool inSpawn = false;       // just spawned, or idle in its own base
	// Seconds since it last fired, was hit, dealt damage, killed (kNever: not yet).
	float sinceFired = kNever, sinceHit = kNever, sinceDamage = kNever, sinceKill = kNever;
	float idleFor = 0;  // seconds idle (slow and without a target, or an idle behaviour)
	float aliveFor = 0; // seconds since it spawned
	int killer = -1;    // dead: who killed it
};

// The interest of a living sample (higher is more worth watching; 0 for the dead).
float interest(const Sample& sample);
// Fighting now: fire, hits, damage or a kill in the last few seconds, or an enemy in sight and range.
bool engaged(const Sample& sample);

// The world and the bots to samples. observe() each tick (fire, hits and
// kills are short); sample() about every 0.5 s builds the samples.
class Observer {
public:
	Observer(GameWorld& world, const BotManager* bots);
	void observe(float now);
	void sample(float now);
	const std::vector<Sample>& samples() const { return m_samples; }
	const Sample* find(int player) const;

private:
	struct Memory {
		float lastFired = -kNever, lastHit = -kNever, lastDamage = -kNever, lastKill = -kNever;
		float idleSince = -1, aliveSince = 0;
		float hitPoints = -1; // its control object's (soldier or vehicle) last seen
		uint32_t object = 0;
		int kills = 0;
		uint32_t gunShots = 0, hits = 0;
		bool alive = false;
		physics::Vec3 position, sampledPosition;
		float sampledAt = -1;
	};
	GameWorld& m_world;
	const BotManager* m_bots;
	std::map<int, Memory> m_memory;
	std::vector<Sample> m_samples;
};

// What the director follows: the most interesting bot (cutting between them
// by interest), or the kills (--director-mode kills: the killer of the
// followed bot, held until it idles long; see ShotDirector), or the kills
// rotating (--director-mode kills-rotate: the same, but a vehicle or a Titan
// gun on a long streak without taking damage is left; see ShotDirector).
enum class Policy { Interest, Kills, KillsRotate };
const char* policyName(Policy policy);
bool parsePolicy(const std::string& name, Policy& policy);

struct Settings {
	Policy policy = Policy::Interest;
	float minShot = 6;      // seconds a shot lasts at least (an idle subject or a death cuts sooner)
	float maxShot = 20;     // and at most
	float idleCut = 4;      // the subject idle this long: cut early to a better one
	float deathHold = 2;    // the subject died: hold on its death view this long
	float margin = 2;       // a candidate must beat the subject's interest by this much
	float marginShare = 0.25f; // or by this share of it, if more
	int history = 4;        // the last subjects variety avoids
	float variety = 1;      // their penalty (plus varietyShare of their interest), so a fresh face wins when scores are close
	float varietyShare = 0.15f;
	float wideShare = 0.12f;        // of the cuts, wide shots
	float firstPersonShare = 0.35f; // of the infantry cuts, first person
	float sampleEvery = 0.5f;       // seconds between decisions
	// Policy::Kills: a followed killer is left after this long without fire,
	// damage dealt or taken, a kill, an enemy in sight and range or a capture
	// (--director-idle). 25 s: longer than a reload, a dash between cover or
	// the wait for the next enemy to come round (the bots' fights pause 10 to
	// 20 s), shorter than a walk across the map to the next point.
	float followIdle = 25;
	float angleEvery = 12; // and its camera changes (chase, first person) this often, the subject kept
	// Policy::KillsRotate: a followed bot in a vehicle that takes no damage for
	// this long (--director-streak; counted from the follow, its last hit or its
	// getting in) is left ("streak"): a tank or gunship farming kills unopposed
	// is the same shot over and over. A gun of its own Titan, safer still and
	// with one fixed view, is left after titanGunStreak. The bot left is not
	// followed again (as a killer or a first kill, nor picked by interest) for
	// benchFor, unless it is hit first (then it is in a fight worth watching).
	float streak = 40;
	float titanGunStreak = 20;
	float benchFor = 60;
};

// A cut: when, to whom, how, and why. Policy::Interest: "start", "better",
// "idle", "max", "death", "lost", "forced". Policy::Kills adds "killer" (to
// the followed bot's killer), "first-kill" (to a bot that killed, while
// waiting), "idle-long" (off a killer idle Settings::followIdle), "left" (the
// subject left the match) and "angle" (the same subject, another camera);
// Policy::KillsRotate "streak" (off a vehicle on a long unharmed streak).
struct Cut {
	float time = 0;
	int subject = -1, previous = -1;
	CameraMode mode = CameraMode::Chase;
	std::string reason;
	float score = 0;
	bool engaged = false;
};

// The shot director: which bot to watch and how, from the samples.
//
// Policy::Kills: while waiting (the start, or after an idle killer, a
// suicide, a team kill or an environment death) the interest rules pick
// the shots; the first kill anywhere (a living bot of the other team killed
// another) cuts to the killer ("first-kill"), which is then followed: no
// cut for a better bot or at maxShot, its own kills keep it, and every
// angleEvery its camera alternates (chase, first person, now and then wide;
// in a vehicle the chase and a wide orbit in turn). Its death holds deathHold, then the cut
// goes to its killer ("killer"), followed in turn; it is left after
// followIdle without action ("idle-long") or if it leaves ("left"). Kills in
// the same decision: the killer with the highest interest, then the lowest
// player id.
//
// Policy::KillsRotate: Policy::Kills, and a followed bot in a vehicle that
// takes no damage for Settings::streak (a gun of its own Titan:
// titanGunStreak) is left ("streak") for the most interesting other bot,
// and benched (see Settings::benchFor).
class ShotDirector {
public:
	explicit ShotDirector(Settings settings = {}, uint32_t seed = 0x2142u);
	// One decision; the cut when it made one.
	std::optional<Cut> update(float now, const std::vector<Sample>& samples);
	void forceCut() { m_forced = true; }
	int subject() const { return m_subject; }
	CameraMode mode() const { return m_mode; }
	bool holdingDeath() const { return m_deathAt >= 0; }
	// Policy::Kills: following a killer (not waiting).
	bool following() const { return m_following; }
	float shotStart() const { return m_shotStart; }
	const Settings& settings() const { return m_settings; }

private:
	const Sample* best(const std::vector<Sample>& samples, int exclude, float& score) const;
	CameraMode chooseMode(const Sample& sample);
	float random();
	Cut cutTo(float now, const Sample* to, const char* reason);
	// Policy::Kills.
	const Sample* killerOf(const std::vector<Sample>& samples, const Sample& victim) const;
	const Sample* newKill(const std::vector<Sample>& samples);
	Cut follow(float now, const Sample* to, const char* reason);
	std::optional<Cut> keepFollowing(float now, const Sample& subject, const std::vector<Sample>& samples);
	Settings m_settings;
	uint32_t m_seed;
	int m_subject = -1;
	CameraMode m_mode = CameraMode::Chase;
	float m_shotStart = 0;
	float m_deathAt = -1;
	bool m_forced = false;
	std::deque<std::pair<int, uint32_t>> m_recent; // the last subjects (player, its vehicle then)
	bool m_following = false;
	float m_lastActive = 0, m_angleAt = 0;
	std::map<int, bool> m_alive; // each player's at the last decision
	// Policy::KillsRotate.
	bool rotates() const { return m_settings.policy == Policy::KillsRotate; }
	bool benched(const Sample& sample) const;
	float m_now = 0, m_lastThreat = 0;
	std::map<int, float> m_benched; // a player left on a streak: when
	std::map<uint32_t, float> m_benchedVehicles; // and its vehicle (the crew with it)
};

// Statistics over a run's cuts, for the end of --simulate. A shot is the
// time from a cut (not an "angle" one) to the next; a follow is a shot that
// began with "killer" or "first-kill"; a chained follow began with "killer"
// right after a follow (the followed bot was killed, its killer followed).
struct Summary {
	int shots = 0, angles = 0;
	float meanShot = 0, medianShot = 0, longestShot = 0;
	int follows = 0, chained = 0, longestChain = 0; // longestChain: follows linked by "killer" cuts
	float meanFollow = 0, longestFollow = 0, followShare = 0; // followShare: of the time
};
Summary summarize(const std::vector<Cut>& cuts, float end);

// The subject's pose for the camera.
struct Pose {
	physics::Vec3 eye;        // the soldier's eye
	float yaw = 0, pitch = 0; // degrees: its aim (yaw 0 along +z, pitch up positive)
	bool inVehicle = false;
	camera::Frame vehicle;    // the vehicle's frame
	float radius = 1;         // the vehicle's (or the soldier's) bounding radius
	bool flying = false;
	physics::Vec3 velocity;
	physics::Vec3 focus;      // the action's centre (the wide shot's)
};

// The camera for a shot: the chase (Cameras.h's follow behind the aim or
// the vehicle), the eye at the aim, a slow orbit of the action; the death
// view (Cameras.h's) when the subject dies. Aim jitter is smoothed (yaw and
// pitch ease with a time constant a few ticks long), never across a cut.
class DirectorCamera {
public:
	void cut() { m_fresh = true; }
	camera::Look update(CameraMode mode, const Pose& pose, float seconds, const camera::Ground& ground);
	// The subject died: the death camera starts on its body (or wreck).
	void startDeath(const physics::Vec3& body, float radius);
	camera::Look death(const physics::Vec3* body, const physics::Vec3* killer, float seconds, const camera::Ground& ground);
	// The vertical field of view (radians) for a mode.
	static float fov(CameraMode mode);
	const camera::Look& last() const { return m_last; }
	float yaw() const { return m_yaw; }
	float pitch() const { return m_pitch; }

	// The smoothing's time constants, seconds.
	static constexpr float kEyeSmoothing = 0.15f, kChaseSmoothing = 0.35f, kFocusSmoothing = 1.0f;
	static constexpr float kOrbitDegreesPerSecond = 7;

private:
	bool m_fresh = true;
	float m_yaw = 0, m_pitch = 0, m_eyeHeight = 0;
	physics::Vec3 m_focus;
	float m_orbit = 0;
	camera::FollowState m_follow;
	camera::DeathState m_death;
	camera::Look m_last;
};

// The director as main runs it: the observer every tick, a decision every
// Settings::sampleEvery, the camera each tick.
class Director {
public:
	Director(GameWorld& world, const BotManager* bots, Settings settings = {}, uint32_t seed = 0x2142u);
	void tick(float seconds);
	// The view now (false before there is a subject).
	bool view(camera::Look& look, float& fovY) const;
	// Whether the last tick cut (no blending into the new view).
	bool cutLastTick() const { return m_cutLastTick; }
	// What the view leaves out (the subject's soldier and weapon in first person).
	std::vector<uint32_t> hidden() const;
	void forceCut() { m_shots.forceCut(); }
	int subject() const { return m_shots.subject(); }
	CameraMode mode() const;
	const std::vector<Cut>& cuts() const { return m_cuts; }
	const Observer& observer() const { return m_observer; }
	std::function<void(const Cut&)> onCut;
	// Statistics: the decisions made, and those whose subject was engaged.
	int decisions = 0, engagedDecisions = 0;
	float time() const { return m_time; }

private:
	Pose poseOf(const Sample& sample) const;
	camera::Ground ground() const;
	GameWorld& m_world;
	Observer m_observer;
	ShotDirector m_shots;
	DirectorCamera m_camera;
	float m_time = 0, m_nextSample = 0;
	bool m_hasView = false, m_cutLastTick = false, m_dying = false;
	camera::Look m_look;
	float m_fov = 1;
	uint32_t m_body = 0;
	std::vector<Cut> m_cuts;
};

} // namespace dice::hfe::game::director
