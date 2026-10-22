#!/usr/bin/env python3
"""Dumps a vtable: one line per slot with offset, target and Ghidra name.

usage: vtable.py <module> <vtable address> [max slots]
  module is a file name in the game dir, e.g. BF2142Audio.dll
"""
import csv
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from pe import PE  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    module, address = sys.argv[1], int(sys.argv[2], 16)
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 512
    pe = PE(ROOT / "source/Battlefield 2142" / module)
    names = {}
    inventory = ROOT / "build/analysis" / module / "functions.csv"
    if inventory.exists():
        with open(inventory) as f:
            for row in csv.DictReader(f):
                names[int(row["address"], 16)] = (row["name"], row["size"])
    for slot in range(limit):
        target = pe.u32(address + 4 * slot)
        if target is None or not pe.is_code(target):
            break
        name, size = names.get(target, ("?", "?"))
        print(f"+0x{4 * slot:03x} {target:08x} {name} size={size}")


if __name__ == "__main__":
    main()
