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
import math
import sys
from pathlib import Path

import pcbnew

_KEEPALIVE: list = []


def silk_signature(fp):
    """Order-independent fingerprint of a footprint's silk geometry, local coords.

    Translation AND rotation must both come off. An earlier revision subtracted
    only the position, so the same library geometry placed at 90 degrees
    fingerprinted differently and every rotated type was reported divergent:
    R0603 read as "31 instances, 4 distinct" and C0805 as "11 instances, 4
    distinct" when both in fact have exactly ONE geometry. That false reading is
    what the silkscreen plan recorded as U0, the blocker that stopped the
    library mirror -- so the tool's own bug was mistaken for a property of the
    board.

    The sign is not a guess. Un-rotating by +angle reproduces the .kicad_mod's
    own coordinates exactly -- 31/31 R0603 and 11/11 C0805, at all four
    rotations -- while -angle matches only the 0 and 180 degree instances. The
    library file is the footprint-local form, so it is the ground truth for
    this convention; re-run that comparison before trusting any change here.
    """
    sig = []
    pos = fp.GetPosition()
    deg = fp.GetOrientationDegrees()
    quarter = round(deg / 90.0)
    if abs(deg - quarter * 90.0) < 1e-6:
        # Exact integer arithmetic for the 90-degree multiples every part on
        # this board uses. Float cos/sin leaves ~1 nm of noise, and coordinates
        # like 0.6605 mm sit exactly on a rounding boundary, which is enough to
        # split one geometry into several groups all over again.
        ca, sa = ((1, 0), (0, 1), (-1, 0), (0, -1))[quarter % 4]

        def loc(p):
            dx, dy = p.x - pos.x, p.y - pos.y
            return (dx * ca - dy * sa, dx * sa + dy * ca)
    else:
        rad = math.radians(deg)
        ca, sa = math.cos(rad), math.sin(rad)

        def loc(p):
            dx, dy = p.x - pos.x, p.y - pos.y
            return (int(round(dx * ca - dy * sa)), int(round(dx * sa + dy * ca)))

    for g in fp.GraphicalItems():
        if g.GetLayer() not in (pcbnew.F_SilkS, pcbnew.B_SilkS):
            continue
        if g.GetClass() != "PCB_SHAPE":
            continue
        if g.ShowShape() in ("Polygon", "Poly"):
            # A polygon's geometry lives in GetPolyShape(); its GetStart() and
            # GetEnd() are not its outline and on this board came back as raw
            # board coordinates, so fingerprinting those produced local deltas
            # of 125-312 mm on parts a few mm across. Every type that reported
            # as divergent was one carrying a polygon -- the polarity bands and
            # pin-1 markers -- and none of them actually differed.
            ps = g.GetPolyShape()
            pts = []
            for i in range(ps.OutlineCount()):
                o = ps.Outline(i)
                pts.append(tuple(loc(o.CPoint(j)) for j in range(o.PointCount())))
            sig.append(("Polygon", g.GetWidth(), g.GetLayer(), tuple(pts)))
            continue
        sig.append((g.ShowShape(), g.GetWidth(), g.GetLayer(),
                    loc(g.GetStart()), loc(g.GetEnd()), loc(g.GetCenter())))
    return tuple(sorted(sig, key=repr))


def group_instances(insts, tol_nm: int):
    """Cluster placements whose local silk agrees to within tol_nm.

    Exact equality is the wrong test. kicad_silk_trim computes its cut points in
    ABSOLUTE board coordinates and rounds to integer nanometres, so the same cut
    applied to two placements of one type lands up to 1 nm apart -- measured, on
    SOIC-8: 26 shapes each, worst matched-shape delta 1 nm. Hashing exact ints
    calls that a divergence and refuses to mirror a type that is, to any
    physical standard, identical. A nanometre is a millionth of a millimetre;
    no fab has any relationship to it.
    """
    groups: list[list] = []
    sigs: list = []
    for fp in insts:
        sig = silk_signature(fp)
        for i, ref in enumerate(sigs):
            if _agree(sig, ref, tol_nm):
                groups[i].append(fp.GetReference())
                break
        else:
            sigs.append(sig)
            groups.append([fp.GetReference()])
    return groups


