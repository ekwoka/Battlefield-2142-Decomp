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

void SoldierBody::collideWithGround(const Ground& ground, float gravity)
{
	m_onGround = false;
	m_groundNormal = {0, 0, 0};
	const float c = std::cos(yawDegrees * 3.14159265f / 180), s = std::sin(yawDegrees * 3.14159265f / 180);
	float lowest = 1e30f;
	for (int i = 0; i < 5; ++i) {
		const Vec3& p = soldier::kGroundPoints[i];
		const Vec3 world {position.x + p.x * c + p.z * s, position.y + p.y, position.z - p.x * s + p.z * c};
		lowest = std::min(lowest, world.y);
		Vec3 normal {0, 1, 0};
		const float height = ground.heightAndNormal(world.x, world.z, normal);
		const float depth = world.y - height;
		if (depth > 0) {
			continue;
		}
		// The collision event (object slot 0xac) over |v|^2 0.1 (*0x00a8477c).
		if (onContact && lengthSquared(velocity) > soldier::kContactSpeed) {
			const uint32_t terrain = ground.material ? ground.material(world.x, world.z) : 0;
			onContact({0, velocity, normal, {world.x, height, world.z}, material, terrain});
		}
		addContact(normal, normal.y * depth, velocity);
		if (i == 0 && m_groundNormal.y < normal.y) {
			m_groundNormal = normal;
			m_onGround = true;
		}
	}
	// Water (0x6c2bf4-0x6c2dfb): the feet's depth under it; deeper than 0.3
	// the water lifts by gravity x gravityModifier x -depth^2 (under a metre)
	// or -depth (a metre on: level at a metre, the origin at the surface)
	// through the node's acceleration (slot 0xa4 -> 0x7c). Swimming from
	// 0.99 deep until under 0.9: the ground counts as flat with friction 1
	// and the velocity as the contacts', so the movement steers along the
	// surface.
	const float level = ground.waterLevel ? ground.waterLevel(position.x, position.z) : -10000.0f;
	const float d = lowest - level;
	if (d >= 0) {
		waterDepth = 0;
		m_swim = false;
		return;
	}
	waterDepth = -d;
	if (waterDepth > soldier::kBuoyancyDepth) {
		const float k = waterDepth < 1.0f ? -std::min(d * d, 1.0f) : d;
		acceleration.y += gravity * gravityModifier * k;
	}
	if (waterDepth >= soldier::kSwimOn) {
		m_swim = true;
	} else if (waterDepth < soldier::kSwimOff) {
		m_swim = false;
	}
	if (m_swim) {
		m_groundNormal = {0, 1, 0};
		m_contactVelocity = velocity;
	}
}

