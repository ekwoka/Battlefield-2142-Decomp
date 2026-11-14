// The enhanced mode: additions that aren't in the original game (new render
// passes, client-side physics and audio effects). Off by default, and while
// off the engine behaves as retail. Not a retail console object: `enhanced.*`
// is ours.
//
// Rule for anything gated on it: the simulation, the network state and the
// saved profiles must not change with it. A headless round prints a state
// fingerprint (main.cpp, --simulate); tools/parity-check.sh runs the same
// round with the switch off and on and fails if the fingerprints differ.
// The exceptions are the gameplay additions with their own switches below
// (botRecovery, cornerSteer): they change the simulation by design, so the
// check turns them off (cornerSteer is off by default).
#pragma once

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"

namespace dice::hfe::scene {

struct EnhancedSettings {
	bool enabled = false; // enhanced.enabled
	// The render passes, each only with enhanced.enabled (src/Render/Enhanced):
	bool taa = true;      // enhanced.taa: temporal anti-aliasing (jitter, history)
	bool ssao = true;     // enhanced.ssao: ground-truth ambient occlusion on the ambient term
	bool csm = true;      // enhanced.csm: cascaded sun shadows with PCF
	bool lightRank = true; // enhanced.lightRank: a mesh's point lights rank by distance over reach, so a small flash doesn't displace a wall light (Render/MeshLights.h)
	bool prepass = false; // enhanced.prepass: the depth/normal/motion prepass even when neither taa nor ssao reads it
	// Gameplay, also only with enhanced.enabled; changes the simulation:
	bool botRecovery = true; // enhanced.botRecovery: bots get out of a vehicle that makes no progress (Game/Bots.cpp)
	bool botMovement = true; // enhanced.botMovement: bots on foot lie down only for far targets and don't walk sideways to look about (Game/Bots.cpp, BotsLook.cpp)
	bool botTitan = true; // enhanced.botTitan: bots aboard a Titan walk its decks to the consoles and the core (Game/BotsTitan.cpp)
	bool cornerSteer = false; // enhanced.cornerSteer: experimental; a driver refusing a corner for room steers wider (Game/Bots.cpp)
};

inline void registerEnhancedConsoleMethods(Console& console, EnhancedSettings& settings)
{
	bindProperty<bool>(console, "enhanced.enabled", ConsoleAccess::Anyone, [&settings] { return settings.enabled; },
		[&settings](const bool& v) { settings.enabled = v; });
	bindProperty<bool>(console, "enhanced.taa", ConsoleAccess::Anyone, [&settings] { return settings.taa; },
		[&settings](const bool& v) { settings.taa = v; });
	bindProperty<bool>(console, "enhanced.ssao", ConsoleAccess::Anyone, [&settings] { return settings.ssao; },
		[&settings](const bool& v) { settings.ssao = v; });
	bindProperty<bool>(console, "enhanced.csm", ConsoleAccess::Anyone, [&settings] { return settings.csm; },
		[&settings](const bool& v) { settings.csm = v; });
	bindProperty<bool>(console, "enhanced.lightRank", ConsoleAccess::Anyone, [&settings] { return settings.lightRank; },
		[&settings](const bool& v) { settings.lightRank = v; });
	bindProperty<bool>(console, "enhanced.prepass", ConsoleAccess::Anyone, [&settings] { return settings.prepass; },
		[&settings](const bool& v) { settings.prepass = v; });
	bindProperty<bool>(console, "enhanced.botRecovery", ConsoleAccess::Anyone, [&settings] { return settings.botRecovery; },
		[&settings](const bool& v) { settings.botRecovery = v; });
	bindProperty<bool>(console, "enhanced.botMovement", ConsoleAccess::Anyone, [&settings] { return settings.botMovement; },
		[&settings](const bool& v) { settings.botMovement = v; });
	bindProperty<bool>(console, "enhanced.botTitan", ConsoleAccess::Anyone, [&settings] { return settings.botTitan; },
		[&settings](const bool& v) { settings.botTitan = v; });
	bindProperty<bool>(console, "enhanced.cornerSteer", ConsoleAccess::Anyone, [&settings] { return settings.cornerSteer; },
		[&settings](const bool& v) { settings.cornerSteer = v; });
}

} // namespace dice::hfe::scene
