#include "Vehicle.h"

#include "Physics/CollisionMeshFile.h"
#include "World/ObjectTemplate.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace dice::hfe::game {

namespace {

using physics::Mat3;

constexpr float kTick = 1.0f / 30.0f;         // 0x3d088889, the spin step's time
constexpr float kTicksPerSecond = 30;         // tyre forces are per tick
constexpr float kDriveTorqueScale = 1.75f;    // 0x006b7910
constexpr float kRpmScale = 3.5f * 9.549296f; // rad/s to rpm with the 3.5 of 0x006b7070
constexpr float kSpringScale = -0.101833f;    // times Physics.gravity (0x006caef0)
constexpr int kLoadExponent = 3;              // 0x00a83e9c
constexpr float kGripScale = 1;               // 0x00a83e90
constexpr float kSlopeInFirstGear = 0;        // 0x00a83ef4
constexpr float kBrakeLatSpeed = 1.5f;        // 0x00a83ef8
constexpr float kAirSpinDamping = 0.99f;      // provisional

const std::string* setting(const world::ObjectTemplate& t, const char* name)
{
	const auto it = t.settings().find(name);
	return it == t.settings().end() ? nullptr : &it->second;
}

float number(const world::ObjectTemplate& t, const char* name, float fallback)
{
	const std::string* value = setting(t, name);
	return value ? static_cast<float>(std::atof(value->c_str())) : fallback;
}

Vec3 vector(const world::ObjectTemplate& t, const char* name, Vec3 fallback = {})
{
	if (const std::string* value = setting(t, name)) {
		std::sscanf(value->c_str(), "%f/%f/%f", &fallback.x, &fallback.y, &fallback.z);
	}
	return fallback;
}

Vec3 normalized(const Vec3& v)
{
	const float length = std::sqrt(physics::lengthSquared(v));
	return length > 1e-6f ? v * (1.0f / length) : v;
}

float sign(float x) { return x > 0 ? 1.0f : x < 0 ? -1.0f : 0.0f; }

// The wheel's radius: how far its vehicle collision triangle (the part's
// LOD 1, in the part's space) reaches below its centre.
float wheelRadius(const physics::CollisionMeshFile* mesh, int part)
{
	if (!mesh || part < 0 || static_cast<size_t>(part) >= mesh->geometries.size()) {
		return 0.5f;
	}
	for (const auto& sub : mesh->geometries[static_cast<size_t>(part)].subGeometries) {
		if (sub.lods.size() >= 2 && -sub.lods[1].boundsMin[1] > 0.05f) {
			return -sub.lods[1].boundsMin[1];
		}
		if (!sub.lods.empty()) {
			return std::max(0.1f, (sub.lods[0].boundsMax[1] - sub.lods[0].boundsMin[1]) * 0.5f);
		}
	}
	return 0.5f;
}

// The table lookup of interpolated settings (0x00627cd0), as the original
// does it: the nearest sample and a fraction that may be negative.
float sampleTable(const std::array<float, 102>& table, float x)
{
	x = std::min(x, 1.0f) * 100.0f;
	const int i = std::clamp(static_cast<int>(std::nearbyint(x)), 0, 100);
	const float f = x - static_cast<float>(i);
	return f * table[static_cast<size_t>(i) + 1] + (1 - f) * table[static_cast<size_t>(i)];
}

} // namespace

EngineSettings::EngineSettings() { gearTable.fill(1.0f); }

