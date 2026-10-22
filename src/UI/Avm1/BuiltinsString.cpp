// String, Number, Boolean and Math. String positions count UTF-16 units.
#include "Builtins.h"

#include <cfloat>
#include <cmath>
#include <limits>

namespace dice::hfe::ui::avm1::builtins {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// The primitive behind `this` for String/Number/Boolean methods.
Value primitiveOf(CallArgs& c)
{
	if (const auto* wrapper = dynamic_cast<PrimitiveObject*>(c.thisObject.get())) {
		return wrapper->primitive;
	}
	return c.thisObject ? Value(c.thisObject) : Value();
}

std::u16string thisText(Interpreter& vm, CallArgs& c) { return utf8ToUtf16(vm.toString(primitiveOf(c))); }

Value text(const std::u16string& s) { return Value(utf16ToUtf8(s)); }

// Simple case mapping: ASCII, Latin-1, Greek and Cyrillic.
char16_t toLower(char16_t c)
{
	if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7) || (c >= 0x391 && c <= 0x3ab && c != 0x3a2) ||
		(c >= 0x410 && c <= 0x42f)) {
		return static_cast<char16_t>(c + 0x20);
	}
	if (c >= 0x400 && c <= 0x40f) {
		return static_cast<char16_t>(c + 0x50);
	}
	return c;
}

char16_t toUpper(char16_t c)
{
	if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7) || (c >= 0x3b1 && c <= 0x3cb && c != 0x3c2) ||
		(c >= 0x430 && c <= 0x44f)) {
		return static_cast<char16_t>(c - 0x20);
	}
	if (c >= 0x450 && c <= 0x45f) {
		return static_cast<char16_t>(c - 0x50);
	}
	return c;
}

// An integer position argument, NaN as 0.
double position(Interpreter& vm, const Value& v) { return toInteger(vm.toNumber(v)); }

size_t clampTo(double n, size_t size) { return static_cast<size_t>(std::max(0.0, std::min(n, static_cast<double>(size)))); }

