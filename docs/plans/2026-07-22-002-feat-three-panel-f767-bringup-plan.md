---
title: Three-Panel F767 Bring-Up - Plan
type: feat
date: 2026-07-22
topic: three-panel-f767-bringup
artifact_contract: ce-unified-plan/v1
artifact_readiness: requirements-only
product_contract_source: ce-brainstorm
execution: hardware
---

# Three-Panel F767 Bring-Up - Plan

## Goal Capsule

- **Objective:** Bring the two 5" RVT50H side panels to first light on the NUCLEO-F767ZI alongside the already-proven center, and verify all three rendering the shipped ENGINE/TIMING/dash layouts simultaneously to the side-panels plan's U9 bar.
- **Product authority:** Product Contract below. This is a bring-up/verification execution of an existing firmware feature — the renderers, channel model, and boot orchestration are already written and ported (`engine_render.h`, `timing_render.h`, the three-panel init loop in `MustangDash.ino`). Content and firmware authority is the parent plan `docs/plans/2026-07-10-002-feat-side-panels-plan.md`; hardware authority is `docs/hardware/three-panel-pin-reference.md` and `docs/hardware/nucleo-f767-center-panel-wiring.md`.
- **Open blockers:** None on parts — both panels, FFC breakouts, and cables are in hand. Bench power provisioning (see Dependencies) is the first thing to confirm at the bench, not a blocker to starting.

---

## Product Contract

### Summary

The 2026-07-21 F767 first light proved one panel (center) on the Nucleo. This round wires the two 5" side panels onto their own dedicated SPI peripherals (left → SPI2, right → SPI4) and brings them to first light **incrementally** — center+left proven and soaked before the right is wired — closing out the side-panels plan's U9 verification on the F767 topology. No firmware feature work: the code is done; tonight is copper, power, clocks, and eyes-on verification.

### Key Decisions

- **This is a bring-up round, not a build round.** The side-panel firmware exists and is already F767-ported: `engine_render.h`/`timing_render.h` are included at `MustangDash.ino:279-280`, the init loop iterates all three panels (`MustangDash.ino:385-395`), the vendored library carries the `EVE_MULTI_PANEL` patch with per-panel runtime timing writes, and the F767 block declares three genuinely dedicated SPI peripherals (`MustangDash.ino:118-129`). Firmware is touched only to fix a bring-up defect found on glass.
- **Dedicated bus per panel — the shared-bus era is over.** The Teensy topology capped the shared SPI bus at 8 MHz on signal integrity and killed two-panel bring-up on 2026-07-10. The F767 gives each panel its own peripheral (SPI1 center / SPI2 left / SPI4 right), so bus contention and the shared-bus ceiling are not in this round's road. The parent plan's R10 "at most one CS asserted" constraint is trivially satisfied and no longer the risk it was.
- **Incremental sequence, one new variable at a time.** Wire and prove center+left first: first light, clock-walk SPI2, read-integrity soak. Only then wire the right. Each new panel introduces two fresh unknowns at once — an unexercised SPI peripheral and the upstream-"untested" RVT50H profile on F767 silicon — so adding them serially means a failure has one suspect, not four. Costs a second wiring pass; buys clean attribution and a bankable mid-point milestone.
- **Clocks are proven per bus, never assumed to transfer.** The center's bench-accepted 13.5 MHz does **not** automatically hold on the side buses: SPI1/SPI4 sit on APB2 (108 MHz) but **SPI2 (left) is on APB1 (54 MHz)** — a different clock tree reaching 13.5 MHz through a different divisor. Given the 27 MHz hard-wedge history, each new bus is clock-walked empirically and accepted on **fps AND read integrity**, never fps alone (per parent plan KTD8).

### Requirements

**Wiring and topology**
- R1. Both 5" panels are wired via their FFC breakouts to the F767 pins already declared in firmware: left → SPI2 (`g_spi_left`, CS `PE9`, PD `PE13`), right → SPI4 (`g_spi_right`, CS `PE11`, PD `PF15`), per `MustangDash.ino:124-129`. The center's existing wiring is unchanged.
- R2. Before applying 5V to any panel, the backlight end of each FFC is positively identified by continuity (RiBus pins 19-20 BLGND beep to pin 2 GND) — the standing bench rule from the F767 first-light hazards.

**Bring-up sequence**
- R3. Center+left reach simultaneous first light before the right panel is wired: both `EVE_init()` return `E_OK`, both read `REG_ID == 0x7C`, both render their live layouts (center dash + left ENGINE), and the mode switch moves both in lockstep.
- R4. Only after center+left is verified clean (R3 + R5 + R6 for the left) is the right panel wired and brought to the same first-light bar.

