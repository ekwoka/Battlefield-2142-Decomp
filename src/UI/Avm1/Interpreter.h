// The ActionScript 1/2 virtual machine (AVM1) with SWF 7 semantics: the
// bytecode in DoAction/DoInitAction tags, clip event handlers and the
// functions they define, plus the built-in classes of Flash Player 7.
//
// SwiffPlayer.dll ran these through gameswf's action code. This is a fresh
// implementation of the format (SWF specification v7 "Actions" chapter):
// SWF 7 is case-sensitive, undefined converts to "undefined"/NaN, and
// strings are UTF-8.
//
// The host drives it: execute() for action blocks, callFunction() and
// callMethod() for events (onEnterFrame, onPress), advance() to run
// setInterval timers. Those entry points never throw; a block with bad
// bytecode, a runaway loop or an uncaught throw is abandoned, reported to
// Host::scriptError, and the entry point returns false.
//
// Inside native functions the other members (call, toNumber, ...) may
// throw ScriptThrow (an ActionScript exception the script can catch) or
// ScriptAbort (fatal for the whole entry), so natives should let them pass.
#pragma once

#include "Host.h"
#include "Object.h"
#include "Value.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace dice::hfe::ui::avm1 {

// Where an action block runs.
struct ExecutionContext {
	// The timeline the actions belong to: unqualified variables live here,
	// frame actions (Play, GotoFrame) act on it. Null means _global.
	ObjectRef target;
	// `this`; null means the target (as for frame scripts and clip events).
	ObjectRef thisObject;
};

// An ActionScript `throw` in flight.
struct ScriptThrow {
	Value value;
};

// The current entry point is abandoned (malformed bytecode, the
// instruction budget or the call depth ran out). try/catch does not see it.
struct ScriptAbort {
	std::string reason;
};

using ActionBytes = std::vector<uint8_t>;
using ConstantPool = std::vector<std::string>;

// A function defined by DefineFunction (0x9b) or DefineFunction2 (0x8e).
class ScriptFunction : public Function {
public:
	struct Parameter {
		uint8_t reg = 0; // DefineFunction2: 0 = a named local instead
		std::string name;
	};

	// DefineFunction2 flags.
	enum : uint16_t {
		kPreloadThis = 0x01,
		kSuppressThis = 0x02,
		kPreloadArguments = 0x04,
		kSuppressArguments = 0x08,
		kPreloadSuper = 0x10,
		kSuppressSuper = 0x20,
		kPreloadRoot = 0x40,
		kPreloadParent = 0x80,
		kPreloadGlobal = 0x100,
	};

	using Function::Function;

	std::string name;
	std::vector<Parameter> parameters;
	bool isFunction2 = false;
	uint8_t registerCount = 0;
	uint16_t flags = 0;

	// The body is code[begin, end), sharing the defining block's bytes and
	// the constant pool in force when it was defined.
	std::shared_ptr<const ActionBytes> code;
	size_t begin = 0;
	size_t end = 0;
	std::shared_ptr<const ConstantPool> constants;

	// The scope chain at the definition (closures) and the timeline.
	std::vector<ObjectRef> scope;
	ObjectRef target;

	Value call(Interpreter& vm, CallArgs& args) override;
	void clearReferences() override;
};

struct Frame; // one activation of the bytecode loop (Execute.cpp)

class Interpreter {
public:
	// `host` must outlive the interpreter; null runs without a stage.
	explicit Interpreter(Host* host = nullptr);
	~Interpreter();
	Interpreter(const Interpreter&) = delete;
	Interpreter& operator=(const Interpreter&) = delete;

	// ===== Entry points for the host (never throw)

	// Runs a DoAction/DoInitAction body or a clip event handler's actions.
	// The bytes are copied, so functions defined here outlive the buffer.
	bool execute(const uint8_t* actions, size_t length, const ExecutionContext& context);
	// Calls a function value. `thisObject` that is not an object means
	// undefined `this`.
	bool callFunction(const Value& function, const Value& thisObject, const std::vector<Value>& args, Value* result = nullptr);
	// object[name](args) if that is a function; true (and nothing run)
	// when it is not.
	bool callMethod(const ObjectRef& object, const std::string& name, const std::vector<Value>& args, Value* result = nullptr);
	// new constructor(args). Null on failure.
	ObjectRef construct(const Value& constructor, const std::vector<Value>& args);
	// Makes an existing object (a movie clip the host just placed) an
	// instance of `constructor` (Object.registerClass): sets __proto__ and
	// __constructor__, then runs the constructor on it.
	bool initializeInstance(const ObjectRef& instance, const Value& constructor, const std::vector<Value>& args);
	// Moves the clock by `milliseconds` and fires due setInterval timers.
	void advance(double milliseconds);

	const std::string& lastError() const { return m_lastError; }
	// Instructions one entry point may run (default 20 million).
	void setInstructionBudget(uint64_t budget) { m_budget = budget; }
	// Nested script calls (default 256, the player's recursion limit).
	void setMaxCallDepth(int depth) { m_maxDepth = depth; }

	Host& host() { return *m_host; }
	const ObjectRef& global() const { return m_global; }
	// Milliseconds advanced so far.
	double clock() const { return m_clock; }

	// ===== Objects

