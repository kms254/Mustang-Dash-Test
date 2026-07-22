---
module: dash-simulation
date: 2026-07-22
problem_type: logic_error
component: tooling
severity: high
symptoms:
  - "Tuning SIM_DRIVER_SKILL across its whole documented 0.82-0.95 band moved lap time only ~6 s against a ~34 s gap"
  - "Simulated High Plains Raceway lap came out 1:46 where real cars run 2:04-2:10"
  - "Every host test passed and the firmware compiled clean the entire time"
root_cause: logic_error
resolution_type: code_fix
related_components:
  - dash-rendering
tags:
  - calibration
  - simulation
  - physics
  - coupling
  - tuning-constants
---

# A calibration knob with no leverage is usually fighting a bug, not underpowered

## Problem

`SIM_DRIVER_SKILL` was designed as the single knob tuned against lap time. Swept
across its entire documented 0.82-0.95 band it moved the lap by about 6 seconds,
against a gap of roughly 34. The obvious readings — "the knob is too weak", "widen
the band", "add a second knob" — were all wrong. The knob was fine. It was being
cancelled out by a coupling defect upstream of it.

## Symptoms

- Lap time 1:46.45 where comparable real cars run 2:04-2:10 at the same circuit.
- Driver skill at 0.88 bought ~6 s; at the 0.82 floor, ~9 s. Neither reached the
  target, and the plan's own text said a constant that will not fit its guardrail
  is evidence of an upstream problem.
- **Nothing failed.** 14/14 host tests green, firmware compiling clean, no warning
  anywhere. The defect was invisible to every automated signal.

## What Didn't Work

- **Assuming the knob needed more range.** The plan asserted skill was "the only
  knob tuned against lap time." That assumption is what kept attention on the knob
  instead of on what the knob was multiplying.
- **Adding corner *duration*.** Corners initially had none — a corner's speed limit
  bound for a single instant at its entry, after which the car accelerated through
  the entire remaining 500-1100 ft. Giving corners a real arc was necessary and
  bought 18 s, but it did not restore the knob's leverage, because the new arc
  inherited the same coupling.
- **Reaching for a second constant.** Tempting, and it would have "worked" by
  masking the defect under a compensating fudge — the exact failure the plan warned
  about when it constrained skill to a defensible band.

## Solution

The corner arc's radius was being derived from the driver's **skill-scaled** speed:

```c
/* WRONG: v is the scaled, jittered speed the driver actually carries */
r   = v_scaled^2 / (SIM_LATERAL_G * g);
arc = r * turn_rad;
```

Arc length goes as `v²`, so arc *time* (`arc / v`) goes as `v`. Scaling the driver
down therefore **shrank the corner itself** — a slower driver drove a physically
shorter corner, which gave back most of the time the lower speed cost. The knob was
fighting its own second-order effect.

The fix derives the radius from the **authored** limit — unscaled and unjittered —
so the corner is a fixed piece of tarmac regardless of who is driving it
(`MustangDash/dash_sim.h:605-629`):

```c
/* r from the AUTHORED limit: the corner is fixed geometry, not a function
   of how fast this particular driver takes it. */
r   = v_authored^2 / (SIM_LATERAL_G * SIM_G_FPS2);
arc = r * SIM_SEGS[seg].turn_rad;
```

With that decoupled, skill regained its leverage and calibration landed at
**0.86 -> 2:01.60**, inside the documented band with no second knob.

A second, independent problem surfaced in the same investigation: a single
circuit-wide arc angle treated a flat-out kink and a 160-degree hairpin as the same
corner. Replacing it with per-corner turn angles (`turn_rad`, `dash_sim.h:215`)
sourced from the track's course guide was worth another ~16 s.

## Why This Works

A tuning constant only has leverage over the quantity you are tuning if nothing
downstream of it varies *with* it in the opposite direction. Here the dependency was
circular in effect: skill scaled speed, speed set the radius, radius set the corner
length, and corner length set how long the lower speed applied for. The two effects
very nearly cancelled.

Deriving geometry from authored values rather than from live simulated state breaks
that loop. It is also more physically honest — a corner is a fixed length of road,
and a slower driver should spend *more* time in it, not less.

Independent check on the corrected model: the authored 40 mph limit for the tightest
turn implies an 82 ft radius against the track's published 80 ft.

## Prevention

- **When a knob has surprisingly little leverage, suspect a coupling before
  suspecting the knob.** Sweep the constant across its full range and measure. A
  small, roughly linear response to a large input change means something downstream
  is moving with it. That measurement is cheap and would have found this in minutes.
- **Derive geometry and other fixed quantities from authored constants, not from
  live simulated state.** If a value represents something physically fixed (track
  layout, tank size, gear ratio), computing it from a variable makes it silently
  elastic.
- **Constrain calibration constants to a defensible range, and treat a fit outside
  that range as evidence rather than as a value to accept.** The 0.82-0.95 guardrail
  is what turned "the number will not fit" into a signal instead of a shrug. Without
  it the natural move is to set skill to 0.6 and ship a model that is wrong in a way
  nobody can see.
- **Do not trust a green suite on a model whose only oracle is itself.** Every test
  passed throughout, because the tests asserted internal consistency and nothing
  compared the model to the outside world. The defect was only visible against
  published real-world lap times. When a simulation's success criteria are all
  self-referential, at least one external anchor is worth finding.
