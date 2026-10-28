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
//  - Wheel spin (0x006b73d0): the engine's torque (the torque curve at the rpm * throttle *
//    differential * gear ratio * 1.75) and the slope's (0x006b7460, not in
//    first gear) turn the wheel against friction, engine braking and brake
//    torques over twice wheelInertia; the tyre's force feeds back on the spin
//    (engineFeedbackMod), and the spin is limited to maxRpm.
//  - Gear ratios (0x00629610): a 101-entry table read at gear / gears,
//    linear between 1 at 0 and the five setGearRatios values at 0.2 ... 1;
//    reverse is gear 0 at 1.2 times first gear's ratio.
//
//  - Steering: the RotationalBundle's yaw axis (0x00563bd0, see
//    rotationAxisStep), driven by PIYaw.
//
//  - The engine (0x0064e920 with 0x0064e810 and 0x0064e6f0, see
//    Vehicle::updateEngine): braking, the clutch, gear changes.
//
//  - A wheel off the ground: no separate branch (the wheel node 0x006b8230
//    hands newCar2 wheels to 0x006b7910 before its contact test, the
//    contact sums at zero); the Spring node's spin factor is 1.0 for
//    newCar2 (*0x00a84c20). See engine.md, "Wheels off the ground".
//  - The radii: the spin's (wheelSpinRadius, the Spring node's 0x006cad60:
//    the height of the wheel's LOD-0 box) and the ground's, how far the
//    wheel's vehicle-LOD vertices (three at one height) lie below its
//    centre: the h of the vertical probe (0x006bb050, h = |v0.y|) and the
//    depth of the points the ground contact (slot 0x28) tests.
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
	// The torque curve (0x374): 101 samples and one past the end.
	std::array<float, 102> torqueCurve;
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
	// The torque at an rpm (0x00629660): the curve at |rpm| (at most 1) x torque.
	float torqueAt(float rpm) const;
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

// One axis of a RotationalBundle (template 0x298 minRotation, 0x2a4
// maxRotation, 0x2bc maxSpeed, 0x2d4 acceleration, 0x2e0 deAcceleration,
// 0x2ec continousRotationSpeed, 0x304 automaticReset, 0x305
// snapToZeroOnNoInput, 0x306 useDeAcceleration, 0x360 capMaxSpeed, 0x364
// maxSpeedMod). The values are per second as written: the getters
// (0x006147c0, 0x00613ca0, 0x00613d00) would scale them by 30 only while
// 0x00a73a1c is 0, and it is set to 1 (0x008e91a0).
struct RotationAxis {
	float minRotation = 0, maxRotation = 0; // degrees
	float maxSpeed = 0;     // degrees a second
	float acceleration = 0; // degrees a second, a second (with automaticReset: degrees a second)
	float deAcceleration = 0;
	float continuousSpeed = 0;
	bool automaticReset = false;
	bool snapToZero = false;
	bool useDeAcceleration = false;
	bool capMaxSpeed = false;
	float maxSpeedMod = 0;
};
// The axis update (0x00563bd0) on the branch the debug variable
// rot-use-old selects, which is 1 (0x008de9e0): the input times maxSpeed
// (maxRotation with automaticReset; negated when acceleration is
// negative, 0 when it is 0) is a target, capped by capMaxSpeed to maxSpeed
// x 30 (rotationalbundle-max-speed-mod) and by maxSpeedMod to maxSpeed x
// it. With automaticReset the angle itself moves to the target at
// |acceleration| a second; otherwise the speed does, and the angle moves
// by it. The angle then stays within the rotation limits (an unlimited
// axis wraps within a turn). snapToZeroOnNoInput first puts the angle and
// speed to 0 when the input is under 0.05 towards the angle's side.
void rotationAxisStep(const RotationAxis& axis, float input, float seconds, float& angle, float& speed);

struct VehicleInput {
	float throttle = 0;  // PIThrottle
	float steer = 0;     // PIYaw: right positive
	float pitch = 0;     // PIPitch (the mouse's y in the land and air control maps)
	float roll = 0;      // PIRoll (the mouse's x)
	float crouch = 0;    // PICrouch (a walker's)
	bool sprint = false; // PISprint
};

// The player inputs a template names (inputToYaw PIThrottle, ...): an index
// into VehicleInput, -1 for none (or one the vehicle doesn't read).
int vehicleInputIndex(const std::string& name);
float vehicleInputValue(const VehicleInput& input, int index);

