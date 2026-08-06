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
are inert on their own.

**The "199 + 199" this plan was originally scoped against is a report limit, not
a count** (see U0 and CLAUDE.md). `kicad-cli pcb drc` applies a per-error-code
limit of 199: at 0.10 mm clearance `silk_over_copper` reads 62, and from 0.15 mm
up both types read exactly 199 and stay there at 1.00 mm — where nearly all 586
of the board's silk shapes must be violating. The limit is not uniform (a probe
run reported `clearance` 514 beside three other types pinned at 199), so a count
near 199 is a lower bound until a stricter rule shows it moving. The real figure,
measured directly off the geometry, was **233 shape×pad pairs over 169 shapes**.
Every count in the original plan text below inherits that limit and should be
read as "at least".

**Done means:** silk clears every mask aperture by 0.15 mm, the rule is in the
`.kicad_dru` so the absolute CI gate enforces it, the fix lives in the footprint
libraries as well as the board's embedded copies, and JLC's DFM returns no red.

**Status 2026-08-05: silk-to-pad 233 pairs → 6**, all six documented exceptions
in the `.kicad_dru`. Board verifies 0 violations / 0 unconnected with the rule
armed; nothing electrical moved. U1–U5 are done; U6 is answered (there is no
separate silk-crossing-silk problem — see below); the JLC DFM re-submission is
the remaining confirmation.

## Why this plan exists

The work was scoped as a one-liner — add the rule with the fix — and the
measurement showed it is not. This plan exists because three things turned out
to be true that were not known when the work was requested:

1. **169 shapes / 233 shape×pad pairs, not a handful** (originally recorded as
   "199 objects", which was the report limit), spread across the whole board
   (x 28–258 mm), owned by **19 footprint types** plus 18 board-level graphics.
2. **None is an overlap.** The range is 0.031–0.1495 mm against a 0.15 mm
   requirement; the median shortfall is **36 µm**. Nothing prints on a pad
   today. The 0.0000 mm entries that appear in the DRC report are
   silk-to-*courtyard*, not silk-to-pad — a layer that is never plotted.
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

> **Resolved, and the premise was wrong.** That trial was deleting compliant
> geometry: R0603's silk measures 0.1522 mm from its own pads, which *passes*
> 0.15 and was only being cut because the tool tested against clearance +
> margin. With that fixed and narrowing tried first, R0603 and C0805 are not
> touched at all, no footprint class loses all its silk, and total removals fall
> from 157 shapes to 22 — all on chip passives where silk between the pads
> genuinely has nowhere to go (`ERJP06F60R4V` 8, `L0603` 4, `1N5819WS` 4, LED
> types 6). The general lesson is that a threshold sweeping up compliant objects
> looks exactly like a hard design trade-off until you check which side of the
> requirement they were on.

**Polarity and pin-1 markers are never trimmed.** The 8 remaining `Polygon`
violations are on `1N5819WS` and the LED footprints. Cutting a cathode band or a
pin-1 dot destroys the meaning the marker exists to carry; those move, shrink,
or take a recorded exemption.

## What already exists

`tools/kicad_silk_trim.py` narrows, then trims, silk **lines, circles and arcs**
back from mask apertures. Exact point-to-polygon distance, 5 µm walk, boundaries
refined by bisection to 0.1 µm. It deliberately does not use
`GetEffectiveShape().Collide()`, which CLAUDE.md records as under-covering
segment midpoints — precisely this query.

Measured in place: silk-to-pad **233 pairs → 6**.

**Two design corrections it now encodes, both of which were destroying good
markings.** *Narrow before trimming* — clearance is edge-to-edge on a stored
centreline, so reducing a line's width buys clearance with no geometry change;
that alone fixes 50 shapes, including all 24 board-level outlines around
C63/C64/C65 that the earlier version deleted outright. It is available whenever
the centreline sits ≥ 0.225 mm out (0.15 clearance + half of JLC's 0.15 mm
minimum printable width). And *only touch what actually violates* — testing
against clearance + margin rather than clearance deleted **137 compliant
shapes**, every R0603 outline among them at a measured 0.1522 mm: above the
requirement, below requirement + margin.

