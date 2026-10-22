// Ground vehicles: an Engine physics node driving Spring wheels, with the
// "newCar2" driving model (engineType c_ETNewCar2, 194), as eu_fav's
// template tree has it:
//
//     PlayerControlObject eu_fav           the rigid body
//       Engine EU_FAV_Engine               throttle, gears, the tyre settings
//         RotationalBundle EU_FAV_nav_LF   steering (inputToYaw PIYaw, +-32)
//           Spring EU_FAV_Wheel_LF         a wheel: suspension and tyre
//         Spring EU_FAV_Wheel_LB ...
//
// What is from the original:
//  - The springs (Spring node step 0x006caef0): a wheel pushed from its rest
//    place by the ground pushes its parent back with
//    disp * strength * 1.5 + disp velocity * damping (1.5 is
//    Physics.gravity * -0.101833), as an acceleration at the wheel (0xa4).
//  - The tyres (wheel node 0x006b7910, force 0x006b71f0, slip curve
//    0x006b7160): the slip between the wheel's surface and the ground, along
//    and across the wheel, each scaled and put through a curve rising to 1 at
//    peakAt and falling to minDynamicFriction, times the friction mods (the
//    "drive" mods for wheels behind the engine), the surface's friction and
//    the cube of the ground normal's height. The force is per tick: times 30
//    it is an acceleration, averaged over the wheels (0xa8).
//  - Wheel spin (0x006b73d0): the engine's torque (torque * throttle *
//    differential * gear ratio * 1.75) and the slope's (0x006b7460, not in
//    first gear) turn the wheel against friction, engine braking and brake
//    torques over twice wheelInertia; the tyre's force feeds back on the spin
//    (engineFeedbackMod), and the spin is limited to maxRpm.
//  - Gear ratios (0x00629610): a 101-entry table read at gear / gears,
//    linear between 1 at 0 and the five setGearRatios values at 0.2 ... 1;
//    reverse is gear 0 at 1.2 times first gear's ratio.
//
// What is provisional (not decompiled yet): steering speed (the
// RotationalBundle update), when the engine changes gears and engages the
// clutch (0x0064e920 is read only in part: no clutch, no gear-change time),
// when it brakes (here: throttle against the direction of travel), wheel
// spin of wheels without contact, and the wheels' radius (the depth of the
// wheel's vehicle collision triangle below its centre).
#pragma once

#include "Physics/RigidBody.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace dice::hfe::world {
class ObjectTemplate;
class ObjectTemplateManager;
} // namespace dice::hfe::world
namespace dice::hfe::physics {
struct CollisionMeshFile;
}

namespace dice::hfe::game {

using physics::Vec3;

// An Engine template's settings (template 0x520-0x7b0, defaults from its
// constructor 0x006291d0).
struct EngineSettings {
	int engineType = 0;
	int numberOfGears = 1;     // 0x520
	float differential = 10;   // 0x524
	float torque = 60;         // 0x528
	float gearUp = 0.7f;       // 0x52c: normalised rpm to change up at
	float gearDown = 0.3f;     // 0x530
	float gearChangeTime = 1;  // 0x534
	// The gear ratio table (0x538): 101 samples and one past the end.
	std::array<float, 102> gearTable;
	// newCar2.*
	float minRpm = 1000;                  // 0x760
	float maxRpm = 4000;                  // 0x764
	bool useClutchedTorque = false;       // 0x768
	float wheelInertia = 12.251251f;      // 0x76c
	float engineFeedbackMod = 0.2f;       // 0x770
	float brakeTorque = 2000;             // 0x774
	float engineBrakeTorque = 0;          // 0x778
	float frictionTorque = 100;           // 0x77c
	float frictionMod = 1;                // 0x780
	float slopeTorqueMod = 1;             // 0x784
	float wheelLongFrictionMod = 1;       // 0x788
	float wheelLongDriveFrictionMod = 1.5f; // 0x78c
	float wheelLongPeakAt = 0.1f;         // 0x790
	float wheelLongMinDynamicFriction = 0.7f; // 0x794
	float wheelLongScale = 0.1f;          // 0x798
	float friLatBrakeMod = 1;             // 0x79c
	float wheelLatFrictionMod = 2;        // 0x7a0
	float wheelLatDriveFrictionMod = 0.5f; // 0x7a4
	float wheelLatPeakAt = 0.1f;          // 0x7a8
	float wheelLatMinDynamicFriction = 0.7f; // 0x7ac
	float wheelLatScale = 0.1f;           // 0x7b0

