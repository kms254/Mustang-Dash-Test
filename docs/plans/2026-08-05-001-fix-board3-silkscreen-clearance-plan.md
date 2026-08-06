---
artifact_contract: ce-unified-plan/v1
artifact_readiness: implementation-ready
execution: code
title: Bring Board3's silkscreen to JLC's 0.15 mm pad clearance, and gate it
date: 2026-08-05
branch: fix/board3-silkscreen-clearance
---

# Bring Board3's silkscreen to JLC's 0.15 mm clearance, and gate it

## Goal Capsule

JLCPCB's DFM check returns **81 red errors** on Board3 — 50 *silkscreen to pad*
and 31 *silkscreen to hole* — and they are the only red in the report. Every
other category passes or warns.

KiCad's DRC reports **zero**, on the same board, at `--severity-all`, with
`min_silk_clearance: 0.15` and `silk_over_copper: warning` both present in the
project file.

Both statements are true. **KiCad runs no silkscreen test at all unless a
`silk_clearance` rule exists in the `.kicad_dru`** — the project-file settings
are inert on their own. Add one and the same board reports 199
`silk_over_copper` plus 199 `silk_overlap`.

**Done means:** silk clears every mask aperture by 0.15 mm, the rule is in the
`.kicad_dru` so the absolute CI gate enforces it, the fix lives in the footprint
libraries as well as the board's embedded copies, and JLC's DFM returns no red.

## Why this plan exists

The work was scoped as a one-liner — add the rule with the fix — and the
measurement showed it is not. This plan exists because three things turned out
to be true that were not known when the work was requested:

1. **199 objects, not a handful**, spread across the whole board
   (x 28–258 mm), owned by **19 footprint types** plus 18 board-level graphics.
2. **None is an overlap.** The range is 0.031–0.1495 mm against a 0.15 mm
   requirement; the median shortfall is **36 µm**. Nothing prints on a pad
   today.
3. **The fab already clips it.** `tools/kicad_fab.py` passes
   `--subtract-soldermask` (verified: the silk gerber is 303,034 chars with it
   and 74,252 without). So the board is buildable and nothing shorts. What is
   wrong is that the *design* claims markings the board will not receive, and
   the clipped remainder still sits at 0 mm from the mask edge where JLC wants
   0.15 mm.

So this is a marking-quality and design-honesty problem, not a manufacturability
blocker. It is worth doing properly and it is not worth rushing.

## Key Decisions

**Trim, do not move.** Cutting the outline where it passes a pad leaves the
surviving pieces in place. Moving the outline outward keeps it continuous at the
cost of drawing a body larger than the part — which is what the assembler reads
— and Board3 is dense enough that courtyards already touch by design (U1's
decoupling ring, U12/LED6). Trimming also makes the design match what
`--subtract-soldermask` already produces, rather than diverging from it.

**Fix the library and the board.** A placed footprint is a full copy of its
library definition (CONCEPTS.md, *Embedded Footprint Copy*). A library-only fix
changes nothing that gets fabricated; a board-only fix is reverted by the next
*Update Footprints from Library*. Confirmed empirically: trimming the board
copies alone raised `lib_footprint_mismatch` from 0 to 33.

**Deletion is a per-class decision, not a global threshold.** A trial run with a
20 µm safety margin removes **28% of all silk line length**, and takes `R0603`
(31 placements) and `ERJP06F60R4V` to **zero silk**. For chip passives that is
plausibly correct — many vendor libraries carry no silk on an 0603 at all — but
it must be decided per footprint class and recorded, not fallen into by tuning a
constant.

**Polarity and pin-1 markers are never trimmed.** The 8 remaining `Polygon`
violations are on `1N5819WS` and the LED footprints. Cutting a cathode band or a
pin-1 dot destroys the meaning the marker exists to carry; those move, shrink,
or take a recorded exemption.

## What already exists

`tools/kicad_silk_trim.py` (landed with this plan) trims silk **lines** back
from mask apertures. Exact point-to-polygon distance, 5 µm walk, boundaries
refined by bisection to 0.1 µm. It deliberately does not use
`GetEffectiveShape().Collide()`, which CLAUDE.md records as under-covering
segment midpoints — precisely this query.

Measured on a copy: `silk_over_copper` **199 → 68**.

Two traps are already paid for and encoded in its source:

- `GetEffectivePolygon()` takes a layer argument in KiCad 10. An earlier
  revision swallowed the `TypeError`, so every pad returned no rings and the
  tool reported **"0 lines need trimming"** on a board with 199 violations.
- That polygon is *inscribed* in a round pad, so distances measured against it
  run a few microns optimistic. Trimming to exactly 0.15 mm left residual
  violations of ~3.8 µm — correct arithmetic against a slightly wrong shape.
  Hence `MARGIN_MM`.

## Implementation Units

### U1. Angular clipping for circles and arcs

68 violations remain and 42 of them are `Circle` (24) or `Arc` (18). Extend the
trimmer to clip in angle rather than in `t`: for each pad keepout, compute the
angular interval(s) of the circle/arc that fall inside it, and emit the
complement as one or more arcs.

The `SW-TH_4P` footprint (6 placements) is the main consumer — its 14 circles
are the switch body, and each crosses all four PTH pads.

