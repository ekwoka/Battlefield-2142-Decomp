// The local player's camera: first person (the soldier camera at the eye,
// or a vehicle seat's camera) or the rewrite's chase view, and what the
// first-person pass draws (the arms and the held weapon), as BF2142.exe's
// Soldier (0x005890a0), Player (0x005948e0, 0x00591740) and zoom component
// (0x005da3f0) run them for the player at the keys.
#pragma once

#include "Animation/FirstPerson.h"
#include "Effects/Effects.h"
#include "Game/Cameras.h"
#include "Game/Random.h"
#include "Game/Weapons.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::host {
class HudWorld;
}
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
	struct View;

	// On foot: first person (the game's only view there) or the rewrite's
	// own chase view behind (--camera-mode third, C). In a vehicle the seat
	// camera's view mode (game::camera::Mode) decides; a third-person
	// choice starts a seat in Chase.
	bool firstPerson = true;
	// c_PIToggleCameraMode (0x004b9e10): in a vehicle the next view mode the
	// seat's camera allows (0x00612eb0: its CVM* flags, sv.allowNoseCam,
	// sv.allowExternalViews); on foot first person and the chase view (the
	// latter only with sv.allowExternalViews). Dead and spectating with a
	// player followed, the next player (0x00615e00).
	void toggleMode(bool backwards = false);
	bool allowExternalViews = true;
	bool allowNoseCam = true; // sv.allowNoseCam (the seats' CVMNose views)
	// sv.allowFreeCam (game logic +0x35): the dead may fly a free camera or
	// follow teammates (0x00616110, 0x00615290).
	bool allowFreeCam = false;
	// The seat camera's view mode now (Inside when not in a seat).
	game::camera::Mode mode() const { return m_mode; }
	// Whether the view now is the first-person one (the soldier's eye or a
	// seat's Inside camera).
	bool insideView() const;
	// The view when it isn't first person and the game places it: a seat
	// camera's Nose, Chase, FrontChase, FlyBy or Trace view, the death
	// camera, a followed player's or the free camera (false otherwise: the
	// rewrite's own chase on foot, or before the first spawn).
	bool externalView(View& view, float aspect) const;
	// What the dead player's camera does (the player camera, vtable
	// 0x0097a8b0): none (alive), the death camera on the killed object
	// (mode 6 with no one followed, 0x00615970), following a teammate
	// (mode 6, 0x00614d00) or flying free (mode 1 unattached, 0x00615290).
	enum class Spectating { None, Death, Follow, Free };
	Spectating spectating() const { return m_spectating; }
	int followedPlayer() const { return m_followPlayer; }
	// c_PIAction pressed while dead (0x00616110): with sv.allowFreeCam it
	// switches between flying free and the death camera / following.
	void spectatorAction();
	// FreeCam.togglePlayer: the next living teammate to follow (0x00615ce0).
	void nextPlayer();
	// The local player is a spectator (player slot 0x3bc; JoinAsSpectator):
	// it flies the free camera whatever sv.allowFreeCam says (0x00615290's
	// test), starting there rather than on a death camera, and
	// FreeCam.toggleCamera swaps flying free and following.
	bool spectator = false;
	// FreeCam.toggleCamera (0x00752150): only for a spectator, the camera
	// to following (mode 6, the next player: slot 0x130) from flying free,
	// or back to flying free (mode 1). Returns the new mode (1 or 6; 0 when
	// not a spectator) for the server (JoinAsSpectator's kind).
	int toggleSpectatorCamera();
	// The keys for the free camera (c_PIYaw, c_PIThrottle, the mouse, c_PILie
	// up, c_PICrouch down, the sprints' speeds).
	void setFreeInput(const game::camera::FreeInput& input) { m_freeInput = input; }

	// The alternate fire as held now (its press cycles the zoom).
	void setAltFire(bool held) { m_altFire = held; }
	// The mouse wheel's step this frame (+1 c_PINextItem, -1 c_PIPrevItem):
	// a zoomed fine-tuning scope's step (ZoomState::fineTune).
	void setItemStep(int step) { m_itemStep = step; }
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
	// The players a joined game's spectator follows (the server's players
	// as the client has them: their ghosts' places and looks, the HudWorld
	// of the joined game). Without it only the local round's are followed.
	void setPlayers(const host::HudWorld* players) { m_players = players; }

	// The first-person camera now: false when it isn't shown (third person,
	// dead, no soldier).
	struct View {
		std::array<float, 3> position {};
		float yaw = 0, pitch = 0; // radians: yaw 0 along +z, pitch up positive
		float fovY = 1.0f;        // radians, for the window's aspect (RendDX9 0x10066ac0)
		float roll = 0;           // radians about the view axis (a seat camera's shakes)
	};
	bool view(View& view, float aspect) const;
	// A seat camera's shakes (the Camera object's 0x00610240, rolled about
	// the view axis by 0x00610620 through 0x005b1a70, degrees): the speed
	// shake past cameraShakeStartSpeed (template +0x3d0) up to
	// cameraShakeMaxSpeed (+0x3d4): sin(fmod(t x 40, 2 pi)) x 0.5 x the
	// share of the way between them (*0x00a739f8 = 40, *0x00a739fc = 0.5,
	// t the time spent above the start speed); and the hit shake
	// (0x00610450 then 0x00610240's last part): for 0.6 s (*0x00a73a14)
	// (1 - u) x sin(u x 7 x 2 pi) x the hit x 2 (*0x00a73a0c = 7,
	// *0x00a73a10 = 2), u the share of the 0.6 s gone. The hit (0x00610450's
	// argument) is a blast's amount from 0x005a6e70 (GameWorld::cameraShake,
	// taken each tick), the camera reached through player slot 0x1b4(0)
	// slot 0x4c (vtable 0x00979a50 slot 19); on foot the SoldierCamera's.
	// And the gun's camera recoil (game::CameraRecoil: the seat's
	// WeaponBasedRecoilComp's cameraRecoilSpeed / cameraRecoilSize, started
	// by each round its gun fires while the view is the inside or the nose
	// one), summed into the same turn (0x00610620).
	float shakeDegrees() const { return m_speedShake + m_hitShake + m_recoilShake; }
	// The field of view of the seat's camera for the view now (0x00610150:
	// insideFOV in the inside mode, worldFOV otherwise; both 1.1 by
	// default, 0x00610fd0), radians; 0 when not in a seat with a camera.
	float seatFov(bool inside) const;
	// The seat camera's static interior sun (0x006104d0 .. 0x00610580:
	// allowInsideDynamicLights / allowInsideStaticSun in the inside mode,
	// insideStaticSunDirection (-0.25, -0.5, 0.75), insideStaticSunColor
	// (1, 1, 1), insideStaticSunAmbientColor (0.7, 0.7, 0.7), and below
	// insideDamageThreshold (0.15) of its hit points the colour flickers
	// to insideStaticSunDamageColor (1, 0, 0) insideDamageFrequency (3)
	// times a second; the flicker's square wave is the rewrite's). For the
	// renderer's interior lighting.
	struct InsideSun {
		bool dynamicLights = true;
		std::array<float, 3> direction {-0.25f, -0.5f, 0.75f};
		std::array<float, 3> color {1, 1, 1};
		std::array<float, 3> ambient {0.7f, 0.7f, 0.7f};
	};
	bool insideSun(InsideSun& sun) const;

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
	// (weaponHud.altGuiIndex) shows. In a seat, the seat's gun's (below).
	int zoomLevel() const { return m_inSeat ? m_seatZoom.state.level() : m_zoom.level(); }
	bool zoomed() const;
	// The mouse's turn scale while zoomed (0x00586420): the field of view
	// over the camera's, times the zoom's mouseSpeedMod. The soldier's only
	// (0x00586420 is Soldier.cpp's): 1 in a seat.
	float mouseScale() const;
	float fieldOfView() const { return m_inSeat ? m_seatZoom.fov.current : m_fov.current; }
	// A seat's zoom (anim::SeatZoom: the seat's weapon's DefaultZoomComp,
	// stepped by the alternate fire). Not on a joined client (its seats have
	// no inside view).
	int seatZoomLevel() const { return m_seatZoom.state.level(); }
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

	// The seat camera's view modes and the dead player's camera (Cameras.h).
	void stepViews(float seconds);
	game::camera::ModeRules modeRules() const;
	bool setMode(game::camera::Mode mode);
	game::camera::Mode m_mode = game::camera::Mode::Inside;
	uint32_t m_modeVehicle = 0;
	int m_modeSeat = -1;
	game::camera::ChaseState m_chase;
	game::camera::FlyByState m_flyBy;
	game::camera::TraceState m_trace;
	bool m_traceStarted = false;
	Spectating m_spectating = Spectating::None;
	game::camera::DeathState m_death;
	uint32_t m_deathObject = 0;
	game::camera::FollowState m_follow;
	int m_followPlayer = -1;
	game::camera::FreeState m_free;
	game::camera::FreeInput m_freeInput;
	bool m_hasExternal = false;
	game::camera::Look m_external;
	float m_externalFov = 1.1f;
	// The last tick's view (the death camera starts from its forward).
	game::camera::Look m_lastLook;
	uint32_t m_lastControlObject = 0;
	game::camera::Ground ground() const;

	game::GameWorld& m_round;
	const host::HudWorld* m_players = nullptr;
	anim::AnimationSystems& m_systems;
	Loader m_load;
	effects::EffectManager m_effects;
	bool m_altFire = false, m_altFireWas = false;
	int m_itemStep = 0;
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
	// The seat's gun's zoom (seatZoomLevel): the gun it was loaded for.
	void stepSeatZoom(const Me& me, bool altPressed, float seconds);
	bool m_inSeat = false;
	const world::ObjectTemplate* m_seatGun = nullptr;
	anim::SeatZoom m_seatZoom;
	anim::CameraSpringState m_springState;
	anim::CameraSpring::V m_springOffset;
	float m_lastYaw = 0, m_lastPitch = 0;
	bool m_alive = false;
	// The seat camera's shakes (shakeDegrees) and their clocks.
	void stepShakes(float seconds);
	float m_speedShake = 0, m_hitShake = 0, m_shakeTime = 0;
	float m_hitLeft = 0, m_hitStrength = 0;
	game::CameraRecoil m_cameraRecoil;
	float m_recoilShake = 0;
	uint32_t m_lastGunShots = 0;
	uint32_t m_shakeVehicle = 0;
	float m_clock = 0;
};

} // namespace dice::hfe
