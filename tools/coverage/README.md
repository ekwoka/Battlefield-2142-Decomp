# Coverage audit scripts

The measurements behind docs/coverage.md, coverage-2.md, uncited.md,
coverage-3.md, coverage-4.md, coverage-5.md, coverage-6.md, coverage-7.md and coverage-8.md, as scripts. Earlier audits ran
throw-away copies of these from a temp folder; these were rebuilt from the
audits' method notes and checked against coverage-3's figures (below).

Run every Python script with `python3 -I`. Each takes `--repo <tree>`
(default: this checkout) and `--out <dir>` (default `build/coverage`, which
git ignores). The game install is `$BF2142_GAME_DIR`, else
`source/Battlefield 2142`.

Nothing here opens a window or goes on the network. The probe and the
level runs use `--simulate` (and `--screenshot` for one probe mode), with
`--nosound --no-movies`. The host probe is `--host --dedicated` without
`--online`.

## Running

```sh
ninja -C build/release src/Main/bf2142
tools/coverage/run_all.sh --out build/coverage/head
# an older commit, for before/after:
tools/coverage/build_old.sh 5480574 /tmp/wt-5480574
tools/coverage/run_all.sh --repo /tmp/wt-5480574 --out build/coverage/old
tools/coverage/run_all.sh --out build/coverage/head --before build/coverage/old
git worktree remove --force /tmp/wt-5480574
```

