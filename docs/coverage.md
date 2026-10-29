# BF2142 rewrite: coverage audit (2026-10-28, at commit 8948ed8)

## How this was measured

- **Console API probe.** I built a copy of both console tables in which every row is a method taking 0 to 13 arguments, passed it with `--api`, and ran Camp_Gibraltar gpm_cq 32 with `--simulate 1`. I also passed one `--console "<object.method> x x x x x x x x x x x"` per method (2,688 distinct names). A real method rejects 11 arguments ("Too many arguments", "Properties only takes one argument", or "not active/unknown" for inactive template methods). A stub accepts them and prints nothing. Result: 469 names are implemented and 2,219 fall through to `ConsoleMethod::stub`. With the normal tables that is 782 EXE stubs and 333 renderer stubs, plus the ObjectTemplate properties and setters that `registerUntypedTemplateProperties` keeps as untyped settings. For each untyped setting I checked whether its name appears as a string literal in src (stored-read) or not (stored-unread).
- **Script calls.** I parsed every `.con/.tweak/.inc/.ai` in the mod's archives and loose files: the common archives plus the 22 official levels and their coop variants. That is 6,693 files and 791k calls.
- **Runtime runs.** I ran `bf2142 --nosound --verbose +level L +gameMode M +mapSize S --simulate 5` on 11 combinations (Minsk ti 48, Cerbere cq 32, Suez cq 64, Highway Tampa sl 64, sidi_power_plant_coop coop 32, Verdun sp 16, Wake Island ca 64, Belgrade nv 32, Molokai ti 48, Operation Shingle sl 32, Camp Gibraltar cq 32).
  - Every run reports **9,238 to 10,168 calls to 157 to 177 unimplemented methods**. That report counts only stubs, not unread untyped settings.
  - About 6,700 "Unknown object or method!" errors per run are names the engine never registers (`ObjectTemplate.creator`, `modifiedByUser`, `game.setToolTip`...), so retail hits them too.
  - Both gpm_sl runs raise a Python traceback (see gap 6).
- **Address citations.** I collected every function address cited in src/ and docs/ and matched it to each module's `functions.csv`. Long runs of uncited functions, named by the strings their decompiled code uses, are listed in Table F.
- **Python.** I checked every `host.*` call and native object method used by `python/bf2`, `mods/bf2142/python` and `Admin/` against `src/Python/ScriptHost.cpp`, `hostobjects.py` and `src/Game/ScriptBridge.cpp`.

Effort scale: **S** = days, **M** = 1 to 3 weeks, **L** = a month or more.

Caveats:
- `RagDoll.*`, `Vars.set`, `undergrowth.load` and the `Overgrowth*` tables are stubs in the console but are read by their own parsers. They are marked * in Table B.
- A method that is "impl" may still be partial: the probe proves registration, not fidelity.
- The probe ran in `--simulate` mode. A spot check found no console methods that only register in `--view` or `--menu` mode.

## Ranked gaps (most visible to a player first)

