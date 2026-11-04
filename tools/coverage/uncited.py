#!/usr/bin/env python3
"""The uncited classifier (uncited.md's `unc/classify.py` + `report.py`).

Every function of BF2142.exe, RendDX9_ori.dll and AIDLL.dll (static_init_*
left out) gets one label, by the first rule that applies:
  1. cited (cite.json; with or without engine.md's appendix, --appendix);
  2. imports and EH thunks -> (a);
  3. console rows (rows.json): each row's constructor and the unique slots of
     the vtables it references take the row's state: impl / stored-read ->
     (c), stub / stored-unread -> (d), or (b) when the name is debug-like.
     template_properties.csv executors are labelled the same way by their
     property's rows;
  4. third-party library signatures in the strings -> (a);
  5. the hand-checked SDK and CRT address ranges of uncited.md's (a) table
     -> (a) (cited functions inside them stay cited);
  6. editor and dev code (uncited.md's (b) ranges: debug drawers, lightmap
     tools, collision debug, profilers; .con writers; debug / profiler /
     editor strings) -> (b);
  7. string matching against src (literals and identifiers; stb_image and
     UI/Avm1 left out): half or more of a function's distinctive strings
     found -> (c), under a quarter -> (d), else "?". In RendDX9, a function
     with under half found but at least half of its strings D3D9-like (a
     word of the shipped .fx shaders, or effect / shader / sampler text) is
     (c), "D3D9 plumbing, substituted" by the rewrite's GLSL pipeline;
  8. the STL length / iterator throwers and their small (<= 512 B)
     string-less callers -> (a);
  9. propagation of (c) and (d) for the rest: callee-of (every caller has one label), then
     vtable siblings (70% agree), then sandwiched between equal labels, then
     the nearest labelled neighbour.
Calibration: propagated (c) is discounted by the precision of propagation
measured when the string-labelled functions are hidden; uncited.md's
factors (EXE 0.84, RendDX9 0.69, AIDLL 0.48) are reused unless
--recalibrate. "?" is split pro rata by (c) / ((c) + (d)).

Output: <out>/<name>.json (per module: the byte buckets, the lower /
calibrated / upper effective coverage, the (d) runs) and labels_<module>.json;
the report on stdout.
"""
import argparse
import bisect
import csv
import os
import re
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402
import extract  # noqa: E402

MODS = ["BF2142.exe", "RendDX9_ori.dll", "AIDLL.dll"]
FACTORS = {"BF2142.exe": 0.84, "RendDX9_ori.dll": 0.69, "AIDLL.dll": 0.48}

