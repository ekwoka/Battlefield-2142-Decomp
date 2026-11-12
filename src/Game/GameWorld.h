// The game's simulation of a round: control points and the object
// spawners that put vehicles and emplacements into the world.
//
// In the original these are object classes in BF2142.exe driven by the
// game's per-tick update (docs/engine.md, "A simulation tick"); the object
// spawner is BF2/Game/Objects/Game/ObjectSpawner.cpp (update 0x005af230,
// spawn 0x005aeb80, team change 0x005ae490, round reset 0x005ae580).
// The spawn hands a spawned PlayerControlObject root (interface 0xc4c5)
// the spawner (slot 0xc0, +0x264), the spawner's timeToLive (slot 0xa4,
// +0x25c and the countdown +0x258) and the time (slot 0xb8): GameWorld runs
// its abandoned-vehicle countdown (0x005c8ba0). With holdObject the spawner
// holds its last spawn at the spot (the update's +0x1fc branch): GameWorld
// (holdSpawned) does that part, as it moves bodies.
// The spawn spot is kept clear (0x005aeb80's first half, 0x006f2e90):
// PlayerControlObjects within 10 m that the template's mesh overlaps there
// hold the spawn back, and after five tries they are removed unless a
// soldier is within 7 m (ObjectSpawner::SpotTest, GameWorld::testSpawnSpot).
// The spot's drop onto what is under it (0x005ad8b0, the test's matrix
// only) is GameWorld::dropSpawnSpot. Activation by events: an armoured
// object destroyed with a Radio destroyedGameEvent N sends N + 0x1c to
// every spawner (0x0063c6a0 -> 0x005ad470; ObjectSpawner::gameEvent,
// GameWorld::destroyedGameEvent). useButtonRadius (waiting for a player
// to use the spawner) is set by no shipped level.
#pragma once

#include "Animation/AnimationSystem.h"
#include "Animation/FirstPerson.h"
#include "Animation/Ik.h"
#include "AmbientTriggers.h"
#include "Effects/Effects.h"
#include "Physics/RigidBody.h"
#include "Random.h"
#include "ScriptGame.h"
#include "SoldierControl.h"
#include "Vehicle.h"
#include "Weapons.h"
#include "World/WorldMath.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <array>
#include <deque>
#include <map>
#include <tuple>
#include <optional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dice::hfe::io {
class FileManager;
}
namespace dice::hfe::physics {
class CollisionTemplateManager;
class MaterialManager;
struct CollisionMeshFile;
}

namespace dice::hfe::world {
class Object;
class ObjectManager;
class ObjectTemplate;
class ObjectTemplateManager;
} // namespace dice::hfe::world

namespace dice::hfe::game {

class CombatAreas;

// The death ragdoll's hit push (0x005f0f50): the particle bone for the
// armour's hit bone (1, 2/3 -> 2, 6, 7/8 -> 7, 0xb/0xc/0x2f -> 0x2f, 0xf,
// 0x11 -> 0x10, 0x1f, 0x21 -> 0x20, anything else 0x2f), and the push:
// the hit's direction with its up part x rd_hitUp (none down), at
// rd_hitForce x the modifier (zero for no direction or no force).
int ragDollHitBone(int bone);
physics::Vec3 ragDollHitForce(physics::Vec3 direction, float hitUp, float hitForce, float modifier);

// The game logic's state (slot 0x164); spawners treat team changes before
// the round starts as the round's starting teams.
enum class RoundState { BeforeStart = 3, Playing = 1 };

// An ObjectSpawner template's settings (ObjectSpawner template 0x2c0-0x304,
// defaults from its constructor 0x005af920).
struct SpawnerSettings {
	float minSpawnDelay = 30;
	float maxSpawnDelay = 60;
	float timeToLive = 60;
	float distance = 50;
	int team = 0;
	int nrOfObjectToSpawn = -1; // spawns in total; -1 without limit
	int maxNrOfObjectSpawned = 1;
	float radius = 5;           // a spawned object nearer than this blocks the next
	float useButtonRadius = 0;
	world::Vec3 spawnOffset;
	int controlPointId = -1;
	bool holdObject = false;
	bool teamOnVehicle = false;
	bool paused = false;            // 0x2fa
	bool spawnDelayAtStart = false; // 0x2fb: wait a spawn delay before the first spawn
	bool active = true;             // 0x2fc
	bool forwardTeamChangeToSpawnedVehicles = false;
	int activateOnEvent = -1;   // 0x300: off at the round's start, on at that game event
	int deActivateOnEvent = -1; // 0x304: off at that game event
	std::string objectTemplates[3]; // per team (0 unused): setObjectTemplate

	static SpawnerSettings fromTemplate(const world::ObjectTemplate& objectTemplate);
};

class ObjectSpawner {
public:
	ObjectSpawner(world::Object& object, SpawnerSettings settings);

	world::Object& object() const { return m_object; }
	const SpawnerSettings& settings() const { return m_settings; }
	int team() const { return m_team; }
	float spawnCountdown() const { return m_countdown; }
	// Ids of what the spawner's slots hold (0 when empty).
	const std::vector<uint32_t>& spawned() const { return m_spawned; }
	// What it spawned since the last call (for the time to live).
	std::vector<uint32_t> takeSpawned() { return std::exchange(m_justSpawned, {}); }
	// holdObject (+0x1fc, set from the template's +0x2f8 at each spawn):
	// the last spawn (+0x1b4) is held at the spot until it is driven.
	bool holding() const { return m_holding; }
	void stopHolding() { m_holding = false; }
	uint32_t lastSpawned() const { return m_lastSpawned; }

	// 0x005ae490.
	void setTeam(int team, RoundState state);
	// The spawner's message handler (0x005ad470, slot 0x100) for game event
	// `event` (message event + 0x1c): its activateOnEvent switches it on
	// (+0x1cc), else its deActivateOnEvent off. On again, it spawns as the
	// round's reset left its countdown (at once without spawnDelayAtStart).
	void gameEvent(int event);
	bool active() const { return m_active; }
	// 0x005ae580: back to the round's starting state.
	void reset(Random& random);
	// 0x005af230: one tick of `seconds`.
	void update(float seconds, world::ObjectManager& objects, world::ObjectTemplateManager& templates,
		Random& random);

	// The spawn spot's test (0x005aeb80's first half), which the world
	// answers: of the PlayerControlObjects within 10 m of the spot, those
	// the template's mesh placed there overlaps (0x006f2e90); whether one of
	// them is in use (the 0xc4c5 interface's slot 0x18 (0x005c87b0): +0x14);
	// whether a soldier is within 7 m of the spawner itself.
	struct SpotTest {
		std::vector<uint32_t> blockers;
		bool inUse = false;
		bool soldierNear = false;
	};
	using SpotCheck = std::function<SpotTest(const world::ObjectTemplate& objectTemplate, const world::Vec3& spot,
		const world::Vec3& rotation, const world::Vec3& spawner)>;
	void setSpotCheck(SpotCheck check) { m_spotCheck = std::move(check); }
	// The tries the spot has been blocked (+0x204).
	int blockedTries() const { return m_blockedTries; }

private:
	uint32_t spawn(world::ObjectManager& objects, world::ObjectTemplateManager& templates);
	void startCountdown(Random& random);

	world::Object& m_object;
	SpawnerSettings m_settings;
	int m_team;
	int m_startTeam;
	bool m_active;
	int m_remaining;           // 0x1e4: spawns left, -1 without limit
	uint32_t m_lastSpawned = 0; // 0x1b4
	float m_countdown = 0;      // 0x1b8: spawns when it is not above 0
	std::vector<uint32_t> m_spawned;
	std::vector<uint32_t> m_justSpawned;
	bool m_holding = false; // +0x1fc
	SpotCheck m_spotCheck;
	int m_blockedTries = 0; // +0x204
};

struct ControlPoint {
	world::Object* object;
	int id;   // controlPointId
	int team; // owner
	// The flag the game mode's script raises and lowers (cp_getParam /
	// cp_setParam): whose flag it is, how high (0 bottom, 1 top), how fast.
	int flag = 0;
	float flagHeight = 0;
	float takeOverChangePerSecond = 0;
	int playerId = -1;           // who took it last
	bool allowCapture[3] = {true, true, true}; // allowCaptureByTeam
	// Assault Lines' supply lines (the ControlPoint template, saved by
	// 0x005bc260): the point's group (supplyGroupId, -1 none, +0x2ec), and
	// per team the groups it needs (supplyGroupNeeded <team> <group>
	// <optional>, 0x00503f73 -> template vt+0xc0; vectors of (group,
	// optional) at +0x2f4/+0x304), the supply value it needs
	// (supplyValueNeededTeam<n>, +0x310) and gives (supplyValueTeam<n>,
	// +0x318). cp_getParam (0x0056a010) hands them to gpm_sl.py.
	struct SupplyGroup {
		int group;
		bool optional;
	};
	int supplyGroupId = -1;
	std::vector<SupplyGroup> supplyGroupsNeeded[3];
	int supplyValueNeeded[3] = {};
	int supplyValue[3] = {};
};

// The parachute's room (0x006f2760 for 0x005e45e0): no segment from the
// round's place `from` to one of its mesh's vertices (turned by `turn`)
// `crosses` a soldier LOD.
bool canopyClear(const physics::Vec3& from, const physics::Mat3& turn, const std::vector<physics::Vec3>& vertices,
	const std::function<bool(const physics::Vec3&, const physics::Vec3&)>& crosses);

class GameWorld {
public:
	GameWorld(world::ObjectTemplateManager& templates, world::ObjectManager& objects, uint32_t seed = 0x759dc150);
	~GameWorld();

	// Finds the control points and spawners among the placed objects and
	// gives each spawner its control point's team (before the round starts).
	void startRound();
	// One simulation tick.
	void tick(float seconds);
	// A body left slumped in a vehicle seat (the seat's die bundle), 0 for
	// none.
	uint32_t seatCorpse(uint32_t vehicle, int seat) const;
	// The kits lying on the ground (their objects; drawn with the kit
	// template's geometry.dropGeom), see Kits.cpp.
	std::vector<uint32_t> droppedKits() const;
	// Objects whose position isn't finite or lies over `limit` from the
	// origin in any axis (a physics blow-up), for checks.
	std::vector<uint32_t> objectsOutOfWorld(float limit = 1e5f) const;
	bool isDroppedKit(uint32_t object) const;
	// Whether a dead player's body has landed (0x005eda20): no ragdoll, or
	// one a particle of which has hit something, ragdollLandedDelay (the
	// soldier template's +0x3bc, 4 s) after the death. The corpse's removal
	// countdown (the armour's, 0x00652380) waits for it, or 60 s.
	bool bodyLanded(int player) const;
	// The bodies left after PlayerDeath (see stepCorpses): their objects.
	std::vector<uint32_t> corpses() const;
	// A joined game's dead soldiers (a ghost's Soldier networkable with its
	// ragdoll group, 0x10000): `object` (the ghost's, a soldier template
	// with skeleton3P) gets a network-driven ragdoll on the first update
	// (0x005f0a00 starts it on a client for a soldier with a player,
	// 0x008acc00), each update drives its networked particles (`particles`,
	// as decoded: 0x008acce0), and between updates (and 0.3 s past the
	// last) it simulates on its own. The object then sits at the middle
	// particle, unturned, posed by skeletonPose. False when the object or
	// its skeleton is missing.
	bool driveRemoteRagDoll(uint32_t object, const std::vector<physics::Vec3>& particles);
	// The body is gone (the ghost removed, the soldier revived).
	void removeRemoteRagDoll(uint32_t object);
	const anim::RagDoll* remoteRagDoll(uint32_t object) const;
	// A joined game's soldier ghost's health (its Soldier networkable's 0x40
	// group: the fraction of maxHitPoints) and whether a player is still
	// attached to it (a RemotePlayer's soldier ghost). The client runs the
	// armour's corpse countdown from it as the server does (0x005fcdb0 sets
	// the hit points through the armour's setter, slot 0x38 -> 0x00655460 ->
	// 0x00654120: at 0 or less the armour turns wreck at the client's time,
	// +0x60; the armour's update 0x00654d80 -> 0x00652380 has no server test
	// up to the fade), so the body fades as on the server (stepRemoteWrecks);
	// only the removal (0x00651db0) is the server's, which the client sees
	// as the ghost going. Back above 0 (revived) it is whole again.
	void setRemoteHealth(uint32_t object, float health, bool playerAttached);
	// A joined game's living soldier ghost (another player's, the server's:
	// no player of this round): its third-person body and the weapon in its
	// hands animated from what its networkable says, as the round's own
	// soldiers are by their state (stepAnimation's input: the speed and its
	// direction against the yaw, the turn rate, the fire pulse), so it is
	// drawn posed (skeletonPose) and holding its weapon (an object made here
	// for it, posed by the skeleton's mesh nodes, partTransforms). The
	// pose comes from the soldier networkable's 0x20 (soldier +0x268, the
	// PoseTrigger's), the aim from 0x4 / 0x10; the fire from the player
	// networkable's fire state (player +0x19c, mask 0x40) and its presses
	// (mask 0x4000000): the client runs the weapon in its hands as the
	// exe's client does for a remote player (0x005981a0 sets the fire
	// state, the firearm fires on it): its rounds at the template's rate
	// of fire and fire mode, the magazine (the server's rounds while it
	// fires, 0x00595c60) and its reload when empty, each round the weapon
	// animation's fire pulse (its muzzle effects through the effect
	// events) and the Fire sound (3P), a reload the reload message and the
	// Reload sound.
	struct RemoteSoldierState {
		physics::Vec3 velocity;
		float yaw = 0; // degrees: the look (body yaw plus the head's twist)
		float aimPitch = 0; // degrees, up positive
		std::string weapon; // the template in its hands ("" none)
		bool onGround = true;
		bool fired = false; // a round left (the client's own soldier: its prediction's)
		bool reloading = false; // the client's own
		SoldierPose pose = SoldierPose::Stand;
		bool remoteFire = false; // run the weapon from the fire state below
		bool firing = false;     // the fire state (player +0x19c)
		int firePresses = -1;    // mask 0x4000000's count (-1 unknown)
		int rounds = -1;         // the server's rounds in the magazine while firing (-1 unknown)
		int fireMode = -1;       // 0 single, 1 burst, 2 automatic (-1 the template's first)
		int player = -1;         // whose (the sounds' first person for the local player)
		bool muzzleEffects = true; // off for our own soldier seen from inside
		// On a ladder (the soldier networkable's 0x20000): the ladder's id
		// (1 and up, 0 none) and the place on it (0..1). The client's
		// reader (0x005fcdb0) seats the soldier in a LadderContainer of its
		// own at that ladder (0x0059fbf0) and moves it to the height the
		// place gives (0x0059ec80); the container's update poses it by that
		// height (0x0059f380, as poseOnLadder), its weapon put away.
		int ladder = 0;
		float ladderPosition = 0;
	};
	void animateRemoteSoldier(uint32_t object, const RemoteSoldierState& state, float seconds);
	// A joined game's soldier ghost seated in a vehicle ghost's seat
	// `seat` (EnterVehicle's ghost part): posed by the seat's animation
	// system as poseSeated poses the round's own, with its own look from
	// the networked +0x25c / +0x278 (the seat's occupant state, 0x00605b00,
	// or the soldier's: `bodyYaw` the body's turn from the seat, which
	// 0x00587a30 turns the body by, Ry(+0x25c) x the seat, and the
	// LookAroundTrigger takes / 360). Hidden in a seat without a
	// seatAnimationSystem, its weapon put away. The place is the ghost's
	// (the server's, which carries the seat's mount); the turn is the
	// vehicle object's with the seat's own (a mount's turret turn and the
	// hands' IK aren't networked to the client: not applied).
	// `ducking`: the seat's (its ghost's bit 8, or the root's 0x100000),
	// which lets the hands' grips go (0x005c7cf0 -> the soldier's +0x2ea).
	void animateRemoteSeated(uint32_t object, uint32_t vehicle, int seat, float bodyYaw, float seconds,
		bool ducking = false);
	// A joined client's vehicle ghost with a bundleAnimationSystem (the
	// mechs): the exe's client runs every object's skeleton helper in its
	// own update (the object base's 0x0063d5b0 -> 0x0063d2f0, then
	// 0x0063d470 poses the boneIndex parts), so the walker's system plays
	// here from the ghost's movement as stepObjectAnimation's does from the
	// body's (the speed and its direction against the heading, the turn
	// rate, 0x800 when a seat is taken), and its bone parts are posed
	// (partBone): the legs drawn walking, and the seats on the torso
	// (seatMount's bone) carried by it (animateRemoteSeated).
	// `legMessages`: the walker's legs' messages from the server's MechLegs
	// networkable (0x00650530: 0x80 / 0x100 / 0x400 / 0x200, 0x10 the
	// crouch), -1 when none came.
	// A joined client's object moved by its ghost (the SimpleObject
	// networkable's update, 0x005ffec0: the position, the orientation and
	// the velocity go to the root's physics node): the client's own body
	// of it, if it has one, is put there too, so what reads the body (the
	// seat guns' muzzles, the rounds meeting the vehicle) finds it where
	// the server has it rather than where the client first saw it.
	void placeRemoteBody(uint32_t object, const physics::Vec3& position, const physics::Mat3& rotation,
		const physics::Vec3& velocity);
	// A joined client's ghost of a Titan (a tree holding the team's RCTitan
	// object, 0x005ab390): the exe's client builds the ghost's whole object
	// tree, each node's collision in the broad phase, so its own soldier
	// walks the Titan's decks; here the Titan's body gets the tree's merged
	// soldier and projectile LODs and its cannons' collision parts, as the
	// server's (rebuildTitanMeshes), without the server's Titan logic (its
	// orders, armour, spawn points and carried objects are the server's).
	// False when `object` is no Titan.
	bool addRemoteTitan(uint32_t object);
	bool isRemoteTitan(uint32_t object) const
	{
		return std::any_of(m_remoteTitans.begin(), m_remoteTitans.end(), [object](const Titan& t) { return t.object == object; });
	}
	void animateRemoteVehicle(uint32_t object, const physics::Vec3& velocity, bool occupied, float seconds,
		int legMessages = -1);
	// A joined client's vehicles' part turns (the RotationalBundle
	// networkables of their ghosts, net::RemoteWorld::partPose by the
	// client's object): the turret a remote seat rides and the grips its
	// hands hold turn as the ghosts say (animateRemoteSeated).
	void setRemotePartPose(std::function<bool(uint32_t object, const std::string& part, world::Vec3& offset,
			world::Vec3& rotation)> source)
	{
		m_remotePartPose = std::move(source);
	}
	// A manual reload seen on a joined client (the player networkable's
	// mask 0x10000: its reader 0x005981a0 sets the networkable's +0x280,
	// player +0x28c, which the client's 0x00590e10 takes through player
	// vtable 0x200, 0x0059b340, handing the soldier input message 10, the
	// reload): the weapon in the remote soldier's hands starts its reload
	// (the Reload sound, the reload message) unless it reloads already or
	// its magazine is full.
	void reloadRemoteSoldier(uint32_t object, int serverRounds);
	// A server projectile's ghost on a joined client (a GenericProjectile
	// with a networkable: the server's FireComp makes it, 0x005f31d0, and
	// the client's leaves it to the ghost: 0x005f1440): it flies here as
	// a visual round from the ghost's place and velocity, its ghost object
	// moved with it, corrected by each update (moveRemoteProjectile); it
	// shows its impact where it hits, and its detonation when the ghost
	// goes (endRemoteProjectile) if it hasn't gone off here.
	void launchRemoteProjectile(uint32_t ghostObject, const std::string& projectileTemplate, const physics::Vec3& at,
		const physics::Vec3& velocity);
	void moveRemoteProjectile(uint32_t ghostObject, const physics::Vec3& at, const physics::Vec3* velocity);
	void endRemoteProjectile(uint32_t ghostObject);
	// The StickyProjectile event (0x0050f5d0) on a joined client: the round
	// stops (the ghost's updates no longer move it) and lies at `local`
	// (its place, `up` its normal) in `target`'s frame (the client's object
	// of the networkable it stuck to; the vehicle's position and rotation as
	// its ghost has them), riding it; with target 0 at `local` in the world.
	// False when the round isn't known here.
	bool stickRemoteProjectile(uint32_t ghostObject, uint32_t target, const physics::Vec3& local, const physics::Vec3& up);
	bool remoteProjectileStuck(uint32_t ghostObject) const;
	bool hasRemoteProjectile(uint32_t ghostObject) const;
	// A round of `projectileTemplate` let go at `at` with `velocity`, as
	// `player`'s (-1 none; its team and soldier the round's): it flies,
	// bounces, sticks and goes off as a fired one. False without the template.
	bool launchProjectile(const std::string& projectileTemplate, const physics::Vec3& at, const physics::Vec3& velocity,
		int player = -1);
	// The ghost gone, dead or in a vehicle: its animation and weapon go.
	void removeRemoteSoldier(uint32_t object);
	bool isRemoteSoldier(uint32_t object) const { return m_remoteSoldiers.count(object) != 0; }
	uint32_t remoteSoldierWeapon(uint32_t object) const
	{
		const auto it = m_remoteSoldiers.find(object);
		return it == m_remoteSoldiers.end() ? 0 : it->second.weaponObject;
	}
	// Where the camera is and looks (unit), for the ragdolls' levels of
	// detail (0x008ae120); without one (a server) every body steps at
	// level 0.
	void setCamera(const physics::Vec3& position, const physics::Vec3& forward);
	// The clock the ragdolls' frame budget (ragdoll_clientMaxUpdateTime)
	// is measured with, in seconds (by default the steady clock: the
	// original reads the performance counter, 0x004423f0).
	void setClock(std::function<double()> clock) { m_clock = std::move(clock); }
	// Sounds the game asks for (to be played by the sound layer), taken
	// once: a ragdoll's impact (S_Soldier_RagDoll_Impact and the like).
	struct SoundEvent {
		std::string sound;
		physics::Vec3 position;
	};
	std::vector<SoundEvent> takeSounds() { return std::exchange(m_sounds, {}); }
	// The close-to indications (the player's 0.2 s scan on the server,
	// 0x0059a6c0, from the player's update 0x0059bd80, +0x178 the
	// countdown): objects whose Radio component has
	// closeToIndicationDist (template +0x200; the Titans 250, the mines
	// 25) within that plus the player's controlled object's radius (slot
	// 0x90) of it, measured on the plane with indicationDistance2D
	// (+0x1fc), gathered within the radius + 200 (the collision manager's
	// sphere query, slot 0x44); only the player's team's objects (an
	// object's team interface 0x94c1 slot 0x60 against the player's slot
	// 0xf8) unless the player's ability block (slot 0x2a0) has its +1
	// flag (player +0x30d, cleared by 0x0059ac00; nothing found sets it).
	// The first of a closeToIndicationType (+0x204) to come in posts
	// event 2/0x3e with the player and the type, the last to go 2/0x3f
	// (the list +0x170, map +0x16c); none in range, or the player dead
	// (slot 0x74, 0x0059a5f0), posts 2/0x3f with type -1 once. The event
	// goes to the local game's event manager (0x00a7d06c slot 0x10) for
	// the server's own player, else to the player's client (game server
	// slot 0x88, a PostRemote, 0x00509990). Its consumers (the HUD's
	// handlers 0x007a2100 and 0x007c1d70, for the local player) set the
	// HUD variable CloseToObject<type> (0x0079a0e0; -1: every one there
	// is) to 1 or 0: CloseToObject0 the soldier's mine icon
	// (HudElementsActionIcons.con), CloseToObject1 the APCs' pod-range
	// lights (HudElementsAPCDriver.con).
	struct CloseToEvent {
		int player = -1;
		int type = 0; // closeToIndicationType; -1 every type
		bool enter = false; // 0x3e, else 0x3f
		uint64_t tick = 0;  // when posted (untaken ones go after 2 s)
	};
	// The events for one player (taken once).
	std::vector<CloseToEvent> takeCloseToEvents(int player);
	// What a weapon's sound component hears (DefaultSoundComp, vtable
	// 0x00971b48: its fire 0x005dbdc0, reload 0x005dbfe0 and deploy
	// 0x005dc1d0 handlers), for the sound layer to play its weapon sounds
	// (S_<weapon>_Fire1P / Fire3P...): a round leaving, a reload starting,
	// a weapon taken in hand. `object` is the soldier, or the vehicle a
	// seat's gun is on; at most the last 256 are kept.
	struct WeaponSoundEvent {
		enum Kind { Fire, Reload, Deploy, SwitchFireRate, TriggerClick } kind = Fire;
		int player = -1;
		uint32_t object = 0;
		std::string weapon; // the weapon's template
		physics::Vec3 position;
	};
	std::vector<WeaponSoundEvent> takeWeaponSounds() { return std::exchange(m_weaponSounds, {}); }

	RoundState state() const { return m_state; }
	const std::vector<ControlPoint>& controlPoints() const { return m_controlPoints; }
	const std::vector<std::unique_ptr<ObjectSpawner>>& spawners() const { return m_spawners; }
	// An abandoned vehicle's countdown (0x005c8ba0): seconds of its
	// spawner's timeToLive left, -1 when it has none.
	float timeToLiveLeft(uint32_t object) const;
	// A vehicle's own sprint (the PCO's +0x2d4: the boost; Vehicle::sprint):
	// its stamina (slot 0x108) and whether it sprints (slot 0x100); false
	// for an object without a vehicle model.
	bool vehicleStamina(uint32_t vehicle, float& stamina, bool& sprinting) const;
	// Changes a control point's owner and the teams of its spawners.
	void setControlPointTeam(int controlPointId, int team);
	ControlPoint* controlPointOf(uint32_t object);

	// Events for the game mode's scripts (PlayerSpawn, EnterVehicle,
	// ControlPointChangedOwner...).
	void setEventSink(ScriptEventSink sink) { m_events = std::move(sink); }
	world::ObjectManager& objects() { return m_objects; }
	world::ObjectTemplateManager& templates() { return m_templates; }
	// The local player's soldier and what it holds; 0 while dead.
	uint32_t playerObject() const { return m_player && m_player->alive ? m_player->object : 0; }
	uint32_t playerVehicleObject() const { return m_player && m_player->alive ? m_player->vehicle : 0; }
	uint32_t playerWeaponObject() const { return m_player && m_player->alive ? m_player->weaponObject : 0; }
	uint32_t playerKitObject() const { return m_player && m_player->alive ? m_player->kitObject : 0; }
	int playerTeam() const { return m_player ? m_player->team : 0; }

