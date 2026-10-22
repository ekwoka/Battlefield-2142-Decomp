#!/usr/bin/env python3
"""Prints the decompiled body of every slot of a vtable, in slot order.

usage: vtable_methods.py <module> <vtable address> [--skip-lib]
Reads build/analysis/<module>/vtables.csv and the decomp dump.
"""
import csv
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    module, vtable = sys.argv[1], sys.argv[2].lower().lstrip("0x")
    analysis = ROOT / "build/analysis" / module
    slots = []
    with open(analysis / "vtables.csv") as f:
        for row in csv.DictReader(f):
            if row["vtable"].lower().lstrip("0") == vtable.lstrip("0"):
                slots.append((int(row["slot"]), row["target"]))
    slots.sort()
    for slot, target in slots:
        print(f"//// slot {slot} (+0x{slot * 4:x}) -> {target}")
        out = subprocess.run(
            [sys.executable, "-I", str(ROOT / "tools/decomp/compact.py"), str(analysis / "decomp"), "--only", target],
            capture_output=True, text=True).stdout
        # Bodies shared by several slots are printed once.
        print(out.strip() if out.strip() else "// (no body)")


if __name__ == "__main__":
    main()
