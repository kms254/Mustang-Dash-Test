#!/usr/bin/env python3
"""Generate JLCPCB-ready BOM and CPL from the KiCad project, and report coverage.

The interesting part is the field mapping. Board3's parts came from EasyEDA
carrying full supplier identity, and the import preserved it -- but under
EasyEDA's field names ("LCSC Part Name", "Manufacturer Part") rather than the
conventional ones. Tools that look for a field literally named MPN therefore
report zero coverage on a fully-sourced board: kicad-happy's SS-001 called it
"0/48 unique parts" and flagged the board as not pre-fab ready, when 49 of 50
grouped BOM lines actually carry both an LCSC number and a manufacturer part.

So this maps rather than sources. Nothing here looks parts up; it reads what is
already in the schematic and emits it under the column names JLCPCB expects.

  python tools/kicad_fab.py kicad/board3/ --out fab/

Rotation warning: KiCad and JLCPCB do not always agree on a footprint's zero
rotation. This tool emits KiCad's value unmodified and does not correct it --
verify at least one known-orientation part against its datasheet pin 1 before
ordering assembly. A silent rotation mismatch is a board full of backwards
parts, not a warning.
"""

from __future__ import annotations

import argparse
import csv
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_INCOMPLETE = 2

# EasyEDA's field names on the left, what we emit on the right. Extend this
# rather than renaming fields in the schematic -- the schematic is the design,
# and editing it to satisfy a downstream tool's naming convention risks the
# netlist for a cosmetic gain.
#
# The LCSC code lives in "Supplier Part" (C14663, C116592...). NOT in
# "LCSC Part Name", which despite its name holds descriptive text, often in
# Chinese -- "0805 白灯 高亮" is a description of an 0805 white LED, not a
# part number. Mapping the obvious-looking field produced a BOM whose LCSC
# column contained footprint names and capacitor values, and it looked fine
# in aggregate because every row was populated.
#
# "JLCPCB Part Class" is Basic vs Extended, which drives assembly cost --
# Extended parts carry a per-reel loading fee, so it belongs in a BOM someone
# is about to quote.
BOM_FIELDS = ("Reference,Value,Footprint,${QUANTITY},Supplier Part,"
              "Manufacturer Part,Manufacturer,JLCPCB Part Class")
BOM_LABELS = "Designator,Comment,Footprint,Quantity,LCSC,MPN,Manufacturer,Part Class"
BOM_GROUP_BY = "Value,Footprint,Supplier Part"


def _cli() -> str:
    sys.path.insert(0, str(HERE))
    from kicad_env import find_kicad_cli  # reuse U1's resolver; never hardcode

    return str(find_kicad_cli())


def _run(*args: str) -> subprocess.CompletedProcess:
    proc = subprocess.run([_cli(), *args], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"kicad-cli {' '.join(args[:3])} failed: {proc.stderr.strip()[:400]}")
    return proc


def find_project(target: Path) -> tuple[Path, Path]:
    """Resolve a project directory to (schematic, board). Ambiguity is an error."""
    project = target if target.is_dir() else target.parent
    schs = sorted(project.glob("*.kicad_sch"))
    pcbs = sorted(project.glob("*.kicad_pcb"))
    if len(schs) != 1 or len(pcbs) != 1:
        raise RuntimeError(
            f"expected exactly one .kicad_sch and one .kicad_pcb in {project}; "
            f"found {len(schs)} and {len(pcbs)}"
        )
    return schs[0], pcbs[0]


def export_bom(schematic: Path, out: Path) -> list[dict]:
    proc = _run("sch", "export", "bom",
                "--fields", BOM_FIELDS, "--labels", BOM_LABELS,
                "--group-by", BOM_GROUP_BY, "--exclude-dnp",
                "-o", str(out), str(schematic))
    # kicad-cli reports annotation problems on stdout rather than failing. An
    # unannotated symbol reaches the BOM as designator "?" and cannot be ordered.
    if "annotation" in (proc.stdout + proc.stderr).lower():
        print("  WARNING: schematic has annotation errors -- see unplaceable rows below",
              flush=True)
    return list(csv.DictReader(out.open(encoding="utf-8-sig")))


def export_cpl(board: Path, out: Path) -> list[dict]:
    """Export placement, then drop entries that are not placeable components.

    The raw export includes free pads carried over from the import -- rows with
    an empty Ref and a package like "Pad_e673". They are board features, not
    parts, and an assembler fed them would try to place something that does not
    exist. Filtering here rather than trusting the export is the point.
    """
    _run("pcb", "export", "pos", "--format", "csv", "--units", "mm",
         "--side", "both", "--exclude-dnp", "-o", str(out), str(board))
    rows = list(csv.DictReader(out.open(encoding="utf-8-sig")))
    placeable = [r for r in rows if (r.get("Ref") or "").strip()]
    dropped = len(rows) - len(placeable)
    if dropped:
        with out.open("w", newline="", encoding="utf-8") as fh:
            writer = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
            writer.writeheader()
            writer.writerows(placeable)
        print(f"  dropped {dropped} non-component row(s) from CPL (free pads, no Ref)",
              flush=True)
    return placeable


def coverage_report(bom: list[dict]) -> tuple[list[dict], list[dict]]:
    """Return (rows lacking any supplier identity, rows that cannot be placed)."""
    unsourced = [r for r in bom
                 if not (r.get("LCSC") or "").strip() and not (r.get("MPN") or "").strip()]
    unplaceable = [r for r in bom if "?" in (r.get("Designator") or "")
                   or not (r.get("Designator") or "").strip()]
    return unsourced, unplaceable


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("target", help="KiCad project directory (or any file inside it)")
    ap.add_argument("--out", default="fab", help="Output directory for BOM and CPL")
    args = ap.parse_args()

    try:
        schematic, board = find_project(Path(args.target))
    except RuntimeError as exc:
        print(f"fab export failed: {exc}", file=sys.stderr)
        return EXIT_ERROR

    outdir = Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)

    try:
        bom = export_bom(schematic, outdir / "bom.csv")
        cpl = export_cpl(board, outdir / "cpl.csv")
    except RuntimeError as exc:
        print(f"fab export failed: {exc}", file=sys.stderr)
        return EXIT_ERROR

    unsourced, unplaceable = coverage_report(bom)
    sourced = len(bom) - len(unsourced)

    print(f"schematic   : {schematic.name}")
    print(f"board       : {board.name}")
    print(f"BOM lines   : {len(bom)}  ({sourced} sourced, {len(unsourced)} without LCSC or MPN)")
    print(f"CPL rows    : {len(cpl)}")
    print(f"written     : {outdir / 'bom.csv'}, {outdir / 'cpl.csv'}")

    for label, rows in (("NOT SOURCED", unsourced), ("NOT PLACEABLE", unplaceable)):
        if rows:
            print(f"\n{label} ({len(rows)}):", flush=True)
            for r in rows:
                print("   designator=%-14r value=%-20r footprint=%r" % (
                    r.get("Designator", ""), (r.get("Comment") or "")[:20],
                    (r.get("Footprint") or "")[:38]), flush=True)

    print("\nRotation is emitted as KiCad reports it and is NOT corrected for JLCPCB. "
          "Verify one known-orientation part before ordering assembly.", flush=True)

    return EXIT_INCOMPLETE if (unsourced or unplaceable) else EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
