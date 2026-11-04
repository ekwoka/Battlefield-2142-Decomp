# BF2142 rewrite: fourth coverage audit (2026-11-04, HEAD 69c8fbc)

This audit compares HEAD against docs/coverage-3.md, which was measured at 5480574, nine commits back.

## How this was measured

Earlier audits ran their scripts from a temp folder, and those scripts are lost. This time the scripts live in `tools/coverage/`, rebuilt from the method notes in coverage.md, coverage-2.md, uncited.md and coverage-3.md (`tools/coverage/README.md`). `tools/coverage/run_all.sh` runs everything.

Every figure below was measured twice with the same scripts:
- **before:** a `git worktree` build of 5480574 (`tools/coverage/build_old.sh`, offline, removed afterwards);
- **after:** a clean worktree build of 69c8fbc. The main checkout picked up other uncommitted edits to src/ and docs/ while this audit ran, so `build/release` and the working tree weren't used for the HEAD figures.

No window was opened. The probe and the level runs used `--simulate` (and `--screenshot` for one probe mode) with `--nosound --no-movies`. The host probe used `--host --dedicated` without `--online`.

### How well the rebuilt scripts reproduce coverage-3 (run on 5480574)

| Measurement | Reproduces coverage-3? |
|---|---|
| Console probe (`probe.py`) | **Exactly**: 1,034 / 1,654 for the union; 876 with `--simulate` alone; 158 names only when hosting; `--screenshot` adds nothing. |
| Row classes (`rows.py`) | **Exactly**: 1,042 impl, 476 stub, 733 stored-read, 586 stored-unread. EXE 850 / 280, RendDX9 192 / 196. 555 unread names, lower bound 476. 71 called stub rows, 403 called unread rows, 382 called unread names. |
| Level runs (`runs.py`) | **Exactly**: every "N calls to M" (5,694–6,622 / 31–52), the unknown-method counts (6,689–7,599) and the start-up 521 / 195. |
| Corpus (`corpus.py`) | **Approximately**: 6,702 files and 816,424 calls, against 6,758 / 824,614. coverage-2's exact file rules weren't recorded. Call totals therefore run 0.2–2% under coverage-3's: stub calls 24,432 against 24,486; unread rows 38,899 against 39,651. Both sides of every delta below use this corpus. |
| Citations (`cite.py`) | **EXE and SwiffPlayer exact** at 5480574, 991d041 and 6001a99 (EXE 2,678 functions / 1,154,647 B, 12 uncited runs / 0.81 MB at 5480574). RendDX9 is one function high (`0x100b2af0`, +1,084 B) and AIDLL one function low (−18 B), at all three commits. |
| Uncited classifier (`uncited.py`) | **Rebuilt from the rule list**: the original's string lists and thresholds were lost. At 5480574 the EXE and AIDLL land within about 2 points of coverage-3 (EXE calibrated 80.7% against 80.9%, AIDLL 60.7% against 59.9%). RendDX9 comes out about 6 points low (64.2% against 70.1%), because it labels more of RendDX9 missing (268 KB raw (d) against 153 KB). The calibration factors 0.84 / 0.69 / 0.48 are reused as asked. This classifier's own propagation precision (`--recalibrate`) is 0.47 / 0.72 / 0.35 at 5480574 and 0.46 / 0.69 / 0.50 at HEAD, so the EXE factor is generous for it. |

Outputs: `scratchpad/c4clean` (HEAD), `scratchpad/c3check` (5480574), both outside the repo. `run_all.sh` regenerates them into `build/coverage/`.

## 1. Headline numbers, before → after

| Metric | coverage-3 (published) | 5480574 (these scripts) | **HEAD 69c8fbc** | Change |
|---|---|---|---|---|
| Probe, distinct names: impl / stub (union of `--simulate`, `--host`, `--screenshot`) | 1,034 / 1,654 | 1,034 / 1,654 | **1,088 / 1,600** | +54 |
| … `--simulate` alone | 876 | 876 | 930 | +54; still 158 host-only names; `--screenshot` adds nothing |
| Console rows (2,837): impl | 1,042 | 1,042 | **1,096** | +54; no impl → stub regressions |
| … stub | 476 | 476 | **422** | −54 |
| … ObjectTemplate stored-read | 733 | 733 | 786 | +53 |
| … ObjectTemplate stored-unread (rows) | 586 | 586 | **533** | −53; no read → unread moves |
| Unread template property names (literal rule) | 555 | 555 | **502** | −53 |
| … lower bound (last segment absent from src) | 476 | 476 | **424** | −52 |
| BF2142.exe rows: impl / stub | 850 / 280 | 850 / 280 | 856 / 274 | |
| RendDX9 rows: impl / stub | 192 / 196 | 192 / 196 | **240 / 148** | the renderer's switches, decals, rain, roads, undergrowth |
| Start-up "console methods stubbed": EXE / RendDX9 | 521 / 195 | 521 / 195 | 515 / 147 | still counts the 158 host-only methods |
| Stub rows called by shipped scripts / calls | 71 / 24,486 | 71 / 24,432 | **60 / 24,138** | 16,416 are `Object.setLightSourceMask` (inert); without it 8,016 → 7,722 |
| Unread rows called / calls | 403 / 39,651 | 403 / 38,899 | **352 / 37,236** | unique names 382 / 38,060 → 331 / 36,397 |
| Level runs: "N calls to M unimplemented methods" | 5,694–6,622 / 31–52 | 5,694–6,622 / 31–52 | **5,644–6,568 / 26–42** | |
| Python tracebacks (11 runs) | 0 | 0 | 0 | |
| "Unknown object or method!" per run | 6,689–7,599 | 6,689–7,599 | 6,689–7,599 | unchanged per run; names retail doesn't register either |
| Unit tests | 596, 2 failing | | **665, 2 failing** | `Voip_LoopbackSession` (VoipTest.cpp:550, 551, 560; 3 of 3 serial runs) and `Voip_RefusedGameServers` (VoipTest.cpp:597, 599; 1 of 2 full runs) |
| Hedge-note hits (`hedges.py`; engine.md + src) | (different grep) | 197 | 219 | more code, more notes; see section 3 |

### Newly implemented console methods (54)

- **RendDX9 renderer (30):**
  - `renderer.vsync`, `presentAsync`, `useSunScaling`, `alphaToMask`, `enableHWOcclusion`, `renderAheadLimit`;
  - `drawFps`, `drawGraphs`, `hud`;
  - the draw switches `draw1pMeshes`, `drawBundledMeshes`, `drawNametags`, `drawObjects`, `drawOvergrowth`, `drawParticles`, `drawRoads`, `drawSkinnedMeshes`, `drawSkyDome`, `drawStaticMeshes`, `drawSunFlare`, `drawTerrain`, `drawTrees`, `drawUndergrowth`;
  - `minCullDistance`, `tweakNearPlane`, `tweakNearPlane1p`;
  - `Renderer.disableFadeOnRoads`, `fieldOfView1p`, `roadDepthBias`, `roadLodDistMod`, `roadSlopeScaleDepthBias`.
