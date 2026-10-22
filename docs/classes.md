# Registered classes

Every class the engine creates through its class manager registers a
`dice.hfe.*` (engine) or `dice.bf.*` (game) name, a 32-bit class ID, and a create function. `tools/decomp/class_map.py`
extracts them from the decompiled modules and follows the create function to
the vtable(s) the constructor installs. A class with several vtables uses
multiple inheritance (the first is the primary interface). Slot counts come
from `build/analysis/<module>/vtables.csv`.

Regenerate the tables with `tools/decomp/class_map.py <module>` after an analysis pass.

## BF2142.exe (191 classes)

### dice.bf

The game classes (server, client, game logic, squads, spawning).

| Class | Class ID | Create | Vtables (slots) |
|---|---|---|---|
| `dice.bf.ClientSquadManager` | `0x0000c48c` | `00424eda` | `00929e20` (48) |
| `dice.bf.SpawnManager` | `0x0000c487` | `00551c3d` | `009827e0` (20) |
| `dice.bf.SpawnPoint` | `0x0000948a` | `00551c7c` | `00982880` (38), `009829c8` (58), `009829b0` (5), `00982918` (38) |
| `dice.bf.SquadManager` | `0x0000c48b` | `00424e9b` | `00929d40` (48) |
| `dice.bf.clientGameLogic` | `0x0001d8ae` | `00424e59` | `00929840` (196), `0092983c` (1) |
| `dice.bf.gameClient` | `0x0001d8ab` | `00423b9e` | `00928f20` (32), `009bd0ec` (1), `00929028` (74), `00928fa8` (32), `00928fa0` (1) |
| `dice.bf.gameServer` | `0x0001d8aa` | `00423b5c` | `00928508` (59), `009bd0ec` (1), `00928d20` (73), `00928c30` (59), `00928c2c` (1) |
| `dice.bf.serverGameLogic` | `0x0001d8af` | `00424e17` | `009294e8` (194), `009294e4` (1) |

### dice.hfe

| Class | Class ID | Create | Vtables (slots) |
|---|---|---|---|
| `dice.hfe.AudioSettings` | `0x0001d8d0` | `0040e0f3` | `009260e0` (47) |
| `dice.hfe.BetweenRoundsLogic` | `0x0001d8b6` | `0040dfb5` | `00925350` (25) |
| `dice.hfe.ControlSettings` | `0x0001d8d4` | `0040e177` | `00926510` (42) |
| `dice.hfe.DebugCounterManager` | `0x000cd528` | `0065c643` | `009bd0ec` (1), `0098af08` (19), `0098af04` (1) |
| `dice.hfe.DemoLibrary` | `0x0001d8ea` | `0040e23a` | `00927640` (20), `009276b0` (20), `00927690` (8) |
| `dice.hfe.EAOnlineManager` | `0x0001d8f4` | `0040e2fa` | `00927a00` (28), `00928230` (49), `009281c0` (28) |
| `dice.hfe.EventManager` | `0x00009c75` | `0065c601` | `0098ae60` (16), `0093f5bc` (3) |
| `dice.hfe.ExcelLexicon` | `0x000cf853` | `00669339` | `009900b8` (8) |
| `dice.hfe.GameServerSettings` | `0x0001d8d8` | `0040e1b6` | `009266d0` (101) |
| `dice.hfe.GameSpyAuth` | `0x0001d8f6` | `0040dbfb` | `009238ac` (9) |
| `dice.hfe.GeneralSettings` | `0x0001d8da` | `0040e1f8` | `00927450` (115) |
| `dice.hfe.GlobalSettings` | `0x0001d8d6` | `0040e0b4` | `00925d18` (17) |
| `dice.hfe.HTTPDownloader` | `0x0001d8fe` | `0040e2bb` | `00923bc8` (7), `00927858` (7), `00927838` (8) |
| `dice.hfe.LeaderBoard` | `0x0001d8e2` | `0040dd78` | `00923c58` (10), `00923c30` (10), `00923c10` (8) |
| `dice.hfe.LocalProfileManager` | `0x0001d8c9` | `0040e036` | `00925968` (66) |
| `dice.hfe.MainMenuLogic` | `0x0001d8b5` | `0040df76` | `009251d8` (25) |
| `dice.hfe.MemoryPool.Standard` | `0x00009c73` | `0065c5c2` | `0098ae18` (10) |
| `dice.hfe.MenuTeamManager` | `0x0001d8cd` | `0040e075` | `00925c40` (16) |
| `dice.hfe.ModInfo` | `0x0001d8e6` | `0040de74` | `009240a8` (20) |
| `dice.hfe.PlayerAwards` | `0x0001d8e0` | `0040dc7c` | `00923ae8` (11), `009239e8` (11), `009239c8` (8) |
| `dice.hfe.PlayerProgress` | `0x0001d8fc` | `0040de35` | `00923eb8` (12), `00923f38` (13), `00923f18` (8) |
| `dice.hfe.PlayerRankInfo` | `0x0001d8f0` | `0040ddb7` | `00923c58` (10), `00923dac` (10), `00923d8c` (8) |
| `dice.hfe.PlayerStats` | `0x0001d8de` | `0040dcfa` | `00923ae8` (11), `00923b48` (11), `00923b28` (8) |
| `dice.hfe.PlayerUnlocks` | `0x0001d8e4` | `0040dcbb` | `00923bc8` (7), `00923a78` (7), `00923a58` (8) |
| `dice.hfe.ProfileManager` | `0x0001d8b9` | `0040dff4` | `009bd0ec` (1), `009254f8` (65), `009254f0` (1) |
| `dice.hfe.Profiler` | `0x0000a5d5` | `0065c57e` | `0098a988` (16), `00989c18` (14), `0098a310` (14) |
| `dice.hfe.SearchForPlayer` | `0x0001d8dc` | `0040dd39` | `00923bc8` (7), `00923ba8` (7), `00923b88` (8) |
| `dice.hfe.SelectPlayerUnlock` | `0x0001d8fa` | `0040ddf6` | `00923e78` (9), `00923e54` (9), `00923e34` (8) |
| `dice.hfe.ServerBrowser` | `0x0001d8cb` | `0040deb3` | `00924358` (26) |
| `dice.hfe.ServerBrowserManager` | `0x0001d8e8` | `0040def5` | `009243e0` (11) |
| `dice.hfe.ServerLogoManager` | `0x0001d8ec` | `0040e27c` | `00923e78` (9), `009277c4` (9), `009277a4` (8) |
| `dice.hfe.StatsManager` | `0x0001d8bb` | `0040dc3d` | `009238f0` (16) |
| `dice.hfe.SwiffHost` | `0x0001d8b4` | `0040df34` | `009bd0ec` (1), `00924d50` (250), `00924d48` (1) |
| `dice.hfe.VideoSettings` | `0x0001d8d2` | `0040e135` | `009261e8` (65) |

