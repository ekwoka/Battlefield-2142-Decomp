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
// (botRecovery, botMovement, botTitan, botCommander, botPilots, guidedCameraEase, cornerSteer,
// passengerWeapons, softLanding, titanKitFloors, squadOrderPoints): they change the simulation by design, so the check turns
// them off (cornerSteer is off by default).
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
	bool staticShadows = true; // enhanced.staticShadows: the terrain and lightmapped surfaces also take the static meshes' and the terrain's cascade shadows, over their baked ones (with csm)
	bool softShadows = true; // enhanced.softShadows: the two nearest cascades' shadows sharp at the caster's contact and wider with its distance (with csm)
	bool titanShadows = true; // enhanced.titanShadows: a Titan's hull casts into the cascades as it moves (with csm)
	bool particleShadows = true; // enhanced.particleShadows: smoke and dust take the cascades' shadows (with csm)
	bool particleLights = true; // enhanced.particleLights: smoke and dust are lit by the frame's point lights (explosions, muzzle flashes, lamps)
	bool glowOcclusion = true; // enhanced.glowOcclusion: a supportsGlow lamp's additive halo is skipped while the line from the camera to its centre is blocked (Render/LevelRenderer.cpp); retail depth-tests halos per pixel only
	bool titanHullLods = true; // enhanced.titanHullLods: a Titan hull half drawn this frame takes its own LOD when the other half was culled, so a far Titan whose front half faded out draws whole (Render/TitanParts.h); retail keeps the culled half's stale LOD
	bool prepass = false; // enhanced.prepass: the depth/normal/motion prepass even when neither taa nor ssao reads it
	// Gameplay, also only with enhanced.enabled; changes the simulation:
	bool botRecovery = true; // enhanced.botRecovery: bots get out of a vehicle that makes no progress (Game/Bots.cpp)
	bool botMovement = true; // enhanced.botMovement: bots on foot lie down only for far targets and don't walk sideways to look about; a gunner's aim starts afresh on a new target (Game/Bots.cpp, BotsLook.cpp)
	bool botTitan = true; // enhanced.botTitan: bots aboard a Titan walk its decks to the consoles and the core; in gpm_ti bots head for the silos (Game/BotsTitan.cpp, Bots.cpp)
	bool botPilots = true; // enhanced.botPilots: a gunship's bot pilot picks the best target it can hurt (vehicles, infantry groups, aircraft, a Titan's gun hubs) and flies attack runs at it (Game/BotsPilot.cpp)
	bool botCommander = true; // enhanced.botCommander: in gpm_ti a team with bots and no commander has its Titan moved and its commander's tools used for it (Game/BotsTitan.cpp)
	bool guidedCameraEase = true; // enhanced.guidedCameraEase: a guided round's camera eases back to centre instead of snapping (Game/GameWorld.cpp)
	bool cornerSteer = false; // enhanced.cornerSteer: experimental; a driver refusing a corner for room steers wider (Game/Bots.cpp)
	bool passengerWeapons = true; // enhanced.passengerWeapons: an air transport's passengers fire their own weapons, the pod on the exit key only (GameWorld::passengerWeapon)
	bool softLanding = true; // enhanced.softLanding: a soldier landing on or walking into a slower object takes the ground's fall damage, not a road kill (Game/MeshCollision.cpp)
	bool titanKitFloors = true; // enhanced.titanKitFloors: a kit dropped (or a beacon or shield placed) on a Titan stays on its walkable floor where the projectile mesh has none, as by the cargo bay's walls (GameWorld::surfaceBelow); retail lets it fall through
	bool squadOrderPoints = true; // enhanced.squadOrderPoints: the squads' order points, their HUD bar and its field upgrades work on any server (GameWorld::squadOrderPointsOn); retail runs them only on a ranked GameSpy server (0x00430a00), so never offline
	// Interface, also only with enhanced.enabled:
	bool chaseRoll = false; // enhanced.chaseRoll: a flying vehicle's chase and front-chase views bank with its hull (Main/PlayerCamera.cpp); off by default, as retail keeps them level (0x006108e0 -> lookAt 0x006ae080, up (0, 1, 0))
	bool killWeaponNames = true; // enhanced.killWeaponNames: a kill from a vehicle without a hudName (a Titan's gun hubs) names the seat's gun (Main/main.cpp); retail prints empty brackets
	bool hudWidescreen = false; // enhanced.hudWidescreen: a mod without Remaster's hudfix gets its 4:3 HUD and side moves (Main/main.cpp)
	bool singleplayAllModes = true; // enhanced.singleplayAllModes: the singleplayer page lists every level's every mode and size (Titan, Assault Lines, ...), not only its gpm_sp layouts (Main/MenuBindings.cpp); the bots may not play them. Retail lists gpm_sp only
	// Drawing, also only with enhanced.enabled; experimental, off by default:
	bool tvEffect = false; // enhanced.tvEffect: the guided round's view gets the TVEffect pass (grey, rolled by noise bands, shifting rows sideways so vertical edges lean); RendDX9 never runs it, retail shows the plain view
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
	bindProperty<bool>(console, "enhanced.staticShadows", ConsoleAccess::Anyone, [&settings] { return settings.staticShadows; },
		[&settings](const bool& v) { settings.staticShadows = v; });
	bindProperty<bool>(console, "enhanced.softShadows", ConsoleAccess::Anyone, [&settings] { return settings.softShadows; },
		[&settings](const bool& v) { settings.softShadows = v; });
	bindProperty<bool>(console, "enhanced.titanShadows", ConsoleAccess::Anyone, [&settings] { return settings.titanShadows; },
		[&settings](const bool& v) { settings.titanShadows = v; });
	bindProperty<bool>(console, "enhanced.particleShadows", ConsoleAccess::Anyone, [&settings] { return settings.particleShadows; },
		[&settings](const bool& v) { settings.particleShadows = v; });
	bindProperty<bool>(console, "enhanced.particleLights", ConsoleAccess::Anyone, [&settings] { return settings.particleLights; },
		[&settings](const bool& v) { settings.particleLights = v; });
	bindProperty<bool>(console, "enhanced.glowOcclusion", ConsoleAccess::Anyone, [&settings] { return settings.glowOcclusion; },
		[&settings](const bool& v) { settings.glowOcclusion = v; });
	bindProperty<bool>(console, "enhanced.titanHullLods", ConsoleAccess::Anyone, [&settings] { return settings.titanHullLods; },
		[&settings](const bool& v) { settings.titanHullLods = v; });
	bindProperty<bool>(console, "enhanced.prepass", ConsoleAccess::Anyone, [&settings] { return settings.prepass; },
		[&settings](const bool& v) { settings.prepass = v; });
	bindProperty<bool>(console, "enhanced.botRecovery", ConsoleAccess::Anyone, [&settings] { return settings.botRecovery; },
		[&settings](const bool& v) { settings.botRecovery = v; });
	bindProperty<bool>(console, "enhanced.botMovement", ConsoleAccess::Anyone, [&settings] { return settings.botMovement; },
		[&settings](const bool& v) { settings.botMovement = v; });
	bindProperty<bool>(console, "enhanced.botTitan", ConsoleAccess::Anyone, [&settings] { return settings.botTitan; },
		[&settings](const bool& v) { settings.botTitan = v; });
	bindProperty<bool>(console, "enhanced.botPilots", ConsoleAccess::Anyone, [&settings] { return settings.botPilots; },
		[&settings](const bool& v) { settings.botPilots = v; });
	bindProperty<bool>(console, "enhanced.botCommander", ConsoleAccess::Anyone, [&settings] { return settings.botCommander; },
		[&settings](const bool& v) { settings.botCommander = v; });
	bindProperty<bool>(console, "enhanced.guidedCameraEase", ConsoleAccess::Anyone, [&settings] { return settings.guidedCameraEase; },
		[&settings](const bool& v) { settings.guidedCameraEase = v; });
	bindProperty<bool>(console, "enhanced.cornerSteer", ConsoleAccess::Anyone, [&settings] { return settings.cornerSteer; },
		[&settings](const bool& v) { settings.cornerSteer = v; });
	bindProperty<bool>(console, "enhanced.passengerWeapons", ConsoleAccess::Anyone, [&settings] { return settings.passengerWeapons; },
		[&settings](const bool& v) { settings.passengerWeapons = v; });
	bindProperty<bool>(console, "enhanced.softLanding", ConsoleAccess::Anyone, [&settings] { return settings.softLanding; },
		[&settings](const bool& v) { settings.softLanding = v; });
	bindProperty<bool>(console, "enhanced.titanKitFloors", ConsoleAccess::Anyone, [&settings] { return settings.titanKitFloors; },
		[&settings](const bool& v) { settings.titanKitFloors = v; });
	bindProperty<bool>(console, "enhanced.squadOrderPoints", ConsoleAccess::Anyone, [&settings] { return settings.squadOrderPoints; },
		[&settings](const bool& v) { settings.squadOrderPoints = v; });
	bindProperty<bool>(console, "enhanced.chaseRoll", ConsoleAccess::Anyone, [&settings] { return settings.chaseRoll; },
		[&settings](const bool& v) { settings.chaseRoll = v; });
	bindProperty<bool>(console, "enhanced.killWeaponNames", ConsoleAccess::Anyone, [&settings] { return settings.killWeaponNames; },
		[&settings](const bool& v) { settings.killWeaponNames = v; });
	bindProperty<bool>(console, "enhanced.hudWidescreen", ConsoleAccess::Anyone, [&settings] { return settings.hudWidescreen; },
		[&settings](const bool& v) { settings.hudWidescreen = v; });
	bindProperty<bool>(console, "enhanced.singleplayAllModes", ConsoleAccess::Anyone, [&settings] { return settings.singleplayAllModes; },
		[&settings](const bool& v) { settings.singleplayAllModes = v; });
	bindProperty<bool>(console, "enhanced.tvEffect", ConsoleAccess::Anyone, [&settings] { return settings.tvEffect; },
		[&settings](const bool& v) { settings.tvEffect = v; });
}

} // namespace dice::hfe::scene
