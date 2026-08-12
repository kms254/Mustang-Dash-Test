---
title: "\"DRC clean and measured\" is not \"assemblable\""
date: 2026-07-27
category: conventions
module: kicad-verification
problem_type: convention
component: tooling
severity: high
applies_when:
  - "Declaring a PCB ready for fabrication or assembly"
  - "A board passes DRC and a geometric measurement harness and is called done"
  - "Adding, reviewing, or trusting a CI gate over board files"
tags: [kicad, pcb, dfm, drc, verification, ci, assembly]
---

# "DRC clean and measured" is not "assemblable"

## Context

Board3 was verified two ways before assembly. `tools/kicad_verify.py` runs DRC
against the real JLCPCB rules and reports new violations over a baseline;
`tools/kicad_measure.py` reports per-net length, via count, layer set, and a
net-by-net diff against a previous board. Both were green.

Six QSPI escape vias were sitting inside U2's SMD pads — the flash's six signal
pins. U6's reroute of the bus to the bottom layer left the router nowhere to
escape those pads, so it put the vias in them. An unplugged 0.3 mm barrel holds
more solder volume than a small pad's entire paste deposit and wicks the joint
dry during reflow.

`kicad_verify.py` reported 35 violations with NEW = 0 over the 41-violation
import baseline. `kicad_measure.py` reported nothing at all, because a via
inside a pad is not a length, a via count, or a layer. The defect surfaced only
because `tools/kicad_review.py` was run for an unrelated thermal-via finding,
and kicad-happy's pad audit mentioned it in passing (`763aff9`).

## Guidance

**Manufacturability is a third category of check.** It is not a strict subset of
either of the other two, and a board can pass both and be unbuildable:

| Question | Tool | What it models |
|---|---|---|
| Does the copper obey the rules? | DRC (`kicad_verify.py`) | clearances, widths, hole sizes, annular rings |
| Is the copper what I intended? | measurement (`kicad_measure.py`) | lengths, via counts, layer sets, net diffs |
| Can a factory build this? | **nothing project-owned** | reflow, plating, assembly, chassis |

DRC checks *minima*. Anything a fab publishes as a number is encodable, and
should be — U7 found `min_via_annular_width` had never been written into
`tools/kicad_rules.json`, so four units of DRC measured rings against the
importer's 0.1 mm default. But via-in-pad is not a minimum. It is a relationship
between two features that are each individually legal, and no threshold
expresses it.

A measurement harness checks *intent*, and only the intent you thought to
measure. `kicad_measure.py` exists to answer "what did this net become" and "did
anything else move". A via that lands in a pad changes neither the net's via
count nor any other net, so the instrument is silent by construction.

**The signature of this category is a check that returns nothing, not a check
that returns something wrong.** Three Board3 defects share it exactly:

- **Six QSPI vias in U2's SMD pads.** Every check green. Found by a third-party
  pad audit run for another reason (`763aff9`).
- **The Top GND pour translated (+0.867, -2.539) mm** from the other three zones
  and from the board outline, leaving strips of board with no top-layer ground.
  It arrived that way in the original EasyEDA import (`52586d4`, byte-identical)
  and survived every check for the whole conversion, because DRC has no opinion
  about where a pour chooses to stop. Spotted by eye in the editor; fixed in
  `4f0913a`.
- **The four M3 mounting holes on no net.** `Pad_e672`/`673`/`674`/`675`, 3.2 mm
  drill with a 5 mm annulus, four isolated copper islands at the corners of a
  board destined for a metal chassis. Also fixed in `4f0913a`.

**The check that closes the category is a pad-occupancy audit, and it is cheap.**
For every SMD pad, assert no via barrel intersects it:

```python
import pcbnew

board = pcbnew.LoadBoard(path)
vias = [t for t in board.Tracks() if isinstance(t, pcbnew.PCB_VIA)]
for fp in board.Footprints():
    for pad in fp.Pads():
        if pad.GetAttribute() != pcbnew.PAD_ATTRIB_SMD:
            continue
        for v in vias:
            if pad.HitTest(v.GetPosition()):
                print(f"via-in-pad: {fp.GetReference()}.{pad.GetNumber()}")
```

A centre-point test is the cheap version and would have caught all six; widen it
to the barrel radius to catch the edge case where only part of the hole
overlaps. The same shape of check covers the other two defects — compare every
zone outline against the board outline inset, and assert every PTH pad carries a
net.

