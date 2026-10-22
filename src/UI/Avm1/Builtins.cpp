// Object, Function, Error, AsBroadcaster, System and the global functions.
#include "Builtins.h"

#include <cmath>
#include <limits>

namespace dice::hfe::ui::avm1 {

void Interpreter::installBuiltins()
{
	m_protos.array = newObject();
	m_protos.string = newObject();
	m_protos.number = newObject();
	m_protos.boolean = newObject();
	m_protos.date = newObject();
	m_protos.error = newObject();
	builtins::installCore(*this);
	builtins::installArray(*this);
	builtins::installString(*this);
	builtins::installDate(*this);
	builtins::installStubs(*this);
}

namespace builtins {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

int digitValue(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'Z') {
		return c - 'A' + 10;
	}
	return 99;
}

// parseInt: optional sign, "0x" for hex and a leading 0 for octal when no
// radix is given, then as many digits as parse.
double parseInt(const std::string& s, double radixValue)
{
	size_t i = 0;
	while (i < s.size() && isSpace(s[i])) {
		++i;
	}
	bool negative = false;
	if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
		negative = s[i] == '-';
		++i;
	}
	int radix = std::isnan(radixValue) ? 0 : toInt32(radixValue);
	if (radix != 0 && (radix < 2 || radix > 36)) {
		return kNaN;
	}
	const bool hexPrefix = i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X');
	if ((radix == 0 || radix == 16) && hexPrefix) {
		radix = 16;
		i += 2;
	} else if (radix == 0) {
		radix = i + 1 < s.size() && s[i] == '0' ? 8 : 10;
	}
	double n = 0;
	size_t digits = 0;
	for (; i < s.size() && digitValue(s[i]) < radix; ++i, ++digits) {
		n = n * radix + digitValue(s[i]);
	}
	if (digits == 0) {
		return kNaN;
	}
	return negative ? -n : n;
}

// parseFloat: the longest decimal prefix.
double parseFloat(const std::string& s)
{
	size_t i = 0;
	while (i < s.size() && isSpace(s[i])) {
		++i;
	}
	size_t k = i;
	if (k < s.size() && (s[k] == '+' || s[k] == '-')) {
		++k;
	}
	size_t digits = 0;
	while (k < s.size() && s[k] >= '0' && s[k] <= '9') {
		++k, ++digits;
	}
	if (k < s.size() && s[k] == '.') {
		++k;
		while (k < s.size() && s[k] >= '0' && s[k] <= '9') {
			++k, ++digits;
		}
	}
	if (digits == 0) {
		return kNaN;
	}
	if (k < s.size() && (s[k] == 'e' || s[k] == 'E')) {
		size_t e = k + 1;
		if (e < s.size() && (s[e] == '+' || s[e] == '-')) {
			++e;
		}
		if (e < s.size() && s[e] >= '0' && s[e] <= '9') {
			while (e < s.size() && s[e] >= '0' && s[e] <= '9') {
				++e;
			}
			k = e;
		}
	}
	return stringToNumber(s.substr(i, k - i));
}

// escape(): everything but letters, digits and "-_." becomes %XX of its
// UTF-8 bytes.
std::string escape(const std::string& s)
{
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	for (char ch : s) {
		const auto c = static_cast<uint8_t>(ch);
		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.') {
			out.push_back(ch);
		} else {
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 15]);
		}
	}
	return out;
}

std::string unescape(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] == '%' && i + 2 < s.size() && digitValue(s[i + 1]) < 16 && digitValue(s[i + 2]) < 16) {
			out.push_back(static_cast<char>(digitValue(s[i + 1]) * 16 + digitValue(s[i + 2])));
			i += 2;
		} else {
			out.push_back(s[i]);
		}
	}
	return out;
}

// The arguments of Function.apply: an array's elements.
std::vector<Value> argumentList(Interpreter& vm, const Value& list)
{
	std::vector<Value> out;
	if (const auto* array = dynamic_cast<ArrayObject*>(list.object().get())) {
		return array->elements;
	}
	if (list.isObject()) {
		const double length = vm.toNumber(vm.getMember(list, "length"));
		for (double i = 0; i < length && i < 65536; ++i) {
			out.push_back(vm.getMember(list, numberToString(i)));
		}
	}
	return out;
}

