// The interpreter's object model glue: entry points, conversions, calls,
// construction, target paths and interval timers. The bytecode loop is in
// Execute.cpp, the built-in classes in Builtins*.cpp.
#include "Interpreter.h"

#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace dice::hfe::ui::avm1 {

namespace {

constexpr int kMaxChain = 256;

// Script calls may also use no more than this much of the machine stack
// (from the outermost call): frames are bigger in debug and sanitizer
// builds, and threads may have small stacks.
constexpr std::ptrdiff_t kStackBudget = 512 * 1024;
thread_local const char* g_stackBase = nullptr;

// Counts nested calls (and watches the stack they use); restores the
// count however the call ends.
class DepthGuard {
public:
	DepthGuard(int& depth, int limit) : m_depth(depth)
	{
		const char here = 0;
		if (m_depth == 0) {
			g_stackBase = &here;
		}
		const std::ptrdiff_t used = g_stackBase ? std::abs(g_stackBase - &here) : 0;
		if (++m_depth > limit || used > kStackBudget) {
			--m_depth;
			throw ScriptAbort {"call depth exceeds " + std::to_string(used > kStackBudget ? m_depth : limit)};
		}
	}
	~DepthGuard() { --m_depth; }
	DepthGuard(const DepthGuard&) = delete;
	DepthGuard& operator=(const DepthGuard&) = delete;

private:
	int& m_depth;
};

} // namespace

const char* clipPropertyName(int index)
{
	static const char* const names[kClipPropertyCount] = {"_x", "_y", "_xscale", "_yscale", "_currentframe", "_totalframes", "_alpha",
		"_visible", "_width", "_height", "_rotation", "_target", "_framesloaded", "_name", "_droptarget", "_url", "_highquality",
		"_focusrect", "_soundbuftime", "_quality", "_xmouse", "_ymouse"};
	return index >= 0 && index < kClipPropertyCount ? names[index] : nullptr;
}

// --- ScriptFunction

Value ScriptFunction::call(Interpreter& vm, CallArgs& args) { return vm.runFunction(*this, args); }

void ScriptFunction::clearReferences()
{
	scope.clear();
	target.reset();
	Function::clearReferences();
}

// --- Lifetime

Interpreter::Interpreter(Host* host) : m_host(host ? host : &m_noHost)
{
	m_protos.object = make<Object>();
	m_protos.function = make<Object>(m_protos.object);
	m_global = make<Object>(m_protos.object);
	installBuiltins();
}

Interpreter::~Interpreter()
{
	// Prototypes, constructors and closures point at each other; clearing
	// every object we made lets them go.
	for (auto& weak : m_objects) {
		if (const ObjectRef object = weak.lock()) {
			object->clearReferences();
		}
	}
	m_intervals.clear();
}

void Interpreter::track(const ObjectRef& object)
{
	if (m_objects.size() >= m_pruneAt) {
		m_objects.erase(std::remove_if(m_objects.begin(), m_objects.end(), [](const std::weak_ptr<Object>& w) { return w.expired(); }),
			m_objects.end());
		m_pruneAt = std::max<size_t>(4096, m_objects.size() * 2);
	}
	m_objects.push_back(object);
}

ObjectRef Interpreter::newObject() { return make<Object>(m_protos.object); }

std::shared_ptr<ArrayObject> Interpreter::newArray(std::vector<Value> elements)
{
	auto array = make<ArrayObject>(m_protos.array);
	array->elements = std::move(elements);
	return array;
}

std::shared_ptr<NativeFunction> Interpreter::newFunction(NativeCallback callback)
{
	return make<NativeFunction>(m_protos.function, std::move(callback));
}

std::shared_ptr<NativeFunction> Interpreter::newClass(NativeCallback callback, InstanceFactory factory, ObjectRef prototype)
{
	auto ctor = make<NativeFunction>(m_protos.function, std::move(callback), std::move(factory));
	if (!prototype) {
		prototype = newObject();
	}
	ctor->define("prototype", prototype, kDontEnum | kDontDelete);
	prototype->define("constructor", ctor, kDontEnum);
	return ctor;
}

void Interpreter::defineMethod(const ObjectRef& object, const std::string& name, NativeCallback callback)
{
	object->define(name, newFunction(std::move(callback)), kDontEnum);
}

// --- Entry points

void Interpreter::fail(const std::string& message)
{
	m_lastError = message;
	m_host->scriptError(message);
}

void Interpreter::abort(const std::string& reason) { throw ScriptAbort {reason}; }

