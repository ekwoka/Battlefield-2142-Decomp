# BF2142 rewrite: coverage re-audit (2026-10-29, HEAD 991d041 against the first audit at 8948ed8)

## How this was measured

I repeated docs/coverage.md's method on two builds. "Before" is a scratch build of 8948ed8 (`git archive`, built outside the repo). "After" is `build/release` at 991d041, which `ninja` said was up to date. Running every measurement on both builds keeps the before/after deltas like-for-like, even where my absolute numbers differ slightly from the first audit's. The scripts are in `scratchpad/tools/`: `probe.py`/`probe2.py` (console probe), `scripts.py` (script-call counts), `analyze.py` + `tables.py` (classification and tables), `cite2.py` + `runs_uncited.py` (address citations), `runs.sh` (level runs). Raw outputs are in `scratchpad/runs/` and `scratchpad/*.json`.

- **Console API probe.** Same method as the first audit: every row of both console tables is turned into a method that takes 0 to 13 arguments, passed with `--api`, plus one `--console "<name> x x x x x x x x x x x"` for each of the 2,688 distinct names, on Camp_Gibraltar gpm_cq 32 `--simulate 1`. On 8948ed8 this gives **469 / 2,219**, exactly the first audit's figure, so the method is confirmed.
  - **New caveat:** the first audit's "no mode-only methods" no longer holds. At HEAD, **158 names register only when hosting** (`--host`, with or without `--dedicated`): `sv.*` 76, `GameServerSettings.*` 45, `admin.*` 16, `maplist.*` 10, `reservedSlots.*` 7, `Game.*` 2, `demo.recordDemo/stopRecording`.
  - Three more `demo.*` methods register only during `--demo` playback (per engine.md). I did not probe those, because that needs a recorded demo.
  - The HEAD numbers below are the union of three probes: `--simulate`, `--host --dedicated` and `--screenshot`. The screenshot probe added nothing.
  - Running the old build with `--host` still gives 469.
- **Categories.** Each table row is one of:
  - **impl**: the probe got an argument-count error back.
  - **stored-read** / **stored-unread**: a non-impl ObjectTemplate property or 1-argument setter that `registerUntypedTemplateProperties` keeps. It counts as read if its name, or `<name>.list` for list adders, appears as a string literal in src. The match is case-insensitive.
  - **stub**: everything else.
  - The literal rule gives 830 unread at 8948ed8, against the first audit's 826. It misses composed keys: `sway.*` is read as `"sway.gunSway" + axis + "Steps"`. So I also give a lower bound: names whose last segment occurs nowhere in src text.
- **Script calls.** I counted every `.con/.tweak/.inc/.ai` in the mod's archives and loose files, plus these levels:
  - the 20 levels that ship `archive.md5`;
  - the 15 official `_coop` variants;
  - the 3 `_Titan16` levels.
  - That is **6,758 files and 824,614 calls**. The first audit's exact level set wasn't recorded; it reported 6,693 / 791k. Both sides of every delta use this one corpus.
- **Level runs.** The same 11 combinations as the first audit, with `--nosound --verbose --simulate 5` (plus `--no-movies` on HEAD), on both builds.
- **Address citations.** I matched hex addresses in src/ and docs/ (`0x…`, bare 8-digit, `FUN_`/`LAB_`) against each module's `build/analysis/<module>/functions.csv`, leaving out `static_init_*`.
  - The DLLs share the base address 0x10000000. An address valid in several DLLs is given to the DLL named nearest before it in the text (within 1,500 chars), or else to the one its src directory implies. 161 occurrences stayed ambiguous and were dropped.
  - This is stricter than the first audit. Its per-module figures can't be reproduced: functions.csv now has 35,589 non-static-init EXE functions against its 34,357, and its 55 "cited" BF2142Audio functions came from cross-DLL address collisions; src/Sound cites almost no DLL addresses.
- **Tests.** `build/release/tests/bf2142_tests` (524 tests) at HEAD; the VoIP test also on a scratch build of 8948ed8.

## 1. Headline numbers, before → after

| Metric | 8948ed8 | 991d041 | Change |
|---|---|---|---|
| Probe, distinct names: implemented / stub | 469 / 2,219 | **999 / 1,689** (841 without `--host`) | +530 |
| Console rows (2,837): impl | 475 | **1,007** | +532 |
| … stub | 1,042 | **510** | −532 |
| … ObjectTemplate stored-read | 490 | 619 | +129 |
| … ObjectTemplate stored-unread (rows) | 830 (first audit: 826) | **701** | −129 |
| Unread template property names (unique, literal rule) | 781 | **663** | −118 |
| … lower bound (last segment absent from src) | 697 | **578** | −119 |
| BF2142.exe rows: impl / stub | 422 / 707 | 840 / 289 | |
| RendDX9 rows: impl / stub | 53 / 335 | 167 / 221 | |
| Start-up line "console methods stubbed" (no `--host`): EXE / RendDX9 | 782 / 333 | 525 / 220 | counts the 158 host-only methods as stubs |
| Stubbed methods called by shipped scripts / their calls | 314 / 34,836 | **85 / 25,157** | 16,416 of the 25,157 are `Object.setLightSourceMask`, which is settled as inert |
| Unread template properties called by scripts: rows / calls | 628 / 120,170 | 503 / 66,948 | unique names: 589 / 100,727 → 475 / 58,164 |
| Level runs: "N calls to M unimplemented methods" | 9,238–10,168 / 157–176 | **5,944–6,872 / 42–63** | |
| gpm_sl Python tracebacks (2 runs) | 2 | 0 | |
| "Unknown object or method!" per run | 6,688–7,598 | unchanged | names retail doesn't register either |
| Python `host.*` names used by shipped scripts and Admin/ | all resolve | all 50 resolve | |
| Unit tests | — | 524 run, 1 failing (flaky, see section 3) | |

### Address citations (functions start-address cited, attributed per module)

| Module | functions / bytes | 8948ed8 cited | 991d041 cited |
|---|---|---|---|
| BF2142.exe | 35,589 / 4,794,004 | 1,458 fns, 668,775 B (14.0%) | **1,983 fns, 852,825 B (17.8%)** |
| RendDX9_ori.dll | 10,810 / 1,677,614 | 108 fns, 102,501 B (6.1%) | **244 fns, 183,557 B (10.9%)** |
| AIDLL.dll | 6,080 / 1,006,188 | 346 fns, 245,582 B (24.4%) | 381 fns, 259,889 B (25.8%) |
| SwiffPlayer.dll | 5,756 / 936,991 | 133 fns, 117,743 B (12.6%) | 153 fns, 122,700 B (13.1%) |
| BF2VoipServer.dll | 377 / 38,482 | 34 fns (28.5%) | 34 fns (28.5%) |
| BF2142Audio.dll | 1,702 / 206,940 | 0 | 2 fns (0.1%); fully rewritten, but cites no DLL addresses |
| Memory.dll | 140 / 10,869 | 0 | 0 (retail drop-in; also cites none) |
| TextureAtlasBuilder.dll | 186 / 80,893 | 0 | 0 (not needed) |

Uncited runs:
- **BF2142.exe** (runs of 30 KB or more): 27 runs, 1.86 MB → **19 runs, 1.24 MB**.
- **RendDX9** (runs of 20 KB or more): 21 runs, 1.25 MB → 30 runs, 0.95 MB. There are more runs because the big ones were split by new citations.

