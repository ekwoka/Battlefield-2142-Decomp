#!/usr/bin/env python3
"""Lists the global singletons a module registers by name.

Refractor modules publish their managers ("ObjectManager", "PortalManager",
...) as global pointers filled in when the class manager creates them. The
registration functions build the name as a std::string and pass the global's
address with the interface and class IDs, either as a struct on the stack

    basic_string(local_20,"PortalManager");
    local_30 = &DAT_00a88390;  local_2c = <interface id>;  local_28 = <class id>;

or as call arguments

    basic_string(local_20,"ObjectManager");
    FUN_00402da0(local_4c,local_20,&DAT_00a85220,0xc35a,0xc35c,1);

usage: singletons.py <module>  -> symbols/<module>.singletons.csv
                                  (global,name,interface id,class id,class,function)
The class column names the class ID from build/analysis/<module>/class_map.csv
(class_map.py), which also catches names this script attributes wrongly when a
function reuses one string for several registrations.
"""
import csv
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = re.compile(r"^// ==== ([0-9a-f]+) (\S+)")
NAME = re.compile(r'basic_string<[^;]*?\((\w+),"([A-Za-z_][\w.]*)"\);')
STRUCT_GLOBAL = re.compile(r"(\w+) = &(?:DAT|PTR)_([0-9a-f]{8});")
ASSIGN = re.compile(r"(\w+) = (0x[0-9a-f]+|\d+);")
CALL_GLOBAL = re.compile(r"\(\w+,(\w+),&(?:DAT|PTR)_([0-9a-f]{8}),(0x[0-9a-f]+|\d+),(0x[0-9a-f]+|\d+),\d+\)")


def number(text):
    return int(text, 0)


def scan(module):
    decomp = ROOT / "build" / "analysis" / module / "decomp"
    rows = {}
    for path in sorted(decomp.glob("*.c")):
        function, body = None, []

        def flush():
            if not function:
                return
            text = "\n".join(body)
            # The name is what the string variable held last before the call.
            strings = [(m.start(), m.group(1), m.group(2)) for m in NAME.finditer(text)]
            for m in CALL_GLOBAL.finditer(text):
                held = [name for pos, var, name in strings if pos < m.start() and var == m.group(1)]
                if held:
                    rows.setdefault(m.group(2), (held[-1], number(m.group(3)), number(m.group(4)), function))
            # Struct form: the global's address followed by two IDs.
            lines = body
            for i, line in enumerate(lines):
                g = STRUCT_GLOBAL.search(line)
                if not g:
                    continue
                ids = []
                for follow in lines[i + 1:i + 4]:
                    a = ASSIGN.search(follow)
                    if a:
                        ids.append(number(a.group(2)))
                if len(ids) < 2:
                    continue
                # The name is the last string built before this line.
                before = "\n".join(lines[max(0, i - 12):i])
                found = NAME.findall(before)
                if found:
                    rows.setdefault(g.group(2), (found[-1][1], ids[0], ids[1], function))

        for line in path.read_text(errors="replace").splitlines():
            m = HEADER.match(line)
            if m:
                flush()
                function, body = m.group(1), []
            else:
                body.append(line)
        flush()
    return rows


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    module = sys.argv[1]
    rows = scan(module)
    classes = {}
    class_map = ROOT / "build" / "analysis" / module / "class_map.csv"
    if class_map.exists():
        for row in csv.reader(class_map.open()):
            if len(row) > 1 and row[1].startswith("0x"):
                classes[int(row[1], 16)] = row[0]
    out = ROOT / "symbols" / f"{module}.singletons.csv"
    with out.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["global", "name", "interface_id", "class_id", "class", "registered_in"])
        for address in sorted(rows):
            name, interface, cls, function = rows[address]
            writer.writerow([f"0x{address}", name, f"0x{interface:x}", f"0x{cls:x}", classes.get(cls, ""),
                f"0x{function}"])
    print(f"{len(rows)} singletons -> {out}")


if __name__ == "__main__":
    main()
