// The vehicle models other than newCar2: the turning parts of the template
// tree, the old Engine object (planes, helicopters, ships, cars, tanks:
// vtable 0x00987e30), the Wing's physics node, the ForceObject's keepUp
// couple, the MechLegs object and response, the PCO's vertical input
// regulator and the old path of the wheel node (0x006b8230). See
// engine.md, "Other vehicles".
//
// Forces here are accelerations (the node's slots 0xa0 / 0xa4 add them to
// the acceleration without dividing by the mass, 0x006c7b10, 0x006c7c00:
// RigidBody::addForce), the same for any mass.
#include "Vehicle.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dice::hfe::game {

namespace {

using physics::Mat3;

constexpr float kDegrees = 0.017453292f;  // 0x00a06968's inverse
constexpr float kSpringScale = -0.101833f; // times Physics.gravity
constexpr float kTick = 1.0f / 30.0f;

float sign(float x) { return x > 0 ? 1.0f : x < 0 ? -1.0f : 0.0f; }
float length(const Vec3& v) { return std::sqrt(physics::lengthSquared(v)); }
Vec3 clampLength(const Vec3& v, float limit)
{
	const float l = length(v);
	return l > limit && l > 0 ? v * (limit / l) : v;
}
// Moving towards a target by at most `step` (0x0064c590).
float moveToward(float value, float target, float step)
{
	return value < target ? std::min(target, value + step) : std::max(target, value - step);
}

} // namespace

int vehicleInputIndex(const std::string& name)
{
	std::string n = name;
	if (n.size() > 2 && (n[0] == 'c' || n[0] == 'C') && n[1] == '_') {
		n = n.substr(2);
	}
	if (n == "PIYaw") {
		return 0;
	}
	if (n == "PIPitch") {
		return 1;
	}
	if (n == "PIRoll") {
		return 2;
	}
	if (n == "PIThrottle") {
		return 3;
	}
	if (n == "PICrouch") {
		return 4;
	}
	if (n == "PISprint") {
		return 5;
	}
	return -1;
}

float vehicleInputValue(const VehicleInput& input, int index)
{
	switch (index) {
	case 0:
		return input.steer;
	case 1:
		return input.pitch;
	case 2:
		return input.roll;
	case 3:
		return input.throttle;
	case 4:
		return input.crouch;
	case 5:
		return input.sprint ? 1.0f : 0.0f;
	default:
		return 0;
	}
}

float wingLift(const Vec3& v, const Vec3& n, float scale)
{
	const float s = length(v);
	if (s == 0) {
		return 0;
	}
	const float nl = length(n);
	const Vec3 u = v * (1.0f / s);
	const Vec3 unit = nl > 0 ? n * (1.0f / nl) : n;
	const float c = std::clamp(physics::dot(u, unit), -1.0f, 1.0f);
	float k = 0;
	if (c != 0) {
		const float a = std::asin(c) * 57.29578f;
		if (std::fabs(a) < 45.0f) {
			k = (a < 0 ? 45.0f + a : 45.0f - a) * a * 0.0019753086f; // peaks at +-22.5 degrees
		}
	}
	return (k * 0.75f + physics::dot(u, n) * 0.25f) * nl * s * s * scale * 0.0025f;
}

float airDensity(float height) { return 1.0f - std::clamp(height / 1000.0f, 0.0f, 1.0f); }

float Vehicle::OldEngine::gearRatio() const
{
	static const float kTable[] = {4.0f, 4.0f, 2.0f, 1.5f, 1.25f, 1.0f}; // 0x00a052a0
	return gear < 2 ? kTable[1] : gear > 5 ? kTable[5] : kTable[gear];
}

float Vehicle::OldEngine::currentRatio() const
{
	const float table = settings.gearRatio(std::max(gear, 1));
	return settings.differential * 3.5f / (table != 0 ? table : 1.0f);
}

float Vehicle::OldEngine::currentTorque() const
{
	return settings.torqueAt(rpm);
}

// feedbackLoop (0x0064dd10): the push along the engine's axis as a load on
// the rpm: a car's (no bit 4) is the running mean of the tick's pushes x
// 0.99, a tank's the largest the rpm's way; a car's within +-1, a
// helicopter's within +-1.25.
void Vehicle::OldEngine::feedback(float push)
{
	const float torqueNow = currentTorque();
	float p = torqueNow != 0 ? currentRatio() * push / torqueNow : 0.0f;
	if (flags & 2) {
		p = std::clamp(p, -1.0f, 1.0f);
	}
	if (!(flags & 4)) {
		if (loadCount < 100) {
			const float n = static_cast<float>(loadCount);
			load = ((n * load + p) * 0.99f) / (n + 1);
			++loadCount;
		} else {
			load *= 0.99f;
		}
		if (std::fabs(load) < 0.001f) {
			load = 0;
		}
	} else if (rpm >= 0) {
		load = std::max(load, p);
	} else {
		load = std::min(load, p);
	}
	if (flags & 0x20) {
		load = std::clamp(load, -1.25f, 1.25f);
	}
}

bool Vehicle::flies() const
{
	return std::any_of(m_engines.begin(), m_engines.end(), [](const OldEngine& e) {
		return (e.flags & 0x20) && !e.pureRotational;
	});
}

Mat3 Vehicle::partWorld(const physics::RigidBody& body, int part) const
{
	return part >= 0 ? m_parts[static_cast<size_t>(part)].rotation * body.rotation : body.rotation;
}

