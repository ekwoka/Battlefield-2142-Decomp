// The helicopters' flight AI (AIDLL.dll's HeliMoveTo 0x1008e2e0 with the
// controller 0x100e50b0, HeliMoveToObject 0x1008e1c0, HeliAimAt 0x1008db80
// with 0x100e5590, HeliMoveAwayFromObject 0x1008d8b0, HeliResetControls
// 0x100e47a0 / 0x1008da80). The primitives are the controllers of
// 100e0000.c; their constants are AIDLL's .rdata (0x1012ad28...).
//
// The pilot's four inputs (the ControlInfo adaptor, BF2142.exe vtable
// 0x009d6b98: driveThrottle +0x14 PIThrottle, driveTurn +0x18 PIYaw,
// drivePitch +0x1c PIPitch, driveRoll +0x20 PIRoll) persist between ticks:
// a controller writes only what it sets. Their senses as the feedback has
// them: PIPitch > 0 lowers the nose, PIRoll > 0 dips the right wing, PIYaw
// > 0 turns right, PIThrottle is the collective (0..1 from the AI).
//
// AirTransportMoveTo (0x1008cc40, controller 0x100e1a50) is not used: no
// shipped AI template has the AirTransport or Plane equipment.
#pragma once

#include "AI/NavMesh.h"

#include <functional>

namespace dice::hfe::ai {

struct HeliControls {
	float yaw = 0;      // PIYaw
	float pitch = 0;    // PIPitch
	float roll = 0;     // PIRoll
	float throttle = 0; // PIThrottle
};

// What the AI object gives the controllers (AIDLL vtbl 0x101187d0 and its
// Mobile part): the hull's frame, its motion, and the terrain.
struct HeliSense {
	Vec3 position;
	Vec3 velocity;        // Mobile slot 0x18
	Vec3 angularVelocity; // Mobile slot 0x20, rad/s, world
	Vec3 forward, right, up;
	Vec3 acceleration;    // (v - the last v) / dt, which the interpreters keep
	// The terrain's height (0x10031a50's heightmap read).
	std::function<float(float x, float z)> terrain;
};

struct HeliState {
	HeliControls controls;
	bool airborne = false; // the bot's +0x140 (vt 0x17c), set by holdHeight over 5 m
};

// The controller primitives (100e0000.c). `localRate` (pitch, yaw, roll
// rates: right . w, up . w, forward . w) is computed from the sense.
class HeliPilot {
public:
	HeliPilot(HeliState& state, const HeliSense& sense);

	// 0x100e4440: hold the nose's (axis 0: forward.y) or the right wing's
	// (axis 2: right.y) height at `target`.
	void attitude(int axis, float target, float gain, float scale, float bias = 0);
	// 0x100e4960: lift the nose against a speed `v` along the hull beyond
	// `deadband`.
	void brake(float v, float deadband, float scale);
	// 0x100e4550: turn the heading to `dir` (horizontal); the error, degrees.
	float yawTo(const Vec3& dir);
	// 0x100e4130: level the hull and climb or sink to `wantY`.
	void holdHeight(float wantY);
	// 0x100e4a70: true when the terrain ahead takes over (pull up, recover
	// or turn from a wall); `maxTerrain` the highest of the probes ahead.
	bool terrainCheck(float& maxTerrain);
	// 0x100e4fa0: drift onto `goal` by leaning.
	void hoverAt(const Vec3& goal);
	// 0x100e50b0 (HeliMoveTo's controller): fly to `goal` at least `margin`
	// over the terrain ahead. HeliMoveTo's wrapper then lets the collective
	// go with a margin of 0.
	void flyTo(const Vec3& goal, float margin);
	// 0x100e5590 (HeliAimAt): point the hull's gun along `direction` (the
	// weapon's frame is the hull's for a fixed gun) from `margin` over the
	// terrain; `lastDirY` is the aim's y a tick ago (the pitch feed-forward).
	// True when the hull is lined up (yaw within 10 degrees, turning slowly).
	bool aimAt(const Vec3& direction, float margin, float lastDirY, float seconds);
	// 0x100e47a0: all four let go.
	void reset() { m_state.controls = {}; }

	float heightAboveGround() const;
	float heightAboveGround(const Vec3& p) const;
	const Vec3& localRate() const { return m_rate; }

private:
	HeliState& m_state;
	const HeliSense& m_sense;
	Vec3 m_rate; // pitch (right . w), yaw (up . w), roll (forward . w)
};

// The highest terrain round `goal` (0x100674f0: 100 samples 2..30 m away at
// any angle, from a fixed stream here) and the goal's own height.
float heliHoverHeight(const Vec3& goal, const std::function<float(float x, float z)>& terrain);

} // namespace dice::hfe::ai
