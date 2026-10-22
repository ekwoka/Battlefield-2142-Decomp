// Ragdolls: a dead soldier's body as particles held together by
// constraints (BF2/Animation/BoneAnimation/RagDoll.cpp, 0x008a7900-0x008ae4e0
// of BF2142.exe), which then poses the soldier's skeleton.
//
// The game builds one template (the RagDoll manager, a singleton at
// DAT_00b4cb94 made by 0x008adac0) with the ragDoll.* console methods of
// Objects/Soldiers/Common/Animations/ragDollInit.con: a particle on a
// handful of skeleton bones (hips, knees, feet, shoulders, elbows, wrists,
// head), distance, angle and dihedral constraints between them measured on
// the setup skeleton, capsules for collisions, and the rules that turn the
// particles back into bone matrices (toSkeleton, lockBone). The ragdoll_*
// variables (static initialisers at 0x008ffa00-0x008ffe60) tune it.
//
// A soldier's death (0x005f0a00) starts an instance (0x008adc00) from the
// skeleton's last pose and the body's velocity, pushed by the explosion
// that killed it, if any (0x005ee4d0 keeps it, addForce with bone -2 applies
// it). Each frame (0x008add10) runs fixed steps of ragdoll_dT: forces and a
// gravity that ramps in over ragdoll_gravityOverTime (0x008a8a10), a Verlet
// step averaged with the previous one (0x008a8860), then ragdoll_iterations
// passes of the constraints and collisions (0x008ac8f0). The body sleeps
// once its middle particle has moved less than ragdoll_sleepDelta a frame
// for ragdoll_sleep seconds. The pose (0x008acde0) is relative to the
// middle particle (the right hip), in world axes.
//
// Not rewritten yet: the network side (ragdoll_net_*: particles driven by
// the server, 0x008acce0) and the level of detail by distance to the
// camera (0x008ae120: dT * 1.5 past the second ragdoll, no leg capsules).
#pragma once

#include "AnimationFiles.h"
#include "Pose.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dice::hfe::anim {

using Vec = std::array<float, 3>;

// The ragdoll_* variables (their defaults; ragDollInit.con sets
// slowMotion 0.75 and gravityOverTime 0.1 with vars.set).
struct RagDollSettings {
	float dT = 0.011111111f;          // ragdoll_dT: one step
	float slowMotion = 1;             // ragdoll_slowMotion: time scale
	float gravityOverTime = 0.5f;     // ragdoll_gravityOverTime: gravity ramps in over it
	int iterations = 2;               // ragdoll_iterations: constraint passes per step
	float sleep = 2.5f;               // ragdoll_sleep: seconds still before sleeping
	float sleepDelta = 0.02f;         // ragdoll_sleepDelta: "still", metres a frame
	float maxInheritSpeed = 40;       // ragdoll-max-inherit-speed
	float expRotateForce = 13;        // ragdoll_expRotateForce
	float boneSize = 0.1f;            // ragdoll_boneSize: particle radius
	float kneeBoneSize = 0.05f;       // ragdoll_kneeBoneSize
	float bodyBoneSize = 0.12f;       // ragdoll_bodyBoneSize (hips, shoulders)
	float completeCollRadius = 1.5f;  // ragdoll_completeCollRadius
	float legBackAngle = -0.1f;       // ragdoll_legBackAngle
	float legForwardAngle = 10;       // ragdoll_legForwardAngle
	float legForwardFactor = 0;       // ragdoll_legForwardFactor
	float legPointForwardAngle = 0.5f;   // ragdoll_legPointForwardAngle
	float legPointForwardFactor = 0.01f; // ragdoll_legPointForwardFactor
	bool enableForce = true;          // ragdoll_enableForce
	bool useConstraints = true;       // ragdoll_useConstraints
	bool useAngularConstraints = true; // ragdoll_useAngularConstraints
	bool useLandCollision = true;     // ragdoll_useLandCollision
	bool useObjectCollision = true;   // ragdoll_useObjectCollision
	bool useCapsuleCollision = true;  // ragdoll_useCapsuleCollision
	float expForceMod = 1;            // rd_expForceMod: explosions' push on soldiers
	float dieForce = 0;               // rd_dieForce (unused by the game: "don't use random die force yet")
};

// The template the console methods build (the manager's half).
class RagDollTemplate {
public:
	RagDollTemplate();

	// ragDoll.gravity (manager 0x4, default -30).
	float gravity = -30;
	RagDollSettings settings;

