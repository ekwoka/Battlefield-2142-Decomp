# BF2142 rewrite: tenth coverage audit (2026-11-11, HEAD 4d7a8b7)

This audit compares HEAD 4d7a8b7 with e8fe41c, the commit that added coverage-9 (coverage-9 itself measured 967ea3a, one commit earlier). Five commits lie between them:

- **39c3bc7**: README only (the licence line).
- **7219f6b**, the playtest sweep:
  - fire.batchSize's projectiles with their sub-projectile deviation (`0x005f31d0`, `0x005f5540`: the shotguns' 12 pellets, the EU AV's 4 rockets);
  - the repair tool's ray from the camera (`0x005d84a0`);
  - `--drive`, `--spawn` and the remote actions;
  - **citations of present code** (coverage-9 §3c-3): the HUD's console rows (`0x00760a26`), the file manager (`0x00696f80`), the display modes (`0x10010830`);
  - `swf.py`'s corrected labels (bfhqComparePlayers, the buddy list and the BFHQ stats rows hand-labelled online).
- **133984a**: the menu cursor after the movies, a reload begun zoomed, landing-gear probes from the rest mount (`0x006caef0`), walker feet against objects (`0x00650e90`), seat cameras by the round's layout; bots: the map spot as the path start (`0x100966c0`, `0x10055650`), the SAI's orders and the bot leader's squad order (`0x100c21b0`), vehicle goals at the areas' order positions (`0x1005d940`).
- **ebf58f5**: bundled and skinned meshes' opaque materials drawn with back faces culled (`0x100238b0`, `0x10040070`, `0x100c3880`); the walker's legs as their own collider (`0x006dc7b0`, `0x006ce740`); a turned seat's placement (`0x00587a30`); bots walking to the vehicle while pressing use (`0x1006dac0`), EntryPoints at any depth (`0x005a98e0`), squad goals on the controlled object's map (`0x100c1300`).
- **4d7a8b7**: static meshes culled but for tree leaves (`0x100db6f0`, with their `_alphatest` maps); drivers' room rays on the trace map (`aiPathfinding.traceMap`, `0x1003f930`) and the look-ahead details; hosted rounds: handlers heard only while playing (`0x00444140`, `0x00446860`), players restarted at the countdown (`0x004c03b0`), client names, spawn groups sent to clients, joined map changes.

Not measured here: the undergrowth's cull rule (§3a), which is in the working tree only.

## How this was measured