Vec3 Vehicle::partPoint(const physics::RigidBody& body, int part) const
{
	return part >= 0 ? body.position + body.rotation.transform(m_parts[static_cast<size_t>(part)].position) : body.position;
}

// The PCO's input regulator (0x005c9ec0, from the input filter 0x005ca2e0,
// while occupied): regulateVerticalPos (a, b) on its input (the shipped
// aircraft's PIThrottle) as out = clamp(((a.y - gMod g) b + v.y) a, -1,
// 1) while the vehicle leans under 20 degrees (noVertRegAngle, 0x39c),
// faded out from 10 (maxVertRegAngle, 0x398); it replaces the input when
// that is 0 or smaller the same way. (Yaw, pitch and roll regulators exist
// but no shipped vehicle sets them.)
void Vehicle::regulateInput(const physics::RigidBody& body, float gravity)
{
	m_regulated = m_input;
	if (m_vertical.input < 0 || !m_occupied) {
		return;
	}
	const float up = body.rotation.rows[1].y;
	const float cos20 = 0.9396926f, cos10 = 0.9848077f;
	float out = 0;
	if (up > cos20) {
		const float ay = body.acceleration.y - body.gravityModifier * gravity;
		out = std::clamp((ay * m_vertical.b + body.velocity.y) * m_vertical.a, -1.0f, 1.0f);
		if (up < cos10) {
			out *= (up - cos20) / (cos10 - cos20);
		}
	}
	float v = vehicleInputValue(m_regulated, m_vertical.input);
	if ((out < v && v <= 0) || (out > v && v >= 0)) {
		v = out;
	}
	switch (m_vertical.input) {
	case 0:
		m_regulated.steer = v;
		break;
	case 1:
		m_regulated.pitch = v;
		break;
	case 2:
		m_regulated.roll = v;
		break;
	case 3:
		m_regulated.throttle = v;
		break;
	default:
		break;
	}
}

// Each part's axes on their inputs (the bundles' handlers; an Engine's own
// in updateOldEngine, a Wing's pitch alone, 0x0064fd40), then its frame.
void Vehicle::updateParts(float seconds)
{
	for (size_t i = 0; i < m_parts.size(); ++i) {
		Part& part = m_parts[i];
		if (part.type != "Engine" && part.type != "LandingGear" && part.moves) {
			for (size_t axis = 0; axis < 3; ++axis) {
				if (part.type == "Wing" && axis != 1) {
					continue;
				}
				if (part.axes[axis].maxSpeed == 0 && part.axes[axis].continuousSpeed == 0) {
					continue;
				}
				float in = part.inputs[axis] >= 0 ? vehicleInputValue(m_regulated, part.inputs[axis]) : 0.0f;
				if (part.rememberExcess) {
					part.excess[axis] = std::clamp(part.excess[axis] + in, -40.0f, 40.0f);
					in = std::clamp(part.excess[axis], -1.0f, 1.0f);
					part.excess[axis] -= in;
				}
				rotationAxisStep(part.axes[axis], in, seconds, part.angle[axis], part.rate[axis]);
			}
		}
		const Mat3 turn = Mat3::fromYawPitchRoll({part.angle[0], part.angle[1], part.angle[2]});
		if (part.parent >= 0) {
			const Part& parent = m_parts[static_cast<size_t>(part.parent)];
			part.rotation = turn * part.base * parent.rotation;
			part.position = parent.position + parent.rotation.transform(part.offset);
		} else {
			part.rotation = turn * part.base;
			part.position = part.offset;
		}
	}
}

