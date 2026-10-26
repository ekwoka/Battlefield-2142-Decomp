// The local player's camera: first person (the soldier camera at the eye,
// or a vehicle seat's camera) or the rewrite's chase view, and what the
// first-person pass draws (the arms and the held weapon), as BF2142.exe's
// Soldier (0x005890a0), Player (0x005948e0, 0x00591740) and zoom component
// (0x005da3f0) run them for the player at the keys.
#pragma once

#include "Animation/FirstPerson.h"
#include "Effects/Effects.h"
#include "Game/Random.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::game {
class GameWorld;
}
namespace dice::hfe::world {
class ObjectTemplate;
}

namespace dice::hfe {

class PlayerCamera {
public:
	using Loader = std::function<const anim::AnimationSystemTemplate*(const std::string& path)>;
	PlayerCamera(game::GameWorld& round, anim::AnimationSystems& systems, Loader load);
	~PlayerCamera();

	// First person (CVMInside, the game's only view on foot and its default
	// in vehicles) or the chase view behind (c_PIToggleCameraMode).
	bool firstPerson = true;
	void toggleMode() { firstPerson = !firstPerson; }

	// The alternate fire as held now (its press cycles the zoom).
	void setAltFire(bool held) { m_altFire = held; }
	// The crouch key (c_PICrouch) as held now: in a seat with allowDucking
	// it ducks (the PlayerControlObject's input, 0x005ca2e0 -> 0x005c7cf0).
	void setCrouch(bool held) { m_crouch = held; }
	// Ducking in a seat that allows it: its camera is the one with CameraId 1,
	// and with disableInputWhileDucking (the default) its inputs are cleared
	// (0x005c9ec0) and it doesn't fire (0x005cacf0).
	bool ducking() const;
	bool duckingDisablesInput() const;
	// One tick (1/30 s) after the world's.
	void tick(float seconds);

	// A joined game's soldier (--connect): the server runs it, so the view
	// takes it from the ghosts (its object, the look) instead of the local
	// game world. Seats aren't followed there (the chase view stays).
	struct Remote {
		bool alive = false;
		uint32_t soldier = 0, weapon = 0, kit = 0; // the ghosts' objects (0: none)
		const world::ObjectTemplate* soldierTemplate = nullptr;
		const world::ObjectTemplate* weaponTemplate = nullptr;
		std::array<float, 3> feet {};
		std::array<float, 3> velocity {};
		float yaw = 0, aimPitch = 0; // degrees: the local look
		int stance = 0;
		bool firing = false; // the fire input held
	};
	void setRemote(std::function<bool(Remote&)> source) { m_remote = std::move(source); }

	// The first-person camera now: false when it isn't shown (third person,
	// dead, no soldier).
	struct View {
		std::array<float, 3> position {};
		float yaw = 0, pitch = 0; // radians: yaw 0 along +z, pitch up positive
		float fovY = 1.0f;        // radians, for the window's aspect (RendDX9 0x10066ac0)
	};
	bool view(View& view, float aspect) const;

	// What the first-person pass draws.
	struct Model {
		std::string geometry;
		int geometryIndex = 0;
		int lod = 0;
		anim::Matrix world {};
		std::vector<anim::Matrix> pose;  // skinned
		std::vector<anim::Matrix> parts; // bundled
	};
	std::vector<Model> models() const;
	const effects::EffectManager& effects() const { return m_effects; }
	// The sounds of the effect bundles the first-person view started.
	std::vector<effects::EffectManager::StartedSound> takeSounds() { return m_effects.takeSounds(); }
	// The vehicle's cockpit (the Camera's 0x00612880): in first person in a
	// seat the vehicle's meshes with more than one geometry draw
	// cockpitSubGeom at cockpitLod: the seat camera's (Camera template
	// +0x44c / +0x448) unless -1, else the seat's (PlayerControlObject
	// slots 0xf8 / 0xf4: template +0x428 / +0x424, both 0 by default);
	// other views draw geometry 1 at the automatic LOD.
	struct Cockpit {
		uint32_t vehicle = 0;
		int subGeometry = 1;
		int lod = 0;
	};
	bool cockpit(Cockpit& cockpit) const;
	// The recoil kick this tick (0x005861a0, RecoilSettings).
	float recoil() const { return m_recoilNow; }
	// Objects the world pass leaves out in first person (the soldier and
	// what it carries).
	bool hides(uint32_t object) const;

