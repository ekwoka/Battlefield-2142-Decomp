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

// 0x006c28c0 at `scale` (the points scaled towards the origin): each
// point at or under the ground pushes out along the normal and cancels the
// velocity into it. In the solve's passes (scale under 1) the depth counts
// double over the scale (*0x00a84794) and the water is left alone. Returns
// the deepest point's depth.
float SoldierBody::collideWithGround(const Ground& ground, float gravity, float scale)
{
	const float c = std::cos(yawDegrees * 3.14159265f / 180), s = std::sin(yawDegrees * 3.14159265f / 180);
	float lowest = 1e30f;
	float deepest = 0;
	for (int i = 0; i < 5; ++i) {
		const Vec3 p = soldier::kGroundPoints[i] * scale;
		const Vec3 world {position.x + p.x * c + p.z * s, position.y + p.y, position.z - p.x * s + p.z * c};
		lowest = std::min(lowest, world.y);
		Vec3 normal {0, 1, 0};
		const float height = ground.heightAndNormal(world.x, world.z, normal);
		float depth = world.y - height;
		if (depth > 0) {
			continue;
		}
		deepest = std::min(deepest, depth);
		if (scale < 1) {
			depth *= soldier::kSolvePush / scale;
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
	if (scale < 1) {
		return deepest;
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
		return deepest;
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
	return deepest;
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
float soldier::stanceHeight(int stance)
{
	if (stance == 1 || stance == 3) {
		return 1.4f; // *0x00a84798
	}
	return stance == 2 ? 0.8f : soldier::kHeight; // *0x00a84778
}

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
	float deepest = 0;
	if (world && !collideWithMesh(*world, nullptr, seconds, 1, true, deepest)) {
		return;
	}
	for (const Mover& mover : movers) {
		if (mover.mesh && !collideWithMesh(*mover.mesh, &mover, seconds, 1, true, deepest)) {
			return;
		}
	}
}

// Another soldier (0x006c2620; slot 9 with scale 1, the solve's passes
// with 0.999): when their heights overlap (each from its feet, a metre
// under the origin, to its stance's height) and their origins are within
// twice this soldier's radius x the scale across (x, z), each is pushed
// away from the other by the overlap with no velocity (0x006c24b0 with
// material 0x2f); 0.01 apart or nearer, along z. The original gives the
// other its contact as well; here each soldier takes its own when it
// steps.
void SoldierBody::collideWithSoldiers(float scale)
{
	const float top = position.y - 1.0f + soldier::stanceHeight(stance);
	for (const Neighbour& other : soldiers) {
		const float otherTop = other.position.y - 1.0f + soldier::stanceHeight(other.stance);
		if (position.y - 1.0f > otherTop || other.position.y - 1.0f > top) {
			continue;
		}
		Vec3 away {other.position.x - position.x, 0, other.position.z - position.z};
		const float reach = 2 * soldier::kRadius * scale;
		const float squared = lengthSquared(away);
		if (squared > reach * reach) {
			continue;
		}
		const float distance = std::sqrt(squared);
		away = distance >= 0.01f ? away * (1.0f / distance) : Vec3 {0, 0, 1};
		addContact(away, reach - distance, {});
	}
}

// One object (0x006c3890; slot 9 with scale 1 and the contacts' normals).
// The stack and the filters' heights are taken where the soldier was
// before the last integration (the node's saved matrix), less how the
// object itself moved the soldier's old point; a stack that moved over 0.9
// of a radius sweeps each sphere from its old centre (put back by 0.45 of
// a radius x the scale along the way) to its new one, otherwise each is
// tested where it is now. A moving object's mesh is tested in its own
// frame. False when a collision event vetoed (the loop ends).
// In the solve's passes (scale 0.999): the spheres are the scale's size,
// the floor and ceiling filter is off (*0x00a8479c 0), a contact counts
// double over the scale (*0x00a84794), with no velocity, no collision
// events and no standing on it; past the first three passes (`normals`
// false) each pushes back along the way the soldier came (the motion's
// direction turned round) by how far the sphere went past the face (the
// mesh query's second depth: the sweep's distance beyond the crossing,
// 0x006bd070 / 0x006be300; the depth itself when not swept).
bool SoldierBody::collideWithMesh(const CollisionWorld& mesh, const Mover* mover, float seconds, float scale,
	bool normals, float& deepest)
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
	const Vec3 back = way * (r * soldier::kSweepFactor * scale * 0.5f);
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
			mesh.sweepSphere(toMesh(start), toMesh(end), r * scale, contacts);
		} else {
			mesh.querySphere(toMesh(end), r * scale, contacts);
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
		if (scale < 1) {
			break; // *0x00a8479c: not in the passes
		}
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
	Vec3 backward = motion * -1.0f; // the way back (0x004481a0 leaves a zero motion zero)
	normalize(backward);
	for (size_t i = 0; i < contacts.size(); ++i) {
		SphereContact& contact = contacts[i];
		if (contact.depth >= 0) {
			continue;
		}
		deepest = std::min(deepest, contact.depth);
		Vec3 normal = backward;
		float depth = contact.travel;
		if (normals) {
			normal = contact.normal;
			depth = contact.depth;
			// Prone, a floor counts as level (*0x00a84788, *0x00a84744).
			if (stance == 2 && normal.y > soldier::kFlatNormal) {
				normal = {0, 1, 0};
			}
			if (dot(contact.point - centres[i], normal) > 0) {
				continue;
			}
		}
		if (scale < 1) {
			addContact(normal, depth * (soldier::kSolvePush / scale), {});
			continue;
		}
		const Vec3 relative = velocity - (mover ? mover->pointVelocity(contact.point) : Vec3 {});
		if (lengthSquared(relative) > 0) {
			// 0x005ee2b0 between the two objects' events: the other's point
			// velocity x 2.5 when faster than 1 m/s relative (a static
			// object's is none).
			if (onPush && lengthSquared(relative) > 1) {
				onPush((mover ? mover->pointVelocity(contact.point) : Vec3 {}) * soldier::kPushScale);
			}
			if (onContact
				&& !onContact({mover ? mover->owner : 0, relative, normal, contact.point, material, contact.material})) {
				return false;
			}
		}
		addContact(normal, depth, relative);
		// Something underfoot holds the soldier up like the ground.
		if (contact.point.y < lowest + soldier::kFloorTolerance && m_groundNormal.y < normal.y) {
			m_groundNormal = normal;
			m_onGround = true;
		}
	}
	return true;
}

// 0x006c48c0: the contacts so far push the soldier out; the velocity into
// them is cancelled (an impulse) once the soldier is free. Then the passes
// (at most eight, *0x00a84738): the objects (the static ones, the moving
// ones, the other soldiers) and the ground again at 0.999 of the size,
// each pass's contacts pushing out at once (the nested solve: its push
// only), until a pass finds none. Free: the impulse, then the run-over
// check (0x006c2e50). Still stuck after eight: squashed (node slot
// 0x148(0): no velocity, impulse or input; gravity only), standing on
// flat ground (+0xf9, the normal up, one contact at no speed), back where
// it was before the last integration (slot 0x48); a heavy object (over
// 100, awake) moving over 3 m/s against it with the crush 0.05 deep or
// more (the objects' contacts in every pass, the ground's past the third)
// throws it at that object's velocity x 2.5 (0x006c1db0 -> 0x005ee2b0)
// and kills it (1000, the object's owner credited). Then the server's
// segment test.
void SoldierBody::solve(float seconds, const Ground& ground, float gravity)
{
	position += m_pushOut;
	const Vec3 cancel = m_cancel;
	m_pushOut = {};
	m_cancel = {};
	m_squashed = false;
	const Vec3 savedVelocity = m_contactVelocity;
	const int savedContacts = m_contacts;
	float deepest = 0;
	bool heavy = false;
	uint32_t crusher = 0;
	Vec3 heavyVelocity;
	int pass = 0;
	for (; pass < soldier::kSolvePasses; ++pass) {
		m_contacts = 0;
		m_contactVelocity = {};
		const bool normals = pass < soldier::kNormalPasses;
		float depth = 0;
		if (world) {
			collideWithMesh(*world, nullptr, seconds, soldier::kSolveScale, normals, depth);
		}
		for (const Mover& mover : movers) {
			if (mover.mass > soldier::kHeavyMass && !mover.asleep) {
				heavy = true;
				heavyVelocity = mover.velocity;
				if (lengthSquared(mover.pointVelocity(position) - velocity) > soldier::kCrushSpeed * soldier::kCrushSpeed) {
					crusher = mover.owner;
				}
			}
			if (mover.mesh) {
				collideWithMesh(*mover.mesh, &mover, seconds, soldier::kSolveScale, normals, depth);
			}
		}
		collideWithSoldiers(soldier::kSolveScale);
		deepest = std::min(deepest, depth);
		const float groundDepth = collideWithGround(ground, gravity, soldier::kSolveScale);
		if (!normals) {
			deepest = std::min(deepest, groundDepth);
		}
		if (m_contacts == 0) {
			break;
		}
		position += m_pushOut;
		m_pushOut = {};
		m_cancel = {};
	}
	if (pass < soldier::kSolvePasses) {
		if (lengthSquared(cancel) > 0) {
			// The elasticity of the ground (0) plus one, times 0x00a8478c (1).
			impulse = flush(impulse + cancel * soldier::kCancelScale);
		}
		m_contactVelocity = savedVelocity;
		m_contacts = savedContacts;
		runOver();
	} else {
		m_squashed = true;
		velocity = {};
		impulse = {};
		acceleration = {0, gravity * gravityModifier, 0};
		m_inputAcceleration = {};
		m_inputs = 0;
		m_onGround = true;
		m_groundNormal = {0, 1, 0};
		m_contacts = 1;
		m_contactVelocity = {};
		position = m_lastPosition;
		if (heavy && crusher != 0 && deepest <= -soldier::kSquashDepth) {
			if (onPush) {
				onPush(heavyVelocity * soldier::kPushScale);
			}
			if (onCrushed) {
				onCrushed(crusher);
			}
		}
	}
	m_pushOut = {};
	m_cancel = {};
	segmentTest(seconds);
}

// 0x006c2e50: each heavy moving object (mass 100 or more, awake) whose
// soldier LOD comes within half a radius of a centre of the stance's
// spheres where the soldier is now, its point velocity there over 3 m/s
// from the soldier's, runs it over: 1000 damage, its owner credited. (The
// original spares an owner under 10 in mass of the Particle class, which
// the 100 already rules out, and leaves out the node's +0x90 velocity,
// which the rewrite's soldiers don't have.)
void SoldierBody::runOver()
{
	if (!onCrushed) {
		return;
	}
	int count = 5;
	if (stance == 1 || stance == 3) {
		count = 3;
	} else if (stance == 2) {
		count = 1;
	}
	const float r = soldier::kRadius;
	const float height = soldier::stanceHeight(stance);
	const float spacing = count > 1 ? (height - 2 * r) / static_cast<float>(count - 1) : 0.0f;
	std::vector<SphereContact> contacts;
	for (const Mover& mover : movers) {
		if (!mover.mesh || mover.mass < soldier::kHeavyMass || mover.asleep) {
			continue;
		}
		for (int i = 0; i < count; ++i) {
			const Vec3 centre {position.x, position.y + spacing * static_cast<float>(i) + r - 1.0f, position.z};
			contacts.clear();
			mover.mesh->querySphere(mover.rotation.untransform(centre - mover.position), r * 0.5f, contacts);
			if (contacts.empty()) {
				continue;
			}
			if (lengthSquared(mover.pointVelocity(centre) - velocity) > soldier::kCrushSpeed * soldier::kCrushSpeed) {
				onCrushed(mover.owner);
				return;
			}
		}
	}
}

// The server's segment test (0x006c48c0's end, game logic slot 0x144):
// the origin's way from before the last integration to now, less each
// moving object's own motion, crossing a face of an object's soldier LOD
// from the front (collision mesh slot 22 0x006e5920 with LOD 2): the
// soldier is killed (1000, no one credited).
void SoldierBody::segmentTest(float seconds)
{
	if (!server || !onCrushed) {
		return;
	}
	const Vec3 way = position - m_lastPosition;
	SphereContact contact;
	if (world && lengthSquared(way) > 0 && world->segmentContact(m_lastPosition, position, contact)) {
		onCrushed(0);
		return;
	}
	for (const Mover& mover : movers) {
		const Vec3 relative = way - mover.pointVelocity(mover.position) * seconds;
		if (mover.mesh && lengthSquared(relative) > 0
			&& mover.mesh->segmentContact(m_lastPosition, m_lastPosition + relative, mover.rotation, mover.position, contact)) {
			onCrushed(0);
			return;
		}
	}
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
	if (m_jumping) {
		// 0x0058a6d0: slot 0xc8 (no impulse), 0x78 (the acceleration) and
		// 0x5c (the velocity), all the jump's.
		m_jumping = false;
		impulse = {};
		velocity = {};
		addVelocity(m_jump);
		acceleration = m_jump;
	}
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
	m_onGround = false; // the response's +0xf9 and normal, each tick
	m_groundNormal = {0, 0, 0};
	collideWithGround(ground, gravity);
	collideWithObjects(seconds);
	collideWithSoldiers(1);
	solve(seconds, ground, gravity);
	move(gravity);
	integrate(seconds);
	acceleration.y += gravity * gravityModifier;
	m_stepEnd = position;
}

} // namespace dice::hfe::physics
