// Rigid-body physics of mobile objects: dice.hfe.world.PhysicsNode.Basic
// (vtable 0x009938a0) and its collision response,
// dice.hfe.world.ResponsePhysics.Basic (vtable 0x00992cb8).
//
// A tick (PhysicsNode slot 0xf4, 0x006c9060): the angular step
// (0x006c8690), then the linear step (0x006c8380), then gravity is added to
// the next tick's acceleration. Velocities change by impulses that are
// velocity changes (contacts do not divide by the mass); turning divides by
// a box inertia from the collision bounds, scaled by inertiaModifier.
//
// Ground contact (ResponsePhysics slot 0x28, 0x006b9260) tests the points
// of the object's vehicle collision LOD against the heightmap and records a
// contact per point below it (slot 0x2c, 0x006ba4c0); the end-of-tick
// solve (slot 0x30, 0x006ba7d0) pushes the body out of the ground and
// applies an impulse per contact.
//
// Constants without names in the original (static data at 0x00a84b54...,
// 0x00a83ea0...) are given names here.
//
// Not rewritten yet: drag and buoyancy (dragModifier, floaterMod), sleeping,
// collisions between objects and the other physics node types (soldier,
// point, rotational point). Engines and wheels are in Game/Vehicle.
#pragma once

#include <array>
#include <functional>
#include <vector>

namespace dice::hfe::physics {

struct Vec3 {
	float x = 0, y = 0, z = 0;

	Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
	Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
	Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
	Vec3 operator-() const { return {-x, -y, -z}; }
	Vec3& operator+=(const Vec3& o) { return *this = *this + o; }
};

float dot(const Vec3& a, const Vec3& b);
Vec3 cross(const Vec3& a, const Vec3& b);
float lengthSquared(const Vec3& v);

// Rotation as rows (the engine's matrices act on row vectors: v * M).
struct Mat3 {
	std::array<Vec3, 3> rows {Vec3 {1, 0, 0}, Vec3 {0, 1, 0}, Vec3 {0, 0, 1}};

	// v * M (0x004b9d60).
	Vec3 transform(const Vec3& v) const;
	// v * M^T: back into the rotated space (rotations are orthonormal).
	Vec3 untransform(const Vec3& v) const { return {dot(v, rows[0]), dot(v, rows[1]), dot(v, rows[2])}; }
	static Mat3 axisAngle(const Vec3& unitAxis, float radians);
	// The engine's yaw/pitch/roll in degrees: roll about z, then pitch about
	// x, then yaw about y.
	static Mat3 fromYawPitchRoll(const Vec3& degrees);
	Vec3 yawPitchRoll() const;
	Mat3 operator*(const Mat3& o) const;
};

namespace tuning {
constexpr float kGravity = -14.73f;             // PhysicsSystem default (0x006da810), "Physics.gravity"
constexpr float kLinearDamping = 0.99f;         // per tick (0x00a84b54)
constexpr float kAngularDamping = 0.99f;        // per tick (0x00a84b58)
constexpr float kInertiaScale = 1.5f;           // 0x00a84b70
constexpr float kVerticalImpulseScale = 0.5f;   // contact impulses' y (0x00a84b74)
constexpr float kMaxAcceleration = 10000;       // 0x00a84b9c, 0x00a84ba0
constexpr float kMaxSpeed = 1500;               // 0x00a84ae8
constexpr float kMaxContactImpulse = 1000;      // 0x00a83ea4
constexpr float kFrictionScale = 1;             // 0x00a83eb8
constexpr float kElasticityScale = 1;           // 0x00a83ebc
} // namespace tuning

// The material properties a contact uses (MaterialManager slots 0x80-0x88;
// the material table is not rewritten yet).
struct ContactMaterial {
	float friction = 0.8f;
	float elasticity = 0.0f;
};

class RigidBody {
public:
	Vec3 position;
	Mat3 rotation;
	Vec3 centerOfMass;      // object space (centerOfMassOffset)
	Vec3 inertiaModifier {1, 1, 1};
	Vec3 inverseBoxInertia {1, 1, 1}; // per unit mass: 3 / (b^2 + c^2) from the box size (slot 0x188)
	float gravityModifier = 1;

	Vec3 velocity;        // 0x20
	Vec3 acceleration;    // 0x2c, cleared each tick (gravity, forces / mass)
	Vec3 impulse;         // 0x38, velocity change applied next tick
	Vec3 angularVelocity; // 0xbc
	Vec3 torque;          // 0xc8, angular acceleration for the next tick
	Vec3 angularImpulse;  // 0xd4
	// Wheel friction: the mean of the tyre forces this tick (0xec, torque
	// 0xf8, count 0x11c), added to the acceleration and the torque.
	Vec3 frictionAcceleration;
	Vec3 frictionTorque;
	int frictionCount = 0;

	// Box inertia from the size (max - min) of the collision bounds
	// (0x006aeda0 keeps the size at bounds + 0x24).
	void setBounds(const Vec3& size);

	// Slot 0x60 / 0x70: immediate velocity changes.
	void addVelocity(const Vec3& change);
	void addAngularVelocity(const Vec3& change);
	// Slot 0xd4: an impulse (a velocity change) at a point given relative
	// to the position.
	void applyImpulse(const Vec3& offset, const Vec3& change);
	// Slot 0xa4 (0x006c7c00): a force (an acceleration: no mass division) at
	// a point given relative to the position, adding its torque about the
	// centre of mass. Springs push their parents with it.
	void addForce(const Vec3& offset, const Vec3& force);
	// Slot 0xa8 (0x006c7980): a tyre's force at a world point, averaged with
	// the others of this tick.
	void addFriction(const Vec3& worldPoint, const Vec3& force);
	// The velocity of a world point moving with the body (slot 0xac).
	Vec3 pointVelocity(const Vec3& worldPoint) const;
	Vec3 worldCenterOfMass() const { return position + rotation.transform(centerOfMass); }

	// One tick (slot 0xf4 without the sleep tracking).
	void step(float seconds, float gravity = tuning::kGravity);

private:
	void stepAngular(float seconds); // 0x006c8690
	void stepLinear(float seconds);  // 0x006c8380
};

// Collision response of one body (ResponsePhysics.Basic).
class ContactResponse {
public:
	struct Contact {
		Vec3 offset;       // from the body's position
		Vec3 normal;
		Vec3 awayVelocity; // minus the point's velocity
		float approachSpeed; // speed into the surface
		float elasticity;
		float friction;
	};

	// Slot 0x2c: a contact at `offset` (from the position) moving at
	// `pointVelocity`, `depth` (negative) into a surface with `normal`.
	void addContact(const Vec3& offset, const Vec3& pointVelocity, const Vec3& normal, float depth,
		const ContactMaterial& material);
	// Slot 0x30: push the body out and apply the contacts' impulses.
	void solve(RigidBody& body);

	const std::vector<Contact>& contacts() const { return m_contacts; }
	const Vec3& pushOut() const { return m_pushOut; }

private:
	Vec3 m_pushOut; // 0x94: the largest push per axis
	std::vector<Contact> m_contacts;
};

// The heightmap as the ground contact sees it (HeightmapCluster slots
// 0x68 and 0x80).
struct Ground {
	std::function<float(float x, float z)> height;
	std::function<float(float x, float z, Vec3& normal)> heightAndNormal;
};

// Slot 0x28: the body's contact points (object space) against the ground.
void collideWithGround(RigidBody& body, ContactResponse& response, const std::vector<Vec3>& points,
	const Ground& ground, const ContactMaterial& material);

} // namespace dice::hfe::physics
