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
// Not rewritten yet: the spawn spot's drop onto what is under it
// (0x005ad8b0), and activation by events. useButtonRadius (waiting
// for a player to use the spawner) is set by no shipped level.
#pragma once

#include "Animation/AnimationSystem.h"
#include "Animation/FirstPerson.h"
#include "Effects/Effects.h"
#include "Physics/RigidBody.h"
#include "Random.h"
#include "ScriptGame.h"
#include "SoldierControl.h"
#include "Vehicle.h"
#include "Weapons.h"
#include "World/WorldMath.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
	int activateOnEvent = -1;
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
};

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

	RoundState state() const { return m_state; }
	const std::vector<ControlPoint>& controlPoints() const { return m_controlPoints; }
	const std::vector<std::unique_ptr<ObjectSpawner>>& spawners() const { return m_spawners; }
	// An abandoned vehicle's countdown (0x005c8ba0): seconds of its
	// spawner's timeToLive left, -1 when it has none.
	float timeToLiveLeft(uint32_t object) const;
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
	// Puts the player's soldier at a spawn point of its team; false when
	// there is none or it lives already.
	bool spawnSoldier(int player);
	struct PlayerInfo {
		int id = -1;
		std::string name;
		int team = 0;
		bool ai = false;
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
		int squad = 0; // 0: none
		bool squadLeader = false;
		bool commander = false;
		bool cloaked = false; // active camouflage on
		// Spawned on the squad leader where only a prone soldier fits (the
		// spawn point's +0x18a): the soldier starts lying down.
		bool spawnedProne = false;
		float battery = -1; // the ability battery, 0..1 (-1 without a kit)
		physics::Vec3 position;  // the soldier's feet
		float yaw = 0;           // degrees, 0 facing +z
		float aimPitch = 0;      // degrees, up positive: the look's (the input's)
		physics::Vec3 velocity;
		std::vector<std::pair<int, std::string>> carried; // (slot, weapon template)
		int inHandSlot = 0;
		float heat = 0; // the weapon in hand's (or the seat's gun's), 0..1
	};
	std::vector<PlayerInfo> playerList() const;
	bool playerInfo(int player, PlayerInfo& info) const;
	// The player whose soldier (or vehicle) an object is, -1 for none.
	int playerOf(uint32_t object) const;
	// An effect event N (0..9) of a player's held weapon's animation, as
	// its animation system fires it (see startEffectsOn).
	void weaponEffectEvent(int player, int effect);
	// The material a soldier's footstep set is for (0x007fda00; -1 before
	// its first collision event).
	uint32_t footstepMaterial(int player) const;
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
	// Whether a team's soldier could pass from one point to the other through
	// the moving objects' soldier meshes (a Titan's: with the other team's
	// forcefields, not its own).
	bool soldierPathClear(int team, const physics::Vec3& from, const physics::Vec3& to) const;
	// The spawn spot's drop (0x005ad8b0): a box from `bottom` to `top`
	// along `up` at `spot` put on what is under its top, or on the terrain.
	bool groundMaterialAlong(const physics::Vec3& from, const physics::Vec3& to, uint32_t ignore, uint32_t& material) const;
	physics::Vec3 dropSpawnSpot(const physics::Vec3& spot, const physics::Vec3& up, float bottom, float top) const;
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
	};
	std::vector<ControlPointInfo> controlPointList() const;
	// Where a player wants to spawn next (a control point id, -1 for any of
	// the team's): the spawn menu's choice. Held points the team owns
	// with spawn points; anywhere else falls back to any.
	void setSpawnChoice(int player, int controlPoint);
	// Squads (1..9 per team, up to six soldiers; the first is the leader):
	// members may spawn on their leader or the leader's spawn beacon
	// (spawnChoice kSpawnOnLeader / kSpawnOnBeacon). The scripts hear
	// PlayerChangedSquad.
	static constexpr int kSquadSize = 6;
	static constexpr int kSpawnOnLeader = -2;
	static constexpr int kSpawnOnBeacon = -3;
	// Joins a squad of the player's team (0 leaves), false when full;
	// a new squad's first member leads it.
	bool joinSquad(int player, int squad);
	// A squad of the team with room (a new one if none): the squad id.
	int openSquad(int team) const;
	// The squad's beacon object (0 for none).
	uint32_t squadBeacon(int team, int squad) const;
	// Unlocks: a kit's ItemContainer children (one per branch and level,
	// unlockLevel) add their items once unlocked; a slot holds the first
	// unlocked item for it (the customise screen's choice is not in yet).
	// Without a stats server nothing is unlocked; `all` unlocks everything.
	void setUnlocks(int player, std::vector<std::string> containers, bool all = false);
	void setBattery(int player, float battery); // tests and the scripts' view
	bool unlockAllByDefault = false; // players joining from now on have everything
	// The local player's unlocks (its soldier's chosen ItemContainers,
	// Stats/Stats.h), given when spawnPlayer adds the player.
	std::vector<std::string> localPlayerUnlocks;
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
	const physics::CollisionWorld& staticCollision() const { return m_collision; }

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
	size_t projectilesInFlight() const { return m_projectiles.size(); }
	const std::vector<Projectile>& projectiles() const { return m_projectiles; }
	// Armor: hit points left (from armor.hitPoints), -1 without armor.
	float hitPoints(uint32_t object) const;
	// Sets them (the scripts' setDamage); none left kills a soldier or
	// destroys the object.
	void setHitPoints(uint32_t object, float hitPoints);
	const FireArm* playerWeapon() const { return m_player && m_player->alive ? m_player->weapon.get() : nullptr; }
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
	struct SeatLayout {
		std::string name;            // the seat's PlayerControlObject
		physics::Vec3 position;      // where the soldier sits
		physics::Vec3 exit;          // where a soldier leaving goes
		bool hasExit = false;
		std::string weapon;          // a GenericFireArm, or empty
		physics::Vec3 weaponPosition;
		std::string yawBundle, pitchBundle;
		float minPitch = -90, maxPitch = 90; // the pitch bundle's limits (its rotation's y; down positive)
		physics::Vec3 rotation;      // seatInformation's yaw/pitch/roll, degrees
		std::string animationSystem; // seatAnimationSystem: the sitter's pose (empty: hidden)
		// The yaw bundle's frame in the vehicle (its parents' turn: the
		// Titans' gun hubs sit turned 180 or +-90 degrees) and its limits
		// (minRotation / maxRotation x; free without them).
		float frameYaw = 0;
		float minYaw = -1e9f, maxYaw = 1e9f;
		// The yaw bundle's regulateVector x (template +0x314) and maxSpeed x:
		// the bundle counter-turns with the hull (0x005644b0, see stepGunner).
		float regulateYaw = 0, yawSpeed = 0;
		float cameraRegulateYaw = 0; // the camera's regulateVector x
		// The seat's camera when it turns on the mouse (a Camera with
		// inputToYaw PIMouseLookX / inputToPitch PIMouseLookY: the drivers'
		// free look, the FAV gunners' lean with automaticReset), its axes
		// (yaw x, pitch y) and pivot.
		std::string camera;
		RotationAxis cameraYaw, cameraPitch;
		physics::Vec3 cameraPivot;
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
	static float regulatedInput(float input, float regulated);
	bool gunAim(uint32_t vehicle, int seat, float& yaw, float& pitch) const;
	const FireArm* gun(uint32_t vehicle, int seat) const;
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
	};
	std::vector<VehicleInfo> vehicleList();
	// The terrain's height under a point (the heightmap the physics uses; 0
	// without one).
	float terrainHeight(float x, float z) const { return m_ground.height ? m_ground.height(x, z) : 0.0f; }
	// The battlefield-objects report (BF2142.exe event 0x41, server
	// 0x005088f0 -> IAIEventCourier vt+0xe0, AIDLL 0x1004e5e0 -> 0x100bf000):
	// `reporter`'s squad now knows of the enemy `players` (and the
	// occupants of their vehicles). The bots send one as their trigger
	// presses or lets go at their target (AIDLL 0x1009a1d0, 0x1009a2a0).
	void reportBattlefieldObjects(int reporter, const std::vector<int>& players);
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
	};
	std::vector<WeaponAmmo> weaponAmmo(int player) const;
	// A projectile template's detonation at a point, as if it went off
	// there (false without one).
	bool detonate(const std::string& projectileTemplate, const physics::Vec3& at, int attacker = -1);
	// A vehicle's or drone's EMP level (0: running) and whether it is a wreck.
	int empLevel(uint32_t vehicle) const;
	// What a vehicle's driver inputs are scaled by (0x005ca2e0): EMP and
	// critical damage; see GameWorld.cpp.
	static float driverInputScale(float empLevel, float hitPoints, const world::ObjectTemplate& vehicle);
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
	// applicants are ranked by; 0 until set.
	void setPlayerRank(int player, int rank);
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
	};
	std::vector<TitanInfo> titans() const;
	// The AI runs on the server (AIDLL's interface 0xb47ce4, its slots 0x18
	// and 0x20): a wreck's armour then drains three times as fast (0x00652380).
	void setAiRunning(bool on) { m_aiRunning = on; }
	bool isTitan(uint32_t object) const; // the Titan's root (0x00561d90)
	uint32_t titanOf(int team) const;     // 0 without one
	// The commander's move order (Commander.moveTitan, the order event's
	// server side 0x0050f1f0 -> 0x005abba0): the point must leave the Titan
	// clear of the terrain (0x005aba40); taken, it holds from two seconds
	// on (the tick + 2 / tick length). The answer is the commander's radio
	// message (0x00757xxx: AUTO_TITAN_CC_moveaccepted / _unablemove).
	enum class TitanMove { Accepted, Unable, NoTitan, TooSoon };
	TitanMove moveTitan(int team, float x, float z);
	// The Titan's armoured parts (its tree's templates with armour below the
	// root): consoles, the core and its doors, the guns, the forcefield.
	struct TitanPartInfo {
		std::string name;     // the part's template
		uint32_t id = 0;      // what hits on it report (Impact::object)
		enum Kind { Other, Console, Core, CoreDoors, Gun, Shield } kind = Other;
		float hitPoints = 0, maxHitPoints = 0;
		bool wreck = false;
		physics::Vec3 position; // in the world
	};
	std::vector<TitanPartInfo> titanParts(int team) const;
	// The game logic's damage (0x004c0e40) as a shot's hit: through the
	// armour's rules (a Titan's floors and parts' parent damage), `attacker`
	// the player (-1 for none). No material cell (a cell of 1).
	void applyDamage(uint32_t object, float amount, int attacker = -1) { loseHitPoints(object, amount, attacker); }
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
		int launched = 0;       // missiles fired this round
	};
	std::vector<SiloInfo> missileSilos() const;
	// The Titans' interior spawn points (as_titan_SpawnPoint children, moved
	// with the Titan); their objects.
	std::vector<uint32_t> titanSpawnPoints(int team) const;
	// Launches a soldier standing at a Titan's pod array (titan_pod_array1/2,
	// within an EntryPoint's entryRadius) or riding a pod-launching seat in
	// a drop pod (the launcher's projectileTemplate, a Parachute PCO with
	// isPod): false when there is none at hand.
	bool launchPod(int player);
	// For a soldier aboard its own Titan (within its box): the nearest entry
	// point of the Titan's pod arrays, in the world, and its radius.
	bool titanPodEntry(int player, physics::Vec3& entry, float& radius) const;
	// The nearest free gun seat of the player's own Titan whose gun works
	// (its console's entry point, world frame), for a soldier on foot within
	// the Titan's box; false for none.
	// `seat` >= 0 asks for that seat only; `skip`: seats others are going to.
	bool titanGunEntry(int player, physics::Vec3& entry, float& radius, int& seat, const std::vector<int>& skip = {}) const;
	bool ridingPod(int player) const;
	struct PodInfo {
		uint32_t object = 0;
		int player = -1;
		physics::Vec3 position, velocity;
	};
	std::vector<PodInfo> pods() const;

	// A soldier's hit capsules (setSkeletonCollisionBone) in world space as
	// of the last tick: segment ends and radius; for checks and drawing.
	struct Capsule {
		physics::Vec3 a, b;
		float radius;
		uint32_t material;
	};
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
		std::vector<physics::Vec3> points; // vehicle collision LOD
		std::shared_ptr<physics::CollisionWorld> projectileMesh; // object space, projectile LOD
		std::shared_ptr<physics::CollisionWorld> soldierMesh;    // object space, soldier LOD (ragdolls)
		// A Titan's: what each team's soldiers meet (the soldier mesh and the
		// other team's forcefields; collisionGroups 0x100 / 0x200).
		std::shared_ptr<physics::CollisionWorld> teamSoldierMesh[2];
		std::unique_ptr<Vehicle> vehicle; // engines and wheels
		// Seats: who sits in each (player id, -1 for nobody) and the guns.
		std::vector<int> occupants;
		double occupiedAt = -1e9; // when someone last sat in it (MagneticObject's disableWhenEmptyVehicle)
		struct Gun {
			std::unique_ptr<FireArm> weapon;
			const ProjectileSettings* projectile = nullptr;
			float yaw = 0, pitch = 0; // the bundles' rotation, degrees (pitch up positive)
		};
		std::vector<Gun> guns;
		// Each seat's camera angles and speeds (SeatLayout::camera), degrees.
		struct Look {
			float yaw = 0, pitch = 0, yawSpeed = 0, pitchSpeed = 0;
		};
		std::vector<Look> looks;
		// Armour (armor.*): the EMP level (armor 0x27c, see empBlast),
		// falling by one every empLevelTime; destroyed, it lies as a wreck
		// for timeToStayAsWreck.
		float empLevel = 0;
		float wreckTime = -1; // >= 0 while a wreck
		float wetTime = 0;    // seconds in water
		// The vehicle LOD's edges (indices into `points`) of its faces
		// steeper than 30 degrees (|normal y| at most cos 30), each face's
		// three in turn (the loader 0x006e12a0 -> 0x006e1430, LOD +0xe4).
		std::vector<std::array<uint16_t, 2>> edges;
		// The vehicle LOD's box (object space; the mesh's slot 0x54), which
		// the third pass tests trees against.
		physics::Vec3 boxMin, boxMax;
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
	void stepVehicleDamage(float seconds);
	bool disabled(const Body& body) const { return body.empLevel > 0 || body.wreckTime >= 0; }

	void seatPlayer(Player& player, Body& body, int seat);
	void stepGunner(Player& player, Body& body, float seconds, bool aims = true);
	Body* findBody(uint32_t object);
	int firstEngineType(const world::ObjectTemplate& root) const;
	void addBodies();
	void stepPhysics(float seconds);
	const io::FileManager* m_files = nullptr;
	physics::CollisionTemplateManager* m_collisionTemplates = nullptr;
	physics::Ground m_ground;
	const physics::MaterialManager* m_materials = nullptr;
	std::vector<uint32_t> m_checked; // object ids already considered for a body (sorted)
	std::vector<Body> m_bodies;
	physics::CollisionWorld m_collision; // static objects, soldier LOD
	physics::CollisionWorld m_projectileCollision; // static objects, projectile LOD

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
	};
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
	std::map<std::pair<const physics::CollisionMeshFile*, std::vector<uint32_t>>, std::shared_ptr<const MeshShape>> m_shapes;
	uint32_t m_staticStamp = 0;
	std::shared_ptr<const MeshShape> meshShape(const physics::CollisionMeshFile& mesh, const std::vector<uint32_t>& materials);
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
		physics::Vec3 pointVelocity(const physics::Vec3& point) const
		{
			return body ? body->body.pointVelocity(point) : physics::Vec3 {};
		}
	};
	MeshActor actorOf(Body& body) const;
	MeshActor actorOf(const StaticPart& part) const;
	void collideBodies(float seconds);
	void collideMover(Body& mover, size_t index, float seconds);
	// Slot 9 of the mobile mesh response (0x006bb050): A's mesh against
	// B's; `trunkOnly` runs the third pass alone (its last argument).
	// Returns false when it destroyed A (the helicopter exit).
	bool meshPair(MeshActor& a, MeshActor& b, float seconds, bool trunkOnly);
	void wheelProbes(Body& body, MeshActor& b, float seconds);
	physics::MassShares addMeshContact(MeshActor& a, MeshActor& b, const physics::Vec3& point, const physics::Vec3& normal, float depth,
		uint32_t materialA, uint32_t materialB, bool trunkPass, bool toA = true);
	std::vector<uint32_t> materialMap(const world::ObjectTemplate& objectTemplate) const;
	void buildStaticCollision();
	void addStaticCollision(const world::ObjectTemplate& objectTemplate, const physics::Mat3& rotation,
		const physics::Vec3& position, uint32_t owner, int depth);

	struct HitBone {
		int node;            // skeleton node
		uint32_t material;
		physics::Vec3 offset; // in the bone's frame
		float radius;
		float length;         // along the bone's -y
	};
	struct Player {
		int id = 0;
		std::string name;
		bool ai = false;
		bool alive = false;
		int rank = 0; // setPlayerRank (the commander's applicants)
		float respawnIn = 0;
		int kills = 0, deaths = 0, teamKills = 0;
		SoldierInput input;
		std::vector<HitBone> hitBones;
		int spawnChoice = -1;
		int squad = 0;
		bool squadLeader = false;
		std::string kitTemplate, soldierTemplate; // the next spawn's kit (empty: the team's assault kit)
		std::vector<std::string> unlocked; // ItemContainer templates
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
		bool selectHeld = false;
		bool abilityActive = false; // the gadget in hand is working this tick
		bool cloaked = false;       // CamouflageAmmoComp held with the trigger down
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
		// The unlocks' ability.increasedRepairAbility (player +0x30c
		// +0x48, copied by 0x005706b0 when above 0): scales the
		// abilities' strength when above 0.
		float abilityModifier = 0;
		double diedAt = 0;          // soldier +0x290: the death's time
		uint32_t object = 0;
		physics::SoldierBody body;
		SoldierControl control;
		std::unique_ptr<FireArm> weapon;
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
		// The player's kit time stamp (player +0x230/+0x234, set on a pickup):
		// a kit is picked up by the use key at most every 2 s.
		double kitTimeStamp = -1e9;
		float lastYaw = 0;
		bool fired = false;
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
		// On a ladder (its LadderContainer, 0x0059dec0): the ladder (an index
		// into m_ladders, -1 for none), the container's height.
		int ladder = -1;
		float ladderY = 0;
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
	bool tryLadder(Player& player, SoldierInput& input);
	void climbLadder(Player& player, SoldierInput& input, float seconds);
	bool leaveLadder(Player& player, int mode);
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
	std::map<std::string, std::unique_ptr<ProjectileSettings>> m_projectileSettings;
	std::vector<Projectile> m_projectiles;
	std::vector<Impact> m_impacts;
	std::map<uint32_t, float> m_hitPoints;
	effects::EffectManager m_effects;
	void fire(const Player& player, float aimPitch);
	// `exclude`: the object whose own blast it is (its armour, 0x005a9090's
	// param_6: neither hurt nor pushed by it).
	void explode(const physics::Vec3& at, const ProjectileSettings& settings, int attacker, float facing = 0,
		uint32_t exclude = 0);
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
	std::map<uint32_t, float> m_resupply; // ammo owed to soldiers (fractions of a magazine)
	// `holder`: the player whose battery pays and whose unlocks scale it
	// (the gadget's holder, or the hub's owner); none: no battery.
	static constexpr uint32_t kSoldierFootStepsMaterial = 5500; // 0x157c
	void playRagDollImpact(anim::RagDoll& ragDoll, const anim::Vec& at);
	void startEffectsOn(const Player& player, int effect);
	void playAnimationSound(const Player& player, int sound);
	void applyAbility(const Ability& ability, const physics::Vec3& at, int team, float seconds, uint32_t skip,
		AbilityScan* scan = nullptr, Player* holder = nullptr);
	void stepAbilities(float seconds);
	void stepProjectiles(float seconds);
	void steerSeeker(Projectile& projectile, float seconds);
	void damage(uint32_t object, float amount, uint32_t projectileMaterial, uint32_t surfaceMaterial,
		int attacker = -1); // the attacking player
	std::vector<std::unique_ptr<Player>> m_players; // by id
	Player* m_player = nullptr; // the local player
	SoldierInput m_playerInput;
	float m_spawnTime = 15;
	Player* findPlayer(int id) const;
	Player* soldierPlayer(uint32_t object) const;
	world::Object* spawnSoldier(Player& player);
	void kill(Player& victim, Player* attacker, uint32_t weapon);
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
	std::vector<int> squadMembers(int team, int squad) const;
	bool spawnAtSquad(Player& player, world::Object*& at, physics::Vec3& position, float& yaw);
	bool scatterOnLeader(const Player& leader, physics::Vec3& feet, float& yaw, bool* prone = nullptr);
	bool soldierFits(const physics::Vec3& origin, SoldierPose pose, uint32_t ignore) const;
	float m_waterLevel = 0;
	bool m_hasWater = false;
	void stepPlayers(float seconds);
	void stepSoldier(Player& player, float seconds);
	// Takes the carried weapon of a slot in hand.
	void equip(Player& player, int slot);
	const ProjectileSettings* projectileFor(const std::string& projectileTemplate);
	world::ObjectTemplate* beaconTemplate(const world::ObjectTemplate* weapon) const;
	world::ObjectTemplate* deployableTemplate(const world::ObjectTemplate* weapon) const;
	std::map<std::pair<int, std::string>, uint32_t> m_deployed; // (owner, gadget) -> object
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
	struct Drone {
		uint32_t object = 0;
		int owner = -1;
		int team = 0;
		// The sentry's auto controller: the gun's heading (radians), its
		// target and when it was last seen.
		float gunYaw = 0;
		int target = -1;
		float targetSeenAt = 0;
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
		const ProjectileSettings* projectile = nullptr;
		bool pulse = false;
		float pulseDistance = 0; // pulse.viewDistance
		float revealTime = 0;    // pulse.revealTime
		float empLevel = 0;      // its armour's EMP level (see empBlast)
		DeviationBuildUp gunDeviation; // the gun's firing build-up (setFireDev)
	};
	std::vector<Drone> m_drones;
	// Soldiers killed in a seat that has a die bundle: the body stays,
	// slumped by the seat's animation system (0x005cea00, 0x005ed890).
	struct SeatCorpse {
		uint32_t object = 0;
		uint32_t vehicle = 0;
		int seat = -1;
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
	void dropKit(Player& player);
	void fillKit(Player& player, world::ObjectTemplate* kitTemplate, world::ObjectTemplate* primary);
	bool pickUpClosestKit(Player& player, float radius);
	void stepDroppedKits();
	std::vector<SoundEvent> m_sounds;
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
	void fireSentry(Drone& drone, float seconds);
	bool removeDrone(uint32_t object);
	Drone* findDrone(uint32_t object);

	// The commander (Commander.cpp). Per team (index 1, 2): the SquadManager's
	// commander (0x004c8120) and applicants (0x004c4fb0), the assets' tools
	// (DAT_00a69cb8: 0x005a1440 gathers them, 0x005a1270 fires), the UAV and
	// scan timers (DAT_00a6a390: 0x005a23b0, 0x005a2450) and the supply drop
	// (0x0043d6d0).
	double m_time = 0; // seconds since the round started
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
	};
	std::vector<StrikeFire> m_strikeFire; // alongside m_strikes
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
	};
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
		int destroyEvent = 0;   // Python.onDestroyEvent
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
	struct TitanMesh {
		const physics::CollisionMeshFile* mesh = nullptr;
		std::vector<uint32_t> materials;
		physics::Mat3 rotation;
		physics::Vec3 position;
		int node = -1;  // the template node's part (its armour's or its armoured ancestor's)
		std::vector<int> chain; // the parts it lies under (its collision goes with any of them)
		int groups = 0;         // collisionGroups: 0x100 / 0x200 a team's own forcefield (PAC's, EU's)
	};
	struct Titan {
		uint32_t object = 0;
		int team = 0;
		int controlPoint = -1;
		std::vector<TitanPart> parts; // [0]: the root
		std::vector<TitanMesh> meshes;
		// Its parts' vehicle LODs (in the Titan's frame), each a candidate of
		// its own for the vehicles' mesh physics (0x006dc7b0 / 0x006bb050)
		// with its collisionGroups: a team's vehicles pass their own fields.
		struct VehiclePart {
			std::shared_ptr<physics::CollisionWorld> mesh;
			std::vector<physics::Vec3> points;
			std::vector<uint32_t> pointMaterials;
			physics::Vec3 boxMin, boxMax;
			float radius = 0;
			uint32_t groups = 0;
		};
		std::vector<VehiclePart> vehicleParts;
		physics::Vec3 velocity;       // x and z (0x005aa0e0 sets the node's)
		// The move orders (+0x3c: tick -> target), the last order's time.
		std::map<uint32_t, std::pair<float, float>> targets;
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
	std::vector<std::unique_ptr<ObjectSpawner>> m_newSpawners; // a Titan's, made during the spawners' loop
	static constexpr uint32_t kTitanPartIds = 0x7f000000u;
	std::vector<std::pair<physics::Vec3, float>> m_titanObstacles; // static objects reaching the Titans (0x00562360)
	bool m_titanObstaclesFound = false;
	void registerTitan(uint32_t object, int controlPoint);
	bool titanGunWorks(uint32_t titan, int seat) const;
	void titanSeatsOut(Titan& titan);
	void removeTitanCarried(Titan& titan);
	void buildTitanParts(Titan& titan);
	void rebuildTitanMeshes(Titan& titan);
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
	bool titanTerrainClear(const Titan& titan, float x, float z) const;
	void findTitanObstacles();
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
	};
	std::vector<Silo> m_silos;
	void registerSilo(world::Object& object);
	void stepSilos(float seconds);
	void launchMissile(Silo& silo, const Titan& target);
	void steerBeacon(Projectile& p, float seconds);
	// Drop pods (a Parachute PCO with isPod; ForceObjects steer it).
	struct Pod {
		uint32_t object = 0;
		int player = -1;
		physics::Vec3 position, velocity;
		float gravityModifier = 1, drag = 0, mass = 1;
		physics::Vec3 dragModifier {1, 1, 1};
		float forward = 0, strafe = 0; // the ForceObjects' force on PIThrottle / PIYaw
		float age = 0;
		float still = 0; // seconds at rest
	};
	bool podLauncherOf(const world::ObjectTemplate& node, std::string& projectile, float& velocity,
		physics::Vec3& start, bool altFire) const;
	bool startPod(Player& player, const std::string& projectile, const physics::Vec3& from, const physics::Vec3& velocity,
		uint32_t launcher);
	std::vector<Pod> m_pods;
	void stepPods(float seconds);
	void stepTitanSpawnPoints(Titan& titan);
	void raiseTitanEvent(int eventId, uint32_t object, int attacker);
};

} // namespace dice::hfe::game