	struct Particle {
		int bone;
		float mass;
		float radius;
	};
	struct Distance {          // addConstraint (0x70), addDistanceLessThanConstraint (0xac)
		int a, b;
		float length;
	};
	struct Angular {           // addAngularConstraint (0x94)
		int a, b, c;
		float ab, bc;
		float limit;           // distance a-c; negative: at most, else at least
	};
	struct Dihedral {          // addDihedralAngleConstraint (0xa0)
		int a, b, c, d;
		float ab, bc, cd;
		float limit;
	};
	struct Pair {              // addCapsuleCollision (0x7c), addJointConstraint (0x88)
		int a, b;
	};
	struct Link {              // toSkeleton (0xc): a bone from particles or bones
		int bone, a, b;
	};

	// ragDoll.skeleton: the setup skeleton the lengths are measured on.
	void setSkeleton(const Skeleton* skeleton);
	const Skeleton* skeleton() const { return m_skeleton; }
	float referenceDistance(int boneA, int boneB) const; // 0x008a7b70

	void addParticle(int bone, float mass);          // 0x008abe00
	void addConstraint(int boneA, int boneB);        // 0x008ab1c0
	void addDistanceLessThanConstraint(int boneA, int boneB, float distance); // 0x008ab140
	void addAngularConstraint(int a, int b, int c, float degrees);         // 0x008aae60
	void addDihedralAngleConstraint(int a, int b, int c, int d, float degrees); // 0x008aafb0
	void removeConstraint(int boneA, int boneB);     // 0x008a85e0
	void addCapsuleCollision(int boneA, int boneB);  // 0x008ab250
	void addJointConstraint(int boneA, int boneB);   // 0x008ab2d0
	void lockBone(int from, int to);                 // 0x008aadf0
	void toSkeleton(int bone, int a, int b);         // 0x008abbd0
	void setParticleCollisionCriteria(float minSpeed, float minDelay); // 0x008a79b0

	// One script line (ragDoll.<method> and vars.set ragdoll_* / rd_*);
	// false for lines it doesn't know.
	bool execute(const std::string& line, std::string* include = nullptr, std::string* skeletonPath = nullptr);

	int particleOf(int bone) const; // -1 for none (0xd4: 128 bones)
	bool locked(int bone) const;
	const std::vector<Particle>& particles() const { return m_particles; }
	const std::vector<Distance>& constraints() const { return m_constraints; }
	const std::vector<Distance>& lessThan() const { return m_lessThan; }
	const std::vector<Angular>& angular() const { return m_angular; }
	const std::vector<Dihedral>& dihedral() const { return m_dihedral; }
	const std::vector<Pair>& capsules() const { return m_capsules; }
	const std::vector<Link>& links() const { return m_links; }
	// The middle particle (the right hip's, bone 6: 0xec): where the body is.
	int midParticle() const { return particleOf(6); }

private:
	const Skeleton* m_skeleton = nullptr;
	std::vector<Vec> m_reference; // setup pose, node positions
	std::vector<Particle> m_particles;
	int m_particleOf[128];
	std::vector<Distance> m_constraints, m_lessThan;
	std::vector<Angular> m_angular;
	std::vector<Dihedral> m_dihedral;
	std::vector<Pair> m_capsules, m_joints;
	std::vector<Link> m_links;
	std::vector<bool> m_locked;
	float m_collisionMinSpeed = -1, m_collisionMinDelay = 0;

public:
	// setParticleCollisionCriteria (+0xbc, +0xc0): an impact sound needs a
	// particle hitting at minSpeed (metres a step; -1: none) and minDelay
	// seconds since the last.
	float collisionMinSpeed() const { return m_collisionMinSpeed; }
	float collisionMinDelay() const { return m_collisionMinDelay; }
};

// Loads a ragdoll script (ragDollInit.con and what it includes) through
// `read`, and its skeleton through `skeleton`.
bool loadRagDoll(RagDollTemplate& ragDoll, const std::string& path,
	const std::function<bool(const std::string& path, std::vector<uint8_t>& data)>& read,
	const std::function<const Skeleton*(const std::string& path)>& skeleton);

// What a ragdoll collides with, in world space: the terrain and the
// objects of its list (0x008ab6b0: those with collision whose soldier LOD
// comes within ragdoll_completeCollRadius of the middle particle, moving
// ones included).
struct RagDollWorld {
	// The terrain's height and normal under a point; empty for none.
	std::function<float(float x, float z, Vec& normal)> ground;
	// The first object, in list order, whose mesh the segment crosses
	// moving into a face (collision-mesh slot 22): the crossing, the face's
	// normal and material.
	std::function<bool(const Vec& from, const Vec& to, Vec& point, Vec& normal, uint32_t& material)> raycast;
	// A capsule against each object's mesh edges (slot 25): every contact's
	// normal and depth (negative: into it).
	std::function<void(const Vec& a, const Vec& b, float radius, std::vector<std::pair<Vec, float>>& contacts)> capsule;
	// Whether an object of the list near `middle` moves (its physics not
	// at rest and over 0.001 m/s: 0x008a79d0), which wakes a sleeping body.
	std::function<bool(const Vec& middle, float reach)> movingNear;
	// The velocity of the object 2 m or less straight under `from` (the ray
	// 0x006f2250; its physics node's point velocity), false for none.
	std::function<bool(const Vec& from, Vec& velocity)> carriedBy;
};