Largest uncited EXE runs at HEAD (labels from the first audit's Table F):

| Range | Size | Contents |
|---|---|---|
| 0x00840350–0x00870c35, 0x008175c0–0x00840328, 0x00870c90–0x00882ff1, 0x00802090–0x008151bf | 178 + 136 + 66 + 72 KB | FESL / EASTL, online |
| 0x004e886d–0x00503dec | 112 KB | template property tables |
| 0x00724c70–0x0073744a | 72 KB | GameSpy GP |
| 0x0075e8f2–0x0076f773 | 69 KB | HudManager settings |
| 0x007c82fb–0x007d641b | 57 KB | awards / popups |
| 0x008c5bc0–0x0090cec5 | 55 KB | PunkBuster |
| 0x0052b781–0x00539094 | 55 KB | particle property tables |
| 0x007380d0–0x00745ea7 | 54 KB | https library |
| 0x0046c3d0–0x0047a0b9, 0x0047a110–0x00487014 | 54 + 52 KB | command line, profile and settings managers |
| 0x0065b300–0x006655dd | 39 KB | ECCore events |
| 0x00697160–0x006a10da | 39 KB | file paths |
| 0x004ded1d–0x004e7af4 | 36 KB | soldier properties |
| 0x0055264c–0x0055a41f | 32 KB | |
| 0x007839b0–0x0078b487 | 31 KB | minimap order icons |
| 0x007be9e0–0x007c657a | 31 KB | HUD variables |

Largest uncited RendDX9 runs: 0x101885b0 (89 KB, debug/menu shaders), 0x101543c0 (50 KB, terrain editor), 0x1019fb20 (49 KB, asserts/debug), 0x1008ce80 (48 KB), 0x10057280 (47 KB, IGA ads), 0x10066bf0 (46 KB), 0x1016d1a0 (39 KB, SwiffRender/NameTagRenderer glue), 0x10101ce0 (35 KB, roads).

### Level runs (calls / methods; tracebacks)

| Run | 8948ed8 | 991d041 | tracebacks |
|---|---|---|---|
| Belgrade gpm_nv 32 | 10,049 / 166 | 6,757 / 53 | 0 → 0 |
| Camp_Gibraltar gpm_cq 32 | 9,901 / 172 | 6,608 / 58 | 0 → 0 |
| Cerbere_Landing gpm_cq 32 | 9,727 / 167 | 6,435 / 54 | 0 → 0 |
| Highway_Tampa gpm_sl 64 | 10,168 / 169 | 6,872 / 54 | 1 → 0 |
| Minsk gpm_ti 48 | 9,670 / 176 | 6,376 / 62 | 0 → 0 |
| Molokai gpm_ti 48 | 9,292 / 167 | 6,001 / 55 | 0 → 0 |
| Operation_Shingle gpm_sl 32 | 9,238 / 157 | 5,944 / 42 | 1 → 0 |
| sidi_power_plant_coop gpm_coop 32 | 9,814 / 163 | 6,519 / 48 | 0 → 0 |
| Suez_Canal gpm_cq 64 | 9,938 / 172 | 6,644 / 58 | 0 → 0 |
| Verdun gpm_sp 16 | 10,063 / 176 | 6,770 / 63 | 0 → 0 |
| Wake_Island_2142 gpm_ca 64 | 9,278 / 173 | 5,986 / 60 | 0 → 0 |

Merged top-25 list at HEAD, summed over the 11 runs:

| Calls | Method |
|---|---|
| 15,756 | GeometryTemplate.compressVertexData |
| 15,734 | maxTextureRepeat |
| 13,292 | lightmapOverride |
| 13,226 | doNotGenerateLightmaps |
| 5,240 | Object.setLightSourceMask |
| 2,541 | GeometryTemplate.isAd |
| 1,265 | ObjectTemplate.armor.addArmorEffectSpectacular |
| 429 | GeometryTemplate.setMaterialReflectionScale |
| 396 | GeometryTemplate.isTitanInterior |
| 330 | RagDoll.toSkeleton |
| 297 | Vars.set |
| 275 | GeometryTemplate.isTitanExterior |
| 198 / 198 | RagDoll.addConstraint / GeometryTemplate.setSpecularStaticGloss |
| 187 | setLightmapSizeAll |
| 176 / 143 / 132 | RagDoll.addAngularConstraint / addParticle / addCapsuleCollision |
| 110 / 110 | Material.triggerRadio / MaterialManager.setRadioMessage |
| 99 | heightmap.setModified |
| 77 | setMaterialParallaxHeightScale |
| 50 / 32 | RagDoll.lockBone / addDihedralAngleConstraint |
| 28 | titanExteriorPartIndex |

Every HUD, warning, ability, deviation and transform-list stub in the first audit's top 25 is gone. What is left is either load-time hints and harmless parser-read rows, or the decodable rendering items listed in section 2.

### Table A'. Console coverage by object, before → after (same 2,837 rows as the first audit's Table A)

| Module | Object | Total | impl before→after | stored-read b→a | stored-unread b→a | stub b→a | script calls to stubs b→a |
|---|---|---|---|---|---|---|---|
| BF2142.exe | ObjectTemplate | 1403 | 50→61 | 490→619 | 830→701 | 33→22 | 2734→172 |
| BF2142.exe | HudBuilder | 139 | 72→115 | 0→0 | 0→0 | 67→24 | 1588→0 |
| BF2142.exe | sv | 76 | 0→76 | 0→0 | 0→0 | 76→0 | 69→0 |
| RendDX9 | renderer | 66 | 12→31 | 0→0 | 0→0 | 54→35 | 382→78 |
| RendDX9 | LightmapGeneration | 50 | 0→0 | 0→0 | 0→0 | 50→50 | 0→0 |
| BF2142.exe | AITemplatePlugIn | 49 | 22→40 | 0→0 | 0→0 | 27→9 | 1284→32 |
| BF2142.exe | Object | 47 | 7→7 | 0→0 | 0→0 | 40→40 | 16568→16568 |
| BF2142.exe | GameServerSettings | 45 | 0→45 | 0→0 | 0→0 | 45→0 | 0→0 |
| BF2142.exe | GeneralSettings | 41 | 0→41 | 0→0 | 0→0 | 41→0 | 0→0 |
| BF2142.exe | GameLogic | 41 | 14→22 | 0→0 | 0→0 | 27→19 | 1081→34 |
| BF2142.exe | AnimationSystem | 32 | 13→13 | 0→0 | 0→0 | 19→19 | 0→0 |
| BF2142.exe | HudManager | 31 | 4→29 | 0→0 | 0→0 | 27→2 | 17→0 |
| BF2142.exe | Commander | 31 | 27→27 | 0→0 | 0→0 | 4→4 | 0→0 |
| RendDX9 | shadowManager | 30 | 0→30 | 0→0 | 0→0 | 30→0 | 0→0 |
| BF2142.exe | AnimationBundle | 28 | 23→23 | 0→0 | 0→0 | 5→5 | 0→0 |
| RendDX9 | lightManager | 27 | 6→27 | 0→0 | 0→0 | 21→0 | 456→0 |
| RendDX9 | Overgrowth | 27 | 0→0 | 0→0 | 0→0 | 27→27 | 448→448 |
| BF2142.exe | Sound | 26 | 18→19 | 0→0 | 0→0 | 8→7 | 54→1 |
| RendDX9 | GeometryTemplate | 26 | 4→8 | 0→0 | 0→0 | 22→18 | 6383→6346 |
| RendDX9 | rainManager | 24 | 17→17 | 0→0 | 0→0 | 7→7 | 121→121 |
| BF2142.exe | weaponTemplate | 22 | 12→12 | 0→0 | 0→0 | 10→10 | 9→9 |
| BF2142.exe | RagDoll | 20 | 0→0 | 0→0 | 0→0 | 20→20 | 102→102 |
| RendDX9 | ParticleSystemManager | 20 | 0→20 | 0→0 | 0→0 | 20→0 | 0→0 |
| RendDX9 | skyDome | 19 | 4→19 | 0→0 | 0→0 | 15→0 | 418→0 |
| BF2142.exe | Material | 18 | 16→16 | 0→0 | 0→0 | 2→2 | 11→11 |
| BF2142.exe | LocalProfile | 16 | 0→16 | 0→0 | 0→0 | 16→0 | 0→0 |
| BF2142.exe | admin | 16 | 0→16 | 0→0 | 0→0 | 16→0 | 0→0 |
| BF2142.exe | AnimationTrigger | 16 | 12→12 | 0→0 | 0→0 | 4→4 | 0→0 |
| BF2142.exe | AudioSettings | 15 | 0→15 | 0→0 | 0→0 | 15→0 | 0→0 |
| BF2142.exe | collisionManager | 15 | 1→1 | 0→0 | 0→0 | 14→14 | 0→0 |
| BF2142.exe | Console | 14 | 0→0 | 0→0 | 0→0 | 14→14 | 26→26 |
| RendDX9 | terrain | 14 | 2→2 | 0→0 | 0→0 | 12→12 | 114→114 |
| BF2142.exe | VideoSettings | 13 | 0→13 | 0→0 | 0→0 | 13→0 | 0→0 |
| BF2142.exe | ControlMap | 12 | 7→12 | 0→0 | 0→0 | 5→0 | 2→0 |
| BF2142.exe | Minimap | 12 | 0→12 | 0→0 | 0→0 | 12→0 | 0→0 |
| RendDX9 | nametags | 12 | 0→0 | 0→0 | 0→0 | 12→12 | 105→105 |
| BF2142.exe | SwiffHost | 11 | 0→11 | 0→0 | 0→0 | 11→0 | 1078→0 |
| BF2142.exe | Player | 11 | 0→0 | 0→0 | 0→0 | 11→11 | 0→0 |
| BF2142.exe | CombatArea | 11 | 8→11 | 0→0 | 0→0 | 3→0 | 0→0 |
| BF2142.exe | heightmapcluster | 11 | 5→5 | 0→0 | 0→0 | 6→6 | 0→0 |
| BF2142.exe | maplist | 10 | 0→10 | 0→0 | 0→0 | 10→0 | 1→0 |
| BF2142.exe | SettingsManager | 10 | 0→10 | 0→0 | 0→0 | 10→0 | 10→0 |
| BF2142.exe | SquadMenu | 10 | 10→10 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | AITemplate | 10 | 6→9 | 0→0 | 0→0 | 4→1 | 351→0 |
| RendDX9 | terrainCuller | 10 | 7→7 | 0→0 | 0→0 | 3→3 | 0→0 |
| RendDX9 | StaticMeshRenderer | 10 | 0→0 | 0→0 | 0→0 | 10→10 | 0→0 |
| RendDX9 | undergrowth | 10 | 0→1 | 0→0 | 0→0 | 10→9 | 74→74 |
| BF2142.exe | NetworkableInfo | 9 | 0→5 | 0→0 | 0→0 | 9→4 | 35→0 |
| BF2142.exe | UnlockInterface | 9 | 9→9 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | demo | 8 | 0→2 | 0→0 | 0→0 | 8→6 | 0→0 |
| BF2142.exe | CommanderMenu | 8 | 7→7 | 0→0 | 0→0 | 1→1 | 0→0 |
| BF2142.exe | KitInterface | 8 | 8→8 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | AnimationValueHolder | 8 | 4→4 | 0→0 | 0→0 | 4→4 | 0→0 |
| RendDX9 | ObjectDrawer | 8 | 0→0 | 0→0 | 0→0 | 8→8 | 0→0 |
| BF2142.exe | SquadManager | 7 | 0→0 | 0→0 | 0→0 | 7→7 | 0→0 |
| BF2142.exe | reservedSlots | 7 | 0→7 | 0→0 | 0→0 | 7→0 | 0→0 |
| BF2142.exe | heightmap | 7 | 6→6 | 0→0 | 0→0 | 1→1 | 342→342 |
| BF2142.exe | Radio | 7 | 5→5 | 0→0 | 0→0 | 2→2 | 0→0 |
| BF2142.exe | kitTemplate | 7 | 0→4 | 0→0 | 0→0 | 7→3 | 144→0 |
| BF2142.exe | AnimationManager | 7 | 6→6 | 0→0 | 0→0 | 1→1 | 0→0 |
| RendDX9 | DecalManager | 7 | 0→0 | 0→0 | 0→0 | 7→7 | 0→0 |
| RendDX9 | OvergrowthType | 7 | 0→0 | 0→0 | 0→0 | 7→7 | 372→372 |
| BF2142.exe | Game | 6 | 1→3 | 0→0 | 0→0 | 5→3 | 0→0 |
| BF2142.exe | MaterialManager | 6 | 5→5 | 0→0 | 0→0 | 1→1 | 10→10 |
| RendDX9 | Object | 6 | 1→1 | 0→0 | 0→0 | 5→5 | 0→0 |
| BF2142.exe | CombatAreaManager | 5 | 1→5 | 0→0 | 0→0 | 4→0 | 121→0 |
| BF2142.exe | SquadLeader | 5 | 1→1 | 0→0 | 0→0 | 4→4 | 0→0 |
| RendDX9 | Renderer | 5 | 0→0 | 0→0 | 0→0 | 5→5 | 0→0 |
| RendDX9 | HemiMapManager | 5 | 0→4 | 0→0 | 0→0 | 5→1 | 50→0 |
| BF2142.exe | GlobalSettings | 4 | 0→4 | 0→0 | 0→0 | 4→0 | 0→0 |
| BF2142.exe | RadioInterface | 4 | 4→4 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | SpawnManager | 4 | 3→3 | 0→0 | 0→0 | 1→1 | 0→0 |
| BF2142.exe | MenuTeamManager | 3 | 0→3 | 0→0 | 0→0 | 3→0 | 10→0 |
| BF2142.exe | HudItems | 3 | 3→3 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | SquadLeaderInterface | 3 | 3→3 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | SquadInterface | 3 | 3→3 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | CommanderInterface | 3 | 3→3 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | RadioVehicleInterface | 3 | 3→3 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | DemoCameraInterface | 3 | 3→3 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | DemoRecorderInterface | 3 | 3→3 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | CameraInterface | 3 | 3→3 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | FreeCam | 3 | 0→2 | 0→0 | 0→0 | 3→1 | 0→0 |
| RendDX9 | envmapManager | 3 | 0→0 | 0→0 | 0→0 | 3→3 | 0→0 |
| BF2142.exe | renderer | 2 | 0→0 | 0→0 | 0→0 | 2→2 | 0→0 |
| BF2142.exe | pythonHost | 2 | 0→0 | 0→0 | 0→0 | 2→2 | 0→0 |
| BF2142.exe | rankSettings | 2 | 1→1 | 0→0 | 0→0 | 1→1 | 0→0 |
| BF2142.exe | FileManager | 2 | 2→2 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | Physics | 2 | 0→0 | 0→0 | 0→0 | 2→2 | 0→0 |
| BF2142.exe | WindManager | 2 | 0→2 | 0→0 | 0→0 | 2→0 | 68→0 |
| BF2142.exe | ObjectManager | 2 | 0→0 | 0→0 | 0→0 | 2→2 | 0→0 |
| BF2142.exe | Chat | 2 | 2→2 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | Scoreboard | 2 | 2→2 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | LevelsList | 2 | 2→2 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | awards | 1 | 0→0 | 0→0 | 0→0 | 1→1 | 126→126 |
| BF2142.exe | misc | 1 | 0→0 | 0→0 | 0→0 | 1→1 | 0→0 |
| BF2142.exe | Vars | 1 | 0→0 | 0→0 | 0→0 | 1→1 | 27→27 |
| BF2142.exe | bf2Engine | 1 | 0→1 | 0→0 | 0→0 | 1→0 | 6→0 |
| BF2142.exe | Renderer | 1 | 1→1 | 0→0 | 0→0 | 0→0 | 0→0 |
| BF2142.exe | responsePhysicsManager | 1 | 0→0 | 0→0 | 0→0 | 1→1 | 0→0 |
| BF2142.exe | Editor | 1 | 0→0 | 0→0 | 0→0 | 1→1 | 0→0 |
| BF2142.exe | ItemSelection | 1 | 0→0 | 0→0 | 0→0 | 1→1 | 0→0 |
| BF2142.exe | Squad | 1 | 0→0 | 0→0 | 0→0 | 1→1 | 0→0 |
| BF2142.exe | IngameHelp | 1 | 0→0 | 0→0 | 0→0 | 1→1 | 0→0 |
| BF2142.exe | aiComm | 1 | 1→1 | 0→0 | 0→0 | 0→0 | 0→0 |
| RendDX9 | texturemanager | 1 | 0→0 | 0→0 | 0→0 | 1→1 | 39→39 |
| RendDX9 | SkinnedMeshRenderer | 1 | 0→0 | 0→0 | 0→0 | 1→1 | 0→0 |
| | **Total** | 2837 | 475→1007 | 490→619 | 830→701 | 1042→510 | 34836→25157 |

### Table B'. Every stubbed method the shipped scripts still call (84 names, 25,157 calls)

"GAP" marks the ones a player can notice. The rest are load-time hints, editor or debug flags, or rows read by their own parsers.

| Calls | Method | Note |
|---|---|---|
| 16416 | `Object.setLightSourceMask` | settled inert (engine.md:1620) |
| 1665 | `GeometryTemplate.compressVertexData` | load hint |
| 1660 | `GeometryTemplate.maxTextureRepeat` | load hint |
| 1248 | `GeometryTemplate.lightmapOverride` | lightmapper hint |
| 1242 | `GeometryTemplate.doNotGenerateLightmaps` | lightmapper hint |
| 342 | `heightmap.setModified` | editor flag |
| 231 | `GeometryTemplate.isAd` | IGA ads (dropped) |
| 144 | `Object.disableChildren` | GAP: children spawned on watchtowers etc. |
| 133 | `ObjectTemplate.armor.addArmorEffectSpectacular` | GAP: destruction effects |
| 126 | `awards.setData` | parsed by src/Stats (awards.con) |
| 108 | `GeometryTemplate.isTitanInterior` | GAP: Titan interior culling |
| 76 | `terrain.waterSunIntensity` | GAP: water look |
| 74 | `undergrowth.load` | read from level files |
| 73 | `GeometryTemplate.isTitanExterior` | GAP: Titan exterior culling |
| 62 | `Overgrowth.addType` | read from level files |
| 62 | `Overgrowth.setActiveType` | read from level files |
| 62 | `OvergrowthType.geometry` | read from level files |
| 62 | `OvergrowthType.density` | read from level files |
| 62 | `OvergrowthType.normalScale` | read from level files |
| 62 | `OvergrowthType.rotationScale` | read from level files |
| 62 | `OvergrowthType.minRadiusToSame` | read from level files |
| 62 | `OvergrowthType.minRadiusToOthers` | read from level files |
| 55 | `nametags.setTexture` | GAP: nametag icons |
| 48 | `GeometryTemplate.setMaterialReflectionScale` | GAP: envmap reflection |
| 45 | `nametags.createIcon` | GAP: nametag icons |
| 39 | `texturemanager.customTextureSuffix` | "" in every level |
| 38 | `renderer.waterFogStartEndAndBase` | GAP: underwater fog |
| 38 | `renderer.waterFogColor` | GAP: underwater fog |
| 38 | `terrain.init` | level init |
| 38 | `Overgrowth.editorEnable` | read from level files |
| 36 | `Overgrowth.path` | read from level files |
| 36 | `Overgrowth.preLoad` | read from level files |
| 36 | `Overgrowth.viewDistance` | read from level files |
| 36 | `Overgrowth.viewDistanceScale` | read from level files |
| 36 | `Overgrowth.closeDistPercentage` | read from level files |
| 35 | `Overgrowth.addMaterial` | read from level files |
| 35 | `Overgrowth.setActiveMaterial` | read from level files |
| 30 | `GameLogic.radialLevelIndex` | GAP? |
| 30 | `RagDoll.toSkeleton` | own parser (src/Animation/RagDoll.cpp) |
| 29 | `GeometryTemplate.setSpecularStaticGloss` | GAP: gloss (0.15 used) |
| 29 | `rainManager.cellDisplay` | debug |
| 27 | `Vars.set` | own parser |
| 26 | `ObjectTemplate.addVehicleType` |  |
| 26 | `Console.allowMultipleFileLoad` | loader flag |
| 23 | `rainManager.randomness` | GAP: rain detail |
| 23 | `rainManager.maxRandomChange` | GAP: rain detail |
| 23 | `rainManager.randomDeviationAngleSpeed` | GAP: rain detail |
| 23 | `rainManager.freeze` | rain (debug?) |
| 21 | `AITemplatePlugIn.cullDistance` | GAP: AI cull |
| 18 | `RagDoll.addConstraint` | own parser (src/Animation/RagDoll.cpp) |
| 17 | `GeometryTemplate.setLightmapSizeAll` | lightmapper hint |
| 16 | `RagDoll.addAngularConstraint` | own parser (src/Animation/RagDoll.cpp) |
| 13 | `RagDoll.addParticle` | own parser (src/Animation/RagDoll.cpp) |
| 12 | `RagDoll.addCapsuleCollision` | own parser (src/Animation/RagDoll.cpp) |
| 12 | `GeometryTemplate.titanExteriorPartIndex` | GAP: Titan exterior parts |
| 10 | `Material.triggerRadio` | GAP: material radio |
| 10 | `MaterialManager.setRadioMessage` | GAP: material radio |
| 8 | `ObjectTemplate.ability.setOverheatMod` | GAP: unlock |
| 8 | `Object.notInAI` |  |
| 7 | `GeometryTemplate.setMaterialParallaxHeightScale` | GAP: parallax |
| 5 | `weaponTemplate.shouldRearm` |  |
| 5 | `RagDoll.lockBone` | own parser (src/Animation/RagDoll.cpp) |
| 5 | `nametags.createBar` | GAP: nametag bars |
| 4 | `RagDoll.addDihedralAngleConstraint` | own parser (src/Animation/RagDoll.cpp) |
| 4 | `GeometryTemplate.nrOfAnimatedUVMatrix` | GAP: animated UVs |
| 3 | `ObjectTemplate.setCheckObstruction` |  |
| 3 | `AITemplatePlugIn.setSoundSphereRadius` |  |
| 3 | `AITemplatePlugIn.setHearingProbability` |  |
| 3 | `AITemplatePlugIn.setUseNoPathfindingToGetToObject` |  |
| 2 | `ObjectTemplate.setIfTitanChangeToWeapon` |  |
| 2 | `AITemplatePlugIn.vehicleNumber` |  |
| 2 | `weaponTemplate.fireRate` |  |
| 2 | `weaponTemplate.isAntiAirWeapon` |  |
| 2 | `RagDoll.addDistanceLessThanConstraint` | own parser (src/Animation/RagDoll.cpp) |
| 2 | `renderer.useSunScaling` |  |
| 2 | `GeometryTemplate.boundingRadiusModifier` |  |
| 1 | `GameLogic.enableVoHelp` |  |
| 1 | `GameLogic.supplyDropHeight` |  |
| 1 | `GameLogic.supplyDropNumSecsToLive` |  |
| 1 | `GameLogic.createObject` |  |
| 1 | `Material.pythonEvent` |  |
| 1 | `Sound.windSetup` |  |
| 1 | `RagDoll.skeleton` | own parser (src/Animation/RagDoll.cpp) |
| 1 | `RagDoll.setParticleCollisionCriteria` | own parser (src/Animation/RagDoll.cpp) |

### Unread template properties at HEAD

Unread template properties by prefix (literal rule; calls in the shipped scripts):

| Prefix | unread names | script calls |
|---|---|---|
| (no prefix) | 361 | 49535 |
| nametag.* | 14 | 2862 |
| vehicleHud.* | 45 | 1632 |
| weaponHud.* | 29 | 817 |
| ammo.* | 15 | 449 |
| armor.* | 20 | 449 |
| fire.* | 33 | 403 |
| recoil.* | 6 | 345 |
| detonation.* | 13 | 267 |
| ability.* | 15 | 218 |
| penetrate.* | 2 | 169 |
| target.* | 10 | 149 |
| radio.* | 5 | 133 |
| python.* | 1 | 117 |
| follow.* | 5 | 108 |
| collision.* | 6 | 99 |
| zoom.* | 13 | 90 |
| sway.* | 22 | 78 |
| Nametag.* | 3 | 54 |
| sound.* | 1 | 38 |
| geometry.* | 1 | 32 |
| seek.* | 2 | 31 |
| helpHud.* | 10 | 30 |
| strategicObject.* | 6 | 30 |
| audio.* | 2 | 11 |
| antenna.* | 5 | 10 |
| animation.* | 3 | 4 |
| spawn.* | 2 | 2 |
| pulse.* | 1 | 2 |

**Caveat on false positives.** `sway.*` (22 names, all read through composed keys in `GunSway::fromTemplate`, src/Game/Weapons.cpp:564) and similar names count as "unread" under the literal rule. The lower-bound list below leaves out any name whose last segment appears anywhere in src.

Top unread names under the lower bound (calls):

`isNotSaveable` 16862, `saveInSeparateFile` 4709, `createdInEditor` 3543, `animationSpeed` 1267, `animationPlayOnce` 1233, `selfShadowIntensity` 1233, `nametag.show3dIcon` 1175, `showInZoomedMode` 1149, `noRotationFromParent` 1049, `randomAgeFactor` 439, `speedFromPhysics` 439, `nearCullingEnable` 424, `linkedEffectContainer` 290, `createNotInGrid` 259, `castsDynamicShadow` 258, `vehicleHud.miniMapInfo` 244, `cullRadiusScale` 239, `projectOnHeightmap` 232, `usedOnTransparentSurface` 224, `sizeModifier` 224, `weaponHud.hasFireRate` 220, `startRotation` 215, `zRotation` 215, `zRotationSnap` 215, `writeZ` 215, `directionalScale` 215, `vehicleHud.useSelectionIcons` 214, `keepProjectiles` 210, `replaceItem` 196, `minimumTimeBetweenTriggering` 195, `chance` 193, `averageTimeBetweenTests` 193, `positionalSpeedInUp` 179, `positionalSpeedInRight` 177, `isStaticSystem` 165, `vehicleHud.vehicleIconPos` 164, `damagedAmbientSoundLimit` 163, `allowEmoteAnimation` 158, `positionalSpeedInDof` 152, `vehicleHud.miniMapIcon` 149, `hasOnTimeEffect` 147, `rotationalSpeedInDof` 139, `maintainCameraOnExit` 138, `weaponHud.selectIcon` 137, `hasRestrictedExit` 135, `listenerObstruction` 131, `rotationalSpeedInUp` 131, `rotationalSpeedInRight` 130, `fire.fireInCameraDof` 125, `autoUseAbility` 123, `weaponHud.disableOnSprint` 121, `dontClearTeamOnExit` 120, `ignoreParentRotation` 120, `python.onRepairEvent` 117, `exitSpeedMod` 115, `showOnMinimap` 112, `isVisible` 111, `boneIndex` 108, `penetrate.allowSolidPenetration` 106, `vehicleHud.typeIcon` 102, `delayToUse` 95, `castsStaticShadow` 94, `selfLights` 94, `selfShadows` 94, `supportsPerVertex` 94, `supportsPerPixel` 94, `supportsGlow` 94, `supportsEmitter` 94, `HDRIntensity` 94, `softness` 94, `objectShadows` 94, `nametag.useBoundingRadius` 94, `recoil.cameraRecoilSpeed` 93, `recoil.cameraRecoilSize` 93, `takeOverInput` 93, `projectorTexture` 93, `projectorFov` 93, `projectorAspect` 93, `projectorNear` 93, `projectorFar` 93, `ammo.ammoType` 90, `maintainCameraOnExitToOtherVehicle` 84, `nametag.showInside` 82, `addWorkOnMaterial` 80, `nametag.selected3dMapIcon` 80, `weaponHud.ShowClips` 79, `weaponHud.overheatSound` 79, `alphaIntensityRange` 78, `emitScaleFactorRange` 78, `emitSpeedScaleRange` 78, `startAgeRange` 78, `AgeScaleFactorRange` 78, `detonation.detectionRadius` 76, `alphaIntensityUsed` 76, `emitScaleFactorUsed` 76, `emitSpeedScaleUsed` 76, `startAgeUsed` 76, `AgeScaleFactorUsed` 76, `detonation.maxDepthForExplosion` 73, `numNetworkedEffects` 72, `detonation.useMMOnEndEffect` 67, `recoil.recoilSize` 66, `recoil.recoilSpeed` 66, `hitImpactModifier` 65, `ability.sprintRechargeModifier` 64, `penetrate.allowLiquidPenetration` 63, `altItemIndex` 60, `alternateTemplate0` 60, `alternateTemplate1` 60, `alternateTemplate2` 60

Stubs never called by the shipped scripts, by object (HEAD):

- **LightmapGeneration** (RendDX9, 50): GILightColor, GILightsBaseDir, GILightsFov, GILightsIntensity, GILightsShadowIntensity, GILightsViewDistance, GITopLightIntensity, ambientColor, blockSize, cameraNearPlane, directlyLightedThreshold, doOvergrowth, enableBackfaceCulling, enableIntensityLightmaps, flush, generationMethod, getWaterSunIntensity, globalSoftness, init, light.create, light.direction, light.fov, light.intensity, light.objectShadows, light.selfShadowing, light.shadowIntensity, light.softness, light.viewDistance, lightmapFormat, noPhongAngle, numberOfGILights, objectPointScale, onlyNearest, phongAngleMode, renderTerrain, resolutionModifier, sampleOffset, shadowsNearCamera, skyColor, skyWhite, sunLightFov, sunLightIntensity, sunLightShadowIntensity, terrainPointScale, traceActiveObject, traceActiveObjectProgress, traceAllObjects, traceTerrain, useGITopLight, useLog
- **Object** (BF2142.exe, 37): HDRIntensity, absolutePositionSecondary, active, attenuationRange1, attenuationRange2, color, coneAngle1, coneAngle2, direction, forceStart, forceStop, fov, getLightSourceMask, getVisibleTeam, glowFrequency, glowOffset, glowType, hasCollision, hasGlow, hasUpdate, initGrid, isInGrid, isSaveable, isVisible, list, listObjectsOfTemplate, rotationSecondary, scale, setAsSkyLight, setIsDisabledRecursive, setIsInTweakModeRecursive, setIsSaveableRecursive, setIsVisibleRecursive, setVisibleTeam, template, togglePause
- **renderer** (RendDX9, 32): alphaToMask, draw1pMeshes, drawBundledMeshes, drawFps, drawGraphs, drawHud, drawNametags, drawObjects, drawOvergrowth, drawParticles, drawRoads, drawSkinnedMeshes, drawSkyDome, drawStaticMeshes, drawSunFlare, drawTerrain, drawTrees, drawUndergrowth, dumpScreen, enableHWOcclusion, hud, indoorBundledMeshCullDistance, indoorSkinnedMeshCullDistance, makeTopWorldScreen, minCullDistance, presentAsync, renderAheadLimit, runTitanExplosion, tweakNearPlane, tweakNearPlane1p, vsync, waterNormalMap
- **HudBuilder** (BF2142.exe, 24): addNodeVariableMoveShowEffect, createSliderNode, deleteNode, searchNodes, setAnimationNodeFrames, setAnimationNodeLooping, setAnimationNodeSpeed, setBarNodeAnim, setBarNodeEffect, setBarNodeMove, setBarNodeVariableTexture, setButtonNodeFunction, setButtonNodeMouseOverColor, setCompassNodeVariableTexture, setHoverInMiddlePos, setHoverMaxValue, setHoverWidthLength, setModifyer, setMouseOverNodeTexture, setNodePos, setPictureNodeAlphaMask, setSliderNodeChild, setSliderNodeData, setTileNodeOptions
- **AnimationSystem** (BF2142.exe, 19): active, activeAnimation, activeBundle, activeTrigger, activeValueHolder, create, deleteActiveBundle, deleteActiveTrigger, hide, list, listAnimations, listBundles, listTriggerTypes, listTriggers, listValueHolders, reloadScript, save, saveActive, saveValueHolders
- **ObjectTemplate** (BF2142.exe, 17): ability.addExtraAmmoItemToKit, armor.addWreckArmorEffect, clearTemplateList, delete, getHeatBarType, getPosition, getRotation, getTemplate, list, listTemplateClasses, listTemplates, name, save, target.onLockChangeToWeapon, vehicleHud.showOnCamMode, weaponHud.showOnCamMode
- **Overgrowth** (RendDX9, 16): closeDistFade, exportToTerrainTexture, generateAll, getActiveMaterialName, getGeometryList, getLodDistance, getMaterialList, getTypeList, lodTimer, paintMaterial, paintShadowmap, postLoad, removeActiveMaterial, removeActiveType, save, setAtlasGenerationSourcePath
- **GameLogic** (BF2142.exe, 14): becomeSquadLeader, defaultTimeToNextAIWave, extraUAVVehicleFlightHeight, getDefaultNumberOfTicketsEx, getTeamLanguage, getTeamName, orbitalBombardmentHeight, remoteCommand, reset, satelliteScanReloadTime, spawnAtCameraPosition, spawnObjectSpeed, togglePause, uavReloadTime
- **collisionManager** (BF2142.exe, 14): attachActiveToObjectInFocus, buildDebugCollisionMeshes, create, drawBoundingBoxes, drawMeshes, drawPivots, drawPrimitives, drawSoldierMesh, getNameOfObjectInFocus, identityRotateObjectInFocus, rotateObjectInFocusAroundAxis, rotateObjectInFocusAroundVector, setActiveLod, useDeferredLoading
- **Console** (BF2142.exe, 13): getActiveMethods, getArgumentTypes, getCategory, getDelta, getEnumName, getRange, getReturnType, getTypeDescription, hasBundledType, hasRange, isDynamic, isSetGet, useRelativePaths
- **Player** (BF2142.exe, 11): active, create, isAi, isNetwork, list, listPlayerClasses, moveTo, name, saveAll, setVehicleWithInputId, vehicle
- **RagDoll** (BF2142.exe, 10): active, addBone, addForce, addJointConstraint, drawConstraints, gravity, linkMode, removeConstraint, reset, save
- **terrain** (RendDX9, 10): forceRenderStyle, waterAlphaAdd, waterAlphaHeightMul, waterAnimSpeed, waterColor, waterNormalMap, waterQuality, waterScroll, waterSpecularColor, waterSpecularPower
- **StaticMeshRenderer** (RendDX9, 10): allowAnisoFiltering, convertLightmapName, enableLightIndexes, enableLightMapAtlas, enableNewLightmapNames, enableRendering, enableUnifiedIndexlist, materialCullingEnabled, maxLightMappedLod, noLods
- **nametags** (RendDX9, 9): deathFadeOutTime, deployableDistance, getCullDistance, manDownFadeOutTime, maxCallDistance, maxFriendlyDistance, setCullDistance, vehicleNametagOffset, vehicleNametagOffsetFactor
- **ObjectDrawer** (RendDX9, 8): collectPlanesDistance, drawDebugPlanes, faceForwardThreshold, isInsideTitanOverride, showAllTitanParts, showCullStats, useExactTest, useOcclusion
- **undergrowth** (RendDX9, 8): enabled, reload, simpleShaderEnable, test, test2, viewDistanceFade, viewDistanceScale, zPassPatchCount
- **SquadManager** (BF2142.exe, 7): changeSquadName, joinSquad, leaveSquad, listSquadMembers, listSquads, makeMeCommander, popNextCommanderApplicant
- **weaponTemplate** (BF2142.exe, 7): active, explosionRadius, firesThroughTransparent, firingPose, soundSphereRadius, weaponActivate, weaponFire
- **DecalManager** (RendDX9, 7): decalGeomClipAngle, decalNormalOffset, drawStats, fadeDistance, fadeOutAtPercentageOfTimeToLive, startFadePercentageOfFadeDistancel, timeToLive
- **demo** (BF2142.exe, 6): adjustDemoFov, restartDemo, shutdownDemo, toggleCameraDemo, toggleDemoPauseOrPlay, togglePlayerDemo
- **heightmapcluster** (BF2142.exe, 6): debugCheckMaterials, importSurroundingRawHeightMap, simulateWater, simulatedWaterDepth, smoothEdges, stitchEdges
- **Sound** (BF2142.exe, 6): interactiveMode, listenerReverb, removeAllTriggers, setMuteLevel, showSoundSources, showStats
- **AnimationBundle** (BF2142.exe, 5): getAnimationLength, getAnimationStartTime, getEventStartTime, listAnimations, listEvents
- **Renderer** (RendDX9, 5): disableFadeOnRoads, fieldOfView1p, roadDepthBias, roadLodDistMod, roadSlopeScaleDepthBias
- **Object** (RendDX9, 5): geometry.GenerateLightmapForThisInstance, geometry.subGeometry, geometry.subGeometryCount, geometry.technique, geometry.template
- **NetworkableInfo** (BF2142.exe, 4): dumpNetworkDescriptors, dumpScopableDescriptors, makeStatisticsMark, saveStats
- **SquadLeader** (BF2142.exe, 4): addRemoveCP, issueSquadRequest, sendRequest, sendRequestForOrder
- **Commander** (BF2142.exe, 4): commanderMutiny, detailedView, spottedMouseOver, zoomCommander
- **AITemplatePlugIn** (BF2142.exe, 4): aimControls, driveControls, lookControls, selectKey
- **AnimationValueHolder** (BF2142.exe, 4): getFilename, max, min, normal
- **AnimationTrigger** (BF2142.exe, 4): getTypeName, listBundles, removeBundle, removeChild
- **GeometryTemplate** (RendDX9, 4): active, dumpInstances, name, printInfo
- **Game** (BF2142.exe, 3): crash, lockFps, simulationRate
- **kitTemplate** (BF2142.exe, 3): getBattleStrength, getStrategicStrength, getType
- **envmapManager** (RendDX9, 3): generateEnvMaps, loadEMIFile, save
- **terrainCuller** (RendDX9, 3): cullerType, detailCullMaxLevel, updateLodLevel
- **renderer** (BF2142.exe, 2): listScreenModes, screenMode
- **pythonHost** (BF2142.exe, 2): reinitialize, sendCommand
- **Physics** (BF2142.exe, 2): airDensityZeroAtHeight, gravity
- **ObjectManager** (BF2142.exe, 2): drawStats, printNetStatsForAllTypeOfObjects
- **HudManager** (BF2142.exe, 2): addFavouriteServer, enableSayPlayerChatBox
- **Radio** (BF2142.exe, 2): answer, sendKitMessage
- **rainManager** (RendDX9, 2): clearChanges, getChanged
- **rankSettings** (BF2142.exe, 1): save
- **misc** (BF2142.exe, 1): getBuildNr
- **responsePhysicsManager** (BF2142.exe, 1): dumpLists
- **Editor** (BF2142.exe, 1): listObjectsOfTemplate
- **ItemSelection** (BF2142.exe, 1): setReverseItems
- **Squad** (BF2142.exe, 1): leaveSquad
- **CommanderMenu** (BF2142.exe, 1): sendRadioMessage
- **SpawnManager** (BF2142.exe, 1): setPlayerTeam
- **IngameHelp** (BF2142.exe, 1): showIngameHelp
- **FreeCam** (BF2142.exe, 1): changeTeam
- **AITemplate** (BF2142.exe, 1): types
- **AnimationManager** (BF2142.exe, 1): originalLength
- **SkinnedMeshRenderer** (RendDX9, 1): shaderQuality
- **HemiMapManager** (RendDX9, 1): makeHemiMap
- **OvergrowthType** (RendDX9, 1): randomScale


## 2. Remaining gaps, ranked by player visibility

Effort: **S** = days, **M** = 1 to 3 weeks, **L** = a month or more. Evidence gives the call counts in the shipped scripts (6,758-file corpus) and the console state at HEAD.

### 2a. Decodable work left

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| 1 | **Projectile tracers** | No tracer streaks from machine guns and vehicle guns. | GenericProjectile `tracerTemplate` (52 templates, e.g. `p_tracer2_as/eu`), `tracerInterval` (7), `tracerSizeModifier`/`tracerScaler` (52), `tracerConvergeDistance` (45), `min/maxTracerScaler` (39): all unread. "tracer" occurs nowhere in src/Game or src/Effects. **Status: done.** (tracers: `FireArm::takeTracer`, `tracerLook`, `GameWorld::placeTracers`; 0x005f2bb0, 0x005b3e60, 0x005b3cb0). Left: the tracer mesh's blend (drawn added), joined clients (their rounds are ghosts). | S–M |
| 2 | **Player fire modes** (single, burst, auto) | The fire-mode key does nothing. Weapons with several modes can't switch, and the HUD's fire-rate field is idle. | `fire.addFireRate` (166) is kept as `.list`, but only bots read it (Bots.cpp:116). `c_PIToggleFireRate` (46) is mapped (ControlMap.cpp:403, GameProtocol.h:61) with no handler. `weaponHud.hasFireRate` (220) is unread. The SwitchFireRate sound exists (0x005dc290) but nothing plays it. **Status: done.** (`FireArm` modes and toggle, 0x005f28a0/0x005f1370/0x005f1820/0x005f1da0; SwitchFireRate sound; the HUD's FireRateIcon from `FireArm::fireMode`). Drones and sentries keep firing as held. | S |
| 3 | **Camera recoil** | No view kick when firing; only the weapon recoil model applies. | `recoil.cameraRecoilSize` / `cameraRecoilSpeed` (93 each) and `recoil.recoilSize`/`recoilSpeed` (66) are unread. GameWorld.cpp:2472-2480 reads the other recoil fields. **Status: done.** (`CameraRecoil`, 0x00610180/0x006101c0, in `PlayerCamera::shakeDegrees`). The gun's own WeaponBasedRecoilComp recoil (recoilSize/Speed/Force) is still unread. | S |
| 4 | **Vehicle and weapon HUD icons, minimap vehicle icons** | Vehicle icons on the minimap and HUD fall back to defaults. Selection icons, clip display, range finder, distance counter and overheat sound are missing. | `vehicleHud.*`: 45 names unread (1,632 calls), including `miniMapInfo` 244, `useSelectionIcons` 214, `vehicleIconPos` 164, `miniMapIcon` 149, `vehicleType` 136, `typeIcon` 102, `spottedIcon`, `showOnEnemyMinimap`. `weaponHud.*`: 29 unread (817), including `selectIcon` 137, `disableOnSprint` 121, `ShowClips` 79, `overheatSound` 79, `crosshairIcon`, `hasRangefinder`, `hasDistanceCounter`, `min/maxVisualDeviation`. Uncited EXE ranges 0x007839b0 (minimap icons) and 0x007be9e0 (HUD variables). **Status (HUD agent):** done in `src/Main/HudMenusVehicles` (engine.md "The weapon, item selection and vehicle HUD"): the weapon HUD of 0x007c1f10 (showAmmo/showClips/hasMag/hasFireRate/showBatteryIconInHud, clip and ammo icons, the clip count and colours, overheatSound), the item selection with selectIcon (0x0079bc70.., `ItemSelection.setReverseItems`), the vehicle HUD (hudName, hasTurretIcon, armour per cent and colour, speed, altitude, maxClimbSpeed, the seat dots from vehicleIconPos drawn by BfOccupiedVehicleNode 0x007e1520) and the map's vehicle icons (miniMapIcon/Inside, miniMapIconSize, showOnEnemyMinimap, alwaysShowOnMinimap, vehicleType filters, usePlayerIcon; 0x00781680, 0x00793b60, 0x0077dd70). Left: range finder, distance counter, scan progress, the Secondary* block, spottedIcon, miniMapReload/BackgroundIcon, miniMapInfo, typeIcon, useSelectionIcons. | M |
| 5 | **Titan interior and exterior part culling** | Inside a Titan, the exterior shell and parts aren't culled or switched as in retail; damaged exterior parts don't map. | `GeometryTemplate.isTitanInterior` (108), `isTitanExterior` (73), `titanExteriorPartIndex` (12) are stubs (396 / 275 / 28 calls over the 11 runs). `ObjectDrawer.isInsideTitanOverride` / `showAllTitanParts` are stubs. RendDX9's interior path 0x100c3880 / 0x10162070 is cited (LevelRenderer.cpp:2706), but only for the hemi map. **Now done:** the three template properties, `ObjectDrawer.*`, `renderer.indoor*CullDistance`; static (0x100bdb00), bundled (0x100c5030) and skinned (0x10162070) culling and the hull parts' LOD pairs (0x100baf40 / 0x100baf90 / 0x100bb470) in `TitanParts.h`; the camera's Titan by the rooms' boxes (the exe's ray 0x006f2b00 not reproduced). | S–M |
| 6 | **Underwater fog and water look** | Under water the view stays clear. The water's sun intensity is ignored. | `renderer.waterFogColor` and `waterFogStartEndAndBase` (38 each, one per level), `terrain.waterSunIntensity` (76) are stubs. 10 `terrain.water*` setters are uncalled stubs. "underwater" and "waterFog" occur nowhere in src. **Now done:** the water fog and SkyDomeUnderWater below the sea (0x100497a0 / 0x10049850 / 0x100fd2f0, `Fog.h`), `terrain.waterSunIntensity` in `water.frag` (0x100767e9's PointColor), 8 `terrain.water*` setters as the `renderer.water*` aliases they are (`waterQuality`, `waterNormalMap` still stubs). The level fog's end stays the 1400 m stand-in (its depth units unsettled). | S |
| 7 | **Scoreboard text colour** | Scoreboard rows draw black. | engine.md:4231-4234: the filler of the scoreboard data sources (HUD manager interface slot 0x214) wasn't found, so rows are black unless `setListTextColor` is called. **Status: done.** The filler is 0x007bb540 (slot 0x214 → 0x007c6d70 → the scoreboard's lists, 0x007b9aa0): dead players black, the squad green, friends blue, enemies red, the local and odd rows' backgrounds, font 1 (`menu::scoreboardStyle`). Left: the squad header rows (0x007ba570). | S |
| 8 | **Vehicle "spectacular" destruction effects** | Destroyed vehicles and aircraft lack the extra close and distant explosion sounds, wreck particles and burning (`e_sfx_exp_close`, `p_wreck_as_ag`, `e_sfx_burning_air_vehicle`...). | `ObjectTemplate.armor.addArmorEffectSpectacular`: 133 calls, a stub (1,265 over the 11 runs). `armor.addWreckArmorEffect` is a stub too. **Status: done** for vehicles (`GameWorld::armorEffectsAfterDamage`: addArmorEffect and addArmorEffectSpectacular, 0x006549a0/0x00654730/0x006525f0). Left: addWreckArmorEffect, hideChildren/destroyOnSpectacularDeath. | S |
| 9 | **Nametag icons and the 3D-map icons** | No 3D icons over objects, no selection, tracker or rank/veteran icons; tag bars partly. | `nametag.show3dIcon` (1,175), `selectable` (1,151), `3dMapIcon` (104), `selected3dMapIcon` (80), `showIconOnTracker` (80), `showInside/Outside`, `useAliveArmor` are unread. `nametags.setTexture` (55), `createIcon` (45), `createBar` (5) are stubs. engine.md:4297-4300: "Not done: ranks and veteran icons, the radio calls' icons". **Status (HUD agent):** `nametags.createIcon`/`setTexture`/`createBar` keep nametags.con's icons (0x10079004, 0x100790e4 → 0x1016b790; the ids of 0x10070e1a), and objects with `show3dIcon` draw icon 0x50 + `3dMapIcon` with showInside/showOutside, teamSpecific and the distance fade. Left: ranks and veteran icons on players (the renderer's icon passes aren't decoded), `selected3dMapIcon`/`selectable`, `showIconOnTracker`, `useAliveArmor`, the radio calls' icons, the inside-a-Titan camera state. | M |
| 10 | **In-game tutorial help** | The help manager's own 75 tutorial messages and the alt and low-ammo help keys never show. | engine.md:5311-5316 (0x00645d40, its timers not read); coverage gap 4 "Left". **Status: done** (`HelpPopups::kTutorials`/`tutorial`, engine.md "Warnings and help pop-ups"): the 83 messages of 0x00645d40 with their play-time limits and event flags, the 10 s tick of 0x00649d80, the conditions of 0x00646cb0 the rewrite keeps state for, 9 s pop-ups, the played list (GeneralSettings.setPlayedVOHelp), the alt help (altGuiIndex shown) and the low-ammo help (below 0.3 of a magazine), low armour at 0.3 of the maximum. Left: the event-only messages, the squad/commander tabs' menu states, the profile's play time. | S–M |
| 11 | **Bots' deviation through the soldier** | Bot shots spread uniformly instead of through the new soldier-deviation model. | Bots.cpp:4553-4556 still uses the stand-in "the rewrite's hand-held firearms have no deviation model", though SoldierDeviationComp now exists (engine.md:4565). It needs the AI deviation 0x00892b70 → soldier vt+0xf4. **Status: done.** (`GameWorld::setAiDeviation`: the SoldierDeviationComp's +0x44 and the SimpleDeviationComp's +0x28, 0x00680290/0x008a1a90; gunners' guns now stray by their SimpleDeviationComp). | S |
| 12 | **Particle and effect detail** | Flipbook speed, play-once, zoomed-mode hiding and effect bundles' random age and speed differ from retail. | Unread: `animationSpeed` (1,267), `animationPlayOnce` (1,233), `selfShadowIntensity` (1,233), `showInZoomedMode` (1,149), `noRotationFromParent` (1,049), EffectBundle `randomAgeFactor`/`speedFromPhysics` (439), `nearCullingEnable` (424), `isStaticSystem` (165), `*Range`/`*Used` emitter modifiers (76-78). Coverage gap 9 "Left". **Now done:** sprite flipbooks (animationSpeed / PlayOnce / RandomizedStartFrame, 0x10131f80 / 0x1013fdc0; sprites had drawn their whole atlas), showInZoomedMode (zoomed first person), noRotationFromParent (0x1013eff0), randomAgeFactor and speedFromPhysics (0x00619f90, 0x00618ae0); selfShadowIntensity read, no reader in RendDX9. Left: nearCullingEnable, isStaticSystem, the emitter modifiers. | S–M |
| 13 | **Projector lights and per-light shadow flags; spot-cone width** | Spotlights and searchlights don't project textures; lights ignore their shadow and self-light settings; spotlight cone width is guessed. | LightSource: `projectorTexture/Fov/Aspect/Near/Far` (93 each), `castsDynamicShadow` (258), `castsStaticShadow`, `selfShadows`, `objectShadows`, `softness` (94) are unread. engine.md:1610-1618 calls the cone mapping "still a guess". **Now settled:** RendDX9 never draws LightManager_r3x0.fx (handles fetched and released at 0x100a9a00, no technique set; no "SpotProjector" string), so retail draws no projector textures or per-light shadows. The cone's degrees-to-width is still not found (the exe's setters 0x00617790 / 0x006177c0 only clamp to 180). | M |
| 14 | **Static-mesh material detail** | Envmap reflection strength, static gloss, parallax and animated UVs are missing or constant. | `GeometryTemplate.setMaterialReflectionScale` (48), `setSpecularStaticGloss` (29; 0.15 used), `setMaterialParallaxHeightScale` (7), `nrOfAnimatedUVMatrix` (4) are stubs. Static meshes' ndetail/ncrack normal maps and parallax are "Left" in coverage gap 10. **Now:** the four setters are kept (bundled-only maps 0x100a25d0 / 0x100a26b0, parallax slot 0x38, `+0x110`) and bundled StaticGloss drawn from setSpecularStaticGloss. Left: the reflection scale (no bundled envmap), parallax and the static normal maps (no static tangents), animated UV matrices. | M |
| 15 | **Vehicle exit and camera behaviour** | The camera snaps on exit, exits are unrestricted, exit speed isn't applied; the camera's sway/bounce, ToggleCameraComp and colorTransform are missing. | `maintainCameraOnExit` (138), `maintainCameraOnExitToOtherVehicle` (84), `hasRestrictedExit` (135), `exitSpeedMod` (115), `dontClearTeamOnExit` (120), `takeOverInput` (93) are unread. Coverage gap 7 "Left": sway/bounce, ToggleCameraComp, colorTransform/saturation, insideViewInterpolate, spectator HUD strings. **Status: partly done.** hasRestrictedExit (0x005c7840), exitSpeedMod (0x004bb570), maintainCameraOnExit (0x005cea00). Left: maintainCameraOnExitToOtherVehicle, dontClearTeamOnExit, takeOverInput, ToggleCameraComp, colorTransform, insideViewInterpolate (the speed and hit shakes were done before). | S–M |
| 16 | `Object.disableChildren` | Placed objects such as `guard_watchtower_01_c` may spawn children that retail suppresses (check what those children are). | 144 calls in the level StaticObjects; a stub. | S |
| 17 | Decals and static-mesh renderer settings | Decal fade and time-to-live use defaults; static-mesh LOD skips are partial. | `DecalManager.*` 7/7 stubs (uncalled), `StaticMeshRenderer.*` 10/10, renderer +0x693 / +0x6a8..+0x6b0, filtering (coverage gap 22 "Left"). | S–M |
| 18 | Sound remainder | No occlusion, the old engine model, no burst samples or PCO ambients, no material radio. | Coverage gap 1 "Left". `listenerObstruction` (131) and `damagedAmbientSoundLimit` (163) unread; `Material.triggerRadio` / `MaterialManager.setRadioMessage` stubs (10 each). | M |
| 19 | Titan mode remainder | The gpm_ti shield kill and the combat-area HUD text. | Coverage gap 16 "Left". | S |
| 20 | Titan pods | Pod spawning paths: `spawn.spawnInPod`, `podSpawnVehicle`, `disablePodControls`. | Coverage gap 8 "Left"; `spawnInPod` unread. | S |
| 21 | Strategic objects and LookAt | The Titan consoles' icons and names; LookAt objects. | `strategicObject.*` 6 unread (30 calls); `nameToLookAt` / `offsetToLookAt` unread. | S |
| 22 | Projectile details | Penetration flags, active defense, MM end effect, detonation radius and depth. | `penetrate.allowSolid/LiquidPenetration` (106/63), `activeDefenseEnabled` (41), `detonation.detectionRadius` (76), `maxDepthForExplosion` (73), `useMMOnEndEffect` (67), `hitImpactModifier` (65) unread. engine.md:4880 "Not traced: the detonation component's 0.05 m MTYVehicle trigger test". | S–M |
| 23 | Commander map remainder | The commander rose's lists, paint filters on the map, the CommanderMiniMap node, the renderer value during the overhead view. | Coverage gap 5 "Left"; engine.md:5139-5141. | S–M |
| 24 | Demo camera | togglePlayerDemo, toggleCameraDemo, adjustDemoFov, playing camera files, message 9, timedemo. | `demo.*` 3 stubs; coverage gap 17 "Left". | S–M |
| 25 | Bots' strategy realism | Knowledge certainty and degeneration, the fire plan's ITAir clause, cullDistance, hearing values. | `AITemplatePlugIn.cullDistance` (21), `setSoundSphereRadius`/`setHearingProbability` (3) stubs; engine.md:7303-7395. | M |
| 26 | Joined-client and online-adjacent details | A joined client's takeable radio and silo "_N" suffix; the commander's scan and UAV ghost scopes; the rcon_feedback length; ServerSettings.con from the menu's server pages. | Coverage gaps 12, 13, 23, 24 "Left". | S each |
| 27 | Remaining GameLogic tunables | Probably the commander's radial menu level, and the drop, UAV and scan timings. | `GameLogic.radialLevelIndex` (30 calls; levels set e.g. 3), `supplyDropHeight`, `supplyDropNumSecsToLive`, `enableVoHelp` (1 each) stubs; `uavReloadTime`, `satelliteScanReloadTime`, `orbitalBombardmentHeight` uncalled stubs (defaults suffice). | S |

### 2b. Not decodable, online-only, or hardware (already documented)

- **Online back end beyond login:** stats, leaderboards, buddies, server logos, award grants, BFHQ series and the ranked overrides. FESL/EASTL (about 450 KB uncited) and GameSpy GP (72 KB) need OpenSpy services and an account (coverage gap 18; engine.md:5683, 8045-8049, 7985-7995). A full join of a vanilla server also needs GameSpy pre-authorisation (engine.md:3102-3106).
- **Dropped on purpose:** PunkBuster (0x008c5bc0, 55 KB), SecuROM, the firewall helper, IGA in-game ads (RendDX9 0x10057280; `GeometryTemplate.isAd`), and TextureAtlasBuilder.dll (tooling).
- **Hardware:** the Novint Falcon haptics (`IDFFalcon`, bf2142haptics.dll / NovintHFX.dll; coverage gap 21).
- **Settled as absent or unused in retail:**
  - glow materials (no caller in RendDX9);
  - Fog and ThermopticCamouflage techniques;
  - `envmapManager.*` (no level ships an .emi);
  - `Object.setLightSourceMask` (inert);
  - ExternTrace (uncalled ICamera slot);
  - `GeneralSettings.setViewIntroMovie` reader;
  - `treeSkyColor`, `shadowUpdateFactor`, `dropShadowClipHeightOffset`;
  - the friendly-fire victim factor's kit list (1 in practice);
  - map preview movies (none shipped);
  - the stuck-driver hand-off (0x008924f0 returns 0);
  - the original's first-person IK (none).
- **Not reproducible by nature:** the AI's real-time CPU budgets (engine.md:7305-7317).
- **Not in the original:** gamepad navigation of the Flash menus, and profile rename.
- **Editor, debug and tooling only** (about 300 of the 510 remaining stub rows):
  - `LightmapGeneration.*` (50);
  - `collisionManager` debug (14);
  - `AnimationSystem` editor methods (19);
  - `Console.get*` (13);
  - `Overgrowth` editor methods (16);
  - `Player.*` (11);
  - `heightmapcluster` (6);
  - `Editor`, `Profiler`, `NetworkableInfo` dumps (4);
  - `GeometryTemplate` lightmapper hints;
  - `RagDoll`/`Overgrowth`/`Vars`/`awards` rows read by their own parsers.

### 2c. "inferred" / "approximation" / "guess" and similar hedges in docs/engine.md that a further decompilation pass could settle

Lines explicitly marked inferred, approximate or a guess:

- engine.md:1611: the spotlight cone: "Still a guess" how `coneAngle1/2` degrees become the shaders' cosine-space width c. Where the conversion happens wasn't found (1616-1618). Still open; the light manager's own spot techniques are never drawn (gap 13).
- engine.md:4175: HUD bar fill directions (types 0 up, 2 from the right, 3 from the left) are "inferred".
- engine.md:4181: the HUD corner roots' positions are "approximate".
- engine.md:4297: name tags: "Guesses: the soldier's origin 1 m over its feet, the vehicle height (4 m; RendDX9 asks the render object), the bar's place."
- docs/coverage.md:35 (gap 11): the soldier deviation's misc part condition ("in the air, inferred").
- docs/roadmap.md:34 (row 5): "the emitter logic is inferred, not yet decompiled". This was stale and is fixed (row 5 cites the emitters).

Other open decodes engine.md marks as not traced, not found, unknown or a stand-in:

- 221: what turns a wreck's mesh over (instance slot 0x30) is unknown.
- 785: the collision response's stuck push (+0x11c) and the damage events' impact effects and decals are not modelled.
- 1196: the caller of the hit shake 0x00610450 wasn't traced; the rewrite uses the share of hit points the hit took.
- 1555: static meshes' LOD skip (0x100dd480 asks template +0xc8, unknown).
- 1690: per-object decal sets: the rewrite's rule is every body a projectile can hit.
- 2233: the tinnitus event's sender wasn't found; the rewrite uses its own radius rule.
- 2238: the view's tint colour (PostProduction slot 0x7c) is not decoded.
- 2379: the server's GameLogic slot 0xf4 wait in 0x0058d350 isn't identified (corpse countdown).
- 2837: the 2 ghost bits at +0x168 (0x1000; vtable 0x1a8/0x1ac): the setter wasn't found, so the meaning is open.
- 3257: ghost dirty masks and the scoping eye are stand-ins.
- 3345: how the exe carries an rcon_feedback text longer than one message.
- 3413: the loader that fills the kit list for the friendly-fire victim factor isn't found.
- 3554: the meaning of the soldier ghost's mask 0x40000 field (interface +0x18c, vt 0x90/0x94).
- 4233: the scoreboard data-source filler (HUD manager slot 0x214) wasn't found, so rows draw black.
- 4615: the lock-on component's object update is not decoded; the lock rule is the rewrite's.
- 4774: whether a downed soldier still holds a ParentTargetComp target (0x0062b750).
- 4834: the cloaked soldier is drawn hidden as a stand-in for the shimmer. This was stale and is fixed in engine.md.
- 4880: the detonation component's 0.05 m MTYVehicle trigger test, `ForceObjectHoverTest` and `drag`.
- 5038: the medic-means flag (kit +0x228 / +0x74 slot 0x38) setter wasn't traced; taken as a kit with an RTHeal gadget.
- 5141: the renderer value the exe sets during the overhead view (DAT_00a2020c slot 0x80) isn't decoded.
- 5300: what sends HUD event 0x2f (the first/second warning split) isn't traced.
- 5323: when the exe starts and stops the ability sounds isn't traced.
- 5760: the caller 0x005a3bd0 of the man-down branch; the enemy-shock team test is the rewrite's.
- 5853: the spawn's extra test (IAIPathfindingCourier slot 0xc, DAT_00b4bb94) isn't traced.
- 5894: offline squad invites are auto-accepted by bots (a stand-in).
- 5896, 6112: the squad and commander list columns are fitted to the scripts.
- 6063: the soldier's own radius in the supply crate's reach is taken as 0.
- 6067: which order kind the minimap draws with which icon isn't traced.
- 6332: the call site of 0x005eaca0 (material penetration exit) wasn't found.
- 6444: without the AI scripts, the shipped values are assumed (test-only).
- 6636: BF2142.exe's sender of the grenade warning event wasn't found; the rewrite checks the rounds in flight.
- 7164: the 20 s pass-over of a failed target (+0x30 writer), the unit-count scaling 0x100d2b30 and the Unit's capability flags.
- 7303-7395, "Not decoded, and the rewrite's stand-ins" (AI), each a decodable item:
  - path search slicing (0x100c6ee0);
  - bots not zooming;
  - the deviation stand-in (stale, see section 3);
  - the air-target clause (ITAir, slot 0x28);
  - the target's Mobile slot 0x78 speed (0x008a1960);
  - the human-attention stamp writer (0x100eb7f0);
  - see-through materials;
  - vehicle-gun magazines;
  - the wire-guided target (LPTargetComp);
  - the area guard's per-side spot and radius;
  - C4's moving-target guard;
  - the guided weapons' own fire plan (0x10078570);
  - +0x209's meaning;
  - the infantry-map reach test (Mobile per-map points);
  - the ITAir room test (0x1006c3e0);
  - knowledge certainty (0x100a9b90);
  - ammo in the capability factor;
  - whether a seated soldier counts on its own;
  - the revive water rule;
  - camouflage charge;
  - the idle alternate-key tap;
  - the commander's strike tool choice and UAV/scan;
  - the path follower's 2 m re-plan.
- 7985: the engine setter of the kit's special-ability icon list.

## 3. Regressions and inconsistencies

1. **Flaky test.** `Voip_LoopbackSession` fails in the full HEAD run (524 tests, 1 failed) at VoipTest.cpp:550, 551 and 560, just after `removePlayer`. Neither src/Voip nor the test changed since 8948ed8, and the old build fails it too. Alone it passed 4/8 times on 8948ed8 and 2/8 on HEAD. It is timing-dependent (threads, real time), not a code regression.
2. **The probe's mode caveat is out of date.** coverage.md:19 says no console methods register only in some modes. Now 158 register only with `--host` and 3 `demo.*` only with `--demo`. The start-up line "525 console methods stubbed" (main.cpp, non-host) counts those as stubs, so it overstates the gaps.
3. **Stale engine.md statements contradicted by code or by other sections:**
   - engine.md:837 "Gun heat isn't modelled yet", against engine.md:4892-4895 and Weapons.h:178-213.
   - engine.md:4181-4182 "The crosshair stays at the weapon's minimum deviation; soldier deviation isn't modelled yet", against engine.md:4565 (SoldierDeviationComp) and coverage gap 11.
   - engine.md:7319-7322, and the same comment at Bots.cpp:4553-4556: "the rewrite's hand-held firearms have no deviation model". The bots still use the uniform-spread stand-in (a code gap as well as a doc one; gap 11 above).
   - engine.md:3331-3333 "reservedSlots … Kept, not yet used to admit a player to a full server", against engine.md:3391-3400, HostedGame.cpp:459 and the test `ServerAdmin_ReservedSlotAdmitsToAFullServer`.
   - engine.md:1208-1211 "the renderer doesn't light by [the interior sun] yet", against LevelRenderer.cpp:3364-3367 (`InsideSun`) and coverage gap 10 "the seat camera's interior sun on the first-person pass".
   - engine.md:4834 "drawn hidden as a stand-in for the shimmer", while the cloak refraction is now drawn (`LevelRenderer::drawCamouflage`, LevelRendererPost.cpp:649; coverage gap 3).
   - **Fixed:** all six lines above now describe the code (gun heat, the crosshair's soldier deviation, reserved slots, the interior sun, the cloak's refraction; the bots' deviation now goes through the soldier, `GameWorld::setAiDeviation`).
4. **Stale roadmap.md:**
   - Row 5 (line 34): "the emitter logic is inferred, not yet decompiled", against coverage gap 9 (RendDX9's emitters 0x1013fb30 / 0x101407c0...).
   - Row 8 (line 37): "`bf2142` still has to feed c_PIAction to `SoldierInput::jump`", against Viewer.cpp:559.
   - **Fixed:** both rows.
5. **coverage.md status lines that overstate:**
   - Gap 5 says "all 27 HudManager stubs". `HudManager.addFavouriteServer` and `enableSayPlayerChatBox` are still stubs (29/31; neither is script-called). **Fixed:** both are rewritten (0x00760987 → 0x00449180; 0x0075ff8e → 0x007d95b0), as are `IngameHelp.showIngameHelp`'s target (slot 0x98) through the help pop-ups and `ItemSelection.setReverseItems`.
   - Gap 10 says "`HemiMapManager.*` 5/5 registered". `makeHemiMap` is still a stub (an editor method).
   - Gap 22 lists `undergrowth.simpleShaderEnable` as done. It is applied from the terrain quality (LevelRenderer.cpp:1503), but the console method is still a stub, so a script or console setting is ignored.
   - Gap 23's "MenuTeamManager … their readers have no caller" is fine. The Python table still says "47 `host.*`" where 50 names are used, counting Admin/.
   - **Fixed:** gaps 5, 10, 22 and 23 and the mode caveat (line 19) now say so; `undergrowth.simpleShaderEnable` is a real console property.
6. **Renderer features without unit tests.** `bf2142_tests` doesn't link src/Render or src/Scene (tests/CMakeLists.txt:2). These have no automated test:
   - dynamic shadows, hemi and Titan hemi maps, sky clouds and flare;
   - normal maps and specular, the texture-class mip skips and lighting level;
   - the overhead commander view, HUD blend modes and pivots.

   PostProductionTest covers only the header maths, and EffectsTest covers the particle side. Also untested: the friendly-fire scaling (0x005b7290; no test references it).

   **Now:** the shadow region and map size, the BC3 hemi alpha, the hemi maps' texels and the Titan maps' sample, HeightOverTerrain, the clouds' offsets, the normal maps' kinds (`SceneMath.h`), the overhead camera's place (`OverheadView.h`), the mip skips and lighting level, the Titan culling (`TitanParts.h`) and the view's fog (`Fog.h`) are GPU-free and tested (`tests/RenderSceneTest.cpp`, 8 tests). The draw passes themselves still have none.
7. **Build artefacts in git.** `src/Python/__pycache__/py2compat.cpython-314.pyc` and `-315.pyc` are tracked.
8. **Unread metric false positives.** `sway.*` (22) and other composed-key reads (see the caveat above), so the "unread" counts here and in the first audit's Table D overstate a little. No ObjectTemplate row went from read or impl back to unread or stub between the two commits, and no console row regressed from impl to stub.