void installStringClass(Interpreter& vm)
{
	const ObjectRef& proto = vm.prototypes().string;
	auto string = vm.newClass(
		[](Interpreter& vm, CallArgs& c) -> Value {
			const Value value(c.size() > 0 ? vm.toString(c[0]) : std::string());
			if (c.constructing) {
				if (auto* wrapper = dynamic_cast<PrimitiveObject*>(c.thisObject.get())) {
					wrapper->primitive = value;
					return Value(c.thisObject);
				}
			}
			return value;
		},
		[](Interpreter& vm) -> ObjectRef { return vm.make<PrimitiveObject>(nullptr, Value("")); }, proto);
	defineGlobal(vm, "String", Value(string));

	vm.defineMethod(string, "fromCharCode", [](Interpreter& vm, CallArgs& c) -> Value {
		std::u16string out;
		for (const Value& v : c.args) {
			out.push_back(static_cast<char16_t>(toUint32(vm.toNumber(v)) & 0xffff));
		}
		return text(out);
	});

	const auto valueOf = [](Interpreter& vm, CallArgs& c) -> Value {
		const Value v = primitiveOf(c);
		return v.isString() ? v : Value(vm.toString(v));
	};
	vm.defineMethod(proto, "toString", valueOf);
	vm.defineMethod(proto, "valueOf", valueOf);
	vm.defineMethod(proto, "charAt", [](Interpreter& vm, CallArgs& c) -> Value {
		const std::u16string s = thisText(vm, c);
		const double i = position(vm, c[0]);
		return i >= 0 && i < static_cast<double>(s.size()) ? text(s.substr(static_cast<size_t>(i), 1)) : Value("");
	});
	vm.defineMethod(proto, "charCodeAt", [](Interpreter& vm, CallArgs& c) -> Value {
		const std::u16string s = thisText(vm, c);
		const double i = position(vm, c[0]);
		return i >= 0 && i < static_cast<double>(s.size()) ? Value(s[static_cast<size_t>(i)]) : Value(kNaN);
	});
	vm.defineMethod(proto, "concat", [](Interpreter& vm, CallArgs& c) -> Value {
		std::string out = vm.toString(primitiveOf(c));
		for (const Value& v : c.args) {
			out += vm.toString(v);
		}
		return Value(out);
	});
	vm.defineMethod(proto, "indexOf", [](Interpreter& vm, CallArgs& c) -> Value {
		const std::u16string s = thisText(vm, c);
		const std::u16string search = utf8ToUtf16(vm.toString(c[0]));
		const size_t from = c.size() > 1 ? clampTo(position(vm, c[1]), s.size()) : 0;
		const size_t at = s.find(search, from);
		return at == std::u16string::npos ? Value(-1) : Value(at);
	});
	vm.defineMethod(proto, "lastIndexOf", [](Interpreter& vm, CallArgs& c) -> Value {
		const std::u16string s = thisText(vm, c);
		const std::u16string search = utf8ToUtf16(vm.toString(c[0]));
		size_t from = std::u16string::npos;
		if (c.size() > 1) {
			const double n = position(vm, c[1]);
			if (n < 0) {
				return Value(-1);
			}
			from = clampTo(n, s.size());
		}
		const size_t at = s.rfind(search, from);
		return at == std::u16string::npos ? Value(-1) : Value(at);
	});
	vm.defineMethod(proto, "slice", [](Interpreter& vm, CallArgs& c) -> Value {
		const std::u16string s = thisText(vm, c);
		const double size = static_cast<double>(s.size());
		double from = position(vm, c[0]);
		double to = c[1].isUndefined() ? size : position(vm, c[1]);
		from = from < 0 ? std::max(0.0, size + from) : std::min(from, size);
		to = to < 0 ? std::max(0.0, size + to) : std::min(to, size);
		return from < to ? text(s.substr(static_cast<size_t>(from), static_cast<size_t>(to - from))) : Value("");
	});
	vm.defineMethod(proto, "substring", [](Interpreter& vm, CallArgs& c) -> Value {
		const std::u16string s = thisText(vm, c);
		size_t from = clampTo(position(vm, c[0]), s.size());
		size_t to = c[1].isUndefined() ? s.size() : clampTo(position(vm, c[1]), s.size());
		if (from > to) {
			std::swap(from, to);
		}
		return text(s.substr(from, to - from));
	});
	vm.defineMethod(proto, "substr", [](Interpreter& vm, CallArgs& c) -> Value {
		const std::u16string s = thisText(vm, c);
		const double size = static_cast<double>(s.size());
		double from = position(vm, c[0]);
		if (from < 0) {
			from = std::max(0.0, size + from);
		}
		from = std::min(from, size);
		const double length = c[1].isUndefined() ? size - from : std::min(position(vm, c[1]), size - from);
		return length > 0 ? text(s.substr(static_cast<size_t>(from), static_cast<size_t>(length))) : Value("");
	});
	vm.defineMethod(proto, "split", [](Interpreter& vm, CallArgs& c) -> Value {
		const std::u16string s = thisText(vm, c);
		std::vector<Value> parts;
		double limit = c[1].isUndefined() ? 4294967295.0 : position(vm, c[1]);
		if (limit <= 0) {
			return Value(vm.newArray());
		}
		const auto add = [&](const std::u16string& part) {
			if (static_cast<double>(parts.size()) < limit) {
				parts.push_back(text(part));
			}
		};
		if (c[0].isUndefined()) {
			add(s);
		} else {
			const std::u16string separator = utf8ToUtf16(vm.toString(c[0]));
			if (separator.empty()) {
				for (char16_t ch : s) {
					add(std::u16string(1, ch));
				}
			} else {
				size_t at = 0;
				for (;;) {
					const size_t found = s.find(separator, at);
					if (found == std::u16string::npos) {
						add(s.substr(at));
						break;
					}
					add(s.substr(at, found - at));
					at = found + separator.size();
				}
			}
		}
		return Value(vm.newArray(std::move(parts)));
	});
	vm.defineMethod(proto, "toLowerCase", [](Interpreter& vm, CallArgs& c) -> Value {
		std::u16string s = thisText(vm, c);
		for (char16_t& ch : s) {
			ch = toLower(ch);
		}
		return text(s);
	});
	vm.defineMethod(proto, "toUpperCase", [](Interpreter& vm, CallArgs& c) -> Value {
		std::u16string s = thisText(vm, c);
		for (char16_t& ch : s) {
			ch = toUpper(ch);
		}
		return text(s);
	});
}

void installNumber(Interpreter& vm)
{
	const ObjectRef& proto = vm.prototypes().number;
	auto number = vm.newClass(
		[](Interpreter& vm, CallArgs& c) -> Value {
			const Value value(c.size() > 0 ? vm.toNumber(c[0]) : 0.0);
			if (c.constructing) {
				if (auto* wrapper = dynamic_cast<PrimitiveObject*>(c.thisObject.get())) {
					wrapper->primitive = value;
					return Value(c.thisObject);
				}
			}
			return value;
		},
		[](Interpreter& vm) -> ObjectRef { return vm.make<PrimitiveObject>(nullptr, Value(0)); }, proto);
	const uint8_t constant = kDontEnum | kDontDelete | kReadOnly;
	number->define("MAX_VALUE", Value(DBL_MAX), constant);
	number->define("MIN_VALUE", Value(std::numeric_limits<double>::denorm_min()), constant);
	number->define("NaN", Value(kNaN), constant);
	number->define("NEGATIVE_INFINITY", Value(-std::numeric_limits<double>::infinity()), constant);
	number->define("POSITIVE_INFINITY", Value(std::numeric_limits<double>::infinity()), constant);
	defineGlobal(vm, "Number", Value(number));

	vm.defineMethod(proto, "toString", [](Interpreter& vm, CallArgs& c) -> Value {
		const double n = vm.toNumber(primitiveOf(c));
		const double radix = c[0].isUndefined() ? 10 : vm.toNumber(c[0]);
		return Value(numberToString(n, radix >= 2 && radix <= 36 ? static_cast<int>(radix) : 10));
	});
	vm.defineMethod(proto, "valueOf", [](Interpreter& vm, CallArgs& c) -> Value { return Value(vm.toNumber(primitiveOf(c))); });
}

