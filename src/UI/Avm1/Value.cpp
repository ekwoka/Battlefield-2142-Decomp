#include "Value.h"

#include "Object.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace dice::hfe::ui::avm1 {

namespace {

bool isSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

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

} // namespace

Value::Value(ObjectRef o)
{
	if (o) {
		m_type = Type::Object;
		m_object = std::move(o);
	} else {
		m_type = Type::Null;
	}
}

Value Value::null()
{
	Value v;
	v.m_type = Type::Null;
	return v;
}

bool Value::isFunction() const { return m_type == Type::Object && m_object->isFunction(); }

bool toBoolean(const Value& v)
{
	switch (v.type()) {
	case Value::Type::Undefined:
	case Value::Type::Null: return false;
	case Value::Type::Boolean: return v.asBoolean();
	case Value::Type::Number: return v.asNumber() != 0 && !std::isnan(v.asNumber());
	case Value::Type::String: return !v.asString().empty();
	case Value::Type::Object: return true;
	}
	return false;
}

double stringToNumber(const std::string& s)
{
	const double nan = std::numeric_limits<double>::quiet_NaN();
	size_t i = 0;
	while (i < s.size() && isSpace(s[i])) {
		++i;
	}
	if (i == s.size()) {
		return nan;
	}
	const std::string text = s.substr(i);

	// Hex: "0x1f", unsigned.
	if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		double n = 0;
		for (size_t k = 2; k < text.size(); ++k) {
			const int d = digitValue(text[k]);
			if (d >= 16) {
				return nan;
			}
			n = n * 16 + d;
		}
		return n;
	}

	// Octal: an optional sign, then "0" and octal digits only.
	{
		size_t k = 0;
		bool negative = false;
		if (text[k] == '-' || text[k] == '+') {
			negative = text[k] == '-';
			++k;
		}
		if (text.size() - k > 1 && text[k] == '0') {
			bool octal = true;
			double n = 0;
			for (size_t j = k; j < text.size() && octal; ++j) {
				octal = text[j] >= '0' && text[j] <= '7';
				n = n * 8 + (text[j] - '0');
			}
			if (octal) {
				return negative ? -n : n;
			}
		}
	}

	// Decimal: [+-] digits [. digits] [(e|E) [+-] digits], at least one digit.
	size_t k = 0;
	if (text[k] == '+' || text[k] == '-') {
		++k;
	}
	size_t digits = 0;
	while (k < text.size() && text[k] >= '0' && text[k] <= '9') {
		++k, ++digits;
	}
	if (k < text.size() && text[k] == '.') {
		++k;
		while (k < text.size() && text[k] >= '0' && text[k] <= '9') {
			++k, ++digits;
		}
	}
	if (digits == 0) {
		return nan;
	}
	if (k < text.size() && (text[k] == 'e' || text[k] == 'E')) {
		size_t e = k + 1;
		if (e < text.size() && (text[e] == '+' || text[e] == '-')) {
			++e;
		}
		size_t expDigits = 0;
		while (e < text.size() && text[e] >= '0' && text[e] <= '9') {
			++e, ++expDigits;
		}
		if (expDigits == 0) {
			return nan;
		}
		k = e;
	}
	if (k != text.size()) {
		return nan;
	}
	return std::strtod(text.c_str(), nullptr);
}

std::string numberToString(double n, int radix)
{
	if (std::isnan(n)) {
		return "NaN";
	}
	if (std::isinf(n)) {
		return n > 0 ? "Infinity" : "-Infinity";
	}
	if (n == 0) {
		return "0";
	}
	if (radix != 10 && radix >= 2 && radix <= 36) {
		double whole = std::trunc(std::fabs(n));
		std::string digits;
		do {
			const int d = static_cast<int>(std::fmod(whole, radix));
			digits.insert(digits.begin(), "0123456789abcdefghijklmnopqrstuvwxyz"[d]);
			whole = std::floor(whole / radix);
		} while (whole > 0 && digits.size() < 1100);
		return n < 0 ? "-" + digits : digits;
	}
	char buffer[40];
	std::snprintf(buffer, sizeof(buffer), "%.15g", n);
	// The C library pads exponents to two digits; the player does not.
	std::string out = buffer;
	const size_t e = out.find('e');
	if (e != std::string::npos && e + 2 < out.size()) {
		size_t first = e + 2;
		while (first + 1 < out.size() && out[first] == '0') {
			out.erase(first, 1);
		}
	}
	return out;
}