bool Interpreter::guarded(const std::function<void()>& body)
{
	if (m_entryDepth == 0) {
		m_remaining = m_budget;
		m_lastError.clear();
	}
	++m_entryDepth;
	bool ok = false;
	try {
		body();
		ok = true;
	} catch (const ScriptThrow& e) {
		std::string what;
		try {
			what = toString(e.value);
		} catch (...) {
			what = typeOf(e.value);
		}
		fail("uncaught exception: " + what);
	} catch (const ScriptAbort& e) {
		fail(e.reason);
	} catch (const std::bad_alloc&) {
		fail("out of memory");
	} catch (const std::length_error&) {
		fail("out of memory");
	}
	--m_entryDepth;
	return ok;
}

bool Interpreter::callFunction(const Value& function, const Value& thisObject, const std::vector<Value>& args, Value* result)
{
	return guarded([&] {
		const Value r = call(function, thisObject.object(), args);
		if (result) {
			*result = r;
		}
	});
}

bool Interpreter::callMethod(const ObjectRef& object, const std::string& name, const std::vector<Value>& args, Value* result)
{
	return guarded([&] {
		const Value r = callMember(Value(object), name, args);
		if (result) {
			*result = r;
		}
	});
}

ObjectRef Interpreter::construct(const Value& constructor, const std::vector<Value>& args)
{
	ObjectRef out;
	guarded([&] { out = constructWith(constructor, args).object(); });
	return out;
}

bool Interpreter::initializeInstance(const ObjectRef& instance, const Value& constructor, const std::vector<Value>& args)
{
	if (!instance || !constructor.isFunction()) {
		return false;
	}
	return guarded([&] {
		const ObjectRef proto = getMember(constructor, "prototype").object();
		instance->setProto(proto);
		instance->define("__constructor__", constructor, kDontEnum);
		CallArgs call {instance, args, proto, true};
		DepthGuard depth(m_depth, m_maxDepth);
		static_cast<Function*>(constructor.asObject().get())->call(*this, call);
	});
}

// --- Conversions

Value Interpreter::toPrimitive(const Value& v, Hint hint)
{
	if (!v.isObject()) {
		return v;
	}
	const ObjectRef& object = v.asObject();
	Value out;
	if (object->defaultValue(*this, hint == Hint::String, out)) {
		return out;
	}
	// ECMA-262 [[DefaultValue]]: valueOf first unless a string is wanted
	// (dates prefer strings when there is no hint).
	const bool stringFirst = hint == Hint::String || (hint == Hint::None && dynamic_cast<DateObject*>(object.get()));
	for (int pass = 0; pass < 2; ++pass) {
		const char* method = (pass == 0) == stringFirst ? "toString" : "valueOf";
		const Value fn = getMember(v, method);
		if (fn.isFunction()) {
			const Value r = call(fn, object, {});
			if (r.isPrimitive()) {
				return r;
			}
		}
	}
	return Value(object->isFunction() ? "[type Function]" : "[type Object]");
}

double Interpreter::toNumber(const Value& v)
{
	switch (v.type()) {
	case Value::Type::Undefined:
	case Value::Type::Null: return std::numeric_limits<double>::quiet_NaN();
	case Value::Type::Boolean: return v.asBoolean() ? 1 : 0;
	case Value::Type::Number: return v.asNumber();
	case Value::Type::String: return stringToNumber(v.asString());
	case Value::Type::Object: return toNumber(toPrimitive(v, Hint::Number));
	}
	return 0;
}

std::string Interpreter::toString(const Value& v)
{
	switch (v.type()) {
	case Value::Type::Undefined: return "undefined";
	case Value::Type::Null: return "null";
	case Value::Type::Boolean: return v.asBoolean() ? "true" : "false";
	case Value::Type::Number: return numberToString(v.asNumber());
	case Value::Type::String: return v.asString();
	case Value::Type::Object: return toString(toPrimitive(v, Hint::String));
	}
	return {};
}

ObjectRef Interpreter::toObject(const Value& v)
{
	switch (v.type()) {
	case Value::Type::Object: return v.asObject();
	case Value::Type::String: return make<PrimitiveObject>(m_protos.string, v);
	case Value::Type::Number: return make<PrimitiveObject>(m_protos.number, v);
	case Value::Type::Boolean: return make<PrimitiveObject>(m_protos.boolean, v);
	default: return nullptr;
	}
}

const char* Interpreter::typeOf(const Value& v)
{
	switch (v.type()) {
	case Value::Type::Undefined: return "undefined";
	case Value::Type::Null: return "null";
	case Value::Type::Boolean: return "boolean";
	case Value::Type::Number: return "number";
	case Value::Type::String: return "string";
	case Value::Type::Object: return v.asObject()->typeName();
	}
	return "undefined";
}