LIB_KEYS = [
    # zlib
    "incorrect header check", "invalid distance", "invalid block type", "inflate ", "deflate ", "need dictionary",
    "invalid literal/length", "incompatible version",
    # Speex
    "speex", "libspeex",
    # GameSpy
    "gamespy", "gpcm", "gpsp.", "\\login\\", "\\lc\\", "peerchat", "natneg", "gamestats", "gt2", "gvsource", "gsi",
    "\\challenge\\", "\\newuser\\",
    # HMAC / SHA-1
    "hmac", "sha-1", "sha1",
    # Logitech LCD
    "lglcd", "logitech",
    # FESL / DirtySock / EASTL
    "fesl", "plasma", "prototunnel", "protoudp", "protossl", "protohttp", "protoaries", "dirtysock", "upnp", "eastl",
    "netconn", "dirtyaddr", "sockaddr", "x-ea-", "lobbyapi",
    # PunkBuster
    "punkbuster", "pbcl", "pbsv", "pnkbstr", "evenbalance",
    # TinyXML
    "tixml", "<?xml", "error document empty", "error parsing element",
    # IGA / Radial
    "radial", "iga_", "igaads", "massive",
    # DxDiag
    "dxdiag", "idxdiag",
]
# (start, end) per module: uncited.md's hand-checked statically linked blocks
RANGES_A = {
    "BF2142.exe": [(0x007fecf0, 0x0086c660), (0x0070fa90, 0x00745ea8), (0x008c5d10, 0x008c9eac),
                   (0x00900c20, 0x01000000), (0x00640c10, 0x00642c10)],
    "RendDX9_ori.dll": [(0x10053e80, 0x1005fa50), (0x101aa0c0, 0x10300000), (0x101a2070, 0x101a60b0),
                        (0x10005010, 0x10007010)],
    "AIDLL.dll": [(0x10101300, 0x10300000)],
}
# uncited.md's (b) blocks: debug drawers, the lightmap tools, collision
# debug, the profilers
RANGES_B = {
    "BF2142.exe": [(0x006d6910, 0x006deb65), (0x0065e0f0, 0x0066639e)],
    "RendDX9_ori.dll": [(0x10189c30, 0x101aa0b5), (0x1008232f, 0x100854ea)],
    "AIDLL.dll": [],
}
DEV_KEYS = [
    "debug", "profil", "editor", "editable", "assert", "_debugmesh_", "coll-", "phy-show", "lightmapgen",
    "textureatlas", "%-20s", "pagefaults", "global timer", "ai.draw", "gtsdata", "maxslope", "startcondition:",
    "../rawdata", "_u8888.dds", "-nomipmap", "dumpstats", "memstats", "leak", "dice_textureatlastool",
    "tile_", "rawdata", "c:\\", "tbmtest", "terraineditable", "roadeditable", "lightspleas",
]
DEBUG_NAME_RE = re.compile(r"debug|draw|show|profil|editor|print|dump|stats|leak|lightmap|test", re.I)
CON_WRITER_RE = re.compile(r"^[A-Za-z][A-Za-z0-9]*\.[A-Za-z0-9_.]+ $")
STL_KEYS = ["too long", "invalid map/set", "invalid string position", "invalid vector", "deque<t> too long",
            "list<t> too long", "bad allocation", "invalid iterator"]
STL_CALLER_MAX = 512  # bytes: larger string-less callers are left to propagation
GENERIC = {"bool", "int", "float", "void", "std::string", "unsigned int", "string", "true", "false", "none",
           "vec3", "vec2", "vec4", "dice::hfe::math::vec3", "dice::hfe::math::vec4", "dice::hfe::math::vec2",
           "u32", "u16", "u8", "char", "long", "double", "short", "%s", "%d", "%f", "%i", "\\n", " ", ""}
LIT_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]{3,}")


def norm(s):
    return re.sub(r"[^a-z0-9]+", "_", s.lower()).strip("_")


def src_corpus(repo):
    lits, idents = set(), set()
    for dirpath, dirnames, filenames in os.walk(os.path.join(repo, "src")):
        rel = os.path.relpath(dirpath, repo).replace("\\", "/")
        if rel.startswith("src/UI/Avm1"):
            continue
        for fn in filenames:
            if "stb_image" in fn or not fn.endswith((".cpp", ".h", ".hpp", ".c", ".inl", ".glsl", ".frag", ".vert", ".py")):
                continue
            with open(os.path.join(dirpath, fn), encoding="utf-8", errors="replace") as f:
                t = f.read()
            for m in LIT_RE.finditer(t):
                lits.add(m.group(1).lower())
            idents.update(i.lower() for i in IDENT_RE.findall(t))
    nlits = set(norm(l) for l in lits)
    return lits, nlits, idents


def fx_vocab():
    z = os.path.join(common.mod_dir(common.REPO), "Shaders_client.zip")
    vocab = set()
    if os.path.exists(z):
        with zipfile.ZipFile(z) as zf:
            for i in zf.infolist():
                if i.filename.lower().endswith((".fx", ".dfx", ".mfx")):
                    vocab.update(w.lower() for w in IDENT_RE.findall(zf.read(i).decode("latin-1")))
    return vocab


def clean_strings(strings):
    out = []
    for s in strings:
        if s.startswith("s_"):
            t = s[2:].replace("_", " ").strip()
        else:
            t = s
        tl = t.lower().strip()
        if tl in GENERIC or len(tl) < 4 or not re.search(r"[a-z]", tl):
            continue
        out.append(s)
    return out


