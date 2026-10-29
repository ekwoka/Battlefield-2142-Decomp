# BF2142 rewrite: third coverage audit (2026-10-29, HEAD 5480574)

Against docs/coverage-2.md (measured at 991d041, 10 commits back) and docs/uncited.md (measured at 6001a99, 7 commits back).

## How this was measured

I re-ran the same tools on `build/release` at HEAD. `ninja -C build/release src/Main/bf2142` reported no work to do, so the binary was current. All runs used `--nosound --no-movies`, and no window was opened. Outputs are in `scratchpad/c3/`.

- **Console probe.** I ran `tools/probe2.py` three times on Camp_Gibraltar gpm_cq 32 `--simulate 1`:
  - plain;
  - with `--host --dedicated`;
  - with `--screenshot`.
  - I took the union of the three (`c3/probe_union.json`). Each row was then classified with `tools/analyze.py` against HEAD's src (`c3/rows_h3.json`). The tables come from `tools/tables.py`, compared with coverage-2's `rows_head.json` (`c3/tables3.py`, `c3/tableA.md`, `c3/stubs_called.md`, `c3/stubs_uncalled.md`).
- **Script corpus.** I reused coverage-2's corpus (`calls.json`: 6,758 files, 824,614 calls). Game data doesn't change, so both sides of every delta use the same corpus.
- **Level runs.** These are coverage-2's 11 combinations with `--verbose --simulate 5`, in `c3/runs/`.
- **Address citations.** `tools/cite2.py` over src/ and docs/, with tests excluded, as uncited.md did (`c3/cite_h3.json`). With tests/ included: `c3/cite_h3_t.json`. Uncited runs come from `tools/runs_uncited.py` (`c3/rc/`).
- **Uncited classifier.** `unc/classify.py` and `report.py` were re-run in `c3/unc/` with:
  - HEAD's citations;
  - HEAD's console rows;
  - a regenerated `srccorpus.json`.
  - The per-function features (`unc/<mod>.json`, from the decompilation) are unchanged.
  - The calibration factors are reused: EXE 0.84, RendDX9 0.69, AIDLL 0.48. I did not recompute them. Recomputing the 6001a99 figures with them gives uncited.md's 78 / 70 / 60 % exactly.
- **Tests.** `build/release/tests/bf2142_tests` with `BF2142_GAME_DIR` set.

## 1. Headline numbers, before → after

| Metric | coverage-2 (991d041) | HEAD 5480574 | Change |
|---|---|---|---|
| Probe, distinct names: implemented / stub (union of `--simulate`, `--host`, `--screenshot`) | 999 / 1,689 | **1,034 / 1,654** | +35 |
| … `--simulate` alone | 841 | 876 | +35. The 158 host-only names are unchanged; `--screenshot` adds nothing. |
| Console rows (2,837): impl | 1,007 | **1,042** | +35. No regressions from impl. |
| … stub | 510 | **476** | −34 |
| … ObjectTemplate stored-read | 619 | 733 | +114 |
| … ObjectTemplate stored-unread (rows) | 701 | **586** | −115. The only read → unread change is `coneAngle1`, now read only as coneAngle2. |
| Unread template property names (unique, literal rule) | 663 | **555** | −108 |
| … lower bound (last segment absent from src) | 578 | **476** | −102 |
| BF2142.exe rows: impl / stub | 840 / 289 | 850 / 280 | |
| RendDX9 rows: impl / stub | 167 / 221 | 192 / 196 | |
| Start-up line "console methods stubbed" (no `--host`): EXE / RendDX9 | 525 / 220 | 521 / 195 | still counts the 158 host-only methods |
| Stubbed methods called by shipped scripts: rows / calls | 85 / 25,157 | **71 / 24,486** | 16,416 of them are `Object.setLightSourceMask` (settled inert). Without it: 8,741 → 8,070. |
| Unread template properties called by scripts: rows / calls | 503 / 66,948 | **403 / 39,651** | unique names 475 / 58,164 → 382 / 38,768 |
| Level runs: "N calls to M unimplemented methods" | 5,944–6,872 / 42–63 | **5,694–6,622 / 31–52** | |
| Python tracebacks (11 runs) | 0 | 0 | |
| "Unknown object or method!" per run | 6,688–7,598 | 6,689–7,599 | unchanged; retail doesn't register these names either |
| Unit tests | 524, 1 failing | **596, 2 failing** | see section 4 |

### Newly implemented console methods (35, all since 991d041)

- **ObjectTemplate:** `armor.addArmorEffectSpectacular`, `armor.addWreckArmorEffect`, `replaceItem`.
- **GeometryTemplate:** `isTitanInterior`, `isTitanExterior`, `titanExteriorPartIndex`, `setMaterialReflectionScale`, `setSpecularStaticGloss`, `setMaterialParallaxHeightScale`, `nrOfAnimatedUVMatrix`.
- **ObjectDrawer:** `isInsideTitanOverride`, `showAllTitanParts`.
- **renderer:** `indoorBundledMeshCullDistance`, `indoorSkinnedMeshCullDistance`, `waterFogColor`, `waterFogStartEndAndBase`.
- **terrain:** eight `water*` setters, including `waterSunIntensity`.
- **nametags:** `createIcon`, `setTexture`, `createBar`.
- **Other objects:** `undergrowth.simpleShaderEnable`, `HudManager.addFavouriteServer` and `enableSayPlayerChatBox`, `IngameHelp.showIngameHelp`, `ItemSelection.setReverseItems`, `SquadLeader.sendRequest`, `Commander.commanderMutiny`, `Radio.answer`.

### Objects that changed (Table A'' — the full table is in `c3/tableA.md`)

