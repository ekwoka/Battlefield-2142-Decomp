# Review backlog: PR #3 (`engine-fixes`)

These are open issues found by reviewing PR #3. Round 1 reviewed `main..8de9cf4`;
round 2 reviewed `ec9c31b..cef07a9`. Review agents reported them and nobody has
fixed them yet. File and line references are as reported at `cef07a9`, so check
them against HEAD before working on an item.

## How to work an item

1. **Verify the claim first.** Every "retail does X" statement below comes from a
   review agent. Check it against the original before changing code:
   - the Ghidra MCP, when it's connected;
   - otherwise headless Ghidra:
     `/opt/homebrew/Cellar/ghidra/12.2/libexec/support/analyzeHeadless <dir> bf2142 -process <module> -readOnly -noanalysis -scriptPath <dir> -postScript <Script.java>`,
     run against a copy of `build/ghidra` if `bf2142.lock` is held;
   - the decomp dumps in `build/analysis/<module>/decomp/` (files named by 64 KiB
     block, with `// ==== <addr>` headers);
   - `llvm-objdump` on the shipped binaries in `source/Battlefield 2142/`, for
     branch targets and data tables.

   Never downgrade a check to "inferred" because a tool is down. At least one
   review claim was wrong (see [Rejected](#rejected-checked-and-not-a-bug)), so
   read the disassembly when the decompiler's structure is in doubt.
2. **Decide which kind of change it is:**
   - default behaviour that differs from retail: match retail;
   - a sensible improvement on retail: keep it, but gate it behind an `enhanced.*`
     flag (`src/Scene/EnhancedSettings.h`) and add a row to README's table;
   - a plain bug: fix it.
3. **Tests should pin retail's numbers.** A test that only re-checks the new
   implementation doesn't count. Several items below exist because a test pinned
   the branch's behaviour.
4. **Build and test** with `nice` and a limited `-j`:

   ```sh
   nice -n 10 ninja -C build/release -j6
   BF2142_GAME_DIR="$PWD/source/Battlefield 2142" nice build/release/tests/bf2142_tests [filter]
   ```

   Test in the Release build. Use the Debug build (`build/native`, with sanitizers)
   only to re-run a failing test when you need the sanitizers or a debugger.

   The Remaster mod isn't installed here, so its legs of the tests skip.

   Run only the tests your change touches, by filter, not the full suite. Run the
   full suite only at the very end, and only if your change could reach systems
   those tests don't cover. The main agent runs the full suite once several items
   are merged, and sends any failures back to an agent (`CLAUDE.md`).
5. **Close the item.** Change its status to `done (<commit>)`, `rejected (<reason>)`
   or `blocked (<what's missing>)`, and keep a short note on what retail does.
6. Run at most 3 agents at once in this repo, and don't let agents start helpers
   of their own.

Status values: `open` (not started), `verify` (needs a retail check before deciding
anything), `pending-commit` (fixed in the working tree but not yet committed).

## Pending commit

| ID | Status | Item |
|---|---|---|
| Q-1 | done (0a0b9ad) | `RemoteWorld` squad reports and radar maps never cleared (0b87ad7). The fix is in the working tree: `src/Net/RemoteWorld.cpp` (`dropExpired`, plus clears in `clear()` and `onLevel`) and the test `NetGame_RemoteWorldSquadReportsEndWithTheLevel` in `tests/NetGameTest.cpp`. The test fails without the fix and passes with it. All `NetGame_` and `NetHost` tests pass; the full suite hasn't been run with it yet. |

## Crashes and correctness bugs

### Q-2. Two new Titan tests crash without the Remaster mod (high)
- **Status:** done (wave 1). Both tests start with `REMASTER_ONLY()` and CHECK the Titan before using it.
- **Commits:** 7c52206, a563255
- **Where:** `tests/TitanTest.cpp` ~5160 (`Titan_CorpseLiesOnTheDeck`) and ~5277
  (`Titan_WreckedGunshipOnTheDeckCantBeBoarded`)
- **Problem:** both tests load `mods/Project_Remaster_v17_Final`, then do
  `const auto titan = *level.titan(team);` with no null check. When the mod is
  missing, `titan()` returns null and ASan stops the whole test process with a
  SEGV.
- **Fix:** `const auto* t = level.titan(team); if (!t) return;`, or gate the test
  on the mod directory existing, as the other Remaster tests do.
- **Done when:** both tests skip cleanly without the mod, and the full suite has
  only the known Walker failure.

### Q-3. `Player::queueFrame` has no limit within one script call (medium)
- **Status:** done (wave 2). In SwiffPlayer.dll, `0x100ca870` calls the sprite's
  slot 0xa8 (`0x100cf5b0`), which appends to the queue at +0x108 every time. Its
  resize (`0x100048a0`) has no limit. When `0x100cfa40` takes the queue, it goes
  to the first frame and pops the entries after it that equal it. So retail
  collapses repeated frames when it reads the queue, and it has no cap.
  `queueFrame` now drops a frame equal to the last one queued, which gives the
  same gotos. It also keeps at most 64 entries (`Player::kMaxQueuedFrames`,
  dropping the oldest). The cap isn't in retail; a comment marks it as a
  safeguard. Dropping repeats doesn't help with an alternating
  `gotoAndStop(i%2+1)`, but the cap does. Test:
  `SwfPlayer_QueuedFramesDropRepeatsAndAreCapped`.
- **Commit:** 86d196d
- **Where:** `src/UI/Player/Player.cpp`, `queueFrame` and `applyQueuedFrames`
- **Problem:** every gotoAndStop, gotoAndPlay, GotoFrame or GoToLabel adds to
  `clip.queuedFrames`. Only the interpreter's 20M-instruction budget limits it.
  A mod SWF looping `gotoAndStop(i%2+1)` queues millions of entries. Each backward
  goto replays the timeline from frame 0, so the HUD stalls for seconds.
- **Fix:** skip a frame equal to the last one queued, and cap the queue (for
  example at 64, dropping the oldest). First check what retail's
  `0x100ca870`/`0x100cfa40` keep when the same frame is queued twice; the review
  says it drops repeats.

### Q-4. The first-person rig blends from the wrong tick on multi-tick frames (low to medium)
- **Status:** done (wave 2). `FirstPersonRig` snapshots `m_previous` on the
  first `update()` after a `blend()` (the pose where the last frame left the
  skeleton), so a frame's alpha, which spans all its ticks (`0x0043f170`),
  blends from there to the last tick's pose. `blend()` clamps alpha to
  −0.1..1.1 as the objects' does. `setWeapon()` also resets `m_previous` and
  the drawn matrices to the setup pose, so a frame drawn before the next tick
  shows the new weapon's start pose. Tests:
  `FirstPerson_MultiTickFrameBlendsFromTheLastDraw` (three-tick frames, the
  extrapolated ends and the clamp) and `FirstPerson_WeaponChangeDrawsTheNewPose`.
  Both fail without the fix.
- **Commit:** 50e85c9
- **Where:** `src/Animation/FirstPerson.cpp:179` (`m_previous = m_pose` every tick)
  and `FirstPerson.h:229` (`blend`)
- **Problem:** alpha covers all the ticks a frame ran, but `m_previous` holds only
  the last tick. Below 30 fps the arms and weapon start one tick behind the view
  and world. Other issues:
  - `blend()` clamps alpha to 0..1, while everything else uses −0.1..1.1;
  - after `setWeapon()`, one frame can blend the old weapon's `m_previous` into
    the setup pose.
- **Fix:** snapshot `m_previous` once per frame, on the first tick after a draw;
  use the shared alpha range; and snap on `setWeapon`. Add a multi-tick frame test.

### Q-5. `BF2142_FRAME_STATS` problems (low)
- **Status:** done (wave 1). `FrameStats::intervalFrom`: unset, empty, 0 or negative is off; the `gpu: driver` line only with the stats on; `dropFrame` on a failed acquire; README row. Tests `FrameStats_IntervalFromTheVariable`, `FrameStats_DroppedFrameAddsNothing`.
- **Commits:** 8e3e8cf, 8a22e02
- **Problems:**
  - `BF2142_FRAME_STATS=0` turns the stats on, at 5 s (`src/Render/FrameStats.h:31`:
    `atof` gives 0, which falls through to the default).
  - An unconditional `gpu: driver …` printf now runs at startup
    (`src/Render/GpuContext.cpp:79`).
  - When the swapchain acquire fails, the `continue` at `src/Main/Viewer.cpp`
    ~1055 skips `endFrame`, so that frame's stage times are added to the next
    frame.
  - The variable isn't in README's environment-variable list (~398-406).
- **Fix:** treat 0 or empty as off; gate the printf behind the variable; end (or
  drop) the frame's stats on the failed-acquire path; document the variable.

### Q-13. `Titan_WalkerSweepsTheSuezBridges` fails, and is disabled (medium)
- **Status:** done (wave 3, 8aa76e6). The test was wrong, not the walker physics. No
  lane stopped at the spans' deck or the joint. The vanilla mech stopped
  against objects that retail's `StaticObjects.con` places on and beside the
  road, and that collide in retail too (`physicsType Mesh`,
  `hasCollisionPhysics 1`):
  - The open-loop throttle drive took a few degrees of heading from a bump at
    the start: `bc_yard_a` on the west road, `eu_ag_wreck` (−76.3, −24.3) on the
    east road. Over the 300 m run that put the walker metres off its lane, into
    the towers' legs (x −303 and −187, z ≈ −20 / −44) or a railing post
    (x −138.85). The scrape's friction then turned it into the obstacle.
  - `as_apc_wreck` (−316.95/52.127/−38.817, yaw −138.4) sits on the west span.
    Its faces reach y 53.0–53.7, above the bottom of the hull's vehicle-LOD box
    (1.9 m under the origin, 54.1 on the deck). That makes it a wall for the
    hull's Basic response. For the legs' sphere it's a wall above stepHeight
    (`us_heavy_mech_MechLegs`: length 2.8, stepHeight 1.4; `0x006cec60`).
    Lanes 3–6 m south of the axis meet it. Some stop there; others climb its
    sloped top or scrape past.

  The sweep now holds its lane as a driver would (PIYaw back to the lane's
  line, at most 20° off). Each lane must cross, or stop within 7 m of that
  wreck (with the default lanes, fwd +6 walk and back −3 sprint stop there).
  A lane ends after 5 s stopped. 8 s a run, in the Release build.
- **Where:** `tests/TitanTest.cpp` (`Titan_WalkerSweepsTheSuezBridges`, its first
  line `DISABLED(...)`)
- **Problem:** the vanilla `us_heavy_mech` doesn't cross the big road's two
  spans (`span(-245,-32)`) in several lanes, both ways: it stops on the deck
  ("stuck at" about y 54.1) and `crossed` fails. It failed before the
  2026-11-25 vehicle and Titan fixes too. A run takes 2.5 to 8.5 minutes, the
  longest in the suite, so it's switched off rather than failing every run.
- **Fix:** find what stops the walker at the spans (the joint at x -245 was
  fixed once: retail's walker response, `0x00854170`), then delete the
  `DISABLED` line. `WALKER_SWEEP=1` logs every lane.
- **Done when:** the test passes with the `DISABLED` line removed.

## Performance

### Q-6. A template lookup for every child, every frame, in `collectParts` (medium)
- **Status:** done (wave 1). `LevelRenderer::blendsBetweenTicks` caches the
  answer per `const ObjectTemplate*` (`m_blendsBetweenTicks`, like
  `m_animatedUVLooks`); the uncached test is `templateBlendsBetweenTicks`, which
  `markBlendedParts` still uses. No behaviour change.
- **Commits:** 3443d51, 81bc213
- **Where:** `src/Render/LevelRenderer.cpp` ~1121-1131
- **Problem:** `blendsBetweenTicks(*childTemplate)` runs for every child of every
  parts instance, every frame. Each call compares 8 type strings and does
  `settings().find("boneIndex")` on the case-insensitive map. `docs/multithreading.md`
  names this kind of lookup as the main-thread hotspot.
- **Fix:** cache the result per `const ObjectTemplate*`, as `m_animatedUVLooks`
  does, or store a flag per node next to `nodesTicked`.

### Q-7. Nametag loops (low)
- **Status:** done (wave 1). The decoy loop sits in `if (m_icons)`; the vehicle reveal reads a set of reported vehicles built once per frame. Same output.
- **Where:** `src/Main/Nametags.cpp`
- **Problems:**
  - The decoy loop, `for (const auto& p : m_icons ? players : std::vector<...>{})`,
    copies `players` every frame. Guard it with `if (m_icons)` instead.
  - The network-marker loop calls `isReported` on every other player for each
    player in a vehicle, which is O(P²) per frame. Precompute the set of reported
    vehicles.

### Q-8. One `titanParts` call still ignores the Titan's id (low)
- **Status:** done (wave 1). The cut-off check passes `titan->object` too.
- **Where:** `src/Game/BotsTitan.cpp:409`, the cut-off check from 98163d0
- **Problem:** this one call still passes `titanParts(titan->team)` without
  `titan->object`; every other call was updated in 709a7bf. It's for consistency
  only, since there's one Titan per team today.

## Missing tests

### Q-9. The tick-blend history and the soldier-eye camera aren't tested (low)
- **Status:** open
- **Where:** `LevelRenderer::syncObjects`, and `src/Main/Viewer.cpp` ~1005-1040
  (carrier yaw, `lookTurn`, eye step)
- **Missing cases:** a new id starts unblended, a removed id is erased, a jump of
  more than 4 m snaps, and the eye composition.
- **Suggestion:** move the object-blend map and the eye composition into free
  functions (`src/Render/TickBlend.h`, `src/Main/MissileViewSmoothing.h`) and test
  them there.

### Q-10. A shader test only searches the source text (low)
- **Status:** done (wave 1). The fog and particle pixel math now live in
  `src/Render/Shaders/fog_share.glsl` and `particle_color.glsl`, written in the
  subset of GLSL that C++ also compiles. The shaders include them, and so does
  `src/Render/ShaderMath.h`, which mirrors the few GLSL types and built-ins they
  use. The `ShaderMath_` tests in `tests/ShaderUniformsTest.cpp` evaluate that
  code against numbers worked out from RaCommon.fx `calcFog`, Particles.fx and
  D3D9's fixed-function fog: the fog's share, the decision of which sprites fog
  (A-8), the additive mask before the clamp, and ps_1_4's saturated output
  before the fog. That last one was a fix: normal particles brighter than 1
  were fogged before the clamp (red 1.2 at a 0.625 fog share gave 0.575;
  retail gives 0.5).
- **Commit:** 01bdaeb
- **Where:** `tests/ShaderUniformsTest.cpp:178`, which greps for
  `clamp(outColor.rgb * outColor.a`
- **Fix:** pin retail's numbers, for example by evaluating the blend for known
  inputs, rather than checking for a source string.

## Tooling and scripts

### Q-11. The Windows build and run scripts (low)
- **Status:** open
- **Commits:** a13fd00, cef07a9
- **Problems:**
  - `build-windows.cmd` hard-codes:
    - VS2019 BuildTools' `ninja.exe`;
    - `C:/Python313/python.exe`;
    - exact sibling folder versions (`llvm-mingw-20261117`, `cmake-4.4.4`,
      `SDL3-3.4.16`, `openal-soft-1.25.2`);
    - drive `Q:` for `subst`.
  - `run-windows.cmd` overwrites the user's `BF2142_GAME_DIR` with
    `C:\Program Files (x86)\Electronic Arts\Battlefield 2142`.
  - README.md:154 says the tools live in an adjacent `work/` folder; the script
    uses `%ROOT%..`.
  - Both files have LF line endings and the repo has no `.gitattributes`. cmd's
    `goto` label search is unreliable with LF endings (`goto failed` comes after a
    469-character line).
  - cef07a9's `-O3 -DNDEBUG` does nothing, since that's already CMake's Release
    default for Clang.
- **Fix:**
  - honour an existing `BF2142_GAME_DIR`;
  - take ninja and Python from `PATH`, or from `%NINJA%` / `%PYTHON%` overrides;
  - fix the README path;
  - add `*.cmd text eol=crlf` to `.gitattributes`;
  - drop or explain the redundant flags.

### Q-12. Small test-tool and key-script nits (low)
- **Status:** done (wave 1). `rclick` is its own `if`; `--remote-actions` holds the lie key while the asked pose and our ghost's pose (the server's, mask 0x20) differ, so a refused press stays one press.
- **Problems:**
  - `src/Main/Viewer.cpp`, `runKeyScript`: the `else if (step.op == "rclick")`
    hangs off the new `release` branch. Make it a separate `if`.
  - `--remote-actions` (`src/Main/main.cpp` ~7588) latches `actProne` from the
    pose it asked for, not the pose it got. If the server refuses to go prone, the
    next scripted change sends a second lie press that inverts the stance. This
    only affects the test tool.

## Enhancements

### Q-14. The widescreen HUD is fixed to 16:9, wrong on ultrawide (UWQHD) screens (medium)
- **Status:** done (wave 3). With `enhanced.hudWidescreen` (and no hudfix), `hud::widescreenLayout`
  (`src/Hud/Widescreen.h`) works out the layout from the window's pixel size each frame
  (`ViewerOptions::windowSize`), so a resize updates it. The squeeze is (4/3) ÷ aspect. The sides
  move 360 (1/squeeze − 1), which is 120 at 16:9. The frames keep their authored on-screen 0..1024,
  plus one unit at each edge. At 16:9 this gives Remaster's 12/16, ±120 and -134/1367 exactly. 21:9
  (3440×1440) gives 0.558, 285 and -317/1836, and 32:9 gives 0.375, 600 and -667/2732. A window 4:3
  or narrower keeps the authored HUD. Tests: `Hud_WidescreenLayoutForTheWindowsAspect`,
  `Hud_WidescreenLayoutFollowsAResize`. Off-screen screenshots at 3440×1440 and 5120×1440 show a
  round minimap, the sides at the edges and the gunner frame covering the full width. When the
  hudfix's shader is installed, its fixed 16:9 layout stays, even with the flag on: its
  HudRotation.con and the re-cut commander screen aren't available here to extend. Flash menus,
  from reading the code: `ui.vert` stretches the stage to the window (`size.z` 0 for menus), so at
  21:9 they stretch about 1.79× wide. That's the same as before, and the pointer mapping matches.
- **Where:** `src/Main/main.cpp` ~3886–3925 (`hudXScale`, the `enhanced.hudWidescreen` block)
- **What works:** at 3440×1440 (21:9), with `+widescreen 1` (`src/Main/DisplayModes.h`),
  the mode is offered. The 3D view widens correctly: `aspectFov`, RendDX9's
  `0x10066ac0`, keeps the vertical FOV from 4:3 up to very wide screens.
- **Problems:**
  - Remaster's HUD fix (applied natively) and `enhanced.hudWidescreen` both squeeze
    the 4:3 HUD by a fixed 12/16, which is only right at 16:9. The right squeeze is
    (4/3) ÷ aspect, about 0.56 at 21:9. At 21:9 the HUD stays about 1.34× too wide,
    so round gauges and the minimap come out oval.
  - The side pieces move a fixed 120 units (`shiftNode` ±120), so on wider screens
    they don't reach the edges.
  - The full-screen gunner frames and filters are spanned to -134..1367
    (`spanNode`), which covers 16:9 only. On 21:9 they likely leave bare strips at
    the sides. This hasn't been looked at on screen.
  - The Flash menus haven't been checked at 21:9.
- **Fix:** under `enhanced.hudWidescreen`, derive the squeeze, the side offset and
  the frame span from the window's real aspect ratio, and update them when the
  window is resized. With Remaster's patch installed, the fixed 12/16 is retail
  behaviour with the mod, so only replace it behind the enhanced flag. Check
  32:9 as well.
- **Done when:** at 3440×1440 and 5120×1440 the HUD keeps its 4:3 proportions,
  the side pieces sit at the screen edges, and the gunner frames cover the
  whole screen. Add a test of the computed values for 4:3, 16:9, 21:9 and 32:9.

## Retail parity: default behaviour that differs from retail

Each of these needs the retail claim confirmed first (step 1), then matching retail
or gating behind a flag.

### A-1. Bot squad split puts the new bot in the wrong squad (round 2, high)
- **Status:** done (wave 1). Confirmed in the disassembly: after the move
  (0x100890fb..0x1008915c), `mov esi, [ecx+4*eax]` at 0x1008914c makes `esi`
  the next squad, and nothing writes `esi` again before `mov ecx, esi` at
  0x100891e1/0x100891fb, so the append (0x100891e3) and the join (0x100891ff)
  both use the next squad. `joinAiSquad` now joins `chosen + 1` after a move.
  The tests pin {0,1,5}, {2,3}, {4,6,7}, and in the human cases the new bot
  follows the member who moved.
- **Commits:** 9d3a0a0, 068490b
- **Where:** `src/Game/GameWorld.cpp` `joinAiSquad` (~5972)
- **Retail (AIDLL 0x10089000):** when the chosen squad has 3 or more members and
  the next squad is empty, it moves its last-joined member there. `esi` is
  reassigned to the next squad at 0x1008914c, so the append (0x100891e3) and the
  join (0x100891ff) put **the new bot in the next squad too**.
- **Branch:** the new bot stays in the original squad. With 4 bots a side that
  gives 3+1 instead of 2+2.
- **Tests:** `Bots_AddBotSpreadsSquadsAsRetail` pins the branch's version (squad 1
  = {0,1,3}). Retail gives {0,1,5}, {2,3}, {4,6,7}.
  `Bots_AddBotMovesTheLastJoinedAsRetail` has the same issue.
- **Fix:** after a move, join the new bot to `chosen + 1`, and fix both tests.

### A-2. Turret EMP only scales seat 0 (round 2, high)
- **Status:** done (wave 2). Confirmed: 0x00565180 walks the bundle and its
  parents (+0x34) to the first with an armour (+0x48), stops there, and scales
  inputs 0..5 by 1 - level / numSteps (armour slots 0x14c, template interface
  0xc4aa slots 0x1bc / 0x1d4, as the PCO filter 0x005ca2e0). No shipped seat
  PCO has an Armor component (the Titans' armoured gun hubs take no EMP), so
  every gunner takes the vehicle's EMP. `SeatLayout::yawArmour` / `pitchArmour`
  hold each look bundle's nearest armoured ancestor; the test now pins eu_tank's
  cupola at x 1/2 and x 0 (12.0, 6.0, 0.0 degrees).
- **Commit:** 8beeeb4
- **Where:** `src/Game/GameWorld.cpp:14312` `stepGunner`
- **Retail (0x00565180):** walks up the bundle's parents (+0x34) to the first one
  with an armour (+0x48), and scales by that armour's EMP. eu_tank's
  `EU_TANK_CupolaBase` PCO has no Armor component (`EU_TANK/eu_tank.tweak`), so
  the cupola gets the tank root's EMP.
- **Branch:** scales only when `player.seat == 0`. The test
  `Vehicles_TankTurretYawFollowsEmpStep` pins the cupola as unscaled.
- **Fix:** take the EMP from the nearest armoured ancestor of the seat's bundle,
  and fix the test. 6830e6f's `pitchInputLost` path must keep working.

### A-3. Titan wreck drain: ×3 dropped for Titans only (round 2, medium)
- **Status:** done (wave 3). Slot 0x20 resolved. AIDLL's interface (vtable
  `0x10107210`, constructor `0x10001030`: `+4` 0, `+5` 0, `+6` 1, `+7` 1) answers
  slot 0x18 with `+4` (`0x1005cf30`), set only by its init (slot 4, `0x10001740`,
  at `0x100017ba`), and slot 0x20 with `+6` (`0x1008a9b0`), which that init sets
  from its first argument. The init's caller is the server's level load
  (`0x004aefe0`, the call at `0x004af290`, through the copy of `DAT_00b47ce4` in a
  local, which is why the earlier scan missed it): first argument
  `!(GSGameMode is "sp1", "gpm_sp" or "gpm_coop")`, second `(it is "sp1" or
  "gpm_sp")`. So the ×3 is the single-player and co-op modes' alone, for vehicles
  and Titans alike; in gpm_ti (and conquest) a wreck drains at 1× with bots,
  which is what the retail observation behind 5bbdb2d saw.
  `GameWorld::wreckDrainsFast` (AI running and one of those modes) now drives
  both `destroyVehicle` and `stepTitanWreck`. `GameWorld_WreckDrainsFasterWithBots`
  checks six mode/AI legs; `Titan_SilosWearTheShieldThenTheHull` checks gpm_ti is 1×.
- **Commit:** 5bbdb2d
- **Where:** `src/Game/Titans.cpp:1978` `stepTitanWreck`; compare
  `src/Game/GameWorld.cpp:1607` for vehicles
- **Retail (0x00652380):** the ×3 depends on AIDLL slot 0x18 being true, slot 0x20
  being false, and object flag `+0x14 & 8`. That flag is set for mobile physics
  (0x005f6ba0 → slot 0x54(8,0) when template `+0xf1 & 2`), and eu_titan has
  `hasMobilePhysics 1`. So retail does not exempt Titans.
- **Open question:** what AIDLL slot 0x20 returns while bots run
  (`docs/coverage-11.md` §3f). If it's true, the ×3 is off for vehicle wrecks too.
- **Fix:** treat Titans and vehicles the same. Either resolve slot 0x20, or drop the
  ×3 everywhere and cite the observation.

### A-4. Parachute starts at rest without a retail citation (round 2, medium)
- **Status:** verify
- **Commit:** 8ce120b
- **Where:** `src/Game/Parachutes.cpp:356`
- **Retail:** the base FireComp launch (0x005f31d0) adds the root node's velocity
  (+0x50, slot 100), capped at `*DAT_00a71d1c`. The review found no override in
  the parachute fire component (0x005e4330, 0x005e4460, 0x005e45e0, 0x005e4750).
- **Fix:** find the routine that zeroes the velocity, or restore the inherited
  velocity. If the player report is trusted, record it as an unverified
  observation.

### A-5. Friendly fire uses a hand-picked type list (round 2, low)
- **Status:** done (wave 2). Confirmed: 0x005b7290 takes the victim's root
  (0x006f1d70) and walks up to the first object answering interface 0xc4c5,
  whose QueryInterface answers it in PlayerControlObject (0x005cc940, and
  Parachute, built on the PCO constructor), Soldier (0x00585af0),
  LadderContainer (0x0059d2d0) and FreeCamera (0x00616600). None found: no
  scaling. Soldier percentages for a root of class 0x9493 / 0x9c46. The damage
  now scales only for those classes (not the `steerClosest` lambda, which keys
  on occupancy); `GameWorld_FriendlyFireSparesNoHub` pins GenericProjectile,
  SupplyObject, SimpleObject and Bundle at full damage and a PCO at 0%.
- **Commit:** fa99915
- **Where:** `src/Game/GameWorld.cpp:1147` damage
- **Retail (0x005b7290):** scales friendly fire only when it finds an interface
  0xc4c5 object up the victim's tree, and compares that object's team (slot 0x9c).
- **Branch:** only GenericProjectile and SupplyObject count as having no control
  object, so teamed SimpleObject, Bundle and DynamicBundle destructibles are still
  scaled.
- **Fix:** use the same "root is a PlayerControlObject or Soldier" test as the
  `controlTeam` lambda in `steerClosest` (5b3110e).

### A-6. Squad order points ignore zero and negative values (round 2, low)
- **Status:** done (wave 1). Confirmed: 0x004c4c90 (squad vtable 0x929cd0
  +0x3c) does `fld [ebp+8]; fadd [esi+0x160]; fstp [esi+0x160]` with no
  check on the sign, then `fcomp [esi+0x168]` / `test ah,1` (0x004c4d64)
  skips the award only when points < max. Otherwise it fires the event and
  calls +0x40 = 0x004c4dd0, which sets the points to 0.0. A max of 0 is
  not special. The squad constructor 0x004c87a0 sets max to 100.0, so the
  port's record now starts at 100, not 0. Only NaN is still refused. The
  test is `ScriptHost_SquadOrderPointsAsRetail`. Not changed (out of
  scope): the whole routine is gated on byte +0x184, which is set only on
  a GameSpyRanked server in a standard game mode (0x00430a00), and the
  port doesn't model that gate. The set-max routine 0x004c4ea0 stores
  `(double)(int)` with no clamp, but the port still clamps the max at 0.
- **Commit:** b3344c1
- **Where:** `src/Game/GameWorld.cpp` `addSquadOrderPoints`
- **Retail (0x004c4c90):** always adds the points, then fires the event and resets
  once points ≥ max, including when max is 0.
- **Branch:** ignores points ≤ 0 and does nothing while max is 0.

### A-7. `setCollisionMesh` fallback is narrower than retail (round 2, low)
- **Status:** done (wave 2). Confirmed: the setCollisionMesh executor
  (0x0051c57b) and the collisionMesh one (0x00530de1) both pass the name to
  the active template's slot 0x8c, whatever its class. Shipped consequence:
  the control points' `flagpole` (a Bundle with children) and depth-0 Bundles
  and SimpleObjects with children placed on the install's community maps
  (house_medium_05jb, chimney_02jbv3, bp1_floodlight_big_cb ...) had no
  collision. The static path
  now reads either (`collisionMeshName`, the later .tweak one first, as
  Titans.cpp). Widening made the held weapon objects (GenericFireArm roots,
  almost all named by setCollisionMesh) solid at the soldier's feet and broke
  the parachute tests, so `addStaticObject` skips GenericFireArm roots: they
  stand in for a weapon in the soldier's tree, and no shipped level places
  one. `GameWorld_SetCollisionMeshServesEveryTemplate` pins the solid pole.
- **Commit:** 090cca2
- **Where:** `src/Game/GameWorld.cpp` `addStaticCollision`
- **Retail:** the setter at 0x0051c57b serves every template.
- **Branch:** only depth-0 SimpleObjects with no children, a limit the commit marks
  as a shortcut. Widen it when it matters.

### A-8. Normal-blend particle sprites aren't fogged (round 2, high)
- **Status:** done (wave 1). Confirmed in `RendDX9_ori.dll`'s disassembly:
  - 0x1012fe20..0x1012fe38 loads `movzwl 0x90(%ecx)`, `setne %cl` and calls
    0x1012f750.
  - With CL clear and the shader quality (+0x8cb0) not 0, 0x1012f750 sets
    render states 0x13/0x14 to 5/6 and 0x1c (FOGENABLE) to 1. Otherwise it sets
    2/2 and fog 0. It sets them through the state manager `DAT_101f8e6c`, whose
    slot 0x1c is SetRenderState.
  - The ParticleLow/Medium/High passes don't set FogEnable.
  - Additive templates (blend mode 1 or 2, 0x10138920) draw through
    0x10130bd0 and 0x10130540 instead: ONE/ONE, fog 0, and AdditiveLow/High,
    which set `FogEnable = FALSE`.

  Fix: sprite groups get `fogged = spriteGroupFogged(additive, quality)`
  (`src/Render/ParticleFog.h`). Normal-blend sprites no longer merge into a
  placed (repeat) group. The comments are fixed.

  Sub-question, vertex `oFog` or table fog: `oFog`. RendDX9 never sets
  FOGTABLEMODE (0x23) or FOGVERTEXMODE (0x8c): no `push $0x23` in the binary,
  and no FogTableMode in the shaders. With a vertex shader and no table mode,
  D3D9 uses `oFog`, which is vsParticle's `calcFog(HPos.w)`. A screen-aligned
  sprite's corners share their w, so particle.frag's per-pixel `fogAmount`
  equals the per-vertex value.
- **Commits:** 55214dc, then 01bdaeb and c5dd144
- **Where:** `src/Render/LevelRenderer.cpp:3015` (sprite groups pushed with
  `fogged=false`); the claim is in `LevelRenderer.h:1011` and `particle.frag:5`
- **Retail:**
  - The sprite draw `0x1012fdf0` calls `0x1012f750` at 0x1012fe38 with
    CL = (particle +0x90 != 0).
  - With CL 0 and quality != 0, it sets SRCBLEND 5, DESTBLEND 6 and
    **D3DRS_FOGENABLE (0x1c) = 1**. With CL set, it sets ONE/ONE and fog 0.
  - Particles.fx's ParticleLow/Medium/High passes don't set FogEnable, and
    `vsParticle` writes `Out.Fog`.
- **Branch:** no sprite is fogged, so far smoke never fades into fog.
- **Fix:** `fogged = !additive` for sprite groups, as the placed-triangle path at
  ~3043 does, and correct the comments. Open sub-question: whether the fog factor
  comes from the vertex shader's `oFog` or from table fog.

### A-9. Particle level-of-detail distance uses the wrong point (round 2, medium)
- **Status:** done (wave 2). Checked against RendDX9_ori.dll's disassembly:
  - `0x10135be0` returns true when min ≤ camera ≤ max on all three axes (both
    faces count as inside), and `0x101393e0` then returns 1.
  - Otherwise the distance is measured from the box centre (+0x114). The size
    term is the half-diagonal (+0x120) × k[i] / distance, where i = 1 only when
    the box's y extent is strictly greater than both its x and z extents.
  - The caller `0x10098bf0` sets k[1] = 1.05 / (fov·tan(fov/2)) and k[0] =
    k[1] × height/width. Here fov is RenderView +0x24 (slot 0x24) and
    height/width is RenderView +0x34 (slot 0x48); the view comes from renderer
    slot 0x5c, which returns +0xf8.
  - The box is the particles' box grown by particleMaxSize (`0x101407c0`
    tail), the same box `Emitter::box`/`bounds` builds.
  - The emission raises the result to minEmitScale (`0x101403b0`).

  `EffectManager::setCamera` now takes the fov and the aspect;
  `LevelRenderer` passes the target's height / width. Test:
  `Effects_DistanceLodAsRendDX9` pins a MassiveSmoke-like 4×104×4 column at
  0.333539, the same box lying along x at 0.277932 (aspect 0.75) and 0.236227
  (aspect 0.5625), and the inside / on-face / just-outside cases.
