// The mobile mesh physics between objects (BF2/Physics): the broad phase
// that pairs each moving object with the objects near it (0x006dc7b0) and
// the mobile mesh response's slot 9 (ResponsePhysics.Basic, vtable
// 0x00992cb8, 0x006bb050) that tests one object's vehicle-LOD mesh (A)
// against another's (B) and records the contacts in both responses.
//
// The broad phase, for a mover M and a candidate C (another object's part,
// not of M's tree; each pair once a frame: a candidate that was a mover
// this frame is left out):
//  - their origins within (|vM| + |vC|) * dt + rM + rC, r being the vehicle
//    LOD's radius (the mesh's slot 0x50: its box's farthest corner from the
//    origin);
//  - the smaller one's sphere (that radius plus the motion) touching the
//    other's LOD box (*0x00a8510c = 1, 0x006aecc0);
//  - when the motion is under both radii and both LODs have over 10
//    vertices (*0x00a85104), their boxes overlapping (*0x00a850f4 = 1,
//    0x006aef70);
//  - not both under 4 vertices;
//  - then by rM / rC: from 1/4 to 4 both ways (slot 9 with A = C, B = M,
//    then A = M, B = C); over 4, C into M, and M into C too when both are
//    mobile (else, with C a root and M with a type-4 LOD, M's third pass
//    alone into C); under 1/4, M into C (and with M a root and C with a
//    type-4 LOD, C's third pass alone into M).
//
// Slot 9 (A into B), in order:
//  - nothing unless A has a vehicle LOD and B the LOD A asks for;
//  - A's motion relative to B this tick, d = (vA - vB) * dt (the nodes'
//    average velocities, +0x44), and A's centre of mass a tick ago,
//    last = A's origin + R * com - d (`travel` below is d - R * com);
//  - the small-object pre-trace: a mobile A whose LOD radius is under 0.3
//    (0x00975eb8) traces from `last` to its origin through B's mesh; a
//    crossing moves A back along that way by how far it went past the face;
//  - the helicopter exit (B's root static, A a root PlayerControlObject
//    whose first Engine is a helicopter's, on the server): its origin from
//    where it was to where it is through B's mesh; a crossing destroys it
//    (1e10 damage, material 0x29a) and ends the pair;
//  - a static A with at most 20 vertices (*0x00a84258), no type-4 LOD and
//    under half B's radius is tested as one point: its origin's way from
//    `last` (a Ladder, class 40000, ends the pair instead);
//  - pass 0, A's vertices: a mobile A's from `last` to the vertex now, a
//    static A's from the vertex now less d (plus R * com) to the vertex
//    now; a part of under 4 vertices (wheels, springs, landing gear) probes
//    down instead (wheelProbes), and a static one so, nothing;
//  - pass 1, the edges (A a PlayerControlObject root, B's root static,
//    *0x00a84214 = 1): A's steep faces' edges through B's mesh from either
//    end, a push of 3 at the crossing;
//  - pass 2, the trunks (B a root; A with a type-4 LOD, or with a radius,
//    treeCollisionDiameter / 2, and then a trunk line up its LOD box);
//  - each crossing: the relative point velocity v = vA - vB at it (pass 2
//    needs v . n >= 0), the shares (0x006b6cc0, physics::massShares), then
//    A's response gets (point, v * share, n, depth * depth share) and B's
//    the same with its shares; materials: A's vertex's (the mesh's slot
//    0x44) and B's face's: friction from their cell, elasticity their mean
//    (0x006ba4c0).
//
// Each contact also calls both objects' collision events (object slot 0xac,
// collisionEvent: the game logic's collision damage), records a contact
// effect (0x006b75d0) and wakes sleeping nodes by the shares; pairs sharing
// a collisionGroups bit don't meet, and a team's PlayerControlObject
// passes its team's force fields. A wheel's probes go to the wheel's own
// response (Vehicle::touchObject). Sleeping nodes are no movers.
//
// Not modelled: the response's stuck push (+0x11c) scaling the events'
// velocity, and a static's vertices into a wheel's three-vertex LOD.
#include "GameWorld.h"

#include "Physics/CollisionWorld.h"
#include "Physics/MaterialManager.h"
#include "World/ObjectTemplate.h"
#include "World/Object.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>

namespace dice::hfe::game {

using physics::Mat3;
using physics::Vec3;

namespace {

constexpr float kSmallObject = 0.3f;     // 0x00975eb8: the pre-trace under this LOD radius
constexpr size_t kOnePointVertices = 20; // *0x00a84258 (0x008ed770)
constexpr size_t kDetailedVertices = 10; // *0x00a85104 (0x00a84dd8)
constexpr float kEdgePush = 3.0f;        // *0x00a8423c (0x008ed730)
constexpr float kEdgeMinNormal = 0.1f;   // *0x00a84230 (0x008ed750)
constexpr float kStaticCell = 16.0f;     // the rewrite's grid of static parts
constexpr uint32_t kKillMaterial = 0x29a;          // the helicopter exit's material (instant kill)
constexpr float kEventSpeed = 0.1f;                // 0x00920bd8: the events over |v|^2 0.1
constexpr float kSoldierTerrainSpeed = 9.1f;       // *0x00a6a788 "soldier-terrain-speed-limit"
constexpr float kImpactSpeed = 4.0f;               // *0x00a6a790 "collision-impact-speed-limit"
constexpr float kSoldierImpactSpeed = 10.0f;       // *0x00a6a794 "soldier-impact-speed-limit"
constexpr float kSoldierImpactSpeedFaster = 3.0f;  // *0x00a6a778 "soldier-impact-speed-limit-2"
constexpr float kSoldierMass = 200.0f;             // 0x005a2ca0: a soldier counts as 200

float length(const Vec3& v) { return std::sqrt(physics::lengthSquared(v)); }

Vec3 normalised(const Vec3& v)
{
	const float l = length(v);
	return l > 1.1920929e-07f ? v * (1.0f / l) : Vec3 {};
}

int64_t gridKey(int x, int z)
{
	return static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(z));
}

// 0x006ae680: a half-ray from `start` along `direction` against a box
// (min, max in the frame of `rotation` at `position`): where it enters
// and the face's outward normal. False when it misses or starts inside.
bool rayEntersBox(const Vec3& start, const Vec3& direction, const Vec3& low, const Vec3& high, const Mat3& rotation,
	const Vec3& position, Vec3& point, Vec3& normal)
{
	const Vec3 s = rotation.untransform(start - position);
	const Vec3 d = rotation.untransform(direction);
	const float so[3] = {s.x, s.y, s.z}, dir[3] = {d.x, d.y, d.z};
	const float lo[3] = {low.x, low.y, low.z}, hi[3] = {high.x, high.y, high.z};
	if (so[0] >= lo[0] && so[0] <= hi[0] && so[1] >= lo[1] && so[1] <= hi[1] && so[2] >= lo[2] && so[2] <= hi[2]) {
		return false;
	}
	float enter = 0, leave = 1e30f;
	int axis = -1;
	float sign = 0;
	for (int k = 0; k < 3; ++k) {
		if (std::fabs(dir[k]) < 1e-12f) {
			if (so[k] < lo[k] || so[k] > hi[k]) {
				return false;
			}
			continue;
		}
		float t0 = (lo[k] - so[k]) / dir[k], t1 = (hi[k] - so[k]) / dir[k];
		float faceSign = -1;
		if (t0 > t1) {
			std::swap(t0, t1);
			faceSign = 1;
		}
		if (t0 > enter) {
			enter = t0;
			axis = k;
			sign = faceSign;
		}
		leave = std::min(leave, t1);
		if (enter > leave) {
			return false;
		}
	}
	if (axis < 0) {
		return false;
	}
	point = start + direction * enter;
	Vec3 local {};
	(axis == 0 ? local.x : axis == 1 ? local.y : local.z) = sign;
	normal = rotation.transform(local);
	return true;
}

} // namespace

