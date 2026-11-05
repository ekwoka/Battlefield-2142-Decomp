# BF2142 rewrite: seventh coverage audit (2026-11-05, HEAD 455d4ec)

This audit compares HEAD 455d4ec with 2041898, the commit coverage-6
measured. Seven commits lie between them (b5bb387, b133966, 8575796,
64f8fe9, 8518873, 64b4bdd, 455d4ec). b5bb387 is coverage-6 itself. The other six are the passes
coverage-6's status blocks describe:
- sticky projectiles, radar, lock tones, seat zoom;
- the three missing-runs passes over BF2142.exe, RendDX9 and AIDLL;
- the bot squads' formation;
- SimpleShadowmap, demo camera files, beacon pods and closeToIndication.

## How this was measured

The method and calibration are coverage-6's, so the figures can be compared:
- the same scripts (`tools/coverage/`);
- the same corpus (`calls.json`: 6,702 files, 816,424 calls);
- the same decompilation features and the same factors (EXE 0.84, RendDX9 0.69, AIDLL 0.48).

- **after:** HEAD 455d4ec, the main checkout, measured with `tools/coverage/run_all.sh --before <2041898's outputs>`.
  - Its only uncommitted change is `.mcp.json`, which no script reads.
  - The release build was stale and was brought up to date first (`ninja -C build/release src/Main/bf2142` rebuilt 155 steps).
- **before:** 2041898, coverage-6's own HEAD outputs.
  - The probe, rows, level-run and citation scripts haven't changed since b5bb387 (`git diff b5bb387..HEAD -- tools/coverage` touches only `uncited.py`), so those outputs are reused as they were.
  - `uncited.py` did change. In 64b4bdd its RendDX9 ranges grew: the IGA (a) range now ends at `0x100638e4` rather than `0x1005fa50`, and RoadEditable `0x100ff130`..`0x1010a6a0` is now (b).
  - So the classifier was run again on a source-only worktree of 2041898 (`git worktree add --detach`, removed afterwards), with HEAD's `uncited.py` and coverage-6's citation files.
  - This moves the before column's RendDX9 away from coverage-6's printed figures: needed 1.29 → 1.24 MB, calibrated 72.0 → 72.2%. The EXE and AIDLL are unchanged (82.7%, 69.5%).

No window was opened:
- The probe and the level runs used `--simulate` (one probe mode used `--screenshot`) with `--nosound --no-movies`.
- The host probe used `--host --dedicated` without `--online`.
- Nothing went beyond loopback.

The unit tests ran with `BF2142_GAME_DIR` set to the checkout's `source/Battlefield 2142` (`build/native` was current).

**The headline figures leave out the audit reports** (`cite.py --no-audit-docs`, `uncited_noaudit`), as coverage-6's did. The coverage-3 rule (audit docs counted) is given beside them for the series. The audit docs' share has grown a lot this time (section 1a), so the two rules now differ by 2 to 8 points.

**Tools:** nothing was broken and nothing was changed. Every script ran clean. Two measurement artefacts are noted in section 5:
- AIDLL citations in `src/Game/Bots*.cpp` are dropped as ambiguous.
- The no-audit classifier doesn't see uncited.md's hand classifications.

`tools/coverage/README.md` still lists the audits only up to coverage-6.

## 1. Headline numbers, before → after

| Metric | 2041898 (coverage-6) | **HEAD 455d4ec** | Change |
|---|---|---|---|
| Probe, distinct names: impl / stub (union of `--simulate`, `--host`, `--screenshot`) | 1,128 / 1,560 | **1,134 / 1,554** | +6: `AITemplatePlugIn.setUseNoPathfindingToGetToObject` / `vehicleNumber`, `GameLogic.defaultTimeToNextAIWave`, `Object.notInAI`, `ObjectTemplate.setCheckObstruction`, `weaponTemplate.isAntiAirWeapon` |
| … `--simulate` alone | 970 | 976 | 158 names still answer only when hosting; `--screenshot` adds none |
| Console rows (2,837): impl / stub | 1,137 / 381 | **1,143 / 375** | +6 / −6 |
| … ObjectTemplate stored-read / stored-unread | 924 / 395 | **974 / 345** | +50 / −50. No row went from read to unread. |
| Unread template property names (lower bound) | 368 (291) | **322 (249)** | −46 (−42) |
| BF2142.exe rows impl / stub; RendDX9 | 874 / 256; 263 / 125 | 880 / 250; 263 / 125 | EXE +6 |
| Start-up "console methods stubbed": EXE / RendDX9 | 507 / 125 | 501 / 125 | still includes the 158 host-only methods |
| Stub rows called by shipped scripts / calls | 36 / 23,262 | **31 / 23,244** | 6,828 calls without `Object.setLightSourceMask` |
| Unread rows called / calls | 230 / 32,785 | **182 / 32,102** | unique names 213 / 32,287 → 169 / 31,693 |
| Level runs: "N calls to M unimplemented methods" | 5,643–6,560 / 24–26 | **5,639–6,556 / 22–24** | −4 calls and −2 methods in every run (setCheckObstruction, defaultTimeToNextAIWave) |
| Python tracebacks (11 runs) / "Unknown object or method!" per run | 0 / 6,689–7,599 | 0 / 6,689–7,599 | identical |
| Unit tests (`BF2142_GAME_DIR` set) | 738, 0 failing | **804, 0 failing** | +66 |
| Hedge-note hits (`hedges.py`) | 216 (engine.md 113, src 103) | **237 (120, 117)** | By text, 16 left and 37 arrived (section 3). |

