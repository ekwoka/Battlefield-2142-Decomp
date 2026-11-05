# Uncited code in BF2142.exe, RendDX9_ori.dll and AIDLL.dll, classified (HEAD 6001a99, 2026-10-29)

## Method

- **Citations.** I re-ran `tools/cite2.py` on src/ and docs/ at HEAD 6001a99, which is two commits after coverage-2's 991d041. Cited bytes are now: EXE 955,445 B (19.9%, up from 17.8%), RendDX9 224,810 B (13.4%), AIDLL 260,114 B (25.9%).
- **Per-function features.** For every function in functions.csv (static_init excluded), I extracted from `build/analysis/<mod>/decomp/*.c`:
  - its string literals and `s_` labels;
  - the functions it calls;
  - the vtables it references.
  - Script: `unc/extract.py`; output `unc/<mod>.json`.
- **Classifier** (`unc/classify.py`, in priority order):
  1. Cited functions stay cited.
  2. Imports and EH thunks go to (a).
  3. **Console rows** (`symbols/*.console.csv`): each row's constructor and its vtable's unique slots are labelled by the row's state.
     - Row state comes from coverage-2's `rows_head.json`. Because that file is two commits stale, a row whose method or full `Obj.method[.list]` name is now a string literal in src is upgraded.
     - impl / stored-read → (c). Stub / unread → (d), or (b) if the name is debug-like.
     - `template_properties.csv` executors are labelled the same way.
  4. **Library signatures** in the strings go to (a): zlib, Speex, GameSpy, HMAC, Logitech LCD, FESL/DirtySock/EASTL, PunkBuster, TinyXML, IGA/Radial, DxDiag.
  5. **Hand-checked address ranges** for the statically linked SDK blocks go to (a). Cited functions inside those blocks stay cited.
  6. **Editor and dev code** goes to (b): `.con` writers (strings like `"ObjectTemplate.radius "` streamed out are the template save path) and debug, profiler and editor strings.
  7. **String matching against src.** A function's distinctive strings are matched against the literals and identifiers in src (`unc/srccorpus.py`; stb_image and UI/Avm1 are excluded as noise). Half or more found → (c), with the matching src file. Under a quarter → (d).
  8. **STL.** Callers of the `vector<T> too long` / `invalid map/set<T> iterator` throwers go to (a).
  9. **Propagation** for functions with no strings. In order: callee-of (every caller has one label), vtable siblings (70% agree), sandwiched between equal labels, then adjacent.
  - RendDX9 functions whose only strings are D3D9 technique and parameter names count as (c) "D3D9 backend plumbing, substituted" (the rewrite has its own GLSL pipeline).
- **Calibration of propagation.** I hid the labels of the string-labelled functions and predicted them from neighbours and callers. Predicted-(c) precision by bytes was:
  - EXE: 0.84
  - RendDX9: 0.69
  - AIDLL: 0.48 (the AI is a partial stand-in, so neighbours of cited code are often still missing)
  - The calibrated numbers below discount propagated (c) by these factors and split "?" pro rata.

Raw outputs (kept outside the repo):
- `lab_<mod>.json`: the label of every function;
- `report.txt`;
- `dclus_exe.txt`: the (d) runs;
- `map_*.txt`: 16 KB address maps.

## Headline numbers

| Module | Total | Cited (raw) | (a) third-party | (b) dev/editor | Needed code (total − a − b) | Effective: lower (cited + string/console-evidenced c) | **Effective: calibrated** | Effective: upper (all c + ?) | (d) missing (calibrated) |
|---|---|---|---|---|---|---|---|---|---|
| BF2142.exe | 4,794,004 B | 19.9% | 884 KB (18.4%) | 177 KB (3.7%) | 3.73 MB | 45.9% | **≈78%** | 84% | ≈0.8 MB |
| RendDX9_ori.dll | 1,677,614 B | 13.4% | 256 KB (15.2%) | 165 KB (9.8%) | 1.26 MB | 29.7% | **≈70%** | 88% | ≈0.37 MB |
| AIDLL.dll | 1,006,188 B | 25.9% | 92 KB (9.2%) | 29 KB (2.9%) | 0.88 MB | 33.9% | **≈60%** | 86% | ≈0.35 MB |
| All three | 7.48 MB | 19.3% | 1.23 MB | 0.37 MB | 5.87 MB | 40.6% | **≈73%** | 85% | ≈1.5 MB |

Reading:
- After excluding third-party and dev code, about three-quarters of the needed code is cited or behaviourally present.
- The hard floor, counting only functions with direct string or console evidence, is about 41%.
- The AIDLL figure is the softest: its implemented code clusters in cited behaviours, and much of its uncited bulk is AI console commands, debug drawing and navmesh tooling.

## (a) Statically linked third-party and compiler code: not needed (1.23 MB)

| Block | Range | Size | Notes |
|---|---|---|---|
| EA FESL SDK 2.8 / DirtySock (Plasma, ProtoTunnel/ProtoUDP, UPnP) / EASTL | EXE 0x007fecf0–0x0086c660 | 380 KB | Online only. The rewrite talks FESL itself (src/Net/Fesl.cpp, OpenSpy). |
| GameSpy SDK (GP, GT2, HTTP, Voice 2), zlib 1.2.2 inflate, Logitech LCD SDK, HMAC/SHA-1, Speex codec | EXE 0x0070fa90–0x00745ea8 | 208 KB | Includes zlib at 0x00733b60, the MD5 context at 0x0071b140 / 0x0071bac0 / 0x0071bb80 (it was listed as zlib; the content check feeds it, now cited by Net/Md5.h), the LCD SDK at 0x00736020 and 0x00739d90, HMAC at 0x00738670, and Speex from ≈0x0073c890 to 0x00745ea7 (`warning: %s`, 0x0073e0d0). Coverage-2 called 0x007380d0–0x00745ea7 an "https library"; it is HMAC plus Speex. Speex and zlib are linked as libraries in the rewrite; Voice 2 is cited (src/Voip). |
| STL template instantiations | All modules | EXE 176 KB, RendDX9 135 KB (including the iostream tail 0x101a2070–0x101a60b0), AIDLL 77 KB | |
| Compiler static initialisers / CRT glue / import and EH thunks | EXE 0x00900c20–end, RendDX9 0x101aa0c0–, AIDLL 0x10101300– | ≈130 KB | |
| PunkBuster | EXE 0x008c5d10–0x008c9eac | 24 KB | |
| IGA / Radial in-game ads | RendDX9 0x10053e80–0x1005fa50 | 58 KB | |
| TinyXML | EXE ≈0x00640c10 | 8 KB | |
| DxDiag | RendDX9 0x10005010 | 8 KB | |

Python and the MSVC runtime are imported DLLs (dice_py, msvcr71/msvcp71), not linked in.

## (b) Editor, debug and dev-only code (0.37 MB)