def found(s, lits, nlits, idents):
    if s.startswith("s_"):
        n = norm(s[2:])
        return n in nlits or n in idents
    sl = s.lower()
    if sl in lits or sl in idents:
        return True
    n = norm(s)
    if n and n in nlits:
        return True
    # a format string: its longest fixed piece is a literal
    pieces = [p for p in re.split(r"%[-0-9.]*[a-zA-Z]|#[A-Z_]+#", sl) if len(p.strip()) >= 6]
    if pieces and any(p in lits for p in pieces):
        return True
    return False


D3D_KEYS = ["shader", "technique", "effect", ".fx", "ps_", "vs_", "d3d", "#define", "vertexbuffer", "indexbuffer",
            "vertexdecl", "setvdecl", "setss", "setps", "setvs", "sampler", "permutation"]


def d3d_like(s, fxv):
    """A D3D9 technique / parameter name (a word of the shipped .fx shaders)
    or effect-management text."""
    t = s[2:].replace("_", " ") if s.startswith("s_") else s
    tl = t.lower()
    return tl in fxv or any(k in tl for k in D3D_KEYS)


def any_key(strings, keys):
    for s in strings:
        sl = s.lower()
        for k in keys:
            if k in sl:
                return k
    return None


def classify(module, repo, out, cited, rows, corpus, fxv, hide_strings=False):
    funcs = common.read_functions(module, repo)
    feats = extract.load(module, repo, out)
    allf = common.read_functions(module, repo, keep_static_init=True)
    vt = {}
    vpath = os.path.join(common.analysis_dir(module, repo), "vtables.csv")
    if os.path.exists(vpath):
        with open(vpath) as f:
            next(f)
            for line in f:
                v, slot, target = line.strip().split(",")
                vt.setdefault(int(v, 16), []).append(int(target, 16))
    vt_of = {}
    for v, ts in vt.items():
        for t in ts:
            vt_of.setdefault(t, set()).add(v)
    thunk = {}
    with open(os.path.join(common.analysis_dir(module, repo), "functions.csv"), encoding="utf-8", errors="replace") as f:
        next(f)
        for line in f:
            c = line.rstrip("\n").split(",")
            thunk[int(c[0], 16)] = c[-3] == "true"
    lits, nlits, idents = corpus
    label, why = {}, {}

    def put(a, lab, reason):
        if a in funcs and a not in label:
            label[a] = lab
            why[a] = reason

    # 1. cited
    for a in cited:
        put(a, "cited", "cited")
    # 2. imports and EH thunks
    for a, (n, _) in funcs.items():
        if n.startswith(("Unwind@", "Catch@", "__", "_CxxThrow", "_EH")) or (thunk.get(a) and not n.startswith("thunk_FUN_")):
            put(a, "a", "import/EH")
    # 3. console rows and template property executors
    mod_short = {"BF2142.exe": "BF2142.exe", "RendDX9_ori.dll": "RendDX9"}.get(module)
    state_by_method = {}
    for r in rows:
        if r["module"] == "BF2142.exe" and r["object"] == "ObjectTemplate":
            st = r["state"]
            prev = state_by_method.get(r["method"].lower())
            good = st in ("impl", "stored-read")
            state_by_method[r["method"].lower()] = good or bool(prev)
    if mod_short:
        for r in rows:
            if r["module"] != mod_short or not r.get("constructor"):
                continue
            try:
                ctor = int(r["constructor"], 16)
            except ValueError:
                continue
            good = r["state"] in ("impl", "stored-read")
            lab = "c" if good else ("b" if DEBUG_NAME_RE.search(r["name"]) else "d")
            reason = "console row %s (%s)" % (r["name"], r["state"])
            put(ctor, lab, reason)
            f = feats.get("%08x" % ctor)
            if f:
                for v in f["v"]:
                    for t in vt.get(int(v, 16), []):
                        if len(vt_of.get(t, ())) == 1:
                            put(t, lab, reason)
        if module == "BF2142.exe":
            with open(os.path.join(repo, "symbols", "BF2142.exe.template_properties.csv"), newline="") as f:
                for r in csv.DictReader(f):
                    try:
                        ex = int(r["execute"], 16)
                    except (ValueError, KeyError):
                        continue
                    good = state_by_method.get(r["method"].lower())
                    if good is None:
                        continue
                    put(ex, "c" if good else "d", "template property %s" % r["method"])
    strings = {int(k, 16): clean_strings(v["s"]) for k, v in feats.items()}
    rawstrings = {int(k, 16): v["s"] for k, v in feats.items()}
    # 4. library signatures
    for a in funcs:
        k = any_key(rawstrings.get(a, []), LIB_KEYS)
        if k:
            put(a, "a", "library string '%s'" % k)
    # 5. hand-checked ranges
    for lo, hi in RANGES_A.get(module, []):
        for a in funcs:
            if lo <= a < hi:
                put(a, "a", "SDK/CRT range 0x%08x" % lo)
    # 6. editor and dev code
    for lo, hi in RANGES_B.get(module, []):
        for a in funcs:
            if lo <= a < hi:
                put(a, "b", "dev range 0x%08x" % lo)
    for a in funcs:
        ss = rawstrings.get(a, [])
        if any(CON_WRITER_RE.match(s) for s in ss):
            put(a, "b", ".con writer")
            continue
        k = any_key(ss, DEV_KEYS)
        if k:
            put(a, "b", "dev string '%s'" % k)
    # 7. string matching (distinctive: in at most 8 functions of the module)
    freq = {}
    for a, ss in strings.items():
        for s in set(ss):
            freq[s] = freq.get(s, 0) + 1
    string_labelled = set()
    for a in funcs:
        if a in label:
            continue
        ss = [s for s in set(strings.get(a, [])) if freq.get(s, 0) <= 8]
        if not ss:
            continue
        hit = sum(1 for s in ss if found(s, lits, nlits, idents))
        share = hit / len(ss)
        if module == "RendDX9_ori.dll" and share < 0.5 and sum(1 for s in ss if d3d_like(s, fxv)) * 2 >= len(ss):
            put(a, "c", "D3D9 plumbing, substituted")
            string_labelled.add(a)
            continue
        if share >= 0.5:
            put(a, "c", "strings %d/%d in src" % (hit, len(ss)))
            string_labelled.add(a)
        elif share < 0.25:
            put(a, "d", "strings %d/%d in src: %s" % (hit, len(ss), "; ".join(ss[:4])))
            string_labelled.add(a)
        else:
            put(a, "q", "strings %d/%d in src" % (hit, len(ss)))
    # 8. STL throwers and their callers
    throwers = set(a for a in funcs if any_key(rawstrings.get(a, []), STL_KEYS))
    for a in throwers:
        put(a, "a", "STL thrower")
    callers = {}
    for k, v in feats.items():
        a = int(k, 16)
        for c in v["c"]:
            callers.setdefault(int(c, 16), set()).add(a)
    for t in throwers:
        for c in callers.get(t, ()):
            if not strings.get(c) and funcs.get(c, ("", 1 << 30))[1] <= STL_CALLER_MAX:
                put(c, "a", "calls an STL thrower")
    if hide_strings:
        for a in string_labelled:
            del label[a]
            del why[a]
    truth = None
    # 9. propagation
    propagate(funcs, feats, label, why, callers, vt, vt_of, allf)
    return funcs, label, why, string_labelled


