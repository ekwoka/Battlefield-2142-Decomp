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

### Movies (`bf2Engine.playMovie`, Bink)

`Init.con` queues the start-up movies: `Movies\health.bik` and
`Movies\warning.bik` (not shipped), `EA.bik`, `dice.bik` and `legal.bik`
with mode 1, `intro.bik` with mode 0 ("These should be noops in server &
editor"). `bf2Engine.playMovie` (`0x004082a0`) does nothing on a dedicated
server (`0x0042b800`), swaps `legal.bik` for `legal_na.bik` when the user's
locale is US or Canadian English (`0x00403230`: 0x409 or 0x1009), and
appends to the engine's list (+0x100). The frame (`0x0040bd90`) takes the
next when none plays (`0x00408680`: an `FSMoviePlayer` from RendDX9,
binkw32.dll underneath; one that won't open is dropped); while one plays
the cursor is hidden and a frame drawn, and it ends at its end, or on Esc
(key 1) when its mode isn't 1: the logos can't be skipped, the intro can.
An idle menu queues `Movies\screensaver.bik` (not shipped), which any key
ends. `GeneralSettings.setViewIntroMovie` (+0x80, default 1) has no
reader: its getter (vtable 0x00927450 slot 33, `0x0049c560`) is never
called in BF2142.exe, and `mainMenu.swf` only declares the
`getViewIntroMovie` native, so the intro plays whatever it says. The
rewrite stores, saves and exposes it and likewise doesn't gate the intro.

The rewrite (`src/Movie`, optional with `BF2142_MOVIES`) decodes the
`.bik` files with FFmpeg's Bink demuxer and `binkvideo` /
`binkaudio_dct` decoders (all six play: 640x480 or 800x600 at 30 fps,
stereo 44.1 or 48 kHz; `Legal*.bik` are silent), converts frames to RGBA
(swscale) and sound to interleaved floats (swresample). The window's
movie screen (`Window::MovieScreen`) blits each frame scaled to fit over
black and plays the sound through SDL; the start-up queue plays before
the menu (`--no-movies` skips it), `--movie <name|file>` plays one, and
with `--screenshot out.png --movie-seconds t` renders the frame at `t`
off screen. Without FFmpeg the movies don't open and are skipped, as the
original skips a movie that won't open.

Movies in menu clips: the Titan tips panel's LAUNCH button stops the menu
music and calls `mcTutorial.loadImg("?titan_tutorial.bik")`, the image
component loading the URL into its clip; a leading `?` marks a movie
(SwiffPlayer's `Multiplay.getMapMovie`, `0x100449b0`, builds
`?/Levels/<level>/Info/..._menuMovie.bik` and answers `?/noise.bik`), which
SwiffPlayer draws as a video texture (RendDX9's Video/VideoTarget). The
rewrite's player (`ui::Player::setVideoOpener`, `ui::VideoSource`) opens
such a URL through the host instead of decoding an image: the first
picture at once (so the clip has its size, as an image's), then the
pictures due as `advance` passes time, the last one staying; `unloadMovie`
drops it. The host (`movie::MenuVideo`) resolves `?name.bik` to
`Movies/name.bik` and `?/path` to the mod's root (where the files are; the
DLL's resolver is in RendDX9's undecompiled video code) and decodes with
the Bink decoder. Its sound plays too, as RendDX9's Bink plays it itself
(0x1011df20 picks `BinkOpenDirectSound`; 0x1011e0d0 opens the file with
`BinkOpen(path, 0)`, no `BINKNOSOUND`, and sets no volume): the decoded
samples go straight to the playback device (`AudioOut`, outside the
game's sound groups) as the pictures go by, and what is queued stops when
the clip lets the movie go (the menu music was stopped by the button,
`Sound.stopMenuMusic`). `--no-movies` leaves the clip empty. The Titan tips panel is
on the online multiplayer page; off screen, `--menu-script` has
`call:<clip>.<method>,<args>` to load it into a clip offline.

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

**Collision mesh instances.** A `.collisionmesh` holds geometries, each
with sub-geometries, each with LODs by collision type. The template's
loader (`0x006d8740`, one `0x006e4880` per geometry) keeps an object per
geometry (`0x6c` bytes, `0x006e45c0`) whose `+8` array (`0x28` bytes an
entry) is its sub-geometries; a trailing run of geometries without a LOD
is dropped (unless it starts at geometry 0), and when no geometry's
sub-geometry 0 has a LOD (`0x006de510`) every geometry loses its first
(`0x006e5820`). The shipped vehicles' files lead every geometry with an
empty one (`as_fav`: 46 geometries of three sub-geometries, the first
empty, the second the normal shape, the third the wreck's for geometry
0), so after loading sub-geometry 0 is the normal shape and 1 the
wreck's, and 15 geometries stay. An object's mesh instance (`0x006e5d70`,
vtable `0x00995ad8`, the collision manager's slot `0x28` = `0x006d8180`
by mesh name and geometry index) keeps the geometry at `+0xc` and the
state, the sub-geometry, at `+0x14` (0; slot `0x30`, `0x006e5df0`,
switches to 1 when the geometry has `+0x60` set and more than one
sub-geometry, applied by slot `0x34`, `0x006e5e90`; slot 5 back to 0).
A part's `collisionPart` (template `+0x74`) is the geometry: `0x0063ccc0`
gives each child with one over 0 an instance of the root's mesh at it.
Every per-LOD query (slots 10–28: faces, vertices, materials, radius of
a LOD, segment and sphere queries) goes through the instance's one
sub-geometry; only the radius (slot `0x74`, `0x006dedb0`) and the box
(slot `0x78`, `0x006deeb0`) look across all of them. Rewritten in
`physics::parseCollisionMesh` (the trim) and `physics::collisionPart`:
the rewrite collides with the normal sub-geometry alone (it used to add
the wreck's faces too) and never switches to the wreck's: no caller of the
instance's slot `0x30` with 1 was found (the `+ 0x30))(1)` calls in
`0x0050`–`0x006f` are other classes' slots), so what turns a wreck's
mesh over is unknown.

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

The node types (`0x005f6ba0`, by `physicsType`: 0 None, 1 Point, 2
RotationalPoint, 3 Mesh, 4 MechLegs; the string switch `0x006bf540`): with
`hasMobilePhysics` a Point gets `0x006c5e70` (vtable `0x00993418`), a
RotationalPoint `0x006c6480` (`0x00993610`), a Mesh the Basic node above;
MechLegs gets none, and a static Mesh a node without physics (`0x006c5500`).
Shipped Points with mobile physics are the GenericProjectiles (rounds,
shells, missiles); RotationalPoints the grenades, mines and debris
particles. Their tick (`0x006c6170` / `0x006c57a0`, the RotationalPoint's
`0x006c6b20` with `0x006c6860`) has no damping and no sleep test: the
impulse, then the acceleration, the mean of the old and new velocity moves
the position, gravity for the next tick; a RotationalPoint turns about its
own origin by its torque × dt × 150 / mass (no shape inertia, no damping).
With `drag` (shotgun pellets 18, C4 15, missiles 0.1–10) they lose
velocity × drag / mass × 0.01π a tick (a 0.1 m sphere, `0x006c5730`), 25
times as much in water. Rewritten in `physics::PointBody`; the rewrite's
projectiles move by it.

**Sleeping** (`0x006c9060`, node slots `0x14c`–`0x160`). The counter
(`+0x68`) is 30 (`*0x00a84adc`) after a wake (slot `0x160`; the
constructors wake the node on the server, clients start asleep) and a still
root counts it down each tick: the mean of its last three position changes
over the tick under 0.5 m/s (`*0x00a84b68`) and of its last five angular
velocities under 0.5 rad/s (`*0x00a84b6c`); more wakes it. Asleep (below 1)
the node is not stepped, feels no gravity or drag, its motion is zeroed
each tick (slot `0x148` → `0x006c6080`), and the collision dispatch
(`0x006dd0b0`) skips it: no ground pass, no broad phase as a mover, no
solve (it stays a candidate for movers, the mass shares deciding). What
wakes it: the mass shares (the other's momentum), the engine (`0x0064e920`:
a throttle or rpm ≥ 0.05), a blast's push, a wheel's contact velocity
(`0x006b7910`), the heavier mobile object it rests on (the response's
`+0xf4`) going or waking; a sleeping PlayerControlObject root woken wakes
what lies round it (`0x006bfad0`, not major actors). Rewritten in
`physics::SleepState` and `GameWorld::stepSleep`.

**Drag and water.** A Basic node with `drag` (vehicles 1–1.2) loses, per
body axis, drag / mass × |v| × the area across it (the vehicle LOD box's
faces × 0.785 × `dragModifier`) of the velocity, at most the velocity's own
component in a tick, as a force at its position (`0x006c7e00`); the
angular velocity likewise against two areas' sums (`0x006c8110`); 25
times as strong by the part of the box's height under water. Buoyancy is
in the ground pass (`0x006b9260`): the LOD's lowest point under the water
level (the heightmap cluster's slot `0x98`: the sea level) sets the node's
depth (`+0x5c`, at most 1 m) and a root with `floaterMod` (default 1; tanks
0.5, the FAV and hovers 0.8) is lifted by −gravity × `floaterMod` × the
largest weight of the box's corners under the water (each Σ |axis.y| ×
min(depth / the box's size along it, 1)), at their weighted mean. So a
fully drowned box with `floaterMod` 1 hangs in the water. Rewritten in
`physics::applyDrag`, `applyBuoyancy`.

**Collision groups.** `collisionGroups` (template `+0x104`) goes to the
response (slot `0x48`, read by slot `0x50`). Pairs sharing a bit never meet
(the broad phases `0x006dc7b0`, `0x006dc430`, `0x006dc640`, the soldiers'
`0x006dcce0` unless both are soldiers); bit 0 turns off the ground pass;
and a team's PlayerControlObject passes what has its team's bit (`0x100`
team 1, `0x200` team 2: the as_* and eu_* force fields and doors;
`0x006bb050`'s start). Projectiles (8) never meet each other.

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
  damping` (slot `0xa4`). The wheel is then put back at rest. Its first
  step that finds the wheel moving (`+0x284`, set by `0x006caae0`) takes
  the speed as 0.
- Wheel (`0x006b7910`), for a wheel in contact: it takes the contact's
  velocity along the ground. The engine's torque (slot `0x1a4`,
  `0x0064f2a0`: the torque curve at the rpm × `torque * throttle *
  differential * gear ratio * 1.75`, zero unless the throttle points the
  gear's way; the curve is `0x00629660` at |rpm| up to 1, the defaults
  below, for an engineType with bit 0x80 read as a signed byte through
  slot 5 of the template's `+0x36c` interface, `0x00629140`; any other
  type gets `torque` flat) and the slope's torque (`0x006b7460`: the downhill part of
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
  `0x1a4`) is scaled by the clutch only with `useClutchedTorque`. (The
  rewrite took the torque flat until it followed `0x0064f2a0`: the FAVs
  now pull 0.7–1 of `torque` by the rpm, as the old engines do.)

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

## Other vehicles (tracks, hover, walkers, aircraft)

`src/Game/VehicleParts.cpp` (with `Vehicle::fromTemplate`, tests in
`tests/VehicleModelsTest.cpp`). Every Engine of another type than newCar2
is the old Engine object (template slot 20, `0x00628090`, picks it by
`engineType & 0x40`; constructor `0x0064d350`, vtable `0x00987e30`). The
type's bits: 1 propeller (plane 1, helicopter 0x21, ship 9), 2 car, 4 tank
(c_ETTank 6), 8 ship, 0x10 rocket, 0x20 helicopter, 0x80 newCar2. Only
vehicles carry Engines (none of the static objects, weapons or Titans).
`setEngineType` takes the c_ET constants, which main registers from
`symbols/BF2142.exe.constants.csv` (without them the setter fails and the
template keeps no type).

**The parts.** Each RotationalBundle, Engine, Wing, ForceObject, MechLegs,
LandingGear and FloatingBundle of the tree is a part with three axes (yaw,
pitch, roll: the x, y, z of its vectors) stepped by `rotationAxisStep`
(`0x00563bd0`) on the inputs it names (`inputToYaw` 0x2f8 ... `0x300`),
`rememberExcessInput` (`0x30a`: input banked within ±40, used ±1 a tick),
and its frame R(angles) × its rotation × its parent's (`0x00565a20`). The
vehicle's inputs: PIThrottle (W/S), PIYaw (A/D), PIRoll and PIPitch (the
mouse's x and y in the land and air maps, `Settings/Controls.con`; a tick's
look over 5°), PICrouch, PISprint.

**The old Engine's update** (`0x0064d4f0`, objects update in ascending
object id, so parents first: `0x006ffa30`). Entering starts it (message 4,
`0x0064c890`); leaving stops a ground engine 0.5 s later. Its own inputs
only while running; `canReverse` 0 (`0x744`) drops an input against the
axis's acceleration. A helicopter's roll input sets the angle of attack,
((maxAngleOfAttack − defaultAngleOfAttack) × input + default) degrees at
attackSpeed degrees a second (`0x0064c330`, `0x0064c590`), and the roll
axis is driven to ±1. The throttle position is the roll angle over
maxRotation.z; rpm moves by ((position − load) − rpm/2) × 0.05 (0.07 for
a tank, whose load fades to 0 at differential × 0.75 m/s, `0x00a78fbc`),
within −1 … 1.2; no clutch, gears at gearUp / gearDown at once. The load
is the feedback (`0x0064dd10`): currentRatio (differential × 3.5 / the
setGearRatios table at gear / numberOfGears) × the push / currentTorque
(the torque curve at |rpm|: 0.7, 0.8 at 0.1, 0.9 at 0.3, 1 at 0.6, 0.85,
0.7 at 1, `0x006291d0`, × torque), a car's the mean × 0.99, a tank's the
largest. A tank with engine wheels touching on both sides gets a torque
about up of sign(rpm) × mean friction × trackTurnAcceleration × yaw while
|ω.y| is under mean friction × trackTurnSpeed.

**Thrust** (`0x0064df20`, only for bit 1 and only while the PCO is
occupied, `0x005c87b0`; along the engine's world z):
- plane: s = clamp(z·v / noPropellerEffectAtSpeed × airDensity, −0.2, 5),
  f = 0.1|rpm| + |rpm − s|(rpm − s), force currentRatio × f (× the PCO's
  sprintFactor while sprinting);
- helicopter: −(airflow·z)(|aoa| + 1)/noPropellerEffectAtSpeed + T × aoa,
  T = currentRatio × (0.1|rpm| + |rpm|rpm), z.y × airDensity, × (1 − the
  flat speed / noEffectAtPerpSpeed); pureRotational: half of it as a
  couple about the root; the horizontal damping under 20 and
  horizontalSpeedMagnifier's extra flat push.
- Air density: 1 − y/1000 (`0x0064d3c0`, Physics.airDensityZeroAtHeight).
  A ship's screw only in water, anything else none under it.

**Wings** (`0x0064fd40`, node `0x006cb780`): the lift orientation is the
wing turned by −pitchOffset (the flap's share, flapLift, is 0 in every
shipped wing); the air's velocity at the wing (+ positionOffset) through
the curve `0x006cb600` (peaking at 22.5°), × (wingLift + flapLift) × 1.5 ×
|v|² × 0.0025 × airDensity (× 50 under water), within ±200, against the
wing's normal.

**ForceObjects** (`0x0062c920`, `0x0062b750`): force × input + constant
along the object's forward; with keepUp a couple of ±15 × (2a v + (a² +
b²)(goal − p)) at the object's offset p from the root (goal (0, |p|, 0)),
per regulatePosX/Y/Z (a, b): the walkers, as_hov_light and the aircraft
are held upright by it (stopRegulateWhenUnoccupied).

**The PCO's vertical regulator** (`0x005c9ec0` from `0x005ca2e0`, while
occupied): regulateVerticalPos (a, b) = (−0.8, 2) on PIThrottle (the
aircraft): out = clamp(((a.y − gMod·g) b + v.y) a, −1, 1) under 20° of
lean (faded from 10°), replacing a zero or smaller same-sign input. It
holds vertical speed, not height (as_at let go: about −0.7 m/s).

**The old wheel path** (`0x006b8230` for Springs under no newCar2 engine;
hover tanks' antigravs under none): the spring as newCar2's; touching,
the slip's tangential part is taken out within a friction circle (0.491 ×
friction × n.y a tick, static up to 0.7365), the friction the wheel part's
material against the ground's (as_tank's Air_cushion 4351 multiplies:
0.2 ×). EngineGrip (8) wheels under an engine move the ground at the
track's speed: rpm × differential × 3.5 / the fixed table 4, 2, 1.5,
1.25, 1 (`0x0064c6e0`), a tank's sides ×(1 ± k × yaw) (k = 1.5 ×
clamp(2.5 − |v|/20, 0, 1), `0x0064c440`); EngineDummyGrip (0x80) wheels
only push. The friction goes to the body's tyre mean.

**Walkers** (MechLegs object `0x00650e90`, input `0x00650b20`, response
class 0xc432): inputs taken 10 ticks after a change of state (walk,
back, turn left/right, crouch); 7.5 m/s forward (× 1.3 sprinting), 4.5
back, 0.25 rad/s a tick turning; the feet's probes 1.7 m to the sides push
sideways by 30 when one is off the ground. The response replaces the
hull's ground points: the legs' segment from stepHeight above the foot
(length under the legs' object) to the heightmap, the body pushed up and
the approach taken out, then the drive: the velocity along the ground to
the desired by at most 1 m/s a tick, ω × 0.75. The MechLegs node's leg
probes (slots 118–123) are dead code in BF2142, so spacing, footWidth,
footLength, testCount and legEnableMask do nothing.

**The walker's own animation system** (`bundleAnimationSystem`: the object
`0x0063d340` builds for the template, updated by `0x0063d2f0`; only the
walkers are fed in the rewrite, `GameWorld::stepObjectAnimation`).
`0x0063ce30` gives it each tick: the root node's velocity (slot `0x64`)
less what it stands on (slot `0xd8`, zero here, so the 0.7 m/s floor of
that subtraction never applies), flattened into the object's frame (x
its right, z its forward, not normalised) with its length as the speed,
divided by the system's speed factor (slot `0x54`) when that is over 0;
the angular velocity (slot `0x68`) as the last vector; the slope (slot
`0x3c`): the terrain normal at the object (the heightmap's slot `0x7c`)
against its forward and right × 3.2 (`*0x00a75504`), each within ±1; the
messages 0x800 occupied (interface `0xc4c5` slot `0x18`), 0x10 the PCO's
crouch flag (slot `0x138`), 0x20 its armour destroyed (`+0x48` slot `0x58`:
the walkers' `alive` switch takes its first child, `dead`), 0x40 a landing
gear down (`+0x18`, `0x00650010`) and the legs' 0x80/0x100/0x200/0x400.
The legs (`0x00650680`, `0x00650530` each tick from `0x00650e90`): the
state follows the inputs only while no playing bundle has blockInput
(slot `0x50`, `0x008b9e00`); its message is cleared (0x780) and set again
while the input's sign holds (`0x00650600`): 0x80 walking, 0x100 backing,
0x400 turning left, 0x200 right; state 5 sets the PCO's crouch flag
(slot `0x134`). The system's speed factor (slot `0x54`, `0x008ba420`: the
product of the playing bundles' speed key points) scales the legs' speed
and turn (`0x00650e90`, the root's slot `0x180`); stand_walk's fade-in
key points (0.2 to 1) slow the start, and with blockInput a walk let go
plays on to its next force-stop point before the legs stand. Its
`effect:N` events (event 0x2c + N to the object, passed down its tree)
start the EffectBundles whose startOnEffects has bit 9 + N
(`0x0061a4e0`): effect 1 the right foot's bundle (1024), 2 the left's
(2048), 3 and 4 the walk cycle's sound (`e_sfx_MECH_Walk_Cycle`, 12288)
(`GameWorld::takeObjectEffectEvents`, `ObjectEffects::ObjectState::effects`).

**The legs against objects** (the MechLegs response's slot `0x24`,
`0x006cec60`, for a pair the dispatch met, unless the other's response is
a walker's too): a sphere of the legs' radius (`+0x14c`, 1.5) at length −
radius under the legs' origin along their up, against the other's mesh
(slot `0x60`, its vehicle LOD). Each contact (any normal:
`*0x00a84cec` −1) is pushed along the way from it to the legs' origin
(`*0x00a84cf0` = 1), its height × 10 (`*0x00a84cf4`) when it lies
stepHeight or more under the legs (length − stepHeight below), (0, 1, 0)
when that is nil; the legs' material 4250 (`*0x00a84d00`), 4251
(`*0x00a84d04`) from a relative speed of 20 (`*0x00a84d08`); then as the
mesh pass's contacts (the events, the shares `0x006b6cc0`, both sides).
Its normal joins the drive's (`+0x1b8`) by axis (`0x006b6b60`: the larger
of one sign, the sum of opposite signs), and a contact under the
half-radius line (lower than length − radius / 2) is ground for the drive
(the best normal by height `+0x1c4`, `+0x21c`). The drive (`0x006cdca0`)
runs after the solve, once a tick, and resets the normal to (0, 1, 0).
`GameWorld::legsSphere`, `Vehicle::legsObjectContact`, `Vehicle::legsDrive`.

**Seats.** seatInformation is optional: a vehicle naming no seat (eu_tank,
as_tank, the walkers, the APCs, as_ag) has a seat at each
PlayerControlObject of its tree.

**Measured** (flat ground, `VehicleModelsTest`): eu_tank 37 m in 4 s from
rest, 64° in 2 s at full lock; as_tank floats at 1.6 m, 14.5 m in 3 s, 13
m strafing, 79° in 2 s on PIRoll; as_heavy_mech stands at 3.32 m, 20.7 m in
3 s, 99° in 2 s on the spot; as_at 32 m up in 6 s at full collective;
as_ag 11.5 m up and 43 m ahead in 5 s.

**FloatingBundle** (the *_NP APCs' four floats; object vtable `0x00988240`,
update `0x0064f420`; its node `0x006ca810`, vtable `0x00993c98`, made by the
template's slot 29 `0x00629a90` whatever `hasMobilePhysics` says; tick
`0x006ca860`; template defaults `0x00629780`: hullHeight 1, both lifts 10).
From the float's point p and the water's height w there (HeightmapCluster
slot 0x98): c = ((p.y − hullHeight − w) + offset) / hullHeight and s =
clamp(input − c × hullHeight, 0, 1) (input: minus the bundle's pitch, which
steps only with a maxRotation.y over 0; offset `+0x264` grows by `+0x268`,
never set). Below (c < 0, at least −1): lift = (s floatMaxLift + (1 − s)
floatMinLift) × gravity × c × 0.101833, k = clamp((25c + 1 − c) × drag ×
the root's vehicle-LOD footprint (box x × z, `+0x24 × +0x2c`) × 10 / mass,
±2 `*0x00a070b8`), and the root is pushed up by k × the point's vertical
velocity + lift at p (slot 0xa0, `0x006c7b10`: an acceleration at a world
point). Over 30 m of water eu_apc_NP rides near the surface (−0.06 m),
eu_apc on its hull's buoyancy alone 3 m lower.

**LandingGear** (object vtable `0x00988768`, update `0x00650120`; defaults
`0x0062a3xx`: down under 30 m and 60 m/s, up from 20 m and 70 m/s, no
delays). A ray down gearUpHeight long gives the height (none: the largest
float; the rewrite's meets only the terrain). Down and under gearDownHeight
and gearDownSpeed: the down delay starts and it goes down; at gearUpHeight
and gearUpSpeed: the up delay and up. After its delay each axis gets +1 up,
−1 down (the other way with invertGear); after more than 5 ticks without an
update (`*0x00a0553c`) the axes stand at their limits. The shipped gears
(eu_at, us_ag: down under 10 m and 10 m/s, up from 20 m/s) have no axes:
only their sound reads the state.

**regulateVector** (RotationalBundle `+0x314`; `0x005644b0` through the
input handler `0x00565180` for a bundle under the root PCO, or the input
pass `0x00565700`): with regulateFromParent −1 (every shipped bundle), the
root node's angular velocity (slot 0x68) carried through the bundle's and
the root's matrices (`0x006acfa0`, `0x004b9d60`) — for an upright bundle
its component about the hull's up — times regulateVector, within ±1,
replaces an axis input that is 0 or weaker the same way. eu_tank's cupola
(−0.5, 90°/s) holds its heading against the hull's turn. The rewrite
regulates the gunners' yaw and the seat cameras' yaw
(`GameWorld::regulation`); pitch and roll regulation and
regulateFromParent(Pos) aren't (the rewrite's gun pitch is the aim's).

**Pilots' guns.** A pilot (a vehicle that flies) fires the seat's own
GenericFireArm, fixed along the hull, with PIFire. The ground drivers'
guns turn on the mouse in the original, which the rewrite's drivers spend
on PIRoll: they aren't fired.

**Spawned in the ground** (a deviation): the shipped spawners stand on the
ground and spawn at their spot (`0x005aeb80`). The rewrite lifts a vehicle
whose box bottom is under the terrain onto it at creation: its push out of
the ground in one tick and the springs' rate after it otherwise wrecked
the gunships and APCs on their pads.

Not rewritten: the wing's liftRegulated, regulateToLift and
wingToRegulatorRatio (no shipped Wing sets them, so they never act); the
yaw/pitch/roll PCO regulators (no shipped vehicle sets them); the other
bundle animation systems (eu_at, us_ag, the sentry gun, the deployables)
aren't fed, as nothing of theirs reaches the simulation.

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

- **The collision events** (object slot `0xac`): before the shares, over
  |v|² 0.1 (`0x920bd8`) or on B's material `0x29a`, A's and B's callbacks
  with their velocity relative to the other. The base (`0x0063c9e0`) asks
  the material's veto (slot `0x70`, which always answers no:
  `0x006a10f0`), and the object's first armour keeps a second's memory of
  each (other object, own material, other's material) (`0x00653ef0`); a
  new pair reaches the server game logic's slot `0x104` (`0x004ba3f0`; the
  client's is empty): from the ground (`0x005a3db0`) the damage is the
  material cell's `damageMod` (slot `0x64`: the cell, else the first
  material's against 0, else 1) × speed² × max(|cos|, `armor.angleMod`),
  a soldier only over 9.1 m/s and by the speed over it (fall damage);
  from an object (`0x005a4690`) (speed − 4)² × cos² × the cell × the mass
  factor (`0x005a2ca0`: clamp(mA / mB, 0.3, 3) for two mobile objects, a
  soldier weighing 200), credited to the attacker's driver unless a
  teammate ran into it faster (`0x005a30b0`); soldiers deal none, and a
  soldier hit is run over (`0x005a43c0`: from 10 m/s, or 3 when the object
  is mobile and faster, (speed − that) × speed × the mass factor). The
  ground pass calls the object's callback while |v|² is over
  `handleCollisionSpeed` (default 1, compared unsquared) and ignores its
  answer. `GameWorld::collisionEvent`.
- **The contact effects** (`0x006b75d0`): from handleCollisionSpeed² on (both
  objects'), one response (A's root if only A moves, B's if A doesn't, the
  first of two mobile ones) records the contact in one of two slots: with
  a sound in the material cell (index 0, `0x0060a6d0`) the sound slot,
  else the effect slot; a free slot takes it. At the end of its solve
  (`0x006ba7d0`) the effect plays (the cell's effect 0, `0x006b8c40`: at the
  point, up along the normal, forward against the slide) and the sound
  0.05 m out, with its effect (not for the materials 61 webMat_Tracked and
  67 webMat_wheelBig). A slot is free again when its 0.5–1.5 s timer
  (`0x006b74e0`, four times as fast with no contacts) runs out.
- **Wheels** on objects: a wheel part's vertical probe contacts go to the
  wheel's own response, as the heightmap's do; the solve's Spring branch
  moves the wheel out along their mean normal by its push (0 to 1 m), so
  its spring and tyre (`0x006b8230`: the response's mean normal, point and
  velocity) treat another object's mesh as ground moving with it
  (`Vehicle::touchObject`).

Not modelled: the response's stuck push (`+0x11c`, normally 0) scaling the
events' velocity, the impact effects and decals of the damage
(`0x005a36d0`), and B's vertices into a wheel's three-vertex LOD.

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

The pose is built per bone (`0x008b8330`): the first instance that moves
the bone sets it, and each later one slerps towards its own by its weight.
A weapon's system goes after the soldier's, so its arm and weapon clips
lie over the body. Bones nothing moves keep their last pose. The held
weapon's bundled-mesh parts hang from the skeleton's nodes `mesh1`,
`mesh2` and so on, which the weapon's clips animate.

Bundles with `useNewSystem` (only the mechs' use it) work differently
(`0x008c30a0`). They fade linearly over their fade times, counted in the
bundle's own time. Once one is no longer queued it plays on to the next of
its `forceStopPoints` (fractions of its length, `"0.3;0.8;"`) before it
fades out; with no stop points it fades out at once. While a bundle with
stop points plays without fading, bundles of other priorities that are not
fading out hold still. `speedKeyPoints`, `fadeInSpeedKeyPoints` and
`fadeOutSpeedKeyPoints` are polylines over the bundle's length or its fades
(`0x008c2cd0`). Their product over the playing bundles (`0x008ba420`) is a
factor the vehicle multiplies into its movement (`0x00650e90`) and divides
out of the speed it gives the system (`0x0063ce30`).

A trigger with a `fadeInTime` fades the whole system in (`0x008b9720`).
The system's pose is slerped from what the bones were before by the
fraction of that time gone, and only a different trigger restarts it.
Movement bundles keep the soldier's step (`0x008c46d0`). The system
remembers where the last movement bundle was in its step (0..1). A new
directional bundle starts there, and side movement past ±0.5 resets it to
the start offsets (`anim_startLeftOffset` 0.2, `anim_startRightOffset`
0.7). A MovementTrigger with no value holder and two bundles picks the
standing foot by that phase. The backward clip plays half the strafe
clip's length ahead (`anim_offsetBackwardTime`). When the forward/backward
or strafe clip changes while it dominates, the bundle's pose fades in
again over its fade-in time.

CountTrigger plays its first child until the bundle on top has played
through once, then its second for good (`0x008c42b0`; the sentry gun's
"open"). LookAroundTrigger blends a two-clip bundle by one look value and
scrubs it by another (`0x008c3d00`; seats and bipods). SlopeTrigger leans a
five-clip bundle to the side by the smoothed ground slope (`0x008c4f00`;
the mechs).

**What the triggers read.** The object passes its movement to the system's
setter (`0x008b9ae0`, slot `0x38`): a time, a direction, a speed and one
more vector. The direction is `(x side, y up, z forward)` (`0x14`), the
speed `0x20`, the vector `0x38`. Given a time the direction's x and z ease
towards the new ones by `anim_smoothSet` (10 per second; at once when one
of them swaps sign past 0.9, or the new direction or speed is nil), and the
accelerations (`0x2c`) are the old speed × direction less the new, per
second. By the trigger factory (`0x008c1170`) and the types' vtables,
ForwardTrigger (`0x009dcd00`, `0x008c48d0`) tests speed × z, SideTrigger
(`0x008c4920`) speed × x, UpTrigger (`0x009dcde0`, `0x008c4980`) the
direction's y and TurnTrigger (`0x009dce50`, `0x008c49e0`) the vector's
second part (`0x3c`); with `triggerOnAcceleration` the first three test the
acceleration instead, and with `useDirection` Side and Up the absolute
value. The rates (slot `0x4c`): Forward and Side speed / normal (Side with
`useDirection` times the direction's y, as in the binary), Up |y / normal|
signed by y with `useDirection` (the ladders climb down by playing their
bundle backwards), Turn |turn| / normal, and a turn sets the step phase to
`anim_startLeftOffset`. The rewrite had Up's and Turn's tests the wrong way
round; fixed.

The soldier (`0x005ef250`) passes the horizontal velocity relative to what
it stands on (nil under 0.7 m/s when that moves), its direction in the
soldier's frame, and as y its jump state (`0x005eef80`): standing (the
response's `+0xf9`) 0; rising at `anim_startJumpAtSpeed` (2.5 m/s) or more
starts a jump (`+0x2e9`, the jump sound) that reads 1; falling faster than
`anim_startLandAtSpeed` (−0.5) a ray from the feet along the velocity,
`anim_startLandAtHeight` (1 m) long (straight down that far under 1 m/s),
looks for the ground (objects, then the heightmap; `+0x2e8`, the land
sound), after which the value is the vertical speed; otherwise the vertical
speed when falling, else 0. Its vector is its rotational velocity × 0.01:
degrees per second, at most 1499 (`0x0058a6d0`), so `3p_turnLeft` (−0.05 to
−1000) starts at 5° a second. `stand_jump` (`3p_jump`, 0.25 to 8) plays
while the jump reads 1.

**Clip sequences.** A bundle's clips are a list: `addAnimation` puts each
first, `setAnimationStartTime` moves it before the first starting at or
after it (`0x008bf1f0`, `0x008bf280`); a bundle's clip index counts from
the front (`0x008be470`). A bundle with a looping clip (`+0x34`, set when
the scripts finish, `0x008bea30`) is inactive between updates like a
looping one. While queued its looping clips repeat; not queued, with
`abruptPlayback` its time skips to the end of the last looping clip,
remembering the skip (`+0x40`; `0x008c3360`). Drawing (`0x008c2e50`) goes
through the clips by start time up to the bundle's time: a running clip (or
any with `playForever`) is drawn, a finished one only if it loops (shifted
by the skip), each until the next one has faded in over its own
`fadeInTime`. The soldier's jumps are such bundles: the start, the looping
middle from 0.25 s and the landing at 2.29 s; once the UpTrigger stops, the
middle is skipped and the landing plays. A one-shot without a looping clip
queued again while it plays restarts with `abruptPlayback` (`0x008c29d0`).
`jumpToLastAnimationAtStop` plays the bundle at rate 1 whatever its
trigger's rate (`0x008c2950`; the binary does nothing else with it).

**Events.** `addEvent` (`0x008bf370`, `0x008bde00`) keeps a lower-cased name:
`effect:N` is effect N, `sound:N...` sound N (N read by `sscanf` from the
start); `setEventStartTime` sorts them by time (`0x008bf4d0`), and a
bundle lasts at least to its last event (`0x008be400`). Each update, before
the time step, every event at or before the bundle's time fires once
(`0x008c3030`, inline in `0x008c3360`), and a loop starts them over. They
go to the object's event handler (slot `0x100`) as `0x36 + N` (sounds) or
`0x2c + N` (effects). The soldier (`0x00587b90`) plays its sound set's
sound (`+0x324`, `0x007fd970`) for event `0x36`, unless it is only being
posed (`+0x2ec`): the footstep set loaded for what it stands on
(`0x007fda00` → `0x007fd840`: the cells of material `0x157c`
(Soldier_FootSteps) against a material; templates 0–3), its template 3,
else 0. The material (`0x007fda00`): on a ladder `0x19c8`; standing or
swimming (the response's `+0xf9`, stance 3) the other material of the
soldier's last collision event (its handler `0x00586a90` keeps it at
`+0x32c`, read through `+0x18c` slot `0x60` = `0x0063a0f0`; −1 at first):
the terrain's, or the face's of the object it walks on; in the air the set
stays. Its jumps' `sound:0_first` is the landing sound; the landing
(`0x005ef250`) also stores its ray's material at `+0x32c` and plays
through `0x007fdcd0`, which asks the player (slot `0x1bc`, `0x00591510`):
the PlayerManager's local player (slot `0x30`) in a view whose kind (slot
`0x1b4(0)` → `0x1c`) is 1 or 2 — so only the local player hears its
landings (the rewrite has no view kinds). `GameWorld::stepAnimation`,
`playAnimationSound`.

Effect events (`0x2c + N`, N 0–9) go down the tree: the base handler
(`0x0063ca60`, which the PlayerControlObject's `0x005c50e0` hands them to)
passes every event to each child's slot `0x100`; an EffectBundle
(vtable `0x0097b658`, `0x0061a4e0`) starts when its `startOnEffects`
(template `+0x454`) has bit 9 + N (`1 << (event − 0x23)`) and stops on
`stopOnEffects` (`+0x458`); it isn't started again while running (`+0x18e`)
unless the template's `+0x3f4` is below 0. Shipped: `e_muzz_eu_av_reload`
(512, effect 0) under a weapon, the mechs' `FootStepEffectRight` /
`Left` (1024, 2048: effects 1 and 2 of their walk). The rewrite starts a
held weapon's bundles from its animation's effect events
(`GameWorld::startEffectsOn`; one-shot, so the stop mask has nothing to
end); the mechs' legs aren't animated, so theirs never fire.

**Camera springs.** `animationSystem.cameraSpring.use 1` gives a system a
spring (`0x008b9670`, 0x50 bytes, `0x008bbfe0`): amplitude, zoomDamping,
stiffness, move, hit and fire (x/y/z) and look (x/y). The soldier
(`0x005ee980`) takes its first-person system's and lets the held weapon's
override each part the weapon sets (`0x008bc390`), then each tick: the
three values (`+0x2f4`) relax by value² × stiffness × dt, at least
`anim-soldier-spring-clamp` (0.001), to 0 under 0.0001 (`0x005edda0`); x
goes by −look.x × the turn, y by look.y × the pitch (degrees this tick,
`+0x300`, `+0x304`); the velocity pushes x by its sideways part × move.x
and y by −0.3 of that, minus the forward part × move.z, plus the upward
part × move.y; the weapon's recoil (`0x005861a0`) by fire; all × dt and
clamped to ±1. The first-person skeleton's root is then moved by amplitude
× value along the view's axes (`0x006acfa0`, `0x008b8880`); zoomed in, each
amplitude goes towards amplitude / (zoomDamping × 6,
`anim-soldier-spring-zoom-factor`) by how far the field of view is zoomed.
The rewrite applies them to its first-person view (see below); `hit` is
never read by the soldier.

Not rewritten: the per-instance time override (system `+0x84`/`+0x88`),
the vehicles' (the mechs') effect events. The first-person weapon's are
played (see below).

### The hands' IK (`ik.*`)

The template component Ik (class `0xc4cc`, interface `0xc4cd`, vtables
`0x0097b4d0` / `0x0097b4b8`, constructor `0x00618430`) is on the FAVs'
steering wheels (`AS_FAV_SteeringWheel`, as_hov_light's) and the
stationary guns' firearms. Its six properties: `ik.positionLeft` (`+0xc`),
`rotationLeft` (`+0x18`, a matrix from degrees as object rotations are,
`0x006adb30(−z, −y, −x)`), `poleVectorLeft` (`+0x58`), and the right's
(`+0x64`, `+0x70`, `+0xb0`). Entering (`0x0058cf00` → `0x005ee910`) the
soldier finds the IK object: depth first from the seat's first child over
children and later siblings (`0x005ed8c0`), not into other seats
(PlayerControlObject `0xc4c2`) or parachutes (`0xc5a8`), the first object
whose template has the component; none, the same from the seat's next
sibling. Each frame, third person only (`0x005f06b0` → `0x005efc00`;
the first-person pose `0x005ee980` has none), the targets are the grips in
the soldier's space (the IK object's world × the soldier's inverse,
`0x006acfa0`): rotation × that turn, position and pole through it; the
left arm at bone `0x12` (joint19) with end `0x13` (left_wrist1), the right
at `0x22` (right_ullna) for both (`0x008b92b0`; 3p_setup.ske's
B−3 shoulder, B−2 elbow, B−1 lower arm). The hierarchy pass (`0x008b8880`)
solves an arm where it reaches its bone (`0x008b6760`): the wrist from the
pose, d the elbow-to-lower-arm length, the handle t + unit(s − t) d and the
effector e + unit(s − e) d (s the shoulder), Maya's ik2Bsolver
(`0x008c26f0` → `0x008c2160`, doubles, twist 0: the middle turn
acos((|vH|² − |v1|² − |v2|²)/(2|v1||v2|)) − angle(v1, v2) about v1 × v2,
the start turn from the effector to the handle and the pole's plane);
the shoulder takes the start turn, the elbow half the middle turn
(`0x00964c78`) and the lower arm all of it, each placed from its parent
again, and the end bone takes the target whole; no blend, the arm doesn't
stretch. `anim::worldMatricesWithIk` (`src/Animation/Ik`),
`GameWorld::seatIkTargets` (the IK object's template path kept by
vehicle template and seat). Not rewritten: the disable flag
(`+0x2ea`, `0x005ed890`: a PlayerControlObject template's `+0x430`
through `0x005c7cf0`, and dying) and the weapon layers `0x005ef250`
switches for an IK object with `0x94c0`. A straight arm (v1 ∥ v2) gives
no plane to bend in, as in the original.

## The first-person view

**The camera.** On foot the player looks through the soldier's camera, its
first Camera child (`SoldierCamera`, soldier `+0x208`; `0x00588ec0`), which
the Soldier moves each tick at the end of its update (`0x005890a0`, from
`0x0058a6d0`): the local position is (0, y, 0) in the body's frame, y the
pose's `poseCameraPosition` (getter `0x00586380`; the setter `0x005c0c70`
keeps only y: stand 0.47, crouch −0.05, prone −0.63 over the origin, a
metre above the feet; a seated soldier counts as crouching), reached 0.2 of
the way each tick (`*0x00a68e14`, `*0x00a68eac`, `*0x00a68e18`, all 0.2,
`0x0043dd80`; the camera-movement-speed vars of `Common.con` are in a
`beginrem` block and not in the binary) from where the template put the
child (0/4.1/0, so a fresh soldier's eye drops into place). The local
rotation is Rx(pitch) × Ry(twist) (slot `0xe8`): the aim's pitch (±85°,
`*0x00a68eb0`) and the head's twist over the body (±40°), i.e. the look.
The field of view is the player's (`+0xc4`, moving to a target `+0xc0` at a
rate `+0xc8`: `0x00591740`, `0x005912c0`), by default the camera's
`insideFOV` in the inside mode, `worldFOV` in the others (`0x00610150`;
template default 1.1, `0x00610fd0`; `SoldierCamera` sets 1.0), vertical,
radians. RendDX9's setFov (`0x10066ac0`) keeps it from 4:3 to 16:9 and, for
height / width of 0.8 or more, scales it by that × 4/3.

**Modes.** The camera modes (`0x004db0c0`, `game::camera::Mode`):
FirstPerson, Inside, Nose, Chase, FrontChase, FlyBy, Trace, ExternTrace.
`setMode` (`0x00612eb0`) takes 0 as Inside and refuses a mode without its
template flag (CVMInside `+0x378`, CVMNose `+0x37e`, CVMChase `+0x379`,
CVMFrontChase `+0x37d`, CVMFlyBy `+0x37a`, CVMTrace `+0x37b`,
CVMExternTrace `+0x37c`; the first four on by default, `0x00610fd0`), Nose
also without sv.allowNoseCam (game logic `+0x32`), Chase, FrontChase and
FlyBy without sv.allowExternalViews (`+0x36`), ExternTrace while its
extern point (`+0x2dc`, the translation of the matrix at `+0x2ac`, set
only by ICamera slot 0x3c `0x006136b0`, which nothing calls) is zero: so
always in retail; its draw would be the camera's own frame (`0x00612d80`'s
default).
`c_PIToggleCameraMode` (input 0x23, `0x004bda00` → `0x004b9e10`) steps on
over 0..7, wrapping, until `setMode` takes one or it is back; the seat
camera then draws (`0x00612d80`) by mode (`src/Game/Cameras`, played by
`PlayerCamera`; the original's view matrices, eye, point looked at, up):

- **Inside** (`0x00610620`): the camera's own frame, its shakes, kept
  0.09 over the terrain (`*0x00a73a04`). insideViewInterpolate (`+0x430`,
  a slerp of the turn) is 1 in every shipped camera, not rewritten.
- **Nose** (`0x00610800`): the camera's frame moved by nosePos (`+0x380`)
  turned by its parent's rotation.
- **Chase / FrontChase** (`0x006108e0`, reset `0x006105c0`): the point
  looked at is the root, or chaseOffset (`+0x39c`) in the chase parent's
  frame (chaseParentIndex `+0x3a8`: -1 the root, else that many parents up
  from the camera, 0 itself); the distance chaseDistance (`+0x394`), or for
  -1 the root's radius (at least 2) × 2 (`*0x00a739ac`); the direction
  normalize(∓forward + chaseAngle (`+0x398`, 0.3) × up) of that parent,
  behind for Chase, ahead for FrontChase (a camera under a soldier whose
  root isn't one takes the radius distance and 0.3). Direction (a 4-vector
  slerp, `0x006ac980`) and distance ease by clamp(followStiffness (`+0x390`)
  × seconds, 0, 1); the velocity term (`*0x00a739b0`) is 0. The eye stays a
  metre over the terrain and the sea (slots `0x6c`, `0x98`).
- **FlyBy** (`0x00612bb0`, points `0x00612b10` / `0x006123a0`, view
  `0x00612660`): a point ahead (the root's radius × 1.2 along its forward
  plus 0.3 of its velocity, × 8 × 0.4, the object's `+0x254`), a metre
  over the sea, joins a list when it is empty or its first is 8 × 60 s old;
  the eye is the list's point (between it and the next by the camera's
  clock `+0x26c`), half a metre over the terrain, looking at the camera.
- **Trace** (`0x00610d20`; `setMode` puts the point 20 m ahead): the
  camera's position looking at a point that moves with it and eases (0.1
  a frame) to 20 m along the parent's forward while off it by more than
  acos 0.9 (put there once within), or goes 0.2 a frame to the player's
  slot `0x338` player's object (its killer, while alive) when that is over
  2 m away; the parent's up.

On foot the original stays in first person; the rewrite's C keeps its own
chase view there (`--camera-mode third`), and a third-person choice starts
a seat in Chase. `PlayerCamera::externalView` hands the viewer every view
the game places (`ViewerOptions::PlayerView::external`).

**The dead player's camera** (the player's own camera, vtable
`0x0097a8b0`, mode at `+0x208`; its matrix `0x00615e70`). The death
(`0x004cd390`) sets mode 6 with the control object (the vehicle, or the
soldier): `0x00615f10` keeps it (`+0x198`), the wanted distance max(3,
3 × its radius) (`+0x30c`), the target its position (`+0x300`) and the
facing the camera's forward (`+0x2e0`). With no player followed (`+0x220`
-1) it is the **death camera** (`0x00615970`): the target follows the
object while it is there; the direction slerps (clamp(seconds ×
cam-stiffness 1.5, 0, 1), `*0x00a73a38`) to normalize(−facing + 0.3 up),
the facing turning (horizontal) to the killer (`0x00614940`, the player's
slot `0x338` while alive) once the distance is past 0.8 of the wanted; the
first frame a tenth of −facing at 0.5 m; the smoothed target eases by
seconds × cam-interpolate-attachobject 1.2 (`*0x00a73a3c`); the eye 0.8
(deathcam-terrainoffset, `*0x00a73a24`) over the terrain, looking at the
target. **Following** a player (`0x00614d00`): behind and above that
player's camera by its root's radius (−forward × r + up × 0.3 r), the
offset easing 1 − e^(−1/15) of the way a frame and lagging 0.6 of the
root's velocity, a metre over the terrain (the PlayerControlObject's
camera offset and scale, slots `0xd4` / `0xd8`, aren't taken).
**Flying free** (mode 1 unattached, `0x00615290`; with sv.allowFreeCam
`+0x35`, a demo, or a spectator): c_PIYaw strafes and c_PIThrottle goes
along the look at 20 m/s × the speed (cam-normal 1, cam-fast 10 with
c_PIAltSprint or once fast and going on, cam-slow 0.1 with c_PISprint;
`*0x00a73a2c`, `*0x00a73a20`, `*0x00a73a30`), c_PILie up and c_PICrouch
down, 0.2 over the terrain; the keys' and mouse's smoothing
(cam-free-interpolate-*) is 1. While dead with sv.allowFreeCam,
c_PIAction (`0x00616110`) swaps flying free and mode 6 (the kept object
again, else the next teammate), and in mode 6 c_PIToggleCameraMode takes
the next living teammate (`0x00615e00` → `0x00615ce0`: the players' order,
the one after the followed one, else the first). `FreeCam.togglePlayer`
and `FreeCam.toggleCamera` (the HUD manager's slots `0x130` / `0x134`,
`0x00752050` / `0x00752150`) do the same. `PlayerCamera::spectating`.

**Seats.** In a vehicle the camera is the seat's (Player::getCamera
`0x005948e0` → the PlayerControlObject's `0x005c7cb0`): the seat's Camera
children (by `cameraId`, `0x0058bdb0`), placed by the template tree, so a
gunner's camera under the gun's pitch bundle follows the gun; its
`insideFOV`. The seat's cameras are mapped by `CameraId` (0 by default);
the rewrite takes the seat's camera with id 0 (nested seats left out) and
turns the bundles as `GameWorld::partPose` does.

**The seat camera's shakes** (the Camera object's per-tick `0x00610240`;
the view matrix `0x00610620` rolls about its forward row by the sum of
four angles, degrees, `0x005b1a70` → `0x006adf40`). The speed shake: past
the template's `cameraShakeStartSpeed` (`+0x3d0`, 0 off) the time above it
runs and the roll is sin(fmod(t × 40, 2π)) × 0.5 × the share of the way to
`cameraShakeMaxSpeed` (`+0x3d4`; all of it when not above the start)
(`*0x00a739f8` 40, `*0x00a739fc` 0.5, set by `0x008e9120`/`0x008e9140`).
The hit shake (`0x00610450`, slot 19 of `0x00979a50`): 0.6 s
(`*0x00a73a14`) of (1 − u) × sin(u × 7 × 2π) × the hit × 2 (`*0x00a73a0c`
7, `*0x00a73a10` 2); its caller wasn't traced, so the rewrite gives the
share of the vehicle's hit points the hit took. (A soldier camera's own
bob, `+0x3d8`, and the `0x006101c0` shake started by `0x00610180` are not
rewritten.) The camera never sinks below the terrain + 0.09
(`*0x00a73a04`). `PlayerCamera::View::roll`, drawn through
`render::Camera::roll`. Template defaults (`0x00610fd0`): CVMInside,
CVMChase, CVMFlyBy and CVMFrontChase on, followStiffness 1, chaseDistance
−1, chaseAngle 0.3, cameraShakeSensitivity 0.1, worldFOV and insideFOV
1.1, allowInsideDynamicLights on, the interior sun off with direction
(−0.25, −0.5, 0.75), colour (1, 1, 1), damage colour (1, 0, 0), ambient
(0.7, 0.7, 0.7), insideDamageFrequency 3, insideDamageThreshold 0.15,
insideViewInterpolate 1, saturation 1, cockpitLod / cockpitSubGeom −1.
`PlayerCamera::insideSun` serves the interior sun (the getters
`0x006104d0`..`0x00610580`, the damage colour flickering below the
threshold; the flicker's wave is the rewrite's); the renderer doesn't
light by it yet. The chase view's field of view is the seat camera's
`worldFOV`. A joined client keeps first person when the server's
settings block has `sv.allowExternalViews` off.

**Before the first spawn** the view is the level's
`GameLogic.setBeforeSpawnCamera <position> <yaw/pitch/roll>`
(`0x004278ef`; the levels' `Init.con`), `GameLogic::beforeSpawnCamera`.

**Ducking.** A seat whose template has `allowDucking` (+0x468; only the
stationary bipods ship it) ducks while c_PICrouch (input 38) is held: the
PlayerControlObject's input (`0x005ca2e0`) calls setDucking (`0x005c7cf0`,
slot `0x12c` of its `+0x184` interface), which with more than one camera
selects camera 1 (the `DuckCamera`, `CameraId 1`), and with
`disableInputWhileDucking` (+0x469, 1 by default) the seat's inputs are
cleared (`0x005c9ec0`) and fire events are ignored (`0x005cacf0`).
Rewritten in `PlayerCamera::ducking` (the main loop clears the seat's
throttle, strafe, turn and fire while it ducks).

**The cockpit.** The Camera's geometry switch (`0x00612880`) runs for the
local player's camera: for the root object's and its children's meshes
with more than one geometry, in the first-person modes (0, 1) it sets the
sub-geometry (mesh slot `0x6c`) to the camera's `cockpitSubGeom`
(template `+0x44c`, -1 by default) or, when that is -1, the seat's
(PlayerControlObject slot `0xf8` = `0x005c6b90`: template `+0x428`, 0 by
default) and the LOD (slot `0x78`) to `cockpitLod` the same way (`+0x448`
/ slot `0xf4` = `0x005c6b80`, `+0x424`); in the other modes geometry 1 at
the automatic LOD. So a driver sees geometry 0 (the cockpit) unless the
seat says 1 (the FAVs). Rewritten in `PlayerCamera::cockpit` and
`LevelSources::geometryFor`.

**Free look.** There is none beyond the seat camera itself: Cameras are
RotationalBundle-type objects (`setInputToYaw PIMouseLookX`,
`setInputToPitch PIMouseLookY`, min/max rotation, `automaticReset`, the
axis update `0x00563bd0`). The drivers' (eu_fav_driver_camera −125..120
across, −20..40 down at 90° a second, as_fav_PilotCamera ±130 / ±20) turn
and stay; the FAV gunners' (eu_fav_MachineGun_Camera, as_fav_GunnerCamera:
±1 / ±0.5, maxSpeed 5, acceleration 1 / 0.5, `automaticReset`; the eu one
about its pivot 0/0/−2) lean with the mouse and settle back.
`GameWorld::stepSeatCamera` turns the seat's Camera (the first with a
PIMouseLook input under the seat) each tick with `rotationAxisStep`:
PIMouseLookX is the turn over 5 degrees, PIMouseLookY the pitch's change
over 5 (down positive), each kept within −1..1 (only `rememberExcessInput`,
not rewritten, banks what goes past); a new sitter starts looking ahead
(`restoreRotationOnExit`). `partPose` gives the camera node's turn about
its pivot, which the seat camera already applies to its own node.

**The arms and the weapon.** For the local player in first person the
soldier's character mode (`+0x2f0`, `0x0058d350` → `0x005f0f20`) is 0, which
is also the geometry index the soldier's and the held weapon's meshes draw
(slot `0x6c`, `0x005f06b0`): geometry 0 is the first person (the arms of the
soldier's skinned mesh; the weapon's bundled mesh), 1 the third. The
skeleton and animation system are the mode's (`+0x2c0`/`+0x2c4` + mode × 8):
`skeleton1P` and `animationSystem1P` for the soldier and the weapon. The
first-person skeleton (`1p_setup.ske`) is the arms and mesh1..mesh16 around
`Camerabone`; `0x005ee860` keeps the inverse of that node's setup matrix,
and each tick `0x005ee980` sets the root to it × the camera's world matrix
(`0x006acfa0`), so the camera bone sits on the camera, plus the spring's
offset along the camera's rows; the bones hang from that root
(`0x008b8880`). The weapon's root is the skeleton's node 54 (mesh1) and its
parts the following nodes. The renderer's meshes with a first-person
field of view set (slot `0x10`, `soldier-fov1p` 0.82; only ≠ −1 is tested)
go to the first-person queues (RSMT1Person), drawn after the world and its
particles, after a depth clear (`0x1002cef0`, if `renderer.draw1pMeshes`),
with a view of their own (renderer `+0xfc`): 0.82 rad whatever the zoom,
near `renderer.tweakNearPlane1p` 0.16, far 15, the world view's camera
(`0x1002aba0`, `0x10028bb0`).

**Zoom** (DefaultZoomComp, defaults `0x005dacc0`): `addZoomFactor` builds a
list starting with 0 (duplicates skipped, `0x005dae50`); the alternate fire
(`0x005da670`) steps to the next level, past the last back to 0. A level's
factor is the field of view in radians it asks the player for, 0 the
camera's (`0x005da3f0`): over `zoomDelay` (0.5 by default) unless
`changeFovDelay` is set. Zooming in sets the first-person weapon's LOD to
`zoomLod` after zoomDelay ticks (`0x005da770` / `0x005da290`; geometry 0's
LOD 1 is the sight), zooming out LOD 0 at once; reloading zooms out
(`zoomOutOnReload`, `0x005da630`). Zoomed, the mouse's yaw is scaled by the
field of view over the camera's and `mouseSpeedMod` (`0x00586420`). The HUD
shows the weapon's `altGuiIndex` (its zoomed reticle). The muzzle children
(FireComp `0x005f25f0`) named "zoom" flash instead of the others while zoomed
with `disableMuzzleWhenZoomed` (`0x005da050`, `0x005f31d0`); they are the
weapon's children, so they follow the first-person weapon and are drawn in
the first-person particle lists.

**The recoil kick** (SoldierBasedRecoilComp: template `0x005dd730`, the
weapon's component `0x005dd370`). The template builds a graph
(`0x005dd890`): n = `recoilGraphFrameCount` (8) values g[i] = −(i + 1)^e ×
T / Σ(i + 1)^e (e `recoilGraphExponent` 2, T `recoilGraphTotalMovement`
2), so they add to −T; with `goBackOnRecoil` (1) a second graph of n + m
values (m = round(n/4)): base = −g[n−1]/2, the first m are base − 1.5 ×
g[m − i], the rest 1.5 × g[i − m] + base, and the positive values are
scaled to cancel the negative ones (`0x005dd030` picks it). A shot
(`0x005dd490` → `0x005dd3f0`) samples `recoilForceUp` (and
`recoilForceLeftRight`), times `zoomModifier` while zoomed (`0x005dd090`),
and records the tick in a ring of 10. The value (`0x005dd5d0`, the
firearm's slot `0x11c` = `0x005c1920` → the component's slot `0x94`) is,
k ticks after the last shot, force × graph[size − 1 − k] for k < size − 1
(the firearm's slot `0x110` factor is 1, `0x005b6b10`); the soldier
(`0x005861a0`, only with `hasRecoilForce`: slot `0x100` → `0x005dd080`)
feeds it to the spring's fire part and adds it to the aim's pitch
(`0x0058a6d0`; the left-right value, `0x00586140`, to the yaw). The
rewrite's spring takes it (`anim::RecoilSettings`, `recoilAt`). The aim's
kick is `GameWorld`'s (`Player::recoil`, set with the weapon in hand): each
tick of the soldier's update the left-right value joins the turn (so it
twists the look like the mouse) and the up value the pitch; the soldier's
update reads it before the weapon's tick fires, so a shot's own tick (k =
0) passes. The rewrite's pitch is the viewer's absolute value, so the kick
waits in `aimKickPitch`, added to the aim (`aimPitchOf`, the shots,
`PlayerInfo::aimPitch`) until the viewer takes it into its own pitch
(`takeAimKick`, `ViewerOptions::takeAimKick`); a bot's aim takes it into
its own angle and corrects from there. `zoomModifier` is the viewer's (the
world keeps no zoom).

**Effects on the first-person weapon.** The weapon's bundles follow it:
the muzzle flash and the bundles its first-person system's effect events
start (`0x2c + N` → startOnEffects bit 9 + N, `0x0061a4e0`) are spawned
against an anchor at the weapon's root (mesh1) that moves each tick, with
the systems that have `showInFirstPerson` (so `glow1p` rather than
`glow3p`); `isLocalSystem 1` systems keep their particles in their own
space. The shipped first-person systems only have effect events (reloads'
`effect:0`); their sound events would be the soldier's (`0x00587b90`).

**In joined games** (`--connect`) the soldier is the server's: the view
takes the control object's position (as the feet), our own look, and the
kit HandlePickup gave (`RemotePlayer::kitGhost`, its firearm with the
selected `itemIndex`, 3 the primary), and the held fire input fires at
`fire.roundsPerMinute` for the muzzle and the kick (the client's own
prediction; the server's shots aren't decoded). Seats there keep the
chase view.

Rewritten in `src/Animation/FirstPerson` (rig, eye, zoom, field of view,
recoil), `src/Main/PlayerCamera` (the player's camera from the game world
or the remote ghosts, seats, ducking, the cockpit, the models, the
weapon's effects) and `LevelRenderer`'s first-person pass (opaque and
additive materials, the first-person particles, then the transparent ones
back to front). The world's near plane in first person is 0.1 (the
original's `renderer.tweakNearPlane` is 0.041 with a 400 m view).

Rewritten in `src/Animation/AnimationSystem` and `AnimationConsole`.

## Transparent meshes and attached effects (RendDX9)

**Render queues.** RendDX9's shader manager (`0x1003ed70`, global
`0x101fae6c`) keeps queues by name (RSMTTransparency `+8`, RSMT1Person
`+0xc`, RSMT1PersonTransparency `+0x10`, the delta-light ones, RSMTRoad,
RSMTWater `+0x28`, RSMTAdditive `+0x2c`, RSMTStaticMeshAlphaBlended
`+0x3c`, ...). A drawable goes to one by `0x1003e530(item, transparent,
firstPerson, delta)`: index (delta × 2 | firstPerson) × 2 | transparent,
0 meaning the material's own opaque queue. The bundled mesh's draw
(`0x100c3880`) sets a material by its alpha mode (`+0x15c`): 1 is
blended (AlphaBlendEnable, no depth writes, CullMode 1, alpha test above
0, SrcBlend SRCALPHA / DestBlend INVSRCALPHA, RaShaderBM's
`outColor.a = diffuse.a × Transparency.a`) into a transparency queue,
unless its geometry template has `hasAdditiveAlpha` (`+0x268`): then
DestBlend ONE, no alpha test, the BMadditive shader (`RaShaderBMadditive`
technique `depthAndFog`: the texture unlit, its alpha × the transparency,
fogged, its coordinates scrolled by `frac(GlobalTime ×
simpleUVTranslation)`, template `+0x26c`) in RSMTAdditive, or in first
person in RSMT1Person with the opaque ones. An object fading out (its
transparency `+0x1e8` below 1) sends all its materials to the
transparency queue. The transparency queues sort (mode 2 at `+0xc`;
`0x10038120` → `0x10039380`, comparator `0x10038340`) by the camera's
distance to the object's origin (`+0x238`, set at the start of
`0x100c3880`), largest first. The frame (`0x1002cef0`) draws the opaque
queues, RSMTStaticMeshAlphaBlended, the water, RSMTTransparency,
RSMTAdditive and the particles; after the first-person depth clear,
RSMT1Person, the first-person particles, then RSMT1PersonTransparency.
Rewritten in `src/Render/MeshQueues.h` and `LevelRenderer::draw` /
`drawFirstPerson` (GpuContext's `fadedMeshPipeline` serves both blended
kinds, `additiveMeshPipeline` the additive one); the additive materials
of the first-person pass are drawn after all its opaque ones.

**Attached effects.** An EffectBundle (vtable `0x0097b658`) is a child
of its object and follows it; its events (`0x0061a4e0`): 0 starts it when
startOnEffects is 0, 4 (sent to a vehicle when a player enters it, Player
`0x00593e30`) starts it with bit `0x20` (the exhaust's startup), 5 (the
PlayerControlObject's exit, `0x005cea00`) stops it with bit `0x40`, `0x2c
+ N` (animation effects) with bit 9 + N; `startAtCreation 1` starts it
with the object (the engines' flames, rocket trails). Its particle
systems' `isLocalSystem 1` keeps their particles in their own space (they
move with it), the others leave theirs in the world (exhaust smoke,
trails); `showInFirstPerson` / `showInThirdPerson` pick the view.
Rewritten in `src/Effects` (`EffectManager::spawnAttached`, `setAnchor`,
`setMount`, `stopAttached`; `ObjectEffects` for the world's objects,
driven by the main loop with their parts' poses; the bundles' Sound
children are handed out by `takeSounds`). The 3P muzzle flash is the game
world's (`GameWorld::fire`): FireComp places the muzzle bundles by the
weapon's matrix times their own (`0x005f25f0` → `0x006acfa0`), so it is
spawned against the weapon object's anchor at the e_muzz child's place
(`GameWorld::weaponPlace`: the soldier's skeleton's mesh1 node, turned by
the soldier's yaw), with the systems shown in third person, and the anchor
follows the gun while it burns (a second; `followSoldier`). Without a posed
skeleton it is spawned at the eyes along the aim.

## Distant terrain, water, LODs and effects (RendDX9)

**The surrounding terrain.** Heightdata.con adds eight secondary
heightmaps around the primary (`heightmapcluster.setClusterSize 3`,
257 × 257 8-bit samples 4 m apart, cluster positions −1..1). They are drawn
as TerrainShader_Shared.fx's `Shared_SurroundingTerrain` (`Shared_PS_STNormal`):
the colormap `<colormap base>_s<i>.dds`, which holds the light already, times
the low-detail texture weighted by the low-detail component map
`<low-detail base>_s<i>.dds` (r: the flat part, the texture's b channel on
the y plane; b: the mountain part, the planes' r/g channels blended by the
normal), `color = lerp(0.5, Y.b, c.r) × lerp(0.5, mountain, c.b) ×
colormap × 4`. RendDX9 names them with `"%s%s_s%i"` (`0x10154110`); i counts
the eight row by row from the −x/−z corner (Heightdata.con's order; the
colormaps' rivers and coasts continue across the seams in that order). The
low-detail texture is `<level>lowdetailtexturehi.dds`, else
`lowDetailTexture.dds`, else `common/terrain/textures/Default.dds`
(`0x100b0850`, `0x100ccab0`), tiled by terraindata.raw's far tilings. The
primary patches use it too (TerrainShader_Hi.fx `Hi_PS_FullDetail`):
`low = lerp(0.5, Y.b, c.r × (0.5 + 0.5 f)) × 4 × lerp(0.5, mountain, c.b)`,
`detail = lerp(2 × detail × low, low, f)` with f the fade to the next LOD;
their component maps are absent, so c = (1, 0, 0) (the patch's default fill,
0xff0000). Rewritten in `LevelRenderer::buildSurroundingTerrain`,
`surrounding.frag` and `terrain.frag`; the secondary meshes take every
other sample. The planes' coordinates (0x100af7e0): on the primary
TEXSCALE = (255 / patch size, -0.00615148, 255 / patch size) over the
vertices' grid bytes, so the low-detail texture repeats farTopTiling times
a **patch** across (not the terrain) and its x / z planes run -0.00615148 a
metre up; on the surroundings STTEXSCALE = (1 / (patch size x secondary x
scale), secondary y scale x 400 x 1.53787e-05 a height unit, ...) over
world metres, and STFARTEXTILING = (farSideTiling.x x patches per side /
(secondary x scale / 4), farSideTiling.y, the top tiling likewise, the y
offset). Both take the same top tiling: terraindata.raw's first one
(terrain +0x2c0) unless +0x2ee picks the second (+0x2bc; set with the
Low_Terrain fallback). vBlendMod is the effect's default (0.2, 0.5, 0.2):
RendDX9 never looks up BLENDMOD.

**The compiled terrain.** terraindata.raw is decoded to its end
(`terrain::parseTerrainData`, `TerrainData::meshes`; the layout is in
`src/Terrain/TerrainData.h`): the LOD index tree all patches share
(`0x100b00a0`: 85 quadtree nodes with three LODs, nodes 0-20 with nine
stitch variants at LODs 1 and 2), each patch's compiled vertices (height,
three geomorph deltas, packed normal; `0x100aed30` → `0x1014be90`) and
detail-material index lists, the surrounding terrains' heights and
normals (`0x100aedb0` → `0x101506d0`, their counts from the meshes
`0x100b3fe0` generates) and the culling quadtree (`0x1009da80`). The
vertices' heights are the heightmap's samples (row z, column x, patches
row-major from the −x/−z corner); a vertex's first byte is the LOD at
which a cell edge drops it (1 on the 32-quad cell lines at odd
positions, 2 at positions 2 mod 4, else 0), the deltas are h − the next
LOD's height there (the third always 0).

**Terrain LODs** (`src/Render/TerrainLod.h`, `LevelRenderer::
buildTerrainLods` / `updateTerrainCells`). The terrain culler (object at
terrain +0x34c, constructor `0x1009d5a0`, `0x1009c820`): lodSwitchDistance
100 (+0x18, taken between baseCellSize and 1000, `0x1009c9b0`),
lod0SwitchStart / Stop 0.4 / 0.9, lod1SwitchStart / Stop 2.0 / 2.7, lod2Start
3.0 (+0x20..+0x30), baseCellSize 64 m (+0x14; a power of two 16..512 no
bigger than a patch, `0x1009cf40`), cullerType 3, version ≥ 26 turning
stitching on (+0x78, `0x1009da80`). Each frame (`0x100a07a0`) s =
max(round(zoom × lodSwitchDistance × 0.75), baseCellSize), the zoom being
RenderView +0x1c = tan(first fov / 2) / tan(fov / 2) (`0x10066b30`); the
culler walks its quadtree (`0x1009f210`) down to the visible cells of
baseCellSize and gives each LOD 0 when the horizontal distance from the
camera to its box plus the camera's height above the ground (terrain
+0x294, `0x100af7e0`) is at most s, LOD 1 up to s × lod2Start, else LOD 2.
Then (`0x1009cfa0`) a LOD 2 cell with a LOD 0 cell among its eight
neighbours becomes LOD 1, and a cell's stitch mask keeps a visible
neighbour's vertices one LOD finer on that edge: bit 1 its −z edge, else 2
its +z edge, 4 its −x edge, else 8 its +x edge. A cell is drawn as its
tree node's range [LOD][mask] (`0x1014d700`). The vertex shader
(TerrainShader_Shared.fx geoMorphPosition) moves y by
`dot(selector, delta) × interp + dot(adder[edge byte], delta)`, interp =
saturate(d² × x − y) with d² from the vertex to (camera x, height above
ground, camera z); per LOD (`0x100af7e0`, squared since `0x101f62e8` = 1):
x = 1 / (stop − start)², y = start² / (stop − start)² with start / stop the
LOD's switch values × s (none at LOD 2); MORPHDELTASELECTOR the LOD's
delta (× the y scale); MORPHDELTAADDER (0, 0, s) for every edge byte but
(s, 0, 0) for byte 1 at LOD 1 and (0, s, 0) for byte 2 at LOD 2 — the
stitched edge's extra vertices sit fully on the coarser line. With the
defaults LOD 0 is fully morphed from 48 m and ends at 75 m, LOD 1 morphs
from 150 to 158.9 m and ends at 225 m: no pops. One crack is left by the
original: a LOD 0 cell reaches 75 + 64√2 m away and its LOD 1
neighbours start morphing towards LOD 2 at 150 m, so with a low camera
their shared edge parts by up to a decimetre; the rewrite also morphs a
LOD 0 cell's vertices on towards LOD 2 with LOD 1's limits (a deliberate
deviation). `TerrainLod_NoCracksOrPops` checks every shared edge of
Cerbere_Landing for eight cameras. The full-detail pass (Hi_PS_FullDetail)
fades its detail by the same interp; the farther cells are the low-detail
pass (Shared_PS_LowDetail), which also tints the ground under the sea:
lerp(colour, TerrainWaterColor (terraindata.raw's water colour,
terrain +0xc4), saturate((WaterHeight − y) / 3)) — here weighted by the
fade so it comes in with the LOD. Frame cost (release build, 1280 × 720,
100 waited frames): Camp_Gibraltar 5.1–5.3 → 4.6–4.7 ms, Cerbere_Landing
7.23 → 6.68 ms and 6.72 → 6.21 ms (the mesh LOD defaults below
changed in the same run). Not done: the per-material detail lists (the
rewrite draws all six layers in one pass), the cullerTypes other than 3,
detailCullMaxLevel.

**Water.** RaShaderWaterBase.fx's `Water` pixel shader (the PS_20 variant
with USE_FRESNEL and USE_SPECULAR, its 2D path): the normal is two frames of
`common/textures/waterbump/Waterbump_0N.dds` (world space, green up)
blended by WaterCycleTime, the reflection of the view off it looks up the
level's cube map `<level>Water/envmap.dds`, `color = specular ×
SpecularColor + lerp(WaterColor, env, 0.4)` with `specular = pow(saturate(
dot(towards the sun, R)), SpecularPower) × SpecularColor.a`, `alpha =
depth alpha × (1.5 − pow(dot(view, up), 30)) + alpha add`. The original's
water objects carry the depth alpha and the sun's shade in a lightmap; the
rewrite's grids (one per heightmap of the cluster, and four strips to the
horizon for the WaterSurrounding / WaterDistant variants) derive the depth
alpha from the sea's depth and use no shade. The variant goes by the
lighting quality (`0x100b0fe0`: 0-1 Water2D, 2 Water3D, 3-4
WaterHighEnd3D); the 3D ones (drawn here) take the normal from the volume
`Textures/watervolume.dds` (the terrain's water map name "watervolume",
`0x100b4690`; 256 × 256 × 8, R5G6B5) at xy = P.xz / (29.13, 31.81) +
WaterScroll × WaterCycleTime, z = WaterCycleTime × 10 + 0.7 x + 1.13 y,
with WaterCycleTime = frac(t / waterAnimSpeed) (`0x100b17d0`; a speed of 0
becomes 0.01, t the renderer's clock in seconds) and WaterScroll
renderer.waterScroll unscaled (defaults 10 and 0.1 / 0.1, `0x100b4690`);
the 2D ones run once through all the waterbump frames in 0.2 × speed
seconds, blending each into the next, at P.xz / (99.13, 71.81), unscrolled.
Cube maps are uploaded as SDL GPU cube textures
(`GpuContext::createTexture(texture, true)`), volumes as 3D textures.

**Culling and LODs.** The world pass culls instances against the view
frustum (`src/Render/Culling.h`): static meshes by the sphere of their
drawn LOD's vertices, bundled meshes by their parts' spheres moved with
the parts; skinned meshes are not culled. Terrain patches, surrounding
patches and water grids are culled by their boxes. A geometry's LOD is
picked by the camera's distance: `GeometryTemplate.setSubGeometryLodDistance
<geometry> <lod> <distance>` (RendDX9 `0x1007303a`) says how far each LOD is
drawn. The switch list a mesh gets (`0x100d4d30` static, `0x100a31d0`
bundled and skinned): entry i is the set distance, else a running value
that starts at 50 m and grows by 50 for each default and by the set value
for each set one (the editor's writer `0x100d3320` leaves out exactly 50,
100, 150 ...). The LOD (`0x100bdb00` static, `0x100c3390` bundled,
`0x101616c0` skinned) is one past the last entry whose value ×
renderer.global{Static,Bundle,Skinned}MeshLodDistanceScale (renderer
+0x9d8 / +0x9dc / +0x9e0, 1 by default; the shipped levels' Init.con set
1, a few 2) × the geometry quality (+0x974, 0.5 / 0.8 / 1; 1 here) is
below the camera's distance over the zoom less the mesh's radius
(`render::lodDistance`). Each LOD has its own lightmap in the atlas
(`=<lod, two digits>=` in its key); a LOD without one is lit as an
unlightmapped mesh. Skipped LODs: `GeometryTemplate.maxSkip3pLods` /
`maxSkipWreckLods` (0x10073920 / 0x10073a8a, template +0xc0 / +0xc4, both
1 by default, 0x10148e50) are capped by renderer +0x910 (1 at the low
geometry quality, else 0) and one less than the LODs; bundled meshes
(0x100a31d0: of three geometries 1 takes the first and 2 the second, of
two geometry 1) and skinned ones (0x100d4b..: the third person's,
geometry 0 of one and 1 of two) drop that many first LODs at load
(0x100a3b70 / 0x100d4160 skip their materials), the rest switching as a
mesh of that many (`GpuMesh::skippedLods`). Not done: static meshes'
(0x100dd480 also asks the template's +0xc8, unknown), the static meshes'
`ObjectTemplate.lodDistance` High/Medium/Low.

**Overgrowth.** Trees placed by `overgrowth/OvergrowthCollision.con`
(`Object.isOvergrowth 1`, `0x006ed5bb`) are drawn whole within
`Overgrowth.viewDistance × viewDistanceScale × closeDistPercentage`, beyond
with their `<geometry>_lod` mesh, whose `*_lod.dds` textures exist only in
the level's overgrowth atlases (`OvergrowthAtlas.tai`,
`OvergrowthAtlas_alphatest.tai`: texture → atlas, offset, size; the mesh
shader scales and offsets the coordinates). viewDistanceScale is
Overgrowth +0x110, which the geometry quality's switch writes too
(0x10010040: 0.4 / 0.7 / 1) and every level's Overgrowth.con sets to 1
after it: the rewrite takes the script's value, and the quality's once
the video settings change after the level loaded (the switch runs when
they are applied). Not done: dropping trees beyond the view distance,
the overgrowth shadow map.

**Mesh particles.** A MeshParticleSystem's particles are copies of its
`geometry` (a MeshParticleMesh template: a bundled-mesh file), scaled by
the size graph, turned by `randomRotation` + `rotationSpeed` (yaw / pitch /
roll), coloured by the colour graph and faded by the transparency graph
(MeshParticleMesh.fx `vsDiffuse`): blended and lit, or added
(`psAdditive`). Drawn before the sprites, in the world and the first-person
pass.

**Lights.** LightSource templates (class `0x94c7`; the fields as the
writer `0x00617030` lists them: lightType `+0x70`, isDynamic `+0x64`,
affectLightmappedObjects `+0x6d`, affectingType `+0x90`,
attenuationRange1 / 2 `+0xa0` / `+0xa4`, color `+0xa8`, coneAngle1 / 2
`+0xd0` / `+0xd4`, flicker `+0xe4` clamped to 0..1 by `0x00616cf0`) are
read by `effects::lightSourceOf`. Enabled Point and Spot lights are lights
of `color × intensity`, full up to attenuationRange1, gone at
attenuationRange2 (the original reads a radial attenuation table over
distance / range, LightManager_r3x0.fx; a linear ramp here), added to the
mesh and terrain lighting (`N·L`; the eight nearest the camera). They
come from two places:

- an effect bundle's LightSource children (explosions, muzzle flashes).
  The template has no lifetime of its own: none of its setters
  (`0x00528f5a`..`0x0052b6ea`) is one, so a light lives as its bundle
  does, which the rewrite takes as the bundle's longest particle system
  (emit delay + time + life), fading; attached looping bundles' lights
  live until stopped. Many shipped effect lights are `enabled 0`.
- the placed objects' template trees: the levels' light bundles
  (`ambient_small`, `baselight`, `houselight_big`, `BigSpot`,
  `Lamp_post_point`..., 52 on Highway_Tampa, 266 on Port_Bavaria) and
  lights carried by vehicles (`as_titan_specular`), placed with their
  objects each frame (`LevelRenderer::m_objectLights`). Static ones
  (`isDynamic 0`) are in the lightmaps already, so only unlightmapped
  surfaces (vehicles, soldiers, unlightmapped statics) take them; a
  dynamic one lights lightmapped surfaces only with
  `affectLightmappedObjects 1`.

Spot lights (`lightType Spot`, one shipped template) light within a cone
about their `direction` (default +z, turned with the object): full inside
coneAngle1, gone outside coneAngle2, both taken as half angles in degrees
with a smooth step between. Still a guess: the templates give degrees
(BigSpot 25 / 25, ZurichLight01 20), the shaders want a cosine-space
width c — SkinnedMesh.fx `saturate(cos − (1 − c)) / c`, LightGeom.fx
`saturate(cos² − (1 − c))`, LightManager a conical texture over 1 − cos²
— handed as ConeAngle from the light's slot 0x1c8 (`0x10060000` region,
the "Spotlight" technique); where the degrees become c wasn't found (no
degree-to-radian constant near the template's +0xd0 / +0xd4 in either
binary). Directional
lights (the sun) are left to the level lighting.

`Object.setLightSourceMask` (`0x006ed811`; 398 statics on Cerbere_Landing
set 6, 82 set 4) reaches the object's render object through `0x006f6f90`
(object `+0x44`, slot `0xe0`); for bundled, skinned and the other mesh
templates' slot 56 RendDX9 has the empty `0x100bc0b0`, static meshes store
it (`0x100e54d0`, `+0x2b0`). Nothing in RendDX9 reads that field but the
static mesh's slot 57 (`0x100e54e0`), and BF2142.exe calls that only
from the object's getter (slot 0x134 = `0x006f6fb0`, 7 without a render
object) for the console (`0x006ed952`); every other `+0x2b0` read in
RendDX9 is another class's. So the mask is inert in the shipped renderer
and the rewrite leaves it unread on purpose. Not done: projector
textures, specular, flicker, shadows, the per-mesh light choice
(`lightManager.maxLightsPer*Mesh`, `lightCullDistance`).

**Non-screen-aligned particles and trails.** A
NonScreenAlignedParticleSystem's particles are squares lying across the
system's y axis (its x and z axes; NonScreenAlignedParticles.fx displaces
each corner by world vectors the CPU gives): water ripples, ground rings.
A TrailSystem (Trail.fx) leaves a point at emitFrequency while its system
moves faster than `minTrailVelocity` and keeps doing so as long as an
anchor carries it; the points, joined in the order they were left, make
strips of 2 × size across in `numPlanes` planes about the path, each
vertex faded by the square of its plane's normal against the eye
(`fadeFactor`), the ends by `fadeInFactor` / `fadeOutFactor` of the life,
the texture tiled `textureTileFactor` times along it (wrapping) and slid by
`textureOffsetVelocity`. Both are built on the CPU as world-space
triangles (`EffectManager::forEachWorldGroup`) and drawn by `particle.vert`
with a negative size. Frames of an animated texture go by the particle's
age for these (`animationFrameCount`, `animationFrameCountX`, the frames'
relative size); the trail's own frame stays the first.

**Rain and snow.** `rainManager.*` (RendDX9 `0x1007e761`..`0x1008009a`;
Sky.con sets them; Cerbere_Landing snows) — `effects::Weather`:
particleCount particles in a box of emitRadius about the camera plus
offset, moving by particleVelocity × particleSpeed and wrapped round the
box (the original's 32 cells that follow the camera), swaying by
maxDeviation at angleSpeed degrees a second in 16 groups, faded from
fadeOutRange of the box to its edge (Rain.fx `vsPoint`: 1 −
|saturate((|Δ| − range) / (radius − range))|), and drawn added (SrcAlpha,
One) in particleColor: as points of `min(particleSize × sqrt(1 /
(scale.x + scale.y × distance)), particleMaxSize)` pixels of the game's
600-line screen with the level's texture, or with lineDisplay as lines
lineLength long along the fall. All particleCount particles are kept:
RendDX9 sizes its buffer by it (`0x1011d5b0`; Aegis Station's 80,000 cost
the rewrite about 4 ms a frame). The original moves 6 × 6 × 6 = 216 cells
of 0.4 × emitRadius (`0x1011c810`) by the velocity (`0x1011b2a0`), wraps
the cells (not the particles) round the box (`0x1011b720`) and draws them
32 at a time (CELLPOSITIONS, CELLVISIBILITY); the sway is 16 deviation
groups (DEVIATIONGROUPS) walked by randomness / maxRandomChange /
randomDeviationAngleSpeed, not a sine. The rewrite's per-particle wrap,
sine sway and line width (a pixel) remain its own.

**Decals.** A material cell's decal template (`MaterialManager.
setDecalTemplate`; `decal` templates: `decalTextureName`, `size` +
`randomSize`, `rotation` + `randomRotation` about the normal, `color`, one
frame of an `animationFrameCount`-frame atlas, `animationFrameCountX`
across) is left where a projectile hits the terrain, a static object or a
vehicle, as Decals.fx's `psDecal` draws it: the texture × colour ×
(ambient + sun N·L), faded out from 100 m over 30 m
(`decalFadeDistanceAndInterval`). The newest 256 stay. A decal on a
vehicle is kept in its space and drawn where it is each frame: BF2142.exe's
object constructor `0x0063c000` asks DecalManager (slot `0x14`, RendDX9
`0x100e6b70`: a set of 0x40 bytes by `0x101647d0`) for a decal set of the
object's own when its template's byte `+0xf1` has bit 4 and not `0x18`,
keeps it at `+0x164` and releases it with the object (`0x0063c210`, slot
`0x20`). The bits are the template's physics flags (their console
accessors `0x0051d5c8`..`0x0051d9ed`): 4 hasCollisionPhysics, 8
hasPointPhysics, 0x10 hasRotationalPointPhysics, 0x20
hasResponsePhysics; DynamicBundle (vehicles) sets 4 by default
(`0x0060fea0`). So every object with collision physics and neither point
physics gets its own set; the rewrite's rule (every body a decal lands
on) is that for the bodies a projectile can hit. Rewritten in
`EffectManager::spawnDecal`, `decal.vert/.frag`; not done: the
normal-mapped technique, clipping decals to the geometry
(`decalGeomClipAngle`).

**Turning parts.** A vehicle's moving parts (RotationalBundle, Engine,
Wing: rotors, thrusters, flaps, turrets and the like) are drawn turned by
their axes' angles (degrees, in their own frame, before their placement:
`turn × base × parent`, as `Vehicle::updateParts` composes them), which
`GameWorld::partPose` hands the renderer, the first-person cockpit and
the attached effects; seat guns' yaw and pitch bundles, seat cameras and
wheels keep their own poses.

**Dropped kits.** A kit lying on the ground draws its template's
`geometry.dropGeom` geometry of its mesh (`EU_Kits` / `Pac_kits`: 8 to
11; the worn kit is `geometry.kit`, hidden here), through the renderer's
`geometryFor`.

## Particle systems, wind and the quality switches (RendDX9)

`src/Effects/Effects.cpp` (the emitters, ObjectEffects), `src/Scene/
EffectSettings.h` (the manager, the wind, VideoSettings), the renderer's
`buildParticles` / `buildShimmer` / `particleLight`.

**The particle system manager** (`dice.hfe.geom.ParticleSystemManager`,
`DAT_101ffc70`, constructed by `0x1009a8c0`, vtable `0x101d3c78`; its
console names `0x1007aa09`..`0x1007bcb7`, all 20 registered). Slot 0x1c
(`0x1009a120`) makes a system template by kind: 0 sprite (vtable
`0x101d8c48`, constructor `0x10139910`, defaults `0x10138b40`: 32 a
second, 4 s, gravity −9.82, LodDistance and emitPrio High, QualityLevel
Low, minimum emit scale 0.05), 1 mesh, 2, 3, 4 (the trail and
non-screen-aligned ones). BF2142.exe's templates hand every property to
it (`0x00625e40`..`0x00626cf0`: slot calls on `+0xa4`); the sprite
template's offsets: emitFrequency `+0x10`, preWarmTime `+0xc`, emitPrio /
LodDistance / QualityLevel `+0x60` / `+0x64` / `+0x68` (Low, Medium, High
at BF2142.exe `0x00a04284`), ttl `+0x88`, gravity `+0x90`, airResistance
`+0x94`, windAffectionFactor `+0xa8`, forceToWaterSurface `+0xac`,
waterSurfaceOffset `+0xb0`, collisionEnable `+0xf0`, rotationGraph
`+0x100`, gravityGraph `+0x110`, airResistanceGraph `+0x120`,
TemplateParameters `+0x140` (`0x70` bytes: uv range, lightMapIntensity
offset `+0x148`, max size `+0x14c`; light colour and random intensity;
colour 1 and the light factor `+0x16c`; colour 2; the blend,
transparency and size graphs), randomSize `+0x1b8`, randomIntensity
`+0x1bc`, randomRotation `+0x1c0`, emitTime / emitDelay `+0x1d8` /
`+0x1dc`.

**An emitter** (the sprite instance, vtable `0x101d9170`, constructor
`0x1013f6a0`):

- *start* (slot 0xc, `0x1013fb30`): nothing when the template's
  QualityLevel is above the manager's `qualityLevel` (`+0x8cb4`); emitDelay,
  emitTime and preWarmTime are taken; a system beyond the manager's view
  distance of the camera doesn't emit (`+0x128`), nor one unseen for 60
  frames (`0x101403b0`: the frame of its last draw `+0x124`; the rewrite
  checks the distance only).
- *update* (slot 0xa0, `0x101407c0`): the time is scaled by the
  instance's `+0x12c` (the bundle's age scale) and cut to the longest
  life; the particles move in steps of 0.01 s (`DAT_101f7030`, the
  remainder carried); the delay runs down (the particles still move);
  then the pre-warm (`+0xf8`) is run at once in steps of 1/30 s; then
  the emission (`0x101403b0`).
- *emission*: a particle every 1 / (emitFrequency × `+0x134` × `+0x138`)
  seconds, `+0x134` the distance LOD (`0x101393e0`, each frame:
  (1 − f)² + the system's box on screen, at most 1 and at least the
  template's `+0xc8`; f = saturate((d − p·end) / ((1 − p) end)), end the
  LodDistance class's distance, p = startFadePercentageOfViewDistance),
  `+0x138` the bundle's emit scale; once the sprite pool (`+0x5f00`, the
  row's 1000) is more than `+0x8c0c` (0.75) used the frequency is scaled
  by emitPrio's `+0x8c00`.. (0.4, 0.55, 0.7); emitTime runs down by the
  interval (a looping system starts again, slot 0xc); each particle is
  born at the system's place lerped between its last two (`0x100c6b90`)
  and moved on for the rest of the step; at most (ttl + random ttl) ×
  frequency + 10 alive (`0x1013f6a0`), none when the pool is empty.
- *a birth* (`0x1013fdc0`): life ttl + u × random ttl; one random s in
  −1..1 gives the size offset s × randomSize and the speed (emitSpeed −
  s × randomSpeed) × the bundle's speed scale (`+0x13c`) × emitSpeedGraph
  over the emit time; the intensity the system's (`+0x160`, 1) and the
  random intensity randomIntensity × (−1..1) (vsParticle: `(colour × i +
  ri) / 2`, doubled by the pixel shader); its alpha factor the bundle's
  alpha intensity (`+0x164`); rotation randomRotation × (−1..1), its speed
  rotationSpeed plus |randomRotationSpeed × r| with r's sign; a world
  system's particles take the system's velocity.
- *motion* (`0x1013f0c0`): per step, w the wind at the particle
  (WindManager slot 0x3c) × windAffectionFactor (world systems,
  `affectedByWind`), r = v − w, dv = −k |r| r + y × gravity ×
  clamp(gravityGraph, 0, 1) × dt with k = airResistance ×
  clamp(airResistanceGraph, 0, 1) × dt, x += (v + dv / 2) dt, v += dv;
  speeds under 0.001 zeroed; with collisionEnable (+0xf0) the particle
  bounces off the ground (the rewrite: the terrain under it taken flat,
  the speed across it reversed times collisionSpeedAlongNormal, along it
  times collisionSpeedAlongPlane; the original's system ground planes
  aren't decoded, nor the two speeds' defaults: 0.7 / 0.8, the commonest
  shipped values); forceToWaterSurface puts it
  on the water (the manager's water, `+0x8c18` slot 0x94; the rewrite
  the sea level plus waterSurfaceOffset); its rotation turns by its speed
  × clamp(rotationGraph(age), 0, 1) (`0x10131f80`).
- *the draw* (`0x10132e90` and the like): a particle's alpha times (1 −
  min((d − s) / (end − s), 1))² past s = startLodPercentageOfLodDistance
  (0.85) of its class's end, d its depth in the view; a particle whose
  alpha is below the template's alphaCull (+0xa0, the exe's slot 0x154,
  `0x00539094` → `0x00626c50`) isn't drawn (the collector `0x10130bd0`).
  pivotOffset (+0x1b0 / +0x1b4, slot 0x1c8) moves the quad's centre off
  the particle by that many half-sizes (`particle.vert`; 0/−0.5 stands
  it on it). drawOrder Back / Front is applied by its name (Back draws the
  newest particles under the older; where RendDX9 reads it wasn't
  found). selfShadowIntensity (+0xa4, slot 0x1d0) is stored by the
  template, but no reader of it in the sprite code was found: not drawn.

**Heat shimmer.** particleShaderType 1 (41 shipped systems: the fires'
and the explosions' `*_Heatshimmer`, engines) picks ParticleHeatShimmer
(`0x1012ed89`; the sprite manager's ShimmerIntensity 0.75, ShimmerPhases
20, its speed 10: `+0x1dcc`..`+0x1dd4`, `0x10131000`): the back buffer
behind the particle displaced by `cos(v × 2π phases + a), sin(u × 2π
phases + a)` × the intensity in texels, a = (FracTime + offset) × 2π,
FracTime = frac(t × speed) (`0x10132de0`), blended by the texture's alpha
× the particle's. The rewrite ends the world pass, copies the scene and
draws them over it (`shimmer.frag`, `LevelRenderer::drawShimmer`).

**The wind** (BF2142.exe's WindManager, `0x00704400`, vtable
`0x0099ad60`; `windManager.globalWindSpeed` / `globalWindDirection`,
`0x006ebec1` / `0x006ebfc8`): slot 0x3c (`0x00703b20`) is the global wind
plus the WindAffector components whose radius reaches the point (slot
0x18, `0x007040d0`: |p − pos| < falloff + the query's radius), slot 0x30
its length. A WindAffector (template component 0x127bbe7, vtable
`0x0097b1a0`; its instance `0x0064b8c0`, vtable `0x009874b8`, registered
by `0x0064b490`, counting windBlowTime down by `0x0064b4c0`) gives
direction × strength (slot 0x50 → 0x4c, `0x0064b5f0`): Directional
(0) the template's windDirection turned by the object's matrix,
strength (1 − min(d² / falloff², 1)) × windSpeed; Point (1) away from
it, the same strength; RadialDir (2) along the turned direction D,
strength (1 − min(a / falloff, 1)) × speed with a = (p − pos)·D,
nothing beyond windRadialFalloff from the axis, the direction bent out
from it by min(r² / rf² + 0.2, 1); TurbulenceField (3) none. Shipped on
the gunships' and transports' rotor Engines (RadialDir 0/0/−1, 20, 20,
10) and some explosions (Point). The rewrite collects them from the
world objects' template trees (`ObjectEffects::localWinds`, placed with
their parts each tick) into `scene::WindSettings::localWinds`;
`windAt(point)` sums them (the explosion effects' own affectors are not
collected). RendDX9 steps it each
frame (`0x10028bb0`). Its readers: the particles (above), and static
meshes' leaves: RendDX9 marks a geometry whose path has "vegitation" a
tree (`0x100de8xx`, `+0x2a2`) and draws its leaves with RaShaderLeaf
(shader key `0xf0000000`, `0x100bdb00`), whose WindSpeed (per object,
`+0x208` = WindManager slot 0x30 at the object) sways them:
`pos += sin((GlobalTime / (ObjRadius + y)) × (WindSpeed + 5)) × (ObjRadius
+ y)² / 1024` (not the overgrowth's: OVERGROWTH has no sway). The rewrite
takes the alpha-tested materials of such meshes as the leaves
(`mesh.vert`, TransformUniforms `sway`), the speed at each tree's origin
(so a helicopter's downwash stirs the trees under it).

**Wheel and draft bundles.** WheelEffectBundle (`0x0061bfc0`) and
AirDraftEffectBundle (`0x0061d820`) take their effect from
MaterialManager's cell of their own `material` (67 wheeled, 61 tracked,
107/108 the smoke, 128 anti-grav, 62 the draft) against the ground's
(slot 0x68, `0x0061d510`: e.g. 62 × 1 `e_ade_genericdraft_water`). The
wheel's is on while its wheel touches (its ground material `+0x188`, set
by the vehicle), at the contact; the draft's casts a ray along its −y,
startDistance + 5 long, at the terrain, the water and the objects, and
is on while it hits closer than startDistance, at the hit, its engine
running (`engineName`'s, `+0x24c`). Each tick (`0x0061bd20`,
`0x0061ce20`) the modifiers — alpha intensity (`+0x164`), emit scale
(`+0x138`), emit speed scale (`+0x13c`), age scale (`+0x12c`), start age
(`+0xd8`) and the sound's intensity — are min + (max − min) × the
product of the mappings set (a/b: 0 at a, 1 at b): FromSpeed the
vehicle's speed at the bundle (slot 0xac), FromSpeedInDof its forward
part (at least 0; its sign turns emitDirectionZFromSpeedDof's emit
direction), FromAltitude the ray's length, FromEngine the throttle. The
rewrite (ObjectEffects::updateGround) probes with the game's collision
(`setGround`: the water, else `groundMaterialAlong`, the point on the
terrain under it), a wheel 1.5 m straight down from its bundle (no
contact list), takes the velocity from the object's moves, the throttle
as 1 when occupied, and skips parked, empty vehicles. With the vehicle's
wheel list (`ObjectState::wheels`, from `game::Vehicle::wheels()`: the
spring's name, its contact and the point under it) a wheel bundle under
a wheel's spring is on only while that wheel touches, at its contact
point, the material probed there; FromEngine takes |PIThrottle| of an
occupied vehicle.

**Cold breath.** `GameLogic.useColdBreath` (`0x00427d38`, the game
logic's slots 0xd8 / 0xdc; 30 levels set it) and the Soldier's
`breathEffect` (`+0x3a0`, only the booster pack's four soldiers:
`e_L_breath`), created with the soldier (`0x00588xxx`) and turned on
every few seconds (`0x00588830`: slot 0x100(0) on, (1) off) while
useColdBreath is on, the kit hasn't disableBreathEffect (`+0x384`), and
it isn't the local player's own soldier in first person. The rewrite
starts it at the head (0, 1.6, 0.15), with the kit's flag (the player's
kit template's disableBreathEffect: two shipped kits) and the soldier in
a vehicle (`+0x264`: the player's vehicle) checked.

**The quality switches** (RendDX9's settings object, vtable `0x101bfd48`,
slot 0x38 `0x10010040`, run at the renderer's start `0x10029e20` and
when the video settings are applied): `scene::QualitySettings`, set by
`VideoSettings.set*Quality` (the Video page; SettingsObjects keeps
Video.con). Applied as the original: terrain 1/2/3 — the terrain
culler's lodSwitchDistance 64/80/100 (unless a level's script set it
after: two shipped levels), undergrowth.viewDistanceScale 0.5/0.75/1;
geometry — the mesh LOD factor (renderer `+0x974`) 0.5/0.8/1; dynamic
lighting 0 — no dynamic lights (the light manager's slot 0xf4); effects
0..4 — the particle manager's row (`DAT_101f45a8` + 0x3c × level,
`0x100991b0`: view distance 500 × scale × 0/0.5/0.75/1/1 (at least 200),
pools 0/300/500/1000/1500, the fade starts 0/0.2/0.3/0.4/0.85, the
shader quality 1/1/3/3/3, the quality level 0/0/1/2/2), and below 2 no
decal sets on moving objects (DecalManager `+0x322e`, `0x100e6b70`);
dynamic shadows as before (`ShadowSettings::quality`). Also applied:
undergrowth.simpleShaderEnable (1/1/0: Undergrowth.fx's `_simple`
techniques, the terrain colour and lightmap read per vertex,
`undergrowth.vert`'s vertex samplers), maxSkip3pLods (renderer +0x910,
above), the Overgrowth's viewDistanceScale (above), the lighting level
and the texture classes (below). Stored only: renderer `+0x693`, the
road renderer's and the sky dome's levels, `+0x6a8`..`+0x6b0`,
`DAT_101fa9ac +0x40` and the filtering.

*Texture classes.* The texture manager (`DAT_101f8e5c`, constructor
`0x10017ca0`) has 15 classes at +0x5c + 8 × class: a "skips" byte (all on
but class 1's, +0x64) and the levels skipped (+0x60). A loader names its
class (`0x10017240` sets +0xd4) before loading (`0x10018650` →
`0x10017f20`), and `0x10017710` drops that many top mips of a 2D texture
while the next stays at least 4 texels (+0x1c). The switch gives every
skipping class GSSkipMipLevels (2/1/0) and classes 2, 6, 10, 11
clamp(skip − 1, 0, 1). The classes by their callers (`push n; call
0x10017240`): 1 the menu atlases (`0x10187ca0`, `0x10188330`), 3 / 5 / 9
a terrain patch's colormap / lightmap / second-name map (`0x1014be90`:
terrain +0x54, +0xa8, +0x70), 4 the low-detail texture (`0x100b0850`), 7
the detail layers (`0x100b2af0`), 8 the numbered patch maps
(`0x1014b6a0`), 10 the undergrowth (`0x101100d0`), 11 the overgrowth
atlases (`0x1008c7e0`) and trees' materials (`0x10151830` with +0x2a2),
12 static (`0x100dea90`), 13 bundled (`0x100a3b70`), 14 skinned meshes
(`0x100d4160`); 0 the rest; 2 and 6 have no caller. Rewritten as
`scene::TextureClass`, `QualitySettings::skipMipLevels(class)`,
`texture::skipTopLevels` and `LevelRenderer::texture`'s class.

*The lighting level* (renderer +0x6b4: 3/1/0 for lighting 1/2/3) is the
RaShader permutations' path: each `.mfx`'s DeprecationList drops its rows
below it (`0x10148ae0`; the static meshes' table at `0x10210924`, built
from RaShaderSTM.mfx by `0x100db190`). At medium bundled and skinned meshes
lose their normal maps (the loaders skip "_b.dds", `0x101499b0`, and
"_b.dds" / "_b_os.dds", `0x100d4160`); at low also their hemi map,
envmap, dynamic shadows and colormap gloss, and static meshes their crack,
dirt and detail normal maps (`0x10151830` skips "_cr", "_crb", "_di",
"_deb"). `QualitySettings::lightingLevel`, `meshNormalMaps`,
`meshHemiAndShadows`, `staticMeshDirtAndCrack`.

**The interior sun.** A seat camera's inside view with
`allowInsideStaticSun` (BF2142.exe `0x006104d0`..`0x00610580`,
`PlayerCamera::insideSun`) lights the first-person pass (the cockpit)
by its insideStaticSunDirection, Color (flickering to the damage colour
below insideDamageThreshold) and AmbientColor instead of the level's,
the effects' lights only with allowInsideDynamicLights
(`LevelSources::insideSun`).

## Shadows, hemi maps, sky and post-production (RendDX9)

`src/Render/LevelRendererPost.cpp`, `src/Render/PostProduction.h`, the
settings in `src/Scene/PostProductionSettings.h`; shaders `shadow.glsl`,
`shadowcaster.frag`, `sky.frag`, `bloom.frag`, `post.frag`, `camo.frag`,
`fullscreen.vert`. A frame is now: the shadow maps, the world pass into a
scene texture, the camouflage mask, the first-person pass, the bloom
chain, then `post.frag` into the target with the HUD and the menus over
it.

**Dynamic shadows.** The settings live in the light manager (class
`dice.hfe.geom.LightManager` `0xd01c1`, created by `0x10070472`,
constructed by `0x100aafe0`, which sets the defaults; its vtable
`0x101d42e0` slots 41 to 94 are the `shadowManager.*` setters and getters
the console's `0x100802eb`..`0x100821b3` call, e.g. `0x1014a580` the map
size, which keeps the old one when the device refuses it, `0x1014a5d0` the
object size, at most the map's, `0x1014a600` the bit depth as
D3DFMT_D16/D24S8/D32, `0x1014a840` the alpha threshold that it uploads as
SHADOWALPHATHRESHOLD). Defaults: map 1024, objects 512, depth distance 40,
dynamic, static-mesh and first-person shadows on, alpha shadows off
(threshold 0.2), update factor 1, the PSM/TSM tuning (fov 60, TSM focus
0.52, 80 % line -0.6). The shipped scripts never call `shadowManager.*`;
the menu's `VideoSettings.DynamicShadowsQuality` (0 off .. 3) picks the
quality. The receivers' shaders are RaCommon.fx's `calcShadowProjection`
and `getShadowFactorExactOther` (four taps half a texel apart) behind the
`_HASSHADOW_` techniques (the `*Shadowed.fx` variants), the occluders'
map (`ShadowOccluderMap`) multiplied in for bundled meshes, and
LightManager_r3x0.fx's `saturate(lightmap.a + ShadowIntensityBias)`.

The rewrite keeps two orthographic maps along the sun over a square region
(half side `shadowMapDepthDistance` × 1, 1.5 or 2 by quality; the map's
size doubled at quality 3), centred half a region ahead of the camera and
snapped to a quarter of the region's side so the projection holds still between
moves: the dynamic objects' map (bundled and skinned meshes, including
the first person's own soldier when `dynamic1pShadowsEnabled`) is redrawn
each frame, the static meshes' (with `shadowMapIncludeStaticMeshes` and
`dynamicStaticMeshShadowsEnabled`) only when the region moves or objects
come and go. Terrain, roads, undergrowth (`undergrowth.dynamicShadowsEnable`,
`0x100861d8`) and lightmapped meshes take only the dynamic map (their
lightmaps hold the statics' shadows); unlightmapped meshes (soldiers,
vehicles, unlit statics, the first-person models) take both. Blended and
additive materials cast nothing; alpha-tested ones only with
`allowAlphaShadows`. The comparison is a filtered depth compare at four
taps, faded out over the region's last fifth. `BF2142_SHADOW_DEBUG=1` shows
the terrain's shadow factor, `BF2142_SHADOW_STATS=1` prints the casters,
`BF2142_SHADOW_REDRAW=1` redraws the static map every frame (its cost).
`shadowManager.shadowUpdatesEnabled 0` (LM `+0x755`, `0x1014a900`;
the shadow pass `0x10127420` runs only with it set) stops the maps'
redraws: they keep what they hold, with their projection.
`GeometryTemplate.useRadiusForShadowDepth` / `useRadiusForShadowSpatial`
(`0x100a7e60`..`0x100a7e90`, the bundled template's `+0x204` / `+0x205`;
the heavy mechs set both) make the light manager fit a caster's depth
range (`0x10125780` → `0x1012b190`) and light-space bounds (`0x10123e70`)
to the box `pos ± radius·√2` instead of the mesh's oriented box, steady
while the legs move; the rewrite's region test takes that wider bound
for them. Settled as unused by RendDX9: `shadowUpdateFactor` (`+0x750`)
and `dropShadowClipHeightOffset` (`+0x1d8`) have no reader but their
getters; `lightmapIntensityBias` (`+0x1d4`, uploaded at once by
`0x1014a780` as LIGHTMAPINTENSITYBIAS) is read only by
LightManager_r3x0.fx's Combine technique, which nothing selects. Not
done: RendDX9's light-space perspective (LSPSM/TSM) fit and its
per-object maps (`shadowMapObjectSize`). A view-fitted perspective map
moves with every turn of the camera, so the static casters' map could no
longer be kept between moves of the region (its redraw costs up to ~1 ms
a frame, see above), and per-object maps add a pass per caster; both
would raise the frame cost well beyond the current 0.2-0.4 ms, so they
are left out on purpose.

**Hemi maps.** `HemiMapManager.setBaseHemiMap <texture> <corner> <size>
[<height>]` (`0x100857b8`; every level's Sky.con: `groundhemi`, 0/0/0,
2048, 500) names a DXT5 map over the level: rgb the ground's bounced
colour, alpha the sun's occlusion (roofs, bridges, the buildings'
shade; Camp Gibraltar's is 97 % at 255). RaShaderBM.fx / RaShaderSM.fx
`calcGroundUVAndLerp` look it up under the point a metre along its normal,
`uv = ((p + size/2 + n).xz - corner.xz) / size`, v flipped, and lerp the
ambient from that ground colour to `HemiMapSkyColor` (the sky colour) by
`clamp((n.y + offset) / 2 + 0.5, 0, 0.9)`, offset = `clamp(localHeight × 2 −
1 + HeightOverTerrain, −2(1 − HeightOverTerrain), 0.8)` with the point's
height over its object's origin at InvHemiHeightScale (0.5 for skinned
meshes); `_HASHEMIOCCLUSION_` multiplies the sun term by the map's alpha.
The renderer uploads HemiMapConstants (corner, size, lerp bias) and
HemiMapSkyColor with its per-frame constants (`0x10030000` table: 0x110,
0x124; TreeSkyColor 0x138). The rewrite applies it to every unlightmapped
mesh in mesh.frag. Per object (the block `0x10041100` builds:
HeightOverTerrain `+0x1e4`, HemiMapConstantColor `+0x1ec`; only RaShaderBM
and RaShaderSM read them):

- *HeightOverTerrain* (BM `0x100c2b70`, SM `0x10160e90`): a ray from the
  object's origin 4 radii down, `h` the height over what it meets (4 r
  if nothing), `max(h − r, 0) / max(r, 2)`; it lifts the lerp's offset,
  `clamp(localHeight × 2 − 1 + HOT, −2(1 − HOT), 0.8)`. The rewrite's
  ray meets only the terrain.
- *HemiMapConstantColor* multiplies the hemi colour: 1 with the hemi
  technique outside the Titans. With the camera in a Titan (the game's
  test, `0x006fa8b0`: the Titan the camera's soldier is in) bundled and
  skinned meshes take the interior path (`0x100c3880`, `0x10162070`):
  HeightOverTerrain 0, and the colour from the Titan maps.
  `HemiMapManager.loadTitanHemiMap <texture> <offset> <size> <which>`
  (`0x1008560d` → `0x100edf90`; 14 calls in the Titans' playtest and
  wake files, `Common/Textures/eu_titan_interior_hemimap_level1_small
  2/-10/-33 128 0` and the `as_` one `2/-9/-33 128 1`: 1 the PAC's Titan,
  0 the EU's) keeps a map (`0x100ed200`, a missing file an 8 × 8 white
  one); each frame (`0x1002db21` → `0x100eda70`) its matrix is the
  Titan's inverse then the offset taken off; an object over a map's
  square (local y > 0, |x|, |z| < size / 2, the nearest above,
  `0x100edce0`) takes its texel bilinearly at `u = size/2 − x`, `v =
  size/2 + z` (`0x100ecb10`, on the CPU), else keeps its last one (0.5
  at first). The rewrite takes the camera as in a Titan when it is over
  a Titan map's square (it has no soldier-in-Titan test of the
  renderer's), the Titans from `LevelSources::titanObject`.

**The light manager's other colours.** All of `lightManager.*` is now
registered; `enableSun` gates the sun term. What RendDX9 does with the
rest (the light manager `DAT_101ffc84`; per-frame table `DAT_101fae14`
filled at `0x1002a4a0` / `0x1002cef0`):

- `treeSkyColor` is uploaded as TreeSkyColor × 0.5 (table 0x138) but no
  shipped shader reads it (commented out in RaShaderLeaf.fx and
  RaShaderTrunkSTMDetail.fx): settled unused. Trees take OverGrowthAmbient
  from `treeAmbientColor` and their light from `treeSunColor`.
- `staticSpecularColor` (StaticSpecularColor, 0x1d8): RaShaderSTM's
  `pow(saturate(N·H), 32) × lightmap.g × gloss` times it (the gloss the
  detail map's alpha when the technique has a detail layer and isn't
  alpha-tested, else StaticGloss: `getCompositeDiffuse`); `sunSpecColor`
  builds SpecLUT64SpecularColor (`0x1014aec0` → `0x101ac540`: a 256 × 1
  texture, texel `i` = `(i/255)^64 × sunSpec`) that BM per-pixel
  specular samples at H·N times the gloss (the colour map's alpha with a
  ColormapGloss technique, else StaticGloss, `setSpecularStaticGloss`,
  default 0.15), killed by the shadow and the hemi occlusion. Drawn by
  mesh.frag's `specularTerm` (LightingUniforms `eye` / `specular`; the
  submesh's `specularKind` from its effect and technique). Bundled meshes
  without a normal map or ColormapGloss take the per-pixel LUT with
  StaticGloss where RaShaderBM lights them per vertex (`lit(N·L, R·V,
  SpecularPower)` clamped to StaticGloss). Skinned meshes (RaShaderSM):
  pow(N·H, SpecularPower) × the gloss × the sun's specular colour,
  SpecularPower the material block's default 20 (`0x10040070` +0x74), the
  gloss the normal map's alpha, else StaticGloss 0.15 (and nothing facing
  away, lit()).
- *Normal maps* (RaShaderBM/SM `_HASNORMALMAP_`, at the top lighting level
  only): a bundled mesh's second map ending "_b.dds" (`0x101499b0` +0x35)
  is in tangent space; a skinned mesh's second map unless the SpecularLUT
  is in tangent space when the technique names "tangent" (`0x100d4160`
  +0x100), else in object space (`_OBJSPACENORMALMAP_`: the first-person
  arms' "_b_os.dds"). The tangent is the vertex's TANGENT, the binormal
  cross(tangent, normal) × (1 − 2 × the third blend-index byte)
  (createWorld2TanMat / getTangentBasis). Bundled and skinned meshes keep
  the tangent in the static layers' coordinates (Vertex dirtCoord,
  crackCoord), mesh.vert hands mesh.frag the map's frame in the world,
  and the map is bound in the dirt map's place; the bumped normal lights
  the sun, the point lights and the specular, a skinned mesh's hemi lerp
  too (per pixel at RAPATH 0), its alpha is the gloss unless the colour
  map's (ColormapGloss). `LevelStats::normalMapped` counts them (17 on
  Cerbere Landing at load).
- `effectSunColor` / `effectShadowColor` light the particles (FXCommon.fx
  `calcParticleLighting`: `lerp(1, lerp(shadow, sun, saturate(lm + off)),
  f)`, `lm` the base hemi map's alpha under the particle, `off` =
  `saturate(clamp((y − HemiShadowAltitude) / 10, 0, 1) +
  lightMapIntensityOffset)` (vsParticle), `f` the light factor).
  Settled: HemiShadowAltitude is the base hemi map's fourth argument (the
  height, 500 in every Sky.con; HemiMapManager's map `+0x44`, uploaded by
  the sprite manager's `0x10131d70`); the light factor is the template's
  `+0x1fc` (TemplateParameters `m_color1AndLightFactor.a`), which the
  constructor `0x10138b40` sets to −1 and no script property reaches
  (its setter, slot 0x15c, has no caller), so `0x10139800` takes
  `lightManager.defaultEffectLightAffectionFactor` (1 on every level).
  The shader quality (`ParticleSystemManager.particleSystemShaderQuality`,
  the effects row's) picks psParticleLow (1: unlit), Medium (2: `lm` 1)
  or High (3: the hemi map). The rewrite lights each particle on the CPU
  (`LevelRenderer::particleLight`, the hemi map's alpha decoded from its
  DXT5 blocks, a level of at most 512 texels; `hemiShadowAt`).

**Environment maps** (`envmapManager.*`, EnvMapManager `DAT_101ff9cc`,
vtable `0x101c6378`). At level load `0x100639b0` reads
`<level>/Envmaps/EnvironmentMapInfo.emi` (`loadEMIFile` `0x10064770`:
lines `EnvMapN x,y,z,bool`); each frame `0x10064f30` binds the entry
nearest the camera as the one cube for bundled materials whose technique
names "envmap" and static meshes (BM `_HASENVMAP_`: `lerp(diffuse,
texCUBE(R), gloss / 4)` with a Fresnel alpha; reflection scale
`setMaterialReflectionScale`, default 0.25), and the Titan interiors bind
`common/textures/indoorenvmap`. `generateEnvMaps` and `save` are the
editor's. No retail level ships an .emi (only six community levels do;
no script calls `envmapManager.*`), so in the retail game the cube stays
unset: not drawn, as shipped.

**Sky layers and the sun flare** (`0x100fd2f0`, `0x100fd900`, SkyDome.fx).
With `skyDome.hasCloudLayer` the dome's technique is SkyDomeNV3x (one
cloud texture on the dome's second coordinate set, scrolled by TEXOFFSET),
with `hasCloudLayer2` SkyDomeNV3xDualClouds (`cloud × lerp.x + cloud2 ×
lerp.y`); both fade out from CLOUDSFADEOUTDIST = (fadeCloudsDistances.x,
y − x) along the dome's radius and below its horizon, then `lerp(sky, c,
c.a)`. The offsets grow by `scrollDirection` a second. The flare is the
flare template's sprite towards `-flareDirection`, added (SkyDomeSunFlare:
ONE/ONE, no depth test) at a strength of the occlusion query's visible
share (`(visible / total + flareFadeAdd) × flareFadeMul`, `SkyDomeFlareOccludeCheck`
every few frames); the rewrite draws a camera-facing quad of the flare
texture 4 km out, depth-tested instead of the query, brightest looking
into it. `renderer.drawSunFlare` and `skyDome.domeSize` are not read.

**Post-production** (PostProduction_nv3x.fx, FSQuadDrawer.fx; the object
`0x1004a590` constructs, `DAT_101fb808`; driven from the renderer's frame
at `0x1002df12`..`0x1002e6d0`). The order:

1. *Glow and camouflage* (`0x1004a0b0`, `0x10049560`, `0x10048a80`,
   `0x100495f0`, `0x10048b50`). Cloaked soldiers draw with
   RaShaderSMActiveCamo.fx / RaShaderBMActiveCammo.fx technique Camouflage
   (alpha only); FSBMExtractCamouflage (`(max(0.4, a) − 0.5) × 2`), a 5 × 5
   blur and FSBMCamouflageBlurPassThrough blend the backbuffer shifted by
   CAMOUFLAGEOFFSET (`renderer.camouflageOffset`), its luminance's
   contrast × 0.1 added, by that mask: the soldier refracts what is behind
   him. The rewrite draws cloaked soldiers (GameWorld hides them; the
   renderer asks `LevelSources::cloaked`) into a mask tested against the
   world's depth. *Glow is dead code in RendDX9.* The list at
   `*(DAT_101fae6c + 0x38)` is not a glow list: `DAT_101fae6c` is the
   RaShader manager (ctor `0x1003ed70`), its +4..+0x3c the 15 render
   queues (`0x10038250`), and +0x38 is RSMTCamouflage, filled only by the
   bundled mesh (`0x100c2990`, BMActiveCammo / AdditiveAlpha) and the
   skinned mesh (`0x10161150`, SMActiveCamo Camouflage, or
   BrokenCamouflage flickering by a random draw): the cloaked soldiers'
   mask above. The glow passes (`0x10049560` ExtractGlow, `0x100495f0` →
   `0x10048c40` GlowH/GlowV and the additive composite) run only when
   PostProduction `+0x21` is set, which only `0x10048d10` (GlowMaterial
   with GLOWSTRENGTH) does, and neither it nor `0x10048e80` (Glow) has a
   caller or a pointer anywhere in the DLL. `renderer.glowEnabled`
   (`0x1004ed50` → `+0x20`, default 0) and `renderer.glowStrength`
   (`0x1004eee0` → `+0xe8`, 0.5) are read only by that dead function, and
   `renderer.colorBrightness` (`0x1004fad0` → `0x10035c40`, `+0xa58`..)
   only by its getter: all three are stored, and nothing glows (the
   shipped `hasGlow 1`, on the EU Titan's monitor and the floodlight
   cones, is the EXE's).
2. *Bloom* (`0x10049140`): FSBMContrastFilter (pass 28: the backbuffer
   cubed, to luminance, less CONTRAST = `bloomFilterContrast`) into a
   small target, FSBMLargeBloomFilter across (pass 29: 13 taps 0.01 and
   0.015 × BLOOMSIZE × 1..3 apart, weighted 1, 0.45 .. 0.15 × BLOOMWEIGHTSCALE
   × 0.9), FSBMGaussianBlur5x5Cheap (10), the large bloom down (30), then
   Soften: `saturate(backbuffer + bloom × GLOWSTRENGTH)` with GLOWSTRENGTH
   = `bloomOverlayAmount`; `showBloomOverlayTexture` shows the bloom alone.
   The setters `0x100508d0`..`0x10050f80` write +0x104..+0x114; defaults 1,
   1, 1, 0.2. The rewrite's small targets are a quarter of the screen.
   `VideoSettings.setUseBloom` (the menu's BloomEnabled) turns it off.
3. *Tinnitus* (`0x1004a2f0`) instead of the bloom while one runs: the game
   logic's event 0xd (ECGameLogicGLTinnitus, data type 1 and three times,
   Sound.tinnitusSetup's 1 2 4) lasts `(a + b + c) × 30` frames (at least
   20, `0x10049440`); the last four frames weighted 0.5, 0.25, 0.125,
   0.0675 at a random SAMPLEOFFSET, lerped to the frame by its progress
   (halved below one half).
4. *Colour grade* (`0x1004a120` then ColorTransform `0x10049a40`): the
   matrix `0x10049900` builds, `out_r = Σ_c ((1 − s) lum_c + s[r = c]) tint_c in_c`
   (lum 0.3086, 0.6094, 0.0820; s `defaultSaturation`, tint
   `defaultColorTint`), transposed into COLORTRANSFORMMATRIX, then
   CONTRASTPOLYNOM `a x + b x² + c` (`defaultContrast`; Camp Gibraltar's
   2.1/−1/−0.04 lifts the mid-tones). It runs unless the view is tinted or
   the player is EMPed (`0x10025be0`). CameraEffect (`0x10049b90`) takes its
   place while the view's camera asks for it: the same grade on a
   fisheye from `common/textures/frame` with `common/textures/scanline`
   rolling (FRACTIME + 0.15 a frame's seconds, used below 1, reset at
   1.5). The rewrite turns it on in a vehicle seat in first person whose
   weapon's `weaponHud.enablePostProcessingOnGuiIndex` names the HUD shown
   (`altGuiIndex` zoomed, else `guiIndex`: the tanks' and walkers' zoomed
   sights).
5. *EMP* (`0x10049d70`): a vehicle's EMP level (the player's slot 0x380)
   loads the static from the table `0x10048950` builds (`0x10048a30`:
   INTERFERENCE 0 then 8 for levels 1-5, DISTORTIONROLL 0 then 0.02, no
   warp) and a second grade (`0x1004a280`: the view's colour at saturation
   0 when the view is tinted, else neutral); a soldier's level + 3 (5 for
   3) with the saturation swinging ±3 by 0.01 a frame. `psDx9_EMP20`: the
   random texture's row shifts the image across by DISTORTIONROLL, the
   static takes from it and adds a little red, the contrast polynomial
   again. `renderer.emp*` (`0x10050250`..`0x10050730`) override the table.
6. *Titan explosion* (event 0x14, `0x10049360`, run by `0x1004a120`): the
   grade's tint 1 + 20 t with the contrast's constant t / 2 for two
   seconds, then white (tint 40, saturation 0, + 1) until 12 s. The
   rewrite starts it when `GameWorld::titans()` reports a core explosion.
7. *TVEffect* (`0x10048f80`, `ps_TVEffect20`: grey image, rolled by the
   noise and `1 − x²` bands, the random texture's interference; the
   PostProcess component's `tvInterference`, `tvDistortionRoll`,
   `tvDistortionFreq`, `tvDistortionScale`): nothing in RendDX9 calls it
   (no caller, no vtable slot) and the game has no TV-guided view, so it
   is implemented (`PostEffects::tv`) but never on.

The rewrite does all of these in one full-screen pass: the passes that
move pixels (the roll, the fisheye, the TV's bands) read the earlier
stages at the moved coordinates. `renderer.drawPostProduction 0` draws
the world straight into the target. `BF2142_POST_EFFECT=emp:N |
soldieremp:N | tinnitus | tv | camera | flash | camo` forces one for a
screenshot. PostProduction's Fog (a full-screen fog from a position
buffer) and ThermopticCamouflage (returns black) techniques are named
nowhere in RendDX9 (no technique string selects them): not drawn; fog
stays in the shaders, the camouflage is FSQuadDrawer's passes above.
The game state: the soldier's own EMP level is its armour's on foot
(the player's `0x005921b0` walks from its control object to the first
armour, slot `0x14c`), which an EMP raises like a vehicle's when the
soldier template has `armor.takesEmpDamage` (no shipped soldier sets it);
`PlayerInfo::empLevel`. The tinnitus event's sender wasn't found (no
`sound-tinnitus-range` setting exists in BF2142.exe; event 0xd is posted
through the game logic's interface slot `0xa4`, `0x004a5e80`): the
rewrite rings the local soldier's ears for an explosion whose radius ×
`GameWorld::tinnitusRange` (2) reaches it (`takeTinnitus`, the setup's
1 + 2 + 4 s). The view's tint colour (its slot 0x7c) is not decoded.

## Kits on the ground

`src/Game/Kits.cpp`. A dead soldier's kit (on foot or thrown out of a
seat) drops (`0x00589890`): its items go back in with their rounds, and a
ray 1000 m straight down from the soldier places it on what it meets
(statics, bodies, the terrain); on a Titan it rides with it
(`0x00561b20` / `0x005ad2e0`, put at the Titan's position plus its offset
each tick, `0x005aa5b0`). It lies timeToLiveAfterDeath (kit template
`+0x2b8`, 30 s by default, `0x00570080`) and goes. ServerGameLogic's
pickUpClosestKit (`0x004bcd80`): the closest kit within the radius whose
slot 0x24 matches the holder's 0x20 (the soldier template's `+0x2bc`
against the kit's ItemContainer `+0x2a4`, neither set by a script: every
kit); the taker drops his own first (`0x0058b680`), takes its items with
their rounds and its battery, holds the first from slot 3 up, and the
player's kit time stamp is set. The use key asks for one within 3 m when
no vehicle takes the soldier and 2 s have passed (`0x004bda00`); a revive
within 10 m, else a new kit (`0x00587b90` event 0x19, `0x004bfcc0`).
DropKit and PickupKit go to the scripts. The renderer should draw a lying
kit with its `geometry.dropGeom` (`GameWorld::isDroppedKit`).

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
- 1 bit, PlayerInput `+0x110` (see below: whether pitch and roll come
  from a device axis; the server stores it and nothing reads it).

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
| 0xe, 0xf, 0x10 | the distributed lightmap renderer's | c→s: 0x10 (no payload) after the client echoes 6/4, when the byte at `0x00a1fc02` is set (`0x0042b820`); the server (`0x004b0690`) hands the sender to `DistributedLightRenderer` (`0x00a0f7a0`, slots `0x18`, `0x20`; 0xf sends the sender a chat line of type 9). Only command-line option 0x22 sets the byte (`0x00409950`, also capping allocations at 26 MB), and the retail table registers no option 0x22 (`0x004409d0`'s 34 calls), so they are never sent |

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
  without that object skips it by the length. With it: 1 bit (a fresh or
  a relative state), a 3-bit history slot, every group of the ghost (and
  the second ghost) in mode 3, a 10-bit length and the player's own state
  (the connection's player, vtable `0x1c8` writes, `0x1cc` reads). See
  [the control-object record](#the-control-object-record-and-prediction).
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
- in mode 3 (the control-object record) the soldier's movement goes at
  full precision: see [the control-object record](#the-control-object-record-and-prediction);
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
  rotation (0x200-0x800 angles, 0x100 a 14-bit quaternion). In mode 3
  every angle and speed takes 16 bits (`0x006015a0`: `(mode == 3) * 4 +
  12`). The writer only sends the angles an input drives. Values within one step of 0 read
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
with the mouse, and 0 where keys drive them (the Sea map). The server
copies it back into the player's PlayerInput with the move (`0x005838d0`,
from `0x004ac660`, which passes the PlayerInput by value to the game
logic and the player). No code in BF2142.exe reads PlayerInput `+0x110`
after that: of the binary's byte reads at `+0x110`, the others are the
move writer (`0x00583b50`), the player (`0x0059bd80`) and unrelated
classes. It changes nothing on the server.

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
- **8, end of round** (EndOfRoundInfo, `src BF2/Game/EndOfRoundInfo.cpp`:
  written by `0x0059cbc0`, read by `0x0059c740`; the scripts'
  `gl_sendEndOfRoundData` is `0x00568930`): the text's size and its
  compressed size (32 bits each), the text compressed with zlib
  (`compress2` at the default level, `0x0071bd80`; at most 0x8000 bytes),
  the end-of-round delay (sign + 31 bits, `0x00515350`: sv.endOfRoundDelay,
  settings `+0x270`, 3..30, or 15 in gpm_ti, `0x00430610`), an 8-bit
  player count, then per player (the player manager's list, slot `0x40`)
  its id (8 bits) and six values of a sign and 15 bits from its stats
  (player vtable `0x1a8`, `0x00840330`: `*(player +0x100)`): kills
  (`+0x14`), skillScore (`+0x8`), deaths (`+0x10`), score (`+0`), rplScore
  (`+4`) and cmdScore (`+0xc`), as the scripts' getters name the offsets
  (`0x0056b6f0`). The reader stores them back into each player's stats.
  `RemoteWorld::endOfRoundData` decodes it. 7 is console text, 9 a demo
  URL. With the distributed lightmap renderer on (see PostRemote 6/0x10)
  block 4 goes to it.

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

### The server's side (`bf2142 --host`)

The original can host a game from the same executable (`+dedicated`,
`+port`, and `+joinServer <address>` for the client's direct join are
among its command-line options, the table near `0x0050e000`). The rewrite's
server is `src/Net/GameServer` (the protocol) and `src/Main/HostedGame`
(the round it serves):

- **Connecting** (`0x00689d30`, `0x00689400`): the protocol and build must
  match (else refusal 9), the password (0x11), the mod directory (0x24); a
  full server refuses 0x1e. The client gets a connection id; once it
  acknowledges (type 4) the server sends the CD-key challenge (event 1,
  `0x00509cc0`) and takes the response (event 2) and the connection type
  (event 3, the rate row of `0x00928408`) before the client counts as
  joined. Pings every 3 s, pongs answered; 30 s of silence drops a client.
- **Reliable events** go both ways in the same blocks as the client's
  (`src/Net/EventChannel`: the writer `0x00644bf0`, the reader
  `0x006447b0`), resent after a loss the notify mask reports.
- **Moves**: each move index is taken once (the client repeats its last
  three, `0x00590450`); the move becomes the player's input as `0x005838d0`
  does (the axes, the trigger bits; the mouse look turns 5° a unit,
  `0x0058a6d0`), through `net::soldierInputFromMove`, which the client's
  prediction uses too.
- **Joining a round** follows the game server's PostRemote handler
  (`0x004b0690`): the settings (data block 0), the map list (5) and the
  level (2) as soon as the client has joined; on its LoadComplete (6/2,
  `0x004a7c00`) the players (CreatePlayer, its own without the remote bit),
  every replicated object (CreateObject, one ghost per PlayerControlObject
  of its tree), the soldiers held (HandlePickup), DatabaseComplete (6/4)
  and the networkable checksum (6/5); on the echo of 6/4 (`0x004af710`)
  StartSimulation (6/3) and the game state (0/1), then BeginRound.
- **A packet per client at its connection's rate** (see [ghost priorities
  and bandwidth](#ghost-priorities-and-bandwidth)): the input block, the
  events, then a ghost block with what is left of the packet's bytes: the
  control-object record of its soldier (or the seat it is in, with the
  vehicle's ghost), then the ghosts the client's GhostManager picks. The
  record carries the prediction state (mode 3 and the player's own state,
  see [the control-object record](#the-control-object-record-and-prediction)).
- **Client requests**: SelectKit (`0x0050b010`), the spawn point (6/6), a
  name (ChangePlayerName, taken only for the sender's own player,
  `0x0050bb90`; the rewrite's client sends its soldier's name after its
  CreatePlayer), squads, the commander application, suicide (6/0xc) and
  chat lines (the StringBlock, sent on to everyone with the sender's id,
  `0x004a5460`).

- **Moves are stepped one a tick**, in order: a client's moves wait in a
  queue of at most four (past that the oldest go; the depth is
  OpenBattlefield2's reading of BF2's `GameServer::clearPlayerActions`, not
  traced in BF2142.exe), and after the queue runs dry it waits for two before
  stepping again, so a move a tick late doesn't leave a tick stepped with
  none. The record's last move is the one the round stepped last.
- **Every group of every ghost** goes out, not only the root's: a
  RotationalBundle's angles (the turrets' yaw and pitch, the steering, the
  rotors, as the round poses the part), a Spring's offset (the wheels'
  suspension), a seat's occupant (a seat without cameras carries its
  soldier's state), a vehicle's velocity and angular velocity, and the
  armoured parts below it (0x800000; the rewrite's vehicles keep their
  armour on the root, so they go as whole). The client poses a joined
  game's vehicles from them (`RemoteWorld::partPose`). Engines, MechLegs and
  a seat's weapon fields stay empty: the round doesn't model them.
- **Team changes** (6/7, `0x004c1dc0`; the server's handler `0x004b0690`
  case 7 hands it to the game logic's slot `0x60`) are taken while the
  player has no soldier (`GameWorld::setPlayerTeam`: the squad and the
  command left, PlayerChangeTeams for the scripts); `--connect --play 1|2`
  asks for the team before spawning.
- **Map changes and new rounds** keep the connections
  (`HostedGame::changeLevel`; `bf2142 --host --maps
  level[:mode[:size]],...` plays a list): every client gets the map list and
  the level block again, loads it and answers LoadComplete, and the join's
  database follows (a new player of the client's team, the new round's
  objects), as the original's map change does (`0x004abbc0(-1)` broadcasts
  the level block and counts a pending load per connection). A round on the
  same map is the same level again. The client clears its players and
  ghosts on the new level block and sends no moves until its new
  CreatePlayer (`0x00590740`).
- **Kicks** (`HostedGame::kickPlayer`): KickBan (0x37, written by
  `0x0050cf30`: the player, banned, a byte) to every playing client, whose
  handler (`0x0050cfd0`) posts message 2/0x4d for the HUD, then the
  disconnect, sent three times as `0x0068e520` does. The client prints who
  left and why, and a disconnect's reason (`net::disconnectReason`).

A client that leaves takes its player with it, as the game server's
destroyPlayer (`0x004acfb0`, GameServer.cpp; reached from the session's
disconnections in `0x004ae1f0`, PlayerManager slot `0x20` by id then the
server's vt+0x120) does: a player with a vehicle that isn't its free camera
(or an AI one) dies there (game logic slot `0x54`, `0x004c1d20`: the
player's vt+0x100(0) and the death `0x004cd390`); it leaves its squad (the
squad manager `DAT_00a1c174` slot `0x38`) and the command (slot `0x70`);
the scripts hear of it; the free camera goes; a human leaves the count
(`+0xe0`); every client gets DestroyPlayer (vtable `0x0091cb58`, the id);
GAME_MESSAGE_PLAYER_DISCONNECTED goes to the chat; the player's
networkable ghost goes (`0x0067bb30`); the PlayerManager lets the player
go (slot `0x14`) and AIDLL's records of it are dropped (`DAT_00b4bbcc`
slot `0x14`). `GameWorld::removePlayer` does the round's part (the body
lies on as a corpse; PlayerDisconnect), `HostedGame` the rest, and the
next `addPlayer` takes the lowest free id.

### Ghost priorities and bandwidth

Each ServerConnection (constructor `0x0057b3f0`) has a GhostManager
(`conn +0x2c`, constructor `0x00582a50`, vtable `0x0096531c`;
GhostManager.cpp) with a state per ghost the client has (0x54 bytes:
`+0x40` always, `+0x44` priority, `+0x48` accumulated, `+0x50` sendable,
12 dirty masks, status 1 in scope or 3 out). `net::GhostScheduler`,
`HostedGame::ghostsFor` / `rescore` / `ghostLost`.

- **The send loop** (`0x004b02a0`): each connection's timer (`+0x1c`)
  falls by the frame; out, the connection sends and adds 1 / its packets a
  second (`+0x14`). The rates are 20 a second of 1400 bytes (`+0x18`,
  0x578) until the connection type comes; then the row of `0x00928408`
  (`0x004a6160`: 4, 4, 10, 20, 20, 20 a second of 260, 260, 350, 400,
  512, 640 bytes; type 6 and over left as they are; not for the demo
  connection). `GameServer::sendPaced`.
- **The packet** (`0x0057b040`): its bytes × 8 bits, shared by the
  writers in order (`0x0057b380`): input, events, ghosts. The ghost writer
  (`0x00582cd0`) reserves its 42-bit header (present, tick, count, record
  bit), writes the record (`0x00580830`), lost removals, then pops the
  heap: at most 40 entries (`gm +0x213c`; the demo's by sv.demoQuality),
  each 2 bits kind and 16 bits id, kind 1 a full bit, the 11-bit length
  (13 on a demo connection) and the groups; one that doesn't fit is
  skipped (keeping its accumulated value) until under 24 bits are left.
- **Base priority**: NetworkableInfo.setBasePriority (the info's `+0x18`,
  1 by default, at most 255; c_NIGhostAlways = −1), copied into the
  networkable by `0x006fa190`. Registering a ghost (`0x00679d90`) puts a
  negative one (or a ghost without an object, the players') in the always
  set: every client has them at 255, never out of scope. 0 is never
  scored. predictionMode's reader isn't in the ghost manager.
- **Scoring** (`0x0057dc10`, at most every 0.1 s a client, `conn +0x20`):
  every state not always goes out of scope at 10000 (`0x00581d40`); each
  object tree within R = max(GSViewDistance, 500) (850 by default,
  `0x004cc8d0`; 1000 for a projectile whose vt+0x108 holds) is scored once
  by its root (`0x0057d300`): min(1, |v|² × 0.000625) + (the direction ·
  the eye's forward ≥ fov × 0.5 × 4/3 ? 1 : 0; an angle against a cosine,
  as the original has it) + (1 − d / R), and each of its ghosts gets (score
  + 0.3 for a projectile) × its base priority (`0x0057d560`), back in
  scope; each living teammate's soldier or vehicle at least 0.03. A state
  new to the client starts at 10000 with every mask set (`0x00581e80`).
  The demo connection takes everything at 1 (`0x0057d8f0`).
- **Queueing** (`0x005823c0`, each packet): a state out of scope or with
  something to send adds its priority to its accumulated value and joins
  a max-heap on it; written, it starts from 0 (the starvation guard).
  Nothing resends a clean in-scope ghost (no periodic refresh).
- **Out of scope**: kind 3, then the state goes (`0x00581dc0`); the client
  hides the object (`0x0057fce0` → `0x0057bb10`) and a kind-1 update
  brings it back (`0x0057bdc0`; `RemoteWorld` keeps it aside).
- **Losses** (`0x005830e0`): what a lost packet carried that no later
  packet carried is dirty again at 10000; a lost removal goes first next.

The rewrite's stand-ins: the dirty masks are "the state the client last
got differs" (the states encoded once a tick for every client); the eye
is the soldier's head (kept from its last life; before any, every ghost at
1), with a field of view of 1.1; the client's own soldier and vehicle are
scored like any (the original skips the player's own, which the record
carries). Not rewritten: the commander's satellite scan and the UAV
(enemies at 0.5, `0x0057cfb0`), his soldiers with `+0x29c` set (0.03),
OptimizeEnclosingObjectsNetworking / GhostNotEnclosed, the alternate eye
of player vt+0xd4, the 20-rescores-a-pass cap, the enable set (`gm +0x28`).

### Server settings, the map list and remote admin

A server (`bf2142 --host`, with `--dedicated` headless) keeps its
configuration from map to map (`src/Net/ServerSettings`,
`src/Main/ServerAdmin`), as BF2142.exe's settings object (`DAT_00a201c0`)
and map list (`DAT_00a21940`, `MapList.cpp`) outlive the levels:

- **Files** (`0x00409950`, the command line): `+config <file>` names the
  settings' file, else `<mod>/Settings/serversettings.con` (with an
  overlay, `+overlayPath`, the original's `GSOverlayPath`:
  `<overlay>Settings/...`); `+mapList <file>` the map list's, else
  `<mod>/Settings/maplist.con`. Both are console scripts. The names are
  matched without case (the install has `ServerSettings.con`). The bans
  are `Settings/banlist.con` (`0x00507570` writes `admin.addKeyToBanList`
  and `admin.addAddressToBanList` lines after each ban; round bans are not
  kept). In the rewrite `--maps` or `+level` replace the map list file's
  maps; command-line `+port`, `--query-port` and `--time-limit` win over
  `sv.serverPort`, `sv.gameSpyPort` and `sv.timeLimit`.
- **`sv.*`** (76, registered `0x0041b003..0x004207a8`): every setting as a
  property (no argument reads, one writes), `sv.load`, `sv.save` (the
  order and quoting of `0x00431880`: strings quoted except `interfaceIP`,
  `voipServerRemoteIP` and `voipSharedPassword`; `sv.ranked` only on a
  ranked server), `sv.configFile`. Read by the server: the name, password,
  port, max players, query port, welcome message (the settings block's up
  to three 256-byte pieces) and the block's flags (`allowNoseCam`,
  `hitIndicator`, `tkPunishByDefault`, `allowFreeCam`,
  `allowExternalViews`, voice ports and quality, radio spam limits,
  voting, players needed, `friendlyFireWithMines`, `allowTitanMovement`),
  `timeLimit` (seconds: the game logic's time limit), `spawnTime`,
  `manDownTime`, `roundsPerMap`, `endDelay` (seconds from a round's end
  to the next map; ServerSettingsDesc.csv: "between when a round ends and
  a new round begins"), `adminScript`, `botSkill`, and the Python keys
  below. The friendly-fire percentages, `startDelay`,
  `notEnoughPlayersRestartDelay`, `endOfRoundDelay`, the demo and sponsor
  settings, PunkBuster and `internet` are kept but not acted on.
- **`GameServerSettings.*`** (45, `0x0041655b..0x00419112`), the menu's
  host page's setters, set the same settings (`setSoldierFF` is
  `soldierFriendlyFire`, `setPunishTeamKills` `tkPunishEnabled`,
  `setTeamRatio` `teamRatioPercent`, `setDemoHook` `autoDemoHook`...).
- **`maplist.*`** (`0x0042154a..0x00421d50`): `append level mode [size]`
  and `insert index level mode [size]` check the level and its mode are
  installed (`0x00441090`: "could not add to maplist. map not found:" /
  "gameMode not found:"), `remove`, `clear`, `list` (`i: "level" "mode"
  size` lines), `mapCount`, `currentMap`, `load`, `save` (`maplist.append
  "level" "mode" size`, `0x00440ef0`), `configFile`. The joining client
  gets the whole list and the running map's index (data block 5,
  `0x00441410`).
- **`admin.*`** (`0x00429bba..0x0042adfc`): `runNextLevel`, `restartMap`,
  `runLevel level mode size` end the map at once and start the one asked
  for; `nextLevel` (property) picks the map after this one; `currentLevel`;
  `kickPlayer id` and `banPlayer id [period]` / `banPlayerKey id [period]`
  (`0x004a6090`, `0x00507e10`, `0x00507e80`: "Player not found." for no
  such player; the address or CD-key hash banned, the list saved, the
  player kicked with KickBan); the ban list's `add*`, `remove*`,
  `clearBanList`, `listBanned*`. A period (`0x00505db0`) is `perm` or
  nothing, `round`, or seconds. A banned address is refused on connecting
  (refusal 0x16), a banned key once its challenge answer gives its hash.
  `listPlayers` is `0x004a86b0`'s text (`Id:%3d - <name> is remote ip:
  <address> ->\n         CD-key hash: <hash> `, ` is local.`, ` is an AI
  bot.`).
- **Rotation**: after each round `ServerAdmin::nextRound` gives the next
  map: what an admin asked for, else `sv.roundsPerMap` rounds on each map
  of the list in turn. Round bans end. The clients stay connected through
  the change (`HostedGame::changeLevel`).
- **`reservedSlots.*`** (`0x0041aa21..`): the nicks, `load`/`save`
  (`reservedSlots.addNick` lines). Kept, not yet used to admit a player to
  a full server.
- **`Game.sayAll` / `Game.sayTeam`**: a chat line on channel 12 (13 to a
  team) from player 0xff with source 1.

**Python.** `host.rcon_invoke` (`0x004427b0`) runs a console command (line
by line through the console's `executeString` `0x00674430`) and returns its
output ending with a newline (nothing for nothing; a ranked server refuses
`vars.set` with "Ranked server blocks vars.set from remote console").
`host.rcon_feedback(player, text)` (`0x00442940`) posts "rcon: " + the text
to the player as message 6/0x13; the rewrite sends it as PostRemote 6/0x13
payloads of the int -1 and up to 48 characters each (the client's handler
prints a -1 payload to its console, `0x004b6630`); how the exe carries a
longer text was not traced. A client's `rcon <line>` (the console's
`executeString` posts 6/0x12) comes to the server as 6/0x12 with the line
and raises the scripts' `RemoteCommand(player, line)` (`0x004b0690` case
0x12 makes event 0x2b); `GameClient::remoteCommand` sends one.
`host.ss_getParam` (`0x00568350`) reads the settings: `gameMode` and
`mapName` from the game logic, `ticketRatio`, `maxPlayers`, `timeLimit`,
`scoreLimit`, `autoBalance` (`sv.autoBalanceTeam`), `teamRatioPct` (a
float), `tkpEnabled`, `tkpNeeded`, `tkpDefault`, `globRank`,
`globUnlocks`, `ranked`, `maxRank`. `host.sgl_getOverlayDirectory`
(`0x00567730`) is `GSOverlayPath`: "" in the exe, which leaves
`admin/default.cfg` to the working directory (the install); the rewrite
gives the install's directory (or `+overlayPath`).

**The admin script.** `sv.adminScript` names a module of the install's
`admin` folder ("default": `admin/default.py`), imported once, its
`init()` run, its `update()` every frame and `shutdown()` at the end
(`src/Main/AdminScript`). `default.py` is the TCP remote console: it binds
port `port` of `admin/default.cfg` (4711) on 0.0.0.0 (127.0.0.1 in the
rewrite, which keeps servers on loopback), greets with `### Battlefield
2142 default RCON/admin ready.`, `### Digest seed: <16 letters>` and an
empty line; `login <md5 hex of seed + password>` authenticates, then
`exec <command>` runs a console command through `rcon_invoke`, `users`
lists the authenticated; a line starting with 0x02 gets a reply ending
with 0x04. Its `init()` imports `admin/standard_admin`: `autobalance.py`
(a joining player to the smaller team; on a death, a player moved when
the teams differ by more than one, weighted by `teamRatioPct`, while
`autoBalance` is on), `tk_punish.py` (team kills to punish or forgive
within 20 s; `tkpNeeded` punishments ban for the round through
`rcon_invoke("admin.banPlayer <id> Round")`) and `playerconnect.py`. Team
balance and TK punishing are these scripts', not the engine's. The
scripts are Python 2; besides `py2compat`'s translation the rewrite gives
them a `socket` module with Python 2's `str` data and errors indexed as
tuples (`detail[0]`, a would-block always `errno.EWOULDBLOCK`, a
`shutdown` of a gone peer ignored as Winsock does), `md5` objects taking
`str`, and the `admin` package from the install's `admin` (or `Admin`)
folder. The interpreter outlives the per-map script hosts, so the admin
module and its connections live on; the handlers and timers it
registered are registered again on each map's host. `bf2142 --rcon
host[:port] --rcon-password p --rcon-command "<command>"` is a client
(`src/Net/RemoteConsole`).

**Dedicated.** `bf2142 --host --dedicated` (or `+dedicated 1`) runs
without a window, sound or client archives: the map list in turn, until
Ctrl+C, SIGTERM or a console `quit` (through rcon), or for `--simulate N`
seconds over all its maps.

### Server rules: reserved slots, friendly fire, the start, votes

- **Reserved slots** (`0x004ae6a0`; sv.numReservedSlots, setter
  `0x004317c0`, at most maxPlayers / 2). The transport takes ten over
  sv.maxPlayers (`0x00688300`); a joiner finding the server full is refused
  (2) unless there are reserved slots, fewer reserved players in than
  slots and its nick (the name after a clan prefix's first space,
  `0x0043f7e0`) is listed: then the connected unlisted player with the
  highest player index is kicked (0x23). No slots are held back.
  `HostedGame::reservedSlotCheck` decides when the client's name comes
  (the rewrite's clients send it after connecting).
- **Friendly fire** (`0x005b7290`): damage from an attacker on a team to
  what its team controls (the victim's control object's team, slot 0x9c)
  is × the percentage / 100: sv.soldierFriendlyFire /
  soldierSplashFriendlyFire for soldiers, vehicleFriendlyFire /
  vehicleSplashFriendlyFire for anything else (setters 0..100, under 1 as
  0). `GameWorld::setFriendlyFire`. The victim's factor f applies to
  all damage, friendly or not: its PlayerControlObject's slot 0xec
  (`0x005c6af0`) counts per ability type n the crew aboard whose kit lists
  the vehicle (kit template `+0x308`, entering `0x0058cf00` and leaving
  `0x00588ed0` move slots 0xe4 / 0xe8); type 4 alone gives 0.9, type 5
  alone 1.1. A soldier's slot 0xec is 0 (`0x007f1f70`): 1. Which loader
  fills the kit's list isn't found, so f is 1 in practice and not
  rewritten.
- **The start** (the server game logic's states, `0x004c03b0`): waiting
  while the players are under sv.numPlayersNeededToStart, then
  sv.startDelay's countdown (`+0x280`), then playing; fewer players than
  needed for sv.notEnoughPlayersRestartDelay raise TimeLimitReached(0),
  the time limit TimeLimitReached(−1). `GameLogic::waitToStart` (a hosted
  game; a local one plays at once). sv.timeBeforeRestartMap is only kept
  by the exe too. Not rewritten: the ranked minimums and overrides, the
  end-of-round delay's per-mode rules.
- **Votes** (manager `DAT_00a693f8`, Vote event 0x27, handler
  `0x00590230`, start / cast `0x0058f470`, end `0x0058fb90`;
  `game::Votes`, `HostedGame`): all skipped without sv.votingEnabled. Map
  (1, a map-list index not running), kick (2, a connected player) and
  mutiny (3, the voter's team's commander) votes; the eligible are the
  humans who haven't opted out (phase 8), a kick's target and a mutiny's
  other team left out (`0x0058f220`); fewer than sv.minPlayersForVoting is
  refused (7) except a lone player's map vote, another running vote (6);
  ceil(0x3f2aaa9f × eligible) yes votes pass it (`0x0058f340`); it ends at
  that, when all voted, or after sv.voteTime (1: failed, with the votes it
  lacked; 3: passed). A passed map vote runs the map 5 s on, a kick kicks
  ("KICKED", 0x15; the round's address ban isn't kept), a mutiny removes
  the commander. A choice is 0 yes, 1 no, 2 counted for neither (the vote
  record `0x00645b20` takes 0..2, `0x00645950`); one vote a player; the
  starter's is 0, and a client sends 0 or 1 (`0x0079d6e0` case 6). It ends
  early once yes + no reach the eligible (`0x00645bb0`); every event
  carries the yes and no counts (`+0x10`, `+0x11`). sv.teamVoteOnly isn't
  in BF2142.exe.

### Demos (`.bf2142demo`)

`demo.recordDemo <name>` (`0x004246fb` → `0x00432cc0`: `Demos/<name>
.bf2142demo` without an extension; "can't record demos as a client") has
the server (`0x004afcc0`) make connection 0x81 (`0x004ac920`: no player,
"DEMORECORD") and send it the join's data and steps (blocks 0 and 5, the
level; `0x004a7c00`, `0x004a7c80`, `0x004af710`), flushed into the file at
once (−1) then by the tick, and the marker (−2, 0); from then the send
loop (`0x004b02a0`) writes the connection's data payload to the file each
tick instead of the socket and counts it delivered at once (`0x0057ab30`).
`demo.stopRecording` (`0x004afdb0`) writes (tick, 0) and closes. The file
is one zlib stream (level 3) of a header (0x70, the time, the server's name
and two strings) and the records (`src/Net/Demo`, `HostedGame::recordDemo`).
Playback (`bf2142 --demo <file>`; `GameClient::playDemo`): a client with
no socket takes the records as they come due (`0x00516c80`: −1 at once,
markers skipped, the clock set by the first timed record or one over 100
ticks ahead), sends nothing and has no player. The header's two strings are the map (server slot 0xf4, `+0x3c`, the
map list entry's map) and the game mode (slot 0xf8, `+4`); records never
carry an empty payload (`0x00516950`).

- **The demo connection's ghost lengths**: the GhostManager's state length
  field (`+0x2140`) is 11 bits, 13 when it is built with a non-zero kind
  (`0x00582a50`): recording (kind 1, `0x004ac920` → `0x0057cec0(id ==
  0x81)`) and playback (kind 2, `0x004b4a80`). Only that field changes (the
  record's 12-bit length and the rest are constants). The rewrite writes
  and reads demos with 13 (`net::kDemoGhostStateLengthBits`).
- **sv.demoQuality** (1..10, setter `0x0041f80c`, 1 by default) sets the
  kind-1 connection's rates (`0x0057b3f0`): with f = (q − 1) × 0.11111111
  (truncated), 4 − (int)(f × −16) records a second, 0x104 − (int)(f ×
  −3836) bytes each and floor(f × 215) + 40 ghost updates a block: 4 / 260
  / 40 at 1, 20 / 4096 / 255 at 10 (`net::demoRates`). The demo connection
  is paced like a client and takes every ghost at priority 1.
- **AutoRecorder** (AutoRecorder.cpp; constructor `0x005854a0`, at the
  server's `+0x164`): with sv.autoRecord, the round's start (the game
  logic turning to playing, `0x004c03b0` → `0x00585860`) records
  `<GSModDirectory>/demos/auto_<%Y-%m-%d %H:%M:%S, every character but
  0-9 and _ as _>.bf2142demo` (`0x00585620`); the round's restart
  (`0x004ba0c0` → `0x00585920`) stops it and, unless the last one is still
  being handled ("dep: cannot call auto-recording hook for …"), logs
  "Uploading demo file … ..." and hands it to sv.autoDemoHook, run by the
  recorder's thread (`0x005853b0`, ShellExecuteExA "open" with the file
  in quotes). Once the hook is done (`0x005852b0`) the server sends game
  message 9 to everyone with sv.demoDownloadURL (its "{file}" replaced by
  the file's name, or the name appended) and the map. sv.demoIndexURL is
  only advertised (GameSpy keys 0x36..0x38). The exe keeps no count of
  demos (rotate_demo.cfg's file_limit is the hook's). `HostedGame`'s
  autoDemo; not rewritten: message 9 (its event isn't mapped), whether a
  map change (`0x004bc850`) stops it, setAutoRecordRounds.
- **Camera files** (`.bf2142cam`, `net::CameraKeyframe`, read/write): the
  same zlib wrapper, no header, a keyframe a tick (`0x005f8d30`, read by
  `0x005f91f0`): u32 tick, u32 mode (follow only when exactly 1), u8
  attached, u8 override (read as mode 0), then a free frame's quaternion
  x, y, z, w and position (zeroed when |p|² > 1e10), or a follow frame's
  object id, a byte and 5 floats. recordDemo writes `<name>-def.bf2142cam`
  only with a local player. The `demo` command-line option names a con
  file of demo.* options (`0x0043a3a0`: demomode playback/record/render/
  rendersound, cameramode, demofile, camerafile, the render settings,
  timedemo...).
- **Playback controls** (the demo interface, vtable `0x00928fa8`):
  toggleDemoPauseOrPlay (slot 0x5c, `0x004b32d0`: the game speed 0 or
  100), restartDemo (slot 0x48, `0x004b3190`: world and ghosts reset,
  `0x00582340`, the demo again from its start), shutdownDemo
  (`0x00404750`). `bf2142 --demo` binds them. Not rewritten: the demo
  camera and its controls (togglePlayerDemo `0x005fa440` / `0x005fa470`,
  toggleCameraDemo `0x005fa130`, adjustDemoFov), playing camera files,
  render and timedemo modes (`0x00432ee0`: timedemo_report.txt).

### The control-object record and prediction

The ghost block's record (written by `0x00580830`, read by `0x00580cd0`)
carries, after its 12-bit length:

| Field | Bits | Notes |
|---|---|---|
| last move | sign + 31 | the last move the server stepped |
| position | 3 floats | the control object's; the stream's reference for delta positions (`0x006874f0`) |
| ghost | 16 | the control object's ghost (a soldier, or the seat's ghost) |
| second ghost | 1 + 16 | `+0x20de` |
| vehicle | 1, then 1 + 16 | only for a soldier control object (`0x0057f060`): whether it is in a vehicle, and its ghost; a client whose own copy of the soldier sits elsewhere skips its own state |
| fresh or relative | 1 | 0: a fresh state, which the client keeps in the history slot (`0x0057f750` via `0x00580800`: 8 slots of `0x402` bytes at the ghost manager's `+0x20f4`); 1: written against a slot the client has (`+0x20f0`, while `+0x20fc` counts down; `0x0057f9b0`) |
| slot | 3 | `(+0x20f8 + 1) & 7` for a fresh one |
| groups | | every group of the ghost in mode 3, then the second ghost's (`0x0057f420` / `0x0057f720`, `0x0057f9f0`) |
| own state | 10 + n | the length, then the player's own state (below); a reader taking fewer bits skips the rest, more fails the block |

Without the record's ghost the client skips it all by the 12-bit length.
Mode 3 differs from mode 2 in three readers only; the others take the same
bits and apply them differently (the seat's `0x00605b00` skips its "is the
seat mine" check):

- **SimpleObject** (`0x005ff5d0` / `0x005ffec0`): the angular velocity
  (mask 8) as a wide delta in 1 mm/s steps (`0x00687510` / `0x006876c0`)
  instead of the delta3 in cm/s; written against a history slot (a
  relative record) the position, velocity and angular velocity are deltas
  from the slot's copy (`0x005fe230` stores them, 0x24 bytes each at the
  slot's cursor `+0x400`) instead of the stream's reference and zero. With
  the setting `OptimizeSimpleObjectNetworking` (`0x004237c0`) at 1 a
  relative record's Ground/AirPhysicsNodeInfo mask is fixed (`0x48600f`,
  `| 0x10d80` on the ground): not rewritten, the default is 0. No other
  shipped networkable reads the slots (the Soldier's, RotationalBundle's
  and Engine's `0x400` are mask bits).
- **Soldier** (`0x005fb8c0` / `0x005fcdb0` with the last argument 3; the
  fields come from `0x005fc1a0`): the velocity (0x100) in 1 mm/s steps
  instead of 1 cm/s, then three more node vectors: 0x200 the acceleration
  the next integration takes (node slot `0x74`, set back with `0x78`), 0x400
  slot `0x88` (`0x8c`; meaning not established) in 0.1 mm, 0x80000 the
  velocity change for the next tick (`0xc4` / `0xc8`) in 1 mm; the four
  angles as raw floats: the body's yaw (`+0x25c`), the look's twist ahead of
  it (`+0x278`), the twist still to catch up (`+0x280`) and the aim
  (`+0x270`); the leans' 16 bits instead of 10 (`+0x264`, `+0x260`: the
  eased throttle and strafe); the stamina's 7 bits instead of 5, then the
  sprinting bit (`0x005fb1f0`, PCO vtable `0x100` / `0x104`); mask 0x40000,
  -1..1 in 7 bits (the soldier's `+0x18c` interface, vtable `0x90` /
  `0x94`; meaning not traced). The server's mask is every bit but the
  ragdoll's.
- **RotationalBundle**: 16 bits an angle and speed.
- **A seat's occupant**: the seat hands the mode to the soldier's reader.

The player's own state (`0x00594cc0` writes, `0x00596ed0` reads):

| Field | Bits | Player field |
|---|---|---|
| a flag | 1 | vtable `0x210` (the reader calls `0x20c(1)`) |
| four flags | 4 | `+0x16a`, `+0x168`, `+0x169`, `+0x16b` |
| a value | 8 | `+0x14c` (vtable `0x228`) |
| the hit's direction | 1 + 8 | `+0x1d0`, -pi..pi; sent for two records after a hit (`+0x1d4`); a new one posts message 2/0x39 (the hit indicator) |
| the hit's strength | 1 + 8 | `+0x1d8`, 0..25, twice too (`+0x1dc`); handed to the control object's vtable `0x1b4(0)` slot `0x4c` |
| per weapon group 0-2 (with a PlayerControlObject on the control object's chain) | 1, then 5 + 2 + 2 + 2 + 11 + 10 (+ 8) | active (`+0x1e0 + g`); the weapon's index in the group + 1 (`0x005a98b0`), three 2-bit values (`+0x208`, `+0x238`, `+0x1fc`), the rounds + 1 (`+0x1e4`), the magazines + 1 (`+0x214`), and for a firearm whose interface `0x94c0` slot `0x30` holds (`0x005c1360`: its template's vector at `+0x2c8`, set by `0x00522125`, has y > 0) a 0..6 (`+0x22c`) |
| group 0's component `0xc506` | 1 + 8 | 0..1 (a template component of class `0x94e5` with its `+0x48` flag: slot `0xcc`) |
| spawn tick | 32 | `+0xdc` |
| man down | 1 | `+0xe0` |
| health | 1 + 8 | the soldier's hit points over maxHitPoints, signed |
| three flags | 3 | `+0x124`, `+0x19f`, `+0x1a0` |
| commander | 1 | `+0x10c` |

`src/Net/Networkables` reads and writes both modes and the own state,
`GhostLayout`'s `ControlState` the record's state; `RemoteWorld` decodes it
with the ghost's layout (`controlState()`) and keeps the eight slots, and
`HostedGame` writes it every tick as `0x00580830` does: a fresh state into
the next slot (its own copy kept, `0x0057f750`) while the client has
acknowledged none (`+0x20f0` -1) or the countdown (`+0x20fc`) has run out,
then 5 (`DAT_00a00ce4`) relative ones against the acknowledged slot. The
acknowledgement is the notify layer's: a delivered packet that carried a
fresh record makes its slot the client's (`0x005830e0`, checking the
record's ghost; `GameServer::onNotified`). A new control object starts
from fresh records again; a fresh record overwriting the acknowledged slot
drops it.

The firearm's 0..6 in the own state (per weapon group, `+0x22c`) goes only
for a firearm whose template's `+0x2c8` vector has y over 0 (`0x005c1360`):
that field is `ObjectTemplate.maxResetSpeed`'s (`0x00521fab`, setter
`0x005220a3` → `0x00522125`, the only writer, whatever the class). No
shipped firearm sets it (only eu_sentrygun's RotationalBundle
EU_Sentrygun_gun has `0/5000/0`), so it is never sent
(`net::firearmSendsHeat`).

After reading the record, `0x00580cd0` hands the stream, the ghost, the
last move and the server's tick to the local player's interface `0x1d4c2`
(`DAT_00a1b5d8` slot `0xc`, then its slot `0x10` with 1/30 s,
`0x005811ee`-`0x0058122a`): the moves the server hasn't taken yet run
again on top of its state. The rewrite's client does that with the shared
soldier simulation (`src/Main/Prediction`, `SoldierPrediction`): each move it
sends (one per simulated tick, `GameClient::simulatedTick`, as
`0x004b3df0` sends after each tick) is stepped once with
`game::SoldierControl` and `physics::SoldierBody` against the round's
ground and static collision, as `GameWorld::stepSoldier` steps the
server's, and remembered with the state it left. A record for move L puts
the state remembered for L where the server's differs (every field mode 3
carries; the timers and contacts it doesn't carry stay the client's own)
and steps the later moves again. The soldier is drawn and seen from the
prediction. The moves a move-to-input conversion shares
(`net::soldierInputFromMove`) make the server and the prediction step the
same input.

The exe's replay is the game client's interface `0x1d4c2` (the client
object's `+0xa8`, `0x004b35b0`; vtable `0x00928fa8`) slot `0x10`,
`0x004b5000`, given the stream, the record's ghost, its last move, the
server's tick and 1/30 s. It keeps the move L in the local player's move
list (player slot `0xa8`; `0x00583710` drops the moves before L and keeps
L at the front), and, with the record's object found (`0x00678d70`) and
prediction on (`+0x55`), sets the clock to the server's tick
(`0x006660b0`), then for every move left in the list, L's own included:
the clock one tick on while the game logic's state (slot `0x164`) is 1
(`0x006660e0(1)`), the move decoded into the player's input
(`0x00583820`, `0x005838d0`) and handed to the player (game logic slot
`0x160`, player slot `0x44(input, 1/30, 4)`; not in state 4), and one
tick of the control object's tree: each object's update (`0x0057b7f0`),
the physics nodes' move (`0x0057b960`: PhysicsNodeManager `0x00a82514`
slot `0x1c` per node), the ObjectManager's slot `0xa4`, the
ResponsePhysicsManager's (`0x00a82518`) slot `0x20` and each response's
slot `0x1c` (`0x0057ba00`). So the original steps whatever the control
object is, a vehicle's tree included. Taking L's move again means the
record's state is the server's before move L by this reading, while the
rewrite's server (`HostedGame`, whose `lastMove` is the move its round last
stepped) and client agree on "after L"; which the original server writes
(`0x00580830`'s `+0x20e4`) wasn't settled.

**Vehicles** (`VehiclePrediction`): the driver's moves step the client's
own copy of the vehicle, `GameWorld::LoneVehicle` (`makeLoneVehicle`: built
as `addBodies` builds a round's body, `buildBody`; `stepLoneVehicle`: the
seat's inputs from the move, `GameWorld::driverInput` as `stepSoldier`
makes them, then `stepBody`'s engines, wheels, drag, integration, legs,
ground and water, the static objects' meshes (`collideMover`) and the
response; not the other bodies, and no collision events, damage or contact
effects). A record of the driver's seat (the root's SimpleObject in mode
3: position, orientation, velocity, angular velocity) is put on the state
remembered for its move, and the later moves are stepped again; the
engines' and wheels' own state (rpm, gear, springs, wheel spin) isn't in
the record and stays the client's. `bf2142 --connect` runs it: every
move sent steps both predictions; a record whose root group is a
SimpleObject with `record.vehicle` set starts (or reconciles) the car's,
a soldier's record the soldier's, and the vehicle ghost
(`record.vehicle`) is drawn, and the camera follows it, at the
prediction's position and orientation.

Measured on 127.0.0.1, a car (`NetHost_VehiclePrediction`: eu_fav on
flat ground, the client driving 10 s at full throttle, steering both ways,
then braking): 304 records (257 of them relative to a history slot), the
prediction corrected by 0.9-1.2 cm on average and at most 11 cm, 143 m
driven; the prediction ran up to 1.7 m ahead of the host's car (the moves
in flight).

Measured on 127.0.0.1: the in-process test (`NetHost_PredictionAndSession`:
a client walking, turning, sprinting and jumping on a slope for eight
seconds) corrects the prediction by 1.3 mm on average (at most 9 cm,
5 of 240 records over 1 cm) and sees the host's bot within 0.1 mm and
0.002 of its health at each update's tick; two processes (`bf2142 --host
... --bots 6 --simulate 90`, `bf2142 --connect 127.0.0.1 --play 2 --walk
--simulate 40` on Cerbere_Landing, the client walking, turning, sprinting
and jumping with `--walk`): 1189 records, the prediction corrected by
0.1 mm on average and at most 1.7 cm (one record over 1 cm); the six bots'
positions as the client saw them equal the host's to the printed
centimetre, their health 1.00 on both. While the client is still loading
its level its game doesn't tick, so its moves go up only on the
connection's own timer (every 200 ms) and the server steps the ticks in
between with the last move held: those records correct it by tens of
centimetres until the client's loop runs. `bf2142 --connect ... --verbose`
prints any correction over half a metre.

### Asking servers directly (QR2) and the LAN search

Each server answers GameSpy's query and reporting protocol 2 on its query
port (29900). The parser (`0x0071a7d0`) takes datagrams starting `FE FD`,
then a type and a 4-byte instance key:

- type 0, a query (`0x00719de0`): three key lists (server, player, team: a
  count, `0xff` for every key, and the key ids) and an optional flags byte
  (bit 0 asks for split replies, `0x00719b30`). The reply (`0x00719af0` →
  `0x00719890`) is 0, the key, the server keys' values as zero-terminated
  strings (an empty one for a missing value, `0x007194c0`), then for player
  and team keys a big-endian 16-bit count and every player's values. Asked
  for every key, the server sends names and values in turn and a 0.
- type 2, an echo: answered with type 5, the key and up to 32 bytes.

The keys are qr2's registered ones (the table at `0x00a08238`): 1
hostname, 2 gamename, 3 gamever, 4 hostport, 5 mapname, 6 gametype, 7
gamevariant, 8 numplayers, 9 numteams, 10 maxplayers, 11 gamemode, 12
teamplay ... 19 password, 20 groupid; player keys from 21 (player_, score_,
skill_, ping_, team_, deaths_, pid_), team keys 28 and 29.

The menu's LOCAL page lists `serverListLAN`, the server browser manager's
"lan" browser (made with the others by `0x00469580`; type 0, 200 servers).
Its search (`0x00467d40` → `0x00715900` → `0x007249c0`) sends `FE FD 02 00
00 00 00 00` to 255.255.255.255 (`0x0058ccb0`) with SO_BROADCAST on every
port from 29900 to 29950 (`0x74cc`-`0x74fe`), and every address that
answers within 2 s (`0x00724440`) is a server, which the browser's query
engine then asks for its keys. `src/Net/ServerQuery` has both sides; the
rewrite asks the standard keys among those `0x00467d40` lists (its others
are the game's own, from 0x35 on, whose names aren't known). `bf2142 --lan
<address>` sends the search elsewhere (127.0.0.1 finds the servers on this
machine). The menu has no direct-IP join: `Multiplay.get/setConnectToIpHost`
and `...Port` exist (the profile's `GSConnectToIpHost`/`Port`, `0x0046c030`
-`0x0046c120`) but no page of the movie calls them; the original joins by
address only from the command line (`+joinServer`).

## Gameplay sound

`src/Sound/Game` is the executable's side of sound (the engine itself,
BF2142Audio.dll, is `src/Sound/SoundEngine`); `src/Main/GameAudio` feeds it
from the round once a tick.

**The sound manager** (vtable `0x009bf168`, `SoundManager`). Its update
(`0x007f9d50`) takes the camera each frame: the listener's position,
forward, up and velocity (engine slots 11, 13, 16; the engine keeps
pointers to the manager's copies), whether it is inside a Titan (slot 18),
the indoor level of the listener's spot (slot 20, `0x007fa100`), then the
engine's update (slot 24). `0x007fa100` casts two rays up (from
x +- 0.4, z +- 0.3) and one down, `sound-raylength` (15 m) long; with all
three hitting, the room's height h (the floor's depth plus the ceilings'
mean, at most 15) gives `sound-maxreverb` (0.5) x h / (0.98 h + 0.02 x 15),
else 0; `0x007fa320` (slot 0x98) is "outdoors": that is 0. The rewrite casts
them through `GameWorld::lineOfSight` (a bisection for the distance) when the
listener has moved half a metre or every 15 frames.

Muting (slot 0xbc, `0x007f9af0`) has five reasons; each update
(`0x007f9f50`) moves every volume group's fade (`+0xe0 + group x 0x14`:
user volume, current, target, rising step, falling step; steps 1/15 from
the constructor `0x007f9040`) a step towards its target and sets the
engine's group volume to user x current. Reason 0 is a one-update pulse
(groups 0-3 at 0.25), 1 silences groups 0-3, 2 groups 0 and 2, 3 (a
voice-over playing, asked each update by `0x004cdfb0` with the help voice's
state) brings group 0 and group 4 to 0.125, 4 (a first-person weapon with
`sound.muteOnFire` firing) multiplies group 0 by `sound-mute-level-1p`
(0.4), with group 0's steps `sound-mute-fadein-1p` / `-fadeout-1p`
(0.333 / 0.167) instead of the `-vo` ones (0.0667). Group 0 is the world's
effects, 4 the first-person weapons (`0x005db760`), 1 music and interface,
2 voice-over, 3 the other menu sounds.

`Sound.addSound <name>` (`0x007fae20`) creates `S_Game_<name>` (left
active for the script's lines that follow, Sound/GameSounds.con) and its
sound; `Sound.addTrigger <template>` (`0x007f95a0`) gives the template to
the engine as an ambient emitter (slot 28; at quality level 2, the lowest,
only the first), which the levels' `AmbientObjects.con` do for their
`S_AmbientDefault` and area sounds. When a level has loaded (the manager's
event 5/3, `0x007fb6c0`) it runs `Sound/GameSounds.con` (`0x007fbb90`) and
`Sound/AudioTweak.con` (the templates' `Sound.tweakTemplate` multipliers).

**Object sounds.** In BF2142.exe each object's sound components own
instances of their `S_<owner>_<event>` children (made by `0x0060e910`, see
`SoundManager::registerTemplateType`) with the object as their transform.
The rewrite's world doesn't carry those components, so `GameSounds` keeps
the instances by owner: one-shots get a sound of their own that is
released at once and plays out where it started (`Sound::detach`); loops
(engines, projectiles) are kept while their owner refreshes them each tick,
their volume and pitch envelopes read at the owner's value. Templates
without files (most generated ones) are skipped. `BF2142_SOUND_LOG=1`
prints each sound triggered.

- Weapons: DefaultSoundComp (vtable `0x00971b48`) has eleven slots from
  `+0x10` in `0x005db760`'s order: Fire1P, Fire1P_Outdoor, Fire3P,
  BoltClick, TriggerClick, SwitchFireRate, Reload1P, Reload3P, Deploy1P,
  Deploy3P, Zoom. "First person" is the owner seen from its own eyes
  (`0x005c2d80` slot 0x1bc). Fire (`0x005dbdc0`): Fire1P_Outdoor when the
  template has one and the listener is outdoors, else Fire1P, in first
  person; Fire3P otherwise; with muteOnFire (on by default: the writer
  `0x005dbc00` saves only a 0) reason 4 until the trigger is released
  (`0x005db6c0`; the rewrite: two ticks without a round). With one sample,
  an automatic burst plays it looped (`+0x43`) and maxSoundsPerBurst
  (`+0x42`) limits a burst's sounds; with four, samples 1-3 are the start,
  loop and end (the rewrite plays one per round). Reload (`0x005dbfe0`):
  Reload1P / Reload3P (two- and three-part reloads by the reload events
  4-7; the rewrite plays the first). Deploy (`0x005dc1d0`): Deploy1P /
  Deploy3P, not within 9 ticks of the last. Zoom (`0x005dc250`) and
  SwitchFireRate (`0x005dc290`) are first person only. `GameWorld` queues
  a `WeaponSoundEvent` for each round (on foot and from a seat's gun), each
  reload's start and each weapon taken in hand (`takeWeaponSounds`).
  CountdownSoundComp adds ReloadTimeTeam<1-2>_<1-5> (`0x005dc760`), played
  at `sound.addReloadTimeSound <n> <seconds>` before a missile silo is
  loaded (the silo's team).
- Engines: the engine audio (`0x007fe050`, vtable `0x009bf914`) plays an
  Engine's Idle (stop type 2, its start-stop sample), Rpm1 and Rpm2 (made 3D,
  looping, min distance 3 by `0x007fdd10`) while it runs (`0x007fe2c0`),
  their envelopes read at |rpm| x 0.8333 (`0x007fe740`, the old-style
  audio; `audio.newStyleAudio`'s throttle crossfade and sway `0x007fe430`
  aren't done). The rewrite sounds the engines of vehicles someone is in.
  Load and the parts' RotationRpm aren't played.
- Projectiles: DefaultProjSoundComp's Looping (`0x005ea930`) follows a
  projectile in flight.
- The round's own sounds (footsteps, ragdoll impacts, `takeSounds`) and
  the effect bundles' Start sounds (explosions, impacts, muzzle flashes)
  are one-shots where they happen.
- Not done: occlusion (the `sound-obstruction-*` tweaks; the engine's
  `ISoundOcclusion` hook is there, the ray test isn't), the inside-Titan
  listener flag, PlayerControlObjects' Ambient / AmbientDamaged loops, the
  wind/speed sound of `0x007f9ea0`.

**Voice messages.** The game logic's message manager (0x26b8 bytes,
`0x004d99c0`, at game logic `+0x170`) is made by `0x004cdab0` once the
level has set the teams' languages: team prefixes
`sound/vo/<language>/` (with a GSLanguage other than English the team
speaking it, else the English one, takes the player's language), the help
prefix `sound/vo/<GSLanguage>/`. It runs `Sound/VoiceMessages.con`:
`addMessage` (`0x004d8af0`, at most 300), `addRadioVoice <voice>
<radio sample> <text> [<local sample> [<flags>]]` (`0x004d8b90`: at most 8
voices; the sample goes to both teams' lists with their prefixes, and entry
message x 8 | voice of the table at `+0xec` holds its first sample and
count, so a voice's lines of a message must come together),
`addRadioFilter` (`0x004d8de0`, no prefix), `addHelpMessage`
(`0x004d8e40`), `addEmoteTrigger` (`0x004d83e0`), `setVolumes <vo>
<filter>` (`0x004d7860`). Then `0x004d8f00` builds the sounds: per team a
2D radio sound of every radio sample (group 2) and, except on a dedicated
server, two 3D local ones (min distance 3, reverb 1), the filters' sound,
and the help sound (group 2); the radio sounds' volume envelope (vtable
`0x0092aad0`) is the VO or filter volume.

A radio message the client hears (`0x0042e0c0`) has the sender's voice:
"commander" (filter "commander") for the commander, else "squadleader" or
"grunt"; with no sender (an automatic line) the commander's. A sender on
foot who isn't the listener speaks it in person when he can
(`0x004d8110`: a local sample, the listener's team unless flag 1, within
15 m (`+0x26b4`) unless flag 2; one of two speaker slots, a silent one or
the farther speaker's, `0x004d7a30`); else, from the listener's team only,
it plays over the radio of the sender's team's voices (`0x004d7950`: one of
the message's samples at random, `0x004d78d0`; the filter's static looped
over its length). `GameAudio` plays what `GameWorld::takeRadioHeard` hands
the local player. Help voices (`0x004d8280`) play unless group 2 is at
0.05 or less; the HUD's help pop-ups that would ask for them aren't done
(coverage gap 4).

**Automatic radio messages.** The Radio template component (class
`0xc4b6`, interface `0xc4b7`; 24 properties, `radio.*`) is read through
its object part (vtable `0x00988f30`, interface `0xc4b5`, at object
`+0x7c`, made by the template component's slot 6 `0x00631b20`; one-shot
flags `+0x10` fail, `+0x11` use, `+0x12` repaired, `+0x13` destroyed,
`+0x14` damage). The server sends from no one on TCServerTeamMessage (0xd,
game slot 0x5c, `0x004a5cf0`) to the object's team (`0x00656910`), the
other team the enemy's message: damage (`0x00656c90`, from the armour's
hit points going down, `0x00655460`; once until repaired), destroyed
(`0x00656d10`, event 0x18: friendlyDestroyedMessage, which is the same
field as destroyedMessage, and enemyDestroyedMessage; arms repaired),
repaired (`0x006571d0`, event 0x19, when armed; re-arms the others),
spawn (`0x00657120` from createObject `0x004ab8a0`) and fire
(`0x00656dd0`, slot 0x54: from the firearm's batch fire `0x005c3cb0` in
fire state 4, every shot, no timer: friendlyFireMessage to its team,
enemyFireMessage to the other; only the Titan missile silos' firearm has
them; fire.radioMessageDelay is the laser painter's, `0x005e1730`, its
"laserpaint" call's delay).
Team changes (`0x00656e80` from a control point's setTeam `0x005cc320`)
play for the local player only (game slot 0x60, `0x0042e820`): to no team
neutralTeamChangeMessage if it was the local team's, else friendly if the
local player is on the new team and enemy if not (SiloSyncEvent's "_N"
suffix, `0x00514030`, not added). The weapons' failMessage (a refused pull
of the trigger, `0x005c2ee0`) and useMessage (the shot) are the player's
own on TCLocalTeam (10, game slot 0x50), once until a reload re-arms them
(`0x005c58a0`). Takeable / not takeable: a control point's
allowCaptureByTeam setter (CP slot 0x34, `0x005bcd00`; every set, from the
scripts' cp_setParam `0x00569510` on the host or the ghost's mask 8 / 0x10
on a client, `0x006084b0`) tells the point's Radio (slots 0x60 / 0x64,
`0x00656fc0` / `0x006567b0`) when the point needs supply groups for that
team: for the local player (none on a dedicated server), team 1 or 2,
friendlyTakeable / enemyTakeable or friendlyNotTakeable /
enemyNotTakeable as the local team is that team or not; with radio.delay
(an int, template `+0x218`) over 0 it goes as timed event 2/0x52 that many
seconds on, whose handler (`0x00758cd0`, the HUD manager's `0x00756b60`)
plays it as is, else at once. Only Assault Lines' points have them. The
SiloSyncEvent's "_N" (`0x00514030`, `sprintf("_%d")` into the Radio's
`+0x18`) is appended to the team-change messages. `GameAudio::autoRadio`
(objects, takeable), `GameWorld`'s fire (weapons) and the silos' launch.
Not rewritten: a joined client's takeable radio (its ghost's bits aren't
applied to a world), the "_N" suffix, destroyedGameEvent (no reader).
The
closeToIndication* and indicationDistance* aren't radio: the server's
0.2 s scan (`0x0059a6c0`) posts events 0x3e / 0x3f as a player comes
within or leaves them; not rewritten. spottedMessage is the HUD's and the
bots' (game slot 52, `0x0042e9d0`).

**Cost** (`bf2142 ... --simulate N --sound` prints it; ASan build): 20 s on
Camp_Gibraltar 32 without bots, 0.27 ms of sound work a tick; 40 s on
Cerbere_Landing sp 16 with 8 bots (a walker's AA cannon firing, three
vehicles' engines, up to 30 samples playing), 0.97 ms a tick, under 3% of
the 33 ms tick. 1,822 files are loaded (10 MB of static buffers), the
voice-over's included.

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
| 200 | DEBUG_DATA | 0x659 bytes (any other size is refused, `0x10002170`): opens the routing log if it isn't open and keeps the bytes at instance `+0xb34`, the player names the logs print |

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
| 4, 5 | none | start or stop logging this player's voice to `VoiceTraffic_Player_*` (`0x10007060`, `0x10007230`; only while the routing log is open, and 4 only with quality 2) |
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

Not rewritten: the DLL's debug logs (`mods/bf2142/Logs/Voip/`:
`VoipServerRouting_Instance_*`, written by `0x10006610` after every game
server packet once DEBUG_DATA opened it, and `VoiceTraffic_Player_*`).
Packets 4, 5 and 200 feed nothing but those logs: nothing else reads the
names DEBUG_DATA stores. The rewrite accepts and drops them
(`VoipServerInstance::handle`); `onLog` carries the DLL's console lines.

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

Lists with commands (`setListNodeConCmd <event> <command>`: 1 a click, 2
a double click; the squad and commander menus' lists) take clicks on their
rows: the row is selected, the column under the pointer kept (the column
whose left edge is the nearest one left of it) and the commands run; a
second click on the same row within half a second runs event 2 as well.
The game reads the clicked row's cells back (`0x007dd3b0`), including
cells past the drawn columns (`HudManager::listCell`, `lastListClick`).

Every node but a split places its children from its own position: the
commander screen's tab pictures carry their buttons, labels and lists
(`CommanderToolsTabSelected` at 3,519 with its button at 0,0), as do the
transforms.

### Name tags

The 3D name tags and object icons (friendly names, health bars, control
point icons over the world) are not HUD nodes: RendDX9.dll's
NametagManager (`dice.hfe.geom.NametagManager`, vtable `0x101d6c80`, its
frame `0x100f3490`) keeps them and its NametagRenderer draws them
(`0x1016c1e0` the bars, `0x10168890`.. the icon passes, `Shaders/Nametag`,
the font `Fonts/BF1942.font`); BF2142.exe only looks the manager up
(`0x0043ab00`) and hands it object tags (slot `0x30`, `0x100f40d0`, from
the templates' Nametag component, `0x0063b760`). `src/Main/Nametags`
rewrites them as HUD quads under the HUD:

- **Players** (`0x100f0e60`, `0x100f0310`): a tag per other player at its
  soldier's head (0.6 m along its up over its origin, more with distance:
  `+ min(d × 0.6 × 0.05, 3)`) or over its vehicle (`0.35 × height +
  0.45`, nametags.vehicleNametagOffsetFactor / Offset), with "  [n/m]" for
  a vehicle's occupants. Friends: full to 80 m, gone at 120
  (maxFriendlyDistance), out to 500 m while aimed at (faded in over 0.7 s,
  out over 1.5 s); down, they fade over 0.6 s (deathFadeOutTime), or 14 s
  (manDownFadeOutTime) for a local medic. Enemies only while aimed at,
  after a delay of their distance's share of 300 m (the player cull
  distance) and gone past 285 m; never while down. Someone in the local
  player's vehicle has none. Colours by ColorIndex: 1 enemy, 2 team, 3
  squad (squad members stay on screen at the edge of a circle of 0.65,
  glueToEdge, with "[d m]"), 4 a call.
- **Aimed at** (`0x100f0d30`): the crosshair's target, a ray 300 m along
  the view every 0.5 s (`0x100ef5b0`; every occupant of a vehicle it meets,
  `0x100f2ec0`), or, looked at every 0.25 s, the tag's point within the
  0.99999 cone and in sight. The rewrite's ray meets soldiers as 0.6 m
  spheres and vehicles as 2.5 m ones and asks `GameWorld::lineOfSight`.
- **The bar** (`ICArmorBar`: `bar_base` under `bar_filler`, filled from
  texel 1 to 45 of 64, `createBar ICArmorBar 1 45`): a hurt friend's
  health under half for a local medic (the Assault kit), a damaged
  vehicle's armour for an engineer.
- **Control points** (object tags, `0x100f1330`, `0x100ef960`): the
  templates' `Nametag.*` (showItem, 3dMapIconOffset 0/5/0 on the shipped
  points, showDistance, teamSpecific, maxDistance / maxEnemyDistance 1200
  by default), the icon `Menu/Nametag/<Neutral|Pac|EU>/miniMap_CP` 32 × 32
  with "[d m]" under it, alpha `1 − d / max`.
- The distance shown changes only by more than 0 / 2 / 5 / 9 m below 25 /
  250 / 400 m / beyond (`0x100f0be0`).

ColorArray for player and object tags (types 0 and 3) is the table at
`0x102112f8` the NametagManager's constructor fills (`0x100f24c0`):
white, red (0.87, 0.094, 0.094), blue (0.254, 0.552, 0.89), green (74,
227, 37 / 255), yellow (1, 1, 0). Guesses: the soldier's origin 1 m over its feet, the vehicle height (4 m; RendDX9 asks
the render object), the bar's place. Not done: ranks and veteran icons
(`ICRank`, `ICVeteran`), the radio calls' icons (medic, ammo, repair,
revive: events 0x33-0x37 within maxCallDistance 150), orders and requests
(TTOrder / TTRequest), the Titans' and other objects' `3dMapIcon` frames,
the fog factor on enemies, the spectator's rules. The HUD's object marker
nodes (`createObjectMarkerNode`) are the AA and gunship lock-on markers;
see "HUD nodes: transform lists, effects, markers and the map menus".

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
  spawn points (`Ingame/Minimap/Icons/spawn_(Un)Selected.dds`), and of
  the team's mobile spawn points: the vehicles carrying a SpawnPoint
  (APCs, the AT and IFV spawn vehicles; `GameWorld::spawnVehicles`), at
  the vehicle and moving with it. The original's spawn map (`0x0078e830`)
  walks every spawn group the spawn manager offers the team, a vehicle's
  among them, with the same icons (+0x9f0 unselected, +0xa00 the selected
  group at +0xa48; the squad and inactive variants +0x9f8/+0xa08 and
  index 0), 32 px; clicking a vehicle's picks it
  (`GameWorld::setSpawnVehicle`, spawned into its free seat);
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
  `explosionConeAngle` (`0x005a6370`). A bone out of sight counts half
  when the object the last blocked ray met (`0x006f2d20`'s out, kept
  across the rays and bones; it or the first parent with armour, `+0x48`)
  is destroyed by what the blasts have dealt it so far: `0x005a8900`
  records each object's damage (after the material cell and `0x005b7290`)
  in a list that `0x005a8d00` applies and swaps each frame (`0x00a6aadc`,
  `0x00a6abcc`, index `0x00a6aacc`), and its hit points (armour slot
  `0x3c`) are at most a recorded amount. The objects are handled before
  the soldiers (`0x005a9090` puts soldiers, `0x9493`, aside), so a blast
  that blows a vehicle away also reaches the soldier behind it (the
  rewrite keeps the one blast's list).
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
- **Other objects** (`0x005a9090`): a ray from the blast to the object
  (`DAT_00a85220` slot `0x68`); something else first (or the object
  without armour) halves the blast, and stops it when the object's radius
  (slot `0x90`: its geometry's and its children's) is under a fifth of
  the blast's (`*0x00a6ab24` = 5). The object's radius (`0x006f6b90`):
  its collision mesh instance's (slot `0x74`, `0x006dedb0` on the
  instance's geometry: the first sub-geometry with a LOD, its LOD of the
  lowest collision type, that LOD's radius `+0xc0`; the state `+0x14` is
  not read) plus slot `0x94` (0), or, farther, a child's: its offset's
  length (slot `0xdc`) plus its own radius (a child's instance is the
  geometry its `collisionPart` names). `Body::objectRadius`.
- **The push on objects** (`0x005a66e0`): roots with a node that are not
  major actors (slot `0x1c4`), the blast's own object excepted (its
  armour, `param_6`). Without armour the mod is 10000 / max(mass, 100) and
  the most 500; a wreck uses `wreckExplosionForceMod` / `Max` (`+0x124`,
  `+0x128`, defaults 1 and 100). The direction is the object's axis
  nearest the line plus the line (y only when strictly nearest). It wakes
  the node and goes in through slot `0xa4` at (blast − object) /
  max(distance, 1) from the object, itself over max(distance, 1); a
  soldier's node (`0x00993aa0` slot `0xa4` → `0x7c`) takes it as
  acceleration, a tenth of it under water (the node's `+0x5c` over 0, set
  by the ground pass `0x006c28c0`). `game::blastPush`.
- **The torque pair** (`0x005a66e0`'s `bVar5`): when the projectile has a
  detonation component (`0xc54d`, the object's slot `0x50`; the
  Default/ReplenishDetonationComp at `+0x10`, `0x005e91b0`) whose slot
  `0x4c` (`0x005e89e0`: the template's `+0x44`, `explosionDamage`) is below
  0, the force is 1 × 20 × `explosionForce` × share (no falloff, no mod;
  the tenth in water, the maximum, the soldier's random factor and speed
  as above), along the nearest axis alone (the line is not added,
  `0x0043dbd0` skipped). After the wake, instead of the spread push it
  goes in as three slot `0xa4` calls, unspread: with a the object's x axis
  (matrix row 0) turned to the blast (negated, `0x00562dd0`, when the line
  runs along +x) and f = force / 2.9 (`*0x00a6aa94`) × max(the box's width
  across x, 2) (slot `0x9c` → `0x006f7b10`, the box `0x006f7360` from the
  mesh's slot `0x78`, `0x006deeb0`: every part's and LOD's boxes; its
  `+0x24`, `0x00561d30`; `*0x00a6aa98`): (0, f, 0) at a, (0, −0.5 f, 0) at
  −a (`*0x00a6aa8c`) and a × f × −0.5 at the centre (`*0x00a6aa90`,
  `0x006ac600`'s zero). No shipped detonation has a negative
  `explosionDamage`. `game::blastTwist`.
- The lag-compensated positions (`*0x00a6ab48`, node slot `0x178`):
  on the server (`DAT_00a06884`, set from game logic slot `0x144` by
  `0x006fac60`) a hit is tested against where the target was the
  shooter's latency ago (`0x006b4ee0`): `lagComp` override
  (`*0x00a83e48`, −1: off) when above 0; for a remote client (player slot
  `0x60`) its ping × 0.5 (`*0x00a83e3c`) / 1000 plus 0.05 s
  (`*0x00a83e40`) or, with `*0x00a83e2c` set, its interpolation (slot
  `0x194`, ms); for a local one `0x006b4e70`: an input value (index 8,
  `0x00696db0` → `*0x00a83e4c`) with `*0x00a83e30` set; at most 0.3 s
  (`*0x00a83e44`). It only matters for remote clients' shots; the rewrite
  has no server for remote clients (its local game is the local player and
  bots), so nothing is rewound.

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
the trigger decays toward the floor (`0x005dedb0`).

**Soldier deviation** (hand-held firearms' `SoldierDeviationComp`, class
`0x94e1`; the object's update `0x005de980` each tick with the holder's
input; `game::SoldierDeviation`). The template adds `setTurnDev max pitch
yaw decay` (`0x005de3c0`: `+0x2c`, `+0x20`, `+0x24`, `+0x28`), `setSpeedDev
max throttle strafe decay` (`0x005de3e0`: `+0x3c`, `+0x30`, `+0x34`,
`+0x38`), `setMiscDev max add decay` (`0x005de400`: `+0x48`, `+0x40`,
`+0x44`), `devModStand/Crouch/Lie` (`+0x4c`..`+0x54`) and `devModZoom` /
`minDevModZoom` (`+0x58`, `+0x5c`), all modifiers 1 by default. The
input's bits pick the parts: PIYaw (strafe, bit 0) and PIThrottle (bit
3) the speed's (a fixed step each while above 0.01, held at its max,
decaying without), PIMouseLookX / Y (bits 4, 5) the turn's (their size
times the factors less the decay); the misc part grows while its flag
holds (the rewrite: in the air; eu_ar_rifle's `7 7 0.14` puts a jump at
the maximum). The deviation = (minDev × minDevModZoom + misc + turn +
speed + the firing's build-up) × devModZoom × the stance's (zoom
modifiers 1 when not zoomed). Rounds stray by it as above, and the
crosshair (`0x007be770`) spreads by it. Its `+0x44` extra (a condition of
the holder's slot `0x1a0`) is left out.

**Gun sway** (`DefaultGunSwayComp`, class `0x94f6`, object `0x005e0640`,
update `0x005e0da0`; `game::GunSway`): it moves the aim by nudging the
holder's look inputs, PIMouseLookY on the pitch (`0x005e09a0`) and
PIMouseLookX left-right (`0x005e0ba0`). With `gunSwayUseZoom` only while
zoomed. Each axis, every `UpdateRate` seconds, takes a step of `Max /
Steps` less its smoothing: `Factor × ZoomModifier × step × dt × the
stability × ±1` goes on the input when the player's own is smaller; the
swing turns at `Max` + an extra (`RandomMin..RandomMax`, drawn at the
ends), and with `DirModifier` / `DirThreshold`'s draw keeps on to
2 × (Max + extra) before starting over (the left-right pass writes the
pitch's keep-on flag, as `0x005e0ba0` does). The smoothing (`0x005dfc20`:
`SmoothFactor`, `SmoothMax`, `SmoothThreshold`) slows the steps near the
ends. Holding the sprint key (PISprint) while zoomed with the scope
stabilizer (an unlock's `ability.hasScopeStabilizerAbility`) steadies it
(`0x005e0750`): the stability falls to 0 over `stabilityTime` and back
over `stabilityRecoverTime`, held at most `stabilityDuration`
(`stabilityDurationRecoverTime` to recover).

**Unlock abilities.** `ability.setMaterialDamageMod <material> <mod>`
(`0x0054eadc` → the Ability's slot `0x94`) scales the damage a projectile
of that material does to the soldier (the assault's armour unlock: 0.75
for twenty bullet materials); `ability.addExtraAmmoItem <item> <mags>
<rounds> <flag>` (`0x0054ebed`) adds magazines and rounds to an item of
the kit (`as_handgun 3 0 0`, `unl_grenade_frag 0 1 1`; the list's
consumer and the flag aren't traced, the rewrite adds to nrOfMags and
magSize).

**Lock-on weapons** (the target components: `MultiFireArmTargetComp` on
the AA and AT launchers, the walkers' and gunships' guided guns; template
`0xc50c`: `target.targetSystem` `+0x80`, `maxDistance` `+0x78`,
`lockAngle` `+0x88`, `lockDelay` `+0x8c`, `loseLockDelay` `+0x94`). The
component's object update is not decoded; the rewrite's rule from those
settings (`GameWorld::weaponLock`): the enemy object carrying the system's
target type (TSHeatSeeking: TTHeat; TSLaserGuided: TTLaser) nearest the
aim within lockAngle, maxDistance and sight is locking at once and locked
after lockDelay in the cone; out of it for loseLockDelay it is lost. The
HUD's object markers (`Bf2ObjectMarkerNode`) show the state.

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
- **Abandoned vehicles:** the spawn (`0x005aeb80`) hands a spawned
  PlayerControlObject root (interface `0xc4c5`) its spawner (slot `0xc0`,
  `+0x264`), the spawner's `timeToLive` (slot `0xa4`: `+0x25c` and the
  countdown `+0x258`; 60 by default, 30 to 999 in the levels) and the time
  (slot `0xb8`). On the server a check (`0x005c8ba0`, event `0x1d`) runs
  half a second on and then: in use (a seat taken) again in 5 s; within
  the spawner's `distance` of it, again in 5 s with the countdown full; a
  living soldier on foot within 3 × its `armor.explosionRadius`, again in
  5 s; otherwise every half second the countdown loses 0.5 and, once out,
  the vehicle 0.5 × the spawner's `+0x2f0` (50, which nothing sets) hit
  points (0.5 without the spawner) through the game logic's damage,
  until it is destroyed. `GameWorld::stepAbandoned`.
- **holdObject** (`0x005af230`'s end; operation_clean_sweep and the_hell):
  the spawner holds its last spawn at the spot (its matrix and
  `spawnOffset`; the node's motion stopped, slot `0x148`) until its first
  Engine (`0x005ad420`) turns at 0.1 or more (`+0x25c`); without an Engine
  or wrecked it lets go at once; letting go puts the node to sleep.
- **The spawn spot's test** (`0x005aeb80`'s first half): the template's
  collision mesh (`0x005ad4d0`: the collision manager's, slot `0x28`, by
  the template's slot `0x90` name) at the spawner's matrix moved by
  `spawnOffset` (unturned) is tested against the PlayerControlObjects
  (interface `0xc4c5`) the scene finds within 10 m (`DAT_00a85220` slot
  `0x44`) of the spot, less those a global list holds (`0x005628f0` /
  `0x00561d90`: the Titan sync manager, `ECPhysicsETitanSync`,
  `0x00561c00`, with the two teams' Titans at `+8` / `+0xc`, in `gpm_ti`
  alone; `GameWorld::testSpawnSpot` skips them, see [Titans](#titans-gpm_ti)). The spot is first dropped
  (`0x005ad8b0`): along the spawner's up axis from the template's
  vehicle-LOD box top (slot `0x54(1)`: max y) to its bottom (min y), the
  scene's ray (slot `0x68`, `0x00700b80`) through objects whose flags
  (`+0x14`) have `0x1000100` and not 8 (`0x006f4aa0`; 8 marks the mobile
  ones, set with their node by `0x005f6ba0` / `0x00621b50`): a hit on a
  static object puts the box's bottom on it; else a bottom under the
  terrain (`DAT_00a83e58` slot `0x6c`) comes up onto it. Only the test
  moves: the spawn is at the spawner's spot. One overlaps (`0x006f2e90`) when it isn't being deleted, its
  response is a mesh one (`0xc42d`, `0xc430`, `0xc431`) and
  `0x006f2760` finds, for any vertex of the template's vehicle LOD (type
  1; slots `0x28`, `0x3c`, `0x40`) turned by the matrix, the segment from
  the spot to it crossing the object's soldier LOD (its slot `0xc4(2)`;
  the mesh instance's slot 22, `0x006e5920` → `0x006de850`, both sides of
  the faces). A template with MechLegs (`0x9482`) also probes three
  segments down (world y) by its object's box bottom (the
  PlayerControlObject template's `+0x4c4`, set by `0x005cd4f0` when one is
  made: slot `0x98(0)`'s min y, `0x006f7050`: its mesh instance's box and
  its children's at their offsets) from the spot and 1.7 m either side
  along the spawner's x axis (`0x006f28e0`: the mesh instance's slot 22,
  soldier LOD), when that is not 0. An existing walker is also tested
  through its legs' objects (`0x006f2e90`'s loop over the responses, slot
  `0x5c`), which the rewrite doesn't have as bodies: its hull alone. An overlapping object in use (the interface's slot `0x18`,
  `0x005c87b0`: `+0x14`) stops the spawn; otherwise the overlapping ones
  are counted as a try (`+0x204`) up to five, and on the sixth they are
  removed (the object manager's slot `0xa4`) unless a soldier (`0xc4d7`) is
  within 7 m of the spawner. Every one of these returns −1: the update
  tries again in a second (`0x005af230`). A spawn clears the count.
  `GameWorld::testSpawnSpot`, `dropSpawnSpot`, `ObjectSpawner::SpotTest`
  (the drop's ray asks the static objects' projectile LOD).

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
  `follow.maxYaw` and `maxPitch` degrees a tick (0.45 and 0.85; see
  DefaultFollowComp below).
- **DefaultFollowComp** (template `0x005ed3b0`: maxYaw `+0xc` 1, maxPitch
  `+0x10` 1, changeYaw `+0x14` 0.1, changePitch `+0x18` 0.1, minDist
  `+0x1c` 50, predictionFactor `+0x20` 0.3, magneticBehaviour `+0x24`,
  useTargetSpeed `+0x25`, speedModifier `+0x28` 1, armingSpeed `+0x2c`,
  followTime `+0x30`): its turn (`0x005ed1a0` → `0x005ecb30`, once a tick
  from the tracking tick, for followTime × 30 ticks when set) heads for the
  target plus its velocity × (distance / own speed) × predictionFactor; in
  the missile's own frame the way's x, y, z give a yaw of |asin x| degrees
  at most maxYaw (to x's side) and a pitch of |asin y| at most maxPitch;
  the missile's matrix turns by the pitch about its right, then the yaw
  about its up, and is rebuilt upright (`0x005b1a70`). A target behind (z ≤
  0) is not turned to, unless `magneticBehaviour`, which turns at the full
  rates; magneticBehaviour also lifts the caps while still or on the tick
  after the target changed (`+0x94`, set by `0x005ed260`). changeYaw,
  changePitch and minDist are stored and saved but never read (no slot of
  the instance vtables `0x00975f98` / `0x00975fe0` reads them). The
  projectile (`0x005b3e60`, `hasDirection` `+0x335`, 1) keeps its speed
  along the new heading and thrusts along it (acceleration `+0x31c`) while
  under maxSpeed (`+0x324`), not with magneticBehaviour and no target.
  `followStep` (Weapons.cpp).
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
unlocks everything. The customise screen's choice (below) gives the
containers chosen.

Guns without magazines (`ammo.magSize -1`, most vehicle guns) heat up
instead. Each round adds `heatAddWhenFire`, the heat cools by
`coolDownPerSec`, and at full heat the gun stops for `overheatPenalty`
seconds. The HUD shows `PrimaryHeatValue`. `--spawn-screen` dies first, to
show the spawn screen.

### Kits, the customise screen and field unlocks

The spawn screen's kit buttons (`HudElementsSpawn.con`, kits 0..6) are
the HUD manager's (`0x0044f1a0` binds them) and `0x0044e650(team)`
recomputes them on its events (entering the spawn screen, a kit change,
the armour, an equip), not every frame: `Kit%iShow` (the team has the
kit, while `KitsShow`), `KitName%iString` (the kit's
`vehicleHud.hudName`), `KitIcon%iPath` (`vehicleHud.vehicleIcon`),
`KitWeaponIcon%iPath` (the chosen primary's icon, else the kit weapon's
`weaponHud.weaponIcon`), `Kit%iAbilityIcon0..6` (the kit's own
`specialAbilityIcon`s, then the chosen unlocks'), `KitUnlock%iShow` (the
first branch has unlocks), `KitAltWeaponIcon%iPath` (that branch's
`altWeaponIcon`, `0x0044a3f0`), `Kit%iSprintAbility` (`0x00449600`: 0.75,
0.25 in heavy armour, more with the sprint unlock), `Kit%iHeavyArmorShow`,
`KitUnlockArrow%iShow` (a field unlock waiting, `+0x5f5`), and
`PlayerKitIcon%iSelectShow` from `spawnManager.setPlayerKit`
(`0x0079aa50`). The blink alphas (`Kit%iUnlockBlinkAlpha`,
`UnlockBlinkAlpha`, `FUBlinkAlpha`) are the only per-frame work
(`0x00448aa0`).

The customise screen is `KitInterface` (constructor `0x00798610`, the
console methods `0x00776db4`-`0x00777539`; `src/Main/HudMenusKits`). Per
team and kit it holds five rows of six items (`this+0xb8`, 0x2c each):
row 0 the primary (the kit's weapons in its primary slot), rows 1-4 the
branches UB_First, UB_Second, UB_Player and UB_SquadLeader with each
`ItemContainer` at its `unlockLevel` (the level-15 replacements are left
out). An item is available when the soldier has it (alpha 1, else 0.3),
shows its `weaponHud.unlockIcon`, and carries `specialAbilityIcon`,
`renderedPicture` and `descrString` (else `hudName`). The rebuild
(`0x00798dd0`) and the display copy (`0x007977a0`) set
`Kit<row>Item<item>Active/FieldUnlocked/Show/Icon/Alpha`, `KitCustom<row>Icon`,
`CustomInfoShow`, `HeavyArmorActive` and `HeavyArmor%iActive`,
`CustomSoldierPicture` (`Ingame/Customization/soldiers/Soldier_<team-1><kit>[1].tga`,
the 1 for heavy armour, `0x007975e0`), `OpenCloseKitCustomString`
(`HUD_TEXT_MENU_UNLOCK_OPEN` / `_CLOSE`), `NotAntiVechicleKitChoosen`; the
information (`0x007981e0`) `KitItemInfoString` with `LargeInfoPic` (row
0) or `SmallInfoPic` (`DESCRIPTION_ARMOR_MOUSEOVER` and `Armour.tga` for
the armour), reset to `HUD_TEXT_MENU_CUSTOMIZE_DEFAULT_TEXT`
(`0x00797530`). `chooseKitSelection -1 <b>` opens it (`0x00799c70`),
`closeCustomInterface` closes it (`0x00797d30`), `setHeavyArmor` sets the
player's armour (`0x007976c0`). Choosing: the original takes an item on
the pointer (`chooseKitCustom`, `0x00798090`) and drops it on a slot
(`selectDroppedItem` `0x00798cf0` → `0x007986f0`: rows 0, 3 and 4 pick one
item, rows 1 and 2 share two drop slots); the rewrite's HUD doesn't drag,
so `chooseKitCustom` puts the item in its own row's slot at once. The
choice goes to the server when the soldier spawns or the menu closes
(`0x00799aa0` → SelectKit with the unlocks); the rewrite gives the chosen
containers to `GameWorld::setUnlocks` then, and the heavy armour picks the
kit's heavy soldier.

The unlock screen (`UnlockInterface`, `0x007b8e60`-`0x007b9a20`) spends
field unlocks: points a server grants during a round (player `+0x2d0`)
that raise a kit's branch for the round (`+0x2d4`; the server's
`0x004baaf0`, for UB_Player and UB_SquadLeader all four kits).
`showUnlockInterface 1` opens it only with points (`0x007b95a0`), so
offline it never opens; its branches' variables (`0x0044a130`) are kept:
`Kit%iBranch%iUnlockLevel` and `MaxUnlockLevel` (eighths), `Alpha` (0.2
once the branch is full), `NextUnlockIconPathString`,
`NextUnlockNameString`, `NextUnlockShow`, `LargePicShow`;
`setInformationString` / `resetInformationString` (`UnlockInfoString`,
`UnlockInfoPic`, `HUD_TEXT_MENU_FU_DEFAULT_TEXT`) and `setFieldUnlock`
(`Kit%iBranch%iSelected`) too. `selectNextUnlock` / `selectUnlockItem`
(`0x007b8f30`, `0x007b92c0`) make an unlock the soldier has its branch's
choice.

### Comm roses and the radio

`src/Main/HudMenusComm` (tests in `tests/HudMenusTest.cpp`). The HUD
manager keeps a radio interface per rose (class vtable `0x009b6528`,
constructor `0x007b88e0`), each named and sized by `0x0075e1f0` (slot 0,
`0x007b7070`): Radio 11, RadioVehicle 11, SquadLeader 11, Squad 11,
Lonewolf 0, Commander 11, DemoCamera 3, DemoRecorder 9, Camera 3.
`Menu/HUD/HudSetup/Communication/<Rose>.con` fills them:
`<name>Interface.setStringMap Interface_1:<n> <text id> "<command>"`
(slot `0x20`, `0x007b8310`: the text localised into the map at `+8`, a
non-empty command into the map at `+0x14`) and `setVariableMap` (slot
`0x24`). The keys are `Interface_<path>:<n>`, the path (`+0x2c`) "1" at
the top.

- **Opening** (slot `0x14`, `0x007b7130`): the path back to "1", each
  entry's text and value into `<name><n>String` / `<name><n>Variable`
  (its last line clears `Radio%iMoseOver`, a slip in the name, after the
  counter went up, so the real `Radio<n>MouseOver` keep their values). Q
  (`c_GIRadioComm`) opens Radio on foot and RadioVehicle aboard; T
  (`c_GITacticalComm`) Commander, SquadLeader, Squad or Lonewolf by the
  player's role (the HUD manager's menu states, hidden by `0x0079ab90`:
  5/6 radio, 7 spotted, 4 Squad, 8 SquadLeader, 9 Commander, 3
  Lonewolf), `<name>InterfaceShow` 1 and the others 0, while the key is
  held, with the pointer.
- **`RadioInterface.mouseOver <n>`** (slot `0x10`, `0x007b70b0`): every
  rose's buttons call it (event 1): `Radio<n>MouseOver` 1 for that entry, 0
  for the rest.
- **`<name>Interface.selectOrder <n>`** (slot `0xc`, `0x007b73d0`): if the
  level below (`Interface_<path>:<n>:<i>`) has texts, they replace the
  entries and the path goes down; else the entry's command runs through
  the console with a new line (the HUD manager's slot `0x420`) and the
  rose closes (`+4`).
- **The Radio.* methods** (the HUD manager's slots): `callForMedic`
  (`0x198` → `0x007c7c10`), `callForAmmo` (`0x1a0` → `0x007c7c70`) and
  `callForRepair` (`0x1a4` → `0x007c7ca0`) hand the game's slot `0xd8`
  (`0x0042d220`) the codes 0x33, 0x34 and 0x35: "medic" on
  TCLocalTeamMedic, "need_ammo" and "need_repair" on TCTeam (it also
  sends the code to the server, whose use isn't rewritten).
  `setSpottedMenuActive` (`0x268`, the middle button's alternate command)
  shows the spotted rose (`SpottedInterfaceShow`, whose buttons say
  `game.radioMessage TCTeam <kind>_spotted`). `sendSpottedMessage <bool>
  <type>` (`0x17c`, `0x007c8160`) spots the HUD's crosshair object
  (`+0x1f0`, through the game's slot `0xd4`, which AIDLL's IAIGame slot `0x68`,
  `0x00895ab0`, calls too, with type 0x1b)
  and with 1 says it on the chat. The crosshair object is filled as a rose
  opens (`0x0075be30`): the HUD's ray (vt+0x18c, out to the local player's
  slot `0x98(1)`) meets an object; the player it belongs to, when of the
  other team, gives it, or that player's vehicle where the object's
  template says so (`+0x64` slot `0x78`'s `+0xb8`); its network id goes to
  `+0x1f0`, the point to `+0x1f4`. `GameWorld::crosshairTarget` /
  `spotCrosshair` (the soldiers as 0.6 m spheres, the vehicles 2.5 m, along
  the view, nothing in between); the HUD binding (`src/Main`) spots the
  local player's crosshair object (`spotCrosshair`) and closes the rose
  (a joined game's spot isn't sent to the server).
- **`game.radioMessage <TargetChannel> <voice>`** (`0x00423e61` → the
  client's slot `0x50`, `0x004b2b60`: the voice's index in the game
  logic's radio table, the spam check, event 0x14 to the server, a local
  echo): the local player's message (`GameWorld::radioMessage`). The
  target channels (`0x0042d4a0`): TCGlobal 0, TCTeam 1, TCCommander 2,
  TCSquad 3, TCSquadLeader 4, TCSquadId 5, TCLocalTeamMedic 6,
  TCLocalGroundVehicle 7, TCLocalAirVehicle 8, TCLocal 9, TCLocalTeam 10,
  TCVehicle 11, TCServerMessage 12, TCServerTeamMessage 13, TCPlayer 14,
  TCLocalRevive 15, TCAutoLocalRevive 16, TCTeamAndLocalEnemies 17,
  TCSquadAndLocalEnemies 18.
- **Who hears** (the server's `0x004a6f70`, its `this` the game server,
  whose `+0x108`, `+0x10c`, `+0x110`, `+0x114` are the squared distances
  10000, 225, 10000, 10000 from `0x004b2330`; distances between the
  controlled objects): Global everyone but the sender; Team the team
  (TeamAndLocalEnemies anyone within 15 m too); Commander the team's
  commander and squad leaders; Squad the sender's squad id, not the
  commander (two players in no squad share id 0), same team
  (SquadAndLocalEnemies anyone within 15 m too); SquadLeader the leader of
  the message's squad; SquadId that squad; LocalTeamMedic / LocalRevive
  teammates within 100 m with the medic's means (the soldier's interface
  `0xc4d7`, `0x00585af0` → its kit, `+0x228`, whose `+0x74` slot `0x38`
  flag's setter wasn't traced: taken as a kit with an RTHeal gadget),
  else as AutoLocalRevive, anyone within 15 m; Local anyone within 100 m;
  LocalTeam teammates within 100 m and enemies within 15 m;
  LocalGround/AirVehicle anyone within 15 m (ground only), else teammates
  of the sender's squad or riding a vehicle whose template's category
  (interface `0xc4c4` slot `0x24`, `0x0059d880`: vehicleCategory) is
  VCLand, VCAir or VCHelicopter within 100 m; Vehicle those in the
  sender's vehicle; ServerMessage everyone; ServerTeamMessage the
  message's team; Player the message's player. The bots take theirs (see
  Bots: Radio); the radio's text and sound for the local player (the
  client's `0x0042e0c0` → `0x0042d7f0`, the voice table's text and the
  `_important` variants) aren't rewritten.

### Chat, kill messages, VOIP and the map vote

The HUD manager's list data ids (slot `0x214`, `0x007c6d70`): 1, 2 the
scoreboard, 3 ChatList, 5 and 8 the squad menu, 6, 14, 15 the commander,
7 the commander's chat, 9, 10 the totals, 11 VoipSquadList, 12 MapList,
13 PersonalList. Edit boxes by data (slot `0x210`): 1 the chat (at most
150 characters), 2 the squad name (8), 3 the commander's chat (100);
`src/Hud` draws them (`createEditNode`, `setEditNode*`) with a caret
while they have the keys. `src/Main/HudMenusChat`:

- **Say boxes.** c_GISayAll (K), c_GISayTeam (L) and c_GISaySquad (no
  default key) open the boxes (`0x00755020`: not over a menu; team and
  squad not as a spectator; squad needs a squad), `enableSay*ChatBox`
  (`0x007d93e0`-`0x007d95b0`) show `SayAllShow` / `SayTeamShow` /
  `SaySquadShow` and give edit 1 the keys; the same again closes them
  (`0x007da200`). Keys (`0x007da320`): 0x20-0x7f are added, Backspace takes
  one off, Enter sends (`0x007d9640`, `0x007d9900`, `0x007d9b50`: to the
  server, which echoes it to everyone; offline straight into the list)
  and closes. While a box is open the soldier gets no input.
- **ChatList** (the message list class `0x007b6d80`): a line is "§1" +
  text + "§0", chat "<prefix><name>: <text>" (`0x007b4cd0`; the prefixes
  `HUD_TEXT_CHAT_TEAM`, `_SQUAD`, `_COMMANDER`, `HUD_CHAT_DEADPREFIX`),
  coloured by who said it (`0x007b4100`): the commander (0.969, 0.827,
  0.255), the squad (0.4, 1, 0), the team (0.012, 0.722, 0.949), the
  enemy (1, 0.392, 0.392), nobody (0.765, 0.729, 0.663). Nine categories
  each keep 5 s from their newest line; when that runs out their oldest
  line goes (`0x007b4a40` → `0x007b4800`). No cap on the lines was found.
- **Kill messages** (`0x00759bf0`): `HUD_HUD_KILLEDBY_KILLS` / `_TEAMKILLS`
  / `_MYSELF` with `#PLAYERNAME1#`, `#WEAPON#` (the weapon's
  `weaponHud.hudName` text, a vehicle's `vehicleHud.hudName`) and
  `#PLAYERNAME2#`, in category 1; the victim's `DeathMessage` is
  `HUD_CHAT_YOUWEREKILLEDBY` with `#PLAYERNAME#`. The rewrite's local game
  takes them from the players' kill and death counters (the killer's
  weapon in hand); a joined game from the server's kill messages
  (PythonCommand type 100, which the rewrite's host sends too).
- **PersonalList** (data 13): personal notices, 5 s each.
- **VoipSquadList** (data 11, `0x007be060` / `0x007be120`): the team's
  commander, then our squad (the squad leaders for the commander), those
  talking: `kit_Commander.tga` in (0.4, 0.6, 1) or
  `mini_Squad_<n>_small.tga` in the squad's green at x 15, the name at 30
  (`0x007bdd20`); `VoiceOverTalkIconShow` / `VoiceOverCommanderIconShow`
  while we send. The scoreboard's voice boxes (`Scoreboard.singleClick`
  → `0x007bc590`, the clicked column's kind: 6 private chat, 7 mute, 8
  buddy, 10/11 kick vote): a click on a player's box mutes or unmutes
  him (`checkbox_voipInactive/Talk/Mute.tga`; where the exe draws the box
  isn't known, the rewrite puts it at the name's end).
- **MapList** (data 12, LevelsList `0x00796ae0`): a row per map
  (`0x00796690`): the vote button (`kickbutton_up.tga`, `_down.tga` while
  a vote runs), `vote_square.tga` on the map voted for, the name with
  spaces. `levelsList.setVoteMapShow` (`0x00796640`) shows it
  (`VoteMapSelected`); a click on the first two columns (`0x00796a60` →
  `0x00573df0`) starts a map vote (the Vote event, kind 1). Offline the
  only voter carries it: the playlist goes on with that map.
- **The squad name box** (`SquadMenu.setSquadNameSelect`, `0x007a5300` →
  `0x007d6420`): edit 2 takes the keys; `createSquad` (`0x007a5ad0`)
  names the squad with what was typed (else the first free name of
  `0x00b3df00`) and sends ChangeSquadName.
- **The commander's zoom** (`Commander.artilleryZoom`, `satelliteZoom`,
  `uavZoom`: slots `0x370`, `0x378`, `0x380` → `0x398`, `0x007d8e00`):
  with the asset available, the event 0x24 (CommanderCam: x, the camera's
  height `+0x2d0`, 80 until the camera template's `height`, z) moves the
  team's overhead camera there on the server (`0x00510b30` → `0x005f8410`);
  the field of view doesn't change. `Commander.detailedView` keeps the
  old position and leaving the commander screen sends it back. The
  rewrite has no overhead camera object: the commander screen's map
  centres on the asset and a joined game sends the event.
- **The overhead view.** The asset zoom also sets the commander map's
  zoom index to 2 (`0x007d7f90` → the map's slot `0xd8 (2, 1)`); the map
  keeps a zoom index per mode (`0x0077e850`), and at 2 in the commander
  screen (`+0x70d`) it turns the overhead view on (`0x00779680`: the
  player's view takes the team's OverheadCamera, game logic slot `0x88`),
  fades its texture out (MenuMapAlpha `+0x7b8` to 0, eased at `+0x7b0`:
  down by a tenth of `1 − e^(−9t)` a frame, up by a fifth) and zooms so
  the node spans the camera's ground width, `2 tan(fov / 2) × height`
  (`0x0077c4b0`, with the view's half extents at `+0x878` for the
  clicks), without easing (`+0x898`). The camera (OverheadCamera,
  `0x005f8170`; MultiPlayerOverheadCamera.tweak: fOV 0.3, height 260)
  sits over the point kept 80 m inside the terrain, at the mean of the
  ground's height there and 2 m off along x and z (or the sea's) plus
  its height, looking straight down with north up (rows 1 0 0 / 0 0 1 /
  0 −1 0). The commander screen's map is the node at
  `setCommanderPos/Size` (−300/−300, 600/600: the square between the black
  borders), `MinimapZoomDisplay` the index. The renderer draws the
  frame's world pass from that camera (`render::OverheadView`,
  `LevelSources::overheadView`) with the fov over the map node's
  rectangle and the screen around it going on with the same view (an
  off-centre perspective, `overheadProjection`), without the first person:
  one pass, as the exe's camera swap, the view seen through the faded
  map. The exe also sets a renderer value to 1 while it is on
  (`DAT_00a2020c` slot `0x80` → `0x28` with `DAT_00a0c204`, restored
  after), not decoded.

### HUD nodes: transform lists, effects, markers and the map menus

The rest of the `hudBuilder` methods the shipped scripts call
(`src/Hud/Hud.cpp`; the builder's methods are `0x007ae270`..`0x007b20c0`,
each handing its arguments to a slot of the node class it casts the
active node to):

- **Transform lists** (`dice::meme::Bf2TransformListNode`, vtable
  `0x009bbf88`). `createTransformListNode <parent> <name> x y w h
  <spacing>` (`0x007b2b50`; the last argument is the spacing, `+0x24`).
  `addTransformListNode <node>` (`0x007aed80` → slot `0xd4`,
  `0x007ebde0`) appends a node, `setTranformListNodePosVariable <axis>
  <variable>` (`0x007aedf0` → slot `0xd0`, `0x007eb9d0`) binds its
  position to an int HUD variable, `setTranformListNodeOffset x y` (slot
  `0xcc`, `0x007ebd60`) adds an offset. Its render (`0x007eba70`) puts
  the list at the variables' values (else where it was made) plus the
  offset, keeps the block (as tall as its nodes' heights plus the spacing
  each, as wide as the widest) inside the screen, then sets each node's
  position (the node's slot `0xcc`, `0x007da5b0` for a transform node)
  one under the other; the node's own `setNodeOffset` (`+0x2c`) goes on
  top. The exe adds the list's offset to its position every frame, which
  only stays put with position variables (every shipped list has them);
  the rewrite adds it once. The bounds use the render's parent rectangle
  about the screen's centre; the rewrite uses the 800 x 600 screen. The
  layout runs for every list before each frame is drawn.
- **Position variables and offsets.** A transform node's render
  (`0x007da720`) is the variable's value plus `setNodeOffset`: the map
  menus' markers (`crosshair_comrose.tga`) sit -20 -20 from
  `OrderMenuPosX/Y`.
- **Move, blend and blink effects.** `addNodeMoveShowEffect <angle>
  <distance>` (`0x007b1f40`) puts a `Bf2MoveEffect` (`0x007e4230`) above
  the node: while the node fades in or out (its in and out times) it and
  its children sit `distance` pixels along the angle (radians, 0 right,
  pi/2 down) times what is left of the fade. `addNodeBlendEffect <src>
  <dest>` (`0x007b20c0`, a `<node>BlendEffect` render-state node) gives
  the quads below it those Direct3D 9 blend factors (the vehicle HUDs'
  `7 2`: `D3DBLEND_DESTALPHA`, `D3DBLEND_ONE`; the render-state node
  hands each quad below it through, `0x00870c40`); `HudQuad::blend`
  carries them to the renderer, which draws those quads with a UI
  pipeline of the same factors (`GpuContext::uiBlendPipeline`, made the
  first time a pair is used; the target's alpha is 1 after the
  post-production, so `7 2` adds the glass over the view). `setNodeTimesBlink <n> <mode>` and
  `setNodeIntervalBlink <s>` (`0x007b1c00`, `0x007b1da0`: the blink
  effect's slots `0xdc`, `0xd8`): once shown, the node is off every other
  interval for its first n blinks.
- **Commands.** `setNodeShowConCmd <command> <event>` (`0x007b0e00`, an
  `executeConsoleCommand` effect): event 1 when the node starts being
  drawn, 0 when it stops (the pods' power-up sounds).
  `setButtonNodeAltConCmd` (`0x007b00b0`, slot `0x100`): the right
  button's commands. `setButtonNodeMouseArea x y w h` (slot `0x104`): the
  button's clickable rectangle from its corner. `setMouseOverNodeConCmd
  <command> 3` (`0x007b02a0`, slot `0xf8`): the release over a mouse-over
  node (the kit screen's drop targets). `setButtonNodeDebug` is kept.
- **Pictures, text and bars.** `setPictureNodeBorder <l> <t> <r> <b>` and
  `setPictureNodeBorderColor` (`0x007af640`, `0x007af6d0`) draw lines of
  those widths round the picture. `setPictureNodeCenterPoint` (slot
  `0xe0`, `0x007c6750`) stores a point the picture's render (`0x007c67b0`)
  turns the quad about instead of the rectangle's centre; the shipped
  scripts' `0 0` (the hit indicator round the crosshair, the gunships'
  horizon bar) is the screen's centre, so the rewrite takes the point
  about it (`HudQuad::pivot`). A picture without a texture
  (`0x007c6580` leaves it null and the render still draws the quad) is a
  fill in its colour: the commander screen's black side borders
  (`CommanderHudLeftBorder` / `RightBorder`, 0..100 and 700..800) and the
  unlock and customise backgrounds. `setTextNodeOutLine <font>` and
  `setTextNodeOutLineOffset` (`0x007afa40`, `0x007afac0`: the text's
  font object's slots `0x5c`, `0x60`) draw the outline font beneath;
  `setTruncate` (slot `0x68`) cuts the text at the node's width.
  `setBarNodeBorder <top> <bottom> <left> <right>` (`0x007b0540`, slot
  `0x104`): the fill takes the inner rectangle (the conquest point bars
  share one 152 px node, `0 0 17 76` filling from the right and `0 0 77
  15` from the left); `setBarNodeIncreaseShow`, `setBarNodeSnapDir`,
  `setBarNodeRound` are kept. `setNodeSize` (`0x007b0a10`, slot `0xc8`).
  Occupied nodes keep their seat, `setOccupiedNodeData` and
  `setOccupiedNodePosVariable` (`0x007afce0`, `0x007afc40`).
- **Object markers** (`dice::meme::Bf2ObjectMarkerNode`, made by
  `0x007e5f30`, vtable `0x009baf30`; the builder's `0x007af210`..
  `0x007af440` → slots `0xc8`..`0xe8`): textures and sizes per state
  (`setObjectMarkerNodeTexture` / `TextureSize <n>`, up to 4), the most
  targets (`setObjectMarkerNodeObjects`, `+8`), the lock-on type
  (`setObjectMarkerNodeLockOnType`, `+0x54`, 2 by default: TTHeat; the
  gunships 1 TTLaser, the mortar HUDs 5 TTBeacon, -1 the weapon's own
  target's type), the weapon (`setObjectMarkerNodeWeapon`, `+0x58`), the
  range cut (`setObjectMarkerNodeRC`, `+0x61`), the labels
  (`setObjectMarkerNodeLockText <on> <prefix>`, `0x007e4bd0`: the HUD
  variables `<prefix><n>String` and `<prefix><n>Show`;
  `setObjectMarkerNodeLockTextOffset`, `+0x40`, -3 0 by default;
  `addObjectMarkerNodeLockTextNode`, `0x007e5cf0`). Its update
  (`0x007e6880`): the targets (`0x007e6520`: the objects of the lock-on
  type in the weapon's lock cone and reach, not the player's team; the
  weapon's lock-on component `0x94c0` gives its target, `0x144`, and
  whether locked, `0x148`), their labels (`0x007e56e0`: the distance
  rounded up, `Show` 1, the rest of the 20 cleared), the draw
  (`0x007e5a10`: each centred on its point with its state's texture, the
  n-th label beside it at the point plus half the marker plus the offset,
  rounded up). The game hands the targets in (`HudManager::
  setMarkerSource`): bf2142 gives the enemy objects that carry a target
  object of the type (`HeatObject` TTHeat on the aircraft,
  `LaserTarget_vehicle`, `Beacon`) in front of the camera, within the
  node's rectangle and 600 m, nearest first. They are all candidates
  (state 0): the game has no lock-on weapons yet (the `0x94c0`
  component, locking 1 and locked 2), so the locking and locked boxes
  don't show.
- **The text node pair.** `createTextNode <name>` makes a transform node
  and a text node `<name>TextNode` below it (`0x007b2b50`'s pattern);
  `setActiveObject <parent> <name>TextNode` finds the one node the
  rewrite makes.
- **The map's right-click menus.** The map node's right click
  (`Bf2MenuMapNode`, `0x00788c00` for event 0x15) sets `OrderMenuPosX/Y`
  to the click and `MinimapRMBMenuState` to what is under it
  (`0x00782d90`: 2 and 3 the player's and the enemy's Titan, 4 / 5 / 6 a
  control point of his team / the enemy's / neutral, 0 one of his
  commander's assets, 7 nothing), and the HUD manager's menu state for
  his role (`0x0079ab90`: 0x16 `CommanderMenuShow`, 0x15
  `SquadLeaderMenuShow`, 0x14 `MapMenuShow`). The menus' rows are
  transform lists at `OrderMenuPosX/Y`; an order row (`Commander.
  sendOrder 6 1`) goes to the clicked point (`+0xdbc`) at once and
  closes the menu, as does any other click. bf2142 checks what is under
  the click within 16 pixels of the map. `--hud-click x/y[/right]` clicks
  the HUD before a screenshot.
- **The HUD manager's settings** (`HudManager.*`, its slots from
  `0x0075e732`): `setMouseTexture{Empty,Full,Artillery,UAV,Supply,
  MoveTitan,OrbitalStrike}` and `setPointerSize` (slot `0x1b4`) give the
  pointer (`HudManager::pointerQuad`; the commander screen draws the armed
  tool's over the map, else the map's), `setCommSize/Pos/MousePos`,
  `setSpottedMenuSize/Pos/MousePos`, the sensitivities and the rest are
  kept by name. **Minimap** (`0x0076f773`..): `setZoom` steps the zoom
  index 0, 1, 2, 0 (`0x007800d0` → `0x0077e850`), `setPaintKit` /
  `setPaintVehicle` toggle a filter bit (`0x0077e670`, `0x0077e6b0`),
  `setPaintAll*` sets all or clears them when all were set
  (`0x0077fff0`, `0x00780040`); the filters are kept (nothing paints
  kits on the map yet), as are the blend and filter colour settings.

### Warnings and help pop-ups

(`src/Main/HudAlerts.*`.)

- **Missile and mine warnings.** The WarningHud template component
  (class id `0xc4f6`; `warningHud.setWarningType`, `setWarningIcon`,
  `setFirstWarningSound`, `setSecondWarningSound <TargetType> <value>`
  from `0x0055a940`; 8 target types of 0x1c bytes, written back by
  `0x00639a40`) says what a vehicle warns of. TargetType (`0x004daf00`):
  TTNone 0, TTLaser 1, TTHeat 2, TTMagnet 3, TTUnique 4, TTBeacon 5,
  TTDecoy 6, TTPod 7. The HUD's event 0x2f (`0x007a2100`) is a guided
  projectile's lock on the player's vehicle: when the vehicle warns of its
  type (TTUnique also when it warns of TTLaser, `+0x1c1`), the
  component's warning sound starts (slot `0x3c`: 1 the first, 2 the
  second), `WarningIconShow` comes on with the type's icon in
  `WarningIconPath` (`HudElementsActionIcons.con`), and the projectile
  joins a set (`+0x2c0`); when the last one lets go (id -1) the sound
  stops and the icon goes. bf2142 makes the events each frame from the
  projectiles locked on the player's vehicle (`GameWorld::projectiles`,
  `lockedOn`): the first warning while a seeker has locked but is still in
  its tracking delay, the second once it chases; only the motion mines
  (TTMagnet) lock on vehicles in the game so far. The tones go to the
  sound layer by name (`HUD_MotionMine_Warning`, `HUD_Laser_Track_Warning`).
  What sends event 0x2f in the exe (and so the exact first/second split)
  is not traced.
- **Help pop-ups.** The HelpHud component (class id `0xc4fa`, 15
  properties from `0x0055b355`: `helpStringKey`/`helpSoundKey`, their
  `alt`, `lowArmor` and `lowAmmo` pairs, `closeToHelp{String,Sound}Key
  {Neutral,Friendly,Enemy}`, `closeToDistance`): the help key shows when
  the player takes the vehicle or the weapon that has one, the low-armour
  key when that vehicle's hit points fall to its `armor.criticalDamage`,
  the close-to key for the object's side when the player comes within its
  distance. Showing (the HUD manager's slot `0x94`, `0x0074f2b0`) sets
  `IngameHelpString` (localised) and `IngameHelpShow`, and a timer
  (`+0x238`) the HUD manager's update counts down (`0x007539c0`) before
  hiding it; the rewrite gives it 6 s (the help manager's own timers,
  around `0x00645d40`, are not read). The matching sound key goes to the
  sound layer's `VoiceMessages::playHelp` (`gameLogic.messages.
  addHelpMessage` keeps the Help_VO files there).
  `GeneralSettings.setHelpPopups 0` turns them off; each key shows once a
  round. The help manager's own tutorial messages (`0x00645d40`'s 75
  HUD_HELP keys: controls, squads, the commander's tools) are not done.
- **Supply sounds.** AbilityHud (`0xc4f2`, `abilityHud.healingSound`,
  `repairingSound`, `ammoSound` from `0x0055a41f`):
  the kits carry them (`S_Heal`, `S_Repair`, `S_Resupply`); the player's
  kit plays its sound while a hub, a repair or a supply crate treats him
  or his vehicle (`GameWorld::received`: the last treatment
  within half a second; when the exe starts and stops it is not traced).

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

A player's vehicle and default vehicle (`pmgr_p_get "vehicle"` /
`"defaultvehicle"`, `0x0056ae00`) are the player's weak handles, `+0x80`
(slot `0x4c`, `0x00593df0`) and `+0xcc` (slot `0xb4`, `0x00594ad0`), None
once the object is being deleted (flag `0x20`); "vehicle" answers the
default vehicle instead when the vehicle's PlayerControlObject's slot
`0x148` says so (a remote-controlled one), and None without a vehicle
(`0x00569770` wraps null as None). The death (`0x004cd390`, which sets the
respawn time) gets the player out of its vehicles until the vehicle is the
default one and clears only the default vehicle (`0x00594b10(0)`): the
vehicle handle keeps the soldier through man down and as a corpse until
the armour's countdown removes it (the soldiers' 6 s after landing and a
second's fade). So gpm_ti.py's VehicleDestroyed handler, which calls
`isValid` on every player's `getVehicle()`, works for the dead whose
bodies lie; for a player whose body is gone or who never spawned the
original's handler raises too (the host prints the error, the round has
ended by then). `PlayerInfo::heldVehicle` / `heldSoldier`.

**Assault Lines** (gpm_sl.py) gates capture by supply groups, all in the
script: a point may be taken by a team (`cp_setParam allowCaptureByTeam`)
only while that team holds every required group it needs and enough
supply value. The engine serves the data (`cp_getParam`, `0x0056a010`):
`supplyGroupId` (the ControlPoint template's, -1 by default; the save
`0x005bc260` writes it from `+0x2ec`), per team `supplyValueNeeded`
(`supplyValueNeededTeam1/2`) and `supplyValue` (`supplyValueTeam1/2`), and
`supplyGroupsNeeded` (a tuple of (group, optional) pairs: the template's
`supplyGroupNeeded <team> <group> [optional]`, `0x00503dec` /
`0x00503f73` → slot `0xc0`, optional false by default; Highway Tampa's
bases have `2 3 0` and `1 3 0`). `host.sgl_getControlPointsInGroup`
(`0x00568890`, the game logic's slot `0x18c`) gives a group's points.
`getSupplyGroupId` isn't in BF2142.exe (only gpm_sl.py's unused
`showControlGroups` calls it); the rewrite answers it. The bots don't
know the supply lines (their control point view, `0x00895d90`, asks
`unableToChangeTeam` only).

**The other host hooks.** `pmgr_getSquadLeader(team, squad)`
(`0x0056a570`, the squad manager's slot `0x40`, -1 for none) and
`pmgr_getPlayersInSquad` (`0x0056adb0`, the player manager's slot `0x58`,
a tuple of indices). `pmgr_enableScoreEvents(n)` (`0x0056ad00`) sets the
flag `0x00a00594` (n > 0) that makes `pmgr_setScore(p, "score", v)` raise
`PlayerScore(player, change)` (`0x0056ae60`, the tuple of `0x0056a4b0`,
event 0x20). `pers_plrGiveFieldUnlock(p)` (`0x00566850`, "no such player"
unless valid) hands the game logic's slot `0x68` the player (the rewrite
counts it: `GameWorld::fieldUnlocks`). `pers_plrRequestStats` /
`pers_plrRequestUnlocks` (`0x00566930`, `0x00567370`) queue a request to
the stats server and return whether it went: 0 here (no server; the
scripts only ask with `sv.globRank` / `globUnlocks`, off), and
`pers_getStatsKeyVal` (`0x00566b90`) raises "there is no current
persistence object downloaded". `gl_sendEndOfRoundData(text)`
(`0x00568930`) sends the end-of-round text to everyone (data block 8 with
the delay, `0x0059cb..`) and, on a client, hands it to the menu's
SwiffHost (slot `0x174`); the rewrite keeps it in `GameLogic` and a hosted
game sends it as block 8. `sgl_getOverlayDirectory` (`0x00567730`) is the
`GSOverlayPath` setting ("" unset) and `gs_abortCurrentConnectionAttempt`
(`0x00568700`, the GameSpy layer's slot `0xe4`) does nothing offline.

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

The sphere stack goes by stance (`0x006c1b30`): five to 1.7 m standing,
three to 1.4 m crouching or swimming, one at the feet prone, where a floor
(n.y over 0.95, `*0x00a84788`) pushes straight up. Its place and the
filters' heights are where the soldier was before the last integration
(the node's saved matrix, slot `0x4c` → `+0xd4`), less what the object
itself moved that point; over 0.9 of a radius a tick (6.75 m/s,
`*0x00a84760`) each sphere sweeps from there (backed off 0.45 r along the
way, `*0x00a8475c`) to where it is now (`0x006e2350`: faces pushed out by
the radius and crossed from the front, `0x006be1a0`; only without one,
the edges' capsules, `0x006be300`), so a fast soldier doesn't pass a wall
between ticks. Moving objects (mesh responses) are tested in their own
frame. Each contact faster than 0 relative to the object calls both
objects' collision events (see the mesh contacts above: a soldier's fall
damage, run over by a vehicle).

**Soldiers against soldiers** (`0x006c2620`, from slot 9 and the solve):
two soldiers on foot whose heights overlap (each from its feet to its
stance's height: 1.7, 1.4 crouching or swimming, 0.8 prone) and whose
origins are within twice the radius × the scale across push each other
apart by the overlap (no velocity; along z when nearer than 0.01). The
original adds both soldiers' contacts at once; the rewrite gives each its
own when it steps.

**The solve** (slot `0x30`, `0x006c48c0`; constants static_init
`0x008ede60`..`0x008ee020`): the contacts so far push the soldier out,
their cancelled velocity is kept aside, then up to eight passes
(`*0x00a84738`) collide again with everything (static objects, moving
ones, soldiers, the ground) at 0.999 of the size (`*0x00a84abc`: the
ground points scaled towards the origin, the spheres smaller), each pass's
contacts pushing out at once (a nested solve: the push only), until a pass
finds none. In the passes a contact counts 2 / 0.999 times as deep
(`*0x00a84794`), has no velocity, no collision events and doesn't hold the
soldier up; the floor and ceiling filter is off (`*0x00a8479c` 0); past the
first three passes (`*0x00a847a0`) an object's contact pushes back along
the way the soldier came by how far the sphere went past the face (the
query's second depth list: the sweep's distance past the crossing,
`0x006bd070` / `0x006be300`). Free: the kept velocity becomes the impulse
(× the elasticity + 1, × `*0x00a8478c`), the first contacts' count and
velocity come back, and the run-over check follows (`0x006c2e50`: a moving
object of mass 100 or more, awake, whose soldier LOD comes within half a
radius of a sphere's centre where the soldier is now — from inside its
hull, where its faces don't push — at over 3 m/s relative kills: 1000
through the game logic's slot `0xe4`, its owner credited). Still stuck
after eight passes, the soldier is squashed: node slot `0x148(0)` (no
velocity, impulse or input; gravity only), standing (`+0xf9`, normal up,
one contact at no speed), back where it was before the last integration;
if a heavy object (over 100, awake) moving at it faster than 3 m/s
(`*0x00a84748`) was near and the deepest contact (objects' in every pass,
the ground's past the third) is 0.05 deep or more (`*0x00a84754`), the
soldier is thrown at that object's velocity × 2.5 (`0x006c1db0`) and
killed (1000, the object's owner credited). Then, on the server, the
segment test: the origin's way since before the last integration, less
each moving object's own motion, crossing a face of a soldier LOD from the
front (mesh slot 22) kills (1000, no one credited) — also a prone soldier
whose spheres pass under something its origin (a metre up) runs into.

**The push** (`0x005ee2b0`): a hit's velocity × 2.5 (`*0x00a847b4`) kept
16 ticks on the soldier (`+0x248`, the tick `+0x254`), which its ragdoll
starts with instead of its own velocity when it dies by then (`0x005f0a00`):
from slot 9's object contacts faster than 1 m/s relative (between the two
objects' events: the other's point velocity, none for a static object) and
from a crush.

**A launcher's projectile** (`noInitialCollisionWithLauncher`, template
`+0x355`, slot `0x114` = `0x005b37d0`; shipped: the portable shield, the
sentry gun, the motion-mine bait): setting its launcher (the projectile
interface `0x94c1`, vtable `0x00969b98`, slot 17 = `0x005b48b0`; on a
client the creation event's `0x0050fb40`) makes its mesh response ignore
the launcher's root (`0x006b8de0`: `+0xfc` the root, `+0xf8` the tick
(`0x00666090`) + 2 (`*0x00a84284`) on the server, −1 for good on a
client; null clears it, as the ghost updates `0x005ffec0` / `0x00606f20`
do). In `0x006c3890`, once a mesh response's faces are found: in the main
pass, when it ignores this soldier (`0x006b8da0`) its contacts are left
out and the window opens again (so they never touch while overlapping);
in the solve's passes whenever the window is open (`0x006b6c30`: any
soldier). Nothing else reads it (the mobile meshes don't).

**Small projectiles** (`0x006c3890`, per contact): the contact of a
GenericProjectile (class `0x9539`) whose soldier LOD's radius (the mesh
instance's slot `0x50(2)`) is under 0.6 m (`*0x00a84acc`) doesn't go to
the soldier (no contact, no standing on it): unless it is the soldier's
own (`*0x00a84774` = 1, the projectile interface's slot `0x48`), the
projectile's response gets it (slot `0x2c`, `0x006ba4c0`: the point from
its position, the soldier's relative velocity turned round, the
soldier's normal and the depth turned round, the two materials), so it is
pushed away. Shipped: the motion mine (0.19 m) and the sentry gun
(0.57 m); the other deployables have no soldier LOD. The collision events
still fire. The rewrite gives an awake body the contact.

**The run-over's team filter** (`0x006c2e50`'s start): the soldier's first
PlayerControlObject up its tree (itself) gives a team (slot `0x9c`); an
object whose response has `collisionGroups` bit `0x10`, or `0x100` for
team 1 / `0x200` for team 2, is passed. With `*0x00a84764` = 1 the soldier
node's slot `0xd8` (`0x006c56e0`: `+0x90` while the tick count `+0x9c`
runs, set by slot `0xdc`, `0x006c5df0`, at most `*PTR_DAT_00a06ee4` long
for `*PTR_DAT_00a06ee0` ticks) comes off the relative velocity; no caller
of the setter was found, so it is none. `SoldierBody::Mover` (`ignoring`,
`ignoresThis`, `pushed`, `own`, `collisionGroups`), `SoldierBody::team`.

**The jump** (the Soldier's `0x0058a6d0`, PIAction: input 9, mask
`0x200`): while the action is held, a soldier standing (pose 0), on the
ground (the response's `+0xf9`), not swimming and without get-up time left
(`+0x3fc`: a second, `*0x00a68e30`, after rising from prone,
`0x005889d0`) jumps: the velocity along its heading and across it (y left
out) × 0.98 (`*0x00a68f14`), at most the current speed index's speed
(`0x00586df0(7)`) × 0.98, and 6 m/s up (`*0x00a68f18` × 6) becomes the
node's velocity and acceleration with no impulse (slots `0xc8`, `0x78`,
`0x5c`; the original's object update comes before the integration, the
rewrite's step collides first, so it waits for the integration). It sets
`+0x3f4` 1 s, `+0x3f8` 0.2 s (no lying down, `0x00586f40`), `+0x408` 1 s
(the stamina's recovery waits), `+0x2e4` 2 s (the air control) and, from
ground steeper than 0.8, `+0x2e0` 0.5 s (the air control's delay); the
stamina loses the template's `sprintLossAtJump` (`+0x308`, `0x006c54e0`).
Held while prone, the action counts as the crouch key (getting up). In the
air (`0x00588420` with its flag) the input steers while `+0x2e4` runs and
`+0x2e0` doesn't: the direction × the time left × 0.5 × 2 m/s
(`*0x00a68eb4`) added to the velocity, the ground speed held to what it
was when that was over the steering speed and grew. The animation's jump
is its up value (see the animation systems).

**The sprint's stamina** (the Soldier's `+0x360`; `0x00645790` from the
template's `sprintDissipationTime`, `sprintRecoverTime`, `sprintLimit` and
the player's ability modifiers; `0x006457f0` each tick from `0x0058d350`):
sprinting drains it by the time over the dissipation time and ends when it
is out, not asked for, crouching, prone or seated; otherwise it recovers by
the time over the recovery time (none while `+0x408` runs) and a sprint
asked for (`0x00645760`: the key's press; holding only keeps one going)
starts at the limit or more. Rewritten in `game::SprintStamina`; the
ability modifiers are 1.

**Water** (`0x006c28c0`): the feet's depth under the water level is the
node's `+0x5c`. Deeper than 0.3 m (`0x00975eb8`) the water lifts by gravity
× `gravityModifier` × −depth² (under a metre) or −depth, level with the
feet a metre under: the origin at the surface. From 0.99 m (`*0x00a847a4`)
until under 0.9 (`*0x00a8476c`) the soldier swims (response `+0xfa`): the
ground counts as flat with friction 1 and the velocity as the contacts', so
the movement steers along the surface; the damping is 0.9 by the depth. The
Soldier object (`0x0058a6d0`) then sets pose 3 (swim speeds 2.1 / 3.645),
ignores the stance keys (`0x005889d0`), stops strafing (`0x00588420`) and
puts the weapon away (`0x005878b0`); out of the water the pose is standing
again.

**Ladders** are not the response's. A Ladder (`0x0059fb00`) touching a
soldier (its callback `0x0059fd40`) seats it in a LadderContainer
(`0x0059dec0`) when it stands above the ladder's bottom, faces it within
60° (either side), looks up (or near the top down by 35°) and fits where
it would be put off. The ladder's top and bottom are its soldier box's
(`0x0059d550`; the bottom no lower than the terrain or 1.2 m under the
water). The container climbs by the throttle (`0x0059f6b0`): 0.045 m a
tick (1.35 m/s), 1.15 times as fast within 1.8 m of the top, the other way
looking down 35° or more, and 4 times down while sprinting (a skid). Past
the top less 0.36 going up, under the bottom going down, or under the
ground, the soldier is put off (`0x0059e820`): over the top 0.75 m on, or
0.75 m back from the bottom or where it is, if it fits. Rewritten in
`src/Game/Ladders.cpp`. Since: the soldier sits at the LadderContainer
template's seat (`seatInformation LadderContainer 0/1/0`, in the
container's frame: the ladder's, at the container's height); the bottom's
ray (`0x0059d550`): down from the top to the bottom 0.6 m (soldier-dist)
behind the ladder's axis against the objects, what it meets is the bottom
(0.01 over it; 1.2 m under an object with a water material); the jump key
(the container's `+0x460`, `0x0059f1f0`) puts the soldier off anywhere
(mode 3); and the rays (`0x0059ed10`) from the container's new place, 0.75 m
back (ladder-exit-offset): going down or skidding, a ray down 0.2 m (0.3
skidding) from there or one back from the container meeting anything puts
the soldier off (1); going up, a ray up 0.2 m from 1.7 m over there or one
back from 1.7 m over the container meeting anything stops it (2); within
1.8 m of the top, unless the soldier fits at the top's exit, a ray over the
top (s = max(0.3, 1 − the way left / 1.8) up, forward 0.75 × s + 0.75)
stops it; getting on, the same rays from the soldier's origin (back against
its facing, the up ray 1.9 m) must meet nothing, and it must not be under
the floor (`0x0059e210`) unless near the top. Not rewritten: the
animation and camera.

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
- **Spawn point options** (dice.bf.SpawnPoint, class `0x948a`, made by
  `0x00635590`; the setters go through its interface `0xc47d` at `+0x68`,
  vtable `0x00982918`; the point object's vtable `0x00969008`): the
  point's test for a player (`0x005b24f0`, slot 7, which the spawn
  manager asks of each, `0x00634fc0`): `setOnlyForAI` (`+0x92`) keeps
  humans off, `setOnlyForHuman` (`+0x91`) bots; with `setEnterOnSpawn`
  (`+0x89`, or a bot and `setAIEnterOnSpawn` `+0x8a`) only with a vehicle
  of the team (or neutral) within 5 m (a bot: 15 m) of it (`0x005a98e0`:
  the EntryComp objects, class `0x9477`, nearest first, with a free
  seat), the soldier then put in that seat; otherwise, unless it scatters
  or has `setAllowSpawnCloseToVehicle` (`+0x93`), not while a living
  soldier or a vehicle is within a metre (objects within 1 m, flags
  `0x1000200`). Where (`0x005b2200`, slot 13): with
  `setScatterSpawnPositions` (`+0x90`) the squad leader's scatter (below)
  from the point and its heading, else the point plus
  `setSpawnPositionOffset` (`+0x70`, 0/1.02/0 by default: the soldier's
  origin, about a metre over the feet), unturned. Not rewritten: the
  `+0x8c` height test (-1, off; no shipped setter found) and the
  parent's armour tests. When every point is taken the rewrite uses one
  anyway. Mobile spawn points: a SpawnPoint in a vehicle's tree (the
  APCs' `eu_apc_SpawnPoint`, the AT and IFV spawn vehicles', all
  `setEnterOnSpawn 1`) is a spawn group of its own (net ids from `0xc0`,
  SpawnManager `0x00635080`); `GameWorld::setSpawnVehicle` picks a team
  vehicle carrying one (`spawnVehicles` lists them) and the soldier
  starts in its nearest free seat. The spawn menu doesn't list them yet.
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

## Combat areas and vehicle teams

**Out of bounds** (the CombatAreaManager `DAT_00a25134`, constructor
`0x004cb380`, reset `0x004cb440`; `src/Game/CombatAreas`,
`GameWorld::stepCombatAreas`). `timeAllowedOutside` (`+0x18`, 15) and
`damage` (`+0x1c`, 100), per vehicle category a timer (`+0x2c`,
addVehicleTimer `0x004cafb0`; VCLand and VCAir start at 15) and a damage
(`+0x20`, addVehicleDamage `0x004cb010`). The server's tick (`0x004bbf60`)
takes every player: dead or inside the areas that apply (team 0 or its
own, binding the controlled object's category; `0x004ca5d0`) the time
outside (player `+0x294`) is 0; outside it runs, and past the timer
(`0x004cac70`: a soldier's is timeAllowedOutside, a vehicle's its
category's) the soldier takes `damage` each tick (not by the second: 100
kills at once). The time goes to the client in the player networkable's
0x800 part as (u8)floor(seconds) in 9 bits (`0x00592410` at `0x592856`,
no clamp; the client takes it back as whole seconds, `0x599209`), whose HUD
shows "TIME UNTIL PUNISH: #TIME#" (`0x0044c7b0`; the timer less the time
outside, `GameWorld::combatAreaTimeLeft`) and plays the commander's
"leaving_the_combat_area" on the first frame. Abandoned vehicles with
destroyWhenOutsideCombatArea (`0x004bed00`) outside the areas binding
everything lose damage(category) × floor(t / timer)² a second after 15 s.
CombatArea: `create` re-selects an area by name, `vehicles` 4 and `layer` 1
by default (`0x004cb230`), addAreaPoint drops repeats and is a polygon from
its third point (`0x004da580`); `active`, `deleteActiveCombatArea`,
`getActiveCombatAreaName`. Not rewritten: the Titan mode's kill inside the
enemy Titan's shield (every 64 ticks, `0x006f2b00`), the HUD's text (the
HUD's), the minimap's area texture.

**A vehicle's team** (a PlayerControlObject's slot 0x9c, `0x005c9500`):
the root's seats in order, the first with a player gives its team; none,
the default team (`+0xd0`, setDefaultTeam `0x005c7ee0`). So a neutral APC
entered by a team-2 soldier is team 2's (its SpawnPoint a mobile spawn
point of that team) and, left empty, neutral again. (The server's branch
of `0x005c9500` returns `+0xcc`, set by setTeam `0x005cc320` from the
spawners, `0x005ae490`, and a parachute's entering, `0x005cfaf0`; the
rewrite keeps the one team.) `GameWorld::stepVehicleTeams`.

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
The spawn screen's squad menu (`SquadMenu`, BF2142.exe `0x007a4000` ..
`0x007a8800`; `src/Main/HudMenus`): `SpawnManager.toggleMembers 0/1`
(`0x007b8b80`, menu states 1 and `0x12`/`0x13`) switches the kit half
(KitsShow) and the squad half (MembersShow). The squad list (list data 5,
built by `0x007a5f10` and `0x007a65a0`) has a row per squad, then its
members; each row carries its id in cell 13 and its kind in cell 14 (2 a
squad, 4 a member; read back by `0x007a5940` / `0x007a59d0`). Cell 1 is the
squad's number, 5 its name, 6 "members/6", 10 JOIN, or LEAVE on the
player's own squad (`HUD_TEXT_MENU_SPAWN_SQUAD_JOIN` / `_LEAVE`). A click
(`SquadMenu.singleClickNew`, `0x007a81b0`) acts by the row's kind and the
column under the pointer (the list's `+0xc`): a squad's column 10 leaves
(`0x007a55f0`: squad manager slots `0x38`, `0x30`) or joins (`0x007a5410`:
only from no squad and not as the commander, else
`HUD_HUD_SQUAD_MUST_LEAVE_TO_JOIN`; slot `0x34`); column 8 locks or unlocks
it for its leader (`0x007a5a60`, slot `0x20`). The double clicks do
nothing more. Creating (`setSquadCreateSelect 1`, `0x007a5700`) opens the
box with the first free squad id (HUD slot `0x310`; a leader alone in his
squad makes it again) in SquadCreateTagString, refused in a squad
(`HUD_HUD_SQUAD_MUST_LEAVE_TO_CREATE`), as the commander
(`HUD_HUD_SQUAD_COMMANDER_MUST_RESIGN`) or with all nine taken
(`HUD_HUD_SQUAD_MAX_REACHED`); `squadCreateLockToggle` (`0x007a5390`) flips
SquadCreateLockedShow and `createSquad` (`0x007a5ad0`) makes the squad,
named by `0x007a5250`: the first of the game's names (757 at `0x00b3df00`,
filled at start-up: "Unassigned", "Alpha", "Bravo", "Charlie", "Delta",
"Echo", "Foxtrot", "Gamma", "Hotel", ...) no squad of the team has. The
HUD variables are the menu's (`0x007a4100`): NrOfUnassigned (`+0xc`) and
NrOfSquads (`+0x10`), numbers; SquadCreateShow `+0xec`,
SquadCreateLockedShow `+0xee`, SquadCreateTagString `+0xf0`, InviteListShow
`+0xa5`, IsInSquad `+0xa6`, IsSquadLeader `+0xa0`.

The leader's invite list (list data 8, `setShowInviteList 1`,
`0x007a7dc0`) lists the team's players with the members selected
(`0x007a7ac0` fills the selected set `+0x104`). A click
(`singleClickInvite`, `0x007a8420`: the player id in cell 2 →
`0x007a7e20`) selects a player outside the squad while there is room (six
less the members) or unselects a member (the set `+0x110`).
`applyInviteList` (`0x007a84a0`) removes the unselected members (slot
`0x38`) and invites the selected players (slot `0x30`). Offline nobody
answers an invite, so a bot in no squad takes it at once (a stand-in).
The squad name box (an edit node) isn't typed into: squads keep the
default names. The list columns' positions are fitted to the scripts'
list widths (the menu's own widths at `+0xc0`.. are set in code not
traced).

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
`satellite` scans at once, `sendOrder` / `issueOrder` arm an order, and
`commRoseAccept` answers one. `SquadLeader.sendOrder` and `Commander.sendOrder
<kind> <add>` share one handler (0x00770889): the squad leader's order goes to
his own squad, the commander's to the squads selected in the screen's squad
list (below; `0x007a9f60` sends one to each squad slot `0x34` reports
selected), or, with none selected, to the squad whose leader is nearest the
click. `Commander.defendArtillery` / `defendSatellite` / `defendUAV` /
`defendOrbitalStrike` (HUD manager slots `0x374`, `0x37c`, `0x384`, ...:
`0x007d7b50`, `0x007d7ac0`, `0x007d7a30`) send a defend order (kind 6, on
the map) at the asset (`0x007d92f0`: the team's object of class `0x9c42`
whose `+0x29c` is the asset's kind, 1 artillery, 2 the satellite, 3 the
UAV) the same way, and `defendMoveTitan` at the team's Titan. The zoom
buttons (`<asset>Zoom`, slots `0x370` / `0x378` / `0x380` → `0x398`,
`0x007d8e00`) send the server event `0xf` with the asset's position (the
commander's camera there), which is not rewritten. `Commander.accept 0/1`
answers the offer of command that follows the 15 s apply period. Caps Lock
opens the commander's screen (the full map and `CommanderShow`;
`--commander-screen` takes command and opens it); the tools'
`CommanderNo*Show`, `*Armor`, `*Reloading`, `*TimeUntilReloaded` and
`*ShowFullStatus` come from `abilityState`, and the minimap shows the UAV,
strikes on their way, supply crates and the squad's order.

The screen's tabs (`CommanderMenu.toggleSquadTab`, `toggleMapTab`,
`toggleCommanderToolsTab`: `0x007a9de0`, `0x007a9e50`, `0x007a9eb0`) flip
the commander menu's `+0x1c0`, `+0x1c3` and `+0x1c4`, the HUD variables
CommanderSquadTabShowFull, CommanderMapTabShowFull and
CommanderToolsTabShowFull (registered by `0x007a8770` with
ShowSquadMemberList `+0x1c1`, ShowSquadLeaderStats `+0x1c2` and
ShowSquadOrderPicture `+0x1c5`); closing the squad tab clears
ShowSquadMemberList. Their first state isn't known (open here). The squad
list (list data 15, `CommanderMenu.singleClick` → `0x007ab380`) has a row
per squad with members, its id in cell 13 and its kind (0) in cell 14
(`0x007a9c90`, `0x007a9c00`). A click on column 4 makes it the only squad
selected and shows its members (`0x007a9f10`: menu state `0x11`, list
data 14, CurrentSquadName, CurrentSquadLeaderName); elsewhere it selects
it alone too (slot `0x1c`; Shift adds it, slot `0x20`, not done); column 3
mutes the leader's voice (slot `0x300`, not done). `CommanderMenu.deselect`
(`0x007a9db0`) closes the members. The list's columns are fitted to the
script's widths.

## Titans (gpm_ti)

`src/Game/Titans.cpp`, `src/Game/CombatAreas.cpp`. The game mode script
(`mods/bf2142/python/game/gamemodes/gpm_ti.py`) runs the round from the
engine's events; the engine keeps the Titans, their armour and parts, their
movement, the silos' missiles and the drop pods.

**The Titan sync manager** (`0x005628f0`, a static at `0x00a67410`,
vtable `0x00962698`). `0x00561c00` registers it for physics event 9
(`ECPhysicsETitanSync`) and makes one sync object per team (`0x005ac0a0`,
vtable `0x009682f4`; the team at `+0x10`) at `+8` / `+0xc`. The server's
frame (`0x004afb20`) and the client's (`0x00590e10`) run `0x00561e20`
with the tick; it returns unless the server's game mode is `gpm_ti`.
`0x005ab390` finds a team's Titan: the RemoteControlledObject of rcType
RCTitan (7, template `+0x29c`; the names are `0x004daf74`'s) of the team
whose armour is no wreck; its root is the Titan, the object's
`minTimeBetweenActivation` (`+0x2a0`, 5 s) kept as ticks at `+0x18`.
`0x00561d90` answers "is this object a Titan" and is asked all over:

- `0x004c0e40` (the game logic's damage): a Titan with no hit points left
  takes none;
- `0x005c8b40`: a Titan is always "in use" for the abandoned-vehicle check;
- `0x005aeb80`: a Titan never blocks a spawner's spot;
- `0x005b4ff0` (a projectile's hit): see the silo missiles;
- `0x00655a00`: a Titan is never repaired; `0x00654120`: its wreck isn't
  scheduled for removal; `0x00656320`: no water or upside-down damage;
- `0x006ca660` / `0x006ca3b0`: what stands on a Titan takes its velocity
  (the rewrite's soldier contacts take the body's velocity already);
- `0x00589890` / `0x005718d0`: kits dropped on a Titan are carried with it:
  the drop (Soldier.cpp, a ray straight down from the soldier) asks each
  team's Titan record (`0x00561b20`) whether the hit object's root is its
  Titan (`0x005aa030`); `0x005ad2e0` then keeps {kit, kit position − Titan
  position} (no rotation: the Titan never turns), the pick-up
  (`0x00561b80` from `0x005718d0`) drops it, and the end of the server's
  Titan tick (`0x00561e20`, also `0x0043f170` → `0x00561bb0`) puts each kit
  at the Titan's position plus its offset (`0x005aa5b0`). Rewritten in
  `src/Game/Kits.cpp` (see [Kits on the ground](#kits-on-the-ground));
- `0x004a5f90`: removing a Titan while the round runs is an error.

**Movement** (`0x005aca70`, a tick at a time). The commander's orders wait
in a map by tick (`+0x3c`): `Commander.moveTitan` arms the HUD (slots
`0x328` / `0x334`), the map's click sends the order, and the server
(`0x0050f1f0` → `0x005abba0`) takes it when the point is inside the combat
areas binding the Titans (`0x005aba40`), 2 s ahead (the tick + round(2 /
tick length)); the commander hears `AUTO_TITAN_CC_moveaccepted` or
`_unablemove` (event 0x2d, `0x00757xxx`). `0x005ab4b0` finds the order in
force (the latest whose tick has come); without one the Titan hangs where it
was spawned. Each tick:

- the push towards the target is min(`*0x00a6afd0` = 0.33, (distance −
  `*0x00a6af98` = 5) × `*0x00a6afa4` = 5), only while the Titan may move
  (`+0x35`: its armour's percent above `*0x00a6afbc` = 60);
- each obstacle within `*0x00a6af9c` = 50 m of its edge (the other Titan,
  and the static objects whose box reaches the lowest Titan's bottom,
  gathered once by `0x00562360`; each by `0x00562090`'s radius, half its
  box's x-z diagonal) adds (50 − gap) × `*0x00a6afc0` = 10 / distance along
  the line away from it (`0x005626a0`);
- a combat area's border within 1.25 radii (`0x005ab740` → `0x004cad50`)
  adds 10 (`*0x00a6afb0`) away from it;
- the sum is scaled back to the push's length (avoidance only turns it);
  velocity = `*0x00a6afb8` (0.99) × (velocity + push × tick); the position
  moves by it across x and z (`0x005aa0e0`: the object's position, the
  node's velocity). It never turns. The top speed is about 1.09 m/s. With
  `sv.allowTitanMovement` off (server settings `+0x59`) it stands.

The combat areas (`CombatArea.*`, `CombatAreaManager.*`; the manager is
`DAT_00a25134`): an area binds vehicle category 4 when its `vehicles` is 4
(all) or 4, 5 binding categories 2 and 3 (`0x004d9cc0`); a point is inside
when it is in one of them, or anywhere when none binds (`0x004ca7d0`).

**Armour.** The Titan's armour (as/eu_titan_playtest: 6,000,000, material
4100) is ticket-shaped: gpm_ti.py sets the team's tickets to
ceil(100 × hit points / maxHitPoints) on VehicleSpawn, VehicleDamage and
VehicleRepair; the HUD's bars span the tickets (HudElementsTitans.con: the
shield 60–100, the hull 0–60). The engine's rules (`0x004c0e40`):

- the damage goes to the hit node's armour or its nearest armoured
  ancestor; under 0.1 nothing;
- `armor.onlyDamageFromChildrenLevel` (template `+0xe4`, −100 by default;
  the Titans' 0) caps damage not from a child at what leaves the armour at
  that percent: shots bring a Titan to 0 and no further;
- after the damage: a part turned wreck (state 1, `0x00651c20`: hit points
  ≤ 0, above −wreckHitPoints) gives its parent `parentDamageWreck`
  (`+0xd4`) % of the parent's maxHitPoints; a part alive before or after
  gives it `parentDamageAlive` (`+0xd0`) × the damage / its hit points
  before × the parent's hit points, both as a child's damage. The check
  is `state after == 1 && state before == 0` (asm `0x4c1793`), but the
  killing-blow limit below keeps any single hit from going past
  −wreckHitPoints from alive (a console: 7,000 hit points, wreckHitPoints
  10, lands at −8 at worst), so an overkilled console is a wreck and the
  doors get their share; later hits on the wreck may destroy it (state 2)
  with no second share;
- the armour update (`0x00654d80`) destroys a part when its parent's or
  root's percent is at or below `destroyOnParentDamageLevel` (`+0xd8`) or
  `destroyOnRootParentDamageLevel` (`+0xe0`), and burns it below
  `criticalDamage` at `hpLostWhileCriticalDamage` a second;
- the setter (`0x00655460`) keeps hit points within −wreckHitPoints
  (`+0xc0`) and maxHitPoints (`+0xc8`); the template's defaults
  (`0x00630040`): 10, 10, parent damages 0, levels −1, −100;
- a wrecked part's subtree loses its collision (`0x00651c60`);
- the killing blow is limited (the armour's damage `0x00656320`, on the
  server): with 0 < hit points < damage, a part takes hit points +
  min(k × wreckHitPoints, (damage − hit points) × f), k = 0.8 and f = 0.5
  (0.1 and 0.1 for a mobile object, 0.9 and 0.9 without `canBeDestroyed`),
  and a Titan's root exactly its hit points.

So the Titan's tree plays out as: the forcefield (as_titan_forcefield,
destroyOnParentDamageLevel 61) goes with the shield; below 60 % the Titan
stops. The four consoles (7,000) each take 2 off the core room's doors
(10; parentDamageWreck 20); at 2, under their criticalDamage 3, the doors
burn at 1 a second, open, and give the core 1 % (400); the core (40,000,
parentDamageAlive 1) takes the Titan down with it share for share, and goes
itself when the Titan reaches 0 (destroyOnParentDamageLevel 0). The
Titan's wreck is VehicleDestroyed, on which gpm_ti.py ends the round
(`sgl_endGame(winner, 3, 25)`; the engine makes the same call itself,
`0x005b7680` → `0x004bbe30(gameLogic, other team, 3, 25.0)`, for a Titan
whose armour state isn't 0: the status stays playing for 25 s, then
ended).

**The crash.** The wreck's armour update (`0x00654d80`'s wreck branch →
`0x00652380`, for a root that is no soldier, with `canBeDestroyed`
(`+0x166`, 1) and timeToStayAsWreck (`+0x158`) not −1) drains it on the
server by wreckHitPoints / (timeToStayAsWreck × 30) a tick through the
game logic's damage (type −5, which the sync's "no damage at 0" check at
`0x4c0eb6` lets by; VehicleDamage keeps coming), three times that with the
AI running and the object mobile (AIDLL's interface `*0x00b47ce4`, slots
`0x18` true and `0x20` false; `GameWorld::setAiRunning`): the Titans'
3,000,000 over 100 s, 1 % a second (3 % with bots). Its armour effects
(`armor.addArmorEffect <level> <effect> <position> <rotation>`, kept in
order by WorldConsole) go off as the percent falls: `0x00654730` (run after
each change of the hit points) takes ceil(hit points / maxHitPoints × 100)
before (`+0x254`) and now (of wreckHitPoints below 0); with the highest
level under the percent now it stops the group started (`0x00652aa0`) and
starts none, otherwise when a level was passed on the way down the group
of the lowest level at or above the percent now starts (`0x006549a0`), in
place of the last, attached to the object (slot `0xf0`) at their offsets,
except levels 0 and −100 (`+0x188`), left in the world; skipped levels'
groups never start. The Titans' groups are 13 … 1 above 0 and −1 … −36
below (the explosive bundle child's 20 … 1), so the explosions run from the
destruction to −36 %; the percent passing −38 (`*0x00a7938c`) sends
ECGameLogicGLTitanCoreExplosion (event 0x14, category 0, `0x004d69d0`)
while the round runs or has ended, 38 s after the destruction (no listener
of it was found: the category-0 listeners test their event id themselves,
like the UAV manager's `0x005a20a0`, and neither a `param_3 == 0x14` test
nor a `case 0x14:` of one on category 0 exists among them; the sites with
`case 0x14:` are other switches. The rewrite notes it in
`TitanInfo::coreExplosion`). Once
timeToStayAsWreck has passed or the hit points reach −wreckHitPoints the
countdown starts (`+0x64`) and timeToStayAfterDestroyed (0) later the
server deletes the object (`0x00651db0`; `0x004a5f90` only logs a Titan
deleted while the round still runs): 100 s after the destruction, 33 s
with bots, after the round's 25 s end delay either way. Meanwhile the sync
still moves the wreck: 0x005ab390 tests the armour of the RCTitan object,
which has none, so the wreck stays its team's Titan; with no push (its
armour under 60 %) its velocity dies away by 0.99 a tick and it coasts to a
stop. **It doesn't fall:** the Titan has gravityModifier 0, nothing on the
wreck's path turns physics on (`0x00654120`'s slot `0x54(0x800, 0)` is a
network dirty mask) and `0x005aa0e0` keeps the object's y and the node's
velocity's y at 0; the "crash" is the explosions, then the deletion.
`GameWorld::stepTitanWreck`, `titanArmorEffects` (the effects in the
world's EffectManager against the Titan's anchor). The gunners are put out
of their seats at the wreck.

The Python component's events by id (the host module's names, as the
templates use them): 25 VehicleDestroyed, 26 VehicleDamage, 27
VehicleRepair, 28 VehicleSpawn, 46/47 TitanPartDestroyed/Repaired (the
consoles), 48/49 TitanCore*, 50/51 TitanWeapon* (the guns), 52/53
GroundCannon* (the silos), 54 MissileDestroyed, 55
CommanderAssetRepaired, 56 VehicleShieldDestroyed. VehicleDestroyed is
raised for every destroyed vehicle.

**The interior.** The Titan's body carries its whole tree's soldier and
projectile LODs, each face reporting its part; soldiers walk its decks and
ride along by its velocity. Its SpawnPoint children (seven in each Titan)
are spawn points of its spawner's control point, moved with it.

**Team forcefields.** collisionGroups 0x100 marks the PAC Titan's fields
(as_titan_forcefield, the vehicle bay's, the corridors', the spawn
room's, the doors), 0x200 the EU Titan's; team 1 owns 0x100 and team 2
0x200. An object of a team passes what carries its own team's bit, the
team being the first PlayerControlObject's up its tree (slot `0x9c`), the
bit read through the response's slot `0x50`: soldier against an object
`0x006c3890`, mesh against mesh `0x006bb050` (already in `meshPair`),
run-over `0x006c2e50`. A projectile's tree has no PlayerControlObject and
its group (8) shares no bit, so rounds meet every field. The shipped
fields' collision meshes carry only the projectile and vehicle LODs (and a
mesh without a soldier LOD stops no soldier), so soldiers of both teams
walk through them; the rule sorts vehicles (the vehicle bay's shield,
material 4117, lets its own team's in) and rounds stop at them unless the
material is penetrable. The rewrite builds each team's soldier mesh only
when a field has a soldier LOD (`Body::teamSoldierMesh`). Vehicles meet
each Titan part's vehicle LOD as a candidate of its own with its
collisionGroups (`Titan::vehicleParts`, `collideMover`), so a team's
vehicles pass their own fields; the Titan, a major actor moved by its sync
alone, takes no push and carries what lies on its decks by its velocity. A
spawner's held spawn (holdObject) is motionless (no step, no mover) while
held, its engines still turning. The
corridor and spawn room fields name their meshes with setCollisionMesh and
no CollisionManager.createTemplate; they are looked for in Meshes/ beside
the template's file (as OpenBattlefield2 resolves such names).

**The penetrable shields** (materials 4112 / 4118, the outer forcefields:
damageLoss and minDamageLoss 1000, overrideNeverPenetrate 1,
maxDamageLoss 100000 by default). DefaultPenetrateComp (`0x005eaca0`,
vtable `0x00975884` slot 7; class `0x953e` on every round): with
overrideNeverPenetrate the solid/liquid tests are skipped; damageLoss −1
means no penetration; the round's damage at its range less the loss must
stay at or above a tenth of its damage, else it hits the face; the loss
is minDamageLoss for a one-sided material, otherwise the thickness to the
way out through the object's mesh (over 2.1 × its radius) × damageLoss,
within minDamageLoss and maxDamageLoss (maxDamageLoss without a way out);
it flies on from a centimetre past the way out with what is left. The
shields are thin shells (about 1.6 m: a loss of about 1,640), so only
rounds over about 1,111 pass: the silo missiles (110,000), static_aa
(2,500) and the EMP strike (3,000); bullets and rockets stop at the
shield. In the rewrite (the projectiles' hit, `Projectile::damageScale`)
the way out is the first face of the same material on along the round's
way. (The call site of `0x005eaca0` wasn't found: no virtual call through
slot `0x1c` in `0x00400000`–`0x00900000` pushes its ten arguments, the
only ones with ten or more, `0x00745945` and `0x0081749f`, being the
menus' and the network's; its last two arguments
are read as the damage left and the round's damage.)

**The guns' seats.** The six gun hubs (eu_groundcanone3*_hub and
eu_aacannon*_hub, the as_ ones alike) are PlayerControlObjects with
seatInformation under the root: the Titan's own seats, so its seat layout
is exactly them. Each hub's EntryPoint (setEntryRadius 5) and seat are at
its console inside (the gunner sits at the bridge's console while the
turret is on the hull); the yaw bundle sits turned with its hub (0, 180,
±90) and is limited (EU_groundcanonE3_Base ±45 and the others' own,
EU_AAcannon_Base −65..145), the pitch bundle too (the ground cannons 5..75
down, the AA guns −89..12). In the rewrite the Titan's body takes seats
like a vehicle's (`enterOrLeaveVehicle`: a seat's own entry leads to that
seat), none of them drives (`drivesSeat`, `PlayerInfo::driving`), the
gunner aims in the hub's frame within the limits (`SeatLayout::frameYaw`,
`minYaw`/`maxYaw`) and `partPose` turns the bundles. A gun whose armour
(the hub's first armoured child: Python.onDestroyEvent 50,
disableInputOnDestroy 1) is out doesn't fire; the Titan's wreck puts its
gunners out.

**Its spawners and supply point.** The Titan's ObjectSpawner children
(as_titan_ATSpawner: as_at_spawn every 10–15 s, AGSpawner: as_ag every
60–75 s; distance 100, two at most, team 1, holdObject; eu_titan_* alike)
and SupplyObject (as_titan_heliRepairRearm: team 1, radius 14, vehicles
only, healSpeed 8, refillAmmoSpeed 12, addVehicleType 3; its storage left
at −1, never spent, `0x005d0650` / `0x005d06a0`) are objects of their own
on its rear deck, carried with it (`Titan::carried`). A held spawn is put
at its spawner each tick, so it rides along. The supply's vehicle types
(`0x005d1710` reads each vehicle's through its template's interface
`0xc4c4`, slot `0x24`: `0x0059d880`, the template's `+0x3d0`, which
`ObjectTemplate.vehicleCategory` sets through slot `0x20`, `0x00504086` /
`0x0059d870`) are its `vehicleCategory`.

**Missile silos** (`isMissileSilo`, missile_silo_v2): the auto controller
CP_CANNON (AI/AutoControllers.ai: fov 179, viewDistance 2000,
reloadOnTeamChange) with the weapon AI missile_silo_Weapon (10–2000 m,
Titans only). A silo held by a team, loaded and in range fires at the enemy
Titan; the missile leaves `fire.fireLaunchDelay` (1 s) later straight up (the
barrel, Missile_Silo_Barrel, turns on its own to its limit: setMinRotation
0/−90/0, setAcceleration 0/−50/0, startAutoRotateOnEnter) at `velocity`
(4); the magazine (1) refills `ammo.reloadTime` (120 s) later, and a change
of hands starts the reload over. The missile (msilo_missile): from
`startDelay` (2.5 s) its `acceleration` (10) pushes it to `maxSpeed` (80);
from seek.trackingDelay (3 s) it turns towards the locked Titan's middle
(FixedTargetComp TTBeacon; DefaultFollowComp's follow.maxYaw 4 degrees a
tick across, follow.maxPitch 1 up and down, predictionFactor 0.3 by
default, magneticBehaviour 1: the first turn after the lock is not capped,
and it coasts without a target; changeYaw 0.2 is never read); for its
first `onlyDetonateOnMaterialTimeLimit` (6.5 s) it sets off only on
material 4100, and anything else in its way that is no Titan's is destroyed
on the server (3e10 damage) while it flies on (`0x005b4ff0`). Its 110,000
damage (less what the penetrable shield took while it is up) meets
veh_titan_armor with a cell of 1 (the turrets' 4113: 3). On Minsk a silo's first missile reaches the other
Titan about 13 s after launch.

**Drop pods.** A GenericFireArm with `fire.isPodLauncher` (ThrownFireComp)
fires its projectile, a Parachute PCO with `isPod` and `enterOnSpawn`
(eu_pod0, eu_pod), which the soldier rides (`dontAllowExit`): the Titans'
pod arrays (titan_pod_array1/2: entered at their EntryPoints, the launcher
40° up at 100 m/s) and the APCs' pod seats (`fire.fireInput PIAltFire`,
pod_launcher at 150 m/s; pod_launcher_cq in other modes). The pod is a
rigid body of its own (gravityModifier 1.5, drag 1, dragModifier 3/1/3);
its ForceObjects steer it (eu_pod_FOFWD 35 on PIThrottle, eu_pod_FOStrafe
20 on PIYaw, × 30 / mass a second as the drones' are). At rest it lets the
soldier out. Not rewritten: the pod's position regulator and keepUp
torque (it is kept upright), lookAtClosestTitanOnEnter, pods against a
shielded Titan.

**Bots.** AIDLL has no Titan-mode strategy (its only Titan code is the
CSTTitan strategic type); single-player BF2142 had no Titan mode. In the
rewrite the bots play gpm_ti with the default strategies: the silos are
capturable control points, the Titans' points are not. The Titan's gun
seats have AI templates of equipment type Fixed (eu_Titan_Cannon1–4AI,
eu_Titan_Cannon_AA_01/02AI: a Unit with setHasExposedSoldier,
Armament setIsAntiAircraft), which AIDLL's seat choice (BBChange) and
behaviours cover, but no strategy of its sends a bot into a Titan (its nav
meshes are on the ground). The rewrite's choice: while fewer than half its
Titan's guns are manned, a bot aboard walks straight to the nearest free
working gun's console and takes it (`titanGunEntry`); the Titan isn't
among the bots' vehicles, so it stays and runs the Fixed behaviours; the
rest walk to the nearest pod array and drop (the arrays'
titan_pod_array_1_ai template makes them vehicles to the AI). Boarding the
enemy Titan and attacking its consoles and core has no AI to rewrite.

**The HUD** (main.cpp's update): GameMode 1 (the Titan HUD shows for 1 or
3), FriendlyTickets / EnemyTickets for the bars, Friendly/EnemyStrategicObject1–4Show
(a console destroyed) and 0Show (the Titan gone); the variables are the HUD
manager's (`0x0044f1a0`). The silos' row is set each frame by `0x0044c350`:
the ten slots' CPCannon%iShow (HUD `+0xdb`..) are cleared, then each object
of class 10 whose first ancestor with a `+0x64` gives an index 1..9 (slot
`0x38`: the silo's control point) shows in slot index − 1, with
CPCannon%iTeam (`+0xe8`) 1 for the player's team, 2 for the enemy's and 0
neutral (the object's team, component `0xc4c5` slot `0x9c`, against the
local player's slot `0xf8`), and CPCannon%iString (`+0x110`) the time to
the next missile as `"%02d:%02d"` minutes and seconds (the object's
`+0x194` slot `0x98`, whole seconds), or `"--:--"` when that is 0 or the
silo is neutral.

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
| Revive (0x10082760) | 100 × the squad factor (task 1) for a downed teammate within the Revive weapon's maxRange. Each candidate scores 1 − min(1, d / speed / time left), × 10 for a human's body. |
| Flee (0x100859d0) | 100 below 30 % health, with the camouflage over 70 % charged (0x10085920: its ammo over its maximum), 10 s after spawning. 0x10085d90 keeps the way out: the sum of the directions away from known enemies (×3 for players, ×2 for bots targeting it). |
| Change (0x1006c520) | best seat / current seat × weight × 10 (best × weight with the current worth nothing), ÷ (1 + 14 clamp((d − 10) / 60, 0, 1)) for the distance to it (0x1006bc50), 0 for a seat another bot reserved (0x1006f320). See Seat values. |
| MedicAssist (0x10070200) | Teammates on foot within the hub's maxRange (70 m) below 75 % health (99 % for a human), the bot itself unless it holds the hub. Score basicTemp / (fuzzy(1 − health) × distance), distance 1 for itself. fuzzy(best) × weight × 2, × 10 for a human; × the squad factor. A "medic" radio call (see Radio) from the candidate: its score × 3 from a human, × 1.5 from a bot; with a human's call heard and the best a human within 50 m: 100 (× the squad factor). |
| AmmoAssist (0x10071a10) | Human players only. A weapon (FireArm, Sniper, C4) with ceil(rounds / magSize) / magazines below 0.6 (0.99 while the bot holds the hub). Score basicTemp / (scurve(ratio) × distance). fuzzy(best) × weight × 4 × the squad factor. A candidate that called "need_ammo" needs only a ratio below 1, scores × 70, and makes the urgency × 100. |
| RepairVehicle (0x10073360) | Vehicles within the tool's maxRange (50 m) between 0 and 60 % health. Score basicTemp / (f × distance), f = fuzzy(1 − health), or 1 for the one it repairs. fuzzy(best) × weight × 10 with a human aboard, × 2 otherwise; × the squad factor. |
| c4 (0x10083750) | 100 to place a charge inside a point the team holds uncontested; 100 to set it off once that point is contested below 0.75 with no teammate within 15 m of it. A charge lapses after 240 s. The weight is not used. |
| DefuseMines (0x10086870) | An engineer (Repair weapon) within 35 m of an enemy mine's stand point (0.8 m behind its facing, on the mesh): 42 × weight. |
| RunOver (0x10087820) | A Car at 10 m/s or more with a known enemy soldier in sight within 50 m, ahead within 0.85, on a clear line over the vehicle mesh: 65 (the weight is not used). |
| Avoid (0x10064850) | On foot only (the 2d pattern of the vehicles' sets never runs); soldiers are never avoided. A wreck or vehicle below 30 % health within max(r, min(10, r + 4)) adds 100. A vehicle at 2 m/s or more, closing at 3 m/s or more, that will pass within 0.6 (r + own radius) × √1.5 in the look-ahead time adds closing speed / distance × its size factor, which is always 0 (see below). Sum × weight, × 2 in the level's first 5 s (see below). With a sum of 0: 1000 while a grenade warning lasts (the weight not applied). |
| BridgeRepair (0x10084620) | 0: the rewrite has no broken bridges. |

- **Squad factor** (0x10089aa0): 0 outside squads; 1 for the bot's own
  task (Medic 1, Repair 2, Ammo 4). For a target outside squads: 0 when a
  teammate has the task or the bot's squad is more than 141 m away. In the
  bot's squad: 0 when a squadmate has it, or (medic and ammo) when the
  target is a living squadmate with the task's own kit. In another squad:
  0 when taken or that squad is 31.6 m or more away, else 0.5, or 0.1
  when that squad has a soldier with the kit (0x100c2830). The kit is the
  player's KitType (`+0x58`, player vt+0x1a4, through the AI player's
  slot 0x40; KitType 0x0088f6b0: Recon 0, Assault 1, Support 2, AT 3,
  Engineer 4, Medic 5, Specops 6, Sniper 7): Medic 5 for heals, Engineer
  4 for repairs, Support 2 for ammo. No shipped kit is a Medic (Assault
  heals). ×2 for a human (0x100bf510).
- **Radio** (the game's event 0x1004e140 → 0x10030b20 → the comm
  object's 0x100a6fe0): a message said by a player and heard by a bot is
  kept under its type (BF2142.exe's table at 0x0088f000: bail_out 0,
  get_in 1, req_pickup 2, medic 3, need_repair 4, roger_that 6, negative
  7, need_ammo 10, ..., req_new_order 14, defend 15, attack_position 16,
  move 17) with its sender, a newer one replacing the older, for its
  receive time to live (`aiComm.addAIRadioMessage`, AI/AIRadioMessages.ai:
  20 s for medic and need_ammo; 0x100a7130). Who hears is the game
  server's 0x004a6f70 (see the HUD's comm roses: `GameWorld::hearsRadio`);
  `BotManager::update` takes the bots' share of what the world heard.
  The bots' own radio, the comm object's sends (0x100a7860): a type is
  queued `delay` s on unless its next time (+0x3c) hasn't come, which
  queuing sets the type's max send period on (+100: the
  addAIRadioMessage column); the comm's update (0x100a7af0, every bot
  tick) sends what is due (0x100a57c0 → the AI game effector's
  0x00896270, the AI's channel mapped: 0 TCLocalTeamMedic, 1
  TCLocalGroundVehicle, 2 TCLocalAirVehicle, 3 TCSquad, 4 TCCommander, 5
  TCSquadLeader, 6 TCLocalTeam, 7 TCVehicle, 8 TCPlayer) unless one of its
  type was heard within the send period. The senders rewritten: the
  low-health call (0x100a7910: unless airborne, +0x140, or in an ITAir
  vehicle over 3 m/s, the bot's object between 0 and half its health —
  0.75 aboard — "medic" on channel 0 on foot, "need_repair" on 6 aboard;
  a squad member whose AI player's slot 0xf4 answers 1, and the bot's
  +0xfc, set by the game's AI event 0x1004e220, hold it back: neither is
  traced, so neither does here); the get_in answers (an unanswered
  get_in whose first sender rides a vehicle, marked answered by
  0x100a61c0: a choice that isn't that vehicle answers "negative" within
  15 m of it, 0x1006be70; heading for it, "roger_that", 0x1006dac0; both
  channel 6, 1 s on); a hornless driver's "PLAYER_TACTICS_gogogo" to the
  soldier in the way (0x100a5c00 → 0x100a5990, at once).
  `BotManager::radioSent` lists what a bot said.
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
  - Avoid (0x10065360): walk 10 m from a wreck or 15 m from a grenade
    (the avoided object −1; straight away, else to either side, by mesh
    rays), or 10 m across a vehicle's motion on the bot's side. It ends
    after the time it allows (a grenade: what is left of the warning) or
    within 2 m.
  - **Avoid's values:** the look-ahead time is the Mobile part's slot
    `0x68` (`0x100a44f0` → BF2142.exe's Mobile adaptor, vtable
    `0x009d6a50` slot `0x68`, `0x008a1830`: the Mobile template's `+0x44`).
    The template's constructor (`0x008917b0`) sets it to 2 and no
    `aiTemplatePlugIn` property writes it (`lookAheadTime` is `+0x20`,
    `0x008840e7`): 2 s, ×2 prone, ×1.5 crouched. The moving vehicle's term
    is multiplied by its Physical part's slot `0x48` (`0x100aae70` → the
    Physical adaptor `0x009d7228` slot `0x1c`, `0x008a3f30`: the Physical
    template's `+0x8`). The constructor (`0x00894450`) zeroes it and
    Physical's only property, `setStrType`, writes `+0x18`: the term is
    always 0, so moving vehicles never raise Avoid in the shipped game.
    A non-zero sum is also scaled by the running-behaviour record's `+8`
    (bot vt+0xe0 → `+0x4c`) when the bot's hint names Avoid (vt+0x68,
    `+0x9c`) and its `+0x50` holds (vt+0x64): made by `0x100ea3f0` and
    reset by `0x100ea410` to 1 and never written otherwise, so × 1 (the
    assists' and Revive's uses alike). It is doubled for a soldier within
    5 s of its comm object's (vt+0xa4) `+0x78`, which only the comm's
    reset (`0x100a7630`) writes, to 0: the clock (IAIGame's, BF2142.exe
    `0x00666070`) counts ticks × 1/30 s from GameLogic's reset
    (`0x004ce910`, `0x006660b0`), so the sum is doubled in the level's
    first 5 s (here the bot manager's).
  - **The grenade warning** (`0x1004eab0`, from the same event as the
    rounds passing by, `0x1004ed30`, for every bot): a round whose AI
    weapon's template is thrown (`+2`, `0x0088a650`), has an explosion
    radius (`+0x20`) and doesn't explode on impact (`+0x4e`), within that
    radius of the bot, with the round or its thrower in front (the unit
    direction to the round, or the raw offset to the thrower, over 0.4
    along the bot's facing). No team test. The bot's path follower keeps
    the round's position and the weapon's stamp (`0x10092920`): AI weapon
    `+0x20`, the clock when it was taken in hand (`0x008a6d80`, slot
    `0xfc`). The warning lasts until 2.8 s after the stamp (`0x10092960`).
    BF2142.exe's sender of the event wasn't found: the rewrite checks the
    rounds in flight every tick, with the thrower's weapon in hand.
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
- **Sharing:** sightings go to the others in the same vehicle (0x100a8ce0),
  and a bot's fire plan tells its squad of its target (0x100795a0 →
  0x10089b70 → 0x100c1ce0). A memory item learnt so is flagged 8
  (0x100ed2c0) until the bot sees the target itself; every sighting
  stamps its `+0x58`.
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
- **Explosives** (the weapon choice 0x10074ef0, the plan 0x100777a0, the
  aim 0x100dbd10): a weapon with an explosion radius against a soldier
  scores × 100 and sets the bot's `+0x249` when (a soldier bot) the target
  is 5 m away (10 m when not over 3 m higher) up to maxRange (+ 1.25 dh
  over 5 m higher, not thrown), seen within 10 s and known only from
  others (flag 8) or over 30 m higher (not snipers or thrown weapons), the
  plan's condition hasn't failed within 10 s (`+0x1f8`) and the bot's last
  explosive round (the third last for an automatic weapon not thrown;
  `0x1004e910` stamps `+0x1e0..`) is over 10 s old; or (in a vehicle)
  minRange..maxRange, seen within 5 s, not failed within 5 s (`+0x250`);
  ±1 m with the aim lobbing. With `+0x249` the plan adds a start
  condition: a soldier's (`0x100d8800`/`0x100d8910`, before the aim's):
  no teammate within 7 m of the target point, and the throw's way clear
  (a ray to 3 m along the aim, or with a lob to the arc's top, then to the
  target), or the first ray stopping within 3.5 m of it; a failure blocks
  it for 10 s (`+0x1f8`) or, lobbing, until the next sighting (`+0x200`).
  A vehicle's (`0x100d8400`/`0x100d8450`, after the aim's): the barrel's
  line to 5 m past the target hits within 3.5 m of it with no teammate
  within 3.5 m of the hit. The aim lobs (the higher arc, `0x100fa440`)
  a soldier's explosive at a target known from others or that high,
  unless a lob failed after the last sighting, and for a weapon not
  thrown only with the target over 10 m higher or steeper than 0.6;
  otherwise it aims an explosive at the ground under the target. Such a
  plan may fire at a target out of the bot's sight.
- **Weapon choice** (0x10074ef0, `chooseWeapon`), over the bot's object's
  weapons (the AI weapon's slots through BF2142.exe's adaptor, vtable
  0x009d5be8): usable ones (slot 0x1c) of type FireArm, C4, Revive,
  CloseCombat, Sniper, FireAndForget or TitanWeapon; without
  `setFiresThroughTransparent` (+0x4c, 1 by default) only at a target
  seen clear (memory flag 0x40) or known from others; C4 not while the
  bot's own charge is out (+0x1b8); none without rounds (slot 0xc, all
  magazines; −1 counts 0x10000) unless Revive. The score is the strength
  against the target's type, × 0.5 against a scripted bot (+0x264),
  × (3 − EMP level) with `isEmpWeapon` (+0x4d, 0x10074c60). CloseCombat
  and Revive need the target on the bot's mesh outside the vehicles round
  it (0x100947b0) and count only soldier to soldier within 3 m or when
  "going in" (a human soldier within 20 m, moving, with its magazine
  empty, and the bot's own empty or a melee weapon in hand); with the
  mesh ray to the target clear such a weapon is taken at once. The
  explosive bonus (above) needs the weapon unguided (+0x6c 0) and, for a
  soldier, neither its `+0x248` nor the area guard: of the areas its
  team doesn't hold (+0x70) the one with the most radius / max(1, d²)
  from the team's spot (0x10034a10, slot 0x68 = 0x1005dd10 → 0x1005dd50
  → 0x1005dd90: +0x98 + 4 team over d²); within 40 m of its centre while
  pushing PIYaw, PIThrottle or PISprint, or with the target within the
  radius of the spot, no bonus. Then an empty magazine of an unguided
  weapon with `ammo.autoReload` and max(reloadTime,
  minimumTimeUntilReload) over 1.9 s scores 0.1; wire guided with a
  target locked × 10; without the bonus, below minRange × (0.1 − 0.09
  min(1, (minRange − d)/5)), beyond maxRange × (1 − 0.9 min(1, (d −
  maxRange)/5)); an empty magazine × 0.33. The best above 0 wins; none
  keeps the last choice. The guidance kind is the target component's
  `ObjectTemplate.target.targetSystem` (+0x80; GenericFireArm slot 0x154
  0x005c1fa0 → slot 0xb4 0x005f5b80, through 0x008923a0): TSNone 0,
  TSHeatSeeking 1, TSLaserGuided 2, TSWireGuided 3, TSTVGuided 4,
  TSParabellic 0.
- **Lob hold** (+0x209): a lob whose start condition held with a weapon
  that isn't thrown sets it (0x100d8910, vt+0x20c(1)); the move plans'
  throttle (0x100e6060) then stops: the follower's timers restart
  (0x100923a0) and the bot crouches (vt+0x170(2, forced)). It falls when
  the lob's condition fails, when the fire plan ends (0x100d4e20, with
  `+0x248`) and when the plan's target condition fails while not lobbing
  (0x100dc730).
- **Aim:** the PID controller of the ControlInfo plugin (0x100e5820:
  the gains of the bot's control object, MouseTurretAimAt 0x1008e940
  passing its AI part's `+0x38`: the soldier's SoldierCtrl, proportional
  −7, derivative −0.01, angle speed 0.4, or the seat's, e.g. a tank
  gunner's −4 / −1 / 1.5, the as_tank driver's −1.5 / −20 / 0; read
  through 0x100cf7e0 / 0x100cf800 / 0x100cf820, adaptor slots 0x3c / 0x40
  / 0x44; on the lead point; yaw limited to 3 × skill + 1). Its outputs
  go to the control object's look inputs (slots 0x30 / 0x34: lookVertical
  and lookHorizontal, PIMouseLookY / X in every gunner seat), each only
  when the PlayerInput's mask has that input. The lead point is the ballistic lead
  (the aim at an object, 0x100d9a60 → 0x100fa6f0, precision 0.5: see
  Helicopters, "The ballistic lead"): the launch direction for the
  weapon's round (speed, drag, gravity; indirect weapons the high arc)
  against the target's velocity, with the shooter's own; the explosives'
  lob keeps its own arc (0x100fa440). Its output is PIMouseLookX/Y, which the
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

- **The ControlInfo plug-in** (BF2142.exe, constructor 0x00891d80, AI
  adaptor vtable 0x009d6b98): `+4` proportionalConstant (−7), `+8`
  derivativeConstant (−1), `+0xc` angleSpeedConstant (1.5), `+0x10`
  yawScale (1); the inputs as c_PI indices (0x40 none; each setter also
  sets the input's bit in its group's 64-bit mask, drive `+0x28`, aim
  `+0x40`, look `+0x50`): drive throttle `+0x14`, turn `+0x18`, pitch
  `+0x1c`, roll `+0x20` (setters 0x008856e1, 0x008857f9, 0x008863e8,
  0x008862d0), aim throttle `+0x30`, vertical `+0x34`, horizontal `+0x38`,
  roll `+0x3c`, look vertical `+0x48`, horizontal `+0x4c`;
  setCameraRelativeDofRotationOffsetDeg `+0x60` (radians), the
  camera-relative min / max rotations `+0x64` / `+0x70` (degrees × π /
  180, each clamped to ±π: 0x00891fa0, 0x00892070; −π..π by default; the
  scan statement 0x100f9a90 takes them as the gun's yaw range, free when
  both are 0 or the range spans ±3.14), maxClimbAngle `+0x58` and
  maxRollAngle `+0x5c` (adaptor slots 0x7c / 0x80, read only by the plane
  controller 0x100e6d00; no shipped template sets them), isFloating
  `+0xfe`. The gains are read by nothing but the aim controller; the
  drivers' controllers read yawScale (cars, the mech) and the input
  indices (the helicopters' PIRoll from slot 0x84). `AiPlugin`,
  `BotManager::aimGainsOf` (looked up once per control object).

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
- **Area units** (0x1005f210): the temperature is the area's base plus
  each unit's basicTemp (aiTemplate.basicTemp, the AI object's +0x14) ×
  its certainty, empty vehicles and every seat's object included; the
  friendly and enemy strengths sum the manned units' battle strengths ×
  certainty (`+0xc0`, `+0xc8`). A unit's strengths (0x100aa740) are its
  Unit plug-in's setBattleStrength / setStrategicStrength × the capability
  factor 0.25 + 0.5 S(ammo) + 0.25 S(health) (0x100aa420); a soldier's
  battle strengths are the most of its weapons' (0xc4c5 slot 0x74's items)
  and its strategic ones its kit's kitTemplate's (the one its kit's AI
  template names, 0x0088c0d0; kitTemplate.create name kitType, vtable
  0x009d5d68). `BotManager::unitStrengths`, `unitAiOf` (cached per
  template).
- **Assignment:** the best NumberOfAttacks and NumberOfDefences areas
  (0x100a21a0, 0x100a2630) get squads, nearest first, until each has its
  required strength: the enemies' strategic strengths (each rounded; index
  1 in an owned area, 0 elsewhere), at least 1, × 2 when owned, else × (2
  − S(the friendly share of the strengths)) × 1.25. An area releases a
  spare squad.
- **Anti-aircraft** (the Armament plug-in's setIsAntiAircraft, 0x00884739;
  read through 0x100f0840): Fire3d's motion test and doubled temperature
  take it (0x1007d6c0); the fire plan's ITAir clause (1: fight from where
  it is; 2: hold the order spot) isn't rewritten.
- **degeneration, allowedTimeDiff, commonKnowledge** go to AIDLL when an
  object is registered (0x008a3980 → 0x1003b900): the knowledge decay out
  of sight, the update throttle (0x100a9e70) and per-team (0, every
  shipped template) or shared knowledge (0x0088d0d0). Stored, not used:
  the rewrite's sides know every unit.
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
    plans again. A soldier that hasn't moved for half a second, and hasn't
    jumped for 2 s, jumps: the stamp `+0xc0` goes to −666, the next tick
    (0x10095720) presses PIAction (input 9) and stamps the clock, and
    0x10092bc0 lets go once the stamp is 0.4 s old.
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
    over 10 m/s brakes fully and swerves (error −2). The same scan finds a
    teammate on foot ahead (dot above 0) within 15 m beyond both radii on
    the line of travel; with the vehicle's driver's seat taken
    (`0x100aa530`) the driver sounds the horn (`0x100a5c00`) at most every
    5 s (20 s when that soldier is a bot, player slot `0x84`): a vehicle
    with ITHasCarHorn (the FAVs' `Car_horn`) holds PIFire for 2 s
    (`0x100a5cf0` lets go); any other sends the radio request
    (`0x100a5990`: 0xb, 6, 3), which the rewrite's bots don't have. Not
    rewritten: the other driver being told (its +0x28 calls).
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

### Helicopters (src/AI/Heli)

The pilots' seats (equipment Helicopter: as_ag, us_ag; TransportHelicopter:
as_at, eu_at) run AIDLL's 3d patterns with the HeliMoveTo family of
interpreters (registered in 10020000.c: HeliMoveTo 0x1008e2e0,
HeliMoveToObject 0x1008e1c0, HeliAimAt 0x1008db80, HeliResetControls
0x1008da80, HeliMoveAwayFromObject 0x1008d8b0; AirTransportMoveTo
0x1008cc40 is unused: no shipped template is AirTransport or Plane). The
sets (AI/AIBehaviours.ai): Helicopter MoveTo (BBPGotoWaypoint3d), Idle
(BBPIdle3d, the default), Fire (BBPHeliFire3d), Change; TransportHelicopter
the same without Fire and with BBChangeLandingCraft. No aircraft set has
Avoid.

- **Inputs** (the ControlInfo adaptor, BF2142.exe vtable 0x009d6b98):
  PIThrottle (the collective, 0..1 from the AI), PIYaw, PIPitch, PIRoll;
  they persist between ticks. PIPitch > 0 lowers the nose, PIRoll > 0 dips
  the right wing, PIYaw > 0 turns right. The rewrite hands PIPitch through
  `SoldierInput::pitch` and PIRoll as the turn (5° a unit).
- **Primitives** (100e0000.c; constants AIDLL .rdata 0x1012ad28...):
  attitude 0x100e4440 (hold forward.y or right.y at a target: clamp((e −
  rate) / 0.5) with e = clamp(error / 0.02) × scale), brake 0x100e4960
  (the nose up by 0.3 × sign(v) × min(1, (|v| − deadband) / 5)²), yawTo
  0x100e4550 (PIYaw = clamp((−1.5 clamp(diff / 30) − yaw rate) / 0.1),
  headings atan2(−x, z)), holdHeight 0x100e4130 (wanted climb 5 ×
  clamp(dy / 5) m/s, collective clamp((wanted − v.y) / 2, 0, 1), levelling
  sign × min(1, e²) terms; airborne once over 5 m), terrainCheck 0x100e4a70
  (probes every 5 m ahead, 2 + 0.01 s² of them, at least 5: under 5 m of
  clearance it brakes and climbs, tilted it recovers, a wall it turns from
  by the side with more room, 12 samples every 22 m), hoverAt 0x100e4fa0.
- **HeliMoveTo** (0x100e50b0): over the highest probe + margin; within
  20 m and not far below, hover or brake (deadband 3..5); else turn (over
  45° off: brake first) and pitch for (1 − 0.9k) × 35 m/s × min(1, d / 50)
  — a fixed 35 m/s, not Mobile maxSpeed. A margin of 0 lets the collective
  go (0x1008e406).
- **BBPGotoWaypoint3d** (0x10067690): no mesh, no path, no strategic
  waypoints. The goal is read once; the hover point is 25 m over the
  highest of it and 100 terrain samples 2–30 m round it (0x100674f0); done
  within 2 m. Not yet airborne the bot first sits 10 s with the collective
  down (While(10 s) HeliMoveTo(its spot, margin 0)).
- **BBPIdle3d** (0x1006ab50): on the ground the controls let go; airborne
  a transport hovers 25 m over its goal (or its spot), a gunship (Armament
  on the root) flies on 500 m ahead and right.
- **BBPHeliFire3d** (0x1007f9d0 / 0x1007f180): to the target (margin 25)
  until within 30 m, then attack runs: away to 150 m (margin 40) and back
  (margin 50); on the way back, 20 m to maxRange ahead (dot over 0.5) with
  the line clear, HeliAimAt (0x100e5590: margin 40; too steep, fly on
  150 m; lined up within 10° turning under 0.1 rad/s, the nose follows the
  aim with a feed-forward) and the trigger every 60 / round((1 − (1 −
  skill) 0.8) round(1.5 rpm)) s. The runs only inside the combat area:
  the plan's If (0x100d29f0) on 0x100dd3e0 (type 9; its 150 at +8 is
  never read), which asks IAITerrain slot 0x1c (BF2142.exe 0x008a0380 →
  the combat area manager's 0x004ca5d0: the areas that apply to the
  player, 0x004d9d50 — its team or none on either side, binding its
  controlled object's vehicleCategory — inside a plain one and in no
  cut-out; with none, the heightmap's bounds), else HeliMoveToObject at
  the target, margin 25 (`GameWorld::insideCombatArea`). HeliAimAt's aim
  is the ballistic lead (below) from the gun along the hull, the hull's
  motion inherited (an indirect weapon: precision 0.75, none inherited,
  the direction laid flat), else straight at the target (the aim's slot
  8); the trigger's condition is the on-target test (0x1007ec50 builds it
  with the AI weapon's allowedDeviation and its fire mode).
- **BBFire3d** (behaviour vtable 0x10116d40 slot 9, 0x1007e1b0; the score
  0x1007d6c0; `BotManager::fire3dUrgency`): over the known targets, each
  `facing × reach × 0.5 × temperature × motion × freshness × strength ×
  recency` (the factors in Bots.cpp: the vehicle's best weapon strength,
  1.5 − 0.5 clamp(t/30) after the target hurt the bot, 1/(0.05 t + 1) once
  lost, the target's motion against the bot's (none for an air target
  over 5 m up without anti-air guns), 20 a vehicle and 1 a soldier × 0.75
  outside the order's area, 1 − d/(3 maxRange), or for an air target
  1 − d/(1.5 maxRange) with the temperature doubled by anti-air guns,
  max(0.5, 1 + facing)); the target kept unless another scores 1.2 times
  more; urgency fuzzy(2 best) × weight. Left out: the 20 s a failed
  target is passed over (+0x30, whose writer isn't traced), the unit-count
  scaling (0x100d2b30) and the Unit's capability flags (0x100cf5c0/a0,
  bot vt+0x180).
- **BBChangeLandingCraft** (vtable 0x10116970, 0x1006f460; the transport
  helicopters' pilots and their ParatrooperFixed passengers;
  `BotManager::landingCraftUrgency`): the vehicle's seats weighed as
  BBChange weighs them (0x100d3e70) × 1 for the bot's own when it drives or
  the driver's seat is taken (else 0.25), × 1.5 for a passenger looking at
  an ITAir root's driver's seat, × 0.25 for the others; a free driver's
  seat of a Mobile root + 100. Near the order (the order's spot within
  15 m, 50 m in an enemy area: 0x100bf800 with 225/2500) a driven
  transport says bail_out (0x100a7860: type 0, the AI's channel 7) and one
  with nobody at the controls (0x1006f3c0) lets its passengers out within
  10 m (40 m), urgency 4. Otherwise a better seat: fuzzy(best / current /
  2) × 4, × 6 within 15 s of taking the seat, 0 for a seat another bot
  reserved (0x1006f320). Not followed: the ground transports' way out (no
  shipped ground vehicle runs it).
- **The ballistic lead** (`src/AI/Ballistics`, `ai::LeadSolver`; the
  solver record 0x100f9fd0, set up by 0x100fa060, solved by 0x100fa6f0):
  every aim that leads (the turret and soldier aim at an object 0x100d9a60,
  HeliAimAt's 0x100dcf10, the aim at a position 0x100dd6a0) solves for the
  launch angle in the vertical plane through where the target will be.
  The round: the muzzle speed (the AI weapon template's +0x28, the
  firearm's velocity), the drag (+0x14: the projectile's drag over its
  mass × π/100 for a Point or RotationalPoint, 0.3 for a Mesh with any)
  and the gravity (IAIPhysics slot 4 × the projectile's gravityModifier,
  +0x2c), all filled by 0x00892be0 through 0x0088a8c0; indirect (+0,
  weaponTemplate.indirect) asks for the high arc. The first guess of the
  flight time is the distance over the closing speed (the muzzle speed plus
  the shooter's motion less the target's along the line; under 1 m/s no
  solution). The search starts from the last solve's angle (the record
  keeps it, 666 at first: straight at the target), the high arc from
  3π/8 and pulled halfway to 1.508 rad each solve with the step reversed;
  each of round(8 p + 2) iterations (p 0.5 at the aims: 6) computes the
  flight time to the target's distance (with drag, 0x100fa100, in the AI
  clock's 1/30 s steps, quadratic over 200 m/s), the round's height there
  (0x100fa200) and the target's new place at that time, and moves the
  angle by its step towards the target; once the target is bracketed the
  step halves each time. A search that never brackets doubles the step
  (up to 0.017 rad upwards) for the next solve, and with a step of a
  degree or more succeeds without a new direction (0x1008e940 then aims
  along its zeroed local; the rewrite keeps the straight line). Over 60 s
  of flight it starts again straight at the first guess; over 100 s, an
  angle past ±2.98 rad, a speed along the ground under 0.1 m/s or a
  non-finite point: no solution. The record's +0x68..+0x6a, which would
  pin the time or drop the point, are never set.
- **The on-target test** (0x100dcab0 / 0x100dcb00, vtable 0x1011b048;
  `ai::OnTargetTest`): with a solution, the gun's forward within
  atan(0.0125 d) of its direction, and a round fired now along the gun
  (0x100fbad0 → 0x100fa680) within d of the solution's impact point at its
  flight time, d the AI weapon's allowedDeviation (slot 0x94 → 0x008926f0,
  the template's +0x44, 0.5 by default; the plain fire statement 0x1007fec0
  passes its own). A weapon that isn't automatic (slot 0x104 → 0x00892650:
  the fire mode isn't 2) waits for the closest approach: while the miss
  shrinks it holds; the test after it grows holds when the best was
  within d.
- **Measured** (flat ground, `Vehicles_BotPilotsFlyToAPointAndHover`):
  the gunships fly 156 m to a point 25 m up and stop on it within 1 m; the
  transports reach it (1.5 m) from 22 m/s and wander round it within 35 m
  (they brake only within 20 m). In a round (minsk_coop gpm_coop 32, 31
  bots, 300 s) a PAC bot flew as_ag 2.2 km: 10 s on its pad, a climb to 25
  m over the ground, the flight to its order, hovering, attack runs.

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
mesh (0x100880e0), a test 0x1006c520 runs only for a bot object that
isn't ITAir (its flags' bit 1): a pilot stays in the air. A seat another
bot reserved is worth nothing to others, but not to the bot sitting in it
(the rewrite's reading: a passenger heading for the vehicle would
otherwise turn its driver out). The other seats' factor (0x1006b910): 1 with the
driver's seat taken; else, for a Mobile root that isn't ITNaval, 0.77 for
ITGround and 0.5 for ITAir. The AI flags are the template's
`aiTemplate.addType` bits (BF2142.exe 0x0088fbd0, 0x00882ff1): ITUnit 0,
ITAir 1, ITHelicopter 2, ITGround 3, ITNaval 4, ITControlpoint 5,
ITTransportation 6, ITMobile 7, ITStructure 8, ITSoldier 9 (also set
through 0x0088d070), ITObstructedView 10, ITNoTemperature 11, ITHasWreck
12, ITObstacle 13, ITHasCarHorn 14; ITBiological sets none.

In a vehicle the driver's current value is 2 × its seat's (250 with
others aboard); a passenger starts from the driver's seat as another's.
Unless it drives an ITAir root, the root's other seats follow × that
factor (1 for a passenger with the driver's seat taken), the passenger's
own seat as its current value (× 2); then the way out (see above). A seat
of the same vehicle is reached by pressing its select key
(`aiTemplatePlugIn.setSelectKey PIPositionSelectN`, 0x100aa5f0; the plan
0x1006dac0) and waiting to sit in it; seats are reserved one by one. Not
rewritten earlier as "the type-0/1 order multipliers": those are the radio
lists (the comm's +0xc, per type; read at their first message from a
human alive and not down). bail_out (0): in a vehicle the way out ×
modifier × 100 (`aiComm.addAIRadioMessage`'s modifier column, the
message's +8; 10 shipped), 10000 in an ITAir object, and no other
vehicle is weighed. get_in (1), on foot: a vehicle of the bot's team that
a sender rides (0x1006c080) × the modifier; when the first sender rides
the vehicle the bot already heads for, only that one counts, × 5. From a
vehicle the other vehicles are weighed as on foot (the own one left out)
unless the way out is already best, a bail_out is held, the bot's object
is ITAir or it is off the mesh; a driver of a Mobile root (and a soldier)
takes one with `setUseNoPathfindingToGetToObject` (Unit +0x15, the
bipods), one without Mobile, or one whose infantry-map spot lies within
the two radii; a passenger any. The choice of another vehicle gets out
and walks there.

### Not decoded, and the rewrite's stand-ins

- **Scheduling:** the CPU budgets are real time (see Scheduling: they
  depend on the machine's speed, so they can't be reproduced; every slice
  runs to its count, the SAI once a second). That holds for the path
  searches too: the segments of three clusters are rewritten, but
  `0x100c6ee0` gives the bots waiting for a path (`+0xd9`, `+0x150`)
  search slices round robin (cursor `+0x100`) while the clock is within a
  tenth of the frame's budget, hands 0.9 of what is left to the
  pathfinder (`DAT_101394d4` slot `0xdc`) and completes the finished ones
  (`0x100f9ce0`); the rewrite finishes each search at once. Each think
  evaluates every behaviour (the original's think stops at its budget,
  the same real-time clock, and the rest move up a priority level for the
  next).
- **Aim:** bots don't zoom. (The head twists before the body for every
  soldier: see `SoldierControl::twist`, 0x0058a6d0.)
- **Deviation:** the AI deviation (0x00892b70) widens the weapon's own
  deviation through the soldier (vt+0xf4); the rewrite's hand-held
  firearms have no deviation model, so the bot spreads its aim uniformly
  over ± half of it on each axis.
- **Explosives and the weapon choice:** the rays bisect
  `GameWorld::lineOfSight`. `+0x248` is set by the fire plan (0x100795a0)
  when it adds a move to the order's spot (0x100d11c0; 0x100775f0 tests
  the spot set): bot vt+0x254 (slot 149, 0x1009b370) with 1, from the
  builder's two MoveToFinding sites; cleared by the statement 0x100d4e20
  and the bot's resets (0x10097..). The builder decides it for a new
  target (the pattern's per-bot byte, set by its slot 7 0x10077650 when
  the target changes): the spot (the order's area position for the bot's
  map, 0x100c1150) set and reachable (the follower's slots 4 and 8), and
  either the target can't be approached (no Mobile part of its own or its
  root's, and the pathfinder's slot 0x94 refuses the way 30 m from it
  towards the bot) or the bot stands more than 30 m (50 m when neither it
  nor the target is ITSoldier) from the spot with the target on the far
  side (dot under 0); never with a close-combat weapon (type 7).
  `BotManager::fireKeepsToOrder`; the plan then walks to the spot while
  it fights. Stand-ins: reachability and the approach are "on the mesh";
  the air-target clause (target ITAir and the plan weapon's slot 0x28 at
  2) isn't traced. The rest of the 14.7 KB builder is the rewrite's
  BotManager::fire.
  Stand-ins: the target's Mobile slot 0x78 (0x008a1960: a soldier's
  interface 0xc4c5 slot 0x108, not traced) is its speed; a human's
  attention on the bot (the sensing agent's stamp 0x100eb7f0, whose
  writer wasn't traced) is a round of it having passed the bot; every
  sighting is clear (no see-through materials); vehicle guns have no
  magazines; no bot is scripted; no wire-guided weapon holds a target
  (LPTargetComp's target isn't modelled); the area guard's per-side spot
  is the control point (0x100623c0 starts it at the area's position) and
  its per-side radius (+0x98 + 4 team: the spot's distance to the area's
  +0x118 point) the control point's radius. Left out: C4's guard on a
  moving target (|Mobile slot 0x18 · the bot object's +0x80| over 4, the
  object field not traced); the guided weapons' own fire plan on foot
  (0x10078570, picked when +0x6c isn't 0). `+0x209` falling with the
  target condition 0x100dc730 is taken as the aim no longer lobbing.
- **Change:** the infantry-map reach test for vehicles (the Mobile's
  per-map points +8/+0x1c/+0x30 aren't kept: every vehicle counts) and the
  room test for ITAir vehicles that aren't ITHelicopter (0x1006c3e0). The
  get_in answers are rewritten (see Radio), and the bail_out radio on
  taking the way out: 0x100a5990(type 0, the AI's channel 7) when the
  bot's object's health is at or under the slot 0x68 value + 0.1. That
  slot is the bot manager's (`DAT_10139500`, constructor 0x100c9c50,
  vtable 0x10119738 slot 0x68 = 0x100c5cd0: its +0xdc, 0.3), not
  IAIGame's: the same floor the seat values refuse vehicles under, so 0.4.
- **Soldier radius:** the object's size through the AI adaptor (vt+0xdc
  → `0x008a3d40` → the object's vt+0x90, for the soldier `0x006f6b90`):
  its collision mesh's radius (`+0x4c`, interface 0xcaa32, the instance
  `0x006e5d70` whose slot 0x74 → `0x006dedb0` reads its first LOD's
  `+0xc0`), plus vt+0x94 (0) and at least each child's radius plus its
  offset (none for the soldier's). Every soldier names the built-in
  `Soldier_CollisionMesh` (`0x006d8740` → `0x006e4880` without a file:
  points within x, z ±0.4 and y −1..0.8), whose box's farthest corner
  (`0x006aeda0`) is √1.32 = 1.149 m.
- **Runs without the AI scripts** (tests): a driver is a Tank when its
  seat has a gun and a Car otherwise, other seats are Fixed, vehicles
  count as LightArmour, and vehicle guns reach 100 m. Every shipped
  vehicle and weapon has its `ai/*.ai`. Levels without
  `StrategicAreas.ai` (three of the 46 with a mesh) leave the original's
  SAI without areas; the rewrite deviates and makes one per control point.
- **Strategy:** every unit's certainty is 1 (the side knows everyone; the
  original's falls by 1 − S(t / degeneration) out of sight, 0x100a9b90),
  a unit's ammo counts as full in its capability factor, and a seated
  soldier counts only through its vehicle's Unit (whether it counts on
  its own isn't settled).
- **Revive:** the time left uses the respawn countdown, or 15 s. The
  original also leaves out a body under more than 0.75 m of water and
  0.5 m below its surface.
- **Flee:** the rewrite's camouflage has no charge; carried counts as
  charged, and only the plan's 4 s end the camouflage.
- **Idle:** the weapon's alternate-key tap in the bot's own area (the
  rewrite's weapons have no such state).
- **Commander:** which tool it strikes with. The UAV, the satellite scan
  and orders for squads the SAI left without one are not from AIDLL's
  commander.
- **Path follower:** a moving target re-planned when it moves 2 m; the
  segment searches' fallback to the whole mesh (see above).
- **Stuck drivers:** after the 20 s progress check the move fails and
  the behaviour plans again. 0x100c91c0 also hands the bot to the game
  (IAIGameEffector vt+0x24 → 0x0089e940 → GameLogic slot 0x58), whose
  slot is `0x008924f0`, returning 0: it does nothing in this build.
- **Assists:** basicTemp is read as the AI object's +0x14.
- **Avoid:** r is the vehicle's AI radius; the grenade warning's event
  is checked every tick.
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
  `bf2142-recomp/profiles.txt` under the user's config directory, and the
  soldiers of the account's local profiles (0x1007ab70).
- **Local profiles** (`LocalProfileManager` in `src/Main/SettingsObjects`;
  BF2142.exe's, vtable 0x00925968): one folder per profile under
  `Profiles/`, numbered `%.4d` from 1 with gaps filled (0x004904d0), each
  with its own `Profile.con`, `General.con`, `Audio.con`, `Video.con` and
  `Controls.con`, plus `Default/` (made at start-up, 0x0048fc30 /
  0x0048f2b0: Profile, Audio and Video only). A profile is valid with its
  four settings files (0x0048ccb0; the original also wants
  `ServerSettings.con`, which the rewrite, keeping the server pages'
  values in the menu, does not write). The menu's offline accounts are
  the profiles without an account: `createOfflineAccount` (0x1004d280)
  makes one with the defaults and makes it current and Global.con's
  default user; the `localProfiles` list (0x100396b0) shows those
  without an account or GameSpy nick, each row's `profileIndex` its place
  among all; `loginOfflineAccount(index)` (0x1004d450) selects one
  (0x004903d0, 0x0048f490: the objects reset, its files run, its
  controls loaded, numTimesLoggedIn counted, the pages and the game given
  its settings); `deleteLocalProfile(index)` (0x1004c720, 0x00490ca0)
  removes its folder (never while one is current; the shipped menu does
  not call it). A soldier's profile is the one whose sub account is its
  name (0x00491030), selected when it logs in (isTransactionComplete
  0x10049bf0, offline connectToSubAccount 0x100510c0), else made (master
  the account, sub the soldier); logging out releases it (0x0048f3a0).
  There is no rename: `LocalProfile.setName` changes the current one's
  name for its next save, and no menu page calls it. At start-up the
  rewrite loads Default and then Global.con's default user (the profile
  last used), so a run without the menu has its options.
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
  `buttonPress`...), through the game's sound manager. The menu music is
  the mod's `Mod.desc` `<music>` (`sound/music/2142MenuMusic.ogg`; ModInfo
  +0x8c), streamed by the music player: the game's menu music object
  (0x007fd030, the game's +0xf4; 0x007fd060) starts it when the menu
  comes up alone (0x004032b0) and fades it out over 5 s when a game takes
  over (0x00403370); `Sound.playMenuMusic` / `stopMenuMusic` (SwiffPlayer
  0x10059dd0 / 0x10059df0, SwiffHost slots 102 / 103: events 0x16 / 0x15)
  do the same (not in an off-screen shot).
- **Credits** (`src/UI/Player/Scroller`): `credits.con` fills SwiffHost
  (`addCredit` 0x0047a0c0, `setScrollSpeed` 0x0046c2a0 clamped to
  500..4000 twips a second, `setScrollerRect` 0x0046c3a0 left top right
  bottom, `setScrollFadeSpeed` 0x0046c170 kept for
  `Multiplay.getScrollFadeSpeed` only), run once when the menu starts
  (SwiffHost slot 5, 0x004697c0). The credits page sets
  `scrollerman._isScroller = true` and calls `Logic.enableScroller(true)`
  (0x1003be40 to 0x0046c2f0: on, from the first line); gameswf then draws
  the roll in that clip's place (0x100dfe77, 0x100c6ee0, RendDX9
  0x10117910), in stage coordinates. Each line is localized (names pass
  as they are), in `StandardTextBold_15` (size 2), `StandardTextBold_13`
  (1) or `DynamicText_11`, centred in a box 1199/1024 of the width
  (0x101774b0, 0x10183570), opaque white, cut to the rectangle (a
  scissor, no fade); 40 slots from the stage's bottom 20 px apart, moving
  up at the speed, the top one recycled 20 px below the last, looping
  (0x10177210, 0x10177290, 0x10177190). `cacheCredits` and
  `getRandomCreditsTexture` have no caller in the shipped menus.
  `gameModes.con`'s `SwiffHost.addGameMode` names the modes (ModInfo,
  0x00461740 / 0x00460ff0, "N/A" when unknown) behind the end-of-round
  `HEADER_<mode>` texts (0x1001beb0); `menuTeams.con`'s
  `MenuTeamManager.addTeam` / `addKit` and `addWeapon` are kept (their
  readers, `Profile.getEquipmentName/Description`, `getSprint` and the
  unlock lists, have no caller in the shipped menus); `addAward` keeps
  the record 0x00459250 makes. `Logic.getDateString` (0x1003d370,
  YYYYMMDD) and `getMaxRank` (0x1003b620, 12) answer as the DLL.
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
  which later runs read instead of the game's. `getMappedString` returns
  the localized `CONTROLLER_…` key the game builds (0x00498750 through
  SwiffPlayer 0x10018050: `CONTROLLER_IDKey_W`,
  `CONTROLLER_IDFMouse_IDButton_0`, `CONTROLLER_IDFGameController_IDAxis_1`;
  "Joy Axis 1" in English). Each map's own settings
  (`ControlMap.invertMouse`, `mouseSensitivity` 0..10, `keyboardSensitivity`
  0.1, `setYawFactor`, `setPitchFactor`; the map object's +0x4c..+0x6c,
  written by 0x00680f90 when not at their defaults) and `setActive` are
  kept and written back.
- **Joysticks and gamepads** (`src/Input/GameControllers`,
  `SdlGameControllers`): the control maps' `IDFGameController_0..7`
  (device flags 4 << n, 0x00683b30), each a DirectInput game controller in
  the original (EnumDevices class 4, `0x006ab430`; vtable 0x00990f60). Its
  axes are set to -1000..1000 (`0x006ab320`) and read each frame as raw ×
  0.001 × the axis' sensitivity (`0x006aaef0`: X, Y, Z, Rx, Ry, Rz and two
  sliders, the ones present); the maps read offset ± that, minus when
  inverted (`0x006aadc0`; sensitivity 1, offset 0, not inverted by default,
  slots 15-20). No dead zone is set (DIPROP_DEADZONE is never written).
  Buttons are bits, then four per POV hat after them (up, right, down,
  left; only the exact directions, `0x006aaef0`; the count `0x006ab270`).
  Axis mappings (`addAxisToAxisMapping input device axis inverted slot`)
  give the axis' value (`0x006802c0`), button triggers the button
  (`0x006803f0`), and `addButtonsToAxisMapping` (written by the profile
  writer) a button pair. The controls page's "press the input"
  (`0x006aadf0`) takes the first button newly pressed, else an axis pushed
  past 0.95 after resting under 0.2, with its direction: `mapInput` binds
  an axis as an axis mapping, inverted when pushed the other way from the
  side being bound. The rewrite opens devices through SDL3 as they are
  attached (also those present at start) and frees their slot when they
  go; SDL gamepads are read through the gamepad API and laid out as
  DirectInput shows an Xbox 360 pad (left stick `IDAxis_0/1`, the
  triggers together on `IDAxis_2`, right stick `IDAxis_3/4`, A B X Y,
  shoulders, back, start, stick clicks `IDButton_0..9`, the d-pad as the
  hat, `IDButton_10..13`), other joysticks in SDL's own order; a stick on
  `c_PIMouseLookX/Y` turns at up to 120° a second (the rewrite's rate; the
  original treats the value as the mouse's). Tested with SDL's virtual
  joysticks.
- **Axis values** (`ControlMaps::value`): an axis input (c_PIYaw ..
  c_PICameraY) takes its primary mapping, or the secondary while the
  primary rests (0x00680dc0: under 0.0001 and the secondary above); a
  trigger is down while either is. Keys or buttons on an axis ramp
  (0x00680990): while the first key is held (it wins over the second) by
  the frame's time over the map's +0x40, while the second by as much
  down, with neither back to 0 by the time over +0x44; both 0.001 s
  (0x006825b0; set by the map's slots 23 / 24, which nothing calls), so a
  key is at its end within a millisecond. `keyboardSensitivity` (+0x54)
  is only stored, written and given to the controls page
  (`ControlSettings` 0x004980b0 / 0x00498090). A POV hat on an axis pair
  (kind 4, 0x006807f0; made by the map's slot 12, 0x00682db0, which no
  console method or script reaches, and not written back) ramps the same
  way towards its direction: the X input by the angle's sine, the Y input
  by minus its cosine (up -1), past ±0.001.
- **Controllers in vehicles:** the viewer's player input carries the
  vehicle map's `c_PIPitch` and `c_PIRoll` from keys, joystick and
  gamepad axes and hats (`c_PIYaw` and `c_PIThrottle` already came from
  them); the driver's pitch and roll are those plus the mouse's share
  (`SoldierInput::pitchAxis / rollAxis`, `GameWorld::driverInput`), and a
  move sends them as `PIPitch` / `PIRoll`. The Falcon (`IDFFalcon`, device
  flag 0x400; the shipped Air and Helicopter maps' secondary pitch and
  roll) is a real device class, a Novint Falcon haptic controller
  (0x0067f830, vtable 0x0098e9d8, opened through `bf2142haptics.dll`'s
  `ConnectHapticAPI`, 0x0067f460, and NovintHFX.dll): not rewritten, its
  bindings read 0.
- **Settings and profile objects** (`src/Main/SettingsObjects`): the
  console objects `GeneralSettings` (41 methods and the writer's
  `setBotSkill`, `setLastAwardsCheckDate`, `setLCDDisplayModes`),
  `AudioSettings` (15 properties), `VideoSettings` (13), `GlobalSettings`
  (4), `LocalProfile` (16 and `save`) and `SettingsManager` (the typed GS*
  settings). Their files are written as the original's writers do, in
  their order: `Global.con` (0x00495d50), `Profile.con` (0x0048d940),
  `General.con` (0x0049aad0, defaults 0x0049bd20 / 0x0049bdf0: crosshair
  255 255 255 255, squad tags 0 255 0, minimap rotating at 170, help
  pop-ups and camera shake on, toggle filters 0x4c382), `Audio.con`
  (0x004961a0, the properties written as `setX`), `Video.con`
  (0x00497560), and `Usersettings.con`; under
  `<config dir>/bf2142-recomp/Profiles/` (the current local profile's
  folder, see Local profiles), run at start-up after the game's defaults. The options pages start from
  their values and write theirs back on apply (the pages' VoiceOverVolume
  is `helpVoiceVolume`, NumberOfVoices `forceNumberOfVoices`, BloomEnabled
  `setUseBloom`). Applied: the volumes (also without the menu), the bots,
  `setCrosshairColor` (the HUD's CrossHairColor* variables), help pop-ups,
  bloom and dynamic shadows (their own systems' methods still run).
  Stored and exposed only: the video qualities (the renderer's), camera
  shake, colour-blind mode, the message filters, the HUD and minimap
  transparencies, the favourites and history lists.
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
  objects), then through the renderer's build of the level: the build
  reports how far it got (`LevelSources::buildProgress`: the terrain, sky,
  water and roads, undergrowth, then every 256 objects), each report the
  last tenth of the bar and a frame of the screen, and the level's
  renderer takes the window once the build is done.

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
  during a round counts from the next one. The spawn screen's kit buttons
  show each kit's upgrades (Kit%iAbilityIcon%iShow / PathString, seven a
  kit, the HUD manager's variables of `0x0044f1a0`): the items of the
  kit's unlocked ItemContainers with a `weaponHud.specialAbilityIcon`
  (`unl_scope_zoom`, `unl_stabilizer`, ...), in the kit's order (their
  engine setter wasn't traced). The kit customise screen and
  the unlock screen are in the [HUD](#kits-the-customise-screen-and-field-unlocks).
- **Awards:** the menu's award definitions are `awards.con`
  (`menu/external/flashmenu/awards.con`, run at start-up, `0x00460000`'s
  loader; `awards.setData <id>[_<level>] "<type>,<criterion>,<stat
  keys>,<value>" ...`, `0x00420a85`, up to nine). The awards a soldier holds
  were the stats server's (getawardsinfo.aspx; Profile.hasAward and the
  other award natives read its list, SwiffPlayer `0x1004f690`,
  `0x1004f770`), and the server decided them (the ranked server's
  `bf2.stats.medal_data`, never shipped with the game); most criteria are
  per round or per weapon, kit and vehicle, which the local store doesn't
  keep. So no awards are earned offline. In detail:
  - `awards.con` holds 126 entries (82 awards: 22 badges in three levels
    100-121, 20 medals 200-219, 24 ribbons 300-323, 16 pins 400-416).
    Each criterion is `"<flags>,<critId>,<keys>,<target>"` (parsed by
    `0x004a4e50` into 0x24-byte records): flag 1 a career criterion (scope
    2, else 1, within a round), 4 a check mark instead of a bar, 8 whole
    numbers ("1" for a target 0), 2 never read; the text is
    `AWARD_CRITERIA_<critId>`; the keys (`;`-separated, a space for none)
    are the stats server's getplayerinfo.aspx keys whose values add up
    (`ktt-3`, `wkls-5`, `vtp-12`, `tt`, `cpt`...; `wlr` is wins over
    losses; a key that is an award id counts 1 when it is held); the
    target is a float (seconds for the times). `stats::parseAwards`
    reads them; the names are `AWARD_NAME_<id>[_<level>]`.
  - The pictures: `images/awards/<badges|medals|ribbons|pins>/<front|
    perspective|start|eor>/<badge|medal|ribbons|pin>_<view>_<id>[_<level>].png`;
    the BFHQ awards page shows `_bw` for an award not held
    (`Profile.hasThisAward`), a badge its level's picture (`_0` locked,
    `getAwardLevel`); the start page's newest three come from
    `getLatestAwards(n)` (`0x100506f0`: an Array of `{id, path}`, from the
    profile's `LocalProfile.setCurrentProfileLatestAwards`, `0x0048d940`),
    with `HEADER_noawardsavailable` when there are none.
  - The held awards (SwiffPlayer's 0x50-byte records through the game's
    vtable `+0x250`/`+0x260`: kind, name, description, id, level, dates)
    come only from getawardsinfo.aspx (`0x004590a0`, parsed `0x00459d80`:
    `award`, `level`, `when`, `first`).
  - The award progress list (`awardProgress`, built by `0x1005fc70`,
    refreshed by `0x1005fde0`, drawn by `0x1005f1e0`; `setArgs(<id>)`):
    `HEADER_inaround` and the round's criteria (rows 1-2: the text and the
    target), `HEADER_global` (row 3) and the career criteria from row 4
    (the text, `value/target` and `mcProgress.setValue(min(value/target,
    1))`, or `mcIconCheck` once reached, the value by `0x1005eea0`).
  - Offline the rewrite answers as the original would without a held
    award: every award locked, no latest awards, the award progress list
    from the definitions with the values the local record has (kills,
    deaths, the score, wins, losses, time played; 0 for the per kit,
    weapon, vehicle and map keys). The BFHQ pages themselves are online
    only in the movie (offline its BFHQ button shows "Not available
    offline"), so offline only the start page's empty latest awards and
    the end-of-round pages show.
- **The stats page:** the details tab's category list (`statsTypes`,
  `0x10080cb0`: overview, player, titan mode, teamwork, leadership,
  weapons, vehicles, maps) and the `stats` list (`0x1007f640`, labels from
  `0x1007dfd0`) with "<category> 21". The overview (career points, score,
  time played, wins/losses, best round) and player (score and kills per
  minute, K/D, kills, deaths, per round) rows the store has are listed;
  the other categories stay empty. The progress tab's chart: its eleven
  buttons call `Logic.setActiveDiagram(mode[, scale])` (`0x1003c0e0` →
  the game's vtable `+0x3c4`), and the game draws into the clip's
  `_isDiagram` (property `0x18`) from `dice.hfe.PlayerProgress`
  (`0x0045f880`: getplayerprogress.aspx with mode point, score, ttp,
  kills, spm, role, flag, waccu, wl, twsc or sup and scale game, day,
  week or month). Not possible offline: the series are the stats
  server's (their column names weren't found either) and the drawing code
  behind `+0x3c4` wasn't found; the native is a no-op.
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
