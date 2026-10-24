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
	// Geometry 0's normal sub-geometry: the instance's (+0x14, state 0).
	const CollisionSubGeometry* part = collisionPart(mesh, 0);
	const CollisionLod* found = part ? collisionLodFor(mesh, *part, static_cast<uint32_t>(user)) : nullptr;
	if (found) {
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

bool CollisionWorld::raycast(const Vec3& from, const Vec3& to, Hit& hit) const { return trace(from, to, hit, false); }

bool CollisionWorld::trace(const Vec3& from, const Vec3& to, Hit& hit, bool frontOnly, uint32_t ignore) const
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
				if (ignore != 0 && t.owner == ignore) {
					continue;
				}
				if (frontOnly && dot(direction, t.normal) >= 0) {
					continue;
				}
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
				hit.faceNormal = t.normal;
				hit.material = t.material;
				hit.owner = t.owner;
				found = true;
			}
		}
	}
	return found;
}

void CollisionWorld::forEachFaceNear(const Vec3& center, float radius,
	const std::function<void(const Vec3& a, const Vec3& b, const Vec3& c, uint32_t material, uint32_t owner)>& visit) const
{
	forEachFaceIndexNear(center, radius, [&](const Triangle& t) { visit(t.a, t.b, t.c, t.material, t.owner); });
}

void CollisionWorld::forEachFaceIndexNear(const Vec3& center, float radius,
	const std::function<void(const Triangle&)>& visit) const
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
				if (t.stamp != m_stamp) {
					t.stamp = m_stamp;
					visit(t);
				}
			}
		}
	}
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
				contacts.push_back({closest, normal, depth, t.material, depth});
			}
		}
	}
}

bool CollisionWorld::edgeContact(const Vec3& a, const Vec3& b, const Vec3& origin, float minNormal, Vec3& point,
	Vec3& normal) const
{
	SphereContact first, second;
	if (!segmentContact(a, b, first)) {
		return false;
	}
	point = first.point;
	normal = first.normal;
	if (segmentContact(b, a, second)) {
		point = (first.point + second.point) * 0.5f;
		normal = first.normal + second.normal;
		normal.y = 0;
		if (lengthSquared(normal) < minNormal * minNormal) {
			normal = origin - point;
		}
		const float length = std::sqrt(lengthSquared(normal));
		normal = length > 0 ? normal * (1 / length) : normal;
	}
	return true;
}

bool CollisionWorld::segmentContact(const Vec3& from, const Vec3& to, SphereContact& contact) const
{
	// The faces crossed from the front only (0x006bd070 with its two-sided
	// flag clear: the end behind the face, the start in front, moving
	// against its normal), the earliest of them (0x006e62c0).
	Hit hit;
	if (!trace(from, to, hit, true)) {
		return false;
	}
	contact.point = hit.point;
	contact.normal = hit.faceNormal;
	contact.depth = std::min(dot(to - hit.point, hit.faceNormal), 0.0f);
	contact.material = hit.material;
	return true;
}

bool CollisionWorld::segmentContact(const Vec3& from, const Vec3& to, const Mat3& rotation, const Vec3& position,
	SphereContact& contact) const
{
	if (!segmentContact(rotation.untransform(from - position), rotation.untransform(to - position), contact)) {
		return false;
	}
	contact.point = position + rotation.transform(contact.point);
	contact.normal = rotation.transform(contact.normal);
	return true;
}

namespace {

Vec3 closestOnSegment(const Vec3& p, const Vec3& a, const Vec3& b)
{
	const Vec3 ab = b - a;
	const float length2 = dot(ab, ab);
	const float t = length2 > 0 ? std::clamp(dot(p - a, ab) / length2, 0.0f, 1.0f) : 0.0f;
	return a + ab * t;
}

} // namespace

// The distance from the segment's point to the edge is convex along the
// line: at most two crossings.
int segmentCapsuleCrossings(const Vec3& a, const Vec3& b, const Vec3& e0, const Vec3& e1, float radius, float roots[2])
{
	const auto f = [&](float t) {
		const Vec3 p = a + (b - a) * t;
		return std::sqrt(lengthSquared(p - closestOnSegment(p, e0, e1))) - radius;
	};
	float lo = 0, hi = 1;
	for (int i = 0; i < 40; ++i) {
		const float m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;
		(f(m1) < f(m2) ? hi : lo) = f(m1) < f(m2) ? m2 : m1;
	}
	const float tMin = (lo + hi) * 0.5f;
	if (f(tMin) >= 0) {
		return 0;
	}
	const auto bisect = [&](float inside, float outside) {
		for (int i = 0; i < 40; ++i) {
			const float mid = (inside + outside) * 0.5f;
			(f(mid) < 0 ? inside : outside) = mid;
		}
		return (inside + outside) * 0.5f;
	};
	int n = 0;
	if (f(0) > 0) {
		roots[n++] = bisect(tMin, 0);
	}
	if (f(1) > 0) {
		roots[n++] = bisect(tMin, 1);
	}
	return n;
}

