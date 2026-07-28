---
title: Calibrate an automated reviewer on a confirmed defect, and make the fixture detect its own repair
date: 2026-07-27
category: design-patterns
module: review-tooling
problem_type: design_pattern
component: tooling
severity: high
applies_when:
  - "Adopting an automated analyzer, linter, or reviewer whose findings will be acted on"
  - "Deciding whether a tool's output is trustworthy before believing what it says about unknowns"
  - "A review tool reports nothing on a defect you are certain exists"
  - "Choosing which known defect a calibration gate should probe for"
  - "A canary or calibration check has been failing since the thing it probes was fixed"
tags: [review-tooling, calibration, verification, false-positive, trust, fixtures]
---

# Calibrate an automated reviewer on a confirmed defect, and make the fixture detect its own repair

## Context

`tools/kicad_review.py` drives `kicad-happy`'s analyzers over Board3 and withholds
trust until the reviewer independently rediscovers a defect we already know is
there. The gate has now failed in four distinct ways. Three were about the
reviewer's output. The fourth was about the fixture, and it is the one nobody
plans for.

**The calibration defect got fixed.** The probe was Board3's `BTN1`–`BTN4`
single-pin nets. Unit U2 repaired them (`8f96e01`, 17:34). From that moment every
run printed `MISSED` — whose documented meaning is *"the reviewer is unreliable;
do not act on its other findings"* — about a reviewer that was working perfectly.
The verdict stayed wrong from U2 until U7 diagnosed it, and the plan's
verification contract runs this gate at U2, U3, U7, U8 and U9.

A fixture that cannot notice its own repair converts every later success into a
false alarm.

## Guidance

**A calibration case must be a defect independently confirmed to exist.** Not
"recorded as a finding", not "concluded in a prior session" — verified against
the artifact itself. Board3's first case was inherited as *"880 µF of bulk
capacitance on VBUS violates the USB-C 10 µF inrush limit"*, carried forward as
critical and fab-blocking. The reviewer reported nothing about it. Checking why
showed VBUS carries a single 100 nF capacitor; the four 220 µF electrolytics sit
on `+5V`, behind the ideal-diode ORing. **The premise was wrong, not the tool.**

**Require the finding to be *about* the defect, not merely near it.** The first
matcher accepted any finding containing `vbus` and confidently returned
`CALIBRATED` — against two `info`-level trace-width reports on nets merely
*named* `VBUS` and `VBUS_SENSE`. A net-name collision had certified the reviewer
as trustworthy, which is precisely the failure the gate exists to prevent. A
match now requires a term from every concept group plus a severity above `info`
(`matches_calibration`, `tools/kicad_review.py:198`):

```python
CALIBRATION = {
    "all_groups": (
        ("usbc1",),                                        # its subject
        ("courtyard", "board edge", "board outline"),      # the defect concept
    ),
    "excluded_severities": ("info", "note", "debug"),
}
```

**Give the probe a presence check, evaluated against the design files and
independent of anything the reviewer said.** This is the half the original
version of this pattern missed. `defect_still_present()` reads the board and
looks for tokens that exist only while the defect does:

```python
"presence": {
    "in": "board",
    "contains": ["USB-C_SMD-TYPE-C-31-M-12_1", "(at 30 100 -90)"],
}
...
def defect_still_present(board, schematic) -> bool | None:
    text = target.read_text(encoding="utf-8", errors="ignore")
    return all(token in text for token in probe["contains"])
```

Cheap, no KiCad, no reviewer. Move USBC1 off `(at 30 100 -90)` and it returns
`False`.

**Fail closed when presence is unknowable.** `defect_still_present()` returns
`None` if the probe cannot be evaluated, and `calibrate()` treats `None` like
"still there" — the verdict is `MISSED`. An unevaluable fixture must not be able
to excuse a reviewer.

**Give the gate four outcomes, not two:**

| Verdict | Meaning | What it says about the tool | Exit |
|---|---|---|---|
| `CALIBRATED` | defect reachable in this input, and found | trustworthy for this input class | 0 |
| `MISSED` | defect reachable in this input, and **not** found | unreliable — do not act on its other findings | 3 |
| `UNREACHABLE` | the input **cannot contain** the defect | nothing at all — get the schematic | 0 |
| `STALE` | the defect is **no longer in the design** | nothing at all — repoint the fixture | 4 |

Only `MISSED` is a verdict about the tool. `UNREACHABLE` is about the input:
the reviewer was first run against a PCB with no schematic while the expected
defect was a schematic-level fact. `STALE` is about the fixture. Collapsing
either into `MISSED` condemns a working reviewer.

`STALE` still exits non-zero, and with its own code — the fixture is broken and
somebody must fix it. It just accuses the right party.

