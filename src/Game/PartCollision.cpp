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
// The rewrite keeps each part's projectile LOD in the part's own frame
// and poses the part, once a tick and only when a segment comes near the
// body, as the drawing places it (the template place, then partBone's
// matrix in its stead, or partPose's turn before it: collectParts). A hit
// on a part is the root's: 0x004c0e40 takes the hit node's armour or its
// nearest armoured ancestor's, and no shipped part with a collisionPart
// has an armour, a mapMaterial or a collision mesh of its own (the face
// materials go through the root's mapMaterial, as the shared mesh's).
//
// The wreck (0x00654120 -> 0x006539f0(object, 1), over the tree): each
// instance's slot 0x30 (0x006e5df0) switches to its geometry's
// sub-geometry 1 when it has more than one (applied by slot 0x34,
// 0x006e5e90), and returns 0 otherwise, which clears the object's flag
// 0x100 (slot 0x54(0, 0x100)): out of the scene's rays. The shipped
// vehicles' geometry 0 has the wreck's shape as its second sub-geometry
// (us_heavy_mech's: 6746 faces), the parts have one: a wreck is its root's
// wreck shape alone.
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

// 0x0063ccc0's test of the template's physicsType (+0xf4; the string
// switch 0x006bf540: 0 None, 1 Point, 2 RotationalPoint, 3 Mesh, 4
// MechLegs): Mesh and MechLegs take an instance.
bool meshPhysicsType(const world::ObjectTemplate& t)
{
	const auto type = t.settings().find("physicsType");
	if (type == t.settings().end()) {
		return false; // None by default
	}
	const std::string& v = type->second;
	return sameName(v, "Mesh") || sameName(v, "MechLegs") || v == "3" || v == "4";
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

// `child` in `parent`'s frame (row vectors: the child's matrix, then the
// parent's).
Frame compose(const Frame& child, const Frame& parent)
{
	return {child.r * parent.r, parent.r.transform(child.t) + parent.t};
}

} // namespace

void GameWorld::buildCollisionParts(Body& body, const world::ObjectTemplate& root, const physics::CollisionMeshFile& mesh)
{
	body.partNodes.clear();
	body.partMeshes.clear();
	const std::vector<uint32_t> rootMaterials = materialMap(root);
	// Depth first, parents before children; a subtree with no part to
	// collide is dropped again.
	const std::function<bool(const world::ObjectTemplate&, int, const Frame&, int)> walk =
		[&](const world::ObjectTemplate& t, int parent, const Frame& frame, int depth) {
			bool any = false;
			for (const world::ChildTemplate& child : t.children()) {
				const world::ObjectTemplate* c = m_templates.find(child.name);
				// A child with its own collision mesh (the heavy IFV's
				// shield generators) has its own instance (+0x4c, the
				// walk's stop) and is the cells' (SeatBehaviours.cpp).
				if (!c || depth > 8 || c->settings().count("collisionMesh")) {
					continue;
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
				if (geometry > 0 && static_cast<size_t>(geometry) < mesh.geometries.size() && meshPhysicsType(*c)) {
					std::vector<uint32_t> materials = materialMap(*c);
					if (materials.empty()) {
						materials = rootMaterials;
					}
					Body::PartMesh made;
					made.node = static_cast<int>(index);
					for (size_t state = 0; state < 2; ++state) {
						const physics::CollisionSubGeometry* sub = physics::collisionPart(mesh, static_cast<size_t>(geometry), state);
						const physics::CollisionLod* lod = sub
							? physics::collisionLodFor(mesh, *sub, static_cast<uint32_t>(physics::CollisionUser::Projectile))
							: nullptr;
						if (!lod || lod->faces.empty()) {
							continue;
						}
						auto world = std::make_shared<physics::CollisionWorld>();
						world->addMesh(mesh, physics::CollisionUser::Projectile, {}, {}, body.object, materials,
							static_cast<size_t>(geometry), state);
						made.lod[state] = world;
						made.radius = std::max(made.radius, physics::boxRadius(lod->boundsMin, lod->boundsMax));
					}
					if (made.lod[0] || made.lod[1]) {
						made.rotation = here.r;
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
	walk(root, -1, Frame {}, 0);
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
	if (body.meshFile->geometries[0].subGeometries.size() >= 2) {
		world->addMesh(*body.meshFile, physics::CollisionUser::Projectile, {}, {}, body.object, body.meshMaterials, 0, 1);
	}
	if (body.cellsBuilt) {
		addShieldCellMeshes(body, *world);
	}
	body.projectileMesh = world;
}

bool GameWorld::raycastBody(const Body& body, const physics::Vec3& from, const physics::Vec3& to,
	physics::CollisionWorld::Hit& hit) const
{
	float best = 2;
	physics::CollisionWorld::Hit candidate {};
	if (body.projectileMesh && body.projectileMesh->raycast(from, to, candidate)) {
		best = candidate.fraction;
		hit = candidate;
	}
	if (body.partMeshes.empty() || segmentDistanceSquared(from, to, {}) > body.partReach * body.partReach) {
		return best <= 1;
	}
	poseCollisionParts(body);
	const int state = body.collisionWrecked ? 1 : 0;
	for (const Body::PartMesh& part : body.partMeshes) {
		const physics::CollisionWorld* lod = part.lod[state].get();
		if (!lod || segmentDistanceSquared(from, to, part.position) > part.radius * part.radius) {
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
		}
	}
	return best <= 1;
}

} // namespace dice::hfe::game
