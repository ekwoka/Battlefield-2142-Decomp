#!/usr/bin/env python3
"""Lists functions whose Ghidra decompilation reads ECX/EDX on entry.

Without RTTI or PDBs Ghidra assumes __cdecl, so member functions show their
`this` pointer as `in_ECX` and callers drop it. MSVC uses ECX for `this`
(__thiscall) and ECX+EDX for __fastcall. This scans an ExportDecompiled.java
dump and prints `address,convention` lines for ApplyCallingConventions.java.

usage: find_register_args.py build/analysis/<module>/decomp > conventions.csv
"""
import pathlib
import re
import sys

HEADER = re.compile(r"^// ==== ([0-9a-f]+) (\S+)")
DECL = re.compile(r"^\s+\S.*\b(in_ECX|in_EDX)\b.*;$")


def scan(decomp_dir):
    for path in sorted(pathlib.Path(decomp_dir).glob("*.c")):
        address, regs, in_locals = None, set(), False
        for line in path.read_text(errors="replace").splitlines():
            m = HEADER.match(line)
            if m:
                if address and regs:
                    yield address, regs
                address, regs, in_locals = m.group(1), set(), False
                continue
            if line == "{":
                in_locals = True
            elif in_locals and not line.strip():
                in_locals = False  # locals end at the first blank line
            elif in_locals:
                d = DECL.match(line)
                if d:
                    regs.add(d.group(1))
        if address and regs:
            yield address, regs


def main():
    for address, regs in scan(sys.argv[1]):
        convention = "__fastcall" if "in_EDX" in regs else "__thiscall"
        print(f"{address},{convention}")


if __name__ == "__main__":
    main()