void installBoolean(Interpreter& vm)
{
	const ObjectRef& proto = vm.prototypes().boolean;
	auto boolean = vm.newClass(
		[](Interpreter&, CallArgs& c) -> Value {
			const Value value(toBoolean(c[0]));
			if (c.constructing) {
				if (auto* wrapper = dynamic_cast<PrimitiveObject*>(c.thisObject.get())) {
					wrapper->primitive = value;
					return Value(c.thisObject);
				}
			}
			return value;
		},
		[](Interpreter& vm) -> ObjectRef { return vm.make<PrimitiveObject>(nullptr, Value(false)); }, proto);
	defineGlobal(vm, "Boolean", Value(boolean));
	vm.defineMethod(proto, "toString", [](Interpreter&, CallArgs& c) -> Value {
		return Value(toBoolean(primitiveOf(c)) ? "true" : "false");
	});
	vm.defineMethod(proto, "valueOf", [](Interpreter&, CallArgs& c) -> Value { return Value(toBoolean(primitiveOf(c))); });
}

void installMath(Interpreter& vm)
{
	const ObjectRef math = vm.newObject();
	const uint8_t constant = kDontEnum | kDontDelete | kReadOnly;
	math->define("E", Value(2.718281828459045), constant);
	math->define("LN10", Value(2.302585092994046), constant);
	math->define("LN2", Value(0.6931471805599453), constant);
	math->define("LOG10E", Value(0.4342944819032518), constant);
	math->define("LOG2E", Value(1.4426950408889634), constant);
	math->define("PI", Value(3.141592653589793), constant);
	math->define("SQRT1_2", Value(0.7071067811865476), constant);
	math->define("SQRT2", Value(1.4142135623730951), constant);

	const auto unary = [&](const char* name, double (*f)(double)) {
		vm.defineMethod(math, name, [f](Interpreter& vm, CallArgs& c) -> Value { return Value(f(vm.toNumber(c[0]))); });
	};
	unary("abs", [](double x) { return std::fabs(x); });
	unary("acos", [](double x) { return std::acos(x); });
	unary("asin", [](double x) { return std::asin(x); });
	unary("atan", [](double x) { return std::atan(x); });
	unary("ceil", [](double x) { return std::ceil(x); });
	unary("cos", [](double x) { return std::cos(x); });
	unary("exp", [](double x) { return std::exp(x); });
	unary("floor", [](double x) { return std::floor(x); });
	unary("log", [](double x) { return std::log(x); });
	unary("round", [](double x) { return std::floor(x + 0.5); });
	unary("sin", [](double x) { return std::sin(x); });
	unary("sqrt", [](double x) { return std::sqrt(x); });
	unary("tan", [](double x) { return std::tan(x); });
	vm.defineMethod(math, "atan2", [](Interpreter& vm, CallArgs& c) -> Value {
		return Value(std::atan2(vm.toNumber(c[0]), vm.toNumber(c[1])));
	});
	vm.defineMethod(math, "pow", [](Interpreter& vm, CallArgs& c) -> Value {
		return Value(std::pow(vm.toNumber(c[0]), vm.toNumber(c[1])));
	});
	vm.defineMethod(math, "max", [](Interpreter& vm, CallArgs& c) -> Value {
		double best = -std::numeric_limits<double>::infinity();
		for (const Value& v : c.args) {
			const double n = vm.toNumber(v);
			if (std::isnan(n)) {
				return Value(kNaN);
			}
			best = std::max(best, n);
		}
		return Value(best);
	});
	vm.defineMethod(math, "min", [](Interpreter& vm, CallArgs& c) -> Value {
		double best = std::numeric_limits<double>::infinity();
		for (const Value& v : c.args) {
			const double n = vm.toNumber(v);
			if (std::isnan(n)) {
				return Value(kNaN);
			}
			best = std::min(best, n);
		}
		return Value(best);
	});
	vm.defineMethod(math, "random", [](Interpreter& vm, CallArgs&) -> Value { return Value(vm.random()); });
	defineGlobal(vm, "Math", Value(math));
}

} // namespace

void installString(Interpreter& vm)
{
	installStringClass(vm);
	installNumber(vm);
	installBoolean(vm);
	installMath(vm);
}

} // namespace dice::hfe::ui::avm1::builtins
