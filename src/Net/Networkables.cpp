#include "Networkables.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace dice::hfe::net {

namespace {

// Smallest n with 2^n - 1 >= count (the engine's loops and 0x00508310).
unsigned bitsFor(uint32_t count)
{
	unsigned n = 0;
	while (n < 32 && ((1ull << n) - 1) < count) {
		++n;
	}
	return n;
}

// 0x0050d450: v / (2^bits - 1).
float readUnit(BitReader& in, unsigned bits)
{
	if (bits >= 32) {
		return in.readFloat();
	}
	const uint32_t v = in.read(bits);
	return static_cast<float>(v) / static_cast<float>((1u << bits) - 1);
}

// 0x0050d4b0: clamp to 0..1, times 2^bits - 1, truncated.
void writeUnit(BitWriter& out, float value, unsigned bits)
{
	if (bits >= 32) {
		out.writeFloat(value);
		return;
	}
	const float max = static_cast<float>((1u << bits) - 1);
	out.write(static_cast<uint32_t>(std::clamp(value, 0.0f, 1.0f) * max), bits);
}

// The readers' "(v * 2 / max) - 1, zeroed under one step" pattern
// (e.g. the soldier's angles at 0x005fcdb0), times `range`.
float readSnorm(BitReader& in, unsigned bits, float range)
{
	const float max = static_cast<float>((1u << bits) - 1);
	float f = static_cast<float>(in.read(bits)) * (2.0f / max) - 1.0f;
	if (std::fabs(f) < 2.0f / max) {
		f = 0;
	}
	return f * range;
}

// The writers' ROUND(clamp((v / range + 1) / 2) * max).
void writeSnorm(BitWriter& out, float value, unsigned bits, float range)
{
	const float max = static_cast<float>((1u << bits) - 1);
	const float u = std::clamp((value / range + 1.0f) * 0.5f, 0.0f, 1.0f);
	out.write(static_cast<uint32_t>(std::lround(u * max)), bits);
}

// 0x00593c70: like readSnorm, but zeroed up to and including one step.
float readSnormLe(BitReader& in, unsigned bits, float range)
{
	const float max = static_cast<float>((1u << bits) - 1);
	float f = static_cast<float>(in.read(bits)) * (2.0f / max) - 1.0f;
	if (std::fabs(f) <= 2.0f / max) {
		f = 0;
	}
	return f * range;
}

// 0x0050d4b0 given (v / range + 1) / 2: truncated (the FPU chops).
void writeSnormTrunc(BitWriter& out, float value, unsigned bits, float range)
{
	const float u = range == 0 ? 0.5f : (value / range + 1.0f) * 0.5f;
	writeUnit(out, u, bits);
}

// A value in min..max as 0x00508370 reads it (bits for the count).
uint32_t readRanged(BitReader& in, uint32_t count) { return in.read(bitsFor(count)); }
void writeRanged(BitWriter& out, uint32_t value, uint32_t count) { out.write(value, bitsFor(count)); }

float length(const Vec3f& a, const Vec3f& b)
{
	const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

const Vec3f kZero {};

// Hit points as 0x006044e0 / 0x005ff5d0 send them: hp + wreck in
// 0..max + wreck (armor vtable 0x8c / 0x90).
bool readHitPoints(BitReader& in, const NetworkableHints& h, float& out)
{
	if (!h.hasArmor) {
		return false;
	}
	const uint32_t count = static_cast<uint32_t>(h.maxHitPoints + h.wreckHitPoints + 1);
	out = static_cast<float>(static_cast<int>(readRanged(in, count)) - h.wreckHitPoints);
	return true;
}

void writeHitPoints(BitWriter& out, const NetworkableHints& h, float hp)
{
	const int count = h.maxHitPoints + h.wreckHitPoints + 1;
	const int v = std::clamp(static_cast<int>(hp) + h.wreckHitPoints, 0, count - 1);
	writeRanged(out, static_cast<uint32_t>(v), static_cast<uint32_t>(count));
}

} // namespace

// ---------------------------------------------------------------------------
// Quantizations

Vec3f readDelta(BitReader& in, DeltaTable table, float scale, const Vec3f& reference)
{
	static const unsigned kWide[4] = {32, 20, 16, 12};  // 0x00a05edc
	static const unsigned kNarrow[4] = {32, 12, 10, 8}; // 0x00a05eec
	const uint32_t mode = in.read(2);
	if (mode == 0) {
		Vec3f v;
		v.x = in.readFloat();
		v.y = in.readFloat();
		v.z = in.readFloat();
		return v;
	}
	const unsigned bits = (table == DeltaTable::Wide ? kWide : kNarrow)[mode];
	const float x = static_cast<float>(in.readSigned(bits));
	const float y = static_cast<float>(in.readSigned(bits));
	const float z = static_cast<float>(in.readSigned(bits));
	return {x * scale + reference.x, y * scale + reference.y, z * scale + reference.z};
}

void writeDelta(BitWriter& out, DeltaTable table, float scale, const Vec3f& value, const Vec3f* reference)
{
	// 0x00687510 / 0x00687770: the mode by the distance in steps (thresholds
	// 2^11, 2^15, 2^19 or 2^7, 2^9, 2^11); 0x0050d5b0 sends floats when the
	// stream has no reference.
	static const unsigned kWide[4] = {32, 20, 16, 12};
	static const unsigned kNarrow[4] = {32, 12, 10, 8};
	const unsigned* bits = table == DeltaTable::Wide ? kWide : kNarrow;
	uint32_t mode = 0;
	if (reference) {
		const float steps = length(value, *reference) / scale;
		for (uint32_t m = 3; m >= 1; --m) {
			if (steps < static_cast<float>(1u << (bits[m] - 1))) {
				mode = m;
				break;
			}
		}
	}
	out.write(mode, 2);
	if (mode == 0) {
		out.writeFloat(value.x);
		out.writeFloat(value.y);
		out.writeFloat(value.z);
		return;
	}
	out.writeSigned(static_cast<int32_t>(std::lround((value.x - reference->x) / scale)), bits[mode]);
	out.writeSigned(static_cast<int32_t>(std::lround((value.y - reference->y) / scale)), bits[mode]);
	out.writeSigned(static_cast<int32_t>(std::lround((value.z - reference->z) / scale)), bits[mode]);
}

Vec3f readDelta3(BitReader& in, float scale, const Vec3f& reference)
{
	static const unsigned kBits[7] = {32, 24, 20, 16, 12, 10, 8}; // 0x00a05efc + mode * 4
	const uint32_t mode = in.read(3);
	if (mode == 0) {
		Vec3f v;
		v.x = in.readFloat();
		v.y = in.readFloat();
		v.z = in.readFloat();
		return v;
	}
	if (mode == 7) {
		return reference;
	}
	const float x = static_cast<float>(in.readSigned(kBits[mode]));
	const float y = static_cast<float>(in.readSigned(kBits[mode]));
	const float z = static_cast<float>(in.readSigned(kBits[mode]));
	return {x * scale + reference.x, y * scale + reference.y, z * scale + reference.z};
}

void writeDelta3(BitWriter& out, float scale, const Vec3f& value, const Vec3f& reference)
{
	static const unsigned kBits[7] = {32, 24, 20, 16, 12, 10, 8};
	const float distance = length(value, reference);
	if (distance < 1.1920929e-07f) {
		out.write(7, 3);
		return;
	}
	const float steps = distance / scale;
	uint32_t mode = 0;
	for (uint32_t m = 6; m >= 1; --m) {
		if (steps < static_cast<float>(1u << (kBits[m] - 1))) {
			mode = m;
			break;
		}
	}
	out.write(mode, 3);
	if (mode == 0) {
		out.writeFloat(value.x);
		out.writeFloat(value.y);
		out.writeFloat(value.z);
		return;
	}
	out.writeSigned(static_cast<int32_t>(std::lround((value.x - reference.x) / scale)), kBits[mode]);
	out.writeSigned(static_cast<int32_t>(std::lround((value.y - reference.y) / scale)), kBits[mode]);
	out.writeSigned(static_cast<int32_t>(std::lround((value.z - reference.z) / scale)), kBits[mode]);
}

Quat readQuaternion(BitReader& in, unsigned bits)
{
	auto component = [&] { return bits >= 32 ? in.readFloat() : readUnit(in, bits) * 2.0f - 1.0f; };
	Quat q;
	q.w = component();
	q.x = component();
	q.y = component();
	q.z = component();
	// 0x005fdba0
	const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	if (n < 1.1920929e-07f) {
		return Quat {};
	}
	q.x /= n;
	q.y /= n;
	q.z /= n;
	q.w /= n;
	return q;
}

void writeQuaternion(BitWriter& out, const Quat& q, unsigned bits)
{
	auto component = [&](float v) {
		if (bits >= 32) {
			out.writeFloat(v);
		} else {
			writeUnit(out, (v + 1.0f) * 0.5f, bits);
		}
	};
	component(q.w);
	component(q.x);
	component(q.y);
	component(q.z);
}

// ---------------------------------------------------------------------------
// Kinds

const char* networkableKindName(NetworkableKind kind)
{
	switch (kind) {
	case NetworkableKind::SimpleObject: return "SimpleObject";
	case NetworkableKind::RotationalBundle: return "RotationalBundle";
	case NetworkableKind::FreeCamera: return "FreeCamera";
	case NetworkableKind::OverheadCamera: return "OverheadCamera";
	case NetworkableKind::Item: return "Item";
	case NetworkableKind::DestroyableObject: return "DestroyableObject";
	case NetworkableKind::DestroyableWindowsBundle: return "DestroyableWindowsBundle";
	case NetworkableKind::Spring: return "Spring";
	case NetworkableKind::PlayerControlObject: return "PlayerControlObject";
	case NetworkableKind::Kit: return "Kit";
	case NetworkableKind::GenericProjectile: return "GenericProjectile";
	case NetworkableKind::ControlPoint: return "ControlPoint";
	case NetworkableKind::Engine: return "Engine";
	case NetworkableKind::TargetObject: return "TargetObject";
	case NetworkableKind::MechLegs: return "MechLegs";
	case NetworkableKind::Soldier: return "Soldier";
	case NetworkableKind::Player: return "Player";
	default: return "Unknown";
	}
}

NetworkableKind networkableKindForTemplateType(const std::string& templateType, bool hasParent, bool hasMobilePhysics)
{
	// Each object class's slot-94 function makes its networkable with the
	// class id it passes to 0x006fa2a0 (an index into 0x00918d88), traced
	// from the template types' object constructors.
	std::string t = templateType;
	std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (t == "soldier") {
		return NetworkableKind::Soldier; // 0x005882f0 (Soldier.cpp)
	}
	if (t == "playercontrolobject" || t == "parachute") {
		// 0x005c77f0: a root (no parent, +0x34) gets the default
		// SimpleObject networkable (0x0063c3f0), a seat a PlayerControlObject one.
		return hasParent ? NetworkableKind::PlayerControlObject : NetworkableKind::SimpleObject;
	}
	if (t == "genericprojectile") {
		return NetworkableKind::GenericProjectile; // 0x005b5670
	}
	if (t == "kit") {
		return NetworkableKind::Kit; // 0x0056d400
	}
	if (t == "controlpoint") {
		return NetworkableKind::ControlPoint; // 0x005bb950
	}
	if (t == "destroyableobject") {
		// 0x005bf200: the template's interface 0xc368 (template +0x68)
		// slot 0x34 (0x005049e8) is hasMobilePhysics; set: the SimpleObject
		// networkable (0x9858), else the DestroyableObject one (0x9876).
		return hasMobilePhysics ? NetworkableKind::SimpleObject : NetworkableKind::DestroyableObject;
	}
	if (t == "destroyablewindowsbundle") {
		return NetworkableKind::DestroyableWindowsBundle; // 0x005be670
	}
	if (t == "rotationalbundle" || t == "camera" || t == "floatingbundle" || t == "wing" || t == "landinggear") {
		return NetworkableKind::RotationalBundle; // 0x005656c0
	}
	if (t == "spring") {
		return NetworkableKind::Spring; // 0x0064f840
	}
	if (t == "engine") {
		return NetworkableKind::Engine; // 0x0064cc80
	}
	if (t == "mechlegs") {
		return NetworkableKind::MechLegs; // 0x00650ae0
	}
	if (t == "targetobject") {
		return NetworkableKind::TargetObject; // 0x0058e890
	}
	if (t == "freecamera") {
		return NetworkableKind::FreeCamera; // 0x00615930
	}
	if (t == "overheadcamera") {
		return NetworkableKind::OverheadCamera; // 0x005f87a0
	}
	if (t == "item") {
		return NetworkableKind::Item; // 0x005bdd10
	}
	// Slot 94 0x0058cc40 makes none, whatever the networkableInfo says.
	static const char* const kNone[] = {"genericfirearm", "ladder", "laddercontainer", "spawnpoint", "particle",
		"spriteparticle", "forceobject", "lightsource", "sound", "envmap"};
	for (const char* none : kNone) {
		if (t == none) {
			return NetworkableKind::Unknown;
		}
	}
	// The default (0x0063c3f0, or SupplyObject's 0x005d0af0; both only with
	// a networkableInfo): every other type that makes an object, traced
	// from the template's create slot to the object's slot 94: SimpleObject,
	// Bundle, DynamicBundle, AnimatedBundle, ItemContainer, KitPart,
	// Obstacle, UAVVehicle, SupplyObject, SupplyDepot, AreaObject,
	// AmbientEffectArea, AntennaObject, DestroyableWindow, Emitter,
	// EntryPoint, LookAtObject, ObjectSpawner, (Turnable)RemoteControlledObject,
	// Rotor, Trigger, GroundEffectBundle, and the effect bundles (Effect-,
	// AirDraft-, WheelEffectBundle), which a dedicated server doesn't make
	// (0x0042b800). Particle systems, decals and trails make no object.
	return NetworkableKind::SimpleObject;
}

// ---------------------------------------------------------------------------
// Readers (mode 2)

namespace {

// 0x005ffec0. Fields of components the object lacks are not sent (the
// writer, 0x005ff5d0, skips them), so the hints say which are there.
bool readSimpleObject(BitReader& in, const NetworkableHints& h, const Vec3f& reference, NetworkableState& s, unsigned mode,
	const MotionHistory* history)
{
	s.mask = in.read(25);
	s.flag = in.read(1) == 1;
	const uint32_t m = s.mask;
	// 0x005ffec0: against a history slot's copy (a relative control record,
	// mode 3) the position, velocity and angular velocity are deltas from
	// it; otherwise from the stream's reference and zero. In mode 3 the
	// angular velocity is a wide delta in 1 mm steps, in mode 2 a delta3 in
	// centimetres.
	if (m & 2) {
		s.hasPosition = true;
		s.position = readDelta(in, DeltaTable::Wide, 0.0005f, history ? history->position : reference);
	}
	// A physics node that moves (vtable 0x164), unless the flag is set.
	const bool physics = h.mobile && !s.flag;
	if (physics) {
		if (m & 4) {
			s.hasVelocity = true;
			s.velocity = readDelta(in, DeltaTable::Wide, 0.001f, history ? history->velocity : kZero);
		}
		if (m & 8) {
			s.hasAngularVelocity = true;
			s.angularVelocity = history ? readDelta(in, DeltaTable::Wide, 0.001f, history->angularVelocity)
				: mode == 3        ? readDelta(in, DeltaTable::Wide, 0.001f, kZero)
								   : readDelta3(in, 0.01f, kZero);
		}
	}
	if (m & 1) {
		s.hasOrientation = true;
		s.orientation = readQuaternion(in, 14);
	}
	if (physics) {
		if (m & 0x10000) {
			readDelta(in, DeltaTable::Wide, 0.001f, kZero); // node +0x38
			readDelta(in, DeltaTable::Wide, 0.001f, kZero); // node +0xd4
		}
		for (uint32_t bit : {0x80u, 0x100u, 0x400u, 0x800u}) {
			if (m & bit) {
				readDelta(in, DeltaTable::Wide, 0.0001f, kZero); // node +0x2c, +0xc8, +0xec, +0xf8
			}
		}
		if (m & 0x200) {
			s.sleeping = in.read(1) == 0; // 1 wakes the node (vtable 0x154), 0 puts it to sleep (0x160)
		}
		if (m & 0x20000) {
			s.supplyFlag = in.read(1) == 1; // SupplyObject +0x198
		}
	}
	if (h.hasArmor) { // object +0x48
		if (m & 0x10) {
			s.hasHealth = readHitPoints(in, h, s.health);
		}
		if (m & 0x20) {
			s.empLevel = static_cast<int>(in.read(4)); // armor +0x27c
		}
	}
	if (h.hasTeamChange && (m & 0x40)) { // object +0x5c
		s.changePoints = static_cast<int>(in.read(8));
	}
	if (m & 0x1000) {
		s.spawnTeam = static_cast<int>(in.read(2)); // object +0x168 (base object vtable 0x1a8)
	}
	// 0x00600560: with a PlayerControlObject on the parent chain.
	if (h.inVehicle) {
		if (h.parachute) {
			if (m & 0x8000) {
				s.parachuteState = static_cast<int>(readRanged(in, 8));
			}
		} else {
			const uint32_t slots[3] = {0x2000, 0x4000, 0x80000};
			for (int g = 0; g < 3; ++g) {
				if (m & slots[g]) {
					s.seatWeapon[g] = static_cast<int>(readRanged(in, 9)) - 1;
				}
			}
			if (m & 0x40000) {
				s.vehicleValue = static_cast<int>(readRanged(in, 16)); // PCO +0x190
			}
			if (m & 0x100000) {
				s.ducking = in.read(1) == 1; // PCO +0x18c (vtable 0x12c)
			}
			if (m & 0x200000) {
				s.vehicleFlag = in.read(1) == 1; // PCO +0x2be
			}
		}
	}
	if (m & 0x800000) {
		// The armored objects below it (object vtable 0x14c): eighths of
		// their hit points (armor.fullChildSync) or whether they are intact.
		if (!h.armoredChildrenKnown) {
			return !in.bad();
		}
		for (bool full : h.armoredChildren) {
			s.childDamage.push_back(full ? static_cast<int>(readRanged(in, 9)) : (in.read(1) == 1 ? 8 : 0));
		}
	}
	if (m & 0x1000000) {
		s.responseFlag = in.read(1) == 1; // ResponsePhysics (0x006b6c30)
	}
	s.complete = !in.bad();
	return s.complete;
}

// 0x005fcdb0. `mode` is the reader's last argument: 2 in ghost updates
// (0x0057fce0), 3 in the control-object record (0x00580cd0). Mode 3 sends
// the soldier's physics node and control state at full precision for the
// client's prediction: the velocity in 1 mm steps and three more node
// vectors, the angles as raw floats, the leans in 16 bits, the stamina in
// 7 bits and the sprinting bit, and mask 0x40000.
bool readSoldier(BitReader& in, const NetworkableHints& h, const Vec3f& reference, NetworkableState& s, unsigned mode = 2)
{
	const bool full = mode == 3;
	s.mask = in.read(21);
	const uint32_t m = s.mask;
	if (m & 0x40) {
		s.hasHealth = true;
		s.health = static_cast<float>(in.read(8)) / 255.0f; // x maxHitPoints (vtable 0x8c)
		s.soldierBits |= in.read(1) == 1 ? 1u : 0u;         // armor +0x21c
	}
	if (m & 0x80) {
		s.empLevel = static_cast<int>(in.read(4));
	}
	if (m & 0x20) {
		s.soldierValues[0] = static_cast<int>(readRanged(in, 5)); // +0x268
		s.soldierValues[1] = static_cast<int>(readRanged(in, 5)); // +0x26c
	}
	if (m & 0x10000) {
		// Dead: the ragdoll (0x008a9ce0 writes it): a 9-bit length, then the
		// networked particles (0x008acce0 reads them by its own flags, the
		// same on both ends): the first's position as floats, the others as
		// delta3 of 0.01 from it. Nothing else follows.
		s.ragdollBits = in.read(9);
		const size_t start = in.position();
		if (s.ragdollBits >= 96) {
			Vec3f first;
			first.x = in.readFloat();
			first.y = in.readFloat();
			first.z = in.readFloat();
			s.ragdollParticles.push_back(first);
			while (in.position() + 3 <= start + s.ragdollBits && !in.bad()) {
				s.ragdollParticles.push_back(readDelta3(in, 0.01f, first));
			}
			s.hasPosition = true;
			s.position = s.ragdollParticles[s.ragdollParticles.size() > 1 ? 1 : 0];
		}
		in.seek(start + s.ragdollBits);
		s.complete = !in.bad();
		return s.complete;
	}
	if (m & 1) {
		s.hasPosition = true;
		s.position = readDelta(in, DeltaTable::Wide, 0.001f, reference);
	}
	if (full) {
		// 0x005fb670 per field: the node's velocity (slot 0x58, set by
		// 0x5c) in 1 mm/s, its slots 0x74 / 0x8c (set 0x78 / 0x8c) in 0.1 mm
		// and 0xc4 (set 0xc8) in 1 mm.
		if (m & 0x100) {
			s.hasVelocity = true;
			s.velocity = readDelta(in, DeltaTable::Wide, 0.001f, kZero);
		}
		if (m & 0x200) {
			s.soldierAcceleration = readDelta(in, DeltaTable::Wide, 0.0001f, kZero);
		}
		if (m & 0x400) {
			s.soldierNode88 = readDelta(in, DeltaTable::Wide, 0.0001f, kZero);
		}
		if (m & 0x80000) {
			s.soldierImpulse = readDelta(in, DeltaTable::Wide, 0.001f, kZero);
		}
	} else if (m & 0x100) {
		s.hasVelocity = true;
		s.velocity = readDelta(in, DeltaTable::Wide, 0.01f, kZero);
	}
	if (m & 0x1e) {
		s.hasAngles = true;
	}
	if (full) {
		// The four angles as raw floats (+0x25c, +0x278, +0x280, +0x270).
		if (m & 2) {
			s.yaw = in.readFloat();
		}
		if (m & 4) {
			s.pitch = in.readFloat();
		}
		if (m & 8) {
			s.roll = in.readFloat();
		}
		if (m & 0x10) {
			s.aimPitch = in.readFloat();
		}
	} else {
		if (m & 2) {
			s.yaw = readSnorm(in, 12, 360.0f);
		}
		if (m & 4) {
			s.pitch = readSnorm(in, 12, 90.0f);
		}
		if (m & 8) {
			s.roll = readSnorm(in, 12, 180.0f);
		}
		if (m & 0x10) {
			s.aimPitch = readSnorm(in, 12, 90.0f);
		}
	}
	s.weaponZoom = static_cast<int>(readRanged(in, 4));      // the weapon's DefaultZoomComp +0xc
	s.weaponFireState = static_cast<int>(readRanged(in, 2)); // the fire comp mode (ExplosivesFireComp 0x005e6350)
	for (int i = 0; i < 3; ++i) {
		s.soldierBits |= (in.read(1) == 1 ? 2u : 0u) << i;   // +0x328, +0x30c, +0x3e9
	}
	const unsigned leanBits = full ? 16 : 10;
	if (m & 0x800) {
		s.soldierLean[0] = readSnorm(in, leanBits, 50.0f); // +0x264
	}
	if (m & 0x1000) {
		s.soldierLean[1] = readSnorm(in, leanBits, 50.0f); // +0x260
	}
	s.responseFlag = in.read(1) == 1;
	if (full) {
		if (m & 0x8000) {
			s.stamina = static_cast<float>(in.read(7)) / 127.0f; // PCO vtable 0x108 / 0x10c
			s.sprinting = in.read(1) == 1;                      // 0x005fb1f0: PCO vtable 0x100 / 0x104
		}
	} else if (m & 0x8000) {
		s.stamina = static_cast<float>(in.read(5)) / 31.0f; // sprint stamina (PCO vtable 0x10c)
	}
	if (m & 0x2000) {
		if (h.soldierItemSlots < 0) {
			return !in.bad();
		}
		s.weapon = static_cast<int>(readRanged(in, static_cast<uint32_t>(h.soldierItemSlots + 2))) - 1;
	}
	if (m & 0x4000) {
		s.soldierValues[2] = static_cast<int>(in.read(2)); // +0x284
	}
	if (full && (m & 0x40000)) {
		s.soldierValue40000 = readSnorm(in, 7, 1.0f); // +0x18c's vtable 0x90 (set by 0x94)
	}
	if (m & 0x20000) {
		s.ladder = static_cast<int>(readRanged(in, 256));
		if (s.ladder != 0) {
			s.ladderPosition = readUnit(in, 12);
		}
	}
	if (m & 0x100000) {
		s.vehicleValue = static_cast<int>(readRanged(in, 16)); // PCO +0x190
	}
	s.complete = !in.bad();
	return s.complete;
}

// 0x00606030 (Kit): 1-bit mask, position.
bool readKit(BitReader& in, const Vec3f& reference, NetworkableState& s)
{
	s.mask = in.read(1);
	if (s.mask & 1) {
		s.hasPosition = true;
		s.position = readDelta(in, DeltaTable::Wide, 0.0005f, reference);
	}
	s.complete = !in.bad();
	return s.complete;
}

// 0x00606f20 (GenericProjectile)
bool readProjectile(BitReader& in, const NetworkableHints& h, const Vec3f& reference, NetworkableState& s)
{
	s.mask = in.read(6);
	if (s.mask & 2) {
		s.hasPosition = true;
		s.position = readDelta(in, DeltaTable::Wide, 0.01f, reference);
	}
	if (s.mask & 1) {
		s.hasOrientation = true;
		s.orientation = readQuaternion(in, 12);
	}
	if (s.mask & 4) {
		if (in.read(1) == 1) {
			in.readSigned(16);
			s.hasVelocity = true;
			s.velocity = readDelta(in, DeltaTable::Narrow, 0.01f, kZero); // 0x006ac600: a zeroed static vector (0x00a82458)
			if (in.read(1) == 1) {
				in.read(16);
			}
		}
	}
	if (s.mask & 0x10) {
		if (!h.hasArmor) {
			return !in.bad();
		}
		s.hasHealth = readHitPoints(in, h, s.health);
	}
	if (s.mask & 0x20) {
		in.read(1);
	}
	s.complete = !in.bad();
	return s.complete;
}

// 0x006084b0 (ControlPoint)
bool readControlPoint(BitReader& in, NetworkableState& s)
{
	s.mask = in.read(6);
	if (s.mask & 1) {
		s.team = in.readSigned(4); // the team component (+0x184 slot 0x18)
	}
	if (s.mask & 2) {
		s.cpNeutralStart = in.read(1) == 1;
	}
	if (s.mask & 4) {
		s.cpValues[0] = readSnorm(in, 16, 1.25f);
		s.cpValues[1] = readSnorm(in, 16, 1.25f);
	}
	if (s.mask & 8) {
		s.cpAllowTeam1 = in.readSigned(4) != 0;
	}
	if (s.mask & 0x10) {
		s.cpAllowTeam2 = in.readSigned(4) != 0;
	}
	if (s.mask & 0x20) {
		s.cpValue = in.readSigned(4);
	}
	s.complete = !in.bad();
	return s.complete;
}

// 0x006048c0 (DestroyableObject)
bool readDestroyable(BitReader& in, const NetworkableHints& h, const Vec3f& reference, NetworkableState& s)
{
	s.mask = in.read(3);
	if (h.hasArmor) {
		if (s.mask & 1) {
			s.hasHealth = readHitPoints(in, h, s.health);
		}
		if (s.mask & 2) {
			s.hasPosition = true;
			s.position = readDelta(in, DeltaTable::Wide, 0.01f, reference);
			s.destroyValue = static_cast<float>(in.read(10)) * (100.0f / 1023.0f);
		}
	}
	if (s.mask & 4) {
		// A bit per armored object below it (the list SimpleObject's mask
		// 0x800000 walks): 1 intact, 0 wrecked.
		if (!h.armoredChildrenKnown) {
			return !in.bad();
		}
		for (size_t i = 0; i < h.armoredChildren.size(); ++i) {
			s.childDamage.push_back(in.read(1) == 1 ? 8 : 0);
		}
	}
	s.complete = !in.bad();
	return s.complete;
}

// 0x00602070 (RotationalBundle; also Camera, Wing, FloatingBundle and
// LandingGear objects). Mode 2 uses 12 bits per angle and speed, mode 3
// 16 (the writer's `(mode == 3) * 4 + 12`). The
// writer (0x006015a0) only sends the fields its template uses (inputs
// mapped, no automatic reset...), so the mask is enough to read them.
bool readRotationalBundle(BitReader& in, const NetworkableHints& h, NetworkableState& s, unsigned mode = 2)
{
	const unsigned kBits = mode == 3 ? 16 : 12;
	s.mask = in.read(12);
	const float speed[3] = {h.maxSpeed.x, h.maxSpeed.y, h.maxSpeed.z};
	for (int i = 0; i < 3; ++i) {
		if (s.mask & (1u << i)) {
			s.rotation[i] = readSnormLe(in, kBits, 360.0f);
		}
	}
	for (int i = 0; i < 3; ++i) {
		if (s.mask & (8u << i)) {
			s.rotationSpeed[i] = readSnormLe(in, kBits, std::fabs(speed[i]) * 16.0f);
		}
	}
	if (s.mask & 0x40) {
		s.excessInput = readSnormLe(in, 16, 41.0f);
	}
	for (int i = 0; i < 3; ++i) {
		if (s.mask & (0x200u << i)) {
			s.parentRotation[i] = readSnormLe(in, kBits, 360.0f);
		}
	}
	if (s.mask & 0x100) {
		s.parentOrientation = readQuaternion(in, 14);
	}
	s.complete = !in.bad();
	return s.complete;
}

// 0x00605320 (Spring)
bool readSpring(BitReader& in, const NetworkableHints& h, NetworkableState& s)
{
	s.springPresent = in.read(1) == 1;
	s.mask = s.springPresent ? 1 : 0;
	if (s.springPresent) {
		s.springOffset = readDelta(in, DeltaTable::Narrow, 0.002f, kZero);
		s.springVelocity = readDelta(in, DeltaTable::Narrow, 0.002f, kZero);
		if (h.springExtra && in.read(1) == 1) {
			s.springExtra = readSnormLe(in, 16, 256.0f);
		}
	}
	s.complete = !in.bad();
	return s.complete;
}

// 0x006027e0: without a reference, mode 0 and the floats.
void writeDelta3Or(BitWriter& out, float scale, const Vec3f& value, const Vec3f* reference)
{
	if (reference) {
		writeDelta3(out, scale, value, *reference);
		return;
	}
	out.write(0, 3);
	out.writeFloat(value.x);
	out.writeFloat(value.y);
	out.writeFloat(value.z);
}

float engineRange(float a, float b) { return 1.25f * std::max(std::fabs(a), std::fabs(b)); }

// 0x00609340 (Engine)
bool readEngine(BitReader& in, const NetworkableHints& h, NetworkableState& s)
{
	s.mask = in.read(11);
	const float rz = engineRange(h.minRotation.z, h.maxRotation.z);
	const float rx = engineRange(h.minRotation.x, h.maxRotation.x);
	if (s.mask & 2) {
		s.gear = static_cast<int>(readRanged(in, static_cast<uint32_t>(std::max(0, h.engineGears) + 1)));
	}
	if (s.mask & 4) {
		s.engineRotation[1] = readSnormLe(in, 16, rz);
	}
	if (s.mask & 8) {
		s.engineRotation[0] = readSnormLe(in, 16, rx);
	}
	if (s.mask & 0x10) {
		s.engineTarget = readSnormLe(in, 16, rz); // the writer scales by 1.25 |maxSpeed.z| (as in the original)
	}
	for (int i = 0; i < 3; ++i) {
		if (s.mask & (0x20u << i)) {
			s.engineInputs[i] = readSnormLe(in, 12, 1.25f);
		}
	}
	if ((h.engineType & 0x20) && (s.mask & 0x100)) {
		s.angleOfAttack = readSnormLe(in, 16, 1.25f * std::fabs(h.maxAngleOfAttack));
	}
	if (s.mask & 0x200) {
		const bool a = in.read(1) == 1;
		const bool b = in.read(1) == 1;
		s.engineDirection = a ? (b ? -1 : 1) : 0;
	}
	if ((h.engineType & 0x80) && (s.mask & 0x400)) {
		const bool negative = in.read(1) == 1;
		const float r = h.minRpm + (h.maxRpm - h.minRpm) * static_cast<float>(in.read(16)) / 65535.0f;
		s.rpm = negative ? -r : r;
		s.clutch = static_cast<float>(in.read(8)) / 255.0f;
		s.carGear = in.readSigned(3);
	}
	s.complete = !in.bad();
	return s.complete;
}

// 0x0060a100 (MechLegs)
bool readMechLegs(BitReader& in, NetworkableState& s)
{
	s.mask = in.read(3);
	if (s.mask & 1) {
		s.legState = static_cast<int>(in.read(4));
	}
	if (s.mask & 2) {
		s.legFlag1 = in.read(1) == 1;
	}
	if (s.mask & 4) {
		s.legFlag2 = in.read(1) == 1;
	}
	s.complete = !in.bad();
	return s.complete;
}

// 0x00609d50 (TargetObject)
bool readTargetObject(BitReader& in, const Vec3f& reference, NetworkableState& s)
{
	s.mask = in.read(5);
	if (s.mask & 1) {
		s.hasPosition = true;
		s.position = readDelta(in, DeltaTable::Wide, 0.0005f, reference);
	}
	if (s.mask & 2) {
		s.targetActive = in.read(1) == 1;
	}
	if (s.mask & 4) {
		s.targetValue = static_cast<int>(in.read(8));
	}
	s.complete = !in.bad();
	return s.complete;
}

// 0x00604120 (Item)
bool readItem(BitReader& in, const Vec3f& reference, NetworkableState& s)
{
	s.mask = in.read(5);
	if (s.mask & 1) {
		s.hasPosition = true;
		s.position = readDelta(in, DeltaTable::Wide, 0.0005f, reference);
	}
	if (s.mask & 2) {
		s.itemValue = in.readSigned(9);
	}
	if (s.mask & 8) {
		s.itemActive = in.read(1) == 1;
	}
	if (s.mask & 0x10) {
		s.itemUnlock = in.readSigned(5);
	}
	s.complete = !in.bad();
	return s.complete;
}

// 0x00602c50 (FreeCamera)
bool readFreeCamera(BitReader& in, const Vec3f& reference, NetworkableState& s)
{
	s.mask = in.read(3);
	if (s.mask & 2) {
		s.hasPosition = true;
		s.position = readDelta3(in, 0.001f, reference);
	}
	if (s.mask & 1) {
		s.hasOrientation = true;
		s.orientation = readQuaternion(in, 16);
	}
	if (s.mask & 4) {
		s.cameraValue = in.readSigned(9);
	}
	s.complete = !in.bad();
	return s.complete;
}

// 0x00603980 (OverheadCamera)
bool readOverheadCamera(BitReader& in, const NetworkableHints& h, const Vec3f& reference, NetworkableState& s)
{
	s.mask = in.read(4);
	if (s.mask & 1) {
		s.cameraPan[0] = readSnorm(in, 8, h.overheadRange);
		s.cameraPan[1] = readSnorm(in, 8, h.overheadRange);
	}
	if (s.mask & 2) {
		s.hasPosition = true;
		s.position = readDelta3(in, 0.001f, reference);
	}
	if (s.mask & 4) {
		s.cameraValue = in.readSigned(8);
	}
	if (s.mask & 8) {
		s.cameraFlag = in.read(1) == 1;
	}
	s.complete = !in.bad();
	return s.complete;
}

// 0x00604e00 (DestroyableWindowsBundle): no mask, a bit per window.
bool readWindows(BitReader& in, const NetworkableHints& h, NetworkableState& s)
{
	if (h.windows < 0) {
		return false;
	}
	s.windowBits = h.windows > 0 ? in.read(static_cast<unsigned>(std::min(h.windows, 32))) : 0;
	s.complete = !in.bad();
	return s.complete;
}

// 0x00605b00 (PlayerControlObject with a parent: a seat)
bool readSeat(BitReader& in, const NetworkableHints& h, const Vec3f& reference, NetworkableState& s, unsigned mode = 2)
{
	if (h.seatCameras < 0) {
		return false;
	}
	if (h.seatCameras == 0) {
		// The occupant's Soldier state; 0x00605b00 only parses it with a
		// soldier in the seat (vtable 0x148) and skips it otherwise.
		s.occupantBits = in.read(11);
		const size_t start = in.position();
		if (s.occupantBits > 0) {
			auto occupant = std::make_shared<NetworkableState>();
			NetworkableHints soldier;
			soldier.kind = NetworkableKind::Soldier;
			// The seat hands its mode on (0x00605b00: the soldier's slot 0x30
			// with the same last argument).
			if (readSoldier(in, soldier, reference, *occupant, mode) && in.position() <= start + s.occupantBits) {
				occupant->kind = NetworkableKind::Soldier;
				s.occupant = occupant;
			}
			in.seek(start + s.occupantBits);
		}
		s.complete = !in.bad();
		return s.complete;
	}
	s.mask = in.read(4);
	for (int g = 0; g < 3; ++g) {
		if (s.mask & (1u << g)) {
			s.seatWeapon[g] = static_cast<int>(readRanged(in, 9)) - 1; // ranged(0, 8) of index + 1
		}
	}
	if (s.mask & 8) {
		s.seatDucking = in.read(1) == 1;
	}
	s.complete = !in.bad();
	return s.complete;
}

// 0x005981a0 (the player)
bool readPlayer(BitReader& in, NetworkableState& s)
{
	PlayerNetState& p = s.player;
	s.mask = in.read(30);
	const uint32_t m = s.mask;
	if (m & 1) {
		p.hasAlive = true;
		p.alive = in.read(1) == 1;
	}
	if (m & 2) {
		p.spawnGroup = static_cast<int>(readRanged(in, 256));
	}
	if (m & 8) {
		p.hasStats = true;
		p.score = in.readSigned(12);
		p.deaths = static_cast<int>(readRanged(in, 0x800));
		p.kills = static_cast<int>(readRanged(in, 0x800));
		p.teamwork = in.readSigned(12);
		p.teamKills = static_cast<int>(readRanged(in, 0x800));
		p.value104 = static_cast<int>(readRanged(in, 0x800));
	}
	if (m & 0x400000) {
		p.hasFlagHolder = true;
		p.flagHolder = in.read(1) == 1;
	}
	if (m & 0x10) {
		in.read(1);
	}
	if (m & 0x4000000) {
		in.read(2);
	}
	// Three kit slots (masks 0x40, 0x80, 0x20000; 0x10000 all).
	const uint32_t slots[3] = {0x40, 0x80, 0x20000};
	for (uint32_t slot : slots) {
		if (!(m & slot) && !(m & 0x10000)) {
			continue;
		}
		const bool present = in.read(1) == 1;
		if (present || (m & 0x10000)) {
			in.read(bitsFor(0x40));
			in.read(2);
			in.read(1);
			in.read(in.read(1) == 1 ? 8 : 6);
			in.read(4);
		}
	}
	if (m & 0x40000) {
		in.read(1);
		in.read(1);
	}
	if (m & 0x100) {
		p.ping = static_cast<int>(in.read(bitsFor(0x400)));
	}
	if (m & 0x20) {
		p.hasSpawnTick = true;
		p.spawnTick = in.read(32);
		p.manDown = in.read(1) == 1;
	}
	if (m & 0x200) {
		p.team = static_cast<int>(in.read(bitsFor(4))) - 1;
	}
	if (m & 0x400) {
		p.value400 = in.read(bitsFor(9));
		in.read(1);
	}
	if (m & 0x800) {
		p.value294 = static_cast<int>(in.read(bitsFor(0x100)));
	}
	if (m & 0x1000) {
		p.kit = static_cast<int>(in.read(bitsFor(0x10)));
	}
	if (m & 0x10000000) {
		for (int i = 0; i < 4; ++i) {
			in.read(8);
		}
	}
	if (m & 0x20000000) {
		for (int i = 0; i < 4; ++i) {
			in.read(8);
		}
	}
	if (m & 0x2000) {
		p.squad = static_cast<int>(in.read(bitsFor(0x10)));
	}
	if (m & 0x4000) {
		p.rank = static_cast<int>(in.read(bitsFor(0x31))) - 1;
	}
	if (m & 0x8000) {
		in.read(1);
	}
	if (m & 0x800000) {
		in.read(1);
	}
	if (m & 0x1000000) {
		in.read(8);
	}
	if (m & 0x2000000) {
		in.read(bitsFor(0x10001));
	}
	const bool b38 = in.read(1) == 1;
	p.squadLeader = b38;
	if (m & 0x8000000) {
		const bool b39 = in.read(1) == 1;
		p.hasCommander = true;
		p.commander = b39;
		if (b39 || b38) {
			for (int i = 0; i < 6; ++i) {
				in.read(1);
			}
			for (int i = 0; i < 4; ++i) {
				in.read(8);
			}
		}
	}
	in.read(1);
	s.complete = !in.bad();
	return s.complete;
}

} // namespace

bool decodeNetworkable(BitReader& in, const NetworkableHints& hints, const Vec3f& reference, NetworkableState& out,
	unsigned mode, const MotionHistory* history)
{
	out = {};
	out.kind = hints.kind;
	switch (hints.kind) {
	case NetworkableKind::SimpleObject: return readSimpleObject(in, hints, reference, out, mode, history);
	case NetworkableKind::Soldier: return readSoldier(in, hints, reference, out, mode);
	case NetworkableKind::Kit: return readKit(in, reference, out);
	case NetworkableKind::GenericProjectile: return readProjectile(in, hints, reference, out);
	case NetworkableKind::ControlPoint: return readControlPoint(in, out);
	case NetworkableKind::DestroyableObject: return readDestroyable(in, hints, reference, out);
	case NetworkableKind::Player: return readPlayer(in, out);
	case NetworkableKind::RotationalBundle: return readRotationalBundle(in, hints, out, mode);
	case NetworkableKind::Spring: return readSpring(in, hints, out);
	case NetworkableKind::Engine: return readEngine(in, hints, out);
	case NetworkableKind::MechLegs: return readMechLegs(in, out);
	case NetworkableKind::TargetObject: return readTargetObject(in, reference, out);
	case NetworkableKind::Item: return readItem(in, reference, out);
	case NetworkableKind::FreeCamera: return readFreeCamera(in, reference, out);
	case NetworkableKind::OverheadCamera: return readOverheadCamera(in, hints, reference, out);
	case NetworkableKind::DestroyableWindowsBundle: return readWindows(in, hints, out);
	case NetworkableKind::PlayerControlObject: return readSeat(in, hints, reference, out, mode);
	default: return false; // the other classes' layouts aren't decoded
	}
}

// ---------------------------------------------------------------------------
// Writers (mode 2), as the originals lay the fields out

void encodeNetworkable(BitWriter& out, const NetworkableHints& h, const Vec3f* reference, const NetworkableState& s,
	unsigned mode, const MotionHistory* history)
{
	const bool full = mode == 3;
	const uint32_t m = s.mask;
	switch (h.kind) {
	case NetworkableKind::SimpleObject: { // 0x005ff5d0
		out.write(m, 25);
		out.write(s.flag ? 1 : 0, 1);
		if (m & 2) {
			writeDelta(out, DeltaTable::Wide, 0.0005f, s.position, history ? &history->position : reference);
		}
		const bool physics = h.mobile && !s.flag;
		if (physics) {
			if (m & 4) {
				writeDelta(out, DeltaTable::Wide, 0.001f, s.velocity, history ? &history->velocity : &kZero);
			}
			if (m & 8) {
				if (history) {
					writeDelta(out, DeltaTable::Wide, 0.001f, s.angularVelocity, &history->angularVelocity);
				} else if (mode == 3) {
					writeDelta(out, DeltaTable::Wide, 0.001f, s.angularVelocity, &kZero);
				} else {
					writeDelta3(out, 0.01f, s.angularVelocity, kZero);
				}
			}
		}
		if (m & 1) {
			writeQuaternion(out, s.orientation, 14);
		}
		if (physics) {
			if (m & 0x10000) {
				writeDelta(out, DeltaTable::Wide, 0.001f, kZero, &kZero);
				writeDelta(out, DeltaTable::Wide, 0.001f, kZero, &kZero);
			}
			for (uint32_t bit : {0x80u, 0x100u, 0x400u, 0x800u}) {
				if (m & bit) {
					writeDelta(out, DeltaTable::Wide, 0.0001f, kZero, &kZero);
				}
			}
			if (m & 0x200) {
				out.write(s.sleeping ? 0 : 1, 1);
			}
			if (m & 0x20000) {
				out.write(s.supplyFlag ? 1 : 0, 1);
			}
		}
		if (h.hasArmor) {
			if (m & 0x10) {
				writeHitPoints(out, h, s.health);
			}
			if (m & 0x20) {
				out.write(static_cast<uint32_t>(std::clamp(s.empLevel, 0, 15)), 4);
			}
		}
		if (h.hasTeamChange && (m & 0x40)) {
			out.write(static_cast<uint32_t>(std::clamp(s.changePoints, 0, 255)), 8);
		}
		if (m & 0x1000) {
			out.write(static_cast<uint32_t>(s.spawnTeam) & 3, 2);
		}
		if (h.inVehicle) {
			if (h.parachute) {
				if (m & 0x8000) {
					writeRanged(out, static_cast<uint32_t>(std::clamp(s.parachuteState, 0, 7)), 8);
				}
			} else {
				const uint32_t slots[3] = {0x2000, 0x4000, 0x80000};
				for (int g = 0; g < 3; ++g) {
					if (m & slots[g]) {
						writeRanged(out, static_cast<uint32_t>(std::clamp(s.seatWeapon[g] + 1, 0, 8)), 9);
					}
				}
				if (m & 0x40000) {
					writeRanged(out, static_cast<uint32_t>(std::clamp(s.vehicleValue, 0, 15)), 16);
				}
				if (m & 0x100000) {
					out.write(s.ducking ? 1 : 0, 1);
				}
				if (m & 0x200000) {
					out.write(s.vehicleFlag ? 1 : 0, 1);
				}
			}
		}
		if (m & 0x800000) {
			for (size_t i = 0; i < h.armoredChildren.size(); ++i) {
				const int v = i < s.childDamage.size() ? std::clamp(s.childDamage[i], 0, 8) : 8;
				if (h.armoredChildren[i]) {
					writeRanged(out, static_cast<uint32_t>(v), 9);
				} else {
					out.write(v > 0 ? 1 : 0, 1);
				}
			}
		}
		if (m & 0x1000000) {
			out.write(s.responseFlag ? 1 : 0, 1);
		}
		break;
	}
	case NetworkableKind::Soldier: { // 0x005fb8c0
		out.write(m, 21);
		if (m & 0x40) {
			out.write(static_cast<uint32_t>(std::lround(std::clamp(s.health, 0.0f, 1.0f) * 255.0f)), 8);
			out.write(s.soldierBits & 1, 1);
		}
		if (m & 0x80) {
			out.write(static_cast<uint32_t>(std::clamp(s.empLevel, 0, 15)), 4);
		}
		if (m & 0x20) {
			writeRanged(out, static_cast<uint32_t>(std::clamp(s.soldierValues[0], 0, 4)), 5);
			writeRanged(out, static_cast<uint32_t>(std::clamp(s.soldierValues[1], 0, 4)), 5);
		}
		if (m & 0x10000) {
			// The ragdoll: its networked particles (only `position` when
			// none are given), the length first.
			std::vector<Vec3f> particles = s.ragdollParticles;
			if (particles.empty()) {
				particles.push_back(s.position);
			}
			BitWriter body;
			body.writeFloat(particles[0].x);
			body.writeFloat(particles[0].y);
			body.writeFloat(particles[0].z);
			for (size_t i = 1; i < particles.size(); ++i) {
				writeDelta3(body, 0.01f, particles[i], particles[0]);
			}
			out.write(static_cast<uint32_t>(body.bits()), 9);
			out.append(body);
			break;
		}
		if (m & 1) {
			writeDelta(out, DeltaTable::Wide, 0.001f, s.position, reference);
		}
		if (full) {
			// 0x005fb8c0 with mode 3 (0x00687510 per vector).
			if (m & 0x100) {
				writeDelta(out, DeltaTable::Wide, 0.001f, s.velocity, &kZero);
			}
			if (m & 0x200) {
				writeDelta(out, DeltaTable::Wide, 0.0001f, s.soldierAcceleration, &kZero);
			}
			if (m & 0x400) {
				writeDelta(out, DeltaTable::Wide, 0.0001f, s.soldierNode88, &kZero);
			}
			if (m & 0x80000) {
				writeDelta(out, DeltaTable::Wide, 0.001f, s.soldierImpulse, &kZero);
			}
			if (m & 2) {
				out.writeFloat(s.yaw);
			}
			if (m & 4) {
				out.writeFloat(s.pitch);
			}
			if (m & 8) {
				out.writeFloat(s.roll);
			}
			if (m & 0x10) {
				out.writeFloat(s.aimPitch);
			}
		} else {
			if (m & 0x100) {
				writeDelta(out, DeltaTable::Wide, 0.01f, s.velocity, &kZero);
			}
			if (m & 2) {
				writeSnorm(out, s.yaw, 12, 360.0f);
			}
			if (m & 4) {
				writeSnorm(out, s.pitch, 12, 90.0f);
			}
			if (m & 8) {
				writeSnorm(out, s.roll, 12, 180.0f);
			}
			if (m & 0x10) {
				writeSnorm(out, s.aimPitch, 12, 90.0f);
			}
		}
		writeRanged(out, static_cast<uint32_t>(std::clamp(s.weaponZoom, 0, 3)), 4);
		writeRanged(out, static_cast<uint32_t>(std::clamp(s.weaponFireState, 0, 1)), 2);
		for (int i = 0; i < 3; ++i) {
			out.write(s.soldierBits >> (i + 1) & 1, 1);
		}
		if (m & 0x800) {
			writeSnorm(out, s.soldierLean[0], full ? 16 : 10, 50.0f);
		}
		if (m & 0x1000) {
			writeSnorm(out, s.soldierLean[1], full ? 16 : 10, 50.0f);
		}
		out.write(s.responseFlag ? 1 : 0, 1);
		if (m & 0x8000) {
			// ROUND(clamp(stamina) x 127) and the sprinting bit (0x005fb300) in
			// mode 3; 5 bits otherwise.
			const float stamina = std::clamp(s.stamina, 0.0f, 1.0f);
			if (full) {
				out.write(static_cast<uint32_t>(std::lround(stamina * 127.0f)), 7);
				out.write(s.sprinting ? 1 : 0, 1);
			} else {
				out.write(static_cast<uint32_t>(std::lround(stamina * 31.0f)), 5);
			}
		}
		if (m & 0x2000) {
			const int slots = std::max(0, h.soldierItemSlots);
			writeRanged(out, static_cast<uint32_t>(std::clamp(s.weapon + 1, 0, slots + 1)), static_cast<uint32_t>(slots + 2));
		}
		if (m & 0x4000) {
			out.write(static_cast<uint32_t>(s.soldierValues[2]) & 3, 2);
		}
		if (full && (m & 0x40000)) {
			writeSnorm(out, s.soldierValue40000, 7, 1.0f);
		}
		if (m & 0x20000) {
			writeRanged(out, static_cast<uint32_t>(std::clamp(s.ladder, 0, 255)), 256);
			if (s.ladder != 0) {
				writeUnit(out, s.ladderPosition, 12);
			}
		}
		if (m & 0x100000) {
			writeRanged(out, static_cast<uint32_t>(std::clamp(s.vehicleValue, 0, 15)), 16);
		}
		break;
	}
	case NetworkableKind::Kit: // 0x00605f90
		out.write(m, 1);
		if (m & 1) {
			writeDelta(out, DeltaTable::Wide, 0.0005f, s.position, reference);
		}
		break;
	case NetworkableKind::GenericProjectile: // 0x006067c0
		out.write(m, 6);
		if (m & 2) {
			writeDelta(out, DeltaTable::Wide, 0.01f, s.position, reference);
		}
		if (m & 1) {
			writeQuaternion(out, s.orientation, 12);
		}
		if (m & 4) {
			out.write(s.hasVelocity ? 1 : 0, 1);
			if (s.hasVelocity) {
				out.writeSigned(0, 16);
				writeDelta(out, DeltaTable::Narrow, 0.01f, s.velocity, &kZero);
				out.write(0, 1);
			}
		}
		if ((m & 0x10) && h.hasArmor) {
			writeHitPoints(out, h, s.health);
		}
		if (m & 0x20) {
			out.write(0, 1);
		}
		break;
	case NetworkableKind::ControlPoint: // 0x006082b0
		out.write(m, 6);
		if (m & 1) {
			out.writeSigned(s.team, 4);
		}
		if (m & 2) {
			out.write(s.cpNeutralStart ? 1 : 0, 1);
		}
		if (m & 4) {
			// The writer scales by 0.8 into 16 bits, the reader back by 1.25.
			writeSnorm(out, s.cpValues[0], 16, 1.25f);
			writeSnorm(out, s.cpValues[1], 16, 1.25f);
		}
		if (m & 8) {
			out.writeSigned(s.cpAllowTeam1 ? 1 : 0, 4);
		}
		if (m & 0x10) {
			out.writeSigned(s.cpAllowTeam2 ? 1 : 0, 4);
		}
		if (m & 0x20) {
			out.writeSigned(s.cpValue, 4);
		}
		break;
	case NetworkableKind::DestroyableObject: // 0x006044e0
		out.write(m, 3);
		if (h.hasArmor) {
			if (m & 1) {
				writeHitPoints(out, h, s.health);
			}
			if (m & 2) {
				writeDelta(out, DeltaTable::Wide, 0.01f, s.position, reference);
				out.write(static_cast<uint32_t>(std::lround(std::clamp(s.destroyValue * 0.01f, 0.0f, 1.0f) * 1023.0f)), 10);
			}
		}
		if (m & 4) {
			for (size_t i = 0; i < h.armoredChildren.size(); ++i) {
				out.write(i < s.childDamage.size() && s.childDamage[i] == 0 ? 0 : 1, 1);
			}
		}
		break;
	case NetworkableKind::Player: { // 0x00596100
		const PlayerNetState& p = s.player;
		out.write(m, 30);
		if (m & 1) {
			out.write(p.alive ? 1 : 0, 1);
		}
		if (m & 2) {
			writeRanged(out, static_cast<uint32_t>(std::clamp(p.spawnGroup, 0, 255)), 256);
		}
		if (m & 8) {
			out.writeSigned(p.score, 12);
			writeRanged(out, static_cast<uint32_t>(p.deaths), 0x800);
			writeRanged(out, static_cast<uint32_t>(p.kills), 0x800);
			out.writeSigned(p.teamwork, 12);
			writeRanged(out, static_cast<uint32_t>(p.teamKills), 0x800);
			writeRanged(out, static_cast<uint32_t>(p.value104), 0x800);
		}
		if (m & 0x400000) {
			out.write(p.flagHolder ? 1 : 0, 1);
		}
		if (m & 0x10) {
			out.write(0, 1);
		}
		if (m & 0x4000000) {
			out.write(0, 2);
		}
		for (uint32_t slot : {0x40u, 0x80u, 0x20000u}) {
			if (!(m & slot) && !(m & 0x10000)) {
				continue;
			}
			out.write(0, 1); // no kit item
			if (m & 0x10000) {
				out.write(0, bitsFor(0x40));
				out.write(0, 2);
				out.write(0, 1);
				out.write(0, 1);
				out.write(0, 6);
				out.write(0, 4);
			}
		}
		if (m & 0x40000) {
			out.write(0, 2);
		}
		if (m & 0x100) {
			out.write(static_cast<uint32_t>(std::clamp(p.ping, 0, 1023)), bitsFor(0x400));
		}
		if (m & 0x20) {
			out.write(p.spawnTick, 32);
			out.write(p.manDown ? 1 : 0, 1);
		}
		if (m & 0x200) {
			out.write(static_cast<uint32_t>(p.team + 1), bitsFor(4));
		}
		if (m & 0x400) {
			out.write(p.value400, bitsFor(9));
			out.write(0, 1);
		}
		if (m & 0x800) {
			out.write(static_cast<uint32_t>(std::max(0, p.value294)), bitsFor(0x100));
		}
		if (m & 0x1000) {
			out.write(static_cast<uint32_t>(std::max(0, p.kit)), bitsFor(0x10));
		}
		if (m & 0x10000000) {
			out.write(0, 32);
		}
		if (m & 0x20000000) {
			out.write(0, 32);
		}
		if (m & 0x2000) {
			out.write(static_cast<uint32_t>(std::max(0, p.squad)), bitsFor(0x10));
		}
		if (m & 0x4000) {
			out.write(static_cast<uint32_t>(std::max(0, p.rank + 1)), bitsFor(0x31)); // rank + 1
		}
		if (m & 0x8000) {
			out.write(0, 1);
		}
		if (m & 0x800000) {
			out.write(0, 1);
		}
		if (m & 0x1000000) {
			out.write(0, 8);
		}
		if (m & 0x2000000) {
			out.write(0, bitsFor(0x10001));
		}
		out.write(p.squadLeader ? 1 : 0, 1);
		if (m & 0x8000000) {
			out.write(p.commander ? 1 : 0, 1);
			if (p.commander || p.squadLeader) {
				out.write(0, 6); // player +0x10e..+0x113
				out.write(0, 32); // four 0..1 floats in 8 bits (+0x114..+0x120)
			}
		}
		out.write(0, 1); // player +0x1c0
		break;
	}
	case NetworkableKind::RotationalBundle: { // 0x006015a0
		out.write(m, 12);
		const unsigned bits = full ? 16 : 12; // (mode == 3) * 4 + 12
		const float speed[3] = {h.maxSpeed.x, h.maxSpeed.y, h.maxSpeed.z};
		for (int i = 0; i < 3; ++i) {
			if (m & (1u << i)) {
				writeSnormTrunc(out, s.rotation[i], bits, 360.0f);
			}
		}
		for (int i = 0; i < 3; ++i) {
			if (m & (8u << i)) {
				writeSnormTrunc(out, s.rotationSpeed[i], bits, std::fabs(speed[i]) * 16.0f);
			}
		}
		if (m & 0x40) {
			writeSnorm(out, s.excessInput, 16, 41.0f);
		}
		for (int i = 0; i < 3; ++i) {
			if (m & (0x200u << i)) {
				writeSnormTrunc(out, s.parentRotation[i], bits, 360.0f);
			}
		}
		if (m & 0x100) {
			writeQuaternion(out, s.parentOrientation, 14);
		}
		break;
	}
	case NetworkableKind::Spring: // 0x00605170
		out.write(s.springPresent ? 1 : 0, 1);
		if (s.springPresent) {
			writeDelta(out, DeltaTable::Narrow, 0.002f, s.springOffset, &kZero);
			writeDelta(out, DeltaTable::Narrow, 0.002f, s.springVelocity, &kZero);
			if (h.springExtra) {
				const bool small = std::fabs(s.springExtra) < 256.0f;
				out.write(small ? 1 : 0, 1);
				if (small) {
					writeSnorm(out, s.springExtra, 16, 256.0f);
				}
			}
		}
		break;
	case NetworkableKind::Engine: { // 0x00608df0
		out.write(m, 11);
		const float rz = engineRange(h.minRotation.z, h.maxRotation.z);
		const float rx = engineRange(h.minRotation.x, h.maxRotation.x);
		if (m & 2) {
			writeRanged(out, static_cast<uint32_t>(std::max(0, s.gear)), static_cast<uint32_t>(std::max(0, h.engineGears) + 1));
		}
		if (m & 4) {
			writeSnorm(out, s.engineRotation[1], 16, rz);
		}
		if (m & 8) {
			writeSnorm(out, s.engineRotation[0], 16, rx);
		}
		if (m & 0x10) {
			writeSnorm(out, s.engineTarget, 16, rz); // the original scales by 1.25 |maxSpeed.z| here
		}
		for (int i = 0; i < 3; ++i) {
			if (m & (0x20u << i)) {
				writeSnorm(out, s.engineInputs[i], 12, 1.25f);
			}
		}
		if ((h.engineType & 0x20) && (m & 0x100)) {
			writeSnorm(out, s.angleOfAttack, 16, 1.25f * std::fabs(h.maxAngleOfAttack));
		}
		if (m & 0x200) {
			out.write(s.engineDirection != 0 ? 1 : 0, 1);
			out.write(s.engineDirection < 0 ? 1 : 0, 1);
		}
		if ((h.engineType & 0x80) && (m & 0x400)) {
			out.write(s.rpm < 0 ? 1 : 0, 1);
			const float span = h.maxRpm - h.minRpm;
			const float u = span > 0 ? std::clamp((std::fabs(s.rpm) - h.minRpm) / span, 0.0f, 1.0f) : 0.0f;
			out.write(static_cast<uint32_t>(std::lround(u * 65535.0f)), 16);
			writeUnit(out, s.clutch, 8);
			out.writeSigned(s.carGear, 3);
		}
		break;
	}
	case NetworkableKind::MechLegs: // 0x0060a000
		out.write(m, 3);
		if (m & 1) {
			out.write(static_cast<uint32_t>(std::max(0, s.legState)), 4);
		}
		if (m & 2) {
			out.write(s.legFlag1 ? 1 : 0, 1);
		}
		if (m & 4) {
			out.write(s.legFlag2 ? 1 : 0, 1);
		}
		break;
	case NetworkableKind::TargetObject: // 0x00609c60
		out.write(m, 5);
		if (m & 1) {
			writeDelta(out, DeltaTable::Wide, 0.0005f, s.position, reference);
		}
		if (m & 2) {
			out.write(s.targetActive ? 1 : 0, 1);
		}
		if (m & 4) {
			out.write(static_cast<uint32_t>(s.targetValue), 8);
		}
		break;
	case NetworkableKind::Item: // 0x00603fe0
		out.write(m, 5);
		if (m & 1) {
			writeDelta(out, DeltaTable::Wide, 0.0005f, s.position, reference);
		}
		if (m & 2) {
			out.writeSigned(s.itemValue, 9);
		}
		if (m & 8) {
			out.write(s.itemActive ? 1 : 0, 1);
		}
		if (m & 0x10) {
			out.writeSigned(s.itemUnlock, 5);
		}
		break;
	case NetworkableKind::FreeCamera: // 0x00602b70
		out.write(m, 3);
		if (m & 2) {
			writeDelta3Or(out, 0.001f, s.position, reference);
		}
		if (m & 1) {
			writeQuaternion(out, s.orientation, 16);
		}
		if (m & 4) {
			out.writeSigned(s.cameraValue, 9);
		}
		break;
	case NetworkableKind::OverheadCamera: // 0x006037a0
		out.write(m, 4);
		if (m & 1) {
			writeSnorm(out, s.cameraPan[0], 8, h.overheadRange);
			writeSnorm(out, s.cameraPan[1], 8, h.overheadRange);
		}
		if (m & 2) {
			writeDelta3Or(out, 0.001f, s.position, reference);
		}
		if (m & 4) {
			out.writeSigned(s.cameraValue, 8);
		}
		if (m & 8) {
			out.write(s.cameraFlag ? 1 : 0, 1);
		}
		break;
	case NetworkableKind::DestroyableWindowsBundle: // 0x00604d70
		if (h.windows > 0) {
			out.write(s.windowBits, static_cast<unsigned>(std::min(h.windows, 32)));
		}
		break;
	case NetworkableKind::PlayerControlObject: // 0x00605650
		if (h.seatCameras <= 0) {
			// 11 bits of length, patched after the soldier's state.
			if (!s.occupant) {
				out.write(0, 11);
				break;
			}
			BitWriter soldier;
			NetworkableHints sh;
			sh.kind = NetworkableKind::Soldier;
			encodeNetworkable(soldier, sh, reference, *s.occupant, mode);
			out.write(static_cast<uint32_t>(soldier.bits()), 11);
			out.append(soldier);
			break;
		}
		out.write(m, 4);
		for (int g = 0; g < 3; ++g) {
			if (m & (1u << g)) {
				writeRanged(out, static_cast<uint32_t>(std::clamp(s.seatWeapon[g] + 1, 0, 8)), 9);
			}
		}
		if (m & 8) {
			out.write(s.seatDucking ? 1 : 0, 1);
		}
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------------------
// The player's own state in the control-object record (player vtable
// 0x1c8 writes, 0x00594cc0; 0x1cc reads, 0x00596ed0)

namespace {

// Bits for a value below `limit` as the loops at 0x00594cc0 count them:
// the smallest n with 2^n - 1 >= limit.
unsigned loopBits(uint32_t limit) { return bitsFor(limit); }

} // namespace

bool decodePlayerOwnState(BitReader& in, const PlayerOwnStateHints& h, PlayerOwnState& s)
{
	s = {};
	s.flag210 = in.read(1) == 1;
	s.flag16a = in.read(1) == 1;
	s.flag168 = in.read(1) == 1;
	s.flag169 = in.read(1) == 1;
	s.flag16b = in.read(1) == 1;
	s.value14c = static_cast<int>(in.read(8));
	s.hasHitDirection = in.read(1) == 1;
	if (s.hasHitDirection) {
		s.hitDirection = readSnorm(in, 8, 6.2831855f);
	}
	s.hasHitStrength = in.read(1) == 1;
	if (s.hasHitStrength) {
		s.hitStrength = static_cast<float>(in.read(8)) * (25.0f / 255.0f);
	}
	if (h.controlObject) {
		for (int g = 0; g < 3; ++g) {
			PlayerOwnState::Weapon& w = s.weapons[g];
			w.active = in.read(1) == 1;
			if (!w.active) {
				continue;
			}
			w.index = static_cast<int>(in.read(loopBits(0x10))) - 1;
			w.value208 = static_cast<int>(in.read(2));
			w.value238 = static_cast<int>(in.read(2));
			w.value1fc = static_cast<int>(in.read(2));
			w.ammo = static_cast<int>(in.read(loopBits(0x400))) - 1;
			w.magazines = static_cast<int>(in.read(loopBits(0x200))) - 1;
			if (h.weaponHeat[g]) {
				w.heat = static_cast<int>(static_cast<float>(in.read(8)) * (1.0f / 255.0f) * 6.0f);
			}
		}
		s.hasCharge = in.read(1) == 1;
		if (s.hasCharge) {
			s.charge = static_cast<float>(in.read(8)) / 255.0f;
		}
	}
	s.spawnTick = in.read(32);
	s.manDown = in.read(1) == 1;
	const bool negative = in.read(1) == 1;
	const int health = static_cast<int>(in.read(8));
	s.health = static_cast<float>(negative ? -health : health) / 255.0f;
	s.flag124 = in.read(1) == 1;
	s.flag19f = in.read(1) == 1;
	s.flag1a0 = in.read(1) == 1;
	s.commander = in.read(1) == 1;
	s.complete = !in.bad();
	return s.complete;
}

void encodePlayerOwnState(BitWriter& out, const PlayerOwnStateHints& h, const PlayerOwnState& s)
{
	out.write(s.flag210 ? 1 : 0, 1);
	out.write(s.flag16a ? 1 : 0, 1);
	out.write(s.flag168 ? 1 : 0, 1);
	out.write(s.flag169 ? 1 : 0, 1);
	out.write(s.flag16b ? 1 : 0, 1);
	out.write(static_cast<uint32_t>(s.value14c) & 0xff, 8);
	out.write(s.hasHitDirection ? 1 : 0, 1);
	if (s.hasHitDirection) {
		writeSnorm(out, s.hitDirection, 8, 6.2831855f); // x 1/(2 pi), (v + 1) / 2, x 255
	}
	out.write(s.hasHitStrength ? 1 : 0, 1);
	if (s.hasHitStrength) {
		out.write(static_cast<uint32_t>(std::lround(std::clamp(std::min(s.hitStrength, 25.0f) * 0.04f, 0.0f, 1.0f) * 255.0f)), 8);
	}
	if (h.controlObject) {
		for (int g = 0; g < 3; ++g) {
			const PlayerOwnState::Weapon& w = s.weapons[g];
			out.write(w.active ? 1 : 0, 1);
			if (!w.active) {
				continue;
			}
			out.write(static_cast<uint32_t>(std::clamp(w.index + 1, 0, 15)), loopBits(0x10));
			out.write(static_cast<uint32_t>(w.value208) & 3, 2);
			out.write(static_cast<uint32_t>(w.value238) & 3, 2);
			out.write(static_cast<uint32_t>(w.value1fc) & 3, 2);
			out.write(static_cast<uint32_t>(std::clamp(w.ammo + 1, 0, 0x3ff)), loopBits(0x400));
			out.write(static_cast<uint32_t>(std::clamp(w.magazines + 1, 0, 0x1ff)), loopBits(0x200));
			if (h.weaponHeat[g]) {
				out.write(static_cast<uint32_t>(std::lround(std::clamp(w.heat / 6.0f, 0.0f, 1.0f) * 255.0f)), 8);
			}
		}
		out.write(s.hasCharge ? 1 : 0, 1);
		if (s.hasCharge) {
			out.write(static_cast<uint32_t>(std::lround(std::clamp(s.charge, 0.0f, 1.0f) * 255.0f)), 8);
		}
	}
	out.write(s.spawnTick, 32);
	out.write(s.manDown ? 1 : 0, 1);
	const int health = static_cast<int>(std::lround(std::clamp(s.health, -1.0f, 1.0f) * 255.0f));
	out.write(health < 0 ? 1 : 0, 1);
	out.write(static_cast<uint32_t>(std::abs(health)), 8);
	out.write(s.flag124 ? 1 : 0, 1);
	out.write(s.flag19f ? 1 : 0, 1);
	out.write(s.flag1a0 ? 1 : 0, 1);
	out.write(s.commander ? 1 : 0, 1);
}

} // namespace dice::hfe::net
