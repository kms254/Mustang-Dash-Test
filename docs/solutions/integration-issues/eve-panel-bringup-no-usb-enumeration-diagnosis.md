---
title: "Panel bring-up kills USB enumeration: shorted FFC and a flaky USB cable"
date: 2026-07-09
category: integration-issues
module: display-bringup
problem_type: integration_issue
component: development_workflow
symptoms:
  - "Teensy stops enumerating on USB (no OS connect sound) the moment the panel or its 5 V supply is connected"
  - "EVE_init() returns 0x02 (EVE_FAIL_REGID_TIMEOUT) and REG_ID reads 0x00 instead of 0x7C"
  - "board appears completely dead even in bootloader mode, then enumerates when the USB cable is wiggled"
  - "continuity beep between panel connector pins 1 (VDD) and 2 (GND)"
root_cause: incomplete_setup
resolution_type: environment_setup
severity: high
related_components:
  - tooling
tags:
  - teensy41
  - riverdi
  - rvt70h
  - usb-enumeration
  - ffc
  - reg-id
  - bring-up
  - short-circuit
---

# Panel bring-up kills USB enumeration: shorted FFC and a flaky USB cable

## Problem

During the first hardware bring-up of the Riverdi RVT70H panel on a Teensy 4.1
(2026-07-09), the Teensy repeatedly dropped off USB — no enumeration, no OS
connect sound — apparently whenever the panel or its 5 V backlight supply was
connected. Two independent physical faults were interleaved, and their
symptoms mimicked each other and mimicked a dead board.

## Symptoms

- Plugging the panel (or later, the 5 V supply) coincided with the Teensy
  vanishing from USB entirely — `arduino-cli board list` showed no Teensy port.
- With the link partially working, the firmware reported
  `EVE_init()... returned 0x02` (`EVE_FAIL_REGID_TIMEOUT`,
  libraries/FT800-FT813/src/EVE_commands.h:156) with `REG_ID = 0x00`.
- The board looked dead even with the PROGRAM button (bootloader mode), which
  enumerates independently of any sketch — the strongest "board is dead"
  signal there is. It was still wrong: a wiggle of the USB cable brought it back.
- A continuity tester beeped between panel pins 1 (VDD, 3.3 V) and 2 (GND).

## What Didn't Work

- **Trusting timing correlation.** "It died exactly when I connected the 5 V"
  pointed at a mis-wired backlight killing the MCU. The board was fine — the
  USB cable had simultaneously shifted into its bad spot. Correlation between
  the last action and the failure misled the diagnosis twice in one session.
- **Trusting the continuity beeper alone.** Pin 1-to-GND beeps can be the
  module's decoupling capacitors charging (a chirp that fades) or the probe
  tip bridging adjacent 0.5 mm-pitch pins — not necessarily a real short.
  Ohms mode with a steady-vs-climbing reading is the discriminating test.
- **Assuming "no LED" meant "no power."** The Teensy 4.1 has no power LED;
  its orange LED is pin 13, which this project uses as SPI SCLK, so it never
  lights. The OS's USB connect sound is the reliable power-up signal.

## Solution

Strict one-change-at-a-time isolation, re-testing USB enumeration at each step:

1. **Bare Teensy on USB** — enumerates? If not, the problem is the board or
   the cable, not the panel. (Here: a flaky micro-USB cable that intermittently
   mimicked a dead board — replaced, plus strain relief taped to the bench.)
2. **Add the panel's FFC, no 5 V** — if enumeration dies here, the logic side
   shorts the 3.3 V rail. Here: the FFC end had visible physical damage on
   pins 1–2, shorting VDD to GND; the panel and cable had each tested fine on
   a separate STM32 eval board, so the damaged end was the remaining suspect.
   Cable replaced — never reuse an FFC with a damaged end; the failure is
   intermittent by seating.
3. **Add the 5 V backlight supply, USB last** — if enumeration dies here, the
   5 V landed on the wrong pins (see the beep trick in Prevention).

With all three cleared, the same firmware that had been "failing" all day
reported `E_OK`, `REG_ID = 0x7C`, and rendered on the panel.

Reading the `REG_ID` serial diagnostic as a symptom ladder:

| REG_ID reads | Meaning |
|---|---|
| `0x00` | MISO dead low — panel unpowered, CS not reaching the panel, or the MISO path broken |
| `0xFF` | chip not driving MISO — CS fault or MOSI/MISO swap |
| `0x7C` | BT81x alive — the SPI link is good |

## Why This Works

Every symptom traced to physics, not firmware: a VDD–GND short makes the
host USB port current-limit, so the Teensy browns out and cannot enumerate —
indistinguishable from a dead board from the outside. A flaky USB cable
produces the identical top-level symptom. Isolation converts one confusing
compound failure into single-variable tests, and the USB connect sound is the
per-step pass/fail oracle that needs no tools.

## Prevention

- **Identify the backlight end by continuity before applying 5 V:** the panel's
  BLGND pins (19–20) are internally tied to logic GND (pin 2), so a ground-lead
  beep against the outermost pin pair positively identifies the 17–20 end.
  No beep = you are counting from the pin-1 end — stop before powering.
- **First power-up behind a current limit** (~100 mA on a bench supply): a
  miswire browns out harmlessly instead of cooking a 3.3 V-max part with 5 V.
- **Ohms, not beeps, for short verdicts:** steady ~0 Ω = short; a reading that
  starts low and climbs = decoupling caps charging, normal.
- **Strain-relieve the USB cable** on the bench; a Teensy that "dies" when
  other wiring is touched deserves a cable wiggle test before a post-mortem.
- The panel renders fine with no 5 V on BLVDD — it is just dark. Bringing up
  logic first (verify `REG_ID = 0x7C`) and backlight second splits the domain.
- The README troubleshooting table maps these serial symptoms to causes;
  see [README.md](../../../README.md).

## Related Issues

- [docs/solutions/best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md](../best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md) —
  the fourth failure mode in this space: everything above passes but the wrong
  display profile still renders garbage.
- [CLAUDE.md](../../../CLAUDE.md) — "Hardware-verified" section records the
  bench rules distilled from this session.