The 46 template names that became read are the gameplay rows of coverage-6's status blocks:
- **Sticky and gadget rounds:** `collision.maxStickAngle` / `stickToTerrain` / `stickToStaticObjects` / `stickToVehicles`, `detonateDelay`, `dudMaterial`.
- **Mines:** `detonation.allowFriendlyFireDetonation` / `triggerType` / `radiusDetonateWithAirVehicles`.
- **Guided rounds and targets:** `follow.followTime` / `targetActivation`, `overrideArmingSpeed`, `switchPlayerOnLock`, `disableWhenEmptyVehicle(Delay)`, `disableWhenWreck`.
- **Radar and spotting:** the `ability.radar*` / `showPlayers` / `showVehicles`, `radio.spottedMessage`, `vehicleHud.canBeSpotted`.
- **Zoom:** fine tuning (`zoom.useZoomFineTuning` / `zoomFineTuneSteps` / `zoomFineTuningNeedsUnlock`, `ability.hasZoomUpgradeAbility`), `zoom.zoomWhenBusy`, `allowZoomOnHeat`.
- **Weapons and seats:** `fire.showWeaponAfterReloadDelay`, `ammo.magLinkWeapon` / `allowReloadWithFullMag` / `allowReplenish`, `soundFilter`, `neverDrawAs1p`.
- **HUD:** `vehicleHud.useChildArmor` / `enablePostProcessingOnGuiIndex`, `weaponHud.altCrosshairIcon` / `firstLockSound` / `secondLockSound`, `radio.closeToIndicationDist` / `Type` / `indicationDistance2D`.
- **Spawning and rendering:** `spawn.spawnInPod` / `spawnVehicle`, `decalNormalMapName`, `isStatic`.

The full list is in `delta.md`.

### 1a. Address citations (start addresses; src/ and docs/, tests excluded)

The headline leaves out the audit docs (`--no-audit-docs`):

| Module | functions / bytes | 2041898, without appendix | **HEAD, without appendix** | with appendix | coverage-3 rule (audit docs counted), without appendix; with |
|---|---|---|---|---|---|
| BF2142.exe | 35,589 / 4,794,004 | 3,034 fns, 1,258,213 B (26.25%) | **3,299 fns, 1,339,524 B (27.94%)** | 3,934 (28.43%) → **4,199, 1,444,136 B (30.12%)** | 3,198 (27.62%) → **3,608, 1,439,735 B (30.03%)**; 29.80 → **32.20%** |
| RendDX9_ori.dll | 10,810 / 1,677,614 | 439, 286,720 B (17.09%) | **1,263, 429,401 B (25.60%)** | same | 529 (20.36%) → **1,496, 557,909 B (33.26%)** |
| AIDLL.dll | 6,080 / 1,006,188 | 1,087, 387,873 B (38.55%) | **1,718, 519,362 B (51.62%)** | same | 1,095 (38.67%) → **1,893, 581,060 B (57.75%)** |
| SwiffPlayer.dll | 5,756 / 936,991 | 179, 127,847 B (13.64%) | **180, 127,973 B (13.66%)** | same | 180 → 185, 128,409 B (13.70%) |
| BF2VoipServer.dll | 377 / 38,482 | 34, 10,976 B (28.5%) | 34 (28.5%) | same | same |
| BF2142Audio.dll | 1,702 / 206,940 | 0 | 0 | 0 | 2 (only through the audit docs) |
| Memory.dll / TextureAtlasBuilder.dll | | 0 | 0 | 0 | 0 |

- **Gain without the audit docs:**
  - EXE +265 functions / +81.3 KB (+1.70 points);
  - RendDX9 +824 / +142.7 KB (+8.51);
  - AIDLL +631 / +131.5 KB (+13.07);
  - SwiffPlayer +1.

  Most of the RendDX9 and AIDLL gain cites code that was already rewritten rather than new code:
  - RendDX9: its console rows' constructors and setters (LightSettings.cpp, PostProductionSettings.cpp, EffectSettings.cpp, OvergrowthSettings.cpp);
  - AIDLL: its runs cited at the rewrite's bots (src/AI, src/Game/Bots*).

  The raw (d) figures in 1b show how much is new.
