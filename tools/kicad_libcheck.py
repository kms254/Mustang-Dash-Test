#!/usr/bin/env python3
"""Assert that every KiCad library this project declares can actually be loaded.

    python tools/kicad_libcheck.py kicad/board3

Why this exists
---------------
On 2026-08-04 `ProPrj_New-easyedapro.kicad_sym` was found to be unloadable, and
had been for months. One symbol in it was named

    CAPACITOR_THT:CP_RADIAL_D8.0MM_P2.50MM

and a colon is the library/symbol separator in a KiCad identifier, so KiCad
rejected the *whole* library -- all 93 other symbols with it.

Nothing caught it, and the reasons are worth stating because they are what this
script is shaped around:

  * Every symbol renders from the schematic's OWN `lib_symbols` cache, so the
    sheet drew correctly, the netlist was right, the BOM was right, and DRC was
    0 violations / 0 unconnected / 0 parity the entire time.
  * ERC did complain -- 124 `lib_symbol_issues` -- but that check is a WARNING,
    and the CI gate runs `--severity-error`. It was invisible by construction.
  * The warning text is "library not found" at a path where the file plainly
    exists. "Not found" there means *failed to load*, which reads like a stale
    path and sends you looking in the wrong place.

It surfaced only when someone tried GUI Change Symbol and got
"*** symbol not found ***" for a part changing to the symbol it already used.

So the check has to load the libraries, not read the schematic.

What it checks
--------------
1. Every library named in `sym-lib-table` / `fp-lib-table` resolves to a path
   that exists (`${KIPRJMOD}` expanded to the project directory).
2. Every SYMBOL library actually loads, probed through `kicad-cli`. Note that
   command exits 0 whether or not the load succeeded, so the stdout text is the
   result -- never `$?`.
3. No symbol or footprint name contains a colon. This is the preventive half:
   it names the offending symbol directly instead of making you bisect a
   400 KB library to find out why it will not load.
4. Every library nickname referenced by the schematic or the board is actually
   registered in the corresponding table. (`Board3:TC2030-IDC-NL` was in use
   while `Board3` was in neither table.)

Exit codes: 0 clean, 1 something is wrong.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

OK, FAIL = 0, 1

# A colon separates nickname from item in a LIB_ID, so it can never appear in
# either half. Everything else EasyEDA emits -- Chinese characters, "/", "*",
# spaces, "%" -- has been verified harmless on this project's libraries.
ILLEGAL_IN_NAME = ":"


def kicad_cli() -> str:
    for c in (r"C:/Program Files/KiCad/10.0/bin/kicad-cli.exe", "kicad-cli"):
        try:
            subprocess.run([c, "version"], capture_output=True, timeout=30)
            return c
        except (OSError, subprocess.SubprocessError):
            continue
    raise SystemExit("kicad-cli not found")


def read_table(path: Path) -> list[tuple[str, str]]:
    """[(nickname, uri), ...] from a *-lib-table."""
    if not path.is_file():
        return []
    text = path.read_text(encoding="utf-8")
    return re.findall(r'\(name "([^"]+)"\).*?\(uri "([^"]+)"\)', text, re.S | re.M) or \
        re.findall(r'\(name "([^"]+)"\)\s*\(type "[^"]*"\)\s*\(uri "([^"]+)"\)', text)


def symbol_names(lib: Path) -> list[str]:
    """Top-level symbol names, by s-expression DEPTH -- not by indentation.

    Do not match on leading whitespace here. This project's two symbol
    libraries disagree about it: the EasyEDA export indents with tabs, the
    kicad_lcsc importer with two spaces. An `^\\t\\(symbol` pattern silently
    reports the second library as empty, and a bare `\\(symbol` pattern counts
    the nested `NAME_1_0` body sub-symbols as real parts. Depth 1 is the only
    thing that means "a symbol in this library".
    """
    if not lib.is_file():
        return []
    text = lib.read_text(encoding="utf-8")
    names, depth, i, instr = [], 0, 0, False
    while i < len(text):
        c = text[i]
        if instr:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                instr = False
        elif c == '"':
            instr = True
        elif c == "(":
            depth += 1
            if depth == 2:  # 1 is (kicad_symbol_lib ...)
                m = re.match(r'\(symbol "([^"]+)"', text[i:i + 400])
                if m:
                    names.append(m.group(1))
        elif c == ")":
            depth -= 1
        i += 1
    return names


def footprint_names(pretty: Path) -> list[str]:
    if not pretty.is_dir():
        return []
    return [p.stem for p in pretty.glob("*.kicad_mod")]


def library_loads(cli: str, lib: Path, probe: str) -> tuple[bool, str]:
    """kicad-cli exits 0 either way -- the stdout text is the result."""
    r = subprocess.run(
        [cli, "sym", "export", "svg", "--symbol", probe, "-o",
         str(Path(lib).parent / "__libcheck_tmp__"), str(lib)],
        capture_output=True, text=True, timeout=180)
    out = (r.stdout or "") + (r.stderr or "")
    tmp = Path(lib).parent / "__libcheck_tmp__"
    if tmp.is_dir():
        for f in tmp.iterdir():
            f.unlink()
        tmp.rmdir()
    return ("Unable to load library" not in out), out.strip().splitlines()[:1]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("project", help="KiCad project directory")
    args = ap.parse_args()
    root = Path(args.project)
    if not root.is_dir():
        root = root.parent
    cli = kicad_cli()
    problems: list[str] = []

    print(f"project: {root}")

    # ---- 1 & 2: declared libraries exist and load -------------------------
    sym_tbl = read_table(root / "sym-lib-table")
    fp_tbl = read_table(root / "fp-lib-table")
    print(f"\nsymbol libraries declared   : {len(sym_tbl)}")
    for nick, uri in sym_tbl:
        path = Path(uri.replace("${KIPRJMOD}", str(root)))
        if not path.is_file():
            problems.append(f"symbol library {nick!r}: no file at {path}")
            print(f"  {nick:28s} MISSING  {path}")
            continue
        names = symbol_names(path)
        if not names:
            problems.append(f"symbol library {nick!r}: no symbols parsed from {path}")
            print(f"  {nick:28s} EMPTY?")
            continue
        loads, first = library_loads(cli, path, names[0])
        state = "loads" if loads else "*** WILL NOT LOAD ***"
        print(f"  {nick:28s} {len(names):4d} symbols  {state}")
        if not loads:
            problems.append(
                f"symbol library {nick!r} cannot be loaded by KiCad ({path.name}). "
                f"Every symbol in it is unusable; the schematic keeps working only "
                f"because it renders from its own lib_symbols cache.")

    print(f"\nfootprint libraries declared: {len(fp_tbl)}")
    for nick, uri in fp_tbl:
        path = Path(uri.replace("${KIPRJMOD}", str(root)))
        if not path.is_dir():
            problems.append(f"footprint library {nick!r}: no directory at {path}")
            print(f"  {nick:28s} MISSING  {path}")
            continue
        print(f"  {nick:28s} {len(footprint_names(path)):4d} footprints")

    # ---- 3: illegal characters in names -----------------------------------
    print("\nnames containing a colon (fatal -- it is the LIB_ID separator):")
    bad_names = 0
    for nick, uri in sym_tbl:
        path = Path(uri.replace("${KIPRJMOD}", str(root)))
        for n in symbol_names(path):
            if ILLEGAL_IN_NAME in n:
                bad_names += 1
                print(f"  SYMBOL    {nick}:{n}")
                problems.append(
                    f"symbol {n!r} in {nick!r} contains ':' -- this alone makes the "
                    f"entire library unloadable")
    for nick, uri in fp_tbl:
        path = Path(uri.replace("${KIPRJMOD}", str(root)))
        for n in footprint_names(path):
            if ILLEGAL_IN_NAME in n:
                bad_names += 1
                print(f"  FOOTPRINT {nick}:{n}")
                problems.append(f"footprint {n!r} in {nick!r} contains ':'")
    if not bad_names:
        print("  none")

    # ---- 4: every referenced nickname is registered ------------------------
    sym_nicks = {n for n, _ in sym_tbl}
    fp_nicks = {n for n, _ in fp_tbl}
    scs = list(root.glob("*.kicad_sch"))
    pcbs = list(root.glob("*.kicad_pcb"))
    used_sym: set[str] = set()
    for f in scs:
        used_sym |= {m.split(":", 1)[0] for m in
                     re.findall(r'\(lib_id "([^"]+)"\)', f.read_text(encoding="utf-8"))
                     if ":" in m}
    used_fp: set[str] = set()
    for f in pcbs:
        used_fp |= {m.split(":", 1)[0] for m in
                    re.findall(r'\(footprint "([^"]+)"', f.read_text(encoding="utf-8"))
                    if ":" in m}
    print("\nnicknames referenced but not registered:")
    miss = (used_sym - sym_nicks) | {n for n in used_fp - fp_nicks}
    for n in sorted(used_sym - sym_nicks):
        print(f"  SYMBOL    {n!r} used in the schematic, absent from sym-lib-table")
        problems.append(f"symbol nickname {n!r} is used but not in sym-lib-table")
    for n in sorted(used_fp - fp_nicks):
        print(f"  FOOTPRINT {n!r} used on the board, absent from fp-lib-table")
        problems.append(f"footprint nickname {n!r} is used but not in fp-lib-table")
    if not miss:
        print("  none")

    print()
    if problems:
        print(f"FAIL -- {len(problems)} problem(s):")
        for p in problems:
            print(f"  * {p}")
        return FAIL
    print("PASS -- every declared library loads, no illegal names, "
          "every referenced nickname registered")
    return OK


if __name__ == "__main__":
    raise SystemExit(main())
