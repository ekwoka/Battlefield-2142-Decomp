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
- **Enhanced.** "Enhanced mode" passes `--enhanced`. Its options sit in one
  collapsible group whose "All" box selects all, none or some of them. Only the
  options you change from their defaults are passed, as `+enhanced.<name> 0|1`.
- **Build.** The first time, it configures the build directory (default
  `build/release`) with `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release`; after that it
  runs `ninja -C <dir> src/Main/bf2142`.
- **Run.** It starts `<dir>/src/Main/bf2142` with the command shown, and stays
  disabled until that binary exists. The list beside it adds `--menu` (the default) or
  `--view`: without one of those (or `--play`, `--connect`) the game opens no
  window, it loads, prints a summary and exits. Stop ends whichever build or run is going.
- **Saving.** Your choices are kept between launches, in the window's local
  storage.

The frontend is Leptos (`src/`), built by Trunk. The backend is Tauri
(`src-tauri/`); it reads the options and runs the builds and the game.
