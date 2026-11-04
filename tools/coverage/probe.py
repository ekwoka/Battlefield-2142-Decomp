#!/usr/bin/env python3
"""Console API probe (coverage.md / coverage-2's `probe.py` + `probe2.py`).

Both console tables (symbols/BF2142.exe.console.csv and
RendDX9_ori.dll.console.csv) are rewritten so that every row is a method
taking 0 to 13 arguments, passed with --api, and each distinct
`Object.method` is called once with --console and 11 arguments. A method
the rewrite implements rejects them ("Too many arguments", "Properties only
takes one argument", "not active"...); a stub takes them and prints
nothing. So a name is `impl` when its console line has output, else `stub`.

Modes (each a separate run on Camp_Gibraltar gpm_cq 32, --simulate 1,
--nosound --no-movies; no window is opened):
  sim   plain
  host  --host --dedicated
  shot  --screenshot <out>/probe_shot.png
The union (impl in any mode) is what the tables use.

Output: <out>/probe_<mode>.json ({name: "impl"|"stub"}), probe_union.json,
and the run logs probe_<mode>.log.
"""
import argparse
import csv
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402

TABLES = ["BF2142.exe.console.csv", "RendDX9_ori.dll.console.csv"]
MODES = {
    "sim": [],
    "host": ["--host", "--dedicated"],
    "shot": ["--screenshot", "{out}/probe_shot.png"],
}


def read_rows(repo):
    rows = []
    for t in TABLES:
        with open(os.path.join(repo, "symbols", t), newline="") as f:
            for r in csv.DictReader(f):
                r["table"] = t
                rows.append(r)
    return rows


def write_probe_tables(repo, out):
    paths = []
    for t in TABLES:
        src = os.path.join(repo, "symbols", t)
        dst = os.path.join(out, "probe_" + t)
        with open(src, newline="") as f, open(dst, "w", newline="") as g:
            rd = csv.reader(f)
            wr = csv.writer(g, lineterminator="\n")
            head = next(rd)
            wr.writerow(head)
            for r in rd:
                if len(r) < 6:
                    continue
                r[2], r[4], r[5] = "method", "0", "13"
                wr.writerow(r)
        paths.append(dst)
    return paths


def names(rows):
    seen = []
    s = set()
    for r in rows:
        if r["object"] == "?":
            continue
        n = r["object"] + "." + r["method"]
        if n not in s:
            s.add(n)
            seen.append(n)
    return seen


def run_mode(binary, game, mode, tables, allnames, out, extra, level, timeout):
    cmd = [binary, "--game", game]
    for t in tables:
        cmd += ["--api", t]
    cmd += ["+level", level[0], "+gameMode", level[1], "+mapSize", level[2], "--simulate", "1"]
    cmd += [a.format(out=out) for a in MODES[mode]]
    cmd += extra
    for n in allnames:
        cmd += ["--console", n + " x x x x x x x x x x x"]
    log = os.path.join(out, "probe_%s.log" % mode)
    with open(log, "w") as f:
        p = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=timeout, cwd=out)
    result = {}
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.startswith("console: "):
                continue
            body = line[len("console: "):].rstrip("\n")
            name = body.split(" ", 1)[0]
            if name not in result and name in allnames_set(allnames):
                result[name] = "impl" if " -> " in body else "stub"
    missing = [n for n in allnames if n not in result]
    return result, missing, p.returncode


_SET = {}


def allnames_set(allnames):
    key = id(allnames)
    if key not in _SET:
        _SET[key] = set(allnames)
    return _SET[key]


def main():
    ap = common.add_common_args(argparse.ArgumentParser(description=__doc__.split("\n")[0]))
    ap.add_argument("--bin", default=None, help="bf2142 binary (default <repo>/build/release/src/Main/bf2142)")
    ap.add_argument("--game", default=None, help="game install (default $BF2142_GAME_DIR or source/Battlefield 2142)")
    ap.add_argument("--modes", default="sim,host,shot")
    ap.add_argument("--no-movies-flag", default="--no-movies", help="pass '' for builds older than the flag")
    ap.add_argument("--level", default="Camp_Gibraltar,gpm_cq,32")
    ap.add_argument("--timeout", type=int, default=900)
    args = ap.parse_args()
    out = os.path.abspath(common.out_dir(args.out))
    binary = args.bin or os.path.join(args.repo, "build", "release", "src", "Main", "bf2142")
    game = args.game or common.game_dir(common.REPO)
    rows = read_rows(args.repo)
    allnames = names(rows)
    tables = write_probe_tables(args.repo, out)
    extra = ["--nosound"] + ([args.no_movies_flag] if args.no_movies_flag else [])
    union = {n: "stub" for n in allnames}
    per = {}
    for mode in args.modes.split(","):
        res, missing, rc = run_mode(binary, game, mode, tables, allnames, out, extra, args.level.split(","), args.timeout)
        common.save_json(os.path.join(out, "probe_%s.json" % mode), res)
        per[mode] = res
        impl = sum(1 for v in res.values() if v == "impl")
        common.log("%s: exit %d, %d names answered, %d impl / %d stub, %d without a console line"
                   % (mode, rc, len(res), impl, len(res) - impl, len(missing)))
        for n, v in res.items():
            if v == "impl":
                union[n] = "impl"
    common.save_json(os.path.join(out, "probe_union.json"), union)
    impl = sum(1 for v in union.values() if v == "impl")
    print("probe union: %d distinct names, %d impl / %d stub" % (len(union), impl, len(union) - impl))
    for mode, res in per.items():
        only = sorted(n for n, v in res.items() if v == "impl" and all(per[m].get(n) != "impl" for m in per if m != mode))
        print("  impl only in %s: %d%s" % (mode, len(only), (" (" + ", ".join(only[:8]) + (", ..." if len(only) > 8 else "") + ")") if only else ""))
        print("  %s alone: %d impl" % (mode, sum(1 for v in res.values() if v == "impl")))


if __name__ == "__main__":
    main()
