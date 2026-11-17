#include "Platform.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace platform {

void showErrorDialog(const char* title, const char* message)
{
	std::fprintf(stderr, "%s: %s\n", title, message);
#ifdef _WIN32
	MessageBoxA(nullptr, message, title, MB_ICONERROR | MB_OK);
#endif
}

void* loadLibrary(const char* name)
{
#ifdef _WIN32
	return reinterpret_cast<void*>(LoadLibraryA(name));
#else
	return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* findSymbol(void* library, const char* name)
{
	if (!library) {
		return nullptr;
	}
#ifdef _WIN32
	return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
	return dlsym(library, name);
#endif
}

void freeLibrary(void* library)
{
	if (!library) {
		return;
	}
#ifdef _WIN32
	FreeLibrary(static_cast<HMODULE>(library));
#else
	dlclose(library);
#endif
}

std::string executableDirectory()
{
	std::string path;
#ifdef _WIN32
	char buffer[MAX_PATH];
	const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
	if (length > 0 && length < MAX_PATH) {
		path.assign(buffer, length);
	}
#elif defined(__APPLE__)
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	path.resize(size);
	if (_NSGetExecutablePath(path.data(), &size) == 0) {
		path.resize(std::strlen(path.c_str()));
	} else {
		path.clear();
	}
#else
	char buffer[4096];
	const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer));
	if (length > 0 && static_cast<size_t>(length) < sizeof(buffer)) {
		path.assign(buffer, static_cast<size_t>(length));
	}
#endif
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

void debugPrint(const char* text)
{
#ifdef _WIN32
	OutputDebugStringA(text);
#endif
	std::fputs(text, stderr);
}

bool restartProcess(const char* program, const std::vector<std::string>& args)
{
	std::vector<char*> argv;
	for (const std::string& arg : args) {
		argv.push_back(const_cast<char*>(arg.c_str()));
	}
	argv.push_back(nullptr);
	std::fflush(stdout);
	std::fflush(stderr);
#ifdef _WIN32
	_execv(program, argv.data());
#else
	execv(program, argv.data());
#endif
	return false;
}

} // namespace platform
