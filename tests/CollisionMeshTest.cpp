// Parses every collision mesh of the game. Needs $BF2142_GAME_DIR.
#include "IO/File/ZipArchive.h"
#include "Physics/CollisionMeshFile.h"
#include "Physics/CollisionWorld.h"
#include "TestMain.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

TEST(CollisionMesh_RetailLodFallback)
{
	using namespace dice::hfe::physics;
	for (const uint32_t version : {8u, 10u}) {
		CollisionMeshFile mesh {};
		mesh.version = version;
		CollisionSubGeometry part;
		CollisionLod projectile {}, vehicle {}, soldier {};
		projectile.collisionType = 0;
		vehicle.collisionType = 1;
		soldier.collisionType = 2;
		part.lods = {projectile, vehicle};
		CHECK(collisionLodFor(mesh, part, 0) == &part.lods[0]);
		CHECK(collisionLodFor(mesh, part, 1) == &part.lods[1]);
		CHECK(collisionLodFor(mesh, part, 2) == &part.lods[1]);
		CHECK(collisionLodFor(mesh, part, 3) == &part.lods[1]);
		CHECK(collisionLodFor(mesh, part, 4) == nullptr);
		part.lods.push_back(soldier);
		CHECK(collisionLodFor(mesh, part, 2) == &part.lods[2]);
		CHECK(collisionLodFor(mesh, part, 3) == &part.lods[2]);
		part.lods.resize(1);
		CHECK(collisionLodFor(mesh, part, 1) == nullptr);
		CHECK(collisionLodFor(mesh, part, 2) == nullptr);
		CHECK(collisionLodFor(mesh, part, 3) == nullptr);
	}
}

