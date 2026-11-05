# BF2142 rewrite: eighth coverage audit (2026-11-05, HEAD cd98860)

This audit compares HEAD cd98860 with c1ecc19, the commit that added coverage-7. coverage-7 measured 455d4ec; c1ecc19 adds only coverage-7 itself and the demo-name extension fix (DemoCamera.h, Demo.cpp/.h). Two commits lie between c1ecc19 and HEAD:

- **8c99848**, the player-visible pass:
  - python.onRepairEvent and Titan part repairs;
  - joined clients' mech seats on bone parts;
  - the commander mini-map tab and scoreboard kick votes;
  - unsteered beacon pods;
  - `--language` with UTF-8 HUD text;
  - the AI commander answering requests, DICE's Mersenne twister for the bots, bot slot 8, the flank spots and SAI step 1 settled;
  - bundle start age and anchored bundles, the 60-frame unseen stop, the static decals' list;
  - the 37 stub rows bound as documented no-ops, addVehicleType fixed, the called template names classified;
  - cite.py now gives `src/Game/Bots*` to AIDLL.
- **cd98860**, the called-template-names pass:
  - Titan shields switching spawners by game event, Titans asleep at the start;
  - crew kits, rolling loops, toggleMouseLook, yaw stabilisation, the heavy IFV's shield cells;
  - arming delays, zoom while cycling, seek direction bonus, lock loss, proximity fuses, the hub's battery cost;
  - collision offsets and other-LOD meshes;
  - the seats' GUI index by camera mode, disableOnSprint, map icons by camera rotation, supply bars on tags;
  - the squad reports' reveal time and cloak, remote mech legs;
  - joined clients' remote seat guns;
  - texture suffixes, ground effect bundles, bundle sound intensity, road static meshes.

## How this was measured

The method and calibration are coverage-6's and coverage-7's, so the figures can be compared:
- the same scripts (`tools/coverage/`) and the same corpus (`calls.json`: 6,702 files, 816,424 calls);
- the same decompilation features and the same factors (EXE 0.84, RendDX9 0.69, AIDLL 0.48).

Both commits were measured from clean worktrees, so the work other agents had in progress in the main checkout couldn't leak in (its only uncommitted change at the start was `.mcp.json`):
- `tools/coverage/build_old.sh c1ecc19 …/wt-c1ecc19` and `build_old.sh cd98860 …/wt-head`, both built offline;
- `run_all.sh --repo <wt> --out …`, with HEAD's scripts and `BF2142_GAME_DIR` set to the main checkout's `source/Battlefield 2142`;
- HEAD's run took `--before` from c1ecc19's outputs;
- both worktrees were removed afterwards (`git worktree remove --force`).

