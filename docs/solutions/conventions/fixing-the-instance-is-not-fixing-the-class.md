---
title: "Fixing the instance is not fixing the class — enumerate every part automation cannot see"
date: 2026-08-20
category: conventions
module: kicad/board3
problem_type: convention
component: tooling
severity: high
applies_when:
  - "A defect is found that no automated gate could have caught (DRC, ERC, netlist, parity, fab DFM)"
  - "Fixing one instance of a defect whose siblings share a footprint, connector family, or orientation property"
  - "Writing or calibrating a checker for a defect class, especially one gated on an allowlist of known parts"
  - "Writing a placement or layout-guide step that names an edge or an order but not a facing"
  - "Judging whether a gestalt look-right review is enough for a part that looks plausible either way"
symptoms:
  - "FPC1/FPC2/FPC3 (Molex 5034802000) placed with the cable opening facing the board interior; found only when a cable physically would not insert"
  - "The identical defect shape was fixed on P1/P2 ten days earlier (f1054f3) and that commit named the class without enumerating it"
  - "A checker for this exact class already existed and passed clean -- its geometry is class-general but its allowlist held two footprint names"
  - "Every gate passed: DRC 0/0 at --severity-all with --schematic-parity, ERC 0, airwires 0, netlist correct, JLCPCB DFM, and the JLC 3D placement-viewer audit"
  - "The mis-facing FPCs were inside the renders that same commit regenerated, and nobody asked the question of them"
root_cause: missing_workflow_step
resolution_type: workflow_improvement
related_components:
  - development_workflow
  - documentation
tags:
  - defect-class
  - connector-orientation
  - mating-direction
  - drc-blind-spot
  - placement-review
  - allowlist
  - kicad
  - board3
---

# Fixing the instance is not fixing the class

## Context

