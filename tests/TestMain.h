// Minimal self-registering test harness (no external dependencies).
#pragma once

#include <cstdio>
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

struct Registrar {
	Registrar(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

} // namespace test

#define TEST(name)                                                   \
	static void name();                                              \
	static test::Registrar name##_registrar(#name, name);            \
	static void name()

#define CHECK(expr)                                                                    \
	do {                                                                               \
		if (!(expr)) {                                                                 \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
			++test::failures();                                                        \
		}                                                                              \
	} while (0)
