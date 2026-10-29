#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "ScriptHost.h"

#include "EmbeddedScripts.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace dice::hfe::script {

using game::ScriptArgs;
using game::ScriptValue;

namespace {

// An owned reference.
class Ref {
public:
	Ref() = default;
	explicit Ref(PyObject* owned) : m_object(owned) {}
	Ref(const Ref& o) : m_object(o.m_object) { Py_XINCREF(m_object); }
	Ref& operator=(const Ref& o)
	{
		Py_XINCREF(o.m_object);
		Py_XDECREF(m_object);
		m_object = o.m_object;
		return *this;
	}
	~Ref() { Py_XDECREF(m_object); }
	static Ref borrowed(PyObject* object)
	{
		Py_XINCREF(object);
		return Ref(object);
	}
	PyObject* get() const { return m_object; }
	PyObject* release()
	{
		PyObject* o = m_object;
		m_object = nullptr;
		return o;
	}
	explicit operator bool() const { return m_object != nullptr; }

private:
	PyObject* m_object = nullptr;
};

} // namespace

struct ScriptHost::Impl {
	game::ScriptGame& game;
	Log log;
	bool ok = false;
	Ref hostObjects; // the _host_objects module
	std::map<std::string, std::vector<Ref>> handlers;
	std::vector<Ref> statusHandlers;
	std::vector<Ref> timers;
	struct Trigger {
		int id;
		uint32_t object;
		Ref callback;
		std::string filter;
		float radius;
		bool hemisphere;
		Ref data;
		std::set<uint32_t> inside;
	};
	std::vector<Trigger> triggers;
	int nextTrigger = 1;

	Impl(game::ScriptGame& g, Log l) : game(g), log(std::move(l)) {}

	void printError()
	{
		if (PyErr_Occurred()) {
			PyErr_Print(); // sys.stderr goes to host.log once bf2 is imported
		}
	}

	Ref toPython(const ScriptValue& value);
	ScriptValue fromPython(PyObject* object);
	Ref call(PyObject* function, const std::vector<Ref>& args);
};

static ScriptHost::Impl* g_host = nullptr;

Ref ScriptHost::Impl::toPython(const ScriptValue& value)
{
	switch (value.kind) {
	case ScriptValue::Kind::Int:
		return Ref(PyLong_FromLongLong(value.i));
	case ScriptValue::Kind::Float:
		return Ref(PyFloat_FromDouble(value.f));
	case ScriptValue::Kind::String:
		return Ref(PyUnicode_FromStringAndSize(value.s.data(), static_cast<Py_ssize_t>(value.s.size())));
	case ScriptValue::Kind::Vec3:
		return Ref(Py_BuildValue("(fff)", value.v[0], value.v[1], value.v[2]));
	case ScriptValue::Kind::Object:
		if (hostObjects) {
			return Ref(PyObject_CallMethod(hostObjects.get(), "wrap", "L", value.i));
		}
		break;
	case ScriptValue::Kind::Player: {
		// bf2.playerConvFunc gives the cached bf2.PlayerManager.Player.
		Ref bf2(PyImport_ImportModule("bf2"));
		if (bf2) {
			Ref convert(PyObject_GetAttrString(bf2.get(), "playerConvFunc"));
			if (convert) {
				return Ref(PyObject_CallFunction(convert.get(), "L", value.i));
			}
		}
		PyErr_Clear();
		return Ref(PyLong_FromLongLong(value.i));
	}
	case ScriptValue::Kind::List: {
		Ref list(PyList_New(0));
		for (const ScriptValue& item : value.list) {
			Ref element = toPython(item);
			PyList_Append(list.get(), element ? element.get() : Py_None);
		}
		return list;
	}
	case ScriptValue::Kind::None:
		break;
	}
	return Ref::borrowed(Py_None);
}