**Verification (the U9 bar)**
- R5. Each new bus is clock-walked to an accepted operating point, starting from a conservative floor and raised only through a per-panel read-integrity soak with **zero spurious coprocessor faults** — fps alone is never sufficient acceptance. The accepted point per bus is recorded.
- R6. Each panel sustains a read-integrity soak (stable `REG_ID`, `faults=0`, steady fps) at its accepted clock, in both TRACK and STREET.
- R7. All three panels render their layouts simultaneously and the parent plan's acceptance examples AE1–AE5 pass on the physical three-panel bench: lockstep mode switch (AE1), center-only alarm takeover with sides live (AE2), dark-boot then fade-in (AE3), a disconnected panel leaving the others normal (AE4), and a serial `set`/`clear` round-trip on a side channel (AE5).

**Degradation**
- R8. A dead or disconnected panel stays dark and has no effect on the others — verified live by booting with one side's FFC unplugged (parent R9 / AE4). The center never depends on the sides.

### Acceptance Examples

- AE1. **Covers R3.** With center+left wired, at power-up both reach first light: serial banner shows both panels `ok`, both `REG_ID 0x7C`, center dash and left ENGINE screen both live, and `mode street` swaps both together with no mismatched frame.
- AE2. **Covers R5, R6.** Walking SPI2 upward, the left panel holds a multi-minute STREET soak with `faults=0` and stable `REG_ID` at its accepted clock; a clock step that produces any spurious fault is rejected and the prior step recorded as the operating point.
- AE3. **Covers R7 (full).** All three panels live simultaneously: center dash, left ENGINE, right TIMING; `mode track`/`mode street` moves all three in lockstep; a forced oil-pressure alarm takes over the center only while both sides keep their live layouts.
- AE4. **Covers R8.** With the right panel's FFC unplugged at power-up, center and left boot and run normally and serial acks as usual; the right is simply dark.

### Success Criteria

- **Full win (tonight's target):** all three panels verified to the U9 bar — AE1–AE4 pass, each bus clock-walked and soaked clean with zero spurious faults, operating points recorded.
- **Fallback line (still a real milestone, never a failure):** center+left verified clean per R3/R5/R6, with the right panel a clean resume for the next session. The incremental sequence exists precisely so running out of bench time banks this instead of leaving a half-bisected board.
- The shipped center-panel experience is unchanged throughout — adding the sides never regresses the panel that already works.

### Scope Boundaries

**Deferred / not tonight**
- Any firmware feature work. The renderers, 13 side channels, boot orchestration, and library multi-panel patch are already done and ported; code changes only to fix a defect surfaced on glass.
- CAN integration, real lap timing, and the sim-through-CAN decode host test — all deferred by the parent plan and unrelated to this bench session.
- Carrier PCB / buffered-SPI topology work — this round runs on the FFC-breakout bench loom; the carrier's higher clock ceiling is a separate track (`docs/solutions/architecture-patterns/dash-carrier-pcb-buffered-spi-topology-30mhz-clock-contract.md`).
- Any RVT50H profile deviation is corrective only — reached from the panel datasheet if the vendored "untested" values misbehave on glass, not a redesign.

**Outside this product's identity**
- Touch input (no-touch panels); the bezel's physical turn-signal LEDs; re-designing the side layouts (the design handoff is authority).

### Dependencies / Assumptions

- **Parts in hand:** 2× SM-RVT50HQBNWN00 (800×480, BT817, no-touch), both FFC breakouts, and cables. Confirmed on the bench.
- **Power is the first bench decision and the likely sleeper.** The center's F767 first light ran entirely off the ST-LINK USB cable. Three logic rails (~0.52 A combined per datasheet) plus two more backlights exceed USB budget — assume a bench buck for backlight 5V (already the norm) and a dedicated 3.3V feed for panel logic if the onboard regulator strains (flicker/reset/glitch during soak is the trigger, per `three-panel-pin-reference.md:54`). Common ground ties Nucleo, buck, and all breakouts.
- **SPI2 clock tree (APB1, 54 MHz) differs from SPI1/SPI4 (APB2, 108 MHz).** 13.5 MHz is reachable on both but via different divisors; the F767 prescaler rounds requests down to 6.75/13.5/27/54 (`MustangDash.ino:248-254`). Per-bus operating point is an empirical result of R5, not an assumption.
- **RVT50H profile is upstream-"untested"** and unproven on F767 — normal side-panel first-light risk; the Riverdi RVT50HQBNWN00 datasheet is the tiebreaker (`dash_panels.h:107-109`).
- **FFC hazards this bench has actually hit:** a damaged FFC end shorting pins 1-2 (VDD-GND) stops the board enumerating; the FFC is down-side contact at the panel; a panel survives being driven with no 5V on BLVDD (renders, just dark). Continuity-ID the backlight end (R2) before applying 5V.
- **Bench ops are Kevin-initiated:** flash/upload/power/wiring steps are staged, never fired unprompted.

### Outstanding Questions

- Does SPI2's APB1 clock tree accept 13.5 MHz cleanly, or does the left panel settle at a lower operating point than the center (6.75 MHz)? Resolved empirically by R5.
- Does three-panel rendering hold 60 fps on the F767 with three independent buses, or does the parent plan's R11 fallback (reduced side refresh, center at 60) get invoked? Measured, and if invoked, recorded explicitly — never silent.