The method and calibration are coverage-6's to coverage-9's, so the series can be compared:
- the same scripts (`tools/coverage/`, HEAD's copy, on both trees);
- the same corpus (`calls.json`: 6,702 files, 816,424 calls) and the same cached decompilation features;
- the same factors (EXE 0.84, RendDX9 0.69, AIDLL 0.48; SwiffPlayer 0.83);
- the corrected cite attribution (`src/Game/Bots*` → AIDLL).

**Clean worktrees.** Another agent had uncommitted src/ work in the main checkout (multiplayer and prediction: GameWorld, Titans, Prediction, main.cpp), and this audit's undergrowth change was uncommitted too. So both commits were measured from clean worktrees:
1. `tools/coverage/build_old.sh e8fe41c …/wt-before` and `build_old.sh 4d7a8b7 …/wt-head`, both built offline.
2. `run_all.sh --repo <wt> --out …` with `BF2142_GAME_DIR` set to the main checkout's `source/Battlefield 2142`. HEAD's run took `--before` from e8fe41c's outputs.
3. The unit tests were built (release) and run headless in each worktree.
4. Both worktrees were removed afterwards.

**The tool change in this interval.** 7219f6b changed `swf.py` (hand labels only). To separate it from the code, e8fe41c's own `tools/coverage` was also run once on e8fe41c's tree (no-audit citations). The EXE, RendDX9 and AIDLL lines come out identical with either copy; SwiffPlayer moves from 92.1% to 92.4% (§1c).

**e8fe41c against coverage-9's HEAD column.** e8fe41c is coverage-9's commit plus the follow-up code it landed with, so it doesn't reproduce coverage-9's 967ea3a digit for digit:
- no-audit citations: EXE 3,511 → 3,538 functions, AIDLL 1,794 → 1,796; RendDX9 1,318 unchanged;
- calibrated 85.5 / 77.1 / 79.3%, as coverage-9; all three 82.7 → 82.8%;
- rows: stored-read 1,019 → 1,021, unread names 277 → 275 (both from e8fe41c's own work);
- hedges 171 raw → 163.

**No window was opened:**
- The probe and the level runs used `--simulate` (one probe mode used `--screenshot`) with `--nosound --no-movies`.
- The host probe used `--host --dedicated` without `--online`.
- Nothing went beyond loopback.

**The headline figures leave out the audit reports** (`--no-audit-docs`, `uncited_noaudit`), as before. The coverage-3 rule is given for the series.

**Tools:** nothing broke and nothing in `tools/` was changed by this audit. Every script ran clean on both trees.

## 1. Headline numbers, before → after

| Metric | e8fe41c | **HEAD 4d7a8b7** | Change |
|---|---|---|---|
| Probe, distinct names: impl / stub (union of `--simulate`, `--host`, `--screenshot`) | 1,179 / 1,509 | **1,179 / 1,509** | none (1,021 with `--simulate` alone; 158 host-only) |
| Console rows (2,837): impl / stub | 1,189 / 332 | **1,189 / 332** | none |
| … ObjectTemplate stored-read / stored-unread | 1,021 / 295 | **1,021 / 295** | none |
| Unread template property names (lower bound) | 275 (205) | **275 (205)** | none |
| BF2142.exe rows impl / stub; RendDX9 | 915 / 218; 274 / 114 | same | |
| Start-up "console methods stubbed": EXE / RendDX9 | 469 / 114 | 469 / 114 | still includes the 158 host-only methods |
| Stub rows called by shipped scripts / calls | 0 / 0 | **0 / 0** | |
| Unread rows called / calls | 137 / 31,613 | **137 / 31,613** | unique names 127 / 31,246 |
| Level runs: "N calls to M unimplemented methods" | 0 / 0 in all 11 | **0 / 0** in all 11 | |
| Python tracebacks (11 runs) / "Unknown object or method!" per run | 0 / 6,689–7,599 | 0 / 6,689–7,599 | Wake_Island_2142 7,104 → 7,103; the rest identical |
| Unit tests (`BF2142_GAME_DIR` set, release, in the worktrees) | 911, 0 failing | **928, 0 failing** | +17 (the working tree with the undergrowth test: 929) |
| Hedge hits (`hedges.py`): raw / filtered false positives | 163 / 41 | **163 / 41** | the same 163 lines by text (section 3d) |

**The console surface didn't move**, and neither did the template rows: the interval's work is behaviour behind rows already bound (fire batches, the walker's legs, the seats, the bots' paths, culling) and citations of present code. Wake_Island_2142's one fewer unknown-method line is the run's own variance (the other ten are identical).

### 1a. Address citations (start addresses; src/ and docs/, tests excluded)

**Headline: no audit docs** (`--no-audit-docs`), corrected attribution:

| Module | functions / bytes | e8fe41c, without appendix | **HEAD, without appendix** | with appendix |
|---|---|---|---|---|
| BF2142.exe | 35,589 / 4,794,004 | 3,538 fns, 1,395,835 B (29.12%) | **3,957 fns, 1,456,600 B (30.38%)** | 4,438 (31.30%) → **4,753, 1,546,495 B (32.26%)** |
| RendDX9_ori.dll | 10,810 / 1,677,614 | 1,318, 443,506 B (26.44%) | **1,325, 450,943 B (26.88%)** | same |
| AIDLL.dll | 6,080 / 1,006,188 | 1,796, 540,196 B (53.69%) | **1,804, 540,826 B (53.75%)** | same |
| SwiffPlayer.dll | 5,756 / 936,991 | 428, 251,777 B (26.87%) | **428, 251,777 B (26.87%)** | same |
| BF2VoipServer.dll | 377 / 38,482 | 34 (28.5%) | 34 | same |

**Gain:**

| Module | Functions | Bytes | Points | Where |
|---|---|---|---|---|
| EXE | +419 | +60.8 KB | +1.26 | src/Hud/Hud.cpp 288 (the HUD rows), engine.md 125, FileManager.cpp 5, BotsSquad.cpp 1 |
| RendDX9 | +7 | +7.4 KB | +0.44 | engine.md's culling notes (`0x100238b0`, `0x100a2100`, `0x100db6f0`, `0x100dca10`, `0x100dcd50`, `0x100e3860`); Viewer.cpp's mode list `0x10010830` |
| AIDLL | +8 | +0.6 KB | +0.06 | the drivers' rays and the use plan (`0x10040530`, `0x1003f370`, `0x100a4330`, `0x100d41c0`…) |
| SwiffPlayer | 0 | 0 | 0 | |

No citation went away. Most of the EXE's gain is the comment-only HUD row table (§3a).

**Ambiguous DLL addresses dropped** (no audit docs): 194 → 199.

**The coverage-3 rule** (audit docs counted):

| Module | e8fe41c | HEAD |
|---|---|---|
| EXE | 3,856 (31.19%) | 4,280 / 1,592,105 B (33.21%) |
| RendDX9 | 1,564 (34.12%) | 1,570 / 578,737 B (34.50%) |
| AIDLL | 1,973 (59.23%) | 1,981 / 596,607 B (59.29%) |
| SwiffPlayer | 500 (37.12%) | 517 / 372,045 B (39.71%) |

SwiffPlayer's gain under this rule is uncited.md's new "Citing present code" section, which names the online list classes it hand-checked.

**Also:**
- **With tests/:** EXE 3,904 → 4,328 (33.41%).
- **With `--headings`:** RendDX9 1,580 → 1,586 (34.91%), AIDLL 2,017 → 2,025 (60.18%).

**Uncited runs, no audit docs:**
- **EXE (≥ 30 KB):** 10 / 0.71 MB with the appendix, 11 / 0.76 without → **10 / 0.68 MB** either way. The HUD map node's rows `0x00760a26` (48 KB) and the file paths `0x00696f80` (31 KB) are gone. FESL / EASTL `0x00840340` (173 KB) is now split in two (69 + 104 KB) by a citation of a 3-byte empty function, `0x00854170` (§5.2).
- **RendDX9 (≥ 20 KB):** 13 / 0.45 → **12 / 0.42 MB**. `0x100df060` (23 KB) is gone, cited by the static mesh material notes (`0x100e3860`). The effect loader `0x1001ddf0` shrank from 27 to 21 KB (the state defaults `0x100238b0`). The display modes' run `0x100106b0` (25 KB) became `0x10010970` (24 KB).
- **AIDLL (≥ 20 KB):** none.
- **SwiffPlayer (≥ 20 KB):** 6 / 0.31 MB, unchanged.

### 1b. Effective coverage (the uncited classifier, `uncited.py`)

The definitions are coverage-4's:
- **Needed** is the total minus (a) third-party and (b) dev-only code. For SwiffPlayer it also excludes (o) online-only and (u) unused by the shipped movies.
- **Lower** is cited plus string- or console-evidenced (c).
- **Calibrated** adds propagated (c) × the factor and splits "?" pro rata.
- **Upper** counts all (c) and all "?".

**Headline: no audit docs, corrected attribution** (`uncited_noaudit`, appendix counted):

| Module | Needed | Cited, of needed | Lower | **Calibrated** | Upper | (d) calibrated | Raw (d) |
|---|---|---|---|---|---|---|---|
| BF2142.exe | 3.709 → 3.710 MB | 40.5 → 41.7% | 55.7 → **55.8%** | 85.5 → **85.6%** | 91.2 → 91.3% | 0.537 → **0.535 MB** | 318 → **316 KB** |
| RendDX9_ori.dll | 1.247 → 1.250 MB | 35.6 → 36.1% | 43.9 → **44.5%** | 77.1 → **77.5%** | 91.8 → 92.1% | 0.285 → **0.281 MB** | 99 → **96 KB** |
| AIDLL.dll | 0.879 MB | 61.5 → 61.6% | 62.8 → **62.9%** | 79.3 → **79.3%** | 96.8% | 0.182 MB | 27 → **27 KB** |
| All three | 5.834 → 5.839 MB | 42.6 → **43.5%** | 54.2 → **54.4%** | 82.79 → **82.91%** | 92.2 → 92.3% | 1.004 → **0.998 MB** | 446 → **440 KB** |
| SwiffPlayer.dll (apart) | 0.593 MB | 42.5% | 55.0% | **92.4%** | 100% | 0.045 MB | 0 KB |

**How to read it:**
- **The three-module calibrated gain is +0.12 points**, the smallest of the series (coverage-9: +0.3, coverage-8: +0.7). The 419 new EXE citations are almost all of code the classifier already counted (c): the HUD rows are console rows, so the row rule labelled their executors (c) before they were cited. Citing them moves "cited" (+1.2 points of the EXE's needed code) and barely moves "calibrated" (+0.1). That is the expected effect of §3c-3's bookkeeping, and coverage-9 predicted it.
- **Raw (d) fell 6 KB:** EXE −2 KB, RendDX9 −3 KB, nothing went up.
  - RendDX9's 2.2 KB (d) run `0x10009cc0` dropped under the 2 KB list threshold: twelve functions at its tail (`0x1000a270`..`0x1000a650`) turned propagated (c) as callees of the newly cited display-mode list. No code was added there.
  - The EXE's 2 KB is propagated (d) next to the new HUD and FileManager citations.
- **RendDX9 +0.4 points** is the culling citations and the mode list. They are citations of code the rewrite now follows (cull modes, the leaf test), which is behaviour, not only bookkeeping.
- **The interval's behaviour work sits behind already-(c) code:** the fire batches, the walker's legs and feet, the seat matrix, the bots' map spots and room rays all extend functions the classifier had as present. It shows in the tests (+17) and in engine.md, not in the effective figures.

**The coverage-3 rule** (`uncited_with`), for the series:

| Module | Lower | Calibrated | Raw (d) |
|---|---|---|---|
| EXE | 57.6 → 58.6% | **87.0 → 87.3%** | 268 → 266 KB |
| RendDX9 | 50.3 → 50.7% | **82.2 → 82.4%** | 43 → 42 KB |
| AIDLL | 68.5 → 68.6% | **83.0 → 83.1%** | 10 KB |
| All three | 57.6 → 58.4% | **85.41 → 85.59%** | 0.858 → 0.848 MB (d) calibrated |
| SwiffPlayer | 60.4 → 62.1% | 93.3 → 93.6% | 0 |

**Also:**
- **With `--headings`:** all three 85.61 → 85.79%; RendDX9 82.6 → 82.8%, AIDLL 83.8%.
- **This classifier's own propagation precision** (`uncited_noaudit_recal`):
  - factors EXE 0.49, RendDX9 0.75 → 0.76, AIDLL 0.42; SwiffPlayer not measurable (it prints 1.00: no (d) is left to measure against, coverage-9 §5.2);
  - calibrated: EXE 73.7%, RendDX9 79.8 → 80.6%, AIDLL 77.3%, all three 75.57 → 75.70%.

  The EXE's reused factor (0.84) is still about 1.7 times this classifier's own. Read the absolute EXE figure as soft and the before/after as firm.

### 1c. SwiffPlayer.dll

| Tree, tools (no audit docs) | Needed | Cited | Lower | **Calibrated** | Upper | Raw (d) | (o) / (u) |
|---|---|---|---|---|---|---|---|
| coverage-9, 967ea3a (its own tools) | 0.617 MB | 40.0% | 52.9% | **92.0%** | 100% | 0 | 66 / 49 KB |
| e8fe41c, e8fe41c's tools | 0.62 MB | | 53.4% | **92.1%** | 100% | 0 | 66 / 48 KB |
| e8fe41c, HEAD's tools | 0.593 MB | 42.5% | 55.0% | **92.4%** | 100% | 0 | 89 / 48 KB |
| **HEAD 4d7a8b7** | 0.593 MB | 42.5% | 55.0% | **92.4%** | 100% | **0** | 89 / 48 KB |

- **The +0.3 points are the tool, not the code.** 7219f6b's `swf.py` labels bfhqComparePlayers' list (`0x1006e8e0`, its fill `0x1006d490`), the buddy list (`0x10061ff0`, `0x100622e0`) and the BFHQ stats list's category 1–6 row makers (`0x1007aec0`…) online (o), with their callees: (o) 66 → 89 KB, needed 0.62 → 0.593 MB. These had been propagated (c) by the shared list base's vtable-sibling vote; uncited.md "Citing present code" has the hand-check.
- No SwiffPlayer address was cited in src or engine.md in this interval.
- The upper bound is still 100%: what is left is the 280 KB of propagated (c), now with three wrong groups taken out.

## 2. The largest remaining runs

### 2a. Missing ((d) runs, no audit docs, ≥ 2 KB)

The counts are runs / KB:

| Module | e8fe41c | **HEAD** | Changed |
|---|---|---|---|
| BF2142.exe | 16 / 76.3 | **16 / 76.3** | none |
| RendDX9_ori.dll | 11 / 35.8 | **10 / 33.7** | `0x10009cc0` (2.2 KB) under the threshold (§1b) |
| AIDLL.dll | 3 / 12.0 | **3 / 12.0** | none |
| SwiffPlayer.dll | 0 | **0** | |

Every run is coverage-7 to coverage-9's, classified in uncited.md. By size:

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
| RendDX9 `0x100ca150` | 4.2 KB | the terrain editor's data | (b) |
| RendDX9 `0x1002bb90` | 4.0 KB | the shader statistics | (b) |
| AIDLL `0x100c4a00` | 3.7 KB | the debug printers | (b) |

The smallest are the 2.0–2.6 KB unset template rows (respawnDelay, cameraSwaySensitivity, regulateYaw, castsStaticShadow). **None of them is behaviour a player would miss.**

### 2b. Largest uncited runs (no audit docs, without the appendix), with the classifier's makeup

The KB values are the classifier's labels inside the run: a = third-party, b = dev, c = evidenced, c-p = propagated present, q = unsure, d = missing, d-p = propagated missing, o = online, u = unused.

**EXE** (≥ 30 KB; 11 → 10 runs, 0.76 → 0.68 MB):

| Run | Size | Makeup (KB) | What it is |
|---|---|---|---|
| `0x008155c0` | 128 KB | a 128 | the FESL browser |
| `0x00854180` | 104 KB | a 88, c-p 10, d 2 | FESL / EASTL, second half (was part of `0x00840340`, 173 KB) |
| `0x007380a0` | 79 KB | a 53, c-p 21, b 3 | GameSpy |
| `0x008025b0` | 69 KB | a 69 | FESL account fields |
| `0x00840340` | 69 KB | a 69 | FESL / EASTL, first half |
| `0x007c9090` | 52 KB | c-p 27, q 20 | the awards pop-up and the HUD manager's head (not citable: uncited.md) |
| `0x0072a180` | 49 KB | a 49 | GameSpy |
| `0x008c970a` | 39 KB | a 38 | static init |
| `0x00870c90` | 36 KB | c-p 20, d 8, d-p 6 | dice::meme effects (not citable: uncited.md) |
| `0x0065d840` | 30 KB | b 27 | the profiler's XML |

Gone: the HUD map node's rows `0x00760a26` (48 KB) and the file paths `0x00696f80` (31 KB), both cited by 7219f6b. Of the other (c) blocks coverage-9 listed as citable, `0x007c9090` and `0x00870c90` were found not citable, and the settings managers `0x0046d580` (32 KB at 967ea3a) were already under the 30 KB threshold at e8fe41c.

**RendDX9** (≥ 20 KB; 13 → 12 runs, 0.45 → 0.42 MB):

| Run | Size | Makeup (KB) | What it is |
|---|---|---|---|
| `0x101885b0` | 83 KB | b 77 | debug drawers, the ported menu shader |
| `0x10053e00` | 58 KB | a 58 | IGA |
| `0x101543a0` | 48 KB | c-p 28, b 10, d 2 | the terrain editor |
| `0x10101310` | 36 KB | b 35 | RoadEditable |
| `0x10041b10` | 25 KB | c-p 9, c 5, a 5, d-p 2 | shader statistics |
| `0x1002eb90` | 25 KB | c-p 7, c 6, d-p 6, a 3 | the shader defines |
| `0x10010970` | 24 KB (was `0x100106b0`, 25) | c-p 15, a 5 | display modes, after the cited mode list |
| `0x100cd220` | 22 KB | b 6, c 5, d-p 4 | TerrainEditable |
| `0x10092990` | 22 KB | c-p 14, a 3 | the overgrowth's containers (not citable: uncited.md) |
| `0x100c7030` | 21 KB | c-p 9, d-p 7 | the bundled mesh's shadow map |
| `0x1001ddf0` | 21 KB (was 27) | c-p 10, c 5, a 5 | the effect loader (not citable: uncited.md) |
| `0x1016d0f0` | 21 KB | c-p 12, a 6 | the name tags, the specular LUT |

Gone: `0x100df060` (23 KB, c-p 19), by the static mesh material citations (`0x100dca10`, `0x100dcd50`, `0x100e3860`).

**AIDLL:** none ≥ 20 KB. **SwiffPlayer:** coverage-9's six runs, unchanged (GameSpy HTTP / SOAP and Speex 127 KB, expat 53, the country tables 44 (o), getListEntries' list class 35 (u), the menu host 21, the server info and BFHQ pages 21).

## 3. What changed, and what remains

### 3a. Since coverage-9

coverage-9's list (§3b) was nearly all done in e8fe41c itself, the "before" commit here:

| Item | Status | Commit | Notes |
|---|---|---|---|
| P1, bot squads checked against the retail game | **open** | | Still no side-by-side. The interval changed the bots' paths a good deal (133984a, ebf58f5, 4d7a8b7), and `BF2142_BOT_STATS` now measures stuck time (engine.md): drivers stuck in MoveTo over 300 s fell from 960–1,381 s to 0–750 s on the five maps measured. |
| P2, collisionParts on statics, decals on parts | **done** | e8fe41c | Then 133984a / ebf58f5: the walker's feet on objects (`0x00650e90`), its legs as their own collider (`0x006dc7b0`). |
| P3 + P4, the canopy's animation; the cells' armour effects | **done** | e8fe41c | Left: a joined client's cells' effects; a part as a mover of its own (`0x005f6ba0`; both closed since, §3g, §3h). |
| P5, the bots' `+0x264` and slot-39 flags | **done** | e8fe41c | |
| P6, debriefing rows, the game modes' order | **done** | e8fe41c | |
| 3c-3, citations of present code | **done** (as far as citable) | 7219f6b | HUD rows, FileManager, display modes cited. `0x007c9090`, `0x00870c90`, the effect loader and the overgrowth's containers found not citable. Six differences found and recorded, not changed (uncited.md). SwiffPlayer's propagated (c) hand-checked: three online groups relabelled. |
| 3c-4, untraced flags | **open** | | 163 hedges, the same lines (§3d). |

**Rendering, found by playtesting rather than by the audits:** back-face culling for bundled, skinned and static meshes (ebf58f5, 4d7a8b7), with RendDX9's state defaults and the leaf test cited. **This audit adds the undergrowth's** (working tree, not in HEAD):
- Undergrowth.fx's 22 passes all set `CullMode = CW`, and its draw (`0x1010e270`) sets no cull state of its own, so the original culls Direct3D's front faces.
- The meshes are wound for that. The editor's import (`0x1010cb70`) negates z, so in Undergrowth.dat 79 of the 94 distinct shipped meshes enclose a negative volume; every rock is among them. The generator (`0x1010ba40`) only rotates and copies the indices.
- So a rock is drawn from outside, and of a ring of grass cards the near ones are drawn.
- The rewrite drew both sides. Its undergrowth pipeline now culls front faces (`GpuContext.cpp`; engine.md "Undergrowth", "Faces culled"; test `Undergrowth_MeshesWoundInwards`).
- Grass on Camp_Gibraltar and Verdun stays visible in `--play` screenshots, thinner where the far cards of each tuft were drawn before.
- Frame cost is within noise (BF2142_FRAME_BENCH=300, three alternating runs each):
  - Camp_Gibraltar: 1.92 / 1.92 / 1.92 → 1.91 / 1.92 / 1.92 ms;
  - Verdun: 4.62 / 3.05 / 3.07 → 3.05 / 3.03 / 3.14 ms.
- Overgrowth trees need nothing: TrunkOG keeps CullMode 3 and LeafOG sets NONE, which the static mesh rule already covers.

### 3b. Behaviour gaps a player could notice (priority order)

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| P1 | **Bots against the retail game** (unchanged) | Squad effectiveness (13.3 kills a round with the formation as decoded, 19.4 when every member heads for the order) and drivers' paths are unverified. Wider vehicles still rock where the infantry mesh leaves less room than they need (minsk_coop's eu_tank, Verdun's as_apc), and a driver stops where the infantry mesh has a hole that the vehicle mesh crosses (Camp_Gibraltar's PAC heavy mech). Verdun's stuck time rose (3,693 → 3,963 s), from seated passengers. | engine.md "Squads", the drivers' room rays | M (verification) |
| P2 | **Multiplayer: the joined client** | A hosted round's restart at the countdown resets the players only; vehicles, control points and dropped kits stay as they are (whether the original's reset, `0x004ce910`, restores them wasn't checked here). ~~A joined client's cells don't show their armour effects~~ (closed, §3g). ~~The dirty masks are stand-ins~~ (closed, §3j). A joined client's Titan velocity is estimated from its updates, so the first second and a half of its motion corrects a soldier standing in it by up to 5 cm (§3g). | engine.md (restartPlayers), uncited.md "canopy, cells and parts", hedges | M; another agent's area |
| P3 | ~~**Parts as movers of their own**~~ **closed** (§3h) | Traced and followed: the dispatch's chain runs each awake mobile part as a mover whose contacts move its root (`0x006dd1b0`, `0x006c9060`, `0x006ba7d0`); eight shipped parts have a vehicle LOD to move with. | engine.md "The parts in the mesh physics" | — |
| P4 | ~~**Commander panel leftovers**~~ **closed** (§3f) | The voice icons, `SELECTED_ALL_SQUADS` and the request's events were already done (c9486a4, engine.md); `us_beacon` (`0x004d2650`) is a load-time pass over a template the shipped content doesn't have. | uncited.md (d) #3 | — |
| P5 | ~~**Particle leftovers**~~ **closed** (§3f) | All four were already done (d9a49fe; engine.md "Mesh particles' directionalScale" and the view flags); the open list was stale. | uncited.md (d) #1 | — |
| P6 | ~~**Small differences recorded while citing**~~ **closed** (§3f, §3g) | Display modes, the FileManager's three cases, the three HUD rows' argument types and the shared border executor follow the original. | uncited.md "Citing present code" | — |
| P7 | ~~**Rendering details**~~ **closed** (§3i) | ~~Undergrowth's `ZFunc = Less`~~ (closed, §3f); ~~the language → LCID table~~ (closed, §3h); ~~the overgrowth's alpha-to-coverage path~~ (closed with the antialiasing's multisampled world pass, §3i). | engine.md "Overgrowth", "Antialiasing"; uncited.md (d) #10 | — |
| P8 | ~~**`allowEmoteAnimation`**~~ **closed** (§3h) | Soldiers gesture with their radio messages. | uncited.md (d) #6 | — |

### 3c. Bookkeeping and plumbing (no visible behaviour)

1. **Unread template rows.** There are 275 names, 127 of them called (31,246 calls). They are all classified in uncited.md or engine.md. The editor flags are still about 25,000 of the calls.
2. **The console counts include inert bindings** (coverage-8 §5.2, unchanged): the 37 InertRows no-ops.
3. **Citations of present code are done as far as they go.** The remaining large (c) runs are not citable (uncited.md). So the effective figures will now move only with new behaviour or with hand-checks of propagated (c). The settings managers `0x0046d580` (under 30 KB) are the one citable EXE block left that coverage-9 named.
4. **SwiffPlayer:** 280 KB of propagated (c). Three groups were hand-checked and found online. A further check of the rest would sharpen 92.4%; the upper bound is 100%.

### 3d. Hedges

`hedges.py` counts **163** at both commits, after its 41 known false positives. They are the same 163 lines by text: the interval's engine.md additions (about 460 lines) brought no new hedge, and none went.

Of the 163, three more are not hedges:
- engine.md:1595, "the rider's-feet stand-in is gone" (coverage-9 §5.3);
- src/Main/HudWorld.h:75, "-1 when not known", which describes a return value;
- engine.md:14096, the heading "Not decoded, and the rewrite's stand-ins", whose items count on their own.

**So there are 160 real hedges.** By kind:
- AIDLL's console rows "not rewritten" (engine.md:13650–13692): 13 lines, all classified (debug, tooling, unused by the scripts);
- untraced flags and writers ("isn't traced", "wasn't found" and the like): about 55;
- stand-ins: 24;
- fitted values: 10. These are the HUD list columns, the overgrowth cell size and the shadow map fit.

**Since (§3j): 154**, after 42 false positives. Gone: the three lines above that weren't hedges (reworded), the dirty masks' stand-in (GhostPriority.h, HostedGame.cpp, engine.md "Ghost priorities and bandwidth"), the soldier's mode-3 0x40000 field (the ability battery), the own state's first flag (the hit indicator), the stale "other classes' layouts aren't decoded" (all 17 networkable classes decode), ServerSettings.cpp's quoted " Time Left: ". `addExtraAmmoItem`'s flag is traced to its one reader (the spawn screen's ability icons); where the firearm takes the counts is still open, and the icons Ability3..6Active are now listed as not rewritten, so that item counts three lines.

**Since (§3k):** those three lines are gone (the counts' reader found, the icons rewritten); one fact came in, written in two places (engine.md "The ability bar", HudWorld.h): a joined client's ammo-kit targets count full, as nothing networks another soldier's magazines and what the exe's client's copies hold isn't traced. Net −1 from this work (the tool's count, 146 in the working tree, also holds another agent's hedge work).

**Since (§3l): 141** in the working tree, after 42 false positives: −12 from §3l's tracing (13 lines gone, one "not rewritten" line in), −1 from §3k's.

**Since (§3m): 133** in the working tree, after 42 false positives: −8 from §3m's tracing (the bot invite's three stand-in lines, `+0x1f8`'s stand-in, the move queue's untraced depth, the mutiny bar's "not modelled", and two stale lines: the bots' on-mesh spawn test, which was already rewritten, and `allowEmoteAnimation`, which the emotes read). No line came in.

### 3e. Out of scope (unchanged)

- the online back end (FESL / EASTL, GameSpy GP / HTTP / SOAP, BFHQ, leaderboards, buddies, the online account and its country list);
- PunkBuster, IGA, SecuROM;
- the lightmapper and TextureAtlasBuilder;
- the editors;
- the Novint Falcon and the G15 LCD;
- the AI's CPU budgets.

### 3f. Closed after the audit (2026-11-11, working tree on a00ce5b)

| Item | What changed | Original | Test |
|---|---|---|---|
| §4 #7, ChangePlayerName | Nothing to change: HostedGame already renames only the sender's own player. The setPlayerName comment says so now. | `0x0050bb90` (`+0x2c` against the event's player, then the player manager's slot 0x38) | `NetHost_NameSquadSpawnAndSpawnWait` (a rename of the bot leader is refused, the client's own goes through) |
| P6, display modes | The video options list RendDX9's modes: 32 bits, ≥ 800 wide, ≥ 480 high at ≥ 60 Hz (a refresh SDL leaves 0 is kept), 4:3 only without `+widescreen 1`, never 848 or 1024 × 480, ascending. New `+widescreen 1` option. | `0x10010830`, `0x100084a0`, `0x100079b0`, `0x10007b60`, `0x10007ce0`; WideScreenEx from `0x00409950` case 0x13 | `DisplayModes_TheOriginalsFilters` |
| P6, FileManager | `exists` erases the first "bf2/content/" anywhere; `expandPath` leaves a path already under the first search path there (under the root, the rewrite's search paths being absolute). The archive-only read mode is never set in the shipped exe (constructor `0x0069aee0` clears +0x68; no writer), so readFile is unchanged. | `0x00698e40`, `0x00697d70`, `0x00698b60` | `FileManager_PathEdgeCases` |
| P6, HUD rows | `setEditNodeFont`'s second argument a bool, `setCompassNodeTextureSize` two ints, `setNodeLogicShowVariable`'s value an int; each row now cited. | `0x00765755`, `0x0076b4ff` (→ `0x007b0880`), `0x0076e090` | `Hud_LogicValueIsAnInt` |
| P7, undergrowth depth | The undergrowth pipeline tests less (Undergrowth.fx's 22 passes, `ZFunc = Less`); the others keep less-or-equal. | Undergrowth.fx | none (a GPU pipeline state); `--play --screenshot` on Camp_Gibraltar still shows the grass |
| P4, P5 | Found already done; the open list was stale. `us_beacon` (`0x004d2650`): a load-time pass that also looks up `ordermarker_*` and `us_beacon` by name and calls each template's slot 0x68; no shipped file names `us_beacon`. | | |

Left of these lists: the overgrowth's alpha-to-coverage, SwiffHost's LCID table, emotes (the border executor closed in §3g; the LCID table and the emotes in §3h).

**Measured** (the same scripts, `run_all.sh` on the working tree; the source measurements also on a clean a00ce5b worktree). Headline rule, no audit docs:

| Module | a00ce5b | Working tree |
|---|---|---|
| BF2142.exe cited / calibrated | 41.69 / 85.59% | 41.73 / 85.59% |
| RendDX9_ori.dll cited / calibrated | 36.08 / 77.49% | 36.20 / **77.69%** |
| RendDX9 raw (d) | 96.7 KB | **94.0 KB** |
| All three calibrated | 82.913% | **82.955%** |

- The working tree also holds another agent's uncommitted physics work; the EXE's +0.04 points of cited include it.
- RendDX9's change is the mode list's filters. `0x100079b0`, `0x10007b60`, `0x10007ce0` and `0x100084a0` are now cited. About 20 functions of the `0x10008630`..`0x1000a1e0` run went from propagated (d) to propagated (c).
- The probe and the level runs are unchanged: 1,179 impl names in the probe union, as at 4d7a8b7. A first run gave 1,177, with `demo.recordDemo` and `demo.stopRecording` unbound in the host probe. Those two are bound only with the server's admin (`if (hosted && session.admin)`). A second `probe.py` run gave 1,179 again, so the dip was the run's environment (another agent was running hosts at the same time), not the tree. Hedges: 163, the same lines.
- Tests: 936, 0 failing (the working tree; 929 at the audit, plus this section's 4 new tests and the other agent's).

### 3g. Closed later (2026-11-11, working tree on 341b3ab)

| Item | What changed | Original | Test |
|---|---|---|---|
| The Titan carry (not listed above; engine.md's soldier movement notes) | A soldier in a Titan's hull, or with a Titan under it, takes the Titan's velocity as its carry for 40 ticks; the integration adds it, and the contacts with the Titan leave its point velocity out, so the soldier's own velocity is relative to the Titan. Anything else under it gives a carry the run-over check takes off. On the server and in the client's prediction (the same `SoldierBody`). | `0x006ca660`, `0x006ca3b0`, `0x006c56e0`, `0x006c5df0`, `0x006c6260`, `0x006c6290`, `0x006f23a0`, `0x006f2b00`, `0x006c2e50`; `*0x00a84ae4` 40, `*0x00a84ae8` 1500 | `SoldierPhysics_TitanCarriesWhatStandsInIt`, `SoldierPhysics_VehicleUnderfootCarriesOnlyTheRunOver` |
| P2, a joined client's cells | The host writes each heavy IFV cell's eighths as the capture makes them (floor, at least 1 while any are left); the client sets its cells from them as the apply does, and their armour effects run there. | `0x005ffec0` (capture and apply), armour slots `0x38` / `0x8c` / `0x90` | `Vehicles_JoinedShieldCellsShowArmourEffects`, `NetHost_JoinedRepairCells` |
| P6, the border executor | One border for bars and compasses; setBarNodeBorder and setCompassNodeBorder both set it on either. The citation `0x007b0540` (slot 0x104) for setBarNodeBorder was setBarNodeSnap's. | `0x0076aebf`, `0x007b0780`, `0x007e9270` (vtables `0x009bbdd0`, `0x009bb950`) | `Hud_BarAndCompassShareTheBorder` |

Measured joined (Suez_Canal gpm_ti, standing in the EU Titan while it moves 86 m at 1.08 m/s): the soldier no longer falls behind (0.88 m over the run); the client's corrections are unchanged by the carry, 28 of 2112 records over 1 cm, all while the client's Titan velocity estimate catches up after the start (engine.md "The carry"). Left: P3 (tracing), P8 emotes (both closed since, §3h).

### 3h. Closed later (2026-11-12, working tree on d856e6a)

| Item | What changed | Original | Test |
|---|---|---|---|
| P8, emotes | A radio message's emote trigger (`messages.addEmoteTrigger`) plays on the sender's soldier where the message is heard, in its 3P system, unless he sprints, lies prone or holds a GenericFireArm without `allowEmoteAnimation`. The round's soldiers and a joined game's ghosts (`GameWorld::playEmote`, `playRemoteEmote`, from `GameAudio::onEmote`). | `0x0042e0c0`, `0x004d7fe0`, `0x005edbf0` (soldier vtable `0x00965ed0` slot 34), `0x00585e80`, `0x005896a0`, `0x008b97f0`; template `+0x31e` | `RagDoll_EmotesFollowAllowEmoteAnimation` |
| P7, the LCID table | SwiffPlayer's table (RendDX9's copy is the online progress chart's) and its date, time and number formatters, with each LCID's Windows XP defaults, in the offline menus: the end-of-round score per minute, player list and debriefing, the award progress values, the demos' dates; the demo bookmarks now show the date alone, as the original. | `0x10007730` (RendDX9 `0x1017af30`), `0x10007aa0`, `0x10007b70`, `0x10007bf0`, `0x10007c60`; callers `0x1001dcd0`, `0x10066c40`, `0x10065ad0`, `0x1005f1e0`, `0x10064a50`, `0x10064510`; SwiffHost slot 0x3a8 `0x0046c510` | `LocaleFormat_LanguagesAndFormats`, `EndOfRoundBindings_Natives` (German), `MenuBindings_Natives` (the bookmark's date) |
| P7, alpha to coverage | Not implemented; decided and documented (engine.md "Overgrowth"). SDL GPU 3.4 has `enable_alpha_to_coverage` and multisampling on all its backends, so portability is not the obstacle. The obstacle is that the frame has no multisampled pass: the world, shimmer, camouflage, first-person and HUD passes share one single-sampled target, every pipeline drawn into it would need a twin at the sample count, and SDL GPU resolves colour but not depth, which the camouflage mask and the HUD read. The original's path also changes the tree textures, the leaf shader's alpha, the overgrowth and undergrowth draws' driver state and the undergrowth's alpha reference. | `0x1000aee0`, `0x1000b500`, `0x100214b0`, `0x1002cef0`, `0x1010e270`, `0x101100d0`, `0x10151830`; RaShaderLeaf.fx | none |
| P3, parts as movers of their own | Traced how an attached part reaches its root: only roots are in the dispatch's lists, the dispatch walks each root's response chain and runs each awake mobile part as a mover; the part's node is never integrated (it takes its root's sleep counter, its velocity is zero, the motion is the roots'), the broad phase skips its own tree and the frame's earlier movers, and its solve moves the root's node with the part's contacts (offsets from the part's origin). Followed: each mover part's ground pass and broad phase after its root, its own response (also for what others record in it) solved onto the root after the root's. Eight shipped parts have a vehicle LOD to move with (eu_tank's barrel base, eu_ifv's tracks, us_ag's engines and TAD base, as_hov_light's gunner gun, missile_silo_closed's console base); Springs stay the Vehicle's wheels. | `0x006dbbe0`, `0x006dd1b0`, `0x006dd0b0`, `0x006c0090`, `0x006c0030`, `0x006bfc40`, `0x006c9060`, `0x006c6d30`, `0x006c99a0`, `0x006dc7b0`, `0x006bb050`, `0x006b9260`, `0x006ba7d0`, `0x006c8ca0`, `0x006ba4c0` | `Vehicles_PartsMeetTheGroundAsMovers`, `Vehicles_PartMoverPushesAnotherBody` |

Left: P1, P2 as above; alpha to coverage once the antialiasing setting drives a multisampled world pass (closed since, §3i).

### 3i. Closed later (2026-11-12, working tree on e4b380a)

| Item | What changed | Original | Test |
|---|---|---|---|
| P7, the antialiasing | `VideoSettings.setAntialiasing` ("Off", "<n>Samples") now drives the frame: the world's pipelines have a second set at the sample count (the most the colour and depth-stencil formats take at or below it; Metal on Apple GPUs gives 4 for "8Samples"), and the world, shimmer, camouflage and first-person passes draw into a multisampled colour and depth-stencil target, resolving the colour into the scene texture (the original's StretchRect of its multisampled back buffer) or the target as each part ends. SDL GPU resolves no depth, so nothing reads a resolved depth: the shimmer copies the resolved colour and continues multisampled, the camouflage mask is drawn multisampled against the multisampled depth and resolved, and the post-production, HUD and menu stay single-sampled on the target's own depth-stencil (the HUD's stencil masks start cleared and never needed the world's depth). Off, nothing changes. Set by the profile's Video.con (the console row) and the video page's apply. | `0x1000fb60` ("%dSamples"), `0x1000b500`, `0x1002cef0` (render target `+0x8a0`, StretchRect into `+0x8bc`, MSAA off and no depth-stencil for the post-production) | `RenderScene_AntialiasingAndAlphaToCoverage` (the names, the console row) |
| P7, alpha to coverage | With antialiasing: a level loaded antialiased keeps the trees' leaves' plain textures (`0x10151830`), the plain undergrowth atlas (`0x101100d0`) and the plain overgrowth atlas first (`0x1008c7e0`); each frame doubles the near pass's alphas (`0x1008da30`), draws the overgrowth's leaves opaque with alpha to coverage, alpha = texture x 4 x the Transparency held to 1 (RaShaderLeafOG with HASALPHA2MASK), and the undergrowth with alpha to coverage, reference 0 and alpha / AlphaRef (`0x1010e270`). Plain leaves drawn without coverage (static trees; the setting turned off after the load) take RaShaderLeaf's 2 x alpha at 127, their shadow casters too. Trunks keep `_alphatest`. | `0x1000aee0`, `0x1000b500`, `0x100214b0`, `0x1002cef0`, `0x1008c7e0`, `0x1008da30`, `0x1010e270`, `0x101100d0`, `0x10151830`; RaShaderLeaf.fx, Undergrowth.fx | `RenderScene_AntialiasingAndAlphaToCoverage` (the alphas) |

Checked headless (release build, 1280 × 720, Metal): with "Off", `--screenshot` of Verdun (default and a far camera over the overgrowth) and the menu are byte-identical to the build before (the far camera's two run-to-run variants are the same pair before and after); the `--play` shots (first person, the camouflage on Camp_Gibraltar, no post-production) differ only as much as two runs of the old build do (mean |Δ| ≤ 0.02). With "4Samples" the edges and the foliage are smoothed (the images' high-frequency measure drops from 7.9 to 6.3 on Verdun, 8.6 to 6.1 on the far camera, 12.9 to 8.3 in first person); every split of the world pass ran (shimmer + first person, shimmer + camouflage, no post-production + first person). `BF2142_FRAME_BENCH=600` (three runs each): Verdun 2.65 ms off, 2.84 / 2.91 / 2.86 ms at 2 / 4 / "8" samples; Verdun `--play` 3.16 ms off, 3.48 / 3.42 / 3.41 ms.

### 3j. Closed later (2026-11-12, hedges; working tree on e4b380a)

| Item | What changed | Original | Test |
|---|---|---|---|
| P2, the dirty masks | The host keeps the original's 12 masks a ghost state for every client. Once a frame its snapshot compares each ghost's fields with the last snapshot's, bit by bit of each group's mask (`HostedGame::snapshotGhosts`, `net::networkableFields` / `changedFields`), and ORs what changed into every client's states. An update writes each group with its mask and clears it; a new state has every mask set, the always ones (the players', the teams') only when given, no longer at every rescore; a lost packet ORs back, per bit, what no later packet carried, at 10000, and every removal it carried goes again. A reload's 0x10000 comes from the presses since the last snapshot. A joined client keeps what an update leaves out; the Titan velocity estimate and a flying round's place go by the updates with a position (`RemoteObject::positionTick`). | `0x0067aec0`, `0x004a91c0`, `0x005800e0`, `0x0057f420`, `0x005824f0`, `0x005823c0`, `0x005830e0`, `0x00581e80`, `0x00581d40`, `0x0057dad0`, `0x004a9ba0`; snapshots `0x005fc1a0`, `0x005929f0`; player slot 0x24 `0x00517c9b` | `Net_GhostScheduler`, `Net_ChangedFields`, `NetHost_JoinedRepairCells` (a standing IFV gets no update, the hull's damage goes in fewer bits than the whole state) |
| The hit indicator (found while tracing the own state's first flag) | A player's hit lights his crosshair's HitIndicatorIcon (alpha 1, falling 3 a second), offline and hosted, unless sv.hitIndicator is 0; a joined client gets the flag in its own state. The rewrite had it always off. | `0x004ba070` (game logic slot 0x128, from `0x004ba440`, `0x004ba500`, `0x004ba7a0`, `0x004ba830`), `0x004c0e40`, `0x0059b460`, `0x0059b480`, `0x005912c0`, `0x0062d770`, `0x007be6e0`, `0x00596ed0`; settings `+0xfb` | `GameWorld_HitIndicator` |
| The soldier's mode-3 0x40000 | The kit's ability battery (soldier `+0x338`); the host now sends it in the control record as the original's every-bit mask does. A joined HUD has no ability bar yet, so the client doesn't use it. | `0x005fb8c0`, `0x005fc1a0`, `0x0064aad0`, `0x00586300` | `NetGame_ControlRecordMode3` |
| `addExtraAmmoItem`'s list | Traced (`0x006316e0`: the flag, rounds, magazines, the name; `addExtraAmmoItemToKit`'s fifth argument); the flag marks the grenade unlocks and its one reader is the spawn screen's icons (`0x00798b20`). The firearm's use of the counts wasn't found. | `0x0054ebed`, `0x006316e0`, `0x00798b20` | none (documentation) |

Measured: hedges 163 → 154 (§3d). A joined client (Camp_Gibraltar gpm_cq, 15 bots, 90 s, `--remote-actions` spawn, run, fire, turn) predicted with a mean correction of 0.0001 m, none over 1 cm, and no decode problems; the minsk_coop bot round (300 s) runs as before. Tests: 952, 0 failing (ASan build; the working tree also holds another agent's rendering work). The release and win32 builds pass.

### 3k. Closed later (2026-11-12, the ability bar and unlock abilities; working tree on dbc7bcb)

| Item | What changed | Original | Test |
|---|---|---|---|
| A joined client's ability bar | The client runs the bar as the exe's does (`0x007c3af0` on every machine). The host now sends the gift flags in the own state (`HostedGame::ownState`: `+0x16a` ammo, `+0x168` healed, `+0x169` repaired); no ticks come, so the vehicle branch stays off on a client as in the original (`+0x15c` / `+0x160` keep the constructor's 0). The battery is the control record's, which the prediction now keeps (`SoldierPrediction::battery`) and fills between records by the worn kit's abilityRestoreRate, as the soldier's update does on every machine; it shows as AbilityValue and replaces PrimaryAmmo for a tool. The tool's targets come from the client's own scan (`RemoteHudWorld::stepAbility`: a hub in hand, the repair tool while firing; rescanned every 1 s with none and 2 s with some): our team's soldiers on foot in the sphere, the objects of our team or none that can be mended, or the first object along the look; a share is the networked health. `GameWorld::abilityBarKit` / `abilityBarGadget` / `gadgetScan` / `firstObjectHit` give the client the round's own rules. The offline and hosted bars were already there (`GameWorld::abilityBar`). | `0x007c3af0`, `0x00594cc0`, `0x00596ed0`, `0x005d8ac0`, `0x005d8740`, `0x005d84a0`, `0x005d7e90`, `0x005d8160`, `0x0058d350`, `0x00586300` | `NetHost_JoinedAbilityBar` (end to end: the flags, the battery within 0.03 of the host's, our wounded soldier as the hub's target at his health, put away no targets) |
| Spawn screen Ability0..6Active | Rewritten (`KitMenu::updateAbilityIcons`), the seven of them, at each rebuild of the open kit while the soldier is dead: the extra-ammo entries' flag, `+8` and `+4` (Ability0..2, only ever set until the kit interface's full reset), the sprint use and recharge modifiers, the overheat pairs and the beacon modifier (Ability3..6). The block they read is the kit built for the player: each owned container's Ability, chosen or not; `GameWorld::fillKit` now applies abilities that way too (it took only the containers that apply their items). | `0x00798b20`, `0x00798dd0`, `0x00797d30`, `0x00570ab0`, `0x005706b0`, `0x00597760` | `HudLists_KitMenuAbilityIcons`; `GameWorld_UnlockAbilityLists` (an owned, unchosen container's extra ammo) |
| `addExtraAmmoItem`'s counts | Found: the ammo comps' getters (vtable slots 55 / 56, `0x005f4270` / `0x005f41b0`) add the largest `+8` of the entries naming the weapon to magSize and the largest `+4` to nrOfMags, each unless the template's is −1; the comp's constructor makes the magazines from them. The console's first number is `+8` (rounds a magazine), the second `+4` (magazines): the rewrite had them the other way round and summed them. So the extra sidearm clip is a 15-round magazine, the extra grenade a third grenade. The not-yet-drawn weapons' counts (`weaponAmmo`) take them too. | `0x006316e0`, `0x00630f10` (the order the template writer prints), `0x005f4270`, `0x005f41b0`, `0x005f4c70` | `GameWorld_UnlockAbilityLists` (two entries: +2 rounds a magazine, +3 magazines, the largest of each) |
| The video page's antialiasing list | The list is the renderer's (`0x1000ff50` → `0x1000fb60` over `0x10007e30`: the multisample types 0..16 the device accepts, type 1 left out), not a fixed four: "Off" and each of 2, 4, 8 that SDL GPU takes for the colour and the depth-stencil formats (`GpuContext::sampleCounts`, `listAntialiasingModes`, `MenuBindings::setAntialiasingModes`). On Apple GPUs that is Off, 2, 4. | `0x1005cc20`, `0x1000ff50`, `0x1000fb60`, `0x10007e30` | `DisplayModes_AntialiasingModesAreTheDevices`, `MenuBindings_OfflineNatives` |

Checked headless (release build, Camp_Gibraltar gpm_cq): a joined client (`--connect 127.0.0.1 --kit EU_Support --remote-actions "10:item:4"`, 4 bots) holding the ammo hub draws the ability ring at the centre and the battery as the tool's ammunition bar, as the offline round does (`--play 2 --kit EU_Support --actions "item:4"`); before, a joined client showed neither. `--spawn-screen --unlock-all` shows the ABILITIES row lit (six of seven for EU_Assault, which has no overheat unlock); before, it was empty. The joined client's prediction stayed exact (mean correction 0.0000 m). Tests: 957, 0 failing (ASan build; the working tree also holds another agent's hedge work). The release and win32 builds pass.

### 3l. Closed later (2026-11-12, hedges; working tree on dbc7bcb)

| Item | What changed | Original | Test |
|---|---|---|---|
| The sentry's message 1 (engine.md "The deployables", GameWorld.cpp) | Traced: the gun's DefaultAnimationComp is handed the deployable's animation system and sets message 1 on each shot (held to the trigger's release with `animation.loopingFire`, `+0x1c`, else a pulse). Tracing it showed the engineer's deployed **sentry gun never fired** in the rewrite: it now runs the SentryGun auto controller as the sentry drone runs UAVDrone (fov 30°, 63 m, scan 0.03, cloaked within 5 m, any enemy), its trigger `reactionTime` (1 s) after it takes a target; kills are the deployer's, EMP silences it. The "still" switch plays "fire" while it fires, through the deployable's and the body's systems. | `0x0063ce30`, `0x005e12e0`, `0x005e1260`, `0x005e0f30`, `0x005e0f60`, `0x007a73e0`, `0x005c3cb0`, `0x005c1010`; AIDLL `0x100777a0`, `0x100d3110` | `Vehicles_DeployedSentryGunFires` |
| The auto-controlled gun (found with the above) | An auto-controlled object's gun is the first GenericFireArm of its tree on PIFire; the breadth-first search found unl_drone_sentry's root siren `Car_horn_Alt` (PIAltFire, no projectile), so the shipped **sentry drone never fired** either. | `Car_horn_Alt` (Vehicles_server.zip) | `Drones_SentryShootsEnemies` (the siren added to the field) |
| The BoltClick (`+0x41`) | The weapon sound component's `+0x41` is set by every reload event but state 2 and cleared by the draw and the click: the first dry click after a reload is the BoltClick (either view), later ones the TriggerClick (first person). | `0x005dbfe0`, `0x005dc1d0`, `0x005dc160` | `GameSounds_BoltClickAfterAReload` |
| TCLocalTeamMedic's receivers | The kit's `+0x74` is its Ability component (interface `0xc4b1`); slot 0x38 reads `ability.hasHealingAbility` (the Assault kits), not "a kit with an RTHeal gadget". | `0x004a6f70`, `0x006f99e0`, `0x00658590`, `0x00656550` | `Radio_TargetChannelsReachTheirHearers` |
| setAIEnterOnSpawn | Seats bots only; a human at a point with only that flag spawns standing (the rewrite seated him within 5 m). | `0x005b18e0` | `Vehicles_SpawnIntoTheApc` |
| The turret's look sectors | Held to the ControlInfo's camera-relative yaws (the heaviest-sector choice, its random fallback's six redraws, the cancel test); the shipped pod cannons and the gunship's second position are limited. The frame is taken as the hull's. | AIDLL `0x100d5a40`, `0x100d5440`, `0x100d60f0`, `0x100d6300`; BF2142.exe `0x008a2790`, `0x008a27a0`, `0x008a27b0` | `Bots_TurretLooksAround` |
| Item selection's `+0x14` bit 0 | The object's disabled flag (`0x006f68f0` flag 1): a soldier's firearm's disable sets it, its enable (which making it active runs) clears it, so the key's pick always opens the list; the wheel's test is left out (no weapon is disabled in place here). | `0x004d2a50`, `0x005c42f0`, `0x005c47d0`, `0x005c9840` | none (documentation) |
| The soldier deviation's `+0x44` | Stale: it is the AI deviation (`setAiDeviation`, slot 0xac `0x00680290`), already added for an AI holder. | `0x005de980` | none (documentation) |

Measured: hedges 154 → **141** with §3k's (§3d). One new line ("Not rewritten for either": the auto controllers' siren, reports, fire warning, line-of-fire check, flip-out and scanning input scale). Tests: 957, 0 failing (ASan; the working tree also holds other agents' HUD and extra-ammo work); the release and win32 builds pass; the minsk_coop bot round (300 s) runs.

Left of the traced-but-open: the soldier's `+0x1f4` / `+0x1f8` (toggleWhenNoAmmo's item before): every store found in BF2142.exe writes 0, but stores through the soldier's interface subobjects weren't ruled out; a bot's answer to an invite (no AI path found in `0x004c3190`, AIDLL not searched for an invite event); the AI object's `+0x14` (basicTemp) store; the vehicle post-processing flag's reader (its getter `0x004b19d0` is shared).

### 3m. Closed later (2026-11-12, hedges; working tree on e4463e0)

| Item | What changed | Original | Test |
|---|---|---|---|
| A bot's answer to an invite (§3l's open item, by the AIDLL route) | Nothing answers for a bot. Squad manager slot 0x30 sets the squad's bit in the player's mask (player vtable 0x224 / 0x228, `+0x14c`) and posts 2/0x10 to his connection, which a bot hasn't; the mask's only other readers are the join (`0x004c7290`: a private squad takes a player only with the bit) and the join's clear (`0x004c6c80`). Of the 148 callers of a slot 0x224, those touching the player manager are the squad code and the HUD; AIDLL's adaptors (`0x0089xxxx`–`0x008axxxx`) never call it, and no AI event carries an invite. The rewrite let a bot in no squad take an invite at once, offline and hosted; now an invited bot stays where he is. (The server still doesn't enforce private squads: the lock is the HUD's; not part of this item.) | `0x004c3190`, `0x004c7290`, `0x004c6c80`, `0x00703170` / `0x00703180` | `HudMenus_InviteList`, `NetHost_RadioCommanderToolsAndSquads` (the invite left unanswered) |
| toggleWhenNoAmmo's fallback, soldier `+0x1f8` (§3l's open item) | Found through the soldier's subobject `+0x184`: the kit's add-item (`0x0058c840`, its slot 27) stores at `+0x1f8`, while the kit is being given (`0x0058b680`: soldier `+0x22c`), the PIFire weapon whose itemIndex is the player's kit template's `+0x2c8` (3): the primary. `+0x1f4` is written by the selection (`0x00589dc0`, `+0x1e8` before a PIFire-group change). After the last round of a toggleWhenNoAmmo weapon whose item before can't be selected, the soldier now takes the primary; the rewrite took the kit's first selectable (the knife). | `0x005883d0`, `0x00589dc0`, `0x0058c840`, `0x0058b680`, `0x00570080` | `GameWorld_ToggleWhenNoAmmoFallsBackToThePrimary` |
| The server's move queue (HostedGame.cpp's "not traced in BF2142.exe") | Traced: the input block (`0x00590990` → `0x005904b0`) appends each newer move to the player's move list (slot 0xa8, `0x00583770`), with no limit; the server's player step (`0x004ac660` from `0x004ad5c0`) takes the first each tick (`0x004ac370`) or, with none, decodes the last one taken again for up to 30 ticks in a row (`+0x30`), then an all-zero input. The rewrite dropped moves past four and, dry, waited for two moves while holding the last move's keys without its turn, indefinitely. Now as the original. | `0x00590990`, `0x005904b0`, `0x00583770`, `0x004ac660`, `0x004ac370`, `0x004ad5c0` | `NetHost_PredictionAndSession` (moves stopping: the soldier walks on for the 30 ticks, then stands) |
| The mutiny's bar on applying (Commander.cpp's "not modelled … no mutiny here", stale since the votes) | A mutiny vote's end, passed or cancelled, sets its target's minutes to 5 (`0x005740e0` cases 2 / 3: slot `0x144(5.0)`); losing command stamps the time (`0x00591950`), becoming commander clears it; `Commander.commanderApply` refuses while 5 less the minutes since is above 0, with HUD_HUD_COMMANDER_CANNOT_APPLY_FOR_MINUTE(S). | `0x007d7050`, `0x005740e0`, `0x00591950`, `0x0059b390`..`0x0059b3c0` | `HudVotes_MutinyBarsTheCommander` |
| Stale | GameWorld.cpp's "not rewritten: the extra test" for a bot's squad spawn (it is: `setAiSpawnTest`, `scatterFrom`); engine.md's "Left: `allowEmoteAnimation` … the rewrite has no emotes" (§3i rewrote the emotes). | | |