	// Players: the local one joins with spawnPlayer, others with addPlayer
	// (bots, remote players); ids count up from 0. A player's soldier dies when its
	// armor runs out (PlayerKilled, PlayerDeath for the scripts) and
	// respawns after the spawn time.
	int addPlayer(const std::string& name, int team, bool ai);
	// A hosted round's restart as its countdown begins (0x004c03b0 runs the
	// game logic's reset, slot 0x24 -> 0x004be630 -> GameLogic::reset
	// 0x004ce910, once the players needed are there): the soldiers go (no
	// handler hears it: the game isn't playing), the counts start again
	// and every player in the game waits out the countdown to spawn
	// (0x004bd2a0 sets each player's spawn time, slot 0x100, to its end).
	// The world's part is resetRound.
	void restartPlayers(float spawnIn);
	// The countdown's reset of the world (GameLogic::reset 0x004ce910, after
	// restartPlayers): what the round made goes (the spawned vehicles and
	// Titans with what they carry, rounds and mines, kits on the ground,
	// deployed gadgets, drones, pods, parachutes, corpses, the commander's
	// supply drops and strikes), the control points return to the level's
	// teams and the spawners to their start, so the vehicles spawn anew.
	void resetRound();
	// A player's new name (a joined client's ChangePlayerName, 0x0050bb90:
	// the player's name for the scripts, the scoreboards and the logs). The
	// original renames only when the event's player is the sender's own
	// (+0x2c, then the player manager's slot 0x38); HostedGame checks that
	// before calling this.
	void setPlayerName(int id, const std::string& name);
	// Moves a player to the other team (a joined client's PostRemote 6/7,
	// which the game server hands the game logic's slot 0x60, 0x004b0690):
	// only while it has no soldier (the spawn screen's team buttons); it
	// leaves its squad and the command, and the scripts hear
	// PlayerChangeTeams. False for a living player or a team other than 1, 2.
	bool setPlayerTeam(int player, int team);
	// A spectator (player slot 0x3b8, +0x404; JoinAsSpectator 0x47): the
	// followed-player cycle skips it (0x00615ce0), it may fly the free
	// camera (0x00615290), the scoreboard leaves it out of the teams'
	// lists (0x0047c7a0) and shows the spectator icon (0x0074e940). False
	// for no such player.
	bool setSpectator(int player, bool spectator);
	// A player leaving the game (the game server's destroyPlayer,
	// 0x004acfb0, for a client that disconnected): a living soldier dies
	// where it is (game logic slot 0x54, 0x004c1d20: the death 0x004cd390,
	// out of its vehicle, no one credited) and its body is left as a corpse;
	// it leaves its squad (the squad manager's slot 0x38) and the command
	// (slot 0x70); the scripts hear PlayerDisconnect; then the player
	// manager lets it go (slot 0x14) and its id is free for the next
	// addPlayer. False for no such player.
	bool removePlayer(int player);
	// Puts the player's soldier at a spawn point of its team; false when
	// there is none or it lives already.
	bool spawnSoldier(int player);
	struct PlayerInfo {
		int id = -1;
		std::string name;
		int team = 0;
		bool ai = false;
		bool spectator = false; // player +0x404 (slots 0x3b8 / 0x3bc): setSpectator
		bool alive = false;
		uint32_t soldier = 0, vehicle = 0, kit = 0, weapon = 0;
		// What the host module's pmgr_p_get hands the scripts (0x0056ae00's
		// "vehicle" and "defaultvehicle"): the player's weak handles, the
		// vehicle (+0x80, slot 0x4c) and the default vehicle (+0xcc, slot
		// 0xb4). The death (0x004cd390, which sets the respawn time) gets the
		// player out of its vehicles and clears only the default vehicle: the
		// vehicle handle keeps the soldier, through man down and then as a
		// corpse until the armour's countdown removes it.
		uint32_t heldVehicle = 0, heldSoldier = 0;
		int seat = -1; // in the vehicle (0 the driver's)
		bool driving = false; // in a vehicle's driver seat (a Titan's seats are all guns: drivesSeat)
		float respawnIn = 0; // seconds until the next spawn while dead
		bool manDown = false; // down, a medic may revive it
		int kills = 0, deaths = 0, teamKills = 0;
		// The player who killed it last (-1: none or itself): the player's
		// slot 0x338, which the death camera (0x00614940) and the trace
		// camera (0x00610d20) look towards while that player lives.
		int killer = -1;
		int squad = 0; // 0: none
		bool squadLeader = false;
		bool commander = false;
		bool cloaked = false; // active camouflage on
		int rank = -1; // the stats' rank (player slot 0x1a8 +0x20; setPlayerRank, -1 unset): the name tags' ICRank
		// The player manager's list order (0x004b8b60 appends each player
		// made): 0, 1, ... by creation, not the id (a left player's id is
		// taken again). The scoreboard's ties keep it (menu::scoreboardOrder).
		uint64_t created = 0;
		// When the player was last connected (player +0x1c8 through slot
		// 0x1c0, -1 never: bots, and every player on a client), round
		// seconds; menu::scoreboardOrder.
		double connectedAt = -1;
		// The soldier's own EMP level (its armour's, 0x00652a00 through the
		// player's 0x005921b0 on foot), 0 none.
		int empLevel = 0;
		// Spawned on the squad leader where only a prone soldier fits (the
		// spawn point's +0x18a): the soldier starts lying down.
		bool spawnedProne = false;
		SoldierPose pose = SoldierPose::Stand; // on foot (the input's)
		// Seated, the soldier's own look (0x0058a6d0): the body's turn from
		// the seat (+0x25c) and the head's twist on it (+0x278), degrees.
		float seatBodyYaw = 0, seatTwist = 0;
		// On a ladder: its id (the Ladder's +0x184, 0x0059fb00: 1, 2, ... in
		// creation order; 0 none) and the place on it (0x0059ebf0: the
		// container's height over the ladder's bottom +0x198, over its
		// length +0x18c, within 0..1), the soldier networkable's 0x20000.
		int ladder = 0;
		float ladderPosition = 0;
		float battery = -1; // the ability battery, 0..1 (-1 without a kit)
		// The hit indicator's alpha (player +0x2a4: 1 at a hit, falling 3 a
		// second, slot 12 0x005912c0); the crosshair's HitIndicatorIcon.
		float hitIndicator = 0;
		physics::Vec3 position;  // the soldier's feet
		float yaw = 0;           // degrees, 0 facing +z
		float aimPitch = 0;      // degrees, up positive: the look's (the input's)
		physics::Vec3 velocity;
		std::vector<std::pair<int, std::string>> carried; // (slot, weapon template)
		int inHandSlot = 0;
		float heat = 0; // the weapon in hand's (or the seat's gun's), 0..1
		float stamina = 1; // the sprint's (SprintStamina, the soldier's +0x378), 0..1
		bool sprinting = false; // the sprint on (PCO vtable 0x100)
		bool jumpHold = false;  // a jump's or the rise from prone's timer running (SoldierControl::jumpHold)
		uint32_t gunShots = 0; // rounds the seats' guns have fired (the camera's recoil)
		// The player's fire states (player +0x19c / +0x19d: vtable 0x1d4 /
		// 0x1dc, set from c_PIFire / c_PIAltFire held by the game logic's
		// input, 0x004bda00; the player networkable's masks 0x40 / 0x80) and
		// the presses of the fire (+0x250, counted on each rising edge by
		// 0x0059b260; mask 0x4000000's two bits).
		bool firing = false, altFiring = false;
		uint32_t firePresses = 0;
		// c_PIReload's presses (the server's input handling 0x004bda00 calls
		// player vtable 0x1fc, 0x0059b330, setting +0x28c, which the next
		// snapshot 0x005929f0 turns into mask 0x10000: every weapon group's
		// fields), counted on each press.
		uint32_t reloadPresses = 0;
		// The weapon in hand (group 0): its rounds in the magazine (-1 none),
		// fire mode (FireArm::fireMode) and whether it reloads.
		int roundsInMag = -1;
		int fireMode = 0;
		bool reloading = false;
	};
	std::vector<PlayerInfo> playerList() const;
	bool playerInfo(int player, PlayerInfo& info) const;
	// Radio messages (Radio.cpp). A target channel's value by its name
	// (TCTeam 1 ... TCSquadAndLocalEnemies 0x12, 0x0042d4a0), -1 for none.
	static int targetChannel(const std::string& name);
	// A control object template's vehicleCategory (its interface 0xc4c4
	// slot 0x24, 0x0059d880): VCLand 0, VCSea 1, VCAir 2, VCHelicopter 3,
	// VCNoVehicleCategory 4; the scripts give the names.
	static int vehicleCategory(const world::ObjectTemplate& t);
	// Whether `receiver` hears a message `sender` says on a target channel
	// (0x004a6f70); `value` is the message's team, squad or player for the
	// channels that name one.
	bool hearsRadio(int channel, int sender, int receiver, int value = 0) const;
	// Whether `receiver` gets a player's chat line on a channel: the
	// server's hand-out to each client's player (0x004a93b0) and to its own
	// (0x004a7800): a line from a dead or man-down sender (man down counts
	// alive on the revive channels 0xf / 0x10) only to the dead and the
	// downed, except the server's own channels 0xc / 0xd, then
	// hearsRadio's rule (0x004a6f70: never back to the sender; the
	// distances between the players' held objects, slot 0x4c, the body
	// while it lies).
	bool hearsChat(int channel, int sender, int receiver, int value = 0) const;
	// A radio message said: each hearer gets it (the server's hand-out).
	void radioMessage(int sender, int channel, const std::string& voice, int value = 0);
	struct RadioHeard {
		int sender = -1, receiver = -1, channel = 0;
		std::string voice;
		// An auto controller's player (an id of 256 or more, Player.cpp's
		// +0x3d1; the rewrite's sentries and drones have no player): its
		// team and controlled object, with sender -1.
		int senderTeam = 0;
		uint32_t senderObject = 0;
	};
	// The messages heard since the last call, in order (at most the last
	// 256 kept); `receiver` takes only those whose hearer it picks.
	std::vector<RadioHeard> takeRadioHeard() { return std::exchange(m_radioHeard, {}); }
	std::vector<RadioHeard> takeRadioHeard(const std::function<bool(int receiver)>& receiver);
	// A message only `receiver` hears (the local plays, game slot 0x60
	// 0x0042e820: the team-change and takeable radio, the warnings; a
	// joined game's from the server's RadioMessage events, with their
	// sender).
	void hearRadio(int receiver, int channel, const std::string& voice, int sender = -1)
	{
		if (!voice.empty()) {
			m_radioHeard.push_back({sender, receiver, channel, voice});
		}
	}
	// A control point's allowCaptureByTeam set (CP interface slot 0x34,
	// 0x005bcd00: every set, changed or not; from the scripts' cp_setParam,
	// 0x00569510, or a client's ghost, 0x006084b0), for the takeable radio.
	struct TakeableChange {
		int point = -1; // controlPointId
		int team = 0;
		bool allowed = true;
	};
	void noteTakeable(int point, int team, bool allowed) { m_takeable.push_back({point, team, allowed}); }
	std::vector<TakeableChange> takeTakeableChanges() { return std::exchange(m_takeable, {}); }
	// The player whose soldier (or vehicle) an object is, -1 for none.
	int playerOf(uint32_t object) const;
	// An effect event N (0..9) of a player's held weapon's animation, as
	// its animation system fires it (see startEffectsOn).
	void weaponEffectEvent(int player, int effect);
	// The effect events objects' own animation systems fired since the
	// last call (a walker's foot steps: "effect:N" as event 0x2c + N to the
	// object's handler, slot 0x100, which passes it down its tree, where an
	// EffectBundle whose startOnEffects has bit 9 + N starts: 0x0061a4e0),
	// as those bits by object.
	std::map<uint32_t, uint32_t> takeObjectEffectEvents() { return std::exchange(m_objectEffectEvents, {}); }
	// The vehicles' weapons that fired a round since the last call (their
	// template names, by vehicle): the fire's event 0 to the weapon's
	// children (ObjectEffects: the muzzle bundles, GroundEffectBundles).
	std::map<uint32_t, std::vector<std::string>> takeObjectWeaponFires() { return std::exchange(m_objectWeaponFires, {}); }
	// A joined client's round of another player's seat gun (client::
	// RemoteSeatGuns, from the player's networked fire state as the client's
	// update 0x00590e10 hands it to the control object, 0x004cc7a0): its
	// Fire sound (DefaultSoundComp, 3P) and the fire's event 0 to the gun's
	// children (FireComp 0x005f31d0's third-person list +0x144).
	void remoteGunFired(uint32_t vehicle, const std::string& weapon, int player, const physics::Vec3& at)
	{
		m_weaponSounds.push_back({WeaponSoundEvent::Fire, player, vehicle, weapon, at});
		if (m_weaponSounds.size() > 256) {
			m_weaponSounds.erase(m_weaponSounds.begin(), m_weaponSounds.end() - 256);
		}
		if (std::vector<std::string>& fires = m_objectWeaponFires[vehicle]; fires.size() < 8) {
			fires.push_back(weapon);
		}
	}
	// That round's projectile on the client (FireComp 0x005f31d0 makes it
	// through 0x005f2bb0 when the projectile template has no networkable
	// info; a networked one is the server's ghost): from the gun's muzzle as
	// the client's copy of the vehicle has it, the seat's yaw and pitch
	// bundles turned as their RotationalBundle ghosts say (0x00602070, the
	// muzzle's world matrix through the tree: MultiFireComp 0x005d5fb0 /
	// 0x005f1a20) and the turret or bone part the gun rides; along the
	// barrel at the firearm's velocity plus the vehicle's, a tracer every
	// tracerInterval-th round (`tracerCount`, the fire comp's +0x78), the
	// barrels in turn (`barrel`, +0x1a8). The firearm's deviation isn't
	// networked or seeded: the client's own firearm strays the round
	// (0x005f5540 on the client's SimpleDeviationComp state, its own random
	// numbers), `deviation` here (degrees, the caller's build-up:
	// RemoteSeatGuns), as fireSeatWeapon does on the server. False when
	// nothing was made; `muzzle` gets the round's start either way the gun
	// is known.
	bool fireRemoteSeatRound(uint32_t vehicle, int seat, const std::string& weapon, const physics::Vec3& velocity,
		int& barrel, int& tracerCount, physics::Vec3* muzzle = nullptr, float deviation = 0);
	// The commanders' strike firearms that fired since the last call: the
	// asset object, its firearm (its template name) and where 0x005a0bf0
	// moved it (orbitalBombardmentHeight over the target). The fire's event
	// 0 goes to that firearm's children there (FireComp 0x005f31d0): the EMP
	// strike's GEB_emp_strike (ObjectEffects::weaponFiredAt).
	struct StrikeFired {
		uint32_t asset = 0;
		std::string fireArm;
		physics::Vec3 at;
		// The burst's first round: 0x005a0e50 fires the firearm once (its
		// burst follows), and that is when a server tells its clients
		// (event 0x1e kind 2, the client's 0x0050eeb0).
		bool first = false;
	};
	std::vector<StrikeFired> takeStrikeFires() { return std::exchange(m_strikeFires, {}); }
	// A walker's animation system's playing bundles (none without one).
	std::vector<std::string> objectAnimation(uint32_t object) const;
	// The material a soldier's footstep set is for (0x007fda00; -1 before
	// its first collision event).
	uint32_t footstepMaterial(int player) const;
	// The other material of the soldier's last collision event (+0x32c,
	// -1 before one): what the sound's material radio (0x00586a90's
	// triggerRadio test) looks at.
	uint32_t contactMaterial(int player) const;
	const physics::MaterialManager* materials() const { return m_materials; }
	// The first thing a segment meets (firstHit with where): the fraction
	// along it and the object (0 the terrain or a static object's owner
	// id); `ignore` a body passed through. For the sound's obstruction rays.
	bool segmentHit(const physics::Vec3& from, const physics::Vec3& to, uint32_t ignore, float& fraction,
		uint32_t& object) const;
	void setInput(int player, const SoldierInput& input);
	// The pitch (degrees, up positive) the weapon's recoil has kicked the
	// player's aim by since the last call: the original adds the kick to the
	// soldier's own angles (0x0058a6d0), so a viewer sending absolute pitches
	// takes it into its own; until taken it is added to the input's pitch.
	float takeAimKick(int player);
	// The aim's pitch with the kick not yet taken (degrees, up positive).
	float aimPitchOf(int player) const;
	// Moves a soldier (its feet) at once, as the scripts' setPosition does;
	// false when the object is no living player's soldier on foot.
	bool setSoldierPosition(uint32_t soldier, const physics::Vec3& feet);
	// Whether nothing solid (static objects' projectile LOD, vehicles, the
	// terrain) lies between two points.
	// `ignore`: an object (a body) that doesn't count, such as the looker's own.
	bool lineOfSight(const physics::Vec3& from, const physics::Vec3& to, uint32_t ignore = 0) const;
	// A firearm's ObjectTemplate.setCheckObstruction (start, end, radius;
	// the pod launchers' -1.5 2 1): five rays along the gun (0x005c2310),
	// from start to end metres along its forward axis through `at`, then the
	// same moved by (r, r), (r, -r), (-r, -r), (-r, r) along its right and
	// up axes (the scene's ray 0x00a85220 slot 0x68 past the gun's own root,
	// `ignore`): one meeting anything refuses the trigger (0x005c2ee0; the
	// event handler 0x005c50e0 turns a press or hold into a release, 9).
	bool fireObstructed(const physics::Vec3& at, const physics::Vec3& forward, const physics::Vec3& right,
		const physics::Vec3& up, float start, float end, float radius, uint32_t ignore) const;
	// The AI's sight ray (IAIObjectEnvironment slot 0x44, 0x00897df0: the
	// terrain, then slot 0x90, 0x00898040, the scene's ray with the filter
	// of vtable 0x009d6794): faces of a Material.isSeeThrough material
	// (material slot 0x88; the filter's slot 4, 0x00897d50) don't stop it;
	// `seenThrough` tells whether one lay on the way (the out flag: the
	// nearest such face before the ray's end).
	bool lineOfSight(const physics::Vec3& from, const physics::Vec3& to, uint32_t ignore, bool& seenThrough) const;
	// Whether a team's soldier could pass from one point to the other through
	// the moving objects' soldier meshes (a Titan's: with the other team's
	// forcefields, not its own).
	bool soldierPathClear(int team, const physics::Vec3& from, const physics::Vec3& to) const;
	// The spawn spot's drop (0x005ad8b0): a box from `bottom` to `top`
	// along `up` at `spot` put on what is under its top, or on the terrain.
	bool groundMaterialAlong(const physics::Vec3& from, const physics::Vec3& to, uint32_t ignore, uint32_t& material) const;
	physics::Vec3 dropSpawnSpot(const physics::Vec3& spot, const physics::Vec3& up, float bottom, float top) const;
	// A template's collision-mesh vertices of its vehicle LOD (type 1), in
	// object space (empty without a mesh); `lod` the last part's LOD.
	std::vector<physics::Vec3> vehicleLodVertices(const world::ObjectTemplate& objectTemplate,
		const physics::CollisionLod** lod = nullptr) const;
	// The spawn spot's test for a spawner (see ObjectSpawner::SpotTest).
	ObjectSpawner::SpotTest testSpawnSpot(const world::ObjectTemplate& objectTemplate, const world::Vec3& spot,
		const world::Vec3& rotation, const world::Vec3& spawner) const;
	// Control points with their position, for the bots' strategy.
	struct ControlPointInfo {
		int id;
		int team;
		physics::Vec3 position;
		float radius;
		// Not ObjectTemplate.unableToChangeTeam (the template's vt+0x64 at
		// +0x298; IAIGame vt+0x24, BF2142.exe 0x00895d90, answers it to
		// the AI).
		bool capturable = true;
		// ControlPoint isVisible (the template's ControlPointInfo block
		// +0x58, template +0x2f0, 1 by default; the runtime +0x1a1 through
		// interface 0xc4d1 slot 0x3c, 0x005bb3d0): on clients the point's
		// start (0x005bbe30) hides its object and its three team flags from
		// the scene's cull (object flag 2, 0x00705650) when 0; nothing in
		// capture, spawning, the AI or the network reads it. The 48-player
		// Titan maps' GamePlayObjects set it 0 on their Conquest points.
		bool visible = true;
		// showOnMinimap (+0x2f1, 1 by default; slot 0x44 0x005bb440): read
		// by the minimap, the map and the spawn screen (0x00780410,
		// 0x00789b50, 0x0078bd00 ...).
		bool showOnMinimap = true;
	};
	std::vector<ControlPointInfo> controlPointList() const;
	// The control points of a supply group (host.sgl_getControlPointsInGroup,
	// 0x00568890: the game logic's vt+0x18c): their objects.
	std::vector<uint32_t> controlPointsInGroup(int group) const;
	// Field unlocks the scripts gave (pers_plrGiveFieldUnlock, a squad's
	// bar filled; the game logic's slot 0x68): the player's count not yet
	// spent (the unlock menu's spendFieldUnlock).
	void giveFieldUnlock(int player) { ++m_fieldUnlocks[player]; }
	int fieldUnlocks(int player) const
	{
		const auto it = m_fieldUnlocks.find(player);
		return it == m_fieldUnlocks.end() ? 0 : it->second;
	}
	static void readSupplyLines(const world::ObjectTemplate& t, ControlPoint& point);
	// Where a player wants to spawn next (a control point id, -1 for any of
	// the team's): the spawn menu's choice. Held points the team owns
	// with spawn points; anywhere else falls back to any.
	void setSpawnChoice(int player, int controlPoint);
	// Squads (1..9 per team, up to six soldiers; the first is the leader):
	// members may spawn on their leader or the leader's spawn beacon
	// (spawnChoice kSpawnOnLeader / kSpawnOnBeacon). The scripts hear
	// PlayerChangedSquad.
	static constexpr int kSquadSize = 6;
	// The kit's primary's itemIndex: the Kit template's +0x2c8, 3, set by
	// its constructor (0x00570080) and by no property.
	static constexpr int kPrimaryItemIndex = 3;
	static constexpr int kSpawnOnLeader = -2;
	static constexpr int kSpawnOnBeacon = -3;
	// Mobile spawn points: a vehicle of the team carrying a SpawnPoint
	// (the APCs' and the AT/IFV spawn vehicles' eu_apc_SpawnPoint...,
	// setEnterOnSpawn 1): the spawn group the original gives such a point
	// (net ids from 0xc0, SpawnManager 0x00635080). setSpawnVehicle picks
	// one (spawnChoice kSpawnInVehicle); spawnVehicles lists the team's.
	static constexpr int kSpawnInVehicle = -4;
	void setSpawnVehicle(int player, uint32_t vehicle);
	std::vector<uint32_t> spawnVehicles(int team) const;
	// The spawn point's parent armour test (0x005b24f0): not on a wreck,
	// nor under the armour's criticalDamage.
	bool spawnParentArmourHolds(const world::Object& vehicle) const;
	// The projectile camera: a seat's weapon with ToggleCameraComp's
	// zoom.useProjectileCamera (as_ag's and us_ag's guided missile seats, the
	// hover bike's) firing a round whose template has useProjectileCamera
	// (GenericProjectile +0x32b; the round keeps its child Camera, class
	// 0x9472, interface 0xc367, at +0x240: 0x005b38a0) makes that camera
	// the player's until the round goes (0x005b4b00 hands the view back,
	// slot 0x34). With the Camera's takeOverInput (+0x42c, vtable
	// 0x00979a50 slot 0x88) the PlayerControlObject's input (0x005ca2e0)
	// goes to the camera's root and stops there: the mouse turns the
	// camera within its min/maxRotation at maxSpeed (snapToZeroOnNoInput:
	// back to straight with none), the seat neither turns nor fires, and
	// the wire guidance (0x005e2e20: the player's active camera, slot
	// 0x1b4(1)) steers the round where its camera looks.
	struct ProjectileCamera {
		physics::Vec3 position;
		float yaw = 0, pitch = 0; // degrees, pitch up positive
		float fov = 1.1f;         // the camera's insideFOV (radians)
		bool takeOverInput = false;
		uint32_t serial = 0;      // the round (Projectile::serial)
	};
	bool projectileCamera(int player, ProjectileCamera& out) const;
	// The bots' spawn test (IAIPathfindingCourier slot 0xc, *0x00b4bb94):
	// each scattered spawn try of a bot (0x005b2200's AI flag) must pass it;
	// the bot manager sets its navigation mesh's polygon lookup.
	void setAiSpawnTest(std::function<bool(const physics::Vec3&)> test) { m_aiSpawnTest = std::move(test); }
	// Joins a squad of the player's team (0 leaves), false when full;
	// a new squad's first member leads it.
	bool joinSquad(int player, int squad);
	// The squad manager's join (slot 0x34, 0x004c8460 -> 0x004c7290), as
	// the squad menu's JOIN (0x007a5410), an invite's yes and a client's
	// JoinSquad ask for it: a squad of six takes nobody (the player hears
	// 2/0xb, HUD_HUD_SQUAD_FULL); a private one (the squad's slot 4) takes
	// a player only with its bit in his invite mask (player vtable 0x224,
	// +0x14c) or when it has no members, else its leader hears 2/0x11 (the
	// player: the HUD's question kind 0) and the player 2/0xc (the squad:
	// HUD_HUD_SQUAD_PRIVATE_REQUEST_JOIN). A join clears the squad's bit
	// (0x004c6c80). The messages wait in squadMessages for a player that
	// isn't a bot (a bot's go nowhere: no connection).
	enum class SquadJoin { Joined, Refused, Full, Private };
	SquadJoin requestJoinSquad(int player, int squad);
	// A squad's lock (squad manager slot 0x20, the squad's +4 flag): the
	// leader's toggle (0x007a5a60) and the create box's (0x007a5ad0). A
	// squad left empty is removed with it (0x004c6c80 -> slot 0x3c).
	void setSquadPrivate(int team, int squad, bool isPrivate);
	bool squadPrivate(int team, int squad) const { return m_privateSquads.count({team, squad}) != 0; }
	// Slot 0x30 (0x004c3190)'s bit in the player's invite mask: set by the
	// leader's invite (and its yes to a request), cleared by the invited
	// player's no and by leaving (0x007a55f0).
	void setSquadInvite(int player, int squad, bool invited);
	uint32_t squadInvites(int player) const;
	struct SquadMessage {
		int id = 0;    // PostRemote 2/id
		int value = 0; // its payload's int: the squad, or the asking player
	};
	std::vector<SquadMessage> takeSquadMessages(int player);
	// A squad of the team with room (a new one if none): the squad id.
	int openSquad(int team) const;
	// The squad's beacon object (0 for none).
	uint32_t squadBeacon(int team, int squad) const;
	// Unlocks: a kit's ItemContainer children (one per branch and level,
	// unlockLevel) add their items once unlocked; a slot holds the first
	// unlocked item for it (the customise screen's choice is not in yet).
	// Without a stats server nothing is unlocked; `all` unlocks everything.
	void setUnlocks(int player, std::vector<std::string> containers, bool all = false);
	// The unlocks the player owns besides those chosen (player slot 0x2a8):
	// replaceWithoutEquip containers apply by them (the kit build 0x00570ab0).
	void setOwnedUnlocks(int player, std::vector<std::string> containers);
	// The containers chosen (or, with `owned`, owned besides) of a player.
	std::vector<std::string> unlocksOf(int player, bool owned = false) const;
	// A bot's unlocks for the kit it was given (call after setKit, as the
	// SAI's respawn does, AIDLL 0x100f04a0). The levels (AIDLL 0x100efce0):
	// for each branch b (UB_First, UB_Second, UB_Player, UB_SquadLeader)
	// the most any living human holds for a kit of the same unlockIndex
	// (the AI player adaptor's slot 38 -> player slot 0x2ac, 0x005977e0:
	// the owned levels' bit mask, compared as a number) plus one, at most
	// 4. They go to the bot's adaptor (vtable 0x009d7650) slot 40
	// (0x008a5b90, see GameWorld.cpp), which chooses and owns containers.
	// Returns the four levels.
	// `antiAir`: whether an AI weapon template (a firearm's aiTemplate) is
	// isAntiAirWeapon (the AI's weaponTemplate rows, BotManager's).
	std::array<int, 4> chooseBotUnlocks(int bot, const std::function<bool(const std::string&)>& antiAir = {});
	void setBattery(int player, float battery); // tests and the scripts' view
	// The player's totalDogtagCount (player +0x104, set by the stats
	// scripts' pmgr_p_set; the kit-selection event carries it to clients):
	// at 50 or more (*0x00a67b68) the kit build (0x00570ab0) applies the
	// kit's isDogtagReplacement containers (+0x2e2) whatever is unlocked.
	void setDogtagCount(int player, int count);
	// Whether the player looks through the weapon's zoom (the local
	// player's camera; the deviation's devModZoom and the gun sway use it).
	void setZoomed(int player, bool zoomed);
	// The distance counter (fire.useDistanceCounter, the rocket launchers'
	// airburst): the range set (0: off), and the wheel's steps
	// (PINextItem +1, PIPrevItem -1) while it is on.
	int burstDistance(int player) const;
	void stepBurstDistance(int player, int steps);
	// The weapon in hand's deviation now (SoldierDeviationComp; 0 without).
	float weaponDeviation(int player) const;
	// A lock-on weapon's lock (the target components: MultiFireArmTargetComp
	// on the AA and AT launchers, the walkers' and gunships' guided
	// guns): the target object and its state (0 none, 1 locking, 2
	// locked), as the HUD's object markers show it.
	struct WeaponLockInfo {
		uint32_t target = 0;
		int state = 0;
		float maxDistance = 0; // target.maxDistance of the weapon (0: no lock-on weapon)
	};
	WeaponLockInfo weaponLock(int player) const;
	// The lock-on feedback (the target component's update 0x005e1b30 and
	// its fire handler 0x005e1ee0), for the HUD: a lock taken (Acquired,
	// 35 ticks after the target was chosen, once locked), the locked
	// target gone (Lost), a round fired at a target (MissileAway). The
	// original voices them as radio too: "target_aquired" / "target_lost"
	// to the local player (game slot 0x58, channel 2) for TSLaserGuided
	// only, "missile_away" on TCPlayer to the target's player when the
	// local player fires (slot 0x50). At most the last 64 kept.
	struct LockEvent {
		enum Kind { Acquired, Lost, MissileAway } kind = Acquired;
		int player = -1;     // whose weapon
		uint32_t target = 0;
		int system = 0;      // as Lock::system
	};
	std::vector<LockEvent> takeLockEvents() { return std::exchange(m_lockEvents, {}); }
	// The local player's lock tones (weaponHud.firstLockSound while
	// tracking, secondLockSound once locked: 0x00657ce0, from the lock box
	// 0x007e5a10): sounds to start (play) or stop, by name, for the sound
	// manager (Sound.playSound / stopSound). See stepLockTone.
	struct LockTone {
		std::string sound;
		bool play = false;
	};
	std::vector<LockTone> takeLockTones() { return std::exchange(m_lockTones, {}); }
	// Hit points lost to a player's attack, for the AI (BF2142.exe's AI
	// event adaptor 0x0089f8d0 -> AIDLL's collision handler slot 0x18,
	// 0x1004f020): the soldier hit, or each rider of the vehicle hit, and
	// the attacker; the last 256 kept.
	struct AiHit {
		int victim = -1, attacker = -1;
	};
	std::vector<AiHit> takeAiHits() { return std::exchange(m_aiHits, {}); }
	// Players out of a vehicle by the use key (0x004bb570's AI event 3/3,
	// posted once the soldier is out, with the seat's AI adaptor): the
	// player and the vehicle's root; the last 64 kept.
	struct UseKeyExit {
		int player = -1;
		uint32_t vehicle = 0;
	};
	std::vector<UseKeyExit> takeUseKeyExits() { return std::exchange(m_useKeyExits, {}); }
	// Where `player` looks (the camera's z axis): on foot his look's yaw
	// and pitch, seated his seat's gun or camera turned by the hull.
	physics::Vec3 viewForward(int player) const;
	// The level ambience over the network (AmbientTriggers.h): what the
	// networked areas (numNetworkedEffects) played since the last call, for
	// a server to send (the AmbientEffectArea event, 0x00510270; kept only
	// until the next tick); a joined client leaves those areas to the
	// server and plays the server's events (0x00510230).
	struct AmbientEvent {
		size_t area = 0;        // the area's id (+0x19c)
		int index = 0;          // the Bundle child
		physics::Vec3 offset;
		uint32_t attachedTo = 0; // the activator's object, or 0
	};
	std::vector<AmbientEvent> takeAmbientEvents() { return std::exchange(m_ambientEvents, {}); }
	void setAmbienceClient(bool client) { m_ambience.client = client; }
	bool playAmbientEvent(size_t area, int index, const physics::Vec3& offset, const physics::Vec3* attachedAt = nullptr);
	bool unlockAllByDefault = false; // players joining from now on have everything
	// The local player's auto reload (GeneralSettings.setAutoReload,
	// +0xb6: on by default, 0x0049a170 from the constructor 0x0049bd20;
	// the player's +0x3d0, set from GeneralSettings slot 0xdc by
	// 0x00591e50, player slot 0x36c): its input (player slot 0x48,
	// 0x00594280) presses c_PIReload while a weapon of its control object
	// needs a reload by hand (wantsAutoReload).
	bool autoReload = true;
	// Whether a firearm needs that press: the ammo's slot 0x94
	// (0x005f37f0) holds while it isn't reloading (+0x3c 0), its magazine
	// is empty (slot 0x98, 0x005f37e0), a magazine can be taken (slot 0x40)
	// and it hasn't ammo.autoReload (template +0x28, which reloads by
	// itself); reached through the firearm's interface 0x94c0 slot 0x80
	// (0x005c0ff0). Not while it is drawn, between rounds or held by
	// busyUntilButtonRelease, which refuse the press (0x005c0e70): the
	// rewrite's press is an edge, so it waits until it would be taken.
	static bool wantsAutoReload(const FireArm& arm);
	// The local player's unlocks (its soldier's chosen ItemContainers,
	// Stats/Stats.h), given when spawnPlayer adds the player.
	std::vector<std::string> localPlayerUnlocks;
	// The ItemContainers the local player owns (player slot 0x2a8's bit
	// maps, +0x2b4: the stats' unlocks, which the offline game takes from
	// the soldier's profile; the kit build 0x00570ab0 applies the
	// replaceWithoutEquip ones by them), given with localPlayerUnlocks.
	std::vector<std::string> localPlayerOwned;
	// The kit (and its soldier template) for the player's next spawn.
	void setKit(int player, const std::string& kit, const std::string& soldier);
	int spawnChoice(int player) const;
	// The control points a team may spawn at (held, with spawn points).
	std::vector<int> spawnablePoints(int team) const;
	// Seconds from death to the next spawn (sv.spawnTime, 15 by default).
	void setSpawnTime(float seconds) { m_spawnTime = seconds; }
	// The sea's height (heightmapcluster.setSeaWaterLevel), for vehicles
	// that drown.
	void setWaterLevel(float level)
	{
		m_waterLevel = level;
		m_hasWater = true;
		m_ground.waterLevel = [this](float, float) { return m_waterLevel; };
	}
	// The friendly-fire percentages (sv.soldierFriendlyFire,
	// vehicleFriendlyFire, soldierSplashFriendlyFire,
	// vehicleSplashFriendlyFire: settings +0x128..+0x134, 0..100): damage
	// from a teammate x the percentage / 100 (0x005b7290), soldiers' by the
	// soldier ones, everything else's by the vehicle ones.
	struct FriendlyFire {
		float soldier = 100, vehicle = 100, soldierSplash = 100, vehicleSplash = 100;
	};
	void setFriendlyFire(const FriendlyFire& ff) { m_friendlyFire = ff; }
	// sv.friendlyFireWithMines (settings +0x288, copied to DAT_00a3fcc0
	// +0x58 by 0x00430e20; on by default): see the mines' trigger.
	void setFriendlyFireWithMines(bool on) { m_friendlyFireWithMines = on; }
	// sv.hitIndicator (settings +0xfb, on by default): whether a player's
	// hits light his crosshair's hit indicator (0x004ba070).
	void setHitIndicatorEnabled(bool on) { m_hitIndicatorEnabled = on; }
	// The hit indicator (player slot 0x20c(1), 0x0059b460: the alpha to 1
	// and the networked flag +0x2a8 set): a joined client's own state
	// carries the flag, read and cleared by slot 0x210 (0x0059b480).
	void showHitIndicator(int player);
	bool takeHitIndicator(int player);
	// The sea's height, or none (false).
	bool waterLevel(float& level) const
	{
		level = m_waterLevel;
		return m_hasWater;
	}
	// How long a killed soldier lies revivable (sv.manDownTime, 15; 0: none).
	void setManDownTime(float seconds) { m_manDownTime = seconds; }
	// Respawn on their own when the time is up (no spawn menu yet).
	bool autoRespawn = true;
	// The local player's muzzle flashes in the world: off while the
	// first-person view draws them on its own weapon (the original's
	// first-person weapon children, FireComp 0x005f31d0).
	bool localMuzzleEffects = true;
	bool playerInsideControlPoint = false; // set by the scripts (setIsInsideCP)
	// The player's rounds fired and rounds that damaged something, by
	// weapon template; `clear` starts counting again.
	struct RoundCount {
		std::string weapon;
		int count;
	};
	std::vector<RoundCount> playerRounds(bool hits, bool clear) { return playerRounds(localPlayer(), hits, clear); }
	std::vector<RoundCount> playerRounds(int player, bool hits, bool clear);
	// The local player's id, -1 before spawnPlayer.
	int localPlayer() const { return m_player ? m_player->id : -1; }
	bool isVehicle(uint32_t object) const { return vehicle(object) != nullptr; }
	// The objects with a vehicle, by id (the objects' order).
	std::vector<uint32_t> vehicleObjects() const;