Two traps are already paid for and encoded in its source:

- `GetEffectivePolygon()` takes a layer argument in KiCad 10. An earlier
  revision swallowed the `TypeError`, so every pad returned no rings and the
  tool reported **"0 lines need trimming"** on a board with 199 violations.
- That polygon is *inscribed* in a round pad, so distances measured against it
  run a few microns optimistic. Trimming to exactly 0.15 mm left residual
  violations of ~3.8 µm — correct arithmetic against a slightly wrong shape.
  Hence `MARGIN_MM`.

## Implementation Units

### U1. DONE — angular clipping for circles and arcs

68 violations remain and 42 of them are `Circle` (24) or `Arc` (18). Extend the
trimmer to clip in angle rather than in `t`: for each pad keepout, compute the
angular interval(s) of the circle/arc that fall inside it, and emit the
complement as one or more arcs.

The `SW-TH_4P` footprint (6 placements) is the main consumer — its 14 circles
are the switch body, and each crosses all four PTH pads.

**Verify:** circle/arc violations reach 0 on a copy; no line count changes.

### U2. DONE — deletion policy per footprint class

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

### U0. RETIRED 2026-08-05: the blocker was the detector, not the board

**Everything below this heading is superseded. It is kept because the reasoning
was sound given what it was told, and the failure was upstream of it.**

`kicad_silk_mirror.silk_signature()` had three bugs, each independently able to
manufacture the divergence U0 records:

1. It subtracted the footprint's **position but never its rotation**, so the
   four rotations of one library part fingerprinted as four geometries. That is
   literally the "C0805, 11 instances, 4 distinct" figure below — C0805 has
   **one** geometry, placed at 0/90/180/270.
2. It compared **exact integers**. The trimmer computes cut points in absolute
   board coordinates and rounds to nanometres, so one cut on two placements can
   land 1 nm apart. Measured on SOIC-8: 26 shapes each, worst delta **1 nm**.
3. For **polygons** it fingerprinted `GetStart()`/`GetEnd()`, which are not a
   polygon's outline and returned raw board coordinates — "local" deltas of
   125–312 mm on parts a few mm across. Every type reported divergent after the
   first two were fixed was one carrying a polarity band or pin-1 marker.

With all three fixed, **all 30 types mirror cleanly** and the board holds DRC
0/0. Own-pad scope (`--scope own`, now the default) introduces **zero** new
divergence: the trimmed board and the pristine board produce the same result.

The sign in fix 1 is validated rather than assumed — un-rotating by *+angle*
reproduces the `.kicad_mod`'s own coordinates 31/31 on R0603 and 11/11 on C0805
at all four rotations, while *−angle* matches only the 0°/180° instances. The
library file is the footprint-local form, so it is the ground truth.

**The lesson worth keeping is not about silkscreen.** A tool reported a property
of the board; the property was a property of the tool; and the campaign was
reverted on it. The report even looked plausible — "instances of one type
genuinely have different neighbours" is a true sentence, which is what made the
false reading survive review. What would have caught it in one step: C0805's
four "distinct geometries" were exactly its four rotations, and R0603's were
too. A grouping that lines up perfectly with an obvious confounder is a bug
until proven otherwise.

<details>
<summary>Superseded original text of U0</summary>

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

</details>

### U3. DONE — mirror into the footprint libraries

Apply the same geometry to `kicad/board3/*.pretty/*.kicad_mod` so future
placements inherit it. Library coordinates are footprint-relative and the placed
instances are rotated, so this is a transform, not a copy.

**Verify:** `--schematic-parity` clean and `lib_footprint_mismatch` back to 0 —
that count is the instrument for this unit, and it is already known to respond
(0 → 33 when the board was trimmed alone).

