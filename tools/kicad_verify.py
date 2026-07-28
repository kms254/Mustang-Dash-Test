#!/usr/bin/env python3
"""Verify a routed board: DRC against real rules, plus connectivity. R6 and R7.

Two independent checks, because neither catches the other's failures. DRC alone
passes a board that silently dropped a net; a connectivity check alone passes a
board whose copper is unmanufacturable.

Both are measured against a BASELINE board, not in isolation. Board3 carries 41
pre-existing violations -- courtyard overlaps and edge clearances that have
nothing to do with routing -- and reporting a routed board's raw total as though
the router caused all of it is how a 33-violation result gets published as 74.

Rules come from tools/kicad_rules.json, never from the board's own .kicad_pro.
The EasyEDA Pro importer writes KiCad's stock defaults there, and measuring
against those produces 503 phantom clearance violations on a clean board.

  python tools/kicad_verify.py routed.kicad_pcb --baseline baseline.kicad_pcb
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
RULES = HERE / "kicad_rules.json"

EXIT_CLEAN = 0
EXIT_ERROR = 1
EXIT_NEW_VIOLATIONS = 2


def _kicad_cli() -> str:
    sys.path.insert(0, str(HERE))
    from kicad_env import find_kicad_cli  # reuse U1's resolver, never hardcode

    return str(find_kicad_cli())


def _rules_project(board: Path, workdir: Path) -> Path:
    """Stage the board beside a project file carrying the real rules."""
    spec = json.loads(RULES.read_text(encoding="utf-8"))
    staged = workdir / board.name
    staged.write_bytes(board.read_bytes())

    sibling = board.with_suffix(".kicad_pro")
    pro = json.loads(sibling.read_text(encoding="utf-8")) if sibling.is_file() else {}
    rules = pro.setdefault("board", {}).setdefault("design_settings", {}).setdefault("rules", {})
    rules.update(spec["rules_mm"])
    classes = pro.setdefault("net_settings", {}).setdefault("classes", [])
    if not any(c.get("name") == "Default" for c in classes):
        classes.append({"name": "Default"})
    for c in classes:
        if c.get("name") == "Default":
            c.update(spec["default_netclass_mm"])
    staged.with_suffix(".kicad_pro").write_text(json.dumps(pro, indent=2), encoding="utf-8")
    return staged


def run_drc(board: Path, workdir: Path) -> tuple[collections.Counter, int]:
    staged = _rules_project(board, workdir)
    report = workdir / (board.stem + ".drc.json")
    subprocess.run(
        [_kicad_cli(), "pcb", "drc", "--severity-error", "--format", "json",
         "-o", str(report), str(staged)],
        capture_output=True, text=True,
    )
    if not report.is_file():
        raise RuntimeError(f"kicad-cli produced no DRC report for {board.name}")
    data = json.loads(report.read_text(encoding="utf-8"))
    counts = collections.Counter(v.get("type") for v in data.get("violations", []))
    return counts, len(data.get("unconnected_items", []))


def cost_floor_advisories(board: Path) -> list:
    """Report geometry that is manufacturable but priced above the cheap tier.

    These must never fail the gate -- a gate that is permanently red gets waved
    through, which is how the review harness's calibration rotted. But they must
    not be invisible either: a 0.2 mm drill is inside JLC's 0.15-6.3 mm range and
    still a surcharge, so it should stay a deliberate, visible choice.
    """
    spec = json.loads(RULES.read_text(encoding="utf-8"))
    floors = {k: v for k, v in spec.get("cost_floors_mm", {}).items()
              if not k.startswith("$")}
    drill_floor = floors.get("through_hole_diameter")
    if not drill_floor:
        return []
    text = board.read_text(encoding="utf-8", errors="ignore")
    small = sorted({float(m) for m in re.findall(r"\(drill ([0-9.]+)\)", text)
                    if float(m) < drill_floor})
    if not small:
        return []
    return [f"drills below the {drill_floor} mm cost floor: "
            + ", ".join(f"{d} mm" for d in small)]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board")
    ap.add_argument("--baseline", help="Board to attribute pre-existing violations to")
    args = ap.parse_args()

    board = Path(args.board)
    if not board.is_file():
        print(f"no such board: {board}", file=sys.stderr)
        return EXIT_ERROR

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        try:
            counts, unconnected = run_drc(board, work)
            base_counts = collections.Counter()
            base_unconn = None
            if args.baseline:
                base_counts, base_unconn = run_drc(Path(args.baseline), work)
        except (RuntimeError, ImportError) as exc:
            print(f"verification failed: {exc}", file=sys.stderr)
            return EXIT_ERROR

    total = sum(counts.values())
    print(f"board        : {board.name}")
    print(f"violations   : {total}")
    for note in cost_floor_advisories(board):
        print(f"advisory     : {note}")
    print(f"unconnected  : {unconnected}"
          + (f"   (baseline {base_unconn})" if base_unconn is not None else ""))

    if args.baseline:
        new = {k: counts[k] - base_counts.get(k, 0) for k in counts
               if counts[k] > base_counts.get(k, 0)}
        print(f"baseline     : {sum(base_counts.values())} violations "
              f"({Path(args.baseline).name})")
        print(f"NEW          : {sum(new.values())}")
        for k, n in sorted(new.items(), key=lambda kv: -kv[1]):
            print(f"   {k:<26} +{n}  (baseline {base_counts.get(k, 0)})")
        carried = {k: v for k, v in base_counts.items() if v and k not in new}
        if carried:
            print("carried over :", ", ".join(f"{k}={v}" for k, v in sorted(carried.items())))
        return EXIT_NEW_VIOLATIONS if new else EXIT_CLEAN

    for k, n in counts.most_common():
        print(f"   {k:<26} {n}")
    return EXIT_NEW_VIOLATIONS if total else EXIT_CLEAN


if __name__ == "__main__":
    raise SystemExit(main())
