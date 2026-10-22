# Binary inventory

Everything here describes the install in `source/Battlefield 2142` (patch **1.51**
plus the OpenSpy patch). All DICE modules are 32-bit x86 PE files built with
**MSVC 7.1 (Visual Studio .NET 2003)**, linker version 7.10. They link the
dynamic CRT (`MSVCR71.dll` / `MSVCP71.dll`) and are not packed or encrypted.
Each one keeps its `.reloc` section, which makes it easier to tell pointers from
plain integers during analysis.

## First-party modules (to be decompiled)

| Module | Size | Built | Role | Exports / interface | Notes |
|---|---|---|---|---|---|
| `BF2142.exe` | 7.1 MB | 2011-02-09 | Engine core, game logic, networking, menus, physics, Python host | – | `.text` is 5.1 MB. PDB path `C:\dice\Projects\BF2142Branches\Patch_1_51in\BF2142.pdb` (PDB not shipped). 49 distinct source paths embedded. |
| `RendDX9_ori.dll` | 2.2 MB | 2011-02-09 | Direct3D 9 renderer | `initDll`, `deinitDll` | Original renderer. `RendDX9.dll` is the OpenSpy proxy (see below). Imports `d3d9`, `d3dx9_29`, `binkw32`. |
| `AIDLL.dll` | 1.3 MB | 2011-02-09 | Bot AI, navmesh path finding, strategy | `initDll`, `deinitDll` | Only imports `Memory.dll`, the CRT and `USER32`, so it's almost pure logic and an easy porting target. |
| `SwiffPlayer.dll` | 1.2 MB | 2011-02-09 | Flash (SWF) UI player | `initDll`, `deinitDll` | Built on **gameswf** (`BF2/External/gameswf/SwiffPlayer`), an open-source, public-domain library, so it can be ported from source instead of decompiled. |
| `BF2142Audio.dll` | 304 KB | 2011-02-09 | Sound engine | `getSoundEngine`, `shutdownSoundEngine` | Imports DirectSound. Source dir `BF2/Sound/SoundEngine`. Nothing references `BF2OpenAL.dll` by an ASCII name, so how (and whether) it gets loaded isn't confirmed yet. |
| `TextureAtlasBuilder.dll` | 268 KB | 2011-02-09 | Texture atlas tool | – | Imports `Memory_r.dll`, which isn't shipped, so this is probably dead tooling. |
| `BF2VoipServer.dll` | 61 KB | 2011-02-09 | VoIP server | – | Imported by `BF2142.exe`. |
| `Memory.dll` | 40 KB | 2011-02-09 | Allocator (`dice::hfe::memory::bf_malloc`/`bf_free`, tracing) | C++ mangled exports | Every DICE module imports it, so port it first. |
| `BFMFC.DLL` | 1 MB | 2005-01-17 | DICE-modified MFC | – | Only for tools and launchers. Not needed for the game. |

`BF2142.exe` loads these modules at runtime, not through its import table:
`RendDX9.dll`, `AIDLL.dll`, `SwiffPlayer.dll`, `BF2142Audio.dll`,
`bf2142haptics.dll`, and the PunkBuster DLLs in `pb/`.

## Third-party modules (to be replaced)

| Module | What it is | Replacement plan |
|---|---|---|
| `dice_py.dll` | Embedded **Python 2.3.4** (DICE build, 2005) | CPython 2.3.x built from source with the same `host`/`bf` built-in modules (which the scripts import), or a later compatible 2.x build. The game scripts in `python/` and `mods/*/python` ship as `.py` source. |
| `pylib-2.3.4.zip` | Python standard library | Ships with the replacement interpreter. |
| `d3dx9_29.dll` | D3DX (effects, math, textures) | Our own implementation of the parts the renderer uses. Shaders ship as HLSL `.fx` source in `mods/bf2142/Shaders_client.zip`. |
| `binkw32.dll` | Bink 1 video (2002) | FFmpeg's `libavcodec` has a Bink decoder. |
| `BF2OpenAL.dll` | Creative OpenAL wrapper (`wrap_oal`) | OpenAL Soft. |
| `zlib122.dll` | zlib 1.2.2 | System or vendored zlib. |
| `PCRegExp.dll` / `PCRegExp_r.dll` | PCRE wrapper (DICE build) | PCRE2. |
| `dbghelp.dll` | Crash dumps | Drop it or use a platform crash reporter. |
| `mfc70`, `MFC71`, `msvc{r,p,i}7x/80` | Runtime libraries | Not needed. |
| `pb/*.dll` | PunkBuster anti-cheat | Drop. The service has been shut down. |
| `bf2142haptics.dll`, `NovintHFX.dll` | Novint Falcon haptics | Drop. |
| `FireWallDLL.dll` | Windows Firewall registration | Drop. |
| `SD*.dll` | Localised SecuROM/installer strings | Drop. |

## OpenSpy patch

OpenSpy doesn't modify `BF2142.exe` on disk. It ships its own `RendDX9.dll`
([openspy/bf2142_rendhook](https://github.com/openspy/bf2142_rendhook)) that
forwards `initDll`/`deinitDll` to `RendDX9_ori.dll`. When the game loads it,
its `DllMain` patches the in-memory image:

1. **Hostnames:** it rewrites `*.gamespy.com` strings to `*.openspy.net`
   (GP, GameStats, QR, server browser, availability check, Stella stats).
2. **FESL login:** it replaces the EA FESL backend hostname with
   `fesl.openspy.net:18301` through a vtable slot. It also swaps the game's
   built-in SSL stack (old SSLv3/RC4, probably EA DirtySock ProtoSSL) for
   OpenSSL with TLS 1.2+, by detouring every call to the game's SSL
   update/send/recv functions.
3. **Bug fixes:** it skips login error −206 and fixes a `FindNextFile` hang
   when deleting a soldier profile.

`symbols/BF2142.exe.csv` lists every address it touches. The recompiled game
should carry these changes into source form: point the hostnames at OpenSpy
(configurable), use a modern TLS library for FESL, and include both bug fixes.

## Original source layout

The binaries embed `__FILE__` paths from asserts. The root is
`C:\dice\Projects\BF2142Branches\Patch_1_51\Code\`. Most-referenced directories:

```
BF2/Game/{GameLogic,GameServer,GameClient,Common,Persistence,Python,MatchMaking,Objects/...}
BF2/Physics, BF2/Scene/Object, BF2/IO/{File,Network}, BF2/Menu, BF2/Core
BF2/Geom, BF2/Geom/Overgrowth, BF2/GeomMisc/MenuRend      (renderer DLL)
BF2/AI/AIDLL/AI/{Main,AIBotActionPlan,AIPathFinding/NavMesh,...}   (AI DLL)
BF2/Sound/SoundEngine                                      (audio DLL)
BF2/External/gameswf/SwiffPlayer                           (Flash UI DLL)
```

Python-facing class names also appear as strings, for example
`dice.hfe.ServerBrowserManager` and `dice.hfe.VideoSettings`, which gives the
`dice::hfe` namespace. `tools/ghidra/scripts/TagSourceFiles.java` uses these
paths to tag functions with the file they came from.
