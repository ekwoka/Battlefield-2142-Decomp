#include "Platform.h"

#include <cstdio>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
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