// One body (an instance, 0x68 bytes: 0x008abc00).
class RagDoll {
public:
	struct Particle {               // 0x58 bytes
		Vec position;               // 0x0
		Vec previous;               // 0xc
		Vec acceleration;           // 0x18
		Vec lastMove;               // 0x24: the previous step's displacement
		Vec force;                  // 0x3c
		int bone;                   // 0x48
		float mass;                 // 0x4c
		float radius;               // 0x50
		bool hitObject = false;     // 0x55
	};

	// 0x008adc00 / 0x008aca60: particles at the bones of `pose` (world
	// matrices of the skeleton's nodes, in world space), moving at
	// `velocity` (at most maxInheritSpeed).
	void start(const RagDollTemplate& ragDoll, const std::vector<Matrix>& pose, Vec velocity);
	bool started() const { return m_template != nullptr; }
	// 0x008aa1c0: a push on a bone's particle; bone < 0 pushes the whole
	// body, the legs one way and the rest along `force`, both times
	// ragdoll_expRotateForce, turning it over.
	void addForce(int bone, const Vec& force, bool again = false);
	// 0x008add10: a frame of `seconds`; returns the middle particle.
	const Vec& update(float seconds, const RagDollWorld& world);
	// 0x008acde0: the skeleton's node matrices from the particles,
	// relative to the middle particle (world axes). `matrices` holds the
	// last pose (in the same frame) and `local` the nodes' local transforms
	// for the locked bones and those that keep their parent's.
	void pose(const Skeleton& skeleton, const LocalPose& local, std::vector<Matrix>& matrices) const;

	const Vec& middle() const;
	bool sleeping() const { return !m_awake; }
	// The impact to sound (0x008a79f0): the fastest hit since the last one
	// taken, once (speed in metres a step, the surface's material).
	bool takeImpact(float& speed, uint32_t& material);
	// A particle has hit something (+0x63): with ragdollLandedDelay since
	// the death, the body has landed (0x005eda20).
	bool touched() const { return m_touched; }
	const std::vector<Particle>& particles() const { return m_particles; }
	const RagDollTemplate* ragDollTemplate() const { return m_template; }
	// The body's facing: the normal of the hips and right shoulder (0x008a9ae0);
	// up when it lies on its back.
	Vec bodyNormal() const;

private:
	void integrate(float step);          // 0x008a8860
	void accumulate();                    // 0x008a8a10
	void solve(const RagDollWorld& world); // 0x008ac8f0
	void distanceConstraints();           // 0x008a8c40
	void lessThanConstraints();           // 0x008a8db0
	void angularConstraints();            // 0x008a8f30
	void dihedralConstraints();           // 0x008a92f0
	void legConstraints();                // 0x008aa360
	void legCollision();                  // 0x008a8ac0
	void collide(const RagDollWorld& world); // 0x008ac200
	void capsuleCollision(const RagDollWorld& world); // 0x008abf20
	void impact(const Particle& p, const Vec& normal, uint32_t material); // 0x008a7a20
	Vec hipAxis() const;                  // 0x008a9bd0
	Particle* particleOfBone(int bone);
	const Particle* particleOfBone(int bone) const;

	const RagDollTemplate* m_template = nullptr;
	std::vector<Particle> m_particles;
	bool m_forced = false;     // 0x1c
	bool m_awake = true;       // 0x1d
	Vec m_last {};             // 0x20: the middle particle last frame
	float m_sleepLeft = 0;     // 0x38
	float m_accumulated = 0;   // 0x4c
	float m_gravityRamp = 0;   // 0x50
	float m_gravityTime = 0;   // 0x54
	// Impacts: the speed and material of the hardest hit waiting (0x3c,
	// 0x40, 0x44) and the time since the last one taken (0x48).
	float m_impactSpeed = 0;
	uint32_t m_impactMaterial = 0;
	bool m_impactPending = false;
	float m_sinceImpact = 0;
	bool m_touched = false;    // 0x63
};

} // namespace dice::hfe::anim