- **DecalManager (7):** `decalGeomClipAngle`, `decalNormalOffset`, `drawStats`, `fadeDistance`, `fadeOutAtPercentageOfTimeToLive`, `startFadePercentageOfFadeDistancel`, `timeToLive`.
- **Rain and undergrowth (8):**
  - `rainManager.freeze`, `randomness`, `maxRandomChange`, `randomDeviationAngleSpeed`;
  - `undergrowth.enabled`, `viewDistanceScale`, `viewDistanceFade`, `zPassPatchCount`.
- **StaticMeshRenderer:** `enableRendering`, `noLods`.
- **EXE (6):** `Object.disableChildren`, `ObjectTemplate.ability.setOverheatMod`, `Material.triggerRadio`, `MaterialManager.setRadioMessage`, `Sound.windSetup`, `Vars.set`.

### Template properties that became read (53)

These are the local-gameplay and sound passes reported in coverage-3's status notes:
- the sprint, beacon and kit abilities and `abilityInVehicle*`;
- the wreck blast `armor.wreckExplosion*`;
- the camera shake;
- the fire flags `busyUntilButtonRelease`, `pullbackTime`, `addBarrelName`, `fireInCameraDof`;
- the detonation's `maxDepthForExplosion` and `useCollisionNormal`;
- the animated UVs (11 names) and `rotateUV`;
- sound: `listenerObstruction`, `damagedAmbientSoundLimit`, `audio.newStyleAudio`, `sound.noisy`, panting and injury sounds;
- the HUD: `sendDestroyedHudEvent`, `guiIndex`, the visual deviation limits;
- seats and spawns: `disableCloseSpawnPoints`, `exitThroughPodLauncher`, `allowTogglePosition`, `teamFromClosestCP`, `lookAtClosestTitanOnEnter`;
- also `isDogtagReplacement`, `directionalScale`, `minDistanceUnderWaterSurface` and `setNumWindows`.

### Objects that changed (Table A; full table in `c4clean/tableA.md`, deltas in `c4clean/delta.md`)

| Module | Object | Rows | impl | stored-read | stored-unread | stub | script calls to stubs |
|---|---|---|---|---|---|---|---|
| BF2142.exe | ObjectTemplate | 1403 | 64→65 | 733→786 | 586→533 | 20→19 | 37→29 |
| RendDX9 | renderer | 66 | 35→61 | | | 31→5 | 2→0 |
| BF2142.exe | Object | 47 | 7→8 | | | 40→39 | 16,568→16,424 |
| RendDX9 | rainManager | 24 | 17→21 | | | 7→3 | 121→29 |
| RendDX9 | DecalManager | 7 | 0→7 | | | 7→0 | 0 |
| RendDX9 | Renderer | 5 | 0→5 | | | 5→0 | 0 |
| RendDX9 | undergrowth | 10 | 2→6 | | | 8→4 | 74 |
| RendDX9 | StaticMeshRenderer | 10 | 0→2 | | | 10→8 | 0 |
| BF2142.exe | Sound / Material / MaterialManager / Vars | | +1 each | | | −1 each | 1 / 11 / 10 / 27 → 0 / 1 / 0 / 0 |

### Stubs still called by the shipped scripts (60 rows, 24,138 calls)

**Not gaps** (load-time hints, editor rows, own parsers, inert):
- `Object.setLightSourceMask` 16,416;
- `GeometryTemplate.compressVertexData` / `maxTextureRepeat` / `lightmapOverride` / `doNotGenerateLightmaps` / `setLightmapSizeAll` / `isAd`;
- `heightmap.setModified` 342, `awards.setData` 126, `undergrowth.load` 74;
- the `Overgrowth.*` / `OvergrowthType.*` rows (21 rows, 35–62 calls each). LevelRenderer reads `Overgrowth.con` itself (LevelRenderer.cpp:1321), see section 4;
- `texturemanager.customTextureSuffix`, `terrain.init`, `rainManager.cellDisplay`, `Console.allowMultipleFileLoad`;
- the `RagDoll.*` rows (own parser).

**Player-facing or uncertain:**

| Calls | Row | Note |
|---|---|---|
| 30 | `GameLogic.radialLevelIndex` | unknown; levels set 3 |
| 24 | `ObjectTemplate.addVehicleType` | its min arity is 4; the scripts' 1-argument calls fail in retail too |
| 17 | `AITemplatePlugIn.cullDistance` | AI cull distance |
| 8 | `Object.notInAI` | |
| 5 / 2 / 2 | `weaponTemplate.shouldRearm` / `fireRate` / `isAntiAirWeapon` | AI weapon hints |
| 3 / 3 / 3 / 2 | `AITemplatePlugIn.setSoundSphereRadius` / `setHearingProbability` / `setUseNoPathfindingToGetToObject` / `vehicleNumber` | bot hearing |
| 3 / 2 | `ObjectTemplate.setCheckObstruction` / `setIfTitanChangeToWeapon` | |
| 2 | `GeometryTemplate.boundingRadiusModifier` | |
| 1 each | `GameLogic.enableVoHelp` / `supplyDropHeight` / `supplyDropNumSecsToLive` / `createObject`, `Material.pythonEvent` | |

### Level runs (calls / methods)

| Run | 5480574 | HEAD |
|---|---|---|
| Belgrade gpm_nv 32 | 6,507 / 42 | 6,452 / 32 |
| Camp_Gibraltar gpm_cq 32 | 6,358 / 47 | 6,305 / 41 |
| Cerbere_Landing gpm_cq 32 | 6,185 / 43 | 6,131 / 34 |
| Highway_Tampa gpm_sl 64 | 6,622 / 43 | 6,568 / 34 |
| Minsk gpm_ti 48 | 6,126 / 51 | 6,072 / 42 |
| Molokai gpm_ti 48 | 5,751 / 44 | 5,701 / 39 |
| Operation_Shingle gpm_sl 32 | 5,694 / 31 | 5,644 / 26 |
| sidi_power_plant_coop gpm_coop 32 | 6,269 / 37 | 6,215 / 31 |
| Suez_Canal gpm_cq 64 | 6,394 / 47 | 6,343 / 41 |
| Verdun gpm_sp 16 | 6,520 / 52 | 6,465 / 42 |
| Wake_Island_2142 gpm_ca 64 | 5,736 / 49 | 5,682 / 40 |

