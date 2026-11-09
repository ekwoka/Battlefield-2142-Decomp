#include "Bots.h"

#include "Console/ConsoleBinding.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dice::hfe::game {

namespace {

constexpr float kDegrees = 180.0f / 3.14159265f;
constexpr float kRadians = 3.14159265f / 180.0f;
// The bot manager's +0xdc (0x100c9c50; its slot 0x68, 0x100c5cd0): seats
// of vehicles under it are refused, and a bot leaving one says bail_out
// under it + 0.1.
constexpr float kVehicleFloor = 0.3f;
constexpr float kEyeHeight = 1.6f;    // above the feet (the soldier fires from here)
// Where the bots look at and aim at a soldier: its physics node (BF2142.exe
// IAIObjectEnvironment slot 0x54, 0x008999a0: the soldier's node, its
// interface 0xc378's slot 0x60 in the world and 0xdc in the soldier's
// frame), the physics origin a metre over the feet. The sensing rays
// (0x100ed700) all go there for a soldier (its ITSoldier bit takes slot
// 0x54 for every ray) and the memory item keeps the point (+4, 0x100ed2c0),
// which the aim (0x100d9a60) takes through the soldier's matrix again.
constexpr float kChestHeight = 1.0f;
// A soldier's AI radius (see soldierAiRadius: the built-in soldier
// collision mesh's, sqrt(1.32) m).
constexpr float kSoldierRadius = 1.1489125f;
constexpr float kSoldierSpeed = 5.0f;  // SoldierMobile's maxSpeed (Objects/Soldiers/Common/ai/Objects.ai)
// Native recovery policy: bound the actual mesh route, not just the straight
// line. 400 m is about 80 seconds at the nominal 5 m/s infantry speed.
bool infantryRecoveryRoute(const ai::NavMesh* mesh, const ai::Vec3& from, const ai::Vec3& target)
{
	constexpr float maxWalk = 400.0f;
	const ai::Vec3 delta = target - from;
	if (!mesh || delta.x * delta.x + delta.y * delta.y + delta.z * delta.z > maxWalk * maxWalk) return false;
	const auto path = ai::findPath(*mesh, from, target);
	if (path.empty()) return false;
	float distance = 0;
	for (size_t i = 1; i < path.size(); ++i) {
		const ai::Vec3 step = path[i] - path[i - 1];
		distance += std::sqrt(step.x * step.x + step.y * step.y + step.z * step.z);
		if (distance > maxWalk) return false;
	}
	return true;
}
// The aim controller's output is a PIMouseLookX/Y value (inputs 4, 5).
// The soldier (BF2142.exe 0x0058a6d0, from 0x0058abc4) turns 5 degrees per
// unit on each call (*0x00a68e4c yaw, *0x00a68f24 pitch, both 5.0, set by
// static_init 0x008dfe00/0x008dfe20), without the time step: it runs once
// a 1/30 s tick, so a unit is 150 degrees a second. Under 0.001 counts as
// none (0x0091fdc0). Zoomed in, both are scaled by the field of view over
// the base one (0x00586420; the zoom's mouseSpeedMod is 1 everywhere):
// the bots' too (stepZoom). The pitch is clamped to +-85 degrees (*0x00a68eb0 + 4,
// 0x005890a0).
constexpr float kInputDegreesPerTick = 5.0f;
constexpr float kInputTicksPerSecond = 30.0f;
constexpr float kPitchLimit = 85.0f;
// The soldier camera's field of view (the camera template's default,
// 0x00610fd0), radians.
constexpr float kBaseFieldOfView = 1.1f;
// The soldier's ControlInfo plugin (SoldierCtrl): proportionalConstant -7,
// derivativeConstant -0.01, angleSpeedConstant 0.4.
constexpr float kProportional = -7.0f;
constexpr float kDerivative = -0.01f;
constexpr float kAngleSpeed = 0.4f;

float headingTo(const physics::Vec3& from, const physics::Vec3& to)
{
	return std::atan2(to.x - from.x, to.z - from.z) * kDegrees;
}

float angleBetween(float a, float b) { return std::remainder(b - a, 360.0f); }

// The squared distance across the ground (AIDLL 0x100a86f0 of two points).
float horizontal2(const physics::Vec3& d) { return d.x * d.x + d.z * d.z; }

std::string lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

ai::Vec3 toAi(const physics::Vec3& v) { return {v.x, v.y, v.z}; }
physics::Vec3 fromAi(const ai::Vec3& v) { return {v.x, v.y, v.z}; }

// The AI object's box (AIDLL 0x100fe990) of a vehicle: the farthest corner
// from its origin, from the box's centre, its width and length, and its
// centre (object space). Without collision points: 2.5 m and 2 x 4 m.
// AIDLL reads them through the unit's physical part (+0x40, vtable
// 0x101188d0) from BF2142.exe's AIObjectPhysical adaptor (0x009d7228): the
// box (slot 0x2c, the collision mesh's slot 0x54(1) 0x006e5c70 ->
// 0x006de820, LOD 1's +0x84), r (slot 0x38, 0x008a3f40 -> the mesh's slot
// 0x50(1) 0x006e5c40 -> 0x006de7d0, LOD 1's +0xc0) and the centre radius
// (slot 0x40, 0x008a4120: the box's +0x1c), all from 0x006aeda0.
struct VehicleBox {
	float radius = 2.5f, centreRadius = 2.5f, width = 2, length = 4;
	physics::Vec3 centre;
};

VehicleBox vehicleBox(const GameWorld::VehicleInfo& v)
{
	VehicleBox box;
	const physics::Vec3 lo = v.boxMin, hi = v.boxMax;
	if (hi.x <= lo.x && hi.z <= lo.z) {
		return box;
	}
	box.centre = (lo + hi) * 0.5f;
	box.radius = 0;
	box.centreRadius = 0;
	for (int i = 0; i < 8; ++i) {
		const physics::Vec3 corner {(i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z};
		box.radius = std::max(box.radius, std::sqrt(physics::lengthSquared(corner)));
		box.centreRadius = std::max(box.centreRadius, std::sqrt(physics::lengthSquared(corner - box.centre)));
	}
	box.width = hi.x - lo.x;
	box.length = hi.z - lo.z;
	return box;
}

bool contains(const std::string& text, const char* part) { return lower(text).find(part) != std::string::npos; }

float settingNumber(const world::ObjectTemplate* t, const char* key, float fallback)
{
	if (!t) {
		return fallback;
	}
	const auto it = t->settings().find(key);
	if (it == t->settings().end()) {
		return fallback;
	}
	char* end = nullptr;
	const float value = std::strtof(it->second.c_str(), &end);
	return end != it->second.c_str() ? value : fallback;
}

// A firearm's fire mode as it is picked up: the first fire.addFireRate,
// 2 (automatic) without one (the fire component's constructor 0x005f28a0).
int fireMode(const world::ObjectTemplate* t)
{
	if (!t) {
		return 2;
	}
	const auto it = t->settings().find("fire.addFireRate.list");
	if (it == t->settings().end()) {
		return 2;
	}
	return std::atoi(it->second.c_str());
}

// CombatStrengthType names (weaponTemplate.setStrength; the original's
// parser 0x10059be0, which also takes a bare "Infantery").
int strengthIndex(const std::string& name)
{
	static const char* names[] = {"infantry", "lightarmour", "heavyarmour", "navalarmour", "helicopter", "airplane", "titan",
		"missiles"};
	const std::string n = lower(name);
	for (int i = 0; i < 8; ++i) {
		if (n == names[i] || n == std::string("cst") + names[i]) {
			return i;
		}
	}
	return n == "cstinfantery" ? 0 : -1;
}

// AIBehaviours.ai's sets, for when the scripts were not run.
void addBehaviour(ai::VehicleSetup& setup, const char* name, const char* bb, const char* pattern, int priority,
	const char* curve, const char* inhibitors)
{
	setup.behaviours.push_back({name, bb, pattern, priority, curve, inhibitors});
}

const std::map<std::string, ai::VehicleSetup>& defaultVehicles()
{
	static const std::map<std::string, ai::VehicleSetup> sets = [] {
		std::map<std::string, ai::VehicleSetup> s;
		ai::VehicleSetup& infantry = s["Infantery"];
		infantry.modifiers = "StandardWeights";
		addBehaviour(infantry, "Avoid", "BBAvoid", "BBPAvoidCollisionInfantery", 1, "UCUnion", "AvoidInhibit");
		addBehaviour(infantry, "MoveTo", "BBMoveTo", "BBPGotoWaypointInfantery", 4, "UCUnion", "UnitWeights");
		addBehaviour(infantry, "Idle", "BBIdle", "BBPIdleInfantery", 5, "UCUnion", "UnitWeights");
		addBehaviour(infantry, "Fire", "BBFireInfantery", "BBPFireInfantery", 4, "UCFire", "UnitWeights");
		addBehaviour(infantry, "MedicAssist", "BBMedicAssist", "BBPMedicAssist", 10, "UCUnion", "UnitWeights");
		addBehaviour(infantry, "AmmoAssist", "BBAmmoAssist", "BBPAmmoAssist", 12, "UCUnion", "UnitWeights");
		addBehaviour(infantry, "RepairVehicle", "BBRepairVehicle", "BBPRepairVehicle", 10, "UCUnion", "UnitWeights");
		addBehaviour(infantry, "BridgeRepair", "BBBridgeRepair", "BBPBridgeRepair", 15, "UCUnion", "UnitWeights");
		addBehaviour(infantry, "c4", "BBC4", "BBPC4", 11, "UCUnion", "UnitWeights");
		addBehaviour(infantry, "DefuseMines", "BBDefuseMines", "BBPDefuseMines", 13, "UCUnion", "UnitWeights");
		addBehaviour(infantry, "Change", "BBChange", "BBPChange", 6, "UCUnion", "UnitWeights");
		addBehaviour(infantry, "Revive", "BBRevive", "BBPRevive", 8, "UCUnion", "UnitWeights");
		addBehaviour(infantry, "Flee", "BBFlee", "BBPFlee", 7, "UCUnion", "UnitWeights");
		ai::VehicleSetup& tank = s["Tank"];
		tank.modifiers = "StandardWeights";
		addBehaviour(tank, "Avoid", "BBAvoid", "BBPAvoidCollision2d", 1, "UCUnion", "AvoidInhibit");
		addBehaviour(tank, "MoveTo", "BBMoveTo", "BBPGotoWaypoint2d", 4, "UCUnion", "UnitWeights");
		addBehaviour(tank, "Idle", "BBIdle", "BBPIdle2d", 5, "UCUnion", "UnitWeights");
		addBehaviour(tank, "Fire", "BBFireInfantery", "BBPFireInfantery", 4, "UCFire", "UnitWeights");
		addBehaviour(tank, "Change", "BBChange", "BBPChange", 6, "UCUnion", "ChangeInhibit");
		ai::VehicleSetup& car = s["Car"];
		car.modifiers = "StandardWeights";
		addBehaviour(car, "Avoid", "BBAvoid", "BBPAvoidCollision2d", 1, "UCUnion", "AvoidInhibit");
		addBehaviour(car, "MoveTo", "BBMoveTo", "BBPGotoWaypoint2d", 4, "UCUnion", "UnitWeights");
		addBehaviour(car, "Idle", "BBIdle", "BBPIdle2d", 5, "UCUnion", "UnitWeights");
		addBehaviour(car, "Change", "BBChange", "BBPChange", 2, "UCUnion", "UnitWeights");
		addBehaviour(car, "RunOver", "BBRunOverSoldier", "BBPRunOverSoldier", 6, "UCUnion", "UnitWeights");
		ai::VehicleSetup& fixed = s["Fixed"];
		fixed.modifiers = "StandardWeights";
		addBehaviour(fixed, "Idle", "BBIdle", "BBPIdle2d", 5, "UCUnion", "UnitWeights");
		addBehaviour(fixed, "Fire", "BBFireInfantery", "BBPFireInfantery", 4, "UCFire", "UnitWeights");
		addBehaviour(fixed, "Change", "BBChange", "BBPChange", 6, "UCUnion", "UnitWeights");
		return s;
	}();
	return sets;
}

} // namespace

std::string BotSettings::moveInterpreter(const std::string& equipment) const
{
	// The interpreter's MoveTo statement (0x10029e10's table): the entry
	// ending in "MoveTo"; without the scripts, AIBehaviours.ai's.
	if (const auto it = interpreter.find(equipment); it != interpreter.end()) {
		for (const std::string& statement : it->second) {
			if (statement.size() > 6 && statement.compare(statement.size() - 6, 6, "MoveTo") == 0) {
				return statement;
			}
		}
	}
	static const std::map<std::string, std::string> shipped = {{"Infantery", "InfanteryMoveTo"}, {"Tank", "TankMoveTo"},
		{"FloatingTank", "FloatingTankMoveTo"}, {"Mech", "MechMoveTo"}, {"Car", "CarMoveTo"}, {"ArmedCar", "CarMoveTo"},
		{"Plane", "PlaneMoveTo"}, {"Helicopter", "HeliMoveTo"}, {"TransportHelicopter", "HeliMoveTo"},
		{"AirTransport", "AirTransportMoveTo"}};
	const auto it = shipped.find(equipment);
	return it != shipped.end() ? it->second : "TankMoveTo";
}

float BotSettings::modifier(const std::string& behaviour) const
{
	const auto set = modifiers.find(botModifiers);
	if (set == modifiers.end()) {
		return 1;
	}
	const auto it = set->second.find(behaviour);
	return it != set->second.end() ? it->second : 1;
}

const ai::VehicleSetup& BotSettings::vehicle(const std::string& type) const
{
	const auto it = vehicles.find(type);
	if (it != vehicles.end() && !it->second.behaviours.empty()) {
		return it->second;
	}
	const auto& defaults = defaultVehicles();
	const auto d = defaults.find(type);
	return d != defaults.end() ? d->second : defaults.at("Infantery");
}

// The weaponTemplate the scripts gave (Objects/**/ai/Weapons.ai). Every
// shipped weapon has one; for runs without those scripts (the tests) the
// kinds the bots handle specially get what the shipped files give them.
WeaponAi BotSettings::weapon(const std::string& name) const
{
	const std::string n = lower(name);
	const auto it = weapons.find(n);
	if (it != weapons.end()) {
		return it->second;
	}
	WeaponAi w;
	if (n.find("defibrillator") != std::string::npos) {
		w.type = WeaponAi::Revive;
		w.maxRange = 60; // Unl_Defibrillator
	} else if (n.find("drone_sentry_detonator") != std::string::npos) {
		w.type = WeaponAi::FireAndForget; // SentryDroneDetonator
		w.minRange = 20;
		w.maxRange = 100;
		w.optimal = 1;
		w.strength = {20, 0, 0, 0, 0, 0, 0, 0};
	} else if (n.find("knife") != std::string::npos) {
		w.type = WeaponAi::CloseCombat;
		w.maxRange = 2.5f;
		w.optimal = 0.25f;
		w.strength = {3, 0, 0, 0, 0, 0, 0, 0};
		w.deviation = 0;
	} else if (n.find("hub_medic") != std::string::npos || n.find("medic") != std::string::npos) {
		w.type = WeaponAi::Healing;
		w.maxRange = 70;
	} else if (n.find("hub_ammo") != std::string::npos || n.find("ammo") != std::string::npos) {
		w.type = WeaponAi::Ammo;
		w.maxRange = 70;
	} else if (n.find("c4") != std::string::npos && n.find("detonator") == std::string::npos) {
		w.type = WeaponAi::C4; // UNL_C4_AI
		w.maxRange = 30;
	} else if (n.find("repair") != std::string::npos) {
		w.type = WeaponAi::Repair;
		w.maxRange = 50;
	} else if (n.find("camo") != std::string::npos) {
		w.type = WeaponAi::Camouflage;
	} else if (n.find("grenade") != std::string::npos || n.find("c4") != std::string::npos
		|| n.find("mine") != std::string::npos || n.find("deployer") != std::string::npos
		|| n.find("drone") != std::string::npos || n.find("beacon") != std::string::npos
		|| n.find("shield") != std::string::npos) {
		// Thrown and placed gadgets: not scored without their template.
		w.type = WeaponAi::Camouflage;
	} else {
		// A rifle (eu_ar_rifle's template).
		w.maxRange = 72;
		w.optimal = 0.35f;
		w.firingPose = 2;
		w.strength = {5, 1, 0, 0, 0, 0, 0, 0};
	}
	return w;
}

const AiPlugin* BotSettings::plugin(const std::string& aiTemplate, const std::string& kind) const
{
	const auto t = aiTemplates.find(lower(aiTemplate));
	if (t == aiTemplates.end()) {
		return nullptr;
	}
	for (const std::string& name : t->second.plugins) {
		const auto p = aiPlugins.find(name);
		if (p != aiPlugins.end() && lower(p->second.kind) == lower(kind)) {
			return &p->second;
		}
	}
	return nullptr;
}

int playerInputIndex(const std::string& name)
{
	// io::PlayerInputMap's reader (0x0068fb90) takes the c_PI names; the
	// AI templates write them without "c_". Only the axes are used.
	static const char* const kAxes[] = {"PIYaw", "PIPitch", "PIRoll", "PIThrottle", "PIMouseLookX", "PIMouseLookY",
		"PICameraX", "PICameraY"};
	const std::string bare = name.rfind("c_", 0) == 0 ? name.substr(2) : name;
	for (int i = 0; i < 8; ++i) {
		if (lower(bare) == lower(kAxes[i])) {
			return i;
		}
	}
	return 0x40;
}

uint32_t informationTypeFlag(const std::string& name)
{
	static const std::pair<const char*, int> names[] = {{"ITUnit", 0}, {"ITAir", 1}, {"ITHelicopter", 2}, {"ITGround", 3},
		{"ITNaval", 4}, {"ITControlpoint", 5}, {"ITTransportation", 6}, {"ITMobile", 7}, {"ITStructure", 8},
		{"ITSoldier", 9}, {"ITObstructedView", 10}, {"ITNoTemperature", 11}, {"ITHasWreck", 12}, {"ITObstacle", 13},
		{"ITHasCarHorn", 14}};
	for (const auto& [n, bit] : names) {
		if (name == n) {
			return 1u << bit;
		}
	}
	return 0;
}

float soldierAiRadius()
{
	// Soldier_CollisionMesh's box (0x006e4880, the dummy built without a
	// file): x and z within +-0.4 (0xbecccccd / 0x3ecccccd), y from -1 (the
	// feet) to 0.8 (0x3f4ccccd); its radius the farthest corner (0x006aeda0).
	return std::sqrt(0.4f * 0.4f + 1.0f + 0.4f * 0.4f);
}

int kitTypeIndex(const std::string& name)
{
	static const std::pair<const char*, int> names[] = {{"Recon", 0}, {"Assault", 1}, {"Support", 2}, {"AT", 3},
		{"Engineer", 4}, {"Medic", 5}, {"Specops", 6}, {"Sniper", 7}, {"Extended1", 8}, {"Extended2", 9}};
	for (const auto& [n, index] : names) {
		if (name == n || name == std::string("KT") + n || name == std::to_string(index)) {
			return index;
		}
	}
	return -1;
}

// The radio message names (0x0088eaa0 builds the same name-to-type map,
// req_new_order 0xe, PLAYER_TACTICS_gogogo 0xb...).
int radioMessageType(const std::string& voiceName)
{
	static const std::pair<const char*, int> names[] = {{"bail_out", 0}, {"get_in", 1}, {"req_pickup", 2},
		{"medic", 3}, {"need_repair", 4}, {"roger_that", 6}, {"negative", 7}, {"PLAYER_CONFIRM_thankyou", 8},
		{"PLAYER_CONFIRM_sorry", 9}, {"need_ammo", 10}, {"PLAYER_TACTICS_gogogo", 11},
		{"PLAYER_TACTICS_followme", 12}, {"laserpaint", 13}, {"req_new_order", 14}, {"defend", 15},
		{"attack_position", 16}, {"move", 17}, {"COMMSL_COMRADIO_commend", 18}, {"COMMSL_COMRADIO_rebuke", 19},
		{"COMMSL_COMRADIO_followorders", 20}, {"COMMSL_COMRADIO_divide", 21}};
	for (const auto& [n, type] : names) {
		if (voiceName == n) {
			return type;
		}
	}
	return -1;
}

float BotSettings::radioModifierOf(int type) const
{
	const auto it = radioModifier.find(type);
	if (it != radioModifier.end()) {
		return it->second;
	}
	// AI/AIRadioMessages.ai: req_pickup 60, the rest 10.
	return type == 2 ? 60.0f : 10.0f;
}

float BotSettings::radioSendPeriodOf(int type) const
{
	const auto it = radioSendPeriod.find(type);
	if (it != radioSendPeriod.end()) {
		return it->second;
	}
	// AI/AIRadioMessages.ai's max send periods when the script was not run.
	switch (type) {
	case 2: return 0;
	case 16: case 6: case 7: case 8: case 9: return 2;
	case 1: case 3: case 4: case 10: return 20;
	default: return 10;
	}
}

float BotSettings::radioLifeOf(int type) const
{
	const auto it = radioLife.find(type);
	if (it != radioLife.end()) {
		return it->second;
	}
	// AI/AIRadioMessages.ai's receive times to live when the script was not run.
	switch (type) {
	case 14: case 15: case 17: return 1;
	case 1: case 3: case 4: case 8: case 10: return 20;
	default: return 10;
	}
}

// 0x100fa440: with D the distance across and k = -g D^2 / (2 v^2), the
// slopes tan a = (D +- sqrt(D^2 - 4 k (k + dy))) / (2 k); the steeper one.
bool lobDirection(const physics::Vec3& d, float speed, float gravity, physics::Vec3& direction)
{
	const float across = std::sqrt(d.x * d.x + d.z * d.z);
	if (speed <= 0 || across < 1e-4f || gravity >= 0) {
		return false;
	}
	const float k = -gravity * across * across / (2 * speed * speed);
	const float disc = across * across - 4 * k * (k + d.y);
	if (disc < 0) {
		return false;
	}
	const float a = std::max(std::atan((std::sqrt(disc) + across) / (2 * k)), std::atan((across - std::sqrt(disc)) / (2 * k)));
	direction = {std::cos(a) / across * d.x, std::sin(a), std::cos(a) / across * d.z};
	return true;
}

// 0x10074ef0's explosive bonus. The weapon (an explosion radius, not
// guided: the AI weapon's +0x6c, set by 0x00892be0 from the weapon, is 0
// for the hand weapons and guns here) against a soldier, from:
//   - a soldier bot: at 5..maxRange m (10 m when the target is no more than
//     3 m higher; maxRange + 1.25 dh when it is over 5 m higher and the
//     weapon isn't thrown), not within 10 s of the fire plan's condition
//     failing (+0x1f8), the target seen (flag 2) and either known only from
//     others (flag 8) or over 30 m higher (not for snipers and thrown
//     weapons), seen by anyone within 10 s (+0x58), and the bot's last
//     explosive round (the third last for an automatic weapon that isn't
//     thrown: vt+0xec(0 or 2), the stamps 0x1004e910 keeps) over 10 s ago;
//   - a bot in a vehicle: at minRange..maxRange m, not within 5 s of the
//     vehicle condition failing (+0x250), seen (flag 2) within 5 s.
// With the aim lobbing (+0x208) the range widens by 1 m each way.
bool explosivePick(const ExplosivePick& p)
{
	if (!p.targetSoldier || p.explosionRadius == 0) {
		return false;
	}
	float lo = p.soldierBot ? (p.heightAbove <= 3 ? 10.0f : 5.0f) : p.minRange;
	float hi = p.maxRange;
	if (p.heightAbove > 5 && !p.thrown) {
		hi += 1.25f * p.heightAbove;
	}
	if (p.lobbing) {
		lo -= 1;
		hi += 1;
	}
	if (p.distance < lo || p.distance > hi) {
		return false;
	}
	const bool high = !p.sniper && !p.thrown && p.heightAbove > 30;
	if (p.soldierBot) {
		if (p.now - p.conditionFailedAt < 10) {
			return false;
		}
	} else if (p.now - p.vehicleFailedAt < 5) {
		return false;
	}
	if (!p.seen || !(p.secondHand || high || !p.soldierBot)) {
		return false;
	}
	if (p.now - p.seenAt >= (p.soldierBot ? 10.0f : 5.0f)) {
		return false;
	}
	const int shot = !p.thrown && p.fireMode2 ? 2 : 0;
	return !p.soldierBot || p.now - p.explosiveShots[shot] > 10;
}

int guidanceKind(const world::ObjectTemplate* weapon)
{
	if (!weapon) {
		return 0;
	}
	const auto it = weapon->settings().find("target.targetSystem");
	if (it == weapon->settings().end()) {
		return 0;
	}
	static const char* names[] = {"tsnone", "tsheatseeking", "tslaserguided", "tswireguided", "tstvguided"};
	const std::string name = lower(it->second);
	for (int i = 0; i < 5; ++i) {
		if (name == names[i]) {
			return i; // 0x008923a0: 1..4 kept, anything else 0
		}
	}
	return 0;
}

// The weapon choice (0x10074ef0), for each weapon of the bot's object
// (its armament, vt+0x1c) in order:
//   - only a usable one (slot 0x1c) of type FireArm, C4, Revive,
//     CloseCombat, Sniper, FireAndForget or TitanWeapon; one that can't
//     fire through transparent (+0x4c) only at a target seen clear (the
//     memory item's flag 0x40) or known from others (8); C4 not while the
//     bot's own charge is out (+0x1b8; DAT_10139508, never written, is 0);
//   - CloseCombat and Revive ("melee"): only with the target on the bot's
//     mesh outside the vehicles round it (0x100947b0); "going in" (+local_6)
//     against a human soldier within 20 m from a soldier: the target
//     moving (its Mobile slot 0x78 over 0.5, or above 0 with a melee
//     weapon in the bot's hand and the target within 10 m), the target's
//     magazine empty or that hand-and-10 m case, the bot's own magazine
//     empty or a melee weapon in hand, and the target attending to the
//     bot or that melee in hand;
//   - none without rounds (slot 0xc below 1; -1 counts 0x10000) unless
//     Revive or going in;
//   - the score: the strength against the target's type, x0.5 against a
//     scripted bot (+0x264), x (3 - EMP level) for an EMP weapon (+0x4d,
//     0x10074c60); 0 drops it. A melee weapon soldier to soldier within
//     3 m, or going in, is taken at once when the mesh ray reaches the
//     target; a melee weapon otherwise is dropped;
//   - the explosive bonus (x100, +0x249): from a soldier, or from a
//     vehicle at a soldier with an unguided (+0x6c 0) explosive weapon,
//     against a soldier; a soldier's not while its +0x248 or the area
//     guard (strategicAreaGuard) holds, nor with the target inside that
//     area; then explosivePick;
//   - an empty magazine of an unguided weapon that reloads by itself in
//     more than 1.9 s: the score 0.1; wire guided (3) with a target
//     locked: x10; without the bonus, below minRange x (0.1 - 0.09 min(1,
//     (minRange - d) / 5)), beyond maxRange x (1 - 0.9 min(1, (d -
//     maxRange) / 5)); an empty magazine x0.33.
// The best score above 0 wins.
int chooseWeapon(const WeaponChoice& c, const std::vector<WeaponOption>& weapons, bool& explosive)
{
	explosive = false; // vt+0x260(0)
	float best = 0;
	int chosen = -1;
	const float d = c.distance;
	for (size_t i = 0; i < weapons.size(); ++i) {
		const WeaponOption& w = weapons[i];
		const bool allowed = w.type == WeaponAi::FireArm || w.type == WeaponAi::C4 || w.type == WeaponAi::Revive
			|| w.type == WeaponAi::CloseCombat || w.type == WeaponAi::Sniper || w.type == WeaponAi::FireAndForget
			|| w.type == WeaponAi::TitanWeapon;
		if (!w.usable || !allowed) {
			continue;
		}
		const bool melee = w.type == WeaponAi::CloseCombat || w.type == WeaponAi::Revive;
		const bool vehicleExplosive = !c.soldierBot && c.targetSoldier && w.guidance == 0 && w.explosionRadius != 0;
		if (!w.firesThroughTransparent && !c.clearSighting) {
			continue;
		}
		if (w.type == WeaponAi::C4 && (c.c4Placed || (c.targetMobile && std::fabs(c.targetAlongFacing) > 4))) {
			continue;
		}
		if (melee && c.soldierBot && c.targetMobile && !w.meleeReachable) {
			continue;
		}
		const bool handAndNear = c.inHandMelee && d < 10;
		const bool moving = c.targetMobile && (c.targetMotion > 0.5f || (handAndNear && c.targetMotion > 0));
		const bool goingIn = moving && melee && (c.targetInHandEmpty || handAndNear) && (c.inHandEmpty || c.inHandMelee)
			&& c.soldierBot && c.targetSoldier && c.targetHuman && d < 20 && (c.targetTargetsMe || c.inHandMelee);
		const int rounds = w.rounds == -1 ? 0x10000 : w.rounds;
		if (rounds < 1 && w.type != WeaponAi::Revive && !goingIn) {
			continue;
		}
		float score = w.strength;
		if (score == 0) {
			continue;
		}
		if (c.targetScripted) {
			score *= 0.5f;
		}
		if (w.emp) {
			score *= 3 - c.targetEmpLevel;
		}
		if (score == 0) {
			continue;
		}
		const bool close = c.soldierBot && c.targetSoldier && d < 3;
		if (melee) {
			if (!(close || goingIn)) {
				continue;
			}
			if (w.meleeRay) {
				return static_cast<int>(i);
			}
		}
		bool bonus = false;
		if ((c.soldierBot || vehicleExplosive) && c.targetSoldier && w.guidance == 0 && w.explosionRadius != 0) {
			const bool held = !vehicleExplosive && (c.orderMove || c.areaHold || c.targetInArea);
			if (!held) {
				ExplosivePick p = c.pick;
				p.soldierBot = c.soldierBot;
				p.targetSoldier = c.targetSoldier;
				p.explosionRadius = w.explosionRadius;
				p.thrown = w.thrown;
				p.sniper = w.type == WeaponAi::Sniper;
				p.fireMode2 = w.fireMode2;
				p.minRange = w.minRange;
				p.maxRange = w.maxRange;
				p.distance = d;
				bonus = explosivePick(p);
				if (bonus) {
					score *= 100;
					explosive = true; // vt+0x260(1)
				}
			}
		}
		if (w.inMag == 0 && w.reloadTime > 1.9f && w.autoReload && w.guidance == 0) {
			score = 0.1f;
		}
		if (w.guidance == 3 && w.locked) {
			score *= 10;
		}
		if (!bonus) {
			if (d < w.minRange) {
				score *= 0.100000024f - std::min(1.0f, (w.minRange - d) * 0.2f) * 0.09f;
			} else if (d > w.maxRange) {
				score *= 1 - std::min(1.0f, (d - w.maxRange) * 0.2f) * 0.9f;
			}
		}
		if (w.inMag == 0) {
			score *= 0.33f;
		}
		if (best < score) {
			best = score;
			chosen = static_cast<int>(i);
		}
	}
	return chosen;
}

void strategicAreaGuard(const std::vector<GuardArea>& areas, const physics::Vec3& bot, bool moving,
	const physics::Vec3& target, bool& hold, bool& targetInside)
{
	hold = false;
	targetInside = false;
	const GuardArea* area = nullptr;
	float best = -1;
	for (const GuardArea& a : areas) {
		if (a.held) {
			continue; // 0x10034a10 with its flag 0: only the areas not held
		}
		const float dx = bot.x - a.spotX, dz = bot.z - a.spotZ;
		const float dist = std::sqrt(dx * dx + dz * dz);
		const float value = dist < 1 ? a.radius : a.radius / (dist * dist);
		if (best < value) {
			best = value;
			area = &a;
		}
	}
	if (!area) {
		return;
	}
	const physics::Vec3 spot {area->spotX, area->centre.y, area->spotZ};
	const float r2 = area->radius * area->radius;
	if (physics::lengthSquared(spot - area->centre) < r2 && physics::lengthSquared(area->centre - bot) < 1600 && moving) {
		hold = true;
	}
	targetInside = physics::lengthSquared(target - spot) < r2;
}

float otherSeatFactor(bool driverTaken, bool rootMobile, uint32_t rootTypes)
{
	if (driverTaken || !rootMobile || (rootTypes & kITNaval)) {
		return 1;
	}
	return (rootTypes & kITGround) ? 0.77f : (rootTypes & kITAir) ? 0.5f : 1.0f;
}

float aiDeviationFactor(uint32_t botTypes, bool botPrimary, uint32_t targetTypes)
{
	if ((botTypes & kITAir) || !(targetTypes & kITAir) || botPrimary || (botTypes & kITNaval)) {
		return 1;
	}
	return 3;
}

bool BotSettings::primary(const std::string& aiTemplate) const
{
	const auto t = aiTemplates.find(lower(aiTemplate));
	return t == aiTemplates.end() || !t->second.secondary;
}

bool transportClose(bool own, float botToInfantrySpot2, float vehicleToVehicleSpot2, bool& noMove)
{
	if (own) {
		return vehicleToVehicleSpot2 < 25;
	}
	if (botToInfantrySpot2 >= 2500 && vehicleToVehicleSpot2 >= 2500
		&& (botToInfantrySpot2 >= 4900 || vehicleToVehicleSpot2 <= botToInfantrySpot2)) {
		noMove = false;
		return false;
	}
	noMove = true;
	return true;
}

uint32_t BotSettings::typeFlags(const std::string& aiTemplate) const
{
	const auto t = aiTemplates.find(lower(aiTemplate));
	return t == aiTemplates.end() ? 0 : t->second.types;
}

ai::ClimbLimits BotSettings::climbLimits(int map) const
{
	ai::ClimbLimits limits;
	if (map >= 0 && static_cast<size_t>(map) < pathfindingMaps.size()) {
		const PathfindingMap& m = pathfindingMaps[static_cast<size_t>(map)];
		// Stored as the cosine and read back through acos (0x100afdf0, 0x100afe00).
		limits.maxSlope = std::acos(std::clamp(std::cos(m.maxSlope * kRadians), -1.0f, 1.0f));
		limits.stepHeight = m.allowedThresholdHeight;
	}
	if (map == 0) {
		limits.stepHeight = 0.35f; // 0x1003f930, 0x1003f810: the infantry map's own
	}
	return limits;
}

// A carried weapon's AI template: its ObjectTemplate.aiTemplate's
// weaponTemplate (Unl_Hub_Medic -> MedicHub), else by its own name.
WeaponAi BotManager::weaponAi(const std::string& name) const
{
	const std::string ai = aiTemplateOf(name);
	if (!ai.empty() && m_settings.weapons.count(lower(ai))) {
		return m_settings.weapons.at(lower(ai));
	}
	return m_settings.weapon(name);
}

BotManager::BotManager(GameWorld& world, BotSettings settings) : m_world(world), m_settings(std::move(settings))
{
	// The AI's start on the level (AIDLL 0x10056b20): a view distance the
	// scripts did not set is the game's.
	if (m_settings.viewDistance <= 0) {
		m_settings.viewDistance = gameViewDistance(m_settings.levelFog, m_settings.levelViewDistance);
	}
}

namespace {
// The game clock's start (0x0065c020 takes QueryPerformanceCounter as the
// game starts): here the process's.
const std::chrono::steady_clock::time_point g_clockStart = std::chrono::steady_clock::now();
} // namespace

uint32_t clockSeedWord(double seconds)
{
	return static_cast<uint32_t>(static_cast<int64_t>(std::llround(seconds * 1000000.0)));
}

uint32_t botRandomSeed(bool fromClock)
{
	if (const char* seed = std::getenv("BF2142_BOT_SEED"); seed && *seed) {
		if (const uint32_t word = static_cast<uint32_t>(std::strtoul(seed, nullptr, 0)); word != 0) {
			return word;
		}
	}
	if (!fromClock) {
		return 0x2545f491u; // the tests' and --simulate's repeatable word
	}
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_clockStart).count();
	return clockSeedWord(seconds);
}

float gameViewDistance(const std::array<float, 4>& fog, float levelViewDistance)
{
	float t = 0;
	for (;;) {
		float density = fog[2] * t;
		if (fog[3] <= density) {
			density = t * t * t + fog[3];
		}
		if (density >= 0.8f) {
			break;
		}
		t += 0.01f;
		if (t >= 1.0f) {
			break;
		}
	}
	return std::min({(fog[1] - fog[0]) * t + fog[0], levelViewDistance, 300.0f});
}

void BotManager::setNavMesh(const ai::NavMesh* mesh)
{
	m_navMesh = mesh;
	m_noNavMesh = mesh == nullptr;
	// A bot's scattered spawn tries (BF2142.exe 0x005b2200 with its AI flag,
	// the player's slot 0x68) must also pass IAIPathfindingCourier slot 0xc
	// (*0x00b4bb94: AIDLL 0x10054ff0, the pathfinder's polygon lookup on the
	// map, 0x101394d4 slots 4 and 0x30): a point on the navigation mesh.
	if (mesh) {
		m_world.setAiSpawnTest([mesh](const physics::Vec3& at) { return mesh->findPolygon(toAi(at)) >= 0; });
	} else {
		m_world.setAiSpawnTest({});
	}
}

BotManager::~BotManager()
{
	if (m_navMesh) {
		m_world.setAiSpawnTest({});
	}
	if (m_source && m_source->manager == this) {
		m_source->manager = nullptr;
		m_source->random = m_settings.random; // the one stream goes on
	}
	if (!std::getenv("BF2142_BOT_STATS")) {
		return;
	}
	std::fprintf(stderr, "bot behaviours (seconds run, all bots):\n");
	for (const auto& [name, seconds] : m_behaviourSeconds) {
		std::fprintf(stderr, "  %-28s %8.1f\n", name.c_str(), seconds);
	}
	for (const auto& [vehicle, metres] : m_driven) {
		const auto name = m_drivenName.find(vehicle);
		std::fprintf(stderr, "  driven %-20s %6.0f m\n", name != m_drivenName.end() ? name->second.c_str() : "?", metres);
	}
}

// The bots' U[0, 1): AIDLL's twister (DAT_101394cc; genrand 0x10046770,
// U[0, 1) 0x10046a50). The callers that draw the word themselves
// (0x10046770 then x 2.3283064e-10) take the same number.
float BotManager::random()
{
	return m_settings.random.uniform();
}

float BotManager::seatValueFor(int player, uint32_t vehicle, int seat) const
{
	const auto it = m_bots.find(player);
	GameWorld::PlayerInfo self;
	if (it == m_bots.end() || !m_world.playerInfo(player, self)) {
		return 0;
	}
	return seatValue(it->second, self, vehicle, seat, vehicle != self.vehicle);
}

std::string BotManager::behaviour(int player) const
{
	const auto it = m_bots.find(player);
	return it != m_bots.end() ? it->second.behaviour : std::string();
}

SoldierInput BotManager::lastInputFor(int player) const
{
	const auto it = m_bots.find(player);
	return it != m_bots.end() ? it->second.lastInput : SoldierInput {};
}

int BotManager::target(int player) const
{
	const auto it = m_bots.find(player);
	return it != m_bots.end() ? it->second.target : -1;
}

float BotManager::preferredRange(int player) const
{
	const auto it = m_bots.find(player);
	return it != m_bots.end() ? it->second.preferredRange : 0.0f;
}

int BotManager::planSlot(int player) const
{
	const auto it = m_bots.find(player);
	return it != m_bots.end() && it->second.planTarget >= 0 ? it->second.planSlot : -1;
}

std::string BotManager::equipment(int player) const
{
	const auto it = m_bots.find(player);
	return it != m_bots.end() ? it->second.equipment : std::string();
}

bool BotManager::knows(int player, int enemy) const
{
	const auto it = m_bots.find(player);
	if (it == m_bots.end()) {
		return false;
	}
	for (const Known& k : it->second.known) {
		if (k.player == enemy && k.everSeen) {
			return true;
		}
	}
	return false;
}

std::vector<physics::Vec3> BotManager::route(int player) const
{
	std::vector<physics::Vec3> points;
	const auto it = m_bots.find(player);
	if (it == m_bots.end() || !it->second.hasGoal) {
		return points;
	}
	for (const Bot::Via& v : it->second.via) {
		points.push_back(v.position);
	}
	points.push_back(it->second.goal);
	return points;
}

std::string BotManager::strategy(int team) const { return team == 1 || team == 2 ? m_sides[team].strategy : std::string(); }

bool BotManager::honking(int player) const
{
	const auto it = m_bots.find(player);
	return it != m_bots.end() && it->second.hornHeld;
}

float BotManager::lastUrgency(int player, const std::string& behaviour) const
{
	const auto it = m_bots.find(player);
	if (it == m_bots.end()) {
		return 0;
	}
	const ai::VehicleSetup& setup = m_settings.vehicle(it->second.equipment.empty() ? "Infantery" : it->second.equipment);
	for (size_t i = 0; i < setup.behaviours.size() && i < it->second.arbitration.urgency.size(); ++i) {
		if (setup.behaviours[i].name == behaviour) {
			return it->second.arbitration.urgency[i];
		}
	}
	return 0;
}

bool BotManager::keepsToOrder(int player) const
{
	const auto it = m_bots.find(player);
	return it != m_bots.end() && it->second.orderMove;
}

int BotManager::zoomLevel(int player) const
{
	const auto it = m_bots.find(player);
	return it == m_bots.end() ? 0 : it->second.zoom.level();
}

bool BotManager::explosive(int player) const
{
	const auto it = m_bots.find(player);
	return it != m_bots.end() && it->second.explosive;
}

bool BotManager::holdingForLob(int player) const
{
	const auto it = m_bots.find(player);
	return it != m_bots.end() && it->second.lobHold;
}

// The game hands the AI a radio message per hearer (0x1004e140 ->
// AIDLL 0x10030b20): not one the hearer said itself (the player's vt+0xa8), and
// for bail_out, get_in and req_pickup (types 0..2) the AI acts on it at
// once (seat changes, a pick-up, not rewritten); every message goes to the
// hearer's comm object (vt+0xa4 -> 0x100a6fe0), filed under its type with
// its sender: a newer one from the same sender replaces the older. It is
// kept for its type's receive time to live (0x100a7130, the comm's +0x54).
void BotManager::radio(int sender, const std::string& voiceName, int receiver)
{
	const int type = radioMessageType(voiceName);
	GameWorld::PlayerInfo from;
	if (type < 0 || !m_world.playerInfo(sender, from)) {
		return;
	}
	for (auto& [id, bot] : m_bots) {
		GameWorld::PlayerInfo info;
		if (id == sender || (receiver >= 0 && id != receiver) || !m_world.playerInfo(id, info)
			|| (receiver < 0 && info.team != from.team)) {
			continue;
		}
		const auto same = std::find_if(bot.radio.begin(), bot.radio.end(),
			[&](const Bot::Radio& r) { return r.type == type && r.sender == sender; });
		if (same != bot.radio.end()) {
			same->at = m_time;
			same->modifier = m_settings.radioModifierOf(type);
			same->answered = false;
		} else {
			bot.radio.push_back({type, sender, m_time, m_settings.radioModifierOf(type)});
		}
	}
}

namespace {
// The voice of an AI radio type (the game's table at 0x0088f000 read back).
const char* radioVoice(int type)
{
	static const char* const voices[] = {"bail_out", "get_in", "req_pickup", "medic", "need_repair", nullptr,
		"roger_that", "negative", "PLAYER_CONFIRM_thankyou", "PLAYER_CONFIRM_sorry", "need_ammo",
		"PLAYER_TACTICS_gogogo", "PLAYER_TACTICS_followme", "laserpaint", "req_new_order", "defend",
		"attack_position", "move", "COMMSL_COMRADIO_commend", "COMMSL_COMRADIO_rebuke",
		"COMMSL_COMRADIO_followorders", "COMMSL_COMRADIO_divide"};
	return type >= 0 && type < static_cast<int>(sizeof(voices) / sizeof(voices[0])) ? voices[type] : nullptr;
}

// The AI's radio channels as the game's effector maps them (0x00896270):
// 0 TCLocalTeamMedic, 1 TCLocalGroundVehicle, 2 TCLocalAirVehicle, 3
// TCSquad, 4 TCCommander, 5 TCSquadLeader, 6 TCLocalTeam, 7 TCVehicle, 8
// TCPlayer; any other 0x13, which nobody hears.
int targetChannelOf(int aiChannel)
{
	static const int channels[] = {6, 7, 8, 3, 2, 4, 10, 11, 14};
	return aiChannel >= 0 && aiChannel < 9 ? channels[aiChannel] : 0x13;
}
} // namespace

// 0x100a7860: a send is queued only once its type's next time (+0x3c) has
// come; queuing sets it a send period on (+100).
void BotManager::queueRadio(Bot& bot, int type, int channel, float delay, int target)
{
	const auto next = bot.radioNext.find(type);
	if (next != bot.radioNext.end() && m_time < next->second) {
		return;
	}
	bot.radioQueue.push_back({type, channel, target, m_time + delay});
	std::stable_sort(bot.radioQueue.begin(), bot.radioQueue.end(),
		[](const RadioSent& a, const RadioSent& b) { return a.at < b.at; });
	bot.radioNext[type] = m_time + m_settings.radioSendPeriodOf(type);
}

// 0x100a57c0 (AIDLL 0x100a5990 sends at once): the type's index in the game's
// radio table (the effector's slot 0x18), then the game's send (0x00896270)
// on the channel's target channel, the target the message's value.
void BotManager::sendRadioNow(Bot& bot, const GameWorld::PlayerInfo& self, int type, int channel, int target)
{
	const char* voice = radioVoice(type);
	if (!voice) {
		return;
	}
	const int tc = targetChannelOf(channel);
	const int value = tc == 14 ? target : tc == 4 || tc == 5 ? self.squad : tc == 13 ? self.team : 0;
	m_world.radioMessage(self.id, tc, voice, value);
	bot.radioSent.push_back({type, channel, target, m_time});
	if (bot.radioSent.size() > 16) {
		bot.radioSent.erase(bot.radioSent.begin());
	}
}

bool BotManager::answerRadio(Bot& bot, int type)
{
	bool any = false;
	for (Bot::Radio& r : bot.radio) {
		if (r.type == type && !r.answered && m_time - r.at < m_settings.radioLifeOf(type)) {
			r.answered = true;
			any = true;
		}
	}
	return any;
}

// The comm's update (0x100a7af0, from the bot's update):
//  - the low-health call (0x100a7910): unless airborne (the bot's +0x140)
//    or in an ITAir vehicle moving over 3 m/s, with the bot's object
//    (its vehicle, else its soldier) between 0 and half its health (0.75 in
//    a vehicle) and its +0xfc clear: "medic" on the AI's channel 0
//    (TCLocalTeamMedic) on foot, "need_repair" on 6 (TCLocalTeam) aboard.
//    A squad member whose AI player's slot 0xf4 answers 1 holds it back;
//    neither that slot nor the setter of +0xfc (the game's AI event
//    0x1004e220, interface slot 18; Idle's driver wait clears it) is
//    traced, so neither holds a call back here;
//  - the queue, by send time: a send due goes out unless a message of its
//    type was heard within the type's send period (the first of its list:
//    another said it already), which holds the queue for later.
void BotManager::updateComm(Bot& bot, const GameWorld::PlayerInfo& self)
{
	{
		bool held = bot.heli.airborne;
		const GameWorld::VehicleInfo* vehicle = nullptr;
		for (const GameWorld::VehicleInfo& v : m_vehicles) {
			vehicle = v.object == self.vehicle ? &v : vehicle;
		}
		if (vehicle) {
			const world::Object* object = m_world.objects().find(vehicle->object);
			const std::string ai = object && object->objectTemplate() ? aiTemplateOf(object->objectTemplate()->name()) : std::string();
			held = held || ((m_settings.typeFlags(ai) & kITAir) && std::sqrt(physics::lengthSquared(vehicle->velocity)) > 3);
		}
		const float health = healthOf(self.vehicle ? self.vehicle : self.soldier);
		if (!held && health > 0 && health < (self.vehicle ? 0.75f : 0.5f)) {
			if (!self.vehicle) {
				queueRadio(bot, 3, 0, 0);
			} else {
				queueRadio(bot, 4, 6, 0, 0);
			}
		}
	}
	while (!bot.radioQueue.empty() && bot.radioQueue.front().at <= m_time) {
		const RadioSent due = bot.radioQueue.front();
		for (const Bot::Radio& r : bot.radio) {
			if (r.type == due.type && m_time - r.at < m_settings.radioSendPeriodOf(due.type)) {
				return;
			}
		}
		bot.radioQueue.erase(bot.radioQueue.begin());
		sendRadioNow(bot, self, due.type, due.channel, due.target);
	}
}

std::vector<BotManager::RadioSent> BotManager::radioSent(int player) const
{
	const auto it = m_bots.find(player);
	return it != m_bots.end() ? it->second.radioSent : std::vector<RadioSent> {};
}

const BotManager::Bot::Radio* BotManager::heardFromHuman(const Bot& bot, int type) const
{
	for (const Bot::Radio& r : bot.radio) {
		if (r.type != type || m_time - r.at >= m_settings.radioLifeOf(type)) {
			continue;
		}
		// The list's first entry only (**(list + 4)).
		GameWorld::PlayerInfo sender;
		const bool human = m_world.playerInfo(r.sender, sender) && !sender.ai && sender.alive && !sender.manDown;
		return human ? &r : nullptr;
	}
	return nullptr;
}

bool BotManager::heard(const Bot& bot, int type, int sender) const
{
	for (const Bot::Radio& r : bot.radio) {
		if (r.type == type && r.sender == sender && m_time - r.at < m_settings.radioLifeOf(type)) {
			return true;
		}
	}
	return false;
}

int BotManager::kitTypeOf(const GameWorld::PlayerInfo& player) const
{
	const world::Object* kit = player.kit ? m_world.objects().find(player.kit) : nullptr;
	if (!kit || !kit->objectTemplate()) {
		return -1;
	}
	const auto it = kit->objectTemplate()->settings().find("kitType");
	return it != kit->objectTemplate()->settings().end() ? kitTypeIndex(it->second) : -1;
}

float BotManager::healthOf(uint32_t object) const
{
	const float left = m_world.hitPoints(object);
	if (left < 0) {
		return 1;
	}
	const world::Object* o = m_world.objects().find(object);
	const float most = settingNumber(o ? o->objectTemplate() : nullptr, "armor.maxHitPoints", 100);
	return most > 0 ? std::clamp(left / most, 0.0f, 1.0f) : 1.0f;
}

// What a soldier carries by slot; without a kit, the weapon in hand.
std::vector<std::pair<int, std::string>> BotManager::carried(const GameWorld::PlayerInfo& self) const
{
	if (!self.carried.empty()) {
		return self.carried;
	}
	const world::Object* weapon = self.weapon ? m_world.objects().find(self.weapon) : nullptr;
	if (weapon && weapon->objectTemplate()) {
		return {{self.inHandSlot, weapon->objectTemplate()->name()}};
	}
	return {};
}

// The soldier's AI armament (the AI object's +0x34 list, read by the
// weapon choice 0x10074ef0 at 0x10075170): AIDLL makes a weapon object
// only for an item whose template's aiTemplate (ObjectTemplate.aiTemplate,
// BF2142.exe 0x0051bab3) names a weaponTemplate; the loop looks each up
// (0x10031d80) and skips one it has none for. Every kit carries the
// ParachuteLauncher (itemIndex 9, no aiTemplate: a GenericFireArm with no
// magazine limit and a 0 m/s "round"), which the bots must never pick.
// (The Armament part's AI weapon list: its constructor 0x100f09c0, rebuilt
// from the game's items through IInformationFactory slot 0x28, 0x100f09e0;
// the weapon in hand's AI weapon, 0x100f0950.)
std::vector<std::pair<int, std::string>> BotManager::armament(const GameWorld::PlayerInfo& self) const
{
	std::vector<std::pair<int, std::string>> items = carried(self);
	if (m_settings.weapons.empty()) {
		return items; // no AI scripts: everything counts (the tests)
	}
	items.erase(std::remove_if(items.begin(), items.end(),
					[this](const std::pair<int, std::string>& item) {
						const std::string ai = aiTemplateOf(item.second);
						return ai.empty() || !m_settings.weapons.count(lower(ai));
					}),
		items.end());
	return items;
}

std::vector<std::pair<int, std::string>> BotManager::seatArmament(const GameWorld::PlayerInfo& self) const
{
	std::vector<std::pair<int, std::string>> items;
	const auto seats = const_cast<GameWorld&>(m_world).vehicleSeats(self.vehicle);
	if (self.seat < 0 || static_cast<size_t>(self.seat) >= seats.size()) {
		return items;
	}
	const GameWorld::SeatLayout& seat = seats[static_cast<size_t>(self.seat)];
	if (m_settings.weapons.empty()) {
		if (!seat.weapon.empty()) {
			items.push_back({m_world.seatWeaponIndex(self.vehicle, self.seat), seat.weapon});
		}
		return items;
	}
	for (size_t i = 0; i < seat.weapons.size(); ++i) {
		const GameWorld::SeatWeapon& w = seat.weapons[i];
		if (w.group >= 0 && !w.podLauncher && !w.aiTemplate.empty() && m_settings.weapons.count(lower(w.aiTemplate))) {
			items.push_back({static_cast<int>(i), w.weapon});
		}
	}
	return items;
}

// The seat's weapons' rounds, as the AI weapons read them through the
// adaptor (BF2142.exe vtable 0x009d5be8: slot 0xc, all magazines' rounds,
// -1 without a limit; slot 0xe4, 0x00892580, the magazine's): the seat's
// firearms (GameWorld::seatFireArm), as the template gives them while one
// isn't made yet.
std::vector<GameWorld::WeaponAmmo> BotManager::seatAmmo(const GameWorld::PlayerInfo& self,
	const std::vector<std::pair<int, std::string>>& weapons) const
{
	std::vector<GameWorld::WeaponAmmo> list;
	for (const auto& [index, name] : weapons) {
		if (const FireArm* gun = m_world.seatFireArm(self.vehicle, self.seat, index)) {
			const FireArmSettings& settings = gun->settings();
			list.push_back({name, gun->roundsInMag() + gun->magsLeft() * std::max(settings.magSize, 0), settings.magSize,
				settings.nrOfMags, gun->roundsInMag()});
		} else if (const world::ObjectTemplate* t = m_world.templates().find(name)) {
			const FireArmSettings settings = FireArmSettings::fromTemplate(*t);
			list.push_back({name, settings.magSize * (settings.nrOfMags + 1), settings.magSize, settings.nrOfMags, settings.magSize});
		}
	}
	return list;
}

bool BotManager::drivesGun(const GameWorld::PlayerInfo& self) const
{
	return self.vehicle && self.driving && m_world.seatTurnsGun(self.vehicle, self.seat);
}

std::string BotManager::seatWeaponName(const GameWorld::PlayerInfo& self, int index) const
{
	const auto seats = const_cast<GameWorld&>(m_world).vehicleSeats(self.vehicle);
	if (self.seat < 0 || static_cast<size_t>(self.seat) >= seats.size()) {
		return {};
	}
	const GameWorld::SeatLayout& seat = seats[static_cast<size_t>(self.seat)];
	return index >= 0 && static_cast<size_t>(index) < seat.weapons.size() ? seat.weapons[static_cast<size_t>(index)].weapon
																		   : seat.weapon;
}

std::string BotManager::weaponName(const GameWorld::PlayerInfo& self, int slot) const
{
	for (const auto& [s, name] : carried(self)) {
		if (s == slot) {
			return name;
		}
	}
	return {};
}

float BotManager::projectileSpeed(const std::string& weapon) const
{
	const world::ObjectTemplate* t = m_world.templates().find(weapon);
	return t ? settingNumber(t, "velocity", FireArmSettings {}.velocity) : 0.0f;
}

// The equipment type (aiSettings.setVehicle) whose behaviours a bot runs:
// Infantery on foot; in a vehicle its seat's (the PlayerControlObject's
// aiTemplate, whose Unit plug-in names it: Car, Tank, FloatingTank, Fixed,
// ...). Every shipped vehicle has its AI templates; for runs without
// Objects/**/ai/Objects.ai (the tests) a driver is a Tank when its seat has
// a gun and a Car otherwise, and other seats are Fixed.
std::string BotManager::equipmentOf(const GameWorld::PlayerInfo& self) const
{
	if (!self.vehicle) {
		return "Infantery";
	}
	const std::vector<GameWorld::SeatLayout> seats = const_cast<GameWorld&>(m_world).vehicleSeats(self.vehicle);
	if (self.seat < 0 || static_cast<size_t>(self.seat) >= seats.size()) {
		return "Fixed";
	}
	const GameWorld::SeatLayout& seat = seats[static_cast<size_t>(self.seat)];
	if (const std::string ai = aiTemplateOf(seat.name); !ai.empty()) {
		if (const AiPlugin* unit = m_settings.plugin(ai, "Unit"); unit && !unit->equipmentType.empty()) {
			return unit->equipmentType;
		}
	}
	if (self.driving) {
		return seat.weapon.empty() ? "Car" : "Tank";
	}
	return "Fixed";
}

// What a player counts as for weapon strengths (CombatStrengthType): its
// vehicle's Physical plug-in (setStrType), Infantry on foot; for runs
// without the vehicles' Objects.ai (the tests; every shipped vehicle has
// one) LightArmour.
int BotManager::strengthTypeOf(const GameWorld::PlayerInfo& player) const
{
	if (!player.vehicle) {
		return 0;
	}
	const world::Object* vehicle = m_world.objects().find(player.vehicle);
	if (vehicle && vehicle->objectTemplate()) {
		const AiPlugin* physical = m_settings.plugin(aiTemplateOf(vehicle->objectTemplate()->name()), "Physical");
		if (physical && physical->strengthType >= 0) {
			return physical->strengthType;
		}
	}
	return 1;
}

// An object template's AI template (ObjectTemplate.aiTemplate), "" for none.
std::string BotManager::aiTemplateOf(const std::string& objectTemplate) const
{
	const world::ObjectTemplate* t = const_cast<GameWorld&>(m_world).templates().find(objectTemplate);
	if (!t) {
		return {};
	}
	const auto it = t->settings().find("aiTemplate");
	return it != t->settings().end() ? it->second : std::string();
}

// The bot manager's frame (BotManager::update 0x100ca590), with the
// original's time slicing. Its CPU budgets are real time (AIMain 0x100571c0
// sets the deadline by the IBFTimer, BF2142.exe 0x0089f380:
// QueryPerformanceCounter), so what fits depends on the machine and can't
// be reproduced; every slice runs to its count, as when no budget runs out:
//   - plan execution (0x100c91c0): about half the bots a frame, round
//     robin (N/2 + 1 and N/2 on alternate frames for an odd N). A bot
//     skipped while moving keeps its input for up to 15 more frames
//     but for the fire, reload and lie buttons, then stops (0x10098c00,
//     0x10097e60; see below).
//   - thinking (AIDLL 0x100c95f0): bots that executed gather time; one with a
//     plan thinks again after 0.5 s of it, one without after 0.3 s
//     (setPlannedDecisionMakingThreshold, setUnplannedDecisionMakingThreshold).
//     At most 2 planned and 2 + (2 - planned waiting) unplanned bots think
//     a frame, the longest waiting first (setDecisionMakingInterleave 2 2).
//   - sensing (0x100c9980): the two bots that executed this frame and
//     waited longest look about (0x100ed700). (The think and sense queues
//     are heaps by waiting time: 0x100c6310 / 0x100c61f0, 0x100c6370 /
//     0x100c6240.)
//   - the path followers' refresh (0x100c6cc0): N / 16 + 1 bots a frame,
//     round robin, gather the objects within 50 m and, a path followed
//     within the last 3 s, update it (0x10095ac0); here every bot each tick.
// Rounds flying past are noticed every frame (0x1004ed30 is an event).
void BotManager::update(float seconds)
{
	m_time += seconds;
	if (enabled()) {
		population(); // AIMain's kick and fill (BotsSquad.cpp), before the lists are taken
	}
	// What every bot looks at this tick, gathered once.
	m_players = m_world.playerList();
	m_vehicles = m_world.vehicleList();
	m_playerIndex.clear();
	for (size_t i = 0; i < m_players.size(); ++i) {
		m_playerIndex[m_players[i].id] = i;
	}
	m_vehicleTeams.clear();
	for (const GameWorld::PlayerInfo& p : m_players) {
		if (p.alive && p.vehicle) {
			m_vehicleTeams.emplace(p.vehicle, p.team);
		}
	}
	// What the bots heard (the game's hand-out, GameWorld::hearsRadio).
	for (const GameWorld::RadioHeard& heard : m_world.takeRadioHeard([this](int receiver) { return m_bots.count(receiver) != 0; })) {
		radio(heard.sender, heard.voice, heard.receiver);
	}
	for (const GameWorld::AiHit& hit : m_world.takeAiHits()) {
		hitBy(hit.victim, hit.attacker);
	}
	// A human player out of a vehicle stamps it for the team (BF2142.exe
	// 0x00894860, exit event: the player not a bot (vt+0x84 -> player
	// vt+0x68) and the soldier's root no longer the vehicle's -> the unit
	// component's vt+0x64 0x008a1b10 with the clock and the player's team).
	for (const GameWorld::PlayerInfo& player : m_players) {
		// The weapon in hand's stamp (0x008a6d80, on every change of the
		// armament's current weapon).
		InHand& inHand = m_inHand[player.id];
		const std::string weapon = player.alive ? weaponName(player, player.inHandSlot) : std::string();
		if (inHand.slot != player.inHandSlot || inHand.weapon != weapon) {
			inHand = {player.inHandSlot, weapon, m_time};
		}
		if (player.ai) {
			continue;
		}
		uint32_t& was = m_humanVehicle[player.id];
		if (was && player.vehicle != was && player.team >= 1 && player.team <= 2) {
			auto& stamp = m_humanLeftAt.try_emplace(was, std::array<float, 3> {-1000, -1000, -1000}).first->second;
			stamp[static_cast<size_t>(player.team)] = m_time;
		}
		was = player.vehicle;
	}
	if (!enabled()) {
		return;
	}
	updateStrategy(seconds);
	command(seconds);
	for (const GameWorld::PlayerInfo& p : m_players) {
		if (p.ai && !m_deactivated.count(p.id) && !m_refused.count(p.id) && !m_bots.count(p.id)) {
			// AIMain's slot 30 (0x100581a0): a new bot is refused once the
			// bot manager's bots less the deactivated ones (its slots 8 and
			// 0x74) reach aiSettings.setMaxNBots (+8); else activated
			// (0x100c8950) and handed to its side's SAI (slot 3).
			if (static_cast<int>(m_bots.size()) >= m_settings.maxBots) {
				m_refused.insert(p.id);
				continue;
			}
			m_bots.try_emplace(p.id);
		}
	}
	updateSquads();
	updateMines();
	const std::vector<GameWorld::PlayerInfo>& players = m_players;
	std::vector<const GameWorld::PlayerInfo*> bots;
	// The game's AI players are the bots (AIMain's IAI slots: a player made
	// a bot, 0x10056020 -> 0x100581a0 under setMaxNBots -> the bot manager's
	// 0x100c8950 "Activating bot"; one leaving, 0x10056d10; one changing
	// team moves between the SAIs, 0x10056090; ai.deactivateBot, AIDLL 0x10055f80;
	// both take the bot out of its squad, 0x10089860, and the game's squad
	// with it: the player's own squad membership here).
	for (const GameWorld::PlayerInfo& self : players) {
		if (!self.ai || m_deactivated.count(self.id) || m_refused.count(self.id)) {
			continue;
		}
		Bot& bot = m_bots[self.id];
		if (!self.alive) {
			// Man down: the game's event (3, 6) reaches IAIEventCourier
			// slot 0x34 (0x1004e620 -> the bot manager's slot 15,
			// 0x100c5ea0: the bot's +0x178 down flag, its path follower
			// reset 0x100962a0); kept here for the revive below.
			if (self.manDown) {
				m_downBots.insert(self.id);
			} else {
				m_downBots.erase(self.id);
			}
			m_teamKillsSeen[self.id] = self.teamKills;
			bot = Bot();
			bot.spawnedAt = -1;
			m_world.setZoomed(self.id, false);
			m_goals.erase(self.id); // a scripted goal ends with the bot's life (the rewrite's choice)
			// The SAI's choice of spawn point and kit (step 4 0x100a1a80 ->
			// 0x100f0590), once a death (BotsSquad.cpp).
			chooseRespawn(self);
			continue;
		}
		if (bot.spawnedAt < 0) {
			bot.spawnedAt = m_time;
			m_respawn[self.id].chosen = false; // the spawn hook (0x1009fc90)
			if (m_downBots.erase(self.id)) {
				// Revived: the soldier's event 0x19 (BF2142.exe 0x00587b90)
				// raises the game's event (3, 7) with the soldier and its
				// player (0x00894860 -> IAIEventCourier slot 0x38,
				// 0x1004e130 -> the bot manager's slot 16, 0x100c5f00): the
				// down flag off (+0x178), a think now (vt+0x24), the path
				// follower reset (0x10095120; the rewrite's bot starts
				// afresh), "thankyou" (8) on channel 6 in 1 s; the SAI's
				// slot 15 (0x100a0f40) has the squad member's goal given
				// again (0x100bf4e0(0)) and its area record redone
				// (0x1009fdc0, the SAI's own bookkeeping).
				queueRadio(bot, 8, 6, 1.0f);
				for (auto& [key, squad] : m_squads) {
					if (SquadMember* m = memberOf(squad, self.id)) {
						m->goalSet = false;
						m->regoal = true;
					}
				}
			}
		}
		// A team kill: BF2142.exe's kill (0x005b7680) raises the game's
		// event (3, 0xb) with the killer and the victim of its team
		// (0x00894860 -> IAIEventCourier slot 0xa8, 0x1004e3e0 -> the bot
		// manager's slot 24, 0x100c60c0): a killer bot that isn't
		// scripted (vt+0x27c, +0x264; none is) says "sorry" (9) on
		// channel 6 at once. GameWorld counts the same kills (teamKills).
		int& teamKills = m_teamKillsSeen.try_emplace(self.id, self.teamKills).first->second;
		if (self.teamKills > teamKills) {
			queueRadio(bot, 9, 6, 0.0f);
		}
		teamKills = self.teamKills;
		noticeRounds(bot, self);
		warnOfGrenades(bot, self);
		// The bot's explosive rounds (0x1004e910, the game's event for each
		// round a player fires): with the weapon in hand (on foot, or the
		// seat's gun) having an explosion radius (and not guided, +0x6c 0),
		// the clock goes into the bot's last three stamps (vt+0xe8,
		// 0x10097370: +0x1e0 newest, +0x1e8, +0x1f0). The rounds that left
		// in the last world tick are the ones aged one tick.
		for (const Projectile& round : m_world.projectiles()) {
			if (round.player != self.id || round.age <= 0 || round.age > seconds * 1.01f + 1e-6f) {
				continue;
			}
			bot.firedAt = m_time; // +0xc0 (slot 0x314, 0x10097a60): hearing waits 1.5 s
			std::string weapon = weaponName(self, self.inHandSlot);
			if (self.vehicle) {
				const auto seats = m_world.vehicleSeats(self.vehicle);
				weapon = self.seat >= 0 && static_cast<size_t>(self.seat) < seats.size()
					? seats[static_cast<size_t>(self.seat)].weapon
					: std::string();
			}
			if (!weapon.empty() && weaponAi(weapon).explosionRadius > 0 && bot.explosiveShots[0] != m_time) {
				bot.explosiveShots[2] = bot.explosiveShots[1];
				bot.explosiveShots[1] = bot.explosiveShots[0];
				bot.explosiveShots[0] = m_time;
			}
		}
		bot.radio.erase(std::remove_if(bot.radio.begin(), bot.radio.end(),
							[&](const Bot::Radio& r) { return m_time - r.at >= m_settings.radioLifeOf(r.type); }),
			bot.radio.end());
		// The comm's tick (0x100a8000, from the bot manager 0x100c6e00): the
		// expiry (0x100a7130) every tick, its update (0x100a7af0) at most once
		// a second there, every tick here. (The comm object: constructor
		// 0x100a8140, its 22 types' settings from the radio table; destructor
		// 0x100a7f70.)
		updateComm(bot, self);
		bots.push_back(&self);
	}
	if (bots.empty()) {
		return;
	}
	hear();
	// Plan execution, round robin.
	const size_t n = bots.size();
	m_oddFrame = !m_oddFrame;
	size_t count = std::min(n, n / 2 + 1);
	if (!m_oddFrame && n % 2 == 1 && count > 0) {
		--count;
	}
	std::vector<const GameWorld::PlayerInfo*> executed;
	for (size_t i = 0; i < n; ++i) {
		const GameWorld::PlayerInfo& self = *bots[(m_cursor + i) % n];
		Bot& bot = m_bots[self.id];
		if (i < count) {
			executed.push_back(&self);
		} else {
			// Skipped this frame (0x10098c00): while it moves (and for 15
			// frames at most) the last input stays but for PIFire,
			// PIAltFire, PIReload and PILie; otherwise 0x10097e60 clears the
			// movement, the look and the buttons, and a driver brakes
			// (PIThrottle -clamp(0.5 v.forward, +-0.4)). The rewrite's
			// presses (use, item, seat) act once per tick, so they are not
			// repeated (the original's held inputs make no new press).
			++bot.skips;
			SoldierInput input = bot.lastInput;
			input.fire = input.altFire = false;
			input.use = false;
			input.selectItem = 0;
			input.switchSeat = 0;
			physics::Vec3 velocity = self.velocity;
			const GameWorld::VehicleInfo* vehicle = nullptr;
			for (const GameWorld::VehicleInfo& v : m_vehicles) {
				vehicle = v.object == self.vehicle && self.driving ? &v : vehicle;
			}
			if (vehicle) {
				velocity = vehicle->velocity;
			}
			// A soldier's PIMouseLookX turns it 5 degrees a unit each tick
			// (BF2142.exe 0x0058abdd) and stays held over a skipped frame while
			// it moves (0x10098c00): the original's unit covers both ticks. The
			// executed frame's turn here is already the whole elapsed time's
			// (aim), so held again it went twice as far, past the target, and
			// back the next frame: the look swung while walking. On foot the
			// skipped frame turns no more (a vehicle's turn is its steering).
			if (!self.vehicle) {
				input.turn = 0;
			}
			if (bot.skips >= 16 || physics::lengthSquared(velocity) <= 0.01f) {
				input.throttle = vehicle ? -std::clamp(0.5f * physics::dot(vehicle->velocity, vehicle->forward), -0.4f, 0.4f) : 0.0f;
				input.strafe = 0;
				input.turn = 0;
				input.sprint = false;
			}
			m_world.setInput(self.id, input);
		}
	}
	m_cursor = (m_cursor + count) % n;
	for (const GameWorld::PlayerInfo* self : executed) {
		Bot& bot = m_bots[self->id];
		bot.skips = 0;
		const float elapsed = bot.lastAct < 0 ? seconds : m_time - bot.lastAct;
		bot.lastAct = m_time;
		bot.sinceThink += elapsed;
		bot.thinkAge += 3;
		bot.senseAge += 3;
		SoldierInput input;
		// Titan mode: no strategy of AIDLL's knows the Titans; a bot that
		// spawned aboard its Titan goes to the nearest pod array (its AI
		// template titan_pod_array_1_ai makes it a vehicle to the AI) and
		// drops in a pod, riding it down without steering.
		physics::Vec3 podEntry;
		float podRadius = 0;
		if (m_world.ridingPod(self->id)) {
			bot.lastInput = input;
			m_world.setInput(self->id, input);
			continue;
		}
		// Its Titan's guns (the gun hubs are seats of equipment type Fixed in
		// their AI templates, which AIDLL's seat choice covers, but no
		// strategy of its sends a bot into a Titan): while fewer than half
		// its Titan's guns are manned or claimed, a bot aboard claims the
		// nearest free working gun no other bot claimed, walks straight to
		// its console and takes it; the Titan isn't among the bots' vehicles,
		// so it stays and runs the Fixed behaviours there. The rest drop in
		// pods.
		int gunSeat = bot.titanGun;
		bool toGun = false;
		if (!self->vehicle && m_world.titanOf(self->team)) {
			std::vector<int> claimed;
			for (const auto& [id, other] : m_bots) {
				const GameWorld::PlayerInfo* mate = nullptr;
				for (const GameWorld::PlayerInfo& p : m_players) {
					mate = p.id == id ? &p : mate;
				}
				if (id != self->id && other.titanGun >= 0 && mate && mate->team == self->team && mate->alive && !mate->vehicle) {
					claimed.push_back(other.titanGun);
				}
			}
			const std::vector<int> gunners = m_world.vehicleOccupants(m_world.titanOf(self->team));
			const long manned = std::count_if(gunners.begin(), gunners.end(), [](int p) { return p >= 0; });
			const long guns = static_cast<long>(m_world.vehicleSeats(m_world.titanOf(self->team)).size());
			if (gunSeat >= 0) {
				toGun = m_world.titanGunEntry(self->id, podEntry, podRadius, gunSeat, claimed);
			} else if ((manned + static_cast<long>(claimed.size())) * 2 < guns) {
				toGun = m_world.titanGunEntry(self->id, podEntry, podRadius, gunSeat, claimed);
			}
		}
		bot.titanGun = toGun ? gunSeat : -1;
		if (toGun) {
			const physics::Vec3 d = podEntry - self->position;
			const float want = std::atan2(d.x, d.z) * 57.29578f;
			input.turn = std::clamp(std::remainder(want - self->yaw, 360.0f), -360 * elapsed, 360 * elapsed);
			input.throttle = std::fabs(std::remainder(want - self->yaw, 360.0f)) < 45 ? 1.0f : 0.0f;
			input.use = physics::lengthSquared(d) < podRadius * podRadius * 0.5f && !bot.usedLast;
			bot.usedLast = input.use;
			bot.lastInput = input;
			m_world.setInput(self->id, input);
			continue;
		}
		if (m_world.titanPodEntry(self->id, podEntry, podRadius)) {
			const physics::Vec3 d = podEntry - self->position;
			const float want = std::atan2(d.x, d.z) * 57.29578f;
			input.turn = std::clamp(std::remainder(want - self->yaw, 360.0f), -360 * elapsed, 360 * elapsed);
			input.throttle = std::fabs(std::remainder(want - self->yaw, 360.0f)) < 45 ? 1.0f : 0.0f;
			input.use = horizontal2(d) < podRadius * podRadius && !bot.usedLast;
			bot.usedLast = input.use;
			bot.lastInput = input;
			m_world.setInput(self->id, input);
			continue;
		}
		updateGoal(bot, *self);
		act(bot, *self, input, elapsed);
		handleWeapon(bot, *self, input);
		stepZoom(bot, *self, elapsed);
		m_behaviourSeconds[bot.behaviour + (self->vehicle ? (self->driving ? " (driving)" : " (seated)") : "")] += elapsed;
		if (self->vehicle && self->driving) {
			m_driven[self->vehicle] += std::sqrt(horizontal2(self->velocity)) * elapsed;
			if (const world::Object* o = m_world.objects().find(self->vehicle); o && o->objectTemplate()) {
				m_drivenName[self->vehicle] = o->objectTemplate()->name();
			}
		}
		bot.usedLast = input.use;
		bot.switchedLast = input.switchSeat > 0;
		bot.firedLast = input.fire;
		bot.reloadedLast = input.reload;
		bot.lastInput = input;
		m_world.setInput(self->id, input);
	}
	// Thinking.
	std::vector<std::pair<int, int>> planned, unplanned; // (age, player)
	for (const GameWorld::PlayerInfo* self : executed) {
		Bot& bot = m_bots[self->id];
		const bool hasPlan = bot.arbitration.running >= 0 && !bot.arbitration.replan;
		if (hasPlan && bot.sinceThink > m_settings.plannedThreshold) {
			planned.push_back({bot.thinkAge, self->id});
		} else if (!hasPlan && bot.sinceThink > m_settings.unplannedThreshold) {
			unplanned.push_back({bot.thinkAge, self->id});
		}
	}
	const auto oldestFirst = [](const std::pair<int, int>& a, const std::pair<int, int>& b) { return a.first > b.first; };
	std::stable_sort(planned.begin(), planned.end(), oldestFirst);
	std::stable_sort(unplanned.begin(), unplanned.end(), oldestFirst);
	std::vector<int> thinkers;
	const int plannedLimit = m_settings.plannedInterleave;
	const size_t unplannedLimit = static_cast<size_t>(
		std::max(0, plannedLimit - static_cast<int>(planned.size())) + m_settings.unplannedInterleave);
	for (size_t i = 0; i < unplanned.size() && i < unplannedLimit; ++i) {
		thinkers.push_back(unplanned[i].second);
	}
	for (size_t i = 0; i < planned.size() && i < static_cast<size_t>(std::max(0, plannedLimit)); ++i) {
		thinkers.push_back(planned[i].second);
	}
	for (int id : thinkers) {
		Bot& bot = m_bots[id];
		for (const GameWorld::PlayerInfo* self : executed) {
			if (self->id == id) {
				think(bot, *self, players);
			}
		}
		bot.sinceThink = 0;
		bot.thinkAge = 0;
	}
	// Sensing: two a frame.
	std::vector<std::pair<int, const GameWorld::PlayerInfo*>> senses;
	for (const GameWorld::PlayerInfo* self : executed) {
		senses.push_back({m_bots[self->id].senseAge, self});
	}
	std::stable_sort(senses.begin(), senses.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	for (size_t i = 0; i < senses.size() && i < 2; ++i) {
		Bot& bot = m_bots[senses[i].second->id];
		sense(bot, *senses[i].second, players);
		bot.senseAge = 0;
	}
}

// A player losing hit points to another's attack (IAICollisionHandler slot
// 0x18, 0x1004f020, from BF2142.exe's AI event adaptor 0x0089f8d0). Of
// another team (the AI objects' slot 0x40), the attacker goes to the
// victim's squad's memory (0x10089b70 -> 0x100c1ce0, as the fire plan's
// share) and, when the victim rides a vehicle whose Unit has a driver
// (0x100aa4e0) who is no bot (the player's slot 0x84), to that driver's
// squad's too. A bot victim (vt+0xc4) is made aware of the attacker as a
// round passing it is: the attacker's AI object stamped (its slot 0xc
// with the clock) and the round's event record 0x100c4500 (from the
// attacker to the hit, weight 1) handed to the bot's vt+0x158. The
// round's own test 0x1004eca0 (an attacker that is the round itself) has
// no counterpart: the rewrite's attacker is always a player.
void BotManager::hitBy(int victim, int attacker)
{
	GameWorld::PlayerInfo self, from;
	if (!m_world.playerInfo(victim, self) || !m_world.playerInfo(attacker, from) || from.team == self.team) {
		return;
	}
	shareTarget(self, from);
	if (self.vehicle) {
		for (const GameWorld::PlayerInfo& p : m_players) {
			if (p.vehicle == self.vehicle && p.driving && !p.ai && p.id != self.id && p.alive) {
				shareTarget(p, from);
			}
		}
	}
	const auto bot = m_bots.find(victim);
	if (bot == m_bots.end()) {
		return;
	}
	auto it = std::find_if(bot->second.known.begin(), bot->second.known.end(), [&](const Known& k) { return k.player == attacker; });
	if (it == bot->second.known.end()) {
		bot->second.known.push_back({});
		it = bot->second.known.end() - 1;
		it->player = attacker;
	}
	it->alertAt = m_time;
}

// Rounds passing a bot (0x1004ed30; the round's event record 0x100c4500).
// The game's events reach it through IAICollisionHandler (vtable 0x101147d8,
// constructor 0x1004e7e0, destructor 0x1004e8e0; slots 8 and 0xc,
// 0x1004efa0 / 0x1004ef80).
void BotManager::noticeRounds(Bot& bot, const GameWorld::PlayerInfo& self)
{
	// Rounds passing within 10 m of a bot make it aware of the shooter for
	// 5 s in every direction (0x1004ed30; the shooter->impact line, here the
	// line flown so far).
	const physics::Vec3 chestSelf = self.position + physics::Vec3 {0, kChestHeight, 0};
	for (const Projectile& round : m_world.projectiles()) {
		if (round.player < 0 || round.player == self.id || round.team == self.team) {
			continue;
		}
		GameWorld::PlayerInfo shooter;
		if (!m_world.playerInfo(round.player, shooter) || shooter.team == self.team) {
			continue;
		}
		const physics::Vec3 line = round.position - round.origin;
		const float length2 = physics::lengthSquared(line);
		float t = length2 > 0 ? physics::dot(chestSelf - round.origin, line) / length2 : 0;
		t = std::clamp(t, 0.0f, 1.0f);
		const physics::Vec3 nearest = round.origin + line * t;
		if (physics::lengthSquared(chestSelf - nearest) < 10.0f * 10.0f) {
			auto it = std::find_if(bot.known.begin(), bot.known.end(), [&](const Known& k) { return k.player == round.player; });
			if (it == bot.known.end()) {
				bot.known.push_back({});
				it = bot.known.end() - 1;
				it->player = round.player;
			}
			it->alertAt = m_time;
			// The round's record for the sensing agent (0x100c4500 on the
			// perception's list +0x6c): the shooter, the time, the impact.
			auto seen = std::find_if(bot.roundsSeen.begin(), bot.roundsSeen.end(),
				[&](const Bot::RoundSeen& r) { return r.shooter == round.player; });
			if (seen == bot.roundsSeen.end()) {
				bot.roundsSeen.push_back({round.player, m_time, round.position, false});
			} else {
				seen->at = m_time;
				seen->impact = round.position;
			}
		}
	}

}

// The grenade warning (AIDLL 0x1004eab0, called for every bot by the same
// event as the rounds above, 0x1004ed30): a round from a weapon whose AI
// template is thrown (+2), has an explosion radius (+0x20) and doesn't
// explode on impact (+0x4e) - the hand grenades - within that radius of
// the bot, with the round or its thrower in front (the direction to the
// round, or the unnormalised offset to the thrower, over 0.4 along the
// bot's facing, its matrix +0x20), is stored in the bot's path follower
// (0x10092920): the round's position and the thrower's weapon's stamp
// (AI weapon slot 0x100: when it was taken in hand, 0x008a6d80). No team
// test: a teammate's grenade (or the bot's own) warns too. It lasts until
// 2.8 s after that stamp (0x10092960); BBAvoid turns it into 1000.
// BF2142.exe's sender of the event wasn't found; like the rounds above it
// is checked every tick on the rounds in flight, with the thrower's weapon
// in hand as the round's weapon.
void BotManager::warnOfGrenades(Bot& bot, const GameWorld::PlayerInfo& self)
{
	if (self.vehicle) {
		return;
	}
	const physics::Vec3 forward {std::sin(self.yaw * kRadians), 0, std::cos(self.yaw * kRadians)};
	for (const Projectile& round : m_world.projectiles()) {
		if (round.player < 0) {
			continue;
		}
		GameWorld::PlayerInfo thrower;
		if (!m_world.playerInfo(round.player, thrower)) {
			continue;
		}
		const auto held = m_inHand.find(round.player);
		if (held == m_inHand.end() || held->second.weapon.empty()) {
			continue;
		}
		const WeaponAi w = weaponAi(held->second.weapon);
		if (!w.thrown || w.explosionRadius <= 0 || w.explodesOnImpact) {
			continue;
		}
		physics::Vec3 to = round.position - self.position;
		const float d = std::sqrt(physics::lengthSquared(to));
		if (d >= w.explosionRadius || d <= 1e-5f) {
			continue;
		}
		to = to * (1 / d);
		const physics::Vec3 toThrower = thrower.position - self.position;
		if (physics::dot(to, forward) <= 0.4f && physics::dot(toThrower, forward) <= 0.4f) {
			continue;
		}
		bot.grenadeAt = round.position;
		bot.grenadeStamp = held->second.since;
	}
}

namespace {
// An object template's radio.spottedMessage (its quotes taken off), "" for
// none: the game's slot 0xd0 (0x0042e9d0) asks the spotted object's Radio
// component.
std::string spottedVoiceOf(const world::ObjectTemplate* t)
{
	if (!t) {
		return {};
	}
	const auto it = t->settings().find("radio.spottedMessage");
	if (it == t->settings().end()) {
		return {};
	}
	std::string name = it->second;
	if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
		name = name.substr(1, name.size() - 2);
	}
	return name;
}
} // namespace

// The bots' spotting. The memory pass (0x100ed2c0) hands an item's first
// own sighting to the comm object (0x100a5850), whose report 0x10030fb0
// (the bot's player, the sensed AI object, the bot's team) throttles it
// against the table DAT_101394d0:
// - the object's kind is its AI info's strength type (vt+0x40: Infantry 0,
//   LightArmour 1, HeavyArmour 2, Helicopter 4, AirPlane 5);
// - a player whose id is 256 or more (the player's slot 0x6c, +0x3d1:
//   autonomous objects) spots without the throttle; a bot never;
// - else the strategic areas are walked in order (0x10034640) for one
//   whose position (0x1005d1c0) lies within 30 m across (100 m for kinds 4
//   and 5) of the object. An aircraft whose last aircraft spot (+0x38) is
//   over 60 s old is spotted and stamps it. Otherwise the team's entry for
//   the area spots when its time is over 60 s old (taking the kind and
//   the time), or upgrades Infantry or LightArmour to HeavyArmour (kind 2
//   and the time); failing both the next area in range is tried. No area
//   in range, no spot.
// The spot is IAIGame slot 0x68 (BF2142.exe 0x00895ab0): the game's slot
// 0xd4 with type 0x1b reports the object to the team's minimap (the
// human's spotting path, reportBattlefieldObjects), then the spotted
// object's voice (slot 0xd0, "spotted" without one) is said on TCTeam
// (slot 0x54, channel 1).
bool BotManager::spotSighting(const GameWorld::PlayerInfo& self, const GameWorld::PlayerInfo& other)
{
	if (self.team < 1 || self.team > 2) {
		return false;
	}
	const int kind = strengthTypeOf(other);
	const bool air = kind == 4 || kind == 5;
	const float range2 = air ? 10000.0f : 900.0f;
	const auto& areas = m_settings.strategy.areas;
	auto& entries = m_spots[static_cast<size_t>(self.team - 1)];
	if (entries.size() < areas.size()) {
		entries.resize(areas.size()); // the reset 0x10031560: {8, -1000}
	}
	bool spot = false;
	for (size_t i = 0; i < areas.size() && !spot; ++i) {
		physics::Vec3 at;
		float radius = 0;
		int owner = 0;
		if (!areaPosition(static_cast<int>(i), at, radius, owner)) {
			if (!areas[i].box) {
				continue;
			}
			at = {(areas[i].boxMin[0] + areas[i].boxMax[0]) * 0.5f, areas[i].height,
				(areas[i].boxMin[1] + areas[i].boxMax[1]) * 0.5f};
		}
		if (horizontal2(other.position - at) >= range2) {
			continue;
		}
		if (air && m_time - m_airSpotAt > 60) {
			m_airSpotAt = m_time;
			spot = true;
			break;
		}
		SpotEntry& entry = entries[i];
		if (m_time - entry.at > 60) {
			entry.kind = kind;
			entry.at = m_time;
			spot = true;
		} else if ((entry.kind == 0 || entry.kind == 1) && kind == 2) {
			entry.kind = 2;
			entry.at = m_time;
			spot = true;
		}
	}
	if (!spot) {
		return false;
	}
	m_world.reportBattlefieldObjects(self.id, {other.id});
	const auto templateOf = [this](uint32_t id) -> const world::ObjectTemplate* {
		const world::Object* o = id ? m_world.objects().find(id) : nullptr;
		return o ? o->objectTemplate() : nullptr;
	};
	// The sensed AI object: the vehicle for a rider, else the soldier (its
	// kit's Radio component, else its own).
	std::string voice = other.vehicle ? spottedVoiceOf(templateOf(other.vehicle)) : spottedVoiceOf(templateOf(other.kit));
	if (voice.empty() && !other.vehicle) {
		voice = spottedVoiceOf(templateOf(other.soldier));
	}
	m_world.radioMessage(self.id, 1, voice.empty() ? "spotted" : voice);
	return true;
}

// Sensing (the soldier's sensing agent, AIDLL 0x100ed700 and 0x100ee430;
// its step 0x100ef370 runs the re-hearing, the threat and the memory pass;
// its view frustum 0x100feea0 with the corners 0x100fed10 / 0x100feba0, an
// object's sphere tested against its six planes by 0x100feac0, 0x100f7340).
void BotManager::sense(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players)
{
	// The view: R = viewDistance, candidates within R cos(fov/2) (a square
	// frustum of half-angle fov/2), seen when one of
	// clamp(round(30 radius / d), 1, 10) rays is clear.
	float gunYaw = 0, gunPitch = 0;
	const bool gunning = self.vehicle && (!self.driving || drivesGun(self)) && m_world.gunAim(self.vehicle, self.seat, gunYaw, gunPitch);
	const float lookYaw = gunning ? self.yaw + gunYaw : self.yaw;
	if (!self.vehicle || turretLooks(self)) {
		faceSectors(bot, lookYaw); // 0x100d6d60 (bot vt+0x308, from this pass)
	}
	const float lookPitch = gunning ? gunPitch : bot.pitch;
	const physics::Vec3 eye = self.position + physics::Vec3 {0, kEyeHeight - 0.1f, 0};
	const float halfFov = m_settings.fieldOfView * 0.5f;
	const float reach = m_settings.viewDistance * std::cos(halfFov * kRadians);
	for (const GameWorld::PlayerInfo& other : players) {
		if (other.team == self.team || other.id == self.id) {
			continue;
		}
		auto it = std::find_if(bot.known.begin(), bot.known.end(), [&](const Known& k) { return k.player == other.id; });
		// Dead, downed or gone: dropped at once.
		if (!other.alive) {
			if (it != bot.known.end()) {
				bot.known.erase(it);
			}
			continue;
		}
		const physics::Vec3 chest = other.position + physics::Vec3 {0, other.vehicle ? 1.0f : kChestHeight, 0};
		const physics::Vec3 d = chest - eye;
		const float distance = std::sqrt(physics::lengthSquared(d));
		bool visible = distance < reach;
		if (visible && other.cloaked && distance > m_settings.discoverCloakedDistance) {
			visible = false;
		}
		if (visible) {
			const bool alerted = it != bot.known.end() && m_time - it->alertAt < 5;
			const bool close = distance < 2 * kSoldierRadius + 3 * kSoldierRadius;
			const float yawOff = std::fabs(angleBetween(lookYaw, headingTo(eye, chest)));
			const float pitchOff = std::fabs(std::atan2(d.y, std::sqrt(horizontal2(d))) * kDegrees - lookPitch);
			visible = alerted || close || (yawOff < halfFov && pitchOff < halfFov);
		}
		bool seenThrough = false;
		if (visible) {
			// A soldier's rays all go to its physics node (kChestHeight): one
			// tells. Others' after the first to points about the object.
			const int rays = other.vehicle
				? std::clamp(static_cast<int>(std::lround(30 * kSoldierRadius / std::max(distance, 0.01f))), 1, 10)
				: 1;
			// The ray (IAIObjectEnvironment slot 0x44, BF2142.exe 0x00897df0)
			// passes see-through materials, telling whether it went
			// through one (its last argument, handed to 0x100ed2c0).
			bool clear = false;
			for (int ray = 0; ray < rays && !clear; ++ray) {
				physics::Vec3 point = chest;
				if (ray > 0 && other.vehicle) {
					point = other.position
						+ physics::Vec3 {(random() * 2 - 1) * 0.3f, 0.2f + random() * 1.5f, (random() * 2 - 1) * 0.3f};
				}
				clear = m_world.lineOfSight(eye, point, self.vehicle, seenThrough);
			}
			visible = clear;
		}
		if (visible) {
			if (it == bot.known.end()) {
				bot.known.push_back({});
				it = bot.known.end() - 1;
				it->player = other.id;
			}
			it->position = other.position;
			it->seen = m_time;
			it->seenAt = m_time;
			it->lostAt = -1;
			it->everSeen = true;
			it->secondHand = false; // seen by the bot itself: flag 8 cleared (0x100ed2c0)
			// Flag 0x40: set by a clear sighting; one through a see-through
			// material leaves a new item without it (0x100ed2c0) and clears
			// a kept one's (0x100ee430).
			it->seenClear = !seenThrough;
			// The item's first own sighting is offered to the spotting
			// report once (+0x68; the bot not scripted, vt+0x27c's +0x264).
			if (!it->spotted) {
				it->spotted = true;
				spotSighting(self, other);
			}
			// Sightings go to the others in the same vehicle (0x100a8ce0;
			// 0x100ed2c0 with "not the seer": a new memory item there gets
			// flag 8, an old one keeps its flags).
			if (self.vehicle) {
				for (auto& [id, mate] : m_bots) {
					if (id == self.id) {
						continue;
					}
					GameWorld::PlayerInfo info;
					if (m_world.playerInfo(id, info) && info.vehicle == self.vehicle) {
						auto m = std::find_if(mate.known.begin(), mate.known.end(), [&](const Known& k) { return k.player == other.id; });
						if (m == mate.known.end()) {
							mate.known.push_back(*it);
							mate.known.back().secondHand = true;
							mate.known.back().spotted = false; // a new item there
						} else {
							m->position = other.position;
							m->seen = m_time;
							m->seenAt = m_time;
							m->lostAt = -1;
							m->everSeen = true;
							m->seenClear = m->seenClear || !seenThrough; // 0x100ed2c0 only sets it
						}
					}
				}
			}
		} else if (it != bot.known.end() && it->everSeen) {
			if (it->lostAt < 0) {
				it->lostAt = m_time;
			}
			// Forgotten after 60 s out of sight; a target in a vehicle (not a
			// soldier) at once (0x100ee430).
			if (m_time - it->lostAt > 60 || other.vehicle) {
				bot.known.erase(it);
			}
		} else if (it != bot.known.end() && m_time - it->alertAt > 30) {
			bot.known.erase(it); // an alert never followed by a sighting
		}
	}
	// Enemy vehicles (their root AI objects, which the agent's query
	// returns with the soldiers: 0x100ed700 case 0): one is seen when
	// anyone aboard is, or else by the same view and rays (clamp(round(30 r
	// / d), 1, 10), r its AI radius, the first to its centre and the rest
	// to points about it, slot 0x50). Out of sight it is kept 60 s
	// (0x100ee430; the side's sensor count falls 2 s after).
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		const int owner = vehicleTeam(v.object);
		if (owner == self.team || owner < 1 || owner > 2 || v.object == self.vehicle) {
			continue;
		}
		bool visible = false;
		for (const Known& k : bot.known) {
			const GameWorld::PlayerInfo* aboard = k.lostAt < 0 && k.seen == m_time ? playerOf(k.player) : nullptr;
			visible = visible || (aboard && aboard->vehicle == v.object);
		}
		// The rays (lineOfSight leaves out one body: the target's) from a
		// bot on foot; one aboard a vehicle sees others' only by those aboard.
		if (!visible && !self.vehicle) {
			const physics::Vec3 centre = v.position + physics::Vec3 {0, 1.0f, 0};
			const physics::Vec3 d = centre - eye;
			const float distance = std::sqrt(physics::lengthSquared(d));
			const float r = vehicleBox(v).radius;
			if (distance < reach) {
				const float yawOff = std::fabs(angleBetween(lookYaw, headingTo(eye, centre)));
				const float pitchOff = std::fabs(std::atan2(d.y, std::sqrt(horizontal2(d))) * kDegrees - lookPitch);
				const bool close = distance < 2 * r + 3 * kSoldierRadius;
				if (close || (yawOff < halfFov && pitchOff < halfFov)) {
					const int rays = std::clamp(static_cast<int>(std::lround(30 * r / std::max(distance, 0.01f))), 1, 10);
					for (int ray = 0; ray < rays && !visible; ++ray) {
						physics::Vec3 point = centre;
						if (ray > 0) {
							point = v.position
								+ physics::Vec3 {(random() * 2 - 1) * r * 0.5f, 0.2f + random() * r * 0.5f, (random() * 2 - 1) * r * 0.5f};
						}
						visible = m_world.lineOfSight(eye, point, v.object);
					}
				}
			}
		}
		auto it = bot.knownVehicles.find(v.object);
		if (visible) {
			KnownVehicle& k = bot.knownVehicles[v.object];
			k.seen = m_time;
			k.lostAt = -1;
		} else if (it != bot.knownVehicles.end() && it->second.seen > -1e8f) {
			if (it->second.lostAt < 0) {
				it->second.lostAt = m_time;
			}
			if (m_time - it->second.lostAt > 60) {
				it->second.seen = -1e9f;
				it->second.lostAt = -1;
			}
		}
	}
}

// The goal a bot walks to (MoveTo's waypoint): its squad's order, else
// for a bot outside squads the area the SAI attacks or defends nearest to
// it, else the nearest control point the team does not hold.
// For an area the team does not hold the goal is a spot within 0.8 of
// the control point's radius (the bot stays where it is when already
// inside); otherwise a spot round the order position
// (order + U * W * 0.8 - 0.5 W on each axis, as the original's
// asymmetric 0x10063460; W = twice the area's radius), up to 20 tries for
// one on the mesh. (A squad member's goal from its squad's order when it
// isn't in the formation: 0x100c0a90 re-goals it with the waypoint goal
// 0x100c0130 with its reachability test 0x100c1300 and
// up to 8 tries within 30 m 0x100c14a0, the order's area 0x100c1210; the
// squad's accessors 0x100c0e40, 0x100c0e50, 0x100c0ea0.)
void BotManager::updateGoal(Bot& bot, const GameWorld::PlayerInfo& self)
{
	ensureAreas();
	// A scripted goal (ai.botMoveTo and the rest) takes the place of the
	// SAI's orders: its head is the goal, with no areas on the way. A
	// waypoint goal that is done gives the bot back to the SAI.
	if (const auto scripted = m_goals.find(self.id); scripted != m_goals.end()) {
		ai::Vec3 head;
		if (!scripted->second.done && ai::goalPoint(scripted->second, head)) {
			const physics::Vec3 at {head.x, head.y, head.z};
			if (!bot.hasGoal || bot.goalOrder != -1 || horizontal2(bot.goal - at) > 0.01f) {
				bot.goalAt = m_time;
				bot.goalReached = false;
			}
			bot.goal = bot.orderPosition = at;
			bot.vehicleApproach = false;
			bot.goalOrder = -1;
			bot.goalMap = self.vehicle && self.driving ? 1 : 0;
			bot.via.clear();
			bot.hasGoal = true;
			return;
		}
		m_goals.erase(scripted);
		bot.hasGoal = false;
	}
	// The squad's goal for a member in its formation (0x100c0890 ->
	// 0x100bff90): its slot, with no areas on the way.
	if (bot.squadGoal) {
		ai::Vec3 head;
		if (ai::goalPoint(bot.formation, head)) {
			const physics::Vec3 at {head.x, head.y, head.z};
			if (!bot.hasGoal || bot.goalOrder != -1 || horizontal2(bot.goal - at) > 0.01f) {
				bot.goalAt = m_time;
				bot.goalReached = false;
			}
			bot.goal = bot.orderPosition = at;
			bot.vehicleApproach = false;
			bot.goalOrder = -1;
			bot.goalMap = 0;
			bot.via.clear();
			bot.hasGoal = true;
			return;
		}
	}
	const std::vector<GameWorld::ControlPointInfo> points = m_world.controlPointList();
	const auto& areas = m_settings.strategy.areas;
	physics::Vec3 orderAt;
	bool hasOrder = false;
	bool titanSiloOrder = false;
	// AIDLL's shipped strategic areas do not describe gpm_ti objectives.
	// Its generic fallback sees the two moving Titan control points alongside
	// the five silos; the Titan points cannot change hands, and bots can spend
	// their whole round treating them as capture objectives. Give on-foot bots
	// and ground-vehicle drivers a silo goal directly, using the mode's actual
	// silo/control-point state.
	// Existing Titan gun and pod behavior runs before updateGoal and is left
	// alone. Ground-vehicle drivers can use the same objective with their
	// vehicle mesh and the area's vehicle order position; passengers keep
	// their normal seat behavior.
	if (m_world.gameMode() == "gpm_ti" && self.team >= 1 && self.team <= 2 && (!self.vehicle || self.driving)) {
		const std::vector<GameWorld::SiloInfo> silos = m_world.missileSilos();
		const auto actionable = [&](const GameWorld::SiloInfo& silo) -> const GameWorld::ControlPointInfo* {
			if (silo.team == self.team) {
				return nullptr;
			}
			for (const GameWorld::ControlPointInfo& point : points) {
				if (point.id == silo.controlPoint && point.capturable) {
					return &point;
				}
			}
			return nullptr;
		};
		const GameWorld::ControlPointInfo* targetPoint = nullptr;
		// Keep an unfinished silo assignment stable. Once that silo changes to
		// our team, the next update selects another live target.
		if (bot.goalOrder >= 0 && static_cast<size_t>(bot.goalOrder) < areas.size()) {
			const int current = areas[static_cast<size_t>(bot.goalOrder)].controlPoint;
			for (const GameWorld::SiloInfo& silo : silos) {
				if (silo.controlPoint == current) {
					if (const GameWorld::ControlPointInfo* point = actionable(silo)) {
						targetPoint = point;
					}
					break;
				}
			}
		}
		if (!targetPoint) {
			float best = std::numeric_limits<float>::infinity();
			for (const GameWorld::SiloInfo& silo : silos) {
				const GameWorld::ControlPointInfo* point = actionable(silo);
				if (!point) {
					continue;
				}
				int assigned = 0;
				for (const auto& [id, other] : m_bots) {
					if (id == self.id || other.goalOrder < 0 || static_cast<size_t>(other.goalOrder) >= areas.size()) {
						continue;
					}
					const GameWorld::PlayerInfo* mate = playerOf(id);
					if (mate && mate->team == self.team && areas[static_cast<size_t>(other.goalOrder)].controlPoint == silo.controlPoint) {
						++assigned;
					}
				}
				// Spread new orders over available silos without taking a bot
				// away from its existing capture. Neutral silos need one flag
				// rise; enemy silos need a neutralize and capture, so favor the
				// former when travel distances are otherwise comparable.
				const float preference = point->team == 0 ? 0.0f : 900.0f;
				// A silo off the navigation mesh (Suez's vanilla mesh has none near silos 901, 905 and 906) cannot be
				// planned to: the bot then walks straight at it and stops at the first wall. Prefer reachable ones.
				const ai::NavMesh* reach = self.vehicle && self.driving ? m_vehicleNavMesh : m_navMesh;
				const float unreachable = reach && reach->findPolygon(toAi(point->position), 5.0f) < 0 ? 9.0e6f : 0.0f;
				const float score = physics::lengthSquared(point->position - self.position) +
					static_cast<float>(assigned) * 250000.0f + preference + unreachable;
				if (score < best) {
					best = score;
					targetPoint = point;
				}
			}
		}
		if (targetPoint) {
			orderAt = targetPoint->position;
			hasOrder = true;
			titanSiloOrder = true;
		}
	}
	// A driver heads for the areas' vehicle order positions over the
	// vehicle mesh.
	const int map = self.vehicle && self.driving ? 1 : 0;
	// A squad member heads for its squad's order under way (squad +0x2c,
	// state 2: 0x100c0130); without one it keeps the goal it had, or, with
	// none, goes as a bot outside squads would.
	const Squad* squad = self.squad ? this->squad(self.team, self.squad) : nullptr;
	if (titanSiloOrder) {
		// The mode-specific target above supersedes a generic SAI squad order.
	} else if (squad && squad->state == 2 && squad->kind != 4) {
		orderAt = squad->orderAt;
		hasOrder = true;
	} else if (squad && squad->state != 2 && bot.hasGoal && bot.goalMap == map && !bot.orderRegoal) {
		return;
	} else if (self.team == 1 || self.team == 2) {
		const Side& side = m_sides[self.team];
		float best = 0;
		// The role steps 10 and 11 gave it (updateStrategy): its list
		// first, both when that one is empty.
		const auto role = side.lonerAttacks.find(self.id);
		const std::vector<int>* first = role == side.lonerAttacks.end() || role->second ? &side.attacks : &side.defences;
		const std::vector<int>* second = first == &side.attacks ? &side.defences : &side.attacks;
		const bool both = role == side.lonerAttacks.end() || first->empty();
		for (const std::vector<int>* list : {first, second}) {
			if (list == second && !both) {
				continue;
			}
			for (int index : *list) {
				const ai::StrategicArea& area = areas[static_cast<size_t>(index)];
				for (const GameWorld::ControlPointInfo& point : points) {
					const float d = physics::lengthSquared(point.position - self.position);
					if (point.id == area.controlPoint && (!hasOrder || d < best)) {
						best = d;
						const int m = area.hasOrderPosition[map] ? map : 0;
						orderAt = area.hasOrderPosition[m]
							? physics::Vec3 {area.orderPosition[m][0], area.orderPosition[m][1], area.orderPosition[m][2]}
							: point.position;
						hasOrder = true;
					}
				}
			}
		}
		if (!hasOrder) {
			for (int pass = 0; pass < 2 && !hasOrder; ++pass) {
				for (const GameWorld::ControlPointInfo& point : points) {
					if ((pass == 0) == (point.team == self.team)) {
						continue;
					}
					const float d = physics::lengthSquared(point.position - self.position);
					if (!hasOrder || d < best) {
						best = d;
						orderAt = point.position;
						hasOrder = true;
					}
				}
			}
		}
	}
	if (!hasOrder) {
		bot.hasGoal = false;
		return;
	}
	// The area the order lies in.
	const GameWorld::ControlPointInfo* point = nullptr;
	int areaIndex = -1;
	for (const GameWorld::ControlPointInfo& p : points) {
		const float r = std::max(p.radius, 10.0f);
		if (horizontal2(p.position - orderAt) <= r * r) {
			point = &p;
			areaIndex = areaOfControlPoint(p.id);
		}
	}
	for (size_t i = 0; i < areas.size() && !point; ++i) {
		for (int m = 0; m <= 1 && !point; ++m) {
			if (!areas[i].hasOrderPosition[m]) continue;
			const physics::Vec3 at {areas[i].orderPosition[m][0], areas[i].orderPosition[m][1], areas[i].orderPosition[m][2]};
			if (horizontal2(at - orderAt) < 1) {
				for (const GameWorld::ControlPointInfo& p : points) {
					if (p.id == areas[i].controlPoint) {
						point = &p;
						areaIndex = static_cast<int>(i);
					}
				}
			}
		}
	}
	// A failed bounded approach search temporarily rejects that objective.
	// Prefer another enemy/neutral point; if none exists, try a friendly one.
	bool retryRejected = false;
	if (map == 1) {
		for (auto it = bot.rejectedVehiclePoints.begin(); it != bot.rejectedVehiclePoints.end();) {
			if (m_time >= it->second) {
				retryRejected = retryRejected || (point && point->id == it->first);
				it = bot.rejectedVehiclePoints.erase(it);
			} else ++it;
		}
	}
	if (map == 1 && point && bot.rejectedVehiclePoints.count(point->id)) {
		const GameWorld::ControlPointInfo* alternative = nullptr;
		physics::Vec3 start = self.position;
		for (const auto& vehicle : m_vehicles) if (vehicle.object == self.vehicle) start = vehicle.position;
		ai::PathOptions vehicleOptions;
		vehicleOptions.vehicle = true;
		for (int pass = 0; pass < 2 && !alternative; ++pass) {
			float best = std::numeric_limits<float>::infinity();
			for (const auto& p : points) {
				if (bot.rejectedVehiclePoints.count(p.id) || (pass == 0 && p.team == self.team) || !m_vehicleNavMesh
					|| ai::findPath(*m_vehicleNavMesh, toAi(start), toAi(p.position), vehicleOptions).empty()) continue;
				const float d = horizontal2(p.position - self.position);
				if (d < best) { best = d; alternative = &p; }
			}
		}
		if (alternative) {
			point = alternative;
			areaIndex = areaOfControlPoint(point->id);
			orderAt = point->position;
		} else {
			bot.orderRegoal = false;
			return; // stopped until this objective's retry deadline
		}
	}
	if (map == 1 && areaIndex >= 0 && areas[static_cast<size_t>(areaIndex)].hasOrderPosition[1]) {
		const auto& o = areas[static_cast<size_t>(areaIndex)].orderPosition[1];
		orderAt = {o[0], o[1], o[2]};
	}
	const bool held = point && point->team == self.team;
	const bool changed = !bot.hasGoal || horizontal2(bot.orderPosition - orderAt) > 1 || bot.goalOrder != areaIndex
		|| bot.goalMap != map;
	// Re-ordered once the goal is reached and 35 s (20 s for the squad
	// leader) have passed (0x1009e7a0).
	const float again = self.squadLeader ? 20.0f : 35.0f;
	const bool stale = bot.hasGoal && bot.goalReached && m_time - bot.goalAt > again;
	const bool regoal = bot.orderRegoal || retryRejected;
	bot.orderRegoal = false;
	if (!changed && !stale && !regoal) {
		return;
	}
	bot.orderPosition = orderAt;
	bot.vehicleApproach = false;
	bot.goalMap = map;
	bot.via.clear();
	if (areaIndex >= 0 && bot.goalOrder != areaIndex) {
		bot.lastArea = bot.goalOrder;
	}
	bot.goalOrder = areaIndex;
	bot.goalControlPoint = point ? point->id : -1;
	bot.goalAt = m_time;
	bot.goalReached = false;
	bot.hasGoal = true;
	if (!point) {
		bot.goal = orderAt; // an order off the areas: its point (and radius)
		return;
	}
	const ai::NavMesh* mesh = map == 1 ? m_vehicleNavMesh : m_navMesh;
	const auto valid = [mesh](const physics::Vec3& at) { return !mesh || mesh->findPolygon({at.x, at.y, at.z}) >= 0; };
	routeVia(bot, self, areaIndex, map);
	if (!held) {
		const float r = point->radius * 0.8f;
		if (horizontal2(self.position - point->position) <= r * r) {
			bot.goal = self.position;
			return;
		}
		bot.goal = point->position;
		bool found = false;
		for (int attempt = 0; attempt < 20; ++attempt) {
			const float angle = random() * 6.2831853f;
			const float distance = std::sqrt(random()) * r;
			const physics::Vec3 at = point->position + physics::Vec3 {std::sin(angle) * distance, 0, std::cos(angle) * distance};
			if (valid(at)) {
				bot.goal = at;
				found = true;
				break;
			}
		}
		if (found && map == 1 && mesh) {
			for (const auto& v : m_vehicles) {
				if (v.object == self.vehicle) {
					ai::PathOptions options;
					options.vehicle = true;
					found = !ai::findPath(*mesh, toAi(v.position), toAi(bot.goal), options).empty();
					break;
				}
			}
		}
		if (!found) {
			// Retail's failed search asks the area for the map's order
			// position (AIDLL 0x100604f6). If that is absent or off the
			// vehicle mesh, find a reachable approach and go on foot.
			bot.goal = orderAt;
			const world::Object* driven = self.vehicle ? m_world.objects().find(self.vehicle) : nullptr;
			const std::string rootAi = driven && driven->objectTemplate() ? aiTemplateOf(driven->objectTemplate()->name()) : "";
			const bool groundVehicle = !(m_settings.typeFlags(rootAi) & (kITAir | kITHelicopter));
			if (map == 1 && mesh && groundVehicle) {
				ai::FollowerSettings settings;
				settings.soldier = false;
				settings.options.vehicle = true;
				settings.climb = m_settings.climbLimits(1);
				const GameWorld::VehicleInfo* vehicle = nullptr;
				for (const GameWorld::VehicleInfo& v : m_vehicles) {
					if (v.object == self.vehicle) vehicle = &v;
				}
				if (!vehicle) return;
				settings.radius = vehicleBox(*vehicle).centreRadius;
				const ai::Vec3 authored = toAi(orderAt);
				const auto onward = [&](const ai::Vec3& approach) {
					if (horizontal2(fromAi(approach) - point->position) <= point->radius * point->radius) return true;
					const ai::Vec3 target = toAi(point->position);
					return infantryRecoveryRoute(m_navMesh, approach, target);
				};
				const auto approach = ai::vehicleApproachPoint(*mesh, toAi(vehicle->position), toAi(point->position),
					areaIndex >= 0 && areas[static_cast<size_t>(areaIndex)].hasOrderPosition[1] ? &authored : nullptr, settings, onward);
				if (approach) {
					bot.goal = fromAi(*approach);
					bot.vehicleApproach = horizontal2(bot.goal - point->position) > point->radius * point->radius;
					bot.approachInfantryGoal = point->position;
					bot.via.clear(); // the complete path was checked from here to the approach
				} else {
					// Stop retrying an unusable order: choose another area on the
					// next update, and allow this one again after 30 seconds.
					bot.goal = vehicle->position;
					bot.via.clear();
					bot.rejectedVehiclePoints[point->id] = m_time + 30;
					bot.orderRegoal = true;
				}
			}
		}
		return;
	}
	// W: the area's size across (vt+0x70 / 0x74, 0x10063de0: twice its
	// radius for the map, +0x98, createFromControlPoint's third argument).
	const float w = 2 * (areaIndex >= 0 ? areas[static_cast<size_t>(areaIndex)].temperature : point->radius);
	bot.goal = orderAt;
	for (int attempt = 0; attempt < 20; ++attempt) {
		const physics::Vec3 at = orderAt + physics::Vec3 {random() * w * 0.8f - 0.5f * w, 0, random() * w * 0.8f - 0.5f * w};
		if (valid(at)) {
			bot.goal = at;
			break;
		}
	}
}

// Bot::think (0x10099280): the weights, the urgencies of the behaviours
// weighted above 0 (in the order of their priority levels; a behaviour
// not evaluated keeps its last urgency, as the BBs store them per bot),
// and the arbiter.
void BotManager::think(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players)
{
	const std::string equipment = equipmentOf(self);
	if (bot.equipment != equipment) {
		bot.equipment = equipment;
		bot.arbitration = ai::Arbitration();
	}
	const ai::VehicleSetup& setup = m_settings.vehicle(equipment);
	const float sinceLast = bot.lastThink < 0 ? 0 : m_time - bot.lastThink;
	bot.lastThink = m_time;
	if (bot.planLocked) {
		return; // LockPlan (0x100ce070, run 0x100ce0a0 -> bot+0x7c): 0x100cd090 keeps the plan
	}
	// The personality ai.addBot gives (StandardWeights); AIBehaviours.ai's
	// values when the scripts were not run.
	static const std::map<std::string, float> standard {{"Idle", 0.1f}, {"Fire", 7.5f}, {"MedicAssist", 3.0f},
		{"Change", 1.9f}, {"Revive", 3.0f}};
	const auto personality = m_settings.modifiers.find(m_settings.botModifiers);
	const std::vector<float> weights = ai::behaviourWeights(setup.behaviours, bot.arbitration, sinceLast,
		personality != m_settings.modifiers.end() ? personality->second : standard, m_settings.modifiers, m_settings.curves);
	bot.arbitration.urgency.resize(setup.behaviours.size(), 0.0f);
	// Lower priority values first (bot+0x68: a list per level).
	std::vector<size_t> order(setup.behaviours.size());
	for (size_t i = 0; i < order.size(); ++i) {
		order[i] = i;
	}
	std::stable_sort(order.begin(), order.end(),
		[&](size_t a, size_t b) { return setup.behaviours[a].priority < setup.behaviours[b].priority; });
	int defaultIndex = -1;
	for (size_t i : order) {
		if (setup.behaviours[i].name == setup.defaultBehaviour) {
			defaultIndex = static_cast<int>(i);
		}
		if (weights[i] > 0) {
			bot.arbitration.urgency[i] = urgency(bot, setup.behaviours[i], weights[i], self, players);
		}
	}
	const int running = ai::arbitrate(bot.arbitration, defaultIndex);
	bot.behaviour = running >= 0 ? setup.behaviours[static_cast<size_t>(running)].name : setup.defaultBehaviour;
	const bool wasIdle = bot.pattern.rfind("BBPIdle", 0) == 0;
	bot.pattern = running >= 0 ? setup.behaviours[static_cast<size_t>(running)].pattern : std::string();
	if (const bool idle = bot.pattern.rfind("BBPIdle", 0) == 0; idle != wasIdle) {
		// The Idle statement made (0x100d2830) or freed (0x100d28a0).
		++(idle ? bot.idleStarts : bot.idleEnds);
	}
	if (bot.pattern.rfind("BBPFire", 0) != 0) {
		// The fire plan's end (AIDLL 0x100d4e20: vt+0x20c(0), vt+0x254(0)).
		bot.lobHold = false;
		bot.orderMove = false;
	}
}

// Bot vt+4 (0x1009a4c0): runs the chosen behaviour's plan for `seconds`
// (the time since this bot last ran).
void BotManager::act(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds)
{
	const std::string equipment = equipmentOf(self);
	if (bot.equipment != equipment) {
		// Into or out of a vehicle: a new behaviour set, nothing planned.
		bot.equipment = equipment;
		bot.arbitration = ai::Arbitration();
		bot.behaviour = m_settings.vehicle(equipment).defaultBehaviour;
		if (bot.pattern.rfind("BBPIdle", 0) == 0) {
			++bot.idleEnds; // the Idle plan's statement freed (0x100d28a0)
		}
		bot.pattern.clear();
		bot.lobHold = false;
		bot.orderMove = false;
	}
	if (self.vehicle != bot.seatedVehicle || self.seat != bot.seatedSeat) {
		if (!self.vehicle && bot.approachExitRequested && bot.seatedVehicle == bot.approachExitRequested) {
			bot.abandonedVehicle = bot.approachExitRequested;
			bot.abandonedAt = m_time;
		}
		if (self.vehicle && self.vehicle == bot.vehicleTarget) {
			// Seated where the get-in plan went (the statement's destructor
			// 0x100d4a90): out of the root's seat with the driver's taken and
			// no other seat reserved (0x100a8d80), "gogogo" (11); else, when
			// someone aboard holds a get_in from the bot, "thankyou" (8):
			// channel 7, 1 s.
			bool driver = false, reserved = false, asked = false;
			for (const GameWorld::PlayerInfo& p : m_players) {
				driver = driver || (p.alive && p.vehicle == self.vehicle && p.driving && p.id != self.id);
				if (p.alive && p.vehicle == self.vehicle && p.id != self.id) {
					if (const auto other = m_bots.find(p.id); other != m_bots.end()) {
						asked = asked || heard(other->second, 1, self.id);
					}
				}
			}
			for (const auto& [id, other] : m_bots) {
				reserved = reserved || (id != self.id && other.vehicleTarget == self.vehicle && other.seatedVehicle != self.vehicle);
			}
			if (!self.driving && driver && !reserved) {
				queueRadio(bot, 11, 7, 1.0f);
			} else if (asked) {
				queueRadio(bot, 8, 7, 1.0f);
			}
		}
		bot.pickupAsked = false;
		bot.seatedVehicle = self.vehicle;
		bot.seatedSeat = self.seat;
		bot.seatedAt = m_time;
	}
	bot.approachExitRequested = 0; // a request that did not produce an exit is not abandonment
	// A reachable approach can be outside the capture area. Stop there and
	// continue on foot; otherwise a bot can remain seated indefinitely.
	if (self.vehicle && self.driving && bot.vehicleApproach && bot.via.empty()
		&& horizontal2(bot.goal - self.position) < 6.0f * 6.0f) {
		for (const GameWorld::VehicleInfo& v : m_vehicles) {
			if (v.object == self.vehicle) {
				if (!infantryRecoveryRoute(m_navMesh, toAi(v.position), toAi(bot.approachInfantryGoal))) {
					// The actual arrival can differ from the planned approach.
					if (bot.goalControlPoint >= 0) bot.rejectedVehiclePoints[bot.goalControlPoint] = m_time + 30;
					bot.goal = v.position;
					bot.via.clear();
					bot.follower.reset();
					bot.vehicleApproach = false;
					bot.orderRegoal = true;
					return;
				}
				const float speed = physics::dot(v.velocity, v.forward);
				input.throttle = std::fabs(speed) > 1 ? (speed > 0 ? -1.0f : 1.0f) : 0;
				if (physics::lengthSquared(v.velocity) < 4.0f) {
					input.use = !bot.usedLast;
					if (input.use) {
						bot.approachExitRequested = self.vehicle;
					}
				}
				return;
			}
		}
	}
	const std::string& pattern = bot.pattern;
	if (pattern != "BBPMedicAssist" && pattern != "BBPAmmoAssist" && pattern != "BBPRepairVehicle"
		&& pattern != "BBPRevive") {
		m_tasks.erase(self.id);
	}
	const bool driving = self.vehicle && self.driving;
	const std::string equipmentNow = driving ? equipmentOf(self) : std::string();
	if ((equipmentNow == "Helicopter" || equipmentNow == "TransportHelicopter") && fly(bot, self, input, seconds)) {
		// A pilot (src/AI/Heli).
	} else if (pattern.rfind("BBPFire", 0) == 0 || pattern == "BBPHeliFire3d") {
		fire(bot, self, input, seconds);
	} else if (pattern.rfind("BBPGotoWaypoint", 0) == 0) {
		if (bot.hasGoal && bot.look.pattern != pattern) {
			// The plan's Turn (0x100d0fc0 -> 0x100d58b0, built by
			// BBPGotoWaypointInfantery 0x10066d80 and BBPGotoWaypoint2d
			// 0x10068980): the agent asked to look at the goal, weight 0.5.
			bot.look.requested = true;
			bot.look.request = bot.goal - self.position;
			bot.look.requestWeight = 0.5f;
		}
		if (bot.hasGoal) {
			const physics::Vec3 target = moveTarget(bot, self);
			const int id = 1000 + bot.goalOrder * 64 + static_cast<int>(bot.via.size());
			const bool moving = driving ? drive(bot, self, target, id, seconds, input) : steer(bot, self, target, id, seconds, input);
			if (driving && moving && drivesGun(self)) {
				// BBPGotoWaypoint2d's look (0x100d2500, vtable 0x1011a278, its
				// way 0x100d2560), built in place of the Turn when the seat's
				// ControlInfo slot 0x38 holds (0x100cf660; the rewrite: a
				// driver whose look turns the gun): along the path, the
				// follower's point 20 m ahead (0x100941c0, DAT_101270ac) on
				// x/z, level; the vehicle's facing within 0.01 m.
				const ai::Vec3 ahead = bot.follower.pointAhead(toAi(self.position), 20);
				physics::Vec3 way {ahead.x - self.position.x, 0, ahead.z - self.position.z};
				const float length = std::sqrt(horizontal2(way));
				if (length < 0.01f) {
					for (const GameWorld::VehicleInfo& v : m_vehicles) {
						way = v.object == self.vehicle ? physics::Vec3 {v.forward.x, 0, v.forward.z} : way;
					}
				} else {
					way = way * (1 / length);
				}
				const float throttle = input.throttle, strafe = input.strafe, turn = input.turn;
				aim(bot, self, self.position + physics::Vec3 {0, kEyeHeight, 0} + way * 20.0f, {}, false, {}, input, seconds);
				input.throttle = throttle;
				input.strafe = strafe;
				if (!drivesGun(self)) {
					input.turn = turn;
				}
			}
			if (!moving && bot.via.empty()) {
				bot.arbitration.replan = true; // the plan is done: think again soon
			}
		}
	} else if (pattern == "BBPRevive") {
		// To the body, the defibrillator in hand, and a shock down at it.
		GameWorld::PlayerInfo downed;
		int defibrillator = 0;
		for (const auto& [slot, name] : self.carried) {
			defibrillator = weaponAi(name).type == WeaponAi::Revive ? slot : defibrillator;
		}
		if (m_world.playerInfo(bot.target, downed)) {
			m_tasks[self.id] = {1, downed.id}; // the squad member's task (the plan 0x10082f10 -> 0x100bf470)
			const physics::Vec3 d = downed.position - self.position;
			// The plan (0x10082f10): within 1.6 m of the body (ObjectDistance)
			// the crouch-then-fire statement (0x100dda20, run 0x100dda60):
			// every run asks to crouch (vt+0x170(2, 1)); crouched, a press of
			// the trigger (PIFire) the first time, its release the next, and
			// the statement ends. Farther, MoveToObjectFinding to the body.
			if (horizontal2(d) > 1.6f * 1.6f) {
				bot.reviveShock = false;
				steer(bot, self, downed.position, 100000 + downed.id, seconds, input);
			} else {
				input.selectItem = self.inHandSlot != defibrillator ? defibrillator : 0;
				const float turn = angleBetween(self.yaw, headingTo(self.position, downed.position));
				input.turn = std::clamp(turn, -360 * seconds, 360 * seconds);
				bot.look.paused = true; // the plan's look at the body (0x100d51c0)
				bot.look.pausedAt = m_time;
				// The look down at the body (from the crouched eye, 1 m).
				input.aimPitch = std::clamp(std::atan2(downed.position.y - (self.position.y + 1.0f),
												std::sqrt(horizontal2(d))) * kDegrees, -60.0f, 0.0f);
				if (bot.pose != SoldierPose::Crouch) {
					bot.pose = SoldierPose::Crouch;
					bot.poseRequested = m_time;
				}
				input.pose = bot.pose;
				input.throttle = input.strafe = 0;
				if (self.pose == SoldierPose::Crouch && self.inHandSlot == defibrillator && std::fabs(turn) < 20) {
					input.fire = !bot.reviveShock;
					bot.reviveShock = !bot.reviveShock;
				}
			}
		}
	} else if (pattern == "BBPChange" || pattern == "BBPChangeLandingCraft") {
		// BBPChange (vtable 0x10116928: name 0x1006d5d0, constructor
		// 0x1006d650, destructor 0x1006d710; the urgencies hand it the
		// choice through 0x1006d760 and 0x1006d5f0). BBPChangeLandingCraft
		// (vtable 0x101169d8: name 0x1006fc90, constructor 0x1006fd00,
		// destructor 0x1006fd50, the choice 0x1006fcb0): its plan
		// 0x1006fd90 presses the seat's select key (0x100aa5f0) or gets out.
		bool working = false;
		physics::Vec3 at;
		const GameWorld::VehicleInfo* target = nullptr;
		for (const GameWorld::VehicleInfo& vehicle : m_vehicles) {
			if (vehicle.object == (self.vehicle ? self.vehicle : bot.vehicleTarget)) {
				working = vehicle.working;
				at = vehicle.position;
				target = &vehicle;
			}
		}
		if (self.vehicle && bot.vehicleTarget == self.vehicle && bot.seatTarget >= 0) {
			// Another seat of the same vehicle (0x1006dac0, the change's +0x14):
			// a press of that seat's select key (its Unit plug-in's setSelectKey,
			// 0x100aa5f0; without AI templates its position number), then wait to
			// be in it (AIDLL 0x100d4d30).
			int key = bot.seatTarget + 1;
			const auto seats = m_world.vehicleSeats(self.vehicle);
			if (static_cast<size_t>(bot.seatTarget) < seats.size()) {
				const AiPlugin* unit = m_settings.plugin(aiTemplateOf(seats[static_cast<size_t>(bot.seatTarget)].name), "Unit");
				key = unit && unit->selectKey > 0 ? unit->selectKey : key;
			}
			if (self.seat != bot.seatTarget) {
				input.switchSeat = bot.switchedLast ? 0 : key;
			}
		} else if (self.vehicle) {
			input.use = !bot.usedLast; // out
		} else if (bot.vehicleTarget) {
			// The get-in plan's request (0x1006dac0, its per-bot +0x14): the
			// vehicle's root seat taken (0x100d4190 over 0x100a8e50) and no
			// get_in held from anyone: req_pickup (2) to its driver, channel
			// 6, at once, once.
			if (!bot.pickupAsked) {
				int driver = -1;
				for (const GameWorld::PlayerInfo& p : m_players) {
					driver = p.alive && p.vehicle == bot.vehicleTarget && p.driving ? p.id : driver;
				}
				bool getIn = false;
				for (const Bot::Radio& r : bot.radio) {
					getIn = getIn || r.type == 1;
				}
				if (driver >= 0 && driver != self.id && !getIn) {
					queueRadio(bot, 2, 6, 0.0f, driver);
					bot.pickupAsked = true;
				}
			}
			// On foot (0x1006e9b9): MoveToObjectFinding at the vehicle with no
			// distance of its own (0x100cff90 keep 0, ended only by the root
			// seat taken or the vehicle's team changing, 0x100d4190 /
			// 0x100d4200), and beside it the PIEnter press (0x100d3110: input
			// 0xa, mask 0x400, 0x1006edbe) while the vehicle lies within the
			// two AI radii plus 1 m (ObjectDistance 0x100d4480 at 0x1006ed13,
			// centre to centre in 3D, 0x100d4620; the radii are the units'
			// physical slot 0x28, BF2142.exe's slot 0x38 r, summed at
			// 0x1006dceb). Not kept: a unit with setUseNoPathfindingToGetToObject
			// (0x100aa570; only the bipods) or without a Mobile part walks
			// instead to a point BBChange stored for the bot (MoveToFinding
			// 0x100d11c0 at the plan's +0x10 record, 0x1006ecbd) and is pressed
			// for from behind it only (0x100d42f0, -0.8, 0x100d4330); without
			// that point the gate would leave a bot in front stranded.
			const float reach = (target ? vehicleBox(*target).radius : VehicleBox().radius) + kSoldierRadius + 1.0f;
			const physics::Vec3 node = self.position + physics::Vec3 {0, kChestHeight, 0};
			const bool press = physics::lengthSquared(at - node) < reach * reach;
			steer(bot, self, at, 200000 + static_cast<int>(bot.vehicleTarget), seconds, input, 0, bot.vehicleTarget);
			if (press) {
				input.use = !bot.usedLast && working; // a press
			}
		}
	} else if (pattern == "BBPMedicAssist" || pattern == "BBPAmmoAssist") {
		assist(bot, self, input, seconds, pattern == "BBPAmmoAssist");
	} else if (pattern == "BBPRepairVehicle") {
		repair(bot, self, input, seconds);
	} else if (pattern == "BBPC4") {
		placeC4(bot, self, input);
	} else if (pattern == "BBPDefuseMines") {
		defuse(bot, self, input, seconds);
	} else if (pattern == "BBPRunOverSoldier") {
		runOver(bot, self, input);
	} else if (pattern.rfind("BBPAvoidCollision", 0) == 0 && !self.vehicle) {
		avoid(bot, self, input, seconds);
	} else if (pattern == "BBPFlee") {
		flee(bot, self, input, seconds);
	} else {
		if (pattern == "BBPIdle2d" && self.vehicle && self.driving && bot.idlePassenger >= 0) {
			// BBPIdle2d's plan (0x1006b2f0): the driver (the root's seat)
			// answers the passenger it waits for when that one asked for a
			// pickup (req_pickup 2, unanswered; 0x100a6260 marks it) and
			// stands within 40 m, a bot only while a seat is free
			// (0x100d2730): roger_that (6) on channel 6, 1 s (the plan's
			// Radio statement 0x100d3470).
			const GameWorld::PlayerInfo* p = playerOf(bot.idlePassenger);
			int free = 0;
			for (const GameWorld::VehicleInfo& v : m_vehicles) {
				free = v.object == self.vehicle ? v.freeSeats : free;
			}
			if (p && p->alive && !p->vehicle && physics::lengthSquared(p->position - self.position) < 1600.0f
				&& (!p->ai || free > 0)) {
				for (Bot::Radio& r : bot.radio) {
					if (r.type == 2 && r.sender == p->id && !r.answered) {
						r.answered = true;
						queueRadio(bot, 6, 6, 1.0f);
						break;
					}
				}
			}
		}
		idle(bot, self, input);
	}
	bot.look.pattern = pattern;
	// The sensing agent's update and look plan after the main plan
	// (0x1009a4c0 -> 0x100d7840): a soldier on foot (the Soldier pattern),
	// or a gunner whose seat's equipment type has the Turret pattern
	// (AIBehaviours.ai: Tank, Mech, Fixed, ArmedCar, ParatrooperFixed). The
	// Null pattern (FloatingTank, ManDown; the null pattern's plans
	// 0x1008b9f0 are a statement that does nothing, 0x100d1fe0(0, 1, 0))
	// and the vehicles without a pattern look nowhere of their own.
	if (!self.vehicle || turretLooks(self)) {
		lookAround(bot, self, input, seconds);
	}
	// The sentry drone is out: the detonator has nothing left to do.
	if (!self.vehicle && m_world.drone(self.id) != 0
		&& weaponAi(weaponName(self, self.inHandSlot)).type == WeaponAi::FireAndForget) {
		for (const auto& [slot, name] : self.carried) {
			if (slot == 3) {
				input.selectItem = slot;
			}
		}
		input.fire = false;
	}
}

namespace {
physics::Vec3 flatUnit(const physics::Vec3& v)
{
	const float l = std::sqrt(v.x * v.x + v.z * v.z);
	return l > 1e-5f ? physics::Vec3 {v.x / l, 0, v.z / l} : physics::Vec3 {0, 0, 1};
}
} // namespace

// The pilot's plans (AIDLL; the controllers in src/AI/Heli):
//  - BBPGotoWaypoint3d (0x10067690): no mesh, no path, no strategic
//    waypoints: straight to the goal, hovering 25 m over the highest of it
//    and the terrain round it (0x100674f0), the plan done within 2 m
//    (0x100cf320 for ITHelicopter; its test 0x100cf400, destructor
//    0x100cf370). Not yet airborne it first sits 10 s with
//    the collective down (While(10 s) HeliMoveTo(its own spot, margin 0),
//    0x100d1c60 / 0x100d19c0: a margin of 0 lets the collective go,
//    0x1008e406), then climbs. (While: 0x100d1950, its run 0x100d1a20, type 0x100d19b0,
//    slots 0x100d1ae0 / 0x100d1b00, destructor 0x100d1c00; the timed
//    condition's slot 0x100d1ca0.)
//  - BBPIdle3d (0x1006ab50): on the ground the controls let go; airborne
//    (its flag, or faster than 3 m/s) a transport hovers 25 m over its goal
//    or its own spot, a gunship (an Armament plug-in on the root) flies on
//    500 m ahead and to the right at its height.
//  - BBPHeliFire3d (0x1007f9d0 built by 0x1007f180): HeliMoveToObject to
//    the target with margin 25 until within 30 m, then attack runs:
//    HeliMoveAwayFromObject (150 m away, margin 40) and back
//    (HeliMoveToObject, margin 50 [0x10127688]); on the way back, with the
//    target between 20 m and the gun's maxRange ahead (dot over 0.5,
//    0x100d7b30), HeliAimAt (margin 40) and the trigger tapped every 60 /
//    round((1 - (1 - skill) x 0.8) x round(1.5 x roundsPerMinute)) s while
//    lined up; all of it only inside the combat area: the plan's If
//    (0x100d29f0) on 0x100dd3e0 (its test 0x100dd400; the target still
//    there: 0x100dd430 / 0x100dd460), which asks IAITerrain slot 0x1c
//    (0x008a0380 -> 0x004ca5d0, GameWorld::insideCombatArea; its 150 at +8
//    is never read), its else HeliMoveToObject at the target, margin 25.
//    The aim's lead point is HeliAimAt's solver (0x100dcf10: AIDLL 0x100fa060
//    takes the target's offset from the gun and the target's Mobile
//    velocity, 0x100fa6f0 iterates the launch angle in round(8 x p + 2)
//    steps with the projectile's drag (0x100fa100) and gravity, keeping
//    the direction at +0xb8, the impact point at +0xc4 and the time at
//    +0x110); its trigger condition 0x100dcab0 (evaluated by 0x100dcb00)
//    holds when the gun's forward is within atan(0.0125 d) of that
//    direction and the impact point within d of the target's position
//    predicted for that time (0x100fbad0), d and a closest-approach flag
//    from the weapon adaptor's slots 0x94 and 0x104. That solver isn't
//    rewritten (1572 bytes of iteration over the round's flight model,
//    which the rewrite's projectiles don't share with the AI): the aim
//    here is the target's position and the trigger the aim lined up.
// The four inputs are the vehicle's: PIThrottle the collective, PIYaw
// (strafe), PIRoll (the mouse's x, 5 degrees a unit) and PIPitch.
bool BotManager::fly(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds)
{
	const std::string& pattern = bot.pattern;
	if (pattern == "BBPChange" || pattern == "BBPChangeLandingCraft") {
		return false;
	}
	const GameWorld::VehicleInfo* vehicle = nullptr;
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		vehicle = v.object == self.vehicle ? &v : vehicle;
	}
	if (!vehicle) {
		return false;
	}
	// The plan is made when the behaviour starts (BBPGotoWaypoint3d reads
	// the goal once, 0x10067690).
	if (bot.heliPattern != pattern && !pattern.empty()) {
		bot.heliPattern = pattern;
		bot.heliPlanned = false;
		bot.heliRun = 0;
	}
	if (bot.heliVehicle != vehicle->object) {
		bot.heliVehicle = vehicle->object;
		bot.heli = {};
		bot.heliPlanned = false;
		bot.heliWaitUntil = -1;
		bot.heliLastAt = -1;
		bot.heliRun = 0;
		bot.heliLead = {};
		bot.heliOnTarget.reset();
	}
	ai::HeliSense sense;
	sense.position = toAi(vehicle->position);
	sense.velocity = toAi(vehicle->velocity);
	sense.angularVelocity = toAi(vehicle->angularVelocity);
	sense.forward = toAi(vehicle->forward);
	sense.right = toAi(vehicle->right);
	const physics::Vec3& f = vehicle->forward;
	const physics::Vec3& r = vehicle->right;
	sense.up = {f.y * r.z - f.z * r.y, f.z * r.x - f.x * r.z, f.x * r.y - f.y * r.x};
	const float dt = bot.heliLastAt >= 0 ? m_time - bot.heliLastAt : 0.0f;
	if (dt > 0) {
		sense.acceleration = toAi((vehicle->velocity - bot.heliLastVelocity) * (1 / dt));
	}
	bot.heliLastVelocity = vehicle->velocity;
	bot.heliLastAt = m_time;
	sense.terrain = [this](float x, float z) { return m_world.terrainHeight(x, z); };
	ai::HeliPilot pilot(bot.heli, sense);
	const physics::Vec3 at = vehicle->position;
	const float hag = pilot.heightAboveGround();
	std::string rootAi;
	if (const world::Object* object = m_world.objects().find(vehicle->object); object && object->objectTemplate()) {
		rootAi = aiTemplateOf(object->objectTemplate()->name());
	}
	const bool armed = m_settings.plugin(rootAi, "Armament") != nullptr;
	const auto hover = [&](const physics::Vec3& spot) {
		physics::Vec3 p = spot;
		p.y = m_world.terrainHeight(spot.x, spot.z) + 25;
		pilot.flyTo(toAi(p), 25);
	};

	GameWorld::PlayerInfo target;
	const bool fighting = pattern == "BBPHeliFire3d" && bot.target >= 0 && m_world.playerInfo(bot.target, target)
		&& target.alive;
	if (fighting) {
		const auto seats = m_world.vehicleSeats(self.vehicle);
		// The pilot's weapon (0x100dd4a0, flags 7, from BBPHeliFire3d's plan
		// 0x1007f9d0): of the seat's weapons with a strength above 0 against
		// the target's CombatStrengthType, the one scoring most (strictly),
		// the one in use counting twice (the plan's per-bot +0x14).
		std::string gun = seats.empty() ? std::string() : seats[0].weapon;
		int group = 0;
		if (!seats.empty() && !seats[0].weapons.empty()) {
			const int type = strengthTypeOf(target);
			int best = -1;
			float bestScore = 0;
			for (size_t i = 0; i < seats[0].weapons.size(); ++i) {
				const GameWorld::SeatWeapon& w = seats[0].weapons[i];
				if (w.weapon.empty()) {
					continue;
				}
				const WeaponAi ai = weaponAi(w.weapon);
				const float strength = type >= 0 && type < 8 ? ai.strength[static_cast<size_t>(type)] : 0;
				const float score = strength * (static_cast<int>(i) == bot.pilotWeapon ? 2.0f : 1.0f);
				if (strength > 0 && score > bestScore) {
					best = static_cast<int>(i);
					bestScore = score;
				}
			}
			if (best >= 0) {
				bot.pilotWeapon = best;
				gun = seats[0].weapons[static_cast<size_t>(best)].weapon;
				group = seats[0].weapons[static_cast<size_t>(best)].group;
			}
		}
		const WeaponAi weapon = weaponAi(gun);
		const float maxRange = weapon.maxRange > 0 ? weapon.maxRange : 300.0f;
		physics::Vec3 to = target.position - at;
		const float dist = std::sqrt(horizontal2(to));
		input.fire = false;
		// The attack runs only inside the combat area (the plan's If on
		// 0x100dd3e0: IAITerrain slot 0x1c, 0x008a0380 -> 0x004ca5d0);
		// outside it HeliMoveToObject at the target, margin 25.
		if (!m_world.insideCombatArea(self.id)) {
			pilot.flyTo(toAi(target.position), 25);
			bot.heliRun = 0;
		} else if (bot.heliRun == 0) {
			pilot.flyTo(toAi(target.position), 25); // HeliMoveToObject, margin 25
			bot.heliRun = dist < 30 ? 1 : 0;
		} else if (bot.heliRun == 1) {
			// HeliMoveAwayFromObject: 100 m on at its own height, margin 40,
			// until 150 m from the target.
			physics::Vec3 away = at + flatUnit(vehicle->forward) * 100.0f;
			pilot.flyTo(toAi(away), 40);
			bot.heliRun = dist > 150 ? 2 : 1;
		} else {
			const float length = std::sqrt(physics::lengthSquared(to));
			const physics::Vec3 dir = length > 0.01f ? to * (1 / length) : vehicle->forward;
			if (dist >= 20 && dist <= maxRange && physics::dot(dir, vehicle->forward) > 0.5f
				&& m_world.lineOfSight(at + vehicle->forward * 4.0f, target.position + physics::Vec3 {0, 1, 0},
					vehicle->object)) {
				// HeliAimAt's aim (0x100dcf10): the lead from the gun (the
				// hull's frame) for the round, the hull's motion inherited;
				// an indirect weapon's with precision 0.75, no inherited
				// motion and the direction laid flat. No solution: straight
				// at the target (slot 8).
				bool indirect = false;
				const ai::RoundModel round = roundModel(gun, &indirect);
				physics::Vec3 aimDir = dir;
				ai::Vec3 solved = toAi(dir), impact;
				double impactTime = 0;
				bool hasSolution = false;
				if (round.speed > 0) {
					bot.heliLead.setup(toAi(target.position - at), toAi(target.velocity),
						indirect ? ai::Vec3 {} : toAi(vehicle->velocity), toAi(vehicle->forward), m_time, round);
					hasSolution = bot.heliLead.solve(m_time, indirect ? 0.75f : 0.5f, false, solved, impact, &impactTime);
					if (hasSolution) {
						aimDir = {solved.x, indirect ? 0.0f : solved.y, solved.z};
						aimDir = aimDir * (1 / std::max(std::sqrt(physics::lengthSquared(aimDir)), 1e-4f));
					}
				}
				pilot.aimAt(toAi(aimDir), 40, bot.heliLastDirY, dt > 0 ? dt : seconds);
				bot.heliLastDirY = aimDir.y;
				// The trigger's condition (0x1007ec50 -> 0x100dcab0): the
				// AI weapon's allowedDeviation (slot 0x94) as the reach, the
				// closest approach for a weapon that isn't automatic (slot
				// 0x104).
				const world::ObjectTemplate* g = m_world.templates().find(gun);
				if (!bot.heliOnTarget) {
					bot.heliOnTarget.emplace(weapon.allowedDeviation, fireMode(g) != 2);
				}
				const bool onTarget = bot.heliOnTarget->test(hasSolution, bot.heliLead, solved, impact, impactTime, m_time);
				const float rpm = settingNumber(g, "fire.roundsPerMinute", 60);
				const float taps = std::round((1 - (1 - m_settings.skill) * 0.8f) * std::round(1.5f * rpm));
				const float spacing = taps > 0 ? 60.0f / taps : 1.0f;
				if (onTarget && m_time - bot.heliTapAt >= spacing) {
					(group == 1 ? input.altFire : input.fire) = true;
					bot.heliTapAt = m_time;
				}
			} else {
				pilot.flyTo(toAi(target.position), 50); // HeliMoveToObject, margin 50
				bot.heliRun = dist < 30 ? 1 : 2;
			}
		}
	} else if (pattern.rfind("BBPGotoWaypoint", 0) == 0 && bot.hasGoal) {
		if (!bot.heliPlanned) {
			bot.heliPlanned = true;
			bot.heliGoal = bot.goal;
			bot.heliHover = bot.goal;
			bot.heliHover.y = ai::heliHoverHeight(toAi(bot.goal),
								  [this](float x, float z) { return m_world.terrainHeight(x, z); })
				+ 25;
			bot.heliWaitUntil = bot.heli.airborne ? -1.0f : m_time + 10;
		}
		// The take-off wait (0x100d1cb0, made (10 s, flag 0)): it holds while
		// a soldier stands within 75 m (IAIInformationEnvironment slot 4): a
		// human always, a bot while the vehicle has a free seat
		// (0x100d3630).
		bool someone = false;
		int free = 0;
		for (const GameWorld::VehicleInfo& v : m_vehicles) {
			free = v.object == self.vehicle ? v.freeSeats : free;
		}
		for (const GameWorld::PlayerInfo& p : m_players) {
			if (p.id != self.id && p.alive && !p.vehicle && physics::lengthSquared(p.position - at) < 75.0f * 75.0f) {
				someone = someone || !p.ai || free > 0;
			}
		}
		if (!someone) {
			bot.heliWaitUntil = -1;
		}
		if (m_time < bot.heliWaitUntil) {
			pilot.flyTo(toAi(at), 0);
			bot.heli.controls.throttle = 0; // margin 0: the collective let go
		} else {
			pilot.flyTo(toAi(bot.heliHover), 25);
			if (physics::lengthSquared(bot.heliHover - at) < 2.0f * 2.0f) {
				bot.arbitration.replan = true; // the plan is done
				bot.heliPlanned = false;
			}
		}
	} else {
		const bool airborne = bot.heli.airborne || std::sqrt(physics::lengthSquared(vehicle->velocity)) > 3;
		if (!airborne) {
			pilot.reset();
		} else if (armed) {
			physics::Vec3 ahead = at + (flatUnit(vehicle->forward) + vehicle->right) * 500.0f;
			ahead.y = at.y;
			pilot.flyTo(toAi(ahead), 25);
		} else {
			hover(bot.hasGoal ? bot.goal : at);
		}
	}
	(void)hag;
	const ai::HeliControls& c = bot.heli.controls;
	input.throttle = std::clamp(c.throttle, -1.0f, 1.0f);
	input.strafe = std::clamp(c.yaw, -1.0f, 1.0f);
	input.turn = std::clamp(c.roll, -1.0f, 1.0f) * 5.0f;
	input.hasPitch = true;
	input.pitch = std::clamp(c.pitch, -1.0f, 1.0f);
	return true;
}

// BBPFlee's plan (0x10085fc0, the point from 0x10085de0): Serial of
// ChangeWeapon(camouflage), LockPlan(on), a Parallel that ends when both
// end: MoveToFinding sprinting to a point 50 m on from the bot along the
// flee direction (0x10085d90), U[-20, 20) m added across x and z, on the
// mesh (ten tries; none found: no run), and While(camouflage not empty
// and under 4 s) TriggerContinously(its fire); then the slot 2 weapon
// selected (Trigger on its select key) and LockPlan(off). The plan is
// locked: no other behaviour takes over until it ends (0x100cd090).
void BotManager::flee(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds)
{
	int camo = 0;
	for (const auto& [slot, name] : self.carried) {
		camo = weaponAi(name).type == WeaponAi::Camouflage && contains(name, "camo") ? slot : camo;
	}
	if (!camo) {
		bot.planLocked = false;
		return; // "Couldn't find the camouflage weapon"
	}
	if (!bot.fleePlanned) {
		bot.fleePlanned = true;
		bot.planLocked = true;
		bot.fleeCamoFrom = -1;
		bot.fleeRun = false;
		for (int attempt = 0; attempt < 10 && !bot.fleeRun; ++attempt) {
			physics::Vec3 p = self.position + bot.fleeDirection * 50.0f;
			p.x += random() * 40 - 20; // 9.313226e-9 x rand - 20
			p.z += random() * 40 - 20;
			if (!m_navMesh) {
				bot.fleePoint = p; // no mesh (the tests): the point as it is
				bot.fleeRun = true;
				break;
			}
			float offset = 0;
			// The pathfinder's slot 0x30 (0x1003f000 -> 0x1003c890: the
			// polygon within 5 m, 2.5 m on map 3).
			if (m_navMesh->findPolygon(toAi(p), 5.0f, nullptr, &offset) >= 0) {
				p.y += offset;
				bot.fleePoint = p;
				bot.fleeRun = true;
			}
		}
	}
	// ChangeWeapon first.
	if (bot.fleeCamoFrom < 0) {
		if (self.inHandSlot != camo) {
			input.selectItem = camo;
			return;
		}
		bot.fleeCamoFrom = m_time;
	}
	// The run and the camouflage side by side. The rewrite's camouflage has
	// no charge, so only the 4 s end it.
	if (bot.fleeRun) {
		bot.fleeRun = steer(bot, self, bot.fleePoint, 300000 + self.id, seconds, input, 0.5f);
		input.sprint = bot.fleeRun;
	}
	const bool camoOn = m_time - bot.fleeCamoFrom < 4.0f; // the double at 0x10116900
	input.fire = camoOn && self.inHandSlot == camo;
	if (!bot.fleeRun && !camoOn) {
		for (const auto& [slot, name] : self.carried) {
			if (slot == 2) {
				input.selectItem = slot; // back to the primary
			}
		}
		bot.fleePlanned = false;
		bot.planLocked = false;
	}
}

// The weapon handling (AIDLL 0x10099b70, called by the bot's execution
// 0x1009a4c0 after its plan ran). For a soldier (the object's flag bit 9)
// while no plan holds the weapon (bot +0x188, vt+0x124: set by the fire
// plan's top statement 0x100d4da0 (an InfoWrapper: 0x100f96c0, which runs
// the wrapped plan, 0x100f9710, and prints it, 0x100f9740; its interpreter
// entry 0x1008e440 / 0x1008e450 hands it to the bot's interpreter) and DefuseMines' 0x100ddc30, cleared at
// their ends 0x100d4e20 / 0x100ddc80):
//   - before that test, whatever plan holds the weapon: with the object's
//     physics node (+0x3c, slot 0x18) moving down at 1 m/s or more (its
//     y at most -1) and input 22 in the bot's input mask (+0x100 bit
//     0x400000), input 22 (c_PIWeaponSelect9, the kit's ParachuteLauncher)
//     is held at 1 (+0x58). The game's choice (0x004d2a50) reads the held
//     input (0x004bba40: over 0.5, no edge) every frame, so the parachute
//     opens as soon as its fire test passes (Parachutes.cpp: 2 m clear
//     below, 10 m/s down). Input 22 is a select input: a lower one pressed
//     the same frame (the primary's, input 16, below) wins the choice. The
//     flag at bot +0x264 that also gates the block isn't traced (taken as
//     clear);
//   - the weapon in hand (the armament's selection) busies the hands when
//     it is Healing (type 1) and the bot's own health is below 0.95, Ammo
//     (2) and the product over its other weapons (not Healing or Ammo, with
//     rounds, slot 0xc not -1, and a magazine size, 0xd8) of rounds /
//     magazine size is below 0.9, or Camouflage (6);
//   - otherwise, with the primary (bot +0x2b4: the weapon whose select
//     input is 16, c_PIWeaponSelect3, found at the spawn by 0x1009a070) in
//     hand and a magazine (the weapon adaptor's slot 0x14 above 0): when its
//     rounds in the magazine / the magazine size (slot 8(0) / 0xdc, both
//     ints, so an integer division) is below 0.8, c_PIReload (input 32,
//     +0x80) is pressed. The division makes that any magazine not full;
//     the firearm's reload (0x005c0f40 through 0x005c0e70) refuses one with
//     no magazine to spare;
//   - with another weapon in hand, the primary's select input is pressed.
// The rewrite presses the reload every other execution (its presses act on
// the rising edge) and, because its plans pick their tools through the
// same one select key, leaves the selection to the plans that use a tool
// (Revive, the assists, repair, C4, flee) and to a plan's own press.
void BotManager::handleWeapon(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input)
{
	if (self.vehicle || !self.alive || self.manDown) {
		return;
	}
	if (self.velocity.y <= -1.0f && input.selectItem == 0) {
		input.selectItem = 9; // input 22 held (+0x58 = 1)
	}
	const std::string& p = bot.pattern;
	if (p.rfind("BBPFire", 0) == 0 || p == "BBPHeliFire3d" || p == "BBPDefuseMines") {
		return; // +0x188 held
	}
	const std::string inHand = weaponName(self, self.inHandSlot);
	const int type = inHand.empty() ? -1 : weaponAi(inHand).type;
	const std::vector<GameWorld::WeaponAmmo> ammo = m_world.weaponAmmo(self.id);
	bool busy = type == WeaponAi::Camouflage;
	if (type == WeaponAi::Healing) {
		busy = healthOf(self.soldier) < 0.95f;
	} else if (type == WeaponAi::Ammo) {
		float product = 1;
		for (const GameWorld::WeaponAmmo& w : ammo) {
			const int t = weaponAi(w.weapon).type;
			if (t != WeaponAi::Healing && t != WeaponAi::Ammo && w.magSize > 0 && w.rounds != -1) {
				product *= static_cast<float>(w.rounds) / static_cast<float>(w.magSize);
			}
		}
		busy = product < 0.9f;
	}
	int primary = -1;
	for (const auto& [slot, name] : self.carried) {
		if (slot == 3) {
			primary = slot; // c_PIWeaponSelect3 (input 16)
		}
	}
	if (busy || primary < 0) {
		return;
	}
	if (self.inHandSlot == primary) {
		const GameWorld::WeaponAmmo* a = nullptr;
		for (const GameWorld::WeaponAmmo& w : ammo) {
			a = w.weapon == inHand ? &w : a;
		}
		if (a && a->magSize > 0 && static_cast<float>(a->inMag / a->magSize) < 0.8f) {
			input.reload = !bot.reloadedLast;
		}
		return;
	}
	// The primary's select (input 16) wins over the parachute's (input 22).
	if ((input.selectItem == 0 || input.selectItem == 9) && p != "BBPRevive" && p != "BBPMedicAssist"
		&& p != "BBPAmmoAssist" && p != "BBPRepairVehicle" && p != "BBPC4" && p != "BBPFlee") {
		input.selectItem = primary;
	}
}

// The bot's zoom. The statements press the AI weapon's zoom input (its
// slot 0x68) through the bot's input (0x1008d740); the game's zoom
// component steps on the press (0x005da670: the next level, past the last
// back to 0, not while reloading), zooms out on a reload with
// zoomOutOnReload (0x005da630) and on a weapon change, and asks the
// player's field of view for the level's factor over zoomDelay
// (0x005da3f0 -> 0x00591740). The soldier turns by its look inputs times
// that field of view over the camera's (1.1, 0x00610fd0) and the zoom's
// mouseSpeedMod (0x00586420; see aim), walks at the zoomed speed index and
// its deviation takes devModZoom (GameWorld::setZoomed). The sight is the
// same zoomed or not: the sensing's cone (0x100ed700) takes its row of the
// table at 0x1012b074 from the object's flag bit 10, not the zoom.
void BotManager::stepZoom(Bot& bot, const GameWorld::PlayerInfo& self, float seconds)
{
	const std::string weapon = self.alive && !self.vehicle && !self.manDown ? weaponName(self, self.inHandSlot) : std::string();
	if (weapon != bot.zoomWeapon) {
		bot.zoomWeapon = weapon;
		bot.zoomSettings = zoomSettingsOf(m_world.templates().find(weapon));
		bot.zoom.reset();
		bot.fov.set(kBaseFieldOfView, 0);
	}
	const anim::ZoomSettings& settings = bot.zoomSettings;
	const bool pressed = bot.zoomPress && !bot.zoomKey;
	if (settings.present()) {
		if (pressed && !self.reloading) {
			const float factor = bot.zoom.cycle(settings);
			bot.fov.set(factor > 0 ? factor : kBaseFieldOfView, settings.delay);
		} else if (self.reloading && !bot.zoomReloadWas && bot.zoom.level() > 0 && settings.zoomOutOnReload) {
			bot.zoom.reset();
			bot.fov.set(kBaseFieldOfView, settings.delay);
		}
	}
	bot.zoomReloadWas = self.reloading;
	bot.zoomKey = bot.zoomPress;
	bot.zoomPress = false;
	bot.zoom.tick();
	bot.fov.update(seconds);
	m_world.setZoomed(self.id, bot.zoom.level() > 0);
}

// BBPIdleInfantery's plan (0x1006a500): Parallel(Idle) under While(True).
// The Idle statement's interpreter (0x1008f840 -> 0x100e6120) lets go of
// the move and the trigger and levels the view: no yaw input, the pitch
// back to level. Inside the bot's own strategic area (within its order
// position's radius + 2 m, 0x10034a10) it also crouches (SoldierPose 2,
// 0x100d2900) and zooms in: If (forced) NOT the weapon zoomed (0x100d2f80
// over 0x100d3070, whose 0x100d3090 is the adaptor's slot 0x70) a
// BAPATrigger (0x100d3110) on its zoom input (slot 0x68), for a weapon that
// has one (stepZoom). The Idle statement 0x100d2830 (destructor 0x100d28a0)
// and the controls' reset statement 0x100d1fe0 (its run 0x100d2060, the
// interpreters' test 0x100d2030); the Idle patterns' base 0x100d26d0
// (destructors 0x100d26f0 / 0x100d2700).
void BotManager::idle(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input)
{
	if (self.vehicle) {
		return;
	}
	input.turn = 0;
	input.aimPitch = bot.pitch = 0;
	const auto& areas = m_settings.strategy.areas;
	for (size_t i = 0; i < areas.size(); ++i) {
		physics::Vec3 at;
		float radius = 0;
		int team = 0;
		if (!areaPosition(static_cast<int>(i), at, radius, team) || horizontal2(self.position - at) >= radius * radius) {
			continue; // not the area the bot is in
		}
		const physics::Vec3 order = areas[i].hasOrderPosition[0]
			? physics::Vec3 {areas[i].orderPosition[0][0], areas[i].orderPosition[0][1], areas[i].orderPosition[0][2]}
			: at;
		if (physics::lengthSquared(self.position - order) < (radius + 2) * (radius + 2)) {
			bot.pose = SoldierPose::Crouch;
			input.pose = bot.pose;
			bot.zoomPress = bot.zoomSettings.present() && bot.zoom.level() == 0;
		}
		break;
	}
}

float BotManager::urgency(Bot& bot, const ai::BehaviourSetup& behaviour, float weight, const GameWorld::PlayerInfo& self,
	const std::vector<GameWorld::PlayerInfo>& players)
{
	const std::string& p = behaviour.pattern;
	if (behaviour.bb == "BBChangeLandingCraft" && self.vehicle) {
		return landingCraftUrgency(bot, self, weight);
	}
	if (behaviour.bb == "BBFire3d") {
		return fire3dUrgency(bot, self, weight, players);
	}
	if (p.rfind("BBPFire", 0) == 0 || p == "BBPHeliFire3d") {
		return fireUrgency(bot, self, players);
	}
	if (p.rfind("BBPGotoWaypoint", 0) == 0) {
		return moveUrgency(bot, self, weight);
	}
	if (p.rfind("BBPIdle", 0) == 0) {
		return idleUrgency(bot, self, weight, players);
	}
	if (p == "BBPRevive") {
		return reviveUrgency(bot, self, players);
	}
	if (p == "BBPChange" || p == "BBPChangeLandingCraft") {
		return changeUrgency(bot, self, weight);
	}
	if (p == "BBPFlee") {
		return fleeUrgency(bot, self, players);
	}
	if (p == "BBPMedicAssist" || p == "BBPAmmoAssist") {
		return medicUrgency(bot, self, weight, p == "BBPAmmoAssist");
	}
	if (p == "BBPRepairVehicle") {
		return repairUrgency(bot, self, weight);
	}
	if (p == "BBPC4") {
		return c4Urgency(bot, self);
	}
	if (p == "BBPDefuseMines") {
		return defuseUrgency(bot, self, weight);
	}
	if (p == "BBPRunOverSoldier") {
		return runOverUrgency(bot, self);
	}
	if (p.rfind("BBPAvoidCollision", 0) == 0) {
		return avoidUrgency(bot, self, weight);
	}
	// BBPBridgeRepair (0x10084620): broken bridges (the bridge-destroyed
	// events 0x1004e4e0) don't exist in the rewrite: 0. (Its helpers
	// 0x100841e0, 0x10084e40; the BB class 0x10084490 / 0x10084590 /
	// 0x100845f0; the pattern BBPBridgeRepair, name 0x10084c20,
	// constructor 0x10084c40, destructor 0x10084dd0, its plan 0x10084f10,
	// never built here.) BBPCallForMedic,
	// the 3d and boat patterns: not in the shipped ground sets.
	return 0;
}

// BBFire's urgency (0x10075fb0) and the choice of target and weapon. For
// each known enemy and each carried weapon: the weapon's strength against
// the target's type (FireAndForget only when facing it within 0.9 and
// seen clear, C4
// within 4 m; Healing/Ammo/Repair/Camouflage none), x1.03 for the weapon
// in hand. The target scores
//   max(0, 1 - d / (1.2 maxRange)) x best strength x freshness x 5
// (freshness 1 in sight, 1 / (0.05 lost + 1.5) since lost), beyond
// maxRange only when the bot could close in within 5 s. The current
// target is kept unless another scores 1.2 times more. Any score above 0:
// urgency 50 (the weight only gates it). (BBFireInfantery's class:
// constructor 0x10081ef0, destructor 0x10081f40; its slot 13 0x10081f30
// -> 0x10074cb0 hands the latched target to the bot, vt+0x70, and to the
// pattern's slot 7: bot.target here.)
float BotManager::fireUrgency(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players)
{
	const physics::Vec3 eye = self.position + physics::Vec3 {0, kEyeHeight, 0};
	float gunYaw = 0, gunPitch = 0;
	const bool gunning = self.vehicle && (!self.driving || drivesGun(self)) && m_world.gunAim(self.vehicle, self.seat, gunYaw, gunPitch);
	if (self.vehicle && !gunning && !self.driving) {
		bot.target = -1;
		return 0; // a seat without a gun
	}
	if (!gunning) {
		bot.seatWeapon = -1;
	}
	const float forward = gunning ? self.yaw + gunYaw : self.yaw;
	float bestScore = 0, currentScore = 0;
	int bestTarget = -1, bestSlot = 0;
	for (const Known& known : bot.known) {
		if (!known.everSeen) {
			continue;
		}
		const auto other = std::find_if(players.begin(), players.end(), [&](const auto& p) { return p.id == known.player; });
		if (other == players.end() || !other->alive) {
			continue;
		}
		const int targetType = strengthTypeOf(*other);
		const physics::Vec3 at = known.position + physics::Vec3 {0, kChestHeight, 0};
		const float distance = std::max(std::sqrt(physics::lengthSquared(at - eye)), 1.0f);
		const bool facing = std::cos(angleBetween(forward, headingTo(eye, at)) * kRadians) >= 0.9f;
		float best = 0, maxRange = 0;
		int slot = 0;
		const auto consider = [&](int s, const std::string& name) {
			const WeaponAi w = weaponAi(name);
			float score = 0;
			switch (w.type) {
			case WeaponAi::FireArm:
			case WeaponAi::Revive:
			case WeaponAi::CloseCombat:
			case WeaponAi::Sniper:
			case WeaponAi::TitanWeapon:
				score = w.strength[static_cast<size_t>(targetType)];
				break;
			case WeaponAi::C4:
				score = distance <= 4 && std::fabs(at.y - eye.y) <= 4 ? w.strength[static_cast<size_t>(targetType)] : 0;
				break;
			case WeaponAi::FireAndForget:
				// Facing within 0.9 and the last sighting clear (flag 0x40;
				// 0x10075fb0's type 9 test).
				score = facing && known.seenClear && m_world.drone(self.id) == 0 ? w.strength[static_cast<size_t>(targetType)] : 0;
				break;
			default:
				break;
			}
			if (score > 0 && s == self.inHandSlot) {
				score *= 1.03f;
			}
			if (score > 0) {
				maxRange = std::max(maxRange, w.maxRange);
			}
			if (score > best) {
				best = score;
				slot = s;
			}
		};
		if (gunning) {
			// Each of the seat's weapons (seatArmament), as the kit's are.
			slot = -1;
			for (const auto& [index, gun] : seatArmament(self)) {
				WeaponAi w = weaponAi(gun);
				float score = w.strength[static_cast<size_t>(targetType)];
				float range = w.maxRange;
				if (!m_settings.weapons.count(lower(gun))) {
					// No weaponTemplate for the gun (the vehicles' ai/Weapons.ai
					// not run, as in the tests; the shipped vehicles have one):
					// any target, out to 100 m.
					score = std::max(score, 1.0f);
					range = std::max(range, 100.0f);
				}
				if (score > 0) {
					maxRange = std::max(maxRange, range);
				}
				if (score > best) {
					best = score;
					slot = index;
				}
			}
		} else {
			for (const auto& [s, name] : armament(self)) {
				consider(s, name);
			}
		}
		if (best <= 0 || maxRange <= 0) {
			continue;
		}
		if (distance > maxRange && distance - maxRange > kSoldierSpeed * 5) {
			continue;
		}
		const float freshness = known.lostAt < 0 ? 1.0f : 1.0f / ((m_time - known.lostAt) * 0.05f + 1.5f);
		const float score = std::max(0.0f, 1 - distance / (1.2f * maxRange)) * best * freshness * 5;
		if (known.player == bot.target) {
			currentScore = score;
		}
		if (score > bestScore) {
			bestScore = score;
			bestTarget = known.player;
			bestSlot = slot;
		}
	}
	if (bestScore <= 0) {
		bot.target = -1;
		bot.planTarget = -1;
		return 0;
	}
	if (bot.target >= 0 && currentScore > 0 && bestScore <= currentScore * 1.2f) {
		// Keep the current target, with the weapon that scores best on it
		// (a sentry detonator scores nothing once its drone is out).
		if (bestTarget == bot.target) {
			(gunning ? bot.seatWeapon : bot.weaponSlot) = bestSlot;
		}
		pickWeapon(bot, self, players);
		planFire(bot, self, gunning);
		return 50;
	}
	if (bestTarget != bot.target) {
		bot.target = bestTarget;
		bot.targetAcquired = m_time; // Bot::setTarget (0x10097af0; its target record's reset 0x100ef3e0)
		// The reaction time (0x100777a0): (1 - skill)(1.5 + U[0,1]) s.
		const WeaponAi w = weaponAi(gunning ? seatWeaponName(self, bestSlot) : weaponName(self, bestSlot));
		bot.triggerAt = m_time + (w.type == WeaponAi::CloseCombat ? 0.0f : (1 - m_settings.skill) * (1.5f + random()));
	}
	(gunning ? bot.seatWeapon : bot.weaponSlot) = bestSlot;
	pickWeapon(bot, self, players);
	planFire(bot, self, gunning);
	return 50;
}

float fireReach(const WeaponAi& w, float dh)
{
	// 0x100795a0 (LAB_10079dde): maxRange + 1.25 dh over 5 m up, not for a
	// close-combat (type 7) or thrown weapon.
	return w.maxRange + (dh > 5 && w.type != WeaponAi::CloseCombat && !w.thrown ? 1.25f * dh : 0.0f);
}

float fireMinRange(const WeaponAi& w, float dh, bool explosivePlan)
{
	// The plan's near limit: minRange, 5 m for a thrown weapon in the
	// explosive plan at a target over 3 m up.
	return dh > 3 && explosivePlan && w.thrown ? 5.0f : w.minRange;
}

float preferredRange(const WeaponAi& w, float skill, float roll, float dh, bool explosivePlan)
{
	float f = w.optimal;
	if (w.type != WeaponAi::CloseCombat) {
		f = std::max(0.2f, skill) * f * (0.8f + roll * 0.4f);
	}
	const float lo = fireMinRange(w, dh, explosivePlan);
	// + 0.25 dh above only in the explosive plan (+0x249 bonus) and not thrown.
	return lo + (fireReach(w, dh) - lo) * f + (explosivePlan && dh > 0 && !w.thrown ? 0.25f * dh : 0.0f);
}

float soundSphereRadius(const AiPlugin* mobile, float speed)
{
	if (!mobile) {
		return 0;
	}
	// 0x008a1880: the template's +8 maxSpeed, +0x34 max, +0x38 min.
	if (speed >= mobile->maxSpeed) {
		return mobile->soundSphereMax;
	}
	const float t = speed / mobile->maxSpeed;
	return t * mobile->soundSphereMax + (1 - t) * mobile->soundSphereMin;
}

float hearingReach(bool weaponWithin3s, float weaponSphere, float mobileSphere)
{
	// 0x100ca280: the armament's current weapon (slot 0x7c -> 0x28) when
	// its stamp (slot 0x100) is under 3 s old, its slot 0x12c; then the
	// Mobile's slot 0x54 when larger. Over 0.1 m the bots within it hear.
	const float weapon = weaponWithin3s ? weaponSphere : 0.0f;
	const float reach = std::max(weapon, mobileSphere);
	return reach > 0.1f ? reach : 0.0f;
}

float hearingOdds(float distance, bool weaponWithin5s, float weaponSphere, float mobileSphere,
	const AiPlugin* listenerMobile, float listenerSpeed, float& p)
{
	p = 0;
	float r = 1;
	if (weaponWithin5s && distance < weaponSphere) {
		r = distance / weaponSphere;
	}
	if (mobileSphere > 0 && distance < mobileSphere) {
		r = std::min(r, distance / mobileSphere);
	}
	if (!(r < 1)) {
		return 0;
	}
	float q = 0;
	p = 1;
	if (listenerMobile) {
		// The listener's Mobile part (AI object +0x3c): its speed (slot
		// 0x24, 0x100ab050) over maxSpeed (AIDLL 0x100a4350), through 0x100317e0,
		// which returns the larger of 1 and that.
		const float fraction = listenerMobile->maxSpeed > 0 ? listenerSpeed / listenerMobile->maxSpeed : 0.0f;
		q = std::max(1.0f, fraction);
		p = listenerMobile->hearingFar * r + (1 - r) * listenerMobile->hearingNear; // slots 0x60, 0x64
	}
	return (1 - q) * p;
}

float heardLastSeen(float now, float c, float degeneration)
{
	c = std::clamp(c, 0.0f, 1.0f);
	if (c == 0) {
		return -1e9f; // +0x48 = 0: seen at the clock's start
	}
	if (!(degeneration > 0)) {
		return now; // no decay (knowledgeCertainty keeps it at 1)
	}
	return now - 1 / (c * degeneration);
}

float fire3dUnitScale(float summedStrength)
{
	return 1.0f < summedStrength ? 1.0f : 0.01f;
}

// The fire plan (0x100795a0, built by BBFire's plan maker 0x1007cf10). The
// running plan is kept while it is a fire plan (type 0xe) of this
// behaviour, not done, for the same target (plan +0x20 against the
// behaviour's per-bot target, +0xc[bot]), the same weapon (+0x24 against
// +0x14[bot], the weapon the choice settled on) and the same flags
// (+0x2c, +0x38 against bot vt+0x264); otherwise a new plan is built,
// and with it the distance kept: minRange + (maxRange - minRange) f,
// f = optimal x max(0.2, skill) x U[0.8, 1.2] (the weapon's +0x18 x
// 0.01; a CloseCombat weapon, type 7, optimal alone), maxRange + 1.25 dh
// for a target over 5 m up (not melee or thrown), + 0.25 dh for one above
// in the explosive plan (not thrown; minRange 5 m for a thrown weapon at
// a target over 3 m up in it). So switching to the knife makes the plan, and the range,
// the knife's.
void BotManager::planFire(Bot& bot, const GameWorld::PlayerInfo& self, bool gunning)
{
	if (bot.target == bot.planTarget && (gunning ? bot.seatWeapon : bot.weaponSlot) == bot.planSlot) {
		return;
	}
	const auto known = std::find_if(bot.known.begin(), bot.known.end(), [&](const Known& k) { return k.player == bot.target; });
	if (known == bot.known.end()) {
		return;
	}
	const std::string gun = gunning ? seatWeaponName(self, bot.seatWeapon) : weaponName(self, bot.weaponSlot);
	const physics::Vec3 eye = self.position + physics::Vec3 {0, kEyeHeight, 0};
	const float dh = known->position.y + kChestHeight - eye.y;
	// The explosive plan's flag (local_60 of 0x100795a0, as in fire): the
	// weapon choice's bonus, an explosive at a soldier known only from
	// others or over 30 m up (not a sniper rifle or thrown weapon).
	const WeaponAi w = weaponAi(gun);
	GameWorld::PlayerInfo target;
	const bool targetSoldier = m_world.playerInfo(bot.target, target) && !target.vehicle;
	const bool high = w.type != WeaponAi::Sniper && !w.thrown && known->position.y - self.position.y > 30;
	const bool explosivePlan = bot.explosive && w.explosionRadius > 0 && targetSoldier && (known->secondHand || high);
	bot.preferredRange = game::preferredRange(w, m_settings.skill, random(), dh, explosivePlan);
	bot.planTarget = bot.target;
	bot.planSlot = gunning ? bot.seatWeapon : bot.weaponSlot;
	bot.manoeuvre = bot.chase = bot.guidedMove = false; // a new plan's statements start afresh
	bot.backOffAt = -1e9f;
}

// The weapon choice for the target BBFire settled on (0x10074ef0, called
// once more for it after the targets' loop; see chooseWeapon), over the
// bot's object's weapons: on foot the kit's, at a gun the seat's. The
// choice replaces the slot the targets' loop found (the original keeps the
// last choice when none comes back). The distance and height are between
// the bot and the target's remembered position. The target's Mobile slot
// 0x78 (BF2142.exe 0x008a1960: its interface 0xc4c5 slot 0x108,
// 0x0059d950) is its sprint stamina (0x006457f0's +0x18), not a speed.
// Stand-ins: the target attending to the bot
// (a bot: its target is this one, its vt+0x74; a human: the sensing
// agent's stamp for it within 9995 s, AIDLL 0x100eb7f0 on the map at +0x74
// +0x84, whose writer wasn't traced) is, for a human, a round of it having
// passed the bot (the rewrite's alert). No bot is scripted
// (+0x264, set by the scripted-bot commands 0x1004d140/0x1004d8e0), and
// no wire-guided weapon holds a target for a bot (the LPTargetComp's
// target isn't modelled). The fire plan's move to the order's spot that
// sets +0x248 (0x100795a0 -> 0x100d11c0, when 0x100775f0 finds the spot
// set) isn't rewritten: +0x248 stays clear.
void BotManager::pickWeapon(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players)
{
	bot.explosive = false;
	const auto known = std::find_if(bot.known.begin(), bot.known.end(), [&](const Known& k) { return k.player == bot.target; });
	const auto target = std::find_if(players.begin(), players.end(), [&](const auto& p) { return p.id == bot.target; });
	if (known == bot.known.end() || target == players.end()) {
		return;
	}
	float gunYaw = 0, gunPitch = 0;
	const bool gunning = self.vehicle && (!self.driving || drivesGun(self)) && m_world.gunAim(self.vehicle, self.seat, gunYaw, gunPitch);
	std::vector<std::pair<int, std::string>> weapons;
	if (gunning) {
		weapons = seatArmament(self);
	} else if (!self.vehicle) {
		weapons = armament(self);
	}
	if (weapons.empty()) {
		return;
	}
	const std::vector<GameWorld::WeaponAmmo> ammo = gunning ? seatAmmo(self, weapons)
		: self.vehicle                                   ? std::vector<GameWorld::WeaponAmmo> {}
														 : m_world.weaponAmmo(self.id);
	const std::vector<GameWorld::WeaponAmmo> targetAmmo = target->vehicle ? std::vector<GameWorld::WeaponAmmo> {}
		: m_world.weaponAmmo(target->id);
	const auto ammoOf = [](const std::vector<GameWorld::WeaponAmmo>& list, const std::string& name) -> const GameWorld::WeaponAmmo* {
		for (const GameWorld::WeaponAmmo& a : list) {
			if (a.weapon == name) {
				return &a;
			}
		}
		return nullptr;
	};
	const int targetType = std::clamp(strengthTypeOf(*target), 0, 7);
	const physics::Vec3 at = known->position;
	WeaponChoice c;
	c.soldierBot = !self.vehicle;
	c.targetSoldier = !target->vehicle;
	c.targetHuman = !target->ai;
	c.distance = std::sqrt(physics::lengthSquared(at - self.position));
	c.targetEmpLevel = target->vehicle ? static_cast<float>(m_world.empLevel(target->vehicle)) : 0.0f;
	if (!self.vehicle) {
		const std::string inHand = weaponName(self, self.inHandSlot);
		const int type = weaponAi(inHand).type;
		c.inHandMelee = type == WeaponAi::CloseCombat || type == WeaponAi::Revive;
		const GameWorld::WeaponAmmo* a = ammoOf(ammo, inHand);
		c.inHandEmpty = !c.inHandMelee && a && a->inMag == 0;
	}
	if (!target->vehicle) {
		std::string theirs;
		for (const auto& [slot, name] : target->carried) {
			theirs = slot == target->inHandSlot ? name : theirs;
		}
		const GameWorld::WeaponAmmo* a = ammoOf(targetAmmo, theirs);
		c.targetInHandEmpty = a && a->inMag == 0;
	}
	if (target->ai) {
		const auto it = m_bots.find(target->id);
		c.targetTargetsMe = it != m_bots.end() && it->second.target == self.id;
	} else {
		c.targetTargetsMe = known->alertAt > -1e8f;
	}
	// The Mobile's slot 0x78 (0x008a1960): the target's interface 0xc4c5
	// slot 0x108, 0x0059d950, is its sprint stamina (the SprintStamina's
	// +0x18 that 0x006457f0 writes: the soldier's +0x378, a vehicle's PCO
	// +0x2ec), 0..1, 1 when rested.
	c.targetMotion = target->stamina;
	if (target->vehicle) {
		bool sprinting = false;
		c.targetMotion = 1;
		m_world.vehicleStamina(target->vehicle, c.targetMotion, sprinting);
	}
	c.c4Placed = bot.c4Placed;
	if (target->vehicle && !m_settings.aiTemplates.empty()) {
		const world::Object* o = m_world.objects().find(target->vehicle);
		c.targetMobile = o && o->objectTemplate() && m_settings.plugin(aiTemplateOf(o->objectTemplate()->name()), "Mobile");
	}
	const float facing = self.yaw * kRadians;
	c.targetAlongFacing = target->velocity.x * std::sin(facing) + target->velocity.z * std::cos(facing);
	c.orderMove = bot.orderMove;
	if (!self.vehicle) {
		std::vector<GuardArea> areas;
		for (const ai::StrategicArea& area : m_settings.strategy.areas) {
			for (const GameWorld::ControlPointInfo& point : m_world.controlPointList()) {
				if (point.id == area.controlPoint) {
					// The side's spot starts at the area's position (0x100623c0)
					// and the rewrite's areas don't move it; the side's radius
					// (+0x98 + 4 team, the spot's distance to the area's +0x118
					// point there) is the control point's here.
					GuardArea g;
					g.held = point.team == self.team;
					g.centre = point.position;
					g.spotX = point.position.x;
					g.spotZ = point.position.z;
					g.radius = point.radius;
					areas.push_back(g);
				}
			}
		}
		const bool moving = bot.lastInput.throttle != 0 || bot.lastInput.strafe != 0 || bot.lastInput.sprint;
		strategicAreaGuard(areas, self.position, moving, at, c.areaHold, c.targetInArea);
	}
	ExplosivePick& pick = c.pick;
	pick.heightAbove = at.y - self.position.y;
	pick.lobbing = bot.lobbing;
	pick.now = m_time;
	pick.conditionFailedAt = bot.conditionFailedAt;
	pick.vehicleFailedAt = bot.vehicleFailedAt;
	// The weapons without setFiresThroughTransparent want the target seen
	// clear or known from others (the memory item's flags 0x48).
	c.clearSighting = known->seenClear || known->secondHand;
	pick.seen = known->everSeen;
	pick.secondHand = known->secondHand;
	pick.seenAt = known->seenAt;
	std::copy(std::begin(bot.explosiveShots), std::end(bot.explosiveShots), pick.explosiveShots);
	// The melee tests: the target on the bot's mesh, outside the vehicles'
	// rooms (0x100947b0), and the mesh ray to it.
	bool reachable = true, ray = true;
	if (!self.vehicle && m_navMesh) {
		reachable = m_navMesh->findPolygon(toAi(at), 1.0f) >= 0;
		for (const GameWorld::VehicleInfo& v : m_vehicles) {
			const float r = vehicleBox(v).centreRadius;
			reachable = reachable && physics::lengthSquared(v.position - at) >= r * r;
		}
		ray = ai::meshRaycast(*m_navMesh, toAi(self.position), toAi(at), nullptr, -1);
	}
	std::vector<WeaponOption> options;
	for (const auto& [slot, name] : weapons) {
		const WeaponAi w = weaponAi(name);
		const world::ObjectTemplate* t = m_world.templates().find(name);
		WeaponOption o;
		o.type = w.type;
		o.strength = w.strength[static_cast<size_t>(targetType)];
		o.explosionRadius = w.explosionRadius;
		o.thrown = w.thrown;
		o.fireMode2 = fireMode(t) == 2;
		o.minRange = w.minRange;
		o.maxRange = w.maxRange;
		if (gunning && !m_settings.weapons.count(lower(name))) {
			o.strength = std::max(o.strength, 1.0f); // as the targets' loop: no weaponTemplate, any target to 100 m
			o.maxRange = std::max(o.maxRange, 100.0f);
		}
		o.guidance = guidanceKind(t);
		o.firesThroughTransparent = w.firesThroughTransparent;
		o.emp = w.emp;
		if (const GameWorld::WeaponAmmo* a = ammoOf(ammo, name); a && a->magSize >= 0) {
			// nrOfMags -1: the ammo component's one magazine (0x005f4c70).
			o.rounds = a->magazines < 0 ? a->inMag : a->rounds;
			o.inMag = a->inMag;
		}
		o.reloadTime = std::max(settingNumber(t, "ammo.reloadTime", 2), settingNumber(t, "ammo.minimumTimeUntilReload", 0));
		o.autoReload = settingNumber(t, "ammo.autoReload", 0) != 0;
		o.meleeReachable = reachable;
		o.meleeRay = ray;
		options.push_back(o);
	}
	bool explosive = false;
	const int chosen = chooseWeapon(c, options, explosive);
	bot.explosive = explosive;
	if (chosen >= 0 && !self.vehicle) {
		bot.weaponSlot = weapons[static_cast<size_t>(chosen)].first;
	} else if (chosen >= 0 && gunning) {
		bot.seatWeapon = weapons[static_cast<size_t>(chosen)].first;
	}
}

// BBMoveTo's urgency (0x10066a70): the goal's (ai::gotoUrgency) times the
// weight. (BBMoveTo, vtable 0x10116578: constructor 0x100669a0, destructor
// 0x10066a40.)
float BotManager::goalObjectUrgency(const Bot& bot, const GameWorld::PlayerInfo& self) const
{
	if (!bot.hasGoal) {
		return 0;
	}
	if (const auto scripted = m_goals.find(self.id); scripted != m_goals.end()) {
		ai::Goal copy = scripted->second;
		return ai::goalUrgency(copy, toAi(self.position), horizontal2(bot.goal - self.position), kSoldierRadius);
	}
	if (bot.squadGoal) {
		ai::Goal copy = bot.formation;
		return ai::goalUrgency(copy, toAi(self.position), horizontal2(bot.goal - self.position), kSoldierRadius);
	}
	Bot probe = bot;
	return const_cast<BotManager*>(this)->moveUrgency(probe, self, 1);
}

float BotManager::moveUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight)
{
	if (!bot.hasGoal) {
		return 0;
	}
	if (const auto scripted = m_goals.find(self.id); scripted != m_goals.end()) {
		// The goal's own urgency (its slot 1); the same distance as the
		// SAI's goals below.
		const float u = ai::goalUrgency(scripted->second, toAi(self.position), horizontal2(bot.goal - self.position),
			kSoldierRadius);
		if (scripted->second.done && !bot.goalReached) {
			bot.goalAt = m_time;
			bot.goalReached = true;
		}
		return u * weight;
	}
	if (bot.squadGoal) {
		// The formation's waypoint goal (0x1008aaa0 over the slot).
		const float u = ai::goalUrgency(bot.formation, toAi(self.position), horizontal2(bot.goal - self.position),
			kSoldierRadius);
		if (bot.formation.done && !bot.goalReached) {
			bot.goalAt = m_time;
			bot.goalReached = true;
		}
		return u * weight;
	}
	ai::GoalArea area;
	float radius = 0;
	if (bot.goalOrder >= 0) {
		const ai::StrategicArea& a = m_settings.strategy.areas[static_cast<size_t>(bot.goalOrder)];
		for (const GameWorld::ControlPointInfo& point : m_world.controlPointList()) {
			if (point.id == a.controlPoint) {
				area.exists = true;
				area.teamHolds = point.team == self.team;
				area.radius = a.temperature; // the area's per-side value at +0x98 (createFromControlPoint's third argument)
				area.inside = horizontal2(self.position - point.position) < area.radius * area.radius;
			}
		}
	} else if (const GameWorld::SquadOrder* order = self.squad ? m_world.squadOrder(self.team, self.squad) : nullptr) {
		radius = order->radius;
	}
	bool arrived = false;
	const float u = ai::gotoUrgency(horizontal2(bot.goal - self.position), radius, kSoldierRadius, area, arrived);
	if (arrived || (area.exists && u == 0)) {
		if (!bot.goalReached) {
			bot.goalAt = m_time;
		}
		bot.goalReached = true;
	}
	return u * weight;
}

// BBIdle's urgency (0x10069f50): the weight; 0.5 for a passenger; a driver
// with seats free (0x100d2730: a seat with nobody in it) waits for a teammate heading for them within 40 m (30,
// 70 when it faces the vehicle within 0.85) for up to 10 s (0x10069af0;
// teammates are the bots, whose choice is known). (BBIdle, vtable
// 0x10116760: constructor 0x100697e0, destructor 0x10069aa0, the per-bot
// reset 0x10069970.)
float BotManager::idleUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight,
	const std::vector<GameWorld::PlayerInfo>& players)
{
	if (self.vehicle && !self.driving) {
		return 0.5f;
	}
	if (self.vehicle && self.driving) {
		float wait = 0;
		bot.idlePassenger = -1; // 0x1006b1f0: the passenger waited for, for BBPIdle2d
		for (const GameWorld::PlayerInfo& mate : players) {
			const auto it = m_bots.find(mate.id);
			if (mate.id == self.id || mate.team != self.team || !mate.alive || mate.vehicle || it == m_bots.end()
				|| it->second.vehicleTarget != self.vehicle) {
				continue;
			}
			const physics::Vec3 d = self.position - mate.position;
			if (horizontal2(d) > 40.0f * 40.0f) {
				continue;
			}
			const float facing = std::cos(angleBetween(mate.yaw, headingTo(mate.position, self.position)) * kRadians);
			const float w = facing >= 0.85f ? 70.0f : 30.0f;
			if (w > wait) {
				bot.idlePassenger = mate.id;
			}
			wait = std::max(wait, w);
		}
		if (wait > 0) {
			if (bot.waitingSince < 0) {
				bot.waitingSince = m_time;
			}
			if (m_time - bot.waitingSince < 10) {
				return wait;
			}
		} else {
			bot.waitingSince = -1;
		}
	}
	return weight;
}

// BBRevive's urgency (0x10082760): with a Revive weapon, the downed
// teammates within its maxRange score 1 - min(1, (d / speed) / time left),
// x10 for a human's; the best gets the bot: 100 x the squad factor of the
// heal task (AIDLL 0x10089aa0, task 1). A body less than 0.5 m over the
// water (the AI terrain's slot 0x2c) where it is more than 0.75 m deep
// (the water less the terrain, slot 4) is left out.
// (BBRevive's class: constructor 0x10082630, destructor 0x10082700.)
bool bodyUnderWater(float y, float water, float ground)
{
	return y < water + 0.5f && water - ground > 0.75f;
}

bool uprightOnGround(const physics::Vec3& up, const physics::Vec3& normal)
{
	return physics::dot(up, normal) >= 0.7071f; // 0x3f350481
}

bool sphereInRoomBox(const physics::Vec3& origin, const physics::Vec3& forward, float width, float height, float length,
	const physics::Vec3& at, float radius)
{
	physics::Vec3 ahead {forward.x, 0, forward.z};
	const float flat = std::sqrt(physics::lengthSquared(ahead));
	ahead = flat > 1e-6f ? ahead * (1 / flat) : physics::Vec3 {0, 0, 1};
	const physics::Vec3 right = physics::cross({0, 1, 0}, ahead);
	const physics::Vec3 d = at - origin;
	const float x = physics::dot(d, right), y = d.y, z = physics::dot(d, ahead);
	const float halfWidth = width * 1.2f * 0.5f, halfHeight = height * 1.2f * 0.5f;
	return x >= -halfWidth - radius && x <= halfWidth + radius && y >= -halfHeight - radius && y <= halfHeight + radius
		&& z >= length * -0.6f - radius && z <= length * 12.0f + radius;
}

// 0x1006c3e0 asks the information environment (DAT_101394b0, vtable
// 0x10119858 slot 0x2c, 0x100caf10) for the dynamic objects of grid 0 and
// of the bot's team's grid in the box, filtered by 0x1006c040 (vtable
// 0x101168b0): not the vehicle, not the bot's own object, ITMobile, not
// ITSoldier, not ITNaval. Room is none found. The seat object (the AG
// gunships' second positions, AS_AG_SecondPosition_AI / US_...: ITUnit
// ITAir) has no collision mesh of its own; its box and matrix are its
// vehicle's here (BF2142.exe's AIObjectPhysical box, 0x008a43f0, asserts
// the object has collision LOD 1, so the box is the hull's). A grid
// entry's radius is the AI object's r (vehicleBox).
bool BotManager::roomAhead(const GameWorld::VehicleInfo& vehicle, const GameWorld::PlayerInfo& self) const
{
	const physics::Vec3 size = vehicle.boxMax - vehicle.boxMin;
	for (const GameWorld::VehicleInfo& other : m_vehicles) {
		if (other.object == vehicle.object || other.object == self.vehicle) {
			continue;
		}
		const world::Object* object = m_world.objects().find(other.object);
		const uint32_t types
			= object && object->objectTemplate() ? m_settings.typeFlags(aiTemplateOf(object->objectTemplate()->name())) : 0;
		const int team = vehicleTeam(other.object);
		if (!(types & kITMobile) || (types & (kITSoldier | kITNaval)) || (team != 0 && team != self.team)) {
			continue;
		}
		if (sphereInRoomBox(vehicle.position, vehicle.forward, size.x, size.y, size.z, other.position, vehicleBox(other).radius)) {
			return false;
		}
	}
	return true;
}

bool BotManager::uprightOnTerrain(const physics::Vec3& at, const physics::Vec3& up) const
{
	physics::Vec3 normal {0, 1, 0};
	if (m_world.ground().heightAndNormal) {
		m_world.ground().heightAndNormal(at.x, at.z, normal);
	}
	return uprightOnGround(up, normal);
}

float BotManager::reviveUrgency(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players)
{
	if (self.vehicle) {
		return 0;
	}
	float range = 0;
	for (const auto& [slot, name] : self.carried) {
		const WeaponAi w = weaponAi(name);
		if (w.type == WeaponAi::Revive) {
			range = std::max(range, w.maxRange);
		}
	}
	if (range <= 0) {
		return 0;
	}
	float best = 0;
	int chosen = -1;
	for (const GameWorld::PlayerInfo& other : players) {
		if (!other.manDown || other.team != self.team || other.id == self.id) {
			continue;
		}
		const float distance = std::sqrt(physics::lengthSquared(other.position - self.position));
		if (distance > range) {
			continue;
		}
		if (float water = 0; m_world.waterLevel(water)
			&& bodyUnderWater(other.position.y, water, m_world.terrainHeight(other.position.x, other.position.z))) {
			continue;
		}
		// The time left before the body is gone (+0x50, filled by the game):
		// the respawn countdown; without one 15 s (sv.manDownTime's default).
		const float left = other.respawnIn > 0.001f ? other.respawnIn : 15.0f;
		float score = 1 - std::min(1.0f, distance / kSoldierSpeed / left);
		if (!other.ai) {
			score *= 10; // a human's body (its player not a bot, vt+0x84)
		}
		if (score > best) {
			best = score;
			chosen = other.id;
		}
	}
	if (chosen < 0) {
		return 0;
	}
	bot.target = chosen;
	GameWorld::PlayerInfo downed;
	m_world.playerInfo(chosen, downed);
	return 100 * coordination(self, 1, chosen, chosen, downed.position);
}

// A seat's value (0x100d3e70): health x (k x weapons + move), k 2 for a
// vehicle seat with an Armament plug-in, else 1.
//   - 0 for a seat another bot heads for (its reservation, +0x2c/+0x30),
//     one (when `other`) whose driver's seat is taken, or a vehicle with
//     an enemy aboard; a vehicle below 0.3 of its health (0.09 the one
//     the bot drives); the root's seat of a vehicle that isn't ITAir
//     within 7 s of a human player of the bot's team getting out of it
//     (the unit's per-team stamp, vt+0x60; set by the game's exit event,
//     BF2142.exe 0x00894860 -> vt+0x64 0x008a1b10, for players that are
//     not bots). The original also refuses one more than 8 m over the
//     terrain (not here).
//   - weapons (0x100d3b80): for each of the seat's guns (on foot the
//     kit's), its strength against each object the bot knows (by the
//     object's CombatStrengthType), plus at a vehicle's seat its eight
//     strengths summed; then, when not the bot's own soldier and not 0,
//     +4 for light armour, naval, helicopters and planes, +10 for heavy
//     armour (+2 / +5 with setHasExposedSoldier).
//   - move (0x100d39a0) x 0.3, when the root has a Mobile plug-in: its
//     maxSpeed x 0.5 x 1.5 at the root's seat; at another seat x 5 behind
//     a driver of the bot's squad, x 1 of its team, and 0 without a
//     driver.
//   - x 1.15 for the seat the bot reserved itself.
// Transport (0x100d3730, 0x1005d840; orderClose): while the bot's order is
// close, the move isn't counted, nor the weapons when the vehicle is
// outside the area's control point. At the root's seat it only stays close
// when the order's spot for the vehicle's map lies outside the point, or
// the bot is under threat and the seat unarmed.
float BotManager::seatValue(const Bot& bot, const GameWorld::PlayerInfo& self, uint32_t vehicle, int seat, bool other) const
{
	const bool soldier = vehicle == 0;
	if (!soldier && other && !self.vehicle && vehicle == bot.abandonedVehicle && m_time - bot.abandonedAt < 30) {
		return 0;
	}
	const float health = healthOf(soldier ? self.soldier : vehicle);
	std::vector<std::string> guns;
	std::string rootAi, seatAi;
	bool driverTaken = false;
	int driverPlayer = -1;
	if (soldier) {
		for (const auto& [slot, name] : self.carried) {
			guns.push_back(name);
		}
		rootAi = seatAi = "";
	} else {
		// Reserved by another bot (its whole vehicle, or this seat): not the
		// seat the bot sits in already (a passenger heading for the
		// vehicle would otherwise turn its driver out).
		const bool sitting = self.vehicle == vehicle && self.seat == seat;
		for (const auto& [id, other] : m_bots) {
			if (!sitting && id != self.id && other.vehicleTarget == vehicle
				&& (other.seatTarget < 0 || other.seatTarget == seat)) {
				return 0;
			}
		}
		const world::Object* object = m_world.objects().find(vehicle);
		rootAi = object && object->objectTemplate() ? aiTemplateOf(object->objectTemplate()->name()) : "";
		const std::vector<GameWorld::SeatLayout> seats = const_cast<GameWorld&>(m_world).vehicleSeats(vehicle);
		const std::vector<int> occupants = m_world.vehicleOccupants(vehicle);
		for (const int occupant : occupants) {
			GameWorld::PlayerInfo aboard;
			if (occupant >= 0 && m_world.playerInfo(occupant, aboard) && aboard.team != self.team) {
				return 0; // the Unit's team is not the bot's
			}
		}
		driverTaken = !occupants.empty() && occupants[0] >= 0 && occupants[0] != self.id;
		driverPlayer = driverTaken ? occupants[0] : -1;
		const bool seatTaken = static_cast<size_t>(seat) < occupants.size() && occupants[static_cast<size_t>(seat)] >= 0
			&& occupants[static_cast<size_t>(seat)] != self.id;
		if (other && (seat == 0 ? driverTaken : seatTaken)) {
			return 0;
		}
		const float floor = kVehicleFloor * (seat == 0 && vehicle == self.vehicle && self.seat == 0 ? 0.3f : 1.0f);
		if (health < floor) {
			return 0;
		}
		// The root's seat (a primary AI object, vt+0xec) of a vehicle that
		// isn't ITAir: not within 7 s of a human player of the bot's team
		// getting out of it (the unit's per-team stamp, vt+0x60).
		if (seat == 0 && !(m_settings.typeFlags(rootAi) & kITAir)) {
			const auto stamp = m_humanLeftAt.find(vehicle);
			if (stamp != m_humanLeftAt.end() && self.team >= 1 && self.team <= 2
				&& m_time - stamp->second[static_cast<size_t>(self.team)] < 7) {
				return 0;
			}
		}
		if (static_cast<size_t>(seat) < seats.size()) {
			seatAi = aiTemplateOf(seats[static_cast<size_t>(seat)].name);
			if (!seats[static_cast<size_t>(seat)].weapon.empty()) {
				guns.push_back(seats[static_cast<size_t>(seat)].weapon);
			}
		}
	}
	const AiPlugin* mobile = soldier ? nullptr : m_settings.plugin(rootAi, "Mobile");
	const bool armament = !soldier && (m_settings.plugin(seatAi.empty() ? rootAi : seatAi, "Armament") || !guns.empty());
	// Transport (0x100d3730, 0x1005d840): see orderClose.
	bool close = false, inArea = false, noMove = false;
	const GameWorld::VehicleInfo* info = nullptr;
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		info = v.object == vehicle ? &v : info;
	}
	if (!soldier && info) {
		close = orderClose(bot, self, *info, rootAi, inArea, noMove);
		if (close && seat == 0) {
			// The root's seat: still close when the order's spot on the
			// vehicle's map lies outside the area's control point (with
			// the area flagged ControlPoint, 0x1005d840: farther than its
			// radius + the vehicle's), else only when the bot is under
			// threat (vt+0x9c) and the seat has no Armament part (+0x34).
			bool outside = false;
			const auto& areas = m_settings.strategy.areas;
			if (mobile && bot.goalOrder >= 0 && static_cast<size_t>(bot.goalOrder) < areas.size()) {
				const ai::StrategicArea& area = areas[static_cast<size_t>(bot.goalOrder)];
				// The template's map by name (IAIPathfindingCourier slot 0x18
				// 0x10055020 -> 0x10040570; the level's table 0x100442f0 / 0x100441e0).
				const int map = lower(mobile->pathfindingMap) == "vehicle" ? 1 : 0;
				for (const GameWorld::ControlPointInfo& point : m_world.controlPointList()) {
					if (area.controlPointFlag && point.id == area.controlPoint) {
						const physics::Vec3 spot = area.hasOrderPosition[map]
							? physics::Vec3 {area.orderPosition[map][0], area.orderPosition[map][1], area.orderPosition[map][2]}
							: point.position;
						outside = std::sqrt(physics::lengthSquared(spot - point.position)) - vehicleBox(*info).radius > point.radius;
					}
				}
			}
			close = outside || (threatened(bot, self) && !armament);
		}
	}
	// Weapons.
	float weapons = 0;
	for (const std::string& name : (close && !inArea) ? std::vector<std::string> {} : guns) {
		const WeaponAi w = weaponAi(name);
		if (!soldier && mobile) {
			for (const float strength : w.strength) {
				weapons += strength;
			}
		}
		for (const Known& known : bot.known) {
			GameWorld::PlayerInfo target;
			if (known.everSeen && m_world.playerInfo(known.player, target)) {
				weapons += w.strength[static_cast<size_t>(std::clamp(strengthTypeOf(target), 0, 7))];
			}
		}
	}
	if (!soldier && weapons != 0) {
		const AiPlugin* physical = m_settings.plugin(rootAi, "Physical");
		const AiPlugin* unit = m_settings.plugin(seatAi.empty() ? rootAi : seatAi, "Unit");
		const bool exposed = unit && unit->exposedSoldier;
		const int type = physical ? physical->strengthType : -1;
		if (type == 1 || type == 3 || type == 4 || type == 5) {
			weapons += exposed ? 2.0f : 4.0f;
		} else if (type == 2) {
			weapons += exposed ? 5.0f : 10.0f;
		}
	}
	// Move.
	float move = 0;
	if (soldier) {
		move = kSoldierSpeed * 1.5f * 0.5f * 0.3f; // SoldierMobile's maxSpeed, the root's factor
	} else if (mobile && !close && !noMove) {
		float f = 1.5f;
		if (seat != 0) {
			f = 0;
			GameWorld::PlayerInfo driver;
			if (driverPlayer >= 0 && m_world.playerInfo(driverPlayer, driver) && driver.team == self.team) {
				f = driver.squad != 0 && driver.squad == self.squad ? 5.0f : 1.0f;
			}
		}
		move = mobile->maxSpeed * f * 0.5f * 0.3f;
	}
	float value = (armament ? 2.0f : 1.0f) * weapons + move;
	if (!soldier && bot.vehicleTarget == vehicle && (bot.seatTarget < 0 || bot.seatTarget == seat)) {
		value *= 1.15f;
	}
	return health * value;
}

// 0x100d3730. For a ground or naval vehicle (not ITSoldier, ITAir or
// ITHelicopter) with a Mobile part, when the bot's order lies in an area
// (the squad's, squad +0x28) whose control point can change hands
// (0x1005d8f0: IAIGame vt+0x24's flag, not unableToChangeTeam) and that the
// bot's team doesn't hold (+0x70 + team x 4 not 0): `inArea` whether the
// vehicle touches the point's radius (its radius + the point's). For a
// vehicle the bot isn't in, the order is far (false, `noMove` cleared)
// when the bot's root (squad member +0x40) is 50 m or more from the area's
// infantry order spot, the vehicle as far from its vehicle spot, and the
// bot 70 m or more away or the vehicle no farther than it (squared, 2500
// and 4900); otherwise it is close (true, `noMove` set). For the vehicle the
// bot is in, close within 5 m of its vehicle spot. The spots are the
// area's order positions (aiStrategicArea.setOrderPosition), the control
// point without one.
bool BotManager::orderClose(const Bot& bot, const GameWorld::PlayerInfo& self, const GameWorld::VehicleInfo& vehicle,
	const std::string& rootAi, bool& inArea, bool& noMove) const
{
	inArea = false;
	const uint32_t types = m_settings.typeFlags(rootAi);
	if ((types & (kITSoldier | kITAir | kITHelicopter)) || !m_settings.plugin(rootAi, "Mobile")) {
		return false;
	}
	const auto& areas = m_settings.strategy.areas;
	if (bot.goalOrder < 0 || static_cast<size_t>(bot.goalOrder) >= areas.size()) {
		return false;
	}
	const ai::StrategicArea& area = areas[static_cast<size_t>(bot.goalOrder)];
	for (const GameWorld::ControlPointInfo& point : m_world.controlPointList()) {
		if (point.id != area.controlPoint || !point.capturable || point.team == self.team) {
			continue;
		}
		const auto spot = [&](int map) {
			return area.hasOrderPosition[map]
				? physics::Vec3 {area.orderPosition[map][0], area.orderPosition[map][1], area.orderPosition[map][2]}
				: point.position;
		};
		const physics::Vec3 vehicleSpot = spot(1), infantrySpot = spot(0);
		const float reach = vehicleBox(vehicle).radius + point.radius;
		inArea = physics::lengthSquared(vehicle.position - point.position) < reach * reach;
		// The bot's root: on foot, or its vehicle (the soldier rides with it).
		return transportClose(self.vehicle == vehicle.object, physics::lengthSquared(self.position - infantrySpot),
			physics::lengthSquared(vehicleSpot - vehicle.position), noMove);
	}
	return false;
}

// 0x100ebdd0's +0x7c: over the sightings the bot remembers, the enemy's
// strength against the bot's own type (+0x40 vt+0x40), decaying with age
// but never to 0; above 0 when one of them carries (or mans) a weapon
// with strength against it.
bool BotManager::threatened(const Bot& bot, const GameWorld::PlayerInfo& self) const
{
	const size_t mine = static_cast<size_t>(std::clamp(strengthTypeOf(self), 0, 7));
	for (const Known& known : bot.known) {
		GameWorld::PlayerInfo enemy;
		if (!known.everSeen || !m_world.playerInfo(known.player, enemy) || enemy.team == self.team) {
			continue;
		}
		std::vector<std::string> weapons;
		for (const auto& [slot, name] : enemy.carried) {
			weapons.push_back(name);
		}
		if (enemy.vehicle) {
			const std::vector<GameWorld::SeatLayout> seats = const_cast<GameWorld&>(m_world).vehicleSeats(enemy.vehicle);
			if (enemy.seat >= 0 && static_cast<size_t>(enemy.seat) < seats.size()) {
				weapons.push_back(seats[static_cast<size_t>(enemy.seat)].weapon);
			}
		}
		for (const std::string& name : weapons) {
			if (!name.empty() && weaponAi(name).strength[mine] > 0) {
				return true;
			}
		}
	}
	return false;
}

// BBChange's urgency (0x1006c520). The current seat: on foot 1.2 x its
// value; in a vehicle 2 x the seat's value (250 at the driver's seat
// with others aboard), and the way out is the soldier's own value, or
// 10000 when the vehicle is worth nothing (a wreck) or stands more than
// 4 m off the vehicle mesh (0x100880e0). The vehicles within 75 m
// (IAIInformationEnvironment 0x100ca9e0; one without a Mobile plug-in
// within 75/sqrt(2) m) on the vehicle mesh, each seat at its value x the
// distance factor max(0, min(0.5, (R^2 - 0.8 d^2) / R^2) + 0.5), x5 for
// a soldier within 20 m below a quarter of its health. Urgency: best /
// current x weight x 10 (best x weight when the current is worth
// nothing), divided by 1 + 14 clamp((d - 10) / 60, 0, 1) for the distance
// to it (0x1006bc50). Another vehicle's seats other than the root's are
// weighed by 0x1006b910: 1 when its driver's seat is taken, else 0.77 for
// an ITGround root and 0.5 for an ITAir one with a Mobile part, not
// ITNaval (the flag bits 3 and 1: ai::InformationType, aiTemplate.addType).
// The radio (the comm's lists, each read at its first message from a
// human alive and not down: heardFromHuman):
//   - bail_out (type 0): in a vehicle the way out x the message's
//     modifier x 100 (10000 in an ITAir object), and no other vehicle is
//     weighed;
//   - get_in (type 1), on foot: a vehicle of the bot's team (0x1006c080:
//     its team, or nobody aboard, 0x100d3530) the sender of one of the
//     messages rides (any seat) is weighed x that modifier; and when the
//     first sender rides the vehicle the bot already heads for (+0x2c),
//     only that one counts, x5.
// From a vehicle the other vehicles are weighed too unless the way out is
// already the best, a bail_out is held, the bot's object is ITAir or it is
// off the mesh; the bot's own vehicle is left out. A driver of a Mobile
// root (and a soldier) takes one with UseNoPathfindingToGetToObject
// (Unit +0x15, 0x00884eaf), or one without a Mobile part, or one whose
// spot on the infantry map lies within its radius + the bot's of it (the
// Mobile's per-map points +8/+0x1c/+0x30, which the rewrite doesn't keep:
// every vehicle counts); a passenger takes any. The get_in answers are
// below. With the way out chosen and the vehicle's health at or under
// 0.4 the bot says bail_out (see below). A seat whose AI object is ITAir
// but not ITHelicopter (the AG gunships' second positions) also needs room
// ahead to be chosen (roomAhead, 0x1006c3e0).
// (BBChange, vtable 0x101168c4: constructor 0x1006c1b0, destructor
// 0x1006c2b0.)
float BotManager::changeUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight)
{
	constexpr float kReach = 75; // 0x42960000
	const std::vector<GameWorld::VehicleInfo>& vehicles = m_vehicles;
	const Bot::Radio* bailOut = heardFromHuman(bot, 0);
	const Bot::Radio* getIn = heardFromHuman(bot, 1);
	const uint32_t reserved = bot.vehicleTarget; // the last choice (+0x2c)
	const auto rootAiOf = [&](uint32_t vehicle) {
		const world::Object* object = m_world.objects().find(vehicle);
		return object && object->objectTemplate() ? aiTemplateOf(object->objectTemplate()->name()) : std::string();
	};
	float current = 0, best = 0;
	uint32_t chosen = 0;
	int chosenSeat = -1; // -2: out
	float chosenDistance = 0;
	bool weighOthers = true;
	physics::Vec3 from = self.position;
	if (self.vehicle) {
		// In a vehicle (0x1006c520 with the bot's object not a soldier). The
		// driver's current value is 2 x its seat's (250 with others aboard);
		// a passenger starts from the driver's seat's value (as another's).
		// Unless it drives an ITAir root, the root's other seats follow, each
		// x 0x1006b910's factor (1 for a passenger with the driver's seat
		// taken; else 0.77 ITGround, 0.5 ITAir with Mobile, not ITNaval), the
		// passenger's own one as its current value (x2), the others as
		// another's; then the way out (above). The best that isn't the bot's
		// seat is the change.
		bot.vehicleTarget = 0;
		bot.seatTarget = -1;
		const GameWorld::VehicleInfo* own = nullptr;
		for (const GameWorld::VehicleInfo& vehicle : vehicles) {
			own = vehicle.object == self.vehicle ? &vehicle : own;
		}
		if (!own) {
			return 0;
		}
		from = own->position;
		bool offMesh = m_vehicleNavMesh && m_vehicleNavMesh->findPolygon(toAi(own->position), 4.0f) < 0;
		const std::vector<int> occupants = m_world.vehicleOccupants(own->object);
		const int aboard = static_cast<int>(std::count_if(occupants.begin(), occupants.end(), [](int p) { return p >= 0; }));
		const bool driving = self.seat == 0;
		const std::string rootAi = rootAiOf(own->object);
		const uint32_t types = m_settings.typeFlags(rootAi);
		const bool mobile = m_settings.plugin(rootAi, "Mobile") != nullptr;
		const bool driverTaken = !occupants.empty() && occupants[0] >= 0 && occupants[0] != self.id;
		// The bot's object: the root for the driver, else its seat's part.
		uint32_t objectTypes = types;
		if (!driving) {
			const std::vector<GameWorld::SeatLayout> seats = m_world.vehicleSeats(own->object);
			objectTypes = static_cast<size_t>(self.seat) < seats.size()
				? m_settings.typeFlags(aiTemplateOf(seats[static_cast<size_t>(self.seat)].name)) : 0;
		}
		// 0x1006c520 runs that test (0x100880e0, its 10000) only for an
		// object that isn't ITAir (the bot object's flags bit 1): a pilot
		// over the vehicle mesh's reach stays.
		if (objectTypes & kITAir) {
			offMesh = false;
		}
		chosen = own->object;
		chosenSeat = self.seat;
		if (driving) {
			current = own->working ? (aboard > 1 ? 250.0f : seatValue(bot, self, own->object, 0, false)) * 2.0f : 0.0f;
			best = current;
		} else {
			best = own->working ? seatValue(bot, self, own->object, 0, true) : 0.0f;
			chosenSeat = 0;
		}
		if (own->working && !(driving && (types & kITAir))) {
			const float factor = otherSeatFactor(!driving && driverTaken, mobile, types);
			for (size_t seat = 1; seat < occupants.size(); ++seat) {
				if (static_cast<int>(seat) == self.seat) {
					current = std::max(0.0f, 2.0f * seatValue(bot, self, own->object, self.seat, false) * factor);
					if (current > best) { // its own seat is weighed with the rest
						best = current;
						chosenSeat = self.seat;
					}
					continue;
				}
				const float value = seatValue(bot, self, own->object, static_cast<int>(seat), true) * factor;
				if (value > best) {
					best = value;
					chosenSeat = static_cast<int>(seat);
				}
			}
		}
		float leave = seatValue(bot, self, 0, 0, false);
		if (bailOut) {
			leave *= bailOut->modifier * 100;
			if (objectTypes & kITAir) {
				leave = 10000;
			}
		}
		if (current < 0.001f || offMesh) {
			leave = 10000;
		}
		if (leave > best) {
			best = leave;
			chosenSeat = -2;
		}
		weighOthers = chosenSeat != -2 && !bailOut && !(objectTypes & kITAir) && !offMesh;
	} else {
		current = 1.2f * seatValue(bot, self, 0, 0, false);
		best = current;
	}
	// The get_in sender's vehicle when it is the one the bot heads for
	// (+local_e: only that one, x5).
	bool onlyReserved = false;
	if (!self.vehicle && getIn && reserved) {
		GameWorld::PlayerInfo sender;
		onlyReserved = m_world.playerInfo(getIn->sender, sender) && sender.vehicle == reserved;
	}
	const float myHealth = healthOf(self.soldier);
	for (const GameWorld::VehicleInfo& vehicle : weighOthers ? vehicles : std::vector<GameWorld::VehicleInfo> {}) {
		if (!vehicle.working || vehicle.freeSeats <= 0 || vehicle.object == self.vehicle
			|| (onlyReserved && vehicle.object != reserved)) {
			continue;
		}
		const float d2 = horizontal2(vehicle.position - from);
		const std::string rootAi = rootAiOf(vehicle.object);
		const bool mobile = !rootAi.empty() && m_settings.plugin(rootAi, "Mobile");
		if (d2 > kReach * kReach || (!mobile && d2 > kReach * kReach * 0.5f)) {
			continue;
		}
		if (m_vehicleNavMesh && mobile && m_vehicleNavMesh->findPolygon(toAi(vehicle.position)) < 0) {
			continue; // not on the vehicle mesh
		}
		float f = std::max(0.0f, std::min(0.5f, (kReach * kReach - 0.8f * d2) / (kReach * kReach)) + 0.5f);
		if (onlyReserved) {
			f *= 5;
		}
		if (!self.vehicle && myHealth < 0.25f && d2 < 400) {
			f *= 5;
		}
		const std::vector<int> occupants = m_world.vehicleOccupants(vehicle.object);
		// A get_in sender aboard (0x1006c080): its modifier.
		if (!self.vehicle && getIn) {
			for (const Bot::Radio& r : bot.radio) {
				if (r.type != 1 || m_time - r.at >= m_settings.radioLifeOf(1)
					|| std::find(occupants.begin(), occupants.end(), r.sender) == occupants.end()) {
					continue;
				}
				GameWorld::PlayerInfo sender;
				if (m_world.playerInfo(r.sender, sender) && sender.team == self.team) {
					f *= getIn->modifier;
					break;
				}
			}
		}
		// Every seat of its layout (the occupants' list only grows as
		// players get in).
		const size_t seats = std::max<size_t>({1, occupants.size(), m_world.vehicleSeats(vehicle.object).size()});
		// The other seats' factor (0x1006b910, for another vehicle): 1 with
		// its driver's seat taken (0x100aa530); else, for a root with a
		// Mobile part that isn't ITNaval, 0.77 for ITGround, 0.5 for ITAir.
		const uint32_t types = m_settings.typeFlags(rootAi);
		const float others = otherSeatFactor(!occupants.empty() && occupants[0] >= 0, mobile, types);
		const std::vector<GameWorld::SeatLayout> layout = m_world.vehicleSeats(vehicle.object);
		for (size_t seat = 0; seat < seats; ++seat) {
			const float value = seatValue(bot, self, vehicle.object, static_cast<int>(seat), true) * f * (seat > 0 ? others : 1.0f);
			// The seat's AI object's types (its own aiTemplate, else the root's).
			const std::string seatAi = seat < layout.size() ? aiTemplateOf(layout[seat].name) : std::string();
			const uint32_t seatTypes = seatAi.empty() ? (seat == 0 ? types : 0) : m_settings.typeFlags(seatAi);
			const bool fixedWing = (seatTypes & kITAir) && !(seatTypes & kITHelicopter);
			if (value > best && (!fixedWing || roomAhead(vehicle, self))) {
				best = value;
				chosen = vehicle.object;
				chosenSeat = -1;
				chosenDistance = std::sqrt(physics::lengthSquared(vehicle.position - from));
			}
		}
	}
	if (self.vehicle) {
		// The way out chosen with the bot's object (its vehicle) at or under
		// the bot manager's vehicle floor + 0.1 (slot 0x68, 0x100c5cd0: its
		// +0xdc, 0.3 from the constructor 0x100c9c50, the floor the seat
		// values use): "bail_out" at once to those aboard (AIDLL 0x100a5990: type
		// 0, the AI's channel 7, TCVehicle).
		if (chosenSeat == -2 && healthOf(self.vehicle) <= kVehicleFloor + 0.1f) {
			sendRadioNow(bot, self, 0, 7);
		}
		if ((chosen == self.vehicle && chosenSeat == self.seat) || best <= 0) {
			return 0;
		}
		if (chosenSeat >= 0) {
			bot.vehicleTarget = self.vehicle; // the seat the bot heads for (reserved: +0x2c/+0x30)
			bot.seatTarget = chosenSeat;
		} else if (chosenSeat == -1) {
			bot.vehicleTarget = chosen; // another vehicle: out, then to it
		}
	} else {
		// The get_in answers (on foot, with a get_in held whose first
		// sender rides a vehicle): a choice that isn't the sender's vehicle,
		// or none, answers "negative" when the bot is within 15 m of that
		// vehicle (0x1006be70: 0x100a61c0 marks the get_ins answered; the
		// AI's channel 6, TCLocalTeam, 1 s on); heading for the sender's
		// vehicle answers "roger_that" (the plan 0x1006dac0 does as it sets
		// off, its choice new: the same channel and delay).
		const uint32_t newChoice = best <= current ? 0 : chosen;
		GameWorld::PlayerInfo sender;
		if (getIn && m_world.playerInfo(getIn->sender, sender) && sender.vehicle) {
			if (newChoice == sender.vehicle) {
				if (reserved != newChoice && answerRadio(bot, 1)) {
					queueRadio(bot, 6, 6, 1);
				}
			} else if (std::any_of(bot.radio.begin(), bot.radio.end(), [](const Bot::Radio& r) { return r.type == 1 && !r.answered; })) {
				const GameWorld::VehicleInfo* ridden = nullptr;
				for (const GameWorld::VehicleInfo& v : vehicles) {
					ridden = v.object == sender.vehicle ? &v : ridden;
				}
				if (answerRadio(bot, 1) && ridden && physics::lengthSquared(ridden->position - self.position) < 225) {
					queueRadio(bot, 7, 6, 1);
				}
			}
		}
		if (best <= current || !chosen) {
			bot.vehicleTarget = 0;
			return 0;
		}
		bot.vehicleTarget = chosen;
	}
	const float urgency = current < 0.001f ? best * weight : best / current * weight * 10;
	return urgency / (1 + 14 * std::clamp((chosenDistance - 10) / 60, 0.0f, 1.0f));
}

// BBFire3d's urgency (behaviour vtable 0x10116d40 slot 9, 0x1007e1b0; the
// gunships' Fire) with its target score 0x1007d6c0. Over the targets the
// bot knows (memory flags 6) and then those the information environment
// lists (DAT_101394b0 slot 0x18; here every enemy the bot knows; its
// per-team static and dynamic grids: constructor 0x100cb4a0, destructor
// 0x100cb6c0 / 0x100cb1e0 / 0x100cb280, slots 0x100cab50, 0x100cabd0,
// 0x100cae50, 0x100caeb0, 0x100caf10, 0x100caf80, 0x100cafc0, 0x100cb000,
// 0x100cb040, 0x100cb080, 0x100cb0c0, 0x100cb130, 0x100cb1a0, 0x100cb290,
// 0x100cb340, 0x100cb3f0), each
// scores:
//   score = facing x reach x 0.5 x temperature x motion x freshness x
//           strength x recency
//   - none for a teammate (the target's +0x40 team), a target without a
//     Physical part, ITNoTemperature (flag 11) or with no health left;
//   - strength: the best of the vehicle's weapons (with rounds: slot 0xc
//     -1 or over 0) against the target's CombatStrengthType (AIDLL 0x100a41f0),
//     none: no score; maxRange the largest of theirs (0x100a4170);
//   - recency: 1.5 - 0.5 clamp((now - t) / 30) after the target last hurt
//     the bot (bot vt+0x30c; here a round of it passing, the alert), else 1;
//   - freshness: 1 / (0.05 (now - lost) + 1) once out of sight, 1 in it;
//   - motion: for a target that isn't ITAir, 1 without a Mobile part,
//     1 / (0.5 |v| + 1) within maxRange, beyond it 0.25 when it is no more
//     than 5 m/s faster than the bot (the Mobile maxSpeeds, 0x100a4350),
//     else 0.25 / (2 (closing along + closing across) + 0.5 |v| + 1); for
//     an ITAir target with anti-air guns (the armament's slot 0x28; here a
//     weapon strength against helicopters) the same once the bot is ITAir
//     or 0.9 maxRange away, else 1; without them none over 5 m up (the
//     target's +0x34) and the same only within 0.5 maxRange under 15 m/s;
//   - temperature: the target's units' temperatures (each seat's per-team
//     value + the table's for its type + its basic temperature, x the
//     strategy's modifier for the type, x 0.75 outside the bot's area);
//     here 20 a vehicle, 1 a soldier (the strategy's), x 0.75 out of the
//     bot's order area;
//   - reach: 1 - min(1, d / 3 maxRange); for an ITAir target max(0, 1 -
//     d / 1.5 maxRange) and the temperature doubled with anti-air guns;
//   - facing: max(0.5, 1 + the bot's forward . the way to it).
// The current target stays unless another scores 1.2 times more; the
// urgency is fuzzy(2 best) x the weight.
//   - unit count (0x1007d6c0's end against 0x100d2b30, the bot's +0x24, 1
//     for every bot): a target whose units (the vehicle's root and seats,
//     AIDLL FUN_100a8cb0's walk) sum battle strengths (the Unit part's +8 at the
//     bot's own type, its Physical's slot 0x40) of at most 1 scores x 0.01
//     (fire3dUnitScale): a gunship's soldiers, battle strength 0 against
//     Helicopter, count for little;
//   - an enemy aircraft whose bot pilot targets this bot (its slot 0x74
//     against this one's 0xa8) is passed over, in both loops;
//   - when the current target isn't among the memory's items, a second
//     loop over the manned enemy units the side knows (the information
//     environment's slot 0x18, 0x100caab0 with -1: within 75 m, 600 m for
//     an ITAir bot, of the side's grid, filtered by 0x100ca960: a Unit part,
//     another team, manned by 0x100d3530) scores them with a freshness of 1;
//   - the "20 s a failed target is passed over" (+0x30, 0x1007df70) never
//     happens: the map's only accessor is that lookup, which makes a
//     missing stamp at -10000 and is never written (BBFire's 0x10075ec0 the
//     same); every 60 s (+0x28) dead entries are purged.
// Left out: the Unit's capability flags (0x100cf5c0 / AIDLL 0x100cf5a0, bot
// vt+0x180).
float BotManager::fire3dUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight,
	const std::vector<GameWorld::PlayerInfo>& players)
{
	const GameWorld::VehicleInfo* vehicle = nullptr;
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		vehicle = v.object == self.vehicle ? &v : vehicle;
	}
	if (!vehicle) {
		return fireUrgency(bot, self, players);
	}
	const auto seats = m_world.vehicleSeats(self.vehicle);
	std::vector<WeaponAi> guns;
	for (const GameWorld::SeatLayout& seat : seats) {
		if (!seat.weapon.empty()) {
			guns.push_back(weaponAi(seat.weapon));
		}
	}
	const auto aiOf = [this](uint32_t object) {
		const world::Object* o = m_world.objects().find(object);
		return o && o->objectTemplate() ? aiTemplateOf(o->objectTemplate()->name()) : std::string();
	};
	const std::string ownAi = aiOf(self.vehicle);
	const bool ownAir = (m_settings.typeFlags(ownAi) & kITAir) != 0;
	const AiPlugin* ownMobile = m_settings.plugin(ownAi, "Mobile");
	const float ownMaxSpeed = ownMobile ? ownMobile->maxSpeed : 0.0f;
	// The motion test and the doubled temperature (0x1007d6c0) take the
	// bot's Armament plug-in's setIsAntiAircraft != 0 (its seat's, else
	// the vehicle's; AI object +0x34 slot 10, BF2142.exe 0x008a7460). Runs
	// without the AI scripts: a gun with a helicopter strength.
	const AiPlugin* armament = nullptr;
	if (self.seat >= 0 && static_cast<size_t>(self.seat) < seats.size()) {
		armament = m_settings.plugin(aiTemplateOf(seats[static_cast<size_t>(self.seat)].name), "Armament");
	}
	if (!armament) {
		armament = m_settings.plugin(ownAi, "Armament");
	}
	bool antiAir = armament && armament->antiAircraft != 0;
	float maxRange = 0;
	for (const WeaponAi& w : guns) {
		antiAir = antiAir || (!armament && w.strength[4] > 0);
		maxRange = std::max(maxRange, w.maxRange);
	}
	if (maxRange <= 0) {
		maxRange = 300;
	}
	physics::Vec3 areaAt;
	float areaRadius = 0;
	int areaOwner = 0;
	const bool hasArea = bot.hasGoal && bot.goalOrder >= 0 && areaPosition(bot.goalOrder, areaAt, areaRadius, areaOwner);
	const auto inArea = [&](const physics::Vec3& p) { return !hasArea || horizontal2(p - areaAt) <= areaRadius * areaRadius; };
	const float areaFactor = inArea(self.position) ? 1.0f : 0.75f;
	const int ownType = std::clamp(strengthTypeOf(self), 0, 7);
	// An enemy aircraft whose bot pilot targets this one: passed over.
	const auto pilotTargetsMe = [&](const GameWorld::PlayerInfo& other) {
		if (!other.vehicle || !(m_settings.typeFlags(aiOf(other.vehicle)) & kITAir)) {
			return false;
		}
		const std::vector<int> aboard = m_world.vehicleOccupants(other.vehicle);
		const auto pilot = aboard.empty() ? m_bots.end() : m_bots.find(aboard[0]);
		return pilot != m_bots.end() && pilot->second.target == self.id;
	};
	const auto scoreOf = [&](const GameWorld::PlayerInfo* other, const physics::Vec3& position, float freshness,
							 float alertAt) -> float {
		const int type = std::clamp(strengthTypeOf(*other), 0, 7);
		float strength = 0;
		for (const WeaponAi& w : guns) {
			strength = std::max(strength, w.strength[static_cast<size_t>(type)]);
		}
		if (guns.empty()) {
			strength = 1; // no weaponTemplates (the tests)
		}
		if (strength <= 0) {
			return 0.0f;
		}
		const physics::Vec3 to = position - self.position;
		const float d = std::max(0.5f, std::sqrt(physics::lengthSquared(to)));
		const physics::Vec3 dir = to * (1 / d);
		const float recency = m_time - alertAt < 1e8f ? 1.5f - 0.5f * std::clamp((m_time - alertAt) / 30.0f, 0.0f, 1.0f) : 1.0f;
		const std::string targetAi = other->vehicle ? aiOf(other->vehicle) : std::string();
		const bool targetAir = other->vehicle && (m_settings.typeFlags(targetAi) & kITAir) != 0;
		const AiPlugin* targetMobile = other->vehicle ? m_settings.plugin(targetAi, "Mobile") : nullptr;
		const physics::Vec3 v = other->velocity;
		const float speed = std::sqrt(physics::lengthSquared(v));
		const auto motionOf = [&]() {
			if (other->vehicle && !targetMobile) {
				return 1.0f;
			}
			if (d <= maxRange) {
				return 1 / (0.5f * speed + 1);
			}
			const float theirs = targetMobile ? targetMobile->maxSpeed : kSoldierSpeed;
			if (theirs - ownMaxSpeed <= 5) {
				return 0.25f;
			}
			const physics::Vec3 side {dir.z, 0, -dir.x}; // (0, 1, 0) x dir
			const float along = std::max(0.0f, physics::dot(dir, v) - physics::dot(dir, vehicle->velocity));
			const float across = std::max(0.0f, physics::dot(side, v) - physics::dot(side, vehicle->velocity));
			return 0.25f / (2 * (along + across) + 0.5f * speed + 1);
		};
		float motion = 1;
		if (!targetAir) {
			motion = motionOf();
		} else if (antiAir) {
			motion = ownAir || d >= 0.9f * maxRange ? motionOf() : 1.0f;
		} else {
			if (other->position.y - m_world.terrainHeight(other->position.x, other->position.z) > 5) {
				return 0.0f;
			}
			motion = d <= 0.5f * maxRange && speed <= 15 ? motionOf() : 0.0f;
		}
		float temperature = (other->vehicle ? 20.0f : 1.0f) * areaFactor;
		if (ownMobile && !ownAir && !inArea(other->position)) {
			temperature *= 0.75f;
		}
		float reach = 1 - std::min(1.0f, d / (3 * maxRange));
		if (targetAir) {
			reach = std::max(0.0f, 1 - d / (1.5f * maxRange));
			if (antiAir) {
				temperature *= 2;
			}
		}
		const float facing = std::max(0.5f, physics::dot(dir, vehicle->forward) + 1);
		const float score = facing * reach * 0.5f * temperature * motion * freshness * strength * recency;
		const std::array<float, 8> units = battleArray(*other);
		// Without the AI scripts (the tests) no unit has strengths: not scaled.
		return m_settings.aiTemplates.empty() ? score : score * fire3dUnitScale(units[static_cast<size_t>(ownType)]);
	};
	float best = 0, currentScore = 0;
	int bestTarget = -1;
	bool currentInMemory = false;
	for (const Known& known : bot.known) {
		if (!known.everSeen) {
			continue;
		}
		const auto other = std::find_if(players.begin(), players.end(), [&](const auto& p) { return p.id == known.player; });
		if (other == players.end() || !other->alive || other->team == self.team || pilotTargetsMe(*other)) {
			continue;
		}
		const float freshness = known.lostAt < 0 ? 1.0f : 1.0f / ((m_time - known.lostAt) * 0.05f + 1);
		const float score = scoreOf(&*other, known.position, freshness, known.alertAt);
		if (known.player == bot.target) {
			currentScore = score;
			currentInMemory = true;
		}
		if (score > best) {
			best = score;
			bestTarget = known.player;
		}
	}
	if (!currentInMemory) {
		// The side's manned units within 75 m (600 m for an ITAir bot),
		// freshness 1; one per vehicle.
		const float radius = ownAir ? 600.0f : 75.0f;
		std::vector<uint32_t> vehiclesDone;
		for (const GameWorld::PlayerInfo& other : players) {
			physics::Vec3 at;
			if (!other.alive || other.team == self.team || other.team < 1 || other.team > 2
				|| !m_knowledge[self.team].count(other.id) || !knownPosition(self.team, other.id, at)
				|| physics::lengthSquared(at - self.position) > radius * radius || pilotTargetsMe(other)) {
				continue;
			}
			if (other.vehicle) {
				if (std::find(vehiclesDone.begin(), vehiclesDone.end(), other.vehicle) != vehiclesDone.end()) {
					continue;
				}
				vehiclesDone.push_back(other.vehicle);
			}
			float alertAt = -1e9f;
			for (const Known& k : bot.known) {
				alertAt = k.player == other.id ? k.alertAt : alertAt;
			}
			const float score = scoreOf(&other, at, 1.0f, alertAt);
			if (other.id == bot.target) {
				currentScore = score;
			}
			if (score > best) {
				best = score;
				bestTarget = other.id;
			}
		}
		// The chosen unit enters the bot's memory as known from others.
		if (bestTarget >= 0 && std::none_of(bot.known.begin(), bot.known.end(), [&](const Known& k) { return k.player == bestTarget; })) {
			Known k;
			k.player = bestTarget;
			knownPosition(self.team, bestTarget, k.position);
			k.seen = m_time;
			k.seenAt = m_time;
			k.everSeen = true;
			k.secondHand = true;
			bot.known.push_back(k);
		}
	}
	if (best <= 0) {
		bot.target = -1;
		return 0;
	}
	if (!(bot.target >= 0 && currentScore > 0 && best <= currentScore * 1.2f)) {
		if (bestTarget != bot.target) {
			bot.targetAcquired = m_time;
			bot.heliOnTarget.reset(); // a new plan's condition
		}
		bot.target = bestTarget;
	} else {
		best = currentScore;
	}
	return ai::fuzzy(2 * best) * weight;
}

// BBChangeLandingCraft's urgency (AIDLL 0x1006f460; the transport helicopters'
// pilots and passengers, AS_AT/EU_AT's ParatrooperFixed seats): a seat
// change within its own vehicle, or the way out once it has landed at the
// bot's order.
//   - Each seat of the vehicle is weighed as BBChange weighs a seat
//     (0x100d3e70, `other` for all but the bot's own) times a factor: the
//     bot's own seat 1 when it drives, when the driver's seat is taken
//     (0x100aa530) or the bot's object is ITNaval (flag bit 4), else 0.25;
//     another seat 0.25, the root's (the driver's) 1.5 for a passenger of
//     an ITAir root, else 1. A free driver's seat of a root with a Mobile
//     part gains 100 (its Mobile with no way, 0x100a46c0's +4 at -1, ends
//     the scan and takes the way out instead: the rewrite's vehicles always
//     have one).
//   - Near the order (the order's spot within 15 m of the bot, 50 m when
//     the area is the enemy's: 0x100bf800 with 225 / 2500) a driven
//     transport (0x1006f3c0: a seat whose Unit's driver's seat is taken)
//     says "bail_out" to those aboard (0x100a7860: type 0, the AI's
//     channel 7) unless its send period runs; one with nobody at the
//     controls lets them out within 10 m (40 m: 100 / 1600): urgency 4 and
//     the way out (0x10097e60 lets go of the controls).
//   - Otherwise the best seat, if it isn't the bot's own: fuzzy(best /
//     current / 2) x 4 (fuzzy(best / 2) x 4 with the current worth
//     nothing), x 6 within 15 s of the vehicle's last entry (the root
//     unit's 0x100aa610: here the bot's own seating), and 0 for a seat
//     another bot reserved (AIDLL 0x1006f320).
// The ground transports' way out (no ITAir: the Physical part's slot
// 0x5c point within 4 m, the Mobile's speed under 2) isn't followed: no
// shipped ground vehicle runs this behaviour. The weight is used only
// through the record's +8 (1).
float BotManager::landingCraftUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight)
{
	(void)weight;
	const uint32_t vehicle = self.vehicle;
	const std::vector<int> occupants = m_world.vehicleOccupants(vehicle);
	const size_t seats = std::max<size_t>({1, occupants.size(), m_world.vehicleSeats(vehicle).size()});
	const world::Object* object = m_world.objects().find(vehicle);
	const std::string rootAi = object && object->objectTemplate() ? aiTemplateOf(object->objectTemplate()->name()) : "";
	const uint32_t types = m_settings.typeFlags(rootAi);
	const bool air = (types & kITAir) != 0;
	const bool mobile = m_settings.plugin(rootAi, "Mobile") != nullptr;
	const bool driving = self.seat == 0;
	const bool driverTaken = !occupants.empty() && occupants[0] >= 0;
	const float own = driving || driverTaken || (types & (1u << 4)) ? 1.0f : 0.25f;
	const float root = !driving && air ? 1.5f : 1.0f;
	float current = 0, best = 0;
	int bestSeat = self.seat;
	for (size_t seat = 0; seat < seats; ++seat) {
		const bool mine = static_cast<int>(seat) == self.seat;
		float value = seatValue(bot, self, vehicle, static_cast<int>(seat), !mine);
		value *= mine ? own : seat == 0 ? root : 0.25f;
		if (seat == 0 && !driverTaken && mobile) {
			value += 100;
		}
		if (mine) {
			current = value;
		}
		if (value > best) {
			best = value;
			bestSeat = static_cast<int>(seat);
		}
	}
	// Landed at the order: out.
	if (air && bot.hasGoal) {
		physics::Vec3 at;
		float radius = 0;
		int owner = 0;
		const bool enemyArea = bot.goalOrder >= 0 && areaPosition(bot.goalOrder, at, radius, owner) && owner != 0
			&& owner != self.team;
		const float d2 = horizontal2(bot.orderPosition - self.position);
		if (d2 < (enemyArea ? 2500.0f : 225.0f)) {
			if (driverTaken) {
				queueRadio(bot, 0, 7, 0); // bail_out
			} else if (d2 < (enemyArea ? 1600.0f : 100.0f)) {
				bot.vehicleTarget = 0; // the way out
				bot.seatTarget = -1;
				return 4;
			}
		}
	}
	if (bestSeat == self.seat || best <= 0) {
		bot.vehicleTarget = 0;
		return 0;
	}
	bot.vehicleTarget = vehicle;
	bot.seatTarget = bestSeat;
	float urgency = ai::fuzzy(current > 0 ? best / current * 0.5f : best * 0.5f) * 4;
	if (m_time - bot.seatedAt < 15) {
		urgency *= 6;
	}
	return urgency;
}

// BBFlee's urgency (0x100859d0): below 30 % health, the camouflage
// charged over 70 % (its ammo ratio; the rewrite's camouflage has no
// charge, so carrying it counts as charged) and more than 10 s since
// spawning: 100, away from
// the known enemies (x3 for players, x2 for bots that target this one).
// (BBFlee's class: constructor 0x10085810, destructor 0x100858e0. Its
// slot 11, shared with BBC4, 0x10082070: runs only for a soldier, the
// object's flag bit 9, that isn't scripted, bot +0x264; every bot here.)
float BotManager::fleeUrgency(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players)
{
	if (self.vehicle || healthOf(self.soldier) >= 0.3f || m_time - bot.spawnedAt <= 10 || self.cloaked) {
		return 0;
	}
	bool camo = false;
	for (const auto& [slot, name] : self.carried) {
		camo = camo || (weaponAi(name).type == WeaponAi::Camouflage && contains(name, "camo"));
	}
	if (!camo) {
		return 0;
	}
	physics::Vec3 away;
	for (const Known& known : bot.known) {
		if (!known.everSeen) {
			continue;
		}
		const auto other = std::find_if(players.begin(), players.end(), [&](const auto& p) { return p.id == known.player; });
		float weight = 1;
		if (other != players.end() && !other->ai) {
			weight = 3;
		} else if (const auto it = m_bots.find(known.player); it != m_bots.end() && it->second.target == self.id) {
			weight = 2;
		}
		physics::Vec3 d = self.position - known.position;
		d.y = 0;
		const float length = std::sqrt(horizontal2(d));
		if (length > 0.001f) {
			away = away + d * (weight / length);
		}
	}
	const float length = std::sqrt(horizontal2(away));
	if (length < 0.001f) {
		return 0;
	}
	bot.fleeDirection = away * (1 / length);
	return 100;
}

int BotManager::kitSlot(const GameWorld::PlayerInfo& self, int type) const
{
	for (const auto& [slot, name] : self.carried) {
		if (weaponAi(name).type == type) {
			return slot;
		}
	}
	return 0;
}

// Bot slot 8 (0x10098200, vtable 0x10118050; run by Bot::think 0x10099280
// when the bot's controlled object changes, and by the bot manager's man
// down, death and spawn events 0x100c5ea0 / 0x100c64d0 / 0x100c72a0): the
// side's AI object of the object the bot leaves has its +0x14 (the
// basicTemp the area pass 0x1005f210 adds) x 0.75, the one it takes
// x 1.3333334; then the path follower is reset (0x10095120), the AI
// object set (vt+0xd0), +600 / +0x25c / +0x260 cleared and the inputs
// cleared (0x10097e60). So the object a bot of the side controls (its
// soldier on foot, its seat aboard) counts 4/3 of its basicTemp in the
// side's area temperatures, and goes back as the bot leaves it.
constexpr float kBotTempFactor = 1.3333334f;

// An object's AI template's basicTemp (aiTemplate.basicTemp, taken as the
// AI object's +0x14 that the assist scores weigh targets by; the store to
// +0x14 was not found).
float BotManager::basicTemp(const std::string& objectTemplate, float fallback) const
{
	const auto it = m_settings.aiTemplates.find(lower(aiTemplateOf(objectTemplate)));
	return it != m_settings.aiTemplates.end() && it->second.basicTemp > 0 ? it->second.basicTemp : fallback;
}

const BotManager::UnitAi& BotManager::unitAiOf(const world::ObjectTemplate* t) const
{
	auto it = m_unitAi.find(t);
	if (it != m_unitAi.end()) {
		return it->second;
	}
	UnitAi u;
	if (t) {
		const auto ai = t->settings().find("aiTemplate");
		const std::string name = ai != t->settings().end() ? lower(ai->second) : std::string();
		if (const auto tt = m_settings.aiTemplates.find(name); tt != m_settings.aiTemplates.end()) {
			u.basicTemp = tt->second.basicTemp;
		}
		u.unit = name.empty() ? nullptr : m_settings.plugin(name, "Unit");
		if (const auto k = m_settings.kitTemplates.find(name); k != m_settings.kitTemplates.end()) {
			u.kit = &k->second;
		}
	}
	return m_unitAi.emplace(t, u).first->second;
}

void BotManager::unitStrengths(const GameWorld::PlayerInfo& p, float& battle, std::array<float, 2>& strategic, float health) const
{
	battle = 0;
	strategic = {};
	const world::Object* soldier = m_world.objects().find(p.soldier);
	const world::Object* root = p.vehicle ? m_world.objects().find(p.vehicle) : nullptr;
	std::array<float, 8> b {};
	std::array<int, 2> st {};
	if (root) {
		// A manned vehicle (seat 0 taken, 0x008a1b30): its root's Unit.
		// Whether the seated soldier counts too isn't settled; here not.
		if (!p.driving) {
			return;
		}
		if (const AiPlugin* unit = unitAiOf(root->objectTemplate()).unit) {
			b = unit->battleStrength;
			st = unit->strategicStrength;
		}
	} else {
		const UnitAi& u = unitAiOf(soldier ? soldier->objectTemplate() : nullptr);
		if (u.unit) {
			b = u.unit->battleStrength;
			st = u.unit->strategicStrength;
		}
		const world::Object* kit = m_world.objects().find(p.kit);
		const BotSettings::KitTemplate* kt = unitAiOf(kit ? kit->objectTemplate() : nullptr).kit;
		if (kt) {
			st = kt->strategic;
		}
		bool armed = false;
		std::array<float, 8> best {};
		for (const auto& [slot, weapon] : p.carried) {
			const WeaponAi w = weaponAi(weapon);
			for (size_t i = 0; i < 8; ++i) {
				best[i] = std::max(best[i], w.strength[i]);
				armed = armed || w.strength[i] > 0;
			}
		}
		if (armed) {
			b = best;
		} else if (kt) {
			b = kt->battle;
		}
	}
	const float h = health >= 0 ? health : healthOf(root ? p.vehicle : p.soldier);
	const float factor = std::clamp(0.25f + 0.5f * ai::scurve(1.0f) + 0.25f * ai::scurve(h), 0.0f, 1.0f);
	for (float x : b) {
		battle += x * factor;
	}
	strategic = {static_cast<float>(st[0]) * factor, static_cast<float>(st[1]) * factor};
}

std::array<float, 8> BotManager::battleArray(const GameWorld::PlayerInfo& p) const
{
	std::array<float, 8> out {};
	if (!p.vehicle) {
		const world::Object* soldier = m_world.objects().find(p.soldier);
		const UnitAi& u = unitAiOf(soldier ? soldier->objectTemplate() : nullptr);
		std::array<float, 8> b = u.unit ? u.unit->battleStrength : std::array<float, 8> {};
		bool armed = false;
		std::array<float, 8> best {};
		for (const auto& [slot, weapon] : p.carried) {
			const WeaponAi w = weaponAi(weapon);
			for (size_t i = 0; i < 8; ++i) {
				best[i] = std::max(best[i], w.strength[i]);
				armed = armed || w.strength[i] > 0;
			}
		}
		const world::Object* kit = m_world.objects().find(p.kit);
		const BotSettings::KitTemplate* kt = unitAiOf(kit ? kit->objectTemplate() : nullptr).kit;
		if (armed) {
			b = best;
		} else if (kt) {
			b = kt->battle;
		}
		const float factor = std::clamp(0.25f + 0.5f * ai::scurve(1.0f) + 0.25f * ai::scurve(healthOf(p.soldier)), 0.0f, 1.0f);
		for (size_t i = 0; i < 8; ++i) {
			out[i] = b[i] * factor;
		}
		return out;
	}
	const world::Object* root = m_world.objects().find(p.vehicle);
	const float factor = std::clamp(0.25f + 0.5f * ai::scurve(1.0f) + 0.25f * ai::scurve(healthOf(p.vehicle)), 0.0f, 1.0f);
	const auto add = [&](const world::ObjectTemplate* t) {
		if (const AiPlugin* unit = unitAiOf(t).unit) {
			for (size_t i = 0; i < 8; ++i) {
				out[i] += unit->battleStrength[i] * factor;
			}
		}
	};
	add(root ? root->objectTemplate() : nullptr);
	for (const GameWorld::SeatLayout& seat : m_world.vehicleSeats(p.vehicle)) {
		const world::ObjectTemplate* st = m_world.templates().find(seat.name);
		if (st && (!root || st != root->objectTemplate())) {
			add(st);
		}
	}
	return out;
}

// The squad coordination factor (AIDLL 0x10089aa0 -> 0x100c28e0) for a task
// (Medic 1, Repair 2, Ammo 4) on a target: 0 outside squads; 1 for the
// bot's own task; for a target outside squads 0 when a teammate has the
// task or the bot's squad (its leader here) is more than 141 m away, else
// 1; in the bot's squad 0 when a squadmate has it, else 1; in another
// squad 0 when the task is taken or that squad is 31.6 m or more from
// the bot's, else 0.5, or 0.1 when that squad has a soldier of the task's
// kit (0x100c2830). In the bot's squad a squadmate of that kit (KitType
// Medic 5 healing, Support 2 resupplying; the player's +0x58 through the
// AI player's slot 0x40) that is alive is left to itself. Doubled for a
// human (the target's squad member not a bot, 0x100bf510). No shipped kit
// is a Medic (the assault kits heal), so that one never applies.
float BotManager::coordination(const GameWorld::PlayerInfo& self, int type, int64_t target, int targetPlayer,
	const physics::Vec3& at) const
{
	if (self.squad == 0) {
		return 0;
	}
	const auto mine = m_tasks.find(self.id);
	if (mine != m_tasks.end() && mine->second == std::make_pair(type, target)) {
		return 1;
	}
	GameWorld::PlayerInfo other;
	const bool hasOther = targetPlayer >= 0 && m_world.playerInfo(targetPlayer, other);
	const float human = hasOther && !other.ai ? 2.0f : 1.0f;
	bool takenInTeam = false, takenInSquad = false;
	for (const auto& [id, task] : m_tasks) {
		GameWorld::PlayerInfo mate;
		if (id == self.id || task != std::make_pair(type, target) || !m_world.playerInfo(id, mate) || mate.team != self.team) {
			continue;
		}
		takenInTeam = true;
		takenInSquad = takenInSquad || mate.squad == self.squad;
	}
	physics::Vec3 squadAt = self.position;
	for (const GameWorld::PlayerInfo& p : m_players) {
		if (p.team == self.team && p.squad == self.squad && p.squadLeader && p.alive) {
			squadAt = p.position;
		}
	}
	if (!hasOther || other.squad == 0 || other.team != self.team) {
		if (takenInTeam) {
			return 0;
		}
		return physics::lengthSquared(squadAt - at) > 20000 ? 0.0f : human;
	}
	// The kit that does the task itself (KitType: Medic 5 for healing,
	// Engineer 4 for repairs, Support 2 for ammo).
	const int kit = type == 1 ? 5 : type == 2 ? 4 : type == 4 ? 2 : -1;
	if (other.squad == self.squad) {
		// A squadmate (not the bot) with that kit and alive (its health over
		// 0.001) heals or resupplies itself (medic and ammo only).
		if (other.id != self.id && (type == 1 || type == 4) && kitTypeOf(other) == kit && healthOf(other.soldier) > 0.001f) {
			return 0;
		}
		return takenInSquad ? 0.0f : human;
	}
	if (takenInTeam || physics::lengthSquared(squadAt - other.position) >= 1000) {
		return 0;
	}
	// 0.1 when the target's squad has its own soldier with the kit (0x100c2830).
	bool ownKit = false;
	for (const GameWorld::PlayerInfo& p : m_players) {
		ownKit = ownKit || (p.team == other.team && p.squad == other.squad && kitTypeOf(p) == kit);
	}
	return (ownKit ? 0.1f : 0.5f) * human;
}

// BBMedicAssist (0x10070200) and BBAmmoAssist (0x10071a10). Candidates are
// the team's soldiers on foot within the kit's maxRange (MedicHub,
// AmmoHub: 70 m). MedicAssist: below 75 % health (99 % for a human),
// the bot itself only while it isn't holding the hub; score basicTemp /
// (fuzzy(1 - health) x distance), the bot itself at distance 1.
// AmmoAssist: human players only (bots are never resupplied), with some
// weapon (FireArm, Sniper, C4) whose ceil(rounds / magSize) / magazines
// is below 60 % (99 % while the bot holds the ammo hub); score basicTemp
// / (scurve(that ratio) x distance). Urgency: fuzzy(best) x weight x 2
// for a bot, x 10 for a human (x 4 for ammo), times the squad
// coordination factor. The radio (types 3 "medic" and 10 "need_ammo" in
// the bot's comm object, kept 20 s, see radio): a heal's score x3 for a
// human's call, x1.5 for a bot's, and with a human's call heard and the
// best a human within 50 m the urgency is 100; ammo for a caller needs only
// some weapon short (ratio below 1), scores x70, and the urgency is x100.
// A candidate past the health / ammo test must stand upright on the
// terrain (uprightOnGround: the slope under him at most 45 degrees).
float BotManager::medicUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight, bool ammo)
{
	const int slot = kitSlot(self, ammo ? WeaponAi::Ammo : WeaponAi::Healing);
	if (self.vehicle || !slot) {
		return 0;
	}
	const float range = std::max(weaponAi(weaponName(self, slot)).maxRange, 1.0f);
	const bool holding = self.inHandSlot == slot;
	float best = 0;
	int chosen = -1;
	bool chosenHuman = false, chosenHeard = false, humanCalled = false;
	float chosenDistance = 0;
	for (const GameWorld::PlayerInfo& o : m_players) {
		if (o.team != self.team || !o.alive || o.manDown || o.vehicle) {
			continue;
		}
		const bool itself = o.id == self.id;
		const bool human = !o.ai;
		// Its "medic!" / "ammo!" (radio types 3 and 10) in the bot's comm
		// object (0x100a6160; a flying bot, vt+0x180, doesn't look).
		const bool called = heard(bot, ammo ? 10 : 3, o.id);
		float ratio = 1;
		if (ammo) {
			if (itself || o.ai) {
				continue;
			}
			// The ammo ratio of its weapons (the last counted one scores).
			float least = 1;
			bool needs = false;
			for (const GameWorld::WeaponAmmo& w : m_world.weaponAmmo(o.id)) {
				const int type = weaponAi(w.weapon).type;
				if ((type != WeaponAi::FireArm && type != WeaponAi::Sniper && type != WeaponAi::C4) || w.magSize <= 0
					|| w.magazines <= 0) {
					continue;
				}
				const float r0 = static_cast<float>(w.rounds) / static_cast<float>(w.magSize);
				const float r = w.magazines != 1 ? std::ceil(r0) / static_cast<float>(w.magazines) : r0;
				least = std::min(least, r);
				needs = needs || r < 0.99f;
				ratio = r;
			}
			const float threshold = holding ? 0.99f : 0.6f;
			if (!needs || !((called && least < 1) || (least > 0 && least < threshold))) {
				continue;
			}
		} else {
			if (itself && holding) {
				continue;
			}
			ratio = healthOf(o.soldier);
			if (ratio >= (human ? 0.99f : 0.75f)) {
				continue;
			}
		}
		if (!uprightOnTerrain(o.position, {0, 1, 0})) {
			continue;
		}
		const float d = std::sqrt(physics::lengthSquared(o.position - self.position));
		if (d > range) {
			continue;
		}
		const float distance = itself ? 1.0f : std::max(d, 0.01f);
		const float curve = ammo ? ai::scurve(ratio) : ai::fuzzy(1 - ratio);
		const float score = basicTemp(m_world.objects().find(o.soldier) && m_world.objects().find(o.soldier)->objectTemplate()
										  ? m_world.objects().find(o.soldier)->objectTemplate()->name()
										  : std::string(),
								1.0f)
			/ (std::max(curve, 1e-4f) * distance);
		// Called for: ammo x70; a heal x3 from a human, x1.5 from a bot.
		float scored = score;
		if (called) {
			scored *= ammo ? 70.0f : human ? 3.0f : 1.5f;
			humanCalled = humanCalled || (!ammo && human);
		}
		if (scored > best) {
			best = scored;
			chosen = o.id;
			chosenHuman = human;
			chosenHeard = called;
			chosenDistance = distance;
		}
	}
	if (chosen < 0) {
		bot.assistTarget = -1;
		return 0;
	}
	bot.assistTarget = chosen;
	GameWorld::PlayerInfo target;
	m_world.playerInfo(chosen, target);
	float urgency = ai::fuzzy(best) * weight * (ammo ? 4.0f : chosenHuman ? 10.0f : 2.0f);
	if (ammo && chosenHeard) {
		urgency *= 100;
	} else if (!ammo && chosenHuman && humanCalled && chosenDistance < 50) {
		urgency = 100; // a human called for a medic and the best is a human within 50 m
	}
	return urgency * coordination(self, ammo ? 4 : 1, chosen, chosen, target.position);
}

// BBRepairVehicle (0x10073360; vtable 0x10116b68, constructor 0x10073250,
// destructor 0x10073300): the vehicles within the repair tool's
// maxRange (50 m) between 0 and 60 % health (the threshold compares the
// held weapon with the ammo hub's slot, a slip of the original, so it
// stays 0.6), not the bot's own; score basicTemp / (f x distance), f =
// fuzzy(1 - health), 1 for the vehicle it repairs already. Urgency:
// fuzzy(best) x weight x 10 with a human aboard, x 2 otherwise, times the
// coordination factor (task 2). A vehicle not upright on the terrain
// (uprightOnGround: on its side or roof) is left out.
float BotManager::repairUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight)
{
	const int slot = kitSlot(self, WeaponAi::Repair);
	if (self.vehicle || !slot) {
		bot.repairTarget = 0;
		return 0;
	}
	const float range = std::max(weaponAi(weaponName(self, slot)).maxRange, 1.0f);
	float best = 0;
	uint32_t chosen = 0;
	bool chosenHuman = false;
	int chosenDriver = -1;
	physics::Vec3 chosenAt;
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		const float h = healthOf(v.object);
		if (h <= 0 || h >= 0.6f || !uprightOnTerrain(v.position, physics::cross(v.forward, v.right))) {
			continue;
		}
		const float d = std::sqrt(physics::lengthSquared(v.position - self.position));
		if (d > range) {
			continue;
		}
		bool human = false;
		int driver = -1;
		for (int occupant : m_world.vehicleOccupants(v.object)) {
			GameWorld::PlayerInfo info;
			if (occupant >= 0 && m_world.playerInfo(occupant, info)) {
				human = human || !info.ai;
				driver = driver < 0 ? occupant : driver;
				if (info.team != self.team) {
					human = false;
					driver = -2; // an enemy's
				}
			}
		}
		if (driver == -2) {
			continue;
		}
		const world::Object* object = m_world.objects().find(v.object);
		const float temp = basicTemp(object && object->objectTemplate() ? object->objectTemplate()->name() : "", 10.0f);
		const float f = v.object == bot.repairTarget ? 1.0f : ai::fuzzy(1 - h);
		const float score = temp / (std::max(f, 1e-4f) * std::max(d, 0.01f));
		if (score > best) {
			best = score;
			chosen = v.object;
			chosenHuman = human;
			chosenDriver = driver;
			chosenAt = v.position;
		}
	}
	bot.repairTarget = chosen;
	if (!chosen) {
		return 0;
	}
	return ai::fuzzy(best) * weight * (chosenHuman ? 10.0f : 2.0f)
		* coordination(self, 2, static_cast<int64_t>(chosen) + (int64_t {1} << 32), chosenDriver, chosenAt);
}

// BBC4 (0x10083750; the weight is not used). With a charge in the kit and
// none placed: 100 inside a control point the team holds uncontested
// (SAI 0x1009c280, the area scoring the point highest 0x100349b0). With
// one placed (it lapses after 240 s): 100 to set
// it off once that point is contested with its flag below 0.75 and no
// teammate within 15 m of the charge (0x1009c4d0, 0x1008a110).
float BotManager::c4Urgency(Bot& bot, const GameWorld::PlayerInfo& self)
{
	const int slot = kitSlot(self, WeaponAi::C4);
	if (self.vehicle || !slot) {
		return 0;
	}
	if (bot.c4Placed && m_time > bot.c4PlacedAt + 240) {
		bot.c4Placed = false;
	}
	const auto pointOf = [&](int area, ControlPoint& out) {
		if (area < 0) {
			return false;
		}
		for (const ControlPoint& cp : m_world.controlPoints()) {
			if (cp.id == m_settings.strategy.areas[static_cast<size_t>(area)].controlPoint) {
				out = cp;
				return true;
			}
		}
		return false;
	};
	if (!bot.c4Placed) {
		int rounds = 0;
		for (const GameWorld::WeaponAmmo& w : m_world.weaponAmmo(self.id)) {
			rounds += weaponAi(w.weapon).type == WeaponAi::C4 ? w.rounds : 0;
		}
		if (rounds < 1) {
			return 0;
		}
		for (size_t i = 0; i < m_settings.strategy.areas.size(); ++i) {
			physics::Vec3 at;
			float radius = 0;
			int owner = 0;
			ControlPoint cp {};
			if (!areaPosition(static_cast<int>(i), at, radius, owner) || owner != self.team
				|| horizontal2(self.position - at) > radius * radius || !pointOf(static_cast<int>(i), cp)) {
				continue;
			}
			if (cp.flag == self.team && cp.flagHeight < 0.9f) {
				continue; // contested
			}
			bot.c4Mode = 0;
			bot.c4Area = static_cast<int>(i);
			return 100;
		}
		return 0;
	}
	ControlPoint cp {};
	if (!pointOf(bot.c4Area, cp) || cp.team != self.team || cp.flag != self.team || cp.flagHeight >= 0.75f) {
		return 0;
	}
	for (const GameWorld::PlayerInfo& mate : m_players) {
		if (mate.alive && mate.team == self.team && physics::lengthSquared(mate.position - bot.c4At) < 15.0f * 15.0f) {
			return 0;
		}
	}
	bot.c4Mode = 1;
	return 100;
}

// BBDefuseMines (0x10086870): an engineer (a Repair weapon), not
// swimming, near an enemy mine lying in wait: its stand point 0.8 m
// behind the mine's facing on the mesh and within 35 m: 42 x weight.
// The original keeps the mines in IAIMineManager (constructor 0x1004c0e0,
// destructor 0x1004c1b0 / 0x1004c010, its two mine grids 0x1004beb0 /
// 0x1004bf60); the rewrite reads the world's mines.
float BotManager::defuseUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight)
{
	if (self.vehicle || !kitSlot(self, WeaponAi::Repair)) {
		return 0;
	}
	float nearest = 1e30f;
	bool found = false;
	for (const Projectile& p : m_world.projectiles()) {
		if (!p.alive || !p.resting || !p.settings || p.settings->triggerRadius <= 0 || p.team == self.team) {
			continue;
		}
		const physics::Vec3 forward {std::sin(p.yaw * kRadians), 0, std::cos(p.yaw * kRadians)};
		const physics::Vec3 stand = p.position - forward * 0.8f;
		const float d2 = physics::lengthSquared(stand - self.position);
		if (d2 >= 1225 || d2 >= nearest || (m_navMesh && m_navMesh->findPolygon(toAi(stand)) < 0)) {
			continue;
		}
		nearest = d2;
		bot.mineAt = p.position;
		bot.mineStand = stand;
		found = true;
	}
	return found ? 42 * weight : 0;
}

// BBRunOverSoldier (0x10087820; the weight is not used): at 10 m/s or
// more, a known enemy soldier in sight within 50 m, ahead within 0.85 of
// the vehicle's heading and on a clear line over the vehicle mesh: 65.
float BotManager::runOverUrgency(Bot& bot, const GameWorld::PlayerInfo& self)
{
	bot.runOverTarget = -1;
	const GameWorld::VehicleInfo* vehicle = nullptr;
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		vehicle = v.object == self.vehicle ? &v : vehicle;
	}
	if (!vehicle || self.seat != 0 || std::sqrt(horizontal2(vehicle->velocity)) < 10) {
		return 0;
	}
	for (const Known& k : bot.known) {
		GameWorld::PlayerInfo other;
		if (k.lostAt >= 0 || !k.everSeen || !m_world.playerInfo(k.player, other) || !other.alive || other.vehicle
			|| other.team == self.team || other.team == 0) {
			continue;
		}
		physics::Vec3 d = other.position - vehicle->position;
		d.y = 0;
		const float distance = std::max(std::sqrt(horizontal2(d)), 1.0f);
		if (distance > 50 || physics::dot(d * (1 / distance), vehicle->forward) < 0.85f) {
			continue;
		}
		if (m_vehicleNavMesh && !ai::meshRaycast(*m_vehicleNavMesh, toAi(vehicle->position), toAi(other.position), nullptr, 30)) {
			continue;
		}
		bot.runOverTarget = other.id;
		return 65;
	}
	return 0;
}

// BBAvoid (0x10064850), on foot only (the urgency wants a soldier: the 2d
// pattern of the vehicles' sets never runs: BBPAvoidCollision2d, vtable
// 0x10116528, plan 0x10066440, constructor 0x100662f0, destructor
// 0x10066370, name 0x100662d0). Other soldiers are never
// avoided. For each vehicle within 50 m (not the one it is getting into
// or repairing): a wreck or one below 30 % health within max(r, min(10,
// r + 4)) adds 100; one moving at 2 m/s or more that will pass within
// 0.6 (r + own radius) x sqrt(1.5) inside the look-ahead time T, closing
// at 3 m/s or more, adds |closing speed| / distance. Urgency: the sum x
// weight. r: the vehicle's AI radius (its box's farthest corner, vt+0x28).
// T, the look-ahead time, is the bot's Mobile part's slot 0x68
// (0x100a44f0 -> the game's Mobile adaptor, vtable 0x009d6a50 slot 0x68
// 0x008a1830: the Mobile template's +0x44), which the template's
// constructor (0x008917b0) sets to 2 and no aiTemplatePlugIn property
// writes (lookAheadTime is +0x20, the driving's): 2 s, x2 prone, x1.5
// crouched. The moving vehicle's term is also multiplied by its Physical
// part's slot 0x48 (0x100aae70 -> the game's Physical adaptor 0x009d7228
// slot 0x1c 0x008a3f30: the Physical template's +0x8). The Physical
// template's constructor (0x00894450) zeroes it and its only property
// (setStrType, +0x18) doesn't touch it, so the term is always 0: in the
// shipped game moving vehicles never raise Avoid; only wrecks and
// vehicles below 30 % do. Kept as the size factor 0.
// With nothing to avoid, the grenade warning (see warnOfGrenades) gives
// 1000 (the weight not applied) while 2.8 s - (now - stamp) is above 0
// (0x10092960), and the plan runs from it.
// A non-zero sum is then scaled twice (0x10064850):
//   - by the bot's running-behaviour record's +8 (vt+0xe0 -> bot +0x4c)
//     when the bot's hint names this behaviour (vt+0x68: bot +0x9c) and
//     its +0x50 holds (vt+0x64). The record is made by 0x100ea3f0 with
//     +8 = 1 and reset by 0x100ea410 to 1; nothing else writes it, so the
//     factor is 1 (the same holds for the assists' and Revive's uses);
//   - x2 for a soldier while the clock is within 5 s of its comm object's
//     +0x78 (bot vt+0xa4), which only the comm's reset (0x100a7630) writes,
//     to 0. The clock (IAIGame's, BF2142.exe 0x00666070) counts the game's
//     ticks from GameLogic's reset (0x004ce910 sets it to 0; 0x006660b0,
//     0x006660e0: ticks x 1/30 s): the first 5 s of the level. Here the
//     bot manager's clock, from its start.
constexpr float kAvoidSizeFactor = 0;
float BotManager::avoidUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight)
{
	if (self.vehicle) {
		return 0;
	}
	float lookAhead = 2;
	if (bot.pose == SoldierPose::Prone) {
		lookAhead *= 2;
	} else if (bot.pose == SoldierPose::Crouch) {
		lookAhead *= 1.5f;
	}
	const physics::Vec3 myVelocity = self.velocity;
	float sum = 0, best = 0;
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		if (v.object == bot.vehicleTarget || v.object == bot.repairTarget) {
			continue;
		}
		physics::Vec3 rel = v.position - self.position;
		rel.y = 0;
		if (horizontal2(rel) > 50.0f * 50.0f) {
			continue;
		}
		physics::Vec3 vrel = v.velocity - myVelocity;
		vrel.y = 0;
		const float distance = std::sqrt(horizontal2(rel));
		const bool moving = healthOf(v.object) >= 0.3f && v.working;
		const float kVehicleRadius = vehicleBox(v).radius;
		if (!moving) {
			const float reach = std::max(kVehicleRadius, std::min(10.0f, kVehicleRadius + 4));
			if (distance < reach) {
				sum += 100;
				if (100 > best) {
					best = 100;
					bot.avoidStatic = true;
					bot.avoidFrom = v.position;
					bot.avoidFor = std::max(1.0f, (reach - distance) * 0.2f + 0.5f);
				}
			}
			continue;
		}
		if (horizontal2(v.velocity) < 4) {
			continue;
		}
		// predict (0x100646e0).
		const float r = 0.6f * (kVehicleRadius + kSoldierRadius);
		const float r2 = 1.5f * r * r;
		const float vv = horizontal2(vrel);
		if (vv < 9) {
			continue;
		}
		const float tMin = -physics::dot(rel, vrel) / vv;
		if (tMin < 0) {
			continue;
		}
		const float d2 = horizontal2(rel + vrel * tMin);
		if (d2 >= r2) {
			continue;
		}
		const float hit = tMin - std::sqrt(r2 - d2) / std::sqrt(vv);
		if (hit < 0 || hit > lookAhead) {
			continue;
		}
		const float score = std::sqrt(vv) / std::max(distance, 0.1f) * kAvoidSizeFactor;
		sum += score;
		if (score > best) {
			best = score;
			bot.avoidStatic = false;
			bot.avoidFrom = v.position;
			bot.avoidVelocity = horizontal2(v.velocity) > 0 ? v.velocity : v.forward;
			bot.avoidFor = hit;
		}
	}
	if (sum != 0) {
		bot.avoidGrenade = false;
		if (m_time < 5) {
			sum *= 2;
		}
		return sum * weight;
	}
	const float left = 2.8f - (m_time - bot.grenadeStamp);
	if (left > 0) {
		bot.avoidGrenade = true;
		bot.avoidStatic = true;
		bot.avoidFrom = bot.grenadeAt;
		bot.avoidFor = left;
		return 1000;
	}
	return 0;
}

// BBPMedicAssist / BBPAmmoAssist (0x10070da0, AIDLL 0x100724e0; BBPMedicAssist's
// vtable 0x10116a7c: name 0x10070a90, constructor 0x10070b30, destructor
// 0x10070bf0): to the target
// (MoveToObject, stopping at the two soldiers' radii), looking at it,
// standing; within half the hub's abilityRadius plus those radii the hub
// in hand. The bot itself: crouch and take the hub. The plan never ends
// on its own: the urgency drops it once the target is served. AIDLL
// presses no fire: the hub works round its holder while in hand (the
// ReplenishingAmmoComp, see GameWorld's gadgets).
void BotManager::assist(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds, bool ammo)
{
	const int slot = kitSlot(self, ammo ? WeaponAi::Ammo : WeaponAi::Healing);
	GameWorld::PlayerInfo target;
	if (!slot || !m_world.playerInfo(bot.assistTarget, target) || !target.alive) {
		return;
	}
	m_tasks[self.id] = {ammo ? 4 : 1, target.id};
	const world::ObjectTemplate* hub = m_world.templates().find(weaponName(self, slot));
	const float abilityRadius = settingNumber(hub, "ammo.abilityRadius", 4);
	const float radii = 2 * kSoldierRadius;
	const bool itself = target.id == self.id;
	const float d = std::sqrt(physics::lengthSquared(target.position - self.position));
	if (!itself && d > radii) {
		steer(bot, self, target.position, 500000 + target.id, seconds, input, radii);
	}
	if (itself || d <= 0.5f * abilityRadius + radii) {
		input.selectItem = self.inHandSlot != slot ? slot : 0;
		if (itself) {
			bot.pose = SoldierPose::Crouch;
			input.pose = bot.pose;
		}
	}
	if (!itself && d <= radii) {
		const physics::Vec3 eye = self.position + physics::Vec3 {0, kEyeHeight, 0};
		aim(bot, self, target.position + physics::Vec3 {0, kChestHeight, 0}, {}, false, {}, input, seconds);
		(void)eye;
	}
}

// BBPRepairVehicle (0x10073da0): to the vehicle until within the two radii
// (the vehicle's AI radius, its box's farthest corner), the repair tool in hand; facing it (dot
// above 0.1, 0x100d7af0) the trigger held (TriggerContinously 0x1008ecd0),
// crouched when it is more than 0.5 m below the bot.
void BotManager::repair(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds)
{
	const int slot = kitSlot(self, WeaponAi::Repair);
	const GameWorld::VehicleInfo* vehicle = nullptr;
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		vehicle = v.object == bot.repairTarget ? &v : vehicle;
	}
	if (!slot || !vehicle) {
		return;
	}
	m_tasks[self.id] = {2, static_cast<int64_t>(vehicle->object) + (int64_t {1} << 32)};
	const float radii = vehicleBox(*vehicle).radius + kSoldierRadius;
	const float d = std::sqrt(horizontal2(vehicle->position - self.position));
	if (d > radii) {
		steer(bot, self, vehicle->position, 600000 + static_cast<int>(vehicle->object), seconds, input, radii, vehicle->object);
		return;
	}
	input.selectItem = self.inHandSlot != slot ? slot : 0;
	aim(bot, self, vehicle->position, {}, true, {}, input, seconds);
	const physics::Vec3 forward {std::sin(self.yaw * kRadians), 0, std::cos(self.yaw * kRadians)};
	physics::Vec3 to = vehicle->position - self.position;
	to.y = 0;
	const float facing = physics::dot(forward, to * (1 / std::max(std::sqrt(horizontal2(to)), 0.01f)));
	input.fire = self.inHandSlot == slot && facing > 0.1f;
	bot.pose = vehicle->position.y < self.position.y - 0.5f ? SoldierPose::Crouch : SoldierPose::Stand;
	input.pose = bot.pose;
}

// BBPC4 (0x10083ae0): no movement. Placing: the charge in hand, 0.7 s,
// one press of fire, the charge remembered (where, which area), then the
// primary weapon. Setting off: the detonation (alternate fire here, as
// the rewrite's soldiers set off their charges), the memory cleared.
// (The charge's memory statement 0x100ddb30 / 0x100ddb70: bot vt+0x13c,
// +0x138 to remember, +0x140 to clear; the 0.7 s wait, 0x100db860 run by
// 0x100db8b0, destructor 0x100db8f0.)
void BotManager::placeC4(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input)
{
	const int slot = kitSlot(self, WeaponAi::C4);
	if (!slot) {
		return;
	}
	if (bot.c4Mode == 1) {
		input.altFire = true;
		bot.c4Placed = false;
		bot.arbitration.replan = true;
		return;
	}
	if (self.inHandSlot != slot) {
		input.selectItem = slot;
		bot.c4InHandAt = -1;
		return;
	}
	if (bot.c4InHandAt < 0) {
		bot.c4InHandAt = m_time;
	}
	if (m_time - bot.c4InHandAt >= 0.7f && !bot.firedLast) {
		input.fire = true;
		bot.c4Placed = true;
		bot.c4At = self.position;
		bot.c4PlacedAt = m_time;
		bot.c4InHandAt = -1;
		bot.arbitration.replan = true;
	}
}

// BBPDefuseMines (0x10086fe0): to the stand point, crouched, looking at
// the mine (the look at a point, 0x100da170 / 0x100da1c0 / 0x100da1f0), the trigger held on the tool. The original's tool is the first
// Healing weapon (a slip: the urgency asks for a Repair one), which an
// engineer's kit doesn't have, so nothing is fired; nothing ends the plan
// but another behaviour.
void BotManager::defuse(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds)
{
	if (horizontal2(bot.mineStand - self.position) > 0.5f * 0.5f
		&& steer(bot, self, bot.mineStand, 700000, seconds, input)) {
		return;
	}
	bot.pose = SoldierPose::Crouch;
	input.pose = bot.pose;
	aim(bot, self, bot.mineAt, {}, true, {}, input, seconds);
	const int tool = kitSlot(self, WeaponAi::Healing);
	if (tool) {
		input.selectItem = self.inHandSlot != tool ? tool : 0;
		input.fire = self.inHandSlot == tool;
	}
}

// BBPRunOverSoldier (0x10087c30, RunOverObject 0x100ddd30): at the
// target's lead point (its position after the time to reach it, twice),
// the car controller's steering (3 x yawScale x angle - turn rate) at full
// throttle. It never ends by itself.
void BotManager::runOver(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input)
{
	GameWorld::PlayerInfo target;
	const GameWorld::VehicleInfo* vehicle = nullptr;
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		vehicle = v.object == self.vehicle ? &v : vehicle;
	}
	if (!vehicle || !m_world.playerInfo(bot.runOverTarget, target) || !target.alive) {
		return;
	}
	const float speed = std::max(std::sqrt(horizontal2(vehicle->velocity)), 1.0f);
	physics::Vec3 lead = target.position;
	for (int pass = 0; pass < 2; ++pass) {
		lead = target.position + target.velocity * (std::sqrt(horizontal2(lead - vehicle->position)) / speed);
	}
	const float heading = std::atan2(vehicle->forward.x, vehicle->forward.z);
	const float error = std::remainder(std::atan2(lead.x - vehicle->position.x, lead.z - vehicle->position.z) - heading,
		2 * 3.14159265f);
	const ai::DriveSettings settings = driveSettings(self);
	const float yawRate = bot.lastYawAt >= 0 && m_time > bot.lastYawAt
		? std::remainder(heading - bot.lastYaw, 2 * 3.14159265f) / (m_time - bot.lastYawAt)
		: 0.0f;
	bot.lastYaw = heading;
	bot.lastYawAt = m_time;
	input.strafe = std::clamp(3 * settings.yawScale * std::clamp(error, -1.5707964f, 1.5707964f) - yawRate, -1.0f, 1.0f);
	input.throttle = 1;
}

// BBPAvoidCollisionInfantery (AIDLL 0x10065360; what to avoid, its time and
// spot handed over by the pattern's slot 0x18, 0x100663b0): away from a standing wreck
// (10 m) or a grenade (15 m; the avoided object -1), or aside from a
// vehicle's path (10 m across its motion, on the side the bot is on),
// checked by a ray over the mesh (else across it, either way); walked to
// until the time it allows (for a grenade what is left of its warning)
// has passed or within 2 m.
void BotManager::avoid(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds)
{
	if (bot.avoidUntil < m_time - 0.5f) {
		physics::Vec3 goal;
		if (bot.avoidStatic) {
			physics::Vec3 away = self.position - bot.avoidFrom;
			away.y = 0;
			const float length = std::max(std::sqrt(horizontal2(away)), 0.01f);
			away = away * (1 / length);
			const float side = random() < 0.5f ? -1.0f : 1.0f;
			const physics::Vec3 across {away.z * side, 0, -away.x * side};
			const float reach = bot.avoidGrenade ? 15.0f : 10.0f;
			goal = self.position + away * reach;
			for (const physics::Vec3& d : {away, across, across * -1.0f}) {
				const physics::Vec3 g = self.position + d * reach;
				if (!m_navMesh || ai::meshRaycast(*m_navMesh, toAi(self.position), toAi(g), nullptr, 8)) {
					goal = g;
					break;
				}
			}
		} else {
			physics::Vec3 v = bot.avoidVelocity;
			v.y = 0;
			const float length = std::max(std::sqrt(horizontal2(v)), 0.01f);
			physics::Vec3 p {v.z / length * 10, 0, -v.x / length * 10};
			if (physics::dot(p, self.position - bot.avoidFrom) < 0) {
				p = p * -1.0f;
			}
			goal = self.position + p;
		}
		bot.avoidGoal = goal;
		bot.avoidUntil = m_time + bot.avoidFor;
	}
	if (m_time > bot.avoidUntil || horizontal2(bot.avoidGoal - self.position) < 2.0f * 2.0f
		|| !steer(bot, self, bot.avoidGoal, 800000, seconds, input)) {
		bot.arbitration.replan = true;
	}
}

// The aim controller's control object (MouseTurretAimAt 0x1008e940 passes
// the bot's control object's AI part, +0x38, whose ControlInfo the
// controller reads through 0x100cf7e0/0x100cf800/0x100cf820, slots
// 0x3c/0x40/0x44): a seat's aiTemplate, or the soldier's (SoldierCtrl).
AimGains BotManager::aimGainsOf(const GameWorld::PlayerInfo& self) const
{
	std::string objectTemplate;
	if (self.vehicle) {
		const std::vector<GameWorld::SeatLayout> seats = const_cast<GameWorld&>(m_world).vehicleSeats(self.vehicle);
		if (self.seat >= 0 && static_cast<size_t>(self.seat) < seats.size()) {
			objectTemplate = seats[static_cast<size_t>(self.seat)].name;
		}
	} else if (const world::Object* soldier = m_world.objects().find(self.soldier);
			   soldier && soldier->objectTemplate()) {
		objectTemplate = soldier->objectTemplate()->name();
	}
	AimGains gains;
	const AiPlugin* c = objectTemplate.empty() ? nullptr : m_settings.plugin(aiTemplateOf(objectTemplate), "ControlInfo");
	if (!c) {
		// Runs without the AI scripts (the tests; every shipped soldier and
		// seat has its ControlInfo): SoldierCtrl's.
		gains.proportional = kProportional;
		gains.derivative = kDerivative;
		gains.angleSpeed = kAngleSpeed;
		gains.lookVertical = 5;
		gains.lookHorizontal = 4;
		return gains;
	}
	gains.proportional = c->proportional;
	gains.derivative = c->derivative;
	gains.angleSpeed = c->angleSpeed;
	gains.lookVertical = c->lookVertical;
	gains.lookHorizontal = c->lookHorizontal;
	return gains;
}

// The aim (MouseTurretAimAt 0x1008e940, controller 0x100e5820), in the
// "towards the target" sense: pitch u = bias + 7 e + 0.01 de + lead,
// clamped to 4 and halved (at least 0.01); yaw likewise without bias and
// halving, limited to 3 skill + 1 (4 sprinting; 1.5 with gain 3 when
// there is nothing to aim at). lead = 0.4 x the aim point's angular speed,
// halved when it turns the same way as the error. The bias pulls down when
// the target has been below on average over the last 10 errors
// (min(0.75, 20 x the average)). A look at a point (MouseTurretLookAt
// 0x1008e6a0 / 0x1008e6e0) runs the same controller on the point, its
// rate from the last angles; MouseTurretAimAt's constructor 0x1008e930.
void BotManager::aim(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& point,
	const physics::Vec3& pointVelocity, bool hasTarget, const std::string& leadWeapon, SoldierInput& input, float seconds)
{
	// A plan's aim (MouseTurretLookAt / MouseTurretAimAt, 0x1008e6e0 /
	// 0x1008e940) pauses the sensing agent's look-around (0x100d51c0).
	if (!bot.look.quiet) {
		bot.look.paused = true;
		bot.look.pausedAt = m_time;
	}
	if (bot.gainsVehicle != self.vehicle || bot.gainsSeat != self.seat) {
		bot.gains = aimGainsOf(self);
		bot.gainsVehicle = self.vehicle;
		bot.gainsSeat = self.seat;
	}
	const AimGains& g = bot.gains;
	float gunYaw = 0, gunPitch = 0;
	const bool gunning = self.vehicle && (self.seat > 0 || drivesGun(self)) && m_world.gunAim(self.vehicle, self.seat, gunYaw, gunPitch);
	// A seat's gun aims from its round's start (the weapon's position,
	// AIDLL 0x1008e940: FUN_100a4090) along its own forward, which turns with the
	// hull: the angles are the hull's frame's, as the gun's yaw and pitch.
	physics::Vec3 eye = self.position + physics::Vec3 {0, kEyeHeight, 0};
	physics::Mat3 hull;
	if (gunning) {
		m_world.gunFrame(self.vehicle, self.seat, eye, hull);
	}
	const auto yawOf = [&](const physics::Vec3& at) {
		const physics::Vec3 d = gunning ? hull.untransform(at - eye) : at - eye;
		return std::atan2(d.x, d.z) * kDegrees;
	};
	const auto pitchOf = [&](const physics::Vec3& at) {
		const physics::Vec3 d = gunning ? hull.untransform(at - eye) : at - eye;
		return std::atan2(d.y, std::sqrt(horizontal2(d))) * kDegrees;
	};
	const float trueYaw = (gunning ? gunYaw : self.yaw) - bot.devYaw;
	// The weapon's recoil kicked the soldier's pitch (GameWorld::takeAimKick):
	// the bot's own angle takes it, and the aim corrects from there.
	if (!gunning) {
		bot.pitch = std::clamp(bot.pitch + m_world.takeAimKick(self.id), -kPitchLimit, kPitchLimit);
	}
	const float truePitch = gunning ? gunPitch : bot.pitch;
	// The lead (the aim object's slot 8, 0x100d9a60; its accessors for the
	// on-target test 0x100d96e0 / 0x100d96a0 / 0x100d96b0 / 0x100d96f0, the
	// aim base's destructor 0x100d9650): the solver takes the
	// point less the gun, the point's motion, the round's inherited motion
	// (the shooter's) and the gun's forward, and gives the launch
	// direction (precision 0.5, the high arc for an indirect weapon). The
	// aim then steers along it; without a solution, straight at the point
	// (slot 9, 0x1008e940's fallback). A solve that succeeds without
	// writing a direction (never bracketing the target) leaves 0x1008e940's
	// zeroed local; here it leaves the straight line.
	physics::Vec3 want = point;
	bool indirect = false;
	const ai::RoundModel round = leadWeapon.empty() ? ai::RoundModel {} : roundModel(leadWeapon, &indirect);
	if (round.speed > 0) {
		const float yr = trueYaw * kRadians, pr = truePitch * kRadians;
		physics::Vec3 forward {std::sin(yr) * std::cos(pr), std::sin(pr), std::cos(yr) * std::cos(pr)};
		if (gunning) {
			forward = hull.transform(forward);
		}
		bot.lead.setup(toAi(point - eye), toAi(pointVelocity), toAi(self.velocity), toAi(forward), m_time, round);
		const float reach = std::sqrt(physics::lengthSquared(point - eye));
		ai::Vec3 direction = toAi((point - eye) * (1 / std::max(reach, 1e-3f))), impact;
		if (bot.lead.solve(m_time, 0.5f, indirect, direction, impact)) {
			want = eye + physics::Vec3 {direction.x, direction.y, direction.z} * reach;
		}
	}
	const float wantYaw = yawOf(want);
	const float wantPitch = pitchOf(want);
	const float ey = angleBetween(trueYaw, wantYaw) * kRadians;
	const float ep = (wantPitch - truePitch) * kRadians;
	const int aimTarget = hasTarget ? bot.target : -1;
	const bool aimTargetChanged = gunning && aimTarget != bot.lastAimTarget;
	bot.lastAimTarget = aimTarget;
	float wy = 0, wp = 0;
	if (!aimTargetChanged && seconds > 0 && physics::lengthSquared(bot.lastAimPoint) > 0) {
		wy = angleBetween(yawOf(bot.lastAimPoint), wantYaw) * kRadians / seconds;
		wp = (wantPitch - pitchOf(bot.lastAimPoint)) * kRadians / seconds;
	}
	bot.lastAimPoint = want;
	bot.pitchErrors[static_cast<size_t>(bot.pitchErrorCount++ % 10)] = ep;
	float average = 0;
	const int n = std::min(bot.pitchErrorCount, 10);
	for (int i = 0; i < n; ++i) {
		average += bot.pitchErrors[static_cast<size_t>(i)];
	}
	average /= static_cast<float>(std::max(n, 1));
	// Pitch.
	float lead = g.angleSpeed * wp;
	if ((ep > 0) == (wp > 0)) {
		lead *= 0.5f;
	}
	const float bias = average < -1e-4f ? -std::min(0.75f, -20 * average) : 0.0f;
	const float pitchErrorDelta = aimTargetChanged ? 0.0f : ep - bot.pitchError;
	float up = bias - g.proportional * ep - g.derivative * pitchErrorDelta + lead;
	up = std::clamp(up, -4.0f, 4.0f) * 0.5f;
	if (up != 0 && std::fabs(up) < 0.01f) {
		up = up > 0 ? 0.01f : -0.01f;
	}
	// Yaw.
	const bool idleLook = !hasTarget && input.throttle == 0 && input.strafe == 0 && !input.sprint;
	const float gain = idleLook ? -3.0f : g.proportional;
	const float limit = idleLook ? 1.5f : input.sprint ? 4.0f : 3 * m_settings.skill + 1;
	float yawLead = g.angleSpeed * wy;
	if ((ey > 0) == (wy > 0)) {
		yawLead *= 0.5f;
	}
	const float yawErrorDelta = aimTargetChanged ? 0.0f
		: gunning ? std::remainder(ey - bot.yawError, 2 * 3.14159265f) : ey - bot.yawError;
	float right = -gain * ey - g.derivative * yawErrorDelta + yawLead;
	right = std::clamp(right, -limit, limit);
	if (std::fabs(right) < 0.001f) {
		right = 0; // the soldier's dead zone (0x0091fdc0)
	}
	bot.pitchError = ep;
	bot.yawError = ey;
	// Zoomed in, a unit turns the soldier by the field of view over the
	// camera's times mouseSpeedMod (0x00586420).
	const float zoomScale = !self.vehicle && bot.zoom.level() > 0
		? bot.fov.current / kBaseFieldOfView * bot.zoomSettings.mouseSpeedMod
		: 1.0f;
	float turn = right * kInputDegreesPerTick * kInputTicksPerSecond * seconds * zoomScale;
	// Not past the target in one tick.
	const float errorDegrees = ey * kDegrees;
	if ((errorDegrees >= 0 && turn > errorDegrees) || (errorDegrees < 0 && turn < errorDegrees)) {
		turn = errorDegrees;
	}
	// stepGunner applies this degree delta directly to the seat gun's yaw
	// (GameWorld.cpp, Body::Gun), bypassing rotationAxisStep. The input adapter
	// therefore has to enforce the yaw bundle's authored maxSpeed itself;
	// otherwise a normal ControlInfo output of 2..4 becomes 10..20 degrees in
	// one 1/30 s tick even on the shipped tank's 25 deg/s turret and walker
	// gunner's 120 deg/s turret. Mouse/player inputs retain their existing path.
	if (gunning) {
		const std::vector<GameWorld::SeatLayout> seats = m_world.vehicleSeats(self.vehicle);
		if (self.seat >= 0 && static_cast<size_t>(self.seat) < seats.size() && seats[static_cast<size_t>(self.seat)].yawSpeed > 0) {
			// Bot plans execute round-robin: a degree command may be held for
			// the physics ticks between decisions, so cap each command to one
			// 1/30-second simulation step instead of scaling by the elapsed plan
			// time passed into the controller.
			const float maxTurn = seats[static_cast<size_t>(self.seat)].yawSpeed / kInputTicksPerSecond;
			turn = std::clamp(turn, -maxTurn, maxTurn);
		}
	}
	float pitchStep = up * kInputDegreesPerTick * kInputTicksPerSecond * seconds * zoomScale;
	const float pitchErrorDegrees = ep * kDegrees;
	if ((pitchErrorDegrees >= 0 && pitchStep > pitchErrorDegrees) || (pitchErrorDegrees < 0 && pitchStep < pitchErrorDegrees)) {
		pitchStep = pitchErrorDegrees;
	}
	input.turn += turn;
	bot.pitch = std::clamp(truePitch + pitchStep, -kPitchLimit, kPitchLimit);
	input.aimPitch = bot.pitch;
}

// The fire plan's +0x248 (0x100795a0 -> bot vt+0x254(1), from either of
// its two MoveToFinding sites, AIDLL 0x100d11c0): instead of closing on the
// target, the plan walks to the spot while it fights. The builder decides
// it once per plan:
//   - the spot: the order's area position for the bot's map (0x100c1150,
//     when the bot's strategic area is set, 0x100c0ef0), replaced by the
//     head of the bot's goal object (bot slot 0x34 -> +0x80, the goal of
//     vt+0x38: its slot 8) when the goal's urgency where the bot stands
//     (slot 4) is over 0.1; without such a goal, no spot (0x100775f0);
//   - keep to it when the target can't be approached: a target without a
//     Mobile part (and no root that has one), with the bot's own Mobile
//     part (+0x3c), for which the pathfinder's slot 0x94 (0x1003fa60 ->
//     0x1003d460) finds none of 10 points evenly from the target to 30 m
//     towards the bot on the bot's map (its Mobile's slot 0x24), each looked
//     up with a height tolerance of 2.5 m for a soldier, 5 m otherwise;
//   - or the target ITAir and the bot's Armament plug-in's
//     setIsAntiAircraft (slot 0x28, 0x100f0840) at 2 (the tanks, APCs,
//     Titans and the static AP/AV guns); its value 1 sets a flag the plan
//     overwrites before it reads it (no effect);
//   - never with a close-combat weapon (type 7);
//   - otherwise when the bot stands more than 30 m from the spot (50 m
//     when neither the bot's object nor the target is ITSoldier) with the
//     target on the far side (the two directions' dot under 0).
// The builder's per-bot byte (the BB's +0x10) is taken as set.
bool BotManager::fireKeepsToOrder(const Bot& bot, const GameWorld::PlayerInfo& self, const GameWorld::PlayerInfo& target,
	physics::Vec3* spotOut) const
{
	// 0x100c0ef0: the squad's order under way with its area (0x100c1150);
	// outside squads, the SAI's area goal.
	const Squad* squad = self.squad ? this->squad(self.team, self.squad) : nullptr;
	const bool areaSpot = squad && !squad->members.empty()
		? squad->state == 2 && squad->area >= 0
		: bot.goalOrder >= 0 || bot.orderPosition.x != 0 || bot.orderPosition.y != 0 || bot.orderPosition.z != 0;
	if (!areaSpot || !bot.hasGoal || goalObjectUrgency(bot, self) <= 0.1f) {
		return false; // no spot (0x100775f0)
	}
	const physics::Vec3 spot = bot.goal; // the goal's head
	if (spotOut) {
		*spotOut = spot;
	}
	const WeaponAi weapon = weaponAi(weaponName(self, bot.weaponSlot));
	if (!self.vehicle && weapon.type == WeaponAi::CloseCombat) {
		return false;
	}
	const auto aiOf = [this](uint32_t object) {
		const world::Object* o = m_world.objects().find(object);
		return o && o->objectTemplate() ? aiTemplateOf(o->objectTemplate()->name()) : std::string();
	};
	const std::string ownAi = self.vehicle ? aiOf(self.vehicle) : aiOf(self.soldier);
	const AiPlugin* ownMobile = m_settings.plugin(ownAi, "Mobile");
	bool keep = false;
	bool targetMobile = !target.vehicle;
	std::string targetAi;
	if (target.vehicle) {
		targetAi = aiOf(target.vehicle);
		targetMobile = m_settings.plugin(targetAi, "Mobile") != nullptr;
	}
	if (!targetMobile && (ownMobile || m_settings.aiTemplates.empty())) {
		physics::Vec3 toward = self.position - target.position;
		const float length = std::sqrt(physics::lengthSquared(toward));
		toward = length > 1e-3f ? toward * (30 / length) : physics::Vec3 {};
		bool found = !m_navMesh;
		const float tolerance = self.vehicle ? 5.0f : 2.5f;
		for (int i = 0; i < 10 && !found; ++i) {
			found = m_navMesh->findPolygon(toAi(target.position + toward * (static_cast<float>(i) / 9)), tolerance) >= 0;
		}
		keep = !found;
	}
	if (!keep && target.vehicle && (m_settings.typeFlags(targetAi) & kITAir)) {
		const AiPlugin* armament = nullptr;
		if (self.vehicle && self.seat > 0) {
			const std::vector<GameWorld::SeatLayout> seats = m_world.vehicleSeats(self.vehicle);
			if (static_cast<size_t>(self.seat) < seats.size()) {
				armament = m_settings.plugin(aiTemplateOf(seats[static_cast<size_t>(self.seat)].name), "Armament");
			}
		}
		if (!armament) {
			armament = m_settings.plugin(ownAi, "Armament");
		}
		keep = armament && armament->antiAircraft == 2;
	}
	if (!keep) {
		const bool soldiers = !self.vehicle || !target.vehicle;
		const float reach = soldiers ? 30.0f : 50.0f;
		const physics::Vec3 toSpot = spot - self.position, toTarget = target.position - self.position;
		keep = physics::lengthSquared(toSpot) > reach * reach && physics::dot(toSpot, toTarget) < 0;
	}
	return keep;
}

// The guided plan's order spot (0x10078570, when it is built): the
// strategic area 0x10034a10 picks for the bot's team at its position (of
// the areas the team doesn't hold, +0x70 + 4 team not 0, the most radius /
// max(1, d^2) from the team's spot, slot 0x68), one with a control point
// (+0x164 not -1); its spot for the bot (0x1005d940: the bot's own
// position when it stands within 0.8 of the control point's radius, else
// one of 20 tries within that on the bot's mesh, the control point's own
// failing them; the vehicle map's order position for a bot on it). With
// that spot within the team's radius of the team's spot (+0x98 / +0x7c + 8
// team, across), the memo (0x100d8e40) of AND (0x100d49a0) of the area not
// held by the team (0x100db550: +0x70 + 4 team not 0; its test 0x100db580,
// slot 0x100db5b0) and the bot within
// 40 m of the spot across (0x100db3b0, ObjectDistance with its flag) starts
// an If over MoveToFinding (0x100d11c0) to the spot at the Mobile's speed;
// a copy of the memo (0x100d8f90) also opens the trigger's distance band
// (OR 0x100cf1b0). The rewrite's areas keep the team's spot and radius at
// the control point's (see pickWeapon).
bool BotManager::guidedOrderSpot(const GameWorld::PlayerInfo& self, physics::Vec3& spot)
{
	const GameWorld::ControlPointInfo* best = nullptr;
	float bestValue = -1;
	const std::vector<GameWorld::ControlPointInfo> points = m_world.controlPointList();
	for (const ai::StrategicArea& area : m_settings.strategy.areas) {
		for (const GameWorld::ControlPointInfo& point : points) {
			if (point.id != area.controlPoint || point.team == self.team) {
				continue;
			}
			const float d2 = horizontal2(self.position - point.position);
			const float value = d2 < 1 ? point.radius : point.radius / d2;
			if (bestValue < value) {
				bestValue = value;
				best = &point;
			}
		}
	}
	if (!best) {
		return false;
	}
	const float r = best->radius * 0.8f;
	if (horizontal2(self.position - best->position) <= r * r) {
		spot = self.position;
	} else {
		spot = best->position;
		for (int attempt = 0; attempt < 20; ++attempt) {
			const float angle = random() * 6.2831853f;
			const float distance = std::sqrt(random()) * r;
			const physics::Vec3 at = best->position + physics::Vec3 {std::sin(angle) * distance, 0, std::cos(angle) * distance};
			if (!m_navMesh || m_navMesh->findPolygon(toAi(at)) >= 0) {
				spot = at;
				break;
			}
		}
	}
	if (horizontal2(spot - best->position) >= best->radius * best->radius) {
		return false;
	}
	return horizontal2(spot - self.position) < 40.0f * 40.0f;
}

int guidedMoveStep(bool& engaged, float distance, float minRange, float maxRange)
{
	const float keep = 0.85f * maxRange, nearLimit = std::max(15.0f, 1.1f * minRange);
	if (!engaged) {
		engaged = distance > 0.5f * maxRange || distance <= minRange;
	}
	if (!engaged) {
		return 0;
	}
	if (distance > keep && keep > nearLimit) {
		return 1;
	}
	if (distance < nearLimit) {
		return -1;
	}
	engaged = false; // within [near limit, 0.85 maxRange]: the move is done
	return 0;
}

anim::ZoomSettings zoomSettingsOf(const world::ObjectTemplate* weapon)
{
	anim::ZoomSettings settings;
	if (!weapon) {
		return settings;
	}
	const auto& components = weapon->components();
	if (std::find(components.begin(), components.end(), "DefaultZoomComp") == components.end()) {
		return settings;
	}
	if (const auto it = weapon->settings().find("zoom.addZoomFactor.list"); it != weapon->settings().end()) {
		std::istringstream factors(it->second);
		for (float f; factors >> f;) {
			if (std::find(settings.factors.begin(), settings.factors.end(), f) == settings.factors.end()) {
				settings.factors.push_back(f); // duplicates skipped (0x005dae50)
			}
		}
	}
	settings.delay = settingNumber(weapon, "zoom.zoomDelay", 0.5f);
	settings.mouseSpeedMod = settingNumber(weapon, "zoom.mouseSpeedMod", 1);
	settings.zoomOutOnReload = settingNumber(weapon, "zoom.zoomOutOnReload", 1) != 0;
	return settings;
}

bool zoomStatementPress(bool moving, bool zoomed, bool ready)
{
	return ready && (moving ? zoomed : !zoomed);
}

// The guided weapons' plan on foot (0x10078570) decides its trigger in two
// steps. The trigger's If (0x100d29f0, forced, frozen) holds while the
// target is in view (the memo of the target conditions 0x100dc400 /
// 0x100dad60, left out for a weapon of type 10) and, by the bot's memory
// of it, lies within [0.9 minRange, 1.1 maxRange] (ObjectDistance
// 0x100d44c0); then the guided fire statements (0x100781e0), else only the
// guided aim (0x100d4f10). The statements, in a Parallell: the aim
// (0x100d4f10, destructor 0x100d4f60, its look direction 0x100d4fa0) at the target, and BAPATrigger (0x100d3110) on the weapon's
// fire input (its slot 0x64) held unless an OR (0x100cf1b0) holds: the
// chosen weapon not in hand (0x100da090's condition 0x100da0c0); for a
// heat-seeking or laser-guided weapon (guidance 1, 2) no lock (NOT
// 0x100d9fd0: the weapon's slot 0xac for the bot gives no object,
// 0x100da010); the look off the aim's point by more than 5 degrees for a
// laser-guided weapon, 3 otherwise (NOT 0x100d9d90, its 0x100d9e00:
// cos(angle) under the bound; DAT_101275c0 is pi/180, DAT_101275e0 5).
// `look` and `toTarget` are unit vectors (the bot's look, to the aim).
bool BotManager::guidedFire(const Bot& bot, const GameWorld::PlayerInfo& self, const WeaponAi& weapon, int guidance,
	const Known& known, bool inSight, const physics::Vec3& look, const physics::Vec3& toTarget) const
{
	const float memory = std::sqrt(physics::lengthSquared(known.position - self.position));
	const bool inView = weapon.type == WeaponAi::TitanWeapon || inSight;
	const bool inBand = memory >= 0.9f * weapon.minRange && memory <= 1.1f * weapon.maxRange;
	if (!inView || !(inBand || bot.orderMove)) {
		return false; // the order spot's memo opens the band (OR 0x100cf1b0)
	}
	if (self.inHandSlot != bot.weaponSlot) {
		return false;
	}
	if (guidance == 1 || guidance == 2) {
		const GameWorld::WeaponLockInfo lock = m_world.weaponLock(self.id);
		if (lock.target == 0 || lock.state != 2) {
			return false;
		}
	}
	const float bound = (guidance == 2 ? 5.0f : 3.0f) * 0.017453292f;
	return physics::dot(look, toTarget) >= std::cos(bound);
}

// BBPFireInfantery's plan (0x1007cf10, 0x100795a0): the chosen weapon in
// hand (ChangeWeapon 0x100db630, run by 0x100db6e0 through the select
// input 0x1008d740; in-hand conditions 0x100d7c80 / 0x100d7cb0 and, with
// it ready, 0x100db940 / 0x100db970; the fire patterns' per-bot base
// 0x100d7ed0, destructors 0x100d7e10 / 0x100d7e60; TriggerContinously's
// statement destructor 0x100d23a0), the aim on the target (with its lead), the distance kept within
// [minRange, 1.1 preferred], the weapon's firing pose taken when standing
// still (at most once a second, no fire 0.8 s after), and the trigger
// (Trigger 0x1008f1e0 taps, TriggerContinously 0x1008ecd0 holds) while
// the aim lies within allowedDeviation metres at the target (0x100d9090;
// the explosive plan's constructor 0x100d9130, the test 0x100d91d0):
// across, |d| L < max(0.1, r); up and down |dy| L < sqrt(0.2) r (0.02 for
// snipers); and within atan(0.02 max(r, 0.1)) of the line. Not while a
// teammate stands in the line of fire (radius 0.5 + 0.02 t); a second's
// hold when one crosses within 5 m, for firearms and snipers that are not
// thrown (0x100dad90; the okToFire test of 0x1008f1e0). A gun that heats
// (heatAddWhenFire > 0) stops at 0.8 and starts again at 0.5 (0x100d9000,
// 0x100d9040). Each round strays by the AI
// deviation (1 - acc)(1 - t/2) x deviation, t = time on the target / 15 s,
// acc = skill against soldiers (1 - 0.5 (1 - skill) against vehicles,
// 1 - 0.4 (1 - skill) with explosives, 1 - 0.8 (1 - skill) with them from
// a vehicle; 1 thrown) (TriggerContinously 0x1008ecd0 -> BF2142.exe
// 0x00892b70; x3 for a secondary bot object that isn't ITAir or ITNaval
// at an ITAir target: aiDeviationFactor). The game
// hands that to the firearm (interface 0x94c0 vt+0xf4 -> its deviation
// component's slot 0xac: the SoldierDeviationComp's +0x44, 0x00680290;
// the SimpleDeviationComp's +0x28, 0x008a1a90), which adds it to its own
// deviation while its holder is an AI (0x005de980, 0x005dedb0): the
// rounds stray by it (GameWorld::setAiDeviation). A firearm without a
// deviation component takes none.
void BotManager::fire(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds)
{
	GameWorld::PlayerInfo target;
	if (!m_world.playerInfo(bot.target, target) || !target.alive) {
		return;
	}
	const auto known = std::find_if(bot.known.begin(), bot.known.end(), [&](const Known& k) { return k.player == bot.target; });
	const bool inSight = known != bot.known.end() && known->lostAt < 0;
	if (known == bot.known.end()) {
		return;
	}
	// The plan tells the bot's squad of its target (0x100795a0 ->
	// 0x10089b70 -> 0x100c1ce0): each squadmate's memory gets it (0x100ed2c0,
	// not as the seer: a new item is flagged 8, known from others).
	if (bot.sharedTarget != bot.target) {
		bot.sharedTarget = bot.target;
		shareTarget(self, target);
		// A guided weapon's plan on foot keeps its own order spot.
		const bool guided = !self.vehicle && guidanceKind(m_world.templates().find(weaponName(self, bot.weaponSlot))) != 0;
		bot.orderMove = guided ? guidedOrderSpot(self, bot.orderSpot) : fireKeepsToOrder(bot, self, target, &bot.orderSpot);
	}
	const physics::Vec3 targetAt = (inSight ? target.position : known->position)
		+ physics::Vec3 {0, target.vehicle ? 1.0f : kChestHeight, 0};
	physics::Vec3 eye = self.position + physics::Vec3 {0, kEyeHeight, 0};
	float gunYaw = 0, gunPitch = 0;
	const bool gunning = self.vehicle && (self.seat > 0 || drivesGun(self)) && m_world.gunAim(self.vehicle, self.seat, gunYaw, gunPitch);
	physics::Mat3 hull;
	if (gunning) {
		m_world.gunFrame(self.vehicle, self.seat, eye, hull); // as the aim: from the gun, in the hull's frame
	}
	std::string weaponTemplate;
	int fireGroup = 0;
	int seatIndex = -1; // the seat weapon fought with (SeatLayout::weapons)
	if (gunning) {
		// The seat's chosen weapon: pressed on its own fire input (the AI
		// weapon's slot 0x60, BF2142.exe 0x008927c0: the fire component's
		// fireInput, BAPATrigger's button); one of the PIFire group that
		// isn't active is taken first, by its item key when it can be
		// selected, else by c_PIToggleWeapon (0x004d2a50).
		const auto seats = m_world.vehicleSeats(self.vehicle);
		const int active = m_world.seatWeaponIndex(self.vehicle, self.seat);
		const int chosen = bot.seatWeapon >= 0 ? bot.seatWeapon : active;
		seatIndex = chosen;
		if (static_cast<size_t>(self.seat) < seats.size()) {
			const GameWorld::SeatLayout& seat = seats[static_cast<size_t>(self.seat)];
			if (chosen >= 0 && static_cast<size_t>(chosen) < seat.weapons.size()) {
				const GameWorld::SeatWeapon& w = seat.weapons[static_cast<size_t>(chosen)];
				weaponTemplate = w.weapon;
				fireGroup = w.group;
				if (w.group == 0 && chosen != active) {
					if (w.selectable && w.itemIndex > 0) {
						input.selectItem = bot.seatSelectTap ? 0 : w.itemIndex;
					} else {
						input.toggleWeapon = !bot.seatSelectTap;
					}
					bot.seatSelectTap = !bot.seatSelectTap; // a press, then a release
					return;
				}
			} else {
				weaponTemplate = seat.weapon;
			}
		}
	} else if (self.vehicle) {
		return; // a seat without a gun its look turns (the FAV's driver)
	} else {
		weaponTemplate = weaponName(self, bot.weaponSlot);
		if (self.inHandSlot != bot.weaponSlot) {
			input.selectItem = bot.weaponSlot;
			return;
		}
	}
	const WeaponAi weapon = weaponAi(weaponTemplate);
	const float distance = std::sqrt(physics::lengthSquared(targetAt - eye));

	// Explosives against soldiers. The plan (0x100777a0) adds a start
	// condition when the weapon choice gave the bonus (+0x249), the weapon
	// has an explosion radius and the target is a soldier, and the target is
	// known only from others (flag 8), or over 30 m higher (not snipers or
	// thrown weapons), or the bot is no soldier. The aim (AIDLL 0x100dbd10) lobs a
	// soldier's explosive (the high arc of 0x100fa440, its +0x208 set) when
	// the target is seen and known from others or that high, unless a lob's
	// condition failed after the last sighting (+0x200 against the item's
	// +0x60), and for a weapon that isn't thrown only with the target over
	// 10 m higher or steeper than 0.6; otherwise it aims an explosive at the
	// ground under the target (0x100dbbd0, a ray 2 m down).
	const bool soldierBot = !self.vehicle;
	const bool targetSoldier = !target.vehicle;
	const float dh = known->position.y - self.position.y;
	const bool explosiveWeapon = weapon.explosionRadius > 0;
	const bool high = weapon.type != WeaponAi::Sniper && !weapon.thrown && dh > 30;
	const bool vehicleOnSoldier = !soldierBot && targetSoldier && explosiveWeapon;
	// A guided weapon's own plan on foot (0x10078570, which BBFire's plan
	// maker picks for an ITSoldier bot when the weapon's +0x6c, its
	// guidanceKind, isn't 0; see guidedFire): it has no explosive
	// conditions, firing pose or teammate test.
	const int guidance = soldierBot ? guidanceKind(m_world.templates().find(weaponTemplate)) : 0;
	const bool guidedPlan = guidance != 0;
	const bool explosivePlan = !guidedPlan && bot.explosive && explosiveWeapon && targetSoldier
		&& (known->secondHand || high || vehicleOnSoldier);
	const physics::Vec3 feet = inSight ? target.position : known->position;
	const bool aimExplosive = explosiveWeapon && known->everSeen
		&& (known->secondHand || high || (vehicleOnSoldier && !weapon.thrown));
	bool lob = false, lobReach = true;
	physics::Vec3 aimPoint = targetAt;
	if (aimExplosive) {
		physics::Vec3 toTarget = feet - self.position;
		const float length = std::sqrt(physics::lengthSquared(toTarget));
		const float steep = length > 1e-4f ? toTarget.y / length : 0.0f;
		lob = known->seenAt >= bot.lobFailedAt && !(vehicleOnSoldier && !weapon.thrown)
			&& !(explosiveWeapon && !weapon.thrown && dh <= 10 && steep <= 0.6f);
		aimPoint = feet;
		if (lob) {
			float speed = 0, gravity = 0;
			ballistics(weaponTemplate, speed, gravity);
			physics::Vec3 direction;
			lobReach = lobDirection(feet - eye, speed, gravity, direction);
			if (lobReach) {
				aimPoint = eye + direction * 10.0f;
			}
		}
	}
	bot.lobbing = lob; // vt+0x204
	if (!lob) {
		// +0x209 also falls when the plan's target condition (0x100dc730,
		// vtable 0x1011afd8; its point and sight test 0x100dc450) fails while not lobbing; that condition isn't
		// rewritten, so the hold ends once the aim stops lobbing.
		bot.lobHold = false;
	}

	// The distance kept (on foot); with +0x248 the plan's move goes to the
	// order's spot instead (MoveToFinding at the Mobile maxSpeed).
	bool moving = false;
	float walkYaw = 0;
	bool walkAside = false;
	if (!self.vehicle && bot.orderMove) {
		if (horizontal2(bot.orderSpot - self.position) > 2.0f * 2.0f) {
			// The walk's own look (steer's aim at its steering point) is not
			// the plan's: the aim controller's state stays the target's.
			const float pitch = bot.pitch, yawError = bot.yawError, pitchError = bot.pitchError;
			const physics::Vec3 aimPoint = bot.lastAimPoint;
			steer(bot, self, bot.orderSpot, 450000 + bot.goalOrder, seconds, input);
			bot.pitch = pitch;
			bot.yawError = yawError;
			bot.pitchError = pitchError;
			bot.lastAimPoint = aimPoint;
			// The way the walk wants; the aim keeps the look, so the walk
			// goes by throttle and strafe against it (below).
			walkYaw = self.yaw + input.turn;
			walkAside = true;
			input.turn = 0;
			moving = !bot.lobHold;
		}
	} else if (self.vehicle && self.driving) {
		if (driveFirePlan(bot, self, target, *known, inSight, weapon, explosivePlan, seconds, input)) {
			return; // the target was forgotten
		}
	} else if (guidedPlan) {
		// 0x10078570's move: If (not forced) the target, by the bot's memory
		// of it (ObjectDistance 0x100d4480 with its +0x14 set), lies farther
		// than half the weapon's maxRange (+0x16: "farther than") or within
		// its minRange: MoveToObjectFinding (0x100cff90, mode 2) keeping
		// 0.85 maxRange with the near limit max(15, 1.1 minRange): done when
		// the distance lies between the two, nearer it backs off. Between
		// minRange and half the reach the bot holds its ground.
		const float memory = std::sqrt(physics::lengthSquared(known->position - self.position));
		const int step = guidedMoveStep(bot.guidedMove, memory, weapon.minRange, weapon.maxRange);
		if (step > 0) {
			const float pitch = bot.pitch;
			steer(bot, self, target.position, 400000 + target.id, seconds, input);
			bot.pitch = pitch;
			input.turn = 0;
			moving = true;
		} else if (step < 0) {
			input.throttle = -1;
			moving = true;
		}
	} else if (!self.vehicle && weapon.type != WeaponAi::FireAndForget) {
		if (distance > 1.1f * std::max(bot.preferredRange, weapon.minRange + 1)) {
			// The aim turns the soldier and keeps its pitch (steering
			// levels the view, which would undo the aim every tick).
			const float pitch = bot.pitch;
			steer(bot, self, target.position, 400000 + target.id, seconds, input);
			bot.pitch = pitch;
			input.turn = 0;
			moving = !bot.lobHold;
		} else if (distance < weapon.minRange && !bot.lobHold) {
			input.throttle = -1;
			moving = true;
		}
	}
	if (guidedPlan) {
		// The guided aim point (0x100da710; made by 0x100da6b0 in the plans
		// 0x10078570 and 0x100795a0, +0x35 for TSWireGuided): T the target's
		// point, B the bot, R its guided round in flight (the bot's +0x34
		// part, slot 0x38) or B, s the round's speed. A soldier target moving
		// over 1 m/s whose distance less s is under the round's gets T
		// lowered by its pose (stand 1.21, crouch 0.73, prone 0.1 m). A
		// wire-guided round past the target by 20 m aims 10 m under it;
		// within 0.5 m of its distance at T; otherwise at T led by the
		// target's motion on x and z over the time to it at the closing
		// speed (|Vr - Vt|, Vr the bot's facing x 20 without a round) when
		// that is under 2.5 s. The lead is in the point: the aim leads no more.
		const Projectile* round = nullptr;
		for (const Projectile& p : m_world.projectiles()) {
			if (p.player == self.id && p.alive && !p.resting && p.settings && (p.settings->seek != ProjectileSettings::Seek::None || p.wireMaxDistance > 0)) {
				round = &p;
			}
		}
		const physics::Vec3 b = self.position;
		const float speed = round ? std::sqrt(physics::lengthSquared(round->velocity)) : 0;
		const bool flying = round && speed != 0;
		const physics::Vec3 r = flying ? round->position : b;
		const float dR = std::sqrt(physics::lengthSquared(b - r)), dT = std::sqrt(physics::lengthSquared(b - aimPoint));
		if (targetSoldier && dT - speed < dR && physics::lengthSquared(target.velocity) > 1) {
			aimPoint.y -= target.pose == SoldierPose::Prone ? 0.1f : (target.pose == SoldierPose::Crouch ? 0.73f : 1.21f);
		}
		if (guidance == 3) {
			if (dT + 20 < dR) {
				aimPoint.y -= 10;
			} else if (!(dT < dR + 0.5f)) {
				const physics::Vec3 vt = target.velocity;
				const physics::Vec3 vr = flying ? round->velocity
												: physics::Vec3 {std::sin(self.yaw * kRadians), 0, std::cos(self.yaw * kRadians)} * 20.0f;
				const float closing = std::sqrt(physics::lengthSquared(vr - vt));
				const float t = closing > 0 ? std::sqrt(physics::lengthSquared(aimPoint - r)) / closing : 1e9f;
				if (t < 2.5f) {
					aimPoint.x += vt.x * t;
					aimPoint.z += vt.z * t;
				}
			}
		}
	}
	aim(bot, self, aimPoint, inSight && !lob && !guidedPlan ? target.velocity : physics::Vec3 {}, true,
		lob || guidedPlan ? std::string() : weaponTemplate, input, seconds);
	if (walkAside) {
		// The walk to the spot while the look stays on the target (the
		// plan's MoveToFinding and aim run side by side; the rewrite's
		// soldier walks along its look, so the way goes to throttle and
		// strafe, right positive, against the aim's heading).
		const float rel = std::remainder(walkYaw - (self.yaw + input.turn), 360.0f) * kRadians;
		const float m = input.throttle;
		input.throttle = m * std::cos(rel);
		input.strafe = m * std::sin(rel);
	}

	// The firing pose, standing still and not turning (the SoldierPose
	// statement 0x1008e4e0 / 0x1008e4f0: standing while the throttle, strafe
	// or sprint input is pressed, else the statement's pose, vt+0x170).
	SoldierPose want = SoldierPose::Stand;
	if (weapon.firingPose == 2) {
		want = SoldierPose::Crouch;
	} else if (weapon.firingPose == 3) {
		want = SoldierPose::Prone;
	}
	if (guidedPlan) {
		input.pose = bot.pose; // the guided plan takes no firing pose
	} else if (!self.vehicle) {
		if (moving || std::fabs(input.turn) > 1) {
			want = SoldierPose::Stand;
		}
		if (bot.lobHold) {
			want = SoldierPose::Crouch; // steer's hold: vt+0x170(2, forced)
		}
		if (want != bot.pose && (m_time - bot.poseRequested >= 1 || bot.lobHold)) {
			bot.pose = want;
			bot.poseRequested = m_time;
		}
		input.pose = bot.pose;
	}

	// The gate.
	const float yaw = ((gunning ? gunYaw : self.yaw) - bot.devYaw + input.turn) * kRadians;
	const float pitch = bot.pitch * kRadians;
	physics::Vec3 cur {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
	if (gunning) {
		cur = hull.transform(cur);
	}
	const physics::Vec3 wantDir = (bot.lastAimPoint - eye) * (1 / std::max(std::sqrt(physics::lengthSquared(bot.lastAimPoint - eye)), 0.001f));
	const float r = weapon.allowedDeviation;
	const float across = (cur.x - wantDir.x) * (cur.x - wantDir.x) * distance * distance
		+ (cur.z - wantDir.z) * (cur.z - wantDir.z) * distance * distance;
	const float dy = (cur.y - wantDir.y) * distance;
	const float upDown = weapon.type == WeaponAi::Sniper ? 0.02f : 0.2f;
	bool onTarget = across < std::max(0.01f, r * r) && dy * dy < upDown * r * r && lobReach;
	if (weapon.type != WeaponAi::CloseCombat) {
		const float angle = std::acos(std::clamp(physics::dot(cur, wantDir), -1.0f, 1.0f));
		onTarget = onTarget && angle < std::atan(0.02f * std::max(r, 0.1f));
	}
	// Teammates in the line of fire.
	bool blocked = false;
	for (const GameWorld::PlayerInfo& mate : m_players) {
		if (mate.id == self.id || mate.team != self.team || !mate.alive) {
			continue;
		}
		const physics::Vec3 rel = mate.position + physics::Vec3 {0, kChestHeight, 0} - eye;
		const float t = physics::dot(rel, wantDir);
		if (t < 0 || t > distance) {
			continue;
		}
		const float perp2 = physics::lengthSquared(rel - wantDir * t);
		const float radius = weapon.type == WeaponAi::Sniper ? 0.5f : 0.5f + 0.02f * t;
		if (perp2 < radius * radius) {
			blocked = true;
			if (t < 5 && (weapon.type == WeaponAi::FireArm || weapon.type == WeaponAi::Sniper)) {
				bot.triggerAt = std::max(bot.triggerAt, m_time + 1);
			}
		}
	}
	const bool poseSettled = m_time - bot.poseRequested >= 0.8f || self.vehicle;
	const world::ObjectTemplate* gun = m_world.templates().find(weaponTemplate);
	if (settingNumber(gun, "heatAddWhenFire", 0) > 0) {
		if (!bot.heatStop && self.heat >= 0.8f) {
			bot.heatStop = true;
		} else if (bot.heatStop && self.heat <= 0.5f) {
			bot.heatStop = false;
		}
	} else {
		bot.heatStop = false;
	}
	// The conditions in the plan's order (the heat first, then a soldier's
	// explosive condition 0x100d8800 before the aim's, a vehicle's 0x100d8400
	// after it), each tried only while those before it hold.
	bool explosiveOk = true;
	if (explosivePlan && !bot.heatStop && (soldierBot || onTarget)) {
		explosiveOk = explosiveClear(bot, self, eye, soldierBot ? wantDir : cur, feet, weapon, weaponTemplate, soldierBot);
	}
	// The trigger's If (0x100d29f0, forced update): outside the explosive
	// plan the statements fire only with the target within [0.9 minRange,
	// 1.1 reach] (ObjectDistance 0x100d44c0 between the two objects); else
	// the plan's other branch (0x100d9740) only aims.
	const float objectDistance = std::sqrt(physics::lengthSquared(target.position - self.position));
	const float nearLimit = fireMinRange(weapon, dh, explosivePlan), farLimit = fireReach(weapon, dh);
	const bool inReach = explosivePlan
		|| (objectDistance >= 0.9f * nearLimit && (farLimit <= 0 || objectDistance <= 1.1f * farLimit));
	bool ready = m_time >= bot.triggerAt && onTarget && !blocked && poseSettled && (inSight || explosivePlan)
		&& !bot.heatStop && (!explosivePlan || explosiveOk) && inReach;
	if (guidedPlan) {
		ready = guidedFire(bot, self, weapon, guidance, *known, inSight, cur, wantDir);
	}
	// Trigger (taps) or TriggerContinously (holds) (0x100777a0): held only
	// when the weapon's fire mode (aiWeapon slot 0x104 -> BF2142.exe
	// 0x00892650: the fire component's +0x18 == 2) is automatic. The mode
	// starts at the first fire.addFireRate, 2 without one (0x005f28a0); the
	// bots never toggle it. Tapping presses on one plan execution and lets
	// go on the next.
	const bool automatic = fireMode(gun) == 2;
	if (guidedPlan) {
		input.fire = ready; // BAPATrigger (0x100d3110): held while nothing stops it
	} else if (weapon.type == WeaponAi::FireAndForget) {
		input.fire = ready && !bot.firedLast;
	} else if (automatic) {
		input.fire = ready;
	} else {
		input.fire = ready && !bot.tapped;
	}
	bot.tapped = input.fire;
	if (fireGroup == 1) {
		input.altFire = std::exchange(input.fire, false); // a PIAltFire weapon (the mechs' rockets)
	}
	// An empty magazine in hand on foot is reloaded here:
	//  - a guided weapon's own plan (0x10078570, for a weapon whose +0x6c
	//    isn't 0; the rewrite runs this plan for it) ends its sequence with
	//    a press of c_PIReload (BAPATrigger 0x100d3110 with button 0x20
	//    between the constant conditions 0x101166b0 / 0x10116648);
	//  - the simple plan (0x100777a0) has no reload. Its Trigger statements
	//    are done after a press and release (0x1008f1e0, 0x1008ecd0: the
	//    outputs 1/1), so the plan ends, the plan maker (0x1007cf10) builds
	//    another at the next think, and the executions in between run the
	//    weapon handling (0x10099b70, handleWeapon) without the fire plan's
	//    +0x188, which reloads the primary. The rewrite's fire plan keeps
	//    running and doesn't model those windows; it presses the reload
	//    for whatever weapon is in hand once its magazine is empty (the
	//    rewrite's stand-in, also for a sidearm the original would only swap
	//    for the primary there).
	if (!self.vehicle && self.roundsInMag == 0) {
		input.fire = false;
		input.reload = !bot.reloadedLast;
	}
	// A gun of a seat: the vehicle plan (0x100795a0) built while the
	// weapon's magazine is empty (the AI weapon's slot 0x14 not 1, its
	// adaptor's slot 0xe4, 0x00892580, 0) and without ammo.autoReload adds
	// If (the chosen weapon in hand, 0x100da090) BAPATrigger (0x100d3110)
	// on c_PIReload (input 32) between the constant conditions; the
	// rewrite's continuous plan presses it whenever that holds.
	if (gunning) {
		const FireArm* gun = m_world.seatFireArm(self.vehicle, self.seat, seatIndex);
		if (gun && gun->settings().magSize > 0 && gun->roundsInMag() == 0 && !gun->settings().autoReload && !gun->reloading()) {
			input.fire = input.altFire = false;
			input.reload = !bot.reloadedLast;
		}
	}
	// A bot in a squad reports its target to its squad as its trigger goes
	// down and comes up (AIDLL 0x1009a1d0, 0x1009a2a0 -> IAIGame vt+0x6c ->
	// BF2142.exe 0x008958a0: event 0x41, type 1; 30 s and 3 s on the
	// squad's minimap, which the rewrite doesn't show).
	if (self.squad != 0 && input.fire != bot.reportedFiring) {
		m_world.reportBattlefieldObjects(self.id, {bot.target});
	}
	bot.reportedFiring = input.fire;
	// The AI deviation on each round (BF2142.exe 0x00892b70).
	if (input.fire) {
		const float s = m_settings.skill;
		float acc = s;
		if (weapon.thrown) {
			acc = 1;
		} else if (weapon.explosionRadius > 0) {
			// 0x1008ecd0: 0.4 for a soldier (the bot's ITSoldier bit), 0.8
			// for a bot in a vehicle.
			acc = 1 - (self.vehicle ? 0.8f : 0.4f) * (1 - s);
		} else if (target.vehicle) {
			acc = 1 - 0.5f * (1 - s);
		}
		const float onIt = std::min(1.0f, (m_time - bot.targetAcquired) / 15.0f);
		// x3 (0x1008f03e..0x1008f043) for a bot object that isn't ITAir or
		// ITNaval and is secondary (its engine adaptor's slot 0xec: 0 for an
		// aiTemplate.secondary object, 0x0088d0d0's +0x19 -> vtables
		// 0x009d5db8 / 0x009d5f30, 0x006a10f0; 1 otherwise, 0x009d60a8 /
		// 0x009d6220, 0x006a10e0) at an ITAir target: the gunner pods and
		// second positions shooting at aircraft.
		const auto objectAi = [this](uint32_t vehicle, int seat, uint32_t soldier) {
			const uint32_t root = vehicle ? vehicle : soldier;
			const world::Object* o = m_world.objects().find(root);
			std::string ai = o && o->objectTemplate() ? aiTemplateOf(o->objectTemplate()->name()) : std::string();
			if (vehicle && seat > 0) {
				const std::vector<GameWorld::SeatLayout> seats = m_world.vehicleSeats(vehicle);
				if (static_cast<size_t>(seat) < seats.size()) {
					if (std::string seatAi = aiTemplateOf(seats[static_cast<size_t>(seat)].name); !seatAi.empty()) {
						ai = seatAi;
					}
				}
			}
			return ai;
		};
		const std::string botAi = objectAi(self.vehicle, self.seat, self.soldier);
		const std::string targetAi = objectAi(target.vehicle, 0, target.soldier);
		const float factor = aiDeviationFactor(m_settings.typeFlags(botAi), m_settings.primary(botAi), m_settings.typeFlags(targetAi));
		const float deviation = (1 - acc) * (1 - onIt / 2) * weapon.deviation * factor;
		// Handed to the weapon's deviation component (vt+0xf4 -> slot 0xac),
		// which adds it to its own while the holder is an AI: the rounds
		// spread by it through the deviation's normal numbers
		// (GameWorld::setAiDeviation). The aim itself stays on the target.
		m_world.setAiDeviation(self.id, deviation);
	}
	// The zoom statement (0x100d8250) for a soldier bot with a zoom on the
	// weapon: in when it stands still, out when it moves, with the weapon
	// ready (not reloading, the chosen weapon in hand: the firearm's busy
	// test 0x005c4e90 in the rewrite's terms).
	if (!self.vehicle && bot.zoomWeapon == weaponTemplate && bot.zoomSettings.present()) {
		const bool moving = input.throttle != 0 || input.strafe != 0 || input.sprint;
		const bool ready = !self.reloading && self.inHandSlot == bot.weaponSlot;
		bot.zoomPress = zoomStatementPress(moving, bot.zoom.level() > 0, ready);
	}
	if (bot.devYaw != 0) {
		input.turn -= bot.devYaw;
		bot.devYaw = 0;
		bot.devPitch = 0;
	}
}

void BotManager::shareTarget(const GameWorld::PlayerInfo& self, const GameWorld::PlayerInfo& target)
{
	if (self.squad == 0) {
		return;
	}
	for (auto& [id, mate] : m_bots) {
		GameWorld::PlayerInfo info;
		if (id == self.id || !m_world.playerInfo(id, info) || !info.alive || info.team != self.team
			|| info.squad != self.squad) {
			continue;
		}
		auto it = std::find_if(mate.known.begin(), mate.known.end(), [&](const Known& k) { return k.player == target.id; });
		if (it == mate.known.end()) {
			mate.known.push_back({});
			it = mate.known.end() - 1;
			it->player = target.id;
			it->secondHand = true;
			it->lostAt = m_time; // not in the squadmate's own sight
		}
		it->position = target.position;
		it->seen = m_time;
		it->seenAt = m_time;
		it->everSeen = true;
	}
}

// `ignore`: the explosive conditions' ray filter (0x100fba60, vtable
// 0x1011bac0: an object in its list is skipped, 0x100fba80).
bool BotManager::firstBlocked(const physics::Vec3& from, const physics::Vec3& to, uint32_t ignore, physics::Vec3& hit) const
{
	if (m_world.lineOfSight(from, to, ignore)) {
		return false;
	}
	float clear = 0, blocked = 1;
	for (int step = 0; step < 12; ++step) {
		const float mid = 0.5f * (clear + blocked);
		if (m_world.lineOfSight(from, from + (to - from) * mid, ignore)) {
			clear = mid;
		} else {
			blocked = mid;
		}
	}
	hit = from + (to - from) * blocked;
	return true;
}

// The AI weapon's +0x28 (its launch speed: the weapon's velocity, 0x00892be0
// from weapon vt+0x1b0) and +0x2c (the projectile's gravityModifier) times
// the world's gravity (DAT_10139460 vt+4).
// 0x00892be0 -> 0x0088a8c0: the AI weapon template's round. The speed is
// the firearm's velocity; the projectile's drag as roundDrag keeps it by
// its physicsType (0x006bf540: None 0, Point 1, RotationalPoint 2, Mesh 3,
// MechLegs 4); the gravity the world's (IAIPhysics slot 4) times its
// gravityModifier.
ai::RoundModel BotManager::roundModel(const std::string& weaponTemplate, bool* indirect) const
{
	ai::RoundModel round;
	ballistics(weaponTemplate, round.speed, round.gravity);
	const world::ObjectTemplate* t = const_cast<GameWorld&>(m_world).templates().find(weaponTemplate);
	const world::ObjectTemplate* projectile = nullptr;
	if (t) {
		const auto it = t->settings().find("projectileTemplate");
		projectile = it != t->settings().end() ? const_cast<GameWorld&>(m_world).templates().find(it->second) : nullptr;
	}
	int type = 0;
	if (projectile) {
		if (const auto it = projectile->settings().find("physicsType"); it != projectile->settings().end()) {
			const std::string& name = it->second;
			type = name == "Point" ? 1 : name == "RotationalPoint" ? 2 : name == "Mesh" ? 3 : name == "MechLegs" ? 4
				: std::atoi(name.c_str());
		}
	}
	round.drag = projectile ? ai::roundDrag(settingNumber(projectile, "drag", 0), settingNumber(projectile, "mass", 1), type) : 0.0f;
	if (indirect) {
		*indirect = weaponAi(weaponTemplate).indirect;
	}
	return round;
}

void BotManager::ballistics(const std::string& weaponTemplate, float& speed, float& gravity) const
{
	const world::ObjectTemplate* t = const_cast<GameWorld&>(m_world).templates().find(weaponTemplate);
	speed = t ? settingNumber(t, "velocity", FireArmSettings {}.velocity) : 0.0f;
	const world::ObjectTemplate* round = nullptr;
	if (t) {
		const auto it = t->settings().find("projectileTemplate");
		round = it != t->settings().end() ? const_cast<GameWorld&>(m_world).templates().find(it->second) : nullptr;
	}
	gravity = physics::tuning::kGravity * settingNumber(round, "gravityModifier", 1);
}

// The explosive start conditions (0x100777a0's statements round the
// trigger). A soldier's (0x100d8800, evaluated by 0x100d8910): none for
// 10 s after it last failed (+0x1f8); none with a soldier of the bot's team
// (the bot too) within 7 m of the target point and 2.5 m of its height;
// then the throw's way: a ray from the muzzle to 3 m along the aim (with
// the aim lobbing, to the arc's top, 0x100fa530: t = -vy / g, across
// t v and up 0.75 vy t), and from there to the target point. Clear when
// the first ray stops within 3.5 m of the target, or neither ray hits
// anything (the second ray's hit isn't read: 0x100d8910 measures from the
// first ray's empty one, so a blocked second ray fails). A failure stamps
// +0x1f8, or with the lob +0x200 (vt+0x1f4), which stops the lobbing until
// the target is seen again. A vehicle's (0x100d8400, 0x100d8450, with
// 3.5 m): the barrel's line to the target's distance + 5 m must hit
// something within 3.5 m of the target point with no soldier of the
// bot's team within 3.5 m (2.5 m of height) of the hit; a failure stamps
// +0x250 (vt+0x268).
bool BotManager::explosiveClear(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& muzzle,
	const physics::Vec3& aimDirection, const physics::Vec3& targetPoint, const WeaponAi& weapon,
	const std::string& weaponTemplate, bool soldierBot)
{
	const auto mateNear = [&](const physics::Vec3& at, float reach) {
		for (const GameWorld::PlayerInfo& p : m_players) {
			if (p.alive && !p.vehicle && p.team == self.team && std::fabs(p.position.y - at.y) < 2.5f
				&& physics::lengthSquared(p.position - at) < reach * reach) {
				return true;
			}
		}
		return false;
	};
	physics::Vec3 hit;
	if (!soldierBot) {
		const float reach = std::sqrt(physics::lengthSquared(targetPoint - muzzle)) + 5;
		const bool ok = firstBlocked(muzzle, muzzle + aimDirection * reach, self.vehicle, hit) && !mateNear(hit, 3.5f)
			&& physics::lengthSquared(hit - targetPoint) < 3.5f * 3.5f;
		if (!ok) {
			bot.vehicleFailedAt = m_time;
		}
		return ok;
	}
	if (m_time - bot.conditionFailedAt < 10 || mateNear(targetPoint, 7)) {
		return false;
	}
	physics::Vec3 end = muzzle + aimDirection * 3.0f;
	if (bot.lobbing) {
		float speed = 0, gravity = 0;
		ballistics(weaponTemplate, speed, gravity);
		const float vy = aimDirection.y * speed;
		if (vy > 0 && gravity < 0) {
			const float t = -vy / gravity;
			end = muzzle + physics::Vec3 {t * speed * aimDirection.x, 0.75f * vy * t, t * speed * aimDirection.z};
		}
	}
	bool ok = false;
	if (firstBlocked(muzzle, end, 0, hit)) {
		ok = physics::lengthSquared(hit - targetPoint) < 3.5f * 3.5f;
	} else {
		ok = !firstBlocked(end, targetPoint, 0, hit);
	}
	if (!ok) {
		(bot.lobbing ? bot.lobFailedAt : bot.conditionFailedAt) = m_time;
		if (bot.lobbing) {
			bot.lobHold = false; // vt+0x20c(0)
		}
	} else if (bot.lobbing && !weapon.thrown) {
		bot.lobHold = true; // vt+0x20c(1): a launcher's lob holds the moves
	}
	return ok;
}

// Keeps the path follower on `to`: a new goal (another id), or the same one
// moved more than 2 m (at most once a second: MoveToObjectFinding's stop
// condition AIDLL 0x100d0e60, its test 0x100d0e90, ends the move when its object leaves the radius;
// the 2 m are the rewrite's), plans again. After a failure (no path, or no
// progress) the next request waits a second (a think's delay in the
// original, which plans the move anew).
bool BotManager::follow(Bot& bot, const ai::NavMesh& mesh, const physics::Vec3& from, const physics::Vec3& to, int goalId,
	const ai::FollowerSettings& settings)
{
	const bool other = bot.followId != goalId;
	const bool moved = horizontal2(bot.followTo - to) > 4 && m_time - bot.followAt >= 1;
	if (bot.follower.failed() && m_time - bot.followAt < 1 && !other) {
		return false;
	}
	if (other) {
		bot.sidestep = ai::SidestepState(); // a new move object (+0x64 = -1, 0x100ce620)
	}
	if (other || moved || !bot.follower.planned() || bot.follower.failed()) {
		bot.follower.reset();
		bot.follower.request(mesh, toAi(from), toAi(to), settings, m_time);
		bot.followId = goalId;
		bot.followTo = to;
		bot.followAt = m_time;
	}
	return !bot.follower.failed();
}

// MoveTo on foot (BBPGotoWaypointInfantery 0x10066d80 plans Turn and
// MoveToFinding; InfanteryMoveTo 0x1008fc90 runs it): the follower's
// steering point, the look turned towards it 5 m ahead (the aim
// controller turns the soldier) and the throttle at speed / maxSpeed
// (0x100e6060; the order's speed is the soldier's top speed: 1). No
// strafing. The move ends within the goal's radius (0x100d10f0).
// MoveToFinding's statement: its step 0x100d1300 (the follower's path and
// way, 0x100966c0 / 0x10095720), destructor 0x100d12d0, follower accessors
// 0x100d1020 / 0x100d1040 / 0x100d1070 / 0x100d1090 and done test
// 0x100d10b0; the Turn (0x100d0fc0, run by 0x100d0f50 / 0x100d0fb0) hands
// the bot's sensing agent the point to look at (0x100d58b0).
// Without a mesh or a path the bot walks straight at the goal. The jump
// the follower asks for (AIDLL 0x10092bc0) holds PIAction for 0.4 s (see steer).
ai::SidestepSense BotManager::sidestepSense(const Bot& bot, const physics::Vec3& at, uint32_t own, const ai::NavMesh& mesh,
	const ai::FollowerSettings& settings, const physics::Vec3& steerPoint, const physics::Vec3& goal, uint32_t object) const
{
	ai::SidestepSense sense;
	sense.position = toAi(at);
	sense.self = static_cast<int>(own);
	sense.steer = toAi(steerPoint);
	sense.ahead = bot.follower.pointAhead(toAi(at), 10); // 0x100941c0
	sense.goal = toAi(goal);
	sense.target = object ? static_cast<int>(object) : -1;
	// +0xe8: the vehicle Change heads for (0x100d4d60); +0xec: the one
	// RepairVehicle works on (0x10073da0).
	sense.related[0] = bot.vehicleTarget ? static_cast<int>(bot.vehicleTarget) : -1;
	sense.related[1] = bot.repairTarget ? static_cast<int>(bot.repairTarget) : -1;
	sense.mesh = &mesh;
	sense.profile = &settings.options.profile;
	sense.heightTolerance = settings.options.heightTolerance;
	// The world list: the vehicles' AI objects (the soldiers' are flagged
	// ITSoldier and never stepped round). Their room is the box's centre
	// radius; ITObstacle comes from their AI template, and the Mobile part
	// from its plug-in (a vehicle without AI scripts counts as mobile).
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		const world::Object* o = m_world.objects().find(v.object);
		const std::string ai = o && o->objectTemplate() ? aiTemplateOf(o->objectTemplate()->name()) : "";
		ai::SidestepObstacle obstacle;
		obstacle.id = static_cast<int>(v.object);
		obstacle.position = toAi(v.position);
		obstacle.radius = vehicleBox(v).centreRadius;
		obstacle.health = healthOf(v.object);
		const uint32_t types = m_settings.typeFlags(ai);
		obstacle.fixed = (types & kITObstacle) != 0;
		obstacle.soldier = (types & kITSoldier) != 0;
		obstacle.mobile = ai.empty() || !m_settings.aiTemplates.count(lower(ai)) || m_settings.plugin(ai, "Mobile");
		sense.obstacles.push_back(obstacle);
	}
	return sense;
}

bool BotManager::steer(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& to, int goalId, float seconds,
	SoldierInput& input, float radius, uint32_t object)
{
	bot.pitch = 0;
	input.aimPitch = 0;
	// The jump: PIAction (input 9) pressed by the follower's tick (0x10095720,
	// when the stuck check of AIDLL 0x10092bc0 left its stamp +0xc0 at -666) and
	// let go once the stamp is 0.4 s old (0x10092bc0, between 0.4 and 1.5 s).
	input.jump = m_time < bot.jumpUntil;
	if (radius > 0 && physics::lengthSquared(to - self.position) <= radius * radius) {
		return false;
	}
	if (bot.lobHold) {
		// A lob's condition held (+0x209, 0x100e6060): the move's timers
		// restart (0x100923a0), the bot asks to crouch (vt+0x170(2, 1),
		// forced past the second between poses) and pushes nothing.
		bot.follower.progress(toAi(self.position), 0, m_time);
		if (bot.pose != SoldierPose::Crouch) {
			bot.pose = SoldierPose::Crouch;
			bot.poseRequested = m_time;
		}
		input.pose = bot.pose;
		input.throttle = 0;
		input.strafe = 0;
		return true;
	}
	physics::Vec3 point = to;
	bool steered = false;
	physics::Vec3 d;
	if (m_navMesh) {
		ai::FollowerSettings settings;
		settings.soldier = true;
		settings.radius = kSoldierRadius;
		settings.climb = m_settings.climbLimits(0);
		if (follow(bot, *m_navMesh, self.position, to, goalId, settings)) {
			const ai::PathFollower::Steer steer = bot.follower.steer(*m_navMesh, toAi(self.position), m_time);
			if (steer.arrived) {
				return false;
			}
			if (steer.jump) {
				bot.jumpUntil = m_time + 0.4f;
				input.jump = true;
			}
			if (steer.ok) {
				point = fromAi(steer.point);
				// The steering with its sidestep round vehicles (0x100ce860):
				// a goal inside one's room, the bot as close as it gets,
				// ends the move (InfanteryMoveTo stops, 0x100e6120).
				ai::Vec3 way;
				if (ai::sidestepSteer(bot.sidestep,
						sidestepSense(bot, self.position, self.soldier, *m_navMesh, settings, point, to, object), way)) {
					return false;
				}
				d = fromAi(way);
				steered = true;
			}
		}
	}
	if (!steered) {
		d = point - self.position;
		d.y = 0;
		const float length = std::sqrt(horizontal2(d));
		d = length > 0.001f ? d * (1 / length) : physics::Vec3 {std::sin(self.yaw * kRadians), 0, std::cos(self.yaw * kRadians)};
	}
	// The way to the sensing agent (0x1008f8e0: +0x5c / +0x60, the pause's
	// stamp cleared).
	bot.look.moveX = d.x;
	bot.look.moveZ = d.z;
	bot.look.pausedAt = 0;
	if (!self.vehicle && !bot.look.paused) {
		// The look is the agent's (BotsLook.cpp); the steering statement
		// (0x100cd3b0) turns the way into throttle and strafe against the
		// view: clamp(sign(c) sqrt|c|) for the forward and the side
		// components, times the speed.
		const float fx = std::sin(self.yaw * kRadians), fz = std::cos(self.yaw * kRadians);
		const float ahead = fx * d.x + fz * d.z, side = fz * d.x - fx * d.z;
		const auto root = [](float c) { return std::clamp(std::copysign(std::sqrt(std::fabs(c)), c), -1.0f, 1.0f); };
		input.throttle = root(ahead) * bot.walkFactor;
		input.strafe = root(side) * bot.walkFactor;
		bot.zoomPress = bot.zoom.level() > 0 && !bot.zoomKey;
		bot.follower.progress(toAi(self.position), std::fabs(input.throttle) + std::fabs(input.strafe), m_time);
		return true;
	}
	// The member's speed over the soldier's (0x100e6060): the squad's
	// (0x100f4740, the slowest x 0.9) for a leader under way in formation.
	input.throttle = bot.walkFactor;
	// InfanteryMoveTo and InfanteryMoveToObject (0x1008fc90, 0x1008f920)
	// zoom out on every step (0x100e5f70): the weapon's zoom key pressed
	// while it is zoomed and the key was let go, else let go.
	bot.zoomPress = bot.zoom.level() > 0 && !bot.zoomKey;
	const physics::Vec3 eye = self.position + physics::Vec3 {0, kEyeHeight, 0};
	bot.look.quiet = true; // the walk's own look (a plan's aim holds the agent)
	// The walk's steering statement (0x100cd3b0, a plan's own, 0x10079489)
	// runs the controller (0x100cd4f3) with its own last errors (+0x18 /
	// +0x1c) and no lead: the plan's aim keeps its errors, aim point and
	// pitch history, or its derivative and bias read the walk's look.
	const physics::Vec3 aimPoint = bot.lastAimPoint;
	const int aimTarget = bot.lastAimTarget;
	const float yawError = bot.yawError, pitchError = bot.pitchError;
	const std::array<float, 10> pitchErrors = bot.pitchErrors;
	const int pitchErrorCount = bot.pitchErrorCount;
	aim(bot, self, eye + d * 5.0f, {}, false, {}, input, seconds);
	bot.lastAimPoint = aimPoint;
	bot.lastAimTarget = aimTarget;
	bot.yawError = yawError;
	bot.pitchError = pitchError;
	bot.pitchErrors = pitchErrors;
	bot.pitchErrorCount = pitchErrorCount;
	bot.look.quiet = false;
	bot.follower.progress(toAi(self.position), std::fabs(input.throttle), m_time);
	return true;
}

physics::Vec3 backOffSpot(const physics::Vec3& target, physics::Vec3 forward, float near, float keep,
	const std::function<bool(const physics::Vec3&)>& clear)
{
	forward.y = 0;
	const float flat = std::sqrt(forward.x * forward.x + forward.z * forward.z);
	forward = flat > 1e-4f ? forward * (1 / flat) : physics::Vec3 {0, 0, 1};
	const float length = near + std::min(10.0f, (keep - near) * 0.2f);
	const physics::Vec3 ways[3] = {forward, {forward.z, 0, -forward.x}, {-forward.z, 0, forward.x}};
	for (const physics::Vec3& way : ways) {
		const physics::Vec3 spot = target + way * length;
		if (clear(spot)) {
			return spot;
		}
	}
	return target + ways[0] * length; // none clear: the first (the original keeps the farthest ray hit)
}

// The fire plan's moves for a bot object with a Mobile part (0x100795a0,
// LAB_10079dde; the plan's statements named by their debug printers). The
// moves run in a Parallell combiner (0x100cddb0; its run 0x100cdca0, add
// 0x100cdfb0 with list nodes 0x100cdaa0 / 0x100cdb30 / 0x100cdbb0,
// destructors 0x100cdd70 / 0x100cdef0 / 0x100ce040) beside the trigger's If:
//  - If (0x100d29f0, forced update, the false branch frozen) the target
//    isn't in view (OR of NOT over the memo 0x100d8e40 of the target
//    conditions 0x100dc400 / 0x100dad60, the latter's test 0x100db150: the
//    memory item first-hand and no teammate in the line, 0x100dad90; the If's
//    destructor 0x100d2d60, NOT's test 0x100d2f60 and destructor 0x100d2ee0;
//    the target alive: NOT ObjectHealth <= 0, 0x100da2b0 / 0x100da320): a Serial combiner (0x100ce370;
//    its run 0x100ce210, destructor 0x100ce2e0) of
//    MoveToObjectFinding (0x100cff90) at the target with no distance kept,
//    then ForgetObject (0x100da5f0: the memory item dropped, 0x100ec400),
//    then a failing statement (0x100da480 with mode 0) that ends the plan.
//    A target without a Mobile part is driven at by MoveToFinding
//    (0x100d11c0) to the spot beside it instead.
//  - else If (not forced) NOT ObjectDistance (0x100d44c0, 0x100d4620) holds
//    the target within [minRange, 1.1 D]: MoveToObjectFinding with D kept
//    (the plan's distance, preferredRange), its near limit ec = min(1.1
//    minRange, D). It ends when ec <= d <= D (0x100cfe20 on the squared
//    distance, 0x100f9970; its base MoveToObject 0x100f9830, vtable
//    0x10119dd0, is done within D or on its abort condition, 0x100f98a0,
//    and prints its name and distance, 0x100f9900; its distance to the
//    object 0x100d0700, MoveToObjectFinding's step 0x100d07a0 re-paths at
//    the moving target, the base's slot 7 0x100d2400). At ec or beyond it paths at the target; nearer
//    it backs off (0x100d00e0) to the first clear of three spots ec + min(10,
//    0.2 (D - ec)) from the target: ahead of it (its forward, a driver being
//    no ITSoldier), then to either side (the original rays each from the
//    target, 0x101394d4 slot 0x84; the rewrite asks the vehicle mesh), and
//    chooses again each second (+0x100).
// The rewrite drives with the vehicles' MoveTo (drive) at the Mobile
// maxSpeed (the move's half-speed approach isn't modelled).
bool BotManager::driveFirePlan(Bot& bot, const GameWorld::PlayerInfo& self, const GameWorld::PlayerInfo& target,
	const Known& known, bool inSight, const WeaponAi& weapon, bool explosivePlan, float seconds, SoldierInput& input)
{
	const float dh = known.position.y - self.position.y;
	const float lo = fireMinRange(weapon, dh, explosivePlan);
	const float keep = bot.preferredRange > 0 ? bot.preferredRange : lo + 0.5f * (fireReach(weapon, dh) - lo);
	const float d = std::sqrt(physics::lengthSquared(target.position - self.position));
	const auto stop = [&] {
		input.throttle = 0;
		input.strafe = 0;
	};
	if (!inSight || bot.chase) {
		bot.manoeuvre = false;
		bot.chase = true; // the then-branch runs to its end
		if (!drive(bot, self, target.position, 400000 + target.id, seconds, input)) {
			// Arrived: ForgetObject, and the failing statement ends the plan.
			bot.chase = false;
			bot.known.erase(std::remove_if(bot.known.begin(), bot.known.end(), [&](const Known& k) { return k.player == target.id; }),
				bot.known.end());
			bot.target = -1;
			bot.planTarget = -1;
			stop();
			return true;
		}
		return false;
	}
	if (!bot.manoeuvre && (d < lo || d > 1.1f * keep)) {
		bot.manoeuvre = true;
		bot.backOffAt = -1e9f;
	}
	if (!bot.manoeuvre) {
		stop();
		return false;
	}
	const float near = std::min(1.1f * lo, keep);
	if (d >= near && d <= keep) {
		bot.manoeuvre = false; // arrived (0x100cfe20)
		stop();
		return false;
	}
	if (d >= near) {
		drive(bot, self, target.position, 400000 + target.id, seconds, input);
		return false;
	}
	if (m_time - bot.backOffAt > 1.0f) {
		bot.backOffAt = m_time;
		physics::Vec3 forward {std::sin(target.yaw * kRadians), 0, std::cos(target.yaw * kRadians)};
		for (const GameWorld::VehicleInfo& v : m_vehicles) {
			if (target.vehicle && v.object == target.vehicle) {
				forward = {v.forward.x, 0, v.forward.z};
			}
		}
		const ai::NavMesh* mesh = m_vehicleNavMesh;
		bot.backOff = backOffSpot(target.position, forward, near, keep,
			[mesh](const physics::Vec3& spot) { return !mesh || mesh->findPolygon(toAi(spot)) >= 0; });
	}
	if (!drive(bot, self, bot.backOff, 410000 + target.id, seconds, input)) {
		bot.backOffAt = -1e9f; // at the spot: choose again
	}
	return false;
}

// The Mobile plug-in's driving settings for a vehicle (its root aiTemplate),
// the shipped FAV's without one. The Unit plug-in's equipment picks the
// interpreter: the MoveTo statement its aiSettings.addInterpreterEntry
// lines name (AIBehaviours.ai: Car and ArmedCar CarMoveTo, Mech
// MechMoveTo, FloatingTank FloatingTankMoveTo for BotManager::drive, Tank
// TankMoveTo; BotSettings::moveInterpreter).
ai::DriveSettings BotManager::driveSettings(const GameWorld::PlayerInfo& self) const
{
	ai::DriveSettings d;
	const world::Object* vehicle = m_world.objects().find(self.vehicle);
	const std::string ai = vehicle && vehicle->objectTemplate() ? aiTemplateOf(vehicle->objectTemplate()->name()) : "";
	if (const AiPlugin* m = m_settings.plugin(ai, "Mobile")) {
		d.maxSpeed = m->maxSpeed > 0 ? m->maxSpeed : d.maxSpeed;
		d.spaceNeededTweak = m->spaceNeededTweak;
		d.speedAtZeroSpace = m->speedAtZeroSpace;
		d.backingSpeed = m->backingSpeed;
		d.brakeDistance = m->brakeDistance;
		d.maxBrakePercentage = m->maxBrakePercentage;
		d.lookAheadTime = m->lookAheadTime;
		d.minSpeedForLookAhead = m->minSpeedForLookAhead;
		d.minAngleDiffForSlowDown = m->minAngleDiffForSlowDown;
		d.angleSlowDownScale = m->angleSlowDownScale;
		d.maxAngleDiffSlowDown = m->maxAngleDiffSlowDown;
	}
	if (const AiPlugin* c = m_settings.plugin(ai, "ControlInfo")) {
		d.yawScale = c->yawScale;
	}
	const std::string move = m_settings.moveInterpreter(equipmentOf(self));
	d.driver = move == "CarMoveTo" ? ai::Driver::Car : move == "MechMoveTo" ? ai::Driver::Mech : ai::Driver::Tank;
	return d;
}

// MoveTo at the wheel (BBPGotoWaypoint2d 0x10068980, named "GotoWaypoints2d"
// 0x10068960, constructor 0x10068940; CarMoveTo 0x10091050,
// TankMoveTo 0x10091b40): the follower on the vehicle mesh, then
// ai::driveControl with the mesh rays round the vehicle (0x100e8170) and
// the other vehicles as obstacles (the AI objects of the bots' world
// list). Without a path it brakes (0x100ea170; the tank's reset AIDLL 0x100e0930,
// the mech's 0x100e3f00 lets go); an obstacle on the goal ends the move.
// The hover tank (FloatingTankMoveTo 0x1008c7c0) has no state machine:
// ai::floatingTankControl strafes it along the way to the steering point.
// The MoveToObject interpreters are the same drive with the object's
// position as the goal, braking on the gap to it less its radius and
// 0.5 m (CarMoveToObject 0x10090b30 / 0x10090b40, TankMoveToObject
// 0x10091680 / 0x10091690, MechMoveToObject 0x1008cd70 / 0x1008cd80,
// FloatingTankMoveToObject 0x1008c330 / 0x1008c340); the resets are the
// brake below (CarResetControls 0x100909f0 / 0x10090a00 -> 0x100ea170,
// TankResetControls 0x10091550 / 0x10091570 -> 0x100e0930). The MoveTo
// interpreters' constructors: CarMoveTo 0x10091040, TankMoveTo
// 0x10091b30, MechMoveTo 0x1008d240, FloatingTankMoveTo 0x1008c7b0.
bool BotManager::drive(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& to, int goalId, float seconds,
	SoldierInput& input, float radius)
{
	const GameWorld::VehicleInfo* vehicle = nullptr;
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		vehicle = v.object == self.vehicle ? &v : vehicle;
	}
	if (!vehicle) {
		return true;
	}
	ai::DriveSettings settings = driveSettings(self);
	const VehicleBox box = vehicleBox(*vehicle);
	settings.radius = box.radius;
	settings.centreRadius = box.centreRadius;
	settings.width = box.width;
	settings.length = box.length;
	const physics::Vec3 at = vehicle->position;
	physics::Vec3 forward = vehicle->forward;
	forward.y = 0;
	const float flat = std::sqrt(horizontal2(forward));
	forward = flat > 1e-4f ? forward * (1 / flat) : physics::Vec3 {0, 0, 1};
	const float heading = std::atan2(forward.x, forward.z);
	ai::DriveSense sense;
	sense.now = m_time;
	sense.speed = physics::dot(vehicle->velocity, forward);
	if (bot.lastYawAt >= 0 && m_time > bot.lastYawAt) {
		sense.yawRate = std::remainder(heading - bot.lastYaw, 2 * 3.14159265f) / (m_time - bot.lastYawAt);
	}
	bot.lastYaw = heading;
	bot.lastYawAt = m_time;
	sense.tilt = std::max(std::fabs(physics::dot(vehicle->angularVelocity, vehicle->right)),
		std::fabs(physics::dot(vehicle->angularVelocity, vehicle->forward)));
	sense.position = toAi(at);
	sense.forward = toAi(forward);
	sense.right = {forward.z, 0, -forward.x};
	sense.velocity = toAi(vehicle->velocity);
	sense.goal = toAi(to);
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		if (v.object == vehicle->object) {
			continue;
		}
		const VehicleBox other = vehicleBox(v);
		const physics::Vec3 up {v.forward.y * v.right.z - v.forward.z * v.right.y, v.forward.z * v.right.x - v.forward.x * v.right.z,
			v.forward.x * v.right.y - v.forward.y * v.right.x};
		const physics::Vec3 centre = v.position + v.right * other.centre.x + up * other.centre.y + v.forward * other.centre.z;
		sense.obstacles.push_back(
			{static_cast<int>(v.object), toAi(v.position), toAi(centre), toAi(v.velocity), other.radius, other.centreRadius});
	}
	const bool floating = m_settings.moveInterpreter(equipmentOf(self)) == "FloatingTankMoveTo";
	const physics::Vec3 lastVelocity = bot.lastVelocity;
	bot.lastVelocity = vehicle->velocity;
	const auto brake = [&] {
		input.strafe = 0;
		if (floating || settings.driver == ai::Driver::Tank) {
			input.throttle = ai::tankStopThrottle(sense.speed); // AIDLL 0x100e0930
		} else if (settings.driver == ai::Driver::Mech) {
			input.throttle = 0; // 0x100e3f00: yaw, pitch, roll and throttle let go
		} else {
			input.throttle = std::fabs(sense.speed) <= 1 ? 0.0f : (sense.speed > 0 ? -1.0f : 1.0f);
		}
	};
	if (radius > 0 && horizontal2(to - at) <= 0.25f * radius * radius) {
		brake(); // within half the goal's radius (0x10068980)
		return false;
	}
	physics::Vec3 point = to;
	sense.remaining = std::sqrt(horizontal2(to - at));
	ai::FollowerSettings floatingFollower;
	if (m_vehicleNavMesh) {
		ai::FollowerSettings follower;
		follower.soldier = false;
		// The follower's radius (+0x100, set by the request 0x10095fc0 from
		// the unit's physical part, +0x40 vt+0x30): the box's centre radius,
		// not the origin's that the drive's rays and room use (vt+0x28).
		follower.radius = settings.centreRadius;
		follower.options.vehicle = true;
		follower.climb = m_settings.climbLimits(1);
		if (bot.vehicleApproach && bot.follower.failed() && bot.followId == goalId) {
			// A complete mesh route is still only a plan. If physical travel
			// fails, dismount here only when infantry has a complete route to
			// the objective within the bounded walking distance.
			const bool onFoot = infantryRecoveryRoute(m_navMesh, toAi(at), toAi(bot.approachInfantryGoal));

			bot.goal = at;
			bot.via.clear();
			bot.follower.reset();
			bot.followId = -1;
			if (!onFoot) {
				if (bot.goalControlPoint >= 0) bot.rejectedVehiclePoints[bot.goalControlPoint] = m_time + 30;
				bot.vehicleApproach = false;
				bot.orderRegoal = true;
			}
			brake();
			return true;
		}
		if (!follow(bot, *m_vehicleNavMesh, at, to, goalId, follower)) {
			brake();
			// No progress for 20 s (0x10092bc0): the move fails and the
			// behaviour plans again (0x100c91c0 hands the stuck bot to the
			// game, IAIGameEffector vt+0x24 -> 0x0089e940 -> GameLogic slot
			// 0x58, 0x008924f0, which only returns 0: nothing to rewrite).
			return true;
		}
		const ai::PathFollower::Steer steer = bot.follower.steer(*m_vehicleNavMesh, toAi(at), m_time);
		if (steer.arrived) {
			brake();
			return false;
		}
		if (!steer.ok || steer.wait) {
			brake();
			return true;
		}
		point = fromAi(steer.point);
		sense.remaining = bot.follower.remaining(toAi(at));
		sense.maxBend = bot.follower.maxBend(toAi(at), toAi(forward), sense.speed, settings.lookAheadTime, m_time);
		const ai::NavMesh& mesh = *m_vehicleNavMesh;
		// The drivers' rays (0x100e8170: vslot 0x84 with 1000 polygons, no
		// width, stopping at cost class 6, the class entered last, the
		// climb test on).
		const ai::ClimbLimits climb = follower.climb;
		sense.ray = [&mesh, climb](const ai::Vec3& from, const ai::Vec3& end) {
			ai::DriveRay ray;
			if (mesh.findPolygon(from) < 0) {
				return ray; // off the mesh (a spawn spot beside it): nothing known
			}
			ai::RayHit hit;
			ray.clear = ai::meshRaycast(mesh, from, end, nullptr, -1, &ray.distance, -1, 0, 6, &climb, &hit);
			ray.lastClass = hit.lastClass;
			return ray;
		};
		floatingFollower = follower;
	}
	sense.goalDistance = std::sqrt(horizontal2(point - at));
	sense.error = std::remainder(std::atan2(point.x - at.x, point.z - at.z) - heading, 2 * 3.14159265f);
	if (floating) {
		physics::Vec3 way = point - at;
		way.y = 0;
		const float length = std::sqrt(horizontal2(way));
		way = length > 0.001f ? way * (1 / length) : physics::Vec3 {1, 0, 0}; // (1, 0) on the point (AIDLL 0x100ce860)
		if (m_vehicleNavMesh) {
			// The infantry steering with its sidestep (0x100ce860); the move
			// ends when it says so (0x1008c7c0 stops the tank, 0x100e0930).
			ai::Vec3 sidestep;
			if (ai::sidestepSteer(bot.sidestep,
					sidestepSense(bot, at, vehicle->object, *m_vehicleNavMesh, floatingFollower, point, to, 0), sidestep)) {
				brake();
				return false;
			}
			way = fromAi(sidestep);
		}
		const physics::Vec3 acceleration = seconds > 0 ? (vehicle->velocity - lastVelocity) * (1 / seconds) : physics::Vec3 {};
		const ai::FloatingTankOutput out = ai::floatingTankControl(settings, sense, toAi(way), toAi(acceleration));
		input.throttle = out.throttle;
		input.strafe = out.strafe; // PIYaw: sideways on the hover tank
		bot.follower.progress(toAi(at), std::max(std::fabs(out.throttle), std::fabs(out.strafe)), m_time);
		return true;
	}
	const ai::DriveOutput out = ai::driveControl(settings, bot.drive, sense);
	if (out.stop) {
		brake(); // something sits on the goal (0x10091050 on 666)
		return false;
	}
	input.throttle = out.throttle;
	input.strafe = out.turn;
	input.sprint = input.sprint || out.sprint;
	bot.follower.progress(toAi(at), std::max(std::fabs(out.throttle), std::fabs(out.turn)), m_time);
	horn(bot, self, *vehicle, input);
	return true;
}

// The horn (AIDLL 0x100a5c00, also the driving plan's horn statement
// 0x100cfbd0, made by 0x100cfb80; from the end of 0x100fc660's scan of what is
// ahead; let go by 0x100a5cf0 through 0x100a7af0 every frame). Moving at
// 2 m/s or more along its motion, the driver looks at the soldiers of the
// world list (alive, with a Mobile part): one of its own team (slot 0x40)
// ahead (dot above 0), at most 15 m beyond both radii, whose sphere the
// line of travel crosses, is the last such one found. Then, with the
// vehicle's driver's seat taken (0x100aa530), at most every 5 s (20 s
// when that soldier's player is a bot, player slot 0x84): a vehicle whose
// AI template has ITHasCarHorn (the FAVs' Car_horn) gets PIFire (input 8,
// when the input has it) for 2 s; any other sends the radio request
// (0x100a5990: PLAYER_TACTICS_gogogo on the AI's channel 6, TCLocalTeam,
// to that player). The stamp (comm object +0x80) is taken either way.
void BotManager::horn(Bot& bot, const GameWorld::PlayerInfo& self, const GameWorld::VehicleInfo& vehicle, SoldierInput& input)
{
	const world::Object* object = m_world.objects().find(vehicle.object);
	const bool hasHorn = object && object->objectTemplate()
		&& (m_settings.typeFlags(aiTemplateOf(object->objectTemplate()->name())) & kITHasCarHorn) != 0;
	physics::Vec3 forward = vehicle.forward;
	const float speed = physics::dot(vehicle.velocity, forward);
	const float motion = speed > 0 ? 1.0f : speed < 0 ? -1.0f : 0.0f;
	bot.hornHeld = false;
	if (hasHorn && m_time - bot.hornAt <= 2) {
		input.fire = true; // held until 0x100a5cf0 lets go 2 s on
		bot.hornHeld = true;
	}
	if (std::fabs(speed) < 2) {
		return;
	}
	const VehicleBox box = vehicleBox(vehicle);
	const physics::Vec3 up {forward.y * vehicle.right.z - forward.z * vehicle.right.y,
		forward.z * vehicle.right.x - forward.x * vehicle.right.z, forward.x * vehicle.right.y - forward.y * vehicle.right.x};
	const physics::Vec3 centre = vehicle.position + vehicle.right * box.centre.x + up * box.centre.y + forward * box.centre.z;
	const int target = hornTarget(centre, forward * motion, box.centreRadius, self.team, self.id, m_players);
	const GameWorld::PlayerInfo* honked = nullptr;
	for (const GameWorld::PlayerInfo& other : m_players) {
		honked = other.id == target ? &other : honked;
	}
	if (!honked) {
		return;
	}
	const float interval = honked->ai ? 20.0f : 5.0f;
	if (m_time - bot.hornAt < interval) {
		return;
	}
	bot.hornAt = m_time;
	if (hasHorn) {
		input.fire = true;
		bot.hornHeld = true;
	} else {
		// No horn (ITHasCarHorn clear): "PLAYER_TACTICS_gogogo" to the
		// soldier in the way at once (AIDLL 0x100a5990 with the AI's channel 6).
		sendRadioNow(bot, self, 11, 6, honked->id);
	}
}

int hornTarget(const physics::Vec3& centre, const physics::Vec3& way, float ownRadius, int team, int self,
	const std::vector<GameWorld::PlayerInfo>& players)
{
	int honked = -1;
	for (const GameWorld::PlayerInfo& other : players) {
		if (!other.alive || other.vehicle || other.id == self || other.team != team) {
			continue;
		}
		const physics::Vec3 at = other.position + physics::Vec3 {0, kChestHeight, 0};
		physics::Vec3 d = at - centre;
		const float dist = std::sqrt(physics::lengthSquared(d));
		const float r = ownRadius + kSoldierRadius;
		if (dist > 1e-5f) {
			d = d * (1 / dist);
		}
		if (dist - r > 15) {
			continue;
		}
		const float f = physics::dot(d, way);
		const physics::Vec3 nearest = centre + way * (f * dist);
		if (physics::lengthSquared(nearest - at) > r * r || f <= 0) {
			continue;
		}
		honked = other.id; // the last one found
	}
	return honked;
}

int BotManager::areaOfControlPoint(int controlPoint) const
{
	const auto& areas = m_settings.strategy.areas;
	for (size_t i = 0; i < areas.size(); ++i) {
		if (areas[i].controlPoint == controlPoint) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

// An area's control point: where, how big, who holds it.
bool BotManager::areaPosition(int area, physics::Vec3& at, float& radius, int& team) const
{
	const auto& areas = m_settings.strategy.areas;
	if (area < 0 || static_cast<size_t>(area) >= areas.size()) {
		return false;
	}
	for (const GameWorld::ControlPointInfo& point : m_world.controlPointList()) {
		if (point.id == areas[static_cast<size_t>(area)].controlPoint) {
			at = point.position;
			radius = point.radius;
			team = point.team;
			return true;
		}
	}
	return false;
}

// The areas on the way from one area to another over their neighbours
// (the routes 0x10061d30 builds outward from the target): the cost of a
// step is the 2-D distance, x1.1 into a neutral area and x5 into one the
// enemy holds: the neighbour's per-side status (+0x70, set by 0x1005f210:
// 0 its side's control point, 1 the enemy's, 2 neutral, or its side's
// with the flag below 0.9; the rewrite's control points carry no flag
// level, so a point being taken counts as held). Without
// `from`, ending with `to`; empty when there is no way.
std::vector<int> BotManager::areaRoute(int from, int to, int team) const
{
	const auto& areas = m_settings.strategy.areas;
	const size_t n = areas.size();
	std::vector<float> cost(n, 1e30f);
	std::vector<int> previous(n, -1);
	std::vector<bool> done(n, false);
	if (from < 0 || to < 0 || static_cast<size_t>(from) >= n || static_cast<size_t>(to) >= n) {
		return {};
	}
	cost[static_cast<size_t>(from)] = 0;
	for (size_t round = 0; round < n; ++round) {
		int best = -1;
		for (size_t i = 0; i < n; ++i) {
			if (!done[i] && cost[i] < 1e29f && (best < 0 || cost[i] < cost[static_cast<size_t>(best)])) {
				best = static_cast<int>(i);
			}
		}
		if (best < 0 || best == to) {
			break;
		}
		done[static_cast<size_t>(best)] = true;
		physics::Vec3 at;
		float radius = 0;
		int owner = 0;
		if (!areaPosition(best, at, radius, owner)) {
			continue;
		}
		for (const std::string& name : areas[static_cast<size_t>(best)].neighbours) {
			for (size_t j = 0; j < n; ++j) {
				physics::Vec3 next;
				int holder = 0;
				if (areas[j].name != name || done[j] || !areaPosition(static_cast<int>(j), next, radius, holder)) {
					continue;
				}
				const float factor = holder == team ? 1.0f : holder == 0 ? 1.1f : 5.0f;
				const float c = cost[static_cast<size_t>(best)] + std::sqrt(horizontal2(next - at)) * factor;
				if (c < cost[j]) {
					cost[j] = c;
					previous[j] = best;
				}
			}
		}
	}
	if (previous[static_cast<size_t>(to)] < 0) {
		return {};
	}
	std::vector<int> route;
	for (int a = to; a != from && a >= 0; a = previous[static_cast<size_t>(a)]) {
		route.push_back(a);
	}
	std::reverse(route.begin(), route.end());
	return route;
}

// The goal list of an order (0x10060300): for each step of the area route
// from the area the bot is in (or last headed for) to the target, one of
// the level's waypoints between the two areas (aiStrategicArea.addWayPoint,
// either direction; for the map the bot moves on), three random tries: one
// ahead of the bot for the first step, closer to the step's area than the
// bot and on the mesh (0x100a8fd0, which the level's start, 0x10038d50,
// also runs once on every waypoint); then a point within 0.8 of each
// intermediate area's radius. A pair without waypoints ("Areas a & b do
// not have waypoints.", 0x10036b50) goes straight. The original keeps the
// first pick for the next order between the same areas (not kept here).
void BotManager::routeVia(Bot& bot, const GameWorld::PlayerInfo& self, int target, int map)
{
	bot.via.clear();
	const auto& s = m_settings.strategy;
	if (target < 0 || s.waypoints.empty()) {
		return;
	}
	int current = -1;
	for (size_t i = 0; i < s.areas.size() && current < 0; ++i) {
		physics::Vec3 at;
		float radius = 0;
		int owner = 0;
		if (areaPosition(static_cast<int>(i), at, radius, owner) && horizontal2(self.position - at) <= radius * radius) {
			current = static_cast<int>(i);
		}
	}
	if (current < 0) {
		current = bot.lastArea;
	}
	if (current < 0 || current == target) {
		return;
	}
	const ai::NavMesh* mesh = map == 1 ? m_vehicleNavMesh : m_navMesh;
	int previous = current;
	const std::vector<int> route = areaRoute(current, target, self.team);
	for (size_t k = 0; k < route.size(); ++k) {
		const int area = route[k];
		physics::Vec3 at;
		float radius = 0;
		int owner = 0;
		if (!areaPosition(area, at, radius, owner)) {
			break;
		}
		const std::string& a = s.areas[static_cast<size_t>(previous)].name;
		const std::string& b = s.areas[static_cast<size_t>(area)].name;
		std::vector<physics::Vec3> list;
		for (const ai::StrategicWaypoint& w : s.waypoints) {
			if (w.map == map && ((w.from == a && w.to == b) || (w.from == b && w.to == a))) {
				list.push_back({w.position[0], w.position[1], w.position[2]});
			}
		}
		for (int attempt = 0; attempt < 3 && !list.empty(); ++attempt) {
			const physics::Vec3 w = list[std::min(list.size() - 1, static_cast<size_t>(random() * static_cast<float>(list.size())))];
			physics::Vec3 ahead = at - self.position, toW = w - self.position;
			ahead.y = toW.y = 0;
			const bool forward = k > 0 || physics::dot(ahead, toW) > 0;
			const bool closer = horizontal2(w - at) < horizontal2(self.position - at);
			const bool onMesh = !mesh || mesh->findPolygon({w.x, w.y + 1.5f, w.z}, map == 1 ? 3.0f : 5.0f) >= 0;
			if (forward && closer && onMesh) {
				bot.via.push_back({w, -1});
				break;
			}
		}
		if (area != target) {
			physics::Vec3 point = at;
			for (int attempt = 0; attempt < 20; ++attempt) {
				const float angle = random() * 6.2831853f;
				const float distance = std::sqrt(random()) * radius * 0.8f;
				const physics::Vec3 p = at + physics::Vec3 {std::sin(angle) * distance, 0, std::cos(angle) * distance};
				if (!mesh || mesh->findPolygon({p.x, p.y, p.z}) >= 0) {
					point = p;
					break;
				}
			}
			bot.via.push_back({point, area});
		}
		previous = area;
	}
}

// The goal's next point (0x1008aaa0): a waypoint is passed within 10 m
// (max(bot radius, 10)); an area on the way within 60 m or once inside it.
physics::Vec3 BotManager::moveTarget(Bot& bot, const GameWorld::PlayerInfo& self)
{
	while (!bot.via.empty()) {
		const Bot::Via& v = bot.via.front();
		const float d2 = horizontal2(v.position - self.position);
		bool reached = v.area < 0 ? d2 < 10.0f * 10.0f : d2 < 60.0f * 60.0f;
		physics::Vec3 at;
		float radius = 0;
		int owner = 0;
		if (!reached && v.area >= 0 && areaPosition(v.area, at, radius, owner)) {
			reached = horizontal2(self.position - at) <= radius * radius;
		}
		if (!reached) {
			break;
		}
		bot.via.erase(bot.via.begin());
	}
	return bot.via.empty() ? bot.goal : bot.via.front().position;
}

// A level without StrategicAreas.ai (three of the 46 with a navigation
// mesh in this install: Liberation_of_Leipzig, Strike_at_Karkand, wushan)
// leaves the original's SAI without areas, so its squads get no orders;
// the rewrite deviates and makes an area per control point
// (createFromControlPoint <name> <id> 50), each the neighbour of all the
// others. Without AIDefaultStrategies.ai (it always runs in the game; the
// tests): its strategies.
void BotManager::ensureAreas()
{
	ai::StrategySettings& s = m_settings.strategy;
	if (s.strategies.empty()) {
		s.addDefaults();
	}
	if (!s.areas.empty()) {
		return;
	}
	const std::vector<GameWorld::ControlPointInfo> points = m_world.controlPointList();
	for (const GameWorld::ControlPointInfo& point : points) {
		ai::StrategicArea area;
		area.name = "cp" + std::to_string(point.id);
		area.controlPoint = point.id;
		area.temperature = 50;
		area.controlPointFlag = true;
		for (const GameWorld::ControlPointInfo& other : points) {
			if (other.id != point.id) {
				area.neighbours.push_back("cp" + std::to_string(other.id));
			}
		}
		s.areas.push_back(area);
	}
}

// The SAI of each side (src/AI/Strategy), once a second (the original
// time-slices it every frame behind a 35 s gate that always opens; the
// slicing's budget is CPU time, so a fixed cadence stands in). Squads of a team whose commander is a player keep
// that player's orders.
void BotManager::updateStrategy(float seconds)
{
	(void)seconds;
	ensureAreas();
	updateKnowledge();
	ai::StrategySettings& s = m_settings.strategy;
	const std::vector<GameWorld::PlayerInfo>& players = m_players;
	const std::vector<GameWorld::ControlPointInfo> points = m_world.controlPointList();
	for (int team = 1; team <= 2; ++team) {
		Side& side = m_sides[team];
		if (m_time < side.nextUpdate) {
			continue;
		}
		side.nextUpdate = m_time + 1;
		// Area statuses for the side.
		struct AreaView {
			const GameWorld::ControlPointInfo* point = nullptr;
			ai::AreaState state;
			physics::Vec3 order;
		};
		std::vector<AreaView> views(s.areas.size());
		const std::vector<GameWorld::VehicleInfo> vehicles = m_world.vehicleList();
		ai::SideStatus mine, theirs;
		mine.time = theirs.time = m_time;
		mine.sinceStrategy = m_time - side.startedAt;
		for (size_t i = 0; i < s.areas.size(); ++i) {
			AreaView& v = views[i];
			for (const GameWorld::ControlPointInfo& point : points) {
				if (point.id == s.areas[i].controlPoint) {
					v.point = &point;
				}
			}
			if (!v.point) {
				ai::StrategicArea& a = s.areas[i];
				if (a.box) {
					// 0x1005f210 for an area of its own: the units each side
					// has in the box (as the side knows them), then the rule.
					int friendly = 0, enemy = 0;
					for (const GameWorld::PlayerInfo& p : players) {
						physics::Vec3 at = p.position;
						if (!p.alive || p.team < 1 || p.team > 2 || !knownPosition(team, p.id, at) || certainty(team, p.id) <= 0
							|| at.x < a.boxMin[0] || at.x > a.boxMax[0] || at.z < a.boxMin[1] || at.z > a.boxMax[1]) {
							continue;
						}
						(p.team == team ? friendly : enemy) += 1;
					}
					a.boxStatus[static_cast<size_t>(team)] = ai::boxAreaStatus(a.boxStatus[static_cast<size_t>(team)], friendly, enemy,
						a.takeable[static_cast<size_t>(team)], a.takeable[static_cast<size_t>(3 - team)]);
					v.state.status = a.boxStatus[static_cast<size_t>(team)];
				}
				continue;
			}
			v.order = s.areas[i].hasOrderPosition[0]
				? physics::Vec3 {s.areas[i].orderPosition[0][0], s.areas[i].orderPosition[0][1], s.areas[i].orderPosition[0][2]}
				: v.point->position;
			float flag = 1;
			for (const ControlPoint& cp : m_world.controlPoints()) {
				if (cp.id == v.point->id && cp.flag == team) {
					flag = cp.flagHeight;
				}
			}
			if (v.point->team == team) {
				v.state.status = flag < 0.9f ? ai::AreaStatus::Neutral : ai::AreaStatus::Owned;
				v.state.contested = flag < 0.9f;
				mine.controlPoints += 1;
			} else if (v.point->team == 0) {
				v.state.status = ai::AreaStatus::Neutral;
			} else {
				v.state.status = ai::AreaStatus::Hostile;
				theirs.controlPoints += 1;
			}
			// Units in the area (0x1005f210, over the side's knowledge,
			// DAT_101394b0 slot 0x28): the temperature is the area's base
			// plus each unit's basicTemp x its certainty (certainty(): an
			// enemy soldier or vehicle the side's bots never saw or heard
			// isn't in it; vehicleCertainty), placed where the side has it
			// (knownPosition: the remembered spot once it is unsensed, slot
			// 0x2c 0x100a9b20), empty vehicles and every seat
			// included; the strengths count the manned units' battle
			// strengths x certainty (friendly +0xc0, enemy +0xc8), and the
			// required strength the enemies' strategic ones (round each,
			// index 1 in an owned area, 0 elsewhere).
			float temperature = s.areas[i].temperature;
			float enemyStrategic = 0;
			const bool owned = v.state.status == ai::AreaStatus::Owned;
			const float r = std::max(v.point->radius, s.areas[i].temperature);
			for (const GameWorld::PlayerInfo& p : players) {
				physics::Vec3 at = p.position;
				if (!p.alive || !knownPosition(team, p.id, at) || horizontal2(at - v.point->position) > r * r) {
					continue;
				}
				const float known = certainty(team, p.id);
				if (known <= 0) {
					continue;
				}
				const world::Object* soldier = m_world.objects().find(p.soldier);
				// A bot of the side on foot controls its soldier, whose
				// object the side holds at 4/3 of its basicTemp (botTempFactor).
				const float controlled = p.ai && p.team == team && !p.vehicle ? kBotTempFactor : 1.0f;
				temperature += unitAiOf(soldier ? soldier->objectTemplate() : nullptr).basicTemp * controlled * known;
				float battle = 0;
				std::array<float, 2> strategic {};
				unitStrengths(p, battle, strategic);
				if (p.team != team) {
					// The capability the side's object holds (+0x44, refreshed
					// under allowedTimeDiff, 0x100a9e70).
					if (const auto k = m_knowledge[team].find(p.id); k != m_knowledge[team].end()) {
						unitStrengths(p, battle, strategic, k->second.health);
					}
				}
				battle *= known;
				if (p.team == team) {
					v.state.friendlyStrength += battle;
				} else {
					v.state.enemyStrength += battle;
					enemyStrategic += std::round(strategic[owned ? 1 : 0]);
				}
			}
			for (const GameWorld::VehicleInfo& vehicle : vehicles) {
				physics::Vec3 at = vehicle.position;
				const int owner = vehicleTeam(vehicle.object);
				if (owner >= 1 && owner <= 2 && owner != team) {
					const auto k = m_knowledge[team].find(vehicleKey(vehicle.object));
					if (k != m_knowledge[team].end() && k->second.hasPosition && !k->second.sensed) {
						at = k->second.position;
					}
				}
				const float vehicleKnown = vehicleCertainty(team, vehicle.object);
				if (vehicleKnown <= 0 || horizontal2(at - v.point->position) > r * r) {
					continue;
				}
				const world::Object* o = m_world.objects().find(vehicle.object);
				if (!o || !o->objectTemplate()) {
					continue;
				}
				const UnitAi& u = unitAiOf(o->objectTemplate());
				if (!u.seatsKnown) {
					UnitAi& w = m_unitAi[o->objectTemplate()];
					for (const GameWorld::SeatLayout& seat : m_world.vehicleSeats(vehicle.object)) {
						const world::ObjectTemplate* st = m_world.templates().find(seat.name);
						if (st && st != o->objectTemplate()) {
							w.seatsTemp += unitAiOf(st).basicTemp;
						}
					}
					w.seatsKnown = true;
				}
				float aboard = 0; // the seats the side's bots control, at 4/3 (botTempFactor)
				for (const GameWorld::PlayerInfo& p : players) {
					if (!p.ai || !p.alive || p.team != team || p.vehicle != vehicle.object || p.seat < 0) {
						continue;
					}
					const std::vector<GameWorld::SeatLayout> seats = m_world.vehicleSeats(vehicle.object);
					const world::ObjectTemplate* st = static_cast<size_t>(p.seat) < seats.size()
						? m_world.templates().find(seats[static_cast<size_t>(p.seat)].name) : nullptr;
					aboard += (st && st != o->objectTemplate() ? unitAiOf(st).basicTemp : u.basicTemp) * (kBotTempFactor - 1);
				}
				temperature += (u.basicTemp + u.seatsTemp + aboard) * vehicleKnown;
			}
			v.state.temperature = temperature;
			side.temperature.resize(s.areas.size(), 0.0f);
			side.temperature[i] = temperature; // +0xa0 + 4 side (0x1005f210), the respawn's roulette reads it
			const float total = v.state.friendlyStrength + v.state.enemyStrength;
			v.state.required = ai::requiredStrength(enemyStrategic, owned, total > 0 ? v.state.friendlyStrength / total : 0);
		}
		for (size_t i = 0; i < s.areas.size(); ++i) {
			if (!views[i].point) {
				continue;
			}
			switch (views[i].state.status) {
			case ai::AreaStatus::Owned:
				mine.friendlyAreas += 1;
				break;
			case ai::AreaStatus::Neutral:
				mine.neutralAreas += 1;
				break;
			case ai::AreaStatus::Hostile:
				mine.hostileAreas += 1;
				break;
			}
			for (const std::string& name : s.areas[i].neighbours) {
				for (size_t j = 0; j < s.areas.size(); ++j) {
					if (s.areas[j].name == name && (views[j].point || s.areas[j].box)) {
						views[i].state.hostileNeighbour |= views[j].state.status == ai::AreaStatus::Hostile;
						views[i].state.neutralNeighbour |= views[j].state.status == ai::AreaStatus::Neutral;
						views[i].state.ownedNeighbour |= views[j].state.status == ai::AreaStatus::Owned;
					}
				}
			}
		}
		theirs.friendlyAreas = mine.hostileAreas;
		theirs.hostileAreas = mine.friendlyAreas;
		theirs.neutralAreas = mine.neutralAreas;

		// The strategy.
		const std::vector<std::string>& list = s.sideStrategies[team];
		std::vector<float> weights;
		for (const std::string& name : list) {
			const auto it = s.strategies.find(name);
			const float w = it == s.strategies.end()
				? 0.0f
				: ai::prerequisiteValue(s, it->second.prerequisite, mine, theirs) * it->second.prerequisiteWeight;
			weights.push_back(std::max(0.0f, w));
		}
		// A forced strategy (aiStrategy.forceStrategy) is taken at every
		// update; otherwise the choice is made again when the weights move,
		// or when the running one's time is up (its record's re-picks count
		// one more).
		const std::string& forced = s.forced[static_cast<size_t>(team)];
		bool spin = !list.empty() && (side.strategy.empty() || ai::strategyWeightsChanged(side.weights, weights));
		if (!forced.empty() && s.strategies.count(forced)) {
			spin = false;
			if (side.strategy != forced) {
				side.strategy = forced;
				side.startedAt = m_time;
				for (const auto& [id, area] : side.squadArea) {
					releaseSquad(team, id, area); // the areas let go (0x100893c0)
				}
				side.squadArea.clear();
			}
		} else if (!spin && !list.empty()) {
			const auto running = s.strategies.find(side.strategy);
			int& picks = side.picks[side.strategy];
			if (running != s.strategies.end() && ai::strategyTimeUp(m_time - side.startedAt, picks, running->second.timeLimit)) {
				++picks;
				spin = true;
			}
		}
		if (spin) {
			float total = 0;
			for (float w : weights) {
				total += w;
			}
			std::string pick = list.front();
			if (total > 0) {
				float spin = random() * total;
				for (size_t i = 0; i < list.size(); ++i) {
					if (weights[i] > 0 && (spin -= weights[i]) <= 0) {
						pick = list[i];
						break;
					}
				}
			}
			if (pick != side.strategy) {
				side.strategy = pick;
				side.startedAt = m_time;
				for (const auto& [id, area] : side.squadArea) {
					releaseSquad(team, id, area); // the areas let go (0x100893c0)
				}
				side.squadArea.clear();
			}
		}
		side.weights = weights;
		const auto strategyIt = s.strategies.find(side.strategy);
		if (strategyIt == s.strategies.end()) {
			continue;
		}
		const ai::Strategy& strategy = strategyIt->second;

		// Squads: where they are and how strong (2 round(members + 0.5)).
		// (AIDLL's squad: constructor 0x100c4080, destructor 0x100c38f0,
		// members joining 0x100c3f10 and leaving 0x100c3630 / 0x100c36d0 /
		// 0x100c1de0, the leader from the game's squad 0x100c34d0, its spot
		// the leader's 0x100c32b0, the strength 0x100c26d0, a member by id
		// 0x100c27d0; the side's lists: a player's squad 0x10089260 /
		// 0x10089340 / 0x100892b0, a squad by index 0x100893a0, its leader
		// refreshed 0x10089320; their manager: constructor 0x1008a720, the
		// sides' squads made up front, destructor 0x1008a240, update 0x1008a910:
		// one squad a frame round robin, here every squad each second.)
		struct Squad {
			physics::Vec3 at;
			int strength = 0;
			int leader = -1;
		};
		std::map<int, Squad> squads;
		for (const GameWorld::PlayerInfo& p : players) {
			if (p.team != team || !p.squad || !p.alive) {
				continue;
			}
			Squad& squad = squads[p.squad];
			if (squad.strength == 0 || p.squadLeader) {
				squad.at = p.position;
			}
			squad.strength += 1;
			if (p.squadLeader) {
				squad.leader = p.id;
			}
		}
		for (auto& [id, squad] : squads) {
			squad.strength = 2 * static_cast<int>(std::lround(squad.strength + 0.5f));
		}
		// Values.
		std::vector<std::pair<float, int>> attackCandidates, defenceCandidates;
		for (size_t i = 0; i < s.areas.size(); ++i) {
			AreaView& v = views[i];
			if (!v.point) {
				continue;
			}
			v.state.temperature = ai::modifiedTemperature(strategy, v.state.temperature, v.state.status,
				s.areas[i].controlPointFlag || s.areas[i].controlPoint >= 0, v.state.hostileNeighbour);
			// The nearest squad of the side (0x10089550: its leader's spot).
			float nearest = 0;
			for (const auto& [id, squad] : squads) {
				const float d = std::sqrt(physics::lengthSquared(squad.at - v.order));
				if (nearest == 0 || d < nearest) {
					nearest = d;
				}
			}
			v.state.squadDistance = nearest;
			const float attack = ai::attackValue(v.state);
			const float defence = ai::defenceValue(v.state);
			if (attack > 0.001f) {
				attackCandidates.push_back({attack, static_cast<int>(i)});
			}
			if (defence > 0.1f) {
				defenceCandidates.push_back({defence, static_cast<int>(i)});
			}
		}
		side.attacks = ai::selectAreas(attackCandidates, strategy.attacks, 0.8f);
		side.defences = ai::selectAreas(defenceCandidates, strategy.defences, 0.9f);
		mine.attacks = static_cast<float>(side.attacks.size());
		mine.defences = static_cast<float>(side.defences.size());

		// Assignment: squads keep areas still chosen; the area with the least
		// assigned / required gets the nearest free squad next; an area
		// releases a squad it can spare (assigned - its strength >= 1.1
		// required).
		const int boss = m_world.commander(team);
		GameWorld::PlayerInfo bossInfo;
		if (boss >= 0 && m_world.playerInfo(boss, bossInfo) && !bossInfo.ai) {
			continue; // a player commands: the squads are theirs
		}
		std::vector<int> chosen = side.attacks;
		chosen.insert(chosen.end(), side.defences.begin(), side.defences.end());
		std::map<int, int> assigned;
		for (auto it = side.squadArea.begin(); it != side.squadArea.end();) {
			const bool keep = squads.count(it->first) && std::find(chosen.begin(), chosen.end(), it->second) != chosen.end();
			if (keep) {
				assigned[it->second] += squads[it->first].strength;
				++it;
			} else {
				releaseSquad(team, it->first, it->second); // 0x100893c0
				it = side.squadArea.erase(it);
			}
		}
		for (int area : chosen) {
			const int required = std::max(1, views[static_cast<size_t>(area)].state.required);
			for (auto it = side.squadArea.begin(); it != side.squadArea.end();) {
				if (it->second == area && static_cast<float>(assigned[area] - squads[it->first].strength) / required >= 1.1f) {
					assigned[area] -= squads[it->first].strength;
					releaseSquad(team, it->first, it->second); // 0x100893c0
					it = side.squadArea.erase(it);
				} else {
					++it;
				}
			}
		}
		while (!chosen.empty()) {
			int neediest = -1;
			int lowest = 0;
			for (int area : chosen) {
				const int required = std::max(1, views[static_cast<size_t>(area)].state.required);
				const int ratio = assigned[area] / required;
				if (ratio >= 1) {
					continue;
				}
				if (neediest < 0 || ratio < lowest) {
					neediest = area;
					lowest = ratio;
				}
			}
			if (neediest < 0) {
				break;
			}
			int bestSquad = -1;
			float bestDistance = 0;
			for (const auto& [id, squad] : squads) {
				if (side.squadArea.count(id)) {
					continue;
				}
				const float d = physics::lengthSquared(squad.at - views[static_cast<size_t>(neediest)].order);
				if (bestSquad < 0 || d < bestDistance) {
					bestSquad = id;
					bestDistance = d;
				}
			}
			if (bestSquad < 0) {
				break;
			}
			side.squadArea[bestSquad] = neediest;
			assigned[neediest] += squads[bestSquad].strength;
		}
		// The orders: defend an owned area that is not contested, attack
		// otherwise. (AIDLL's squad takes a new order, 0x100c1eb0 then
		// 0x100c21b0 / 0x100c1fe0, its leader accepting it.)
		for (const auto& [id, area] : side.squadArea) {
			const AreaView& v = views[static_cast<size_t>(area)];
			const bool defend = v.point->team == team;
			const GameWorld::OrderType type = defend ? GameWorld::OrderType::Defend : GameWorld::OrderType::Attack;
			const GameWorld::SquadOrder* order = m_world.squadOrder(team, id);
			if (!order || order->type != type || horizontal2(order->position - v.order) > 1) {
				m_world.issueOrder(team, id, type, v.order.x, v.order.z, -1);
			}
			// The squad takes it at its next update (0x100c1eb0) and its bot
			// leader answers (0x100c21b0, BotsSquad.cpp).
		}
		// Steps 10 and 11 (0x100a0420, 0x100a09c0): the side's bots outside
		// the squads' assignments, each in turn, to attack or to defend by
		// the strategy's Aggression; the rounded attack and defence
		// strengths (the unit's strategic strengths, +0xc [0] and [1]) of
		// those sent so far keep the split. (In the shipped game every bot
		// is in a squad (0x10089000), so the bots of squads count as
		// assigned; the rewrite's goal for a bot outside squads takes its
		// role: an attack area or a defence, updateGoal.)
		float attackShare = 0, defenceShare = 0;
		ai::aggressionShares(strategy.aggression, mine.friendlyAreas, mine.neutralAreas, mine.hostileAreas, attackShare,
			defenceShare);
		float attacking = 0, defending = 0;
		side.lonerAttacks.clear();
		for (const GameWorld::PlayerInfo& p : players) {
			if (p.team != team || !p.ai || !p.alive || p.squad) {
				continue;
			}
			float battle = 0;
			std::array<float, 2> strategic {};
			unitStrengths(p, battle, strategic);
			const bool attack = ai::assignsAttack(attackShare, defenceShare, attacking, defending);
			(attack ? attacking : defending) += std::round(attack ? strategic[0] : strategic[1]);
			side.lonerAttacks[p.id] = attack;
		}
	}
}

// AIDLL's commander is the team's strategic AI, not a player: it never
// applies for command. For a team with no commander (the SAI's +0x241, set
// by 0x1009bcd0) it uses the team's tools itself, through the game's
// IBFCommander (DAT_10139498, BF2142.exe vtable 0x009d6618) on four clocks
// (+0x78 artillery, +0x80 orbital strike, +0x88 supplies, +0x90 UAV),
// which the SAI's constructor (0x100a39c0) starts at 120, 90, 60 and 35 s
// from the level's start and a commander leaving (0x1009bcd0) starts again
// from then (read back by 0x100a3980 / 0x100a3990 / 0x100a39a0 /
// 0x100a39b0).
//
// Its own strikes (slot 25, 0x1009c050): for a team with bots
// (IAIPlayerFactory slot 0x2c, 0x008973e0: the team's AI players), with an
// artillery asset standing (IBFCommander slots 0x34, 0x38 → 0x008970e0,
// 0x005a1050: one not a wreck), once its clock +0x78 and its look clock
// +0x98 (10 s, set as it looks) have passed: at a random spot of the first
// point the team holds whose flag is below 0.75 (being taken; the SAI's
// slot 0x68, 0x1009c4d0), nine times in ten, with no teammate within 40 m
// of it (0x1008a110 with the squad manager's strike radius +0xb8; the
// terrain's height there), it fires the artillery (IBFCommander slot 0x20,
// 0x00896f00: the commander's tool event with rcType 1 and no sender,
// which 0x0050ee10 lets through). Failing that, at an enemy human off the
// navigation mesh for over 60 s (0x100888d0 over 0x1008a3b0's list: one
// the bots can't reach). Either strike goes to the squad manager
// (0x10088040, BotsSquad.cpp's noteStrike) and restarts +0x78 at 120 s.
// (0x1008a110 counts the side's players whose AI info has flag 4 or not
// flag 2; the rewrite counts every one alive.) It gives no orders of its
// own (the SAI orders the squads, updateStrategy).
void BotManager::command(float seconds)
{
	(void)seconds;
	using A = GameWorld::CommanderAbility;
	const std::vector<GameWorld::PlayerInfo> players = m_world.playerList();
	for (int team = 1; team <= 2; ++team) {
		const bool human = m_world.commander(team) != -1;
		if (m_humanCommander[team] && !human) {
			// 0x1009bcd0: the four clocks from now.
			m_assetNext[team] = {m_time + 120, m_time + 90, m_time + 35, m_time + 60};
			// 0x1009bcd0 -> 0x10089440: every squad of the side drops its order (0x100c0f00).
			for (auto& [key, squad] : m_squads) {
				if (key.first == team) {
					squad.state = 0;
					squad.kind = 4;
				}
			}
		}
		m_humanCommander[team] = human;
	}
	answerRequests();
	const std::vector<GameWorld::ControlPointInfo> points = m_world.controlPointList();
	for (int team = 1; team <= 2; ++team) {
		const bool bots = std::any_of(players.begin(), players.end(), [&](const GameWorld::PlayerInfo& p) {
			return p.ai && p.team == team;
		});
		if (!bots || m_humanCommander[team] || !m_world.abilityState(team, A::Artillery).available
			|| !(m_assetNext[team][0] < m_time) || !(m_lookNext[team] < m_time)) {
			continue;
		}
		m_lookNext[team] = m_time + 10;
		bool fired = false;
		for (const ControlPoint& cp : m_world.controlPoints()) {
			if (cp.team != team || cp.flagHeight >= 0.75f || cp.flag != team) {
				continue;
			}
			const world::Vec3 p = cp.object->position();
			float radius = 10;
			for (const GameWorld::ControlPointInfo& info : points) {
				radius = info.id == cp.id ? info.radius : radius;
			}
			const float x = p.x + (random() * 2 - 1) * radius;
			const float z = p.z + (random() * 2 - 1) * radius;
			if (random() > 0.1f) {
				const float y = m_world.terrainHeight(x, z);
				const bool clear = std::none_of(players.begin(), players.end(), [&](const GameWorld::PlayerInfo& mate) {
					const physics::Vec3 d = mate.position - physics::Vec3 {x, y, z};
					return mate.alive && mate.team == team && d.x * d.x + d.y * d.y + d.z * d.z < 40.0f * 40.0f;
				});
				if (clear) {
					m_assetNext[team][0] = m_time + 120;
					m_world.commanderAction(team, A::Artillery, x, z, true);
					fired = true;
				}
			}
			break;
		}
		physics::Vec3 camper;
		if (!fired && offMeshEnemy(team, camper)) {
			m_assetNext[team][0] = m_time + 120;
			m_world.commanderAction(team, A::Artillery, camper.x, camper.z, true);
		}
	}
}

// The squad leaders' requests (SquadLeader.sendRequest; GameWorld's
// SquadRequest). BF2142.exe's squad manager stores one (slot 0x50,
// 0x004c5720) and, where the AI runs and the squad has a leader (+0x24),
// posts AI event 3/0xf with the request (0x004c3d60 / 0x004c4790: team
// +0x30, squad +0x34, the point +0x38..+0x40); the AI's event listener
// (0x00894860) hands it by kind to IAIEventCourier slots 24..27
// (0x1004e6e0 / 0x1004e720 / 0x1004e760 / 0x1004e7a0, vtable 0x101146d0),
// which call the team's SAI's slots 20..23 with the squad and the point.
// Only squad leaders request, and only humans have the HUD and console
// that send them: AIDLL has no caller of the store, so its bot leaders
// never ask (they ask a human commander for orders, req_new_order). Here
// a request new in the world's store (stored since the last look) is the
// event.
//
// With no human commander (+0x241) the SAI answers at once:
// - artillery (0x1009e020) and orbital strike (0x1009e200): with the asset
//   standing (IBFCommander 0x34 / 0x38, 0x3c / 0x40: 0x00897100,
//   0x00896c10) and its clock passed (+0x78, 120 s; +0x80, 90 s), the tool
//   at the request's point (slot 0x20 0x00896f00 / slot 0x24 0x00896f80,
//   which take the squad's request of kind 0 / 1 off, slot 0x24 of the
//   squad manager) and the strike to the squad manager (0x10088040);
//   the clock not passed, a turn-down of type 1, the asset gone one of
//   type 2;
// - the UAV (0x1009e3e0, +0x90, 35 s; slot 0x28 0x00897000, kind 2 off)
//   and supplies (0x1009e500, +0x88, 60 s; slot 0x2c 0x00897070, kind 3
//   off) need no asset test; the clock not passed, a turn-down of type 3
//   (UAV) or 0 (supplies).
// A turn-down (0x18 bytes: type, time, squad) waits 2 + 3U s (the word
// through 0x10046770; queued at +0xa0), then the SAI's update (slot 24,
// 0x1009ce20, first in 0x100a2dd0) says it: type 0 supply_deny (slot
// 0x48, 0x00896d10, kind 3 off), 1 artillery_deny (0x30, 0x00896b10,
// kind 0 off), 2 artillery_unavailable (0x44, 0x00896c30, kind 0 off), 3
// uav_deny (0x4c, 0x00896df0, kind 2 off). An orbital strike turned down
// thus says the artillery's lines and takes the squad's artillery request
// off, leaving its own standing (as the original). The lines are the
// commander's voice ("commander") over the team's radio on the machine
// running the AI (0x004cd1b0 / 0x004d7950, no listener test): here the
// local player hears them. A commander coming drops the queue (0x1009ce20
// with +0x241 set).
void BotManager::answerRequests()
{
	using A = GameWorld::CommanderAbility;
	for (int team = 1; team <= 2; ++team) {
		std::vector<GameWorld::SquadRequest> standing = m_world.squadRequests(team);
		for (const GameWorld::SquadRequest& r : standing) {
			float& seen = m_requestSeen.try_emplace({team, r.squad, r.kind}, -1.0f).first->second;
			if (seen == r.storedAt) {
				continue;
			}
			seen = r.storedAt;
			if (m_humanCommander[team]) {
				continue; // 0x1009e020..0x1009e500 return with +0x241 set
			}
			const auto turnDown = [&](int type) {
				// The time: (float)word x 2.3283064e-10 x 3 + now + 2.
				const float delay = ai::wordToFloat(m_settings.random.next()) * 2.3283064e-10f * 3.0f;
				m_turnDowns.push_back({team, r.squad, type, m_time + delay + 2});
			};
			std::array<float, 4>& next = m_assetNext[team];
			switch (r.kind) {
			case 0: // 0x1009e020
			case 1: { // 0x1009e200
				const A ability = r.kind == 0 ? A::Artillery : A::OrbitalStrike;
				if (!m_world.abilityState(team, ability).available) {
					turnDown(2);
				} else if (next[static_cast<size_t>(r.kind)] < m_time) {
					next[static_cast<size_t>(r.kind)] = m_time + (r.kind == 0 ? 120.0f : 90.0f);
					m_world.commanderAction(team, ability, r.position.x, r.position.z, true);
					m_world.removeSquadRequest(team, r.squad, r.kind);
				} else {
					turnDown(1);
				}
				break;
			}
			case 2: // 0x1009e3e0
				if (next[2] < m_time) {
					next[2] = m_time + 35;
					m_world.commanderAction(team, A::Uav, r.position.x, r.position.z, true);
					m_world.removeSquadRequest(team, r.squad, 2);
				} else {
					turnDown(3);
				}
				break;
			case 3: // 0x1009e500
				if (next[3] < m_time) {
					next[3] = m_time + 60;
					m_world.commanderAction(team, A::Supplies, r.position.x, r.position.z, true);
					m_world.removeSquadRequest(team, r.squad, 3);
				} else {
					turnDown(0);
				}
				break;
			default:
				break;
			}
		}
		for (auto it = m_requestSeen.begin(); it != m_requestSeen.end();) {
			const auto& [t, squad, kind] = it->first;
			const bool stands = t != team || std::any_of(standing.begin(), standing.end(), [&](const GameWorld::SquadRequest& r) {
				return r.squad == squad && r.kind == kind;
			});
			it = stands ? std::next(it) : m_requestSeen.erase(it);
		}
	}
	// 0x1009ce20: the turn-downs due.
	for (auto it = m_turnDowns.begin(); it != m_turnDowns.end();) {
		if (m_humanCommander[it->team]) {
			it = m_turnDowns.erase(it);
			continue;
		}
		if (m_time <= it->due) {
			++it;
			continue;
		}
		static const char* const kLines[] = {"supply_deny", "artillery_deny", "artillery_unavailable", "uav_deny"};
		static const int kKinds[] = {3, 0, 0, 2};
		const size_t type = static_cast<size_t>(it->type);
		if (const int local = m_world.localPlayer(); local >= 0) {
			m_world.hearRadio(local, 2, kLines[type]);
		}
		m_world.removeSquadRequest(it->team, it->squad, kKinds[type]);
		m_commanderSaid.push_back({it->team, it->squad, kLines[type]});
		it = m_turnDowns.erase(it);
	}
}

void registerBotConsoleMethods(Console& console, BotSettings& settings)
{
	const uint32_t access = ConsoleAccess::Anyone;
	BotSettings* s = &settings;
	bindMethod(console, "aiSettings.setBotSkill", access, [s](float skill) { s->skill = std::clamp(skill, 0.0f, 1.0f); });
	bindMethod(console, "aiSettings.setMaxNBots", access, [s](int count) { s->maxBots = count; });
	bindMethod(console, "aiSettings.setDiscoverCloakedEnemiesDistance", access,
		[s](float distance) { s->discoverCloakedDistance = distance; });
	bindMethod(console, "aiSettings.setViewDistance", access, [s](float distance) { s->viewDistance = distance; });
	bindMethod(console, "aiSettings.getViewDistance", access, [s]() { return s->viewDistance; });
	if (!console.hasMethod("GameLogic.maximumLevelViewDistance")) {
		bindProperty<float>(console, "GameLogic.maximumLevelViewDistance", access, [s]() { return s->levelViewDistance; },
			[s](float distance) { s->levelViewDistance = distance; });
	}
	bindMethod(console, "aiSettings.setBehaviour", access, [s](int id, std::string name) {
		if (id >= 0 && id < 256) {
			if (s->behaviours.size() <= static_cast<size_t>(id)) {
				s->behaviours.resize(static_cast<size_t>(id) + 1);
			}
			s->behaviours[static_cast<size_t>(id)] = name;
		}
	});
	bindMethod(console, "aiSettings.createBehaviourModifiers", access, [s](std::string name) {
		s->editing = name;
		s->modifiers[name];
	});
	bindMethod(console, "aiSettings.setBehaviourModifier", access, [s](std::string behaviour, float value) {
		if (!s->editing.empty()) {
			s->modifiers[s->editing][behaviour] = value;
		}
	});
	// The equipment types' behaviours (AIDLL's AIConsole halves:
	// setBehaviourModifier 0x10027940, setVehicleMods 0x10027fa0,
	// setVehicleBehaviour 0x10027df0 with its argument parse 0x1000e7e0 and
	// the named-behaviour lookup 0x10025e70, setVehicleDefaultBehaviour
	// 0x100271b0, createUCConstant / UCLinear / UCXInverse 0x10026020 /
	// 0x10026070 / 0x100260c0).
	bindMethod(console, "aiSettings.setVehicle", access, [s](std::string name) { s->vehicles[name]; });
	bindMethod(console, "aiSettings.setVehicleMods", access, [s](std::string vehicle, std::string mods) {
		s->vehicles[vehicle].modifiers = mods;
	});
	bindMethod(console, "aiSettings.setVehicleMaterialCost", access, [s](std::string vehicle, std::string material, float cost) {
		s->vehicles[vehicle].materialCost[material] = cost;
	});
	bindMethod(console, "aiSettings.setVehicleBehaviour", access,
		[s](std::string vehicle, std::string behaviour, std::string bb, std::string pattern, int priority, std::string curve,
			std::string inhibitors) {
			auto& set = s->vehicles[vehicle].behaviours;
			const auto it = std::find_if(set.begin(), set.end(), [&](const ai::BehaviourSetup& b) { return b.name == behaviour; });
			const ai::BehaviourSetup setup {behaviour, bb, pattern, priority, curve, inhibitors};
			if (it != set.end()) {
				*it = setup;
			} else {
				set.push_back(setup);
			}
		});
	bindMethod(console, "aiSettings.setVehicleDefaultBehaviour", access, [s](std::string vehicle, std::string behaviour) {
		s->vehicles[vehicle].defaultBehaviour = behaviour;
	});
	bindMethod(console, "aiSettings.addSensingAgentPattern", access, [s](std::string vehicle, std::string pattern) {
		s->vehicles[vehicle].sensingPattern = pattern;
	});
	bindMethod(console, "aiSettings.createUCConstant", access, [s](std::string name, float a) {
		s->curves[name] = {ai::UrgencyCurve::Kind::Constant, a, 0, 0, 0};
	});
	bindMethod(console, "aiSettings.createUCLinear", access, [s](std::string name, float a, float b) {
		s->curves[name] = {ai::UrgencyCurve::Kind::Linear, a, b, 0, 0};
	});
	bindMethod(console, "aiSettings.createUCXInverse", access, [s](std::string name, float a, float b, float c, float d) {
		s->curves[name] = {ai::UrgencyCurve::Kind::XInverse, a, b, c, d};
	});

	// weaponTemplate.* (Objects/**/ai/Weapons.ai).
	const auto weapon = [s]() -> WeaponAi* { return s->editingWeapon.empty() ? nullptr : &s->weapons[s->editingWeapon]; };
	bindMethod(console, "weaponTemplate.create", access, [s](std::string name) {
		s->editingWeapon = lower(name);
		s->weapons[s->editingWeapon] = WeaponAi();
	});
	bindMethod(console, "weaponTemplate.weaponType", access, [weapon](std::string type) {
		static const char* names[] = {"firearm", "healing", "ammo", "revive", "repair", "c4", "camouflage", "closecombat",
			"sniper", "fireandforget", "titanweapon"};
		if (WeaponAi* w = weapon()) {
			for (int i = 0; i < 11; ++i) {
				if (lower(type) == names[i]) {
					w->type = i;
				}
			}
		}
	});
	bindMethod(console, "weaponTemplate.minRange", access, [weapon](float v) {
		if (WeaponAi* w = weapon()) {
			w->minRange = v;
		}
	});
	bindMethod(console, "weaponTemplate.maxRange", access, [weapon](float v) {
		if (WeaponAi* w = weapon()) {
			w->maxRange = v;
		}
	});
	bindMethod(console, "weaponTemplate.optimalRangePercentage", access, [weapon](float v) {
		if (WeaponAi* w = weapon()) {
			w->optimal = v * 0.01f;
		}
	});
	bindMethod(console, "weaponTemplate.setFiringPose", access, [weapon](std::string pose) {
		if (WeaponAi* w = weapon()) {
			const std::string p = lower(pose);
			w->firingPose = p == "standing" ? 1 : p == "crouching" ? 2 : p == "lying" ? 3 : p == "swimming" ? 4 : 0;
		}
	});
	bindMethod(console, "weaponTemplate.setStrength", access, [weapon](std::string type, float v) {
		const int i = strengthIndex(type);
		if (WeaponAi* w = weapon(); w && i >= 0) {
			w->strength[static_cast<size_t>(i)] = v;
		}
	});
	bindMethod(console, "weaponTemplate.deviation", access, [weapon](float v) {
		if (WeaponAi* w = weapon()) {
			w->deviation = v;
		}
	});
	bindMethod(console, "weaponTemplate.allowedDeviation", access, [weapon](float v) {
		if (WeaponAi* w = weapon()) {
			w->allowedDeviation = v;
		}
	});
	bindMethod(console, "weaponTemplate.setExplosionRadius", access, [weapon](float v) {
		if (WeaponAi* w = weapon()) {
			w->explosionRadius = v;
		}
	});
	bindMethod(console, "weaponTemplate.indirect", access, [weapon](bool v) {
		if (WeaponAi* w = weapon()) {
			w->indirect = v;
		}
	});
	bindMethod(console, "weaponTemplate.isThrown", access, [weapon](bool v) {
		if (WeaponAi* w = weapon()) {
			w->thrown = v;
		}
	});
	// explodesOnImpact: the template's +0x4e (0x00887c2c).
	bindMethod(console, "weaponTemplate.explodesOnImpact", access, [weapon](bool v) {
		if (WeaponAi* w = weapon()) {
			w->explodesOnImpact = v;
		}
	});

	// aiStrategy.* and ai.addSAIStrategy. The strategy manager's halves:
	// createConstantCondition 0x1003a9f0, createPrerequisite 0x1003b540,
	// addCondition 0x1003a970 (0x1003a9b0 without a weight: 1; the
	// condition by name 0x1003a6b0), createStrategy 0x1003b470 (filed by
	// 0x1003b400), addSAIStrategy 0x10027260 (the strategy by name
	// 0x1003a380).
	ai::StrategySettings* st = &settings.strategy;
	// The argument parsers: Crisp/Fuzzy/FuzzySqr 0x1005a060, the compares
	// 0x1005a5e0, Friendly/Enemy/Both 0x1005a110, the types 0x1005a880;
	// setConditionStrength's names 0x10059f30.
	bindMethod(console, "aiStrategy.createConstantCondition", access,
		[st](std::string name, std::string conditional, std::string compare, std::string side, std::string type, float value) {
			using C = ai::StrategyCondition;
			C c;
			const std::string cond = lower(conditional);
			c.conditional = cond == "fuzzy" ? C::Conditional::Fuzzy : cond == "fuzzysqr" ? C::Conditional::FuzzySqr : C::Conditional::Crisp;
			static const char* compares[] = {"equal", "equalgreater", "equalsmaller", "quotient", "quotientgreater",
				"quotientsmaller", "difference", "differencegreater", "differencesmaller"};
			for (int i = 0; i < 9; ++i) {
				if (lower(compare) == compares[i]) {
					c.compare = static_cast<C::Compare>(i);
				}
			}
			const std::string sd = lower(side);
			c.side = sd == "enemy" ? 1 : sd == "both" ? 2 : 0;
			static const std::pair<const char*, int> types[] = {{"ticket", 13}, {"controlpoint", 14}, {"time", 15},
				{"starttime", 16}, {"attacks", 17}, {"defences", 18}, {"numberoffriendlyareas", 19},
				{"numberofneutralareas", 20}, {"numberofhostileareas", 21}};
			for (const auto& [n, id] : types) {
				if (lower(type) == n) {
					c.type = id;
				}
			}
			c.constant = value;
			st->conditions[name] = c;
			st->condition = name;
		});
	bindMethod(console, "aiStrategy.setConditionStrength", access, [st](std::string strength) {
		static const char* names[] = {"required", "requiredpositive", "requirednegative", "advisory", "advisorypositive",
			"advisorynegative"};
		const auto it = st->conditions.find(st->condition);
		for (int i = 0; i < 6 && it != st->conditions.end(); ++i) {
			if (lower(strength) == names[i]) {
				it->second.strength = static_cast<ai::StrategyCondition::Strength>(i);
			}
		}
	});
	bindMethod(console, "aiStrategy.createPrerequisite", access, [st](std::string name) {
		st->prerequisites[name].clear();
		st->prerequisite = name;
	});
	bindMethod(console, "aiStrategy.addCondition", access, [st](std::string condition, float weight) {
		if (!st->prerequisite.empty()) {
			st->prerequisites[st->prerequisite].push_back({condition, weight});
		}
	});
	bindMethod(console, "aiStrategy.createStrategy", access, [st](std::string name) {
		ai::Strategy strategy;
		strategy.name = name;
		st->strategies[name] = strategy;
		st->strategy = name;
	});
	const auto strategy = [st]() -> ai::Strategy* {
		const auto it = st->strategies.find(st->strategy);
		return it != st->strategies.end() ? &it->second : nullptr;
	};
	bindMethod(console, "aiStrategy.Aggression", access, [strategy](float v) {
		if (ai::Strategy* x = strategy()) {
			x->aggression = v;
		}
	});
	bindMethod(console, "aiStrategy.NumberOfAttacks", access, [strategy](int v) {
		if (ai::Strategy* x = strategy()) {
			x->attacks = v;
		}
	});
	bindMethod(console, "aiStrategy.NumberOfDefences", access, [strategy](int v) {
		if (ai::Strategy* x = strategy()) {
			x->defences = v;
		}
	});
	bindMethod(console, "aiStrategy.setPrerequisite", access, [strategy](std::string name) {
		if (ai::Strategy* x = strategy()) {
			x->prerequisite = name;
		}
	});
	bindMethod(
		console, "aiStrategy.setStrategicObjectsModifier", access,
		[strategy](std::string type, float value, std::string status) {
			static const std::pair<const char*, int> types[] = {{"controlpoint", ai::kTypeControlPoint},
				{"front", ai::kTypeFront}, {"safe", ai::kTypeSafe}, {"enemy", ai::kTypeEnemy}, {"neutral", ai::kTypeNeutral}};
			int t = -1;
			for (const auto& [n, id] : types) {
				t = lower(type) == n ? id : t;
			}
			ai::Strategy* x = strategy();
			if (!x || t < 0) {
				return;
			}
			const std::string st2 = lower(status); // Owned/Hostile/Neutral (0x1005bd50)
			const int which = st2 == "owned" ? 0 : st2 == "hostile" ? 1 : st2 == "neutral" ? 2 : -1;
			for (int i = 0; i < 3; ++i) {
				if (which < 0 || which == i) {
					x->modifiers[static_cast<size_t>(i)][static_cast<size_t>(t)] = value;
				}
			}
		},
		2);
	bindMethod(console, "ai.addSAIStrategy", access, [st](int side, std::string name) { st->sideStrategies[side].push_back(name); });

	// aiStrategicArea.* (the level's StrategicAreas.ai).
	bindMethod(console, "aiStrategicArea.createFromControlPoint", access, [st](std::string name, int controlPoint, float t) {
		ai::StrategicArea area;
		area.name = name;
		area.controlPoint = controlPoint;
		area.temperature = t;
		st->areas.push_back(area);
		st->area = name;
	});
	// The area manager's halves of these (DAT_101394ec): setActive
	// 0x10036310, addNeighbour 0x100360a0 (the original links only areas
	// on the same layer, +0x1d4), addObjectTypeFlag 0x10036240,
	// setOrderPosition 0x100363c0, addWayPoint 0x10039000 (kept per area
	// pair, lower index first, a list per map).
	bindMethod(console, "aiStrategicArea.setActive", access, [st](std::string name) { st->area = name; });
	bindMethod(console, "aiStrategicArea.layer", access, [st](int layer) {
		if (ai::StrategicArea* a = st->findArea(st->area)) {
			a->layer = layer;
		}
	});
	bindMethod(console, "aiStrategicArea.addNeighbour", access, [st](std::string name) {
		if (ai::StrategicArea* a = st->findArea(st->area)) {
			a->neighbours.push_back(name);
		}
	});
	bindMethod(console, "aiStrategicArea.addObjectTypeFlag", access, [st](std::string type) {
		if (ai::StrategicArea* a = st->findArea(st->area); a && lower(type) == "controlpoint") {
			a->controlPointFlag = true;
		}
	});
	bindMethod(console, "aiStrategicArea.setOrderPosition", access, [st](std::string map, ConsoleVec3 at) {
		ai::StrategicArea* a = st->findArea(st->area);
		const int m = lower(map) == "vehicle" ? 1 : 0;
		if (a) {
			a->hasOrderPosition[m] = true;
			a->orderPosition[m][0] = at.x;
			a->orderPosition[m][1] = at.y;
			a->orderPosition[m][2] = at.z;
		}
	});
	bindMethod(console, "aiStrategicArea.setSide", access, [st](int side) {
		if (ai::StrategicArea* a = st->findArea(st->area)) {
			a->side = side;
		}
	});
	bindMethod(console, "aiStrategicArea.vehicleSearchRadius", access, [st](float radius) {
		if (ai::StrategicArea* a = st->findArea(st->area)) {
			a->vehicleSearchRadius = radius;
		}
	});
	// aiTemplate.* and aiTemplatePlugIn.*: a template is a list of plug-ins
	// made before it; the object templates name theirs (ObjectTemplate.aiTemplate).
	bindMethod(console, "aiTemplatePlugIn.create", access, [s](std::string kind, std::string name) {
		s->editingPlugin = lower(name);
		AiPlugin plugin;
		plugin.kind = kind;
		s->aiPlugins[s->editingPlugin] = plugin;
	});
	bindMethod(console, "aiTemplatePlugIn.active", access, [s](std::string name) { s->editingPlugin = lower(name); });
	const auto plugin = [s]() -> AiPlugin* {
		const auto it = s->aiPlugins.find(s->editingPlugin);
		return it != s->aiPlugins.end() ? &it->second : nullptr;
	};
	bindMethod(console, "aiTemplatePlugIn.equipmentTypeName", access, [plugin](std::string name) {
		if (AiPlugin* p = plugin()) {
			p->equipmentType = name;
		}
	});
	bindMethod(console, "aiTemplatePlugIn.setStrType", access, [plugin](std::string type) {
		if (AiPlugin* p = plugin()) {
			p->strengthType = strengthIndex(type);
		}
	});
	bindMethod(console, "aiTemplatePlugIn.setPathfindingMap", access, [plugin](std::string map) {
		if (AiPlugin* p = plugin()) {
			p->pathfindingMap = map;
		}
	});
	bindMethod(console, "aiTemplatePlugIn.setHasExposedSoldier", access, [plugin](bool exposed) {
		if (AiPlugin* p = plugin()) {
			p->exposedSoldier = exposed;
		}
	});
	bindMethod(console, "aiTemplatePlugIn.setUseNoPathfindingToGetToObject", access, [plugin](bool on) {
		if (AiPlugin* p = plugin()) {
			p->noPathfindingToObject = on; // Unit +0x15 (0x00884eaf)
		}
	});
	bindProperty<int>(console, "aiTemplatePlugIn.vehicleNumber", access,
		[plugin] { return plugin() ? plugin()->vehicleNumber : -1; },
		[plugin](int number) {
			if (AiPlugin* p = plugin()) {
				p->vehicleNumber = number; // Mobile +4 (0x008834f9)
			}
		});
	// PIPositionSelectN: the key that moves a player to the seat (the Unit
	// plug-in's, 0x100aa5f0 reads it for BBPChange's seat change).
	bindMethod(console, "aiTemplatePlugIn.setSelectKey", access, [plugin](std::string key) {
		if (AiPlugin* p = plugin()) {
			const std::string prefix = "PIPositionSelect";
			p->selectKey = key.rfind(prefix, 0) == 0 ? std::atoi(key.c_str() + prefix.size()) : 0;
		}
	});
	bindMethod(console, "aiComm.addAIRadioMessage", access, [s](std::string voice, float modifier, float period, float life) {
		voice.erase(std::remove(voice.begin(), voice.end(), '"'), voice.end());
		const int type = radioMessageType(voice);
		if (type >= 0) {
			s->radioLife[type] = life;
			s->radioModifier[type] = modifier;
			s->radioSendPeriod[type] = period;
		}
	});
	bindMethod(console, "weaponTemplate.setFiresThroughTransparent", access, [weapon](bool v) {
		if (WeaponAi* w = weapon()) {
			w->firesThroughTransparent = v;
		}
	});
	bindMethod(console, "weaponTemplate.isEmpWeapon", access, [weapon](bool v) {
		if (WeaponAi* w = weapon()) {
			w->emp = v;
		}
	});
	bindMethod(console, "weaponTemplate.isAntiAirWeapon", access, [weapon](bool v) {
		if (WeaponAi* w = weapon()) {
			w->antiAir = v;
		}
	});
	bindMethod(console, "aiTemplatePlugIn.isFloating", access, [plugin](bool floating) {
		if (AiPlugin* p = plugin()) {
			p->floating = floating;
		}
	});
	const std::pair<const char*, float AiPlugin::*> numbers[] = {{"maxSpeed", &AiPlugin::maxSpeed},
		{"spaceNeededTweak", &AiPlugin::spaceNeededTweak}, {"speedAtZeroSpace", &AiPlugin::speedAtZeroSpace},
		{"backingSpeed", &AiPlugin::backingSpeed}, {"brakeDistance", &AiPlugin::brakeDistance},
		{"maxBrakePercentage", &AiPlugin::maxBrakePercentage}, {"lookAheadTime", &AiPlugin::lookAheadTime},
		{"minSpeedForLookAhead", &AiPlugin::minSpeedForLookAhead},
		{"minAngleDiffForSlowDown", &AiPlugin::minAngleDiffForSlowDown},
		{"angleSlowDownScale", &AiPlugin::angleSlowDownScale}, {"maxAngleDiffSlowDown", &AiPlugin::maxAngleDiffSlowDown},
		{"proportionalConstant", &AiPlugin::proportional}, {"derivativeConstant", &AiPlugin::derivative},
		{"angleSpeedConstant", &AiPlugin::angleSpeed}, {"yawScale", &AiPlugin::yawScale}};
	for (const auto& [name, member] : numbers) {
		bindMethod(console, std::string("aiTemplatePlugIn.") + name, access, [plugin, member = member](float value) {
			if (AiPlugin* p = plugin()) {
				p->*member = value;
			}
		});
	}
	// The ControlInfo inputs (BF2142.exe 0x008856e1 driveThrottleControl ..
	// 0x00886618 aimRollControl): the index, and its bit in the group's mask.
	struct InputSetter {
		const char* name;
		int AiPlugin::*index;
		uint64_t AiPlugin::*mask;
	};
	const InputSetter inputs[] = {{"driveThrottleControl", &AiPlugin::driveThrottle, &AiPlugin::driveMask},
		{"driveTurnControl", &AiPlugin::driveTurn, &AiPlugin::driveMask},
		{"drivePitchControl", &AiPlugin::drivePitch, &AiPlugin::driveMask},
		{"driveRollControl", &AiPlugin::driveRoll, &AiPlugin::driveMask},
		{"aimThrottleControl", &AiPlugin::aimThrottle, &AiPlugin::aimMask},
		{"aimVerticalControl", &AiPlugin::aimVertical, &AiPlugin::aimMask},
		{"aimHorizontalControl", &AiPlugin::aimHorizontal, &AiPlugin::aimMask},
		{"aimRollControl", &AiPlugin::aimRoll, &AiPlugin::aimMask},
		{"lookVerticalControl", &AiPlugin::lookVertical, &AiPlugin::lookMask},
		{"lookHorizontalControl", &AiPlugin::lookHorizontal, &AiPlugin::lookMask}};
	for (const InputSetter& in : inputs) {
		bindMethod(console, std::string("aiTemplatePlugIn.") + in.name, access, [plugin, in](std::string name) {
			if (AiPlugin* p = plugin()) {
				const int index = playerInputIndex(name);
				p->*in.index = index;
				if (index < 64) {
					p->*in.mask |= uint64_t {1} << index;
				}
			}
		});
	}
	// "x/y/z" degrees, each x pi / 180 clamped to +-pi (0x00891fa0, 0x00892070).
	const auto rotation = [](const std::string& text, std::array<float, 3>& out) {
		float v[3] = {0, 0, 0};
		std::sscanf(text.c_str(), "%f/%f/%f", &v[0], &v[1], &v[2]);
		for (int i = 0; i < 3; ++i) {
			out[static_cast<size_t>(i)] = std::clamp(v[i] * 0.017453292f, -3.1415927f, 3.1415927f);
		}
	};
	bindMethod(console, "aiTemplatePlugIn.setCameraRelativeMinRotationDeg", access, [plugin, rotation](std::string v) {
		if (AiPlugin* p = plugin()) {
			rotation(v, p->cameraMin);
		}
	});
	bindMethod(console, "aiTemplatePlugIn.setCameraRelativeMaxRotationDeg", access, [plugin, rotation](std::string v) {
		if (AiPlugin* p = plugin()) {
			rotation(v, p->cameraMax);
		}
	});
	// 0x00885d71: degrees x pi / 180 into +0x60.
	bindMethod(console, "aiTemplatePlugIn.setCameraRelativeDofRotationOffsetDeg", access, [plugin](float degrees) {
		if (AiPlugin* p = plugin()) {
			p->dofRotationOffset = degrees * 0.017453292f;
		}
	});
	bindMethod(console, "aiTemplatePlugIn.maxClimbAngle", access, [plugin](float v) {
		if (AiPlugin* p = plugin()) {
			p->maxClimbAngle = v;
		}
	});
	bindMethod(console, "aiTemplatePlugIn.maxRollAngle", access, [plugin](float v) {
		if (AiPlugin* p = plugin()) {
			p->maxRollAngle = v;
		}
	});
	bindMethod(console, "aiTemplate.create", access, [s](std::string name) {
		s->editingTemplate = lower(name);
		s->aiTemplates[s->editingTemplate] = AiTemplate();
	});
	bindMethod(console, "aiTemplate.active", access, [s](std::string name) { s->editingTemplate = lower(name); });
	bindMethod(console, "aiTemplate.addPlugIn", access, [s](std::string name) {
		const auto it = s->aiTemplates.find(s->editingTemplate);
		if (it != s->aiTemplates.end()) {
			it->second.plugins.push_back(lower(name));
		}
	});
	bindMethod(console, "aiTemplate.addType", access, [s](std::string type) {
		const auto it = s->aiTemplates.find(s->editingTemplate);
		if (it != s->aiTemplates.end()) {
			it->second.types |= informationTypeFlag(type);
		}
	});
	// AIDLL's pathfinding maps (0x10020000's console settings on the active
	// map, DAT_101394fc vt+0x48).
	const auto activeMap = [s]() -> BotSettings::PathfindingMap* {
		return s->activeMap >= 0 && static_cast<size_t>(s->activeMap) < s->pathfindingMaps.size()
			? &s->pathfindingMaps[static_cast<size_t>(s->activeMap)]
			: nullptr;
	};
	// createMap: DAT_101394fc vt+0xc (0x10044590), the map object
	// (vtable 0x10114378: constructor 0x100440d0, destructor 0x10044160 /
	// 0x10044710).
	bindMethod(console, "aiPathfinding.createMap", access, [s](std::string name) {
		s->pathfindingMaps.push_back({name});
		s->activeMap = static_cast<int>(s->pathfindingMaps.size()) - 1;
	});
	bindMethod(console, "aiPathfinding.setActiveMap", access, [s](std::string name) {
		for (size_t i = 0; i < s->pathfindingMaps.size(); ++i) {
			if (lower(s->pathfindingMaps[i].name) == lower(name)) {
				s->activeMap = static_cast<int>(i);
			}
		}
	});
	// The map.* command objects (constructor / argument parse, slot 0x64 /
	// execute on the active map, slot 0x5c): maxSlope 0x100203c0 /
	// 0x100204b0 / 0x10020510, allowedThresholdHeight 0x10020790 /
	// 0x10020880 / 0x100208e0, headClearance 0x100205a0 / 0x100206a0 /
	// 0x10020700; below, radius 0x10020970 / 0x10020a60 / 0x10020ac0,
	// allowedHeightDiff 0x10020b50 / 0x10020c40 / 0x10020ca0,
	// addVehicleForClusterCost 0x100210d0 / 0x10021210 / 0x100211c0.
	bindMethod(console, "aiPathfinding.map.maxSlope", access, [activeMap](float degrees) {
		if (BotSettings::PathfindingMap* m = activeMap()) {
			m->maxSlope = degrees;
		}
	});
	bindMethod(console, "aiPathfinding.map.allowedThresholdHeight", access, [activeMap](float height) {
		if (BotSettings::PathfindingMap* m = activeMap()) {
			m->allowedThresholdHeight = height;
		}
	});
	bindMethod(console, "aiPathfinding.map.headClearance", access, [activeMap](float height) {
		if (BotSettings::PathfindingMap* m = activeMap()) {
			m->headClearance = height;
		}
	});
	// The mesh generator's settings (the editor traces the maps with them;
	// the bots read the generated .qtr/.clb): accepted, not used.
	bindMethod(console, "aiPathfinding.map.radius", access, [](float) {});
	bindMethod(console, "aiPathfinding.map.allowedHeightDiff", access, [](float) {});
	bindMethod(console, "aiPathfinding.map.addVehicleForClusterCost", access, [](std::string) {});
	bindMethod(console, "aiPathfinding.traceMap", access, [](std::string) {});
	bindMethod(console, "aiPathfinding.maxWaterDepth", access, [](float) {});
	bindMethod(console, "aiTemplate.basicTemp", access, [s](float value) {
		const auto it = s->aiTemplates.find(s->editingTemplate);
		if (it != s->aiTemplates.end()) {
			it->second.basicTemp = value;
		}
	});
	bindMethod(console, "aiTemplatePlugIn.setBattleStrength", access, [plugin](std::string type, float value) {
		const int i = std::isdigit(static_cast<unsigned char>(type.empty() ? 'x' : type[0])) ? std::atoi(type.c_str()) : strengthIndex(type);
		if (AiPlugin* p = plugin(); p && i >= 0 && i < 8) {
			p->battleStrength[static_cast<size_t>(i)] = value;
		}
	});
	bindMethod(console, "aiTemplatePlugIn.setStrategicStrength", access, [plugin](int side, int value) {
		if (AiPlugin* p = plugin(); p && side >= 0 && side < 2) {
			p->strategicStrength[static_cast<size_t>(side)] = value;
		}
	});
	bindMethod(console, "aiTemplatePlugIn.setIsAntiAircraft", access, [plugin](int value) {
		if (AiPlugin* p = plugin()) {
			p->antiAircraft = value;
		}
	});
	// Hearing (AIDLL 0x100ca280, 0x100ea820) and the memory pass's cull
	// distance (0x100ee430).
	bindMethod(console, "aiTemplatePlugIn.setSoundSphereRadius", access, [plugin](float min, float max) {
		if (AiPlugin* p = plugin()) {
			p->soundSphereMin = min; // 0x00891840: +0x38
			p->soundSphereMax = max; // +0x34
		}
	});
	bindMethod(console, "aiTemplatePlugIn.setHearingProbability", access, [plugin](float nearer, float farther) {
		if (AiPlugin* p = plugin()) {
			p->hearingNear = nearer; // 0x00891860: +0x40
			p->hearingFar = farther; // +0x3c
		}
	});
	bindMethod(console, "aiTemplatePlugIn.cullDistance", access, [plugin](float d) {
		if (AiPlugin* p = plugin()) {
			p->cullDistance = d; // 0x00884818: +0x18 (and d * d at +0x1c)
		}
	});
	bindMethod(console, "weaponTemplate.soundSphereRadius", access, [weapon](float d) {
		if (WeaponAi* w = weapon()) {
			w->soundSphere = d; // 0x00887256: +0x24
		}
	});
	bindMethod(console, "kitTemplate.create", access, [s](std::string name, std::string type) {
		s->editingKit = lower(name);
		BotSettings::KitTemplate& k = s->kitTemplates[s->editingKit];
		k = {};
		k.kitType = kitTypeIndex(type);
	});
	bindMethod(console, "kitTemplate.active", access, [s](std::string name) { s->editingKit = lower(name); });
	bindMethod(console, "kitTemplate.setStrategicStrength", access, [s](int side, int value) {
		const auto it = s->kitTemplates.find(s->editingKit);
		if (it != s->kitTemplates.end() && side >= 0 && side < 2) {
			it->second.strategic[static_cast<size_t>(side)] = value;
		}
	});
	bindMethod(console, "kitTemplate.setBattleStrength", access, [s](std::string type, float value) {
		const auto it = s->kitTemplates.find(s->editingKit);
		const int i = std::isdigit(static_cast<unsigned char>(type.empty() ? 'x' : type[0])) ? std::atoi(type.c_str()) : strengthIndex(type);
		if (it != s->kitTemplates.end() && i >= 0 && i < 8) {
			it->second.battle[static_cast<size_t>(i)] = value;
		}
	});
	bindMethod(console, "aiTemplate.degeneration", access, [s](float value) {
		if (const auto it = s->aiTemplates.find(s->editingTemplate); it != s->aiTemplates.end()) {
			it->second.degeneration = value;
		}
	});
	bindMethod(console, "aiTemplate.allowedTimeDiff", access, [s](float value) {
		if (const auto it = s->aiTemplates.find(s->editingTemplate); it != s->aiTemplates.end()) {
			it->second.allowedTimeDiff = value;
		}
	});
	bindMethod(console, "aiTemplate.commonKnowledge", access, [s](bool value) {
		if (const auto it = s->aiTemplates.find(s->editingTemplate); it != s->aiTemplates.end()) {
			it->second.commonKnowledge = value;
		}
	});
	bindMethod(console, "aiTemplate.secondary", access, [s](bool value) {
		const auto it = s->aiTemplates.find(s->editingTemplate);
		if (it != s->aiTemplates.end()) {
			it->second.secondary = value;
		}
	});

	bindMethod(console, "aiStrategicArea.addWayPoint", access, [st](ConsoleVec3 at, std::string map, std::string from, std::string to) {
		st->waypoints.push_back({{at.x, at.y, at.z}, lower(map) == "vehicle" ? 1 : 0, from, to});
	});
	registerBotScriptMethods(console, settings);
}

} // namespace dice::hfe::game