| # | Gap | What a player notices | Evidence | Effort |
|---|---|---|---|---|
| 1 | **Gameplay sound: weapons, vehicles, engines, ambient, voice-over** | The battlefield is nearly silent. No gunfire, engine, reload or explosion sounds from the objects that make them. No radio or commander voice-over, help voices or automatic "enemy spotted" lines. | `main.cpp:3819-3834` plays only three sources: `GameWorld::takeSounds` (filled at 2 sites, ragdoll impacts and animation footsteps), the effect bundles' `S_<bundle>_Start` sounds and the player camera's. `SoundManager` builds the DefaultSoundComp weapon sounds (`weaponSounds`, 0x005db760) and engine templates but nothing triggers them. Nothing updates the listener from the camera. Stubs: `GameLogic.messages.addRadioVoice` (532 calls), `.addMessage` (262), `.addHelpMessage` (147), `.addEmoteTrigger` (34), `.addRadioFilter`, `.setVolumes`; `Sound.addTrigger` (51); `ObjectTemplate.sound.addReloadTimeSound` (40, stub); `sound.noisy` and `sound.muteOnFire` are stored but never read; `Material.triggerRadio`, `MaterialManager.setRadioMessage`. The data is there: `Sound_server.zip` has VoiceMessages*.con and Sound_client.zip has VO/. The roadmap row 2 still says "sounds aren't yet triggered by gameplay". **Status (after 5a1905f):** mostly done, see engine.md "Gameplay sound": the listener follows the camera (1P/3P/vehicle, 0x007f9d50) with the indoor level (0x007fa100); weapons fire/reload/deploy 1P and 3P (DefaultSoundComp 0x005dbdc0 / 0x005dbfe0 / 0x005dc1d0, `GameWorld::takeWeaponSounds`), `sound.muteOnFire` ducking (0x007f9af0 / 0x007f9f50), engines' idle/rpm loops (0x007fe050), projectile loops, effect-bundle start sounds (explosions, impacts) and footsteps now actually play (they were looked up as game sounds and never found), `Sound.addSound` creates its template, `Sound.addTrigger` ambients, GameSounds.con / AudioTweak.con at level load; `GameLogic.messages.addMessage/addRadioVoice/addRadioFilter/addHelpMessage/addEmoteTrigger/setVolumes` (0x004d8af0..0x004d8e40) and the radio VO (0x0042e0c0, 0x004d8110, 0x004d7950), help voices (0x004d8280), `sound.addReloadTimeSound` (silo countdown). Left: occlusion, new-style engine audio, `sound.noisy` (its reader not found), burst/multi-part samples, PCO ambients, `Material.triggerRadio` / `MaterialManager.setRadioMessage`, the HUD asking for help voices, a joined game's radio. | L |
| 2 | **Dynamic shadows** | No shadows from soldiers, vehicles, trees or Titans, and no 1P shadows. Only the baked lightmaps darken things. | `shadowManager.*`: 30 of 30 stubs. `LevelRenderer.h:7` says "no shadows yet". The `*Shadowed.fx` shaders (RaShaderLeafShadowed, TrunkSTMBaseShadowed...) are referenced nowhere. `undergrowth.dynamicShadowsEnable`, `GeometryTemplate.useRadiusForShadowDepth`, `renderer.*` and `VideoSettings.setDynamicShadowsQuality` are stubs. RendDX9 range 1012b190-1014b69c (124 KB, HemiShadow/Shimmer/Effect shadow strings) is uncited. **Status (after 5a1905f):** done in the rewrite's own form, see engine.md "Shadows, hemi maps, sky and post-production": all 30 `shadowManager.*` (the light manager's settings, defaults from its constructor 0x100aafe0, setters at vtable 0x101d42e0 slots 41-94: 0x1014a580 / 0x1014a5d0 / 0x1014a600 / 0x1014a840...), `VideoSettings.setDynamicShadowsQuality` (and the menu's DynamicShadowsQuality), `undergrowth.dynamicShadowsEnable`; two sun-aligned orthographic maps over a snapped region ahead of the camera (dynamic objects each frame, incl. the 1P soldier; statics only when it moves), received by terrain, roads, undergrowth, lightmapped meshes (dynamic map) and unlightmapped soldiers/vehicles/statics/1P models (both, RaCommon.fx's 4-tap compare). Left: RendDX9's LSPSM/TSM fit and per-object maps, `GeometryTemplate.useRadiusForShadowDepth` (Geometry's), alpha-tested casters only with `allowAlphaShadows` (off, as shipped), `lightmapIntensityBias`, `dropShadowClipHeightOffset`, the update factor. Cost about 0.2-0.4 ms a frame, the static map's redraw up to ~1 ms on the frames the region moves. **Since:** `shadowUpdatesEnabled` (LM +0x755, 0x1014a900: 0 keeps the maps, the pass 0x10127420), `GeometryTemplate.useRadiusForShadowDepth`/`Spatial` (0x100a7e60.., the heavy mechs: the caster's bound pos ± radius·√2, 0x1012b190 / 0x10123e70); settled unused by RendDX9: `shadowUpdateFactor` (+0x750) and `dropShadowClipHeightOffset` (+0x1d8) have no reader, `lightmapIntensityBias` only feeds LightManager_r3x0.fx's never-selected Combine. Left: the LSPSM/TSM fit and per-object maps. **Since (render round 3):** settled as left out: a view-fitted perspective map can't keep the static casters' map between region moves (its redraw ~1 ms a frame) and per-object maps add a pass per caster, both well over the current 0.2-0.4 ms (engine.md); the low lighting level drops bundled and skinned meshes' dynamic shadows (RaShaderBM/SM DeprecationList row 2). | L |
| 3 | **Post-production effects** | No bloom/glow and no EMP screen distortion and static. No TV-guided missile view, no thermoptic (active camo) shimmer on cloaked soldiers, no tinnitus blur, no damage or colour-grade effects. | `PostProduction_nv3x.fx` techniques Tinnitus, ColorTransform, CameraEffect, Soften, EMP, Contrast, ThermopticCamouflage, GlowMaterial, Glow, Fog and TVEffect are all unreferenced. `RaShaderBMActiveCammo.fx` and `RaShaderSMActiveCamo.fx` are unreferenced too. Stubs: `renderer.bloomSize/bloomFilterContrast/bloomWeightScale/bloomOverlayAmount/defaultColorTint/defaultSaturation/defaultContrast` (36 calls each, one per level), `renderer.emp*`, `renderer.glow*`, `renderer.camouflageOffset`, `renderer.drawPostProduction`. The PostProcess component's 4 properties are stored, none read. `vehicleHud.enablePostProcessingOnGuiIndex` and `weaponHud.enablePostProcessingOnGuiIndex` are unread. **Status (after 5a1905f):** mostly done (engine.md "Shadows, hemi maps, sky and post-production"; `src/Render/PostProduction.h`, `post.frag`, `bloom.frag`): the bloom chain (0x10049140: FSBM contrast, large bloom across, 5x5, down; Soften), the colour grade (0x10049900 / 0x1004a120 / 0x10049a40) with every `renderer.bloom*`/`default*`, CameraEffect (0x10049b90) on the zoomed vehicle sights that `weaponHud.enablePostProcessingOnGuiIndex` names, EMP (0x10048a30 table, 0x10049d70) from `GameWorld::empLevel` of the player's vehicle, `renderer.emp*`, Tinnitus (0x1004a2f0, four-frame history), the Titan core explosion's flash (event 0x14, 0x10049360), the cloaked soldiers' refraction (ActiveCamo mask + FSBMExtractCamouflage / CamouflageBlurPassThrough, `renderer.camouflageOffset`), TVEffect (0x10048f80; RendDX9 never calls it), `renderer.drawPostProduction`, `VideoSettings.setUseBloom`. Left: the glow materials (`renderer.glow*` stored; the glow list at `*(DAT_101fae6c + 0x38)` not decoded), the PostProcess component's `tv*` (only TVEffect reads them), the view tint colour (slot 0x7c) and `vehicleHud.enablePostProcessingOnGuiIndex`'s own effect; GameWorld additions needed: the soldier's EMP level and the tinnitus trigger (an explosion within radius × `sound-tinnitus-range`). Fog and ThermopticCamouflage are unused by RendDX9. Cost about 0.3-0.5 ms a frame at 1280x720. **Since:** the glow materials settled as dead code: `*(DAT_101fae6c+0x38)` is the RaShader manager's RSMTCamouflage queue (the cloak, already drawn), and the glow passes need PostProduction +0x21, set only by 0x10048d10, which (like Glow 0x10048e80) has no caller in the DLL; `renderer.glowEnabled`/`glowStrength`/`colorBrightness` are stored only, as in RendDX9. | M |
| 4 | **Missile-lock warnings, lock-on markers and help pop-ups** | No "missile locked" warning icon or tones in aircraft and vehicles. AA and gunship lock boxes are incomplete. No contextual help messages ("press E to enter"...). | WarningHud component (0xc4f6): `warningHud.setWarningType/Icon/FirstWarningSound/SecondWarningSound` are stubs (about 350 calls each), so none of its 1,400 calls reach code. `HudBuilder.setObjectMarkerNode*` stubs: Texture/TextureSize 48, LockTextNode 40, LockText 10, LockOnType 12, Weapon 12, Objects 18, RC 2. HelpHud (0xc4fa): 15 of 15 properties unread (395 calls); `GameLogic.messages.addHelpMessage` is a stub; GeneralSettings.setHelpPopups. AbilityHud's 3 sounds are unread. **Status (after 5a1905f):** mostly done, see engine.md "Warnings and help pop-ups" and "HUD nodes": `warningHud.*` kept per TargetType (0x0055a940, 0x00639a40) and the HUD's event 0x2f (0x007a2100) made each frame from the projectiles locked on the player's vehicle (WarningIconShow/Path, the first and second tones through the sound layer); all 9 `setObjectMarkerNode*` / `addObjectMarkerNodeLockTextNode` (Bf2ObjectMarkerNode 0x007e5f30, update 0x007e6880: textures per state, labels' distance variables and placement), fed with the enemy objects carrying a target object of the marker's type; HelpHud's help, low-armour and close-to keys (IngameHelpShow/String, 0x0074f2b0, the count down 0x007539c0, the help voice through `VoiceMessages::playHelp`), `GeneralSettings.setHelpPopups`; AbilityHud's sounds from the kits while a hub or a repair treats the player (`GameWorld::received`). Left: the lock-on weapons themselves (the 0x94c0 component: locking and locked boxes, TTHeat/TTLaser seekers), what sends event 0x2f in the exe, the help manager's 75 tutorial messages (0x00645d40), the alt and low-ammo help keys. | M |
| 5 | **Commander and squad-leader strategic map menus; HUD pointer/minimap config** | The commander's and squad leader's map menus are missing their transform-list rows (asset and order lists). The commander's mouse cursors per tool (artillery, UAV, supply, orbital strike, move Titan) are missing. Minimap paint and zoom options are ignored. There is no overhead commander camera. | `HudBuilder.addTransformListNode` (750 calls, in HudElementsCommanderMenu{Titans,Conquest}.con, HudElementsSquadLeaderMenu*.con and CommanderRadioTitans.con), `setTranformListNodePosVariable` (168) and `setTranformListNodeOffset` (41) are stubs. `HudManager`: 27 of 31 stubs (setMouseTexture* x7, setComm*, setSpotted*, setPointerSize...). `Minimap.*`: 12 of 12 stubs. `HudBuilder.addNodeMoveShowEffect` (84) and `addNodeBlendEffect` (47) are stubs: HUD slide-in and fade animations. 67 of 139 HudBuilder methods are stubs in total. Roadmap row 8 lists "an overhead camera for the commander" as to do. **Status (after 5a1905f):** mostly done, see engine.md "HUD nodes: transform lists, effects, markers and the map menus": the 3 transform-list methods (Bf2TransformListNode 0x007eba70: stacking at OrderMenuPosX/Y, kept on screen), the map's right click (0x00788c00, MinimapRMBMenuState by what is under it 0x00782d90, the menu states 0x14/0x15/0x16, an order row placed at the click), move/blend/blink show effects (0x007b1f40, 0x007b20c0, 0x007b1c00), and every HudBuilder method the shipped scripts call (43 more: mouse areas, alternate and show commands, mouse-over drops, picture borders, text outlines and truncation, bar borders, occupied nodes, the map settings); all 27 HudManager stubs (pointer textures per commander tool drawn over the map, pointer size, comm/spotted settings kept) and all 12 Minimap (zoom steps, kit/vehicle paint filters, blend and filter settings kept). Left: the renderer honouring `HudQuad::blend`, picture centre points, the paint filters' use on the map, the commander rose's lists; the overhead commander camera is a renderer view pass (the server's camera at the asset drawn into the map, MinimapZoomDisplay 2), not HUD-side. 24 HudBuilder methods remain stubs, none called by the shipped scripts (sliders, tiles, hovers, animation nodes...). **Since:** the overhead commander camera (the asset zoom sets the commander map's zoom 2, 0x007d7f90; at 2 the player's view takes the team's OverheadCamera, 0x00779680 / 0x005f8170: over the point at ground + `height` 260, fOV 0.3, straight down; the map fades out, MenuMapAlpha +0x7b8, and zooms to the view's ground width, 0x0077c4b0); the renderer draws the world pass from it with the fov over the commander map's rectangle (`setCommanderPos/Size`, now used) and the screen around it continuing (`render::OverheadView`). The renderer honours `HudQuad::blend` (UI pipelines per D3DBLEND pair), `setPictureNodeCenterPoint` pivots (0x007c67b0, about the screen's centre) and textureless pictures as fills (the commander screen's black borders). Left: the commander rose's lists, the paint filters on the map, the CommanderMiniMap node, the renderer value the exe sets during the overhead view (DAT_00a2020c slot 0x80). | M |
| 6 | **Assault Lines (gpm_sl) supply lines** | On Highway Tampa, Bridge at Remagen, Port Bavaria, Yellow Knife, Leipzig and Operation Shingle (sl), supply groups don't gate capture. The game mode script throws at each status change. | Both sl runs: `TypeError: 'int' object is not iterable` at gpm_sl.py:121 `hasSupply`, called from `updateSupplyLines`/`onGameStatusChanged`. `ScriptBridge::controlPointParam` returns a number for `supplyGroupsNeeded`, where the script expects a list of (group, optional) pairs. `getSupplyGroupId` is missing from `hostobjects._METHODS`. `host.sgl_getControlPointsInGroup` returns `[]` (gpm_sl.py:149,163). `ObjectTemplate.supplyGroupNeeded` (25 calls) is a stub; ControlPoint `supplyGroupId`/`supplyGroupNeeded` are unread. **Status (this round):** done. `ObjectTemplate.supplyGroupNeeded <team> <group> [optional]` (0x00503dec/0x00503f73) is kept per team; ControlPoint `supplyGroupId` (-1 default), `supplyValueNeededTeam1/2`, `supplyValueTeam1/2` (save 0x005bc260) are read at the round's start; `cp_getParam` (0x0056a010) serves `supplyGroupId`, `supplyValueNeeded`/`supplyValue` (by team) and `supplyGroupsNeeded` as (group, optional) pairs; `host.sgl_getControlPointsInGroup` (0x00568890) gives the group's points; `getSupplyGroupId` (not in BF2142.exe; only gpm_sl.py's unused helper) answers too. Highway_Tampa sl 64 runs 300 s without a traceback; ScriptHost_SupplyLinesGateCapture shows an unsupplied point refusing capture and a supplied one taken. | S |
| 7 | **Camera behaviour: vehicle interior cameras, before-spawn, death and spectator cameras** | Inside vehicles the camera lacks its static interior sun and damage shake/flicker. No before-spawn fly-over camera, no free or spectator camera, no external or nose cameras. | Camera class: 45 of 49 properties unread (1,922 calls; `insideStaticSun*`, `insideDamageThreshold/Frequency`, `worldFOV`...). `GameLogic.setBeforeSpawnCamera` (36 calls) is a stub. `FreeCam.*`: 3 of 3 stubs. `sv.allowFreeCam/ExternalViews/NoseCam/Spectators` are stubs. OverheadCamera `fOV` is unread. ToggleCameraComp has no code (Table G). The roadmap's to-do list includes "the external camera modes" and "cameras as the game has them". **Status (this round):** partly done. `GameLogic.setBeforeSpawnCamera` (0x004278ef) is kept and frames the view before the first spawn; the seat cameras' speed shake (`cameraShakeStartSpeed/MaxSpeed`, 0x00610240) and hit shake (0.6 s, 0x00610450) roll the view (render::Camera::roll); `worldFOV` for the chase view and `insideFOV` inside (0x00610150); the static interior sun's settings and damage flicker (0x006104d0..0x00610580) are served by `PlayerCamera::insideSun` (the renderer doesn't use them yet); a joined client takes `sv.allowExternalViews` / `sv.allowNoseCam` from the settings block (no chase view without the former). Left: the chase/nose/fly-by camera geometry (chaseDistance, chaseAngle, chaseOffset, followStiffness, nosePos, CVM*), the sway/bounce properties, FreeCam and the spectator camera, ToggleCameraComp, colorTransform/saturation, the hit shake's true measure (0x00610450's caller not traced). **Since:** the seat cameras' view modes (setMode 0x00612eb0, the toggle 0x004b9e10, the CVM* flags, sv.allowNoseCam / allowExternalViews) with the original's Nose (0x00610800), Chase / FrontChase (0x006108e0: chaseDistance, chaseAngle, chaseOffset, chaseParentIndex, followStiffness), FlyBy (0x00612bb0) and Trace (0x00610d20) geometry (`src/Game/Cameras`, `PlayerCamera::externalView`); the dead player's camera (vtable 0x0097a8b0): the death camera on the killed object turning away from the killer (0x00615970), following a teammate (0x00614d00), the free camera with sv.allowFreeCam (0x00615290, 0x00616110), `FreeCam.togglePlayer/toggleCamera`. Left: ExternTrace (its target never set), insideViewInterpolate (1 in all data), the sway/bounce properties, ToggleCameraComp, colorTransform/saturation, the hit shake's true measure, Trace's roll (the parent's up isn't rolled into the view), the spectator's HUD strings. | M |
| 8 | **Spawn-point options** | Spawning straight into a vehicle seat (Titan pods, APC/tank spawns with `setEnterOnSpawn`) doesn't happen. Human-only and AI-only spawn points, position offsets and scatter are ignored. | SpawnPoint: 11 of 12 properties unread. 11,074 calls, only 1,096 reach code (`setEnterOnSpawn`, `setAIEnterOnSpawn`, `setOnlyForHuman`, `setOnlyForAI`, `setSpawnPositionOffset`, `setScatterSpawnPositions`). **Status (this round):** done but the vehicles' spawn screen entry. SpawnPoint (dice.bf.SpawnPoint 0x948a, interface 0xc47d, 0x00635590): `setSpawnPositionOffset` (default 0/1.02/0) added to the point, `setOnlyForHuman/AI` (0x005b24f0), `setScatterSpawnPositions` (0x005b2200 from the point), the 1 m clear-spot test unless scattering or `setAllowSpawnCloseToVehicle` (the rewrite falls back to a taken point when all are taken), `setEnterOnSpawn` (5 m) / `setAIEnterOnSpawn` (15 m) seating the soldier in the nearest free seat (0x005a98e0), and mobile spawn points: a team vehicle carrying a SpawnPoint (APCs, AT and IFV spawn vehicles) via `GameWorld::setSpawnVehicle` / `spawnVehicles`. **Since:** the spawn screen lists the mobile spawn points: the team's spawn vehicles' icons on the map (0x0078e830: the spawn manager's groups with spawn_(Un)Selected at +0x9f0/+0xa00, the selected group +0xa48), moving with them, a click picks the vehicle (`setSpawnVehicle`). In a running round none shows yet: spawned vehicles stay team 0 (no `teamOnVehicle` on the shipped APC spawners) and entering one doesn't give it the occupant's team in GameWorld, so `spawnVehicles(team)` finds none. Left: that team change on entering, the point's +0x8c height test, spawnInPod / podSpawnVehicle / disablePodControls (the Titan pods have their own path). | S |
| 9 | **Effect detail: wheel dust, rotor/thruster downwash, wind, heat shimmer, particle LOD** | Vehicles kick up no dust and aircraft no downwash. Smoke ignores wind. No heat haze. Effects don't fade by LOD or pre-warm. | WheelEffectBundle: 30 of 30 properties unread; AirDraftEffectBundle: 33 of 33 unread. Particle systems (Sprite/NSA/Mesh/Trail/Emitter): about 35k script calls to unread properties (`LodDistance`, `windAffectionFactor`, `preWarmTime`, `rotationGraph`, `randomIntensity`, `waterSurfaceOffset`, `forceToWaterSurface`). EffectBundle: 38 of 42 unread. `ParticleSystemManager.*`: 20 of 20 stubs (shimmer, view distance, quality). `WindManager.*` (34 calls each) and the WindAffector component (7 properties) are unread. The emitter logic is "inferred, not yet decompiled" (roadmap 5). **Status (this round):** mostly done, see engine.md "Particle systems, wind and the quality switches": the emitters are RendDX9's (start `0x1013fb30`, update `0x101407c0`, emission `0x101403b0`, birth `0x1013fdc0`, motion `0x1013f0c0`, rotation `0x10131f80`): `preWarmTime`, `emitSpeedGraph`, `windAffectionFactor`, `forceToWaterSurface`/`waterSurfaceOffset`, `randomIntensity`, `randomRotationSpeed`, `rotationGraph`, `LodDistance`, `QualityLevel`, `emitPrio`, `particleShaderType` (heat shimmer), `lightMapIntensityOffset` read; the sprite template's defaults (`0x10138b40`). `ParticleSystemManager.*` 20/20 registered and read (view distance, LOD/fade starts, shader quality, quality level, the shimmer's). `windManager.*` 2/2 (the global wind on particles and the trees' leaves, RaShaderLeaf). WheelEffectBundle and AirDraftEffectBundle: the ground's material effect, the speed/speed-in-dof/altitude/engine modifiers (`0x0061bd20`, `0x0061ce20`, `0x0061d820`). Left: the WindAffector components (local winds), particle `collisionEnable`, `alphaCull`, `selfShadowIntensity`, `pivotOffset`, `drawOrder`, `nearCullingEnable`, `isStaticSystem`, EffectBundle's own `timeToLive`/`randomAgeFactor`/`speedFromPhysics`, the wheels' contact list (the rewrite probes under the bundle). **Since (render round 3):** the WindAffector winds (`0x0064b5f0`: Directional, Point, RadialDir; reach `0x007040d0`) from the objects' trees on particles and the trees' leaves, `collisionEnable` (+0xf0) against the terrain with collisionSpeedAlongNormal/Plane, `alphaCull` (+0xa0, `0x10130bd0`), `pivotOffset` (+0x1b0), `drawOrder` (by its name), the wheel bundles on `game::Vehicle`'s wheel contacts and FromEngine on the throttle. Left: `selfShadowIntensity` (+0xa4, no reader found), the system ground planes, the explosions' own affectors, `nearCullingEnable`, `isStaticSystem`, EffectBundle's `timeToLive`/`randomAgeFactor`/`speedFromPhysics`. | M |
| 10 | **Lighting detail: hemi-maps, light manager, sky, light masks** | Soldiers and vehicles get no hemispheric ambient (no darkening under roofs or inside Titans). Tree, effect and specular colours are flat. No cloud layers, no sun flare. Dynamic lights hit everything instead of their masks. | `HemiMapManager.*`: 5 of 5 stubs (`setBaseHemiMap` 36, `loadTitanHemiMap` 12); "hemi" occurs 0 times in src. `lightManager`: 21 of 27 stubs (tree/sky/effect colours, `sunSpecColor`, `hemiLerpBias`, 36 calls each). `skyDome`: 15 of 19 stubs (`cloudTexture`, `cloudTexture2`, `scrollDirection`, `flareTexture`, `flareDirection`...); "cloud" occurs 0 times in src. `Object.setLightSourceMask`: 16,415 calls, the most-called stub. `envmapManager.*`: 3 stubs. `GeometryTemplate.setMaterialReflectionScale`/`setSpecularStaticGloss` are stubs. **Status (after 5a1905f):** hemi maps, light manager and sky done; light masks and envmaps left. `HemiMapManager.*` 5/5 registered: the base hemi map lights every unlightmapped mesh (RaShaderBM/SM `calcGroundUVAndLerp`: ground colour to sky by normal and height, the map's alpha occluding the sun), `loadTitanHemiMap` stored only. `lightManager.*` 27/27 registered (`enableSun` used; tree, effect and specular colours stored for shaders not written yet). `skyDome.*` 19/19: one or two cloud layers (SkyDome.fx, 0x100fd2f0: scrolled, faded by `fadeCloudsDistances`, `cloudLerpFactors`), the sun flare towards `-flareDirection` (0x100fd900; depth-tested instead of the occlusion query). Left: `Object.setLightSourceMask`, `envmapManager.*`, `GeometryTemplate.setMaterialReflectionScale`/`setSpecularStaticGloss`, the Titan interiors' hemi maps, HeightOverTerrain and HemiMapConstantColor per object. **Since:** HeightOverTerrain and HemiMapConstantColor per object (0x10041100 +0x1e4/+0x1ec; BM 0x100c2b70, SM 0x10160e90: max(h − r, 0)/max(r, 2), terrain-only ray), the Titan interiors' hemi maps (0x100edf90, 0x100eda70, 0x100edce0, CPU-sampled 0x100ecb10) with the camera in a Titan; settled: `treeSkyColor` has no reader, `envmapManager.*` binds the nearest .emi cube and no retail level has an .emi (no script calls it). Left: specular (`staticSpecularColor`, `sunSpecColor`'s SpecLUT64), particle lighting (`effectSunColor`/`effectShadowColor`, FXCommon.fx calcParticleLighting: light factor and HemiShadowAltitude unsettled), `Object.setLightSourceMask`, `setMaterialReflectionScale`. **Since:** the specular term (mesh.frag: RaShaderSTM's pow(N·H, 32) × lightmap.g × gloss × staticSpecularColor, RaShaderBM's SpecLUT64 × gloss), particle lighting (settled: HemiShadowAltitude is setBaseHemiMap's height, `0x10131d70`; the light factor is `defaultEffectLightAffectionFactor`, the template's `+0x1fc` staying −1, `0x10138b40` / `0x10139800`), the seat camera's interior sun on the first-person pass. Left: `Object.setLightSourceMask` (settled inert), `setMaterialReflectionScale`, `setSpecularStaticGloss` (0.15 used), normal maps. **Since (render round 3):** normal maps of bundled ("_b.dds", tangent space, `0x101499b0`) and skinned meshes (tangent space with a "tangent" technique, else object space, `0x100d4160`), only at the top lighting level, and RaShaderSM's specular (pow 20 × the normal map's alpha or StaticGloss). Left: `setMaterialReflectionScale`, `setSpecularStaticGloss`, static meshes' detail/crack normal maps (ndetail, ncrack) and parallax. | M |
| 11 | **Weapon feel: gun sway, deviation details, IK, ability modifiers** | Sniper and scope sway is missing. Turning, moving and misc deviation isn't added. Hands don't IK to the weapon. Unlock abilities that change material damage or add extra ammo do nothing. | DefaultGunSwayComp (`sway.*`): 28 of 28 unread. `deviation.setTurnDev/setSpeedDev/setMiscDev`: stubs, 66 calls each. `ik.*`: 6 of 6 unread. `ability.setMaterialDamageMod` (672 calls) and `ability.addExtraAmmoItem` (224) are stubs; 0xc4b3 Ability: 1,147 calls, 24 reach code. `penetrate.allowSolid/LiquidPenetration` are unread. Fire: `fire.addFireRate` (fire-mode switching), `pullbackTime`, `maxProjectilesInWorld` and `useDistanceCounter` are unread. `weaponTemplate`: 10 of 22 stubs. Roadmap: "the crosshair's spread with soldier deviation", "the rewrite's hand-held firearms have no deviation model" (engine.md, Deviation). **Status (this round):** mostly done. SoldierDeviationComp (0x005de980: setTurnDev / setSpeedDev / setMiscDev kept by the console, devMod* by stance and zoom, the firing build-up) strays the hand-held rounds (0x005f5540) and widens the crosshair; DefaultGunSwayComp (0x005e0da0, 0x005e09a0/0x005e0ba0, smoothing 0x005dfc20, stability 0x005e0750) nudges the look while zoomed, the sprint key with the scope stabilizer steadying it; `ability.setMaterialDamageMod` (damage taken by projectile material) and `ability.addExtraAmmoItem` (magazines / rounds; its consumer not traced) from the unlocks; lock-on weapons (`target.targetSystem/maxDistance/lockAngle/lockDelay/loseLockDelay`) lock on (GameWorld::weaponLock) and the object markers show locking/locked. Left: `ik.*` (the hands' IK), the misc deviation's real condition (in the air, inferred), the lock's object update (the rule is from the settings), `fire.addFireRate`, `pullbackTime`, `maxProjectilesInWorld`, `useDistanceCounter`, penetration. **Since:** `ik.*`: the third-person hands on the seat's IK object (0x005ee910, 0x005efc00; the solve 0x008b6760 / Maya's ik2Bsolver 0x008c2160; `src/Animation/Ik`, `GameWorld::seatIkTargets`); the original has no first-person IK. Left: the IK disable flag (+0x2ea), the weapon layers 0x005ef250 switches. | M |
| 12 | **Automatic radio messages from objects** | No automatic "enemy vehicle spotted", "X destroyed" or "control point takeable" radio chatter. | Radio component (0xc4b7): 24 of 24 properties unread (835 calls: `radio.spottedMessage`, `friendlyDestroyedMessage`, `enemyTakeableMessage`...). Together with gap 1's VO stubs. **Status (after 5a1905f):** partly done: the VO plays (gap 1) and `GameAudio::autoRadio` sends the team-change (friendly/enemy/neutral) and destroyed (friendly/enemy) messages on TCServerTeamMessage; the original's sender of these wasn't found (the component's readers go through the template, not `0xc4b7`), so that trigger model is inferred from the names. spottedMessage (the HUD's `Radio.sendSpottedMessage` is a stub; the spotted rose sends its own `<kind>_spotted`), takeable, spawn, damage, repaired, use, fire, `closeToIndication*` and `delay` aren't sent. **Since:** the senders found (the Radio object component, vtable 0x00988f30 at object +0x7c): damage (0x00656c90), destroyed (0x00656d10), repaired (0x006571d0) and spawn (0x00657120) from the server on 0xd to the object's team; team changes played locally by the local player's team (0x00656e80); the weapons' fail and use messages said by the player on TCLocalTeam (0x005c2ee0). **Since:** takeable / not takeable from the control point's allowCaptureByTeam setter (0x005bcd00 → Radio slots 0x60/0x64, 0x00656fc0/0x006567b0; points needing supply groups, for the local player, radio.delay through timed event 2/0x52, whose handler is 0x00758cd0 in the HUD manager's 0x00756b60); the fire message (0x00656dd0) on every silo shot (the firearm's batch fire 0x005c3cb0 in fire state 4, no gate: fire.radioMessageDelay is the laser painter's, 0x005e1730). Left: a joined client's takeable radio (its CP ghost bits aren't applied), the silo "_N" suffix (SiloSyncEvent 0x00514030 → Radio slot 0x68), destroyedGameEvent (no reader); closeToIndication is the 0x0059a6c0 proximity scan (events 0x3e/0x3f), not radio. | S-M |
| 13 | **Dedicated-server configuration and admin** | A hosted server ignores ServerSettings.con, maplist.con and reservedSlots. No remote admin (rcon) and no kick/ban by console. The standard admin scripts (autobalance, TK punish) are inert. | `sv.*`: 76 of 76 stubs (69 calls from Settings/ServerSettings.con). `maplist.*`: 10/10, `admin.*`: 16/16, `reservedSlots.*`: 7/7, `GameServerSettings.*`: 45/45. Python `host.rcon_invoke` returns "" and `rcon_feedback` is a no-op (Admin/standard_admin calls them 5 times). `ss_getParam` keys `autoBalance`, `tkpEnabled`, `tkpNeeded`, `tkpDefault`, `ranked`, `globRank` and `globUnlocks` aren't handled. `--host` has its own command-line options instead. **Status:** done, see engine.md "Server settings, the map list and remote admin": all 76 `sv.*` (0x0041b003..0x004207a8; save order 0x00431880), 45 `GameServerSettings.*`, 10 `maplist.*` (MapList.cpp, block 5 with every map 0x00441410), 16 `admin.*` (0x00429bba..; listPlayers 0x004a86b0, bans 0x00505db0 / 0x00507e10 / 0x00507570, refusal 0x16), 7 `reservedSlots.*`, `Game.sayAll/sayTeam`; ServerSettings.con / maplist.con from `+config` / `+mapList` / the mod's Settings (0x00409950); `rcon_invoke` (0x004427b0), `rcon_feedback` (0x00442940, 6/0x13), the client's `rcon` as 6/0x12 raising `RemoteCommand`, `sgl_getOverlayDirectory` (0x00567730), every `ss_getParam` key (0x00568350); the install's `admin/default.py` TCP remote console and `standard_admin` (auto-balance and TK punish are these scripts', not engine-side) on Python 3 with a py2 socket/md5 layer; `bf2142 --host --dedicated` headless, rotating the map list (roundsPerMap, endDelay, timeLimit; admin.runNextLevel/restartMap/runLevel/nextLevel); `bf2142 --rcon`. Left: reserved slots admitting a player to a full server, the friendly-fire percentages, start/restart delays and voting settings (kept, not acted on), how the exe carries an rcon_feedback text longer than one message. **Since:** reserved slots admit a listed nick to a full server by kicking an unlisted player (0x004ae6a0: refusal 2, kick 0x23); the friendly-fire percentages scale teammates' damage (0x005b7290); a hosted round waits for sv.numPlayersNeededToStart and counts down sv.startDelay, and too few players for notEnoughPlayersRestartDelay end it (0x004c03b0); engine-side votes (`game::Votes`: map, kick, mutiny; 0x00590230 / 0x0058f470 / 0x0058fb90). Left: the ranked overrides, the victim's 0.9/1.1 damage factor, a kicked voter's round ban, the rcon_feedback length. | M |
| 14 | **Bink movies** | No EA/DICE logos, no intro, no Titan tutorial video. | 6 files in `mods/bf2142/Movies/*.bik`. `bf2Engine.playMovie` (6 calls) is a stub. "bink" occurs 0 times in src; there is no decoder (docs/binaries.md plans libavcodec). RendDX9 Video/VideoTarget classes are uncited (range 1011d605-1012764b). **Status:** done except the menu's video, see engine.md "Movies": `src/Movie` (optional, `BF2142_MOVIES`; FFmpeg's Bink demuxer and binkvideo / binkaudio_dct decoders, all 6 files decode); `bf2Engine.playMovie` (0x004082a0: no-op when dedicated, legal_na.bik for en-US/en-CA 0x00403230), the queue taken one at a time (0x00408680), Esc skipping a mode-0 movie (0x0040bd90), the start-up queue from Init.con before the menu, sound through SDL; `--movie` and an off-screen `--screenshot` of a frame. Without FFmpeg (the win32 cross build) the movies are skipped. **Since:** movies in menu clips (`ui::Player::setVideoOpener`, `movie::MenuVideo`): the Titan tips' `loadImg ?titan_tutorial.bik` plays in its clip, a `?` URL opening a movie (SwiffPlayer 0x100449b0's `?/...bik`), pictures by time; `GeneralSettings.setViewIntroMovie` has no reader (its getter 0x0049c560, slot 33 of 0x00927450, is never called; the SWF only declares the native), so it is stored and saved only. **Since:** the menu movie's sound (Bink plays it in RendDX9, 0x1011e0d0 `BinkOpen(path, 0)`; to the playback device as decoded, stopped with the clip) and the menu music (Mod.desc's `<music>`, 0x007fd030 / 0x007fd060, `Sound.playMenuMusic/stopMenuMusic`). Left: RendDX9's Video/VideoTarget classes themselves (not decompiled; the rewrite blits instead), the map preview movies (none shipped). | M |
| 15 | **Squad-aware Python and stats hooks** | Squad-based scoring and field unlocks given by script never fire. Round-end stats aren't sent through the script path. | `host.pmgr_getSquadLeader` → -1, `pmgr_getPlayersInSquad` → `[]` (bf2/PlayerManager.py:159,170; stats.py:959). `pers_getStatsKeyVal`, `pers_plrGiveFieldUnlock`, `pers_plrRequestStats` and `pers_plrRequestUnlocks` are no-ops. `gl_sendEndOfRoundData` is a no-op (endofround.py:161; the native end-of-round block exists separately). `pmgr_enableScoreEvents` is a no-op. **Status (this round):** done. `pmgr_getSquadLeader` / `pmgr_getPlayersInSquad` (0x0056a570, 0x0056adb0), `pmgr_enableScoreEvents` (0x0056ad00: PlayerScore raised on score changes, 0x0056ae60), `pers_plrGiveFieldUnlock` (0x00566850: counted per player), `pers_plrRequestStats/Unlocks` (0x00566930, 0x00567370: 0, no stats server), `pers_getStatsKeyVal` (0x00566b90: no persistence object), `gl_sendEndOfRoundData` (0x00568930: kept by GameLogic, a hosted game sends it as data block 8). `sgl_getOverlayDirectory` is "" (GSOverlayPath unset) and `gs_abortCurrentConnectionAttempt` a no-op, as offline; `rcon_*` are the admin layer's. SquadBarFilled is never raised (no squad bar). | S |
| 16 | **Combat area options, cold breath, misc game logic** | The out-of-bounds timer always uses defaults, and vehicles get no damage or timer outside. No cold breath on winter maps. | `CombatAreaManager.timeAllowedOutside` (100 calls), `addVehicleDamage` and `addVehicleTimer` are stubs. `GameLogic.useColdBreath` (27; **since:** registered and drawn, the soldier's `breathEffect`, `0x00588830`) and `radialLevelIndex` (30) are stubs, as are `supplyDropHeight`, `uavReloadTime`, `satelliteScanReloadTime` and `orbitalBombardmentHeight` (all in GameLogic: 27 of 41 stubs). **Status:** combat areas done (`CombatAreas`, `GameWorld::stepCombatAreas`): `timeAllowedOutside`, `damage`, `addVehicleTimer`, `addVehicleDamage`, the player timer and damage (0x004bbf60), the abandoned vehicles' decay (0x004bed00), the time sent to clients, the commander's warning; CombatArea create/addAreaPoint as 0x004cb230 / 0x004da580, `active`, `deleteActiveCombatArea`, `getActiveCombatAreaName`. Left: the gpm_ti shield kill, the HUD's text; cold breath is the render side's (`GameLogic.useColdBreath`), the other GameLogic stubs unchanged. **Since (render round 3):** the cold breath checks the kit's `disableBreathEffect` (+0x384) and the soldier in a vehicle (+0x264). | S |
| 17 | **Demo recording and playback (BF2 demos / BFTV)** | You can't record or watch demos. | `demo.*`: 8 of 8 stubs; DemoLibrary and AutoRecorder (BF2/Game/GameServer/AutoRecorder.cpp) have no counterpart; `GameServerSettings.setAutoRecord*` are stubs. Only the protocol's demo connection id 0x81 is noted (GameProtocol.h:446). **Status:** recording and playback done in the original's format (`src/Net/Demo`: zlib, the 0x70 header, timed records): `demo.recordDemo` / `demo.stopRecording` on a host (connection 0x81 with no player, 0x004afcc0 / 0x004b02a0), `bf2142 --demo <file>` plays it on a socketless client (`GameClient::playDemo`, 0x004b52e0 / 0x00516c80). **Since:** the demo connection's 13-bit ghost state lengths (the GhostManager's +0x2140 for kinds 1 and 2, 0x00582a50), sv.demoQuality's rates (0x0057b3f0: 4..20 records a second, 260..4096 bytes, 40..255 updates), no empty records; the AutoRecorder (0x00585860 at the round's start, 0x00585920 at its restart, auto_<date> names 0x00585620, the hook run by 0x005853b0); .bf2142cam keyframes (0x005f8d30 / 0x005f91f0, `net::CameraKeyframe`); demo.toggleDemoPauseOrPlay, restartDemo, shutdownDemo on `--demo`. Left: the demo camera itself (togglePlayerDemo, toggleCameraDemo, adjustDemoFov, playing camera files), message 9 (the download URL), render/timedemo modes. | L |
| 18 | **Online back-end beyond login: stats, leaderboards, buddies, server logos** | BFHQ online stats, leaderboards, buddy lists and community logos are missing. | EAOnlineManager, StatsManager, LeaderBoard, SearchForPlayer, ServerLogoManager and HTTPDownloader are uncited. The FESL SDK (BF2142.exe 0x00801fa0-0x00882ff1, about 450 KB) is cited only for the login, and GameSpy GP/presence (0x00724b20-0x0073744a, 72 KB) is uncited. Roadmap: "the stats server (its answers parse; no account to ask with)". This is partly blocked by which OpenSpy services exist. | L |
| 19 | **Vehicle-bot controls and AI template tuning** | Bot driving and aiming use the rewrite's own controllers, not each vehicle's tuned PID inputs. Strategic and battle strengths per kit and vehicle aren't used. | `AITemplatePlugIn`: 27 of 49 stubs (`driveThrottleControl`, `driveTurnControl`, `aimHorizontalControl`, `aimVerticalControl`, `lookHorizontalControl`, `lookVerticalControl`: 113-117 calls each; `setStrategicStrength` 234, `setBattleStrength`, `setIsAntiAircraft` 70). `AITemplate.degeneration/allowedTimeDiff/commonKnowledge` (117 each). `kitTemplate`: 7 of 7 stubs. engine.md "Not decoded, and the rewrite's stand-ins" lists the rest. **Status (this round):** the ControlInfo plug-in (0x00891d80; adaptor 0x009d6b98) read whole: the gains, the drive/aim/look inputs and masks, the camera-relative limits and offset, maxClimb/RollAngle; the aim controller (0x100e5820) takes the bot's control object's gains (the seat's, or SoldierCtrl's), looked up once per control object. The Unit plug-in's setBattleStrength / setStrategicStrength, kitTemplate.* (0x0088cc40), the Armament's setIsAntiAircraft and aiTemplate.degeneration / allowedTimeDiff / commonKnowledge are kept; the SAI's areas (0x1005f210) sum basicTemp (empty vehicles and seats too) and the manned units' battle strengths × the capability factor (0x100aa420), the required strength the enemies' strategic ones with the friendly share; Fire3d's anti-air test reads isAntiAircraft. Left: the knowledge's certainty (every unit known, the degeneration unused), the fire plan's ITAir clause, the plane controller (no shipped planes), cullDistance, the Mobile plug-in's hearing values. | M |
| 20 | **Object-template odds and ends** | Strategic objects (Titan consoles) lack their icons and names. LookAt objects don't track. Pods/parachutes ignore `isPod`. Antenna sway is partial. Help and vehicle HUD fields are missing. | StrategicObject component: 6 of 6 unread. LookAtObject: 2 of 2 unread. Parachute `isPod` is unread. vehicleHud (0xc4ea): 43 of 48 unread (`miniMapIcon`, `vehicleIconPos`, `typeIcon`, `useSelectionIcons`...), so the vehicle HUD icons fall back to defaults. weaponHud: 30 of 41 unread. nametag (0xc506): 16 of 26 unread. PlayerControlObject: 48 of 68 unread. GenericProjectile: 21 of 35 unread (tracers: `tracerTemplate`, `tracerSizeModifier`, `tracerConvergeDistance`; `activeDefenseEnabled`). `armor.addArmorEffectSpectacular` (133) is a stub. See Table E. **Since:** a vehicle takes its first occupant's team and its own back when empty (0x005c9500), so an entered neutral APC is a mobile spawn point of that team; idle EffectBundles in a template tree (eu_fav_EMP_trigger's e_EMP_Fav) are no longer drawn as parts of the object (`render::drawnWithObject`; a bundle's children exist once it starts, 0x0061a4e0). Left: the rest of this row (the bots' tuning is gap 19's line; the networking priorities are done, gap 24). Also settled: the friendly-fire victim factor (slot 0xec, 0x005c6af0: crew ability types 4 / 5 aboard give 0.9 / 1.1 on all damage; the kit list that fills it isn't found, so 1 in practice), a vote's choice (0 yes, 1 no, 2 neither, 0x00645950; early end on yes + no, 0x00645bb0), the time-outside byte ((u8)floor, 0x00592410), ExternTrace (its point +0x2dc is set only by the uncalled ICamera slot 0x3c, so never in retail). | M (spread out) |
| 21 | **Joystick and gamepad input** | Only keyboard and mouse. | `dice.hfe.io.InputDeviceManager.DX8` has no counterpart; "joystick" and "gamepad" occur 0 times in src/Input. **Status (this round):** done, see engine.md "Menus" (Joysticks and gamepads): `IDFGameController_0..7` (0x00683b30) as the DX8 game controller (0x006ab430 enumeration, 0x006ab320 ranges, 0x006aaef0 polling: raw × 0.001 × sensitivity, POV hats as four buttons each after the buttons; 0x006aadc0 offset ± axis, inverted; slots 15-20; no dead zone, as the original sets none), the control maps' axis and button mappings (0x006802c0, 0x006803f0, `addButtonsToAxisMapping`), the controls page's last input (0x006aadf0) and `CONTROLLER_…` names (0x00498750), SDL3 joysticks and gamepads with hot-plugging (`src/Input/SdlGameControllers`; gamepads in the Xbox 360 DirectInput layout), tested with virtual SDL devices; the per-map `ControlMap.setActive/invertMouse/mouseSensitivity/keyboardSensitivity/setYawFactor/setPitchFactor` (+0x4c..+0x6c, written by 0x00680f90). **Since:** the keys' ramp on axes (0x00680990: +0x40 / +0x44, 0.001 s each, the first key winning; keyboardSensitivity +0x54 is only stored, nothing reads it for the ramp), the primary-or-secondary rule (0x00680dc0), the POV-to-axis mapping kind 4 (0x006807f0, made by the map's slot 12 0x00682db0, which nothing in the shipped game reaches), joystick and gamepad axes and hats as vehicle pitch and roll (`PlayerInput.pitch/roll`, `SoldierInput::pitchAxis/rollAxis`, PIPitch/PIRoll in moves; yaw and throttle already came from them). Impossible: the Falcon (`IDFFalcon`, flag 0x400) is a Novint Falcon haptic device (0x0067f830 through bf2142haptics.dll / NovintHFX.dll, 0x0067f460), hardware with a proprietary Windows driver; its bindings read 0. Not in the original: gamepad navigation of the Flash menus. | S-M |
| 22 | **Renderer configuration and quality switches** | View distances, LOD and quality options from the video settings don't change rendering. | `StaticMeshRenderer.*` (10/10), `ObjectDrawer.*` (8/8; includes `isInsideTitanOverride`, `showAllTitanParts`), `DecalManager.*` (7/7: fade and time to live), `nametags.*` (12/12: `setTexture` 55, `createIcon` 45, `createBar` 5, cull and fade distances). 54 of 66 `renderer.*` methods are stubs. `GeometryTemplate`: 22 of 26 stubs (`compressVertexData`, `maxTextureRepeat`, `lightmapOverride` and `doNotGenerateLightmaps` are mostly load-time hints; `isTitanInterior`/`isTitanExterior`/`titanExteriorPartIndex` drive Titan interior culling). `VideoSettings.*`: 13/13 stubs; the menu keeps its own copy in MenuBindings. **Since:** `VideoSettings.*` are stored, saved to Video.con (0x00497560) and fed to the video page (SettingsObjects); applying the qualities stays with the renderer. **Since (renderer):** the levels are applied as RendDX9's switch (`0x10010040`, `0x100991b0`; `scene::QualitySettings`, Scene's `VideoSettings.set*Quality`): terrain (terrain LOD distance 64/80/100, undergrowth view distance 0.5/0.75/1), geometry (the mesh LOD factor 0.5/0.8/1), dynamic lighting (dynamic lights off at 0), effects (the particle manager's row: view distance, pools, fades, shader and quality level; object decals off below 2), the view distance scale; lighting, texture and filtering stored (renderer `+0x6b4`, the mip skips' texture classes unidentified). Left: `StaticMeshRenderer.*`, `ObjectDrawer.*`, `DecalManager.*`, most of `renderer.*`, maxSkip3pLods. **Since (render round 3):** the texture classes identified (`push n; call 0x10017240`: 1 menu, 3/5/8/9 terrain patch maps, 4 low detail, 7 detail layers, 10 undergrowth, 11 overgrowth and trees, 12/13/14 static/bundled/skinned meshes) and their mip skips (`0x10017710`), the lighting level +0x6b4 (the RaShader DeprecationLists: normal maps, hemi, shadows, static layers), `GeometryTemplate.maxSkip3pLods`/`maxSkipWreckLods` with renderer +0x910, the Overgrowth's viewDistanceScale, `undergrowth.simpleShaderEnable`. Left: renderer +0x693, the road and sky levels, +0x6a8..+0x6b0, the filtering, static meshes' LOD skips (+0xc8 unknown). | S-M |
| 23 | **Settings and profile console objects** | Hardly visible: the menu's natives (MenuBindings) cover the same ground. | `GeneralSettings` (41/41), `AudioSettings` (15/15), `LocalProfile` (16/16), `GlobalSettings` (4/4), `SettingsManager` (10/10; 10 calls), `SwiffHost` (11/11; `addCredit` 1,068: the credits roll; `addGameMode` 7; `setScroll*`), `MenuTeamManager` (3/3). **Status (this round):** done but SwiffHost and MenuTeamManager (`src/Main/SettingsObjects`, engine.md "Menus"): GeneralSettings (41 and the writer's three), AudioSettings (15 properties), VideoSettings (13), GlobalSettings (4), LocalProfile (16 and `save`), SettingsManager (10): stored, written as the writers do (Global.con 0x00495d50, Profile.con 0x0048d940, General.con 0x0049aad0 with the defaults of 0x0049bd20/0x0049bdf0, Audio.con 0x004961a0, Video.con 0x00497560, Usersettings.con) under `<config>/bf2142-recomp/Profiles/`, run at start-up, the options pages starting from and writing back to them; applied: volumes, bots, crosshair colour, help pop-ups, bloom and shadows (a system's own method still called, `ConsoleMethod::replaces`). **Since:** SwiffHost (the credits roll drawn in the `_isScroller` clip's place, 0x101774b0 / 0x10117910 / 0x10177290, `addGameMode` behind the end-of-round mode names, `addAward`, `loadImage`, `setLatestGameVersion`), MenuTeamManager (teams, kits, weapon stats kept; their readers have no caller in the shipped menus), several local profiles (LocalProfileManager: numbered folders, Default, create/select/delete/release as 0x004904d0 / 0x004903d0 / 0x00490ca0 / 0x0048f3a0, each with its own settings and Controls.con; offline accounts and each soldier's profile through the menu's natives). The original has no profile rename. Left: ServerSettings.con (the server pages' values stay in the menu), MenuBindings' profiles.txt still keeps the remembered logins and offline soldiers beside the profiles. | S |
| 24 | **Networking priorities** | Possibly bandwidth and priority differences against retail peers. | `NetworkableInfo.*`: 9 of 9 stubs (`createNewInfo` 14, `setPredictionMode` 14, `setBasePriority` 5). The rewrite uses its own ghost layouts from the templates. **Status (this round):** done as BF2142.exe's GhostManager has it (0x00581e80, 0x0057dc10, 0x005823c0, 0x00582cd0, 0x005830e0): NetworkableInfo.* kept (base priority, c_NIGhostAlways = −1, prediction mode, unique, forced id); per client, scoping within max(GSViewDistance, 500) with the score 0x0057d300 × the base priority, teammates at 0.03, always ghosts at 255, new ones at 10000; the accumulating max-heap, 40 entries a packet, skip-and-go-on until 24 bits; kind 3 removals (the client hides and brings back); a packet at the connection type's rate and bytes (0x004a6160), the ghosts with what the input and events leave; losses dirty again at 10000. Left: the commander's scan and UAV scopes, the enclosing-object rules, the eye of player vt+0xd4, predictionMode's reader (not in the ghost manager). | S |
| 25 | **Editor and debug only (no player impact)** | Nothing. | `LightmapGeneration.*` (50), `collisionManager` debug (14), AnimationSystem editor methods (19), `Console.get*` introspection (14), `Editor.*`, `Profiler`, `DebugCounterManager`, `heightmap.setModified` (324 calls: editor flag), TerrainEditable/RoadEditable/GeometryOptimizer, the IGA ad code (RendDX9 10057230-1005ca77), PunkBuster (0x008c5440-0x0090cec5), TextureAtlasBuilder.dll. | none needed |

### Already well covered (for contrast)

Console scripts, archives, templates, the static and terrain renderer (roads, water, undergrowth, overgrowth, decals, rain), skinned animation and the animation systems, ragdolls, vehicle physics (newCar2, tracks, hover, walkers, aircraft), soldier movement, weapons and projectiles, gadgets, Titans, the commander's tools, squads, the HUD's core nodes (72 of 139 HudBuilder methods), name tags, menus (AVM1 player and natives), FESL login, the server browser, the client and host netcode with prediction, VoIP, the bots and nav mesh, and the Python game-mode and scoring host (47 `host.*` functions registered; all Python game-mode calls resolve).

## Module status

| Module | Status | Evidence |
|---|---|---|
| Memory.dll | **Done** (retail drop-in) | src/Memory, src/Memory/retail |
| BF2142Audio.dll | **Done** (engine, tested in retail via src/Sound/retail). The game-side use is gap 1. | src/Sound/SoundEngine; roadmap row 2 |
| BF2VoipServer.dll | **Done** | src/Voip/VoipServer*; 34% of bytes cited (the highest of any module) |
| SwiffPlayer.dll | **Mostly done** (a gameswf-style AVM1 player rewritten, natives mapped) | src/UI, 23k lines; the only large uncited range is the host glue at 10009ac0 (29 KB: AudioSettings, Bf2VoipClient, URL errors) |
| AIDLL.dll | **Partly, substantial**: behaviours, sensing, aim, strategy, path following, nav mesh, helicopters | src/AI and src/Game/Bots.cpp; the only large uncited range is the debug/console block 100090a0 (44 KB). Stand-ins are listed in engine.md "Not decoded". AITemplatePlugIn tuning is stubbed (gap 19). |
| RendDX9_ori.dll | **Partly**: static world, terrain, effects, skinned meshes and menus are drawn; shadows, post-production, hemi-maps, light manager, sky clouds and flare, video and the quality switches are not | 333 of 388 console methods stubbed. Uncited: 1012b190 (124 KB, shader parameter and post/shadow), 10186330 (96 KB, debug/menu shaders), 1016d0f0 (45 KB, SwiffRender/NameTagRenderer glue), 1011d605 (39 KB, Video/geometry loading) |
| BF2142.exe | **Partly**: game logic, physics, netcode and the HUD core are deep; the gaps above remain | 782 console stubs; 826 template properties unread; 26 uncited ranges of 30 KB or more totalling 1.7 MB of 4.8 MB (FESL/EASTL 450 KB, PunkBuster 56 KB, GameSpy GP 72 KB, settings/profile managers 0x00411d6c, 0x0046c160, 0x0048e5f0, template property tables 0x004e7cb8 / 0x00547599 / 0x0052b76f, HUD nodes 0x007ea4f0, awards 0x007c8290) |
| TextureAtlasBuilder.dll | **Not at all**; not needed (tooling that imports the unshipped Memory_r.dll) | 0 citations |
| Third party | dice_py → embedded Python 3 with a py2 translator (done). binkw32 → FFmpeg's Bink decoders, optional (`src/Movie`, gap 14). d3dx9 → native GL path. OpenAL wrapper → own engine. PunkBuster, haptics, firewall and SecuROM → dropped on purpose. | docs/binaries.md |

## Python host API

All 47 `host.*` names the shipped scripts call are registered. These are placeholders that return a constant or do nothing:

| host function | Returns | Callers | Effect |
|---|---|---|---|
| `sgl_getControlPointsInGroup` | done (0x00568890) | gpm_sl.py:149,163 | supply groups (gap 6) |
| `pmgr_getSquadLeader` | done (0x0056a570) | bf2/PlayerManager.py:159 | squad scoring |
| `pmgr_getPlayersInSquad` | done (0x0056adb0) | PlayerManager.py:170, stats.py:959 | squad field unlocks |
| `pmgr_enableScoreEvents` | done (0x0056ad00, PlayerScore) | stats.py:106,314 | — |
| `pers_getStatsKeyVal`, `pers_plrGiveFieldUnlock`, `pers_plrRequestStats`, `pers_plrRequestUnlocks` | as with no stats server (field unlocks counted) | stats/, unlocks | persistence |
| `rcon_invoke` / `rcon_feedback` | "" / no-op (**now:** the server console's output / 6/0x13 to the player, gap 13) | Admin/standard_admin (5 calls) | admin scripts (gap 13) |
| `gl_sendEndOfRoundData` | done (0x00568930: GameLogic, data block 8 from a hosted game) | endofround.py:161 | round stats via script |
| `gs_abortCurrentConnectionAttempt` | no-op | — | — |
| `sgl_getOverlayDirectory` | "" (**now:** the install's directory or `+overlayPath`, gap 13) | — | harmless |

Native object methods:
- Every method the scripts call on engine objects resolves (`getSupplyGroupId`, which BF2142.exe lacks, answers cp_getParam's supplyGroupId).
- `cp_getParam('supplyGroupsNeeded', team)` gives the (group, optional) pairs.

Keys that `pmgr_p_get`/`p_set` don't handle explicitly go to a generic per-player store, which is fine for the script-owned ones (`sgr`, `tts`, `suicide`, `totalDogtagCount`, `orderPoint`, `maxOrderPoint`). `cdKeyHash` is not served.

`ss_getParam` doesn't handle these keys: `autoBalance`, `tkpEnabled`, `tkpNeeded`, `tkpDefault`, `ranked` (asked 4 times), `globRank` and `globUnlocks`. (**Now** handled on a server from its sv settings, gap 13.)

`sgl_setParam` keeps other keys (`giveAmmoScoreLimit`, `healScoreLimit`, `repairScoreLimit`, `teamDamageScoreLimit`, `teamVehicleDamageScoreLimit`) in `GameLogic::limits` and hands them back to `sgl_getParam`, but no engine code acts on them.

"Snapshot module not found" and "Medal awarding module not found" also happen with retail files, because `bf2.stats.snapshot` and `medals` were never shipped.

## Runtime unimplemented-method summary (11 runs, top-25 lists merged)

| Calls (sum of 11 runs) | Runs | Method |
|---|---|---|
| 15,756 | 11 | GeometryTemplate.compressVertexData |
| 15,734 | 11 | GeometryTemplate.maxTextureRepeat |
| 13,292 | 11 | GeometryTemplate.lightmapOverride |
| 13,226 | 11 | GeometryTemplate.doNotGenerateLightmaps |
| 8,052 | 11 | HudBuilder.addTransformListNode |
| 5,240 | 8 | Object.setLightSourceMask |
| 3,465 / 3,465 / 3,443 / 3,443 | 11 | ObjectTemplate.warningHud.setWarningType / setWarningIcon / setSecondWarningSound / setFirstWarningSound |
| 2,541 | 11 | GeometryTemplate.isAd |
| 1,848 | 11 | ObjectTemplate.ability.setMaterialDamageMod |
| 1,760 | 11 | HudBuilder.setTranformListNodePosVariable |
| 1,265 | 11 | ObjectTemplate.armor.addArmorEffectSpectacular |
| 946 | 11 | HudBuilder.setButtonNodeMouseArea |
| 924 | 11 | HudBuilder.addNodeMoveShowEffect |
| 616 | 11 | ObjectTemplate.ability.addExtraAmmoItem |
| 528 / 528 | 11 | HudBuilder.setObjectMarkerNodeTexture / TextureSize |
| 517 | 11 | HudBuilder.addNodeBlendEffect |
| 440 / 440 | 11 | HudBuilder.setTranformListNodeOffset / addObjectMarkerNodeLockTextNode |
| 390 / 273 / 234 | 10 / 7 / 6 | ObjectTemplate.deviation.setSpeedDev / setMiscDev / setTurnDev |
| 390 | 10 | GeometryTemplate.setMaterialReflectionScale |
| 117 | 3 | HudBuilder.setBarNodeBorder |

(The per-run output was kept outside the repo.)

## Table A. Console API coverage by object

Categories: **impl** = a real method is registered (the probe got an argument-count or "not active" error back); **stored-read** = an ObjectTemplate property or one-argument setter kept as an untyped setting whose name appears as a literal in src (probably consumed); **stored-unread** = kept but no code reads that name; **stub** = falls through to ConsoleMethod::stub. "Calls" = static calls in the shipped scripts (common archives + 22 official levels and their coop variants, 6,693 script files, 791k calls).

| Module | Object | Total | impl | stored-read | stored-unread | stub | Script calls to non-impl |
|---|---|---|---|---|---|---|---|
| BF2142.exe | ObjectTemplate | 1403 | 50 | 494 | 826 | 33 | 353334 |
| BF2142.exe | HudBuilder | 139 | 72 | 0 | 0 | 67 | 1588 |
| BF2142.exe | sv | 76 | 0 | 0 | 0 | 76 | 69 |
| RendDX9 | renderer | 66 | 12 | 0 | 0 | 54 | 362 |
| RendDX9 | LightmapGeneration | 50 | 0 | 0 | 0 | 50 | 0 |
| BF2142.exe | AITemplatePlugIn | 49 | 22 | 0 | 0 | 27 | 1284 |
| BF2142.exe | Object | 47 | 7 | 0 | 0 | 40 | 16563 |
| BF2142.exe | GameServerSettings | 45 | 0 | 0 | 0 | 45 | 0 |
| BF2142.exe | GeneralSettings | 41 | 0 | 0 | 0 | 41 | 0 |
| BF2142.exe | GameLogic | 41 | 14 | 0 | 0 | 27 | 1078 |
| BF2142.exe | AnimationSystem | 32 | 13 | 0 | 0 | 19 | 0 |
| BF2142.exe | HudManager | 31 | 4 | 0 | 0 | 27 | 17 |
| BF2142.exe | Commander | 31 | 27 | 0 | 0 | 4 | 0 |
| RendDX9 | shadowManager | 30 | 0 | 0 | 0 | 30 | 0 |
| BF2142.exe | AnimationBundle | 28 | 23 | 0 | 0 | 5 | 0 |
| RendDX9 | lightManager | 27 | 6 | 0 | 0 | 21 | 432 |
| RendDX9 | Overgrowth | 27 | 0 | 0 | 0 | 27 | 416 |
| BF2142.exe | Sound | 26 | 18 | 0 | 0 | 8 | 52 |
| RendDX9 | GeometryTemplate | 26 | 4 | 0 | 0 | 22 | 6383 |
| RendDX9 | rainManager | 24 | 17 | 0 | 0 | 7 | 115 |
| BF2142.exe | weaponTemplate | 22 | 12 | 0 | 0 | 10 | 9 |
| BF2142.exe | RagDoll | 20 | 0 | 0 | 0 | 20 | 102 |
| RendDX9 | ParticleSystemManager | 20 | 0 | 0 | 0 | 20 | 0 |
| RendDX9 | skyDome | 19 | 4 | 0 | 0 | 15 | 396 |
| BF2142.exe | Material | 18 | 16 | 0 | 0 | 2 | 11 |
| BF2142.exe | LocalProfile | 16 | 0 | 0 | 0 | 16 | 0 |
| BF2142.exe | admin | 16 | 0 | 0 | 0 | 16 | 0 |
| BF2142.exe | AnimationTrigger | 16 | 12 | 0 | 0 | 4 | 0 |
| BF2142.exe | AudioSettings | 15 | 0 | 0 | 0 | 15 | 0 |
| BF2142.exe | collisionManager | 15 | 1 | 0 | 0 | 14 | 0 |
| BF2142.exe | Console | 14 | 0 | 0 | 0 | 14 | 20 |
| RendDX9 | terrain | 14 | 2 | 0 | 0 | 12 | 108 |
| BF2142.exe | VideoSettings | 13 | 0 | 0 | 0 | 13 | 0 |
| BF2142.exe | ControlMap | 12 | 7 | 0 | 0 | 5 | 2 |
| BF2142.exe | Minimap | 12 | 0 | 0 | 0 | 12 | 0 |
| RendDX9 | nametags | 12 | 0 | 0 | 0 | 12 | 105 |
| BF2142.exe | SwiffHost | 11 | 0 | 0 | 0 | 11 | 1078 |
| BF2142.exe | Player | 11 | 0 | 0 | 0 | 11 | 0 |
| BF2142.exe | CombatArea | 11 | 8 | 0 | 0 | 3 | 0 |
| BF2142.exe | heightmapcluster | 11 | 5 | 0 | 0 | 6 | 0 |
| BF2142.exe | maplist | 10 | 0 | 0 | 0 | 10 | 1 |
| BF2142.exe | SettingsManager | 10 | 0 | 0 | 0 | 10 | 10 |
| BF2142.exe | SquadMenu | 10 | 10 | 0 | 0 | 0 | 0 |
| BF2142.exe | AITemplate | 10 | 6 | 0 | 0 | 4 | 351 |
| RendDX9 | terrainCuller | 10 | 7 | 0 | 0 | 3 | 0 |
| RendDX9 | StaticMeshRenderer | 10 | 0 | 0 | 0 | 10 | 0 |
| RendDX9 | undergrowth | 10 | 0 | 0 | 0 | 10 | 71 |
| BF2142.exe | NetworkableInfo | 9 | 0 | 0 | 0 | 9 | 35 |
| BF2142.exe | UnlockInterface | 9 | 9 | 0 | 0 | 0 | 0 |
| BF2142.exe | demo | 8 | 0 | 0 | 0 | 8 | 0 |
| BF2142.exe | CommanderMenu | 8 | 7 | 0 | 0 | 1 | 0 |
| BF2142.exe | KitInterface | 8 | 8 | 0 | 0 | 0 | 0 |
| BF2142.exe | AnimationValueHolder | 8 | 4 | 0 | 0 | 4 | 0 |
| RendDX9 | ObjectDrawer | 8 | 0 | 0 | 0 | 8 | 0 |
| BF2142.exe | SquadManager | 7 | 0 | 0 | 0 | 7 | 0 |
| BF2142.exe | reservedSlots | 7 | 0 | 0 | 0 | 7 | 0 |
| BF2142.exe | heightmap | 7 | 6 | 0 | 0 | 1 | 324 |
| BF2142.exe | Radio | 7 | 5 | 0 | 0 | 2 | 0 |
| BF2142.exe | kitTemplate | 7 | 0 | 0 | 0 | 7 | 144 |
| BF2142.exe | AnimationManager | 7 | 6 | 0 | 0 | 1 | 0 |
| RendDX9 | DecalManager | 7 | 0 | 0 | 0 | 7 | 0 |
| RendDX9 | OvergrowthType | 7 | 0 | 0 | 0 | 7 | 330 |
| BF2142.exe | Game | 6 | 1 | 0 | 0 | 5 | 0 |
| BF2142.exe | MaterialManager | 6 | 5 | 0 | 0 | 1 | 10 |
| RendDX9 | Object | 6 | 1 | 0 | 0 | 5 | 0 |
| BF2142.exe | CombatAreaManager | 5 | 1 | 0 | 0 | 4 | 106 |
| BF2142.exe | SquadLeader | 5 | 1 | 0 | 0 | 4 | 0 |
| RendDX9 | Renderer | 5 | 0 | 0 | 0 | 5 | 0 |
| RendDX9 | HemiMapManager | 5 | 0 | 0 | 0 | 5 | 48 |
| BF2142.exe | GlobalSettings | 4 | 0 | 0 | 0 | 4 | 0 |
| BF2142.exe | RadioInterface | 4 | 4 | 0 | 0 | 0 | 0 |
| BF2142.exe | SpawnManager | 4 | 3 | 0 | 0 | 1 | 0 |
| BF2142.exe | MenuTeamManager | 3 | 0 | 0 | 0 | 3 | 10 |
| BF2142.exe | HudItems | 3 | 3 | 0 | 0 | 0 | 0 |
| BF2142.exe | SquadLeaderInterface | 3 | 3 | 0 | 0 | 0 | 0 |
| BF2142.exe | SquadInterface | 3 | 3 | 0 | 0 | 0 | 0 |
| BF2142.exe | CommanderInterface | 3 | 3 | 0 | 0 | 0 | 0 |
| BF2142.exe | RadioVehicleInterface | 3 | 3 | 0 | 0 | 0 | 0 |
| BF2142.exe | DemoCameraInterface | 3 | 3 | 0 | 0 | 0 | 0 |
| BF2142.exe | DemoRecorderInterface | 3 | 3 | 0 | 0 | 0 | 0 |
| BF2142.exe | CameraInterface | 3 | 3 | 0 | 0 | 0 | 0 |
| BF2142.exe | FreeCam | 3 | 0 | 0 | 0 | 3 | 0 |
| RendDX9 | envmapManager | 3 | 0 | 0 | 0 | 3 | 0 |
| BF2142.exe | renderer | 2 | 0 | 0 | 0 | 2 | 0 |
| BF2142.exe | pythonHost | 2 | 0 | 0 | 0 | 2 | 0 |
| BF2142.exe | rankSettings | 2 | 1 | 0 | 0 | 1 | 0 |
| BF2142.exe | FileManager | 2 | 2 | 0 | 0 | 0 | 0 |
| BF2142.exe | Physics | 2 | 0 | 0 | 0 | 2 | 0 |
| BF2142.exe | WindManager | 2 | 0 | 0 | 0 | 2 | 68 |
| BF2142.exe | ObjectManager | 2 | 0 | 0 | 0 | 2 | 0 |
| BF2142.exe | Chat | 2 | 2 | 0 | 0 | 0 | 0 |
| BF2142.exe | Scoreboard | 2 | 2 | 0 | 0 | 0 | 0 |
| BF2142.exe | LevelsList | 2 | 2 | 0 | 0 | 0 | 0 |
| BF2142.exe | awards | 1 | 0 | 0 | 0 | 1 | 126 |
| BF2142.exe | misc | 1 | 0 | 0 | 0 | 1 | 0 |
| BF2142.exe | Vars | 1 | 0 | 0 | 0 | 1 | 27 |
| BF2142.exe | bf2Engine | 1 | 0 | 0 | 0 | 1 | 6 |
| BF2142.exe | Renderer | 1 | 1 | 0 | 0 | 0 | 0 |
| BF2142.exe | responsePhysicsManager | 1 | 0 | 0 | 0 | 1 | 0 |
| BF2142.exe | Editor | 1 | 0 | 0 | 0 | 1 | 0 |
| BF2142.exe | ItemSelection | 1 | 0 | 0 | 0 | 1 | 0 |
| BF2142.exe | Squad | 1 | 0 | 0 | 0 | 1 | 0 |
| BF2142.exe | IngameHelp | 1 | 0 | 0 | 0 | 1 | 0 |
| BF2142.exe | aiComm | 1 | 1 | 0 | 0 | 0 | 0 |
| RendDX9 | texturemanager | 1 | 0 | 0 | 0 | 1 | 37 |
| RendDX9 | SkinnedMeshRenderer | 1 | 0 | 0 | 0 | 1 | 0 |
| | **Total** | 2837 | 475 | 494 | 826 | 1042 | 385228 |

## Table B. Every stubbed (non-ObjectTemplate-property) method the shipped scripts call, by call count

Methods marked * are parsed by a dedicated reader in src (RagDoll.* by src/Animation/RagDoll.cpp; undergrowth.load and the Overgrowth* tables are read from the level files directly), so the stub is harmless.

| Calls | Module | Method |
|---|---|---|
| 16415 | exe | `Object.setLightSourceMask` |
| 1665 | rend | `GeometryTemplate.compressVertexData` |
| 1660 | rend | `GeometryTemplate.maxTextureRepeat` |
| 1248 | rend | `GeometryTemplate.lightmapOverride` |
| 1242 | rend | `GeometryTemplate.doNotGenerateLightmaps` |
| 1068 | exe | `SwiffHost.addCredit` |
| 750 | exe | `HudBuilder.addTransformListNode` |
| 672 | exe | `ObjectTemplate.ability.setMaterialDamageMod` |
| 532 | exe | `GameLogic.messages.addRadioVoice` |
| 351 | exe | `ObjectTemplate.warningHud.setWarningType` |
| 351 | exe | `ObjectTemplate.warningHud.setWarningIcon` |
| 349 | exe | `ObjectTemplate.warningHud.setFirstWarningSound` |
| 349 | exe | `ObjectTemplate.warningHud.setSecondWarningSound` |
| 324 | exe | `heightmap.setModified` |
| 262 | exe | `GameLogic.messages.addMessage` |
| 234 | exe | `AITemplatePlugIn.setStrategicStrength` |
| 231 | rend | `GeometryTemplate.isAd` |
| 224 | exe | `ObjectTemplate.ability.addExtraAmmoItem` |
| 168 | exe | `HudBuilder.setTranformListNodePosVariable` |
| 147 | exe | `GameLogic.messages.addHelpMessage` |
| 144 | exe | `Object.disableChildren` |
| 133 | exe | `ObjectTemplate.armor.addArmorEffectSpectacular` |
| 126 | exe | `awards.setData` |
| 117 | exe | `AITemplate.degeneration` |
| 117 | exe | `AITemplate.allowedTimeDiff` |
| 117 | exe | `AITemplate.commonKnowledge` |
| 117 | exe | `AITemplatePlugIn.driveThrottleControl` |
| 117 | exe | `AITemplatePlugIn.driveTurnControl` |
| 117 | exe | `AITemplatePlugIn.aimVerticalControl` |
| 117 | exe | `AITemplatePlugIn.aimHorizontalControl` |
| 116 | exe | `AITemplatePlugIn.setCameraRelativeMinRotationDeg` |
| 116 | exe | `AITemplatePlugIn.setCameraRelativeMaxRotationDeg` |
| 113 | exe | `AITemplatePlugIn.lookVerticalControl` |
| 113 | exe | `AITemplatePlugIn.lookHorizontalControl` |
| 108 | rend | `GeometryTemplate.isTitanInterior` |
| 100 | exe | `CombatAreaManager.timeAllowedOutside` |
| 96 | exe | `kitTemplate.setBattleStrength` |
| 90 | exe | `HudBuilder.setButtonNodeMouseArea` |
| 84 | exe | `HudBuilder.addNodeMoveShowEffect` |
| 73 | rend | `GeometryTemplate.isTitanExterior` |
| 72 | rend | `terrain.waterSunIntensity` |
| 71 | rend | `undergrowth.load`* |
| 70 | exe | `AITemplatePlugIn.setIsAntiAircraft` |
| 66 | exe | `ObjectTemplate.deviation.setTurnDev` |
| 66 | exe | `ObjectTemplate.deviation.setSpeedDev` |
| 66 | exe | `ObjectTemplate.deviation.setMiscDev` |
| 55 | rend | `nametags.setTexture` |
| 55 | rend | `Overgrowth.addType`* |
| 55 | rend | `Overgrowth.setActiveType`* |
| 55 | rend | `OvergrowthType.geometry`* |
| 55 | rend | `OvergrowthType.density`* |
| 55 | rend | `OvergrowthType.normalScale`* |
| 55 | rend | `OvergrowthType.rotationScale`* |
| 55 | rend | `OvergrowthType.minRadiusToSame`* |
| 55 | rend | `OvergrowthType.minRadiusToOthers`* |
| 51 | exe | `Sound.addTrigger` |
| 48 | exe | `HudBuilder.setObjectMarkerNodeTexture` |
| 48 | exe | `HudBuilder.setObjectMarkerNodeTextureSize` |
| 48 | rend | `GeometryTemplate.setMaterialReflectionScale` |
| 47 | exe | `HudBuilder.addNodeBlendEffect` |
| 45 | rend | `nametags.createIcon` |
| 41 | exe | `HudBuilder.setTranformListNodeOffset` |
| 40 | exe | `ObjectTemplate.sound.addReloadTimeSound` |
| 40 | exe | `HudBuilder.addObjectMarkerNodeLockTextNode` |
| 39 | exe | `HudBuilder.setBarNodeBorder` |
| 37 | rend | `texturemanager.customTextureSuffix` |
| 36 | exe | `GameLogic.setBeforeSpawnCamera` |
| 36 | rend | `renderer.waterFogStartEndAndBase` |
| 36 | rend | `renderer.waterFogColor` |
| 36 | rend | `renderer.bloomFilterContrast` |
| 36 | rend | `renderer.bloomSize` |
| 36 | rend | `renderer.bloomWeightScale` |
| 36 | rend | `renderer.bloomOverlayAmount` |
| 36 | rend | `renderer.showBloomOverlayTexture` |
| 36 | rend | `renderer.defaultColorTint` |
| 36 | rend | `renderer.defaultSaturation` |
| 36 | rend | `renderer.defaultContrast` |
| 36 | rend | `terrain.init` |
| 36 | rend | `skyDome.flareTexture` |
| 36 | rend | `skyDome.flareDirection` |
| 36 | rend | `skyDome.hasCloudLayer` |
| 36 | rend | `skyDome.hasCloudLayer2` |
| 36 | rend | `skyDome.cloudTemplate` |
| 36 | rend | `skyDome.cloudTexture` |
| 36 | rend | `skyDome.cloudTexture2` |
| 36 | rend | `skyDome.scrollDirection` |
| 36 | rend | `skyDome.scrollDirection2` |
| 36 | rend | `skyDome.fadeCloudsDistances` |
| 36 | rend | `skyDome.cloudLerpFactors` |
| 36 | rend | `lightManager.init` |
| 36 | rend | `lightManager.treeAmbientColor` |
| 36 | rend | `lightManager.treeSunColor` |
| 36 | rend | `lightManager.treeSkyColor` |
| 36 | rend | `lightManager.staticSpecularColor` |
| 36 | rend | `lightManager.singlePointColor` |
| 36 | rend | `lightManager.effectSunColor` |
| 36 | rend | `lightManager.effectShadowColor` |
| 36 | rend | `lightManager.defaultEffectLightAffectionFactor` |
| 36 | rend | `lightManager.hemiLerpBias` |
| 36 | rend | `lightManager.sunSpecColor` |
| 36 | rend | `lightManager.enableSun` |
| 36 | rend | `HemiMapManager.setBaseHemiMap` |
| 36 | rend | `Overgrowth.editorEnable`* |
| 34 | exe | `GameLogic.messages.addEmoteTrigger` |
| 34 | exe | `WindManager.globalWindSpeed` |
| 34 | exe | `WindManager.globalWindDirection` |
| 34 | rend | `Overgrowth.path`* |
| 34 | rend | `Overgrowth.preLoad`* |
| 34 | rend | `Overgrowth.viewDistance`* |
| 34 | rend | `Overgrowth.viewDistance`* |
| 34 | rend | `Overgrowth.viewDistanceScale`* |
| 34 | rend | `Overgrowth.closeDistPercentage`* |
| 33 | rend | `GeometryTemplate.maxSkip3pLods` |
| 33 | rend | `Overgrowth.addMaterial`* |
| 33 | rend | `Overgrowth.setActiveMaterial`* |
| 32 | exe | `kitTemplate.setStrategicStrength` |
| 31 | exe | `HudBuilder.setButtonNodeDebug` |
| 30 | exe | `GameLogic.radialLevelIndex` |
| 30 | exe | `RagDoll.toSkeleton`* |
| 29 | rend | `GeometryTemplate.setSpecularStaticGloss` |
| 27 | exe | `Vars.set`* |
| 27 | exe | `GameLogic.useColdBreath` |
| 27 | exe | `HudBuilder.setPictureNodeBorderColor` |
| 27 | rend | `rainManager.cellDisplay` |
| 26 | exe | `ObjectTemplate.addVehicleType` |
| 25 | exe | `ObjectTemplate.supplyGroupNeeded` |
| 22 | rend | `rainManager.randomness` |
| 22 | rend | `rainManager.maxRandomChange` |
| 22 | rend | `rainManager.randomDeviationAngleSpeed` |
| 22 | rend | `rainManager.freeze` |
| 21 | exe | `AITemplatePlugIn.cullDistance` |
| 20 | exe | `Console.allowMultipleFileLoad` |
| 18 | exe | `HudBuilder.setObjectMarkerNodeObjects` |
| 18 | exe | `RagDoll.addConstraint`* |
| 17 | rend | `GeometryTemplate.setLightmapSizeAll` |
| 16 | exe | `HudBuilder.setOccupiedNodePosVariable` |
| 16 | exe | `kitTemplate.create` |
| 16 | exe | `RagDoll.addAngularConstraint`* |
| 14 | exe | `NetworkableInfo.createNewInfo` |
| 14 | exe | `NetworkableInfo.setPredictionMode` |
| 14 | exe | `HudBuilder.setTextNodeOutLineOffset` |
| 13 | exe | `RagDoll.addParticle`* |
| 12 | exe | `HudBuilder.setObjectMarkerNodeLockOnType` |
| 12 | exe | `HudBuilder.setObjectMarkerNodeWeapon` |
| 12 | exe | `HudBuilder.setNodeShowConCmd` |
| 12 | exe | `RagDoll.addCapsuleCollision`* |
| 12 | rend | `GeometryTemplate.titanExteriorPartIndex` |
| 12 | rend | `HemiMapManager.loadTitanHemiMap` |
| 10 | exe | `Material.triggerRadio` |
| 10 | exe | `MaterialManager.setRadioMessage` |
| 10 | exe | `HudBuilder.setObjectMarkerNodeLockText` |
| 10 | exe | `HudBuilder.setObjectMarkerNodeLockTextOffset` |
| 9 | exe | `HudBuilder.setMouseOverNodeConCmd` |
| 8 | exe | `MenuTeamManager.addKit` |
| 8 | exe | `ObjectTemplate.ability.setOverheatMod` |
| 8 | exe | `HudBuilder.setOccupiedNodeData` |
| 8 | exe | `HudBuilder.setButtonNodeAltConCmd` |
| 8 | exe | `HudBuilder.setNodeSize` |
| 7 | exe | `SwiffHost.addGameMode` |
| 7 | rend | `GeometryTemplate.setMaterialParallaxHeightScale` |
| 6 | exe | `bf2Engine.playMovie` |
| 6 | exe | `HudBuilder.setTruncate` |
| 6 | exe | `AITemplatePlugIn.setBattleStrength` |
| 5 | exe | `GameLogic.messages.addRadioFilter` |
| 5 | exe | `SettingsManager.boolSet` |
| 5 | exe | `NetworkableInfo.setBasePriority` |
| 5 | exe | `HudBuilder.setPictureNodeCenterPoint` |
| 5 | exe | `weaponTemplate.shouldRearm` |
| 5 | exe | `RagDoll.lockBone`* |
| 5 | rend | `nametags.createBar` |
| 4 | exe | `Object.notInAI` |
| 4 | exe | `HudBuilder.setBarNodeIncreaseShow` |
| 4 | exe | `HudBuilder.setTextNodeOutLine` |
| 4 | exe | `AITemplatePlugIn.driveRollControl` |
| 4 | exe | `AITemplatePlugIn.drivePitchControl` |
| 4 | exe | `AITemplatePlugIn.aimThrottleControl` |
| 4 | exe | `AITemplatePlugIn.aimRollControl` |
| 4 | exe | `RagDoll.addDihedralAngleConstraint`* |
| 4 | rend | `GeometryTemplate.nrOfAnimatedUVMatrix` |
| 3 | exe | `CombatAreaManager.addVehicleDamage` |
| 3 | exe | `CombatAreaManager.addVehicleTimer` |
| 3 | exe | `ObjectTemplate.setCheckObstruction` |
| 3 | exe | `SettingsManager.U32Set` |
| 3 | exe | `HudBuilder.setNodeTimesBlink` |
| 3 | exe | `HudBuilder.setNodeIntervalBlink` |
| 3 | exe | `AITemplatePlugIn.setSoundSphereRadius` |
| 3 | exe | `AITemplatePlugIn.setHearingProbability` |
| 3 | exe | `AITemplatePlugIn.setUseNoPathfindingToGetToObject` |
| 2 | exe | `MenuTeamManager.addTeam` |
| 2 | exe | `ObjectTemplate.setIfTitanChangeToWeapon` |
| 2 | exe | `HudBuilder.setObjectMarkerNodeRC` |
| 2 | exe | `HudBuilder.setBarNodeRound` |
| 2 | exe | `AITemplatePlugIn.vehicleNumber` |
| 2 | exe | `weaponTemplate.fireRate` |
| 2 | exe | `weaponTemplate.isAntiAirWeapon` |
| 2 | exe | `RagDoll.addDistanceLessThanConstraint`* |
| 2 | rend | `renderer.useSunScaling` |
| 2 | rend | `GeometryTemplate.boundingRadiusModifier` |
| 2 | rend | `GeometryTemplate.useRadiusForShadowDepth` |
| 2 | rend | `GeometryTemplate.useRadiusForShadowSpatial` |
| 1 | exe | `SwiffHost.setScrollFadeSpeed` |
| 1 | exe | `SwiffHost.setScrollSpeed` |
| 1 | exe | `SwiffHost.setScrollerRect` |
| 1 | exe | `sv.serverName` |
| 1 | exe | `sv.password` |
| 1 | exe | `sv.internet` |
| 1 | exe | `sv.bandwidthChoke` |
| 1 | exe | `sv.welcomeMessage` |
| 1 | exe | `sv.serverIP` |
| 1 | exe | `sv.serverPort` |
| 1 | exe | `sv.allowFreeCam` |
| 1 | exe | `sv.allowExternalViews` |
| 1 | exe | `sv.allowNoseCam` |
| 1 | exe | `sv.allowSpectators` |
| 1 | exe | `sv.hitIndicator` |
| 1 | exe | `sv.maxPlayers` |
| 1 | exe | `sv.maxRank` |
| 1 | exe | `sv.numPlayersNeededToStart` |
| 1 | exe | `sv.notEnoughPlayersRestartDelay` |
| 1 | exe | `sv.startDelay` |
| 1 | exe | `sv.endDelay` |
| 1 | exe | `sv.spawnTime` |
| 1 | exe | `sv.manDownTime` |
| 1 | exe | `sv.endOfRoundDelay` |
| 1 | exe | `sv.ticketRatio` |
| 1 | exe | `sv.roundsPerMap` |
| 1 | exe | `sv.timeLimit` |
| 1 | exe | `sv.scoreLimit` |
| 1 | exe | `sv.soldierFriendlyFire` |
| 1 | exe | `sv.vehicleFriendlyFire` |
| 1 | exe | `sv.soldierSplashFriendlyFire` |
| 1 | exe | `sv.vehicleSplashFriendlyFire` |
| 1 | exe | `sv.tkPunishEnabled` |
| 1 | exe | `sv.tkNumPunishToKick` |
| 1 | exe | `sv.tkPunishByDefault` |
| 1 | exe | `sv.votingEnabled` |
| 1 | exe | `sv.voteTime` |
| 1 | exe | `sv.minPlayersForVoting` |
| 1 | exe | `sv.voipEnabled` |
| 1 | exe | `sv.voipServerRemote` |
| 1 | exe | `sv.voipServerRemoteIP` |
| 1 | exe | `sv.voipServerPort` |
| 1 | exe | `sv.voipBFClientPort` |
| 1 | exe | `sv.voipBFServerPort` |
| 1 | exe | `sv.voipSharedPassword` |
| 1 | exe | `sv.voipQuality` |
| 1 | exe | `sv.gameSpyPort` |
| 1 | exe | `sv.allowNATNegotiation` |
| 1 | exe | `sv.interfaceIP` |
| 1 | exe | `sv.timeBeforeRestartMap` |
| 1 | exe | `sv.autoBalanceTeam` |
| 1 | exe | `sv.teamRatioPercent` |
| 1 | exe | `sv.maxUnlockLevel` |
| 1 | exe | `sv.minUnlockLevel` |
| 1 | exe | `sv.autoRecord` |
| 1 | exe | `sv.demoIndexURL` |
| 1 | exe | `sv.demoDownloadURL` |
| 1 | exe | `sv.autoDemoHook` |
| 1 | exe | `sv.demoQuality` |
| 1 | exe | `sv.adminScript` |
| 1 | exe | `sv.sponsorText` |
| 1 | exe | `sv.sponsorLogoURL` |
| 1 | exe | `sv.communityLogoURL` |
| 1 | exe | `sv.customMapURL` |
| 1 | exe | `sv.radioSpamInterval` |
| 1 | exe | `sv.radioMaxSpamFlagCount` |
| 1 | exe | `sv.radioBlockedDurationTime` |
| 1 | exe | `sv.useGlobalRank` |
| 1 | exe | `sv.useGlobalUnlocks` |
| 1 | exe | `sv.botSkill` |
| 1 | exe | `sv.allowTitanMovement` |
| 1 | exe | `sv.punkBuster` |
| 1 | exe | `maplist.append` |
| 1 | exe | `GameLogic.enableVoHelp` |
| 1 | exe | `GameLogic.supplyDropHeight` |
| 1 | exe | `GameLogic.supplyDropNumSecsToLive` |
| 1 | exe | `GameLogic.messages.setVolumes` |
| 1 | exe | `GameLogic.createObject` |
| 1 | exe | `Material.pythonEvent` |
| 1 | exe | `SettingsManager.stringSet` |
| 1 | exe | `SettingsManager.floatSet` |
| 1 | exe | `ControlMap.InvertMouse` |
| 1 | exe | `ControlMap.keyboardSensitivity` |
| 1 | exe | `NetworkableInfo.setIsUnique` |
| 1 | exe | `NetworkableInfo.setForceNetworkableId` |
| 1 | exe | `HudManager.setPointerSize` |
| 1 | exe | `HudManager.setMouseTextureEmpty` |
| 1 | exe | `HudManager.setMouseTextureFull` |
| 1 | exe | `HudManager.setMouseTextureArtillery` |
| 1 | exe | `HudManager.setMouseTextureUAV` |
| 1 | exe | `HudManager.setMouseTextureSupply` |
| 1 | exe | `HudManager.setMouseTextureMoveTitan` |
| 1 | exe | `HudManager.setMouseTextureOrbitalStrike` |
| 1 | exe | `HudManager.setCommMouseSensitivity` |
| 1 | exe | `HudManager.setPointerMouseSensitivity` |
| 1 | exe | `HudManager.setCommSize` |
| 1 | exe | `HudManager.setCommPos` |
| 1 | exe | `HudManager.setCommMousePos` |
| 1 | exe | `HudManager.setSpottedMenuSize` |
| 1 | exe | `HudManager.setSpottedMenuPos` |
| 1 | exe | `HudManager.setSpottedMousePos` |
| 1 | exe | `HudManager.setMaximumNrOfCPs` |
| 1 | exe | `HudBuilder.createMiniMapNode` |
| 1 | exe | `HudBuilder.setCommanderSize` |
| 1 | exe | `HudBuilder.setCommanderPos` |
| 1 | exe | `HudBuilder.setZoomIcons` |
| 1 | exe | `HudBuilder.setCPFont` |
| 1 | exe | `HudBuilder.setCPFontColor` |
| 1 | exe | `HudBuilder.setAlphaMask` |
| 1 | exe | `HudBuilder.setObjectSelectionNodePointerSize` |
| 1 | exe | `HudBuilder.newLayer` |
| 1 | exe | `Sound.windSetup` |
| 1 | exe | `RagDoll.skeleton`* |
| 1 | exe | `RagDoll.setParticleCollisionCriteria`* |

## Table C. Stubs never called by shipped scripts (by object)

These are reachable only from the console, menus, server config files outside the archives or the editor.

- **LightmapGeneration** (50): init, onlyNearest, renderTerrain, shadowsNearCamera, resolutionModifier, skyColor, ambientColor, blockSize, sampleOffset, traceTerrain, traceAllObjects, traceActiveObject, traceActiveObjectProgress, light.create, noPhongAngle, generationMethod, phongAngleMode, globalSoftness, directlyLightedThreshold, cameraNearPlane, GILightsBaseDir, GILightsIntensity, GILightsShadowIntensity, GILightsFov, GILightsViewDistance, GILightColor, numberOfGILights, useGITopLight, GITopLightIntensity, sunLightIntensity, sunLightShadowIntensity, sunLightFov, enableIntensityLightmaps, enableBackfaceCulling, lightmapFormat, skyWhite, useLog, objectPointScale, terrainPointScale, doOvergrowth, flush, getWaterSunIntensity, light.direction, light.intensity, light.shadowIntensity, light.selfShadowing, light.objectShadows, light.fov, light.viewDistance, light.softness
- **GameServerSettings** (45): setServerName, setPassword, setInternet, setBandwidthChoke, setMaxPlayers, setGameMode, setStartDelay, setEndDelay, setSpawnTime, setManDownTime, setTicketRatio, setRoundsPerMap, setTimeLimit, setScoreLimit, setSoldierFF, setVehicleFF, setSoldierSplashFF, setVehicleSplashFF, setPunishTeamKills, setVoteTime, setMinPlayersForVoting, setVoipEnabled, setVoipQuality, setVoipServerRemote, setVoipServerRemoteIP, setVoipServerPort, setVoipBFClientPort, setVoipBFServerPort, setVoipSharedPassword, setGameSpyPort, setAllowNATNegotation, setInterfaceIP, setAutoRecord, setTimeBeforeRestartMap, setDemoIndexURL, setDemoDownloadURL, setDemoHook, setAdminScript, setSponsorText, setSponsorLogoURL, setCommunityLogoURL, setSvPunkBuster, setAutoBalanceTeam, setAutoRecordRounds, setTeamRatio
- **renderer** (45): listScreenModes, screenMode, vsync, presentAsync, dumpScreen, enableHWOcclusion, drawObjects, drawBundledMeshes, draw1pMeshes, drawParticles, drawStaticMeshes, drawSkinnedMeshes, drawSkyDome, drawSunFlare, drawRoads, drawTerrain, drawUndergrowth, drawOvergrowth, drawNametags, drawTrees, drawPostProduction, drawGraphs, indoorSkinnedMeshCullDistance, indoorBundledMeshCullDistance, alphaToMask, makeTopWorldScreen, glowEnabled, glowStrength, colorBrightness, hud, empEffectLevel, empEnableSimpleShader, empVerticalRoll, empWarpAmount, empWarpFrequency, empStaticNoiseAmount, camouflageOffset, runTitanExplosion, minCullDistance, tweakNearPlane, tweakNearPlane1p, renderAheadLimit, waterNormalMap, drawHud, drawFps
- **Object** (42): setVisibleTeam, getVisibleTeam, direction, scale, attenuationRange1, attenuationRange2, color, HDRIntensity, coneAngle1, coneAngle2, fov, setAsSkyLight, togglePause, template, absolutePositionSecondary, getLightSourceMask, setIsInTweakModeRecursive, rotationSecondary, initGrid, isVisible, hasUpdate, hasCollision, isInGrid, isSaveable, setIsInTweakModeRecursive, setIsVisibleRecursive, setIsSaveableRecursive, setIsDisabledRecursive, hasGlow, glowFrequency, glowType, glowOffset, active, list, listObjectsOfTemplate, forceStart, forceStop, geometry.template, geometry.technique, geometry.subGeometry, geometry.subGeometryCount, geometry.GenerateLightmapForThisInstance
- **GeneralSettings** (41): addFavouriteServer, addServerHistory, setPlayedVOHelp, setSortKey, setSortOrder, setNumRoundsPlayed, setServerFilter, setHUDTransparency, setCrosshairColor, setBuddytagColor, setSquadtagColor, setMinimapRotate, setMinimapTransparency, setViewIntroMovie, setOutOfVoting, setBFTVSaveDirectory, setConfirmQuit, setMapIconAlphaTransparency, setMaxBots, setMaxBotsIncludeHumans, setAllowPunkBuster, setItemSelectionReverseItems, setToggleFilters, setAutoReload, setConnectionType, setAutoScreenshot, setIgnoreBuddyRequests, setCameraShake, setColorBlindFriendly, setHelpPopups, setKillMessagesFilter, setRadioMessagesFilter, setChatMessagesFilter, setConfirmLaunchEADownloader, setConfirmLaunchBFWeb, setConfirmDuplicateKey, setConfirmLogout, setConfirmDiscardBuddyMessages, setConfirmDuplicateKey, setConfirmInstantAction, setConfirmReservedSlotsWarning
- **shadowManager** (30): shadowMapSize, shadowMapObjectSize, shadowMapBitDepth, shadowMapDepthDistance, shadowMapIncludeStaticMeshes, shadowShowFrustums, shadowIntensityBias, lightmapIntensityBias, dropShadowClipHeightOffset, dynamicShadowsEnabled, dynamicStaticMeshShadowsEnabled, dynamic1pShadowsEnabled, allowAlphaShadows, alphaShadowThreshold, shadowUpdateFactor, shadowUpdatesEnabled, shadowMapDepthBias, shadowMapSlopeBias, shadowOccluderOversizeFactor, useLSSMOptimizations, usePSMOptimizations, shadowPSMUseSlideback, shadowPSMUseUnitCubeClipping, shadowPSMZNearOffset, shadowPSMZFarOffset, shadowPSMZMinInfinityZ, shadowPSMFovY, shadowTSMFocus, shadow80Line, shadowMergeDynamicShadowFrustums
- **HudBuilder** (26): setModifyer, createSliderNode, setSliderNodeData, setSliderNodeChild, setTileNodeOptions, setHoverInMiddlePos, setHoverMaxValue, setHoverWidthLength, setAnimationNodeFrames, setAnimationNodeSpeed, setAnimationNodeLooping, setPictureNodeAlphaMask, setPictureNodeBorder, setBarNodeVariableTexture, setBarNodeSnapDir, setBarNodeEffect, setBarNodeMove, setBarNodeAnim, setCompassNodeVariableTexture, setButtonNodeMouseOverColor, setButtonNodeFunction, setMouseOverNodeTexture, addNodeVariableMoveShowEffect, setNodePos, deleteNode, searchNodes
- **ParticleSystemManager** (20): drawStats, drawStats, resetStats, drawBoundingBoxEnable, startFadePercentageOfViewDistance, viewDistance, startLodPercentageOfLodDistance, affectedByWind, particleSystemShaderQuality, overdrawScale, qualityLevel, shimmerIntensity, shimmerPhases, shimmerSpeed, nsaShimmerIntensity, nsaShimmerPhases, nsaShimmerSpeed, trailShimmerIntensity, trailShimmerPhases, trailShimmerSpeed
- **AnimationSystem** (19): create, active, list, saveActive, save, reloadScript, deleteActiveBundle, deleteActiveTrigger, listTriggerTypes, activeBundle, listBundles, activeTrigger, listTriggers, hide, listAnimations, activeAnimation, listValueHolders, saveValueHolders, activeValueHolder
- **ObjectTemplate** (17): target.onLockChangeToWeapon, listTemplates, clearTemplateList, getTemplate, getPosition, getRotation, getHeatBarType, listTemplates, armor.addWreckArmorEffect, ability.addExtraAmmoItemToKit, vehicleHud.showOnCamMode, weaponHud.showOnCamMode, name, listTemplateClasses, delete, list, save
- **LocalProfile** (16): setName, setGamespyNick, setEmail, setEAOnlineMasterAccount, setEAOnlineSubAccount, setNumTimesLoggedIn, setRank, setCareerPoints, setLastBaseUpdate, setCurrentProfileLatestAwards, setCurrentProfileMuted, save, addDemoBookmark, setTotalPlayedTime, setCurrentProfileKitSetting, setCurrentProfileHeavyArmor
- **admin** (16): runNextLevel, currentLevel, nextLevel, restartMap, runLevel, banPlayer, banPlayerKey, addAddressToBanList, addKeyToBanList, removeAddressFromBanList, removeKeyFromBanList, clearBanList, listBannedAddresses, listBannedKeys, kickPlayer, listPlayers
- **Overgrowth** (16): postLoad, getMaterialList, removeActiveMaterial, getActiveMaterialName, getTypeList, removeActiveType, save, exportToTerrainTexture, paintMaterial, paintShadowmap, getGeometryList, generateAll, getLodDistance, setAtlasGenerationSourcePath, closeDistFade, lodTimer
- **AudioSettings** (15): voipEnabled, voipPlaybackVolume, voipCaptureVolume, voipCaptureThreshold, voipBoostEnabled, voipUsePushToTalk, provider, soundQuality, effectsVolume, musicVolume, helpVoiceVolume, englishOnlyVoices, enableEAX, alwaysAllowSystemOpenAL, forceNumberOfVoices
- **GameLogic** (14): togglePause, extraUAVVehicleFlightHeight, reset, getTeamLanguage, getTeamName, becomeSquadLeader, satelliteScanReloadTime, uavReloadTime, orbitalBombardmentHeight, remoteCommand, spawnAtCameraPosition, getDefaultNumberOfTicketsEx, defaultTimeToNextAIWave, spawnObjectSpeed
- **collisionManager** (14): useDeferredLoading, create, setActiveLod, drawMeshes, drawBoundingBoxes, drawPivots, drawSoldierMesh, drawPrimitives, getNameOfObjectInFocus, identityRotateObjectInFocus, rotateObjectInFocusAroundAxis, rotateObjectInFocusAroundVector, attachActiveToObjectInFocus, buildDebugCollisionMeshes
- **VideoSettings** (13): setTerrainQuality, setGeometryQuality, setLightingQuality, setDynamicLightingQuality, setDynamicShadowsQuality, setEffectsQuality, setTextureQuality, setTextureFilteringQuality, setResolution, setAntialiasing, setViewDistanceScale, setUseBloom, setVideoOptionScheme
- **Console** (13): getArgumentTypes, getEnumName, getRange, getReturnType, getDelta, hasBundledType, getTypeDescription, isDynamic, isSetGet, hasRange, getCategory, getActiveMethods, useRelativePaths
- **Minimap** (12): setPaintKit, setPaintAllKits, setPaintVehicle, setPaintAllVehicles, setCurrOrderList, setZoom, setFullScreenZoom, toggleShowKits, setCommanderOffset, setSourceBlend, setDestinationBlend, setFilterColor
- **Player** (11): moveTo, create, active, listPlayerClasses, list, saveAll, isAi, isNetwork, name, vehicle, setVehicleWithInputId
- **HudManager** (10): setSpottedAngle, enableSayPlayerChatBox, setPaint, setUpdate, addTextureAtlas, setDisplayControlpoints, setDisplayTickets, setMapStatic, setTargetTop, addFavouriteServer
- **RagDoll** (10): reset, save, active, drawConstraints, gravity, addBone, linkMode, removeConstraint, addForce, addJointConstraint
- **terrain** (10): forceRenderStyle, waterQuality, waterScroll, waterAnimSpeed, waterNormalMap, waterSpecularColor, waterColor, waterSpecularPower, waterAlphaHeightMul, waterAlphaAdd
- **StaticMeshRenderer** (10): enableRendering, noLods, materialCullingEnabled, enableLightMapAtlas, convertLightmapName, enableNewLightmapNames, enableUnifiedIndexlist, maxLightMappedLod, enableLightIndexes, allowAnisoFiltering
- **maplist** (9): load, save, configFile, mapCount, currentMap, clear, remove, list, insert
- **nametags** (9): getCullDistance, setCullDistance, maxCallDistance, vehicleNametagOffset, vehicleNametagOffsetFactor, maxFriendlyDistance, deathFadeOutTime, manDownFadeOutTime, deployableDistance
- **lightManager** (9): groundHemi, lightCullDistance, maxLightsPerBundledMesh, maxLightsPerStaticMesh, maxLightsPerSkinnedMesh, maxLightsPerTerrainPatch, skinnedMeshShaderLodDistance, lightMapAmbientAlphaCutoffRange, enableStencilCullerReset
- **undergrowth** (9): reload, enabled, dynamicShadowsEnable, simpleShaderEnable, zPassPatchCount, test, test2, viewDistanceScale, viewDistanceFade
- **demo** (8): recordDemo, stopRecording, shutdownDemo, adjustDemoFov, restartDemo, togglePlayerDemo, toggleCameraDemo, toggleDemoPauseOrPlay
- **ObjectDrawer** (8): useOcclusion, collectPlanesDistance, useExactTest, faceForwardThreshold, showCullStats, drawDebugPlanes, isInsideTitanOverride, showAllTitanParts
- **SquadManager** (7): joinSquad, leaveSquad, makeMeCommander, popNextCommanderApplicant, changeSquadName, listSquadMembers, listSquads
- **reservedSlots** (7): load, save, configFile, addNick, removeNick, clear, list
- **sv** (7): load, save, configFile, maxConnectionType, numReservedSlots, friendlyFireWithMines, ranked
- **AITemplatePlugIn** (7): selectKey, driveControls, aimControls, lookControls, setCameraRelativeDofRotationOffsetDeg, maxClimbAngle, maxRollAngle
- **weaponTemplate** (7): active, weaponActivate, weaponFire, soundSphereRadius, firingPose, explosionRadius, firesThroughTransparent
- **DecalManager** (7): drawStats, decalNormalOffset, decalGeomClipAngle, fadeDistance, startFadePercentageOfFadeDistancel, timeToLive, fadeOutAtPercentageOfTimeToLive
- **SwiffHost** (6): addAward, loadImage, enableScroller, joinServer, disconnectGame, setLatestGameVersion
- **SettingsManager** (6): intSet, stringGet, boolGet, intGet, floatGet, U32Get
- **heightmapcluster** (6): debugCheckMaterials, importSurroundingRawHeightMap, smoothEdges, stitchEdges, simulateWater, simulatedWaterDepth
- **Sound** (6): listenerReverb, removeAllTriggers, interactiveMode, showStats, showSoundSources, setMuteLevel
- **Game** (5): simulationRate, sayAll, sayTeam, crash, lockFps
- **AnimationBundle** (5): listAnimations, getAnimationStartTime, getAnimationLength, listEvents, getEventStartTime
- **Renderer** (5): fieldOfView1p, disableFadeOnRoads, roadDepthBias, roadSlopeScaleDepthBias, roadLodDistMod
- **GeometryTemplate** (5): active, dumpInstances, name, maxSkipWreckLods, printInfo
- **GlobalSettings** (4): setDefaultUser, setLastOnlineUser, setEncryptedLogin, setNamePrefix
- **NetworkableInfo** (4): saveStats, makeStatisticsMark, dumpNetworkDescriptors, dumpScopableDescriptors
- **SquadLeader** (4): sendRequest, sendRequestForOrder, addRemoveCP, issueSquadRequest
- **Commander** (4): commanderMutiny, detailedView, zoomCommander, spottedMouseOver
- **kitTemplate** (4): active, getStrategicStrength, getBattleStrength, getType
- **AnimationValueHolder** (4): getFilename, min, normal, max
- **AnimationTrigger** (4): getTypeName, listBundles, removeChild, removeBundle
- **skyDome** (4): domeSize, flareTemplate, flareFadeAdd, flareFadeMul
- **CombatArea** (3): deleteActiveCombatArea, active, getActiveCombatAreaName
- **ControlMap** (3): setActive, setYawFactor, setPitchFactor
- **FreeCam** (3): togglePlayer, toggleCamera, changeTeam
- **envmapManager** (3): loadEMIFile, generateEnvMaps, save
- **terrainCuller** (3): detailCullMaxLevel, cullerType, updateLodLevel
- **HemiMapManager** (3): makeHemiMap, hemiLerpBias, useCurrentSetting
- **pythonHost** (2): reinitialize, sendCommand
- **Physics** (2): gravity, airDensityZeroAtHeight
- **ObjectManager** (2): drawStats, printNetStatsForAllTypeOfObjects
- **Radio** (2): sendKitMessage, answer
- **rainManager** (2): clearChanges, getChanged
- **MenuTeamManager** (1): addWeapon
- **rankSettings** (1): save
- **misc** (1): getBuildNr
- **CombatAreaManager** (1): damage
- **responsePhysicsManager** (1): dumpLists
- **Editor** (1): listObjectsOfTemplate
- **ItemSelection** (1): setReverseItems
- **Squad** (1): leaveSquad
- **CommanderMenu** (1): sendRadioMessage
- **SpawnManager** (1): setPlayerTeam
- **IngameHelp** (1): showIngameHelp
- **AITemplate** (1): types
- **AnimationManager** (1): originalLength
- **SkinnedMeshRenderer** (1): shaderQuality
- **OvergrowthType** (1): randomScale

## Table D. ObjectTemplate properties stored but read by nothing, with script calls > 0

| Calls | Property |
|---|---|
| 15810 | `isNotSaveable` |
| 8751 | `setSpawnPositionOffset` |
| 4706 | `saveInSeparateFile` |
| 3543 | `createdInEditor` |
| 1523 | `lodDistance` |
| 1523 | `LodDistance` |
| 1523 | `lodDistance` |
| 1472 | `emitPrio` |
| 1472 | `preWarmTime` |
| 1472 | `emitSpeedGraph` |
| 1472 | `emitRadius` |
| 1472 | `randomIntensity` |
| 1472 | `lightMapIntensityOffset` |
| 1472 | `alphaCull` |
| 1472 | `rotationGraph` |
| 1472 | `windAffectionFactor` |
| 1472 | `collisionEnable` |
| 1472 | `collisionSpeedAlongNormal` |
| 1472 | `collisionSpeedAlongPlane` |
| 1472 | `forceToWaterSurface` |
| 1472 | `waterSurfaceOffset` |
| 1448 | `randomRotationSpeed` |
| 1448 | `randomRotationSpeed` |
| 1448 | `randomRotationSpeed` |
| 1448 | `randomRotationSpeed` |
| 1447 | `QualityLevel` |
| 1372 | `drawOrder` |
| 1372 | `drawOrder` |
| 1267 | `animationSpeed` |
| 1267 | `animationSpeed` |
| 1267 | `animationSpeed` |
| 1267 | `animationSpeed` |
| 1233 | `animationPlayOnce` |
| 1233 | `animationPlayOnce` |
| 1233 | `selfShadowIntensity` |
| 1233 | `selfShadowIntensity` |
| 1233 | `pivotOffset` |
| 1233 | `pivotOffset` |
| 1149 | `showInZoomedMode` |
| 1149 | `showInZoomedMode` |
| 1110 | `anchor` |
| 1076 | `nametag.show3dIcon` |
| 1056 | `pan` |
| 1049 | `noRotationFromParent` |
| 943 | `particleShaderType` |
| 943 | `particleShaderType` |
| 943 | `particleShaderType` |
| 670 | `setOnlyForHuman` |
| 485 | `triggerId` |
| 485 | `triggerId` |
| 439 | `randomAgeFactor` |
| 439 | `speedFromPhysics` |
| 424 | `nearCullingEnable` |
| 358 | `template` |
| 328 | `setOnlyForAI` |
| 290 | `linkedEffectContainer` |
| 259 | `createNotInGrid` |
| 258 | `castsDynamicShadow` |
| 258 | `castsDynamicShadow` |
| 244 | `vehicleHud.miniMapInfo` |
| 239 | `cullRadiusScale` |
| 239 | `cullRadiusScale` |
| 232 | `projectOnHeightmap` |
| 231 | `worldFOV` |
| 231 | `worldFOV` |
| 224 | `usedOnTransparentSurface` |
| 224 | `Transparency` |
| 224 | `sizeModifier` |
| 220 | `weaponHud.hasFireRate` |
| 215 | `startRotation` |
| 215 | `zRotation` |
| 215 | `zRotationSnap` |
| 215 | `writeZ` |
| 215 | `directionalScale` |
| 214 | `vehicleHud.useSelectionIcons` |
| 211 | `setEnterOnSpawn` |
| 210 | `keepProjectiles` |
| 201 | `ammo.reloadWithoutPlayer` |
| 196 | `replaceItem` |
| 195 | `events` |
| 195 | `minimumTimeBetweenTriggering` |
| 193 | `chance` |
| 193 | `averageTimeBetweenTests` |
| 179 | `positionalSpeedInUp` |
| 177 | `positionalSpeedInRight` |
| 172 | `helpHud.helpStringKey` |
| 172 | `helpHud.helpSoundKey` |
| 169 | `zoom.addZoomFactor` |
| 166 | `fire.addFireRate` |
| 165 | `isStaticSystem` |
| 164 | `vehicleHud.vehicleIconPos` |
| 163 | `damagedAmbientSoundLimit` |
| 158 | `allowEmoteAnimation` |
| 152 | `positionalSpeedInDof` |
| 149 | `vehicleHud.miniMapIcon` |
| 147 | `hasOnTimeEffect` |
| 139 | `isHemisphere` |
| 139 | `rotationalSpeedInDof` |
| 138 | `maintainCameraOnExit` |
| 137 | `weaponHud.selectIcon` |
| 136 | `vehicleHud.vehicleType` |
| 135 | `hasRestrictedExit` |
| 135 | `supplyGroupId` |
| 131 | `listenerObstruction` |
| 131 | `rotationalSpeedInUp` |
| 130 | `rotationalSpeedInRight` |
| 125 | `fire.fireInCameraDof` |
| 123 | `autoUseAbility` |
| 121 | `weaponHud.disableOnSprint` |
| 120 | `dontClearTeamOnExit` |
| 120 | `ignoreParentRotation` |
| 117 | `python.onRepairEvent` |
| 116 | `radio.friendlyTakeableMessage` |
| 116 | `radio.enemyTakeableMessage` |
| 116 | `radio.friendlyNotTakeableMessage` |
| 116 | `radio.enemyNotTakeableMessage` |
| 115 | `exitSpeedMod` |
| 115 | `allowInsideDynamicLights` |
| 115 | `allowInsideStaticSun` |
| 115 | `insideStaticSunDirection` |
| 115 | `insideStaticSunColor` |
| 115 | `insideStaticSunDamageColor` |
| 115 | `insideDamageFrequency` |
| 115 | `insideDamageThreshold` |
| 115 | `insideStaticSunAmbientColor` |
| 112 | `showOnMinimap` |
| 111 | `isVisible` |
| 108 | `boneIndex` |
| 106 | `penetrate.allowSolidPenetration` |
| 104 | `nametag.3dMapIcon` |
| 102 | `vehicleHud.typeIcon` |
| 95 | `delayToUse` |
| 95 | `fOV` |
| 95 | `fov` |
| 94 | `castsStaticShadow` |
| 94 | `selfLights` |
| 94 | `selfShadows` |
| 94 | `supportsPerVertex` |
| 94 | `supportsPerPixel` |
| 94 | `supportsGlow` |
| 94 | `supportsEmitter` |
| 94 | `speccolor` |
| 94 | `HDRIntensity` |
| 94 | `shadowIntensity` |
| 94 | `softness` |
| 94 | `objectShadows` |
| 94 | `scale` |
| 94 | `nametag.useBoundingRadius` |
| 93 | `recoil.cameraRecoilSpeed` |
| 93 | `recoil.cameraRecoilSize` |
| 93 | `takeOverInput` |
| 93 | `projectorTexture` |
| 93 | `projectorFov` |
| 93 | `projectorAspect` |
| 93 | `projectorNear` |
| 93 | `projectorFar` |
| 90 | `ammo.ammoType` |
| 90 | `radio.friendlyDestroyedMessage` |
| 85 | `radio.spottedMessage` |
| 84 | `maintainCameraOnExitToOtherVehicle` |
| 82 | `nametag.showInside` |
| 80 | `addWorkOnMaterial` |
| 80 | `nametag.selected3dMapIcon` |
| 80 | `nametag.showIconOnTracker` |
| 79 | `weaponHud.ShowClips` |
| 79 | `weaponHud.overheatSound` |
| 78 | `alphaIntensityRange` |
| 78 | `emitScaleFactorRange` |
| 78 | `emitSpeedScaleRange` |
| 78 | `startAgeRange` |
| 78 | `AgeScaleFactorRange` |
| 76 | `detonation.detectionRadius` |
| 76 | `alphaIntensityUsed` |
| 76 | `emitScaleFactorUsed` |
| 76 | `emitSpeedScaleUsed` |
| 76 | `startAgeUsed` |
| 76 | `AgeScaleFactorUsed` |
| 75 | `radio.enemyDestroyedMessage` |
| 73 | `detonation.maxDepthForExplosion` |
| 72 | `numNetworkedEffects` |
| 67 | `detonation.useMMOnEndEffect` |
| 66 | `recoil.recoilSize` |
| 66 | `recoil.recoilSpeed` |
| 65 | `hitImpactModifier` |
| 64 | `ability.sprintRechargeModifier` |
| 63 | `penetrate.allowLiquidPenetration` |
| 63 | `armor.canBeRepaired` |
| 60 | `altItemIndex` |
| 60 | `alternateTemplate0` |
| 60 | `alternateTemplate1` |
| 60 | `alternateTemplate2` |
| 60 | `alternateTemplate3` |
| 60 | `alternateTemplate4` |
| 60 | `nametag.showOutside` |
| 57 | `vehicleHud.sendDestroyedHudEvent` |
| 55 | `soundIntensityRange` |
| 55 | `soundIntensityUsed` |
| 54 | `armor.explosionForce` |
| 53 | `deviation.devModZoom` |
| 53 | `deviation.devModZoom` |
| 52 | `tracerTemplate` |
| 52 | `tracerScaler` |
| 52 | `tracerSizeModifier` |
| 51 | `addChild` |
| 50 | `vehicleHud.enablePostProcessingOnGuiIndex` |
| 49 | `noPhysics` |
| 48 | `soundFilter` |
| 48 | `target.disableGuidanceOnReload` |
| 48 | `soundFilter` |
| 48 | `rotateUV` |
| 48 | `vehicleHud.usePlayerIcon` |
| 48 | `Nametag.useAliveArmor` |
| 46 | `localPredictOnClient` |
| 46 | `disableCloseSpawnPoints` |
| 46 | `neverDrawAs1p` |
| 45 | `tracerConvergeDistance` |
| 44 | `followStiffness` |
| 43 | `emitScaleFactorFromSpeed` |
| 43 | `armor.wreckExplosionForce` |
| 42 | `chaseDistance` |
| 42 | `chaseAngle` |
| 41 | `activeDefenseEnabled` |
| 41 | `chaseOffset` |
| 41 | `armor.canBeRepairedWhenWreck` |
| 41 | `vehicleHud.canBeSpotted` |
| 41 | `weaponHud.enablePostProcessingOnGuiIndex` |
| 40 | `isFirearmReplacement` |
| 40 | `firearmReplacement` |
| 40 | `ammo.allowReplenish` |
| 40 | `deviation.devModStand` |
| 40 | `armor.wreckExplosionRadius` |
| 40 | `armor.wreckExplosionMaterial` |
| 39 | `maxTracerScaler` |
| 39 | `minTracerScaler` |
| 38 | `sound.noisy` |
| 38 | `collision.bouncing` |
| 38 | `collision.bouncing` |
| 38 | `collision.bouncing` |
| 38 | `collision.bouncing` |
| 37 | `collision.hasCollisionEffect` |
| 37 | `collision.hasCollisionEffect` |
| 37 | `collision.hasCollisionEffect` |
| 37 | `collision.hasCollisionEffect` |
| 36 | `startAutoRotateOnEnter` |
| 35 | `target.maxDistance` |
| 34 | `ammo.instantReloadOnEnable` |
| 34 | `detonation.detonateOnWaterCollision` |
| 34 | `armor.wreckExplosionDamage` |
| 34 | `armor.disableInputOnDestroy` |
| 34 | `vehicleHud.useChildArmor` |

(625 such properties in all; the list is cut at 250. The full data was kept outside the repo.)

## Table E. Template classes and components: property coverage

From symbols/BF2142.exe.template_properties.csv joined with Table A. Component IDs are shown as the property prefix. Ranked by script calls going to properties nothing reads (stored-unread + stub).

| Class / component | Props | impl | stored-read | stored-unread | stub | Script calls | Calls reaching code | Top unread properties by calls |
|---|---|---|---|---|---|---|---|---|
| SpriteParticleSystem | 70 | 0 | 41 | 29 | 0 | 94816 | 59722  |  LodDistance windAffectionFactor waterSurfaceOffset rotationGraph randomIntensity preWarmTime lightMapIntensityOffset forceToWaterSurface |
| NonScreenAlignedParticleSystem | 67 | 0 | 40 | 27 | 0 | 91863 | 58565  |  LodDistance windAffectionFactor waterSurfaceOffset rotationGraph randomIntensity preWarmTime lightMapIntensityOffset forceToWaterSurface |
| MeshParticleSystem | 58 | 1 | 34 | 23 | 0 | 83752 | 54561  |  LodDistance windAffectionFactor waterSurfaceOffset rotationGraph randomIntensity preWarmTime lightMapIntensityOffset forceToWaterSurface |
| TrailSystem | 66 | 0 | 43 | 23 | 0 | 82711 | 54560  |  LodDistance windAffectionFactor waterSurfaceOffset rotationGraph randomIntensity preWarmTime lightMapIntensityOffset forceToWaterSurface |
| (generic) | 24 | 4 | 0 | 15 | 5 | 72277 | 46625  |  isNotSaveable saveInSeparateFile createdInEditor pan createNotInGrid castsDynamicShadow preCacheObject createInvisible |
| ParticleSystemEmitter | 45 | 0 | 27 | 18 | 0 | 63696 | 40888  |  LodDistance windAffectionFactor waterSurfaceOffset rotationGraph preWarmTime forceToWaterSurface emitSpeedGraph emitRadius |
| SpawnPoint | 12 | 0 | 1 | 11 | 0 | 11074 | 1096  |  setSpawnPositionOffset setOnlyForHuman setOnlyForAI setEnterOnSpawn setAIEnterOnSpawn setScatterSpawnPositions |
| Emitter | 30 | 0 | 5 | 25 | 0 | 11030 | 5979  |  lodDistance showInZoomedMode template zRotationSnap zRotation startRotation positionalSpeedInUp positionalSpeedInRight |
| EffectBundle | 42 | 0 | 4 | 38 | 0 | 6334 | 2785  |  lodDistance speedFromPhysics randomAgeFactor addWorkOnMaterial startAgeRange emitSpeedScaleRange emitScaleFactorRange alphaIntensityRange |
| LightSource | 35 | 1 | 13 | 21 | 0 | 7858 | 5818  |  castsDynamicShadow fov supportsPerVertex supportsPerPixel supportsGlow supportsEmitter speccolor softness |
| 0xc506 | 26 | 0 | 10 | 16 | 0 | 6147 | 4481  |  nametag.show3dIcon nametag.3dMapIcon nametag.useBoundingRadius nametag.showInside nametag.showIconOnTracker nametag.selected3dMapIcon nametag.showOutside Nametag.useAliveArmor |
| Camera | 49 | 0 | 4 | 45 | 0 | 1922 | 342  |  worldFOV insideStaticSunDirection insideStaticSunDamageColor insideStaticSunColor insideStaticSunAmbientColor insideDamageThreshold insideDamageFrequency allowInsideStaticSun |
| 0xc4ea | 48 | 0 | 4 | 43 | 1 | 1887 | 351  |  vehicleHud.miniMapInfo vehicleHud.useSelectionIcons vehicleHud.vehicleIconPos vehicleHud.miniMapIcon vehicleHud.vehicleType vehicleHud.typeIcon vehicleHud.sendDestroyedHudEvent vehicleHud.enablePostProcessingOnGuiIndex |
| SimpleObject | 66 | 3 | 25 | 38 | 0 | 50844 | 49381  |  anchor boneIndex otherCollisionLod animatedUVTranslationSpeed animatedUVTranslationSize animatedUVTranslationReverse animatedUVTranslationMax animatedUVTranslationIndex |
| 0xc4f6 | 8 | 0 | 0 | 4 | 4 | 1400 | 0  |  warningHud.setWarningType warningHud.setWarningIcon warningHud.setSecondWarningSound warningHud.setFirstWarningSound |
| PlayerControlObject | 68 | 3 | 17 | 48 | 0 | 2130 | 743  |  damagedAmbientSoundLimit maintainCameraOnExit hasRestrictedExit listenerObstruction autoUseAbility dontClearTeamOnExit exitSpeedMod maintainCameraOnExitToOtherVehicle |
| Decal | 18 | 0 | 12 | 6 | 0 | 12004 | 10706  |  animationSpeed isStatic randomColorIntensity decalNormalMapName |
| Trigger | 7 | 0 | 1 | 6 | 0 | 2579 | 1318  |  triggerId minimumTimeBetweenTriggering events chance averageTimeBetweenTests |
| 0xc4b3 | 21 | 0 | 1 | 16 | 4 | 1147 | 24  |  ability.setMaterialDamageMod ability.addExtraAmmoItem ability.sprintRechargeModifier ability.sprintUseModifier ability.beaconSpawnTimeMod ability.setOverheatMod ability.radialRadar ability.radarRadius |
| AmbientEffectArea | 6 | 0 | 1 | 5 | 0 | 2397 | 1318  |  triggerId linkedEffectContainer projectOnHeightmap numNetworkedEffects |
| WheelEffectBundle | 30 | 0 | 0 | 30 | 0 | 1047 | 0  |  startAgeRange emitSpeedScaleRange emitScaleFactorRange alphaIntensityRange AgeScaleFactorRange startAgeUsed emitSpeedScaleUsed emitScaleFactorUsed |
| AirDraftEffectBundle | 33 | 0 | 0 | 33 | 0 | 1045 | 0  |  startAgeRange emitSpeedScaleRange emitScaleFactorRange alphaIntensityRange AgeScaleFactorRange startAgeUsed emitSpeedScaleUsed emitScaleFactorUsed |
| 0xc4ee | 41 | 0 | 10 | 30 | 1 | 1913 | 1055  |  weaponHud.hasFireRate weaponHud.selectIcon weaponHud.disableOnSprint weaponHud.overheatSound weaponHud.ShowClips weaponHud.enablePostProcessingOnGuiIndex weaponHud.hasDistanceCounter weaponHud.maxVisualDeviation |
| 0xc4b7 | 24 | 0 | 0 | 24 | 0 | 835 | 0  |  radio.friendlyTakeableMessage radio.friendlyNotTakeableMessage radio.enemyTakeableMessage radio.enemyNotTakeableMessage radio.friendlyDestroyedMessage radio.spottedMessage radio.enemyDestroyedMessage radio.closeToIndicationDist |
| Particle | 26 | 2 | 17 | 7 | 0 | 52309 | 51593  |  usedOnTransparentSurface sizeModifier Transparency otherCollisionLod isSleeping centerOfCollisionOffset |
| GenericProjectile | 35 | 0 | 14 | 21 | 0 | 3466 | 2805  |  hasOnTimeEffect hitImpactModifier tracerTemplate tracerSizeModifier tracerScaler localPredictOnClient tracerConvergeDistance activeDefenseEnabled |
| 0x946f | 6 | 0 | 1 | 3 | 2 | 635 | 25  |  cullRadiusScale cullRadiusScale deviation.setTurnDev deviation.setSpeedDev |
| 0xc4f1 | 44 | 0 | 7 | 37 | 0 | 912 | 305  |  fire.addFireRate fire.fireInCameraDof fire.addBarrelName fire.busyUntilButtonRelease fire.pullbackTime fire.projectileStartRotation fire.maxProjectilesInWorld fire.useDistanceCounter |
| GenericFireArm | 18 | 0 | 6 | 10 | 2 | 1382 | 792  |  keepProjectiles allowEmoteAnimation delayToUse altItemIndex isSelectable pickupEnabled blockInputOnRotBundle setCheckObstruction |
| 0xc4ab | 60 | 1 | 37 | 20 | 2 | 7287 | 6705  |  armor.addArmorEffectSpectacular armor.canBeRepaired armor.explosionForce armor.wreckExplosionForce armor.canBeRepairedWhenWreck armor.wreckExplosionRadius armor.wreckExplosionMaterial armor.wreckExplosionDamage |
| ControlPoint | 21 | 0 | 8 | 12 | 1 | 7750 | 7225  |  isHemisphere supplyGroupId showOnMinimap isVisible supplyGroupNeeded loseControlWhenEnemyClose |
| 0xc4f4 | 29 | 0 | 13 | 16 | 0 | 1293 | 842  |  ammo.reloadWithoutPlayer ammo.ammoType ammo.allowReplenish ammo.instantReloadOnEnable ammo.toggleWhenNoAmmo ammo.changeMagAt ammo.magLinkWeapon ammo.forceSoundPostReloadOnReload |
| 0xc4fa | 15 | 0 | 0 | 15 | 0 | 395 | 0  |  helpHud.helpStringKey helpHud.helpSoundKey helpHud.closeToDistance helpHud.lowArmorHelpStringKey helpHud.lowArmorHelpSoundKey helpHud.closeToHelpStringKeyFriendly helpHud.closeToHelpStringKeyEnemy helpHud.closeToHelpSoundKeyFriendly |
| 0xc4f8 | 14 | 0 | 8 | 6 | 0 | 672 | 327  |  recoil.cameraRecoilSpeed recoil.cameraRecoilSize recoil.recoilSpeed recoil.recoilSize recoil.recoilSpeedGB recoil.recoilForce |
| ItemContainer | 8 | 2 | 0 | 6 | 0 | 1616 | 1272  |  replaceItem isFirearmReplacement firearmReplacement replaceWithoutEquip isDogtagReplacement isRetailReplacement |
| 0xc54b | 14 | 0 | 2 | 12 | 0 | 334 | 10  |  collision.bouncing collision.bouncing collision.bouncing collision.bouncing collision.hasCollisionEffect collision.hasCollisionEffect collision.hasCollisionEffect collision.hasCollisionEffect |
| 0xc54f | 24 | 0 | 11 | 13 | 0 | 757 | 490  |  detonation.detectionRadius detonation.maxDepthForExplosion detonation.useMMOnEndEffect detonation.detonateOnWaterCollision detonation.triggerType detonation.preventDetonationFromDestroyedParent detonation.radiusDetonateWithAirVehicles detonation.detonateDistanceToTarget |
| 0xc4fc | 12 | 1 | 3 | 7 | 1 | 490 | 226  |  deviation.setMiscDev deviation.devModZoom deviation.devModZoom deviation.devModStand deviation.devModCrouch deviation.minDevModZoom deviation.minDevModZoom |
| 0xc508 | 19 | 0 | 5 | 14 | 0 | 397 | 138  |  zoom.addZoomFactor zoom.startCameraViewMode zoom.startCameraId zoom.allowUntoggledFire zoom.useProjectileCamera zoom.zoomWhenBusy zoom.zoomFineTuningNeedsUnlock zoom.preventZoomWhen3P |
| Kit | 15 | 1 | 4 | 10 | 0 | 360 | 128  |  sprintStaminaDissipationFactor geometry.kit dogTagRewardTemplate dogTagExchangeTemplate abilityInVehicleStrength abilityInVehicleRadius abilityInVehicleMaterial dropHeadwear |
| FreeCamera | 2 | 0 | 1 | 1 | 0 | 462 | 231  |  worldFOV |
| 0xc50c | 16 | 0 | 1 | 14 | 1 | 244 | 35  |  target.disableGuidanceOnReload target.maxDistance target.targetObjectTemplate target.useCameraDirectionToTarget target.setTargetAtMaxDistance target.wireGuidedLookAheadDistance target.disableFireWhenActiveMissile target.connectionEffectOnlyWhenZoomed |
| RotationalBundle | 34 | 0 | 19 | 15 | 0 | 376 | 173  |  ignoreParentRotation startAutoRotateOnEnter surpressPlayerInputVector restoreRotationOnExit automaticYawStabilization |
| 0xc563 | 2 | 0 | 0 | 2 | 0 | 169 | 0  |  penetrate.allowSolidPenetration penetrate.allowLiquidPenetration |
| 0x127bbe7 | 7 | 0 | 0 | 7 | 0 | 140 | 0  |  windType windSpeed windRadialFalloff windIsDynamic windFalloff windDirection windBlowTime |
| 0xc4bb | 4 | 0 | 3 | 1 | 0 | 250 | 133  |  python.onRepairEvent |
| 0xc55b | 12 | 0 | 7 | 5 | 0 | 216 | 108  |  follow.minDist follow.changeYaw follow.changePitch follow.targetActivation follow.followTime |
| 0xc567 | 28 | 0 | 0 | 28 | 0 | 106 | 0  |  sway.gunSwayUseZoom sway.gunSwayPitchSteps sway.gunSwayPitchMax sway.gunSwayPitchFactor sway.gunSwayPitchDirThreshold sway.gunSwayPitchDirModifier sway.gunSwayLrRandomMin sway.gunSwayLrRandomMax |
| 0xc500 | 3 | 0 | 0 | 2 | 1 | 102 | 0  |  sound.addReloadTimeSound sound.noisy sound.muteOnFire |
| 0xc4f2 | 3 | 0 | 0 | 3 | 0 | 96 | 0  |  abilityHud.repairingSound abilityHud.healingSound abilityHud.ammoSound |
| OverheadCamera | 3 | 0 | 2 | 1 | 0 | 97 | 2  |  fOV |
| Soldier | 16 | 4 | 8 | 4 | 0 | 479 | 411  |  soundFilter manDownCameraOffsetInUp manDownCameraOffsetInDof breathEffect |
| 0xc4cd | 6 | 0 | 0 | 6 | 0 | 53 | 0  |  ik.rotationRight ik.rotationLeft ik.positionRight ik.positionLeft ik.poleVectorLeft ik.poleVectorRight |
| Spring | 4 | 0 | 2 | 2 | 0 | 48 | 0  |  rotateUV |
| SupplyObject | 15 | 0 | 10 | 5 | 0 | 2477 | 2433  |  addVehicleType teamFromClosestCP visibleOn3dMap |
| TargetObject | 9 | 0 | 1 | 8 | 0 | 2383 | 2346  |  targetType disableWhenWreck disableWhenEmptyVehicle disableWhenEmptyVehicleDelay switchPlayerOnLock overrideArmingSpeed activeOnCreate |
| 0xc557 | 10 | 0 | 8 | 2 | 0 | 232 | 201  |  seek.reLockTime seek.directionBonus |
| 0xc4fe | 6 | 0 | 0 | 6 | 0 | 30 | 0  |  strategicObject.intactIcon strategicObject.strategicObjectName strategicObject.destroyedIcon strategicObject.isOnePiece strategicObject.3dMapIcon strategicObject.part |
| SupplyDepot | 5 | 0 | 4 | 1 | 0 | 2386 | 2360  |  addVehicleType |
| Bundle | 13 | 3 | 1 | 4 | 5 | 17611 | 17591  |  bundleSkeleton |
| ForceObject | 24 | 0 | 16 | 8 | 0 | 127 | 111  |  targetPosition relativeYCollisionGroupInverse enableOnStartMessage useAbsoluteDirection rotationalForce regulateRoll |
| EnvMap | 1 | 0 | 0 | 1 | 0 | 11 | 0  |  isStatic |
| Engine | 54 | 0 | 49 | 4 | 1 | 322 | 311  |  audio.newStyleAudio audio.transformationRelativeRoot |
| Parachute | 1 | 0 | 0 | 1 | 0 | 6 | 0  |  isPod |
| MechLegs | 8 | 0 | 3 | 5 | 0 | 1328 | 1322  |  testCount spacing footLength |
| AntennaObject | 5 | 0 | 3 | 2 | 0 | 10 | 6  |  antenna.maxSwayAngle antenna.accelerationFactor |
| 0xc504 | 3 | 0 | 0 | 3 | 0 | 4 | 0  |  animation.loopingFire |
| LookAtObject | 2 | 0 | 0 | 2 | 0 | 4 | 0  |  offsetToLookAt nameToLookAt |
| GroundEffectBundle | 1 | 0 | 0 | 1 | 0 | 3 | 0  |  maxTriggerDistance |
| ObjectSpawner | 16 | 1 | 14 | 1 | 0 | 23343 | 23341  |  deActivateOnEvent |
| TurnableRemoteControlledObject | 1 | 0 | 0 | 1 | 0 | 2 | 0  |  turnVelocity |
| AnimatedBundle | 3 | 0 | 3 | 0 | 0 | 224 | 224  |   |
| Item | 1 | 0 | 1 | 0 | 0 | 985 | 985  |   |
| DestroyableObject | 5 | 0 | 0 | 5 | 0 | 0 | 0  |   |
| DestroyableWindowsBundle | 4 | 0 | 0 | 4 | 0 | 0 | 0  |   |
| Obstacle | 1 | 0 | 1 | 0 | 0 | 162 | 162  |   |
| Ladder | 1 | 0 | 1 | 0 | 0 | 85 | 85  |   |
| RemoteControlledObject | 7 | 0 | 7 | 0 | 0 | 27 | 27  |   |
| DynamicBundle | 1 | 0 | 0 | 1 | 0 | 0 | 0  |   |
| EntryPoint | 1 | 0 | 1 | 0 | 0 | 0 | 0  |   |
| FloatingBundle | 5 | 0 | 4 | 1 | 0 | 15 | 15  |   |
| LandingGear | 8 | 0 | 7 | 1 | 0 | 0 | 0  |   |
| Rotor | 6 | 0 | 0 | 6 | 0 | 0 | 0  |   |
| Wing | 9 | 0 | 5 | 3 | 1 | 22 | 22  |   |
| 0xc510 | 3 | 0 | 0 | 3 | 0 | 0 | 0  |   |
| 0xc502 | 4 | 0 | 0 | 4 | 0 | 0 | 0  |   |
| Sound | 23 | 23 | 0 | 0 | 0 | 21886 | 21886  |   |

## Table F. Address coverage by module, and the largest uncited ranges

A function counts as cited when its address appears in src/ or docs/ (as 0x... or a bare 8-digit address). This is a lower bound: BF2142Audio.dll is fully rewritten but shows 13%, because the rewrite cites only its key functions. Long ranges with no citation, named by the strings their decompiled functions use, point at code nobody has looked at.

```
AIDLL.dll: 5673 functions, 998043 bytes; cited 536 functions (9.4%), 32.2% of bytes
BF2142.exe: 34357 functions, 4770835 bytes; cited 1490 functions (4.3%), 14.8% of bytes
BF2142Audio.dll: 1649 functions, 205997 bytes; cited 55 functions (3.3%), 13.2% of bytes
BF2VoipServer.dll: 281 functions, 37296 bytes; cited 37 functions (13.2%), 33.6% of bytes
Memory.dll: 129 functions, 10774 bytes; cited 8 functions (6.2%), 8.7% of bytes
RendDX9_ori.dll: 9899 functions, 1659739 bytes; cited 464 functions (4.7%), 17.1% of bytes
SwiffPlayer.dll: 5618 functions, 934486 bytes; cited 404 functions (7.2%), 38.8% of bytes
TextureAtlasBuilder.dll: 163 functions, 80366 bytes; cited 0 functions (0.0%), 0.0% of bytes
```

```
BF2142.exe: 26 uncited runs >= 30000 bytes, 1703408 of 4770835 bytes
00411d6c-0041ffcc   57952 B  607 fns | std::string | bool | float | setConfirmDuplicateKey | configFile | helpVoiceVolume | englishOnlyVoices | enableEAX | alwaysAllowSystemOpenAL | forceNumberOfVoices | setTerrainQuality | setGeometryQuality | setLightingQuality | setDynamicLightingQuality | setDynamicShadowsQuality | setEffect
00431710-0043aafa   36038 B  163 fns | Error | GSFullScreen | playback | record | http://battlefield2142.co/downloads/?map= | English | GSLanguage | Cannot change sv.numReservedSlots while running. | sv.spawnTime  | sv.timeBeforeRestartMap  | sv.autoBalanceTeam  | sv.allowFreeCam  | sv.bandwidthChoke  | sv.voteTime  | sv.useGlob
0046c160-0047ffd9   75930 B  410 fns | map/set<T> too long | SwiffRender | LocalProfileManager | GameServerSettings | CommandLine.cmd | invalid map/set<T> iterator | GeneralSettings | no mod specified | xpack2 | xpack | English | GSLanguage | unable to open commandline file | joinServerInet | joinServerLan | invalidip | GSPort |
0048e5f0-00496822   31090 B  179 fns | SwiffHost | Default | ProfileManager | EAOnlineManager | favourites | GSPlayerName | buddy | history | Profile.con | objects/weapons/handheld/ | unabletoopenprofile | DemoBookmarks.con | DemoLibrary | Default/ServerSettings.con | GlobalSettings | Unable to quickLogin while already logged in
004deba0-004e7af4   36692 B  377 fns | float | bool | std::string | io::PlayerInputMap | fire.showWeaponAfterReloadDelay | Vec3 | soundFilter | manDownCameraOffsetInDof | manDownCameraOffsetInUp | sprintRecoverTime | sprintDissipationTime | sprintLimit | sprintLossAtJump | ragdollLandedDelay | breathEffect | skeleton | animation
004e7cb8-00504086  115661 B 1196 fns | float | bool | std::string | Vec3 | radius | io::PlayerInputMap | collision.hasCollisionEffect | collision.bouncing | Vec2 | RandomDistribution | timeToLive | TargetType | deviation.devModZoom | deviation.minDevModZoom | CameraViewMode | damage | visibleOn3dMap | 3dMapIcon | noInitialCollis
0052b76f-005406d1   84411 B  798 fns | float | bool | std::string | Vec2 | RandomDistribution | Vec3 | Vec4 | randomRotation | animationEnable | animationSpeed | animationFrameWidth | animationFrameHeight | setAnimationFrameWidthRelative | setAnimationFrameHeightRelative | animationFrameCount | animationFrameCountX | timeToLive 
00547599-00561b15  106714 B  921 fns | bool | std::string | float | Vec2 | TargetType | Vec3 | regulatePosY | regulatePosZ | targetPosition | regulationTolerance | keepUp | destroyWhenTargetLost | enableOnStartMessage | uint | relativeYCollisionGroupInverse | positionToleranceXZ | minHeightAdjustDistance | warningDistance | dest
00630380-0063a0f0   37361 B  461 fns | Spawngroup allocated netId out of range  | GLSWorldSizeZ | GLSWorldSizeX | .disableOnSprint  | .enablePostProcessingOnGuiIndex  | ObjectTemplate.Ability.hasScopeStabilizerAbility  | ObjectTemplate.Ability.showVehicles  | ObjectTemplate.Ability.increasedRepairAbility  | ObjectTemplate.Abilit
0065b2c0-006655dd   39170 B  256 fns | map/set<T> too long | invalid map/set<T> iterator | Global Timer | Unknown ID %d | no_conversion_exists | ECCoreCEUpdateLoadStats | ECCoreCECollectSystemInfo | ECCoreCEPingEAOnline | ECCoreCEActivateApp | ECCoreCEStartPerformanceTest | ECCoreCEReloadRadcontent | ECCoreCEPreLoad | ECCoreCESh
00696f80-006a10da   38366 B  283 fns | list<T> too long | invalid map/set<T> iterator | @HOME@ | @ROOT@ | bf2/content/ | map/set<T> too long | vector<T> too long | not supported in runtime | C:\\dice\\Projects\\BF2142Branches\\Patch_1_51\\Co | tweak | font | desc | info | Couldn\'t resolve application data folder, termina | \\Ba
006a1100-006a9ba3   33807 B  134 fns | map/set<T> too long | GIScript3 | GIPageDown | GIMenuSelect5 | GILeftShift | GIAltOk | GIMenuSelect7 | GILeft | GISayTeam | GIScript6 | GISaySquad | GIScript9 | GIScript10 | GIAutoAccept | GIMouseWheelDown | GIOk | GISayAll | GIDown | GIMouseLookY | GILeftAlt | GICreateSquad | GIDeselectAll
007010f0-0070a6f5   36365 B  197 fns | map/set<T> too long | m_noCollisionCuller->getNumberOfObjects() == 0 | m_dynamicCuller->getNumberOfObjects() == 0 | m_noCollisionDynamicCuller->getNumberOfObjects() = | m_latencyCompensationCuller->getNumberOfObjects()  | invalid map/set<T> iterator | mHandleFrameUpdateObjects.empty() | mRe
00724b20-0073744a   72020 B  297 fns | Out of memory. | \\final\\ | Unexpected data was received from the server. | \\profileid\\ | \\file\\ | \\sesskey\\ | \\uniquenick\\ | \\pid\\ | \\zipcode\\ | \\inc\\ | \\pic\\ | \\mar\\ | \\sex\\ | \\occ\\ | Invalid info. | \\ind\\ | \\chc\\ | \\icquin\\ | \\nick\\ | \\msg\\ | https:// | e
007380a0-0074523c   51030 B  163 fns | Out of memory. | https:// | Fatal error: %s\n | warning: %s\n | warning: %s %d\n
00745ed0-00753b3a   47085 B  169 fns | ShowDogTagMessage | ShowMedalIcon | ShowRankUpIcon | IngameHelpShow | CustomInfoShow | UnlocksShow | UnlockInfoBarShow | CommanderShow | defaultGameControlMap | defaultPlayerInputControlMap | SquadRenameString | CommanderTracking | SetSpawnPoint | SquadRenameData | CommanderChatBoxString | 
0075e6d0-00770889   74167 B  569 fns | std::string | bool | float | Vec2 | setPointerSize | setMouseTextureEmpty | setMouseTextureFull | setMouseTextureArtillery | setMouseTextureUAV | setMouseTextureSupply | setMouseTextureMoveTitan | setMouseTextureOrbitalStrike | setCommMouseSensitivity | setPointerMouseSensitivity | setCommS
00781420-0078b487   40389 B   65 fns | map/set<T> too long | Ingame/Minimap/Icons/orderMine.tga | Ingame/Minimap/Icons/laserpaintTarget.tga | Ingame/Minimap/Icons/orderMoveCommander.tga | Ingame/Minimap/icon_uav_circle.tga | Ingame/Player/Icons/Minimap/mini_Ammo.tga | Ingame/Minimap/Icons/orderDefendCommander.tga | Ingame/Minima
007be960-007c6d3b   32823 B   93 fns | String | Select action | Editbox data | Focus | SecondaryHeatColorAlpha | AbilityValue | HudScanProgress | CrossRightPos | SecondaryAmmoColorGreen | BottomRight/Alpha | TargetCoordZString | PrimaryAmmoBlink | BottomRight_newXPos | SecondarySingleFireShow | SecondaryClips | PrimaryClipPathSt
007c8290-007d641b   56859 B   81 fns | HUD_HUD_AWARDED | Ingame/GeneralIcons/Medals/ | #AWARD# | AWARD_Name_%i | AWARD_NAME_%i.tga | AUTO_RULES_youbeenrecommendedforaward | QuadDrawer | menu/ | map/set<T> too long | id: %d name: %s\n | Error: Player is already ignored! | Error: Could not locate player: %d | IngamePopupShow | Sho
007e0b20-007e91b6   31561 B  358 fns | Alpha | dice::meme::BfLocaleData | End value | Start time | Delay | End time | Value | Start percentage | Green | Valid | Locale | dice::meme::BfOccupiedVehicleNode | dice::meme::BfOccupiedVehicleData | Position | BfOccupiedVehicleData | Draw debug pic | Ingame/Vehicles/Icons/Hud/Positions/
007ea4f0-007f9a71   58755 B  463 fns | std::string | float | bool | Width | Height | Picture | Action | Mouse over picture | vailable | Index | Clicked picture | Current clicked index | Transformed node | Maximum value | Number visible | Minimum value | Cursor node | Vec3 | dice::meme::Bf2BarNode | dice::meme::Bf2TransformListNo
00801fa0-008151bf   72276 B  637 fns | false | \\cygwin\\home\\codebuilder\\perforce\\feslsdk\\2. | ..\\source\\include\\fesl/internal/connection/conn | 0 && \"Illegal Action requested on current state.\ | names | ..\\source\\include\\fesl/internal/util.h | strlen(src) < dstSize | ownerType | clubId | country | ownerId | \\cygwi
00817510-00840328  136566 B 1067 fns | ..\\source\\include\\fesl/internal/util.h | strlen(src) < dstSize | count > 0 && \"Invalid allocation.\" | EASTL vector | EASTL basic_string | false | \\cygwin\\home\\codebuilder\\perforce\\feslsdk\\2. | GROUP | EASTL fixed_pool | params->GetID() == STATE_ID | ..\\source\\include\\fesl/inte
00840340-00882ff1  244111 B 2569 fns | ..\\source\\include\\fesl/internal/util.h | strlen(src) < dstSize | false | Action | Ubyte | count > 0 && \"Invalid allocation.\" | EASTL vector_map | EASTL vector | false && __FUNCTION__ | Object | Variable | EASTL hash_set | Class name | Out time | In time | HeapArray::Allocate | EASTL ba
008c5440-0090cec5   56209 B 3027 fns | PunkBuster Client Not Installed | omep | pbcl.dll | version | fs_homepath | server | fs_basepath | pbsv.dll | PunkBuster Server Not Installed | GSClPunkBuster | pbclnew.dll | PB Error: Client DLL Get Procedure Failure | PB Error: Client DLL Load Failure | pbclold.dll | set_cl_punkbuster | P
```

```
RendDX9_ori.dll: 10 uncited runs >= 20000 bytes, 547511 of 1659739 bytes
10009090-10014792   42109 B  301 fns | GSManagedTextures | list<T> too long | menu/atlas/ | GSWindowSizeX | GSWindowSizeY | GSSkipMipLevels | map/set<T> too long | VideoSettings | Error! | @HOME@/Screenshots | GSDeviceBehavior | GSDeviceType | hWndBf2TempSlt | rodSpecial | ECRendererREToggleFullscreen | ECRendererREPreChangeReso
10057230-1005ca77   20118 B   78 fns | Software_IGA | cellid | contentId | weight | radial_idb | POST___HTTP_1_0_Content_Type__ap | userid | maxsize | deflection | mintime | minsize | radial_cdb
10101c30-1010aa67   34797 B  122 fns | list<T> too long | Shaders/Road | TEXLAYER1 | Material | FogColor | TEXLAYER0 | ViewPos | BlendFactor | WorldViewProjection | fogRange | HeightmapCluster | RoadEditable
1011d605-1012764b   39360 B  155 fns | dice.hfe.geom.VideoTarget | C:\\dice\\Projects\\BF2142Branches\\Patch_1_51\\Co | pStream | Failed 2 open file  | RoadCompiled | dice.hfe.geom.GeometryTemplate. | temp_rel | dice.hfe.geom.GeometryOptimizer. | GSKeepTemplates | Geometries | map/set<T> too long | HeightmapCluster | OvergrowthS
1012b190-1014b69c  123859 B 1096 fns | Texture1 | HemiShadowAltitude | HemiMapInfo | ParticleHeatShimmer | Texture0 | ShimmerPhases | TemplateParameters | fogColor | EffectShadowColor | ProjMat | FracTime | ShimmerIntensity | EffectSunColor | BackBufferTexture | ViewMat | TexelSize | fogRange | bounding | .dds | ParticleShowFill
101543a0-101616b6   52195 B  201 fns | .dds | HeightmapCluster | .tga | -u8888 -nomipmap | /Editor | _u8888.dds | _u8888_.dds | _.dds | SETTRANSXZ | QuadPatchEditable | _%i_tmp | _tmp | COMPONENTSELECTOR | NEARTEXTILING | TEXOFFSET | BIFIXTEX | TEXLAYER7 | SurroundingPatchEditable | SETBIFIXTEX | TEXLAYER4 | SETBIFIXTEX2 | /Ligh
1016d0f0-10178cce   45227 B  205 fns | SwiffRender | map/set<T> too long | SwiffHost | invalid map/set<T> iterator | DynamicText | gpu_ | TEXT_accuracy | TEXT_scoreperminute | Menu/External/FlashMenu/ | @HOME@\\LogoCache\\ | ServerLogoManager | NameTagRenderer | default_sdgbmfbf_color_lut | @@ABSOLUTE@@Common/Textures/SpecularLU
10179060-10182428   33115 B  187 fns | TEXLAYER0 | StencilPasses | SMP1 | SMP2 | Italian | German | Swedish | French | Russian | Japanese | Korean | Czech | Chinese | Polish | Danish | SwiffHost | English | Finnish | Dutch | Thai | Spanish | DebugTextDrawer
10186330-1019fa3a   96237 B  772 fns | WorldViewProjection | Shaders/ | BLENDALPHA | World | TEXLAYER0 | MATERIALAMBIENT | DebugD3DXMeshShapeShader | t0_usezbuffer | Shaders/PortedMenuShader | QuadDrawer | Menu | DebugSphereShader | ../../BF2Editor/Content/DebugPrimitives/Textures/ | redcheckers.dds | ZBUFFER | DebugCircleShader
1019fab0-101bcbf3   60494 B  781 fns | c:\\cubedump_%i.dds | Renderer | list<T> too long | map/set<T> too long | Module:  | Debug | Line:  | Expression:  | Error | (unknown) | DxAssert | File:  | Assert | Warning | Text:  | Debug assertion failed! | BF2142 Error | Info | Alpha | Green | Height | Width
```

```
AIDLL.dll: 1 uncited runs >= 20000 bytes, 44061 of 998043 bytes
100090a0-100146ec   44061 B  311 fns | std::string | float | double | bool | hfe::uint | TemperatureTreeNode | hfe::Vec3 | CombatStrengthType | drawOnlyClosestBot | drawAddClosestBot | drawRemoveAllBots | drawAddAllBots | drawAddBot | drawRemoveBot | drawAddClosestSecondaryBot | pauseAllBots | unpauseAllBots | pauseAllButClosest
```

```
SwiffPlayer.dll: 1 uncited runs >= 20000 bytes, 28828 of 934486 bytes
10009ac0-100120d6   28828 B  368 fns | AudioSettings | vector<T> too long | Remove Me | LocalProfileManager | WARNING: The original message was too long for for | Bf2VoipClient | SwiffPlayer | dice.hfe.ui.Swiff | <There was an error parsing the URL> | TEXT_servererror | <An error occured writing to the local file (for g | <The s
```

```
BF2142Audio.dll: 2 uncited runs >= 20000 bytes, 59902 of 205997 bytes
100090a0-100146d2   39362 B  504 fns | list<T> too long | C:\\dice\\Projects\\BF2142Branches\\Patch_1_51\\Co | vector<T> too long | SND-ERR:  | SND-WRN:  | SND-INF:  | SND-ASSERTION FAILED:  | SND-DBG:  | map/set<T> too long | invalid map/set<T> iterator | At least one error caused by music player (see log | Music thread OpenAL 
10020030-100253fc   20540 B   79 fns | 
```

## Table G. Registered classes (class_map.csv) whose short name appears nowhere in src/ (docs mention in brackets: number of mentions in src+docs)

**BF2142.exe**: 
dice.bf.ClientSquadManager (1), dice.hfe.BetweenRoundsLogic (1), dice.hfe.DebugCounterManager (1), dice.hfe.DemoLibrary (1), dice.hfe.EAOnlineManager (1), dice.hfe.EventManager (2), dice.hfe.ExcelLexicon (1), dice.hfe.GameServerSettings (1), dice.hfe.GameSpyAuth (1), dice.hfe.GlobalSettings (1), dice.hfe.HTTPDownloader (1), dice.hfe.LeaderBoard (1), dice.hfe.LocalProfileManager (1), dice.hfe.MainMenuLogic (1), dice.hfe.MenuTeamManager (1), dice.hfe.ModInfo (1), dice.hfe.PlayerAwards (1), dice.hfe.PlayerProgress (2), dice.hfe.PlayerRankInfo (1), dice.hfe.PlayerUnlocks (1), dice.hfe.Profiler (1), dice.hfe.SearchForPlayer (1), dice.hfe.SelectPlayerUnlock (1), dice.hfe.ServerBrowserManager (2), dice.hfe.ServerLogoManager (1), dice.hfe.StatsManager (1), dice.hfe.io.InputDeviceManager.DX8 (1), dice.hfe.io.InputManager (1), dice.hfe.io.Stream.Buffered (1), dice.hfe.world.Culler.BinTreeCuller (1), dice.hfe.world.Culler.QuadTreeCuller (1), dice.hfe.world.HeightmapClusterEditable (1), dice.hfe.world.HeightmapEditable (1), dice.hfe.world.TemplateComponent.AbilityHud (1), dice.hfe.world.TemplateComponent.ActiveDefenseAmmoComp (1), dice.hfe.world.TemplateComponent.DefaultAnimationComp (1), dice.hfe.world.TemplateComponent.DefaultCollisionComp (1), dice.hfe.world.TemplateComponent.DefaultGunSwayComp (1), dice.hfe.world.TemplateComponent.DefaultRicochetComp (1), dice.hfe.world.TemplateComponent.HelpHud (1), dice.hfe.world.TemplateComponent.Ik (1), dice.hfe.world.TemplateComponent.LPFireComp (1), dice.hfe.world.TemplateComponent.MultiFireArmTargetComp (1), dice.hfe.world.TemplateComponent.MultiFireComp (1), dice.hfe.world.TemplateComponent.PostProcess (1), dice.hfe.world.TemplateComponent.ReplenishCollisionComp (1), dice.hfe.world.TemplateComponent.SeekClosestTargetComp (1), dice.hfe.world.TemplateComponent.SimpleTargetComp (1), dice.hfe.world.TemplateComponent.SoldierDeviationComp (2), dice.hfe.world.TemplateComponent.SpawnObjectFireComp (1), dice.hfe.world.TemplateComponent.StickyCollisionComp (1), dice.hfe.world.TemplateComponent.StrategicObject (1), dice.hfe.world.TemplateComponent.TargetDeviationComp (1), dice.hfe.world.TemplateComponent.ThrownAnimationComp (1), dice.hfe.world.TemplateComponent.ThrownSpawnPointComp (1), dice.hfe.world.TemplateComponent.ToggleCameraComp (1), dice.hfe.world.TemplateComponent.VehicleHud (1), dice.hfe.world.TemplateComponent.WarningHud (1), dice.hfe.world.TemplateComponent.WeaponBasedRecoilComp (1), dice.hfe.world.TemplateComponent.WeaponHud (1), dice.hfe.world.TemplateComponent.WindAffector (1), **RendDX9_ori.dll**: 
dice.hfe.Rend.EnvMapManager (1), dice.hfe.geom.GeometryOptimizer.BundledMeshOptimizer (1), dice.hfe.geom.GeometryOptimizer.StaticMeshOptimizer (1), dice.hfe.geom.HemiMapManager (1), dice.hfe.geom.LightGeometry (1), dice.hfe.geom.ParticleSystemManager (1), dice.hfe.geom.RainManager (1), dice.hfe.geom.RoadCompiledRenderer (1), dice.hfe.geom.RoadEditableRenderer (1), dice.hfe.geom.Roads.RoadEditable (1), dice.hfe.geom.Roads.RoadTemplate (1), dice.hfe.geom.SkinnedMeshRenderer (1), dice.hfe.geom.SkyDome (1), dice.hfe.geom.StaticMeshRenderer (1), dice.hfe.geom.SwiffRender (1), dice.hfe.geom.TerrainEditable (1), dice.hfe.geom.VideoTarget (1), 

Name matching is weak: the rewrite often uses its own names (the sky dome is drawn by src/Render without a SkyDome class), so this table is evidence only when it agrees with Tables A and E.
