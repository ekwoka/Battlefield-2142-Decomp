# bf2142-recomp

This project decompiles **Battlefield 2142** (DICE/EA, 2006; Windows XP era,
patch 1.51) into maintainable C++ that can be recompiled natively for
**macOS**, with Apple Silicon as the main target. macOS comes first, but we
don't make choices that would rule out Windows or Linux. Where a
cross-platform library can do the job (SDL3, OpenAL Soft, FFmpeg and so on),
we use it rather than a Mac-only API such as Metal.

Online play targets [OpenSpy](https://openspy.net), the community replacement
for the GameSpy/EA backends. Network code follows OpenSpy's published source
and patches rather than re-reversing protocols from scratch (see
[Networking](#networking-openspy)).

[OpenBattlefield2](https://github.com/Moxnatiy/OpenBattlefield2) (MIT) is a
clean-room reimplementation of DICE's Refractor 2 engine for Battlefield 2,
the engine BF2142 is built on. We use it as a **reference implementation** to
cross-check details such as file formats, soldier physics, animation, the HUD's
node tree and the network ghost records. It is not an exact match for BF2142:
the games differ (2142 adds Titans, its own HUD and its own network records),
and it is still in progress. The BF2142 binaries stay the authority, so check
every detail against them and cite the BF2142 address, not the reference.
Clone it into the git-ignored `reference/` folder:

```sh
git clone https://github.com/Moxnatiy/OpenBattlefield2 reference/OpenBattlefield2
```

> **Status:** playable offline. The native `bf2142` runs the game's own menus
> and starts single-player, co-op, conquest, NV and SL rounds with bots. It
> also has first-person view, vehicles, physics, the HUD, the commander, stats
> and a network client. Titan mode runs (Titans, silos and missiles, shields,
> the core's consoles, the commander's moves, drop pods; see
> [docs/engine.md](docs/engine.md#titans-gpm_ti)); several rendering details are still in
> progress (see [Progress](#progress) and [docs/roadmap.md](docs/roadmap.md)).

## Legal

This repository contains **no game code or assets** and never will. You need
your own legitimately obtained copy of BF2142. `source/` (the game install) and
`build/` (Ghidra projects and analysis output) are git-ignored. Don't commit
anything taken from the retail files, such as decompiler output dumps, game
archives, or shaders copied verbatim. Commit only our own source, symbol and
annotation files, and tools.

## Repository layout

```
source/Battlefield 2142/   your game install, BF2142 1.51 + OpenSpy patch (git-ignored)
build/                     build trees, Ghidra project, analysis output (git-ignored)
reference/OpenBattlefield2/ the Refractor 2 reference implementation (git-ignored clone)
src/                       the rewrite, laid out like DICE's Code/BF2 tree
  Platform/                OS abstraction (dialogs, dynamic libraries, ...)
  Memory/                  dice::hfe::memory (was Memory.dll)
  IO/File/                 virtual file system: ZIP archives, search paths, @ROOT@/@HOME@
  Console/                 console script interpreter (.con/.inc/.tweak files)
  World/                   object templates and objects
  Geometry/                mesh files and geometry templates
  Texture/                 DDS textures
  Terrain/                 heightmaps
  Animation/               skeletons (.ske), bone animations (.baf), animation systems (trigger trees)
  Physics/                 collision meshes (.collisionmesh)
  Render/                  SDL3 GPU renderer (shaders in Render/Shaders, GLSL)
  Game/                    engine core: simulation clock, random numbers, rounds, spawners, soldiers, weapons,
                           vehicles, game logic (teams, tickets) and what the scripts see of the game
  Python/                  the scripts' host module on an embedded Python 3 (was dice_py.dll's Python 2.3),
                           with an import-time Python 2 translator (py2compat.py)
  Effects/                 effect bundles and particle systems
  AI/                      bots' navigation mesh and path finding (AIDLL.dll)
  UI/                      the Flash menus: .dfm/SWF, AVM1 interpreter, characters, player
  Hud/                     the HUD: hudBuilder node trees, HUD variables, .dif fonts
  Net/                     networking: GameSpy server browsing against OpenSpy, the game's UDP connections
  Voip/                    voice chat: Speex, the voice client and the voice server (was BF2VoipServer.dll)
  Main/                    the bf2142 executable (start-up sequence and level viewer so far)
  Sound/SoundEngine/       the sound engine (was BF2142Audio.dll)
  */retail/                drop-in replacements for the retail DLLs (32-bit Windows builds)
tests/                     unit and integration tests
symbols/<module>.csv       hand-maintained symbols applied to the Ghidra project
symbols/<module>.console.csv  console methods and signatures (tools/decomp/console_methods.py)
symbols/BF2142.exe.templates.csv            template types and class IDs
symbols/BF2142.exe.template_properties.csv  template properties -> class/component, fields (tools/decomp/template_properties.py)
symbols/BF2142.exe.singletons.csv           named global managers and their classes (tools/decomp/singletons.py)
tools/ghidra/              headless analysis driver and Ghidra scripts
tools/decomp/              helpers for reading the decompiled output
tools/retail-*.sh          swap rewritten DLLs into a CrossOver bottle and smoke-test the game
docs/research.md           survey of decompilation techniques and how they apply
docs/binaries.md           module inventory, third-party replacements, OpenSpy patch details
docs/workflow.md           step-by-step: analyse, read, rewrite, verify (plus ABI rules)
docs/console.md            the console script language (.con files) and its quirks
docs/roadmap.md            milestones, what is done, what is next
docs/engine.md             the executable's start-up, frame and level-loading order
```

## Getting started

You'll need the game folder after installing the base game, the 1.51 patch, and the openspy patches from bf hub, as described [Here](https://docs.getbf2142.net/getting-started/quick-start/)

Requirements: macOS with [Homebrew](https://brew.sh).

```sh
brew install ghidra cmake ninja mingw-w64   # ghidra pulls in openjdk@21
brew install sdl3 glslang spirv-cross        # for the renderer (optional)
brew install python@3                         # for the game-mode scripts (optional)
# put your BF2142 1.51 install (OpenSpy-patched) at "source/Battlefield 2142"

# Native (macOS) build and tests (ASan/UBSan enabled)
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
ninja -C build/native && build/native/tests/bf2142_tests
# tests run in parallel processes, one per core (about 30 s); -j 1 runs them
# one by one (about 4 min under ASan); a name runs only the tests containing it

# 32-bit Windows build: drop-in replacements for the retail DLLs
cmake -S . -B build/win32 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build/win32

# Run the start-up and level scripts against your install, and look at the level
build/native/src/Main/bf2142 --game "source/Battlefield 2142" \
    --api symbols/BF2142.exe.console.csv --api symbols/RendDX9_ori.dll.console.csv \
    +level Camp_Gibraltar --view          # or --screenshot shot.png [--camera x/y/z/yaw/pitch]
# run around as a soldier (team 1 PAC, 2 EU): WASD, shift sprints, the mouse turns,
# Space jumps, left Ctrl crouches, Z lies down, Caps Lock opens the commander screen,
# (keys from the game's Settings/Controls.con; rebind them on the Options page),
# E enters and leaves a vehicle nearby (then W/S drive and brake, A/D steer;
# F1..F8 change seats: a gunner's seat aims with the mouse and fires;
# 1..9 take the kit's weapon of that slot: 1 knife, 2 sidearm, 3 primary;
# the right button zooms a weapon that zooms and sets off your C4;
# C switches between first person (the default: arms and weapon, a seat's
# own camera in vehicles) and the chase view; --unlock-all gives every kit unlock)
#   --play 2
#   --camera-mode third          (start in the chase view; screenshots too)
#   --zoom                       (a screenshot's soldier zooms in after firing)
# with --screenshot: put a vehicle before the soldier, get in and drive it
# (--seat N: stay aboard in seat N for the shot)
#   --drive eu_fav [--seat 2]
# bots (the game's AI settings; paths on levels with a navigation mesh),
# and a headless run that prints the scores afterwards
#   --bots 8 --simulate 120     (e.g. +level minsk_coop +gameMode gpm_coop)
# the scoreboard (Tab while playing; this forces it on), the spawn screen
# (dying first; while dead, click a spawn icon on the map to pick it)
#   --scoreboard
#   --spawn-screen
#   --commander-screen          (take command of the team and open its screen)
# the Flash main menu (no level needed): a window, or a screenshot after 10 s
#   bf2142 --game "source/Battlefield 2142" --menu --view
#   bf2142 --game "source/Battlefield 2142" --menu --screenshot menu.png
# start a game from it: log in (or play offline), then Singleplay ->
# Instant Action or a playlist; the chosen map loads with the menu's bots
# and settings; the playlist goes on map after map, Esc shows the menu over
# the game (Disconnect leaves it). --offline skips the account service;
# --console "<command>" runs a console command once the round has started
# ("<seconds>:<command>" once the round is that old);
# --menu-script drives
# the menu without a window ("wait:<s> click:<x>,<y> type:<text>"), e.g.
#   bf2142 --game "source/Battlefield 2142" --menu --view --offline
# unattended runs in the window: timed input and screenshots, e.g.
#   --input-script "12:click:0.49,0.37 12.5:text:Tester 40:shot:out.png 41:down:W 44:up:W 60:quit"
#   (key:/down:/up: SDL key names, click:/move: window fractions, look:dx,dy,
#   button:n,0|1, text:, shot:file.png, quit; seconds from the window's opening)
# list the servers on OpenSpy's master server
#   bf2142 --game "source/Battlefield 2142" --servers
# play on one (loads its map, shows the server's objects; servers expect a
# logged-in account and a CD key), or only shake hands and print what it sends
#   bf2142 --game "source/Battlefield 2142" --connect 1.2.3.4:17567
#   bf2142 --game "source/Battlefield 2142" --probe 1.2.3.4:17567
# greet OpenSpy's account service (FESL, over TLS), or log in with an
# OpenSpy account (the first soldier unless one is named) and join with it
#   bf2142 --game "source/Battlefield 2142" --fesl
#   bf2142 --game "source/Battlefield 2142" --login name:password[@soldier] [--connect 1.2.3.4:17567]
# add objects and play an animation on skinned ones:
#   --spawn us_light_soldier@-38/38.3/-55/90 --animation Objects/Soldiers/Common/Animations/3P/3p_standfire.baf

# Ghidra analysis of the retail binaries
DECOMPILE=1 tools/ghidra/analyze.sh BF2142Audio.dll
```

Third-party libraries (libogg, libvorbis, zlib, Speex) are fetched and built by CMake.
OpenAL is loaded at runtime. On macOS that means OpenAL Soft
(`brew install openal-soft`). With Python 3 (3.9 or later, found by CMake)
the game runs its game-mode scripts (`python/bf2`, `mods/bf2142/python`):
conquest's control points, tickets and scoring. With OpenSSL (found by
CMake; `brew install openssl@3` on macOS) it can log in to the account
service. With SDL3 the voice chat uses the microphone and speakers (and the
options page's microphone test); without it voice still builds and runs
without audio devices. [docs/workflow.md](docs/workflow.md) covers
analysis, the decompilation helpers, and how to test a rewritten module inside
the retail game under CrossOver.

## Progress

| Module | Functions | State |
|---|---:|---|
| `Memory.dll` | 140 | Rewritten (`src/Memory`). Unit tests pass natively and as 32-bit Windows code. The retail game runs with the drop-in DLL. |
| `BF2142Audio.dll` | 1,690 | Rewritten (`src/Sound/SoundEngine`). It plays real game WAV/Ogg files natively on macOS through OpenAL Soft. In the retail game the drop-in DLL initializes ("Software Audio + EAX", EAX 3), creates the menu sounds and runs every frame. Not yet tested in a match. The executable's side of sound (`src/Sound/Game`) is rewritten too: the sound manager, `Sound` templates with their default per-class and per-component sounds, and the `Sound.*` console methods. In the native build the scripts' menu sounds play through it (`bf2142 --play-sound checkboxPress`). |
| `BF2142.exe`: file system | — | Rewritten (`src/IO/File`): ZIP archives and the virtual file system. Mounts the 27 archives of the real `ClientArchives.con` and reads game files from them. |
| `BF2142.exe`: console | — | Script interpreter rewritten (`src/Console`): every keyword, scopes, `run`/`include` with arguments, `->` capture, `>` redirection, method access rules, with the original's error messages and quirks. Typed method/property bindings (`ConsoleBinding.h`). The full console API, 2,449 methods in the EXE and 388 in the renderer with their signatures, is extracted to `symbols/*.console.csv`. Most methods are not implemented yet. |
| `BF2142.exe`: world | — | Object templates and objects (`src/World`): the template manager (type registry with the 68 template types and their class IDs, `create`/`active`/`activeSafe`, child templates), placed objects, and template loading from `Objects/` in the original order. Properties not modelled yet are kept as values on the template. For Camp Gibraltar this loads 5,632 templates and places 1,023 objects. |
| `RendDX9_ori.dll`: meshes, textures, terrain | — | Loaders for BF2-engine meshes (`src/Geometry`, all 1,672 shipped static/bundled/skinned meshes parse), DDS textures (`src/Texture`, all 1,549 object textures), and heightmaps (`src/Terrain`). A first renderer on SDL3's GPU API (`src/Render`: Metal on macOS, Vulkan/D3D12 elsewhere) draws the terrain with its colormaps, lightmaps and blended detail textures (settings from `terraindata.raw`), the static objects with their meshes, texture layers (base, detail, dirt, crack as `StaticMesh.fx` combines them; `_alphatest` variants for foliage) and atlas lightmaps, overgrowth trees placed by `OvergrowthCollision.con`, vehicles assembled from their bundled-mesh parts (put out by the rewritten object spawners at round start), skinned soldiers posed by a `.baf` animation, compiled roads (`src/Geometry/RoadFile`: 4,462 of the 4,471 shipped road meshes parse; the rest are BF2 leftovers), undergrowth generated per patch around the camera with the original placement rules and random generator (`src/Terrain/Undergrowth`; all 99 levels' `Undergrowth.dat` parse), a first water surface, and the sky dome, lit and fogged with the level's `Sky.con` settings using the original shaders' lighting formulas. Distant overgrowth billboards, water reflections and effects are still missing. |
| `BF2142.exe`: start-up | — | `bf2142` (`src/Main`) runs the original start-up sequence: archive scripts, sound and menu settings, `Init.con`, and with `+level` the level loader's scripts. Unimplemented methods are stubbed from the API tables. All 112 installed levels load their scripts with no errors beyond those the retail game also hits (editor-only and BF2 leftovers). `--view` opens the level in a window with a free camera; `--play` simulates the round (spawners, rigid bodies, the player's soldier, its rifle, vehicles to drive). |
| `AIDLL.dll` | 5,600 | Analysed, not started |
| `SwiffPlayer.dll` | 5,400 | Analysed. Port from gameswf source instead of decompiling. |
| `RendDX9_ori.dll` | 9,400 | Analysed, not started |
| `BF2VoipServer.dll` | 360 | Rewritten (`src/Voip/VoipServer`): the instances, the game server's challenge and session string, the player table and the team/squad/commander channels, voice forwarding. The game server's side (`VoipServerConnection`) and the client (`VoipClient`, `VoiceChat`: Speex narrowband as the original's GameSpy Voice runs it, voice activity and push-to-talk, a jitter buffer, the menu's voice natives and mic test) too. Tested in loopback sessions; not yet against the retail game. |
| `BF2142.exe` | 30,000 | Analysed (calling conventions, vtables, classes, console API, singletons and constants recovered). In progress: the simulation (`src/Game`, `src/Physics`, `src/Effects`): object spawners and control points, rigid bodies, soldier movement, firearms and projectiles, armor and materials, newCar2 ground vehicles. See [docs/roadmap.md](docs/roadmap.md). |

## What we're working with

The [binary inventory](docs/binaries.md) has the details. In short:

- The DICE modules are **32-bit x86, MSVC 7.1 (VS .NET 2003)**, built
  2011-02-09 from the `Patch_1_51` branch. They are **unpacked** and keep
  relocations, but have **no PDBs and no RTTI**.
- **`BF2142.exe`** (5 MB of code) holds the engine core, game logic,
  networking, physics, menus and the Python host. Subsystems in separate DLLs
  use a small `initDll`/`deinitDll` interface: the renderer
  (`RendDX9_ori.dll`), AI (`AIDLL.dll`), Flash UI (`SwiffPlayer.dll`, built on
  open-source **gameswf**), and audio (`BF2142Audio.dll`). They all use DICE's
  allocator, `Memory.dll`.
- **Game rules are Python 2.3** and ship as source. The engine embeds Python
  through `dice_py.dll` and exposes `host` and `bf` modules to the scripts.
- **Shaders ship as HLSL `.fx` source** (D3DX effects).
- Third-party pieces such as Bink video, zlib, PCRE, OpenAL, D3DX and Python
  get replaced with maintained libraries. PunkBuster and the haptics support
  are dropped.

## Strategy

The [research notes](docs/research.md) compare the options. The current plan
has four phases:

1. **Map the binaries.** Run Ghidra auto-analysis, apply known symbols, and
   attribute functions to source files using the assert paths. Recover class
   layouts from Python bindings, strings, and the `initDll` interfaces between
   modules. Write it all into `symbols/` and later into shared headers.
2. **Decompile functionally, module by module.** Write readable C++ in the
   original directory layout (`BF2/Game/...`, `BF2/Physics/...`). The goal is
   *behavioural* equivalence, not byte-matching (see trade-offs below). Start
   with the small, self-contained modules: `Memory.dll`, then `AIDLL.dll`
   (pure logic), then `BF2142Audio.dll`.
3. **Verify incrementally where we can.** While `BF2142.exe` is still
   original, rewritten functions can be built as a 32-bit Windows DLL and
   hooked into the real game the way OpenSpy does it. That runs under
   CrossOver or Wine on the Mac and checks the rewrite against the original in
   a live game. Rosetta 2 can't run 32-bit code, so this has to go through
   Wine.
4. **Add a native platform layer** that replaces Win32, DirectInput,
   DirectSound and Direct3D 9:
   - Windowing and input: **SDL3**
   - Graphics: **SDL3 GPU** with HLSL translated by **SDL_shadercross**
     (Metal, Vulkan and D3D12 backends), plus a small D3DX-effect
     compatibility layer. bgfx is the fallback.
   - Audio: **OpenAL Soft**
   - Video: **FFmpeg** (Bink 1 decoder)
   - Scripting: **CPython 2.3.x** built from source
   - Everything 64-bit clean, with explicit serialisation for 32-bit on-disk
     and network formats.

LEGO Island followed much the same path ([isledecomp](https://git.mattkc.com/isledecomp/isle)
→ [isle-portable](https://git.mattkc.com/isledecomp/isle-portable), which uses
SDL3 and runs on macOS), so we can learn from how that project went.

**Why not a matching decomp?** It gives the strongest correctness guarantee,
but it needs the MSVC 7.1 toolchain with the exact flags, and roughly 5 MB of
x86 code makes that a multi-year effort. We can still revisit it for
individual functions with `reccmp`/`objdiff` where exact behaviour matters
(physics, netcode).

**Why not static recompilation?** It would get something running sooner, but
it produces source nobody can maintain, and it still needs Win32 and D3D9
emulated underneath. We may use it as a stopgap for code we haven't
decompiled yet.

## Networking (OpenSpy)

The installed OpenSpy patch is a proxy `RendDX9.dll`
([openspy/bf2142_rendhook](https://github.com/openspy/bf2142_rendhook)). It
patches the game in memory when loaded. The files on disk are unchanged.

- It redirects all GameSpy hostnames (GP, GameStats, QR master, server
  browser, availability check, Stella stats) to `*.openspy.net`.
- It sends EA **FESL** login to `fesl.openspy.net:18301` and replaces the
  game's outdated built-in SSL with OpenSSL (TLS 1.2+).
- It fixes two client bugs: login error −206, and a hang when deleting a
  soldier profile.

Every address it touches is in [`symbols/BF2142.exe.csv`](symbols/BF2142.exe.csv),
so those functions are already named in Ghidra. When we decompile the
networking code we'll build in these changes directly (configurable
hostnames, a modern TLS library for FESL, both bug fixes). Protocol details
come from OpenSpy's server code
([openspy-core](https://github.com/openspy/openspy-core),
[bf2142-stella_stats](https://github.com/openspy/bf2142-stella_stats)) and the
GameSpy SDK it's compatible with, not from blind reverse engineering.

## Conventions

- Names in `symbols/*.csv` should match DICE's where the evidence supports
  it: source paths, Python names, and `dice::hfe` strings. Mark guesses with
  `guess:` in the comment column and give the source of each fact.
- Addresses always refer to the 1.51 binaries listed in
  [docs/binaries.md](docs/binaries.md).
- Don't do anything that rules out Windows or Linux. Wrap platform-specific
  code behind a platform layer.
