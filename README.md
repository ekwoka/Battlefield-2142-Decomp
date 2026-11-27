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

### Use and remixing

This project's code was written by AI, with a human guiding it. Because of
that, there are no IP protections on this repository's own code: use it, copy
it, change it and remix it however you see fit. This covers only what is in
this repository. Battlefield 2142 itself, its assets and its trademarks belong
to their owners and still need your own copy of the game.

## Repository layout

```
source/Battlefield 2142/   your game install, BF2142 1.51 + OpenSpy patch (git-ignored)
build/                     build trees, Ghidra project, analysis output (git-ignored)
reference/OpenBattlefield2/ the Refractor 2 reference implementation (git-ignored clone)
launcher/                  a Tauri + Leptos app that builds and runs the port (development)
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

# Release build: for playing and for the tests
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build/release
BF2142_GAME_DIR="$PWD/source/Battlefield 2142" nice build/release/tests/bf2142_tests
# the tests run in parallel processes, slowest first (the whole suite takes about
# a minute): every core when run under nice, else at most 8; -j N runs N at a
# time; a name runs only the tests containing it
# --remaster (or BF2142_REMASTER_TESTS=1) adds the Project Remaster tests; they
# need mods/Project_Remaster_v17_Final in the install (BF2142_GAME_DIR)

# Debug build with ASan/UBSan: only for investigating a failing test (several
# times slower); run just the failing tests in it, by name
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"
ninja -C build/native && build/native/tests/bf2142_tests <failing test>

# Optional: Steam Audio for the enhanced mode's occlusion (a 181 MB download)
cmake -S . -B build/release -DBF2142_STEAM_AUDIO=ON && ninja -C build/release src/Main/bf2142

# 32-bit Windows build: drop-in replacements for the retail DLLs
cmake -S . -B build/win32 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake -DCMAKE_BUILD_TYPE=Release
ninja -C build/win32

# Ghidra analysis of the retail binaries
DECOMPILE=1 tools/ghidra/analyze.sh BF2142Audio.dll
```

### Native Windows build

This checkout also builds the native port for Windows with LLVM-MinGW. Run
`build-windows.cmd` to configure and build the renderer-enabled executable;
`run-windows.cmd --menu --view` starts it against the retail install at
`C:\Program Files (x86)\Electronic Arts\Battlefield 2142`. The launcher accepts
the same options as `bf2142`, for example:

```bat
run-windows.cmd +level Camp_Gibraltar --play --view
run-windows.cmd +level Camp_Gibraltar --screenshot build\camp-gibraltar.png
run-windows.cmd +modPath mods/Project_Remaster_v17 +level Camp_Gibraltar --play --view
run-windows.cmd --play-sound checkboxPress
```

The local scripts expect CMake, LLVM-MinGW, SDL3, OpenAL Soft, glslang and
SPIRV-Cross in the adjacent workspace `work/` directory. `build-windows.cmd` uses `Q:` as a
temporary short path while compiling the embedded shaders; it removes that
mapping when the build finishes. Windows builds may omit OpenSSL (account
service login) and FFmpeg (Bink movies) if those libraries are not installed.
The build also fetches Valve's pinned Steam Audio SDK (181 MB); pass `--enhanced`
to use it for sound occlusion. Without `--enhanced`, the SDK is present but
unused, and the game keeps retail sound obstruction.

