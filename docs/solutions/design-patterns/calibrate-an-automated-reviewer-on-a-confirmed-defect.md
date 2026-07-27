---
title: Calibrate an automated reviewer on a confirmed defect, and give the gate three outcomes
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
tags: [review-tooling, calibration, verification, false-positive, trust]
---

# Calibrate an automated reviewer on a confirmed defect, and give the gate three outcomes

## Context

Adopting `kicad-happy` as an automated PCB reviewer raised the obvious question:
its findings on unknown problems are worth nothing until it has reproduced a
problem we already knew about. So the harness was built to withhold trust until
it independently rediscovered a known defect.

The gate then failed twice, in two different ways, before it worked — and the
second failure was the useful one.

## Guidance

**A calibration case must be a defect independently confirmed to exist.** Not
"recorded as a finding", not "concluded in a prior session" — verified against
the artifact itself. Board3's calibration case was inherited as *"880 µF of bulk
capacitance on VBUS violates the USB-C 10 µF inrush limit"*, carried forward as
critical and fab-blocking. The reviewer reported nothing about it. Checking why
showed VBUS carries a single 100 nF capacitor; the four 220 µF electrolytics sit
on `+5V`, behind the ideal-diode ORing. **The premise was wrong, not the tool.**

**Require the finding to be *about* the defect, not merely near it.** The first
matcher accepted any finding containing `vbus` and confidently returned
`CALIBRATED` — against two `info`-level trace-width reports on nets merely
*named* `VBUS` and `VBUS_SENSE`. A net-name collision had certified the reviewer
as trustworthy, which is precisely the failure the gate exists to prevent. A
match now requires a term from every concept group plus a severity above `info`:

```python
CALIBRATION = {
    "all_groups": (
        ("single-pin", "single pin", "exactly one pin"),   # the defect concept
        ("btn1", "btn2", "btn3", "btn4"),                  # its subject
    ),
    "excluded_severities": ("info", "note", "debug"),
}

def matches(finding):
    if finding["severity"].lower() in CALIBRATION["excluded_severities"]:
        return False
    blob = " ".join(str(v) for v in finding.values()
                    if isinstance(v, (str, int, float))).lower()
    return all(any(t in blob for t in group) for group in CALIBRATION["all_groups"])
```

**Give the gate three outcomes, not two:**

| Verdict | Meaning | What it says about the tool |
|---|---|---|
| `CALIBRATED` | defect reachable in this input, and found | trustworthy for this input class |
| `MISSED` | defect reachable in this input, and **not** found | unreliable — do not act on its other findings |
| `UNREACHABLE` | the input **cannot contain** the defect | nothing at all |

Only `MISSED` is a verdict about the tool. The reviewer here was first run
against a PCB with no schematic, while the expected defect was a schematic-level
fact. Collapsing `UNREACHABLE` into `MISSED` would have condemned a working
reviewer for input it was never given.

## Why This Matters

An uncalibrated reviewer is worse than no reviewer, because its output looks
like evidence. Both failure modes here produced confident, specific, wrong
answers: one certified a tool on a coincidence, the other would have discarded a
working tool on a bad premise.

The gate paid for itself in an unexpected direction. It was built to test the
reviewer; the first time it ran for real it **disproved the calibration case** —
overturning a finding that had been carried as critical and fab-blocking. A
mechanism that can only ever validate a tool is much less useful than one that
can also invalidate what you thought you knew.

## When to Apply

- Before acting on any automated analyzer's findings about things you cannot
  independently check
- When a tool reports nothing on a defect you are confident exists — investigate
  the premise before concluding the tool is weak
- When writing any matcher that decides whether output "counts": ask what else
  could satisfy it, and whether that would be a coincidence

## Examples

The replacement calibration case satisfies the bar the original never did.
Board3's `BTN1`–`BTN4` are single-pin nets: each pull-up net terminates at
`R28.2`–`R31.2` and was never joined to its switch's `BTN*_SW` net, leaving four
dead buttons. That was found by hand in the netlist first, then reproduced
unprompted by the analyzer as `NT-001` warnings naming the exact pins.

Found by hand, then rediscovered by the tool, independently — which is what a
calibration case is.

## Related

- [Refill zones before measuring a headlessly routed board](../developer-experience/refill-zones-before-measuring-a-headlessly-routed-board.md) — a measurement that looked precise and was wrong by an order of magnitude