ScriptValue ScriptHost::Impl::fromPython(PyObject* object)
{
	if (!object || object == Py_None) {
		return ScriptValue::none();
	}
	if (PyBool_Check(object) || PyLong_Check(object)) {
		return ScriptValue::integer(PyLong_AsLongLong(object));
	}
	if (PyFloat_Check(object)) {
		return ScriptValue::number(PyFloat_AsDouble(object));
	}
	if (PyUnicode_Check(object)) {
		const char* text = PyUnicode_AsUTF8(object);
		return ScriptValue::string(text ? text : "");
	}
	if (PyTuple_Check(object) && PyTuple_Size(object) == 3) {
		bool numbers = true;
		float v[3];
		for (Py_ssize_t i = 0; i < 3; ++i) {
			PyObject* item = PyTuple_GetItem(object, i);
			numbers = numbers && PyNumber_Check(item);
			v[i] = numbers ? static_cast<float>(PyFloat_AsDouble(item)) : 0;
		}
		if (numbers) {
			return ScriptValue::vec3(v[0], v[1], v[2]);
		}
	}
	if (PyList_Check(object) || PyTuple_Check(object)) {
		std::vector<ScriptValue> items;
		Ref sequence(PySequence_Fast(object, ""));
		const Py_ssize_t n = PySequence_Fast_GET_SIZE(sequence.get());
		for (Py_ssize_t i = 0; i < n; ++i) {
			items.push_back(fromPython(PySequence_Fast_GET_ITEM(sequence.get(), i)));
		}
		return ScriptValue::makeList(std::move(items));
	}
	if (PyObject_HasAttrString(object, "token")) {
		Ref token(PyObject_GetAttrString(object, "token"));
		return ScriptValue::object(static_cast<uint32_t>(PyLong_AsLongLong(token.get())));
	}
	if (PyObject_HasAttrString(object, "index")) {
		Ref index(PyObject_GetAttrString(object, "index"));
		return ScriptValue::player(static_cast<int>(PyLong_AsLong(index.get())));
	}
	PyErr_Clear();
	return ScriptValue::none();
}

Ref ScriptHost::Impl::call(PyObject* function, const std::vector<Ref>& args)
{
	Ref tuple(PyTuple_New(static_cast<Py_ssize_t>(args.size())));
	for (size_t i = 0; i < args.size(); ++i) {
		PyObject* item = args[i] ? args[i].get() : Py_None;
		Py_INCREF(item);
		PyTuple_SetItem(tuple.get(), static_cast<Py_ssize_t>(i), item);
	}
	Ref result(PyObject_Call(function, tuple.get(), nullptr));
	if (!result) {
		printError();
	}
	return result;
}

// --- the host module ----------------------------------------------------------

