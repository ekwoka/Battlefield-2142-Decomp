// The vehicles' parts as the rounds meet them.
//
// A vehicle's collision mesh holds a geometry per part: geometry 0 is the
// root's, and each child whose template sets collisionPart N (template
// +0x74) is geometry N. When the object is made, 0x0063ccc0 walks the
// object tree and gives each child with a collisionPart over 0 and a
// physicsType (+0xf4) other than None, Point or RotationalPoint (0, 1, 2)
// an instance of the root's mesh at that geometry (the collision manager's
// slot 0x28, 0x006d8180, set on the child as interface 0xcaa32), so every
// part collides in its own frame as it moves: a walker's legs and torso
// (their bones, boneIndex), turrets and guns (RotationalBundles), wheels
// and springs, the FAV's arms (LookAtObject). The walkers' geometry 0 is
// a single small face (us_heavy_mech's: one, with 48 parts); their
// shape is all parts.
//
// The walk runs for every object with an instance of its own (0x0063d720,
// with that object's mesh) and ends at the first child that has one: a
// Titan's gun hubs' cannons (eu_aacannon's base, barrel and firearm) are
// parts of their cannon's mesh; a static object's parts are the static
// world's (GameWorld::addStaticCollision).
//
// The rewrite keeps each part's projectile LOD in the part's own frame
// and poses the part, once a tick and only when a segment comes near the
// body, as the drawing places it (the template place, then partBone's
// matrix in its stead, or partPose's turn before it: collectParts). A hit
// on a part is the root's: 0x004c0e40 takes the hit node's armour or its
// nearest armoured ancestor's, and no shipped part with a collisionPart
// has an armour, a mapMaterial or a collision mesh of its own (the face
// materials go through the root's mapMaterial, as the shared mesh's). Its
// decal is the part's own (0x005a36d0: the hit object's set and mesh),
// in the part's frame (partCollision, collisionPartFrame).
//
// The wreck (0x00654120 -> 0x006539f0(object, 1), over the tree): each
// instance's slot 0x30 (0x006e5df0) switches to its geometry's
// sub-geometry 1 when it has more than one (applied by slot 0x34,
// 0x006e5e90), and returns 0 otherwise, which clears the object's flag
// 0x100 (slot 0x54(0, 0x100)): out of the scene's rays. The shipped
// vehicles' geometry 0 has the wreck's shape as its second sub-geometry
// (us_heavy_mech's: 6746 faces), the parts have one: a wreck is its root's
// wreck shape alone. Every other LOD query goes through the instance's
// sub-geometry too: the vehicle LOD the ground pass and the mesh physics
// read, and the soldier LOD (setBodyLods).
#include "GameWorld.h"

#include "Animation/Pose.h"
#include "Physics/CollisionMeshFile.h"
#include "Physics/CollisionWorld.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>

namespace dice::hfe::game {

namespace {

bool sameName(const std::string& a, const std::string& b)
{
	return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
		return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
	});
}

// Where the segment a-b comes nearest the point c, squared.
float segmentDistanceSquared(const physics::Vec3& a, const physics::Vec3& b, const physics::Vec3& c)
{
	const physics::Vec3 d = b - a;
	const float span = physics::lengthSquared(d);
	const float along = span > 0 ? std::clamp(physics::dot(c - a, d) / span, 0.0f, 1.0f) : 0.0f;
	return physics::lengthSquared(a + d * along - c);
}

struct Frame {
	physics::Mat3 r;
	physics::Vec3 t;
};

} // namespace

// A part's vehicle and soldier LODs at one sub-geometry (as setBodyLods
// takes the root's): the vehicle LOD's vertices, their materials and box.
void GameWorld::partPhysics(Body::PartMesh::Physics& out, const physics::CollisionMeshFile& mesh,
	const physics::CollisionSubGeometry& sub, const std::vector<uint32_t>& materials, uint32_t owner, size_t geometry,
	size_t state)
{
	if (const physics::CollisionLod* lod = physics::collisionLodFor(mesh, sub, 1); lod && !lod->vertices.empty()) {
		for (size_t i = 0; i < lod->vertices.size(); ++i) {
			const uint16_t index = i < lod->vertexMaterials.size() ? lod->vertexMaterials[i] : 0;
			out.pointMaterials.push_back(index < materials.size() ? materials[index] : index);
			out.points.push_back({lod->vertices[i][0], lod->vertices[i][1], lod->vertices[i][2]});
		}
		out.boxMin = {lod->boundsMin[0], lod->boundsMin[1], lod->boundsMin[2]};
		out.boxMax = {lod->boundsMax[0], lod->boundsMax[1], lod->boundsMax[2]};
		out.radius = physics::boxRadius(lod->boundsMin, lod->boundsMax);
		auto world = std::make_shared<physics::CollisionWorld>();
		world->addMesh(mesh, physics::CollisionUser::Vehicle, {}, {}, owner, materials, geometry, state);
		out.vehicle = world;
	}
	if (const physics::CollisionLod* lod = physics::collisionLodFor(mesh, sub, 2); lod && !lod->faces.empty()) {
		auto world = std::make_shared<physics::CollisionWorld>();
		world->addMesh(mesh, physics::CollisionUser::Soldier, {}, {}, owner, materials, geometry, state);
		out.soldier = world;
	}
}

