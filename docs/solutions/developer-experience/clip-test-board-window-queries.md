---
title: Never window-filter board geometry queries by endpoint containment — clip-test the segment
date: 2026-07-29
category: developer-experience
module: kicad/board3
problem_type: developer_experience
component: tooling
severity: high
applies_when:
  - "Dumping or querying board copper inside a coordinate window via pcbnew"
  - "Building obstacle maps for routing, placement, or clearance work"
  - "Any spatial filter over segments, wires, or edges (schematic or PCB)"
tags: [pcbnew, geometry, query, window, clip, liang-barsky, routing]
---

# Never window-filter board geometry queries by endpoint containment — clip-test the segment

## Context

While routing Board3's TT2 net (PR kms254/Mustang-Dash-Test#11), window dumps
of the form "print every track with an endpoint inside x∈[a,b], y∈[c,d]" were
used to build the obstacle picture. A track whose endpoints BOTH lie outside
the window is invisible to that filter even when it passes straight through
the window. Board3's `VBUS_SENSE` Inner1 track spans the board's full width
(y≈95.0, x≈54–110): both endpoints sat outside every window queried, so it
was absent from two separate dumps — and three successive routing iterations
laid copper directly across it, each failing DRC for a reason the "complete"
map said could not exist.

This is the same failure family as the repo's standing "read board facts
through `pcbnew`, never regex" rule: a query that looks exhaustive but has a
blind spot produces confident wrong conclusions, which cost far more than the
query saved.

## Guidance

Filter segments by **segment-vs-window intersection**, not endpoint
containment. A cheap exact test is Liang-Barsky clipping:

```python
def seg_hits_win(sx, sy, ex, ey, X1, X2, Y1, Y2):
    if max(sx, ex) < X1 or min(sx, ex) > X2 or max(sy, ey) < Y1 or min(sy, ey) > Y2:
        return False
    dx, dy = ex - sx, ey - sy
    t0, t1 = 0.0, 1.0
    for p, q in ((-dx, sx - X1), (dx, X2 - sx), (-dy, sy - Y1), (dy, Y2 - sy)):
        if p == 0:
            if q < 0:
                return False
        else:
            r = q / p
            if p < 0:
                if r > t1: return False
                if r > t0: t0 = r
            else:
                if r < t0: return False
                if r < t1: t1 = r
    return True
```

Vias and pads are points/rects, so containment (inflated by their radius) is
fine for them — the trap is specifically **segments**, whose extent between
endpoints the naive filter ignores. Long power/sense rails, bus serpentines,
and inner-layer rivers are exactly the objects most likely to cross a local
window with both endpoints far away — and exactly the objects a route most
needs to know about.

Also dump **footprint pads**, not just tracks and vias, when mapping
obstacles: the same session separately missed an FPC connector's pad row
because the dump enumerated only `GetTracks()`.

## Why This Matters

The blind spot selects for the worst possible victims: the longest tracks on
the board. Each miss produced a route derived with exact clearance math that
was provably correct against an incomplete world — the errors surfaced only
at the DRC gate, one rework iteration per miss. The cost asymmetry is stark:
the clip test is ~15 lines once; each miss cost a full derive-lay-verify
cycle.

## When to Apply

- Every windowed geometry dump in routing/placement scripts (`pcbnew`,
  EasyEDA bridge, any EDA scripting).
- Building obstacle inventories before deriving track paths by hand.
- Reviewing someone else's board query script: search for `inwin(sx, sy) or
  inwin(ex, ey)` shapes — that pattern is the bug.

## Examples

Before (the bug — both dumps in the session used this):

```python
if inwin(sx, sy) or inwin(ex, ey):   # misses through-tracks entirely
    print(track)
```

After (the fix used for the final, successful map):

```python
if seg_hits_win(sx, sy, ex, ey, X1, X2, Y1, Y2):
    print(track)
```

## Related

- CLAUDE.md — "Never window-filter board dumps by endpoint containment" (compressed version) and "Read board facts through `pcbnew`, never regex" (same failure family)
- `docs/solutions/tooling-decisions/freerouting-headless-integration-for-kicad.md` — the routing session this bug shaped
- `tools/kicad_shove.py` — uses exact segment math for the same reason (its `GetEffectiveShape().Collide()` lesson is the DRC-side sibling of this query-side rule)
