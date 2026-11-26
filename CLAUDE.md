# Notes for agents

## Running tests

- **Test with the Release build** (`build/release`, optimised, no sanitizers):

  ```sh
  nice ninja -C build/release -j 6
  BF2142_GAME_DIR="$PWD/source/Battlefield 2142" nice build/release/tests/bf2142_tests <filter>
  ```

  If `build/release` doesn't exist yet, set it up first with
  `cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release`.
- **Use the Debug build only to investigate failures** (`build/native`: Debug with AddressSanitizer
  and UndefinedBehaviorSanitizer, several times slower). Re-run just the failing tests there, by
  name, when a crash or a wrong result needs the sanitizers or a debugger. Never run the full suite
  in it.
- Run tests under `nice` and without `-j`. Niced, the runner uses every core; at a lower niceness
  it uses at most 8 (`defaultJobs` in `tests/TestMain.cpp`). It starts the slowest tests first,
  using the times it records in `test-times.txt` beside the test binary.
- **Sub-agents don't run the full suite.** Run only the tests your change touches, by filter (for
  example `Titan_`, `Vehicles_Hover`, or the names of the tests you added). Run the full suite at
  the very end of your task only if you're concerned that your change could affect systems your
  filtered tests don't cover, and say in your report that you did.
- Run one `bf2142_tests` command at a time. Several at once overload the machine and make the
  times meaningless.
- **The main agent runs the full suite** after several agents have finished and their work is
  merged. It sends any failures back to an agent to fix.
- Builds use `nice` and a limited `-j`: `nice ninja -C <build dir> -j 6`.
