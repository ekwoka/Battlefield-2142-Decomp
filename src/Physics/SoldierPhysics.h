// Soldier movement physics: the soldier's physics node (vtable 0x00993aa0)
// and its response, BF2/Physics/SoldierResponse.cpp (vtable 0x009930f8).
//
// The soldier does not turn with physics and has no inertia: the Soldier
// object turns it and asks for a velocity (response slot 0x84). Per tick:
//   ground contact (slot 0x28, 0x006c28c0): the first five points of the
//     built-in "Soldier_CollisionMesh" (0x006e4880: the feet at 1 m below the
//     origin and four at 0.6 m) against the heightmap; the first point
//     decides whether the soldier stands and on which slope;
//   solve (slot 0x30, 0x006c48c0): push out of the ground, cancel the
//     velocity into it;
//   movement (slot 0x34, 0x006c2230): on the ground, accelerate towards the
//     requested velocity along the ground, limited by friction and the slope;
//   integration (node slot 0xf4, 0x006ca660 / 0x006ca3b0) and gravity.
//   objects (0x006c3890): a stack of spheres by stance against their
//     soldier collision LOD (static objects', and moving ones' in their own
//     frames), swept from the last tick's place when it moved over 0.9 of a
//     radius, contacts handled like the ground's;
//   water (in 0x006c28c0): buoyancy under the feet's depth, swimming.
// Not rewritten yet: the solve's iterations (slot 12's eight passes at
// 0.999 of the radius, its squash damage and the server's segment test),
// soldiers against soldiers (0x006c2620), the push by a fast object
// (0x005ee2b0) and ladders' climbing (a LadderContainer the soldier sits
// in, see GameWorld).
#pragma once

#include "CollisionWorld.h"
#include "RigidBody.h"

#include <functional>
#include <vector>

namespace dice::hfe::physics {

namespace soldier {
constexpr float kLandDamping = 0.99f;  // 0x00a84bec
constexpr float kWaterDamping = 0.9f;  // 0x00a84bf0
constexpr float kStaticGrip = 1.5712f;  // per tick, times normal.y^5 * friction
constexpr float kSlidingGrip = 2.3568f; // grip before slipping starts
constexpr float kMaxInputAcceleration = 250; // squared limit 62500
constexpr float kRadius = 0.25f;           // 0x00a68a80
constexpr float kHeight = 1.7f;            // standing (0x00a68a7c)
constexpr float kFloorTolerance = -0.04f;  // contacts this far below the lowest sphere hold the soldier up (0x00a84698)
constexpr float kUpperContact = 0.5f;      // spacings above the lowest sphere where floors stop lifting (0x00a84718)
constexpr float kFlatNormal = 0.95f;       // normals steeper than this are floors or ceilings (0x00a8471c)
// The first points of the built-in soldier collision mesh (object space).
constexpr Vec3 kGroundPoints[5] = {{0, -1, 0}, {-0.2f, -0.6f, -0.2f}, {0.2f, -0.6f, -0.2f}, {0.2f, -0.6f, 0.2f},
	{-0.2f, -0.6f, 0.2f}};
} // namespace soldier

// Whether a soldier fits at `origin` (its physics origin, a metre over the
// feet) in a stance (0 stand, 1 crouch, 2 prone, 3 swim as crouch): the
// stack of 0x006c1b30 (five spheres of kRadius from the feet to 1.7 m,
// three to 1.4 m crouching (*0x00a84798), one at the feet prone (0.8 m,
// *0x00a84778); the lowest centre a radius over the feet) with none
// touching the mesh (the response's slot 0x98, 0x006c3340). A mesh in
// its own frame (a vehicle's) is tested with the centres taken into it.
bool soldierFits(const CollisionWorld& world, const Vec3& origin, int stance, const Mat3& rotation = {},
	const Vec3& position = {});

namespace soldier {
constexpr float kSweepFactor = 0.9f;      // *0x00a84760 and *0x00a8475c: sweep over 0.9 r a tick (6.75 m/s)
constexpr float kBuoyancyDepth = 0.3f;    // 0x00975eb8: the feet this deep before the water lifts
constexpr float kSwimOn = 0.99f;          // *0x00a847a4
constexpr float kSwimOff = 0.9f;          // *0x00a8476c
constexpr float kContactSpeed = 0.1f;     // the ground's collision events over |v|^2 0.1
} // namespace soldier

class SoldierBody {
public:
	Vec3 position;
	float yawDegrees = 0;
	float gravityModifier = 1;
	float friction = 1;   // of the ground material (MaterialManager slot 0x88)
	// How deep the feet are under the water (node +0x5c, slot 0x140 / 0x144;
	// 0 out of it), set by the ground pass.
	float waterDepth = 0;
	// The stance (0 stand, 1 crouch, 2 prone, 3 swim): the sphere stack.
	int stance = 0;
	uint32_t material = 0; // the soldier's own, for the collision events
	// Static objects the soldier collides with (their soldier collision LOD).
	const CollisionWorld* world = nullptr;
	// Moving objects near it: their soldier LOD in their own frame.
	struct Mover {
		const CollisionWorld* mesh = nullptr;
		Mat3 rotation;
		Vec3 position;
		Vec3 velocity, angularVelocity, centerOfMass; // centre of mass in world space
		uint32_t owner = 0;
		Vec3 pointVelocity(const Vec3& p) const { return velocity + cross(angularVelocity, p - centerOfMass); }
	};
	std::vector<Mover> movers;
	// The collision events (object slot 0xac on the soldier, from the ground
	// pass and 0x006c3890): the other (0: the ground), the soldier's
	// velocity relative to it, the contact's normal and point, the two
	// materials. False vetoes: the object loop stops.
	struct Contact {
		uint32_t owner = 0;
		Vec3 velocity;
		Vec3 normal;
		Vec3 point;
		uint32_t ownMaterial = 0, otherMaterial = 0;
	};
	std::function<bool(const Contact&)> onContact;
	bool swimming() const { return m_swim; }

