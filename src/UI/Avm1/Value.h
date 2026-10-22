// ActionScript 1/2 values as the SWF 7 player (AVM1) sees them.
//
// A value is undefined, null, a boolean, a number (always a double), a
// string or a reference to an object. Strings are UTF-8 (SWF 6 and later);
// the string functions index them by UTF-16 unit like the player does, so
// they go through utf8ToUtf16 first.
//
// Conversions that may run script (an object's valueOf or toString) live on
// the Interpreter; the ones here only handle primitives.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace dice::hfe::ui::avm1 {

class Object;
using ObjectRef = std::shared_ptr<Object>;

class Value {
public:
	enum class Type : uint8_t { Undefined, Null, Boolean, Number, String, Object };

	Value() = default;
	Value(bool b) : m_type(Type::Boolean), m_bool(b) {}
	template <typename T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, int> = 0>
	Value(T n) : m_type(Type::Number), m_number(static_cast<double>(n))
	{
	}
	// Enumerators are numbers (not booleans, which C++ would pick).
	template <typename T, std::enable_if_t<std::is_enum_v<T>, int> = 0>
	Value(T e) : Value(static_cast<std::underlying_type_t<T>>(e))
	{
	}
	Value(const char* s): m_type(Type::String), m_string(s) {}
	Value(std::string s) : m_type(Type::String), m_string(std::move(s)) {}
	Value(ObjectRef o);
	template <typename T, std::enable_if_t<std::is_base_of_v<Object, T>, int> = 0>
	Value(std::shared_ptr<T> o) : Value(ObjectRef(std::move(o)))
	{
	}

	static Value null();

	Type type() const { return m_type; }
	bool isUndefined() const { return m_type == Type::Undefined; }
	bool isNull() const { return m_type == Type::Null; }
	bool isNullish() const { return m_type == Type::Undefined || m_type == Type::Null; }
	bool isBoolean() const { return m_type == Type::Boolean; }
	bool isNumber() const { return m_type == Type::Number; }
	bool isString() const { return m_type == Type::String; }
	bool isObject() const { return m_type == Type::Object; }
	bool isPrimitive() const { return m_type != Type::Object; }
	bool isFunction() const;

	// Raw accessors: the result is meaningless for another type.
	bool asBoolean() const { return m_bool; }
	double asNumber() const { return m_number; }
	const std::string& asString() const { return m_string; }
	const ObjectRef& asObject() const { return m_object; }

	// The object if this is one, else null.
	ObjectRef object() const { return m_type == Type::Object ? m_object : nullptr; }

private:
	Type m_type = Type::Undefined;
	bool m_bool = false;
	double m_number = 0;
	std::string m_string;
	ObjectRef m_object;
};

// ToBoolean for SWF 7: strings are true when not empty.
bool toBoolean(const Value& v);

// The player's number parser (SWF 6+): leading white space, an optional
// sign, "0x" hex, a leading zero for octal, or a decimal with an exponent.
// Anything else, including trailing text and the empty string, is NaN.
double stringToNumber(const std::string& s);

// Numbers print with 15 significant digits; exponents start at 1e15 and
// below 1e-4 ("1e+15", "1e-5"). Other radixes print the integer part.
std::string numberToString(double n, int radix = 10);

// ECMA-262 ToInt32/ToUint32 (wrapping modulo 2^32, NaN and infinities 0).
int32_t toInt32(double n);
uint32_t toUint32(double n);
// ToInteger: truncated toward zero, NaN 0.
double toInteger(double n);

// StrictEquals (0x66): same type and same value, objects by identity.
bool strictEquals(const Value& a, const Value& b);

// A canonical array index ("0", "17", not "01" or "-1"), or -1.
int64_t arrayIndex(const std::string& name);

// UTF-8 <-> UTF-16. Bytes that are not valid UTF-8 pass through as Latin-1.
std::u16string utf8ToUtf16(const std::string& s);
std::string utf16ToUtf8(const std::u16string& s);
// Length of `s` in UTF-16 units.
size_t utf16Length(const std::string& s);

} // namespace dice::hfe::ui::avm1