Measured: hedges 141 → **133** (§3d). The minsk_coop bot round (300 s, 15 bots) prints the same as a build of HEAD, line for line. Joined runs on 127.0.0.1 (`--host` Cerbere_Landing gpm_cq, 6 bots, 90 s; `--connect 127.0.0.1 --play 2 --walk --simulate 40`), two each: HEAD's prediction corrected by 0.5 / 0.2 mm on average, at most 17.5 / 3.6 cm (2 / 6 records over 1 cm), 1592 / 1594 moves replayed; the working tree's by 0.1 mm, at most 0.5 mm (none over 1 cm), 796 replayed: with no move dropped and none waited for, the list stays a move deep. The in-process `NetHost_PredictionAndSession`, whose client and host tick in one thread, goes from 0.2 mm to 3–4 mm on average (at most 20–30 cm, 4–6 records over 1 cm): its dry ticks now repeat the last move, turn included, which the client didn't step. Tests: 959, 1 failing (`GameAudio_WeaponsOfARound`, from another agent's auto reload in the working tree); the release and win32 builds pass.

Left of the traced-but-open (§3l): the AI object's `+0x14` (basicTemp) store; the vehicle post-processing flag's reader. Seen while tracing, not done: the server doesn't enforce a private squad (`0x004c7290`: a locked squad takes a player only with his invite bit, else asks its leader, 2/0x11, and tells the player, 2/0xc); a seated kit's ability pass over other vehicles' occupants (`0x00599f30`'s first sphere, which with the player's `ability.increasedRepairAbility` above 0 takes the occupants of every vehicle in the sphere, else only his own vehicle's; engine.md "Kit abilities in a vehicle").