### U4. DONE — board-level graphics

18 violations belong to no footprint. Handle directly on the board; there is no
library half.

### U5. DONE — add the rule and close the gate

```
(rule jlc_silk_to_pad
  (constraint silk_clearance (min 0.15mm)))
```

Add to `kicad/board3/*.kicad_dru`. This is what makes the check exist at all,
and the CI DRC gate is absolute, so it must go in **with** a clean board rather
than before one.

**Verify:** `silk_over_copper` 0; `tools/kicad_verify.py` 0 violations, 0
unconnected; fab package regenerated and re-submitted to JLC's DFM with no red.

### U6. ANSWERED: `silk_overlap` is not silk crossing silk

The premise was wrong. Resolving every finding's item **uuid** through `pcbnew`
rather than reading its description shows `silk_overlap` pairs silk with
`F.Courtyard` rectangles and Component Marking Layer fields — and, before the
fix, with pads. It is not a silk-to-silk problem, and there is nothing separate
to settle.

What it did expose is that the constraint is over-broad by default: neither a
courtyard nor the Component Marking Layer is plotted (`kicad_fab.py` exports 11
layers and Courtyard is not among them), so those findings can never change the
board JLCPCB builds. Scoping the rule with `(condition "B.Type == 'Pad'")` takes
it from 18 + 199 to 9 + 0 and leaves exactly the class JLC checks.

Two mechanics worth reusing, both already paid for elsewhere in this repo:
DRC's item **descriptions are inconsistent** — "Pad 2 [/GND] of U9 on Top Layer"
for some findings and "Rectangle of U1 on F.Courtyard" for others — so parsing
them mis-attributed the findings twice. The **uuid maps exactly to the object in
the file** and settled it in one pass. And the reported `pos` for a rectangle is
its first corner, not its centre: reading it as a centre made a genuine 0.0000 mm
overlap look like an impossible 13 mm-away collision.

## Scope Boundaries

**In:** silkscreen geometry, the `.kicad_dru` rule, the footprint libraries, the
trimmer.

**Out:** ~~the `Silkscreen line width` warnings in JLC's report (25 items inherit
the board default of 0.1 mm against JLC's 0.15 minimum)~~ — measured and
answered above; the premise was false and the `.kicad_pro` defaults are fixed.
Also out: `Solder mask opening exposing trace` (52) and `Negative
soldermask expansion` (80) from the same DFM report, which are the U53 mask
family and want their own measurement.

## Success Criteria

1. ✅ `silk_over_copper` **0** with the rule active, measured in place —
   `kicad_verify.py` reports 0 violations / 0 unconnected at `--severity-all`
   with `jlc_silk_to_pad` armed. Six residual cases are scoped exceptions in the
   `.kicad_dru`, held to their measured worst (0.09 mm) rather than muted.
2. ✅ `lib_footprint_mismatch` **0** — library and board agree; 17 `.kicad_mod`
   files rewritten, 13 left alone because they already matched.
3. ⬜ **JLC DFM returns no red on silkscreen** — the remaining confirmation, and
   the only criterion this repo cannot self-assess. Re-submit the fab package.
4. ⬜ Every affected footprint reviewed by eye; no part left unidentifiable.
   Silk exists to be read and a DRC count cannot tell you it still is.
5. ✅ No regression: 150 footprints, 229 nets, 2780 tracks, 238 vias, 7 zones,
   660 pads — identical before and after, as silk edits must be.

### Silkscreen to hole: 30 red, and the recommendation is to leave them

JLC's DFM on the 2026-08-05 package: **81 red → 35** (silkscreen-to-pad 50 → 5,
silkscreen-to-hole 31 → 30). Silkscreen is still the board's only red; every
other category is 0.

