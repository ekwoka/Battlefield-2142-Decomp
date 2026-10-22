#!/usr/bin/env python3
"""Maps registered class names to class IDs, create functions and vtables.

Refractor registers every creatable class with the class manager as
(class ID, "dice.hfe.<name>" or "dice.bf.<name>", create function). This finds those triples in a
module's decompiled code and follows each create function (and the
constructor it calls) to the vtables it installs.

usage: class_map.py <module>   -> build/analysis/<module>/class_map.csv
"""
import csv
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = re.compile(r"^// ==== ([0-9a-f]+) (\S+)")
NAME = re.compile(r'\((\w+),"(dice\.(?:hfe|bf)\.[^"]+)"\)')
REGISTER = re.compile(r"\(\s*\w+\s*,\s*(0x[0-9a-f]+|\d+)\s*,\s*(\w+)\s*,\s*&?(?:LAB|FUN)_([0-9a-f]+)\s*\)")
VTABLE = re.compile(r"= &(?:vtbl|PTR_FUN|PTR_LAB)_([0-9a-f]{8})")
CALL = re.compile(r"\bFUN_([0-9a-f]{8})\(")


def load_functions(decomp):
    functions = {}
    for path in sorted(decomp.glob("*.c")):
        address, lines = None, []
        for line in path.read_text(errors="replace").splitlines():
            m = HEADER.match(line)
            if m:
                if address:
                    functions[address] = "\n".join(lines)
                address, lines = m.group(1), []
            elif address:
                lines.append(line)
        if address:
            functions[address] = "\n".join(lines)
    return functions


OBJDUMP = "/opt/homebrew/opt/llvm/bin/llvm-objdump"
GAME = ROOT / "source/Battlefield 2142"


def disassembly_calls(module, address):
    """Call targets and pushed/moved immediates of an uncreated stub function."""
    start = int(address, 16)
    out = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", "--x86-asm-syntax=intel",
                          f"--start-address={start:#x}", f"--stop-address={start + 0x80:#x}", str(GAME / module)],
                         capture_output=True, text=True).stdout
    calls, immediates = [], []
    for line in out.splitlines():
        m = re.search(r"\bcall\s+0x([0-9a-f]+)", line)
        if m:
            calls.append(m.group(1).rjust(8, "0"))
        m = re.search(r"mov\s+dword ptr \[\w+\], 0x([0-9a-f]{8})", line)
        if m:
            immediates.append(m.group(1))
        if "int3" in line:
            break
    return calls, immediates


def vtables_of(functions, address, depth=2, module=None):
    if address not in functions and module:
        calls, immediates = disassembly_calls(module, address)
        if immediates:
            return immediates
        for callee in calls:
            found = vtables_of(functions, callee, depth - 1, module)
            if found:
                return found
        return []
    body = functions.get(address, "")
    found = VTABLE.findall(body)
    if not found and depth > 0:
        for callee in CALL.findall(body)[:6]:
            found = vtables_of(functions, callee, depth - 1, module)
            if found:
                break
    return found


def main():
    module = sys.argv[1]
    analysis = ROOT / "build/analysis" / module
    functions = load_functions(analysis / "decomp")
    rows = []
    for address, body in functions.items():
        pending = {}
        for line in body.splitlines():
            for var, name in NAME.findall(line):
                pending[var] = name
            m = REGISTER.search(line)
            if m and m.group(2) in pending:
                cid = int(m.group(1), 0)
                create = m.group(3)
                rows.append((pending.pop(m.group(2)), f"0x{cid:08x}", create, address,
                             ";".join(dict.fromkeys(vtables_of(functions, create, 3, module)))))
    rows.sort()
    with open(analysis / "class_map.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["name", "class_id", "create_function", "registered_in", "vtables"])
        w.writerows(rows)
    print(f"{len(rows)} classes -> {analysis / 'class_map.csv'}")


if __name__ == "__main__":
    main()
