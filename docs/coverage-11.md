# BF2142 rewrite: eleventh coverage audit (2026-11-12, HEAD dae4d74)

This audit compares HEAD dae4d74 with 4d7a8b7, the commit coverage-10 measured. Ten commits lie between them. coverage-10's §3f–§3q recorded most of them as they landed; §3a folds those sections in. In order:

- **a00ce5b** (with coverage-10 itself):
  - joined soldiers predicted against moving objects, with client Titan collision;
  - the round restart at the countdown resets what the round made (`0x004ce910`);
  - script triggers gated like handlers;
  - the paddles' targets;
  - undergrowth drawn with front faces culled;
  - §3f's display modes, FileManager and HUD rows.
- **341b3ab**:
  - soldiers keep the velocity asked for through a tick in the air (`0x00588420`, `0x006c2230`);
  - contact push-outs combined as the original's (`0x006b6bd0` / `0x006b6b60`);
  - the undergrowth's `ZFunc = Less`;
  - text input only while a menu or the chat takes typing.
- **d856e6a**:
  - drivers arrive by the vehicle's position and radius, on their vehicle type's cost profile (`0x10066a70`, `0x10051050`);
  - the Titan carry (§3g);
  - joined clients' IFV cells (§3g);
  - the shared bar / compass border (§3g).
- **e4b380a**: parts as movers of their own, emotes, the LCID formatters (§3h).
- **dbc7bcb**: the multisampled world pass and alpha to coverage (§3i); the dirty masks, the hit indicator and the mode-3 battery (§3j).
- **e4463e0**: the sentry gun and the sentry drone fire (§3l); the joined ability bar, Ability0..6Active and the extra-ammo counts (§3k).
- **1b1906c**:
  - the kit and vehicle sweep (auto reload `0x00594280`, ammo refills `0x004cf7b0`, the local HUD's health `0x007a0f80`, air transports' pods);
  - §3m's hedges.
- **fde17f4**:
  - the portable shield's facing (§3n), private squads (§3n), the seated kit's targets (§3n);
  - the move queue's records (§3o), globalEffectLevel (§3o), the seated body's countdown (§3o), the spawn point's armour test (§3o).
