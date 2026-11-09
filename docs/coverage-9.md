# BF2142 rewrite: ninth coverage audit (2026-11-09, HEAD 967ea3a)

This audit compares HEAD 967ea3a with cd98860, the commit coverage-8 measured. Six commits lie between them:

- **307b8a3**:
  - parachutes as simulated bodies, opened by key 9 or by falling bots;
  - bipods and static guns as stationary seats;
  - the heavy IFV's shield cells taking rounds;
  - team damage lists and notices;
  - the joined client's seat-gun rounds and strike fire;
  - the first offline menu natives pass (favourites, direct IP, map list, mods, settings resets);
  - the controls screen's game map and duplicate codes;
  - coverage-8 itself.
- **d6f5340**:
  - the remaining offline menu natives (command-line join and host, local demos and bookmarks, loading screen, map and server info, mod restart, cursor);
  - sentries, drones and pods on the map;
  - the order marker's height and MechFootAnimationValue;
  - the sprint test's jump hold;
  - joined seat views and zoom, and the client-side seat round deviation.
- **5090df6** and **0d15d6a**, the two hedge passes. They include the soldier's real collision box, the bots' clock seed, the medic upright test, the fixed-wing room box, the HUD corner roots and effects draw order. They also:
  - add SwiffPlayer to `uncited.py` (with `swf.py`) and make `hedges.py` drop false positives;
  - bring gameswf citations and the list, text-field and create-server fixes.
- **1876fff**, fixes from manual testing:
  - the right mouse button is IDButton_1;
  - a reload's message is sent once;
  - rockets have their engines (`0x005b3e60`).
- **967ea3a**:
  - rounds meet vehicles' collisionPart children in their live pose (`0x0063ccc0`);
  - wrecks switch to their wreck sub-geometry (`0x006539f0`);
  - rounds with a geometry are drawn with their child bundles (`0x005b2c10` / `0x005b2ca0`).

## How this was measured