- **The audit docs' share** (the coverage-3 rule minus the headline): EXE 164 → 309 functions (65.7 → 100.2 KB, 2.1 points), RendDX9 90 → 233 (54.8 → 128.5 KB, 7.7 points), AIDLL 8 → 175 (61.7 KB, 6.1 points). This audit trail is the reason it grew: coverage-6's run tables and uncited.md's classification tables list each run's start address, cited or not. Read the coverage-3 rule as inflated by that from now on.
- **The appendix** is almost unchanged: 900 → 896 EXE functions only it cites, 104,103 B (+2.2 points raw). It changes no effective figure.
- **With tests/:** EXE 3,657 / 1,449,362 B (30.23%), RendDX9 1,497, AIDLL 1,894 (coverage-3 rule).
- **With `--headings`:** RendDX9 539 (20.66%) → 1,513 / 565,304 B (33.70%), AIDLL 1,125 (39.08%) → 1,937 / 591,125 B (58.75%).
- **Ambiguous DLL addresses dropped:** 153 → 357 (coverage-3 rule), 128 → 251 (no audit docs), 89 → 254 (headings). Most are AIDLL addresses in `src/Game/Bots*.cpp` (section 5, item 1).
- **Uncited runs, no audit docs:**
  - EXE (≥ 30 KB): 11 / 0.76 → **11 / 0.75 MB** with the appendix; 13 → 12 runs without it. The template setters' run `0x004e886d` (34.8 KB) is gone; `0x0046c880` shrank from 47.2 to 35.9 KB.
  - RendDX9 (≥ 20 KB): 19 / 0.63 → **15 / 0.54 MB**. Gone: `0x10004930` (screenshots, DxDiag), `0x100e6de0` (the decal renderer), `0x1012b3f0` (non-screen-aligned particles), `0x100f7b80` (the sky dome).
  - AIDLL (≥ 20 KB): 2 / 0.06 → **1 / 0.02 MB**. `0x10044aa0` (containers, 33 KB) is gone.
- **Uncited runs, coverage-3 rule:** EXE 8 / 0.55 → 7 / 0.51 MB; RendDX9 13 / 0.42 → 4 / 0.17 MB; AIDLL 2 / 0.05 → 0.

### 1b. Effective coverage (the uncited classifier, `uncited.py`)

The definitions are coverage-4's:
- **Needed** is the total minus (a) third-party and (b) dev-only code.
- **Lower** is cited plus string- or console-evidenced (c).
- **Calibrated** adds propagated (c) × the factor and splits "?" pro rata.
- **Upper** counts all (c) and all "?".

**Headline: without the audit docs** (`uncited_noaudit`, appendix counted). The before column is 2041898 re-classified with HEAD's `uncited.py` (see "How this was measured"):

| Module | Needed | Cited, of needed | Lower | **Calibrated** | Upper | (d) calibrated | Raw (d) |
|---|---|---|---|---|---|---|---|
| BF2142.exe | 3.68 → 3.69 MB | 37.0 → 39.1% | 51.7 → **53.9%** | 82.7 → **84.3%** | 88.6 → 90.1% | 0.637 → **0.578 MB** | 408 → **355 KB** |
| RendDX9_ori.dll | 1.24 MB | 23.1 → 34.6% | 38.0 → **42.8%** | 72.2 → **76.5%** | 87.3 → 91.3% | 0.345 → **0.293 MB** | 153 → **105 KB** |
| AIDLL.dll | 0.87 → 0.88 MB | 44.3 → 59.2% | 46.3 → **60.6%** | 69.5 → **77.7%** | 94.3 → 95.8% | 0.266 → **0.196 MB** | 48 → **35 KB** |
| All three | 5.80 → 5.81 MB | 35.1 → **41.2%** | 47.9 → **52.5%** | 78.5 → **81.6%** | | 1.249 → **1.067 MB** | 609 → **495 KB** |

- **How to read the gain.**
  - Citing code that was already rewritten turns propagated (c), which is discounted by the factor, into fully counted "cited". So part of every calibrated gain is confirmation, not new code. That part is largest in AIDLL (factor 0.48: +8.2 points).
  - Raw (d) and the upper bound don't move when (c) code is cited, so they are the best signs of new or newly settled work: raw (d) is down 53 KB in the EXE, 48 KB in RendDX9 and 13 KB in AIDLL (114 KB in all), and the upper bounds are up 1.5, 4.0 and 1.5 points.
- **The coverage-3 rule** (`uncited_with`), for the series:
  - EXE 53.1 / **84.1** / 90.0 → 55.9 / **86.1** / 91.8%, (d) 0.588 → 0.515 MB, raw 360 → 295 KB;
  - RendDX9 39.8 / **74.0** → 49.3 / **81.6%**, 0.326 → 0.233 MB, raw 132 → 48 KB;
  - AIDLL 46.5 / **70.0** → 67.0 / **82.1%**, 0.262 → 0.157 MB, raw 40 → 13 KB;
  - all three 49.2 / **79.8** → 56.1 / **84.5%**, 1.18 → 0.90 MB.

  Because the audit docs' share grew, this rule is now 1.8 (EXE) to 5.1 (RendDX9) points above the headline.
- **Without the appendix** the effective figures are the same. The appendix's addresses move from "cited" to console-evidenced (c): EXE c ev 515 → 616 KB.
- **With `--headings`:** RendDX9 40.2 / 74.4 → 49.8 / 82.1%, AIDLL 46.9 / 70.4 → 68.1 / 82.8%, all three 49.4 / 79.9 → 56.4 / 84.7%.
- **This classifier's own propagation precision** (`uncited_noaudit_recal`):
  - factors EXE 0.45 → 0.49, RendDX9 0.71 → 0.74, AIDLL 0.42 → 0.37;
  - calibrated: EXE 69.0 → 72.2%, RendDX9 73.1 → 78.8%, AIDLL 66.6 → 73.8%, all three 69.5 → 73.9%.

  The reused EXE factor (0.84) is still about 1.7 times this classifier's. The absolute EXE figure is soft; the before/after is not.
- **SwiffPlayer.dll** isn't in the classifier. The rewrite's SWF player (src/UI) is a new implementation of the format, not a port of the DLL's code, so its 13.7% cited is the natives' and mapped calls' share and says little about completeness. No uncited.md-style audit of it exists.

