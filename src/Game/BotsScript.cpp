// The rest of AIDLL.dll's console (its 277 commands, registered by the
// static initialisers 0x10100000.. under the classes AutoControllerTemplate,
// AI, aiSettings, AIBotManager, aiStrategy, AIStrategicArea, AILandingZone,
// AIPathFinding, AIDebug and AIRandom; each command is an object with a
// constructor naming it and its argument types, a parser per argument and
// an execute slot, vtable slot 23): the settings the shipped scripts set
// that Bots.cpp's registerBotConsoleMethods doesn't, AIBotManager's
// scheduling, autoControllerTemplate.*, the strategy commands left, and
// the scripted-bot commands (ai.botMoveTo and the rest), which act on the
// running BotManager. The debug-only commands (ai.draw*, ai.pause*Bots,
// ai.showAll, ai.getRendCamera*, aiDebug.*, the navigation mesh
// generator's aiPathfinding.save/load/export*) aren't rewritten: see
// docs/engine.md, "AIDLL's console".
#include "Bots.h"

#include "Console/ConsoleBinding.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace dice::hfe::game {

namespace {

std::string lowerCase(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return text;
}

physics::Vec3 toPhysics(const ConsoleVec3& v) { return {v.x, v.y, v.z}; }

ai::Vec3 toAiVec(const ConsoleVec3& v) { return {v.x, v.y, v.z}; }

// The run-time setters change the console's settings and, once a manager
// runs, its copy.
template <class F>
void both(BotSettings* s, F change)
{
	change(*s);
	if (s->manager) {
		change(s->manager->liveSettings());
	}
}

// The first `count` (1..3) of the three points the patrol commands take
// (0x10028680, 0x10028790: count 1, 2 or 3 of them go in the list).
std::vector<ai::Vec3> patrolPoints(int count, const ConsoleVec3& a, const ConsoleVec3& b, const ConsoleVec3& c)
{
	std::vector<ai::Vec3> points;
	const ConsoleVec3* all[3] = {&a, &b, &c};
	for (int i = 0; i < std::min(count, 3); ++i) {
		points.push_back(toAiVec(*all[i]));
	}
	return points;
}

} // namespace

// --- The settings object's name list and auto controllers ---

void BotSettings::addBotName(const std::string& first, const std::string& last, float u)
{
	BotName entry;
	entry.key = botNames.size() < 6 ? u * 0.4f : u;
	entry.name = first + " " + last;
	const auto at = std::find_if(botNames.begin(), botNames.end(), [&](const BotName& n) { return !(n.key < entry.key); });
	botNames.insert(at, entry);
}

std::string BotSettings::nextBotName()
{
	if (botNames.empty()) {
		return {};
	}
	BotName head = botNames.front();
	botNames.erase(botNames.begin());
	botNames.push_back(head);
	return head.name;
}

const BotSettings::AutoControllerTemplate* BotSettings::autoController(const std::string& name) const
{
	for (const auto& [key, value] : autoControllers) {
		if (lowerCase(key) == lowerCase(name)) {
			return &value;
		}
	}
	return nullptr;
}

// --- The scripted-bot commands on the running manager ---

void BotManager::attach(BotSettings& source)
{
	m_source = &source;
	source.manager = this;
}

bool BotManager::scriptGoal(int player, ai::Goal goal)
{
	GameWorld::PlayerInfo info;
	if (!m_world.playerInfo(player, info) || !info.ai || m_deactivated.count(player)) {
		return false;
	}
	m_goals[player] = std::move(goal);
	return true;
}

bool BotManager::giveOrder(int player, const std::string& behaviour, const std::string& modifierSet)
{
	const auto bot = m_bots.find(player);
	const auto& names = m_settings.behaviours;
	if (bot == m_bots.end() || std::find(names.begin(), names.end(), behaviour) == names.end()
		|| !m_settings.modifiers.count(modifierSet)) {
		return false;
	}
	m_orders[player] = behaviour + " " + modifierSet;
	return false; // the bot's +0x50 not set: 0x10098b80 refuses it
}

std::string BotManager::lastOrder(int player) const
{
	const auto it = m_orders.find(player);
	return it != m_orders.end() ? it->second : std::string();
}

bool BotManager::scriptMoveTo(int player, float radius, const physics::Vec3& at)
{
	GameWorld::PlayerInfo info;
	if (!m_world.playerInfo(player, info)) {
		return false;
	}
	// The bot's object (vt+0xcc) ITAir: a fly-to with the radius as every
	// reach (0x1008b450); otherwise a waypoint goal (0x1008ae90).
	bool air = false;
	if (info.vehicle) {
		if (const world::Object* o = m_world.objects().find(info.vehicle); o && o->objectTemplate()) {
			air = (m_settings.typeFlags(aiTemplateOf(o->objectTemplate()->name())) & kITAir) != 0;
		}
	}
	const ai::Vec3 point {at.x, at.y, at.z};
	return scriptGoal(player, air ? ai::flyToGoal(radius, radius, point) : ai::moveToGoal(radius, point));
}

bool BotManager::scriptFlyTo(int player, float radius, float height, const physics::Vec3& at)
{
	GameWorld::PlayerInfo info;
	if (!m_world.playerInfo(player, info) || !info.vehicle) {
		return false;
	}
	const world::Object* o = m_world.objects().find(info.vehicle);
	if (!o || !o->objectTemplate() || !(m_settings.typeFlags(aiTemplateOf(o->objectTemplate()->name())) & kITAir)) {
		return false;
	}
	return scriptGoal(player, ai::flyToGoal(radius, height, {at.x, at.y, at.z}));
}

const ai::Goal* BotManager::scriptedGoal(int player) const
{
	const auto it = m_goals.find(player);
	return it != m_goals.end() ? &it->second : nullptr;
}

bool BotManager::deactivate(int player)
{
	const auto it = m_bots.find(player);
	if (it == m_bots.end() || m_deactivated.count(player)) {
		return false;
	}
	m_bots.erase(it);
	m_goals.erase(player);
	m_deactivated[player] = true;
	m_world.setInput(player, SoldierInput {});
	return true;
}

void BotManager::thinkSoon(int player)
{
	if (const auto it = m_bots.find(player); it != m_bots.end()) {
		it->second.sinceThink = 1e9f;
	}
}

