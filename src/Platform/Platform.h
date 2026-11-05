// Thin platform layer. Everything that the original code did through Win32
// directly goes through here so the engine stays portable.
#pragma once

#include <string>
#include <vector>

namespace platform {

// Shows a blocking error dialog (or prints to stderr when no UI is available).
void showErrorDialog(const char* title, const char* message);

// Dynamic libraries (LoadLibrary/GetProcAddress, dlopen/dlsym).
void* loadLibrary(const char* name);
void* findSymbol(void* library, const char* name);
void freeLibrary(void* library);

// Replaces this process with `program` run on `args` (args[0] the
// program's name): execv, _execv on Windows. Returns only on failure.
bool restartProcess(const char* program, const std::vector<std::string>& args);

// Prints to the debugger output (OutputDebugString) or stderr.
void debugPrint(const char* text);

} // namespace platform
