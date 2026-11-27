# BF2142 Launcher

A small desktop app for development: it builds the port with ninja and runs it
with the options you tick, from the repository root, as if you ran `bf2142` in
a terminal there.

```sh
cargo tauri dev   # from the repository root (or from launcher/)
```

You need the Tauri CLI (`cargo install tauri-cli`), Trunk (`cargo install trunk`)
and the `wasm32-unknown-unknown` target (`rustup target add wasm32-unknown-unknown`).

## What it does

- **Options.** It reads them when it starts:
  - every flag from the README's "Option reference", grouped as there;
  - the enhanced options from `src/Scene/EnhancedSettings.h`, with their defaults.

  Adding an option to either file adds it here.
- **BF2142 directory.** Type it, or pick it with "Choose…"; a folder inside the
  repository is kept relative to it.
- **Map.** It lists the levels of a mod in the BF2142 directory (the mods are
  the folders in `mods/`; another than `bf2142` adds `+modPath mods/<mod>`).
  For each `mods/<mod>/Levels/<level>/` it reads:
  - `server.zip`'s `GameModes/<mode>/<size>/GamePlayObjects.con`: the modes and
    sizes the game can load, as the menu's level list (`scanMenuMaps`);
  - `Info/<level>.desc`: the modes in the file's order (then those only
    `server.zip` has), the sizes it names, and the briefing's `locid`, shown in
    English from `Localization/strings.csv` (and `strings_patch.csv`), as are
    the modes' names (`TEXT_gpm_cq`, "Conquest");
  - `AIPathFinding/Infantry.qtr` in `server.zip` (or `client.zip`): the game
    adds bots only on levels with it ("nav" in the list);
  - `Info/<mode>_<size>_preview.png` (or the menu map), as the picture.

  The list shows levels as the menu does (the folder's name with underscores as
  spaces) in the menu's scan order, and can be filtered by name, by a mode (the
  mod's modes in the order the create-server page lists them) and to levels with
  a navmesh. A level whose `.desc` or `server.zip` is missing or unreadable stays
  in the list, marked "!", with what's wrong; a mode or size that only the
  `.desc` names is shown greyed (the game can't load it). Choosing a level
  passes `+level <level> +gameMode <mode> +mapSize <n>`, plus `--bots <n>`,
  `--bot-skill <0..1>` (only with bots) and `--time-limit <s>` (entered in
  minutes). Choosing one while Run is set to the menu switches it to "Play the
  map"; with the menu (or "No level") none of these are passed, as the menu
  picks the map. The option groups below leave out the flags the map picker
  owns. Ticket ratio and spawn time aren't offered: the binary takes them only
  from the menu or a hosted server's settings, not from the command line.
- **Demos.** "Record a demo" with a name passes `--record-demo <name>` for
  whatever Run starts (playing the map, the options' `--simulate` or `--host`):
  the round goes to `Demos/<name>.bf2142demo` under the repository root, the
  game's working folder. Below it the recorded demos are listed, the newest
  first: `Demos/*.bf2142demo` and the auto-recordings
  (`sv.autoRecord`) in `mods/<mod>/demos/`. Choosing one sets Run to
  **Play back a demo**, which passes `--demo <file>` (no level: the demo has
  its own), and, with "With its camera" ticked, `--demo-camera` with the
  camera file beside it (`<name>-def.bf2142cam`, or an auto demo's
  `<file>-def.bf2142cam`). The option groups leave out `--demo`,
  `--demo-camera` and `--record-demo`.
- **Enhanced.** "Enhanced mode" passes `--enhanced`. Its options sit in one
  collapsible group whose "All" box selects all, none or some of them. Only the
  options you change from their defaults are passed, as `+enhanced.<name> 0|1`.
- **Build.** The first time, it configures the build directory (default
  `build/release`) with `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release`; after that it
  runs `ninja -C <dir> src/Main/bf2142`.
- **Run.** It starts `<dir>/src/Main/bf2142` with the command shown, and stays
  disabled until that binary exists. The list beside it picks what opens:
  - **Play the map** (`--menu` with the level): the game logs in on the menu's
    login page (the last account, offline, its first soldier) and starts the
    level as the singleplayer page does, with the deployment screen, the HUD,
    Esc for the in-game menu and its DISCONNECT back to the menu. The map
    picker's `--bots` (always given, 0 for none), `--bot-skill`,
    `--time-limit` and `--team` (EU or PAC) override the profile's settings.
    Without a level it is the menu.
  - **Menu** (`--menu`, the default): the menu picks the map.
  - **Free camera, no player** (`--view`): the level with a free camera and
    no soldier of yours (the bots, if any, play).
  - **Only the options below**: none of those. Without `--menu`, `--view`,
    `--play` or `--connect` the game opens no window, it loads, prints a
    summary and exits (with `--simulate <s>` from "Bots and headless rounds",
    a headless round).
  - **Play back a demo** (`--demo`): the demo chosen under Demos, in a window.

  A "Level view" kept by an older launcher becomes "Play the map". Stop ends
  whichever build or run is going.
- **Command.** The full command line, as run from the repository root, is shown
  at the bottom; "Copy" puts it on the clipboard.
- **Saving.** Your choices are kept between launches, in the window's local
  storage.

The frontend is Leptos (`src/`), built by Trunk. The backend is Tauri
(`src-tauri/`); it reads the options and runs the builds and the game.