	// The weapon's zoom: its level (0 not zoomed) and whether the zoomed HUD
	// (weaponHud.altGuiIndex) shows.
	int zoomLevel() const { return m_zoom.level(); }
	bool zoomed() const;
	// The mouse's turn scale while zoomed (0x00586420): the field of view
	// over the camera's, times the zoom's mouseSpeedMod.
	float mouseScale() const;
	float fieldOfView() const { return m_fov.current; }
	// The bundles the first-person systems play (checks).
	std::vector<std::string> playing() const;
	// The eye's height over the soldier's origin.
	float eyeHeight() const { return m_eye.current; }

private:
	struct SeatCamera;
	bool seatCamera(View& view) const;
	void resetSoldier();

	// The anchor the first-person weapon's bundles follow: its root (mesh1).
	static constexpr uint32_t kWeaponAnchor = 1;
	struct SeatView;
	bool findSeatCamera(SeatView& seat) const;
	// The player now, from the game world or the remote source.
	struct Me {
		bool alive = false;
		uint32_t soldier = 0, weapon = 0, kit = 0, vehicle = 0;
		int seat = -1;
		const world::ObjectTemplate* soldierTemplate = nullptr;
		const world::ObjectTemplate* weaponTemplate = nullptr;
		std::array<float, 3> origin {}; // the body's (a metre over the feet)
		std::array<float, 3> velocity {};
		float bodyYaw = 0, yaw = 0, aimPitch = 0; // degrees
		int stance = 0;
		bool reloading = false;
		int rounds = -1; // in the magazine, -1 unknown
		bool firing = false; // remote: the fire input held
	};
	bool current(Me& me) const;
	std::function<bool(Remote&)> m_remote;
	int m_lastRemoteShot = -1000;
	void startWeaponEffects(int effect);

	game::GameWorld& m_round;
	anim::AnimationSystems& m_systems;
	Loader m_load;
	effects::EffectManager m_effects;
	bool m_altFire = false, m_altFireWas = false;
	bool m_crouch = false;

	// The soldier this was set up for.
	const world::ObjectTemplate* m_soldierTemplate = nullptr;
	std::unique_ptr<anim::FirstPersonRig> m_rig;
	float m_baseFov = 1.0f; // the soldier camera's insideFOV
	float m_cameraStart = 4.1f;
	// The held weapon.
	const world::ObjectTemplate* m_weaponTemplate = nullptr;
	anim::ZoomSettings m_zoomSettings;
	int m_roundsWas = -1;
	bool m_reloadingWas = false;
	// The muzzle effects among the weapon's children (FireComp 0x005f25f0):
	// those named "zoom" flash while zoomed with disableMuzzleWhenZoomed,
	// the others otherwise (0x005f31d0, 0x005da050).
	struct Muzzle {
		std::string effect;
		std::array<float, 3> offset {};
	};
	Muzzle m_muzzle, m_zoomMuzzle;
	// The weapon's effect bundles started by its first-person system's
	// effect events (startOnEffects bit 9 + N), with their places on it.
	struct WeaponBundle {
		std::string name;
		std::array<float, 3> offset {};
		uint32_t startOnEffects = 0;
	};
	std::vector<WeaponBundle> m_weaponBundles;
	// SoldierBasedRecoilComp (RecoilSettings).
	anim::RecoilSettings m_recoil;
	std::vector<float> m_recoilGraph;
	game::RandomDistribution m_recoilForceUp;
	float m_recoilForce = 0, m_recoilNow = 0;
	int m_tick = 0, m_lastShotTick = -1000;
	game::Random m_random {0x759dc150};

	anim::EyeHeight m_eye;
	anim::ZoomState m_zoom;
	anim::FieldOfView m_fov;
	anim::CameraSpringState m_springState;
	anim::CameraSpring::V m_springOffset;
	float m_lastYaw = 0, m_lastPitch = 0;
	bool m_alive = false;
};

} // namespace dice::hfe