Third-party libraries (libogg, libvorbis, zlib, Speex) are fetched and built by CMake;
Valve's Steam Audio is fetched only when asked for (see below). OpenAL is
loaded at runtime. On macOS that means OpenAL Soft
(`brew install openal-soft`). With Python 3 (3.9 or later, found by CMake)
the game runs its game-mode scripts (`python/bf2`, `mods/bf2142/python`):
conquest's control points, tickets and scoring. With OpenSSL (found by
CMake; `brew install openssl@3` on macOS) it can log in to the account
service. With SDL3 the voice chat uses the microphone and speakers (and the
options page's microphone test); without it voice still builds and runs
without audio devices. [docs/workflow.md](docs/workflow.md) covers
analysis, the decompilation helpers, and how to test a rewritten module inside
the retail game under CrossOver.

### Steam Audio (optional)

[Steam Audio](https://valvesoftware.github.io/steam-audio/) does one thing
here: the enhanced mode's sound occlusion (`--enhanced`, with sound), where
walls and buildings between you and a sound muffle it. It is never used in
the default mode, which behaves as retail. It is opt-in:
`-DBF2142_STEAM_AUDIO=ON` makes CMake fetch Valve's prebuilt Steam Audio
4.8.1 SDK (Apache-2.0; a 181 MB zip, 414 MB unpacked), pinned by its
SHA-256. The build copies its library (`libphonon.dylib`, `phonon.dll` or
`libphonon.so`) next to the executable, and the game loads it from there.
Valve ships it for macOS, Windows x64 and Linux x86_64; on other platforms,
and in the 32-bit Windows build, the option is ignored with a warning.
Offline, point `-DBF2142_STEAM_AUDIO_URL=/path/to/steamaudio_4.8.1.zip` at a
copy of the zip (the hash is still checked).

Without it (the default), or when the library is missing at run time, the
enhanced mode keeps its other audio additions (HRTF and the EFX filters)
and occlusion stays the original game's obstruction.

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
$B --game "$G" --menu +level Cerbere_Landing +gameMode gpm_sp +mapSize 16 --bots 15  # straight into a level, as the singleplayer page starts it
$B --game "$G" +level Camp_Gibraltar --play --view # into a level as an EU soldier, without the menu
$B --game "$G" +level Cerbere_Landing +gameMode gpm_sp +mapSize 16 --bots 15 --simulate 120   # a headless bot round
```

For development there is also a small launcher app in [`launcher/`](launcher/README.md). Start it
with `cargo tauri dev` from the repository root. It builds with ninja, runs the game, and lets you
tick any of the options below and the enhanced options.

### How a run behaves

What a run does depends on three options:

| You give | You get |
|---|---|
| `--view` (or `--menu`, `--play`, `--connect` without the next two) | A window with sound. Runs until you quit. |
| `--screenshot out.png` | No window: the frame is drawn off screen, saved, and the program exits. |
| `--simulate N` | No window: N seconds of game time run as fast as possible, then the scores print (joined to a server: in real time, following its map changes). Only `--view` adds a window to it. |
| `--language L` | The game's language (the original's registry Language): strings.csv's column and the fonts of `Fonts/L/` (German, French, Russian, Chinese_Simplified, ...; English when the file has no such column). |

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
| Left button / right button | Fire / zoom (sets off placed C4; in a seat, the seat's PIFire and PIAltFire weapons, also a ground vehicle's driver's; the right button also zooms a seat gun that has a zoom, such as the tanks' and walkers' guns, in the inside or nose view) |
| Shift | Sprint |
| Space | Jump |
| Left Ctrl / Z | Crouch / lie down |
| R | Reload (weapons only reload by themselves when set to, as in the original) |
| 1 … 9 | Kit slots: 1 knife, 2 sidearm, 3 primary … |
| Mouse wheel | Item list: scroll through the kit (in a vehicle, where its HUD allows it), then fire to take the selected item; zoomed in with a rocket launcher that has a range counter, the airburst distance up / down |
| E | Enter or leave a vehicle nearby |
| F1 … F8 | Change seats |
| F | Cycle weapons on foot or in the current seat |
| X | In a seat: flares or active defence (keys 1 … 9 pick a seat weapon; the alt-fire button arms a TV-guided missile) |
| C | Cycle vehicle cameras (living infantry stays in first person) |
| Enter | Open or close deployment; its Suicide button requests redeployment |
| 7 | Select frag grenades when equipped; left button throws (G is kit pickup/drop) |
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
| `+widescreen 1` | List display modes wider than 4:3 in the video options (as the original, which lists only 4:3 modes without it). |
| `--movie <name>` / `--movie-seconds <s>` | Play one movie (`Movies/<name>.bik`); with `--screenshot`, save its frame at that time. |

**Playing a level yourself**

| Option | What it does |
|---|---|
| `--play [1\|2]` | Spawn and control a soldier of team 1 (PAC) or 2 (EU, the default). |
| `--team <1\|2>` | The team to play for, with `--play` or with `--menu` and `+level` (EU, 2, without it). |
| `--drive <vehicle>` | Put a vehicle (e.g. `eu_fav`) in front of the soldier and get in. It lands on whatever is under the spot (a roof, a deck or the terrain) when a 4 × 6 m footprint there is clear and level with the soldier; else on the nearest such spot round him, else on the nearest open ground within 60 m. A leading `goto:` in `--actions` places the soldier first (the rest run in the seat). |
| `--seat <n>` | With `--drive`, stay in seat n (1 is the driver). |
| `--kit <name\|n>` | Spawn with that kit of the team's list (e.g. `EU_Anti-Vehicle`, or its number from 1), also on a server joined with `--play`. |
| `--zoom` | A screenshot's soldier zooms in after firing. |
| `--actions "<cmd> …"` | A screenshot's soldier's inputs before the shot, e.g. `"reload wait:60 firemode fire:10 altfire:3 item:2 prone:20 jump"` (`altfire` also zooms, as the right button; `flare` is a seat's flares or active defence; `goto:x/y/z/yaw` puts the soldier at a spot facing a way). Each prints the weapon in hand (in a seat, each weapon group's), the soldier's place, hit points and battery, the objects new since the last, the rounds' hits since the last and the `--spawn` targets' hit points. |
| `--unlock-all` | Accepted and ignored: human players always own every unlock, in every mode (bots own only what they are given). |
| `--unlocks <a,b,…>` | Equip only these unlocks (ItemContainer names, e.g. `UnlockEU_SupportHeavy2`), as the customise screen equips them; every other unlock stays owned. |
| `--squad-leader` | Lead a squad of your own that the team's bots join (the squad leader's beacon and drones want members). |
| `--scoreboard` / `--spawn-screen` / `--commander-screen` | Show the scoreboard; die first and show the spawn screen; take command and open the commander screen. |
| `--hud-click x/y[/right\|/move]` | A click or pointer move on the HUD's 800 × 600 screen before a screenshot (repeatable). |
| `--console "<command>"` | Run a console command once the round starts; `"<s>:<command>"` runs it when the round is that many seconds old (repeatable). |
| `--spawn <template>@x/y/z[/yaw]` | Add an object to look at or shoot at (repeatable; placed before the round settles and the `--actions` run). |
| `--animation <file.baf>` | Play an animation on the skinned objects. |
| `--dump-template <name>` | Print a template's tree and settings. |

**Bots and headless rounds**

| Option | What it does |
|---|---|
| `--bots <n>` | Add n AI players, alternately PAC and EU. Bots only move on levels with a navigation mesh. |
| `--bot-skill <0..1>` | The bots' skill (as `aiSettings.setBotSkill`). Without it: the AI scripts' own, or with `--menu` and `+level` the profile's. |
| `--simulate <s>` | Run s seconds of game time without a window, then print tickets, control points and each player's score. |
| `--director` | A match of bots only, filmed by an automatic camera, for recording footage with an outside screen recorder (the game captures nothing itself). The local player joins as a spectator, so every soldier is a bot (with `--menu +level` the player's slot goes to a bot too). Rounds and maps cycle as usual. About every 0.5 s each living bot gets an interest score from what the AI and the world know. Firing, being hit, dealing damage, a recent kill, an enemy in sight and in range, vehicle and aircraft fights, capturing a point and assaulting the enemy Titan (or its core) raise it. Idling, a seat with nothing to shoot, the spawn and manning your own Titan's guns lower it. The camera cuts between bots. A shot lasts 6 to 20 s, ends early after about 4 s of idling, and holds 2 s on the subject's death (the death camera, turned to the killer). A new subject must beat the current one clearly, and bots watched in the last few shots lose close calls. Each shot is a chase of the soldier or vehicle, the bot's own eye at its aim (no first-person weapon), or now and then a slow wide orbit of the action. The aim is smoothed so it doesn't jitter. The HUD, name tags, map and crosshair are hidden: **H** shows them, **Space** cuts to the next shot. Each cut is logged (`director: <time> cut to <bot> ... <mode>, <reason>`), and `--simulate` ends with how often it cut and how often it watched an engaged bot. For example `--menu +level Verdun +gameMode gpm_ti --bots 63 --director`, or `+level Verdun +gameMode gpm_ti +mapSize 48 --bots 47 --director --view`. |

**Menus**

| Option | What it does |
|---|---|
| `--menu [mainMenu\|endOfRound]` | Show the Flash menu (no level needed). With `--screenshot`, it plays for `--menu-seconds` (default 10) first. With `+level` in a window, the game starts that level at once as the singleplayer page does: it logs in on the menu's login page first (the last account, offline, its first soldier), skips the movies, and starts the round with the profile's settings (`--bots`, `--bot-skill`, `--time-limit` and `--team` override them). You get the deployment screen and the HUD, Esc opens the in-game menu on the singleplayer page, and its DISCONNECT goes back to the menu. |
| `--offline` | The menu finds no account service, so it plays offline. |
| `--enhanced` | Start in the enhanced mode (`enhanced.enabled 1`): additions that aren't in the original game. Off by default; while off the engine behaves as retail. `tools/parity-check.sh [seconds]` checks that the switch leaves the simulation alone. See [Enhanced mode](#enhanced-mode). |
| `--menu-script "<steps>"` | Drive the menu without a window: `wait:<s>` (`realwait:<s>` in real time, for searches and probes), `click:<x>,<y>`, `drag:<x>,<y>,<x2>,<y2>`, `press:<clip path's end>` (the middle of that clickable), `type:<text>`, `key:<Flash key code>`, `call:<clip>.<method>,<args>` (a result other than undefined printed); for checks `print:<path>` (a value from `_root`), `clickables[:<part>]` and `unbound` (natives called that nothing binds) print on stdout. |
| `--input-script "<s>:<cmd> …"` | Timed input in a window: `key:` / `down:` / `up:` (SDL key names), `click:` / `move:` (window fractions), `look:dx,dy`, `button:n,0\|1`, `text:`, `shot:file.png`, `quit`. |
| `--keys "<s>:<op> …"` | A level's game input without a window (with `--play`, `--connect`, `--host`; after `--simulate`, or before `--screenshot`): from that second on, `down:` / `up:` / `press:<control>` (the control map's names: `c_GIRadioComm`, `c_GITacticalComm`, `c_GISayAll`, `c_GISayTeam`, `c_GISaySquad`, `c_GIYes`, `c_GINo`, `c_GIShowScoreboard`, `c_PIFire`…), `text:<text>` and `key:<Flash code>` (13 Enter, 27 Esc) to the open say box, `move:` / `click:` / `rclick:<x>/<y>` on the HUD's 800 × 600 screen, `console:<line>` (`_` a space). It prints each step and what changed: the rose shown, the say box, new chat rows, the radio heard. A hosted or joined game runs in real time. |

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
| `--connect host[:port]` / `+joinServer host[:port]` | Join a server. Real servers also need `--online` and a login. With `--menu`, `+joinServer` waits for the login and the menu's main page joins it, as the original does. |
| `+password <text>` | With `--menu +joinServer`, the server's password. |
| `+hostServer 1` | With `--menu`, the main page hosts the menu's map list once logged in. |
| `--name <name>` | Your player name on the server. |
| `--play [1\|2]` | On a server, spawn, and ask for that team. |
| `--spectate` | Join as a spectator (the host needs `sv.allowSpectators 1`). |
| `--walk` | Without a window, walk and turn (a check of the client's prediction). |
| `--remote-actions "<s>:<op> …"` | Without a window, the soldier's inputs from that second on: `crouch`, `prone`, `stand`, `fire`, `hold`, `altfire` / `altstop`, `reload`, `forward`, `back`, `throttle:<x>`, `strafe:<x>`, `sprint` / `walk`, `stop`, `turn:<deg>`, `face:<player>` (`_` for a space; `enemy`, the nearest; or a template, e.g. `us_heavy_mech`), `goto:<same>` (and walk up to it), `still`, `zoom`, `pitch:<deg>`; presses `use`, `seat:<n>`, `item:<n>`, `jump`, `nextitem`, `previtem`, `weapon`, `firemode`, `flare`; requests `team:<n>`, `kit:<index>`, `spawn[:<group>]`, `leader` (spawn on the squad leader), `squad:<n>` (make one), `joinsquad:<n>`, `suicide`, `say:<word>`, `commander` (apply), `accept` (take the command offered), `titan:<x>:<z>` (as commander, move the Titan). `face`/`goto` turn from where the soldier looks and aim at a man-down soldier's body (a medic's shock). |
| `--probe host[:port]` | Only shake hands and print what the server sends. |
| `--servers` | List the servers the master server knows. |
| `--lan <address>` | Where the menu's LOCAL search probes (default 255.255.255.255; `127.0.0.1` keeps it on this machine). |
| `--demo <file>` | Play a recorded demo back. A name with no extension that isn't a file there plays `./Demos/<name>.bf2142demo`, where `demo.recordDemo <name>` puts it. See [Demos](#demos). |
| `--demo-camera <file.bf2142cam>` | With `--demo`: the camera follows a camera file (the original's `demo.cameramode playback`; recording with a local player writes `<name>-def.bf2142cam` beside the demo). The demo ends with the file. |
| `--demo-record-camera <file.bf2142cam>` | With `--demo`: record the demo camera you steer into a camera file (`demo.cameramode record`). |
| `--record-demo <name>` | Record the round from its start, as `demo.recordDemo <name>` does: `./Demos/<name>.bf2142demo` (and its camera file with a local player). Works in singleplayer, with `--simulate` and with `--host`. |
| `--demo-stats <file>` | Print a demo's ghost update rates (each soldier's updates a second and the gaps between them), its bandwidth and its size a minute, then quit. |
| `+sv.<setting> <value>` | Set a server setting at start-up, over the config file, e.g. `+sv.demoQuality 10` or `+sv.autoRecord 1`. A local round (singleplayer, `--simulate`) has its own `sv.*` for its demo recorder. |

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
| `BF2142_CONFIG_DIR` | The folder for profiles, settings, stats and demos, instead of `~/Library/Application Support/bf2142-recomp` (`%APPDATA%\bf2142-recomp`, `~/.config/bf2142-recomp`). Point it at a scratch folder for test runs of the menu. |
| `BF2142_ACCOUNT`, `BF2142_PASSWORD`, `BF2142_SOLDIER`, `BF2142_CDKEY` | Online credentials, instead of `bf2142-online.cfg`. |
| `BF2142_FRAME_BENCH=<n>` | With `--screenshot`, print the mean cost of n rendered frames. |
| `BF2142_FRAME_STATS=<seconds>` | Every `<seconds>` (5 if not a number; off when 0 or empty), print the main thread's time per frame stage (mean, 95th percentile and max, ms) and the frames over 50 ms. Also prints the GPU driver at startup. |
| `BF2142_BOT_SEED=<n>` | The bots' random seed, to compare rounds. Without it the seed comes from the clock, as in the original, except under `--simulate`, whose rounds repeat. |
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

### Demos

Any round you play or simulate can be recorded, as the original's local
games and servers can: singleplayer (the menu's or `+level`), `--simulate`
and `--host`. A local round records through a server of its own that opens
no socket and has no client but the demo's, so the round stays a local one.

| How | What it does |
|---|---|
| `--record-demo <name>` | Record from the round's start. |
| `demo.recordDemo <name>` / `demo.stopRecording` | The console commands (`--console`, the in-game console). |
| `sv.autoRecord 1` | Record each round from when it plays to its end, as `<mod>/demos/auto_<date>.bf2142demo` (`./mods/bf2142/demos/` by default). A local round takes `+sv.autoRecord 1`; a server its settings file too. |
| `sv.demoQuality <1..10>` | The original's rate: 4 to 20 packets a second (1 by default). With `--enhanced`, `enhanced.demoQuality` (on) records every tick instead. |

Files go under the folder you run from: `demo.recordDemo match1` writes
`./Demos/match1.bf2142demo` and, with a local player, its camera
`./Demos/match1-def.bf2142cam` (an auto demo's camera is
`<file>.bf2142demo-def.bf2142cam`). The camera file is written as the round
goes, as the original writes it each tick, so a crash doesn't lose it.
`--demo match1` plays `./Demos/match1.bf2142demo` back and `--demo-camera`
plays a camera file with it. The dev launcher lists both folders.

How a 64-player round (63 bots and you, Suez Canal conquest, a minute with
`--enhanced`'s bots) records:

| Setting | Each soldier's updates | Gaps between them (ticks) | Size |
|---|---|---|---|
| `sv.demoQuality 1` (the default) | under 0.05 a second | median 157 (5 s), 95% within 967 | 0.03 MB a minute |
| `sv.demoQuality 10` | 16 a second | median 2, 95% within 3 | 2.4 MB a minute |
| `enhanced.demoQuality` | 26 a second (every tick while it moves) | 1 | 3.6 MB a minute |

`--demo-stats <file>` prints these figures for a demo. Recording adds
about 1.2 ms a tick at 64 players and full rate (0.3 ms at
`sv.demoQuality 1`), mostly the ghosts' snapshot a live server takes too.

```sh
# a simulated Cerbere Landing 32-player conquest round, recorded, then played back
$B --game "$G" +level Cerbere_Landing +gameMode gpm_cq +mapSize 32 --bots 31 --play 1 --simulate 300 --enhanced --record-demo cerbere
$B --game "$G" --demo cerbere --demo-camera Demos/cerbere-def.bf2142cam
```

## Enhanced mode

The enhanced mode holds the rewrite's additions that aren't in the original
game: new render passes, audio effects, bot behaviour, camera and interface
changes. They are all ours (`enhanced.*` is not a retail console object) and
are listed in [`src/Scene/EnhancedSettings.h`](src/Scene/EnhancedSettings.h).

**The rule:** with `enhanced.enabled` off (the default) the game behaves as
the original. Every enhanced feature needs `enhanced.enabled` *and* its own
option; an option set while the switch is off does nothing.

### Turning it on

| How | What it does |
|---|---|
| `--enhanced` | Start with `enhanced.enabled 1`. |
| `+enhanced.<name> <value>` | Set an option at start-up, before the audio, the renderer and the bots read it, e.g. `+enhanced.enabled 1 +enhanced.taa 0`. This is what the launcher passes. |
| `enhanced.<name> <value>` | The console variable, e.g. `--console "enhanced.chaseRoll 0"`. Most options are read live; the audio reads `enhanced.enabled` once, when it opens, so set that one at start-up. |

```sh
$B --game "$G" +level Camp_Gibraltar --play --view --enhanced
$B --game "$G" +level Camp_Gibraltar --play --view +enhanced.enabled 1 +enhanced.ssao 0 +enhanced.cornerSteer 1
```

### Options

"Simulation" marks the options that change the game's simulation by design;
[`tools/parity-check.sh`](tools/parity-check.sh) turns exactly these off.
The rest only change what you see or hear.

| Option | Default | Category | What it changes compared with the original | Simulation |
|---|---|---|---|---|
| `enabled` | off | all | The master switch. Off, none of the options below apply. | — |
| `taa` | on | graphics | Temporal anti-aliasing (camera jitter and a history buffer). The world is then drawn single-sampled: TAA replaces the MSAA setting. | no |
| `ssao` | on | graphics | Ground-truth ambient occlusion (GTAO) darkens the lit shaders' ambient term. | no |
| `csm` | on | graphics | Cascaded sun shadows with PCF filtering. | no |
| `lightRank` | on | graphics | A mesh's point lights are ranked by distance over reach, so a small flash doesn't push out a wall light. | no |
| `staticShadows` | on | graphics | The terrain and lightmapped surfaces also take the static meshes' and the terrain's cascade shadows, over their baked ones (with `csm`). | no |
| `softShadows` | on | graphics | The two nearest cascades' shadows are sharp where they touch the caster and widen with distance from it (with `csm`). | no |
| `titanShadows` | on | graphics | A Titan's hull casts into the cascades as it moves (with `csm`). | no |
| `particleShadows` | on | graphics | Smoke and dust take the cascades' shadows (with `csm`). | no |
| `particleLights` | on | graphics | Smoke and dust are lit by the frame's point lights (explosions, muzzle flashes, lamps). | no |
| `glowOcclusion` | on | graphics | A lamp's additive halo is skipped while the line from the camera to its centre is blocked; the original only depth-tests halos per pixel. | no |
| `titanHullLods` | on | graphics | A Titan hull half drawn this frame takes its own LOD when the other half was culled, so a far Titan whose front half faded out draws whole; the original keeps the culled half's stale LOD. | no |
| `actorRange` | on | graphics | Soldiers and vehicles are drawn out to the view distance and fade into the fog as the terrain does. The original drops them by their size on screen: unzoomed on high geometry a soldier is gone at about 176 m (141 m on medium, 106 m on low), well inside every level's fog and every weapon's reach, so from a Titan's ground guns, high above the ground, soldiers below barely show. What bots see is unchanged. | no |
| `prepass` | off | graphics | The depth, normal and motion prepass even when neither `taa` nor `ssao` reads it. | no |
| `tvEffect` | off | graphics | Experimental: a guided round's view gets the TVEffect pass (grey, rolled by noise bands, rows shifted sideways so vertical edges lean). The original's renderer never runs it and shows the plain view. | no |
| `botRecovery` | on | bots | Bots get out of a vehicle that makes no progress. | yes |
| `botMovement` | on | bots | Bots on foot lie down only for far targets and don't walk sideways to look about; a gunner's aim starts afresh on a new target. | yes |
| `botTitan` | on | bots | Bots aboard a Titan walk its decks to the consoles and the core, man its guns and fire them at what the guns can reach (the shipped guns have no AI weapon, so they are fought with one made from their rounds, and the gunners see as far as the guns shoot); a transport's side gunners reach 220 m from its support station; in Titan mode bots head for the silos. A Titan gunner is seen and aimed at where its gun hangs under the hull, and fought as heavy armour (no shipped weapon has a strength against a Titan, so nobody ever fired at a manned Titan gun). | yes |
| `botOffMesh` | on | bots | A bot on foot where the level's infantry navigation mesh doesn't reach walks the terrain to its goal or onto the mesh. Each level ships one mesh, made for its 16-player layout, so the bigger layouts' spawns and points can lie off it (Cerbere Landing's 32 and 64 PAC spawns); the original's bots stand there doing nothing. | yes |
| `botAsyncPaths` | on | bots | The bots' costly Titan-deck ways (to the consoles and the core, and the pods' landing spots) are made while the level loads, and after a Titan part goes down or comes back they are made anew on background threads while the bots keep walking the last ones; a parked vehicle's search for the vehicle mesh is spread over ticks. Big Titan-mode rounds no longer freeze for half a second at those moments. | yes |
| `botPilots` | on | bots | A gunship's bot pilot picks the best target its guns can hurt (a vehicle, an infantry group, an aircraft, an enemy Titan's gun hubs), favouring those by its goal, the silos and, while the enemy's shield is down, the enemy Titan, and flies shallow attack runs at it: at a Titan gun straight out from the hull, a little under the gun, around the Titan rather than under it, breaking off rather than hanging under the hull. Locked on or hit hard it breaks away for a few seconds; badly damaged it flies to a supply point that mends it (its Titan's deck) and comes back. A deck gunship's take-off ends 300 m out or after 25 s rather than hovering over a silo the Titan stands by. The original's pilots mostly fly their waypoints and fire only when a target lines up. | yes |
| `botCommander` | on | bots | In Titan mode a team with bots and no commander has its Titan moved and its commander's tools used for it. | yes |
| `botLoneSeat` | on | bots | A bot left alone in a passenger or gunner seat of a vehicle nobody drives (its driver got out, or it spawned beside one who left) takes the free driver's seat after 2 s, or gets out after 6 s if it can't; a bot driving alone no longer moves over to its own vehicle's gun seat. The original's bots sit there for the rest of the round. | yes |
| `botThreat` | on | bots | A player who has killed lately (a busy Titan gun, a tank, a gunship) draws more attention: each kill in the last minute weighs it half as much again (up to four times) in the target choice of every bot whose weapons can hurt it, gunship pilots included. The original's target choice knows nothing of kills. | yes |
| `botTitanRepair` | on | bots | In Titan mode a bot respawning while its Titan has a gun down (or under half its hit points) and fewer than two engineers aboard takes the engineer kit and spawns aboard to mend it; a bot manning a gun that is wrecked under it gets out. The original's bots never board or mend a Titan. | yes |
| `cornerSteer` | off | bots | Experimental: a bot driver that keeps refusing a corner for lack of room steers wider (cars and tanks). | yes |
| `guidedCameraEase` | on | camera | A guided round's camera eases back to centre instead of snapping. | yes |
| `chaseRoll` | off | camera | A flying vehicle's chase and front-chase views bank with its hull; the original keeps them level. | no |
| `passengerWeapons` | on | gameplay | An air transport's passengers fire their own weapons (the pod only on the exit key). | yes |
| `softLanding` | on | gameplay | A soldier landing on or walking into a slower object takes the ground's fall damage instead of a road kill. | yes |
| `titanKitFloors` | on | gameplay | A kit dropped, or a beacon or shield placed, on a Titan stays on its walkable floor where the hull's bullet mesh has none (by the cargo bay's walls), and a portable shield or sentry gun set down on a Titan's deck starts standing on it. The original lets them fall through the hull. | yes |
| `titanDeckMines` | on | gameplay | A Titan doesn't set off the mines lying on its own decks. The original's mine trigger takes the moving, driverless Titan for a vehicle running over them, so a motion or EMP mine on its deck goes off as soon as it moves. | yes |
| `hudWidescreen` | off | interface | A mod without Project Remaster's hudfix gets the 4:3 HUD squeezed to the window's aspect ((4/3) ÷ aspect: the hudfix's 12/16 at 16:9, about 0.56 at 21:9), with its side pieces moved out to match (120 at 16:9) and the gunner frames widened to cover the window. It is laid out again when the window is resized. Ignored when the hudfix's shader is already installed, whose fixed 16:9 layout stays. | no |
| `squadOrderPoints` | on | gameplay | The squads' order points, their HUD bar and the field upgrades it awards work on any server, offline included. The original runs them only on a ranked GameSpy server, so offline it never shows the bar. | yes |
| `botSquadInvites` | on | gameplay | Offline, a bot you invite from the Manage Squad screen's invite list while you lead a squad joins it (unless it is full; a bot leading a squad hands that squad to its next member), so you can lead bots and use the squad leader's items, which need squad members. The original's bots never answer an invite, and join squads only when they are added at the start, so offline a squad you make stays yours alone. | yes |
| `squadItemHints` | on | interface | A weapon key that takes nothing because of the squad says why on the help line, every time: a squad leader's item (the beacon, the drones) when you lead no squad, or a squad too small for it, with the count ("(1/2)": members against the item's need). The original says nothing for the first, and for the second shows its "Your squad is not large enough" help once, only with help on and only to a squad leader. Either way the item is dimmed in the item list, as in the original. | no |
| `singleplayAllModes` | on | interface | The singleplayer page lists every level's every mode and size (Titan, Assault Lines, conquest and the rest), each row naming its mode, not only its single-player layouts; the chosen map starts in its own mode. For testing: the bots may not play those modes (Titan bots need `botTitan` and `botCommander`). | no |
| `demoQuality` | on | demos | A recorded demo's ghosts go out every tick (30 a second, retail's biggest packet: 4096 bytes, 255 updates), with more packets that tick when one is full, so a 64-player round plays back with every soldier updated every tick. The original's `sv.demoQuality` (1 to 10, 1 by default) sends 4 to 20 packets a second of 40 to 255 updates: at 1 a 64-player round's soldiers go seconds without an update; at 10 they get about a live client's 20 a second. See [Demos](#demos). The file stays readable by the original's reader (no payload over its 256 KB, no block over 255 updates). | no |
| `singleplayBotCount` | on | interface | The singleplayer page gets a MAX PLAYERS slider (1 to 64, default 16) beside the bot skill, kept in the profile. A playlist or instant action started from the page fills each map with that many players, you included, but never more than the map holds: with the slider at 64 a 32-player map gets 31 bots and the next 16-player map 15. The original's page starts every map with 16 players, you included, whatever its size, so at 16 the games are the original's. | no |

