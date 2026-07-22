---
title: F767 SPI clocks are prescaler-quantized and 27 MHz hard-wedges the firmware
date: 2026-07-21
category: integration-issues
module: stm32-migration
problem_type: integration_issue
component: tooling
symptoms:
  - "At 27 MHz run clock the firmware hard-wedges within seconds -- serial completely dead (every command times out, no acks), dash frozen, no recovery until reflash"
  - "The SPI clock the firmware requests is not the clock it gets -- an 8 MHz request runs at 6.75 MHz on the F767 (prescaler rounds down)"
root_cause: config_error
resolution_type: config_change
severity: high
tags: [spi, clock-walk, stm32, f767, prescaler, bt817, read-integrity, hang]
---

# F767 SPI clocks are prescaler-quantized and 27 MHz hard-wedges the firmware

## Problem

The first clock walk on the NUCLEO-F767ZI (center 7" panel, long
low-quality jumpers, 2026-07-21) surfaced two facts: STM32 SPI clocks are
quantized to a small prescaler set so requested frequencies silently round
down, and above the wiring's margin the failure mode is not the Teensy
rig's polite read corruption -- it is a total firmware wedge.

## Symptoms

- 27 MHz: seconds after boot, serial goes fully dead -- six consecutive
  status commands time out with no ack, the dash freezes, and only an
  ST-LINK reflash (which works fine over the hung firmware, no power
  cycle needed) recovers the board.
- At every clock: the boot banner prints the *requested* frequency while
  the bus runs the rounded-down *attained* one.

## What Didn't Work

Expecting the documented Teensy-era failure signature (fps sag with
faults=0, font-inflate verify failures, white screen -- see
`docs/solutions/integration-issues/spi-run-clock-24mhz-overclock-corrupts-eve-coprocessor-reads.md`).
On this wiring the marginal-bus regime was skipped entirely: 13.5 MHz was
fully clean and 27 MHz was fully dead, with no polite in-between.

## Solution

Two-part understanding, both now recorded at the `DASH_SPI_RUN_HZ`
definition in `MustangDash/MustangDash.ino` and in `CLAUDE.md`'s bench
truths:

1. **Quantization.** The F767's SPI1 runs from APB2 (108 MHz with the
   216 MHz core) through power-of-two prescalers only: the attainable set
   is 6.75 / 13.5 / 27 / 54 MHz, and any request rounds DOWN to the next
   attainable value. A "walk it up gradually" plan collapses to exactly
   two candidate raises below the BT817's 30 MHz ceiling. Record the
   attained value, never the requested one.

2. **The wedge mechanism.** The library's `EVE_execute_cmd()` busy-waits
   on `EVE_busy()` with no timeout. When reads corrupt hard enough,
   `REG_CMDB_SPACE` readbacks return persistent garbage, the busy-wait
   never exits, `loop()` never runs again, and the serial pump dies with
   it. Nothing recovers because the fault detectors themselves depend on
   the reads that are corrupted.

Accepted operating point on this rig: **13.5 MHz** -- proven by a 3-minute
STREET-mode soak (fps=60 on every sample, faults=0, REG_ID stable across
15 reads, staging spot-checks clean) plus eyes-on-panel in both modes.

## Why This Works

The clock walk's acceptance rules already demanded read-integrity proof
rather than frame-rate; the new facts refine the *shape* of the walk on
STM32 targets: steps are few and large (octave jumps), the top step can
fail as a wedge rather than a degradation, and the operating point is a
property of the wiring (long bench jumpers here) -- the carrier PCB's
point-to-point copper re-owns the number when it exists.

## Prevention

- On any STM32 target, derive the attainable SPI clock set from the bus
  clock and prescalers before planning a walk; log or record the attained
  frequency, not the requested one.
- Accept a clock step only on: staging spot-checks clean, REG_ID stable
  across repeated reads, font-inflate verification clean, AND
  eyes-on-panel in every display mode. Never frame rate alone.
- Treat sudden total serial silence at a new clock as the wedge signature
  (distinct from the polite-corruption signature in the 24 MHz doc);
  recover by reflashing over the debug probe -- no power cycle required.
- A wedge risk exists anywhere `EVE_execute_cmd()` runs on a marginal
  bus; long blocking EVE operations (e.g. flash erase) inherit it.

## Related Issues

- `docs/solutions/integration-issues/spi-run-clock-24mhz-overclock-corrupts-eve-coprocessor-reads.md`
  -- the polite-corruption failure mode and the faults=0 blind spot.
- `docs/solutions/architecture-patterns/dash-carrier-pcb-buffered-spi-topology-30mhz-clock-contract.md`
  -- the 30 MHz ceiling and the carrier re-walk contract.