// handleUpdate (0x0064d4f0) for an Engine of another type than newCar2.
void Vehicle::updateOldEngine(OldEngine& e, physics::RigidBody& body, const physics::Ground& ground, float seconds,
	float gravity)
{
	// The messages (0x0064c890): someone entering starts it (4); leaving
	// (5) stops a ground engine half a second later, an air or sea one
	// keeps running (unless it is on the ground: the rewrite stops it too).
	if (m_occupied) {
		e.running = e.heat < 1 || e.heatIncrement <= 0 ? true : e.running;
		e.stopTimer = -1;
	} else if (e.running) {
		if (e.stopTimer < 0) {
			e.stopTimer = 0.5f;
		}
		e.stopTimer -= seconds;
		if (e.stopTimer <= 0) {
			e.running = false;
			e.stopTimer = -1;
		}
	}
	if (e.part < 0) {
		return;
	}
	Part& part = m_parts[static_cast<size_t>(e.part)];
	// Its own inputs (0x0064c760): only while running; canReverse 0 drops an
	// input against the axis's acceleration.
	std::array<float, 3> in {};
	for (size_t axis = 0; axis < 3; ++axis) {
		in[axis] = e.running && part.inputs[axis] >= 0 ? vehicleInputValue(m_regulated, part.inputs[axis]) : 0.0f;
		if (!part.canReverse && in[axis] != 0 && sign(in[axis]) != sign(part.axes[axis].acceleration)) {
			in[axis] = 0;
		}
	}
	// A helicopter's collective: the roll input sets the angle of attack
	// (moved at attackSpeed degrees a second), the roll axis itself is
	// driven to +-1 so the rotor spins up.
	if ((e.flags & 0x20) && e.running) {
		const float s = part.axes[2].acceleration > 0 ? 1.0f : -1.0f;
		float target, rate;
		if (e.decreaseAngleToZero && std::fabs(in[2]) < 0.07f
			&& std::fabs(body.velocity.y) < e.decreaseAngleToZeroVerticalVel) {
			target = 0;
			rate = 0; // the template's 0x738 (no console name: 0)
		} else {
			target = ((e.maxAngleOfAttack - e.defaultAngleOfAttack) * std::clamp(s * in[2], -1.0f, 1.0f)
						 + e.defaultAngleOfAttack)
				* kDegrees;
			rate = e.attackSpeed;
		}
		e.aoa = rate <= 0 ? target : moveToward(e.aoa, target, rate * kDegrees * seconds);
		in[2] = s;
	}
	if (e.flags & 0x10) {
		in[2] = 1;
	}
	for (size_t axis = 0; axis < 3; ++axis) {
		if (part.axes[axis].maxSpeed != 0) {
			float input = in[axis];
			if (part.rememberExcess) {
				part.excess[axis] = std::clamp(part.excess[axis] + input, -40.0f, 40.0f);
				input = std::clamp(part.excess[axis], -1.0f, 1.0f);
				part.excess[axis] -= input;
			}
			rotationAxisStep(part.axes[axis], input, seconds, part.angle[axis], part.rate[axis]);
		}
	}
	{
		const Mat3 turn = Mat3::fromYawPitchRoll({part.angle[0], part.angle[1], part.angle[2]});
		const Mat3 parent = part.parent >= 0 ? m_parts[static_cast<size_t>(part.parent)].rotation : Mat3 {};
		part.rotation = turn * part.base * parent;
	}
	const float position = part.axes[2].maxRotation != 0 ? part.angle[2] / part.axes[2].maxRotation : 0.0f;
	e.yaw = part.axes[0].maxRotation > 0 ? part.angle[0] / part.axes[0].maxRotation : 0.0f;
	e.braking = (in[2] < -0.1f && e.rpm > 0) || (in[2] > 0.1f && e.rpm < 0);
	const bool tank = (e.flags & 6) == 6;
	float load = e.load;
	float r;
	if (!(e.flags & 0x20)) {
		float k = 0.05f; // *0xa78ff0
		if (tank) {
			// c2_maxSpeed (0x00a78fbc): differential x 0.75, kept from the
			// first tank engine updated (the shipped tanks' alone matter).
			const float maxSpeed = e.settings.differential * 0.75f;
			load *= std::clamp(1.0f - length(body.velocity) / maxSpeed, 0.0f, 1.0f);
			k = 0.07f; // *0xa78ff4
		}
		r = ((position - load) - e.rpm * 0.5f) * k + e.rpm;
	} else {
		r = ((position - load) - e.rpm * 0.5f) * 0.05f + e.rpm;
	}
	// The tracks' turn: a torque about the up axis while both sides' engine
	// wheels touched last tick (0x0064d440), up to trackTurnSpeed x their
	// mean friction.
	if (tank && std::fabs(r) > 0.01f && (e.trackSides & 3) == 3) {
		const float m = e.trackFriction / static_cast<float>(std::max(e.trackCount, 1));
		if (std::fabs(body.angularVelocity.y) < m * e.trackTurnSpeed) {
			body.torque.y += sign(r) * m * e.trackTurnAcceleration * e.yaw;
		}
	}
	e.trackFriction = 0;
	e.trackCount = 0;
	e.trackSides = 0;
	if (!e.running) {
		r = 0;
		e.rpm = 0;
		e.aoa = 0;
	} else {
		r = std::clamp(r, -1.0f, 1.2f);
	}
	if (e.heatIncrement > 0) {
		e.heat += std::fabs(r) * e.heatIncrement - e.coolingFactor;
		if (e.heat >= 1) {
			e.running = false;
			e.heat = 1;
			r = 0;
		} else if (e.heat < 0) {
			e.heat = 0;
		}
	}
	if (std::fabs(r) < 0.01f) {
		r = 0;
	}
	e.rpm = r;
	if (r > e.settings.gearUp && e.gear < e.settings.numberOfGears) {
		++e.gear;
	}
	if (r < e.settings.gearDown && e.gear > 1) {
		--e.gear;
	}
	e.load = 0;
	e.loadCount = 0;
	engineForce(e, body, ground, gravity);
}