	// Mobile objects (hasMobilePhysics, physicsType Mesh, a collision mesh)
	// get rigid bodies that fall and rest on `ground`.
	void enablePhysics(const io::FileManager& files, physics::CollisionTemplateManager& collisionTemplates,
		physics::Ground ground, const physics::MaterialManager* materials = nullptr);
	size_t physicsBodies() const { return m_bodies.size(); }
	// Whether a mobile object's node sleeps (PhysicsNode slot 0x14c), and
	// how deep it is in the water (node +0x5c).
	bool bodyAsleep(uint32_t object) const;
	float bodySubmerged(uint32_t object) const;
	// Whether a player's soldier is on a ladder (its LadderContainer).
	bool onLadder(int player) const;
	// The weapon in hand put away after a throw (FireArm::hidden): not drawn.
	bool weaponHidden(int player) const;
	// The player's unlocks give the zoom upgrade (ability.hasZoomUpgradeAbility,
	// the Ability template's +0x54, executor 0x0054dd78; copied to the
	// player's ability block +0x44 by 0x005706b0 and read only by the zoom's
	// fine tuning 0x005da890): UnlockEU / UnlockPac_ReconSni1.
	bool zoomUpgrade(int player) const;
	// A player's squad-report reading (0x005088f0 over each recipient's
	// ability block, slot 0x2a0): the reveal time (+0xc, 3 s; 6 with the
	// recon's UnlockEU / Pac_ReconSni1 networkableHudRevealTime) and
	// whether cloak reports reach him (+0x12, the support's
	// networkableHudShowCloaked unlock). False for no such player.
	bool squadReportAbility(int player, int& revealTime, bool& showCloaked) const;
	// The radio filter of `player`'s voice (the radio handler 0x0042e0c0):
	// the soundFilter of the nearest PlayerControlObject over his control
	// object (PCO +0x424, executor 0x004fd0c8; slot 0xf0 of 0xc4c5,
	// 0x005c6830): his seat's, else his vehicle's; "" on foot (the
	// soldier's own +0x3c0, 0x004ded9a, ships unset). The filters are
	// gameLogic.messages.addRadioFilter's: ground / vehicle / airplane a
	// static loop, heli / commander a click.
	std::string radioFilter(int player) const;
	// Whether `object`'s effect bundles may start in their first-person
	// form for the local player (the EffectBundle's test 0x006187c0, when
	// he looks in first person): he sits in it and neither its
	// PlayerControlObject nor his seat's is neverDrawAs1p (PCO +0x481, 0;
	// executor 0x004ff358, getter 0x005c67e0; shipped on the Titans).
	bool firstPersonEffects(uint32_t object) const;
	const physics::CollisionWorld& staticCollision() const { return m_collision; }
	// The projectile LOD's faces a round's hit lands on, for its decal's
	// clipping (RendDX9 0x101655c0 walks the hit mesh's faces): the static
	// objects' (world space) for object 0, else that body's (object space);
	// null without one.
	const physics::CollisionWorld* projectileCollision(uint32_t object) const;
	// A body's collisionPart `part` (raycastBody's index, Decal::part): its
	// projectile LOD now (the wreck's sub-geometry once wrecked) in its own
	// frame, and that frame in the body's as the part stands now (posed
	// as poseCollisionParts poses it); null / false without one.
	const physics::CollisionWorld* partCollision(uint32_t object, int part) const;
	bool collisionPartFrame(uint32_t object, int part, physics::Mat3& rotation, physics::Vec3& position) const;
	// How many collisionPart instances a body's rays meet (0x0063ccc0's).
	size_t collisionPartCount(uint32_t object) const;
	// A body's vehicle LOD faces now (its root instance's sub-geometry,
	// Body::meshState), in its own space: what other movers meet. Null
	// without a body.
	const physics::CollisionWorld* vehicleCollision(uint32_t object) const;
	// The ground the soldiers and bodies stand on (enablePhysics').
	const physics::Ground& ground() const { return m_ground; }
	// A living player's soldier movement (its control and physics body),
	// null otherwise: what a server sends a client's prediction (the
	// control-object record, src/Main/HostedGame).
	// The MaterialManager's friction of a material pair (slot 0x88), which a
	// soldier's contacts take (SoldierBody::frictionOf); 1 without materials.
	float soldierFriction(uint32_t own, uint32_t other) const;
	const SoldierControl* soldierControl(int player) const;
	// The moving objects a soldier at `origin` (its physics origin) of
	// `team` meets this tick: the bodies' soldier LODs and their posed
	// collisionPart children's, in their own frames (the soldier broad
	// phase 0x006dcce0); not `vehicle` (the one it sits in) nor a drone.
	// `soldier` is its object (a launcher's round ignoring it). A joined
	// client's prediction takes them for its own soldier.
	void soldierMovers(const physics::Vec3& origin, int team, uint32_t vehicle, uint32_t soldier,
		std::vector<physics::SoldierBody::Mover>& movers) const;
	const physics::SoldierBody* soldierBody(int player) const;

	// The local player's soldier: spawned at a spawn point of a control
	// point the team holds (team 1 PAC, team 2 EU). Returns its object.
	world::Object* spawnPlayer(int team);
	world::Object* player() const;
	const physics::SoldierBody* playerBody() const { return m_player ? &m_player->body : nullptr; }
	// Where a projectile stopped (for effects and checks).
	struct Impact {
		physics::Vec3 point;
		physics::Vec3 normal;
		uint32_t object; // 0 for the terrain
		uint32_t material;
		float damage;
	};
	const std::vector<Impact>& impacts() const { return m_impacts; }
	// Every impact recorded this round (impacts() keeps the last 65).
	size_t impactsTotal() const { return m_impactsTotal; }
	size_t projectilesInFlight() const { return m_projectiles.size(); }
	// The weapon HUD's ability bar's inputs (0x007c3af0) for a player:
	// - what he last gave (player +0x158..+0x16b through slots 0x25c..0x284:
	//   the game logic's heal 0x004bc410 stamps the giver (tick, 1, 0) for
	//   a soldier of his team healed and (tick, 1, 1) for a vehicle of his
	//   team with a player in it repaired, slot 0x258; its ammo 0x004bd5f0
	//   stamps the giver and the soldier rearmed (slot 0x260; a refill
	//   that gave nothing puts the giver's time back to 0). The flags stay
	//   set once set (the player's constructor 0x0059c010 clears them);
	// - the gun in hand's ReplenishingAmmoComp (0x007bea60: the template
	//   component's ammo.replenishingType, +0x84; 3 without one): whether it
	//   treats (the comp's slot 0x24, 0x005d7e90: targets and on), the
	//   holder's battery (slot 0x28, 0x005d8160) and per target (the list
	//   slot 0x20, 0x0059d8b0) its armour's share of maxHitPoints or, for a
	//   kit with hasAmmoAbility, the control object's slot 0x154 / 15 (a
	//   soldier's ammunition, 0x00588ba0; 15 for anything else);
	// - the kit's Ability flags (kit +0x74 slot 0x38: healing, repairing,
	//   ammo).
	struct AbilityBarInfo {
		bool alive = false;
		bool healFlag = false, repairFlag = false, ammoFlag = false;
		int64_t sinceHeal = -1, sinceAmmo = -1; // ticks since the stamps (-1: none)
		int replenishingType = 3; // 0 RTHeal, 1 RTAmmo, 2 RTTeam, 3 no comp
		bool active = false;
		float battery = 0;
		std::vector<float> targets;
		bool kitAbility = false; // the kit has an Ability component
		bool kitHealing = false, kitRepairing = false, kitAmmo = false;
	};
	bool abilityBar(int player, AbilityBarInfo& out) const;
	// Its parts any machine has: the kit's Ability flags, and the gun's
	// ReplenishingAmmoComp (its replenishingType; false without one).
	static void abilityBarKit(const world::ObjectTemplate& kit, AbilityBarInfo& out);
	static bool abilityBarGadget(const world::ObjectTemplate& gun, AbilityBarInfo& out);
	double roundTime() const { return m_time; }
	const std::vector<Projectile>& projectiles() const { return m_projectiles; }
	// The objects with a geometry in the armour effects started on an
	// object (an EffectBundle's SimpleObject children, created with it by
	// 0x006549a0's slot 0x50: the heavy IFV's cells' e_healing_node glow,
	// ifv_shield_the_generatoreffect), placed now; for the drawing.
	struct PlacedMesh {
		const world::ObjectTemplate* objectTemplate = nullptr;
		physics::Vec3 position;
		physics::Mat3 rotation;
	};
	void armorEffectMeshes(std::vector<PlacedMesh>& out) const;
	// Armor: hit points left (from armor.hitPoints), -1 without armor.
	float hitPoints(uint32_t object) const;
	// An object's radius (Body::objectRadius: its collision mesh's, or a
	// child's farther out), 0 for one without a body. Object slot 0x8c
	// (0x006f7ec0), which the name tags read, is the same figure for an
	// object with a collision mesh (+0x4c slot 0x74) and BundledMesh
	// geometry (its template's slot 0xe4, 0x100a7da0, adds 0).
	float objectRadius(uint32_t object) const;
	// A body's collision box in its own space (Body::boxMin / boxMax); false
	// for an object without one.
	bool objectBox(uint32_t object, physics::Vec3& low, physics::Vec3& high) const;
	// Where a vehicle's entry point for its driver (VehicleLayout::entries:
	// the driver's own, else a shared one) lies in the world; false without one.
	bool entryPoint(uint32_t vehicle, physics::Vec3& at) const;
	// Sets them (the scripts' setDamage); none left kills a soldier or
	// destroys the object.
	void setHitPoints(uint32_t object, float hitPoints);
	const FireArm* playerWeapon() const { return m_player && m_player->alive ? m_player->weapon.get() : nullptr; }
	// The fire mode of the local player's weapon (the one in hand, or the
	// seat's gun; FireMode), -1 without one: the HUD's fire-rate icon
	// (0x007c1f10 with weaponHud.hasFireRate, firerate_<mode>.tga).
	int playerFireMode() const;
	// The bots' AI deviation (BF2142.exe 0x00892b70 -> the firearm's
	// interface 0x94c0 slot 0xf4 -> its deviation component's slot 0xac,
	// 0x00680290 / 0x008a1a90): kept by the weapon's deviation and added to
	// its deviation while the holder is an AI (0x005de980's end, the
	// SoldierDeviationComp's +0x44; 0x005dedb0's, the SimpleDeviationComp's
	// +0x28), so the rounds spread by it through the normal deviation.
	void setAiDeviation(int player, float deviation);
	// An armour's effect levels as its percent falls (0x00654730, the
	// percent ceil(hit points / maxHitPoints x 100), of wreckHitPoints
	// below 0): `levels` highest first, `before` and `now` the percents,
	// `current` the group running (0: none). Stop: the hit points are above
	// every level (the group running stops, none starts); Start `group`:
	// a level was passed on the way down and the lowest at or above `now`
	// differs from `current`.
	enum class ArmorEffectStep { Keep, Stop, Start };
	static ArmorEffectStep armorEffectStep(const std::vector<int>& levels, int before, int now, int current, int& group);
	static int armorPercent(float hitPoints, float maxHitPoints, float wreckHitPoints);
	// Whether a hit is "spectacular" (0x006525f0, called by the armour's
	// damage 0x00656320 when the damage has a giver, kept at +0x250): the
	// object's id (its network id, else +0x20; the rewrite's object id) & 0x7fffffff
	// through the engine's two-step generator (0x0042b230), h, gives
	// ceil((((h | 0x80) >> 7) + 1) x 1.7881393e-07) = 3 for about a third
	// of the objects; for them a hit taking more than 0.2 x armor.hitPoints
	// (slot 0x38, +0xc4), or leaving more than 0.25 x armor.hitPoints, is.
	// The group a spectacular level starts is its addArmorEffectSpectacular
	// list when it has one (0x00652190 / 0x006521b0), else its
	// addArmorEffect list.
	static bool spectacularHit(uint32_t id, float before, float after, float hitPoints);
	const effects::EffectManager& effects() const { return m_effects; }

	// Animation systems for soldiers and their weapons: `load` gives the
	// system of a script path (running it the first time).
	using AnimationLoader = std::function<const anim::AnimationSystemTemplate*(const std::string& path)>;
	void setAnimationSystems(anim::AnimationSystems* systems, AnimationLoader load);
	// The pose of an object's skeleton (model-space matrices per node) from
	// its animation systems, as of the last tick; false for objects without.
	bool skeletonPose(uint32_t object, const anim::Skeleton& skeleton, std::vector<anim::Matrix>& world);
	// The transforms of an object's mesh parts relative to it, when the game
	// places them: a held weapon's parts follow the soldier's skeleton nodes
	// mesh1, mesh2... (posed by the weapon's animation system).
	bool partTransforms(uint32_t object, std::vector<anim::Matrix>& parts) const;
	// A part with a boneIndex under an object with a bundleSkeleton: its
	// local matrix (in its parent's frame) from the bone (0x0063d470), in
	// place of its template placement.
	bool partBone(uint32_t object, const std::string& part, anim::Matrix& local) const;
	// A part's pose as a joined game's ghosts have it (setRemotePartPose):
	// a seat's bundles on a client; false without one.
	bool remotePartPose(uint32_t object, const std::string& part, world::Vec3& offset, world::Vec3& rotation) const
	{
		return seatPartPose(object, part, offset, rotation, true);
	}
	// A LookAtObject part (the EU FAV's rear arms, 0x0064bc00): its local
	// matrix in its parent's frame, turned to face nameToLookAt's object at
	// offsetToLookAt; partBone answers with it.
	bool lookAtPart(uint32_t object, const std::string& part, anim::Matrix& local) const;
	// The bundles an object's body system plays, by priority.
	std::vector<std::string> playingAnimations(uint32_t object) const;
	// The animation an object plays (a .baf path), or empty.
	std::string animationFor(uint32_t object) const;
	// The vehicle the player drives (its object), or null on foot.
	world::Object* playerVehicle() const;
	const Vehicle* playerVehicleState() const;
	// How the game moves a part of an object's template tree (wheels: the
	// suspension's offset and the spin; steering bundles: the steering), as
	// an offset and yaw/pitch/roll in degrees before the part's own
	// placement; false for parts the game does not move.
	bool partPose(uint32_t object, const std::string& part, world::Vec3& offset, world::Vec3& rotation) const;
	// A vehicle's seats and entry points, from its template tree: each
	// PlayerControlObject with seatInformation is a seat (the root first:
	// the driver), with its exit (setSoldierExitLocation), and the gun its
	// subtree holds, aimed by the RotationalBundles that take the mouse
	// (inputToYaw, inputToPitch). Positions are in the vehicle's frame,
	// summed from the children's offsets (their rotations are left out).
	// A firearm of a seat: every GenericFireArm under the seat's
	// PlayerControlObject (not under another seat's), in the tree's order
	// (the PCO's add-weapon, interface 0xc4c5 slot 0x6c 0x005cc5e0: all of
	// them at +0x68, and by fire.fireInput (the fire component's template
	// +0x50, slot 0x38) into three groups: PIFire (8) at +0x78, PIAltFire
	// (31) at +0x88, PIFlareFire (47) at +0x98; any other input joins none).
	// Each group has one active weapon (+0xa8 + group x 4: slot 0x78 gets
	// it, slot 0x7c 0x005c9840 sets it, the old one's slot 0x1a8 and the
	// new one's 0x1a4, its enable); the fire event (6, from the player's
	// input 0x004cc7a0) goes to the three active ones (0x005cacf0), and
	// each fires only on its own input (0x005c50e0).
	struct SeatWeapon {
		std::string weapon;          // a GenericFireArm, or empty
		physics::Vec3 weaponPosition;
		int group = -1;              // 0 PIFire, 1 PIAltFire, 2 PIFlareFire, -1 none
		int itemIndex = -1;          // the item's itemIndex (template +0x2e0), -1 none
		bool selectable = true;      // isSelectable (template +0x31c, 0x005c1150)
		bool podLauncher = false;    // fire.isPodLauncher: the pod seats' (Titans.cpp)
		std::string aiTemplate;      // its aiTemplate (the bots' armament)
		// An ActiveDefenseAmmoComp (ads_land, ads_air): ammo.protectionRadius
		// (+0x6c) and ammo.detonateMaterial (+0x68) (see stepActiveDefense).
		bool activeDefense = false;
		float protectionRadius = 0;
		int detonateMaterial = 0;
		// The gun's WeaponBasedRecoilComp camera recoil (cameraRecoilSpeed,
		// cameraRecoilSize: CameraRecoil) and its SimpleDeviationComp
		// (minDev, setFireDev: DeviationBuildUp; hasDeviation without one off).
		float cameraRecoilSpeed = 0, cameraRecoilSize = 0;
		GunRecoil gunRecoil; // the component's own slide and push (GunRecoil)
		// The gun's ToggleCameraComp (template 0x005db120): zoom.startCameraId
		// (+0x1c, -1 none) and startCameraViewMode (+0x20): the seat's camera
		// as the weapon is taken up (0x005db260: the control object's slot
		// 0x4c, its camera's slot 0x18).
		int startCameraId = -1, startCameraViewMode = -1;
		DeviationBuildUp deviation;
		bool hasDeviation = false;
		// Where the gun's rounds start (MultiFireComp 0x005d5fb0, the fire
		// component's slot 53 0x005f1a20): with fire.fireInCameraDof
		// (template +0x44) the seat camera's frame (fireCamera's),
		// else a barrel's (fire.addBarrelName, 0x004e45a6 -> 0x005d6ae0: the
		// gun's children of those names, 0x005d6d20; one in turn,
		// 0x005d64a0), else the gun's; then fire.projectileStartPosition
		// (+0x5c) in that frame. Positions in the vehicle's frame at rest.
		bool fireInCameraDof = false;
		std::vector<physics::Vec3> barrels;
		bool hasStartPosition = false;
		physics::Vec3 startPosition;
	};
	// A seat. Its SeatWeapon part is its weapon: the first of the PIFire
	// group in the layout (else its first weapon); vehicleSeats() gives the
	// one each seat has selected now.
	struct SeatLayout : SeatWeapon {
		std::string name;            // the seat's PlayerControlObject
		physics::Vec3 position;      // where the soldier sits
		physics::Vec3 exit;          // where a soldier leaving goes
		bool hasExit = false;
		std::vector<SeatWeapon> weapons; // all of the seat's firearms (SeatWeapon)
		int primary = -1;                // the index of the SeatWeapon part in `weapons`
		// specialToggleWeaponInput (PCO template +0x46c, 0x40 none; 0x005c7f80):
		// a press of that input moves the PIFire group on as PIToggleWeapon
		// does, and while it is held that input fires nothing (0x005c69a0 in
		// 0x005c50e0). toggleWeapon (+0x420) has no reader in BF2142.exe.
		int specialToggleInput = -1;
		// The bundles the seat's look turns: the first RotationalBundle of its
		// subtree with an inputToYaw / inputToPitch that the mouse gives
		// (PIMouseLookX / PIMouseLookY; in the land and air maps the mouse's
		// x and y are also PIRoll / PIPitch, which a driver's barrel takes on
		// the hover tank, AS_TANK_barrelbase, only counted with a firearm
		// under it); PICameraX / PICameraY bundles (hatches, AS_TANK_Cupola1)
		// aren't the look's.
		std::string yawBundle, pitchBundle;
		float minPitch = -90, maxPitch = 90; // the pitch bundle's limits (its rotation's y; down positive)
		// The seat's first firearm's rest pitch under the pitch bundle (up
		// positive): the turn of the parts between them (eu_bipod's
		// eu_bipod_toprotation sits 40 degrees up, setRotation 0/-40/0, under
		// eu_down's 20..60 down, so the barrel spans 20 up to 20 down). The
		// bundle's angle is the look less it; the round leaves along the
		// bundle's angle plus it.
		float restPitch = 0;
		// The seat's other bundles on the same pitch input (the heavy mechs'
		// rocket racks, cannon mounts and camera mounts all take
		// PIMouseLookY): each turns with the look within its own limits.
		struct PitchPart {
			std::string name;
			float minPitch = -90, maxPitch = 90;
		};
		std::vector<PitchPart> morePitchBundles;
		// What carries a part of the seat in the vehicle (seatMount): the
		// seat whose yaw bundle it sits under (eu_tank's cupola on the
		// driver's turret, the heavy mechs' second positions on the torso's
		// turret; -1 none), and the bone part above it (boneIndex: the mechs'
		// torsospring; empty none) with that part's place at rest.
		struct Anchor {
			int yawSeat = -1;
			std::string bone;
			physics::Vec3 boneAt;
			physics::Mat3 boneTurn;
		};
		Anchor gunAnchor;  // the gun's bundles and firearms
		Anchor seatAnchor; // the seat's own place (the sitter)
		physics::Vec3 rotation;      // seatInformation's yaw/pitch/roll on the vehicle (with its placement's turn), degrees
		std::string animationSystem; // seatAnimationSystem: the sitter's pose (empty: hidden)
		// seatInformation's first word: the object of the vehicle's tree the
		// soldier sits on (template +0x2a0). The PCO's creation (0x005cd4f0)
		// finds it in its tree (0x005c6670, by template) and keeps it
		// (+0x280); the enter (0x005cdd60) then hangs the soldier under it at
		// the seat's place (its slot 0xf0, the soldier's 0xe8 with template
		// +0x2bc), or without one disables him (updateFlags (1, 0)): only
		// such a seat shows its soldier. Every shipped seat with
		// seatInformation names its own PCO but the static guns', whose
		// soldier sits on the cockpit (Static_AV_Cockpit ...), which the
		// gun's yaw turns.
		std::string seatParent;
		bool seatShown = false;
		// seatLeftRotationLimit / seatRightRotationLimit (the seat's +0x300 /
		// +0x304, interface 0xc4c4 slots 0xd8 / 0xe0): how far the seated
		// soldier's body turns from the seat (the FAVs' open position
		// -90 / 90, the air transports' side seats -15 / 40 and -25 / 30).
		float leftLimit = 0, rightLimit = 0;
		// The yaw bundle's frame in the vehicle (its parents' turn: the
		// Titans' gun hubs sit turned 180 or +-90 degrees) and its limits
		// (minRotation / maxRotation x; free without them).
		float frameYaw = 0;
		float minYaw = -1e9f, maxYaw = 1e9f;
		// The yaw bundle's regulateVector x (template +0x314) and maxSpeed x:
		// the bundle counter-turns with the hull (0x005644b0, see stepGunner).
		float regulateYaw = 0, yawSpeed = 0;
		float ignoreParentYaw = 0; // the yaw bundle's ignoreParentRotation x (+0x344, see stepGunner)
		// The yaw bundle's automaticYawStabilization (+0x308; the tanks'
		// cupolas, the walkers' AA cannons and second gun): see
		// stabilizeGunYaws.
		bool yawStabilized = false;
		// The yaw bundle's surpressPlayerInputVector x (+0x354; the air
		// transports' gun mounts 0.6, the static AP / AV guns' cockpits 0.1):
		// see stepGunner.
		float suppressYaw = 0;
		float ignoreParentPitch = 0; // the pitch bundle's ignoreParentRotation y (see stepGunner)
		float cameraRegulateYaw = 0; // the camera's regulateVector x
		// The camera's ignoreParentRotation (yaw x, pitch y: see stepSeatCamera).
		float cameraIgnoreYaw = 0, cameraIgnorePitch = 0;
		// The seat's camera when it turns on the mouse (a Camera with
		// inputToYaw PIMouseLookX / inputToPitch PIMouseLookY: the drivers'
		// free look, the FAV gunners' lean with automaticReset), its axes
		// (yaw x, pitch y) and pivot.
		std::string camera;
		RotationAxis cameraYaw, cameraPitch;
		physics::Vec3 cameraPivot;
		// The camera's toggleMouseLook (Camera +0x38c: the tanks', the AT
		// walkers' and the gunships' pilots'): see stepSeatCamera.
		bool cameraToggleMouseLook = false;
		// Leaving (the seat's PlayerControlObject template, offsets as its
		// save 0x005cb1xx gives them plus 8): its own place in the vehicle
		// (frameAt, frameTurn), soldierExitLocation (+0x308) and
		// altSoldierExitPosition (+0x354) in that frame, hasRestrictedExit
		// (+0x353), maintainCameraOnExit (+0x480), exitSpeedMod (+0x40c, 1)
		// and the vehicle category (vehicleCategory, the restricted exit's
		// air test).
		physics::Vec3 frameAt;
		physics::Mat3 frameTurn;
		physics::Vec3 exitLocal, altExitLocal;
		bool restrictedExit = false, maintainCameraOnExit = false;
		float exitSpeedMod = 1;
		int category = 4;
		// The seat's maintainCameraOnExitToOtherVehicle (template +0x488):
		// leaving straight into another vehicle (a pod) it stands in for
		// maintainCameraOnExit (GameServer 0x004b0b90).
		bool maintainCameraOnExitToOther = false;
		bool dontClearTeamOnExit = false; // +0x34f (0x005c95b0)
		// The yaw bundle's place, in the vehicle's frame.
		physics::Vec3 yawPivot;
		// The seat's Cameras at rest (fireInCameraDof's frame: the seat's
		// active camera, its first, or the one whose CameraId the weapon's
		// zoom.startCameraId switches to; fireCamera): place and facing in
		// the vehicle's frame, and whether it hangs under the seat's own
		// yaw or pitch bundle (it then turns with the gun's aim; else only
		// by its own look, when it is the seat's mouse-look camera).
		struct SeatCamera {
			std::string name;
			int id = -1; // CameraId
			physics::Vec3 at;
			physics::Mat3 turn;
			bool ridesGun = false;
		};
		std::vector<SeatCamera> cameras;
	};
	struct VehicleLayout {
		std::vector<SeatLayout> seats;
		struct Entry {
			physics::Vec3 position;
			float radius;
			int seat = -1; // an EntryPoint of a seat's own (the Titans' gun hubs): that seat
		};
		std::vector<Entry> entries; // EntryPoint children (entryRadius / setEntryRadius)
	};
	// A vehicle's seats (for checks and the HUD) and who sits in them.
	std::vector<SeatLayout> vehicleSeats(uint32_t object);
	std::vector<int> vehicleOccupants(uint32_t object) const;
	// A seat's gun aim (degrees, yaw relative to the vehicle, pitch up
	// positive); false for seats without a gun.
	// Whether a seat drives: the first of a vehicle with an engine (the
	// Titan's six seats are all guns).
	bool drivesSeat(uint32_t vehicle, int seat) const;
	// A bundle's regulateVector on its yaw (0x005644b0): clamp(regulate x
	// the hull's yaw rate, -1, 1); and the input handler's merge (0x00565180:
	// it replaces an input that is 0 or weaker the same way).
	static float regulation(float regulate, const physics::RigidBody& body);
	// ignoreParentRotation's measure (0x00563180): the turn the parent made
	// from `before` to `now` at a child aimed at yaw / pitch (degrees, pitch
	// up positive) in it, as the change of those angles.
	static void parentTurn(const physics::Mat3& before, const physics::Mat3& now, float yaw, float pitch, float& yawTurn,
		float& pitchTurn);
	static float regulatedInput(float input, float regulated);
	bool gunAim(uint32_t vehicle, int seat, float& yaw, float& pitch) const;
	// A seat's yaw bundle's own rotation (Gun::yaw, degrees, without the
	// bundle's frame): what the vehicle HUD's GunnerAngle reads (the
	// bundle's slot 0x188, 0x007a07c0); false for a seat without one.
	bool seatYaw(uint32_t vehicle, int seat, float& yaw) const;
	// The walk cycle of a vehicle's own animation system (the root's slot
	// 0x180, its slot 0x40: anim::AnimationState::walkCycle); false without
	// one (no bundleAnimationSystem, or not built yet).
	bool walkCycle(uint32_t vehicle, float& length, float& time) const;
	// Whether the seat's look turns its gun (seatAims): every gunner with a
	// gun bundle, and the ground drivers whose turret or barrel is theirs
	// (eu_tank's turret, the APCs' first position, the mechs' torso, the
	// hover tank's barrel); not a pilot.
	bool seatTurnsGun(uint32_t vehicle, int seat) const;
	// The item selection a weapon key opened in a seat (0x004d2a50 ->
	// 0x0079c190 for the local player): the key's item index, once; 0 none.
	int takeSeatItemSelect(int player);
	// minimumSquadSizeForSelection (GenericFireArm template +0x320, executor
	// 0x004e1ed9; the beacon 2, the recon drone's detonator 3, the sentry
	// drone's 4): the firearm's slot 0x188 (0x005c3080) says it can't be
	// selected (part of slot 0x170's test, 0x005c1150) while its owner's
	// squad (the squad manager's slot 0x2c) has fewer members (+0x17c) than
	// that, one fewer for a bot (player slot 0x68); never without a squad.
	bool squadTooSmall(int player, const world::ObjectTemplate& weapon) const;
	// A weapon key that found its weapon refused by that test (0x004d2a50
	// at 0x004d2ea9: the local player's, the HUD's help message 0x52): the
	// key, once; 0 none.
	int takeSquadSizeRefused(int player);
	// A seat gun's next round's start in the world and the hull's turn
	// (the gun's yaw and pitch are in the hull's frame): what stepGunner
	// fires from (seatGunOrigin, the next barrel), for the bots' aim.
	bool gunFrame(uint32_t vehicle, int seat, physics::Vec3& origin, physics::Mat3& hull) const;
	const FireArm* gun(uint32_t vehicle, int seat) const;
	// A seat's active weapon of a group (0 PIFire, 1 PIAltFire, 2
	// PIFlareFire) as an index into its SeatLayout::weapons (-1 none), and
	// any of its weapons' state (nullptr before the seat was first taken).
	int seatWeaponIndex(uint32_t vehicle, int seat, int group = 0) const;
	// A seat's ducking (PCO +0x18c, object +0x310; setDucking, slot 0x12c,
	// 0x005c7cf0): 0x005ca2e0 sets it from its occupant's kPICrouch when
	// the seat's template has allowDucking (the shipped bipods'). Its
	// soldier lets go of the grips while it ducks (poseSeated).
	bool seatDucking(uint32_t vehicle, int seat) const;
	// The Titan whose hull holds the player's soldier on foot (0 none): the
	// soldier's node +0x120 (slot 0xec; the soldier's +0x18c slot 0xa8,
	// 0x00585cb0), set by the response (0x006c3890) when its origin is
	// inside a Titan's hull LOD (insideTitanHull). The ghost priorities'
	// OptimizeEnclosingObjectsNetworking reads it (HostedGame::rescore).
	uint32_t enclosingTitan(int player) const;
	// The seat's third-person system's messages for an IK object that is a
	// firearm (0x005ef250's weapon layers): `messages` with 4 while it is
	// zoomed and 1 while its fire state is above 0, else without them.
	uint32_t ikWeaponMessages(const world::ObjectTemplate& vehicle, const SeatLayout& seat, bool zoomed, bool firing,
		uint32_t messages) const;
	const FireArm* seatFireArm(uint32_t vehicle, int seat, int index) const;
	// The active defence's shield on a vehicle now: its radius (0 none).
	float shieldRadius(uint32_t vehicle) const;
	// Vehicles (for the bots): where, which way, how many seats are free.
	struct VehicleInfo {
		uint32_t object;
		physics::Vec3 position;
		float yaw;
		int freeSeats;
		bool driverFree;
		bool working; // not a wreck, not shut down by EMP
		// Its motion (world frame): for the bots' driving.
		physics::Vec3 velocity;
		physics::Vec3 angularVelocity; // radians per second about each axis
		physics::Vec3 forward;         // its +z
		physics::Vec3 right;           // its +x
		// Its box in object space (the vehicle collision LOD's points): the
		// bots' AI object's box (AIDLL 0x100fe990).
		physics::Vec3 boxMin, boxMax;
		// Degrees: the heading of the camera's yaw bundle where the world
		// turns it itself (a sentry drone's gun, mapObjects); NaN: by the
		// seats (seatYaw).
		float cameraYaw = std::numeric_limits<float>::quiet_NaN();
	};
	std::vector<VehicleInfo> vehicleList();
	// The map's other PlayerControlObjects (the map node 0x00793b60 walks
	// the game's list of the root PlayerControlObjects, DAT_00a1c170 slot
	// 0x1a4, of which vehicleList has the vehicle bodies): the deployed
	// ones (the sentry gun), the drones and the drop pods, which aren't
	// bodies here. Their templates' vehicleHud decides what the map shows
	// (the sentries' mini_sentry; the pods have only usePlayerIcon).
	std::vector<VehicleInfo> mapObjects() const;
	// The terrain's height under a point (the heightmap the physics uses; 0
	// without one).
	float terrainHeight(float x, float z) const { return m_ground.height ? m_ground.height(x, z) : 0.0f; }
	// A world order marker's height (the order object 0x005b8b20, an order
	// on the ground): from a metre over the order's point, the higher of the
	// water (DAT_00a83e58 slot 0x94) and the terrain (slot 0x68), or what a
	// ray 100 m down through the static objects (the scene's slot 0x68 with
	// flags 0x1000100, 0x006f4aa0) meets above that; then up by half the
	// marker's radius (its object's slot 0x8c).
	float orderMarkerHeight(const physics::Vec3& order, float radius) const;
	// Where the order's name tag stands (0x005b8b20, handed to the name tag
	// manager's slot 0x2c): at the order's point when its marker (at
	// `markerHeight` over the same x and z) is 1.5 m or more from it, else
	// 1.5 m over the marker.
	static physics::Vec3 orderTagPoint(const physics::Vec3& order, float markerHeight);
	// The battlefield-objects report (BF2142.exe event 0x41, server
	// 0x005088f0 -> IAIEventCourier vt+0xe0, AIDLL 0x1004e5e0 -> 0x100bf000):
	// `reporter`'s squad now knows of the enemy `players` (and the
	// occupants of their vehicles). The bots send one as their trigger
	// presses or lets go at their target (AIDLL 0x1009a1d0, 0x1009a2a0).
	// `seconds`: how long the clients' minimaps show them (the report's
	// +0x14: a recon drone's pulse its template's +0x70, the revealTime).
	// `kind` the report's +0xc: 0 a sighting, 2 a cloak switched on or
	// off (0x005d92c0), which the server sends on only as kind 1 to a
	// squad with a showCloaked reader (HostedGame::sendBattlefieldReports).
	void reportBattlefieldObjects(int reporter, const std::vector<int>& players, float seconds = 3, int kind = 0);
	// An object template's radio.spottedMessage (its quotes taken off), ""
	// for none: the game's slot 0xd0 (0x0042e9d0) asks the spotted
	// object's Radio component.
	static std::string spottedMessageOf(const world::ObjectTemplate* t);
	// The reports made since the last call, for a server to send each to
	// the reporter's squad (event 0x41: 0x005088f0 posts a copy to each,
	// 0x005087b0; the client's 0x00508590 puts them on its minimap).
	struct BattlefieldReport {
		int reporter = -1;
		int team = 0, squad = 0; // the reporter's
		int seconds = 3;
		int kind = 0;
		std::vector<int> players;
	};
	std::vector<BattlefieldReport> takeBattlefieldReports() { return std::exchange(m_battlefieldReports, {}); }
	// The HUD's crosshair object (the HUD manager's +0x1f0, filled by
	// 0x0075be30 as a comm rose opens): what the player's view ray meets
	// out to `reach` (HUD vt+0x18c with the player's slot 0x98(1)), when
	// it is an enemy's soldier or the vehicle an enemy rides; the enemy
	// player (-1 for none) and where the ray met it.
	int crosshairTarget(int player, float reach = 300, physics::Vec3* point = nullptr) const;
	// Radio.sendSpottedMessage (0x007c8160): the crosshair object reported
	// through the game's slot 0xd4, the reports' path (event 0x41: the
	// squad's minimap knows it, reportBattlefieldObjects). The enemy
	// reported, -1 for none.
	int spotCrosshair(int player);
	// Whether a team's squad had a player reported within the last 3 s
	// (0x100bea30).
	bool squadKnows(int team, int squad, int player) const;
	// A player's carried weapons' ammunition (for the bots' AmmoAssist):
	// rounds left in all, the magazine size and the kit's magazines.
	struct WeaponAmmo {
		std::string weapon;
		int rounds = 0;
		int magSize = 0;
		int magazines = 0;
		int inMag = 0; // the rounds in the gun's magazine (-1 without magazines)
		int magsLeft = 0;    // the magazines besides the gun's (the firearm's slot 0xb0 is this + 1)
		bool inHand = false; // the player's weapon in hand (the PlayerControlObject's +0x1f8)
	};
	std::vector<WeaponAmmo> weaponAmmo(int player) const;
	// A projectile template's detonation at a point, as if it went off
	// there (false without one).
	bool detonate(const std::string& projectileTemplate, const physics::Vec3& at, int attacker = -1);
	// A vehicle's or drone's EMP level (0: running) and whether it is a wreck.
	int empLevel(uint32_t vehicle) const;
	// The camera shake and the tinnitus of a blast (0x005a6e70; its
	// callers: the server's and the client's game logic slot 0xe8,
	// 0x004ba7a0 -> 0x005a9090 and 0x004cc4c0, for a detonation; a gun's
	// round with recoilForce above 5, 0x005ddfc0; an EffectBundle with
	// cameraShake starting, 0x0061a1c0). The arguments as 0x005a6e70 takes
	// them (the detonation's 0x005e9300 fills them, see GameWorld.cpp):
	struct ShakeSource {
		physics::Vec3 at;
		int material = -1;       // edx: -1 shakes but rings no ears
		float radius = 0;        // [ebp+8]
		float damage = 0;        // [ebp+0xc]: the detonation's explosionDamage
		float force = 0;         // [ebp+0x24]: its explosionForce
		bool instigated = false; // [ebp+0x1c]: the projectile's +0x184 slot 0x30 (its handle +0x1c0) set
		uint32_t hitObject = 0;  // [ebp+0x2c]: what the round hit
	};
	// The local player (PlayerManager slot 0x30) with a control object
	// (slot 0x4c): amount = min(damage / 150, 1) x min(force / 20, 1) x 4
	// (*0x00a6ab74, *0x00a6abc8, *0x00a6ab4c) x (instigated ? 1 : 0.1,
	// *0x00a6ab84) x (on foot, or the blast inside its vehicle's box
	// (0x005a60a0) or on its own object: 1, else 0.2, *0x00a6ab00) x (on
	// or inside its own object: 1, else max(0, 1 - d / (3 x radius))), d
	// from the control object's position; above 0 it goes to the player's
	// camera (slot 0x1b4(0)) slot 0x4c: the Camera object's 0x00610450,
	// the hit shake (takeCameraShake). The players in the hit object's
	// PlayerControlObject (interface 0xc4c5, its slot 0x64 list, each
	// with slot 0x60) get the amount without the distance and the
	// on-foot factors through player slot 0x21c (0x00592010: +0x1d8, the
	// player networkable's hit strength, sent twice, +0x1dc): see
	// PlayerInfo::sentShake. And with a material, a local player on foot
	// within 0.6 x radius (*0x00a6ab7c) hears the tinnitus (event 0xd).
	void cameraShake(const ShakeSource& source);
	// The local camera's hit shake this tick (the largest; 0 for none),
	// taken once: 0x00610450's argument.
	float takeCameraShake() { return std::exchange(m_cameraShake, 0.0f); }
	// A player's hit strength for its networkable's record (+0x1d8, 0..25
	// on the wire): the last blast on its vehicle, for the next two records
	// (+0x1dc; each call counts one), else 0.
	float takeSentShake(int player);
	// An EffectBundle's own shake as it starts (cameraShake, 0x0061a1c0).
	void bundleCameraShake(const std::string& bundle, const physics::Vec3& at);
	// The tinnitus the local player's ears got this tick (the game
	// logic's event 0xd, ECGameLogicGLTinnitus, which RendDX9's post
	// production and the sound engine take; see cameraShake). Its seconds
	// (the Sound.tinnitusSetup fades summed, 1 + 2 + 4), 0 for none;
	// taken once.
	float takeTinnitus() { return std::exchange(m_tinnitus, 0.0f); }
	// What a vehicle's driver inputs are scaled by (0x005ca2e0): EMP and
	// critical damage; see GameWorld.cpp.
	static float driverInputScale(float empLevel, float hitPoints, const world::ObjectTemplate& vehicle);
	// The driver's seat's inputs from the soldier's (stepSoldier: the land
	// and air maps): W/S PIThrottle, A/D PIYaw, the mouse's x PIRoll (a
	// tick's look over 5 degrees), its y PIPitch (the aim pitch's change
	// from `lastLookPitch`, unless the input carries a pitch), Z PICrouch,
	// the sprint; each times `scale`.
	static VehicleInput driverInput(const SoldierInput& input, float scale, float lastLookPitch);