def cat(lab):
    if lab in ("cited", "c", "c-p"):
        return "c"
    return lab.split("-")[0]


def propagate(funcs, feats, label, why, callers, vt, vt_of, allf):
    order = sorted(funcs)
    # Only (c) and (d) spread: (a) and (b) come from strings and ranges
    # alone (spreading them over-counts the libraries' neighbours).
    # thunk_FUN_x takes x's label
    for a, (n, _) in funcs.items():
        if a not in label and n.startswith("thunk_FUN_"):
            t = int(n[len("thunk_FUN_"):], 16)
            if t in label and label[t] != "q":
                label[a] = cat(label[t]) + "-p"
                why[a] = "thunk"
    changed = True
    while changed:
        changed = False
        # callee-of: every caller has one label
        for a in order:
            if a in label:
                continue
            cs = [c for c in callers.get(a, ()) if c in funcs and cat(label.get(c, "")) not in ("a", "b")]
            if not cs or any(c not in label or label[c] == "q" for c in cs):
                continue
            cats = set(cat(label[c]) for c in cs)
            if len(cats) == 1:
                label[a] = cats.pop() + "-p"
                why[a] = "callee of %d" % len(cs)
                changed = True
        if changed:
            continue
        # vtable siblings: 70% agree
        for a in order:
            if a in label or a not in vt_of:
                continue
            counts = {}
            for v in vt_of[a]:
                for t in vt[v]:
                    if t != a and t in label and cat(label[t]) in ("c", "d") and t in funcs:
                        k = cat(label[t])
                        counts[k] = counts.get(k, 0) + 1
            tot = sum(counts.values())
            if tot:
                k, n = max(counts.items(), key=lambda x: x[1])
                if n / tot >= 0.7:
                    label[a] = k + "-p"
                    why[a] = "vtable siblings %d/%d" % (n, tot)
                    changed = True
    # sandwiched between equal labels, then the nearest labelled neighbour
    known = [a for a in order if a in label and cat(label[a]) in ("c", "d")]
    for a in order:
        if a in label:
            continue
        i = bisect.bisect_left(known, a)
        prev = known[i - 1] if i > 0 else None
        nxt = known[i] if i < len(known) else None
        if prev is not None and nxt is not None and cat(label[prev]) == cat(label[nxt]):
            label[a] = cat(label[prev]) + "-p"
            why[a] = "sandwiched"
    for a in order:
        if a in label:
            continue
        i = bisect.bisect_left(known, a)
        prev = known[i - 1] if i > 0 else None
        nxt = known[i] if i < len(known) else None
        pick = prev if nxt is None or (prev is not None and a - prev <= nxt - a) else nxt
        if pick is None:
            label[a] = "q"
            why[a] = "nothing near"
        else:
            label[a] = cat(label[pick]) + "-p"
            why[a] = "adjacent"


