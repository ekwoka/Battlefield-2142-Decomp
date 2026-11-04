# BF2142 rewrite: sixth coverage audit (2026-11-04, HEAD 2041898)

This audit compares HEAD 2041898 with fec6f08, the commit that added
docs/coverage-5.md. Four commits lie between them (c9486a4, e2a0e2a,
bd7daec, 2041898): the game-rules and HUD passes, voice natives, joined
seats and ladders, the mouse wheel, vehicle boost, guided bots, the
rendering pass and the vehicle and weapon HUD's blinkers.

## How this was measured

Both sides used the same scripts, `tools/coverage/` as fixed in this audit
(see "Script fixes" below):
- **before:** fec6f08, a clean worktree build (`tools/coverage/build_old.sh fec6f08 …`);
- **after:** HEAD 2041898, the main checkout. Its only uncommitted change is `.mcp.json`, which no script reads; the release build was brought up to date first.

The worktree was removed afterwards. The game install is the checkout's
`source/Battlefield 2142`. No window was opened. The probe and the level
runs used `--simulate` (one probe mode used `--screenshot`) with
`--nosound --no-movies`. The host probe used `--host --dedicated` without
`--online`. Nothing went beyond loopback. Both sides share the same corpus
(`calls.json`: 6,702 files, 816,424 calls) and the same decompilation
features.

**The headline citation figures leave out the audit reports**
(`cite.py --no-audit-docs`, `uncited_noaudit`). Their gap lists cite
addresses that nothing implements. At HEAD they still account for 164 EXE
functions (66 KB, 1.4 points), 90 RendDX9 functions (55 KB, 3.3 points),
8 AIDLL functions and both BF2142Audio functions. The coverage-3 rule
(audit docs counted) is given beside them for the series.

**Script fixes made in this audit.** They apply to both columns.
- **`rows.py`, composed keys:** three more forms src builds are now recognised.
  - `"alternateTemplate" + std::to_string(i)` (Effects.cpp:1320): `alternateTemplate1..4`, 60 calls each.
  - `std::string("helpHud.closeToHelpStringKey") + side`, where side is "Friendly", "Enemy" or "Neutral" (HudAlerts.cpp:452): the six `helpHud.closeToHelp*` rows.
  - `("supplyValueTeam" + n)` (GameWorld.cpp:342): the four `supplyValue*Team1/2` rows.

  These 14 rows (270 calls) were read already but counted as unread. The change is the same on both sides, and no other row changed class. Still missed: the `kInputs` table (`base + kInputs[j]`, Effects.cpp:2216–2226). Three names stay unread because of it: `alphaIntensityFromRotationalSpeed`, `emitSpeedScaleFromRotationalSpeed` and `emitSpeedScaleFromAcceleration` (8 calls).
- **`uncited.py`, label files:** each run wrote `labels_<appendix>_<module>.json`. The headings run and the no-audit run therefore overwrote `labels_with_*`, so the labels on disk didn't belong to the default run. The files are now named by `--name` when one is given (`labels_uncited_noaudit_*`).
- **`uncited.py --min-run`:** sets the smallest (d) run that is listed (default 4 KB).
- **`run_all.sh`:**
  - lists the no-audit (d) runs down to 2 KB;
  - adds `uncited_noaudit_recal` (`--recalibrate` on the headline citations). coverage-5 ran that step by hand.
- `tools/coverage/README.md` has been updated to match.

**fec6f08 against coverage-5's printed figures.** coverage-5 measured 4912c02, and fec6f08 only adds that report. The probe, the console rows, the level runs and the no-audit citations therefore come out identical to coverage-5's HEAD column. There are two exceptions:
- **Stored-read and unread counts.** The rows fix gives 882 stored-read (coverage-5: 868) and 406 unread names (420).
- **The EXE's effective figure.** It rises by about 0.1 point (82.1 → 82.2% calibrated, no audit docs).

Everything below compares fec6f08 as re-measured by these scripts.

Outputs (outside the repo) are in the scratchpad:
- `c6before/` is fec6f08 and `c6head/` is HEAD. `c6check/` is a full `run_all.sh --before` rerun of HEAD with the fixed scripts; its figures match `c6head/`.
- Logs: `c6before.log`, `c6head.log`, `c6check.log`, `c6_tests.log`, `c6_tests_before.log`.

## 1. Headline numbers, before → after

| Metric | coverage-5 as published (4912c02) | fec6f08, these scripts | **HEAD 2041898** | Change |
|---|---|---|---|---|
| Probe, distinct names: impl / stub (union of `--simulate`, `--host`, `--screenshot`) | 1,128 / 1,560 | 1,128 / 1,560 | **1,128 / 1,560** | 0. The four commits bound no new console method. |
| … `--simulate` alone | 970 | 970 | 970 | 158 names answer only when hosting; `--screenshot` adds none |
| Console rows (2,837): impl / stub | 1,137 / 381 | 1,137 / 381 | **1,137 / 381** | 0 |
| … ObjectTemplate stored-read | 868 | 882 | **924** | +42 |
| … ObjectTemplate stored-unread | 451 | 437 | **395** | −42. No row went from read to unread. |
| Unread template property names | 420 | 406 | **368** | −38 |
| … lower bound (last segment absent from src) | 341 | 327 | **291** | −36 |
| BF2142.exe rows impl / stub; RendDX9 | 874 / 256; 263 / 125 | same | same | 0 |
| Start-up "console methods stubbed": EXE / RendDX9 | 507 / 125 | 507 / 125 | 507 / 125 | still includes the 158 host-only methods |
| Stub rows called by shipped scripts / calls | 36 / 23,262 | 36 / 23,262 | **36 / 23,262** | 6,846 calls without `Object.setLightSourceMask` |
| Unread rows called / calls | 282 / 34,733 | 272 / 34,463 | **230 / 32,785** | −42 / −1,678. Unique names: 251 / 33,624 → **213 / 32,287**. |
| Level runs: "N calls to M unimplemented methods" | 5,643–6,560 / 24–26 | same | **identical, run for run** | |
| Python tracebacks (11 runs) / "Unknown object or method!" per run | 0 / 6,689–7,599 | same | same | |
| Unit tests (`BF2142_GAME_DIR` set) | "738, all passing" (its last status block) | 698, 2 failing (`Voip_LoopbackSession`, `Voip_RefusedGameServers`) | **738, 0 failing** | Without `BF2142_GAME_DIR`, `Commander_OrdersAndBots` fails on both sides (section 4, item 1). |
| Hedge-note hits (`hedges.py`) | 214 | 214 | **216** | engine.md 113 → 113, src 101 → 103. By text, 19 left and 21 arrived (section 3). |

### Template properties that became read (38 names, 42 rows)

- **Game rules:**
  - `fire.maxProjectilesInWorld`, `fire.fireStartDelay`, `fire.maximumVehicleTiltForFire`;
  - `fire.useDistanceCounter` / `distanceStep` / `minimumDistance`, `target.disableFireWhenActiveMissile`;
  - `collision.bouncing`;
  - `armor.canBeRepaired` / `canBeRepairedWhenWreck` / `deepWaterDamageDelay` / `disableInputOnDestroy` / `dontUseWreck` / `explosionForce`;
  - `minimumSquadSizeForSelection`, `isPod`;
  - `startAutoRotateOnEnter`, `restoreRotationOnExit`, `surpressPlayerInputVector`;
  - `seatLeftRotationLimit` / `seatRightRotationLimit`.
- **HUD:**
  - `vehicleHud.useSelectionIcons` (200 calls), `useChildHud`, `showOneSecondaryGunnerAngle`, `zoomOnMinimap`;
  - `miniMapIconOffset` / `Inside`, `standardHelpEnabled`, `useVehicleCommRose`, `displaySelectionOnEnter`;
  - `weaponHud.displaySelectOnActivation`;
  - `nametag.secondary3dMapIcon` / `useBoundingRadius`.
- **Rendering and effects:** `cullRadiusScale` (233), `supportsEmitter`, `speccolor`, `maxDistanceUnderWaterSurface`, `windBlowTime`.

### Level runs

They are identical, run for run (Belgrade 6,446 / 26 … Wake_Island_2142 5,643 / 25; the table is in `c6check/runs_table.md`). What remains is coverage-5's list:
- the load hints;
- `setLightSourceMask`;
- the RagDoll rows;
- `heightmap.setModified` 99, `setCheckObstruction` 33, `setIfTitanChangeToWeapon` 22, `boundingRadiusModifier` 22;
- `GameLogic.defaultTimeToNextAIWave` 11, `Material.pythonEvent` 11, `texturemanager.customTextureSuffix` 11;
- `rainManager.cellDisplay` 6, `undergrowth.load` 6.

### Address citations (start addresses; src/ and docs/, tests excluded)

The headline is without the audit docs (`--no-audit-docs`):

| Module | functions / bytes | fec6f08, without appendix | **HEAD, without appendix** | fec6f08 → HEAD, with appendix | coverage-3 rule (audit docs counted), without / with appendix |
|---|---|---|---|---|---|
| BF2142.exe | 35,589 / 4,794,004 | 2,928 fns, 1,230,831 B (25.67%) | **3,034 fns, 1,258,213 B (26.25%)** | 3,828 (27.86%) → **3,934 (28.43%)** | 3,099 (27.09%) → **3,198, 1,323,932 B (27.62%)**; 29.27 → **29.80%** |
| RendDX9_ori.dll | 10,810 / 1,677,614 | 415, 276,314 B (16.47%) | **439, 286,720 B (17.09%)** | same | 506 (19.74%) → **529, 341,551 B (20.36%)** |
| AIDLL.dll | 6,080 / 1,006,188 | 1,061, 382,501 B (38.01%) | **1,087, 387,873 B (38.55%)** | same | 1,070 (38.22%) → **1,095, 389,074 B (38.67%)** |
| SwiffPlayer.dll | 5,756 / 936,991 | 168, 124,611 B (13.30%) | **179, 127,847 B (13.64%)** | same | 169 (13.31%) → **180 (13.66%)** |
| BF2VoipServer.dll | 377 / 38,482 | 34, 10,976 B (28.5%) | 34 (28.5%) | same | same |
| BF2142Audio.dll | 1,702 / 206,940 | 0 | 0 | 0 | 2 (only through the audit docs) |
| Memory.dll / TextureAtlasBuilder.dll | | 0 | 0 | 0 | 0 |

- **Gain.**
  - Without the audit docs: EXE +106 functions / +27.4 KB (+0.57 points), RendDX9 +24 / +10.4 KB (+0.62), AIDLL +26 / +5.4 KB (+0.53), SwiffPlayer +11 / +3.2 KB (the voice natives).
  - The coverage-3 rule gives about the same: +0.53, +0.62 and +0.45.
- **The audit docs' share:**
  - fec6f08: 171 EXE functions / 67.7 KB, 91 RendDX9 / 54.8 KB, 9 AIDLL.
  - HEAD: 164 / 65.7 KB, 90 / 54.8 KB, 8. Some of the addresses coverage-5.md cited are cited from engine.md and src now.
- **The appendix** is unchanged: 900 EXE functions / 104,612 B, +2.2 points raw. It changes no effective figure.
- **With tests/:** EXE 3,149 (27.30%) → 3,248 / 1,333,927 B (27.82%; 30.01% with the appendix), RendDX9 507 → 530, AIDLL 1,070 → 1,095.
- **With `--headings`:** RendDX9 515 / 20.03% → 539 / 346,569 B (20.66%), AIDLL 1,100 / 38.63% → 1,125 / 393,190 B (39.08%).
- **Ambiguous DLL addresses dropped:** 139 → 153 (coverage-3 rule), 119 → 128 (no audit docs), 79 → 89 (headings).
- **Uncited runs, no audit docs:**
  - EXE (≥ 30 KB): 11 / 0.76 MB, unchanged (13 without the appendix).
  - RendDX9 (≥ 20 KB): 20 / 0.70 MB → **19 / 0.63 MB**. `0x100189a0` (49 KB) shrank to `0x1001ddf0` (28.5 KB); `0x1019fab0` 49 → 31 KB; `0x100c6570`, `0x100df010` and `0x100cd220` are smaller.
  - AIDLL (≥ 20 KB): 2 / 0.06 MB, unchanged.
- **Uncited runs, coverage-3 rule:** EXE 8 / 0.55 MB; RendDX9 14 / 0.45 MB → 13 / 0.42 MB; AIDLL 2 / 0.05 MB.

### 1b. Effective coverage (the uncited classifier, `uncited.py`)

The definitions are coverage-4's:
- **Needed** is the total minus (a) third-party and (b) dev-only code.
- **Lower** is cited plus string- or console-evidenced (c).
- **Calibrated** adds propagated (c) × the factor (EXE 0.84, RendDX9 0.69, AIDLL 0.48) and splits "?" pro rata.
- **Upper** counts all (c) and all "?".

**Headline: without the audit docs** (`uncited_noaudit`, appendix counted):