| Module | Object | Rows | impl | stored-read | stored-unread | stub | script calls to stubs |
|---|---|---|---|---|---|---|---|
| BF2142.exe | ObjectTemplate | 1403 | 61→64 | 619→733 | 701→586 | 22→20 | 172→39 |
| RendDX9 | renderer | 66 | 31→35 | | | 35→31 | 78→2 |
| BF2142.exe | HudManager | 31 | 29→31 | | | 2→0 | 0 |
| BF2142.exe | Commander | 31 | 27→28 | | | 4→3 | 0 |
| RendDX9 | GeometryTemplate | 26 | 8→15 | | | 18→11 | 6346→6065 |
| RendDX9 | terrain | 14 | 2→10 | | | 12→4 | 114→38 |
| RendDX9 | nametags | 12 | 0→3 | | | 12→9 | 105→0 |
| RendDX9 | undergrowth | 10 | 1→2 | | | 9→8 | 74→74 |
| RendDX9 | ObjectDrawer | 8 | 0→2 | | | 8→6 | 0 |
| BF2142.exe | Radio / SquadLeader / ItemSelection / IngameHelp | | +1 each | | | −1 each | 0 |
| | **Total** | 2837 | 1007→1042 | 619→733 | 701→586 | 510→476 | 25157→24486 |

### Stubs still called by the shipped scripts (71 rows, 24,486 calls)

**Load-time, editor or own-parser rows (not gaps):**
- `Object.setLightSourceMask` 16,416 (inert);
- `GeometryTemplate.compressVertexData` 1,665;
- `maxTextureRepeat` 1,660;
- `lightmapOverride` 1,248;
- `doNotGenerateLightmaps` 1,242;
- `heightmap.setModified` 342;
- `GeometryTemplate.isAd` 231 (IGA ads);
- `awards.setData` 126;
- `undergrowth.load` 74;
- `Overgrowth*` / `OvergrowthType*` (14 rows, 35–62 each);
- `texturemanager.customTextureSuffix` 39;
- `terrain.init` 38;
- the `RagDoll.*` rows (own parser);
- `Vars.set` 27;
- `Console.allowMultipleFileLoad` 26;
- `rainManager.cellDisplay` / `freeze` (debug);
- `setLightmapSizeAll` 17.

**Player-facing or uncertain:**

| Calls | Method | Note |
|---|---|---|
| 144 | `Object.disableChildren` | **GAP.** It switches off the children of 144 placed objects (gastank_c 48, residential_house_01_e 38, guard_watchtower_01_c 20, cistern_small_c 18, …). Those children are **ladders** (`gastank_c.tweak: addTemplate ladder_5m_c`, `guard_watchtower_01_c.tweak: ladder_10m_c, ladder_6m_c`), so the rewrite offers ladders that retail removes. |
| 30 | `GameLogic.radialLevelIndex` | Unknown; levels set e.g. 3. |
| 26 | `ObjectTemplate.addVehicleType` | Its min arity is 4; the scripts' 1-argument calls error the same way in retail. |
| 23 ×3 | `rainManager.randomness` / `maxRandomChange` / `randomDeviationAngleSpeed` | rain detail |
| 21 | `AITemplatePlugIn.cullDistance` | AI cull |
| 10 / 10 | `Material.triggerRadio` / `MaterialManager.setRadioMessage` | material radio |
| 8 | `ObjectTemplate.ability.setOverheatMod` | an unlock's overheat modifier |
| 8 | `Object.notInAI` | |
| ≤5 | `weaponTemplate.shouldRearm` / `fireRate` / `isAntiAirWeapon`; `AITemplatePlugIn.setSoundSphereRadius` / `setHearingProbability` / `setUseNoPathfindingToGetToObject` / `vehicleNumber`; `ObjectTemplate.setCheckObstruction` / `setIfTitanChangeToWeapon`; `renderer.useSunScaling`; `GeometryTemplate.boundingRadiusModifier`; `GameLogic.enableVoHelp` / `supplyDropHeight` / `supplyDropNumSecsToLive` / `createObject`; `Material.pythonEvent`; `Sound.windSetup` | |

### Unread template properties at HEAD (literal rule; the top ones by script calls)

- Editor flags: `isNotSaveable` 16,862, `saveInSeparateFile` 4,709, `createdInEditor` 3,543, `createNotInGrid` 259.
- Settled or no effect: `anchor`/`pan` (composed reads), `castsDynamicShadow` 258, `cullRadiusScale` 239.
- Then the real ones:
  - `usedOnTransparentSurface` 224;
  - `writeZ` / `directionalScale` 215;
  - `vehicleHud.useSelectionIcons` 214;
  - `damagedAmbientSoundLimit` 163;
  - `allowEmoteAnimation` 158;
  - `listenerObstruction` 131;
  - `fire.fireInCameraDof` 125;
  - `autoUseAbility` 123;
  - `weaponHud.disableOnSprint` 121;
  - `ignoreParentRotation` 120;
  - `penetrate.allowSolidPenetration` 106;
  - `vehicleHud.typeIcon` 102;
  - the LightSource projector and shadow flags 93–94 (settled: not drawn by retail);
  - `takeOverInput` 93;
  - `ammo.ammoType` 90;
  - the emitter `*Range`/`*Used` modifiers 76–78;
  - `detonation.detectionRadius` 76;
  - `maxDepthForExplosion` 73;
  - `useMMOnEndEffect` 67;
  - `hitImpactModifier` 65;
  - `ability.sprintRechargeModifier` 64;
  - `penetrate.allowLiquidPenetration` 63;
  - `alternateTemplate1..4` 60;
  - `vehicleHud.sendDestroyedHudEvent` 57;
  - `soundIntensity*` 55;
  - `animatedUV*` / `rotateUV` 48;
  - `localPredictOnClient`, `neverDrawAs1p`, `disableCloseSpawnPoints` 46;
  - `activeDefenseEnabled` 41;
  - `armor.wreckExplosion{Material,Radius,Damage}` 40/40/34;
  - `firearmReplacement` 40;
  - `dogTag{Reward,Exchange}Template` / `isDogtagReplacement` 32;
  - `vehicleHud.panting/injurySound` 32;
  - `sprintStaminaDissipationFactor` 32;
  - `ability.sprintUseModifier` / `beaconSpawnTimeMod` 32.
