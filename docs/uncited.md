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
| GameSpy SDK (GP, GT2, HTTP, Voice 2), zlib 1.2.2 inflate, Logitech LCD SDK, HMAC/SHA-1, Speex codec | EXE 0x0070fa90–0x00745ea8 | 208 KB | Includes zlib at 0x0071bac0 and 0x00733b60, the LCD SDK at 0x00736020 and 0x00739d90, HMAC at 0x00738670, and Speex from ≈0x0073c890 to 0x00745ea7 (`warning: %s`, 0x0073e0d0). Coverage-2 called 0x007380d0–0x00745ea7 an "https library"; it is HMAC plus Speex. Speex and zlib are linked as libraries in the rewrite; Voice 2 is cited (src/Voip). |
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

## (c) Implemented in the rewrite but uncited (citations could be added)

- EXE: 757 KB evidenced plus 1.27 MB propagated (≈84% reliable).
- RendDX9: 148 KB plus 623 KB.
- AIDLL: 40 KB plus 408 KB.

### Largest evidenced runs, with the best-guess src file

| Range | Size | Contents | Likely src |
|---|---|---|---|
| EXE 0x007cb760–0x007d7047 | 44 KB | awards / medals pop-up (`HUD_HUD_AWARDED`, `AWARD_Name_%i`) | Main/MenuBindings.cpp, Main/HudAlerts.cpp |
| EXE 0x0040ec54–0x004193c7 | 40 KB | SwiffHost / GeneralSettings / ProfileManager console objects | UI/Player/NativeTable.cpp, Main/SettingsObjects.cpp, Main/MenuBindings.cpp |
| EXE 0x00784cc0–0x0079a035 | 39 KB | minimap, spawn and kit screens | Main/main.cpp, Main/HudMenusKits.cpp |
| EXE 0x00506210–0x005144df | 31 KB | admin, ban lists, UB_* | Main/ServerAdmin.cpp |
| EXE 0x0041aa74–0x00422035 | 28 KB | GameServerSettings console | Net/ServerSettings.cpp |
| EXE 0x004b9220–0x004c4909 | 26 KB | spawning / kill player | Game/GameLogic*, Game/GameWorld.cpp |
| EXE 0x006a4760–0x006aa7ce, 0x0068e8d0–0x00693365, 0x0066cf6d | ≈40 KB | GI*/PI*/IDKey enum tables, control maps | Input/ControlMap.cpp |
| EXE 0x0075c6b0–0x00761c05 and HudBuilder (0x0075f91a–0x00773b23) | | | Hud/Hud.cpp (62 KB of evidenced functions in total) |
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

## (d) Genuinely missing, ranked by player visibility

Evidence is the strings in the uncited code, the call counts in coverage-2's script corpus, and absence from src.