// `child` in `parent`'s frame (row vectors: the child's matrix, then the
// parent's).
Frame compose(const Frame& child, const Frame& parent)
{
	return {child.r * parent.r, parent.r.transform(child.t) + parent.t};
}

bool GameWorld::takesCollisionPart(const world::ObjectTemplate& t)
{
	// 0x0063ccc0's test of the template's physicsType (+0xf4; the string
	// switch 0x006bf540: 0 None, 1 Point, 2 RotationalPoint, 3 Mesh, 4
	// MechLegs): Mesh and MechLegs take an instance.
	const auto type = t.settings().find("physicsType");
	if (type == t.settings().end()) {
		return false; // None by default
	}
	const std::string& v = type->second;
	return sameName(v, "Mesh") || sameName(v, "MechLegs") || v == "3" || v == "4";
}

void GameWorld::buildCollisionParts(Body& body, const world::ObjectTemplate& root, const physics::CollisionMeshFile& mesh)
{
	body.partNodes.clear();
	body.partMeshes.clear();
	addCollisionParts(body, root, -1, {}, {}, mesh, materialMap(root), body.object, -1);
	finishCollisionParts(body);
}

void GameWorld::addCollisionParts(Body& body, const world::ObjectTemplate& owner, int ownerNode,
	const physics::Mat3& rotation, const physics::Vec3& position, const physics::CollisionMeshFile& mesh,
	const std::vector<uint32_t>& ownerMaterials, uint32_t faceOwner, int source)
{
	// Depth first, parents before children; a subtree with no part to
	// collide is dropped again.
	const std::function<bool(const world::ObjectTemplate&, int, const Frame&, int)> walk =
		[&](const world::ObjectTemplate& t, int parent, const Frame& frame, int depth) {
			bool any = false;
			for (const world::ChildTemplate& child : t.children()) {
				const world::ObjectTemplate* c = m_templates.find(child.name);
				if (!c || depth > 8) {
					continue;
				}
				// A child with its own collision mesh (the heavy IFV's
				// shield generators, a Titan's gun hubs' cannons) has its own
				// instance (+0x4c, set by its constructor 0x0063c000) and its
				// own walk (0x0063d720); this walk ends at it, leaving the
				// siblings after it too (0x0063ccc0's loop runs while +0x4c
				// is 0; no shipped tree has a part after such a sibling).
				if (c->settings().count("collisionMesh")) {
					break;
				}
				const size_t index = body.partNodes.size();
				Body::PartNode node;
				node.name = c->name();
				node.parent = parent;
				node.rotation = physics::Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z});
				node.position = {child.position.x, child.position.y, child.position.z};
				const Frame here = compose({node.rotation, node.position}, frame);
				body.partNodes.push_back(node);
				bool used = false;
				const auto part = c->settings().find("collisionPart");
				const int geometry = part == c->settings().end() ? 0 : std::atoi(part->second.c_str());
				if (geometry > 0 && static_cast<size_t>(geometry) < mesh.geometries.size() && takesCollisionPart(*c)) {
					std::vector<uint32_t> materials = materialMap(*c);
					if (materials.empty()) {
						materials = ownerMaterials;
					}
					Body::PartMesh made;
					made.node = static_cast<int>(index);
					made.source = source;
					made.file = &mesh;
					made.materials = materials;
					made.geometry = static_cast<size_t>(geometry);
					made.collisionGroups = static_cast<uint32_t>(std::atoi(
						c->settings().count("collisionGroups") ? c->settings().at("collisionGroups").c_str() : "0"));
					for (size_t state = 0; state < 2; ++state) {
						const physics::CollisionSubGeometry* sub = physics::collisionPart(mesh, static_cast<size_t>(geometry), state);
						const physics::CollisionLod* lod = sub
							? physics::collisionLodFor(mesh, *sub, static_cast<uint32_t>(physics::CollisionUser::Projectile))
							: nullptr;
						if (sub) {
							partPhysics(made.physics[state], mesh, *sub, materials, faceOwner, static_cast<size_t>(geometry), state);
						}
						if (!lod || lod->faces.empty()) {
							continue;
						}
						auto world = std::make_shared<physics::CollisionWorld>();
						world->addMesh(mesh, physics::CollisionUser::Projectile, {}, {}, faceOwner, materials,
							static_cast<size_t>(geometry), state);
						made.lod[state] = world;
						made.radius = std::max(made.radius, physics::boxRadius(lod->boundsMin, lod->boundsMax));
					}
					if (made.lod[0] || made.lod[1]) {
						// Its own response and node (0x005f6a60 / 0x005f6ba0,
						// by the template's +0xf1 bits 0x2 / 0x4 and +0xf4). A
						// Spring's contacts stay the Vehicle's wheels'.
						const bool spring = sameName(m_templates.typeNameOf(c->classId()), "Spring");
						const auto flag = [&](const char* key) {
							const auto it = c->settings().find(key);
							return it != c->settings().end() && std::atoi(it->second.c_str()) != 0;
						};
						const auto type = c->settings().find("physicsType");
						const bool mesh = type != c->settings().end() && (sameName(type->second, "Mesh") || type->second == "3");
						made.ownResponse = !spring && flag("hasCollisionPhysics");
						made.mover = made.ownResponse && mesh && flag("hasMobilePhysics");
						made.rotation = here.r; // at rest in the body's frame, until posed
						made.position = here.t;
						body.partNodes[index].mesh = static_cast<int>(body.partMeshes.size());
						body.partMeshes.push_back(std::move(made));
						used = true;
					}
				}
				used = walk(*c, static_cast<int>(index), here, depth + 1) || used;
				if (!used) {
					body.partNodes.resize(index);
				}
				any = any || used;
			}
			return any;
		};
	walk(owner, ownerNode, Frame {rotation, position}, 0);
}