bool Interpreter::equals(const Value& a, const Value& b)
{
	if (a.type() == b.type()) {
		return strictEquals(a, b);
	}
	if (a.isNullish() || b.isNullish()) {
		return a.isNullish() && b.isNullish();
	}
	if (a.isNumber() && b.isString()) {
		return a.asNumber() == stringToNumber(b.asString());
	}
	if (a.isString() && b.isNumber()) {
		return stringToNumber(a.asString()) == b.asNumber();
	}
	if (a.isBoolean()) {
		return equals(Value(a.asBoolean() ? 1 : 0), b);
	}
	if (b.isBoolean()) {
		return equals(a, Value(b.asBoolean() ? 1 : 0));
	}
	if (a.isObject()) {
		const Value p = toPrimitive(a);
		return p.isObject() ? false : equals(p, b);
	}
	if (b.isObject()) {
		const Value p = toPrimitive(b);
		return p.isObject() ? false : equals(a, p);
	}
	return false;
}

Value Interpreter::add(const Value& a, const Value& b)
{
	const Value pa = toPrimitive(a);
	const Value pb = toPrimitive(b);
	if (pa.isString() || pb.isString()) {
		return Value(toString(pa) + toString(pb));
	}
	return Value(toNumber(pa) + toNumber(pb));
}

Value Interpreter::less(const Value& a, const Value& b)
{
	const Value pa = toPrimitive(a, Hint::Number);
	const Value pb = toPrimitive(b, Hint::Number);
	if (pa.isString() && pb.isString()) {
		return Value(pa.asString() < pb.asString()); // UTF-8 bytes order like code points
	}
	const double na = toNumber(pa);
	const double nb = toNumber(pb);
	if (std::isnan(na) || std::isnan(nb)) {
		return Value();
	}
	return Value(na < nb);
}

namespace {

// Does prototype `p` declare (ImplementsOp) `ctor` or an interface that
// extends it?
bool implementsInterface(Interpreter& vm, Object* p, const ObjectRef& ctor, int depth)
{
	if (!p || depth > 32) {
		return false;
	}
	for (const ObjectRef& iface : p->interfaces()) {
		if (iface == ctor) {
			return true;
		}
		const ObjectRef ifaceProto = vm.getMember(Value(iface), "prototype").object();
		for (Object* q = ifaceProto.get(); q; q = q->proto().get()) {
			if (implementsInterface(vm, q, ctor, depth + 1)) {
				return true;
			}
			if (++depth > 32) {
				break;
			}
		}
	}
	return false;
}

} // namespace

bool Interpreter::instanceOf(const Value& object, const Value& constructor)
{
	if (!object.isObject() || !constructor.isFunction()) {
		return false;
	}
	const ObjectRef target = getMember(constructor, "prototype").object();
	Object* p = object.asObject()->proto().get();
	for (int depth = 0; p && depth < kMaxChain; ++depth, p = p->proto().get()) {
		if (p == target.get() || implementsInterface(*this, p, constructor.asObject(), 0)) {
			return true;
		}
	}
	return false;
}

// --- Members and calls

Value Interpreter::getMember(const Value& object, const std::string& name)
{
	if (object.isString() && name == "length") {
		return Value(utf16Length(object.asString()));
	}
	const ObjectRef o = toObject(object);
	Value out;
	if (o) {
		o->getMember(*this, name, out);
	}
	return out;
}

void Interpreter::setMember(const Value& object, const std::string& name, const Value& value)
{
	if (object.isObject()) {
		object.asObject()->setMember(*this, name, value);
	}
}

Value Interpreter::call(const Value& function, const ObjectRef& thisObject, std::vector<Value> args, const ObjectRef& superHome)
{
	if (!function.isFunction()) {
		return Value();
	}
	const ObjectRef keep = function.asObject();
	DepthGuard depth(m_depth, m_maxDepth);
	CallArgs call {thisObject, std::move(args), superHome, false};
	return static_cast<Function*>(keep.get())->call(*this, call);
}

Value Interpreter::callMember(const Value& object, const std::string& name, std::vector<Value> args)
{
	auto* super = object.isObject() ? dynamic_cast<SuperObject*>(object.asObject().get()) : nullptr;
	if (name.empty()) {
		if (super) {
			// super(...): the parent constructor on the same `this`.
			const ObjectRef ctor = super->constructorFunction(*this);
			return call(Value(ctor), super->self, std::move(args), super->home ? super->home->proto() : nullptr);
		}
		return call(object, nullptr, std::move(args));
	}
	if (super) {
		// super.method(...): found from the parent prototype on, `this` kept.
		Object* start = super->home ? super->home->proto().get() : nullptr;
		Value fn;
		if (!start || !Object::getFromChain(*this, start, name, super->self, fn)) {
			return Value();
		}
		Object* owner = start->findOwner(name);
		return call(fn, super->self, std::move(args), owner ? owner->shared_from_this() : nullptr);
	}
	const ObjectRef o = toObject(object);
	if (!o) {
		return Value();
	}
	Value fn;
	o->getMember(*this, name, fn);
	if (!fn.isFunction()) {
		return Value();
	}
	Object* owner = o->findOwner(name);
	return call(fn, o, std::move(args), owner ? owner->shared_from_this() : o->proto());
}

