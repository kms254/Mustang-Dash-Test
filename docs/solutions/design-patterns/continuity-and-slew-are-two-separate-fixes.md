---
title: "Removing a discontinuity took two mechanisms, and the first one measured as done"
date: 2026-07-25
category: design-patterns
module: dash-simulation
problem_type: design_pattern
component: tooling
severity: medium
applies_when:
  - "a simulated or animated value moves too abruptly and the model has a boundary where a constraint is released"
  - "a formula is continuous in principle but its slope is near-vertical over part of its range"
  - "an actuator in a model instantly takes whatever the current constraints permit"
  - "a smoothing fix looks correct by inspection and its rate of change has not been measured"
related_components:
  - dash-rendering
tags:
  - dash-sim
  - simulation
  - continuity
  - slew-limit
  - grip-circle
  - hpr
---

# Removing a discontinuity took two mechanisms, and the first one measured as done

## Context

The TRACK simulator modelled a corner as a constant-radius arc that simply **ended**. Lateral demand went from everything to nothing between two 20 ms steps, so the grip circle's longitudinal allowance (`grip_scale`) snapped from ~0 to 1 in a single step and the car took full thrust instantly. On glass the throttle bar slammed open at every corner exit — the bench report was "really aggressive throttle moves rather than easing into it; you'd lose traction and spin out."

The obvious fix was to make the boundary continuous: unwind the corner instead of ending it. That fix was correct, necessary, and **not sufficient** — and it looked sufficient, because afterward the arc's end was mathematically continuous and the visible cliff was gone.

## Guidance

Treat these as two independent properties, and verify each by measurement:

1. **Continuity at the boundary** — the constraint must release progressively rather than vanishing. In `MustangDash/dash_sim.h`, lateral demand now tapers to zero past `SIM_CORNER_APEX_FRAC`; the speed ceiling rises as `v_lim/sqrt(lat)` and the longitudinal allowance as `sqrt(1 - (lat·(v/v_lim)²)²)`, so at `lat → 0` the corner *is* a straight and crossing the arc's end is a non-event.

2. **A bounded rate of change** — whatever follows the freed constraint needs its own speed limit. Continuity alone bounds nothing: `sqrt(1 - used²)` is near-vertical as `used` leaves 1, so the freed grip arrived in a rush and the pedal still reached full travel in about 50 ms. `SIM_THROTTLE_ROLLON_S` (0.7 s) plus the `DashSimState.throttle_frac` state fixed that — rate-limited rising, free falling, since lifting is genuinely fast.

**Measure the per-step delta across the range; do not conclude from the formula.** The first fix was declared done by inspection and disproved by one measurement:

| Model state | Worst per-step throttle rise (10 ms steps) |
|---|---|
| before either fix | +32.3 points |
| continuity only | +18.5 points |
| continuity + slew limit | **+4.3 points** |

## Why This Matters

A smooth function is not a slow one. "No discontinuity" and "no abrupt movement" feel like one property and are not, so a fix that removes a cliff can leave a step function with rounded corners behind — indistinguishable by inspection, and indistinguishable in a static screenshot. A derivative measurement is the only thing that separates them.

The second mechanism also carries a cost that must be paid honestly rather than hidden: rolling on takes time, so the lap went from 1:52.4 to 1:59.7. That slowdown *is* the fix working. Smoothing only the displayed value would have shown the same gentle bar while the car kept its impossible pace.

## When to Apply

- Any model with a boundary where a constraint switches off — a zone the entity leaves, a phase that ends, a limit that lifts.
- Any value whose instantaneous "correct" target can move faster than the real thing it represents could move.
- **Not** for transitions that are genuinely instantaneous. The one-step lift before each braking zone (100% → ~22% → brake) was deliberately left alone: a driver really does lift that fast, and it is one frame at 60 fps.

## Examples

The pedal is state, rate-limited upward, with thrust derived from it (`MustangDash/dash_sim.h`):

```c
/* the pedal chases what the grip circle freed up, but may only rise so fast */
float want_frac = (grip_scale > maint_frac) ? grip_scale : maint_frac;
const float rise_max = dt_s / SIM_THROTTLE_ROLLON_S;
if (want_frac > sim->throttle_frac + rise_max) { want_frac = sim->throttle_frac + rise_max; }
sim->throttle_frac = want_frac;
a_applied = a_prop * sim->throttle_frac;
```

The invariant pinning it (`tests/test_dash_sim.c`) is asserted in pedal **fraction**, recovered by inverting the display's sqrt map, so it keeps testing the slew limiter even if the map changes.

## Related

- [Pedal display derived from a flag while the model had the value](../logic-errors/pedal-display-derived-from-a-flag-while-the-model-had-the-value.md) — the defect this work started from
- [When a model change breaks pinned invariants, re-derive the constant](../conventions/re-derive-the-constant-not-the-threshold.md) — how `SIM_CORNER_APEX_FRAC` was calibrated
- [A calibration knob with no leverage is usually fighting a bug](../logic-errors/calibration-knob-with-no-leverage.md) — the earlier corner-model calibration trap
- PR #8 (merged 2026-07-25)
