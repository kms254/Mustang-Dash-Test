---
title: "Derive the fab viewer's rule before trusting its outlier"
date: 2026-08-10
category: developer-experience
module: board3-jlcpcb-order
problem_type: developer_experience
component: tooling
severity: high
applies_when:
  - "A fabricator's 3D placement viewer disagrees with the design about a part's orientation"
  - "Deciding whether to trust a rendered pin-1 marker over the board's own footprint file"
  - "Reviewing orientation of symmetric-pad parts, which no electrical check (ERC, DRC, parity, netlist) can see"
  - "One part mismatches while every other marked part agrees, especially at a unique input value such as the board's only 180-degree rotation"
  - "A rendered 3D model's testimony must be validated before it is admitted (scale it against its own pad pitch)"
symptoms:
  - "JLCPCB 3D viewer rendered U2 (W25Q256JVEIQ, WSON-8 8x6) with pin 1 at the NE corner where the design places it SE"
  - "The mismatched part is the board's only 180-degree CPL rotation; 13 other marked parts at 0/+90/-90 all agree with the derived viewer rule"
  - "JLC's model, measured against its own 3.81 mm pad pitch, is an undersized ~4.7 x 6.4 mm portrait stand-in for an 8.0 x 6.0 landscape part -- it cannot confirm or deny the design"
tags: [jlcpcb, 3d-viewer, cpl-rotation, pin-1-orientation, instrument-finding, verification, wson, board3]
---

# Derive the fab viewer's rule before trusting its outlier

## Context

Board3's first JLCPCB order (2026-08-10). The pre-order placement review in JLC's 3D
viewer had just earned its keep: it caught the P1/P2 CAN terminals designed 180°
backwards — a real defect, fixed at the source in PR kms254/Mustang-Dash-Test#36. So
when the same viewer showed U2 (Winbond W25Q256JVEIQ, WSON-8 8×6 mm, LCSC C97522) with
its pin-1 marker and molded dot at the NE corner, where the design's pin 1 lands SE, the
finding arrived with credibility: same instrument, same session, same class of defect it
had just proven it could find.

The first hypothesis — "viewer artifact" — was lazier than the evidence and did not
survive the user's challenge ("the U1 dots line up just like U2"). That challenge is
what forced the real work: a ladder of cheap, independent checks that ended at the
part's own mechanical drawing. The design was right. The viewer's model for this one
package was an undersized generic stand-in whose orientation carried no information in
either direction — it could neither confirm nor deny anything.

## Guidance: the verification ladder

Each rung is cheap, and each is independently decisive. Climb until one settles it; on
Board3 every rung was climbed and they all agreed.

**1. Consistency sweep — derive the viewer's marker rule from parts you can verify.**
Do not adjudicate the disputed part first. Tabulate marker corner against CPL rotation
for every IC on the board that carries a visible pin-1 marker:

| CPL rotation | Viewer marker corner | Parts | Matches design pin-1? |
|---|---|---|---|
| 0° | SW | 5 | yes, all |
| +90° | SE | 4 | yes, all |
| −90° | NW | 4 | yes, all |
| 180° | NE | 1 (U2 — the board's *only* 180° part) | **no — design pin 1 is SE** |

One consistent rule with a single outlier at a unique input value localizes the fault to
one library entry, not to the design and not to the viewer as a whole. A bonus that fell
out for free: 180° is chirality-immune — it is the same rotation clockwise or
counter-clockwise — so the mismatch cannot be a CW/CCW sign-convention disagreement
between the CPL and the viewer. That whole family of explanations dies in one row.

**2. Feasibility pruning — enumerate the physically mountable orientations before
arguing about which one is shown.** U2's footprint puts its pad columns east and west.
A chip whose pads sit on two opposite edges has exactly two mountable orientations, so
its corner dot can land only on one diagonal — here {SE, NW}. The viewer showed NE,
which is unreachable by *any* physical placement of the real part. The render was
depicting something no pick-and-place machine could build. That is proof the render is
wrong, obtained with zero external references.

**3. Scale check (the decisive one) — measure the viewer's model against its own pad
pitch.** The render contains its own ruler: four pads at 1.27 mm pitch span 3.81 mm
center-to-center. Scaling JLC's rendered image by that reference gives a model body of
roughly 4.7 × 6.4 mm drawn portrait — against the real chip's 8.0 × 6.0 mm landscape —
with visible gaps between the model body and the pads it is supposed to sit on. The
model is an undersized generic stand-in for a different package. **A model that does not
match the part can neither confirm nor deny the design** — once this is established, its
orientation carries no information, and no amount of further screenshot inspection can
extract any.

**4. Ground truth comes from the part, not the render.** Winbond W25Q256JV datasheet
Rev I (October 23, 2018), §9.4: the WSON 8x6 mechanical drawing gives D = 8.00 mm,
E = 6.00 mm, with the PIN 1 INDENT marked at pin 1; Figure 1a assigns the pads to the
two short 6 mm edges, pins 1–4 down one column. Checked against the board's footprint,
`kicad/board3/ProPrj_New-easyedapro.pretty/WSON-8_L8.0-W6.0-P1.27-TL-EP.kicad_mod`:
numbered pads 1–4 at (−4, −1.905) through (−4, +1.905) and pads 5–8 mirrored at x = +4 —
land columns 8.0 mm apart, 0.7 × 1.0 mm lands at 1.27 mm pitch — exactly the drawing's
geometry with toe extension. The footprint is correct, and at the placed 180° rotation
the design's pin 1 (/CS) lands on the south-east pad. (Read the `.kicad_mod` and cite
the pad lines when making this argument; the coordinates above are from the file, not
from memory.)