**The cite attribution changed between the two commits.** 8c99848 changed `cite.py`'s `dir_module` so that `src/Game/Bots*` maps to AIDLL. Both rules are reported:
- **corrected** (HEAD's cite.py, on both trees): the headline;
- **old** (c1ecc19's cite.py, on both trees): run from a scratch copy, for the series.

The two rules differ only in AIDLL; the EXE and RendDX9 are byte-identical under both. Under the old rule, c1ecc19 reproduces coverage-7's printed AIDLL figures exactly (1,718 functions, 51.62%, calibrated 77.7%). Under the corrected rule it reproduces coverage-7 §5's corrected figures (1,769, 52.56%, 78.2%). The EXE has one function more than coverage-7 printed (3,300 against 3,299): that is c1ecc19's demo-name fix.

No window was opened:
- The probe and the level runs used `--simulate` (one probe mode used `--screenshot`) with `--nosound --no-movies`.
- The host probe used `--host --dedicated` without `--online`.
- The unit tests ran headless in HEAD's worktree.
- Nothing went beyond loopback.

**The headline figures leave out the audit reports** (`--no-audit-docs`, `uncited_noaudit`), as before. The coverage-3 rule (audit docs counted) is given for the series.

**Tools:** nothing was broken and nothing in `tools/` was changed. Every script ran clean. The SwiffPlayer pass in section 1c used a scratch wrapper around `uncited.py`, kept outside the repo (see "Raw outputs"). `tools/coverage/README.md` still lists the audits only up to coverage-6.

## 1. Headline numbers, before → after

| Metric | c1ecc19 | **HEAD cd98860** | Change |
|---|---|---|---|
| Probe, distinct names: impl / stub (union of `--simulate`, `--host`, `--screenshot`) | 1,134 / 1,554 | **1,179 / 1,509** | +45: the 37 inert rows bound as no-ops (InertRows.cpp), `Material.pythonEvent`, `setIfTitanChangeToWeapon`, `addVehicleType`, `texturemanager.customTextureSuffix`, and the four `*Hud.(add)showOnCamMode` |
| … `--simulate` alone | 976 | 1,021 | the same 158 names still answer only when hosting |
| Console rows (2,837): impl / stub | 1,143 / 375 | **1,189 / 332** | +46 / −43. Of the 43 stub → impl, 37 are documented no-ops, not behaviour (section 5). |
| … ObjectTemplate stored-read / stored-unread | 974 / 345 | **1,015 / 301** | +41 / −44. No row went from read to unread. |
| Unread template property names (lower bound) | 322 (249) | **281 (207)** | −41 (−42) |
| BF2142.exe rows impl / stub; RendDX9 | 880 / 250; 263 / 125 | 915 / 218; 274 / 114 | |
| Start-up "console methods stubbed": EXE / RendDX9 | 501 / 125 | 469 / 114 | still includes the 158 host-only methods |
| Stub rows called by shipped scripts / calls | 31 / 23,244 | **0 / 0** | |
| Unread rows called / calls | 182 / 32,102 | **142 / 31,650** | unique names: 169 / 31,693 → 132 / 31,283 |
| Level runs: "N calls to M unimplemented methods" | 5,639–6,556 / 22–24 | **0 / 0** in all 11 | |
| Python tracebacks (11 runs) / "Unknown object or method!" per run | 0 / 6,689–7,599 | 0 / 6,689–7,599 | identical but for Wake_Island_2142, 7,104 → 7,103 |
| Unit tests (`BF2142_GAME_DIR` set) | 804, 0 failing (coverage-7) | **856, 0 failing** | +52 |
| Hedge-note hits (`hedges.py`) | 237 | **231** | By text, 13 gone and 7 new (section 3) |

The 39 template names that went from unread to read (`delta.md`) are cd98860's rows:
- **Vehicles and seats:** `automaticYawStabilization`, `toggleMouseLook`, `crewKitIndex`, `groundContactVolume`, `boundingRadiusModifierWhenOccupied`, `lockToTeam`, `disablePodControls`, `blockInputOnRotBundle`.
- **Physics and collision:** `centerOfCollisionOffset`, `otherCollisionLod`, `isSleeping`, ForceObject's `rotationalForce` / `useAbsoluteDirection` / `targetPosition` / `enableOnStartMessage` / `relativeYCollisionGroupInverse`, `regulationTolerance`, `nameToLookAt` / `offsetToLookAt`.
- **Weapons:** `fire.allowZoomOnROF`, `seek.directionBonus`, `target.notifyProjectileOnLostTarget`, `detonation.detonateDistanceToTarget`, `ammo.abilityCost`, `maxTriggerDistance`.
- **Events and spawners:** `radio.destroyedGameEvent`, `deActivateOnEvent`, `python.onRepairEvent`.
- **HUD and tags:** `vehicleHud` / `weaponHud.disableOnSprint`, `vehicleHud.useVehicleCameraRotation`, `vehicleHud.AbilityIcon`, `weaponHud.showInfo`, `Nametag.showArmor` / `showSupply`, `armor.hudRepairCellNo` / `mirrorParentDamageLevel`, `ability.networkableHudRevealTime` / `ShowCloaked`.

### 1a. Address citations (start addresses; src/ and docs/, tests excluded)

**Headline: no audit docs** (`--no-audit-docs`), corrected attribution:

| Module | functions / bytes | c1ecc19, without appendix | **HEAD, without appendix** | with appendix |
|---|---|---|---|---|
| BF2142.exe | 35,589 / 4,794,004 | 3,300 fns, 1,339,637 B (27.94%) | **3,404 fns, 1,361,449 B (28.40%)** | 4,200 (30.13%) → **4,304, 1,466,061 B (30.58%)** |
| RendDX9_ori.dll | 10,810 / 1,677,614 | 1,263, 429,401 B (25.60%) | **1,303, 441,038 B (26.29%)** | same |
| AIDLL.dll (corrected) | 6,080 / 1,006,188 | 1,769, 528,819 B (52.56%) | **1,790, 538,017 B (53.47%)** | same |
| AIDLL.dll (old attribution) | | 1,718, 519,362 B (51.62%) | 1,742, 529,100 B (52.58%) | same |
| SwiffPlayer.dll | 5,756 / 936,991 | 180, 127,973 B (13.66%) | 180 (13.66%) | same |
| BF2VoipServer.dll | 377 / 38,482 | 34, 10,976 B (28.5%) | 34 | same |
| BF2142Audio / Memory / TextureAtlasBuilder | | 0 | 0 | 0 |

- **Gain:** EXE +104 functions / +21.8 KB (+0.46 points), RendDX9 +40 / +11.6 KB (+0.69), AIDLL +21 / +9.2 KB (+0.91; +24 / +9.7 KB under the old rule). This is much smaller than coverage-7's gain. Both commits are mostly gameplay rows whose executors were already counted by the console-row rule (section 1b).
- **The attribution fix** adds AIDLL 51 functions / 9.5 KB at c1ecc19 and 48 / 8.9 KB at HEAD. The gap between the rules is steady, so before/after is the same under either.
- **Ambiguous DLL addresses dropped** (no audit docs): 167 → 167 (old rule: 251 → 250). The 167 that are left are EXE-side files with DLL-looking words (Main's menu bindings, Nametags, Stats), as coverage-7 found.
- **The coverage-3 rule** (audit docs counted), corrected: EXE 3,612 / 30.04% → 3,717 / 1,459,227 B (30.44%); RendDX9 1,503 / 33.32% → 1,543 / 569,309 B (33.94%); AIDLL 1,944 / 58.67% → 1,967 / 593,798 B (59.01%); SwiffPlayer 185 → 186.
- **With tests/:** EXE 3,766 / 30.64%, RendDX9 1,543, AIDLL 1,967 (coverage-3 rule).
- **With `--headings`:** RendDX9 1,519 → 1,559 (34.34%), AIDLL 1,988 → 2,012 (60.05%).
- **Uncited runs, no audit docs:**
  - EXE (≥ 30 KB): 11 / 0.75 MB with the appendix, 12 without, unchanged.
  - RendDX9 (≥ 20 KB): 15 / 0.54 → **13 / 0.46 MB**. Gone: `0x1019fab0` (30 KB; split by the texture-suffix and frustum-sort citations, `0x1019f4c0` / `0x101a4720`) and `0x10179060` (27 KB, SwiffRender's fonts and languages, cited by the `--language` work, `0x1017af30`…). Shrunk: `0x1016d0f0`, name tags, from 38 to 21 KB.
  - AIDLL (≥ 20 KB): 1 / 0.02 → **0**. `0x1002ae90` was split by the citation of `0x1002eff0`.
  - SwiffPlayer (≥ 20 KB): 7 / 0.49 MB, unchanged.

### 1b. Effective coverage (the uncited classifier, `uncited.py`)

The definitions are coverage-4's:
- **Needed** is the total minus (a) third-party and (b) dev-only code.
- **Lower** is cited plus string- or console-evidenced (c).
- **Calibrated** adds propagated (c) × the factor and splits "?" pro rata.
- **Upper** counts all (c) and all "?".

**Headline: no audit docs, corrected attribution** (`uncited_noaudit`, appendix counted):

| Module | Needed | Cited, of needed | Lower | **Calibrated** | Upper | (d) calibrated | Raw (d) |
|---|---|---|---|---|---|---|---|
| BF2142.exe | 3.69 → 3.70 MB | 39.1 → 39.7% | 53.9 → **54.9%** | 84.3 → **85.0%** | 90.1 → 90.8% | 0.578 → **0.553 MB** | 355 → **333 KB** |
| RendDX9_ori.dll | 1.24 → 1.25 MB | 34.6 → 35.4% | 42.8 → **43.6%** | 76.5 → **77.0%** | 91.3 → 91.7% | 0.293 → **0.287 MB** | 105 → **100 KB** |
| AIDLL.dll | 0.88 MB | 60.2 → 61.3% | 61.5 → **62.6%** | 78.2 → **79.1%** | 95.8 → 96.7% | 0.192 → **0.183 MB** | 35 → **27 KB** |
| All three | 5.81 → 5.82 MB | 41.3 → **42.0%** | 52.7 → **53.6%** | 81.7 → **82.4%** | | 1.062 → **1.023 MB** | 495 → **460 KB** |

- **Old attribution (series with coverage-7's main table):** AIDLL 60.6 / **77.7** / 95.8% → 61.7 / **78.7** / 96.7%, (d) calibrated 0.196 → 0.187 MB; all three 52.5 / **81.6%** → 53.5 / **82.3%**. The EXE and RendDX9 are as above.
- **How to read it:**
  - Raw (d) fell by 35 KB (EXE 22, RendDX9 5, AIDLL 8) and the upper bounds rose 0.4 to 0.9 points. That is newly settled or new code.
  - The calibrated gain (+0.7 points overall) is a third of coverage-7's. Most of what the two commits implemented was template rows whose executors the console-row rule had already labelled: as (d) when unread, and in tiny functions. Reading those rows moves executors of a few dozen bytes each from (d) to (c).
- **The coverage-3 rule** (`uncited_with`), for the series:
  - EXE 55.9 / **86.1** / 91.8 → 56.8 / **86.7** / 92.4%, (d) 0.515 → 0.492 MB, raw 295 → 275 KB;
  - RendDX9 49.4 / **81.7** → 50.1 / **82.1%**, raw 48 → 45 KB;
  - AIDLL 67.9 / **82.6** → 68.3 / **82.9%**, raw 13 → 11 KB;
  - all three 56.3 / **84.6** → 57.1 / **85.2%**, 0.90 → 0.87 MB.

  (coverage-7 printed 86.1 / 81.6 / 82.1 for the EXE / RendDX9 / AIDLL at 455d4ec. The c1ecc19 column differs because this rule counts coverage-7.md itself, which c1ecc19 added, and because HEAD's cite.py gives the Bots files to AIDLL.)
- **With `--headings`:** all three 84.8 → 85.4%; RendDX9 82.1 → 82.5%, AIDLL 83.3 → 83.7%.
- **This classifier's own propagation precision** (`uncited_noaudit_recal`):
  - factors EXE 0.49 → 0.49, RendDX9 0.74 → 0.74, AIDLL 0.35 → 0.41;
  - calibrated: EXE 72.2 → 73.0%, RendDX9 78.8 → 79.5%, AIDLL 73.8 → 76.9%, all three 73.9 → 75.0%.

  The EXE's reused factor (0.84) is still about 1.7 times this classifier's own. Read the absolute EXE figure as soft and the before/after as firm.

### 1c. SwiffPlayer.dll: a first classification pass

coverage-7 §3b-5 asked for an effective figure for the Flash menu player. `uncited.py` covers only the other three modules, so the pass ran its `classify` / `effective` / `d_runs` unchanged from a scratch wrapper (`swf_classify.py`, see "Raw outputs"), with these SwiffPlayer-specific inputs.

**The module's layout** (from its strings, calling conventions and `source_files.csv`, which names `BF2/External/gameswf/SwiffPlayer/Natives.cpp`):

| Range | Size | What it is | Class |
|---|---|---|---|
| `0x10001000`..`0x100836f0` | 479 KB | SwiffHost and its natives: the `dice.*` objects the menus call (GeneralSettings, VideoSettings, Profile, Multiplay, the server browser, BFHQ stats, EndOfRound, …) | needed |
| `0x100836f0`..`0x100a1990` | 111 KB | GameSpy HTTP / SOAP (`GameSpyHTTP/1.0`, `SOAP-ENV`), voice device enumeration, base64, and the Speex codec (`nb_ctl`, "No pitch prediction") | (a) |
| `0x100a1990`..`0x100e0800` | 227 KB | gameswf itself: the AVM1 actions and built-ins (String, Key, Mouse, XML, MovieClip), the tag loaders, shapes, text, fonts, sound tags | needed |
| `0x100e0800`..`0x100f2020` | 55 KB | expat (its API `0x100e0a20`… XML_GetErrorCode / GetCurrentByteIndex / LineNumber, `xml=http://www.w3.org/XML/1998/namespace`, PCDATA / ATTLIST) | (a) |
| `0x100f2020`..`0x100f3000` | | the CRT | (a) |
| `0x100f31c0`, `0x100f8000` | 2 × 17.6 KB | two country-table builders (≈ 230 country names) | needed (online account, below) |

There is no zlib (gameswf's "TU_CONFIG_LINK_TO_ZLIB is 0") and no statically linked libjpeg.

**Rules changed from the other modules'**, and why:
- The library words are only the codec and parser words (Speex, expat, GameSpy HTTP). The general list's 'plasma' and 'punkbuster' would put the menus' own server-browser natives in (a), and they are 34 KB of DICE code.
- 'profil' became 'profiler': the old word put every native that names `LocalProfileManager` in (b).
- gameswf's own debug output is (b): the action disassembler, the timers, the loaders' verbose logs.
- src/UI/Avm1 is in the source corpus (the other modules leave it out as noise).
- The factor is the pass's own `--recalibrate` measurement. No earlier figure exists to reuse.
- Strings: the headline counts identifier-like strings only. A variant with every string is given beside it. The rewrite's player is a new implementation of the format: it has gameswf's method and property names (`gotoAndPlay`, `onRelease`, `fromCharCode`) but not its log and error text ("error: line_to needs two args", "shape_loader: id = %d"). With every string counted, the loaders and built-ins that the rewrite plainly has come out as (d): DefineFont2, sprite_goto_and_play, the morph fills, set_mask, createEmptyMovieClip / lineTo. Those all exist in src/UI.

| SwiffPlayer.dll | Needed | Cited, of needed | Lower | **Calibrated** | Upper | (d) calibrated | Raw (d) |
|---|---|---|---|---|---|---|---|
| identifier strings (headline), c1ecc19 → HEAD | 0.73 MB | 17.5% | 28.8% | 62.1 → **62.4%** (own factor 0.55) | 87.1 → 87.2% | 0.28 MB | 92 → 91 KB |
| every string (variant), HEAD | 0.74 MB | 17.3% | 28.5% | 43.9% (factor 0.42) | 63.4% | 0.42 MB | 264 KB |

- The two string rules bracket the figure at **≈ 44–62%**, and the identifier rule is the closer of the two. That is about 20 points under the other modules. It is also the softest figure in this report:
  - only 17.5% of the needed code is cited;
  - 382 KB of the (c) is propagated;
  - nobody has hand-checked a SwiffPlayer label before this pass.
- **A cross-check on the natives.** NativeTable.cpp lists all 702 native members. A src binding was found for about 418 of them (60%), by literal member name or by the settings key the rewrite composes (`"VideoSettings.TerrainQuality"`). This is a lower bound, because bindings built from other strings aren't found.
  - Complete or nearly so: dice.bf2.Sound, Locale, MessageHandler, List, EndOfRound (58 / 61) and ControlSettings (20 / 24).
  - Unbound, almost all online: BuddySystem (0 / 12); dice.General (0 / 13: the server lists' revisions, filters and HTTP downloads); Mod (1 / 13: mod switching); Multiplay 22 / 80 (favourites, the map list, the server ids); Profile 54 / 93 (the online account, search and compare); PlayerStats 2 / 5 (the leaderboards).
  - Also unbound: Cursor (0 / 3) and Singleplay (0 / 3: chooseMission / get- / setBotSkills). The README's menu walk does reach Singleplay, so either they are bound through strings the check doesn't see or the menu manages without them. This wasn't traced.

### 1d. SwiffPlayer.dll: the second pass (folded into `uncited.py`)

§3b-5 is done. `tools/coverage/uncited.py` now measures SwiffPlayer like the other three modules. Its own inputs are in `tools/coverage/swf.py`; the method is in tools/coverage/README.md. In short:
- hand-checked library ranges;
- the menu natives' bodies labelled by uncited.md's natives table, as the EXE's console rows are;
- hand-checked single functions: replaced by design, online-only, or never reached by the shipped movies;
- two buckets left out of the needed code like (a) and (b): (o) online-only and (u) unused;
- a fixed factor of 0.83, this classifier's own measurement.

The pass also implemented what the shipped menus show or do and the rewrite didn't, and cited src/UI's existing counterparts of gameswf. Details are in uncited.md, "SwiffPlayer.dll: gameswf and the classifier":
- the list rows' onDoublePress;
- text fields' `_focus`, `tabIndex` with Tab / Shift+Tab and `Logic.enableTab`, `restrict` and `_maxrows`;
- the create-server page's GAME MODE drop list, which was empty, and the bot skill select box.

| SwiffPlayer.dll (no audit docs) | Needed | Cited, of needed | Lower | **Calibrated** | Upper | (d) calibrated | Raw (d) |
|---|---|---|---|---|---|---|---|
| first pass (§1c, scratch wrapper, factor 0.55) | 0.73 MB | 17.5% | 28.8% | 62.4% | 87.2% | 0.28 MB | 91 KB |
| **`uncited.py`, this tree** (factor 0.83) | 0.62 MB | 39.1% | 52.1% | **91.3%** | 99.3% | 0.05 MB | 4.4 KB |

- Left out of the needed code: (a) 193 KB, (b) 4.5 KB, (o) online-only 105 KB, (u) unused by the shipped movies 10 KB.
- With the first pass's factor (0.55), the calibrated figure is 78%. The two factors bracket it at **≈ 78–91%**, against §1c's ≈ 44–62%.
- Citations without audit docs went from 160 KB to 241 KB.
- No (d) run of 2 KB is left. The 4.4 KB that remain:
  - the nine missing natives;
  - the server info rows (SERVERINFO_*; LAN servers only, offline);
  - the `#HH#:#MM#` time cells of the stats and end-of-round rows.
- The figures for the EXE, RendDX9 and AIDLL are unchanged by the tool change: run on the same inputs, they match the previous `uncited.py` digit for digit. Don't add SwiffPlayer to the three-module total.

## 2. The largest remaining unrewritten or unclassified runs

### 2a. Missing ((d) runs, no audit docs, ≥ 2 KB)

The counts are runs / KB:

| Module | c1ecc19 | **HEAD** | Gone since |
|---|---|---|---|
| BF2142.exe | 16 / 81.1 | **16 / 80.8** | none; `0x0066b175` and `0x0047f5e0` shrank a little |
| RendDX9_ori.dll | 12 / 38.1 | **11 / 35.8** | `0x100118d0` (VB dump totals, cited) |
| AIDLL.dll | 4 / 17.6 | **3 / 12.0** | `0x1002db50` and `0x1002eff0` (the bot and SAI debug printers): `0x1002eff0` cited, and the rest re-bounded as `0x1002e180` (3.4 KB) |
| SwiffPlayer.dll (section 1c) | 13 / 80.5 | **12 / 78.5** | |

The EXE's, RendDX9's and AIDLL's runs are those of coverage-7 §2a, all classified in uncited.md or coverage-6's run tables (the no-audit classifier doesn't read those files, section 5). By size:
- EXE `0x0047f5e0` (13.1 KB, BFHQ stats fields): online-only.
- EXE `0x006e8570` (11.8 KB, the object grid's tree build): replaced by design.
- EXE `0x00683e40` (10.0 KB, IDKey_ / IDButton_ names): replaced by design.
- EXE `0x0066b175` (6.3 KB, `Console.getArgumentTypes`): dev.
- RendDX9 `0x100ce6c0` (6.3 KB, `terraindata.raw`): (b).
- EXE `0x006ee282` (5.8 KB, `Object.initGrid`…): editor.
- EXE `0x0070cb00` (5.5 KB): the G15 LCD.
- AIDLL `0x10052fe0` (4.9 KB, IDPManager): (b).
- EXE `0x004196a4` (4.9 KB, `SquadManager.list*`): dev.
- … down to the 2.0–2.6 KB unset template rows (respawnDelay, cameraSway*, regulateYaw, castsStaticShadow).

**None of them is behaviour that a player would miss.**

SwiffPlayer's (d) runs (first pass, identifier rule). None of them was in an earlier table:

| # | Run | Size | What it is | Assessment |
|---|---|---|---|---|
| 1 | `0x100f31c0`..`0x100ffe2d` | 39.1 KB | two country-table builders (`United States`, `US`, …, ≈ 230 names) | the online account's country (Profile.createOnlineAccount is unbound); online |
| 2 | `0x10014ef0`..`0x10016247` | 4.8 KB | the control id → `HEADER_<action>` name map (HEADER_forward = 3, …) | the controls screen's action names. The rewrite binds mapInput / getMappedString differently (MenuBindings.cpp:2093), so probably replaced. Not verified. |
| 3 | `0x100760a0`, `0x10077c90` | 4.1 / 3.9 KB | server-browser rows (`images/list/ServerMod.png`, the mod, PB, ranked and password icons) | the server list's icons; partly online. Check against the rewrite's LAN list. |
| 4 | `0x1000acd0`..`0x1000c276` | 4.0 KB | `BF2.HTTPRequest.Post`, the `drivers\etc\hosts` check, ghttp saves | online (news, HTTP) |
| 5 | `0x100426a0`..`0x1004360c` | 3.8 KB | `BFHQ_leaderboardscores`, the favourites' `serveralreadyfavorite` / `invalidip` | leaderboards online. **Adding a favourite server by IP** is offline-capable and unbound (Multiplay.addFavouriteServer). |
| 6 | `0x1006db20`, `0x1007d000`, `0x10071120`, `0x1007ada0` | 3.7 / 3.4 / 2.4 / 3.1 KB | BFHQ's stats pages: weapon and vehicle names (KILLMESSAGE_*, NAME_UNLOCK_*), time format `#HH#:#MM#` | BFHQ is online (out of scope) |
| 7 | `0x1000e4e0`..`0x1000f58a` | 3.1 KB | string conversions (`no_conversion_exists`, formatString's overflow warning) | plumbing |
| 8 | `0x100b9950`..`0x100ba5a2` | 3.0 KB | gameswf's `.gsc` font and character cache files | replaced by design |

### 2b. Largest uncited runs (no audit docs, without the appendix), with the classifier's makeup

The KB values are the classifier's labels inside the run: a = third-party, b = dev, c = evidenced, c-p = propagated present, ? = unsure, d = missing.

**EXE** (≥ 30 KB): the same 12 runs as coverage-7 §2b, none changed.
- (a): FESL / EASTL `0x00840340` 173 KB; the FESL browser `0x008155c0` 128 KB; GameSpy `0x007380a0` 79 KB and `0x0072a180` 49 KB; FESL account fields `0x008025b0` 69 KB; static init `0x008c970a` 39 KB.
- (b): the profiler's XML `0x0065d840`, 30 KB.
- The (c) blocks that could take citations: the HUD's font, QuadDrawer and Ref2System `0x007c9090`, 52 KB (c-p 27, ? 20); the HUD map node's rows `0x00760a26`, 48 KB (c 30, d 8, cited only by the appendix); file paths `0x00696f80`, 38 KB; dice::meme::Effect `0x00870c90`, 36 KB (c-p 20, d 8, d-p 7); the settings managers `0x0046c880`, 35 KB.

**RendDX9** (≥ 20 KB; 13 runs):

| Run | Size | Makeup (KB) | What it is |
|---|---|---|---|
| `0x101885b0` | 84 KB | b 78 | debug drawers, the ported menu shader |
| `0x10053e00` | 58 KB | a 58 | IGA (re-bounded from `0x10053c80`) |
| `0x101543a0` | 48 KB | c-p 29, b 10 | the terrain editor |
| `0x10101310` | 36 KB | b 35 | RoadEditable |
| `0x1001ddf0` | 28 KB | c-p 16, c 6, a 5 | the effect loader and its state cache |
| `0x10041b10` | 26 KB | c-p 9, c 6, a 6 | shader statistics |
| `0x100106b0` | 25 KB | c-p 16, a 5 | display modes |
| `0x1002eb90` | 25 KB | c-p 7, c 7, d-p 6 | the shader defines |
| `0x100c6570` | 24 KB | c-p 10, d-p 8 | the bundled mesh's shadow map |
| `0x100df060` | 24 KB | c-p 20, a 4 | |
| `0x100cd220` | 23 KB | b 7, c 6, d-p 5 | TerrainEditable |
| `0x10092990` | 23 KB | c-p 15 | the rest of the overgrowth |
| `0x1016d0f0` | 21 KB (38) | c-p 13, a 6 | the name tags, the specular LUT |

**AIDLL**: none ≥ 20 KB.

**SwiffPlayer** (≥ 20 KB; 7 runs, classified as in section 1c):
- `0x10080eb0`, 194 KB: the tail of the natives, GameSpy and Speex, the start of gameswf (a 112, c-p 53, c 15);
- `0x100c6f00`, 91 KB: gameswf's String, drawing, sprite, XML and font code (c-p 67, c 18);
- `0x100e00b0`, 53 KB: expat (a 52);
- `0x100212b0`, 51 KB: the server-browser natives (mostly c-p and ?);
- `0x100f1060`, 44 KB: the country tables (d 34);
- `0x10068030`, 30 KB: the server info and BFHQ pages (c-p 19, d 6);
- `0x10009ac0`, 22 KB: the menu host (c-p 12, d-p 6).

## 3. What remains to call the rewrite complete

Since coverage-7:
- coverage-7's P2, P3, P4, P5, P6, P8 and most of P7 are done (8c99848, cd98860; uncited.md's "player-visible pass" and "guns' fire pass").
- No called stub row is left: the level runs print "0 calls to 0 unimplemented methods".
- Every called template name is classified in uncited.md.
- The hedges that went: the bone part's mount, the per-instance time, the static decals' second list, destroyedGameEvent, the pod's input mask, the AI commander's answers, the twister seed, the flank-spot stand-in, start age / sound intensity, the bots' U[0,1) stand-in, the strike spots' 15 m stand-in, PCO slot 0x178, and the spawn-point activation by event.
- The hedges that arrived: the shield cells' own collision (engine.md:1287, SeatBehaviours.cpp:256), the commander marker's height (engine.md:10882), an untraced +0x250 state 6 (GameWorld.cpp:10707), InertRows' stand-in note, and two rewordings (engine.md:1999, :11983).

**S** = days, **M** = 1–3 weeks.

### 3a. Behaviour gaps a player could notice

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| P1 | **Bot squads' effectiveness, unverified against the original** (unchanged) | Squad bots on Cerbere_Landing gpm_sp 16 score 13.3 kills a round with the formation as decoded, against 19.4 when every member heads for the order. | engine.md "Squads". Settled since: flank spots, SAI step 1, bot slot 8, the twister. The danger list's writer is still unfound. Only a side-by-side with the retail game can settle it. | M (verification) |
| P2 | **The menus' unbound natives that work offline** (by section 1c's name check; not traced in the menus) | Adding a favourite server by IP (Multiplay.addFavouriteServer / removeFavouriteServer, `0x100426a0` …); the map list editor for a local server (setMapList, addMapToMapList, moveMapInList); the cursor natives; mod switching (dice.bf2.Mod, 1 / 13). Possibly Singleplay's chooseMission / botSkills (section 1c). | section 1c's natives check; SwiffPlayer runs 3 and 5 | S–M |
| P3 | **The heavy IFV's shield cells** | The cells can't be shot (their meshes aren't the vehicle's). The HUD doesn't draw them, and their damage-assist award is missing (since: the event 0xb is team damage, ECGameLogicGLTeamDamage, done: engine.md "Team damage"). | engine.md:1287; uncited.md "Template names: gaps" | S |
| P4 | **Remaining joined-client items** | The remote vehicle guns' own rounds on a client. The soldier's jump hold in the sprint test (`+0x334` taken as 1). | uncited.md "The guns' fire pass", "Left" | S |
| P5 | **Small HUD, map and seat items** | The mech's foot animation on the HUD (engine.md:7542). The sentries and pods aren't on the map (`0x00793b60`). The commander marker's height from its geometry (engine.md:10882). The parachutes aren't simulated bodies, so rotationalForce / useAbsoluteDirection act on nothing shipped. The bots' kit pool's slot-39 flag (engine.md:13015). | as cited | S each |
| P6 | **The controls screen's action names** | Whether the key-binding list shows the same `HEADER_*` labels as the original's control id map (SwiffPlayer `0x10014ef0`). | section 2a, SwiffPlayer run 2 | S (check) |

### 3b. Bookkeeping and plumbing (no visible behaviour)

1. **Unread template rows.** There are 281 names, 132 of them called (31,283 calls). The called ones are now all classified as inert, editor or settled in uncited.md or engine.md, except a handful of uncalled names in neither. The bulk of the calls are still the editor flags `isNotSaveable`, `saveInSeparateFile` and `createdInEditor` (25,000).
2. **The console counts include inert bindings.** 37 of the 43 stub → impl rows are no-ops (`ConsoleMethod::stub` stand-ins). The probe's impl count and the level runs' empty stub lists now mean "answered", not "does something". Each of them is settled in uncited.md.
3. **Citations of present code.** The largest (c) blocks without a citation:
   - EXE: `0x007c9090`, `0x00760a26`, `0x00696f80`, `0x00870c90`, `0x0046c880`;
   - RendDX9: the terrain editor's runtime half, the effect loader, display modes, shader defines and statistics, the overgrowth, the shadow map;
   - SwiffPlayer: gameswf's core (227 KB, 7 KB cited).

   Citing them moves the effective figures toward their upper bounds (EXE 90.8%, RendDX9 91.7%, AIDLL 96.7%, SwiffPlayer 87%) without changing behaviour.
4. **Hedges and stand-ins** (231 hits). The ones that matter for exactness are unchanged:
   - the soldier's box (0.7 × 1.85 m; GameWorld.cpp:32 and 3555, engine.md:8294);
   - `Projectile::lyingOn` (Weapons.h:714);
   - the overhead camera's speed factor;
   - the bots' seed when the script gives none (`0x2545f491` in place of the clock, Bots.cpp:758, where `0x100309a0` takes the clock × 10⁶).

   Since (2026-11-05; `hedges.py` 241 → 227 with the other changes then in the tree): the soldier's box is Soldier_CollisionMesh's LOD 1 (0.8 × 1.8 m, `0x006e4880`); the mine's frame follows the face once stuck (`0x005e83d0`); the bots' seed comes from the game clock (`0x0089f470` → `0x0065c270`) except under `--simulate` and the tests; the order marker's radius and the vehicles' tag height are RendDX9's instance radius (`0x100c7000`); the order tag stands by its marker (`0x005b8b20`); BBRevive skips drowned bodies (`0x10082760`); `lyingOn` is reworded as settled. The overhead camera's speed factor is unchanged.
   Then (2026-11-05, second pass; `hedges.py` 227 → 215 raw, of which `hedges.py` now drops 41 as false positives — log and error texts, quoted messages, prose "left:", the ballistic solver's "first guess", runtime "isn't known here", the names of deliberate stubs — leaving 174 real): the assist behaviours' upright test (dot of the target's up and the terrain normal ≥ 0.7071, `0x10070200` / `0x10071a10` / `0x10073360`); BBChange's room ahead of a fixed-wing seat (`0x1006c3e0`: the AG gunships' second positions; the box `0x100ff690`, the information environment's slot `0x2c` `0x100caf10` → `0x100f8b00`, the filter `0x1006c040`); the parachute's room (`0x005e45e0` / `0x006f2760`: the round's collision mesh at its start against the soldier LODs); the HUD's corner roots from the "Ingame" MemeFile (−1, 563 and 401, 563; the Animate pair under BfTransformNodes `0x007ed800`); the overhead camera's speed factor settled as 1 (`0x005f8870`, no other writer); the vehicles' tag height at the LOD the camera's distance picks (`0x100c3390` → `0x100c3260`); the soldier's friction per contact from Human_body (0x18, `0x006e4880`) and the surface (`0x006c24b0`, MaterialManager slot `0x88`); drawOrder as the sprite systems' two batch lists, Back drawn first, each system back to front (`0x10130bd0`, `0x10131b70`, `0x1012f800`); CreateSpawnGroup's bits and bytes (`0x00632e50`, `0x006329c0`: the group's flags and map place); KickBan's byte (always 0, `0x00505fa0` / `0x00506050`).
5. **SwiffPlayer: a second pass.**
   - Hand-check the propagated (c) of gameswf's core.
   - Add `swf_classify.py`'s ranges and rules to `uncited.py`, so later audits measure the module without a wrapper.
   - Classify the server-browser natives (`0x100212b0`, 51 KB) as online or LAN.

### 3c. Out of scope (unchanged)

- the online back end (FESL / EASTL, GameSpy GP / HTTP / SOAP, BFHQ and its stats pages, leaderboards, buddies, the online account and its country list);
- PunkBuster, IGA, SecuROM;
- the lightmapper and TextureAtlasBuilder;
- the editors;
- the Novint Falcon and the G15 LCD;
- the AI's CPU budgets.

### Priority, overall

| Rank | Item | Kind |
|---|---|---|
| 1 | P1: the bot squads checked against the retail game | behaviour / verification |
| 2 | P2: the offline menu natives (favourites by IP, the local map list, Mod, Cursor; check Singleplay) | behaviour (menus) |
| 3 | P3: the shield cells' collision, HUD and award | behaviour |
| 4 | P4: the remote guns' rounds on a client, the sprint test's jump hold | behaviour (joined) |
| 5 | P5 + P6: the small HUD, map and seat items; the controls screen's names | behaviour, low |
| 6 | 3b-5: SwiffPlayer's second pass, folded into `uncited.py` | bookkeeping |
| 7 | 3b-4: the soldier's box, lyingOn, the bots' default seed | exactness |
| 8 | 3b-3: citations of present code | bookkeeping |

## 4. Citation spot-check (citations added since c1ecc19)

`git diff c1ecc19..cd98860 -- src` adds 474 lines with addresses in 68 files. Seventeen citations were checked across the four modules. Each was read against `build/analysis/<module>/decomp`, against `symbols/BF2142.exe.template_properties.csv` for template rows, and against `vtables.csv` for slot numbers.

| # | Citation | Claim | Result |
|---|---|---|---|
| 1 | AI/Random.cpp:62–78 | `0x10046770`: refill past 623; y ^= y >> 11; (y & 0x56b64a) << 7; (y & 0xffffb716) << 15; y ^ y >> 18 | match (index `+0x9c4`, refill `0x100308f0` past 0x26f) |
| 2 | AI/Random.cpp:9–41 | `0x10030810`: two Schrage steps (0x5e30, then 0x661f) with signed shifts; `0x100309a0`: the seed's low 31 bits, 624 words, index 624 | match. The original takes the clock × 10⁶ when the seed is 0; the rewrite's Bots.cpp:758 constant stands in for it, as its comment says. |
| 3 | Effects.cpp:654 | unseen for 60 frames (`0x101403b0` / `0x10144d90`): no emission, the emit clock stays behind | match (frame counter > `+0x124` + 0x3c → return before the emission) |
| 4 | Effects.cpp:2081 | modifier 4 = `0x00618d00` | match (the bundle update's switch: case 4 → `0x00618d00`; case 5 clamps into `+0x1d8`, the sound intensity) |
| 5 | Effects.h:340–343 | `0x00618d00` calls each system's slot 0x80; RendDX9 `0x1013ee80` stores `+0xd8`, `0x10143ee0` `+0xe0`; the birth `0x1013fdc0` reads it | match (vtable `0x101d9170` slot 32 = 0x80; `0x1013fdc0` reads `+0xd8`) |
| 6 | Bots.cpp:4743–4752 | bot slot 8 `0x10098200` (vtable `0x10118050`): the left object's AI `+0x14` × 0.75, the new one's × 1.3333334, then `0x10095120`, vt+0xd0, `+600` / `+0x25c` / `+0x260` cleared, `0x10097e60` | match, every step |
| 7 | BotsScript.cpp:1046–1051 | ai.addBot's body `0x10028c00`: −1 without an SAI, a team outside 0..2, an unknown equipment ("NoEquipment" is none) or personality (`+4`'s map); else the count less one after `0x100580c0` | match (`DAT_101394bc`, `DAT_10139500` slot 8) |
| 8 | GameWorld.cpp:10806 | blockInputOnRotBundle: GenericFireArm `+0x324`, executor `0x004e1d2b` | match (csv) |
| 9 | Weapons.h:227 | centerOfCollisionOffset: SimpleObject `+0xa4`, executor `0x0051e654` | match (csv; Particle's is `+0xe8`, `0x00532038`) |
| 10 | GameWorld.cpp:10347 | crewKitIndex: the enter `0x005cdd60` takes the seat's `+0x47c` when not −1, through `0x0056cf50` | match (through component 0xc4d7's slot 0x58) |
| 11 | GameWorld.cpp:11825–11831, Titans.cpp:640 | `0x005f6ba0`: template `+0xf1` bit 1 → sleep (slot 0x150); on the server otherwise wake (slot 0x160); 0x2 mobile, 0x4 collision | match (the sleep / wake applies to mobile nodes, as the comment says) |
| 12 | Hud/HudFont.h:62 | `0x00405360`: the registry's Language, "English" by default, kept as GSLanguage | match (also: strings.csv in that language, else English). The fonts' folder isn't in this function and wasn't followed. |
| 13 | HudMenusVehicles.cpp:1021 | `0x00657a70`'s first test: with the sprint argument, slot 0x13c set → 0 | match on the test. That slot 0x13c is disableOnSprint's getter wasn't followed (the row's executor `0x00554952` uses the component's slots 0x74 / 0x78). |
| 14 | Render/TextureSuffix.h | `0x10018ca0`: with `+0x114` set, each texture: no suffix → reload a swapped one; else base name (`0x1019f4c0`) + "_" + suffix + ".dds" via `0x10017f20(…, 0, 0)`; "rad_" names skipped; fallback to the own name | match (`0x1019ee40` is a thunk to `0x1019f4c0`) |
| 15 | World/InertRows.cpp:30–33 | heightmap slot 0x24 (executor `0x006b3a5f`), vtable `0x009949f8` slot 9 `0x006d3a50` writes `+0xf0` | match |
| 16 | Weapons.h:546 | `0x005d3c40`: busy when the fire state (slot 0xd8) is above 1 and the fire template's `+0xb4` (allowZoomOnROF) is 0 | match (csv: allowZoomOnROF `+0xb4`, component 0xc4f1) |
| 17 | main.cpp:5042 | the mini-map tab `0x007e7d00`, button 1: the commander's camera to the clicked point (slot 0x398) | match (it also sets camera mode 0x10 through slot 0x224, which the rewrite's `sendCommanderCam` does by its own path) |

**All 17 match.** Two were only partly verified, as noted:
- #12: the font folder;
- #13: the getter behind slot 0x13c.

One documented divergence (#2) is a stand-in that the code already names: the clock seed.

## 5. Inconsistencies and measurement artefacts

1. **The attribution fix is in, and the series has two AIDLL lines.**
   - Corrected (the headline from now on): 52.56 → 53.47% cited, 78.2 → 79.1% calibrated.
   - Old (coverage-7's main table): 51.62 → 52.58%, 77.7 → 78.7%.

   Later audits should compare with the corrected line.
2. **Inert bindings count as implemented.** InertRows.cpp registers 37 rows as `ConsoleMethod::stub` no-ops. The probe can't tell them from real bindings, so +45 probe names and "0 unimplemented methods" overstate the change in behaviour. The rows are settled one by one in uncited.md, so the classification is right; only the counts' meaning moved.
3. **The no-audit classifier still can't see uncited.md's classifications** (coverage-7 §5.2). The (d) runs of §2a are classified there but count as missing in `uncited_noaudit`.
4. **SwiffPlayer's figure depends on the string rule** by 18 points (section 1c). It is the first pass, from a scratch wrapper, with its own factor (0.55) rather than a reused one. Don't add it to the three-module total. Superseded by §1d: `uncited.py` measures the module now.
5. **The RendDX9 IGA run is re-bounded** (`0x10053c80` → `0x10053e00`) by a citation at its head. It is the same (a) block.
6. **Carried over:**
   - The level runs print only the top 25 stubs (now moot: there are none).
   - The start-up count includes the 158 host-only methods.
   - AIDLL's own console (282 commands) is in neither console table.
   - Container code propagated as (d) makes rewritten modules look missing.
7. **No regressions.**
   - No row went from impl to stub, and none from read to unread.
   - Every level run's unknown-method count is unchanged (Wake −1).
   - All 856 tests pass.

## Raw outputs

Outside the repo, in the session scratchpad (`/private/tmp/claude-501/-Volumes-Corsair-git-projects-bf2142-recomp/4fc29a33-5499-4852-8614-58e6319f17f7/scratchpad/a8/`):
- `head/`: HEAD cd98860, the full `run_all.sh --before before` output:
  - `probe_*`, `rows*.json`, `delta.md`, `tableA.md`, `stubs_called.md`, `unread_called.md`;
  - `runs/`, `runs_table.md`;
  - `cite*.json`, `uncited_*.json`, `labels_*`, `hedges.*`;
  - `labels_swf*.json`.
- `before/`: c1ecc19, the same, with HEAD's scripts.
- `oldcite_before/`, `oldcite_head/`: c1ecc19's cite.py (no Bots fallback) on each tree, no audit docs, and the classifier on it.
- `swf_classify.py`: section 1c's wrapper (`swf_classify.py <tree> <cite_noaudit.json> <out> [--all-strings]`). `swf_classify_nt.py` is the same without NativeTable.cpp in the corpus; it gives identical figures.
- `fn.py`: the decompilation lookup of the spot-check (`fn.py <module> <addr>…`, the containing function).
- Logs: `before.log`, `head.log`, `build_old.log`, `build_head.log`, `tests.log`.
