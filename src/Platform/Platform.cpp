#include "Platform.h"

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
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

} // namespace platform