## 2. The largest remaining unrewritten or unclassified runs

### 2a. Missing ((d) runs, no audit docs, ≥ 2 KB)

The counts are runs / KB:

| Module | 2041898 | **HEAD** | Gone since |
|---|---|---|---|
| BF2142.exe | 20 / 100.7 | **16 / 81.1** | `0x008ae4e8` (6.9 KB, the RagDoll editor rows); `0x0042ec60` (5.7 KB, the network event names); `0x00504be6` (4.4 KB, the component class names); `0x004f63d8` (2.9 KB, `follow.*`, implemented); `0x00420bdc` (2.4 KB); `0x004fd04b` (re-bounded as `0x004fd1c3`) |
| RendDX9_ori.dll | 21 / 78.3 | **12 / 38.1** | the GeometryOptimizer runs (`0x100f96b0`, `0x10170370`, `0x100f7b80`, `0x1016f370`, `0x100f6f90`, now (b) or cited), `0x100f62a0`, `0x1002f3d0` (re-bounded as `0x1002f780`), and the cited `0x100b1bf0`, `0x10121790`, `0x1008fb30` |
| AIDLL.dll | 5 / 20.4 | **4 / 17.6** | `0x10067e00` (GotoWaypointsBoat: boats, inert) |

**Every run left is already classified** in uncited.md or in coverage-6's run tables. The no-audit classifier doesn't read those files (section 5, item 2). Not one of these runs is behaviour that the rewrite lacks and a player would see:

| # | Run | Size | What it is | Where it was classified |
|---|---|---|---|---|
| 1 | EXE `0x0047f5e0`..`0x00483b50` | 13.4 KB | SwiffHost's BFHQ stats fields (`dstrk`, `wdths-%d`, …) | online-only (uncited.md, missing-runs pass) |
| 2 | EXE `0x006e8570`..`0x006ebdbc` | 12.1 KB | the object grid's tree build ("Recursing again…"), IObject_ptrproxy | replaced by design |
| 3 | EXE `0x00683e40`..`0x00686e89` | 10.2 KB | IDKey_ / IDButton_ name tables | replaced by design (ControlMap.cpp composes them) |
| 4 | EXE `0x0066b175`..`0x0066cee1` | 6.7 KB | `Console.getArgumentTypes` / `getEnumName` reflection | dev |
| 5 | RendDX9 `0x100ce6c0`..`0x100cffe1` | 6.4 KB | TerrainEditable's `terraindata.raw` writer | (b) editor |
| 6 | EXE `0x006ee282`..`0x006ef9bd` | 5.9 KB | `Object.initGrid` / `isVisible` … rows | editor |
| 7 | EXE `0x0070cb00`..`0x0070e120` | 5.6 KB | the Logitech G15 LCD applet's pages | hardware |
| 8 | AIDLL `0x1002db50`..`0x1002ef2d` | 5.1 KB | the bot debug printer ("HasZoom: ", "Behaviour: ") | (b) |
| 9 | EXE `0x004196a4`..`0x0041aa21` | 5.0 KB | `SquadManager.listSquads` / `listSquadMembers` | dev |
| 10 | AIDLL `0x10052fe0`..`0x10054688` | 5.0 KB | IDPManager's text demo and its neighbours | (b) |
| 11 | RendDX9 `0x100ca150`..`0x100cb3e4` | 4.3 KB | nvdxt / tgasplit calls | (b) |
| 12 | AIDLL `0x1002eff0`..`0x10030019` | 4.1 KB | the SAI debug printer ("Team1:", "[CP]") | (b) |
| 13 | RendDX9 `0x1002bb90`..`0x1002cb8d` | 4.1 KB | renderer statistics ("fps: %f") | (b) |
| 14 | RendDX9 `0x10158520`..`0x10159d29` | 4.0 KB | the terrain editor's .dds splitting | (b) |
| 15 | AIDLL `0x100c4a00`..`0x100c5a35` | 3.8 KB | small accessors and the IDPManager (debug drawer) singletons | (a) / (b) plumbing (uncited.md, AIDLL pass) |
| 16 | RendDX9 `0x1001af30`..`0x1001be55` | 3.7 KB | the .fx preprocessor | replaced by design |
| 17 | EXE `0x006f0d36`, `0x006efbdb`, `0x006ec29f` | 3.6 / 3.6 / 3.3 KB | `Player.create`, `ObjectTemplate.listTemplateClasses` / `saveInSeparateFile`, `Object.direction` | editor |
| 18 | EXE `0x004f64fb`..`0x004f6f54` | 2.6 KB | DestroyableObject's respawnDelay, usedByAi … (re-bounded, not new) | unset: no shipped template sets them |
| 19 | RendDX9 `0x10095810`, `0x100793e1` | 2.6 / 2.6 KB | the overgrowth's vertex streams; the `nametags.*` tuning rows | replaced; (d) low, unbound and uncalled |
| 20 | EXE `0x00525958`, `0x004fd1c3`, `0x0052930c` | 2.3 / 2.2 / 2.1 KB | cameraSway*; regulateYaw / Pitch; castsStaticShadow and the light flags | unset; unset; rendering flags settled in engine.md |
| 21 | RendDX9 `0x100118d0`, `0x10179bf0`, `0x10003b10`, `0x10009cc0`, `0x1002f780` | 2.1–2.4 KB each | VB dump, lightmap samples, frame capture, display modes, profiler containers | (b) / replaced |
| 22 | EXE `0x00436530`, `0x006af640` | 2.2 / 2.1 KB | the G15 applet's start-up; the object grid's triangle-box test | hardware; replaced |