	// A vehicle on its own, outside the round: a joined client's copy of
	// the vehicle it drives, for the prediction (src/Main/Prediction). It
	// is built as the round's bodies are (addBodies) and stepped as
	// stepPhysics steps one: the driver's inputs, the engines and wheels,
	// the drag, the integration, the ground and the water, then the static
	// objects' meshes; not the other bodies (they are the server's), and
	// with no collision events, damage or contact effects.
	class LoneVehicle; // below
	std::unique_ptr<LoneVehicle> makeLoneVehicle(const world::ObjectTemplate& t, const physics::Vec3& position,
		const physics::Mat3& rotation);
	// One tick of it with the driver's inputs (already driverInput's).
	void stepLoneVehicle(LoneVehicle& vehicle, const VehicleInput& driver, float seconds);
	// Deployed hubs (their objects).
	std::vector<uint32_t> supplyObjects() const;
	// Squad leaders' drones (a detonator whose projectile carries a
	// ForceObject): the drone hovering by a player (its object, 0 for
	// none), and whether it has strayed past its warningDistance (the
	// owner's "uav_warning").
	uint32_t drone(int owner) const;
	bool droneWarning(int owner) const;
	// Recon drones' pulses: whether a player shows on a team's minimap
	// (revealed for pulse.revealTime seconds), and every player that does.
	bool isRevealed(int player, int byTeam) const;
	std::vector<int> revealedPlayers(int byTeam) const;
	// Radar sources: the sonar balls, the radar grenade, the recon drone's
	// rounds (a projectile whose Ability has radarRadius; registered at
	// its creation, 0x00656550, no arming or rest needed). In BF2142.exe
	// they live in each client's minimap only (nothing networked, no
	// server scan): the minimap's update (0x0078f680, every 5th frame,
	// 0x00789f30) keeps those of the local player's team (the shooter's
	// root's team) within max(view distance, 500) m of the player's
	// controlled object (0x00789a10), and draws each as icon_uav_circle
	// (0x0078f060): with radialRadar a full ring turned by the sweep, else
	// one of diameter 2 x radius x sweep / 2pi fading as 1 - sweep / 2pi.
	struct RadarSource {
		uint32_t serial = 0; // Projectile::serial
		physics::Vec3 position;
		float radius = 0;
		bool radial = true, showPlayers = true, showVehicles = true;
	};
	std::vector<RadarSource> radarSources(int team) const;
	// The minimap's sweep (+0xb5c): 4 rad/s (+0xb60), wrapping at 2pi
	// (0x00788600); kept here with the round's ticks.
	float radarSweep() const { return m_radarSweep; }
	// Whether the ring of `source` at `sweep` passes `target` (0x00791660):
	// within the radius across the ground, and radial: the sweep within
	// 0.1 rad of the bearing (atan2 of the x and z offsets here; the
	// engine's is in minimap coordinates), else the expanding ring within
	// 2.5 m of the target's distance.
	static bool radarPasses(const RadarSource& source, const physics::Vec3& target, float sweep);
	// What the rings of `team`'s sources pass now (0x007926c0 from
	// 0x00792980 and 0x00793b60): enemy soldiers on foot, alive and not
	// man-down (with showPlayers), enemy vehicles with a living occupant
	// (with showVehicles); none whose template has
	// vehicleHud.alwaysShowOnMinimap or lacks canBeSpotted. The HUD puts
	// each on the minimap as a short blip (0x00790d60, once while listed)
	// and plays HUD_Uav_target. (A decoy shown in a soldier's place,
	// HudManager slot 0x70 within 60 m, is the HUD's: radarMarkers.)
	struct RadarContact {
		uint32_t object = 0; // the soldier's or the vehicle's
		int player = -1;     // the soldier's player, or the vehicle's first occupant
		bool vehicle = false;
		physics::Vec3 position;
		uint32_t source = 0; // the RadarSource's serial
	};
	std::vector<RadarContact> radarContacts(int team) const;

	// The commander (one per team; docs/engine.md, "Commander"). The game
	// mode's script enables it (host.sh_setEnableCommander). Players apply
	// (Commander.commanderApply) and the team's best applicant leads; the
	// scripts hear ChangedCommander(team, old, new). A commander leaves its
	// squad.
	void setCommanderEnabled(bool enabled);
	bool commanderEnabled() const { return m_commanderEnabled; }
	int commander(int team) const; // the player, -1 for none
	// Makes a player the team's commander (-1: nobody); false when the
	// commander is disabled or the player isn't on the team.
	bool setCommander(int team, int player);
	// Commander.commanderApply / commanderResign (docs/engine.md,
	// "Commander"): applications wait for the apply period, then the
	// highest-ranked applicant is offered command and answers.
	void applyForCommander(int player);
	void resignCommander(int player);
	// The applicant offered command (-1: none) and its answer (the
	// Commander event's kind 0 accepts, kind 1 declines).
	int commanderOffer(int team) const { return team == 1 || team == 2 ? m_commanders[team].candidate : -1; }
	void answerCommanderOffer(int player, bool accept);
	// The player's rank (the stats' rank, player slot 0x1a8 +0x20) the
	// applicants are ranked by; -1 until set (pmgr_setScore "rank").
	void setPlayerRank(int player, int rank);
	// The player's connected flag set (slot 0x8c with 1, 0x005915f0): the
	// clock (here the round's seconds) goes to +0x1c8. addPlayer does it
	// for a human (0x004a8460); the host again when the client has loaded
	// the level (0x004a7c80).
	void setPlayerConnected(int player);
	// The commander's tools (Commander.orbitalStrike, .artillery, .uav,
	// .satellite, .supplies). All but the supply drop need the team's asset
	// intact: a placed object whose template holds a RemoteControlledObject
	// of the tool's rcType (commander_emp_strike: RCOrbitalStrike,
	// commander_orbital_strike: RCArtillery, commander_uav_drone_control:
	// RCUav, commander_movement_analyst: RCSatellite).
	enum class CommanderAbility { OrbitalStrike, Artillery, Uav, Satellite, Supplies };
	static constexpr int kCommanderAbilities = 5;
	struct CommanderAbilityState {
		bool available = false;   // the asset stands (the supply drop needs none)
		uint32_t asset = 0;       // its object (the first intact one)
		float armor = 0;          // the asset's hit points over its maximum (1 for supplies)
		bool ready = false;
		float timeUntilReady = 0; // seconds
		float reloaded = 0;       // 0..1 of the recharge done (the HUD's *TimeUntilReloaded bar)
		bool active = false;      // a UAV or scan running, a strike on its way
	};
	CommanderAbilityState abilityState(int team, CommanderAbility ability) const;
	bool abilityReady(int team, CommanderAbility ability) const { return abilityState(team, ability).ready; }
	// Uses a tool at a point of the map (x, z; the ground's height is
	// found); false when it isn't ready. The orbital strike and the
	// artillery fire their asset's weapon from orbitalBombardmentHeight
	// over the point; the UAV watches round the point; the satellite scan
	// covers the map; the supply drop falls from supplyDropHeight.
	// byAi: the AI commander's use (AIDLL's IBFCommander, sender -1),
	// which needs no commander.
	bool commanderAction(int team, CommanderAbility ability, float x, float z, bool byAi = false);
	// A joined game's commander used a tool on the server: its recharge
	// counts from now in this (the client's) round, as the exe's client
	// keeps its tools' reload times itself (0x007d81d0: the tool's +0x5c
	// and +0x60 = now + its reload time). abilityState then has it.
	void noteCommanderUse(int team, CommanderAbility ability);
	// The team's UAV while it is up: where, and how far it reveals.
	bool uavArea(int team, physics::Vec3& at, float& radius) const;
	bool satelliteScanActive(int team) const;
	// The enemies a running satellite scan shows: to the team's commander
	// only (0x0057dc90), empty for anyone else. (revealedPlayers holds the
	// UAV's and the drones' for the whole team.)
	std::vector<int> scannedPlayers(int viewer) const;
	// Strikes on their way, for the HUD and the map.
	struct Strike {
		CommanderAbility ability;
		int team;
		physics::Vec3 target;
		float fireIn;      // seconds until the asset fires (its launch delay)
		int salvosLeft;    // rounds of the burst still to fire
	};
	const std::vector<Strike>& strikes() const { return m_strikes; }
	// Supply crates dropped by the commanders (their objects).
	std::vector<uint32_t> supplyCrates() const;
	// A supply object's storage for its name tag's bar (RendDX9 0x1016c1e0
	// with Nametag.showSupply: the SupplyObject's +0x194 over its
	// sharedStorageSize +0x2bc, 0..1) and the bar's kind (the tag's +0xc8:
	// 2 with healSpeed +0x2a8 above 0, else 4 with refillAmmoSpeed +0x2ac,
	// else 3). False when `object` isn't a crate or a deployed hub here.
	bool supplyStorage(uint32_t object, float& share, int& kind) const;
	// The gameLogic settings for the tools (GameLogicInit.con and the
	// constructor 0x004d69d0's defaults).
	struct CommanderSettings {
		float supplyDropHeight = 50;          // 0x1b8
		float supplyDropNumSecsToLive = 300;  // 0x1bc
		float supplyDropReloadTime = 60;      // "SupplyDrop-Reload-Time" (0x0043d6d0)
		float uavTime = 30;                   // 0x228: how long the UAV stays
		float uavReloadTime = 5;              // 0x22c
		float uavRadius = 100;                // 0x230: the minimap's ring round it
		float satelliteScanTime = 11;         // 0x238
		float satelliteScanReloadTime = 16;   // 0x23c
		float orbitalBombardmentHeight = 500; // 0x254
		float extraUAVVehicleFlightHeight = 0; // 0x234 (slots 0xc8/0xcc; no shipped script sets it)
	};
	// The team's UAV drone in the air (eu_drone), 0 for none.
	uint32_t uavDrone(int team) const { return team == 1 || team == 2 ? m_commanders[team].uavDrone : 0; }
	CommanderSettings commanderSettings;
	// Orders to squads. The HUD's SquadLeader.sendOrder <kind> <bool> and
	// Commander.sendOrder <kind> <bool> both run HudManager slot 0x3f4
	// (0x007d6ab0); the kind is the engine's order kind (a byte on the wire,
	// the order event 0x0050dfe0):
	// - 0 move, 1 attack, 2 defend, 3 repair: the issue-order menu
	//   (HudElementsIssueOrders.con: "sendOrder 0" by Move, "1" by Attack,
	//   "2" by Defend) and the order icons (Ingame/Commander/Orders/
	//   orderMove..orderRepair at 0x184 + 4 kind, 0x007a0000);
	// - 4 move, 5 attack, 6 defend, 7 repair: placed on the map (the
	//   right-click menus' "sendOrder 4 1" .. "7 1"; a map click arms 4, or
	//   5 on an enemy and 6 on a friendly object, 0x00753c90). Only 5 and 6
	//   count for the scripts' ifAcceptedAttackDefendOrder (0x00592160).
	// A kind of 10 or more drops the armed order. The bool takes the
	// position from the minimap's cursor as the order's next point
	// (0x00780820, up to three; the 9 floats of the order) instead of the
	// point already armed. A squad leader's order goes to his own squad
	// (SquadManager slot 0x48, 0x004c5360: the squad's leader order +0x30,
	// accepted at once); the commander's to each selected squad with
	// members (slot 0x4c, 0x004c5530: the squad's commander order +0x120,
	// for its leader to accept).
	enum class OrderType { None = -1, Move = 0, Attack = 1, Defend = 2, Repair = 3, MapMove = 4, MapAttack = 5,
		MapDefend = 6, MapRepair = 7 };
	// The engine's kind number as an order (None for 10 and more, and for
	// numbers that are no kind).
	static OrderType orderTypeFromKind(int kind)
	{
		return kind >= 0 && kind <= 7 ? static_cast<OrderType>(kind) : OrderType::None;
	}
	static bool isAttackOrDefend(OrderType type) { return type == OrderType::MapAttack || type == OrderType::MapDefend; }
	struct SquadOrder {
		OrderType type = OrderType::None;
		physics::Vec3 position;
		// The player's orderRadius (player +0x144, set to 80 by the
		// constructor 0x0059c010; nothing in the game changes it).
		float radius = 80;
		bool fromCommander = true;
		bool accepted = false; // the squad leader's answer (Commander.commRoseAccept 1)
	};
	// The commander's order: false for a squad that doesn't exist or an
	// order from someone else than the team's commander (-1 for none: the
	// game itself).
	bool issueOrder(int team, int squad, OrderType type, float x, float z, int from = -1);
	void acceptOrder(int player, bool accept);
	const SquadOrder* squadOrder(int team, int squad) const;
	// A squad leader's order to his own squad (SquadLeader.sendOrder):
	// false when the player leads no squad. The scripts' orderPosition,
	// orderRadius and ifAccepted* read this one (squad slot 0x10,
	// 0x004c2ee0), not the commander's.
	bool issueSquadLeaderOrder(int player, OrderType type, float x, float z);
	const SquadOrder* squadLeaderOrder(int team, int squad) const;
	// The squad leaders' requests to the commander (SquadRequests.cpp; the
	// squad manager's slot 0x50, 0x004c5720, into the squad's record
	// 0x004c3f20): kind 0 artillery, 1 orbital strike, 2 UAV, 3 supplies,
	// one of each kind a squad (+0x2b + kind its flag, +0x60 + 0x30 kind
	// its point), until the commander answers or the squad has no leader.
	struct SquadRequest {
		int team = 0, squad = 0, kind = 0;
		physics::Vec3 position;
		float storedAt = 0; // the world's time
	};
	static constexpr int kRequestKinds = 4;
	// False for a kind out of range or a squad without a leader.
	bool storeSquadRequest(int team, int squad, int kind, const physics::Vec3& at);
	// The squad manager's slot 0x24: the request taken off.
	void removeSquadRequest(int team, int squad, int kind);
	const SquadRequest* squadRequest(int team, int squad, int kind) const;
	std::vector<SquadRequest> squadRequests(int team) const;
	// A request's tool.
	static CommanderAbility requestAbility(int kind)
	{
		return kind == 1 ? CommanderAbility::OrbitalStrike : kind == 2 ? CommanderAbility::Uav
			: kind == 3 ? CommanderAbility::Supplies : CommanderAbility::Artillery;
	}
	// The vehicle model of an object with one.
	const Vehicle* vehicle(uint32_t object) const;
	// The input for the next ticks.
	void setPlayerInput(const SoldierInput& input) { m_playerInput = input; }
	// Titan mode (Titans.cpp; docs/engine.md, "Titans"). The Titan sync
	// manager (0x005628f0, ECPhysicsETitanSync: set up by 0x00561c00, run by
	// 0x00561e20 only when the game mode is "gpm_ti") keeps one Titan per
	// team: a spawned object whose template tree holds a
	// RemoteControlledObject of rcType RCTitan (7) of that team, not a
	// wreck (0x005ab390). The game mode is the server settings' (main's
	// +gameMode).
	void setGameMode(std::string mode) { m_gameMode = std::move(mode); }
	const std::string& gameMode() const { return m_gameMode; }
	// The level's combat areas (CombatAreas.h): the Titans keep inside those
	// binding vehicle category 4 (0x005ab740).
	void setCombatAreas(const CombatAreas* areas) { m_combatAreas = areas; }
	const CombatAreas* combatAreas() const { return m_combatAreas; }
	// Whether a player's controlled object (its vehicle aboard, else its
	// soldier) is inside the combat areas (0x004ca5d0, the AI's IAITerrain
	// slot 0x1c 0x008a0380; CombatAreas.cpp).
	bool insideCombatArea(int player) const;
	// The time the player has been outside (player +0x294, sent to its
	// client in the player networkable's 0x800 part) and the seconds left
	// before it takes damage (the HUD's "TIME UNTIL PUNISH: #TIME#",
	// 0x0044c7b0); -1 while inside or when the areas aren't used.
	float outsideCombatArea(int player) const;
	float combatAreaTimeLeft(int player) const;
	// sv.allowTitanMovement (the server settings' +0x59, read by the move
	// 0x005aca70 and the order's check 0x005abaf0).
	bool allowTitanMovement = true;
	struct TitanInfo {
		uint32_t object = 0;
		int team = 0;
		int controlPoint = -1;  // its spawner's (the Titan's spawn points belong to it)
		physics::Vec3 position, velocity;
		float hitPoints = 0, maxHitPoints = 0;
		float percent = 0;      // armour vtable 0x40 (0x00651f90): hit points over the maximum, x 100
		bool shield = false;    // above 60 %: the HUD's shield span (HudElementsTitans.con), the forcefield up
		bool canMove = false;   // above 60 % (0x005aca70's +0x35 against *0x00a6afbc)
		bool hasTarget = false; // a commander's move order in force
		physics::Vec3 target;
		bool wreck = false;
		int consoles = 0, consolesLeft = 0;
		bool coreExposed = false; // the core's doors are down
		bool coreDestroyed = false;
		float radius = 0;         // 0x00562090: half the diagonal of its box across x and z
		// The wreck's end (0x00652380): the armour drains below 0 towards
		// -wreckHitPoints; the explosions at their levels (addArmorEffect);
		// ECGameLogicGLTitanCoreExplosion (event 0x14) at -38 %.
		int effectLevel = 0;      // the armour effect group last started (0: none)
		bool coreExplosion = false;
		// Its name tag's data (the Nametag component, object +0x8c):
		// nametag.healthActivationLevel (template +0x24, 100 by default,
		// getter slot 0x50): RendDX9's refresh (0x100f40d0) under 100 sets
		// tag flag 0x1000000 and the secondary icon (slot 0x44(1)) once the
		// first armour's percent is at or below it (a change of icon or
		// team blinks the tag); armor.addNametagOnDestroy (+0x195, getter
		// slot 0x1f4): each drop of the hit points re-pushes the tag to the
		// NametagManager (0x00655460 at 0x6555e0 -> *0x00a20a00 slot 0x30),
		// counted here.
		float healthActivationLevel = 100;
		int nametagPushes = 0;
	};
	std::vector<TitanInfo> titans() const;
	// The rolling-surface loops (SeatBehaviours.cpp; the PCO's contact sound
	// object, 0x005c64a0 / 0x005c65d0 / 0x005c6510, stepped by its update
	// 0x005c8e70): a vehicle whose template has groundContactVolume over 0
	// (+0x474: the APCs, tanks, FAVs, the heavy IFV and the hover tank, all
	// 1) loops its wheels' material sound, the material cell (the first
	// WheelEffectBundle's own material, the ground under it) sound 0
	// (S_GroundContact_Dirt, _Tarmac, ...), its envelopes at `input`: the
	// root's speed's (|v| - 0.1) / 30 within 0..1, eased by 0.08 a tick and
	// 0 off the ground.
	struct RollingSound {
		uint32_t vehicle = 0;
		std::string sound;
		float input = 0;
		physics::Vec3 position, velocity;
	};
	std::vector<RollingSound> rollingSounds() const;
	// The heavy IFV's shield cells (SeatBehaviours.cpp): its armoured
	// children with hudRepairCellNo (+0x204 getter, armour slot 0x168; not
	// -1), in the root's armour order, each true while its armour stands
	// (state 0): the vehicle HUD's cells (0x007a1e50 -> its +0x336 + n).
	std::vector<bool> repairCells(uint32_t vehicle) const;
	// The same cells' hit points as the root's SimpleObject state carries
	// them in mask 0x800000 (armor.fullChildSync, 0x005ffec0's capture):
	// eighths of the maximum, rounded down, at least 1 while any are left,
	// 0 at none or fewer.
	std::vector<int> repairCellEighths(uint32_t vehicle) const;
	// A joined client's cells from those eighths (0x005ffec0's apply
	// through the armour's setter, slot 0x38): 1..8 eighths of the
	// maximum, a wreck's negative wreckHitPoints at 0; the cells' armour
	// effects then follow (stepShieldCells), and the host's mirror of the
	// hull no longer runs on them here.
	void setRemoteRepairCells(uint32_t vehicle, const std::vector<int>& eighths);
	// A shield cell's id for the damage paths (hitPoints, damage: the
	// projectile LOD's owner of its faces; kShieldCellIds + vehicle x 16 +
	// index), 0 for none.
	static constexpr uint32_t kShieldCellIds = 0x7e000000u;
	uint32_t shieldCellId(uint32_t vehicle, size_t index) const;
	// Team damage (TeamDamage.cpp; ECGameLogicGLTeamDamage, game logic event
	// 0/0xb, raised by the damage 0x004c0e40): an attacker's damage on his
	// own team's soldier past teamDamageScoreLimit (50 %) of its maximum, or
	// on his team's manned (or teamed) vehicle past
	// teamVehicleDamageScoreLimit (25 %), raises PlayerTeamDamagePoint
	// (attacker, object) for the scripts (scoringCommon.py's -1) and sends
	// {kind, victim, attacker} to the attacker and the victim (the vehicle's
	// first occupant, -1 none). Each recipient takes its own once.
	struct TeamDamage {
		int kind = 0;        // 0 a soldier, 1 a vehicle
		int victim = -1;     // the soldier's player, the vehicle's first occupant (-1 none)
		int attacker = -1;
		uint32_t object = 0; // what took it (the soldier, the vehicle)
	};
	std::vector<TeamDamage> takeTeamDamage(int recipient);
	// The game logic's limits (sgl_setParam's teamDamageScoreLimit and
	// teamVehicleDamageScoreLimit, the score manager +0x54 / +0x58, slots
	// 0x40..0x4c; its constructor 0x00560e00 sets 50 and 25).
	void setTeamDamageLimits(float soldierPercent, float vehiclePercent)
	{
		m_teamDamageLimits[0] = soldierPercent;
		m_teamDamageLimits[1] = vehiclePercent;
	}
	// The HUD's answer to one on `local`'s screen (0x00759bf0 case 0xb):
	// the victim himself says AUTO_MOODGP_friendlyfire to the attacker
	// (TCPlayer, the client's slot 0x50 with the spam check); else, the
	// local player the attacker and the victim alive and up, the victim's
	// voice says it to him (slot 0x54); else the personal line
	// HUD_HUD_TEAM_DAMAGE_PLAYER (kind 0) or _VEHICLE (kind 1).
	enum class TeamDamageNotice { None, SayFriendlyFire, VictimSaysFriendlyFire, PlayerLine, VehicleLine };
	static TeamDamageNotice teamDamageNotice(const TeamDamage& event, int local, bool victimUp);
	static constexpr const char* kFriendlyFireVoice = "AUTO_MOODGP_friendlyfire";
	// The AI runs on the server (AIDLL's interface 0xb47ce4, its slots 0x18
	// and 0x20): a wreck's armour then drains three times as fast (0x00652380).
	void setAiRunning(bool on) { m_aiRunning = on; }
	// The ladder's camera (the LadderContainer's LadderContainerCamera,
	// DummyObjects.con: 1.5 m over the container and 0.4 m behind it, the
	// mouse's PIMouseLookX / Y within -130..140 and -60..60 degrees of the
	// ladder, 250 degrees a second), pushed over the top near it
	// (0x0059e730). False off a ladder.
	struct LadderView {
		physics::Vec3 position;
		float yaw = 0, pitch = 0; // degrees (pitch up positive)
	};
	bool ladderView(int player, LadderView& view) const;
	// The seated soldier's own camera (the SoldierCamera, 0x005890a0),
	// which a seat without a camera of its own looks through (0x005c7cb0:
	// no camera in the seat's subtree, +0x2c 0, gives the occupant's): the
	// seat's place (the soldier's physics origin there; the camera adds the
	// crouch pose's eye, seated being pose 1), the yaw of the seat plus the
	// body's turn and the twist, the aim's pitch within 85 degrees.
	bool seatedSoldierView(int player, LadderView& view);
	bool isTitan(uint32_t object) const; // the Titan's root (0x00561d90)
	// Destroyable props (DestroyableObject, Props.cpp): broken this round,
	// and the geometry its wreck shows (-1: its own; 1: a render object of
	// two geometries shows its second, 0x00651c60's slot 0xcc(1)).
	bool propWrecked(uint32_t object) const;
	// The geometry a kit's mesh shows instead of its default (0x0056cc50:
	// the mesh's sub-geometry, slot 0x6c; one past its count falls back to
	// the default): its wearer's seat's crewKitIndex, -1 for the default.
	int kitGeometry(uint32_t kitObject) const;
	int propGeometry(uint32_t object) const;
	// Whether heals and repairs may mend `object` (gameLogic slot 0x108's
	// test 0x004cc380 and the armour's 0x00655a00): its armour template's
	// canBeRepaired (+0x14c, 1 by default), a wreck (no hit points left)
	// only with canBeRepairedWhenWreck (+0x14d, 0 by default), never a
	// Titan's own armour (0x00561d90).
	bool repairable(uint32_t object) const;
	// A gadget's ReplenishingAmmoComp as the scan reads it (abilityOf): its
	// abilityRadius, whether it rearms (RTAmmo) and onlyActiveWhileFiring;
	// false without one. A joined client's own tool scans with these
	// (host::RemoteHudWorld::stepAbility).
	static bool gadgetScan(const world::ObjectTemplate& gadget, float& radius, bool& ammo, bool& whileFiring);
	// The object a segment meets first (firstHit, passing through
	// `ignore`'s body); 0 for none or the terrain.
	uint32_t firstObjectHit(const physics::Vec3& from, const physics::Vec3& to, uint32_t ignore) const
	{
		uint32_t object = 0;
		return firstHit(from, to, object, ignore) ? object : 0;
	}
	uint32_t titanOf(int team) const;     // 0 without one
	// The commander's move order (Commander.moveTitan, the order event's
	// server side 0x0050f1f0 -> 0x005abba0): the point must leave the Titan
	// clear of the terrain (0x005aba40); taken, it holds from two seconds
	// on (the tick + 2 / tick length). The answer is the commander's radio
	// message (0x00757xxx: AUTO_TITAN_CC_moveaccepted / _unablemove).
	enum class TitanMove { Accepted, Unable, NoTitan, TooSoon };
	TitanMove moveTitan(int team, float x, float z);
	// The Titan sync's message (ECPhysicsETitanSync, net::TitanSyncInfo):
	// `order` an order taken (id 0: 0x0050f1f0, or the server's first order
	// at the Titan's own place, 0x005aca70), else the state every 30 ticks
	// (id 2, 0x00561e20); the team's last order and the Titan's last state
	// (0x005ab040). Ticks are the round's (ticks()) on the server; the host
	// hands them on as its own.
	struct TitanSync {
		bool order = false;
		int team = 0;
		uint32_t orderTick = 0;
		float targetX = 0, targetZ = 0;
		uint32_t stateTick = 0;
		float velocityX = 0, velocityZ = 0, x = 0, z = 0;
	};
	std::vector<TitanSync> takeTitanSync() { return std::exchange(m_titanSyncOut, {}); }
	// Each Titan's last order and state as an order message: a client that
	// joins after the orders went out has its Titans' sync from them (the
	// rewrite's; the exe's late client keeps its Titan where CreateObject put
	// it until the next order).
	std::vector<TitanSync> titanSyncState() const;
	// A joined client's side (0x00562100 takes the messages, 0x00561e20 ->
	// 0x005aca70 runs in the client's frame 0x00590e10): its Titans are
	// moved as the server moves its own, a tick at a time from the last
	// state up to `tick` (the server's, as the ghost blocks count them),
	// and put back to the server's state where one differs.
	void titanSyncMessage(const TitanSync& message);
	void stepTitanSync(uint32_t tick);
	// Whether the client's sync put this Titan (an order in force and a
	// state to go from): its ghost's place is then not the one to take.
	bool titanSyncPlaced(uint32_t object, physics::Vec3* velocity = nullptr) const;
	// A joined client's Titan's hit points (its ghost's armour, 0x005ffec0):
	// above 60 % it may move (0x005aca70's +0x35).
	void setRemoteTitanHitPoints(uint32_t object, float hitPoints);
	uint32_t ticks() const { return m_ticks; } // the engine's tick count (0x00666090)
	// The Titan's armoured parts (its tree's templates with armour below the
	// root): consoles, the core and its doors, the guns, the forcefield.
	struct TitanPartInfo {
		std::string name;     // the part's template
		uint32_t id = 0;      // what hits on it report (Impact::object)
		enum Kind { Other, Console, Core, CoreDoors, Gun, Shield } kind = Other;
		float hitPoints = 0, maxHitPoints = 0;
		bool wreck = false;
		physics::Vec3 position; // in the world
		float percent = 0;      // the armour's (vtable 0x40)
		float healthActivationLevel = 100; // as TitanInfo's
		int nametagPushes = 0;
	};
	std::vector<TitanPartInfo> titanParts(int team) const;
	// The game logic's damage (0x004c0e40) as a shot's hit: through the
	// armour's rules (a Titan's floors and parts' parent damage), `attacker`
	// the player (-1 for none). No material cell (a cell of 1).
	void applyDamage(uint32_t object, float amount, int attacker = -1) { loseHitPoints(object, amount, attacker); }
	// The game logic's heal (slot 0x108 -> 0x004cc380's gates -> the
	// armour's 0x00655a00, then 0x004bc410): `percent` of the object's
	// maximum hit points by `repairer` (-1 none), raising its Python
	// component's onRepairEvent out of a wreck. False when nothing moved.
	bool repair(uint32_t object, float percent, int repairer = -1);
	// Missile silos (templates with isMissileSilo): each fires its
	// GenericFireArm's projectile at the enemy Titan when loaded (the auto
	// controller CP_CANNON, AI/AutoControllers.ai), reloading
	// ammo.reloadTime after each launch and after each change of hands
	// (reloadOnTeamChange).
	struct SiloInfo {
		uint32_t object = 0;
		int team = 0;
		int controlPoint = -1;
		physics::Vec3 position;
		bool loaded = false;
		float reloadLeft = 0;   // seconds until it is loaded
		float reloadTime = 120; // ammo.reloadTime (the map's reload wipe, 0x0078a990)
		int launched = 0;       // missiles fired this round
	};
	std::vector<SiloInfo> missileSilos() const;
	// The Titans' interior spawn points (as_titan_SpawnPoint children, moved
	// with the Titan); their objects.
	std::vector<uint32_t> titanSpawnPoints(int team) const;
	// Launches a soldier standing at a Titan's pod array (titan_pod_array1/2,
	// within an EntryPoint's entryRadius) or riding a pod-launching seat in
	// a drop pod (the launcher's projectileTemplate, a Parachute PCO with
	// isPod): false when there is none at hand. A seat's launcher is the
	// one on PIAltFire (the APCs' pod seats' alternate fire); `exitKey`
	// (exitThroughPodLauncher's exit, 0x004bda00 mask 0x400) takes the
	// seat's pod launcher whatever its input (the air transports' seats'
	// air_pod_launcher is on PIFire).
	bool launchPod(int player, bool exitKey = false);
	// For a soldier aboard its own Titan (within its box): the nearest entry
	// point of the Titan's pod arrays, in the world, and its radius.
	bool titanPodEntry(int player, physics::Vec3& entry, float& radius) const;
	// For the help manager's tick (0x00646cb0), on foot: a ladder (class
	// 0x9c40) whose object lies within `radius` of the soldier (0x00646880;
	// case 0x43 asks 3 m), and a vehicle with a free seat whose entry point
	// reaches him (case 0x47: 0x005a98e0 with 3 m; the rewrite's entering
	// test, enterOrLeaveVehicle's).
	bool ladderInReach(int player, float radius = 3) const;
	bool vehicleInReach(int player) const;
	// The nearest free gun seat of the player's own Titan whose gun works
	// (its console's entry point, world frame), for a soldier on foot within
	// the Titan's box; false for none.
	// `seat` >= 0 asks for that seat only; `skip`: seats others are going to.
	bool titanGunEntry(int player, physics::Vec3& entry, float& radius, int& seat, const std::vector<int>& skip = {}) const;
	bool ridingPod(int player) const;
	// Hanging under a soldier's parachute (Parachutes.cpp).
	bool ridingParachute(int player) const;
	// The soldier's falling flag (+0x328, read through interface 0xc4d7's
	// slot 0x78, 0x007f9bc0): the soldier update (0x0058d350) sets it while
	// he is on foot and his velocity's y is under -10 (*0x00a68e8c, static
	// init 0x008dfee0). The HUD's parachute help (0x00646cb0 case 0x0d)
	// and the parachute's jump key (0x004d2a50) ask it.
	bool soldierFalling(int player) const;
	// The parachutes in the air: object, rider (-1 none) and the state its
	// animation systems get (1 high, 2 near the ground, 4 landed).
	struct ParachuteInfo {
		uint32_t object = 0;
		int player = -1;
		int state = 1;
	};
	std::vector<ParachuteInfo> parachutes() const;
	struct PodInfo {
		uint32_t object = 0;
		int player = -1;
		physics::Vec3 position, velocity;
		bool controlsOff = false; // the spawn point's disablePodControls (0x0059d2a0)
	};
	std::vector<PodInfo> pods() const;

