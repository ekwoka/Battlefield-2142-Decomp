// Typed console methods and properties.
//
// The original declares each console method as an object whose argument
// and return types are template parameters. Strings are passed through
// whole; other arguments are parsed with `istream >> value`, and results are
// printed with `ostream << value`. Vectors are written "x/y/z". The helpers below register plain C++ functions with
// the same conversions:
//
//   bindMethod(console, "renderer.drawHud", access, [](bool on) { ... });
//   bindProperty<float>(console, "audioSettings.voipPlaybackVolume", access,
//       [] { return volume; }, [](float v) { volume = v; });
#pragma once

#include "Console.h"

#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace dice::hfe {

struct ConsoleVec2 {
	float x = 0, y = 0;
};
struct ConsoleVec3 {
	float x = 0, y = 0, z = 0;
};
struct ConsoleVec4 {
	float x = 0, y = 0, z = 0, w = 0;
};

namespace console_detail {

// Reads "a/b/c": a number, any one separator character, a number, ...
template <size_t N>
void readFloats(std::istream& in, float (&values)[N])
{
	for (size_t i = 0; i < N; ++i) {
		in >> values[i];
		if (i + 1 < N) {
			char separator;
			in >> separator;
		}
	}
}

template <size_t N>
void writeFloats(std::ostream& out, const float (&values)[N])
{
	for (size_t i = 0; i < N; ++i) {
		out << values[i];
		if (i + 1 < N) {
			out << '/';
		}
	}
}

} // namespace console_detail

// A value that can't be parsed keeps its default, as in the original.
template <class T>
T parseConsoleValue(const std::string& text)
{
	if constexpr (std::is_same_v<T, std::string>) {
		return text; // strings are passed through whole, spaces included
	}
	std::istringstream in(text);
	T value {};
	if constexpr (std::is_same_v<T, ConsoleVec2>) {
		float v[2] = {};
		console_detail::readFloats(in, v);
		value = {v[0], v[1]};
	} else if constexpr (std::is_same_v<T, ConsoleVec3>) {
		float v[3] = {};
		console_detail::readFloats(in, v);
		value = {v[0], v[1], v[2]};
	} else if constexpr (std::is_same_v<T, ConsoleVec4>) {
		float v[4] = {};
		console_detail::readFloats(in, v);
		value = {v[0], v[1], v[2], v[3]};
	} else if constexpr (std::is_same_v<T, bool>) {
		int number = 0; // "1"/"0"; MSVC's bool extraction is numeric
		in >> number;
		value = number != 0;
	} else {
		in >> value;
	}
	return value;
}

template <class T>
std::string formatConsoleValue(const T& value)
{
	std::ostringstream out;
	if constexpr (std::is_same_v<T, ConsoleVec2>) {
		const float v[2] = {value.x, value.y};
		console_detail::writeFloats(out, v);
	} else if constexpr (std::is_same_v<T, ConsoleVec3>) {
		const float v[3] = {value.x, value.y, value.z};
		console_detail::writeFloats(out, v);
	} else if constexpr (std::is_same_v<T, ConsoleVec4>) {
		const float v[4] = {value.x, value.y, value.z, value.w};
		console_detail::writeFloats(out, v);
	} else if constexpr (std::is_same_v<T, bool>) {
		out << (value ? 1 : 0);
	} else {
		out << value;
	}
	return out.str();
}

namespace console_detail {

template <class F>
struct Signature : Signature<decltype(&F::operator())> {};
template <class C, class R, class... A>
struct Signature<R (C::*)(A...) const> {
	using Result = R;
	using Args = std::tuple<std::decay_t<A>...>;
};
template <class C, class R, class... A>
struct Signature<R (C::*)(A...)> : Signature<R (C::*)(A...) const> {};
template <class R, class... A>
struct Signature<R (*)(A...)> {
	using Result = R;
	using Args = std::tuple<std::decay_t<A>...>;
};

template <class R, class Args, class F, size_t... I>
std::string invoke(F& function, const std::vector<std::string>& args, std::index_sequence<I...>)
{
	// Missing optional arguments parse from "" and keep their defaults.
	const auto arg = [&](size_t i) -> const std::string& {
		static const std::string empty;
		return i < args.size() ? args[i] : empty;
	};
	if constexpr (std::is_void_v<R>) {
		function(parseConsoleValue<std::tuple_element_t<I, Args>>(arg(I))...);
		return {};
	} else {
		return formatConsoleValue(function(parseConsoleValue<std::tuple_element_t<I, Args>>(arg(I))...));
	}
}

} // namespace console_detail

// Registers a method taking typed arguments. `minArgs` defaults to all of them.
template <class F>
void bindMethod(Console& console, const std::string& name, uint32_t access, F function, int minArgs = -1)
{
	using Sig = console_detail::Signature<F>;
	constexpr size_t count = std::tuple_size_v<typename Sig::Args>;
	ConsoleMethod method;
	method.name = name;
	method.access = access;
	method.minArgs = minArgs < 0 ? static_cast<int>(count) : minArgs;
	method.maxArgs = static_cast<int>(count);
	method.invoke = [function](const std::vector<std::string>& args) mutable {
		return console_detail::invoke<typename Sig::Result, typename Sig::Args>(function, args,
			std::make_index_sequence<count>());
	};
	console.registerMethod(std::move(method));
}

// Registers a property: no argument reads it, one argument writes it.
template <class T>
void bindProperty(Console& console, const std::string& name, uint32_t access, std::function<T()> get,
	std::function<void(const T&)> set)
{
	ConsoleMethod method;
	method.name = name;
	method.access = access;
	method.kind = !set ? ConsoleMethodKind::ReadOnlyProperty
		: !get         ? ConsoleMethodKind::WriteOnlyProperty
					   : ConsoleMethodKind::ReadWriteProperty;
	method.minArgs = 0;
	method.maxArgs = set ? 1 : 0;
	method.invoke = [get, set](const std::vector<std::string>& args) -> std::string {
		if (args.empty()) {
			return get ? formatConsoleValue(get()) : std::string();
		}
		set(parseConsoleValue<T>(args[0]));
		return {};
	};
	console.registerMethod(std::move(method));
}

} // namespace dice::hfe