- **Left the stub report** (merged over the 11 runs): `Vars.set` 297, `Material.triggerRadio` 110, `MaterialManager.setRadioMessage` 110, `ability.setOverheatMod` 12, `Object.disableChildren` 7.
- **What remains:** the load hints (compressVertexData 15,756, maxTextureRepeat 15,734, lightmapOverride 13,292, doNotGenerateLightmaps 13,226, isAd 2,541), setLightSourceMask 5,241, the RagDoll rows, `heightmap.setModified` 99, `setCheckObstruction` 33, the Overgrowth rows and `setIfTitanChangeToWeapon` 16.

### Address citations (start addresses; src/ and docs/, tests excluded)

| Module | functions / bytes | coverage-3 (5480574) | **HEAD without appendix** | **HEAD with appendix** |
|---|---|---|---|---|
| BF2142.exe | 35,589 / 4,794,004 | 2,678 fns, 1,154,647 B (24.1%) | **2,895, 1,221,549 B (25.5%)** | **3,795, 1,326,161 B (27.7%)** |
| RendDX9_ori.dll | 10,810 / 1,677,614 | 339, 233,155 B (13.9%); these scripts 340 / 234,239 | **463, 315,910 B (18.8%)** | same |
| AIDLL.dll | 6,080 / 1,006,188 | 388, 261,236 B (26.0%); these scripts 387 / 261,218 | **820, 333,024 B (33.1%)** | same |
| SwiffPlayer.dll | 5,756 / 936,991 | 168, 124,680 B (13.3%) | 169, 124,725 B (13.3%) | same |
| BF2VoipServer.dll | 377 / 38,482 | 34 (28.5%) | 34 (28.5%) | same |
| BF2142Audio.dll | 1,702 / 206,940 | 2 | 2 | same |
| Memory.dll / TextureAtlasBuilder.dll | | 0 | 0 | 0 |

- **The appendix** (engine.md "Appendix: where the rewrite reads the exe's template properties and console rows") cites 900 EXE functions / 104,612 B that nothing else cites, all of them property executors and console-row constructors. It adds 2.2 points to the EXE's raw figure and nothing to the effective figures (section 1b).
- **Ambiguous DLL addresses dropped:** 183 → 429. The rise comes from engine.md's AIDLL console tables, which run more than 1,500 characters past the last "AIDLL" mention. Adding a fallback to the DLL named in the enclosing `##` heading (`cite.py --headings`) gives AIDLL **1,039 functions / 369,191 B (36.7%)**, RendDX9 472 / 320,801 B (19.1%), and 176 ambiguous. That matches coverage-3's own status line for the AIDLL pass (1,044 / 375,347 B, counted with tests). At 5480574 the same fallback changes nothing for AIDLL and adds 5 RendDX9 functions.
- **With tests/:** EXE 3,846 / 1,336,355 B; RendDX9 464 / 316,234 B; AIDLL 823 / 336,474 B.
- **Uncited runs, EXE (≥30 KB):** 12 / 0.81 MB → 11 / 0.68 MB without the appendix, 9 / 0.59 MB with it.
  - The top-level template property setters (0x004e88de, 99 KB) shrink to 42 KB without the appendix and drop out of the list with it.
  - With the appendix, the HudBuilder run (0x00761cc3, 44 KB) also drops out.
  - What is left is FESL/EASTL (0x00840370 157 KB, 0x00817850 121 KB, 0x008025b0 69 KB, 0x00870cb0 36 KB), the command line, profile and settings managers (0x0046c3f0, 52 KB), file paths (0x006971b0, 37 KB), static init (0x00900c64, 36 KB) and GameSpy GP (0x0072a180, 36 KB).
- **Uncited runs, RendDX9 (≥20 KB):** 20 / 0.66 MB → 15 / 0.50 MB. The largest:
  - debug drawers 0x10189d40 (81 KB);
  - the terrain editor 0x10154610 (48 KB);
  - IGA 0x10057740 (44 KB);
  - the SwiffRender / name-tag glue 0x1016d1c0 (38 KB);
  - 0x1008f720 (37 KB);
  - roads 0x10101ea0 (31 KB);
  - particles 0x10141350 (26 KB).
  - The shader-management run 0x100189a0 (47 KB) is gone: the replaced-by-design anchors are cited now.
- **Uncited runs, AIDLL (≥20 KB):** 5 / 0.22 MB → 2 / 0.06 MB (0x10044aa0 32 KB, 0x1002ae90 25 KB). The 86 KB console-command run 0x10009120 is cited (BotsScript.cpp, engine.md "AIDLL's console and scripted bots").

### 1b. Effective coverage (the uncited classifier, `uncited.py`)

Needed code is the total minus (a) third-party and (b) dev-only. "Lower" is cited plus string- or console-evidenced (c). "Calibrated" adds propagated (c) × the factor and splits "?" pro rata. "Upper" counts all (c) and "?". The before and after columns come from the same classifier, at 5480574 and at HEAD.

| Module | Needed | Cited (of total) 5480574 → HEAD (without / with appendix) | Lower | **Calibrated** | Upper | (d) calibrated | Raw (d) |
|---|---|---|---|---|---|---|---|
| BF2142.exe | 3.66 → 3.67 MB | 24.1% → 25.5% / 27.7% | 47.7% → **49.6%** (both) | 80.7% → **81.7%** (both) | 87.1% → 87.9% | 0.71 → **0.67 MB** | 461 → 434 KB |
| RendDX9_ori.dll | 1.28 → 1.29 MB | 14.0% → 18.8% | 32.2% → **36.4%** | 64.2% → **69.9%** | 78.5% → 84.8% | 0.46 → **0.39 MB** | 268 → 191 KB |
| AIDLL.dll | 0.87 MB | 26.0% → 33.1% | 34.4% → **41.8%** | 60.7% → **66.6%** | 88.5% → 93.0% | 0.34 → **0.29 MB** | 97 → 59 KB |
| All three | 5.80 → 5.83 MB | | 42.3% → **45.5%** | 74.1% → **76.8%** | | 1.51 → **1.35 MB** | |

