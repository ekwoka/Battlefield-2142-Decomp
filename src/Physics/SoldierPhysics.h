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
//   static objects (0x006c3890): a stack of spheres against their soldier
//     collision LOD, contacts handled like the ground's.
// Not rewritten yet: collisions with moving objects, ladders, water and the
// material table (a default friction is used).
#pragma once

#include "CollisionWorld.h"
#include "RigidBody.h"

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

class SoldierBody {
public:
	Vec3 position;
	float yawDegrees = 0;
	float gravityModifier = 1;
	float friction = 1;   // of the ground material (MaterialManager slot 0x88)
	float waterDepth = 0; // 0 on land, 1 under water
	// Static objects the soldier collides with (their soldier collision LOD).
	const CollisionWorld* world = nullptr;

	Vec3 velocity;
	Vec3 acceleration;      // gravity for the next tick
	Vec3 impulse;           // velocity change for the next tick

	bool onGround() const { return m_onGround; }
	const Vec3& groundNormal() const { return m_groundNormal; }

	// Response slot 0x84: the velocity the soldier wants (along the ground).
	void setDesiredVelocity(const Vec3& velocity) { m_desired = velocity; }

	void step(float seconds, const Ground& ground, float gravity = tuning::kGravity);

private:
	void collideWithGround(const Ground& ground); // 0x006c28c0
	void collideWithObjects();                    // 0x006c3890
	void addContact(const Vec3& normal, float depth); // 0x006c24b0
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
};

} // namespace dice::hfe::physics