- By prefix: (no prefix) 308 names / 35,004 calls, `vehicleHud.*` 29 / 765, `armor.*` 18 / 418, `fire.*` 33 / 403, `detonation.*` 13 / 267, `ammo.*` 14 / 248, `ability.*` 15 / 218, `weaponHud.*` 13 / 216, `penetrate.*` 2 / 169, `target.*` 10 / 149, `radio.*` 5 / 133, `nametag.*` 7 / 130.
- False positives remain (composed keys):
  - `sway.*` 22;
  - `python.onRepairEvent`, which `Titans.cpp:161` handles as events 49/51/53;
  - `anchor`, `pan`.

### Level runs (calls / methods)

| Run | 991d041 | HEAD |
|---|---|---|
| Belgrade gpm_nv 32 | 6,757 / 53 | 6,507 / 42 |
| Camp_Gibraltar gpm_cq 32 | 6,608 / 58 | 6,358 / 47 |
| Cerbere_Landing gpm_cq 32 | 6,435 / 54 | 6,185 / 43 |
| Highway_Tampa gpm_sl 64 | 6,872 / 54 | 6,622 / 43 |
| Minsk gpm_ti 48 | 6,376 / 62 | 6,126 / 51 |
| Molokai gpm_ti 48 | 6,001 / 55 | 5,751 / 44 |
| Operation_Shingle gpm_sl 32 | 5,944 / 42 | 5,694 / 31 |
| sidi_power_plant_coop gpm_coop 32 | 6,519 / 48 | 6,269 / 37 |
| Suez_Canal gpm_cq 64 | 6,644 / 58 | 6,394 / 47 |
| Verdun gpm_sp 16 | 6,770 / 63 | 6,520 / 52 |
| Wake_Island_2142 gpm_ca 64 | 5,986 / 60 | 5,736 / 49 |

- Merged over the 11 runs, these left the top list:
  - `isTitanInterior` 396;
  - `isTitanExterior` 275;
  - `titanExteriorPartIndex` 28;
  - `setMaterialReflectionScale` 429;
  - `setSpecularStaticGloss` 198;
  - `setMaterialParallaxHeightScale` 77;
  - `armor.addArmorEffectSpectacular` 1,265.
- What remains is load hints (compressVertexData 15,756, maxTextureRepeat 15,734, lightmapOverride 13,292, doNotGenerateLightmaps 13,226, isAd 2,541), setLightSourceMask 5,240, the RagDoll and Vars rows, and then a tail of `Material.triggerRadio` / `MaterialManager.setRadioMessage` 110 each, `heightmap.setModified` 99, `ObjectTemplate.setCheckObstruction` 21, `ability.setOverheatMod` 12, `setIfTitanChangeToWeapon` 12 and `Object.disableChildren` 7.

### Address citations (start addresses cited in src/ and docs/, tests excluded)

| Module | functions / bytes | 991d041 (coverage-2) | 6001a99 (uncited.md) | **HEAD** |
|---|---|---|---|---|
| BF2142.exe | 35,589 / 4,794,004 | 1,983 fns, 852,825 B (17.8%) | 2,124 fns, 955,445 B (19.9%) | **2,678 fns, 1,154,647 B (24.1%)** |
| RendDX9_ori.dll | 10,810 / 1,677,614 | 244, 183,557 B (10.9%) | 310, 224,810 B (13.4%) | **339, 233,155 B (13.9%)** |
| AIDLL.dll | 6,080 / 1,006,188 | 381, 259,889 B (25.8%) | 382, 260,114 B (25.9%) | **388, 261,236 B (26.0%)** (with tests/: 391, 264,686 B) |
| SwiffPlayer.dll | 5,756 / 936,991 | 153, 122,700 B (13.1%) | 153 | **168, 124,680 B (13.3%)** |
| BF2VoipServer.dll | 377 / 38,482 | 34 (28.5%) | 34 | 34 (28.5%) |
| BF2142Audio.dll | 1,702 / 206,940 | 2 | 2 | 2 (rewritten, but cites no DLL addresses) |
| Memory.dll / TextureAtlasBuilder.dll | | 0 | 0 | 0 |

181 ambiguous DLL occurrences were dropped (161 at 991d041).

Uncited runs:
- **EXE (≥30 KB):** 19 runs / 1.24 MB → **12 runs / 0.81 MB**. The largest are:
  - FESL/EASTL 0x00840360 (158 KB), 0x008176d0 (133 KB), 0x008020d0 (70 KB), 0x00870ca0 (37 KB);
  - top-level template property setters 0x004e88de–0x005016fc (100 KB);
  - GameSpy GP 0x00724d20 (56 KB);
  - the command line, profile and settings managers 0x0046c3e0 (52 KB);
  - HudBuilder 0x00761c76–0x0076cd5a (44 KB);
  - file paths 0x006971a0 (38 KB);
  - static init 0x00900c43 (37 KB);
  - particle property tables 0x00530d64 (33 KB);
  - Speex 0x0073e100 (30 KB).
