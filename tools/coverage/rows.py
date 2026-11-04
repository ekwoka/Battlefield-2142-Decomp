#!/usr/bin/env python3
"""Console rows classified, script calls joined, unread template properties
(coverage-2/3's `analyze.py` + `tables.py`).

Each of the 2,837 rows of symbols/BF2142.exe.console.csv and
RendDX9_ori.dll.console.csv is one of:
  impl           the probe (probe.py, the union of its modes) got an
                 argument-count error back;
  stored-read    a non-impl ObjectTemplate property or 1-argument setter
                 (the rows main.cpp's StubMethods keeps on the template),
                 whose name, or `<name>.list` for list adders, is a string
                 literal in src (case-insensitive);
  stored-unread  the same, not a literal in src;
  stub           everything else.
The literal rule alone misses composed keys (`base + "Range"`, the effect
bundles' modifiers in Effects.cpp; `"sway.gunSway" + axis + "Steps"`), so a
name that two or three src literals concatenate to (the first at least 5
characters, the others camel-case continuations, the last one appended
with `+ "..."` somewhere in src) also counts as stored-read (`"composed": true`
in rows.json). So does a literal of at least 5 characters followed by a
number, when src appends `std::to_string(...)` to that literal
(`"alternateTemplate" + std::to_string(i)`, Effects.cpp), and a literal
a concatenation starts with followed by a camel-case literal
(`std::string("helpHud.closeToHelpStringKey") + side`, where side is one
of "Friendly" / "Enemy" / "Neutral", HudAlerts.cpp; `"supplyValueTeam" + n`). A lower bound is also given: unread names whose last segment
occurs nowhere in the src text.

Script calls come from corpus.py's calls.json (lower-cased Object.method).

Inputs: <probe>/probe_union.json, <corpus>/calls.json.
Output: <out>/rows.json, tableA.md, stubs_called.md, unread_called.md,
unread.json, and a summary on stdout. With --before <rows.json of an
older run>, also the deltas (newly implemented names, read <-> unread
moves, Table A with before -> after cells): delta.md.
"""
import argparse
import csv
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402

TABLES = [("BF2142.exe", "BF2142.exe.console.csv"), ("RendDX9", "RendDX9_ori.dll.console.csv")]
LIT_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
# A literal appended in a concatenation (`+ "Range"`): the last piece of a
# composed key.
SUFFIX_RE = re.compile(r'\+\s*"((?:[^"\\\n]|\\.)*)"')
# One a concatenation starts with (`"sway.gunSway" +`, also wrapped as
# `std::string("sway.gunSway") + axis`): a three-piece key's first.
PREFIX_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"\s*\)?\s*\+')
# One src numbers (`"alternateTemplate" + std::to_string(i)`).
NUMBERED_RE = re.compile(r'"((?:[^"\\\n]|\\.){5,})"\s*\+\s*std::to_string\(')
SRC_EXT = (".cpp", ".h", ".hpp", ".c", ".cc", ".inl", ".py", ".glsl", ".frag", ".vert", ".comp")


def src_text(repo):
    lits = set()
    suffixes = set()
    capped = set()
    prefixes = set()
    numbered = set()
    chunks = []
    for dirpath, dirnames, filenames in os.walk(os.path.join(repo, "src")):
        for fn in filenames:
            if not fn.endswith(SRC_EXT):
                continue
            with open(os.path.join(dirpath, fn), encoding="utf-8", errors="replace") as f:
                t = f.read()
            chunks.append(t.lower())
            for m in LIT_RE.finditer(t):
                lits.add(m.group(1).lower())
                if m.group(1)[:1].isupper() or m.group(1)[:1].isdigit():
                    capped.add(m.group(1).lower())
            for m in PREFIX_RE.finditer(t):
                prefixes.add(m.group(1).lower())
            for m in NUMBERED_RE.finditer(t):
                numbered.add(m.group(1).lower())
            for m in SUFFIX_RE.finditer(t):
                if m.group(1)[:1].isupper() or m.group(1)[:1].isdigit():
                    suffixes.add(m.group(1).lower())
    return lits, "\n".join(chunks), (suffixes, capped, prefixes, numbered)