| Module | Needed | Cited, of needed | Lower | **Calibrated** | Upper | (d) calibrated | Raw (d) |
|---|---|---|---|---|---|---|---|
| BF2142.exe | 3.68 MB | 36.3 → 37.0% | 50.7 → **51.7%** | 82.2 → **82.7%** | 88.3 → 88.6% | 0.654 → **0.637 MB** | 422 → **408 KB** |
| RendDX9_ori.dll | 1.29 MB | 21.4 → 22.2% | 36.1 → **36.6%** | 71.8 → **72.0%** | 87.6 → 87.7% | 0.364 → **0.361 MB** | 157 → **155 KB** |
| AIDLL.dll | 0.87 MB | 43.8 → 44.3% | 45.7 → **46.3%** | 69.2 → **69.5%** | 94.3 → 94.3% | 0.269 → **0.266 MB** | 48 → **48 KB** |
| All three | 5.84 → 5.85 MB | 34.1 → **34.8%** | 46.7 → **47.6%** | 78.0 → **78.4%** | | 1.288 → **1.265 MB** | 627 → **612 KB** |

- **The coverage-3 rule** (`uncited_with`), for the series:
  - EXE 52.1 / 83.6 / 89.6 → 53.1 / **84.1** / 90.0%, (d) 0.605 → 0.588 MB, raw 374 → 361 KB;
  - RendDX9 37.9 / 73.5 → 38.4 / **73.8%**, 0.344 → 0.340 MB;
  - AIDLL 46.0 / 69.8 → 46.5 / **70.0%**, 0.264 → 0.262 MB;
  - all three 48.1 / 79.3 → 48.8 / **79.7%**, 1.213 → 1.191 MB, raw 549 → 535 KB.

  The no-audit figures are 1.4 points lower for the EXE and 1.8 points lower for RendDX9.
- **Without the appendix** the effective figures are the same. The appendix's addresses move from "cited" to console-evidenced (c): EXE c ev 612 → 623 KB.
- **With `--headings`:** RendDX9 38.2 / 73.9 → 38.8 / 74.2%, AIDLL 46.4 / 70.2 → 46.9 / 70.4%, all three 48.2 / 79.5 → 49.0 / 79.9%.
- **This classifier's own propagation precision** (`uncited_noaudit_recal`):
  - factors EXE 0.45, RendDX9 0.71, AIDLL 0.42, the same on both sides;
  - calibrated: EXE 68.4 → 69.0%, RendDX9 72.7 → 73.0%, AIDLL 66.2 → 66.6%, all three 69.0 → 69.5%.

  The reused EXE factor (0.84) is still about twice this classifier's. The absolute EXE figure is soft; the before/after is not.
- **What moved:**
  - EXE: about half a point. Most of it is the HUD and game-rule code coverage-5's status blocks list: the combat-area warning, the seat weapon HUD and item lists, props, ladders, the seated look, the weapon flags, the blinkers, name tags and the distance fade.
  - RendDX9: +0.2 (the light filter, the terrain and undergrowth light passes, name tags).
  - AIDLL: +0.3 (the vehicle plan's Mobile branch, the guided plan on foot, the SAI's box-area status).
- **Missing code:** 15 KB less raw (d) (627 → 612 KB) and 23 KB less calibrated (d) (1.288 → 1.265 MB). (d) is now mostly residue: dev rows, editor code and propagated container code (section 2g).

## 2. Remaining decodable gaps, ranked by player visibility

Effort: **S** = days, **M** = 1–3 weeks.
- Call counts are from this corpus. Executors and offsets are from `symbols/BF2142.exe.template_properties.csv`.
- "Templates" are the shipped `.con` / `.tweak` files that set the property (Objects archives).
- Every gap in coverage-5's §2 that its status blocks mark done is left out. Its open leftovers are folded in below.