**Verify:** circle/arc violations reach 0 on a copy; no line count changes.

### U2. Deletion policy per footprint class

Decide, and record in the spec, what "trimmed" means for each of the 19 types.
Three classes to settle:

- **Chip passives** (`R0603`, `L0603`, `C0805`, `ERJP06F60R4V`) — silk between
  the pads has nowhere to go. Decide: no silk, or silk only on the outer edges.
- **Multi-pin ICs** (`SOIC-8`, `LQFP-144`, `WSON-8`, `SOP-8`) — trim the body
  sides between pins, keep the ends and the pin-1 feature. This is the normal
  IPC-7351 form and should look unremarkable.
- **Connectors and switches** (`SW-TH_4P`, `FPC-SMD`, `DC-IN-TH`, `USB-C`) —
  keep the body outline recognisable; these are the parts an assembler orients
  by.

**Verify:** a rendered before/after of every affected footprint, reviewed by
eye. Silk exists to be read; a DRC number cannot tell you it is still legible.

### U0. BLOCKER, found 2026-08-05: per-instance trimming defeats U3

Attempted in this order — U1 (angular clipping) landed, the trim ran on the
board, then the mirror was written. It got as far as **199 → 8 violations**
before U3 exposed a contradiction the plan did not anticipate.

**Trimming each placement against its neighbours makes instances of one
footprint type diverge.** After the trim, `C0805` had **11 instances with 4
distinct silk geometries**; eight more types diverged the same way. That is
*correct per-board geometry* — each placement really does have different
neighbours — and it is structurally incompatible with a single library
definition. Route 1's premise ("fix the library and every instance inherits")
only holds if the trim depends solely on the footprint's **own** pads.

So the two goals cannot both be met by the current trimmer:

| | Clearance correct everywhere | Library-mirrorable |
|---|---|---|
| Trim vs. all nearby pads | yes | **no** — instances diverge |
| Trim vs. own pads only | partially | yes |

**The redesign this implies.** Trim against the footprint's own pads only —
that is a property of the type, identical for every placement, and mirrors
cleanly. Then handle the residue (silk close to a *neighbouring* part's pad)
separately: as board-level per-instance edits, or by moving the parts, or by
accepting it with a recorded reason. Measure the residue first; it may be small
enough that the type-level fix does nearly all the work.

`tools/kicad_silk_mirror.py` already reports divergence rather than silently
picking a winner, so it will confirm when the redesign has fixed this: the
divergent list must be empty before any mirror is written.

The board was reverted; the gate is back to 0 violations / 0 unconnected. The
trimmer and the mirror are landed and unapplied.

### U3. Mirror into the footprint libraries

Apply the same geometry to `kicad/board3/*.pretty/*.kicad_mod` so future
placements inherit it. Library coordinates are footprint-relative and the placed
instances are rotated, so this is a transform, not a copy.

**Verify:** `--schematic-parity` clean and `lib_footprint_mismatch` back to 0 —
that count is the instrument for this unit, and it is already known to respond
(0 → 33 when the board was trimmed alone).

### U4. Board-level graphics

18 violations belong to no footprint. Handle directly on the board; there is no
library half.

### U5. Add the rule and close the gate

```
(rule jlc_silk_to_pad
  (constraint silk_clearance (min 0.15mm)))
```

Add to `kicad/board3/*.kicad_dru`. This is what makes the check exist at all,
and the CI DRC gate is absolute, so it must go in **with** a clean board rather
than before one.

**Verify:** `silk_over_copper` 0; `tools/kicad_verify.py` 0 violations, 0
unconnected; fab package regenerated and re-submitted to JLC's DFM with no red.

### U6. Also settle `silk_overlap`

199 `silk_overlap` findings (silk crossing silk) appear alongside. JLC did not
flag them and they were explicitly deferred when this work was scoped. Decide
whether to fix, exempt with a reason, or leave the rule scoped to
`silk_clearance` only — but decide, rather than leaving 199 findings that the
absolute gate would fail on.

## Scope Boundaries

**In:** silkscreen geometry, the `.kicad_dru` rule, the footprint libraries, the
trimmer.

**Out:** the `Silkscreen line width` warnings in JLC's report (25 items inherit
the board default of 0.1 mm against JLC's 0.15 minimum) — related, separately
decidable, and it slightly *worsens* clearance, so it should not be entangled
with this. Also out: `Solder mask opening exposing trace` (52) and `Negative
soldermask expansion` (80) from the same DFM report, which are the U53 mask
family and want their own measurement.

## Success Criteria

1. `silk_over_copper` 0 with the rule active, measured in place.
2. `lib_footprint_mismatch` 0 — library and board agree.
3. JLC DFM returns no red on silkscreen.
4. Every affected footprint reviewed by eye; no part left unidentifiable.
5. No regression: netlist unchanged, DRC 0/0/0, BOM 41 / CPL 142, tests 15/15.

## Outstanding Questions

- Do chip passives keep any silk at all on this board? (U2)
- Is `silk_overlap` in scope for the gate? (U6)
- `ERJP06F60R4V` arrived from JLCImport without a courtyard, authored during
  U57. Is its silk worth fixing at the source rather than trimming?