EngineSettings EngineSettings::fromTemplate(const world::ObjectTemplate& t)
{
	EngineSettings s;
	s.engineType = static_cast<int>(number(t, "engineType", 0));
	s.numberOfGears = std::max(1, static_cast<int>(number(t, "numberOfGears", static_cast<float>(s.numberOfGears))));
	s.differential = number(t, "differential", s.differential);
	s.torque = number(t, "torque", s.torque);
	s.gearUp = number(t, "gearUp", s.gearUp);
	s.gearDown = number(t, "gearDown", s.gearDown);
	s.gearChangeTime = number(t, "gearChangeTime", s.gearChangeTime);
	if (const std::string* ratios = setting(t, "gearRatios")) {
		std::array<float, 5> r {1, 1, 1, 1, 1};
		std::sscanf(ratios->c_str(), "%f/%f/%f/%f/%f", &r[0], &r[1], &r[2], &r[3], &r[4]);
		s.setGearRatios(r);
	}
	s.minRpm = number(t, "newCar2.minRpm", s.minRpm);
	s.maxRpm = number(t, "newCar2.maxRpm", s.maxRpm);
	s.useClutchedTorque = number(t, "newCar2.useClutchedTorque", 0) != 0;
	s.wheelInertia = number(t, "newCar2.wheelInertia", s.wheelInertia);
	s.engineFeedbackMod = number(t, "newCar2.engineFeedbackMod", s.engineFeedbackMod);
	s.brakeTorque = number(t, "newCar2.brakeTorque", s.brakeTorque);
	s.engineBrakeTorque = number(t, "newCar2.engineBrakeTorque", s.engineBrakeTorque);
	s.frictionTorque = number(t, "newCar2.frictionTorque", s.frictionTorque);
	s.frictionMod = number(t, "newCar2.frictionMod", s.frictionMod);
	s.slopeTorqueMod = number(t, "newCar2.slopeTorqueMod", s.slopeTorqueMod);
	s.wheelLongFrictionMod = number(t, "newCar2.wheelLongFrictionMod", s.wheelLongFrictionMod);
	s.wheelLongDriveFrictionMod = number(t, "newCar2.wheelLongDriveFrictionMod", s.wheelLongDriveFrictionMod);
	s.wheelLongPeakAt = number(t, "newCar2.wheelLongPeakAt", s.wheelLongPeakAt);
	s.wheelLongMinDynamicFriction = number(t, "newCar2.wheelLongMinDynamicFriction", s.wheelLongMinDynamicFriction);
	s.wheelLongScale = number(t, "newCar2.wheelLongScale", s.wheelLongScale);
	s.friLatBrakeMod = number(t, "friLatBrakeMod", s.friLatBrakeMod);
	s.wheelLatFrictionMod = number(t, "newCar2.wheelLatFrictionMod", s.wheelLatFrictionMod);
	s.wheelLatDriveFrictionMod = number(t, "newCar2.wheelLatDriveFrictionMod", s.wheelLatDriveFrictionMod);
	s.wheelLatPeakAt = number(t, "newCar2.wheelLatPeakAt", s.wheelLatPeakAt);
	s.wheelLatMinDynamicFriction = number(t, "newCar2.wheelLatMinDynamicFriction", s.wheelLatMinDynamicFriction);
	s.wheelLatScale = number(t, "newCar2.wheelLatScale", s.wheelLatScale);
	return s;
}

// The five ratios are keys at samples 20, 40 ... 100; samples between keys
// are linear (0x0061ecf0), from sample 0's 1.
void EngineSettings::setGearRatios(const std::array<float, 5>& ratios)
{
	float previous = gearTable[0];
	for (size_t key = 0; key < ratios.size(); ++key) {
		const size_t from = key * 20, to = from + 20;
		for (size_t i = from + 1; i <= to; ++i) {
			const float f = static_cast<float>(i - from) / 20.0f;
			gearTable[i] = previous * (1 - f) + ratios[key] * f;
		}
		previous = ratios[key];
	}
	gearTable[101] = gearTable[100];
}

float EngineSettings::gearRatio(int gear) const
{
	if (gear == 0) {
		return gearRatio(1) * 1.2f;
	}
	gear = std::clamp(gear, 0, numberOfGears);
	return sampleTable(gearTable, static_cast<float>(gear) / static_cast<float>(numberOfGears));
}