- **With and without the appendix the effective figures are identical.** The appendix's addresses were already console-evidenced (c) (the rows they register are impl or stored-read), so it moves only the raw cited share (EXE 25.5% → 27.7%) and the "c evidenced" bucket (581 → 479 KB) into "cited".
- **With `--headings` citations** (AIDLL's tables attributed by their section): AIDLL is 44.2 / 68.3 / 94.2%, RendDX9 36.8 / 70.2 / 85.2%, all three 45.9 / 77.1%. About 40 KB of AIDLL's newly cited rows are debug or not-rewritten console commands, so the cited share overstates it by about 4.5 points.
- **On coverage-3's published series:** adding this classifier's deltas to coverage-3's figures gives EXE ≈ 80.9 + 1.0 = **82%**, RendDX9 ≈ 70.1 + 5.7 = **76%**, AIDLL ≈ 59.9 + 5.9 = **66%**, all three ≈ 75.6 + 2.7 = **78%**. These are approximate. On this classifier, RendDX9's absolute figure is 6 points lower (see the reproduction table).
- **Reading.** RendDX9 moved most (render pass 5: the switches, decals, roads, rain and undergrowth, and the D3D9 layer classed as replaced by design). AIDLL is next (the console and scripted-bot pass). The EXE moved about a point, mostly game-logic citations. The appendix doesn't count as new behaviour.
- **What remains in (d)** is fragmented: no (d) run above 11 KB in the EXE or 9 KB in RendDX9 (`c4clean/uncited_with.json` → `d_runs`). The largest EXE runs:
  - the template proxy / grid code 0x006e8570 (11 KB);
  - the emitter `*Used` setters 0x0053cc2e. These are a false (d): they are read through composed keys, see section 4;
  - the IDKey / IDButton name tables 0x006841f0. These are a false (d): the rewrite parses these names;
  - `RagDoll.save` / `reset` and `Console.get*` (editor);
  - the `sway.*` setters 0x004e9071 (composed keys);
  - `Object.initGrid`;
  - the network event name table 0x0042ed10;
  - `SquadManager.makeMeCommander` / `popNextCommanderApplicant` 0x00419419.

## 2. Remaining decodable gaps, ranked by player visibility

Effort: **S** = days, **M** = 1–3 weeks. "Unread" means the property name is in neither a string literal nor any src text (the lower-bound rule). Calls are from this corpus. Template offsets and executors are from `symbols/BF2142.exe.template_properties.csv`. To decode each one, find its offset's reader in the decompilation. Where that reader isn't traced, the "What a player notices" column goes by the property's name and the templates that set it.

### 2a. Local play

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| L1 | **Projectile, detonation and penetration remainder** | Rounds that should pass through solids or water don't, or the reverse. Proximity fuses don't trigger by radius. Rounds don't go off on hitting water. The active-defence shield doesn't stop projectiles. Hit impulses use one scale. | Unread: `penetrate.allowSolidPenetration` 94 / `allowLiquidPenetration` 57 (comp 0xc563 +0xc, 0x004f2c86); `detonation.detectionRadius` 62 (0x004f35b0); `detonation.detonateOnWaterCollision` 34 (+0x68, 0x004f3fb8); `hitImpactModifier` 51 (GenericProjectile +0x298); `activeDefenseEnabled` 37 (+0x336; the reader in the collision response 0x006c07a0 is traced but not rewritten). engine.md:6757 "Not traced: the detonation …". | S–M |
| L2 | **Weapon handling remainder** | Going by the names (the readers aren't traced): no delay before a drawn weapon can be used, no alternative item slot, the retail magazine-toggle and replenish rules missing, fire not blocked by vehicle tilt. | Unread: `delayToUse` 95 (GenericFireArm +0x2b8, 0x004e1a2d); `altItemIndex` 60 (+0x2e4); `ammo.ammoType` 84, `ammo.allowReplenish` 40 (+0x42), `ammo.instantReloadOnEnable` 34 (+0x29), `ammo.toggleWhenNoAmmo` 31 (+0x40), `ammo.changeMagAt` 16; `fire.showWeaponAfterReloadDelay` 18; `fire.maximumVehicleTiltForFire` 13 (+0x7c); `weaponHud.altCrosshairIcon` 10; `fire.fireInCameraDofLocked` 1. GameWorld.cpp:782 (addExtraAmmoItem's flag consumer not traced; engine.md:6438). | S |
| L3 | **Guided weapons and lock-on** | Missiles keep guiding through a reload. Re-lock timing, wire-guided look-ahead and camera-direction targeting use the rewrite's rule. | Unread: `target.disableGuidanceOnReload` 44 (comp 0xc50c +0xb1), `seek.reLockTime` 20 (0xc557 +0x28), `target.targetObjectTemplate` 23, `target.useCameraDirectionToTarget` 14, `target.wireGuidedLookAheadDistance` 12. The lock-on component's object update is not decoded (engine.md:6490, GameWorld.cpp:3673). | S–M |
| L4 | **Vehicle details** | Going by the names: the hover vehicles' camera doesn't take over the input, the APC pods' ability isn't used by itself, rotational bundles turn with their parent, walker bundles use the default bone, owner damage and the deep-water damage delay are missing. | Unread: `takeOverInput` 75 (Camera +0x42c, 0x005276f0); `autoUseAbility` 109 (PlayerControlObject +0x46a, 0x004fed8a); `ignoreParentRotation` 94 (RotationalBundle +0x344..+0x34c); `boneIndex` 108 (SimpleObject, 0x0051d06d; mech bundles); `damageOwner` 27 (+0x32c); `armor.deepWaterDamageDelay` 18; `neverDrawAs1p` 46; `localPredictOnClient` 42; `soundFilter` 42 (Soldier). engine.md:645 (the wing's regulator; no shipped Wing sets it). | S each |
| L5 | **Kit and unlock items** | A firearm replacement unlock doesn't swap the kit's rifle. Items that should equip without the switch don't. Alternate templates aren't offered. Headwear doesn't drop. Beacons can't be picked up. | Unread: `firearmReplacement` / `isFirearmReplacement` 40 (ItemContainer +0x2c4, 0x004e0ab8), `replaceWithoutEquip` 32 (+0x2e0), `alternateTemplate1..4` 60, `dropHeadwear` 16, `pickupEnabled` 18 (Beacon). Stub `ObjectTemplate.ability.addExtraAmmoItemToKit`. | S |
| L6 | **Titan-mode details** | Control points that the 48-player Titan maps mark invisible are treated as visible; what `isVisible` governs (map icon, HUD, capture) isn't traced. Titan parts get no name tag on destroy and no health activation level. There is no combat-area text and no minimap area texture. | Unread: `isVisible` 111 (ControlPoint +0x298, 0x00503481; every gpm_ti 48 GamePlayObjects.con sets it 0), `armor.addNametagOnDestroy` 22, `nametag.healthActivationLevel` 24. engine.md:7931 "Not rewritten: the HUD's text …, the minimap's area texture". | S |
| L7 | **Game-logic leftovers** | The commander's radial menu level, the supply drop's height and life, the slowed movement state, banked excess input, and where a squad member spawns when no spot fits. | Stubs: `GameLogic.radialLevelIndex` 30, `supplyDropHeight` / `supplyDropNumSecsToLive` / `enableVoHelp` 1 each. SoldierControl.cpp:87 (speed index 2), GameWorld.cpp:7894 (rememberExcessInput), GameWorld.cpp:3455 (0x005b2200's caller), 3509–3511 and engine.md:7976 (the spawn's extra test), GameWorld.cpp:1747 and engine.md:7870 (0x005a3bd0), GameWorld.cpp:4844 (how long a slumped body stays). | S each |

**Status (local gameplay pass: L1 penetration, L2–L7, after HEAD 9c4af0b):**
- L1 penetration, decided: retail rounds do pass. DefaultPenetrateComp (`0x005eaca0`) is called from the projectile's `+0x184` interface, slot `0xd4` (`0x005b54e0`), which reads the component at the projectile's `+0x1ac` as its own `+0x28`. The game logic's hit (`0x005a4dd0`) asks it after the hit's damage, when the round doesn't ricochet. The rewrite now applies the whole rule to every round with the component (`GameWorld::penetrate`): the solid/liquid flags, overrideNeverPenetrate, the material defaults (damageLoss −1, `0x0060a9b0`), one-sided minDamageLoss, the way out over 2.1 radii, the tenth-of-damage floor, the exit impact, and penetrationDeviation. So bullets go through grass, bushes, glass, cloth and thin metal. Silo missiles pass the Titans' shields (cell 0 at the shield, about 1,640 lost); bullets and rockets don't.
- L2 done: `delayToUse` (the enable `0x005c47d0`, the busy test `0x005c14f0`), `altItemIndex` (the kit's add-item `0x00589510`), `instantReloadOnEnable` with `autoReload` (`0x005f4bb0`), `toggleWhenNoAmmo` (`0x005f39c0`, `0x005f4a40`), `changeMagAt` (`0x005f4830`), and the reloadWithoutPlayer default 1. Found no runtime reader: `ammoType`, `allowReplenish` (network flag only), `damageOwner`. Left: retail's automatic reload only with `ammo.autoReload`.
- L3 done:
  - the lock-on update (`0x005e1b30`/`0x004cf290`): the nearest candidate of the round's target type, empty vehicles of either side, the losing timer in ticks;
  - `useCameraDirectionToTarget`;
  - heat and laser rounds home (SeekClosestTargetComp `0x005ec260`/`0x005ebea0`, `seek.reLockTime`; TTHeat rounds didn't home before);
  - the wire-guided look-ahead (`0x005e27a0`) with its range and obstruction drop;
  - `disableGuidanceOnReload` (`0x005e23b0`).

  Left: guidance passing to the next missile, `targetObjectTemplate`'s object.
- L4: `ignoreParentRotation` is done for the gunners' yaw (`0x00563180`); its pitch and the seat cameras' aren't. Decoded, not rewritten: `takeOverInput` (needs the projectile camera), `boneIndex` (needs bundleSkeleton posing, `0x0063d470`), `autoUseAbility` (only the help popup, `0x00646cb0`, a HUD matter). `damageOwner` has no reader.
- L5 done: firearm replacement and `replaceWithoutEquip` (`0x00570ab0`, `GameWorld::setOwnedUnlocks`; the profile doesn't feed the owned set yet), `altItemIndex`, the use key's pick-up of placed items and beacons (`0x004bce30`). `alternateTemplate*` are Emitter properties (effects), not kit ones.
- L6 done: `isVisible` / `showOnMinimap` in the control point info (`0x005bbe30`; only the scene's cull reads isVisible, the maps read showOnMinimap). The Titans' name-tag data: healthActivationLevel, the re-push count, the parts' percent (`0x00655460`, RendDX9 `0x100f40d0`). The combat-area text needs nothing more from the game side (HUD `0x0044c7b0`).
- L7 done:
  - `GameLogic.supplyDropHeight` / `NumSecsToLive` / `radialLevelIndex` / `enableVoHelp` are bound; radialLevelIndex is IGA's texture key, `0x10017f20`;
  - the zoomed walk (speed index 2, `0x00585e90`);
  - a squad spawn with no room is refused (`0x004bef90`);
  - an enemy's paddles do nothing (`0x004ba5c0`).

  Left: the navmesh test for bots' spawn tries, the mobile spawn point's CP rule (`0x005b24f0`), rememberExcessInput, and the slumped body's time.

### 2b. AI

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| A1 | **Bot perception** | Bots know every vehicle, ignore hearing, and aren't culled by distance. | Stubs `AITemplatePlugIn.cullDistance` 17, `setSoundSphereRadius` / `setHearingProbability` 3; engine.md:9155 and Bots.cpp:5786 "Stand-in: vehicles count as known"; allowedTimeDiff stored, not used. | M |
| A2 | **Bot fire-plan and behaviour stand-ins** | Bots never use the guided weapons' own plan on foot. C4 isn't guarded on moving targets. The air-target clause and the target's speed and attention are the rewrite's. | engine.md:9812–9828; Bots.cpp:2523–2536, 3814 (the upright test), 2769 (heal task), 4606; PathFollower.cpp:1229; the grenade warning's sender (engine.md:8886, Bots.cpp:1510); `weaponTemplate.shouldRearm` / `fireRate` / `isAntiAirWeapon` stubs. | M |
| A3 | **Scripted-bot commands left** | Mods' coop scripts that use them. | engine.md:9730, 9737, 9749, 9756, 9761: `botGiveOrder`, `createSAI`, `addPlacement`, the tree modifiers, landing zones. Shipped scripts barely call them. | S |

**Status (bot perception and plan pass, after 9c4af0b):**
- **A1 done.** Enemy vehicles are known per side (seen, or anyone aboard seen, or heard; certainty by degeneration). A unit stays sensed 2 s after it is lost and is placed where the side last had it (0x100ee430, 0x100a9b20). allowedTimeDiff throttles the knowledge's update (0x100a9e70). Hearing is rewritten (0x100ca280 → 0x100ec100 → 0x100ea820). Only listeners without a Mobile part hear, because 0x100317e0 is a max, and the shipped `weaponTemplate.setSoundSphereRadius` is no command, so every weapon keeps 50 m. `cullDistance` is decoded: it applies to neutral current targets only, so it has no effect on the rewrite. The three plug-in rows and `weaponTemplate.soundSphereRadius` are bound.
- **A2 partly done.** Done: BBFire3d's unit-count scale (0x100d2b30), its pass over the side's known manned units (0x100caab0) and the aircraft-targeting-us skip. Its and BBFire's 20 s stamps are decoded as never written (0x1007df70, 0x10075ec0). In the fire plan's +0x248, the spot is now the goal object's head (bot +0x80 slots 4/8, not the follower), the approach test is the pathfinder's 10-point test (0x1003fa60 → 0x1003d460), and the air clause is setIsAntiAircraft 2 (value 1 is overwritten unread). Still open: the guided weapons' plan on foot, C4 on moving targets, the target's speed and attention, and the ground transports' way out.
- **A3:** `botGiveOrder` is decoded and bound. It is refused, because no writer of the bot's +0x50 was found. The SAI's status rule for areas without a control point is still not rewritten.
- **Citations (strict rule):** AIDLL 823 / 333,582 B (33.2%) → 1,059 / 381,557 B (37.9%). Ambiguous occurrences: 429 → 130. With `--headings`: 1,042 → 1,089 functions. Most of the gain comes from naming AIDLL in the Bots code's comments and engine.md's Bots section (70 code occurrences). Some is the console table rows, whose "not rewritten" and debug rows now attribute too.
- **Rounds:** over 48 seeds of `BF2142_BOT_SEED` on Cerbere, 9c4af0b fires 13 % fewer rounds than 69c8fbc, with more kills and hits. The rebuild per weapon (0x1007cf10) is faithful. The single default seed's −80 % is chaos (engine.md, Fire).

### 2c. Joined and hosted play

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| J1 | **Spectator** | No spectator rules or HUD. | engine.md:6062 "Not done: the spectator's rules"; the menu's `readyToPlay(asSpectator)` isn't traced to the network. | M |
| J2 | **Commander's overhead camera on the server; squad minimap sight test** | The server keeps no commander camera object. The squad minimap shows players without the 60 m sight test. | main.cpp:3793, engine.md:8224; engine.md:4286 (the 60 m test and the vehicle's 2/0x2e). | S–M |
| J3 | **Ghost field meanings** | Minor state on clients. | Networkables.h:221 (mask 0x40000), 235 (+0x168 bits), 258, 355 (flag210), 369; GameProtocol.h:433; RemoteWorld.h:154 (the kick reason byte); engine.md:3461 and HostedGame.cpp:2166 (CreateSpawnGroup's bits and bytes); engine.md:3674. | S each |
| J4 | **Server odds and ends** | A long rcon_feedback, the ranked minimums, refusing a barred applicant after a mutiny, the CD-key refusal reason. | engine.md:4534, 4610; Commander.cpp:138; HostedGame.cpp:587. Also the ghost dirty masks' stand-in (GhostPriority.h:78, HostedGame.cpp:2433, engine.md:4436), which costs bandwidth but no state. | S |

### 2d. Menus and front end

| # | Gap | Evidence | Effort |
|---|---|---|---|
| M1 | **Squad and commander menu remainder** | Stubs `SquadManager.changeSquadName` / `joinSquad` / `leaveSquad`, `Squad.leaveSquad`, `Commander.zoomCommander` / `detailedView` / `spottedMouseOver`, `CommanderMenu.sendRadioMessage`, `SquadLeader.issueSquadRequest` / `sendRequestForOrder` / `addRemoveCP`. HudMenus.h:156–157 (the tabs' first state), 176–177 (Shift-add, slot 0x20; muting the leader's voice, slot 0x300), 238; engine.md:8202 (which order kind draws which icon), 8292, 8347 (the AI commander's answers). | S |
| M2 | **Demo playback controls** | Stubs `demo.togglePlayerDemo` / `toggleCameraDemo` / `adjustDemoFov` / `restartDemo` / `shutdownDemo` / `toggleDemoPauseOrPlay`; engine.md:4677 (message 9), 4693. | S–M |
| M3 | **HudBuilder authoring rows** | 24 stub rows (`createSliderNode`, `setBarNodeAnim`, `setButtonNodeFunction`, the animation-node setters…). No shipped HUD script calls them, so this matters only for mods. | S |
| M4 | **Awards pop-up** | 0x007d1270; shows only for the server's Medal events (online-gated). | S once events exist |

### 2e. Rendering

| # | Gap | Evidence | Effort |
|---|---|---|---|
| R1 | **Lights remainder**: specular, flicker, the per-mesh light choice | engine.md:1954–1955 (`lightManager.maxLightsPer*Mesh`, `lightCullDistance`) | M |
| R2 | **Overgrowth cross-fade and shadow map; decals on objects** | engine.md:1885–1887 (the full / LOD cross-fade and lodTimer 0x1008da30, the near-cell fade, the overgrowth shadow map); engine.md:2036, 2058 (the normal-mapped decal technique, +0x1a's refusal on an unseen owner, the static decals' list) | S–M |
| R3 | **Particle remainder** | engine.md:10617 (start age and the sound's intensity), 2638 (the zoomed-mode systems' `+8` reader), 962 (the Particle object's blend), 1319 (the per-instance time override); Effects.h:234, 239 (drawOrder, the systems' ground planes) | S |
| R4 | **Terrain detail and culling** | engine.md:1792 (the per-material detail lists); stubs `terrainCuller.cullerType` / `detailCullMaxLevel` / `updateLodLevel`, `terrain.waterQuality` / `waterNormalMap` | S |
| R5 | **View tint; name-tag distances** | engine.md:3037 (PostProduction slot 0x7c not decoded); `nametags.*` cull and fade stubs (9 rows; no shipped script sets them) | S |

### 2f. Not decodable or out of scope (unchanged)

- the online back end (FESL/EASTL, GameSpy GP, BFHQ, leaderboards, award grants);
- PunkBuster, IGA, SecuROM;
- the lightmapper and TextureAtlasBuilder (`LightmapGeneration.*` 50 rows);
- editor and debug objects (collisionManager, AnimationSystem editor rows, `Console.get*`, Overgrowth editor rows, `Player.*`, heightmapcluster);
- Novint Falcon and the G15 LCD;
- the AI's real-time CPU budgets.

### Top 15, overall

| Rank | Gap | Area | Effort |
|---|---|---|---|
| 1 | L1: penetration, proximity and water detonation, hit impulse, active defence | local | S–M |
| 2 | L2: `delayToUse` and the weapon and ammo flags | local | S |
| 3 | L3: guided weapons (guidance on reload, re-lock, wire-guided look-ahead) and the lock-on update | local | S–M |
| 4 | L4: vehicle details (`takeOverInput`, `autoUseAbility`, `ignoreParentRotation`, `boneIndex`, `damageOwner`) | local | S |
| 5 | L6: Titan mode's hidden control points (`isVisible` 0 on 111 CPs) and part name tags | local | S |
| 6 | L5: firearm replacement and `replaceWithoutEquip` unlocks, alternate templates, beacon pickup | local | S |
| 7 | A1: bot perception (vehicles count as known, hearing, cull distance) | AI | M |
| 8 | A2: bot fire plan and behaviour stand-ins | AI | M |
| 9 | L7: game-logic leftovers (radial level, supply drop, slowed state, spawn fallbacks) | local | S |
| 10 | J1: spectator | joined | M |
| 11 | J2: commander camera on the server, squad minimap sight test | joined | S–M |
| 12 | R1: lights (specular, flicker, per-mesh choice) | rendering | M |
| 13 | R2: overgrowth cross-fade and shadow map, decals on objects | rendering | S–M |
| 14 | M1: squad and commander menu remainder | menus | S |
| 15 | M2: demo playback controls | menus | S–M |

**Status (gaps 11–15, rendering/HUD/menus pass, after HEAD 9c4af0b):**
- **J2 done.** The server makes each team's overhead camera (MultiPlayerOverheadCamera, class 0x948c; 0x004c5120) on the commander's first CommanderCam and places it (0x00510b30 → 0x005f8410 / 0x005f8170); it goes out as its OverheadCamera ghost, and a joined commander's overhead view stands where the ghost is (0x00779680). The squad radar (0x00508590): enemies only (kind 0, other team), the Bp1_Decoy within 60 m shown instead of an enemy on foot (0x0075bd30), and a reported player's vehicle spotted (the local 2/0x2e) as a red map marker.
- **R1 done** except projector textures, the lights' shadows and the template speccolor: the per-frame light list within lightCullDistance (0x100a93f0), each mesh's nearest maxLightsPer*Mesh by its kind's filter (0x100a9f80 / 0x100a7f00; isDynamic only), the dynamic shadows' quality's table (0x10010040), each shader's point-light diffuse, specular and falloff, and flicker on the ranges (0x0064a920).
- **R2 done** except the alpha doubling (0x1000b500, not traced) and the normal-mapped decal technique: the full / LOD cross-fade and lodTimer (0x1008da30), the type factor (0x100917e0), the cells' nearest-corner fade (0x10120ef0) and near-cell fade (0x10121410); decals clipped to the hit object's collision faces. The overgrowth shadow map is settled as editor-only (read only by 0x1008acd0 for terrain style 6).
- **M1 done** but `SquadLeader.addRemoveCP` (an unused rally-point toggle): joinSquad / leaveSquad / changeSquadName, Squad.leaveSquad, zoomCommander, detailedView, spottedMouseOver, sendRadioMessage, sendRequestForOrder, issueSquadRequest; the squad list's Ctrl / Shift selection and the leader mute; the tabs' first state (0x007ac090).
- **M2 mostly done:** the game speed (Game.simulationRate, pause, restart keeping it), adjustDemoFov, togglePlayerDemo / toggleCameraDemo and the follow view (0x005f8f40). Left: the camera's own key input (0x005fa5e0), the HUD's Demo* strings, camera files.
- Inconsistencies 1, 2 (the sway), 4 (the composed keys: `rows.py` now counts a name two or three src literals concatenate to: 51 names, the bundles' modifiers, sway.* and alternateTemplate0) and 5 (the `Overgrowth.*` rows the levels call are registered, `scene/OvergrowthSettings.h`) are fixed.

## 3. Remaining hedges a further pass could settle

From `hedges.py` (219 hits at HEAD, 197 at 5480574). The 5480574 count was taken with the same grep; coverage-3's 50 + 58 used a different, hand-filtered grep.

Most of coverage-3's §3a list is still open, at new line numbers. Some items now have their addresses and are marked "Left out" or "Not rewritten": the guided weapons' plan 0x10078570 (engine.md:9828), the unit-count scaling 0x100d2b30 (engine.md:9464), and the ITAir room test 0x1006c3e0 (Bots.cpp:3099). Those are gaps (A2) rather than hedges.

By area:

**Physics, weapons, effects**
- engine.md:813, MeshCollision.cpp:63: the response's stuck push (+0x11c).
- engine.md:962: the Particle object's blend.
- engine.md:1319: the per-instance time override.
- engine.md:2148: the rain's two speeds' defaults.
- engine.md:2638–2640: the zoomed-mode systems' +8 reader.
- engine.md:3037: the view's tint (slot 0x7c).
- engine.md:7036–7037: the overhead view's renderer value (`DAT_00a2020c` slot 0x80).
- engine.md:8571: the call site of 0x005eaca0 (material penetration exit).
- engine.md:6490, GameWorld.cpp:3673: the lock-on component's update.
- engine.md:6757: the detonation's MTYVehicle test.
- engine.md:6438, GameWorld.cpp:782: addExtraAmmoItem's flag.
- Weapons.h:540: the deviation's slot 0x1a0.
- Effects.h:234, 239: drawOrder, ground planes.
- Effects.h:1137: Rain.fx Data.y.
- GameWorld.cpp:4273: the ground material on both sides.

**Spawning and game logic**
- engine.md:7870, GameWorld.cpp:1747: 0x005a3bd0.
- engine.md:7976, GameWorld.cpp:3509–3511: the spawn's extra test.
- GameWorld.cpp:3455: 0x005b2200's caller.
- GameWorld.cpp:4349: the hull-kill note.
- GameWorld.cpp:4844: the slumped body's time.
- GameWorld.h:19: activation by events.
- SoldierControl.cpp:87: the slowed state.
- GameWorld.cpp:7894: rememberExcessInput.
- engine.md:6921, Radio.cpp:95: the medic flag's setter.
- engine.md:7355: the ability sounds' start and stop.
- engine.md:4602: the friendly-fire kit list.
- Commander.cpp:138: the post-mutiny refusal.
- engine.md:10489: an engine setter for the kit screen.

**HUD**
- engine.md:5598: the `.dif` font layout, "inferred".
- engine.md:5618, Hud.h:32, Hud.cpp:49: the corner roots, "approximate".
- engine.md:5689: the scoreboard's black rows.
- engine.md:5818: the range finder's further reader.
- engine.md:5946: the name-tag "Guesses".
- engine.md:6992: the position of a name's suffix.
- engine.md:8019, 8247, HudMenus.cpp:42, 409: the list columns, "fitted".
- engine.md:8202: the order icons.
- engine.md:8239, HudMenus.h:156–157: the tabs' first state.
- HudMenusVehicles.h:320.
- AnimationSystem.h:318: LookAroundTrigger's argument.
- main.cpp:531: where the mute icon is drawn.
- engine.md:10552–10553: the +0x3c4 drawing.

**Network**
- engine.md:3461, HostedGame.cpp:2166, GameProtocol.h:433: CreateSpawnGroup.
- engine.md:3674, Networkables.h:235: the +0x168 bits' setter.
- Networkables.h:221, 258, 355, 369.
- RemoteWorld.h:154: the kick reason.
- engine.md:4264: the whisper's recipient.
- engine.md:4534: the long rcon_feedback.
- engine.md:4743: the server mask's bit 0x94.
- engine.md:4079, main.cpp:4524: the radio heard as the bots' stand-in.
- HostedGame.cpp:159: the move buffer's depth, "not traced in BF2142.exe".
- HostedGame.cpp:587: the CD-key reason.
- engine.md:4436, GhostPriority.h:78, HostedGame.cpp:2433: the dirty masks.

**Rendering**
- engine.md:1792, 1885, 2036, 2058, 2385, 2700, 10595, 10617.
- engine.md:2781: the view-fitted shadow map. This is a design choice, not a hedge.

**AI**
- engine.md:8886, Bots.cpp:1510: the grenade warning's sender.
- engine.md:9155, Bots.cpp:5786: vehicles count as known.
- engine.md:9463, Bots.cpp:3353: the +0x30 writer.
- engine.md:9812–9828, Bots.cpp:2523–2536, 4606: the air clause, the Mobile slot 0x78, the attention stamp, C4's guard.
- Bots.cpp:868: seat changes and pick-ups.
- Bots.cpp:2769, 3099, 3814.
- PathFollower.cpp:1229.
- Strategy.h:57.

**Not settleable by decompilation** (unchanged):
- the real-time budgets: PathFollower.h:37;
- no human on the other end offline: engine.md:8017, HudMenus.h:36, HostedGame.cpp:1203;
- the test-only defaults: engine.md:8694, Bots.h:1011;
- architecture: LevelRenderer.h:4, Console.h:76, the natives' stand-ins (ClipMethods.cpp:3, VoipNatives.h:3, main.cpp:2901), OpenSpyEmulator.h:1, Sound.cpp:17, Ambience.h:18;
- algorithm terms: Ballistics.cpp:132, 186, engine.md:9490, 9505;
- diagnostics: main.cpp:1903, RemoteWorld.cpp:546, 569.

## 4. Inconsistencies

1. **engine.md:10646–10648 is stale.** It says how SwaySpeed / SwayScale spread over the 16 undergrowth sway offsets "isn't traced, so the rewrite spreads the speeds over 1 ± 0.2". src/Terrain/Undergrowth.h:94–103 and LevelRenderer.cpp:1572 / 1678 have the decoded draw (0x1010ad20: speed = r/2^24 × speedDiff + speed, amplitude, phase), and uncited.md's render pass 5 calls it done.
2. **roadmap.md row 5 contradicts itself.** It says "the sway's per-frame walk is 0x1010a900's, the 16 offsets' speeds and sizes a stand-in", then later "the undergrowth's 16 sway offsets (0x1010ad20 / 0x1010a900)". Rows 8–12 still begin "Started:" (unchanged since coverage-3's note).
3. **The AIDLL citation figure depends on the attribution rule.** coverage-3's status line reports 1,044 functions / 375,347 B (37.3%), counted over src/AI, src/Game/Bots*, their tests and engine.md's Bots section. coverage-3's own audit rule gives 821 / 333,035 B (33.1%) at HEAD: engine.md's AIDLL console tables (engine.md:9730–9776) run more than 1,500 characters from any "AIDLL" mention, and their addresses collide with RendDX9's and SwiffPlayer's, so 429 occurrences are dropped. Both figures are given above. Future audits should say which rule they use.
4. **False positives of the unread rule.**
   - The literal rule and its lower bound both miss the effect bundles' modifiers: `alphaIntensity` / `emitScaleFactor` / `emitSpeedScale` / `startAge` / `AgeScaleFactor` / `soundIntensity` + `Range` / `Used` / `FromSpeed`, 16 names and about 1,000 calls. Effects.cpp:2146–2224 reads them as `base + "Range"` etc. startAge and soundIntensity are read but not applied (engine.md:10617). The classifier's (d) run 0x0053cc2e (9 KB) is the same false negative.
   - As before: `sway.*` (composed), `python.onRepairEvent` (Titans.cpp handles it as events), `anchor` / `pan`.
5. **The `Overgrowth.*` console rows are stubs, but the behaviour is done.** LevelRenderer reads `Overgrowth.con` itself (LevelRenderer.cpp:1321: viewDistance, viewDistanceScale…), so the 21 Overgrowth and OvergrowthType rows count 36–62 script calls each, and 5–21 per level run, as unimplemented while engine.md calls the view distance done. The `RagDoll.*` rows are the same pattern (own parser).
6. **Measurement artefacts to keep in mind.**
   - The level runs print only the top 25 stubs. The "new" entries in the merged list (`Overgrowth.addMaterial` 6, `Overgrowth.viewDistance` 5, `GameLogic.radialLevelIndex` 1, …) moved up when others left, not because something regressed.
   - The start-up "console methods stubbed" line (515 / 147) still counts the 158 host-only methods.
   - AIDLL's own console (282 commands, engine.md "AIDLL's console and scripted bots") is in neither `symbols/*.console.csv`, so the probe and Table A don't cover it.
7. **The generated appendix inflates raw EXE citations without adding behaviour.** It adds 900 functions / 104.6 KB (25.5% → 27.7% raw) and changes neither the lower nor the calibrated figure: every appendix address was already console-evidenced. engine.md:10719 says this itself. Coverage figures should quote the raw EXE share without the appendix, or both.
8. **Tests.** 665 tests, 2 failing, as in coverage-3. `Voip_LoopbackSession` (VoipTest.cpp:550, 551, 560) fails 3 of 3 serial runs (`-j 1 Voip_`). `Voip_RefusedGameServers` (VoipTest.cpp:597, 599) failed in 1 of 2 full runs. src/Voip and the test haven't changed, so this still points at the environment or timing.
9. **No "done in the docs, stubbed in the code" cases** among console rows: there are no impl → stub and no read → unread moves since 5480574.

## Raw outputs

These are outside the repo: `c4clean/` is HEAD (the clean worktree) and `c3check/` is 5480574. `tools/coverage/run_all.sh` regenerates them.
- `probe_{sim,host,shot,union}.json`, `probe_*.log`;
- `calls.json`;
- `rows.json`, `rows_summary.json`, `tableA.md`, `delta.md`, `stubs_called.md`, `unread_called.md`, `unread.json`;
- `runs/*.txt`, `runs.json`, `runs_table.txt`;
- `cite.json`, `cite_tests.json`, `cite_headings.json`;
- `features_*.json`, `uncited_{with,without,headings,recal}.json`, `labels_*.json`;
- `hedges.txt`;
- `tests.log`.