	struct Prototypes {
		ObjectRef object, function, array, string, number, boolean, date, error;
	};
	const Prototypes& prototypes() const { return m_protos; }

	// Creates an object the interpreter tracks so that ~Interpreter can
	// break reference cycles. Use it for anything scripts may see.
	template <typename T, typename... Args>
	std::shared_ptr<T> make(Args&&... args)
	{
		auto object = std::make_shared<T>(std::forward<Args>(args)...);
		track(object);
		return object;
	}
	void track(const ObjectRef& object);

	ObjectRef newObject();
	std::shared_ptr<ArrayObject> newArray(std::vector<Value> elements = {});
	std::shared_ptr<NativeFunction> newFunction(NativeCallback callback);
	// A native constructor with a fresh prototype object (constructor
	// linked both ways, dontEnum).
	std::shared_ptr<NativeFunction> newClass(NativeCallback callback, InstanceFactory factory = {}, ObjectRef prototype = nullptr);
	// Adds a native method (dontEnum, like the player's built-ins).
	void defineMethod(const ObjectRef& object, const std::string& name, NativeCallback callback);

	// ===== Semantics, for natives (may throw ScriptThrow/ScriptAbort)

	enum class Hint { None, Number, String };
	Value toPrimitive(const Value& v, Hint hint = Hint::None);
	double toNumber(const Value& v);
	std::string toString(const Value& v);
	// Primitives become wrapper objects; undefined and null stay null.
	ObjectRef toObject(const Value& v);
	const char* typeOf(const Value& v);
	bool equals(const Value& a, const Value& b); // Equals2
	Value add(const Value& a, const Value& b);   // Add2
	Value less(const Value& a, const Value& b);  // Less2: true, false or undefined (NaN)
	bool instanceOf(const Value& object, const Value& constructor);

	Value getMember(const Value& object, const std::string& name);
	void setMember(const Value& object, const std::string& name, const Value& value);
	// Calls `function` (undefined if it is not one).
	Value call(const Value& function, const ObjectRef& thisObject, std::vector<Value> args, const ObjectRef& superHome = nullptr);
	// CallMethod (0x52): object[name](args) with `this` = object and super
	// bound to where the method was found. Empty name calls the object.
	Value callMember(const Value& object, const std::string& name, std::vector<Value> args);
	// NewObject/NewMethod: undefined if `constructor` is not a function.
	Value constructWith(const Value& constructor, std::vector<Value> args);

	// A movie clip by target path, starting at `from` (see Host).
	ObjectRef resolveTarget(const ObjectRef& from, const std::string& path);
	// _root of `from`.
	ObjectRef rootOf(const ObjectRef& from);

	// setInterval/clearInterval. A non-empty `method` calls
	// thisObject[method], else `function`.
	int addInterval(const Value& function, const ObjectRef& thisObject, const std::string& method, double period, std::vector<Value> args);
	bool clearInterval(int id);

	// Math.random/RandomNumber.
	double random();

	// Runs a script function (ScriptFunction::call).
	Value runFunction(ScriptFunction& function, CallArgs& args);

private:
	enum class Flow { Leave, Return };

	struct Interval {
		int id = 0;
		Value function;
		ObjectRef thisObject;
		std::string method;
		std::vector<Value> args;
		double period = 0;
		double due = 0;
	};

	// The entry-point wrapper: budget, exceptions, error reporting.
	bool guarded(const std::function<void()>& body);
	void fail(const std::string& message);
	[[noreturn]] void abort(const std::string& reason);

	// Execute.cpp: the bytecode loop over code[begin, end) of a frame.
	Flow run(Frame& frame, size_t begin, size_t end);
	Flow runTry(Frame& frame, size_t at, size_t length, size_t& next);
	Flow runWith(Frame& frame, const ObjectRef& object, size_t begin, size_t end);
	std::shared_ptr<ScriptFunction> defineFunction(Frame& frame, size_t at, size_t length, bool function2, size_t& next);
	Value getVariable(Frame& frame, const std::string& name);
	void setVariable(Frame& frame, const std::string& name, const Value& value);
	bool lookupScope(Frame& frame, const std::string& name, Value& out, ObjectRef* holder = nullptr);
	bool specialName(Frame& frame, const std::string& name, Value& out);
	ObjectRef targetOf(Frame& frame, const Value& target);
	void setTarget(Frame& frame, const Value& target);
	void enumerate(Frame& frame, const Value& object);
	void gotoFrameExpression(Frame& frame, const Value& frameValue, bool play, int bias);
	Value superObject(Frame& frame);
	Value callFunctionByName(Frame& frame, const std::string& name, std::vector<Value> args);

	void installBuiltins();

	Host m_noHost;
	Host* m_host;
	ObjectRef m_global;
	Prototypes m_protos;
	std::vector<std::weak_ptr<Object>> m_objects;
	size_t m_pruneAt = 4096;

	uint64_t m_budget = 20'000'000;
	uint64_t m_remaining = 0;
	int m_maxDepth = 256;
	int m_depth = 0;
	int m_entryDepth = 0;
	std::string m_lastError;

	double m_clock = 0;
	std::vector<Interval> m_intervals;
	int m_nextInterval = 1;
	std::mt19937 m_random {0x2142};
};

} // namespace dice::hfe::ui::avm1