float tyreCurve(float x, float peakAt, float minDynamic)
{
	const float s = sign(x);
	const float a = std::fabs(x);
	if (a < peakAt) {
		return a / peakAt * s;
	}
	const float falling = (a - 1.0f) * ((1.0f - minDynamic) / (peakAt - 1.0f)) + minDynamic;
	return std::max(falling, minDynamic) * s;
}

float wheelSpinStep(float spin, float torque, float resistance, float inertia, float seconds)
{
	spin += (1.0f / inertia) * torque * seconds;
	const float slowed = std::fabs(spin) - (1.0f / inertia) * resistance * seconds;
	return slowed < 0 ? 0.0f : slowed * sign(spin);
}

Vec3 tyreForce(const EngineSettings& e, const TyreContact& contact, const Vec3& slip, const Vec3& forward,
	const Vec3& right, float* longitudinal, float* lateral)
{
	float load = std::max(contact.normalY, 0.0f);
	float loadPower = 1;
	for (int i = 0; i < kLoadExponent; ++i) {
		loadPower *= load;
	}
	float along = tyreCurve(physics::dot(slip, forward) * e.wheelLongScale, e.wheelLongPeakAt,
					  e.wheelLongMinDynamicFriction)
		* e.wheelLongFrictionMod;
	float across = tyreCurve(physics::dot(slip, right) * e.wheelLatScale, e.wheelLatPeakAt,
					   e.wheelLatMinDynamicFriction)
		* e.wheelLatFrictionMod;
	if (contact.drive) {
		along *= e.wheelLongDriveFrictionMod;
		across *= e.wheelLatDriveFrictionMod;
	}
	const float scale = contact.grip * kGripScale * loadPower;
	const float acrossApplied = (contact.brakeLatMod ? e.friLatBrakeMod : 1.0f) * across;
	if (longitudinal) {
		*longitudinal = along;
	}
	if (lateral) {
		*lateral = across;
	}
	return (forward * along + right * acrossApplied) * scale;
}

std::unique_ptr<Vehicle> Vehicle::fromTemplate(const world::ObjectTemplate& root,
	const world::ObjectTemplateManager& templates, const physics::CollisionMeshFile* mesh)
{
	auto vehicle = std::make_unique<Vehicle>();
	bool haveEngine = false;
	struct Steering {
		float maxSteer = 0, speed = 1;
		std::string name;
	};
	// Depth-first over the tree with each node's offset and rotation.
	const std::function<void(const world::ObjectTemplate&, Vec3, Mat3, Steering, int)> visit =
		[&](const world::ObjectTemplate& t, Vec3 position, Mat3 rotation, Steering steering, int depth) {
			if (depth > 8) {
				return;
			}
			const std::string& type = t.typeName();
			if (type == "Engine" && !haveEngine) {
				haveEngine = true;
				vehicle->m_engine = EngineSettings::fromTemplate(t);
				vehicle->m_engineOffset = position;
			} else if (type == "RotationalBundle") {
				const std::string* input = setting(t, "inputToYaw");
				if (input && *input == "PIYaw") {
					steering.maxSteer = vector(t, "maxRotation").x;
					steering.speed = std::max(0.01f, vector(t, "maxSpeed", {1, 0, 0}).x);
					steering.name = t.name();
				}
			} else if (type == "Spring") {
				Wheel wheel;
				wheel.mount = position;
				wheel.radius = wheelRadius(mesh, static_cast<int>(number(t, "collisionPart", -1)));
				wheel.strength = number(t, "strength", 0);
				wheel.damping = number(t, "damping", 0);
				wheel.maxSteer = steering.maxSteer;
				wheel.steerSpeed = steering.speed;
				wheel.spring = t.name();
				wheel.steering = steering.name;
				vehicle->m_wheels.push_back(wheel);
				return;
			}
			for (const world::ChildTemplate& child : t.children()) {
				const world::ObjectTemplate* childTemplate = templates.find(child.name);
				if (!childTemplate) {
					continue;
				}
				const Mat3 local = Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z});
				visit(*childTemplate, position + rotation.transform({child.position.x, child.position.y, child.position.z}),
					local * rotation, steering, depth + 1);
			}
		};
	visit(root, {}, {}, {}, 0);
	if (!haveEngine || vehicle->m_wheels.empty()) {
		return nullptr;
	}
	vehicle->m_mass = number(root, "mass", 1);
	return vehicle;
}

