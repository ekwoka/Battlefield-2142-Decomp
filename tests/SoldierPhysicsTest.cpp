// Soldier movement: standing on the ground, walking, running up to speed.
#include "Game/SoldierControl.h"
#include "Physics/SoldierPhysics.h"
#include "TestMain.h"

#include <cmath>
#include <cstdio>

namespace physics = dice::hfe::physics;
namespace game = dice::hfe::game;

namespace {

physics::Ground slope(float rise)
{
	physics::Ground ground;
	ground.height = [rise](float x, float) { return x * rise; };
	ground.heightAndNormal = [rise](float x, float, physics::Vec3& normal) {
		const float length = std::sqrt(rise * rise + 1);
		normal = {-rise / length, 1 / length, 0};
		return x * rise;
	};
	return ground;
}

} // namespace

TEST(SoldierPhysics_StandsOnTheGround)
{
	physics::SoldierBody body;
	body.position = {0, 5, 0};
	for (int i = 0; i < 90; ++i) {
		body.step(1 / 30.0f, slope(0));
	}
	// The feet (1 m below the origin) on the ground.
	CHECK(body.onGround());
	CHECK(std::fabs(body.position.y - 1) < 0.1f);
}

TEST(SoldierPhysics_RunsAtThePoseSpeed)
{
	physics::SoldierBody body;
	body.position = {0, 1, 0};
	game::SoldierControl control;
	game::SoldierInput forward;
	forward.throttle = 1;
	for (int i = 0; i < 60; ++i) {
		control.update(forward, body);
		body.step(1 / 30.0f, slope(0));
	}
	const float speed = std::sqrt(body.velocity.x * body.velocity.x + body.velocity.z * body.velocity.z);
	std::printf("  running at %.2f m/s, at z %.2f\n", speed, body.position.z);
	CHECK(speed > 3.5f && speed < 4.1f);
	CHECK(std::fabs(body.velocity.x) < 0.01f && body.velocity.z > 0);

	// Sprinting is faster; letting go stops.
	forward.sprint = true;
	for (int i = 0; i < 60; ++i) {
		control.update(forward, body);
		body.step(1 / 30.0f, slope(0));
	}
	const float sprint = std::sqrt(body.velocity.x * body.velocity.x + body.velocity.z * body.velocity.z);
	CHECK(sprint > 5.5f);
	game::SoldierInput idle;
	for (int i = 0; i < 60; ++i) {
		control.update(idle, body);
		body.step(1 / 30.0f, slope(0));
	}
	CHECK(std::fabs(body.velocity.z) < 0.2f);
}

TEST(SoldierPhysics_WalksUpAHill)
{
	physics::SoldierBody body;
	body.position = {0, 1, 0};
	body.yawDegrees = 90; // facing +x, up the slope
	game::SoldierControl control;
	game::SoldierInput forward;
	forward.throttle = 1;
	for (int i = 0; i < 90; ++i) {
		control.update(forward, body);
		body.step(1 / 30.0f, slope(0.3f));
	}
	// Climbed, and still standing on the surface.
	CHECK(body.position.x > 5);
	CHECK(std::fabs(body.position.y - (body.position.x * 0.3f + 1)) < 0.3f);
}

namespace {

// A wall across +z at z = 3 (two triangles facing -z, 4 m wide and 3 m high)
// and a 1 m box to stand on at x = 10.
dice::hfe::physics::CollisionMeshFile testMesh()
{
	dice::hfe::physics::CollisionLod lod;
	const auto add = [&lod](std::array<float, 3> a, std::array<float, 3> b, std::array<float, 3> c) {
		const uint16_t i = static_cast<uint16_t>(lod.vertices.size());
		lod.vertices.push_back(a);
		lod.vertices.push_back(b);
		lod.vertices.push_back(c);
		lod.faces.push_back({{i, static_cast<uint16_t>(i + 1), static_cast<uint16_t>(i + 2)}, 0});
	};
	// Wall (normal -z: wound clockwise seen from the front, so that the
	// face normal (c - b) x (b - a) points to -z).
	add({-2, 0, 3}, {2, 3, 3}, {-2, 3, 3});
	add({-2, 0, 3}, {2, 0, 3}, {2, 3, 3});
	// Box top at y = 1 (normal +y).
	add({9, 1, -1}, {11, 1, 1}, {9, 1, 1});
	add({9, 1, -1}, {11, 1, -1}, {11, 1, 1});
	dice::hfe::physics::CollisionSubGeometry part;
	part.lods = {lod, lod, lod};
	dice::hfe::physics::CollisionGeometry geometry;
	geometry.subGeometries = {part};
	dice::hfe::physics::CollisionMeshFile mesh;
	mesh.geometries = {geometry};
	return mesh;
}

} // namespace