// updatePhysics (0x0064df20): a propeller's thrust (plane, ship) or a
// rotor's lift (helicopter) along the engine's z axis.
void Vehicle::engineForce(OldEngine& e, physics::RigidBody& body, const physics::Ground& ground, float gravity)
{
	(void)gravity;
	if (!(e.flags & 1)) {
		return;
	}
	const float k = m_occupied ? 1.0f : 0.0f; // 0x005c87b0
	const Mat3 m = partWorld(body, e.part);
	Vec3 z = m.rows[2];
	if (e.noPowerWhenUpsideDown && z.y < 0) {
		return;
	}
	const Vec3 at = partPoint(body, e.part);
	const float water = ground.waterLevel ? ground.waterLevel(at.x, at.z) : -10000.0f;
	if (water <= at.y) {
		if ((e.flags & 8) && std::fabs(e.rpm) > 0.02f) {
			e.rpm = 1;
			return; // a ship's screw out of the water
		}
	} else if (!(e.flags & 8)) {
		e.rpm = 0;
		return; // anything else under water
	}
	const float ratio = e.currentRatio();
	const Vec3 offset = at - body.position;
	if (!(e.flags & 0x20)) {
		const float s = std::clamp(physics::dot(z, body.velocity) / e.noPropellerEffectAtSpeed * airDensity(at.y), -0.2f, 5.0f);
		const float d = e.rpm - s;
		const float f = std::fabs(e.rpm) * 0.1f + std::fabs(d) * d;
		e.feedback(f);
		float thrust = ratio * f * k;
		if (m_regulated.sprint && m_sprintFactor > 0) {
			thrust *= m_sprintFactor;
		}
		body.addForce(offset, z * thrust);
		return;
	}
	Vec3 airflow = body.pointVelocity(at);
	if (e.pureRotational) {
		airflow = airflow - body.velocity * (1 - e.airFlowAffect);
	}
	// forceIntoEngine (0x0064deb0) with 0.
	const float f = std::fabs(e.rpm) * 0.1f + std::fabs(e.rpm) * e.rpm;
	e.feedback(f);
	const float t = ratio * f;
	float force = -physics::dot(airflow, z) * ((std::fabs(e.aoa) + 1) / e.noPropellerEffectAtSpeed) + t * e.aoa;
	z.y *= airDensity(at.y);
	const float flat = std::sqrt(body.velocity.x * body.velocity.x + body.velocity.z * body.velocity.z);
	force *= 1 - std::clamp(flat / e.noEffectAtPerpSpeed, 0.0f, 1.0f);
	if (e.rpm < 1.1920929e-07f) {
		force *= 0.05f;
		const float vu = physics::dot(body.velocity, body.rotation.rows[1]);
		if (e.spinWhenNoEngineInAir > 1.1920929e-07f && std::fabs(vu) > 3) {
			force += vu * e.spinWhenNoEngineInAir;
		}
	}
	if (e.pureRotational) {
		force *= 0.5f * k;
		body.addForce(offset, z * force);
		body.addForce(-offset, z * -force);
		return;
	}
	if (force < 20 && e.horizontalDampAngle > 0) {
		const Vec3 h {body.velocity.x, 0, body.velocity.z};
		const float hs = length(h);
		if (hs < e.dampHorizontalVel) {
			if (hs > 0.01f) {
				float mag = hs * e.dampHorizontalVelFactor;
				if (hs > e.dampHorizontalVel * 0.5f) {
					mag -= 2 * (hs - e.dampHorizontalVel * 0.5f) * e.dampHorizontalVelFactor;
				}
				body.addForce(offset, h * (mag * k / hs));
			}
			const float threshold = kDegrees * e.horizontalDampAngle;
			const float tilt = std::acos(std::clamp(body.rotation.rows[1].y, 0.0f, 1.0f));
			if (tilt < threshold) {
				const float sn = std::sin(std::fmod((1 / threshold) / e.horizontalDampAngleFactor * tilt * 1.5707964f,
					6.2831855f));
				z.x *= sn;
				z.z *= sn;
			}
		}
	}
	force *= k;
	body.addForce(offset, z * force);
	if (e.rpm > 1.1920929e-07f) {
		const float x = force * k * (e.horizontalSpeedMagnifier - 1);
		body.addForce({}, {z.x * x, 0, z.z * x});
	}
}

// A Wing (its object's update 0x0064fd40 and its physics node 0x006cb780):
// the lift orientation is the part's frame turned by the flap's share of
// the pitch less pitchOffset about x; the air's velocity at the wing
// (positionOffset from it) through the lift curve, x 50 under water,
// within +-200, pushes against the wing's normal.
void Vehicle::updateWing(WingPart& w, physics::RigidBody& body, const physics::Ground& ground, float seconds,
	float gravity)
{
	if (w.startDelay > 0 && w.waited < w.startDelay) {
		w.waited += seconds;
		return;
	}
	const float total = w.wingLift + w.flapLift;
	if (total == 0 || w.part < 0) {
		return;
	}
	const Part& part = m_parts[static_cast<size_t>(w.part)];
	const float a1 = std::clamp(((part.angle[1] * w.multiply + w.offsetY) * w.flapLift) / total - w.pitchOffset, -360.0f,
		360.0f);
	const Mat3 parent = part.parent >= 0 ? m_parts[static_cast<size_t>(part.parent)].rotation : Mat3 {};
	const Mat3 lift = Mat3::fromYawPitchRoll({0, a1, 0}) * part.base * parent * body.rotation;
	const Vec3 n = lift.rows[1];
	const float scale = total * gravity * kSpringScale;
	const Vec3 p = body.position + body.rotation.transform(part.position + w.positionOffset);
	const float sea = ground.waterLevel ? ground.waterLevel(p.x, p.z) : -10000.0f;
	const Vec3 relative = body.pointVelocity(p); // no wind in the shipped levels (PhysicsSystem +0x18)
	float l = wingLift(relative, n, scale);
	l = sea <= p.y ? l * airDensity(p.y) : l * 50.0f;
	l = std::clamp(l, -200.0f, 200.0f);
	body.addForce(p - body.position, n * -l);
}