// The engine's part of 0x0064e920 for newCar2: the rpm from the wheels'
// mean, direction changes when nearly stopped, and gears by rpm.
void Vehicle::updateEngine()
{
	const float wheelRpm = m_wheelRpmCount > 0 ? m_wheelRpmSum / static_cast<float>(m_wheelRpmCount) : 0.0f;
	m_wheelRpmSum = 0;
	m_wheelRpmCount = 0;
	const float throttle = m_input.throttle;
	if (std::fabs(m_rpm) < 0.05f) {
		if (m_gear == 1 && throttle < -0.5f) {
			m_gear = 0;
		} else if (m_gear == 0 && throttle > -0.05f) {
			m_gear = 1;
		}
	}
	m_braking = (m_gear >= 1 && throttle < 0) || (m_gear == 0 && throttle > 0);
	float rpm = std::max(0.0f, (std::fabs(wheelRpm) - m_engine.minRpm) / (m_engine.maxRpm - m_engine.minRpm));
	if (m_gear == 0) {
		rpm = -rpm;
	}
	m_rpm = std::clamp(rpm, -1.0f, 1.2f);
	if (m_gear >= 1) {
		if (m_rpm > m_engine.gearUp && m_gear < m_engine.numberOfGears) {
			++m_gear;
		} else if (m_gear >= 2
			&& m_rpm / m_engine.gearRatio(m_gear) * m_engine.gearRatio(m_gear - 1) < 0.55f) {
			--m_gear;
		}
	}
}

void Vehicle::tick(physics::RigidBody& body, const physics::Ground& ground, float seconds, float grip, float gravity)
{
	updateEngine();
	for (Wheel& wheel : m_wheels) {
		updateWheel(wheel, body, ground, seconds, grip, gravity);
	}
}

