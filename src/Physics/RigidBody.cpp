#include "RigidBody.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::physics {

namespace {

// The original zeroes components below 1e-6 after most updates.
float flush(float v) { return std::fabs(v) < 1e-6f ? 0.0f : v; }
Vec3 flush(const Vec3& v) { return {flush(v.x), flush(v.y), flush(v.z)}; }

// Scales `v` down to `limit` when longer (0x005f1310).
Vec3 clampLength(const Vec3& v, float limit)
{
	const float squared = lengthSquared(v);
	if (squared > limit * limit && squared > 0) {
		return v * (limit / std::sqrt(squared));
	}
	return v;
}

// The projection of `v` onto `axis` (0x005a2ba0).
Vec3 project(const Vec3& v, const Vec3& axis)
{
	const float squared = lengthSquared(axis);
	if (squared <= 1.1920929e-07f) {
		return {};
	}
	return axis * (dot(v, axis) / squared);
}

} // namespace

float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float lengthSquared(const Vec3& v) { return dot(v, v); }

Vec3 Mat3::transform(const Vec3& v) const { return rows[0] * v.x + rows[1] * v.y + rows[2] * v.z; }

Mat3 Mat3::operator*(const Mat3& o) const
{
	Mat3 r;
	for (int i = 0; i < 3; ++i) {
		r.rows[static_cast<size_t>(i)] = o.transform(rows[static_cast<size_t>(i)]);
	}
	return r;
}

// Rotation about a unit axis for row vectors (v * M).
Mat3 Mat3::axisAngle(const Vec3& a, float radians)
{
	const float c = std::cos(radians), s = std::sin(radians), t = 1 - c;
	Mat3 m;
	m.rows[0] = {t * a.x * a.x + c, t * a.x * a.y + s * a.z, t * a.x * a.z - s * a.y};
	m.rows[1] = {t * a.x * a.y - s * a.z, t * a.y * a.y + c, t * a.y * a.z + s * a.x};
	m.rows[2] = {t * a.x * a.z + s * a.y, t * a.y * a.z - s * a.x, t * a.z * a.z + c};
	return m;
}

Mat3 Mat3::fromYawPitchRoll(const Vec3& degrees)
{
	constexpr float toRadians = 3.14159265358979f / 180.0f;
	const float cy = std::cos(degrees.x * toRadians), sy = std::sin(degrees.x * toRadians);
	const float cp = std::cos(degrees.y * toRadians), sp = std::sin(degrees.y * toRadians);
	const float cr = std::cos(degrees.z * toRadians), sr = std::sin(degrees.z * toRadians);
	Mat3 m;
	m.rows[0] = {cr * cy + sr * sp * sy, sr * cp, -cr * sy + sr * sp * cy};
	m.rows[1] = {-sr * cy + cr * sp * sy, cr * cp, sr * sy + cr * sp * cy};
	m.rows[2] = {cp * sy, -sp, cp * cy};
	return m;
}

Vec3 Mat3::yawPitchRoll() const
{
	constexpr float toDegrees = 180.0f / 3.14159265358979f;
	const float sinPitch = std::clamp(-rows[2].y, -1.0f, 1.0f);
	return {std::atan2(rows[2].x, rows[2].z) * toDegrees, std::asin(sinPitch) * toDegrees,
		std::atan2(rows[0].y, rows[1].y) * toDegrees};
}

void RigidBody::setBounds(const Vec3& h)
{
	const float x = h.x * h.x, y = h.y * h.y, z = h.z * h.z;
	inverseBoxInertia = {3.0f / std::max(z + y, 1e-6f), 3.0f / std::max(z + x, 1e-6f), 3.0f / std::max(y + x, 1e-6f)};
}

// 0x006c59e0.
void RigidBody::addVelocity(const Vec3& change)
{
	velocity = clampLength(flush(velocity + change), tuning::kMaxSpeed);
}

// 0x006c95b0.
void RigidBody::addAngularVelocity(const Vec3& change) { angularVelocity = flush(angularVelocity + change); }

// 0x006c8ca0.
void RigidBody::applyImpulse(const Vec3& offset, const Vec3& change)
{
	impulse = flush(impulse + change);
	const Vec3 arm = offset - rotation.transform(centerOfMass);
	angularImpulse = flush(angularImpulse + cross(arm, change));
}

void RigidBody::addForce(const Vec3& offset, const Vec3& force)
{
	acceleration += force;
	torque += cross(offset - rotation.transform(centerOfMass), force);
}