- **Commit:** 62ac2aa
- **Where:** `src/Effects/Effects.cpp:781`
- **Retail (0x101393e0):**
  - returns 1.0 when the camera is inside the system's box (`0x10135be0` on
    +0xfc);
  - measures distance from the box centre (+0x114..+0x11c);
  - the size term is radius (+0x120) × a factor chosen by the box's longest axis,
    over distance.
- **Branch:** measures from the emitter position (`m_position`) and has no
  inside-box case.
- **Fix:** use the `bounds()` centre and add the inside-box case. Wide columns such
  as MassiveSmoke are where this shows.

### A-10. First-person skeleton blended between ticks with no flag (round 2, medium)
- **Status:** done (wave 2), no flag. Retail steps the soldier's animation
  systems once a frame with the frame's time, not in the tick: the frame
  (`0x0040bd90`) calls ObjectManager slot `0x2c` (`0x007002d0`) with its
  seconds, which runs slot `0xa4` of the objects with update flag `0x2000`
  (`0x006f68f0` → manager slot `0xf8`). The Soldier's constructor
  (`0x0058df00`) sets `0x3800`, and its slot `0xa4` is `0x00586710` →
  `0x005ef250(frame dt)` → the active system's update (`0x008baed0`) and the
  weapon's. `0x00586200` (1/30 s) is a side path for the scene rays, so the
  engine.md claim was wrong. The slot `0xa0` caller (`0x00585bf0` →
  `0x005f06b0`, where the pose is evaluated) is still unfound: no direct
  `call`/`jmp [reg+0xa0]` site in the exe's disassembly is a Soldier's. A
  per-tick pose is therefore less like retail than the blend, so the blend
  stays the default and isn't gated. Moving the port's first-person step
  into the frame (and its third-person bones, which stay per tick) is a
  separate change. The comments in `FirstPerson.h` and `docs/engine.md` now
  say this.