- **3a6fdd4**:
  - the menu and HUD sweep (volume sliders, video presets, mouse settings, the controls' reset, the HUD page's values, the scoreboard's Server tab);
  - §3p's hedges.
- **dae4d74**:
  - the minimap turns with the player;
  - the audio page's reset applies;
  - `--keys`, and joined clicks reach the HUD;
  - chat handed out by channel, voters asked, the map-vote list;
  - §3q's hedges and the rcon socket's SO_REUSEPORT.

## How this was measured

The method and calibration are coverage-6's to coverage-10's, so the series can be compared:
- the same scripts (`tools/coverage/`; unchanged since 4d7a8b7 but for one README line);
- the same corpus (`calls.json`: 6,702 files, 816,424 calls) and the same cached decompilation features;
- the same factors (EXE 0.84, RendDX9 0.69, AIDLL 0.48; SwiffPlayer 0.83);
- the corrected cite attribution (`src/Game/Bots*` → AIDLL).

**A clean worktree.** The main checkout may hold another agent's uncommitted work, so HEAD was measured from a clean worktree:
1. `tools/coverage/build_old.sh dae4d74 build/scratch/wt11`, built offline (release).
2. `run_all.sh --repo build/scratch/wt11` with `BF2142_GAME_DIR` set to the main checkout's `source/Battlefield 2142` and `BF2142_CONFIG_DIR` set to a scratch folder.
3. The before column is coverage-10's own HEAD output for 4d7a8b7, kept in its scratchpad (`a10/head`, the same scripts). It was passed as `--before`. 4d7a8b7 was not rebuilt.
4. The unit tests were built and run in the worktree, and so were the three sweeps (§6).
5. The worktree was removed afterwards.

**One snag, not a tool bug.** The first `run_all.sh` call ran the worktree's own copy of the scripts. `common.analysis_dir` looks for `build/analysis` in `--repo` and then in the scripts' own checkout, which was the same worktree in that call. `cite.py` stopped with no `functions.csv`. The probe, rows and level runs had already finished, and they don't read the analysis. A symlink to the main checkout's `build/analysis` fixed it, and the remaining steps ran with the main checkout's scripts (identical). The README's recipe (the main checkout's `run_all.sh --repo <wt>`) doesn't hit this. Nothing in `tools/` was changed.

**No window was opened:**
- The probe and the level runs used `--simulate` (one probe mode used `--screenshot`) with `--nosound --no-movies`.
- The host probe used `--host --dedicated` without `--online`.
- The sweeps used `--screenshot` (offscreen) or `--keys` (no window).
- Hosts and clients used 127.0.0.1 only.

**The headline figures leave out the audit reports** (`--no-audit-docs`, `uncited_noaudit`), as before. The coverage-3 rule is given for the series.

## 1. Headline numbers, before → after

| Metric | 4d7a8b7 (coverage-10) | **HEAD dae4d74** | Change |
|---|---|---|---|
| Probe, distinct names: impl / stub (union of `--simulate`, `--host`, `--screenshot`) | 1,179 / 1,509 | **1,179 / 1,509** | none (1,021 with `--simulate` alone; 158 host-only) |
| Console rows (2,837): impl / stub | 1,189 / 332 | **1,189 / 332** | none |
| … ObjectTemplate stored-read / stored-unread | 1,021 / 295 | **1,036 / 280** | +15 read |
| Unread template property names (lower bound) | 275 (205) | **262 (192)** | −13 |
| BF2142.exe rows impl / stub; RendDX9 | 915 / 218; 274 / 114 | same | |
| Start-up "console methods stubbed": EXE / RendDX9 | 469 / 114 | 469 / 114 | still includes the 158 host-only methods |
| Stub rows called by shipped scripts / calls | 0 / 0 | **0 / 0** | |
| Unread rows called / calls | 137 / 31,613 | **132 / 31,358** | unique names 127 / 31,246 → 123 / 30,992 |
| Level runs: "N calls to M unimplemented methods" | 0 / 0 in all 11 | **0 / 0** in all 11 | |
| Python tracebacks (11 runs) / "Unknown object or method!" per run | 0 / 6,689–7,599 | 0 / 6,689–7,599 | Wake_Island_2142 7,103 → 7,104 (coverage-9's value; run variance); the rest identical |
| Unit tests (`BF2142_GAME_DIR` set, release, in the worktree) | 928, 0 failing | **984, 0 failing** | +56 |
| Hedge hits (`hedges.py`): raw / filtered false positives | 163 / 41 | **94 / 42** | −69 (§3d) |

**The console surface didn't move.** The interval's work is behaviour behind rows that were already bound, as in coverage-10. The template rows did move: 13 names went from stored-unread to stored-read, each now read by the code that uses it:
- `abilityInVehicleStrengthModifier` and `autoUseAbility` (the seated kit's ability, §3p);
- `allowEmoteAnimation` (the emotes, §3h);
- `animation.loopingFire` (the sentry's fire animation, §3l);
- the nine `regulatePitch / Yaw / Roll` (with `…Factor` and `…Input`), the PCO regulators (§3q).

Five of the called unread rows (255 calls) went with them. They are the BF2 conversions' regulators and the sentry's looping fire.

### 1a. Address citations (start addresses; src/ and docs/, tests excluded)

**Headline: no audit docs** (`--no-audit-docs`), corrected attribution:

| Module | functions / bytes | 4d7a8b7, without appendix | **HEAD, without appendix** | with appendix |
|---|---|---|---|---|
| BF2142.exe | 35,589 / 4,794,004 | 3,957 fns, 1,456,600 B (30.38%) | **4,088 fns, 1,481,978 B (30.91%)** | 4,753 (32.26%) → **4,881, 1,571,414 B (32.78%)** |
| RendDX9_ori.dll | 10,810 / 1,677,614 | 1,325, 450,943 B (26.88%) | **1,340, 462,283 B (27.56%)** | same |
| AIDLL.dll | 6,080 / 1,006,188 | 1,804, 540,826 B (53.75%) | **1,820, 542,478 B (53.91%)** | same |
| SwiffPlayer.dll | 5,756 / 936,991 | 428, 251,777 B (26.87%) | **438, 254,317 B (27.14%)** | same |
| BF2VoipServer.dll | 377 / 38,482 | 34 (28.5%) | 34 | same |

**Gain** (new citations by the first file that names them):

| Module | Functions | Bytes | Points | Where |
|---|---|---|---|---|
| EXE | +131 (+128 with the appendix) | +25.4 KB | +0.53 | spread over 30-odd files: engine.md 16, GameWorld.cpp 15, GameWorld.h 14, Hud.cpp 14 (§3f's HUD row executors, §3g's shared border `0x007e9270`, the minimap's turn `0x00778d00`, `0x00778e20`, `0x00870f60`, the map alphas `0x00780940`), Networkables.h 7, main.cpp, Bots.cpp and HostedGame.cpp 6 each, GhostPriority.h 4 (the dirty masks), HudMenus.h 4 (the scoreboard) |
| RendDX9 | +15 | +11.3 KB | +0.68 | MenuBindings.cpp 5 (the video presets `0x1000f780`, `0x1000f730`, `0x1000ff50`, `0x1000f130`, `0x1000f600`), DisplayModes.h 4 (§3f's filters), engine.md 3 (the alpha-to-coverage path, `0x100214b0` 6.6 KB), GpuContext.h 2 (`0x10007e30`, `0x1000fb60`), LocaleFormat.h 1 (RendDX9's LCID copy) |
| AIDLL | +17 / −1 | +1.7 KB | +0.16 | Strategy.h 4 (the box area), Bots.h 3, Bots.cpp 3, BotsLook.cpp 3, engine.md 3, BotsScript.cpp 1. Gone: `0x100ed5b0` (326 B), from engine.md's stale "human attention" paragraph, which §3q reworded. |
| SwiffPlayer | +10 | +2.5 KB | +0.27 | MenuBindings.cpp 5 (the audio reset `0x10010a60` / `0x10010bd0`, the controls' reset `0x10016490`, the video presets `0x1005ba10`, `0x1005b9d0`), LocaleFormat.h 3 (the LCID formatters), MenuBindingsOffline.cpp 2 (`0x100a4c50`, `0x10064510`) |

coverage-10's interval added no SwiffPlayer citation. Unlike coverage-10's HUD row table, the EXE's gain is citations of new behaviour, not bookkeeping.

**Ambiguous DLL addresses dropped** (no audit docs): 199 → 215.

**The coverage-3 rule** (audit docs counted):

| Module | 4d7a8b7 | HEAD |
|---|---|---|
| EXE | 4,280 (33.21%) | 4,417 / 1,618,739 B (33.77%) |
| RendDX9 | 1,570 (34.50%) | 1,589 / 582,795 B (34.74%) |
| AIDLL | 1,981 (59.29%) | 1,997 / 598,001 B (59.43%) |
| SwiffPlayer | 517 (39.71%) | 528 / 374,654 B (39.98%) |

**Also:**
- **With tests/:** EXE 4,328 → 4,465 (33.96%).
- **With `--headings`:** RendDX9 1,586 → 1,605 (35.15%), AIDLL 2,025 → 2,041 (60.32%).

**Uncited runs, no audit docs:**
- **EXE (≥ 30 KB):** 10 / 0.68 → **10 / 0.67 MB**. The dice::meme effects run `0x00870c90` (36 KB) now starts at `0x008710c0`. Its head was cut by the minimap's turned draw (`0x00870f60`, engine.md "The minimap turns with the player").
- **RendDX9 (≥ 20 KB):** 12 / 0.42 → **11 / 0.40 MB**. The effect loader's run `0x1001ddf0` (21 KB) went under the threshold. It was split by the alpha-to-coverage path's `0x100214b0` (6.6 KB), cited in engine.md "Antialiasing".
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
| BF2142.exe | 3.710 → 3.717 MB | 41.7 → 42.3% | 55.8 → **56.4%** | 85.6 → **85.9%** | 91.3 → 91.5% | 0.535 → **0.525 MB** | 316 → **309 KB** |
| RendDX9_ori.dll | 1.250 MB | 36.1 → 37.0% | 44.5 → **45.1%** | 77.5 → **77.9%** | 92.1 → 92.5% | 0.281 → **0.276 MB** | 96 → **91 KB** |
| AIDLL.dll | 0.879 MB | 61.6 → 61.7% | 62.9 → **63.0%** | 79.3 → **79.4%** | 96.8% | 0.182 → **0.181 MB** | 27 KB |
| All three | 5.839 → 5.846 MB | 43.5 → **44.1%** | 54.4 → **55.0%** | 82.91 → **83.19%** | 92.3 → 92.5% | 0.998 → **0.983 MB** | 440 → **428 KB** |
| SwiffPlayer.dll (apart) | 0.593 MB | 42.5 → 42.9% | 55.0 → 55.3% | **92.4%** | 100% | 0.045 MB | 0 KB |

**How to read it:**
- **The three-module calibrated gain is +0.28 points.** That is more than coverage-10's interval (+0.12), less than coverage-8's (+0.7). This time it is mostly lower-bound movement (+0.6 points): the new citations are of functions the classifier had as propagated (c) or as missing, not of console executors it had already counted. EXE +0.3, RendDX9 +0.4, AIDLL +0.1.
- **Raw (d) fell 12 KB:** EXE −7, RendDX9 −5, AIDLL 0. Nothing went up. Two (d) runs left the 2 KB list (§2a).
  - EXE: 7.2 KB of (d) turned (c), almost all of it the executors of the 13 template rows that became stored-read (§1). These are the regulate* setters `0x004fd1c3`..`0x004fd988` and the setters at `0x00547255`..`0x005474a2`, `0x0049fda0` and `0x00577f60`. The row rule labels a read row's executor (c).
  - RendDX9: propagated (d) next to the new citations turned (c): the video presets' and display modes' callees in `0x10007…`–`0x1000f…`, and the STL helpers by the shader defines (`0x1002f800`).
- **What the classifier doesn't see:** most of the interval's work is behaviour behind code that was already (c), as in coverage-10. The tests (+56) and engine.md (+1,301 lines) show it, and so do the hedges (−69, §3d). This includes the multisampled pass, the Titan carry, the dirty masks, the ability bar, the private squads, chat by channel and the votes.

**The coverage-3 rule** (`uncited_with`), for the series:

| Module | Lower | Calibrated | Raw (d) |
|---|---|---|---|
| EXE | 58.6 → 59.2% | **87.3 → 87.5%** | 266 → 261 KB |
| RendDX9 | 50.7 → 51.0% | **82.4 → 82.7%** | 42 → 39 KB |
| AIDLL | 68.6 → 68.7% | **83.1%** | 10 KB |
| All three | 58.4 → 58.8% | **85.59 → 85.78%** | 0.848 → 0.838 MB (d) calibrated |
| SwiffPlayer | 62.1 → 62.3% | 93.6% | 0 |

**Also:**
- **With `--headings`:** all three 85.79 → 85.96%; RendDX9 82.8 → 83.0%, AIDLL 83.8 → 83.9%.
- **This classifier's own propagation precision** (`uncited_noaudit_recal`):
  - factors EXE 0.49, RendDX9 0.76 → 0.75, AIDLL 0.42; SwiffPlayer not measurable (1.00, as coverage-9 §5.2 said);
  - calibrated: EXE 73.7 → 74.2%, RendDX9 80.6 → 80.8%, AIDLL 77.3%, all three 75.70 → 76.05%.

  The EXE's reused factor (0.84) is still about 1.7 times this classifier's own, so read the absolute EXE figure as soft. The before/after figures are firm.

### 1c. SwiffPlayer.dll

| Tree (HEAD's tools, no audit docs) | Needed | Cited | Lower | **Calibrated** | Upper | Raw (d) | (o) / (u) |
|---|---|---|---|---|---|---|---|
| 4d7a8b7 | 0.593 MB | 42.5% | 55.0% | **92.4%** | 100% | 0 | 89 / 48 KB |
| **HEAD dae4d74** | 0.593 MB | 42.9% | 55.3% | **92.4%** | 100% | **0** | 89 / 48 KB |

- `swf.py` didn't change, so this line moved with the code alone.
- The ten new citations are the settings pages' natives the menu sweep rewrote and the LCID formatters (§1a). All ten were propagated (c) or evidenced (c) before, so lower and cited rise while calibrated stays at 92.4%.
- What is left is still the propagated (c), now 261 KB of the needed code. A hand-check is the only thing that would move it (§3c).

## 2. The largest remaining runs

### 2a. Missing ((d) runs, no audit docs, ≥ 2 KB)

The counts are runs / KB:

| Module | 4d7a8b7 | **HEAD** | Changed |
|---|---|---|---|
| BF2142.exe | 16 / 76.3 | **15 / 74.2** | `0x004fd1c3` (2.1 KB) is gone: the regulate* rows' executors (`0x004fd2ae`, `0x004fd497`, `0x004fd60e`…), cited and rewritten (§3q) |
| RendDX9_ori.dll | 10 / 33.7 | **9 / 31.6** | `0x1002f780` (2.1 KB) under the threshold: `0x1002f800` and `0x1002fcc0` (STL helpers) turned propagated (c) |
| AIDLL.dll | 3 / 12.0 | **3 / 12.0** | none |
| SwiffPlayer.dll | 0 | **0** | |

Every remaining run is coverage-7 to coverage-10's, classified in uncited.md. By size:

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

The smallest are the 2.0–2.6 KB unset template rows (respawnDelay, cameraSwaySensitivity, castsStaticShadow); `regulateYaw` has left that list. **None of them is behaviour a player would miss.**

### 2b. Largest uncited runs (no audit docs, without the appendix), with the classifier's makeup

The KB values are the classifier's labels inside the run: a = third-party, b = dev, c = evidenced, c-p = propagated present, q = unsure, d = missing, d-p = propagated missing, o = online, u = unused.

**EXE** (≥ 30 KB; 10 runs, 0.68 → 0.67 MB):

| Run | Size | Makeup (KB) | What it is |
|---|---|---|---|
| `0x008155c0` | 128 KB | a 128 | the FESL browser |
| `0x00854180` | 104 KB | a 88, c-p 10, d 2 | FESL / EASTL, second half |
| `0x007380a0` | 79 KB | a 53, c-p 21, b 3 | GameSpy |
| `0x008025b0` | 69 KB | a 69 | FESL account fields |
| `0x00840340` | 69 KB | a 69 | FESL / EASTL, first half |
| `0x007c9090` | 52 KB | c-p 27, q 20 | the awards pop-up and the HUD manager's head (not citable: uncited.md) |
| `0x0072a180` | 49 KB | a 49 | GameSpy |
| `0x008c970a` | 39 KB | a 38 | static init |
| `0x008710c0` (was `0x00870c90`) | 36 KB | c-p 19, d 8, d-p 6 | dice::meme effects (not citable: uncited.md), its head now cited (§1a) |
| `0x0065d840` | 30 KB | b 27 | the profiler's XML |

**RendDX9** (≥ 20 KB; 12 → 11 runs, 0.42 → 0.40 MB):

| Run | Size | Makeup (KB) | What it is |
|---|---|---|---|
| `0x101885b0` | 83 KB | b 77 | debug drawers, the ported menu shader |
| `0x10053e00` | 58 KB | a 58 | IGA |
| `0x101543a0` | 48 KB | c-p 28, b 10, d 2 | the terrain editor |
| `0x10101310` | 36 KB | b 35 | RoadEditable |
| `0x10041b10` | 25 KB | c-p 9, c 5, a 5, d-p 2 | shader statistics |
| `0x1002eb90` | 25 KB | c-p 8, c 6, d-p 5, a 3 | the shader defines |
| `0x10010970` | 24 KB | c-p 15, a 5 | display modes, after the cited mode list |
| `0x100cd220` | 22 KB | b 6, c 5, d-p 4 | TerrainEditable |
| `0x10092990` | 22 KB | c-p 14, a 3 | the overgrowth's containers (not citable: uncited.md) |
| `0x100c7030` | 21 KB | c-p 9, d-p 7 | the bundled mesh's shadow map |
| `0x1016d0f0` | 21 KB | c-p 12, a 6 | the name tags, the specular LUT |

Gone: the effect loader `0x1001ddf0` (21 KB), split by `0x100214b0`.

**AIDLL:** none ≥ 20 KB. **SwiffPlayer:** coverage-9's six runs, unchanged:
- GameSpy HTTP / SOAP and Speex, 127 KB;
- expat, 53 KB;
- the country tables, 44 KB (o);
- getListEntries' list class, 35 KB (u);
- the menu host, 21 KB;
- the server info and BFHQ pages, 21 KB.

## 3. What changed, and what remains

### 3a. Since coverage-10

coverage-10's priority list, its §3f–§3q (each written as it landed) and the commits that §3f–§3q don't cover, folded together:

| Item | Status | Commit | Notes |
|---|---|---|---|
| P1, bots against the retail game | **open** | | Still no side-by-side with retail. The interval changed the bots again: drivers arrive by the vehicle's position and radius on their type's cost profile (d856e6a); the sentry gun and drone fire (§3l); the radio's get_in / bail_out / req_pickup (§3p); the box areas' size (§3q); invites unanswered (§3m). Bot rounds of 22 level / mode pairs end with the same tickets and scores as 3a6fdd4 (§3q). |
| P2, the joined client and the hosted round | **mostly done** | a00ce5b, d856e6a, dbc7bcb, e4463e0, fde17f4, dae4d74 | The countdown's restart now resets what the round made (vehicles, Titans, control points, kits, deployables: `0x004ce910`, engine.md "The countdown"). Also done: joined cells' armour effects (§3g); the dirty masks (§3j); the joined ability bar (§3k); private squads enforced by the server (§3n); a record per move (§3o); joined clicks reaching the HUD, chat by channel, the votes and the map vote (dae4d74, engine.md "The roses, chat and votes headless"). Left: §3b P2. |
| P3, parts as movers of their own | **done** | e4b380a | §3h |
| P4–P8 | **done** | a00ce5b–e4b380a | §3f–§3i: the commander panel and particles (stale), the six differences found while citing, the undergrowth's depth and cull, the LCID table, emotes, alpha to coverage with a real multisampled world pass |
| 3c-4, untraced flags | **largely done** | dbc7bcb–dae4d74 | Hedges 163 → 94 (§3d), seven passes (§3j–§3q) |
| 3c-4, a hand-check of SwiffPlayer's propagated (c) | **open** | | §3c |

**Found by the sweeps rather than by the audits.** 1b1906c, 3a6fdd4 and dae4d74 came out of three headless sweeps (§6). They found behaviour that no audit metric pointed at:
- the auto reload;
- ammo refills for the weapons put away;
- the sentry gun and the drone never firing;
- the portable shield set down edge-on;
- volume sliders that never saved;
- video presets that changed only the radio button;
- mouse settings lost at the next start;
- the HUD page's values never reaching the HUD;
- a joined player unable to pick a rose entry;
- chat going to everyone;
- voters never asked;
- the map vote's list never shown.

All were bound console rows, and all were (c) code to the classifier.

### 3b. Behaviour gaps a player could notice (priority order)

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| P1 | **Bots against the retail game** (unchanged) | Squad effectiveness and drivers' paths are unverified. Wider vehicles still rock where the infantry mesh leaves less room than they need, and drivers stop at infantry-mesh holes the vehicle mesh crosses. engine.md's stand-ins remain: the bots' grenade warning for grenades in flight (the original warns as one goes off), the weapon handling's reload press, no wire-guided target, the area guard's spot. | engine.md "Squads", "Weapons", the drivers' room rays; hedges | M (verification) |
| P2 | **Multiplayer leftovers** | A joined client's Titan velocity is still estimated from the updates; the first second and a half corrects a soldier standing in it by up to 5 cm. The ghost priorities leave out the commander's scan and UAV boosts, `OptimizeEnclosingObjectsNetworking` and the team object's eye (bandwidth order only; HostedGame.cpp). `callForMedic`'s code to the server isn't used there. Autodemo's message 9 and setAutoRecordRounds; the ranked minimums. | engine.md 6716, 6919, 6986, 10818; HostedGame.cpp 3285 | S–M |
| P3 | **Small gameplay details still stand-ins or unrewritten** | The auto controllers' `useSiren`, `reportEnemiesToTeam`, `sendFireWarning`, `checkLineOfFire`, `flipOutAtHealth`, `scanningInputScale` (sentry gun and drone); a deployable on another object isn't picked up within 9 m (`0x004c5950`); the dog tags (`ECGameLogicGLDogTag`, HUD event 0x28) aren't shown; the IK disable flag (`+0x2ea`) and the weapon layers `0x005ef250` for an IK object. | engine.md 10213, 10455, 9409, 2782 | S each |
| P4 | **Rendering details** | Terrain's per-material detail lists, cullerTypes other than 3, detailCullMaxLevel (frame-cost choices); a vehicle's parts drawn with the root's instance (the children's second record); the fade-skipping walk flag `0x1000000`; rotateAsAnimatedUV (one shipped bundle, no object named). | engine.md 3336, 4462, 16257 | S–M, low |
| P5 | **Menu leftovers** | Removing a favourite server (no console method; GeneralSettings slot 0x190); the BFHQ / leaderboard series are offline by design. Not checked headless: the display modes' box, the end-of-round screen in a round, the online pages. | engine.md 10911, 16214, "Not checked headless" | S |
| P6 | **BF2 conversions only** | The wing's `liftRegulated`, `regulateToLift` and `wingToRegulatorRatio` (three conversions' boats and BMP-3 in the install's Levels; no official vehicle). | engine.md 1105 | S |

No gap above makes a shipped level, mode or kit unplayable. The kit sweep, the menu sweep and the key sweep pass in full at HEAD (§6).

### 3c. Bookkeeping and plumbing (no visible behaviour)

1. **Unread template rows.** There are 262 names, 123 of them called (30,992 calls). They are all classified in uncited.md or engine.md. The editor flags are still about 25,000 of the calls (`isNotSaveable` 16,860, `saveInSeparateFile` 4,667, `createdInEditor` 3,453).
2. **The console counts include inert bindings** (coverage-8 §5.2, unchanged): the 37 InertRows no-ops.
3. **Citations of present code** are done as far as they are citable (coverage-10 §3c-3). The effective figures now move with new behaviour, as in this interval, or with hand-checks.
4. **SwiffPlayer:** 261 KB of propagated (c). A hand-check would sharpen 92.4%; the upper bound is 100%.

### 3d. Hedges

`hedges.py` counts **94** at HEAD, after 42 known false positives. At 4d7a8b7 the count was 163 after 41.
- **By text:** 77 of 4d7a8b7's lines are gone and 8 are new.
- **The 8 new lines** come from rewording or are newly named stand-ins:
  - the bots' grenade warning (two lines; §3o);
  - the auto controllers' "Not rewritten for either" (§3l);
  - the joined ammo-kit targets (two lines; §3k);
  - the vehicle node line, engine.md 11645;
  - the weapon choice's stand-ins, engine.md 15216 (§3q);
  - the eye's stand-ins, engine.md 6719.
- **This matches the working-tree counts §3j–§3q recorded:** 163 → 154 → 141 → 133 → 124 → 114 → 94. HEAD 3a6fdd4 counted 115, and dae4d74 brings it to 94.

**By kind, at HEAD:**
- **AIDLL's console rows "not rewritten"** (engine.md 14690–14732): 13 lines, all classified (debug, tooling, unused by the scripts).
- **"Not rewritten" / "isn't rewritten" elsewhere:** 20. Most are listed in §3b P2–P6. The rest name tooling: the Voip server's debug logs, the Novint Falcon, and PathFollower's search time-slicing (a segment a tick).
- **Untraced writers and readers** ("isn't traced", "wasn't found", "not decoded" and the like): about 40. None is on a path the sweeps showed as wrong.
- **Stand-ins:** 12. Of these, the bots' are 11, and P1 covers them; the other is the ghost priority's eye (engine.md 6719).
- **Fitted values:** 6. These are the HUD list columns (3 in engine.md, 2 in HudMenus.cpp) and the shadow map's view-fitted perspective.
- **Other:** 3. These are "not modelled" (the stuck push, twice) and "inferred" (the `.dif` fonts).

### 3e. Out of scope (unchanged)

- the online back end (FESL / EASTL, GameSpy GP / HTTP / SOAP, BFHQ, leaderboards, buddies, the online account and its country list);
- PunkBuster, IGA, SecuROM;
- the lightmapper and TextureAtlasBuilder;
- the editors;
- the Novint Falcon and the G15 LCD;
- the AI's CPU budgets.

### Priority, overall

| Rank | Item | Kind |
|---|---|---|
| 1 | P1: bots (squads, drivers, the stand-ins) checked against the retail game | behaviour / verification |
| 2 | P2: the multiplayer leftovers (Titan velocity on a client, the commander's ghost priorities) | behaviour (multiplayer) |
| 3 | P3: the auto controllers' extras, the dog tags, deployables on objects | behaviour, small |
| 4 | P4 + P5: rendering and menu leftovers | visual / UI, low |
| 5 | 3c-4: a hand-check of SwiffPlayer's propagated (c) | bookkeeping |
| 6 | 3d: the remaining untraced flags (about 40) | exactness |

## 4. Citation spot-check (citations added since 4d7a8b7)

`git diff 4d7a8b7..dae4d74 -- src` adds 594 lines with addresses in 71 files. Twenty citations were checked across the EXE, RendDX9, AIDLL and SwiffPlayer, each read against `build/analysis/<module>/decomp`. Ghidra's MCP server didn't connect, so the dumps were the only source.

| # | Citation | Claim | Result |
|---|---|---|---|
| 1 | GameWorld.h:1024–1029 auto reload | `0x00594280`: with the player's `+0x3d0`, the control object's weapons (0xc4c5 slot 0x78, three) through 0x94c0 slot 0x80; the first that needs it presses the reload | match (`+0x80 = 1.0` under mask `+0x104 & 1`) |
| 2 | GameWorld.h:1031–1035 | `0x005f37f0`: not reloading (`+0x3c` 0), magazine empty (slot 0x98), one can be taken (slot 0x40), template `+0x28` (autoReload) clear | match |
| 3 | GameWorld.cpp:10996, 11202 RTAmmo | `0x004cf7b0`: min(100, it)% of a magazine for every firearm of the kit: the player's containers (slot 0x64), each one's items (slot 0x74), 0x94c0 slot 0x88 | match |
| 4 | HudWorld.cpp:48, HudMenusVehicles.h:706–721 | `0x007a0f80`: PlayerHealth from the soldier's armour (slot 0x3c over slot 0x8c), the string ceil(× 100); the sprint flag `+0x2a8` from slot 0x100, the weapon flag `+0x334` from slot 0xa4 | match |
| 5 | MenuBindingsOffline.cpp:889, SettingsObjects.cpp:20 | `0x0049bdf0`: transparencies 255, crosshair white, minimap 170 rotating, help pop-ups and shake on, colour-blind off | match (`+0x48`/`+0x7c` 255.0, four 0xff, `+0x78` 170.0, bytes 1 1 1 0) |
| 6 | HudMenus.h:486–490 | `0x007bb540`: with the Server tab's `+0x38b` set, no player row (at most a blank odd row) | match |
| 7 | HudMenus.h:493–496 | `0x0065af00`: toupper, and å ä ö (ü é) by hand | match (0xe5 → 0xc5, 0xe4 → 0xc4, 0xf6 → 0xd6, …) |
| 8 | VehicleParts.cpp:111–128 | `0x00563bd0`: without `+0x30a`, or with the bank and input of opposite signs, the bank empties and the input goes; else banked within ±40, at most 1 a tick | match |
| 9 | HostedGame.cpp:641–645 | `0x00642f90` goes on at once; `0x00642e70` goes on in every branch, the text only compared with "Your CD Key is disabled…" | match |
| 10 | HudMenusComm.h:28–33 | `0x0042d220`: codes 0x33 / 0x34 / 0x35 say "medic" on channel 6 (TCLocalTeamMedic), "need_ammo" and "need_repair" on 1 (TCTeam) | match (0x36 / 0x37 are "revive" on 0xf / 0x10) |
| 11 | SoundObjectTemplate.h:64 | `0x007f54a0`: globalEffectLevel is the active engine template's slot 0xb8 (set) / 0xbc (get) | match on the slots. That those slots are the occlusion factor (the engine's side) wasn't re-read. |
| 12 | HudMenusChat.h:203–208 | `0x0079d6e0` case 6 → `0x00572390`: only without a vote of the player's (slot 0x310); not by a kick's or mutiny's target (kinds 2 / 3); a Vote event phase 4; marks it cast | match |
| 13 | GameWorld.h:887–896 | `0x004c7290`: six or more → 2/0xb; private (slot 4) without the invite bit (player slot 0x224) and with members → 2/0x11 to the leader (`piVar6[9]`) and 2/0xc to the player; else the join (`0x004c6c80`) | match |
| 14 | engine.md / GameWorld.cpp, the seated body | `0x00652380`: a soldier's body with a parent skips the landed test (`0x005eda20`), so timeToStayAsWreck (`+0x158`) after the death, or the global fallback, then fades over `+0x15c` and goes (`0x00651db0`) | match. The fallback's 60 s (`*0x00a79388`) wasn't read. |
| 15 | MenuBindings.cpp:1960 | SwiffPlayer `0x10010a60`: AudioSettings slot 0x24 (reset), then as `0x10010bd0`: canApply (slot 0x1c), apply(1) (slot 0x18), LocalProfileManager slot 0x34(1), the save (slot 0xc8) when applied | match (the two bodies are the same after the reset) |
| 16 | MenuBindings.cpp:2058–2077 | RendDX9 `0x1000f780`'s presets: low 1 1 1 0 0 1 2 1 / 800x600 / Off / 0.75; medium 2s, texture 3, 800x600, Off, 0.9; high 3s, 1024x768, 2Samples, 1; texture and high lighting by memory (`0x1000f600`, `0x1000f130` < 1100) | **match, one branch left out**: the high preset takes 800x600 instead of 1024x768 when RendDX9's global `DAT_101f8e58 + 0x102` is set. The comment doesn't mention this, and what the flag is wasn't identified. |
| 17 | Strategy.h, the box area | AIDLL `0x10062e10`: the first x/z into `+0x118` (the corner), then `0x100623c0(this, …, &second)` sets `+0x84` / `+0x8c` from the second (the position) | match. The isInside span (`0x1005d1c0`) wasn't read. |
| 18 | Hud.cpp / engine.md, the minimap's heading | `0x00752640`: atan2 (`fpatan`) of the camera direction, 0 unless the local player is alive (slot 0x74), into `0x0077b460` → `0x00778ea0` | match |
| 19 | engine.md / Hud.cpp, the minimap's turn | `0x0078f680`: only at the minimap's size (`+0x709`) with MapStatic off (`+0x7dc`), alive (slot 0x74), not man down (slot 0x114); `+0x7fc` eased by `exp(−9 dt)` (`0x00614830`); `+0x804`, `+0x800`, `+0x80c` after it; angles wrapped by 2π | match |
| 20 | VehicleParts.cpp:196–205, Vehicle.h:584 | `0x005c9ec0`: each of three inputs (`+0x36c`, `+0x368`, `+0x370`) gets clamp(((R_i·w)/dt × b + R_i·w) × a, −1, 1) with a = `+0x380` / `+0x378` / `+0x388`, b the next field; it replaces the input only when stronger the same way, or the input is 0 | match. The vertical regulator's 10°/20° fade matches too (`+0x398` / `+0x39c` cosines). |

Also read: SwiffPlayer `0x100a4c50`, the number of an as_value. A Number object (type 4) gives its slot 0x1c string through `atof`, and MenuBindings.cpp:1911's claim (a slider's `m_percent` object is its value) matches.

**All 20 match on the claims checked.** Notes:
- **#16 is incomplete:** the 800x600 fallback of the high preset is missing. It is minor, as the rewrite takes "today's machines" anyway. Worth a line in the comment once the flag is known.
- **#11, #14 and #17** were only partly followed, as noted.

## 5. Inconsistencies and measurement artefacts

1. **The before column is coverage-10's own output** for 4d7a8b7 (its scratchpad), not a fresh rebuild. The scripts are byte-identical, and the corpus and features were copied from it. The probe and rows reproduce coverage-10's numbers exactly in the unchanged columns.
2. **Wake_Island_2142's unknown-method count** went back from 7,103 to 7,104 (coverage-9's figure). This is the run's variance, as coverage-10 said; the other ten runs are identical.
3. **The radio's spam check in a local `--keys` run.** In the key sweep's local rounds (§6c), six rose entries in 22 s of game time tripped the spam block. The rewrite's `RadioSpam` clock is the wall clock (`steady_clock`), and a local `--keys` run steps its ticks faster than real time, so 4-second gaps in game time are well under the 6 s interval in wall time. The original's check (`0x00591d40`, the player's tick `0x0059bd80`) reads `0x00666070`, the game's timer. In a real-time game the two are the same, and the joined runs (real time) weren't affected. It is a harness artefact, but the rewrite's clock is not the original's: on a paused or slowed game the limits would differ.
   - The entry clicked at 25.3 s (the bottom one, "medic") was refused twice in that run: two "spam" voices and two personal lines. Its command and its button both reach the spam check, which looks like `callForMedic` also going through `sayRadio`'s check. Not followed further.
4. **Carried over:**
   - The no-audit classifier can't see uncited.md's classifications, so the (d) runs in §2a count as missing although each is classified.
   - Inert bindings count as implemented.
   - The start-up count includes the 158 host-only methods.
   - AIDLL's own console (282 commands) is in neither console table.
   - Ambiguous DLL addresses (199 → 215) are dropped, so the DLLs' cited counts are lower bounds.
   - The EXE's reused calibration factor is about 1.7 times the classifier's own.
5. **No regressions.**
   - No row went from impl to stub, and none from read to unread.
   - No citation went away except the reworded AIDLL line (§1a).
   - All 984 tests pass at HEAD.

## 6. The sweeps at HEAD

The three headless sweeps were run on the worktree's release build, each with a scratch profile copied from its own folder (`BF2142_CONFIG_DIR` under `build/scratch`). The game data was the main checkout's, through a symlink.

### 6a. Kit and vehicle sweep (`build/scratch/sweep/sweep.py`, `-j 6`)

**190 / 200 ok, 10 n/a, 0 failing.** This is the same result, row for row, as the main checkout's last run (`build/scratch/sweep/results.txt`). By kind:
- 49 guns, 16 melee, 16 thrown, 10 placed, 32 deploy, 2 camo, 4 hubs, 8 tools, 54 seats;
- 9 effect runs: medic hub 52 → 100 hit points, ammo hub to a put-away MG (3 → 4 magazines), repair tool, C4 on a vehicle, guided AV (with the auto reload), active camo, the shield stopping 9 of 10 rounds, the sentry shot down, the air transport's pod.

The 10 n/a are the eight battery tools (repair, defibrillator, defuser, pulse meter for both factions), which have nothing to act on at the test spot, and the two FAVs' first seat (a horn).

### 6b. Menu and HUD sweep (`build/scratch/menusweep/sweep.py`)

**47 checks, 0 failed.** The main checkout's last results had one failure, the audio page's reset to default. dae4d74 fixed it (`0x10010a60`), and it passes here. The checks cover:
- the game, HUD, video, audio and controls pages: each control's value reaching the profile and coming back, the presets' qualities, the sliders, the resets;
- the singleplayer lists, playlist and instant action;
- the LAN list on loopback (a host on 127.0.0.1 for 90 s) and the create tab;
- the in-game HUD, scoreboard, Server tab, spawn and commander screens;
- the profile's HUD transparency in the HUD.

### 6c. Key sweep (`--keys`; engine.md "The roses, chat and votes headless")

The key sweep has no single script. dae4d74's runs were `mp.sh` (a host and up to three clients on 127.0.0.1) plus key lines written by hand. They were rebuilt from those runs' logs as `run11.sh`, kept with the logs in `build/scratch/a11/keysweep/`. The level is Camp_Gibraltar gpm_cq; for the joined runs the host is team 1, Alice and Bob team 2, Carol team 2, or team 1 in the chat run.

| Run | What it does | Result at HEAD |
|---|---|---|
| L1, local | the Radio rose's 11 entries by clicks (`radio.keys`, the mouse areas of engine.md) | **11 / 11** entries were chosen and closed the rose. The first six were said, each heard and each with its chat row where its voice has a text (as in dae4d74's run, some don't). From the seventh on, the spam block refused them ("Shut up and ditch this channel", "Blocked for 31 second(s)"): §5.3, a harness artefact. |
| L2, local | the 11 entries by `RadioInterface.selectOrder 1..11` | **11 / 11** entries said ("Enemy forces in sight!", "Move it!", "Requesting a pickup!", …, "Medic needed", "Understood"); the spam block from the ninth (§5.3) |
| L3, local | the say boxes: team, all, squad (in a squad), then squad with Esc | **all four**: each box opens on its control; Enter sends `<TEAM>` / plain / `<SQUAD>` rows; Esc closes without a row |
| M1, joined chat | Alice's team, all and squad lines and three rose entries; the host's all and team lines | Alice's **team line reaches Bob, not Carol (team 1) or the host**. Her **all line reaches all three**. Her rose entries go by channel: "Not happening" and "Right-o" / "All right" reach Bob, while "Go, go, go!" (TCLocalTeam, 100 m) reaches no one. **The host's all line reaches all three clients, and his team line only Carol.** Alice's squad box didn't open ("no text box open"), as in dae4d74's own run: her console `SquadMenu.createSquad` at 20 s, four seconds before, left her without a squad on the client by then (not followed). |
| M2, joined votes | Bob applies for command and takes it; Alice's mutiny; Carol's kick from the scoreboard's Manage tab | Bob is "accepted as Commander" and takes the offer with c_GIYes. **Alice's mutiny asks Carol, Carol's yes passes it** (2 of 2) and Bob is demoted. Carol's kick (second row's box) gives Bob the local line with no question; **Alice's yes passes it**, and Bob's client is dropped (reason 21). |
| M3, joined map vote | Alice opens the Server tab and votes for the second map; Bob answers c_GIYes | **The vote opens and Bob's yes passes it** ("Map Cerbere Landing will now be loaded"). The host needs two maps (`--maps Camp_Gibraltar:gpm_cq:32,Cerbere_Landing:gpm_cq:32`): with the mod's one-map maplist the tab's second row is empty and nothing happens, which is what this audit's first try did. |

**Everything dae4d74 recorded as seen works at HEAD.** Two observations are not regressions:
- the local spam block (§5.3);
- Alice's squad box, which didn't open in dae4d74's run either.

## Raw outputs

In `build/scratch/a11/` (git-ignored):
- `head/`: HEAD dae4d74, the full `run_all.sh` output (probe, rows, delta, runs, citations, classifier labels, hedges);
- `before/`: 4d7a8b7, coverage-10's own output;
- `spot/`: the spot-check's decompiled functions; `fn.py`, `summ.py`, `summ2.py` and `runs_mk.py` (the lookups and the tables' summaries);
- logs: `head.log`, `head-rest.log`, `build_tests.log`, `tests.log`; the worktree's build log is `build/scratch/wt11-build.log`.

The sweeps' results, copied out before the worktree went: `build/scratch/a11/sweep-results.txt`, `menusweep-results.txt`, `keysweep/`.
