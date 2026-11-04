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

### 2e. AI

| # | Gap | Evidence | Effort |
|---|---|---|---|
| A1 | **Bots don't zoom.** Bot snipers and scoped riflemen aim unzoomed. | engine.md:11348 | S–M |
| A2 | **Sighting and vehicle ammunition:** every sighting is clear (no see-through materials); bots' vehicle guns have no magazines. | engine.md:11385–11387 | S–M |
| A3 | **Fire-plan stand-ins:** C4's guard on a moving target (the bot object's `+0x80`), the attention stamp (`0x100eb7f0`), the guided plan's order spot (`0x100db550` / `0x100db3b0`). | engine.md:11373–11392, 11428; Bots.cpp:2886–2897 | S each |
| A4 | **Bot leftovers:** ITAir vehicles, the terrain-normal "upright" test, seat changes and pick-ups, the grenade warning's sender. | Bots.cpp:3481, 4321, 917, 1612; engine.md:10310 | S each |
| A5 | **AI hint stubs the scripts call:** `GameLogic.defaultTimeToNextAIWave` (11 a run: the co-op maps' AI waves), `ObjectTemplate.setCheckObstruction` (33 a run), `weaponTemplate.shouldRearm` / `fireRate` / `isAntiAirWeapon`, `AITemplatePlugIn.setUseNoPathfindingToGetToObject` / `vehicleNumber`, `Object.notInAI`. | `stubs_called.md`; constructors `0x004283b6`, `0x004e21f5`, `0x00887dca`, `0x00884df1`, `0x006ed6e6` | S |
| A6 | **Scripted-bot (SAI) commands.** Only mods use them. | engine.md:11286–11317 | S |

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