void RigidBody::addFriction(const Vec3& worldPoint, const Vec3& force)
{
	const float n = static_cast<float>(frictionCount);
	const float mean = 1.0f / static_cast<float>(frictionCount + 1);
	frictionAcceleration = (frictionAcceleration * n + force) * mean;
	frictionTorque = (frictionTorque * n + cross(worldPoint - worldCenterOfMass(), force)) * mean;
	++frictionCount;
}

Vec3 RigidBody::pointVelocity(const Vec3& worldPoint) const
{
	return velocity + cross(angularVelocity, worldPoint - worldCenterOfMass());
}

void RigidBody::stepAngular(float seconds)
{
	torque = clampLength(torque, tuning::kMaxAcceleration);
	frictionTorque = clampLength(frictionTorque, tuning::kMaxAcceleration);
	// Per body axis: the impulse's part along it, over the inertia.
	const auto perAxis = [this](const Vec3& v) {
		Vec3 result;
		const float scale[3] = {inverseBoxInertia.x / (tuning::kInertiaScale * inertiaModifier.x),
			inverseBoxInertia.y / (tuning::kInertiaScale * inertiaModifier.y),
			inverseBoxInertia.z / (tuning::kInertiaScale * inertiaModifier.z)};
		for (int i = 2; i >= 0; --i) {
			result += project(v, rotation.rows[static_cast<size_t>(i)]) * scale[i];
		}
		return result;
	};
	addAngularVelocity(perAxis(angularImpulse));
	addAngularVelocity(perAxis(torque + frictionTorque) * seconds);
	const Vec3 previous = angularVelocity;
	angularVelocity = angularVelocity * tuning::kAngularDamping;
	// Turn by the tick's average angular velocity about the centre of mass.
	const Vec3 average = flush((previous + angularVelocity) * 0.5f);
	const float speedSquared = lengthSquared(average);
	if (speedSquared > 1.0000001e-06f) {
		const float speed = std::sqrt(speedSquared);
		const Vec3 axis = average * (1.0f / speed);
		const Vec3 pivot = worldCenterOfMass();
		rotation = rotation * Mat3::axisAngle(axis, speed * seconds);
		position = pivot - rotation.transform(centerOfMass);
	}
	torque = {};
	angularImpulse = {};
	frictionTorque = {};
}

void RigidBody::stepLinear(float seconds)
{
	acceleration = clampLength(acceleration, tuning::kMaxAcceleration);
	frictionAcceleration = clampLength(frictionAcceleration, tuning::kMaxAcceleration);
	impulse.y *= tuning::kVerticalImpulseScale;
	addVelocity(impulse);
	const Vec3 previous = velocity;
	addVelocity((frictionAcceleration + acceleration) * seconds);
	velocity = flush(velocity * tuning::kLinearDamping);
	const Vec3 average = flush((previous + velocity) * 0.5f);
	averageVelocity = average;
	position += average * seconds;
	acceleration = {};
	impulse = {};
	frictionAcceleration = {};
	frictionCount = 0;
}

void RigidBody::step(float seconds, float gravity)
{
	stepAngular(seconds);
	stepLinear(seconds);
	// Gravity acts in the next tick.
	acceleration.y += gravity * gravityModifier;
}

void ContactResponse::addContact(const Vec3& offset, const Vec3& pointVelocity, const Vec3& normal, float depth,
	const ContactMaterial& material)
{
	m_contacts.push_back({offset, normal, -pointVelocity, -dot(pointVelocity, normal), material.elasticity,
		material.friction});
	// The push out keeps the largest magnitude per axis (0x006b6bd0).
	const Vec3 push = normal * -depth;
	const auto larger = [](float a, float b) { return std::fabs(b) > std::fabs(a) ? b : a; };
	m_pushOut = {larger(m_pushOut.x, push.x), larger(m_pushOut.y, push.y), larger(m_pushOut.z, push.z)};
}

