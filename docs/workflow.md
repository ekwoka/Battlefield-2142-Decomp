# Decompilation workflow

This page explains how a module goes from the retail binary to verified,
readable C++, and lists the pitfalls already found. `Memory.dll` and
`BF2142Audio.dll` were done this way and serve as worked examples
(`src/Memory`, `src/Sound`).

## 1. Analyse

```sh
tools/ghidra/analyze.sh Module.dll                      # import + analysis
DECOMPILE=1 tools/ghidra/analyze.sh Module.dll          # + decompiled C dump
python3 -I tools/decomp/find_register_args.py build/analysis/Module.dll/decomp \
    > build/analysis/Module.dll/conventions.csv
DECOMPILE=1 tools/ghidra/analyze.sh Module.dll          # re-run with __thiscall fixed
```

The pipeline runs these scripts on every module:

| Script | Why |
|---|---|
| `ConfigureAnalysis.java` (pre) | Turns off "Non-Returning Functions - Discovered". It flagged `operator delete` (a `jmp [bf_free]` thunk) as non-returning and cut ~14,000 functions short. |
| `CreateStaticInitializers.java` | Creates the C++ static initializers listed in the CRT's `__xc_a..__xc_z` table (4,176 entries at `0x009f8004` in the EXE). They are only reachable through that table, so auto-analysis skips most of them, but they hold the registration code: console methods, class factories, Python bindings. |
| `ApplyCallingConventions.java` | Without RTTI or PDBs, Ghidra treats methods as `__cdecl` and shows `this` as `in_ECX`. This applies `__thiscall`/`__fastcall` to the functions that read ECX/EDX on entry (21,629 in the EXE). |
| `FindVtables.java` | Finds vtables through constructor vptr stores and creates the functions that are only ever called virtually (Ghidra misses them otherwise). Writes `vtables.csv`. |
| `ApplyKnownSymbols.java` | Applies `symbols/<module>.csv`. |
| `TagSourceFiles.java` | Tags functions with the DICE source file named in their assert strings. |
| `ExportFunctions.java` / `ExportDecompiled.java` | Inventory and decompiled C in `build/analysis/<module>/`. |

## 2. Read

Read the decompiled code with these tools rather than the raw dump:

- `tools/decomp/compact.py <decomp dir> [lo-hi] [--only a,b] --skip-lib --min-size=25`
  shortens STL names and drops inlined MSVC runtime/STL code (iostreams,
  exception funclets, string concatenation) and trivial accessors. That cuts
  the text to read by about half.
- `tools/decomp/vtable.py <module> <vtable>` lists the slot targets.
- `tools/decomp/vtable_methods.py <module> <vtable>` prints every slot's body
  in slot order. Start here: the interfaces a module exposes define its
  architecture.

Strings are the best naming source. Assert paths name the file, log messages
name the operation, and `.con` property names (search the extracted archives
for `ObjectTemplate.*`) name the setters.

## 3. Rewrite

- Mirror DICE's source layout (`src/<Area>/<Module>/<OriginalFile>.cpp`) and
  use the `dice::hfe::*` namespaces.
- Write idiomatic C++ with the same behaviour. Keep the original's numbers
  (defaults, thresholds, clamps) and comment where they come from. When the
  original has an obvious bug, either reproduce it with a comment or fix it
  with a comment that says so (for example the sound fade start time stored
  as float bits).
- Keep the platform out: OS calls go through `src/Platform`, and libraries
  are loaded through tables the way the original did with OpenAL.
- Keep 64-bit builds safe. The originals assume 32-bit pointers; see
  `Memory.cpp`, whose pool headers only apply to 32-bit builds.

## 4. Interfaces that cross module boundaries

Until every module is rewritten, our replacements must interoperate with the
retail executable, so interfaces it calls must match MSVC 7.1's binary layout.
GCC (MinGW, i686) can match it as long as these rules hold:

- **Slot order is fixed**: declare virtuals in the original vtable order and
  note the slot numbers.
- **No virtual destructors.** MSVC has one "scalar deleting destructor" slot,
  usually the last; the Itanium ABI uses two. Declare
  `virtual void* destroy(unsigned flags)` at the original slot instead.
- **No overloaded virtuals.** MSVC places overloads in reverse declaration
  order, so give each one a distinct name (`play`, `playLooped`, ...).
- Member functions are `__thiscall` on both compilers. `float` returns come
  back in ST0 on both. Avoid returning structs by value from methods.
- Small interfaces implemented by the *game* (loggers, file systems,
  transforms, envelopes) need the same care, with protected non-virtual
  destructors.
- C++-mangled DLL exports are defined as `extern "C"` forwarders and renamed
  with a `.def` file (see `src/Memory/retail/Memory.def`).

## 5. Verify

- Unit and integration tests: `ninja -C build/native && build/native/tests/bf2142_tests`. They run in parallel processes, one per core; `-j N` sets how many (`-j 1` runs them in one process, as on Windows), and a name argument runs only the tests containing it. Tests over a second print their time.
  The native build uses ASan/UBSan. Sound tests need
  `BF2142_SOUND_TEST_DIR` pointing at extracted game sounds, and run on
  OpenAL Soft's silent `null` driver.
- Run the same tests as 32-bit Windows code:
  `tools/crossover-run.sh build/win32/tests/bf2142_tests.exe`.
- Retail drop-in test in a CrossOver bottle:
  ```sh
  cmake -S . -B build/win32 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake && ninja -C build/win32
  tools/retail-swap.sh install BF2142Audio.dll   # keeps the original as .retail
  BF2142_SOUND_LOG='C:\sound_trace.log' tools/retail-smoke.sh 60
  tools/retail-swap.sh restore BF2142Audio.dll
  ```
  `retail-smoke.sh` launches the game windowed and reports whether it is still
  alive, and its memory use, after N seconds.