The source-based measurements (rows' literal rule, citations, classifier, hedges) read the tree as it is on disk. If the checkout has uncommitted work, measure a clean `build_old.sh <HEAD> <dir>` worktree instead, so the figures belong to a commit.

A full run takes about half a minute on an M-series Mac. For builds
older than `--no-movies`, pass `--no-movies-flag=`.

## The scripts

| Script | What it measures | Output |
|---|---|---|
| `corpus.py` | The shipped-script call corpus. It reads every `.con/.tweak/.inc/.ai` in the mod's archives (every `*.zip` outside `Levels/`, `Objects/` included) and loose files, plus the archives of the 20 levels with `archive.md5`, the 15 official `_coop` variants (a `_coop` folder whose base level is one of the 20) and the 3 `_Titan16` levels. A call is a line whose first word is `Object.method…`, outside `beginRem`/`endRem` and `rem`. | `calls.json` |
| `probe.py` | The console API probe. Both console tables are rewritten so every row takes 0–13 arguments and are passed with `--api`. Each distinct name gets one `--console "<name> x x x x x x x x x x x"`. A name whose console line has output (an argument-count, property or not-active error) is implemented; one with no output is a stub. Three runs (plain, `--host --dedicated`, `--screenshot`) on Camp_Gibraltar gpm_cq 32 `--simulate 1`; the union is what counts. | `probe_{sim,host,shot,union}.json`, logs |
| `rows.py` | The 2,837 console rows classified as impl, stored-read, stored-unread or stub. A stored row is a non-impl ObjectTemplate property or 1-argument setter, read if its name (or `<name>.list`) is a string literal in src, or a key src composes from literals (the rule is in the docstring: `base + "Range"`, `"sway.gunSway" + axis + "Steps"`, `"supplyValueTeam" + n`, `"alternateTemplate" + std::to_string(i)`). Script calls are joined to each row. Also: Table A, the stubs and unread properties the scripts call, and the lower bound (last segment absent from src). `--before` adds the deltas. | `rows.json`, `rows_summary.json`, `tableA.md`, `stubs_called.md`, `unread_called.md`, `unread.json`, `delta.md` |
| `runs.py` | The 11 level runs (`--verbose --simulate 5`). From each: "N calls to M unimplemented methods", the start-up "console methods stubbed" lines, Python tracebacks and "Unknown object or method!" errors. | `runs/*.txt`, `runs.json`, table on stdout |
| `cite.py` | Address citations per module, as functions and bytes against `build/analysis/<module>/functions.csv` (static_init left out), from src/ and docs/ (tests/ with `--tests`). DLL addresses that collide go to the DLL named nearest before them (within 1,500 characters), else to the src directory's DLL (src/AI and, since coverage-7, src/Game/Bots* → AIDLL), else they are dropped. engine.md's generated appendix is counted separately: every figure is given with and without it. `--headings` adds a fallback (the DLL in the enclosing `##` heading of a doc) that coverage-3 didn't use. `--no-audit-docs` leaves out docs/coverage*.md and docs/uncited.md, whose gap lists cite addresses nothing implements (coverage-5: about 1.2 points of the EXE and 3 of RendDX9; each new audit adds a little). Uncited runs: ≥ 30 KB for the EXE, ≥ 20 KB for the DLLs. | `cite.json` |
| `extract.py` | Per-function strings, callees and vtables from `build/analysis/<module>/decomp/*.c`. Cached, since the decompilation doesn't change with the tree. | `features_<module>.json` |
| `uncited.py` | The uncited classifier: third-party (a), dev-only (b), present-uncited (c), missing (d), or "?"; for SwiffPlayer also online-only (o) and unused by the shipped movies (u). The rules are in the script's docstring; SwiffPlayer's inputs are in `swf.py` (below). It reuses uncited.md's calibration factors (EXE 0.84, RendDX9 0.69, AIDLL 0.48) and SwiffPlayer's own (0.83). `--recalibrate` measures this classifier's own propagation precision instead. `--min-run` sets the smallest (d) run listed (4 KB by default; `run_all.sh` lists 2 KB runs for the no-audit-docs run). | `uncited_<with/without>.json` (or `<--name>.json`), `labels_<appendix or --name>_<module>.json` |
| `hedges.py` | The hedge notes in engine.md and src ("not traced", "stand-in", "fitted", "approximate"…), with file:line. | `hedges.txt`, `hedges.json` |
| `run_all.sh`, `build_old.sh` | All of the above in order; a worktree build of an older commit without network (FetchContent pointed at `build/release/_deps`). | |

## How close to the earlier audits

Checked on a worktree build of 5480574, the commit coverage-3 measured:

- **Probe, rows, level runs:** these reproduce coverage-3 exactly.
  - Probe union: 1,034 / 1,654; 876 with `--simulate` alone; 158 names only when hosting.
  - Rows: 1,042 impl, 476 stub, 733 stored-read, 586 stored-unread. EXE 850 / 280, RendDX9 192 / 196.
  - Unread names: 555 (lower bound 476). 71 called stub rows, 403 called unread rows.
  - Every level run's "N calls to M" matches (e.g. Belgrade 6,507 / 42), as do the unknown-method counts and the start-up 521 / 195.
- **Corpus:** this gives 6,702 files and 816,424 calls, against coverage-2's 6,758 / 824,614. coverage-2 didn't record its exact file rules, so call counts here run 0.2–2% under the earlier audits'. Compare them only with each other.
- **Citations:** the EXE reproduces exactly at 5480574 (2,678 functions, 1,154,647 B, 12 runs ≥ 30 KB / 0.81 MB), at 991d041 (1,983 / 852,825) and at 6001a99 (2,124 / 955,445). So does SwiffPlayer (168 / 124,680; 153 / 122,700). RendDX9 comes out one function high (+1,084 B, `0x100b2af0`) and AIDLL one function low (−18 B) at all three commits. The ambiguous count is 183 against 181.
- **Uncited classifier:** rebuilt from uncited.md's rule list; the original's string lists and thresholds were lost.
  - At 5480574 the EXE and AIDLL land within about 2 points of coverage-3:
    - EXE: lower 47.7% against 49.8%; calibrated 80.7% against 80.9%; upper 87.1% against 86.9%.
    - AIDLL: 34.4 / 60.7 / 88.5% against 34.0 / 59.9 / 86.4%.
  - RendDX9 comes out about 6 points low: 32.2 / 64.2 / 78.5% against 30.3 / 70.1 / 87.8%. It finds about 268 KB of raw (d) where coverage-3 found 153 KB.
  - Read its figures as before/after on one classifier, not as a continuation of coverage-3's series.
  - `--recalibrate` gives EXE 0.47, RendDX9 0.72 and AIDLL 0.35 for this classifier's propagation, against the reused 0.84 / 0.69 / 0.48.

## SwiffPlayer.dll

`uncited.py` measures SwiffPlayer like the other three modules; the module's own inputs are in `swf.py`. Its line is printed apart: "all three" stays the EXE, RendDX9 and AIDLL. The rules that differ, and why:

- **Libraries (a).** These are hand-checked ranges: GameSpy HTTP / SOAP, voice device enumeration, base64 and Speex (`0x100836f0`..`0x100a1990`); expat (`0x100e0800`..`0x100f2020`); the CRT. The library words are only the codec and parser ones. The general list's `plasma` and `punkbuster` would take the menus' own server-browser natives.
- **Dev (b).** gameswf's disassembler, timers and loader logs are dev. `profil` becomes `profiler`, since the plain word matches every native that names LocalProfileManager.
- **The natives (rule 3).** SwiffPlayer has no console rows. Instead, each `dice.*` member's body is found in the constructors that register it (named per object in `src/UI/Player/NativeTable.cpp`). It then takes the member's status from uncited.md's natives table:
  - bound or bound elsewhere → (c);
  - online-only or leaving the game → (o);
  - dev → (b);
  - missing → (d).
  `MenuBindings_NativeCoverage` keeps that table equal to what the rewrite binds.
- **Hand-checked functions.** `swf.HAND` labels single functions, each with its reason:
  - gameswf parts the rewrite replaces by design → (c);
  - online pages → (o);
  - gameswf parts the shipped movies never reach → (u): sound tags, DefineText, DefineFontInfo, JPEG2, ImportAssets, XML. The movies' tag and action inventory behind this is in uncited.md, "SwiffPlayer.dll: gameswf and the classifier".
- **(o) and (u) are left out of the needed code**, like (a) and (b), and are reported on their own line. They spread to callees whose callers are all (o) or (u), and to nothing else.
- **Strings.** Only identifier-like strings count. The rewrite's player is a new implementation of the format: it has gameswf's method and property names but not its log text. With every string counted, the loaders and built-ins src/UI plainly has came out (d) (coverage-8 §1c). src/UI/Avm1 is in the corpus for this module, since it holds the built-ins.
- **Factor 0.83.** This classifier's own `--recalibrate` measurement on the no-audit citations when the module was added; it moved between 0.83 and 0.87 over that pass, and the lower is kept. coverage-8's first pass used 0.55 with a different rule set. With 0.55 the figure is about 13 points lower.

