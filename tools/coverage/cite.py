#!/usr/bin/env python3
"""Address citations per module (coverage-2/3's `cite2.py` + `runs_uncited.py`).

A function counts as cited when its start address (functions.csv in
build/analysis/<module>/, static_init_* left out) appears in src/ or docs/
as `0x...`, a bare 8-digit hex word, or `FUN_`/`LAB_` + 8 hex digits.

The DLLs share the base 0x10000000. An address that starts a function in
more than one DLL goes to the DLL named nearest before it in the text
(within 1,500 characters); failing that, to the DLL the src directory
implies (src/AI and src/Game/Bots* -> AIDLL, src/Render... -> RendDX9,
src/UI -> SwiffPlayer, src/Voip -> BF2VoipServer, src/Sound -> BF2142Audio).
Otherwise it is dropped as ambiguous (with --headings, docs first try the
DLL named in the enclosing "##" heading: an extra rule, not coverage-3's).

With --no-audit-docs, docs/coverage*.md and docs/uncited.md are left out:
the audits' own gap lists cite addresses that nothing implements, and
each new audit adds some (coverage-3's rule, the default, counts them).

docs/engine.md's generated appendix ("## Appendix: where the rewrite reads
the exe's template properties ...", to the end of the file) is a separate
source, so every figure is given with and without it. tests/ is left out
unless --tests.

Output: <out>/cite.json (per module: totals, cited addresses per source
group, the with/without-appendix figures, uncited runs) and a text summary
on stdout.
"""
import argparse
import bisect
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402

ADDR_RE = re.compile(r"(?<![0-9A-Za-z_])(?:0x|FUN_|LAB_)?([0-9a-fA-F]{8})(?![0-9A-Za-z_])|0x([0-9a-fA-F]{5,7})(?![0-9A-Za-z_])")
DLL_NAMES = [
    ("rendDX9", "RendDX9_ori.dll"),
    ("aidll", "AIDLL.dll"),
    ("swiff", "SwiffPlayer.dll"),
    ("bf2voipserver", "BF2VoipServer.dll"),
    ("bf2142audio", "BF2142Audio.dll"),
    ("memory.dll", "Memory.dll"),
    ("textureatlasbuilder", "TextureAtlasBuilder.dll"),
]
DLL_NAME_RE = re.compile("|".join(re.escape(k) for k, _ in DLL_NAMES), re.IGNORECASE)
APPENDIX_HEAD = "## Appendix: where the rewrite reads"
THRESHOLD = {"BF2142.exe": 30 * 1024}


def dir_module(rel):
    """The DLL a src path implies, or None."""
    p = rel.replace("\\", "/")
    if p.startswith("src/AI/") or os.path.basename(p).startswith("Bots") and p.startswith("src/Game/"):
        # src/Game/Bots*.cpp/.h are the bots' half of AIDLL (the game side's
        # adaptor and the AI players), cited with AIDLL addresses
        return "AIDLL.dll"
    for d in ("src/Render/", "src/Scene/", "src/Terrain/", "src/Geometry/", "src/Texture/", "src/Effects/", "src/Movie/"):
        if p.startswith(d):
            return "RendDX9_ori.dll"
    if p.startswith("src/UI/"):
        return "SwiffPlayer.dll"
    if p.startswith("src/Voip/"):
        return "BF2VoipServer.dll"
    if p.startswith("src/Sound/"):
        return "BF2142Audio.dll"
    if p.startswith("src/Memory/"):
        return "Memory.dll"
    if p.startswith("tests/"):
        name = os.path.basename(p).lower()
        if name.startswith(("bots", "ai", "navmesh", "pathfollow")):
            return "AIDLL.dll"
    return None


AUDIT_DOCS = re.compile(r"^docs/(coverage(-\d+)?|uncited)\.md$")