// The lift of a wing (0x006cb600): the air's velocity `v` against the
// wing's normal `n`, a curve of the angle peaking at 22.5 degrees, times
// |v|^2, `scale` and 0.0025.
float wingLift(const Vec3& v, const Vec3& n, float scale);
// The air's density at a height (Engine::getAirDensity 0x0064d3c0): 1 at
// 0 falling to 0 at Physics.airDensityZeroAtHeight (1000).
float airDensity(float height);

class Vehicle {
public:
	struct Wheel {
		Vec3 mount;          // the wheel's centre at rest, object space
		float radius = 0.5f;     // how far its vehicle-LOD vertices lie below the centre (the probes' h)
		int collisionPart = -1;  // its geometry in the vehicle's collision mesh (the template's collisionPart, +0x74: 0x0063ccc0 -> 0x006d8180 picks the geometry; the instance's +0x14 is the state, sub-geometry 0)
		float spinRadius = 0.5f; // the engine's (0x006cad60): its LOD 0 box's height, at least 0.1
		float strength = 0;  // Spring template 0x2b4
		float damping = 0;   // 0x2b0
		float maxSteer = 0;  // degrees; 0 for wheels that do not steer
		RotationAxis steerAxis; // the yaw of the PIYaw RotationalBundle above it
		// State.
		float steer = 0;      // degrees (the bundle's rotation, 0x188)
		float steerRate = 0;  // degrees a second (0x194)
		float compression = 0; // how far the ground pushes the wheel up
		bool firstStep = true; // the Spring node's +0x284: no displacement rate on its first move
		float spin = 0;       // rad/s (Spring 0x27c)
		float angle = 0;      // radians (0x280)
		bool contact = false;
		std::string spring;   // template names, for drawing
		std::string steering; // the RotationalBundle above it, if any
		// What an object's mesh gave the wheel's own response last tick (its
		// vertical probes in 0x006bb050 add to the wheel's response, slot
		// 0x2c, as the heightmap's points do): the highest crossing, the
		// mean normal, the mean velocity of what it touched and that
		// surface's grip. The spring and the tyre read it as the ground
		// when it is above the terrain.
		struct ObjectGround {
			int count = 0;
			float height = 0;
			Vec3 normal;
			Vec3 velocity;
			float grip = 0.8f;
		};
		ObjectGround objectGround;
		// The other engines' wheels (0x006b8230's old path): driven by a
		// newCar2 engine above it, else by an old one (`engine`, an index
		// into engines(), -1 for none: the hover tanks' antigravs).
		bool newCar2 = true;
		int engine = -1;
		int part = -1;          // its parent part (whose forward is the wheel's)
		int gripFlags = 2;      // the template's grip (+0xfc): 8 EngineGrip, 0x80 EngineDummyGrip, ...
		bool sticky = true;     // the response's static friction bit (+0xec & 0x40)
		float surfaceSpeed = 0; // the track's speed along it (Spring +0x27c)
		// Its part's material (the vehicle LOD's, through the template's
		// mapMaterial) and the grip against the ground under it this tick
		// (the material pair's friction, -1: the vehicle's), set by the world.
		uint32_t material = 0;
		float grip = -1;
	};

	// A node of the template tree that turns (a RotationalBundle, an Engine,
	// a Wing, ...): its three axes (yaw, pitch, roll: the x, y and z of the
	// template's vectors) driven by the inputs it names (0x00563bd0,
	// rotationAxisStep), placed in its parent by its offset and rotation;
	// its frame is R(angles) x base x the parent's (the bundle's matrix,
	// 0x00565a20).
	struct Part {
		int parent = -1;
		std::string name, type;
		Vec3 offset;
		physics::Mat3 base;
		std::array<RotationAxis, 3> axes;
		std::array<int, 3> inputs {-1, -1, -1};
		std::array<float, 3> angle {}, rate {}, excess {};
		bool rememberExcess = false; // 0x30a: input over 1 is kept for later ticks (+-40)
		bool canReverse = true;      // an Engine's 0x744: an input against the axis's acceleration is 0
		bool moves = false;          // an axis with a speed
		// This tick, in object space.
		physics::Mat3 rotation;
		Vec3 position;
	};