- **RendDX9 (≥20 KB):** 30 runs / 0.95 MB → **21 runs / 0.68 MB**. The largest are debug drawers 0x10189c60 (82 KB), the terrain editor 0x10154550 (49 KB), shader management 0x100189a0 (48 KB), 0x1008cf60 (47 KB) and IGA 0x10057470 (45 KB).
- **AIDLL (≥20 KB):** 6 runs / 0.25 MB → 5 runs / 0.22 MB. The AI console commands 0x10009120–0x1001ffff (87 KB) lead.

### Calibrated effective coverage (uncited.md's classifier re-run at HEAD)

| Module | Needed code (total − a − b) | Cited (raw) | Lower (cited + string/console-evidenced c) | **Calibrated** 6001a99 → **HEAD** | Upper | (d) missing, calibrated |
|---|---|---|---|---|---|---|
| BF2142.exe | 3.74 MB | 19.9% → **24.1%** | 45.9% → **49.8%** | 77.6% → **80.9%** | 84.0% → 86.9% | 0.84 → **0.72 MB** |
| RendDX9_ori.dll | 1.26 MB | 13.4% → 13.9% | 29.7% → 30.3% | 69.7% → **70.1%** | 87.6% → 87.8% | 0.38 → 0.38 MB |
| AIDLL.dll | 0.88 MB | 25.9% → 26.0% | 33.9% → 34.0% | 59.8% → **59.9%** | 86.3% → 86.4% | 0.36 → 0.35 MB |
| All three | 5.88 MB | 19.3% → **22.1%** | 40.6% → **43.3%** | 73.3% → **75.6%** | | 1.57 → **1.44 MB** |

Raw (d) bytes, uncalibrated: EXE 598 → 490 KB, RendDX9 153 KB (unchanged), AIDLL 120 KB (unchanged).

Biggest movements in the EXE's (d) buckets (`c3/unc/db_*.txt`):
- commander screen and assets 42 → 17 KB;
- online and BFHQ strings 29 → 15 KB;
- unbucketed 123 → 96 KB;
- top-level unread template properties 111 → 99 KB;
- the meme HUD framework 70 → 61 KB (the rewrite replaces it; not needed one-to-one);
- the vehicleHud setters 9 → 7 KB;
- the kick/map vote texts (5 KB) are gone from (d).

**Reading.** Almost all of the 7 commits' work landed in the EXE: game logic, HUD and network. RendDX9 and AIDLL barely moved, so their remaining gaps are where they were at uncited.md.

## 2. Remaining decodable gaps, ranked by player visibility

Effort: **S** = days, **M** = 1–3 weeks, **L** = a month or more. Call counts are from the 6,758-file corpus.

