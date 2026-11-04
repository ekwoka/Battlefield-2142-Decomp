#!/usr/bin/env python3
"""Per-function features from the decompilation (uncited.md's `unc/extract.py`).

For every function of build/analysis/<module>/decomp/*.c (blocks headed
`// ==== <address> <name> size=<n>`): its string literals and `s_` string
labels, the functions it calls (FUN_xxxxxxxx( or a named function of
functions.csv followed by "("), and the vtables it references
(`vtbl_xxxxxxxx`, `PTR_..._xxxxxxxx` into vtables.csv).

The decompilation doesn't change with the source tree, so the output is
cached: <out>/features_<module>.json ({address hex: {"s": [...], "c":
[...], "v": [...]}}).
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402

HEAD_RE = re.compile(r"^// ==== ([0-9a-f]{8}) (\S+) size=(\d+)", re.M)
STR_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
SLABEL_RE = re.compile(r"\bs_([A-Za-z0-9_]+?)_([0-9a-f]{8})\b")
FUN_RE = re.compile(r"\b(?:FUN|thunk_FUN)_([0-9a-f]{8})\s*\(")
NAMED_CALL_RE = re.compile(r"([A-Za-z_~][\w:~<>,@]*)\s*\(")
VT_RE = re.compile(r"\b(?:vtbl|PTR_[A-Za-z0-9_]*?)_([0-9a-f]{8})\b")


def extract(module, repo):
    funcs = common.read_functions(module, repo, keep_static_init=True)
    by_name = {}
    for a, (n, _) in funcs.items():
        by_name.setdefault(n, []).append(a)
    unique = {n: v[0] for n, v in by_name.items() if len(v) == 1 and not n.startswith("FUN_")}
    vt = set()
    vpath = os.path.join(common.analysis_dir(module, repo), "vtables.csv")
    if os.path.exists(vpath):
        with open(vpath) as f:
            next(f)
            for line in f:
                vt.add(int(line.split(",", 1)[0], 16))
    out = {}
    ddir = os.path.join(common.analysis_dir(module, repo), "decomp")
    if not os.path.isdir(ddir):
        return out
    for fn in sorted(os.listdir(ddir)):
        if not fn.endswith(".c"):
            continue
        with open(os.path.join(ddir, fn), encoding="utf-8", errors="replace") as f:
            text = f.read()
        heads = list(HEAD_RE.finditer(text))
        for i, h in enumerate(heads):
            body = text[h.end(): heads[i + 1].start() if i + 1 < len(heads) else len(text)]
            a = int(h.group(1), 16)
            strings = [m.group(1) for m in STR_RE.finditer(body)]
            strings += ["s_" + m.group(1) for m in SLABEL_RE.finditer(body)]
            calls = set(int(m.group(1), 16) for m in FUN_RE.finditer(body))
            for m in NAMED_CALL_RE.finditer(body):
                t = unique.get(m.group(1))
                if t is not None:
                    calls.add(t)
            calls.discard(a)
            vts = set(int(m.group(1), 16) for m in VT_RE.finditer(body)) & vt
            out["%08x" % a] = {"s": strings, "c": ["%08x" % c for c in sorted(calls) if c in funcs],
                               "v": ["%08x" % v for v in sorted(vts)]}
    return out


def load(module, repo, out):
    path = os.path.join(out, "features_%s.json" % module)
    if os.path.exists(path):
        return common.load_json(path)
    data = extract(module, repo)
    common.save_json(path, data)
    return data


def main():
    ap = common.add_common_args(argparse.ArgumentParser(description=__doc__.split("\n")[0]))
    ap.add_argument("--modules", default="BF2142.exe,RendDX9_ori.dll,AIDLL.dll")
    args = ap.parse_args()
    out = common.out_dir(args.out)
    for m in args.modules.split(","):
        data = extract(m, args.repo)
        common.save_json(os.path.join(out, "features_%s.json" % m), data)
        print("%s: %d functions, %d with strings" % (m, len(data), sum(1 for d in data.values() if d["s"])))


if __name__ == "__main__":
    main()
