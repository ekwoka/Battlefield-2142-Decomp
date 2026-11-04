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

// What the sides' bots know of the enemy soldiers: a bot that sees one now
// (its memory item in sight, by itself or a vehicle mate) senses it; the
// time of the last sighting stays after the bots forget it. A soldier that
// died is a new unit when it spawns again.
void BotManager::updateKnowledge()
{
	for (int team = 1; team <= 2; ++team) {
		for (auto& [id, k] : m_knowledge[team]) {
			k.sensed = false;
		}
	}
	for (const auto& [id, bot] : m_bots) {
		GameWorld::PlayerInfo self;
		if (!m_world.playerInfo(id, self) || self.team < 1 || self.team > 2 || !self.alive) {
			continue;
		}
		for (const Known& known : bot.known) {
			if (!known.everSeen) {
				continue;
			}
			Knowledge& k = m_knowledge[self.team][known.player];
			k.lastSeen = std::max(k.lastSeen, known.seen);
			k.sensed = k.sensed || known.lostAt < 0;
		}
	}
	for (int team = 1; team <= 2; ++team) {
		for (auto it = m_knowledge[team].begin(); it != m_knowledge[team].end();) {
			GameWorld::PlayerInfo p;
			it = m_world.playerInfo(it->first, p) && p.alive ? std::next(it) : m_knowledge[team].erase(it);
		}
	}
}

float BotManager::certainty(int team, int player) const
{
	GameWorld::PlayerInfo p;
	if (!m_world.playerInfo(player, p)) {
		return 0;
	}
	if (p.team == team || p.team < 1 || p.team > 2 || team < 1 || team > 2) {
		return 1;
	}
	// commonKnowledge (the template's +0x18): one object for every side,
	// the owner's static kind.
	if (const world::Object* soldier = m_world.objects().find(p.soldier); soldier && soldier->objectTemplate()) {
		const auto t = m_settings.aiTemplates.find(lowerCase(aiTemplateOf(soldier->objectTemplate()->name())));
		if (t != m_settings.aiTemplates.end() && t->second.commonKnowledge) {
			return 1;
		}
	}
	const auto it = m_knowledge[team].find(player);
	if (it == m_knowledge[team].end()) {
		return 0;
	}
	return ai::knowledgeCertainty(it->second.sensed, m_time - it->second.lastSeen, degenerationOf(p));
}

// --- The commands ---

void registerBotScriptMethods(Console& console, BotSettings& settings)
{
	const uint32_t access = ConsoleAccess::Anyone;
	BotSettings* s = &settings;

	// aiSettings.* (the settings object; the execute slots 0x1000ade0..
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
		// The key's U[0, 1): a fixed sequence here.
		s->botNameSeed = s->botNameSeed * 1664525u + 1013904223u;
		s->addBotName(first, last, static_cast<float>(s->botNameSeed >> 8) / static_cast<float>(1u << 24));
	});
	bindMethod(console, "aiSettings.addInterpreterEntry", access, [s](std::string equipment, std::string statement) {
		std::vector<std::string>& list = s->interpreter[equipment];
		if (std::find(list.begin(), list.end(), statement) == list.end()) {
			list.push_back(statement);
		}
	});
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

	// autoControllerTemplate.* (0x10002210..0x100049b0; the template
	// manager DAT_101394e8). A property before any create is lost.
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

	// aiStrategy.* left (the strategy manager DAT_101394f0).
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

	// aiStrategicArea.* left (AIStrategicArea).
	bindMethod(
		console, "aiStrategicArea.create", access,
		[st](std::string name, ConsoleVec2 corner, ConsoleVec2 other, float height, float temperature, std::string) {
			// An area of its own (0x10017a00): a box, no control point. Kept
			// for the names; the rewrite's SAI weighs only the control
			// point areas (see docs/engine.md, Strategy).
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
	bindMethod(
		console, "ai.addBot", access,
		[s](std::string name, int team, std::string, std::string) { return s->manager ? s->manager->addBot(name, team) : -1; },
		2);
	// ai.init (0x10004dc0): the level's AI/AI.ai starts the AI (AIMain's
	// initiation; a level without it has no bots, see BotManager::setNavMesh).
	bindMethod(console, "ai.init", access, [s]() { s->initiated = true; });
	// addUnspawnedBot (0x10005f90 -> 0x10028d70) does nothing and answers -1.
	bindMethod(console, "ai.addUnspawnedBot", access, [](std::string, int, std::string) { return -1; });
}

} // namespace dice::hfe::game