Raw (d) that remains (EXE 355 KB, RendDX9 105 KB, AIDLL 35 KB) is now mostly in runs under 2 KB (77% of the EXE's, 64% of RendDX9's, 50% of AIDLL's):
- string-less container code propagated from a neighbour;
- one-row executors of stored-unread template rows.

### 2b. Largest uncited runs (no audit docs, without the appendix), with the classifier's makeup

The KB values are the classifier's labels inside the run:
- a: third-party;
- b: dev;
- c: evidenced;
- c-p: propagated present;
- ?: unsure;
- d: missing.

EXE (≥ 30 KB; 12 runs):

| Run | Size | Makeup (KB) | What it is |
|---|---|---|---|
| `0x00840340` | 177.7 KB | a 177 | FESL / EASTL |
| `0x008155c0` | 131.7 KB | a 158 | the FESL game browser |
| `0x007380a0` | 81.8 KB | a 62, c-p 23 | GameSpy SDK and HTTP |
| `0x008025b0` | 70.8 KB | a 75 | the FESL account fields |
| `0x007c9090` | 53.6 KB | c-p 28, ? 20 | the HUD's font, QuadDrawer and Ref2System (substituted by src/Hud) |
| `0x0072a180` | 50.8 KB | a 53 | GameSpy GP |
| `0x00760a26` | 50.0 KB | c 30, d 8 | the HUD map node's console rows; cited only by engine.md's appendix |
| `0x008c970a` | 40.8 KB | a 133, c 71 | static init and the networkable registrations |
| `0x00696f80` | 39.0 KB | c-p 25, a 11 | file paths and the app-data folder |
| `0x00870c90` | 37.8 KB | c-p 22, d-p 10, d 9 | dice::meme::Effect's shader-state names |
| `0x0046c880` | 35.9 KB (47.2) | c-p 23, a 9 | the command-line, profile and settings managers |
| `0x0065d840` | 30.7 KB | b 29 | the profiler's XML |

RendDX9 (≥ 20 KB; 15 runs):

| Run | Size | Makeup (KB) | What it is |
|---|---|---|---|
| `0x101885b0` | 89 KB | b 85 | debug drawers, the ported menu shader |
| `0x10053c80` | 60 KB | a 63 | IGA |
| `0x101543a0` | 49 KB | c-p 30, b 10 | the terrain editor |
| `0x1016d0f0` | 40 KB | c-p 19, a 11, ? 6 | name tags and the specular LUT |
| `0x10101310` | 37 KB | b 35 | roads (RoadEditable, now (b)) |
| `0x1019fab0` | 31 KB | a 17, b 16 | |
| `0x1001ddf0` | 28.5 KB | c-p 18, c 6 | the effect loader and its state cache |
| `0x10179060` | 28 KB (32) | c-p 18 | SwiffRender's fonts and languages |
| `0x10041b10` | 26 KB | c-p 10, a 7, c 6 | shader statistics |
| `0x100106b0` | 26 KB | c-p 15, a 6 | display modes |
| `0x1002eb90` | 26 KB | c-p 8, c 7, d-p 6 | the shader defines |
| `0x10092990` | 25 KB | c-p 16 | the rest of the overgrowth |
| `0x100c6570` | 25 KB | c-p 11, d-p 8 | the bundled mesh's shadow map |
| `0x100df060` | 24 KB | c-p 19, a 7 | |
| `0x100cd220` | 23 KB | b 7, c 6, d-p 5 | TerrainEditable |

AIDLL (≥ 20 KB): `0x1002ae90`, 22 KB (c-p 11, d 7): the bot debug printers, (b) in uncited.md.

## 3. What remains to call the rewrite complete

The measurable gaps are now small:
- The decodable gaps of coverage-6's §2 are all done or settled, according to its status blocks.
- The probe's 1,554 stub names are mostly editor, lightmapper, debug and online rows.
- Every (d) run of 2 KB or more is classified.

What is left splits as follows. **S** = days, **M** = 1–3 weeks.

### 3a. Behaviour gaps a player could notice

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| P1 | **Bot squads' effectiveness, unverified against the original** | Squad bots fight less: on Cerbere_Landing gpm_sp 16, 13.3 kills a round with the formation as decoded, against 19.4 when every member heads for the order. | engine.md:12295–12306. The formation matches the binary as decoded (coverage-6, AIDLL bots pass). Not decoded: the flank spots (`0x1005e820`, a stand-in), the danger list's writer (none found), SAI step 1's strength sums (`0x1009ee00`), bot slot 8 (`0x10098200`). The stuck vehicles on Cerbere are the original's too (engine.md:11626–11657). A side-by-side with the retail game is the only way to settle it. | M (verification) |
| P2 | **The mechs' seats on a joined client** | A joined client's view of the gunners on another player's mech: they don't ride the mech's bone parts. | engine.md:1474: a bone part's mount isn't on the client. | S–M |
| P3 | **Repair events to Python** | Repairing a Titan console, core or gun, or an AA / ground cannon, raises no script event. Any score or message the scripts give for it is missing. | `python.onRepairEvent` (117 calls, 43 templates, values 27–55; comp 0xc4bb, executor `0x00551983`); no reader in src, none traced; not in engine.md (coverage-6 L10, still open). | S |
| P4 | **The commander screen's mini-map tab** | The commander's mini-map tab (CommanderMapTabSelected) is empty. | `dice::meme::Bf2MiniMenuMapNode` (vtable `0x009bb688`, factory `0x007e8fb0`) decoded, not rewritten (uncited.md, second pass). Hud.cpp keeps an undrawn node. | S |
| P5 | **Mech effect events and the per-instance particle time** | The mechs' animation-driven effects (engine.md says "the vehicles' (the mechs') effect events"), and systems with a time override, don't play as in the original. | engine.md:1742 | S |
| P6 | **Commander and voting remainder** | The AI commander doesn't answer bot squad leaders. A kick vote can't be started from the scoreboard, and the host's own vote lines aren't shown. | engine.md:10441; uncited.md (d) #4 "Left" | S |
| P7 | **Small HUD and seat items** | Several small items, below. | Below. | S each |
| P8 | **Non-English text** | Unverified: whether the non-English fonts and locales (RendDX9 `0x1017af30`; SwiffRender's font run `0x10179060`, 28 KB, uncited) render. | uncited.md (d) #10 has no status. The language table is "replaced by design" (second pass). | S–M |

P7's items:
- the mech's foot animation on the HUD (engine.md:7166, which element is still unidentified);
- the pod's input mask (spawn point template `+0x95` → PCO slot `0x178`, engine.md:10007);
- picking up a deployable on another object within 9 m (engine.md:8663);
- the static decals' second list (engine.md:2706);
- the bots' kit pool's slot-39 flag (engine.md:12438);
- the man-down camera offsets (`manDownCameraOffsetInDof` / `InUp`, 8 calls, not traced);
- `groundContactVolume` (20).

### 3b. Bookkeeping and plumbing (no visible behaviour)

1. **Unimplemented console rows the scripts call.** There are 31 stub rows with 23,244 calls, and they produce the level runs' "22–24 unimplemented methods". All are inert or another tool's:
   - `Object.setLightSourceMask` (16,416): read by nothing that draws (engine.md:2456).
   - The lightmapper's GeometryTemplate rows: `compressVertexData`, `maxTextureRepeat`, `lightmapOverride`, `doNotGenerateLightmaps`, `setLightmapSizeAll`, `isAd`, `boundingRadiusModifier`.
   - The RagDoll rows: the rewrite parses ragdolls itself.
   - `heightmap.setModified` (an editor row), `awards.setData` (126; the awards were the stats server's, engine.md:13336).
   - `undergrowth.load`, `texturemanager.customTextureSuffix`, `terrain.init`, `rainManager.cellDisplay`, `Console.allowMultipleFileLoad`.
   - Still to look at: `ObjectTemplate.addVehicleType` (24), `ObjectTemplate.setIfTitanChangeToWeapon` (2), `weaponTemplate.shouldRearm` / `fireRate` (5 / 2), `Material.pythonEvent` (1), `GameLogic.createObject` (1).

   Binding the settled ones as accepted no-ops would empty the level runs' stub lists. That is cosmetic, but it makes real regressions visible.
2. **Unread template rows.** There are 322 names, 182 of them called (32,102 calls). Most are settled in engine.md as read by nothing in 1.51, or are editor flags: `isNotSaveable`, `saveInSeparateFile` and `createdInEditor` alone are 25,000 of the calls. A classification pass is needed for the called names that neither engine.md nor uncited.md mentions:
   - `createNotInGrid` (241), `python.onRepairEvent` (117, P3), `controlsCategory` (29), `projectileSubGeom` (20);
   - `otherCollisionLod` (18), `isSleeping` (13), `centerOfCollisionOffset` (11), `blockInputOnRotBundle` (10), `weaponHud.crosshairIconSize` (10);
   - the four `ability.networkableHud*` (8 each), `ammo.forceSoundPostReloadOnReload` (8), `seek.directionBonus` (7);
   - `armor.wreckDelay` (5), `autoExitTimeDelay` (4), `crewKitIndex`, `loseControlWhenEnemyClose` (3), and smaller ones.

   Some of these may turn out to be behaviour, which would move them to 3a. The rows tool still misses the `kInputs` keys (Effects.cpp; 3 names, 8 calls), as coverage-6 noted.
3. **Citations of present code.** The largest (c) blocks without a citation, by size:
   - EXE:
     - the HUD's font, QuadDrawer and Ref2System (`0x007c9090`, 54 KB);
     - the HUD map node's rows (`0x00760a26`, 50 KB, cited only by the appendix);
     - file paths (`0x00696f80`, 39 KB);
     - dice::meme::Effect (`0x00870c90`, 38 KB);
     - the settings managers (`0x0046c880`, 36 KB).
   - RendDX9:
     - the terrain editor's runtime half (`0x101543a0`);
     - name tags (`0x1016d0f0`);
     - the effect loader (`0x1001ddf0`);
     - SwiffRender's fonts (`0x10179060`);
     - display modes, shader defines and statistics, the overgrowth, the bundled mesh's shadow map.

   Citing these moves the effective figures toward their upper bounds (EXE 90.1%, RendDX9 91.3%, AIDLL 95.8%) without changing behaviour.
4. **Hedges and stand-ins** (237 hits). The new ones that matter for exactness:
   - the bots' random numbers (Bots.cpp:811, a stand-in for AIDLL's Mersenne twister `DAT_101394cc`);
   - the soldier's box (0.7 × 1.85 m; GameWorld.cpp:32 and 3383, engine.md:7891: the soldier mesh's box isn't loaded), which mine triggers and stuck rounds use;
   - a round lying on a vehicle (`Projectile::lyingOn`, a stand-in for the engine's rigid body: engine.md:8140, Weapons.h:624);
   - the flank spots (engine.md:12314);
   - the overhead camera's speed factor `+0x270` (taken as 1, HudMenus.h:198).
5. **SwiffPlayer.dll has no classification audit.** Its 13.7% cited means little (section 1b). An uncited.md-style pass over its 0.94 MB would give it an effective figure like the other three modules'.

### 3c. Out of scope (unchanged)

- the online back end (FESL / EASTL, GameSpy GP, BFHQ, leaderboards, award grants);
- PunkBuster, IGA, SecuROM;
- the lightmapper and TextureAtlasBuilder;
- the editors;
- the Novint Falcon and the G15 LCD;
- the AI's CPU budgets.

### Priority, overall

| Rank | Item | Kind |
|---|---|---|
| 1 | P1: the bot squads checked against the retail game (formation, flank spots, the SAI's strength sums) | behaviour / verification |
| 2 | P3: python.onRepairEvent's reader | behaviour |
| 3 | P2: mech seats on joined clients | behaviour (joined) |
| 4 | P4: the commander's mini-map tab | behaviour (commander) |
| 5 | P5 + P7: mech effect events, the small HUD and seat items | behaviour |
| 6 | 3b-2: classify the ~25 called template names that nothing mentions (may promote some to 3a) | bookkeeping |
| 7 | P6 + P8: commander / vote remainder; non-English text | behaviour, low |
| 8 | 3b-1: bind the settled stub rows as no-ops | plumbing |
| 9 | 3b-4: the bots' RNG and the soldier's box | exactness |
| 10 | 3b-3, 3b-5: citations of present code; a SwiffPlayer classification pass | bookkeeping |

## 4. Citation spot-check (citations added since coverage-6)

There are 17 citations from `git diff 2041898..HEAD -- src`, across the three modules. Each was read against `build/analysis/<module>/decomp`, and against `symbols/BF2142.exe.template_properties.csv` for the template rows. Where a constant was stored as data, the PE bytes were read directly.

| # | Citation | Claim | Result |
|---|---|---|---|
| 1 | BotsSquad.cpp:35–36 | strike radius 40 (`+0xb8`, getter `0x10087f80`), life 25 s (`+0xc0`) | match: the squad manager's constructor stores `+0xb8 = 0x42200000` (40.0f) and `+0xc0 = 25.0` (double) |
| 2 | BotsSquad.cpp:351 | `0x10088f20`: a spot older than 25 s goes, every member of every squad re-goaled (`0x100c1ca0`) | match (both sides' spots, `+0x28`/`+0x30` and `+0x70`/`+0x78`) |
| 3 | Formation.h:20, :2–4 | a slot is 0x24 bytes from `0x100f67d0`'s loop, one per possible member (IAIGame slot 0x54) | match (`FUN_10023880(0x24)`, `DAT_10139454` vt+0x54) |
| 4 | Formation.h:85 | `0x100f64a0`: the slot given up is cleared and the last used slot moves into it | match |
| 5 | BotsSquad.cpp:1589 | the kit `0x100f04a0`: floor(U × 4); unlock levels one over, at most 4, through vt+0xb0 → vt+0xa0 | match (`DAT_1012b1b8` is the int 4) |
| 6 | MineZones.h:19 | class 7 through the pathfinder's slot 0x5c `0x1003f600` → `0x100b8030` | match (callers pass 7; `0x100b8030` writes the polygon record's byte `+1`) |
| 7 | FirstPerson.h (fine tuning) | zoomFineTuneSteps `+0x44` / `0x004eb8ad`, useZoomFineTuning `0x004eb9c7` | match (csv) |
| 8 | FirstPerson.h (zoomWhenBusy) | `+0xd`, executor `0x004eaebd`, getter slot 0xb8 `0x0064a000` | match (`0x0064a000` returns byte `+0xd`) |
| 9 | FirstPerson.h:60–75 | allowZoomOnHeat (GenericFireArm `+0x2d0`) in `0x005c50e0`'s busy test: busy or hot, the press is taken only with zoomWhenBusy, or with allowZoomOnHeat while overheated (heat `+0x1a0` slot 0x2c) | match; `takesPress` is the same logic |
| 10 | GameWorld.cpp (magnet bait) | overrideArmingSpeed: row `0x005009ed`, executor `0x00500aab`, TargetObject `+0x2a7` | match (csv) |
| 11 | Weapons.h:402, Weapons.cpp:434 | canReload `0x005f4140`: not full unless allowReloadWithFullMag (`+0x43`) | match |
| 12 | GameProtocol.cpp:494, :951 | StickyProjectile (`0x0050f720`): u16, u16, (u8, u8 when the target is set), u8, then four vectors into `+0x2c`, `+0x0c`, `+0x1c`, `+0x3c` | match |
| 13 | GameWorld.cpp:4486, GameWorld.h:2744 | a pod spawn's tries 150 m up (`static_init_008e2e20`, `*0x00a6b0d8`), `0x005b2200` with the point's `+0x2b`, less 0.875 for a soldier root | match (`0x43160000` = 150.0) |
| 14 | SimpleShadowmap.h:12–13, :67–91 | `0x100a8f40` quantises floor(v × 0.011764706 + 0.5) & 3 into 2-bit fields; `0x100a84c0` reads q / 3 at `+0x110` / `+0x114`, 1 with no map | match |
| 15 | LightSettings.cpp | `renderer.globalStaticMeshLodDistanceScale`: constructor `0x10051e80`, setter `0x10051f70` | match |
| 16 | HudCombatArea.h:16–24 | RendDX9 `0x10187e70`: 0x500 alpha 0; 0x402 OR buffer 2 into alpha; 0x703 alpha = (255 − a) × buffer 3 / 256 | match (buffers `+0x84` / `+0x80`) |
| 17 | DemoCamera.h:131–141 | `0x00432cc0`: '\\' and '/' as '/' (`0x0065bce0` → `0x0065b770`), "Demos/" when the name has no extension, + "-def.bf2142cam", only with a local player | **mismatch in an edge case** (below) |

All 17 match except one edge case:
- **#17, the extension test.** The exe's extension is the text after the name's last '.' (`0x0065bb00` → `0x0065b5c0`: rfind('.'), and none if that '.' follows another '.'). The rewrite looks for a '.' after the last '/'.
- **Where they differ:** names whose directory has a dot (`my.demos/run`: the exe adds neither "Demos/" nor ".bf2142demo") and names starting with "../" (`../run`: the exe sees an extension "/run").
- `net::demoPath` (src/Net/Demo.cpp:383) has the same test. Low impact: it only matters for a typed `demo.recordDemo` name.

One citation is consistent but only partly verified:
- **GameWorld.cpp:6066:** `0x005706b0` copies the kit's ability flags only when they are set, so "once set it stays" holds. The byte for hasZoomUpgradeAbility is reached through the Ability's vtable (`0x0054dd78` → slot 0x9c) and wasn't followed to its offset.

## 5. Inconsistencies and measurement artefacts

1. **AIDLL citations in `src/Game/Bots*.cpp` are dropped.** cite.py gives a colliding DLL address to the DLL named within 1,500 characters before it, else to the src directory's DLL. `src/Game` maps to none, and since 2041898 about 600 lines with AIDLL addresses were added to Bots.cpp, BotsSquad.cpp, Bots.h and BotsLook.cpp. Many of those addresses also fall inside SwiffPlayer or RendDX9 functions.
   - The no-audit run drops 251 occurrences, against 128 at 2041898.
   - Counted as AIDLL's, those under `src/Game/Bot*` would add 51 functions / 9.5 KB to AIDLL's citations (1,769 functions, 52.56%). That was measured with a scratch copy of cite.py; the checked-in tool was not changed, to keep the series comparable.
   - Either a `src/Game/Bot` → AIDLL fallback in `dir_module`, or a DLL name in those files' comments, would fix it.
2. **The no-audit classifier can't see uncited.md's classifications.** The (d) runs of section 2a are all classified there, but `uncited_noaudit` (which leaves uncited.md out) still counts them as missing. The no-audit (d) figures are therefore an upper bound on what is missing. In the coverage-3 rule, which reads uncited.md, the same runs count as cited.
3. **The audit trail inflates the coverage-3 rule.** coverage-6's run tables and uncited.md list the start addresses of runs classified as (a) / (b) / inert. That rule counts them as cited: 7.7 points of RendDX9 and 6.1 of AIDLL. Prefer the headline.
4. **The release build was stale at HEAD.** `build/release`'s bf2142 predated the last commit's sources, so the probe and level runs would have measured older code. It was rebuilt before measuring. Anyone rerunning should run `ninja -C build/release src/Main/bf2142` first, as run_all.sh's header says.
5. **Carried over from coverage-6:**
   - The level runs print only the top 25 stubs.
   - The start-up count includes the 158 host-only methods.
   - AIDLL's own console (282 commands) is in neither console table.
   - Container code propagated (d) makes rewritten modules look missing.
6. **No regressions.** No row went from impl to stub, and none from read to unread. The level runs' stub lists only shrank, and all 804 tests pass.

## Raw outputs

Outside the repo, in the session scratchpad (`/private/tmp/claude-501/-Volumes-Corsair-git-projects-bf2142-recomp/4fc29a33-5499-4852-8614-58e6319f17f7/scratchpad/`):
- `c7head/`: HEAD 455d4ec, the full `run_all.sh --before c6head` output:
  - `probe_*`, `rows*.json`, `delta.md`, `tableA.md`, `stubs_called.md`, `unread_called.md`;
  - `runs/`, `runs_table.md`;
  - `cite*.json`, `uncited_*.json`, `labels_*`, `hedges.*`.
- `c7before/`: 2041898, coverage-6's outputs with the classifier re-run by HEAD's `uncited.py` (`u_*.txt`).
- `c6head/`: coverage-6's 2041898 outputs, unchanged.
- `citealt/`: the scratch cite.py with the Bots fallback (section 5, item 1).
- Logs: `c7head.log`, `c7_tests.log`.
- `c7_tables.py`, `fn.sh`, `peread.py`: the run tables, the decompilation lookups and the PE reads of the spot-check.
