#!/usr/bin/env python3
"""Run automated design review over a converted KiCad board, and say whether to trust it.

Drives kicad-happy's analyzers. They parse KiCad S-expressions directly and need
neither KiCad installed nor anything on PATH, so this runs anywhere Python 3.10+
does -- that independence is the point, and R12 asserts it.

Calibration, and why it has four outcomes rather than two
---------------------------------------------------------
A review tool's findings are worth nothing until it has reproduced a defect we
already know is there, so the harness looks for one first and refuses to vouch
for anything else until it appears.

Two things can go wrong that are NOT the reviewer's fault: the input may not be
able to contain the defect (a PCB-only run cannot see a schematic fact), and the
defect may simply have been fixed. Both would otherwise read as failure. So the
outcomes are:

  CALIBRATED    expected defect reachable in the given input, and found
  MISSED        expected defect reachable in the given input, and NOT found
                -> the reviewer is unreliable; do not act on its other findings
  UNREACHABLE   the input cannot contain the expected defect (no schematic)
                -> says nothing about the reviewer; get the schematic
  STALE         the expected defect is no longer IN the design -- somebody fixed
                it -> says nothing about the reviewer; repoint the fixture

STALE was added on 2026-07-27 after the gate spent a whole unit crying wolf. Its
probe was the BTN1-4 single-pin nets, U2 repaired them, and from then on every
run reported MISSED -- "the reviewer is unreliable; do not act on its other
findings" -- about a reviewer that was working perfectly. A calibration fixture
that cannot notice its own defect being fixed turns every later success into a
false alarm, and a gate that is always red is a gate everybody waves through.

So a probe now carries a `presence` check against the design files themselves,
evaluated independently of what the reviewer reported. Only MISSED is a verdict
about the tool; UNREACHABLE is about the input and STALE is about the fixture.

Usage:

  python tools/kicad_review.py kicad/board3/board3.kicad_pcb
  python tools/kicad_review.py kicad/board3/ --json report.json
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

# kicad-happy is cloned beside this repo rather than vendored -- it is a
# third-party analyzer we track upstream, not project code. KICAD_HAPPY
# overrides for a different checkout.
DEFAULT_HAPPY = Path.home() / "Code" / "kicad-happy"

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_MISSED_CALIBRATION = 3  # distinct: the reviewer failed, not the harness
EXIT_STALE_CALIBRATION = 4   # distinct again: the FIXTURE failed, not either

# The known defect the reviewer must independently rediscover before its other
# findings are believed.
#
# Matching must be loose enough to accept the reviewer's own phrasing and tight
# enough not to fire on a coincidence. An early version matched any finding
# containing "vbus" and duly reported CALIBRATED against two info-level
# trace-width reports on nets *named* VBUS -- vouching for the reviewer on a
# net-name collision, the exact failure this gate exists to prevent. So a match
# needs a term from EVERY group, and must clear `excluded_severities`.
#
# A calibration case must be a defect independently CONFIRMED to exist, and it
# must be one nobody is about to fix. Two previous probes failed that second
# test: the USB VBUS inrush case turned out not to exist at all (VBUS carries a
# single 100 nF; the 220 uF bulk sits on +5V behind the ideal-diode ORing), and
# the BTN1-4 single-pin nets were real but got repaired by U2.
#
# Repointed 2026-07-27. The previous probe (BTN1-4 single-pin nets) was fixed by
# U2, which is exactly the event `presence` now detects.
#
# This defect is independently confirmed, which is what a calibration case needs:
# KiCad's own DRC reports ten copper_edge_clearance violations against USBC1's
# polygon and pads, so the overhang exists whether or not kicad-happy sees it.
# Two tools, two codebases, same conclusion.
CALIBRATION = {
    "name": "USBC1's courtyard overhangs the board edge (0.37 mm)",
    "needs": "pcb",
    "all_groups": (
        ("usbc1",),
        ("courtyard", "board edge", "board outline"),
    ),
    "excluded_severities": ("info", "note", "debug"),
    "presence": {
        # Cheap, KiCad-free evidence that the defect is still in the design. If
        # USBC1 is ever moved to fix the overhang, these strings stop matching
        # and the verdict becomes STALE instead of blaming the reviewer.
        "in": "board",
        "contains": ["USB-C_SMD-TYPE-C-31-M-12_1", "(at 30 100 -90)"],
        "confirmed_by": "KiCad DRC, 10 copper_edge_clearance violations at USBC1 "
                        "(2026-07-27)",
    },
}

# kicad-happy findings known to be wrong about THIS board, with the evidence.
# Annotated rather than suppressed: a reviewer you silently edit is a reviewer
# you have stopped reading.
KNOWN_FALSE_POSITIVES = {
    "PR-003": "Looks for a single ~120R part across CANH/CANL and cannot see split "
              "termination. U8 and U9 both have 60.4+60.4R with a centre tap and an "
              "independent jumper -- verified in the netlist by U3.",
    "KO-001": "Tests vias against the keepout's BOUNDING BOX, not its outline. The "
              "Inner2 rule areas are L-shaped; the three vias it flags are inside the "
              "bbox and outside the polygon, and KiCad's DRC reports no keepout "
              "violation (verified by point-in-polygon, 2026-07-27).",
    "TV-001": "Its 'via(s) are not tented' clause cannot read KiCad 10's per-via "
              "(tenting (front yes) (back yes)) block, which pcbnew confirms as "
              "TENTING_MODE_TENTED. The via-count half of this rule is trustworthy.",
}


class ReviewError(RuntimeError):
    """Something stopped the review from running at all."""


def find_analyzers() -> Path:
    root = Path(os.environ.get("KICAD_HAPPY", DEFAULT_HAPPY))
    scripts = root / "skills" / "kicad" / "scripts"
    if not (scripts / "analyze_pcb.py").is_file():
        raise ReviewError(
            f"kicad-happy analyzers not found under {root}. "
            "Clone https://github.com/aklofas/kicad-happy or set KICAD_HAPPY."
        )
    return scripts


def _run_analyzer(script: Path, target: Path) -> dict:
    """Return an analyzer's JSON verbatim. Never re-derive findings here.

    A non-zero exit is not failure on its own -- these analyzers use the exit
    code to signal finding counts and severity, and still emit a full report on
    stdout. Only an empty stdout means the run genuinely died.
    """
    proc = subprocess.run(
        [sys.executable, str(script), str(target), "--compact"],
        capture_output=True,
        text=True,
    )
    if not proc.stdout.strip():
        raise ReviewError(
            f"{script.name} produced no output ({proc.returncode}): "
            f"{proc.stderr.strip()[:500]}"
        )
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise ReviewError(f"{script.name} produced non-JSON output: {exc}") from exc


def run_pcb_analysis(board: Path, scripts: Path) -> dict:
    return _run_analyzer(scripts / "analyze_pcb.py", board)


def run_schematic_analysis(schematic: Path, scripts: Path) -> dict:
    return _run_analyzer(scripts / "analyze_schematic.py", schematic)


def find_board_and_schematic(target: Path) -> tuple[Path, Path | None]:
    """Resolve a project directory (or a board path) to its board and schematic.

    KiCad's EasyEDA Pro importer names files after the source project, so the
    paths are long and not stable enough to hardcode -- glob for them instead.
    """
    if target.is_file():
        project = target.parent
        board = target
    else:
        project = target
        boards = sorted(project.glob("*.kicad_pcb"))
        if not boards:
            raise ReviewError(f"no .kicad_pcb found in {project}")
        if len(boards) > 1:
            raise ReviewError(
                f"{len(boards)} .kicad_pcb files in {project} -- name one explicitly: "
                + ", ".join(p.name for p in boards)
            )
        board = boards[0]
    schematics = sorted(project.glob("*.kicad_sch"))
    return board, (schematics[0] if len(schematics) == 1 else None)


def finding_text(finding: dict) -> str:
    """Everything a finding says, flattened, for substring matching."""
    parts = [str(v) for v in finding.values() if isinstance(v, (str, int, float))]
    return " ".join(parts).lower()


def matches_calibration(finding: dict) -> bool:
    """True only when the finding is *about* the known defect, not merely near it."""
    if str(finding.get("severity", "")).lower() in CALIBRATION["excluded_severities"]:
        return False
    blob = finding_text(finding)
    return all(any(term in blob for term in group) for group in CALIBRATION["all_groups"])


def defect_still_present(board: Path, schematic: Path | None) -> bool | None:
    """Is the calibration defect still IN the design? None when unknowable.

    Deliberately independent of the reviewer's output -- the whole point is to
    tell "the reviewer missed it" apart from "somebody fixed it".
    """
    probe = CALIBRATION.get("presence")
    if not probe:
        return None
    target = board if probe.get("in", "board") == "board" else schematic
    if target is None or not target.is_file():
        return None
    text = target.read_text(encoding="utf-8", errors="ignore")
    return all(token in text for token in probe["contains"])


def calibrate(findings: list, schematic_present: bool,
              still_present: bool | None) -> tuple:
    """Return (verdict, reason). See the module docstring for the four outcomes."""
    hit = next((f for f in findings if matches_calibration(f)), None)
    if hit:
        return (
            "CALIBRATED",
            f"reviewer independently reported: {CALIBRATION['name']} "
            f"(finding {hit.get('finding_id', '?')})",
        )
    if CALIBRATION["needs"] == "schematic" and not schematic_present:
        return (
            "UNREACHABLE",
            "the expected defect is a schematic-level fact and no schematic was "
            "analyzed; this is not evidence about the reviewer",
        )
    if still_present is False:
        return (
            "STALE",
            f"the calibration defect is no longer in the design ({CALIBRATION['name']}) "
            "-- the fixture needs repointing at a defect that still exists. This says "
            "nothing about the reviewer.",
        )
    return "MISSED", f"reviewer did not report the known defect: {CALIBRATION['name']}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("target", help="Path to a .kicad_pcb or a project directory")
    ap.add_argument("--json", dest="json_out", help="Write the full report here")
    args = ap.parse_args()

    target = Path(args.target)
    if not target.exists():
        print(f"no such path: {target}", file=sys.stderr)
        return EXIT_ERROR

    try:
        scripts = find_analyzers()
        board, schematic = find_board_and_schematic(target)
        findings = [
            dict(f, source="pcb") for f in run_pcb_analysis(board, scripts).get("findings", [])
        ]
        if schematic:
            findings += [
                dict(f, source="schematic")
                for f in run_schematic_analysis(schematic, scripts).get("findings", [])
            ]
    except ReviewError as exc:
        print(f"review failed: {exc}", file=sys.stderr)
        return EXIT_ERROR

    schematic_present = schematic is not None
    still_present = defect_still_present(board, schematic)
    verdict, reason = calibrate(findings, schematic_present, still_present)

    flagged = 0
    for f in findings:
        note = KNOWN_FALSE_POSITIVES.get(f.get('rule_id'))
        if note:
            f['known_false_positive'] = note
            flagged += 1

    by_sev: dict[str, int] = {}
    for f in findings:
        sev = str(f.get("severity", "unknown")).lower()
        by_sev[sev] = by_sev.get(sev, 0) + 1

    by_source: dict[str, int] = {}
    for f in findings:
        by_source[f["source"]] = by_source.get(f["source"], 0) + 1

    report = {
        "target": str(target),
        "board": str(board),
        "schematic": str(schematic) if schematic else None,
        "schematic_analyzed": schematic_present,
        "by_source": by_source,
        "calibration": {
            "verdict": verdict,
            "reason": reason,
            "expected": CALIBRATION["name"],
            "defect_still_present": still_present,
            "confirmed_by": CALIBRATION.get("presence", {}).get("confirmed_by"),
        },
        "known_false_positives_annotated": flagged,
        "finding_count": len(findings),
        "by_severity": by_sev,
        "findings": findings,
    }

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(f"board       : {board.name}")
    print(f"schematic   : {schematic.name if schematic else 'ABSENT'}")
    print(
        f"findings    : {len(findings)}  "
        + ", ".join(f"{k}={v}" for k, v in sorted(by_sev.items()))
        + "  [" + ", ".join(f"{k}={v}" for k, v in sorted(by_source.items())) + "]"
    )
    print(f"calibration : {verdict}")
    print(f"              {reason}")
    if flagged:
        print(f"annotated   : {flagged} finding(s) matched a known false positive "
              f"({', '.join(sorted(KNOWN_FALSE_POSITIVES))}) -- see the JSON report")

    if verdict == "MISSED":
        return EXIT_MISSED_CALIBRATION
    if verdict == "STALE":
        return EXIT_STALE_CALIBRATION
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