### 2a. Local play

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| L1 | **Sticky projectiles** | C4, the decoy, the sonar, the shotgun's explosive rounds, mines, hubs, the shield and the beacon bounce off and settle like grenades. C4 thrown at a tank doesn't stay on it. | `collision.maxStickAngle` (comp 0xc54b `+0x10`, executor `0x004f27f1`): 11 templates, Unl_c4_Projectile among them (45 / 180 / 25). `collision.stickToVehicles` (`+0x16`, `0x004f2b6c`) is set 0 on 7 (the mines, decoy, shield, beacon, hubs), so the rest stick to vehicles. Both are unread. engine.md:7697: "only works through StickyCollisionComp (`0x005e83d0`), which the rewrite has none of". GameWorld.cpp:3369: "Mesh projectiles (C4, mines, hubs) … here they bounce the same way." | S–M |
| L2 | **Motion sensors and the radar abilities** | The sonar ball, the Bp1 sonar and the radar grenade reveal no one. The recon drone's and the UAV's ring isn't drawn on the minimap. | The Ability component (0xc4b3) is set by 5 templates: unl_sonar_Projectile and Bp1_sonar_Projectile (`radarRadius` 100 / 60), Bp1_Grenade_Radar_Projectile (40), unl_drone_recon_Projectile (65) and commander_uav_drone, with `radialRadar` / `showPlayers` / `showVehicles` (executors `0x0054dc61`, `0x0054db42`, `0x0054d904`, `0x0054da23`). All are unread. No src reads `ability.radar*`. `reportBattlefieldObjects`' only callers are the cloak, the radio spot, the recon drone and the bots (GameWorld.cpp:7812, 7831, 11134; Radio.cpp:455; Bots.cpp:5600). engine.md:7788: "`Ability.radarRadius` (65) is the minimap's ring and isn't drawn yet". The reader isn't traced. | M |
| L3 | **Spotting vehicles and objects; the typed spotted message** | The spot key reports only soldiers. An APC, a Titan or a walker can't be spotted, and the voice says no "APC spotted" / "Titan spotted". | Radio.cpp:451 (`spotCrosshair` takes only `crosshairTarget`'s player). `vehicleHud.canBeSpotted` (19 templates; engine.md:6684–6688: "the rewrite's spotting reports players … not objects"). `radio.spottedMessage` (comp 0xc4b7 `+0x74`, `0x0054fb6d`; 40 templates: apc_spotted, infantry_spotted, airtransport_spotted, titan_spotted, heli_spotted…; engine.md:6027: game slot 52, `0x0042e9d0`). | S |
| L4 | **The minimap's combat-area texture** | The out-of-bounds region isn't shown on the minimap, on every map. The warning and countdown work. | engine.md:9202 "Not rewritten: the minimap's area texture" (HUD `0x0044c7b0` family). | S |
| L5 | **Lock-on and guided-weapon details** | No lock tones on the AA launchers. The guided weapons' alternate crosshair, target-at-max-distance and connection effect are missing. The TV / guided turrets don't block their base's input. | `weaponHud.firstLockSound` / `secondLockSound` (TargetTrack on as_aa, eu_aa, the mechs' AA, static_aa; `0x00559673`, `0x005597ff`). `weaponHud.altCrosshairIcon` (4; `0x005576cc`). `target.setTargetAtMaxDistance` (5; `+0x7c`, `0x004ee089`). `target.connectionEffectOnlyWhenZoomed` (3; `0x004ee343`). `blockInputOnRotBundle` (4; `0x004e1d2b`). All are unread; none is in engine.md. (`target.targetObjectTemplate` is settled: engine.md:1494–1497, the rewrite steers at the point itself.) | S each |
| L6 | **Sniper zoom fine-tuning and zoom rules** | The three sniper rifles have no fine zoom steps, and neither does the zoom-upgrade unlock. Tank guns zoom as if not busy; untoggled fire isn't refused. | `zoom.useZoomFineTuning` (as_sni, eu_sni, unl_adv_sni; `0x004eb9c7`), `zoomFineTuneSteps` (`0x004eb8ad`), `zoomFineTuningNeedsUnlock` (`0x004ebae1`), `ability.hasZoomUpgradeAbility` (UnlockEU / Pac_ReconSni1; `0x0054dd78`), `zoom.zoomWhenBusy` (8; `0x004eaebd`), `zoom.allowUntoggledFire` (4; `0x004ec43b`). | S |
| L7 | **Gadget and weapon timings** | Grenades and hubs reappear in hand at once. The soft launch delay is missing. The underslung shotgun and rocket don't share the rifle's magazine. EU soldiers don't drop their helmet. Duds go off. | `fire.showWeaponAfterReloadDelay` (9: grenades, hubs, sonar; `0x004e3990`). `fire.fireLaunchDelaySoft` (5 grenades, 0.8; `+0x9c`). `ammo.magLinkWeapon` (14; `+0x48`, `0x004e6528`). `dropHeadwear` (Kit `+0x2d8`, the 4 EU kits). `detonateDelay` / `dudMaterial` (GenericProjectile `+0x33c` / `+0x340`; 4 projectiles). Unread: `ammo.allowReloadWithFullMag` (the silos; Weapons.cpp:370 "aside"). | S each |
| L8 | **Vehicle and seat details** | See the list below this table. | See the list below this table. | S each |
| L9 | **Flares and a few HUD leftovers** | No FlareBlink and no flare reload time on the HUD. The name tags' icon change doesn't blink. A vehicle's help pop-up ignores autoUseAbility. | engine.md:6766 (no FlaresReloadTime, `0x007be490`); the tag icon's blink (`+0x68` / `+0x6c`, coverage-5's last status); engine.md:742 (autoUseAbility, 108 templates, `0x00646deb`); engine.md:6811 ("the mech's foot animation", HUD). | S |
| L10 | **Repair events to Python** | Going by the name: repairing a Titan console, core or gun, or an AA / ground cannon, raises no script event. The reader isn't traced. | `python.onRepairEvent` (43 templates, values 27–55; comp 0xc4bb, `0x00551983`); no reader in src. | S |

L8, vehicle and seat details. None of these readers is traced, so the effects of `neverDrawAs1p`, `copySoldierRotationOnEnter`, `soundFilter`, `groundContactVolume` and the man-down offsets are read from their names:

| Property | What a player notices | Templates | Where |
|---|---|---|---|
| `vehicleHud.useChildArmor` | The AA and ground-cannon hubs' HUD armour is the hub's, not the gun's. | 12 | `0x0055427a`. engine.md:6507 says it is done (section 4, item 3). |
| `vehicleHud.enablePostProcessingOnGuiIndex` | The pod cannons' and guided seats' zoomed view has no post-processing. | 49 | `0x00554a71`. Only the weaponHud row is read (main.cpp:7006). |
| `neverDrawAs1p` | The hub guns and the Titans are drawn in first person. | 16 | PCO `+0x298`, `0x004ff358` |
| `copySoldierRotationOnEnter` | Pods ignore the soldier's facing on entering. | 6 | `0x004ff22b` |
| `soundFilter` | Sound from heli, vehicle and ground seats isn't filtered. | 42 seats | `0x004ded9a` |
| `groundContactVolume` | | 20 | |
| `manDownCameraOffsetInDof` / `InUp` | The man-down camera's offset is missing. | 8 | `0x004def45` |

### 2b. Joined and hosted play

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| J1 | **Client-predicted projectiles** | On a joined client, one's own grenades, mines, C4, sonars and guided missiles appear only when the server's ghost arrives: a round trip late. | `localPredictOnClient` (GenericProjectile `+0x298`, `0x004efe5a`; 24 templates, all 1); engine.md:1525–1530 (the 84 networked projectiles are the server's ghosts). | M |
| J2 | **Spectator remainder** | A joined spectator can't follow another player. The scoreboard has no spectator icon. | engine.md:1899–1900 | S–M |
| J3 | **Others' seats on a client** | A mount's turret turn and the hands' IK on the grips are missing for other players. A joined game's vehicle stamina shows 1. | engine.md:1451–1452; coverage-5's status (VehicleStamina, not networked) | S |
| J4 | **Ghost fields and dirty masks** | Minor client state; bandwidth. | Networkables.h:221, 235, 258, 355, 369; Networkables.cpp:1146; GameProtocol.h:458 and HostedGame.cpp:2329 (CreateSpawnGroup's bits); RemoteWorld.h:165 (the kick reason); GhostPriority.h:78, HostedGame.cpp:2596, engine.md:5144 | S each |
| J5 | **Server odds and ends** | A long rcon_feedback. Ranked minimums. Message 9 (demo URL). Refusing an applicant after a mutiny. The CD-key reason. | engine.md:5242, 5318, 5385; Commander.cpp:138; HostedGame.cpp:600 | S |

### 2c. Menus and front end

| # | Gap | Evidence | Effort |
|---|---|---|---|
| M1 | **The commander's camera offline.** The zoom buttons send event 0xf for a camera the local round doesn't have, so the offline commander map centres on the asset. | main.cpp:4150–4157; roadmap row 8 | S–M |
| M2 | **Commander remainder:** the AI commander's answers to bot squad leaders, the marker's height from its geometry. | engine.md:9696–9698 | S |
| M3 | **Demo playback remainder:** the demo camera's own input (`0x005fa5e0`), the HUD's Demo* strings, camera files (cameramode 1 / 2). | engine.md:5418–5421 | S–M |
| M4 | **The kit screens:** dragging (roadmap row 8: "the HUD has no dragging"), the kits' upgrade-icon setter. | engine.md:12092 | S |
| M5 | **HUD placement guesses:** the corner roots, the list columns, the mute icon, the scoreboard's black rows. | engine.md:6360, 9316, 9575, 6431; Hud.h:32; HudMenus.cpp:42, 409; main.cpp:536 | S |
| M6 | **HudBuilder authoring rows.** No shipped script calls them, so only mods are affected. | 24 stub rows | S |

### 2d. Rendering

| # | Gap | Evidence | Effort |
|---|---|---|---|
| R1 | **Decals:** the normal-mapped technique (`decalNormalMapName`, 10 calls); `+0x1a`'s refusal on an unseen owner. | engine.md:2594, 2616 | S |
| R2 | **Particles:** whether zoomed-mode systems with showInFirstPerson 0 draw unzoomed; the Particle object's blend; the per-instance time override. | engine.md:3308–3310, 1296, 1720 | S |
| R3 | **Terrain:** the per-material detail lists, cullerTypes other than 3, detailCullMaxLevel. | engine.md:2228 | S |
| R4 | **Distance fade remainder:** the children's second record and the walk's 0x1000000 skip flag. | engine.md:3144–3148 | S |
| R5 | **Small items:** the view's tint (PostProduction slot 0x7c), the bundle modifiers' start age and sound intensity, rotateAsAnimatedUV, the rain's two speeds. | engine.md:3723, 12220, 12198, 2706; Effects.h:1192 | S each |

**Status (rendering pass after this audit):**
- R1 done: normal-mapped decals (Decals.fx technique 2 for the sets of objects drawn this frame, `0x100e6a80` / `0x10163810` / `0x10164190` / `0x10164040` / `0x10163570` / `0x101631f0`; the world's set keeps technique 0; `decal.frag`, `DecalTemplate::normalMap`); the add's refusal on an unseen owner's set unless the template isStatic (`0x10166410`, `+0x1a` from `0x10164f10`, isStatic the RendDX9 template's `+0x20` via slot 10 `0x10167b70`). Test: checks in `Effects_DecalsOnMovingObjects`.
- R2 done but the time override: showInZoomedMode hides nothing; in first person a system shows with it or showInFirstPerson (`0x0061a1c0`'s flags & 5), and zoomed the first-person particles take the first-person meshes' view (`+8` read by `0x10131680` and its siblings through `0x100990b0`; cameras slot 0x5c / 0xdc). The Particle objects (tracers) draw with MeshParticleMesh.fx's Additive technique (`0x10135020` → `0x10167d90` with 1; the list's filler not found). The per-instance time override (engine.md:1720) is the animation system's, outside this pass. Tests: `RenderScene_SpriteFlipbooks`, `Effects_ViewsPickTheirSystems` updated.
- §4 item 8's first half done: `RenderScene_TerrainPointLightPasses` (`pickTerrainLights`, `pointLightInverseRange2`, `terrainPointLight` in MeshLights.h, used by `LevelRenderer::applyTerrainLights`).
- §2g rows 4, 5 and 9 are the GeometryOptimizer (BundledMeshOptimizer `0x100f6f00` / vtable `0x101d6da0`, StaticMeshOptimizer `0x100f9610` / `0x101d6e18`, base `0x10170290` / `0x101da970`), a build tool: (b) in docs/uncited.md. Row 9's sky-dome reading was wrong; the SkyDome's technique choice (`0x100fc410`), flare loader (`0x100fc9f0`) and effect (`0x100fcd80`) are now cited in LevelRendererPost.cpp.

### 2e. AI

| # | Gap | Evidence | Effort |
|---|---|---|---|
| A1 | **Bots don't zoom.** Bot snipers and scoped riflemen aim unzoomed. | engine.md:11348 | S–M |
| A2 | **Sighting and vehicle ammunition:** every sighting is clear (no see-through materials); bots' vehicle guns have no magazines. | engine.md:11385–11387 | S–M |
| A3 | **Fire-plan stand-ins:** C4's guard on a moving target (the bot object's `+0x80`), the attention stamp (`0x100eb7f0`), the guided plan's order spot (`0x100db550` / `0x100db3b0`). | engine.md:11373–11392, 11428; Bots.cpp:2886–2897 | S each |
| A4 | **Bot leftovers:** ITAir vehicles, the terrain-normal "upright" test, seat changes and pick-ups, the grenade warning's sender. | Bots.cpp:3481, 4321, 917, 1612; engine.md:10310 | S each |
| A5 | **AI hint stubs the scripts call:** `GameLogic.defaultTimeToNextAIWave` (11 a run: the co-op maps' AI waves), `ObjectTemplate.setCheckObstruction` (33 a run), `weaponTemplate.shouldRearm` / `fireRate` / `isAntiAirWeapon`, `AITemplatePlugIn.setUseNoPathfindingToGetToObject` / `vehicleNumber`, `Object.notInAI`. | `stubs_called.md`; constructors `0x004283b6`, `0x004e21f5`, `0x00887dca`, `0x00884df1`, `0x006ed6e6` | S |
| A6 | **Scripted-bot (SAI) commands.** Only mods use them. | engine.md:11286–11317 | S |

**Status (AI pass after this audit):**
- A1 done: bots zoom (`BotManager::stepZoom`; the zoom statement `0x100d8250` of the fire statements `0x100777a0` / `0x100781e0`, the moves' zoom-out `0x100e5f70`, Idle's zoom-in in `0x1006a500`; the zoom input `0x00892800` ← `0x0088a8c0`; the look's zoomed scale `0x00586420`, devModZoom and the zoomed walk). Sensing is unchanged by zoom (the cone's row is the object's flag bit 10). Test `Bots_ZoomScopedWeapon`.
- A2 done: the sight ray passes `Material.isSeeThrough` faces and reports them (`0x00897df0` → `0x00898040`, filter `0x00897d50`, material slot `0x88`); flag 0x40 feeds the weapon choice (flags 0x48) and the targets' loop's FireAndForget (`0x10075fb0`). Seat guns' magazines are read (`BotManager::seatAmmo`) and the vehicle plan's reload press is made (`0x100795a0`, adaptor slot `0xe4`); the rewrite's seats don't yet act on c_PIReload (GameWorld, rules side). Test `Bots_SightThroughSeeThrough`.
- A3: C4's guard is done (the bot object's slot `0x3c`, its facing, not `+0x80`); the guided plan's order spot is done (`BotManager::guidedOrderSpot`: `0x10034a10`, `0x1005d940`, `0x100269d0`, `0x100db550`, `0x100db3b0`, `0x100d8f90`). Not possible: the attention stamp's writer — in AIDLL's decompilation only the find `0x100eb7f0` and the destructor `0x100ed5b0` touch the sensing agent's map at `+0x84` (direct, indexed and helper accesses searched), so the stand-in stays.

### 2f. Not decodable or out of scope (unchanged)

- the online back end (FESL/EASTL, GameSpy GP, BFHQ, leaderboards, award grants, the PlayerProgress diagrams, engine.md:12150–12156);
- PunkBuster, IGA, SecuROM;
- the lightmapper and TextureAtlasBuilder;
- editor and debug objects;
- Novint Falcon and the G15 LCD;
- the AI's real-time CPU budgets.

### Top 15, overall

| Rank | Gap | Area | Effort |
|---|---|---|---|
| 1 | L1: sticky C4, mines and gadgets (StickyCollisionComp `0x005e83d0`) | local, every map | S–M |
| 2 | L2: the sonars and radar grenade reveal nothing; the drones' radar ring | local | M |
| 3 | L3: spotting vehicles and objects; the typed spotted message | local and joined | S |
| 4 | L4: the minimap's combat-area texture | local, every map | S |
| 5 | J1: client-predicted projectiles (`localPredictOnClient`) | joined | M |
| 6 | L5: lock tones and the guided weapons' HUD and target flags | local | S |
| 7 | A1: bots don't zoom | AI | S–M |
| 8 | L6: sniper fine zoom and the zoom rules | local | S |
| 9 | L7: gadget timings, magLinkWeapon, dropHeadwear, duds | local | S |
| 10 | L8: vehicle details (useChildArmor, vehicle post-processing, neverDrawAs1p, soundFilter) | local | S |
| 11 | M1: the offline commander's camera | menus | S–M |
| 12 | J2 + J3: spectator following; others' turret turn and IK | joined | S–M |
| 13 | A2 + A3: see-through sighting, vehicle-gun magazines, the fire-plan stand-ins | AI | S–M |
| 14 | R1 + R2: normal-mapped decals; zoomed-mode particles and blend | rendering | S |
| 15 | A5: the co-op AI wave time and the AI hint stubs | AI | S |

**Status (HUD and joined-client pass after this audit: ranks 3, 4, 5, 10's HUD parts, 11, 12; section 4 items 2–5 and 8's second):**
- L3 done: `Radio.sendSpottedMessage` reports the crosshair's soldier or vehicle and, with its first argument, says the typed voice on TCTeam (`0x007c8160` → game slot `0xd0` `0x0042e9d0` → slot `0x50`): the spotted object's Radio component's `radio.spottedMessage` (a soldier's kit's, a vehicle's own), "spotted" otherwise. The spotted object follows `0x0075bfe0` / `0x0075be30`: the root when canBeSpotted, the rider's soldier with usePlayerIcon. A joined client picks the enemy from its own view and sends the object's ghost, as the exe's client does. `menu::spotChoice`, `crosshairEnemy`; test `GameWorld_SpottedMessageVoice`.
- L4 done: the minimap's combat-area layer (`0x0078b490`, RendDX9's texture ops `0x10187e70`, draw `0x0077d200`) and the map's framing on the applying areas (`setMapArea`, which nothing called before). `src/Main/HudCombatArea`, `FileManager::addMemoryFile`; test `HudCombatArea_MinimapLayer`. Screenshot: the scratchpad's `hudagent/area_spawn.png`.
- J1 not done, by the exe: `localPredictOnClient` (template `+0x334` through the `+0x298` interface, vtable `0x00969318` slots 0x30 / 0x34) has no reader in BF2142.exe 1.51; the template save's "localPredictOnClient" line writes `damageOwner` (`+0x32c`). A 1.51 client waits for the server's ghost too (engine.md, "Not read at run time").
- L8's HUD rows done: `vehicleHud.useChildArmor` (template `+0x102`, `0x005c6460`: the Titan gun hubs show their gun's armour; section 4 item 3 resolved by implementing it) and `vehicleHud.enablePostProcessingOnGuiIndex` (template `+0x11c`, `0x006376c0`; its reader isn't traced, so the seats that set it turn the zoomed-view pass on for their own view). `menu::childArmorTemplate`, `seatPostProcessing`.
- M1 done: a local round's commander has the overhead camera a server's has: moved kept 80 m inside the terrain (`0x005f8170`) and panned by the movement keys (`0x005f8510`, 250 m/s² — `*0x00a7303c` "overheadCamera-acceleration" — up to the template's 50 m/s). `menu::OverheadCamera`; test `HudCommander_OverheadCameraPan`.
- J2 partly done: a joined spectator follows the server's players (`PlayerCamera::setPlayers`). Not done: the scoreboard's spectator icon (`0x0074e940` picks the kit column's icon, a column the rewrite's scoreboard lacks).
- J3 not done: the turret turn and hand IK of other players' seats on a client (the seated pose is the game rules' `animateRemoteSeated`; neither value is in the soldier's state).
- L2's HUD part done with the game rules' `radarSources` / `radarContacts`: the rings (`0x0078f060`: icon_uav_circle, radial ones turned by the sweep, the others growing and fading) and the contacts on the minimap (`MapMarker::worldSize`). L5's lock tones are played from the game rules' `takeLockTones`. Not done: `weaponHud.altCrosshairIcon` (block `+0x8c`, executor `0x005576cc`; no HUD reader found) and the other L5 rows (game rules).
- Section 4: item 2 (engine.md's animated-UV line), item 4 (roadmap row 8) and item 5 (coverage-5's §2 and §3 now point here) fixed; item 8's joined combat-area countdown has `NetHost_JoinedCombatAreaCountdown`.

**Status (game-rules pass after this audit: ranks 1, 2's game side, 6, 8, 9, 10's game rows, 15; section 4 item 1):**
- L1 done: StickyCollisionComp (`0x005e83d0`; template `0x005e8820`, maxStickAngle as a cosine `0x005e8030`): soldiers and PCO-derived objects bounce it off, a face over the angle bounces, an unknown or water material bounces, stickToTerrain / stickToStaticObjects / stickToVehicles; stuck, it lies at the contact and rides the vehicle (`GameWorld::stickProjectile`, `rideStuck`; the whole vehicle's frame, not the hit bundle's). A round only lying on a vehicle falls when it moves away. `Vehicles_StickyProjectilesRideVehicles` (C4 on eu_tank rides 25 m; the AP mine bounces off and sticks to the ground). Placed hubs, beacons and shields aren't thrown in the rewrite, so their flags act on nothing; the client's StickyProjectile event isn't sent.
- L2's game side done: the radar sources are client-side minimap code in BF2142.exe (no server scan, nothing networked, no arming test; `0x00656550`, `0x00789f30`, `0x00789a10`, `0x007926c0`, `0x00791660`). The Ability's defaults (`0x00631790`: show flags and radialRadar 1, radius 0); `ProjectileSettings::radarRadius`, `GameWorld::radarSources` / `radarSweep` / `radarPasses` / `radarContacts` (`GameWorld_RadarRingsPassEnemies`). Corrections to the L2 row: Bp1_sonar's radius is 40, and commander_uav_drone sets no Ability. unl_sonar shows vehicles only, Bp1_sonar and the radar grenade soldiers only.
- L5 done where game-side: the lock tones (`0x00657ce0` from `0x007e5a10`; `GameWorld::takeLockTones`, played by `bf2142`; `GameWorld_LockTones`). altCrosshairIcon is the HUD's (`0x00657bb0`: while the fire state is above 0). setTargetAtMaxDistance has no reader; connectionEffectOnlyWhenZoomed's executor always writes 1 and gates a connection effect nothing ships; allowUntoggledFire needs a changeCameraId nothing ships; blockInputOnRotBundle not looked at.
- L6 done: fine tuning (`0x005da670`, `0x005da820`, `0x005da890`, `0x005da290`) with hasZoomUpgradeAbility (`0x005706b0`), zoomWhenBusy (`0x005c50e0`) (`FirstPerson_ZoomFineTuning`). allowUntoggledFire: see L5.
- L7 done: showWeaponAfterReloadDelay (ThrownFireComp `+0xa4`, `0x005e5720` / `0x005e49d0` / `0x005e5610`; `Weapons_ThrownWeaponHiddenUntilShown`), magLinkWeapon (the shotgun and rocket link each other, not the rifle; `0x005f45e0`, `0x005f46f0`), allowReloadWithFullMag (`0x005f4140`; `Weapons_FullMagReloadAndMagazineLink`), duds (`0x005b4ff0`, `0x005b54e0`; `GameWorld_DudsDontGoOff`). Settled as doing nothing to a round: fireLaunchDelaySoft (only the reload's busy time, `0x005e4e70`, unmodelled). Settled as inert in BF2142.exe: dropHeadwear (the drop `0x005ee2f0` is never started and no "helmet" template ships). The shipped values in the L7 row: showWeaponAfterReloadDelay is 0.05 / 0.2 s on 9 ThrownFireComps (`0x004e4d1e`; `0x004e3990` is SingleFireComp's unused one).
- L8's game rows done: soundFilter is the radio filter under a seated sender's voice (`0x0042e0c0`; `GameWorld::radioFilter` → `VoiceMessages::Heard::senderFilter`), neverDrawAs1p is the effect bundles' first-person test (`0x006187c0`; `GameWorld::firstPersonEffects`, for the renderer; 4 Titan roots set it) (`Vehicles_RadioFilterAndFirstPersonEffects`). The hub armour (useChildArmor) is the HUD pass's (above).
- A5 done where there is something to do: defaultTimeToNextAIWave is bound (`0x00428474`, server `+0x28c`, 8) and read by nothing in BF2142.exe or AIDLL, so there is no wave timer; Object.notInAI destroys the object in sp1 / gpm_sp / gpm_coop (`0x006ed7a4` → `0x006f6ef0`, `0x006f91e0`; `WorldConsole_AiWaveTimeAndNotInAI`). Not done: setCheckObstruction is a weapon rule (the pod launchers' five rays `0x005c2310` refusing the trigger), not a hint; weaponTemplate.* aren't read by AIDLL's weapon choice; the AITemplatePlugIn rows stay stubs.
- Section 4 item 1 fixed: `Commander_OrdersAndBots` skips the bots' walk without the game data.
- Also: a seat's c_PIReload reloads the seat's active firearms (`0x00590e10` event 10 → `0x005cacf0` → `0x0063ca60` → `0x005c62c0` → `0x005c50e0` case 10; `Vehicles_SeatReloadPress`), so the bots' seat reload presses (BAPATrigger input 32) act.
- Tests: 756 with `BF2142_GAME_DIR`, all passing (this pass adds 12); `ninja -C build/win32` builds. Bot rounds (`BF2142_BOT_SEED` 1–2 on Cerbere_Landing gpm_sp 16, 1–7 on minsk_coop gpm_coop 32, 300 s) fight as at HEAD with the same CPU (4.8–9.6 s user each).

**Status (seats, joined clients and HUD pass after the game-rules pass):**
- L6's seats done: a seat gun with a DefaultZoomComp zooms on the alternate fire (`0x005c58a0` → `0x005c50e0` → `0x005c0da0` → `0x005da670`; the field of view `0x005da3f0` → `0x00591740`, level 0 the camera's own `0x00610150` / `0x005da290`; out on a camera change unless inside or nose, `0x005da5c0` / `0x00591510`), zoomWhenBusy on seats (the tanks' and ATs' 1, the static AV's 0); the zoomed HUD (altGuiIndex by the firearm's slot 300, `0x00657a70`) and the seat post-processing by the index shown; the seats' turn isn't zoom-scaled (`0x00586420` is the soldier's). `anim::SeatZoom`, `PlayerCamera::stepSeatZoom`, `menu::weaponGuiIndex` (`FirstPerson_SeatZoom`, `HudVehicle_SeatZoomGuiIndex`). `zoom.preventZoomWhen3P` (`+0xe`): no reader found. Not on a joined client (no inside seat view there).
- L1's client part done: the StickyProjectile event (0x20; `0x0050f850` / `0x0050f460` / `0x0050f720` / `0x0050f5d0`, the joiner's copy `0x0057dda0`): `HostedGame::stickyEvent`, `net::make/parseStickyProjectile`, `RemoteWorld::stuckProjectile`, `GameWorld::stickRemoteProjectile` (the client's round stops and rides the client's object of the vehicle's ghost) (`NetHost_StickyProjectileEvent`, `RagDoll_RemoteStickyProjectileRides`). The rewrite names the root ghost, part 0, bundle 0 (its rounds ride the whole vehicle); the thrower's firearm list (`0x94c0` slot 0x16c) isn't kept on a client.
- J3 done: the exe sends neither value in the soldier's state; the turrets are the vehicle ghost's RotationalBundle networkables (read `0x00602070`) and the client finds the mount and the IK object (`0x005ee910`, `0x005efc00`) in its own tree. `GameWorld::setRemotePartPose` (the ghosts' bundle turns) gives `animateRemoteSeated` the carrier seat's yaw bundle and `seatIkTargets` the grips (`RagDoll_RemoteSeatedTurretAndGrips`). Not done: bone-part mounts (the mechs) on a client.
- L5's HUD part done: altCrosshairIcon while the fire state is above 0 (`0x00657bb0`, `0x005c1b60`; `WeaponHudInput::firing`) (`HudWeapon_AltCrosshairWhileFiring`).
- L8's render part done: the local player's vehicle's object bundles start in their first-person form in the inside or nose view unless neverDrawAs1p (`0x006187c0`, `0x0061a1c0`; `ObjectEffects::ObjectState::firstPerson` from `GameWorld::firstPersonEffects`) (`Effects_ObjectBundlesFirstPersonForm`). The form is taken as a bundle starts; the zoomed systems' first-person view (`+8`) isn't moved.
- A5's setCheckObstruction done: bound (`0x004e236e`, `checkObstruction`), the five rays (`0x005c2310`) refuse an APC pod seat's launch (`0x005c2ee0`, `0x005c50e0`) (`GameWorld::fireObstructed`; `GameWorld_FireObstruction`).
- J2's spectator icon done: the scoreboard's kit icon column (`0x0074e940` from `0x007bb540`: spectator, death, faded enemy, the kit's icon, drawn black) (`menu::scoreboardIcon`; `HudScoreboard_KitIconColumn`); the HUD manager's slot 0x410 test isn't modelled; the column's place is the rewrite's.
- A5's AITemplatePlugIn rows bound: setUseNoPathfindingToGetToObject (Unit `+0x15`, `0x00884eaf`; AIDLL reads it through `0x008a1d10` / `0x100aa570` in `0x1006c520`, `0x1006dac0`, `0x1006bd70`, `0x10070200`, `0x10070da0`, `0x10071a10`, `0x100724e0`) and vehicleNumber (Mobile `+4`, `0x008834f9`; `0x008a1770` / `0x100a46c0`): both only steer the tests on the Mobile's per-map point records, which the rewrite doesn't keep, so they change no decision (`Bots_PlugInNoPathfindingAndVehicleNumber`).
- Tests: 766 with `BF2142_GAME_DIR`, all passing (this pass adds 10); `ninja -C build/win32` builds. Bot rounds (`BF2142_BOT_SEED` 1–3 on Cerbere_Landing gpm_sp 16 and minsk_coop gpm_coop 32, 300 s) give the same kills as HEAD's build with the same CPU (4.6–5.0 s user on Cerbere_Landing, 7.7–8.5 s on minsk_coop; HEAD 4.6–4.9 and 7.3–8.1, run side by side).

**Status (RendDX9 missing-code pass: §2g's RendDX9 runs, `uncited.py --cite cite_noaudit.json --min-run 1024`):**
- Before → after (RendDX9, no audit docs, without the appendix): cited 458 functions / 293,470 B (17.5%) → 1,194 / 386,953 B (23.1%); raw (d) 154 → 109 KB; (d) runs of 1 KB or more 44 / 114.5 KB → 33 / 70.0 KB; calibrated effective 72.1% → 75.0%. Most of the citation gain is the present-uncited console rows below; classifying in uncited.md moves none of these figures (the no-audit run leaves it out).
- The 44 (d) runs of 1 KB or more, by outcome (sizes as first measured): cited 7 / 14.4 KB, editor and tools (b) 26 / 80.5 KB, replaced by design 9 / 15.9 KB, still missing (d) 2 / 3.7 KB. None was rendering the rewrite lacked and could take within the frame budget; the two that are (SimpleShadowmap, the terrain as a shadow occluder) are written up in uncited.md's new "RendDX9: the missing-code pass" with the reasons.

| Run | Size | Outcome |
|---|---|---|
| `0x100f96b0` | 10,030 | (b) GeometryOptimizer (already in uncited.md) |
| `0x10170370` | 7,660 | (b) GeometryOptimizer base (already) |
| `0x100ce6c0` | 6,408 | (b) TerrainEditable's `terraindata.raw` writer |
| `0x100f7b80` | 6,287 | (b) GeometryOptimizer (already) |
| `0x100ca150` | 4,299 | (b) nvdxt / tgasplit |
| `0x1002bb90` | 4,071 | (b) draw statistics (already) |
| `0x10158520` | 3,976 | (b) the terrain editor's .dds splitting |
| `0x1001af30` | 3,715 | replaced: the .fx preprocessor |
| `0x1016f370` | 3,693 | (b) the optimizer base's writer and slots |
| `0x1002f3d0` | 2,976 | (b) statistics and profiler containers |
| `0x100f6330` | 2,949 | (b) BundledMeshOptimizer's slots |
| `0x10095810` | 2,630 | replaced: the overgrowth tiles' vertex streams |
| `0x100793e1` | 2,595 | (d), low: `nametags.*` tuning rows, unbound, called by no shipped script |
| `0x100b1bf0` | 2,571 | cited: the water patch quad `0x100b1f10` (LevelRenderer::buildWater); the rest TerrainEditable's (b) |
| `0x10121790` | 2,455 | cited: the overgrowth type lookup `0x10121790` (LevelRenderer.cpp); `0x10121930`'s painted maps (b) |
| `0x100118d0` | 2,380 | (b) the VertexBuffers dump |
| `0x10179bf0` | 2,370 | (b) lightmap samples |
| `0x10003b10` | 2,303 | (b) frame capture |
| `0x1008fb30` | 2,262 | cited: gathering the trees into cells, with the preLoad `0x100909e0` (LevelRenderer.cpp) |
| `0x100f6f90` | 2,210 | (b) BundledMeshOptimizer |
| `0x10009cc0` | 2,186 | replaced: display modes |
| `0x10042ca0` | 2,045 | (b) profiler sections |
| `0x100a0dc0` | 1,950 | cited: the per-part bounds (LevelRenderer.cpp's partBounds) |
| `0x100ff650` | 1,892 | (b) the road compiler |
| `0x1014f1d0` | 1,817 | cited: TerrainMakeDynamicLightMap / TerrainDynamicShadow `0x1014f400` (terrain.frag) |
| `0x1012d5b0` | 1,814 | cited: the NSA particles' fill `0x1012d720`, batch `0x1012d660`, draw `0x1012e920` (Effects.cpp collectAligned) |
| `0x100cbc90` | 1,774 | (b) TerrainEditable |
| `0x10030140` | 1,735 | (b) statistics |
| `0x1008d1f0` | 1,707 | (b) Overgrowth.paintMaterial |
| `0x1008767d` | 1,643 | (b) Overgrowth editor rows |
| `0x1008bae0` | 1,637 | (b) Overgrowth.raw save |
| `0x100b53c0` | 1,501 | cited: DrawWater (buildWater's comment, with what the rewrite does differently) |
| `0x1017a690` | 1,426 | (b) lightmap samples |
| `0x1007a01f` | 1,412 | (b) StaticMeshRenderer lightmap / index-list dev rows |
| `0x1002b4d0` | 1,339 | (b) statistics |
| `0x10034c10` | 1,332 | replaced: RaDefines.fx / GSLoadObfuscated |
| `0x1001c040` | 1,308 | replaced: the effect loader's maps |
| `0x1001d0f0` | 1,290 | replaced: the same |
| `0x100092f0` | 1,245 | replaced: display modes |
| `0x100c7400` | 1,189 | (b) the terraindata writer |
| `0x100aa290` | 1,147 | (d): SimpleShadowmap.raw, not done (uncited.md) |
| `0x10030950` | 1,103 | (b) statistics |
| `0x1001a680` | 1,099 | replaced: the .fx preprocessor |
| `0x1000af90` | 1,067 | replaced: the device's caps probe |

- Under 1 KB, also cited: the `.font` reader `0x10180dd0` (Nametags.cpp), SkinnedMeshDrawShadowMap `0x10162c70` (the shadow casters), the MeshParticles draw `0x101351e0` / `0x10135460` (engine.md). Classified: TerrainOccluderShadows `0x1014d450` (d), LeafOG / TrunkOG `0x1008e400` (replaced); the rest are containers in the replaced effect and device code.
- Present but uncited (c): every implemented RendDX9 console row's constructor and executors are now cited on the line that binds the row (260 rows, ≈75 KB: LightSettings.cpp, PostProductionSettings.cpp, EffectSettings.cpp, OvergrowthSettings.cpp, GeometryTemplateManager.cpp, TerrainData.cpp, Nametags.cpp), and the mesh loads' paths (`0x100a1860`, `0x100d3080`, `0x100e9060`, GeometryTemplateManager.cpp). The largest (c) runs left are propagated labels in the particle and mesh renderers (`0x10143980` 17.5 KB, `0x10135bb0`, `0x10141350`) with no evidence of their own.
- Fixed on the way: `nametags.createIcon` / `createBar` take an icon id by number as the console's parser does (`0x1007168d`; `Nametags_IconRegistry`).
- Frame cost (Cerbere_Landing gpm_sp 16, 15 bots, 300 frames): 2.78, 2.68, 2.66 ms; nothing drawn changed. Tests: 768 with `BF2142_GAME_DIR`, all passing; `ninja -C build/win32` builds.

### 2g. The remaining large uncited and missing code blocks, by size

**Missing ((d) runs, no audit docs, ≥ 2 KB).** EXE: 20 runs / 103 KB (fec6f08: 23 / 111 KB). RendDX9: 21 / 80 KB. AIDLL: 5 / 21 KB. The other ~70% of raw (d) is in runs under 2 KB.
- **Gone since fec6f08:**
  - `0x004e3200` (the `fire.*` setters: maxProjectilesInWorld …);
  - `0x004e1b9a` (allowEmoteAnimation / blockInputOnRotBundle; now split by read rows);
  - `0x0051bc36` (destroyableWindowsTemplate / hasGlow).
- **No run is new.** Little gameplay is left in (d): the largest runs are dev and editor rows, online-gated menu code, and container code propagated from one string.

| # | Run | Size | What it is |
|---|---|---|---|
| 1 | EXE `0x0047f5e0`..`0x00483b50` | 13.4 KB | SwiffHost's stats fields (`acdt`, `wdths-%d`, `tdmg`, `suic`, `twsc`): the BFHQ stats pages. Online-gated. |
| 2 | EXE `0x006e8570`..`0x006ebdbc` | 12.1 KB | The object grid / spatial tree build ("Recursing again… leaf can't hold all the elements", `Object.initGrid`) and IObjectTemplate_ptrproxy reflection. The rewrite has its own culling. |
| 3 | EXE `0x00683e40`..`0x00686e89` | 10.2 KB | The input devices' IDKey_ / IDButton_ name tables. The rewrite composes its own names (ControlMap.cpp:174, GameControllers.cpp:220), so they are substituted. |
| 4 | RendDX9 `0x100f96b0`..`0x100fc371` | 10.0 KB | Propagated (d) only ("callee of", "sandwiched"): container code in the sky-dome / flare module. The sky (sky.frag) and the flare (LevelRendererPost.cpp:461, `0x100fd900`) are rewritten. A classifier artefact. |
| 5 | RendDX9 `0x10170370`..`0x10172941` | 7.7 KB | The same in the name-tag renderer module (NameTagRenderer `0x1016c1e0`, rewritten in src/Main/Nametags). An artefact. |
| 6 | EXE `0x008ae4e8`..`0x008b00be` | 6.9 KB | The RagDoll editor rows (`reset`, `save`, …). The rewrite parses ragdolls itself. |
| 7 | EXE `0x0066b175`..`0x0066cee1` | 6.7 KB | `Console.getArgumentTypes` / `getEnumName` reflection. Dev. |
| 8 | RendDX9 `0x100ce6c0`..`0x100cffe1` | 6.4 KB | TerrainEditable's `terraindata.raw` writer. Editor. |
| 9 | RendDX9 `0x100f7b80`..`0x100f9607` | 6.3 KB | The sky dome's technique choice (SkyDomeNV3x / NoClouds / DualClouds) and the `flaresprite` loader, uncited. The behaviour is in sky.frag and LevelRendererPost.cpp. Present-uncited in fact. |
| 10 | EXE `0x006ee282`..`0x006ef9bd` | 5.9 KB | `Object.initGrid` / `isVisible` / `direction` / `scale` console rows. Editor. |

After those:
- EXE `0x0042ec60` (5.7 KB): the network event names (ToggleFreeCameraEvent, ECGame…).
- EXE `0x0070cb00` (5.6 KB): beside QuadTreeCuller; no strings.
- AIDLL `0x1002db50` (5.1 KB): the bot debug printer ("** NO PLAN **", "HasZoom: ").
- EXE `0x004196a4` (5.0 KB): `SquadManager.listSquads` / `listSquadMembers`.
- AIDLL `0x10052fe0` (5.0 KB): no strings.
- EXE `0x00504be6` (4.4 KB): the template component class names.
- RendDX9 `0x100ca150` (4.3 KB): the nvdxt / tgasplit texture tool calls.
- RendDX9 `0x1002bb90` (4.1 KB): renderer statistics.
- AIDLL `0x1002eff0` (4.1 KB): the SAI debug printer.

**Largest uncited runs (no audit docs, without the appendix), with the classifier's makeup:**

EXE (≥ 30 KB):

| Run | Size | Makeup | What it is |
|---|---|---|---|
| `0x00840340` | 177.7 KB | a 157 KB | FESL / EASTL |
| `0x008155c0` | 131.7 KB | a | the FESL game browser |
| `0x007380a0` | 81.8 KB | a 53, c 22 KB | GameSpy SDK and HTTP |
| `0x008025b0` | 70.8 KB | a | the FESL account fields |
| `0x007c9090` | 53.6 KB | c 28, ? 20 KB | the HUD's font, QuadDrawer and Ref2System; substituted |
| `0x0072a180` | 50.8 KB | a | GameSpy GP |
| `0x00760a26` | 50.0 KB | c 30, d 7 KB | the HUD map node's console rows (createMapNode, setMiniSize…); cited only by the appendix |
| `0x0046c880` | 47.2 KB | c 29, d 8 KB | the command line, profile and settings managers |
| `0x008c970a` | 40.8 KB | a | static init and networkable registrations |
| `0x00696f80` | 39.0 KB | c | file paths and the app-data folder |
| `0x00870c90` | 37.8 KB | d 15, c 21 KB | dice::meme::Effect: the shader-state names ("Source color", …) |
| `0x004e886d` | 34.8 KB | | the template setters (deviation, sway); without the appendix |
| `0x0065d840` | 30.7 KB | b | the profiler's XML |

RendDX9 (≥ 20 KB):

| Run | Size | Makeup | What it is |
|---|---|---|---|
| `0x101885b0` | 89 KB | b | debug drawers, the ported menu shader |
| `0x10053bd0` | 60 KB | a | IGA |
| `0x101543a0` | 49 KB | | the terrain editor |
| `0x1016d0f0` | 40 KB | d 11 KB | name tags and the specular LUT |
| `0x10101310` | 37 KB | | roads |
| `0x10179060` | 32 KB | | SwiffRender's fonts and languages |
| `0x1019fab0` | 31 KB | a / b | |
| `0x1001ddf0` | 28.5 KB | | the effect loader and its state cache |
| `0x10041b10` | 26 KB | | shader statistics |
| `0x1002ea10` | 26 KB | d 8 KB | the shader defines |
| `0x100106b0` | 26 KB | | display modes |
| `0x10092990` | 25 KB | | the rest of the overgrowth |
| `0x100c6570` | 25 KB | d 8 KB | the bundled mesh's shadow map |
| `0x10004930` | 24 KB | | screenshots, DxDiag |
| `0x100df010` | 24 KB | | |
| `0x100cd220` | 24 KB | | TerrainEditable |
| `0x100e6de0` | 23 KB | | the decal renderer |
| `0x1012b3f0` | 22 KB | | non-screen-aligned particles |
| `0x100f7b80` | 21 KB | d 16 KB | the sky dome |

AIDLL (≥ 20 KB):

| Run | Size | Makeup | What it is |
|---|---|---|---|
| `0x10044aa0` | 33 KB | c | containers, OvergrowthCollision.con |
| `0x1002ae90` | 23 KB | | the bot debug printers |

**Status (missing-runs pass after the seats pass):** the EXE's (d) runs of 1 KB or more (`uncited.py --cite cite_noaudit.json --min-run 1024`), biggest first, and the largest evidenced (c) functions; each implemented, cited where the rewrite already does it, or classified in uncited.md ("The missing-runs pass").

| Run | Size | Outcome |
|---|---|---|
| `0x004f5900`..`0x004f6496` (follow.* setters, in run `0x004f63d8`) | 2.9 KB | implemented: follow.followTime / targetActivation (`0x005ed1a0`, `0x005ec7c0`, `0x005b2c10`, `0x005b3e60`); changeYaw / changePitch / minDist cited as never read |
| `0x00500594`..`0x00500883` (TargetObject setters, run `0x00500594`) | 2.0 KB | implemented: disableWhenEmptyVehicle(Delay), disableWhenWreck (`0x0058eba0`, `0x0058e720`) |
| `0x004f64fb`..`0x004f6efe` (DestroyableObject / DestroyableWindowsBundle rows) | 1.6 KB | classified: no shipped script sets them |
| `0x0042ec60`..`0x0043031d`, `0x0042d9e0`..`0x0042df62` (the network event registry) | 7.0 KB | cited: `0x0042ed10`, its helpers `0x0042d9e0`..`0x0042de90`, `0x0042ec60` (GameProtocol.h's event ids are this registry's) |
| `0x0070cb00`..`0x0070e120` | 5.6 KB | classified: the Logitech G15 LCD applet's pages (0x0070cc50 page state, 0x0070d880 the "1/3" page writer through the LCD SDK 0x0071fc50) |
| `0x008ae4e8`..`0x008b00be` (RagDoll rows) | 6.9 KB | cited: the 19 row executes in RagDoll.cpp's `.con` reader (`0x008ae555`..`0x008afd68`); AnimationSystem.create / active / list (`0x008afd9a`..`0x008b007a`) are the editor's |
| `0x00420bdc`..`0x0042154a` (awards.setData) | 2.4 KB | cited: `0x00420be8`, `0x004211d2` in Stats.h (parseAwards reads awards.con itself) |
| `0x0066b175`..`0x0066cee1` (Console.get* reflection) | 6.7 KB | classified: dev (already in uncited.md (b), "Dev console methods") |
| `0x006ee282`..`0x006ef9bd`, `0x006ec29f`..`0x006ecf7a`, `0x006efbdb`..`0x006f0cb9`, `0x006f0d36`..`0x006f1b6f` (Object.* / ObjectTemplate.* / Player.* editor rows) | 16.4 KB | classified: editor (no shipped script calls them; isNotSaveable / saveInSeparateFile / createdInEditor are the editor's save flags) |
| `0x004196a4`..`0x0041aa21` (SquadManager.list*, renderer.screenMode / listScreenModes, pythonHost.sendCommand / reinitialize) | 5.0 KB | classified: dev console listings, no shipped caller |
| `0x00504be6`..`0x00505cf0` (template class registration) | 4.4 KB | cited: `0x00504be6` in ObjectTemplateManager.cpp (its type table) |
| (c) `0x004dc66d`, `0x004dce2e`, `0x004dd759`, `0x004dd315`, `0x004dc3c3`, `0x00424ff0`, `0x008902f0`, `0x0088eaa0` (enum argument readers) | 6.1 KB | cited where the rewrite reads the same names (Nametags.cpp, Weapons.cpp, Cameras.h, Bots.h, CombatAreas.cpp, Bots.cpp) |
| `0x00525958`..`0x00526270` (Camera sway), `0x004fd1c3`..`0x004fda4b` (regulateYaw), `0x00544efd` (changeLodAt), `0x004fc895` (soldierExit*), `0x005234b3` (regulateFromParent), `0x004ed6a5` (useShiftAnimation), `0x00520880` (isPortalPassing), `0x0055360d` (addVehicleIcon), `0x0052796e` (insideViewInterpolate), `0x004e4667` (fire.isBarrel), `0x0055264c` (podSpawnVehicle), `0x00552d4b` (tvInterference), `0x004e39f3` (dropWeaponAfterReloadDelay), `0x004fdcd6` (maxVertRegAngle) | 20.6 KB | classified: stored rows no shipped script sets (the default is what plays); the runs' few called rows left (uncited.md) |
| `0x0051ca27`..`0x0051cfaf` (anchor, 1,110 calls) | 1.4 KB | classified: editor (the roads' and statics' snap point) |
| `0x007c3af0`..`0x007c40b9` (the HUD ability bar) | 1.5 KB | left (d): AbilityBarShow / AbilityIconPath / AbilityBar / AbilityValue aren't set |
| `0x00500594` remainder (MagneticBait's overrideArmingSpeed / switchPlayerOnLock, PodTarget's activeOnCreate) | 0.7 KB | left (d): the booster's motion mine bait isn't a magnet target |
| `0x00676c10`..`0x00677255` (the settings save) | 1.5 KB | cited: `0x00676c10` in SettingsObjects.cpp (userSettingsScript); the rest its stream helpers |
| (c) `0x00753060` (the key names put into wide text) | 1.2 KB | cited in ControlMap.h (withKeyNames) |
| `0x006e8570`..`0x006ebdbc`, `0x006af640`..`0x006afead` (the object grid) | 14.2 KB | classified: replaced by design |
| `0x00683e40`..`0x00686e89` (IDKey / IDButton names) | 10.2 KB | classified: replaced by design (ControlMap.cpp composes them) |
| `0x0047f5e0`..`0x00483b50`, (c) `0x00483e30` (BFHQ stats, leaderboards) | 19.8 KB | classified: online-only |
| `0x0049eef0`, `0x0049e8b0`, `0x00479970`, `0x0046b900`, `0x00574550` | 6.1 KB | classified: online-only (logo and HTTP caches, account messages, downloads) |
| `0x00436530`..`0x00436dd8` (the LCD applet's start-up) | 2.2 KB | classified: G15 LCD |
| `0x006b3db1`, `0x006b2fc3`, `0x007636c8`, `0x008b0792`, `0x006f3d18`, `0x006f4558`, `0x00521440` (collisionManager, heightmapcluster, slider node, animation browser, Object / ObjectTemplate listings) | 9.8 KB | classified: editor / dev, no shipped caller |
| `0x0070b600` (Effects.con writer) | 1.8 KB | classified: editor |
| `0x0087bc00`, `0x0087a1c0`, `0x007b2f60`, `0x007b3750`, `0x00874cd0`, `0x007b2220`, `0x007dbd10`, `0x00870020` (dice::meme) | 12.1 KB | classified: replaced by design (src/Hud) |
| `0x004f45e4` (allowFriendlyFireDetonation), `0x00499430` (the host page's settings) | 3.0 KB | left (d) (uncited.md) |

- Implemented: `follow.followTime` and `targetActivation` (`followActive`, Weapons.cpp; the AA missiles stop turning 10 s after their lock, the mechs' rockets after 0.35 s; a lock taken mid-flight now waits trackingDelay) and the TargetObject switch (`GameWorld::targetEnabled`: HeatObject and LaserTarget_vehicle go off 5 s after their vehicle empties, the motion mines' MagneticObject reads its own 3 s, wrecks per disableWhenWreck). engine.md "Seeking projectiles".
- Measured with `cite.py --no-audit-docs` and `uncited.py --min-run 1024` (rows re-measured at the same probe) on 8575796 and on this tree:
  - EXE citations: 3,130 functions / 1,283,955 B (26.8%) → 3,194 / 1,310,132 B (27.3%) without engine.md's appendix; 4,030 / 1,388,567 B (29.0%) → 4,094 / 1,414,744 B (29.5%) with it.
  - EXE raw (d): 398 KB → 379 KB (evidenced 304,788 → 288,799 B, propagated 103,124 → 99,822 B); runs ≥ 1 KB 90 / 189,408 B → 84 / 166,059 B; calibrated (d) 0.62 → 0.60 MB; effective lower / calibrated / upper 52.5 / 83.1 / 88.9% → 53.0 / 83.6 / 89.5%.
  - Classifying in uncited.md moves none of these: the no-audit measurement leaves uncited.md out, so the ≈128 KB of (d) classified above still counts as (d) there.
- Tests: 768 with `BF2142_GAME_DIR`, all passing (this pass adds `Weapons_FollowTimeLimitsTheGuidance` and `GameWorld_TargetObjectOffWhenEmpty`); `ninja -C build/win32` builds. Bot rounds (`BF2142_BOT_SEED` 1–3 on Cerbere_Landing gpm_sp 16 and minsk_coop gpm_coop 32, 15 bots, 300 s) give the same kills, rounds and hits as 8575796's build tick for tick (Cerbere 18 / 20 / 14 kills, minsk 1 / 3 / 1), with the same CPU (6.1–6.4 s user on Cerbere_Landing, 9.3–10.1 s on minsk_coop, both builds side by side, twelve runs at once).

**Status (AIDLL missing-runs pass):** AIDLL's uncited runs of 512 B or more that the classifier calls (c), (d) or "?" (`uncited.py --cite cite_noaudit.json --min-run 512`; contiguous functions labelled anything but cited, (a) or (b)), biggest first: 267 runs / 353,516 B. Each run's functions were implemented, cited at the rewrite's code that does their job (comments in src/AI, src/Game/Bots*), or classified in uncited.md ("AIDLL: the missing-runs pass"). Outcomes in the table: *cited*; *(a)* STL, containers, accessors and plumbing; *(b)* debug drawing and printers, navmesh-generation tooling; *inert* (behaviour no shipped set or level can reach: planes, boats, broken bridges, the landing zones); *gap* (bot behaviour the rewrite lacks, decoded, not yet written: listed below); *not settled*; *impossible*.

| Run | Size | Outcome |
|---|---|---|
| `0x1002d800`..`0x10030493` | 11,296 | (b) debug: the bot and team printers ("** NO PLAN **", `0x1002e180`, `0x1002eff0`, `0x1002db50`) and the debug drawer's primitives |
| `0x10080590`..`0x1008275a` | 8,539 | inert: BBPFire3d's plan `0x100819b0` (the Plane set only) and BBCallForMedic (in no shipped set); cited: BBFireInfantery / BBRevive classes, the slot-11 gate `0x10082070` |
| `0x100c2ee0`..`0x100c4905` | 6,442 | cited: the squad bookkeeping (`0x100c4080`, `0x100c3f10`, `0x100c34d0`, `0x100c32b0`), the round's event record `0x100c4500`, the move interpreters; gap: the squad update `0x100c4240`, its radio `0x100c2ee0` / `0x100c30f0`, bounding and flanking `0x100c4010` / `0x100c3af0` / `0x100c3390` |
| `0x10053620`..`0x10054fe2` | 6,319 | (b) debug: IDPManager's text demo; the IAIStrategicEnvironment and pathfinding courier slots nothing reads |
| `0x100bac90`..`0x100bbfef` | 4,875 | (b) tooling: GTSData `.qti` and the GTS build; bridge obstacles (no broken bridges) |
| `0x100204b0`..`0x10021778` | 4,436 | cited: the `aiPathfinding.map.*` rows the rewrite reads (Bots.cpp); (b): minPolygonsInLeaf, maxDepthInTree, map.draw* |
| `0x1008fec0`..`0x10091049` | 4,421 | cited: Car / Tank interpreters at drive(); inert: the Plane interpreters |
| `0x100d4f60`..`0x100d5f96` | 3,938 | gap: the sensing agent's look-around (`0x100d50b0`..`0x100d5ca0`); cited the aim's `0x100d4f60` / `0x100d4fa0` |
| `0x100f4c70`..`0x100f5c57` | 3,879 | gap: the squad formation (`0x100f4c70`, squad `+0x7c`) |
| `0x10023e70`..`0x10024dfd` | 3,795 | (a): the behaviour-pattern factories and singleton fetches |
| `0x10055020`..`0x10055eff` | 3,459 | cited: the courier's map by name `0x10055020`; (a) plumbing and debug console helpers |
| `0x1008b9e0`..`0x1008c7b9` | 3,410 | cited: FloatingTank interpreters; gap (not decoded): the sensing patterns' look plans (Null / Soldier / MouseFreeLook / Turret) |
| `0x100c0130`..`0x100c0ee2` | 3,408 | cited: the member's waypoint goal `0x100c0130`, re-goal `0x100c0a90`; gap: the member update `0x100c0bb0`, the formation consumer `0x100c0890` |
| `0x100fad20`..`0x100fbac2` | 3,370 | (a): the root solver behind aim slot 0x44, which has no caller; cited the ray filter `0x100fba60` / `0x100fba80` |
| `0x10046080`..`0x10046e32` | 3,365 | (b) tooling: exportGTSMeshes; cited the Mersenne twister's draw `0x10046770` / `0x10046a50` (the xorshift stand-in) |
| `0x10043ac0`..`0x10044878` | 3,320 | cited: the A* open-list refill (NavMesh.cpp), createMap and the per-template map table; (a) resets; impossible: the path-cost slot 0x80 (no caller resolved) |
| `0x10084c20`..`0x10085915` | 3,271 | cited: BBPBridgeRepair and BBFlee's class (the bridge stand-in: 0) |
| `0x10035dc0`..`0x10036b41` | 3,244 | cited: the aiStrategicArea.* halves (setActive, addNeighbour, …; BotsScript.cpp); (a) containers |
| `0x10065dd0`..`0x10066a67` | 3,168 | cited: BBPAvoidCollision2d (never runs, documented), BBMoveTo's class; inert: AvoidCollision3d (in no shipped set) |
| `0x100f5f80`..`0x100f6c06` | 3,144 | gap: the squad formation's slots (`0x100f67d0`, `0x100f69a0`, `0x100f60a0`, `0x100f64a0`) |
| `0x100efc10`..`0x100f083f` | 3,046 | gap: the SAI's respawn (`0x100f0590`, the spawn-point chooser `0x100eff20`, kit and unlocks `0x100f04a0`) |
| `0x10067d70`..`0x1006897b` | 3,021 | cited: BBPGotoWaypoint2d; inert: GotoWaypointsBoat (no Boat set) |
| `0x100d64f0`..`0x100d70f7` | 3,021 | gap: the sensing agent's reset and sector weights (`0x100d6b90`, `0x100d6630`, `0x100d67b0`) |
| `0x100e0e60`..`0x100e1a39` | 3,009 | inert: AirTransportMoveTo (no AirTransport ships) |
| `0x100b8e90`..`0x100b9a38` | 2,917 | (b) tooling: bridge obstacles, GTS helpers; gap: the mine zones' flood fills `0x100b94d0` / `0x100b9200` |
| `0x100a0420`..`0x100a0f92` | 2,886 | gap: the SAI's loners split by aggression (`0x100a09c0`, `0x100a0420`) |
| `0x100f88d0`..`0x100f93d1` | 2,773 | cited: the information grid's queries (Bots.h) |
| `0x100b9d90`..`0x100ba84b` | 2,736 | (b) tooling: obstacle creation (bridges), GTS; `0x100b9d90` not settled |
| `0x1003c2d0`..`0x1003ce4b` | 2,701 | cited: the flee's mesh lookup `0x1003c890`, the A* tree helpers; (b): the polygon-normal drawer, the "NO ACTIVE MAPPROPERTIES" printer; implemented: the step ahead behind slot 0x90 (`0x1003cb90`) |
| `0x1005a5e0`..`0x1005aff7` | 2,569 | cited: createConstantCondition's parsers |
| `0x100c1de0`..`0x100c2821` | 2,553 | cited: taking an order (`0x100c1eb0`, `0x100c21b0`, `0x100c1fe0`), the squad's strength `0x100c26d0`; gap: the radio inside `0x100c21b0` |
| `0x100a3620`..`0x100a4088` | 2,532 | cited: the SAI's constructor `0x100a39c0` and destructor, the asset clocks |
| `0x10051d20`..`0x1005279a` | 2,514 | (a): AISettings' modifier storage (BotSettings holds the values), IDPManager map nodes |
| `0x100e77a0`..`0x100e8165` | 2,469 | inert: the Plane controllers |
| `0x100ab080`..`0x100abb8b` | 2,356 | (a): the AI objects' plug-in part wrappers (the rewrite reads AiPlugin directly) |
| `0x100cd170`..`0x100cdb0f` | 2,344 | cited: Parallell's list node and the combiners; (b) printers; not settled: the steering statement `0x100cd3b0` |
| `0x10056110`..`0x10056aeb` | 2,343 | gap, low: AIMain's bot population (slots 0x50 / 0x1c: maxBots, includeHumans; `--bots` stands in) |
| `0x100b0a00`..`0x100b1380` | 2,286 | (b) tooling: the `.cls` GTS reader and writer; cited `0x100b11f0`; (a) map iterators |
| `0x1009ee00`..`0x1009f6a5` | 2,175 | not settled: SAI step 1 `0x1009ee00` (per-side strength sums) |
| `0x1003d840`..`0x1003e0ff` | 2,166 | cited: the A* open list and visited hash, slot 0x88's ray `0x1003de90` |
| `0x100016d5`..`0x10002034` | 2,154 | (a): AISingletonServerUpdater's start-up and teardown, the console base slots |
| `0x100cae50`..`0x100cb6e1` | 2,095 | cited: the information environment's per-team grids (the documented stand-in) |
| `0x100f0860`..`0x100f10ef` | 2,009 | cited: the armament's weapon list; (a) forwarders and STL; (b) mesh-generation geometry |
| `0x10088f20`..`0x10089729` | 1,986 | cited: a player's squad, the nearest squad, joining (stand-in: the original fills the emptiest); not settled: order expiry `0x10088f20` |
| `0x100c5420`..`0x100c5ccf` | 1,984 | (a): STL and the interfaces' RTTI constructors |
| `0x10062b80`..`0x10063460` | 1,962 | cited: the strategic areas' constructors and destructors (Strategy.h); (a) accessors |
| `0x10038d50`..`0x10039512` | 1,961 | cited: the level-start pass `0x10038d50` (routeVia), addWayPoint `0x10039000` |
| `0x1005cfe0`..`0x1005d837` | 1,897 | (a): the area vtable's accessors |
| `0x100d0700`..`0x100d0e5c` | 1,876 | cited: MoveToObject's distance and MoveToObjectFinding's re-path |
| `0x1008a240`..`0x1008a9a0` | 1,852 | cited: the squad manager (`0x1008a720`, `0x1008a240`, update `0x1008a910`); not settled: the human tracking `0x1008a3b0` / `0x1008a690` |
| `0x10098200`..`0x1009895f` | 1,799 | not settled: bot slot 8 `0x10098200` (rebinding to a new controlled object) |
| `0x1006b1d0`..`0x1006b907` | 1,795 | gap: BBPIdle2d (`0x1006b2f0`): a driver answers a passenger's req_pickup with roger_that |
| `0x100ff690`..`0x100ffd93` | 1,779 | (a): the box frustum of the ITAir room test (no Plane ships), matrix helpers |
| `0x100a1a80`..`0x100a2199` | 1,777 | not settled: SAI step 4 `0x100a1a80` |
| `0x1004beb0`..`0x1004c635` | 1,769 | cited: IAIMineManager's class and grids (the mine stand-in) |
| `0x100da620`..`0x100dad53` | 1,737 | gap: the guided aim point (`0x100da6b0` / `0x100da710`); (b) printer |
| `0x10026300`..`0x100269ce` | 1,701 | cited: the clamp `0x10026930`, the clamped acos `0x10026960`; (a) the BB factory registration |
| `0x100d12d0`..`0x100d19b6` | 1,698 | cited: MoveToFinding, the 3d move, While |
| `0x10096ab0`..`0x10097364` | 1,664 | (a): the bot's accessor slots |
| `0x10026f60`..`0x1002762f` | 1,642 | cited: setVehicleDefaultBehaviour, addSAIStrategy, activateDefaultInterpreter; (b) the settings printer, the debug camera |
| `0x100ecca0`..`0x100ed2bd` | 1,514 | cited: the sensing memory's constructor and clear (Bots.h); (a) STL |
| `0x100cbd70`..`0x100cc34a` | 1,498 | (a): a 101-entry x^6 table with no reader |
| `0x10091550`..`0x10091b39` | 1,489 | cited: the Tank interpreters |
| `0x100d7190`..`0x100d77a7` | 1,489 | gap: the sensing agent's constructor and stimulus collection (`0x100d7210`, `0x100d7400`, `0x100d7590`) |
| `0x100d9130`..`0x100d973c` | 1,489 | cited: LookDeviation and the aim's accessors |
| `0x100304f0`..`0x10030b1b` | 1,476 | cited: the Mersenne twister (Bots.h, the xorshift stand-in); (b) the debug drawer's set-up |
| `0x10097470`..`0x10097a60` | 1,462 | cited: the bot's parts and input binding (Bots.h); not settled: slots `0x100977f0`..`0x10097a40` |
| `0x10059be0`..`0x1005a1b3` | 1,461 | cited: the CST, strength, Crisp / Fuzzy and side names |
| `0x1003b970`..`0x1003bfa8` | 1,457 | (a): the information factory and map-properties slots |
| `0x10057410`..`0x100579b8` | 1,429 | (a): AIMain's list bookkeeping |
| `0x100db580`..`0x100dbbca` | 1,424 | cited: the area-not-held test, ChangeWeapon, the wait, weapon ready; impossible: `0x100db5c0` / `0x100db5e0` / `0x100dba60` (fields unidentified) |
| `0x10061440`..`0x10061a15` | 1,423 | (a): the zone lookup (console), area accessors |
| `0x100c4e30`..`0x100c5403` | 1,416 | (a): STL |
| `0x10029e80`..`0x1002a3f4` | 1,373 | cited: removeInterpreterEntry; (b) the path printer |
| `0x1009ce20`..`0x1009d3cb` | 1,345 | not settled: SAI slots and step 8 `0x1009cf90` |
| `0x1006fc90`..`0x100701fa` | 1,339 | cited: BBPChangeLandingCraft; (a) containers |
| `0x1002d1c0`..`0x1002d6e8` | 1,293 | (b) debug: the path drawer, the plan-name printer |
| `0x100d23a0`..`0x100d28fe` | 1,275 | cited: the Idle statement, the pattern base, the empty-seat test; gap: the 2d path look `0x100d2500`; inert: the boat look |
| `0x10100760`..`0x10100d68` | 1,272 | (b) debug: the PI* names for the statement printers |
| `0x1002c180`..`0x1002c6a3` | 1,266 | (a): the debug drawer's containers |
| `0x100ac300`..`0x100ac843` | 1,262 | (b) tooling: saveQuadTrees, the cluster-distance precompute; cited the island lookup; gap: the mine zones `0x100ac660` / `0x100ac690` |
| `0x10092400`..`0x10092914` | 1,257 | not settled: the path follower's step count and point cache |
| `0x1003a200`..`0x1003a720` | 1,247 | cited: the strategy manager's command halves; inert: the preceding-strategy commands (not rewritten) |
| `0x100be300`..`0x100be86a` | 1,242 | cited: the A* successor iterators; (a) the interfaces' RTTI bases |
| `0x10032e10`..`0x100332f5` | 1,235 | cited: the auto-controller template; inert: ai.spawnBot's placement spawner (not rewritten) |
| `0x1006d5d0`..`0x1006dabd` | 1,234 | cited: BBPChange; inert: the exit spot for planes `0x1006d840` |
| `0x1008cd70`..`0x1008d249` | 1,234 | cited: the Mech interpreters |
| `0x100511a0`..`0x10051691` | 1,227 | (a): AISettings slot 0x84's storage |
| `0x10058540`..`0x10058a3e` | 1,224 | (b): the material and strength-name printers, the AIMain destructor |
| `0x10035440`..`0x10035935` | 1,215 | inert: the broken-bridge list (no broken bridges) |
| `0x1005bb60`..`0x1005c032` | 1,214 | cited: Owned / Hostile / Neutral; inert: the landing zones' side names |
| `0x100fff20`..`0x101003d1` | 1,201 | (a): a 4x4 inverse for the run-time obstacles |
| `0x1003b400`..`0x1003b8f8` | 1,197 | cited: createStrategy, createPrerequisite; (a) the manager's construction |
| `0x100c1820`..`0x100c1cdc` | 1,195 | gap: the squad's member-in-area test, mode choice and danger list (`0x100c1820`, `0x100c1940`, `0x100c1b60`) |
| `0x1008e440`..`0x1008e939` | 1,176 | cited: InfoWrapper, SoldierPose, the turret look and aim; not settled: the Sense statement `0x1008e620` |
| `0x100344b0`..`0x10034a08` | 1,169 | cited: the best area for a point `0x100349b0`; (a) accessors |
| `0x100f4350`..`0x100f481f` | 1,169 | (b) tooling: the quad-tree build; gap: the formation's getters |
| `0x100c1210`..`0x100c16af` | 1,156 | cited: the order's area, reachability `0x100c1300`, the 8 tries within 30 m `0x100c14a0` |
| `0x1009b380`..`0x1009b8e1` | 1,154 | cited: the bot's destructor; (a) accessors |
| `0x100ef370`..`0x100ef84b` | 1,138 | cited: the sensing step `0x100ef370`, the target reset `0x100ef3e0`; gap: the SAI's death and spawn hooks (with run 21) |
| `0x10030f90`..`0x10031417` | 1,135 | implemented: the bots' spotting `0x10030fb0` (with `0x100a5850`); (a) vector code |
| `0x100aa920`..`0x100aae65` | 1,124 | (a): the Unit and Physical part wrappers |
| `0x100f7090`..`0x100f756f` | 1,117 | cited: the arbiter (Behaviour.h), the grid helpers; (a) RTTI |
| `0x100cf910`..`0x100cfe14` | 1,103 | cited: the horn statement `0x100cfbd0`; (a) the plan framework's helpers |
| `0x1002cad0`..`0x1002cf7e` | 1,101 | (a): the debug drawer's containers |
| `0x1003ee10`..`0x1003f2c2` | 1,096 | (b) debug: the trace-map printers; cited slot 0x30 `0x1003f000` |
| `0x100a9660`..`0x100a9b1c` | 1,089 | cited: the AI object (Heli.h); inert: the homogenous / heterogeneous conditions (no shipped script) |
| `0x100fe500`..`0x100fe963` | 1,083 | (b): the assert reporter, box math |
| `0x100259c0`..`0x10025e1f` | 1,078 | (a): the AIConsole name registry |
| `0x100841e0`..`0x10084611` | 1,055 | cited: the BridgeRepair helper and class |
| `0x10001000`..`0x100014aa` | 1,050 | (a): the singleton's slots; the menu's bot count and skill (main.cpp's stand-in) |
| `0x10063f90`..`0x100643b6` | 1,047 | cited: the strategic objects' base |
| `0x100d2b40`..`0x100d2f7e` | 1,032 | cited: the info-grid getter, If, NOT; (b) printers |
| `0x1004a4d0`..`0x1004a907` | 1,008 | (a): the mine grid's insert and remove |

The 153 runs under 1 KB (105 KB) were triaged the same way: mostly cited (the A* drivers, the aiStrategicArea / strategy / behaviour command halves, the botSetWaypoints_Patrol parsers, the aerial height map, the cluster search's hash, the comm object, the MoveTo statements, the target conditions, the sensing frustum) or (a) (vector `_Insert_n` instantiations, map inserts, accessors, the interfaces' RTTI constructors); the (b) ones are the landing-zone and placement parses, the camera commands and the GTS build; the gaps among them are the get-in plan's conditions and seated radio (`0x100d4190`, `0x100d4a90`), the pilot's weapon choice (`0x100dd4a0`), the pilot's take-off wait test (`0x100d1cb0`), Revive's crouch-then-fire (`0x100dda20`), the damage event (below, done) and the squads' manager radio slots (`0x100c5f00`, `0x100c60c0`).

- Implemented:
  - **The bots' spotting** (`0x100ed2c0` → `0x100a5850` → `0x10030fb0`, IAIGame slot 0x68 = BF2142.exe `0x00895ab0`): a memory item's first own sighting (`+0x68`), near a strategic area (30 m across, 100 m for aircraft), throttled per team and area for 60 s (an upgrade to HeavyArmour passes) and for aircraft by one stamp, is reported to the team (the human spotting's path) and its `radio.spottedMessage` said on TCTeam. `BotManager::spotSighting`; test `Bots_SpotSightingsNearAreas`.
  - **A hit tells the squad** (IAICollisionHandler slot 0x18, `0x1004f020`, from BF2142.exe `0x0089f8d0`): the attacker goes to the victim's squad's memory and a human driver's squad; a bot victim is made aware of the attacker. `GameWorld::takeAiHits`, `BotManager::hitBy`; test `Bots_HitSharedWithSquad`.
  - **The soldier's step ahead** (`0x10093150` → slot 0x90 `0x1003f8c0` → `0x1003cb90`: the mesh ray's climb test at 85° / 0.35 m half a radius ahead): the follower presses jump at a rise in its way while it moves. `PathFollower::steer`, `RayHit::climbStopped`; test `PathFollower_SoldierJumpsAStepAhead`.
- Gaps decoded and left (bot behaviour, in order of size): the squads' formation, bounding and flanking, danger list and order radio (`0x100c4240`, `0x100f4c70`..`0x100f6c06`, `0x100c0bb0`, `0x100c2ee0`, `0x100c30f0`, `0x100c21b0`, `0x100c1940`); the sensing agent's look-around (`0x100d7210`, `0x100d7840`, `0x100d60f0`, `0x100d6630`); the SAI's respawn choice of spawn point and kit (`0x100f0590`, `0x100eff20`, `0x100f04a0`); the SAI's loners by aggression (`0x100a09c0`); avoiding known mines on the mesh (cost class 7 through slot 0x5c, `0x1004ae90`, `0x100b94d0`, `0x100b9200`); BBPIdle2d's req_pickup answer (`0x1006b2f0`); the guided aim point (`0x100da6b0`, `0x100da710`, `0x100da380`); the 2d path look (`0x100d2500`); the small ones above. Not settled (need a read against Bots.cpp / Strategy.cpp): SAI steps 1, 4, 6, 8 (`0x1009ee00`, `0x100a1a80`, `0x1009c7c0`, `0x1009cf90`), the steering statement `0x100cd3b0`, bot slots 7–9 (`0x10098d10`, `0x10098200`, `0x1009b9e0`).
- Measured with `cite.py --no-audit-docs` and `uncited.py` on 8575796 (with the other passes' uncommitted work) and on this tree (AIDLL, without engine.md's appendix, which cites no AIDLL code): cited 1,099 functions / 391,276 B (38.9%) → 1,556 / 473,113 B (47.0%); raw (d) 49,501 → 37,139 B; (d) runs of 512 B or more 17 / 33,946 B → 12 / 24,828 B; calibrated (d) 0.26 → 0.22 MB; effective lower / calibrated 46.7 / 69.7% → 55.5 / 75.0%; uncited (c)/(d)/"?" runs of 512 B or more 267 / 353,516 B → 211 / 248,480 B. Classifying in uncited.md moves none of these (the no-audit run leaves it out).
- Tests: 771 with `BF2142_GAME_DIR`, all passing (this pass adds three); `ninja -C build/win32` builds. Bot rounds, `BF2142_BOT_SEED` 1–3, 300 s, kills (rounds fired) before → after, run side by side: Cerbere_Landing gpm_sp 16: 18 (1,552), 20 (1,230), 14 (2,688) → 15 (1,166), 11 (1,592), 16 (1,752); minsk_coop gpm_coop 32: 1 (82), 3 (1,435), 1 (327) → 6 (187), 6 (568), 4 (281). CPU the same: 4.6–5.0 → 4.6–4.8 s user on Cerbere_Landing, 7.8–8.6 → 7.5–7.9 s on minsk_coop.

## 3. Remaining hedges a further pass could settle

`hedges.py` finds 216 hits at HEAD and 214 at fec6f08.
- **Settled since fec6f08** (19 hits by text):
  - maxLightsPerTerrainPatch's reader;
  - the overgrowth alpha doubling `0x1000b500`;
  - projector textures and per-light shadows;
  - the vehicle plan's Mobile branch;
  - the target's speed (sprint stamina);
  - the guided plan on foot;
  - the seated look;
  - the HUD message 0x52;
  - pods against a shielded Titan;
  - the ladder's animation and camera;
  - the shipped destroyable props;
  - the vehicles' stamina and torque;
  - cullRadiusScale's fade.
- **New** (21): the distance fade's remainder, the voice natives' stand-ins, the guided plan's stand-ins, the SAI area constructor (engine.md:11273), BBChangeLandingCraft's adaptor slots (engine.md:11030), Strategy.h:98, HudMenusVehicles.h:371, LevelRenderer.h:561 and others.

Line numbers below are HEAD's.

**Weapons, projectiles, physics**
- engine.md:962, MeshCollision.cpp:63: the response's stuck push (`+0x11c`).
- engine.md:794: the wing's liftRegulated / regulateToLift.
- engine.md:7406, GameWorld.cpp:792: addExtraAmmoItem's flag.
- engine.md:7960, GameSounds.cpp:500: the BoltClick flag `+0x41`'s writer.
- engine.md:7884: the detonation's slot (`0xf8`).
- Weapons.h:686: the deviation's slot 0x1a0.
- GameWorld.cpp:4926: the ground's material.
- GameWorld.cpp:7839: the stand-in for `+0x1f8`.
- engine.md:1079, 6682; GameWorld.cpp:9544; HudMenusVehicles.h:371: the active weapon's `+0x14` bit 0.
- engine.md:733, GameWorld.cpp:10480: the sentry's message 1 sender (`0x0063ce30`).

**Spawning, game logic, kits**
- GameWorld.h:19: activation by events.
- GameWorld.cpp:4039: the extra test some callers ask for.
- GameWorld.cpp:5002: the hull kill.
- GameWorld.cpp:5568: where the engine keeps it.
- engine.md:1982, GameWorld.cpp:9291: rememberExcessInput.
- engine.md:8128, Radio.cpp:95: the medic flag's setter.
- engine.md:8562: the kits' supply sound start and stop.
- engine.md:5310: the kit list's filler.
- engine.md:12092: the kit-screen setter.
- engine.md:1755: the IK disable flag and the weapon layers.
- engine.md:8554: the parachute unlock (case 0x0d).
- engine.md:7974: a deployable placed on another object.
- engine.md:1434: whether a passenger's own weapon fires.

**HUD**
- engine.md:6340: the `.dif` layout ("inferred").
- engine.md:6360, Hud.h:32, Hud.cpp:49: the corner roots ("approximate").
- engine.md:6431: the scoreboard's black rows.
- engine.md:6571: the range finder.
- engine.md:6886: the name tags' "Guesses".
- engine.md:8199: the name's suffix.
- engine.md:9316, 9575, 9683; HudMenus.cpp:42, 409: the list columns ("fitted").
- engine.md:9502: the order icons.
- HudMenusVehicles.h:541.
- AnimationSystem.h:318: LookAroundTrigger's argument.
- main.cpp:536: the mute icon.
- engine.md:12155–12156: the `+0x3c4` drawing.

**Network**
- engine.md:4147, GameProtocol.h:458, HostedGame.cpp:2329: CreateSpawnGroup.
- Networkables.h:221, 235, 258, 355, 369; Networkables.cpp:1146.
- RemoteWorld.h:165: the kick reason.
- engine.md:4360: the server code that sets it.
- engine.md:4962: the whisper's recipient.
- engine.md:5242: rcon_feedback.
- engine.md:5469: the server mask's bit 0x94.
- engine.md:5717: the game's own GameSpy keys from 0x35.
- HostedGame.cpp:159: the move buffer's depth.
- HostedGame.cpp:600: the CD-key reason.
- engine.md:5144, GhostPriority.h:78, HostedGame.cpp:2596: the dirty masks.

**Rendering**
- engine.md:1296: the Particle blend.
- engine.md:1720: the time override.
- engine.md:2228: the detail lists.
- engine.md:2706: the rain's two speeds.
- engine.md:3144–3148: the fade's children and its 0x1000000 flag.
- engine.md:3308–3310: the zoomed-mode `+8` reader.
- engine.md:3723: the tint.
- engine.md:12198: rotateAsAnimatedUV.
- engine.md:12220: start age and the sound's intensity.
- engine.md:2594, 2616: decals.
- Effects.h:234, 239, 1192.
- LevelRenderer.h:561.
- engine.md:3379 is stale (section 4, item 2).

**AI**
- engine.md:10310, Bots.cpp:1612: the grenade warning's sender.
- engine.md:11373–11392, Bots.cpp:2886–2897: the attention stamp, C4 on a moving target, `+0x248`.
- engine.md:11428: the guided plan's order spot.
- engine.md:10413, Bots.cpp:2840: the type-0xe fire plan.
- Bots.cpp:4110: the `+0x14` that wasn't found.
- Bots.cpp:3151, 3481, 4321, 917.
- PathFollower.cpp:1229.
- Strategy.h:57, 98.
- engine.md:11273: the area's constructor.
- engine.md:11030: BBChangeLandingCraft's adaptor slots.

**Not settleable by decompilation** (listed so a pass skips them):
- PathFollower.h:37 (real-time budgets);
- HudMenus.h:36, HostedGame.cpp:1217, engine.md:9314 (no human on the other end);
- Bots.h:1162, engine.md:10118 (test defaults);
- architecture: LevelRenderer.h:4, Console.h:76, ClipMethods.cpp:3, VoipNatives.h:3, main.cpp:204, 3210, OpenSpyEmulator.h:1, Sound.cpp:17, Ambience.h:18, engine.md:6278, 6281;
- algorithm terms: Ballistics.cpp:132, 186, engine.md:11041, 11056;
- fitted to the terrain by design: LevelRenderer.cpp:1372, OvergrowthLod.h:7, OvergrowthSettings.h:26, engine.md:2319;
- engine.md:3460 (a design choice);
- diagnostics: RemoteWorld.cpp:588, 611, NetDiagnostics.cpp:195, main.cpp:2209;
- error strings the pattern catches: ServerAdmin.cpp:391, 406, 440; ServerSettings.h:130; TlsSocket.cpp:116; ISoundProvider.h:20; Reverb.cpp:58; main.cpp:2604, 2610, 8136; engine.md:5204–5214.

## 4. Inconsistencies

1. **A test depends on the game data without saying so.**
   - `Commander_OrdersAndBots` (CommanderTest.cpp:202–237) needs `BF2142_GAME_DIR`. Without it, the check at line 237 (the squad member walked more than 5 m) fails every run, at fec6f08 and at HEAD.
   - The file's `Field` keeps a `loaded` flag that this test doesn't check, so it fails instead of skipping.
   - With `BF2142_GAME_DIR` set, HEAD passes 738 / 738. fec6f08 fails `Voip_LoopbackSession` and `Voip_RefusedGameServers` (fixed in c9486a4).
2. **engine.md:3374–3379 is stale.** It says "Not drawn: the animated UV matrices (the exe's per-part UV transforms, the tracks', aren't traced)". Since d9a49fe, src/Render/AnimatedUV.h and engine.md:12194–12198 have them traced and drawn.
3. **useChildArmor is documented as done but not read.**
   - engine.md:6507–6508 and HudMenusVehicles.h:69 say VehicleArmor takes the child's armour with `vehicleHud.useChildArmor`.
   - Nothing in src reads the row: the 12 AA and ground-cannon hubs set it.
   - Either implement it (`0x005c6460`) or move it to "not done" (gap L8).
4. **roadmap.md row 8:**
   - "The spotted message's crosshair target isn't rewritten" is stale: `GameWorld::spotCrosshair`, Radio.cpp:451, since 9caf973. What is missing is objects and vehicles (gap L3).
   - "The remaining gaps are coverage-5.md §2a": every row of that table is marked done in its status blocks. The pointer should move to this report's §2.
5. **coverage-5.md reads as open where it is done.**
   - Its §2 tables and Top 15 keep the gaps as first written. The completions are in the later status blocks.
   - Its §3 hedge list carries 4912c02's line numbers and still lists settled items: engine.md:2349 maxLightsPerTerrainPatch, 2242–2243 `0x1000b500`, 1125–1128 the Mobile branch.
6. **The rows tool missed composed keys** (fixed in this audit; section "Script fixes"): 14 rows / 270 calls. The `kInputs` table (Effects.cpp:2216–2226) is still missed: 3 names, 8 calls.
7. **The uncited label files were overwritten** by the headings and no-audit runs (fixed). Anyone who read `labels_with_*` after a full `run_all.sh` got the no-audit labels.
8. **Features since fec6f08 with no direct test:**
   - the terrain's point-light passes: `LevelRenderer::applyTerrainLights`, LevelRenderer.cpp:2654, terrain.frag. `RenderScene_MeshLightsPickedPerMesh` checks the undergrowth's pick only;
   - a joined client's combat-area seconds: `RemotePlayer::outsideSeconds`, RemoteWorld.cpp:649. `CombatArea_OutsideTimerThenDamage` is local.

   Most other passes added a test (coverage-5's status lists them).
9. **"The mech's foot animation."** engine.md:6811 lists it as not done under the vehicle HUD. coverage-5's rendering status says the walkers' feet need nothing more and calls the line "the HUD's". What HUD element it means isn't said anywhere.
10. **No "done in the docs, stubbed in the code" regressions.** No row went from impl to stub or from read to unread since fec6f08. The four commits bound no console method: the probe and the stub counts are identical, and all the gains are template reads and internal code.
11. **Measurement artefacts:**
    - The level runs print only the top 25 stubs.
    - The start-up count (507 / 125) includes the 158 host-only methods.
    - AIDLL's own console (282 commands) is in neither console table.
    - The classifier's propagated (d) makes container code in rewritten modules look missing (section 2g, rows 4, 5 and 9).

## Raw outputs

Outside the repo, in the session scratchpad: `c6head/` (2041898), `c6before/` (fec6f08) and `c6check/` (the full rerun of HEAD with `--before`). Each holds:
- `probe_*` and `calls.json`;
- `rows.json`, `rows_summary.json`, `tableA.md`, `stubs_called.md`, `unread_called.md`, `unread.json` and `delta.md` (`c6check`);
- `runs/`, `runs.json`, `runs_table.md`;
- `cite.json`, `cite_tests.json`, `cite_headings.json`, `cite_noaudit.json`;
- `uncited_{with,without,headings,noaudit,noaudit_recal}.json` and `labels_*`;
- `hedges.txt`, `hedges.json`.