	EngineSettings();
	static EngineSettings fromTemplate(const world::ObjectTemplate& engine);
	// setGearRatios (0x00629590): the ratios of gears at 0.2, 0.4 ... 1.
	void setGearRatios(const std::array<float, 5>& ratios);
	// Slot 0x1a0 for newCar2 (0x0064f360): gear 0 is reverse.
	float gearRatio(int gear) const;
};

// The tyre slip curve (0x006b7160): |x| / peakAt up to peakAt, then down
// in a straight line to minDynamic at 1 and no lower; signed like x.
float tyreCurve(float x, float peakAt, float minDynamic);

// One tick of wheel spin (0x006b73d0): `torque` over `inertia` speeds it up
// for `seconds`, then `resistance` slows it, not past 0.
float wheelSpinStep(float spin, float torque, float resistance, float inertia, float seconds);

// The tyre force (0x006b71f0): `slip` (wheel surface velocity less ground
// velocity) along `forward` and `right` through the slip curves. The result
// is a velocity change per tick; `longitudinal` and `lateral` receive the
// curve values times their mods.
struct TyreContact {
	float grip = 1;     // the surfaces' friction (0xe0)
	float normalY = 1;  // the ground normal's height (0xbc)
	bool drive = false; // behind the engine: the drive mods apply
	bool brakeLatMod = false; // braking: friLatBrakeMod across the wheel
};
Vec3 tyreForce(const EngineSettings& engine, const TyreContact& contact, const Vec3& slip, const Vec3& forward,
	const Vec3& right, float* longitudinal = nullptr, float* lateral = nullptr);

struct VehicleInput {
	float throttle = 0; // PIThrottle
	float steer = 0;    // PIYaw: right positive
};

class Vehicle {
public:
	struct Wheel {
		Vec3 mount;          // the wheel's centre at rest, object space
		float radius = 0.5f;
		float strength = 0;  // Spring template 0x2b4
		float damping = 0;   // 0x2b0
		float maxSteer = 0;  // degrees; 0 for wheels that do not steer
		float steerSpeed = 1; // degrees per tick (RotationalBundle maxSpeed)
		// State.
		float steer = 0;      // degrees
		float compression = 0; // how far the ground pushes the wheel up
		float spin = 0;       // rad/s (Spring 0x27c)
		float angle = 0;      // radians (0x280)
		bool contact = false;
		std::string spring;   // template names, for drawing
		std::string steering; // the RotationalBundle above it, if any
	};

	// The vehicle in a template tree with an Engine node whose children
	// include Springs; null otherwise. `mesh` gives the wheels' radius.
	static std::unique_ptr<Vehicle> fromTemplate(const world::ObjectTemplate& root,
		const world::ObjectTemplateManager& templates, const physics::CollisionMeshFile* mesh);

	const EngineSettings& engine() const { return m_engine; }
	const std::vector<Wheel>& wheels() const { return m_wheels; }
	float mass() const { return m_mass; }
	int gear() const { return m_gear; }
	float rpm() const { return m_rpm; } // normalised: 0 at minRpm, 1 at maxRpm
	bool braking() const { return m_braking; }

	void setInput(const VehicleInput& input) { m_input = input; }
	// One tick: suspension and tyre forces on `body`; call before it steps.
	void tick(physics::RigidBody& body, const physics::Ground& ground, float seconds, float grip,
		float gravity = physics::tuning::kGravity);

private:
	void updateEngine();
	void updateWheel(Wheel& wheel, physics::RigidBody& body, const physics::Ground& ground, float seconds,
		float grip, float gravity);

	EngineSettings m_engine;
	Vec3 m_engineOffset; // object space
	float m_mass = 1;    // slot 0x114
	std::vector<Wheel> m_wheels;
	VehicleInput m_input;
	int m_gear = 1;          // 0x280
	float m_rpm = 0;         // 0x25c
	float m_wheelRpmSum = 0; // 0x284
	int m_wheelRpmCount = 0; // 0x288
	bool m_braking = false;  // 0x274
};

} // namespace dice::hfe::game