void ContactResponse::solve(RigidBody& body)
{
	if (m_contacts.empty() && lengthSquared(m_pushOut) == 0) {
		return;
	}
	body.position += m_pushOut;
	// Contacts share the response by how fast they approach.
	float total = 0;
	for (const Contact& c : m_contacts) {
		total += std::max(c.approachSpeed, 0.0f);
	}
	const float inverse = total > 0.0001f ? 1.0f / total : total;
	for (const Contact& c : m_contacts) {
		const float weight = std::max(c.approachSpeed, 0.0f) * inverse;
		const Vec3 weighted = c.awayVelocity * weight;
		const float normalPart = weight * c.approachSpeed;
		// Friction along the sliding direction, less when hitting head on.
		Vec3 sliding = weighted - c.normal * normalPart;
		float friction = c.friction;
		const float slidingSquared = lengthSquared(sliding);
		if (slidingSquared <= 0.001f) {
			sliding = {};
		} else {
			sliding = sliding * (1.0f / std::sqrt(slidingSquared));
			const float length = std::sqrt(std::max(lengthSquared(weighted), 1e-12f));
			const float cosine = dot(weighted * (1.0f / length), c.normal);
			friction = (1.0f - cosine * cosine * cosine) * friction * tuning::kFrictionScale;
		}
		const float strength = (c.elasticity * tuning::kElasticityScale + 1.0f) * normalPart;
		const Vec3 change = clampLength((c.normal + sliding * friction) * strength, tuning::kMaxContactImpulse);
		body.applyImpulse(c.offset, change);
	}
	m_contacts.clear();
	m_pushOut = {};
}

MassShares massShares(const ShareNode& a, const ShareNode& b, float approach, bool particle, bool fixedA, bool fixedB)
{
	MassShares s;
	const auto limited = [particle](float share) {
		return particle ? share : std::clamp(share, tuning::kMassShareLimit, 1.0f - tuning::kMassShareLimit);
	};
	// A major actor is not moved by a minor one.
	if (!a.majorActor && b.majorActor) {
		s.velocityA = s.depthA = 1;
		s.wakeA = !fixedB && a.asleep;
		return s;
	}
	if (a.majorActor && !b.majorActor) {
		s.velocityB = s.depthB = -1;
		s.wakeB = !fixedA && b.asleep;
		return s;
	}
	// A sleeping node is not moved: the other takes it all; else by mass.
	if (a.asleep) {
		s.velocityB = -1;
	} else if (b.asleep) {
		s.velocityA = 1;
	} else {
		s.velocityA = limited(b.mass / (b.mass + a.mass));
		s.velocityB = s.velocityA - 1;
	}
	s.depthA = s.velocityA;
	s.depthB = s.velocityB;
	if (particle) {
		return s;
	}
	// A sleeping node hit hard enough (the other's momentum along the
	// normal over its own mass) wakes and takes its mass share.
	const auto share = [&] {
		s.velocityA = limited(b.mass / (a.mass + b.mass));
		s.velocityB = s.velocityA - 1;
	};
	if (a.asleep && !fixedA) {
		if (-(b.mass * approach) > a.mass * tuning::kWakeMomentum) {
			s.wakeA = true;
			share();
		}
	} else if (b.asleep && !fixedB) {
		if (-(a.mass * approach) > b.mass * tuning::kWakeMomentum) {
			s.wakeB = true;
			share();
		}
	}
	// A hard contact between two mobile objects: the push out halved each way.
	if (std::fabs(approach) > tuning::kHardContactSpeed && !fixedA && !fixedB) {
		s.depthA = 0.5f;
		s.depthB = -0.5f;
	}
	return s;
}

bool sphereTouchesBox(const Vec3& center, float radius, const Vec3& low, const Vec3& high, const Mat3& rotation,
	const Vec3& position)
{
	const Vec3 p = rotation.untransform(center - position);
	return p.x >= low.x - radius && p.x <= high.x + radius && p.y >= low.y - radius && p.y <= high.y + radius
		&& p.z >= low.z - radius && p.z <= high.z + radius;
}

bool boxesOverlap(const Vec3& lowA, const Vec3& highA, const Mat3& rotationA, const Vec3& positionA, const Vec3& lowB,
	const Vec3& highB, const Mat3& rotationB, const Vec3& positionB)
{
	const Vec3 halfA = (highA - lowA) * 0.5f, halfB = (highB - lowB) * 0.5f;
	const Vec3 centreA = positionA + rotationA.transform((lowA + highA) * 0.5f);
	const Vec3 centreB = positionB + rotationB.transform((lowB + highB) * 0.5f);
	const Vec3 d = centreB - centreA;
	const float ea[3] = {halfA.x, halfA.y, halfA.z}, eb[3] = {halfB.x, halfB.y, halfB.z};
	const auto& ra = rotationA.rows;
	const auto& rb = rotationB.rows;
	const auto extent = [&](const Vec3& axis) {
		float r = 0;
		for (int i = 0; i < 3; ++i) {
			r += ea[i] * std::fabs(dot(ra[static_cast<size_t>(i)], axis)) + eb[i] * std::fabs(dot(rb[static_cast<size_t>(i)], axis));
		}
		return r;
	};
	const auto separates = [&](const Vec3& axis) {
		const float squared = lengthSquared(axis);
		return squared > 1e-12f && std::fabs(dot(d, axis)) > extent(axis);
	};
	for (int i = 0; i < 3; ++i) {
		if (separates(ra[static_cast<size_t>(i)]) || separates(rb[static_cast<size_t>(i)])) {
			return false;
		}
	}
	for (int i = 0; i < 3; ++i) {
		for (int j = 0; j < 3; ++j) {
			if (separates(cross(ra[static_cast<size_t>(i)], rb[static_cast<size_t>(j)]))) {
				return false;
			}
		}
	}
	return true;
}