GameWorld::MeshActor GameWorld::actorOf(Body& body) const
{
	MeshActor a;
	a.body = &body;
	a.points = &body.points;
	a.pointMaterials = &body.pointMaterials;
	a.edges = &body.edges;
	a.mesh = body.vehicleMesh.get();
	a.trunk = body.trunk.get();
	a.trunkFromTypeFour = body.trunkFromTypeFour;
	a.trunkRadius = body.trunkRadius;
	a.boxMin = body.boxMin;
	a.boxMax = body.boxMax;
	a.radius = body.radius;
	a.rotation = body.body.rotation;
	a.position = body.body.position;
	a.centerOfMass = body.body.centerOfMass;
	a.velocity = body.body.averageVelocity;
	a.controlObject = body.controlObject;
	a.helicopter = body.helicopterEngine;
	a.particle = body.particle;
	a.material = body.material;
	a.collisionGroups = body.collisionGroups;
	if (body.controlObject) {
		if (const world::Object* object = m_objects.find(body.object)) {
			a.team = object->team();
		}
	}
	a.node = {body.mass, body.majorActor, asleep(body)};
	return a;
}

GameWorld::MeshActor GameWorld::actorOf(const StaticPart& part) const
{
	MeshActor a;
	a.part = &part;
	const MeshShape& shape = *part.shape;
	a.points = &shape.points;
	a.pointMaterials = &shape.pointMaterials;
	a.mesh = shape.hasVehicleLod ? shape.mesh.get() : nullptr;
	a.trunkFromTypeFour = shape.trunkFromTypeFour;
	a.trunk = shape.trunkFromTypeFour || part.trunkRadius > 0 ? shape.trunk.get() : nullptr;
	a.trunkRadius = part.trunkRadius;
	a.boxMin = shape.boxMin;
	a.boxMax = shape.boxMax;
	a.radius = shape.radius;
	a.rotation = part.rotation;
	a.position = part.position;
	a.fixed = true;
	a.ladder = part.ladder;
	a.material = part.material;
	a.collisionGroups = part.collisionGroups;
	// A static object's node: never a major actor here, and not awake
	// (0x006b6cc0 then leaves the whole push to the mobile one).
	a.node = {1, false, true};
	return a;
}

// The mobile mesh physics of a tick: every body is a mover in turn
// (0x006dc7b0), against the static parts near it and the bodies after it.
void GameWorld::collideBodies(float seconds)
{
	// A sleeping node is no mover (the dispatch 0x006dd0b0 skips it) but
	// stays a candidate for the others.
	for (size_t i = 0; i < m_bodies.size(); ++i) {
		if (!findDrone(m_bodies[i].object) && !isTitan(m_bodies[i].object) && m_objects.find(m_bodies[i].object)
			&& !asleep(m_bodies[i])) {
			collideMover(m_bodies[i], i, seconds);
		}
	}
}

