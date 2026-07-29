---
title: Search every copper layer before placing a via, because the renders cannot show the layers that will short
date: 2026-07-27
category: developer-experience
module: kicad-routing
problem_type: developer_experience
component: tooling
severity: high
applies_when:
  - "Placing an escape, thermal or stitching via on a board with more than two copper layers"
  - "Adding a via array to a thermal pad, or a via fence along an edge"
  - "Reviewing a board change from committed renders rather than from the board file"
root_cause: incomplete_verification
resolution_type: workflow_improvement
tags: [kicad, pcb, vias, clearance, inner-layers, verification, renders]
---

# Search every copper layer before placing a via

## Context

A through via is a hole through the whole stack, so its clearance constraint is
the **union of every copper layer** — not the two you can see. Board3 hit this
twice in one day, in two different shapes.

**U6 (`4f16bb2`)** placed a USB escape via at `(37.100, 93.918)`. Both outer
layers were clear there — the only copper within 0.6 mm of that point on `F.Cu`
or `B.Cu` belongs to `USB_DM_CONN`, the via's *own* net, because the via sits on
the DM route. One layer down, `/VBUS` runs vertically on `In1.Cu` at
`x = 36.852`, 0.254 mm wide, spanning `y` 89.547 to 95.135. Centre-to-centre
distance 0.248 mm against a 0.6 mm via: copper overlapping copper by 0.179 mm.
A short, on a power rail, invisible from either surface.

**U7 (`afd4c22`)** needed nine thermal vias in U2's 5.20 × 4.65 mm pad at
`(171.590, 107.206)`. Three nets crossed that pad before a single via existed:
`QSPI_IO2` and `QSPI_IO1` on the bottom layer, and `MOSI_R_MCU` cutting `In1.Cu`
corner to corner. An evenly spaced 3×3 grid puts **five of its nine positions**
below the 0.1016 mm clearance rule. The hand-drawn pattern was never a candidate.

This is not an exotic board. Its inner layers are declared `mixed`, not plane,
and they carry real signal:

```text
F.Cu    3307.7 mm    97 nets
In1.Cu   934.1 mm    23 nets
In2.Cu   655.7 mm    20 nets
B.Cu     481.8 mm    19 nets
```

Roughly 1,590 mm of routing across 36 nets lives where you cannot see it.

## Guidance

**Never hand-place a via position. Generate candidates and filter them.** The
method is four constraints applied together, and dropping any one of them
reproduces one of the two failures above:

1. **Test each candidate against every copper layer's copper**, plus keepouts
   and rule areas. Not the outer pair. Not "the layers this net uses" — a
   through via belongs to all of them.
2. **Use the project's real rules**, from `tools/kicad_rules.json`, never the
   board's `.kicad_pro` — the EasyEDA Pro importer wrote factory defaults there
   (clearance 0.2 mm against the real 0.1016).
3. **Enforce a hole-to-hole floor** independently of copper clearance. It is a
   drill constraint, not an etch constraint, and copper clearance does not imply
   it. Board3's floor is `min_hole_to_hole` 0.5 mm.
4. **Treat candidates already accepted in this run as obstacles.** They are not
   in the board file yet, so nothing else will catch them colliding with each
   other.

```python
def viable(cx, cy, via_d, obstacles, accepted, clearance, hole_floor, drill):
    for lay, net, seg in obstacles:            # EVERY copper layer, all nets
        if net == via_net:
            continue
        if dist_point_seg(cx, cy, seg) - via_d / 2 - seg.width / 2 < clearance:
            return False
    for ax, ay in accepted:                    # this run's own vias
        if math.dist((cx, cy), (ax, ay)) - drill < hole_floor:
            return False
    return True
```

Feed it a dense grid over the target region, keep the survivors, then pick from
them — greedy max-spread for a thermal array, nearest-to-ideal for an escape.
The output is a position that is *provably* placeable, which a drawn pattern
never is.

**Do not review via placement from the committed renders.**
`tools/kicad_render.py` commits exactly three views:

```python
VIEWS = (
    ("top.png",    ["--side", "top"]),
    ("bottom.png", ["--side", "bottom"]),
    ("angled.png", ["--side", "top", "--rotate", "-25,0,20"]),
)
```

All three are surface views. `.githooks/pre-commit` refreshes them on every
commit that stages a `.kicad_pcb`, so they are always current and always
incomplete. The project's own visual-verification artifact **structurally cannot
render the inner copper**, which is exactly where the counter-evidence to a via
position lives. An agent or a reviewer looking at these images is looking at a
surface that cannot hold the disproof.

## Why This Matters

Both failure modes are silent and both look fine. The U6 via sat on its own
net's track with clear copper above and below it; nothing in the editor, the
renders, or a reading of the route said otherwise. Only a clearance query
against the inner layers found it.

The failure also scales with how many vias you place at once. One escape via is
one chance to be wrong; a nine-via thermal array is nine, and they interact — a
grid that clears the copper can still violate hole-to-hole against itself. Hand
placement gets less trustworthy exactly as the stakes rise.

`tools/kicad_handroute.py` already records the U6 incident as trap 2 and
prescribes *"Check the inner layers before placing one."* That is the correct
instinct and the wrong instruction: it does not say what checking means, it does
not scale past one via, and it says nothing about hole-to-hole. A search is
reusable; a reminder to be careful is not.

## When to Apply

- Placing any via on a board with inner copper, including escapes, thermal
  arrays, stitching and via fences
- Before believing that "the top and bottom are clear here" means anything
- When reviewing a board change and the only evidence offered is a render
- When a via array is specified as a pattern (3×3, 5-point, 1 mm pitch) — the
  pattern is a starting hypothesis, never the answer

## Examples

**Escape via, U6.** `(37.100, 93.918)` overlapped `In1.Cu` `/VBUS` by 0.179 mm.
The fix moved it 0.453 mm along the same 45° approach to `(37.420, 93.598)`,
where the gap is 0.141 mm against the 0.1016 mm rule. The route's length and
skew were unaffected. A correct position was 0.45 mm away — the cost of the
defect was entirely in not looking.

**Thermal array, U7.** Nine positions from a clearance-aware grid search over
every copper layer, then greedy max-spread:

```text
plain 3x3, even thirds   5 of 9 below the 0.1016 mm clearance rule
searched positions       9 of 9 clear, tightest gap 0.1104 mm (In1.Cu MOSI_R_MCU)
hole-to-hole achieved    1.500 mm, against a 0.500 mm floor
```

Note the tightest constraint is on an inner layer, on a net with no relationship
to U2 at all. `MOSI_R_MCU` is not a flash signal; it merely crosses.

The U7 search lived in a scratch `place_thermal.py` that was never committed, so
the next via array on this board starts from this document rather than from
running code. Promote the search into `tools/` the next time one is needed.

## Related

- [Refill zones before measuring a headlessly routed board](refill-zones-before-measuring-a-headlessly-routed-board.md) — the other way a confident-looking measurement on this board is wrong
- [Migrating a board from EasyEDA Pro to KiCad loses data silently](../integration-issues/easyeda-pro-to-kicad-migration-silent-data-loss.md) — why the clearance rule must come from `tools/kicad_rules.json`