### 2a. Local play

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| L1 | **Explosion camera shake (and the tinnitus trigger)** | Nearby explosions don't shake the view. The tinnitus fires on the rewrite's own radius rule. | `0x005a6e70` is cited only for its first line (the ambient trigger, event 8). The rest of it walks the local player's camera (`DAT_00a85228` slot 0x30 → slot 0x1b4 → +0x4c): amount = clamp(force / `DAT_00a6ab74`) × clamp(x / `DAT_00a6abc8`) × `DAT_00a6ab4c` × (1 − d / (3r)), full when the hit is on or inside one's own object. It also posts event 0xd (`DAT_00a7d06c` +0x10) when a soldier (class 0x9493) is within r × `DAT_00a6ab7c`. That is very likely the "tinnitus event's sender wasn't found" (engine.md:2851). Callers: 0x004c…, 0x005d… (gun recoilForce > 5, Weapons.h:349 "the rewrite leaves out") and 0x0061…. `GSCameraShake` / `GeneralSettings.setCameraShake` is stored (SettingsObjects.cpp:55) but read by nothing. EffectBundle `cameraShake` / `Radius` / `Force` are unread. | S |
| L2 | **Ladders retail removes** (`Object.disableChildren`) | Players can climb ladders on 144 placed gas tanks, houses and watchtowers that retail strips. | A stub with 144 calls in the levels' StaticObjects.con. The children are `ladder_5m_c` / `ladder_6m_c` / `ladder_10m_c` (the objects' .tweak). | S |
| L3 | **Vehicle and soldier sound remainder** | No sound occlusion. No damaged-vehicle ambient loops. No soldier panting or injury breathing. No wind/speed sound. No material radio. | `listenerObstruction` 131, `damagedAmbientSoundLimit` 163, `vehicleHud.pantingSound` / `injurySound` 32 each are unread. engine.md:4672 "Not done: occlusion…, the inside-Titan listener flag, PlayerControlObjects' Ambient / AmbientDamaged loops, the wind/speed sound of 0x007f9ea0". `Material.triggerRadio` / `MaterialManager.setRadioMessage` are stubs (10 each). | M |
| L4 | **Unlock and ability modifiers** | The sprint unlocks don't change stamina use or recharge. The overheat and beacon unlocks do nothing. | Unread: `ability.sprintRechargeModifier` 64 (16 Ability templates, 1.5–1.8), `sprintUseModifier` 32, `sprintStaminaDissipationFactor` 32, `beaconSpawnTimeMod` 32, `abilityInVehicle{Material,Radius,Strength}` 24. `ability.setOverheatMod` is a stub (8). Setters at 0x0054d4d7–0x0054eef1 (the (d) "ability" bucket). | S |
| L5 | **Wreck explosions** | A wreck going off doesn't damage or push what is near it with the wreck's own values. | `armor.wreckExplosionDamage` 34, `wreckExplosionRadius` 40, `wreckExplosionMaterial` 40 are unread (29 Armor templates; force and max are read, GameWorld.cpp:1937). Setters at 0x00548e79–0x0054cc50. | S |
| L6 | **Projectile and detonation details** | Proximity and water behaviour of rounds and mines, penetration flags, end effects, active defence. | Unread: `detonation.detectionRadius` 76 (51 comps), `maxDepthForExplosion` 73, `useMMOnEndEffect` 67, `detonateOnWaterCollision` 34, `triggerType` / `useCollisionNormal` / `radiusDetonateWithAirVehicles` (0x004f34da–0x004f4ad1), `penetrate.allowSolid/LiquidPenetration` 106/63, `hitImpactModifier` 65, `activeDefenseEnabled` 41. engine.md:6046 "Not traced: the detonation component's 0.05 m MTYVehicle trigger test, ForceObjectHoverTest". | S–M |
| L7 | **Firing direction and fire-component flags** | Guns with `fireInCameraDof` aim off the camera in retail. Busy-until-release, prone-only fire, pull-back time and barrels differ. | `fire.fireInCameraDof` 125 (86 fire comps), `fire.busyUntilButtonRelease` 25, `pullbackTime` 24, `projectileStartRotation` 24, `addBarrelName` 32, `useDistanceCounter` / `distanceStep` / `minimumDistance` 20, `altFireInput` 19 are unread (0x004e39f3–0x004e586c). | S |
| L8 | **Weapon and vehicle HUD remainder** | The crosshair doesn't hide while sprinting. Vehicle type and selection icons fall back. No destroyed-vehicle HUD event. Visual deviation limits are missing. | `weaponHud.disableOnSprint` 121 (85 comps), `vehicleHud.useSelectionIcons` 214, `typeIcon` 102, `sendDestroyedHudEvent` 57, `canBeSpotted` 41, `weaponHud.min/maxVisualDeviation` 16/24, `altCrosshairIcon` 14 are unread. engine.md "Left: typeIcon, useSelectionIcons". | S |
| L9 | **Titan-mode remainder** | In gpm_ti, standing inside the enemy Titan's shield doesn't kill. No combat-area HUD text or minimap area texture. Pods have no regulator. No Titan-interior indoor cube. | engine.md:7117 (`0x006f2b00`, every 64 ticks), 7814 (pod regulator and keepUp, lookAtClosestTitanOnEnter), 2728 (`+0x6c`, `0x100c3880`). | S each |
| L10 | **Animated UVs on tank and IFV tracks and wheels** | Tracks and road wheels don't scroll or turn their textures when driving. | `animatedUVTranslation*` 16, `animatedUVRotation*` 8, `rotateUV` 48 are unread. The setters are at 0x0051f24d–0x005205e5, in the (d) list. They are set on `EU_IFV_tracks_Left/Right`, `EU_TANK_Rwheel_06` and similar. `GeometryTemplate.nrOfAnimatedUVMatrix` is kept, but coverage-2 #14 says "the exe's per-part UV transforms not traced". | S–M |
| L11 | **Dog tags** | Knife kills don't produce the dog-tag reward or exchange items. | `dogTagRewardTemplate` / `dogTagExchangeTemplate` / `isDogtagReplacement` are unread (32 each; e.g. EU_Assault → knife_unlock). | S |
| L12 | **Spawn, exit and seat flags** | Close spawn points stay active. Pod-launcher exits, team from the closest CP, seat rotation limits and toggling positions differ. | Unread: `disableCloseSpawnPoints` 46, `exitThroughPodLauncher` 32, `teamFromClosestCP` 17, `seatLeft/RightRotationLimit` 18, `allowTogglePosition` 18, `lockToTeam`, `destroyOnExit` (0x004fbdb3). GameWorld.h:18: the spawn spot's drop `0x005ad8b0` and activation by events are not rewritten. | S |
| L13 | **Small game-logic tunables and leftovers** | The commander radial level, supply drop height and life, UAV and scan timings; the slowed movement state; banked excess input. | `GameLogic.radialLevelIndex` (30), `supplyDropHeight` / `NumSecsToLive` / `enableVoHelp` stubs. SoldierControl.cpp:87 ("slowed state not rewritten"), GameWorld.cpp:7260 (rememberExcessInput). | S |
| L14 | **Bot realism** | Bots don't zoom, know everything, ignore hearing and cull distance, and never use the guided weapons' own plan. | `AITemplatePlugIn.cullDistance` (21), `setSoundSphereRadius` / `setHearingProbability` (3) stubs; engine.md:8724–8815 (knowledge certainty 0x100a9b90, ITAir clause, 0x10078570, the C4 guard, revive water rule…). | M |

### 2b. Joined and hosted play

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| J1 | **Other players' rounds on a client** | A joined client sees others' muzzle flashes and hears their shots, but not their projectiles, tracers, impact effects or decals. | engine.md:1517 "the server's shots aren't decoded"; 1025–1029 "Tracers in joined clients … Not done". HEAD's commit runs others' firearms for effects and sound only (no projectile spawn in its diff). | M |
| J2 | **Commander camera and scoping on the network** | The commander's asset camera event and the scan/UAV ghost scoping are missing. | main.cpp:3583 (server event 0xf "the commander's camera, which isn't rewritten"); HostedGame.cpp:2331 / engine.md:4131 (scan and UAV at 0.5, `0x0057cfb0`; OptimizeEnclosingObjects; the alternate eye vt+0xd4). | S–M |
| J3 | **Whisper and takeable radio on clients** | Whispers stay local. A client misses the control point's "takeable" radio. | main.cpp:3795 / engine.md:6246 (the whisper's network message isn't decoded); engine.md:4753 (the takeable radio). | S |
| J4 | **Spectator** | No spectator rules or HUD. | engine.md:5467 "Not done: … the spectator's rules"; nametags "Left: the spectator". | M |
| J5 | **Ghost field meanings** | Minor state on clients (fields read but unused). | Networkables.h:221, 235, 258, 355, 369; engine.md:3462 (+0x168 bits), 3955 (mask 0x10000 via +0x280), 4424; RemoteWorld.h:149 (kick reason byte); CreateSpawnGroup's bits (engine.md:3266, HostedGame.cpp:1939). | S each |
| J6 | **Server odds and ends** | rcon_feedback over one message; the ranked minimums; the end-of-round delay's per-mode rules; mutiny re-application refusal. | engine.md:4215, 4291; Commander.cpp:138. | S |

