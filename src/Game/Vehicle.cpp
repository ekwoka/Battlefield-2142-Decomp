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
	for (const auto& sub : physics::collisionParts(*mesh, static_cast<size_t>(part))) {
		if (sub.lods.size() >= 2 && -sub.lods[1].boundsMin[1] > 0.05f) {
			return -sub.lods[1].boundsMin[1];
		}
		if (!sub.lods.empty()) {
			return std::max(0.1f, (sub.lods[0].boundsMax[1] - sub.lods[0].boundsMin[1]) * 0.5f);
		}
	}
	return 0.5f;
}

// The radius the engine turns the wheel with (the Spring node's
// 0x006cad60): the height (max y - min y, the box's +0x28 from 0x006aeda0)
// of the wheel object's collision box at LOD 0 (projectiles'), plus the
// object's slot 0x94 (0 for the base Object, 0x005b6b80), at least 0.1.
// For as_fav's wheels that is 0.97 m, the wheel's full height: the spin
// and slope torques work with it as the exe has it.
float wheelSpinRadius(const physics::CollisionMeshFile* mesh, int part)
{
	if (!mesh || part < 0 || static_cast<size_t>(part) >= mesh->geometries.size()) {
		return 0.5f;
	}
	for (const auto& sub : physics::collisionParts(*mesh, static_cast<size_t>(part))) {
		if (!sub.lods.empty()) {
			return std::max(0.1f, sub.lods[0].boundsMax[1] - sub.lods[0].boundsMin[1]);
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

// The engineType constants (symbols/BF2142.exe.constants.csv), or a number.
static int engineTypeOf(const std::string& text)
{
	static const std::pair<const char*, int> kTypes[] = {{"c_etcar", 2}, {"c_ethelicopter", 33}, {"c_etnewcar", 66},
		{"c_etnewcar2", 194}, {"c_etnewtank", 70}, {"c_etplane", 1}, {"c_etrocket", 17}, {"c_etship", 9},
		{"c_ettank", 6}, {"c_ettorpedo", 25}};
	std::string l = text;
	std::transform(l.begin(), l.end(), l.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	for (const auto& [name, value] : kTypes) {
		if (l == name) {
			return value;
		}
	}
	return std::atoi(text.c_str());
}

std::unique_ptr<Vehicle> Vehicle::fromTemplate(const world::ObjectTemplate& root,
	const world::ObjectTemplateManager& templates, const physics::CollisionMeshFile* mesh)
{
	auto vehicle = std::make_unique<Vehicle>();
	bool haveEngine = false;
	struct Steering {
		float maxSteer = 0;
		RotationAxis axis;
		std::string name;
	};
	// Where a node hangs: the nearest part above it (-1 the root), its
	// offset and rotation from that part, the engine above it.
	struct Place {
		int part = -1;
		Vec3 offset;
		Mat3 rotation;
		int oldEngine = -1;
		bool newCar2 = false;
	};
	const auto axisOf = [](const world::ObjectTemplate& t, Part& part) {
		const Vec3 lo = vector(t, "minRotation"), hi = vector(t, "maxRotation"), speed = vector(t, "maxSpeed");
		const Vec3 accel = vector(t, "acceleration"), decel = vector(t, "deAcceleration");
		const Vec3 cont = vector(t, "continousRotationSpeed");
		const float lows[3] = {lo.x, lo.y, lo.z}, highs[3] = {hi.x, hi.y, hi.z}, speeds[3] = {speed.x, speed.y, speed.z};
		const float accels[3] = {accel.x, accel.y, accel.z}, decels[3] = {decel.x, decel.y, decel.z};
		const float conts[3] = {cont.x, cont.y, cont.z};
		const char* inputs[3] = {"inputToYaw", "inputToPitch", "inputToRoll"};
		for (size_t i = 0; i < 3; ++i) {
			RotationAxis& a = part.axes[i];
			a.minRotation = lows[i];
			a.maxRotation = highs[i];
			a.maxSpeed = speeds[i];
			a.acceleration = accels[i];
			a.deAcceleration = decels[i];
			a.continuousSpeed = conts[i];
			a.automaticReset = number(t, "automaticReset", 0) != 0;
			a.snapToZero = number(t, "snapToZeroOnNoInput", 0) != 0;
			a.useDeAcceleration = number(t, "useDeAcceleration", 0) != 0;
			a.capMaxSpeed = number(t, "capMaxSpeed", 0) != 0;
			a.maxSpeedMod = number(t, "maxSpeedMod", 0);
			if (const std::string* input = setting(t, inputs[i])) {
				part.inputs[i] = vehicleInputIndex(*input);
			}
			part.moves = part.moves || a.maxSpeed != 0 || a.continuousSpeed != 0;
		}
		part.rememberExcess = number(t, "rememberExcessInput", 0) != 0;
	};
	// Depth-first over the tree with each node's offset and rotation.
	const std::function<void(const world::ObjectTemplate&, Vec3, Mat3, Steering, Place, int)> visit =
		[&](const world::ObjectTemplate& t, Vec3 position, Mat3 rotation, Steering steering, Place place, int depth) {
			if (depth > 8) {
				return;
			}
			const std::string& type = t.typeName();
			const bool isPart = depth > 0
				&& (type == "RotationalBundle" || type == "Engine" || type == "Wing" || type == "ForceObject"
					|| type == "MechLegs" || type == "LandingGear" || type == "FloatingBundle");
			if (isPart) {
				Part part;
				part.parent = place.part;
				part.name = t.name();
				part.type = type;
				part.offset = place.offset;
				part.base = place.rotation;
				if (type != "ForceObject" && type != "MechLegs") {
					axisOf(t, part);
				}
				vehicle->m_parts.push_back(part);
				place.part = static_cast<int>(vehicle->m_parts.size()) - 1;
				place.offset = {};
				place.rotation = {};
			}
			const int self = place.part;
			if (type == "Engine") {
				const int engineType = setting(t, "engineType") ? engineTypeOf(*setting(t, "engineType")) : 0;
				if ((engineType & 0x80) && !haveEngine) {
					haveEngine = true;
					vehicle->m_newCar2 = true;
					vehicle->m_engine = EngineSettings::fromTemplate(t);
					vehicle->m_engine.engineType = engineType;
					vehicle->m_engineOffset = position;
					place.newCar2 = true;
				} else if (!(engineType & 0x80)) {
					OldEngine engine;
					engine.part = self;
					engine.flags = engineType;
					engine.settings = EngineSettings::fromTemplate(t);
					engine.settings.engineType = engineType;
					// The torque curve's default keys (0x006291d0): 0.7, 0.8 at
					// 0.1, 0.9 at 0.3, 1 at 0.6, 0.85 at 0.85, 0.7 at 1.
					const float keys[][2] = {{0, 0.7f}, {0.1f, 0.8f}, {0.3f, 0.9f}, {0.6f, 1.0f}, {0.85f, 0.85f}, {1, 0.7f}};
					for (size_t i = 0; i <= 100; ++i) {
						const float x = static_cast<float>(i) / 100.0f;
						size_t k = 0;
						while (k + 2 < std::size(keys) && x > keys[k + 1][0]) {
							++k;
						}
						const float f = (x - keys[k][0]) / (keys[k + 1][0] - keys[k][0]);
						engine.torqueCurve[i] = keys[k][1] + (keys[k + 1][1] - keys[k][1]) * std::clamp(f, 0.0f, 1.0f);
					}
					engine.torqueCurve[101] = engine.torqueCurve[100];
					engine.noPropellerEffectAtSpeed = number(t, "noPropellerEffectAtSpeed", engine.noPropellerEffectAtSpeed);
					engine.heatIncrement = number(t, "heatIncrement", engine.heatIncrement);
					engine.coolingFactor = number(t, "coolingFactor", engine.coolingFactor);
					engine.horizontalSpeedMagnifier = number(t, "horizontalSpeedMagnifier", engine.horizontalSpeedMagnifier);
					engine.maxAngleOfAttack = number(t, "maxAngleOfAttack", engine.maxAngleOfAttack);
					engine.defaultAngleOfAttack = number(t, "defaultAngleOfAttack", engine.defaultAngleOfAttack);
					engine.pureRotational = number(t, "pureRotational", 0) != 0;
					engine.attackSpeed = number(t, "attackSpeed", engine.attackSpeed);
					engine.decreaseAngleToZero = number(t, "decreaseAngleToZero", 0) != 0;
					engine.spinWhenNoEngineInAir = number(t, "spinWhenNoEngineInAir", engine.spinWhenNoEngineInAir);
					engine.horizontalDampAngle = number(t, "horizontalDampAngle", engine.horizontalDampAngle);
					engine.horizontalDampAngleFactor = number(t, "horizontalDampAngleFactor", engine.horizontalDampAngleFactor);
					engine.dampHorizontalVel = number(t, "dampHorizontalVel", engine.dampHorizontalVel);
					engine.dampHorizontalVelFactor = number(t, "dampHorizontalVelFactor", engine.dampHorizontalVelFactor);
					engine.decreaseAngleToZeroVerticalVel =
						number(t, "decreaseAngleToZeroVerticalVel", engine.decreaseAngleToZeroVerticalVel);
					engine.airFlowAffect = number(t, "airFlowAffect", engine.airFlowAffect);
					engine.noEffectAtPerpSpeed = number(t, "noEffectAtPerpSpeed", engine.noEffectAtPerpSpeed);
					engine.trackTurnAcceleration = number(t, "trackTurnAcceleration", engine.trackTurnAcceleration);
					engine.trackTurnSpeed = number(t, "trackTurnSpeed", engine.trackTurnSpeed);
					engine.noPowerWhenUpsideDown = number(t, "noPowerWhenUpsideDown", 0) != 0;
					if (self >= 0) {
						vehicle->m_parts[static_cast<size_t>(self)].canReverse = number(t, "canReverse", 1) != 0;
					}
					vehicle->m_engines.push_back(engine);
					place.oldEngine = static_cast<int>(vehicle->m_engines.size()) - 1;
					place.newCar2 = false;
				}
			} else if (type == "Wing") {
				WingPart wing;
				wing.part = self;
				wing.wingLift = number(t, "wingLift", 0);
				wing.flapLift = number(t, "flapLift", 0);
				wing.pitchOffset = number(t, "pitchOffset", 0);
				wing.positionOffset = vector(t, "positionOffset");
				wing.startDelay = number(t, "startDelay", 0);
				wing.multiply = number(t, "multiply", 1);
				wing.offsetY = vector(t, "offset").y;
				vehicle->m_wings.push_back(wing);
			} else if (type == "ForceObject") {
				ForcePart force;
				force.part = self;
				force.keepUp = number(t, "keepUp", 0) != 0;
				const char* names[3] = {"regulatePosX", "regulatePosY", "regulatePosZ"};
				for (size_t i = 0; i < 3; ++i) {
					const Vec3 v = vector(t, names[i]);
					force.a[i] = v.x;
					force.b[i] = v.y;
					force.regulated[i] = v.x != 0 || v.y != 0;
				}
				force.onlyWhenOccupied = number(t, "onlyWhenOccupied", 0) != 0;
				force.stopWhenUnoccupied = number(t, "stopRegulateWhenUnoccupied", 0) != 0;
				force.startDelay = number(t, "startDelay", 0);
				if (const std::string* input = setting(t, "input")) {
					force.input = vehicleInputIndex(*input);
				}
				force.force = number(t, "force", 0);
				force.constantForce = number(t, "constantForce", 0);
				force.maxForce = number(t, "maxForceOnObject", 0);
				vehicle->m_forces.push_back(force);
			} else if (type == "MechLegs") {
				vehicle->m_hasLegs = true;
				vehicle->m_legs.part = self;
				vehicle->m_legs.length = number(t, "length", vehicle->m_legs.length);
				vehicle->m_legs.stepHeight = number(t, "stepHeight", vehicle->m_legs.stepHeight);
				vehicle->m_legs.radius = number(t, "radius", vehicle->m_legs.radius);
			} else if (type == "RotationalBundle") {
				const std::string* input = setting(t, "inputToYaw");
				if (input && *input == "PIYaw") {
					steering.maxSteer = vector(t, "maxRotation").x;
					RotationAxis& a = steering.axis;
					a.minRotation = vector(t, "minRotation").x;
					a.maxRotation = steering.maxSteer;
					a.maxSpeed = vector(t, "maxSpeed").x;
					a.acceleration = vector(t, "acceleration").x;
					a.deAcceleration = vector(t, "deAcceleration").x;
					a.continuousSpeed = vector(t, "continousRotationSpeed").x;
					a.automaticReset = number(t, "automaticReset", 0) != 0;
					a.snapToZero = number(t, "snapToZeroOnNoInput", 0) != 0;
					a.useDeAcceleration = number(t, "useDeAcceleration", 0) != 0;
					a.capMaxSpeed = number(t, "capMaxSpeed", 0) != 0;
					a.maxSpeedMod = number(t, "maxSpeedMod", 0);
					steering.name = t.name();
				}
			} else if (type == "Spring") {
				Wheel wheel;
				wheel.mount = position;
				wheel.collisionPart = static_cast<int>(number(t, "collisionPart", -1));
				wheel.radius = wheelRadius(mesh, static_cast<int>(number(t, "collisionPart", -1)));
				wheel.spinRadius = wheelSpinRadius(mesh, static_cast<int>(number(t, "collisionPart", -1)));
				wheel.strength = number(t, "strength", 0);
				wheel.damping = number(t, "damping", 0);
				wheel.maxSteer = steering.maxSteer;
				wheel.steerAxis = steering.axis;
				wheel.spring = t.name();
				wheel.steering = steering.name;
				wheel.newCar2 = place.newCar2;
				wheel.engine = place.oldEngine;
				wheel.part = self;
				wheel.gripFlags = static_cast<int>(number(t, "grip", 2));
				vehicle->m_wheels.push_back(wheel);
				return;
			}
			for (const world::ChildTemplate& child : t.children()) {
				const world::ObjectTemplate* childTemplate = templates.find(child.name);
				if (!childTemplate) {
					continue;
				}
				const Mat3 local = Mat3::fromYawPitchRoll({child.rotation.x, child.rotation.y, child.rotation.z});
				const Vec3 childOffset {child.position.x, child.position.y, child.position.z};
				Place next = place;
				next.offset = place.offset + place.rotation.transform(childOffset);
				next.rotation = local * place.rotation;
				visit(*childTemplate, position + rotation.transform(childOffset), local * rotation, steering, next,
					depth + 1);
			}
		};
	visit(root, {}, {}, {}, {}, 0);
	const bool newCar2Wheels = std::any_of(
		vehicle->m_wheels.begin(), vehicle->m_wheels.end(), [](const Wheel& w) { return w.newCar2; });
	if (!(haveEngine && newCar2Wheels) && vehicle->m_engines.empty() && !vehicle->m_hasLegs) {
		return nullptr;
	}
	vehicle->m_mass = number(root, "mass", 1);
	vehicle->m_sprintFactor = number(root, "sprintFactor", 0);
	if (const std::string* input = setting(root, "regulateVerticalPosInput")) {
		vehicle->m_vertical.input = vehicleInputIndex(*input);
		const Vec3 ab = vector(root, "regulateVerticalPos");
		vehicle->m_vertical.a = ab.x;
		vehicle->m_vertical.b = ab.y;
	}
	for (Part& part : vehicle->m_parts) {
		part.rotation = part.base;
	}
	return vehicle;
}

// The engine's update (0x0064e920) for newCar2 (engine flag 0x80):
// - Braking (0x0064e810, dead zone 0.04): the throttle against the way
//   the engine turns (rpm beyond +-0.04), or no throttle while it is
//   nearly still.
// - The rpm (0x0064e6f0, then the newCar2 part): an rpm the throttle (times
//   the clutch) no longer holds decays, x0.9 while braking, then x0.96
//   without throttle (engine-inertia3) or x0.99 with it (engine-inertia2).
//   Nearly still (|rpm| < 0.05) it changes from first to reverse when the
//   throttle is under -0.5 and back over -0.05. The new rpm is the wheels'
//   mean between minRpm and maxRpm, blended by the clutch with the old:
//   clutch x wheels + (1 - clutch) x |old|, negative in reverse, within
//   -1 .. 1.2.
// - The clutch: it comes in at 2 / gearChangeTime a second, and goes out
//   as fast while a gear change is under way or while braking. Braking
//   with the clutch out (under 0.05) and the rpm under 0.1 puts it in
//   first; a change under way takes effect once the clutch is out.
// - Changes: up above gearUp with the clutch in (over 0.95), below the top
//   gear, but not while climbing (the nose up more than 0.174, sin 10
//   degrees, 0x00a78ffc) and moving forward; down when the rpm in the lower
//   gear would be under 0.55, from second up, with the clutch in.
void Vehicle::updateEngine(const physics::RigidBody& body, float seconds)
{
	constexpr float kDeadZone = 0.04f;  // 0x0064e810 for newCar2 (0.1 otherwise)
	constexpr float kInertiaOff = 0.96f; // engine-inertia3 (0x00a79000)
	constexpr float kInertiaOn = 0.99f;  // engine-inertia2 (0x00a79010)
	constexpr float kClimb = 0.174f;     // 0x00a78ffc
	const float wheelRpm = m_wheelRpmCount > 0 ? m_wheelRpmSum / static_cast<float>(m_wheelRpmCount) : 0.0f;
	m_wheelRpmSum = 0;
	m_wheelRpmCount = 0;
	const float throttle = m_input.throttle;
	const float clutched = throttle * m_clutch;
	// 0x0064e810
	const int towards = throttle < -kDeadZone ? -1 : throttle > kDeadZone ? 1 : 0;
	m_braking = m_rpm < -kDeadZone ? towards == 1 : (m_rpm > kDeadZone ? 1 : 0) + towards == 0;
	// 0x0064e6f0
	if ((clutched < m_rpm && m_rpm > 0) || (clutched > m_rpm && m_rpm < 0)) {
		if (m_braking) {
			m_rpm *= 0.9f;
		}
		m_rpm *= std::fabs(throttle) < 0.05f ? kInertiaOff : kInertiaOn;
	}
	if (std::fabs(m_rpm) < 0.05f) {
		if (m_gear == 1 && throttle < -0.5f) {
			m_shift = -1;
		} else if (m_gear == 0 && throttle > -0.05f) {
			m_shift = 1;
		}
	}
	const float wheels = std::max(0.0f, (std::fabs(wheelRpm) - m_engine.minRpm) / (m_engine.maxRpm - m_engine.minRpm));
	float rpm = m_clutch * wheels + (1 - m_clutch) * std::fabs(m_rpm);
	if (m_gear == 0) {
		rpm = -rpm;
	}
	m_rpm = std::clamp(rpm, -1.0f, 1.2f);
	// The clutch.
	const int shift = m_shift;
	if (m_clutch < 1 || m_shift != 0 || m_braking) {
		const float step = 2 * seconds / m_engine.gearChangeTime;
		if (m_shift == 0 && !m_braking) {
			m_clutch = std::min(1.0f, m_clutch + step);
		} else {
			m_clutch = std::max(0.0f, m_clutch - step);
		}
	}
	if (m_braking && m_clutch < 0.05f && m_rpm < 0.1f) {
		m_gear = 1;
	}
	if (shift != 0 && m_clutch <= 0.05f) {
		m_gear += shift;
		m_shift = 0;
	}
	// Changes.
	if (m_rpm > m_engine.gearUp && m_gear < m_engine.numberOfGears && m_clutch > 0.95f) {
		const Vec3& forward = body.rotation.rows[2];
		if (forward.y < kClimb || physics::dot(forward, body.velocity) <= 0) {
			m_shift = 1;
		}
	} else if (m_gear >= 2 && m_clutch > 0.95f
		&& m_rpm / m_engine.gearRatio(m_gear) * m_engine.gearRatio(m_gear - 1) < 0.55f) {
		m_shift = -1;
	}
}

void Vehicle::touchObject(size_t index, float height, const Vec3& normal, const Vec3& velocity, float grip)
{
	if (index >= m_wheels.size()) {
		return;
	}
	Wheel::ObjectGround& g = m_wheels[index].objectGround;
	const float n = static_cast<float>(g.count);
	const float mean = 1.0f / static_cast<float>(g.count + 1);
	g.height = g.count == 0 ? height : std::max(g.height, height);
	g.normal = (g.normal * n + normal) * mean;
	g.velocity = (g.velocity * n + velocity) * mean;
	g.grip = grip;
	++g.count;
}

void Vehicle::tick(physics::RigidBody& body, const physics::Ground& ground, float seconds, float grip, float gravity)
{
	++m_tick;
	regulateInput(body, gravity);
	updateParts(seconds);
	if (m_newCar2) {
		updateEngine(body, seconds);
	}
	for (OldEngine& engine : m_engines) {
		updateOldEngine(engine, body, ground, seconds, gravity);
	}
	if (m_hasLegs) {
		updateLegs(body, ground, seconds);
	}
	for (WingPart& wing : m_wings) {
		updateWing(wing, body, ground, seconds, gravity);
	}
	for (ForcePart& force : m_forces) {
		updateForce(force, body, seconds);
	}
	for (Wheel& wheel : m_wheels) {
		if (wheel.newCar2) {
			updateWheel(wheel, body, ground, seconds, grip, gravity);
		} else {
			updateOldWheel(wheel, body, ground, seconds, grip, gravity);
		}
	}
}

void rotationAxisStep(const RotationAxis& axis, float input, float seconds, float& angle, float& speed)
{
	constexpr float kMaxSpeedMod = 30; // rotationalbundle-max-speed-mod (0x00a67490)
	if (axis.acceleration == 0 && axis.continuousSpeed == 0) {
		return;
	}
	if (axis.snapToZero && ((input < 0.05f && angle > 0) || (input > -0.05f && angle < 0))) {
		angle = 0;
		speed = 0;
	}
	const float scale = axis.automaticReset ? axis.maxRotation : axis.maxSpeed;
	float target = axis.acceleration > 0 ? input * scale : axis.acceleration < 0 ? -(input * scale) : 0.0f;
	if (axis.capMaxSpeed) {
		target = std::clamp(target, -axis.maxSpeed * kMaxSpeedMod, axis.maxSpeed * kMaxSpeedMod);
	}
	if (axis.maxSpeedMod != 0) {
		const float cap = std::fabs(axis.maxSpeed * axis.maxSpeedMod);
		target = std::clamp(target, -cap, cap);
	}
	const float up = std::fabs(axis.acceleration) * seconds;
	const float down = std::fabs(axis.useDeAcceleration ? axis.deAcceleration : axis.acceleration) * seconds;
	float& moving = axis.automaticReset ? angle : speed;
	if (target > moving) {
		moving = std::min(target, moving + up);
	} else if (target < moving) {
		moving = std::max(target, moving - down);
	}
	if (!axis.automaticReset) {
		angle += seconds * speed + seconds * axis.continuousSpeed;
	}
	if (axis.maxRotation != 0 || axis.minRotation != 0) {
		if (angle > axis.maxRotation) {
			angle = axis.maxRotation;
		} else if (angle < axis.minRotation) {
			angle = axis.minRotation;
		}
	} else if (angle > 360) {
		angle -= 360;
	} else if (angle < -360) {
		angle += 360;
	}
}

void Vehicle::updateWheel(Wheel& wheel, physics::RigidBody& body, const physics::Ground& ground, float seconds,
	float grip, float gravity)
{
	// Steering: the RotationalBundle above the wheel (0x00565a20 runs the
	// axis while its maxSpeed is not 0).
	if (wheel.maxSteer != 0 && wheel.steerAxis.maxSpeed != 0) {
		rotationAxisStep(wheel.steerAxis, m_input.steer, seconds, wheel.steer, wheel.steerRate);
	}
	const Mat3 wheelRotation = Mat3::fromYawPitchRoll({wheel.steer, 0, 0}) * body.rotation;
	const Vec3 right = wheelRotation.rows[0];
	const Vec3 up = body.rotation.rows[1];
	const Vec3 forward = wheelRotation.rows[2];
	const Vec3 centre = body.position + body.rotation.transform(wheel.mount);
	const Vec3 bottom = centre - up * wheel.radius;
	Vec3 normal {0, 1, 0};
	float groundHeight = ground.heightAndNormal(bottom.x, bottom.z, normal);
	// An object under the wheel (a bridge, a deck, another vehicle) is its
	// ground where it is higher, moving as that object does.
	Vec3 surfaceVelocity;
	if (wheel.objectGround.count > 0 && wheel.objectGround.height > groundHeight) {
		groundHeight = wheel.objectGround.height;
		const float l = std::sqrt(physics::lengthSquared(wheel.objectGround.normal));
		normal = l > 1e-6f ? wheel.objectGround.normal * (1.0f / l) : Vec3 {0, 1, 0};
		surfaceVelocity = wheel.objectGround.velocity;
		grip = wheel.objectGround.grip;
	}
	wheel.objectGround = {};
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

	// The driven wheel (0x006b7910) runs whether it touches or not
	// (0x006b8230 hands newCar2 wheels to it before its contact test). Off
	// the ground its contact sums are zero: no normal, the ground velocity
	// gravity's tick alone, so no tyre force and no feedback, while the
	// engine and gravity along the wheel still turn it, and its rpm still
	// counts towards the engine's. (The Spring node then scales the spin by
	// 1.0 for newCar2, 0x00a84c20; 0.9 in water.)
	{
		const Vec3 point {bottom.x, wheel.contact ? groundHeight : bottom.y, bottom.z};
		Vec3 groundVelocity {0, gravity * kTick, 0};
		Vec3 contactNormal {};
		if (wheel.contact) {
			Vec3 velocity = body.pointVelocity(point) - surfaceVelocity;
			velocity.y += gravity * kTick;
			groundVelocity = velocity - normal * physics::dot(velocity, normal);
			contactNormal = normal;
		}
		TyreContact contact;
		contact.grip = grip;
		contact.normalY = contactNormal.y;
		const Vec3 engine = body.position + body.rotation.transform(m_engineOffset);
		contact.drive = physics::dot(point - engine, body.rotation.rows[2]) < 0;
		contact.brakeLatMod = m_braking && std::fabs(m_input.throttle) > 0.01f
			&& std::fabs(physics::dot(body.velocity, right)) < kBrakeLatSpeed;

		// Driven wheel (0x006b7910): spin from the engine and the slope.
		const Vec3 driveDirection = normalized(forward - contactNormal * physics::dot(forward, contactNormal));
		const float quarterMass = m_mass * 0.25f;
		const float ratio = m_engine.gearRatio(m_gear);
		float throttle = m_input.throttle;
		if ((m_gear < 1 && throttle > 0) || (m_gear >= 1 && throttle <= 0)) {
			throttle = 0; // slot 0x1a4: only towards the gear's direction
		}
		// Slot 0x1a4: the torque curve (flat, 0x374) times the throttle, times
		// the clutch with useClutchedTorque.
		const float engineTorque = m_engine.torque * throttle * (m_engine.useClutchedTorque ? m_clutch : 1.0f);
		const float driveTorque = engineTorque * m_engine.differential * ratio * kDriveTorqueScale;
		// 0x006b7460: the downhill part of the wheel's share of the weight,
		// along the wheel, times the radius (all of it along the wheel off
		// the ground).
		const float weight = gravity * quarterMass;
		const float intoGround = weight * contactNormal.y;
		const float downhill = (-intoGround * contactNormal.x) * forward.x + (weight - intoGround * contactNormal.y) * forward.y
			+ (-intoGround * contactNormal.z) * forward.z;
		const float slopeTorque = downhill * wheel.spinRadius * m_engine.slopeTorqueMod
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
		const Vec3 slip = driveDirection * (spun * wheel.spinRadius) - groundVelocity;
		const Vec3 force = wheel.contact ? tyreForce(m_engine, contact, slip, forward, right) * m_engine.frictionMod : Vec3 {};

		// The tyre's grip turns the wheel back.
		const float feedback = wheel.spinRadius * physics::dot(force, forward) * m_engine.engineFeedbackMod * quarterMass
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
		if (wheel.contact) {
			body.addFriction(point, force * kTicksPerSecond);
		}
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
