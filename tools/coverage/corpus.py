#!/usr/bin/env python3
"""The shipped-script call corpus (coverage-2's `scripts.py`).

Every .con / .tweak / .inc / .ai in the mod's archives (the mod root's
*.zip) and loose files outside Levels/, plus these levels' archives (every
*.zip in the level folder):
  - the 20 levels that ship archive.md5;
  - the 15 official `_coop` variants (a `*_coop` folder whose base level is
    one of the 20, case-insensitively);
  - the 3 `*_Titan16` levels.
Archives are read with Python's zipfile (the same bytes `unzip -p` gives).

A call is a line whose first word is `Object.method...` (a dot in it, not
a v_/c_ variable), outside beginRem/endRem blocks and `rem` lines; the
name is that word, lower-cased. Arguments aren't parsed.

Output: <out>/calls.json: {"files": N, "calls": total, "levels": [...],
"names": {lower-cased name: count}, "first": {name: "archive:path"}}.
Game data doesn't change between commits, so one corpus serves every
before/after.
"""
import argparse
import os
import re
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402

EXT = (".con", ".tweak", ".inc", ".ai")
WORD_RE = re.compile(r"^\s*([A-Za-z_][\w]*\.[\w.]+)")


def levels(mod):
    lv = os.path.join(mod, "Levels")
    all_levels = sorted(os.listdir(lv))
    base = [d for d in all_levels if os.path.exists(os.path.join(lv, d, "archive.md5"))]
    lower = {b.lower() for b in base}
    coop = [d for d in all_levels if d.lower().endswith("_coop") and d.lower()[:-5] in lower]
    titan = [d for d in all_levels if d.lower().endswith("_titan16")]
    return base, coop, titan


def count_text(text, names, first, where):
    calls = 0
    in_rem = False
    for line in text.splitlines():
        s = line.strip()
        if not s:
            continue
        low = s.lower()
        if in_rem:
            if low.startswith("endrem"):
                in_rem = False
            continue
        if low.startswith("beginrem"):
            in_rem = True
            continue
        if low.startswith("rem ") or low == "rem" or low.startswith("rem\t"):
            continue
        m = WORD_RE.match(s)
        if not m:
            continue
        w = m.group(1).lower()
        if w.startswith(("v_", "c_")):
            continue
        names[w] = names.get(w, 0) + 1
        first.setdefault(w, where)
        calls += 1
    return calls


def main():
    ap = common.add_common_args(argparse.ArgumentParser(description=__doc__.split("\n")[0]))
    ap.add_argument("--mod", default=None, help="mods/bf2142 folder (default from $BF2142_GAME_DIR)")
    args = ap.parse_args()
    out = common.out_dir(args.out)
    mod = args.mod or common.mod_dir(common.REPO)
    base, coop, titan = levels(mod)
    names, first = {}, {}
    files = calls = 0
    archives = []
    for dirpath, dirnames, filenames in os.walk(mod):
        if os.path.relpath(dirpath, mod).split(os.sep)[0] == "Levels":
            dirnames[:] = []
            continue
        dirnames.sort()
        archives += sorted(os.path.join(dirpath, f) for f in filenames if f.lower().endswith(".zip"))
    for lvl in base + coop + titan:
        d = os.path.join(mod, "Levels", lvl)
        archives += sorted(os.path.join(d, f) for f in os.listdir(d) if f.lower().endswith(".zip"))
    for z in archives:
        with zipfile.ZipFile(z) as zf:
            for info in zf.infolist():
                if info.is_dir() or not info.filename.lower().endswith(EXT):
                    continue
                text = zf.read(info).decode("latin-1")
                files += 1
                calls += count_text(text, names, first, os.path.relpath(z, mod) + ":" + info.filename)
    for dirpath, dirnames, filenames in os.walk(mod):
        rel = os.path.relpath(dirpath, mod)
        if rel.split(os.sep)[0] == "Levels":
            dirnames[:] = []
            continue
        for fn in sorted(filenames):
            if fn.lower().endswith(EXT):
                with open(os.path.join(dirpath, fn), encoding="latin-1") as f:
                    files += 1
                    calls += count_text(f.read(), names, first, os.path.join(rel, fn))
    common.save_json(os.path.join(out, "calls.json"), {
        "files": files, "calls": calls, "levels": base + coop + titan,
        "names": names, "first": first})
    print("levels: %d with archive.md5, %d coop, %d Titan16" % (len(base), len(coop), len(titan)))
    print("files %d, calls %d, distinct names %d" % (files, calls, len(names)))


if __name__ == "__main__":
    main()
