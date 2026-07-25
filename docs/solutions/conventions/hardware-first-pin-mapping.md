---
title: Pin maps optimize for hardware; firmware constants follow the board
date: 2026-07-25
category: conventions
module: carrier-hardware
problem_type: convention
component: development_workflow
severity: high
applies_when:
  - "Assigning MCU pins for a custom PCB (schematic capture or layout)"
  - "A firmware pin table and a cleaner routing option are in tension"
  - "A plan or review is about to enshrine 'zero firmware edits' as a goal"
tags: [pin-mapping, pcb-layout, firmware, stm32, carrier-board, priorities]
---

# Pin maps optimize for hardware; firmware constants follow the board

## Context

During Board3 planning (2026-07-25), the review process converged on "the board runs the existing firmware pin table with zero pin-constant edits" as a success criterion — and the schematic was about to inherit routing constraints to protect it. Kevin inverted the priority: firmware pin constants are one-line data edits; routing compromises are soldered into every board of a spin.

## Guidance

Optimization order for pin assignment: **silicon constraints → layout quality → firmware convenience.**

- **Immovable (the silicon's mux is fixed):** peripheral signals (SPI/QUADSPI/CAN/I2C/USB) must land on pins carrying the required alternate function; dedicated pins (BOOT0, NRST, oscillator, VCAP, SWD, USB DM/DP, VBUS sense) never move; package-specific traps hold (on the STM32H755 LQFP-144: no `_C` analog dual-pads on digital nets, no pull-down on the pin the ROM bootloader uses for USART detection).
- **Free for layout:** every GPIO-class net (chip selects, resets, LED/lamp drives, buttons) — assign to whatever routes cleanest, as late as placement/routing. Also free: which instance of a peripheral serves which connector (swapping SPI1↔SPI4 between panels is a constants edit, not a design change).
- **Process:** a draft pin map (often the firmware's existing table) starts schematic capture; layout has explicit authority to reassign within the immovable constraints; every swap back-annotates to the schematic and the pin-map document; the firmware's board pin table is written *from* the as-routed document at layout freeze. The board is the source of truth; the code follows.

A zero-edit outcome is welcome (it means the draft was electrically legal) but is never a constraint anyone routes around.

## Why This Matters

The asymmetry is total: changing a pin constant costs one line and zero risk, any day. A via forced onto a marginal read line, a longer SPI run, or a split ground reference to protect that constant is permanent for the spin — and this project's bench history shows read-line integrity is exactly where marginal routing bites (see the SPI clock-integrity and ground IR-drop learnings).

## When to Apply

- Any custom-board pin assignment, especially first spins
- Reviews: treat "matches firmware pins" as a nice default in requirements docs, not an acceptance criterion

## Examples

Board3: the review-approved plan initially mirrored the Nucleo bench pin map with enumerated exceptions to keep firmware untouched. After the inversion, the plan's KTD1 grants layout pin-swap authority, marks the pin-map doc DRAFT until layout freeze, and rewrites the firmware Board3 table from the final routed map (docs/plans/2026-07-24-001-feat-board3-h755-carrier-plan.md, docs/hardware/board3-h755-pin-map.md).

## Related

- docs/solutions/integration-issues/multi-panel-shared-ground-ir-drop-floats-spi-reference.md — why routing quality on ground/read paths dominates
- docs/solutions/integration-issues/f767-spi-prescaler-quantization-27mhz-hard-wedge.md — cost of marginal SPI signal paths
