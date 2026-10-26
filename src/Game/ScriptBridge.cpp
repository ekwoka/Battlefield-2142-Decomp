#include "ScriptBridge.h"

#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <cstdio>
#include <cstdlib>
#include <map>

namespace dice::hfe::game {

namespace {

const std::string* setting(const world::ObjectTemplate& t, const std::string& name)
{
	const auto it = t.settings().find(name);
	return it == t.settings().end() ? nullptr : &it->second;
}

// What a template property is when its script leaves it out. The
// ControlPoint template's save (0x005b8xxx) writes only values other than
// these: radius 1, timeToGetControl and timeToLoseControl 10.
const std::string* templateDefault(const world::ObjectTemplate& t, const std::string& name)
{
	static const std::map<std::pair<std::string, std::string>, std::string> defaults = {
		{{"ControlPoint", "radius"}, "1"},
		{{"ControlPoint", "timeToGetControl"}, "10"},
		{{"ControlPoint", "timeToLoseControl"}, "10"},
	};
	const auto it = defaults.find({t.typeName(), name});
	return it == defaults.end() ? nullptr : &it->second;
}

float number(const world::ObjectTemplate& t, const std::string& name, float fallback)
{
	const std::string* value = setting(t, name);
	return value ? static_cast<float>(std::atof(value->c_str())) : fallback;
}

int teamArgument(const ScriptArgs& args, size_t index, int fallback)
{
	return args.size() > index ? static_cast<int>(args[index].asInt()) : fallback;
}

} // namespace

ScriptBridge::ScriptBridge(GameWorld& world, GameLogic& logic, ServerSettings settings, std::string modDirectory)
	: m_world(world), m_logic(logic), m_settings(std::move(settings)), m_modDirectory(std::move(modDirectory))
{
}

ScriptValue ScriptBridge::gameParam(const std::string& name, int team, int id) { return m_logic.param(name, team, id); }

void ScriptBridge::setGameParam(const std::string& name, int team, const ScriptValue& value, int id)
{
	m_logic.setParam(name, team, value, id);
}

ScriptValue ScriptBridge::serverSetting(const std::string& name)
{
	if (name == "ticketRatio") {
		return ScriptValue::integer(m_settings.ticketRatio);
	}
	if (name == "teamRatioPct") {
		return ScriptValue::integer(m_settings.teamRatioPercent);
	}
	if (name == "maxPlayers") {
		return ScriptValue::integer(m_settings.maxPlayers);
	}
	if (name == "gameMode") {
		return ScriptValue::string(m_settings.gameMode);
	}
	if (name == "mapName") {
		return ScriptValue::string(m_settings.mapName);
	}
	if (name == "timeLimit") {
		return ScriptValue::integer(m_settings.timeLimit);
	}
	if (name == "scoreLimit") {
		return ScriptValue::integer(m_settings.scoreLimit);
	}
	if (name == "maxRank") {
		return ScriptValue::integer(m_settings.maxRank);
	}
	return ScriptValue::integer(0); // autoBalance, tkp*, globRank, globUnlocks: off
}

void ScriptBridge::textMessage(int, const std::string& text) { messages.push_back(text); }

void ScriptBridge::gameEvent(const std::string& kind, const ScriptArgs& args)
{
	std::string text = kind;
	for (const ScriptValue& a : args) {
		text += ' ' + std::to_string(a.asInt());
	}
	messages.push_back(text);
}

std::vector<uint32_t> ScriptBridge::objectsOfType(const std::string& type)
{
	// "dice.hfe.world.ObjectTemplate.ControlPoint": the template type's name.
	const std::string name = type.substr(type.rfind('.') + 1);
	std::vector<uint32_t> found;
	for (const auto& object : m_world.objects().objects()) {
		if (object->objectTemplate() && object->objectTemplate()->typeName() == name) {
			found.push_back(object->id());
		}
	}
	return found;
}

std::vector<uint32_t> ScriptBridge::objectsOfTemplate(const std::string& name)
{
	std::vector<uint32_t> found;
	for (const auto& object : m_world.objects().objects()) {
		if (object->objectTemplate() && object->objectTemplate()->name() == name) {
			found.push_back(object->id());
		}
	}
	return found;
}

uint32_t ScriptBridge::parentOf(uint32_t object)
{
	GameWorld::PlayerInfo info;
	if (m_world.playerInfo(m_world.playerOf(object), info) && info.soldier == object) {
		return info.vehicle;
	}
	return 0;
}

std::vector<uint32_t> ScriptBridge::triggerCandidates(const std::string&)
{
	std::vector<uint32_t> found;
	for (const GameWorld::PlayerInfo& player : m_world.playerList()) {
		if (player.soldier) {
			found.push_back(player.soldier);
		}
	}
	for (const auto& object : m_world.objects().objects()) {
		if (m_world.isVehicle(object->id())) {
			found.push_back(object->id());
		}
	}
	return found;
}

bool ScriptBridge::objectPosition(uint32_t id, float position[3])
{
	const world::Object* object = m_world.objects().find(id);
	if (!object) {
		return false;
	}
	position[0] = object->position().x;
	position[1] = object->position().y;
	position[2] = object->position().z;
	return true;
}

ScriptValue ScriptBridge::controlPointParam(ControlPoint& point, const std::string& name, const ScriptArgs& args)
{
	const world::ObjectTemplate& t = *point.object->objectTemplate();
	if (name == "team") {
		return ScriptValue::integer(point.team);
	}
	if (name == "flag") {
		return ScriptValue::integer(point.flag);
	}
	if (name == "takeOverChangePerSecond") {
		return ScriptValue::number(point.takeOverChangePerSecond);
	}
	if (name == "playerId") {
		return ScriptValue::integer(point.playerId);
	}
	if (name == "allowCaptureByTeam") {
		const int team = teamArgument(args, 0, 0);
		return ScriptValue::integer(team >= 0 && team <= 2 && point.allowCapture[team] ? 1 : 0);
	}
	if (name == "areaValue") {
		const int team = teamArgument(args, 0, point.team);
		return ScriptValue::number(number(t, team == 2 ? "areaValueTeam2" : "areaValueTeam1", 0));
	}
	// Template settings, numbers (onlyTakeableByTeam, unableToChangeTeam,
	// isHemisphere, timeToGetControl, timeToLoseControl,
	// enemyTicketLossWhenCaptured, supplyGroupId, supplyValue...).
	const float value = number(t, name, 0);
	if (name == "timeToGetControl" || name == "timeToLoseControl") {
		return ScriptValue::number(value);
	}
	return ScriptValue::integer(static_cast<int64_t>(value));
}

void ScriptBridge::setControlPointParam(ControlPoint& point, const std::string& name, const ScriptArgs& args)
{
	const ScriptValue value = args.size() > 0 ? args[0] : ScriptValue::none();
	if (name == "team") {
		m_world.setControlPointTeam(point.id, static_cast<int>(value.asInt()));
	} else if (name == "flag") {
		point.flag = static_cast<int>(value.asInt());
	} else if (name == "takeOverChangePerSecond") {
		point.takeOverChangePerSecond = static_cast<float>(value.asNumber());
	} else if (name == "playerId") {
		point.playerId = static_cast<int>(value.asInt());
	} else if (name == "allowCaptureByTeam" && args.size() >= 2) {
		const int team = static_cast<int>(args[0].asInt());
		if (team >= 0 && team <= 2) {
			point.allowCapture[team] = args[1].asInt() != 0;
		}
	}
}

ScriptValue ScriptBridge::objectCall(uint32_t id, const std::string& method, const ScriptArgs& args)
{
	world::Object* object = m_world.objects().find(id);
	if (method == "isValid") {
		return ScriptValue::integer(object ? 1 : 0);
	}
	if (!object) {
		return ScriptValue::none();
	}
	const world::ObjectTemplate* t = object->objectTemplate();
	if (method == "getPosition") {
		return ScriptValue::vec3(object->position().x, object->position().y, object->position().z);
	}
	if (method == "getRotation") {
		return ScriptValue::vec3(object->rotation().x, object->rotation().y, object->rotation().z);
	}
	if (method == "setPosition" && !args.empty() && args[0].kind == ScriptValue::Kind::Vec3) {
		if (m_world.setSoldierPosition(id, {args[0].v[0], args[0].v[1], args[0].v[2]})) {
			return ScriptValue::none();
		}
		object->setPosition({args[0].v[0], args[0].v[1], args[0].v[2]});
		return ScriptValue::none();
	}
	if (method == "templateName") {
		return ScriptValue::string(t ? t->name() : std::string());
	}
	if (method == "getTemplateProperty" && t && !args.empty()) {
		const std::string* value = setting(*t, args[0].s);
		if (!value) {
			value = templateDefault(*t, args[0].s);
		}
		return value ? ScriptValue::string(*value) : ScriptValue::none();
	}
	if (method == "getParent") {
		return ScriptValue::object(parentOf(id));
	}
	if (method == "getChildren") {
		std::vector<ScriptValue> children;
		for (const GameWorld::PlayerInfo& player : m_world.playerList()) {
			if (player.vehicle == id) {
				children.push_back(ScriptValue::object(player.soldier));
			}
		}
		return ScriptValue::makeList(children);
	}
	if (method == "getTeam") {
		return ScriptValue::integer(object->team());
	}
	if (method == "setTeam" && !args.empty()) {
		object->setTeam(static_cast<int>(args[0].asInt()));
		return ScriptValue::none();
	}
	if (method == "getOccupyingPlayers") {
		std::vector<ScriptValue> players;
		for (const GameWorld::PlayerInfo& player : m_world.playerList()) {
			if (player.alive && (player.soldier == id || player.vehicle == id)) {
				players.push_back(ScriptValue::player(player.id));
			}
		}
		return ScriptValue::makeList(players);
	}
	if (method == "isControlPoint") {
		return ScriptValue::integer(m_world.controlPointOf(id) ? 1 : 0);
	}
	if (method == "isPlayerControlObject") {
		return ScriptValue::integer(m_world.playerOf(id) >= 0 || m_world.isVehicle(id) ? 1 : 0);
	}
	if (method == "hasArmor" || method == "getDamage" || method == "getMaxDamage") {
		const float hp = m_world.hitPoints(id);
		if (method == "hasArmor") {
			return ScriptValue::integer(hp >= 0 ? 1 : 0);
		}
		return ScriptValue::number(method == "getDamage" ? hp : (t ? number(*t, "armor.maxHitPoints", 0) : 0));
	}
	if (method == "setDamage" && !args.empty()) {
		m_world.setHitPoints(id, static_cast<float>(args[0].asNumber()));
		return ScriptValue::none();
	}
	if (method == "getIsWreck") {
		return ScriptValue::integer(object->destroyed() ? 1 : 0);
	}
	if (method == "isTitan") {
		return ScriptValue::integer(m_world.isTitan(id) ? 1 : 0); // the Titan sync's 0x00561d90
	}
	if (method == "getIsRemoteControlled") {
		return ScriptValue::integer(0);
	}
	if (method == "cp_getParam" || method == "cp_setParam") {
		ControlPoint* point = m_world.controlPointOf(id);
		if (!point || args.empty()) {
			return ScriptValue::none();
		}
		const ScriptArgs rest(args.begin() + 1, args.end());
		if (method == "cp_getParam") {
			return controlPointParam(*point, args[0].s, rest);
		}
		setControlPointParam(*point, args[0].s, rest);
		return ScriptValue::none();
	}
	return ScriptValue::none();
}

std::vector<int> ScriptBridge::players()
{
	std::vector<int> ids;
	for (const GameWorld::PlayerInfo& player : m_world.playerList()) {
		ids.push_back(player.id);
	}
	return ids;
}

bool ScriptBridge::playerValid(int index)
{
	GameWorld::PlayerInfo info;
	return m_world.playerInfo(index, info);
}

ScriptValue ScriptBridge::playerParam(const std::string& name, int index, const ScriptArgs&)
{
	GameWorld::PlayerInfo player;
	if (!m_world.playerInfo(index, player)) {
		return ScriptValue::none();
	}
	if (name == "team") {
		return ScriptValue::integer(player.team);
	}
	if (name == "alive") {
		return ScriptValue::integer(player.alive ? 1 : 0);
	}
	if (name == "connected") {
		return ScriptValue::integer(1);
	}
	if (name == "ai") {
		return ScriptValue::integer(player.ai ? 1 : 0);
	}
	if (name == "mandown") {
		return ScriptValue::integer(player.manDown ? 1 : 0);
	}
	if (name == "isql") {
		return ScriptValue::integer(player.squadLeader ? 1 : 0);
	}
	if (name == "sqid") {
		return ScriptValue::integer(player.squad);
	}
	if (name == "remote" || name == "autoController" || name == "fholder") {
		return ScriptValue::integer(0);
	}
	if (name == "commander") {
		return ScriptValue::integer(player.commander ? 1 : 0);
	}
	// The squad leader's order (squad slot 0x10, 0x004c2ee0; player slots
	// 0x15c position 0x00593b90, 0x164 radius 0x006766b0, 0x168 accepted
	// 0x00592120, 0x16c accepted and of kind 5 or 6 0x00592160). The
	// radius is the player's own (+0x144: 80, an integer), order or not.
	if (name == "orderPosition" || name == "orderRadius" || name == "ifAcceptedOrder"
		|| name == "ifAcceptedAttackDefendOrder") {
		const GameWorld::SquadOrder* order = player.squad ? m_world.squadLeaderOrder(player.team, player.squad) : nullptr;
		if (name == "orderPosition") {
			return order ? ScriptValue::vec3(order->position.x, order->position.y, order->position.z)
						 : ScriptValue::vec3(0, 0, 0);
		}
		if (name == "orderRadius") {
			return ScriptValue::integer(80);
		}
		const bool attackDefend = order && GameWorld::isAttackOrDefend(order->type);
		return ScriptValue::integer(order && order->accepted && (name == "ifAcceptedOrder" || attackDefend) ? 1 : 0);
	}
	if (name == "name") {
		return ScriptValue::string(player.name);
	}
	if (name == "vehicle") {
		return ScriptValue::object(player.vehicle ? player.vehicle : player.soldier);
	}
	if (name == "defaultvehicle") {
		return ScriptValue::object(player.soldier);
	}
	if (name == "addr") {
		return ScriptValue::string("127.0.0.1");
	}
	if (name == "kit") {
		return ScriptValue::object(player.kit);
	}
	if (name == "weapon") {
		return ScriptValue::object(player.weapon);
	}
	if (name == "allweapons") {
		return player.weapon ? ScriptValue::makeList({ScriptValue::object(player.weapon)}) : ScriptValue::makeList({});
	}
	if (name == "timeToSpawn") {
		return ScriptValue::number(player.respawnIn);
	}
	if (name == "profileid" || name == "ping") {
		return ScriptValue::integer(0);
	}
	const auto& params = m_playerParams[index];
	const auto it = params.find(name);
	return it != params.end() ? it->second : ScriptValue::integer(0);
}

void ScriptBridge::setPlayerParam(const std::string& name, int index, const ScriptValue& value)
{
	if (!playerValid(index)) {
		return;
	}
	if (name == "isInsideCP" && index == m_world.localPlayer()) {
		m_world.playerInsideControlPoint = value.asInt() != 0;
	}
	m_playerParams[index][name] = value;
}

ScriptValue ScriptBridge::playerScore(int index, const std::string& name)
{
	// Rounds by weapon: [(template, count)]; "AndClear" restarts the count.
	if (name.compare(0, 11, "bulletsFire") == 0 || name.compare(0, 19, "bulletsGivingDamage") == 0) {
		const bool hits = name.compare(0, 19, "bulletsGivingDamage") == 0;
		const bool clear = name.size() > 8 && name.compare(name.size() - 8, 8, "AndClear") == 0;
		std::vector<ScriptValue> list;
		for (const auto& [weapon, count] : m_world.playerRounds(index, hits, clear)) {
			list.push_back(ScriptValue::makeList({ScriptValue::string(weapon), ScriptValue::integer(count)}));
		}
		return ScriptValue::makeList(std::move(list));
	}
	GameWorld::PlayerInfo player;
	if (m_world.playerInfo(index, player)) {
		// What the engine counts itself (the scripts add score and the rest).
		if (name == "kills") {
			return ScriptValue::integer(player.kills);
		}
		if (name == "deaths") {
			return ScriptValue::integer(player.deaths);
		}
		if (name == "TKs") {
			return ScriptValue::integer(player.teamKills);
		}
	}
	const auto& scores = m_scores[index];
	const auto it = scores.find(name);
	return it != scores.end() ? it->second : ScriptValue::integer(0);
}

void ScriptBridge::setPlayerScore(int index, const std::string& name, const ScriptValue& value)
{
	m_scores[index][name] = value;
}

} // namespace dice::hfe::game