### 2c. Menus and front end

| # | Gap | Evidence | Effort |
|---|---|---|---|
| M1 | **Language list and non-English locales** (only English is selectable) | uncited.md (d) 10: 0x0049fda0 (Chinese_Traditional, zh_CN, code pages), RendDX9 0x1017af30 | S–M |
| M2 | **Demo camera and playback controls** | `demo.togglePlayerDemo` / `toggleCameraDemo` / `adjustDemoFov` / `restartDemo` / `shutdownDemo` / `toggleDemoPauseOrPlay` stubs; engine.md:4358, 4374 (camera files, message 9, timedemo) | S–M |
| M3 | **Squad menu remainder** | squad name box typing (engine.md:7204–7206); `SquadManager.changeSquadName` / `joinSquad` / `leaveSquad`, `Squad.leaveSquad` stubs; the squad panel's `voip_icon_*`, `SELECTED_ALL_SQUADS`, the order points bar (engine.md:7523) | S |
| M4 | **Commander screen remainder** | `Commander.zoomCommander` / `detailedView` / `spottedMouseOver`, `CommanderMenu.sendRadioMessage`, `SquadLeader.issueSquadRequest` / `sendRequestForOrder` / `addRemoveCP` stubs; the commander tabs' first state (HudMenus.h:157); which order kind draws which minimap icon (engine.md:7378) | S |
| M5 | **Awards pop-up** (0x007d1270, 44 KB) | Shows only for the server's Medal events; offline nothing raises them, so it is mostly online-gated | S (once events exist) |
| M6 | **AI console commands for scripted bots** (AIDLL 0x10009120–0x1001ffff, 87 KB) | Barely used by shipped scripts; matters for mods | M |

### 2d. Rendering

| # | Gap | Evidence | Effort |
|---|---|---|---|
| R1 | **Animated UVs** | see L10 | S–M |
| R2 | **Particle draw detail** | `usedOnTransparentSurface` 224, MeshParticleSystem `writeZ` / `directionalScale` 215, the bundles' `*Range` / `*Used` / `*FromSpeed` modifiers (76–78, 43), `soundIntensity*` 55; the zoomed-mode systems' `+8` reader (engine.md:2460); the Particle object's blend for tracers (897); drawOrder and the system ground planes (Effects.h:227, 232). Setters 0x0053c649–0x0053f395 (11.6 KB, top (d) cluster). | S–M |
| R3 | **Undergrowth sway and distance** | LevelRenderer.cpp:1566 "The engine animates 16 sway offsets; until that is decompiled, each swings on its own phase"; roadmap row 5 "sway animation and view-distance scaling are approximations"; `undergrowth.viewDistanceFade` / `viewDistanceScale` stubs | S |
| R4 | **Overgrowth and static LOD** | engine.md:1749 (dropping trees beyond the view distance, the overgrowth shadow map), 1734 (template +0xc8 at 0x100dd480, `ObjectTemplate.lodDistance` High/Medium/Low); `StaticMeshRenderer.*` 10/10 stubs | S–M |
| R5 | **Decals** | engine.md:1878 (normal-mapped technique, `decalGeomClipAngle`); `DecalManager.*` 7/7 stubs (timeToLive, fade) | S–M |
| R6 | **Lights** | engine.md:1816 (specular, flicker, the per-mesh light choice `lightManager.maxLightsPer*Mesh`, `lightCullDistance`); projectors and shadows are settled as not drawn by retail | M |
| R7 | **Terrain and rain detail** | engine.md:1678 (per-material detail lists, cullerTypes other than 3, detailCullMaxLevel); `rainManager.randomness` / `maxRandomChange` / `randomDeviationAngleSpeed` stubs (23 each); `terrain.waterQuality` / `waterNormalMap` | S |
| R8 | **View tint and overhead value** | engine.md:2856 (PostProduction slot 0x7c), 6325 (`DAT_00a2020c` slot 0x80) | S |

### 2e. Not decodable or out of scope (unchanged from coverage-2 §2b)

- the online back end (FESL/EASTL ≈ 0.4 MB, GameSpy GP, BFHQ, leaderboards, award grants, entitlements);
- PunkBuster, IGA ads, SecuROM;
- TextureAtlasBuilder and the lightmapper (`LightmapGeneration.*` 50);
- editor and debug objects (collisionManager, AnimationSystem editor, `Console.get*`, Overgrowth editor, Player.*, heightmapcluster);
- Novint Falcon and the G15 LCD;
- the AI's real-time CPU budgets.

## 3. Remaining hedges ("inferred", "approximate", "guess", "stand-in", "not traced"/"not found"/"not decoded", "fitted")

Searched in docs/engine.md (9,515 lines; 50 hits after removing false matches) and src/ (58 hits, plus "not rewritten" and "until … decompiled" notes). stb_image, miniz and Avm1 are excluded.

### 3a. A further decompilation pass could settle these

