#!/usr/bin/env python3
"""Add LCSC-sourced parts to the KiCad project, and keep 3D models present.

Board3 came out of EasyEDA with every part carrying full supplier identity, and
the downstream fab flow depends on that staying true: `kicad_fab.py` builds the
JLCPCB BOM by reading the schematic's supplier fields, so a part added without
them is not "missing a nice-to-have field", it is a part that silently drops out
of the BOM. This tool is how a new part gets added so that never happens.

It drives the JLCImport KiCad plugin (jvanderberg/kicad-jlcimport) headlessly.
That plugin is stdlib-only, so it runs under any Python 3.11+ -- it does not
need KiCad's bundled interpreter. The one wrinkle is that it is installed under
a PCM directory name (`com_github_jvanderberg_kicad-jlcimport`) that is not a
legal module name, while its own code does absolute `kicad_jlcimport.*` imports.
`_load_plugin()` bridges that with an importlib alias rather than asking anyone
to pip-install a second copy.

Two field-naming traps, both load-bearing:

  * JLCImport writes the LCSC code to a property named "LCSC". Board3 -- and
    therefore `kicad_fab.py` -- reads it from "Supplier Part". Import without
    remapping and the BOM shows the part unsourced.
  * "LCSC Part Name" is NOT the part number despite the name; it holds
    descriptive text. Do not put a C-number there.

Commands:

    python tools/kicad_lcsc.py add C116592          # new part, full metadata + 3D
    python tools/kicad_lcsc.py models               # backfill missing 3D models
    python tools/kicad_lcsc.py check                # audit; non-zero if gaps

The 3D model transform and the CPL rotation are independent values -- the
rotation caveat in `kicad_fab.py` is about assembly and is not addressed here.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
REPO = HERE.parent
DEFAULT_PROJECT = REPO / "kicad" / "board3"

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_INCOMPLETE = 2

# JLCImport's property name on the left, Board3's on the right. Board3's names
# are EasyEDA's, and they are the design's vocabulary -- kicad_fab.py, the
# schematic, and every existing part already speak it. Remap the import to the
# design, never the other way around.
FIELD_REMAP = {"LCSC": "Supplier Part"}

# Properties every sourced part carries. "Supplier" is constant; the others are
# filled from the component data JLCImport already fetched.
REQUIRED_FIELDS = ("Supplier", "Supplier Part", "Manufacturer", "Manufacturer Part")


# --------------------------------------------------------------------------
# plugin loading


def _plugin_candidates() -> list[Path]:
    """Directories that may hold the JLCImport plugin, best guess first."""
    env = os.environ.get("JLCIMPORT_PLUGIN_DIR")
    if env:
        return [Path(env)]

    home = Path.home()
    if sys.platform in ("win32", "darwin"):
        roots = [home / "Documents" / "KiCad"]
    else:
        roots = [home / ".local" / "share" / "kicad"]

    found: list[Path] = []
    for root in roots:
        if not root.is_dir():
            continue
        # Newest KiCad version first.
        for ver in sorted(root.iterdir(), reverse=True):
            plugins = ver / "3rdparty" / "plugins"
            if plugins.is_dir():
                found.extend(sorted(plugins.glob("*jlcimport*")))
    return found


def _load_plugin():
    """Import the plugin package under the name its own code expects."""
    if "kicad_jlcimport" in sys.modules:
        return sys.modules["kicad_jlcimport"]

    for cand in _plugin_candidates():
        init = cand / "__init__.py"
        if not init.is_file():
            continue
        spec = importlib.util.spec_from_file_location(
            "kicad_jlcimport", init, submodule_search_locations=[str(cand)]
        )
        if spec is None or spec.loader is None:
            continue
        module = importlib.util.module_from_spec(spec)
        sys.modules["kicad_jlcimport"] = module
        spec.loader.exec_module(module)
        return module

    raise SystemExit(
        "JLCImport plugin not found.\n"
        "  Install it from the KiCad Plugin and Content Manager, or set\n"
        "  JLCIMPORT_PLUGIN_DIR to the plugin directory."
    )


# --------------------------------------------------------------------------
# s-expression helpers
#
# These do balanced-paren span finding rather than full parse-and-re-emit. A
# round trip through a parser would reformat the whole library file and bury the
# one-line change in thousands of lines of noise; splicing keeps diffs readable.


def _span(text: str, start: int) -> tuple[int, int]:
    """Return (start, end) of the s-expression beginning at *start*."""
    depth = 0
    i = start
    in_str = False
    while i < len(text):
        c = text[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return start, i + 1
        i += 1
    raise ValueError("unbalanced s-expression")


def _find_symbol(text: str, name: str) -> tuple[int, int] | None:
    """Find the top-level (symbol "name" ...) block in a .kicad_sym."""
    for m in re.finditer(r'\(symbol\s+"([^"]+)"', text):
        if m.group(1) == name:
            return _span(text, m.start())
    return None


def _properties(block: str) -> dict[str, tuple[int, int, str]]:
    """Map property name -> (start, end, raw text) within *block*."""
    out: dict[str, tuple[int, int, str]] = {}
    for m in re.finditer(r'\(property\s+"([^"]+)"', block):
        s, e = _span(block, m.start())
        out[m.group(1)] = (s, e, block[s:e])
    return out


def _property_value(raw: str) -> str:
    m = re.search(r'\(property\s+"(?:[^"\\]|\\.)*"\s+"((?:[^"\\]|\\.)*)"', raw)
    return m.group(1) if m else ""


def _hide(raw: str) -> str:
    """Force a property block hidden; metadata should not clutter the sheet."""
    if "(hide yes)" in raw:
        return raw
    if "(hide no)" in raw:
        return raw.replace("(hide no)", "(hide yes)")
    # KiCad 9/10 write `(effects (font ...))` with no hide token when visible.
    return re.sub(r"(\(effects\s)", r"\1(hide yes) ", raw, count=1)


def _set_property(block: str, name: str, value: str, template: str) -> str:
    """Add or update property *name* in a symbol *block*.

    *template* is an existing property's raw text, cloned for shape so the new
    block inherits whatever field layout this KiCad version writes.
    """
    props = _properties(block)
    esc = value.replace("\\", "\\\\").replace('"', '\\"')
    if name in props:
        s, e, raw = props[name]
        new = re.sub(
            r'(\(property\s+"' + re.escape(name) + r'"\s+)"(?:[^"\\]|\\.)*"',
            lambda m: m.group(1) + '"' + esc + '"',
            raw,
            count=1,
        )
        return block[:s] + new + block[e:]

    clone = re.sub(
        r'(\(property\s+)"(?:[^"\\]|\\.)*"(\s+)"(?:[^"\\]|\\.)*"',
        lambda m: m.group(1) + '"' + name.replace('"', '\\"') + '"' + m.group(2) + '"' + esc + '"',
        template,
        count=1,
    )
    clone = _hide(clone)
    # Insert after the last existing property so ordering stays stable, matching
    # that property's indentation -- KiCad reformats on save, but an unindented
    # block makes the intermediate file hostile to read and to diff.
    last_start, last_end = max(((s, e) for s, e, _ in props.values()), key=lambda t: t[1])
    indent = block[block.rfind("\n", 0, last_start) + 1:last_start]
    return block[:last_end] + "\n" + indent + clone + block[last_end:]


def _rename_property(block: str, old: str, new: str) -> str:
    props = _properties(block)
    if old not in props or new in props:
        return block
    s, e, raw = props[old]
    renamed = raw.replace(f'(property "{old}"', f'(property "{new}"', 1)
    return block[:s] + renamed + block[e:]


# --------------------------------------------------------------------------
# schematic reading


def find_schematic(project: Path) -> Path:
    scs = sorted(project.glob("*.kicad_sch"))
    if not scs:
        raise SystemExit(f"no .kicad_sch in {project}")
    return scs[0]


def read_parts(schematic: Path) -> list[dict]:
    """Every placed component with its footprint and supplier fields."""
    text = schematic.read_text(encoding="utf-8")
    parts: list[dict] = []
    for m in re.finditer(r"\(symbol\s*\n", text):
        try:
            s, e = _span(text, m.start())
        except ValueError:
            continue
        block = text[s:e]
        props = {name: _property_value(raw) for name, (_, _, raw) in _properties(block).items()}
        ref = props.get("Reference", "")
        if not ref or ref.startswith("#"):
            continue
        parts.append(props)
    return parts


def footprint_name(fp_field: str) -> str:
    """'Lib:FP-NAME' -> 'FP-NAME'."""
    return fp_field.split(":", 1)[-1] if fp_field else ""


# --------------------------------------------------------------------------
# 3D model backfill


def model_refs(project: Path) -> dict[str, list[Path]]:
    """Map absolute model path -> footprint files referencing it."""
    refs: dict[str, list[Path]] = {}
    for pretty in project.glob("*.pretty"):
        for mod in sorted(pretty.glob("*.kicad_mod")):
            text = mod.read_text(encoding="utf-8", errors="replace")
            for m in re.finditer(r'\(model\s+"([^"]+)"', text):
                resolved = m.group(1).replace("${KIPRJMOD}", str(project)).replace("\\", "/")
                refs.setdefault(resolved, []).append(mod)
    return refs


def fp_to_lcsc(parts: list[dict]) -> dict[str, list[str]]:
    """Footprint short name -> LCSC codes of parts using it."""
    out: dict[str, list[str]] = {}
    for p in parts:
        fp = footprint_name(p.get("Footprint", ""))
        code = (p.get("Supplier Part") or "").strip()
        if fp and code:
            codes = out.setdefault(fp, [])
            if code not in codes:
                codes.append(code)
    return out


def fetch_model(lcsc: str, kicad_version: int, log) -> Path | None:
    """Export one part to a temp dir and return its .step, or None."""
    _load_plugin()
    from kicad_jlcimport.importer import import_component  # noqa: PLC0415

    tmp = Path(tempfile.mkdtemp(prefix="jlcmodel_"))
    try:
        import_component(
            lcsc, str(tmp), "JLCImport",
            export_only=True, log=lambda _m: None, kicad_version=kicad_version,
        )
    except Exception as exc:  # noqa: BLE001 - network and parse errors are both just "no model"
        log(f"    fetch failed for {lcsc}: {exc}")
        shutil.rmtree(tmp, ignore_errors=True)
        return None

    steps = list(tmp.rglob("*.step"))
    if not steps:
        shutil.rmtree(tmp, ignore_errors=True)
        return None
    return steps[0]


def cmd_models(args) -> int:
    project = Path(args.project).resolve()
    parts = read_parts(find_schematic(project))
    mapping = fp_to_lcsc(parts)
    refs = model_refs(project)

    missing = {path: fps for path, fps in refs.items() if not Path(path).exists()}
    print(f"model refs   : {len(refs)}")
    print(f"already have : {len(refs) - len(missing)}")
    print(f"missing      : {len(missing)}")
    if not missing:
        return EXIT_OK

    fetched = 0
    failed: list[str] = []
    for path, fps in sorted(missing.items()):
        target = Path(path)
        # Any part sharing this footprint has the right body; R0603 is R0603.
        codes: list[str] = []
        for fp in fps:
            for code in mapping.get(fp.stem, []):
                if code not in codes:
                    codes.append(code)
        if not codes:
            failed.append(f"{target.name}: no LCSC part uses {[f.stem for f in fps]}")
            continue

        print(f"  {target.name}  <- {codes[0]}")
        src = None
        for code in codes[:3]:  # a part can lack a model; try the next one
            src = fetch_model(code, args.kicad_version, print)
            if src:
                break
        if not src:
            failed.append(f"{target.name}: no model from {codes[:3]}")
            continue

        if args.dry_run:
            print(f"    (dry run) would write {target}")
        else:
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, target)
        shutil.rmtree(src.parent, ignore_errors=True)
        fetched += 1

    print(f"\nfetched      : {fetched}")
    if failed:
        print(f"unresolved   : {len(failed)}")
        for f in failed:
            print(f"  {f}")
        return EXIT_INCOMPLETE
    return EXIT_OK


# --------------------------------------------------------------------------
# add


def cmd_add(args) -> int:
    _load_plugin()
    from kicad_jlcimport.easyeda.api import APIError, validate_lcsc_id  # noqa: PLC0415
    from kicad_jlcimport.importer import import_component  # noqa: PLC0415

    try:
        lcsc = validate_lcsc_id(args.part)
    except ValueError as exc:
        print(f"error: {exc}")
        return EXIT_ERROR

    project = Path(args.project).resolve()
    if not project.is_dir():
        print(f"error: project directory does not exist: {project}")
        return EXIT_ERROR

    try:
        result = import_component(
            lcsc, str(project), args.lib_name,
            overwrite=args.overwrite, use_global=False,
            log=lambda m: print(f"  {m}"), kicad_version=args.kicad_version,
        )
    except APIError as exc:
        print(f"error: {exc}")
        return EXIT_ERROR
    if not result:
        print("error: import produced nothing (part may already exist; use --overwrite)")
        return EXIT_ERROR

    name = result["name"]
    symlib = project / f"{args.lib_name}.kicad_sym"
    if not symlib.is_file():
        print(f"error: expected symbol library not written: {symlib}")
        return EXIT_ERROR

    text = symlib.read_text(encoding="utf-8")
    found = _find_symbol(text, name)
    if not found:
        print(f"error: symbol {name!r} not found in {symlib.name} after import")
        return EXIT_ERROR
    s, e = found
    block = text[s:e]

    props = _properties(block)
    if not props:
        print("error: imported symbol has no properties to remap")
        return EXIT_ERROR
    template = next(iter(props.values()))[2]

    for old, new in FIELD_REMAP.items():
        block = _rename_property(block, old, new)
    block = _set_property(block, "Supplier", "LCSC", template)
    # "LCSC Part Name" is descriptive text, not the code. Mirror what the
    # EasyEDA-imported parts carry: the component title.
    block = _set_property(block, "LCSC Part Name", result.get("title", name), template)

    symlib.write_text(text[:s] + block + text[e:], encoding="utf-8")

    final = _properties(block)
    missing = [f for f in REQUIRED_FIELDS if f not in final]
    models = sorted((project / f"{args.lib_name}.3dshapes").glob(f"{name}.*"))

    print()
    print(f"added        : {name}  ({lcsc})")
    print(f"library      : {symlib.name} / {args.lib_name}.pretty")
    print(f"3D model     : {', '.join(m.name for m in models) if models else 'NONE'}")
    print(f"metadata     : {'complete' if not missing else 'MISSING ' + ', '.join(missing)}")
    for field in REQUIRED_FIELDS:
        if field in final:
            print(f"  {field:20s} {_property_value(final[field][2])}")

    if missing or not models:
        if not models:
            print("\nwarning: no 3D model was available for this part.")
        return EXIT_INCOMPLETE
    print("\nNOTE: reopen the project in KiCad for new library tables to take effect.")
    return EXIT_OK


# --------------------------------------------------------------------------
# check


def find_board(project: Path) -> Path | None:
    boards = sorted(project.glob("*.kicad_pcb"))
    return boards[0] if boards else None


def board_footprints(pcb: Path) -> list[dict]:
    """Every footprint placed on the board, with its reference and model refs.

    Audits the board rather than the library on purpose: the board is what gets
    fabricated, and each placed footprint carries its own copy of the model
    reference. A library footprint can be perfect while an instance placed
    before it was fixed is not.
    """
    text = pcb.read_text(encoding="utf-8", errors="replace")
    out: list[dict] = []
    for m in re.finditer(r"\n\t\(footprint ", text):
        s, e = _span(text, m.start() + 1)
        blk = text[s:e]
        lib = re.search(r'\(footprint "([^"]+)"', blk)
        ref = re.search(r'\(property "Reference" "([^"]*)"', blk)
        out.append({
            "ref": (ref.group(1) if ref else "").strip(),
            "lib": lib.group(1) if lib else "?",
            "models": re.findall(r'\(model\s+"([^"]+)"', blk),
        })
    return out


# Footprints that legitimately have no 3D body and no LCSC part behind them.
# Kept deliberately narrow, and every match is printed on every run: an
# exemption you cannot see is an exemption you cannot audit, and this table is
# the only route by which a real part could ever pass unnoticed.
BODILESS = (
    (re.compile(r"fiducial", re.I), "fiducial - copper and mask only"),
    (re.compile(r"^pad_e\d+$", re.I), "free pad - bare copper, not a component"),
    (re.compile(r"mountinghole|^hole[-_]", re.I), "mounting hole"),
    (re.compile(r"^testpoint|^tp[-_]", re.I), "test point"),
    (re.compile(r"^solderjumper|^jumper[-_]", re.I), "solder jumper"),
    (re.compile(r"^logo|^graphic", re.I), "silkscreen graphic"),
)


def bodiless_reason(fp: dict) -> str:
    """Why this footprint needs neither a 3D model nor an LCSC code, or ''."""
    name = fp["lib"].split(":", 1)[-1]
    for pattern, reason in BODILESS:
        if pattern.search(name):
            return reason
    return ""


def _short(fp: dict) -> str:
    return fp["lib"].split(":", 1)[-1]


def cmd_check(args) -> int:
    project = Path(args.project).resolve()
    schematic = find_schematic(project)
    parts = read_parts(schematic)
    by_ref = {p.get("Reference", ""): p for p in parts}

    print(f"schematic    : {schematic.name}")

    pcb = find_board(project)
    if pcb is None:
        print("board        : none found -- checking the schematic only")
        placed: list[dict] = []
    else:
        print(f"board        : {pcb.name}")
        placed = board_footprints(pcb)

    exempt = [(fp, bodiless_reason(fp)) for fp in placed]
    exempt = [(fp, r) for fp, r in exempt if r]
    exempt_ids = {id(fp) for fp, _ in exempt}
    real = [fp for fp in placed if id(fp) not in exempt_ids]

    no_lcsc: list[dict] = []
    no_model: list[dict] = []
    broken: list[tuple[dict, str]] = []
    for fp in real:
        sch = by_ref.get(fp["ref"], {})
        if not (sch.get("Supplier Part") or "").strip():
            no_lcsc.append(fp)
        if not fp["models"]:
            no_model.append(fp)
        for raw in fp["models"]:
            if not Path(raw.replace("${KIPRJMOD}", str(project))).exists():
                broken.append((fp, raw))

    # A symbol drawn but never placed is never fabricated, and a board-side
    # audit alone would never notice it.
    on_board = {f["ref"] for f in placed}
    unplaced = [p for p in parts if p.get("Reference") and p["Reference"] not in on_board]
    sch_unsourced = [p for p in parts if not (p.get("Supplier Part") or "").strip()]

    print(f"placed       : {len(placed)}  ({len(real)} components, {len(exempt)} bodiless by design)")
    print(f"LCSC mapped  : {len(real) - len(no_lcsc)}/{len(real)}")
    print(f"3D modelled  : {len(real) - len(no_model)}/{len(real)}")

    if exempt:
        print(f"\nexempt, needing neither a model nor an LCSC code ({len(exempt)}):")
        for fp, reason in sorted(exempt, key=lambda t: (t[1], t[0]["ref"])):
            print(f"  {fp['ref'] or '(no ref)':10s} {_short(fp):32s} {reason}")

    if no_lcsc:
        print(f"\nNOT MAPPED TO LCSC ({len(no_lcsc)}):")
        for fp in no_lcsc:
            sch = by_ref.get(fp["ref"], {})
            why = "not in schematic" if not sch else "no Supplier Part field"
            print(f"  {fp['ref'] or '(no ref)':10s} {sch.get('Value', ''):22s} {_short(fp):30s} {why}")
        print("  every placed part must carry one: python tools/kicad_lcsc.py add C<n>")

    if no_model:
        print(f"\nNO 3D MODEL REFERENCE ({len(no_model)}):")
        for fp in no_model:
            print(f"  {fp['ref'] or '(no ref)':10s} {_short(fp)}")
        print("  re-import the part, or extend BODILESS if it genuinely has no body")

    if broken:
        print(f"\nMODEL FILE MISSING ON DISK ({len(broken)}):")
        for fp, raw in sorted(broken, key=lambda t: t[1]):
            print(f"  {fp['ref'] or '(no ref)':10s} {raw}")
        print("  run: python tools/kicad_lcsc.py models")

    if unplaced:
        print(f"\nin the schematic but not on the board ({len(unplaced)}):")
        for p in unplaced:
            print(f"  {p.get('Reference', '?'):10s} {p.get('Value', '')}")

    if sch_unsourced:
        print(f"\nschematic symbols with no LCSC code ({len(sch_unsourced)}):")
        for p in sch_unsourced:
            print(f"  {p.get('Reference', '?'):10s} {p.get('Value', '')}")

    failed = bool(no_lcsc or no_model or broken or sch_unsourced)
    print(f"\n{'FAIL' if failed else 'PASS'}")
    return EXIT_INCOMPLETE if failed else EXIT_OK


def _relaunch_utf8() -> int | None:
    """Re-run under UTF-8 mode if the interpreter is on a legacy codepage.

    JLCImport opens its output files without an explicit encoding, so on a
    Windows cp1252 console every part whose EasyEDA description carries a
    Chinese character or a degree sign dies with a UnicodeEncodeError -- three
    of Board3's parts did exactly that (the crystal, the ULN2803, the USB-C).
    Relaunch rather than documenting `PYTHONUTF8=1`: the failure is
    machine-specific and a note in a README does not survive contact.

    Deliberately subprocess and not os.execv: on Windows execv does not replace
    the process, so the parent returns to the shell immediately and the child's
    output and exit code are both lost.
    """
    if sys.flags.utf8_mode or os.environ.get("KICAD_LCSC_REEXEC") == "1":
        return None
    env = dict(os.environ, KICAD_LCSC_REEXEC="1", PYTHONUTF8="1")
    argv = [sys.executable, "-X", "utf8", os.path.abspath(__file__), *sys.argv[1:]]
    return subprocess.run(argv, env=env).returncode


def _annotate(level: str, msg: str) -> None:
    """Emit a GitHub Actions annotation when running in CI, plain text otherwise."""
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print(f"::{level}::{msg}")
    else:
        print(f"  {msg}")


def cmd_duplicates(args) -> int:
    """Report parts that are the same thing bought twice, or two things bought once.

    Two directions, and only one of them is a defect:

      - one value+footprint mapped to several LCSC codes is legal but wasteful --
        every extra code is another reel and another feeder at assembly. Board3
        ships two different 100nF 0603 parts and two different 10k 0603 parts.
      - one LCSC code mapped to several values is almost always a mistake: the
        schematic says 10k and the BOM orders something else. Nothing downstream
        catches this, because both fields are individually well-formed.
    """
    project = Path(args.project).resolve()
    parts = read_parts(find_schematic(project))

    by_value: dict[tuple[str, str], dict[str, list[str]]] = {}
    by_code: dict[str, dict[str, list[str]]] = {}
    for p in parts:
        code = (p.get("Supplier Part") or "").strip()
        if not code:
            continue
        ref = p.get("Reference", "?")
        value = (p.get("Value") or "").strip()
        fp = footprint_name(p.get("Footprint", ""))
        by_value.setdefault((value, fp), {}).setdefault(code, []).append(ref)
        by_code.setdefault(code, {}).setdefault(value, []).append(ref)

    split = {k: v for k, v in by_value.items() if len(v) > 1}
    mixed = {k: v for k, v in by_code.items() if len(v) > 1}

    print(f"distinct LCSC codes : {len(by_code)}")
    print(f"value+footprint keys: {len(by_value)}")

    if split:
        print(f"\none value+footprint, several LCSC codes ({len(split)}):")
        for (value, fp), codes in sorted(split.items()):
            print(f"  {value or '(no value)'} / {fp}")
            for code, refs in sorted(codes.items()):
                print(f"      {code:12s} x{len(refs):<3d} {','.join(sorted(refs))}")
            _annotate("warning",
                      f"{value} / {fp} is sourced as {len(codes)} different LCSC parts "
                      f"({', '.join(sorted(codes))}) - each is an extra reel and feeder")

    if mixed:
        print(f"\nONE LCSC CODE, SEVERAL VALUES ({len(mixed)}) - likely a mismapping:")
        for code, values in sorted(mixed.items()):
            print(f"  {code}")
            for value, refs in sorted(values.items()):
                print(f"      {value or '(no value)':24s} {','.join(sorted(refs))}")
            _annotate("error",
                      f"LCSC {code} is mapped to {len(values)} different values "
                      f"({', '.join(sorted(values))}) - the BOM may order the wrong part")

    if not split and not mixed:
        print("\nno duplicate or conflicting LCSC mappings")

    # Only the mismapping direction is a defect. Consolidation is a judgement
    # call, and failing a PR over it would just teach people to ignore the check.
    if mixed:
        return EXIT_INCOMPLETE
    return EXIT_INCOMPLETE if (split and args.strict) else EXIT_OK


def main() -> int:
    relaunched = _relaunch_utf8()
    if relaunched is not None:
        return relaunched
    ap = argparse.ArgumentParser(
        prog="kicad_lcsc.py",
        description="Add LCSC-sourced parts to the KiCad project with full metadata and 3D models.",
    )
    ap.add_argument("-p", "--project", default=str(DEFAULT_PROJECT),
                    help="KiCad project directory (default: kicad/board3)")
    ap.add_argument("--kicad-version", type=int, default=10, help="target KiCad version (default: 10)")
    sub = ap.add_subparsers(dest="command", required=True)

    a = sub.add_parser("add", help="import an LCSC part with metadata and 3D model")
    a.add_argument("part", help="LCSC part number, e.g. C116592")
    a.add_argument("--lib-name", default="JLCImport", help="project library name (default: JLCImport)")
    a.add_argument("--overwrite", action="store_true", help="replace an existing symbol/footprint")
    a.set_defaults(func=cmd_add)

    m = sub.add_parser("models", help="download any 3D model the board references but lacks")
    m.add_argument("--dry-run", action="store_true", help="report what would be fetched, write nothing")
    m.set_defaults(func=cmd_models)

    c = sub.add_parser("check", help="audit LCSC metadata and 3D model coverage")
    c.set_defaults(func=cmd_check)

    d = sub.add_parser("duplicates", help="find duplicate or conflicting LCSC mappings")
    d.add_argument("--strict", action="store_true",
                   help="also fail when one value is sourced as several LCSC parts")
    d.set_defaults(func=cmd_duplicates)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