namespace {

ScriptArgs argsFrom(PyObject* args, Py_ssize_t first)
{
	ScriptArgs out;
	for (Py_ssize_t i = first; i < PyTuple_Size(args); ++i) {
		out.push_back(g_host->fromPython(PyTuple_GetItem(args, i)));
	}
	return out;
}

std::string stringArg(PyObject* args, Py_ssize_t index)
{
	if (index >= PyTuple_Size(args)) {
		return {};
	}
	PyObject* item = PyTuple_GetItem(args, index);
	if (PyUnicode_Check(item)) {
		const char* text = PyUnicode_AsUTF8(item);
		return text ? text : "";
	}
	Ref text(PyObject_Str(item));
	const char* s = text ? PyUnicode_AsUTF8(text.get()) : nullptr;
	return s ? s : "";
}

int64_t intArg(PyObject* args, Py_ssize_t index, int64_t fallback = 0)
{
	if (index >= PyTuple_Size(args)) {
		return fallback;
	}
	PyObject* item = PyTuple_GetItem(args, index);
	if (PyNumber_Check(item)) {
		Ref number(PyNumber_Long(item));
		if (number) {
			return PyLong_AsLongLong(number.get());
		}
	}
	PyErr_Clear();
	return fallback;
}

PyObject* result(const ScriptValue& value) { return g_host->toPython(value).release(); }

PyObject* none() { Py_RETURN_NONE; }

PyObject* host_log(PyObject*, PyObject* args)
{
	g_host->log(stringArg(args, 0));
	return none();
}

PyObject* host_registerHandler(PyObject*, PyObject* args)
{
	PyObject* function = PyTuple_Size(args) > 1 ? PyTuple_GetItem(args, 1) : nullptr;
	if (function && PyCallable_Check(function)) {
		g_host->handlers[stringArg(args, 0)].push_back(Ref::borrowed(function));
	}
	return none();
}

PyObject* host_unregisterHandler(PyObject*, PyObject* args)
{
	PyObject* function = PyTuple_Size(args) > 1 ? PyTuple_GetItem(args, 1) : nullptr;
	auto& list = g_host->handlers[stringArg(args, 0)];
	list.erase(std::remove_if(list.begin(), list.end(),
				   [function](const Ref& r) { return PyObject_RichCompareBool(r.get(), function, Py_EQ) == 1; }),
		list.end());
	return none();
}

PyObject* host_registerGameStatusHandler(PyObject*, PyObject* args)
{
	PyObject* function = PyTuple_Size(args) > 0 ? PyTuple_GetItem(args, 0) : nullptr;
	if (function && PyCallable_Check(function)) {
		g_host->statusHandlers.push_back(Ref::borrowed(function));
	}
	return none();
}

PyObject* host_unregisterGameStatusHandler(PyObject*, PyObject* args)
{
	PyObject* function = PyTuple_Size(args) > 0 ? PyTuple_GetItem(args, 0) : nullptr;
	auto& list = g_host->statusHandlers;
	list.erase(std::remove_if(list.begin(), list.end(),
				   [function](const Ref& r) { return PyObject_RichCompareBool(r.get(), function, Py_EQ) == 1; }),
		list.end());
	return none();
}

PyObject* host_timer_getWallTime(PyObject*, PyObject*) { return PyFloat_FromDouble(g_host->game.wallTime()); }

PyObject* host_timer_created(PyObject*, PyObject* args)
{
	if (PyTuple_Size(args) > 0) {
		g_host->timers.push_back(Ref::borrowed(PyTuple_GetItem(args, 0)));
	}
	return none();
}

PyObject* host_timer_destroy(PyObject*, PyObject* args)
{
	PyObject* timer = PyTuple_Size(args) > 0 ? PyTuple_GetItem(args, 0) : nullptr;
	auto& list = g_host->timers;
	list.erase(std::remove_if(list.begin(), list.end(), [timer](const Ref& r) { return r.get() == timer; }), list.end());
	return none();
}

PyObject* host_sgl_getParam(PyObject*, PyObject* args)
{
	return result(g_host->game.gameParam(stringArg(args, 0), static_cast<int>(intArg(args, 1)),
		static_cast<int>(intArg(args, 2))));
}

PyObject* host_sgl_setParam(PyObject*, PyObject* args)
{
	const ScriptValue value = PyTuple_Size(args) > 2 ? g_host->fromPython(PyTuple_GetItem(args, 2)) : ScriptValue::none();
	g_host->game.setGameParam(stringArg(args, 0), static_cast<int>(intArg(args, 1)), value, static_cast<int>(intArg(args, 3)));
	return none();
}

PyObject* host_ss_getParam(PyObject*, PyObject* args) { return result(g_host->game.serverSetting(stringArg(args, 0))); }

PyObject* host_sgl_getIsAIGame(PyObject*, PyObject*) { return PyLong_FromLong(g_host->game.isAIGame() ? 1 : 0); }
PyObject* host_sgl_getMapName(PyObject*, PyObject*) { return PyUnicode_FromString(g_host->game.mapName().c_str()); }
PyObject* host_sgl_getModDirectory(PyObject*, PyObject*) { return PyUnicode_FromString(g_host->game.modDirectory().c_str()); }
PyObject* host_sgl_getWorldSize(PyObject*, PyObject*)
{
	const int size = g_host->game.worldSize();
	return Py_BuildValue("(ii)", size, size);
}

PyObject* host_sgl_endGame(PyObject*, PyObject* args)
{
	g_host->game.endGame(static_cast<int>(intArg(args, 0)), static_cast<int>(intArg(args, 1)));
	return none();
}

PyObject* host_sgl_sendTextMessage(PyObject*, PyObject* args)
{
	g_host->game.textMessage(static_cast<int>(intArg(args, 0)), stringArg(args, 3));
	return none();
}

PyObject* gameEvent(const char* kind, PyObject* args)
{
	g_host->game.gameEvent(kind, argsFrom(args, 0));
	return none();
}

PyObject* host_sgl_sendGameLogicEvent(PyObject*, PyObject* args) { return gameEvent("gameLogicEvent", args); }
PyObject* host_sgl_sendHudEvent(PyObject*, PyObject* args) { return gameEvent("hudEvent", args); }
PyObject* host_sgl_sendMedalEvent(PyObject*, PyObject* args) { return gameEvent("medalEvent", args); }
PyObject* host_sgl_sendRankEvent(PyObject*, PyObject* args) { return gameEvent("rankEvent", args); }
PyObject* host_sgl_sendPythonEvent(PyObject*, PyObject* args) { return gameEvent("pythonEvent", args); }

PyObject* objectList(const std::vector<uint32_t>& ids)
{
	std::vector<ScriptValue> items;
	for (uint32_t id : ids) {
		items.push_back(ScriptValue::object(id));
	}
	return result(ScriptValue::makeList(std::move(items)));
}

PyObject* host_omgr_getObjectsOfType(PyObject*, PyObject* args)
{
	return objectList(g_host->game.objectsOfType(stringArg(args, 0)));
}

PyObject* host_omgr_getObjectsOfTemplate(PyObject*, PyObject* args)
{
	return objectList(g_host->game.objectsOfTemplate(stringArg(args, 0)));
}

PyObject* host_obj_call(PyObject*, PyObject* args)
{
	return result(g_host->game.objectCall(static_cast<uint32_t>(intArg(args, 0)), stringArg(args, 1), argsFrom(args, 2)));
}

PyObject* host_trig_create(PyObject*, PyObject* args)
{
	if (PyTuple_Size(args) < 5) {
		PyErr_SetString(PyExc_TypeError, "trig_create needs object, callback, filter, radius, hemisphere");
		return nullptr;
	}
	ScriptHost::Impl::Trigger trigger;
	trigger.id = g_host->nextTrigger++;
	trigger.object = static_cast<uint32_t>(g_host->fromPython(PyTuple_GetItem(args, 0)).i);
	trigger.callback = Ref::borrowed(PyTuple_GetItem(args, 1));
	trigger.filter = stringArg(args, 2);
	trigger.radius = static_cast<float>(PyFloat_AsDouble(PyTuple_GetItem(args, 3)));
	trigger.hemisphere = intArg(args, 4) != 0;
	trigger.data = Ref::borrowed(PyTuple_Size(args) > 5 ? PyTuple_GetItem(args, 5) : Py_None);
	g_host->triggers.push_back(trigger);
	return PyLong_FromLong(trigger.id);
}

PyObject* host_trig_destroy(PyObject*, PyObject* args)
{
	const int id = static_cast<int>(intArg(args, 0));
	auto& list = g_host->triggers;
	list.erase(std::remove_if(list.begin(), list.end(), [id](const auto& t) { return t.id == id; }), list.end());
	return none();
}

PyObject* host_trig_destroyAll(PyObject*, PyObject*)
{
	g_host->triggers.clear();
	return none();
}

PyObject* host_trig_getObjects(PyObject*, PyObject* args)
{
	const int id = static_cast<int>(intArg(args, 0));
	for (const auto& t : g_host->triggers) {
		if (t.id == id) {
			return objectList(std::vector<uint32_t>(t.inside.begin(), t.inside.end()));
		}
	}
	return PyList_New(0);
}

PyObject* host_pmgr_p_get(PyObject*, PyObject* args)
{
	return result(g_host->game.playerParam(stringArg(args, 0), static_cast<int>(intArg(args, 1)), argsFrom(args, 2)));
}

PyObject* host_pmgr_p_set(PyObject*, PyObject* args)
{
	const ScriptValue value = PyTuple_Size(args) > 2 ? g_host->fromPython(PyTuple_GetItem(args, 2)) : ScriptValue::none();
	g_host->game.setPlayerParam(stringArg(args, 0), static_cast<int>(intArg(args, 1)), value);
	return none();
}

PyObject* host_pmgr_isIndexValid(PyObject*, PyObject* args)
{
	return PyLong_FromLong(g_host->game.playerValid(static_cast<int>(intArg(args, 0, -1))) ? 1 : 0);
}

PyObject* host_pmgr_getPlayers(PyObject*, PyObject*)
{
	Ref list(PyList_New(0));
	for (int index : g_host->game.players()) {
		Ref i(PyLong_FromLong(index));
		PyList_Append(list.get(), i.get());
	}
	return list.release();
}

PyObject* host_pmgr_getNumberOfPlayers(PyObject*, PyObject*)
{
	return PyLong_FromSize_t(g_host->game.players().size());
}

PyObject* host_pmgr_getScore(PyObject*, PyObject* args)
{
	return result(g_host->game.playerScore(static_cast<int>(intArg(args, 0)), stringArg(args, 1)));
}

PyObject* host_pmgr_setScore(PyObject*, PyObject* args)
{
	const ScriptValue value = PyTuple_Size(args) > 2 ? g_host->fromPython(PyTuple_GetItem(args, 2)) : ScriptValue::none();
	g_host->game.setPlayerScore(static_cast<int>(intArg(args, 0)), stringArg(args, 1), value);
	return none();
}

PyObject* host_pmgr_getCommander(PyObject*, PyObject* args)
{
	return PyLong_FromLong(g_host->game.commander(static_cast<int>(intArg(args, 0))));
}

PyObject* host_sh_setEnableCommander(PyObject*, PyObject* args)
{
	g_host->game.setCommanderEnabled(intArg(args, 0) != 0);
	return none();
}

PyObject* host_sgl_getControlPointsInGroup(PyObject*, PyObject* args)
{
	// 0x00568890: a tuple of the group's control points.
	std::vector<ScriptValue> items;
	for (uint32_t id : g_host->game.controlPointsInGroup(static_cast<int>(intArg(args, 0)))) {
		items.push_back(ScriptValue::object(id));
	}
	Ref list(result(ScriptValue::makeList(std::move(items))));
	return list ? PyList_AsTuple(list.get()) : nullptr;
}

PyObject* host_pmgr_getSquadLeader(PyObject*, PyObject* args)
{
	return PyLong_FromLong(g_host->game.squadLeader(static_cast<int>(intArg(args, 0)), static_cast<int>(intArg(args, 1))));
}

PyObject* host_pmgr_getPlayersInSquad(PyObject*, PyObject* args)
{
	// 0x0056adb0: a tuple of player indices.
	const std::vector<int> ids =
		g_host->game.playersInSquad(static_cast<int>(intArg(args, 0)), static_cast<int>(intArg(args, 1)));
	Ref tuple(PyTuple_New(static_cast<Py_ssize_t>(ids.size())));
	for (size_t i = 0; i < ids.size(); ++i) {
		PyTuple_SetItem(tuple.get(), static_cast<Py_ssize_t>(i), PyLong_FromLong(ids[i]));
	}
	return tuple.release();
}

PyObject* host_pmgr_enableScoreEvents(PyObject*, PyObject* args)
{
	g_host->game.setScoreEvents(intArg(args, 0) > 0);
	return none();
}

PyObject* host_pers_plrGiveFieldUnlock(PyObject*, PyObject* args)
{
	if (!g_host->game.giveFieldUnlock(static_cast<int>(intArg(args, 0, -1)))) {
		PyErr_SetString(PyExc_RuntimeError, "no such player");
		return nullptr;
	}
	return none();
}

// pers_plrRequestUnlocks (0x00567370) queues a request to the stats
// server and returns whether it was sent; there is none to ask for unlocks
// (Stats/Stats.h keeps the standing on this machine and the menu reads it
// there), so the request fails: 0, as with the server down.
PyObject* host_pers_request(PyObject*, PyObject*) { return PyLong_FromLong(0); }

// pers_plrRequestStats(player, ?, query, ?) (0x00566930): a player index
// below 0x100 stands for its profile id (slot 0x324); 1 when the request
// went out. The game answers from the local record (ScriptGame::requestStats).
PyObject* host_pers_plrRequestStats(PyObject*, PyObject* args)
{
	return PyLong_FromLong(g_host->game.requestStats(intArg(args, 0, -1)) ? 1 : 0);
}

PyObject* host_pers_getStatsKeyVal(PyObject*, PyObject* args)
{
	// 0x00566b90: the value as a string from the downloaded record.
	std::string value;
	const int64_t profile = intArg(args, 1);
	if (!g_host->game.statsKeyValue(stringArg(args, 0), profile, value)) {
		PyErr_SetString(PyExc_RuntimeError, "there is no current persistence object downloaded");
		return nullptr;
	}
	return PyUnicode_FromStringAndSize(value.data(), static_cast<Py_ssize_t>(value.size()));
}

PyObject* host_gl_sendEndOfRoundData(PyObject*, PyObject* args)
{
	g_host->game.endOfRoundData(stringArg(args, 0));
	return none();
}

PyObject* host_rcon_invoke(PyObject*, PyObject* args)
{
	const std::string output = g_host->game.rconInvoke(stringArg(args, 0));
	return PyUnicode_FromStringAndSize(output.data(), static_cast<Py_ssize_t>(output.size()));
}

PyObject* host_rcon_feedback(PyObject*, PyObject* args)
{
	g_host->game.rconFeedback(static_cast<int>(intArg(args, 0, -1)), stringArg(args, 1));
	return none();
}

PyObject* host_sgl_getOverlayDirectory(PyObject*, PyObject*)
{
	return PyUnicode_FromString(g_host->game.overlayDirectory().c_str());
}

PyObject* host_nothing(PyObject*, PyObject*) { return none(); }

PyMethodDef g_methods[] = {
	{"log", host_log, METH_VARARGS, nullptr},
	{"registerHandler", host_registerHandler, METH_VARARGS, nullptr},
	{"unregisterHandler", host_unregisterHandler, METH_VARARGS, nullptr},
	{"registerGameStatusHandler", host_registerGameStatusHandler, METH_VARARGS, nullptr},
	{"unregisterGameStatusHandler", host_unregisterGameStatusHandler, METH_VARARGS, nullptr},
	{"timer_getWallTime", host_timer_getWallTime, METH_VARARGS, nullptr},
	{"timer_created", host_timer_created, METH_VARARGS, nullptr},
	{"timer_destroy", host_timer_destroy, METH_VARARGS, nullptr},
	{"sgl_getParam", host_sgl_getParam, METH_VARARGS, nullptr},
	{"sgl_setParam", host_sgl_setParam, METH_VARARGS, nullptr},
	{"sgl_getIsAIGame", host_sgl_getIsAIGame, METH_VARARGS, nullptr},
	{"sgl_getMapName", host_sgl_getMapName, METH_VARARGS, nullptr},
	{"sgl_getModDirectory", host_sgl_getModDirectory, METH_VARARGS, nullptr},
	{"sgl_getOverlayDirectory", host_sgl_getOverlayDirectory, METH_VARARGS, nullptr},
	{"sgl_getWorldSize", host_sgl_getWorldSize, METH_VARARGS, nullptr},
	{"sgl_getControlPointsInGroup", host_sgl_getControlPointsInGroup, METH_VARARGS, nullptr},
	{"sgl_endGame", host_sgl_endGame, METH_VARARGS, nullptr},
	{"sgl_sendTextMessage", host_sgl_sendTextMessage, METH_VARARGS, nullptr},
	{"sgl_sendGameLogicEvent", host_sgl_sendGameLogicEvent, METH_VARARGS, nullptr},
	{"sgl_sendHudEvent", host_sgl_sendHudEvent, METH_VARARGS, nullptr},
	{"sgl_sendMedalEvent", host_sgl_sendMedalEvent, METH_VARARGS, nullptr},
	{"sgl_sendRankEvent", host_sgl_sendRankEvent, METH_VARARGS, nullptr},
	{"sgl_sendPythonEvent", host_sgl_sendPythonEvent, METH_VARARGS, nullptr},
	{"ss_getParam", host_ss_getParam, METH_VARARGS, nullptr},
	{"sh_setEnableCommander", host_sh_setEnableCommander, METH_VARARGS, nullptr},
	{"omgr_getObjectsOfType", host_omgr_getObjectsOfType, METH_VARARGS, nullptr},
	{"omgr_getObjectsOfTemplate", host_omgr_getObjectsOfTemplate, METH_VARARGS, nullptr},
	{"obj_call", host_obj_call, METH_VARARGS, nullptr},
	{"trig_create", host_trig_create, METH_VARARGS, nullptr},
	{"trig_destroy", host_trig_destroy, METH_VARARGS, nullptr},
	{"trig_destroyAll", host_trig_destroyAll, METH_VARARGS, nullptr},
	{"trig_getObjects", host_trig_getObjects, METH_VARARGS, nullptr},
	{"pmgr_p_get", host_pmgr_p_get, METH_VARARGS, nullptr},
	{"pmgr_p_set", host_pmgr_p_set, METH_VARARGS, nullptr},
	{"pmgr_isIndexValid", host_pmgr_isIndexValid, METH_VARARGS, nullptr},
	{"pmgr_getPlayers", host_pmgr_getPlayers, METH_VARARGS, nullptr},
	{"pmgr_getNumberOfPlayers", host_pmgr_getNumberOfPlayers, METH_VARARGS, nullptr},
	{"pmgr_getScore", host_pmgr_getScore, METH_VARARGS, nullptr},
	{"pmgr_setScore", host_pmgr_setScore, METH_VARARGS, nullptr},
	{"pmgr_enableScoreEvents", host_pmgr_enableScoreEvents, METH_VARARGS, nullptr},
	{"pmgr_getCommander", host_pmgr_getCommander, METH_VARARGS, nullptr},
	{"pmgr_getSquadLeader", host_pmgr_getSquadLeader, METH_VARARGS, nullptr},
	{"pmgr_getPlayersInSquad", host_pmgr_getPlayersInSquad, METH_VARARGS, nullptr},
	{"rcon_invoke", host_rcon_invoke, METH_VARARGS, nullptr},
	{"rcon_feedback", host_rcon_feedback, METH_VARARGS, nullptr},
	{"pers_getStatsKeyVal", host_pers_getStatsKeyVal, METH_VARARGS, nullptr},
	{"pers_plrGiveFieldUnlock", host_pers_plrGiveFieldUnlock, METH_VARARGS, nullptr},
	{"pers_plrRequestUnlocks", host_pers_request, METH_VARARGS, nullptr},
	{"pers_plrRequestStats", host_pers_plrRequestStats, METH_VARARGS, nullptr},
	{"gs_abortCurrentConnectionAttempt", host_nothing, METH_VARARGS, nullptr},
	{"gl_sendEndOfRoundData", host_gl_sendEndOfRoundData, METH_VARARGS, nullptr},
	{nullptr, nullptr, 0, nullptr},
};

PyModuleDef g_module = {PyModuleDef_HEAD_INIT, "host", nullptr, -1, g_methods, nullptr, nullptr, nullptr, nullptr};

PyObject* initHost() { return PyModule_Create(&g_module); }

// Runs embedded source as a module of that name.
Ref loadModule(const char* name, const char* source)
{
	Ref module(PyImport_AddModule(name)); // borrowed
	Py_XINCREF(module.get());
	if (!module) {
		return {};
	}
	PyObject* globals = PyModule_GetDict(module.get());
	PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
	Ref code(Py_CompileString(source, name, Py_file_input));
	if (!code) {
		return {};
	}
	Ref done(PyEval_EvalCode(code.get(), globals, globals));
	return done ? module : Ref {};
}

} // namespace