| Kind | Size | Examples | Should the rewrite have it? |
|---|---|---|---|
| `.con` writers (the editor's template and object save) | EXE 80 KB, RendDX9 5 KB, AIDLL 4 KB | 0x005ad6c0–0x00649d80, interleaved with the property executors | No. The one exception, SettingsManager's profile save, is already done (SettingsObjects.cpp:411). |
| Terrain, road and overgrowth editors; texture-atlas and lightmap tools | RendDX9 ≈100 KB | `TerrainEditable`, `RoadEditable`, `_u8888.dds`, `-nomipmap`, `tile_`, `../RawData/Undergrowth/`, `LightmapGeneration.*` (146 functions of stubbed rows, 0x1008232f–0x100854ea), `dice_TextureAtlasTool` | No. |
| Debug drawers and asserts | RendDX9 0x10189c30–0x101aa0b5 (DebugLineDrawer, DebugRenderer, shaders, DxAssert); EXE `_debugMesh_`, `coll-*` / `phy-show-*` (0x006d6910–0x006deb65) | | No. |
| Profilers and stats | EXE 0x0065e0f0–0x0066639e (Global Timer, fps / PageFaults); RendDX9 0x10025340–0x10043790 (`%-20s Avg`, SetVB/SetTex counters) | | Optional. A small fps/frame-time overlay (`renderer.drawFps`) is the only player-facing bit. |
| AI debug | AIDLL `ai.draw*`, `pauseAllBots`, BAP statement printers (`StartCondition:`), the navmesh generator's `/GTSData/Output` writers and `MaxSlope…` dumps | | No. The generator is offline tooling. |
| Dev console methods | `Console.get*`, `collisionManager`, `NetworkableInfo`, `Profiler` (≈20 KB) | | Optional. `Console.getArgumentTypes`/`getRange` would help modders, which is low priority. |
| The geometry optimizers (coverage-6 §2g rows 4, 5 and 9, classified 2026-11-05) | RendDX9 ≈24 KB: 0x100f7b80–0x100f9607 (6.3 KB), 0x100f96b0–0x100fc371 (10.0 KB), 0x10170370–0x10172941 (7.7 KB) | `dice.hfe.geom.GeometryOptimizer.BundledMeshOptimizer` (factory 0x10070805 → constructor 0x100f6f00, vtable 0x101d6da0) and `.StaticMeshOptimizer` (0x10070847 → 0x100f9610, vtable 0x101d6e18), both on the base 0x10170290 (vtable 0x101da970, slots 0x1016f3d0–0x1016fd30: the `gpu_` mesh name, 0x1016f5f0). The runs are the two vtables' own slots (0x100f8860–0x100f9330, 0x100f9720, 0x100fc300–0x100fc350), the base's (0x10170450, 0x10172900 / 0x10172920), their hash-map and vector helpers (no strings; `map/set<T> too long` at 0x10170810) and 0x100f8950 (GetSystemInfo, a +0x68 × +0x6c table). The classifier's propagated (d) came from no evidence at all. They are not the sky dome or the name tags: the SkyDome's technique choice is 0x100fc410, its flare loader 0x100fc9f0, its effect 0x100fcd80 (cited in LevelRendererPost.cpp), and the name tags start at 0x1016c1e0. | No: a build tool for the `gpu_`-named optimised meshes, not used by the game's own loading here (the rewrite loads the shipped meshes as they are). |

## (c) Implemented in the rewrite but uncited (citations could be added)

- EXE: 757 KB evidenced plus 1.27 MB propagated (≈84% reliable).
- RendDX9: 148 KB plus 623 KB.
- AIDLL: 40 KB plus 408 KB.

### Largest evidenced runs, with the best-guess src file

| Range | Size | Contents | Likely src |
|---|---|---|---|
| EXE 0x007cb760–0x007d7047 | 44 KB | awards / medals pop-up (`HUD_HUD_AWARDED`, `AWARD_Name_%i`) | **Checked: a false (c).** The pop-up (`0x007d1270`: `HUD_HUD_AWARDED` with #AWARD#, `Ingame/GeneralIcons/Medals/`) isn't in src; MenuBindings has only the menu's award lists. It shows the server's Medal events, which no offline game raises. |
| EXE 0x0040ec54–0x004193c7 | 40 KB | SwiffHost / GeneralSettings / ProfileManager console objects | UI/Player/NativeTable.cpp, Main/SettingsObjects.cpp, Main/MenuBindings.cpp |
| EXE 0x00784cc0–0x0079a035 | 39 KB | minimap, spawn and kit screens | Main/main.cpp, Main/HudMenusKits.cpp. **Cited since:** the map node's constructor `0x00784cf0`, its event handler `0x00788a90`, its zoom event `0x007918d0`, the kit screen's variables `0x00796d80`, uav_offline `0x00788600`. |
| EXE 0x00506210–0x005144df | 31 KB | admin, ban lists, UB_* | Main/ServerAdmin.cpp. **Cited since:** the admin rows `0x00429bba`..`0x0042adfc` with the ban-list functions each calls (`0x005077b0`..`0x00508100`), and the listing at `0x00506b60` (ServerAdmin.cpp, Net/ServerSettings.cpp BanList). |
| EXE 0x0041aa74–0x00422035 | 28 KB | GameServerSettings console | Net/ServerSettings.cpp. **Cited since:** each sv.* field's row constructor and its executors on that field's line, plus sv.load and sv.save (`0x0041b003`, `0x0041b0a1`). |
| EXE 0x004b9220–0x004c4909 | 26 KB | spawning / kill player | Game/GameLogic*, Game/GameWorld.cpp |
| EXE 0x006a4760–0x006aa7ce, 0x0068e8d0–0x00693365, 0x0066cf6d | ≈40 KB | GI*/PI*/IDKey enum tables, control maps | Input/ControlMap.cpp. **Cited since:** the GameInput writer `0x006a5190` and its readers `0x006a5880` / `0x006a7780`, and the PlayerInput writer and reader `0x0068f5a0` / `0x0068fb90`. The rest of these ranges is list and stream plumbing. |
| EXE 0x0075c6b0–0x00761c05 and HudBuilder (0x0075f91a–0x00773b23) | | | Hud/Hud.cpp (62 KB of evidenced functions in total). **Cited since:** the method objects' pattern at `registerHudConsoleMethods` (createTextNode `0x0076cd5a` / `0x0076ce5e`, setTextNodeStyle `0x0076d28f` → `0x007af920`; `0x0075cba0` the HUD manager's destructor). |
| EXE 0x007720e9–0x00775d90 | 15 KB | commander / order menus | Main/HudMenus.cpp, Main/HudMenusComm.cpp |
| EXE 0x007b3f40–0x007bd15d | 14 KB | chat and kill messages | Main/HudMenusChat.cpp |
| EXE 0x00495620–0x0049bf2a | 14 KB | Video/Audio/Global.con settings | Main/SettingsObjects.cpp |
| EXE 0x005662b0–0x0056bb9f | 12 KB | python `host.*` | Game/ScriptBridge.cpp, Python/ScriptHost.cpp |
| EXE 0x004aadc0–0x004b372f, 0x00686f90–0x0068cc19 | 24 KB | archive.md5, Client/ServerArchives.con | IO/File/FileManager, Net/Md5.cpp, Main/HostedGame.cpp |
| EXE 0x006700e5–0x00676ada | 12 KB | console parser, `SettingsManager.*Set` writer | Console/Console.cpp, Main/SettingsObjects.cpp |
| EXE 0x005d9980–0x005e1621 | | weapon sounds (BoltClick, Fire1P_Outdoor) | Sound/Game/GameSounds.cpp |
| EXE 0x007efbc0–0x007f43d9 | 15 KB | Sound console | Sound/Game/SoundConsole.cpp |
| RendDX9 0x1007c55b–0x1007f92a, 0x10077640 | | sky / light settings | Scene/LightSettings.cpp |
| RendDX9 0x1008035c–0x1008232f | | shadow settings | Scene/PostProductionSettings.cpp |
| RendDX9 0x10134220–0x10149f29 | 68 KB | particle and mesh renderers | Effects/Effects.cpp, Render/LevelRenderer.cpp |
| RendDX9 0x1010a0b0–0x10112b1c | | undergrowth | Terrain/Undergrowth.h |
| RendDX9 0x10119f60 | | RainManager | Scene/LightSettings / Render |
| RendDX9 0x100d2e20, 0x100a0b80 | | skinned / bundled mesh loaders | Geometry/GeometryTemplateManager.cpp |
| AIDLL 0x100781e0–0x100884df, 0x1006a9c0, 0x100718e0 | | behaviours (Fire3d, CallForMedic, RepairVehicle…) | Game/Bots.cpp |

Evidenced (c) bytes by src file:
- EXE: Hud.cpp 62 KB, GameWorld.cpp 47 KB, Effects.cpp 45 KB, MenuBindings 44 KB, ControlMap 30 KB, main.cpp 27 KB, Vehicle 24 KB, Bots 24 KB, SoundConsole 24 KB, SettingsObjects 22 KB, ServerSettings 22 KB, Weapons 18 KB, WorldConsole 17 KB, AnimationConsole 16 KB.
- RendDX9: LightSettings 30 KB, PostProductionSettings 15 KB, GpuContext 11 KB.

Per-function file guesses are in `lab_<mod>.json` (`ev` = "file <- matched string").

Not needed one-to-one:
- The generic **dice::meme** framework (EXE 0x0086c660–0x008806e0, ≈80 KB: MemeFile 2.0 streams, actions, events, effects) and the uncited Bf2*Node classes (0x007da500–0x007f2580). They are the original HUD's substrate; src/Hud replaces them with its own node tree, so only the node behaviours matter, and those are largely cited.
- RendDX9's D3D9 effect / shader-permutation management (0x10019000–0x1004b000), which the rewrite's GLSL pipeline replaces.

### RendDX9: replaced by design (render pass 5)

The D3D9 shader-management, device and resource code. The rewrite's own SDL GPU layer and its GLSL shaders (src/Render/GpuContext, Shaders/) stand in for all of it, so it is classed with (c) as substituted, not ported. The anchors, by job:

| Job | RendDX9 functions | Size | Stands in for it |
|---|---|---|---|
| The effect loader and the shader-permutation generator: the `.fx` preprocessor and its `#include` / `RaDefines.fx` reader, the "AutoGenerated shader file" writer, the precompiled `%s_%u_dfx.bin` cache and the GSModDirectory `@HOME@/…/cache/` paths, the effect compiler's shader-version and vendor defines (`PSVERSION`, `ps_2_0`, `PS2_EXT`, `USEPARTIALPRECISIONTYPES`, `#define NVIDIA …`) | `0x1001b3f0` (1,524), `0x10020a80`, `0x100214b0` (3,274), `0x10022310` (the effect pool), `0x100313b0` (1,073), `0x100320a0` (2,456), `0x10032a50` (2,512), `0x10033590` (ShaderCache), `0x10034c30` (1,080), `0x10039dc0` (3,062), `0x1003af60` (1,556), `0x1003b580`, `0x1003ea70` | ≈25 KB of named functions in 0x10019000–0x1004b000 | The shaders are compiled at build time (src/Render/Shaders → SPIR-V / MSL) |
| The per-effect state blocks and technique / parameter bookkeeping (`_States`, `Restore`, `defaultTechnique`, `reqVertexElement`, `InstanceParameters`, `TemplateParameters`), the vertex-declaration builder (`Position`, `PositionPacked`, `NormalPacked8`…) | `0x1001e1a0`, `0x100462b0` (2,062), `0x10046ad0` (2,658) | ≈6 KB | GpuContext's pipelines and their vertex layouts |
| The common shader constants each frame (FogColor, FogRange, CameraPos, GlobalTime…) | `0x100360e0` (3,805) | 4 KB | The uniform blocks LevelRenderer pushes (Fog.h, `lighting()`) |
| The device: Direct3DCreate9, the window and its resolution and full-screen events (`ECRendererREToggleFullscreen`, `ECRendererREChangeResolution`, `GSWindowSizeX/Y`), the VideoSettings reader | `0x1000bee0` (740), `0x1000fc40` (782), `0x1000ba30` (879) | 2.4 KB | SDL's window and GPU device (Main/Viewer.cpp) |
| Surface and texture loading and saving through D3DX (D3DXLoadSurface…, D3DXFilterTexture, D3DXSaveTexture), the screenshot writer (`renderer.dumpScreen`, `@HOME@/Screenshots`, `.jpg` / `.png`) | `0x100039f0`, `0x10006270`, `0x10006460` (858), `0x10017460`, `0x10064b20`, `0x10154110`, `0x10177bb0`, `0x10179960` | ≈6 KB | Texture/DdsFile, Texture/TgaFile, Render/PngWriter |
| The draw-call and memory statistics (`SetVB`, `SetIB`, `SetVDecl`, `numDrawsPerSecond`, `VertexBufferMemory`, `shaderName`) | `0x1002c230` (2,397), `0x1002bb90` (1,674), `0x10042540` (1,829), `0x10042120` (1,043), `0x10026990` (2,877) | ≈10 KB | (b) profilers; only `renderer.drawFps` is player-facing, and it is done (`0x10027570`, engine.md "The renderer's switches") |

The console rows that only reach this layer are kept as stored values: `renderer.vsync` / `presentAsync` (`0x1000adb0` / `0x1000adf0`), `useSunScaling` (device +0x41), `alphaToMask` (+0x57c), `enableHWOcclusion` (`0x10004900`), `renderAheadLimit` (slots 0xb0 / 0xb4); `GeometryTemplate.compressVertexData` / `maxTextureRepeat` / `lightmapOverride` / `doNotGenerateLightmaps` / `setLightmapSizeAll` and `Object.geometry.GenerateLightmapForThisInstance` are the lightmap tools' and the vertex packer's (b); `GeometryTemplate.isAd` is IGA's (a).

**Status (render pass 5).** Newly implemented and cited in RendDX9: the draw switches and the views that read them (`0x1004c7a0`..`0x1004e0b0`, `0x1002cef0`, `0x100264c0`, `0x10027630`), `renderer.drawFps` (`0x10053ae0`, `0x10027570`), the near planes (`0x10051b40`, `0x10051ce0`), the road renderer's distance and fade (`0x100ba7e0`, `0x100baa70`, `0x100ba1d0`, `0x10070c42`, `Renderer.road*` `0x100727ee`..`0x10072b02`), `StaticMeshRenderer.enableRendering` / `noLods` (`0x10079e04`, `0x10079f17`), `rainManager.freeze` (`0x1007fc4b`, `0x1011ab20`, `0x1011d250`), the undergrowth's sway (`0x1010ad20`, `0x1010a900`, `0x1010f600`) and view distance (`0x100866ff`, `0x10086806`, `0x100863f6`, `0x100860c9`, `0x1010e270`, `0x1010d240`), the overgrowth's view distance (`0x1008f3b0`, `0x1008b7a0`, `0x1008b580`, `0x10120ef0`, `0x10121410`, `0x1008da30`), the decals' clipping and doubled ageing (`0x10166410`, `0x10165f60`, `0x101655c0`, `0x10165020`, `0x10164d70`, `0x10164940`, `0x10164f10`, `0x101647d0`, `0x101646b0`, `0x10164040`, `0x100b84b0`, `0x100b8380`, `0x100b71c0`), and the video classes (`0x1011dcc0`, `0x1011dd00`, `0x1011e0d0`, `0x1011df80`, `0x10178080`, `0x10118270`; routed as src/Movie already does). RendDX9 console methods stubbed at start-up: 185 → 147.

### RendDX9: the missing-code pass (coverage-6, 2026-11-05)

The classifier's (d) runs of 1 KB or more (no audit docs), each settled. Those the rewrite does are now cited in src (coverage-6's last status block lists them); the rest are below, with the reason the rewrite has no counterpart.

