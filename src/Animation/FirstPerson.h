// The first-person view's animation side: the soldier's and the held
// weapon's first-person systems (animationSystem1P) posing the first-person
// skeleton (skeleton1P, Objects/Soldiers/Common/Animations/1p_setup.ske),
// the eye's height by pose, the weapon's zoom (DefaultZoomComp) and the
// camera spring, as BF2142.exe's Soldier runs them for the local player.
//
// The first-person skeleton is the arms and the weapon's mesh nodes
// (mesh1..mesh16 under "torus") around a camera node, "Camerabone" (its
// root, at -0.024/1.504/-0.111 in the setup pose). The skeleton is placed
// so that its camera bone sits at the eye, turned with the view; the
// spring moves it along the view's axes (0x006acfa0, 0x008b8880). The
// weapon's first-person geometry (0 of its bundled mesh) hangs its parts
// from the mesh nodes; the soldier mesh's geometry 0 is the arms, skinned
// to this skeleton.
#pragma once

#include "AnimationSystem.h"
#include "Pose.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::anim {

// DefaultZoomComp (zoom.*, template defaults 0x005dacc0): the weapon's
// zoom levels, cycled by the alternate fire (0x005da670: the next level,
// past the last back to 0). Each level's factor is the field of view in
// radians that level asks the player for (0x005da3f0), 0 for the camera's
// own; the player's field of view goes there linearly over zoomDelay
// (0x00591740 / 0x005912c0). Zooming in sets the weapon's first-person LOD
// to zoomLod once zoomDelay has passed (0x005da770, round(delay x 30)
// ticks, 0x005da150), zooming out sets LOD 0 at once.
struct ZoomSettings {
	std::vector<float> factors {0}; // +0x18: zoom.addZoomFactor (duplicates skipped, 0x005dae50)
	float delay = 0.5f;          // +0x24: zoom.zoomDelay, seconds
	int lod = 0;                 // +0x2c: zoom.zoomLod
	bool disableMuzzle = false;  // +0x40: zoom.disableMuzzleWhenZoomed
	float mouseSpeedMod = 1;     // +0x3c: zoom.mouseSpeedMod
	bool zoomOutOnReload = true; // +0x4a
	bool present() const { return factors.size() > 1; }
	int delayTicks() const { return static_cast<int>(std::lround(delay * 30.0f)); }
};

// The player's field of view (+0xc0 target, +0xc4 current, +0xc8 rate):
// set to a target over a time (0x00591740: the rate is the distance over
// the time, 0 snaps), stepped each tick (0x005912c0).
struct FieldOfView {
	float target = 1.1f, current = 1.1f, rate = 0;
	void set(float to, float seconds);
	void update(float seconds);
};

class ZoomState {
public:
	// The alternate fire: the next level; returns the field of view to ask
	// for (0: the camera's).
	float cycle(const ZoomSettings& settings);
	// Back to level 0 (a weapon change, death, a reload with zoomOutOnReload).
	void reset();
	// One tick: counts towards the zoomed LOD.
	void tick();
	int level() const { return m_level; }
	// The weapon's first-person LOD now (0x005da770).
	int lod(const ZoomSettings& settings) const;
	float factor(const ZoomSettings& settings) const;

private:
	int m_level = 0;   // +0x1c of the component
	int m_ticks = 0;   // since the last change
};

// The eye (the soldier camera's local position, 0x005890a0): only the y of
// ObjectTemplate.poseCameraPosition counts (0x005c0c70 stores x and z as 0;
// 0 stand, 1 crouch, 2 prone; a seated soldier counts as crouching), and
// each tick the camera moves 0.2 of the way there (*0x00a68e14, 0x00a68eac,
// 0x00a68e18, all 0.2, 0x0043dd80). It starts where the template puts the
// camera child (SoldierCamera at 0/4.1/0).
struct EyeHeight {
	float poses[3] = {0.47f, -0.05f, -0.63f};
	float current = 4.1f;
	static constexpr float kSpeed = 0.2f;
	void tick(int pose);
};