void GameWorld::collideMover(Body& mover, size_t index, float seconds)
{
	if (!mover.vehicleMesh || mover.points.empty()) {
		return;
	}
	MeshActor m = actorOf(mover);
	const float speedM = length(mover.body.velocity);
	// The pair's tests before slot 9 (0x006dc7b0).
	const auto paired = [&](const MeshActor& c, float speedC) {
		const float motion = (speedM + speedC) * seconds;
		const float reach = motion + m.radius + c.radius;
		if (physics::lengthSquared(c.position - m.position) > reach * reach) {
			return false;
		}
		if (c.radius <= m.radius) {
			if (!physics::sphereTouchesBox(c.position, motion + c.radius, m.boxMin, m.boxMax, m.rotation, m.position)) {
				return false;
			}
		} else if (!physics::sphereTouchesBox(m.position, motion + m.radius, c.boxMin, c.boxMax, c.rotation, c.position)) {
			return false;
		}
		if (motion < std::min(m.radius, c.radius) && m.points->size() > kDetailedVertices
			&& c.points->size() > kDetailedVertices
			&& !physics::boxesOverlap(m.boxMin, m.boxMax, m.rotation, m.position, c.boxMin, c.boxMax, c.rotation, c.position)) {
			return false;
		}
		// collisionGroups (response slot 0x50): pairs sharing a bit don't meet.
		if (m.collisionGroups & c.collisionGroups) {
			return false;
		}
		// The launcher a response ignores (+0xfc, until the tick +0xf8:
		// 0x006b8de0): a drop pod and its Titan or APC.
		if (c.body && ((mover.ignoreSet && mover.ignored == c.body->object && m_ticks <= mover.ignoreUntil)
						  || (c.body->ignoreSet && c.body->ignored == mover.object && m_ticks <= c.body->ignoreUntil))) {
			return false;
		}
		return m.points->size() >= 4 || c.points->size() >= 4;
	};
	// Slot 9 in the orders the radii choose; false when the mover was
	// destroyed (the helicopter exit).
	const auto collide = [&](MeshActor& c, bool cRoot) {
		const float ratio = c.radius > 0 ? m.radius / c.radius : 1e30f;
		if (ratio >= 0.25f) {
			if (ratio <= 4.0f) {
				meshPair(c, m, seconds, false);
				return meshPair(m, c, seconds, false);
			}
			meshPair(c, m, seconds, false);
			if (!c.fixed) {
				return meshPair(m, c, seconds, false);
			}
			if (cRoot && m.trunkFromTypeFour) {
				meshPair(m, c, seconds, true);
			}
			return true;
		}
		const bool alive = meshPair(m, c, seconds, false);
		if (c.trunkFromTypeFour) {
			meshPair(c, m, seconds, true);
		}
		return alive;
	};

	// The static parts near it.
	++m_staticStamp;
	float reachM = m.radius;
	if (mover.vehicle) {
		const auto& wheels = mover.vehicle->wheels();
		for (size_t w = 0; w < wheels.size(); ++w) {
			const float part = w < mover.wheelRadii.size() ? mover.wheelRadii[w] : wheels[w].radius;
			reachM = std::max(reachM, length(wheels[w].mount) + part + 2 * std::fabs(wheels[w].radius));
		}
	}
	reachM += speedM * seconds;
	const int x0 = static_cast<int>(std::floor((m.position.x - reachM) / kStaticCell));
	const int x1 = static_cast<int>(std::floor((m.position.x + reachM) / kStaticCell));
	const int z0 = static_cast<int>(std::floor((m.position.z - reachM) / kStaticCell));
	const int z1 = static_cast<int>(std::floor((m.position.z + reachM) / kStaticCell));
	for (int x = x0; x <= x1; ++x) {
		for (int z = z0; z <= z1; ++z) {
			const auto cell = m_staticGrid.find(gridKey(x, z));
			if (cell == m_staticGrid.end()) {
				continue;
			}
			for (const uint32_t i : cell->second) {
				StaticPart& part = m_staticParts[i];
				if (part.stamp == m_staticStamp) {
					continue;
				}
				part.stamp = m_staticStamp;
				MeshActor c = actorOf(part);
				if (!c.mesh) {
					continue;
				}
				if (paired(c, 0)) {
					if (!collide(c, part.root)) {
						return;
					}
					m = actorOf(mover); // the pre-trace may have moved it
				}
				// Its wheels' parts as movers of their own (their probes).
				wheelProbes(mover, c, seconds);
			}
		}
	}
	// The other bodies but those that were movers already (before it, awake).
	for (size_t j = 0; j < m_bodies.size(); ++j) {
		Body& other = m_bodies[j];
		if (j == index || (j < index && !asleep(other))) {
			continue;
		}
		if (!other.vehicleMesh || other.points.empty() || findDrone(other.object) || !m_objects.find(other.object)) {
			continue;
		}
		MeshActor c = actorOf(other);
		if (paired(c, length(other.body.velocity))) {
			if (!collide(c, true)) {
				return;
			}
			m = actorOf(mover);
		}
		wheelProbes(mover, c, seconds);
		if (!asleep(other)) {
			wheelProbes(other, m, seconds);
		}
	}
	// The Titans' parts (their vehicle LODs, Titans.cpp), each a candidate
	// of its own with its collisionGroups: the team forcefields (0x100 the
	// PAC Titan's, 0x200 the EU's) let their own team's vehicles through
	// (meshPair's team rule, 0x006bb050's start). A Titan is a major actor
	// moved by its sync alone: the vehicle takes the whole push and rides
	// along by the Titan's velocity.
	for (const Titan& titan : m_titans) {
		Body* tb = findBody(titan.object);
		if (!tb || tb == &mover || titan.vehicleParts.empty()) {
			continue;
		}
		for (const Titan::VehiclePart& part : titan.vehicleParts) {
			MeshActor c;
			c.body = tb;
			c.points = &part.points;
			c.pointMaterials = &part.pointMaterials;
			c.mesh = part.mesh.get();
			c.boxMin = part.boxMin;
			c.boxMax = part.boxMax;
			c.radius = part.radius;
			c.rotation = tb->body.rotation;
			c.position = tb->body.position;
			c.velocity = tb->body.velocity;
			c.controlObject = true;
			c.collisionGroups = part.groups;
			c.team = titan.team;
			c.node = {std::max(tb->mass, 1e6f), true, false};
			if (paired(c, length(tb->body.velocity))) {
				if (!collide(c, true)) {
					return;
				}
				m = actorOf(mover);
			}
			wheelProbes(mover, c, seconds);
		}
	}
}