void installObject(Interpreter& vm)
{
	const ObjectRef& proto = vm.prototypes().object;
	auto object = vm.newClass(
		[](Interpreter& vm, CallArgs& c) -> Value {
			if (c.size() > 0 && !c[0].isNullish()) {
				return Value(vm.toObject(c[0]));
			}
			return c.constructing ? Value(c.thisObject) : Value(vm.newObject());
		},
		{}, proto);
	defineGlobal(vm, "Object", Value(object));

	vm.defineMethod(proto, "toString", [](Interpreter&, CallArgs&) -> Value { return Value("[object Object]"); });
	vm.defineMethod(proto, "toLocaleString", [](Interpreter& vm, CallArgs& c) -> Value {
		return vm.callMember(Value(c.thisObject), "toString", {});
	});
	vm.defineMethod(proto, "valueOf", [](Interpreter&, CallArgs& c) -> Value { return Value(c.thisObject); });
	vm.defineMethod(proto, "hasOwnProperty", [](Interpreter& vm, CallArgs& c) -> Value {
		return Value(c.thisObject && c.size() > 0 && c.thisObject->hasOwnMember(vm.toString(c[0])));
	});
	vm.defineMethod(proto, "isPropertyEnumerable", [](Interpreter& vm, CallArgs& c) -> Value {
		if (!c.thisObject || c.size() == 0) {
			return Value(false);
		}
		const std::string name = vm.toString(c[0]);
		std::vector<std::string> names;
		c.thisObject->ownEnumerableNames(names);
		for (const std::string& n : names) {
			if (n == name) {
				return Value(true);
			}
		}
		return Value(false);
	});
	vm.defineMethod(proto, "isPrototypeOf", [](Interpreter&, CallArgs& c) -> Value {
		const ObjectRef other = c[0].object();
		int depth = 0;
		for (Object* p = other ? other->proto().get() : nullptr; p && depth < 256; p = p->proto().get(), ++depth) {
			if (p == c.thisObject.get()) {
				return Value(true);
			}
		}
		return Value(false);
	});
	vm.defineMethod(proto, "addProperty", [](Interpreter& vm, CallArgs& c) -> Value {
		if (!c.thisObject || c.size() < 2) {
			return Value(false);
		}
		const ObjectRef setter = c[2].isFunction() ? c[2].asObject() : nullptr;
		return Value(c.thisObject->addAccessor(vm.toString(c[0]), c[1].object(), setter));
	});
	vm.defineMethod(proto, "watch", [](Interpreter& vm, CallArgs& c) -> Value {
		if (!c.thisObject || !c[1].isFunction()) {
			return Value(false);
		}
		c.thisObject->watch(vm.toString(c[0]), Object::Watch {c[1].asObject(), c[2]});
		return Value(true);
	});
	vm.defineMethod(proto, "unwatch", [](Interpreter& vm, CallArgs& c) -> Value {
		return Value(c.thisObject && c.thisObject->unwatch(vm.toString(c[0])));
	});

	vm.defineMethod(object, "registerClass", [](Interpreter& vm, CallArgs& c) -> Value {
		const ObjectRef ctor = c[1].isFunction() ? c[1].asObject() : nullptr;
		return Value(vm.host().registerClass(vm.toString(c[0]), ctor));
	});
}

void installFunction(Interpreter& vm)
{
	const ObjectRef& proto = vm.prototypes().function;
	auto function = vm.newClass(
		[](Interpreter& vm, CallArgs&) -> Value { return Value(vm.newFunction({})); }, {}, proto);
	defineGlobal(vm, "Function", Value(function));

	vm.defineMethod(proto, "toString", [](Interpreter&, CallArgs&) -> Value { return Value("[type Function]"); });
	vm.defineMethod(proto, "call", [](Interpreter& vm, CallArgs& c) -> Value {
		std::vector<Value> args;
		if (c.size() > 1) {
			args.assign(c.args.begin() + 1, c.args.end());
		}
		return vm.call(Value(c.thisObject), vm.toObject(c[0]), std::move(args));
	});
	vm.defineMethod(proto, "apply", [](Interpreter& vm, CallArgs& c) -> Value {
		return vm.call(Value(c.thisObject), vm.toObject(c[0]), argumentList(vm, c[1]));
	});
}

void installError(Interpreter& vm)
{
	const ObjectRef& proto = vm.prototypes().error;
	auto error = vm.newClass(
		[](Interpreter& vm, CallArgs& c) -> Value {
			ObjectRef self = c.constructing ? c.thisObject : nullptr;
			if (!self) {
				self = vm.newObject();
				self->setProto(vm.prototypes().error);
			}
			if (c.size() > 0 && !c[0].isUndefined()) {
				self->setMember(vm, "message", c[0]);
			}
			return Value(self);
		},
		{}, proto);
	proto->define("name", Value("Error"), kDontEnum);
	proto->define("message", Value("Error"), kDontEnum);
	vm.defineMethod(proto, "toString", [](Interpreter& vm, CallArgs& c) -> Value {
		return Value(vm.toString(vm.getMember(Value(c.thisObject), "message")));
	});
	defineGlobal(vm, "Error", Value(error));
}

