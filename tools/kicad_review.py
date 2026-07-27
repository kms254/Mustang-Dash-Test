#!/usr/bin/env python3
"""Run automated design review over a converted KiCad board, and say whether to trust it.

Drives kicad-happy's analyzers. They parse KiCad S-expressions directly and need
neither KiCad installed nor anything on PATH, so this runs anywhere Python 3.10+
does -- that independence is the point, and R12 asserts it.

Calibration, and why it has three outcomes rather than two
----------------------------------------------------------
A review tool's findings are worth nothing until it has reproduced a defect we
already know is there. Board3 has one: the USB VBUS bulk capacitance that
violates the USB-C inrush limit. So the harness looks for that first and refuses
to vouch for anything else until it appears.

The complication is that the known defect is a SCHEMATIC-level fact -- it is
about capacitor values and their position in the power path. A PCB-only run
cannot see it, and reporting "uncalibrated" there would blame the reviewer for
input it was never given. So the outcomes are:

  CALIBRATED    expected defect reachable in the given input, and found
  MISSED        expected defect reachable in the given input, and NOT found
                -> the reviewer is unreliable; do not act on its other findings
  UNREACHABLE   the input cannot contain the expected defect (no schematic)
                -> says nothing about the reviewer; get the schematic

Only MISSED is a verdict about the tool. Collapsing it with UNREACHABLE would
have us discard a working reviewer because R2 has not landed yet.

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

# The known defect the reviewer must independently rediscover before its other
# findings are believed.
#
# Matching must be loose enough to accept the reviewer's own phrasing and tight
# enough not to fire on a coincidence. The first version of this matched any
# finding containing "vbus" and duly reported CALIBRATED against two info-level
# trace-width reports on nets *named* VBUS -- vouching for the reviewer on a
# net-name collision, the exact failure this gate exists to prevent.
#
# So a match now needs a term from EVERY group (the defect concept and its
# subject), and must clear `min_severity`: a real inrush violation is never
# filed as info.
CALIBRATION = {
    "name": "USB VBUS inrush (bulk capacitance exceeds the USB-C 10 uF limit)",
    "needs": "schematic",
    "all_groups": (
        ("inrush", "bulk cap", "bulk capacit", "capacitance"),
        ("vbus", "vusb", "usb-c", "usb c"),
    ),
    "excluded_severities": ("info", "note", "debug"),
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


def run_pcb_analysis(board: Path, scripts: Path) -> dict:
    """Return the analyzer's JSON verbatim. Never re-derive findings here."""
    proc = subprocess.run(
        [sys.executable, str(scripts / "analyze_pcb.py"), str(board), "--compact"],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0 and not proc.stdout.strip():
        raise ReviewError(
            f"analyze_pcb.py failed ({proc.returncode}): {proc.stderr.strip()[:500]}"
        )
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise ReviewError(f"analyzer produced non-JSON output: {exc}") from exc


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


def calibrate(findings: list[dict], schematic_present: bool) -> tuple[str, str]:
    """Return (verdict, reason). See the module docstring for the three outcomes."""
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
        result = run_pcb_analysis(target, scripts)
    except ReviewError as exc:
        print(f"review failed: {exc}", file=sys.stderr)
        return EXIT_ERROR

    findings = result.get("findings", [])
    project = target if target.is_dir() else target.parent
    schematic_present = any(project.glob("*.kicad_sch"))

    verdict, reason = calibrate(findings, schematic_present)

    by_sev: dict[str, int] = {}
    for f in findings:
        sev = str(f.get("severity", "unknown")).lower()
        by_sev[sev] = by_sev.get(sev, 0) + 1

    report = {
        "target": str(target),
        "schematic_analyzed": schematic_present,
        "calibration": {
            "verdict": verdict,
            "reason": reason,
            "expected": CALIBRATION["name"],
        },
        "finding_count": len(findings),
        "by_severity": by_sev,
        "findings": findings,
    }

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(f"target      : {target}")
    print(f"schematic   : {'analyzed' if schematic_present else 'ABSENT'}")
    print(
        f"findings    : {len(findings)}  "
        + ", ".join(f"{k}={v}" for k, v in sorted(by_sev.items()))
    )
    print(f"calibration : {verdict}")
    print(f"              {reason}")

    if verdict == "MISSED":
        return EXIT_MISSED_CALIBRATION
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