void GameWorld::buildTitanCollisionParts(Titan& titan, Body& body)
{
	body.partNodes.clear();
	body.partMeshes.clear();
	for (size_t i = 0; i < titan.meshes.size(); ++i) {
		const TitanMesh& m = titan.meshes[i];
		if (!m.t || !m.mesh) {
			continue;
		}
		// The node with the mesh, fixed in the Titan's frame (its hubs
		// don't move); the walk's parts hang under it.
		const size_t node = body.partNodes.size();
		Body::PartNode owner;
		owner.name = m.t->name();
		owner.rotation = m.rotation;
		owner.position = m.position;
		body.partNodes.push_back(owner);
		const size_t before = body.partMeshes.size();
		const uint32_t faceOwner = m.node >= 0 ? titan.parts[static_cast<size_t>(m.node)].id : titan.object;
		addCollisionParts(body, *m.t, static_cast<int>(node), m.rotation, m.position, *m.mesh, m.materials, faceOwner,
			static_cast<int>(i));
		// A Titan's parts move with its sync, and what meets them takes the
		// whole push (a major actor): no responses of their own here.
		for (size_t p = before; p < body.partMeshes.size(); ++p) {
			body.partMeshes[p].ownResponse = body.partMeshes[p].mover = false;
		}
		if (body.partMeshes.size() == before) {
			body.partNodes.resize(node);
		}
	}
	finishCollisionParts(body);
}

void GameWorld::finishCollisionParts(Body& body)
{
	// The sphere a segment must come within for the parts to be posed and
	// tested: the parts at rest, with room for how far they move (a
	// walker's legs and torso stride and crouch within a few metres).
	float reach = 0;
	for (const Body::PartMesh& part : body.partMeshes) {
		reach = std::max(reach, std::sqrt(physics::lengthSquared(part.position)) + part.radius);
	}
	body.partReach = body.partMeshes.empty() ? 0.0f : reach * 1.5f + 2.0f;
	body.partsPosed = 0xffffffffu;
}

