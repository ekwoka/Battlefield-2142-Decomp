#include "Bots.h"

#include "Console/ConsoleBinding.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dice::hfe::game {

namespace {

constexpr float kDegrees = 180.0f / 3.14159265f;
constexpr float kRadians = 3.14159265f / 180.0f;
constexpr float kEyeHeight = 1.6f;    // above the feet (the soldier fires from here)
constexpr float kChestHeight = 1.3f;  // where bots aim
constexpr float kSoldierRadius = 0.5f; // the AI object's radius (obj+0xdc; provisional)
constexpr float kSoldierSpeed = 5.0f;  // SoldierMobile's maxSpeed (Objects/Soldiers/Common/ai/Objects.ai)
// The aim controller's output is a mouse-look input; how far the engine
// turns a soldier for one unit is not traced: 90 degrees a second here
// (provisional).
constexpr float kInputDegreesPerSecond = 90.0f;
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

float horizontal2(const physics::Vec3& d) { return d.x * d.x + d.z * d.z; }

std::string lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

ai::Vec3 toAi(const physics::Vec3& v) { return {v.x, v.y, v.z}; }
physics::Vec3 fromAi(const ai::Vec3& v) { return {v.x, v.y, v.z}; }

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

// CombatStrengthType names (weaponTemplate.setStrength).
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

// The weaponTemplate the scripts gave, or what the shipped Weapons.ai files
// say of the kinds the bots handle specially (provisional stand-ins when
// Objects/**/ai/*.ai were not run).
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

BotManager::BotManager(GameWorld& world, BotSettings settings) : m_world(world), m_settings(std::move(settings)) {}

BotManager::~BotManager()
{
	if (!std::getenv("BF2142_BOT_STATS")) {
		return;
	}
	std::fprintf(stderr, "bot behaviours (seconds run, all bots):\n");
	for (const auto& [name, seconds] : m_behaviourSeconds) {
		std::fprintf(stderr, "  %-28s %8.1f\n", name.c_str(), seconds);
	}
	for (const auto& [vehicle, metres] : m_driven) {
		const world::Object* o = m_world.objects().find(vehicle);
		std::fprintf(stderr, "  driven %-20s %6.0f m\n", o && o->objectTemplate() ? o->objectTemplate()->name().c_str() : "?", metres);
	}
}

float BotManager::random()
{
	m_random ^= m_random << 13;
	m_random ^= m_random >> 17;
	m_random ^= m_random << 5;
	return static_cast<float>(m_random & 0xffffff) / static_cast<float>(0x1000000);
}

std::string BotManager::behaviour(int player) const
{
	const auto it = m_bots.find(player);
	return it != m_bots.end() ? it->second.behaviour : std::string();
}

int BotManager::target(int player) const
{
	const auto it = m_bots.find(player);
	return it != m_bots.end() ? it->second.target : -1;
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
	return settingNumber(m_world.templates().find(weapon), "velocity", 0);
}

// The equipment type (aiSettings.setVehicle) whose behaviours a bot runs:
// Infantery on foot; in a vehicle its seat's (the PlayerControlObject's
// aiTemplate, whose Unit plug-in names it: Car, Tank, FloatingTank, Fixed,
// ...). Without the AI templates a driver is a Tank when its seat has a
// gun and a Car otherwise, and other seats are Fixed (provisional).
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
	if (self.seat == 0) {
		return seat.weapon.empty() ? "Car" : "Tank";
	}
	return "Fixed";
}

// What a player counts as for weapon strengths (CombatStrengthType): its
// vehicle's Physical plug-in (setStrType), Infantry on foot; LightArmour
// for a vehicle without an AI template (provisional).
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
// original's time slicing (its CPU budgets are not modelled: every slice
// runs to its count):
//   - plan execution (0x100c91c0): about half the bots a frame, round
//     robin (N/2 + 1 and N/2 on alternate frames for an odd N). A bot
//     skipped while moving keeps its movement for up to 16 frames
//     (0x10098c00); the trigger is kept too (provisional: which input bits
//     the original clears isn't decoded), the turn is not.
//   - thinking (0x100c95f0): bots that executed gather time; one with a
//     plan thinks again after 0.5 s of it, one without after 0.3 s
//     (setPlannedDecisionMakingThreshold, setUnplannedDecisionMakingThreshold).
//     At most 2 planned and 2 + (2 - planned waiting) unplanned bots think
//     a frame, the longest waiting first (setDecisionMakingInterleave 2 2).
//   - sensing (0x100c9980): the two bots that executed this frame and
//     waited longest look about (0x100ed700).
// Rounds flying past are noticed every frame (0x1004ed30 is an event).
void BotManager::update(float seconds)
{
	m_time += seconds;
	// What every bot looks at this tick, gathered once.
	m_players = m_world.playerList();
	m_vehicles = m_world.vehicleList();
	if (!enabled()) {
		return;
	}
	updateStrategy(seconds);
	command(seconds);
	const std::vector<GameWorld::PlayerInfo>& players = m_players;
	std::vector<const GameWorld::PlayerInfo*> bots;
	for (const GameWorld::PlayerInfo& self : players) {
		if (!self.ai) {
			continue;
		}
		Bot& bot = m_bots[self.id];
		if (!self.alive) {
			bot = Bot();
			bot.spawnedAt = -1;
			// Spawn on the squad leader when there is a living one on foot.
			bool leaderUp = false;
			for (const GameWorld::PlayerInfo& other : players) {
				leaderUp = leaderUp || (other.squadLeader && other.alive && !other.vehicle && other.team == self.team
										   && other.squad == self.squad && other.id != self.id);
			}
			m_world.setSpawnChoice(self.id, self.squad != 0 && leaderUp ? GameWorld::kSpawnOnLeader : -1);
			continue;
		}
		if (bot.spawnedAt < 0) {
			bot.spawnedAt = m_time;
		}
		noticeRounds(bot, self);
		bots.push_back(&self);
	}
	if (bots.empty()) {
		return;
	}
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
			// Skipped this frame (0x10098c00).
			++bot.skips;
			SoldierInput input = bot.lastInput;
			input.turn = 0;
			input.use = false;
			input.selectItem = 0;
			input.switchSeat = 0;
			if (bot.skips >= 16 || horizontal2(self.velocity) < 0.01f) {
				input.throttle = input.strafe = 0;
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
		if (bot.lastVehicle && !self->vehicle) {
			bot.leftVehicleAt = m_time; // Change avoids it for 7 s (0x100d3e70)
		}
		updateGoal(bot, *self);
		SoldierInput input;
		act(bot, *self, input, elapsed);
		m_behaviourSeconds[bot.behaviour + (self->vehicle ? (self->seat == 0 ? " (driving)" : " (seated)") : "")] += elapsed;
		if (self->vehicle && self->seat == 0) {
			m_driven[self->vehicle] += std::sqrt(horizontal2(self->velocity)) * elapsed;
		}
		bot.lastVehicle = self->vehicle;
		bot.usedLast = input.use;
		bot.firedLast = input.fire;
		bot.lastInput = input;
		m_world.setInput(self->id, input);
	}
	// Thinking.
	std::vector<std::pair<int, int>> planned, unplanned; // (age, player)
	for (const GameWorld::PlayerInfo* self : executed) {
		Bot& bot = m_bots[self->id];
		const bool hasPlan = bot.arbitration.running >= 0 && !bot.arbitration.replan;
		if (hasPlan && bot.sinceThink > 0.5f) {
			planned.push_back({bot.thinkAge, self->id});
		} else if (!hasPlan && bot.sinceThink > 0.3f) {
			unplanned.push_back({bot.thinkAge, self->id});
		}
	}
	const auto oldestFirst = [](const std::pair<int, int>& a, const std::pair<int, int>& b) { return a.first > b.first; };
	std::stable_sort(planned.begin(), planned.end(), oldestFirst);
	std::stable_sort(unplanned.begin(), unplanned.end(), oldestFirst);
	std::vector<int> thinkers;
	const size_t unplannedLimit = static_cast<size_t>(std::max(0, 2 - static_cast<int>(planned.size())) + 2);
	for (size_t i = 0; i < unplanned.size() && i < unplannedLimit; ++i) {
		thinkers.push_back(unplanned[i].second);
	}
	for (size_t i = 0; i < planned.size() && i < 2; ++i) {
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

// Rounds passing a bot (0x1004ed30).
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
		}
	}

}