	Vec3 velocity;
	Vec3 acceleration;      // gravity for the next tick
	Vec3 impulse;           // velocity change for the next tick

	bool onGround() const { return m_onGround; }
	const Vec3& groundNormal() const { return m_groundNormal; }

	// Response slot 0x84: the velocity the soldier wants (along the ground).
	void setDesiredVelocity(const Vec3& velocity) { m_desired = velocity; }

	void step(float seconds, const Ground& ground, float gravity = tuning::kGravity);

private:
	void collideWithGround(const Ground& ground, float gravity); // 0x006c28c0
	void collideWithObjects(float seconds);                      // 0x006c3890 per object
	bool collideWithMesh(const CollisionWorld& mesh, const Mover* mover, float seconds);
	void addContact(const Vec3& normal, float depth, const Vec3& velocity); // 0x006c24b0
	void solve();                                 // 0x006c48c0 without objects
	void move(float gravity);                     // 0x006c2230
	void integrate(float seconds);                // 0x006ca3b0

	Vec3 m_desired;
	// Contacts this tick (0x94, 0xa0, 0xb8, 0xd0).
	Vec3 m_pushOut;
	Vec3 m_cancel;
	Vec3 m_contactVelocity;
	int m_contacts = 0;
	bool m_onGround = false;
	bool m_slipping = false;
	Vec3 m_groundNormal {0, 1, 0};
	// The node's averaged input acceleration (0xc4, count 0xd0).
	Vec3 m_inputAcceleration;
	int m_inputs = 0;
	// The node's matrix before the last integration (slot 0x4c -> +0xd4):
	// the spheres' last place; where the last step left it (a position set
	// from outside since is a jump, not a motion).
	Vec3 m_lastPosition;
	Vec3 m_stepEnd;
	bool m_hasLast = false;
	bool m_swim = false; // response +0xfa
};

} // namespace dice::hfe::physics