**It must run where the DRC gate already runs, not after it.** Board3's DRC is
gated in `.github/workflows/kicad-drc-erc.yml` on every PR and again in
`fab-release.yml` before an order is placed. A DFM check that runs only when
someone remembers is not a gate, and this one was found by hand precisely
because nothing ran it.

**Give it a recorded-exception mechanism from the start.** One via-in-pad still
ships: `U1.93` (BTN1). It cannot leave its pad — 0.28 × 2.0 mm QFP pad on 0.5 mm
pitch, a `+3V3` diagonal 1.6 mm east and a load-bearing GND trace 0.295 mm west
— and it cannot grow, because the neighbouring pads cap the diameter at
0.517 mm. Its hard breach was the annular ring, so the drill shrank instead:
0.5/0.2 gives exactly the 0.15 mm ring JLC requires. A gate with no way to record
that is a gate that goes permanently red, which is how it gets waved through.
`kicad_verify.py`'s cost-floor advisory already solves this shape of problem —
copy it.

## Why This Matters

Board3 has eight CI workflows. They check DRC, ERC, LCSC mapping, duplicate part
numbers, stale renders, stock, the fab archive's contents, and host tests. None
of them checks whether the board can be assembled.

The one tool that found this — `tools/kicad_review.py`, wrapping kicad-happy —
is not in any workflow, and it resolves its analyzers from a clone at
`~/Code/kicad-happy` that no CI runner has. So the only DFM coverage in the
project is a manual invocation against a checkout outside the repo. That is why
this is a structural gap rather than a fixed bug: nothing stops the next
reroute putting vias back in those pads.

The cost asymmetry is what makes it worth a gate. A DRC violation is caught in
review; a starved joint on six of a flash chip's signal pins is caught after the
panel is built and populated, on a board that measured perfect.

## When to Apply

- Before declaring any board fab-ready, regardless of how green DRC and the
  measurement harness are
- After any autorouter run or bulk reroute — a router optimizes for the rules it
  was given, and will happily satisfy every one of them unbuildably
- When adding a verification tool: ask which of the three categories it covers,
  and say so in its docstring
- When a check "found nothing" on an area you changed heavily — that is the
  signature, not reassurance

## Examples

The same reroute produced both a caught and an uncaught defect, which is the
clearest illustration of the boundary. U6's QSPI work put six vias in U2's pads
(uncaught by everything) and left `R28.2`'s via in its pad too (also uncaught).
When the walk-back was applied, the *replacement* hand-route was rejected
immediately — it produced 8 `shorting_items` and a clearance violation, all
visible to DRC in seconds (`tools/handroutes/README.md`).

DRC caught the bad fix instantly and the original defect never. The difference is
not severity; it is whether the defect is expressible as a rule.

## Related

- [Calibrate an automated reviewer on a confirmed defect](../design-patterns/calibrate-an-automated-reviewer-on-a-confirmed-defect.md) — the harness that eventually found this, and why its output is only worth acting on once calibrated
- [Refill zones before measuring a headlessly routed board](../developer-experience/refill-zones-before-measuring-a-headlessly-routed-board.md) — the same pipeline failing the other way, reporting confident violations that were not real
- [Migrating a board from EasyEDA Pro to KiCad loses data silently](../integration-issues/easyeda-pro-to-kicad-migration-silent-data-loss.md) — where the misaligned Top GND pour and the netless mounting holes came from
- [Derive the fab viewer's rule before trusting its outlier](../developer-experience/derive-the-fab-viewers-rule-before-trusting-its-outlier.md) — a fourth member of the "check that returns nothing" family: orientation of a symmetric-pad part is invisible to every electrical check (the netlist is identical both ways), so the fabricator's order-time 3D render is the only instrument that can see it — and that instrument's own model must be verified before its testimony is admitted
- [Diff the fab's production file before confirming the order](../developer-experience/diff-the-fabs-production-file-before-confirming-the-order.md) — a fifth member, the purest drill-side case: a footprint missing holes it should have (USBC1's locating pegs, modeled as Edge.Cuts artwork that reaches neither Excellon file) is green in DRC, parity, measurement, and sourcing audits by construction — and it supplies the "Can a factory build this?" row with a real instrument at last: the factory's own CAM output, diffed at the paid checkpoint, where their engineer's silent fix was found already applied