// Sensing (the soldier's sensing agent, AIDLL 0x100ed700 and 0x100ee430).
void BotManager::sense(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players)
{
	// The view: R = viewDistance, candidates within R cos(fov/2) (a square
	// frustum of half-angle fov/2), seen when one of
	// clamp(round(30 radius / d), 1, 10) rays is clear.
	float gunYaw = 0, gunPitch = 0;
	const bool gunning = self.vehicle && self.seat > 0 && m_world.gunAim(self.vehicle, self.seat, gunYaw, gunPitch);
	const float lookYaw = gunning ? self.yaw + gunYaw : self.yaw;
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
		if (visible) {
			const int rays = std::clamp(static_cast<int>(std::lround(30 * kSoldierRadius / std::max(distance, 0.01f))), 1, 10);
			bool clear = false;
			for (int ray = 0; ray < rays && !clear; ++ray) {
				physics::Vec3 point = chest;
				if (ray > 0) {
					point = other.position
						+ physics::Vec3 {(random() * 2 - 1) * 0.3f, 0.2f + random() * 1.5f, (random() * 2 - 1) * 0.3f};
				}
				clear = m_world.lineOfSight(eye, point, self.vehicle);
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
			it->lostAt = -1;
			it->everSeen = true;
			// Sightings go to the others in the same vehicle (0x100a8ce0).
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
						} else {
							m->position = other.position;
							m->seen = m_time;
							m->lostAt = -1;
							m->everSeen = true;
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
}

// The goal a bot walks to (MoveTo's waypoint): its squad's order, else
// for a bot outside squads the area the SAI attacks or defends nearest to
// it, else the nearest control point the team does not hold.
// For an area the team does not hold the goal is a spot within 0.8 of
// the control point's radius (the bot stays where it is when already
// inside); otherwise a spot round the order position
// (order + U * W * 0.8 - 0.5 W on each axis, as the original's
// asymmetric 0x10063460; W = the control point's radius here, provisional).
void BotManager::updateGoal(Bot& bot, const GameWorld::PlayerInfo& self)
{
	ensureAreas();
	const std::vector<GameWorld::ControlPointInfo> points = m_world.controlPointList();
	const auto& areas = m_settings.strategy.areas;
	physics::Vec3 orderAt;
	bool hasOrder = false;
	// A driver heads for the areas' vehicle order positions over the
	// vehicle mesh.
	const int map = self.vehicle && self.seat == 0 ? 1 : 0;
	if (const GameWorld::SquadOrder* order = self.squad ? m_world.squadOrder(self.team, self.squad) : nullptr) {
		orderAt = order->position;
		hasOrder = true;
	} else if (self.team == 1 || self.team == 2) {
		const Side& side = m_sides[self.team];
		float best = 0;
		for (const std::vector<int>* list : {&side.attacks, &side.defences}) {
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
		if (areas[i].hasOrderPosition[0]) {
			const physics::Vec3 at {areas[i].orderPosition[0][0], areas[i].orderPosition[0][1], areas[i].orderPosition[0][2]};
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
	if (!changed && !stale) {
		return;
	}
	bot.orderPosition = orderAt;
	bot.goalMap = map;
	bot.via.clear();
	if (areaIndex >= 0 && bot.goalOrder != areaIndex) {
		bot.lastArea = bot.goalOrder;
	}
	bot.goalOrder = areaIndex;
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
		for (int attempt = 0; attempt < 20; ++attempt) {
			const float angle = random() * 6.2831853f;
			const float distance = std::sqrt(random()) * r;
			const physics::Vec3 at = point->position + physics::Vec3 {std::sin(angle) * distance, 0, std::cos(angle) * distance};
			if (valid(at)) {
				bot.goal = at;
				break;
			}
		}
		return;
	}
	const float w = point->radius;
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
	bot.pattern = running >= 0 ? setup.behaviours[static_cast<size_t>(running)].pattern : std::string();
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
		bot.pattern.clear();
	}
	const std::string& pattern = bot.pattern;
	if (pattern != "BBPMedicAssist" && pattern != "BBPAmmoAssist" && pattern != "BBPRepairVehicle") {
		m_tasks.erase(self.id);
	}
	const bool driving = self.vehicle && self.seat == 0;
	if (pattern.rfind("BBPFire", 0) == 0 || pattern == "BBPHeliFire3d") {
		fire(bot, self, input, seconds);
	} else if (pattern.rfind("BBPGotoWaypoint", 0) == 0) {
		if (bot.hasGoal) {
			const physics::Vec3 target = moveTarget(bot, self);
			const int id = 1000 + bot.goalOrder * 64 + static_cast<int>(bot.via.size());
			const bool moving = driving ? drive(bot, self, target, id, seconds, input) : steer(bot, self, target, id, seconds, input);
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
			const physics::Vec3 d = downed.position - self.position;
			if (horizontal2(d) > 1.4f * 1.4f) {
				steer(bot, self, downed.position, 100000 + downed.id, seconds, input);
			} else {
				input.selectItem = self.inHandSlot != defibrillator ? defibrillator : 0;
				const float turn = angleBetween(self.yaw, headingTo(self.position, downed.position));
				input.turn = std::clamp(turn, -360 * seconds, 360 * seconds);
				input.aimPitch = -60;
				input.fire = self.inHandSlot == defibrillator && std::fabs(turn) < 20 && !bot.firedLast;
			}
		}
	} else if (pattern == "BBPChange" || pattern == "BBPChangeLandingCraft") {
		bool working = false;
		physics::Vec3 at;
		for (const GameWorld::VehicleInfo& vehicle : m_vehicles) {
			if (vehicle.object == (self.vehicle ? self.vehicle : bot.vehicleTarget)) {
				working = vehicle.working;
				at = vehicle.position;
			}
		}
		if (self.vehicle) {
			input.use = !bot.usedLast; // out
		} else if (bot.vehicleTarget) {
			const physics::Vec3 d = at - self.position;
			if (horizontal2(d) > 2.5f * 2.5f) {
				steer(bot, self, at, 200000 + static_cast<int>(bot.vehicleTarget), seconds, input);
			} else {
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
		// Away from the enemies, the camouflage on (provisional plan: the
		// BBPFlee plan 0x10085d90 is not decoded).
		int camo = 0;
		for (const auto& [slot, name] : self.carried) {
			camo = weaponAi(name).type == WeaponAi::Camouflage && contains(name, "camo") ? slot : camo;
		}
		if (camo && self.inHandSlot != camo) {
			input.selectItem = camo;
		}
		input.fire = camo && self.inHandSlot == camo;
		steer(bot, self, self.position + bot.fleeDirection * 30.0f, 300000 + self.id, seconds, input);
		input.sprint = true;
	} else {
		// Idle: stand and look about (the soldier's sensing pattern turns the
		// head; sweeping 30 degrees a second is provisional).
		if (!self.vehicle) {
			const float sweep = std::sin(m_time * 0.5f + static_cast<float>(self.id)) * 30.0f * seconds;
			input.turn = sweep;
			input.aimPitch = bot.pitch = 0;
		}
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

float BotManager::urgency(Bot& bot, const ai::BehaviourSetup& behaviour, float weight, const GameWorld::PlayerInfo& self,
	const std::vector<GameWorld::PlayerInfo>& players)
{
	const std::string& p = behaviour.pattern;
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
	// events 0x1004e4e0) don't exist in the rewrite: 0. BBPCallForMedic,
	// the 3d and boat patterns: not in the shipped ground sets.
	return 0;
}

// BBFire's urgency (0x10075fb0) and the choice of target and weapon. For
// each known enemy and each carried weapon: the weapon's strength against
// the target's type (FireAndForget only when facing it within 0.9, C4
// within 4 m; Healing/Ammo/Repair/Camouflage none), x1.03 for the weapon
// in hand. The target scores
//   max(0, 1 - d / (1.2 maxRange)) x best strength x freshness x 5
// (freshness 1 in sight, 1 / (0.05 lost + 1.5) since lost), beyond
// maxRange only when the bot could close in within 5 s. The current
// target is kept unless another scores 1.2 times more. Any score above 0:
// urgency 50 (the weight only gates it).
float BotManager::fireUrgency(Bot& bot, const GameWorld::PlayerInfo& self, const std::vector<GameWorld::PlayerInfo>& players)
{
	const physics::Vec3 eye = self.position + physics::Vec3 {0, kEyeHeight, 0};
	float gunYaw = 0, gunPitch = 0;
	const bool gunning = self.vehicle && self.seat > 0 && m_world.gunAim(self.vehicle, self.seat, gunYaw, gunPitch);
	if (self.vehicle && !gunning && self.seat != 0) {
		bot.target = -1;
		return 0; // a seat without a gun
	}
	const float forward = gunning ? self.yaw + gunYaw : self.yaw;
	float bestScore = 0, currentScore = 0;
	int bestTarget = -1, bestSlot = 0;
	float bestPreferred = 0;
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
		WeaponAi chosen;
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
				score = facing && m_world.drone(self.id) == 0 ? w.strength[static_cast<size_t>(targetType)] : 0;
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
				chosen = w;
			}
		};
		if (gunning) {
			const auto seats = m_world.vehicleSeats(self.vehicle);
			if (static_cast<size_t>(self.seat) < seats.size()) {
				WeaponAi w = weaponAi(seats[static_cast<size_t>(self.seat)].weapon);
				best = std::max(w.strength[static_cast<size_t>(targetType)], 1.0f);
				maxRange = std::max(w.maxRange, 100.0f); // vehicle guns' templates are not loaded (provisional)
				chosen = w;
			}
		} else {
			for (const auto& [s, name] : carried(self)) {
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
			// The fighting distance (0x100795a0): minRange + (maxRange -
			// minRange) f, f = optimal x max(0.2, skill) x U[0.8, 1.2].
			float f = chosen.optimal;
			if (chosen.type != WeaponAi::CloseCombat) {
				f = std::max(0.2f, m_settings.skill) * f * (0.8f + random() * 0.4f);
			}
			const float dh = at.y - eye.y;
			float reach = chosen.maxRange;
			if (dh > 5 && chosen.type != WeaponAi::CloseCombat && !chosen.thrown) {
				reach += 1.25f * dh;
			}
			bestPreferred = chosen.minRange + (reach - chosen.minRange) * f + (dh > 0 && !chosen.thrown ? 0.25f * dh : 0.0f);
		}
	}
	if (bestScore <= 0) {
		bot.target = -1;
		return 0;
	}
	if (bot.target >= 0 && currentScore > 0 && bestScore <= currentScore * 1.2f) {
		return 50; // keep the current target and weapon
	}
	if (bestTarget != bot.target) {
		bot.target = bestTarget;
		bot.targetAcquired = m_time; // Bot::setTarget (0x10097af0)
		// The reaction time (0x100777a0): (1 - skill)(1.5 + U[0,1]) s.
		const WeaponAi w = weaponAi(weaponName(self, bestSlot));
		bot.triggerAt = m_time + (w.type == WeaponAi::CloseCombat ? 0.0f : (1 - m_settings.skill) * (1.5f + random()));
		bot.preferredRange = bestPreferred;
	}
	bot.weaponSlot = bestSlot;
	return 50;
}

// BBMoveTo's urgency (0x10066a70): the goal's (ai::gotoUrgency) times the
// weight.
float BotManager::moveUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight)
{
	if (!bot.hasGoal) {
		return 0;
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
// with seats free waits for a teammate heading for them within 40 m (30,
// 70 when it faces the vehicle within 0.85) for up to 10 s (0x10069af0;
// teammates are the bots, whose choice is known).
float BotManager::idleUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight,
	const std::vector<GameWorld::PlayerInfo>& players)
{
	if (self.vehicle && self.seat != 0) {
		return 0.5f;
	}
	if (self.vehicle && self.seat == 0) {
		float wait = 0;
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
			wait = std::max(wait, facing >= 0.85f ? 70.0f : 30.0f);
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
// teammates within its maxRange score 1 - min(1, (d / speed) / time left);
// the best gets the bot (urgency 100).
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
		// The time left before the body is gone (+0x50): the respawn
		// countdown, 15 s without one (provisional).
		const float left = other.respawnIn > 0.001f ? other.respawnIn : 15.0f;
		const float score = 1 - std::min(1.0f, distance / kSoldierSpeed / left);
		if (score > best) {
			best = score;
			chosen = other.id;
		}
	}
	if (chosen < 0) {
		return 0;
	}
	bot.target = chosen;
	return 100;
}

// BBChange's urgency (0x1006c520): the best seat within reach against the
// one the bot holds. A seat's value (0x100d3e70) is not decoded in full:
// health x 2 for a free driver's seat, 1.5 for a free gun, 0.5 for
// another free seat, a soldier on foot 1 (provisional). The current seat
// counts x1.2 on foot and x2 in a vehicle; others x the distance factor
// max(0, min(0.5, (R^2 - 0.8 d^2) / R^2) + 0.5) within R/sqrt(2) (R = 50 m
// here, provisional), x5 within 20 m below a quarter of health, never one
// left less than 7 s ago. Urgency: best / current x weight x 10 (best x
// weight when the current seat is worth nothing, as in a wreck).
float BotManager::changeUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight)
{
	constexpr float kReach = 50;
	const std::vector<GameWorld::VehicleInfo>& vehicles = m_vehicles;
	float current = 1.2f;
	if (self.vehicle) {
		current = 0;
		for (const GameWorld::VehicleInfo& vehicle : vehicles) {
			if (vehicle.object == self.vehicle && vehicle.working) {
				current = healthOf(vehicle.object) * (self.seat == 0 ? 2.0f : 1.0f) * 2.0f;
			}
		}
		bot.vehicleTarget = 0;
		// A wreck or a vehicle shut down: out (10000, 0x1006c8f0).
		return current < 0.001f ? 10000 * weight : 0;
	}
	const float myHealth = healthOf(self.soldier);
	float best = 0;
	uint32_t chosen = 0;
	for (const GameWorld::VehicleInfo& vehicle : vehicles) {
		if (!vehicle.working || vehicle.freeSeats <= 0) {
			continue;
		}
		if (vehicle.object == bot.lastVehicle && m_time - bot.leftVehicleAt < 7) {
			continue;
		}
		const float d2 = horizontal2(vehicle.position - self.position);
		if (d2 > kReach * kReach * 0.5f) {
			continue;
		}
		float value = healthOf(vehicle.object) * (vehicle.driverFree ? 2.0f : 0.5f);
		if (!vehicle.driverFree) {
			const std::vector<GameWorld::SeatLayout> seats = m_world.vehicleSeats(vehicle.object);
			const std::vector<int> occupants = m_world.vehicleOccupants(vehicle.object);
			for (size_t s = 1; s < seats.size(); ++s) {
				if (!seats[s].weapon.empty() && s >= occupants.size()) {
					value = std::max(value, 1.5f * healthOf(vehicle.object));
				}
			}
		}
		float f = std::max(0.0f, std::min(0.5f, (kReach * kReach - 0.8f * d2) / (kReach * kReach)) + 0.5f);
		if (myHealth < 0.25f && d2 < 400) {
			f *= 5;
		}
		if (value * f > best) {
			best = value * f;
			chosen = vehicle.object;
		}
	}
	if (best <= current) {
		bot.vehicleTarget = 0;
		return 0;
	}
	bot.vehicleTarget = chosen;
	return best / current * weight * 10;
}

// BBFlee's urgency (0x100859d0): below 30 % health, the camouflage
// charged over 70 % (its ammo ratio; not readable here: carried counts as
// charged, provisional) and more than 10 s since spawning: 100, away from
// the known enemies (x3 for players, x2 for bots that target this one).
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

// An object's AI template's basicTemp (the AI object's +0x14, which the
// assist scores weigh targets by; provisional reading).
float BotManager::basicTemp(const std::string& objectTemplate, float fallback) const
{
	const auto it = m_settings.aiTemplates.find(lower(aiTemplateOf(objectTemplate)));
	return it != m_settings.aiTemplates.end() && it->second.basicTemp > 0 ? it->second.basicTemp : fallback;
}

// The squad coordination factor (0x10089aa0 -> 0x100c28e0) for a task
// (Medic 1, Repair 2, Ammo 4) on a target: 0 outside squads; 1 for the
// bot's own task; for a target outside squads 0 when a teammate has the
// task or the bot's squad (its leader here) is more than 141 m away, else
// 1; in the bot's squad 0 when a squadmate has it, else 1; in another
// squad 0 when the task is taken or that squad is 31.6 m or more from
// the bot's, else 0.5. Doubled for a human. The order-type checks (a
// squad ordered to heal, repair, resupply) are not modelled.
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
	if (other.squad == self.squad) {
		return takenInSquad ? 0.0f : human;
	}
	if (takenInTeam || physics::lengthSquared(squadAt - other.position) >= 1000) {
		return 0;
	}
	return 0.5f * human;
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
// coordination factor. The radio requests ("medic!", "ammo!") that raise
// it to 100 aren't modelled (bots don't call), nor the terrain-normal
// "upright" test (soldiers on foot always pass it here).
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
	bool chosenHuman = false;
	for (const GameWorld::PlayerInfo& o : m_players) {
		if (o.team != self.team || !o.alive || o.manDown || o.vehicle) {
			continue;
		}
		const bool itself = o.id == self.id;
		const bool human = !o.ai;
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
			if (!needs || least <= 0 || least >= threshold) {
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
		if (score > best) {
			best = score;
			chosen = o.id;
			chosenHuman = human;
		}
	}
	if (chosen < 0) {
		bot.assistTarget = -1;
		return 0;
	}
	bot.assistTarget = chosen;
	GameWorld::PlayerInfo target;
	m_world.playerInfo(chosen, target);
	const float scale = ammo ? 4.0f : chosenHuman ? 10.0f : 2.0f;
	return ai::fuzzy(best) * weight * scale * coordination(self, ammo ? 4 : 1, chosen, chosen, target.position);
}

// BBRepairVehicle (0x10073360): the vehicles within the repair tool's
// maxRange (50 m) between 0 and 60 % health (the threshold compares the
// held weapon with the ammo hub's slot, a slip of the original, so it
// stays 0.6), not the bot's own; score basicTemp / (f x distance), f =
// fuzzy(1 - health), 1 for the vehicle it repairs already. Urgency:
// fuzzy(best) x weight x 10 with a human aboard, x 2 otherwise, times the
// coordination factor (task 2).
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
		if (h <= 0 || h >= 0.6f) {
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
// (SAI 0x1009c280). With one placed (it lapses after 240 s): 100 to set
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
// pattern of the vehicles' sets never runs). Other soldiers are never
// avoided. For each vehicle within 50 m (not the one it is getting into
// or repairing): a wreck or one below 30 % health within max(r, min(10,
// r + 4)) adds 100; one moving at 2 m/s or more that will pass within
// 0.6 (r + own radius) x sqrt(1.5) inside the look-ahead time T, closing
// at 3 m/s or more, adds |closing speed| / distance. Urgency: the sum x
// weight. The grenade warning (1000 for 2.8 s after an explosion lands
// in front, 0x1004eab0) isn't modelled. Provisional: the vehicles'
// radius (2.5 m), T (2 s; x2 prone, x1.5 crouched) and the size factor
// (phys +0x48, 1 here).
float BotManager::avoidUrgency(Bot& bot, const GameWorld::PlayerInfo& self, float weight)
{
	if (self.vehicle) {
		return 0;
	}
	constexpr float kVehicleRadius = 2.5f;
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
		const float score = std::sqrt(vv) / std::max(distance, 0.1f);
		sum += score;
		if (score > best) {
			best = score;
			bot.avoidStatic = false;
			bot.avoidFrom = v.position;
			bot.avoidVelocity = horizontal2(v.velocity) > 0 ? v.velocity : v.forward;
			bot.avoidFor = hit;
		}
	}
	return sum * weight;
}

// BBPMedicAssist / BBPAmmoAssist (0x10070da0, 0x100724e0): to the target
// (MoveToObject, stopping at the two soldiers' radii), looking at it,
// standing; within half the hub's abilityRadius plus those radii the hub
// in hand. The bot itself: crouch and take the hub. The plan never ends
// on its own: the urgency drops it once the target is served. AIDLL
// presses no fire; the hubs here heal only once thrown (ThrownFireComp),
// so a bot holding the hub at its target throws it, at most every 20 s
// (provisional).
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
		if (self.inHandSlot == slot && m_time - bot.assistFiredAt > 20 && !bot.firedLast) {
			input.fire = true;
			bot.assistFiredAt = m_time;
		}
	}
	if (!itself && d <= radii) {
		const physics::Vec3 eye = self.position + physics::Vec3 {0, kEyeHeight, 0};
		aim(bot, self, target.position + physics::Vec3 {0, kChestHeight, 0}, {}, false, 0, input, seconds);
		(void)eye;
	}
}

// BBPRepairVehicle (0x10073da0): to the vehicle until within the two radii
// (vehicle 2.5 m, provisional), the repair tool in hand; facing it (dot
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
	const float radii = 2.5f + kSoldierRadius;
	const float d = std::sqrt(horizontal2(vehicle->position - self.position));
	if (d > radii) {
		steer(bot, self, vehicle->position, 600000 + static_cast<int>(vehicle->object), seconds, input, radii);
		return;
	}
	input.selectItem = self.inHandSlot != slot ? slot : 0;
	aim(bot, self, vehicle->position, {}, true, 0, input, seconds);
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
// the mine, the trigger held on the tool. The original's tool is the first
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
	aim(bot, self, bot.mineAt, {}, true, 0, input, seconds);
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

// BBPAvoidCollisionInfantery (0x10065360): away from a standing wreck
// (10 m), or aside from a vehicle's path (10 m across its motion, on the
// side the bot is on), checked by a ray over the mesh (else across it,
// either way); walked to until the time it allows has passed or within
// 2 m.
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
			goal = self.position + away * 10.0f;
			for (const physics::Vec3& d : {away, across, across * -1.0f}) {
				const physics::Vec3 g = self.position + d * 10.0f;
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

// The aim (MouseTurretAimAt 0x1008e940, controller 0x100e5820), in the
// "towards the target" sense: pitch u = bias + 7 e + 0.01 de + lead,
// clamped to 4 and halved (at least 0.01); yaw likewise without bias and
// halving, limited to 3 skill + 1 (4 sprinting; 1.5 with gain 3 when
// there is nothing to aim at). lead = 0.4 x the aim point's angular speed,
// halved when it turns the same way as the error. The bias pulls down when
// the target has been below on average over the last 10 errors
// (min(0.75, 20 x the average)).
void BotManager::aim(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& point,
	const physics::Vec3& pointVelocity, bool hasTarget, float speed, SoldierInput& input, float seconds)
{
	float gunYaw = 0, gunPitch = 0;
	const bool gunning = self.vehicle && self.seat > 0 && m_world.gunAim(self.vehicle, self.seat, gunYaw, gunPitch);
	const physics::Vec3 eye = self.position + physics::Vec3 {0, kEyeHeight, 0};
	// Lead: where the target will be when the round gets there.
	physics::Vec3 want = point;
	if (speed > 0) {
		const float flight = std::sqrt(horizontal2(point - eye)) / speed;
		want = point + pointVelocity * flight;
	}
	const physics::Vec3 d = want - eye;
	const float wantYaw = headingTo(eye, want);
	const float wantPitch = std::atan2(d.y, std::sqrt(horizontal2(d))) * kDegrees;
	const float trueYaw = (gunning ? self.yaw + gunYaw : self.yaw) - bot.devYaw;
	const float truePitch = gunning ? gunPitch : bot.pitch;
	const float ey = angleBetween(trueYaw, wantYaw) * kRadians;
	const float ep = (wantPitch - truePitch) * kRadians;
	float wy = 0, wp = 0;
	if (seconds > 0 && physics::lengthSquared(bot.lastAimPoint) > 0) {
		const physics::Vec3 last = bot.lastAimPoint - eye;
		wy = angleBetween(headingTo(eye, bot.lastAimPoint), wantYaw) * kRadians / seconds;
		wp = (wantPitch - std::atan2(last.y, std::sqrt(horizontal2(last))) * kDegrees) * kRadians / seconds;
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
	float lead = kAngleSpeed * wp;
	if ((ep > 0) == (wp > 0)) {
		lead *= 0.5f;
	}
	const float bias = average < -1e-4f ? -std::min(0.75f, -20 * average) : 0.0f;
	float up = bias - kProportional * ep - kDerivative * (ep - bot.pitchError) + lead;
	up = std::clamp(up, -4.0f, 4.0f) * 0.5f;
	if (up != 0 && std::fabs(up) < 0.01f) {
		up = up > 0 ? 0.01f : -0.01f;
	}
	// Yaw.
	const bool idleLook = !hasTarget && input.throttle == 0 && input.strafe == 0 && !input.sprint;
	const float gain = idleLook ? -3.0f : kProportional;
	const float limit = idleLook ? 1.5f : input.sprint ? 4.0f : 3 * m_settings.skill + 1;
	float yawLead = kAngleSpeed * wy;
	if ((ey > 0) == (wy > 0)) {
		yawLead *= 0.5f;
	}
	float right = -gain * ey - kDerivative * (ey - bot.yawError) + yawLead;
	right = std::clamp(right, -limit, limit);
	bot.pitchError = ep;
	bot.yawError = ey;
	float turn = right * kInputDegreesPerSecond * seconds;
	// Not past the target in one tick.
	const float errorDegrees = ey * kDegrees;
	if ((errorDegrees >= 0 && turn > errorDegrees) || (errorDegrees < 0 && turn < errorDegrees)) {
		turn = errorDegrees;
	}
	float pitchStep = up * kInputDegreesPerSecond * seconds;
	const float pitchErrorDegrees = ep * kDegrees;
	if ((pitchErrorDegrees >= 0 && pitchStep > pitchErrorDegrees) || (pitchErrorDegrees < 0 && pitchStep < pitchErrorDegrees)) {
		pitchStep = pitchErrorDegrees;
	}
	input.turn += turn;
	bot.pitch = std::clamp(truePitch + pitchStep, -89.0f, 89.0f);
	input.aimPitch = bot.pitch;
}

// BBPFireInfantery's plan (0x1007cf10, 0x100795a0): the chosen weapon in
// hand, the aim on the target (with its lead), the distance kept within
// [minRange, 1.1 preferred], the weapon's firing pose taken when standing
// still (at most once a second, no fire 0.8 s after), and the trigger
// (Trigger 0x1008f1e0 taps, TriggerContinously 0x1008ecd0 holds) while
// the aim lies within allowedDeviation metres at the target (0x100d9090):
// across, |d| L < max(0.1, r); up and down |dy| L < sqrt(0.2) r (0.02 for
// snipers); and within atan(0.02 max(r, 0.1)) of the line. Not while a
// teammate stands in the line of fire (radius 0.5 + 0.02 t; a second's
// hold when one crosses within 5 m). Each round strays by the AI
// deviation (1 - acc)(1 - t/2) x deviation, t = time on the target / 15 s,
// acc = skill against soldiers (1 - 0.5 (1 - skill) against vehicles,
// 1 - 0.4 (1 - skill) with explosives), spread uniformly over +-half of it
// on each axis (provisional).
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
	const physics::Vec3 targetAt = (inSight ? target.position : known->position)
		+ physics::Vec3 {0, target.vehicle ? 1.0f : kChestHeight, 0};
	const physics::Vec3 eye = self.position + physics::Vec3 {0, kEyeHeight, 0};
	float gunYaw = 0, gunPitch = 0;
	const bool gunning = self.vehicle && self.seat > 0 && m_world.gunAim(self.vehicle, self.seat, gunYaw, gunPitch);
	std::string weaponTemplate;
	if (gunning) {
		const auto seats = m_world.vehicleSeats(self.vehicle);
		weaponTemplate = static_cast<size_t>(self.seat) < seats.size() ? seats[static_cast<size_t>(self.seat)].weapon : "";
	} else if (self.vehicle) {
		return; // a driver's guns are not aimed by the bots yet
	} else {
		weaponTemplate = weaponName(self, bot.weaponSlot);
		if (self.inHandSlot != bot.weaponSlot) {
			input.selectItem = bot.weaponSlot;
			return;
		}
	}
	const WeaponAi weapon = weaponAi(weaponTemplate);
	const float distance = std::sqrt(physics::lengthSquared(targetAt - eye));

	// The distance kept (on foot).
	bool moving = false;
	if (!self.vehicle && weapon.type != WeaponAi::FireAndForget) {
		if (distance > 1.1f * std::max(bot.preferredRange, weapon.minRange + 1)) {
			// The aim turns the soldier and keeps its pitch (steering
			// levels the view, which would undo the aim every tick).
			const float pitch = bot.pitch;
			steer(bot, self, target.position, 400000 + target.id, seconds, input);
			bot.pitch = pitch;
			input.turn = 0;
			moving = true;
		} else if (distance < weapon.minRange) {
			input.throttle = -1;
			moving = true;
		}
	}
	aim(bot, self, targetAt, inSight ? target.velocity : physics::Vec3 {}, true, projectileSpeed(weaponTemplate), input,
		seconds);

	// The firing pose, standing still and not turning.
	SoldierPose want = SoldierPose::Stand;
	if (weapon.firingPose == 2) {
		want = SoldierPose::Crouch;
	} else if (weapon.firingPose == 3) {
		want = SoldierPose::Prone;
	}
	if (!self.vehicle) {
		if (moving || std::fabs(input.turn) > 1) {
			want = SoldierPose::Stand;
		}
		if (want != bot.pose && m_time - bot.poseRequested >= 1) {
			bot.pose = want;
			bot.poseRequested = m_time;
		}
		input.pose = bot.pose;
	}

	// The gate.
	const float yaw = ((gunning ? self.yaw + gunYaw : self.yaw) - bot.devYaw + input.turn) * kRadians;
	const float pitch = bot.pitch * kRadians;
	const physics::Vec3 cur {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
	const physics::Vec3 wantDir = (bot.lastAimPoint - eye) * (1 / std::max(std::sqrt(physics::lengthSquared(bot.lastAimPoint - eye)), 0.001f));
	const float r = weapon.allowedDeviation;
	const float across = (cur.x - wantDir.x) * (cur.x - wantDir.x) * distance * distance
		+ (cur.z - wantDir.z) * (cur.z - wantDir.z) * distance * distance;
	const float dy = (cur.y - wantDir.y) * distance;
	const float upDown = weapon.type == WeaponAi::Sniper ? 0.02f : 0.2f;
	bool onTarget = across < std::max(0.01f, r * r) && dy * dy < upDown * r * r;
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
			if (t < 5) {
				bot.triggerAt = std::max(bot.triggerAt, m_time + 1);
			}
		}
	}
	const bool poseSettled = m_time - bot.poseRequested >= 0.8f || self.vehicle;
	const bool ready = m_time >= bot.triggerAt && onTarget && !blocked && poseSettled && inSight;
	const world::ObjectTemplate* gun = m_world.templates().find(weaponTemplate);
	const bool automatic = settingNumber(gun, "fire.roundsPerMinute", 600) >= 200; // TriggerContinously (provisional test)
	if (weapon.type == WeaponAi::FireAndForget) {
		input.fire = ready && !bot.firedLast;
	} else if (automatic) {
		input.fire = ready;
	} else {
		input.fire = ready && !bot.tapped;
	}
	bot.tapped = input.fire;
	// The AI deviation on each round (BF2142.exe 0x00892b70).
	if (input.fire) {
		const float s = m_settings.skill;
		float acc = s;
		if (weapon.thrown) {
			acc = 1;
		} else if (weapon.explosionRadius > 0) {
			acc = 1 - 0.4f * (1 - s);
		} else if (target.vehicle) {
			acc = 1 - 0.5f * (1 - s);
		}
		const float onIt = std::min(1.0f, (m_time - bot.targetAcquired) / 15.0f);
		const float deviation = (1 - acc) * (1 - onIt / 2) * weapon.deviation;
		const float newYaw = (random() - 0.5f) * deviation;
		const float newPitch = (random() - 0.5f) * deviation;
		input.turn += newYaw - bot.devYaw;
		input.aimPitch = bot.pitch + newPitch;
		bot.devYaw = newYaw;
		bot.devPitch = newPitch;
	} else if (bot.devYaw != 0) {
		input.turn -= bot.devYaw;
		bot.devYaw = 0;
		bot.devPitch = 0;
	}
}

// Keeps the path follower on `to`: a new goal (another id), or the same one
// moved more than 2 m (at most once a second: MoveToObjectFinding's stop
// condition 0x100d0e60 ends the move when its object leaves the radius;
// the 2 m are provisional), plans again. After a failure (no path, or no
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
// Without a mesh or a path the bot walks straight at the goal. The jump
// the follower asks for is not in the rewrite's soldiers: a second's back
// step to one side stands in for it (provisional).
bool BotManager::steer(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& to, int goalId, float seconds,
	SoldierInput& input, float radius)
{
	bot.pitch = 0;
	input.aimPitch = 0;
	if (m_time < bot.unstickUntil) {
		input.throttle = -1;
		input.strafe = bot.unstickSide;
		return true;
	}
	if (radius > 0 && physics::lengthSquared(to - self.position) <= radius * radius) {
		return false;
	}
	physics::Vec3 point = to;
	if (m_navMesh) {
		ai::FollowerSettings settings;
		settings.soldier = true;
		settings.radius = kSoldierRadius;
		if (follow(bot, *m_navMesh, self.position, to, goalId, settings)) {
			const ai::PathFollower::Steer steer = bot.follower.steer(*m_navMesh, toAi(self.position), m_time);
			if (steer.arrived) {
				return false;
			}
			if (steer.jump) {
				bot.unstickUntil = m_time + 1;
				bot.unstickSide = random() < 0.5f ? -1.0f : 1.0f;
			}
			if (steer.ok) {
				point = fromAi(steer.point);
			}
		}
	}
	physics::Vec3 d = point - self.position;
	d.y = 0;
	const float length = std::sqrt(horizontal2(d));
	d = length > 0.001f ? d * (1 / length) : physics::Vec3 {std::sin(self.yaw * kRadians), 0, std::cos(self.yaw * kRadians)};
	input.throttle = 1;
	const physics::Vec3 eye = self.position + physics::Vec3 {0, kEyeHeight, 0};
	aim(bot, self, eye + d * 5.0f, {}, false, 0, input, seconds);
	bot.follower.progress(toAi(self.position), std::fabs(input.throttle), m_time);
	return true;
}

// The Mobile plug-in's driving settings for a vehicle (its root aiTemplate),
// the shipped FAV's without one; the tank controller for every type but
// Car and ArmedCar (provisional for Mech and FloatingTank, whose own
// interpreters MechMoveTo and FloatingTankMoveTo are not decoded).
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
	const std::string type = equipmentOf(self);
	d.tank = type != "Car" && type != "ArmedCar";
	return d;
}

// MoveTo at the wheel (BBPGotoWaypoint2d 0x10068980; CarMoveTo 0x10091050,
// TankMoveTo 0x10091b40): the follower on the vehicle mesh, the mesh's
// free space round the vehicle (rays ahead, 15 degrees to either side of
// ahead, behind and its diagonals, and to the sides, out to radius + 10 m;
// the diagonals' angle is provisional) and ai::driveControl. Without a
// path it brakes (0x100ea170).
bool BotManager::drive(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& to, int goalId, float seconds,
	SoldierInput& input, float radius)
{
	(void)seconds;
	const GameWorld::VehicleInfo* vehicle = nullptr;
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		vehicle = v.object == self.vehicle ? &v : vehicle;
	}
	if (!vehicle) {
		return true;
	}
	const ai::DriveSettings settings = driveSettings(self);
	const physics::Vec3 at = vehicle->position;
	physics::Vec3 forward = vehicle->forward;
	forward.y = 0;
	const float flat = std::sqrt(horizontal2(forward));
	forward = flat > 1e-4f ? forward * (1 / flat) : physics::Vec3 {0, 0, 1};
	const float heading = std::atan2(forward.x, forward.z);
	ai::DriveSense sense;
	sense.speed = physics::dot(vehicle->velocity, forward);
	if (bot.lastYawAt >= 0 && m_time > bot.lastYawAt) {
		sense.yawRate = std::remainder(heading - bot.lastYaw, 2 * 3.14159265f) / (m_time - bot.lastYawAt);
	}
	bot.lastYaw = heading;
	bot.lastYawAt = m_time;
	sense.tilt = std::max(std::fabs(physics::dot(vehicle->angularVelocity, vehicle->right)),
		std::fabs(physics::dot(vehicle->angularVelocity, vehicle->forward)));
	const auto brake = [&] {
		input.strafe = 0;
		input.throttle = std::fabs(sense.speed) <= 1 ? 0.0f : (sense.speed > 0 ? -1.0f : 1.0f);
	};
	if (radius > 0 && horizontal2(to - at) <= 0.25f * radius * radius) {
		brake(); // within half the goal's radius (0x10068980)
		return false;
	}
	physics::Vec3 point = to;
	sense.remaining = std::sqrt(horizontal2(to - at));
	if (m_vehicleNavMesh) {
		ai::FollowerSettings follower;
		follower.soldier = false;
		follower.radius = settings.radius;
		follower.options.vehicle = true;
		if (!follow(bot, *m_vehicleNavMesh, at, to, goalId, follower)) {
			brake();
			// No progress for 20 s (0x10092bc0): the original hands a stuck
			// bot to the game (0x100c91c0, game vt+0x24, not traced); here
			// the driver gets out (provisional).
			if (bot.follower.waypoints().size() > 0 && std::fabs(sense.speed) < 0.5f) {
				input.use = !bot.usedLast;
			}
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
		const float reach = settings.radius + 10;
		const bool onMesh = m_vehicleNavMesh->findPolygon(toAi(at)) >= 0;
		const auto freeAlong = [&](float degrees) {
			if (!onMesh) {
				return reach; // off the mesh (a spawn spot beside it): nothing known
			}
			const float a = heading + degrees * kRadians;
			const physics::Vec3 end = at + physics::Vec3 {std::sin(a), 0, std::cos(a)} * reach;
			float got = 0;
			return ai::meshRaycast(*m_vehicleNavMesh, toAi(at), toAi(end), nullptr, 30, &got) ? reach : got;
		};
		sense.front = std::min({freeAlong(0), freeAlong(-15), freeAlong(15)});
		sense.back = std::min({freeAlong(180), freeAlong(165), freeAlong(-165)});
		sense.left = freeAlong(-90);
		sense.right = freeAlong(90);
	}
	// Wedged: pushing at least 0.9 for 1.5 s and slower than 0.5 m/s.
	// The original's other obstacle checks (the dynamic objects' cap
	// 0x100fc660, the vehicle states 4-6, 13, 14) aren't decoded; here the
	// side it pushes against reads as no space for 3 s, so the state
	// machine backs off or turns as it does for the mesh's edges
	// (provisional).
	if (std::fabs(bot.lastInput.throttle) >= 0.9f && std::fabs(sense.speed) < 0.5f) {
		if (bot.wedgedSince < 0) {
			bot.wedgedSince = m_time;
		}
		if (m_time - bot.wedgedSince > 1.5f) {
			bot.blockedUntil = m_time + 3;
			bot.blockedAhead = bot.lastInput.throttle > 0;
			bot.wedgedSince = -1;
		}
	} else {
		bot.wedgedSince = -1;
	}
	if (m_time < bot.blockedUntil) {
		(bot.blockedAhead ? sense.front : sense.back) = 0;
	}
	sense.goalDistance = std::sqrt(horizontal2(point - at));
	sense.error = std::remainder(std::atan2(point.x - at.x, point.z - at.z) - heading, 2 * 3.14159265f);
	const ai::DriveOutput out = ai::driveControl(settings, bot.drive, sense);
	input.throttle = out.throttle;
	input.strafe = out.turn;
	bot.follower.progress(toAi(at), std::max(std::fabs(out.throttle), std::fabs(out.turn)), m_time);
	return true;
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
// enemy holds (which status gets which factor is provisional). Without
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
// bot and on the mesh (0x100a8fd0); then a point within 0.8 of each
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

// Without the level's StrategicAreas.ai: an area per control point
// (createFromControlPoint <name> <id> 50), each the neighbour of all the
// others; without AIDefaultStrategies.ai: its strategies (provisional).
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
// time-slices it every frame behind a 35 s gate that always opens;
// provisional cadence). Squads of a team whose commander is a player keep
// that player's orders.
void BotManager::updateStrategy(float seconds)
{
	(void)seconds;
	ensureAreas();
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
			// Units in the area: temperature (basicTemp: a soldier 1, a
			// vehicle 20) and strengths (the soldier's battle strengths sum
			// to 8; vehicles counted the same, provisional).
			float temperature = s.areas[i].temperature;
			float enemies = 0;
			const float r = std::max(v.point->radius, s.areas[i].temperature);
			for (const GameWorld::PlayerInfo& p : players) {
				if (!p.alive || horizontal2(p.position - v.point->position) > r * r) {
					continue;
				}
				temperature += (p.vehicle ? 20.0f : 1.0f) * healthOf(p.soldier);
				if (p.team == team) {
					v.state.friendlyStrength += 8;
				} else {
					v.state.enemyStrength += 8;
					enemies += 1;
				}
			}
			v.state.temperature = temperature;
			v.state.required = ai::requiredStrength(enemies, v.state.status == ai::AreaStatus::Owned);
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
					if (s.areas[j].name == name && views[j].point) {
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
		if (!list.empty() && (side.strategy.empty() || ai::strategyWeightsChanged(side.weights, weights))) {
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
				it = side.squadArea.erase(it);
			}
		}
		for (int area : chosen) {
			const int required = std::max(1, views[static_cast<size_t>(area)].state.required);
			for (auto it = side.squadArea.begin(); it != side.squadArea.end();) {
				if (it->second == area && static_cast<float>(assigned[area] - squads[it->first].strength) / required >= 1.1f) {
					assigned[area] -= squads[it->first].strength;
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
		// otherwise.
		for (const auto& [id, area] : side.squadArea) {
			const AreaView& v = views[static_cast<size_t>(area)];
			const bool defend = v.point->team == team;
			const GameWorld::OrderType type = defend ? GameWorld::OrderType::Defend : GameWorld::OrderType::Attack;
			const GameWorld::SquadOrder* order = m_world.squadOrder(team, id);
			if (!order || order->type != type || horizontal2(order->position - v.order) > 1) {
				m_world.issueOrder(team, id, type, v.order.x, v.order.z, -1);
			}
			const auto leader = squads[id].leader;
			GameWorld::PlayerInfo info;
			if (leader >= 0 && m_world.playerInfo(leader, info) && info.ai && m_world.squadOrder(team, id)
				&& !m_world.squadOrder(team, id)->accepted) {
				m_world.acceptOrder(leader, true);
			}
		}
	}
}

// A team without a commander gets one of its bots (when the game mode
// enables the commander). Every 10 s (AIDLL's commander check 0x1009c050):
// - a strike, no sooner than 120 s into the round nor 120 s after the last,
//   at a random spot of the first point the team holds whose flag is
//   below 0.75 (being taken), with no teammate within 15 m of it, nine
//   times in ten. Which tool the original fires (IBFCommander+0x20) is not
//   traced: the orbital strike, else the artillery;
// - the UAV, then the satellite scan, over the nearest control point the
//   team doesn't hold, and attack orders for squads the SAI left without
//   one (provisional: not in AIDLL's commander).
void BotManager::command(float seconds)
{
	m_commandIn -= seconds;
	if (m_commandIn > 0 || !m_world.commanderEnabled()) {
		return;
	}
	m_commandIn = 10;
	const std::vector<GameWorld::PlayerInfo> players = m_world.playerList();
	const std::vector<GameWorld::ControlPointInfo> points = m_world.controlPointList();
	for (int team = 1; team <= 2; ++team) {
		if (m_world.commander(team) == -1) {
			for (const GameWorld::PlayerInfo& p : players) {
				if (p.ai && p.team == team) {
					m_world.applyForCommander(p.id);
					break;
				}
			}
		}
		GameWorld::PlayerInfo boss;
		if (!m_world.playerInfo(m_world.commander(team), boss) || !boss.ai) {
			continue;
		}
		const auto attackPoint = [&](const physics::Vec3& from) -> const GameWorld::ControlPointInfo* {
			const GameWorld::ControlPointInfo* best = nullptr;
			for (const GameWorld::ControlPointInfo& point : points) {
				if (point.team != team
					&& (!best || physics::lengthSquared(point.position - from) < physics::lengthSquared(best->position - from))) {
					best = &point;
				}
			}
			return best;
		};
		using A = GameWorld::CommanderAbility;
		const A strike = m_world.abilityReady(team, A::OrbitalStrike) ? A::OrbitalStrike : A::Artillery;
		if (m_time >= m_firstStrikeAt && m_time - m_lastStrike[team] >= 120 && m_world.abilityReady(team, strike)) {
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
				const bool clear = std::none_of(players.begin(), players.end(), [&](const GameWorld::PlayerInfo& mate) {
					return mate.alive && mate.team == team
						&& (mate.position.x - x) * (mate.position.x - x) + (mate.position.z - z) * (mate.position.z - z) < 15.0f * 15.0f;
				});
				if (clear && random() < 0.9f) {
					m_world.commanderAction(team, strike, x, z);
					m_lastStrike[team] = m_time;
				}
				break;
			}
		}
		if (const GameWorld::ControlPointInfo* point = attackPoint(boss.position)) {
			if (m_world.abilityReady(team, A::Uav)) {
				m_world.commanderAction(team, A::Uav, point->position.x, point->position.z);
			} else if (m_world.abilityReady(team, A::Satellite)) {
				m_world.commanderAction(team, A::Satellite, point->position.x, point->position.z);
			}
		}
		for (const GameWorld::PlayerInfo& leader : players) {
			if (!leader.squadLeader || leader.team != team || m_world.squadOrder(team, leader.squad)) {
				continue;
			}
			if (const GameWorld::ControlPointInfo* point = attackPoint(leader.position)) {
				m_world.issueOrder(team, leader.squad, GameWorld::OrderType::Attack, point->position.x, point->position.z,
					boss.id);
				if (leader.ai) {
					m_world.acceptOrder(leader.id, true);
				}
			}
		}
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
	bindMethod(console, "aiSettings.setViewDistance", access, [s](float distance) {
		if (distance > 0) {
			s->viewDistance = distance;
		}
	});
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
	// The equipment types' behaviours.
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

	// aiStrategy.* and ai.addSAIStrategy.
	ai::StrategySettings* st = &settings.strategy;
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
			const std::string st2 = lower(status);
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
		const bool gain = std::string(name).find("Constant") != std::string::npos;
		bindMethod(console, std::string("aiTemplatePlugIn.") + name, access, [plugin, member = member, gain](float value) {
			if (AiPlugin* p = plugin()) {
				p->*member = value;
				p->hasGains = p->hasGains || gain;
			}
		});
	}
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
	bindMethod(console, "aiTemplate.basicTemp", access, [s](float value) {
		const auto it = s->aiTemplates.find(s->editingTemplate);
		if (it != s->aiTemplates.end()) {
			it->second.basicTemp = value;
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
}

} // namespace dice::hfe::game
