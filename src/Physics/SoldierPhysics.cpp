#include "SoldierPhysics.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::physics {

namespace {

float flush(float v) { return std::fabs(v) < 1e-6f ? 0.0f : v; }
Vec3 flush(const Vec3& v) { return {flush(v.x), flush(v.y), flush(v.z)}; }

// Keeps the larger magnitude per axis (0x006b6bd0).
Vec3 largest(const Vec3& a, const Vec3& b)
{
	const auto pick = [](float x, float y) { return std::fabs(y) > std::fabs(x) ? y : x; };
	return {pick(a.x, b.x), pick(a.y, b.y), pick(a.z, b.z)};
}

Vec3 project(const Vec3& v, const Vec3& axis)
{
	const float squared = lengthSquared(axis);
	return squared <= 1.1920929e-07f ? Vec3 {} : axis * (dot(v, axis) / squared);
}

// Shortens `v` to `limit`; true if it was longer (0x006b6fd0).
bool clampTo(Vec3& v, float limit)
{
	const float squared = lengthSquared(v);
	if (squared > limit * limit && squared > 0) {
		v = v * (limit / std::sqrt(squared));
		return true;
	}
	return false;
}

} // namespace

void SoldierBody::collideWithGround(const Ground& ground)
{
	m_onGround = false;
	m_groundNormal = {0, 0, 0};
	const float c = std::cos(yawDegrees * 3.14159265f / 180), s = std::sin(yawDegrees * 3.14159265f / 180);
	for (int i = 0; i < 5; ++i) {
		const Vec3& p = soldier::kGroundPoints[i];
		const Vec3 world {position.x + p.x * c + p.z * s, position.y + p.y, position.z - p.x * s + p.z * c};
		Vec3 normal {0, 1, 0};
		const float height = ground.heightAndNormal(world.x, world.z, normal);
		const float depth = world.y - height;
		if (depth > 0) {
			continue;
		}
		addContact(normal, normal.y * depth);
		if (i == 0 && m_groundNormal.y < normal.y) {
			m_groundNormal = normal;
			m_onGround = true;
		}
	}
}

// 0x006c24b0: push out along the normal, stop motion into the surface.
void SoldierBody::addContact(const Vec3& normal, float depth)
{
	m_pushOut = largest(m_pushOut, normal * -depth);
	m_cancel = largest(m_cancel, project(-velocity, normal));
	m_contactVelocity = flush((m_contactVelocity * static_cast<float>(m_contacts) + velocity) *
		(1.0f / static_cast<float>(m_contacts + 1)));
	++m_contacts;
}

// A stack of spheres from the feet up (0x006c1b30: five while standing)
// against the static objects' meshes (0x006c3890, with 0x00a84ac4 set). The
// contacts are filtered before they count:
//   - a contact above half a spacing over the lowest sphere's centre with a
//     normal pointing up (y >= 0.95, 0x00a8471c) loses the normal's height,
//     and so does one below the top with a normal pointing down: the upper
//     spheres only push sideways off floors, and ceilings never press the
//     soldier into the ground;
//   - of the contacts below the lowest centre with an upward normal, within
//     a radius of it, the first counts and each nearer one after it is
//     dropped (0x006c4180, literally);
//   - a contact whose sphere centre lies behind the face (the normal points
//     from the centre to the contact) is dropped (0x00a846fc): a face seen
//     from behind does not push the soldier through it.
// Not rewritten: prone, a floor contact (y > 0.95, 0x00a8470c) pushes
// straight up; the body always uses the standing stack. The spheres are not
// swept from the last position (the original sweeps them above 0.9 radius
// a tick, faster than walking).
bool soldierFits(const CollisionWorld& world, const Vec3& origin, int stance, const Mat3& rotation, const Vec3& position)
{
	int count = 5;
	float height = soldier::kHeight;
	if (stance == 1 || stance == 3) {
		count = 3;
		height = 1.4f; // *0x00a84798
	} else if (stance == 2) {
		count = 1;
		height = 0.8f; // *0x00a84778 (one sphere: no spacing)
	}
	const float spacing = count > 1 ? (height - 2 * soldier::kRadius) / static_cast<float>(count - 1) : 0.0f;
	std::vector<SphereContact> contacts;
	for (int i = 0; i < count; ++i) {
		// origin.y + i x spacing + (radius - *0x00a84768, 1)
		const Vec3 centre {origin.x, origin.y + spacing * static_cast<float>(i) + soldier::kRadius - 1.0f, origin.z};
		world.querySphere(rotation.untransform(centre - position), soldier::kRadius, contacts);
		if (!contacts.empty()) {
			return false;
		}
	}
	return true;
}