| # | Feature | Player sees | Evidence | Effort |
|---|---|---|---|---|
| 1 | **Particle and debris emitter detail** | Explosion debris and particle shapes differ: no emitter area, debris doesn't spin or scatter. | Particle systems' `emitRadius` is unread (1,472 calls; only `rainManager.emitRadius` is read). `Emitter.positionalSpeedIn{Dof,Up,Right}`, `rotationalSpeedIn*`, `startRotation`, `zRotation`/`zRotationSnap` (130–215 each) are unread. `Particle.Transparency`/`sizeModifier`/`usedOnTransparentSurface` (224), MeshParticleSystem `writeZ`/`directionalScale` (215) and `isStaticSystem` (165) are unread. Setters at EXE 0x0052e9bf–0x0052fcae and 0x00530d0e–0x00539515. Coverage-2 #12 covers only part of this. | S–M |
| 2 | **AmbientEffectArea and Trigger objects** | Area-triggered level ambience (effects or sounds firing near players) never happens. | `triggerId` 485, `linkedEffectContainer` 290, `projectOnHeightmap` 232, `events` 195, `minimumTimeBetweenTriggering` 195, `chance` 193, `averageTimeBetweenTests` 193 are all unread. Setters at 0x005016fc–0x005026ff. In src the class is only an id in ObjectTemplateManager.cpp:90. Not in coverage-2. | S–M |
| 3 | **Commander screen remainder** | The commander's squad panel and its icons, asset feedback, application accept/reject, UAV-offline and squad-info states, and world order markers. | The squad panel uses `Ingame/Commander/squad_bg*`, `voip_icon_*` and `icon_kit_*` (0x007ab790–0x007ad252, 0x007ac090) and `SELECTED_ALL_SQUADS` (0x007ac8f0). Asset feedback: `artillery_deny/accept/unavailable` (0x007c7e20) and `mapAction`, `orbitalstrike_in_use`, `satellite_not_ready` (0x007d7c20). Also `ShowCommanderAcceptReject` (0x00753b40), `uav_offline` (0x00788600), `HUD_HUD_SQUADINFO_*` (0x007a0f80), and the order markers `ordermarker_move/attack/defend` and `us_beacon` (0x005b8b20, 0x004d2650). About 40 KB, and none of these strings is in src. Coverage-2 #23 is a subset of this. | M |
| 4 | **Kick and map vote HUD texts** | The vote runs (src/Game/Votes.h), but none of the original's progress, result or refusal lines show. | One 5.3 KB function, 0x005725e0, builds `HUD_HUD_KICKVOTE_*` (IN_PROGRESS, PLAYER_STAYS, COMMANDER_KICKED, MAP_CHANGE, CANNOT_KICK_HOST…) with `#YESVOTES#`/`#NEEDED#`/`#MINUTES#`. "KICKVOTE" appears nowhere in src or engine.md. | S |
| 5 | **Lock-on and guided-missile feedback** | No "target acquired / lost / missile away" cues. | `target_aquired`, `target_lost` (0x005e1b30) and `missile_away` (0x005e1ee0) appear nowhere in src. | S |
| 6 | **Remaining template behaviours** | Assorted per-weapon and per-vehicle behaviours. | 640 ObjectTemplate rows (47k calls) remain unread under the literal rule. Excluding the editor flags (`isNotSaveable` 16,862, `saveInSeparateFile`, `createdInEditor`) and items already listed in coverage-2, the notable ones are: `replaceItem` 196, `keepProjectiles` 210, `ammo.reloadWithoutPlayer` 201, `allowEmoteAnimation` 158, GenericProjectile `hasOnTimeEffect` 147, ControlPoint `isHemisphere` 148, `castsDynamicShadow` 258, `cullRadiusScale` 239. About 111 KB of setter code; the consuming logic is spread around. | S each |
| 7 | **Soldier hit-direction triggers** | Hit reactions or indicators by direction. | 0x005eda90 maps a direction to `hitFront{Left,Right,Body}`/`hitBack*`; no "hitBack" in src. Not yet traced whether this is an animation or the HUD. | S |
| 8 | **Pure-server enforcement** | A pure server never kicks clients with modified archives. | 0x004aec40 (`for using modified data`, `dep: kicking client`). The archive.md5 load is present (it is in (c)). | S |
| 9 | **Object caps** | Debris and destroyable counts aren't limited, which shows as a performance difference. | `GSDestroyableObjectsLimit`, `GSDestroyableWindowsBundlesLimit`, `GSDynamicObjectsLimit` (0x004d2250) | S |
| 10 | **Language list and non-English fonts and locales** | Only English is selectable. | 0x0049fda0 (Chinese_Traditional, zh_CN, code page 125170…), RendDX9 0x1017af30 | S–M |
| 11 | **AI console commands for scripted bots** | Co-op and SP scripts that steer bots directly. | AIDLL 0x10004da0–0x100593f0 (59 KB): `botMoveTo`, `botFlyTo`, `botGiveOrder`, `botSetWaypoints_Patrol*`, `addPlacement`, `createLandingZone`, `forceStrategy`, `setTakeable`, `addUnspawnedBot`… src binds only `aiSettings` / `aiStrategy` / `aiTemplate` / `aiStrategicArea` / `weaponTemplate` rows (Bots.cpp). The shipped scripts barely call the rest, so this is low priority unless mods rely on it. | M |
| 12 | **Online back end** (already in coverage-2 §2b) | BFHQ stats, leaderboards, award grants, rank changes, booster entitlements. | Player-info keys `mtt-%d-%d`… (0x004818f0, 8.8 KB), `getleaderboard.aspx` (0x0045d7e0), `rankchange/grantaward/playerdisconnect.aspx` (0x0063da50), `REG-PC-BF2142-UNLOCK-%d` / `online_content_cqc` (0x004a1d70) | L; needs services |
| 13 | **Logitech G15 LCD applet** (game side) | Hardware only. | 0x0070cc50–0x0070fa50 (server, map, ping, K/D and dog tags on the LCD) | S, low value |

Also seen; most of these are names the rewrite spells differently, or debug-only:
- the network event name table (0x0042ec60);
- the `ECHudHE*` HUD event names (0x0074d370);
- the IDKey/IDButton name tables (0x00683e40–0x00686f56; the rewrite parses these names, so mostly (c));
- the threads and core events table (0x0065c6dc);
- the culler and leak bookkeeping (0x00703220, 0x00702150: shutdown asserts, effectively (b)).

## Caveats

- The (c)/(d) split rests on string matching. Composed strings (for example `"SettingsManager." + type + "Set"`) make false (d)s. Generic words make false (c)s. I spot-checked about 50 functions.
- The console-row states are coverage-2's (991d041), upgraded where the literal now exists in src.
- Propagated labels carry the calibrated error rates above.
- Function sizes are Ghidra's. Data tables and switch tables are not counted.
