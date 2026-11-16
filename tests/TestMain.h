// Minimal self-registering test harness (no external dependencies).
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

namespace test {

struct Case {
	const char* name;
	std::function<void()> body;
};

inline std::vector<Case>& registry()
{
	static std::vector<Case> cases;
	return cases;
}

inline int& failures()
{
	static int count = 0;
	return count;
}

// Sets an environment variable for the rest of the run (setenv has no Windows twin).
inline void setEnv(const char* name, const char* value)
{
#ifdef _WIN32
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

inline void unsetEnv(const char* name)
{
#ifdef _WIN32
	_putenv_s(name, "");
#else
	unsetenv(name);
#endif
}

// Project Remaster is a mod, not part of the game: its tests run only with
// --remaster (or BF2142_REMASTER_TESTS=1) and then need it under the game's mods.
inline bool& remaster()
{
	static bool on = false;
	return on;
}

// Set by a test that returned without running (REMASTER_ONLY).
inline bool& skipped()
{
	static bool s = false;
	return s;
}

// Whether a pass over `mod` runs: every mod's, Remaster's only with the flag.
inline bool runs(const char* mod)
{
	return remaster() || !mod || !std::strstr(mod, "Project_Remaster");
}

template <typename String>
inline bool runs(const String& mod)
{
	return runs(mod.c_str());
}

struct Registrar {
	Registrar(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

} // namespace test

#define TEST(name)                                                   \
	static void name();                                              \
	static test::Registrar name##_registrar(#name, name);            \
	static void name()

// The test needs Project Remaster: skipped without --remaster.
#define REMASTER_ONLY()                \
	do {                               \
		if (!test::remaster()) {       \
			test::skipped() = true;    \
			return;                    \
		}                              \
	} while (0)

#define CHECK(expr)                                                                    \
	do {                                                                               \
		if (!(expr)) {                                                                 \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
			++test::failures();                                                        \
		}                                                                              \
	} while (0)