	// An Engine of another type than newCar2 (the old Engine object, vtable
	// 0x00987e30: handleUpdate 0x0064d4f0, updatePhysics 0x0064df20).
	struct OldEngine {
		int part = -1;
		int flags = 0;      // engineType's bits: 1 propeller, 2 car, 4 tank, 8 ship, 0x10 rocket, 0x20 helicopter
		EngineSettings settings;
		float noPropellerEffectAtSpeed = 100; // 0x6e4
		float heatIncrement = -1, coolingFactor = 0; // 0x6ec, 0x6f0
		float horizontalSpeedMagnifier = 1;   // 0x704
		float maxAngleOfAttack = 20, defaultAngleOfAttack = 0; // 0x708, 0x70c
		bool pureRotational = false;          // 0x701
		float attackSpeed = 0;                // 0x710
		bool decreaseAngleToZero = false;     // 0x718
		float spinWhenNoEngineInAir = 0;      // 0x71c
		float horizontalDampAngle = -1, horizontalDampAngleFactor = 1; // 0x724, 0x728
		float dampHorizontalVel = 0, dampHorizontalVelFactor = 0;      // 0x72c, 0x730
		float decreaseAngleToZeroVerticalVel = 0; // 0x734
		float airFlowAffect = 0;              // 0x73c
		float noEffectAtPerpSpeed = 10000;    // 0x740
		float trackTurnAcceleration = 120, trackTurnSpeed = 1.5f; // 0x758, 0x75c
		bool noPowerWhenUpsideDown = false;   // 0x7d4
		// State.
		bool running = false; // +0x24c
		float stopTimer = -1; // +0x28c
		float rpm = 0;        // +0x25c
		float load = 0;       // +0x260, the feedback this tick
		int loadCount = 0;    // +0x268
		float aoa = 0;        // +0x270, radians
		float heat = 0;       // +0x250
		int gear = 1;         // +0x280
		bool braking = false; // +0x274
		float yaw = 0;        // +0x26c: the yaw axis over its maximum
		float trackFriction = 0; // +0x2a4
		int trackCount = 0;      // +0x2a8
		int trackSides = 0;      // +0x2ac
		float gearRatio() const;  // slot 0x1a0 (0x0064c6e0): 4, 2, 1.5, 1.25, 1
		float currentRatio() const;  // slot 0x1b8 (0x0064d3f0): differential x 3.5 / the table's ratio
		float currentTorque() const; // slot 0x1a4 (0x00629660): the curve at |rpm| x torque
		void feedback(float push);   // feedbackLoop 0x0064dd10 with the push along the axis
	};
	struct WingPart {
		int part = -1;
		float wingLift = 0, flapLift = 0, pitchOffset = 0; // 0x36c, 0x370, 0x374
		Vec3 positionOffset;                                // 0x378
		float startDelay = 0;                               // 0x390
		float multiply = 1, offsetY = 0;                    // the bundle's 0x340, 0x338
		float waited = 0;
	};
	// A ForceObject child (object 0x0062cd10, tick 0x0062c920, regulation
	// 0x0062b750): with keepUp, a couple holding the vehicle upright.
	struct ForcePart {
		int part = -1;
		bool keepUp = false;
		std::array<float, 3> a {}, b {}; // regulatePosX/Y/Z
		std::array<bool, 3> regulated {};
		bool onlyWhenOccupied = false, stopWhenUnoccupied = false;
		float startDelay = 0, waited = 0;
		int input = -1;
		float force = 0, constantForce = 0, maxForce = 0;
	};
	// MechLegs (the object's update 0x00650e90 with its input handler
	// 0x00650b20, and the MechLegs response 0xc432, vtable 0x009944b8).
	struct Legs {
		int part = -1;
		float length = 2.7f, stepHeight = 0.5f, radius = 1.5f;
		// State.
		int state = 0, stateTick = -100;
		float throttle = 0, yaw = 0, crouch = 0;
		float speed = 0;
		Vec3 desired;
		int footLeft = 0, footRight = 0;
		int pushTick = 0;
		float push = 0;
		bool grounded = false;
		Vec3 bestNormal;
		bool landed = false;
		// The drive's contact normal (+0x1b8, (0, 1, 0) after each drive),
		// which the legs' object pass (0x006cec60) adds its contacts into
		// per axis (0x006b6bd0); the heightmap's contact sets it.
		Vec3 contactNormal {0, 1, 0};
		bool objectNormal = false; // an object contact combined into it this tick
		// The walker's animation system (the root's slot 0x180): whether a
		// playing bundle blocks input (slot 0x50, 0x008b9e00: blockInput),
		// which holds the state (0x00650680), and its speed factor (slot
		// 0x54, 0x008ba420), which scales the drive (0x00650e90). Set by
		// the game before each tick; 1 and no block without a system.
		bool animationBlocks = false;
		float speedFactor = 1;
		// What the state gives the system (0x00650530, each tick): 0x80
		// walking, 0x100 backing, 0x400 turning left, 0x200 right (with
		// the input's sign holding, 0x00650600), and the PCO's crouch flag
		// (interface 0xc4c5 slot 0x134, set in state 5), which 0x0063ce30
		// passes on as 0x10.
		uint32_t messages = 0;
		bool crouching = false;
	};
	// A FloatingBundle (the *_NP APCs' floats; object vtable 0x00988240,
	// update 0x0064f420; its own node PhysicsNode 0x006ca810, vtable
	// 0x00993c98, made by the template's slot 29 0x00629a90 whatever
	// hasMobilePhysics says; tick 0x006ca860).
	struct FloaterPart {
		int part = -1;
		float hullHeight = 1;   // template 0x370
		float maxLift = 10;     // 0x374 (floatMaxLift)
		float minLift = 10;     // 0x378 (floatMinLift)
		float input = 0;        // the node's +0x260: minus the bundle's pitch (0x0064f420)
		float offset = 0;       // +0x264 (grows by +0x268, which nothing sets)
	};
	// A LandingGear (object vtable 0x00988768, update 0x00650120; template
	// defaults 0x0062a3xx: down 30 m / 60 m/s, up 20 m / 70 m/s, no delays).
	// Its axes run to their limits on an input of +-1: up once it is
	// gearUpHeight over what is below and at gearUpSpeed, down under
	// gearDownHeight and gearDownSpeed, each after its delay.
	struct GearPart {
		int part = -1;
		float downHeight = 30, upHeight = 20;  // 0x36c, 0x370
		float downSpeed = 60, upSpeed = 70;    // 0x374, 0x378
		float upDelay = 0, downDelay = 0;      // 0x37c, 0x380
		bool invert = false;                   // 0x384 (invertGear)
		// State.
		bool up = false;          // +0x250
		float upTimer = 0;        // +0x248
		float downTimer = 0;      // +0x24c
		int lastTick = -100;      // +0x260
	};
	struct Regulator { // the PCO's input regulator (0x005c9ec0)
		int input = -1;
		float a = 0, b = 0;
	};

