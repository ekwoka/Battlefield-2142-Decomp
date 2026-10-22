#include "TestMain.h"

#include <cstring>

// Runs every test, or those whose names contain the first argument.
int main(int argc, char** argv)
{
	const char* filter = argc > 1 ? argv[1] : nullptr;
	for (const test::Case& c : test::registry()) {
		if (filter && !std::strstr(c.name, filter)) {
			continue;
		}
		const int before = test::failures();
		c.body();
		std::printf("%s %s\n", test::failures() == before ? "PASS" : "FAIL", c.name);
	}
	return test::failures() == 0 ? 0 : 1;
}
