# Multithreading study: why a window runs one core flat out, and where threads would help

Status: a study, with no code changed. The measurements were taken on 2026-10-28 on an
Apple M4 Pro (8 performance and 4 efficiency cores) at HEAD `8948ed8`, using an
instrumented copy of the tree outside the repo. Line numbers are as of that date; other
work moves them.

## Summary

The headless `--simulate` run is cheap because it runs only the simulation. A window
frame also runs the renderer's per-frame object sync, the HUD callbacks and the draw.
At HEAD, two pieces of plain waste dominate that work, and neither needs a thread to fix:

1. **Case-insensitive template lookups every frame.** `ObjectTemplateManager::find`
   is a `std::map<std::string, ObjectTemplate*, CaseInsensitiveLess>`, and every
   comparison calls `tolower` on each character (`src/World/ObjectTemplateManager.cpp:8`,
   `:173`). Two callers run it thousands of times a frame:
   - `LevelRenderer::syncObjects` → `collectParts` walks every instance's template tree
     each frame and looks up each child by name (`src/Render/LevelRenderer.cpp:1472`,
     `:526`).
   - `KitMenu::update` rebuilds the kit HUD from template lookups on every frame
     (`src/Main/main.cpp:3325` → `src/Main/HudMenusKits.cpp:206`).

   In a `sample` profile these calls are **about 75% of the main thread's time** in a
   release build. A 20-line hashed cache in front of `find`, prototyped in the scratch
   copy, cut the frame from **12.2 to 4.5 ms** (16 players) and from **19.0 to 7.6 ms**
   (32 players).
2. **The README's run line uses the ASan/UBSan Debug build** (`README.md:118`,
   `build/native`, configured at `README.md:107`). That build is about 10 to 15 times
   slower: 177 ms of CPU per frame at one tick per frame. In real time it falls behind
   the 30 Hz clock, and `TickTimer` (`src/Game/TickTimer.cpp:7`) then runs up to 10
   ticks per frame. The measured result was **about 500 ms per frame (2 fps)**, which
   matches "extremely low FPS, one core at 100%".

