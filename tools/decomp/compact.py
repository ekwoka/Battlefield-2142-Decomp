#!/usr/bin/env python3
"""Prints decompiled C from an ExportDecompiled.java dump in compact form.

Drops blank lines and Ghidra warnings and shortens MSVC STL template names,
which roughly halves the text to read. Optionally restricts to an address
range or to named functions.

usage: compact.py <decomp dir> [start-end] [--only addr,addr...] [--skip-lib] [--min-size=N]

--skip-lib drops functions that are evidently inlined MSVC 7.1 runtime/STL
code (iostreams, locales, exception funclets, string concatenation), which a
rewrite replaces with the standard library rather than translating.
"""
import pathlib
import re
import sys

REPLACEMENTS = [
    (r"std::basic_string<char,std::char_traits<char>,std::allocator<char>_>", "std::string"),
    (r"basic_string<char,std::char_traits<char>,std::allocator<char>_>", "std::string"),
    (r"basic_string<char,struct_std::char_traits<char>,class_std::allocator<char>_>", "std::string"),
    (r"std::basic_string<unsigned_short,std::char_traits<unsigned_short>,std::allocator<unsigned_short>_>", "std::wstring"),
    (r"basic_string<unsigned_short,std::char_traits<unsigned_short>,std::allocator<unsigned_short>_>", "std::wstring"),
    (r"std::string::\s*\n\s*", "std::string::"),
    (r"undefined4", "u32"),
    (r"undefined1", "u8"),
    (r"undefined2", "u16"),
    (r"\(undefined \*\)", "(u8*)"),
]
HEADER = re.compile(r"^// ==== ([0-9a-f]+) (\S+) size=(\d+)")
LIBRARY_MARKERS = re.compile(
    r"std::_Lockit|basic_istream|basic_ostream|basic_streambuf|std::_Mutex|std::locale|"
    r"uncaught_exception|ios_base|_Getcat|std::_Xlen|std::_Xran|_Nomemory|bad_alloc|"
    r"__CxxFrameHandler|_CxxThrowException|_Tidy|_Grow|_Copy|_Eos|_Inside|_Chassign|"
    r"basic_string<unsigned_short")


def is_library(name, body):
    if name.startswith(("Catch@", "Unwind@", "__", "_")):
        return True
    return bool(LIBRARY_MARKERS.search(body))


def main():
    decomp = pathlib.Path(sys.argv[1])
    lo, hi = 0, 1 << 32
    only = None
    for arg in sys.argv[2:]:
        if arg.startswith("--"):
            continue
        if "-" in arg:
            a, b = arg.split("-")
            lo, hi = int(a, 16), int(b, 16)
        elif sys.argv[sys.argv.index(arg) - 1] == "--only":
            only = {int(x, 16) for x in arg.split(",")}
    skip_lib = "--skip-lib" in sys.argv
    min_size = 0
    for arg in sys.argv:
        if arg.startswith("--min-size="):
            min_size = int(arg.split("=", 1)[1])
    functions = []
    for path in sorted(decomp.glob("*.c")):
        current = None
        for line in path.read_text(errors="replace").splitlines():
            m = HEADER.match(line)
            if m:
                a = int(m.group(1), 16)
                wanted = lo <= a < hi and (only is None or a in only) and int(m.group(3)) >= min_size
                current = [m.group(2), []] if wanted else None
                if current:
                    functions.append(current)
            if current is None or not line.strip():
                continue
            if "/* WARNING" in line or line.strip().startswith("/* 0x"):
                continue
            current[1].append(line)
    out = []
    skipped = []
    for name, lines in functions:
        body = "\n".join(lines)
        if skip_lib and is_library(name, body):
            skipped.append(name)
            continue
        out.append(body)
    if skipped:
        out.append(f"// skipped {len(skipped)} library functions")
    text = "\n".join(out)
    for pattern, repl in REPLACEMENTS:
        text = re.sub(pattern, repl, text)
    print(text)


if __name__ == "__main__":
    main()
