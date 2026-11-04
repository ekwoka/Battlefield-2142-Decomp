#!/usr/bin/env python3
"""Hedge notes in docs/engine.md and src/ (coverage-3 section 3's greps).

Lines with "inferred", "approximat", "guess", "stand-in", "not / wasn't /
isn't traced|found|decoded|known", "fitted", "not rewritten", "isn't rewritten",
"until ... decompiled", "Left:", "Not done" are listed as file:line with
the matched term and the line (src/UI/Avm1, stb_image and miniz left out:
their hedges are the libraries'). engine.md's generated appendix is
skipped.

Output: <out>/hedges.txt (all hits) and hedges.json; counts on stdout.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402

TERMS = [r"inferred", r"approximat\w*", r"\bguess\w*", r"stand-in",
         r"(?:not|wasn't|weren't|isn't|aren't) (?:traced|found|decoded|known)",
         r"\bfitted\b", r"not rewritten", r"isn't rewritten", r"aren't rewritten", r"until [^.]{0,60}decompiled",
         r"\bLeft:", r"Not done", r"not modelled"]
TERM_RE = re.compile("|".join("(%s)" % t for t in TERMS), re.I)
SKIP = ("src/UI/Avm1", "stb_image", "miniz")
APPENDIX_HEAD = "## Appendix: where the rewrite reads"


def main():
    ap = common.add_common_args(argparse.ArgumentParser(description=__doc__.split("\n")[0]))
    args = ap.parse_args()
    out = common.out_dir(args.out)
    hits = []
    files = [os.path.join(args.repo, "docs", "engine.md")]
    for dirpath, dirnames, filenames in os.walk(os.path.join(args.repo, "src")):
        for fn in sorted(filenames):
            if fn.endswith((".cpp", ".h", ".hpp", ".c", ".inl")):
                files.append(os.path.join(dirpath, fn))
    for path in files:
        rel = os.path.relpath(path, args.repo)
        if any(s in rel for s in SKIP):
            continue
        with open(path, encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                if line.startswith(APPENDIX_HEAD):
                    break
                m = TERM_RE.search(line)
                if m:
                    hits.append({"where": "%s:%d" % (rel, n), "term": m.group(0).lower(), "text": line.strip()[:240]})
    common.save_json(os.path.join(out, "hedges.json"), hits)
    with open(os.path.join(out, "hedges.txt"), "w") as f:
        for h in hits:
            f.write("%s [%s] %s\n" % (h["where"], h["term"], h["text"]))
    by = {}
    for h in hits:
        k = ("engine.md" if h["where"].startswith("docs") else "src", h["term"])
        by[k] = by.get(k, 0) + 1
    print("%d hits" % len(hits))
    for k, v in sorted(by.items(), key=lambda x: (x[0][0], -x[1])):
        print("  %-9s %-28s %d" % (k[0], k[1], v))


if __name__ == "__main__":
    main()