void GameWorld::poseCollisionParts(const Body& body) const
{
	if (body.partsPosed == m_ticks || body.partMeshes.empty()) {
		return;
	}
	if (!m_objects.find(body.object)) {
		return; // the rest frames (partPose asks the object)
	}
	body.partsPosed = m_ticks;
	std::vector<Frame> frames(body.partNodes.size());
	for (size_t i = 0; i < body.partNodes.size(); ++i) {
		const Body::PartNode& node = body.partNodes[i];
		Frame placed {node.rotation, node.position};
		anim::Matrix bone;
		world::Vec3 offset, rotation;
		if (partBone(body.object, node.name, bone)) {
			// A bone part (boneIndex, 0x0063d470 -> slot 0xe8) or a
			// LookAtObject (0x0064bc00): its matrix in place of the template's.
			placed.r.rows[0] = {bone[0], bone[1], bone[2]};
			placed.r.rows[1] = {bone[4], bone[5], bone[6]};
			placed.r.rows[2] = {bone[8], bone[9], bone[10]};
			placed.t = {bone[12], bone[13], bone[14]};
		} else if (partPose(body.object, node.name, offset, rotation)) {
			// The bundles' and wheels' turn and travel before the place.
			const physics::Mat3 turn = physics::Mat3::fromYawPitchRoll({rotation.x, rotation.y, rotation.z});
			placed = {turn * placed.r, placed.r.transform({offset.x, offset.y, offset.z}) + placed.t};
		}
		frames[i] = node.parent >= 0 ? compose(placed, frames[static_cast<size_t>(node.parent)]) : placed;
		if (node.mesh >= 0) {
			const Body::PartMesh& part = body.partMeshes[static_cast<size_t>(node.mesh)];
			part.rotation = frames[i].r;
			part.position = frames[i].t;
		}
	}
}

void GameWorld::wreckCollision(Body& body)
{
	if (body.collisionWrecked || !body.meshFile || isTitan(body.object)) {
		return;
	}
	body.collisionWrecked = true;
	// The root's instance: its wreck sub-geometry, or out of the rays.
	auto world = std::make_shared<physics::CollisionWorld>();
	const bool wreckShape = body.meshFile->geometries[0].subGeometries.size() >= 2;
	if (wreckShape) {
		world->addMesh(*body.meshFile, physics::CollisionUser::Projectile, {}, {}, body.object, body.meshMaterials, 0, 1);
	}
	if (body.cellsBuilt) {
		addShieldCellMeshes(body, *world);
	}
	body.projectileMesh = world;
	// Every other query of the instance goes through the same sub-geometry:
	// the mobile physics' vehicle LOD (the ground pass's points, the edges,
	// the box, the faces others meet) and the soldier LOD. The shipped
	// wrecks' sub-geometry has a vehicle LOD of the wreck's shape (as_fav's
	// 32 faces, us_heavy_mech's 68) and no soldier LOD. One without a
	// vehicle LOD would leave the ground pass nothing (slot 0x70(1) false):
	// the rewrite keeps the normal one then.
	if (wreckShape) {
		setBodyLods(body, *body.meshFile, body.meshMaterials, 1);
	}
}

bool GameWorld::raycastBody(const Body& body, const physics::Vec3& from, const physics::Vec3& to,
	physics::CollisionWorld::Hit& hit, int* partHit) const
{
	float best = 2;
	physics::CollisionWorld::Hit candidate {};
	if (partHit) {
		*partHit = -1;
	}
	if (body.projectileMesh && body.projectileMesh->raycast(from, to, candidate)) {
		best = candidate.fraction;
		hit = candidate;
	}
	if (body.partMeshes.empty() || segmentDistanceSquared(from, to, {}) > body.partReach * body.partReach) {
		return best <= 1;
	}
	poseCollisionParts(body);
	const int state = body.collisionWrecked ? 1 : 0;
	for (size_t i = 0; i < body.partMeshes.size(); ++i) {
		const Body::PartMesh& part = body.partMeshes[i];
		const physics::CollisionWorld* lod = part.lod[state].get();
		if (!lod || part.off || segmentDistanceSquared(from, to, part.position) > part.radius * part.radius) {
			continue;
		}
		const physics::Vec3 a = part.rotation.untransform(from - part.position);
		const physics::Vec3 b = part.rotation.untransform(to - part.position);
		if (lod->raycast(a, b, candidate) && candidate.fraction < best) {
			best = candidate.fraction;
			hit = candidate;
			hit.point = from + (to - from) * candidate.fraction;
			hit.normal = part.rotation.transform(candidate.normal);
			hit.faceNormal = part.rotation.transform(candidate.faceNormal);
			if (partHit) {
				*partHit = static_cast<int>(i);
			}
		}
	}
	return best <= 1;
}

