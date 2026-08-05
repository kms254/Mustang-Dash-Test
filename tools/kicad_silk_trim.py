#!/usr/bin/env python3
"""Trim silkscreen lines back from solder-mask apertures.

    python tools/kicad_silk_trim.py BOARD.kicad_pcb            # report
    python tools/kicad_silk_trim.py BOARD.kicad_pcb --apply    # edit in place

Why this exists
---------------
JLCPCB wants 0.15 mm between silkscreen and a pad. Board3's imported footprints
draw body outlines straight past their pads, so 199 silk objects sit 0.031 to
0.1495 mm away -- every one a near miss, none an actual overlap.

Nothing caught it. The board's own .kicad_pro carries `min_silk_clearance: 0.15`
and `silk_over_copper: warning`, and KiCad runs NO silk test at all unless a
`silk_clearance` rule exists in the .kicad_dru -- so a setting that looks
configured was enforcing nothing, and DRC reported 0 while the fab reported 81.
The gerbers already pass --subtract-soldermask, so the fab clips the ink and the
board is buildable; what is wrong is the design, which claims markings it will
not get.

Trimming, not moving
--------------------
The outline is cut where it runs past a pad, leaving the surviving pieces where
they are. Moving it outward would keep it continuous at the cost of drawing a
body larger than the part -- which is what the assembler reads, and this board is
dense enough that courtyards already touch. Trimming also makes the design match
what --subtract-soldermask produces, instead of diverging from it.

Method
------
The keepout is each pad's mask aperture (its effective polygon; this board's
solder-mask expansion is 0) inflated by the clearance plus half the silk line
width, because the constraint is edge-to-edge and the stored geometry is a
centreline. Each line is walked in fine steps, exact point-to-polygon distance
at every step, and every blocked/clear boundary is refined by bisection to
0.1 um. `GetEffectiveShape().Collide()` is deliberately not used -- CLAUDE.md
records it under-covering segment midpoints, which is exactly this query.

Only `Line` shapes are trimmed. Circles, arcs and polygons need angular
clipping and are reported, not touched -- and a polygon on silk is usually a
polarity or pin-1 marker, where cutting destroys the meaning the marker exists
to carry.
"""
from __future__ import annotations

import argparse
import collections
import math
import sys

import pcbnew

NM = 1e6
CLEARANCE_MM = 0.15
# GetEffectivePolygon() returns a polygon INSCRIBED in a round pad, so its
# corners sit inside the true arc and every distance measured against it runs a
# few microns optimistic. Trimming to exactly 0.15 mm therefore left residual
# violations of ~3.8 um -- correct arithmetic against a slightly wrong shape.
# Cut this much further so the approximation cannot decide the outcome.
MARGIN_MM = 0.02
STEP_NM = 5_000          # 5 um walk
REFINE_NM = 100          # bisect to 0.1 um
MIN_KEEP_NM = 200_000    # drop surviving stubs under 0.2 mm -- ink noise

_KEEPALIVE: list = []    # removed items must outlive SaveBoard (SWIG lifetime)


def poly_points(pad, layer) -> list[list[tuple[int, int]]]:
    """Outline point lists of a pad's effective polygon, in board coordinates.

    GetEffectivePolygon() takes a layer in KiCad 10 and raises without one. Do
    NOT wrap this in a bare except: an earlier revision did, every pad silently
    returned no rings, and the tool cheerfully reported zero work to do on a
    board with 199 violations. A geometry helper that returns "nothing here" on
    error is indistinguishable from one that found nothing.
    """
    ps = pad.GetEffectivePolygon(layer)
    out = []
    for i in range(ps.OutlineCount()):
        o = ps.Outline(i)
        pts = [(o.CPoint(j).x, o.CPoint(j).y) for j in range(o.PointCount())]
        if len(pts) >= 3:
            out.append(pts)
    return out


def dist_point_poly(px: float, py: float, rings) -> float:
    """Exact distance from a point to a polygon (0 when inside)."""
    best = float("inf")
    for pts in rings:
        inside = False
        n = len(pts)
        for i in range(n):
            ax, ay = pts[i]
            bx, by = pts[(i + 1) % n]
            # edge distance
            dx, dy = bx - ax, by - ay
            if dx or dy:
                t = ((px - ax) * dx + (py - ay) * dy) / float(dx * dx + dy * dy)
                t = 0.0 if t < 0 else (1.0 if t > 1 else t)
                cx, cy = ax + t * dx, ay + t * dy
            else:
                cx, cy = ax, ay
            d = math.hypot(px - cx, py - cy)
            if d < best:
                best = d
            # crossing test
            if (ay > py) != (by > py) and px < (bx - ax) * (py - ay) / (by - ay) + ax:
                inside = not inside
        if inside:
            return 0.0
    return best


def blocked_at(t: float, sx, sy, ex, ey, keepouts) -> bool:
    px, py = sx + (ex - sx) * t, sy + (ey - sy) * t
    for rings, limit in keepouts:
        if dist_point_poly(px, py, rings) < limit:
            return True
    return False


