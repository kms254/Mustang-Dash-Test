---
title: "Window-filter board geometry by shape intersection, never by an object's reference point"
date: 2026-07-29
last_updated: 2026-08-10
category: developer-experience
module: kicad/board3
problem_type: developer_experience
component: tooling
severity: high
applies_when:
  - "Dumping or querying board copper inside a coordinate window via pcbnew"
  - "Building obstacle maps for routing, placement, or clearance work"
  - "Any spatial filter over segments, wires, or edges (schematic or PCB)"
  - "Any spatial filter over pads, footprints, or other objects that have extent"
  - "Substituting any simpler proxy shape for a real one — a circle for a slot, a box for a polygon"
  - "Measuring clearance to the board edge or any stroked outline — the datum is the stroke CENTERLINE (the manufactured cut), never the artwork's bounding box"
tags: [pcbnew, geometry, query, window, clip, liang-barsky, routing, pads, bounding-box, slots, edge-cuts, board-edge, datum]
---

# Window-filter board geometry by shape intersection, never by an object's reference point

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

**Second incident, 2026-08-02 (U49 — H2's Tag-Connect debug header).** Routing
`/SWO` out of `U1.130`, a window dump enumerated pads by testing whether
`pad.GetPosition()` fell inside the window. `C6` — a 100 nF decoupling cap at
origin `(151.294, 123.079)` — sat outside, so it never appeared. Its GND pad
spans `x 150.844..151.744, y 121.980..122.779` and reaches well into the
window, and `/SWO` was routed straight through it.

The instructive part is that this doc *caused* it. The rule above had been
generalised as "clip segments," and the paragraph below explicitly excused pads
from the treatment. Filtering pads by centre was therefore not an oversight —
it was the documented guidance, followed correctly.

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

Long power/sense rails, bus serpentines, and inner-layer rivers are exactly
the objects most likely to cross a local window with both endpoints far away
— and exactly the objects a route most needs to know about.

Also dump **footprint pads**, not just tracks and vias, when mapping
obstacles: the same session separately missed an FPC connector's pad row
because the dump enumerated only `GetTracks()`.

### Every object type needs its own test — and none of them is "is the origin inside?"

> **Corrected 2026-08-02.** This doc previously said "vias and pads are
> points/rects, so containment (inflated by their radius) is fine for them —
> the trap is specifically segments." **The pad half of that was wrong**, and
> it cost a second incident (see Context). A pad is not a point: it has extent,
> and `pad.GetPosition()` is a *reference point*, not the shape. The real rule
> is not "clip segments"; it is **never filter by a reference point**, whatever
> the object.

| Object | Correct window test | Why the naive test fails |
|--------|--------------------|--------------------------|
| Track segment | Liang-Barsky clip of the segment | Both endpoints can sit outside a window the segment crosses |
| Pad | Bounding-box **overlap** against `pad.GetBoundingBox()` | The centre can sit outside while copper reaches in |
| Footprint | Bounding-box overlap against `fp.GetBoundingBox()` | Same as pads, at part scale |
| Via | Centre ± radius, **on every layer it spans** | A via is a disc on all its layers; a layer-filtered query hides it (see [search every copper layer before placing a via](search-every-copper-layer-before-placing-a-via.md)) |
| **Courtyard** | The **polygon**, never the bounding box | A courtyard is frequently non-rectangular, so a bbox test both over- and under-reports. U59 proposed two replacement vias that a bbox test called clear and a polygon test put inside U1's and U9's courtyards |
| **Slotted drill** | A **capsule** — a segment swept by a circle: half-length `(max−min)/2` along the long axis, radius `min/2`, rotated by the pad's orientation | A slot is not a circle. `max(w,h)/2` as a radius inflates the short axis by the full aspect ratio: DC1's 2.800 × 0.800 mm slot became a 2.8 mm-diameter disc, turning a 0.4 mm half-height into 1.4 mm |
| **Board edge** | Distance to the **Edge.Cuts centerline** (the manufactured cut), derived from the edge segments themselves | `GetBoardEdgesBoundingBox()` is the bbox of the *drawn* artwork, stroke width included — half a line-width outside the cut on every side (0.127 mm at Board3's 0.254 mm edge stroke). It is also a rectangle, so a routed outline's corner arcs make it over-cover at corners. P1/P2 silk clipped to the bbox datum left the DRC count frozen across two fixes (2026-08-10) |

**The bounding box is the right test for a pad or a footprint and the wrong test
for a courtyard**, which is easy to conflate because all three are "an object
with extent". A pad's copper is its bounding box to within a rounding; a
courtyard is an authored polygon that can be any shape. When the question is
"does this land inside a keep-clear region", use the region's own geometry.

```python
# Pads: overlap on the pad's own extent, never containment of its origin.
bb = pad.GetBoundingBox()
px1, py1 = bb.GetLeft() / NM, bb.GetTop() / NM
px2, py2 = bb.GetRight() / NM, bb.GetBottom() / NM
if not (px2 < X1 or px1 > X2 or py2 < Y1 or py1 > Y2):
    report(pad)
```

## Why This Matters

The blind spot selects for the worst possible victims: the longest tracks on
the board. Each miss produced a route derived with exact clearance math that
was provably correct against an incomplete world — the errors surfaced only
at the DRC gate, one rework iteration per miss. The cost asymmetry is stark:
the clip test is ~15 lines once; each miss cost a full derive-lay-verify
cycle.

The pad variant is worse in one specific way: **the more carefully a part is
placed, the more likely it is to be missed.** A decoupling cap is deliberately
pushed hard against the IC it serves, so its body sits just outside the pin's
local window while its pads reach across the boundary. The naive filter is
therefore most blind exactly where the layout is tightest and a wrong route is
most expensive.

Note also what did *not* catch it. The window dump is the map a hand route is
derived from; if the map is wrong, the clearance arithmetic downstream is
flawless and the answer is still wrong. Only DRC caught it, at the end.

### It recurred five more times after this doc was written

Each in a unit that had the rule available, and one of them in a unit that
*cites this doc while breaking it*:

| Unit | The test that was used | Cost |
|---|---|---|
| **U51** | Pad **corners**, with an early `break`, instead of the pad shape | A uniform widening of `/+5V_BARREL` swallowed Q2's gate pad — a short, on the very net the rule warns about. Correct test was `EffectivePolygon` |
| **U53** | A via's **centre** instead of its shape | Claimed "none under a component courtyard"; TP2's solder mask merged with C34's pad and shipped as a real defect until U59 |
| **U59** | A courtyard **bounding box** instead of the polygon | Two proposed replacement vias called clear that were actually inside U1's and U9's courtyards |
| **Silkscreen, 2026-08-06** | A drill's **bounding circle** (`max(w,h)/2`) instead of the slot | Reported silk 0.027 mm *inside* DC1's pad-3 hole. The silk was 1.1 mm clear. The "defect" was the approximation, and it was nearly fixed on the board |
| **P1/P2 silk clip, 2026-08-10** | The board-edge **bounding box** instead of the Edge.Cuts centerline | Two successive clip passes left `silk_edge_clearance` frozen at exactly 12; the fix was aimed 0.127 mm outside the datum DRC measures. The violation text's own "actual N mm" figure exposed the gap |

Four lessons the original write-up did not contain. **A sampled point set is
still a reference point** — U51 tested four corners rather than one centre and
failed anyway, because "test more points" is not the same as "test the shape".
**The rule generalises past window queries** — U51's failure was a clearance
pre-check, not a window dump, and the same defect appeared because the same
substitution was made. And **citing the rule is not applying it**: U53 quotes
this rule in its own spec and then tests a via centre four lines later.

The fourth adds a direction the others never showed. **A proxy shape can
over-cover as easily as under-cover, and then it invents defects instead of
hiding them.** Every earlier recurrence was a false *negative* — an obstacle
missed, discovered later at the DRC gate. The slot-as-circle substitution is a
false *positive*: it manufactured a violation on a board that did not have one,
and the fix very nearly applied was an edit to real silkscreen to satisfy a
measurement error.

That inverts which instinct protects you. A false negative is eventually caught
by something downstream — DRC, the fab, a test. **A false positive is confirmed
by the fix**: you change the artifact, the tool stops complaining, and the loop
closes with the artifact worse and the tool still wrong. The habit that catches
it is the same one either way — test the shape, not a proxy for it — but the
consequence of skipping it is not symmetric, and "the tool found something, so
there was something" is not sound.

The tell here was cheap and was nearly missed: the reported violation sat
**1.1 mm** from the hole edge on a part whose whole slot is 0.8 mm across. When
a finding's magnitude is implausible against the object's own dimensions,
suspect the measurement before the artifact — the same range-check that catches
"footprint-local" coordinates in the hundreds of millimetres.

The fifth (2026-08-10) adds the *datum* variant and its diagnostic. Clipping
P1/P2's flipped silk against the bottom board edge, the limit was computed from
`board.GetBoardEdgesBoundingBox().GetBottom()` = 135.127 mm — but that is the
bbox of the Edge.Cuts *artwork*, stroke included; the manufactured cut is the
stroke **centerline** at y = 135.000. Two successive passes (the second even
correcting for the silk's own half-stroke) left `silk_edge_clearance` frozen at
exactly 12. **A violation count frozen across two different fixes means the fix
is not reaching the checker's datum.** A stationary count has two causes — a
reporting ceiling (see
[a count at the report limit is not a measurement](a-count-at-the-report-limit-is-not-a-measurement.md),
where the number *cannot* move) or a real count aimed past by the fix, as here —
and both are told apart the same way: change what the checker measures and watch
for movement.

The reconciliation instrument is free, because **DRC prints its own measurement
in the violation text** — "silk clearance 0.1500 mm; actual 0.0240 mm". Testing
candidate datums against that figure settles it in one line of arithmetic:

```
Flagged item after pass 2: silk centerline y = 134.849, stroke 0.254
                           → outermost silk edge at 134.849 + 0.127 = 134.976

bbox datum (135.127):       135.127 − 134.976 = 0.151  → "compliant". Contradicts DRC. ✗
centerline datum (135.000): 135.000 − 134.976 = 0.024  → reproduces "actual 0.0240". ✓
```

Only the centerline model reproduces the checker's own number; clipping to
`135.000 − clearance − strokeWidth/2 − ε` cleared all 12 in one pass (merged
with the rotation in PR kms254/Mustang-Dash-Test#36). If your model of the
geometry cannot reproduce the "actual" a checker reports, your datum is wrong,
not the board — the DRC-side twin of
[a large ERC count is a broken instrument](a-large-erc-count-is-a-broken-instrument.md)'s
"the report's own uuid/pos are authoritative; when a computed answer is
uncertain, ask ERC". Note also the proxy error's *scale*: 0.127 mm against a
0.15 mm rule silently consumed 85 % of the clearance budget — a proxy shape is
dangerous in proportion to tolerance/error, not to its absolute size.

## When to Apply

- Every windowed geometry dump in routing/placement scripts (`pcbnew` or any
  EDA scripting; this project's EasyEDA bridge was retired 2026-07-31).
- Building obstacle inventories before deriving track paths by hand.
- Reviewing someone else's board query script. Two patterns are the bug:
  `inwin(sx, sy) or inwin(ex, ey)` for segments, and any range test applied to
  `pad.GetPosition()` / `fp.GetPosition()` for pads and footprints.
- Generalising *any* rule of this shape. The 2026-08-02 recurrence happened
  because the original write-up drew the boundary at "segments" rather than at
  "reference points," and then explicitly cleared the object type that broke
  next. When a rule turns on "this object has extent the filter ignores," check
  every object type against that test before exempting any of them.

## Examples

Before (the bug — both dumps in the 2026-07-29 session used this):

```python
if inwin(sx, sy) or inwin(ex, ey):   # misses through-tracks entirely
    print(track)
```

After (the fix used for the final, successful map):

```python
if seg_hits_win(sx, sy, ex, ey, X1, X2, Y1, Y2):
    print(track)
```

The pad recurrence, 2026-08-02 — same shape, different object:

```python
p = pad.GetPosition()                       # a reference point, not the shape
if X1 <= p.x / NM <= X2 and Y1 <= p.y / NM <= Y2:   # C6 invisible
    print(pad)
```

```python
bb = pad.GetBoundingBox()                   # the shape
if not (bb.GetRight() / NM < X1 or bb.GetLeft() / NM > X2 or
        bb.GetBottom() / NM < Y1 or bb.GetTop() / NM > Y2):
    print(pad)                              # C6 appears
```

## The sweep, and why three of four were safe to fix untested (2026-08-03)

A repo-wide sweep after the second incident found the same reference-point
reduction in four places, all in `tools/kicad_fit_telltales.py` — the one-off
fitter from U11. Three are now fixed; one is deliberately not.

The three fixed ones are **pre-filters in front of an exact
`GetEffectiveShape(F_Cu).Collide()` test**, and that is what made them safe to
correct without re-running the fit. A pre-filter's only job is to never drop an
obstacle the exact test would have caught, so **widening one is monotone**: it
can add candidates, never remove them. Worst case the script runs slower.

- tracks — was `min(hypot(start), hypot(end)) > 12 mm`, now segment distance
- pads — was `hypot(pad.GetPosition() - centre)`, now `GetBoundingBox()` distance
- footprints — was `fp.GetPosition()`, now `GetBoundingBox()`; the highest-risk
  of the three, since a QFP or FPC courtyard reaches far past its origin

The fourth is the stale-stub matcher, which reduces each pad to its centre and
so lets a track landing on the far edge of a wide pad survive. It is **left as a
recorded hazard**, because the same widening is *not* monotone there: that test
gates **deletion** on a same-net match, so making it more inclusive deletes more
copper — which is exactly how the file's own comment records orphaning C30's
`/+5V`. Correcting it means testing the endpoint against the pad's real shape
*and* re-verifying the fit, which is a different piece of work.

Two lessons worth carrying:

- **An exact predicate behind a lossy filter is only as good as the filter.**
  That file had the correct conflict test all along and still misjudged, because
  the exact test only ever saw what the sloppy filter admitted.
- **Whether a fix is safe to apply untested depends on its monotonicity, not its
  size.** All four sites are the same one-line bug. Three could be corrected
  blind; the fourth could not, and the difference is only which direction the
  error pushes — toward seeing more, or toward deleting more.

Two adjacent gaps found by the same sweep:

- [Search every copper layer before placing a via](search-every-copper-layer-before-placing-a-via.md)
  — its prescribed `viable()` obstacle model iterates tracks only; pads are
  absent from the obstacle set entirely, so a via placed by that recipe can land
  in a pad it never saw.
- `tools/kicad_handroute.py` deletion semantics (both track endpoints inside the
  bbox, a via when its centre is) read like this bug but are **not** — for
  *deletion* the conservative rule is deliberate, since a segment merely passing
  through must survive. The distinction worth holding: reference-point
  containment is wrong for *discovery*, and right for *narrow, conservative
  selection*.

The good precedent to copy is `tools/kicad_shove.py:163-198`, which stores pads
by all four `GetBoundingBox()` edges and hashes them into every spatial bucket
they touch — structurally the overlap test this doc argues for. (The same file
is also the repo's one live `GetBoardEdgesBoundingBox()` caller — line 119, an
edge margin inset 0.45 mm — so its pad hashing is the precedent to copy while
its edge datum inherits the board-edge row's caveat: the effective margin
under-delivers by half the edge stroke, and the rectangle ignores the corner
arcs. Conservative enough there to leave, but do not copy that half.)

## Related

- CLAUDE.md — "Never window-filter board dumps by an object's reference point" (compressed version) and "Read board facts through `pcbnew`, never regex" (same failure family). **Quote that heading exactly.** It originally read "by endpoint containment", and the narrower wording is what let the rule be read as segments-only and recur on pads; an earlier revision of this doc reproduced the superseded phrasing in this very line
- `docs/solutions/tooling-decisions/freerouting-headless-integration-for-kicad.md` — the routing session this bug shaped
- `tools/kicad_shove.py` — uses exact segment math for the same reason (its `GetEffectiveShape().Collide()` lesson is the DRC-side sibling of this query-side rule)
- [Search every copper layer before placing a via](search-every-copper-layer-before-placing-a-via.md) — the via row of the table above; a via is a disc on every layer it spans, so a layer-filtered query hides it
- [a tool's finding can be a property of the tool](a-tools-finding-can-be-a-property-of-the-tool.md) — the 2026-08-06 recurrence is an instance of it: the "defect" was in the measuring script, and the range-check that would have caught it is the same one that catches a 300 mm "footprint-local" coordinate
- `tools/handroutes/u49-h2-tag-connect.json` — the route derived after the correction; its `$why` blocks record the clearances the corrected map produced
- [a count at the report limit is not a measurement](a-count-at-the-report-limit-is-not-a-measurement.md) — the complementary stationary count: there the number *cannot* move (a reporting ceiling); in the 2026-08-10 recurrence it truthfully *did not* move because two fixes never reached the checker's datum. Same observable, opposite diagnosis, same disambiguation: change what the checker measures and watch for movement
- [a large ERC count is a broken instrument](a-large-erc-count-is-a-broken-instrument.md) — its "ask ERC / the report's own numbers are authoritative" mechanic is the ERC-side sibling of reading DRC's "actual N mm" as the datum oracle
