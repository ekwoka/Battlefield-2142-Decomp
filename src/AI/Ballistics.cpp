#include "AI/Ballistics.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace dice::hfe::ai {

namespace {

constexpr float kMaxAngle = 2.984513f;    // DAT_1012b4e0: the launch angle's bound either way
constexpr float kHighStart = 1.1780972f;  // 3 pi / 8: the high arc's first try
constexpr float kHighPull = 1.5079645f;   // the high arc is pulled halfway to this each solve
constexpr float kMaxStep = 0.017f;        // the step grows to at most this between solves
constexpr float kMinStep = 0.0001f;
constexpr float kNoSpeedLift = 20;        // a round without speed is aimed 20 m higher

float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
float across(const Vec3& v) { return std::sqrt(v.x * v.x + v.z * v.z); }

// 0x10026a20: unit length, zero for a vector too short to have a direction.
Vec3 normalized(const Vec3& v)
{
	const float l2 = dot(v, v);
	if (std::fabs(l2 - 1) < 1.1920929e-07f) {
		return v;
	}
	if (std::fabs(l2) < 1.1920929e-07f) {
		return {};
	}
	return v * (1 / std::sqrt(l2));
}

float sign(float v) { return v > 0 ? 1.0f : v < 0 ? -1.0f : 0.0f; } // 0x1005cf40

} // namespace

float roundDrag(float drag, float mass, int physicsType)
{
	switch (physicsType) {
	case 1:
	case 2:
		return mass != 0 ? drag / mass * 0.03141593f : 0.0f;
	case 3:
		return drag != 0 ? 0.3f : 0.0f;
	default:
		return drag;
	}
}

void LeadSolver::setup(const Vec3& offset, const Vec3& targetVelocity, const Vec3& shooterVelocity,
	const Vec3& gunForward, double now, const RoundModel& round)
{
	m_round = round;
	m_offset = offset;
	m_toTarget = normalized(offset);
	m_targetVelocity = targetVelocity;
	m_shooterVelocity = shooterVelocity;
	m_gunForward = gunForward;
	m_stamp = now;
}

Vec3 LeadSolver::targetAt(double time) const
{
	// The record's +0x68..+0x6a would hold the time or drop the point a
	// tenth; no aim ever sets them (0x100f9fd0 clears them).
	const float elapsed = static_cast<float>(time - m_stamp);
	return m_offset + m_targetVelocity * elapsed;
}

float LeadSolver::flightTime(float distance, float speed) const
{
	const float dt = m_round.step;
	const bool quadratic = speed > 200; // by the launch speed, for the whole flight
	float time = 0;
	float v = speed;
	while (distance > 0) {
		float loss = dt * m_round.drag * v;
		if (quadratic) {
			loss *= v;
		}
		const float next = std::max(v - loss, 0.0f);
		const float covered = (v + next) * dt * 0.5f;
		if (covered <= 0 || 1000 * covered < distance) {
			return 1000; // it stops (or would take a thousand steps more) short of it
		}
		time += covered <= distance ? dt : distance / covered * dt;
		distance -= covered;
		v = next;
	}
	return time;
}

Vec3 LeadSolver::flight(const Vec3& velocity, float seconds) const
{
	Vec3 travelled;
	if (length(velocity) == 0) {
		return travelled;
	}
	Vec3 v = velocity;
	while (seconds > 0) {
		const float dt = std::min(m_round.step, seconds);
		Vec3 loss;
		if (m_round.drag != 0) {
			// Linear up to 200 m/s, then by the speed squared.
			const float speed = length(v);
			loss = (speed <= 200 ? v : v * speed) * (m_round.drag * dt);
		}
		Vec3 next = v - loss;
		next.y += m_round.gravity * dt;
		travelled += (next + v) * (0.5f * dt);
		v = next;
		seconds -= dt;
	}
	return travelled;
}

bool LeadSolver::roundAt(const Vec3& direction, float seconds, Vec3& position) const
{
	if (seconds > 100) {
		return false;
	}
	position = flight(direction * m_round.speed + m_shooterVelocity, seconds);
	return true;
}