// Slot 9 of the mobile mesh response (0x006bb050), A into B.
bool GameWorld::meshPair(MeshActor& a, MeshActor& b, float seconds, bool trunkOnly)
{
	if (!a.mesh || !b.mesh || a.points->empty()) {
		return true;
	}
	// A team's PlayerControlObject passes the other's force fields and
	// doors (0x006bb050's start: interface 0xc4c5 slot 0x9c, team 1 against
	// collisionGroups 0x100, team 2 against 0x200; shipped: as_* 256, eu_* 512).
	const auto passes = [](const MeshActor& pco, const MeshActor& other) {
		return (pco.team == 1 && (other.collisionGroups & 0x100)) || (pco.team == 2 && (other.collisionGroups & 0x200));
	};
	if (passes(a, b) || passes(b, a)) {
		return true;
	}
	const Vec3 motion = (a.velocity - b.velocity) * seconds;
	Vec3 travel = motion - a.rotation.transform(a.centerOfMass); // local_8c
	const Vec3 last = a.position - travel;                       // local_12c: the centre of mass a tick ago

	// The small-object pre-trace (0x6bb47f): its way back to where it is
	// stops at the first face crossed.
	if (a.body && a.radius < kSmallObject) {
		physics::SphereContact hit;
		const Vec3 end = last + travel;
		if (b.mesh->segmentContact(last, end, b.rotation, b.position, hit)) {
			const Vec3 back = normalised(travel) * length(end - hit.point);
			travel = travel - back;
			a.body->body.position = a.body->body.position - back;
			a.position = a.body->body.position;
		}
	}

	// The helicopter exit, and whether the edge pass runs.
	bool edges = false;
	if (b.fixed && a.body && a.controlObject) {
		edges = true;
		if (a.helicopter && a.body->wreckTime < 0) {
			physics::SphereContact hit;
			if (b.mesh->segmentContact(a.body->lastPosition, a.position, b.rotation, b.position, hit)) {
				damage(a.body->object, 1e10f, 0x29a, 0x29a, -1);
				return false;
			}
		}
	}

	// A small static's vertices as one point (local_25).
	bool onePoint = false;
	if (a.fixed && a.points->size() <= kOnePointVertices && !a.trunkFromTypeFour && a.radius < b.radius * 0.5f) {
		if (a.ladder) {
			return true;
		}
		onePoint = true;
	}

	std::vector<Vec3> turned; // the vertices turned by A's rotation (0x00a842d4)
	for (int pass = trunkOnly ? 2 : 0; pass < 3; ++pass) {
		if (pass == 0) {
			const std::vector<Vec3>& points = *a.points;
			for (size_t i = 0; i < points.size(); ++i) {
				const Vec3 v = a.rotation.transform(points[i]);
				turned.push_back(v);
				Vec3 start = last, along = travel;
				if (!onePoint) {
					if (a.fixed) {
						start = last + v;
					} else {
						along = v + travel;
					}
					if (points.size() < 4) {
						// A part of under 4 vertices probes down (as wheelProbes
						// does for the wheels): from its origin raised by h = |v0.y|
						// (and for i 1 and 2 by 0.1 more, 0.1 forward or back), 2h
						// down its up axis. A static one's way is then nothing
						// (local_8c zeroed, its start not along it).
						travel = {};
						if (a.fixed) {
							continue;
						}
						const float h = std::fabs(points[0].y);
						const Vec3 up = a.rotation.transform({0, 1, 0}), forward = a.rotation.transform({0, 0, 1});
						start = i == 0 ? a.position + up * h
									   : a.position + up * (h + 0.1f) + forward * (i == 1 ? 0.1f : -0.1f);
						along = up * (-2 * h);
					}
				}
				if (physics::lengthSquared(along) <= 1.1920929e-07f * 1.1920929e-07f) {
					continue;
				}
				physics::SphereContact hit;
				if (!b.mesh->segmentContact(start, start + along, b.rotation, b.position, hit)) {
					continue;
				}
				const uint32_t material = i < a.pointMaterials->size() ? (*a.pointMaterials)[i] : 0;
				addMeshContact(a, b, hit.point, hit.normal, hit.depth, material, hit.material, false);
			}
		} else if (pass == 1) {
			// The edges (a PlayerControlObject root against a static's root):
			// a push of 3 at the crossing (node slot 0xa0, 0x006c7b10).
			if (!edges || !a.edges || !a.body) {
				continue;
			}
			const Vec3 origin = b.rotation.untransform(a.position - b.position);
			for (const auto& edge : *a.edges) {
				if (edge[0] >= turned.size() || edge[1] >= turned.size()) {
					continue;
				}
				const Vec3 p0 = b.rotation.untransform(a.position + turned[edge[0]] - b.position);
				const Vec3 p1 = b.rotation.untransform(a.position + turned[edge[1]] - b.position);
				Vec3 point, normal;
				if (b.mesh->edgeContact(p0, p1, origin, kEdgeMinNormal, point, normal)) {
					point = b.position + b.rotation.transform(point);
					normal = b.rotation.transform(normal);
					a.body->body.addForce(point - a.body->body.position, normal * kEdgePush);
				}
			}
		} else {
			// The trunks: A's type-4 faces, or a trunk line, against B's box.
			const bool bRoot = b.body || (b.part && b.part->root);
			if (!bRoot || !a.trunk || (!a.trunkFromTypeFour && a.trunkRadius <= 0)) {
				continue;
			}
			float radius = a.trunkRadius;
			if (a.trunkFromTypeFour && radius < 0.01f) {
				radius = 0.3f;
			}
			const Vec3 c = b.position + b.rotation.transform(b.centerOfMass);
			const Vec3 up = b.rotation.transform({0, 1, 0});
			const Vec3 localC = a.rotation.untransform(c - a.position);
			const float reach = length(b.boxMax - b.boxMin) + length(travel) + radius + 1.0f;
			a.trunk->forEachFaceNear(localC, reach,
				[&](const Vec3& la, const Vec3& lb, const Vec3& lc, uint32_t material, uint32_t) {
					const Vec3 p0 = a.position + a.rotation.transform(la);
					const Vec3 v1 = a.position + a.rotation.transform(lb);
					const Vec3 v2 = a.position + a.rotation.transform(lc);
					const float s0 = physics::dot(p0 - c, up);
					Vec3 p1 = v1;
					float s1 = physics::dot(p1 - c, up);
					if ((s0 < 0) == (s1 < 0)) {
						p1 = v2;
						s1 = physics::dot(p1 - c, up);
						if ((s0 < 0) == (s1 < 0)) {
							return;
						}
					}
					const float t = s1 / (s1 - s0);
					const Vec3 x = p1 + (p0 - p1) * t;
					const Vec3 start = x - travel;
					const Vec3 towards = (c - start) + travel;
					Vec3 point, normal;
					if (!rayEntersBox(start, towards, b.boxMin, b.boxMax, b.rotation, b.position, point, normal)) {
						return;
					}
					normal.y = 0;
					const float l = length(normal);
					if (l < 1e-6f) {
						return;
					}
					normal = normal * (-1.0f / l);
					const float dist = physics::dot(normal, point - x);
					if (std::fabs(dist) > radius) {
						return;
					}
					addMeshContact(a, b, point, normal, radius - dist, material, b.material, true);
				});
		}
	}
	return true;
}