void collideWithGround(RigidBody& body, ContactResponse& response, const std::vector<Vec3>& points,
	const Ground& ground, const ContactMaterial& material, const GroundTouch& touched)
{
	// An object below the ground is lifted onto it first.
	const float floor = ground.height(body.position.x, body.position.z);
	if (body.position.y < floor) {
		body.position.y = floor;
	}
	for (size_t index = 0; index < points.size(); ++index) {
		const Vec3& point = points[index];
		const Vec3 world = body.position + body.rotation.transform(point);
		Vec3 normal {0, 1, 0};
		const float groundHeight = ground.heightAndNormal(world.x, world.z, normal);
		const float depth = world.y - groundHeight;
		// Few-point objects only take contacts deeper than the push so far.
		if (depth < 0 && (points.size() > 3 || response.pushOut().y < -depth)) {
			const Vec3 contactPoint {world.x, groundHeight, world.z};
			const Vec3 velocity = body.pointVelocity(contactPoint);
			if (touched) {
				touched(index, contactPoint, velocity, normal);
			}
			response.addContact(contactPoint - body.position, velocity, normal, depth, material);
		}
	}
}

bool SleepState::sample(const Vec3& position, const Vec3& angularVelocity, float seconds)
{
	if (!sampled) {
		sampled = true;
		lastPosition = position;
	}
	const Vec3 change = (position - lastPosition) * (1.0f / seconds);
	lastPosition = position;
	linearSum = linearSum - linear[static_cast<size_t>(linearIndex)] + change;
	linear[static_cast<size_t>(linearIndex)] = change;
	linearIndex = (linearIndex + 1) % 3;
	if (lengthSquared(linearSum) >= kLinear * kLinear * 9.0f) { // 9.0 at 0x00929cbc
		return wake(), true;
	}
	angularSum = angularSum - angular[static_cast<size_t>(angularIndex)] + angularVelocity;
	angular[static_cast<size_t>(angularIndex)] = angularVelocity;
	angularIndex = (angularIndex + 1) % 5;
	if (lengthSquared(angularSum) >= kAngular * kAngular * 25.0f) { // 25.0 at 0x00993410
		return wake(), true;
	}
	if (counter > 0) {
		--counter; // slot 0x15c
	}
	return false;
}

namespace {

// At most `limit`'s own size each way, per component (the drags' clamp).
Vec3 notPast(const Vec3& change, const Vec3& limit)
{
	const auto one = [](float c, float l) { return l >= 0 ? std::min(c, l) : std::max(c, l); };
	return {one(change.x, limit.x), one(change.y, limit.y), one(change.z, limit.z)};
}

} // namespace

void applyDrag(RigidBody& body, float drag, float mass, const Vec3& dragModifier, const Vec3& size, float submerged)
{
	if (drag <= 0 || mass <= 0) {
		return;
	}
	const float f = std::fabs(size.y) < 0.01f ? 0.0f : std::min(submerged / size.y, 1.0f);
	const float water = f * 25.0f + (1.0f - f); // 25 at 0x00993410
	constexpr float kArea = 0.785f;             // *0x00993a7c
	const float areaZ = size.x * size.y * kArea * dragModifier.z;
	const float areaY = size.x * size.z * kArea * dragModifier.y;
	const float areaX = size.y * size.z * kArea * dragModifier.x;
	const auto& r = body.rotation.rows;
	// 0x006c7e00.
	const Vec3 v = body.velocity * water;
	const float k = drag / mass * std::sqrt(lengthSquared(v));
	const Vec3 linear = project(v, r[0]) * (k * areaX) + project(v, r[1]) * (k * areaY) + project(v, r[2]) * (k * areaZ);
	const Vec3 dv = notPast(linear * 0.033333335f, body.velocity);
	body.addForce({}, -(dv * 30.0f));
	// 0x006c8110.
	const Vec3& w = body.angularVelocity;
	const float kw = drag / mass * std::sqrt(lengthSquared(w)) * water;
	const Vec3 turning = project(w, r[0]) * ((areaZ + areaY) * kw) + project(w, r[1]) * ((areaZ + areaX) * kw)
		+ project(w, r[2]) * ((areaZ + areaX) * kw);
	const Vec3 dw = notPast(turning * 0.033333335f, w);
	body.torque += -(dw * 30.0f);
}