- **Commit:** 50e85c9
- **Where:** `src/Animation/FirstPerson.h:229`, called at `src/Main/main.cpp`
  ~10643 and ~11036
- **Problem:** the code's own comment says the retail mechanism is unknown, but
  docs/engine.md:2987 (from 3443d51) says bone poses stay per tick, as in retail
  (0x00586200 → 0x005ef250 with 1/30 s).
- **Lead for tracing:** Soldier vtable 0x966150 slot 40 (+0xa0) = `0x00585bf0` →
  `0x005f06b0(this, dt, 1)`. No direct caller has been found yet.
- **Fix:** until a per-frame retail caller is traced, gate the blend behind a new
  flag (for example `enhanced.firstPersonBlend`). Make the code and docs agree.

### A-11. Squad order points ignore retail's ranked-server gate (wave 1, low)
- **Status:** open
- **Where:** `src/Game/GameWorld.cpp` `addSquadOrderPoints` and `setSquadOrderMax`;
  `ScriptBridge`; the HUD's squad-order bar
- **Retail:** the add (0x004c4c90) and set-max (0x004c4ea0) run only when squad
  byte +0x184 is set, which happens only on a GameSpyRanked server in a standard
  game mode (0x00430a00). Offline retail never shows the bar (`docs/coverage-5.md`).
  The set-max also stores `(double)(int)` with no clamp, and the HUD shows the bar
  only after a max message (event 0x56, value > -1).
