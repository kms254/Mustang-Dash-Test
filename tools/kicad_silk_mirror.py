#!/usr/bin/env python3
"""Mirror a board's edited footprints back into their .pretty libraries.

    python tools/kicad_silk_mirror.py BOARD.kicad_pcb            # report
    python tools/kicad_silk_mirror.py BOARD.kicad_pcb --apply

A placed footprint is a full COPY of its library definition (CONCEPTS.md,
"Embedded Footprint Copy"). Editing the board copies alone -- which is what
kicad_silk_trim.py does -- leaves the two disagreeing, and DRC's
lib_footprint_mismatch says so: 0 before the silk trim, 83 after. A
library-only fix changes nothing that gets fabricated; a board-only fix is
reverted by the next "Update Footprints from Library". Both halves, or neither.

The transform is the reason this is a tool and not a copy: library geometry is
footprint-local while the placed instance is translated and rotated. Rather than
invert that by hand, the instance is cloned, moved to the origin and unrotated,
and handed to pcbnew.FootprintSave(), which writes the local form.

Only the FIRST instance of each library id is mirrored. Instances of the same
type share their library definition, so writing several would be writing the
same file repeatedly -- and if two instances of one type differ, that is a
finding, not something to silently pick a winner for. This reports it.
"""
from __future__ import annotations

import argparse
import collections
import sys
from pathlib import Path

import pcbnew

_KEEPALIVE: list = []


def silk_signature(fp):
    """Order-independent fingerprint of a footprint's silk geometry, local coords."""
    sig = []
    for g in fp.GraphicalItems():
        if g.GetLayer() not in (pcbnew.F_SilkS, pcbnew.B_SilkS):
            continue
        if g.GetClass() != "PCB_SHAPE":
            continue
        s, e, c = g.GetStart(), g.GetEnd(), g.GetCenter()
        pos = fp.GetPosition()
        sig.append((g.ShowShape(), g.GetWidth(),
                    s.x - pos.x, s.y - pos.y, e.x - pos.x, e.y - pos.y,
                    c.x - pos.x, c.y - pos.y))
    return tuple(sorted(sig))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board")
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    board = pcbnew.LoadBoard(args.board)
    root = Path(args.board).parent

    by_fpid = collections.defaultdict(list)
    for fp in board.GetFootprints():
        by_fpid[fp.GetFPIDAsString()].append(fp)

    written, skipped, divergent = 0, 0, []
    for fpid, insts in sorted(by_fpid.items()):
        if ":" not in fpid:
            skipped += 1
            continue
        nick, name = fpid.split(":", 1)
        libdir = root / (nick + ".pretty")
        if not libdir.is_dir():
            skipped += 1
            continue

        sigs = {silk_signature(f) for f in insts}
        if len(sigs) > 1:
            divergent.append((fpid, len(insts), len(sigs)))
            continue

        src = insts[0]
        clone = pcbnew.Cast_to_FOOTPRINT(src.Duplicate(False))
        _KEEPALIVE.append(clone)
        clone.SetOrientationDegrees(0.0)
        clone.SetPosition(pcbnew.VECTOR2I(0, 0))
        if args.apply:
            pcbnew.FootprintSave(str(libdir), clone)
        written += 1

    print("library dir      : %s" % root)
    print("types mirrored   : %d" % written)
    print("types skipped    : %d  (no .pretty beside the board)" % skipped)
    if divergent:
        print("\nDIVERGENT -- instances of one type whose silk differs, NOT mirrored:")
        for fpid, n, s in divergent:
            print("    %-58s %d instances, %d distinct" % (fpid, n, s))
        print("  Pick the intended geometry by hand; a tool cannot know which is right.")
    if not args.apply:
        print("\n(report only -- pass --apply to write)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
