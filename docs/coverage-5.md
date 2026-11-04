# BF2142 rewrite: fifth coverage audit (2026-11-04, HEAD 4912c02)

This audit compares HEAD 4912c02 against docs/coverage-4.md, which was measured at 69c8fbc, seven commits back (52b956f … 4912c02: the coverage tooling, then the local-gameplay, bot, rendering and menu passes, seat weapons, drivers' guns and bone parts).

## How this was measured

Both sides were measured with the same scripts, `tools/coverage/` as fixed in this audit (see "Script fixes" below), run on clean `git worktree` builds:
- **before:** 69c8fbc (`tools/coverage/build_old.sh 69c8fbc …`);
- **after:** 4912c02 (`build_old.sh 4912c02 …`; the main checkout's only change is `.mcp.json`, but a worktree keeps the figures tied to the commit).

Both worktrees were removed afterwards. `BF2142_GAME_DIR` pointed at the main checkout's `source/Battlefield 2142`. No window was opened. The probe and the level runs used `--simulate` (one probe mode used `--screenshot`) with `--nosound --no-movies`; the host probe used `--host --dedicated`, without `--online`. The corpus (`calls.json`) and the decompilation features are the same files for both sides.

**Script fixes made in this audit** (they apply to both columns):
- **`rows.py`'s composed-key rule.** It missed `std::string("sway.gunSway") + axis + "Steps"` (src/Game/Weapons.cpp:978), because a `)` sits between the literal and the `+`. All 22 `sway.gunSwayPitch*` / `gunSwayLr*` rows therefore stayed "unread". coverage-4's status lists `sway.*` among the fixed composed keys (its inconsistency 4), but these rows weren't fixed. The rule now allows the `)`. Nothing else changed class.
- **`cite.py --no-audit-docs`** (also in `run_all.sh` as `cite_noaudit` / `uncited_noaudit`). It leaves out `docs/coverage*.md` and `docs/uncited.md`. Their gap lists cite addresses that nothing implements, and each new audit adds more (section 4, item 5). The default rule is still coverage-3's, so the series stays comparable. Both figures are given.

**Re-measuring 69c8fbc.** These scripts give slightly different figures at 69c8fbc than coverage-4 published:
- stored-read 835 against coverage-4's 786, and unread names 453 against 502. This comes from the composed-key rule added in 5d5403b and the fix above.
- EXE calibrated effective 82.1% against 81.7%. The rows feed the classifier's console evidence.

Everything below compares the re-measured 69c8fbc, not coverage-4's printed column.

Outputs, outside the repo, under `scratchpad/`: `c5before/` is 69c8fbc and `c5head/` is 4912c02. `run_all.sh` regenerates them.

## 1. Headline numbers, before → after

| Metric | coverage-4 as published (69c8fbc) | 69c8fbc, these scripts | **HEAD 4912c02** | Change |
|---|---|---|---|---|
| Probe, distinct names: impl / stub (union of `--simulate`, `--host`, `--screenshot`) | 1,088 / 1,600 | 1,088 / 1,600 | **1,128 / 1,560** | +40 |
| … `--simulate` alone | 930 | 930 | 970 | +40. Still 158 host-only names; `--screenshot` adds nothing. |
| Console rows (2,837): impl | 1,096 | 1,096 | **1,137** | +41. No impl → stub regressions. |
| … stub | 422 | 422 | **381** | −41 |
| … ObjectTemplate stored-read | 786 | 835 | **868** | +33 |
| … ObjectTemplate stored-unread | 533 | 484 | **451** | −33. No read → unread moves. |
| Unread template property names (literal + composed rule) | 502 | 453 | **420** | −33 |
| … lower bound (last segment absent from src) | 424 | 376 | **341** | −35 |
| BF2142.exe rows: impl / stub | 856 / 274 | 856 / 274 | **874 / 256** | +18 |
| RendDX9 rows: impl / stub | 240 / 148 | 240 / 148 | **263 / 125** | +23 (the `Overgrowth.*` / `OvergrowthType.*` rows) |
| Start-up "console methods stubbed": EXE / RendDX9 | 515 / 147 | 515 / 147 | **507 / 125** | still counts the 158 host-only methods |
| Stub rows called by shipped scripts / calls | 60 / 24,138 | 60 / 24,138 | **36 / 23,262** | Without `Object.setLightSourceMask` (16,416, inert): 7,722 → **6,846** |
| Unread rows called / calls | 352 / 37,236 | 315 / 36,064 | **282 / 34,733** | Unique names: 294 / 35,225 → 261 / 33,894 |
| Level runs: "N calls to M unimplemented methods" | 5,644–6,568 / 26–42 | same | **5,643–6,560 / 24–26** | see the table below |
| Python tracebacks (11 runs) | 0 | 0 | 0 | |
| "Unknown object or method!" per run | 6,689–7,599 | 6,689–7,599 | 6,689–7,599 | unchanged; names retail doesn't register either |
| Unit tests | 665, 2 failing | 665 | **698, 1–2 failing** | `Voip_LoopbackSession` (VoipTest.cpp:550, 551, 560) fails every run; `Voip_RefusedGameServers` fails in 2 of 4 runs (2 full, 2 `Voip_` only). src/Voip and VoipTest haven't changed. |
| Hedge-note hits (`hedges.py`; engine.md + src) | 219 | 219 | **214** | engine.md 117 → 113, src 102 → 101 |

### Newly implemented console methods (40 names)

- **The level overgrowth (RendDX9, 23):**
  - `Overgrowth.addMaterial`, `addType`, `closeDistFade`, `closeDistPercentage`, `editorEnable`, `getActiveMaterialName`, `lodTimer`, `path`, `preLoad`, `removeActiveMaterial`, `removeActiveType`, `setActiveMaterial`, `setActiveType`, `viewDistance`, `viewDistanceScale`;
  - `OvergrowthType.density`, `geometry`, `minRadiusToOthers`, `minRadiusToSame`, `normalScale`, `randomScale`, `rotationScale`.
  - This settles coverage-4's inconsistency 5: the rows the levels call are bound (`scene/OvergrowthSettings`).
- **Squads and commander (EXE, 10):**
  - `SquadManager.joinSquad`, `leaveSquad`, `changeSquadName`, `Squad.leaveSquad`;
  - `SquadLeader.issueSquadRequest`, `sendRequestForOrder`;
  - `Commander.zoomCommander`, `detailedView`, `spottedMouseOver`, `CommanderMenu.sendRadioMessage`.
- **Game logic (EXE, 4):** `GameLogic.radialLevelIndex`, `supplyDropHeight`, `supplyDropNumSecsToLive`, `enableVoHelp`.
- **Bot perception (EXE, 4):** `AITemplatePlugIn.cullDistance`, `setHearingProbability`, `setSoundSphereRadius`, `weaponTemplate.soundSphereRadius`.

### Template properties that became read (33)

- **Penetration and projectiles:** `penetrate.allowSolidPenetration` / `allowLiquidPenetration`, `hitImpactModifier`, `activeDefenseEnabled`, `ammo.protectionRadius`, `ammo.detonateMaterial`.
- **Weapon handling:** `delayToUse`, `altItemIndex`, `ammo.changeMagAt`, `ammo.instantReloadOnEnable`, `ammo.toggleWhenNoAmmo`, `fire.altFireInput`, `isSelectable`, `specialToggleWeaponInput`.
- **Guidance:** `target.disableGuidanceOnReload`, `target.dontLooseGuidanceOnObstruction`, `target.useCameraDirectionToTarget`, `target.wireGuidedLookAheadDistance`, `seek.reLockTime`, `useProjectileCamera`, `zoom.useProjectileCamera`, `takeOverInput`.
- **Vehicles:** `boneIndex`, `bundleSkeleton`, `ignoreParentRotation`.
- **Unlocks and kits:** `firearmReplacement`, `isFirearmReplacement`, `replaceWithoutEquip`, `pickupEnabled`.
- **Titan mode and maps:** `isVisible`, `showOnMinimap`, `armor.addNametagOnDestroy`, `nametag.healthActivationLevel`.

### Objects that changed (Table A; full table in `c5head/tableA.md`, deltas in `c5head/delta.md`)

| Module | Object | Rows | impl | stored-read | stored-unread | stub | script calls to stubs |
|---|---|---|---|---|---|---|---|
| BF2142.exe | ObjectTemplate | 1403 | 65 | 835→868 | 484→451 | 19 | 29 |
| RendDX9 | Overgrowth | 27 | 0→16 | | | 27→11 | 448→0 |
| RendDX9 | OvergrowthType | 7 | 0→7 | | | 7→0 | 372→0 |
| BF2142.exe | AITemplatePlugIn | 49 | 40→43 | | | 9→6 | 28→5 |
| BF2142.exe | GameLogic | 41 | 22→26 | | | 19→15 | 34→1 |
| BF2142.exe | Commander / CommanderMenu | 31 / 8 | 28→31 / 7→8 | | | 3→0 / 1→0 | 0 |
| BF2142.exe | SquadManager / SquadLeader / Squad | 7 / 5 / 1 | 0→3 / 2→4 / 0→1 | | | 7→4 / 3→1 / 1→0 | 0 |
| BF2142.exe | weaponTemplate | 22 | 12→13 | | | 10→9 | 9 |

### Stubs still called by the shipped scripts (36 rows, 23,262 calls)

**Not gaps** (load hints, editor rows, own parsers, inert):
- `Object.setLightSourceMask` 16,416;
- `GeometryTemplate.compressVertexData` / `maxTextureRepeat` / `lightmapOverride` / `doNotGenerateLightmaps` / `isAd` / `setLightmapSizeAll` / `boundingRadiusModifier`;
- `heightmap.setModified` 342, `awards.setData` 126, `undergrowth.load` 74, `texturemanager.customTextureSuffix` 39, `terrain.init` 38, `rainManager.cellDisplay` 29, `Console.allowMultipleFileLoad` 26;
- the `RagDoll.*` rows (own parser).

**Player-facing or uncertain:**

| Calls | Row | Note |
|---|---|---|
| 24 | `ObjectTemplate.addVehicleType` (4-argument form) | The scripts' 1-argument calls fail in retail too. The 1-argument row is stored-read (Commander.cpp:893). |
| 8 | `Object.notInAI` | |
| 5 / 2 / 2 | `weaponTemplate.shouldRearm` / `fireRate` / `isAntiAirWeapon` | AI weapon hints |
| 3 / 2 | `AITemplatePlugIn.setUseNoPathfindingToGetToObject` / `vehicleNumber` | |
| 3 / 2 | `ObjectTemplate.setCheckObstruction` / `setIfTitanChangeToWeapon` | |
| 1 each | `GameLogic.createObject`, `Material.pythonEvent` | |

### Level runs (calls / methods)

| Run | 69c8fbc | HEAD |
|---|---|---|
| Belgrade gpm_nv 32 | 6,452 / 32 | 6,446 / 26 |
| Camp_Gibraltar gpm_cq 32 | 6,305 / 41 | 6,289 / 25 |
| Cerbere_Landing gpm_cq 32 | 6,131 / 34 | 6,123 / 26 |
| Highway_Tampa gpm_sl 64 | 6,568 / 34 | 6,560 / 26 |
| Minsk gpm_ti 48 | 6,072 / 42 | 6,020 / 26 |
| Molokai gpm_ti 48 | 5,701 / 39 | 5,658 / 24 |
| Operation_Shingle gpm_sl 32 | 5,644 / 26 | 5,643 / 25 |
| sidi_power_plant_coop gpm_coop 32 | 6,215 / 31 | 6,209 / 25 |
| Suez_Canal gpm_cq 64 | 6,343 / 41 | 6,303 / 25 |
| Verdun gpm_sp 16 | 6,465 / 42 | 6,431 / 26 |
| Wake_Island_2142 gpm_ca 64 | 5,682 / 40 | 5,643 / 25 |

- **Left the stub report:** all 15 `Overgrowth*` rows (2–21 calls each) and `GameLogic.radialLevelIndex`.
- **Joined the top-25 list:** `rainManager.cellDisplay` and `undergrowth.load` (6 each). They moved up when other rows left; neither regressed.
- **What remains, every run:**
  - the load hints (compressVertexData 15,756, maxTextureRepeat 15,734, lightmapOverride 13,292, doNotGenerateLightmaps 13,226, isAd 2,541) and setLightSourceMask 5,241;
  - the RagDoll rows;
  - `heightmap.setModified` 99, `setCheckObstruction` 33, `setIfTitanChangeToWeapon` 22, `boundingRadiusModifier` 22, `GameLogic.defaultTimeToNextAIWave` 11 and `Material.pythonEvent` 11.

### Address citations (start addresses; src/ and docs/, tests excluded)

| Module | functions / bytes | 69c8fbc without appendix | **HEAD without appendix** | **HEAD with appendix** | HEAD, no audit docs (without / with appendix) |
|---|---|---|---|---|---|
| BF2142.exe | 35,589 / 4,794,004 | 2,895, 1,221,549 B (25.5%); with appendix 3,795 / 27.7% | **3,083, 1,291,342 B (26.9%)** | **3,983, 1,395,954 B (29.1%)** | 2,928 / 1,230,831 B (25.7%); 3,828 / 27.9% |
| RendDX9_ori.dll | 10,810 / 1,677,614 | 463, 315,910 B (18.8%) | **496, 328,714 B (19.6%)** | same | 415, 276,314 B (16.5%) |
| AIDLL.dll | 6,080 / 1,006,188 | 820, 333,024 B (33.1%) | **1,068, 384,143 B (38.2%)** | same | 1,061, 382,501 B (38.0%) |
| SwiffPlayer.dll | 5,756 / 936,991 | 169, 124,725 B (13.3%) | 169 (13.3%) | same | |
| BF2VoipServer.dll | 377 / 38,482 | 34 (28.5%) | 34 (28.5%) | same | |
| BF2142Audio.dll | 1,702 / 206,940 | 2 | 2 | same | |
| Memory.dll / TextureAtlasBuilder.dll | | 0 | 0 | 0 | |

- **Without the audit docs at 69c8fbc:**
  - EXE 2,761 fns / 1,169,837 B (24.4%), 26.6% with the appendix;
  - RendDX9 389 / 265,297 B (15.8%);
  - AIDLL 811 / 331,564 B (33.0%).

  So the gain is about the same either way: EXE +1.3 points, RendDX9 +0.7, AIDLL +5.0.
- **The appendix** is unchanged: 900 EXE functions / 104,612 B. It adds 2.2 points of raw EXE share and nothing to the effective figures (section 1b).
- **coverage-4.md on its own** adds 22 EXE functions / 10.3 KB, 7 RendDX9 / 1.8 KB and 2 AIDLL functions, measured by removing it from the HEAD worktree. That is why several uncited-run starts below moved by one function (`0x00840370` → `0x00840490` and others): the audit cited the runs' first functions.
- **Ambiguous DLL addresses dropped:** 429 → 136. engine.md's AIDLL tables now name their DLL.
- **With `--headings`:** RendDX9 505 / 333,605 B (19.9%), AIDLL 1,098 / 388,259 B (38.6%), 76 ambiguous.
- **With tests/:** EXE 3,133 / 1,301,337 B (27.1%; 29.3% with the appendix), RendDX9 497 / 329,038 B, AIDLL 1,068 / 384,143 B.
- **Uncited runs, EXE (≥30 KB):** 9 / 0.59 MB with the appendix, unchanged. Without it, 11 / 0.68 MB → 11 / 0.66 MB.
  - The template-setter run `0x004e892b` (42 KB) shrank to `0x004e90e2` (31 KB).
  - Left: FESL/EASTL (157, 121, 69, 36 KB), the command-line / profile / settings managers (`0x0046c880`, 46 KB, was 52), file paths (37 KB), static init (36 KB), GameSpy GP (36 KB), and `0x0073e120` (30 KB).
- **Uncited runs, RendDX9 (≥20 KB):** 15 / 0.50 MB → 15 / 0.49 MB.
  - The overgrowth run `0x1008f720` (37 KB) is gone: it is cited now.
  - Largest left: debug drawers `0x10189e10` (81 KB), the terrain editor `0x101546d0` (47 KB), IGA `0x100579d0` (43 KB), the SwiffRender / name-tag glue `0x1016d220` (38 KB), roads `0x10101fe0` (31 KB), particles `0x101413c0` (26 KB), `0x100106b0` (25 KB), and `0x10092990` (24 KB, the rest of the overgrowth module).
- **Uncited runs, AIDLL (≥20 KB):** 2 / 0.06 MB → 2 / 0.055 MB (`0x10044cc0` 31 KB, `0x1002aeb0` 22 KB).

### 1b. Effective coverage (the uncited classifier, `uncited.py`)

The definitions are coverage-4's:
- **Needed** is the total minus (a) third-party and (b) dev-only code.
- **Lower** is cited plus string- or console-evidenced (c).
- **Calibrated** adds propagated (c) × the factor (0.84 / 0.69 / 0.48) and splits "?" pro rata.
- **Upper** counts all (c) and all "?".

| Module | Needed | Cited share of total, with appendix | Lower | **Calibrated** | Upper | (d) calibrated | Raw (d) |
|---|---|---|---|---|---|---|---|
| BF2142.exe | 3.67 → 3.69 MB | 27.7% → 29.1% | 50.0% → **51.8%** | 82.1% → **83.3%** | 88.3% → 89.3% | 0.66 → **0.62 MB** | 419 → 385 KB |
| RendDX9_ori.dll | 1.29 → 1.30 MB | 18.8% → 19.6% | 36.4% → **37.7%** | 69.9% → **73.5%** | 84.8% → 89.3% | 0.39 → **0.34 MB** | 191 → 135 KB |
| AIDLL.dll | 0.87 MB | 33.1% → 38.2% | 41.8% → **45.9%** | 66.6% → **69.8%** | 93.0% → 95.2% | 0.29 → **0.26 MB** | 59 → 40 KB |
| All three | 5.83 → 5.87 MB | 33.9% → 36.0% (of needed) | 45.8% → **47.8%** | 77.1% → **79.1%** | | 1.34 → **1.23 MB** | 669 → 560 KB |

- **Without the appendix the effective figures are identical** for every module. The cited share drops to 25.5% → 26.9% (EXE), and the appendix's addresses move to the console-evidenced bucket (EXE "c ev" 494 → 596 KB before, 505 → 606 KB after).
- **Without the audit docs** (`uncited_noaudit`):

  | Module | Lower | Calibrated | (d) calibrated | Raw (d) |
  |---|---|---|---|---|
  | EXE | 48.9 → 50.6% | 80.8 → **82.1%** | 0.70 → 0.66 MB | 463 → 426 KB |
  | RendDX9 | 34.9 → 36.1% | 67.6 → **71.8%** | 0.42 → 0.36 MB | 224 → 156 KB |
  | AIDLL | 41.6 → 45.7% | 66.5 → **69.2%** | 0.29 → 0.27 MB | |
  | All three | 44.7 → 46.6% | 75.7 → **77.9%** | 1.41 → 1.29 MB | |

  The deltas match the default rule's within a point. The absolute figures are 1.3–1.7 points lower for the EXE and RendDX9. The audit docs had been counted as implementation evidence (section 4, item 5).
- **With `--headings`:** RendDX9 38.0 / 73.8 / 89.6%, AIDLL 46.3 / 70.1 / 95.5%, all three 47.9 / 79.2%. At 69c8fbc: 36.8 / 70.2, 44.2 / 68.3, 46.2 / 77.4.
- **This classifier's own propagation precision** (`--recalibrate`):
  - factors: EXE 0.46 → 0.49, RendDX9 0.69 → 0.69, AIDLL 0.50 → 0.42;
  - calibrated with those factors: EXE 68.2 → 70.5%, RendDX9 69.7 → 73.5%, AIDLL 67.8 → 66.8%, all three 68.5 → 70.6%.
  - The reused EXE factor (0.84) is still generous for this classifier. Read the calibrated figures as before/after on one scale.
- **"Needed" grew** because 25 KB of EXE code that the classifier had guessed dev-only (b) is cited now: the spectator's list code `0x0047c7a0`, the PCO save `0x005caff0`, the squad-spawn refusal `0x004bef90`.
- **On coverage-3/4's published series:** adding this audit's deltas to coverage-4's continuation (EXE ≈ 82, RendDX9 ≈ 76, AIDLL ≈ 66, all ≈ 78) gives about **EXE 83%, RendDX9 80%, AIDLL 69%, all three 80%**. These are approximate. On this classifier RendDX9 is about 6 points lower in absolute terms (coverage-4's reproduction table).
- **What moved:**
  - RendDX9: the overgrowth, the mesh lights and decal clipping.
  - AIDLL: the reload decision, the seat armament, BBFire3d's scale and hearing.
  - EXE (about a point): penetration, seat weapons, guidance, the bone parts.
- **What remains in (d)** is fragmented. No (d) run in the EXE is above 11 KB: the template proxy / grid code `0x006e8610` 11 KB, RagDoll's editor rows `0x008ae4e8`, `Console.get*` `0x0066b175`, `0x00685520`, `Object.initGrid` `0x006ee282`, `SquadManager.makeMeCommander` `0x004196a4` and `0x00504be6`. None in RendDX9 is above 9 KB (`0x100f96b0`, the name-tag code `0x10170370`), and none in AIDLL above 4 KB.

## 2. Remaining decodable gaps, ranked by player visibility

Effort: **S** = days, **M** = 1–3 weeks. Calls are from this corpus. Offsets and executors are from `symbols/BF2142.exe.template_properties.csv`. Every gap in coverage-4's §2 that its status lines mark done has been left out. Some new ones come from the code that went in since: the seats now have three weapon groups, but the HUD and the docs still assume one gun.

### 2a. Local play

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| L1 | **Leaving the combat area: no warning text or countdown** | No HUD_TEXT_MENU_LEAVING_COMBATAREA, no HUD_CENTERINFOBOX_timeuntildamage seconds. The damage still comes (and the commander's radio line). | engine.md:8537–8545: "Not rewritten: the HUD's text …, the minimap's area texture. The text's data is all there" (HUD `0x0044c7b0`, `0x0044cfe0`..`0x0044d220`). `GameWorld::combatAreaTimeLeft` / `outsideCombatArea` (src/Game/Radio.cpp:301, 307) have no HUD caller: no `CENTERINFOBOX` anywhere in src. | S |
| L2 | **No limit on placed explosives and gadgets** | C4, mines, beacons, the sentry gun, decoys and sonar can be placed without limit. Only hubs and drones are one per owner. | `fire.maxProjectilesInWorld` (comp 0xc4f1 `+0x74`, executor `0x004e33d8`), set by 16 templates: Unl_c4 / Unl_Mine_EMP / Unl_Mine_Motion / Beacon 5, Unl_Mine_AntiPersonnel / sonar 2, the sentry deployer / Bp1_Decoy / Bp1_sonar / the drone detonators 1, eu_aa / eu_av 8. GameWorld.h:2284 (one per owner for supplies only); GameWorld.cpp:7630, 10365. Reader not traced. | S |
| L3 | **A vehicle's second weapon has no HUD** | In a seat with PIAltFire / PIFlareFire weapons (since 8dc3e13), the Secondary* ammo block never shows. | main.cpp:1205–1207 forces `SecondaryAmmoShow` 0 ("the game's seats carry one gun"). `WeaponHud::update(…, secondary=true)` exists (HudMenusVehicles.cpp:101–110) but nothing calls it. engine.md:6279–6282 (`0x007c2d50`, the seat's weapon 1). | S |
| L4 | **Vehicle item selection by wheel and icons** | The mouse wheel's item list in vehicles, with the positions listed, and `displaySelectOnActivation`. The rewrite opens the list on any weapon change. | `vehicleHud.useSelectionIcons` 200 calls, `weaponHud.displaySelectOnActivation` 10, both unread. Decoded in engine.md:6323–6330 (`0x00755d50`, `0x0079fd30`, `0x0079c470`). The doc's "the rewrite's seats carry one gun" is stale. | S |
| L5 | **Repairability flags** | Engineers repair objects retail won't, such as the Titan cores (`as_coree3*` set 0). Wrecks of commander assets follow their own flag. | `armor.canBeRepaired` (executor `0x0054b1c9`; 16 templates 0, 6 templates 1) and `armor.canBeRepairedWhenWreck` (`0x0054b2ee`, 19 templates), both unread. The repair scan takes every team body (GameWorld.cpp:8305–8388). The heal test's armour slot `0x48` (`0x00655a00`) is the probable reader. | S |
| L6 | **Destroyable props** | Barrels, crates, a bridge piece, rail bars and a Titan console never break or respawn. | engine.md:2785–2790: "the rewrite destroys none of them" (the six DestroyableObject templates, class `0xc4d0`; the spawners `0x004d2250`, `+0x29c` / `+0x2a0`). | M |
| L7 | **Ladders: animation and camera** | Climbing works but looks like standing. | engine.md:8267–8271 "Not rewritten: the animation and camera" (`0x0059f6b0`, `0x0059e210`). | S–M |
| L8 | **Seated look in open seats** | Passengers in open seats (the FAVs' open position, the AT walkers' side seats) can't look around within their limits. | engine.md:1353–1365; `seatLeftRotationLimit` / `seatRightRotationLimit` 18 each, unread. The rewrite's seated soldier has no look of its own (`GameWorld::poseSeated`). | M |
| L9 | **Titan mode remainder** | Pods hitting a shielded Titan. The minimap's combat-area texture. | engine.md:9347 "Not rewritten: pods against a shielded Titan"; engine.md:8537. | S |
| L10 | **Vehicle HUD remainder** | No vehicle stamina, torque, mech foot animation or blinks. The child HUD and secondary gunner angle are missing. | engine.md:6337. Unread: `vehicleHud.useChildHud` 19, `showOneSecondaryGunnerAngle` 19, `zoomOnMinimap` 14, `miniMapIconOffset` / `Inside` 12, `standardHelpEnabled` 8, `useVehicleCommRose` 7. | S |
| L11 | **Kit and ability leftovers** | The ammo / repair gadget's first sphere pass and its message. addExtraAmmoItem's flag. The parachute field unlock. Picking up a deployable placed on another object. | engine.md:6871 (Left: `0x005910c0`), 6884–6887 (the flag's consumer not traced; GameWorld.cpp:790), 7939 (case 0x0d), 7356–7359 (`0x004c5950`). Unread: `dropHeadwear` 16, `ammo.magLinkWeapon` 14, `ammo.allowReloadWithFullMag` 8. | S each |
| L12 | **Unread weapon flags with no reader traced** | Going by the names: a fire delay at the start, a ban on firing while tilted, no fire while a missile flies, a rocket's minimum distance. | `fire.fireStartDelay` 8 (`0x004e259f`); `fire.maximumVehicleTiltForFire` 13 (+0x7c); `target.disableFireWhenActiveMissile` 9 (comp 0xc50c +0xb0); `fire.minimumDistance` 20 / `useDistanceCounter` (the as/eu_ar rockets, 20 m); `fire.projectileStartRotation` 24; `collision.bouncing` 36, `hasCollisionEffect` 31, `maxStickAngle` 17; `armor.deepWaterDamageDelay` 18, `dontUseWreck` 12, `disableInputOnDestroy` 34; `startAutoRotateOnEnter` 36; `surpressPlayerInputVector` 22. None is in engine.md. | S each |

### 2b. AI

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| A1 | **Bots driving: the vehicle plan's Mobile branch** | Bot drivers drive straight at a target and stop within reach. No flanking or ranging moves. | engine.md:1125–1130 ("the rest of that sequence isn't decoded"; `0x100795a0`, `0x100cff90`, `0x100d11c0`); Bots.cpp:5326. | M |
| A2 | **Fire-plan stand-ins** | Bots don't use the guided weapons' own plan on foot. C4 isn't guarded on moving targets. The target's speed and attention are the rewrite's. Bots press reload themselves. | engine.md:10654–10669, 9700–9705 (the plan's continuity, type 0xe), 9740; Bots.cpp:2828–2873, 5474. | M |
| A3 | **Bot leftovers** | Bots in ITAir vehicles. The terrain-normal "upright" test. Seat changes and pick-ups. The grenade warning's sender. | Bots.cpp:3448, 4288, 917, 1612; engine.md:9599. | S each |
| A4 | **Scripted-bot commands** | Only mods use them. | engine.md:10567–10598 (`createSAI`, `addPlacement`, the tree modifiers, landing zones); the SAI's status rule for areas without a control point. | S |

**Status (bot drivers, fire-plan stand-ins and the voice-test pass):**
- **A1 done.** The Mobile branch of `0x100795a0` is decoded through its statements' debug printers (If `0x100d29f0`, Serial / Parallell combiners `0x100ce370` / `0x100cddb0`, MoveToObjectFinding `0x100cff90` and its back-off `0x100d00e0`, ObjectDistance `0x100d44c0` / `0x100d4620`, ForgetObject `0x100da5f0`). Drivers now hold [minRange, 1.1 D], drive in until ec ≤ d ≤ D, and back off to a spot ahead of or beside a target that is too near. A lost target is chased down and then forgotten. The trigger waits for [0.9 minRange, 1.1 reach]. `BotManager::driveFirePlan`, `backOffSpot` (engine.md, "The vehicle's fire plan"). Measured over 16 seeds (15 bots, 300 s), against the same working tree without the change: Cerbere_Landing gpm_sp 16 kills 18.94 ± 0.84 → 19.56 ± 0.77, rounds 1,648 → 1,654, hits 86.4 → 74.1, CPU 5.15 → 5.82 s; minsk_coop gpm_coop 32 kills 2.62 ± 0.33 → 3.19 ± 0.33, rounds 583 → 453, hits 32.8 → 79.5, CPU 8.23 → 9.04 s. The extra CPU is in the physics (closestOnSegment, trace), because drivers that used to park now move.
- **A2 partly done.** Done: the distance's + 0.25 dh and its 5 m floor apply only in the explosive plan (`local_60`), as the builder has them. Decoded, not rewritten: the guided weapons' plan on foot `0x10078570` (same shape as the Mobile branch, with its own statements `0x100781e0`, `0x100d4f10`, `0x100d4480`). The target's speed traced to `0x008a1960` → PCO interface slot 0x108 `0x0059d950` (the object's +0x2ec), but no writer was found. Still open: C4 on moving targets (the bot object's +0x80 field) and the target's attention (`0x100eb7f0`'s writer).
- **A4:** the SAI's status rule for areas without a control point (`0x1005f210`) is rewritten (`ai::boxAreaStatus`). It feeds the neighbours' flags; the SAI still orders units only to control-point areas.
- **BBChangeLandingCraft's ground way out** (A3 / engine.md): decoded to the Physical slots 0x64 / 0x5c, the Mobile speed under 2 and the Mobile's +0x10 point within 4 m. Not rewritten: no shipped ground vehicle runs the behaviour, and the adaptor slots weren't traced.
- **Inconsistency 8 fixed.** On Darwin, loopback UDP is delivered asynchronously (through the input thread), and a sandbox's network filter delays it further. `Session::step(2)` / `step(3)` never slept, so the voice server hadn't read the removal or the routing table yet (VoipTest.cpp's removal / re-add checks; in `RefusedGameServers`, the challenge). Each step now ends only when every datagram sent during it has been read (`UdpServerSocket::datagramsSent` / `datagramsReceived`, a 2 s cap), and `pump` waits up to 2 s of wall time. The checks themselves are unchanged.

### 2c. Joined and hosted play

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| J1 | **Spectator remainder** | A joined spectator can't follow another player. No scoreboard icon. | engine.md:1812–1813 (the joined view has no other players' frames). | S–M |
| J2 | **A joined client's seat item popup** | The key's flash and the seat list show only offline. | engine.md:1103; also the `+0x14` bit 0 suppression (engine.md:1064, GameWorld.cpp:9110) and the HUD message 0x52 (engine.md:1075). | S |
| J3 | **Ghost field meanings and dirty masks** | Minor client state. Bandwidth. | Networkables.h:221, 235, 258, 355, 369; GameProtocol.h:458; HostedGame.cpp:2308 (CreateSpawnGroup's bits); RemoteWorld.h:157 (the kick reason); GhostPriority.h:78, HostedGame.cpp:2575, engine.md:4861. | S each |
| J4 | **Server odds and ends** | A long rcon_feedback. Ranked minimums. Message 9 (the demo download URL). Refusing an applicant after a mutiny. The CD-key reason. | engine.md:4959, 5035, 5102; Commander.cpp:138; HostedGame.cpp:600. | S |

### 2d. Menus and front end

| # | Gap | Evidence | Effort |
|---|---|---|---|
| M1 | **Commander and squad remainder:** `<tool>_unavailable` to the commander; the AI commander's answers to bot squad leaders; the squad panel's voice icons; select-all; the order-points bar. | engine.md:8941–8944, 8996–9000; HudMenus.h:252 | S |
| M2 | **Demo playback remainder:** the demo camera's own key input (`0x005fa5e0`), the HUD's Demo* strings, camera files (cameramode 1 / 2). | engine.md:5135–5138 | S–M |
| M3 | **HudBuilder authoring rows**: no shipped script calls them, so only mods are affected. | 24 stub rows | S |
| M4 | **Awards pop-up**: online-gated. | `0x007d1270` | S once events exist |

### 2e. Rendering

| # | Gap | Evidence | Effort |
|---|---|---|---|
| R1 | **Lights:** projector textures, the lights' shadows, the template's speccolor, supportsEmitter. | engine.md:2350–2352. Unread on the 93–94 light templates: `projectorTexture` / `Fov` / `Aspect` / `Near` / `Far`, `softness`, `HDRIntensity`, `shadowIntensity`, `objectShadows`. maxLightsPerTerrainPatch's reader (engine.md:2349). | M |
| R2 | **Decals:** the normal-mapped technique (`decalNormalMapName` 10), +0x1a's refusal on an unseen owner, the static decals' list. | engine.md:2439, 2461–2464 | S |
| R3 | **Terrain:** the per-material detail lists, cullerTypes other than 3, detailCullMaxLevel. | engine.md:2141–2143 | S |
| R4 | **Overgrowth alpha doubling; zoomed-mode particles; particle blend and time override.** | engine.md:2242–2243 (`0x1000b500`), 3041–3043, 1240, 1633; Effects.h:234, 239 | S |
| R5 | **The view's tint** (PostProduction slot 0x7c). | engine.md:3440 | S |

### 2f. Not decodable or out of scope (unchanged)

- the online back end (FESL/EASTL, GameSpy GP, BFHQ, leaderboards, award grants);
- PunkBuster, IGA, SecuROM;
- the lightmapper and TextureAtlasBuilder;
- editor and debug objects;
- Novint Falcon and the G15 LCD;
- the AI's real-time CPU budgets.

### Top 15, overall

| Rank | Gap | Area | Effort |
|---|---|---|---|
| 1 | L1: combat-area warning text and countdown | local, every map | S |
| 2 | L2: `fire.maxProjectilesInWorld`: no limit on C4, mines, beacons, sentry, decoys | local | S |
| 3 | L3: the second weapon's HUD in vehicles (Secondary* never driven) | local | S |
| 4 | L5: `armor.canBeRepaired` / `canBeRepairedWhenWreck` | local | S |
| 5 | L4: vehicle item selection (`useSelectionIcons`, `displaySelectOnActivation`) | local | S |
| 6 | A1: bot drivers' vehicle plan (the Mobile branch of `0x100795a0`) | AI | M |
| 7 | L6: destroyable props (six DestroyableObject templates) | local | M |
| 8 | L7: ladder animation and camera | local | S–M |
| 9 | A2: bot fire-plan stand-ins (guided weapons on foot, C4, target speed and attention) | AI | M |
| 10 | L9 + L10: Titan pods against shields; vehicle HUD remainder | local | S |
| 11 | L8: open seats' look | local | M |
| 12 | J1 + J2: joined spectator following; the joined seat popup | joined | S–M |
| 13 | R1: lights' projector textures, shadows, speccolor | rendering | M |
| 14 | M1: commander and squad menu remainder | menus | S |
| 15 | L12: the unread weapon and armour flags (start delay, tilt, active missile, bouncing, deep-water delay) | local | S each |

**Status (HUD pass: gaps 1, 3, 5, 10's HUD part, 12's seat popup, 14; after HEAD 4912c02):**
- L1 done: the combat-area warning (`0x0044c7b0`): the seconds left, ceiled (`0x004480f0`), as #TIME# in HUD_TEXT_MENU_LEAVING_COMBATAREA through the HUD manager's slot `0x144` (`0x007c6fd0`: PauseMessageActive and DisconnectMessage, HudElementsGameInfo.con's centre line) while the game runs, and HUD_CENTERINFOBOX_timeuntildamage in SpawnInfoString (the layer's `+0xa70`). Joined clients take the time outside from the player networkable's 0x800 part (`RemotePlayer::outsideSeconds`) and the timer from their level's areas (`HudWorld::combatAreaTimeLeft`). Screenshot: `scratchpad/combat_area_warning.png`. The minimap's area texture is still not drawn.
- L3 done: the Secondary* block from the seat's PIAltFire group's active weapon (`0x007c2d50`: the control object's slot `0x78(1)`; the PIFlareFire group has no HUD), with its own WeaponHud (`HudWorld::seatGroupWeapon`, a joined game's from our own state's group 1).
- L4 done (in part): the seat's item list (`0x0079c470`: the PIFire group, the soldier's own without one, useSelectionIcons' longer PIAltFire group; `menu::seatSelectItems`), displaySelectOnActivation on a seat's weapon change (event 2/0x20 → `0x0079fd30(0)`: 2 s). The in-game mouse wheel's list (`0x00755d50`, inputs 0x30 / 0x31) isn't rewritten: the rewrite's wheel reaches only the menus, and how the wheel's pick is taken up isn't traced.
- J2 done: a joined client's seat item popup (its weapon keys now also go to the server as PIWeaponSelect / PIPositionSelect), and the help message 0x52 (`0x004d2ea9`): the firearm's slot `0x188` (`0x005c3080`) is minimumSquadSizeForSelection (template `+0x320`, executor `0x004e1ed9`: the beacon 2, the recon drone's detonator 3, the sentry drone's 4) against the owner's squad (one fewer for a bot), part of the selectable test (`0x005c1150`); `GameWorld::squadTooSmall` refuses the weapon and the local player's key lets 0x52 through. The `+0x14` bit 0 suppression is still not traced.
- L10 (HUD part) done: the torque arrows (`0x007a2e60`: Torque / TorqueAngle / SecondTorque / SecondTorqueAngle, eased a hundredth a frame; their source `+0x2f4` has no writer in BF2142.exe, so they rest at 0 and 2π), VehicleStamina* (the control object's slots `0x108` / `0x100`; the rewrite's vehicles keep no stamina, so 1) and PlayerStamina* (`0x007a0f80`, the soldier's sprint, also joined), the child HUD (useChildHud, `0x007c38c0`: UsingChildHud and the soldier's weapon in a gunless seat). Not done: the mech's foot animation, the blinks, showOneSecondaryGunnerAngle, zoomOnMinimap, miniMapIconOffset / Inside, standardHelpEnabled, useVehicleCommRose.
- M1 done: `<tool>_unavailable` to the commander was already done (local and joined, HudMenus.cpp's `SquadRequests::answer`, main.cpp's joined accept); the squad panel's voice icons (`0x007aaf10`: blocked / transmitting / distorted, help 0x26 and 0x27); "select-all" is the tactical rose's all-squads row (`0x007ac8f0` with the panel's `+0x1e4`, set by c_GITacticalComm in `0x00754290`), now drawn. The order points bar can't work offline: the squad's `+0x184` needs `0x00430a00` (a GameSpyRanked server with the online stats), so offline the exe never shows it either.
- Inconsistencies 1, 2, 3 and 6 fixed in the docs (engine.md's TV effect, the seats' one-gun lines, roadmap rows 8–12, coverage-3's `0x005eaca0` line, detectionRadius / detonateOnWaterCollision in engine.md). Inconsistency 7: tests for the squad radar's decoy and vehicle markers (`HudRadar_DecoyAndVehicleMarkers`, the markers now `menu::radarMarkers`), the commander and squad leader rose rows (`HudMenus_CommanderRoseRows`), the TVEffect through a round (`Vehicles_GunshipMissileCamera`, `host::roundCameraEffects`); plus `Vehicles_SecondWeaponHud`, `HudVehicle_TorqueAndStamina`, `HudItemSelection_SeatLists`, `HudItemSelection_SquadSizeRefusal`, `HudMenus_CommanderVoiceAndAllSquads` and the combat area's HUD countdown in `CombatArea_OutsideTimerThenDamage`.

**Status (game-rules pass: gaps 2, 4, 7, 8, 10's pods part, 11, 15; after HEAD 4912c02):**
- L2 done: `fire.maxProjectilesInWorld`'s only reader is the fire component's projectile creation `0x005f2bb0`: past the limit the firearm's oldest rounds are deleted (no detonation), it never refuses to fire, and the count is per firearm instance (`GameWorld::limitInWorld`; the hubs, shield, drones and beacon keep their own paths, which give 1). The list also has the hubs and the portable shield (1), which §2's list missed. `destroyProjectilesOnDestroy` has no reader.
- L5 done: `armor.canBeRepaired` (`+0x14c`) and `canBeRepairedWhenWreck` (`+0x14d`) gate every heal (`0x004bc410` → `0x004cc380`, `0x00655a00`; Titans' own armour never): `GameWorld::repairable` in the ability scan and heal, the seated kit and the crates.
- L6 done: the five armoured DestroyableObjects break (`0x00654120` → `0x00651c60`: the physics gone, the second geometry or hidden, the armour's blast with chain reactions, level-0 effects; the rail bars drain away in 20 s) and stand again the next round; `0x004d2250`'s spawner spawns once a round (its 10.0 is the spawner's distance, not a time; engine.md corrected). `src/Game/Props.cpp`. Screenshots of the props: `scratchpad/props_intact.png` (the `--spawn`ed props aren't in the static collision built at the round's start, so rounds pass them; the level's own props break).
- L7 done: the ladder's climb (`animationSystemHandles.inc` by height through the time override `0x008bbab0`, `0x0059f380`) and its camera (LadderContainerCamera 0/1.5/−0.4, −130..140 / ±60, the top push `0x0059e730`); no first-person models on a ladder.
- L8 done: the seated look (`0x0058a6d0`: the twist to ±40, then the body within seatLeft/RightRotationLimit, turning the third-person body, `0x00587a30`, and the LookAroundTrigger) and the seated soldier's own camera for seats without one (`0x005c7cb0`). The as_at / eu_at seats are the air transports' (the left −15 / 40, the right −25 / 30; engine.md said "AT walkers" with the pairs swapped). Screenshot: `scratchpad/fav_seat3_ahead.png`.
- L9 (pods) done: no shield code; the fields' vehicle LOD is material 666, which destroys a PlayerControlObject or a Parachute (`0x005c72f0`, `0x005cf750`), team-filtered: enemy pods and vehicles die on a standing field.
- L12 done where there is behaviour: fireStartDelay, maximumVehicleTiltForFire (pods), disableFireWhenActiveMissile, the distance counter's airburst (the wheel's step is an API, `stepBurstDistance`, not yet bound in main), collision.bouncing (EMP and smoke grenades now go off where they land), deepWaterDamageDelay (two 1 s timers, waterDamageDelay 3 s by default), disableInputOnDestroy (read), startAutoRotateOnEnter, surpressPlayerInputVector (the yaw). Inert or not applicable: hasCollisionEffect (no caller), maxStickAngle (StickyCollisionComp, not rewritten), projectileStartRotation (orientation only), dontUseWreck (the Titan wake / playtest hulls; honoured by the props).
- Inconsistency 7: `GameWorld_GuidanceLostOnReload` (disableGuidanceOnReload) and `Bots_SeedRepeatsTheRound` (BF2142_BOT_SEED: the same seed, the same round tick for tick; another, another). New tests: `GameWorld_MaxProjectilesInWorld`, `_CanBeRepaired`, `_DeathMaterialDestroysVehicles`, `_DestroyablePropsBreak`, `_NoFireWhileTheMissileFlies`, `_WaterDamageDelays`, `_AirburstDistance`, `_LadderCamera`, `_OpenSeatLook`, `Weapons_FireStartDelay`, `Weapons_MaximumVehicleTiltForFire`, `Vehicle_AutoRotateOnEnter`, `VehicleModels_SuppressedInputWhileZoomed`, `AnimationSystem_LadderClimbByPhase`.

## 3. Remaining hedges a further pass could settle

From `hedges.py`: 214 hits at HEAD, 219 at 69c8fbc. These can be settled by decompilation; line numbers are HEAD's.

**Weapons, projectiles, physics**
- engine.md:947, MeshCollision.cpp:63: the response's stuck push (+0x11c).
- engine.md:7269: the detonation's 0.05 m MTYVehicle test, ForceObjectHoverTest, drag.
- engine.md:6884–6887, GameWorld.cpp:790: addExtraAmmoItem's flag.
- engine.md:7345: the BoltClick flag `+0x41`'s writer; GameSounds.cpp:500 is the same.
- Weapons.h:657: the deviation's slot 0x1a0.
- GameWorld.cpp:4760: the ground's material.
- GameWorld.cpp:7483: the stand-in for +0x1f8.
- engine.md:1064, GameWorld.cpp:9110: the active weapon's `+0x14` bit 0.
- engine.md:718, GameWorld.cpp:9997: the sentry's message 1 sender (`0x0063ce30`).

**Spawning, game logic, kits**
- GameWorld.h:19: activation by events.
- GameWorld.cpp:3924: the extra test some callers ask.
- GameWorld.cpp:4836: the hull-kill note.
- GameWorld.cpp:5402: where the engine keeps it.
- GameWorld.cpp:8865, engine.md:1892–1895: rememberExcessInput.
- engine.md:8419–8421: the `+0x8c` height test.
- engine.md:7513, Radio.cpp:95: the medic flag's setter.
- engine.md:7944–7947: when the kits' supply sound starts and stops.
- engine.md:5027: the kit list's filler.
- engine.md:11332: the kit-screen setter.
- engine.md:1665–1669: the IK disable flag and the weapon layers.

**HUD**
- engine.md:6041: the `.dif` layout, "inferred".
- engine.md:6061, Hud.h:32, Hud.cpp:49: the corner roots, "approximate".
- engine.md:6132: the scoreboard's black rows.
- engine.md:6263: the range finder.
- engine.md:6391: the name tags' "Guesses".
- engine.md:7584: the name's suffix.
- engine.md:8639, 8898, HudMenus.cpp:42, 409: the list columns, "fitted".
- engine.md:8825: the order icons.
- HudMenusVehicles.h:331.
- AnimationSystem.h:318: LookAroundTrigger's argument.
- main.cpp:536: the mute icon.
- engine.md:11395–11396: the +0x3c4 drawing.

**Network**
- engine.md:3864, HostedGame.cpp:2308, GameProtocol.h:458: CreateSpawnGroup.
- Networkables.h:221, 235, 258, 355, 369.
- RemoteWorld.h:157: the kick reason.
- engine.md:4077: the server code that sets it.
- engine.md:4679: the whisper's recipient.
- engine.md:4959: rcon_feedback.
- engine.md:5186: the server mask's bit 0x94.
- engine.md:5434: the game's own GameSpy keys from 0x35.
- HostedGame.cpp:159: the move buffer's depth.
- HostedGame.cpp:600: the CD-key reason.
- engine.md:4861, GhostPriority.h:78, HostedGame.cpp:2575: the dirty masks.

**Rendering**
- engine.md:1240: the Particle blend.
- engine.md:1633: the time override.
- engine.md:2141: the detail lists.
- engine.md:2242–2243: `0x1000b500`.
- engine.md:2349: maxLightsPerTerrainPatch.
- engine.md:2551: the rain's two speeds.
- engine.md:3041–3043: the zoomed-mode `+8` reader.
- engine.md:3103: the tracks.
- engine.md:3440: the tint.
- Effects.h:234, 239, 1147.

**AI**
- engine.md:9599, Bots.cpp:1612: the grenade warning's sender.
- engine.md:10654–10669, Bots.cpp:2860–2873: Mobile slot 0x78, the +0x84 attention stamp, +0x248.
- Bots.cpp:4077: the +0x14 that wasn't found.
- Bots.cpp:3118, 3448, 4288, 5326, 917.
- PathFollower.cpp:1229.
- Strategy.h:57.
- engine.md:1125–1128.

**Not settleable by decompilation** (unchanged; listed so a pass skips them):
- PathFollower.h:37 (real-time budgets);
- HudMenus.h:36, HostedGame.cpp:1217, engine.md:8637 (no human on the other end);
- Bots.h:1123, engine.md:9407 (test defaults);
- LevelRenderer.h:4, Console.h:76, ClipMethods.cpp:3, VoipNatives.h:3, main.cpp:2986, OpenSpyEmulator.h:1, Sound.cpp:17, Ambience.h:18 (architecture);
- Ballistics.cpp:132, 186, engine.md:10326, 10341 (algorithm terms);
- LevelRenderer.cpp:1371, OvergrowthLod.h:7, OvergrowthSettings.h:26, engine.md:2232 (fitted to the terrain by design);
- engine.md:3184 (a design choice);
- RemoteWorld.cpp:588, 611, NetDiagnostics.cpp:195, main.cpp:1985 (diagnostics).

## 4. Inconsistencies

1. **engine.md:3417–3422 is stale.** It says the TVEffect (`0x10048f80`) "nothing in RendDX9 calls it … and the game has no TV-guided view, so it is implemented (`PostEffects::tv`) but never on". Since e6bb0b4, engine.md:1381–1401 and main.cpp:6719–6722 turn it on while the player views through a guided round's camera.
2. **"The seats carry one gun"** is stale since 8dc3e13, which gave seats three weapon groups:
   - engine.md:6281 (the Secondary* block stays 0);
   - engine.md:6329–6330 (useSelectionIcons);
   - main.cpp:1205–1207.

   roadmap.md row 8 lists "the Secondary* block (0x007c2d50)" as done. `WeaponHud::update`'s secondary path exists, but nothing calls it, so the block never shows (gap L3).
3. **roadmap.md is stale.**
   - Rows 8–12 still begin "Started:", as in coverage-3 and coverage-4.
   - Row 8's "To do:" list still names lock-on weapons and heat seekers (done in 5d5403b), remote players (milestone 10 covers them) and the commander rose's lists (`RadioInterface` / `Interface_` is in src/Main/HudMenusComm).
   - Row 8 also says "the rewrite has no overhead camera: the commander map centres on the asset". That predates 5d5403b, which added the server's overhead camera and a joined commander's view of its ghost. The offline commander view still centres on the asset, so the row should say which case it means.
4. **coverage-4's sway fix didn't take.** Its status says inconsistency 4 (the composed keys, `sway.*` among them) is fixed. The `sway.gunSway*` rows stayed unread until this audit's `rows.py` fix (22 rows, 78 calls).
5. **The audits inflate their own citation figures.** `cite.py` (coverage-3's rule) counts docs/coverage*.md and docs/uncited.md, whose gap lists and uncited-run tables cite addresses nothing implements.
   - At HEAD they account for 155 EXE functions / 60 KB (1.2 points), 81 RendDX9 functions / 52 KB (3.1 points) and 7 AIDLL functions.
   - coverage-4.md alone accounts for 22 / 7 / 2 functions. They include the FESL run start `0x00840370`, the network event table `0x0042ed10` and the IDKey tables `0x006841f0`, which the classifier now calls "cited".
   - `--no-audit-docs` gives the figures without them (section 1b). Future audits should quote both or switch.
6. **Settled properties documented only in an audit.** 9c4af0b's message says `detonateOnWaterCollision` and `detectionRadius` were "decoded and documented". engine.md names neither. The settlement (read only by the saves `0x005e97b0` / `0x005e9b56`) is in coverage-3.md:275 alone, so both still count as unread (34 + 62 calls). The same line says DefaultPenetrateComp `0x005eaca0` "has no caller". coverage-4's L1 status and engine.md later found it is called through the projectile's slot `0xd4` (`0x005b54e0`). Other settled names engine.md does cover are still unread by the literal rule, as designed: `ammo.ammoType`, `allowReplenish`, `damageOwner` (engine.md:7327–7331); `weaponHud` / `vehicleHud.disableOnSprint`, `vehicleHud.typeIcon` (6316–6322); `cullRadiusScale`, `allowEmoteAnimation` (2879–2883).
7. **Features since 69c8fbc with no test.** 33 tests were added and most passes have one. These have none:
   - `target.disableGuidanceOnReload` (GameWorld.cpp:7534);
   - the squad radar's decoy and vehicle markers (main.cpp:908–910; NetHost_WhisperTakeableAndRadar checks only `radarPlayers`);
   - the commander-menu rows `zoomCommander` / `detailedView` / `spottedMouseOver` / `sendRadioMessage` and `SquadLeader.issueSquadRequest` / `sendRequestForOrder` (main.cpp:4313–4450);
   - the TVEffect while viewing through a round (main.cpp:6719–6722);
   - `BF2142_BOT_SEED`.

   The wire-guided look-ahead (GameWorld.cpp:2776) is checked only through BotsTest's target-type table.
8. **Tests.** 698 tests. `Voip_LoopbackSession` fails every run (VoipTest.cpp:550, 551, 560), and `Voip_RefusedGameServers` (597, 599) fails intermittently, as in coverage-3 and coverage-4. src/Voip hasn't changed, so this still points at the environment or timing.
9. **No "done in the docs, stubbed in the code" regressions** among console rows: no impl → stub or read → unread moves since 69c8fbc.
10. **Measurement artefacts.**
    - The level runs print only the top 25 stubs. `boundingRadiusModifier` 12 → 22 and `setIfTitanChangeToWeapon` 16 → 22 in the merged list are rows that moved up, not regressions.
    - The start-up count (507 / 125) still includes the 158 host-only methods.
    - AIDLL's own console (282 commands) is in neither console table.

## Raw outputs

Outside the repo: `scratchpad/c5head/` (4912c02) and `scratchpad/c5before/` (69c8fbc). Each holds:
- `probe_*`, `calls.json`;
- `rows.json`, `rows_summary.json`, `tableA.md`, `delta.md` (HEAD), `stubs_called.md`, `unread_called.md`, `unread.json`;
- `runs/`, `runs.json`, `runs_table.md`;
- `cite.json`, `cite_tests.json`, `cite_headings.json`, `cite_noaudit.json`;
- `uncited_{with,without,headings,noaudit,noaudit_without,recal}.json`, `labels_*`;
- `hedges.txt`.

The test logs are `scratchpad/c5_tests.log` and `c5_tests2.log`.