The method and calibration are coverage-6's to coverage-8's, so the series can be compared:
- the same scripts (`tools/coverage/`, HEAD's copy, on both trees);
- the same corpus (`calls.json`: 6,702 files, 816,424 calls) and the same cached decompilation features;
- the same factors (EXE 0.84, RendDX9 0.69, AIDLL 0.48; SwiffPlayer 0.83 per tools/coverage/README.md);
- the corrected cite attribution (`src/Game/Bots*` → AIDLL), which is coverage-8's headline line.

**Clean worktrees.** Both commits were measured from clean worktrees, because other agents had uncommitted src/ work in the main checkout (HostedGame, MenuBindings*, ServerQuery):
1. `tools/coverage/build_old.sh cd98860 …/wt-before` and `build_old.sh 967ea3a …/wt-head`, both built offline.
2. `run_all.sh --repo <wt> --out …` with `BF2142_GAME_DIR` set to the main checkout's `source/Battlefield 2142`. HEAD's run took `--before` from cd98860's outputs.
3. The unit tests ran headless in HEAD's worktree.
4. Both worktrees were removed afterwards.

A third, source-only worktree of 5090df6 was used once for SwiffPlayer's intermediate point (§1c), then removed.

**Reproduction.** With HEAD's scripts, cd98860 reproduces coverage-8's HEAD column digit for digit for the EXE, RendDX9 and AIDLL:
- citations 3,404 / 1,303 / 1,790 functions;
- calibrated 85.0 / 77.0 / 79.1%, all three 82.4%;
- raw (d) 333 / 100 / 27 KB;
- probe 1,179 / 1,509; rows 1,189 / 332, 1,015 / 301;
- hedges 231 raw.

So the tool changes since cd98860 (`swf.py`, `uncited.py`'s fourth module, `hedges.py`'s false-positive filter) don't move the three-module series.

**No window was opened:**
- The probe and the level runs used `--simulate` (one probe mode used `--screenshot`) with `--nosound --no-movies`.
- The host probe used `--host --dedicated` without `--online`.
- Nothing went beyond loopback.

**The headline figures leave out the audit reports** (`--no-audit-docs`, `uncited_noaudit`), as before. The coverage-3 rule is given for the series.

**Tools:** nothing was broken and nothing in `tools/` was changed. Every script ran clean on both trees. Two things behave in ways worth knowing (section 5):
- SwiffPlayer's `--recalibrate` factor has become degenerate (1.00): no (d) is left to measure against.
- `hedges.py` counts one new line that is a false positive.

`tools/coverage/README.md` lists the audits up to coverage-8.

## 1. Headline numbers, before → after

| Metric | cd98860 | **HEAD 967ea3a** | Change |
|---|---|---|---|
| Probe, distinct names: impl / stub (union of `--simulate`, `--host`, `--screenshot`) | 1,179 / 1,509 | **1,179 / 1,509** | none (1,021 with `--simulate` alone; 158 host-only) |
| Console rows (2,837): impl / stub | 1,189 / 332 | **1,189 / 332** | none |
| … ObjectTemplate stored-read / stored-unread | 1,015 / 301 | **1,019 / 297** | +4 / −4 |
| Unread template property names (lower bound) | 281 (207) | **277 (206)** | −4 (−1) |
| BF2142.exe rows impl / stub; RendDX9 | 915 / 218; 274 / 114 | same | |
| Start-up "console methods stubbed": EXE / RendDX9 | 469 / 114 | 469 / 114 | still includes the 158 host-only methods |
| Stub rows called by shipped scripts / calls | 0 / 0 | **0 / 0** | |
| Unread rows called / calls | 142 / 31,650 | **139 / 31,643** | unique names 132 / 31,283 → 129 / 31,276 |
| Level runs: "N calls to M unimplemented methods" | 0 / 0 in all 11 | **0 / 0** in all 11 | |
| Python tracebacks (11 runs) / "Unknown object or method!" per run | 0 / 6,689–7,599 | 0 / 6,689–7,599 | identical in every run |
| Unit tests (`BF2142_GAME_DIR` set) | 856, 0 failing | **901, 0 failing** | +45 |
| Hedge hits (`hedges.py`): real / false positives / raw | 190 / 41 / 231 | **171 / 41 / 212** | −19 real (section 3) |

The four names that went from unread to read (`delta.md`):
- `armor.wreckDelay`: the wreck's sub-geometry, `0x00654120`;
- `armor.damagePointOnDestroy`: team damage on the destroying hit;
- `hasDirection`: the rocket engine's nose push, `0x005b3e60`;
- `soldierExitPosition`: the parachute and stationary seats.

The console surface didn't move. This interval's work is template rows read, gameplay behind already-bound rows, the menus (SwiffPlayer, which has no console rows) and hedges settled.

### 1a. Address citations (start addresses; src/ and docs/, tests excluded)

**Headline: no audit docs** (`--no-audit-docs`), corrected attribution:

| Module | functions / bytes | cd98860, without appendix | **HEAD, without appendix** | with appendix |
|---|---|---|---|---|
| BF2142.exe | 35,589 / 4,794,004 | 3,404 fns, 1,361,449 B (28.40%) | **3,511 fns, 1,388,037 B (28.95%)** | 4,304 (30.58%) → **4,411, 1,492,649 B (31.14%)** |
| RendDX9_ori.dll | 10,810 / 1,677,614 | 1,303, 441,038 B (26.29%) | **1,318, 443,506 B (26.44%)** | same |
| AIDLL.dll | 6,080 / 1,006,188 | 1,790, 538,017 B (53.47%) | **1,794, 540,173 B (53.69%)** | same |
| SwiffPlayer.dll | 5,756 / 936,991 | 180, 127,973 B (13.66%) | **415, 246,917 B (26.35%)** | same |
| BF2VoipServer.dll | 377 / 38,482 | 34 (28.5%) | 34 | same |

**Gain:**

| Module | Functions | Bytes | Points |
|---|---|---|---|
| EXE | +107 | +26.6 KB | +0.55 |
| RendDX9 | +15 | +2.5 KB | +0.15 |
| AIDLL | +4 | +2.2 KB | +0.21 |
| SwiffPlayer | +235 | +118.9 KB | +12.7 |

SwiffPlayer's gain is the gameswf and natives citations of 5090df6 / 307b8a3 / d6f5340.

**Ambiguous DLL addresses dropped** (no audit docs): 167 → 192. The new ones are MenuBindingsOffline.cpp's SwiffPlayer addresses beside EXE ones.

**The coverage-3 rule** (audit docs counted):

| Module | cd98860 | HEAD |
|---|---|---|
| EXE | 3,717 (30.44%) | 3,827 / 1,487,244 B (31.02%) |
| RendDX9 | 1,543 (33.94%) | 1,563 / 572,469 B (34.12%) |
| AIDLL | 1,967 (59.01%) | 1,971 / 595,954 B (59.23%) |
| SwiffPlayer | 186 (13.73%) | 482 / 343,301 B (36.64%) |

**Also:**
- **With tests/:** EXE 3,766 → 3,875 (31.2%).
- **With `--headings`:** RendDX9 1,559 → 1,579 (34.53%), AIDLL 2,012 → 2,015 (60.12%).

**Uncited runs, no audit docs:**
- **EXE (≥ 30 KB):** 11 / 0.75 MB with the appendix, 12 / 0.80 without, unchanged in count. `0x0046c880` (the settings managers) shrank from 35 to 32 KB and now starts at `0x0046d580`, through the menu natives' SwiffHost slot citations.
- **RendDX9 (≥ 20 KB):** 13 / 0.46 → **13 / 0.45 MB**. `0x100c6570` (24 KB) now starts at `0x100c7030` (21 KB): the mesh radius getter `0x100c7000` is cited.
- **AIDLL (≥ 20 KB):** none.
- **SwiffPlayer (≥ 20 KB):** 7 / 0.50 → **6 / 0.31 MB** (section 2b).

### 1b. Effective coverage (the uncited classifier, `uncited.py`)

The definitions are coverage-4's:
- **Needed** is the total minus (a) third-party and (b) dev-only code. For SwiffPlayer it also excludes (o) online-only and (u) unused by the shipped movies.
- **Lower** is cited plus string- or console-evidenced (c).
- **Calibrated** adds propagated (c) × the factor and splits "?" pro rata.
- **Upper** counts all (c) and all "?".

**Headline: no audit docs, corrected attribution** (`uncited_noaudit`, appendix counted):

| Module | Needed | Cited, of needed | Lower | **Calibrated** | Upper | (d) calibrated | Raw (d) |
|---|---|---|---|---|---|---|---|
| BF2142.exe | 3.697 → 3.705 MB | 39.7 → 40.3% | 54.9 → **55.5%** | 85.0 → **85.5%** | 90.8 → 91.2% | 0.553 → **0.538 MB** | 333 → **320 KB** |
| RendDX9_ori.dll | 1.246 MB | 35.4 → 35.6% | 43.6 → **43.9%** | 77.0 → **77.1%** | 91.7 → 91.8% | 0.287 → **0.285 MB** | 101 → **100 KB** |
| AIDLL.dll | 0.878 MB | 61.3 → 61.5% | 62.6 → **62.8%** | 79.1 → **79.3%** | 96.7 → 96.7% | 0.183 → **0.182 MB** | 28 → **28 KB** |
| All three | 5.821 → 5.830 MB | 42.0 → **42.5%** | 53.6 → **54.1%** | 82.4 → **82.7%** | 91.9 → 92.1% | 1.023 → **1.006 MB** | 462 → **447 KB** |
| SwiffPlayer.dll (apart) | 0.674 → 0.617 MB | 19.0 → 40.0% | 31.8 → **52.9%** | 86.2 → **92.0%** | 96.5 → 100% | 0.093 → **0.049 MB** | 23 → **0 KB** |

**How to read it:**
- **Raw (d) fell 15 KB, almost all in the EXE** (−14 KB; nothing went the other way). Where the EXE's 14 KB went:
  - 9 KB was propagated (d) that new neighbouring citations turned (c) by adjacency, among it `0x0047f5e0`'s head;
  - the four template rows read (soldierExitPosition, wreckDelay, damagePointOnDestroy, hasDirection: 1.2 KB);
  - the menu natives' EXE side (`CommandLine.cmd`, the cursor, "no mod specified", GSGameMode / GSJoinAddress).
  - RendDX9 moved by 1 KB.
  - AIDLL moved not at all. Its 28 KB of (d) is the debug printers and IDPManager, (b) by uncited.md.
- **The three-module calibrated gain is +0.3 points**, under half of coverage-8's +0.7. The interval's work was mostly:
  - the menus (SwiffPlayer, counted apart);
  - behaviour behind executors already labelled (c) by the console-row rule: the parachutes, the collisionParts, the rocket engine, the team damage;
  - hedges settled, which cite functions already counted.

  Citations of already-(c) code move "cited" and "lower" but not "calibrated" much.
- **SwiffPlayer: the cd98860 column is HEAD's classifier run on cd98860's tree.** That tree has no natives table in uncited.md and fewer citations. SwiffPlayer's line is not added to "all three".

**The coverage-3 rule** (`uncited_with`), for the series:

| Module | Lower | Calibrated | Raw (d) |
|---|---|---|---|
| EXE | 56.8 → 57.4% | **86.7 → 87.0%** | 275 → 269 KB |
| RendDX9 | 50.1 → 50.3% | **82.1 → 82.2%** | 45 → 44 KB |
| AIDLL | 68.3 → 68.5% | **82.9 → 83.0%** | 11 KB |
| All three | 57.1 → 57.5% | **85.2 → 85.4%** | 0.870 → 0.859 MB (d) calibrated |
| SwiffPlayer | | 86.2 → 93.0% | |

**Also:**
- **With `--headings`:** all three 85.4 → 85.6%; RendDX9 82.5 → 82.6%, AIDLL 83.75 → 83.77%.
- **This classifier's own propagation precision** (`uncited_noaudit_recal`):
  - factors EXE 0.49, RendDX9 0.74 → 0.75, AIDLL 0.41, SwiffPlayer 0.82 → **1.00 (degenerate, section 5)**;
  - calibrated: EXE 73.0 → 73.5%, RendDX9 79.5 → 79.8%, AIDLL 76.9 → 77.0%, all three 75.0 → 75.4%.

  The EXE's reused factor (0.84) is still about 1.7 times this classifier's own. Read the absolute EXE figure as soft and the before/after as firm.

### 1c. SwiffPlayer.dll: the series

| Tree (HEAD's `uncited.py`, no audit docs) | Needed | Cited | Lower | **Calibrated** | Upper | Raw (d) | (o) / (u) |
|---|---|---|---|---|---|---|---|
| cd98860 (no natives table, before the gameswf citations) | 0.674 MB | 19.0% | 31.8% | **86.2%** | 96.5% | 23 KB | 47 / 10 KB |
| 5090df6 (SwiffPlayer added to the tools; coverage-8 §1d printed 91.3% for its working tree) | 0.62 MB | | 52.2% | **91.5%** | 99.5% | 2 KB | 104 / 11 KB |
| **HEAD 967ea3a** | 0.617 MB | 40.0% | 52.9% | **92.0%** | 100.0% | **0 KB** | 66 / 49 KB |

- **5090df6 → HEAD:**
  - d6f5340 bound the last missing natives (command-line join and host, local demos, loading screen, server info, mod restart, cursor), so no native is (d) any more.
  - 0d15d6a reclassified `getListEntries` and its list class (`0x10026850`, 35 KB) as unreached by the shipped movies. That is most of the (o) → (u) shift: (o) 104 → 66 KB, (u) 11 → 49 KB.
- **The upper bound is 100%,** so the figure now only measures how much of the module's (c) is propagated: 283 KB of 617. A further gain needs hand-checks of gameswf's propagated core, not more code.
- coverage-8 §1c's first pass (scratch wrapper, every string, factor 0.55) is not comparable with this series.

## 2. The largest remaining unrewritten or unclassified runs

### 2a. Missing ((d) runs, no audit docs, ≥ 2 KB)

The counts are runs / KB:

| Module | cd98860 | **HEAD** | Changed |
|---|---|---|---|
| BF2142.exe | 16 / 80.8 | **16 / 76.3** | `0x0047f5e0` (13.1 KB, BFHQ stats fields) → `0x004818f0` (8.6 KB): its head is cited |
| RendDX9_ori.dll | 11 / 35.8 | **11 / 35.8** | none |
| AIDLL.dll | 3 / 12.0 | **3 / 12.0** | none |
| SwiffPlayer.dll | 4 / 16.6 | **0** | all gone. `0x10014ef0` (the controls' `HEADER_*` table), `0x100760a0` / `0x10077c90` (the server browsers' row fills) and `0x10042d20` (favourites by IP) are cited. `0x10042720` (leaderboards) is (o). |

Every EXE, RendDX9 and AIDLL run is coverage-7 / coverage-8's, classified in uncited.md. By size:

| Run | Size | What it is | Class |
|---|---|---|---|
| EXE `0x006e8570` | 11.8 KB | the object grid's tree build | replaced by design |
| EXE `0x00683e40` | 10.0 KB | IDKey_ / IDButton_ names | replaced by design |
| EXE `0x004818f0` | 8.6 KB | BFHQ stats fields | online |
| EXE `0x0066b175` | 6.3 KB | `Console.getArgumentTypes` | dev |
| RendDX9 `0x100ce6c0` | 6.3 KB | `terraindata.raw` | (b) |
| EXE `0x006ee282` | 5.8 KB | `Object.initGrid` | editor |
| EXE `0x0070cb00` | 5.5 KB | the G15 LCD | out of scope |
| AIDLL `0x10052fe0` | 4.9 KB | IDPManager | (b) |
| EXE `0x004196a4` | 4.9 KB | `SquadManager.list*` | dev |

The smallest are the 2.0–2.6 KB unset template rows (respawnDelay, cameraSwaySensitivity, regulateYaw, castsStaticShadow).

**None of them is behaviour that a player would miss.** The no-audit classifier still can't read uncited.md's classifications (section 5).

### 2b. Largest uncited runs (no audit docs, without the appendix), with the classifier's makeup

The KB values are the classifier's labels inside the run: a = third-party, b = dev, c = evidenced, c-p = propagated present, ? = unsure, d = missing, o = online, u = unused.

**EXE** (≥ 30 KB; 12 runs, as coverage-8 but for the settings managers' run):
- **(a):**
  - FESL / EASTL `0x00840340`, 173 KB;
  - the FESL browser `0x008155c0`, 128 KB;
  - GameSpy `0x007380a0` 79 KB and `0x0072a180` 49 KB;
  - FESL account fields `0x008025b0`, 69 KB;
  - static init `0x008c970a`, 39 KB.
- **(b):** the profiler's XML `0x0065d840`, 30 KB.
- **The (c) blocks that could take citations:**
  - the HUD's font, QuadDrawer and Ref2System `0x007c9090`, 52 KB (c-p 27, ? 20);
  - the HUD map node's rows `0x00760a26`, 48 KB (c 30, d 7, cited only by the appendix);
  - file paths `0x00696f80`, 38 KB (c-p 24);
  - dice::meme::Effect `0x00870c90`, 36 KB (c-p 20, d 8, d-p 6);
  - the settings managers `0x0046d580`, 32 KB (was `0x0046c880`, 35 KB; c-p 23).

**RendDX9** (≥ 20 KB; 13 runs, 0.45 MB):

| Run | Size | Makeup (KB) | What it is |
|---|---|---|---|
| `0x101885b0` | 83 KB | b 77 | debug drawers, the ported menu shader |
| `0x10053e00` | 58 KB | a 58 | IGA |
| `0x101543a0` | 48 KB | c-p 28, b 10 | the terrain editor |
| `0x10101310` | 36 KB | b 35 | RoadEditable |
| `0x1001ddf0` | 27 KB | c-p 16, c 5, a 5 | the effect loader and its state cache |
| `0x10041b10` | 25 KB | c-p 9, c 5, a 5, d-p 3 | shader statistics |
| `0x100106b0` | 25 KB | c-p 15, a 5 | display modes |
| `0x1002eb90` | 25 KB | c-p 7, c 6, d-p 6 | the shader defines |
| `0x100df060` | 23 KB | c-p 19, a 4 | |
| `0x100cd220` | 22 KB | b 6, c 5, d-p 4 | TerrainEditable |
| `0x10092990` | 22 KB | c-p 14 | the rest of the overgrowth |
| `0x100c7030` | 21 KB (was 24) | c-p 9, d-p 7 | the bundled mesh's shadow map |
| `0x1016d0f0` | 21 KB | c-p 12, a 6 | the name tags, the specular LUT |

**AIDLL:** none ≥ 20 KB.

**SwiffPlayer** (≥ 20 KB; 7 / 0.50 → 6 / 0.31 MB):

| Run | Size | Makeup (KB) | What it is |
|---|---|---|---|
| `0x10081910` | 127 KB (was `0x10080eb0`, 194) | a 111, c-p 14 | GameSpy HTTP / SOAP, Speex. The natives' tail and gameswf's start are cited. |
| `0x100e00b0` | 53 KB | a 52 | expat |
| `0x100f1060` | 44 KB | o 34 | the country tables |
| `0x10026850` | 35 KB | u 33 | `getListEntries`' list class, unreached by the shipped movies |
| `0x10009ed0` | 21 KB (was `0x10009ac0`, 22) | c-p 14 | the menu host |
| `0x1006a960` | 21 KB (was `0x10068030`, 30) | c-p 17, o 3 | the server info and BFHQ pages |

Gone from the list:
- `0x100c6f00` (91 KB, gameswf's String, drawing, sprite and font code): cited in src/UI;
- `0x100212b0` (51 KB, the server-browser natives): cited by MenuBindings*.

## 3. What changed, and what remains

### 3a. Since coverage-8

coverage-8's list:

| Item | Status | Commit | Notes |
|---|---|---|---|
| P2, the offline menu natives | **done** | 307b8a3, d6f5340 | Favourites by IP, the local map list, Mod, Cursor and Singleplay. Singleplay's three members are a bare return and two no-ops in the original (spot-check #14). |
| P3, the shield cells | **done** | 307b8a3 | They take rounds in the projectile LOD and are on the HUD. Event 0xb is team damage, now done. The cells' armour effects are left. |
| P4, the joined-client items | **done** | 307b8a3, d6f5340 | The remote seat guns' rounds and the sprint test's jump hold. |
| P5, the small HUD, map and seat items | **done** | d6f5340 | The mech foot animation, the sentries, drones and pods on the map, the marker's height, parachutes as bodies. The kit pool's slot-39 flag is left. |
| P6, the controls screen's names | **done** | 307b8a3 | `0x10014ef0`'s `HEADER_*` table drives getLastDuplicateInput. |
| 3b-4, exactness | **done** | 5090df6 | The soldier's box (Soldier_CollisionMesh's LOD 1, 0.8 × 1.8 m), `lyingOn` settled, the bots' clock seed and the overhead camera's factor (1). |
| 3b-5, SwiffPlayer in `uncited.py` | **done** | 5090df6 | |

Hedges: 190 → 171 real (41 false positives on both trees).

**The 27 hedge lines that went** (by text), among them:
- the shield cells' collision;
- CreateSpawnGroup's bits and KickBan's byte;
- the overhead camera's speed factor;
- the HUD corner roots;
- the mech foot animation;
- the vehicle tag height guesses;
- the soldier box stand-in (engine.md and three src lines);
- `lyingOn`;
- the order marker's height;
- three Bots.cpp "not rewritten"s (the upright test, the ITAir room, the drowned bodies);
- the IGA texture note;
- drawOrder's sense;
- the ground material.

**The eight that arrived:**
- the decals' faces on collisionParts (engine.md:277);
- the cells' armour effects (engine.md:1349, SeatBehaviours.cpp:262);
- the parachute press after the choice (engine.md:1393);
- the bots' `+0x264` gate (engine.md:1402, Bots.cpp:3009);
- the canopy's AnimatedBundle (engine.md:1458);
- engine.md:1437, which is a false positive ("the rider's-feet stand-in is gone").

**S** = days, **M** = 1–3 weeks.

### 3b. Behaviour gaps a player could notice (priority order)

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| P1 | **Bot squads' effectiveness, unverified against the original** (unchanged) | Squad bots on Cerbere_Landing gpm_sp 16 score 13.3 kills a round with the formation as decoded, against 19.4 when every member heads for the order. | engine.md "Squads"; the danger list's writer is still unfound. Only a side-by-side with the retail game can settle it. | M (verification) |
| P2 | **Collision parts: decals and static objects** | Since 967ea3a, rounds hit a walker's legs or a turret in their live pose. The decal still lands on the root's faces. The static objects' collisionPart children (eu_aacannon's, the ground cannons') aren't in the static world, so rounds pass through those parts. | engine.md:277–281 | S |
| P3 | **The parachute's visuals and edge cases** | The canopy's own AnimatedBundle (parachute_deploy / idle / collapse on message 4) isn't played. Parachutes against a Titan's decks are listed as left in uncited.md. The press that lets the round go after the choice isn't traced. | engine.md:1393, 1458; uncited.md "The parachutes … pass", Left | S |
| P4 | **The heavy IFV's cells' armour effects** | The cells don't show e_healing_node at 99, e_titan_consol at 0, or e_bluespark_loop when wrecked. | engine.md:1349, SeatBehaviours.cpp:262 | S |
| P5 | **Bots: two untraced gates** (**done since**) | The bots' `+0x264` flag gating the parachute input block, taken as clear (Bots.cpp:3009). The kit pool's slot-39 container flag, so a bot never takes the second pool (engine.md:13444, GameWorld.cpp:5273). **Since:** `+0x264` is the auto controller flag (`0x1009ac30`'s last argument, 1 from `0x100c8d20`; IAIAutoControllersManager `0x1004d140` / `0x1004d8e0`), which no rewritten bot has (`Bot::autoController`). The slot-39 flag is "the item's itemIndex is the kit's primary slot" (`0x00440dc0` against Kit `+0x2c8` = 3, `0x00570080`), so bots take a primary unlock about half the time; containers whose item has no aiTemplate are left out (`GameWorld::chooseBotUnlocks`; `Bots_UnlocksTakeAPrimaryFromItsOwnPool`). | as cited | S each |
| P6 | **The end-of-round debriefing's rows** (`0x10065ad0`) | Per-kit, per-weapon and per-vehicle round records the game side doesn't keep. The game modes' order on create-server follows names, not the level scan. | uncited.md "Left, with reasons" | S–M |

### 3c. Bookkeeping and plumbing (no visible behaviour)

1. **Unread template rows.** There are 277 names, 129 of them called (31,276 calls). They are all classified in uncited.md or engine.md. The editor flags `isNotSaveable`, `saveInSeparateFile` and `createdInEditor` are still about 25,000 of the calls.
2. **The console counts include inert bindings** (coverage-8 §5.2, unchanged). They are the 37 InertRows no-ops.
3. **Citations of present code.** Citing these moves the effective figures toward their upper bounds (EXE 91.2%, RendDX9 91.8%, AIDLL 96.7%, SwiffPlayer 100%) without changing behaviour:
   - EXE: `0x007c9090`, `0x00760a26`, `0x00696f80`, `0x00870c90`, `0x0046d580`;
   - RendDX9: the effect loader, display modes, shader defines and statistics, the overgrowth, the shadow map, the terrain editor's runtime half;
   - SwiffPlayer: the propagated 283 KB, hand-checked.
4. **Hedges** (171 real). Most of what is left is untraced flags and "not rewritten" lists of AIDLL's console (engine.md:13131–13173). Fitted list columns (HudMenus) and the overgrowth cell size are also left.
5. **The no-audit classifier can't see uncited.md's run classifications** (section 5). The EXE's and RendDX9's (d) runs in §2a count as missing although each is classified.

### 3d. Out of scope (unchanged)

- the online back end (FESL / EASTL, GameSpy GP / HTTP / SOAP, BFHQ, leaderboards, buddies, the online account and its country list);
- PunkBuster, IGA, SecuROM;
- the lightmapper and TextureAtlasBuilder;
- the editors;
- the Novint Falcon and the G15 LCD;
- the AI's CPU budgets.

### Priority, overall

| Rank | Item | Kind |
|---|---|---|
| 1 | P1: the bot squads checked against the retail game | behaviour / verification |
| 2 | P2: collisionParts on static objects, decals on parts | behaviour |
| 3 | P3 + P4: the parachute canopy's animation and Titan decks; the cells' armour effects | behaviour, visual |
| 4 | P5: the bots' `+0x264` gate and slot-39 kit flag (**done since**) | behaviour, low |
| 5 | P6: the end-of-round debriefing rows; the game modes' order | menus, low |
| 6 | 3c-3: citations of present code, and a hand-check of SwiffPlayer's propagated (c) | bookkeeping |
| 7 | 3c-4: the remaining untraced flags | exactness |

## 4. Citation spot-check (citations added since cd98860)

`git diff cd98860..967ea3a -- src` adds 599 lines with addresses in 65 files. Seventeen citations were checked across the four modules. Each was read against `build/analysis/<module>/decomp`; for SwiffPlayer's tiny natives, also against the DLL's bytes.

| # | Citation | Claim | Result |
|---|---|---|---|
| 1 | Parachutes.cpp kOpenFallSpeed | `0x005e4750`: the holder a soldier (0xc4d7); unless `*0x00a6ef84`, the 2 m ray (`0x005e4330`); velocity y under −10; then the room test `0x005e45e0` | match (the flag through `PTR_DAT_00a02a38`) |
| 2 | Parachutes.cpp kRemoveDelay | `0x005cff20`: timer event 0x21 after `0x3dcccccd` (0.1 s) | match |
| 3 | Parachutes.cpp kStillHeight / kLandingTime / kLandingNormal | `*0x00a02000` / `04` / `08` / `0c` from static_init `0x008e42e0`..`0x008e4310`: 8, 0.05, 0.05, 0.3 | match. The cited addresses are the pointers that `0x005cf750` reads; the inits write their targets `0x00a6c9b4`..`c0`. |
| 4 | Parachutes.cpp landing | `0x005cf750`: server only, once (`+0x45c` < 0); any normal when the other is under a quarter of its mass, else the normal's y ≥ 0.3; the water's or the ground's time | match |
| 5 | Bots.cpp:3813 uprightOnGround | BBMedicAssist `0x10070200`: up · normal ≥ 0.7071 | match |
| 6 | Bots.cpp:3815–3828 sphereInRoomBox | `0x1006c3e0` → `0x100ff690(w × 1.2, h × 1.2, l × −0.6, l × 12)`, queried by DAT_101394b0 slot 0x2c, filter vtable `0x101168b0` | match |
| 7 | GameWorld.cpp:32–36 soldier box | `0x006e4880` builds Soldier_CollisionMesh: points at ±0.4 (x, z), y −1..0.8; LOD 1 a copy of LOD 0 (`0x006e1ea0`) | match |
| 8 | Effects.h / RoundVisuals.cpp | `0x005b2c10` sets `+0x25f`, sends 0x44 then 0 to each child; `0x005b2ca0` clears it, sends 1 then 0x45 | match |
| 9 | Weapons.h:161–170 rocket engine | `0x005b3e60`: `+0x25f` set, acceleration `+0x31c` > 0, maxSpeed `+0x324`, hasDirection `+0x335`; start through `0x005b2c10` on the follow's target | match on the fields and gates. The heading-versus-nose order was not followed line by line. |
| 10 | PlayerCamera.cpp:963 | `0x005e0fd0`'s kind 3 sends message 2 once (slot 0x30(2)); 4 and 6 hold and drop it | match on the switch (case 3 → slot 0x30(2), case 4 → 0x2c(2), 0/6 → 0x34(2)). "Once" depends on the caller, which wasn't followed. |
| 11 | Effects.h:262–269 drawOrder | the collector `0x10130bd0`: template `+0x80` 0 (Front) → list `+0x20`, else `+0xc20`; the frame `0x10131b70` draws `+0xc20` first | match |
| 12 | GameWorld.cpp wreck | `0x00654120`: wreckDelay `+0x160` ≤ 0.01, not a soldier (0x9493), no dontUseWreck `+0x169` → `0x006539f0(object, 1)` at once; else event 0x27 after the delay | match |
| 13 | TeamDamage.cpp:30, kHealClears | `0x00655a00`: the heal (percent × max) added to `+0x264`; over 50, or the armour at full, empties `+0x220` and clears `+0x264` | match. The callers pass hit points, which is the original's `fVar2`. |
| 14 | MenuBindingsOffline.cpp:696 Singleplay | constructor `0x10059c20`: chooseMission `0x10059bb0` a bare `ret 0xc`; get/setBotSkills `0x10059bc0` / `0x10059bd0` fetch SwiffHost (slot 0x60) and return | match (bytes `c20c00`; `8b0d54331210 8b01 ff5060 c20c00`) |
| 15 | MenuBindingsOffline.cpp:645 | setCurrentMap `0x100414c0` and setMapList `0x100414d0` do nothing | match (the same two byte patterns) |
| 16 | MenuBindingsOffline.cpp:325 | `0x1003fe20`: SwiffHost slot 0x3a0 ("mods/" + name, …); `0x00469a60` only answers true | match. The native itself returns nothing, as the rewrite's `Value()`. |
| 17 | PartCollision.cpp:3–14, 58–60 | `0x0063ccc0`: each child with collisionPart `+0x74` > 0 and physicsType `+0xf4` not 0/1/2 gets the collision manager's slot 0x28 instance, set as interface 0xcaa32 | match. The original admits any type above 2; the rewrite admits Mesh / MechLegs (3, 4), the only ones the string switch makes. |

**All 17 match** on the claims checked. Notes:
- **#9 and #10** were only partly followed, as noted.
- **One loose wording,** not on the list above. MenuBindingsOffline.cpp:545 says `0x100447e0` (isServerOnline) "answers false". The original returns as_value type 3 with a double 0: the same type `0x100429f0` (validateIp) uses for `(double)bool`. So it is the number 0, not a boolean. The rewrite answers booleans for these natives throughout, and AS1 treats 0 and false alike in the tests the menus make.

## 5. Inconsistencies and measurement artefacts

1. **SwiffPlayer's before column is not the same input as its after.** HEAD's `swf.py` reads the natives table from the measured tree's `docs/uncited.md`, and cd98860 has none. Its natives fall through to the string and propagation rules. The 5090df6 row (§1c) is the first tree with the table: 91.5%, close to coverage-8 §1d's 91.3% for its working tree.
2. **SwiffPlayer's `--recalibrate` is degenerate at HEAD** (factor 1.00, calibrated 100%). With no (d) left, hiding the string labels leaves nothing to propagate (d) from, so every hidden function comes back (c). Use the fixed 0.83 (README). This is not a tool bug, but later audits should report the factor as "not measurable", not 1.00.
3. **`hedges.py`:** engine.md:1437 ("the rider's-feet stand-in is gone") is counted as a hedge. The real count is 170.
4. **Ambiguous DLL addresses rose 167 → 192** (no audit docs). MenuBindingsOffline.cpp cites SwiffPlayer and EXE (SwiffHost slot) addresses side by side, and the nearest-DLL rule drops a share of them. SwiffPlayer's cited count is a lower bound.
5. **Carried over:**
   - The no-audit classifier can't see uncited.md's classifications.
   - Inert bindings count as implemented.
   - The start-up count includes the 158 host-only methods.
   - AIDLL's own console (282 commands) is in neither console table.
   - Container code propagated as (d) makes rewritten modules look missing.
6. **No regressions.**
   - No row went from impl to stub, and none from read to unread.
   - Every level run's unknown-method count is identical to cd98860's.
   - All 901 tests pass.

## Raw outputs

Outside the repo, in the session scratchpad (`/private/tmp/claude-501/-Volumes-Corsair-git-projects-bf2142-recomp/4fc29a33-5499-4852-8614-58e6319f17f7/scratchpad/a9/`):
- `head/`: HEAD 967ea3a, the full `run_all.sh --before before` output:
  - `probe_*`, `rows*.json`, `delta.md`, `tableA.md`, `stubs_called.md`, `unread_called.md`;
  - `runs/`, `runs_table.md`;
  - `cite*.json`, `uncited_*.json`, `labels_*`, `hedges.*`.
- `before/`: cd98860, the same, with HEAD's scripts.
- `mid/`: 5090df6's no-audit citations and classifier (source only, HEAD's rows).
- `fn.py` (the decompilation lookup of the spot-check) and `runs_mk.py` (the runs' makeup).
- Logs: `before.log`, `head.log`, `build_before.log`, `build_head.log`, `tests.log`.