def composed(name, lits, pieces):
    """Whether `name` is two or three literals put together, the first at
    least 5 characters (for three, one a concatenation starts with), the
    others camel-case continuations (an upper-case letter or a digit first)
    and the last one src appends (`+ "Range"`): the
    keys src builds as `base + "Range"` or `"sway.gunSway" + axis + "Steps"`.
    Also two pieces whose first starts a concatenation and whose second is
    any camel-case literal (`"supplyValueTeam" + n`, n "1" / "2"), and a
    numbered literal (`"alternateTemplate" + std::to_string(i)`)."""
    suffixes, capped, prefixes, numbered = pieces
    n = len(name)
    stem = name.rstrip("0123456789")
    if stem != name and len(stem) >= 5 and stem in numbered:
        return True
    for i in range(5, n):
        if name[:i] not in lits:
            continue
        if name[i:] in suffixes:
            return True
        if name[:i] not in prefixes:
            continue
        if name[i:] in capped:
            return True
        for j in range(i + 1, n):
            if name[i:j] in capped and name[j:] in suffixes:
                return True
    return False


def classify(repo, probe, calls):
    lits, text, suffixes = src_text(repo)
    rows = []
    for module, t in TABLES:
        with open(os.path.join(repo, "symbols", t), newline="") as f:
            for r in csv.DictReader(f):
                if r["object"] == "?":
                    continue
                name = r["object"] + "." + r["method"]
                state = "stub"
                glued = False
                if probe.get(name) == "impl":
                    state = "impl"
                elif module == "BF2142.exe" and r["object"] == "ObjectTemplate" and (
                        r["kind"] == "property" or (r["kind"] == "method" and r["min"] == "1" and r["max"] == "1")):
                    m = r["method"].lower()
                    if m in lits or m + ".list" in lits:
                        state = "stored-read"
                    elif composed(m, lits, suffixes):
                        state, glued = "stored-read", True
                    else:
                        state = "stored-unread"
                last = r["method"].split(".")[-1].lower()
                rows.append({
                    "module": module, "object": r["object"], "method": r["method"], "name": name,
                    "kind": r["kind"], "min": r["min"], "max": r["max"], "constructor": r.get("constructor", ""),
                    "state": state, "calls": calls.get(name.lower(), 0),
                    "last_in_src": last in text, "composed": glued,
                })
    return rows


def summary(rows):
    s = {"rows": len(rows)}
    for st in ("impl", "stub", "stored-read", "stored-unread"):
        s[st] = sum(1 for r in rows if r["state"] == st)
    for mod in ("BF2142.exe", "RendDX9"):
        s[mod + " impl"] = sum(1 for r in rows if r["module"] == mod and r["state"] == "impl")
        s[mod + " stub"] = sum(1 for r in rows if r["module"] == mod and r["state"] == "stub")
    unread = {}
    for r in rows:
        if r["state"] == "stored-unread":
            unread.setdefault(r["method"].lower(), r)
    s["unread names"] = len(unread)
    s["unread names lower bound"] = sum(1 for r in unread.values() if not r["last_in_src"])
    called = [r for r in rows if r["state"] == "stub" and r["calls"]]
    s["stub rows called"] = len(called)
    s["stub calls"] = sum(r["calls"] for r in called)
    s["stub calls without setLightSourceMask"] = s["stub calls"] - sum(r["calls"] for r in called if r["name"] == "Object.setLightSourceMask")
    uc = [r for r in rows if r["state"] == "stored-unread" and r["calls"]]
    s["unread rows called"] = len(uc)
    s["unread row calls"] = sum(r["calls"] for r in uc)
    s["unread names called"] = sum(1 for r in unread.values() if r["calls"])
    s["unread name calls"] = sum(r["calls"] for r in unread.values())
    names = {}
    for r in rows:
        if r["state"] == "impl":
            names[r["name"]] = True
    s["impl names (rows)"] = len(names)
    return s


