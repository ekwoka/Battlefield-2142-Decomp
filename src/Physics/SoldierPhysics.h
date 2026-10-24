// Soldier movement physics: the soldier's physics node (vtable 0x00993aa0)
// and its response, BF2/Physics/SoldierResponse.cpp (vtable 0x009930f8).
//
// The soldier does not turn with physics and has no inertia: the Soldier
// object turns it and asks for a velocity (response slot 0x84). Per tick:
//   ground contact (slot 0x28, 0x006c28c0): the first five points of the
//     built-in "Soldier_CollisionMesh" (0x006e4880: the feet at 1 m below the
//     origin and four at 0.6 m) against the heightmap; the first point
//     decides whether the soldier stands and on which slope;
//   objects (slot 9, 0x006c5140 -> 0x006c3890): a stack of spheres by
//     stance against their soldier collision LOD (static objects', and
//     moving ones' in their own frames), swept from the last tick's place
//     when it moved over 0.9 of a radius, contacts handled like the
//     ground's; other soldiers (0x006c2620: upright cylinders);
//   solve (slot 0x30, 0x006c48c0): push out of the ground and objects,
//     cancel the velocity into them; then up to eight passes of the
//     collisions at 0.999 of the size, each pushing out what still
//     overlaps; a soldier still stuck after eight is squashed (put back
//     where it was a tick ago, and killed when a heavy object did it);
//     after it, objects running it over (0x006c2e50) and the server's
//     segment test (the origin's way through a mesh kills);
//   movement (slot 0x34, 0x006c2230): on the ground, accelerate towards the
//     requested velocity along the ground, limited by friction and the slope;
//   integration (node slot 0xf4, 0x006ca660 / 0x006ca3b0) and gravity;
//     a jump (the Soldier's 0x0058a6d0) sets the velocity before it;
//   water (in 0x006c28c0): buoyancy under the feet's depth, swimming.
// Ladders are not the response's (a LadderContainer the soldier sits in,
// see GameWorld's Ladders.cpp).
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
// The solve (0x006c48c0; static_init 0x008ede60..0x008ee020).
constexpr int kSolvePasses = 8;           // *0x00a84738
constexpr int kNormalPasses = 3;          // *0x00a847a0: the passes that push along the contacts' normals
constexpr float kSolveScale = 0.999f;     // *0x00a84abc: the points and spheres' size in the passes
constexpr float kSolvePush = 2.0f;        // *0x00a84794: a pass's depth x 2 / 0.999
constexpr float kCancelScale = 1.0f;      // *0x00a8478c
constexpr float kHeavyMass = 100;         // an object over this mass (slot 0x114) crushes and runs over
constexpr float kCrushSpeed = 3.0f;       // *0x00a84748: m/s relative to the soldier
constexpr float kSquashDepth = 0.05f;     // *0x00a84754: how deep the crush has to be
constexpr float kPushScale = 2.5f;        // *0x00a847b4: the hit's velocity kept for the ragdoll (0x005ee2b0)
constexpr float kCrushDamage = 1000;      // the game logic's damage (slot 0xe4) for a crush or a run-over
// The stance's height (0x006c1b30 / 0x006c2620): standing the template's
// 1.7, crouching or swimming 1.4 (*0x00a84798), prone 0.8 (*0x00a84778).
float stanceHeight(int stance);
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
		float mass = 0;      // node slot 0x114
		bool asleep = false; // node slot 0x14c
		// collisionGroups (response slot 0x50): the run-over check
		// (0x006c2e50) passes one with bit 0x10, or with its team's bit
		// (0x100 team 1, 0x200 team 2) for the soldier's team.
		uint32_t collisionGroups = 0;
		// A projectile with noInitialCollisionWithLauncher (template +0x355,
		// read by slot 0x114 = 0x005b37d0) ignores its launcher's root
		// (0x005b48b0 -> the mesh response's 0x006b8de0: the response's +0xfc
		// and, on the server, +0xf8 = the tick + 2, *0x00a84284; on a client
		// +0xf8 = -1, for good). `ignoring`: that window is open (0x006b6c30);
		// `ignoresThis`: and it is this soldier (0x006b8da0).
		bool ignoring = false;
		bool ignoresThis = false;
		// A GenericProjectile (class 0x9539) whose soldier LOD's radius (the
		// mesh instance's slot 0x50(2), 0x006de7d0) is under 0.6 m
		// (*0x00a84acc): its contacts don't hold the soldier, they push it
		// (its response's slot 0x2c), unless it is the soldier's own
		// (*0x00a84774 = 1: the projectile interface 0x94c1's slot 0x48).
		bool pushed = false;
		bool own = false;
		Vec3 pointVelocity(const Vec3& p) const { return velocity + cross(angularVelocity, p - centerOfMass); }
	};
	std::vector<Mover> movers;
	// Other soldiers near it (on foot): their origin and stance (0x006c2620).
	struct Neighbour {
		Vec3 position;
		int stance = 0;
	};
	std::vector<Neighbour> soldiers;
	// The hit's velocity a ragdoll starts with when the soldier dies within
	// 16 ticks (0x005ee2b0, read by 0x005f0a00): a moving object's point
	// velocity x 2.5 at a contact faster than 1 m/s (none for a static
	// one: zero), or a crushing object's x 2.5.
	std::function<void(const Vec3&)> onPush;
	// 1000 damage through the game logic (slot 0xe4): squashed by a heavy
	// object (its owner), run over (0x006c2e50; its owner) or through a
	// mesh by the server's segment test (no one: 0).
	std::function<void(uint32_t attacker)> onCrushed;
	bool server = true; // the segment test (game logic slot 0x144)
	int team = 0;       // the run-over check's filter (interface 0xc4c5 slot 0x9c)
	// 0x006c3890 with a mover's contacts that its response ignores (the
	// main pass, 0x006b8da0): the response's window is opened again
	// (0x006b8de0: the tick + 2 on the server).
	std::function<void(uint32_t owner)> onIgnored;
	// A small projectile pushed (`pushed` above): its response's slot 0x2c
	// with the contact's point, the projectile's velocity relative to the
	// soldier (the soldier's relative velocity turned round: none in the
	// solve's passes), the soldier's normal and the depth turned round
	// (positive: the push goes against the normal, away from the soldier),
	// the soldier's material and the mesh's.
	std::function<void(uint32_t owner, const Vec3& point, const Vec3& velocity, const Vec3& normal, float depth,
		uint32_t ownMaterial, uint32_t otherMaterial)>
		onPushObject;
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
	// A jump (0x0058a6d0: node slots 0xc8, 0x78, 0x5c): no impulse, and the
	// velocity and the acceleration both `velocity` for the next
	// integration (the original's object update runs before the physics'
	// integration; this one's step collides first, so the jump waits for
	// its integration instead of being cancelled by the ground it leaves).
	void jump(const Vec3& velocity)
	{
		m_jump = velocity;
		m_jumping = true;
	}
	// Whether the last step's solve gave up (eight passes) and put the
	// soldier back (the response's +0xf9 then set).
	bool squashed() const { return m_squashed; }

	void step(float seconds, const Ground& ground, float gravity = tuning::kGravity);

private:
	// 0x006c28c0 at a scale (1 on slot 0x28; 0.999 in the solve's passes):
	// the deepest point's depth (0 or less).
	float collideWithGround(const Ground& ground, float gravity, float scale = 1);
	void collideWithObjects(float seconds);                      // 0x006c3890 per object
	// 0x006c3890: false when a collision event vetoed; `deepest` the
	// deepest contact (0 or less). `normals`: the contacts push along their
	// own normals (slot 9 and the first three passes), else back along the
	// way the soldier came.
	bool collideWithMesh(const CollisionWorld& mesh, const Mover* mover, float seconds, float scale, bool normals,
		float& deepest);
	void collideWithSoldiers(float scale);        // 0x006c2620
	void addContact(const Vec3& normal, float depth, const Vec3& velocity); // 0x006c24b0
	void solve(float seconds, const Ground& ground, float gravity); // 0x006c48c0
	void runOver();                               // 0x006c2e50
	void segmentTest(float seconds);              // 0x006c48c0's server part
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
	Vec3 m_jump;
	bool m_jumping = false;
	bool m_squashed = false;
};

} // namespace dice::hfe::physics