### Priority, overall

| Rank | Item | Kind |
|---|---|---|
| 1 | P1: bots (squads, drivers) checked against the retail game | behaviour / verification |
| 2 | P2: the joined client's and the hosted round's remaining pieces | behaviour (multiplayer) |
| 3 | ~~P3: parts as movers of their own~~ (closed, §3h) | behaviour |
| 4 | ~~P4 + P5: the commander panel's and the particles' leftovers~~ (closed, §3f) | visual, low |
| 5 | ~~P7: alpha to coverage, after a multisampled world pass~~ (closed with the antialiasing, §3i; P6, P8 and the LCID table closed, §3f–§3h) | exactness, low |
| 6 | 3c-4: a hand-check of SwiffPlayer's propagated (c) | bookkeeping |
| 7 | 3d: the remaining untraced flags | exactness |

## 4. Citation spot-check (citations added since e8fe41c)

`git diff e8fe41c..4d7a8b7 -- src` adds 229 lines with addresses in 33 files. Seventeen citations were checked across the EXE, RendDX9 and AIDLL; no SwiffPlayer address was added. Each was read against `build/analysis/<module>/decomp`.

| # | Citation | Claim | Result |
|---|---|---|---|
| 1 | Weapons.h:374–380 batchSize | FireComp `0x005f31d0` makes fire template `+0x48` projectiles, each through `0x005f2bb0` with the same velocity | match (the loop runs to `*(+0x98)+0x48` with one `&local_1c`; `+0x11c` keeps the count less one) |
| 2 | GameWorld.cpp batchDeviation | `0x005f5540`: a sub-projectile strays by subProjectileDev (template `+0x18`) and is kept at `+0xc` / `+0x10`; the main round adds them; none at 0.01 or less; before the build-up grows | match |
| 3 | MeshCollision.cpp:496–518 kLegsRadius | `0x006dc7b0`: the radius 5 without an instance (`local_8`), the candidate at the root's slot `0x1a4` LOD, the radii's rule calling slot 9 within 0.25..4 | match (`0.25 <= local_8 / local_10 <= 4.0`) |
| 4 | MeshCollision.cpp:1022 | the Spring's step `0x006caef0` reads how far the solve pushed the part (`+0x26c..`), then puts it at rest with `+0x260` through slot `0xe8` | match |
| 5 | GameWorld.cpp repair tool | `0x005d84a0`: the object manager's ray (DAT_00a85220 slot 0x68) along a view's forward × range, the filter `0x005d7d40`, a bot's AI target (DAT_00b4bc0c slot 8) when the ray meets nothing | match on the ray, filter and fallback. That the view is the camera's rests on the `−0x248` adjustment, which wasn't followed. |
| 6 | GameWorld.h:494–500 restartPlayers | `0x004c03b0` calls its slot 0x24 → `0x004be630` → `0x004ce910`; `0x004bd2a0` sets each player's slot 0x100 to now + countdown × 30 | match |
| 7 | GameWorld.h:502 ChangePlayerName | `0x0050bb90` | match. It also checks that the sender's player byte (`+0x2c`) is the player renamed, then calls the player manager's slot 0x38. |
| 8 | GameWorld.cpp turned seat | `0x00587a30`: Ry(`+0x25c`) × the seat's matrix (`0xc4c4` slot 0x108) | match |
| 9 | ScriptHost.cpp:61 | `0x00444140`: a handler runs when its always flag is set or the game logic's slot 0x164 is 1 and slot 0x168 is 3 | match |
| 10 | ScriptHost.cpp:260 | `0x00446860`: a function already registered for the event is left as it is | match (the loop returns None on `*puVar8 == function`) |
| 11 | FileManager.cpp:102 | `0x0069a810` refuses with "not supported in runtime" by `+0x6d` | match (refused while the flag is clear) |
| 12 | BotsSquad.cpp:738 | IBFSquad's `0x008968d0` / `0x00896910` / `0x008968f0` → `0x00896830` (types 5 / 6 / 4) → squad manager slot 0x48 | match on the chain. Which type is attack, defend or move wasn't checked against the HUD. |
| 13 | Bots.h:624, Bots.cpp traceMap −666 | `0x10040530` writes the map's index, or `0xfffffd66` (−666) for a name it doesn't know; `0x1003f930` reads the pathfinder's `+0x5c` | match. `0x10040530` writes `+0x58` of its `this`, which is the pathfinder's second base (`param_1[1] = &vtbl_101143f8`), so `+0x5c` of the pathfinder. |
| 14 | PathFollower.cpp:397 | the straight walk `0x100952c0`: under 200 m (`_DAT_101287b0`, compared with the squared distance) and the mesh ray (slot 0x88) with cost class 5 | match (`0x101287b0` holds 40000.0) |
| 15 | Bots.cpp:2348 | `0x1005d940`: map 1, or `0x1005d840` true, takes the area's vt+0x10 order position; otherwise a spot of the point's | match on the branch (`param_3 == 1`, the float 1.4e-45). `0x1005d840`'s test itself wasn't read. |
| 16 | MeshQueues.h treeLeaf; engine.md | `0x100db6f0`: in a tree (`+0x2a2`), a material with no second map (`+0x30`) whose first map's name (`+0x2c`, `+0x5c`) has no "tile_" sets `+0x1ec` | match |
| 17 | engine.md culling; MeshQueues.h meshCulled | `0x100238b0` leaves the state table's `+0x178` at 3; `0x100c3880` / `0x100a2100` set the cull field (`+0x9c`) to 1 only for alpha mode 1 (`+0x15c`) | match. `+0x178` is entry 22 (D3DRS_CULLMODE) of the 0x10-byte table: its neighbours check out the same way (FILLMODE 3, SHADEMODE 2, ZFUNC 4). |