def table_a(rows, before=None):
    key = lambda r: (r["module"], r["object"])
    agg = {}
    for r in rows:
        a = agg.setdefault(key(r), {"rows": 0, "impl": 0, "stored-read": 0, "stored-unread": 0, "stub": 0, "stubcalls": 0})
        a["rows"] += 1
        a[r["state"]] += 1
        if r["state"] == "stub":
            a["stubcalls"] += r["calls"]
    bagg = table_a(before)[1] if before else None
    lines = ["| Module | Object | Rows | impl | stored-read | stored-unread | stub | script calls to stubs |",
             "|---|---|---|---|---|---|---|---|"]
    order = sorted(agg, key=lambda k: -agg[k]["rows"])
    for k in order:
        a = agg[k]
        b = bagg.get(k) if bagg else None
        if bagg is not None and b == a:
            continue
        cell = lambda f: ("%d→%d" % (b[f], a[f]) if b and b[f] != a[f] else str(a[f])) if bagg is not None else str(a[f])
        lines.append("| %s | %s | %d | %s | %s | %s | %s | %s |" % (k[0], k[1], a["rows"], cell("impl"), cell("stored-read"),
                                                               cell("stored-unread"), cell("stub"), cell("stubcalls")))
    return "\n".join(lines) + "\n", agg


def main():
    ap = common.add_common_args(argparse.ArgumentParser(description=__doc__.split("\n")[0]))
    ap.add_argument("--probe", default=None, help="probe_union.json (default <out>/probe_union.json)")
    ap.add_argument("--calls", default=None, help="calls.json (default <out>/calls.json)")
    ap.add_argument("--before", default=None, help="rows.json of the older run, for the deltas")
    args = ap.parse_args()
    out = common.out_dir(args.out)
    probe = common.load_json(args.probe or os.path.join(out, "probe_union.json"))
    calls = common.load_json(args.calls or os.path.join(out, "calls.json"))["names"]
    rows = classify(args.repo, probe, calls)
    common.save_json(os.path.join(out, "rows.json"), rows)
    s = summary(rows)
    common.save_json(os.path.join(out, "rows_summary.json"), s)
    for k, v in s.items():
        print("%-40s %s" % (k, v))
    with open(os.path.join(out, "tableA.md"), "w") as f:
        f.write(table_a(rows)[0])
    called = sorted([r for r in rows if r["state"] == "stub" and r["calls"]], key=lambda r: -r["calls"])
    with open(os.path.join(out, "stubs_called.md"), "w") as f:
        f.write("| Calls | Row | Module | Constructor |\n|---|---|---|---|\n")
        for r in called:
            f.write("| %d | `%s` | %s | %s |\n" % (r["calls"], r["name"], r["module"], r["constructor"]))
    unread = {}
    for r in rows:
        if r["state"] == "stored-unread":
            unread.setdefault(r["method"].lower(), r)
    with open(os.path.join(out, "unread_called.md"), "w") as f:
        f.write("| Calls | Property | Last segment elsewhere in src |\n|---|---|---|\n")
        for r in sorted(unread.values(), key=lambda r: -r["calls"]):
            f.write("| %d | `%s` | %s |\n" % (r["calls"], r["method"], "yes" if r["last_in_src"] else "no"))
    prefix = {}
    for r in unread.values():
        p = r["method"].split(".")[0] + ".*" if "." in r["method"] else "(no prefix)"
        a = prefix.setdefault(p, [0, 0])
        a[0] += 1
        a[1] += r["calls"]
    common.save_json(os.path.join(out, "unread.json"), {"names": {k: r["calls"] for k, r in unread.items()},
                                                         "by_prefix": prefix})
    if args.before:
        before = common.load_json(args.before)
        bstate = {}
        for r in before:
            bstate[(r["module"], r["name"], r["constructor"])] = r["state"]
        moved = {}
        for r in rows:
            b = bstate.get((r["module"], r["name"], r["constructor"]))
            if b and b != r["state"]:
                moved.setdefault((b, r["state"]), set()).add(r["name"])
        bs = summary(before)
        with open(os.path.join(out, "delta.md"), "w") as f:
            f.write("| Metric | before | after | change |\n|---|---|---|---|\n")
            for k in s:
                if k in bs:
                    f.write("| %s | %s | %s | %+d |\n" % (k, bs[k], s[k], s[k] - bs[k]))
            f.write("\n")
            for (b, a), ns in sorted(moved.items()):
                f.write("**%s → %s (%d names):** %s\n\n" % (b, a, len(ns), ", ".join("`%s`" % n for n in sorted(ns))))
            f.write(table_a(rows, before)[0])
        print("deltas in", os.path.join(out, "delta.md"))


if __name__ == "__main__":
    main()