`enhanced.enabled` on its own also turns on the enhanced audio (below), lets
a sound the data names but never creates take its weapon event's defaults,
and makes the kit screen skip its HUD writes while nothing it shows has
changed (the original rewrites them every frame).

### Enhanced audio

The audio additions have no option of their own: they follow
`enhanced.enabled` as it stands when the sound engine opens. None of them
feeds the simulation.

- **HRTF:** OpenAL Soft is asked for HRTF (binaural 3D sound); the original
  leaves the device's default.
- **Filters:** the 3D sources get an EFX direct filter (gain and low-pass),
  which replaces the original's obstruction gain for them while an
  occlusion is active. A filter moves towards its new value by at most 0.1
  an update, the original obstruction's rate, so passing a corner fades.
- **Loop tracking:** a looping sound's position comes from OpenAL's
  `AL_SAMPLE_OFFSET` (the frames actually played, with pitch and device
  timing) instead of the original's estimate from elapsed ticks. Streams
  keep the estimate.
- **First-person fire:** a weapon whose `Fire1P` sound is filed under the
  same words in another order still finds it, and a first-person gun with no
  1P sound stays head-relative and ducks the world, so HRTF and occlusion
  don't treat your own gun as distant. A held automatic's fire loop also
  restarts in the new perspective's slot when the camera switches between
  first and third person; the original keeps it in the slot it started in
  until the trigger is released.