// A ForceObject (0x0062c920 with 0x0062b750): its force along its forward
// axis (force x input + constantForce, within maxForceOnObject), and with
// keepUp a couple turning the vehicle upright: the object's offset p from
// the root, its velocity w x p, each regulated axis' 2a v + (a^2 + b^2)
// (goal - p) for the goal (0, |p|, 0) (the linear regulator 0x00667710),
// pushed at p and its opposite at -p, x 15.
void Vehicle::updateForce(ForcePart& f, physics::RigidBody& body, float seconds)
{
	if (f.onlyWhenOccupied && !m_occupied) {
		return;
	}
	if (f.startDelay > 0 && f.waited < f.startDelay) {
		f.waited += seconds;
		return;
	}
	const Vec3 at = partPoint(body, f.part);
	const float in = f.input >= 0 ? vehicleInputValue(m_regulated, f.input) : 0.0f;
	float push = f.force * in + f.constantForce;
	if (f.maxForce != 0) {
		push = std::clamp(push, -f.maxForce, f.maxForce);
	}
	if (push != 0) {
		body.addForce(at - body.position, partWorld(body, f.part).rows[2] * push);
	}
	if (!f.keepUp || (f.stopWhenUnoccupied && !m_occupied)) {
		return;
	}
	const Vec3 p = at - body.position;
	const Vec3 v = physics::cross(body.angularVelocity, p);
	const Vec3 goal {0, length(p), 0};
	const float pv[3] = {p.x, p.y, p.z}, vv[3] = {v.x, v.y, v.z}, gv[3] = {goal.x, goal.y, goal.z};
	float out[3] = {};
	for (size_t i = 0; i < 3; ++i) {
		if (f.regulated[i]) {
			out[i] = 2 * f.a[i] * vv[i] + (f.a[i] * f.a[i] + f.b[i] * f.b[i]) * (gv[i] - pv[i]);
		}
	}
	const Vec3 force {out[0] * 15, out[1] * 15, out[2] * 15};
	body.addForce(p, force);
	body.addForce(-p, -force);
}

// The MechLegs object's update (0x00650e90, inputs 0x00650b20, the state
// 0x00650680): the inputs taken 10 ticks after a change of state; walking
// at 7.5 m/s (x 1.3 sprinting), backwards at 4.5, turning on the spot at
// 0.25 rad/s a tick; the feet's probes give a sideways push when one foot
// is off the ground (30, the turn's balance).
// LandingGear's update (0x00650120): the height over what lies below (a
// ray down from the gear, gearUpHeight long, 0x00a85220 slot 0x68; none:
// the largest float) and the root's speed. Down (+0x250 clear) and under
// gearDownHeight and gearDownSpeed: the down delay starts and the gear
// goes down; at gearUpHeight and gearUpSpeed or more: the up delay starts
// and it goes up. Once its delay has run out each axis gets an input of +1
// going up and -1 going down (the other way with invertGear). After more
// than 5 ticks without an update (*0x00a0553c; the first one) the axes
// stand at their limits at once: max for a gear up (flipped by invertGear
// and by a negative maxSpeed x acceleration), else min. The rewrite's ray
// meets only the terrain.
void Vehicle::updateGear(GearPart& g, const physics::RigidBody& body, const physics::Ground& ground, float seconds)
{
	if (g.part < 0) {
		return;
	}
	Part& part = m_parts[static_cast<size_t>(g.part)];
	const bool snap = static_cast<unsigned>(m_tick - g.lastTick) > 5u;
	g.lastTick = m_tick;
	const Vec3 at = partPoint(body, g.part);
	float height = std::numeric_limits<float>::max();
	if (ground.height) {
		const float below = at.y - ground.height(at.x, at.z);
		if (below >= 0 && below <= g.upHeight) {
			height = below;
		}
	}
	const float speed2 = physics::lengthSquared(body.velocity);
	if (height < g.downHeight && speed2 < g.downSpeed * g.downSpeed && g.up) {
		g.downTimer = g.downDelay;
		g.up = false;
	}
	float input = 0;
	bool hold = false;
	if (height >= g.upHeight && speed2 >= g.upSpeed * g.upSpeed && !g.up) {
		g.upTimer = g.upDelay;
		g.up = true;
	}
	if (g.up) {
		if (g.upTimer > 0) {
			g.upTimer -= seconds;
			hold = true;
		} else {
			input = g.invert ? -1.0f : 1.0f;
		}
	} else if (g.downTimer > 0) {
		g.downTimer -= seconds;
		hold = true;
	} else {
		input = g.invert ? 1.0f : -1.0f;
	}
	for (size_t axis = 0; axis < 3; ++axis) {
		RotationAxis& a = part.axes[axis];
		if (snap) {
			const float k = a.maxSpeed * a.acceleration;
			if (k != 0) {
				bool high = g.up != g.invert;
				if (k < 0) {
					high = !high;
				}
				part.angle[axis] = high ? a.maxRotation : a.minRotation;
			}
		}
		if (!hold && (a.maxSpeed != 0 || a.continuousSpeed != 0)) {
			rotationAxisStep(a, input, seconds, part.angle[axis], part.rate[axis]);
		}
	}
}