// The parts in the mesh physics. Every object whose template has
// hasCollisionPhysics gets a response (0x005f6a60: the mobile Mesh one,
// 0x006ba0d0, or the non-mobile one, 0x006bfcc0), the parts' among them
// (the turrets', guns' and legs' templates set it), and a root's response
// chains its tree's (slot 0x5c, 0x006c0090: rebuilt from the children by
// 0x006c0030 when dirty). The broad phase finds a tree by its first
// object with a response in the scene's rays (0x006dbb70) and walks that
// chain (0x006f2d50), so a mover meets each part of another tree
// (0x006dc7b0, the part's instance +0x4c at its LOD), and a soldier each
// part's soldier LOD (0x006c3890 through the same chain, 0x006c4xxx's
// loop over the responses), in the part's frame as it is now (its object's
// matrix, slot 0x78). A wrecked part without a wreck shape is out of the
// rays (flag 0x100, 0x006e5df0) and so out of the broad phase too.
void GameWorld::partActors(Body& body, std::vector<MeshActor>& out)
{
	out.clear();
	if (body.partMeshes.empty() || !m_objects.find(body.object)) {
		return;
	}
	poseCollisionParts(body);
	const size_t state = body.collisionWrecked ? 1 : 0;
	const MeshActor root = body.vehicleMesh ? actorOf(body) : MeshActor {};
	const physics::Vec3 centre = body.body.position + body.body.rotation.transform(body.body.centerOfMass);
	for (size_t index = 0; index < body.partMeshes.size(); ++index) {
		const Body::PartMesh& part = body.partMeshes[index];
		const Body::PartMesh::Physics& lods = part.physics[state];
		if (part.off || !lods.vehicle || lods.points.empty()) {
			continue;
		}
		MeshActor a;
		a.body = &body;
		a.points = &lods.points;
		a.pointMaterials = &lods.pointMaterials;
		a.mesh = lods.vehicle.get();
		a.boxMin = lods.boxMin;
		a.boxMax = lods.boxMax;
		a.radius = lods.radius;
		a.rotation = part.rotation * body.body.rotation;
		a.position = body.body.position + body.body.rotation.transform(part.position);
		a.centerOfMass = a.rotation.untransform(centre - a.position);
		a.velocity = body.body.averageVelocity;
		a.material = root.material;
		a.collisionGroups = part.collisionGroups;
		a.fixed = body.fixed;
		// A Titan is a major actor its sync alone moves (as its meshes in
		// collideMover): the other takes the whole push.
		a.node = isTitan(body.object) ? physics::ShareNode {std::max(body.mass, 1e6f), true, false}
									   : physics::ShareNode {body.mass, body.majorActor, asleep(body)};
		a.file = part.file;
		a.materials = &part.materials;
		a.geometry = part.geometry;
		a.state = state;
		a.partMesh = part.ownResponse ? static_cast<int>(index) : -1;
		out.push_back(a);
	}
}

const physics::CollisionWorld* GameWorld::partCollision(uint32_t object, int part) const
{
	const Body* body = findBody(object);
	if (!body || part < 0 || static_cast<size_t>(part) >= body->partMeshes.size()) {
		return nullptr;
	}
	const Body::PartMesh& mesh = body->partMeshes[static_cast<size_t>(part)];
	return mesh.off ? nullptr : mesh.lod[body->collisionWrecked ? 1 : 0].get();
}

bool GameWorld::collisionPartFrame(uint32_t object, int part, physics::Mat3& rotation, physics::Vec3& position) const
{
	const Body* body = findBody(object);
	if (!body || part < 0 || static_cast<size_t>(part) >= body->partMeshes.size()) {
		return false;
	}
	poseCollisionParts(*body);
	const Body::PartMesh& mesh = body->partMeshes[static_cast<size_t>(part)];
	rotation = mesh.rotation;
	position = mesh.position;
	return true;
}

size_t GameWorld::collisionPartCount(uint32_t object) const
{
	const Body* body = findBody(object);
	return body ? body->partMeshes.size() : 0;
}

const physics::CollisionWorld* GameWorld::vehicleCollision(uint32_t object) const
{
	const Body* body = findBody(object);
	return body ? body->vehicleMesh.get() : nullptr;
}

} // namespace dice::hfe::game