| Run | Size | What it is | Class |
|---|---|---|---|
| `0x100ce6c0`..`0x100cffe1`, `0x100cbc90`, `0x100c7400` | 6.4, 1.8, 1.2 KB | TerrainEditable's `terraindata.raw` writer (`0x100cf8e0`, file opened for writing) and its helpers (`0x100cf5a0`, `0x100cab70`, `0x100b1bf0`) | (b) editor |
| `0x100ca150`..`0x100cb3e4` | 4.3 KB | the `Misc\nvdxt` / `Misc\tgasplit` command lines | (b) texture tools |
| `0x10158520`..`0x10159d29` | 4.0 KB | `-u8888 -nomipmap`, `_%i_tmp`, `%s_%i` .dds splitting for the terrain editor (`0x100ccab0`) | (b) texture tools |
| `0x1002bb90`, `0x1002f3d0`..`0x100300a1`, `0x10030140`, `0x10030950`, `0x1002b4d0`, `0x10042ca0`..`0x1004360a` | 4.1, 3.0, 1.7, 1.1, 1.3, 2.0 KB | the draw statistics and their containers (`avgFR` / `maxSM` tables `0x1002b870`), the named profiler sections (`0x100430b0`, e.g. "DrawWater", "NSAParticleSystem") | (b) profilers |
| `0x100118d0`..`0x1001239c` | 2.4 KB | the "VertexBuffers" memory dump | (b) |
| `0x10179bf0`..`0x1017a601`, `0x1017a690` | 2.4, 1.4 KB | the lightmapper's samples (`SMP1` / `SMP2`, `.samples`, `.samp_%d%d` from `0x10152ce0`) | (b) lightmap tools |
| `0x10003b10`..`0x10004466` | 2.3 KB | the frame capture to `%s\%02d\frame%06d.tga` | (b) |
| `0x100f6330`, `0x100f6f90`, `0x1016f370` | 2.9, 2.2, 3.7 KB | more of the geometry optimizers above (BundledMeshOptimizer's slots, the base's writer `0x1016f810` that renames `default_sdgbmfbf_color_lut` to the SpecularLUT) | (b) build tool |
| `0x100ff650`..`0x1010037f` | 1.9 KB | the road compiler (`_compiled.mesh` writer, `projectroad`, `c:\tbmTest.dds`) | (b) editor |
| `0x10121930`, `0x1008d1f0`, `0x1008767d`, `0x1008bae0` | 2.0, 1.7, 1.6, 1.6 KB | the Growth base's painted maps: `Overgrowth.raw` (the material per cell, +0x80) and `OvergrowthShadowmap.raw` (+0x84), read at the preLoad and written by `Overgrowth.paintMaterial` (`0x1008d1f0` from the row `0x100878ac`), `paintShadowmap`, `save` (`0x1008c0f0`) and the other editor rows; the game only reads them back, and nothing it draws uses them (engine.md, "Overgrowth") | (b) editor |
| `0x1007a01f`..`0x1007aa09` | 1.4 KB | StaticMeshRenderer.materialCullingEnabled / enableLightMapAtlas / convertLightmapName (`0x100bb170`, `=00=%d=%d=%d`) / enableNewLightmapNames / enableUnifiedIndexlist / maxLightMappedLod / enableLightIndexes / allowAnisoFiltering, SkinnedMeshRenderer.shaderQuality: no shipped script calls them; lightmap-naming and D3D9 index-list switches | (b) dev rows |
| `0x1001af30`..`0x1001be55`, `0x1001a680`, `0x1001c040`, `0x1001d0f0`, `0x10034c10`, `0x1003b580`, `0x10047910`, `0x10030ec0` | 3.7, 1.1, 1.3, 1.3, 1.3 KB and smaller | the `.fx` preprocessor (`#include`, `#define`, `const … : LI…` scanners), its maps, `RaDefines.fx` / `GSLoadObfuscated`, the technique-name lookup (`%s_%iX`), the obfuscated shader cache's hash | replaced by design (shaders built at compile time) |
| `0x10009cc0`..`0x1000a667`, `0x100092f0`, `0x1000fb60`, `0x1000af90` | 2.2, 1.2, 1.0, 1.1 KB | the display-mode lists (`%ix%i@%iHz`, WideScreenEx, `%dSamples`, GSWindowSizeX/Y) and the device's caps probe (`0x1000b550`, the alpha-to-coverage check engine.md describes) | replaced by design (SDL window and GPU device) |
| `0x10095810`..`0x10096277`, `0x1008e400` | 2.6 KB, 0.8 KB | the overgrowth tiles' D3D9 vertex streams (`Vtx_Position` / `Vtx_Normal` / `Vtx_TBase`, vtables `0x101d36d4` / `0x101d3690`) and their LeafOG / TrunkOG technique handles | replaced by design (OvergrowthLod.h draws the `_lod` meshes as meshes) |
| `0x100793e1`..`0x10079e04` | 2.6 KB | `nametags.get/setCullDistance` (TagType: TTPlayer 100/300, TTOrder, TTRequest 100/1200, TTObject 0/1200, table `0x101f67a8`), `maxCallDistance`, `vehicleNametagOffset(Factor)`, `maxFriendlyDistance`, `deathFadeOutTime`, `manDownFadeOutTime`, `deployableDistance` | (d), low: the rows aren't bound; no shipped script calls them, and their defaults are Nametags.h's |
| `0x100aa290`..`0x100aa70b` | 1.1 KB | the light manager's `SimpleShadowmap.raw` / `SimpleShadowmapIO.raw` (every level's server.zip): a sun-occlusion map in 2-bit steps and an inside/outside bit, 2 m a texel about the origin (slots 153 / 154, `0x100a8cb0` / `0x100a9160`, bilinear) | done since (below) |
| `0x1014d450` (0.7 KB) | | TerrainOccluderShadows: the terrain patches drawn into the occluders' map by `0x100b0a40` | dead code in RendDX9 (below) |