TEST(SoldierPhysics_StopsAtWalls)
{
	physics::CollisionWorld world;
	world.addMesh(testMesh(), physics::CollisionUser::Soldier, {}, {});
	CHECK(world.triangles() == 4);
	physics::SoldierBody body;
	body.position = {0, 1, 0};
	body.world = &world;
	game::SoldierControl control;
	game::SoldierInput forward;
	forward.throttle = 1;
	for (int i = 0; i < 90; ++i) {
		control.update(forward, body);
		body.step(1 / 30.0f, slope(0));
	}
	// Stopped with its 0.25 m radius against the wall at z = 3.
	CHECK(body.position.z < 3 - 0.2f && body.position.z > 2.4f);
	CHECK(std::fabs(body.position.y - 1) < 0.1f);
}

TEST(SoldierPhysics_StandsOnObjects)
{
	physics::CollisionWorld world;
	world.addMesh(testMesh(), physics::CollisionUser::Soldier, {}, {});
	physics::SoldierBody body;
	body.position = {10, 3, 0}; // above the box
	body.world = &world;
	for (int i = 0; i < 60; ++i) {
		body.step(1 / 30.0f, slope(0));
	}
	// On the box top (y = 1), feet there: origin at 2.
	CHECK(body.onGround());
	CHECK(std::fabs(body.position.y - 2) < 0.15f);
}

// Each LOD serves the user its collision type names (0x006e4880): a mesh
// with only a projectile LOD (a bush's) gives soldiers and vehicles nothing.
TEST(CollisionWorld_LodsByCollisionType)
{
	physics::CollisionMeshFile mesh = testMesh();
	mesh.version = 10;
	physics::CollisionSubGeometry& part = mesh.geometries[0].subGeometries[0];
	part.lods.resize(1);
	part.lods[0].collisionType = 0;
	physics::CollisionWorld soldiers, projectiles;
	soldiers.addMesh(mesh, physics::CollisionUser::Soldier, {}, {});
	projectiles.addMesh(mesh, physics::CollisionUser::Projectile, {}, {});
	CHECK(soldiers.triangles() == 0);
	CHECK(projectiles.triangles() == 4);
	// Types in any order: the soldiers' is the one marked 2.
	part.lods = {part.lods[0], part.lods[0]};
	part.lods[0].collisionType = 2;
	part.lods[1].collisionType = 0;
	part.lods[1].faces.resize(1);
	physics::CollisionWorld again;
	again.addMesh(mesh, physics::CollisionUser::Soldier, {}, {});
	CHECK(again.triangles() == 4);
	// Version 8 files have no types: by order.
	mesh.version = 8;
	physics::CollisionWorld byOrder;
	byOrder.addMesh(mesh, physics::CollisionUser::Vehicle, {}, {});
	CHECK(byOrder.triangles() == 1);
}