**5. Procedural close-out when the instrument cannot adjudicate.** JLC's order UI has no
rotate control, and rung 3 established the viewer is untrustworthy for this part — so
there is nothing left to fix in software and nothing left to check in the viewer. The
resolution is procedural: a written assembly remark on the order — "pin 1 (/CS, marked
by the PIN 1 INDENT) must land on the south-east pad", with the datasheet URL — plus the
paid Confirm-Parts-Placement checkpoint, which puts a human at JLC with the real reel in
the loop. Recorded in `fab/ORDER.md`'s board-state block (PR kms254/Mustang-Dash-Test#37,
since merged).

A note on durability: the measurement scripts behind rungs 1–3 were session scratch and
are not committed. The method is the durable part — marker-rule table, feasibility
diagonal, pad-pitch scale reference — each reconstructible in minutes from the board
files and a screenshot. The project-side ground-truth instrument is already in the tree:
`tools/kicad_fab.py`'s `rotation_audit()` carries U2 in its witness list, and its
docstring prescribed exactly this resolution — "if those disagree with the datasheet,
stop before ordering assembly."

## Why This Matters

Orientation of a symmetric-pad or fine-pitch part is structurally invisible to **every**
electrical check. ERC, DRC, `--schematic-parity`, and the netlist are all blind to which
way a body faces — they verify connectivity and geometry against the footprint, and the
footprint is equally satisfied by a body soldered on backwards. A 3D placement review is
therefore the *only* instrument for this defect class, and this one session exhibited
both of its failure modes back to back:

