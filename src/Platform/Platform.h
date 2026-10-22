// Thin platform layer. Everything that the original code did through Win32
// directly goes through here so the engine stays portable.
#pragma once

namespace platform {

// Shows a blocking error dialog (or prints to stderr when no UI is available).
void showErrorDialog(const char* title, const char* message);

// Dynamic libraries (LoadLibrary/GetProcAddress, dlopen/dlsym).
void* loadLibrary(const char* name);
void* findSymbol(void* library, const char* name);
void freeLibrary(void* library);

// Prints to the debugger output (OutputDebugString) or stderr.
void debugPrint(const char* text);

} // namespace platform