**Physics, weapons and effects**
- engine.md:221: what turns a wreck's mesh over (instance slot 0x30) is unknown.
- engine.md:785, MeshCollision.cpp:63: the response's stuck push (+0x11c) and the damage events' impact effects and decals.
- MeshCollision.cpp:698: the wheel-into-small-static LOD path.
- engine.md:897: the Particle object's blend for tracers.
- engine.md:1375, PlayerCamera.h:125: the caller of the hit shake 0x00610450.
- engine.md:2851: the tinnitus event's sender. **Lead:** the tail of 0x005a6e70 posts event 0xd for a soldier within r × `DAT_00a6ab7c` (see L1).
- engine.md:2856: the view's tint colour (PostProduction slot 0x7c).
- engine.md:5780, GameWorld.cpp:3373: the lock-on component's object update; the lock rule is the rewrite's.
- engine.md:6046: the detonation component's 0.05 m MTYVehicle test, ForceObjectHoverTest and drag.
- engine.md:7747: the call site of 0x005eaca0 (material penetration exit).
- Weapons.h:501: the deviation's +0x44 extra (holder slot 0x1a0).
- GameWorld.cpp:728: the consumer of addExtraAmmoItem's flag.

**Rendering**
- engine.md:1735: template +0xc8 in 0x100dd480 (static LOD skip).
- engine.md:1876: per-object decal sets; the rewrite's rule is fitted to what a projectile can hit.
- engine.md:2460: the zoomed-mode systems' `+8` reader.
- engine.md:6325: the overhead view's renderer value (`DAT_00a2020c` slot 0x80).
- Effects.h:227: drawOrder's use. Effects.h:232: the systems' ground planes.
- LevelRenderer.cpp:1566, roadmap.md:34: the undergrowth sway offsets and view-distance scaling (approximation).

**Spawning and game logic**
- engine.md:7070: the caller 0x005a3bd0 of the man-down branch.
- engine.md:7163, GameWorld.cpp:3211: the spawn's extra test (IAIPathfindingCourier slot 0xc, `DAT_00b4bb94`).
- GameWorld.cpp:3155: what 0x005b2200's caller does when no try fits (the rewrite puts the member a metre behind).
- GameWorld.cpp:4047: which flag seats a bot on setEnterOnSpawn points.
- GameWorld.cpp:4496: how long a seat's slumped body stays.
- GameWorld.h:18: the spawn spot's drop 0x005ad8b0 and activation by events.
- SoldierControl.cpp:87: the slowed state (speed index 2).
- GameWorld.cpp:7260: rememberExcessInput.
- engine.md:6210, Radio.cpp:95: the medic-means flag's setter (kit +0x228 / +0x74 slot 0x38).
- engine.md:6633: when the exe starts and stops the ability sounds.
- engine.md:9436: the setter of the kit's special-ability icon list.
- engine.md:4283: the friendly-fire victim factor's kit-list loader.
- Commander.cpp:138: the client refusing an applicant barred after a mutiny.

**HUD**
- engine.md:5049: the bar fill directions are "inferred".
- engine.md:5069, Hud.h:32, Hud.cpp:49: the corner roots' positions are "approximate" (the engine places them by screen size).
- engine.md:5140: the rows the scoreboard blacks out via +0x410 / +0x404.
- main.cpp:474: the scoreboard list's own drawing (RendDX9) isn't decompiled; the layout is read off the scripts.
- engine.md:5269: the range finder's firearm components (0x94d3 / 0xc4f1 / 0x94db, +0x17c).
- engine.md:5352: the name-tag "Guesses": the soldier's origin 1 m over the feet, the 4 m vehicle height, the bar's place.
- engine.md:6542: the sender of HUD event 0x2f (the first/second warning split).
- engine.md:7206, 7423, HudMenus.cpp:42, 409: the squad and commander list columns are fitted to the scripts' widths (the menu's own widths at +0xc0 are set in code).
- engine.md:7378: which order kind the minimap draws with which icon.
- HudMenus.h:157: the commander tabs' first state (constructor not found).
- HudMenusVehicles.h:271: the silo frames' pick.
- AnimationSystem.h:318: what seats pass to LookAroundTrigger.
- engine.md:9500: the drawing code behind +0x3c4 (a no-op native).

