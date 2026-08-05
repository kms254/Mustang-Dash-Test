---
title: PTH pads are layer junctions — audit copper before deleting any through-hole part
date: 2026-07-30
category: best-practices
module: kicad/board3
problem_type: best_practice
component: tooling
severity: high
applies_when:
  - "Deleting or replacing a through-hole part on a routed board"
  - "MOVING a through-hole part, even by a fraction of a pad width"
  - "Converting THT parts to SMD (footprint swap included)"
  - "Diagnosing airwires that appear after a part deletion"
tags: [kicad, pth, tht, airwires, layer-junction, deletion, smd-conversion]
---

# PTH pads are layer junctions — audit copper before deleting any through-hole part

## Context

Board3's THT-to-SMD bulk conversion (PR kms254/Mustang-Dash-Test#12) deleted
six 220 µF through-hole electrolytics. The schematic and placement work
verified clean, yet the board came out with five airwires that took a full
diagnostic session (closed in PR kms254/Mustang-Dash-Test#13). Every one
traced to the same cause: a plated through-hole pad is not just a part
terminal — it is a **via**. It joins every copper layer it touches, and
routed copper on different layers legitimately meets *at* it.

The five specifically: two `/+5V` chains whose front-to-back transitions were
the deleted caps' PTH pads (an F track and a B track met at the pad with no
other junction), and three top-layer GND fill fragments whose only bond to
the rest of the ground system ran through the deleted parts' holes.

## Guidance

Before deleting or footprint-swapping any through-hole part on a routed
board, audit each of its holes as if it were a via being removed:

1. **List the copper that terminates at each PTH pad, per layer.** Tracks on
   two different layers ending at the same pad means the pad is their only
   junction — deleting it opens the net.
2. **Check zone fills.** A fill fragment whose only same-net item is the PTH
   pad loses its inter-layer bond with the part (SMD replacement pads touch
   one layer only; zone spokes bond a pad to its *fragment*, not to the
   world).
3. **Plan replacement vias in the same edit** — a via at or near each
   orphaned junction restores the connection at near-zero cost. Doing it in
   the deletion commit avoids the airwires ever existing.
4. **Gate with connectivity, not just DRC.** The airwire count
   (`GetUnconnectedCount` after a zone refill) is the check that catches
   this; clearance DRC stays green throughout because nothing *collides*.

   **But not on a poured net, which is where most of these junctions live.**
   Four of Board3's five live PTH layer junctions are on `/GND`, which is poured
   on three layers — and there the airwire count returns zero regardless,
   because the pour absorbs the connection. Use the DRC census and
   `starved_thermal` for those, per
   [an airwire count cannot validate copper changes on a poured net](../developer-experience/airwire-counts-cannot-validate-deleting-copper-on-a-poured-net.md).
   This doc's own example is the case where the airwire gate works — the
   fragments were on a *top-layer fill*, not a live pour — which is exactly what
   makes the limitation easy to miss here.

5. **Moving counts as touching, and the test is not "still inside the pad".**
   U56 shifted H1/H3 by 1.0 mm on 2.00 mm pads. Every track end stayed within
   the new pad, so no gate objected — and each one landed *on the pad edge*,
   which is a connection too marginal to leave to a DRC opinion. All four stubs
   were re-laid with their endpoints at the new pad centres. A landing that is
   technically inside and practically on the boundary is a defect that every
   automated check will pass.

## Worked confirmation — U54, a swap that was safe and was proven so

The procedure's value is not only that it catches disasters. U54 swapped SW1–SW4
from the LS5.4 to the LS5.0 land pattern on a fully-routed board, and the audit
is what made that a decision rather than a hope:

> *"Only pad1 (`/BTNn_SW`) and pad4 (`/GND`) are connected on these four; pads 2
> and 3 are unconnected. The pads move 0.2 mm and the pads are 1.6 mm square, so
> an existing track ending at the old centre is still 0.6 mm inside the new pad.
> Nothing needs re-laying."*

SW1.4 and SW2.4 are Top+Bottom junctions — precisely the condition step 1 exists
to find. The audit said the routing survives, and DRC afterwards agreed at 0/0/0.

## Why This Matters

The failure is silent and non-local: the schematic still netlists cleanly,
DRC reports no new violations, and the openings can sit far from the deleted
part (one of Board3's was a 10 mm bottom-layer run two connector-widths
away). Each orphan then costs a diagnosis session; the audit costs minutes.

## When to Apply

- Any THT→SMD conversion, even "pure part swaps".
- Deleting connectors, jumpers, or test points with plated holes.
- Reviewing someone else's deletion diff: grep the removed footprints for
  drilled pads and ask where their layer transitions went.

## Examples

Board3's fixes, for shape (from PR kms254/Mustang-Dash-Test#13): a `/+5V`
junction via at the exact point where an F track and a B track met at a
deleted pad's location, and GND stitching vias dropped into each orphaned
fill fragment (grid-searched for clearance, required to land on the In1
fill so the via actually bonds).

## Related

- `docs/solutions/conventions/drc-clean-and-measured-is-not-assemblable.md` — same family: green gates that don't cover the failure class
- `docs/solutions/conventions/connector-approach-zones-are-mechanical-keepouts.md` — the other blind spot found in the same conversion