// The contact of a crossing (0x6bc764 on): the shares, then each side's
// response (slot 0x2c, 0x006ba4c0). Pass 2's `depth` is the push (r - dist,
// positive along the normal into B); the others' is <= 0 (how far the end
// lies behind B's face).
physics::MassShares GameWorld::addMeshContact(MeshActor& a, MeshActor& b, const Vec3& point, const Vec3& normal,
	float depth, uint32_t materialA, uint32_t materialB, bool trunkPass, bool toA)
{
	const Vec3 relative = a.pointVelocity(point) - b.pointVelocity(point);
	const float approach = physics::dot(relative, normal);
	if (trunkPass && approach < 0) {
		return {}; // pass 2: only when B comes towards A's trunk
	}
	// The collision events (0x6bc764 on): over |v|^2 0.1, or B's face of
	// material 0x29a, on both objects (their vetoes are never used in the
	// shipped game, see collisionEvent).
	const uint32_t objectA = a.body ? a.body->object : a.part ? a.part->object : 0;
	const uint32_t objectB = b.body ? b.body->object : b.part ? b.part->object : 0;
	const float speed = physics::lengthSquared(relative);
	if (speed > kEventSpeed || materialB == kKillMaterial) {
		collisionEvent(objectA, objectB, relative, normal, point - a.position, materialA, materialB);
		collisionEvent(objectB, objectA, -relative, normal, point - b.position, materialB, materialA);
	}
	// The contact effect (0x6bcab2), past both nodes' handleCollisionSpeed:
	// in A's root's response if only A is mobile, B's if A is not, the
	// first's of two mobile ones.
	const float hcsA = a.body ? a.body->handleCollisionSpeed : 1.0f;
	const float hcsB = b.body ? b.body->handleCollisionSpeed : 1.0f;
	if (hcsA * hcsA <= speed && hcsB * hcsB <= speed) {
		Body* record = a.body && !b.body ? a.body : !a.body ? b.body : (a.body->object < b.body->object ? a.body : b.body);
		if (record) {
			recordContactEffect(*record, materialA, materialB, relative, point, normal);
		}
	}
	const physics::MassShares shares =
		physics::massShares(a.node, b.node, approach, a.particle || b.particle, a.fixed, b.fixed);
	physics::ContactMaterial material;
	if (m_materials) {
		material.friction = m_materials->friction(materialA, materialB);
		material.elasticity = (m_materials->elasticity(materialA) + m_materials->elasticity(materialB)) * 0.5f;
	}
	if (shares.wakeA && a.body) {
		wake(*a.body);
	}
	if (shares.wakeB && b.body) {
		wake(*b.body);
	}
	// A lighter object on a heavier mobile one keeps it (+0xf4).
	if (a.body && b.body && a.body->mass < b.body->mass) {
		a.body->support = b.body->object;
	}
	if (shares.velocityA != 0 && a.body && toA) {
		a.body->response.addContact(point - a.position, relative * shares.velocityA, normal, depth * shares.depthA,
			material);
	}
	if (shares.velocityB != 0 && b.body) {
		b.body->response.addContact(point - b.position, relative * shares.velocityB, normal, depth * shares.depthB,
			material);
	}
	return shares;
}

// A body's wheels as the parts they are in the original (a Spring each
// with a vehicle LOD of three vertices at one height: 73 shipped parts),
// movers against B of their own: their vertex pass probes down (0x6bbbaa:
// local_60 < 4). Per vertex i, from the part's origin P0 raised by h
// (|v0.y|: the wheel's radius) and, for i 1 and 2, by 0.1 more and 0.1
// forward or back, a segment 2h down along the part's up axis; a part
// 0.5 m or more to a side of the root (along the root's right axis) starts
// 0.5 m inwards and ends where it would have. Each crossing is a contact
// as in the vertex pass, here in the vehicle's response. The pair's tests
// with the wheel's own radius: the probe runs unless B is static and over
// four times smaller (then only B's vertices into the wheel's LOD, which
// isn't modelled).
void GameWorld::wheelProbes(Body& body, MeshActor& b, float seconds)
{
	if (!body.vehicle || !b.mesh) {
		return;
	}
	const Mat3& r = body.body.rotation;
	const Vec3 right = r.transform({1, 0, 0}), up = r.transform({0, 1, 0}), forward = r.transform({0, 0, 1});
	MeshActor a = actorOf(body);
	const auto& wheels = body.vehicle->wheels();
	for (size_t w = 0; w < wheels.size(); ++w) {
		const Vehicle::Wheel& wheel = wheels[w];
		const float h = std::fabs(wheel.radius);
		const Vec3 mount {wheel.mount.x, wheel.mount.y + wheel.compression, wheel.mount.z};
		const Vec3 p0 = body.body.position + r.transform(mount);
		const float partRadius = w < body.wheelRadii.size() ? body.wheelRadii[w] : h;
		const float motion = (length(body.body.velocity) + length(b.body ? b.body->body.velocity : Vec3 {})) * seconds;
		const float reach = motion + partRadius + b.radius;
		if (physics::lengthSquared(b.position - p0) > reach * reach || b.points->size() < 4) {
			continue;
		}
		if (b.radius > partRadius
			&& !physics::sphereTouchesBox(p0, motion + partRadius, b.boxMin, b.boxMax, b.rotation, b.position)) {
			continue;
		}
		if (b.fixed && b.radius > 0 && partRadius / b.radius > 4.0f) {
			continue;
		}
		// The part's forward turns with its steering (about its up axis).
		const float steer = wheel.steer * 3.14159265f / 180.0f;
		const Vec3 fwd = forward * std::cos(steer) + right * std::sin(steer);
		const float side = physics::dot(p0 - body.body.position, right);
		for (int i = 0; i < 3; ++i) {
			Vec3 start = i == 0 ? p0 + up * h : p0 + up * (h + 0.1f) + fwd * (i == 1 ? 0.1f : -0.1f);
			Vec3 along = up * (-2 * h);
			if (side <= -0.5f) {
				start = start + right * 0.5f;
				along = along - right * 0.5f;
			} else if (side >= 0.5f) {
				start = start - right * 0.5f;
				along = along + right * 0.5f;
			}
			physics::SphereContact hit;
			if (!b.mesh->segmentContact(start, start + along, b.rotation, b.position, hit)) {
				continue;
			}
			// The contact goes to the wheel's own response (its spring and
			// tyre, Vehicle::touchObject: as deep as its depth share puts it),
			// the mirror to B's.
			const physics::MassShares shares =
				addMeshContact(a, b, hit.point, hit.normal, hit.depth, body.material, hit.material, false, false);
			if (shares.velocityA != 0) {
				const float grip = m_materials ? m_materials->friction(body.material, hit.material) : 0.8f;
				// The solve's Spring branch moves the wheel out along the mean
				// normal by its push, within 0..1 m (0x006ba7d0): the spring
				// takes the height that gives it.
				const float push = std::clamp(-hit.depth * shares.depthA, 0.0f, 1.0f);
				const float height = (start + along).y + push * hit.normal.y;
				body.vehicle->touchObject(w, height, hit.normal, b.pointVelocity(hit.point), grip);
			}
		}
	}
}