Everything game-side runs on the main thread. The only other threads are the
OS/driver ones (Metal's command queue, CoreAudio/OpenAL mixing) plus three small
helpers: music streaming (`src/Sound/SoundEngine/Music.cpp:173`), VoIP
(`src/Voip/VoiceChat.cpp:34`) and the menu's blocking network jobs
(`src/Main/MenuBindings.cpp:182`).

Threads are still worth adding once the waste is gone, in this order:

1. A simulation thread behind a render snapshot. It decouples the frame rate from the
   1.5 to 4 ms tick, and from 25 to 40 ms ticks in a sanitizer build.
2. A small job system that runs the per-player work in `stepPlayers` in parallel. Pose
   evaluation and soldier collision are about half of a 32-player tick.
3. Asset loading off the main thread, to remove hitches from meshes, LODs and textures
   that are loaded lazily during a frame.

Parallel GPU command encoding, threading the Flash/HUD, and a sound mixing thread are
not worth doing: each measured at or below 0.3 ms, or is already threaded.

## How it was measured

`BF2142_FRAME_BENCH` (`src/Main/Viewer.cpp:325`) times only `LevelRenderer::draw`. It
leaves out `syncObjects`, the ticks and the input callbacks, and two of those are where
the time goes. So a copy of HEAD was taken with `git archive` into the session's
scratchpad, built as RelWithDebInfo at `-O3` and as Debug with ASan/UBSan, and
instrumented as follows:

- **`BF2142_LOOP_BENCH=N`** in `screenshot()` copies the body of `interactive()`'s loop
  (`Viewer.cpp:378`–`604`) and runs it N times off-screen: ticks
  (`options.onTicks`), `playerView`/`onCamera`/`onView`, `syncObjects`, `draw`, then
  submit and a wait on the fence. By default it runs one tick per frame.
  `BF2142_LOOP_REALTIME=1` uses a real `TickTimer(10, 30)` instead, as the window does.
- **Scoped timers** around the stages of `draw` and `syncObjects`, the parts of
  `step()`, the `sources.overlay` callback and the menu callback.
- **`sample <pid>`** on a 2,500-frame loop bench, and on a headless 32-player
  `--simulate` run.

Base command (no window; `--screenshot` renders off-screen):

```sh
BF2142_LOOP_BENCH=300 <scratch>/bf2142 --game "source/Battlefield 2142" --nosound \
  +level Cerbere_Landing +gameMode gpm_sp +mapSize 16 --bots 15 --play 2 --simulate 10 \
  --screenshot shot.png
```

Sound was not measured, because `--view` enables OpenAL and would play audio. See
"Sound" below.

## Measured frame breakdown

Times are per frame (ms) on the main thread. "submit+gpu" is the wait on the frame's
fence, which is the GPU's time, since the bench waits for each frame.

| stage | release, sp 16 / 15 bots | release, cq 32 / 31 bots | ASan Debug, sp 16 | release + find cache, sp 16 | release + find cache, cq 32 |
|---|---:|---:|---:|---:|---:|
| simulation tick (×1) | 1.6 | 3.8–4.2 | 24.4 | 1.5 | 4.0 |
| `syncObjects` | **5.4** | **9.7** | **106** | 0.9 | 1.2 |
| — of which `collectParts` | 5.2 | 9.3 | 89 | — | 0.8 |
| `draw` CPU | 3.3 | 3.3 | 43 | 1.0 | 1.0 |
| — of which HUD overlay callback | 2.7 | 2.7 | 35 | ~0.3 | 0.4 |
| — — of which `KitMenu::update` | 2.5 | 2.4 | 30 | ~0.3 | 0.3 |
| — particles (vertex build+upload) | 0.36 | 0.38 | 6.1 | | |
| — cull/LOD (`prepareInstances`) | 0.08 | 0.22 | 5.1 | | |
| — command encoding (all passes) | ~0.2 | ~0.3 | ~3 | | |
| submit + GPU | 1.8 | 1.6 | 3.2 | 1.1 | 1.4 |
| **frame** | **12.2** | **19.0** | **177** | **4.5** | **7.6** |

Other findings:

- **Real-time clock:**
  - Release: 11 ms/frame, with 0.33 ticks per frame at about 90 fps.
  - ASan: **498 ms/frame**, with 9.4 ticks per frame. The simulation falls behind,
    `TickTimer` drops the backlog, and the game runs slowly and stutters.
- **Resolution barely matters.** At 2560×1440 (a Retina window) the GPU wait rose only
  from 1.8 to 2.3 ms, so the GPU is not the bottleneck.
- **The Flash menu over a level is cheap:** `advance` 0.01 ms, `frame` 0.01 ms, menu
  geometry upload 0.06 ms. `hudManager.frame` takes 0.05 ms and the name tags 0.01 ms.
- **The headless tick, 32 players** (sample of `--simulate 240`, 1.7 ms/tick):
  - `GameWorld::stepPlayers` takes about 70%. Within it:
    - `stepAnimation` (animation update, `apply`, `worldMatrices`): about 30% of the tick.
    - `SoldierBody::step` (`querySphere`, `collideWithObjects` → `collisionEvent`):
      about 30%.
  - `BotManager::update` takes about 10%.
  - The rest is about 4% `tolower` again: template and setting lookups from the
    simulation.

## Root causes of the single-core, low-FPS behaviour

1. **Debug + sanitizers used to play.** The README's run line, the only one it gives,
   is `build/native`, built at `-O0` with ASan and UBSan. Every other cost is multiplied
   by 10 to 15, the tick no longer fits in 33 ms, and the frame enters the
   10-ticks-per-frame spiral (`TickTimer::advance`, `src/Game/TickTimer.cpp:14`–`22`).
2. **String-keyed case-insensitive `std::map` lookups in per-frame code.** Each
   `find` costs about 12 comparisons, and each comparison makes a
   `tolower` call through the dyld stub for every character. In the `sample` profile,
   `__tolower` and its stub are the top two leaf functions.
   - `collectParts` (`LevelRenderer.cpp:519`–`543`) runs for every instance
     with a `partsTemplate`, which in practice is **almost every instance, every frame**:
     about 1,450 per frame on Cerbere Landing, static scenery included. For each child
     it does:
     - a `templates.find`;
     - a `settings().find("geometryPart")`, a second case-insensitive map;
     - an `atoi`;
     - a `std::function` call to `partPose`, which on the `GameWorld` side scans
       `m_bodies` linearly (`src/Game/GameWorld.cpp:4776`).
   - `KitMenu::update` does around 40 `find`/`setting` calls and about 60 HUD string
     sets per frame for data that changes only when the team or kit changes.
3. **No "dirty" notion in `syncObjects`.** World transforms, the cockpit geometry
   check, parts and animation lookups are recomputed for every instance every frame,
   whether or not the object moved (`LevelRenderer.cpp:1400`–`1482`).
4. **Everything is serial on the main thread.** Events, input, 0–10 simulation ticks
   (bots, physics, animation, effects, Python), the HUD callbacks, render prep,
   encoding and present all run in sequence. The other 11 cores sit idle.
5. **The renderer reads the simulation live.** It does so through `LevelSources`
   callbacks (`skeletonPose`, `partPose`, `partTransforms`, `animationFor`,
   `geometryFor`, `firstPerson`, `overlay`; set up at `src/Main/main.cpp:3322`–`3480`)
   and through the `EffectManager`s directly. Nothing can run concurrently with the
   simulation until that boundary becomes a snapshot.

## Fix first: plain waste, no threads needed

Ranked by measured gain:

1. **Fast template lookup.** Gain: 12.2 → 4.5 ms per frame in release, 177 → 101 ms in
   ASan.
   - Resolve `ChildTemplate::name` to an `ObjectTemplate*` once
     (`src/World/ObjectTemplate.h:29`). The name is resolved lazily only because the
     child may be defined later, so it can be cached on the first successful lookup and
     invalidated in `create`/`destroy`.
   - Alternatively, or as well, put a hash map with a lower-cased key in front of
     `m_byName`.
   - Make `CaseInsensitiveLess` an inline ASCII comparison. That also speeds up
     `settings().find`.
2. **Precompute each instance's part layout** at `addTemplate` time
   (`LevelRenderer.cpp:445`): a flat list of (part index, static local matrix, child
   name or id, whether it can move). Per frame:
   - call `partPose` only for instances whose object is a vehicle or Titan;
   - skip static objects entirely;
   - give `GameWorld::partPose` an id → body map instead of scanning `m_bodies`.
   This takes the remaining `collectParts` cost (0.8 ms release, 37 ms ASan) to near
   zero.
3. **Dirty flags in `syncObjects`.** Recompute `instance.world` only when the
   object's transform changed. `ObjectManager` could hand out a "moved this tick" list.
   Cache the `animationFor` path instead of building a `std::string` for every skinned
   instance each frame.
4. **`KitMenu::update` only on change** (team, kit, unlocks). The same applies to
   `updateKitAbilities` (0.1 ms).
5. **Give developers an optimised build to play with.** Add a `build/release` or
   RelWithDebInfo line to the README's run section, keep ASan for tests, and say so.
   Optionally print a warning at start-up when running a sanitizer build with a window.
6. **Smaller per-frame allocations and uploads.** Each is under 0.5 ms in release but
   adds up under ASan:
   - `prepareInstances` allocates `lodSwitchDistances` for every instance every frame
     (`LevelRenderer.cpp:2195`);
   - `drawInstance` copies each soldier's `worldPose` into a fresh vector on every pass
     (`:2605`);
   - the HUD, menu, particle and decal vertex buffers are created and destroyed every
     frame, each with its own upload command buffer (`buildOverlay` `:1568`,
     `GpuContext::createBuffer` `src/Render/GpuContext.cpp:314`, `flushUploads` `:299`).
     A persistent, cycled transfer buffer plus vertex buffer per stream, uploaded in the
     frame's own command buffer, would remove four or five submits per frame.
7. **Make `BF2142_FRAME_BENCH` time the whole frame** (ticks, sync, overlay, draw),
   like the scratch `BF2142_LOOP_BENCH`, so these regressions show up.

With items 1 to 3, a release frame is about 3 ms of CPU plus a 1.5 to 4 ms tick at
30 Hz. At 60 fps that leaves the main thread less than 40% busy. Threading is
therefore mainly for headroom: 64-player servers, Titans and heavier effects,
sanitizer and debug builds, and higher refresh rates. It is not the fix for the
reported slowness.

## Threading recommendations, ranked

The table rates each option on four points. Expected gain is measured where possible.
Risk covers thread-safety and determinism. Effort is a rough estimate.

| # | change | expected gain | risk | effort | prerequisites |
|---|---|---|---|---|---|
| 1 | Simulation on its own thread, with the renderer and HUD drawing from a published snapshot | Removes the tick (1.5–4 ms release, 25–40 ms debug/ASan, up to 10× when behind) from the frame; frame rate independent of the tick; the spiral of death can't freeze input | medium-high: shared state is everywhere at the moment | large (2–4 weeks) | the snapshot boundary (phase 3), input via a queue, Python confined to the sim thread |
| 2 | Job system + parallel `stepPlayers` phases (pose evaluation, soldier physics queries) | About 50% of a 32-player tick spread across cores (1.7 ms → ~0.9 ms; ~12 ms → ~6 ms under ASan); grows with player count | medium: determinism, `CollisionWorld`'s mutable stamps | medium | job system; split `stepAnimation` into serial events + parallel pose; per-thread query scratch |
| 3 | Asynchronous asset loading/streaming (meshes, LODs, textures, sounds) | Removes hitches from lazy loads during a frame; faster level build (~1.5 s release) | low-medium | medium | a loader thread; main-thread (or worker command-buffer) upload; placeholder until ready |
| 4 | Parallel render prep (`syncObjects` instance loop, `prepareInstances` cull/LOD, particle vertex build) | After the fixes, ~1–1.5 ms → ~0.3 ms in release; larger in debug | low (pure functions over instances once the callbacks are snapshot reads) | small, after #1's snapshot | job system; no `mesh()` loads inside the loop (defer to #3) |
| 5 | Ogg stream refill and sound decoding on a sound thread | Unmeasured (sound off in benches); likely small | low | small | `BfSoundEngine` stream queue with a lock; OpenAL is already thread-safe |
| 6 | Network receive/send on an I/O thread | Small CPU; steadier timestamps and ping | low if packets are consumed at tick boundaries | small | SPSC packet queues; `remoteStep`/`hosted->tick` drain them |
| 7 | Parallel GPU command encoding | ≤0.3 ms measured: not worth it | — | — | — |
| 8 | Flash/AVM1 and HUD rasterising on a worker | ≤0.1 ms measured: not worth it | AVM1 interpreter and host bindings are single-threaded | — | — |
| 9 | Python on a worker | Not possible without a redesign: the GIL, and `GameWorld` raises script events synchronously (`main.cpp:2894`) | — | — | keep it on the sim thread |

### 1. Simulation thread with a render snapshot

What the window path does today in each frame (`Viewer.cpp:378`–`604`):
- polls events and builds `PlayerInput`, then `onPlayerInput` → `round.setPlayerInput`;
- runs `ticks.advance` and `onTicks(n)`, which calls `step()` n times (`main.cpp:2910`)
  and then `sounds.update`;
- calls `playerView`, then `onCamera` → `round.setCamera`;
- runs `syncObjects` and `draw`. Inside `draw`, the `overlay` callback reads `round`,
  `gameLogic`, `hudManager` and `session`.

Proposed split:

- **Sim thread.** It owns `GameWorld`, `BotManager`, `GameLogic`, the effect managers'
  simulation, `PlayerCamera::tick`, `scripts.update` (it holds the GIL) and the
  network's `remoteStep`/`hosted->tick`. It runs fixed 30 Hz ticks from its own
  `TickTimer`. At the end of each tick it publishes an immutable **`FrameSnapshot`**.
- **Main thread.** It handles SDL events, input sampling and the swapchain. SDL needs
  these on the window thread: `SDL_WaitAndAcquireGPUSwapchainTexture` and
  `SDL_PollEvent` must run on the thread that created the window, and that is the main
  thread on macOS. The main thread also runs the HUD/menu Flash players and render prep.
  It encodes from the newest snapshot, interpolating between the last two with
  `TickTimer::interpolation` (`src/Game/TickTimer.h`; it already exists and isn't used).
- **Input path.** The main thread pushes `PlayerInput` (stamped with the tick it is for)
  into an SPSC queue, and the sim thread consumes it at the start of a tick. That
  matches how the network layer already treats moves (`src/Main/Prediction.cpp`).
- **Snapshot contents.** Everything the `LevelSources` callbacks and the overlay read
  today:
  - for each object: id, template, position, rotation, `hidden`, `fade`;
  - vehicle part poses, and for each soldier `worldPose` and weapon part matrices;
  - the dropped-kit and cockpit geometry choice;
  - the first-person models (`PlayerCamera::models`);
  - particle/sprite/mesh-particle lists, or a double-buffered `EffectManager` view;
  - point lights and decals;
  - the HUD's model: the values `updateHud`, `updateKitAbilities` and the menus push
    into `hudManager`, the name-tag inputs and the kill/chat feeds;
  - sounds to start, and the listener;
  - end-of-round state.

  Publishing it as a triple buffer, or a `std::shared_ptr<const FrameSnapshot>` swapped
  atomically, gives lock-free reads.
- **Sounds.** `round.takeSounds()` and the others move to the snapshot, and the main
  thread (or a sound thread) plays them. OpenAL itself is thread-safe.
- **Why phase 3 first:** turning the callbacks into snapshot reads *on one thread*
  surfaces every hidden dependency before any concurrency is introduced. It is also
  testable: the snapshot path and the live path must draw the same screenshot.

Risks:
- The HUD menus (`KitMenu`, `SquadMenu`, `CommanderMenu`, `commanderClick`,
  `onClick` → `round.setSpawnChoice`) read and write `round` directly from the main
  thread. Those writes become commands in the input queue.
- `MenuBindings` and Python share the console and global script state
  (`src/Python/ScriptHost.cpp:89`, `g_host`). Every console command issued from the menu
  must be marshalled to the sim thread.
- `Memory.cpp`'s chunk pools allow allocation only on the main thread
  (`src/Memory/Memory.cpp:113`, `:143`). Native code doesn't route through `bf_malloc`
  (only the retail DLL export does), but the Win32 drop-in would need the pools made
  per-thread, or the "main thread" made the sim thread.

### 2. Job system and parallel per-player work

**Job system.** A small fixed pool of `std::jthread`s, sized hardware threads − 2
(main + sim), with:
- `parallelFor(begin, end, grain, fn)` using a **fixed, deterministic partition**;
- a `TaskGroup::wait()`;
- a work-stealing queue, or a simple counter-based queue.

About 200 lines, portable C++20. Alternatives: enkiTS (zlib licence, C++11, tiny) or
Taskflow (MIT, header-only). Avoid libdispatch/GCD and PPL, which are tied to one
platform.

**Candidates in the tick, from the profile:**

- **`stepAnimation`'s pose half** (`GameWorld.cpp:4092`–`4191`). These steps touch
  only the player:
  - `bodyAnimation->apply`;
  - `weaponAnimation->apply`;
  - `anim::worldMatrices`.

  Split `stepAnimation` into two parts:
  - a serial "update + events" part, which plays sounds, starts effects and runs
    ground ray casts;
  - a parallel "pose" pass over all living players, run after the serial loop in
    `stepPlayers` (`:4284`).

  Poses are consumed later in the same tick, by hits in `stepProjectiles`, which already
  runs after `stepPlayers` in `GameWorld::tick` (`:465`), so ordering holds. Ragdolls'
  `stepRagDoll` is also per-player.
- **Soldier collision queries** (`SoldierBody::solve` → `CollisionWorld::querySphere`).
  These queries are `const` but not thread-safe: they bump a `mutable m_stamp` and write
  `Triangle::stamp` (`src/Physics/CollisionWorld.cpp:188`–`206`, also `:94` and `:166`).
  Give each thread its own visited set (a per-query generation array indexed by
  triangle, or a small hash set) before running them in parallel.

  The order-dependent parts (`collideWithObjects` → `collisionEvent`, pushing between
  soldiers, damage) must either stay serial, or be collected per player and applied in
  player order afterwards.

  A two-phase step would work: parallel "integrate and query the static world" per
  player, then a serial "resolve contacts and events in id order". It changes the
  arithmetic order of nothing that is per-player, so results stay bit-identical.
- **Bots** (`BotManager::think`, about 10%). Perception and path queries per bot are
  parallelisable if they only read the world. Decisions must be applied serially in
  bot order. `Random` draws must stay serial, or each bot gets a seeded sub-stream.
- **Effects** (`EffectManager::update`, `ObjectEffects::update`). These are
  per-emitter and independent. Any random draws must come from per-emitter streams, so
  that results don't depend on scheduling.

**Determinism rules for any parallel phase:**
- no shared RNG;
- no shared mutable scratch;
- output written to per-item slots, then merged in index order;
- no floating-point reductions across threads (or reduce in a fixed tree order).

Test it by running the same `--simulate` headless twice, with `BF2142_JOBS=1` and
`BF2142_JOBS=N`, and comparing the printed scores and positions, which must be
identical.

### 3. Asynchronous asset loading

Today `LevelRenderer::mesh()` and `texture()` read, decode and upload synchronously
wherever they are first needed:
- LOD switches in `prepareInstances` (`LevelRenderer.cpp:2205`);
- mesh particles and first-person models before the pass (`draw`, `:2276` ff.);
- HUD textures in `buildOverlay`;
- `menuTexture`.

Every first sight of a weapon, vehicle LOD or HUD icon is a frame hitch.

- **Proposed:**
  - a loader thread (or jobs) reads the file and parses DDS and the mesh;
  - the main thread uploads the result at the start of the next frame;
  - until then the renderer keeps drawing the previous LOD, or skips the draw.
- **SDL GPU constraint:** command buffers may be used only on the thread that acquired
  them. A worker *can* acquire its own command buffer for a copy pass, submit it and
  hand over a fence, but `GpuContext`'s single `m_uploadPass` (`GpuContext.cpp:290`–`310`)
  would need to become per-thread.
- **Level build.** `LevelRenderer::build` takes about 1.5 s in release and several
  times that under ASan. Texture and mesh decoding are independent per file, so a
  parallel prefetch of the level's file list before `build` would cut it.

### 4. Parallel render prep

Once the snapshot exists, `syncObjects` and `prepareInstances` are pure loops over
`m_instances`: frustum test, LOD pick, world matrix. They can use `parallelFor` with
per-chunk draw lists concatenated in order. Do this only after the fixes above, where
the loops are 0.3 to 1.5 ms in release. The same applies to `buildParticleBuffer`,
which builds vertices per emitter.

### 5–9. Sound, network, encoding, Flash, Python

- **Sound.**
  - OpenAL (system framework or OpenAL Soft) already mixes on its own thread.
  - Music streams on a thread (`Music.cpp:173`).
  - `BfSoundEngine::update` (`src/Sound/SoundEngine/BfSoundEngine.cpp:519`) runs on the
    main thread once per frame that has ticks, including Ogg stream refills
    (`OpenALSample.cpp:113`).

  Measure it first with sound on, in a window: wrap `sounds.update` in a timer. Move
  stream refills to a sound thread only if it shows up.
- **Network.** `remoteStep()` (`main.cpp:2674`) and `hosted->tick()` poll sockets inside
  `step()`.
  - An I/O thread that receives into a queue, timestamps packets and sends queued
    output is low risk, as long as game state is only touched when the sim thread drains
    the queue at a tick boundary.
  - Prediction replays moves by tick, so it isn't affected.
- **GPU encoding.** Encoding all passes costs about 0.2 to 0.3 ms. SDL3 GPU would need
  one command buffer per thread, and on Apple's tile-based GPUs splitting the main pass
  across command buffers costs extra load/store passes. Not worth it.
- **Flash/HUD.**
  - The measured cost is tiny (≤0.1 ms).
  - The AVM1 interpreter and its host bindings (`src/UI/Avm1`, `src/UI/Player`) are
    single-threaded, and the menu's callbacks call into `MenuBindings`.
  - Keep them on the main thread.
- **Python.** It stays on the sim thread with the GIL, because `GameWorld` and
  `GameLogic` raise events into scripts synchronously.

## Proposed architecture

```
 main thread (SDL window thread)          sim thread (30 Hz fixed)              workers (N-2)
 ───────────────────────────────          ────────────────────────              ─────────────
 poll events, sample input ──► input SPSC queue ──► drain inputs/commands
 HUD/menu Flash advance                    net: drain packet queue ◄── net I/O thread
 read latest FrameSnapshot ◄─────┐         bots.update ──────────────────────► parallelFor(bots, perception)
 interpolate (TickTimer::interp)  │         GameWorld::tick
 render prep (parallelFor) ───────┼──────────────────────────────────────────► parallelFor(instances)
 encode, acquire swapchain,       │           stepPlayers: serial moves ──────► parallelFor(poses, static queries)
   submit, present                │           projectiles, effects ──────────► parallelFor(emitters)
 upload finished loads ◄──── loader thread(s): read + decode files
 play queued sounds               └──────── publish FrameSnapshot (triple buffer / atomic shared_ptr)
                                            scripts.update (GIL)
```

## Phased plan

0. **Measure.**
   - Make `BF2142_FRAME_BENCH` time the full frame, as `BF2142_LOOP_BENCH` does.
   - Add optional stage timers (an env var or `--profile`) that print the table above.
   - Add a sanitizer-build warning.
1. **Fix the waste** (the list above). Expected: release about 3 ms CPU plus the tick;
   ASan about 60 to 80 ms.
2. **Job system + deterministic parallel phases** in the tick: poses first (no shared
   writes), then the static-world collision queries with per-thread visited sets. Gate
   it on `BF2142_JOBS`, and add the determinism comparison test to `tests/`.
3. **Snapshot boundary, still single-threaded.** Replace the `LevelSources` callbacks
   and the overlay's direct reads with a `FrameSnapshot` built at the end of `step()`.
   Route HUD/menu writes to `round` through a command queue. Screenshots must stay
   pixel-identical.
4. **Move the sim to its own thread.** Add interpolation between snapshots. The main
   loop no longer calls `onTicks`.
5. **Asynchronous loading** (meshes, textures, then sounds), and a parallel prefetch in
   `LevelRenderer::build`.
6. **Parallel render prep; I/O thread for net; sound streaming thread**, each only if
   measurements still justify it.

## Risks and determinism notes

- **The simulation must stay deterministic.** This covers network prediction
  (`src/Main/Prediction.cpp`, the client replaying moves), the headless `--simulate`
  scores, and the tests. Parallel work must:
  - use a fixed partition, and write results to per-item slots merged in index order;
  - take no RNG draws, or draw from per-entity seeded streams;
  - do no cross-thread float reductions.

  Run the comparison at `JOBS=1` and `JOBS=N` in CI.
- **Hidden shared mutable state:**
  - `CollisionWorld`'s `mutable` stamps;
  - `GameWorld`'s `m_random` (`src/Game/GameWorld.h:916`), used throughout the step;
  - lazily filled caches (`LevelRenderer::mesh/texture/animation`, template settings);
  - the `GameWorld` event sink into Python;
  - `Console` globals;
  - `Memory.cpp`'s main-thread-only pools in the Win32 DLL build.

  Run ThreadSanitizer (`-fsanitize=thread`, its own build directory, since it can't be
  combined with ASan) on the headless and off-screen benches after each phase.
- **SDL3 rules:**
  - Window, events and swapchain stay on the main thread.
  - A command buffer is used and submitted only on the thread that acquired it
    (`SDL_gpu.h`: "you should only access a command buffer on the thread you acquired it
    from").
  - Buffers and textures may be created from any thread, but not used concurrently
    while being mapped.
- **Latency.** A sim thread plus interpolation adds up to one tick (33 ms) of display
  latency unless the local player's camera is taken from the latest input. The
  original engine has the same 30 Hz tick, so match its feel by applying the look
  (yaw/pitch) on the main thread every frame, as `remoteLook` already does for joined
  games.

## Cross-platform notes

- Use `std::jthread`, `std::atomic`, `std::mutex` and `std::condition_variable`
  (C++20, already in use), or enkiTS/Taskflow. Avoid GCD, Win32 thread pools and
  pthread-specific APIs.
- Hardware concurrency differs on Apple silicon (P and E cores) and on Windows (SMT).
  Size the pool from `std::thread::hardware_concurrency()` minus 2, and leave the main
  and sim threads at normal priority. On macOS, consider QoS user-interactive for the
  sim thread.
- The Win32 drop-in DLLs run inside the retail engine, which has its own threading.
  The job system belongs to the native executable only.

## How to measure progress

- **Off-screen full-frame bench.** This needs `BF2142_FRAME_BENCH` extended to the whole
  frame, or the scratch `BF2142_LOOP_BENCH` upstreamed:
  `BF2142_FRAME_BENCH=300 bf2142 --game … +level Cerbere_Landing +gameMode gpm_cq +mapSize 32 --bots 31 --play 2 --simulate 20 --screenshot s.png`.
  Track the per-stage table.

  | build | baseline (ms) | phase-1 target (ms) |
  |---|---:|---:|
  | release, sp16 | 12.2 | ≤4 |
  | release, cq32 | 19.0 | ≤7 |
  | ASan, sp16 | 177 | ≤80 |

- **Tick cost.** Run `--simulate 240` headless with 31 bots; it was 12.3 s CPU, about
  1.7 ms/tick. Compare at `BF2142_JOBS=1` and `=N`, with identical printed results.
- **Real-time behaviour.** The same bench with `BF2142_LOOP_REALTIME=1`. Report
  frames/s and ticks per frame; ticks per frame above 1 at 60 fps means the sim fell
  behind.
- **Profiles:** `sample <pid> 10` on a long bench run, or Instruments' Time Profiler.
  Verify that `__tolower` and `ObjectTemplateManager::find` leave the top of the list.
- **Thread safety:** a TSan build running the headless sim and the off-screen bench, and
  the existing test suite (`build/native/tests/bf2142_tests`).
