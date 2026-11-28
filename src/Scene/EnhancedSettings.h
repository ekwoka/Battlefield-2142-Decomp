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
// (botRecovery, botMovement, botTitan, botOffMesh, botAsyncPaths, botCommander, botPilots, botLoneSeat, botThreat, botTitanRepair, guidedCameraEase, cornerSteer,
// passengerWeapons, softLanding, titanKitFloors, titanDeckMines, podExitAtRest, squadOrderPoints, botSquadInvites): they change the simulation by design, so the check turns
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
	bool actorRange = true; // enhanced.actorRange: soldiers and vehicles are drawn out to the view distance and fade with the fog (Render/DistanceFade.h actorRangeRecord); retail drops them by their screen area, a soldier at ~176 m unzoomed, inside every level's fog and every weapon's reach
	bool prepass = false; // enhanced.prepass: the depth/normal/motion prepass even when neither taa nor ssao reads it
	// Gameplay, also only with enhanced.enabled; changes the simulation:
	bool botRecovery = true; // enhanced.botRecovery: bots get out of a vehicle that makes no progress (Game/Bots.cpp)
	bool botMovement = true; // enhanced.botMovement: bots on foot lie down only for far targets and don't walk sideways to look about; a gunner's aim starts afresh on a new target (Game/Bots.cpp, BotsLook.cpp)
	bool botTitan = true; // enhanced.botTitan: bots aboard a Titan walk its decks to the consoles and the core, man its guns and fire them (Bots.cpp titanGunWeaponAi), and are fought at the gun's hub (titanGunnerAt); in gpm_ti bots head for the silos (Game/BotsTitan.cpp, Bots.cpp)
	bool botOffMesh = true; // enhanced.botOffMesh: a bot on foot where the level's infantry mesh doesn't reach (the 32/64 layouts' spawns and points beyond the mesh made for the 16 one) walks a terrain grid to its goal or onto the mesh (Game/BotsTitan.cpp groundWalk), to a vehicle it heads for too (a walker, its origin over 3 m up: to the ground under it); retail's bots stand there
	bool botAsyncPaths = true; // enhanced.botAsyncPaths: the bots' costly Titan-deck ways (BotsTitan.cpp titanField) are made at the load and, when a part goes down or comes back, on worker threads while the bots walk the last ones; the vehicle-mesh search is spread over ticks. The round plays the same whatever the threads' timing
	bool botPilots = true; // enhanced.botPilots: a gunship's bot pilot picks the best target it can hurt (vehicles, infantry groups, aircraft, a Titan's gun hubs) and flies attack runs at it, evades a lock or a heavy hit and flies off to be mended when badly damaged (Game/BotsPilot.cpp)
	bool botCommander = true; // enhanced.botCommander: in gpm_ti a team with bots and no commander has its Titan moved and its commander's tools used for it (Game/BotsTitan.cpp)
	bool botLoneSeat = true; // enhanced.botLoneSeat: a bot left alone in a passenger or gunner seat of a vehicle with nobody driving takes the free driver's seat after 2 s, or gets out after 6 s; a bot driving alone doesn't move to its own vehicle's gun seat (Game/Bots.cpp loneSeat). Retail's sit there for good
	bool botThreat = true; // enhanced.botThreat: a player who has killed lately (a busy Titan gun, a tank, a gunship) weighs more in the target choice of every bot whose weapons can hurt it, and of the gunship pilots' hunt (Game/Bots.cpp threat); retail's target scores know no kills
	bool botTitanRepair = true; // enhanced.botTitanRepair: in gpm_ti an engineer bot respawns on its Titan while one of its guns is down and no engineer is aboard, and a gunner gets out of a gun that is a wreck (Game/BotsTitan.cpp); retail's bots never mend a Titan
	bool guidedCameraEase = true; // enhanced.guidedCameraEase: a guided round's camera eases back to centre instead of snapping (Game/GameWorld.cpp)
	bool cornerSteer = false; // enhanced.cornerSteer: experimental; a driver refusing a corner for room steers wider (Game/Bots.cpp)
	bool passengerWeapons = true; // enhanced.passengerWeapons: an air transport's passengers fire their own weapons, the pod on the exit key only (GameWorld::passengerWeapon)
	bool softLanding = true; // enhanced.softLanding: a soldier landing on or walking into a slower object takes the ground's fall damage, not a road kill (Game/MeshCollision.cpp)
	bool titanKitFloors = true; // enhanced.titanKitFloors: a kit dropped (or a beacon or shield placed) on a Titan stays on its walkable floor where the projectile mesh has none, as by the cargo bay's walls (GameWorld::surfaceBelow), and a shield or sentry gun set down on a Titan's floor starts with its bottom on it (GameWorld::addBodies); retail lets them fall through
	bool podExitAtRest = true; // enhanced.podExitAtRest: the rider of a landed drop pod leaves it at rest on what it landed on, not at the pod's velocity (Game/Titans.cpp stepPods); retail's occupant exit (0x005cfd70) hands him the pod's, so a pod still sliding over a Titan's deck throws him off it
	bool titanDeckMines = true; // enhanced.titanDeckMines: a Titan doesn't set off the mines lying on its own decks (GameWorld::stepProjectiles); retail's radius trigger (0x005e8aa0) takes the driverless moving Titan for a vehicle, so a motion or EMP mine on its deck goes off when it moves
	bool squadOrderPoints = true; // enhanced.squadOrderPoints: the squads' order points, their HUD bar and its field upgrades work on any server (GameWorld::squadOrderPointsOn); retail runs them only on a ranked GameSpy server (0x00430a00), so never offline
	bool botSquadInvites = true; // enhanced.botSquadInvites: offline, a bot the squad leader invites (SquadMenu.applyInviteList) joins his squad, so a human can lead bots and use the squad leader's items; retail's bots never answer an invite (0x004c3190 only sets a mask bit) and join squads only as they are added
	// Interface, also only with enhanced.enabled:
	bool chaseRoll = false; // enhanced.chaseRoll: a flying vehicle's chase and front-chase views bank with its hull (Main/PlayerCamera.cpp); off by default, as retail keeps them level (0x006108e0 -> lookAt 0x006ae080, up (0, 1, 0))
	bool followFirstPerson = true; // enhanced.followFirstPerson: --director's first-person shots and a demo's followed player (--demo-first-person) show that player's own first person: his arms and weapon animated from his state, his zoom, his seat's camera and cockpit, and (J / --follow-hud) his HUD (Main/PlayerCamera.h setSubject, HudWorld::viewedPlayer); retail's spectator (0x00614d00) and demo (0x005f8f40) follow cameras are chases behind the player and show no HUD of his
	bool killWeaponNames = true; // enhanced.killWeaponNames: a kill from a vehicle without a hudName (a Titan's gun hubs) names the seat's gun (Main/main.cpp); retail prints empty brackets
	bool squadItemHints = true; // enhanced.squadItemHints: a weapon key refused for the squad says why on the help line each time (Main/HudAlerts.h squadItemRefusal): a squad leader's item for one who leads no squad, or a squad too small for it with its count, "(1/2)"; retail is silent for the first and shows the second (help message 0x52) once, with help on
	bool hudWidescreen = false; // enhanced.hudWidescreen: a mod without Remaster's hudfix gets its 4:3 HUD and side moves (Main/main.cpp)
	bool singleplayAllModes = true; // enhanced.singleplayAllModes: the singleplayer page lists every level's every mode and size (Titan, Assault Lines, ...), not only its gpm_sp layouts (Main/MenuBindings.cpp); the bots may not play them. Retail lists gpm_sp only
	bool demoQuality = true; // enhanced.demoQuality: a recorded demo's ghosts go every tick, several packets a tick when one is full (net::EnhancedDemoRates), so a 64-player round plays back smoothly; retail's sv.demoQuality 1..10 sends 4..20 packets a second of 40..255 updates, which starves most ghosts of a big round. The file stays readable (a payload under the reader's 256 KB, at most 255 updates a block)
	bool singleplayBotCount = true; // enhanced.singleplayBotCount: the singleplayer page gets a players slider beside the bot skill (Main/MenuSingleplayBots.h) and a game it starts fills each map with min(target, the map's size) players, you included (Main/MenuBindings.cpp playersForMap). Retail's page plays 16 on every map
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
	bindProperty<bool>(console, "enhanced.actorRange", ConsoleAccess::Anyone, [&settings] { return settings.actorRange; },
		[&settings](const bool& v) { settings.actorRange = v; });
	bindProperty<bool>(console, "enhanced.prepass", ConsoleAccess::Anyone, [&settings] { return settings.prepass; },
		[&settings](const bool& v) { settings.prepass = v; });
	bindProperty<bool>(console, "enhanced.botRecovery", ConsoleAccess::Anyone, [&settings] { return settings.botRecovery; },
		[&settings](const bool& v) { settings.botRecovery = v; });
	bindProperty<bool>(console, "enhanced.botMovement", ConsoleAccess::Anyone, [&settings] { return settings.botMovement; },
		[&settings](const bool& v) { settings.botMovement = v; });
	bindProperty<bool>(console, "enhanced.botTitan", ConsoleAccess::Anyone, [&settings] { return settings.botTitan; },
		[&settings](const bool& v) { settings.botTitan = v; });
	bindProperty<bool>(console, "enhanced.botOffMesh", ConsoleAccess::Anyone, [&settings] { return settings.botOffMesh; },
		[&settings](const bool& v) { settings.botOffMesh = v; });
	bindProperty<bool>(console, "enhanced.botAsyncPaths", ConsoleAccess::Anyone, [&settings] { return settings.botAsyncPaths; },
		[&settings](const bool& v) { settings.botAsyncPaths = v; });
	bindProperty<bool>(console, "enhanced.botPilots", ConsoleAccess::Anyone, [&settings] { return settings.botPilots; },
		[&settings](const bool& v) { settings.botPilots = v; });
	bindProperty<bool>(console, "enhanced.botCommander", ConsoleAccess::Anyone, [&settings] { return settings.botCommander; },
		[&settings](const bool& v) { settings.botCommander = v; });
	bindProperty<bool>(console, "enhanced.botLoneSeat", ConsoleAccess::Anyone, [&settings] { return settings.botLoneSeat; },
		[&settings](const bool& v) { settings.botLoneSeat = v; });
	bindProperty<bool>(console, "enhanced.botThreat", ConsoleAccess::Anyone, [&settings] { return settings.botThreat; },
		[&settings](const bool& v) { settings.botThreat = v; });
	bindProperty<bool>(console, "enhanced.botTitanRepair", ConsoleAccess::Anyone, [&settings] { return settings.botTitanRepair; },
		[&settings](const bool& v) { settings.botTitanRepair = v; });
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
	bindProperty<bool>(console, "enhanced.titanDeckMines", ConsoleAccess::Anyone, [&settings] { return settings.titanDeckMines; },
		[&settings](const bool& v) { settings.titanDeckMines = v; });
	bindProperty<bool>(console, "enhanced.podExitAtRest", ConsoleAccess::Anyone, [&settings] { return settings.podExitAtRest; },
		[&settings](const bool& v) { settings.podExitAtRest = v; });
	bindProperty<bool>(console, "enhanced.squadOrderPoints", ConsoleAccess::Anyone, [&settings] { return settings.squadOrderPoints; },
		[&settings](const bool& v) { settings.squadOrderPoints = v; });
	bindProperty<bool>(console, "enhanced.botSquadInvites", ConsoleAccess::Anyone, [&settings] { return settings.botSquadInvites; },
		[&settings](const bool& v) { settings.botSquadInvites = v; });
	bindProperty<bool>(console, "enhanced.chaseRoll", ConsoleAccess::Anyone, [&settings] { return settings.chaseRoll; },
		[&settings](const bool& v) { settings.chaseRoll = v; });
	bindProperty<bool>(console, "enhanced.followFirstPerson", ConsoleAccess::Anyone, [&settings] { return settings.followFirstPerson; },
		[&settings](const bool& v) { settings.followFirstPerson = v; });
	bindProperty<bool>(console, "enhanced.killWeaponNames", ConsoleAccess::Anyone, [&settings] { return settings.killWeaponNames; },
		[&settings](const bool& v) { settings.killWeaponNames = v; });
	bindProperty<bool>(console, "enhanced.squadItemHints", ConsoleAccess::Anyone, [&settings] { return settings.squadItemHints; },
		[&settings](const bool& v) { settings.squadItemHints = v; });
	bindProperty<bool>(console, "enhanced.hudWidescreen", ConsoleAccess::Anyone, [&settings] { return settings.hudWidescreen; },
		[&settings](const bool& v) { settings.hudWidescreen = v; });
	bindProperty<bool>(console, "enhanced.singleplayAllModes", ConsoleAccess::Anyone, [&settings] { return settings.singleplayAllModes; },
		[&settings](const bool& v) { settings.singleplayAllModes = v; });
	bindProperty<bool>(console, "enhanced.singleplayBotCount", ConsoleAccess::Anyone, [&settings] { return settings.singleplayBotCount; },
		[&settings](const bool& v) { settings.singleplayBotCount = v; });
	bindProperty<bool>(console, "enhanced.demoQuality", ConsoleAccess::Anyone, [&settings] { return settings.demoQuality; },
		[&settings](const bool& v) { settings.demoQuality = v; });
	bindProperty<bool>(console, "enhanced.tvEffect", ConsoleAccess::Anyone, [&settings] { return settings.tvEffect; },
		[&settings](const bool& v) { settings.tvEffect = v; });
}

} // namespace dice::hfe::scene