// CollisionWorld's segment queries walk only the grid cells a segment
// crosses: the same first face as testing every face, for long diagonal,
// axis-aligned and vertical segments, along cell borders and through cell
// corners, with an ignored owner and front-only crossings (segmentContact).
TEST(CollisionWorld_TraceMatchesEveryFace)
{
	using namespace dice::hfe::physics;
	struct Face {
		Vec3 a, b, c;
		uint32_t owner, material;
	};
	std::vector<Face> faces;
	CollisionWorld world;
	uint32_t seed = 12345;
	const auto rnd = [&seed](float lo, float hi) {
		seed = seed * 1664525u + 1013904223u;
		return lo + (hi - lo) * static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
	};
	const auto add = [&](uint32_t owner, const std::vector<std::array<Vec3, 3>>& triangles, uint16_t material) {
		CollisionMeshFile file {};
		file.version = 10;
		file.geometries.resize(1);
		file.geometries[0].subGeometries.resize(1);
		CollisionLod lod {};
		for (const auto& t : triangles) {
			const uint16_t first = static_cast<uint16_t>(lod.vertices.size());
			for (const Vec3& v : t) {
				lod.vertices.push_back({v.x, v.y, v.z});
			}
			lod.faces.push_back({{first, static_cast<uint16_t>(first + 1), static_cast<uint16_t>(first + 2)}, material});
			faces.push_back({t[0], t[1], t[2], owner, material});
		}
		file.geometries[0].subGeometries[0].lods.push_back(lod);
		world.addMesh(file, CollisionUser::Projectile, {}, {}, owner, {});
	};
	uint32_t owner = 1;
	for (; owner <= 40; ++owner) {
		std::vector<std::array<Vec3, 3>> triangles;
		for (int i = 0; i < 20; ++i) {
			const Vec3 centre {rnd(-120, 120), rnd(-10, 30), rnd(-120, 120)};
			std::array<Vec3, 3> t;
			for (Vec3& v : t) {
				v = centre + Vec3 {rnd(-12, 12), rnd(-12, 12), rnd(-12, 12)};
			}
			triangles.push_back(t);
		}
		add(owner, triangles, static_cast<uint16_t>(owner % 7));
	}
	// Walls lying on cell borders (x = 16, z = -8) and meeting at a cell corner, both windings.
	add(owner++, {{Vec3 {16, -5, -40}, Vec3 {16, 25, -40}, Vec3 {16, 25, 40}}, {Vec3 {16, -5, -40}, Vec3 {16, 25, 40}, Vec3 {16, -5, 40}}}, 3);
	add(owner++, {{Vec3 {-40, -5, -8}, Vec3 {40, -5, -8}, Vec3 {40, 25, -8}}, {Vec3 {-40, -5, -8}, Vec3 {40, 25, -8}, Vec3 {-40, 25, -8}}}, 4);
	// A floor whose corners sit on cell corners.
	add(owner++, {{Vec3 {-24, 2, -24}, Vec3 {24, 2, -24}, Vec3 {24, 2, 24}}, {Vec3 {-24, 2, -24}, Vec3 {24, 2, 24}, Vec3 {-24, 2, 24}}}, 5);

	// Every face, the trace's Moller-Trumbore and filters.
	const auto expected = [&](const Vec3& from, const Vec3& to, uint32_t ignore, bool frontOnly, float& best) -> const Face* {
		const Vec3 direction = to - from;
		const Face* first = nullptr;
		best = 1.0f;
		for (const Face& f : faces) {
			const Vec3 normal = cross(f.c - f.b, f.b - f.a);
			if ((ignore != 0 && f.owner == ignore) || (frontOnly && dot(direction, normal) >= 0)) {
				continue;
			}
			const Vec3 e1 = f.b - f.a, e2 = f.c - f.a, p = cross(direction, e2);
			const float determinant = dot(e1, p);
			if (std::fabs(determinant) < 1e-9f) {
				continue;
			}
			const float inverse = 1.0f / determinant;
			const Vec3 s = from - f.a;
			const float u = dot(s, p) * inverse;
			const Vec3 q = cross(s, e1);
			const float v = dot(direction, q) * inverse;
			const float fraction = dot(e2, q) * inverse;
			if (u < 0 || u > 1 || v < 0 || u + v > 1 || fraction < 0 || fraction >= best) {
				continue;
			}
			best = fraction;
			first = &f;
		}
		return first;
	};
	int mismatches = 0, hits = 0;
	const auto check = [&](const Vec3& from, const Vec3& to) {
		float best = 1;
		const Face* face = expected(from, to, 0, false, best);
		CollisionWorld::Hit hit {};
		const bool found = world.raycast(from, to, hit);
		hits += found;
		if (found != (face != nullptr) || (face && (std::fabs(hit.fraction - best) > 1e-6f || hit.owner != face->owner))) {
			++mismatches;
		}
		if (face) {
			// Its owner ignored: the next face along, from another object.
			const uint32_t ignore = face->owner;
			const Face* other = expected(from, to, ignore, false, best);
			const bool again = world.raycast(from, to, hit, ignore);
			if (again != (other != nullptr) || (other && (std::fabs(hit.fraction - best) > 1e-6f || hit.owner == ignore))) {
				++mismatches;
			}
		}
		const Face* front = expected(from, to, 0, true, best);
		// The scene ray's front-only test (raycastFront), also with the owner left out.
		if (world.raycastFront(from, to, hit) != (front != nullptr)
			|| (front && (std::fabs(hit.fraction - best) > 1e-6f || hit.owner != front->owner))) {
			++mismatches;
		}
		if (front) {
			float next = 1;
			const Face* other = expected(from, to, front->owner, true, next);
			if (world.raycastFront(from, to, hit, front->owner) != (other != nullptr)
				|| (other && (std::fabs(hit.fraction - next) > 1e-6f || hit.owner == front->owner))) {
				++mismatches;
			}
		}
		SphereContact contact {};
		const bool crossed = world.segmentContact(from, to, contact);
		const Vec3 at = from + (to - from) * best;
		if (crossed != (front != nullptr)
			|| (front && (contact.material != front->material || lengthSquared(contact.point - at) > 1e-6f))) {
			++mismatches;
		}
	};
	for (int i = 0; i < 3000; ++i) {
		const Vec3 from {rnd(-150, 150), rnd(-15, 35), rnd(-150, 150)};
		const Vec3 to {rnd(-150, 150), rnd(-15, 35), rnd(-150, 150)};
		check(from, to);
		check(to, from);
		// Vertical, along x and along z.
		check(Vec3 {from.x, 40, from.z}, Vec3 {from.x, -20, from.z});
		check(Vec3 {from.x, from.y, from.z}, Vec3 {to.x, from.y, from.z});
		check(Vec3 {from.x, from.y, from.z}, Vec3 {from.x, from.y, to.z});
	}
	for (int i = -20; i <= 20; ++i) {
		const float c = 8.0f * i;
		const float y = rnd(-5, 25);
		// On cell borders, through cell corners and straight down a corner.
		check(Vec3 {c, y, -150}, Vec3 {c, y, 150});
		check(Vec3 {150, y, c}, Vec3 {-150, y, c});
		check(Vec3 {-150, y, -150 + c}, Vec3 {150, rnd(-5, 25), 150 + c});
		check(Vec3 {c, 40, c}, Vec3 {c, -20, c});
		check(Vec3 {c, 40, -8}, Vec3 {c, -20, -8});
	}
	std::fprintf(stderr, "  trace: %d hits, %d mismatches\n", hits, mismatches);
	CHECK(hits > 1000);
	CHECK(mismatches == 0);
}

