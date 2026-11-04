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
  Net/                     networking: GameSpy server browsing against OpenSpy, the game's UDP connections,
                           a server's settings, map list and bans, the remote console client
  Movie/                   the Bink movies' queue and decoder (FFmpeg; optional)
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

# Release build for playing (much faster than the ASan debug build above)
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build/release src/Main/bf2142

# 32-bit Windows build: drop-in replacements for the retail DLLs
cmake -S . -B build/win32 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build/win32

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

## Running the game

The program is `bf2142`. Build it as above; then run one of:

| Binary | Use |
|---|---|
| `build/release/src/Main/bf2142` | Playing and anything timed. Use this one. |
| `build/native/src/Main/bf2142` | Debugging. Built with ASan/UBSan, so 10–15 times slower; a real-time game crawls. |

Every run needs the game folder: `--game "source/Battlefield 2142"`, or set
`BF2142_GAME_DIR` once. The engine's console tables in `symbols/` are found
by themselves (from the working directory, else the source tree).

```sh
B=build/release/src/Main/bf2142
G="source/Battlefield 2142"

$B --game "$G" --menu --view                       # the main menu in a window: log in or play offline, then Singleplay
$B --game "$G" +level Camp_Gibraltar --play --view # straight into a level as an EU soldier
$B --game "$G" +level Cerbere_Landing +gameMode gpm_sp +mapSize 16 --bots 15 --simulate 120   # a headless bot round
```

### How a run behaves

What a run does depends on three options:

| You give | You get |
|---|---|
| `--view` (or `--menu`, `--play`, `--connect` without the next two) | A window with sound. Runs until you quit. |
| `--screenshot out.png` | No window: the frame is drawn off screen, saved, and the program exits. |
| `--simulate N` | No window: N seconds of game time run as fast as possible, then the scores print. Only `--view` adds a window to it. |

`--screenshot` and `--simulate` can be combined with almost everything below,
which is how the tests and the agents check the game without a window.

Options that start with `+` are the original game's own command-line
settings (`+level`, `+joinServer`, …); the rest are the rewrite's.

### Controls (in a window)

The keys come from the game's `Settings/Controls.con` and can be rebound on
the Options → Controls page; joysticks and gamepads (SDL3, plugged in or out
at any time) bind there too, by picking an action and pressing a button or
pushing a stick. A gamepad reads as an Xbox 360 pad.