int BotManager::addBot(const std::string& name, int team)
{
	if (team != 1 && team != 2) {
		return -1;
	}
	const std::string given = name.empty() && m_source ? m_source->nextBotName() : name;
	const int id = m_world.addPlayer(given.empty() ? "Bot" : given, team, true);
	if (id >= 0) {
		// AIDLL's 0x10089000 puts a new bot in its side's squad with the
		// fewest members (at most IAISettings slot 0x54 of them), the first
		// when all are full, moving a member on to keep the squads even; here
		// the first open squad.
		m_world.joinSquad(id, m_world.openSquad(team));
	}
	return id;
}

int BotManager::botCount() const
{
	int count = 0;
	for (const GameWorld::PlayerInfo& p : m_world.playerList()) {
		count += p.ai ? 1 : 0;
	}
	return count;
}

// --- The sides' knowledge (ai::knowledgeCertainty) ---

float BotManager::degenerationOf(const GameWorld::PlayerInfo& player) const
{
	const world::Object* soldier = m_world.objects().find(player.soldier);
	if (!soldier || !soldier->objectTemplate()) {
		return 0;
	}
	const auto it = m_settings.aiTemplates.find(lowerCase(aiTemplateOf(soldier->objectTemplate()->name())));
	return it != m_settings.aiTemplates.end() ? it->second.degeneration : 0.0f;
}

const GameWorld::PlayerInfo* BotManager::playerOf(int id) const
{
	const auto it = m_playerIndex.find(id);
	return it != m_playerIndex.end() && it->second < m_players.size() ? &m_players[it->second] : nullptr;
}

int BotManager::vehicleTeam(uint32_t vehicle) const
{
	if (const auto it = m_vehicleTeams.find(vehicle); it != m_vehicleTeams.end()) {
		return it->second; // those aboard
	}
	const world::Object* o = m_world.objects().find(vehicle);
	return o ? o->team() : 0;
}

// The side's AI object's update (vtbl 0x10118838 slot 3, 0x100a9e70):
// within allowedTimeDiff (the template's +0x10, the object's +0xc) of the
// last full update (+0x20) only the stamp (+0x48) moves. A full update
// copies the position while no sensor senses it (+0xa8 at 0), the team
// (+0xa0, adaptor slot 0x120) and the capability (+0x44, 0x100aa420), and
// stamps +0x20. Hearing forces one (0x100a9bb0 sets +0x20 to -1 first).
void BotManager::refreshKnowledge(Knowledge& k, float allowedTimeDiff, float health, const physics::Vec3& live, bool force)
{
	if (!force && k.updatedAt + allowedTimeDiff >= m_time) {
		return;
	}
	if (!k.sensed) {
		k.position = live;
		k.hasPosition = true;
	}
	k.health = health;
	k.updatedAt = m_time;
}

// What the sides know (the AI objects of BF2142.exe 0x008a2f00, one per
// side for each enemy unit): a unit is sensed while one of the side's bots
// has it in sight and for 2 s after it lost it (the memory pass 0x100ee430
// counts the sensor down, slot 6 0x100aa0f0, once the item has been out of
// sight over 2 s); the count reaching 0 copies the unit's position and
// matrix (+0x54, +0x60), where the side then places it (slot 0x2c,
// 0x100a9b20; the information grid, slot 0x34). Each sighting is an update
// (slot 3, from 0x100ed2c0 and 0x100ee430), throttled by allowedTimeDiff.
// Soldiers by their memory items; vehicles by the bots' sightings of them
// or of anyone aboard (a vehicle's seats' sightings are its own, 0x100a8ce0).
// A soldier that died is a new unit when it spawns again.
void BotManager::updateKnowledge()
{
	std::map<int64_t, bool> sensedNow[3];
	std::map<int64_t, float> seenAt[3];
	const auto note = [&](int team, int64_t key, float seen, bool sensed) {
		float& s = seenAt[team].try_emplace(key, -1e9f).first->second;
		s = std::max(s, seen);
		bool& now = sensedNow[team][key];
		now = now || sensed;
	};
	std::map<int, uint32_t> vehicleOf;
	for (const GameWorld::PlayerInfo& p : m_players) {
		if (p.alive && p.vehicle) {
			vehicleOf[p.id] = p.vehicle;
		}
	}
	for (const auto& [id, bot] : m_bots) {
		const GameWorld::PlayerInfo* me = playerOf(id);
		if (!me || me->team < 1 || me->team > 2 || !me->alive) {
			continue;
		}
		const GameWorld::PlayerInfo& self = *me;
		for (const Known& known : bot.known) {
			if (!known.everSeen) {
				continue;
			}
			const bool sensed = known.lostAt < 0 || m_time - known.lostAt <= 2;
			note(self.team, known.player, known.seen, sensed);
			if (const auto v = vehicleOf.find(known.player); v != vehicleOf.end()) {
				note(self.team, vehicleKey(v->second), known.seen, sensed);
			}
		}
		for (const auto& [vehicle, known] : bot.knownVehicles) {
			if (known.seen > -1e8f) {
				note(self.team, vehicleKey(vehicle), known.seen, known.lostAt < 0 || m_time - known.lostAt <= 2);
			}
		}
	}
	const auto liveOf = [this](int64_t key, physics::Vec3& at, float& health, std::string& ai) {
		if (key >= 0) {
			const GameWorld::PlayerInfo* found = playerOf(static_cast<int>(key));
			if (!found || !found->alive) {
				return false;
			}
			const GameWorld::PlayerInfo& p = *found;
			at = p.position;
			health = healthOf(p.soldier);
			const world::Object* soldier = m_world.objects().find(p.soldier);
			ai = soldier && soldier->objectTemplate() ? aiTemplateOf(soldier->objectTemplate()->name()) : std::string();
			return true;
		}
		const uint32_t vehicle = static_cast<uint32_t>(-(key + 1));
		for (const GameWorld::VehicleInfo& v : m_vehicles) {
			if (v.object == vehicle) {
				at = v.position;
				health = healthOf(vehicle);
				const world::Object* o = m_world.objects().find(vehicle);
				ai = o && o->objectTemplate() ? aiTemplateOf(o->objectTemplate()->name()) : std::string();
				return true;
			}
		}
		return false;
	};
	for (int team = 1; team <= 2; ++team) {
		for (const auto& [key, seen] : seenAt[team]) {
			m_knowledge[team].try_emplace(key);
			(void)seen;
		}
		for (auto it = m_knowledge[team].begin(); it != m_knowledge[team].end();) {
			physics::Vec3 live;
			float health = 1;
			std::string ai;
			if (!liveOf(it->first, live, health, ai)) {
				it = m_knowledge[team].erase(it);
				continue;
			}
			Knowledge& k = it->second;
			const auto s = seenAt[team].find(it->first);
			const bool sensed = s != seenAt[team].end() && sensedNow[team][it->first];
			if (s != seenAt[team].end() && s->second > k.lastSeen) {
				// A new sighting: the update (slot 3), then the count.
				const auto t = m_settings.aiTemplates.find(lowerCase(ai));
				refreshKnowledge(k, t != m_settings.aiTemplates.end() ? t->second.allowedTimeDiff : 0.0f, health, live, false);
				k.lastSeen = s->second;
			}
			if (k.sensed && !sensed) {
				k.position = live; // the count at 0 (0x100aa0f0)
				k.hasPosition = true;
			}
			k.sensed = sensed;
			if (sensed) {
				k.position = live;
				k.hasPosition = true;
			}
			++it;
		}
	}
}