void installBroadcaster(Interpreter& vm)
{
	const ObjectRef broadcaster = vm.newObject();
	vm.defineMethod(broadcaster, "initialize", [](Interpreter& vm, CallArgs& c) -> Value {
		if (const ObjectRef target = c[0].object()) {
			initializeBroadcaster(vm, target);
		}
		return Value();
	});
	vm.defineMethod(broadcaster, "addListener", [](Interpreter& vm, CallArgs& c) -> Value {
		auto* listeners = dynamic_cast<ArrayObject*>(vm.getMember(Value(c.thisObject), "_listeners").object().get());
		if (!listeners) {
			return Value(false);
		}
		for (const Value& l : listeners->elements) {
			if (strictEquals(l, c[0])) {
				return Value(true);
			}
		}
		listeners->elements.push_back(c[0]);
		return Value(true);
	});
	vm.defineMethod(broadcaster, "removeListener", [](Interpreter& vm, CallArgs& c) -> Value {
		auto* listeners = dynamic_cast<ArrayObject*>(vm.getMember(Value(c.thisObject), "_listeners").object().get());
		if (!listeners) {
			return Value(false);
		}
		for (auto it = listeners->elements.begin(); it != listeners->elements.end(); ++it) {
			if (strictEquals(*it, c[0])) {
				listeners->elements.erase(it);
				return Value(true);
			}
		}
		return Value(false);
	});
	vm.defineMethod(broadcaster, "broadcastMessage", [](Interpreter& vm, CallArgs& c) -> Value {
		auto* listeners = dynamic_cast<ArrayObject*>(vm.getMember(Value(c.thisObject), "_listeners").object().get());
		if (!listeners || c.size() == 0) {
			return Value();
		}
		const std::string event = vm.toString(c[0]);
		const std::vector<Value> args(c.args.begin() + 1, c.args.end());
		const std::vector<Value> snapshot = listeners->elements; // listeners may unsubscribe
		for (const Value& l : snapshot) {
			vm.callMember(l, event, args);
		}
		return Value(true);
	});
	defineGlobal(vm, "AsBroadcaster", Value(broadcaster));
}

// ASSetPropFlags(object, names, set, clear): names is null (all), "a,b" or
// an array.
Value setPropFlags(Interpreter& vm, CallArgs& c)
{
	const ObjectRef object = c[0].object();
	if (!object) {
		return Value();
	}
	const uint8_t set = static_cast<uint8_t>(toInt32(vm.toNumber(c[2])) & 7);
	const uint8_t clear = c.size() > 3 ? static_cast<uint8_t>(toInt32(vm.toNumber(c[3])) & 7) : 0;
	const auto apply = [&](Property& p) { p.flags = static_cast<uint8_t>((p.flags & ~clear) | set); };
	if (c[1].isNull()) {
		for (const auto& entry : object->properties().entries()) {
			apply(*object->findOwn(entry.first));
		}
		return Value();
	}
	std::vector<std::string> names;
	if (const auto* array = dynamic_cast<ArrayObject*>(c[1].object().get())) {
		for (const Value& v : array->elements) {
			names.push_back(vm.toString(v));
		}
	} else {
		const std::string list = vm.toString(c[1]);
		size_t at = 0;
		while (at <= list.size()) {
			const size_t comma = std::min(list.find(',', at), list.size());
			names.push_back(list.substr(at, comma - at));
			at = comma + 1;
		}
	}
	for (const std::string& name : names) {
		if (Property* p = object->findOwn(name)) {
			apply(*p);
		}
	}
	return Value();
}