	// A soldier's hit capsules (setSkeletonCollisionBone) in world space as
	// of the last tick: segment ends and radius; for checks and drawing.
	struct Capsule {
		physics::Vec3 a, b;
		float radius;
		uint32_t material;
		int node = -1; // the bone's skeleton node (-1: no skeleton)
	};
	// The 3P hit reaction a round's hit on a living soldier plays (the
	// soldier's 0x005eda90, from the projectile's first hit, 0x005e7520:
	// the soldier still above 0 hit points): by the hit bone's node of
	// 3p_setup.ske (0xf left_shoulder / 0x11 left_low_arm: Left, 0x1f /
	// 0x21 the right ones: Right, 0x2f head: Head, else Body) and whether
	// the round flew along the soldier's facing (dot >= 0: from the Back)
	// or against it (Front): "hitBackLeft" ... "hitFrontBody", the
	// RandomTriggers of AnimationSystem3p.inc. Not in a vehicle.
	static const char* hitReaction(int node, const physics::Vec3& direction, float soldierYawDegrees);
	// A radio message's emote (TemplateBehaviours.cpp). The client that
	// hears a message (0x0042e0c0) has the sender's soldier, when it is
	// the sender's controlled object (class 0x9493: on foot, not on a
	// ladder), play the message's emote trigger (messages.addEmoteTrigger,
	// 0x004d7fe0) through the soldier's slot 0x88 (0x005edbf0). That plays
	// it in the 3P system (+0x2f0 == 1) through the system's slot 0x1c
	// (0x008b97f0, as the hit reactions) unless the emote is "", the
	// soldier sprints (slot 0x100: +0x37e), lies prone (+0x268 == 2) or
	// holds a GenericFireArm (class 0x94d3, the item in hand 0x005896a0)
	// without allowEmoteAnimation (template +0x31e, default 0, set by
	// 0x004e1c58). Whether the soldier may emote by that rule:
	static bool emoteAllowed(const std::string& emote, bool sprinting, SoldierPose pose,
		const world::ObjectTemplate* inHand);
	// The round's own player `player` (false when it doesn't play), and a
	// joined game's soldier ghost (animateRemoteSoldier's; `sprinting` the
	// client's own prediction's, false for others: the sprint is in the
	// control-object record only).
	bool playEmote(int player, const std::string& emote);
	bool playRemoteEmote(uint32_t object, const std::string& emote, bool sprinting = false);
	std::vector<Capsule> soldierCapsules(uint32_t soldier) const;
	// A downed soldier's or a body's ragdoll (null for others; docs/engine.md, "Ragdolls").
	const anim::RagDoll* ragDoll(uint32_t soldier) const;

private:
	world::ObjectTemplateManager& m_templates;
	world::ObjectManager& m_objects;
	Random m_random;
	Gaussian m_gaussian; // the deviation components' normal numbers
	RoundState m_state = RoundState::BeforeStart;
	std::vector<ControlPoint> m_controlPoints;
	std::vector<std::unique_ptr<ObjectSpawner>> m_spawners;

	const VehicleLayout& vehicleLayout(const world::ObjectTemplate& vehicle);
	std::map<std::string, VehicleLayout> m_layouts;

	struct Player;
	struct Body {
		uint32_t object;
		physics::RigidBody body;
		physics::ContactResponse response;
		// A walker's legs object's own response (MechLegs 0xc432, made by
		// its template's slot 31, 0x0062dc20): its sphere's contacts and
		// push out, solved after the hull's (solveLegs, 0x006ce740).
		physics::ContactResponse legsResponse;
		std::vector<physics::Vec3> points; // vehicle collision LOD
		std::shared_ptr<physics::CollisionWorld> projectileMesh; // object space, projectile LOD
		std::shared_ptr<physics::CollisionWorld> soldierMesh;    // object space, soldier LOD (ragdolls)
		// The parts with a collisionPart (0x0063ccc0: a child whose template
		// +0x74 is over 0 and whose physicsType is Mesh or MechLegs gets an
		// instance of the root's mesh at that geometry): each part's
		// projectile LOD in its own frame, posed as the part stands
		// (PartCollision.cpp). The tree down to them: each node's template
		// place in its parent's frame; `mesh` its entry in partMeshes.
		struct PartNode {
			std::string name;
			int parent = -1; // -1: the root
			physics::Mat3 rotation;
			physics::Vec3 position;
			int mesh = -1;
		};
		struct PartMesh {
			int node = -1;
			// The instance's sub-geometries' projectile LODs in the part's
			// frame: [0] the normal one, [1] a wreck's (null: none, and the
			// wrecked part is out of the rays: 0x006e5df0 returns 0).
			std::shared_ptr<const physics::CollisionWorld> lod[2];
			float radius = 0; // the LODs' farthest box corner from the part's origin
			// A Titan's: the TitanMesh whose instance it shares (the node
			// of the tree with the collision mesh, 0x0063d720's walk), and
			// whether that mesh is down with its part (rebuildTitanMeshes).
			int source = -1;
			bool off = false;
			// The instance's other LODs for the mesh physics (the part's
			// response, 0x005f6a60, in its root's chain, 0x006c0090: the
			// movers' candidates, 0x006dc7b0, and the soldiers', 0x006c3890),
			// per sub-geometry as `lod`: the vehicle LOD's faces, vertices
			// and box, the soldier LOD's faces.
			struct Physics {
				std::shared_ptr<const physics::CollisionWorld> vehicle, soldier;
				std::vector<physics::Vec3> points;
				std::vector<uint32_t> pointMaterials;
				physics::Vec3 boxMin, boxMax;
				float radius = 0;
			};
			Physics physics[2];
			const physics::CollisionMeshFile* file = nullptr;
			std::vector<uint32_t> materials;
			size_t geometry = 0;
			uint32_t collisionGroups = 0; // the part template's (response slot 0x50)
			// The part's own response (0x005f6a60; not a Spring's, whose
			// contacts are the Vehicle's wheels', nor a Titan's): the
			// contacts others record in it (slot 0x2c) and its own ground
			// pass and mover pass record, solved onto the root's node
			// (0x006ba7d0 with the root's node in the part's stead).
			bool ownResponse = false;
			// A mover of its own (0x005f6ba0's Basic node, the dispatch
			// 0x006dd0b0 through its root's chain): hasMobilePhysics and
			// hasCollisionPhysics with physicsType Mesh, and a vehicle LOD
			// (slot 0xc4(1)). A part without a mobile node keeps a static
			// node, asleep for ever (slot 0x14c), whose response is never
			// solved.
			bool mover = false;
			physics::ContactResponse response;
			// Its frame in the object's as of `partsPosed` (poseCollisionParts).
			mutable physics::Mat3 rotation;
			mutable physics::Vec3 position;
		};
		std::vector<PartNode> partNodes;
		std::vector<PartMesh> partMeshes;
		float partReach = 0; // a sphere about the origin the posed parts stay within
		mutable uint32_t partsPosed = 0xffffffffu; // the tick of the frames
		// The mesh instances in their wreck state (0x006539f0(.., 1) from the
		// armour's wreck, 0x00654120): the root's projectile LOD and each
		// part's switched to sub-geometry 1 (wreckCollision).
		bool collisionWrecked = false;
		// The root instance's sub-geometry (+0x14: 1 once wrecked when its
		// geometry has a second, 0x006e5df0 / 0x006e5e90), which every LOD
		// query of the instance goes through: the vehicle LOD's points,
		// edges, faces and box (setBodyLods), the soldier LOD, and the LOD
		// another mover asks it for (MeshActor::state).
		size_t meshState = 0;
		// A Titan's: what each team's soldiers meet (the soldier mesh and the
		// other team's forcefields; collisionGroups 0x100 / 0x200).
		std::shared_ptr<physics::CollisionWorld> teamSoldierMesh[2];
		std::unique_ptr<Vehicle> vehicle; // engines and wheels
		// A parachute's Wing and ForceObjects (the soldier's Parachute, the
		// supply crate's supply_parachute; Vehicle::fromTemplate's
		// flightParts): stepped with the body as a vehicle's parts are, but
		// not a vehicle to enter, drive or spawn at (Parachutes.cpp).
		std::unique_ptr<Vehicle> canopy;
		// A stationary PlayerControlObject (hasMobilePhysics 0 with seats:
		// the bipods eu_bipod / as_bipod / hmg_bipod, static_aa / ap / av).
		// The original gives it no mobile node (its collision is the static
		// world's, buildStaticCollision), but it is a control object to sit
		// in like any (0x005cdd60): the body carries its seats, guns,
		// cameras and radius (slot 0x90 with boundingRadiusModifierWhenOccupied,
		// 0x005c6be0) and never moves.
		bool fixed = false;
		// A walker's own animation system (bundleAnimationSystem: the
		// object 0x0063d340 makes for it, updated by 0x0063d2f0), built
		// the first tick it is needed.
		std::unique_ptr<anim::AnimationState> animation;
		bool animationTried = false;
		// Its bundleSkeleton (template +0x274: the skeleton helper's instance
		// +0x24, 0x0063d340) posed by that system, and the parts with a
		// boneIndex (0x0063d5e0) that take their bone's local matrix
		// (0x0063d470 -> the part's slot 0xe8): part name, bone, matrix.
		const anim::Skeleton* bundleSkeleton = nullptr;
		anim::LocalPose bonePose;
		struct BonePart {
			std::string part;
			int bone = -1;
			anim::Matrix matrix {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		};
		std::vector<BonePart> boneParts;
		bool bonesPosed = false; // boneParts' matrices are this tick's (partBone)
		// A joined client's ghost (animateRemoteVehicle): the system's input
		// from the ghost (its velocity, turn and taken seats) in place of the
		// body's own, which a client's round has no occupants for.
		struct RemoteDrive {
			bool on = false;
			physics::Vec3 velocity;
			float turn = 0, lastYaw = 0;
			bool occupied = false;
			int legMessages = -1; // the server's legs' (animateRemoteVehicle), -1 none
		} remoteDrive;
		// Seats: who sits in each (player id, -1 for nobody) and the guns.
		std::vector<int> occupants;
		// Its own team while nobody sits in it (the PlayerControlObject's
		// default team +0xd0, setDefaultTeam 0x005c7ee0; -1: not taken yet,
		// the object's team then).
		int defaultTeam = -1;
		double occupiedAt = -1e9; // when someone last sat in it (MagneticObject's disableWhenEmptyVehicle)
		// The target object's +0x1a0 (0x0058eba0): the last player to get in,
		// or to get out while others stayed; and the riders it last saw.
		int lastRider = -1;
		std::vector<int> ridersSeen;
		// One seat firearm's state (SeatLayout::weapons).
		struct Arm {
			std::unique_ptr<FireArm> weapon;
			const ProjectileSettings* projectile = nullptr;
			DeviationBuildUp deviation; // its SimpleDeviationComp's state (SeatWeapon::deviation)
			int barrel = 0;             // the MultiFireComp's next barrel (+0x1a8)
		};
		// A seat's guns. The Arm part is the PIFire group's active weapon
		// (SeatLayout::weapons[active[0]]), whose own entry in `arms` is
		// empty while it is; `arms` holds the others' state by index.
		struct Gun : Arm {
			float yaw = 0, pitch = 0; // the bundles' rotation, degrees (pitch up positive)
			float aiDeviation = 0;      // the component's +0x28 (setAiDeviation)
			bool manned = false;        // someone in its seat last tick (stepUnmannedGuns)
			float hullYaw = NAN;        // the hull's yaw at the last tick (ignoreParentRotation)
			// The pitch bundle's ignoreParentRotation: the hull's rotation at
			// the last tick and the pitch taken back so far, kept on top of
			// the look's pitch (input.aimPitch is the look, not a change).
			physics::Mat3 hullTurn;
			float pitchOffset = 0;
			uint64_t gunnedTick = 0;    // the last tick its seat was taken (stepGunner)
			float stabilizedHeading = 0; // the bundle's parent's yaw last undone (stabilizeGunYaws)
			std::vector<Arm> arms;
			int active[3] = {-1, -1, -1}; // each group's active weapon (PCO +0xa8 + group x 4), -1 none
			bool specialHeld = false;     // specialToggleWeaponInput held (PCO +0x14c)
			bool toggleWas = false;       // c_PIToggleWeapon last tick
			bool selectWas = false;       // a c_PIWeaponSelect key last tick
			int recoilWeapon = -1;        // the weapon whose slide gunRecoil holds
			// An active weapon's blockInputOnRotBundle names the seat's yaw or
			// pitch bundle: that bundle ignores the gunner (stepGunner).
			bool yawBlocked = false, pitchBlocked = false;
		};
		std::vector<Gun> guns;
		// Armoured children with armor.mirrorParentDamageLevel or
		// hudRepairCellNo (the heavy IFV's seven ifv_shield_the_generator,
		// 110 hit points each; SeatBehaviours.cpp).
		struct Cell {
			const world::ObjectTemplate* t = nullptr;
			float hitPoints = 0, maxHitPoints = 0;
			bool wreck = false;
			bool mirror = false;   // mirrorParentDamageLevel (getter slot 0xac)
			int hudCell = -1;      // hudRepairCellNo
			float timer = 0.1f;    // the armour's +0x288 (+0x28c: 0.1 s)
			bool pointOnDestroy = false; // damagePointOnDestroy (armour slot 0x16c)
			physics::Mat3 rotation;  // its place in the vehicle's frame
			physics::Vec3 position;
			// Its armour's effects (armorEffectsAfter, anchored on the
			// cell's id): e_healing_node at 99, e_titan_consol at 0 and
			// e_bluespark_loop at -1.
			float effectHitPoints = 0;
			int effectLevel = 0;
			bool effectsAttached = false, effectsSeen = false;
			bool synced = false; // a joined client's: its hit points from the host (setRemoteRepairCells)
		};
		std::vector<Cell> cells;
		bool cellsBuilt = false;
		// Each seat's camera angles and speeds (SeatLayout::camera), degrees.
		struct Look {
			float yaw = 0, pitch = 0, yawSpeed = 0, pitchSpeed = 0;
			physics::Mat3 hullTurn; // the hull's rotation at its last step (ignoreParentRotation)
			uint64_t tick = 0;      // that step's tick
		};
		std::vector<Look> looks;
		// Armour (armor.*): the EMP level (armor 0x27c, see empBlast),
		// falling by one every empLevelTime; destroyed, it lies as a wreck
		// for timeToStayAsWreck.
		float empLevel = 0;
		float wreckTime = -1; // >= 0 while a wreck
		// The armour's water timers (0x006521d0): in water (+0xf8, from
		// waterDamageDelay) and in deep water (+0xf4, from
		// deepWaterDamageDelay); -1 while out of it.
		float waterTimer = -1, deepWaterTimer = -1;
		// The armour's effects (ArmorEffects, stepArmorEffects): the hit
		// points last seen (+0x254), the level started (+0x184), the last
		// damage's spectacular flag (+0x250) and whether some follow it.
		float effectHitPoints = 0;
		int effectLevel = 0;
		bool spectacular = false;
		bool effectsAttached = false;
		bool effectsSeen = false; // effectHitPoints set (its first look: the template's armor.hitPoints)
		// The vehicle LOD's edges (indices into `points`) of its faces
		// steeper than 30 degrees (|normal y| at most cos 30), each face's
		// three in turn (the loader 0x006e12a0 -> 0x006e1430, LOD +0xe4).
		std::vector<std::array<uint16_t, 2>> edges;
		// The vehicle LOD's box (object space; the mesh's slot 0x54), which
		// the third pass tests trees against.
		physics::Vec3 boxMin, boxMax;
		// The soldier LOD's box (the mesh's slot 0x54(2)): where a restricted
		// exit's ray starts (0x005c6ff0).
		physics::Vec3 soldierBoxMin, soldierBoxMax;
		bool hasSoldierBox = false;
		// The dontClearTeamOnExit (template +0x34f, read by 0x005c95b0 as a
		// seat is left) of the seat last left: emptied, it keeps its team.
		bool dontClearTeam = false;
		// The armour's spectacular death (+0x250 at the killing hit) with
		// hideChildrenOnSpectacularDeath (template +0x164, 0x00651c60): the
		// mesh's parts after the first are hidden (zero matrices).
		bool hideChildren = false;
		bool destroyNow = false; // destroyOnSpectacularDeath (+0x251)
		// The wreck: the time it has stood, how long its armour drains to
		// -wreckHitPoints (timeToStayAsWreck, a third of it with the AI
		// running: 0x00652380's x 3) and the fade after (timeToStayAfterDestroyed).
		float wreckAge = 0, wreckDrain = 0;
		// Each seat gun's WeaponBasedRecoilComp slide (GunRecoil), by seat.
		std::vector<GunRecoil> gunRecoil;
		// The active defence's shield (ActiveDefenseAmmoComp 0x005d97f0: the
		// root's slot 0x118 while the ADS weapon's fire state is 4, one
		// round's interval): its time left, radius and material (slot 0x11c).
		float shieldLeft = 0, shieldRadius = 0;
		int shieldMaterial = 0;
		// Its first Engine (0x006b6c50: the tree searched siblings first)
		// is a helicopter's (engineType c_ETHelicopter, 33): hitting a static
		// object is the end of it (0x006bb050's early exit).
		bool helicopterEngine = false;
		// For the mobile mesh physics (0x006bb050, see MeshCollision.cpp):
		// the vehicle LOD in object space, its vertices' materials, its
		// radius (the mesh's slot 0x50: the box's farthest corner), the
		// template's mass (+0x94, 1 by default) and isMajorActor (+0xf0),
		// whether it is a PlayerControlObject (the edge pass and the
		// helicopter exit ask for one) or a Particle (0x947b), and its
		// trunk (a type-4 LOD, or treeCollisionDiameter / 2 above 0).
		std::shared_ptr<physics::CollisionWorld> vehicleMesh;
		std::vector<uint32_t> pointMaterials;
		float radius = 0;
		// The object's radius (object slot 0x90, 0x006f6b90): its collision
		// mesh's (slot 0x74, 0x006dedb0: the first part with a LOD, its LOD
		// of the lowest collision type, that LOD's radius) or, farther, a
		// child's with one (its offset's length plus its own); and the
		// width across x of its mesh's box (slot 0x9c's +0x24: every part's
		// and LOD's box, 0x006deeb0). Explosions use them (0x005a9090,
		// 0x005a66e0).
		float objectRadius = 0;
		float boxWidth = 0;
		// Touching nothing since its last step (the physics node's +0x249,
		// slot 0x1b8): the radius trigger's radiusDetonateWithAirVehicles.
		bool inAir = false;
		float mass = 1;
		bool majorActor = false;
		bool controlObject = false;
		bool particle = false;
		std::shared_ptr<physics::CollisionWorld> trunk;
		float trunkRadius = 0;
		bool trunkFromTypeFour = false;
		// The position before this tick's step (the helicopter exit's start).
		physics::Vec3 lastPosition;
		// The wheels' parts' radius (their vehicle LOD's, slot 0x50), by wheel.
		std::vector<float> wheelRadii;
		uint32_t material = 0; // the template's material
		// Sleeping (the node's counter +0x68 and its history).
		physics::SleepState sleep;
		bool held = false; // held at its spawner (holdObject, 0x005af230: the node's motion stopped, slot 0x148)
		// The response's support (+0xf4, 0x006bb050): a heavier mobile object
		// it touched; asleep, it wakes when that one is gone or awake.
		uint32_t support = 0;
		// Drag and water (the template's drag +0x78, dragModifier +0x88,
		// floaterMod +0xcc, default 1) and how deep it is (node +0x5c).
		float drag = 0;
		physics::Vec3 dragModifier {1, 1, 1};
		float floaterMod = 1;
		float submerged = 0;
		// collisionGroups (template +0x104 -> response +0x1c, slot 0x50).
		uint32_t collisionGroups = 0;
		// otherCollisionLod (SimpleObject +0xec, executor 0x0051ee4d; -1,
		// that is 1, by default; the node's slot 0x1a4): the LOD of the
		// other object its mesh meets (MeshCollision.cpp); its collision
		// mesh and material map for the others' LODs it may be asked for.
		int otherCollisionLod = 1;
		const physics::CollisionMeshFile* meshFile = nullptr;
		std::vector<uint32_t> meshMaterials;
		// handleCollisionSpeed (template +0x200, default 1; node +0x14).
		float handleCollisionSpeed = 1;
		// The response's contact effect and contact sound (0x006b75d0:
		// +0x130.. and +0x16c..), played at the end of its solve.
		struct ContactEffect {
			int state = 0;        // 0 free, 1 recorded, 2 played
			float strongest = 0;  // |v|^2
			uint32_t first = 0, second = 0; // the materials
			float timer = 0;      // 0.5..1.5 s
			physics::Vec3 point, normal, direction;
			std::string sound;
		};
		ContactEffect contactEffect, contactSound;
		// The launcher its response ignores (noInitialCollisionWithLauncher,
		// 0x005b48b0 -> 0x006b8de0: +0xfc) until the tick `ignoreUntil`
		// (+0xf8, the tick + 2 on the server, kept open while they touch).
		uint32_t ignored = 0;
		uint32_t ignoreUntil = 0;
		bool ignoreSet = false;
		// A GenericProjectile (0x9539) with a soldier LOD under 0.6 m in
		// radius: soldiers push it instead of standing against it
		// (0x006c3890); its launcher (the projectile interface's slot 0x48)
		// neither.
		bool pushedBySoldiers = false;
		uint32_t launcher = 0;
	};
	// The collision events (object slot 0xac, BF2142.exe's base
	// 0x0063c9e0, a Soldier's 0x00586a90): `self` hit `other` (0 the
	// ground) with `velocity` relative to it. Through the armour's one-second
	// memory of each pair (0x00653ef0) to the server game logic's slot 0x104
	// (0x004ba3f0): the ground's damage (0x005a3db0) or an object's
	// (0x005a4690, a soldier's road kill 0x005a43c0).
	void collisionEvent(uint32_t self, uint32_t other, const physics::Vec3& velocity, const physics::Vec3& normal,
		const physics::Vec3& point, uint32_t ownMaterial, uint32_t otherMaterial);
	void groundCollisionDamage(uint32_t victim, const physics::Vec3& velocity, const physics::Vec3& normal,
		uint32_t groundMaterial, uint32_t victimMaterial);
	void objectCollisionDamage(uint32_t attacker, uint32_t victim, const physics::Vec3& velocity,
		const physics::Vec3& normal, uint32_t attackerMaterial, uint32_t victimMaterial);
	void roadKill(uint32_t attacker, Player& victim, const physics::Vec3& velocity, uint32_t attackerMaterial,
		uint32_t victimMaterial);
	int collisionCredit(uint32_t attacker, uint32_t victim, const physics::Vec3& velocity) const;
	float collisionMassFactor(uint32_t attacker, uint32_t victim) const;
	struct CollisionMemory {
		uint32_t other;
		uint32_t ownMaterial, otherMaterial;
		float left;
	};
	std::map<uint32_t, std::vector<CollisionMemory>> m_collisionMemory; // by the armoured object
	// The contact effects (0x006b75d0, 0x006ba7d0's end, 0x006b8c40) and
	// their cooldown (response slot 0x9c, 0x006b74e0).
	void recordContactEffect(Body& body, uint32_t first, uint32_t second, const physics::Vec3& velocity,
		const physics::Vec3& point, const physics::Vec3& normal);
	void playContactEffects(Body& body);
	void coolContactEffects(Body& body, float seconds);
	Random m_contactRandom; // the shared generator's draws (0x00a05b30) for the effects' timers
	void sleep(Body& body);
	// Slot 0x160: a sleeping PlayerControlObject root wakes what is round it
	// (0x006bfad0) as well.
	void wake(Body& body);
	// Asleep, or held by its spawner (its motion stopped: a candidate the
	// movers meet, moved by nothing).
	static bool asleep(const Body& body) { return body.sleep.asleep() || body.held; }
	void stepSleep(Body& body, float seconds);
	void destroyVehicle(Body& body, int attacker);
	// The armour's wreck (0x00654120) sends its object message 0x18; the
	// object's handler (0x0063c6a0) on the server, for an armoured object
	// whose Radio part has destroyedGameEvent N (Radio template +0x1f8, -1
	// none: eu_forcefield's and as_titan_forcefield's 1), sends N + 0x1c to
	// every spawner (the game logic's list, slot 0x1ec; 0x005ad470):
	// a Titan's shield down switches on the gpm_ti maps' activateOnEvent 1
	// spawners (the APC pods) and off Verdun's deActivateOnEvent 1 FAVs.
	void destroyedGameEvent(const world::ObjectTemplate& destroyed);
	void stepVehicleDamage(float seconds);
	// A vehicle's armour effects (VehicleParts.cpp): its template's lists
	// (cached), the change after its hit points moved (0x00654730 /
	// 0x006549a0) and the attached ones following it.
	struct ArmorEffectList {
		struct Effect {
			int level = 0;
			std::string effect;
			physics::Vec3 position;
			physics::Mat3 rotation;
			bool spectacular = false;
		};
		std::vector<Effect> effects; // by level, highest first
		std::vector<int> levels;     // the effects' levels, the same order
		float maxHitPoints = 0, wreckHitPoints = 0, hitPoints = 0;
	};
	const ArmorEffectList& armorEffectList(const world::ObjectTemplate& t);
	std::map<const world::ObjectTemplate*, ArmorEffectList> m_armorEffectLists;
	void armorEffectsAfterDamage(Body& body, float hitPoints);
	// One armour's effect state (the armour's +0x254, +0x184, whether some
	// follow it, whether +0x254 was set), held where that armour is kept.
	struct ArmorEffectState {
		float& hitPoints;
		int& level;
		bool& attached;
		bool& seen;
	};
	// 0x006549a0 / 0x00654730 for any armour: its effects placed from `at`
	// and `turn` (the attached ones on `anchor`, an id followArmorEffects
	// moves by armorAnchorFrame).
	void armorEffectsAfter(const ArmorEffectList& list, ArmorEffectState state, uint32_t anchor, const physics::Vec3& at,
		const physics::Mat3& turn, bool spectacularHit, float hitPoints);
	bool armorAnchorFrame(uint32_t anchor, physics::Vec3& at, physics::Mat3& turn) const;
	// The meshes of the attached armour effects (armorEffectMeshes): each in
	// its anchor's frame.
	struct ArmorMesh {
		uint32_t anchor = 0;
		const world::ObjectTemplate* objectTemplate = nullptr;
		physics::Vec3 position;
		physics::Mat3 rotation;
	};
	std::vector<ArmorMesh> m_armorMeshes;
	void stopArmorEffects(uint32_t anchor);
	static constexpr uint32_t kArmorEffectTag = 0x7d000000u;
	std::vector<uint32_t> m_armorAnchors; // the bodies and cells whose armour effects follow them
	void followArmorEffects();
	// The tracer rounds' tracers this tick, handed to the effects to draw.
	void placeTracers();
	std::vector<std::pair<std::string, std::vector<effects::MeshSprite>>> m_tracerMeshes;
	// The exit a restricted seat allows (0x005c7840 over 0x005c71a0's four
	// places); false when none does.
	bool restrictedExit(const Body& body, const VehicleLayout& layout, int seat, physics::Vec3& at) const;
	static physics::Vec3 exitRayStart(const Body& body, const physics::Vec3& place);
	// A vehicle's team is its first occupant's (0x005c9500), its own when
	// empty: each tick from the seats.
	void stepVehicleTeams();
	// The levels' Trigger and AmbientEffectArea objects (AmbientTriggers.h,
	// Ambience.cpp): linked at the round's start, tested each tick, set off
	// by shots (event 2) and explosions (event 8) too.
	AmbientTriggers m_ambience;
	std::map<int, physics::Vec3> m_ambiencePlaces; // the players' last places (their speed)
	uint32_t m_ambienceTag = 0;
	std::vector<AmbientEvent> m_ambientEvents;
	void startAmbientEffects();
	void linkAmbience();
	void stepAmbience(float seconds);
	void ambientShot(const Player& player);
	void ambientExplosion(const physics::Vec3& at);
	// The lock's transitions after stepWeaponLock (LockFeedback.cpp).
	void lockFeedback(Player& player, uint32_t targetBefore, int stateBefore);
	void missileAway(const Player& player);
	void lockEvent(LockEvent::Kind kind, const Player& player, uint32_t target);
	void playHitReaction(Player& victim, int node, const physics::Vec3& direction);
	// The seats' guns nobody uses (ammo.reloadWithoutPlayer; TemplateBehaviours.cpp).
	void stepUnmannedGuns(float seconds);
	// The vehicles' and seats' template behaviours (SeatBehaviours.cpp):
	// automaticYawStabilization, the rolling loops, the shield cells.
	void stepSeatBehaviours(float seconds);
	void stabilizeGunYaws(Body& body);
	void stepRollingSound(Body& body);
	void stepShieldCells(Body& body, float seconds);
	struct Rolling {
		// The first two WheelEffectBundles of the tree (0x006bf940 at the
		// PCO's creation 0x005cd4f0, +0x318 / +0x31c): their material and
		// the spring (wheel) they hang under.
		struct Bundle {
			uint32_t material = 0;
			int wheel = -1;
		};
		std::vector<Bundle> bundles;
		// The sound object (0x34 bytes): touching (+4), changed (+5), the
		// ground material (+8) and the bundle's (+0xc), the volume wanted
		// (+0x10) and eased (+0x2c), the state (+0x20: 0 waiting, 1 on) and
		// its ticks (+0x24), the sound chosen.
		bool contact = false, changed = false;
		int ground = -1;
		uint32_t own = 0;
		float wanted = 0, eased = 0;
		int state = 1, ticks = 0;
		std::string sound;
		bool playing = false;
	};
	std::map<uint32_t, Rolling> m_rolling;
	std::vector<LockEvent> m_lockEvents;
	std::vector<LockTone> m_lockTones;
	// The close-to scan's state per player (kept over respawns as the
	// player object's own +0x16c..+0x178 are): the countdown and the
	// objects in range, by object id (world objects) or projectile serial
	// (bit 32 set), with their type and state (0 new, 1 seen again, 2 to
	// be checked: 0x0059a6c0's +0x10).
	struct CloseToEntry {
		uint64_t key = 0;
		int type = 0;
		int state = 0;
	};
	struct CloseToScan {
		float countdown = 0;
		std::vector<CloseToEntry> entries;
	};
	std::map<int, CloseToScan> m_closeTo;
	// A template's Radio rows (distance, type, 2-D), read once.
	struct CloseToRows {
		int distance = 0, type = 0;
		bool flat = false;
	};
	std::map<const world::ObjectTemplate*, CloseToRows> m_closeToRows;
	std::vector<CloseToEvent> m_closeToEvents;
	void stepCloseTo(float seconds);
	void scanCloseTo(Player& player, CloseToScan& scan);
	void clearCloseTo(int player, CloseToScan& scan);
	std::vector<AiHit> m_aiHits;
	std::vector<UseKeyExit> m_useKeyExits;
	struct {
		std::string weapon; // the weapon whose tones play
		int state = 0;      // the runtime WeaponHud's +0x10
	} m_lockTone;
	void stepLockTone();
	std::map<std::pair<int, int>, std::array<std::optional<SquadRequest>, 4>> m_squadRequests; // (team, squad)
	// The combat areas' rules each tick (0x004bbf60, 0x004bed00).
	void stepCombatAreas(float seconds);
	// Titan mode: the soldier of a player inside the enemy Titan's hull
	// while its shield is up dies (Titans.cpp, 0x004bbf60's tail).
	void stepTitanHullKills();
	bool insideCombatArea(const Player& player) const;
	std::map<uint32_t, float> m_outsideVehicles; // +0x304: abandoned vehicles' seconds outside
	FriendlyFire m_friendlyFire;
	bool m_friendlyFireWithMines = true;
	bool m_hitIndicatorEnabled = true;
	// The seats' IK objects (seatIkTargets): the template path from the
	// vehicle's root, by root template and seat (ik null: none).
	struct IkPath {
		const world::ObjectTemplate* ik = nullptr;
		std::vector<const world::ChildTemplate*> path;
		// Its ik.* (left, right): position, rotation (degrees), pole.
		physics::Vec3 grips[2][3];
	};
	mutable std::map<std::pair<const world::ObjectTemplate*, std::string>, IkPath> m_ikPaths;
	const IkPath& seatIkPath(const world::ObjectTemplate& root, const SeatLayout& seat) const;
	bool disabled(const Body& body) const { return body.empLevel > 0 || body.wreckTime >= 0; }

