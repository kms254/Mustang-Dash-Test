---
module: dash-serial
date: 2026-07-22
problem_type: logic_error
component: tooling
severity: high
symptoms:
  - "The `alarm oilt` bench command acknowledged `ok` and raised no alarm"
  - "Whole host suite stayed green while the command was dead"
  - "Only reachable on hardware, and only if you noticed nothing happened"
root_cause: config_error
resolution_type: code_fix
related_components:
  - dash-rendering
  - dash-simulation
tags:
  - magic-constants
  - thresholds
  - coupling
  - diagnostics
  - test-oracle
---

# A forced-alarm constant divorced from the threshold it must exceed

## Problem

A bench command that forces an alarm state carried its own hardcoded value, in a
different file from the threshold that value has to exceed. Raising the threshold
left the forcing value below it, so the command kept acking success while doing
nothing — and every automated test stayed green, because nothing tied the two
constants together.

## Symptoms

- `alarm oilt` returned its normal `ok` acknowledgement and raised no alarm.
- No test failed. No compiler warning. Nothing in the firmware logged a problem.
- The failure is only observable by issuing the command on hardware **and noticing
  that the expected thing did not happen** — the ack looks identical either way.

## What Didn't Work

Nothing was tried and rejected here, and that is the point worth recording: the
defect was **not found by debugging**. It surfaced only because a reviewer was asked
to enumerate every consumer of the thresholds being changed. Had the change been
made without that sweep, it would have shipped — the bench command would have been
quietly inert until someone needed it to verify an alarm and drew the wrong
conclusion from a passing ack.

That is the real hazard: a diagnostic that silently no-ops does not merely fail to
help, it actively teaches you the system is fine.

## Solution

Two constants had to satisfy an invariant that was expressed nowhere:

```
dash_math.h    DASH_OILT_RED_F     — the temperature above which the alarm fires
dash_serial.h  DASH_ALARM_OILT_F   — the value the bench command writes to force it
```

The forcing value must exceed the threshold. It was a bare number in a different
file, with no comment, no assertion, and no test connecting it to what it targets.
Raising the threshold silently inverted the relationship.

The fix is not only the new number. It is the test that makes the coupling
enforceable — the shortcut is now driven through the real classifier rather than
checked by inspecting the value it wrote (`tests/test_dash_serial.c:347-361`):

```c
/* Before: the test asserted the command wrote the constant.
   That passes forever, including when the constant is wrong. */

/* After: assert the invariant, then assert the OUTCOME. */
expect(DASH_ALARM_OILT_F > DASH_OILT_RED_F, ...);
expect(dash_alarm_classify(&s) == DASH_ALARM_OILT, ...);
```

Every alarm shortcut now round-trips through `dash_alarm_classify()`, so any future
threshold change that breaks one of them fails the host suite instead of the bench.

## Why This Works

The original test had the wrong oracle. It verified the command did what the code
said it did — wrote a particular constant — which is a tautology that cannot detect
a wrong constant. Asserting the *observable outcome* (an alarm is actually
classified) makes the test sensitive to the thing that matters.

The explicit `DASH_ALARM_OILT_F > DASH_OILT_RED_F` assertion earns its place
alongside the outcome check because it fails with a message naming the invariant,
rather than leaving a future maintainer to work out why a classifier returned the
wrong enum.

## Prevention

- **When changing a threshold, enumerate every consumer before changing it — across
  file boundaries.** A grep for the constant name is the cheap version and would have
  found this. The constants that need this most are exactly the ones that live apart
  from what they target.
- **A constant that must maintain a relationship with another constant should assert
  that relationship**, ideally where it is defined. A one-line assertion converts a
  silent inversion into a build or test failure.
- **Test diagnostics by their outcome, not by their mechanism.** "The command wrote
  X" is unfalsifiable with respect to whether X is correct. "The command produced the
  state it claims to produce" is not. This applies to any forcing, seeding, or
  simulation helper whose whole purpose is to induce a condition.
- **Be suspicious of commands whose success ack is indistinguishable from their
  failure.** This one returned `ok` in both cases. Where a diagnostic can tell the
  difference cheaply, having it report what it actually observed is worth more than a
  bare acknowledgement — this repo already learned that lesson once with a full-chip
  erase that reported success in zero seconds against a detached flash chip.