def buckets(funcs, label):
    b = {}
    for a, (n, s) in funcs.items():
        lab = label.get(a, "q")
        key = {"cited": "cited", "c": "c_ev", "c-p": "c_prop", "d": "d_ev", "d-p": "d_prop",
               "a": "a", "a-p": "a", "b": "b", "b-p": "b", "q": "q"}[lab]
        b[key] = b.get(key, 0) + s
    for k in ("cited", "c_ev", "c_prop", "d_ev", "d_prop", "a", "b", "q"):
        b.setdefault(k, 0)
    return b


def effective(b, total, factor):
    needed = total - b["a"] - b["b"]
    c = b["c_ev"] + b["c_prop"]
    d = b["d_ev"] + b["d_prop"]
    share = c / (c + d) if c + d else 0
    lower = b["cited"] + b["c_ev"]
    cal = lower + factor * b["c_prop"] + b["q"] * share
    upper = lower + b["c_prop"] + b["q"]
    return {"needed": needed, "lower": lower / needed, "calibrated": cal / needed, "upper": upper / needed,
            "cited": b["cited"] / needed if needed else 0, "cited_raw": b["cited"] / total,
            "d_calibrated": needed - cal, "d_raw": d, "q": b["q"]}


def d_runs(funcs, label, why, feats, min_bytes):
    runs, cur = [], None
    for a in sorted(funcs):
        lab = label.get(a, "q")
        if lab in ("d", "d-p"):
            if cur is None:
                cur = [a, a, 0, []]
            cur[1] = a + funcs[a][1]
            cur[2] += funcs[a][1]
            f = feats.get("%08x" % a)
            if lab == "d" and len(cur[3]) < 6:
                cur[3].append("0x%08x %s" % (a, why.get(a, "")[:120]))
        elif lab in ("a", "a-p", "b", "b-p", "q"):
            continue  # transparent
        else:
            if cur:
                runs.append(cur)
            cur = None
    if cur:
        runs.append(cur)
    return [r for r in sorted(runs, key=lambda r: -r[2]) if r[2] >= min_bytes]