	void seatPlayer(Player& player, Body& body, int seat);
	// `groups`: which weapon groups fire (bit 0 PIFire, 1 PIAltFire, 2
	// PIFlareFire); `aims`: the look turns the seat's bundles (not a
	// pilot's, whose mouse flies the vehicle).
	void stepGunner(Player& player, Body& body, float seconds, bool aims = true, unsigned groups = 7);
	// A seat's weapon choice (0x004d2a50 for the player's control object):
	// c_PIWeaponSelect1..9 take the weapon of that item index in each group
	// (selectable and usable, slot 0x170), c_PIToggleWeapon and the seat's
	// specialToggleWeaponInput the next of the group by item index.
	void selectSeatWeapons(Player& player, Body& body, const SeatLayout& seat);
	// The player's may-use test (player slot 0x384, 0x005921e0; see
	// GameWorld.cpp): `selectable` the weapon's slot 0x170.
	bool mayUseWeapon(const Player& player, const std::string& weapon, bool selectable, bool forced) const;
	// Makes `index` its group's active weapon (0x005c9840): the old one's
	// disable (slot 0x1a8), the new one's enable (0x1a4).
	void activateSeatWeapon(Body& body, int seat, const SeatLayout& layout, int index);
	// The seat's firing of one of its weapons (its round, recoil and sound).
	void fireSeatWeapon(Player& player, Body& body, const SeatLayout& seat, const SeatWeapon& weapon,
		Body::Arm& arm, int index, float seconds, bool trigger);
	// The active defence (ADS): its shield on the body, and a projectile
	// with activeDefenseEnabled entering it (0x006c07a0 -> 0x006bf220).
	void raiseShield(Body& body, const SeatWeapon& weapon, const FireArm& firearm);
	// Where a seat's part sits now (SeatLayout::Anchor): the layout's
	// at-rest place (vehicle frame) carried by the yaw of the seat whose
	// turret it rides (about that seat's yaw pivot) and by its bone part's
	// posed matrix in place of that part's placement (0x0063d470 -> slot
	// 0xe8, as partBone gives it to the drawing).
	struct SeatMount {
		physics::Mat3 turn;
		physics::Vec3 at;
		physics::Vec3 place(const physics::Vec3& v) const { return at + turn.transform(v); }
	};
	SeatMount seatMount(const Body& body, const VehicleLayout& layout, const SeatLayout::Anchor& anchor) const;
	// The bone part's share of a mount (0x0063d470 -> slot 0xe8): its
	// bone's local matrix in place of the part's placement, after `m`'s turret turn.
	void boneMount(uint32_t object, const SeatLayout::Anchor& anchor, SeatMount& m) const;
	// The frame the seat's gun turns in (the mount's turn on the hull's).
	physics::Mat3 gunHull(const Body& body, const VehicleLayout& layout, const SeatLayout& seat) const
	{
		return seatMount(body, layout, seat.gunAnchor).turn * body.body.rotation;
	}
	// Whether a seat's guns turn with its look: a bundle the look turns
	// (yawBundle / pitchBundle); a flying vehicle's pilot keeps its guns
	// along the hull (the mouse flies it).
	bool seatAims(const Body& body, int seat) const;
	// The camera a seat weapon fires in with fireInCameraDof (0x005f1a20:
	// the control object's active camera, the one its zoom.startCameraId
	// takes up, 0x005db260, else the seat's first); null without one.
	static const SeatLayout::SeatCamera* fireCamera(const SeatLayout& seat, const SeatWeapon& weapon);
	static physics::Vec3 seatGunOrigin(const SeatLayout& seat, const SeatWeapon& weapon, Body::Gun& gun, int& barrel);
	static physics::Vec3 seatGunOrigin(const SeatLayout& seat, const SeatWeapon& weapon, float gunYaw, float gunPitch, int& barrel);
	Body* findBody(uint32_t object);
	const Body* findBody(uint32_t object) const { return const_cast<GameWorld*>(this)->findBody(object); }
	int firstEngineType(const world::ObjectTemplate& root) const;
	mutable std::map<const world::ObjectTemplate*, int> m_engineTypes; // firstEngineType's answers, by root
	void addBodies();
	bool buildBody(const world::ObjectTemplate& t, uint32_t id, const physics::Vec3& position,
		const physics::Mat3& rotation, Body& body);
	// PartCollision.cpp: the collisionPart children's projectile LODs
	// (0x0063ccc0), their live frames, the wreck state (0x006539f0) and
	// the segment against a body's projectile LOD and its parts, in the
	// body's frame (every round, line-of-sight and drop ray goes through it).
	void buildCollisionParts(Body& body, const world::ObjectTemplate& t, const physics::CollisionMeshFile& mesh);
	// 0x0063ccc0's walk below `owner` (a node with a mesh instance of its
	// own at `node`, -1 the root, placed at `rotation` / `position` in the
	// body's frame): each child with a collisionPart gets `mesh`'s
	// geometry, its faces reporting `faceOwner`.
	void addCollisionParts(Body& body, const world::ObjectTemplate& owner, int node, const physics::Mat3& rotation,
		const physics::Vec3& position, const physics::CollisionMeshFile& mesh, const std::vector<uint32_t>& materials,
		uint32_t faceOwner, int source);
	static void finishCollisionParts(Body& body);
	// A child whose collisionPart takes an instance (physicsType Mesh or MechLegs).
	static bool takesCollisionPart(const world::ObjectTemplate& t);
	void poseCollisionParts(const Body& body) const;
	void wreckCollision(Body& body);
	// The body's vehicle LOD (points, their materials, edges, box,
	// radius, faces) and soldier LOD at the root instance's sub-geometry
	// `state`; false when it has no vehicle LOD there.
	bool setBodyLods(Body& body, const physics::CollisionMeshFile& mesh, const std::vector<uint32_t>& materials,
		size_t state);
	// `part`: the collisionPart the segment met (its partMeshes index), -1
	// the root's faces.
	bool raycastBody(const Body& body, const physics::Vec3& localFrom, const physics::Vec3& localTo,
		physics::CollisionWorld::Hit& hit, int* part = nullptr) const;
	bool stepBody(Body& body, float seconds);
	void stepPhysics(float seconds);
	// A walker's animation system fed as 0x0063ce30 does (see GameWorld.cpp).
	void stepObjectAnimation(Body& body, float seconds);
	std::map<uint32_t, uint32_t> m_objectEffectEvents;
	std::map<uint32_t, std::vector<std::string>> m_objectWeaponFires;
	std::vector<RadioHeard> m_radioHeard;
	std::vector<TakeableChange> m_takeable;
	const io::FileManager* m_files = nullptr;
	physics::CollisionTemplateManager* m_collisionTemplates = nullptr;
	physics::Ground m_ground;
	const physics::MaterialManager* m_materials = nullptr;
	std::vector<uint32_t> m_checked; // object ids already considered for a body (sorted)
	std::vector<Body> m_bodies;
	physics::CollisionWorld m_collision; // static objects, soldier LOD
	physics::CollisionWorld m_projectileCollision; // static objects, projectile LOD
	// The static objects' projectile-LOD radius (the largest part's), for
	// the penetration's way out (2.1 x the object's radius).
	std::unordered_map<uint32_t, float> m_staticProjectileRadius;

	// The mobile mesh physics between objects (BF2/Physics, see
	// MeshCollision.cpp): the static objects' parts (each template of a
	// static object's tree with a collision mesh), in a grid for the
	// broad phase (0x006dc7b0).
	struct MeshShape {
		std::shared_ptr<physics::CollisionWorld> mesh; // the vehicle LOD's faces, object space
		std::vector<physics::Vec3> points;             // its vertices
		std::vector<uint32_t> pointMaterials;          // their materials (through mapMaterial)
		physics::Vec3 boxMin, boxMax;                  // its box (the mesh's slot 0x54)
		float radius = 0;                              // slot 0x50
		bool hasVehicleLod = false;
		std::shared_ptr<physics::CollisionWorld> trunk; // the type-4 faces, or the trunk line
		bool trunkFromTypeFour = false;
		const physics::CollisionMeshFile* file = nullptr; // what it was made from (another LOD's faces)
		std::vector<uint32_t> materials;
		size_t geometry = 0, state = 0; // the instance's geometry (a collisionPart's) and sub-geometry
	};
	// A mesh's faces at another LOD (0 projectiles', 2 soldiers'), its box
	// and radius (slots 0x54 / 0x50 of that LOD), made when first asked.
	struct LodFaces {
		std::shared_ptr<physics::CollisionWorld> mesh;
		physics::Vec3 boxMin, boxMax;
		float radius = 0;
	};
	std::map<std::tuple<const physics::CollisionMeshFile*, std::vector<uint32_t>, int, size_t, size_t>, std::shared_ptr<const LodFaces>>
		m_lodFaces;
	const LodFaces* lodFaces(const physics::CollisionMeshFile& file, const std::vector<uint32_t>& materials, int lod,
		size_t geometry = 0, size_t state = 0);
	struct StaticPart {
		uint32_t object = 0;
		bool root = true;    // the object itself, not a template of its tree
		bool ladder = false; // template class Ladder (40000: 0x006bb050 leaves it out)
		physics::Mat3 rotation;
		physics::Vec3 position;
		std::shared_ptr<const MeshShape> shape;
		float trunkRadius = 0; // the node's +0x198: treeCollisionDiameter / 2 (0x005f6ba0), 0.3 under 0.01 with a type-4 LOD
		uint32_t material = 0; // the template's material
		uint32_t stamp = 0;
		uint32_t collisionGroups = 0;
	};
	void addStaticPart(StaticPart part);
	std::vector<StaticPart> m_staticParts;
	std::unordered_map<int64_t, std::vector<uint32_t>> m_staticGrid; // 16 m cells (x, z) -> parts
	std::map<std::tuple<const physics::CollisionMeshFile*, std::vector<uint32_t>, size_t, size_t>, std::shared_ptr<const MeshShape>>
		m_shapes;
	uint32_t m_staticStamp = 0;
	// An instance's vehicle LOD at `geometry` (0, or a collisionPart's) and
	// sub-geometry `state`.
	std::shared_ptr<const MeshShape> meshShape(const physics::CollisionMeshFile& mesh, const std::vector<uint32_t>& materials,
		size_t geometry = 0, size_t state = 0);
	// One side of a pair: a body (mobile, a root) or a static part.
	struct MeshActor {
		Body* body = nullptr;
		const StaticPart* part = nullptr;
		const std::vector<physics::Vec3>* points = nullptr;      // the vehicle LOD's vertices
		const std::vector<uint32_t>* pointMaterials = nullptr;
		const std::vector<std::array<uint16_t, 2>>* edges = nullptr;
		const physics::CollisionWorld* mesh = nullptr;            // object space
		const physics::CollisionWorld* trunk = nullptr;           // object space
		bool trunkFromTypeFour = false;
		physics::Vec3 boxMin, boxMax;
		float radius = 0, trunkRadius = 0;
		physics::Mat3 rotation;
		physics::Vec3 position;
		physics::Vec3 centerOfMass;   // object space
		physics::Vec3 velocity;       // the tick's average (node +0x44)
		bool fixed = false;           // the non-mobile response (0xc430)
		bool controlObject = false, helicopter = false, ladder = false, particle = false;
		uint32_t material = 0; // the template's material (pass 2's B side)
		uint32_t collisionGroups = 0;
		int team = 0; // a PlayerControlObject's (interface 0xc4c5 slot 0x9c)
		physics::ShareNode node;
		// Its otherCollisionLod (what LOD of B it asks for) and its mesh's
		// source, for B's faces at another's ask (atLod).
		int otherLod = 1;
		// A body's part with a response of its own (Body::PartMesh::
		// ownResponse): its index there; -1 for a root (its contacts are
		// the root's response's).
		int partMesh = -1;
		const physics::CollisionMeshFile* file = nullptr;
		const std::vector<uint32_t>* materials = nullptr;
		size_t geometry = 0, state = 0; // the instance's geometry and sub-geometry (a wreck's 1)
		physics::Vec3 pointVelocity(const physics::Vec3& point) const
		{
			return body ? body->body.pointVelocity(point) : physics::Vec3 {};
		}
	};
	MeshActor actorOf(Body& body) const;
	MeshActor actorOf(const StaticPart& part) const;
	// B as A asks for it (otherCollisionLod): its faces, box and radius at
	// `lod`; false when its mesh has none there (no pair). Actors without
	// a mesh source (the Titans' parts) stay at their vehicle LOD.
	bool atLod(MeshActor& actor, int lod);
	void collideBodies(float seconds);
	void collideMover(Body& mover, size_t index, float seconds);
	// A mover's candidates (0x006dc7b0 with `m` as param_3): the static
	// parts near it, the other bodies' roots and parts that weren't movers
	// before it this frame, the Titans' parts. `m` is the root (mRoot) or
	// one of its mover parts. False when the mover was destroyed.
	bool collideCandidates(Body& mover, size_t index, MeshActor& m, bool mRoot, float seconds);
	// The parts as movers of their own, after their root (the dispatch's
	// chain, 0x006dd1b0): each mover part's ground pass (0x006b9260) and
	// broad phase (0x006dc7b0); their responses are solved onto the root
	// after its own (solveParts).
	void partsMove(Body& mover, size_t index, float seconds);
	void solveParts(Body& body);
	// Whether one of the body's parts was a mover this frame already.
	bool partMoves(const Body& body, const MeshActor& part) const;
	// A body's posed collisionPart children as candidates (their vehicle
	// LOD in their frame now); empty when wrecked without a wreck shape.
	void partActors(Body& body, std::vector<MeshActor>& out);
	static void partPhysics(Body::PartMesh::Physics& out, const physics::CollisionMeshFile& mesh,
		const physics::CollisionSubGeometry& sub, const std::vector<uint32_t>& materials, uint32_t owner, size_t geometry,
		size_t state);
	// Slot 9 of the mobile mesh response (0x006bb050): A's mesh against
	// B's; `trunkOnly` runs the third pass alone (its last argument).
	// Returns false when it destroyed A (the helicopter exit).
	bool meshPair(MeshActor& a, MeshActor& b, float seconds, bool trunkOnly);
	void wheelProbes(Body& body, MeshActor& b, float seconds);
	// A walker's legs' sphere against B (the MechLegs response's slot 0x24).
	void legsSphere(Body& walker, MeshActor& a, MeshActor& b);
	// The legs object as a mover: its candidates and legsSphere with each.
	void legsAgainstObjects(Body& mover, float seconds);
	// A body whose response is a walker's legs (MechLegs, 0xc432), not a
	// wreck: its contacts are its legs' only (solveLegs).
	static bool walker(const Body& body);
	static bool walker(const MeshActor& actor) { return actor.body && walker(*actor.body); }
	// A walker's feet on the objects under them (0x00650e90's second probe:
	// a sphere at each foot against the objects' vehicle LODs), before its
	// update.
	void feetOnObjects(Body& walker);
	// Whether a sphere (world centre and radius) touches the vehicle LOD of
	// a static part or of another body (its root or a posed part).
	bool sphereTouchesObjects(Body& self, const physics::Vec3& centre, float radius);
	physics::MassShares addMeshContact(MeshActor& a, MeshActor& b, const physics::Vec3& point, const physics::Vec3& normal, float depth,
		uint32_t materialA, uint32_t materialB, bool trunkPass, bool toA = true);
	std::vector<uint32_t> materialMap(const world::ObjectTemplate& objectTemplate) const;
	void buildStaticCollision();
	// `t` at `rotation` / `position` and its tree; `parts` / `partMaterials`
	// the mesh whose collisionPart children this node's walk (0x0063ccc0)
	// gives an instance (null: none).
	void addStaticCollision(const world::ObjectTemplate& objectTemplate, const physics::Mat3& rotation,
		const physics::Vec3& position, uint32_t owner, int depth, const physics::CollisionMeshFile* parts = nullptr,
		const std::vector<uint32_t>* partMaterials = nullptr);
	// One mesh instance of a static object (`geometry` 0, or a collisionPart's).
	void addStaticMesh(const world::ObjectTemplate& t, const physics::CollisionMeshFile& mesh,
		const std::vector<uint32_t>& materials, size_t geometry, const physics::Mat3& rotation, const physics::Vec3& position,
		uint32_t owner, bool root);
	// A stationary control object with seats: a fixed body (Body::fixed),
	// whose posed parts the rounds meet through raycastBody.
	static bool stationaryWithSeats(const world::ObjectTemplate& t);

