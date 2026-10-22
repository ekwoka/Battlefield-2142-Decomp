#!/usr/bin/env python3
"""Lists the console methods registered in BF2142.exe, with their signatures.

Every console method ("ObjectTemplate.setNetworkableInfo" etc.) is a global
object built by a static initializer. The initializer first calls the
"begin object" function (0x00666110 in 1.51) with the object name and access
flags, then runs one constructor per method. A constructor stores:

    +0x04 kind        0 method, 1 read/write, 2 read-only, 3 write-only property
    +0x08 object name (copied from the begin-object globals)
    +0x0c method name
    +0x18 / +0x1c     minimum / maximum argument count
    +0x4c             return type name, e.g. "std::string"
    +0x50...          argument type names

This script reads the Ghidra decompiler dump (ExportDecompiled.java output,
after CreateStaticInitializers.java has run) and prints a CSV:

    object,method,kind,access,min,max,return,args,constructor,registry

`registry` is the method list the constructor adds the method to. Each
source module has one; its init function registers the list with the
console and, for object templates, the template types the module defines.
With --types=<csv>, those are written as registry,type,classId. (One type
is missed: the sound module registers "Sound" with ID 0x985fd1, which Ghidra
prints as a string pointer; symbols/BF2142.exe.templates.csv lists it by hand.) A console
lookup picks the first method of a name whose isActive() holds, e.g. the
one for the active template's type.

usage: console_methods.py <module dir, e.g. build/analysis/BF2142.exe> <BF2142.exe>
                          [--begin=00666110] [--types=out.csv]
"""
import csv
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from pe import PE  # noqa: E402

HEADER = re.compile(r"^// ==== ([0-9a-f]+) (\S+) size=(\d+)", re.M)
FIELD = re.compile(r"\(int\)this \+ (0x[0-9a-f]+|\d+)\) = (.+?);")
KINDS = {0: "method", 1: "property", 2: "readonly", 3: "writeonly"}


def functions(decomp_dir):
    for path in sorted(pathlib.Path(decomp_dir).glob("*.c")):
        text = path.read_text(errors="replace")
        heads = list(HEADER.finditer(text))
        for i, m in enumerate(heads):
            end = heads[i + 1].start() if i + 1 < len(heads) else len(text)
            yield m.group(1), m.group(2), text[m.start():end]


def value(pe, expr):
    """A string literal, &DAT_xxx string, or integer from a decompiled store."""
    expr = expr.strip()
    m = re.search(r'"((?:[^"\\]|\\.)*)"', expr)
    if m:
        return m.group(1)
    m = re.search(r"(?:DAT|vtbl|PTR_DAT)_([0-9a-f]{8})", expr)
    if m:
        return pe.cstr(int(m.group(1), 16)) or ""
    m = re.fullmatch(r"(?:\([^)]*\))?\s*(-?(?:0x[0-9a-f]+|\d+))", expr)
    if m:
        return int(m.group(1), 0)
    return None


REGISTER = re.compile(r"\+ 0xa0\) = 0;.*?\b(FUN_[0-9a-f]{8})\(\);", re.S)
LIST_ADD = re.compile(r"FUN_[0-9a-f]{8}\(DAT_([0-9a-f]{8}),&stack")
MODULE_INIT = re.compile(r"\(PTR_DAT_[0-9a-f]{8},&DAT_([0-9a-f]{8})\);")
TYPE_NAME = re.compile(r'\(local_\w+,"([^"]+)"\)\s*;\s*\(\*\*\(code \*\*\)\(\*DAT_[0-9a-f]{8} \+ 0x14\)\)\((0x[0-9a-f]+|\d+),local_\w+\);')


def parse_constructor(pe, body):
    fields = {}
    for m in FIELD.finditer(body):
        fields[int(m.group(1), 0)] = m.group(2)
    if 0xC not in fields or 0x18 not in fields or 0x1C not in fields or 0x4C not in fields:
        return None
    # Short names can be mislabelled by Ghidra (e.g. "set" as vtbl_009194f4).
    name = value(pe, fields[0xC])
    if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z_][\w.]*", name):
        return None
    args = []
    offset = 0x50
    while offset in fields:
        args.append(value(pe, fields[offset]) or "?")
        offset += 4
    kind = value(pe, fields.get(4, "0"))
    register = REGISTER.search(body)
    return {
        "register": register.group(1) if register else None,
        "method": name,
        "kind": KINDS.get(kind, str(kind)) if isinstance(kind, int) else "?",
        "min": value(pe, fields[0x18]),
        "max": value(pe, fields[0x1C]),
        "return": value(pe, fields[0x4C]) or "?",
        "args": args,
    }


CALL = re.compile(r"\b(FUN_[0-9a-f]{8}|thunk_FUN_[0-9a-f]{8})\(([^;]*)\);")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    module_dir = pathlib.Path(sys.argv[1])
    pe = PE(sys.argv[2])
    begin = "FUN_00666110"
    types_out = None
    for arg in sys.argv[3:]:
        if arg.startswith("--begin="):
            begin = "FUN_" + arg.split("=", 1)[1]
        elif arg.startswith("--types="):
            types_out = arg.split("=", 1)[1]

    decomp = module_dir / "decomp"
    constructors = {}
    initializers = []
    list_of = {}  # registration function -> method list global
    types = []    # (method list, type name, class id)
    for address, name, body in functions(decomp):
        method = parse_constructor(pe, body)
        if method:
            constructors["FUN_" + address] = dict(method, constructor=address)
        if name.startswith("static_init_") or begin + "(" in body:
            initializers.append((address, body))
        if len(body) < 400:
            m = LIST_ADD.search(body)
            if m:
                list_of["FUN_" + address] = m.group(1)
        m = MODULE_INIT.search(body)
        if m:
            # The list object; its node pointer (registry) is 4 bytes in.
            registry = "%08x" % (int(m.group(1), 16) + 4)
            for t in TYPE_NAME.finditer(body):
                types.append((registry, t.group(1), "0x%x" % int(t.group(2), 0)))
    for method in constructors.values():
        method["registry"] = list_of.get(method["register"], "")

    # Walk the initializers in table order: a begin-object call sets the
    # object for the constructors that follow it.
    # Each initializer is a separate function, so the object carries over.
    rows = []
    seen = set()
    current = None
    for address, body in sorted(initializers):
        for m in CALL.finditer(body):
            callee = m.group(1).replace("thunk_", "")
            if callee == begin:
                parts = [p.strip() for p in m.group(2).split(",")]
                current = (value(pe, parts[0]) if parts else None, value(pe, parts[1]) if len(parts) > 1 else None)
            elif callee in constructors and current and current[0] and callee not in seen:
                method = constructors[callee]
                seen.add(callee)
                rows.append([current[0], method["method"], method["kind"], current[1], method["min"], method["max"],
                    method["return"], " ".join(method["args"]), method["constructor"], method["registry"]])
    for callee, method in constructors.items():
        if callee not in seen:
            rows.append(["?", method["method"], method["kind"], "", method["min"], method["max"], method["return"],
                " ".join(method["args"]), method["constructor"], method["registry"]])

    out = csv.writer(sys.stdout, lineterminator="\n")
    out.writerow(["object", "method", "kind", "access", "min", "max", "return", "args", "constructor", "registry"])
    out.writerows(rows)
    if types_out:
        with open(types_out, "w", newline="") as f:
            w = csv.writer(f, lineterminator="\n")
            w.writerow(["registry", "type", "class_id"])
            w.writerows(sorted(types))
    print(f"{len(rows)} methods, {len(seen)} with a known object", file=sys.stderr)


if __name__ == "__main__":
    main()