| Key | Action |
|---|---|
| W A S D | Move (in a vehicle: drive, brake, steer) |
| Mouse | Look and aim (driving: the turret or the barrel where the driver's seat has one, as a tank's or a walker's; the hover tank also turns on it) |
| Left button / right button | Fire / zoom (sets off placed C4; in a seat, the seat's PIFire and PIAltFire weapons, also a ground vehicle's driver's) |
| Shift | Sprint |
| Space | Jump |
| Left Ctrl / Z | Crouch / lie down |
| R | Reload (weapons only reload by themselves when set to, as in the original) |
| 1 … 9 | Kit slots: 1 knife, 2 sidearm, 3 primary … |
| E | Enter or leave a vehicle nearby |
| F1 … F8 | Change seats |
| F / X | In a seat: next weapon / flares or active defence (keys 1 … 9 pick a seat weapon; the alt-fire button arms a TV-guided missile) |
| C | First person ↔ chase view |
| Tab | Scoreboard |
| M / N / Left Alt | Big map / minimap zoom / 3D name tags |
| Q / T | Radio menu / squad and commander menu |
| Caps Lock | Commander screen |
| K / L | Chat to all / to the team |
| Del / End | Answer yes / no to an invite or the offer of command |
| Esc | Menu over the game (Disconnect leaves it) |

### Option reference

**Game and level**

| Option | What it does |
|---|---|
| `--game <dir>` | The BF2142 install. Required (or `BF2142_GAME_DIR`). |
| `+level <name>` | The level to load, e.g. `Camp_Gibraltar`, `Cerbere_Landing`, `Suez_Canal`. |
| `+gameMode <mode>` | `gpm_cq` (default) conquest, `gpm_ti` Titan, `gpm_sp` / `gpm_coop` single player and co-op, `gpm_sl` Assault Lines, `gpm_nv`, `gpm_ca`. |
| `+mapSize <n>` | The layout: 16, 32 (default), 48 or 64. The level must ship that mode and size. |
| `+modPath <dir>` | The mod folder, default `mods/bf2142`. |
| `--api <file.csv>` | Extra console tables (by default the two in `symbols/`). |
| `--verbose` | More output. |

**Window, sound and pictures**

| Option | What it does |
|---|---|
| `--view` | Open a window (with sound). |
| `--screenshot <file.png>` | Draw one frame off screen, save it and exit. |
| `--camera x/y/z/yaw/pitch` | Where a screenshot's camera stands (degrees for yaw and pitch). |
| `--camera-mode first\|third` | Start in first person (default) or the chase view. |
| `--nosound` / `--sound` | No audio at all / the audio engine even without a window. |
| `--play-sound <name>` | Play a named sound after start-up. |
| `--no-movies` | Skip the start-up movies. |
| `--movie <name>` / `--movie-seconds <s>` | Play one movie (`Movies/<name>.bik`); with `--screenshot`, save its frame at that time. |

**Playing a level yourself**

| Option | What it does |
|---|---|
| `--play [1\|2]` | Spawn and control a soldier of team 1 (PAC) or 2 (EU, the default). |
| `--drive <vehicle>` | Put a vehicle (e.g. `eu_fav`) in front of the soldier and get in. |
| `--seat <n>` | With `--drive`, stay in seat n (1 is the driver). |
| `--zoom` | A screenshot's soldier zooms in after firing. |
| `--actions "<cmd> …"` | A screenshot's soldier's inputs before the shot, e.g. `"reload wait:60 firemode fire:10 item:2 prone:20 jump"`. |
| `--unlock-all` | Every kit unlock. |
| `--scoreboard` / `--spawn-screen` / `--commander-screen` | Show the scoreboard; die first and show the spawn screen; take command and open the commander screen. |
| `--hud-click x/y[/right\|/move]` | A click or pointer move on the HUD's 800 × 600 screen before a screenshot (repeatable). |
| `--console "<command>"` | Run a console command once the round starts; `"<s>:<command>"` runs it when the round is that many seconds old (repeatable). |
| `--spawn <template>@x/y/z[/yaw]` | Add an object to look at (repeatable). |
| `--animation <file.baf>` | Play an animation on the skinned objects. |
| `--dump-template <name>` | Print a template's tree and settings. |

**Bots and headless rounds**

| Option | What it does |
|---|---|
| `--bots <n>` | Add n AI players, alternately PAC and EU. Bots only move on levels with a navigation mesh. |
| `--simulate <s>` | Run s seconds of game time without a window, then print tickets, control points and each player's score. |

**Menus**

| Option | What it does |
|---|---|
| `--menu [mainMenu\|endOfRound]` | Show the Flash menu (no level needed). With `--screenshot`, it plays for `--menu-seconds` (default 10) first. |
| `--offline` | The menu finds no account service, so it plays offline. |
| `--menu-script "<steps>"` | Drive the menu without a window: `wait:<s>`, `click:<x>,<y>`, `type:<text>`, `call:<clip>.<method>,<args>`. |
| `--input-script "<s>:<cmd> …"` | Timed input in a window: `key:` / `down:` / `up:` (SDL key names), `click:` / `move:` (window fractions), `look:dx,dy`, `button:n,0\|1`, `text:`, `shot:file.png`, `quit`. |

**Hosting a game**

| Option | What it does |
|---|---|
| `--host` | Serve the level to other `bf2142` clients (port 17567). |
| `--dedicated` / `+dedicated 1` | A dedicated server: no window, no sound. Runs the map list until Ctrl+C or an rcon `quit`. |
| `+port <n>` / `--query-port <n>` | The game port / the LAN query port (default 29900; 0 for none). |
| `+config <file>` / `+mapList <file>` | Server settings (default `mods/bf2142/Settings/ServerSettings.con`) / map list (default `maplist.con`). |
| `+overlayPath <dir/>` | Read `Settings/` and `admin/default.cfg` from that folder instead. |
| `--maps level[:mode[:size]],…` | A playlist for `--host`, replacing the map list. |
| `--time-limit <s>` | The round's time limit. |
| `--require-login` | Check players' logins as a retail server does. |

**Joining a game**

| Option | What it does |
|---|---|
| `--connect host[:port]` / `+joinServer host[:port]` | Join a server. Real servers also need `--online` and a login. |
| `--name <name>` | Your player name on the server. |
| `--play [1\|2]` | On a server, spawn, and ask for that team. |
| `--spectate` | Join as a spectator (the host needs `sv.allowSpectators 1`). |
| `--walk` | Without a window, walk and turn (a check of the client's prediction). |
| `--remote-actions "<s>:<op> …"` | Without a window, the soldier's inputs from that second on: `crouch`, `prone`, `stand`, `fire`, `hold`, `reload`, `forward`, `stop`, `turn:<deg>`, `face:<player>`, `still`, `zoom`, `pitch:<deg>`. |
| `--probe host[:port]` | Only shake hands and print what the server sends. |
| `--servers` | List the servers the master server knows. |
| `--lan <address>` | Where the menu's LOCAL search probes (default 255.255.255.255; `127.0.0.1` keeps it on this machine). |
| `--demo <file>` | Play a recorded demo back. |

**Network diagnostics**

| Option | What it does |
|---|---|
| `--net-log <file>` | Log every datagram in and out, hex and decoded. |
| `--net-strict` | Report every decode problem on stderr as it happens, with a summary at the end. |
| `--net-capture <file>` / `--net-replay <file>` | Record a session's datagrams / replay a recording into the client offline (nothing is sent). |

**Online services** (see [Networking (OpenSpy)](#networking-openspy))

| Option | What it does |
|---|---|
| `--online` | Allow the real OpenSpy services. **Without it nothing leaves this machine or the local network.** |
| `--login [name:password[@soldier]]` | Log in (EA's FESL, then GameSpy Presence). Bare, it takes the credentials from the environment or `bf2142-online.cfg`. |
| `--fesl` | Only greet the account service. |
| `--openspy-emulator` | Run local stand-ins of OpenSpy's services on 127.0.0.1. |
| `--emulator-account name:password@soldier[,soldier]` | An account the emulator accepts (repeatable). |
| `--emulator-cdkey <key>` | Make the emulator's CD-key service accept only these keys (repeatable). |
| `--emulator-list host:port[:queryport]` | A server the emulator's master server lists (repeatable). |

**Remote console**

| Option | What it does |
|---|---|
| `--rcon host[:port]` | Talk to a server's remote console (port 4711 by default, from `admin/default.cfg`). |
| `--rcon-password <password>` | Its password. |
| `--rcon-command "<command>"` | A command to run, e.g. `admin.listPlayers` (repeatable). |

### Environment variables

| Variable | What it does |
|---|---|
| `BF2142_GAME_DIR` | The install, instead of `--game`. |
| `BF2142_ACCOUNT`, `BF2142_PASSWORD`, `BF2142_SOLDIER`, `BF2142_CDKEY` | Online credentials, instead of `bf2142-online.cfg`. |
| `BF2142_FRAME_BENCH=<n>` | With `--screenshot`, print the mean cost of n rendered frames. |
| `BF2142_BOT_SEED=<n>` | The bots' random seed, to compare rounds. |
| `BF2142_BOT_STATS=1` | Per-bot statistics at the end of a run. |
| `BF2142_SOUND_LOG=1` | Print every sound played. |
| `BF2142_POST_EFFECT=emp:N\|soldieremp:N\|tinnitus\|tv\|camera\|flash\|camo` | Force one screen effect, for screenshots. |
| `BF2142_SHADOW_DEBUG`, `BF2142_SHADOW_STATS`, `BF2142_SHADOW_REDRAW` | Shadow debugging: show the shadow factor, print the casters, redraw every frame. |
| `BF2142_TERRAIN_LODS=1` | Tint each terrain cell by its detail level. |

### More examples

```sh
# a screenshot of a level (the camera is placed for you; --camera x/y/z/yaw/pitch
# picks the spot, but keep it within the level's fog: Camp Gibraltar's ends at 100 m)
$B --game "$G" +level Cerbere_Landing --screenshot shot.png

# drive the FAV and shoot from the gunner's seat, as a screenshot
$B --game "$G" +level Camp_Gibraltar --play 2 --drive eu_fav --seat 2 --screenshot gunner.png

# a Titan-mode bot round
$B --game "$G" +level Suez_Canal +gameMode gpm_ti +mapSize 48 --bots 15 --simulate 300

# host on this machine and join it from a second terminal
$B --game "$G" +level Camp_Gibraltar +gameMode gpm_cq --host --bots 8 --simulate 600
$B --game "$G" --connect 127.0.0.1 --name Joiner --play --view

# a dedicated server and its remote console
$B --game "$G" --host --dedicated
$B --rcon 127.0.0.1:4711 --rcon-password <yours> --rcon-command "admin.listPlayers"

# the menu, driven without a window: offline login, then a screenshot
$B --game "$G" --menu --menu-script "wait:12 click:500,283 type:Tester click:500,330 type:pw click:510,364 wait:4 click:437,363 wait:4 click:450,258 click:510,398 wait:6" --screenshot menu.png

# the whole online chain against the local emulator (nothing leaves this machine)
$B --game "$G" --openspy-emulator --emulator-account tester:secret@Tester
$B --game "$G" +level Camp_Gibraltar +gameMode gpm_cq --host --require-login --simulate 600
$B --game "$G" --login tester:secret@Tester --connect 127.0.0.1 --play --view
```

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

### Offline by default

The rewrite contacts nothing outside this machine (and its LAN) unless
the command line has `--online`. Without it every service (FESL, GameSpy
Presence, the master server, the stats and CD-key services) is
`127.0.0.1`, FESL is plain TCP there, and connecting to any public address
is refused with "live servers need --online". `bf2142 --openspy-emulator`
runs local stand-ins of OpenSpy's services built from OpenSpy's code
(`src/Net/OpenSpyEmulator.h`); `--emulator-account name:password@soldier`
(repeatable), `--emulator-cdkey KEY` (then only that key passes) and
`--emulator-list host:gameport[:queryport]` set what they know. A host
started with `--require-login` checks a joining player as a retail server
does (name checksum, profile, CD key, ValidatePlayer) before making it.

### Credentials

Nothing is stored in the repository. `--login` with no value, the menu's
login page, and the CD-key answer read, in order of precedence:

- the environment: `BF2142_ACCOUNT`, `BF2142_PASSWORD`, `BF2142_SOLDIER`,
  `BF2142_CDKEY`;
- a key=value file: `$BF2142_ONLINE_CONFIG`, else `./bf2142-online.cfg`
  (git-ignored), else `~/.config/bf2142/online.cfg`:

```
# bf2142-online.cfg — keep it out of git
account = your OpenSpy account name
password = your password
soldier = the soldier to play (empty: the first)
cdkey = XXXX-XXXX-XXXX-XXXX-XXXX
```

`--login name:password[@soldier]` overrides the account part. The retail
game reads the CD key from the registry
(`HKLM\SOFTWARE\Electronic Arts\EA Games\Battlefield 2142\ergc`); OpenSpy's
CD-key service accepts any key, but the key you send should be yours.

### Your first live test

Everything below happens on your machine and talks to OpenSpy only
because of `--online`; do it yourself, once you are happy with the local
run above.

1. Make an OpenSpy account and a soldier (in the retail game's login
   page, or on openspy.net), then write `bf2142-online.cfg` as above (or
   export the `BF2142_*` variables).
2. Log in only (FESL, the soldier, GameSpy Presence):
   ```
   bf2142 --game "source/Battlefield 2142" --online --login 2>&1 | tee login.log
   ```
   It should end with `playing as <soldier> (profile N, name checksum …,
   GP login ticket received)`.
3. List the servers:
   ```
   bf2142 --game "source/Battlefield 2142" --online --servers | tee servers.log
   ```
4. Join one (pick an IP and its `hostport` from the list; an unranked,
   non-PunkBuster server with free slots), headless for two minutes, with
   the network log:
   ```
   bf2142 --game "source/Battlefield 2142" --online --login --connect <ip>:<port> --play 1 \
       --simulate 120 --net-log join-net.log --net-capture join.cap 2>&1 | tee join.log
   ```
5. Send back `login.log`, `servers.log`, `join.log`, `join-net.log` and
   `join.cap` (they hold no password; the logs show the soldier name and
   profile id, and the capture the GP login ticket, which expires — log out
   or wait a day before sharing them publicly). What to look for: in
   `join.log`, whether the server sent CreatePlayer for us (a local player
   id and the level loading) or disconnected us (the reason number).

## Conventions

- Names in `symbols/*.csv` should match DICE's where the evidence supports
  it: source paths, Python names, and `dice::hfe` strings. Mark guesses with
  `guess:` in the comment column and give the source of each fact.
- Addresses always refer to the 1.51 binaries listed in
  [docs/binaries.md](docs/binaries.md).
- Don't do anything that rules out Windows or Linux. Wrap platform-specific
  code behind a platform layer.
