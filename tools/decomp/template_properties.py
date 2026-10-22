#!/usr/bin/env python3
"""Maps ObjectTemplate console properties to template classes and fields.

An "ObjectTemplate.<name>" method's execute function (descriptor vtable slot
0x5c) casts the active template to a template class and reads or writes a
field:

    mov ecx, [IID global]      ; the class ID, e.g. 0x947e (SimpleObject)
    call activeTemplateAs      ; 0x006f62f0 in 1.51: active->queryInterface(ecx)
    ... [eax + 0x94] ...       ; the field

Some properties go through a helper that tries several classes in turn.
Dotted properties ("armor.maxHitPoints") go through a component: a helper
(0x006f1e90) called with the component ID in ECX casts the template to the
component container (0x946f) and calls getComponent(id); the property then
uses the component's virtual getter/setter. This script records, per
method, the class IDs (with names from the templates table), the component
IDs, and the field offsets the decompiled code touches on the cast result.

usage: template_properties.py <module dir> <BF2142.exe> <console.csv> <templates.csv>
       [--accessor=006f62f0] > template_properties.csv
"""
import csv
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from pe import PE  # noqa: E402

HEADER = re.compile(r"^// ==== ([0-9a-f]+) (\S+) size=(\d+)", re.M)


def load_functions(decomp_dir):
    funcs = {}
    for path in sorted(pathlib.Path(decomp_dir).glob("*.c")):
        text = path.read_text(errors="replace")
        heads = list(HEADER.finditer(text))
        for i, m in enumerate(heads):
            end = heads[i + 1].start() if i + 1 < len(heads) else len(text)
            funcs[int(m.group(1), 16)] = (int(m.group(3)), text[m.start():end])
    return funcs


def calls(pe, start, size):
    """(target, ecx global or None) for each `call rel32` in [start, start+size)."""
    o = pe.offset(start)
    code = pe.data[o:o + size]
    found = []
    for i in range(len(code) - 4):
        if code[i] != 0xE8:
            continue
        target = start + i + 5 + struct.unpack_from("<i", code, i + 1)[0]
        ecx = None
        # mov ecx, [imm32] (8b 0d) or mov ecx, imm32 (b9) shortly before.
        for back in range(6, 16):
            j = i - back
            if j < 0:
                break
            if code[j] == 0x8B and code[j + 1] == 0x0D:
                ecx = pe.u32(struct.unpack_from("<I", code, j + 2)[0])
                break
            if code[j] == 0xB9:
                ecx = struct.unpack_from("<I", code, j + 1)[0]
                break
        found.append((target, ecx))
    return found


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) < 4:
        sys.exit(__doc__)
    accessor = 0x006F62F0
    for a in sys.argv[1:]:
        if a.startswith("--accessor="):
            accessor = int(a.split("=", 1)[1], 16)
    module_dir, exe, console_csv, templates_csv = args
    pe = PE(exe)
    funcs = load_functions(pathlib.Path(module_dir) / "decomp")
    class_names = {int(r["class_id"], 16): r["type"] for r in csv.DictReader(open(templates_csv))}

    components = []

    def class_ids(address, depth=0):
        """Class IDs passed to the accessor from `address` or its helpers."""
        if address not in funcs or depth > 2:
            return []
        size, _ = funcs[address]
        ids = []
        for target, ecx in calls(pe, address, size):
            if target == accessor and ecx is not None:
                ids.append(ecx)
            elif target != accessor and target in funcs and funcs[target][0] < 200:
                inner = class_ids(target, depth + 1)
                if ecx is not None and depth == 0 and inner:
                    components.append(ecx)  # helper taking a component ID
                ids += inner
        return ids

    out = csv.writer(sys.stdout, lineterminator="\n")
    out.writerow(["method", "kind", "type", "classes", "components", "offsets", "execute"])
    for row in csv.DictReader(open(console_csv)):
        if row["object"] != "ObjectTemplate":
            continue
        ctor = funcs.get(int(row["constructor"], 16))
        m = re.search(r"= &vtbl_([0-9a-f]+);", ctor[1]) if ctor else None
        if not m:
            continue
        execute = pe.u32(int(m.group(1), 16) + 0x5C)
        components.clear()
        ids = class_ids(execute)
        names = []
        for i in dict.fromkeys(ids):
            names.append(class_names.get(i, "0x%x" % i))
        body = funcs.get(execute, (0, ""))[1]
        offsets = sorted(set(re.findall(r"\((?:i|pi|pu|pp|pf)Var\d+ \+ (0x[0-9a-f]+)\)", body)), key=lambda x: int(x, 16))
        out.writerow([row["method"], row["kind"], row["return"], " ".join(names),
            " ".join("0x%x" % c for c in dict.fromkeys(components)), " ".join(offsets), "%08x" % execute])


if __name__ == "__main__":
    main()