// The static objects' parts, each template of a static object's tree with
// a collision mesh, in a grid of 16 m cells by their sphere.
void GameWorld::addStaticPart(StaticPart part)
{
	const uint32_t index = static_cast<uint32_t>(m_staticParts.size());
	const float r = part.shape->radius;
	const int x0 = static_cast<int>(std::floor((part.position.x - r) / kStaticCell));
	const int x1 = static_cast<int>(std::floor((part.position.x + r) / kStaticCell));
	const int z0 = static_cast<int>(std::floor((part.position.z - r) / kStaticCell));
	const int z1 = static_cast<int>(std::floor((part.position.z + r) / kStaticCell));
	for (int x = x0; x <= x1; ++x) {
		for (int z = z0; z <= z1; ++z) {
			m_staticGrid[gridKey(x, z)].push_back(index);
		}
	}
	m_staticParts.push_back(std::move(part));
}

// A collision mesh's vehicle LOD in its object's space, shared by the
// objects that use it with the same materials.
std::shared_ptr<const GameWorld::MeshShape> GameWorld::meshShape(const physics::CollisionMeshFile& file,
	const std::vector<uint32_t>& materials)
{
	auto& shape = m_shapes[{&file, materials}];
	if (shape) {
		return shape;
	}
	auto made = std::make_shared<MeshShape>();
	made->mesh = std::make_shared<physics::CollisionWorld>();
	made->mesh->addMesh(file, physics::CollisionUser::Vehicle, {}, {}, 0, materials);
	Vec3 low {1e9f, 1e9f, 1e9f}, high {-1e9f, -1e9f, -1e9f};
	if (!file.geometries.empty()) {
		for (const physics::CollisionSubGeometry& part : physics::collisionParts(file, 0)) {
			if (physics::collisionLodFor(file, part, 4)) {
				made->trunkFromTypeFour = true;
			}
			const physics::CollisionLod* lod = physics::collisionLodFor(file, part, 1);
			if (!lod) {
				continue;
			}
			made->hasVehicleLod = true;
			for (size_t i = 0; i < lod->vertices.size(); ++i) {
				const auto& v = lod->vertices[i];
				made->points.push_back({v[0], v[1], v[2]});
				const uint16_t index = i < lod->vertexMaterials.size() ? lod->vertexMaterials[i] : 0;
				made->pointMaterials.push_back(index < materials.size() ? materials[index] : index);
			}
			low = {std::min(low.x, lod->boundsMin[0]), std::min(low.y, lod->boundsMin[1]), std::min(low.z, lod->boundsMin[2])};
			high = {std::max(high.x, lod->boundsMax[0]), std::max(high.y, lod->boundsMax[1]), std::max(high.z, lod->boundsMax[2])};
		}
	}
	if (made->hasVehicleLod) {
		made->boxMin = low;
		made->boxMax = high;
		made->radius = physics::boxRadius({low.x, low.y, low.z}, {high.x, high.y, high.z});
	}
	made->trunk = std::make_shared<physics::CollisionWorld>();
	if (made->trunkFromTypeFour) {
		made->trunk->addMesh(file, physics::CollisionUser::Tree, {}, {}, 0, materials);
	} else if (made->hasVehicleLod) {
		// The trunk line (0x6bc9xx: one face of three vertices) up the LOD
		// box's middle across x, at z 0: (cx, min y, 0), (cx, max y, 0),
		// (cx + 0.05, min y, 0); its material LOD 1's vertex 0's.
		const float cx = (low.x + high.x) * 0.5f;
		physics::CollisionLod lod;
		lod.collisionType = 4;
		lod.vertices = {{cx, low.y, 0}, {cx, high.y, 0}, {cx + 0.05f, low.y, 0}};
		lod.faces = {{{0, 1, 2}, 0}};
		physics::CollisionSubGeometry part;
		part.lods = {lod};
		physics::CollisionMeshFile line;
		line.version = 10;
		line.geometries.resize(1);
		line.geometries[0].subGeometries = {part};
		made->trunk->addMesh(line, physics::CollisionUser::Tree, {}, {}, 0,
			{made->pointMaterials.empty() ? 0u : made->pointMaterials[0]});
	}
	shape = std::move(made);
	return shape;
}

namespace {


float lengthOf(const Vec3& v) { return std::sqrt(physics::lengthSquared(v)); }

float number(const world::ObjectTemplate& t, const char* name, float fallback)
{
	const auto it = t.settings().find(name);
	return it == t.settings().end() ? fallback : static_cast<float>(std::atof(it->second.c_str()));
}

bool sameName(const std::string& a, const std::string& b)
{
	const world::CaseInsensitiveLess less;
	return !less(a, b) && !less(b, a);
}

} // namespace

void GameWorld::collisionEvent(uint32_t self, uint32_t other, const Vec3& velocity, const Vec3& normal,
	const Vec3& point, uint32_t ownMaterial, uint32_t otherMaterial)
{
	(void)point;
	const world::Object* object = m_objects.find(self);
	const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr;
	if (!t) {
		return;
	}
	// PlayerControlObject (0x005c72f0): material 0x29a is the helicopter
	// exit's (MeshCollision's meshPair destroys it), anything else the base.
	if (otherMaterial == kKillMaterial && sameName(t->typeName(), "PlayerControlObject")) {
		return;
	}
	// The base (0x0063c9e0): the material's veto (slot 0x70) is never set in
	// the shipped game (0x006a10f0 returns 0); the first armour of the
	// object (its own here) remembers the pair for a second (0x00653ef0:
	// by the other's material, the other object and its own material) and
	// only a new pair reaches the game logic.
	if (hitPoints(self) < 0) {
		return;
	}
	std::vector<CollisionMemory>& memory = m_collisionMemory[self];
	for (const CollisionMemory& m : memory) {
		if (m.other == other && m.ownMaterial == ownMaterial && m.otherMaterial == otherMaterial) {
			return;
		}
	}
	memory.push_back({other, ownMaterial, otherMaterial, 1.0f});
	// The server's slot 0x104 (0x004ba3f0) with the velocity turned round.
	if (other == 0) {
		groundCollisionDamage(self, -velocity, normal, otherMaterial, ownMaterial);
	} else {
		objectCollisionDamage(other, self, -velocity, normal, otherMaterial, ownMaterial);
	}
}