	struct HitBone {
		int node;            // skeleton node
		uint32_t material;
		physics::Vec3 offset; // in the bone's frame
		float radius;
		float length;         // along the bone's -y
	};
	struct Player {
		int id = 0;
		bool present = true; // false once removed: the slot is free
		std::string name;
		bool ai = false;
		bool spectator = false; // +0x404
		bool alive = false;
		// The stats' rank (stats +0x20, -1 from 0x005a5fc0 until the scripts
		// set it: stats.py gives 0 on PlayerConnect, rank.py the stats
		// server's "rnk"); setPlayerRank.
		int rank = -1;
		uint64_t created = 0;     // the manager's list order (PlayerInfo::created)
		double connectedAt = -1;  // +0x1c8 (PlayerInfo::connectedAt)
		float respawnIn = 0;
		int kills = 0, deaths = 0, teamKills = 0;
		int killer = -1; // who killed it last (the player's slot 0x338: the death camera's other object)
		float outsideTime = 0; // +0x294: seconds outside the combat area
		// The held weapon's Radio one-shots (the object component's +0x10
		// fail, +0x11 use) and the trigger last tick.
		bool radioFailArmed = true, radioUseArmed = true, triggerWas = false;
		uint32_t firePresses = 0; // +0x250: the fire's presses (0x0059b260)
		SoldierInput input;
		std::vector<HitBone> hitBones;
		int spawnChoice = -1;
		uint32_t spawnVehicle = 0; // spawnChoice kSpawnInVehicle: the vehicle
		int squad = 0;
		bool squadLeader = false;
		std::string kitTemplate, soldierTemplate; // the next spawn's kit (empty: the team's assault kit)
		std::vector<std::string> unlocked; // ItemContainer templates chosen (equipped)
		std::vector<std::string> owned;    // and owned but not chosen (replaceWithoutEquip's test)
		bool allUnlocked = false;
		// The kit's weapons by slot (itemIndex: 1 knife, 2 sidearm, 3
		// primary, then gadgets); the one in hand is `weapon`, the others
		// keep their state here.
		struct Carried {
			std::string name;
			int slot;
			std::unique_ptr<FireArm> weapon;
		};
		std::vector<Carried> carried;
		int inHand = -1; // index into carried
		int previousSlot = -1; // the slot drawn before (0x005883d0's pick: soldier +0x1f4)
		bool selectHeld = false;
		bool abilityActive = false; // the gadget in hand is working this tick
		bool cloaked = false;       // CamouflageAmmoComp held with the trigger down
		float empLevel = 0;         // the soldier's armour's EMP level (empBlast)
		bool manDown = false;       // killed, the body lying (sv.manDownTime)
		// The player's vehicle handle (+0x80) after death: the soldier's
		// object, held weakly (gone with the corpse); see PlayerInfo::held.
		uint32_t lastSoldier = 0;
		float lastLookPitch = 0; // seated: the last tick's aim pitch (PIMouseLookY is its change)
		bool spawnedProne = false;  // the leader's scatter found room only lying (spawn point +0x18a)
		float manDownLeft = 0;
		// The ability battery (soldier +0x338, read and drawn through
		// component 0xc4d7 slots 0x90/0x98): 0..1, -1 without a kit.
		float battery = -1;
		// The hit indicator: its alpha (+0x2a4) and the flag the own state
		// sends (+0x2a8).
		float hitIndicator = 0;
		bool hitIndicatorFlag = false;
		// What he last gave (abilityBar): player +0x168 healed or repaired,
		// +0x169 repaired, +0x16a ammo; the ticks +0x15c / +0x160.
		bool gaveHeal = false, gaveRepair = false, gaveAmmo = false;
		int64_t gaveHealTick = -1, gaveAmmoTick = -1;
		// The unlocks' ability.increasedRepairAbility (player +0x30c
		// +0x48, copied by 0x005706b0 when above 0): scales the
		// abilities' strength when above 0.
		float abilityModifier = 0;
		// The weapon in hand's soldier deviation and gun sway (Weapons.h),
		// for `feelFor` (its template); zoomed as the player's camera says
		// (setZoomed: the world keeps no zoom of its own); the scope
		// stabilizer (an unlock's ability.hasScopeStabilizerAbility).
		SoldierDeviation deviation;
		float aiDeviation = 0; // the deviation component's +0x44 (setAiDeviation)
		uint32_t gunShots = 0;  // the seats' guns' rounds (PlayerInfo::gunShots)
		float sentShake = 0;    // player +0x1d8 (cameraShake), for
		int sentShakeRecords = 0; // +0x1dc more records (takeSentShake)
		bool toggleFireWas = false; // c_PIToggleFireRate last tick
		int seatItemSelect = 0;     // a seat weapon taken by its key, for the item selection (takeSeatItemSelect)
		int squadSizeRefused = 0;   // a key whose weapon the squad is too small for (takeSquadSizeRefused)
		bool reloadWas = false;     // c_PIReload last tick
		uint32_t reloadPresses = 0; // its presses (PlayerInfo::reloadPresses)
		GunSway sway;
		std::string feelFor;
		bool zoomed = false;
		// fire.useDistanceCounter's range mode (SingleFireComp +0x178 /
		// +0x17c): metres, 0 off; and the zoom last tick.
		int burstDistance = 0;
		bool burstZoomWas = false;
		bool scopeStabilizer = false;
		bool zoomUpgrade = false; // hasZoomUpgradeAbility (GameWorld::zoomUpgrade)
		// ability.networkableHudRevealTime / networkableHudShowCloaked (the
		// ability block's +0xc, 3 by default, and +0x12; 0x005706b0 copies
		// an unlock's when not 3 / when set): squadReportAbility.
		int hudRevealTime = 3;
		bool hudShowCloaked = false;
		// The unlocks' Ability lists: damage taken by projectile material
		// (setMaterialDamageMod) and per item named by addExtraAmmoItem the
		// most magazines (an entry's +4) and rounds a magazine (+8) any
		// entry adds (withExtraAmmo).
		std::map<uint32_t, float> materialDamageMods;
		std::map<std::string, std::pair<int, int>> extraAmmo;
		// The unlocks' sprint and beacon modifiers (player +0x30c: +0x4c
		// sprintRechargeModifier, +0x50 sprintUseModifier, +0x54
		// beaconSpawnTimeMod; 0x005706b0 copies each that isn't 1, the last
		// taken winning) and ability.setOverheatMod's list (+0x24: a weapon
		// template's heat factor, read by the batch fire 0x005c3cb0).
		float sprintRechargeModifier = 1, sprintUseModifier = 1, beaconSpawnTimeMod = 1;
		int dogtagCount = 0; // totalDogtagCount (setDogtagCount)
		std::map<std::string, float> overheatMods;
		float lastSwayPitch = 0;
		struct Lock {
			uint32_t target = 0;
			int state = 0;
			int64_t lockStart = 0;    // the tick the target was taken (the lock's clock)
			int64_t losingSince = -1; // locked, another candidate since this tick
			float maxDistance = 0;
			int system = 0;        // target.targetSystem: 1 TSHeatSeeking, 2 TSLaserGuided
			double chosenAt = 0;   // the component's +0x4c: when the target was taken
			bool announce = false; // +0x44: locked, "target_aquired" not said yet
			std::string weapon;    // the weapon looked at (in hand, or the seat's active gun)
			// The last round fired with target.notifyProjectileOnLostTarget
			// (+0x9c; the component's +0x50, by serial; 0 none): set at each
			// shot (0x005e1ee0), freed by the lock's loss (0x005e1b30).
			uint32_t notifyRound = 0;
		} lock;
		double diedAt = 0;          // soldier +0x290: the death's time
		uint32_t object = 0;
		physics::SoldierBody body;
		SoldierControl control;
		std::unique_ptr<FireArm> weapon;
		// Each firearm's rounds in the world, oldest first (the fire comp's
		// list +0x14..+0x1c of weak handles, by serial): kept for
		// fire.maxProjectilesInWorld; it goes with the kit's firearms.
		std::map<std::string, std::deque<uint32_t>> inWorld;
		const ProjectileSettings* projectile = nullptr;
		std::string muzzleEffect; // the weapon's e_muzz_* child
		physics::Vec3 muzzleOffset; // its place on the weapon (the child's position and rotation)
		physics::Mat3 muzzleRotation;
		double muzzleUntil = -1; // its flash follows the weapon (the anchor moved) until then
		int team = 0;
		uint32_t vehicle = 0;     // the vehicle sat in, 0 on foot
		int seat = -1;            // its seat (0 the driver's)
		bool switchHeld = false;
		std::string weaponTemplate;
		int roundsFired = 0;      // since the scripts last cleared them
		int roundsHit = 0;
		bool useHeld = false;
		// Animation: the soldier's system and the weapon's over it.
		std::unique_ptr<anim::AnimationState> bodyAnimation;
		std::unique_ptr<anim::AnimationState> weaponAnimation;
		anim::LocalPose pose; // kept between updates: bones nothing moves keep theirs
		const anim::Skeleton* skeleton = nullptr; // skeleton3P
		std::vector<anim::Matrix> worldPose; // the last pose, for drawing and the weapon's parts
		std::vector<int> weaponNodes;        // skeleton nodes mesh1, mesh2...
		uint32_t weaponObject = 0;           // the held weapon, an object of its own
		uint32_t kitObject = 0;
		// The kit's geometry index (kit +0x1d0, -1 its default +0x1d4): a
		// seat's crewKitIndex (PlayerControlObject +0x47c, -1 none; the
		// gunships' pilot and gunner 12) on entering (0x005cdd60 ->
		// 0x0056cf50), the default again on leaving (0x00587540).
		int kitGeometry = -1;
		// The player's kit time stamp (player +0x230/+0x234, set on a pickup):
		// a kit is picked up by the use key at most every 2 s.
		double kitTimeStamp = -1e9;
		float lastYaw = 0;
		bool fired = false;
		bool reloadingWas = false; // sends the reload message as one starts
		// The jump state the animation's up value comes from (soldier
		// +0x2e8 / +0x2e9, 0x005eef80).
		anim::SoldierJumpAnimation jumpAnimation;
		// Dead: the body as a ragdoll, and the push of the explosion that
		// killed it (the soldier's 0x2b0, kept by 0x005ee4d0).
		std::unique_ptr<anim::RagDoll> ragDoll;
		physics::Vec3 ragDollForce;
		double ragDollForceAt = -1; // when it came: it lasts 0.5 s (0x2bc)
		// The hit's velocity x 2.5 the ragdoll starts with until 16 ticks
		// after it (0x005ee2b0: soldier +0x248, the tick +0x254; read by
		// 0x005f0a00).
		physics::Vec3 hitPush;
		double hitPushUntil = -1;
		// The armour's last hit while it had hit points (SoldierResponse
		// 0x006c1df0 -> armour slots 0xd8 / 0xe0 / 0xe8: the bone +0x44, the
		// round's direction +0x48, the projectile's hitImpactModifier +0x290
		// until the tick after, +0x294; slot 0xec 0x00652120 gives 1 later).
		int armourHitNode = -1;
		physics::Vec3 armourHitDirection;
		float armourHitModifier = 1;
		uint64_t armourHitTick = 0;
		// The other material of the soldier's last collision event (its
		// handler 0x00586a90 keeps it at +0x32c, -1 at first): the ground's
		// or an object's face. The footstep set (0x007fda00 -> 0x007fd840)
		// is the one for it while the soldier stands or swims, 0x19c8 on a
		// ladder, and stays as it was in the air.
		uint32_t contactMaterial = 0xffffffffu;
		uint32_t footstepMaterial = 0xffffffffu;
		// The aim's recoil kick (SoldierBasedRecoilComp: anim::RecoilSettings
		// of the weapon in hand; 0x005dd5d0 through 0x005861a0 / 0x00586140):
		// the graph, the forces drawn at the last shot (recoilForceUp,
		// recoilForceLeftRight; 0x005dd3f0) and its tick; the pitch kicked
		// and not yet taken by the viewer (takeAimKick), up positive.
		anim::RecoilSettings recoil;
		std::vector<float> recoilGraph;
		RandomDistribution recoilUp, recoilLeftRight;
		float recoilForceUp = 0, recoilForceLeftRight = 0;
		int64_t lastShotTick = -1000;
		float aimKickPitch = 0;
		// Seated: the seat's animation system (seatAnimationSystem).
		std::unique_ptr<anim::AnimationState> seatAnimation;
		std::string seatAnimationPath;
		// The messages the seat's system gets (its MessageTriggers): a
		// parachute's state (0x005cfbb0 -> the PCO's +0x188 slot 0x10: 1
		// high, 2 near the ground, 4 landed; 3p_parachute_animSys's still,
		// landing and landed). 0 in other seats.
		uint32_t seatMessages = 0;
		// On a ladder (its LadderContainer, 0x0059dec0): the ladder (an index
		// into m_ladders, -1 for none), the container's height.
		// Seated, the soldier's own look (0x0058a6d0): the body's turn
		// from the seat (+0x25c, within the seat's limits) and the head's
		// twist on it (+0x278, within 40 degrees).
		float seatBodyYaw = 0, seatTwist = 0;
		int ladder = -1;
		float ladderY = 0;
		// The ladder's camera (LadderContainerCamera): its yaw off the
		// ladder's, by the mouse, within -130..140 degrees.
		float ladderLook = 0;
		float ladderSpeed = 0; // the container's climb this tick (m/s, up positive)
	};
	// Ladders (template class Ladder, 0x0059fb00): placed with their
	// buildings; their soldier LOD's box (mesh slot 0x54(2)) gives the
	// length and the ends (0x0059d550).
	struct Ladder {
		uint32_t owner = 0;
		physics::Mat3 rotation;
		physics::Vec3 position;
		physics::Vec3 boxMin, boxMax; // object space
		float top = 0, bottom = 0;
		bool measured = false;
	};
	std::vector<Ladder> m_ladders;
	void measureLadder(Ladder& ladder) const;
	// The networked place on a ladder (0x0059ebf0 on the server, 0x0059ec80
	// on a client): (height - bottom) / length, length the box's height
	// (+0x18c), within 0..1; and back.
	static float ladderFraction(const Ladder& ladder, float y);
	static float ladderHeight(const Ladder& ladder, float fraction);
	// The climb's pose input at a height (0x0059f380): the message and
	// the time override.
	static void ladderPoseInput(const Ladder& ladder, float y, float upSpeed, anim::AnimationInput& in, float& phase);
	std::string ladderAnimationPath() const;
	bool tryLadder(Player& player, SoldierInput& input);
	void climbLadder(Player& player, SoldierInput& input, float seconds);
	bool leaveLadder(Player& player, int mode);
	void poseOnLadder(Player& player, world::Object& soldier, float seconds);
	void stepSeatedLook(Player& player, const SeatLayout& seat, const SoldierInput& input);
	physics::Vec3 ladderSeat() const;
	int ladderRays(const Ladder& ladder, const physics::Vec3& p, const physics::Vec3& back, bool container, bool up,
		bool down, bool skid) const;
	anim::AnimationSystems* m_animationSystems = nullptr;
	AnimationLoader m_loadAnimationSystem;
	void stepAnimation(Player& player, float seconds);
	// `thrown`: a body thrown out of its seat, pushed along its forward by
	// rd_dieInVehicleForce (0x005f0f50 with +0x2eb) instead of a blast's push.
	bool startRagDoll(Player& victim, bool thrown = false);
	void stepRagDoll(Player& player, float seconds);
	anim::RagDollWorld ragDollWorld(const anim::Vec& middle, float reach) const;
	void poseSeated(Player& player, world::Object& soldier, const Body& body, const SeatLayout& seat, float seconds);
	void stepControlPoints(float seconds);
	ScriptEventSink m_events;
	std::map<int, int> m_fieldUnlocks;
	float m_tinnitus = 0;
	float m_cameraShake = 0;
	uint32_t m_blastSelf = 0; // the object whose own blast explode() runs (its rays pass it)
	void raise(const std::string& event, const ScriptArgs& args)
	{
		if (m_events) {
			m_events(event, args);
		}
	}
	void followSoldier(const Player& player, const world::Object& soldier);
	// The held weapon's root (the soldier's skeleton's mesh1) in the world;
	// false without a posed skeleton.
	bool weaponPlace(const Player& player, physics::Vec3& at, physics::Mat3& rotation) const;
	void weaponTraits(Player& player, const world::ObjectTemplate& weapon);
	void stepSeatCamera(Player& player, Body& body, float seconds);
	// Where a soldier looks (degrees): its body's yaw plus the head's
	// twist (SoldierControl, 0x0058a6d0).
	float lookYaw(const Player& player) const;
	void enterOrLeaveVehicle(Player& player, world::Object& soldier);
	// The entry search's team test (0x005a98e0, for the use key 0x004bb570
	// and the spawn points' enterOnSpawn 0x005b24f0 alike): a vehicle may be
	// entered by a soldier of `team` when its team (the PlayerControlObject
	// root's slot 0x9c: the first seated player's, else its own) is that
	// team or none, or when it isn't a PlayerControlObject (template class
	// 0xc4c2); otherwise only when nobody sits in it (0x005c8b40: a Titan
	// always counts as manned) and its template isn't lockToTeam (+0x350;
	// the heavy IFV eu_ifv: an enemy's empty one stays shut).
	bool mayEnter(const Body& body, int team) const;
	std::map<std::string, std::unique_ptr<ProjectileSettings>> m_projectileSettings;
	std::vector<Projectile> m_projectiles;
	std::vector<Impact> m_impacts;
	size_t m_impactsTotal = 0;
	std::map<uint32_t, float> m_hitPoints;
	effects::EffectManager m_effects;
	void stepBurstRange(Player& player, const world::ObjectTemplate* inHand);
	void fire(const Player& player, float aimPitch);
	bool batchDeviation(DeviationBuildUp& build, bool sub, float mainDeviation, float& alongUp, float& alongRight);
	// target.disableFireWhenActiveMissile: `firearm`'s guided round (of
	// `player`, from `shooter`) is still flying (0x005e2430).
	bool activeMissile(const Player& player, uint32_t shooter, const std::string& firearm,
		const ProjectileSettings* projectile) const;
	// fire.maxProjectilesInWorld (FireComp template +0x74, 0 no limit):
	// before `round` joins the world, the firearm's oldest rounds go until
	// fewer than the limit are left (0x005f2bb0).
	void limitInWorld(Player& player, const world::ObjectTemplate& firearm, Projectile& round);
	// `exclude`: the object whose own blast it is (its armour, 0x005a9090's
	// param_6: neither hurt nor pushed by it).
	// `hitObject`: what the round hit (0x005e9300's param_2; the camera
	// shake's own-object test).
	void explode(const physics::Vec3& at, const ProjectileSettings& settings, int attacker, float facing = 0,
		uint32_t exclude = 0, uint32_t hitObject = 0, const physics::Vec3* up = nullptr);
	// The object a segment meets first (static objects' projectile LOD,
	// bodies, the terrain as 0); false when nothing is in the way.
	bool firstHit(const physics::Vec3& from, const physics::Vec3& to, uint32_t& object) const;
	// firstHit passing through `ignore`'s body.
	bool firstHit(const physics::Vec3& from, const physics::Vec3& to, uint32_t& object, uint32_t ignore) const;
	// `pending`: the damage (after the material cell) this blast has dealt
	// objects so far (0x005a8900's list at 0x00a6aadc / 0x00a6abcc).
	float blastShare(const physics::Vec3& at, const Player& target, float beside,
		const std::vector<std::pair<uint32_t, float>>& pending = {}) const;
	void empBlast(const physics::Vec3& at, const ProjectileSettings& settings,
		const std::function<float(const physics::Vec3&)>& distanceTo);
	// Gadgets' abilities (ammo.replenishingType): a source treats what is
	// within its radius each tick, by strength times the cell of its
	// ability material and the target's armour material, per second.
	// Hubs are thrown out and stay (one per owner); the repair tool works
	// round the soldier while firing.
	struct Ability {
		bool ammo = false;     // RTAmmo; RTHeal otherwise
		float radius = 0;
		float strength = 0;
		uint32_t material = 0;
		float drain = 0;       // abilityDrain: of the holder's battery a second per target treated
	};
	static bool abilityOf(const world::ObjectTemplate& gadget, Ability& ability, bool& whileFiring);
	struct Supply {
		uint32_t object;
		int owner;
		int team;
		Ability ability;
		std::string gadget; // fire.maxProjectilesInWorld 1: one per owner and gadget
		bool crate = false; // works as a SupplyObject (m_crates), not by the gadget's ability
	};
	// The in-hand ability's targets (0x005d8ac0): rescanned every second
	// with none, every two with some, and treated in between.
	struct AbilityScan {
		float timer = 0;
		std::vector<uint32_t> targets;
	};
	std::map<int, AbilityScan> m_abilityScans;
	std::vector<Supply> m_supplies;
	// Ammo owed to soldiers' weapons (fractions of a magazine), by soldier
	// and kit slot (each firearm's ammo keeps its own, 0x005f5110's +0x30).
	std::map<std::pair<uint32_t, int>, float> m_resupply;
	// `holder`: the player whose battery pays and whose unlocks scale it
	// (the gadget's holder, or the hub's owner); none: no battery.
	static constexpr uint32_t kSoldierFootStepsMaterial = 5500; // 0x157c
	void playRagDollImpact(anim::RagDoll& ragDoll, const anim::Vec& at);
	void startEffectsOn(const Player& player, int effect);
	// The same for a weapon template at a muzzle point and its turn (a
	// joined game's remote soldiers).
	void startWeaponEffects(const std::string& weapon, const physics::Vec3& origin, const physics::Mat3& turn, int effect);
	void playAnimationSound(const Player& player, int sound);
	void applyAbility(const Ability& ability, const physics::Vec3& at, int team, float seconds, uint32_t skip,
		AbilityScan* scan = nullptr, Player* holder = nullptr, bool soldiersOnly = false, bool vehiclesOnly = false,
		Player* giver = nullptr, // whose gifts are stamped (abilityBar): the holder by default
		const physics::Vec3* rayEnd = nullptr, // onlyActiveWhileFiring: the ray at -> rayEnd instead of the sphere
		std::vector<uint32_t>* treated = nullptr); // the targets that took something
	void stepAbilities(float seconds);
	// A soldier's ammunition for the ability bar (0x00588ba0): over its
	// weapons that can be refilled (ammo.allowReplenish, nrOfMags not
	// -1) the magazines (one less with the gun's empty) over nrOfMags,
	// each averaged into the running value, times 15, floored, 0..15.
	int soldierAmmoLevel(const Player& player) const;
	// A seated soldier's kit ability (the player's update 0x00599f30, each
	// tick from 0x0059bd80 while the soldier sits in a vehicle): the kit's
	// Ability component (ability.hasHealingAbility / hasRepairingAbility /
	// hasAmmoAbility, the kit object's +0x74 slot 0x38) works round the
	// vehicle within abilityInVehicleRadius (Kit +0x328) at
	// abilityInVehicleStrength (+0x324) of material abilityInVehicleMaterial
	// (+0x320): healing the team's soldiers and repairing its vehicles
	// (game logic slot 0x108) or rearming its soldiers (slot 0x10c); the
	// passengers of his own vehicle only, unless his increasedRepairAbility
	// is above 0 (then everyone in the sphere).
	void stepVehicleAbility(Player& player, float seconds);
	void stepProjectiles(float seconds);
	// StickyCollisionComp's response (0x005e83d0) to a bouncing round's
	// contact: whether it sticks (and if so it is stuck: placed, its
	// vehicle taken), else it bounces as usual.
	bool stickProjectile(Projectile& projectile, const physics::CollisionWorld::Hit& hit, uint32_t hitObject,
		const Body* hitBody, bool soldier);
	// A stuck round rides its vehicle (its place and normal in the
	// vehicle's frame); false when the vehicle has gone.
	bool rideStuck(Projectile& projectile);
	void steerSeeker(Projectile& projectile, float seconds);
	void wireSettings(Projectile& projectile, const world::ObjectTemplate& firearm) const;
	void steerClosest(Projectile& projectile, float seconds);
	// The shot of a lock-on weapon (0x005e1ee0); with the weapon's
	// target.notifyProjectileOnLostTarget the round is kept (its serial,
	// the component's +0x50) for the lock's loss to free (stepWeaponLock).
	void launchLocked(Projectile& projectile, Player& player);
	// Where a round's seek target lies (the seek's target object's
	// position, 0x60: a vehicle's MagneticObject for a magnet, the bait
	// round); false without one.
	bool seekTargetPoint(const Projectile& projectile, physics::Vec3& at) const;
	// A round's hovering ForceObject (ProjectileSettings::Hover, 0x0062c920
	// -> 0x0062b750): its vertical regulation over what lies under it.
	void hoverProjectile(Projectile& projectile, float seconds);
	void damage(uint32_t object, float amount, uint32_t projectileMaterial, uint32_t surfaceMaterial,
		int attacker = -1, bool splash = false); // the attacking player; an explosion's splash
	std::vector<std::unique_ptr<Player>> m_players; // by id
	Player* m_player = nullptr; // the local player
	bool m_addingLocal = false; // spawnPlayer is adding it (addPlayer sets m_player before PlayerConnect)
	SoldierInput m_playerInput;
	float m_spawnTime = 15;
	Player* findPlayer(int id) const;
	bool carriesHealing(const Player& player) const; // TCLocalTeamMedic's receivers (Radio.cpp)
	Player* soldierPlayer(uint32_t object) const;
	world::Object* spawnSoldier(Player& player);
	void kill(Player& victim, Player* attacker, uint32_t weapon);
	// The hands' IK targets of a seated soldier (Animation/Ik.h): the
	// seat's IK object (an Ik component in the seat's tree, 0x005ee910),
	// its ik.* grips in the skeleton's space (0x005efc00).
	// `remote`: a joined client's seated soldier, its vehicle's parts as
	// their ghosts turn them (setRemotePartPose).
	bool seatIkTargets(uint32_t vehicle, const anim::Skeleton* skeleton, const SeatLayout& seat,
		const physics::Mat3& soldierRotation, const physics::Vec3& soldierPosition, std::vector<anim::IkTarget>& targets,
		bool remote = false) const;
	// A part's turn for the seats: a joined client's from the ghosts
	// (m_remotePartPose) first, else the round's own (partPose).
	bool seatPartPose(uint32_t object, const std::string& part, world::Vec3& offset, world::Vec3& rotation, bool remote) const;
	std::function<bool(uint32_t, const std::string&, world::Vec3&, world::Vec3&)> m_remotePartPose;
	// What becomes of the body at PlayerDeath: it stays (a corpse, see
	// stepCorpses), it is slumped in its seat (m_seatCorpses), or it goes
	// at once (the occupants of a destroyed vehicle, 0x005cacf0).
	enum class BodyFate { Corpse, Seat, Remove };
	void finishDeath(Player& victim, BodyFate fate = BodyFate::Corpse);
	void revive(Player& victim, Player* medic, float restoreHitPoints);
	Player* downedPlayer(uint32_t object) const;
	void shockDowned(Player& victim, const ProjectileSettings& shock, float amount, uint32_t surfaceMaterial, int attacker);
	float m_manDownTime = 15;
	std::map<std::pair<int, int>, uint32_t> m_beacons; // (team, squad) -> beacon object
	// The beacon's pod (the thrower's ThrownSpawnPointComp, class 0x94f3:
	// spawn.spawnInPod +0xe0 and spawn.spawnVehicle +0xc4, handed by the
	// throw 0x005e5fd0 to the squad's spawn point, slots 0x54 / 0x5c):
	// the vehicle's template, empty without spawnInPod.
	std::map<std::pair<int, int>, std::string> m_beaconPods;
	std::vector<int> squadMembers(int team, int squad) const;
	// `pod`: the vehicle a beacon spawn drops in (empty for none).
	bool spawnAtSquad(Player& player, world::Object*& at, physics::Vec3& position, float& yaw, std::string& pod);
	bool scatterOnLeader(const Player& leader, physics::Vec3& feet, float& yaw, bool* prone = nullptr, bool ai = false);
	// `pod`: the spawn point's spawnInPod (+0x2b): the tries 150 m up
	// (0x005b2200 / 0x005b1e70; see spawnAtSquad).
	bool scatterFrom(const physics::Vec3& base, float heading, const physics::Vec3& faceTo, uint32_t ignore,
		physics::Vec3& feet, float& yaw, bool* prone, bool ai = false, bool pod = false);
	std::function<bool(const physics::Vec3&)> m_aiSpawnTest;
	struct ProjectileView {
		uint32_t serial = 0;    // the round (Projectile::serial)
		physics::Vec3 offset;   // the camera's place on the round
		bool takeOverInput = false;
		bool snapToZero = false;
		physics::Vec3 minRotation, maxRotation, maxSpeed; // degrees, degrees a second
		float yaw = 0, pitch = 0; // the camera's turn on the round (pitch up positive)
		float lastAimPitch = NAN;
		float fov = 1.1f;
	};
	std::map<int, ProjectileView> m_projectileViews; // by player
	void startProjectileView(const Player& player, const Projectile& round, const world::ObjectTemplate& firearm);
	// Steps the view (its round gone: ended); true while it takes the input.
	bool stepProjectileView(Player& player, float seconds);
	bool teamHoldsControlPoint(int team) const;
	bool spawnPointUsable(const world::Object& point, const Player& player, bool spotClear) const;
	bool enterOnSpawn(Player& player, world::Object& soldier, const physics::Vec3& from, float radius, uint32_t only = 0);
	bool soldierFits(const physics::Vec3& origin, SoldierPose pose, uint32_t ignore) const;
	float m_waterLevel = 0;
	bool m_hasWater = false;
	void stepPlayers(float seconds);
	void stepSoldier(Player& player, float seconds);
	void stepWeaponFeel(Player& player, SoldierInput& turned, float seconds);
	void stepWeaponLock(Player& player, float seconds);
	int targetTypeOf(const world::ObjectTemplate& t, int depth = 0) const;
	// The TargetObject template that gives a tree its targetType (the
	// root's own or a child's), nullptr if none.
	const world::ObjectTemplate* targetObjectOf(const world::ObjectTemplate& t, int depth = 0) const;
	// Whether a target root's TargetObject is enabled (0x0058eba0):
	// disableWhenWreck and disableWhenEmptyVehicle(Delay).
	bool targetEnabled(const world::Object& root);
	mutable std::map<const world::ObjectTemplate*, int> m_targetTypes;
	FireArmSettings withExtraAmmo(const Player& player, const world::ObjectTemplate& item) const;
	// Takes the carried weapon of a slot in hand.
	void equip(Player& player, int slot);
	const ProjectileSettings* projectileFor(const std::string& projectileTemplate);
	world::ObjectTemplate* beaconTemplate(const world::ObjectTemplate* weapon) const;
	world::ObjectTemplate* deployableTemplate(const world::ObjectTemplate* weapon) const;
	std::map<std::pair<int, std::string>, uint32_t> m_deployed; // (owner, gadget) -> object
	// keepProjectiles (0x005f1cc0): what a gone player's firearms left, and
	// when it goes.
	std::vector<std::pair<double, uint32_t>> m_keptObjects;
	void keepProjectilesOf(int player);
	void stepKeptObjects();
	bool removeDeployed(uint32_t object);
	// Drones: the ForceObject's template settings (0x0062d4f0; offsets in
	// the template) that hold the drone by its target, the owner.
	struct ForceSettings {
		// regulatePosX/Y/Z (0x270, 0x278, 0x280): each axis's regulator,
		// as the real and imaginary parts of its poles.
		float regulatePos[3][2] = {};
		float positionToleranceXZ = 0;    // 0x2a0
		float warningDistance = 0;        // 0x2a4
		float destroyDistance = 0;        // 0x2a8
		float minHeightAdjustDistance = 0; // 0x2ac
		bool destroyWhenTargetLost = false; // 0x295
	};
public:
	// An auto controller's values (autoControllerTemplate.*, AIDLL's
	// template 0x100a8ed0) for the guns the rewrite runs in GameWorld.
	struct AutoControl {
		float viewDistance = 30;      // +0x10
		float discoverCloaked = 0;    // discoverCloakedEnemiesDistance, +0x2c
		float fov = 20;               // +0xc, degrees
		float scanningSpeed = 0.03f;  // +4, radians per 1/30 s
		float targetLostTimeout = 0;  // +0x1c
		float reactionTime = 0;       // +0x18
		float yawSpeed = 0;           // the yaw bundle's maxSpeed x maxTrackingInput (degrees/s)
		bool onlyAttackNetworkBattlefieldObjects = false; // +0x39
		bool neverAttackDeployer = false;                 // +0x20
		bool checkLineOfFire = false;                     // +0x28
		float flipOutAtHealth = 0;                        // +0x30
		bool useSiren = false;                            // +0x14
		bool reportEnemiesToTeam = false;                 // +0x21
	};

private:
	struct Drone {
		uint32_t object = 0;
		int owner = -1;
		int team = 0;
		const AutoControl* control = nullptr;
		// A deployed sentry gun (eu_sentrygun): it stands where it was set
		// down (no ForceObject) and its gun sits gunHeight over its origin.
		bool stationary = false;
		float gunHeight = 0;
		// The sentry's auto controller: the gun's heading (radians), its
		// target and when it was last seen; the trigger waits until
		// fireFrom (the reaction time from the target's taking).
		float gunYaw = 0;
		int target = -1;
		float targetSeenAt = 0;
		float fireFrom = 0;
		// The fire plan's statements for an auto controller (AIDLL
		// 0x10077xxx's tail, made with the plan as the target is taken):
		// useSiren's (0x100d8090 / 0x100d80f0) holds PIAltFire for the
		// plan's first second, from planFrom; reportEnemiesToTeam's
		// (0x100d7fa0 / 0x100d7ff0) reports the target every 5 s, the
		// last at reportedAt (+0x20, 0 for none yet). altFireHeld: the
		// siren's input as last given (a SingleFireComp sounds on a press).
		float planFrom = 0;
		double reportedAt = 0;
		bool altFireHeld = false;
		// The gun's DefaultAnimationComp (0x005e12e0; template 0x005e1260,
		// animation.loopingFire +0x1c) gives the deployable's animation
		// system message 1: held from a shot to the trigger's release with
		// loopingFire, else a pulse a shot.
		bool animated = false, loopingFire = false;
		bool fireHeld = false, firePulse = false;
		ForceSettings force;
		float hoverHeight = 0; // the target's physics origin over its feet (see stepDrones)
		physics::Vec3 position, velocity;
		float yaw = 0;
		float height = 0;      // the height it holds (0x1a8), kept while the owner is near
		bool hasHeight = false;
		bool warning = false;  // past warningDistance (0x1e9)
		// Its weapon: a recon drone's pulse (PulseMeterAmmoComp) or a
		// sentry drone's gun, fired by its auto controller.
		std::unique_ptr<FireArm> weapon;
		// Its auto controller's trigger (an AIDLL bot's fire statement,
		// 0x100777a0 / BAPATrigger 0x100d3110): held for an automatic gun
		// (fire mode 2), else a tap, pressed on one execution and let go on
		// the next (autoTrigger).
		bool tapped = false;
		const ProjectileSettings* projectile = nullptr;
		bool pulse = false;
		float pulseDistance = 0; // pulse.viewDistance
		float revealTime = 0;    // pulse.revealTime
		float empLevel = 0;      // its armour's EMP level (see empBlast)
		DeviationBuildUp gunDeviation; // the gun's firing build-up (setFireDev)
	};
	std::vector<Drone> m_drones;
	// The deployed sentry guns (a deployable with the SentryGun auto
	// controller), by their deployed object.
	std::vector<Drone> m_sentryGuns;
	// The deployables' own animation systems (spawn_beacon, the hubs, the
	// portable shield, the sentry gun): objects that aren't bodies here but
	// get the skeleton helper as every object does (the object base's
	// constructor 0x0063d530 builds it, 0x0063d340, for a template with a
	// bundleSkeleton, +0x274; its update 0x0063d5b0 -> 0x0063d470 poses the
	// boneIndex parts). Kept by object; `tried` with no system.
	struct ObjectAnimation {
		uint32_t object = 0;
		std::unique_ptr<anim::AnimationState> animation;
		const anim::Skeleton* skeleton = nullptr;
		anim::LocalPose pose;
		std::vector<Body::BonePart> boneParts;
		bool posed = false;
		// A joined client's vehicle ghost (animateRemoteVehicle): kept while
		// its object is; its last yaw (degrees) for the turn rate.
		bool remote = false;
		float lastYaw = 0;
	};
	std::vector<ObjectAnimation> m_objectAnimations;
	void posedAfterUpdate(ObjectAnimation& a);
	void stepDeployableAnimations(float seconds);
	// A template's bundleAnimationSystem and bundleSkeleton (0x0063d340) and
	// its boneIndex parts (0x0063d5e0); false without a system.
	bool loadBundleAnimation(const world::ObjectTemplate& t, std::unique_ptr<anim::AnimationState>& animation,
		const anim::Skeleton*& skeleton, anim::LocalPose& pose, std::vector<Body::BonePart>& parts);
	// The bone parts' matrices from the skeleton's pose (0x008b8880 then
	// 0x0063d470's hand-out).
	static void poseBoneParts(anim::AnimationState& animation, const anim::Skeleton& skeleton, anim::LocalPose& pose,
		std::vector<Body::BonePart>& parts);
	// Soldiers killed in a seat that has a die bundle: the body stays,
	// slumped by the seat's animation system (0x005cea00, 0x005ed890),
	// counted down by its armour as any body (0x00652380).
	struct SeatCorpse {
		uint32_t object = 0;
		uint32_t vehicle = 0;
		int seat = -1;
		double diedAt = 0;        // the armour's wreck time (+0x60)
		float stayAsWreck = 20;   // armor.timeToStayAsWreck (+0x158)
		float afterDestroyed = 0; // armor.timeToStayAfterDestroyed (+0x15c)
		double fadeFrom = -1;     // when the countdown began (+0x64)
		std::unique_ptr<anim::AnimationState> animation;
		const anim::Skeleton* skeleton = nullptr;
		anim::LocalPose pose;
		std::vector<anim::Matrix> worldPose;
	};
	std::vector<SeatCorpse> m_seatCorpses;
	// Kits on the ground (Kits.cpp): a dead soldier's kit dropped where a
	// ray straight down from it meets something (0x00589890), with its
	// weapons and their rounds and its battery, gone timeToLiveAfterDeath
	// (kit template +0x2b8, 30 s by default, 0x00570080) later, carried by
	// a Titan it lies on (0x00561b20 / 0x005ad2e0 / 0x005aa5b0).
	struct DroppedKit {
		uint32_t object = 0;
		int team = 0;
		std::vector<Player::Carried> carried;
		int inHand = -1;
		float battery = -1;
		float abilityModifier = 0;
		double removeAt = 0;
		uint32_t titan = 0;   // the Titan it lies on (its object), 0 for none
		physics::Vec3 offset; // kit position - Titan position (no rotation: Titans never turn)
	};
	std::vector<DroppedKit> m_droppedKits;
	bool m_reportedBlowUp = false;
	// Stepping a LoneVehicle: no collision events or contact effects, no
	// other bodies met.
	bool m_predicting = false;
	void dropKit(Player& player);
	void fillKit(Player& player, world::ObjectTemplate* kitTemplate, world::ObjectTemplate* primary);
	bool pickUpClosestKit(Player& player, float radius);
	// The use key's pick-up of a placed item (0x004bce30): see Kits.cpp.
	bool pickUpPlaced(Player& player);
	void stepDroppedKits();
	std::vector<SoundEvent> m_sounds;
	std::vector<WeaponSoundEvent> m_weaponSounds;
	void weaponSound(WeaponSoundEvent::Kind kind, const Player& player, uint32_t object, const std::string& weapon,
		const physics::Vec3& position);
	void stepSeatCorpses(float seconds);
	// Bodies after PlayerDeath, with their ragdolls (the soldier's armour
	// stays a wreck until its countdown, 0x00652380, removes the object).
	struct Corpse {
		uint32_t object = 0;
		std::unique_ptr<anim::RagDoll> ragDoll;
		const anim::Skeleton* skeleton = nullptr;
		anim::LocalPose pose;
		std::vector<anim::Matrix> worldPose;
		double diedAt = 0;          // the armour's wreck time (+0x60)
		float landedDelay = 4;      // ragdollLandedDelay (+0x3bc)
		float stayAsWreck = 20;     // armor.timeToStayAsWreck (+0x158)
		float afterDestroyed = 0;   // armor.timeToStayAfterDestroyed (+0x15c)
		double fadeFrom = -1;       // +0x64: when the countdown began
	};
	std::vector<Corpse> m_corpses;
	void stepCorpses(float seconds);
	// Remote soldiers' bodies (driveRemoteRagDoll); no armour countdown:
	// the server removes the ghost.
	struct RemoteBody {
		uint32_t object = 0;
		std::unique_ptr<anim::RagDoll> ragDoll;
		const anim::Skeleton* skeleton = nullptr;
		anim::LocalPose pose;
		std::vector<anim::Matrix> worldPose;
	};
	std::vector<RemoteBody> m_remoteBodies;
	struct RemoteSoldier {
		const anim::Skeleton* skeleton = nullptr;
		std::unique_ptr<anim::AnimationState> body, weapon;
		std::string weaponName;
		uint32_t weaponObject = 0;
		std::vector<int> weaponNodes;
		anim::LocalPose pose;
		std::vector<anim::Matrix> worldPose;
		float lastYaw = 0;
		bool started = false;
		bool reloadingWas = false; // sends the reload message as one starts
		// The weapon run from the fire state: its settings, rounds in the
		// magazine, the time to the next round, the reload left, the last
		// press count, the burst's rounds still to go.
		FireArmSettings fireArm;
		int rounds = -1;
		float nextRound = 0;
		float reloadLeft = 0;
		int presses = -1;
		bool wasFiring = false;
		int burstLeft = 0;
		int tracerCount = 0;       // the fire comp's +0x78 (FireArm::takeTracer)
		std::vector<HitBone> hitBones; // the soldier template's skeletonCollisionBone records
		// Seated or on a ladder: the seat's (the container's) system in
		// place of the body's, and the height it was last at.
		std::unique_ptr<anim::AnimationState> seat;
		std::string seatPath;
		float ladderY = 0;
		bool onLadder = false;
		bool hiddenBySeat = false;
		SoldierPose posture = SoldierPose::Stand; // the last state's (the emotes' prone test)
	};
	std::map<uint32_t, RemoteSoldier> m_remoteSoldiers;
	RemoteSoldier* startRemoteSoldier(uint32_t object);
	bool remoteSeatSystem(RemoteSoldier& remote, const std::string& path);
	void leaveRemoteSeat(RemoteSoldier& remote, world::Object& soldier);
	// A remote soldier's round: the projectile its firearm makes on a
	// client (one without a networkable), from the muzzle along the aim.
	void fireRemoteRound(RemoteSoldier& remote, uint32_t object, const physics::Vec3& muzzle, const physics::Mat3& turn,
		const physics::Vec3& velocity);
	// A visual projectile's detonation: its end effect and the ambient
	// triggers' event, nothing hurt.
	void explodeEffects(const physics::Vec3& at, const ProjectileSettings& settings, const physics::Vec3* up = nullptr);
	// A hit round's detonation (0x005e9300): the depth test
	// (maxDepthForExplosion) and the end effect's up (useCollisionNormal);
	// false when it doesn't go off.
	bool detonateHit(Projectile& p, const physics::Vec3& point, const physics::Vec3& normal, uint32_t hitObject);
	// DefaultPenetrateComp (0x005eaca0): whether the round goes on through
	// what it hit; if so it is moved past it with what is left of its damage.
	bool penetrate(Projectile& p, const physics::CollisionWorld::Hit& hit, uint32_t hitObject, const Body* hitBody,
		bool soldier);
	uint32_t m_projectileSerial = 0;
	std::vector<BattlefieldReport> m_battlefieldReports;
	float m_radarSweep = 0; // radarSweep()
	void stepRemoteBodies(float seconds);
	struct RemoteWreck {
		double wreckedAt = 0;  // armour +0x60
		double fadeFrom = -1;  // +0x64: when the countdown began
		bool attached = false; // a player still on it (the PCO's slot 0x10)
		bool soldier = false;  // class 0x9493: lands, waits and fades
		float landedDelay = 4, stayAsWreck = 20, afterDestroyed = 0;
	};
	std::map<uint32_t, RemoteWreck> m_remoteWrecks;
	void stepRemoteWrecks();
	std::map<uint32_t, double> m_remoteBodyStarted; // when each remote body began (its death on the client)
	bool m_hasCamera = false;
	physics::Vec3 m_cameraPosition, m_cameraForward {0, 0, 1};
	void setRagDollLevels();
	float ragDollDistance(const anim::RagDoll& body) const;
	std::function<double()> m_clock;
	double m_ragDollFrameStart = 0; // the frame's ragdoll updates began (0x008ae120's counter)
	std::map<std::pair<int, int>, float> m_revealed; // (player, team) -> seconds left
	// The squads' battlefield objects (AIDLL 0x100bf000, 0x100bea30): per
	// (team, squad), when each enemy player was last reported.
	std::map<std::pair<int, int>, std::map<int, double>> m_squadKnowledge;
	world::ObjectTemplate* droneTemplate(const world::ObjectTemplate* weapon) const;
	void launchDrone(Player& player, const world::ObjectTemplate& detonator, world::ObjectTemplate& drone);
	void stepDrones(float seconds);
	void pulse(const Drone& drone);
	static bool autoTrigger(Drone& drone, bool want);
	void fireSentry(Drone& drone, float seconds);
	static void fireMessage(Drone& drone, bool trigger, bool fired);
	uint32_t gunMessages(uint32_t object, uint32_t& pulses) const;
	void addSentryGun(const Player& player, const world::Object& placed);
	const world::ObjectTemplate* autoControlledGun(const world::ObjectTemplate& t, float* height) const;
	void stepSentryGuns(float seconds);
	// An auto controller's report of its target (IAIGame slot 0x68,
	// 0x00895ab0, from its player) and the siren's press.
	void autoControllerReport(Drone& drone, const Player& target);
	void autoControllerSiren(const Drone& drone);
	bool removeDrone(uint32_t object);
	Drone* findDrone(uint32_t object);
	bool isDrone(uint32_t object) const;

