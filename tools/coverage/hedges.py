#!/usr/bin/env python3
"""Hedge notes in docs/engine.md and src/ (coverage-3 section 3's greps).

Lines with "inferred", "approximat", "guess", "stand-in", "not / wasn't /
isn't traced|found|decoded|known", "fitted", "not rewritten", "isn't rewritten",
"until ... decompiled", "Left:", "Not done" are listed as file:line with
the matched term and the line (src/UI/Avm1, stb_image and miniz left out:
their hedges are the libraries'). engine.md's generated appendix is
skipped.

False positives are dropped (--all keeps them, marked): in source files
the terms inside string literals (log and error texts: "Player not
found."); "left:" unless it is a capitalised "Left:" opening a sentence
(not "seconds left:"); "first guess" (the ballistics solver's start);
"isn't / aren't known here / before" and "empty: not found" (runtime
states, not gaps); quoted text (the original's messages); "stand-in" as
the name of a deliberate stub (the inert MenuBindings rows, the local
OpenSpy services, the silent null sound).

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
STRING_RE = re.compile(r'"(?:[^"\\]|\\.)*"')
LEFT_RE = re.compile(r"(?:^|//\s*|\.\s+|\*\*|^\s*-\s*)Left:")
RUNTIME_RE = re.compile(r"(?:isn't|aren't) known (?:here|before)|empty: not found|OpenAL not found", re.I)
QUOTE_RE = re.compile(r'"[^"]*"')
# "Stand-in" as the name of a deliberate stub or null object, not a guess.
STUB_RE = re.compile(r"(?:local|inert|silent) stand-ins?|MenuBindings' stand-ins|interpreter's stand-ins"
                     r"|registered as stand-ins|stand-in for (?:a method not rewritten|RendDX9's renderers)"
                     r"|Stand-in when the game sets no transform", re.I)


def false_positive(rel, line, term):
    """Why a hit isn't a hedge (None when it is one)."""
    code = rel.endswith((".cpp", ".h", ".hpp", ".c", ".inl"))
    if code:
        stripped = line.split("//", 1)[0] if '"' in line.split("//", 1)[0] else ""
        if stripped and TERM_RE.search(STRING_RE.sub("", line)) is None:
            return "string literal"
    if '"' in line and TERM_RE.search(QUOTE_RE.sub("", line)) is None:
        return "quoted text"
    if term.lower().startswith("stand-in") and STUB_RE.search(line):
        return "a stub's name"
    if term.lower() == "left:" and not LEFT_RE.search(line):
        return "prose 'left:'"
    if re.search(r"first guess", line, re.I) and term.lower().startswith("guess"):
        return "solver's first guess"
    if RUNTIME_RE.search(line):
        return "runtime state"
    return None
SKIP = ("src/UI/Avm1", "stb_image", "miniz")
APPENDIX_HEAD = "## Appendix: where the rewrite reads"


def main():
    ap = common.add_common_args(argparse.ArgumentParser(description=__doc__.split("\n")[0]))
    ap.add_argument("--all", action="store_true", help="keep the false positives (marked)")
    args = ap.parse_args()
    out = common.out_dir(args.out)
    hits = []
    dropped = 0
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
                    why = false_positive(rel, line, m.group(0))
                    if why:
                        dropped += 1
                        if not args.all:
                            continue
                    hits.append({"where": "%s:%d" % (rel, n), "term": m.group(0).lower(), "text": line.strip()[:240],
                                 "false_positive": why})
    common.save_json(os.path.join(out, "hedges.json"), hits)
    with open(os.path.join(out, "hedges.txt"), "w") as f:
        for h in hits:
            f.write("%s [%s]%s %s\n" % (h["where"], h["term"], " (false positive: %s)" % h["false_positive"]
                                          if h.get("false_positive") else "", h["text"]))
    by = {}
    for h in hits:
        k = ("engine.md" if h["where"].startswith("docs") else "src", h["term"])
        by[k] = by.get(k, 0) + 1
    print("%d hits (%d false positives %s)" % (len(hits), dropped, "kept, marked" if args.all else "left out"))
    for k, v in sorted(by.items(), key=lambda x: (x[0][0], -x[1])):
        print("  %-9s %-28s %d" % (k[0], k[1], v))


if __name__ == "__main__":
    main()