// A FloatingBundle's node (0x006ca860). Its bundle (0x0064f420) steps the
// pitch axis only with a parent and a maxRotation.y over 0 (no shipped
// float has one) and hands the node minus that angle. The node, from the
// float's world point p and the water's height w there (HeightmapCluster
// slot 0x98):
//   c = ((p.y - hullHeight - w) + offset) / hullHeight, s = clamp(input - c
//   hullHeight, 0, 1); below (c < 0, c at least -1):
//   lift = (s floatMaxLift + (1 - s) floatMinLift) x gravity x c x 0.101833
//   k = clamp((25 c + 1 - c) x drag x footprint x 10 / mass, -2, 2)
//   (*0x00a070b8; drag the root node's slot 0xfc, +0x50; the footprint the
//   root's vehicle-LOD box, x by z, 0x006aeda0's +0x24 x +0x2c)
// and pushes the root up by k x (the point's vertical velocity) + lift at p
// (slot 0xa0, 0x006c7b10: an acceleration at a world point, no mass
// division).
void Vehicle::updateFloater(FloaterPart& f, physics::RigidBody& body, const physics::Ground& ground, float seconds,
	float gravity)
{
	(void)seconds;
	if (f.part < 0 || !ground.waterLevel || f.hullHeight == 0) {
		return;
	}
	const Part& part = m_parts[static_cast<size_t>(f.part)];
	if (part.parent >= 0 && part.axes[1].maxRotation > 0) {
		f.input = -part.angle[1];
	}
	const Vec3 p = partPoint(body, f.part);
	const float water = ground.waterLevel(p.x, p.z);
	float c = ((p.y - f.hullHeight - water) + f.offset) / f.hullHeight;
	const float s = std::clamp(-(c * f.hullHeight - f.input), 0.0f, 1.0f);
	if (c >= 0) {
		return;
	}
	c = std::max(c, -1.0f);
	const float lift = (s * f.maxLift + (1 - s) * f.minLift) * gravity * c * 0.101833f;
	float k = (c * 25.0f + (1 - c)) * m_drag * m_footprint * 10.0f;
	k = std::clamp(k / std::max(m_mass, 1e-3f), -2.0f, 2.0f);
	const float vy = body.pointVelocity(p).y;
	body.addForce(p - body.position, {0, k * vy + lift, 0});
}

void Vehicle::updateLegs(physics::RigidBody& body, const physics::Ground& ground, float seconds)
{
	(void)seconds;
	Legs& l = m_legs;
	if (m_tick >= l.stateTick + 10) {
		l.throttle = std::clamp(m_regulated.throttle, -1.0f, 1.0f);
		if (std::fabs(l.throttle) < 0.001f) {
			l.throttle = 0;
		}
		l.yaw = std::clamp(m_regulated.steer, -1.0f, 1.0f);
		l.crouch = std::clamp(m_regulated.crouch, 0.0f, 1.0f);
	}
	// 0x00650680: the state follows the inputs only while no playing
	// bundle of the walker's animation system blocks input (slot 0x50).
	if (!l.animationBlocks) {
		const int state = l.crouch > 0 ? 5 : l.throttle != 0 ? (l.throttle > 0 ? 1 : 2) : l.yaw != 0 ? (l.yaw > 0 ? 4 : 3) : 0;
		if (state != l.state) {
			l.state = state;
			l.stateTick = m_tick;
		}
	}
	if (!m_occupied) {
		l.state = 0;
	}
	// 0x00650530 (each tick from 0x00650e90): the movement messages
	// cleared (0x780), then the state's while its input's sign still holds
	// (0x00650600); the crouch flag only in state 5.
	{
		const bool holds = (l.state == 1 && l.throttle > 0) || (l.state == 2 && l.throttle < 0)
			|| (l.state == 3 && l.yaw < 0) || (l.state == 4 && l.yaw > 0) || (l.state == 5 && l.crouch > 0);
		l.messages &= ~0x780u;
		l.crouching = false;
		if (holds) {
			l.messages |= l.state == 1 ? 0x80u : l.state == 2 ? 0x100u : l.state == 3 ? 0x400u : l.state == 4 ? 0x200u : 0u;
			l.crouching = l.state == 5;
		}
	}
	const Mat3 m = partWorld(body, l.part);
	const Vec3 at = partPoint(body, l.part);
	// The animation's speed factor (the root's system, slot 0x54: 0x008ba420),
	// 1 for a vehicle without one.
	const float k = l.speedFactor;
	const float target = l.state == 1 ? 7.5f * (m_regulated.sprint ? 1.3f : 1.0f) : l.state == 2 ? -4.5f : 0.0f;
	const float turn = l.state == 4 ? 1.0f : l.state == 3 ? -1.0f : 0.0f;
	const bool stop = l.speed > 0 && target == 0;
	l.speed += (target - l.speed) * (std::fabs(l.speed) <= std::fabs(target) ? 0.9f : 1.0f);
	if (std::fabs(l.speed) < 0.001f) {
		l.speed = 0;
	}
	l.desired = m.rows[2] * (l.speed * k);
	if (stop) {
		body.velocity = l.desired;
	}
	const Vec3 up = m.rows[1];
	if (turn != 0) {
		body.angularVelocity += up * (turn * 0.25f * k);
	} else if (l.state == 1 && l.yaw != 0) {
		body.angularVelocity += up * (l.yaw * 0.25f);
	} else if (l.state == 2 && l.yaw != 0) {
		body.angularVelocity += up * (-l.yaw * 0.25f);
	}
	// The feet: a metre's probe down from 1.7 m to either side, 2.3 down.
	const Vec3 right = m.rows[0];
	if (ground.height) {
		const Vec3 left = at - right * 1.7f - up * 2.3f, rightFoot = at + right * 1.7f - up * 2.3f;
		if (ground.height(left.x, left.z) >= left.y - 1.0f) {
			l.footLeft = 10;
		}
		if (ground.height(rightFoot.x, rightFoot.z) >= rightFoot.y - 1.0f) {
			l.footRight = 10;
		}
	}
	const int bits = (l.footLeft > 0 ? 1 : 0) | (l.footRight > 0 ? 2 : 0);
	l.footLeft = std::max(0, l.footLeft - 1);
	l.footRight = std::max(0, l.footRight - 1);
	if (m_tick >= l.pushTick && bits == 1) {
		body.addForce({}, right * 30.0f);
	} else if (m_tick >= l.pushTick && bits == 2) {
		body.addForce({}, right * -30.0f);
	} else if (bits == 0 && std::fabs(body.velocity.y) < 6) {
		if (m_tick >= l.pushTick) {
			l.push = right.y >= 0 ? -30.0f : 30.0f;
		}
		body.addForce({}, right * l.push);
		l.pushTick = m_tick + 30;
	}
}