float applyBuoyancy(RigidBody& body, const Vec3& boxMin, const Vec3& boxMax, const std::vector<Vec3>& points,
	float waterLevel, float floaterMod, float gravity, float submerged, bool root, bool* entered)
{
	if (entered) {
		*entered = false;
	}
	if (points.empty()) {
		return 0;
	}
	float lowest = 1e30f;
	for (const Vec3& p : points) {
		lowest = std::min(lowest, (body.position + body.rotation.transform(p)).y);
	}
	const float depth = lowest - waterLevel;
	if (depth >= 0) {
		return 0;
	}
	if (entered) {
		*entered = submerged <= 0;
	}
	if (root && std::fabs(floaterMod) >= 1.1920929e-07f) {
		const Vec3 size = boxMax - boxMin;
		const auto& r = body.rotation.rows;
		const float a0 = std::fabs(r[0].y), a1 = std::fabs(r[1].y), a2 = std::fabs(r[2].y);
		const Vec3 up = body.rotation.untransform({0, 1, 0});
		const float h0 = body.position.y - waterLevel;
		const float gg = gravity * body.gravityModifier;
		const auto part = [](float d, float extent) { return extent > 0 ? std::min(d / extent, 1.0f) : 1.0f; };
		float most = 0, sum = 0;
		Vec3 weighted;
		for (int i = 0; i < 8; ++i) { // 0x006ae570
			const Vec3 c {(i & 4) ? boxMax.x : boxMin.x, (i & 2) ? boxMax.y : boxMin.y, (i & 1) ? boxMax.z : boxMin.z};
			const float h = dot(c, up) + h0;
			if (h > 0) {
				continue;
			}
			const float d = -h;
			const float weight = part(d, size.x) * a0 + part(d, size.y) * a1 + part(d, size.z) * a2;
			most = std::max(most, weight);
			sum += weight;
			weighted += c * weight;
		}
		if (sum > 0.01f) {
			body.addForce(body.rotation.transform(weighted * (1.0f / sum)), {0, -(most * floaterMod * gg), 0});
		}
	}
	return std::min(-depth, 1.0f);
}

void PointBody::step(float seconds, float gravity)
{
	if (drag > 0 && mass > 0) {
		// 0x006c5730 with a radius of 0.1.
		const float f = std::min(submerged * 10.0f, 1.0f);
		acceleration += velocity * (drag / mass * 0.01f * (f * 25.0f + 1.0f - f) * -3.14159265f);
	}
	// 0x006c57a0.
	const auto add = [](Vec3& v, const Vec3& change) { v = clampLength(flush(v + change), tuning::kMaxSpeed); };
	add(velocity, impulse);
	const Vec3 previous = velocity;
	add(velocity, acceleration * seconds);
	averageVelocity = flush((previous + velocity) * 0.5f);
	position += averageVelocity * seconds;
	if (rotational) {
		// 0x006c6860: no shape inertia (150 / mass) and no damping; it turns
		// about the node's origin.
		Vec3 t = torque;
		if (lengthSquared(t) > 1e6f) {
			t = t * (1000.0f / std::sqrt(lengthSquared(t)));
		}
		Vec3 dw = t * (seconds / (mass * 0.0066666673f));
		if (lengthSquared(dw) > 1e6f) {
			dw = {};
		}
		const Vec3 before = angularVelocity;
		add(angularVelocity, dw);
		const Vec3 average = flush((before + angularVelocity) * 0.5f);
		const float speedSquared = lengthSquared(average);
		if (speedSquared > 1.0000001e-06f) {
			const float speed = std::sqrt(speedSquared);
			rotation = rotation * Mat3::axisAngle(average * (1.0f / speed), speed * seconds);
		}
		torque = {};
	}
	acceleration = {};
	impulse = {};
	acceleration.y += gravity * gravityModifier;
}

} // namespace dice::hfe::physics