- **Port:** no ranked gate; the max is clamped at 0; the HUD shows the bar when
  max > 0. Since wave 1 the default max is retail's 100, so points added before any
  set-max would show the bar (the shipped `scoringCommon.py` always sets it first).
- **Fix:** model the gate (off for offline and unranked games), drop the clamp, and
  show the bar on retail's condition. If the bar is wanted offline, put it behind an
  `enhanced.*` flag.

### A-12. Additive trails and placed particles aren't fogged (wave 1, medium)
- **Status:** done (wave 3). The claim is confirmed, and retail goes further. RendDX9's trail
  renderer (0x10133c00: technique 0x10133d90, batches 0x10133000) and its NSA renderer
  (0x1012e920: 0x1012ebf0, 0x1012d720) pick the technique only by particleSystemShaderQuality,
  plus particleShaderType 1 for heat shimmer (system +0x174 from the template's getter). Between
  BeginPass and EndPass they set only textures and constants, no render states. Trail.fx and
  NonScreenAlignedParticles.fx have no additive technique: Low, Medium and High are SRCALPHA /
  INVSRCALPHA with FogEnable TRUE, and their pixel shaders don't mask by alpha. A template's
  particleType Additive never reaches these renderers. The only shipped one is Codename_Excalibur's
  `em_excalibur_trail_TrailGlow`. So placed groups now always draw normal-blend, lit and fogged
  (`ParticleFog.h` `placedGroupDraw`), which avoids the problem of ONE/ONE fog brightening.
  Test: `ShaderMath_TrailsAndPlacedQuadsAlwaysBlendAndFog`. Still open: the port fogs placed
  triangles per pixel rather than per vertex (oFog), a small difference.
- **Where:** `src/Render/LevelRenderer.cpp`, the placed-triangle path (`fogged = !additive`)
- **Retail:** Trail.fx and NonScreenAlignedParticles.fx set `FogEnable = TRUE` in
  every technique, additive ones included. (Screen-aligned additive sprites are
  unfogged in retail, see A-8.)
- **Port:** additive placed-triangle and trail groups are left unfogged. Placed
  triangles also fog per pixel rather than per vertex (`oFog`), a small difference.
- **Fix:** confirm in RendDX9 which path draws trails and placed quads and what fog
  state it leaves set, then fog them as retail does.

## Open verification questions (no known bug yet)

| ID | Commit | Question |
|---|---|---|
| V-1 | 0c7061c / 3443d51 | Retail's `0x0043ef30` skips the blend for a node with no child, no sibling and `+0x44 == 0` that isn't a camera. The port blends every object. What is +0x44? Needs an xref pass over the object layout. |
| V-2 | 86d196d | AVM1 GotoFrame (0x81) and GoToLabel (0x8c) queueing. SwiffPlayer's opcode dispatch is an unrecovered jump table, so only the method side (`gotoAndPlay`/`gotoAndStop`) has been checked. |
| V-3 | beff7b5, 8178a1e, 4bddd3c | The "front faces only" claim for the scene rays (`0x006fb610`) hasn't been traced. |
| V-4 | f68bbe0 | Retail runs every ammo component's update twice a tick. The port doubles only abilities; check whether overheat and recharge also need doubling. |
| V-5 | c9e31ec, 861bf5e, 5d1ff3f, 3120aef, 87dd54d, f51d34c, b3344c1 | Claims not yet checked: 0x00570ab0 kit unlocks; 0x0058da3f corpse kit (and its shortcut for early revives); StickyCollisionComp; supply drop lifetime; crate sync and the network kill feed; 0x004baaf0 "spend on both factions". |
| V-6 | 0b87ad7 | A cloaked player's name is hidden as an admitted shortcut. Retail's name pass has no cloak test, and the hide hasn't been traced. |
| V-7 | cb386ee | Retail also requires player slot 0x114 to be false before turning the map. The port uses "man down"; confirm that's what slot 0x114 is. |
| V-8 | 6aa0258 (round 1) | The minimap inside-icon fix dropped the "not on the commander screen" condition. That assumes retail keeps the player's camera on the commander screen, which was inferred, not traced. |
| V-9 | e4e3cfe (round 1) | The enhanced Doppler cap at half the speed of sound (`kEnhancedDopplerMaxSpeedFraction` 0.5) was the agent's choice, not a retail value. Confirm it sounds right, or tune it. |
| V-10 | 75268cd (round 1) | The renderer's look == 1 path keeps the normal geometry for a node with only 2 geometries; retail hides those. No shipped Titan mesh is affected today. |
| V-11 | dc6422a (round 1) | Sat Track sweep: does retail also draw it on the commander's minimap, beyond the port's `CommanderShow != 0` gate? Is +0xb3c exactly 150 m? |
| V-12 | bd51c18, 75bbef3 (round 1) | Retail's use of `nametag.healthActivationLevel` for the core icon, and node creation for a list that shares its parent's name, are both untraced. |
| V-13 | b3d105b (round 1) | Does a loaded silo round survive a team change (0x005f3a40, fields +0x20/+0x34/+0x3c)? |
| V-14 | 429806a, a591847 (round 1) | "Only the two pilot weapons set fireRate 100" needs the `.ai` data. AIDLL player slot 0x84 meaning "is AI" was inferred from consistent use. |

## Rejected (checked and not a bug)

Don't reopen these without new evidence.

- **Minimap: reset the angles when not at minimap size** (round 2, presentation
  review item 4). Rejected. Retail 0x0078f680: `78f810 movzbl 0x709` /
  `78f819 je 0x78fd28` jumps *past* the reset at 0x78fcf7..0x78fd21 when the map
  isn't at minimap size, so the angles are kept, not reset. They're reset only at
  minimap size when the map is static, the player is dead or the player is down.
  `HudManager::tickMapTurn` (`src/Hud/Hud.cpp` ~1698) already matches this.
- **Bots: answer only just-arrived "go go go" messages** (round 1 quality review).
  Rejected. Retail AIDLL 0x10069f50 walks the whole type-11 list with no
  answered check. The port's 2 s send gap on roger_that already lets only one
  reply through, and the code has a comment saying so.
