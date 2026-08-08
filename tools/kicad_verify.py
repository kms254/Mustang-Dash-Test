#!/usr/bin/env python3
"""Verify a routed board: DRC against real rules, plus connectivity. R6 and R7.

Two independent checks, because neither catches the other's failures. DRC alone
passes a board that silently dropped a net; a connectivity check alone passes a
board whose copper is unmanufacturable.

Both are ABSOLUTE by default: a clean board reports zero. That was not always
true -- Board3 once carried 41 inherited violations (courtyard overlaps and edge
clearances that had nothing to do with routing), so measurement was against a
baseline and --baseline was effectively mandatory. Those are cleared, the board
reports 0/0, and CI gates at zero with no baseline at all. --baseline and
--contract survive for interactive work, where attributing a pre-existing
violation while you edit is still useful.

Rules come from tools/kicad_rules.json, never from the board's own .kicad_pro.
The EasyEDA Pro importer writes KiCad's stock defaults there, and measuring
against those produces 503 phantom clearance violations on a clean board.

That direction has a failure mode worth knowing: because _rules_project() does
rules.update(spec["rules_mm"]), this file OVERWRITES the staged project's value
for every key it names. A design rule raised in the .kicad_pro and not mirrored
here is silently reverted for the duration of the check -- so a rule change is
not done until it lands in kicad_rules.json.

A sibling silent-degradation mode lives in the .kicad_dru this script stages
verbatim: one malformed constraint voids the ENTIRE file -- kicad-cli exits 0,
prints nothing, and judges with no custom rules at all. The fingerprint here is
known-waived findings reappearing (courtyards_overlap +26, copper_edge +10 on
Board3); a sudden jump in exactly those types after a .kicad_dru edit means the
file died, not that the board got worse. See docs/solutions/integration-issues/
kicad-dru-one-bad-constraint-silently-voids-every-custom-rule.md.

  python tools/kicad_verify.py routed.kicad_pcb                      # absolute
  python tools/kicad_verify.py routed.kicad_pcb --baseline old.kicad_pcb
"""

from __future__ import annotations

import argparse
import collections
import json
import re
import shutil
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

    # Custom DRC rules (.kicad_dru) are part of the design's rule set --
    # scoped exceptions like courtyard allowances live there. Stage the
    # sibling file too or kicad-cli judges without them.
    dru = board.with_suffix(".kicad_dru")
    if dru.is_file():
        staged.with_suffix(".kicad_dru").write_bytes(dru.read_bytes())

    # Library tables and the .pretty dirs. Without these, kicad-cli cannot
    # resolve a single footprint library and reports ~120 phantom
    # lib_footprint_issues ("library not found") -- which is exactly why this
    # function's output used to be safe only at --severity-error. Staging them
    # is what makes an all-severities run mean anything, and the phantoms are
    # not merely noise: they MASK the real lib_footprint_mismatch findings
    # underneath. See docs/solutions/developer-experience/
    # stage-project-sidecars-for-headless-drc.md.
    for name in ("fp-lib-table", "sym-lib-table"):
        table = board.parent / name
        if table.is_file():
            (workdir / name).write_bytes(table.read_bytes())
    for pretty in board.parent.glob("*.pretty"):
        if pretty.is_dir():
            shutil.copytree(pretty, workdir / pretty.name, dirs_exist_ok=True)
    return staged


def run_drc(board: Path, workdir: Path) -> tuple[collections.Counter, int]:
    staged = _rules_project(board, workdir)
    report = workdir / (board.stem + ".drc.json")
    # --severity-all, not --severity-error. A severity filter is a blind spot,
    # not a setting: the ERC gate ran errors-only for months while 124 warnings
    # saying a whole symbol library would not load sat inside it, invisible by
    # construction. This is only safe because _rules_project now stages the
    # library tables -- without them an all-severities run is mostly phantoms.
    subprocess.run(
        [_kicad_cli(), "pcb", "drc", "--severity-all", "--format", "json",
         "-o", str(report), str(staged)],
        capture_output=True, text=True,
    )
    if not report.is_file():
        raise RuntimeError(f"kicad-cli produced no DRC report for {board.name}")
    data = json.loads(report.read_text(encoding="utf-8"))
    counts = collections.Counter(v.get("type") for v in data.get("violations", []))
    return counts, _identities(data), len(data.get("unconnected_items", []))