	// The vehicle in a template tree with an Engine node whose children
	// include Springs; null otherwise. `mesh` gives the wheels' radius.
	static std::unique_ptr<Vehicle> fromTemplate(const world::ObjectTemplate& root,
		const world::ObjectTemplateManager& templates, const physics::CollisionMeshFile* mesh);

	const EngineSettings& engine() const { return m_engine; }
	bool hasNewCar2() const { return m_newCar2; }
	const std::vector<Part>& parts() const { return m_parts; }
	const std::vector<OldEngine>& engines() const { return m_engines; }
	const std::vector<WingPart>& wings() const { return m_wings; }
	const std::vector<FloaterPart>& floaters() const { return m_floaters; }
	const std::vector<GearPart>& gears() const { return m_gears; }
	const Legs* legs() const { return m_legs.part >= 0 || m_hasLegs ? &m_legs : nullptr; }
	// The walker's animation system's state for the next tick (see Legs).
	void setLegsAnimation(bool blocks, float speedFactor)
	{
		m_legs.animationBlocks = blocks;
		m_legs.speedFactor = speedFactor;
	}
	// A contact of the legs' sphere with another object (0x006cec60): its
	// normal into the drive's (per axis, 0x006b6bd0) and, below the legs'
	// half-radius line, the best ground normal by height (+0x1c4).
	void legsObjectContact(const Vec3& normal, const Vec3& queryNormal, bool below);
	// The legs' drive (0x006cdca0), after every contact of the tick.
	void legsDrive(physics::RigidBody& body);
	// The legs' frame in the world (the MechLegs node's matrix); false
	// without legs.
	bool legsFrame(const physics::RigidBody& body, physics::Mat3& frame, Vec3& origin) const
	{
		if (!m_hasLegs) {
			return false;
		}
		frame = partWorld(body, m_legs.part);
		origin = partPoint(body, m_legs.part);
		return true;
	}
	// Whether its engines push it through the air (a propeller engine).
	bool flies() const;
	// Someone aboard (the engines' messages 4 and 5: 0x0064c890; the force
	// only with the PCO occupied, 0x005c87b0) and the tick (for the legs'
	// latched inputs).
	void setOccupied(bool occupied) { m_occupied = occupied; }
	// A walker's response (MechLegs 0xc432) in place of the hull's ground
	// points: its legs against the ground (0x006cf5c0), the solve
	// (0x006ce740) and the drive (0x006cdca0). Call after the body steps.
	void legsRespond(physics::RigidBody& body, const physics::Ground& ground, float seconds,
		float gravity = physics::tuning::kGravity);
	const std::vector<Wheel>& wheels() const { return m_wheels; }
	float mass() const { return m_mass; }
	int gear() const { return m_gear; }
	// normalised: 0 at minRpm, 1 at maxRpm; the first old engine's for the others
	float rpm() const { return m_newCar2 || m_engines.empty() ? m_rpm : m_engines.front().rpm; }
	bool braking() const { return m_braking; }
	float clutch() const { return m_clutch; }

