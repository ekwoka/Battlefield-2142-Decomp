// The static collision meshes of a level, for queries by moving objects.
//
// The original keeps each object's collision mesh in object space with a
// BSP per LOD (0x006e66b0 walks it). This keeps the triangles of one LOD
// (in world space for the static objects, in object space for a moving
// one's) bucketed in a grid; a query tests the triangles near a sphere or
// a segment. The BSP only chooses which faces get tested, so the contacts
// are the same. The queries: a sphere where it is (querySphere; the
// soldiers' swept sphere, slot 24 0x006e2350, is SoldierPhysics'), segments
// (raycast; segmentContact, the mobile mesh physics' slot 22 0x006e5920),
// and capsules against the edges (capsuleEdgeContacts, slot 25).
#pragma once

#include "CollisionMeshFile.h"
#include "RigidBody.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace dice::hfe::physics {

struct SphereContact {
	Vec3 point;   // on the surface
	Vec3 normal;  // away from the surface, towards the sphere centre
	float depth;  // negative: how far the sphere is inside
	uint32_t material;
};

// Where the segment a-b comes within `radius` of the segment e0-e1 (a
// capsule), as parameters along a-b strictly between 0 and 1, in order:
// at most two (0x006bde70: the distance against the capsule's cylinder
// and end spheres).
int segmentCapsuleCrossings(const Vec3& a, const Vec3& b, const Vec3& e0, const Vec3& e1, float radius, float roots[2]);

// LODs of BF2 collision meshes by user.
// Collision type 4 is the trees' and poles' (statics with a trunk: the
// mobile mesh physics' third pass, 0x006bb050).
enum class CollisionUser { Projectile = 0, Vehicle = 1, Soldier = 2, Tree = 4 };

class CollisionWorld {
public:
	// Adds geometry 0 of `mesh` (all parts) at the LOD for `user` (parts
	// without one add nothing: collisionLodFor), placed by
	// an object transform (row vectors: world = local * rotation + position).
	// `materials` maps the mesh's material indices to material ids (the
	// template's mapMaterial); `owner` is reported with hits.
	void addMesh(const CollisionMeshFile& mesh, CollisionUser user, const Mat3& rotation, const Vec3& position,
		uint32_t owner = 0, const std::vector<uint32_t>& materials = {});
	size_t triangles() const { return m_triangles.size(); }

	// The faces a sphere touches (0x006bf010 per face): inside a face the
	// contact has the face's normal, also with the centre behind the face
	// (depth below -radius then); an edge or corner only touches from the
	// front.
	void querySphere(const Vec3& center, float radius, std::vector<SphereContact>& contacts) const;

	struct Hit {
		float fraction; // along the segment, 0..1
		Vec3 point;
		Vec3 normal; // facing the segment's start
		uint32_t material;
		uint32_t owner;
		Vec3 faceNormal {}; // the face's own (out of its object)
	};
	// The first face the segment from `from` to `to` passes through.
	bool raycast(const Vec3& from, const Vec3& to, Hit& hit) const;
	// A moving mesh vertex against the faces (the mobile mesh physics'
	// first pass, 0x006bb050 -> collision-mesh slot 22 0x006e5920): the
	// vertex's segment from its last pose to now. The first face it
	// crosses from the front gives a contact at the crossing with the
	// face's normal, as deep as the end lies behind the face (<= 0).
	bool segmentContact(const Vec3& from, const Vec3& to, SphereContact& contact) const;
	// segmentContact in a mesh kept in its object's space, placed by
	// `rotation` and `position` (the segment and the contact in world space).
	bool segmentContact(const Vec3& from, const Vec3& to, const Mat3& rotation, const Vec3& position,
		SphereContact& contact) const;
	// A moving mesh's edge a-b against the faces (0x006bb050's second
	// pass): segmentContact from a, then from b. Crossed from a only: the
	// crossing and its face's normal; from both ends: the middle of the two
	// crossings, along the normals' sum flattened (y 0) and normalised, or
	// from there towards `origin` when that sum is under `minNormal`
	// (*0x00a84230, 0.1). False when a's segment crosses nothing.
	bool edgeContact(const Vec3& a, const Vec3& b, const Vec3& origin, float minNormal, Vec3& point, Vec3& normal) const;
	// A capsule (segment a-b, radius) against the faces' edges only
	// (collision-mesh slot 25, 0x006e5a50 -> 0x006e2ee0, per face
	// 0x006be990; the ragdoll's capsules). At most one contact a face: for
	// each edge, where the segment comes within the radius of it
	// (0x006bde70): two crossings give their midpoint (taken over any
	// earlier edge), one is taken if it comes earlier along a-b. The
	// contact's normal runs from the edge's nearest point to it, its depth
	// is that distance less the radius; a normal pointing down is turned up
	// and the contact made no push (depth the distance).
	void capsuleEdgeContacts(const Vec3& a, const Vec3& b, float radius, std::vector<SphereContact>& contacts) const;
	// Each face with a cell within `radius` of `center` across (x, z), once.
	void forEachFaceNear(const Vec3& center, float radius,
		const std::function<void(const Vec3& a, const Vec3& b, const Vec3& c, uint32_t material, uint32_t owner)>& visit) const;

private:
	bool trace(const Vec3& from, const Vec3& to, Hit& hit, bool frontOnly) const;
	struct Triangle {
		Vec3 a, b, c;
		Vec3 normal;
		uint32_t material;
		uint32_t owner;
		mutable uint32_t stamp = 0;
	};
	static constexpr float kCell = 8.0f;
	static int64_t cellKey(int x, int z)
	{
		return static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(z));
	}

	std::vector<Triangle> m_triangles;
	std::unordered_map<int64_t, std::vector<uint32_t>> m_cells;
	mutable uint32_t m_stamp = 0;
};

} // namespace dice::hfe::physics