// 0x005a3db0: the ground (or nothing) hurting `victim`: the material
// cell's damageMod (slot 0x64) x speed x speed x the angle (|cos| between
// the motion and the normal, at least armor.angleMod); a soldier only
// over 9.1 m/s, and then by the speed over 9.1. No one is credited. (The
// impact's sound, effect and decal, 0x005a36d0, are left out.)
void GameWorld::groundCollisionDamage(uint32_t victim, const Vec3& velocity, const Vec3& normal, uint32_t groundMaterial,
	uint32_t victimMaterial)
{
	const world::Object* object = m_objects.find(victim);
	const world::ObjectTemplate* t = object ? object->objectTemplate() : nullptr;
	const float speed = lengthOf(velocity);
	if (!t || speed <= 0) {
		return;
	}
	const bool soldier = soldierPlayer(victim) != nullptr;
	if (soldier && speed < kSoldierTerrainSpeed) {
		return;
	}
	const float n = lengthOf(normal);
	float angle = n > 0 ? std::fabs(physics::dot(velocity, normal) / (speed * n)) : 1.0f;
	angle = std::max(angle, number(*t, "armor.angleMod", 0)); // armor template +0xf0
	const float mod = m_materials ? m_materials->damageMod(groundMaterial, victimMaterial) : 1.0f;
	const float amount = mod * (soldier ? speed - kSoldierTerrainSpeed : speed) * speed * angle;
	if (amount >= 0.1f) { // 0x004c0e40's least
		loseHitPoints(victim, amount, -1);
	}
}

// 0x005a4690: `attacker` running into `victim`. Soldiers deal none; a
// soldier victim is a road kill. Otherwise the speed over 4 m/s, squared,
// x cos^2 of the angle x the cell's damageMod x the mass factor (x the
// armour's +0xf4, 1, which nothing sets), credited to the attacker's
// driver (0x005a30b0).
void GameWorld::objectCollisionDamage(uint32_t attacker, uint32_t victim, const Vec3& velocity, const Vec3& normal,
	uint32_t attackerMaterial, uint32_t victimMaterial)
{
	if (soldierPlayer(attacker)) {
		return;
	}
	if (Player* soldier = soldierPlayer(victim)) {
		roadKill(attacker, *soldier, velocity, attackerMaterial, victimMaterial);
		return;
	}
	if (attacker == victim || hitPoints(victim) < 0) {
		return;
	}
	const float speed = lengthOf(velocity);
	const float n = lengthOf(normal);
	const float angle = speed > 0 && n > 0 ? std::fabs(physics::dot(velocity, normal) / (speed * n)) : 0.0f;
	const float over = std::max(0.0f, speed - kImpactSpeed);
	const float mod = m_materials ? m_materials->damageMod(attackerMaterial, victimMaterial) : 1.0f;
	const float amount = mod * collisionMassFactor(attacker, victim) * over * over * angle * angle;
	if (amount >= 0.1f) {
		loseHitPoints(victim, amount, collisionCredit(attacker, victim, velocity));
	}
}

// 0x005a43c0: a soldier hit by an object. Under 10 m/s nothing (3 m/s when
// the object is mobile and faster than the soldier); then (speed - that) x
// speed x the mass factor (the soldier as 200), no material cell (unless
// 0x29a, then it and 1 for the speed term).
void GameWorld::roadKill(uint32_t attacker, Player& victim, const Vec3& velocity, uint32_t attackerMaterial,
	uint32_t victimMaterial)
{
	const float speed = lengthOf(velocity);
	const bool instant = attackerMaterial == kKillMaterial;
	const Body* body = findBody(attacker);
	const float limit = body && physics::lengthSquared(victim.body.velocity) < physics::lengthSquared(body->body.velocity)
		? kSoldierImpactSpeedFaster
		: kSoldierImpactSpeed;
	if (speed < limit && !instant) {
		return;
	}
	const float term = instant ? 1.0f : (speed - limit) * speed;
	const float mod = instant && m_materials ? m_materials->damageMod(attackerMaterial, victimMaterial) : 1.0f;
	const float amount = mod * collisionMassFactor(attacker, victim.object) * term;
	if (amount >= 0.1f) {
		loseHitPoints(victim.object, amount, collisionCredit(attacker, victim.object, velocity));
	}
}

// 0x005a2ca0: clamp(attacker's mass / victim's, 0.3, 3) when both are
// mobile (a soldier weighing 200), else 1.
float GameWorld::collisionMassFactor(uint32_t attacker, uint32_t victim) const
{
	const Body* a = const_cast<GameWorld*>(this)->findBody(attacker);
	if (!a) {
		return 1;
	}
	float victimMass = 0;
	if (soldierPlayer(victim)) {
		victimMass = kSoldierMass;
	} else if (const Body* b = const_cast<GameWorld*>(this)->findBody(victim)) {
		victimMass = b->mass;
	} else {
		return 1;
	}
	return std::clamp(a->mass / std::max(victimMass, 1e-6f), 0.3f, 3.0f);
}

// 0x005a30b0: the attacker's driver (its PlayerControlObject's player), or
// nobody when the victim is a teammate who ran into it faster.
int GameWorld::collisionCredit(uint32_t attacker, uint32_t victim, const Vec3& velocity) const
{
	const Body* a = const_cast<GameWorld*>(this)->findBody(attacker);
	if (!a || a->occupants.empty() || a->occupants[0] < 0) {
		return -1;
	}
	const Player* driver = findPlayer(a->occupants[0]);
	if (!driver) {
		return -1;
	}
	const Player* hit = soldierPlayer(victim);
	Vec3 victimVelocity;
	if (hit) {
		victimVelocity = hit->body.velocity;
	} else if (const Body* b = const_cast<GameWorld*>(this)->findBody(victim)) {
		if (!b->occupants.empty() && b->occupants[0] >= 0) {
			hit = findPlayer(b->occupants[0]);
		}
		victimVelocity = b->body.velocity;
	}
	if (hit && hit->team == driver->team) {
		const float l = lengthOf(velocity);
		const Vec3 way = l > 0 ? velocity * (1.0f / l) : Vec3 {};
		if (std::fabs(physics::dot(victimVelocity, way)) > std::fabs(physics::dot(a->body.velocity, way))) {
			return -1;
		}
	}
	return driver->id;
}