float BotManager::knowledgeCertainty(int team, int64_t key, const std::string& aiTemplate) const
{
	// commonKnowledge (the template's +0x18): one object for every side,
	// the owner's static kind.
	const auto t = m_settings.aiTemplates.find(lowerCase(aiTemplate));
	if (t != m_settings.aiTemplates.end() && t->second.commonKnowledge) {
		return 1;
	}
	const auto it = m_knowledge[team].find(key);
	if (it == m_knowledge[team].end()) {
		return 0;
	}
	const float degeneration = t != m_settings.aiTemplates.end() ? t->second.degeneration : 0.0f;
	return ai::knowledgeCertainty(it->second.sensed, m_time - it->second.lastSeen, degeneration);
}

float BotManager::certainty(int team, int player) const
{
	GameWorld::PlayerInfo info;
	const GameWorld::PlayerInfo* found = playerOf(player);
	if (!found && m_world.playerInfo(player, info)) {
		found = &info;
	}
	if (!found) {
		return 0;
	}
	const GameWorld::PlayerInfo& p = *found;
	if (p.team == team || p.team < 1 || p.team > 2 || team < 1 || team > 2) {
		return 1;
	}
	const world::Object* soldier = m_world.objects().find(p.soldier);
	return knowledgeCertainty(team, player,
		soldier && soldier->objectTemplate() ? aiTemplateOf(soldier->objectTemplate()->name()) : std::string());
}

float BotManager::vehicleCertainty(int team, uint32_t vehicle) const
{
	const int owner = vehicleTeam(vehicle);
	if (owner == team || owner < 1 || owner > 2 || team < 1 || team > 2) {
		return 1; // its own side's or neutral: the static kind
	}
	const world::Object* o = m_world.objects().find(vehicle);
	return knowledgeCertainty(team, vehicleKey(vehicle),
		o && o->objectTemplate() ? aiTemplateOf(o->objectTemplate()->name()) : std::string());
}

bool BotManager::knownPosition(int team, int player, physics::Vec3& at) const
{
	GameWorld::PlayerInfo info;
	const GameWorld::PlayerInfo* found = playerOf(player);
	if (!found && m_world.playerInfo(player, info)) {
		found = &info;
	}
	if (!found || team < 1 || team > 2) {
		return false;
	}
	const GameWorld::PlayerInfo& p = *found;
	if (p.team == team || p.team < 1 || p.team > 2) {
		at = p.position;
		return true;
	}
	if (const world::Object* soldier = m_world.objects().find(p.soldier); soldier && soldier->objectTemplate()) {
		const auto t = m_settings.aiTemplates.find(lowerCase(aiTemplateOf(soldier->objectTemplate()->name())));
		if (t != m_settings.aiTemplates.end() && t->second.commonKnowledge) {
			at = p.position; // one object for all: live
			return true;
		}
	}
	const auto it = m_knowledge[team].find(player);
	if (it == m_knowledge[team].end() || !it->second.hasPosition) {
		return false;
	}
	at = it->second.sensed ? p.position : it->second.position;
	return true;
}

std::vector<uint32_t> BotManager::heardVehicles(int player) const
{
	std::vector<uint32_t> out;
	const auto it = m_bots.find(player);
	if (it != m_bots.end()) {
		for (const auto& [vehicle, known] : it->second.knownVehicles) {
			if (known.heardAt > -1e8f) {
				out.push_back(vehicle);
			}
		}
	}
	return out;
}

