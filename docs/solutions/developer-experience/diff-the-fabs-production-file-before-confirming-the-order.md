---
title: "Diff the fab's production file against your export before confirming the order"
date: 2026-08-11
category: developer-experience
module: board3-jlcpcb-order
problem_type: developer_experience
component: tooling
severity: high
applies_when:
  - "A fab's paid confirm-production-file checkpoint offers the production files for download"
  - "Approving a fab's render when NPTH-vs-PTH, plating compensation, or mask-web survival cannot be seen in a render"
  - "Reading a JLCPCB CAM job -- an ODB++-style package (steps/edit/layers/<layer>/features + tools, matrix file) plus their archived copy of your gerbers"
  - "Interpreting fab layer short-names, which use Protel semantics -- ts is top SOLDER (mask), to is top OVERLAY (silk)"
  - "A delta between the fab's CAM output and your export must be classified as a defect they fixed (adopt it) or a change to challenge"
symptoms:
  - "JLC's CAM job drilled four holes our drill programs never had -- two 0.744 mm NPTH at USBC1's locating pegs (5.8 mm apart), a genuine assembly defect their CAM engineer silently fixed, plus two corner tooling NPTH the order form disclosed"
  - "Footprint USB-C_SMD-TYPE-C-31-M-12_1 models the pegs as filled Edge.Cuts polygons that reach neither Excellon file, so the defect survives in the board file for any future order or other fab"
  - "Checking 'to' for mask openings reports every opening missing -- 'to' is overlay (silk), not solder mask"
  - "The checkpoint render alone cannot show NPTH-vs-PTH or mask-web survival; the CAM text is plain and checkable to the micrometer (TP1-TP3 untents preserved to 1.6 um)"
tags: [jlcpcb, cam-job, odb, production-file, npth, paid-checkpoint, verification, board3]
---

# Diff the fab's production file against your export before confirming the order

## Context

Board3's first JLCPCB order (2026-08-10/11: PCB prototype **Y4-13066334A**, PCBA
**SMT026081160502**; the order-session changes landed in PRs #36 and #37). Both
order lines were placed with the paid **Confirm Production File** option, and
`fab/ORDER.md`'s Board state block had already queued what that checkpoint was
for: "Pending checkpoints, all by email: production-file confirmation (verify
H2's three NPTH holes stay unplated; see how rails were added around the USBC1
edge overhang)…".

When the checkpoint email arrived it presented a Top/Bottom render, a
"Download Production file" link, and a Yes/No. A render cannot show any of the
things the checkpoint exists to catch — plating (NPTH vs PTH), solder-mask web
survival, or holes the fab added. So instead of eyeballing the picture, the
production file was downloaded and diffed against our own export. Every number
below was measured live in that session. The JLC CAM artifacts themselves are
session scratch — they were not committed, and a reorder will produce fresh
ones — the durable part is the method.

## Guidance — how to read a JLCPCB production file

1. **Zip anatomy.** The archive is their internal CAM job, three things deep:
   `ok/` holds short-named preview gerbers; `YG/` is their archived copy of the
   customer upload — our KiCad gerbers, byte-preserved (the `NPTH.drl` inside
   carried our exact 3 holes and KiCad's own header timestamps, which is how
   you know it is untouched input rather than output); and a `.tgz` holding an
   ODB++-style job: `steps/edit/layers/<layer>/{features,tools}` plus
   `matrix/matrix`. The `.tgz` is the output — the thing they will actually
   build — so that is what you diff.

2. **Layer-name trap: the short names are Protel semantics.** `ts` = Top
   SOLDER (mask), `to` = Top OVERLAY (silk), `tl`/`bl` = copper, `ko` =
   outline, `drl`/`npth`/`sk` = drill layers. The first mask check here was
   run against `to` and reported every test-point opening missing, nearest
   candidates 11–24 mm away; re-run against `ts`, all three were found within
   1.6 µm. A wrong-layer check FAILS LOUD — errors of tens of millimetres — so
   it self-identifies, but only if you sanity-check the magnitude of the
   mismatch instead of writing down "missing".

3. **The `tools` files are the plating truth.** Each tool carries
   `TYPE=VIA/PLATED/NON_PLATED`, and `FINISH_SIZE` vs `DRILL_SIZE` exposes
   their plating compensation: plated tools drill ~0.15 mm larger than finish;
   non-plated tools drill at finish exactly. The `features` files carry the
   ODB `.drill` attribute per hole: `;0=0` plated, `;0=1` non_plated, `;0=2`
   via. Coordinates are **inches** (×25.4 → mm), in a **board-local frame with
   y flipped about the outline**: `their_y = y_edge_max − kicad_y`,
   `their_x = kicad_x − x_edge_min`. Calibrate the frame on a known feature
   before trusting any comparison — H2's distinctive three-hole NPTH
   constellation matched to ~2 µm and anchored everything else.

4. **Diff their data against your export, hole census first.** Board3's
   census came out exactly right: 235 vias + 47 plated THT + 7 plated slots +
   3 NPTH (H2 — present in BOTH a dedicated npth layer and non_plated-flagged
   in the unified drill program, belt and braces) + the TP1–TP3 untent mask
   openings at 1.6 µm. Then hunt ADDITIONS: **4 holes existed in their job and
   not in our files**, verified by minimum-distance against every hole in our
   PTH+NPTH programs — nearest neighbours 1.4–5.9 mm away, so genuinely new,
   not relocated.