The two rendering items, settled:
- **SimpleShadowmap: done** (`src/Render/SimpleShadowmap.h`, `LevelRenderer::loadSimpleShadowmap` / `simpleShadowFor`, mesh.frag). The maps are the lightmapper's: they are loaded at the level's load, not generated. They hold the terrain's and the static meshes' shade, so this is how RendDX9 has hills shade vehicles. Their layout is in the header: N half the cluster's heightmapSize, the file's first row at +z, a grid with a lit outdoors border, `0x100a8f40` / `0x100a84c0` / `0x100a9080`.
  - The bundled meshes' draw (`0x100c3880`, `0x100c5030`) scales the sun's colour and specular by pow(f, k), k the light manager's `+0xcc` (1), when the mesh isn't drawn with the shadowed technique (flag 0x1000: the shadow quality, video setting 4, over 2, and the mesh in this frame's dynamic map, 0x80).
  - The skinned meshes' draw (`0x10162070`) always scales; with `renderer.useSunScaling` (device `+0x41`, slot 0xbc) it takes the mesh as indoors.
  - With nothing above (the third ray, 150 m up, `+0xc8`): outdoors, f is the map faded to 1 by the height over the ground across `+0xd8` / `+0xdc` (3..6 m, a ray 6 m down); indoors, f is 1. With something above: indoors, f is the map; outdoors, 1.
  - The rewrite has no per-mesh collision rays (`0x100c2b70` / `0x10160e90`). The height comes from the terrain, as HeightOverTerrain's does. For the upward ray the IO map's bit stands in: a roof indoors, open sky outdoors.
  - The rewrite also draws every vehicle and soldier with its two shadow maps. Where both apply, the shader takes the darker of the factor and the maps.
- **TerrainOccluderShadows: dead code.** `0x1014d450`'s only caller, `0x100b0a40` (technique "TerrainOccludershadow", vpLightMat, vpLightTrapezMat), is unreachable: no vtable holds it, and no call, jump or absolute pointer in the image reaches it. RendDX9 never makes the terrain a shadow caster. It has the SimpleShadowmap above instead, so the rewrite adds no terrain shadow pass.

The three largest present-but-uncited runs are the particle manager's other system kinds: `0x10143980` (17.5 KB), `0x10135bb0` (9.6 KB) and `0x10141350` (9.4 KB). Slot 0x1c `0x1009a120` makes a template by kind; `0x10099970` makes the instance from the template's type id. The kinds are:

| Kind | System | Template vtable | Instance vtable |
|---|---|---|---|
| 1 | MeshParticleSystem | `0x101d8800` | `0x101d9348` |
| 2 | TrailSystem | `0x101d95a8` | |
| 3 | ParticleSystemEmitter | `0x101d9420` | `0x101d9268` |
| 4 | NonScreenAlignedParticleSystem | `0x101d89f8` | |

The mesh system's code and the templates' code are cited in Effects.h. Decoding them added the mesh particles' randomRotationSpeed, their rotation graph, one shared random per axis set, the 1/60 s pre-warm, and no isStaticSystem for meshes.

| Run | Size | What it is | Class |
|---|---|---|---|
| `0x10143980`..`0x10148430` | 17.5 KB | the mesh system's instance (start `0x10144ab0`, update `0x10145bf0`, emission `0x10144d90`, one particle `0x10145660`, motion `0x10144110`, placements `0x10145fc0` / `0x101460f0` / `0x101462e0`), the trail and ParticleSystemEmitter templates, the mesh geometry's load steps `0x101482f0` / `0x101483f0` / `0x10148370` | (c) cited (Effects.h, MeshFile.cpp); the plane ring `0x10144cf0` is not done (the terrain instead) |
| `0x10135bb0`..`0x10138740` | 9.6 KB | the mesh and non-screen-aligned templates: constructors `0x10136720` / `0x10137fe0`, defaults `0x10135f30` / `0x10137290`, compile `0x10135e20` / `0x10137eb0`, LOD `0x10136570` / `0x10137a90`, texture `0x10137c60`, property accessors | (c) cited |
| `0x10141350`..`0x10143940` | 9.4 KB | the ParticleSystemEmitter instance (constructor `0x10142010`, update `0x10143540`, emission `0x101426f0`, motion `0x10141720`): particles that each carry a child system | (d) unused: only levels without an archive.md5 (BF2 leftovers) create one |

## (d) Genuinely missing, ranked by player visibility

Evidence is the strings in the uncited code, the call counts in coverage-2's script corpus, and absence from src.

| # | Feature | Player sees | Evidence | Effort |
|---|---|---|---|---|
| 1 | **Particle and debris emitter detail** | Explosion debris and particle shapes differ: no emitter area, debris doesn't spin or scatter. | Particle systems' `emitRadius` is unread (1,472 calls; only `rainManager.emitRadius` is read). `Emitter.positionalSpeedIn{Dof,Up,Right}`, `rotationalSpeedIn*`, `startRotation`, `zRotation`/`zRotationSnap` (130–215 each) are unread. `Particle.Transparency`/`sizeModifier`/`usedOnTransparentSurface` (224), MeshParticleSystem `writeZ`/`directionalScale` (215) and `isStaticSystem` (165) are unread. Setters at EXE 0x0052e9bf–0x0052fcae and 0x00530d0e–0x00539515. Coverage-2 #12 covers only part of this. **Status: done** (engine.md "Debris, emitter types, level ambience and template behaviours"): emitterType Standard/RadialDirection/Radial/Box with `emitRadius` (RendDX9 `+0x18`, `0x10140fa0` / `0x101410d0` / `0x101412c0`), `isStaticSystem` (`+0x26d`, `0x101407c0`), the debris Emitter (`0x0061f360`, `0x0061ef80`: relative positions, positional and rotational speed spreads, startRotation / zRotation / zRotationSnap, alternates, noPhysics) and its Particle pieces (`0x00621250` / `0x00621480`: life, size x sizeModifier, transparencyGraph x (1 - Transparency), gravityModifier, drag), drawn by `LevelRenderer::syncDebris`. The pieces' own physics nodes are done too: each piece is a rigid body whose collision-mesh box corners meet the terrain and the static objects, and it sleeps when still (Effects.h DebrisPiece). Left: `usedOnTransparentSurface`, MeshParticleSystem `writeZ` / `directionalScale`, the emitter's view flags. | S–M |
| 2 | **AmbientEffectArea and Trigger objects** | Area-triggered level ambience (effects or sounds firing near players) never happens. | `triggerId` 485, `linkedEffectContainer` 290, `projectOnHeightmap` 232, `events` 195, `minimumTimeBetweenTriggering` 195, `chance` 193, `averageTimeBetweenTests` 193 are all unread. Setters at 0x005016fc–0x005026ff. In src the class is only an id in ObjectTemplateManager.cpp:90. Not in coverage-2. **Status: done** (`src/Game/AmbientTriggers`, `Ambience.cpp`): the trigger manager `0x00514fc0`, the tests `0x005d3890` / `0x005d3600` / `0x005d3540`, events 1/2/4/8, the areas `0x005ba9c0` / `0x005ba490`. The network event for `numNetworkedEffects` is done too (`0x00510270` built and `0x00510160` written on the server, `0x005100a0` read and `0x00510230` played on the client; the area's id comes from `DAT_00a6bb08`). `projectOnHeightmap` now defaults to on (`0x005b9340`). | S–M |
| 3 | **Commander screen remainder** | The commander's squad panel and its icons, asset feedback, application accept/reject, UAV-offline and squad-info states, and world order markers. | The squad panel uses `Ingame/Commander/squad_bg*`, `voip_icon_*` and `icon_kit_*` (0x007ab790–0x007ad252, 0x007ac090) and `SELECTED_ALL_SQUADS` (0x007ac8f0). Asset feedback: `artillery_deny/accept/unavailable` (0x007c7e20) and `mapAction`, `orbitalstrike_in_use`, `satellite_not_ready` (0x007d7c20). Also `ShowCommanderAcceptReject` (0x00753b40), `uav_offline` (0x00788600), `HUD_HUD_SQUADINFO_*` (0x007a0f80), and the order markers `ordermarker_move/attack/defend` and `us_beacon` (0x005b8b20, 0x004d2650). About 40 KB, and none of these strings is in src. Coverage-2 #23 is a subset of this. **Status: mostly done** (engine.md "Squad requests, asset feedback and the commander screen's panel"): the requests and their answers (`0x007708d4`, `0x007d6d20`, `0x004c5720`, `0x007c8f10`, `0x007c8a60`, `0x007c7a00`, `0x007c7e20`, `0x007d8d30`; ShowCommanderAcceptReject `0x00753b40`), the tools' refusals and `mapAction` (`0x007d73a0`, `0x007d74e0`, `0x007d7640`, `0x007d88d0`, `0x007d7d80`, `0x007d8370`, `0x007d81d0`, `0x007d8660`, `0x007d7c20`), `uav_offline` (`0x00788600`), the squad panel's order and kit pictures (`0x007ab790`, `0x007ac8f0`, `0x007aa0f0`), the squad info bar (`0x007a0f80`), the world order markers (`0x005b8b20`). Left: the squad panel's `voip_icon_*` and `SELECTED_ALL_SQUADS`, the request's network events (Request `0x0050e170`, PostRemotes 2/0x1d, 2/0x1e), `0x004d2650` (`us_beacon`; not looked at). | M |
| 4 | **Kick and map vote HUD texts** | The vote runs (src/Game/Votes.h), but none of the original's progress, result or refusal lines show. | One 5.3 KB function, 0x005725e0, builds `HUD_HUD_KICKVOTE_*` (IN_PROGRESS, PLAYER_STAYS, COMMANDER_KICKED, MAP_CHANGE, CANNOT_KICK_HOST…) with `#YESVOTES#`/`#NEEDED#`/`#MINUTES#`. "KICKVOTE" appears nowhere in src or engine.md. **Status: done** (`menu::VoteMessages`; engine.md "Vote lines, radio text, text styles and the map keys"): `0x005725e0`'s lines from the client's vote object (`0x005740e0`, its tick `0x00573ae0`, the start checks `0x00573cb0` / `0x00573df0` / `0x00573f20`) on a joined client. Left: the host's own player, the kick vote's start from the scoreboard. | S |
| 5 | **Lock-on and guided-missile feedback** | No "target acquired / lost / missile away" cues. | `target_aquired`, `target_lost` (0x005e1b30) and `missile_away` (0x005e1ee0) appear nowhere in src. **Status: done** (game side, `GameWorld::takeLockEvents`, `LockFeedback.cpp`): `0x005e1b30` (target_aquired 35 ticks after the target, target_lost; radio for TSLaserGuided only) and `0x005e1ee0` (missile_away on TCPlayer). HUD: **done**: the exe's HUD shows these only as the radio's text (`0x0042d7f0` → `0x0042bef0`, the sample's text in the chat), now drawn (`GameAudio::onRadioText`); BF2142.exe has no other lock-on text. | S |
| 6 | **Remaining template behaviours** | Assorted per-weapon and per-vehicle behaviours. | 640 ObjectTemplate rows (47k calls) remain unread under the literal rule. Excluding the editor flags (`isNotSaveable` 16,862, `saveInSeparateFile`, `createdInEditor`) and items already listed in coverage-2, the notable ones are: `replaceItem` 196, `keepProjectiles` 210, `ammo.reloadWithoutPlayer` 201, `allowEmoteAnimation` 158, GenericProjectile `hasOnTimeEffect` 147, ControlPoint `isHemisphere` 148, `castsDynamicShadow` 258, `cullRadiusScale` 239. About 111 KB of setter code; the consuming logic is spread around. **Status: partly done:** `hasOnTimeEffect` (`0x005b62a0`), `ammo.reloadWithoutPlayer` (`0x005f4bb0` / `0x005f3a40`); settled without effect: `isHemisphere` (scripts only, `0x0056a010`), `castsDynamicShadow` (no shader). Then done: `keepProjectiles` (`0x005f1cc0`, default 5 at `0x005c2800`: a leaving player's rounds, mines, hubs and deployables are removed that many seconds later) and `replaceItem` (`0x004e0758` -> `0x005bdd80`: an equipped unlock's item replaces the kit items it names). Settled without effect on play: `cullRadiusScale` (template `+0x58`, `0x00504987` / `0x00504994`; the renderer's view culling). Left: `allowEmoteAnimation` (`0x005edbf0`; no emotes in the rewrite). | S each |
| 7 | **Soldier hit-direction triggers** | Hit reactions or indicators by direction. | 0x005eda90 maps a direction to `hitFront{Left,Right,Body}`/`hitBack*`; no "hitBack" in src. Not yet traced whether this is an animation or the HUD. **Status: done:** an animation (`GameWorld::hitReaction`, `0x005eda90` from `0x005e7520`): the 3P RandomTriggers `hitFront*/hitBack*` by the hit bone's node. | S |
| 8 | **Pure-server enforcement** | A pure server never kicks clients with modified archives. | 0x004aec40 (`for using modified data`, `dep: kicking client`). The archive.md5 load is present (it is in (c)). **Status: done** (`src/Net/ContentCheck`, engine.md "The pure-server content check"). The server is pure when std_archive.md5 loads (`0x004aff80` -> `0x004adb60`). Each level draws an ordinal (`0x004aefe0`). The client fingerprints its content for that ordinal (`0x0068ba40`) and answers with ContentCheck (`0x005123f0`, vtable `0x0091d0c8`; its digests come from `0x005a9ea0` / `0x0087b560` / `0x0058cc60`). The server compares in game server slot 51 (`0x004aec40`) and kicks with 0x1b, including a client that sent no check (`0x004af710`). Loopback-tested, and the rewrite's fingerprint reproduces the shipped .md5 files. | S |
| 9 | **Object caps** | Debris and destroyable counts aren't limited, which shows as a performance difference. | `GSDestroyableObjectsLimit`, `GSDestroyableWindowsBundlesLimit`, `GSDynamicObjectsLimit` (0x004d2250) **Status: settled:** `0x004d2250` reads the three limits from the SettingsRepository and drops them (`0x00659a40`'s result unused); nothing limits by them. The function's real work, turning destroyables into ObjectSpawners (class `0x94a1`, 10 s), is noted in engine.md. It is not done because it would change nothing in play: the rewrite destroys none of the six shipped DestroyableObject templates. | S |
| 10 | **Language list and non-English fonts and locales** | Only English is selectable. | 0x0049fda0 (Chinese_Traditional, zh_CN, code page 125170…), RendDX9 0x1017af30 | S–M |
| 11 | **AI console commands for scripted bots** | Co-op and SP scripts that steer bots directly. | AIDLL 0x10004da0–0x100593f0 (59 KB): `botMoveTo`, `botFlyTo`, `botGiveOrder`, `botSetWaypoints_Patrol*`, `addPlacement`, `createLandingZone`, `forceStrategy`, `setTakeable`, `addUnspawnedBot`… src binds only `aiSettings` / `aiStrategy` / `aiTemplate` / `aiStrategicArea` / `weaponTemplate` rows (Bots.cpp). The shipped scripts barely call the rest, so this is low priority unless mods rely on it. **Status:** mostly done (src/Game/BotsScript.cpp; every command classified in engine.md "AIDLL's console and scripted bots"). | M |
| 12 | **Online back end** (already in coverage-2 §2b) | BFHQ stats, leaderboards, award grants, rank changes, booster entitlements. | Player-info keys `mtt-%d-%d`… (0x004818f0, 8.8 KB), `getleaderboard.aspx` (0x0045d7e0), `rankchange/grantaward/playerdisconnect.aspx` (0x0063da50), `REG-PC-BF2142-UNLOCK-%d` / `online_content_cqc` (0x004a1d70) | L; needs services |
| 13 | **Logitech G15 LCD applet** (game side) | Hardware only. | 0x0070cc50–0x0070fa50 (server, map, ping, K/D and dog tags on the LCD) | S, low value |

Also seen; most of these are names the rewrite spells differently, or debug-only:
- the network event name table (0x0042ec60);
- the `ECHudHE*` HUD event names (0x0074d370);
- the IDKey/IDButton name tables (0x00683e40–0x00686f56; the rewrite parses these names, so mostly (c));
- the threads and core events table (0x0065c6dc);
- the culler and leak bookkeeping (0x00703220, 0x00702150: shutdown asserts, effectively (b)).

## The missing-runs pass (2026-11-05)

`uncited.py --cite cite_noaudit.json --min-run 1024` at 8575796 listed 90 EXE (d) runs of 1 KB or more (189 KB). Those this pass settled, with the code they cite or the reason they are not needed (coverage-6.md's last status block has the full table with the implemented and cited ones):

| Run | Size | Class | Reason |
|---|---|---|---|
| EXE `0x006e8570`..`0x006ebdbc`, `0x006af640`..`0x006afead` | 14.2 KB | replaced by design | The object grid's tree build ("Recursing again…", `Object.initGrid`) and its triangle-box test (`0x006af710`, from `0x006ea1d0`). The rewrite culls and queries with its own structures. |
| EXE `0x00683e40`..`0x00686e89` | 10.2 KB | replaced by design | The IDKey_ / IDButton_ name tables; ControlMap.cpp and GameControllers.cpp compose the same names. |
| EXE `0x0047f5e0`..`0x00483b50`, `0x00483e30` (6.4 KB, a false (c)) | 19.8 KB | online-only | The BFHQ stats and leaderboard pages' fields (`acdt`, `wdths-%d`, `rank` / `pid` / `nick` / `globalscore`). |
| EXE `0x0049eef0`, `0x0049e8b0`, `0x00479970`, `0x0046b900`, `0x00574550` | 6.1 KB | online-only | The server logo and HTTP caches, the account errors, the account name rules, the content download's messages. |
| EXE `0x0070cb00`..`0x0070e120`, `0x00436530`..`0x00436dd8` | 7.8 KB | hardware (third-party SDK) | The Logitech G15 LCD applet: its pages (`0x0070cc50`, the "1/3" page writer `0x0070d880` through the LCD SDK `0x0071fc50`) and its start-up (`0x00436890`: the page object and the applet's icons). |
| EXE `0x0066b175`..`0x0066cee1` | 6.7 KB | dev | `Console.getArgumentTypes` / `getEnumName` / `getRange`… reflection; nothing ships a call (`Console.allowMultipleFileLoad`, 26 calls, is in the same run and is a load flag the rewrite doesn't need). |
| EXE `0x006ee282`..`0x006ef9bd`, `0x006ec29f`..`0x006ecf7a`, `0x006efbdb`..`0x006f0cb9`, `0x006f0d36`..`0x006f1b6f`, `0x006f3d18`..`0x006f4249`, `0x006f4558`..`0x006f4976`, `0x00521440`..`0x005218df` | 20.0 KB | editor | `Object.*` (isVisible, direction, the light's color / fov, togglePause, active, list), `Player.*` (create, list, saveAll), `ObjectTemplate.*` (delete, list, save, getPosition, readBinary / writeBinary, and the save flags isNotSaveable, saveInSeparateFile, createdInEditor). No shipped script calls the methods; the flags only steer the editor's save. |
| EXE `0x004196a4`..`0x0041aa21` | 5.0 KB | dev | SquadManager.listSquads / listSquadMembers, renderer.listScreenModes / screenMode, pythonHost.sendCommand / reinitialize: console listings, no shipped caller. |
| EXE `0x006b3db1`..`0x006b49c8`, `0x006b2fc3`..`0x006b3534`, `0x007636c8`..`0x00763cbd`, `0x008b0792`..`0x008b0d51`, `0x008afd9a`..`0x008b00be` | 7.0 KB | editor / dev | collisionManager's draw and focus rows, heightmapcluster's import / smooth / stitch / simulateWater, HudBuilder's slider node, AnimationSystem's trigger and bundle browser (create / active / list); no shipped caller. |
| EXE `0x004f64fb`..`0x004f6efe` | 1.6 KB | unset | DestroyableObject's and DestroyableWindowsBundle's respawnDelay, usedByAi, initiallyWalkable, min/maxCorner, effectTemplateBaseName, clientHitDetection: no shipped template sets them. |
| EXE `0x00525958`, `0x004fd1c3`, `0x00544efd`, `0x004fc895`, `0x005234b3`, `0x004ed6a5`, `0x00520880`, `0x0055360d`, `0x0052796e`, `0x004e4667`, `0x0055264c`, `0x00552d4b`, `0x004e39f3`, `0x004fdcd6` | 20.6 KB | unset | Stored template rows no shipped script sets, so their constructor default is what plays: the Camera's cameraSway*, the PCO's regulateYaw / Pitch / Roll(Input), maxVertRegAngle / noVertRegAngle (VehicleParts.cpp uses the defaults), soldierExitPosition / Rotation / Speed, the Rotor's changeLodAt and engine rows, RotationalBundle's regulateFromParent*, useShiftAnimation / shiftDelay, isPortalPassing, vehicleHud.addVehicleIcon / addAbilityIcon, insideViewInterpolate and the colour transform, fire.isBarrel / useDummyProjectiles, podSpawnVehicle / spawnInPod, postProcess.tvInterference / tvDistortion*, fire.drop / hideWeaponAfterReloadDelay. Left in these runs: cameraShakeSensitivity (8 calls), regulateRoll (2), toggleWeapon (4), animation.loopingFire (4), fire.fireLaunchDelaySoft (11), shakeFactor (2). |
| EXE `0x0051ca27`..`0x0051cfaf` | 1.4 KB | editor | anchor / anchorOffset (1,110 calls in the road and static templates): the editor's snap point (SimpleObject slot 0x98). isOvergrowth (13) and relativePositionOffset (4) are in the run too. |
| EXE `0x0070b600`..`0x0070bd31` | 1.8 KB | editor | The effects editor's `Objects/Effects/…/Effects.con` writer ("Untitled"). |
| EXE `0x0087bc00`, `0x0087a1c0`, `0x007b2f60`, `0x007b3750`, `0x00874cd0`, `0x007b2220`, `0x007dbd10`, `0x00870020` | 12.1 KB | replaced by design | dice::meme nodes, actions and effects (EditNode, ButtonNode, OccupiedNode, IfElseEventActionNode, %sMoveEffect, BfNewListBoxNode, MovePointer): the HUD substrate src/Hud replaces (see (c), "Not needed one-to-one"). |

Left (d) by that pass, done by the next (coverage-6.md, "Status (EXE missing-runs pass, second)"): the HUD ability bar `0x007c3af0`, `detonation.allowFriendlyFireDetonation` with `sv.friendlyFireWithMines` (`0x005e8aa0`), the motion mine bait (`0x00500594`'s MagneticBait rows), the host page's settings (`0x00499430`).

### The second pass (2026-11-05)

| Run | Size | Class | Reason |
|---|---|---|---|
| EXE `0x00475cd0`..`0x00476483` | 1.9 KB | online-only / (a) | SwiffHost (vtable `0x00924d50`) slot 241 `0x00475cd0` (the page listener: kept in `+0xe68`, told `+0x770` and the page `+0xe50`; MenuBindings' pages replace it), slot 247 `0x00475d60` (the menu's news line: the next of the fetched strings `+0xcc`, else a fetch through the HTTP object `+0x80` / `+0xbc`, every 300 calls), slot 64 `0x00475ec0` (event 0x17 to itself); the rest vector assignments (`0x00475f10`, `0x00476050`, ...). |
| EXE `0x0047a8e0`..`0x0047b0b9` | 1.8 KB | (a) | SwiffHost's data copies: `vector<0x38-byte record>::operator=` (`0x0047a8e0`), the end-of-round block's constructor `0x0047ac90` / destructor `0x0047ae10` (SwiffHost `+0x7b8`) and copy `0x0047af00`, their element loops and catch handlers. EndOfRoundBindings keeps its own. |
| EXE `0x007a6e30` | 1.1 KB | implemented | The invite list's player row (from `0x007a7450` for `0x007a7ff0`); with the header rows (`0x007a4680`) in `SquadMenu::update`. |
| EXE `0x00567cc0`..`0x00568344` | 1.6 KB | cited | The host module's sgl_endGame / sendGameLogicEvent / sendPythonEvent / sendMedalEvent / sendRankEvent / sendHudEvent / sendTextMessage (ScriptHost.cpp). |
| EXE `0x007a7620`..`0x007a7ab5` | 1.2 KB | cited | SquadMenu's constructor (HudMenus.h; its icons are the scripts'). |
| EXE `0x00453bf0`..`0x00454c98` | 2.0 KB | online-only | The server's GameSpy session key ("%04x%ld%04x%04x%04x%04x": sv.gameSpyPort, the time, four randoms, `0x00453ca0`) and its reporting neighbours. |
| EXE `0x005fa890`..`0x005fb1b1` | 1.8 KB | replaced by design | GSWindowSizeX / GSWindowSizeY, the window's size in the settings (`0x005fa8e0`): the rewrite's window is main.cpp's. |
| EXE `0x00571220`..`0x005718c9` | 1.7 KB | not settled | A kit-side update (its neighbours are the Kit's `0x0056cc50`, `0x0056e5c0`) that looks up a "recycle bin" object; needs a read. |
| EXE `0x00477e60`..`0x0047849e` | 1.5 KB | not needed | The menu's mod switch: CommandLine.cmd written for the restart ("no mod specified", `0x00477e60`); the rewrite runs one mod. |
| EXE `0x00577870`..`0x005782c8` | 1.4 KB | replaced by design | The language table (`0x00577f60`: English, French, German, ... Polish, Chinese, Czech, by name into DAT_00a67f74): the rewrite's localisation reads the mod's own language folders. |
| EXE `0x008e87a0`..`0x008e9005` | 1.4 KB | (a) | Static registration of the networkable classes' names (SoldierNetworkable, SimpleObjectNetworkable, ...: DAT_00a73384); net::networkableInfos keys them. |
| EXE `0x007e8a90`..`0x007e8fd8` | 1.3 KB | not settled | A map node's subclass constructor (over `0x00784cf0`, vtable `0x009b29d0`: map_Line, micromap_zoomSquare): the commander's micro map's; needs a read against HudMenus' map. |
| EXE `0x005172f0`..`0x005177a3` | 1.2 KB | gap | The demo reader's camera files (Settings/demo-recordcam.con, demo-playcam.con); DemoCamera keeps its defaults. |
| EXE `0x006694eb`..`0x00669969` | 1.1 KB | dev | The console's object-function test ("ERROR: Objectname or/and functionname are invalid"). |
| EXE `0x0040e5fc`..`0x0040eaac` | 1.1 KB | (a) | Vector growth and a delta-compression helper's "No Delta" assert. |
| EXE `0x0051aaa9`..`0x0051b215` | 1.1 KB | not settled | The wheels' grip flags' names (PGFRollGrip, PGFEngineGrip, PGFDummyGrip, PGFStaticFriction...): their reader. |
| EXE `0x0088d1a0`..`0x0088d607` | 1.1 KB | not settled | A "_wreck" template child made for a destroyable (flag `+9 & 0x10`). |
| EXE `0x0042737f`..`0x004278ef` | 1.4 KB | dev | GameLogic.remoteCommand / createObject / spawnAtCameraPosition (1 call between them); setBeforeSpawnCamera is implemented. |

Stored template rows still unread, with their shipped calls (runs of 1 KB or more; each a (d) gap):
- `0x0052930c` (2.1 KB): castsStaticShadow, castsDynamicShadow (252), selfLights, selfShadows, supportsPerVertex / PerPixel / Glow / Emitter (94 each): the light sources' flags (rendering).
- `0x0055c95e` (2.0 KB): strategicObject.* (strategicObjectName 7, destroyedIcon 7, intactIcon 8, part, 3dMapIcon, isOnePiece), nametag.3dMapIcon (102).
- `0x0052a79a` (1.8 KB): the projector lights' projectorTexture / Fov / Aspect / Near / Far, enabled (93) (rendering).
- `0x004dfcee` (1.6 KB): antenna.* (2 each; team is read elsewhere).
- `0x004e52b2` (1.5 KB): fire.hasAlternateOrigin (2), spawn.spawnVehicle / spawnInPod (1), fire.detonatorObject (4), fire.showWeaponAfterReloadDelay (18).
- `0x00546302` (1.2 KB): the walkers' foot spacing / footLength / testCount (2).
- `0x0051ffae` (1.2 KB): rotateAsAnimatedUV* (1 each), bundleSkeleton (20).
- `0x004f9047` (1.2 KB): kitCustomPic (8), dogTagRewardTemplate / dogTagExchangeTemplate (32), disableBreathEffect (8).
- `0x00550a65` (1.1 KB): radio.destroyedGameEvent (6), indicationDistance2D (13), closeToIndicationDist (16), closeToIndicationType (13), friendlySpawnMessage (1).
- `0x004e1f2f` (1.1 KB): allowZoomOnHeat (3); setIfTitanChangeToWeapon, setCheckObstruction stubs here.
- `0x005264aa` (1.0 KB): bounceLength / Strength / Damping (3).
- `0x004e4d83` (1.0 KB): fire.projectileStartRotation (24), randomRotationSpeed (6), isPodLauncher (13).

## AIDLL: the missing-runs pass (2026-11-05)

AIDLL's uncited (c) / (d) / "?" runs of 512 B or more (coverage-6.md, "Status (AIDLL missing-runs pass)", has every run of 1 KB or more with its outcome). What the rewrite does was cited in src; the rest is classified here, one line each.

| Kind | Where (AIDLL) | Why the rewrite doesn't need it |
|---|---|---|
| (b) Bot, team and path debug printers and the debug drawer | `0x1002d800`..`0x10030b1b` ("** NO PLAN **", "Incoming fire (count):", "Team1:", the drawer's vectors and IDPManager release `0x10030140`), `0x1002d1c0`, `0x1002d4f0`, `0x1002b930`, `0x10029ef0`, `0x1003c330`, `0x1003c970`, `0x1003ee10`..`0x1003f2c2` (trace-map printers), `0x100350e0`, `0x10055ca0`..`0x10055ed0`, `0x10058570`..`0x10058860`, `0x100cd9b0`, `0x100ce0e0`, `0x100dac90`, `0x100b0d00`, `0x10100760`..`0x10100d68` (PI* names), `0x100fe500` (assert reporter) | Only the debug passes (AIMain `0x10057430` / `0x10056c30`) and the console's draw commands call them. |
| (b) IDPManager's text demo | `0x10053620`..`0x10054690` ("Static 2D/3D Text Demo") | Debug. |
| (b) Navmesh generation (GTS) | `0x100bac90`..`0x100bbfef` (the `.qti` reader and the build under `0x100bc000`), `0x100b0a00` / `0x100b0c60` / `0x100b1280` (`.cls`), `0x100ac300` (saveQuadTrees), `0x100ac550`, `0x100b69b0` (cluster link distances, which the shipped `.clb` carries), `0x100b2750`, `0x100b3cc0`, `0x100b33a0`, `0x100b9a50`, `0x100f3e10`, `0x100f4350`, `0x100f15e0`, `0x10046080` / `0x100463f0` (exportGTSMeshes), `0x10031a70` / `0x10031ba0` / `0x10031820` (createHeightMap) | Offline tooling; the rewrite loads the shipped `.qtr` / `.clb` / `.ahm`. |
| (b) The navmesh generator's settings | `aiPathfinding.map.minPolygonsInLeaf` / `maxDepthInTree` (`0x10020d30`..`0x10021050`), `map.draw*` / `undraw*` (`0x10021290`..`0x10021730`) | Generator settings and debug. |
| (a) STL, containers, RTTI and plumbing | vector `_Insert_n` (`0x10040800`, `0x1004cee0`, `0x10050210`, `0x10088ba0`, `0x1009d940`, `0x100accd0`, `0x100b8af0`, `0x100c2cb0`, `0x100c7690`, `0x100f1aa0`, `0x100f49c0`, `0x100f7a70`, `0x1003fe30`, `0x100323c0`, `0x10036ea0`), the debug drawer's and comm object's containers, the interfaces' constructors (IAIMineManager, IAIAutoControllersManager, IAIPlayerCourier, IAIEventCourier, IAICollisionHandler, IAISettingsModifier, IDP*), the behaviour-pattern factories (`0x10024190`..`0x10024d70`, `0x10026300`), AISingletonServerUpdater (`0x10001740`, `0x100019d0`), the bot's and areas' accessor slots (`0x10096ab0`..`0x10097364`, `0x1005cfe0`..`0x1005d837`), AISettings' modifier storage (`0x10051d20`, `0x100511a0`, `0x10050500`), the plug-in part wrappers (`0x100ab080`..`0x100abb8b`, `0x100aa920`), the mine grids (`0x100499e0`, `0x10049650`, `0x100480a0`, `0x1004a4d0`) | The rewrite's own containers and BotSettings / AiPlugin stand for them. |
| (a) Dead code | `0x100cbd70` (a 101-entry x^6 table no code reads), `0x100fad20`..`0x100fbac2` (a root solver behind aim slot 0x44, which nothing calls), the IAIStrategicEnvironment and pathfinding courier slots the exe never calls (`0x10054bd0`..`0x10054fc0`) | Nothing reaches them. |
| Inert: no shipped content reaches it | Planes (BBPFire3d `0x100819b0`, the Plane interpreters `0x1008fec0`..`0x100908e0`, controllers `0x100e77a0`, the ITAir room test's frustum `0x100ff690`, the exit spot `0x1006d840`); AirTransportMoveTo (`0x100e0e60`); boats (GotoWaypointsBoat `0x10067e00`, the boat look); AvoidCollision3d (`0x10065dd0`); BBCallForMedic (`0x10081f70`..`0x100824e0`, in no set); broken bridges (`0x10035440`..`0x10035935`, the obstacles `0x100b8e90`, `0x100b9e40`, `0x100bb130`); the landing zones and placements (`0x10032790`, `0x1001f320`, `0x1001f750`, `0x1000a690`, `0x10032e10`); the homogenous / heterogeneous conditions (`0x100a9660`, `0x100a96f0`) | No Plane, AirTransport or Boat equipment, no broken bridge and no script using these ships; the rewrite would gain nothing observable. |
| Left as gaps (bot behaviour) | the squads' formation and order radio, the sensing look-around, the SAI's respawn choice, the loners by aggression, mine avoidance on the mesh, and the smaller ones listed in coverage-6.md | Decoded there; not yet written. |

## Caveats

- The (c)/(d) split rests on string matching. Composed strings (for example `"SettingsManager." + type + "Set"`) make false (d)s. Generic words make false (c)s. I spot-checked about 50 functions.
- The console-row states are coverage-2's (991d041), upgraded where the literal now exists in src.
- Propagated labels carry the calibrated error rates above.
- Function sizes are Ghidra's. Data tables and switch tables are not counted.