**Choose a defect that is confirmed *and* that nobody is about to fix.** These
two criteria pull against each other, and that tension is the real content of
this lesson. The best-confirmed defects are the ones with an owner and a due
date. The BTN fixture was pointed at the single-pin nets at 12:19 (`18a2e0a`);
the plan that scheduled their repair as unit U2 was written at 14:01
(`a0434a5`), one hour and forty-two minutes later, and already contained the
line *"U2. Join BTN pull-ups to their switches"*. The fixture was aimed at
something the project had already decided to destroy.

Prefer a fact about the artifact that is real, checkable, and structurally
frozen. USBC1's courtyard overhang qualifies on the third count because the
connector's position is fixed by the board edge it mates through.

**Confirm the fixture against the same fact, not a neighbouring one.** The
"about it, not near it" rule applies to your evidence as hard as it applies to
the matcher — see the correction in Examples.

## Why This Matters

An uncalibrated reviewer is worse than no reviewer, because its output looks
like evidence. Every failure here produced a confident, specific, wrong answer:
one certified a tool on a coincidence, one would have discarded a working tool
on a bad premise, and one blamed a working tool for its own obsolescence.

The stale-fixture failure is the most corrosive of the three because it is
self-reinforcing. A gate that is always red stops carrying information, and a
gate that carries no information gets waved through — at which point the *next*
genuine `MISSED` is invisible too. The mechanism the gate exists to prevent is
exactly the mechanism a stale fixture installs.

The gate has also paid for itself in an unexpected direction. It was built to
test the reviewer; the first time it ran for real it **disproved the calibration
case**, overturning a finding that had been carried as critical and fab-blocking.
A mechanism that can only ever validate a tool is much less useful than one that
can also invalidate what you thought you knew.

## When to Apply

- Before acting on any automated analyzer's findings about things you cannot
  independently check
- When choosing what a calibration or canary probe should look for: check the
  open plan first, and reject anything a scheduled unit is going to remove
- Whenever a check that probes for a known condition starts failing — establish
  whether the condition is still there before believing the check
- When a tool reports nothing on a defect you are confident exists — investigate
  the premise before concluding the tool is weak
- When writing any matcher that decides whether output "counts": ask what else
  could satisfy it, and whether that would be a coincidence

## Examples

**The current fixture, measured.** USBC1's `F.CrtYd` rectangle runs
`(-5.175, -3.374)` to `(5.175, 5.3695)`; the footprint is placed
`(at 30 100 -90)`, which puts the courtyard's west extent at x = 24.6305 mm. The
`Edge.Cuts` outline starts at x = 25.0 mm. The overhang is **0.3695 mm**, derived
from the `.kicad_pcb` with no reviewer involved. `kicad-happy` reports
`pcb:PM-002:usbc1`, severity `error`, `edge_clearance_mm: -0.37`. Two derivations,
two codebases, same number — which is what a calibration case is.

**A correction to the evidence recorded in the harness.**
`tools/kicad_review.py` claims the case is *"independently confirmed"* by
*"KiCad DRC, 10 copper_edge_clearance violations at USBC1"*. The ten violations
are real and reproducible. **They are not about the overhang.** Every one sits at
x ≈ 31.25, y ≈ 97.47 or 103.27 — USBC1's two internal `Edge.Cuts` peg cutouts,
against its own pads and `GND` tracks. The overhang is 6 mm west of there, at the
outer outline. KiCad has no DRC rule for a courtyard crossing the board outline
at all, so its DRC cannot corroborate this defect in principle. Of the DRC
violations naming USBC1, ten are `copper_edge_clearance` and three are
`silk_edge_clearance`; none is the overhang.

The `VBUS` matcher accepted a finding that shared a *name* with the defect. This
`confirmed_by` string accepts violations that share a *component* with it. Same
error, one level up, in the evidence rather than the matcher — and it survived
review because the count was specific and the component was right.

**The gate today**, `python tools/kicad_review.py kicad/board3/`:

```text
findings    : 131  error=14, info=84, warning=33  [pcb=66, schematic=65]
calibration : CALIBRATED
              reviewer independently reported: USBC1's courtyard overhangs the
              board edge (0.37 mm) (finding pcb:PM-002:usbc1)
annotated   : 10 finding(s) matched a known false positive (KO-001, PR-003, TV-001)
```

Rewrite USBC1's `(at 30 100 -90)` and the same run returns `STALE` with
`defect_still_present: false`, naming the fixture rather than the reviewer.

## Related

- [A gate that cannot pass gets waved through](../conventions/a-gate-that-cannot-pass-gets-waved-through.md) — the same social failure reached from the other side, and the source of the "calibration rot" the advisory docstring names
- [Refill zones before measuring a headlessly routed board](../developer-experience/refill-zones-before-measuring-a-headlessly-routed-board.md) — a measurement that looked precise and was wrong by an order of magnitude