- **Doppler:** a moving source's speed is held to half the speed of sound
  over the Doppler factor, so a round diving at you (an orbital strike's
  shells) rises at most an octave. The original passes the full velocity
  (dropped only when an axis reaches 300), and its pitch shoots up just
  before the impact.
- **Steam Audio occlusion:** with a Steam Audio build (see
  [Steam Audio (optional)](#steam-audio-optional)), walls and buildings
  between you and a sound muffle it, simulated against the level's static
  meshes. Without the library, occlusion stays the original's obstruction.

### Experimental and off-by-default options

- **`cornerSteer`:** an experimental path-following aid for vehicles that
  loop at tight corners. It changes how bots drive, so it waits until it is
  proven.
- **`prepass`:** the prepass only has a reader in `taa` and `ssao`, which
  already turn it on; alone it costs a pass and changes nothing on screen.
- **`hudWidescreen`:** it changes the HUD's layout, and is only meant for a
  mod without Remaster's own widescreen fix; turn it on if you want it.
- **`tvEffect`:** the original's renderer has the TVEffect pass but nothing
  calls it, so the original never shows it; it stays off unless you want it.

### Parity checking

`tools/parity-check.sh [seconds] [extra game args...]` runs the same headless
bot round (Cerbere Landing, 15 bots) twice without `--enhanced`, to make sure
the round repeats, and once with it, with the simulation options above
(`botRecovery`, `botMovement`, `botTitan`, `botOffMesh`, `botAsyncPaths`, `botCommander`, `botPilots`, `botLoneSeat`, `botThreat`, `botTitanRepair`, `guidedCameraEase`,
`cornerSteer`, `passengerWeapons`, `softLanding`, `titanKitFloors`, `titanDeckMines`, `squadOrderPoints`, `botSquadInvites`) turned off. It compares the state
fingerprints each run prints and fails if `--enhanced` changed the
simulation. A fourth run records the enhanced round (`--record-demo`, at
`enhanced.demoQuality`'s full rate) and must match it: recording a demo
never changes the round. Anything added to the enhanced mode either leaves the
fingerprint alone or gets its own option, listed in the script.

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
   - Audio: **OpenAL Soft**; optionally **Steam Audio** for the enhanced
     mode's occlusion only (see [Steam Audio (optional)](#steam-audio-optional))
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
does (name checksum, profile, ValidatePlayer; the CD key is asked about
but, as in retail, a bad one is only logged) before making it.

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