void CollisionWorld::sweepSphere(const Vec3& start, const Vec3& end, float radius, std::vector<SphereContact>& contacts) const
{
	const Vec3 delta = end - start;
	if (lengthSquared(delta) <= 0.01f * 0.01f) {
		querySphere(end, radius, contacts);
		return;
	}
	const Vec3 middle = (start + end) * 0.5f;
	const float reach = std::sqrt(lengthSquared(delta)) * 0.5f + radius * 1.01f; // *0x00a8517c
	std::vector<const Triangle*> near;
	forEachFaceIndexNear(middle, reach, [&](const Triangle& t) { near.push_back(&t); });
	bool faceMet = false;
	for (const Triangle* t : near) {
		const Vec3 offset = t->normal * radius;
		const float startDistance = dot(start - (t->a + offset), t->normal);
		const float endDistance = dot(end - (t->a + offset), t->normal);
		if (startDistance < 0 || endDistance > 0 || dot(delta, t->normal) >= 0) {
			continue;
		}
		const float fraction = startDistance / (startDistance - endDistance);
		const Vec3 hit = start + delta * fraction;
		const Vec3 onFace = hit - offset;
		if (lengthSquared(closestOnTriangle(onFace, t->a, t->b, t->c) - onFace) > 1e-8f) {
			continue;
		}
		contacts.push_back({onFace, t->normal, endDistance, t->material, (fraction - 1) * std::sqrt(lengthSquared(delta))});
		faceMet = true;
	}
	if (faceMet) {
		return;
	}
	for (const Triangle* t : near) {
		const Vec3 corners[3] = {t->a, t->b, t->c};
		for (int k = 0; k < 3; ++k) {
			const Vec3& e0 = corners[k];
			const Vec3& e1 = corners[(k + 1) % 3];
			float roots[2];
			if (segmentCapsuleCrossings(start, end, e0, e1, radius, roots) == 0) {
				continue;
			}
			const Vec3 centre = start + delta * roots[0];
			const Vec3 point = closestOnSegment(centre, e0, e1);
			Vec3 normal = centre - point;
			const float length = std::sqrt(lengthSquared(normal));
			if (length <= 0) {
				continue;
			}
			normal = normal * (1.0f / length);
			const float depth = dot(end - centre, normal);
			if (depth >= 0) {
				continue;
			}
			if (normal.y < 0) {
				normal.y = 0;
				const float flat = std::sqrt(lengthSquared(normal));
				if (flat <= 1.1920929e-07f) {
					continue;
				}
				normal = normal * (1.0f / flat);
			}
			contacts.push_back({point, normal, depth, t->material, -std::sqrt(lengthSquared(end - centre))});
		}
	}
}

void CollisionWorld::capsuleEdgeContacts(const Vec3& a, const Vec3& b, float radius, std::vector<SphereContact>& contacts) const
{
	++m_stamp;
	const int x0 = static_cast<int>(std::floor((std::min(a.x, b.x) - radius) / kCell));
	const int x1 = static_cast<int>(std::floor((std::max(a.x, b.x) + radius) / kCell));
	const int z0 = static_cast<int>(std::floor((std::min(a.z, b.z) - radius) / kCell));
	const int z1 = static_cast<int>(std::floor((std::max(a.z, b.z) + radius) / kCell));
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
				const Vec3 corners[3] = {t.a, t.b, t.c};
				bool have = false;
				float best = 2;
				Vec3 candidate, e0, e1;
				for (int k = 0; k < 3; ++k) {
					float roots[2];
					const int n = segmentCapsuleCrossings(a, b, corners[k], corners[(k + 1) % 3], radius, roots);
					if (n == 2) {
						candidate = (a + (b - a) * roots[0] + a + (b - a) * roots[1]) * 0.5f;
						best = roots[0];
					} else if (n == 1 && roots[0] < best) {
						candidate = a + (b - a) * roots[0];
						best = roots[0];
					} else {
						continue;
					}
					have = true;
					e0 = corners[k];
					e1 = corners[(k + 1) % 3];
				}
				if (!have) {
					continue;
				}
				const Vec3 nearest = closestOnSegment(candidate, e0, e1);
				const Vec3 d = candidate - nearest;
				const float length = std::sqrt(lengthSquared(d));
				if (length < 1e-6f) {
					continue;
				}
				SphereContact contact;
				contact.normal = d * (1 / length);
				contact.depth = length - radius;
				if (contact.normal.y < 0) {
					contact.normal = contact.normal * -1.0f;
					contact.depth = length;
				}
				contact.point = nearest;
				contact.material = t.material;
				contacts.push_back(contact);
			}
		}
	}
}

} // namespace dice::hfe::physics
