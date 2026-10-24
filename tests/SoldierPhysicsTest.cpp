// Soldier movement: standing on the ground, walking, running up to speed.
#include "Game/SoldierControl.h"
#include "Physics/SoldierPhysics.h"
#include "TestMain.h"

#include <algorithm>
#include <array>
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

// A vehicle's hull edge against static objects (0x006bb050's edge pass):
// through the wall from the front only: the crossing, the wall's normal;
// through a slab (its front and its back): the middle, and with the two
// normals cancelling, from there towards the vehicle's origin.
TEST(CollisionWorld_EdgeContact)
{
	physics::CollisionWorld world;
	world.addMesh(testMesh(), physics::CollisionUser::Vehicle, {}, {});
	physics::Vec3 point, normal;
	CHECK(world.edgeContact({0, 1, 2}, {0, 1, 4}, {0, 1, 0}, 0.1f, point, normal));
	CHECK(std::fabs(point.z - 3) < 1e-4f && normal.z < -0.99f);
	CHECK(!world.edgeContact({0, 1, 4}, {0, 1, 2}, {0, 1, 0}, 0.1f, point, normal)); // from behind
	CHECK(!world.edgeContact({5, 1, 2}, {5, 1, 4}, {0, 1, 0}, 0.1f, point, normal));  // beside it
	// A slab: the wall and a back face at z = 3.5 facing +z.
	physics::CollisionMeshFile slab = testMesh();
	physics::CollisionLod& lod = slab.geometries[0].subGeometries[0].lods[1];
	const uint16_t i = static_cast<uint16_t>(lod.vertices.size());
	for (const std::array<float, 3>& v : {std::array<float, 3> {-2, 0, 3.5f}, {-2, 3, 3.5f}, {2, 3, 3.5f}, {2, 0, 3.5f}}) {
		lod.vertices.push_back(v);
	}
	lod.faces.push_back({{i, static_cast<uint16_t>(i + 1), static_cast<uint16_t>(i + 2)}, 0});
	lod.faces.push_back({{i, static_cast<uint16_t>(i + 2), static_cast<uint16_t>(i + 3)}, 0});
	physics::CollisionWorld both;
	both.addMesh(slab, physics::CollisionUser::Vehicle, {}, {});
	physics::SphereContact back;
	CHECK(both.segmentContact({0, 1, 4.5f}, {0, 1, 2}, back) && back.normal.z > 0.99f);
	CHECK(both.edgeContact({0, 1, 2}, {0, 1, 4.5f}, {0, 1, 0}, 0.1f, point, normal));
	CHECK(std::fabs(point.z - 3.25f) < 1e-4f && normal.z < -0.99f && std::fabs(normal.y) < 1e-6f);
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

// The look (0x0058a6d0): standing, a turn twists the head and the body
// stays until the twist reaches 40 degrees (*0x00a68eb0); then the body
// catches the twist up at 4 degrees a tick (*0x00a68ee0). Moving (the
// throttle; strafing doesn't count), any twist is caught up.
TEST(SoldierControl_HeadTwistsBeforeTheBodyTurns)
{
	game::SoldierControl control;
	physics::SoldierBody body;
	game::SoldierInput look;
	look.turn = 10;
	for (int i = 0; i < 3; ++i) {
		control.update(look, body);
	}
	CHECK(body.yawDegrees == 0 && control.twist() == 30); // the head alone
	game::SoldierInput still;
	control.update(still, body);
	CHECK(body.yawDegrees == 0 && control.twist() == 30); // within the limit it stays twisted
	look.turn = 15; // 45: past the limit
	control.update(look, body);
	CHECK(std::fabs(body.yawDegrees - 19) < 1e-4f && std::fabs(control.twist() - 41) < 1e-4f); // the turn and 4
	for (int i = 0; i < 20; ++i) {
		control.update(still, body);
	}
	CHECK(std::fabs(control.twist()) < 1e-4f && std::fabs(body.yawDegrees - 60) < 1e-4f); // all caught up
	// Twisted, then walking forward: caught up at 4 a tick.
	look.turn = 20;
	control.update(look, body);
	CHECK(std::fabs(body.yawDegrees - 60) < 1e-4f && std::fabs(control.twist() - 20) < 1e-4f);
	game::SoldierInput walk;
	walk.throttle = 1;
	control.update(walk, body);
	CHECK(std::fabs(body.yawDegrees - 64) < 1e-4f && std::fabs(control.twist() - 16) < 1e-4f);
	game::SoldierInput strafe;
	strafe.strafe = 1;
	control.reset();
	look.turn = 20;
	control.update(look, body);
	control.update(strafe, body);
	CHECK(std::fabs(control.twist() - 20) < 1e-4f); // strafing: still twisted
}

// The spawn scatter's stance test (0x00585d00 -> 0x006c3340 with the stack
// of 0x006c1b30): on the box (top at 1 m) a soldier fits standing; sunk in
// it, not even prone; under a slab 1.2 m over the floor only prone.
TEST(SoldierPhysics_StanceFitsTheSpot)
{
	physics::CollisionWorld world;
	world.addMesh(testMesh(), physics::CollisionUser::Soldier, {}, {});
	CHECK(physics::soldierFits(world, {10, 2.01f, 0}, 0));  // feet on the box top
	CHECK(!physics::soldierFits(world, {10, 1.5f, 0}, 0));  // in it
	CHECK(!physics::soldierFits(world, {10, 1.5f, 0}, 2));
	CHECK(physics::soldierFits(world, {0, 1, 0}, 0));       // clear of the wall (z = 3)
	CHECK(!physics::soldierFits(world, {0, 1, 2.9f}, 0));   // against it
	// A slab facing down at 1.2 m over a 4 x 4 m floor at x 20.
	physics::CollisionLod lod;
	lod.vertices = {{18, 1.2f, -2}, {22, 1.2f, -2}, {22, 1.2f, 2}, {18, 1.2f, 2}};
	lod.faces = {{{0, 1, 2}, 0}, {{0, 2, 3}, 0}};
	physics::CollisionSubGeometry part;
	part.lods = {lod, lod, lod};
	physics::CollisionGeometry geometry;
	geometry.subGeometries = {part};
	physics::CollisionMeshFile slab;
	slab.geometries = {geometry};
	physics::CollisionWorld low;
	low.addMesh(slab, physics::CollisionUser::Soldier, {}, {});
	CHECK(!physics::soldierFits(low, {20, 1, 0}, 0)); // standing: the upper spheres reach it
	CHECK(!physics::soldierFits(low, {20, 1, 0}, 1)); // crouching too (1.4 m)
	CHECK(physics::soldierFits(low, {20, 1, 0}, 2));  // lying fits
	// The same mesh in a vehicle's frame: turned and moved.
	const physics::Mat3 turned = physics::Mat3::fromYawPitchRoll({90, 0, 0});
	CHECK(!physics::soldierFits(low, {50 + turned.transform({20, 0, 0}).x, 1, turned.transform({20, 0, 0}).z}, 0, turned,
		{50, 0, 0}));
}

namespace {

// A wall in the x-y plane at z = 3 facing -z, from `low` to 3 m up.
dice::hfe::physics::CollisionMeshFile wallFrom(float low)
{
	dice::hfe::physics::CollisionLod lod;
	lod.vertices = {{-2, low, 3}, {2, 3, 3}, {-2, 3, 3}, {2, low, 3}};
	lod.faces = {{{0, 1, 2}, 0}, {{0, 3, 1}, 0}};
	dice::hfe::physics::CollisionSubGeometry part;
	part.lods = {lod, lod, lod};
	dice::hfe::physics::CollisionGeometry geometry;
	geometry.subGeometries = {part};
	dice::hfe::physics::CollisionMeshFile mesh;
	mesh.geometries = {geometry};
	return mesh;
}

} // namespace

// The swept sphere (0x006e2350 with a motion, 0x006be1a0): a sphere moving
// through a face from the front meets it where its centre crosses the face
// pushed out by the radius; one standing behind the face (where a fast
// one lands) is not pushed back through it.
TEST(CollisionWorld_SweptSphere)
{
	physics::CollisionWorld world;
	world.addMesh(wallFrom(0), physics::CollisionUser::Soldier, {}, {});
	std::vector<physics::SphereContact> contacts;
	world.sweepSphere({0, 1, 2.2f}, {0, 1, 3.1f}, 0.25f, contacts);
	CHECK(contacts.size() == 1);
	CHECK(!contacts.empty() && contacts[0].normal.z < -0.99f);
	// The end lies 0.35 behind the plane pushed out by the radius (z 2.75).
	CHECK(!contacts.empty() && std::fabs(contacts[0].depth + 0.35f) < 1e-4f);
	contacts.clear();
	world.querySphere({0, 1, 3.1f}, 0.25f, contacts);
	CHECK(contacts.size() == 1 && contacts[0].depth < 0 && contacts[0].point.z == 3); // from behind: the face's normal
	contacts.clear();
	world.sweepSphere({0, 1, 3.2f}, {0, 1, 2.2f}, 0.25f, contacts); // from behind: nothing
	CHECK(contacts.empty());
}

// Over 0.9 of a radius a tick (6.75 m/s) the soldier's spheres sweep from
// the last tick's place (0x006c3890): a soldier thrown at a wall at 40 m/s
// stops at it instead of passing through between two ticks.
TEST(SoldierPhysics_FastSoldiersDoNotPassWalls)
{
	physics::CollisionWorld world;
	world.addMesh(wallFrom(0), physics::CollisionUser::Soldier, {}, {});
	physics::SoldierBody body;
	body.position = {0, 1, 0.3f};
	body.world = &world;
	body.step(1 / 30.0f, slope(0));
	body.velocity = {0, 0, 40};
	for (int i = 0; i < 30; ++i) {
		body.step(1 / 30.0f, slope(0));
	}
	std::fprintf(stderr, "  the soldier ends at z %.2f (the wall at 3)\n", body.position.z);
	CHECK(body.position.z < 3);
}

// Prone the stack is one sphere at the feet (0x006c1b30): a soldier lying
// down crawls under an overhang 0.6 m up that stops it standing.
TEST(SoldierPhysics_ProneCrawlsUnderAnOverhang)
{
	physics::CollisionWorld world;
	world.addMesh(wallFrom(0.6f), physics::CollisionUser::Soldier, {}, {});
	float ends[2] = {};
	for (int prone = 0; prone < 2; ++prone) {
		physics::SoldierBody body;
		body.position = {0, 1, 1.5f};
		body.world = &world;
		game::SoldierControl control;
		game::SoldierInput forward;
		forward.throttle = 1;
		forward.pose = prone ? game::SoldierPose::Prone : game::SoldierPose::Stand;
		body.stance = static_cast<int>(forward.pose);
		for (int i = 0; i < 120; ++i) {
			control.update(forward, body);
			body.step(1 / 30.0f, slope(0));
		}
		ends[prone] = body.position.z;
	}
	std::fprintf(stderr, "  standing ends at z %.2f, prone at %.2f\n", ends[0], ends[1]);
	CHECK(ends[0] < 3);
	CHECK(ends[1] > 3.5f);
}

// Water (0x006c28c0): deeper than 0.3 m the water lifts the soldier, level
// with its feet a metre under (its origin at the surface); from 0.99 m it
// swims (the ground flat, friction 1) until under 0.9 m.
TEST(SoldierPhysics_SwimsInDeepWater)
{
	physics::Ground ground = slope(0);
	ground.height = [](float, float) { return -5.0f; };
	ground.heightAndNormal = [](float, float, physics::Vec3& normal) {
		normal = {0, 1, 0};
		return -5.0f;
	};
	ground.waterLevel = [](float, float) { return 0.0f; };
	physics::SoldierBody body;
	body.position = {0, -1, 0};
	game::SoldierControl control;
	game::SoldierInput forward;
	forward.throttle = 1;
	forward.pose = game::SoldierPose::Swim;
	for (int i = 0; i < 150; ++i) {
		control.update(forward, body);
		body.step(1 / 30.0f, ground);
	}
	std::fprintf(stderr, "  the swimmer's origin at %.2f, %.2f m deep, %.2f m/s\n", body.position.y, body.waterDepth,
		body.velocity.z);
	CHECK(body.swimming());
	CHECK(std::fabs(body.position.y) < 0.3f);
	CHECK(body.velocity.z > 1.5f && body.velocity.z < 2.3f); // swim speed 2.1
	// On land again, no swimming.
	body.position = {0, 5, 0};
	physics::SoldierBody dry;
	dry.position = {0, 2, 0};
	for (int i = 0; i < 30; ++i) {
		dry.step(1 / 30.0f, slope(0));
	}
	CHECK(!dry.swimming() && dry.waterDepth == 0);
}

// The jump (0x0058a6d0): standing on the ground, PIAction sets the velocity
// to the ground speed x 0.98 and 6 m/s up; crouching it does nothing; it
// costs the template's sprintLossAtJump of stamina.
TEST(SoldierControl_Jumps)
{
	physics::SoldierBody body;
	body.position = {0, 1, 0};
	for (int i = 0; i < 10; ++i) {
		body.step(1 / 30.0f, slope(0));
	}
	game::SoldierControl control;
	control.stamina().configure(12, 17, 0.1f);
	control.jumpLoss = 0.25f;
	game::SoldierInput jump;
	jump.jump = true;
	jump.pose = game::SoldierPose::Crouch;
	control.update(jump, body);
	CHECK(!control.jumped());
	jump.pose = game::SoldierPose::Stand;
	control.update(jump, body);
	CHECK(control.jumped());
	CHECK(std::fabs(control.stamina().stamina() - 0.75f) < 1e-4f);
	body.step(1 / 30.0f, slope(0));
	CHECK(body.velocity.y > 5.5f);
	float peak = 0;
	game::SoldierInput idle;
	bool landed = false;
	for (int i = 0; i < 60; ++i) {
		control.update(idle, body);
		body.step(1 / 30.0f, slope(0));
		peak = std::max(peak, body.position.y - 1);
		landed = landed || body.onGround();
	}
	std::printf("  jumped %.2f m high\n", peak);
	CHECK(peak > 1.2f && peak < 2.0f);
	CHECK(landed && std::fabs(body.position.y - 1) < 0.1f);
}

// The sprint's stamina (0x006457f0): it drains over sprintDissipationTime
// and the sprint ends when it is out; it starts again only from the limit.
TEST(SoldierControl_SprintStaminaRunsOut)
{
	game::SprintStamina stamina;
	stamina.configure(1, 2, 0.5f);
	stamina.request(false);
	stamina.update(false, 0, 1 / 30.0f);
	CHECK(stamina.sprinting());
	for (int i = 0; i < 31; ++i) {
		stamina.request(true);
		stamina.update(false, 0, 1 / 30.0f);
	}
	CHECK(!stamina.sprinting() && stamina.stamina() < 0.02f);
	stamina.request(false);
	stamina.update(false, 0, 1 / 30.0f);
	CHECK(!stamina.sprinting()); // under the limit
	for (int i = 0; i < 31; ++i) {
		stamina.update(false, 0, 1 / 30.0f);
	}
	stamina.request(false);
	stamina.update(false, 0, 1 / 30.0f);
	CHECK(stamina.sprinting());
	// Crouching (blocked) ends it.
	stamina.request(true);
	stamina.update(true, 0, 1 / 30.0f);
	CHECK(!stamina.sprinting());
}

// Two soldiers closer than two radii push each other apart (0x006c2620).
TEST(SoldierPhysics_SoldiersPushApart)
{
	physics::SoldierBody a;
	a.position = {0, 1, 0};
	a.step(1 / 30.0f, slope(0));
	a.soldiers = {{{0.3f, 1, 0}, 0}};
	a.step(1 / 30.0f, slope(0));
	CHECK(a.position.x < -0.15f);
	// Lying under the other's feet (heights apart) nothing.
	physics::SoldierBody b;
	b.position = {0, 1, 0};
	b.step(1 / 30.0f, slope(0));
	b.soldiers = {{{0.3f, 3, 0}, 0}};
	b.step(1 / 30.0f, slope(0));
	CHECK(std::fabs(b.position.x) < 1e-4f);
}

namespace {

// A box from `low` to `high` with its faces out.
physics::CollisionMeshFile boxMesh(physics::Vec3 low, physics::Vec3 high)
{
	physics::CollisionLod lod;
	const auto quad = [&lod](physics::Vec3 a, physics::Vec3 b, physics::Vec3 c, physics::Vec3 d) {
		// Faces a b c and a c d: (c - b) x (b - a) points out for a quad
		// listed clockwise seen from outside.
		for (const physics::Vec3& v : {a, b, c, d}) {
			lod.vertices.push_back({v.x, v.y, v.z});
		}
		const uint16_t i = static_cast<uint16_t>(lod.vertices.size() - 4);
		lod.faces.push_back({{i, static_cast<uint16_t>(i + 1), static_cast<uint16_t>(i + 2)}, 0});
		lod.faces.push_back({{i, static_cast<uint16_t>(i + 2), static_cast<uint16_t>(i + 3)}, 0});
	};
	const physics::Vec3 l = low, h = high;
	quad({l.x, l.y, l.z}, {h.x, l.y, l.z}, {h.x, h.y, l.z}, {l.x, h.y, l.z}); // -z
	quad({h.x, l.y, h.z}, {l.x, l.y, h.z}, {l.x, h.y, h.z}, {h.x, h.y, h.z}); // +z
	quad({l.x, l.y, h.z}, {l.x, l.y, l.z}, {l.x, h.y, l.z}, {l.x, h.y, h.z}); // -x
	quad({h.x, l.y, l.z}, {h.x, l.y, h.z}, {h.x, h.y, h.z}, {h.x, h.y, l.z}); // +x
	quad({l.x, h.y, l.z}, {h.x, h.y, l.z}, {h.x, h.y, h.z}, {l.x, h.y, h.z}); // +y
	physics::CollisionSubGeometry part;
	part.lods = {lod, lod, lod};
	physics::CollisionGeometry geometry;
	geometry.subGeometries = {part};
	physics::CollisionMeshFile mesh;
	mesh.geometries = {geometry};
	return mesh;
}

} // namespace

// The solve (0x006c48c0): a soldier caught between a wall and a heavy
// moving object closer than its width can't be pushed out in eight
// passes: it is put back where it was and, the object moving at it faster
// than 3 m/s with the crush 0.05 m deep, killed with the object's velocity
// x 2.5 for its ragdoll.
TEST(SoldierPhysics_HeavyObjectsCrush)
{
	physics::CollisionWorld wall;
	wall.addMesh(boxMesh({-2, 0, 0.15f}, {2, 3, 1.15f}), physics::CollisionUser::Soldier, {}, {});
	physics::CollisionWorld slab;
	slab.addMesh(boxMesh({-2, 0, -1.15f}, {2, 3, -0.15f}), physics::CollisionUser::Soldier, {}, {});
	physics::SoldierBody body;
	body.position = {0, 1, 0};
	body.step(1 / 30.0f, slope(0));
	body.world = &wall;
	physics::SoldierBody::Mover tank;
	tank.mesh = &slab;
	tank.velocity = {0, 0, 4};
	tank.centerOfMass = {0, 1, -0.65f};
	tank.owner = 7;
	tank.mass = 20000;
	body.movers = {tank};
	uint32_t crushedBy = 0;
	physics::Vec3 push;
	body.onCrushed = [&crushedBy](uint32_t by) { crushedBy = by; };
	body.onPush = [&push](const physics::Vec3& p) { push = p; };
	body.step(1 / 30.0f, slope(0));
	CHECK(body.squashed());
	CHECK(crushedBy == 7);
	CHECK(std::fabs(push.z - 10) < 1e-3f);
	CHECK(std::fabs(body.position.z) < 0.05f);
	// Light (100 or less), it only squashes.
	crushedBy = 0;
	body.movers[0].mass = 100;
	body.step(1 / 30.0f, slope(0));
	CHECK(crushedBy == 0 && body.squashed());
}

// After a free solve, a heavy object's soldier LOD within half a radius of
// a sphere's centre (from inside it, where its faces don't push) at over
// 3 m/s runs the soldier over (0x006c2e50).
TEST(SoldierPhysics_RunOver)
{
	physics::CollisionWorld hull;
	hull.addMesh(boxMesh({-0.1f, 0, -1}, {2, 3, 1}), physics::CollisionUser::Soldier, {}, {});
	physics::SoldierBody body;
	body.position = {0, 1, 0};
	body.step(1 / 30.0f, slope(0));
	physics::SoldierBody::Mover car;
	car.mesh = &hull;
	car.velocity = {0, 0, -5}; // along its side
	car.owner = 3;
	car.mass = 1500;
	uint32_t by = 0;
	body.onCrushed = [&by](uint32_t who) { by = who; };
	body.movers = {car};
	body.step(1 / 30.0f, slope(0));
	CHECK(by == 3 && !body.squashed());
	by = 0;
	body.movers[0].velocity = {0, 0, -2}; // slower than 3 m/s
	body.step(1 / 30.0f, slope(0));
	CHECK(by == 0);
}

// The server's segment test (0x006c48c0): the origin's way crossing a face
// of a soldier LOD from the front kills, even where the spheres pass (a
// prone soldier under a wall 0.6 m up).
TEST(SoldierPhysics_SegmentThroughAMeshKills)
{
	physics::CollisionWorld world;
	world.addMesh(wallFrom(0.6f), physics::CollisionUser::Soldier, {}, {});
	physics::SoldierBody body;
	body.position = {0, 1, 2.8f};
	body.stance = 2;
	body.world = &world;
	body.step(1 / 30.0f, slope(0));
	int killed = 0;
	body.onCrushed = [&killed](uint32_t by) { killed += by == 0 ? 1 : 100; };
	body.velocity = {0, 0, 10};
	for (int i = 0; i < 10 && killed == 0; ++i) {
		body.step(1 / 30.0f, slope(0));
	}
	CHECK(killed == 1);
}

// The run-over check's team filter (0x006c2e50's start): the soldier's
// team (its PlayerControlObject's slot 0x9c) passes an object with its
// team's collisionGroups bit (0x100 team 1, 0x200 team 2), and every team
// one with 0x10.
TEST(SoldierPhysics_RunOverTeamFilter)
{
	physics::CollisionWorld hull;
	hull.addMesh(boxMesh({-0.1f, 0, -1}, {2, 3, 1}), physics::CollisionUser::Soldier, {}, {});
	struct Case {
		int team;
		uint32_t groups;
		bool killed;
	};
	for (const Case& c : {Case {1, 0x100, false}, Case {2, 0x100, true}, Case {2, 0x200, false}, Case {1, 0x10, false},
			 Case {0, 0x300, true}}) {
		const int team = c.team;
		const uint32_t groups = c.groups;
		const bool killed = c.killed;
		physics::SoldierBody body;
		body.position = {0, 1, 0};
		body.step(1 / 30.0f, slope(0));
		physics::SoldierBody::Mover car;
		car.mesh = &hull;
		car.velocity = {0, 0, -5};
		car.owner = 3;
		car.mass = 1500;
		car.collisionGroups = groups;
		uint32_t by = 0;
		body.team = team;
		body.onCrushed = [&by](uint32_t who) { by = who; };
		body.movers = {car};
		body.step(1 / 30.0f, slope(0));
		CHECK((by == 3) == killed);
	}
}

// A projectile that ignores its launcher (noInitialCollisionWithLauncher:
// 0x005b48b0 -> 0x006b8de0) doesn't stop it, and touching keeps the
// window open (0x006b8da0, then 0x006b8de0 again); another soldier is
// stopped. Small GenericProjectiles (soldier LOD under 0.6 m) are pushed
// instead of stopping anyone but their own launcher, who neither stops nor
// pushes them (0x006c3890's 0x9539 branch).
TEST(SoldierPhysics_IgnoredAndPushedObjects)
{
	physics::CollisionWorld box;
	box.addMesh(boxMesh({-1, 0, 0.5f}, {1, 2, 1.5f}), physics::CollisionUser::Soldier, {}, {});
	const auto walk = [&](physics::SoldierBody::Mover mover, int& ignored, int& pushes, physics::Vec3& push) {
		physics::SoldierBody body;
		body.position = {0, 1, 0};
		body.step(1 / 30.0f, slope(0));
		body.movers = {mover};
		body.onIgnored = [&ignored](uint32_t owner) { ignored += owner == 9 ? 1 : 100; };
		body.onPushObject = [&](uint32_t owner, const physics::Vec3&, const physics::Vec3&, const physics::Vec3& normal,
								float depth, uint32_t, uint32_t) {
			if (pushes == 0) {
				push = normal * -depth; // the first: against the near face
			}
			pushes += owner == 9 ? 1 : 100;
		};
		game::SoldierControl control;
		game::SoldierInput forward;
		forward.throttle = 1;
		for (int i = 0; i < 30; ++i) {
			control.update(forward, body);
			body.step(1 / 30.0f, slope(0));
		}
		return body.position.z;
	};
	physics::SoldierBody::Mover mover;
	mover.mesh = &box;
	mover.owner = 9;
	mover.mass = 50;
	int ignored = 0, pushes = 0;
	physics::Vec3 push;
	// Stopped at the box's face (z 0.5) by its 0.25 m radius.
	CHECK(walk(mover, ignored, pushes, push) < 0.3f && ignored == 0 && pushes == 0);
	mover.ignoring = mover.ignoresThis = true;
	CHECK(walk(mover, ignored, pushes, push) > 0.6f && ignored > 0 && pushes == 0);
	ignored = 0;
	mover.ignoresThis = false; // another soldier's: only the passes skip it
	CHECK(walk(mover, ignored, pushes, push) < 0.3f && ignored == 0);
	mover.ignoring = false;
	mover.pushed = true;
	CHECK(walk(mover, ignored, pushes, push) > 0.6f && pushes > 0);
	CHECK(push.z > 0); // away from the soldier, into +z
	pushes = 0;
	mover.own = true;
	CHECK(walk(mover, ignored, pushes, push) > 0.6f && pushes == 0);
}