def main():
    ap = common.add_common_args(argparse.ArgumentParser(description=__doc__.split("\n")[0]))
    ap.add_argument("--cite", default=None, help="cite.json (default <out>/cite.json)")
    ap.add_argument("--rows", default=None, help="rows.json (default <out>/rows.json)")
    ap.add_argument("--appendix", choices=["with", "without"], default="with")
    ap.add_argument("--name", default=None, help="output stem (default uncited_<appendix>)")
    ap.add_argument("--recalibrate", action="store_true", help="measure the propagation precision instead of reusing uncited.md's factors")
    ap.add_argument("--features", default=None, help="directory holding features_<module>.json (default <out>)")
    args = ap.parse_args()
    out = common.out_dir(args.out)
    fdir = args.features or out
    cite = common.load_json(args.cite or os.path.join(out, "cite.json"))
    rows = common.load_json(args.rows or os.path.join(out, "rows.json"))
    corpus = src_corpus(args.repo)
    fxv = fx_vocab()
    name = args.name or "uncited_" + args.appendix
    result = {}
    tot = {"needed": 0, "lower": 0, "cal": 0, "cited": 0, "d": 0}
    print("%-16s %9s %7s %7s %9s | %6s %6s %10s %6s | %8s" % ("module", "needed", "a KB", "b KB", "cited", "lower", "calib", "(factor)", "upper", "(d) cal"))
    for m in MODS:
        key = "cited_with_appendix" if args.appendix == "with" else "cited_without_appendix"
        cited = set(int(a, 16) for a in cite["modules"][m][key])
        funcs, label, why, sl = classify(m, args.repo, fdir, cited, rows, corpus, fxv)
        total = sum(s for _, s in funcs.values())
        b = buckets(funcs, label)
        factor = FACTORS[m]
        if args.recalibrate:
            _, label_h, _, sl_h = classify(m, args.repo, fdir, cited, rows, corpus, fxv, hide_strings=True)
            pc = sum(funcs[a][1] for a in sl if cat(label_h.get(a, "q")) == "c")
            tp = sum(funcs[a][1] for a in sl if cat(label_h.get(a, "q")) == "c" and cat(label[a]) == "c")
            factor = tp / pc if pc else factor
        e = effective(b, total, factor)
        feats = extract.load(m, args.repo, fdir)
        runs = d_runs(funcs, label, why, feats, 4096)
        result[m] = {"total": total, "buckets": b, "factor": factor, "effective": e,
                     "d_runs": [{"start": "0x%08x" % r[0], "end": "0x%08x" % r[1], "bytes": r[2], "examples": r[3]} for r in runs]}
        common.save_json(os.path.join(out, "labels_%s_%s.json" % (args.appendix, m)),
                         {"0x%08x" % a: [label.get(a, "q"), why.get(a, "")] for a in sorted(funcs)})
        tot["needed"] += e["needed"]
        tot["lower"] += e["lower"] * e["needed"]
        tot["cal"] += e["calibrated"] * e["needed"]
        tot["cited"] += b["cited"]
        tot["d"] += e["d_calibrated"]
        print("%-16s %7.2fMB %7d %7d %8.1f%% | %5.1f%% %5.1f%% %10.2f %5.1f%% | %6.2fMB  (raw d %d KB, ? %d KB, c ev %d KB, c prop %d KB)"
              % (m, e["needed"] / 1e6, b["a"] // 1024, b["b"] // 1024, 100 * b["cited"] / total, 100 * e["lower"],
                 100 * e["calibrated"], factor, 100 * e["upper"], e["d_calibrated"] / 1e6, e["d_raw"] // 1024,
                 e["q"] // 1024, b["c_ev"] // 1024, b["c_prop"] // 1024))
    print("%-16s %7.2fMB %7s %7s %8.1f%% | %5.1f%% %5.1f%% %10s %6s | %6.2fMB" % (
        "all three", tot["needed"] / 1e6, "", "", 100 * tot["cited"] / tot["needed"], 100 * tot["lower"] / tot["needed"],
        100 * tot["cal"] / tot["needed"], "", "", tot["d"] / 1e6))
    print("(the 'cited' column here is of the module's total; 'all three' cited is of the needed code)")
    common.save_json(os.path.join(out, name + ".json"), result)


if __name__ == "__main__":
    main()