int32_t toInt32(double n)
{
	return static_cast<int32_t>(toUint32(n));
}

uint32_t toUint32(double n)
{
	if (std::isnan(n) || std::isinf(n)) {
		return 0;
	}
	double m = std::fmod(std::trunc(n), 4294967296.0);
	if (m < 0) {
		m += 4294967296.0;
	}
	return static_cast<uint32_t>(m);
}

double toInteger(double n)
{
	if (std::isnan(n)) {
		return 0;
	}
	return std::trunc(n);
}

bool strictEquals(const Value& a, const Value& b)
{
	if (a.type() != b.type()) {
		return false;
	}
	switch (a.type()) {
	case Value::Type::Undefined:
	case Value::Type::Null: return true;
	case Value::Type::Boolean: return a.asBoolean() == b.asBoolean();
	case Value::Type::Number: return a.asNumber() == b.asNumber();
	case Value::Type::String: return a.asString() == b.asString();
	case Value::Type::Object: return a.asObject() == b.asObject();
	}
	return false;
}

int64_t arrayIndex(const std::string& name)
{
	if (name.empty() || name.size() > 10 || (name.size() > 1 && name[0] == '0')) {
		return -1;
	}
	int64_t n = 0;
	for (char c : name) {
		if (c < '0' || c > '9') {
			return -1;
		}
		n = n * 10 + (c - '0');
	}
	return n < 4294967295LL ? n : -1;
}

std::u16string utf8ToUtf16(const std::string& s)
{
	std::u16string out;
	out.reserve(s.size());
	size_t i = 0;
	while (i < s.size()) {
		const uint8_t c = static_cast<uint8_t>(s[i]);
		if (c < 0x80) {
			out.push_back(c);
			++i;
			continue;
		}
		uint32_t code = 0;
		size_t extra = 0;
		if (c >= 0xf0 && c < 0xf8) {
			code = c & 0x07, extra = 3;
		} else if (c >= 0xe0 && c < 0xf0) {
			code = c & 0x0f, extra = 2;
		} else if (c >= 0xc0 && c < 0xe0) {
			code = c & 0x1f, extra = 1;
		}
		bool valid = extra > 0 && i + extra < s.size();
		for (size_t k = 1; valid && k <= extra; ++k) {
			const uint8_t next = static_cast<uint8_t>(s[i + k]);
			valid = (next & 0xc0) == 0x80;
			code = code << 6 | (next & 0x3f);
		}
		if (!valid) {
			out.push_back(c); // Latin-1 fallback
			++i;
			continue;
		}
		i += extra + 1;
		if (code >= 0x10000) {
			code -= 0x10000;
			out.push_back(static_cast<char16_t>(0xd800 + (code >> 10)));
			out.push_back(static_cast<char16_t>(0xdc00 + (code & 0x3ff)));
		} else {
			out.push_back(static_cast<char16_t>(code));
		}
	}
	return out;
}

std::string utf16ToUtf8(const std::u16string& s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		uint32_t code = s[i];
		if (code >= 0xd800 && code < 0xdc00 && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
			code = 0x10000 + ((code - 0xd800) << 10) + (s[i + 1] - 0xdc00);
			++i;
		}
		if (code < 0x80) {
			out.push_back(static_cast<char>(code));
		} else if (code < 0x800) {
			out.push_back(static_cast<char>(0xc0 | code >> 6));
			out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
		} else if (code < 0x10000) {
			out.push_back(static_cast<char>(0xe0 | code >> 12));
			out.push_back(static_cast<char>(0x80 | (code >> 6 & 0x3f)));
			out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
		} else {
			out.push_back(static_cast<char>(0xf0 | code >> 18));
			out.push_back(static_cast<char>(0x80 | (code >> 12 & 0x3f)));
			out.push_back(static_cast<char>(0x80 | (code >> 6 & 0x3f)));
			out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
		}
	}
	return out;
}

size_t utf16Length(const std::string& s)
{
	bool ascii = true;
	for (char c : s) {
		ascii = ascii && static_cast<uint8_t>(c) < 0x80;
	}
	return ascii ? s.size() : utf8ToUtf16(s).size();
}

} // namespace dice::hfe::ui::avm1
