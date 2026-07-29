---
title: "Two consumers of the same physics disagreeing in one frame means the display, not the model"
date: 2026-07-25
category: logic-errors
module: dash-simulation
problem_type: logic_error
component: tooling
severity: medium
symptoms:
  - "THROTTLE read 100% while the car held a constant speed through a corner"
  - "BRAKE read 82% with a slow wobble in every zone, identical for a 170->65 mph stop and an 80->70 mph brush"
  - "In one status frame, fuel burn implied near-idle load while THROTTLE claimed 100%"
  - "Every host test passed and the firmware compiled clean the entire time"
root_cause: logic_error
resolution_type: code_fix
related_components:
  - dash-rendering
tags:
  - dash-sim
  - display-model-divergence
  - grip-circle
  - telemetry
  - channels
---

# Two consumers of the same physics disagreeing in one frame means the display, not the model

## Problem

The dash's THROTTLE and BRAKE channels were derived from a single boolean rather than from the physics that moved the car: throttle was `braking ? 0 : 100`, and brake was a fixed 82% plus a wall-clock sine. The bars looked plausible in motion and were wrong wherever it mattered.

## Symptoms

- Throttle showed 100% at a constant speed mid-corner — a driver holding a limit is not flat.
- Every braking zone reported the same effort, so the biggest stop on the track was indistinguishable from the lightest brush.
- The brake "texture" was `82 + 8·sin(3t)` against **wall-clock** time, so it drifted out of phase with track position by construction.
- No test caught any of it: the channels stayed in range, moved smoothly enough, and were never `NaN`.

## What Didn't Work

- **Reading the simulator for a bug.** There was none — `dash_sim_step()` computed the corner physics correctly the whole time. The defect was one layer out, where two `DashState` channels got their values.
- **Assuming the visibly wrong number was the broken one.** A related report on the same screen ("lap count says 18") turned out to be a *correct* fuel-range figure with a misleading label. See [query the running device before theorising](../developer-experience/query-the-running-device-before-theorising.md).

## Solution

Derive both pedals from the same quantities that move the car, in `MustangDash/dash_sim.h`:

- **Brake** = the fraction of the step's braking authority being spent, so 100% means all of it and the trace tapers as the car settles onto a limit.
- **Throttle** = the thrust the driver may ask for as a fraction of what the engine can give — grip-circle capped mid-corner, floored at the throttle that balances drag, mapped through `sqrtf` because pedal travel versus torque is nonlinear.

Deliberately **not** `p_frac`, the power fraction already computed for fuel: it reads below 1 wherever traction is the constraint, which is exactly where a real foot is flat to the floor. Same physics, wrong quantity for a pedal.

## Why This Works

The simulator already computed the right shape — `grip_scale` goes to ~0 at an apex, and `a_applied` (hence fuel burn) follows it down. Only the two pedal channels ignored it. Once they read from that state, the bars cannot drift from the car without the physics changing too.

**The reusable tell:** two consumers of the same underlying state disagreeing *within one frame* localises the bug to a consumer. Here a single status line reported near-idle fuel burn and 100% throttle simultaneously. Both cannot be right, and the one with an independent, long-calibrated derivation (load-proportional fuel burn) is the more trustworthy witness. That comparison is far cheaper than reading either code path.

## Prevention

Assert cross-consumer coherence, not just per-channel ranges. The invariants added to `tests/test_dash_sim.c` include "throttle must not read 100% while the car holds a constant speed below terminal velocity" and "brake must read 0 on any step the car gained speed" — both cheap, and both would have failed on the original code. A range check cannot catch a channel that is internally plausible and externally inconsistent.

When smoothing is needed, rate-limit the model's **input** so thrust follows the displayed value, rather than filtering the output — a display-only filter reintroduces exactly this defect, with the bar describing a car the physics is not driving.

## Related Issues

- [Removing a discontinuity took two mechanisms](../design-patterns/continuity-and-slew-are-two-separate-fixes.md) — the corner-exit follow-on to this fix
- [When a model change breaks pinned invariants, re-derive the constant](../conventions/re-derive-the-constant-not-the-threshold.md)
- PR #8 (merged 2026-07-25)
