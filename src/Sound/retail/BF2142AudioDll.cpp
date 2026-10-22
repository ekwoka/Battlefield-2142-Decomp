// Drop-in replacement for the retail BF2142Audio.dll. The game loads it with
// LoadLibrary and resolves these two C exports; everything else goes through
// the ISoundEngine interface.
#include "Sound/SoundEngine/ISoundEngine.h"

#include <windows.h>

namespace snd = dice::hfe::sound;

extern "C" __declspec(dllexport) snd::ISoundEngine* __cdecl getSoundEngine()
{
	return snd::getSoundEngine();
}

extern "C" __declspec(dllexport) void __cdecl shutdownSoundEngine()
{
	snd::shutdownSoundEngine();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(instance);
	}
	return TRUE;
}