def _points(entry):
    """Flatten a signature entry's coordinates to a flat point list."""
    if entry[0] == "Polygon":
        return [p for ring in entry[3] for p in ring]
    return list(entry[3:])


def _agree(a, b, tol_nm: int) -> bool:
    """Two signatures describe the same geometry within tol_nm."""
    if len(a) != len(b):
        return False
    for x, y in zip(a, b):
        if x[0] != y[0] or x[2] != y[2]:          # shape kind, layer
            return False
        if abs(x[1] - y[1]) > tol_nm:             # width
            return False
        px, py = _points(x), _points(y)
        if len(px) != len(py):
            return False
        for (ax, ay), (bx, by) in zip(px, py):
            if abs(ax - bx) > tol_nm or abs(ay - by) > tol_nm:
                return False
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--tolerance", type=int, default=1000,
                    help="nanometres below which two placements count as the "
                         "same geometry (default 1000 = 1 um)")
    args = ap.parse_args()

    board = pcbnew.LoadBoard(args.board)
    root = Path(args.board).parent

    by_fpid = collections.defaultdict(list)
    for fp in board.GetFootprints():
        by_fpid[fp.GetFPIDAsString()].append(fp)

    written, skipped, unchanged, divergent = 0, 0, 0, []
    for fpid, insts in sorted(by_fpid.items()):
        if ":" not in fpid:
            skipped += 1
            continue
        nick, name = fpid.split(":", 1)
        libdir = root / (nick + ".pretty")
        if not libdir.is_dir():
            skipped += 1
            continue

        groups = group_instances(insts, args.tolerance)
        if len(groups) > 1:
            divergent.append((fpid, len(insts), len(groups),
                              [sorted(g) for g in groups]))
            continue

        # Write only what actually changed. Saving every type rewrites the
        # uuid of every property in 20-odd .kicad_mod files that no edit
        # touched, burying the real diff in churn.
        lib_fp = pcbnew.FootprintLoad(str(libdir), name)
        if lib_fp is not None:
            _KEEPALIVE.append(lib_fp)
            if _agree(silk_signature(lib_fp), silk_signature(insts[0]), args.tolerance):
                unchanged += 1
                continue

        src = insts[0]
        clone = pcbnew.Cast_to_FOOTPRINT(src.Duplicate(False))
        _KEEPALIVE.append(clone)
        clone.SetOrientationDegrees(0.0)
        clone.SetPosition(pcbnew.VECTOR2I(0, 0))
        # A library definition is a TEMPLATE, so it must not carry the
        # designator of whichever placement happened to be cloned. Left alone,
        # FootprintSave writes Reference "R12" into R0603.kicad_mod and every
        # future placement of that part arrives pre-named R12. Same family as
        # the U57 finding that a footprint swap silently changed a field's
        # visibility: the operation edits more of the part than it advertises.
        clone.SetReference("REF**")
        if args.apply:
            pcbnew.FootprintSave(str(libdir), clone)
        written += 1

    print("library dir      : %s" % root)
    print("types mirrored   : %d" % written)
    print("types unchanged  : %d  (library already matches, not rewritten)" % unchanged)
    print("types skipped    : %d  (no .pretty beside the board)" % skipped)
    if divergent:
        print("\nDIVERGENT -- instances of one type whose silk differs, NOT mirrored:")
        for fpid, n, s, groups in divergent:
            print("    %-58s %d instances, %d distinct" % (fpid, n, s))
            for g in groups:
                print("        %s" % ", ".join(g))
        print("  Pick the intended geometry by hand; a tool cannot know which is right.")
    if not args.apply:
        print("\n(report only -- pass --apply to write)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