**All 17 match** on the claims checked. Notes:
- **#5, #12 and #15** were only partly followed, as noted.
- **#7: the citation leaves out the sender check.** `0x0050bb90` renames only when the event's player is the sender's own. The rewrite's `setPlayerName` comment doesn't say so; whether its caller checks wasn't looked at (multiplayer code, another agent's area). **Since checked (§3f):** `HostedGame`'s ChangePlayerName case has compared the event's player with the sender's since 97c5452; the comment now says so and `NetHost_NameSquadSpawnAndSpawnWait` sends a rename of another player and checks it is refused.

## 5. Inconsistencies and measurement artefacts

1. **The before commit is not coverage-9's.** e8fe41c = 967ea3a + coverage-9's follow-up code, so its column is a little ahead of coverage-9's HEAD column (§How this was measured).
2. **A 3-byte citation splits a third-party run.** engine.md:744 cites `0x00854170`, an empty `ret` that the walker legs' slot 0x2c points at. It lies inside the FESL / EASTL block, so the 173 KB run is now reported as 69 + 104 KB. The bytes and labels don't change. Read the EXE's run count (11 → 10) with that in mind: two runs went away (48 + 31 KB) and one was split.
3. **The SwiffPlayer line moved with the tool, not the code** (§1c). Compare SwiffPlayer only within one `swf.py`.
4. **Recalibration** for SwiffPlayer is still degenerate (1.00), as coverage-9 §5.2 said. Report it as not measurable.
5. **Carried over:**
   - The no-audit classifier can't see uncited.md's classifications, so the (d) runs in §2a count as missing although each is classified.
   - Inert bindings count as implemented.
   - The start-up count includes the 158 host-only methods.
   - AIDLL's own console (282 commands) is in neither console table.
   - Ambiguous DLL addresses (194 → 199) are dropped, so the DLLs' cited counts are lower bounds.
6. **No regressions.**
   - No row went from impl to stub, and none from read to unread.
   - The level runs' unknown-method counts are e8fe41c's (one line fewer on Wake_Island_2142).
   - All 928 tests pass at HEAD (929 in the working tree with the undergrowth test).

## Raw outputs

Outside the repo, in the session scratchpad (`/private/tmp/claude-501/-Volumes-Corsair-git-projects-bf2142-recomp/143ae6bf-6c04-4474-a765-c72ce376e4bd/scratchpad/a10/`):
- `head/`: HEAD 4d7a8b7, the full `run_all.sh --before before` output (probe, rows, delta, runs, citations, classifier labels, hedges).
- `before/`: e8fe41c, the same, with HEAD's scripts.
- `oldswf/`: e8fe41c's no-audit classifier with e8fe41c's own `tools/coverage` (§1c).
- `../fn.py` (the decompilation lookup of the spot-check), `../summ.py`, `../summ2.py`, `../runs_mk.py` (the tables' summaries and the runs' makeup).
- Logs: `before.log`, `head.log`, `build_before.log`, `build_head.log`, `tests_before.log`, `tests_head.log`.
