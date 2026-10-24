// The local player's camera: first person (the soldier camera at the eye,
// or a vehicle seat's camera) or the rewrite's chase view, and what the
// first-person pass draws (the arms and the held weapon), as BF2142.exe's
// Soldier (0x005890a0), Player (0x005948e0, 0x00591740) and zoom component
// (0x005da3f0) run them for the player at the keys.
#pragma once

#include "Animation/FirstPerson.h"
#include "Effects/Effects.h"

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
	// One tick (1/30 s) after the world's.
	void tick(float seconds);

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

	game::GameWorld& m_round;
	anim::AnimationSystems& m_systems;
	Loader m_load;
	effects::EffectManager m_effects;
	bool m_altFire = false, m_altFireWas = false;

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

	anim::EyeHeight m_eye;
	anim::ZoomState m_zoom;
	anim::FieldOfView m_fov;
	anim::CameraSpringState m_springState;
	anim::CameraSpring::V m_springOffset;
	float m_lastYaw = 0, m_lastPitch = 0;
	bool m_alive = false;
};

} // namespace dice::hfe