bool LeadSolver::solve(double now, float precision, bool high, Vec3& direction, Vec3& impact, double* impactTime)
{
	const bool noSpeed = m_round.speed == 0;
	const float lift = noSpeed ? kNoSpeedLift : 0.0f;
	// The first guess of the flight time: the straight distance over the
	// speed at which the round closes along the line.
	const float closing = m_round.speed + dot(m_toTarget, m_shooterVelocity) - dot(m_targetVelocity, m_toTarget);
	if (!(closing >= 1)) {
		return false;
	}
	const float firstGuess = length({m_offset.x, m_offset.y + lift, m_offset.z}) / closing;
	if (!(firstGuess >= 0)) {
		return false;
	}
	const double firstTime = firstGuess + now;
	impact = targetAt(firstTime);
	float distance = across(impact);
	if (distance == 0) {
		return false;
	}
	float height = impact.y + lift;

	// Where the search starts: the last solve's angle, or straight at the
	// target; the high arc from 3 pi / 8 unless the last was already over
	// pi / 4, pulled halfway to 1.508 each time and searched with the step
	// reversed (lowering the high arc lengthens it).
	float angle;
	if (high && m_angle < 0.7853982f) {
		angle = kHighStart;
	} else if (!noSpeed && m_angle != 666) {
		angle = m_angle;
	} else {
		angle = std::atan2(height, distance);
	}
	m_angleStep = sign(m_angleStep) * std::max(std::fabs(m_angleStep), kMinStep);
	if (high && !noSpeed) {
		angle = (angle + kHighPull) * 0.5f;
		if (m_angleStep > 0) {
			m_angleStep = -m_angleStep;
		}
	}

	const bool drag = m_round.drag != 0;
	const int steps = static_cast<int>(std::lround(precision * 8 + 2));
	bool bracketed = noSpeed;
	int firstSide = 0; // +1: the round passed under the target, -1: over it
	double hitTime = firstTime;
	for (int i = 0; i < steps; ++i) {
		// The launch velocity in the vertical plane through the target:
		// along the ground (with the shooter's own motion that way) and up.
		const Vec3 ground = normalized({impact.x, 0, impact.z});
		float alongGround = std::cos(angle) * m_round.speed + dot(ground, m_shooterVelocity);
		float up = std::sin(angle) * m_round.speed + m_shooterVelocity.y;
		if (alongGround <= 0.1f) {
			return false;
		}
		float time = drag ? flightTime(distance, alongGround) : distance / alongGround;
		if (time > 60) {
			// Too far round: start again straight at where the first guess
			// put the target.
			angle = std::atan2(height, distance);
			m_angle = angle;
			alongGround = std::cos(angle) * m_round.speed + dot(ground, m_shooterVelocity);
			up = std::sin(angle) * m_round.speed + m_shooterVelocity.y;
			impact = targetAt(firstTime);
			distance = across(impact);
			time = drag ? flightTime(distance, alongGround) : distance / alongGround;
		}
		if (time > 100) {
			return false;
		}
		const float roundHeight = flight({alongGround, up, 0}, time).y;
		m_halfFlight = time * 0.5f;
		hitTime = time + now;
		impact = targetAt(hitTime);
		height = impact.y + lift;
		distance = across(impact);
		if (!std::isfinite(distance) || !std::isfinite(height)) {
			return false;
		}
		if (noSpeed || i == steps - 1) {
			break;
		}
		const int side = height <= roundHeight ? -1 : 1;
		if (side < 0 ? angle <= -kMaxAngle : angle >= kMaxAngle) {
			return false;
		}
		if (firstSide == 0) {
			firstSide = side;
		} else if (side != firstSide) {
			bracketed = true;
		}
		if (bracketed) {
			m_angleStep *= 0.5f;
		}
		angle = std::clamp(angle + m_angleStep * static_cast<float>(side), -kMaxAngle, kMaxAngle);
	}
	m_angle = angle;
	if (!bracketed) {
		// Never crossed the target: a wider step next time.
		const bool wide = std::fabs(m_angleStep) >= kMaxStep;
		m_angleStep = std::min(m_angleStep * 2, kMaxStep);
		if (wide) {
			return true;
		}
	}
	const float c = std::cos(angle) / distance;
	Vec3 d {c * impact.x, std::sin(angle), c * impact.z};
	const float l = length(d);
	if (!(l >= 0.95f)) {
		return false;
	}
	direction = d * (1 / l);
	if (impactTime) {
		*impactTime = hitTime;
	}
	return true;
}

OnTargetTest::OnTargetTest(float reach, bool closestApproach)
	: m_reach(reach)
	, m_reachSquared(reach * reach)
	, m_closestApproach(closestApproach)
	, m_best(FLT_MAX)
	, m_maxAngle(std::atan(reach * 0.0125f))
{
}

bool OnTargetTest::test(bool solved, const LeadSolver& solver, const Vec3& direction, const Vec3& impact,
	double impactTime, double now)
{
	if (!solved) {
		return false;
	}
	const float off = std::acos(std::clamp(dot(direction, solver.gunForward()), -1.0f, 1.0f));
	if (off > m_maxAngle) {
		return false;
	}
	Vec3 round;
	if (!solver.roundAt(solver.gunForward(), static_cast<float>(impactTime - now), round)) {
		return false;
	}
	const Vec3 d = round - impact;
	const float miss = dot(d, d);
	if (!m_closestApproach) {
		return miss <= m_reachSquared;
	}
	// Not automatic: hold back while the miss shrinks; on the first test it
	// grows again (or with a best of a centimetre or less), fire when the
	// best was within reach.
	if (m_best <= 0.01f || miss >= m_best) {
		const bool hit = m_best <= m_reachSquared;
		m_best = FLT_MAX;
		return hit;
	}
	m_best = miss;
	return false;
}

} // namespace dice::hfe::ai
