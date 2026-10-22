# Decompilation research notes (October 2026)

This is a survey of current approaches to decompiling games and how each one
applies to BF2142. The [README](../README.md#strategy) has the strategy we chose.

## 1. Approaches

### Matching decompilation
You write C/C++ that compiles to **byte-identical** machine code with the
original compiler and flags. This is the standard approach for console games
(the [Super Smash Bros. Melee decomp](https://github.com/doldecomp/melee),
which was reported complete in September 2026, plus many N64, GC and GBA
projects).

- Tooling: [objdiff](https://github.com/encounter/objdiff) shows per-function
  diffs, [decomp.me](https://decomp.me) hosts shared scratch builds, and
  `decomp-permuter` tries random code variations to find a match.
- On PC, [isledecomp (LEGO Island)](https://git.mattkc.com/isledecomp/isle)
  built **reccmp**, which uses source annotations to check function assembly,
  vtables and variable offsets against the original binary. It counts a
  function that differs only by register swaps as an effective match. That
  project decompiled `ISLE.EXE` and `LEGO1.DLL` completely and then produced
  [isle-portable](https://git.mattkc.com/isledecomp/isle-portable), which runs
  on Windows, Linux, macOS and the web through SDL3 and a `miniwin` shim for
  Windows APIs. This is the closest precedent to what we want.
- Cost: you need the exact compiler (MSVC 7.1 here) and the exact flags. You
  find the flags by compiling test programs and comparing the generated code.
  Signs like whether stack cookies are present (`/GS`) narrow it down
  ([fmtk notes](https://github.com/widberg/fmtk/wiki/Decompilation)). The
  effort grows with code size, and `BF2142.exe` has about 5 MB of `.text`.

### Functional (non-matching) decompilation
You write source that **behaves** the same as the original but may compile to
different code. It's much faster per function, but you have to check
correctness with tests or by comparing the game's behaviour, not by diffing
bytes. Most PC "reimplementation" projects work this way, including OpenRCT2
(which began from decompiled code), re3/reVC and the
upcoming community BF2 rebuild
([news](https://www.tweaktown.com/news/114023/battlefield-2-is-getting-an-unofficial-open-source-revival-with-working-multiplayer/index.html)).

### Static recompilation
You lift the machine code mechanically into C and compile that against a
runtime that emulates the original platform. Recent examples are N64Recomp
(DK64, Banjo), XenonRecomp (Xbox 360) and an Xbox x86 recomp
([BLiNX 2](https://raw.githubusercontent.com/apexJCL/blinx2-recomp/HEAD/README.md))
that targets macOS. It gets a game running natively quickly, but the output is
not maintainable source. For a Windows game you'd also have to emulate Win32,
D3D9, DirectSound and DirectInput, which brings back most of the porting work.
It could still serve as a stopgap for code we haven't decompiled yet.

### Hybrid / incremental replacement
You load your own DLL into the original process, as OpenSpy already does, and
redirect original functions to rewritten ones one at a time. The game stays
playable the whole way through, so each rewrite can be checked against the
original in a real match. re3 and many PC decomps used this approach. On a Mac
this requires running the 32-bit game under Wine or CrossOver, because Rosetta
2 cannot run 32-bit x86 code.

## 2. Tooling

- **Ghidra** (12.1.x, installed through Homebrew) is the main disassembler and
  decompiler. Our headless pipeline is in `tools/ghidra/`.
- **AI-assisted decompilation** is now common practice:
  - LLM agents connect to Ghidra over MCP servers such as
    [GhidraMCP](https://github.com/13bm/GhidraMCP) and GhydraMCP
    ([River Raid write-up](https://quesma.com/blog/ghidra-mcp-unlimited-lives/)).
  - Mizuchi runs the write/compile/compare loop with AI models plus
    decomp-permuter and objdiff.
  - One PC project reportedly matched 83% of *Call of Duty: Modern Warfare 2*
    byte for byte with AI agents in about three months
    ([write-up](https://gambiconf.substack.com/p/development-journey-on-game-decompilation)).
  - Caveat: match-rate metrics count matching bytes, not readable names or
    structure. Human review is still needed.
- **Microsoft symbol servers** have symbols for system DLLs only, not for the
  game. DICE's PDBs (`...\Patch_1_51in\*.pdb`) were never shipped.

## 3. What makes BF2142 easier

- **No packing or DRM** on the 1.51 binaries, and every module keeps its
  relocations.
- **Embedded source paths**: about 75 `__FILE__` strings across the modules
  give the original directory layout and tie many functions to their files.
- **Python game logic**: game rules, stats, unlocks and the server-side game
  modes ship as `.py` source. The C++ side just has to expose the same `host`
  and `bf` modules.
- **HLSL shader source**: `.fx` files ship in plain text, so they can be
  translated instead of decompiled.
- **gameswf**: the Flash UI player is built on public-domain open-source code.
- **Module boundaries**: the renderer, AI, audio and UI are separate DLLs with
  small interfaces (`initDll`/`deinitDll`), which are natural seams to port.
- **OpenSpy**: the network backends (GameSpy, FESL, Stella stats) have an open
  server implementation ([openspy-core](https://github.com/openspy/openspy-core))
  and known patch points in the client
  ([bf2142_rendhook](https://github.com/openspy/bf2142_rendhook)).
- **Shared engine with BF2**: Refractor 2 knowledge from the BF2 modding
  community applies here, and so may a public BF2 rebuild once it is released.

## 4. What makes it harder

- **RTTI is disabled** (only CRT and STL type info is present), so class names
  have to be recovered from strings, Python bindings and context.
- **32-bit x86 only**: Apple Silicon has no way to run the original code
  natively. The recompiled code must be 64-bit clean. On-disk formats and the
  network protocol use 32-bit layouts, so serialisation has to be explicit.
- **Direct3D 9 + D3DX effects**: there's no maintained D3D9-on-Metal library
  you can just link. DXVK needs Vulkan features that MoltenVK only partly
  provides. The renderer needs a real backend abstraction.
- **Size**: Ghidra finds 30,072 functions in `BF2142.exe` alone. Only 117 of
  them reference an assert source path, so most functions will have to be
  placed by call-graph proximity, since MSVC lays out functions in object-file
  order.

## 5. Graphics options for macOS (cross-platform preferred)

| Option | Notes |
|---|---|
| **SDL3 GPU** | Vulkan, Metal and D3D12 backends. **SDL_shadercross** turns HLSL into MSL or SPIR-V. It pairs naturally with SDL3 for windowing and input. |
| **bgfx** | Mature, used in many shipping games. Has its own shader language and toolchain, so the `.fx` files would need converting. |
| **Vulkan + MoltenVK** | Portable, but adds a translation layer on macOS. |
| **DXVK-native / wined3d** | Lets the D3D9 code stay, but runs badly or not at all on Metal. |

Leaning: **SDL3 + SDL3 GPU + SDL_shadercross**, the same stack isle-portable
uses, with the `.fx` effect files handled by a small effect-framework layer in
our code. Nothing is decided until the renderer is analysed.

## Sources

- [doldecomp/melee](https://github.com/doldecomp/melee)
- [Development Journey on Game Decompilation Using AI](https://gambiconf.substack.com/p/development-journey-on-game-decompilation)
- [Game decompilation using AI (macabeus)](https://macabeus.medium.com/game-decompilation-using-ai-4d47b65f8852)
- [fmtk wiki: Decompilation](https://github.com/widberg/fmtk/wiki/Decompilation)
- [isledecomp/isle](https://git.mattkc.com/isledecomp/isle) and [isle-portable](https://git.mattkc.com/isledecomp/isle-portable)
- [Reverse engineering River Raid with Claude, Ghidra, and MCP](https://quesma.com/blog/ghidra-mcp-unlimited-lives/)
- [13bm/GhidraMCP](https://github.com/13bm/GhidraMCP)
- [BLiNX 2 recomp README](https://raw.githubusercontent.com/apexJCL/blinx2-recomp/HEAD/README.md)
- [DK64 Rekongpiled](https://hwbusters.com/news/dk64-rekongpiled-hits-1-0-running-donkey-kong-64-natively-on-pc-without-an-emulator/)
- [Battlefield 2 open-source revival (TweakTown)](https://www.tweaktown.com/news/114023/battlefield-2-is-getting-an-unofficial-open-source-revival-with-working-multiplayer/index.html)
- [Introducing SDL_shadercross](https://moonside.games/posts/introducing-sdl-shadercross/)
- [Wine Vulkan on macOS via MoltenVK (Phoronix)](https://www.phoronix.com/news/Wine-macOS-Vulkan-MoltenVK)
- [openspy/bf2142_rendhook](https://github.com/openspy/bf2142_rendhook)