Silk-to-hole barely moved **by construction**. The rule added in U5 is scoped
`B.Type == 'Pad'`, and a via is not a pad, so 238 vias were never measured.
Measured now: **32 collisions, every one against a via**, several negative
(silk running up to 0.198 mm into the hole).

**They are all over TENTED vias, so there is no open barrel for ink to reach.**
Three independent measurements agree:

- `F_Mask.gbr` carries **642 regions against 639 pads on F.Mask** — the
  difference is exactly U53's 3 explicitly untented test-point vias, and **none
  of the 3 is involved in a silk collision**.
- The 24 colliding vias are 22 `FROM_RULES` + 2 explicitly `TENTED`; none is
  `NOT_TENTED`.
- Every via on the board drills **0.250 mm** (237) or 0.200 mm (1), and all 24
  colliding ones are 0.250 mm. JLCPCB's own via-covering page puts reliable full
  coverage at "**0.4 mm or less and no larger than 0.5 mm**", warning that above
  that "larger vias are not guaranteed to be fully covered; no complaints are
  accepted regarding this type of defect". Board3 has **zero** colliding vias
  above 0.4 mm — the entire population sits at 62% of the ideal threshold.

JLC flags them because its DFM compares the silk layer against the drill file
and cannot see tenting.

**Recommendation: do not chase them, and record why.** Fixing means trimming
each placement against board-level vias, which is inherently per-instance —
reintroducing exactly the library divergence U0's fix resolved, so the mirror
would refuse those types and `lib_footprint_mismatch` would fire permanently.
That is a real, permanent cost against a defect that does not physically exist.
Revisit only if the via drill ever grows past 0.4 mm, which is the condition
that would make it real.

### Silkscreen line width: the premise was wrong, and the cause is unconfirmed

This plan recorded JLC's 25 *silkscreen line width* warnings as "25 items
inherit the board default of 0.1 mm". **Nothing on Board3 is below 0.15 mm** —
measured on both the pristine and the current board:

```
shape widths : 0.0 (x25)  0.15  0.152  0.1524  0.1525  0.2  0.203  0.254  0.3  0.4
text strokes : 0.0 (x4)   0.15  0.152  0.1525
shapes < 0.15 mm : 0        text < 0.15 mm : 0
```

The 0.1 mm is real but it is a **`.kicad_pro` default for newly-created items**
(`silk_line_width`, `silk_text_thickness`), not a property of anything drawn.
Reading a setting as a population is the same mistake as reading
`min_silk_clearance` as an active check — twice in one file, in opposite
directions.

**Fixed anyway, because it is a live trap rather than a present defect:** both
defaults are raised 0.1 → 0.15, so silk drawn in the GUI tomorrow cannot arrive
below the fab minimum. No existing geometry changes; DRC stays 0/0.

**What JLC actually flagged is not established.** The strongest candidate is the
board's **25 zero-width silk polygons** (all on F.SilkS — the polarity bands and
pin-1 markers), which export `%ADD22C,0.000000*` into the front silk gerber. The
count matches exactly, but the evidence does not close:

- the aperture is selected **14 times, not 25**, and every selection is
  immediately followed by `G36*` (region begin), where the Gerber spec says the
  aperture is unused — so nothing is actually stroked at zero width;
- the polygons are correct as geometry: filled regions are what a cathode band
  should be;
- and giving them a real width is **not free** — a stroke straddles the outline,
  so it would grow D10/D11's bands outward from their already-tightest 0.1000 mm.

So this may well be a false positive in JLC's parser reacting to a 0.000 mm
aperture *definition*. **Settle it with JLC's DFM item list before changing any
polygon**, rather than inferring from counts — the last two confident readings
of JLC's published figures in this repo both had to be retracted.

## Outstanding Questions

- Do chip passives keep any silk at all on this board? (U2)
- Is `silk_overlap` in scope for the gate? (U6)
- `ERJP06F60R4V` arrived from JLCImport without a courtyard, authored during
  U57. Is its silk worth fixing at the source rather than trimming?
