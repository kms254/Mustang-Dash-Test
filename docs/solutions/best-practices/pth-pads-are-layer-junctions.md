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