	void setInput(const VehicleInput& input) { m_input = input; }
	// A probe of wheel `index` met an object's face at `height` (see
	// Wheel::objectGround); taken by the next tick.
	void touchObject(size_t index, float height, const Vec3& normal, const Vec3& velocity, float grip);
	void setWheelMaterial(size_t index, uint32_t material)
	{
		if (index < m_wheels.size()) {
			m_wheels[index].material = material;
		}
	}
	void setWheelGrip(size_t index, float grip)
	{
		if (index < m_wheels.size()) {
			m_wheels[index].grip = grip;
		}
	}
	const VehicleInput& input() const { return m_input; }
	// One tick: suspension and tyre forces on `body`; call before it steps.
	void tick(physics::RigidBody& body, const physics::Ground& ground, float seconds, float grip,
		float gravity = physics::tuning::kGravity);

private:
	void updateParts(float seconds);
	void updateOldEngine(OldEngine& engine, physics::RigidBody& body, const physics::Ground& ground, float seconds,
		float gravity);
	void engineForce(OldEngine& engine, physics::RigidBody& body, const physics::Ground& ground, float gravity);
	void updateWing(WingPart& wing, physics::RigidBody& body, const physics::Ground& ground, float seconds, float gravity);
	void updateForce(ForcePart& force, physics::RigidBody& body, float seconds);
	void updateGear(GearPart& gear, const physics::RigidBody& body, const physics::Ground& ground, float seconds);
	void updateFloater(FloaterPart& floater, physics::RigidBody& body, const physics::Ground& ground, float seconds,
		float gravity);
	void updateLegs(physics::RigidBody& body, const physics::Ground& ground, float seconds);
	void updateOldWheel(Wheel& wheel, physics::RigidBody& body, const physics::Ground& ground, float seconds, float grip,
		float gravity);
	void regulateInput(const physics::RigidBody& body, float gravity);
	float trackSpeed(const OldEngine& engine, float side, const physics::RigidBody& body) const;
	physics::Mat3 partWorld(const physics::RigidBody& body, int part) const;
	Vec3 partPoint(const physics::RigidBody& body, int part) const;
	void updateEngine(const physics::RigidBody& body, float seconds);
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
	float m_clutch = 0;      // 0x27c: 0 at the start, engaged at 1
	int m_shift = 0;         // 0x2a4: a gear change under way, -1 or +1
	// The other models (VehicleParts.cpp).
	bool m_newCar2 = false;
	std::vector<Part> m_parts;
	std::vector<OldEngine> m_engines;
	std::vector<WingPart> m_wings;
	std::vector<ForcePart> m_forces;
	std::vector<FloaterPart> m_floaters;
	std::vector<GearPart> m_gears;
	float m_drag = 0;          // the root node's +0x50 (slot 0xfc): the template's drag
	float m_footprint = 0;     // the root's vehicle-LOD box, size x times size z (+0x24 x +0x2c)
	Legs m_legs;
	bool m_hasLegs = false;
	Regulator m_vertical; // regulateVerticalPos (0x390) on its input (0x374)
	bool m_occupied = false;
	float m_sprintFactor = 0; // the PCO template's (0x464)
	int m_tick = 0;
	VehicleInput m_regulated; // the inputs after the PCO's regulator
};

} // namespace dice::hfe::game
