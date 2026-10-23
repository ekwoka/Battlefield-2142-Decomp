# Engine structure

How `BF2142.exe` starts, runs a frame and loads a level, as recovered so far.
Addresses are 1.51. `src/Main` follows this order. Pieces that aren't
rewritten yet are noted.

## Start-up

`0x004028e0` is the real main function:

1. `0x00401f80` creates the engine object (`BF2Engine`, 0x138 bytes,
   constructor `0x00408b10`, global `0x00a0f3b0`) and initialises it with
   the command line (`0x0040d270`). That loads the settings, the sound module
   (`0x007fcd50`), the HUD and VoIP, then runs `Settings/Sound.con`,
   `Menu/MenuSounds.con` and `Init.con`. `0x00406420` mounts the archives
   (`ServerArchives.con`, and `ClientArchives.con` unless dedicated) and
   registers the menu events.
2. If `GSDemo` is set, it plays the demo.
3. Main loop: pump window messages (`0x004013f0`; when the window loses
   focus this also mutes sound unless `GSDeveloper` is set), then run one
   frame (`0x0040bd90`). The loop ends when the frame returns false.
4. Shutdown (`0x00409160`), then the engine is deleted.

The window procedure (`0x00401570`) forwards mouse and keyboard input to the
active Flash menu (`0x00a0f7a8`) and handles the EA launcher's shutdown
message.

## A frame (`0x0040bd90`)

In order:

1. Without focus, sleep 100 ms.
2. Frame time is measured with the time manager (`0x00a7d060`, slot 0x18 =
   current time in seconds), clamped to 1e-5 .. 1/15 s.
3. **Simulation ticks:** a `TickTimer(10, 30.0)` (`0x00677f50`, advance
   `0x00677f90`) gives the number of whole 1/30 s ticks since the last frame,
   at most 10 (it resynchronises when further behind). Rewritten as
   `src/Game/TickTimer`.
4. The menu, if open, updates (`0x00403120`). The game world advances by
   the ticks (`0x00a1b5d8` slot 0x2c with the tick count and 1/30 s). The
   sound engine updates by the same ticks (sound manager slot 0x18).
5. Network, physics and object updates (`0x00a85220`, `0x00a85228`), the
   camera, then rendering (engine member `+0x18`, slots 0x14..0x1c).
6. Events from the Flash menu (cases 2..0x17: quit, host, join, play a
   sound, log in, ...).
7. On a dedicated server, sleep so a frame takes at least 1/30 s.
8. `dice::hfe::memory::nextVisualFrame()` (`Memory.dll`, rewritten).

`0x0042b800` ("is dedicated server") gates much of this.

## The game object and a simulation tick

The game is a class-manager object: `dice.bf.gameServer` (class `0x1d8aa`,
vtable `0x00928d20`) or `dice.bf.gameClient` (`0x1d8ab`, `0x00929028`),
published as the `Game` singleton (`0x00a1b5d8`). Slot `0x2c` advances it by
a frame's ticks (server `0x004b1c90`): more than 10 ticks count as one
(and are recorded as dropped), then each tick runs `0x004afb20` with
1/30 s. A tick, in order:

1. game logic (`0x00a1c170`) slot `0x20`; its state (slot `0x164`) gates the rest
2. HUD information layer update (not on a dedicated server)
3. `EventManager` (`0x00a7d06c`) slot `0x20`: timed events
4. unless the game state is 4: `ObjectManager` (`0x00a85220`) slot `0x28` (object
   updates), sound, `PhysicsNodeManager` (`0x00a82514`) slot `0x1c` (move),
   `ResponsePhysicsManager` (`0x00a82518`) slot `0x24` (collision response,
   run twice around `0x0042b840`), `ObjectManager` slot `0x128`, then both
   physics managers' end-of-tick slots (`0x20`, `0x30`)
5. `dice::hfe::memory::nextPhysicFrame()`

`symbols/BF2142.exe.singletons.csv` (`tools/decomp/singletons.py`) lists
these globals with their class IDs and classes.

## Level loading (`0x004d3450`)

1. Mount `Levels/<name>/server.zip` (and `client.zip` unless dedicated) at
   the level's directory, then run the level's `ServerArchives.con` /
   `ClientArchives.con`.
2. Run every template script under `Objects/` (`0x004d5440`; order in
   [console.md](console.md) and `src/World/TemplateFiles`).
3. `Common/Material/MaterialManagerSettings.con`, then the level's
   `Init.con` (no arguments; the editor passes `BF2Editor`).
4. `StaticObjects.con`, then the game mode's
   `GameModes/<mode>/<size>/GamePlayObjects.con` with `v_arg1` = `host`
   (server) or `client`.

## Terrain materials

A level's `Heightdata.con` loads the primary heightmap's material map:
`heightmap.setMaterialScale 1` (slot `0x70`, `0x00676620`: `+0xd0`) and
`heightmap.loadMaterialData Levels/<name>/HeightmapPrimary.mat` (slot
`0x74`, `0x006d2f40`: width × height × scale² bytes, raw, to `+0xd4`).
Each byte is the physics material's id (`Material.active` in
`materialManagerDefine.con`: 2 Sand, 3 Gravel, 4 Tarmac, 8 Dirt, 12
rock...), with no table between; the secondary heightmaps have none (0).
Every shipped level uses scale 1, the file the heightmap's size (the
painted map is one sample smaller, its last row and column 0). The lookup
(`0x006cfe40`, slot `0x7c`): x + width / 2, y + height / 2 (integer
halves, unsigned, 0 outside or without a map), byte
`[(width·y·s + x)·s]`; the height query (`0x006d2fc0`, slot `0xb4`) and
the ray (`0x006d3210`, slot `0xc8`) take floor(x / scale.x),
floor(z / scale.z) — the cell's lower corner, laid out as the heights, no
flip — and the ray answers material 1 for a nearer water-table hit. The
cluster's point query (`0x006b5430`, slot `0x80`) and ray (`0x006b5a90`,
slot `0x8c`) pass it on: ragdoll particles (`0x008ac200`, material 1 no
hit), Point projectiles (`0x006c0560`) and RotationalPoint ones
(`0x006c0f90`, grenades) store it at `+0x80`. Rewritten in
`terrain::Heightmap::materialAt` / `HeightmapCluster::materialAt` and
`physics::Ground::material`; vehicles take the material under their
centre for their tyres' grip (the engine's per-wheel material isn't
traced).

## Undergrowth (RendDX9 `UndergrowthSystem`)

Init `0x101100d0` reads `Undergrowth.cfg`, `Undergrowth.dat` (`0x1010fd90`)
and `Undergrowth.raw` (one material byte per heightmap sample). The terrain's
patches (`terraindata.raw` patches per side and patch size) are each split
into `PatchSubdivide`² undergrowth patches, numbered row by row from the
-x/-z corner. Per patch, `0x1010b240` records what fraction of its map
samples hold each material 0-15.

A patch is generated when it comes into view (`0x1010ba40`). The random
generator (`0x10025420`, x · 630360016 mod 2³¹-1 in two steps) is seeded
with the patch index. For each material present, each type walks an N×N
grid (N = round(√density · patch size)); each point draws one number whose
bits give the jitter (bits 0-15), scale (16-21), sway offset (22-25) and one
of 32 yaw steps (26-30). Points whose map sample is not the material, or
whose terrain normal's y is outside the type's range, are skipped. The
instance is rotated, tilted onto the terrain normal and scaled; vertices
are packed as shorts relative to the patch with the ground height separate,
so the shader can grow plants out of the ground with distance. Each patch's
buffer holds the worst case plus a tenth.

Rewritten in `src/Terrain/Undergrowth` (data, generator) and
`src/Render` (`undergrowth.vert/.frag`, after `Undergrowth.fx`).

## Physics of mobile objects

Objects whose template has `hasMobilePhysics 1` and `physicsType Mesh`
get a `PhysicsNode.Basic` (created in `0x005f6ba0` with the template's
mass, centre of mass, inertia and gravity modifiers). Per tick
(`PhysicsNodeManager` slot `0x1c`, node slot `0xf4` = `0x006c9060`):

1. angular step (`0x006c8690`): impulses and torque become angular
   velocity per body axis over `1.5 * inertiaModifier` times a box inertia
   from the collision bounds' size (`3 / (b^2 + c^2)`); damping 0.99; the
   rotation turns about the centre of mass by the tick's average angular
   velocity;
2. linear step (`0x006c8380`): contact impulses (velocity changes, the
   vertical part halved) and accelerations change the velocity (at most
   1500 m/s), damping 0.99, the position moves by the average velocity;
3. gravity (-14.73, `Physics.gravity`) times `gravityModifier` is added
   for the next tick.

Ground contact (`ResponsePhysics.Basic` slot `0x28` = `0x006b9260`) puts
the points of collision LOD 1 (the vehicle LOD) into the world and records
a contact for every point below the heightmap (slot `0x2c`); the solve
(slot `0x30` = `0x006ba7d0`) pushes the body out by the largest
penetration per axis and gives every contact an impulse
`(n + friction * slide) * (1 + elasticity) * share`, the shares weighted
by approach speed. Rewritten in `src/Physics/RigidBody`.

Two more inputs feed the steps. Slot `0xa4` (`0x006c7c00`) adds a force
at a point to the acceleration and its moment to the torque, without
dividing by the mass; springs push with it. Slot `0xa8` (`0x006c7980`)
keeps a running mean of the tick's tyre forces and their moments (`0xec`,
`0xf8`, count `0x11c`), which the steps add to the acceleration and the
torque.

## Ground vehicles (newCar2)

A car is a tree of physics nodes under its body: an `Engine` node and
`Spring` nodes for the wheels, the steering wheels under
`RotationalBundle`s with `inputToYaw PIYaw`. Engine templates with
`engineType 194` (`c_ETNewCar2`) use the newCar2 model. Its settings are
at template `0x760`-`0x7b0` (`newCar2.minRpm` ... `newCar2.wheelLatScale`,
plus `friLatBrakeMod` at `0x79c`; defaults in `0x006291d0`). Per tick:

- Spring (`0x006caef0`): the wheel collides as an object of its own. How
  far the ground pushed it from its rest place, and how fast, pushes the
  parent back by `disp * strength * Physics.gravity * -0.101833 + speed *
  damping` (slot `0xa4`). The wheel is then put back at rest.
- Wheel (`0x006b7910`), for a wheel in contact: it takes the contact's
  velocity along the ground. The engine's torque (`torque * throttle *
  differential * gear ratio * 1.75`, zero unless the throttle points the
  gear's way) and the slope's torque (`0x006b7460`: the downhill part of
  the wheel's quarter of the weight times the radius, times
  `slopeTorqueMod`, none in first gear) spin the wheel. `frictionTorque`,
  plus `engineBrakeTorque` off the throttle and `brakeTorque` while
  braking, slow it (`0x006b73d0`, over `2 * wheelInertia`, per 1/30 s).
  - The slip is the wheel's surface velocity less the ground's. The tyre
    force (`0x006b71f0`) puts its parts along and across the wheel, times
    `wheelLongScale` and `wheelLatScale`, through a curve (`0x006b7160`:
    linear to 1 at `peakAt`, then falling to `minDynamicFriction` at 1).
    It multiplies by the friction mods: the drive mods for wheels behind
    the engine, and `friLatBrakeMod` across the wheel while braking with
    under 1.5 m/s of side slip. It then scales by the surfaces' friction,
    `normal.y^3` and `frictionMod`.
  - The force along the wheel turns the spin back (`radius * force *
    engineFeedbackMod * mass / 4 * -30`). The spin is limited to `maxRpm`
    (rpm = spin * gear ratio * differential * 3.5 * 9.549). The rpm is
    summed for the engine, and the force times 30 goes to the body's mean
    (slot `0xa8`).
- Engine (`0x0064e920`): the wheels' mean rpm, between `minRpm` and
  `maxRpm`, is the engine's normalised rpm. When it is nearly zero, the
  engine changes between first and reverse (gear 0, 1.2 times first's
  ratio) by the throttle's sign. It changes up above `gearUp`. Gear ratios
  come from a 101-sample table read at `gear / numberOfGears`, linear
  between keys at 0.2 ... 1 that `setGearRatios` sets (`0x00629590`,
  `0x0061ecf0`).

- Steering: the `RotationalBundle` axis update (`0x00563bd0`). The debug
  variable `rot-use-old` is 1 (`0x008de9e0`), which selects the branch in
  which an `automaticReset` axis (the FAV's `nav` bundles) moves its angle
  straight to `input × maxRotation` at `|acceleration|` degrees a second
  (100: full lock in 0.32 s); other axes move a speed towards `input ×
  maxSpeed` at `|acceleration|` and turn by it. `snapToZeroOnNoInput` puts
  the angle to 0 when the input drops under 0.05 towards its side;
  `capMaxSpeed` and `maxSpeedMod` cap the target; the limits clamp it.
  The values are per second as written: the getters (`0x006147c0`,
  `0x00613ca0`) only scale by 30 while `0x00a73a1c` is 0, and it is 1.
- Braking (`0x0064e810`, dead zone 0.04 for newCar2): the throttle
  against the way the engine turns, or no throttle while it is nearly
  still.
- The clutch and gears (`0x0064e920`): the clutch starts out and comes in
  at `2 / gearChangeTime` a second; it goes out as fast while a change is
  under way or while braking, and a change takes effect once it is out
  (under 0.05). Braking with the clutch out and the rpm under 0.1 selects
  first. Up above `gearUp` with the clutch in (over 0.95), except while
  climbing (nose over 0.174, `0x00a78ffc`) and moving forward; down when
  the rpm in the lower gear would be under 0.55. Nearly still, first and
  reverse change by the throttle (under −0.5, over −0.05). The rpm is the
  wheels' blended with the old by the clutch, and an rpm the throttle no
  longer holds decays (×0.9 braking, then ×0.96 without throttle or ×0.99
  with: `engine-inertia3`, `engine-inertia2`). The engine's torque (slot
  `0x1a4`) is scaled by the clutch only with `useClutchedTorque`.

Rewritten in `src/Game/Vehicle` (`rotationAxisStep`,
`Vehicle::updateEngine`).

- **Wheels off the ground:** there is no separate branch. The wheel node
  (`0x006b8230`, vtable `0x00992cb8` slot `0x34`) hands newCar2 wheels to
  `0x006b7910` before its own contact test, so the driven-wheel code runs
  every tick with the contact sums at zero: no normal, the ground
  velocity gravity's tick alone. The tyre force (scaled by the normal's
  y) and the feedback are 0; the engine torque and the slope torque (all
  of the weight along the wheel, `0x006b7460`) still turn it, through the
  same friction, engine-brake and brake torques; its rpm still counts in
  the engine's mean. The Spring node then multiplies the spin by
  `*0x00a84c20` = 1.0 for newCar2 (0.99 is the other engine types', 0.9
  in water).
- **Wheel radius** (`0x006cad60`): the height (max y − min y, the box's
  `+0x28` set by `0x006aeda0`) of the wheel object's collision box at
  LOD 0, plus the object's slot `0x94` (0 for the base Object), at least
  0.1. For as_fav's wheels that is 0.97 m, the wheel's full height; the
  spin, slope and feedback torques use it (the FAV's top gear then tops
  out near 35 m/s, its AI `maxSpeed` being 30). The rewrite keeps the
  depth of the vehicle-LOD triangle (0.485 m) for its own ground probe.
- **Objects against objects** (`src/Game/MeshCollision.cpp`): see the
  next section.

### Mobile meshes against objects (0x006dc7b0, 0x006bb050)

Each moving object with a vehicle LOD is a mover in turn (`0x006dc7b0`).
Its candidates are the objects' parts in the grid near it, not of its
own tree; a candidate that was a mover earlier in the frame is left out,
so each pair is met once. A pair goes on if:

- their origins are within `(|vM| + |vC|) * dt + rM + rC`, `r` being the
  vehicle LOD's radius (the mesh's slot `0x50`, `0x006de7d0`: the LOD's
  `+0xc0`, the farthest of its box's corners from the origin,
  `0x006aeda0`; `physics::boxRadius`);
- the smaller one's sphere (its radius plus the motion) touches the
  other's LOD box (`*0x00a8510c` = 1, `0x006aecc0`:
  `physics::sphereTouchesBox`);
- while the motion is under both radii and both LODs have over 10
  vertices (`*0x00a85104`), the two boxes overlap (`*0x00a850f4` = 1,
  `0x006aef70`, a separating-axis test: `physics::boxesOverlap`);
- not both have under 4 vertices.

Then the radii choose who goes into whom (the response's slot 9,
`0x006bb050`, with A's mesh into B's): with `rM / rC` from 1/4 to 4, C
into M and M into C; over 4, C into M, and M into C too when both are
mobile (else, C a root and M with a type-4 LOD: M's third pass alone into
C); under 1/4, M into C (and with M a root and C with a type-4 LOD, C's
third pass alone into M). So a vehicle's vertices are not tested against
a static object over four times smaller than it: that one's vertices go
into the vehicle's mesh instead.

Slot 9, A into B (nothing unless A has a vehicle LOD and B the LOD A asks
for; segments go through B's mesh by its slot 22, `0x006e5920` →
`0x006de850` → `0x006e6530` → `0x006e62c0`: the faces crossed from the
front only, `0x006bd070` with the two-sided flag clear, the earliest of
them; the contact's depth is how far the end lies behind that face,
`CollisionWorld::segmentContact`):

- **The motion:** `d = (vA - vB) * dt`, the nodes' average velocities
  this tick (node slot `0xb4`, `+0x44`: the mean of the old and new
  velocity that moved the position, `RigidBody::averageVelocity`). A's
  centre of mass a tick ago is `last = A's origin + R·com - d`.
- **The small-object pre-trace** (`0x6bb47f`): a mobile A whose LOD radius
  is under 0.3 (`0x00975eb8`) traces from `last` to its origin; a crossing
  moves A back along that way by how far it went past the face (node
  slot `0x64`), and shortens `d` by as much. Shipped meshes under 0.3:
  rocks, wreck and destruction debris, missiles, small handhelds.
- **The helicopter exit** (`0x6bb5b9`–`0x6bb787`): a root
  PlayerControlObject (interface `0xc4c5`) against an object whose root
  has the non-mobile response (`0xc430`), on the server (`gameLogic` slot
  `0x144`), whose first Engine (`0x006b6c50` → `0x005c63b0`: a node, its
  siblings, then their children) has engineType `c_ETHelicopter` (33;
  cached at `+0x110`): its origin's segment from its last stored matrix to
  now through B's LOD; a crossing ends slot 9 for the pair, notifying both
  with material `0x29a`, which PlayerControlObject's slot 43
  (`0x005c72f0`) turns into 1e10 damage (`0x004c0e40`). Shipped: eu_at,
  as_at (TailEngine), us_ag, as_ag (TailThruster), as_tank and
  as_hov_light (TurnThruster). `Body::helicopterEngine`. The same
  PlayerControlObject root against a static's root also enables the edge
  pass.
- **One point** (`local_25`): a static A with at most 20 vertices
  (`*0x00a84258`), no type-4 LOD and a radius under half of B's is tested
  as its origin only: each of its vertices runs the origin's way from
  `last`. A Ladder (class 40000) ends slot 9 instead.
- **Pass 0, the vertices** (each turned by A's rotation, kept for pass 1):
  a mobile A's from `last` to the vertex now, a static A's from the vertex
  now less `d` (plus R·com) to the vertex now. So a moving A's vertex
  already behind a face still meets it while the centre of mass is in
  front of it.
  - *The vertical probe* (`0x6bbbaa`–`0x6bbdb2`): a part whose vehicle LOD
    has under 4 vertices (the wheels', springs' and landing gear's three
    vertices at one height; 73 shipped parts) runs, for each vertex i, a
    segment 2h down its up axis from its origin raised by h
    (h = |v0.y|), for i 1 and 2 raised 0.1 more and 0.1 forward or back; a
    part 0.5 m or more to a side of the root's centre starts 0.5 m
    inwards and ends where it would have; `d` is zeroed. A static part of
    under 4 vertices then has no way at all. Rewritten for
    `Vehicle::Wheel`s as movers of their own (`GameWorld::wheelProbes`:
    h is the wheel's `radius`, the part at its mount and compression,
    turned by its steer; its radius its geometry's LOD radius).
- **Pass 1, the edges** (A a PlayerControlObject root, B's root static,
  `*0x00a84214` = 1): the edges of A's LOD-1 faces no steeper than 30° off
  the vertical (|n.y| ≤ 0.866, `0x006e1430`, kept at load in
  `Body::edges`), at the current pose, through B's mesh from either end
  (`CollisionWorld::edgeContact`). Crossed from its first end only: a push
  at that crossing along the face's normal; from both: at the middle of
  the two crossings, along their normals' sum flattened (y 0) and
  normalised, or from the crossing towards A's origin when that sum is
  under 0.1 (`*0x00a84230`). The push is a force of 3 (`*0x00a8423c`) at
  the point (node slot `0xa0`, `0x006c7b10`: an acceleration and its
  torque about the centre of mass, no contact).
- **Pass 2, the trunks** (`0x6bb90f`–`0x6bba92`, `0x6bbdf8`–`0x6bc735`; B a
  root): A with a type-4 LOD (trees, lamp posts, poles, billboard
  frames, cover: 164 shipped meshes, all statics, all with a vehicle LOD
  too), or with a radius (`treeCollisionDiameter` / 2, the node's
  `+0x198` set by `0x005f6ba0`) and a vehicle LOD: then one face, a trunk
  line up the LOD box's middle across x at z 0: `(cx, min y, 0)`,
  `(cx, max y, 0)`, `(cx + 0.05, min y, 0)` (`local_114`..`local_2a8`).
  Shipped with a radius and no type-4 LOD: `brick_pillarjba` (jbcustom3,
  a static); the handheld Beacon (`treeCollisionDiameter 0.7`) has its
  mesh's LODs only in geometries 1-3, geometry 0's two parts being empty. Each face crossing the plane through B's centre of mass
  across B's up axis (edge v0-v1, else v0-v2) gives a point X; a half-ray
  from X - d towards B's centre of mass (plus d) enters B's LOD box
  (`0x006ae680`) at P through a face whose outward normal, flattened and
  turned round, is n; with dist = n·(P − X) within the radius (0.3 under
  0.01 with a type-4 LOD), a contact at P along n, r − dist deep, when B
  comes towards it (v·n ≥ 0 below).
- **Each contact** (`0x6bc764` on): the relative point velocity
  v = vA − vB at it (node slot `0xac`); the shares (`0x006b6cc0`,
  `physics::massShares`); then A's response gets (point, v × its share, n,
  depth × its depth share) and B's the same with B's (slot `0x2c`,
  `0x006ba4c0`): the friction of the cell of A's vertex material (the
  mesh's slot `0x44`, `0x006de710`: a material index per vertex,
  `CollisionLod::vertexMaterials`) and B's face's, the elasticity their
  mean. The solve's approach weights (`0x006ba7d0`) are never negative,
  so B's side, whose velocity share is negative, only pushes out.
- **The shares** (`0x006b6cc0`): a major actor (`isMajorActor`, template
  `+0xf0` → node `+0x24a`, slot `0x1c4`) is not moved by a minor one (the
  other takes 1 or −1, waking it when asleep unless fixed); a sleeping
  node (slot `0x14c`) is not moved; two awake ones share by mass,
  A taking mB / (mA + mB) and B that less 1, kept within 0.05..0.95
  (`*0x00a84240`) unless one is a Particle (class `0x947b`). Then (not
  for Particles) a sleeping A (slot `0x158` 0) not fixed, hit with
  −mB·(v·n) over mA·1 (`*0x00a84248`), wakes (slot `0x160`) and takes the
  mass share; else a sleeping B likewise. Finally, with |v·n| over 10
  (`*0x00a84278`) and neither fixed, the depth shares are 0.5 and −0.5.
  A static object takes nothing, the mobile one everything.

Not modelled: the collision callbacks each contact calls on both objects
(object slot `0xac`: the game logic's collision damage and its veto of the
response), the effects (`0x006b75d0`), sleeping (no node sleeps here, so
the shares' sleeping terms never apply), the collision groups (response
slot `0x50`), a wheel's own part as B (a static's vertices into its
three-vertex LOD), and the wheels' contacts go to the vehicle's response,
not the wheel's node (whose spring reads the ground only).

### Seats and vehicle weapons

A vehicle's seats come from its template tree. Each `PlayerControlObject`
with `seatInformation` (seat name, position, rotation) is a seat, the root
first: the driver. Each seat has an exit (`setSoldierExitLocation`) and
may have a gun: the `GenericFireArm` in its subtree, aimed by the
`RotationalBundle`s that take the mouse. The yaw bundle has `inputToYaw`.
The pitch bundle has `inputToPitch`, limited by its `minRotation` and
`maxRotation` (rotation y, down positive). Using the FAV as the example:

- the root, `eu_fav`: the driver;
- `EU_FAV_SecondPosition`: the gunner, with `EU_FAV_MachineGun_Base` for
  yaw, `EU_FAV_MachineGun` for pitch (−30°..15°), and
  `eu_fav_MachineGun_Firearm` (`ammo.magSize -1`: no magazine; it heats up
  instead);
- `EntryPoint` children (`entryRadius 3`) at the front, the back and the
  top.

A soldier inside an entry radius enters the first free seat. F1..F8 switch
to a free seat. The driver steers and the others aim and fire; the gunner's
camera follows the gun, and the renderer turns the bundles through
`partPose`. Leaving goes to the seat's exit.

Seated soldiers are posed by the seat's `seatAnimationSystem` (for
example `3p_as_fav_driver_animSys.inc`, whose `root` trigger plays the
`still` bundle and whose `die` trigger the death: `die` fades in over
0.4 s and plays forever). A soldier killed in a seat of a vehicle that
still stands leaves by the seat's "occupant died" exit (interface
`0xc4c5` slot 10 → `0x005cea00` with died = 1): the seat's system plays
`die` (`0x008b9790`), the soldier keeps it as its dead seat (`+0x244`,
`0x005c7790`; `0x005ed890` sets `+0x2ea`) and is posed by it from then on
(`0x005ef250`), slumped in the seat with no ragdoll. A seat without a
die bundle ejects the body at its exit with a ragdoll pushed forward by
`rd_dieInVehicleForce` (100); the rewrite's bodies don't outlive the
player's death, so that one goes at once. In a destroyed vehicle the
occupants die and are deleted (`0x005cacf0`). The rewrite keeps the
slumped body (`GameWorld::seatCorpse`) until the vehicle is wrecked or
gone or someone takes the seat; how long the engine keeps it is not
traced. The soldier is placed at the seat's `seatInformation` position and
rotation in the vehicle's frame, with its skeleton a metre below that
point. The engine keeps a soldier's object at its physics origin, a metre
above the skeleton's root (at death `0x005f0a00` poses the skeleton at
0/−1/0 from the object). A seat without a seat animation system (closed
cockpits) keeps its soldier hidden, and a seated soldier's handheld weapon
is put away. Rewritten in `GameWorld::poseSeated`.