def clear_intervals(sx, sy, ex, ey, keepouts):
    """Sub-intervals of the segment that clear every keepout."""
    length = math.hypot(ex - sx, ey - sy)
    if length == 0:
        return [(0.0, 1.0)] if not blocked_at(0.0, sx, sy, ex, ey, keepouts) else []
    steps = max(2, int(length / STEP_NM) + 1)
    flags = [blocked_at(i / steps, sx, sy, ex, ey, keepouts) for i in range(steps + 1)]

    def refine(lo, hi):                     # lo/hi are t with differing flags
        want = flags_at_lo = blocked_at(lo, sx, sy, ex, ey, keepouts)
        while (hi - lo) * length > REFINE_NM:
            mid = (lo + hi) / 2
            if blocked_at(mid, sx, sy, ex, ey, keepouts) == want:
                lo = mid
            else:
                hi = mid
        return hi if flags_at_lo else lo

    spans, start = [], None
    for i in range(steps + 1):
        t = i / steps
        if not flags[i] and start is None:
            start = refine((i - 1) / steps, t) if i else 0.0
        elif flags[i] and start is not None:
            spans.append((start, refine((i - 1) / steps, t)))
            start = None
    if start is not None:
        spans.append((start, 1.0))
    return [(a, b) for a, b in spans if (b - a) * length >= MIN_KEEP_NM]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board")
    ap.add_argument("--apply", action="store_true", help="write the board")
    ap.add_argument("--clearance", type=float, default=CLEARANCE_MM)
    ap.add_argument("--margin", type=float, default=MARGIN_MM,
                    help="extra cut to absorb pad-polygon approximation error")
    args = ap.parse_args()

    board = pcbnew.LoadBoard(args.board)
    silk_layers = (pcbnew.F_SilkS, pcbnew.B_SilkS)
    mask_of = {pcbnew.F_SilkS: pcbnew.F_Mask, pcbnew.B_SilkS: pcbnew.B_Mask}

    pads_by_mask = {m: [] for m in mask_of.values()}
    for pad in board.GetPads():
        for m in pads_by_mask:
            if pad.IsOnLayer(m):
                pads_by_mask[m].append(pad)
    poly_cache = {}

    trimmed = deleted = kept = 0
    deferred = collections.Counter()
    per_fp = collections.Counter()
    edits = []

    for fp in board.GetFootprints():
        for g in list(fp.GraphicalItems()):
            lay = g.GetLayer()
            if lay not in silk_layers or g.GetClass() != "PCB_SHAPE":
                continue
            shape = g.ShowShape()
            if shape != "Line":
                # measure whether it is even in violation before reporting it
                deferred[shape] += 1
                continue
            half = g.GetWidth() / 2.0
            limit = (args.clearance + args.margin) * NM + half
            s, e = g.GetStart(), g.GetEnd()
            mask_layer = mask_of[lay]
            keep = []
            for pad in pads_by_mask[mask_layer]:
                key = (id(pad), mask_layer)
                if key not in poly_cache:
                    poly_cache[key] = poly_points(pad, mask_layer)
                rings = poly_cache[key]
                if not rings:
                    continue
                # cheap reject: bounding box of the pad vs the segment box
                bb = pad.GetBoundingBox()
                pad_x1, pad_y1 = bb.GetLeft() - limit, bb.GetTop() - limit
                pad_x2, pad_y2 = bb.GetRight() + limit, bb.GetBottom() + limit
                if max(s.x, e.x) < pad_x1 or min(s.x, e.x) > pad_x2:
                    continue
                if max(s.y, e.y) < pad_y1 or min(s.y, e.y) > pad_y2:
                    continue
                keep.append((rings, limit))
            if not keep:
                continue
            spans = clear_intervals(s.x, s.y, e.x, e.y, keep)
            if len(spans) == 1 and spans[0] == (0.0, 1.0):
                continue
            edits.append((fp, g, spans, s, e))
            per_fp[fp.GetFPIDAsString()] += 1
            if not spans:
                deleted += 1
            else:
                trimmed += 1
                kept += len(spans)

    print(f"board            : {args.board.split('/')[-1]}")
    print(f"lines trimmed    : {trimmed}   (into {kept} pieces)")
    print(f"lines removed    : {deleted}   (no clear span >= {MIN_KEEP_NM/NM} mm)")
    print(f"deferred shapes  : {dict(deferred)}   (circles/arcs/polygons untouched)")
    print("\nby footprint type:")
    for fpid, n in per_fp.most_common():
        print(f"  {n:4d}  {fpid}")

    if not args.apply:
        print("\n(report only -- pass --apply to write)")
        return 0

    for fp, g, spans, s, e in edits:
        for a, b in spans:
            new = g.Duplicate()
            new.SetStart(pcbnew.VECTOR2I(int(s.x + (e.x - s.x) * a),
                                         int(s.y + (e.y - s.y) * a)))
            new.SetEnd(pcbnew.VECTOR2I(int(s.x + (e.x - s.x) * b),
                                       int(s.y + (e.y - s.y) * b)))
            fp.Add(new)
        fp.Remove(g)
        _KEEPALIVE.append(g)

    board.Save(args.board)
    print(f"\nwritten: {args.board}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
