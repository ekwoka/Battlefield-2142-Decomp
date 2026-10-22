// What the game's Python scripts see of the engine: the functions of the
// "host" module (BF2142.exe registers it in the Python 2.3 interpreter of
// dice_py.dll) answered by the game, without Python types. The script host
// (src/Python) converts between these values and Python objects.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dice::hfe::game {

// A value passed between the game and its scripts.
struct ScriptValue {
	enum class Kind { None, Int, Float, String, Vec3, Object, Player, List };
	Kind kind = Kind::None;
	int64_t i = 0;  // Int; Object (its id); Player (its index)
	double f = 0;   // Float
	std::string s;  // String
	float v[3] = {}; // Vec3
	std::vector<ScriptValue> list;

	static ScriptValue none() { return {}; }
	static ScriptValue integer(int64_t value)
	{
		ScriptValue r;
		r.kind = Kind::Int;
		r.i = value;
		return r;
	}
	static ScriptValue number(double value)
	{
		ScriptValue r;
		r.kind = Kind::Float;
		r.f = value;
		return r;
	}
	static ScriptValue string(std::string value)
	{
		ScriptValue r;
		r.kind = Kind::String;
		r.s = std::move(value);
		return r;
	}
	static ScriptValue vec3(float x, float y, float z)
	{
		ScriptValue r;
		r.kind = Kind::Vec3;
		r.v[0] = x;
		r.v[1] = y;
		r.v[2] = z;
		return r;
	}
	static ScriptValue object(uint32_t id)
	{
		ScriptValue r;
		r.kind = id ? Kind::Object : Kind::None;
		r.i = id;
		return r;
	}
	static ScriptValue player(int index)
	{
		ScriptValue r;
		r.kind = index >= 0 ? Kind::Player : Kind::None;
		r.i = index;
		return r;
	}
	static ScriptValue makeList(std::vector<ScriptValue> items)
	{
		ScriptValue r;
		r.kind = Kind::List;
		r.list = std::move(items);
		return r;
	}

	double asNumber() const { return kind == Kind::Float ? f : kind == Kind::Int ? static_cast<double>(i) : 0.0; }
	int64_t asInt() const { return kind == Kind::Int ? i : kind == Kind::Float ? static_cast<int64_t>(f) : 0; }
};

using ScriptArgs = std::vector<ScriptValue>;

// Events the game raises for its scripts ("PlayerSpawn", "ControlPointChangedOwner"...).
using ScriptEventSink = std::function<void(const std::string& event, const ScriptArgs& args)>;

// Game status values (bf2.GameStatus).
enum class GameStatus { Playing = 1, EndGame = 2, PreGame = 3, Paused = 4, RestartServer = 5, NotConnected = 6 };

class ScriptGame {
public:
	virtual ~ScriptGame() = default;

	virtual double wallTime() const = 0; // seconds since the round started

	// sgl_*: game logic.
	virtual ScriptValue gameParam(const std::string& name, int team, int id) = 0;
	virtual void setGameParam(const std::string& name, int team, const ScriptValue& value, int id) = 0;
	virtual ScriptValue serverSetting(const std::string& name) = 0;
	virtual std::string mapName() = 0;
	virtual std::string modDirectory() = 0;
	virtual int worldSize() = 0;
	virtual bool isAIGame() = 0;
	virtual void endGame(int winner, int victoryType) = 0;
	virtual void textMessage(int player, const std::string& text) = 0;
	virtual void gameEvent(const std::string& kind, const ScriptArgs& args) = 0; // sendGameEvent, sendHudEvent...

	// omgr_* and the object type's methods.
	virtual std::vector<uint32_t> objectsOfType(const std::string& type) = 0;
	virtual std::vector<uint32_t> objectsOfTemplate(const std::string& name) = 0;
	virtual ScriptValue objectCall(uint32_t object, const std::string& method, const ScriptArgs& args) = 0;
	// The objects a trigger with this filter ("<<PCO>>": player control
	// objects) looks for.
	virtual std::vector<uint32_t> triggerCandidates(const std::string& filter) = 0;
	virtual bool objectPosition(uint32_t object, float position[3]) = 0;

	// pmgr_*: players.
	virtual std::vector<int> players() = 0;
	virtual bool playerValid(int index) = 0;
	virtual ScriptValue playerParam(const std::string& name, int index, const ScriptArgs& extra) = 0;
	virtual void setPlayerParam(const std::string& name, int index, const ScriptValue& value) = 0;
	virtual ScriptValue playerScore(int index, const std::string& name) = 0;
	virtual void setPlayerScore(int index, const std::string& name, const ScriptValue& value) = 0;
	// pmgr_getCommander (0x0056a530: the SquadManager's slot 0x78) and
	// sh_setEnableCommander (0x00568640: event 0x47).
	virtual int commander(int team) { (void)team; return -1; }
	virtual void setCommanderEnabled(bool enabled) { (void)enabled; }
};

} // namespace dice::hfe::game