void SoldierBody::collideWithObjects()
{
	if (!world) {
		return;
	}
	constexpr int kSpheres = 5;
	const float spacing = (soldier::kHeight - 2 * soldier::kRadius) / (kSpheres - 1);
	const float lowest = position.y + soldier::kRadius - 1.0f; // the origin is a metre above the feet
	std::vector<SphereContact> contacts;
	std::vector<Vec3> centres; // each contact's sphere
	for (int i = 0; i < kSpheres; ++i) {
		const Vec3 centre {position.x, lowest + spacing * static_cast<float>(i), position.z};
		world->querySphere(centre, soldier::kRadius, contacts);
		centres.resize(contacts.size(), centre);
	}
	const auto normalize = [](Vec3& v) { // 0x004481a0
		const float squared = lengthSquared(v);
		if (std::fabs(squared - 1.0f) >= 1.1920929e-07f) {
			v = std::fabs(squared) < 1.1920929e-07f ? Vec3 {} : v * (1.0f / std::sqrt(squared));
		}
	};
	const float upper = lowest + spacing * soldier::kUpperContact;
	const float top = lowest + spacing * static_cast<float>(kSpheres);
	for (SphereContact& contact : contacts) {
		bool flattened = false;
		if (contact.point.y >= upper && contact.normal.y >= soldier::kFlatNormal) {
			contact.normal.y = 0;
			flattened = true;
		}
		if (contact.point.y <= top && contact.normal.y < -soldier::kFlatNormal) {
			contact.normal.y = 0;
			flattened = true;
		}
		if (flattened) {
			normalize(contact.normal);
		}
	}
	bool floorFound = false;
	float nearest = soldier::kRadius;
	for (size_t i = 0; i < contacts.size(); ++i) {
		const SphereContact& contact = contacts[i];
		if (contact.point.y >= lowest || contact.normal.y <= 0) {
			continue;
		}
		const float distance = std::sqrt(lengthSquared(contact.point - Vec3 {position.x, lowest, position.z}));
		if (distance < nearest) {
			if (floorFound) {
				contacts[i].depth = 1;
			}
			floorFound = true;
			nearest = distance;
		}
	}
	for (size_t i = 0; i < contacts.size(); ++i) {
		const SphereContact& contact = contacts[i];
		if (contact.depth >= 0 || dot(contact.point - centres[i], contact.normal) > 0) {
			continue;
		}
		addContact(contact.normal, contact.depth);
		// Something underfoot holds the soldier up like the ground.
		if (contact.point.y < lowest + soldier::kFloorTolerance && m_groundNormal.y < contact.normal.y) {
			m_groundNormal = contact.normal;
			m_onGround = true;
		}
	}
}

void SoldierBody::solve()
{
	position += m_pushOut;
	if (lengthSquared(m_cancel) > 0) {
		// The elasticity of the ground (0) plus one, times 0x00a846cc (1).
		impulse = flush(impulse + m_cancel);
	}
	m_pushOut = {};
	m_cancel = {};
}

void SoldierBody::move(float gravity)
{
	if (m_onGround) {
		const float ny = m_groundNormal.y;
		const float grip = ny * ny * ny * ny * ny * friction;
		// How far the ground velocity is from the wanted one; gravity's tick is
		// part of the vertical error.
		Vec3 error = m_contactVelocity - m_desired;
		error.y += gravity / 30.0f;
		error = error - project(error, m_groundNormal);
		Vec3 correction;
		Vec3 wanted = -error;
		if (!m_slipping) {
			correction = wanted;
			if (clampTo(correction, grip * soldier::kStaticGrip)) {
				m_slipping = true;
			}
		} else {
			correction = wanted;
			if (!clampTo(correction, grip * soldier::kSlidingGrip)) {
				m_slipping = false;
				clampTo(correction, grip * soldier::kStaticGrip);
			}
		}
		// Node slot 0xa8: averaged with other requests this tick.
		const Vec3 acceleration = correction * 30.0f;
		m_inputAcceleration = (m_inputAcceleration * static_cast<float>(m_inputs) + acceleration) *
			(1.0f / static_cast<float>(m_inputs + 1));
		++m_inputs;
	}
	m_desired = {};
	m_contactVelocity = {};
	m_contacts = 0;
}

void SoldierBody::integrate(float seconds)
{
	if (lengthSquared(acceleration) > 1e6f) {
		acceleration = acceleration * (1000.0f / std::sqrt(lengthSquared(acceleration)));
	}
	if (lengthSquared(m_inputAcceleration) > soldier::kMaxInputAcceleration * soldier::kMaxInputAcceleration) {
		m_inputAcceleration = {};
	}
	const auto addVelocity = [this](const Vec3& change) {
		velocity = flush(velocity + change);
		if (lengthSquared(velocity) > tuning::kMaxSpeed * tuning::kMaxSpeed) {
			velocity = velocity * (tuning::kMaxSpeed / std::sqrt(lengthSquared(velocity)));
		}
	};
	addVelocity(impulse);
	const Vec3 previous = velocity;
	addVelocity((m_inputAcceleration + acceleration) * seconds);
	float water = std::min(waterDepth, 1.0f);
	if (water > 0.05f && water < 0.5f) {
		water = 0.5f;
	}
	velocity = velocity * (water * soldier::kWaterDamping + (1.0f - water) * soldier::kLandDamping);
	const Vec3 average = flush((previous + velocity) * 0.5f);
	position += average * seconds;
	acceleration = {};
	impulse = {};
	m_inputAcceleration = {};
	m_inputs = 0;
}

void SoldierBody::step(float seconds, const Ground& ground, float gravity)
{
	collideWithGround(ground);
	collideWithObjects();
	solve();
	move(gravity);
	integrate(seconds);
	acceleration.y += gravity * gravityModifier;
}

} // namespace dice::hfe::physics