// 0x006c24b0: push out along the normal, stop motion into the surface.
// `relative`: the soldier's velocity relative to what it touches.
void SoldierBody::addContact(const Vec3& normal, float depth, const Vec3& relative)
{
	m_pushOut = largest(m_pushOut, normal * -depth);
	m_cancel = largest(m_cancel, project(-relative, normal));
	m_contactVelocity = flush((m_contactVelocity * static_cast<float>(m_contacts) + relative) *
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
// Prone (one sphere at the feet) a floor contact (y > 0.95, *0x00a84788)
// pushes straight up. The stack sweeps from its last place when it moved
// over 0.9 of a radius in the tick (6.75 m/s).
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

void SoldierBody::collideWithObjects(float seconds)
{
	if (world && !collideWithMesh(*world, nullptr, seconds)) {
		return;
	}
	for (const Mover& mover : movers) {
		if (mover.mesh && !collideWithMesh(*mover.mesh, &mover, seconds)) {
			return;
		}
	}
}

// One object (0x006c3890 on the slot-9 path: the full radius, the
// normals as found). The stack and the filters' heights are taken where
// the soldier was before the last integration (the node's saved matrix),
// less how the object itself moved the soldier's old point; a stack that
// moved over 0.9 of a radius sweeps each sphere from its old centre (put
// back by 0.45 of a radius along the way) to its new one, otherwise each
// is tested where it is now. A moving object's mesh is tested in its own
// frame. False when a collision event vetoed (the loop ends).
bool SoldierBody::collideWithMesh(const CollisionWorld& mesh, const Mover* mover, float seconds)
{
	int count = 5;
	float height = soldier::kHeight;
	if (stance == 1 || stance == 3) {
		count = 3;
		height = 1.4f; // *0x00a84798
	} else if (stance == 2) {
		count = 1;     // prone: one sphere at the feet
		height = 0.8f; // *0x00a84778
	}
	const float r = soldier::kRadius;
	const float spacing = count > 1 ? (height - 2 * r) / static_cast<float>(count - 1) : 0.0f;
	const Vec3 otherMotion = mover ? mover->pointVelocity(m_lastPosition) * seconds : Vec3 {};
	const Vec3 motion = position - m_lastPosition - otherMotion;
	const Vec3 previous = position - motion;
	const bool sweep = lengthSquared(motion) > (r * soldier::kSweepFactor) * (r * soldier::kSweepFactor);
	Vec3 way;
	if (sweep) {
		way = motion * (1.0f / std::sqrt(lengthSquared(motion)));
	}
	const Vec3 back = way * (r * soldier::kSweepFactor * 0.5f);
	const float lowest = previous.y + r - 1.0f; // the origin is a metre above the feet
	const auto toMesh = [mover](const Vec3& p) { return mover ? mover->rotation.untransform(p - mover->position) : p; };
	std::vector<SphereContact> contacts;
	std::vector<Vec3> centres; // each contact's sphere, where it was
	for (int i = 0; i < count; ++i) {
		const Vec3 centre {previous.x, lowest + spacing * static_cast<float>(i), previous.z};
		const Vec3 start = centre - back;
		const Vec3 end = centre + motion;
		const size_t before = contacts.size();
		if (sweep) {
			mesh.sweepSphere(toMesh(start), toMesh(end), r, contacts);
		} else {
			mesh.querySphere(toMesh(end), r, contacts);
		}
		if (mover) {
			for (size_t k = before; k < contacts.size(); ++k) {
				contacts[k].point = mover->position + mover->rotation.transform(contacts[k].point);
				contacts[k].normal = mover->rotation.transform(contacts[k].normal);
			}
		}
		centres.resize(contacts.size(), centre);
	}
	const auto normalize = [](Vec3& v) { // 0x004481a0
		const float squared = lengthSquared(v);
		if (std::fabs(squared - 1.0f) >= 1.1920929e-07f) {
			v = std::fabs(squared) < 1.1920929e-07f ? Vec3 {} : v * (1.0f / std::sqrt(squared));
		}
	};
	const float upper = lowest + spacing * soldier::kUpperContact;
	const float top = lowest + spacing * static_cast<float>(count);
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
	float nearest = r;
	for (size_t i = 0; i < contacts.size(); ++i) {
		const SphereContact& contact = contacts[i];
		if (contact.point.y >= lowest || contact.normal.y <= 0) {
			continue;
		}
		const float distance = std::sqrt(lengthSquared(contact.point - Vec3 {previous.x, lowest, previous.z}));
		if (distance < nearest) {
			if (floorFound) {
				contacts[i].depth = 1;
			}
			floorFound = true;
			nearest = distance;
		}
	}
	for (size_t i = 0; i < contacts.size(); ++i) {
		SphereContact& contact = contacts[i];
		if (contact.depth >= 0) {
			continue;
		}
		// Prone, a floor counts as level (*0x00a84788, *0x00a84744).
		if (stance == 2 && contact.normal.y > soldier::kFlatNormal) {
			contact.normal = {0, 1, 0};
		}
		if (dot(contact.point - centres[i], contact.normal) > 0) {
			continue;
		}
		const Vec3 relative = velocity - (mover ? mover->pointVelocity(contact.point) : Vec3 {});
		if (onContact && lengthSquared(relative) > 0
			&& !onContact({mover ? mover->owner : 0, relative, contact.normal, contact.point, material, contact.material})) {
			return false;
		}
		addContact(contact.normal, contact.depth, relative);
		// Something underfoot holds the soldier up like the ground.
		if (contact.point.y < lowest + soldier::kFloorTolerance && m_groundNormal.y < contact.normal.y) {
			m_groundNormal = contact.normal;
			m_onGround = true;
		}
	}
	return true;
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
	if (m_onGround || m_swim) {
		const float ny = m_groundNormal.y;
		const float grip = ny * ny * ny * ny * ny * (m_swim ? 1.0f : friction);
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
	m_lastPosition = position; // slot 0x4c: the matrix before the step
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
	if (!m_hasLast || lengthSquared(position - m_stepEnd) > 1e-10f) {
		m_lastPosition = position; // put there from outside: no motion
		m_hasLast = true;
	}
	collideWithGround(ground, gravity);
	collideWithObjects(seconds);
	solve();
	move(gravity);
	integrate(seconds);
	acceleration.y += gravity * gravityModifier;
	m_stepEnd = position;
}

} // namespace dice::hfe::physics
