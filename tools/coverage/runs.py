#!/usr/bin/env python3
"""The 11 level runs (coverage.md / coverage-2's `runs.sh`).

`bf2142 --nosound --no-movies --verbose +level L +gameMode M +mapSize S
--simulate 5` on the same 11 combinations as every audit since the first;
no window is opened. From each log: the "N calls to M unimplemented
methods" line and its top list, the "console methods stubbed from" lines,
Python tracebacks and "Unknown object or method!" errors.

Output: <out>/runs/<level>_<mode>_<size>.txt (the logs), runs.json, and a
table on stdout. With --before <runs.json>, the before/after table and the
stub methods whose calls changed most (merged over the 11 runs).
"""
import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402

RUNS = [
    ("Belgrade", "gpm_nv", "32"),
    ("Camp_Gibraltar", "gpm_cq", "32"),
    ("Cerbere_Landing", "gpm_cq", "32"),
    ("Highway_Tampa", "gpm_sl", "64"),
    ("Minsk", "gpm_ti", "48"),
    ("Molokai", "gpm_ti", "48"),
    ("Operation_Shingle", "gpm_sl", "32"),
    ("sidi_power_plant_coop", "gpm_coop", "32"),
    ("Suez_Canal", "gpm_cq", "64"),
    ("Verdun", "gpm_sp", "16"),
    ("Wake_Island_2142", "gpm_ca", "64"),
]
SUMMARY_RE = re.compile(r"^(\d+) calls to (\d+) unimplemented methods")
TOP_RE = re.compile(r"^\s+(\d+) (\S+)\s*$")
STUBBED_RE = re.compile(r"^(\d+) console methods stubbed from (.*)$")


def parse(path):
    r = {"calls": None, "methods": None, "top": {}, "stubbed": {}, "tracebacks": 0, "unknown": 0}
    in_top = False
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = SUMMARY_RE.match(line)
            if m:
                r["calls"], r["methods"] = int(m.group(1)), int(m.group(2))
                in_top = True
                continue
            if in_top:
                m = TOP_RE.match(line)
                if m:
                    r["top"][m.group(2)] = int(m.group(1))
                    continue
                in_top = False
            m = STUBBED_RE.match(line)
            if m:
                r["stubbed"][os.path.basename(m.group(2))] = int(m.group(1))
            if line.startswith("Traceback (most recent call last)"):
                r["tracebacks"] += 1
            if "Unknown object or method!" in line:
                r["unknown"] += 1
    return r


def main():
    ap = common.add_common_args(argparse.ArgumentParser(description=__doc__.split("\n")[0]))
    ap.add_argument("--bin", default=None)
    ap.add_argument("--game", default=None)
    ap.add_argument("--no-movies-flag", default="--no-movies")
    ap.add_argument("--seconds", default="5")
    ap.add_argument("--parse-only", action="store_true", help="re-read the logs in <out>/runs")
    ap.add_argument("--before", default=None, help="runs.json of the older run")
    args = ap.parse_args()
    out = common.out_dir(args.out)
    rdir = common.out_dir(os.path.join(out, "runs"))
    binary = args.bin or os.path.join(args.repo, "build", "release", "src", "Main", "bf2142")
    game = args.game or common.game_dir(common.REPO)
    res = {}
    for lvl, mode, size in RUNS:
        key = "%s %s %s" % (lvl, mode, size)
        log = os.path.join(rdir, "%s_%s_%s.txt" % (lvl, mode, size))
        if not args.parse_only:
            cmd = [binary, "--game", game, "--nosound"] + ([args.no_movies_flag] if args.no_movies_flag else []) + [
                "--verbose", "+level", lvl, "+gameMode", mode, "+mapSize", size, "--simulate", args.seconds]
            with open(log, "w") as f:
                subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=1800, cwd=rdir)
        res[key] = parse(log)
    common.save_json(os.path.join(out, "runs.json"), res)
    before = common.load_json(args.before) if args.before else None
    print("| Run | %scalls / methods | tracebacks | unknown | stubbed (EXE / RendDX9) |" % ("before → after: " if before else ""))
    print("|---|---|---|---|---|")
    for key, r in res.items():
        st = "%s / %s" % (r["stubbed"].get("BF2142.exe.console.csv", "?"), r["stubbed"].get("RendDX9_ori.dll.console.csv", "?"))
        cm = "%s / %s" % (r["calls"], r["methods"])
        if before and key in before:
            b = before[key]
            cm = "%s / %s → %s" % (b["calls"], b["methods"], cm)
        print("| %s | %s | %d | %d | %s |" % (key, cm, r["tracebacks"], r["unknown"], st))
    if before:
        merged_b, merged_a = {}, {}
        for key in res:
            for n, c in before.get(key, {}).get("top", {}).items():
                merged_b[n] = merged_b.get(n, 0) + c
            for n, c in res[key]["top"].items():
                merged_a[n] = merged_a.get(n, 0) + c
        gone = sorted((n for n in merged_b if n not in merged_a), key=lambda n: -merged_b[n])
        print("\nleft the stub list (merged calls before): " + ", ".join("%s %d" % (n, merged_b[n]) for n in gone))
        new = sorted((n for n in merged_a if n not in merged_b), key=lambda n: -merged_a[n])
        if new:
            print("new on the stub list: " + ", ".join("%s %d" % (n, merged_a[n]) for n in new))
    merged = {}
    for r in res.values():
        for n, c in r["top"].items():
            merged[n] = merged.get(n, 0) + c
    print("\nmerged stub calls, top 40: " + ", ".join("%s %d" % (n, c) for n, c in sorted(merged.items(), key=lambda x: -x[1])[:40]))


if __name__ == "__main__":
    main()
