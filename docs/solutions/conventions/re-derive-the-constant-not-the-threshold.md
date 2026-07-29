---
title: "When a model change breaks pinned invariants, re-derive the constant"
date: 2026-07-25
category: conventions
module: dash-simulation
problem_type: convention
component: testing_framework
severity: high
applies_when:
  - "a physics, pricing, scheduling, or other model change makes several pinned test invariants fail at once"
  - "the quickest way to green is widening a band or bumping a tolerance"
  - "a test probes a subject's intrinsic property and the change added a new upstream input"
tags:
  - testing
  - invariants
  - calibration
  - simulation
  - tuning-constants
---

# When a model change breaks pinned invariants, re-derive the constant

## Context

Giving the TRACK simulator progressive corner exits broke five assertions in the sim suite at once: the driven-arc-matches-geometry check, `DASH_LAP_BURN_GAL` agreement, "a 20-minute session burns roughly half a tank", "a tank spans roughly two sessions", and the laps-remaining agreement.

Five failures from one change reads like stale tests. Widening five bands would have taken minutes and produced a green suite. It would also have destroyed the only thing those numbers were for.

## Guidance

**A pinned invariant that fails after a model change is a measurement, not an obstacle.** Read it before editing it:

1. **Ask what number the model now produces, and whether that number is defensible.** Here it was not. The first attempt unwound corners from mid-arc and ran a 1:52.4 lap against the 2:01.6 the model was calibrated to — the tests were correctly reporting that the change was too generous, handing the car half of every corner.
2. **Re-derive the constant against the external reference, then re-measure.** `SIM_CORNER_APEX_FRAC` was calibrated against lap time (0.50 → 1:52.4, 0.75 → 1:57.2, 0.85 → 1:59.7) and burn was re-measured at 0.618 gal/lap, moving `DASH_LAP_BURN_GAL` from 0.59 to 0.63 in `MustangDash/dash_math.h`. **All five assertions then passed with no threshold edited** — because they were encoding real relationships, not arbitrary bands.
3. **When a test genuinely needs updating, change its setup, not its assertion.** Two U2 probes in `tests/test_dash_sim.c` also failed, and legitimately: they measure the *car* ("at full throttle, acceleration is traction-limited"), and the new rate-limited pedal starts closed, so they were measuring the roll-on. The fix was to open `throttle_frac` before measuring. The claims under test never moved.
4. **Only loosen a threshold when the invariant's *intent* changed** — and say so in the test comment, with the new intent stated. The one deliberate divergence here (laps-remaining now excludes an unusable fuel reserve) was written as burn-rate agreement **plus a named offset**, so a genuine drift in the burn constant still fails.

## Why This Matters

Thresholds are the compressed output of past calibration work. A band widened to accommodate a change discards the evidence that produced it, and the test survives as a shape that can no longer fail. The suite still reports 14/14 and no longer defends anything.

The asymmetry is what makes this worth a convention: re-deriving cost about three measurement runs, while loosening five bands would have shipped a simulator 9 seconds a lap too quick, with a green suite asserting it was fine.

## When to Apply

- Any change to a calibrated model — physics, pricing, scheduling, ranking, capacity — that trips more than one pinned assertion.
- Any single assertion that fails where the tolerance is the tempting edit.
- Not applicable when the invariant was always arbitrary (an unmeasured "reasonable" bound). Those can move — but note the arbitrariness in the test so the next reader knows which kind it is.

## Examples

Deliberate divergence, written so it cannot mask drift (`tests/test_dash_sim.c`):

```c
/* The agreement is now offset by the reserve, deliberately: LAPS counts USABLE
 * laps ... Written as burn-rate agreement PLUS a named offset rather than a
 * loosened tolerance, so a genuine drift in the burn constant still fails. */
const float reserve_laps = DASH_FUEL_RESERVE_GAL / DASH_LAP_BURN_GAL;
expect(fabsf((float) laps - (predicted + reserve_laps)) <= 1.5f, "...");
```

## Related

- [A calibration knob with no leverage is usually fighting a bug](../logic-errors/calibration-knob-with-no-leverage.md) — the same suite catching a coupling bug rather than needing a wider band
- [Removing a discontinuity took two mechanisms](../design-patterns/continuity-and-slew-are-two-separate-fixes.md) — the model change that triggered these five failures
- PR #8 (merged 2026-07-25)