On 2026-08-10, during Board3's first JLCPCB order, the 3D placement viewer
showed P1 and P2 — the two WJ500V CAN screw terminals — designed 180°
backwards, wire mouths pointing into the board at the LQFP instead of out at
the edge. Commit `f1054f3` ("fix(kicad): P1/P2 CAN terminals face the board
edge, not the LQFP", merged as PR #36) fixed it, and its message named the
class of defect exactly:

> Nothing electrical could ever have seen this -- the two pads are symmetric -- so it survived every gate.

That was correct, and the general form was already in `CLAUDE.md`: the JLCPCB
placement viewer is the only instrument that sees part orientation, because
symmetric pads are invisible to ERC/DRC/parity/netlist.

**The class was named, a checker was built for it, and neither reached the
rest of the class.** Ten days later, on 2026-08-20, FPC1/FPC2/FPC3 — the three
20-pin panel connectors that are the entire reason the board exists — were
found by hand at the bench, all three with the cable mouth facing the board
interior. The first FFC simply would not go in from the panel side.

Measured this session against the board file:

- The `Edge.Cuts` outline spans **y 85 → 135**; the north edge is the
  panel-facing edge.
- **FPC1 (50.0005, 90), FPC2 (150.0005, 90), FPC3 (250, 90)**, all rotation 0,
  all footprint `ProPrj_New-easyedapro:FPC-SMD_5034802000`.
- The **20 signal tails sit at y = 88.675** (toward the board edge) and the
  **two hold-down tabs at y = 91.325**. Body y 87.949 → 91.949 — **4.000 mm**,
  matching the Molex drawing's stated 4.00 mm width.
- The part is **Molex 5034802000 / LCSC C234192**, an Easy-On **BackFlip**
  (FBH1) right-angle 20P 0.5 mm connector, "Double-Sided Contacts, Top and
  Bottom Entry".

BackFlip means actuator at the back, cable in at the front. The actuator is on
the **north** face, so the mouth opens **south, into the board interior** — the
cable is asked to arrive from over the MCU. All three parts, all the same way.

## The part that makes this a convention and not an anecdote

A checker for this exact defect class **already existed and passed clean**.

`tools/kicad_placement_lint.py` was written after the order session
specifically to automate these lessons, and calibrated on the confirmed
defects per this repo's own doctrine. Its check 2 docstring states the class
correctly:

> connector-mouth -- a wire-entry connector (screw terminal, barrel jack)
> placed against a board edge must have its entry face toward that edge. […]
> The pads are electrically symmetric, so no electrical check can ever see
> this class.

Its geometry is genuinely class-general: it takes the footprint body's **deep
side** (the courtyard extends further past the pin row on the mouth side,
because the clamp cavity needs the depth) and dots that vector against the
nearest board edge's outward normal. It even carries a `--self-test` that
extracts the pre-PR#36 board from git and asserts the check *fires* on it —
the "a check that has never been seen to fire is indistinguishable from a
malformed one" discipline.

And it is gated on an allowlist:

```python
WIRE_ENTRY_CLASSES = ("WJ500V", "DC-005")   # tools/kicad_placement_lint.py
...
if not any(cls in fpid for cls in WIRE_ENTRY_CLASSES):
    continue
```

Two footprint names: the instance that was found, plus one sibling. FPC/ZIF
connectors are not members, so the lint reports clean while three instances of
its own documented class sit on the board.

**Measured, not reasoned.** Loading the current board and widening only that
tuple:

```
--- as shipped: WIRE_ENTRY_CLASSES = ('WJ500V', 'DC-005')
ok   connector-mouth: wire-entry faces the nearest edge

--- with 'FPC-SMD_5034802000' added ---
FAIL connector-mouth: wire-entry faces the nearest edge
     FPC1 (FPC-SMD_5034802000): wire-entry face points AWAY from the north edge 2.73 mm away -- deep-side vector (-0.01, +1.02) mm
     FPC2 (FPC-SMD_5034802000): …
     FPC3 (FPC-SMD_5034802000): …
```

The detector worked. The geometry was right. One tuple was the difference
between a clean CI run and three findings.

So the failure is not "nobody looked." It is sharper and more instructive:
**the fix was scoped to the class, the check was scoped to the class, and the
allowlist was scoped to the instance** — so class-general machinery inherited
the instance's blast radius anyway.

## Guidance

**1. Treat "nothing could have caught this" as a trigger, not a conclusion.**
When a review or commit message reaches for that sentence — "it survived every
gate", "no electrical check can ever see this" — stop and enumerate the class
**in the same change**: name the property that made the defect invisible, list
**every** part sharing it, check each by measurement, and record the list and
the verdicts. A named blind spot that has not been enumerated is not closed.
Here the enumeration was three connectors and about ten minutes.

**2. A checker for an unseeable class must default to judging, not to
skipping.** An allowlist assembled from the calibration case silently re-imposes
that case's boundaries on general geometry, and it does so invisibly: the run
is green, the self-test passes, and the parts outside the tuple are not
reported as unchecked — they are not mentioned at all. **Invert it.** Judge
every part that meets the structural precondition (here: any connector-class
footprint whose courtyard sits within `EDGE_NEAR_MM` of a board edge) and
require an explicit, recorded exemption for anything that legitimately has no
mouth constraint. A new connector should default to *checked*, not to
*invisible*.

**3. Keep a per-connector mating-direction table as a pre-order artifact.**
For every connector: (a) what mates with it, (b) which direction that mating
half arrives from in board coordinates, (c) does the part point that way —
with the evidence, read from the `.kicad_mod` and the datasheet, not from the
schematic and not from memory. Connectors are a small, finite, enumerable set.

**4. A placement rule that omits facing is two-thirds of a rule.**
`docs/hardware/board3-pcb-layout-guide.md` step 1.2 specifies the FPCs go
"along the panel-facing edge, in physical left–center–right order" — edge and
order, never **facing**.

**5. Calibrate visual review with a specific question per item.** "Does this
look right?" only fires on absurdity. **P1/P2's error looked absurd** — screw
terminals aimed at an LQFP, no screwdriver access. **A mis-facing FPC looks
plausible** — a connector looks like a connector, and the mouth is a subtle
feature of a 4 mm body seen at an angle. Gestalt review catches absurdity; it
does not catch plausible-but-wrong. Replace it with a closed question that
names the feature: *"Which side of FPC2 does the cable go in?"* cannot be
answered without resolving the mouth in the image.

This matters because coverage was never the problem: `f1054f3`'s own
changed-file list includes `kicad/board3/renders/*.png` — the renders were
regenerated in the very commit that fixed P1/P2, and CI keeps them fresh. The
mis-facing FPCs were in that refreshed picture, at the moment of maximum
attention to part orientation. **Render freshness is not review.**

**6. Where a document asserts a physical fact about a part, tie it to a
measurement.** `docs/solutions/conventions/connector-approach-zones-are-mechanical-keepouts.md`
(2026-07-30) reserved "the full-width band north of FPC1/FPC2/FPC3" as cable
territory and measured those runways at 2.726 mm. That keepout is on the face
the cable never uses. The assertion was believed for three weeks and shaped a
placement rule because nothing tied it to the part.

## Why This Matters

**The defect ships.** A board can be electrically perfect and physically
unassemblable, and this class is invisible to every gate this repo runs,
because symmetric pads carry no orientation information. The instruments that
can see it are a human eye on a render and a checker someone remembered to
point at the right parts.

**The second-order cost is worse than the first.** A mechanical defect found
at the bench does not stop work — it gets worked around, under time pressure,
by someone who wants to see the panel light up. Here the workaround (fold the
FFC back to reach a south-facing mouth) inverted which face of the cable was
up; because this part takes contacts on **both** faces, a wrong-face cable does
not fail open, it mirrors the pinout. FPC2 pad 1 is `/+3V3` and pads 19/20 are
`/GND`, so +3V3 met the panel's ground: two hard shorts and a collapsed rail.
The board survived, flipping the cable fixed it, and first glass followed the
same day at 60 fps. **A workaround for a mechanical defect introduced an
electrical failure mode the original design could never have had.**

**And the fix was available for ten days**, in a change that was already
touching the renders and already had the class written down in its own commit
message.

## When to Apply

- **Immediately after any review finds a defect no automated check could have
  seen.** Symmetric-pad orientation is one instance; the general form is any
  defect whose signature is absent from the artifacts the gates read.
- **When writing or extending a checker for such a class** — audit its scoping
  predicate separately from its detection logic. Calibrating on one confirmed
  instance validates the *mechanism*, not the *scope*.
- **Before any fab order**, alongside the gerber/CPL diff: walk the connector
  table, every row, every time.
- **Whenever a placement guide or convention is written for edge-driven
  parts** — check that it constrains facing, not just position and order.
- **Whenever a bench workaround is invented for a mechanical problem.** Ask
  what electrical regime the workaround enters that the design never
  contemplated — reversed conductors, unmated shields, cables under
  compression, connectors mated at an angle.

## Examples

**The enumeration that should have shipped in `f1054f3`.** The property is
"orientation is not electrically observable"; the population is every part
that mates with something off-board:

| Part | Mates with | Arrives from | Points that way? |
|---|---|---|---|
| P1, P2 | CAN harness, screwdriver | outside the board edge | **was: no** — fixed in `f1054f3` |
| FPC1/2/3 | panel FFC from the cluster | outside the **north** edge (y=85) | **no** — mouth opens south; found 2026-08-20 |
| DC1 | barrel plug | outside the user edge | — |
| USBC1 | USB-C plug | outside the user edge | — |
| H2 / TC2030 | probe, from above | +Z | — |

Two of five rows were already answered by `f1054f3`'s own investigation. The
FPC row is the one nobody wrote down.

**The evidence one row needs, worked for FPC2.** North edge at y=85; part at
(150.0005, 90) rotation 0; 20 signal tails at y=88.675 and hold-downs at
91.325, so the contact row is on the north face of a 4.000 mm body; a BackFlip
part puts the actuator with the tails and takes the cable opposite; therefore
the mouth opens **south**. Intended: north. **Mismatch.** Every number comes
from the `.kicad_mod` and the board file.

**The scoping fix, concretely.** Rather than appending `FPC-SMD_5034802000` to
`WIRE_ENTRY_CLASSES` — which repeats the original error one part later —
select by structural precondition and carry exemptions explicitly:

```python
# judge every edge-adjacent connector-class footprint; a part with no mouth
# constraint must be named here, with a reason, so it is exempt on purpose
MOUTH_EXEMPT = {
    "H2_TC2030": "probe mates from +Z, no in-plane mouth",
}
```

The difference is what happens to the part nobody thought about: under an
allowlist it is silently skipped, under an exemption list it is reported.

## Related

- `f1054f3` (PR #36) — the P1/P2 fix and the sentence that should have
  triggered the enumeration.
- `tools/kicad_placement_lint.py` — check 2 `connector-mouth`; correct
  geometry, class-general docstring, instance-scoped `WIRE_ENTRY_CLASSES`.
  The concrete action this doc implies.
- `docs/solutions/design-patterns/calibrate-an-automated-reviewer-on-a-confirmed-defect.md`
  — the doctrine that produced that lint. Its verdicts (CALIBRATED / MISSED /
  UNREACHABLE / STALE) all describe whether a checker *fires* on its fixture;
  none describes a checker that fires correctly on a scope narrower than its
  own documented class. That is a fifth failure mode.
- `docs/solutions/conventions/connector-approach-zones-are-mechanical-keepouts.md`
  — the same three connectors; its "generalize per connector type" is the
  un-enumerated rule, and its north runway sits on the wrong side of the parts
  until they are rotated.
- `docs/solutions/developer-experience/derive-the-fab-viewers-rule-before-trusting-its-outlier.md`
  — that doc covers trusting the viewer's output; this one covers what you
  point it at, and records that the viewer resolves absurd but not plausible.
- `docs/solutions/developer-experience/clip-test-board-window-queries.md` —
  the same lesson one altitude up, for geometry predicates: "check every
  object type against that test before exempting any of them."
- `docs/solutions/conventions/drc-clean-and-measured-is-not-assemblable.md` —
  the family this defect belongs to: gates that measure copper cannot judge
  assembly.
- `docs/solutions/design-patterns/verifying-every-part-of-a-claim-does-not-verify-the-claim.md`
  — why the P1/P2 fix's own review never asked about FPCs: an audit that
  inherits the question inherits the blind spot.
- `docs/hardware/board3-pcb-layout-guide.md` — step 1.2 specifies edge and
  order but not facing.
