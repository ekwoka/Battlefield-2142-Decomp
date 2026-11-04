"""Shared paths and helpers for the coverage audit scripts (tools/coverage).

Every script takes --repo (the source tree to measure, default: this one)
and writes into --out (default build/coverage, which git ignores). Run them
with `python3 -I`.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

MODULES = [
    "BF2142.exe",
    "RendDX9_ori.dll",
    "AIDLL.dll",
    "SwiffPlayer.dll",
    "BF2VoipServer.dll",
    "BF2142Audio.dll",
    "Memory.dll",
    "TextureAtlasBuilder.dll",
]
DLLS = [m for m in MODULES if m != "BF2142.exe"]


def game_dir(repo=REPO):
    env = os.environ.get("BF2142_GAME_DIR")
    if env:
        return env
    return os.path.join(repo, "source", "Battlefield 2142")


def mod_dir(repo=REPO):
    return os.path.join(game_dir(repo), "mods", "bf2142")


def analysis_dir(module, repo=REPO):
    """build/analysis/<module> of the main checkout (the analysis isn't
    versioned, so an older worktree uses the main one's)."""
    for root in (repo, REPO):
        path = os.path.join(root, "build", "analysis", module)
        if os.path.isdir(path):
            return path
    return os.path.join(REPO, "build", "analysis", module)


def read_functions(module, repo=REPO, keep_static_init=False):
    """{address int: (name, size)} from functions.csv. One EXE row has an
    unquoted comma in its name, so the name is everything between the first
    field and the last five."""
    path = os.path.join(analysis_dir(module, repo), "functions.csv")
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        next(f)
        for line in f:
            cols = line.rstrip("\n").split(",")
            if len(cols) < 7:
                continue
            name = ",".join(cols[1:-5])
            if not keep_static_init and name.startswith("static_init"):
                continue
            out[int(cols[0], 16)] = (name, int(cols[-5]))
    return out


def out_dir(path):
    os.makedirs(path, exist_ok=True)
    return path


def save_json(path, data):
    with open(path, "w") as f:
        json.dump(data, f, indent=1, sort_keys=True)


def load_json(path):
    with open(path) as f:
        return json.load(f)


def default_out():
    return os.path.join(REPO, "build", "coverage")


def add_common_args(parser):
    parser.add_argument("--repo", default=REPO, help="source tree to measure (default: this checkout)")
    parser.add_argument("--out", default=default_out(), help="output directory (default build/coverage)")
    return parser


def log(*args):
    print(*args, file=sys.stderr)