- it caught a **real** 180° defect (P1/P2, fixed in PR #36), and
- it presented an **unfalsifiable** one (U2) with exactly the same visual confidence.

The difference between the two cases was not visible in the renders themselves. It was
established by verifying the instrument — at the cost of one pad-pitch measurement.
Compare the costs of trusting either way without that measurement: rotate a correct part
on the render's say-so and the first power-up kills a WSON flash chip that only hot-air
rework can replace; dismiss the render reflexively and the next P1/P2 ships backwards.
Both wrong answers are expensive; the measurement that separates them is nearly free.

## When to Apply

- Any fabricator placement review — JLCPCB's 3D viewer or any other vendor's equivalent.
- Any time a 3D viewer, DFM report, or generated rendering disagrees with the design:
  the disagreement is a claim about *one of two* artifacts, and which one is wrong is an
  open question until the instrument is checked.
- Any part whose orientation only eyes can check — symmetric pad patterns, no-lead
  packages (WSON/DFN/QFN), diodes and polarized capacitors under generic models.
- Before acting on a single-part anomaly in an otherwise-consistent automated report:
  the consistency of the other N−1 parts is itself evidence, and a sole outlier at a
  unique input value (here, the board's only 180° rotation) points at a library entry,
  not at the design.

## Examples

**The marker-rule table (rung 1).** Thirteen matches and one outlier: rotation 0° → SW
corner (5 parts), +90° → SE (4), −90° → NW (4), every one agreeing with the design's
pin-1 placement — and U2, the only part on the board placed at 180°, showing NE where
the design says SE. The table does three jobs at once: it validates the viewer as an
instrument for 13 parts, it isolates the anomaly to a single library entry, and (because
180° is the same rotation in either direction) it eliminates every CW/CCW convention
theory without a separate argument.

**The scale-check arithmetic (rung 3).** Reference: four pads at 1.27 mm pitch span
3.81 mm. Measured against that reference, the rendered body came out ~4.7 × 6.4 mm,
drawn portrait; the real W25Q256JVEIQ is 8.0 × 6.0 mm, landscape (datasheet Rev I,
§9.4). The model does not even touch its own pads. One division ended an argument that
two rounds of screenshot inspection had failed to settle.

**Before and after.** Before: adjudicating corners from screenshots — which produced a
wrong answer twice in one session (first "the design is backwards", then "it's just a
viewer artifact", neither supported). After: the ladder above — derive the instrument's
rule from verifiable parts, prune to physically mountable orientations, scale-check the
model against its own pads, then go to the datasheet and the `.kicad_mod` for ground
truth, and close out with a written remark plus a human checkpoint when the instrument
cannot be repaired from the ordering side.

## Related

- [a tool's finding can be a property of the tool](a-tools-finding-can-be-a-property-of-the-tool.md)
  — the same attack run against an instrument we owned: there the fingerprint's three
  bugs were found and fixed and the campaign unblocked. Here the instrument is a third
  party's viewer, un-fixable — the ladder ends not at a repair but at "uninformative,"
  and the close-out is ground truth plus a written assembly remark.
- [clip-test board window queries](clip-test-board-window-queries.md) — the scale-check's
  home: a finding whose magnitude is implausible against the object's own dimensions
  indicts the measurement, and a false positive is confirmed by its own fix. The
  pad-pitch check on JLC's U2 model is that rule pointed at a model instead of a proxy
  shape.
- [verifying every part of a claim does not verify the claim](../design-patterns/verifying-every-part-of-a-claim-does-not-verify-the-claim.md)
  — its "would the evidence look different if you were wrong?" test is what "can neither
  confirm nor deny" means formally: a stand-in model produces the same picture whichever
  way the part faces, so the render stops being evidence in either direction.
- [calibrate an automated reviewer on a confirmed defect](../design-patterns/calibrate-an-automated-reviewer-on-a-confirmed-defect.md)
  — the sweep is this pattern run against a reviewer whose insides you cannot read:
  establish the viewer's marker rule on parts whose orientation is already known, and
  only then ask what its report on U2 means.
- ["DRC clean and measured" is not "assemblable"](../conventions/drc-clean-and-measured-is-not-assemblable.md)
  — the category this belongs to: a defect every project-owned check is silent on by
  construction. Orientation of a symmetric-pad part is its purest case — the netlist is
  identical both ways — and the fabricator's 3D render is the only instrument that can
  see it at order time.
- `fab/ORDER.md` board-state block (PR #37, since merged) and
  `tools/kicad_fab.py` `rotation_audit()` / its witness list — the operational halves:
  the assembly remark, the check-U2-first placement checkpoint, and the audit whose
  docstring prescribed the datasheet-vs-footprint resolution.
- [diff the fab's production file before confirming the order](diff-the-fabs-production-file-before-confirming-the-order.md)
  — the same order's converse instrument verdict, one checkpoint later: there
  the fab's CAM job *out-informed* every project-owned check, fixing a footprint
  defect nothing of ours could see. Rung 1's "derive the rule from parts you can
  verify" reappears there as coordinate-frame calibration on a known hole
  constellation.
- [audit the fab's revision set before confirming placement](audit-the-fabs-revision-set-before-confirming-placement.md)
  — the execution of rung 5's close-out: at the Confirm-Parts-Placement
  checkpoint this doc deferred to, the written U2 remark surfaced in the
  engineer's own revision set (honored — pin 1 rendered SE), and rung 1's
  consistency-sweep idea returns inverted as a design-truth table read against
  one representative per polarity family.
- `docs/hardware/board3-bringup-card.md` — the U2 JEDEC-ID smoke test is the downstream
  detector, but it fires only after boards arrive; order-time 3D eyes are the only
  pre-fab instrument.