**Network**
- engine.md:3266, HostedGame.cpp:1939: CreateSpawnGroup's three bits and two bytes.
- engine.md:3462, Networkables.h:235: the +0x168 ghost bits' setter.
- engine.md:3955: mask 0x10000's setter (+0x280).
- engine.md:4424, Networkables.h:221: the soldier's mask 0x40000 field.
- Networkables.h:258, 355, 369: the fields at 0x00562a80 / 0x00562ee0, flag210, and +0x208 / +0x238 / +0x1fc.
- RemoteWorld.h:149: the kick reason byte.
- engine.md:4215: how a long rcon_feedback is carried.
- main.cpp:3795, engine.md:6246: the whisper's network message.
- engine.md:3829, main.cpp:4309: the radio heard is the rewrite's stand-in for the bots' calls (bots don't go through slot 0xd8).
- HostedGame.cpp:156: the move buffer's depth is "OpenBattlefield2's reading of BF2's, not traced in BF2142.exe".
- engine.md:4126, GhostPriority.h:77, HostedGame.cpp:2199: the ghost dirty masks' stand-in. Settleable by decoding the 12 masks, but low value.

**AI** (AIDLL addresses given)
- engine.md:8051, Bots.cpp:1451: the grenade warning's sender.
- engine.md:8585, Bots.cpp:3258: the +0x30 pass-over writer, the unit-count scaling 0x100d2b30, the Unit's capability flags.
- engine.md:8758–8773, Bots.cpp:2439–2452, 4495: the ITAir clause (slot 0x28), Mobile slot 0x78 (0x008a1960), the attention stamp 0x100eb7f0, the C4 guard, the guided weapons' plan 0x10078570, +0x209.
- Bots.cpp:839 (seat changes, pick-up), 2674 (revive water rule), 3004 (ITAir room test 0x1006c3e0), 3719 (the terrain-normal upright test).
- PathFollower.cpp:1229 (the avoid rule).
- engine.md:8800–8815: knowledge certainty 0x100a9b90, ammo in the capability factor, seated soldiers, camouflage charge, the idle alternate-key tap, the commander's strike tool choice and UAV/scan, the path follower's 2 m re-plan.

### 3b. Not settleable by decompilation (by nature, deliberate, or not a hedge)

- **Real time:** engine.md:8726 and PathFollower.h:37, the CPU-budgeted search slicing. Real-time budgets can't be reproduced deterministically.
- **No human on the other end offline:** engine.md:7204, HudMenus.h:36, HostedGame.cpp:1114. Squad invites offline are answered by bots at once.
- **Test-only defaults without the AI scripts:** engine.md:7859, Bots.h:71. Every shipped vehicle has its `.ai` file.
- **Architecture, not hedges:** LevelRenderer.h:4 (GLSL replaces RendDX9); Console.h:76 (the stub mechanism); UI/Avm1/Builtins.h:19, UI/Player/ClipMethods.cpp:3, Voip/VoipNatives.h:3, main.cpp:2732 (replaceable natives); Sound.cpp:17 and Ambience.h:18 (null objects).
- **Algorithm terms:** NavMesh.cpp:694, NavMesh.h:216 and 237 (the original's heuristic); Ballistics.cpp:132 and 186, engine.md:8612 and 8627 ("first guess" of flight time).
- **Diagnostic output:** main.cpp:1800, the `--connect` printer for undecoded event types.
- **Needs services:** GameClient.h:55 (CD key: GameSpy).

## 4. Inconsistencies

**Docs that say "not done" for something the code now does (stale):**
1. **engine.md:5298–5301** says "Not done: … the map's spotted vehicles and the silos' reload and background icons (0x0077dee0 / 0x0077df60 …)". The same file (5246), main.cpp:764–765, HudMenusVehicles.h:256 and coverage-2 gap 4 all say these are done (miniMapInfo, `HudQuad::sweep`, spottedIcon).
2. **engine.md:5463–5467** says "the rewrite's squad leaders send no requests (SquadLeader.sendRequest 0x007708d4 and the squad manager's request store aren't rewritten). Not done: the requests (above)". The probe shows `SquadLeader.sendRequest` implemented at HEAD. engine.md:7433, HudMenus.h:209 and main.cpp:3386 describe it, and coverage-2 #9 has it done.
3. **src/Game/GameWorld.cpp:6249–6251** says the cloak is "drawn hidden as a stand-in for the shimmer". `LevelRenderer::drawCamouflage` (LevelRendererPost.cpp:627, called at LevelRenderer.cpp:3451) draws the refraction. Coverage-2 fixed the engine.md line but not this comment.
4. **roadmap.md** states:
   - Rows 8 (playable local game), 10 (multiplayer), 11 (AI) and 12 (VoIP) still open with "Started:". Given the audits (≈81% calibrated EXE, joined and hosted games at parity), "Started" understates them.
   - Row 5's "sway animation and view-distance scaling are approximations" is still true (LevelRenderer.cpp:1566). It is consistent, but it is an open item.
5. **docs/coverage-2.md** is a historical snapshot. Its Table B' still lists GAP rows done since (isTitanInterior/Exterior, addArmorEffectSpectacular, the nametags rows, waterFog, waterSunIntensity…), so it should be read with this audit's tables.

**Docs that say "done" where the code stubs it:**
- None found among console rows. No impl → stub regressions, and no read → unread template property except `coneAngle1`, whose reader moved to coneAngle2 (radians; mesh.frag:107). Intended.
- Partial: the uncited (d) #1 "Status: done" for emitters still lists `usedOnTransparentSurface` / `writeZ` / `directionalScale` as "Left", and those are among the top unread rows. That is consistent, just open.

**Tests:**
- 596 tests, 2 failing:
  - `Voip_LoopbackSession` (VoipTest.cpp:550, 551, 560) now fails **6 of 6** times serially (`-j 1 Voip_`) and in the full run. Coverage-2 called it a timing flake (2/8 passes).
  - `Voip_RefusedGameServers` (VoipTest.cpp:597) fails 2 of 6 times in parallel runs; it is a new flake.
  - Neither src/Voip nor tests/VoipTest.cpp changed since 8948ed8, so this is an environment or timing problem worth pinning down rather than a regression.
- Features without a test referencing them:
  - the friendly-fire scaling (0x005b7290, GameWorld.cpp:747; still untested since coverage-2);
  - `maintainCameraOnExit` (PlayerCamera isn't linked into the tests);
  - the silo "_N" radio suffix (`GameAudio::siloSuffix`);
  - `hasOnTimeEffect` and `ammo.reloadWithoutPlayer`;
  - `scatterOnLeader` (squad spawn);
  - the world order markers;
  - `CommanderTabs`;
  - the undergrowth sway;
  - all of src/Render's draw passes: the tests don't link Render/Scene (tests/CMakeLists.txt:2), so `drawCamouflage`, the Titan interior pass and the water fog draw have only the GPU-free maths tests;
  - Main/Viewer (the game loop).
- Fixed since coverage-2: no `__pycache__` file is tracked any more.

## Raw outputs

All kept outside the repo:
- `probe_{sim,host,shot,union}.json`;
- `rows_h3.json`, `tableA.md`, `stubs_called.md`, `stubs_uncalled.md`, `unread_called.md`, `unread.json`;
- `runs/*.txt`;
- `cite_h3.json`, `cite_h3_t.json`;
- `unc/report.txt`, `unc/lab_*.json`, `unc/db_*.txt`;
- `eng_ctx.txt`, `src_hits.txt` (the hedge greps);
- `tests.log`.