// Faces point out of the object (0x006e8080): a sphere on the box top is
// pushed up, and only a face's front pushes (0x006c3890 drops contacts whose
// sphere centre lies behind the face).
TEST(SoldierPhysics_OnlyFrontFacesPush)
{
	physics::CollisionWorld world;
	world.addMesh(testMesh(), physics::CollisionUser::Soldier, {}, {});
	std::vector<physics::SphereContact> contacts;
	world.querySphere({10.5f, 1.2f, -0.3f}, 0.25f, contacts);
	CHECK(contacts.size() == 1 && contacts[0].normal.y > 0.99f && std::fabs(contacts[0].depth + 0.05f) < 1e-4f);
	// Just behind the wall, the face (seen from behind) still reports a deep
	// contact; edges only touch from the front.
	contacts.clear();
	world.querySphere({-1, 2.2f, 3.1f}, 0.25f, contacts);
	CHECK(contacts.size() == 1 && contacts[0].normal.z < -0.99f && contacts[0].depth < -0.25f);
	contacts.clear();
	world.querySphere({2.1f, 1.5f, 3.1f}, 0.25f, contacts);
	CHECK(contacts.empty());

	// A soldier that starts behind the wall (in it, seen from the object's
	// side) walks on through it instead of being thrown back through the
	// face and held there.
	physics::SoldierBody body;
	body.position = {0, 1, 3.05f};
	body.world = &world;
	game::SoldierControl control;
	game::SoldierInput forward;
	forward.throttle = 1;
	for (int i = 0; i < 60; ++i) {
		control.update(forward, body);
		body.step(1 / 30.0f, slope(0));
	}
	CHECK(body.position.z > 8);
	CHECK(std::fabs(body.position.y - 1) < 0.1f);

	// A face overhead, facing down (a slab's underside at 1.14 m, the 4th
	// sphere's centre just above it): walking under and past it is free.
	physics::CollisionLod lod;
	lod.vertices = {{-3, 1.14f, 5}, {3, 1.14f, 5}, {3, 1.14f, 9}, {-3, 1.14f, 9}};
	lod.faces = {{{0, 2, 1}, 0}, {{0, 3, 2}, 0}};
	physics::CollisionSubGeometry part;
	part.lods = {lod, lod, lod};
	physics::CollisionGeometry geometry;
	geometry.subGeometries = {part};
	physics::CollisionMeshFile slab;
	slab.geometries = {geometry};
	physics::CollisionWorld under;
	under.addMesh(slab, physics::CollisionUser::Soldier, {}, {});
	contacts.clear();
	under.querySphere({-1.5f, 1.0f, 8}, 0.25f, contacts);
	CHECK(contacts.size() == 1 && contacts[0].normal.y < -0.99f);
	physics::SoldierBody walker;
	walker.position = {0, 1, 0};
	walker.world = &under;
	game::SoldierControl walking;
	for (int i = 0; i < 90; ++i) {
		walking.update(forward, walker);
		walker.step(1 / 30.0f, slope(0));
	}
	CHECK(walker.position.z > 10);
	CHECK(std::fabs(walker.position.y - 1) < 0.1f);
}

// A vehicle's collision vertex against static objects (0x006bb050 ->
// slot 22): its segment from the last pose crosses the box top from above:
// a contact at the crossing, normal up, as deep as the end lies below;
// from below (behind the face) nothing.
TEST(CollisionWorld_VertexSegmentContact)
{
	physics::CollisionWorld world;
	world.addMesh(testMesh(), physics::CollisionUser::Vehicle, {}, {});
	physics::SphereContact contact;
	CHECK(world.segmentContact({10, 1.2f, 0}, {10, 0.9f, 0}, contact));
	CHECK(std::fabs(contact.point.y - 1) < 1e-4f && contact.normal.y > 0.99f && std::fabs(contact.depth + 0.1f) < 1e-4f);
	CHECK(!world.segmentContact({10, 0.9f, 0}, {10, 1.2f, 0}, contact));
	CHECK(!world.segmentContact({10, 1.3f, 0}, {10, 1.1f, 0}, contact));
}

// A round against a bone's capsule (0x006bde70): the segment's crossings
// of the capsule (cylinder and end spheres), strictly inside the segment:
// in and out through it, only out from inside, none past it.
TEST(CollisionWorld_SegmentCapsuleCrossings)
{
	float roots[2];
	// The capsule along y from 0 to 1, radius 0.1; a round along x at y 0.5.
	CHECK(physics::segmentCapsuleCrossings({-1, 0.5f, 0}, {1, 0.5f, 0}, {0, 0, 0}, {0, 1, 0}, 0.1f, roots) == 2);
	CHECK(std::fabs(roots[0] - 0.45f) < 1e-4f && std::fabs(roots[1] - 0.55f) < 1e-4f);
	// Over the top: the end sphere.
	CHECK(physics::segmentCapsuleCrossings({-1, 1.05f, 0}, {1, 1.05f, 0}, {0, 0, 0}, {0, 1, 0}, 0.1f, roots) == 2);
	CHECK(std::fabs(roots[0] - (1 - std::sqrt(0.1f * 0.1f - 0.05f * 0.05f)) / 2) < 1e-4f);
	CHECK(physics::segmentCapsuleCrossings({0, 0.5f, 0}, {1, 0.5f, 0}, {0, 0, 0}, {0, 1, 0}, 0.1f, roots) == 1);
	CHECK(std::fabs(roots[0] - 0.1f) < 1e-4f);
	CHECK(physics::segmentCapsuleCrossings({-1, 1.2f, 0}, {1, 1.2f, 0}, {0, 0, 0}, {0, 1, 0}, 0.1f, roots) == 0);
	CHECK(physics::segmentCapsuleCrossings({-1, 0.5f, 0}, {-0.2f, 0.5f, 0}, {0, 0, 0}, {0, 1, 0}, 0.1f, roots) == 0);
}
