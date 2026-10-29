// The geometry of BF2142.exe's cameras: the vehicle Camera object's view
// modes (CameraViewMode, setMode 0x00612eb0, the per-mode matrices picked by
// 0x00612d80) and the player's own camera (vtable 0x0097a8b0: the death
// camera, following another player and the free camera, 0x00615e70). Pure
// math on the objects' frames; PlayerCamera feeds them.
//
// Frames are the engine's: rows right, up, forward (row vectors, v * M) and
// a position. A Look is what the lookAt (0x006ae080) builds: an eye, the
// point it looks at and the up it keeps.
#pragma once

#include "Physics/RigidBody.h"

#include <deque>
#include <functional>

namespace dice::hfe::game::camera {

using physics::Mat3;
using physics::Vec3;

struct Frame {
	Mat3 rotation;
	Vec3 position;
	Vec3 right() const { return rotation.rows[0]; }
	Vec3 up() const { return rotation.rows[1]; }
	Vec3 forward() const { return rotation.rows[2]; }
};

struct Look {
	Vec3 eye, at;
	Vec3 up {0, 1, 0};
};

// The ground the cameras keep above: the heightmap cluster's slot 0x6c
// (the terrain's height) and slot 0x98 (the sea's).
struct Ground {
	std::function<float(float x, float z)> terrain;
	std::function<float(float x, float z)> sea;
	float terrainAt(float x, float z) const { return terrain ? terrain(x, z) : -1e9f; }
	float seaAt(float x, float z) const { return sea ? sea(x, z) : -1e9f; }
};

// CameraViewMode (0x004db0c0's names): setMode's argument.
enum class Mode { FirstPerson = 0, Inside = 1, Nose, Chase, FrontChase, FlyBy, Trace, ExternTrace };

// The Camera template's view flags (+0x378..+0x37e; 0x00610fd0 turns on
// CVMInside, CVMChase, CVMFlyBy and CVMFrontChase) and the server rules
// setMode checks.
struct ModeRules {
	bool inside = true, nose = false, chase = true, frontChase = true, flyBy = true, trace = false, externTrace = false;
	bool allowNoseCam = true;       // game logic +0x32 (sv.allowNoseCam)
	bool allowExternalViews = true; // game logic +0x36 (sv.allowExternalViews)
	// ExternTrace needs the camera's extern point (+0x2dc, the translation
	// of the matrix at +0x2ac) to be non-zero; its only setter, ICamera
	// slot 0x3c (0x006136b0), has no caller in BF2142.exe, so it never is.
	bool hasExternTarget = false;
};
// setMode's refusal (0x00612eb0): FirstPerson becomes Inside; each mode needs
// its flag, Nose sv.allowNoseCam too, Chase / FrontChase / FlyBy the
// external views.
bool modeAllowed(Mode mode, const ModeRules& rules);
// c_PIToggleCameraMode (0x004b9e10): steps on from `current` (back with
// `backwards`) over 0..7, wrapping, until setMode takes one or it is back
// where it started; FirstPerson is taken as Inside.
Mode nextMode(Mode current, const ModeRules& rules, bool backwards = false);

// A 4-vector the chase and death cameras smooth their direction in (w is
// carried along; the slerp 0x006ac980 treats it as a quaternion).
struct Dir4 {
	float x = 0, y = 0, z = 0, w = 0;
	Vec3 xyz() const { return {x, y, z}; }
	bool zero() const { return x * x + y * y + z * z <= 0; }
};
Dir4 slerp(const Dir4& a, const Dir4& b, float t);
Vec3 normalized(const Vec3& v);

// Chase and FrontChase (0x006108e0, reset by 0x006105c0).
struct ChaseSettings {
	float distance = -1;  // chaseDistance (+0x394): -1 the root's radius (at least 2) x 2 (*0x00a739ac)
	float angle = 0.3f;   // chaseAngle (+0x398): how far up the direction leans
	Vec3 offset;          // chaseOffset (+0x39c): the point looked at, in the chase parent's frame
	float stiffness = 1;  // followStiffness (+0x390): x the frame's seconds, the smoothing's share
	// A camera under a soldier whose root isn't one: the radius distance and
	// an angle of 0.3 whatever the template says.
	bool soldierParent = false;
};
struct ChaseState {
	Dir4 direction; // +0x258..+0x264
	float distance = 0; // +0x268
	void reset() { *this = {}; }
};
// `parent` is the chase parent's frame (chaseParentIndex: -1 the root, else
// that many parents up from the camera), `root` the root object's position
// and `radius` its bounding radius (slot 0x90).
Look chase(ChaseState& state, const ChaseSettings& settings, const Frame& parent, const Vec3& root, float radius,
	bool front, float seconds, const Ground& ground);

// Nose (0x00610800): the camera's own frame moved by nosePos (+0x380) turned
// by its parent's rotation.
Look nose(const Frame& camera, const Mat3& parent, const Vec3& nosePos);

// FlyBy (0x00612bb0, the points 0x00612b10 / 0x006123a0, the view 0x00612660):
// a point ahead of the root on its way (its radius x 1.2 along its forward
// plus 0.3 of its velocity, x 8 x 0.4), kept 1 m over the terrain, joins a
// list at most every 8 x 60 s; the camera sits on the list's point (between
// it and the next by the clock) and looks at the camera object's position.
struct FlyByState {
	struct Point {
		Vec3 position;
		float time;
	};
	std::deque<Point> points; // +0x274
	float clock = 0;          // +0x26c: the object's seconds
	void reset() { points.clear(); }
};
Look flyBy(FlyByState& state, const Vec3& cameraPosition, const Frame& root, const Vec3& rootVelocity, float radius,
	const Ground& ground);

// Trace (0x00610d20; setMode 6 starts the point 20 m ahead): the camera's
// position looking at a point that follows the parent's forward 20 m on,
// or at `target` (the player's slot 0x338 player's object) when that is
// more than 2 m away.
struct TraceState {
	Vec3 point, last; // +0x288, +0x294
};
void traceStart(TraceState& state, const Vec3& cameraPosition, const Frame& parent);
Look trace(TraceState& state, const Vec3& cameraPosition, const Frame& parent, const Vec3* target);

// The death camera (the player camera's mode 6 with no player followed,
// 0x00615970; started by setMode 6 with the dead control object, 0x00615f10,
// from the death 0x004cd390).
struct DeathState {
	Vec3 facing;      // +0x2e0: away from it the camera settles
	Dir4 direction;   // +0x2f0
	float distance = 0;   // +0x2ec
	float wanted = 2;     // +0x30c
	Vec3 target, smoothed; // +0x300, +0x310
};
// `forward` the camera's forward row then, `radius` the object's.
void deathStart(DeathState& state, const Vec3& position, const Vec3& forward, float radius);
// `body` the object's position while it is there, `killer` that of the
// player's killer while it lives.
Look death(DeathState& state, const Vec3* body, const Vec3* killer, float seconds, const Ground& ground);
// cam-stiffness, cam-interpolate-attachobject, deathcam-terrainoffset.
constexpr float kCamStiffness = 1.5f, kCamInterpolateAttach = 1.2f, kDeathTerrainOffset = 0.8f;

// Following another player (mode 6 with one followed, 0x00614d00): behind
// and above the player's camera by the root's radius, the offset easing
// in 1 - e^(-1/15) a frame, lagging 0.6 of its velocity.
struct FollowState {
	Vec3 offset; // +0x26c
};
Look follow(FollowState& state, const Frame& camera, float radius, const Vec3* velocity, const Ground& ground);

// The free camera (mode 1 unattached, 0x00615290), for spectators and, with
// sv.allowFreeCam, the dead.
struct FreeInput {
	float strafe = 0, throttle = 0; // c_PIYaw (+0), c_PIThrottle (+3)
	float turnDegrees = 0, pitchDegrees = 0;
	bool up = false, down = false;   // c_PILie, c_PICrouch
	bool slow = false, fast = false; // c_PISprint (cam-slow), c_PIAltSprint (cam-fast)
};
struct FreeState {
	Vec3 position;
	float yaw = 0, pitch = 0; // degrees
	float speed = 1;          // +0x280
};
// cam-slow 0.1, cam-normal 1, cam-fast 10 (the speeds) x 20 m a second.
constexpr float kCamSlow = 0.1f, kCamNormal = 1, kCamFast = 10;
void freeMove(FreeState& state, const FreeInput& input, float seconds, const Ground& ground);
Look freeLook(const FreeState& state);

// The yaw and pitch (radians; yaw 0 along +z, pitch up positive) a Look
// faces.
void anglesOf(const Look& look, float& yaw, float& pitch);

} // namespace dice::hfe::game::camera
