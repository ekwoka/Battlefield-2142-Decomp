#!/usr/bin/env python3
"""Lists the console constants a module registers (c_ETCar = 2, ...).

The registration code builds the name as a std::string and passes it to the
constant registration function with the value:

    basic_string(local_20,"c_ETCar");
    FUN_004da5fd(local_20,2);

Float constants go through another function that takes only the value
(the name is the string built just before):

    basic_string(abStack_3c,"c_NIGhostAlways");
    FUN_00669489(0xbf800000);

usage: console_constants.py <module> [--register=FUN_004da5fd] [--register-float=FUN_00669489]
       -> symbols/<module>.constants.csv (name,value)
"""
import pathlib
import re
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 1:
        sys.exit(__doc__)
    module = args[0]
    register = "FUN_004da5fd"
    register_float = "FUN_00669489"
    for a in sys.argv[1:]:
        if a.startswith("--register="):
            register = a.split("=", 1)[1]
        elif a.startswith("--register-float="):
            register_float = a.split("=", 1)[1]
    float_re = re.compile(re.escape(register_float) + r"\((0x[0-9a-f]+)\);")
    name_re = re.compile(r'\((\w+),"(c_\w+)"\);')
    call_re = re.compile(re.escape(register) + r"\((\w+),(0x[0-9a-f]+|-?\d+)\);")
    constants = {}
    for path in sorted((ROOT / "build" / "analysis" / module / "decomp").glob("*.c")):
        held = {}
        last = None
        for line in path.read_text(errors="replace").splitlines():
            m = name_re.search(line)
            if m:
                held[m.group(1)] = m.group(2)
                last = m.group(2)
                continue
            m = float_re.search(line)
            if m and last:
                value = struct.unpack("<f", struct.pack("<I", int(m.group(1), 16)))[0]
                constants.setdefault(last, f"{value:g}")
                continue
            m = call_re.search(line)
            if m and m.group(1) in held:
                constants.setdefault(held[m.group(1)], int(m.group(2), 0))
    out = ROOT / "symbols" / f"{module}.constants.csv"
    with out.open("w") as f:
        f.write("name,value\n")
        for name in sorted(constants, key=str.lower):
            f.write(f"{name},{constants[name]}\n")
    print(f"{len(constants)} constants -> {out}")


if __name__ == "__main__":
    main()