### dice.hfe.io

| Class | Class ID | Create | Vtables (slots) |
|---|---|---|---|
| `dice.hfe.io.ArchiveHandler.ZIP` | `0xffb1a6fa` | `0067d9c7` |  |
| `dice.hfe.io.AsyncMultiStreamer.Standard` | `0xab8d45fc` | `0067daab` | `0098e144` (8) |
| `dice.hfe.io.CacheManager.Null` | `0x4a07c0fe` | `0066b2cb` |  |
| `dice.hfe.io.ControlMap.Standard` | `0xc4cd20fb` | `006692f7` | `0098eca0` (49) |
| `dice.hfe.io.ControlMapManager.Standard` | `0xc90479fb` | `006692b8` | `00990008` (22) |
| `dice.hfe.io.FileChangeManager.Standard` | `0x336976ff` | `0067d810` | `0096c258` (5), `00990388` (14), `00990374` (5) |
| `dice.hfe.io.FileManager.Standard` | `0x9f94b3fc` | `0067d7d1` | `009902c0` (45) |
| `dice.hfe.io.FileSystem.Win32` | `0xc08658fb` | `0067d84f` | `0097d5d8` (6), `00990648` (20), `0099062c` (6) |
| `dice.hfe.io.InputDeviceManager.DX8` | `0x072ed200` | `0067dcb2` | `009909dc` (12) |
| `dice.hfe.io.InputDeviceManager.Null` | `0xb89556fc` | `0066fd57` | `0098d1a4` (12) |
| `dice.hfe.io.InputManager` | `0xbb1fc34b` | `0067dcf1` | `0098de40` (22) |
| `dice.hfe.io.NetworkableInfoManager.Standard` | `0xcccce9fb` | `0067ddba` | `00990a10` (17) |
| `dice.hfe.io.ResourceManager.Null` | `0x67e0cefe` | `00669279` | `0098ffc4` (14) |
| `dice.hfe.io.Stream.Buffered` | `0xffb8f1fa` | `0067d8cd` | `00990524` (15), `00990560` (16), `00990808` (17), `009907c8` (15), `00990788` (16) |
| `dice.hfe.io.Stream.Memory` | `0x80619afd` | `0067d88e` | `00990560` (16), `009906f8` (21), `009906b8` (16) |

### dice.hfe.world

