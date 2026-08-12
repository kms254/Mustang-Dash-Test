"""Design-truth compass table for placement audits.

Prints, for every polarized/orientation-bearing part, where pin 1 (or each pad
of a two-pad part, with its net) sits relative to the footprint center, as a
compass direction in the board frame. This is the table a fab placement review
is audited AGAINST: generate it from the board file first, then check one
representative per polarity family in the fab's viewer (method write-up:
docs/solutions/developer-experience/audit-the-fabs-revision-set-before-confirming-placement.md).

Born as session scratch in the U2 fab-viewer investigation (2026-08-10), where
it produced the 13-match/1-outlier sweep that localized JLC's model fault, and
reused for the 8-click placement-confirmation audit (2026-08-11).

Usage:
    python tools/kicad_pin1_truth.py [board.kicad_pcb] [REF ...]

With no REF arguments, prints every footprint that has a pad "1" plus every
two-pad part. Run under an interpreter with pcbnew.
"""

import argparse
from pathlib import Path

import pcbnew

BOARD_REL = "kicad/board3/ProPrj_New Project_2026-07-15_23-14-34_2026-07-27.kicad_pcb"
NM = 1e6


def compass(dx, dy):
    ns = "N" if dy < -0.05 else ("S" if dy > 0.05 else "")
    ew = "E" if dx > 0.05 else ("W" if dx < -0.05 else "")
    return (ns + ew) or "center"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board", nargs="?", default=None)
    ap.add_argument("refs", nargs="*", help="restrict to these references")
    args = ap.parse_args()

    repo = Path(__file__).resolve().parent.parent
    board_path = Path(args.board) if args.board else repo / BOARD_REL
    board = pcbnew.LoadBoard(str(board_path))

    want = set(args.refs)
    for fp in sorted(board.GetFootprints(), key=lambda f: f.GetReference()):
        ref = fp.GetReference()
        if want and ref not in want:
            continue
        pads = list(fp.Pads())
        c = fp.GetPosition()
        numbered = {p.GetNumber(): p for p in pads if p.GetNumber()}
        if "1" in numbered and len(numbered) > 2:
            p = numbered["1"].GetPosition()
            print(f"{ref:8s} rot {fp.GetOrientationDegrees():6.1f}  "
                  f"pin1 -> {compass((p.x - c.x) / NM, (p.y - c.y) / NM)}")
        elif len(numbered) == 2:
            parts = []
            for num in sorted(numbered):
                p = numbered[num].GetPosition()
                parts.append(f"pad{num}({numbered[num].GetNetname()}) "
                             f"{compass((p.x - c.x) / NM, (p.y - c.y) / NM)}")
            print(f"{ref:8s} rot {fp.GetOrientationDegrees():6.1f}  " + "  ".join(parts))


if __name__ == "__main__":
    main()