// The MechLegs response (0xc432): the legs' segment from stepHeight above
// the foot down to it (length under the legs' object) against the
// heightmap (0x006cf5c0): the body pushed up by the depth and the approach
// taken out at the foot (0x006ce740, no friction); then the drive
// (0x006cdca0): the velocity along the ground moved to the legs' desired
// one by at most 1 m/s a tick, a quarter of the turn damped.
void Vehicle::legsRespond(physics::RigidBody& body, const physics::Ground& ground, float seconds, float gravity)
{
	(void)seconds;
	(void)gravity;
	if (!m_hasLegs || !ground.heightAndNormal) {
		return;
	}
	Legs& l = m_legs;
	const Mat3 m = partWorld(body, l.part);
	const Vec3 at = partPoint(body, l.part);
	const Vec3 up = m.rows[1];
	const Vec3 foot = at - up * l.length;
	const Vec3 top = foot + up * l.stepHeight;
	Vec3 normal {0, 1, 0};
	const float groundY = ground.heightAndNormal(foot.x, foot.z, normal);
	l.grounded = groundY >= foot.y && groundY <= top.y + 0.01f;
	if (!l.grounded && groundY > top.y) {
		// Sunk past the step: the segment starts under the ground; the
		// heightmap's segment test meets nothing (the hull's own LOD then
		// lands it, as the original's Basic points do not: rewrite's guard).
		l.grounded = true;
	}
	Vec3 contactNormal {0, 1, 0};
	l.objectNormal = false;
	if (l.grounded) {
		const Vec3 point {foot.x, groundY, foot.z};
		const Vec3 r = point - at;
		const float rl = length(r);
		// The normal from the foot's contact towards the legs (mode 1); the
		// rewrite's guard for a contact level with or above them: up.
		const Vec3 n = rl > 0 && r.y < -0.1f ? r * (-1.0f / rl) : Vec3 {0, 1, 0};
		contactNormal = n;
		const float depth = foot.y - groundY; // negative below the ground
		body.position += n * -depth;
		const Vec3 v = body.pointVelocity(point);
		const float approach = -physics::dot(n, v);
		if (approach > 0) {
			body.applyImpulse(point - body.position, clampLength(n * approach, 1000.0f));
		}
		l.bestNormal = normal;
	} else {
		l.bestNormal = {};
	}
	l.contactNormal = contactNormal;
}

namespace {
// 0x006b6b60: an axis of a contact normal taken into the drive's: of the
// same sign the larger, of opposite signs (or into 0) the sum.
float combineAxis(float into, float value)
{
	if (into == 0) {
		return value;
	}
	if (into > 0 && value > 0) {
		return std::max(into, value);
	}
	if (into < 0 && value < 0) {
		return std::min(into, value);
	}
	return into + value;
}
} // namespace

void Vehicle::legsObjectContact(const Vec3& normal, const Vec3& queryNormal, bool below)
{
	Legs& l = m_legs;
	l.contactNormal = {combineAxis(l.contactNormal.x, normal.x), combineAxis(l.contactNormal.y, normal.y),
		combineAxis(l.contactNormal.z, normal.z)};
	l.objectNormal = true;
	// Below the legs (0x006cec60's bVar3): the best ground normal by its
	// height (+0x1c4, its y at +0x1c8) and the landed flag (+0x21c).
	if (below && l.bestNormal.y < queryNormal.y) {
		l.bestNormal = queryNormal;
		l.grounded = true;
		l.landed = true;
	}
}

// The drive (0x006cdca0): the velocity's part along the ground (off the
// contact normal) moved towards the legs' desired one, by the best ground
// normal's height, at most 1 m/s, as a force of 30 at the legs; a quarter
// of the turn damped (*0x00a072fc). The contact normals are reset after.
void Vehicle::legsDrive(physics::RigidBody& body)
{
	if (!m_hasLegs) {
		return;
	}
	Legs& l = m_legs;
	const Vec3 at = partPoint(body, l.part);
	const Vec3 e = body.velocity - l.desired;
	const Vec3 along = e - l.contactNormal * physics::dot(e, l.contactNormal);
	Vec3 dv = along * (-std::max(0.0f, l.bestNormal.y));
	dv = clampLength(dv, 1.0f);
	body.addForce(at - body.position, dv * 30.0f);
	body.angularVelocity = body.angularVelocity + body.angularVelocity * -0.25f;
	l.contactNormal = {0, 1, 0};
}