TEST(CollisionMesh_ParsesGameMeshes)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	int parsed = 0, failed = 0;
	for (const auto& entry : fs::recursive_directory_iterator(fs::path(gameDir) / "mods/bf2142")) {
		if (entry.path().extension() != ".zip") {
			continue;
		}
		auto zip = dice::hfe::io::ZipArchive::open(entry.path().string());
		if (!zip) {
			continue;
		}
		for (const std::string& name : zip->listRecursive("")) {
			if (name.size() < 14 || name.compare(name.size() - 14, 14, ".collisionmesh") != 0) {
				continue;
			}
			std::vector<uint8_t> data;
			zip->read(name, data);
			std::string error;
			if (dice::hfe::physics::parseCollisionMesh(data, &error)) {
				++parsed;
			} else {
				std::fprintf(stderr, "  rejected %s: %s\n", name.c_str(), error.c_str());
				++failed;
			}
		}
	}
	std::fprintf(stderr, "  %d collision meshes parsed, %d rejected\n", parsed, failed);
	CHECK(parsed >= 3762);
	CHECK(failed <= 3); // the shipped data has three malformed files
}

// The FAV's mesh: a material index per vertex (the LOD's +0xd8, the
// mesh's slot 0x44) and the vehicle LOD's radius (slot 0x50: its box's
// farthest corner from the origin, 0x006aeda0).
TEST(CollisionMesh_VertexMaterialsAndRadius)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	auto zip = dice::hfe::io::ZipArchive::open((fs::path(gameDir) / "mods/bf2142/Objects/Vehicles_server.zip").string());
	CHECK(zip != nullptr);
	if (!zip) {
		return;
	}
	std::vector<uint8_t> data;
	zip->read("as_fav/meshes/as_fav.collisionmesh", data);
	const auto mesh = dice::hfe::physics::parseCollisionMesh(data);
	CHECK(mesh.has_value());
	if (!mesh) {
		return;
	}
	const dice::hfe::physics::CollisionLod* lod = nullptr;
	for (const auto& part : mesh->geometries[0].subGeometries) {
		if (!lod) {
			lod = dice::hfe::physics::collisionLodFor(*mesh, part, 1);
		}
	}
	CHECK(lod != nullptr);
	if (!lod) {
		return;
	}
	CHECK(lod->vertexMaterials.size() == lod->vertices.size());
	const float radius = dice::hfe::physics::boxRadius(lod->boundsMin, lod->boundsMax);
	std::fprintf(stderr, "  as_fav: %zu vehicle-LOD vertices, radius %.3f\n", lod->vertices.size(), radius);
	CHECK(radius > 2.3f && radius < 2.7f);
	// A box's farthest corner, not its half diagonal.
	CHECK(std::abs(dice::hfe::physics::boxRadius({0, 0, 0}, {3, 4, 0}) - 5) < 1e-5f);
}