// Hearing. The dispatcher (BotManager 0x100ca280, its share of the AI's
// budget: 0.025, case 3 of 0x100ca590) walks the AI objects round robin;
// each with a team (adaptor slot 0x120) and an Armament or Mobile part
// makes a sound sphere (hearingReach), and every bot of another team
// within it (3-D), not dead (slot 0xb8) or slot 0x18c, is told (bot slot
// 0x164 = 0x100969b0, not for a scripted bot, +0x264) -> 0x100ec100: not
// within 1.5 s of the bot's own last round (+0xc0, 0x10097a60 from the
// rounds' event 0x1004e910), not its own object, not its team, and a
// soldier source only for a soldier listener (the listener's flag bit 9);
// then the roll (0x100ea820, hearingOdds). Heard: the memory item gets
// flag 1 and the source's position (0x100ea620), the side's object the
// certainty max(p, 0.1) (slot 2, 0x100a9bb0: heardLastSeen), and the item
// goes on the re-hearing list (0x100ec930: re-rolled, dropped 30 s after
// the last success). The listener is the bot's controlled object: on foot
// its soldier, at the wheel the vehicle, at a gun the seat's object, and a
// listener with a Mobile part never succeeds (hearingOdds), so of the
// shipped templates only the gunners' seats without one hear, and as they
// aren't soldiers only units that aren't soldiers: vehicles. A heard item
// has flag 1 only, which the fire behaviours' target loops pass over
// (they want flag 2 or 4, 0x1007e1b0's & 6): hearing feeds the side's
// knowledge. The rewrite runs the pass every tick over every unit, the
// weapon's "taken in hand" stamp (0x008a6d80) from the players' weapon in
// hand (a vehicle's from anyone aboard).
void BotManager::hear()
{
	struct Source {
		int team = 0;
		physics::Vec3 at;
		bool soldier = false;
		bool weapon3 = false, weapon5 = false;
		float weaponSphere = 50;
		float mobileSphere = 0;
		float health = 1;
		int64_t key = 0;
		uint32_t vehicle = 0;
		std::string ai;
	};
	std::vector<Source> sources;
	const auto weaponOf = [this](const GameWorld::PlayerInfo& p, Source& s) {
		const auto held = m_inHand.find(p.id);
		if (held == m_inHand.end() || held->second.weapon.empty()) {
			return;
		}
		const float age = m_time - held->second.since;
		if (age < 5) {
			const float sphere = weaponAi(held->second.weapon).soundSphere;
			s.weaponSphere = s.weapon5 ? std::max(s.weaponSphere, sphere) : sphere;
			s.weapon3 = s.weapon3 || age < 3;
			s.weapon5 = true;
		}
	};
	for (const GameWorld::PlayerInfo& p : m_players) {
		if (!p.alive || p.vehicle || p.team < 1 || p.team > 2) {
			continue;
		}
		Source s;
		s.team = p.team;
		s.at = p.position;
		s.soldier = true;
		s.key = p.id;
		s.health = healthOf(p.soldier);
		const world::Object* soldier = m_world.objects().find(p.soldier);
		s.ai = soldier && soldier->objectTemplate() ? aiTemplateOf(soldier->objectTemplate()->name()) : std::string();
		s.mobileSphere = soundSphereRadius(m_settings.plugin(s.ai, "Mobile"), std::sqrt(physics::lengthSquared(p.velocity)));
		weaponOf(p, s);
		sources.push_back(s);
	}
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		Source s;
		s.team = vehicleTeam(v.object);
		if (s.team < 1 || s.team > 2 || !v.working) {
			continue;
		}
		s.at = v.position;
		s.vehicle = v.object;
		s.key = vehicleKey(v.object);
		s.health = healthOf(v.object);
		const world::Object* o = m_world.objects().find(v.object);
		s.ai = o && o->objectTemplate() ? aiTemplateOf(o->objectTemplate()->name()) : std::string();
		s.soldier = (m_settings.typeFlags(s.ai) & kITSoldier) != 0;
		s.mobileSphere = soundSphereRadius(m_settings.plugin(s.ai, "Mobile"), std::sqrt(physics::lengthSquared(v.velocity)));
		for (const GameWorld::PlayerInfo& p : m_players) {
			if (p.alive && p.vehicle == v.object) {
				weaponOf(p, s);
			}
		}
		sources.push_back(s);
	}
	struct Listener {
		Bot* bot = nullptr;
		const GameWorld::PlayerInfo* self = nullptr;
		bool soldier = true;
		const AiPlugin* mobile = nullptr;
		float speed = 0;
	};
	std::vector<Listener> listeners;
	for (auto& [id, bot] : m_bots) {
		const GameWorld::PlayerInfo* self = playerOf(id);
		if (!self || !self->alive || self->team < 1 || self->team > 2 || m_deactivated.count(id) || m_time - bot.firedAt < 1.5f) {
			continue;
		}
		// The listener's controlled object (slot 0xcc).
		Listener l;
		l.bot = &bot;
		l.self = self;
		std::string ai;
		physics::Vec3 velocity = self->velocity;
		if (self->vehicle) {
			const world::Object* o = m_world.objects().find(self->vehicle);
			ai = o && o->objectTemplate() ? aiTemplateOf(o->objectTemplate()->name()) : std::string();
			if (!self->driving) {
				const std::vector<GameWorld::SeatLayout> seats = m_world.vehicleSeats(self->vehicle);
				if (self->seat >= 0 && static_cast<size_t>(self->seat) < seats.size()) {
					if (std::string seatAi = aiTemplateOf(seats[static_cast<size_t>(self->seat)].name); !seatAi.empty()) {
						ai = seatAi;
					}
				}
			}
			for (const GameWorld::VehicleInfo& v : m_vehicles) {
				velocity = v.object == self->vehicle ? v.velocity : velocity;
			}
			l.soldier = (m_settings.typeFlags(ai) & kITSoldier) != 0;
		} else if (const world::Object* soldier = m_world.objects().find(self->soldier); soldier && soldier->objectTemplate()) {
			ai = aiTemplateOf(soldier->objectTemplate()->name());
		}
		l.mobile = m_settings.plugin(ai, "Mobile");
		l.speed = std::sqrt(physics::lengthSquared(velocity));
		listeners.push_back(l);
	}
	for (Source& s : sources) {
		const float reach = hearingReach(s.weapon3, s.weaponSphere, s.mobileSphere);
		if (reach <= 0 || s.health <= 0) {
			continue;
		}
		for (const Listener& l : listeners) {
			const GameWorld::PlayerInfo& self = *l.self;
			if (self.team == s.team || (s.vehicle && self.vehicle == s.vehicle) || (!l.soldier && s.soldier)) {
				continue;
			}
			const float d2 = physics::lengthSquared(s.at - self.position);
			if (d2 > reach * reach) {
				continue;
			}
			float p = 0;
			const float odds = hearingOdds(std::sqrt(d2), s.weapon5, s.weaponSphere, s.mobileSphere, l.mobile, l.speed, p);
			if (odds <= 0 || !(random() <= odds)) {
				continue;
			}
			if (s.vehicle) {
				l.bot->knownVehicles[s.vehicle].heardAt = m_time;
			}
			// The side's object: certainty max(p, 0.1) (0x100317e0).
			const float c = std::max(p, 0.1f);
			if (knowledgeCertainty(self.team, s.key, s.ai) < c) {
				Knowledge& k = m_knowledge[self.team][s.key];
				const auto t = m_settings.aiTemplates.find(lowerCase(s.ai));
				refreshKnowledge(k, 0, s.health, s.at, true);
				k.position = s.at;
				k.hasPosition = true;
				k.lastSeen = heardLastSeen(m_time, c, t != m_settings.aiTemplates.end() ? t->second.degeneration : 0.0f);
			}
		}
	}
	// The re-hearing list's 30 s (0x100ec930: an item no longer heard is
	// dropped 30 s after it last was).
	for (auto& [id, bot] : m_bots) {
		for (auto it = bot.knownVehicles.begin(); it != bot.knownVehicles.end();) {
			if (it->second.heardAt > -1e8f && m_time - it->second.heardAt > 30) {
				it->second.heardAt = -1e9f;
			}
			it = it->second.heardAt <= -1e8f && it->second.seen <= -1e8f ? bot.knownVehicles.erase(it) : std::next(it);
		}
	}
}