Rewritten in `GameWorld` (`vehicleLayout`, `seatPlayer`, `stepGunner`). Child
placements turn their offsets by their parents' rotations, as the wheels'
do. Gun heat isn't modelled yet. The vehicle HUD gets its
icon, its armour, the gunner's `GuiIndex` (from the firearm's
`weaponHud.guiIndex`) and the turret's angle. Compass nodes scroll a strip
texture by a heading variable (`VehicleAngle`, `MinimapDelayedMapAngle`).
They are BF2142.exe's `Bf2CompassNode` (render `0x007e9560`; RendDX9.dll
only draws the quads): the strip (`setCompassNodeTextureSize`) sits at the
node's corner, slid back by `floor(value / 360 * (length - border) +
offset)` pixels (along y for direction 0, x for 3; the exe's radians are
scaled by 57.29578, `0x00a0d138`) and cut to the node. Wrapping nodes
(`createCompassNode`'s first flag) take headings in [-180, 180) (as the exe
wraps the map angle, `0x0078fc80`) and draw a second copy a period away to
fill the gap; snapping ones (the weapon HUDs' movement arrows) stop at the
node's edge less the snap offsets and swap in the snap textures; tilt
values slide the strip sideways.

## Animation systems

Soldiers, weapons and seats name animation-system scripts
(`animationSystem3P`, `animationSystem1P`, `seatAnimationSystem`). A
script creates clips (`createAnimation`: a `.baf`, 24 frames per second
(`0x008c1c..`), with `animationManager.looping/length/fadeInTime`),
bundles of clips (`createBundle`; defaults from `0x008bf660`: looping,
fade in 0.2 s, fade out 0.1 s) and a tree of triggers (`createTrigger
<type> <name>`, `addChild`, `addBundle`). Value holders (`values min normal
max`, from `ValueHolders.inc`) give the range in which a trigger fires and
the "normal" value at which its bundles play at their own speed.

Each tick (`0x008baed0`) the object's system evaluates `startup` once, then
`root` and `postRoot`, with the object's state: pose index, message bits
(1 fire, 2 reload, 4 zoom...), the movement direction in the object's
frame, the speed, the turn rate and the vertical speed. Evaluating a
trigger (vtable slot `0x48`) tests it (by type: the pose picks a child,
Movement the speed, Forward speed times the forward part of the direction,
Side the sideways part, Turn the turn rate, Up the vertical speed,
Message the message bits), evaluates its children and queues its bundles
(slot `0x4c`) with a playback rate (value / normal). Movement bundles of
four clips blend a strafe clip and the forward or backward one by
`asin(|forward|) * 2/pi`. `addAnimation` puts each clip first, so a bundle's
clips are indexed from the end.

Queued instances are kept by priority, a trigger's place in a pre-order
walk of its tree, the top-level triggers taken by name (`0x008c0f50`).
Each tick advances them (`0x008c3360`): time wraps for looping bundles and
ends the others, and the weight is the square of a ramp over the fade
times. Looping bundles that were not queued again fade out, and finished
ones are dropped. Instances below an IdleTrigger are also dropped when the
messages change or something is queued above them.

The pose is built per bone (`0x008b8530`): the first instance that moves
the bone sets it, and each later one slerps towards its own by its weight.
A weapon's system goes after the soldier's, so its arm and weapon clips
lie over the body. Bones nothing moves keep their last pose. The held
weapon's bundled-mesh parts hang from the skeleton's nodes `mesh1`,
`mesh2` and so on, which the weapon's clips animate.

Rewritten in `src/Animation/AnimationSystem` and `AnimationConsole`.

## Ragdolls

A dead soldier's body is a ragdoll: particles on a few skeleton bones,
held together by constraints, which then pose the skeleton. The code is
`BF2/Animation/BoneAnimation/RagDoll.cpp` (`0x008a7900`–`0x008ae4e0`). One
template, the RagDoll manager (`DAT_00b4cb94`, built by `0x008adac0`), is
filled by the `ragDoll.*` console methods in
`Objects/Soldiers/Common/Animations/ragDollInit.con` and its
`ragDollConstraints.inc`:

- `addParticle bone mass` (`0x008abe00`): 13 particles on the hips (1, 6),
  knees (2, 7), feet (4, 9), shoulders (15, 31), elbows (16, 32), wrists
  (19, 34) and head (47). Knees get `ragdoll_kneeBoneSize` (0.05 m) as
  their radius, hips and shoulders `ragdoll_bodyBoneSize` (0.12 m), the
  rest `ragdoll_boneSize` (0.1 m). The right hip's particle is the middle
  one: where the body is.
- `addConstraint a b` (`0x008ab1c0`): a fixed distance, measured on the
  setup skeleton named by `ragDoll.skeleton` (`0x008a7b70`).
  `addDistanceLessThanConstraint a b d` (`0x008ab140`) keeps two particles
  at least `d` apart (the wrists from the hips).
- `addAngularConstraint a b c degrees` (`0x008aae60`) turns the angle at
  b into a distance a–c by the law of cosines: a minimum, or a maximum for
  a negative angle. The same triple again replaces the limit, so the
  file's pairs such as `1 2 4 110` then `1 2 4 -150` leave only the
  second. `addDihedralAngleConstraint a b c d degrees` (`0x008aafb0`)
  keeps the arms a–b and c–d from folding together: their sum must be at
  least `sqrt(ab² + cd² − 2·ab·cd·cos)` long.
- `addCapsuleCollision a b` (`0x008ab250`): segments that collide with
  objects.
- `lockBone from to` (`0x008aadf0`): bones (fingers, face, the weapon's
  mesh nodes) that keep their pose relative to their parents.
- `toSkeleton bone a b` (`0x008abbd0`): how a bone's matrix is rebuilt
  (below).

The `ragdoll_*` variables are registered by static initialisers at
`0x008ffa00`–`0x008ffe60`. `ragdoll_dT` is 1/90 s, `ragdoll_iterations`
2, `ragdoll_sleep` 2.5 s, `ragdoll_sleepDelta` 0.02 m,
`ragdoll-max-inherit-speed` 40 m/s and `ragdoll_expRotateForce` 13.
`ragDollInit.con` sets `ragdoll_slowMotion` to 0.75 and
`ragdoll_gravityOverTime` to 0.1 s. The manager's gravity
(`ragDoll.gravity`) defaults to −30 m/s².

At death (`0x005f0a00`) the soldier poses its skeleton a metre below its
object and starts an instance (`0x008adc00`, `0x008aca60`). Each particle
sits at its bone, and its previous position is 0.015 s of the body's
velocity behind (the velocity is clamped to the inherit speed). If an
explosion killed it, the explosion's push, kept by `0x005ee4d0` times
`rd_expForceMod`, goes to `addForce` with bone −2 (`0x008aa1c0`). A
negative bone pushes the legs against the body's normal and the rest
along the push, both times `ragdoll_expRotateForce`, so the body turns
over.

A frame (`0x008add10`) scales its time by `ragdoll_slowMotion` and runs
`round(time / dT)` fixed steps. A step does three things:

1. **Forces** (`0x008a8a10`): force / mass, plus gravity times a ramp that
   reaches 1 after `ragdoll_gravityOverTime`.
2. **Verlet** (`0x008a8860`): the displacement `(x − x_prev) + a·dT²` is
   averaged with the previous step's before it is added. This damps the
   motion.
3. **Constraints and collisions** (`0x008ac8f0`), `ragdoll_iterations`
   times: angular (`0x008a8f30`; a violated limit is followed by putting
   a–b and b–c back to length, with the inverse masses mixed as the
   original mixes them), dihedral (`0x008a92f0`), the legs' bend
   (`0x008aa360`; only `legPointForward` is active, at 0.01), the minimum
   distances (`0x008a8db0`), the distances (`0x008a8c40`, by inverse
   mass), then collisions (`0x008ac200`). The collisions keep the shins
   from crossing (`0x008a8ac0`). Each particle is then tested against
   objects, and failing that the terrain, with a segment from its
   previous position to a radius past its new one. A particle moving into
   a face is put back so the segment's end lies on the face, and its
   previous position is set to the new one: it stops dead, which is the
   ragdoll's friction. A particle that starts under the terrain is lifted
   1 % of the way a pass. Last, the capsules are pushed out of objects
   (`0x008abf20`).

Once the middle particle moves less than `ragdoll_sleepDelta` a frame for
`ragdoll_sleep` seconds, the instance sleeps. Asleep, every 1/30 s it
rebuilds its object list and wakes (`0x008a79d0`) on the first object
whose physics isn't at rest and moves at 0.001 m/s or more. `addForce`
also wakes it, but only the first force counts (`+0x1c` is never
cleared), so a later explosion's push neither moves nor wakes a settled
body. Each awake frame, whatever lies under the middle particle (a ray 2 m
down, `0x006f2250`) carries every particle and its previous position by
its point velocity × 1/30 s (`0x008add10`).

**The object list** (`0x008ab6b0`, each solve): the objects with
collision for soldiers whose physics flags lack `0x10` and whose soldier
LOD box comes within `ragdoll_completeCollRadius` (1.5 m) of the middle
particle (`0x006aec10`): static objects and vehicles. A particle's
segment goes through each in list order (collision-mesh slot 22 at LOD
2); the first face it moves into stops it. The capsules (`0x008abf20`,
with `ragdoll_useCapsuleCollision`) go through each object's slot 25
(`0x006e2ee0`, per face `0x006be990`), which tests the segment a→b with
a's radius against the faces' edges only: for each edge, where the
segment comes within the radius (`0x006bde70`), two crossings giving
their midpoint, one taken when earlier; the contact's normal runs from
the edge to it, its depth is that distance less the radius, and a normal
pointing down is turned up without a push. Every contact moves both ends
out and stops them.

**Impact sounds:** `setParticleCollisionCriteria` (template `+0xbc`
minimum speed, −1 for none; `+0xc0` minimum delay; ragDollInit.con: 0.05
and 1) and each hit (`0x008a7a20`): the particle's speed into the face is
its last step's displacement against the normal (metres a step), counted
when the delay has passed since the last sound; the hardest waits. The
soldier's tick takes it (`0x008a79f0`) and plays the material manager's
sound for it (`0x007fd780`): the material manager's slot `0x74`
(`0x0060a6d0`) takes `0x157c` as a material, 5500 (Soldier_FootSteps),
against the surface's: cells are keyed `a × 10000 + b` (`0x0060ad20`),
and the sound is the cell's template 4 (`S_Soldier_RagDoll_Impact`),
from the cell against material 0 when that one has none. Corpses play
theirs too.

**ragdollLandedDelay** (Soldier template `+0x3bc`, 4): `0x005eda20` says
the body has landed when it has no ragdoll, or a particle has touched
something (`+0x63`) and `ragdollLandedDelay` seconds have passed since
the death. The armour's corpse countdown (`0x00652380`) waits for it (or
60 s); on the server a GameLogic slot `0xf4` call in `0x0058d350` waits
too (not identified).

The pose (`0x008acde0`) is relative to the middle particle, in world axes;
the soldier's object sits there, unturned. Each `toSkeleton` entry
rebuilds one bone, in file order:

- **A bone with a particle:** placed at the particle, its y axis from
  particle a to particle b.
- **A bone without one:** placed along a→b at its setup distance from a.
  Negative indices name bones whose matrices are already built, and with
  them the bone stays where it is and is only turned, so the spine is
  aimed bone by bone (`0 0 -11`, `11 -11 -12`...).
- **The x axis:**
  - legs: the leg's plane (hip, knee, foot);
  - hips, spine and head: −(left hip − right hip);
  - collars: ±the body's normal (`0x008a9ae0`: hips and right shoulder);
  - arms: the arm's plane crossed with the bone.

  The axes are then made orthonormal: z = x × y, y = z × x, x = y × z
  (`0x006ac530`).
- **`a == b`:** the bone keeps its local rotation under its parent.

Locked bones follow their parents.

Rewritten in `src/Animation/RagDoll` (`RagDollTemplate`, `RagDoll`,
`loadRagDoll`; `AnimationSystems::ragDoll` loads and keeps it) and
`GameWorld` (`startRagDoll`, `stepRagDoll`). What is open:

- Nothing in the executable names `ragDollInit.con` (it names only
  `ragDollConstraints.inc`, which `0x008a8650` writes), and no shipped
  `.con`/`.inc`/`.tweak`/`.py` includes it, so what runs it is not
  determinable. `GameWorld` loads it from the soldiers' animation
  directory.
- The explosion's push is the blast's (`0x005a66e0`, see Explosions):
  `0x005ee4d0` keeps it for 0.5 s and the ragdoll's start applies it
  (bone −2) times `rd_expForceMod`.
- The object list: the static objects are one merged mesh in the
  rewrite, first in the list; the bodies (vehicles) follow, each in its
  own frame. The terrain's material under a particle is the level's
  material map's (see Terrain materials); deep water (material 1) is no
  hit.
- The impact sounds are queued (`GameWorld::takeSounds`) for the sound
  layer. `GameWorld::bodyLanded` answers `ragdollLandedDelay`, which the
  corpse countdown waits on (see Final death).
- The level of detail by distance to the camera (`0x008ae120` /
  `0x008add10` / `0x008ac200`, `RagDoll::setLevelOfDetail`,
  `anim::ragDollLevels`): past LOD 0 a step is dT × 1.5; LOD 2 also skips
  the leg collisions, capsules and the object pass for the particles of
  bones 2, 7, 16, 32 and 47. LOD 2 is only for network-driven bodies
  (`+0x61`): the frame updates the bodies nearest first, reading the
  performance counter (`0x004423f0`) before the first; a network-driven
  body whose turn comes after `ragdoll_clientMaxUpdateTime` milliseconds
  (0.002, `0x00b4cc10`) takes level 2. With 2 µs, in practice every
  network-driven body after the first updated one. `GameWorld::setCamera`
  gives the camera; bodies behind it count three times as far;
  `GameWorld::setClock` the counter (the steady clock by default).
  A client's bodies are all remote ones, which `stepRemoteBodies` updates
  in that order.

**The network side.** The Soldier networkable's writer (`0x005fb8c0`)
sends mask `0x10040` (health and the ragdoll) while the body is awake and
its player still attached (`+0x184` slot `0x18`, soldier `+0x210`), else
clears `0x10000`. The group (`0x008a9ce0`): a 9-bit length, then the
networked particles (`+0x54`, set at start by `0x008aca60` from
`ragdoll_net_boneEnable`, 15: bones 1, 6, 15 and 31, the hips and
shoulders), the first as three floats and the others as delta3 of 0.01
from it (`0x006879d0`). The reader (`0x008acce0`) reads by its own flags
(the length is skipped): each target goes to `+0x30`, and the first update
puts the particles there; it sets the network timer (`+0x5c`) to 0.3,
network-driven (`+0x61`), wakes the body and clears `+0x62` (no update
yet). A client starts a network-driven body at the death for a soldier
with a player (`0x005f0a00` → `0x008acc00`); a packet with no body is
skipped (`0x008a7aa0`). Driven, a networked particle's step (`0x008a8860`)
moves it `ragdoll_net_interpolate` (0.1) of the way to its target, and the
solve (`0x008ac8f0`) holds them through every pass; the others simulate
round them. The frame (`0x008add10`) does nothing before the first update
and, awake, counts the timer down: at 0 `setNetworkDriven(false)` puts the
networked particles at their targets once, solves, and the body is its
own. `ragdoll_net_debug` is never read. Rewritten in `RagDoll`
(`setNetworkDriven`, `receive`, `networkedPositions`), the decoder
(`NetworkableState::ragdollParticles`, the body's position the middle
particle), `RemoteObject::ragdoll*` and `GameWorld::driveRemoteRagDoll`
(the body started on the first update from the setup pose at the ghost).

**A client's corpse countdown.** The client runs the armour's countdown
from the networked health as the server does: the reader (`0x005fcdb0`)
sets the hit points (8 bits of `maxHitPoints`) through the armour's setter
(slot `0x38` → `0x00655460` → `0x00654120`), which at 0 or less makes the
armour a wreck at the client's own time (`+0x15`, `+0x60`), and the
armour's update (`0x00654d80` → `0x00652380`) has no server test before
the removal: the countdown begins once the body has landed, no player is
attached (the PlayerControlObject's slot `0x10`) and `timeToStayAsWreck`
has passed (or after 60 s), and the soldier fades over
`timeToStayAfterDestroyed`. Only the removal (`0x00651db0`) is behind the
server test (`gameLogic` slot `0x144`): a client's body stays, faded,
until the server removes the ghost. Health back above 0 (a revive) makes
it whole (fade 1, the times cleared). `GameWorld::setRemoteHealth`.

## Server browsing (GameSpy, via OpenSpy)

The game finds servers through GameSpy's master servers. BF2142's GameSpy
name is `stella`; its secret key, `M8o1Qw`, is put together at run time
(`0x00574510`). The master's host is `stella.ms<N>.gamespy.com`, with N =
hash(name) % 20 (`0x00723450`: `h = tolower(c) + h * 0x9ccf9319`). The
OpenSpy patch replaces the domain with `openspy.net`, which gives
`stella.ms5.openspy.net:28910`.

The list request is sent over TCP. In order: a big-endian length, request
type 0, protocol 1, encoding 3, the game version, the game name twice, an
8-byte client challenge, a filter, the wanted fields as `\\a\\b`, and the
options (1 sends the fields of every server).

The reply begins with a header: `byte0 ^ 0xEC` + 2 is its length, and its
last byte `^ 0xEA` is the length of the server's challenge that follows.
Both challenges and the secret key make the key (`0x00723890`) of
GameSpy's "GOA" stream cipher (`0x00737ed0`, `0x00737f80`), which decrypts
the rest. The decrypted reply holds, in order:

- our public IP and the default query port;
- the field definitions (type and name);
- "popular" strings that server entries refer to by index;
- the servers: flags, IP, optional port, private and ICMP addresses, and
  the field values;
- an end marker, IP `255.255.255.255`.

Rewritten in `src/Net`.

## Game connections

Client and server talk over UDP (port 17567 by default) in datagrams of at
most 1472 bytes. Each datagram is a bit stream packed from the lowest bit
of each byte up (`0x006871b0` reads, `0x00687290` writes). It starts with a
4-bit packet type and an 8-bit connection id (`0x006aa930`).

Connecting takes four steps:

1. The client sends type 1 (`0x0068dd00`): protocol `0x1002`, build
   `0x1a007000` (`0x00423320`), the anti-cheat's bit (always 0: PunkBuster is
   not part of the rewrite), a 4-byte key, the password
   (32 bytes) and the mod directory (32 bytes).
2. The server checks them (`0x00689d30`). It may refuse with type 3: a
   reason (password `0x11`, banned `0x16`, full `0x1e`, different mod
   `0x24`...) and an optional message.
3. Or it accepts with type 2 (`0x00689400`): the connection id, its clock
   and the anti-cheat's bit, which the rewrite ignores.
4. The client confirms with type 4. Type 5 disconnects, with a reason.

Connected packets (types 7 ping, 8 pong, 15 data) carry a Tribes-style
"notify" header (`0x006aa980`): the sender's sequence (6 bits), the last
sequence it received (6 bits) and a 32-bit mask of which earlier ones
arrived. A ping carries the sender's clock, and the pong echoes it with
the replier's clock (`0x0068e050` → `0x006a9c30`). Data payloads start on
the next byte with a 16-bit length and are queued for the game
(`0x006a9cd0`). The game's `ServerConnection` (`0x0057b3f0`) reads them.
PunkBuster is not part of the rewrite: servers that require it are not
joinable, and its traffic and menu options are left out.

### Data payloads

A payload is read by the connection's three subsystems in order
(`0x0057a170`; the constructor `0x0057b3f0` makes them):

1. the input block (`0x00590990` reads, `0x00590740` writes);
2. the event block (`0x006447b0` reads, `0x00644bf0` writes);
3. the ghost block (`0x00581290` reads).

A side with nothing for a block writes a single 0 bit: servers send no
input, clients no ghosts.

**Input** (client to server):

- 1 bit present, 4 bits the move count, 9 bits the client's view delay
  for lag compensation (see Ghost state);
- with moves: the first move's index (a sign bit and 31 bits, the form
  `0x0050a460` reads), then the moves.

A move (`0x00590570` reads) holds:

- six axes, each a sign bit and 15 bits: the player inputs 0-5 (yaw,
  pitch, roll, throttle, mouse look x and y) times 100, clamped
  (`0x005837d0`);
- 32 + 11 trigger bits: bit `id - 8` for each input in the table at
  `0x009653a8` that is above 0.5 (fire 8, action 9, use 10, sprint 13,
  weapon 1-9 14-22, seat 1-8 23-30, alt fire 31, reload 32, lie 37, crouch
  38...; the ids are the `c_PI...` constants);
- 1 bit, PlayerInput `+0x110` (unknown).

The client keeps its last three moves and sends them all each time
(`0x00590450`). It writes no input until it has a player: `0x00590740`
writes the lone 0 bit while its player (`+0x84`) is unset, and the
server's reader hands moves to the connection's player
(`0x005904b0`). One live server stopped answering after getting moves
before the player existed and didn't take a new connect a minute later.

**Events** (both ways): 1 bit present, 8 bits the count, 5 bits the block
sequence, 1 bit set when the block is a resend, then the events, each a
7-bit type and its fields. The receiver keeps blocks by sequence and
handles them in order (`0x006447b0`). The sender keeps a block until the
notify layer reports the packet that carried it: delivered, it is dropped;
lost, it goes again with the resend bit. At most 15 blocks are in flight
(`0x00644bf0`).

The event types register at `0x00a3ba44` (`0x005094f0`) with a factory
each. Each event class has the reader in vtable slot 10, the writer in 11,
the client's handler in 8 and the server's in 9. The names come from a
table built at start-up ("ChallengeEvent", "CreatePlayerEvent"...). All
69 layouts are rewritten; the ones a client uses:

| Id | Event | Fields |
|---|---|---|
| 1 | Challenge | 10 bytes challenge, 8-bit length + bytes |
| 2 | ChallengeResponse | 73 bytes response, the mixed value, the build, the profile id |
| 3 | ConnectionType | 0-6 in 3 bits |
| 4 | DataBlock | 1 bit start: type and size (32 bits each); else 8-bit length + up to 63 bytes |
| 5 | CreatePlayer | team (3), 4 bits, remote (1), player id (8), 16, 16, 1, name (32 bytes), profile id (sign + 31), flags (sign + 3), 1 |
| 6 | CreateObject | template id (32), ghost id (16), team (2), 1 bit: a count (8) of ghosts at the origin; else 1 bit + position, 1 bit + rotation (3 floats each) |
| 7 | DestroyObject | ghost id, 1 bit |
| 8 | DestroyPlayer | player id |
| 9, 10 | EnterVehicle, ExitVehicle | player id, (vehicle ghost,) 1 bit |
| 0xb | PostRemote | category (4), message id (32), delay (float), 8-bit length + up to 52 bytes |
| 0xc | ChangePlayerName | player id, 32 bytes |
| 0xd, 0xe | HandleDrop, HandlePickup | player id, two ghost ids, a position or eight kit bytes |
| 0xf | StringBlock | like DataBlock: chat text |
| 0x15 | KilledBy | attacker, victim, has attacker, an id, two template values, the armor's byte, the position, damage (10 bits) |
| 0x1f | CreateKit | template id, ghost id, position, 4 bits, four item bytes |
| 0x31 | PythonCommand | type (32), 8 bits, a count (4) of 32-bit arguments |
| 0x3f | SelectKit | kit (4), four unlock bytes (value + 1), 1 bit |

The template id is the template's network id (`ObjectTemplate +0x44`):
templates count up in creation order, network-shared and client-only
(flag `0x2000`, numbered from `0x1000000`) apart (`0x0070a700`), so both
sides must have loaded the same content. `world::ObjectTemplateManager`
numbers them the same way.

PostRemote carries a message for the other side's event manager
(`0x00a7d06c`). Category 0 id 1 is the game status (game logic `+0x40`,
`0x004ba2a0`): 1 playing, 2 end of game, 3 before the start, 4 paused.
Category 6 holds the network messages (client handler `0x004b6630`,
server `0x004b0690`):

| Id | Message | Direction, payload |
|---|---|---|
| 2 | LoadComplete | c→s, none: one per level block (data block 2) |
| 3 | StartSimulation | s→c: the client sends its connection type again (`0x004b48a0`) |
| 4 | DatabaseComplete | s→c after the objects; the client echoes it |
| 5 | Reset | s→c, the networkable checksum (32 bits); echoed when it matches (`0x00678cf0`) |
| 6 | SelectSpawnGroup | c→s, 32 bits (`0x004c1d50`) |
| 7 | SelectTeam | c→s, 32 bits (`0x004c1dc0`) |
| 9, 0xa | PlayerSpawned, PlayerDead | s→c, the player id (PlayerDead: and a byte) |
| 0xc | Suicide | c→s (`0x004c22e0`) |
| 0x10 | (unknown) | c→s after 6/4 when the byte at `0x00a1fc02` is set |

Squads, the commander, radio and votes are events of their own, not
PostRemotes:

| Id | Event | Fields |
|---|---|---|
| 0x10 | JoinSquad | team, player, squad (bytes; sent by `0x004c9bf0`; server `0x0050c6c0` → SquadManager `0x34`) |
| 0x11 | LeaveSquad | team, player, squad, 1 bit (`0x004c9c50`; another player: the leader kicks) |
| 0x19 | Invite | team, player, squad, 1 bit set for the leader's invitation (`0x0050e700`) |
| 0x1c | SetSquadLeader | as JoinSquad |
| 0x16 | ChangeSquadName | squad, team, the sender's player id, 8-bit length + name of at most 31 (`0x0050d960`). Naming an empty squad creates it: the server (`0x0050da80` → `0x004c6720`) needs the squad empty and the name unused (else PostRemote 2/0xf), names it, joins the sender (`0x004c7290`) and answers 2/0xe |
| 0x17 | SetPrivateSquad | team, squad, 1 bit (`0x0050db90`) |
| 0x13 | Commander | kind (0-8 in 4 bits), team, player (sign + 15; must be the sender): 0 step down, 1 withdraw, 2 resign, 3 set (s→c), 4 apply, 6 / 7 leave / enter the commander screen |
| 0x27 | Vote | target, choice, top count, target, top choice, votes needed, voter, phase, type (bytes), 32 bits (`0x005113c0`, server `0x00590230`): phases 0 start, 1 failed, 2 progress / ended, 3 passed, 4 cast, 5 bad target, 6 refused, 7 too many votes; types 0 internal, 1 map, 2 kick, 3 mutiny |
| 0x32 | Request | squad, team, 0-3 (the leader asks the commander; forwarded as PostRemote 2/0x19) |
| 0x14 | RadioMessage | the radio table index (16 bits), a byte, 2 bits (channel), a byte, 0..0x12, 0..4 (`0x004b3020`, written by `0x0050d1f0`). A client puts the channel in the byte after it too, and 0 in the last field. The first byte and the 0..0x12 value come from GameLogic slot 23's callers, which go through the scripts and weren't traced |

Squad refusals come back as PostRemote category 2 (0xb full, 0xc
private, 0x11 asks the leader). A squad is looked up before it is joined
(`0x004c6450`).

Chat goes as a StringBlock (queued by `0x00643370`, sent by `0x006431c0`,
put back together by `0x006430c0`): the text, then a 12-byte header
(channel and source as 32-bit numbers, the player id and two bytes). A
server only takes a client's line with source 2 and a channel other than
0xc and 0xd, which are its own messages. The source is the scripts'
"type" (GameLogic slot 37, `0x004a5ec0`): 1 from a server, 2 typed by a
player. The channels, as `0x004ab6b0` names them for the scripts:

| Channel | Name | Channel | Name |
|---|---|---|---|
| 0 | Global | 10 | LocalTeam |
| 1 | Team | 11 | Vehicle |
| 2 | Commander | 12 | ServerMessage (player 0xff) |
| 3 | Squad | 13 | ServerTeamMessage (player 0xff) |
| 4 | SquadLeader | 14 | Player |
| 5 | SquadId | 15, 16 | LocalRevive |
| 6 | LocalTeamMedic | 17 | TeamAndLocalEnemies |
| 7 | LocalGroundVehicle | 18 | SquadAndLocalEnemies |
| 8 | LocalAirVehicle | 19 | Invalid |
| 9 | Local | | |

Kills reach the HUD as PythonCommand type 100, with the arguments (kind,
victim, attacker...): kind 2 goes to the victim, 0 and 1 to the attacker
(`0x00511ba0`). KilledBy tells the victim where the damage came from
(built by `0x005b7680` → `0x005b7130`, written by `0x0050d750`, handled by
`0x0050d520`). Byte 0 is the attacker and byte 1 the victim. The two
template values are the weapon or projectile's, then the attacker's
vehicle's (or the source's root object's). The armor's vtable `0xdc`
gives the byte, `0xe4` the position and `0xec` the damage. The position
writer (`0x0050d5b0`) delta-codes only while the stream has a reference.
`0x0057b040` clears the reference before each packet's events, and only
the ghost block sets one, so servers send three raw floats.

CreatePlayer (`0x0050a4c0`, built by `0x0050a7c0`) carries the team (3
bits), the spawn group (4 bits, player `+0xe8`; the client handler
ignores it), remote (1), the id (8), the player's networkable ghost (16),
16 bits that are always 0, and then the bot bit. The builder reads the bot
bit through player vtable `0x68` (`+0xd4`), and the handler sets it with
`0x64` on remote players. The name, profile id and flags follow.

CreateObject's rotation is in degrees (yaw, pitch, roll). `0x004ab8a0`
builds the matrix with `0x006adb30(-z, -y, -x)`, which converts degrees
(`× π/180`). The late-join replay produces the vector with `0x00568c30` →
`0x006ad2a0`.

HandlePickup and HandleDrop (`0x0050bfd0`, `0x0050c1c0`) carry the player,
the kit's ghost and the soldier's ghost. The server's handler
(`0x004bb350`) asks the third for the kit holder interface `0xc49d`; the
soldier's QueryInterface (`0x00585af0`) answers it at `+0x188`. A drop
then moves the kit to the position.

**Ghosts** (server to client): 1 bit present, the server's tick (32 bits,
1/30 s), 8 bits the update count, 1 bit a control-object record follows.

- The control-object record (`0x00580cd0`) describes the client's own
  soldier or vehicle for its prediction: a 12-bit length of the rest, the
  last move the server took, the position (3 floats; it also becomes the
  reference for delta-coded positions, `0x006874f0`), the ghost id, an
  optional second ghost and vehicle, then networkable state. A client
  without that object skips it by the length.
- Each update (`0x0057fce0`) has 2 bits kind and a 16-bit ghost id. Kind
  1 updates: 1 bit (full), the state's length and that much state. Kind 3
  removes the ghost; kind 0 does nothing; kind 2 is an error.
- The length's width is the ghost manager's `+0x2140`, which both the
  reader (`0x0057fce0`) and the writer (`0x0057fbc0`) use. It is 11 bits,
  or 13 when the manager is built with a non-zero kind (`0x00582a50`).
  ServerConnection `0x0057b3f0` passes its own kind. A client connecting
  to a server uses kind 0 (`0x004b4d10`), and so do a server's per-client
  managers (`0x004ac920`). Only demo recording (connection id `0x81`) and
  demo playback (kind 2, `0x004b4a80`) use 13 bits.

### Ghost state

An update's state is read by the ghost's networkables (`0x0057f670`;
written by `0x0057f420`, both with mode 2). The networkable classes are
`dice.bf2.Networkable.*`, registered by `0x00518628` (class ids at
`0x00918d88`); their vtables have the writer in slot 11 and the reader in
slot 12. An object gets its networkable from its class: object vtable
slot 94 passes a class id from that table to `0x006fa2a0`, which makes
one only when the template has a `networkableInfo`
(`ObjectTemplate.setNetworkableInfo BasicInfo`...). The infos come from
`Objects/Common/Networkables.con`: `SpringInfo` is unique, and
`HandFireArmsInfo` forces a networkable id of its own.

| Template types | Networkable | Writer / reader |
|---|---|---|
| SimpleObject and every other type that makes an object (the default `0x0063c3f0`, or SupplyObject's `0x005d0af0`; see below); a root PlayerControlObject or Parachute (`0x005c77f0`); a DestroyableObject with `hasMobilePhysics` | SimpleObject | `0x005ff5d0` / `0x005ffec0` |
| Soldier (`0x005882f0`) | Soldier | `0x005fb8c0` / `0x005fcdb0` |
| PlayerControlObject or Parachute with a parent (a seat) | PlayerControlObject | `0x00605650` / `0x00605b00` |
| RotationalBundle, Camera, Wing, FloatingBundle, LandingGear (`0x005656c0`) | RotationalBundle | `0x006015a0` / `0x00602070` |
| Engine (`0x0064cc80`) | Engine | `0x00608df0` / `0x00609340` |
| Spring (`0x0064f840`) | Spring | `0x00605170` / `0x00605320` |
| MechLegs | MechLegs | `0x0060a000` / `0x0060a100` |
| TargetObject | TargetObject | `0x00609c60` / `0x00609d50` |
| Item | Item | `0x00603fe0` / `0x00604120` |
| GenericProjectile | GenericProjectile | `0x006067c0` / `0x00606f20` |
| Kit | Kit | `0x00605f90` / `0x00606030` |
| ControlPoint | ControlPoint | `0x006082b0` / `0x006084b0` |
| DestroyableObject without `hasMobilePhysics` (`0x005bf200`: template interface `0xc368` slot `0x34`, `0x005049e8`) | DestroyableObject | `0x006044e0` / `0x006048c0` |
| DestroyableWindowsBundle | DestroyableWindowsBundle | `0x00604d70` / `0x00604e00` |
| FreeCamera, OverheadCamera | their own | `0x00602b70` / `0x00602c50`, `0x006037a0` / `0x00603980` |
| player (`+0xc`) | (the player's) | `0x00596100` / `0x005981a0` |

GenericFireArm, LightSource, ForceObject, Ladder, SpawnPoint and the
particles make none (slot 94 is `0x0058cc40`, a return), whatever their
networkableInfo: a vehicle's guns have no state of their own.

Each remaining type was traced from the template's create slot to its
object's slot 94, and all of them get the default. The list: Bundle,
DynamicBundle, AnimatedBundle, ItemContainer, KitPart, Obstacle,
UAVVehicle, SupplyObject, SupplyDepot, AreaObject, AmbientEffectArea,
AntennaObject, DestroyableWindow, Emitter, EntryPoint, LookAtObject,
ObjectSpawner, RemoteControlledObject and TurnableRemoteControlledObject,
Rotor, Trigger and GroundEffectBundle.

The effect bundles (Effect, AirDraft and WheelEffectBundle) get the
default too, but a dedicated server never creates them (`0x0042b800`).
Particle systems, decals and trails create no object at all.

A ghost covers a subtree of objects (`0x0067c690`, cached per template):

- Every PlayerControlObject of the tree (interface `0xc4c5`) starts a
  ghost, in pre-order (`0x0067a270`), numbered from CreateObject's ghost
  id; CreateObject's count is how many. A vehicle is its driver's ghost
  and one per other seat (as_fav: the driver, AS_FAV_SecondPosition and
  as_fav_openPosition).
- A ghost's objects are its root's subtree without other
  PlayerControlObjects' subtrees, in pre-order (`0x0067a5e0`), those with a
  networkable.
- They are grouped in order. A projectile (interface `0x94c1`) with a
  parent is left out, and so is one whose info forces a networkable id
  unless it is the root or has no parent. A non-unique one joins the first
  group whose first member has the same template, or is a
  RotationalBundle or Camera with the same rotation settings (inputs,
  automatic reset, rotation limits, speed, acceleration; `0x00678070`);
  else it starts a group. More than 12 groups is an error ("has to many
  statemasks").
- An update is each group's state in turn, and every member of a group
  takes the same bits (`0x0057f670` rewinds the stream for each).

Positions are delta coded:

- `0x006876c0` / `0x00687510`: 2 bits mode. Mode 0 is three floats;
  modes 1-3 are three signed values of 20, 16 or 12 bits, times a step,
  from a reference. The writer takes the smallest mode that fits.
- The reference is the control-object record's position in this block
  (`0x006874f0`), else the origin.
- A narrow variant (`0x00687920` / `0x00687770`) uses 12, 10 or 8 bits.
- `0x00687c20` / `0x006879d0` take 3 bits mode: 24 down to 8 bits, and
  mode 7 is "the reference itself".

Quaternions (`0x00687f40` / `0x00687dc0`) are w, x, y, z, each
`(v + 1) / 2` in n bits (truncated), normalised when read. Values in a
range use the bits for the count (`0x00508370`).

Fields of components an object lacks are not sent at all, so the reader
needs to know them from its own template.

SimpleObject (25-bit mask, then a flag that drops the physics fields; it
is set for an object held by `0x00a67410`'s singleton):

- position (2, 0.5 mm steps from the reference);
- with a moving physics node and no flag: the velocity (4, 1 mm) and the
  angular velocity (8, 3-bit mode, 0.01). The node is object `+0x50`
  (node vtable 0x164), made by template slot 29 (`0x005f6ba0`). It moves
  when the template has `hasCollisionPhysics`, `hasMobilePhysics` and a
  physicsType of Point, RotationalPoint or Mesh. PhysicsType is template
  `+0xf4`, 0 (None) by default, or set by `hasPointPhysics`,
  `hasRotationalPointPhysics` and `hasResponsePhysics`. Without mobile
  physics only a Mesh gets a node, a static one;
- the rotation (1, 14-bit quaternion);
- physics again: two vectors (0x10000), four of 0.1 mm (0x80, 0x100,
  0x400, 0x800), whether the node sleeps (0x200) and a SupplyObject's bit
  (0x20000);
- with armor (object `+0x48`): hit points as `ceil(hp) + wreckHitPoints`
  in `0..maxHitPoints + wreckHitPoints` (0x10; armor vtable 0x8c is
  maxHitPoints, 0x90 wreckHitPoints), and the EMP level in 4 bits (0x20,
  armor `+0x27c`, falling every `armor.empLevelTime`);
- with a TeamChange component (`+0x5c`): its changePoints rounded up
  (0x40, 8 bits);
- 2 bits at `+0x168` (0x1000; base object vtable `0x1a8` sets it and
  `0x1ac` reads it). Which server code sets it wasn't found among the
  vtable's many indirect callers, so its meaning is open;
- with a PlayerControlObject on the parent chain (`0x00600560`): on a
  Parachute its state (0x8000, 0-7); else the selected weapon of weapon
  groups 0-2 as index + 1 in 0..8 (0x2000, 0x4000, 0x80000), a value
  0-15 at PCO `+0x190` (0x40000), PCO `+0x18c` (0x100000) and `+0x2be`
  (0x200000). PCO `+0x18c` is ducking (object `+0x310`, setter
  `0x005c7cf0`), which `0x005ca2e0` sets from kPICrouch when the template
  has `allowDucking`;
- the armored objects below the root (0x800000): the root's list (object
  vtable 0x14c), filled by `0x0060ec00` with every armored descendant in
  post-order, each child pushed after its own init made its children;
  per object its hit points in eighths (4 bits, `armor.fullChildSync`)
  or whether it is intact (1 bit);
- a ResponsePhysics bit (0x1000000).

Soldier (21-bit mask):

- health as 8 bits of maxHitPoints and a bit (0x40); the EMP level (0x80);
  two values 0-4 (0x20);
- dead (0x10000): the ragdoll (`0x008a9ce0`): a 9-bit length, the first
  particle's position as floats and the others as 3-bit-mode deltas of
  0.01 from it. Nothing else follows. (A seated soldier's state goes with
  the seat, below.)
- the position (1, 1 mm steps), the velocity (0x100, 0.01);
- yaw, pitch, roll and aim, each 12 bits of ±360, ±90, ±180 and ±90
  degrees (2, 4, 8, 0x10);
- always: the weapon's zoom level (0-3; its DefaultZoomComp), its fire
  comp's mode (0-1; only ExplosivesFireComp's `0x005e6350` uses it) and
  three bits;
- two leans of ±50 (0x800, 0x1000, 10 bits), a ResponsePhysics bit, a
  5-bit 0..1 (0x8000): sprint stamina (soldier `+0x378`; PCO vtable
  `0x108`/`0x10c`). In the control-object record (mode 3) it is 7 bits
  plus a sprinting bit;
- the active item (0x2000): index + 1 in `0..inventorySize + 1`
  (template `inventorySize`);
- 2 bits (0x4000); the ladder (0x20000: its id in 9 bits, then 12 bits
  of position on it); PCO `+0x190` (0x100000).

Vehicle parts:

- **RotationalBundle** (12-bit mask): rotation yaw, pitch, roll in degrees
  (1, 2, 4; 12 bits of ±360), their speeds (8, 0x10, 0x20; ±16 maxSpeed),
  0x40 a 16-bit ±41, and under a PlayerControlObject the ignore-parent
  rotation (0x200-0x800 angles, 0x100 a 14-bit quaternion). The writer
  only sends the angles an input drives. Values within one step of 0 read
  as 0 (`0x00593c70`).
- **Engine** (11-bit mask): the gear (2; `0..numberOfGears`), rotation
  speeds z and x (4, 8; 16 bits of 1.25 x the rotation limit), a target
  (0x10), three values of ±1.25 (0x20-0x80, 12 bits): the throttle that
  came out (`+0x25c`), the rotation over the template's `+0x2a4`
  (`+0x26c`) and the frame's averaged input (`+0x260`), a helicopter's
  angle of attack (0x100), a direction (0x200, 2 bits) and newCar2's rpm,
  clutch and gear (0x400). The engineType adds the last fields.
- **Spring** (unique): 1 bit, then the offset and velocity (narrow
  deltas of 2 mm). A value of ±256 follows for some springs
  (`0x00604f10`): those whose engine (the nearest Engine ancestor) is a
  newCar2 one, and whose other physics (object `+0x54`) has flag 8. That
  physics is made by template slot 31 (`0x005f6a60`), and only
  ResponsePhysics keeps flags. It is made for a Mesh with collision and
  mobile physics, and its flags are the template's `grip`: 2 by default,
  8 EngineGrip, 0x80 EngineDummyGrip (`0x0051aed0`).
- **PlayerControlObject** (a seat) works from the seat's PCO map
  (interface `+0x2c`):
  - With a non-empty map: a 4-bit mask, the active weapon of groups 0-2
    (index + 1, 4 bits), and ducking (8; PCO `+0x310`).
  - With an empty map: an 11-bit length and the occupant soldier's
    Soldier state.
  - The root PCO's init fills the map (`0x005cd4f0` → `0x0058dde0` →
    `0x0058bdb0`). It takes the cameras (Camera, FreeCamera,
    OverheadCamera) of the seat's whole subtree, nested seats included,
    whose pcoId matches. The shipped data sets no `setPcoId`, so every
    camera counts.
- **MechLegs**: 3-bit mask, 4 bits and two flags. **TargetObject**: 5-bit
  mask, a position, a flag and a byte. **Item**: 5-bit mask, a position,
  a signed 9-bit value, a flag, a signed 5-bit unlock. **FreeCamera**:
  position, 16-bit quaternion, a value. **OverheadCamera**: the pan
  velocity (two values of -1..1 times the template's speed), a position,
  a value, a flag. **DestroyableWindowsBundle**: a bit per window
  (`numWindows`), no mask, set while the window is intact (`0x005be010`
  breaks the cleared ones). **DestroyableObject**: hit points,
  a position and a 0-100 value (10 bits), a bit per armored part.
- **Kit:** a bit, then the position.
- **GenericProjectile:** a 6-bit mask, the position (0.01), a 12-bit
  quaternion, then a speed block and hit points.
- **ControlPoint:** a 6-bit mask, the team (sign + 3 bits), and a flag
  (cp `+0x1a0`). Reset (`0x005bb980`) sets that flag when the point starts
  with team 0. Then come the flag's height (the capture progress, 0..1)
  and its rate per second (takeOverChangePerSecond), as two 16-bit values
  of ±1.25. Then allowCaptureByTeam for teams 1 and 2 (cp `+0x1b4`,
  `+0x1b5`), and the scripts' "flag", whose flag is raised (0-2).

The player's networkable (30-bit mask; CreatePlayer gives its ghost id) is
fully decoded:

- alive (1; player `+0xd5`, the scripts' "alive");
- the selected spawn group (2, 0-255; player `+0xe8`, which the spawn
  code reads);
- the stats (8): score, deaths, kills, rplScore, TKs and player `+0x104`.
  Score and rplScore are sign + 11 bits, the others 12 bits. They go to
  the stats the scripts read (`0x0056b6f0`);
- the flag holder (0x400000, player `+0x154`);
- three kit slots;
- the ping in ms (0x100; 11 bits, 0-1023, player `+0xf0`). The server
  sets it from the connection's round trip (`0x0057ab30`);
- the spawn tick and man-down (0x20; player `+0xdc`, `+0xe0`);
- the team (0x200; the value + 1 in 3 bits);
- the float at player `+0x294` (0x800; 9 bits);
- the selected kit (0x1000; player `+0x58`, vtable `0x1a0`);
- the kit's unlocks (0x10000000, 0x20000000);
- the squad (0x2000; 5 bits, player `+0x108`, setter vtable `0x11c`);
- the rank + 1 (0x4000; 6 bits, stats `+0x20`);
- always the squad-leader bit (player `+0x10d`), then with 0x8000000 the
  commander bit (`+0x10c`). For a leader or commander, 6 bits and four
  0..1 bytes follow. A last bit (player `+0x1c0`) ends the state.

`src/Net/Networkables` has readers and writers following these,
`GhostLayout` splits a template tree into ghosts and groups (and builds
the tree and the `NetworkableHints` from the client's templates), and
`RemoteWorld` applies every group: the parts' rotations, springs and
engine, a seat's weapons and occupant.

The input block's 9-bit value is `200 + 1000 ×` the client's
interpolation time (`0x0057b040`). The 200 is the networkable manager's
`+0x50`, which the rate table sets. The interpolation time comes from
`0x006b4e70`, from settings at `0x00a83e4c`-`0x00a83e54`; the rewrite
takes it as 0. The server keeps the value at the player's `+0xf4` for lag
compensation.

A move's last bit is PlayerInput `+0x110`: whether pitch and roll come
from a device axis. `0x00696db0` fills PlayerInput from the control maps.
The bit starts at 1 and is cleared when a bound pitch or roll isn't an
axis-to-axis mapping (`0x00681fc0`, `0x00680dc0`). So it is 1 on foot and
with the mouse, and 0 where keys drive them (the Sea map).

The connection type picks a row of rates (`0x00928408`). `0x004b48a0`
applies a client's columns, and `0x004a6160` applies a server's for each
client:

| Type | Client packets/s | Server packets/s | Client bytes | Server bytes |
|---|---|---|---|---|
| 0, 1 | 10 | 4 | 160 | 260 |
| 2 | 30 | 10 | 160 | 350 |
| 3 | 30 | 20 | 320 | 400 |
| 4 | 30 | 20 | 320 | 512 |
| 5 | 30 | 20 | 320 | 640 |

Type 6 is ignored. For every type but 0, a client also sends after each
simulated tick (`0x004b3df0`), which comes to 30 a second. Once
connected, a client pings every 3 s (`0x0068f280`). The ping's sequenced
bit (`0x006a9bb0`) is set once data has gone out, as it is in a pong.

### Joining

1. The server sends event 1, a 9-letter CD-key challenge (`0x00509cc0`).
2. The client answers with event 2 (`0x004b2790`). It carries GameSpy's
   CD-key response (`0x00719260`): md5(key), a random number in hex, and
   md5(key + number % 0xffff + challenge). It also carries the build and
   the player's profile id.
3. The server checks the build (refusing old and new ones with 0x17 and
   0x18) and the profile, then validates the key with GameSpy's CD-key
   service (`0x004af940`).
4. The client tells its connection type (event 3; `0x004b48a0`, also
   called when the connection is made).
5. The server sends its settings (data block 0) and the map list (5)
   (`0x004a7ce0`).
6. With the player accepted (`0x004ae6a0`), the server sends
   CreatePlayer (the client's own player has the remote bit clear) and
   the level to load (data block 2), and marks the connection loading.
7. The client answers the level block at once with PostRemote 6/2
   (LoadComplete; `0x004b5eb0`) and loads it.
8. The server sends the objects (CreateObject) and 6/4, which the client
   echoes; 6/5 (the networkable checksum), echoed when it matches; then
   (`0x004af710`) 6/3 StartSimulation, on which the client sends its
   connection type again, and the game status (PostRemote 0/1). A server
   that requires a content check (settings) disconnects a client without
   one (reason 0x1b). Then ghosts.

Data blocks are bit streams; strings are a 16-bit length and the bytes
(`0x005155b0`):

- **0, settings** (`0x00516230` / `0x00515bc0`): the server's name; a
  16-bit count of welcome-message pieces (up to 3 of up to 256 bytes);
  7 bits: the writer's argument (`+0x30`; 1 for the send on connecting,
  `0x004a7ce0`, and 0 when resent after a change, `0x004b1c90`),
  voipEnabled, then allowNoseCam, hitIndicator, tkPunishByDefault,
  allowFreeCam and allowExternalViews (settings `+0xfa`, `+0xfb`,
  `+0x148`, `+0xf8`, `+0xf9`). Next come the voice server's address (32
  bits), sv.voipServerPort and sv.voipBFClientPort (16 each) and
  sv.voipQuality (signed 32). These are game logic slot `0x254`'s values,
  filled by `0x0057c250`. The address is inet_addr(sv.voipServerRemoteIP)
  when sv.voipServerRemote is set, else 1 (the game server's own). Then
  the GameSpy port and
  the game port (16 bits); radioSpamInterval, radioMaxSpamFlagCount and
  radioBlockedDurationTime (signed 32); content check required, voting
  enabled (bits); the players needed to start (16); friendly fire with
  mines and allowTitanMovement (bits). The players needed to start is
  `0x004316b0` (settings `+0x100`, at least 6 or 8 when `0x00430a00`
  holds).
- **5, map list** (`0x00441e40` / `0x00441410`): a 16-bit count, the
  16-bit index of the current map (maplist `+0x1c`), then per map the
  name, the game mode and a 16-bit size. It is sent again when the list
  or its current map changes.
- **2, the level** (`0x00515650` / `0x005156f0`): the game mode, the level
  directory, the map name, the map size as a signed 32 (the max players;
  `GSMaxNrOfPlayers`), a bit for commanders, and a signed 32 random 0-9
  (kept by the ghost manager). A map change is just another level block
  on the same connection: the server broadcasts it (`0x004abbc0(-1)`),
  counts a pending load per connection and waits for each 6/2. The client
  unloads, loads `levelDir + mapName` and answers.
- **8, end of round** (`gl_sendEndOfRoundData`, parsed by `0x0059c740`): a
  zlib-compressed text (sizes first), a signed 32, and per player bits of
  15-bit values (not decoded). 7 is console text, 9 a demo URL.

The round: BeginRound (0x38) carries the round's state (0 not started, 1
waiting for players, 2 the countdown, 3 playing; game logic `+0x204`),
the tick it ends at (`+0x208`: the countdown's end, or the time limit's,
0 for none) and the players needed (signed 8, -1 unchanged). The server
goes from 1 to 2 when enough players are there, and to 3 when the
countdown ends. EndOfRound (0x30) carries the rounds left on the map
(roundsPerMap - played - 1), the winning team and the victory type; the
game status goes to 2 with it.

Rewritten in `src/Net`: `BitStream`, `GameConnection` (with a
non-blocking connect and the acknowledgements), `GameEvents` (the
handshake), `GameProtocol` (the three blocks, all event layouts, typed
views of the ones above), `RemoteWorld` (the replicated players and
objects) and `GameClient` (update loop, reliable sending and resending,
input, requests). `bf2142 --connect host[:port]` (or the menu's Join)
plays on a server: it connects, answers the challenge and waits for the
map list; the level (the browser's map, else the list's first; the level
block and the list's current index say which: `RemoteWorld::level()`,
`currentMapIndex()`) loads as for a local game, with its gameplay objects as a client's (`v_arg1 client`: no
spawners, the server's objects come as ghosts). Each tick the client
updates and every ghost whose template we have becomes, and moves as, an
object of the level; the keys go up as moves, the camera follows the
control object, and a click on a spawn point asks for it. The server runs
the round (no local game-mode script, no bots). Each run hands the
client this level's templates (`setTemplateResolver`, `setGhostTemplates`
for the ghost layouts); seat ghosts are drawn with their vehicle. Another
level block on the connection is a map change: the level reloads and the
connection stays. A lost connection or the
menu's Disconnect goes back to the menu. `--probe host[:port]` only shakes
hands and prints what the server sends. A vanilla 1.51 server on OpenSpy (2026-10) takes the
handshake and the CD-key answer, sends its settings and map list, and
then waits; it sends no CreatePlayer, most likely because it needs a
logged-in profile id and a valid CD key (GameSpy pre-authorisation).
Without an OpenSpy account the rest is tested against a loopback server
in `tests/NetGameTest.cpp` that speaks these formats.

## Voice chat (BF2VoipServer.dll and Bf2VoipClient)

Three parties talk over UDP. The game server keeps a voice server's player
table up to date (BF2142.exe's `VoipServerConnection.cpp`,
`0x0057bf10`-`0x0057ce10`). The voice server (`BF2VoipServer.dll`,
`CBF2VoipServer` and its instances) routes voice between clients by
channels. Clients send Speex frames (`Bf2VoipClient`,
`0x0043ba40`-`0x0043cd40`, on GameSpy Voice 2's client
`0x00711070`-`0x00711e90`, capture and playback `0x00720910`-`0x00721390`,
codec `0x00737b30`-`0x00737d80`). Rewritten in `src/Voip`.

The settings are `sv.voipEnabled`, `sv.voipQuality` (1-5, default 3),
`sv.voipServerRemote` (0: the built-in server) and `sv.voipServerRemoteIP`.
`sv.voipServerPort` (55125) is the voice server's one port, for the game
server and the clients. `sv.voipBFServerPort` (55124) is the game server's
local port and `sv.voipBFClientPort` (55123) the clients' local port. An
external server reads port and password pairs from `voip.con`
(`55125 no_password`). The built-in server runs on a thread of the game
server, updated every 10 ms (`0x00645480`). Its password is made up per
session: `v` and 8-17 random letters (`0x0057c600`).

Every packet is read from its end. The last byte is the type, and each
layer takes its fields off the tail (`0x10008660`, `0x10008690`,
`0x10001cc0`). Integers are little-endian.

### Game server to voice server

A packet is the payload, then 24 bytes of version, then a 20-byte session
string, then the type (`0x0057c0a0`). The version is
`sprintf("%06d%06d%06d%06d", 1, 10, 112, 0)`, the game's 1.10.112.0
(`0x10002a0d`, `0x0057c68c`).

| Type | Name | Payload |
|---|---|---|
| 100 | SET_PLAYER_DATA | team, commander, squad leader, squad (1 byte each), session (16), player id (16; 0xff for the server's own player) |
| 101 | REMOVE_PLAYER_DATA | session (16) |
| 102 | RESET_PLAYER_ROUTING | none |
| 103 | FULL_ROUTING_DATA_SET | 65 records of 4 bytes by player id (64: the server's player): flags, 0, session. The flags are 0x80 for team 2, 0x40 commander, 0x20 squad leader, and the squad in the low nibble. |
| 200 | DEBUG_DATA | 0x659 bytes that switch on the debug logs |

Authentication works like this (`0x10001d40`, `0x10002510`, `0x100021f0`):

- The instance draws a challenge of ten lower-case letters. The game
  server's session string must be the first 20 hex digits of
  `md5(challenge + password)`.
- A packet without it gets the challenge back: the letters and `h`
  (type 104). The game server answers (`0x0057c4f0`, `0x0057c430`) and
  sends again. Before its first challenge the session string is 20 spaces,
  and that isn't counted as a wrong answer.
- A right answer with the wrong version is refused.
- An instance serves one game server. Another one is refused ("instance in
  use") while the first was heard within 28 s.
- After 28 s of silence the instance disconnects the game server and draws
  a new challenge (`0x100026c0`).

The game server sends SET_PLAYER_DATA when a player changes. A player who
is neither commander nor in a squad gets REMOVE_PLAYER_DATA instead
(`0x0057c8a0`). The whole table goes out every 5 s (`0x0057cbe0`), so a
voice server that restarted, or a game server that was just authenticated,
catches up. A session id is `(rand() & 0xffff) % (int)pow(256, 2)`
(`0x0057ce10`). The CRT's `rand()` only reaches 32767, so ids are 0-32767.
If another player already has the id, 1 + `rand() % 500` is added and the
players are checked again. Id 0 is possible, and the voice server refuses
it (`0x10005720`), as in the original. The game server sends each player's own in the VoipSession
event (0x36, 16 bits).

### The voice server's table and channels

There are 38 channels (`0x10002e70`). Each team has a commander channel,
nine squad-leader channels and nine squad-member channels: team 1 uses
0-18 and team 2 19-37. Each client listens to one channel and talks into
channel sets (`0x10003850`):

| Player | Listens to | Set 0 | Other sets |
|---|---|---|---|
| Commander | commander | all nine leader channels | 1-9: squad n's leader |
| Squad leader | squad n's leader channel | squad n's members | 1: the commander |
| Squad member | squad n's member channel | squad n's members and its leader | |

A player without a squad who isn't commander gets no channels. Squads
range from 1 to 9.

SET_PLAYER_DATA replaces the client with that session whole. Its address
comes back with its next packet. FULL_ROUTING_DATA_SET removes and re-adds
every record that differs (`0x10003dd0`).

### Clients and the voice server

A client's packet ends with its channel choice, its session id (16) and
its type:

| Type | Tail before the session | |
|---|---|---|
| 1 | none | talk into set 0 |
| 2 | the set's index | talk into another set (`0x00711710`) |
| 3 | the sets' indices, then their count | talk into several sets (`0x00711c10`) |
| 4, 5 | none | start or stop logging this player's voice (debug) |
| 6 | none | keep-alive, the whole packet is 3 bytes (`0x00711760`), every 3 s |

There's no login. A packet with a session the game server announced is
accepted, and its source address becomes that client's (`0x10008b40`).
The keep-alives register the address before the player talks.

The voice payload is whole Speex frames followed by the 16-bit frame
sequence number of the first one (`0x0043be90`). The server appends the
speaker's player id (16 bits). It sends the packet to every listener of
every channel in the chosen sets, except the speaker, clients with no
address yet, and the speaker's own address (`0x10008830`,
`0x100088e0`). The client takes the speaker and the sequence off the end
(`0x0043c0b0`) and drops speakers it muted.

How the Bf2VoipClient picks its sets:

- A squad member uses set 0.
- A squad leader holding `c_GIVoipUseLeaderChannel` (B) uses set 1, the
  commander (`0x0043be40`).
- A commander holding it talks to all squad leaders. Otherwise the
  commander talks to the squads selected on the commander screen, as a
  multi-set packet (`0x0043cd40`). The selection is read only while that
  screen is open. If the screen is open and no squad is selected, the
  commander is held back (`+0xc0`); `0x0043c8d0` clears that once the
  screen closes. If the screen is closed, the last selection stays.
- Only commanders and squad members may talk (`0x0043c8d0`).
- After more than 10 s of talk (`+0xa8`, counted from the first captured
  packet, sent or not) the client is blocked (`0x0043c140` ->
  `0x0043c290(1)`; player `+0x388`) and sends nothing. A talk ends after
  0.5 s without voice (player `+0x398`). The block is lifted on the next
  update after that (`0x0043c8d0`). The 5 s stored at `+0xb8` is never
  read, so talking on keeps the block.

### Codec and audio

Speex narrowband (`speex_nb_mode` at `0x00a090a4`, bit-stream version 4)
runs at 8 kHz with 20 ms frames of 160 samples. The decoder's perceptual
enhancement is on. `sv.voipQuality` 1-5 becomes GameSpy Voice's codec
4..0 (`0x00711120`) and Speex quality 1, 2, 4, 7 or 10 (`0x007217f0`). Any
other value gives 2.

A frame is written as `SPEEX_GET_BITRATE / 50 / 8` bytes (`0x00737b89`):
9, 14, 20, 37 and 61 bytes. That cuts the last 4-7 bits of a frame at
every quality but 3, and the decoder reads them as zeros. The rewrite does
the same, so frames stay the size the original's clients expect.

The capture path (`0x007210f0`):

1. DirectSound captures 8 kHz mono 16-bit. The rewrite uses SDL3 audio
   streams.
2. A frame is sent when one of its samples exceeds
   `voipCaptureThreshold` × 32767, and for 20 frames after the last loud
   one (`0x00737a20`). With push-to-talk (`c_GIVoipPushToTalk`, V) the
   threshold is 0 while the key is held and 1 when it's released
   (`0x0043bdd0`).
3. The capture volume scales the samples.
4. BF2142's filter runs: an RBJ band-pass at 2500 Hz with a bandwidth of
   1.75, at 8000 Hz, its output doubled and clamped. The filter's history
   is cleared every frame (`0x00712250` set up by `0x0043c420`, run by
   `0x0043bb60`).
5. Speex encodes the frame.

`voipBoostEnabled` never scales samples. `0x0070ff50` finds the
microphone line on the OS mixer's two destinations: component type
`0x1003`, or a name containing "Mic" (`0x0070fa90`). It switches that
line's LOUDNESS control (`0x20010004`), or its ONOFF control
(`0x20010001`), with `mixerSetControlDetails`. How much this adds depends
on the sound card driver, so it can't be known from the binary. The usual
case is AC'97's "Mic +20 dB". The rewrite adds those 20 dB itself.

The mic test (`0x007115e0`) loops the encoded frames straight into
playback.

On playback the rewrite follows GameSpy Voice 2's source list
(`0x00737710` adds a packet, `0x00737630` stores its frames, `0x00737590`
makes a source, `0x007378c0` mixes):

- There are up to 8 sources. A new speaker takes a free source, or else
  the first source whose frames have all played; failing both, the packet
  is dropped.
- Frames are stamped on a 16-bit play clock that counts mixed frames. A
  speaker's first packet gets the offset `clock - sequence + 10`, so it
  plays 200 ms later (`DAT_00b2d084`).
- Every later packet's offset feeds a running average
  (`avg × 31/32 + offset/32`, unwrapped by `0xffff`). If the source's
  offset is more than 10 above the average, it goes down by one and the
  packet's first frame is dropped. If it is more than 5 below, it goes up
  by one.
- A packet whose first frame is already behind the clock is dropped, as is
  one whose stamp is already in the list.
- Mixing decodes the frames stamped with the current clock and drops older
  ones. Nothing is concealed: a missing frame plays as silence. A source
  is freed once it is drained and past its last frame.
- The rewrite paces mixing by the wall clock, capped by the device's queue.
  This stands in for DirectSound's write cursor.

A speaker counts as talking for 500 ms after a packet.

### How a client starts

1. The server's settings transfer (data block 0, written by `0x00515bc0`)
   carries:
   - the server name (a u16 length and the text)
   - a u16 count of u16-length strings
   - seven flags, the second being voipEnabled
   - the voice server's IPv4 address (32 bits, network order; 1 means the
     game server's own)
   - `sv.voipServerPort` and `sv.voipBFClientPort` (16 bits each)
   - `sv.voipQuality` (a sign bit and 31 bits)
2. `0x004ab030` keeps these values in clientGameLogic (`0x004cbdb0`).
3. The VoipSession event (`0x00512250`) gives the local player its session.
4. clientGameLogic starts the client (`0x004cbe40` → `0x0043c630`).

VoipPlayerMute (0x26: mute, the muted player, the player muting) and
VoipOnOff (0x23: a player and whether their voice is on) go to the
players. The order of the settings flags around voipEnabled follows the
writer, but a live server's block hasn't been checked against it.

Rewritten in `src/Voip`:

- `VoipProtocol`: every format above.
- `VoipCodec`: Speex.
- `VoipServer`: the instance and CBF2VoipServer.
- `VoipServerConnection`: the game server's side.
- `VoipClient`: the client.
- `VoiceChat`: settings, events, role and keys around the client, on its
  own thread in the game.
- `VoipNatives`: the menu's `dice.bf2.Sound.voip*` natives.

`bf2142` binds the natives and applies `AudioSettings.Voip*`. On a joined
server it feeds the data block, the voice events and the squad to
VoiceChat, and the viewer feeds the push-to-talk and leader-channel keys.

The tests run loopback sessions with synthetic tones. They cover:

- authentication, refused passwords, versions and second servers
- squad, leader and commander routing
- muting, removal and the routing table's repair
- the talk limit
- voice activity
- the mic test

Not rewritten: the DLL's debug logs (`mods/bf2142/Logs/Voip/`).

## HUD

At start-up the engine runs `Menu/HUD/HudSetup/HudSetupMain.con`. Its
`hudBuilder` commands build a tree of nodes in an 800 x 600 screen below
six roots the engine provides: `Global`, `TopLayer`, and the bottom-left and
bottom-right `Static` and `Animate` corners. The node types are split,
transform, picture, text, bar, button, compass, object marker, list and a
few more. Nodes take their colour, alpha, RGB, position and texture from
HUD variables, which the game registers by name and binds to its own fields
(e.g. `0x0044c...` for the soldier's HUD information).

A node shows when its show variable is non-zero and every
`setNodeLogicShowVariable` term holds. `AND` and `EQUAL` test `variable ==
value`, and `NOT` tests `variable != value` (the engine builds a NOT node
over an EQUAL node; `0x007b0fd0`). An `OR` entry joins the term before it:
`NOT AffectedByEMPShow 1` followed by `OR UpdateAllEnabled 1` is one term.
A colour variable tints the node and everything below it, and a position
variable sets the coordinate once the game gives it a value.

RendDX9.dll draws the HUD twice a frame (`0x1002d5d9`):

1. With `AffectedByEMPShow` 0 and `UpdateAllEnabled` 1: the communication,
   game info and soldier parts.
2. With `AffectedByEMPShow` 1 and `UpdateAllEnabled` 0: the weapon and
   vehicle parts, which an EMP distorts.

The engine-side flag (`HudInformationLayer +0xb38`) is only ever set by the
renderer. The weapon HUDs pick their crosshair by `GuiIndex`, the template's
`weaponHud.guiIndex`. The parts take `Cross{Left,Right,Up,Down}Pos`
(`0x007be770`): the spread is the deviation in degrees times
`weaponHud.deviationFactor` (default 10), and the parts are 20 long about
the screen centre (Right = 400 + spread, Left = 399 - 20 - spread). Their
colour comes from the profile's `game.setCrossHairColor` (0-255, in
`Settings/Profiles/Default/GeneralOptions.con`). Bars fill
by their value variable (types 0 up, 2 from the right, 3 from the left;
inferred). Text uses `.dif` fonts: a text header and glyph table with A/B/C
spacing, top offset and texture rectangle, kerning pairs, and a `.dds`
beside it. Textures are under `Menu/HUD/Texture/`, mostly `.tga`.

Rewritten in `src/Hud` (nodes, variables, fonts, quads), with the drawing
in `src/Render` (`ui.vert/.frag`) and the variables set in `src/Main`. The
corner roots' positions are approximate. The crosshair stays at the
weapon's minimum deviation; soldier deviation isn't modelled yet.

### Text and the scoreboard

Text ids such as `HUD_TEXT_MENU_SCORE_PLAYERS` are looked up in
`Localization/strings.csv` (`0x00405360`: the profile's `GSLanguage`,
falling back to English), with `strings_patch.csv` applied over it. The
files are UTF-16LE, one row per line with tab-separated columns: the id,
then one column per language named in the header row. Values may be
quoted. This is rewritten in `src/IO/Localization`.

The scoreboard (`ScoreboardShow`, menu state 0xd in `0x0079ab90`) is built
from list nodes. Each node names a data source with `setListNodeData`:

| Source | Contents |
|---|---|
| 2 | friendly players |
| 1 | enemy players |
| 10, 9 | the two teams' totals |
| 3 | chat |
| 5 | squad members |
| 12 | maps |

The engine sets the team names (`HUD_TEXT_MENU_SPAWN_ARMY_MEC` and
`_USMC`) and the flags, at `Ingame/Flags/Icons/Hud/Score/<team
name>/scoreBoard_Flag.tga`. The lists are BF2142.exe's
`Bf2NewListBoxNode` (render `0x007de7d0`; RendDX9.dll only draws quads):

- `setListNodeBorder top bottom left right` (`0x007add90`) also turns the
  border on. The background colour fills the node; the border colour's
  bands go top (across), right, bottom (up to the right band) and left.
  The top band is where the column headers go.
- Rows start at the top border plus the row spacing and go a row height
  plus the spacing apart while one fits above the bottom border. A
  selectable list (`createListNode`'s last argument) fills the selected
  row in the select colour (default (1, 1, 1, 0.5)) less the side borders.
- A cell's text sits at its column: left, centred, or right-aligned to the
  column's end (its x plus its width, else the node less the right
  border, 6 pixels in or clear of the scroll bar, `0x007dd800`); it is
  centred in the row by its glyphs' extent and stops at the glyph that
  leaves the column. `§n` switches to font n (0..3, `setListNodeFont`),
  `|c` toggles the highlight colour (0.996, 0.639, 0.169); outlined lists
  (`setListNodeOutline`) draw each glyph eight times one pixel around in
  (0.15, 0.15, 0.15) first.
- With more rows than fit, the scroll bar (`setListNodeScrollbar width
  margin`): its track down the right inside the borders, its thumb the
  shown share of the track less two rows (at least 4 pixels).
- The cells sit under `scoreboard_icons.tga`: score, teamwork, kills,
  deaths, ping.
- The text colour is the list data's per cell (`0x007ddb90`); the code
  that fills the scoreboard's data sources is reached only through the HUD
  manager's interface (slot `0x214`) and wasn't found, so the rows are
  drawn black (`HudManager::setListTextColor` changes it).
- Rows are sorted by the scripts' score.

Tab shows the scoreboard in play; `--scoreboard` forces it on.

### Map and spawn screen

`createMapNode` builds the map. Its sizes come from the scripts:
`setMiniPos 245/-291` and `setMiniSize 146/146` for the minimap,
`setMaxiPos -122/-271` and `setMaxiSize 512/512` for the big map, with
positions from the screen's centre. The map texture is the level's
`Hud/Minimap/ingameMap.dds`, covering the primary terrain with north up.

The engine switches the map between two states:

- **Playing:** `MapShow` and `MapMinSize`, the minimap.
- **Dead:** `MapFullSize`, `MapFullSizeAndSpawnShow` and `SpawnShow`, the
  spawn screen. The in-game HUD stays up while dead.

The markers:

- control points, by owner (`Ingame/Flags/Icons/Minimap/<team>/miniMap_CP.tga`);
- on the spawn screen, the spawn icons of the points the team holds with
  spawn points (`Ingame/Minimap/Icons/spawn_(Un)Selected.dds`);
- teammates;
- the player's heading arrow.

While dead, the mouse becomes a pointer, and clicking a spawn icon picks
where the player spawns once the spawn time is up
(`GameWorld::setSpawnChoice`). HUD buttons run their console commands (`setButtonNodeConCmd`: event 0
is the press, 1 the pointer entering, 2 leaving, 3 the release) when
clicked. The commands include `hudManager.setDone`, `hudItems.setBool`,
`scoreboard.setToggleShow` and `spawnManager.setPlayerKit`.

A kit is chosen by the team's kit index (`gameLogic.setKit team index
kit soldier`: Recon, Assault, Anti-Vehicle, Support). The soldier spawns
with its kit's soldier template and the kit's primary weapon: its
`GenericFireArm` child with `itemIndex` 3 (1 is the knife, 2 the sidearm).
Bots take the kits in turn.

The map is BF2142.exe's `Bf2MenuMapNode` (render `0x00795620`). The
node is the minimap's or the big map's rectangle; the map texture is the
node's size times 2.3 to the power of a zoom level (`0x0077b7b0`), divided
by the shown area's share of the map, placed with the player at the
node's centre and pushed back to keep the shown area covering the node,
then cut to it. The level eases (1 - e^(-6 dt) a frame) to its target by
the zoom index (1 at start): minimap 0.5/1.5/2.5 on foot, 0/1/2 in a
vehicle, times 1/2/2.3 inside a Titan; the big map 0. The shown area is
the combat area's bounds in the big map's pixels, 20 pixels wider a side,
made square and kept on the map (`0x0078b490`); without one, the whole
map. Markers keep their pixel size at every zoom (`0x0077c610`: the
rectangle is floored, turned by the marker's yaw, cut to the node). The
exe's own sizes and tints (set where its markers are made): control
points 32 px, soldiers 15 or 16 px, squad leaders 32 px, the commander
16 px (`0x0077d9f0`, `0x0077dbf0`); players are tinted by side from the
table at `0x00b3be30` (no team (1, 1, 1), either team (0.7, 0.7, 0.7)).
Which markers to show and their sizes come from the game
(`HudManager::setMapMarkers`).

The soldier carries every `GenericFireArm` of the kit by slot, and keys
1–9 take the slot's weapon in hand. Each keeps its own magazine; the held
object, the muzzle effect and the weapon's animation system change with
it. Gadgets work through abilities (`ammo.replenishingType`: RTHeal or
RTAmmo, `abilityRadius`, `abilityStrength`, `abilityMaterial`). The medic
and ammo hubs are thrown out as their stationary projectile objects, one
per owner and gadget, and treat the team within their radius. The repair
tool (`onlyActiveWhileFiring`) works round the soldier's hands while the
trigger is held; a gadget without that flag (the hubs) works there while
it is in hand (the ReplenishingAmmoComp's tick, `0x005d8740`). The material table's `damageMod` doubles as the
ability's factor per target: the medic material 73 is 1 on soldiers'
materials and 0 elsewhere, and the repair material 84 has factors on
vehicles' materials.

The component's update (object vtable `0x00970b10` slot 6, `0x005d8ac0`;
template constructor `0x005d7f50`: abilityDrain `0x68` (a tick's at
`0x6c`), abilityStrength `0x70` (a tick's, × 0.033333, at `0x74`),
abilityMaterial `0x7c`, abilityRadius `0x80`, replenishingType `0x84`,
onlyActiveWhileFiring `0x88`) rescans its targets every second while it
has none and every two seconds while it has some (`*0x00a6db70` 1,
`*0x00a6db68` 0.5 a second): a sphere of abilityRadius round the holder
(`0x005d8740`), or with onlyActiveWhileFiring a ray of it along the
holder's forward (`0x005d84a0`); held targets are treated in between even
out of reach. Only the holder's team or team 0 for RTHeal and RTAmmo,
only enemies for RTTeam (`0x005d7d40`). Each tick each target gets the
tick's strength × the cell (`0x005d7a40`):

- **RTHeal** (gameLogic slot `0x108` → `0x004cc380` → armour slot `0x48`,
  `0x00655a00`): that many per cent of its maximum hit points, when it
  can be repaired, is alive and not full;
- **RTAmmo** (slot `0x10c` → `0x004cf7b0`): min(100, it) per cent of a
  magazine for each weapon (`0x005f5110`, the fraction kept a second).

The strength is also multiplied by the holder's player's
`ability.increasedRepairAbility` when above 0 (`0x005d7a40`: player slot
`0x2a0` → `+0x30c`, `+0x48`; copied from an unlock's Ability component
by `0x005706b0`, e.g. 1.5 for UnlockEU_AssaultMedic1).

The battery is the soldier's (`+0x338`, read and drawn through component
`0xc4d7` slots `0x90`/`0x98`, `0x0064aad0`/`0x00586310`), 0 to 1, −1
without a kit. A new kit's is empty (kit `+0x1bc`, 0 from `0x00562a80`);
the soldier takes it with the kit (`0x0058b680`) and hands it back when
dropping it (`0x00589890`). Each soldier tick adds the kit's
`abilityRestoreRate` / 30 (kit `+0x31c`, setter `0x0056cd90`), up to 1
(`0x0058d350`). Nothing is treated unless the battery holds a tick's
`abilityDrain` (template `+0x6c` = drain / 30, `0x005d7ae0`), and each
target treated takes that much when it is there (`0x005d81b0`). A hub's
battery is its owner's soldier's: owner seated or dead, none. RTTeam
(slot `0x110`) is used by no ammo in the game's data and is not
rewritten. A thrown hub (`unl_hub_*_stationary`) carries a `SupplyObject`
child (`Unl_Hub_Medic_Supply`: radius 5, healSpeed 4, sharedStorageSize
300; `Unl_Hub_Ammo_Supply`: radius 6, refillAmmoSpeed 8, workOnVehicles,
sharedStorageSize 500; both destroyWhenEmptyStorage) and works as the
supply crates do (see [Commander](#commander)), not by the gadget's
ability.

Projectiles with a detonation component (`DefaultDetonationComp`,
template `0x953b`) explode where they hit. Its constructor (`0x005e9df0`)
sets the defaults: `explosionMaterial` −1, `explosionForce` 150,
`explosionYMod` 1, `explosionRadius` 10 (so C4, which sets none, reaches
10 m), `explosionDamage` 500. The detonation (`0x005e9300`) hands the
blast to the server game logic (vtable `0x009294e8`): slot `0xe8`
(`0x004ba7a0` → `0x005a9090`) for a blast, slot `0xec` (`0x004ba830` →
`0x005a7ab0`) for an EMP (`isEMPWeapon`, `+0x5f`). With material −1 both do
nothing (the car horns' detonations); `endEffectTemplate` plays either way.
Rewritten in `GameWorld::explode`, `blastShare` and `empBlast`;
`GameWorld::detonate` sets one off at a point.

- **Distance:** to the object's position (a soldier's physics origin, a
  metre over its feet; a vehicle's centre), the height difference times
  `explosionYMod`.
- **Falloff** (`0x005a61a0`): 1 within a fifth of the radius
  (`0x00a6aab4`), then linear to 0 at the radius.
- **Soldiers' share** (`0x005a7530`): the head (bone 47) is worth 0.5,
  bone 11 0.35 and bone 3 0.15 (`0x00a01750`), each when a ray from the
  blast reaches it, or one from a point min(0.02 × radius, 0.5) m beside the
  blast (below, left, right, then above; each point used only when the
  blast sees it). A directional charge reaches only soldiers inside
  `explosionConeAngle` (`0x005a6370`).
- **Damage** (`0x005a8900`): `explosionDamage` × falloff × share × the cell
  of `explosionMaterial` against the target's material; 0.1 or less is
  dropped.
- **Push** (`0x005a66e0`): `explosionForce` × falloff × share × the target
  armour's `explosionForceMod` (default 1; soldiers 25), at most its
  `explosionForceMax` (default 10; soldiers 1000), for a soldier times
  0.8 + 0.005 × (rand % 100) and with its speed counting against the
  maximum. It points along the soldier's axis nearest the line from the
  blast plus that line made a unit with 5 added upwards (`0x00a6aac0`).
  `0x005ee4d0` keeps it for 0.5 s for the ragdoll, times `rd_expForceMod`.
- Not in the rewrite: the ray test for other objects (blocked: half the
  damage, or none when the object's vtable `0x90` value, not identified,
  is below a fifth of the radius), a bone hidden only by an object the same
  blast destroys counting half, the push's 0.1 for a soldier in water
  (its physics node's slot `0x144`, `+0x5c`: how deep it is under the
  surface, which `0x006c9060`'s buoyancy divides by the box's height; the
  rewrite's soldiers have no water physics yet) and the push on other
  objects' physics (node slot `0xa4` at the point, with 10000 /
  max(mass, 100) and at most 500 for an object without armour, and a
  torque pair for one with component `0xc54d`).

Thrown weapons: projectiles with `physicsType` RotationalPoint or Mesh
(grenades, mines, C4; rounds and missiles are Points) bounce. The
template makes the node and response by physicsType (`0x005f6ba0`,
`0x005f6a60`); RotationalPoint's response (`0x006c0cb0`, vtable
`0x00993018`) finds the deepest ground contact (`0x006c0f90`: the
heightmap's height, normal and material under the node's centre of
collision, or the water) and object contacts (`0x006c1220`) and solves
them (`0x006c0d00`): pushed out by the depth, the velocity's part along
the normal reversed times the mean of the two materials' elasticity
(slot `0x80`), the part along the surface times 1 − their friction (slot
`0x88`, at most 1); nothing on a water material. A frag grenade
(Grenade_hand: friction 0.25, elasticity 0.5) keeps 0.25 of its speed
into grass and 0.475 of its slide. There is no rest speed: only the spin
stops, under |v|² 0.1. The rewrite's projectiles lie still (for the
mines' triggers) under that on a face turned up, and Mesh projectiles,
rigid bodies in the engine (ResponsePhysics), bounce the same way here;
the terrain's material is the level's material map's (see Terrain
materials). They go off when
their time to live runs out (frag grenades: 2.8 s, 140 damage over 9 m),
not on impact.

**Deviation** (the deviation components: `SimpleDeviationComp`, template
`0x005df040`, object `0x005defc0`; the shared base `0x005f5750` also
serves `SoldierDeviationComp`). The template keeps `setFireDev a b c` (the
build-up's maximum `+0x10`, a round's addition `+0x08`, an update's
decay `+0x0c`), `minDev` `+0x14`, `subProjectileDev` `+0x18`. A firing
(`0x005c3cb0`) makes `batchSize` projectiles, the first the main round
and the rest sub-projectiles; for each the component updates once a tick
(`0x005dedb0`: the build-up decays; the deviation is minDev plus it, 0
while zoomed) and applies (`0x005f5540`): with a deviation over 0.01, two
normal(0, 1) numbers from a table of 1024 (filled once by `0x005b28d0`,
Marsaglia's polar method on the engine's shared generator, at the first
component's construction; read at Game slot `0x68` + the round's index)
times the deviation (`subProjectileDev` for a sub, kept; the main round's
adds the last sub's), each × 0.01 × the firearm's velocity (`0x005c1ac0`),
added to the projectile's velocity along its up and right axes. Every
round adds to the build-up, to its maximum. So deviation 1 is 0.57°, one
standard deviation on each axis; 45 is 24°. The rewrite applies it to
the strikes' shells and the sentry drone's rounds with its own generator
(the table's contents depend on the shared generator's state at the time,
which the rewrite can't reproduce). The build-up (`DeviationBuildUp`):
`deviation.setFireDev` is a method of three numbers (template `0x005f5360`:
`+0x10` the most, `+0x8` added a round, `+0xc` the decay a tick), with
`minDev` (`+0x14`) the floor; a round adds (`0x005f5540`), each tick off
the trigger decays toward the floor (`0x005dedb0`). Hand-held firearms
have no deviation model yet.

- **Mines:** a mine fires when an enemy faster than
  `triggerVictimMinSpeed` (3.6 m/s: walking past is safe) comes within its
  `triggerRadius`, inside `triggerAngle` of its facing. Its blast reaches
  only `explosionConeAngle` of that facing.
- **C4:** it has neither a trigger nor a fuse to speak of (9999 s), so its
  owner sets it off with alternate fire (the right mouse button). It has
  no radius of its own: the detonation's default, 10 m.

Vehicles take damage through their armour settings:

- **EMP:** an EMP blast (`detonation.isEMPWeapon`: the EMP grenade, mine
  and launchers, the commander's EMP strike) hurts nobody (`0x005a7ab0`).
  Every armoured object within the radius takes EMP damage `explosionDamage`
  × (1 − distance / radius) (`0x005a6500`; with `armor.empDamageIgnoreRadius`
  × `empDamageIgnoreRadiusModifier`, default 0.1; 0.1 or less is dropped).
  The armour (vtable `0x00988cb8`, slot `0x148` = `0x00652980`) adds it in
  steps: level = min(`empDamageNumStep`, ceil(level + damage /
  `empDamageStepSize`)), and the level falls by one every `empLevelTime`
  seconds (`0x00652910`; defaults at the armor template's `0x180`-`0x188`:
  5 s, 120, 3 steps); slot `0x14c` (`0x00652a00`) gives it rounded up. The
  EMP grenade's default 500 at its centre fills every step. The
  PlayerControlObject's input filter (`0x005ca2e0`) scales the driver's
  throttle, yaw, pitch and roll by 1 − level / `empDamageNumStep` (0, 1/3
  and 2/3 at levels 3, 2 and 1 with 3 steps), mouse look left alone; a
  seat whose own armour is a child's (not the root's) scales mouse look
  too. With `autoControllerTemplateName` (template `+0x298` slot `0x110`)
  it cuts every input above level 1. With `scaleInputOnCriticalDamage`,
  below the armour's `criticalDamage` the inputs are scaled by
  `criticalDamageScaleModifier`. The level falls continuously, at
  1 / `empLevelTime` a second. Firearm components never read the EMP
  level: the filter (`0x005ca2e0`) scales the driver's inputs only, and a
  gunner's fire (PIFire, input 8) and look are touched only on a seat
  with its own child armour. So gunners keep firing; an auto-controlled
  object (the sentry drone) neither turns nor fires above level 1. Only
  ActiveDefense ammo (`0x005d9560`) and camouflage (`0x005d9030`,
  `0x005d8ec0`) drain faster, × (level + 1).
- **Critical damage:** below `armor.criticalDamage` a vehicle burns down at
  `hpLostWhileCriticalDamage` a second (FAV: 18 below 151, out in 5.6 s from
  100).
- **Water and rolling:** in water deeper than `armor.waterLevel` over its
  centre for longer than `waterDamageDelay`, a vehicle loses
  `hpLostWhileInWater` a second (`hpLostWhileInDeepWater` past
  `deepWaterLevel`). Upside down, it loses `hpLostWhileUpSideDown`.
- **Destruction:** a destroyed vehicle explodes with `armor.explosion*`
  (FAV: 300 over 6 m), kills everyone aboard, and stays as a wreck for
  `timeToStayAsWreck`. Its spawner then puts out a new one.

Deployables: a gadget whose projectile has mesh physics and armour, and
neither explodes nor holds a beacon (`portable_shield`: 600 hit points),
is set down a metre ahead, one per owner and gadget. Its mesh then blocks
rounds like any body's, and shot to pieces it is gone.

Squad leader drones: the recon and sentry drones' detonators
(`Unl_Drone_Recon_Detonator`, `Unl_Drone_Sentry_Detonator`, slot 8) fire a
`PlayerControlObject` (`unl_drone_recon`, `unl_drone_sentry`: 120 hit
points, `takesEmpDamage`) that carries a `ForceObject` child. The drone
starts at the detonator's `fire.projectileStartPosition` (1/1.75/1 from the
feet) and leaves at its `velocity` (5 m/s). Each owner has one drone
(`maxProjectilesInWorld` 1), and the detonator's `ParentTargetComp` makes
the owner the drone's target. The detonators have `ammo.nrOfMags -1`,
which means magazines without end.

The ForceObject template (class `0x9489`, created by `0x0053fd17`,
constructor `0x0062d4f0`, properties registered from `0x00547519`, saved
by `0x0062add0`) creates a runtime object of `0x1f0` bytes (`0x0062d4c0`,
constructor `0x0062cd10`). Its tick (`0x0062c920`, then `0x0062b750`)
works like this:

- The target is lost (the owner is dead or gone): with
  `destroyWhenTargetLost` (template `0x295`) the drone is destroyed.
- Across the ground, the goal is the point `positionToleranceXZ`
  (`0x2a0`, 1.5 m) short of the target. When the drone is that near
  already, the goal is where it is. In height, the goal is the target's
  height, taken again only while the target is more than
  `minHeightAdjustDistance` (`0x2ac`, 0.7 m) off across the ground (kept
  at `0x1a8`).
- Farther than `destroyDistance` (`0x2a8`, 12 m) from the goal, the drone
  is destroyed. Past `warningDistance` (`0x2a4`, 10 m) the owner gets the
  `uav_warning` HUD message.
- Each axis has a linear regulator (`0x006678a0`; set up by `0x0062cf60`
  with the mass m and `regulatePosX/Y/Z`, `0x270`/`0x278`/`0x280`). Its
  output (`0x00667710`) is force = 2am·v + (a² + b²)m·(goal − p), so the
  pair (a, b) gives the poles a ± bi. The drones' −0.5/2 decay by half a
  unit a second and turn at 2 rad/s (Y: −0.5/1.75).
- The drone turns to face the way it moves.

The force goes to the physics node's slot `0xa0` (`0x006c7b10`: added to
the acceleration, its moment to the torque) times 30 over the mass, so a
30 Hz tick changes the velocity by 2a·v + (a² + b²)(goal − p); the node's
step adds gravity and damps by 0.99. The drones sit about 0.15 m under the
goal against gravity. The target's position is the soldier's physics
origin, a metre over its feet, which is the height held. While the
drone's armour has an EMP level (rounded up) above 1, the update stops
before the force (and the height held becomes the drone's own), so the
drone falls until the level drops (an EMP grenade: 3 levels, 5 s each).
The target is lost when the ParentTargetComp's handle (`0x1b0`) empties;
the rewrite takes the owner's death for that (whether a downed soldier
still holds it isn't settled by `0x0062b750`).

The drones' guns are run by auto controllers (`AI/AutoControllers.ai`):

- **Recon drone (`ReconDrone`):** `simpleFirePlan` fires its
  `unl_drone_recon_FireArm` all the time, 10 a minute. Its
  `PulseMeterAmmoComp` (template `0x94f4`; `pulse.viewDistance` at `0x68`,
  `fov` `0x6c`, `revealTime` `0x70`, an int, 10 by default) pulses on each
  round (`0x005d9cf0`). With `fov` below 0 the pulse reaches all round:
  every player whose object is within `viewDistance` (60 m) of the drone,
  with no line of sight needed. The server sends them to the drone's team
  with `revealTime` (3 s) for its minimap. The projectile's
  `Ability.radarRadius` (65) is the minimap's ring and isn't drawn yet.
  `GameWorld::isRevealed` and `revealedPlayers` answer for the HUD.
- **Sentry drone (`UAVDrone`):** `unl_drone_sentry_FireArm` fires at 600 a
  minute at the nearest enemy soldier on foot within `viewDistance`
  (100 m) and in line of sight. It sees cloaked soldiers only within
  `discoverCloakedEnemiesDistance` (8 m) and never targets its deployer.
  The rounds go through the ordinary projectile path (10 damage, falling
  to 5 from 15 to 40 m) and stray by the gun's deviation component (see
  Deviation below). Kills count for the owner. Its auto controller
  (AIDLL; an ordinary Bot, `0x100c8d20`):
  - **Scan** (ScanForEnemies, `0x100f9a90`/`0x100f9cf0`): without a target
    the gun's yaw turns by `scanningSpeed` × 30 radians a second (0.13:
    3.9 rad/s); the camera bundle has no yaw limits, so it goes round one
    way (a limited bundle ping-pongs between its limits).
  - **Sight:** a frustum of `fov` (90°: ±45° each way about the gun) out
    to `viewDistance` / cos(fov / 2) (141 m, `0x100feea0`); a target out
    of sight for `targetLostTimeout` (0.25 s) is dropped. While tracking,
    the yaw input is limited to `maxTrackingInput` (1) × the bundle's 250°
    a second. The rewrite fires once the gun points within 2°.
  - **Only what the squad reported** (`onlyAttackNetworkBattlefieldObjects`,
    template `0x39`; `0x100ea470`): with an owner in a squad, an enemy
    counts only when the owner's squad had it reported in the last 3 s
    (`0x100bea30`: a table keyed by team × 100 + squad, then object id,
    filled by `0x100bf000` through IAIEventCourier vt+0xe0). An owner
    without a squad lets it attack anyone.
  - **The reports** (BF2142.exe event `0x41`, server `0x005088f0`): a recon
    drone's pulse (`0x005d9cf0`, the drone's owner reporting those inside
    it), a bot in a squad as its trigger goes down and comes up at its
    target (AIDLL `0x1009a1d0`, `0x1009a2a0` → IAIGame vt+0x6c,
    `0x008958a0`; 30 s and 3 s on the squad's minimap), and active
    camouflage going on or off (`0x005d92c0`: each enemy squad leader
    reports the cloaker). Each reported enemy and the occupants of its
    vehicle count from then. The radio's "spotted" (SpottedEvent `0x1d`,
    `0x0050e910`) only tells the team's players; it never reaches the
    AI. The minimap side (the client's `0x00508590`) isn't rewritten.

Under EMP above level 1 the sentry drone's auto controller (UAVDrone) has
every input cut by the PlayerControlObject's filter (`0x005ca2e0`), the
fire too: its gun neither turns nor fires. The recon drone's pulse is
not an input and goes on. Bots that lead a squad send out the
sentry drone at infantry 20 to 100 m off, following its weapon template
`SentryDroneDetonator` (FireAndForget).

Active camouflage (`unl_active_camo`, recognised by its
`CamouflageAmmoComp`) cloaks the soldier while the trigger is held, using
heat as its charge (0.005 a round, cooling 0.08 a second, 1.5 s off when
spent). Nothing else can be fired while it's in hand. Bots see a cloaked
enemy only within `aiSettings.setDiscoverCloakedEnemiesDistance` (3 m).
The soldier is drawn hidden as a stand-in for the shimmer.

Seeking projectiles (`seek.*`, `follow.*`):

- **TTUnique (the AV missile):** follows its shooter's aim, turning at most
  `follow.maxYaw` and `maxPitch` radians a second (0.45 and 0.85).
- **TTMagnet (the motion mine,** `Unl_Mine_Motion`; the projectile's
  update `0x005b3e60`, seek component `+0x1a0` (0xc555), follow `+0x1a4`
  (0xc559)): once `armingDelay` (3.5 s, template `+0x2fc`) and
  `seek.trackingDelay` (1.5 s) have passed since the throw it looks for a
  target (`0x005ec260` → gameLogic slot `0x1c8`, `0x004cf290`): the
  registered TTMagnet targets (the vehicles' `MagneticObject` children,
  aimed at; `disableWhenEmptyVehicle` turns one off 3 s after the vehicle
  empties, wrecks stay), not of its side (`ignoreFriendly`), within
  `maxDistLock` (20 m) and `maxAngleLock` of its facing, in line of sight
  (terrain and objects), the nearest. A held target is kept out to
  `distLoseLock` (24 m). The chase starts `trackingDelay` after the lock
  (`0x005b2c10`). The follow (`0x005ed1a0` → `0x005ecb30`) aims at the
  target plus its velocity × (distance / own speed) × `predictionFactor`
  (0.1); the target's top speed so far arms it once over `armingSpeed`
  (7.2 m/s). The thrust: `acceleration` (60 m/s²) towards that while
  under the cap, `deAcceleration` (50) against the velocity at it; the cap
  is `maxSpeed` (300), or with `useTargetSpeed` the target's top speed,
  × `speedModifier` (2) once armed (`0x005ec770`, `0x005ec7a0`). A parked
  vehicle is never chased. It goes off on hitting the vehicle (its
  collision, gameLogic slot `0xf8`). Not traced: the detonation
  component's 0.05 m MTYVehicle trigger test, its `ForceObjectHoverTest`
  and `drag`.

Unlocks are the kits' `ItemContainer` children, one per branch and level
(`unlockLevel`). An unlocked container adds its items to free slots: the
frag grenade at 7, and the shotgun, defibrillator, mines, C4 and EMP
grenade at 5. The local player gets the containers of its soldier's chosen
unlocks ([Soldier stats](#soldier-stats-ranks-and-unlocks)); `--unlock-all`
unlocks everything. The customise screen's choice between items for the
same slot isn't in yet.

Guns without magazines (`ammo.magSize -1`, most vehicle guns) heat up
instead. Each round adds `heatAddWhenFire`, the heat cools by
`coolDownPerSec`, and at full heat the gun stops for `overheatPenalty`
seconds. The HUD shows `PrimaryHeatValue`. `--spawn-screen` dies first, to
show the spawn screen.

## Game scripts

The game logic of a round is written in Python: `python/bf2` (the
managers, timers, triggers, stats) and `mods/<mod>/python/game` (scoring
and the game modes, e.g. `gamemodes/gpm_cq.py`). BF2142.exe embeds
CPython 2.3 (`dice_py.dll`, its library in `pylib-2.3.4.zip`) and registers
the `host` module (strings near `cp_getParam` in the executable list its
functions and the object methods). Engine objects reach scripts as objects
holding a token, one per engine object, so scripts can keep attributes on
them. Players reach scripts through `bf2.playerConvFunc`.

At round start the engine imports `bf2`, runs `bf2.init_module()` and then
the game mode's `init()`, and calls the game status handlers (PreGame,
Playing). Engine events (`PlayerConnect`, `PlayerSpawn`, `EnterVehicle`,
`ControlPointChangedOwner`, `TicketLimitReached`, `TimeLimitReached`...)
call the handlers scripts registered with `host.registerHandler`.

In conquest the script does the deciding. It counts who is inside each
control point's radius trigger, picks whose flag is up (`cp_setParam
flag`) and how fast it moves (`takeOverChangePerSecond`, from
`timeToGetControl` and `timeToLoseControl`), and sets the new owner when
the flag reaches the top or bottom. It also sets each team's ticket loss
from the area values the teams hold. The engine moves the flags, applies
`ticketChangePerSecond` and raises the limit events (`ticketLimit` ids,
negative going down; id 1 at 0 tickets ends the round).
`bf2.GameLogic.setTicketLimit(team, id, value)` calls `sgl_setParam` with
the id where the other parameters pass their value, and the value last.
`getTemplateProperty` answers a property the template's script left out
with the engine's default (a control point's `radius` is 1).

The rewrite embeds Python 3 instead (`src/Python`). `py2compat.py`
translates the Python 2 scripts as they are imported. It rewrites syntax
(print and exec statements, `except E, e`, backticks, `<>`, old literals,
tab indentation) and semantics (integer `/` floors, `has_key`, `keys()`
lists, `sort(cmp)`, implicit relative imports, list-returning `map` and
`range`, `string`, `types`, `fpformat`, `md5`). `ScriptHost` provides
`host` against `game::ScriptGame` (`src/Game/ScriptGame.h`), which
`ScriptBridge` answers from the game world and `GameLogic`.

## Soldier movement

The Soldier object reads the player's input (`0x0058a6d0`): throttle and
strafe are shaped (`sign(x) * x^2`) and eased (0.1 of the way per tick
while pressed, 0.2 when released); their direction along the soldier's
heading, at most 1 long, times the pose's speed (table at `0x00a68f4c`:
prone 0.8, crouch 2.0, stand 3.9, sprint 6.3, swim 2.1 / 3.645 m/s) is the
velocity the soldier's response is asked for (`0x00588420`, response slot
`0x84`). The response (`BF2/Physics/SoldierResponse.cpp`, vtable
`0x009930f8`) then per tick:

- ground (`0x006c28c0`): the first five points of the built-in
  `Soldier_CollisionMesh` (feet 1 m below the origin, four at 0.6 m)
  against the heightmap; each touching point pushes out, cancels the
  velocity into the ground and adds its velocity to an average
  (`0x006c24b0`); the first point decides standing and the slope;
- static objects (`0x006c3890`): five spheres of 0.25 m from the feet to
  1.7 m (three crouching, one prone) against the objects' soldier collision
  LOD, contacts handled the same way, after three filters:
  - a contact more than half a sphere spacing above the lowest centre whose
    normal points up (y at least 0.95) loses the normal's height, and so
    does one below the top whose normal points down;
  - of the floor contacts under the lowest centre, the first counts and
    each nearer one after it is dropped (`0x006c4180`, taken literally);
  - a contact whose sphere centre lies behind the face is dropped
    (`0x00a846fc`).

  A sphere touches a face (`0x006bf010`) when its centre is within a
  radius of the face's plane: inside the face with the face's normal (also
  from behind), at an edge or corner only from the front. Face normals are
  `(c - b) x (b - a)` (`0x006e8080`); faces are wound clockwise, so the
  normals point out of the object. Each LOD serves the user its collision
  type names (0 projectiles, 1 vehicles, 2 soldiers, 3 AI navigation;
  `0x006e4880`, by order in version 8 files); a mesh without a soldier LOD,
  like a bush's, does not stop soldiers;
- movement (`0x006c2230`): the difference between the average contact
  velocity and the wanted one, along the ground, corrected by at most
  `normal.y^5 * friction * 1.5712` m/s per tick (2.3568 before slipping),
  as an acceleration for the soldier's physics node, which integrates
  without rotation, damped 0.99 per tick (0.9 in water).

The look turns the head before the body (`0x0058a6d0`,
`SoldierControl::twist`): standing (the shaped throttle within 0.01) the
mouse turns the head up to 40° (`*0x00a68eb0`) off the body; past that
the body turns and the twist catches up 4° a tick (`*0x00a68ee0`; the
original turns the body by the excess and catches up as well, so it
briefly turns twice, which the rewrite keeps). Moving, the twist catches
up. Fire, aim and placing use the look (`GameWorld::lookYaw`), not the
body's heading.

Rewritten in `src/Physics/SoldierPhysics` and `src/Game/SoldierControl`.

## Account login (EA's FESL, via OpenSpy)

Before it browses or joins, the game logs in to EA's account service
(feslsdk 2.8.0.0, statically linked). The unpatched game connects to
`bf2142-pc.fesl.ea.com:18300` over its own SSLv3 (`0x004a0000`'s call to
`0x00800c60`). The OpenSpy patch replaces that connect (RendDX9.dll
`0x1004aab0`) with OpenSSL to `fesl.openspy.net:18301`: TLS 1.3, the host
name sent for SNI, a valid certificate.

Messages have a 12-byte big-endian header: the service (`fsys` for the
connection, `acct` for accounts), an id word (`0x80000000` plus a count from
the server; the client uses `0xC0000000` plus its transaction number), and
the whole length. Then come `key=value` lines and a NUL. `TXN` names the
transaction; `TID` numbers it, and the reply repeats it. Values may be quoted
and URL-encoded. Lists are `name.[]=count`, then `name.0`, `name.1`, ...
Errors carry `errorCode`: 101 means no such account, 122 a wrong password
(OpenSpy's `FESL_ERROR`).

The game's login runs:

1. `fsys Hello` (`0x008154f0`): `clientString=bf2142-pc`, SKU, locale,
   platform, versions, `protocolVersion=2.0`, `fragmentSize=2048`. The server
   answers with its time and its theater and messenger hosts, then sends
   `fsys MemCheck`.
2. `fsys MemCheck` reply (`0x00815360`): the result is a hash of the asked
   memory regions XOR the salt. OpenSpy asks for none, so it is empty.
3. `acct Login` (`0x00801ef0`): name, password (or a saved `encryptedInfo`),
   machine id and MAC address. The reply carries `lkey`, `userId` and
   `profileId`.
4. `acct GetSubAccounts`: the soldiers. Then `acct LoginSubAccount name=...`
   gives the soldier's `profileId`, which goes into event 2 when joining.
5. `acct GameSpyPreAuth`: a ticket for GameSpy presence and stats.

The server pings (`fsys Ping`) and the client answers.

Rewritten in `src/Net/Fesl` and `TlsSocket` (OpenSSL; optional at build
time). `bf2142 --fesl` greets the live service. `bf2142 --login
name:password[@soldier]` runs the whole login, and with `--connect` it joins
with the soldier's profile id. Live: OpenSpy answers the greeting, and an
unknown name gets error 101. A full login needs an OpenSpy account; none was
used here.

## Players, deaths and respawning

`GameWorld` keeps a list of players: the local one (`spawnPlayer`), plus
bots and later remote players (`addPlayer`). Each has its own soldier
state, input, kills, deaths and team kills.

- **Spawning:** a soldier appears at a spawn point of a control point its
  team holds. A spawn point's control point comes from its object
  (`Object.setControlPointId`) or else its template
  (`ObjectTemplate.setControlPointId`, as the coop levels set it). The
  scripts get `PickupKit` and then `PlayerSpawn`.
- **Hits:** soldiers are hit through their bones' capsules. In the soldier
  template, `setSkeletonCollisionBone bone material offset radius length`
  (`0x004deb29` → `0x005c0930` → `0x008b7db0` → `0x008b7d40`: 0x24-byte
  records, offset `+0xc`, signed length `+0x18`, radius `+0x1c`, material
  `+0x20`) places a capsule in a frame of its own (`0x008b6fb0`):
  translate(ox, oy, ∓oz) × Rx(±90°) × the bone, by the length's sign. In
  the bone's frame it starts at (ox, oz, oy), or (ox, oz, −oy) for a
  negative length, and runs `length` along the bone's −y. The capsule's
  material (25 head, 77 limbs, 24 body) selects the projectile's damage
  modifier. The hit test (`0x006fb4xx` for class 0x9493): the ray is cut to
  the soldier's bounding sphere (`0x006fa7f0`), then `0x008b75c0` tries the
  capsules in the records' order and the first one the segment enters
  (`0x006bde70`: the segments' distance against the radius, then the
  exact crossings of the cylinder and its end spheres, a root strictly
  inside the segment) is the hit, not the nearest. Its normal is the hit's
  offset from the axis. Across objects the nearest hit counts. The rewrite
  takes the roots along the tick's whole segment.
- **Death:** when the armour (`armor.maxHitPoints`, 100 for soldiers) runs
  out, the scripts get `PlayerKilled` (victim, attacker, weapon, assists,
  soldier) and `DropKit`, and the soldier goes man down. Its body lies
  where it fell for `sv.manDownTime` (15 s) as a ragdoll (see
  [Ragdolls](#ragdolls)); its bone capsules follow it, so shots and
  shocks still find it.
- **Revives:** while a soldier is down, a teammate's defibrillator shock
  (projectiles with `damageMandownSoldiers`; material 89,
  "Soldier_resurrection") revives it. The shock's `ResurrectCollisionComp`
  (`0x005e7a70`) hands its `collision.resurrectDamage` (20000) and
  `collision.restoreHP` (120) to the game logic (`0x004c1960`): the
  resurrect damage times the material cell brings the armour back up, and
  hit points under `restoreHP` are raised to it through the armour's
  setter (`0x00655460`), which clamps to `maxHitPoints`: the soldier is
  back at its full 100. The scripts get
  `PlayerRevived`, and the medic says `get_up` (radio event 9). The soldier
  stands up where the ragdoll lies with no get-up animation: BF2142.exe
  never names the `reviveOnBack`/`reviveOnStomach` triggers (its named
  trigger lookups, `0x008c0e70`, are the prone transitions), it only leaves
  ragdoll mode (`0x005f0f20`). An enemy's shock finishes the body off (a
  team test of the rewrite's: `0x004c1960`'s man-down branch reads none,
  and its caller `0x005a3bd0` is not traced that far).
- **Final death:** at the end of man down, or when the player respawns
  (after `sv.spawnTime`, 15 s), the scripts get `PlayerDeath`. With the
  real `bf2/stats` and game-mode scripts, this is when the team loses its
  ticket. A soldier killed in a vehicle dies at once: slumped in the seat
  when the seat has a die bundle (see Seats), else put out at the seat's
  exit as a ragdoll pushed forward by `rd_dieInVehicleForce` (100,
  `0x005f0f50`); in a vehicle that is destroyed the body goes with it.
- **The body afterwards** (`GameWorld::Corpse`): it outlives `PlayerDeath`
  as a ragdoll, counted down by its armour (`0x00652380`): once landed
  (`0x005eda20`) it waits `timeToStayAsWreck` (`+0x158`, 20 by default,
  `0x00630040`), or 60 s if it never lands (`*0x00a79388`), then
  `timeToStayAfterDestroyed` (`+0x15c`, 0), and goes. Meanwhile it
  fades (soldiers only, class `0x9493`): the geometry's (`+0x44`) game
  alpha (`+0x28`, set through slot `0x104` times the view-distance fade
  `+0x2c`) is the time left over timeToStayAfterDestroyed, clamped to
  0..1 (`0x00402dd0`), and RendDX9's skinned mesh draws with alpha under 1
  in the blended bucket (`0x10162070`). It neither sinks nor shrinks.
  The soldiers' armour (their `.tweak` files) sets 6 and 1: six seconds
  after landing (or after the player detaches, at PlayerDeath), gone in a
  second. The rewrite: `world::Object::fade`, drawn by the renderer's
  faded mesh pipeline after the opaque objects.

## Squads

A team has up to nine squads of six. The first member to join leads, and
when a leader leaves the next member takes over. The scripts get
`PlayerChangedSquad (player, old squad, new squad)` and read `sqid` and
`isql` for the player.

Members may spawn on a living leader who is on foot (scattered round it,
below) or
at the leader's spawn beacon. The beacon is the `beacon` gadget, slot 8,
whose `spawn_beacon` has `armor.holdsSlBeacon`; the leader sets it down a
metre ahead, one per squad. On the spawn screen both are clickable
markers. Bots join squads with room and spawn on their leader when they
can. The engine spawns them through a `SquadLeaderSpawnPoint` child of the
leader's soldier (`0x004c7f10`; a SpawnPoint with
`setScatterSpawnPositions 1`: the setter `0x005b1c80` stores it at the
template's `+0x90`). Its position (`0x005b2200`), for a soldier parent:
from the soldier's origin less 0.875, a heading of the leader's ± 70°
(140 × U − 70) and a distance of `spawnpoint-scatter-mindist` + (maxdist
− mindist) × U (3 to 9 m, drawn once), five tries turning on by 0, 72,
144, 216 and 288° more (cumulative, so the last two repeat the first two
directions), each behind the leader. A try (`0x005b1e70`) at the full
distance and then half: the terrain's height and a ray 4 m down from the
point at the leader's height through the objects; the member stands on the higher of
the two plus `spawnpoint-terrain-offset` (1.01), when that is within the
probe, and the way from the leader's point to it is clear
(`0x006f2d20`). The member faces the leader (`0x005b1d30`). The spot
must also hold the soldier (component `0xc4d7` slot `0x14`, `0x00585d00`
→ the response's slot `0x98`, `0x006c3340`; `physics::soldierFits`): a
stack of spheres from the lowest centre (origin − 1 + 0.25) up, five to
1.7 m standing, three to 1.4 m crouched (`*0x00a84798`) and one to 0.8 m
prone (`*0x00a84778`), clear of the static meshes and vehicles. Standing,
else prone (the spawn point's `+0x18a`, `PlayerInfo::spawnedProne`), else
the next try. Not traced: an extra test some callers ask for
(IAIPathfindingCourier slot `0xc`, `DAT_00b4bb94`). When no try fits, the
rewrite puts the member a metre behind the leader.
Not in yet: the squad menus (invites, names).

## Commander

Each team has one commander. The SquadManager (`dice.bf.SquadManager`,
vtable `0x00929d40`) keeps it: slot `0x74` sets it (`0x004c8120`), slot
`0x78` reads it (the scripts' `host.pmgr_getCommander`, `0x0056a530`), and
slot `0x64` pops the next applicant (`0x004c4fb0`). The game modes turn the
commander on with `host.sh_setEnableCommander(1)` (`0x00568640`, event
`0x47`); squad deathmatch turns it off.

- **Applying:** `Commander.commanderApply` (HUD `0x007d7050`; the
  Commander event's kind 4, slot `0x60` → `0x004c7160`) puts a player on
  the team's list of applicants. The first application while nobody is
  being offered command starts the apply period: event 7 after the
  SquadManager's `+0x14` (15 s, constructor `0x004c96c0`). Then
  `0x004c5070` takes the applicant with the highest rank (player slot
  `0x1a8` `+0x20`), the first of equal ranks (`0x004c4fb0`), off the list
  and offers it command (PostRemote 0x23). Its answer is kind 0 (slot
  `0x68`, `0x004c8c20`: it becomes commander) or kind 1 (slot `0x6c`: the
  next is offered at once). The client bars an applicant for minutes
  after a mutiny (player slots `0x140`, `0x148`).
- **Taking over:** the new commander leaves its squad (`0x004c7bb0`) and
  gets the commander flag (player slot `0x124`); the other applicants hear
  `HUD_COMMANDERAPPLY_POSFILLED` (PostRemote 0x22) and the list is
  cleared. The scripts hear
  `ChangedCommander` (event 10: team, old commander, new commander). The
  player parameter `commander` is that flag (player slot `0x128`).
- **Resigning:** `Commander.commanderResign` (kind 2, slot `0x70` →
  `0x004c8a60`) clears the commander (PostRemote 0x25); with applicants
  waiting, the next is offered command after the SquadManager's `+0x18`
  (10 s).

The commander's tools are the HUD's `Commander.orbitalStrike`,
`.artillery`, `.uav`, `.satellite` and `.supplies`. All but the supply
drop need an asset: an object placed by the level's spawners at each
team's base, whose template holds a `RemoteControlledObject` child. Its
`rcType` names the tool (`0x004daf74`: 1 RCArtillery, 2 RCSatellite, 3
RCUav, 4 RCOrbitalStrike):

| Asset (Weapons_server.zip) | rcType | minTimeBetweenActivation |
|---|---|---|
| `commander_emp_strike` (strategic object COS) | RCOrbitalStrike | 115 s |
| `commander_orbital_strike`, `_16`, `_64` (COB) | RCArtillery | 85 s |
| `commander_uav_drone_control` (ComUAV) | RCUav | 5 s |
| `commander_movement_analyst` (RADAR) | RCSatellite | 5 s |

The names are crossed over: the RCOrbitalStrike station is the
`commander_emp_strike` template. Each asset has 1000 hit points and
`armor.canBeDestroyed 0`. Shot down, it stays as a wreck that can be
repaired, and its tool is gone until then. 0x005a1440 gathers a team's
standing assets of a type (`DAT_00a69cb8` holds them per team and type)
and takes the largest `minTimeBetweenActivation`.

- **Orbital strike and artillery:** the commander's event (vtable
  `0x0091e42c`, server handler `0x0050ee10`) is honoured only from the
  team's commander. `0x005a11c0` puts the target on the ground (the higher
  of the terrain and the water). `0x005a0bf0` moves the asset's firearm
  `orbitalBombardmentHeight` (500 m) over it. The delay is the firearm's
  launch delay in whole seconds: `fire.fireLaunchDelay` (the fire
  component's slot 300, `0x005f16f0`, template `+0x1c`), rounded, times 30
  ticks. `0x005a0e50` then fires the firearm straight down `(0, -1, 0)`
  for its burst. The ballistic solution `0x005a0750` only turns an
  artillery asset's barrels. `0x005a1270` refuses a use until
  `minTimeBetweenActivation` has passed since the last.
  - The orbital strike's firearm has `fireLaunchDelay` 1.5 (2 s) and one
    round. Its projectile leaves at 100 m/s with five times gravity, hits
    for 3000, and explodes for 1666 over 15 m with `isEMPWeapon`. So it
    lands about 4.6 s after the call, kills soldiers within about 14 m and
    shuts down vehicles.
  - The artillery's firearm fires 16 rounds at 60 a minute. Each round is
    `fire.batchSize` shells (5; 3 for `_16`, 7 for `_64`) at 10 m/s, spread by
    `deviation.subProjectileDev` (35, 25, 45; see Deviation below: 45 is
    4.5 m/s, one standard deviation, on each axis across the fall). Each
    shell hits for 150 and explodes for 30 over 8 m.
- **UAV and satellite scan** (`DAT_00a6a390`, `0x005a23b0` and
  `0x005a2450`): with a standing asset, the UAV stays over its point for
  30 s (gameLogic `0x228`). It is ready again `uavReloadTime` (5 s) after
  that. Its radius is 100 m (`0x230`, gameLogic slot `0x28c`). The scan
  runs 11 s (`0x238`), then `satelliteScanReloadTime` (16 s). They reveal
  through the server's choice of what each client hears of
  (`0x0057dc90`, `0x0057cfb0`): with the UAV up, every player of the team
  gets the enemies (their soldier or vehicle) within the radius of the
  UAV's point, in 3-D; with the scan running, the team's commander, and
  only the commander, gets every enemy.
- **The UAV's drone** (the UAV manager, `DAT_00a6a090`, constructor
  `0x005a1a00`, vtable `0x00967f68`): the RC object's activation
  (`0x0050eb90`) raises ECGameLogicGLUAV; the manager (`0x005a20a0` →
  `0x005a1e40`) looks up the template named `UAVControlObject` (whatever
  the asset) and creates its `uavVehicleTemplate` (eu_drone: mass 500,
  1000 hit points) for the team, unturned, then places it (`0x005a1a40`):
  R = `uavVehicleRadius` (40), v = `uavVehicleSpeed` (14), a turn rate of
  asin(v / 30R) × 57.29578 × −0.5263158 (`0x00967f6c`) radians a second
  (−0.3518), so a circle of v / |rate| = 39.8 m. It starts at x + R, at
  the highest ground or water at the point and along four lines out from
  it (x ± √((R + 5)² + i), z ± (i / N)(R + 5), i < N =
  `uavVehicleSampleResolution`: "+ i" as the exe has it) plus
  `uavVehicleFlightHeight` (70) and gameLogic's
  `extraUAVVehicleFlightHeight` (`+0x234`, 0). Each game-logic tick
  (`0x005a1f70`) sets its yaw rate and its forward × v as its velocity:
  round the circle from +x towards +z, a lap in 17.9 s. It goes
  (`0x005a1d90`, ECGameLogicGLUAVVehicleDestroyed) when the UAV's time is
  up or the team's RCUav assets are wrecked (their hit points over their
  maxima, `0x005a2850`; 1 without any), and when shot down (the team's
  HUD hears ECHudHEUAVVehicleDestroyed, `0x005a1c90`). The reveal stays
  round the point either way. The rewrite flies it kinematically
  (`GameWorld::uavDrone`); the events aren't raised.
- **Supply drop** (`0x0043d6d0`): every `SupplyDrop-Reload-Time` (60 s,
  less 0.1) per team, without an asset. `0x0043d510` creates a
  `supply_crate` at the ground's height plus `gameLogic.supplyDropHeight`
  (50). `0x0043d880` removes it after `supplyDropNumSecsToLive` (300). The
  crate is a `SupplyObject` (template `0x005d1940`), updated every 0.2 s
  (timer event 0x1d, `0x005d1710`). Its team's soldiers, and vehicles
  (`workOnVehicles`), within its radius (5) plus their own (`0x005d0e80`)
  are healed by `healSpeed` × 0.2 per cent of their maximum (3 a second
  for a soldier) through gameLogic slot `0x108` (`0x004cc380` → the
  armour's slot `0x48`, `0x00655a00`: scaled by the material cell for 73
  and 24, the same for every target) and their weapons get `refillAmmoSpeed` × 0.2 per
  cent of a magazine (7 % a second; the ammo component's slot `0xa8`,
  `0x005f5110`). Both draw on 500 of shared storage, and the crate goes
  when that is spent (`destroyWhenEmptyStorage`).

The gameLogic defaults come from its constructor (`0x004d69d0`);
`GameLogicInit.con` sets the supply drop's height and time to live.

Orders: `SquadLeader.sendOrder <kind> <bool>` and `Commander.sendOrder
<kind> <bool>` both run HudManager slot `0x3f4` (`0x007d6ab0`) with the
order armed on the map. The kind is a byte on the wire (event `0x0050dfe0`):
0 move, 1 attack, 2 defend, 3 repair from the issue-order menu
(`HudElementsIssueOrders.con`), and 4 move, 5 attack, 6 defend, 7 repair
placed on the map (the right-click menus' `sendOrder 4 1` .. `7 1`; a map
click arms 4, or 5 on an enemy and 6 on a friendly object, `0x00753c90`).
A kind of 10 or more drops the armed order. The bool adds the minimap
cursor as the order's next point (`0x00780820`; up to three). A squad
leader's order goes to his own squad, accepted (SquadManager slot `0x48`,
`0x004c5360`: the squad's leader order at `+0x30`); the commander's goes
to each selected squad with members (slot `0x4c`, `0x004c5530`: the
commander order at `+0x120`), whose leader accepts or rejects it
(`Commander.commRoseAccept`; a rejected order goes, `0x004c2f60`). The
scripts' `orderPosition`, `orderRadius` and `ifAcceptedAttackDefendOrder`
(player slots `0x15c`, `0x164`, `0x16c`) read the leader's order (squad
slot `0x10`, `0x004c2ee0`); only kinds 5 and 6 count as attack or defend
(`0x00592160`), and `orderRadius` is the player's own 80 (`+0x144`, set by
`0x0059c010`).

AIDLL.dll's commander is not a player: bots never apply. For a team
with bots (IAIPlayerFactory slot `0x2c`) and no commander it calls the
game's `IBFCommander` (`0x009d6618`). Every 10 s it looks for a point of
its own being taken and shells it with the artillery (slot `0x20`,
`0x00896f00`: the tool event with rcType 1 and no sender, which
`0x0050ee10` lets through), then waits 120 s (`0x1009c050`). The orbital
strike (90 s), supply drop (35 s) and UAV (60 s) answer its squads'
requests (`0x1009e200`, `0x1009e3e0`, `0x1009e500`).

Rewritten in `src/Game/Commander.cpp` (`GameWorld::setCommander`,
`applyForCommander`, `answerCommanderOffer`, `resignCommander`,
`abilityState`, `commanderAction`, `issueOrder`, `issueSquadLeaderOrder`,
`acceptOrder`) and `BotManager::command`. Bots follow their squad's
commander order (the control point it lies on, or the marker). Not
rewritten:

- The firing's build-up of deviation (`setFireDev`): the rewrite's
  templates keep one argument of the scripts' three.
- Without a body the crate comes down at
  5 m/s (its parachute's Wing and ForceObject only act on a rigid body);
  a soldier's own radius in the crate's reach is taken as 0.
- The bots don't send squad requests, so the AI commander's answers
  (orbital strike, supplies, UAV) never run.
- Which order kind the minimap draws with which icon (it loads move,
  attack, defend, repair, destroy, mine) isn't traced.

In `bf2142` the commander's HUD buttons run console commands:
`Commander.commanderApply` / `commanderResign`, the tool toggles
(`toggleOrbitalStrikeState`, `toggleArtilleryState`, `toggleUAVState`,
`toggleSupplyState`, and the rose's `orbitalStrike`, `artillery`, `uav`,
`supplies`) arm a tool that the next click on the map uses there
(`HudManager::mapWorldAt` turns the click into a world point),
`satellite` scans at once, `sendOrder` / `issueOrder` arm an order for the
squad whose leader is nearest the click (the commander screen's squad
selection, HudManager `+0xe4` slot `0x34`, isn't drawn yet), and
`commRoseAccept` answers one. `SquadLeader.sendOrder` and `Commander.sendOrder
<kind> <add>` share one handler (0x00770889): the squad leader's order goes to
his own squad, the commander's to the squad whose leader is nearest the click
(the screen's squad selection is not drawn yet). `Commander.accept 0/1`
answers the offer of command that follows the 15 s apply period. Caps Lock opens the commander's screen (the
full map and `CommanderShow`); the tools' `CommanderNo*Show`, `*Armor`,
`*Reloading`, `*TimeUntilReloaded` and `*ShowFullStatus` come from
`abilityState`, and the minimap shows the UAV, strikes on their way,
supply crates and the squad's order.

## Bots (AIDLL.dll)

`AIDLL.dll` exports only `initDll` and `deinitDll`, and it has about 6,400
functions. Rewritten in `src/AI/Behaviour` (selection), `src/AI/Strategy`
(the strategic AI), `src/AI/PathFollower` (following paths, driving) and
`src/Game/Bots` (scheduling, sensing, the patterns, aiming).
The settings come from the game's own scripts: `aiSettings.*` in
`AI/AIBehaviours.ai`, `weaponTemplate.*` in `Objects/**/ai/Weapons.ai`,
`aiTemplate.*` and `aiTemplatePlugIn.*` in `Objects/**/ai/Objects.ai`,
`aiStrategy.*` in `AI/AIDefaultStrategies.ai`, and `aiStrategicArea.*` in
the level's `GameModes/<mode>/<size>/ai/StrategicAreas.ai`. Without those
scripts the shipped values are assumed: the built-in rifle stand-in and
one area per control point, each a neighbour of every other.

The AI templates name, per seat (`ObjectTemplate.aiTemplate` on the
PlayerControlObject), the equipment type whose behaviours a bot there
runs (the Unit plug-in's `equipmentTypeName`: Car, ArmedCar, Tank,
FloatingTank, Mech, Fixed...), what a vehicle counts as for weapon
strengths (Physical `setStrType`), the Mobile plug-in's driving settings
and the ControlInfo `yawScale`. A carried weapon's AI template is its
object's `aiTemplate` (Unl_Hub_Medic is MedicHub).

### Scheduling (BotManager::update 0x100ca590)

AIMain::update (0x100571c0) gives the bots what is left of the frame's
AI budget after one side's SAI (the sides take turns). The budgets are
real time: the deadline is the `IBFTimer`'s clock (its slot `0xc`,
BF2142.exe `0x0089f420` → `0x0089f380`, QueryPerformanceCounter) plus the
frame's time times AIMain's fraction (`+0x8`), and the bot manager
(`0x100ca590`) gets what is left of it when the SAI is done. Which slices
fit before the deadline depends on how fast the machine running the
server is, so it can't be reproduced from the binaries; the rewrite runs
each slice to its count, as a machine on which no budget runs out would:

- **Plan execution** (0x100c91c0): about half the bots a frame, round
  robin: N/2 + 1 and N/2 on alternate frames for an odd N, N/2 + 1 for an
  even one. A bot skipped while moving keeps its last input for up to 15
  more frames, but for PIFire, PIAltFire, PIReload and PILie (0x10098c00);
  after that, or standing, 0x10097e60 clears the movement, the look and
  the buttons, and a driver brakes (PIThrottle −clamp(0.5 v·forward,
  ±0.4)).
- **Thinking** (0x100c95f0): a bot gathers the time of the frames it
  executed. With a plan it thinks again after 0.5 s of them
  (`setPlannedDecisionMakingThreshold`), without one after 0.3 s
  (`setUnplannedDecisionMakingThreshold`). Each frame up to 2 planned and
  `max(0, 2 − planned waiting) + 2` unplanned bots think, longest waiting
  first (`setDecisionMakingInterleave 2 2`).
- **Sensing** (0x100c9980): the two bots that executed this frame and
  waited longest. With 16 bots at 30 frames a second each looks about
  every 8 frames.
- **Paths** (0x100c6ee0): the searches run time-sliced in the original;
  here they finish at once.
- **Inside a think** (0x10099280): the behaviours are evaluated in the
  order of their priority values (a list per level, lower first). When
  the think's budget runs out, the rest move up a level for the next
  think. A behaviour not evaluated keeps its last urgency (the BBs store
  them per bot); the rewrite evaluates all of them every think.
- Rounds passing within 10 m are noticed every frame (0x1004ed30 is an
  event).

### Behaviour selection (Bot::think 0x10099280)

Each equipment type has its own behaviour set: Infantery on foot, and
Tank, Car, Fixed and so on in vehicles (`aiSettings.setVehicleBehaviour
<type> <behaviour> <BB> <BBP> <priority> <curve> <inhibitors>`). A BB
class computes the urgency (vtable slot 9, `urgency(bot, full, weight)`)
and its BBP pattern builds the action plan. Every think:

- **Weight:** `personality[b]` (StandardWeights: Idle 0.1, Fire 7.5,
  Change 1.9, Revive 3). While a behaviour runs, that is multiplied by its
  urgency curve at the time since the last think (0x100ea430) and by its
  inhibitor set. For example, AvoidInhibit holds MoveTo to 0.3.
- **Curves:**
  - UCConstant: `a` (0x10087ec0).
  - UCLinear: `max(0, a t + b)` (0x10087ef0). UCFire is -0.22 / 1.3.
  - UCXInverse: `max(0, a / (c t + b) + d)` (0x10087f50).
- **Urgency:** only behaviours with a weight above 0 are evaluated. Each
  pattern decides whether to scale by the weight.
- **Arbiter** (vtbl 0x1011b868): it latches every urgency and runs the
  largest. Ties go to the first in set order. With none above 0 it runs
  the set's default behaviour. It picks again only when a urgency leaves
  [0.87, 1.15] times its latched value, or goes from zero to non-zero or
  back (0x100f7030, 0x100ccdd0).

The urgencies:

| Behaviour | Urgency |
|---|---|
| MoveTo (0x10066a70) | The goal's (0x1008aaa0) × weight. r = max(ceil(radius), bot radius + 1). 0 within r (reached). Otherwise clamp(d² / (1.5 r²), 0.1, 1) × 2 for an area the team does not hold. Inside an area it holds the urgency is 0; outside it, d² − area radius² is used. |
| Idle (0x10069f50) | The weight. A passenger gets 0.5. A driver with free seats gets 30 while a teammate within 40 m heads for it (70 when that teammate faces the vehicle within 0.85), for up to 10 s. |
| Fire (0x10075fb0) | 50 while a known enemy scores above 0. The weight only gates it. |
| Revive (0x10082760) | 100 for a downed teammate within the Revive weapon's maxRange. Each candidate scores 1 − min(1, d / speed / time left). |
| Flee (0x100859d0) | 100 below 30 % health, with the camouflage over 70 % charged (0x10085920: its ammo over its maximum), 10 s after spawning. 0x10085d90 keeps the way out: the sum of the directions away from known enemies (×3 for players, ×2 for bots targeting it). |
| Change (0x1006c520) | best seat / current seat × weight × 10 (best × weight with the current worth nothing), ÷ (1 + 14 clamp((d − 10) / 60, 0, 1)) for the distance to it (0x1006bc50), 0 for a seat another bot reserved (0x1006f320). See Seat values. |
| MedicAssist (0x10070200) | Teammates on foot within the hub's maxRange (70 m) below 75 % health (99 % for a human), the bot itself unless it holds the hub. Score basicTemp / (fuzzy(1 − health) × distance), distance 1 for itself. fuzzy(best) × weight × 2, × 10 for a human; × the squad factor. |
| AmmoAssist (0x10071a10) | Human players only. A weapon (FireArm, Sniper, C4) with ceil(rounds / magSize) / magazines below 0.6 (0.99 while the bot holds the hub). Score basicTemp / (scurve(ratio) × distance). fuzzy(best) × weight × 4 × the squad factor. |
| RepairVehicle (0x10073360) | Vehicles within the tool's maxRange (50 m) between 0 and 60 % health. Score basicTemp / (f × distance), f = fuzzy(1 − health), or 1 for the one it repairs. fuzzy(best) × weight × 10 with a human aboard, × 2 otherwise; × the squad factor. |
| c4 (0x10083750) | 100 to place a charge inside a point the team holds uncontested; 100 to set it off once that point is contested below 0.75 with no teammate within 15 m of it. A charge lapses after 240 s. The weight is not used. |
| DefuseMines (0x10086870) | An engineer (Repair weapon) within 35 m of an enemy mine's stand point (0.8 m behind its facing, on the mesh): 42 × weight. |
| RunOver (0x10087820) | A Car at 10 m/s or more with a known enemy soldier in sight within 50 m, ahead within 0.85, on a clear line over the vehicle mesh: 65 (the weight is not used). |
| Avoid (0x10064850) | On foot only (the 2d pattern of the vehicles' sets never runs); soldiers are never avoided. A wreck or vehicle below 30 % health within max(r, min(10, r + 4)) adds 100. A vehicle at 2 m/s or more, closing at 3 m/s or more, that will pass within 0.6 (r + own radius) × √1.5 in the look-ahead time adds closing speed / distance. Sum × weight. |
| BridgeRepair (0x10084620) | 0: the rewrite has no broken bridges. |

- **Squad factor** (0x10089aa0): 0 outside squads; 1 for the bot's own
  task (Medic 1, Repair 2, Ammo 4). For a target outside squads: 0 when a
  teammate has the task or the bot's squad is more than 141 m away. In the
  bot's squad: 0 when a squadmate has it. In another squad: 0 when taken
  or that squad is 31.6 m or more away, else 0.5. ×2 for a human.
- **Curves:** `ai::fuzzy` (0x100cbce0, table 0x100cb6f0) takes the entry
  at trunc(100 x); `ai::scurve` (0x100cc930, table 0x100cc350)
  interpolates its own S-shaped table.
- **Plans:**
  - MedicAssist/AmmoAssist (0x10070da0, 0x100724e0): walk to the target,
    look at it, take the hub within half its abilityRadius plus the two
    radii. The bot itself: crouch and take the hub. Nothing presses fire:
    a hub (no `onlyActiveWhileFiring`) works round its holder while in
    hand (the ReplenishingAmmoComp's tick, BF2142.exe 0x005d8740), so
    holding it heals or resupplies.
  - RepairVehicle (0x10073da0): walk to the vehicle's radius, take the
    tool, hold the trigger while facing it (dot above 0.1). Crouch when it
    is more than 0.5 m lower.
  - c4 (0x10083ae0): no movement. The charge in hand, 0.7 s, fire, then the
    primary weapon. Setting it off is alternate fire here.
  - DefuseMines (0x10086fe0): walk to the stand point, crouch, look at the
    mine, hold the trigger of the first Healing weapon. That is a slip in
    the original (the urgency asks for a Repair one); an engineer has none,
    so nothing is fired.
  - RunOver (0x100ddd30): steer at the lead point (3 × yawScale × angle −
    turn rate), full throttle.
  - Avoid (0x10065360): walk 10 m from a wreck (straight away, else to
    either side, by mesh rays), or 10 m across a vehicle's motion on the
    bot's side. It ends after the time it allows or within 2 m.
  - Flee (0x10085fc0; the point 0x10085de0): ChangeWeapon(camouflage),
    LockPlan(on) (0x100ce070: bot+0x7c, so 0x100cd090 keeps the plan with
    no arbitration), then side by side a sprinting MoveToFinding to 50 m
    along the way out plus U[−20, 20) m on x and z (ten tries for a point
    on the mesh; none: no run) and While(camouflage not empty and under
    4 s) TriggerContinously(its fire); when both end, the slot 2 weapon's
    select key, LockPlan(off). The rewrite's camouflage has no charge, so
    only the 4 s end it.
  - Idle (BBPIdleInfantery 0x1006a480, plan 0x1006a500): Parallel(Idle)
    under While(True). The Idle statement (0x1008f840 → 0x100e6120) lets
    go of the move and the trigger, gives no yaw and levels the pitch: an
    idle soldier never turns. Within its own strategic area (its order
    position's radius + 2 m, 0x10034a10) it crouches (SoldierPose 2) and
    taps its weapon's alternate key once (the weapon adaptor's vt+0x68,
    on when vt+0x70; not in the rewrite's weapons).
  - The assist plans and DefuseMines never end on their own; the urgency
    drops them.

### Sensing (0x100ed700, 0x100ee430)

- **Reach:** viewDistance × cos(fov / 2), with fov 100° from the table at
  0x1012b074. This reaches into a square frustum of half-angle 50°.
- **Rays:** `clamp(round(30 r / d), 1, 10)`. The target is seen if any ray
  is clear.
- **Sharing:** sightings go to the others in the same vehicle (0x100a8ce0).
- **Shot at:** a round within 10 m makes the bot aware of the shooter in
  every direction for 5 s (0x1004ed30).
- **Forgetting:** a target out of sight is lost; it is forgotten after
  60 s, or at once for one in a vehicle.
- **Skill:** it does not change sensing.
- **Hearing:** the roll (0x100ec930) cannot succeed for a listener with a
  Mobile plugin, so soldiers never hear. This is kept.

### Fire

- **Weapon choice:** each weapon's strength against the target's type
  (`weaponTemplate.setStrength`), ×1.03 for the weapon in hand. Healing,
  Ammo, Repair and Camouflage weapons score nothing. FireAndForget counts
  only when facing within 0.9, and C4 only within 4 m.
- **Target score:** `max(0, 1 − d / (1.2 maxRange)) × strength × 5 ×
  freshness`. Freshness is 1 in sight and `1 / (0.05 t + 1.5)` once lost.
  The current target stays unless another scores 1.2 times more.
- **Distance kept:** the bot stays within [minRange, 1.1 × preferred],
  where preferred = minRange + (maxRange − minRange) × optimal ×
  max(0.2, skill) × U[0.8, 1.2] (0x100795a0).
- **Firing pose:** taken when standing still, at most once a second, with
  no fire for 0.8 s after (Trigger 0x1008f1e0).
- **Tap or hold** (0x100777a0): TriggerContinously (0x1008ecd0, held while
  its conditions hold) only for a weapon whose fire mode is 2 (aiWeapon
  slot 0x104 → BF2142.exe 0x00892650: the fire component's `+0x18`, which
  starts at the first `fire.addFireRate`, or 2 without one, 0x005f28a0;
  the bots never toggle it). Anything else gets Trigger (0x1008f1e0, once):
  pressed on one plan execution, let go on the next. Fire rate and
  distance play no part. So the assault rifles, machine guns and the like
  hold; pistols, shotguns, snipers, rockets and tank guns tap.
- **Heat** (0x100d9000, 0x100d9040): with `heatAddWhenFire` over 0, the
  trigger stops at heat 0.8 and starts again at 0.5.
- **Teammates:** a teammate crossing the line of fire within 5 m holds
  the trigger for 1 s (0x100dad90) for firearms and snipers that aren't
  thrown.
- **Reports:** a bot in a squad reports its target to its squad as its
  trigger goes down and comes up (0x1009a1d0, 0x1009a2a0), which the
  sentry drones' filter reads (see the drones).
- **Reaction time:** (1 − skill)(1.5 + U[0, 1]) s at a new target
  (0x100777a0).
- **Aim:** the PID controller of the ControlInfo plugin (0x100e5820:
  proportional −7, derivative −0.01, angle speed 0.4 on the lead point;
  yaw limited to 3 × skill + 1). Its output is PIMouseLookX/Y, which the
  soldier (BF2142.exe 0x0058a6d0) turns into 5° per unit a call
  (`*0x00a68e4c`, `*0x00a68f24`, 5.0 each; no time step, so 150° a
  second at 30 calls), scaled down by the field of view when zoomed
  (0x00586420), under 0.001 none; the pitch is clamped to ±85°
  (0x005890a0). Standing still, the head turns up to ±40° before the body
  follows at 4° a call (the rewrite's soldiers turn the body at once). The trigger holds while the aim lies
  within allowedDeviation at the target, plus an angle check
  (0x100d9090). It does not fire while a teammate is in the line of fire.
- **Deviation:** each round strays by (1 − acc)(1 − t/2) × deviation,
  with t the time on target over 15 s (BF2142.exe 0x00892b70). acc is
  skill against soldiers.

### Strategy (SAI, 0x100a2dd0)

- **Choosing a strategy:** each side weighs its strategies by their
  prerequisites, the weighted condition values with their
  Required/Advisory rules (0x100a9300). The choice is kept while no weight
  moves by about 20 %; otherwise it is picked by roulette (0x100a0130).
  With the shipped data, both sides run basicAttackEnemy until the enemy
  holds nothing.
- **Area values:** the strategy's modifiers scale area temperatures.
  - Attack value: `100 v / d − penalty` for areas not owned that border
    an owned one.
  - Defence value: `(enemy strength + temp) × 2 × neighbour factor`.
- **Assignment:** the best NumberOfAttacks and NumberOfDefences areas
  (0x100a21a0, 0x100a2630) get squads, nearest first, until each has its
  required strength (`2 × max(enemies, 1)`, ×1.25 if not owned). An area
  releases a spare squad.
- **Orders:** attack or defend orders go to the area's order position.
  Squads under a human commander keep that player's orders.
- **Goals** (0x10060300): a bot heads for a spot within 0.8 of the
  control point's radius when its team does not hold the point. Otherwise
  it heads for `order + U × 0.8W − 0.5W`. It is re-ordered 35 s (20 s for
  a leader) after arriving.
- **AI commander** (0x1009c050; not a player, see "Commander"): for a
  team with bots and no commander it shells the first held point whose
  flag is below 0.75 with the artillery, at most every 120 s and not
  before 120 s.

### Following paths (src/AI/PathFollower)

BBPGotoWaypointInfantery (0x10066d80) and BBPGotoWaypoint2d (0x10068980)
plan a Turn then MoveToFinding (0x100d11c0) to the goal. At the wheel the
move ends within half the goal's radius. MoveToFinding asks the bot's path
follower (bot +0xd8, constructor 0x10094f10) for a path when the goal
changes (0x100966c0 → 0x10095fc0).

- **Path:**
  - First a straight walk over the mesh (0x100952c0): under 200 m, the
    mesh ray to the goal (up to 1000 polygons, no width) not entering cost
    class 5 (or 7).
  - Otherwise the start and the goal on the mesh (vslot 0x38, 0x1003f2d0:
    the polygon each stands on, a height tolerance of 2.5 m for bot kind 3
    and 5 m for the others; none fails the request, 0x100966c0; there is
    no search nearby). Over 100 m (+0x8c) the cluster search runs first
    (0x10095620, vslot 0x24) and keeps {cluster, entry link} per step.
  - Then polygon searches in segments (0x100962e0, vslot 0x20): from the
    last waypoint (the start for the first) to the link into the cluster
    two steps on along the cluster path (0x10092250: links[firstLink +
    entry], its position and polygon), or the goal when there is none.
    The search may enter only the start's cluster, the cluster path's next
    three from the cursor and the segment goal's cluster (vslot 0xb4: the
    successors 0x100bcbb0 refuse any other); a trip of 100 m or less only
    the start's and the goal's clusters. Each node becomes a waypoint
    (0x10095b20, the first replacing the last one kept), the goal on the
    last segment; then the string-pull, the look-ahead twice and the
    cursor on. While the cluster path goes on (0x10095720) the waypoints
    are cut to ten past the current one and the next segment is searched
    from the last. The next segment waits while the path left (+0x14c,
    3-D, 0x10093e50) is 30 m or more and over ten waypoints are ahead,
    unless the current waypoint's cluster is the cursor's index or its
    cluster (a mix-up kept). The rewrite searches a segment a tick (the
    original's are time-sliced), and where a segment's search fails it
    searches the whole mesh: the original fails the move, which measured
    on random 20–100 m trips (clusters 7–26 m) fails 312 of 321 on
    Cerbere, 351 of 393 on Camp Gibraltar and 255 of 397 on Minsk.
- **String-pull** (0x10093610), twice, for waypoints 5 m or more from the
  bot: where the line from the previous waypoint to the next crosses the
  portal, kept max(1, 0.8 radius) (at most half the portal) from its ends.
  Then a last pass without the margin where that line crosses the portal
  itself.
- **Each tick** (0x10095720):
  - **Off the path:** back onto the waypoint whose polygon the bot is on,
    up to 20 ahead (0x10093a20).
  - **Reached** (0x10094050): within 3 m of height (5 m in a vehicle) and
    0.5 m horizontally on foot; in a vehicle max(1, approach), the last
    one also within its radius.
  - **Look-ahead** (0x10093420): within 10 m (20 m) of the waypoint,
    skip to the next while the mesh ray to it is clear (8 polygons on
    foot, 30 at the wheel, stopping at cost class 5 or 7; one a tick on
    foot, up to four at the wheel). When none was skipped, the ray to the
    waypoint itself (1000 polygons) says whether it is blocked (+0xda,
    until planned again). Blocked for 2 s (0.5 s) since last clear
    (0x10095470, `PathFollower::reroute`): with more than one cluster
    step left, the cursor goes to the bot's cluster on the path (all but
    the last step), else to the first one next to it (0x1003f7a0 →
    0x100b0810); the waypoints are cleared and a segment is planned from
    the bot. Neither: the move fails (0x10095120).
  - **The mesh ray** (vslot 0x84 0x1003f930 → 0x1003ce50, the edge step
    0x100b82a0): from the bot's polygon, across the x/z plane from the
    bot's position each time, the edge each polygon is left by (not the
    one back; a crossing within 0.001 of a vertex only when heading out;
    else the nearest miss under 0.001, with no edge to leave by). It is
    clear once the exit point lies within 0.05 m of the end (3-D) or
    0.16 m across. It fails off the mesh, into a polygon the profile can't
    walk, into the stop class, past its polygon count, or (with a width:
    0.5 on foot, `0x101287a8`; 0 at the wheel) where an edge leaves less
    than the width beside the crossing (the original also compares twice
    that with a local it never sets; the edge's length is meant). It also
    fails a climb (0x1003ce50 / 0x1003c220): a run of polygons steeper
    than the map's slope limit along the ray may rise no more than the
    step height. The limits are the level `AI/AI.ai`'s
    `aiPathfinding.map.maxSlope` (stored as its cosine, 0x1003bfb0 →
    0x100afdf0) and `map.allowedThresholdHeight` per map (defaults
    0x100b0440: cos 0, 0.35, headClearance 2); map 0 (Infantry) always
    uses 0.35 (0x1003f930 / 0x1003f810). Drivers' rays (0x100e8170) pass
    −1 polygons, stop class 6 and the climb, and read the last polygon's
    class (class 4: the diagonals ignore it, the sides get full reach).
  - **Progress** (0x10092bc0): a bot pushing its controls that doesn't get
    2 m (4 m) further in 10 s (20 s) fails; the move ends and the behaviour
    plans again. A soldier that hasn't moved for half a second jumps.
- **Infantry** (InfanteryMoveTo 0x1008fc90): the look turned to the
  steering point 5 m ahead (the aim controller turns the soldier), the
  throttle at the order's speed over maxSpeed (0x100e6060), no strafing.
- **Vehicles** (the Unit plug-in's equipment picks the interpreter:
  CarMoveTo 0x10091050 for Car and ArmedCar, MechMoveTo 0x1008d250,
  FloatingTankMoveTo 0x1008c7c0, TankMoveTo 0x10091b40 for the rest),
  from the Mobile plug-in (FAV: maxSpeed 30, speedAtZeroSpace 3, backingSpeed 7,
  brakeDistance 25, maxBrakePercentage 80, lookAheadTime 1.5,
  minAngleDiffForSlowDown 0.2, angleSlowDownScale 1.8,
  maxAngleDiffSlowDown 0.8, spaceNeededTweak 0.94):
  - **Speed wanted:** min(brake, bend) × maxSpeed.
    - brake = max(1 − maxBrake %, space² / brakeDistance²)², space =
      path left − radius − 0.5. It only applies above backingSpeed.
    - bend = 1 − maxAngleDiffSlowDown × min(1, angleSlowDownScale × (a −
      minAngleDiffForSlowDown)). a is the sharpest bend within max(3,
      speed × lookAheadTime) m ahead (0x10093c00), or the heading error.
  - **Size:** the AI object's box (0x100fe990): radius r = its farthest
    corner from the origin (vt+0x28), the centre radius (vt+0x30), width
    and length. The rewrite takes the box of the vehicle collision LOD's
    points.
  - **Space** (0x100e8170): mesh rays to r + 10 m ahead and along the
    box's diagonals (forward × length ± right × width), behind and its
    diagonals, and to either side. The rays behind are cut short by other
    vehicles' spheres (0x100fbdb0). The speed is capped from maxSpeed down
    to speedAtZeroSpace as the space ahead closes (backingSpeed below 95 %;
    in the backing states from the space behind).
  - **States** (one change per time stamp, 0x100973b0): from 0, a goal
    behind within 10 r with room behind is backed towards (8); behind and
    no room to turn (2.7 r across and lengthwise) backs out straight (12);
    otherwise 9/11 (back at the opposite lock) and 2/3 (ahead at full
    lock), alternating with the space ahead and behind, until facing
    within cos 0.9. Boxed in (no space either way): ahead at half speed,
    or backing in state 8.
  - **Other vehicles** (0x100fbff0): the one in the way (within the
    forward speed + max(r, 5) + both radii, the forward line crossing its
    sphere) is passed in state 6 on a side chosen by the room beside it
    (mesh rays from it, cut by other vehicles) or by which side of the way
    it is on: heading for the tangent of 0.8 × its radius + r seen from
    a quarter second ahead. Touching it, states 13/14 back off and 4/5 come
    round. A side that flips stops the avoiding: the driver drives on
    behind it. An obstacle on the goal next to the driver ends the move
    (666). Behind another vehicle within 15 m the speed is capped at its
    speed along the way less 1, at least 2 (0x100fc660); one closing at
    over 10 m/s brakes fully and swerves (error −2). Not rewritten: the
    other driver being told (its +0x28 calls) and the horn for friendly
    soldiers in front (0x100a5c00).
  - **Car** (0x100e9d40): the error mirrored when moving against the
    direction wanted; turn = 3 × yawScale × error − turn rate; the target
    speed lowered on a tilt and capped (keeping the cap's sign); throttle =
    target / 15 + min(1, (target − speed) / 3). Full throttle when off the
    line (under 1.5 m/s).
  - **Car** sprints (PISprint, input 13) at full throttle over 0.8
    maxSpeed.
  - **Tank** (machine 0x100de8a0, controller 0x100e0540): the car's
    machine with a goal behind within 5 r (not 10 r), driving on from
    cos 0 (not −0.2), round an obstacle at ∓0.2 × min(1, speed) (not ±2)
    and on ahead while touching it unless it is straight ahead (dot 0.8),
    the obstacle seen from half a second ahead; the brake's percentage
    not scaled and nothing squared, the bend alone. No steering below
    1 m/s across the ground, the error × min(1, speed / 2)²; the throttle
    term squared. Stopping (0x100e0930): a throttle against the speed
    along the heading, t = −clamp(v, ±0.7): none within 0.05, 0.4 within
    0.4, else t.
  - **Mech** (MechMoveTo 0x1008d250, machine 0x100e1f20, controller
    0x100e3c00; the mechs' Mobile: maxSpeed 8, maxBrakePercentage 0,
    yawScale 15): the tank's machine with each tick starting on the spot
    (direction 0, not ahead), diagonal rays along forward × (length + 1)
    ± right × (width − 1), no side rays (no state 12), and its own
    default: ahead only facing within cos 0.6, else states 15/16 turning
    on the spot until facing within cos 0.9 (states 2/3 turn on the spot
    too). The controller steers at full lock (yawScale × error raised to
    1; on the spot yawScale × its sign) and holds the throttle at the
    direction: no speed regulation, the speed wanted only says when to
    sprint (at maxSpeed and over 0.7 of it). The bend counts the
    machine's error too. Stopping (0x100e3f00) lets go of everything.
  - **Hover tank** (FloatingTankMoveTo 0x1008c7c0, controller
    0x100de350): no machine, no mesh rays, no avoidance. The way to the
    steering point comes from the infantry steering (0x100ce860), sidestep
    included.
- **Sidestep** (0x100ce860 → 0x100e63c0, `sidestepSteer` /
  `findSidestep`): obstacles near the way ahead (a vehicle, a mobile
  object; not the one the move is for: the vehicle being entered,
  +0xe8 via 0x100d4d60, or repaired, +0xec, 0x10073da0) are walked round
  on the side with room, aiming at a point ahead on the path
  (0x100941c0); the move ends when the sidestep says so. The rewrite
  looks the obstacle's polygon up from its position (the original uses
  the AI's nav position). The speed wanted is max(min(
    brake², bend) × maxSpeed, backingSpeed), the brake with the percentage
    × 0.01 at any speed. A velocity loop in the frame the hull will have
    half a second on: along its forward and right axes, ((wanted −
    current) × 1 − acceleration) × 3, or 10 towards the way wanted while
    moving against it, to PIThrottle and to PIYaw, which strafes the
    hover tank; the hull is turned by its PIRoll, the look's. The original
    writes them unclamped; the rewrite's inputs are clamped to ±1.
- **Strategic waypoints** (0x10060300): an order's goal list. From the
  area the bot is in (else the one it last headed for), the areas' route
  goes to the target over neighbours (0x10061d30). For each step of it,
  one of the level's `aiStrategicArea.addWayPoint` points between the two
  areas (either direction, for the bot's map), three random tries: ahead
  of the bot for the first step, closer to the area than the bot, on the
  mesh. Then a point in each area on the way. A waypoint is passed within
  10 m (0x1008aaa0), an area on the way within 60 m or once inside it. A
  pair without waypoints ("do not have waypoints.", 0x10036b50) goes
  straight. Drivers use the areas' Vehicle order positions and the
  vehicle mesh.

### Levels without a navigation mesh

The game runs the level's `AI/AI.ai`, which holds `ai.init 2`, only in
single-player and co-op (BF2142.exe 0x004d3450). Levels without
`AIPathFinding` (Sidi_Power_Plant, for one) ship no `AI/AI.ai` either, so
AIMain never initiates and `ai.addBot` is refused. In multiplayer,
`AIDefaultMultiplayer.ai` sets maxNBots 0. Missing `.qtr`/`.clb` files
load silently as no trees (0x100ae4e0); `ai.init` never fails
(0x10056b20). `BotManager::setNavMesh(nullptr)` does the same:
`enabled()` turns false and the manager runs no bots.

- **View distance:** `aiSettings.setViewDistance`, else (−1, AIDLL
  settings +0x1c) the game's at the AI's start on the level (0x10056b20:
  IAIGame vt+0x58, BF2142.exe 0x008959f0): the distance into the fog
  (`Renderer.fogStartEndAndBase`) where its density t × base (below w,
  else t³ + w) reaches 0.8, t in steps of 0.01, at most
  `GameLogic.maximumLevelViewDistance` and 300 m (2142_Shipment: 30 m).
  `BotSettings::levelFog` must be handed the level's fog.
- **Strategy conditions** (0x100a9620, 0x1005cce0, 0x1005ce20): a
  constant condition reads the friendly side's value for Friendly and the
  enemy's for anything else (Both included); the compares test v − k
  (Equal…, Difference…) or v / k (Quotient…) against 0. The route's cost
  factors read the neighbour's per-side status (+0x70, 0x1005f210: 0 held,
  1 the enemy's, 2 neutral or its own point with the flag below 0.9).
- **Goal spots:** order + U × W × 0.8 − W / 2 on each axis with W twice
  the area's radius (0x10063460, 0x10063de0).

### Seat values (0x100d3e70) and Change

A seat's value is health × (k × weapons + move), k 2 for a vehicle seat
with an Armament plug-in, else 1:

- **Refused (0):** a seat another bot reserved (+0x2c/+0x30); with
  `other`, one whose driver's seat is taken; a vehicle whose Unit team is
  not 0 or the bot's; below 0.3 health (AIMain +0xdc; × 0.3 for the
  vehicle the bot drives); the root seat of a non-ITAir vehicle a human
  of the bot's team got out of under 7 s ago (the unit component, vtable
  0x009d6af0, vt+0x60/+0x64 per team, −1000 at first, 0x008a1d70; set by
  the exit event 0x00894860 for players that aren't bots, player vt+0x68);
  more than 8 m over the terrain (not in the rewrite).
- **Weapons** (0x100d3b80): for each of the seat's guns (on foot the
  kit's), its strength against each object the bot knows (by its
  CombatStrengthType), plus at a vehicle seat with Mobile its eight
  strengths summed; then, not for the bot's own soldier and not 0, +4 for
  light armour, naval, helicopters and planes, +10 for heavy armour (+2
  and +5 with `setHasExposedSoldier`).
- **Move** (0x100d39a0) × 0.3, with a Mobile plug-in on the root: its
  maxSpeed × 0.5 × 1.5 at the root's seat; at another, × 5 behind a
  driver of the bot's squad, × 1 of its group, 0 without a driver.
- × 1.15 for the seat the bot reserved itself.
- **Transport** (0x100d3730, 0x1005d840): `orderClose` (the order's
  spot within its area), `transportClose` (the bot's or the vehicle's
  second position), and `threatened` (sensing +0x7c, 0x100ebdd0). Close:
  no move value; close and not in the area: no weapons either. The root
  seat stays close only when the order's spot is outside a capturable
  control point (area flag bit 16; capturable = not
  `unableToChangeTeam`, IAIGame vt+0x24 0x00895d90) or when threatened
  and unarmed.

Change (0x1006c520): the vehicles within 75 m (IAIInformationEnvironment
0x100ca9e0; one without Mobile within 75/√2 m), on the vehicle mesh, each
seat at its value × max(0, min(0.5, (R² − 0.8 d²) / R²) + 0.5), × 5 for a
soldier under a quarter of its health within 20 m. On foot the current
value is 1.2 × the soldier's own; in a vehicle 2 × the seat's (250 at the
driver's seat with others aboard), and the way out is the soldier's own
value, or 10000 for a wreck or a vehicle more than 4 m off the vehicle
mesh (0x100880e0). The other seats' factor (0x1006b910): 1 with the
driver's seat taken; else, for a Mobile root that isn't ITNaval, 0.77 for
ITGround and 0.5 for ITAir. The AI flags are the template's
`aiTemplate.addType` bits (BF2142.exe 0x0088fbd0, 0x00882ff1): ITUnit 0,
ITAir 1, ITHelicopter 2, ITGround 3, ITNaval 4, ITControlpoint 5,
ITTransportation 6, ITMobile 7, ITStructure 8, ITSoldier 9 (also set
through 0x0088d070), ITObstructedView 10, ITNoTemperature 11, ITHasWreck
12, ITObstacle 13, ITHasCarHorn 14; ITBiological sets none. Not
rewritten: changing seats inside a vehicle and the type-0/1 order
multipliers.

### Not decoded, and the rewrite's stand-ins

- **Scheduling:** the CPU budgets are real time (see Scheduling: they
  depend on the machine's speed, so they can't be reproduced; every slice
  runs to its count, the SAI once a second), and the whole path is
  searched at once (the original searches three clusters at a time). Each
  think evaluates every behaviour (the original's think stops at its
  budget, the same real-time clock, and the rest wait for the next).
- **Aim:** bots don't zoom. (The head twists before the body for every
  soldier: see `SoldierControl::twist`, 0x0058a6d0.)
- **Deviation:** the AI deviation (0x00892b70) widens the weapon's own
  deviation through the soldier (vt+0xf4); the rewrite's hand-held
  firearms have no deviation model, so the bot spreads its aim uniformly
  over ± half of it on each axis.
- **Fire plan:** the explosives' start conditions (0x100d8800,
  0x100d8400) aren't decoded.
- **Soldier radius:** the game object's size through the AI adaptor
  (vt+0xdc → 0x008a3d40 → the object's vt+0x90), not identified: 0.5 m.
- **Runs without the AI scripts** (tests): a driver is a Tank when its
  seat has a gun and a Car otherwise, other seats are Fixed, vehicles
  count as LightArmour, and vehicle guns reach 100 m. Every shipped
  vehicle and weapon has its `ai/*.ai`. Levels without
  `StrategicAreas.ai` (three of the 46 with a mesh) leave the original's
  SAI without areas; the rewrite deviates and makes one per control point.
- **Strategy:** area temperatures (soldier 1, vehicle 20) and strengths
  (8 per unit; a vehicle's own battle strengths are not read).
- **Revive:** the time left uses the respawn countdown, or 15 s. The
  squad factor the original multiplies in is not.
- **Flee:** the rewrite's camouflage has no charge; carried counts as
  charged, and only the plan's 4 s end the camouflage.
- **Idle:** the weapon's alternate-key tap in the bot's own area (the
  rewrite's weapons have no such state).
- **Commander:** which tool it strikes with. The UAV, the satellite scan
  and orders for squads the SAI left without one are not from AIDLL's
  commander.
- **Path follower:** a moving target re-planned when it moves 2 m; the
  segment searches' fallback to the whole mesh (see above). The jump (a
  PIJump press) is not in the rewrite's soldiers: a second's back step to
  one side stands in.
- **Stuck drivers:** after the 20 s progress check the move fails and
  the behaviour plans again. 0x100c91c0 also hands the bot to the game
  (IAIGameEffector vt+0x24 → 0x0089e940 → GameLogic slot 0x58), whose
  slot is `0x008924f0`, returning 0: it does nothing in this build.
- **Assists:** the radio requests ("medic!", "ammo!") and the squads'
  order types the factors read are not modelled; basicTemp is read as the
  AI object's +0x14.
- **Avoid:** r is the vehicle's AI radius; the look-ahead time (2 s; ×2
  prone, ×1.5 crouched) and the size factor (1) come from the game's side
  of the AI object, not traced. The grenade warning (1000 for 2.8 s after
  an explosion in front, 0x1004eab0) is not rewritten.
- **Keeping distance:** while the Fire pattern walks the bot towards its
  target, the aim keeps the view's pitch (walking otherwise levels it).

`bf2142 --bots N [--simulate S]` adds bots and, with `--simulate`, runs S
seconds headless and prints the scores. With `BF2142_BOT_STATS` set, it
also prints how long the bots ran each behaviour and how far they drove.

### Navigation mesh

Each level with bots ships `AIPathFinding/{Infantry,Vehicle}.{qtr,clb}`
(QuadTreeManager, `0x100ae4e0`). The `.qtr` holds a quad tree over walkable
polygons with their edges and neighbours. The `.clb` groups the polygons
into clusters joined by links, with precomputed link distances. Rewritten
in `src/AI/NavMesh`:

- Ported: the parser, point lookup, the polygon and cluster A* with the
  original's costs and heuristics.
- How the bots combine the two searches is in "Following paths" above
  (rolling segments of three clusters); `findPath` (tools, tests) keeps
  a cluster corridor for one polygon search.

On minsk_coop's infantry mesh (76,605 polygons, 2,154 clusters), a 639 m
query gives a continuous 853 m path.

Levels shipped without `AIPathFinding` (Sidi_Power_Plant, for one) have
no bots (see above).

## Menus (SwiffPlayer.dll: Flash)

The front end and the end-of-round screen are Flash movies played by
SwiffPlayer.dll, a DICE build of the open-source gameswf player
(`Code\BF2\External\gameswf\SwiffPlayer`). They live in `Menu_server.zip`
under `External/FlashMenu/`: `mainMenu.dfm` (2.4 MB) and `endOfRound.dfm`.
Their images are separate PNGs in `Menu_client.zip`.

A `.dfm` is a plain SWF XORed byte by byte with the key `0b ad f0 0d`,
indexed by the file position modulo 4. The key is at `0x101019f4`. The
movie loader (`0x100080a0`) picks this reader (`0x10007e80`) by the `.dfm`
extension and passes the result to gameswf's FWS/CWS parser. Both menus
decode to uncompressed SWF 7 at 1024×768 and 30 fps:

| | mainMenu | endOfRound |
|---|---|---|
| Frames | 156 | 135 |
| DefineSprite | 556 | 361 |
| DoAction (top level + in sprites) | 8 + 1063 | 7 + 470 |
| DoInitAction | 110 | 83 |
| Bitmaps (JPEG / JPEG3 / lossless) | 7 / 45 / 57 | 6 / 40 / 52 |
| Shapes | 219 | 169 |
| Edit texts | 167 | 152 |
| Fonts | 19 | 14 |

The ActionScript is AS2 on AVM1, with no AVM2. Its classes are compiled
into `DoInitAction` blocks as `__Packages.*` (`dice.UIs.*`,
`dice.holders.*` and others).

The menus reach the game through natives that `0x10009050` installs under
`_global.dice`: `General`, `MessageHandler`, and under `dice.bf2` `Locale`,
`Logic`, `Singleplay`, `Multiplay`, `Profile`, `Client`, `Sound`, `Cursor`,
`Mod`, `EndOfRound`, `ControlSettings`, `VideoSettings`, `AudioSettings`,
`ServerSettings`, `ServerInfo`, `GeneralSettings`, `PlayerStats`,
`Shader`, `BuddySystem` and `Mouse`. Each is a set of typed slots (bool,
string, int, float, object, function, getter). `List` makes 30 named lists
(serverList, buddyList, eorPlayerList, ...), each with fillList, sortList,
scroll, select and the row fields.

Rewritten so far:

- `src/UI/Dfm` and `src/UI/Swf`: the decoding, the header and the tags.
- `src/UI/Avm1`: an AVM1 interpreter covering every SWF 7 action, with
  DefineFunction2 registers, closures, `Extends`/`super`, try/catch/finally,
  `With`, getters and setters, and the built-in classes. The main menu's
  110 `DoInitAction` blocks and 1,071 `DoAction` blocks run without errors.
- `src/UI/Characters`: shapes and morph shapes, tessellated with the fill
  rule from SWF's edge ownership; bitmaps (lossless, JPEG with tables,
  JPEG3 alpha); `DefineFont2` glyphs; edit texts; buttons; sprites. All
  characters of both menus decode.
- `src/UI/Player`: drawing into the renderer's menu layer (`MenuDraw.h`:
  triangles with straight-alpha images and Flash colour transforms), and
  the timeline and display list.

- `src/UI/Player`: the player (`Player`). Clips, text fields and buttons
  are script objects with the MovieClip and TextField methods the menus use
  (`attachMovie` with `registerClass`ed classes, `createEmptyMovieClip`,
  drawing, `loadMovie` for the PNG images, depths, bounds, hit tests,
  events, text bound to variables). It handles mouse and keyboard input,
  and draws into the renderer's menu layer (`MenuDraw.h`: triangles with
  straight-alpha images and Flash colour transforms).

One tick runs in this order:

1. Timelines advance.
2. New instances are initialised (construct event, the registered class's
   constructor, initialize event).
3. Frame actions run, then load and unload events, then actions queued by a
   goto.
4. `onEnterFrame` handlers run.
5. Intervals fire.
6. Bound text fields update.

All `DoInitAction` blocks run once at load. Mouse events go to the deepest
interactive object and every interactive parent.

The natives live in `Natives.h`. Every object and member exists with its
decompiled name and slot type; the game binds real behaviour with
`setFunction`, `setValue` and `setListRows`, and unbound calls are logged.
There's no sound in the player itself (the menus call `dice.bf2.Sound`).

### Text fields

SwiffPlayer.dll never draws an edit text with its SWF glyphs: its display
(0x100c01a0) only draws the background, the border and one RendDX9.dll
text object per line, in the engine's bitmap fonts. Fonts get names when
the movie is read (0x100b9000): an exported font's id maps to its export
name, and every font that isn't exported takes the export name of the
exported font with the same font name. The menus export fonts 1-4 as
`StandardTextBold`, `StandardText`, `DynamicText` and `Titles`, so the
device fonts "Helvetica" and "Helvetica Condensed" draw as `DynamicText`
and `StandardText`. A field whose font has an export name gets text objects
(0x100c1700); the rest (mainMenu's "TradeGothic LT BdCondTwenty" fields)
draw no text at all. A text object's font is FontSystem's
`<name>_<round(height / 20)>` (0x101172f0), read from
`Fonts/<language>/<res>/` and then `Fonts/<res>/` (0x10185e90), where
`<res>` is `800` for an 800-pixel-wide window and empty from 1024 pixels
up (scaled by width / 1024). The menu stage is 1024 wide, so the player
takes the 1024 fonts at their own size. English lacks `Titles_*`, so
`Titles` text doesn't show in English.

The layout (0x100bfd00):

- `\r`, `\n` (not straight after `\r`) and `|L` end a line. `|` is dropped.
- With word wrap, a line ends once its text reaches the field's width (times
  the world x scale) less the right margin and 2 pixels. It ends at the last
  space or bullet, which is dropped, or else before the character that
  didn't fit. Setting `multiline` turns word wrap on.
- Unless the field sizes itself, lines starting below its height (times the
  world y scale) are dropped.
- Lines are leading + font height twips apart.
- Line `i` goes at the field's origin (its world translation, not its
  bounds) plus max(0, indent + left margin) + 40 twips across, and
  30 + i x step twips down. The scale and rotation are dropped.
  `drawText` (0x101190b0) rounds that to pixels as round(v + 0.5).
- The text object adds its alignment (0x10183570): for right, the field's
  width less the text (not below 0); for centre, half of that. The
  margins don't narrow the field. Then it floors the position
  (0x10182430).
- Each character goes where RendDX9.dll lays it (0x10183240). The pen moves
  by the pair's kerning, and the glyph goes at floor(pen + before + 0.5).
  The pen then moves by width + after. A character the font lacks takes
  the previous one's place.

`textHeight` is the value 0x100bd740 gets: round((font leading + leading +
height) / 20), or with autoSize round((font leading + height + the last
line's offset) / 20). The font leading is the SWF font's, in unscaled EM
units. It is 666 for a field without text objects. A field shorter than
that grows downwards. autoSize is only a flag (a non-empty string such as
`"none"` sets it). `Locale.getTextboxWidth` (0x1003a9d0) is the end of
the first line. SwiffPlayer has no `textWidth` member, so we answer with
the widest line.

The original keeps a field's text objects and lays them out only when the
text changes. The rewrite keeps each layout by what it depends on (the
text, the font, its size, the bounds, right margin, leading, the html,
password, word-wrap and autoSize flags and the world scale), and the fonts
look up codes under 256 in flat tables (`HudFont::buildIndex`). With the
shipped menu after an offline login (`MenuBindings_FrameText`,
`BF2142_MENU_BENCH=1`), `Player::frame()` went from 3.78 to 0.65 ms in the
Debug build and from 0.065 to 0.015 ms in a Release one, the frames
identical.

### Masks

Masks use the stencil buffer, as gameswf's display list drives it
(0x100dfdd0). A mask layer (clip depth) is drawn between
`begin_submit_mask` and `end_submit_mask`. The objects above it show
through it until the first object above its clip depth, which first calls
`disable_mask`. A clip with `setMask` draws its mask the same way, then
itself, then calls `disable_mask`. `setMask` arguments aren't drawn.
RendDX9.dll (0x10118920, 0x10118a10, 0x10118aa0) works like this:

- `begin_submit_mask` clears the stencil at the first level and raises the
  level. It turns colour writes off and tests GREATER against the level,
  incrementing on pass and replacing on fail.
- `end_submit_mask` turns colour writes back on and draws where the
  stencil EQUALs the level.
- `disable_mask` turns the test off and lowers the level.

So a nested mask shows the intersection, and after it ends the outer mask
no longer applies. Everything a mask draws counts, strokes and transparent
parts included. `Stencil` keeps the stencil as disjoint convex polygons
with their values and cuts the triangles drawn under the test to those
whose value is the level. The result is the stencil's, with no stencil
buffer in the renderer.

### Name matching and method targets

Two behaviours of DICE's gameswf that the menus depend on:

- Names are matched without regard to case. The movie writes
  `_root.UpdateManager1` 206 times for `updateManager1`.
- A function called as a clip's method takes that clip as its target:
  its variables and `_parent` come before the defining timeline's (the
  list rows' `onRelease = _parent.rowRelease`).

Script calls are limited to 256 deep and 512 KB of machine stack.

The game's side is `src/Main/MenuBindings`. Jobs run on worker threads and
the natives poll them: calls with `true` start a job, calls with `false`
ask whether it's done.

- **Version check:** there's no patch server, so the check reports no
  patch.
- **Online check and login:** the menu calls
  `Profile.connectToMasterAccount(name, password, remember)`, then polls
  `isTransactionComplete` and reads `getLastError`. The error codes come
  from the ProfileManager table at BF2142.exe `0x004a0980`; FESL's codes
  are mapped at `0x004a1730`, for example 122 to `LOCERROR_passwordwrong`.
  The account is checked against OpenSpy's FESL.
- **Soldiers:** the `soldiers` list, then `connectToSubAccount`.
- **Offline play:** `Logic.setOfflineMode` with soldiers kept in
  `bf2142-recomp/profiles.txt` under the user's config directory.
- **MessageHandler** popups: the game's error texts.
- **Server lists:** `fillList` and `refreshList` query OpenSpy's master
  server. The columns are those of `0x10079770`.
- **Joining:** `Multiplay.joinServer` hands the address to the game and
  probes the server's handshake. A refusal comes back as a popup.
- **Starting a game:** the singleplayer page calls
  `Multiplay.setFilterGameMode("gpm_sp")`. The list `mapListAvailable`
  shows that mode's levels as "Name - size" (SwiffPlayer 0x100723c0, with
  underscores as spaces). Its `selectRow` gives `rowIndex` and `rowData`
  ("Level size"). `addMapIndexToMapList` and `removeMapfromMapList` fill
  the map list (`mapListChosen`). The page sets `ServerSettings.*` and
  `GeneralSettings.*`: each `setX` is stored by name and `getX` reads it
  back, with the game's defaults before. `createServer` starts the first
  map. Instant action clears the list, adds one map at random and starts
  it with 16 bots (players included), ticket ratio 200 and 15 s spawns.
  `bf2142` lists the levels from each `Levels/<level>/server.zip`
  (`GameModes/<mode>/<size>/GamePlayObjects.con`). Without `+level` the
  menu runs alone; `createServer` closes it and loads the level as
  `+level … +gameMode … +mapSize …` would, with the bots, their skill and
  the ticket ratio from the menu.
- **The playlist and the game under the menu:** each map runs in
  `runGame` (`src/Main/main.cpp`), whose managers all start empty, so the
  next map loads from nothing. The menu (its movie and the login) lives on
  in the session across maps. `sv.endDelay` (15 s) after a round ends the
  playlist's next map loads, after `roundsPerMap` rounds on one map. While
  a game started from the menu runs, `Logic.isGameRunning` is true, Esc
  shows or hides the menu over it (the soldier stops taking input while it
  is up), and `Logic.disconnectGame` ends the game and goes back to the
  menu alone.
- **The end of a round:** once the round is over, `endOfRound.dfm` (a
  player of its own, `EndOfRoundBindings`) shows over the game with the
  outcome, the scoreboard from the scripts' scores and the player's
  totals, until the next map; JOIN GAME skips the rest of `sv.endDelay`.
- **Sound:** `Sound.playSound(name)` and `stopSound` play the menu sounds
  that `Menu/MenuSounds.con` adds with `sound.addMenuSound` (`rowPress`,
  `buttonPress`...), through the game's sound manager.
- **Options:** the game, controls, video and audio pages read
  `VideoSettings`, `AudioSettings`, `ControlSettings`, `GeneralSettings`
  (and the server pages `ServerSettings`) with `getX` and write with
  `setX`; `apply()` keeps the values and writes them to
  `<config dir>/bf2142-recomp/settings.txt`, `cancel()` goes back to the
  last applied ones, `reset()` to the defaults. Video qualities are 1 low,
  2 medium, 3 high (0 off for dynamic shadows and light);
  `getAvailableAntialiasing` (0x1005cc20) gives `{value, text}` objects
  ("Off", "2Samples"... with `VIDEOSETTINGS_` texts); the select boxes
  show the list their first argument names (`displayModes`: the display's
  modes as "1280x720@60Hz", 0x10065360). Applied, the resolution sizes the
  window; the volumes set the master volume to 1 and the sound groups
  (0x00496830): effects groups 0, 3 (menu sounds) and 4 (first-person
  weapon sounds), music group 1 (the music and the interface menu
  sounds), voice-over (`helpVoiceVolume`) group 2 (the voice messages);
  and the mouse sensitivity (0.5 the
  default) and inversion steer the soldier's look. The menus' SWF version
  matches names without regard to case, and so do the natives (the video
  page calls `getTexturequality`).
- **Controls:** `Settings/Controls.con` builds the control maps with the
  `ControlMap` console object (`src/Input/ControlMap`):
  `InfantryPlayerInputControlMap`, `Land…`, `Air…`, `Helicopter…`,
  `Sea…`, `defaultPlayerInputControlMap` (player inputs common to all) and
  `defaultGameControlMap` (menu, map, scoreboard, chat; `c_GILeader`, Caps
  Lock, opens the commander's screen). Its trigger mappings end with a
  repeat delay (10000 once per press, 0 while held) and the binding's slot
  (0 primary, 1 secondary) where BF2's profiles wrote `c_CMPushAndHold`.
  The input names are engine constants: 0x00669baf registers the player
  inputs `c_PI…` and the game inputs `c_GI…`, each counted from 0
  (`c_PIYaw` 0, `c_PIPitch` 1, `c_PIRoll` 2, `c_PIThrottle` 3…). The
  viewer reads the soldier's keys from the infantry map on foot and, in a
  vehicle, the map of its `vehicleCategory` (0x004b141a: VCLand, VCSea,
  VCAir, VCHelicopter). The controls page asks
  `ControlSettings.getMappedString(input number, secondary, negative)` for
  a key's name and binds the key pressed last with `mapInput(input number,
  secondary, negative, superceed)`; `setActiveControlMap(n)` picks the
  map from SwiffPlayer's table (0x100f7b50: infantry, land, air,
  helicopter, sea, the game's, the common player map). Applying the page writes the
  maps in Controls.con's format to `<config dir>/bf2142-recomp/controls.con`,
  which later runs read instead of the game's.
- **Scripted input:** `--input-script` feeds timed SDL events (keys, clicks,
  relative mouse, text) to the window and saves screenshots, so a whole
  session (login, a game from the menu, playing, the menu over the game)
  runs unattended in the real window. The menu layer keeps its own frame
  clock in the renderer (it plays with or without a HUD).
- **One window:** the viewer's `Window` (`src/Main/Viewer.h`, an SDL window
  and GPU device) opens with the first view and stays open from the menu
  through every map. While a level loads in it, `endOfRound.dfm` shows as
  the loading screen (`Window::MenuScreen`): the map's name, picture and
  briefing from `Levels/<level>/Info/<level>.desc`, and the progress after
  each loading step (templates, `Init.con`, static objects, gameplay
  objects). The renderer's build of the level comes after it.

The loading and end-of-round screen (`endOfRound.dfm`) gets its natives
from `src/Main/EndOfRoundBindings`, which answers them from a
`RoundSummary` the game fills. `dice.bf2.EndOfRound` is made at
`0x1001fd50`; its functions read the game's end-of-round record (SwiffHost
slot `0x180`) and the level being loaded. The movie polls them from its
root every frame:

- **Loading or waiting:** `Logic.getIsLoading` (`0x1003c9e0`) is true
  while a level loads and false while the finished round's pages wait
  ("END OF ROUND. LOADING NEW MAP..."). A first load
  (`Logic.isFirstTimeLoading`, `0x1003c970`), or one with no finished
  round (`EndOfRound.isValidEOR`, `0x1001bdd0`), shows the briefing: the
  map's name, its picture (`$/Levels/<level>/Info/<mode>_<size>_menuMap.png`,
  where `$/` is the game's root), the `.desc`'s description
  (`LOADINGSCREEN_MAPDESCRIPTION_<level>`), the game mode's, a random
  `TEXT_gametip<n>` (`0x1003b290`) and the server. Otherwise the finished
  round's pages take turns every 8 s while the next level loads.
- **Progress:** `Client.getLoadProgress` (`0x100147b0`) fills the bar. From
  0.95 on the movie says it is verifying and waits. It shows JOIN GAME
  when it isn't loading, the progress is under 0.95 and
  `Logic.getNumRoundsLeft` is above 0. The bindings answer that way once
  the game says the level is up. JOIN GAME and SPECTATOR call
  `Logic.readyToPlay(asSpectator)` (`0x1003dba0`).
- **The finished round:** teams are 0 and 1 in the movie (the game's 1 and
  2). `getWinningTeam` (`0x1001d910`) is -1 for a draw. The outcome
  (win, loss or draw for the player's team) picks
  `images/eor/<level>/outcome_[gpm_ti_]<outcome>_<team>.png`
  (`0x1001d9c0`) and the header image (`0x1001db80`). The player's
  score, kills, score per minute (`"%0.2f"`), bonuses, round and total
  points, and rank (`RANK_<n>`) come from the summary. The rank's progress
  (`0x1001c4a0`) runs between the points of this rank and the next.
  The points are `Settings/RankSettings.con`'s; the summary's round
  points, total, rank and `gotRankUp` come from the soldier's record
  (below). The commander is shown as "N. name" with
  their score, a medal (`0x1001f020`) when they are among the round's
  three best, and `HEADER_commanderwin/loss/draw`. Squads are named
  "N. name" (`0x1001e010`).
- **Lists:** `eorPlayerList "0"|"1"` (the player's team, the other;
  `0x10066c40`) has rows "N. name", score, team score, kills, deaths and
  score per minute, without the commander. The player's row is
  highlighted, and the last row shows the player when they are further
  down. The round's three best get the page's gold, silver and bronze
  icons. `eorEarnedAwards "<page>"` (`0x10066190`, `0x10066540`) shows
  four awards a page: the picture from
  `images/awards/<kind>/eor/` by the id's hundreds, `AWARD_NAME_<id>[_<level>]`,
  and the count. `eorSquadScore "<0|1> <place>"` (`0x100678b0`) shows the
  squad leader, the members and the totals. `eorDebriefing` (best kit,
  weapon and vehicle) stays empty because there are no per-weapon
  records yet.

Driven headlessly with a summary, the movie shows the loading screen for
Minsk (map, description, tip, server, progress, then JOIN GAME). After a
round it shows the outcome with the awards, the scoreboard, the squads and
the briefing, without script errors.

A soldier's rank, score and unlocks come from the local stats store
(below); awards and veteran status are not kept. Driven headlessly, the
main menu goes from its login (or offline mode) to the soldier list, the
BFHQ page with the rank, the unlock page (choosing one), the stats page's
details, and the multiplayer browser, and a join reaches the game.

### Soldier stats, ranks and unlocks

The game kept a soldier's standing on the stats server (BF2142.exe
`0x004590a0` getawardsinfo.aspx, `0x0045bb10`/`0x0045bd10`
getunlocksinfo.aspx, `0x0045c580` getplayerinfo.aspx, `0x0045e110`
getrankinfo.aspx, `0x0045e4b0` selectunlock.aspx, all with GameSpy auth
tokens, at stella.prod.gamespy.com, which OpenSpy's patch points at
stella.prod.openspy.net). There is no account here to ask with, so
`src/Stats/Stats.h` keeps each soldier's record on this machine
(`<config dir>/bf2142-recomp/stats.txt`, next to `profiles.txt`) and
answers the menu itself; nothing is sent.

- **Ranks:** `0x0043ac80` runs `<mod>/settings/ranksettings.con` at
  start-up: `rankSettings.setRank <rank> <points>` fills a map rank ->
  points (`rankSettings.save`, `0x0043afa0`, writes it back). Ranks 0..40
  have points (40 at rank 1, 57,700 at rank 40); 41..43 are listed with 0
  (the leaderboard's top ranks) and are not reached by points. The rank is
  the highest one the experience (the global score) reaches.
- **The record:** experience, kills, deaths, rounds, wins and losses,
  time played, the best round's score and the unlocks chosen, in order.
  The end-of-round code adds the round (`MenuBindings::recordRound`: the
  local player's score as experience, never below 0; the outcome from the
  winning team) and fills the end-of-round movie's round points, total,
  rank and `gotRankUp`; the main menu's `PlayerStats.getRankNotification`
  then answers 1 once (its "promoted" popup). Nothing is recorded without
  a soldier logged in from the menu.
- **Unlock ids** (the menu's `mcUnlock_<id>` clips, `NAME_UNLOCK_<id>`,
  `images/unlocks/<id>.png`): `<kit><branch><level>`. Kits 1 Recon,
  2 Assault, 3 Engineer, 4 Support are the Kit templates' `unlockIndex`
  + 1; branches 1 and 2 their ItemContainers' `unlockBranch` UB_First and
  UB_Second; the level is `unlockLevel` + 1 (1..5). Kit 5 is the player
  (UB_Player, 511..516) and squad leader (UB_SquadLeader, 521..524)
  upgrades, which every kit has its own container for. The containers
  with `unlockLevel 15` (the dog tag knife, the retailer and HAR/LAR
  replacements) are not chosen. 50 ids, 40 without Northern Strike's ten:
  each kit branch's level 5, 516 and 524 (SwiffPlayer `0x1004e000`,
  `0x1004b250`).
- **The rules:** `0x0045c030` turns the server's list of owned ids into
  the menu's table: in each branch the levels up to the highest one owned
  are taken (1), the next is available (2) and the rest locked (0), so a
  branch is bought in order (5 levels, 6 and 4 for kit 5). `0x0045be30`
  turns it into the game's: a mask per kit and branch, the 5xx ones the
  same for every kit. `getUnlockInfo` (`0x1004e000`) shows an available
  one as locked without credits, and Northern Strike's as locked without
  the booster (`hasBoosterPack`, `0x1004a8e0`: offline, or the account's
  flag; `hasBoosterPatch` is always true). The credits were the server's
  (`Avcred`, the unlock info's `+0x18`, `getUnlocksCountUnselected`
  `0x10049a40`); here a soldier has one per rank, less the unlocks chosen.
  That is the published game's rule: the client only reads the count, and
  the code that worked it out was GameSpy's stats back end (the `.aspx`
  pages), which was never shipped. `selectUnlock(id, newRequest)` (`0x1004b250`) answers 0
  (done), 1 (not available), 302 (invalid or Northern Strike without the
  booster), 303 (selected already) or 304 (no credits), at once here (the
  menu would poll on -1). `getUnlocksCountTotal` (`0x1004b190`) is 40 or
  50, `getUnlocksProgress` (`0x1004de60`) the taken ones over that.
- **In the game:** the chosen ids' ItemContainers of every kit of both
  armies (`stats::unlockContainers`) are the local player's
  (`GameWorld::localPlayerUnlocks`, given when it joins). A choice made
  during a round counts from the next one.
- **The stats page:** the details tab's category list (`statsTypes`,
  `0x10080cb0`: overview, player, titan mode, teamwork, leadership,
  weapons, vehicles, maps) and the `stats` list (`0x1007f640`, labels from
  `0x1007dfd0`) with "<category> 21". The overview (career points, score,
  time played, wins/losses, best round) and player (score and kills per
  minute, K/D, kills, deaths, per round) rows the store has are listed;
  the other categories stay empty. The progress tab's chart
  (`Logic.setActiveDiagram`) is drawn by the game and is not in.
- **The server's answers** (parsed, not used): `0x005752b0` splits the
  text into tokens at runs of tabs and newlines (no empty fields); it
  starts with `O` (anything else failed), then `H` header and `D` data
  lines, and `$ <length> $` ends it. An answer without the `$` failed; the
  length is never checked. For the unlocks (`0x0045c030`, mode 1) the
  reading is by position: the 6th token after `O` is the pid, the 12th the
  credits (`Avcred`), and from the next `H` on each `D` row's first value
  is an id (none for pid 0). `stats::parseStatsResponse` and
  `parseUnlocksInfo` do the same.

`bf2142 --menu [mainMenu|endOfRound]` runs a menu. Add `--view` for a
window that takes the mouse and keyboard, or `--screenshot out.png
[--menu-seconds 10]` to capture it. With no level loaded, the main menu
shows its EA account login over the scratched-metal background, as in the
game.
