// The bots' ballistic lead (AIDLL.dll's solver record, constructor
// 0x100f9fd0, set up by 0x100fa060 and solved by 0x100fa6f0) and the
// on-target test built on it (the plan condition 0x100dcab0, vtable
// 0x1011b048, evaluated by 0x100dcb00).
//
// Every aim that leads a target owns one solver: the turret and soldier
// aim at an object (vtable 0x1011ab40 slot 8, 0x100d9a60), HeliAimAt's aim
// at an object (vtable 0x1011b0c0 slot 9, 0x100dcf10) and the aim at a
// position (vtable 0x1011b1c8 slot 9, 0x100dd6a0). The interpreters
// (MouseTurretAimAt 0x1008e940, HeliAimAt 0x1008db80) hand them the round
// the AI weapon describes (AIDLL 0x100a4130/0x100a4140/0x100a4150/
// 0x100a4110 -> the AI weapon adaptor, BF2142.exe vtable 0x009d5be8 slots
// 0x78/0x7c/0x80/0x108 -> the AI weapon template's +0x14, +0x2c, +0x28 and
// +0, which 0x00892be0 fills through 0x0088a8c0 from the firearm and its
// projectile template):
//   - the muzzle speed (+0x28): the firearm's velocity (GenericFireArm
//     slot 0x1b0) unless a script set one;
//   - the drag (+0x14): the projectile's drag (+0x78), for a Point or
//     RotationalPoint (physicsType +0xf4 1 or 2) over its mass (+0x94)
//     x 0.0314159 (pi / 100, what the point node's own drag works out at),
//     for physicsType 3 0.3 when it has any, else as it is;
//   - the gravity: IAIPhysics slot 4 (the world's) x the projectile's
//     gravityModifier (+0xc8, 1 without a projectile);
//   - indirect (+0, weaponTemplate.indirect): the high arc.
#pragma once

#include "AI/NavMesh.h"

namespace dice::hfe::ai {

struct RoundModel {
	float speed = 0;   // the muzzle speed, m/s
	float drag = 0;    // the AI's drag factor, per second
	float gravity = 0; // the vertical acceleration (negative: down)
	// The AI clock's step (DAT_10139440 slot 8): the flight is integrated
	// in steps of it.
	float step = 1.0f / 30.0f;
};

// The drag the AI weapon template keeps for a projectile (0x00892be0's
// switch on physicsType).
float roundDrag(float drag, float mass, int physicsType);

class LeadSolver {
public:
	// 0x100fa060: the shot. `offset` is the target's aim point less the
	// gun's position, `targetVelocity` the target's Mobile velocity (slot
	// 0x18; none: zero), `shooterVelocity` what the round inherits (the
	// shooter's velocity), `gunForward` the gun's forward (the gun matrix
	// times +z) and `now` the AI clock.
	void setup(const Vec3& offset, const Vec3& targetVelocity, const Vec3& shooterVelocity, const Vec3& gunForward,
		double now, const RoundModel& round);

	// 0x100fa6f0: the launch direction that puts a round on the target
	// where it will be when the round gets there. The launch angle is
	// searched in round(8 precision + 2) steps from the last solve's
	// (persisted, like the record's +0x60 and +0x64); `high` (an indirect
	// weapon) searches the high arc. On success `impact` is where the
	// target will be at the impact (relative to the gun) and `impactTime`
	// that time on the AI clock. A search that never brackets the target
	// with a step of a degree or more doubles the step for the next solve
	// and succeeds without writing `direction`, which then keeps what the
	// caller gave it.
	bool solve(double now, float precision, bool high, Vec3& direction, Vec3& impact, double* impactTime = nullptr);

	// 0x100fa5c0: the target's position relative to the gun at `time`.
	Vec3 targetAt(double time) const;
	// 0x100fa100: the time a round leaving at `speed` (along the ground)
	// takes to cover `distance` with the drag; 1000 when it never does.
	float flightTime(float distance, float speed) const;
	// 0x100fa200: how far a round leaving with `velocity` flies in
	// `seconds` (the drag linear in the speed, quadratic over 200 m/s; the
	// gravity on y; the trapezoid rule in the clock's steps).
	Vec3 flight(const Vec3& velocity, float seconds) const;
	// 0x100fa680: where (relative to the gun) a round fired along `direction`
	// now is after `seconds`; false from 100 s on.
	bool roundAt(const Vec3& direction, float seconds, Vec3& position) const;

	const Vec3& gunForward() const { return m_gunForward; }
	const RoundModel& round() const { return m_round; }
	float halfFlight() const { return m_halfFlight; }
	float angle() const { return m_angle; }

private:
	RoundModel m_round;
	double m_stamp = 0;       // +0x10: when the offset held
	Vec3 m_offset;            // +0x18
	Vec3 m_toTarget;          // +0x24: the offset's direction
	Vec3 m_targetVelocity;    // +0x30
	Vec3 m_shooterVelocity;   // +0x3c
	Vec3 m_gunForward {0, 0, 1}; // +0x48
	float m_halfFlight = 0;   // +0x58
	float m_angle = 666;      // +0x60: the last launch angle (666: none)
	float m_angleStep = 0.017453292f; // +0x64: a degree to start with
};

// The plan's on-target condition (0x100dcab0; its +8 the target, +0xc and
// +0x10 the reach squared and the reach, +0x14 the aim, +0x18 the
// closest-approach flag, +0x1c the best miss so far, +0x20 the angle). The
// reach is the AI weapon's allowedDeviation (slot 0x94 -> 0x008926f0: the
// AI weapon template's +0x44, 0.5 by default, weaponTemplate.
// allowedDeviation; the plain fire statement 0x1007fec0 takes its own
// value instead), the closest approach the weapon not being automatic
// (slot 0x104 -> 0x00892650: the fire component's mode is 2).
class OnTargetTest {
public:
	OnTargetTest(float reach, bool closestApproach);

	// 0x100dcb00: holds when the aim has a solution (its slot 0x2c), the
	// gun points within atan(0.0125 reach) of the solution's direction, and
	// a round fired now along the gun (aim slot 0x40, 0x100fbad0: the
	// solver's roundAt) lands within the reach of the solution's impact
	// point at its flight time. A weapon that isn't automatic waits for the
	// closest approach: while the miss shrinks it holds back, and it holds
	// on the first test the miss grows again when the best was within
	// reach.
	bool test(bool solved, const LeadSolver& solver, const Vec3& direction, const Vec3& impact, double impactTime,
		double now);

	float reach() const { return m_reach; }
	float maxAngle() const { return m_maxAngle; }

private:
	float m_reach;
	float m_reachSquared;
	bool m_closestApproach;
	float m_best;
	float m_maxAngle;
};

} // namespace dice::hfe::ai