5. **Adjudicate each addition individually.** The four split into two very
   different stories:
   - **Two 0.744 mm NPTH at USBC1's locating-peg positions** (5.8 mm apart,
     matching the TYPE-C-31-M-12's two Ø0.65 mm pegs): THEIR ENGINEER FIXING
     OUR DEFECT. Our drill programs never made peg holes, so the connector
     would have sat proud on its pegs with gapped paste joints. Invisible to
     DRC, schematic parity, courtyard checks and DFM alike — no check compares
     a footprint to the part's mechanical drawing.
   - **Two corner NPTH (1.05 mm and 0.80 mm, ~5 mm inboard of the corner
     mounting holes, in free space)**: the order form's "Tooling holes: Added
     by JLCPCB" materializing — authorized on the form, expected, harmless.
     (Consistent with `fab/ORDER.md`'s panelisation note that JLC would add
     their own handling features to this rail-less 250 × 50 board.)

## Why This Matters

The fab's production file is their engineer's answer sheet. The delta against
your own export is a findings list, and every delta is one of two things: a
defect they quietly fixed — which you must ADOPT into the design so the fix
survives a different fab or a reorder — or a change to challenge BEFORE
production, while the checkpoint is still open. The paid confirmation
checkpoint is only as strong as the review done at it; clicking Yes on the
render alone would have verified nothing the render can't show — which is
everything that matters here (plating, mask webs, added holes).

Cost of doing it properly: about 10 minutes of parsing plain-text files.
Counterfactual: the USBC1 peg defect would have been discovered as a connector
that rocks on its pegs at assembly — or never, with JLC silently fixing it
every run while our board file stayed wrong for any other fab.

## Follow-up debt (open as of 2026-08-11)

**`kicad/board3/ProPrj_New-easyedapro.pretty/USB-C_SMD-TYPE-C-31-M-12_1.kicad_mod`
still lacks the two peg NPTHs; JLC's fix exists only in their CAM job.**
Verified by reading the footprint: its only drilled pads are the four plated
`thru_hole` oval shell legs (pads 1–4, `*.Cu`/`*.Mask`, 0.8 mm-wide oval
drills) — there are **no NPTH pads** of any size. The peg geometry does exist
in the file, but only as artwork that never reaches a drill program: two filled
`fp_poly` circles on **Edge.Cuts**, Ø0.7436 mm, centred at footprint-local
(−2.9, −1.2055) and (+2.9, −1.2055) — exactly 5.8 mm apart and matching the
0.744 mm holes JLC added — plus two annotation circles on `Dwgs.User` at the
same centres. That is the shape of the defect: the EasyEDA import carried the
pegs onto layers that describe them without drilling them.

The fix is to give the footprint two real NPTH pads (Ø0.65–0.75 mm hole,
no copper) at those local coordinates, and retire the Edge.Cuts polygons so
the holes aren't represented twice in different languages. Until that lands,
**every re-export reproduces the defect**, and only JLC's CAM step is
correcting it. (Fix in place — same footprint name, same placement — so
`tools/kicad_review.py`'s calibration-fixture tokens stay valid; Edge.Cuts
changes alter the outline gerber, so regenerate the fab package and re-run the
gate after.)

## When to Apply

Any fab production-file / CAM confirmation checkpoint, JLCPCB or otherwise:
whenever a fab hands back their interpretation of your data and asks for a
Yes/No. Also any time a "confirm" step offers only a render — treat the render
as marketing and go find the data behind it. The same frame-calibration and
min-distance techniques apply to any ODB++/Excellon comparison.

## Examples

- **Worked example (this session):** calibrate the coordinate frame on H2's
  three-hole NPTH constellation (~2 µm agreement), run the full hole census
  (235 via / 47 PTH / 7 slots / 3 NPTH / 3 untent openings), then hunt
  additions by minimum-distance against every exported hole — which surfaced
  the two USBC1 peg holes (adopt: fix the footprint) and the two tooling
  holes (authorized: no action).
- **Cautionary example:** the mask check run against `to` (silk) instead of
  `ts` (mask) reported all three test-point openings "missing" at 11–24 mm.
  A wrong-layer check fails loud enough to self-identify — tens of millimetres,
  not microns — but only if you interrogate the magnitude instead of reporting
  the absence.

## Related

- [derive the fab viewer's rule before trusting its outlier](derive-the-fab-viewers-rule-before-trusting-its-outlier.md)
  — the same order's converse instrument verdict: there the fab's 3D viewer
  could neither confirm nor deny the design; here the fab's CAM job
  out-informs every project-owned check. Its rung 1 ("derive the rule from
  parts you can verify") reappears here as coordinate-frame calibration.
- ["DRC clean and measured" is not "assemblable"](../conventions/drc-clean-and-measured-is-not-assemblable.md)
  — the peg holes are its "check that returns nothing" family's purest
  drill-side case, and this doc supplies its previously-empty "Can a factory
  build this?" row with a real instrument: the factory's own CAM output,
  diffed at the checkpoint.
- [calibrate an automated reviewer on a confirmed defect](../design-patterns/calibrate-an-automated-reviewer-on-a-confirmed-defect.md)
  — the frame calibration is this pattern applied to a data artifact: validate
  the inch/y-flip transform on holes whose positions are already known before
  believing any per-hole finding.
- [clip-test board window queries](clip-test-board-window-queries.md)
  — the datum-discipline sibling: if your model of the geometry cannot
  reproduce the checker's own number, your datum (or your layer choice) is
  wrong — the 11–24 mm "missing" mask openings were the loud version.
- `fab/ORDER.md` board-state block — the operational record: this checkpoint
  executed 2026-08-11, outcome recorded there; the parts-placement (U2)
  checkpoint was executed later the same day — see below.
- [audit the fab's revision set before confirming placement](audit-the-fabs-revision-set-before-confirming-placement.md)
  — the same order's third and final checkpoint: this doc's adjudication frame
  (every fab-produced delta is adopt-or-challenge) applied to a placement
  revision set delivered through a UI filter instead of a CAM zip.