Value Interpreter::constructWith(const Value& constructor, std::vector<Value> args)
{
	if (!constructor.isFunction()) {
		return Value();
	}
	const ObjectRef ctor = constructor.asObject();
	auto* function = static_cast<Function*>(ctor.get());
	ObjectRef instance = function->createInstance(*this);
	if (!instance) {
		instance = make<Object>();
	}
	ObjectRef proto = getMember(constructor, "prototype").object();
	if (!proto) {
		proto = m_protos.object;
	}
	instance->setProto(proto);
	instance->define("__constructor__", constructor, kDontEnum);
	DepthGuard depth(m_depth, m_maxDepth);
	CallArgs call {instance, std::move(args), proto, true};
	const Value result = function->call(*this, call);
	return result.isObject() ? result : Value(instance);
}

// --- Target paths

ObjectRef Interpreter::rootOf(const ObjectRef& from)
{
	if (ObjectRef root = m_host->root(from)) {
		return root;
	}
	// Without a host answer, climb _parent.
	ObjectRef clip = from;
	for (int depth = 0; clip && depth < kMaxChain; ++depth) {
		const ObjectRef parent = getMember(Value(clip), "_parent").object();
		if (!parent) {
			break;
		}
		clip = parent;
	}
	return clip;
}

ObjectRef Interpreter::resolveTarget(const ObjectRef& from, const std::string& path)
{
	if (path.empty()) {
		return from;
	}
	if (ObjectRef clip = m_host->resolveTarget(from, path)) {
		return clip;
	}
	ObjectRef clip = from;
	size_t at = 0;
	if (path[0] == '/') {
		clip = rootOf(from);
		at = 1;
	}
	// Components split on '/' or '.', except that ".." is one component.
	while (at < path.size() && clip) {
		size_t stop = at;
		if (path.compare(at, 2, "..") == 0) {
			stop = at + 2;
		} else {
			while (stop < path.size() && path[stop] != '/' && path[stop] != '.') {
				++stop;
			}
		}
		const std::string part = path.substr(at, stop - at);
		at = stop < path.size() ? stop + 1 : stop;
		if (part.empty()) {
			continue;
		}
		if (part == ".." || part == "_parent") {
			clip = getMember(Value(clip), "_parent").object();
		} else if (part == "_root") {
			clip = rootOf(clip);
		} else if (part == "_global") {
			clip = m_global;
		} else if (part.compare(0, 6, "_level") == 0 && part.size() > 6) {
			clip = m_host->level(toInt32(stringToNumber(part.substr(6))));
		} else if (part != "this") {
			clip = getMember(Value(clip), part).object();
		}
	}
	return clip;
}

// --- Intervals

int Interpreter::addInterval(const Value& function, const ObjectRef& thisObject, const std::string& method, double period, std::vector<Value> args)
{
	Interval interval;
	interval.id = m_nextInterval++;
	interval.function = function;
	interval.thisObject = thisObject;
	interval.method = method;
	interval.args = std::move(args);
	interval.period = std::isnan(period) || period < 1 ? 1 : period;
	interval.due = m_clock + interval.period;
	m_intervals.push_back(std::move(interval));
	return m_intervals.back().id;
}

bool Interpreter::clearInterval(int id)
{
	const auto it = std::find_if(m_intervals.begin(), m_intervals.end(), [id](const Interval& i) { return i.id == id; });
	if (it == m_intervals.end()) {
		return false;
	}
	m_intervals.erase(it);
	return true;
}

void Interpreter::advance(double milliseconds)
{
	if (!(milliseconds > 0)) {
		return;
	}
	m_clock += milliseconds;
	// Each due timer fires once per advance, in id order; a timer that fell
	// behind catches up from now rather than bursting.
	std::vector<int> due;
	for (const Interval& i : m_intervals) {
		if (i.due <= m_clock) {
			due.push_back(i.id);
		}
	}
	for (int id : due) {
		const auto it = std::find_if(m_intervals.begin(), m_intervals.end(), [id](const Interval& i) { return i.id == id; });
		if (it == m_intervals.end()) {
			continue; // cleared by an earlier callback
		}
		it->due += it->period;
		if (it->due <= m_clock) {
			it->due = m_clock + it->period;
		}
		const Interval interval = *it; // the callback may change the list
		if (!interval.method.empty()) {
			callMethod(interval.thisObject, interval.method, interval.args);
		} else {
			callFunction(interval.function, Value(interval.thisObject), interval.args);
		}
	}
}

double Interpreter::random()
{
	return std::uniform_real_distribution<double>(0.0, 1.0)(m_random);
}

} // namespace dice::hfe::ui::avm1