// The weapon's recoil kick, as SoldierBasedRecoilComp runs it (template
// 0x005dd730, defaults below; the weapon's component 0x005dd370). The
// template builds a graph (0x005dd890): n = recoilGraphFrameCount values
// g[i] = -(i + 1)^recoilGraphExponent x recoilGraphTotalMovement / S, S the
// sum of the (i + 1)^exponent (so they add up to -totalMovement); with
// goBackOnRecoil a second one of n + m values, m = round(n / 4), the
// first m going back from -g[n-1] / 2 with 1.5 x the curve and the rest
// the curve x 1.5 on top, its positive values then scaled to cancel the
// negative ones (the kick returns), used instead (0x005dd030). Each shot
// (0x005dd490 -> 0x005dd3f0) draws its force: recoilForceUp (and
// recoilForceLeftRight) sampled, times zoomModifier while zoomed in
// (0x005dd090), and keeps the tick in a ring of 10 (+0x60). The value
// (0x005dd5d0, through the firearm's slot 0x11c = 0x005c1920 -> the
// component's slot 0x94 = 0x005dd710) is, k ticks after the last shot,
// force x graph[size - 1 - k] for k < size - 1, else 0 (the firearm's
// slot 0x110 factor is 1, 0x005b6b10). The soldier (0x005861a0, only
// with hasRecoilForce: slot 0x100 = 0x005c1960 -> 0x005dd080) feeds it to
// the camera spring's fire part (0x005ee980) and adds it to the aim's pitch
// (0x0058a6d0; the left-right one, 0x00586140, to its yaw).
struct RecoilSettings {
	bool hasRecoilForce = false; // +0x34: recoil.hasRecoilForce
	bool goBackOnRecoil = true;  // +0x35
	float zoomModifier = 1;      // +0x38
	int graphFrameCount = 8;     // +0x3c: recoil.recoilGraphFrameCount
	float graphExponent = 2;     // +0x40: recoil.recoilGraphExponent
	float graphTotalMovement = 2; // +0x44: recoil.recoilGraphTotalMovement
	// The graph the kick follows (0x005dd890 / 0x005dd030).
	std::vector<float> graph() const;
};

// The kick k ticks after a shot of `force` (0x005dd5d0).
float recoilAt(const std::vector<float>& graph, int ticksSinceShot, float force);

// The first-person skeleton posed by the soldier's and the weapon's
// first-person systems.
class FirstPersonRig {
public:
	FirstPersonRig(AnimationSystems& systems, const Skeleton& skeleton, const AnimationSystemTemplate* soldier);
	// The held weapon's system (null for none); a new one starts over
	// (its startup trigger: the deploy).
	void setWeapon(const AnimationSystemTemplate* weapon);
	const AnimationSystemTemplate* weapon() const { return m_weaponTemplate; }
	// One tick. Model-space node matrices follow. The systems' events this
	// tick (the soldier's, then the weapon's) go to `events` if given.
	struct Event {
		bool weapon = false; // the weapon's system, else the soldier's
		int type = Bundle::SoundEvent; // Bundle::SoundEvent or Bundle::EffectEvent
		int id = 0;
	};
	void update(const AnimationInput& input, float seconds, std::vector<Event>* events = nullptr);
	const Skeleton& skeleton() const { return m_skeleton; }
	const std::vector<Matrix>& pose() const { return m_world; }
	int cameraNode() const { return m_cameraNode; }
	// The camera spring: the soldier's system's with the weapon's parts over
	// it (0x008bc390); null when neither has one.
	const CameraSpring* spring() const { return m_hasSpring ? &m_spring : nullptr; }
	std::vector<std::string> playing() const;

private:
	AnimationSystems& m_systems;
	const Skeleton& m_skeleton;
	std::unique_ptr<AnimationState> m_soldier;
	std::unique_ptr<AnimationState> m_weapon;
	const AnimationSystemTemplate* m_soldierTemplate = nullptr;
	const AnimationSystemTemplate* m_weaponTemplate = nullptr;
	LocalPose m_pose;
	std::vector<Matrix> m_world;
	int m_cameraNode = -1;
	CameraSpring m_spring;
	bool m_hasSpring = false;
	void combineSprings();
};

// Where the first-person skeleton goes (0x005ee980): root = the inverse of
// the camera node's setup matrix x the camera's world matrix, so that the
// unanimated camera bone sits on the camera, its translation moved by the
// spring's offset along the camera's rows (x right, y up, z forward); the
// bones then hang from that root (0x008b8880, 0x006acfa0). Returns the model-to-world matrix (D3D row
// vectors) for a view at `eye` with yaw and pitch in radians (yaw 0 looks
// along +z, pitch up positive).
Matrix firstPersonPlacement(const Skeleton& skeleton, const std::vector<Matrix>& pose, int cameraNode,
	const Position& eye, float yaw, float pitch, const CameraSpring::V& springOffset);

} // namespace dice::hfe::anim