// 0x006b75d0: a response keeps one contact effect and one contact sound
// until it has played them and its timer has run out: the material pair's
// cell (or the first material against 0) with a sound (index 0) records
// the sound slot, one without the effect slot; a free slot takes a contact
// faster than the one it holds (in practice the first after a reset).
void GameWorld::recordContactEffect(Body& body, uint32_t first, uint32_t second, const Vec3& velocity,
	const Vec3& point, const Vec3& normal)
{
	if (!m_materials) {
		return;
	}
	const float strength = physics::lengthSquared(velocity);
	if (strength <= 0) {
		return;
	}
	std::string sound;
	for (const uint32_t other : {second, 0u}) {
		const physics::MaterialCell* cell = m_materials->cell(first, other);
		if (cell && !cell->soundTemplates.empty() && !cell->soundTemplates[0].empty()) {
			sound = cell->soundTemplates[0];
			break;
		}
	}
	Body::ContactEffect& slot = sound.empty() ? body.contactEffect : body.contactSound;
	if (slot.state != 0 || slot.strongest >= strength) {
		return;
	}
	slot.state = 1;
	slot.strongest = strength;
	slot.first = first;
	slot.second = second;
	slot.timer = m_contactRandom.unit() + 0.5f;
	slot.point = point;
	slot.normal = normal;
	slot.direction = velocity * (1.0f / std::sqrt(strength));
	slot.sound = sound;
}

// The end of the solve (0x006ba7d0): the effect (the cell's effect index
// 0, 0x006b8c40: at the point, its up the normal and its forward against
// the motion along the surface); the sound (but not for the tracks' and
// big wheels' materials, 61 webMat_Tracked and 67 webMat_wheelBig) 0.05
// out from the point, with the cell's effect. (As the original, playing
// the sound's effect marks the effect slot, not the sound's.)
void GameWorld::playContactEffects(Body& body)
{
	const auto effectOf = [this](uint32_t first, uint32_t second) -> const std::string* {
		for (const uint32_t other : {second, 0u}) {
			const physics::MaterialCell* cell = m_materials->cell(first, other);
			if (cell && !cell->effectTemplates.empty() && !cell->effectTemplates[0].empty()) {
				return &cell->effectTemplates[0];
			}
		}
		return nullptr;
	};
	const auto spawn = [this](const std::string& name, const Body::ContactEffect& e) {
		physics::Mat3 frame;
		if (std::fabs(physics::dot(e.normal, e.direction)) >= 0.0001f) {
			const Vec3 up = normalised(e.normal);
			const Vec3 right = normalised(physics::cross(e.direction, up));
			frame.rows = {right, up, physics::cross(right, up)};
		}
		m_effects.spawn(name, e.point, frame);
	};
	if (!m_materials) {
		return;
	}
	if (body.contactEffect.state == 1) {
		if (const std::string* effect = effectOf(body.contactEffect.first, body.contactEffect.second)) {
			spawn(*effect, body.contactEffect);
			body.contactEffect.state = 2;
		}
	}
	Body::ContactEffect& s = body.contactSound;
	if (s.state == 1 && s.first != 0x3d && s.first != 0x43) {
		if (!s.sound.empty()) {
			m_sounds.push_back({s.sound, s.point + s.normal * 0.05f});
			s.state = 2;
		}
		if (const std::string* effect = effectOf(s.first, s.second)) {
			spawn(*effect, s);
			body.contactEffect.state = 2;
		}
	}
}

// Response slot 0x9c (0x006b74e0, from the object's update): each slot's
// timer runs down, four times as fast while the response holds no
// contacts (between solves it never does); out, the slot is free again.
void GameWorld::coolContactEffects(Body& body, float seconds)
{
	for (Body::ContactEffect* slot : {&body.contactEffect, &body.contactSound}) {
		if (slot->timer > 0) {
			slot->timer -= seconds * (body.response.contacts().empty() ? 4.0f : 1.0f);
			if (slot->timer <= 0) {
				slot->state = 0;
				slot->strongest = 0;
			}
		}
	}
}

bool GameWorld::bodyAsleep(uint32_t object) const
{
	const Body* body = const_cast<GameWorld*>(this)->findBody(object);
	return body && asleep(*body);
}

float GameWorld::bodySubmerged(uint32_t object) const
{
	const Body* body = const_cast<GameWorld*>(this)->findBody(object);
	return body ? body->submerged : 0.0f;
}

// Slot 0x150 (0x006c6de0): asleep, the history cleared, the motion gone.
void GameWorld::sleep(Body& body)
{
	body.sleep.sleep();
	physics::RigidBody& r = body.body;
	r.velocity = r.averageVelocity = r.acceleration = r.impulse = {};
	r.angularVelocity = r.torque = r.angularImpulse = {};
	r.frictionAcceleration = r.frictionTorque = {};
	r.frictionCount = 0;
}

// Slot 0x160 (0x006c5650): the counter back to 30; a sleeping
// PlayerControlObject root also wakes the objects round it (0x006bfad0:
// those a query of its radius, slot 0x90, finds; not major actors).
void GameWorld::wake(Body& body)
{
	if (!body.sleep.wake() || !body.controlObject) {
		return;
	}
	for (Body& other : m_bodies) {
		if (&other == &body || other.majorActor || !asleep(other)) {
			continue;
		}
		const float reach = body.radius + other.radius;
		if (physics::lengthSquared(other.body.position - body.body.position) <= reach * reach) {
			wake(other);
		}
	}
}

// The sleep test of the node's tick (0x006c9060): a still root counts
// down; asleep, its motion is zeroed (slot 0x148 -> 0x006c6080, gravity
// waiting in the acceleration), and it wakes when the heavier object it
// rests on (the response's +0xf4) is gone or awake.
void GameWorld::stepSleep(Body& body, float seconds)
{
	physics::RigidBody& r = body.body;
	if (body.sleep.counter >= 0) {
		body.sleep.sample(r.position, r.angularVelocity, seconds);
	}
	if (!asleep(body)) {
		return;
	}
	r.velocity = r.averageVelocity = r.acceleration = r.impulse = {};
	r.angularVelocity = r.torque = r.angularImpulse = {};
	r.frictionAcceleration = r.frictionTorque = {};
	r.frictionCount = 0;
	r.acceleration.y = physics::tuning::kGravity * r.gravityModifier;
	if (body.support != 0) {
		const Body* under = findBody(body.support);
		if (!under || !asleep(*under)) {
			body.support = 0;
			wake(body);
		}
	}
}


} // namespace dice::hfe::game