| Class | Class ID | Create | Vtables (slots) |
|---|---|---|---|
| `dice.hfe.world.CollisionManager` | `0x000cae18` | `006b244d` | `009bd0ec` (1), `009950d8` (42), `009950d0` (1) |
| `dice.hfe.world.Culler.BinTreeCuller` | `0x0000c357` | `006f3334` | `0099aef0` (20) |
| `dice.hfe.world.Culler.QuadTreeCuller` | `0x0000c356` | `006f32f2` | `0099ade8` (20) |
| `dice.hfe.world.Heightmap` | `0x000c8709` | `006b238a` | `00994580` (62) |
| `dice.hfe.world.HeightmapCluster` | `0x000c8708` | `006b234b` | `009bd0ec` (1), `00992b90` (45), `00994b50` (1) |
| `dice.hfe.world.HeightmapClusterEditable` | `0x000c870b` | `006b240e` | `00994c78` (25), `00994b58` (45), `00994c10` (25), `00994b50` (1) |
| `dice.hfe.world.HeightmapEditable` | `0x000c870a` | `006b23cc` | `00994af0` (15), `0096c258` (5), `009949f8` (62), `009949bc` (15), `009949a8` (5) |
| `dice.hfe.world.Object.Standard` | `0x0000c379` | `006f32ae` | `0096c258` (5), `00999d68` (96), `0096f3c8` (5) |
| `dice.hfe.world.ObjectManager.Standard` | `0x0000c35c` | `006f322d` | `0099aba8` (81) |
| `dice.hfe.world.ObjectPriorityCalculator.Standard` | `0x0000c37f` | `006f33d6` |  |
| `dice.hfe.world.ObjectTemplate.AirDraftEffectBundle` | `0x00009485` | `0052c837` | `0097c588` (64), `0097c574` (5), `0098093c` (14), `0097c3c8` (107) |
| `dice.hfe.world.ObjectTemplate.AmbientEffectArea` | `0x00009c44` | `004db764` | `00969d80` (68), `00969d68` (5), `009634d8` (14), `00977e30` (11) |
| `dice.hfe.world.ObjectTemplate.AnimatedBundle` | `0x0000949c` | `004db302` | `0096bf00` (68), `0096bee8` (5), `009634d8` (14), `00977e30` (11) |
| `dice.hfe.world.ObjectTemplate.AntennaObject` | `0x0000949d` | `004db2c0` | `0096bda8` (64), `0096bd94` (5), `0097b348` (14) |
| `dice.hfe.world.ObjectTemplate.AreaObject` | `0x000094b3` | `004db5d8` | `0096f5b8` (68), `0096f5a4` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplate.Bundle` | `0x00009471` | `0051ab14` | `00923ae8` (11), `00979148` (68), `00979134` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplate.Camera` | `0x00009472` | `0051ab98` | `00979428` (68), `00979410` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplate.ControlPoint` | `0x000094a5` | `004db554` | `0096a458` (63), `0096aaa8` (68), `0096aa90` (5), `0096aa58` (14), `0096aa2c` (11), `0096a930` (63) |
| `dice.hfe.world.ObjectTemplate.Decal` | `0x00009487` | `0052c8bb` | `0097c998` (58), `0097c980` (5) |
| `dice.hfe.world.ObjectTemplate.DestroyableObject` | `0x0000c4d0` | `004db23c` | `0096ba28` (68), `0096ba14` (5), `009634d8` (14), `00977e30` (11) |
| `dice.hfe.world.ObjectTemplate.DestroyableWindow` | `0x0000c4dd` | `004db1fa` | `0096b6d0` (64), `0096b6b8` (5), `0097b348` (14) |
| `dice.hfe.world.ObjectTemplate.DestroyableWindowsBundle` | `0x0000c4dc` | `004db1b8` | `0096b338` (68), `0096b320` (5), `0096b2e8` (14), `0096b2bc` (11) |
| `dice.hfe.world.ObjectTemplate.DynamicBundle` | `0x00009c41` | `0051ab56` | `009792b8` (68), `009792a4` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplate.EffectBundle` | `0x00009474` | `0052c7b3` | `0097bb28` (107), `0097ba10` (70), `0097b9f8` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplate.Emitter` | `0x00009475` | `0052c8fa` | `0097d310` (64), `0097d2fc` (5), `0098093c` (14) |
| `dice.hfe.world.ObjectTemplate.Engine` | `0x00009476` | `0053fb8b` | `0097f058` (18), `0097f160` (68), `0097f14c` (5), `0097f114` (14), `0097f0e8` (11), `0097f0a0` (18) |
| `dice.hfe.world.ObjectTemplate.EntryPoint` | `0x00009477` | `0051abda` | `00979eb8` (64), `00979ea0` (5), `0098093c` (14) |
| `dice.hfe.world.ObjectTemplate.EnvMap` | `0x00009488` | `0052cb07` | `0097e6d0` (64), `0097e6b8` (5), `0097e680` (14) |
| `dice.hfe.world.ObjectTemplate.FloatingBundle` | `0x00009478` | `0053fbcd` | `0097fa28` (68), `0097fa10` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplate.ForceObject` | `0x00009489` | `0053fd17` | `00980988` (91), `00980974` (5), `0098093c` (14) |
| `dice.hfe.world.ObjectTemplate.FreeCamera` | `0x0000948b` | `0051aca0` | `0097a540` (68), `0097a52c` (5), `009634d8` (14), `00977e30` (11) |
| `dice.hfe.world.ObjectTemplate.GenericFireArm` | `0x000094d3` | `004db386` | `0096ac14` (8), `0096c758` (68), `0096c744` (5), `0096c70c` (14), `0096c6e0` (11), `0096c6c0` (8) |
| `dice.hfe.world.ObjectTemplate.GenericProjectile` | `0x00009539` | `004dbef0` | `0097f058` (18), `009693d8` (70), `009693c4` (5), `0096938c` (14), `00969360` (11), `00969318` (18) |
| `dice.hfe.world.ObjectTemplate.GroundEffectBundle` | `0x00009486` | `0052c879` | `0097c860` (64), `0097c84c` (5), `0098093c` (14) |
| `dice.hfe.world.ObjectTemplate.Item` | `0x00009492` | `004db134` | `0096ac14` (8), `0096ae58` (74), `0096ae44` (5), `00963914` (14), `0096ae18` (11), `009638c8` (8) |
| `dice.hfe.world.ObjectTemplate.ItemContainer` | `0x00009499` | `004db176` | `0096ac14` (8), `0096afc0` (68), `0096ae44` (5), `00963914` (14), `0096ae18` (11), `0096afa0` (8) |
| `dice.hfe.world.ObjectTemplate.Kit` | `0x00009496` | `004db27e` | `00963960` (74), `0096394c` (5), `00963914` (14), `009638e8` (11), `009638c8` (8) |
| `dice.hfe.world.ObjectTemplate.KitPart` | `0x000094a3` | `004db48e` | `0096eb98` (73), `00979134` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplate.Ladder` | `0x00009c40` | `00504ba4` | `0093e4b8` (64), `0093e4a4` (5), `0093e46c` (14) |
| `dice.hfe.world.ObjectTemplate.LadderContainer` | `0x00009c46` | `004db65c` | `00967790` (71), `00967778` (5), `00967740` (14), `00967714` (11), `00967578` (103) |
| `dice.hfe.world.ObjectTemplate.LandingGear` | `0x000094ae` | `0053fc93` | `00980070` (68), `00980058` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplate.LightSource` | `0x000094c7` | `0051ad24` | `0097ab68` (130), `0097ab50` (5) |
| `dice.hfe.world.ObjectTemplate.LookAtObject` | `0x0000948d` | `0051ada5` | `0097b398` (64), `0097b380` (5), `0097b348` (14) |
| `dice.hfe.world.ObjectTemplate.MechLegs` | `0x00009482` | `0053fd59` | `00980b30` (84), `00980b18` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplate.MeshParticleSystem` | `0x000094d0` | `0052c9ff` | `0097df00` (91), `0097dee8` (5) |
| `dice.hfe.world.ObjectTemplate.NonScreenAlignedParticleSystem` | `0x00009537` | `0052cac5` | `0097e518` (77), `0097e500` (5) |
| `dice.hfe.world.ObjectTemplate.ObjectSpawner` | `0x000094a1` | `004db3c8` | `009684a8` (48), `00968ad8` (64), `00968ac0` (5), `00968a88` (14), `009689c8` (48) |
| `dice.hfe.world.ObjectTemplate.Obstacle` | `0x000094af` | `004db596` | `0096f298` (68), `00979134` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplate.OverheadCamera` | `0x0000948c` | `0051ace2` | `00977e70` (68), `00977e5c` (5), `009634d8` (14), `00977e30` (11) |
| `dice.hfe.world.ObjectTemplate.Parachute` | `0x0000c5a8` | `004db44c` | `0096e518` (71), `0096e500` (5), `0096e00c` (14), `0096e4d4` (11), `0096e338` (103) |
| `dice.hfe.world.ObjectTemplate.Particle` | `0x0000947b` | `0052c93c` | `0097d5d8` (6), `0097d7c0` (61), `0097d7a8` (5), `0097d790` (6) |
| `dice.hfe.world.ObjectTemplate.ParticleSystemEmitter` | `0x000094d2` | `0052ca83` | `0097e340` (85), `0097e328` (5) |
| `dice.hfe.world.ObjectTemplate.PlayerControlObject` | `0x0000c4c2` | `004db40a` | `0096cf98` (103), `0096e058` (71), `0096e044` (5), `0096e00c` (14), `0096e4d4` (11), `0096e338` (103) |
| `dice.hfe.world.ObjectTemplate.RemoteControlledObject` | `0x00009c42` | `004db69e` | `00963528` (68), `00963510` (5), `009634d8` (14), `00977e30` (11) |
| `dice.hfe.world.ObjectTemplate.RotationalBundle` | `0x0000947d` | `0051ac1c` | `00979ff0` (68), `00979fd8` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplate.Rotor` | `0x000094cb` | `0053fcd5` | `009802a0` (68), `00980288` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplate.SimpleObject` | `0x0000947e` | `0051ac5e` | `00977608` (14), `00977c70` (64), `00977c5c` (5), `0098093c` (14) |
| `dice.hfe.world.ObjectTemplate.Soldier` | `0x00009493` | `004db344` | `0096c258` (5), `0096c2f8` (84), `0096c2e4` (5), `0096c2ac` (14), `0096c280` (11), `0096c26c` (5) |
| `dice.hfe.world.ObjectTemplate.Spring` | `0x00009481` | `0053fc0f` | `0097fcf0` (68), `0097fcdc` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplate.SpriteParticle` | `0x00009480` | `0052c97e` | `0097daa0` (58), `0097da88` (5) |
| `dice.hfe.world.ObjectTemplate.SpriteParticleSystem` | `0x000094cf` | `0052c9bd` | `0097dbe0` (83), `0097dbc8` (5) |
| `dice.hfe.world.ObjectTemplate.SupplyDepot` | `0x000094a4` | `004db4d0` | `00968e78` (64), `00968e60` (5), `0098093c` (14) |
| `dice.hfe.world.ObjectTemplate.SupplyObject` | `0x00009c47` | `004db512` | `0096efe0` (68), `0096efcc` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplate.TargetObject` | `0x000094be` | `004db61a` | `00966708` (68), `009666f0` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplate.TrailSystem` | `0x000094d1` | `0052ca41` | `0097e108` (77), `0097e0f0` (5) |
| `dice.hfe.world.ObjectTemplate.Trigger` | `0x00009c45` | `004db722` | `0096fb68` (68), `0096fb54` (5), `009634d8` (14), `00977e30` (11) |
| `dice.hfe.world.ObjectTemplate.TurnableRemoteControlledObject` | `0x00009c43` | `004db6e0` | `0096f978` (68), `0096f964` (5), `0096f92c` (14), `0096f900` (11) |
| `dice.hfe.world.ObjectTemplate.UAVVehicle` | `0x00009c52` | `004db7a6` | `00967e58` (68), `00979134` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplate.WheelEffectBundle` | `0x00009484` | `0052c7f5` | `0097bfd0` (107), `0097bec8` (66), `0097beb0` (5), `0098093c` (14) |
| `dice.hfe.world.ObjectTemplate.Wing` | `0x00009483` | `0053fc51` | `0097ff28` (68), `0097ff10` (5), `0097f9d8` (14), `00979278` (11) |
| `dice.hfe.world.ObjectTemplateManager.Standard` | `0x0000c36f` | `006f36df` | `009bd0ec` (1), `0099b080` (35), `0099b078` (1) |
| `dice.hfe.world.PhysicsNode.Basic` | `0x0000c423` | `006b251f` | `009938a0` (118) |
| `dice.hfe.world.PhysicsNodeManager.Basic` | `0x0000c441` | `006b2561` | `009954bc` (12) |
| `dice.hfe.world.PhysicsSystem.Basic` | `0x0000c3d3` | `006b24e0` | `009951c0` (12) |
| `dice.hfe.world.PlayerManager.Standard` | `0x0000c374` | `00423be0` | `009292a0` (52) |
| `dice.hfe.world.ResponsePhysics.Basic` | `0x0000c42d` | `006b25a0` | `00992cb8` (41) |
| `dice.hfe.world.ResponsePhysicsManager.Basic` | `0x0000c45f` | `006b25e2` | `0099554c` (15) |
| `dice.hfe.world.TemplateComponent.Ability` | `0x0000c4b2` | `00548a20` | `0096c258` (5), `00981e68` (52), `00981e50` (5) |
| `dice.hfe.world.TemplateComponent.AbilityHud` | `0x0000c4f3` | `00552c07` | `0096c258` (5), `00984078` (18), `00984060` (5) |
| `dice.hfe.world.TemplateComponent.ActiveDefenseAmmoComp` | `0x000094f2` | `004db92c` | `00970f98` (13), `00970f84` (5) |
| `dice.hfe.world.TemplateComponent.Armor` | `0x0000c4aa` | `005489de` | `0096c258` (5), `00981898` (133), `00981880` (5) |
| `dice.hfe.world.TemplateComponent.CamouflageAmmoComp` | `0x000094f1` | `004db8ed` | `00970de8` (13), `00970dd4` (5) |
| `dice.hfe.world.TemplateComponent.CountdownSoundComp` | `0x000094f7` | `004dba67` | `00971db8` (12), `00971da4` (5) |
| `dice.hfe.world.TemplateComponent.DefaultAmmoComp` | `0x000094e0` | `004db86c` | `00970958` (12), `00970944` (5) |
| `dice.hfe.world.TemplateComponent.DefaultAnimationComp` | `0x000094e8` | `004dbc20` | `009731f0` (20), `009731dc` (5) |
| `dice.hfe.world.TemplateComponent.DefaultCollisionComp` | `0x0000953a` | `004dbf32` | `0096c258` (5), `00974ad8` (12), `00974ac4` (5) |
| `dice.hfe.world.TemplateComponent.DefaultDetonationComp` | `0x0000953b` | `004dc02e` | `0096c258` (5), `00975514` (15), `00975500` (5) |
| `dice.hfe.world.TemplateComponent.DefaultFollowComp` | `0x00009542` | `004dc1ed` | `0096c258` (5), `00976030` (11), `0097601c` (5) |
| `dice.hfe.world.TemplateComponent.DefaultGunSwayComp` | `0x000094f6` | `004dbbe1` | `00973018` (13), `00973004` (5) |
| `dice.hfe.world.TemplateComponent.DefaultPenetrateComp` | `0x0000953e` | `004dc0f1` | `0096c258` (5), `00975960` (11), `0097594c` (5) |
| `dice.hfe.world.TemplateComponent.DefaultProjSoundComp` | `0x0000953c` | `004dc0b2` | `0096c258` (5), `00975814` (11), `00976104` (5) |
| `dice.hfe.world.TemplateComponent.DefaultRicochetComp` | `0x0000953d` | `004dc130` | `0096c258` (5), `00975a34` (11), `00976104` (5) |
| `dice.hfe.world.TemplateComponent.DefaultSoundComp` | `0x000094e7` | `004dba28` | `0096c258` (5), `00971b98` (12), `00971b84` (5) |
| `dice.hfe.world.TemplateComponent.DefaultZoomComp` | `0x000094e5` | `004db9aa` | `00971640` (13), `0097162c` (5) |
| `dice.hfe.world.TemplateComponent.ExplosivesFireComp` | `0x000094df` | `004dbeae` | `0097478c` (13), `00974778` (5) |
| `dice.hfe.world.TemplateComponent.FixedTargetComp` | `0x00009541` | `004dc1ae` | `0096c258` (5), `00975d38` (11), `00975d24` (5) |
| `dice.hfe.world.TemplateComponent.HelpHud` | `0x0000c4fb` | `00552c8b` | `0096c258` (5), `009842c0` (42), `009842a8` (5) |
| `dice.hfe.world.TemplateComponent.Ik` | `0x0000c4cc` | `0051ade7` | `0096c258` (5), `0097b4d0` (26), `0097b4b8` (5) |
| `dice.hfe.world.TemplateComponent.LPFireComp` | `0x000094dd` | `004dbda6` | `00973d48` (13), `00973d34` (5) |
| `dice.hfe.world.TemplateComponent.LPTargetComp` | `0x000094eb` | `004dbce0` | `00973604` (12), `009735f0` (5) |
| `dice.hfe.world.TemplateComponent.MultiFireArmTargetComp` | `0x000094ea` | `004dbc9e` | `00973574` (12), `00973560` (5) |
| `dice.hfe.world.TemplateComponent.MultiFireComp` | `0x000094dc` | `004db82a` | `0097062c` (13), `00970618` (5) |
| `dice.hfe.world.TemplateComponent.Nametag` | `0x0000c507` | `00552d0c` | `0096c258` (5), `00984910` (66), `009848f8` (5) |
| `dice.hfe.world.TemplateComponent.ParentTargetComp` | `0x000094f0` | `004dbd64` | `00973cac` (12), `00973c98` (5) |
| `dice.hfe.world.TemplateComponent.PostProcess` | `0x0000c503` | `00552b44` | `0096c258` (5), `00982d50` (20), `00982d38` (5) |
| `dice.hfe.world.TemplateComponent.PreDestinedFollowComp` | `0x00009543` | `004dc22c` | `0096c258` (5), `00976118` (11), `00976104` (5) |
| `dice.hfe.world.TemplateComponent.PulseMeterAmmoComp` | `0x000094f4` | `004db96b` | `00971220` (13), `0097120c` (5) |
| `dice.hfe.world.TemplateComponent.Python` | `0x0000c4ba` | `00548a62` | `0096c258` (5), `00981f70` (20), `00981f58` (5) |
| `dice.hfe.world.TemplateComponent.Radio` | `0x0000c4b6` | `00548aa1` | `0096c258` (5), `00982098` (62), `00982080` (5) |
| `dice.hfe.world.TemplateComponent.ReplenishCollisionComp` | `0x00009544` | `004dbf71` | `0096c258` (5), `00974c20` (12), `00974c0c` (5) |
| `dice.hfe.world.TemplateComponent.ReplenishDetonationComp` | `0x00009547` | `004dc070` | `00975680` (15), `0097566c` (5) |
| `dice.hfe.world.TemplateComponent.ReplenishingAmmoComp` | `0x000094ed` | `004db8ab` | `009709cc` (13), `009709b8` (5) |
| `dice.hfe.world.TemplateComponent.ResurrectCollisionComp` | `0x00009545` | `004dbfb0` | `0096c258` (5), `00974d6c` (12), `00974d58` (5) |
| `dice.hfe.world.TemplateComponent.SeekClosestTargetComp` | `0x00009540` | `004dc16f` | `0096c258` (5), `00975c5c` (11), `00975c48` (5) |
| `dice.hfe.world.TemplateComponent.SimpleDeviationComp` | `0x000094e2` | `004dbb63` | `00972708` (13), `009726f4` (5) |
| `dice.hfe.world.TemplateComponent.SimpleTargetComp` | `0x000094ec` | `004dbd22` | `00973b08` (12), `00973af4` (5) |
| `dice.hfe.world.TemplateComponent.SingleFireComp` | `0x000094db` | `004db7e8` | `00970390` (13), `0097037c` (5) |
| `dice.hfe.world.TemplateComponent.SoldierBasedRecoilComp` | `0x000094e3` | `004dbaa6` | `00972094` (13), `00972080` (5) |
| `dice.hfe.world.TemplateComponent.SoldierDeviationComp` | `0x000094e1` | `004dbb24` | `009725b4` (13), `009725a0` (5) |
| `dice.hfe.world.TemplateComponent.SpawnObjectFireComp` | `0x000094ef` | `004dbde8` | `009740b8` (13), `009740a4` (5) |
| `dice.hfe.world.TemplateComponent.StickyCollisionComp` | `0x00009546` | `004dbfef` | `0096c258` (5), `00974f0c` (12), `00974ef8` (5) |
| `dice.hfe.world.TemplateComponent.StrategicObject` | `0x0000c4ff` | `00552ccd` | `0096c258` (5), `009846e8` (27), `009846d0` (5) |
| `dice.hfe.world.TemplateComponent.TargetDeviationComp` | `0x000094ee` | `004dbba2` | `00972798` (13), `00972784` (5) |
| `dice.hfe.world.TemplateComponent.TeamChange` | `0x0000c50f` | `0054899f` | `0096c258` (5), `00980d80` (18), `00980d68` (5) |
| `dice.hfe.world.TemplateComponent.ThrownAnimationComp` | `0x000094e9` | `004dbc5f` | `00973350` (14), `0097333c` (5) |
| `dice.hfe.world.TemplateComponent.ThrownFireComp` | `0x000094de` | `004dbe2a` | `009742e8` (13), `009742d4` (5) |
| `dice.hfe.world.TemplateComponent.ThrownSpawnPointComp` | `0x000094f3` | `004dbe6c` | `009746a0` (13), `0097468c` (5) |
| `dice.hfe.world.TemplateComponent.ToggleCameraComp` | `0x000094e6` | `004db9e9` | `009717a0` (12), `0097178c` (5) |
| `dice.hfe.world.TemplateComponent.VehicleHud` | `0x0000c4eb` | `00552b83` | `0096c258` (5), `00983748` (107), `00983734` (5) |
| `dice.hfe.world.TemplateComponent.WarningHud` | `0x0000c4f7` | `00552c49` | `0096c258` (5), `00984178` (20), `00984160` (5) |
| `dice.hfe.world.TemplateComponent.WeaponBasedRecoilComp` | `0x000094e4` | `004dbae5` | `009722d8` (13), `009722c4` (5) |
| `dice.hfe.world.TemplateComponent.WeaponHud` | `0x0000c4ef` | `00552bc5` | `0096c258` (5), `00983ec8` (93), `00983eb0` (5) |
| `dice.hfe.world.TemplateComponent.WindAffector` | `0x0127bc2d` | `0051ad66` | `0096c258` (5), `0097b1a0` (27), `0097b188` (5) |
| `dice.hfe.world.WindManager.Standard` | `0x0000c35d` | `006f326f` | `0099ad60` (19) |

