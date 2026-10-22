#include "CollisionWorld.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::physics {

namespace {

// The point of triangle abc closest to p (Ericson, Real-Time Collision
// Detection 5.1.5).
Vec3 closestOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c)
{
	const Vec3 ab = b - a, ac = c - a, ap = p - a;
	const float d1 = dot(ab, ap), d2 = dot(ac, ap);
	if (d1 <= 0 && d2 <= 0) {
		return a;
	}
	const Vec3 bp = p - b;
	const float d3 = dot(ab, bp), d4 = dot(ac, bp);
	if (d3 >= 0 && d4 <= d3) {
		return b;
	}
	const float vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) {
		return a + ab * (d1 / (d1 - d3));
	}
	const Vec3 cp = p - c;
	const float d5 = dot(ab, cp), d6 = dot(ac, cp);
	if (d6 >= 0 && d5 <= d6) {
		return c;
	}
	const float vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) {
		return a + ac * (d2 / (d2 - d6));
	}
	const float va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
		return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
	}
	const float denominator = 1.0f / (va + vb + vc);
	return a + ab * (vb * denominator) + ac * (vc * denominator);
}

} // namespace

void CollisionWorld::addMesh(const CollisionMeshFile& mesh, CollisionUser user, const Mat3& rotation,
	const Vec3& position, uint32_t owner, const std::vector<uint32_t>& materials)
{
	if (mesh.geometries.empty()) {
		return;
	}
	for (const CollisionSubGeometry& part : mesh.geometries[0].subGeometries) {
		const CollisionLod* found = collisionLodFor(mesh, part, static_cast<uint32_t>(user));
		if (!found) {
			continue;
		}
		const CollisionLod& lod = *found;
		for (const CollisionFace& face : lod.faces) {
			Triangle t;
			Vec3 corners[3];
			for (int i = 0; i < 3; ++i) {
				const auto& v = lod.vertices[face.vertices[static_cast<size_t>(i)]];
				corners[i] = rotation.transform({v[0], v[1], v[2]}) + position;
			}
			t.a = corners[0];
			t.b = corners[1];
			t.c = corners[2];
			// The face's normal (0x006e8080): (c - b) x (b - a). Faces are
			// wound clockwise seen from the front, so this points out of the
			// object (up on a floor).
			Vec3 n = cross(t.c - t.b, t.b - t.a);
			const float length = std::sqrt(lengthSquared(n));
			if (length < 1e-8f) {
				continue; // degenerate
			}
			t.normal = n * (1.0f / length);
			t.material = face.material < materials.size() ? materials[face.material] : face.material;
			t.owner = owner;
			const uint32_t index = static_cast<uint32_t>(m_triangles.size());
			m_triangles.push_back(t);
			const float minX = std::min({t.a.x, t.b.x, t.c.x}), maxX = std::max({t.a.x, t.b.x, t.c.x});
			const float minZ = std::min({t.a.z, t.b.z, t.c.z}), maxZ = std::max({t.a.z, t.b.z, t.c.z});
			for (int x = static_cast<int>(std::floor(minX / kCell)); x <= static_cast<int>(std::floor(maxX / kCell)); ++x) {
				for (int z = static_cast<int>(std::floor(minZ / kCell)); z <= static_cast<int>(std::floor(maxZ / kCell));
					 ++z) {
					m_cells[cellKey(x, z)].push_back(index);
				}
			}
		}
	}
}

bool CollisionWorld::raycast(const Vec3& from, const Vec3& to, Hit& hit) const
{
	++m_stamp;
	const Vec3 direction = to - from;
	bool found = false;
	hit.fraction = 1.0f;
	// Walk the cells the segment's bounding box covers (segments are short:
	// one tick of a projectile).
	const int x0 = static_cast<int>(std::floor(std::min(from.x, to.x) / kCell));
	const int x1 = static_cast<int>(std::floor(std::max(from.x, to.x) / kCell));
	const int z0 = static_cast<int>(std::floor(std::min(from.z, to.z) / kCell));
	const int z1 = static_cast<int>(std::floor(std::max(from.z, to.z) / kCell));
	for (int x = x0; x <= x1; ++x) {
		for (int z = z0; z <= z1; ++z) {
			const auto cell = m_cells.find(cellKey(x, z));
			if (cell == m_cells.end()) {
				continue;
			}
			for (uint32_t index : cell->second) {
				const Triangle& t = m_triangles[index];
				if (t.stamp == m_stamp) {
					continue;
				}
				t.stamp = m_stamp;
				// Moller-Trumbore, both sides.
				const Vec3 e1 = t.b - t.a, e2 = t.c - t.a;
				const Vec3 p = cross(direction, e2);
				const float determinant = dot(e1, p);
				if (std::fabs(determinant) < 1e-9f) {
					continue;
				}
				const float inverse = 1.0f / determinant;
				const Vec3 s = from - t.a;
				const float u = dot(s, p) * inverse;
				if (u < 0 || u > 1) {
					continue;
				}
				const Vec3 q = cross(s, e1);
				const float v = dot(direction, q) * inverse;
				if (v < 0 || u + v > 1) {
					continue;
				}
				const float fraction = dot(e2, q) * inverse;
				if (fraction < 0 || fraction >= hit.fraction) {
					continue;
				}
				hit.fraction = fraction;
				hit.point = from + direction * fraction;
				hit.normal = dot(direction, t.normal) > 0 ? -t.normal : t.normal;
				hit.material = t.material;
				hit.owner = t.owner;
				found = true;
			}
		}
	}
	return found;
}

void CollisionWorld::querySphere(const Vec3& center, float radius, std::vector<SphereContact>& contacts) const
{
	++m_stamp;
	const int x0 = static_cast<int>(std::floor((center.x - radius) / kCell));
	const int x1 = static_cast<int>(std::floor((center.x + radius) / kCell));
	const int z0 = static_cast<int>(std::floor((center.z - radius) / kCell));
	const int z1 = static_cast<int>(std::floor((center.z + radius) / kCell));
	for (int x = x0; x <= x1; ++x) {
		for (int z = z0; z <= z1; ++z) {
			const auto cell = m_cells.find(cellKey(x, z));
			if (cell == m_cells.end()) {
				continue;
			}
			for (uint32_t index : cell->second) {
				const Triangle& t = m_triangles[index];
				if (t.stamp == m_stamp) {
					continue;
				}
				t.stamp = m_stamp;
				// The sphere against one face (0x006bf010): the centre within a
				// radius of the face's plane; inside the face it touches the
				// face (the face's normal, also from behind); outside it, an edge
				// or corner, from the front only (0x00a84330 and 0x00a84334 are
				// set).
				const float distance = dot(center - t.a, t.normal);
				if (std::fabs(distance) > radius) {
					continue;
				}
				const Vec3 onPlane = center - t.normal * distance;
				const Vec3 closest = closestOnTriangle(onPlane, t.a, t.b, t.c);
				Vec3 normal;
				float depth;
				if (lengthSquared(closest - onPlane) <= 1e-8f) {
					normal = t.normal;
					depth = distance - radius;
				} else {
					if (distance < 0) {
						continue;
					}
					const Vec3 away = center - closest;
					const float squared = lengthSquared(away);
					if (squared > radius * radius || squared <= 0) {
						continue;
					}
					const float length = std::sqrt(squared);
					normal = away * (1.0f / length);
					depth = length - radius;
				}
				contacts.push_back({closest, normal, depth, t.material});
			}
		}
	}
}

} // namespace dice::hfe::physics
