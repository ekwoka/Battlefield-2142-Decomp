// The platform layer (Platform/Platform.h): the executable's folder, where
// the libraries shipped beside it (Steam Audio's) are loaded from.
#include "Platform/Platform.h"
#include "TestMain.h"

#include <filesystem>

TEST(Platform_ExecutableDirectoryHoldsTheRunningBinary)
{
	const std::string directory = platform::executableDirectory();
	CHECK(!directory.empty());
	CHECK(directory.back() == '/' || directory.back() == '\\');
	const std::filesystem::path self = std::filesystem::path(directory) /
#ifdef _WIN32
		"bf2142_tests.exe";
#else
		"bf2142_tests";
#endif
	CHECK(std::filesystem::exists(self));
}