## RendDX9_ori.dll (31 classes)

### dice.hfe

| Class | Class ID | Create | Vtables (slots) |
|---|---|---|---|
| `dice.hfe.Rend.EnvMapManager` | `0x000c844e` | `10066684` | `101c6378` (15) |
| `dice.hfe.Rend.RenderView` | `0x000c844d` | `10066642` | `101c6610` (40) |
| `dice.hfe.geom.DecalManager` | `0x000c871a` | `100706c1` | `101c1590` (6), `101d67c0` (22), `101d67a8` (6) |
| `dice.hfe.geom.GeometryOptimizer.BundledMeshOptimizer` | `0x870caa39` | `10070805` | `101d6da0` (22) |
| `dice.hfe.geom.GeometryOptimizer.StaticMeshOptimizer` | `0x870caa3b` | `10070847` | `101d6e18` (22) |
| `dice.hfe.geom.GeometryTemplate.BundledMesh` | `0x870ca651` | `10070538` | `101d3f58` (76), `101d3f44` (5) |
| `dice.hfe.geom.GeometryTemplate.MeshParticleMesh` | `0x870ca655` | `10070703` | `101d6898` (75), `101d6880` (5) |
| `dice.hfe.geom.GeometryTemplate.RoadCompiled` | `0x000c8a8d` | `1007090d` | `101d7178` (58) |
| `dice.hfe.geom.GeometryTemplate.SkinnedMesh` | `0x870ca652` | `1007057a` | `101d6210` (74), `101d61fc` (5) |
| `dice.hfe.geom.GeometryTemplate.StaticMesh` | `0x870ca653` | `100705bc` | `101d6588` (76), `101d6574` (5) |
| `dice.hfe.geom.GeometryTemplateManager` | `0x870ca329` | `10089510` | `101d2d40` (25) |
| `dice.hfe.geom.HemiMapManager` | `0x000c871b` | `10070745` | `101d6a58` (17) |
| `dice.hfe.geom.LightGeometry` | `0x3103a73c` | `100707c6` | `101d6d54` (5) |
| `dice.hfe.geom.LightManager` | `0x000d01c1` | `10070472` | `101beaf8` (3), `101c1590` (6), `101d42e0` (157), `101d42d0` (3), `101d42b8` (6) |
| `dice.hfe.geom.NametagManager` | `0x000d01c3` | `10070784` | `101bfb94` (1), `101beaf8` (3), `101d6c80` (33), `101d6c78` (1), `101d6c6c` (3) |
| `dice.hfe.geom.ObjectDrawer` | `0x870caa3e` | `10070a0f` | `101bfb94` (1), `101d4e98` (19), `101d4e94` (1) |
| `dice.hfe.geom.ParticleSystemManager` | `0x000c8719` | `1007067f` | `101d3c78` (18) |
| `dice.hfe.geom.RainManager` | `0x3103a745` | `10070ad5` | `101d7dd8` (54) |
| `dice.hfe.geom.RoadCompiledRenderer` | `0x000c8a98` | `100709cd` | `101d4f08` (10), `101c1590` (6), `101d4fa0` (10), `101d4f88` (6) |
| `dice.hfe.geom.RoadEditableRenderer` | `0x000c8a99` | `1007098e` | `101d7468` (9) |
| `dice.hfe.geom.Roads.RoadEditable` | `0x000c8713` | `1007094c` | `101c0118` (5), `101bfb94` (1), `101d7398` (37), `101d7384` (5), `101d7380` (1) |
| `dice.hfe.geom.Roads.RoadTemplate` | `0x000c8710` | `100708cb` | `101d70e0` (29) |
| `dice.hfe.geom.SkinnedMeshRenderer` | `0x000c8717` | `10070640` | `101c1590` (6), `101d6740` (6), `101d6728` (6) |
| `dice.hfe.geom.SkyDome` | `0x870caa3d` | `10070889` | `101c1590` (6), `101d6ee0` (39), `101d6ec8` (6) |
| `dice.hfe.geom.StaticMeshRenderer` | `0x000c8716` | `100705fe` | `101d4f08` (10), `101bfb94` (1), `101d4ffc` (10), `101d4ff8` (1) |
| `dice.hfe.geom.SwiffRender` | `0x3103a73f` | `10070a93` | `101beaf8` (3), `101bfb94` (1), `101d7ad8` (71), `101d7ac8` (3), `101d7ac4` (1) |
| `dice.hfe.geom.Terrain` | `0x000c870c` | `100704b4` | `101beaf8` (3), `101c1590` (6), `101d4bc0` (60), `101d4bb0` (3), `101d4b98` (6) |
| `dice.hfe.geom.TerrainEditable` | `0x000c870d` | `100704f6` | `101d5678` (100), `101c0118` (5), `101bfb94` (1), `101d5218` (7), `101d5bc0` (74), `101d4bb0` (3), `101d5ba8` (6), `101d5a18` (100), `101d5a00` (5), `101d59fc` (1), `101d59e0` (7) |
| `dice.hfe.geom.UndergrowthSystem` | `0x3103a73d` | `10070a51` | `101d7640` (30) |
| `dice.hfe.geom.Video` | `0x3103a742` | `10070b56` | `101beaf8` (3), `101d7f24` (14), `101d7f18` (3) |
| `dice.hfe.geom.VideoTarget` | `0x3103a741` | `10070b17` | `101beaf8` (3), `101d7edc` (7), `101d7ed0` (3) |