ScriptHost::ScriptHost(game::ScriptGame& game, std::vector<std::string> paths, Log log)
	: m_impl(std::make_unique<Impl>(game, std::move(log)))
{
	g_host = m_impl.get();
	if (!Py_IsInitialized()) {
		PyImport_AppendInittab("host", initHost);
		PyConfig config;
		PyConfig_InitIsolatedConfig(&config);
		const PyStatus status = Py_InitializeFromConfig(&config);
		PyConfig_Clear(&config);
		if (PyStatus_Exception(status)) {
			m_impl->log("python: cannot start the interpreter");
			return;
		}
	}
	Ref compat = loadModule("py2compat", kPy2Compat);
	m_impl->hostObjects = loadModule("_host_objects", kHostObjects);
	if (!compat || !m_impl->hostObjects) {
		m_impl->printError();
		return;
	}
	Ref list(PyList_New(0));
	for (const std::string& path : paths) {
		Ref item(PyUnicode_FromString(path.c_str()));
		PyList_Append(list.get(), item.get());
	}
	Ref installed(PyObject_CallMethod(compat.get(), "install", "O", list.get()));
	if (!installed) {
		m_impl->printError();
		return;
	}
	m_impl->ok = true;
}

ScriptHost::~ScriptHost()
{
	// The interpreter stays up (CPython does not reinitialise cleanly);
	// drop what the scripts registered.
	if (m_impl && Py_IsInitialized()) {
		m_impl->handlers.clear();
		m_impl->statusHandlers.clear();
		m_impl->timers.clear();
		m_impl->triggers.clear();
		m_impl->hostObjects = Ref();
	}
	if (g_host == m_impl.get()) {
		g_host = nullptr;
	}
}

