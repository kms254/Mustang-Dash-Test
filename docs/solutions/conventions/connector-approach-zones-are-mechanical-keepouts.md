---
title: Connector approach zones are mechanical keepouts — DRC cannot see cables
date: 2026-07-30
category: conventions
module: kicad/board3
problem_type: convention
component: tooling
severity: high
applies_when:
  - "Placing any component near a cable-mating connector (FFC/ZIF, USB, barrel, headers)"
  - "Reviewing placement diffs around board-edge connectors"
  - "Automated or scripted placement that optimizes purely on copper clearances"
tags: [kicad, ffc, zif, connector, mechanical, keepout, placement, drc-blind-spot]
---

# Connector approach zones are mechanical keepouts — DRC cannot see cables

## Context

During Board3's bulk-cap work, C67 (an 0805) was placed in the open strip
between FPC2's north face and the board edge — the only pocket the placer's
clearance search could find. Every gate passed: zero new DRC violations,
clean netlist, verified stubs. Kevin caught it on visual review: these are
bottom-contact ZIF connectors whose flat-flex ribbon enters flat from the
north, and the cap sat 0.32 mm from the connector's mouth — squarely inside
the cable's insertion runway (fixed in PR kms254/Mustang-Dash-Test#12's
follow-up commit by relocating C67 as a 0603 to a pocket clear of the path).

The root issue: **every automated check on this board reasons about copper.
Cables, actuator flips, finger access, and insertion travel exist only in
the mechanical world, and no tool in the pipeline models them.**

## Guidance

Standing convention for this board (and any board with mating connectors):

- **The strip between each FPC's cable face and the board edge is cable
  territory — no components, ever.** On Board3 that is the full-width band
  north of FPC1/FPC2/FPC3 up to the outline.
- Generalize per connector type: FFC/ZIF needs a flat runway the full ribbon
  width plus finger/tool access to the actuator; USB and barrel jacks need
  plug-body clearance beyond the outline; headers need vertical clearance.
- **Placement searches must carry a mechanical-exclusion list** alongside
  copper clearances. A search that optimizes only DRC distances will happily
  put parts in cable paths, under actuator arcs, or inside plug envelopes —
  the "best" pocket by copper metrics is often exactly the void that exists
  *because* the cable needs it.
- Model it in CAD where possible: extend the connector footprint's courtyard
  over the approach zone, or draw an explicit keepout rule area. A courtyard
  that covers the runway turns this class of mistake into a visible
  courtyard-overlap violation instead of a silent pass.

## Why This Matters

The failure mode ships: a board can be electrically perfect and unusable
because a cable cannot be inserted, a latch cannot be flipped, or a plug
cannot seat. Catching it costs a human glance at placement renders; missing
it costs a respin.

## When to Apply

- Every placement or relocation within ~5 mm of a mating connector.
- Every review of automated placement output — check the connector
  neighborhoods first, because that is where the search finds its "free"
  space.
- Board bring-up planning: verify cable dress and actuator access on the
  renders before ordering.

## Examples

Before: C67 at (144.5, 86.3), courtyard 0.32 mm from FPC2's north face —
DRC-clean, mechanically blocking. After: C67 as a 0603 at (140.35, 92.70),
southwest of the connector body, all gates unchanged and the runway clear.

## Related

- `docs/solutions/best-practices/pth-pads-are-layer-junctions.md` — the other automation blind spot from the same conversion
- `docs/solutions/conventions/drc-clean-and-measured-is-not-assemblable.md` — the founding member of this family: green copper gates do not certify a buildable product