void installGlobalFunctions(Interpreter& vm)
{
	const ObjectRef& global = vm.global();
	defineGlobal(vm, "NaN", Value(kNaN));
	defineGlobal(vm, "Infinity", Value(std::numeric_limits<double>::infinity()));
	defineGlobal(vm, "_global", Value(global));

	vm.defineMethod(global, "parseInt", [](Interpreter& vm, CallArgs& c) -> Value {
		return Value(parseInt(vm.toString(c[0]), c.size() > 1 && !c[1].isUndefined() ? vm.toNumber(c[1]) : kNaN));
	});
	vm.defineMethod(global, "parseFloat", [](Interpreter& vm, CallArgs& c) -> Value { return Value(parseFloat(vm.toString(c[0]))); });
	vm.defineMethod(global, "isNaN", [](Interpreter& vm, CallArgs& c) -> Value { return Value(std::isnan(vm.toNumber(c[0]))); });
	vm.defineMethod(global, "isFinite", [](Interpreter& vm, CallArgs& c) -> Value { return Value(std::isfinite(vm.toNumber(c[0]))); });
	vm.defineMethod(global, "escape", [](Interpreter& vm, CallArgs& c) -> Value { return Value(escape(vm.toString(c[0]))); });
	vm.defineMethod(global, "unescape", [](Interpreter& vm, CallArgs& c) -> Value { return Value(unescape(vm.toString(c[0]))); });
	vm.defineMethod(global, "ASSetPropFlags", setPropFlags);
	// ASnative(a, b) names player internals by number; scripts only see a
	// function that does nothing.
	vm.defineMethod(global, "ASnative", [](Interpreter& vm, CallArgs&) -> Value { return Value(vm.newFunction({})); });
	vm.defineMethod(global, "updateAfterEvent", [](Interpreter&, CallArgs&) -> Value { return Value(); });
	vm.defineMethod(global, "getVersion", [](Interpreter&, CallArgs&) -> Value { return Value("WIN 7,0,19,0"); });

	// setInterval(function, ms, args...) or setInterval(object, "method", ms, args...).
	vm.defineMethod(global, "setInterval", [](Interpreter& vm, CallArgs& c) -> Value {
		if (c[0].isFunction()) {
			const std::vector<Value> args(c.args.begin() + std::min<size_t>(2, c.size()), c.args.end());
			return Value(vm.addInterval(c[0], nullptr, {}, vm.toNumber(c[1]), args));
		}
		if (c[0].isObject() && c.size() >= 3) {
			const std::vector<Value> args(c.args.begin() + 3, c.args.end());
			return Value(vm.addInterval(Value(), c[0].asObject(), vm.toString(c[1]), vm.toNumber(c[2]), args));
		}
		return Value();
	});
	vm.defineMethod(global, "clearInterval", [](Interpreter& vm, CallArgs& c) -> Value {
		vm.clearInterval(toInt32(vm.toNumber(c[0])));
		return Value();
	});
}

void installSystem(Interpreter& vm)
{
	const ObjectRef system = vm.newObject();
	const ObjectRef capabilities = vm.newObject();
	capabilities->define("version", Value("WIN 7,0,19,0"));
	capabilities->define("os", Value("Windows XP"));
	capabilities->define("language", Value("en"));
	capabilities->define("manufacturer", Value("Macromedia Windows"));
	capabilities->define("playerType", Value("External"));
	capabilities->define("screenResolutionX", Value(800));
	capabilities->define("screenResolutionY", Value(600));
	capabilities->define("hasAudio", Value(true));
	system->define("capabilities", Value(capabilities));
	const ObjectRef security = vm.newObject();
	vm.defineMethod(security, "allowDomain", [](Interpreter&, CallArgs&) -> Value { return Value(); });
	vm.defineMethod(security, "allowInsecureDomain", [](Interpreter&, CallArgs&) -> Value { return Value(); });
	vm.defineMethod(security, "loadPolicyFile", [](Interpreter&, CallArgs&) -> Value { return Value(); });
	system->define("security", Value(security));
	system->define("useCodepage", Value(false));
	system->define("exactSettings", Value(true));
	vm.defineMethod(system, "setClipboard", [](Interpreter&, CallArgs&) -> Value { return Value(false); });
	vm.defineMethod(system, "showSettings", [](Interpreter&, CallArgs&) -> Value { return Value(); });
	defineGlobal(vm, "System", Value(system));
}

} // namespace

void defineGlobal(Interpreter& vm, const std::string& name, const Value& value) { vm.global()->define(name, value, kDontEnum); }

std::shared_ptr<NativeFunction> defineClass(Interpreter& vm, const std::string& name, NativeCallback constructor)
{
	if (!constructor) {
		constructor = [](Interpreter&, CallArgs&) -> Value { return Value(); };
	}
	auto ctor = vm.newClass(std::move(constructor));
	defineGlobal(vm, name, Value(ctor));
	return ctor;
}

void initializeBroadcaster(Interpreter& vm, const ObjectRef& object)
{
	const Value broadcaster = vm.global()->findOwn("AsBroadcaster") ? vm.global()->findOwn("AsBroadcaster")->value : Value();
	for (const char* method : {"addListener", "removeListener", "broadcastMessage"}) {
		object->define(method, vm.getMember(broadcaster, method), kDontEnum);
	}
	object->define("_listeners", Value(vm.newArray()), kDontEnum);
}

void installCore(Interpreter& vm)
{
	installObject(vm);
	installFunction(vm);
	installError(vm);
	installBroadcaster(vm);
	installGlobalFunctions(vm);
	installSystem(vm);
}

} // namespace builtins

} // namespace dice::hfe::ui::avm1