// --- The commands ---

void registerBotScriptMethods(Console& console, BotSettings& settings)
{
	const uint32_t access = ConsoleAccess::Anyone;
	BotSettings* s = &settings;

	// aiRandom.seed (u32; 0x10023670 / 0x100237d0): the twister's seed word,
	// read or written; writing it doesn't seed again (only the AI's start
	// does, 0x100309a0), so it changes no draw.
	bindProperty<int>(console, "aiRandom.seed", access, [s] { return static_cast<int>(s->random.seedWord()); },
		[s](int seed) { s->random.setSeedWord(static_cast<uint32_t>(seed)); });

	// aiSettings.* (the settings object; the execute slots AIDLL 0x1000ade0..
	// 0x10011570 call its modifier vtable 0x10114828).
	bindMethod(console, "aiSettings.setMaxNAutoControllers", access, [s](int n) { s->maxAutoControllers = n; });
	bindMethod(console, "aiSettings.maxBotsIncludeHumans", access, [s](bool on) { s->maxBotsIncludeHumans = on; });
	bindMethod(console, "aiSettings.setIgnoreCommanderDistance", access, [s](float d) { s->ignoreCommanderDistance = d; });
	bindMethod(console, "aiSettings.setMaxNumberOfBehaviours", access, [s](int n) { s->maxBehaviours = n; });
	bindMethod(console, "aiSettings.setStatsViewDistance", access, [s](float d) { s->statsViewDistance = d; });
	bindMethod(console, "aiSettings.getStatsViewDistance", access, [s]() { return s->statsViewDistance; });
	bindMethod(console, "aiSettings.setInformationGridDimension", access, [s](int n) { s->informationGridDimension = n; });
	bindMethod(console, "aiSettings.getInformationGridDimension", access, [s]() { return s->informationGridDimension; });
	bindMethod(console, "aiSettings.setNSides", access, [s](int n) { s->sides = n; });
	bindMethod(console, "aiSettings.getNSides", access, [s]() { return s->sides; });
	bindMethod(console, "aiSettings.getBotSkill", access, [s]() { return s->skill; });
	bindMethod(console, "aiSettings.setSAIUpdateFrequency", access, [s](float f) { s->saiUpdateFrequency = f; });
	bindMethod(console, "aiSettings.getSAIUpdateFrequency", access, [s]() { return s->saiUpdateFrequency; });
	bindMethod(console, "aiSettings.setRespawnAllowed", access, [s](bool on) { s->respawnAllowed = on; });
	bindMethod(console, "aiSettings.isRespawnAllowed", access, [s]() { return s->respawnAllowed; });
	bindMethod(console, "aiSettings.autoSpawnBots", access, [s](bool on) { s->autoSpawnBots = on; });
	bindMethod(console, "aiSettings.overrideMenuSettings", access, [s](bool on) { s->overrideMenuSettings = on; });
	bindMethod(console, "aiSettings.setVehicleClusterMapping", access,
		[s](std::string type, std::string cluster) { s->clusterMapping[type] = cluster; });
	bindMethod(console, "aiSettings.getVehicleClusterMapping", access, [s](std::string type) {
		const auto it = s->clusterMapping.find(type);
		return it != s->clusterMapping.end() ? it->second : type;
	});
	bindMethod(console, "aiSettings.setManDownEquipment", access, [s](std::string name) { s->manDownEquipment = name; });
	bindMethod(console, "aiSettings.setBasicBotWeights", access, [s](std::string set) {
		if (s->modifiers.count(set)) { // 0x10028da0: only a set that exists
			s->basicBotWeights = set;
		}
	});
	bindMethod(console, "aiSettings.setStandardPersonality", access,
		[s](std::string set, int side) { s->standardPersonality[side] = set; });
	bindMethod(console, "aiSettings.createVehicleGroup", access, [s](std::string name) { s->vehicleGroups[name]; });
	bindMethod(console, "aiSettings.addVehicleToVehicleGroup", access, [s](std::string group, std::string vehicle) {
		if (const auto it = s->vehicleGroups.find(group); it != s->vehicleGroups.end()) {
			it->second.push_back(vehicle);
		}
	});
	bindMethod(console, "aiSettings.getNumberOfVehicleGroups", access, [s]() { return static_cast<int>(s->vehicleGroups.size()); });
	bindMethod(console, "aiSettings.addBotName", access, [s](std::string first, std::string last) {
		// 0x10050720: the key is the AI's next word as a float x 2^-32
		// (x 0.4 x 2^-32, 9.313226e-11, for the first six; addBotName
		// applies the 0.4).
		s->addBotName(first, last, ai::wordToFloat(s->random.next()) * 2.3283064e-10f);
	});
	bindMethod(console, "aiSettings.addInterpreterEntry", access, [s](std::string equipment, std::string statement) {
		std::vector<std::string>& list = s->interpreter[equipment];
		if (std::find(list.begin(), list.end(), statement) == list.end()) {
			list.push_back(statement);
		}
	});
	// The AIConsole halves: removeInterpreterEntry 0x10029e80,
	// activateDefaultInterpreter 0x10027210, getBehaviourModifier
	// 0x100278d0, activeBehaviourModifiers 0x10027f40 (the active set's
	// name 0x10027890).
	bindMethod(console, "aiSettings.removeInterpreterEntry", access, [s](std::string equipment, std::string statement) {
		std::vector<std::string>& list = s->interpreter[equipment];
		list.erase(std::remove(list.begin(), list.end(), statement), list.end());
	});
	bindMethod(console, "aiSettings.activateDefaultInterpreter", access,
		[s](std::string equipment, std::string type) { s->defaultInterpreter[equipment] = type; });
	bindMethod(console, "aiSettings.setSensingAgentPatternName", access,
		[s](std::string name) { s->sensingPatternNames.push_back(name); });
	bindMethod(console, "aiSettings.setSensingAgentNullPattern", access, [s](std::string name) { s->sensingNullPattern = name; });
	bindMethod(console, "aiSettings.getBehaviourModifier", access, [s](std::string behaviour) {
		const auto set = s->modifiers.find(s->editing);
		if (set == s->modifiers.end()) {
			return 0.0f;
		}
		const auto it = set->second.find(behaviour);
		return it != set->second.end() ? it->second : 0.0f;
	});
	bindMethod(console, "aiSettings.activeBehaviourModifiers", access, [s](std::string set) {
		if (s->modifiers.count(set)) {
			s->editing = set;
		}
	});
	// The list's text: the settings' 0x10027990 (a name a line).
	bindMethod(console, "aiSettings.listBehaviourModifiers", access, [s]() {
		std::string out;
		for (const auto& [name, set] : s->modifiers) {
			out += name + "\n";
		}
		return out;
	});

	// aiPathfinding.activeMap (0x1001ff10): the map the aiPathfinding.map.*
	// lines edit, as setActiveMap.
	bindMethod(console, "aiPathfinding.activeMap", access, [s](std::string name) {
		for (size_t i = 0; i < s->pathfindingMaps.size(); ++i) {
			if (lowerCase(s->pathfindingMaps[i].name) == lowerCase(name)) {
				s->activeMap = static_cast<int>(i);
			}
		}
	});

	// aiBotManager.* (AIBotManager: the bot manager's vtbl 0x101196b0).
	bindMethod(console, "aiBotManager.setPlannedDecisionMakingThreshold", access,
		[s](double t) { both(s, [t](BotSettings& x) { x.plannedThreshold = std::fabs(t); }); });
	bindMethod(console, "aiBotManager.setUnplannedDecisionMakingThreshold", access,
		[s](double t) { both(s, [t](BotSettings& x) { x.unplannedThreshold = std::fabs(t); }); });
	bindMethod(console, "aiBotManager.setDecisionMakingInterleave", access, [s](int planned, int unplanned) {
		both(s, [=](BotSettings& x) {
			x.plannedInterleave = planned;
			x.unplannedInterleave = unplanned;
		});
	});
	bindMethod(console, "aiBotManager.setSensingQuotient", access,
		[s](unsigned a, unsigned b) { both(s, [=](BotSettings& x) { x.sensingQuotient = {a, b}; }); });
	bindMethod(console, "aiBotManager.setSystemQuotient", access,
		[s](unsigned d, unsigned p, unsigned n) { both(s, [=](BotSettings& x) { x.systemQuotient = {d, p, n}; }); });
	bindMethod(console, "aiBotManager.getPlannedDecisionMakingThresholdFull", access, [s]() { return s->plannedThreshold; });
	bindMethod(console, "aiBotManager.getUnplannedDecisionMakingThresholdFull", access, [s]() { return s->unplannedThreshold; });
	bindMethod(console, "aiBotManager.getPlannedDecisionMakingInterleave", access, [s]() { return s->plannedInterleave; });
	bindMethod(console, "aiBotManager.getUnplannedDecisionMakingInterleave", access, [s]() { return s->unplannedInterleave; });
	bindMethod(console, "aiBotManager.getSensingQuotient", access, [s]() {
		const double total = static_cast<double>(s->sensingQuotient[0]) + s->sensingQuotient[1];
		return total > 0 ? s->sensingQuotient[0] / total : 0.0;
	});

	// autoControllerTemplate.* (AIDLL 0x10002210..0x100049b0; the template
	// manager DAT_101394e8, constructor 0x10034520; the properties go to
	// its active template, 0x10033270, whose name is 0x10033240). A
	// property before any create is lost.
	bindMethod(console, "autoControllerTemplate.create", access, [s](std::string name) {
		s->autoControllers[name] = BotSettings::AutoControllerTemplate {};
		s->editingAutoController = name;
	});
	bindMethod(console, "autoControllerTemplate.active", access, [s](std::string name) {
		if (s->autoControllers.count(name)) {
			s->editingAutoController = name;
		}
	});
	using Ac = BotSettings::AutoControllerTemplate;
	const auto editing = [s]() -> Ac* {
		const auto it = s->autoControllers.find(s->editingAutoController);
		return it != s->autoControllers.end() ? &it->second : nullptr;
	};
	const std::pair<const char*, float Ac::*> floats[] = {{"scanningSpeed", &Ac::scanningSpeed}, {"accuracy", &Ac::accuracy},
		{"fov", &Ac::fov}, {"viewDistance", &Ac::viewDistance}, {"reactionTime", &Ac::reactionTime},
		{"targetLostTimeout", &Ac::targetLostTimeout}, {"maxTrackingInput", &Ac::maxTrackingInput},
		{"discoverCloakedEnemiesDistance", &Ac::discoverCloakedEnemiesDistance}, {"flipOutAtHealth", &Ac::flipOutAtHealth},
		{"scanningInputScale", &Ac::scanningInputScale}};
	for (const auto& [name, member] : floats) {
		bindMethod(console, std::string("autoControllerTemplate.") + name, access, [editing, m = member](float v) {
			if (Ac* t = editing()) {
				t->*m = v;
			}
		});
	}
	const std::pair<const char*, bool Ac::*> flags[] = {{"sendFireWarning", &Ac::sendFireWarning},
		{"operatesAlone", &Ac::operatesAlone}, {"useSiren", &Ac::useSiren}, {"neverAttackDeployer", &Ac::neverAttackDeployer},
		{"reportEnemiesToTeam", &Ac::reportEnemiesToTeam}, {"allowTakeOver", &Ac::allowTakeOver},
		{"checkLineOfFire", &Ac::checkLineOfFire}, {"reloadOnTeamChange", &Ac::reloadOnTeamChange},
		{"onlyAttackNetworkBattlefieldObjects", &Ac::onlyAttackNetworkBattlefieldObjects},
		{"simpleFirePlan", &Ac::simpleFirePlan}};
	for (const auto& [name, member] : flags) {
		bindMethod(console, std::string("autoControllerTemplate.") + name, access, [editing, m = member](bool v) {
			if (Ac* t = editing()) {
				t->*m = v;
			}
		});
	}

	// aiStrategy.* left (the strategy manager DAT_101394f0, got by
	// 0x10039880). Its halves: setActiveStrategy 0x1003a2a0 (an unknown
	// name makes the original's active one "NULL"), getActiveStrategyName
	// 0x10039890, listStrategies 0x10039df0, setActivePrerequisite
	// 0x1003a4f0, getActivePrerequisiteName 0x100398b0, setActiveCondition
	// 0x1003a640, getActiveConditionName 0x10039f60; the active condition
	// 0x10039f50.
	ai::StrategySettings* st = &settings.strategy;
	const auto strategy = [st]() -> ai::Strategy* {
		const auto it = st->strategies.find(st->strategy);
		return it != st->strategies.end() ? &it->second : nullptr;
	};
	bindMethod(console, "aiStrategy.setActiveStrategy", access, [st](std::string name) {
		if (st->strategies.count(name)) {
			st->strategy = name;
		}
	});
	bindMethod(console, "aiStrategy.getActiveStrategyName", access, [st]() { return st->strategy; });
	bindMethod(console, "aiStrategy.timeLimit", access, [strategy](float v) {
		if (ai::Strategy* x = strategy()) {
			x->timeLimit = v; // +0x20 (1e9 at first, 0x1005cb60)
		}
	});
	bindMethod(console, "aiStrategy.setPrerequisiteWeight", access, [strategy](float v) {
		if (ai::Strategy* x = strategy()) {
			x->prerequisiteWeight = v; // +0x1c
		}
	});
	bindMethod(console, "aiStrategy.getPrerequisiteWeight", access, [strategy]() {
		const ai::Strategy* x = strategy();
		return x ? x->prerequisiteWeight : 0.0f;
	});
	bindMethod(console, "aiStrategy.clearAllStrategicObjectsModifiers", access, [strategy]() {
		if (ai::Strategy* x = strategy()) {
			for (auto& row : x->modifiers) {
				row.fill(1.0f);
			}
		}
	});
	bindMethod(console, "aiStrategy.listStrategies", access, [st]() {
		std::string out;
		for (const auto& [name, x] : st->strategies) {
			out += name + "\n";
		}
		return out;
	});
	bindMethod(console, "aiStrategy.setActivePrerequisite", access, [st](std::string name) {
		if (st->prerequisites.count(name)) {
			st->prerequisite = name;
		}
	});
	bindMethod(console, "aiStrategy.getActivePrerequisiteName", access, [st]() { return st->prerequisite; });
	bindMethod(console, "aiStrategy.setActiveCondition", access, [st](std::string name) {
		if (st->conditions.count(name)) {
			st->condition = name;
		}
	});
	bindMethod(console, "aiStrategy.getActiveConditionName", access, [st]() { return st->condition; });
	bindMethod(console, "aiStrategy.getStrategicObjectsModifier", access, [strategy](std::string type, std::string status) {
		static const std::pair<const char*, int> types[] = {{"controlpoint", ai::kTypeControlPoint}, {"front", ai::kTypeFront},
			{"safe", ai::kTypeSafe}, {"enemy", ai::kTypeEnemy}, {"neutral", ai::kTypeNeutral}};
		const std::string st2 = lowerCase(status);
		const int which = st2 == "hostile" ? 1 : st2 == "neutral" ? 2 : 0;
		const ai::Strategy* x = strategy();
		for (const auto& [n, id] : types) {
			if (x && lowerCase(type) == n) {
				return x->modifiers[static_cast<size_t>(which)][static_cast<size_t>(id)];
			}
		}
		return 1.0f;
	});
	// forceStrategy <name> <side> (0x10017560 -> 0x1003a720), answering
	// with the original's messages.
	bindMethod(console, "aiStrategy.forceStrategy", access, [s](std::string name, int side) -> std::string {
		if (side != 1 && side != 2) {
			return "Side must be 1 or 2, current side: " + std::to_string(side);
		}
		const bool known = s->strategy.strategies.count(name) != 0;
		both(s, [&](BotSettings& x) { x.strategy.forced[static_cast<size_t>(side)] = known ? name : std::string(); });
		return (known ? "Strategy is activated : " : "No active strategy chosen : ") + name;
	});

	// aiStrategicArea.* left (AIStrategicArea). The area manager's halves
	// (DAT_101394ec, 0x10034590): create's argument parse 0x10017dc0 /
	// 0x10018010, getActive 0x10036380, addTwoWayNeighbour 0x10036140,
	// removeAllNeighbours 0x10036220, removeObjectTypeFlag 0x10036270,
	// removeAllObjectTypeFlags 0x100362f0, getOrderPosition 0x10036420,
	// isInside 0x10036780, getStrategicAreaNames 0x100365f0.
	bindMethod(
		console, "aiStrategicArea.create", access,
		[st](std::string name, ConsoleVec2 corner, ConsoleVec2 other, float height, float temperature, std::string) {
			// An area of its own (0x10017a00): a box, no control point. The
			// rewrite's SAI keeps its per-side status (ai::boxAreaStatus,
			// 0x1005f210) for its neighbours; it orders units only to the
			// control point areas (see docs/engine.md, Strategy).
			ai::StrategicArea area;
			area.name = name;
			area.temperature = temperature;
			area.box = true;
			area.boxMin[0] = std::min(corner.x, other.x);
			area.boxMin[1] = std::min(corner.y, other.y);
			area.boxMax[0] = std::max(corner.x, other.x);
			area.boxMax[1] = std::max(corner.y, other.y);
			area.height = height;
			st->areas.push_back(area);
			st->area = name;
		},
		5);
	bindMethod(console, "aiStrategicArea.getActive", access, [st]() { return st->area; });
	bindMethod(console, "aiStrategicArea.addTwoWayNeighbour", access, [st](std::string a, std::string b) {
		ai::StrategicArea* x = st->findArea(a);
		ai::StrategicArea* y = st->findArea(b);
		if (x && y) {
			x->neighbours.push_back(b);
			y->neighbours.push_back(a);
		}
	});
	bindMethod(console, "aiStrategicArea.removeAllNeighbours", access, [st]() {
		if (ai::StrategicArea* a = st->findArea(st->area)) {
			a->neighbours.clear();
		}
	});
	bindMethod(console, "aiStrategicArea.getSide", access, [st]() {
		const ai::StrategicArea* a = st->findArea(st->area);
		return a ? a->side : 0;
	});
	// setTakeable <side> <bool> (0x1001ab30: the area's +399 + side).
	bindMethod(console, "aiStrategicArea.setTakeable", access, [st](int side, bool takeable) {
		if (ai::StrategicArea* a = st->findArea(st->area); a && side >= 0 && side <= 2) {
			a->takeable[static_cast<size_t>(side)] = takeable;
		}
	});
	bindMethod(console, "aiStrategicArea.isTakeable", access, [st](int side) {
		const ai::StrategicArea* a = st->findArea(st->area);
		return a && side >= 0 && side <= 2 && a->takeable[static_cast<size_t>(side)];
	});
	bindMethod(console, "aiStrategicArea.basicTemperature", access, [st](float t) {
		if (ai::StrategicArea* a = st->findArea(st->area)) {
			a->temperature = t;
		}
	});
	bindMethod(console, "aiStrategicArea.getTemperature", access, [st]() {
		const ai::StrategicArea* a = st->findArea(st->area);
		return a ? a->temperature : 0.0f;
	});
	bindMethod(console, "aiStrategicArea.getNeighbours", access, [st]() {
		std::string out;
		if (const ai::StrategicArea* a = st->findArea(st->area)) {
			for (const std::string& n : a->neighbours) {
				out += n + "\n";
			}
		}
		return out;
	});
	bindMethod(console, "aiStrategicArea.removeObjectTypeFlag", access, [st](std::string type) {
		if (ai::StrategicArea* a = st->findArea(st->area); a && lowerCase(type) == "controlpoint") {
			a->controlPointFlag = false;
		}
	});
	bindMethod(console, "aiStrategicArea.removeAllObjectTypeFlags", access, [st]() {
		if (ai::StrategicArea* a = st->findArea(st->area)) {
			a->controlPointFlag = false;
		}
	});
	bindMethod(console, "aiStrategicArea.getOrderPosition", access, [st](std::string map) {
		ConsoleVec3 at;
		const int m = lowerCase(map) == "vehicle" ? 1 : 0;
		if (const ai::StrategicArea* a = st->findArea(st->area); a && a->hasOrderPosition[m]) {
			at = {a->orderPosition[m][0], a->orderPosition[m][1], a->orderPosition[m][2]};
		}
		return at;
	});
	// isInside <area> <x/z> (0x1001e330): a box area's test.
	bindMethod(console, "aiStrategicArea.isInside", access, [st](std::string name, ConsoleVec2 at) {
		const ai::StrategicArea* a = st->findArea(name);
		return a && a->box && at.x >= a->boxMin[0] && at.x <= a->boxMax[0] && at.y >= a->boxMin[1] && at.y <= a->boxMax[1];
	});
	bindMethod(console, "aiStrategicArea.getStrategicAreaNames", access, [st]() {
		std::string out;
		for (const ai::StrategicArea& a : st->areas) {
			out += a.name + "\n";
		}
		return out;
	});

	// ai.* (AI): the scripted bots, on the running manager.
	bindMethod(console, "ai.botMoveTo", access, [s](float radius, ConsoleVec3 at, int bot) {
		if (s->manager) {
			s->manager->scriptMoveTo(bot, radius, toPhysics(at));
		}
	});
	bindMethod(console, "ai.botFlyTo", access, [s](int bot, float radius, float height, float, ConsoleVec3 at) {
		if (s->manager) {
			s->manager->scriptFlyTo(bot, radius, height, toPhysics(at));
		}
	});
	// The patrols' argument parses: 0x100069d0 and, for Patrol3d,
	// 0x10007000 (a point each, 0x10006c00).
	bindMethod(
		console, "ai.botSetWaypoints_Patrol", access,
		[s](int bot, float radius, int count, ConsoleVec3 a, ConsoleVec3 b, ConsoleVec3 c) {
			if (s->manager) {
				s->manager->scriptGoal(bot, ai::patrolGoal(radius, patrolPoints(count, a, b, c)));
			}
		},
		4);
	bindMethod(
		console, "ai.botSetWaypoints_Patrol3d", access,
		[s](int bot, float radius, int count, ConsoleVec3 a, ConsoleVec3 b, ConsoleVec3 c) {
			if (s->manager) {
				s->manager->scriptGoal(bot, ai::patrol3dGoal(radius, patrolPoints(count, a, b, c)));
			}
		},
		4);
	bindMethod(console, "ai.botGiveOrder", access, [s](int bot, std::string behaviour, std::string set) {
		if (s->manager) {
			s->manager->giveOrder(bot, behaviour, set);
		}
	});
	bindMethod(console, "ai.deactivateBot", access, [s](int bot) { return s->manager && s->manager->deactivate(bot); });
	bindMethod(console, "ai.updateBot", access, [s](int bot) {
		if (s->manager) {
			s->manager->thinkSoon(bot);
		}
	});
	bindMethod(console, "ai.getBotCount", access, [s]() { return s->manager ? s->manager->botCount() : 0; });
	bindMethod(console, "ai.getNumberOfActiveBots", access,
		[s]() { return s->manager ? s->manager->botCount() - s->manager->deactivatedCount() : 0; });
	bindMethod(console, "ai.getNumberOfDeactivedBots", access, [s]() { return s->manager ? s->manager->deactivatedCount() : 0; });
	// ai.addBot <name> <team> <personality> <equipment> (0x10005bc0): a
	// bot player through the game's AI player factory (BF2142.exe
	// 0x0089e7a0, which names a nameless one by nextBotName); its id.
	// The original's body (0x10028c00) answers -1 without an SAI, for a
	// team outside 0..2, an equipment type it doesn't know ("NoEquipment"
	// is none) or a personality it doesn't have (+4's map), and otherwise
	// the bot manager's count less one (slot 8) after AIMain's add
	// 0x100580c0; the rewrite answers the new player's id and doesn't
	// check the personality or equipment names (the shipped scripts don't
	// read the answer).
	bindMethod(
		console, "ai.addBot", access,
		[s](std::string name, int team, std::string, std::string) { return s->manager ? s->manager->addBot(name, team) : -1; },
		2);
	// ai.init (0x10004dc0): the level's AI/AI.ai starts the AI (AIMain's
	// initiation 0x10026eb0: AIMain made and started, 0x10056b20; a level
	// without it has no bots, see BotManager::setNavMesh).
	bindMethod(console, "ai.init", access, [s]() { s->initiated = true; });
	// addUnspawnedBot (0x10005f90 -> 0x10028d70) does nothing and answers -1.
	bindMethod(console, "ai.addUnspawnedBot", access, [](std::string, int, std::string) { return -1; });
}

} // namespace dice::hfe::game
