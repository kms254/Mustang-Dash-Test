---
title: "Prove the load path with max-flow before costing a trunk"
date: 2026-08-08
category: design-patterns
module: kicad/board3
problem_type: design_pattern
component: tooling
severity: medium
applies_when:
  - "A layout-guide row names a minimum trunk width and a net carries segments below it"
  - "Deciding whether a narrow segment is series load-path or a parallel branch / sense spur"
  - "A triage declined to cost a finding because series-vs-parallel was unproven"
  - "Verifying that a copper fix actually moved a bottleneck, in the same units it was found in"
tags: [kicad, pcbnew, max-flow, power-distribution, trunk, load-path, min-cut, calibration, board3]
---

# Prove the load path with max-flow before costing a trunk

## Context

A power net's *narrowest segment* and its *bottleneck* are different things. A
0.254 mm stub feeding a sense divider constrains nothing; a 0.400 mm segment
that every ampere crosses constrains everything. The 2026-08-03 review triage
explicitly declined to cost Board3's `/VBUS` narrow segments for exactly this
reason — the net spans three layers through four vias, and nobody had proven
which narrow copper was series and which was parallel. "Narrowest width on the
net" is not a risk figure; it is an upper bound on ignorance.

The barrel input (`/+5V_BARREL`) got its answer by hand — a manual trunk walk,
which is what made U51's +8 °C figure trustworthy. `/VBUS` got it computed,
during the 2026-08-07 pre-order review (session work; the resulting copper fix
landed in PR #30). The method is worth keeping because it converts "declined
to cost" into a measured min-cut in one pass, and because its validation
contract is reusable even where its code is not.

## Guidance

Model the net's copper as a flow network and ask for the maximum flow between
the real source and the real sink:

1. **Nodes and edges from items, connectivity from geometry.** Each track
   segment, via, and pad on the net is an element. Two elements connect when
   their **copper overlaps** — exact segment-segment overlap math, via
   annulus against track ends, pad shapes against landing segments. Never
   connect by endpoint coincidence (see the instrument bug below).
2. **Capacity = width, derated by layer.** A segment's capacity is its width
   times a copper-weight factor (Board3: 1 oz outer = 1.0, 0.5 oz inner =
   0.5), so the flow value reads directly in "mm-equivalent of 1 oz copper".
3. **Source and sink are the electrical endpoints**, not net extremities:
   here USBC1's VBUS pads (A4B9 + B4A9 in parallel) to Q1's source pads.
4. **Max-flow gives the bottleneck; the min-cut names the conductors.** The
   flow value is the trunk's effective width; the cut set is the exact copper
   that binds it. Everything narrow that is *not* in the cut is thereby
   proven parallel or spur — "proven, not assumed" is the entire point.
5. **Convert the min-cut to consequence** with the ordinary IPC-2221
   arithmetic at the documented worst-case current. On `/VBUS`: min-cut
   0.400 mm → ~+19 °C at 1.65 A; after the fix, 0.650 mm → ~+9 °C.

What the method proved on Board3 that inspection had not:

- The min-cut was **exactly one conductor** — a 0.400 mm × 3.6 mm Top segment.
- The 0.254–0.290 mm segments elsewhere on the net are **sense spurs**
  (U5.3/U5.6, C53, R6/R7 taps), not load path.
- Upstream, the two USBC1 pad branches carry load **in parallel** (~0.77
  summed), so the 0.640 mm segment there does not bind.
- After the fix, re-running the same instrument showed the cut **moved** to
  the 0.650 mm downstream run — the before/after is in the same units because
  it is the same measurement.

## The calibration contract — this is what makes the number trustworthy

The method's first output was wrong, and the way it was caught is the
transferable half of this doc.

**The instrument bug:** the first graph connected elements by **1 µm endpoint
coincidence** and reported 0.370 mm-equivalent with a different min-cut. It
was blind to via-annulus and track-overlap connections — the same
reference-point-instead-of-shape failure this repo has now hit on window
queries, pads, courtyards, and slotted drills (see
[window-filter by shape, never by reference point](../developer-experience/clip-test-board-window-queries.md)).
Connectivity is a shape question; endpoints are where shape questions go wrong.

**The control net:** before believing any `/VBUS` number, the same code ran on
`/+5V_BARREL`, whose answer was already known from U51's manual walk and
documented (+8 °C at 2 A through 0.900 mm). The corrected instrument
reproduced **0.900, same cut segment** — and reproduced it *again* after the
`/VBUS` edit, proving the instrument stable across the change it was
measuring. A control with a known answer is what
[calibrate an automated reviewer on a confirmed defect](calibrate-an-automated-reviewer-on-a-confirmed-defect.md)
prescribes for reviewers; a measurement tool deserves exactly the same
treatment, and here it caught a 7.5% error and a misidentified cut before
either reached a decision.

Both runs matter: **control-before** catches a broken instrument;
**control-after** catches an instrument the edit itself broke.

## When to Apply

- Any time a guide rule ("input trunk ≥ 60 mil") meets a net whose topology
  is not a single visible run — multiple layers, vias, parallel branches.
- Before accepting *or dismissing* a "narrow copper" review finding: the
  method both confirmed `/VBUS` (real, one conductor) and would have
  dismissed a sense-spur finding for free.
- After the fix, re-run rather than re-argue: the bottleneck should move and
  the value should rise, in the same units.
- Not for nets where a visual trunk walk is trivial (single-layer, single
  run) — U51's manual walk was cheaper and equally conclusive.

## Caveats

- The temperature figures are IPC-2221 estimates on the min-cut conductor,
  not thermal simulation; treat them as ranking, not prediction.
- The implementation lived in review-session code and is **not in the tree**.
  This doc records the algorithm and its calibration contract so a
  reimplementation starts from the method, not from scratch; if the need
  recurs, it belongs in `tools/` with the control-net check built in as a
  self-test.
- Capacity-as-width assumes uniform thickness per layer class; exotic
  stackups need real per-layer weights.

## Related

- [calibrate an automated reviewer on a confirmed defect](calibrate-an-automated-reviewer-on-a-confirmed-defect.md)
  — the control-net discipline is this pattern applied to a measurement tool.
- [window-filter by shape, never by reference point](../developer-experience/clip-test-board-window-queries.md)
  — the instrument bug (endpoint-coincidence connectivity) is that family's
  fifth appearance, this time inside a graph builder.
- [a tool's finding can be a property of the tool](../developer-experience/a-tools-finding-can-be-a-property-of-the-tool.md)
  — the first 0.370 reading was exactly this; the control net is what caught it.
- `fab/ORDER.md` — the `/VBUS` reinforcement record carries the method's
  before/after numbers (0.400 → 0.650 mm-eq, +19 → +9 °C) and the deliberate
  decision to stop at parity with the barrel path.
- CLAUDE.md, U50/U51 notes — the manual trunk walk that produced the control
  net's known answer.