bool ScriptHost::ok() const { return m_impl->ok; }

bool ScriptHost::run(const std::string& source)
{
	if (!m_impl->ok) {
		return false;
	}
	g_host = m_impl.get();
	Ref main = Ref::borrowed(PyImport_AddModule("__main__"));
	PyObject* globals = PyModule_GetDict(main.get());
	Ref done(PyRun_String(source.c_str(), Py_file_input, globals, globals));
	if (!done) {
		m_impl->printError();
		return false;
	}
	return true;
}

bool ScriptHost::startGameMode(const std::string& mode)
{
	if (!m_impl->ok) {
		return false;
	}
	g_host = m_impl.get();
	Ref bf2(PyImport_ImportModule("bf2"));
	if (!bf2) {
		m_impl->printError();
		return false;
	}
	Ref init(PyObject_CallMethod(bf2.get(), "init_module", nullptr));
	if (!init) {
		m_impl->printError();
	}
	const std::string name = "game.gamemodes." + mode;
	Ref gameMode(PyImport_ImportModule(name.c_str()));
	if (!gameMode) {
		m_impl->printError();
		return false;
	}
	Ref started(PyObject_CallMethod(gameMode.get(), "init", nullptr));
	if (!started) {
		m_impl->printError();
		return false;
	}
	return true;
}

