---
title: "A plan's instructions are authoritative; a plan's facts are hypotheses"
date: 2026-08-03
category: conventions
module: kicad/board3
problem_type: convention
component: tooling
severity: high
applies_when:
  - "A plan says to label, mark, or print a value onto hardware (silk, BOM, order form)"
  - "A plan says to correct value A to value B, where B is measurable from the design"
  - "Executing any planning document written before the work it describes"
  - "A plan states a fact with a hedge, e.g. 'X (or the confirmed value)'"
tags: [planning, verification, silkscreen, kicad, board3, ktd27, safety]
---

# A plan's instructions are authoritative; a plan's facts are hypotheses

Three assertions in one plan document were wrong on the same branch, in one
session. Each was caught by a different mechanism, and the third would have
destroyed hardware.

This is KTD27 — *verify by pin FUNCTION, not pin number* — generalised from pin
numbers to **any fact a planning document states**.

## Context

`docs/plans/2026-08-02-001-fix-board3-review-blockers-plan.md` is a good plan.
It is detailed, it states how each unit is verified, and its instructions were
right. The failures were in its *facts*.

**1. The 12 V label that would have killed the board.** Unit U45's table said to
silkscreen the barrel jack `12V (or the confirmed supply rating)`. Nothing in the
repo confirmed a rating, so the label went on as `DC IN` and the gap was flagged.
Tracing the power tree to answer the question later:

    DC1 → /DC1_IN → F1 (PTC) → /+5V_BARREL → Q2 (reverse-polarity FET) → /+5V

There is no regulator in that path. `U3` (TPS563201) has its `VIN` on `/+5V` — it
is the 5 V→3.3 V buck, *downstream*. **The barrel jack is the 5 V rail.** A `12V`
legend would have instructed someone to put 12 V onto a rail feeding the CAN
transceivers, both AW9523B expanders (6 V absolute maximum), eight LEDs, and
pins 17/18 of all three panel connectors — destroying the board and up to three
panels, on the authority of a silkscreen label. It now reads `5V ONLY`.

**2. Silk that encoded a reversed mapping.** The same table said
`FPC1/FPC2/FPC3 → CENTER/LEFT/RIGHT`. The nets say `FPC1` carries `/SCLK_L` and
`FPC2` carries `/SCLK_C`. The labels were placed straight from the table; a
seven-lens review caught it as its only CRITICAL. U45 exists *because* "plugging
the centre panel into the wrong FPC is a real bench mistake and `FPC2` does not
protect against it" — so the safeguard would have caused the exact error it was
added to prevent, with **more** confidence than no label.

**3. A count that was stale before it was written.** Unit U39 said to correct
"198 vias" to "205". By the time the plan was authored, three later units had
each added vias; the board measured 222. Replacing one hardcoded count with
another only resets the clock.

## Guidance

**Before writing any plan-supplied fact onto hardware or into a design decision,
re-derive it from the design.**

The distinction that matters:

| | authority | why |
|---|---|---|
| A plan's **instructions** | authoritative | deciding what to do is the plan's job |
| A plan's **facts** | hypotheses | they were true at authoring time, or never |

Concretely, before acting:

```bash
# Net-level truth, not the plan's table
kicad-cli sch export netlist --format kicadsexpr -o net.net "<sch>"

# Which channel does this connector actually carry?
#   FPC1 → /SCLK_L, /MOSI_L, /MISO_L   → it is the LEFT panel
```

**The detection heuristic.** The danger sign is a plan sentence shaped like
*"label X as Y"* or *"correct A to B"*, where `Y`/`B` is a **measurable property
of the design** rather than a decision. Those are the ones to re-derive. A plan
saying "use a Tag-Connect instead of a header" is a decision — accept it. A plan
saying "the jack is 12 V" is a measurement — check it.

**Hedged facts are the highest-risk kind.** U45's text literally said
`12V (or the confirmed supply rating)`. The parenthetical *was* the plan telling
us it did not know — but once skipped, the sentence reads as authoritative. Treat
a hedge as a flag, never as coverage.

**Prefer deleting a fact to correcting it.** U39's fix was not to write "222"
where "198" stood; a hardcoded count rots on the next change. `tools/kicad_rules.json`
now says how to count vias and states no number. A fact that will rot is not
fixed by a fresher fact.

## Why This Matters

The failure is not carelessness in the plan. It is that **execution treats a
plan's facts with the same authority as its instructions**, and only the
instructions earned it. A plan is written before the work, from the state at
authoring time, and nothing re-checks it afterwards — so its facts decay silently
while its instructions stay valid.

The cost asymmetry is extreme. Verifying the supply rating was one netlist query.
Not verifying it puts a legend on the board telling a technician to destroy it,
and silkscreen is *trusted* precisely because it is physical and permanent.

Note the direction of failure in cases 1 and 2: **both were safety features.** A
voltage marking and a connector legend both exist to prevent mistakes. A
safeguard carrying a wrong value is worse than no safeguard, because it converts
a careful person's caution into confident error.

## When to Apply

- Any silkscreen legend stating a voltage, polarity, channel, or identity
- Any BOM value, part number, or quantity a plan supplies
- Any order-form field (board size, drill, finish, assembly service)
- Any number a plan asks you to "correct" — check whether it should exist at all
- Any plan fact carrying a hedge, an "approximately", or a parenthetical

## Examples

Wrong — the plan is the source:

```text
plan: "FPC1 / FPC2 / FPC3 → CENTER / LEFT / RIGHT"
→ place CENTER at FPC1                      # CRITICAL: FPC1 is the LEFT channel
```

Right — the design is the source, the plan is the prompt:

```text
plan: "label the FPCs by function"           # instruction: accept
→ read each FPC's SCLK net from the netlist  # fact: derive
   FPC1 /SCLK_L → LEFT
   FPC2 /SCLK_C → CENTER
   FPC3 /SCLK_R → RIGHT
```

When a fact is corrected, correct it **in the plan too**, with a note saying it
was wrong — the next person reads the same row. U39's and U45's tables were
amended in place rather than silently superseded.

## Related

- `CLAUDE.md` KTD27 — "verify by pin FUNCTION, never pin number". This doc is
  that rule generalised from pin numbers to any plan-supplied fact.
- [Window-filter board geometry by shape intersection](../developer-experience/clip-test-board-window-queries.md)
  — the same shape one level down: guidance that was wrong, followed correctly,
  and which caused the incident it was meant to prevent.
- [lib_footprint_mismatch is a real diff](../integration-issues/kicad-lib-footprint-mismatch-integer-nanometre-comparison.md)
  — integer nanometres as the unit of truth; another case of a plausible value
  being the wrong value.
- [A gate that cannot pass gets waved through](a-gate-that-cannot-pass-gets-waved-through.md)
  — the sibling convention: a signal nobody expects to change stops being read.
- `docs/reviews/2026-08-03-board3-review-triage.md` — the review that caught
  case 2 as its only CRITICAL.