	// The commander (Commander.cpp). Per team (index 1, 2): the SquadManager's
	// commander (0x004c8120) and applicants (0x004c4fb0), the assets' tools
	// (DAT_00a69cb8: 0x005a1440 gathers them, 0x005a1270 fires), the UAV and
	// scan timers (DAT_00a6a390: 0x005a23b0, 0x005a2450) and the supply drop
	// (0x0043d6d0).
	double m_time = 0; // seconds since the round started
	uint64_t m_playersMade = 0; // addPlayer's count (Player::created)
	uint32_t m_ticks = 0; // the engine's tick count (0x00666090: DAT_00a7d1f8)
	// The launchers of the objects thrown or set down (the projectile
	// interface's +0x1c8, 0x005b48b0), by object; read when its body is made.
	std::map<uint32_t, uint32_t> m_launchers;
	// The box bottoms of templates with MechLegs (their +0x4c4, 0x005cd4f0),
	// once one of them has a body.
	std::map<const world::ObjectTemplate*, float> m_legsBottom;
	bool m_commanderEnabled = false;
	struct CommanderTeam {
		int commander = -1;
		std::vector<int> applicants;
		int candidate = -1;   // offered command, awaiting the answer (+8 of 0x004c4fb0's list)
		double pickAt = -1;   // when the best applicant is offered command (event 7), -1: not due
		double lastUse[kCommanderAbilities] = {-1e9, -1e9, -1e9, -1e9, -1e9}; // when each tool was used
		float minTime[kCommanderAbilities] = {};  // its recharge then (minTimeBetweenActivation...)
		physics::Vec3 uavAt;
		// The UAV's drone (the UAV manager DAT_00a6a090, slots +4/+8): its
		// object and the circle it flies.
		uint32_t uavDrone = 0;
		physics::Vec3 uavCentre;
		float uavCircle = 0, uavTurnRate = 0, uavAngle = 0;
	};
	void launchUavDrone(int team, const physics::Vec3& at);
	void flyUavDrones(float seconds);
	void removeUavDrone(int team);
	CommanderTeam m_commanders[3];
	std::map<std::pair<int, int>, SquadOrder> m_orders;       // (team, squad): the commander's
	std::map<std::pair<int, int>, SquadOrder> m_leaderOrders; // (team, squad): the leader's
	// An asset: a placed object whose template holds a RemoteControlledObject.
	struct CommanderAsset {
		uint32_t object;
		CommanderAbility ability;
		float minTimeBetweenActivation;
		std::string fireArm; // its GenericFireArm child (the strikes)
	};
	std::vector<CommanderAsset> m_assets;
	std::map<const world::ObjectTemplate*, std::vector<CommanderAsset>> m_assetTemplates; // what each holds
	float m_assetScan = 0; // seconds until the objects are looked through again
	std::vector<Strike> m_strikes;
	struct StrikeFire {
		std::string fireArm;
		int player = -1; // the commander who called it (the kills are its)
		float subUp = 0, subRight = 0; // the last sub-projectile's offset (deviation +0xc, +0x10)
		DeviationBuildUp deviation;    // its firing's build-up (setFireDev)
		uint32_t asset = 0;            // the asset object whose firearm it is
	};
	std::vector<StrikeFire> m_strikeFire; // alongside m_strikes
	std::vector<StrikeFired> m_strikeFires; // takeStrikeFires
	struct Crate {
		uint32_t object = 0;
		int team = 0;
		float storage = 0;   // sharedStorageSize left
		float timeLeft = 0;  // supplyDropNumSecsToLive
		float tick = 0;  // to the next of its 0.2 s updates (event 0x1d)
		std::map<const FireArm*, float> owed; // magazines' fractions refilled
		// The SupplyObject's template when it is a child of the object (a
		// deployed hub's Unl_Hub_*_Supply); null: the object's own.
		const world::ObjectTemplate* settings = nullptr;
		bool carried = false; // on a Titan (its supply point): moved with it, not lowered to the ground
		// teamFromClosestCP (SupplyObject +0x2e0; the helipads' and
		// airstrips' pads): each update its team is the nearest control
		// point's (0x005d0f20: the game logic's points, slot 0x180, by the
		// 3-D distance from 9999 m; the point's team, interface 0xc4d1 slot
		// 0x1c), its +0x188.
		bool teamFromClosestCP = false;
	};
	// The static objects' SupplyObject children (helipad_m_SupplyObject,
	// concreteblock_helipad_SupplyObject_helipad...): each placed one gets
	// a supply object of its own at the child's place, its storage never
	// spent (healStorageSize / ammoStorageSize -1, 0x005d0650 / 0x005d06a0).
	void addStaticSupplies(const std::vector<uint32_t>& objects);
	std::vector<Crate> m_crates;

	// Spawned PlayerControlObjects' time to live (interface 0xc4c5: +0x258
	// the countdown, +0x25c timeToLive, +0x264 the spawner), checked every
	// half second (event 0x1d) or five while in use or watched.
	struct Abandonment {
		uint32_t spawner = 0;
		float timeToLive = 0;
		float left = 0;
		double nextCheck = 0;
	};
	std::map<uint32_t, Abandonment> m_abandonment;
	void stepAbandoned();
	void holdSpawned(ObjectSpawner& spawner);
	// Hit points lost without a material cell (the game logic's damage,
	// 0x004c0e40, as the countdown and the impacts call it).
	void loseHitPoints(uint32_t object, float amount, int attacker);
	const CommanderAsset* intactAsset(int team, CommanderAbility ability) const;
	float commanderRecharge(int team, CommanderAbility ability) const;
	void scanCommanderAssets();
	void stepCommander(float seconds);
	void offerCommand(int team);
	void fireStrike(Strike& strike, StrikeFire& fire);
	void stepCrates(float seconds);
	bool isCommanderAsset(uint32_t object) const;

	// Titan mode (Titans.cpp).
	std::string m_gameMode;
	const CombatAreas* m_combatAreas = nullptr;
	uint32_t m_titanSerial = 0;
	bool titanModeOn() const;
	// A node of the Titan's template tree with armour (or with a collision
	// mesh): where it sits in the Titan's frame, its armour's state
	// (ArmorGLComp: hit points +0x10, the template's settings) and its parent
	// part (-1: the root's own armour, part 0).
	struct TitanPart {
		const world::ObjectTemplate* t = nullptr;
		int parent = -1;   // the nearest armoured ancestor's part
		uint32_t id = 0;   // the root's object id for part 0, else kTitanPartIds + ...
		physics::Mat3 rotation;
		physics::Vec3 position;
		bool armoured = false;
		float hitPoints = 0, maxHitPoints = 10, wreckHitPoints = 10;
		float criticalDamage = 0, hpLostWhileCritical = 0;
		float parentDamageAlive = 0, parentDamageWreck = 0;
		float destroyOnParentLevel = -1, destroyOnRootLevel = -1, onlyFromChildrenLevel = -100;
		float criticalTime = 0; // +0xf0: seconds burnt since the last loss
		int state = 0;          // vtable 0x60 (0x00651c20): 0 alive, 1 wreck, 2 destroyed
		bool off = false;       // its subtree's collision gone (0x00651c60)
		bool addNametagOnDestroy = false; // armor.addNametagOnDestroy (+0x195)
		float healthActivationLevel = 100; // nametag.healthActivationLevel (+0x24)
		int nametagPushes = 0;
		int destroyEvent = 0;   // Python.onDestroyEvent
		int repairEvent = 0;    // Python.onRepairEvent
		// The Python component's sent flags (+0x10 destroy, +0x11 repair):
		// each event goes once until the other has gone (0x005b7680, 0x004bc410).
		bool destroySent = false, repairSent = false;
		TitanPartInfo::Kind kind = TitanPartInfo::Other;
		std::string seat;       // the gun seat (a hub PlayerControlObject) it lies under
		// armor.addArmorEffect (0x00654730's list, by level, highest first):
		// the effects started as the percent falls to a level, in the part's
		// frame; the hit points last seen (+0x254) and the level started (+0x184).
		struct ArmorEffect {
			int level = 0;
			std::string effect;
			physics::Vec3 position;
			physics::Mat3 rotation;
		};
		std::vector<ArmorEffect> effects;
		float effectHitPoints = 0;
		int effectLevel = 0;
	};
	// A Titan mesh's collision LOD as a candidate of its own (Titan::
	// vehicleParts).
	struct TitanLodPart {
		int lod = 1; // physics::CollisionUser
		std::shared_ptr<physics::CollisionWorld> mesh;
		std::vector<physics::Vec3> points;
		std::vector<uint32_t> pointMaterials;
		physics::Vec3 boxMin, boxMax;
		float radius = 0;
		uint32_t groups = 0;
	};
	struct TitanMesh {
		const world::ObjectTemplate* t = nullptr; // the node with the mesh
		const physics::CollisionMeshFile* mesh = nullptr;
		std::vector<uint32_t> materials;
		physics::Mat3 rotation;
		physics::Vec3 position;
		int node = -1;  // the template node's part (its armour's or its armoured ancestor's)
		std::vector<int> chain; // the parts it lies under (its collision goes with any of them)
		int groups = 0;         // collisionGroups: 0x100 / 0x200 a team's own forcefield (PAC's, EU's)
		// Its LODs' parts, made once (rebuildTitanMeshes): they depend on
		// the mesh and its place in the Titan only, not on which parts are
		// down.
		std::vector<std::shared_ptr<const TitanLodPart>> lodParts;
		bool lodPartsMade = false;
	};
	struct Titan {
		uint32_t object = 0;
		bool collisionPartsBuilt = false; // its body's partNodes / partMeshes (buildTitanCollisionParts)
		int team = 0;
		int controlPoint = -1;
		std::vector<TitanPart> parts; // [0]: the root
		std::vector<TitanMesh> meshes;
		// Its parts' collision LODs (in the Titan's frame), each a candidate
		// of its own for the bodies' mesh physics (0x006dc7b0 / 0x006bb050)
		// with its collisionGroups: a team's vehicles pass their own fields.
		// A mover meets each part at the LOD it asks for (otherCollisionLod:
		// the vehicles 1, the parachute, crates, beacons and drones 2, the
		// soldier LOD with the decks), as it meets any other mesh.
		using VehiclePart = TitanLodPart;
		std::vector<std::shared_ptr<const VehiclePart>> vehicleParts; // the live meshes' (TitanMesh::lodParts)
		physics::Vec3 velocity;       // x and z (0x005aa0e0 sets the node's)
		// The move orders (+0x3c: tick -> target), the last order's time.
		std::map<uint32_t, std::pair<float, float>> targets;
		// The sync object's states (+0x48: tick -> the velocity and the
		// position across x and z, 0x005ab670) and, on a client, the
		// server's (+0x54, 0x005ab8a0); whether the client's sync placed it
		// on its last step.
		struct SyncState {
			float velocityX = 0, velocityZ = 0, x = 0, z = 0;
		};
		std::map<uint32_t, SyncState> states, serverStates;
		bool syncPlaced = false;
		double lastOrder = -1e9;
		float orderInterval = 5; // the RCTitan object's minTimeBetweenActivation
		float radius = 0;
		float bottom = 0;        // the box's lowest point below the origin (0x005aa090)
		bool wrecked = false;
		struct Spawn {
			uint32_t object = 0;
			physics::Vec3 offset; // in the Titan's frame
			float yaw = 0;        // degrees, the child's
		};
		std::vector<Spawn> spawnPoints;
		// Its object spawners (as_titan_ATSpawner / AGSpawner: team 1,
		// holdObject) and supply point (as_titan_heliRepairRearm), objects of
		// their own carried with it.
		std::vector<Spawn> carried;
		// Its pod arrays (titan_pod_array1/2: a PlayerControlObject with
		// EntryPoints and a GenericFireArm with fire.isPodLauncher), in its
		// frame.
		struct PodArray {
			std::vector<std::pair<physics::Vec3, float>> entries; // entry points and their radius
			physics::Vec3 launcher;
			physics::Vec3 direction;
			std::string projectile;
			float velocity = 100;
			float maxTilt = 0; // its launcher's fire.maximumVehicleTiltForFire
		};
		std::vector<PodArray> podArrays;
		bool meshesDirty = true;
		uint32_t serial = 0;      // its parts' ids: kTitanPartIds + serial << 12 + part
		std::vector<int> seatParts; // each gun seat's armour (its hub's first armoured node), -1 for none
		// The wreck (0x00652380): when it became one (+0x60), the root's
		// armor.timeToStayAsWreck (+0x158), timeToStayAfterDestroyed (+0x15c)
		// and canBeDestroyed (+0x166); the countdown's start (+0x64).
		double wreckedAt = -1, removeFrom = -1;
		float stayAsWreck = 20, afterDestroyed = 0;
		bool canBeDestroyed = true;
		bool coreExplosion = false; // event 0x14 sent (0x00654730)
	};
	std::vector<Titan> m_titans;
	bool insideTitanHull(const Titan& titan, const physics::Vec3& point) const;
	std::vector<std::unique_ptr<ObjectSpawner>> m_newSpawners; // a Titan's, made during the spawners' loop
	static constexpr uint32_t kTitanPartIds = 0x7f000000u;
	// The armour's damage lists (0x006557a0): +0x220 every hit, +0x22c a
	// vehicle's hits while manned or on a team, each entry {attacker, time
	// (the game's clock, 0x00666070, as a double), amount}, appended; +0x238
	// the soldiers' attackers past the limit; +0x264 the heal since
	// (0x00655a00). The tests sum an attacker's amounts in list order
	// (0x006557a0, 0x004c0e40); nothing found reads the times.
	struct DamageEntry {
		int attacker = -1;
		double time = 0;
		float amount = 0;
	};
	struct ArmourDamage {
		std::vector<DamageEntry> all;
		std::vector<DamageEntry> manned;
		std::set<int> over;
		float healed = 0;
	};
	static float damageSum(const std::vector<DamageEntry>& list, int attacker);
	static void dropDamage(std::vector<DamageEntry>& list, int attacker);
	std::map<uint32_t, ArmourDamage> m_armourDamage;
	struct PendingTeamDamage {
		TeamDamage event;
		std::vector<int> recipients;
	};
	std::vector<PendingTeamDamage> m_teamDamage;
	float m_teamDamageLimits[2] = {50, 25};
	std::set<std::pair<int, int>> m_privateSquads;          // (team, squad): requestJoinSquad
	std::map<int, uint32_t> m_squadInvites;                  // player -> its invite mask
	std::map<int, std::vector<SquadMessage>> m_squadMessages; // player -> its 2/x messages
	// 0x006557a0 for `armour` (the object whose armour took it; `vehicle`
	// the body whose control object it is under, `most` its maximum).
	void recordArmourDamage(uint32_t armour, uint32_t vehicle, float amount, int attacker, float most);
	// 0x004c0e40's tail after the damage: the team damage test.
	void checkTeamDamage(uint32_t armour, uint32_t vehicle, int attacker, bool destroyed, bool wasDestroyed,
		bool pointOnDestroy, float maxHitPoints);
	// 0x00655a00: a heal of `amount`; over 50 since, or full, the list goes.
	void armourHealed(uint32_t armour, float amount, bool full);
	// The cell behind an id (shieldCellId); false for any other id.
	bool shieldCell(uint32_t id, Body*& body, Body::Cell*& cell);
	bool shieldCell(uint32_t id, const Body*& body, const Body::Cell*& cell) const;
	// A hit on a cell (loseHitPoints): its armour, the award, its destroy event.
	bool shieldCellDamage(uint32_t id, float amount, int attacker);
	void buildShieldCells(Body& body);
	// The cells' meshes into a projectile LOD (the root's state: a wreck's
	// sub-geometry where the cell's mesh has one).
	void addShieldCellMeshes(const Body& body, physics::CollisionWorld& world) const;
	std::vector<std::pair<physics::Vec3, float>> m_titanObstacles; // static objects reaching the Titans (0x00562360)
	bool m_titanObstaclesFound = false;
	void registerTitan(uint32_t object, int controlPoint);
	bool titanGunWorks(uint32_t titan, int seat) const;
	void titanSeatsOut(Titan& titan);
	void removeTitanCarried(Titan& titan);
	// `remote`: a joined client's (addRemoteTitan): the tree's parts and
	// meshes only, its spawn points and carried objects being the server's
	// ghosts.
	void buildTitanParts(Titan& titan, bool remote = false);
	void rebuildTitanMeshes(Titan& titan);
	// A joined client's Titans (addRemoteTitan): their collision only.
	std::vector<Titan> m_remoteTitans;
	void stepRemoteTitans();
	// The collisionPart children under each node of the Titan's tree with a
	// mesh (its gun hubs' cannons: 0x0063d720 -> 0x0063ccc0 with that
	// node's mesh), as the body's posed parts (PartCollision.cpp).
	void buildTitanCollisionParts(Titan& titan, Body& body);
	static std::vector<std::shared_ptr<const TitanLodPart>> titanLodParts(const TitanMesh& m, uint32_t owner);
	Titan* titanByObject(uint32_t object);
	const Titan* titanByObject(uint32_t object) const;
	bool titanPart(uint32_t id, Titan*& titan, int& part);
	bool titanPart(uint32_t id, const Titan*& titan, int& part) const;
	float titanPercent(const TitanPart& part) const;
	// The game logic's damage (0x004c0e40) on a Titan's armour: true when
	// `id` is a Titan or one of its parts (the damage handled).
	// `drain`: the wreck's own (damage type -5), which a wrecked Titan takes.
	bool titanDamage(uint32_t id, float amount, int attacker, bool fromChild = false, bool drain = false);
	void setTitanPartHitPoints(Titan& titan, int part, float value, int attacker);
	void titanPartWrecked(Titan& titan, int part, int attacker);
	void titanArmorEffects(Titan& titan, int part);
	void stepTitanWreck(Titan& titan, float seconds);
	bool m_aiRunning = false;
	std::set<uint32_t> m_hiddenChildren; // wrecks with hideChildren (partTransforms' quick test)
	// Destroyable props (Props.cpp): the level's DestroyableObjects with
	// armour (rescanned when the object count changes) and the broken ones.
	struct BrokenProp {
		uint32_t object = 0;
		double removeAt = 1e30; // a draining wreck's end (canBeDestroyed, timeToStayAsWreck)
		int geometry = -1;
	};
	std::vector<BrokenProp> m_brokenProps;
	std::vector<uint32_t> m_props;
	size_t m_propsScanned = static_cast<size_t>(-1);
	bool isProp(uint32_t object) const;
	const std::vector<uint32_t>& props();
	bool breakProp(uint32_t object, int attacker);
	void stepProps();
	void resetProps();
	void removeStaticParts(uint32_t object); // the mobile mesh physics' parts of a static object
	std::set<uint32_t> m_bonedObjects;   // bodies with bone parts (partBone's quick test)
	bool titanTerrainClear(const Titan& titan, float x, float z) const;
	void findTitanObstacles();
	// One tick of a Titan's move (0x005aca70's loop), from `from` towards
	// the order's target (pushed only once `pushing`), steering off the
	// other Titans of `titans` and the tall static objects.
	Titan::SyncState titanMoveTick(const Titan& titan, const std::vector<Titan>& titans, const Titan::SyncState& from,
		float targetX, float targetZ, bool pushing, bool canMove, float seconds) const;
	TitanSync titanSyncOf(const Titan& titan, bool order) const;
	std::vector<TitanSync> m_titanSyncOut;
	uint32_t m_titanSyncSent = 0; // the manager's +0x30: the tick the last states went
	// A client's Titan one tick on (0x005aca70 on a client) and put at a
	// state (0x005aa0e0).
	void stepRemoteTitanSync(Titan& titan, uint32_t tick);
	void placeSyncedTitan(Titan& titan, const Titan::SyncState& state);
	std::vector<Titan*> titansByTeam(std::vector<Titan>& list);
	void stepTitans(float seconds);
	void moveTitanBody(Titan& titan, const physics::Vec3& delta);
	physics::Vec3 titanAim(const Titan& titan) const;
	// Silos.
	struct Silo {
		uint32_t object = 0;
		int team = 0;
		bool loaded = false;
		float reloadLeft = 0;
		float reloadTime = 120;   // ammo.reloadTime
		float launchDelay = 1;    // fire.fireLaunchDelay
		float launchIn = -1;      // a launch pending (seconds)
		float velocity = 4;
		physics::Vec3 muzzle;     // the barrel's pivot in the silo's frame
		float muzzleLength = 0;   // the muzzle along the aim from it
		const ProjectileSettings* missile = nullptr;
		uint32_t target = 0;
		int launched = 0;
		// The firearm's Radio fire messages (Missile_Silo_firearm's
		// radio.friendlyFireMessage / enemyFireMessage).
		std::string friendlyFire, enemyFire;
	};
	std::vector<Silo> m_silos;
	void registerSilo(world::Object& object);
	void stepSilos(float seconds);
	void launchMissile(Silo& silo, const Titan& target);
	void steerBeacon(Projectile& p, float seconds);
	// The soldiers' parachutes (Parachutes.cpp): the kit's ParachuteLauncher
	// (itemIndex 9, a SpawnObjectFireComp whose projectile is the Parachute
	// "parachute") puts one over a falling soldier, who rides it
	// (enterOnSpawn, dontAllowExit) until it lands.
	struct Parachute {
		uint32_t object = 0;
		int player = -1;       // the rider, -1 once he is out
		float landing = -1;    // the landing timer (+0x45c; 0x005cf750 sets it, 0x005d01c0 runs it)
		float removeIn = -1;   // to the timer event 0x21 (0x005cff20: 0.1 s), which takes it away
		int state = 0;         // 0x005cfbb0's message (+0x460): 1 high, 2 near the ground, 4 landing
		float stateTimer = 1.0f / 3.0f; // to the next timer event 0x20 (0x005cf6d0 / 0x005d0220: 1/3 s)
		// The canopy: its AnimatedBundle child (animatedparachute; object
		// 0x005c06a0) with its skeleton instance (+0x198) and the system of
		// its animationSystem1P (+0x18c), messaged by 0x005cfbb0 through the
		// child's interface 0xc49e and run by its update (0x005c00e0).
		std::unique_ptr<anim::AnimationState> canopy;
		const anim::Skeleton* canopySkeleton = nullptr;
		anim::LocalPose canopyPose;
		std::vector<anim::Matrix> canopyWorld; // model-space matrices per node, for the drawing
	};
	std::vector<Parachute> m_parachutes;
	Parachute* findParachute(uint32_t object);
	const Parachute* findParachute(uint32_t object) const;
	// The canopy's system and skeleton (the Parachute's first AnimatedBundle
	// child with both); false without one.
	bool loadCanopy(Parachute& chute, const world::ObjectTemplate& t);
	// One update of the canopy (0x005c00e0) with the parachute's message.
	void stepCanopy(Parachute& chute, float seconds);
	// The kit's parachute launcher: its slot, its projectile and
	// fire.projectileStartPosition; false without one.
	bool parachuteLauncher(const Player& player, std::string& projectile, physics::Vec3& start) const;
	// The SpawnObjectFireComp's fire test (slot 7, 0x005e4750); `projectile`
	// and `start` the launcher's round and its start (parachuteRoom).
	bool mayOpenParachute(const Player& player, const std::string& projectile, const physics::Vec3& start) const;
	// The room for the canopy (0x005e45e0): the round template's collision
	// mesh (the collision manager's, DAT_00a82510 slot 0x28) placed in the
	// round's frame (the soldier's heading, at `start` from his feet)
	// against the objects within 15 m.
	bool parachuteRoom(const Player& player, const std::string& projectile, const physics::Vec3& start) const;

	bool openParachute(Player& player);
	// The rider's tick in place of a seat's (stepSoldier).
	void rideParachute(Player& player, world::Object& soldier, Body& body, float seconds);
	void stepParachutes(float seconds);
	// The Parachute's collision handler (0x005cf750) before the
	// PlayerControlObject's: the landing.
	void parachuteCollision(uint32_t self, uint32_t other, const physics::Vec3& normal, uint32_t otherMaterial);
	void leaveParachute(Player& player, Parachute& chute);
	// Nothing a soldier of the team stands on, nor what stops a round,
	// between two points (`ignore`: an object left out).
	bool soldierRayClear(int team, const physics::Vec3& from, const physics::Vec3& to, uint32_t ignore) const;
	bool soldierRayHit(int team, const physics::Vec3& from, const physics::Vec3& to, uint32_t ignore, float& fraction) const;
	// Drop pods (a Parachute PCO with isPod; ForceObjects steer it).
	struct Pod {
		uint32_t object = 0;
		int player = -1;
		physics::Vec3 position, velocity;
		float gravityModifier = 1, drag = 0, mass = 1;
		physics::Vec3 dragModifier {1, 1, 1};
		float forward = 0, strafe = 0; // the ForceObjects' force on PIThrottle / PIYaw
		// The PlayerControlObject's +0x444 (slot 0x178, 0x0059d2a0; a spawn
		// point's disablePodControls): the input filter 0x005ca2e0 zeroes
		// its inputs 0..3, so the ForceObjects get none.
		bool controlsOff = false;
		float age = 0;
		float still = 0; // seconds at rest
		// Its keepUp ForceObject (eu_pod_ForceObject: regulatePosX / Z
		// -0.5/1 at 0/1.5/0; 0x0062b750 with the linear regulator
		// 0x00667710): the couple Vehicle::updateForce gives a vehicle's.
		bool keepUp = false;
		physics::Vec3 keepUpAt;
		float a[3] = {}, b[3] = {};
		bool regulated[3] = {};
	};
	bool podLauncherOf(const world::ObjectTemplate& node, std::string& projectile, float& velocity,
		physics::Vec3& start, bool altFire) const;
	// `yaw` (degrees): the pod's heading; NaN takes the velocity's.
	bool startPod(Player& player, const std::string& projectile, const physics::Vec3& from, const physics::Vec3& velocity,
		uint32_t launcher, float yaw = std::numeric_limits<float>::quiet_NaN());
	std::vector<Pod> m_pods;
	void stepPods(float seconds);
	void stepTitanSpawnPoints(Titan& titan);
	void raiseTitanEvent(int eventId, uint32_t object, int attacker);
	// The Python component's destroy and repair events (template +0x10 /
	// +0x14, the ids of the host's event table 0x009fce60; 64 is none) for
	// objects outside a Titan's tree, with the component's sent flags:
	// - wrecked by damage (0x005b7680: the armour alive before, a wreck
	//   after): its own component's onDestroyEvent with (object, attacker)
	//   unless already sent; the repair's flag cleared either way;
	// - repaired out of the wreck (0x004bc410 after the heal: the armour's
	//   state 1 before, 0 after): the nearest component up the tree
	//   (0x004a5570), its onRepairEvent with (object, repairer) unless
	//   already sent; the destroy's flag cleared either way.
	struct PythonSent {
		bool destroyed = false, repaired = false;
	};
	std::map<uint32_t, PythonSent> m_pythonSent;
	void pythonWrecked(uint32_t object, int attacker);
	void pythonRepaired(uint32_t object, int repairer);
	// The armour heal (0x00655a00 -> the setter 0x00655460) of `object` to
	// `value` by `repairer` (-1 none), then 0x004bc410's repair event.
	void healHitPoints(uint32_t object, float value, int repairer);
	// The same on a Titan's part (index > 0): `percent` of its maximum,
	// kept within -wreckHitPoints and maxHitPoints; out of the wreck the
	// part's subtree is back (0x00654120's repaired branch, 0x00654070) and
	// its onRepairEvent goes. False when nothing moved.
	bool healTitanPart(uint32_t id, float percent, int repairer);
	// The armour's template: a Titan part's node, else the object's.
	const world::ObjectTemplate* armourTemplate(uint32_t id) const;
};

class GameWorld::LoneVehicle {
public:
	~LoneVehicle();
	// Its state for remembering and putting back (the rigid body, the
	// response, the engines and wheels, the sleep).
	struct State {
		physics::RigidBody body;
		physics::ContactResponse response;
		std::shared_ptr<const Vehicle> vehicle;
		physics::SleepState sleep;
		float submerged = 0;
		uint32_t support = 0;
	};
	State save() const;
	void restore(const State& state);
	physics::RigidBody& body();
	const physics::RigidBody& body() const;
	const Vehicle* vehicle() const;

private:
	friend class GameWorld;
	LoneVehicle();
	std::unique_ptr<GameWorld::Body> m_body;
};

} // namespace dice::hfe::game