def iter_sources(repo, with_tests, skip_audits=False):
    """(group, relative path, text): group is src, docs, appendix or tests.
    skip_audits leaves out the audits' own reports (docs/coverage*.md,
    docs/uncited.md), whose gap lists cite addresses nothing implements."""
    roots = ["src", "docs"] + (["tests"] if with_tests else [])
    for root in roots:
        base = os.path.join(repo, root)
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames.sort()
            for fn in sorted(filenames):
                path = os.path.join(dirpath, fn)
                rel = os.path.relpath(path, repo)
                if skip_audits and AUDIT_DOCS.match(rel.replace(os.sep, "/")):
                    continue
                if fn.endswith((".pyc", ".png", ".jpg", ".bin", ".spv", ".metallib")):
                    continue
                try:
                    with open(path, encoding="utf-8", errors="replace") as f:
                        text = f.read()
                except OSError:
                    continue
                if rel == os.path.join("docs", "engine.md") and APPENDIX_HEAD in text:
                    cut = text.index(APPENDIX_HEAD)
                    yield root, rel, text[:cut], 0
                    yield "appendix", rel, text[cut:], text[:cut].count("\n")
                else:
                    yield root, rel, text, 0


def main():
    ap = common.add_common_args(argparse.ArgumentParser(description=__doc__.split("\n")[0]))
    ap.add_argument("--tests", action="store_true", help="also count tests/")
    ap.add_argument("--headings", action="store_true",
                    help="extra fallback: in docs, the DLL named in the enclosing '## ' heading (not coverage-3's rule)")
    ap.add_argument("--name", default="cite", help="output file stem (default cite)")
    ap.add_argument("--no-audit-docs", action="store_true",
                    help="leave out docs/coverage*.md and docs/uncited.md (the audits' gap lists cite unimplemented addresses)")
    args = ap.parse_args()
    out = common.out_dir(args.out)

    funcs = {m: common.read_functions(m, args.repo) for m in common.MODULES}
    all_funcs = {m: common.read_functions(m, args.repo, keep_static_init=True) for m in common.MODULES}
    exe = funcs["BF2142.exe"]
    dll_owner = {}
    for m in common.DLLS:
        for a in funcs[m]:
            dll_owner.setdefault(a, []).append(m)

    # cited[module][group] = {addr: first "file:line"}
    cited = {m: {} for m in common.MODULES}
    ambiguous = 0
    ambiguous_examples = []
    for group, rel, text, line0 in iter_sources(args.repo, args.tests, args.no_audit_docs):
        lower_map = {k.lower(): mod for k, mod in DLL_NAMES}
        name_pos, name_mod = [], []
        for mt in DLL_NAME_RE.finditer(text):
            name_pos.append(mt.start())
            name_mod.append(lower_map[mt.group(0).lower()])
        dmod = dir_module(rel)
        heads = []  # (position, DLL named in that "## " heading or None)
        if args.headings and rel.startswith("docs"):
            for hm in re.finditer(r"^##+ .*$", text, re.M):
                nm = DLL_NAME_RE.search(hm.group(0))
                heads.append((hm.start(), lower_map[nm.group(0).lower()] if nm else None))
        head_pos = [p for p, _ in heads]
        for mt in ADDR_RE.finditer(text):
            h = mt.group(1) or mt.group(2)
            a = int(h, 16)
            where = None
            if a in exe:
                mods = ["BF2142.exe"]
            elif a in dll_owner:
                mods = dll_owner[a]
            else:
                continue
            if len(mods) > 1:
                pick = None
                i = bisect.bisect_right(name_pos, mt.start()) - 1
                if i >= 0 and mt.start() - name_pos[i] <= 1500 and name_mod[i] in mods:
                    pick = name_mod[i]
                elif dmod in mods:
                    pick = dmod
                elif heads:
                    j = bisect.bisect_right(head_pos, mt.start()) - 1
                    if j >= 0 and heads[j][1] in mods:
                        pick = heads[j][1]
                if pick is None:
                    ambiguous += 1
                    if len(ambiguous_examples) < 40:
                        ambiguous_examples.append("%s:%d 0x%08x %s" % (rel, line0 + text.count("\n", 0, mt.start()) + 1, a, "/".join(mods)))
                    continue
                mods = [pick]
            m = mods[0]
            g = cited[m].setdefault(group, {})
            if a not in g:
                g[a] = "%s:%d" % (rel, line0 + text.count("\n", 0, mt.start()) + 1)

    result = {"repo": args.repo, "tests": args.tests, "ambiguous": ambiguous,
              "ambiguous_examples": ambiguous_examples, "modules": {}}
    print("ambiguous DLL occurrences dropped: %d" % ambiguous)
    print("%-24s %8s %10s | %-28s | %-28s | %s" % ("module", "fns", "bytes", "cited without appendix", "cited with appendix", "appendix-only"))
    for m in common.MODULES:
        f = funcs[m]
        total_b = sum(s for _, s in f.values())
        groups = cited[m]
        without = set()
        for g, d in groups.items():
            if g != "appendix":
                without |= set(d)
        withapp = without | set(groups.get("appendix", {}))
        b_without = sum(f[a][1] for a in without)
        b_with = sum(f[a][1] for a in withapp)
        only = withapp - without
        # uncited runs over the address-sorted function list (static_init
        # functions are transparent: they neither break nor extend a run)
        runs = []
        cur = None
        for a in sorted(f):
            if a in withapp:
                if cur:
                    runs.append(cur)
                cur = None
                continue
            size = f[a][1]
            if cur is None:
                cur = [a, a + size, size, 1]
            else:
                cur[1] = a + size
                cur[2] += size
                cur[3] += 1
        if cur:
            runs.append(cur)
        thr = THRESHOLD.get(m, 20 * 1024)
        big = sorted([r for r in runs if r[2] >= thr], key=lambda r: -r[2])
        runs_without = []
        cur = None
        for a in sorted(f):
            if a in without:
                if cur:
                    runs_without.append(cur)
                cur = None
                continue
            size = f[a][1]
            if cur is None:
                cur = [a, a + size, size, 1]
            else:
                cur[1] = a + size
                cur[2] += size
                cur[3] += 1
        if cur:
            runs_without.append(cur)
        big_without = sorted([r for r in runs_without if r[2] >= thr], key=lambda r: -r[2])
        result["modules"][m] = {
            "functions": len(f), "bytes": total_b,
            "without_appendix": {"functions": len(without), "bytes": b_without},
            "with_appendix": {"functions": len(withapp), "bytes": b_with},
            "appendix_only": {"functions": len(only), "bytes": sum(f[a][1] for a in only)},
            "by_group": {g: {"functions": len(d), "bytes": sum(f[a][1] for a in d)} for g, d in groups.items()},
            "cited": {"0x%08x" % a: w for g in groups for a, w in groups[g].items()},
            "cited_without_appendix": ["0x%08x" % a for a in sorted(without)],
            "cited_with_appendix": ["0x%08x" % a for a in sorted(withapp)],
            "run_threshold": thr,
            "uncited_runs": [{"start": "0x%08x" % r[0], "end": "0x%08x" % r[1], "bytes": r[2], "functions": r[3]} for r in big],
            "uncited_runs_without_appendix": [{"start": "0x%08x" % r[0], "end": "0x%08x" % r[1], "bytes": r[2], "functions": r[3]} for r in big_without],
        }
        pct = lambda b: 100.0 * b / total_b if total_b else 0.0
        print("%-24s %8d %10d | %5d fns %9d B %5.1f%% | %5d fns %9d B %5.1f%% | %d fns %d B; runs>=%dK: %d / %.2f MB"
              % (m, len(f), total_b, len(without), b_without, pct(b_without), len(withapp), b_with, pct(b_with),
                 len(only), sum(f[a][1] for a in only), thr // 1024, len(big), sum(r[2] for r in big) / 1e6))
    del all_funcs
    common.save_json(os.path.join(out, args.name + ".json"), result)


if __name__ == "__main__":
    main()