void Vehicle::updateWheel(Wheel& wheel, physics::RigidBody& body, const physics::Ground& ground, float seconds,
	float grip, float gravity)
{
	// Steering (provisional: maxSpeed degrees per tick, straight back
	// without input).
	if (wheel.maxSteer != 0) {
		const float target = m_input.steer * wheel.maxSteer;
		if (m_input.steer == 0) {
			wheel.steer = 0;
		} else {
			wheel.steer += std::clamp(target - wheel.steer, -wheel.steerSpeed, wheel.steerSpeed);
		}
	}
	const Mat3 wheelRotation = Mat3::fromYawPitchRoll({wheel.steer, 0, 0}) * body.rotation;
	const Vec3 right = wheelRotation.rows[0];
	const Vec3 up = body.rotation.rows[1];
	const Vec3 forward = wheelRotation.rows[2];
	const Vec3 centre = body.position + body.rotation.transform(wheel.mount);
	const Vec3 bottom = centre - up * wheel.radius;
	Vec3 normal {0, 1, 0};
	const float groundHeight = ground.heightAndNormal(bottom.x, bottom.z, normal);
	const float compression = std::max(0.0f, groundHeight - bottom.y);
	wheel.contact = compression > 0;

	// The spring pushes the body by how far, and how fast, the ground
	// pushed the wheel.
	const float compressionSpeed = (compression - wheel.compression) / seconds;
	wheel.compression = compression;
	if (wheel.contact || compressionSpeed != 0) {
		const float push = compression * wheel.strength * (gravity * kSpringScale) + compressionSpeed * wheel.damping;
		body.addForce(centre - body.position, {0, push, 0});
	}

	if (!wheel.contact) {
		wheel.spin *= kAirSpinDamping;
	} else {
		const Vec3 point {bottom.x, groundHeight, bottom.z};
		Vec3 velocity = body.pointVelocity(point);
		velocity.y += gravity * kTick;
		const Vec3 groundVelocity = velocity - normal * physics::dot(velocity, normal);
		TyreContact contact;
		contact.grip = grip;
		contact.normalY = normal.y;
		const Vec3 engine = body.position + body.rotation.transform(m_engineOffset);
		contact.drive = physics::dot(point - engine, body.rotation.rows[2]) < 0;
		contact.brakeLatMod = m_braking && std::fabs(m_input.throttle) > 0.01f
			&& std::fabs(physics::dot(body.velocity, right)) < kBrakeLatSpeed;

		// Driven wheel (0x006b7910): spin from the engine and the slope.
		const Vec3 driveDirection = normalized(forward - normal * physics::dot(forward, normal));
		const float quarterMass = m_mass * 0.25f;
		const float ratio = m_engine.gearRatio(m_gear);
		float throttle = m_input.throttle;
		if ((m_gear < 1 && throttle > 0) || (m_gear >= 1 && throttle <= 0)) {
			throttle = 0; // slot 0x1a4: only towards the gear's direction
		}
		const float engineTorque = m_engine.torque * throttle; // the torque curve is flat (0x374)
		const float driveTorque = engineTorque * m_engine.differential * ratio * kDriveTorqueScale;
		// 0x006b7460: the downhill part of the wheel's share of the weight,
		// along the wheel, times the radius.
		const float weight = gravity * quarterMass;
		const float intoGround = weight * normal.y;
		const float downhill = (-intoGround * normal.x) * forward.x + (weight - intoGround * normal.y) * forward.y
			+ (-intoGround * normal.z) * forward.z;
		const float slopeTorque = downhill * wheel.radius * m_engine.slopeTorqueMod
			* (m_gear == 1 ? kSlopeInFirstGear : 1.0f);
		float resistance = m_engine.frictionTorque;
		if (m_input.throttle == 0) {
			resistance += m_engine.engineBrakeTorque;
		}
		if (m_braking) {
			resistance += m_engine.brakeTorque;
		}
		const float inertia = m_engine.wheelInertia + m_engine.wheelInertia;
		const float spun = wheelSpinStep(wheel.spin, slopeTorque + driveTorque, resistance, inertia, kTick);
		const Vec3 slip = driveDirection * (spun * wheel.radius) - groundVelocity;
		Vec3 force = tyreForce(m_engine, contact, slip, forward, right) * m_engine.frictionMod;

		// The tyre's grip turns the wheel back.
		const float feedback = wheel.radius * physics::dot(force, forward) * m_engine.engineFeedbackMod * quarterMass
			* -kTicksPerSecond;
		float spin = wheelSpinStep(wheel.spin, feedback + slopeTorque + driveTorque, resistance, inertia, kTick);
		const float toRpm = ratio * m_engine.differential * kRpmScale;
		const float maxSpin = m_engine.maxRpm / toRpm;
		spin = sign(spin) * std::min(std::fabs(spin), maxSpin);
		float rpm = toRpm * spin;
		if ((m_gear < 1 && rpm > 0) || (m_gear >= 1 && rpm < 0)) {
			rpm = 0;
			spin = 0;
		}
		wheel.spin = spin;
		m_wheelRpmSum += rpm;
		++m_wheelRpmCount;
		body.addFriction(point, force * kTicksPerSecond);
	}
	// 0x006cace0: the wheel's turn, kept within +-pi.
	wheel.angle += wheel.spin * seconds;
	if (wheel.angle >= 3.1415927f) {
		wheel.angle -= 6.2831855f;
	} else if (wheel.angle < -3.1415927f) {
		wheel.angle += 6.2831855f;
	}
}

} // namespace dice::hfe::game