void ScriptHost::setGameStatus(game::GameStatus status)
{
	if (!m_impl->ok) {
		return;
	}
	g_host = m_impl.get();
	const std::vector<Ref> handlers = m_impl->statusHandlers; // handlers may unregister
	for (const Ref& handler : handlers) {
		m_impl->call(handler.get(), {Ref(PyLong_FromLong(static_cast<long>(status)))});
	}
}

void ScriptHost::raise(const std::string& event, const ScriptArgs& args)
{
	if (!m_impl->ok) {
		return;
	}
	g_host = m_impl.get();
	const auto it = m_impl->handlers.find(event);
	if (it == m_impl->handlers.end()) {
		return;
	}
	std::vector<Ref> pythonArgs;
	for (const ScriptValue& a : args) {
		pythonArgs.push_back(m_impl->toPython(a));
	}
	const std::vector<Ref> handlers = it->second;
	for (const Ref& handler : handlers) {
		m_impl->call(handler.get(), pythonArgs);
	}
}

void ScriptHost::update()
{
	if (!m_impl->ok) {
		return;
	}
	g_host = m_impl.get();
	Impl& impl = *m_impl;
	// Timers (bf2.Timer): due ones fire; recurring ones move on.
	const double now = impl.game.wallTime();
	const std::vector<Ref> timers = impl.timers;
	for (const Ref& timer : timers) {
		Ref time(PyObject_GetAttrString(timer.get(), "time"));
		if (!time) {
			PyErr_Clear();
			continue;
		}
		const double at = PyFloat_AsDouble(time.get());
		if (now < at) {
			continue;
		}
		Ref fired(PyObject_CallMethod(timer.get(), "onTrigger", nullptr));
		if (!fired) {
			impl.printError();
		}
		Ref interval(PyObject_GetAttrString(timer.get(), "interval"));
		const double every = interval ? PyFloat_AsDouble(interval.get()) : 0;
		PyErr_Clear();
		if (every > 0) {
			Ref next(PyFloat_FromDouble(at + every));
			PyObject_SetAttrString(timer.get(), "time", next.get());
		} else {
			impl.timers.erase(std::remove_if(impl.timers.begin(), impl.timers.end(),
								  [&timer](const Ref& r) { return r.get() == timer.get(); }),
				impl.timers.end());
		}
	}
	// Triggers: who is inside each sphere now.
	for (size_t i = 0; i < impl.triggers.size(); ++i) {
		float centre[3];
		if (!impl.game.objectPosition(impl.triggers[i].object, centre)) {
			continue;
		}
		std::set<uint32_t> now;
		for (uint32_t candidate : impl.game.triggerCandidates(impl.triggers[i].filter)) {
			float p[3];
			if (candidate == impl.triggers[i].object || !impl.game.objectPosition(candidate, p)) {
				continue;
			}
			const float dx = p[0] - centre[0], dy = p[1] - centre[1], dz = p[2] - centre[2];
			const float r = impl.triggers[i].radius;
			const bool within = dx * dx + dy * dy + dz * dz <= r * r && (!impl.triggers[i].hemisphere || dy >= 0);
			if (within) {
				now.insert(candidate);
			}
		}
		const std::set<uint32_t> before = impl.triggers[i].inside;
		impl.triggers[i].inside = now;
		const int id = impl.triggers[i].id;
		const uint32_t owner = impl.triggers[i].object;
		const Ref callback = impl.triggers[i].callback;
		const Ref data = impl.triggers[i].data;
		const auto notify = [&](uint32_t object, bool entered) {
			impl.call(callback.get(), {Ref(PyLong_FromLong(id)), impl.toPython(ScriptValue::object(owner)),
										  impl.toPython(ScriptValue::object(object)), Ref(PyLong_FromLong(entered ? 1 : 0)), data});
		};
		for (uint32_t object : now) {
			if (!before.count(object)) {
				notify(object, true);
			}
		}
		for (uint32_t object : before) {
			if (!now.count(object)) {
				notify(object, false);
			}
		}
	}
}

} // namespace dice::hfe::script