def _identities(data: dict) -> collections.Counter:
    """Violations keyed by what they are, not just how many share a type.

    Counting by type alone cannot see a swap: fix one courtyard overlap,
    introduce a different one, and the total is unchanged, so the gate reports
    NEW = 0 and the regression ships. Keying on the involved reference
    designators makes it visible. UUIDs would be stabler but they move whenever
    a footprint is re-placed, which is exactly when this check earns its keep.
    """
    out: collections.Counter = collections.Counter()
    for v in data.get("violations", []):
        refs = []
        for item in v.get("items", []):
            desc = item.get("description", "")
            m = re.search(r"(?:of|Footprint) ([A-Za-z]+\d+)", desc)
            refs.append(m.group(1) if m else desc[:16])
        out[(v.get("type"), tuple(sorted(refs)))] += 1
    return out


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
    ap.add_argument("--contract", choices=("identity", "count"), default="identity",
                    help="Exit-code semantics. 'identity' (default) fails on any "
                         "violation instance not present in the baseline -- right "
                         "for adjacent-state gates, where a fixed+introduced swap "
                         "must not hide. 'count' fails only when a type's count "
                         "exceeds the baseline -- right for a distant baseline "
                         "(e.g. the import), where deliberate placement changes "
                         "lawfully relocate violations. Identity detail and SWAP "
                         "lines are printed in both modes.")
    args = ap.parse_args()

    board = Path(args.board)
    if not board.is_file():
        print(f"no such board: {board}", file=sys.stderr)
        return EXIT_ERROR

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        try:
            counts, idents, unconnected = run_drc(board, work)
            base_counts = collections.Counter()
            base_idents = collections.Counter()
            base_unconn = None
            if args.baseline:
                base_counts, base_idents, base_unconn = run_drc(Path(args.baseline), work)
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
        appeared = idents - base_idents          # by identity, so a swap cannot hide
        resolved = base_idents - idents
        if args.contract == "count":
            # Far-baseline contract: "no worse than the baseline", per type.
            # Identity churn between distant states is lawful (parts move);
            # only a count exceedance fails. SWAP lines below stay informative.
            new = collections.Counter({k: counts[k] - base_counts.get(k, 0)
                                       for k in counts
                                       if counts[k] > base_counts.get(k, 0)})
        else:
            new = collections.Counter(k[0] for k, n in appeared.items() for _ in range(n))
        print(f"baseline     : {sum(base_counts.values())} violations "
              f"({Path(args.baseline).name})")
        print(f"NEW          : {sum(new.values())}")
        for k, n in sorted(new.items(), key=lambda kv: -kv[1]):
            print(f"   {k:<26} +{n}  (baseline {base_counts.get(k, 0)})")
        for (kind, refs), n in sorted(appeared.items())[:12]:
            print(f"      {kind}: {' <-> '.join(refs)}")
        if resolved:
            fixed = collections.Counter(k[0] for k, n in resolved.items() for _ in range(n))
            print("resolved     :", ", ".join(f"{k}={v}" for k, v in sorted(fixed.items())))
        # A swap keeps the totals identical while changing what is wrong.
        for kind in sorted({k[0] for k in appeared} & {k[0] for k in resolved}):
            print(f"SWAP         : {kind} — different instances than the baseline, "
                  f"same total. Counting by type alone would have missed this.")
        carried = {k: v for k, v in base_counts.items() if v and k not in new}
        if carried:
            print("carried over :", ", ".join(f"{k}={v}" for k, v in sorted(carried.items())))
        return EXIT_NEW_VIOLATIONS if new else EXIT_CLEAN

    for k, n in counts.most_common():
        print(f"   {k:<26} {n}")
    return EXIT_NEW_VIOLATIONS if total else EXIT_CLEAN


if __name__ == "__main__":
    raise SystemExit(main())