// The track's speed at a wheel (0x0064c440): a tank's sides by the yaw,
// the inside slower and the outside faster, each within +-1 of the rpm.
float Vehicle::trackSpeed(const OldEngine& e, float side, const physics::RigidBody& body) const
{
	if ((e.flags & 6) != 6 || side == 0) {
		return e.rpm;
	}
	const float k = 1.5f * std::clamp(2.5f - length(body.velocity) / 20.0f, 0.0f, 1.0f);
	const float speed = side < 0 ? (1 + k * e.yaw) * e.rpm : (1 - k * e.yaw) * e.rpm;
	return std::clamp(speed, -1.0f, 1.0f);
}

// The wheel node's old path (0x006b8230, for wheels under no newCar2
// engine): the spring as newCar2's, then while touching the ground a
// friction circle on the slip: an EngineGrip wheel (8) under an engine
// moves the ground at its track's speed (the rpm x differential x 3.5 /
// the gear table), others hold still (ContactGrip, DummyGrip); within
// 0.491 x friction x n.y (static: 0.7365) a tick; EngineDummyGrip (0x80)
// wheels only push.
void Vehicle::updateOldWheel(Wheel& wheel, physics::RigidBody& body, const physics::Ground& ground, float seconds,
	float grip, float gravity)
{
	if (wheel.grip >= 0) {
		grip = wheel.grip;
	}
	const Vec3 up = body.rotation.rows[1];
	const Vec3 centre = body.position + body.rotation.transform(wheel.mount);
	const Vec3 bottom = centre - up * wheel.radius;
	Vec3 normal {0, 1, 0};
	float groundHeight = ground.heightAndNormal(bottom.x, bottom.z, normal);
	Vec3 surfaceVelocity;
	if (wheel.objectGround.count > 0 && wheel.objectGround.height > groundHeight) {
		groundHeight = wheel.objectGround.height;
		const float l = length(wheel.objectGround.normal);
		normal = l > 1e-6f ? wheel.objectGround.normal * (1.0f / l) : Vec3 {0, 1, 0};
		surfaceVelocity = wheel.objectGround.velocity;
		grip = wheel.objectGround.grip;
	}
	wheel.objectGround = {};
	const float compression = std::max(0.0f, groundHeight - bottom.y);
	wheel.contact = compression > 0;
	// The Spring node's first step that finds it moving (0x006caef0's
	// +0x284, set by 0x006caae0) takes its displacement's rate as 0, so a
	// vehicle spawned with its wheels in the ground isn't kicked out.
	float compressionSpeed = (compression - wheel.compression) / seconds;
	if (wheel.firstStep && compressionSpeed != 0) {
		wheel.firstStep = false;
		compressionSpeed = 0;
	}
	wheel.compression = compression;
	if (wheel.contact || compressionSpeed != 0) {
		const float push = compression * wheel.strength * (gravity * kSpringScale) + compressionSpeed * wheel.damping;
		body.addForce(centre - body.position, {0, push, 0});
	}
	if ((wheel.gripFlags & 0x80) || !wheel.contact) {
		wheel.surfaceSpeed *= 0.99f; // 0xa84c1c
		wheel.angle += wheel.surfaceSpeed / std::max(wheel.spinRadius, 0.1f) * seconds;
		return;
	}
	const Vec3 point {bottom.x, groundHeight, bottom.z};
	Vec3 v = body.pointVelocity(point) - surfaceVelocity;
	v.y += gravity * kTick;
	const Vec3 vt = v - normal * physics::dot(v, normal);
	const Vec3 forward = partWorld(body, wheel.part).rows[2];
	OldEngine* engine = wheel.engine >= 0 && (wheel.gripFlags & 8) ? &m_engines[static_cast<size_t>(wheel.engine)] : nullptr;
	Vec3 dv;
	if (engine) {
		const float w = trackSpeed(*engine, wheel.mount.x, body) * engine->settings.differential * 3.5f / engine->gearRatio();
		const Vec3 t = engine->braking ? Vec3 {} : forward * w;
		const Vec3 tt = t - normal * physics::dot(t, normal);
		dv = tt - vt;
		wheel.surfaceSpeed = physics::dot(tt, forward);
	} else {
		dv = -vt;
		wheel.surfaceSpeed *= 0.99f;
	}
	wheel.angle += wheel.surfaceSpeed / std::max(wheel.spinRadius, 0.1f) * seconds;
	const float limit = normal.y * grip;
	const float dynamic = -gravity * kTick * limit;     // 0.491 x friction
	const float stick = -gravity * kTick * 1.5f * limit; // 0.7365 x friction
	Vec3 change;
	if (!wheel.sticky) {
		change = clampLength(dv, dynamic);
		if (length(dv) <= dynamic) {
			wheel.sticky = true;
		}
	} else if (length(dv) > stick) {
		wheel.sticky = false;
		change = clampLength(dv, dynamic);
	} else {
		change = dv;
	}
	if (engine) {
		engine->feedback(physics::dot(change, forward));
		engine->trackFriction += grip;
		++engine->trackCount;
		engine->trackSides |= wheel.mount.x < 0 ? 1 : wheel.mount.x > 0 ? 2 : 0;
	}
	body.addFriction(point, change * 30.0f);
}

} // namespace dice::hfe::game
