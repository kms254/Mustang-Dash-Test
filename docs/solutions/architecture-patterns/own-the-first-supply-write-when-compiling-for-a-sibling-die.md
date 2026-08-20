---
title: "Own the first post-POR supply write when compiling for a sibling die"
date: 2026-08-19
category: architecture-patterns
module: stm32-migration
problem_type: architecture_pattern
component: tooling
severity: critical
applies_when:
  - "compiling for one STM32 die under a sibling die's headers or board definition (H755 under H743 here; any pin-compatible stand-in)"
  - "a board hangs pre-main -- no USB, no serial, PC sampled over SWD shows a tight spin -- and reflashing does not recover it"
  - "bringing up a new H7 board or a new VCORE wiring (LDO-only, direct SMPS, SMPS-feeds-LDO, bypass) for its first true power-on"
  - "about to call HAL_PWREx_ConfigSupply or trust the framework's ExitRun0Mode to select the core supply"
  - "a failure that survives reflash and warm reset but changes after a full power cycle -- the write-once supply-latch signature"
symptoms:
  - "ACTVOSRDY never sets; Reset_Handler spins in ExitRun0Mode before main -- board looks bricked, USB never enumerates"
  - "Reflashing does not recover the hang; only a true power cycle running fixed firmware does (H7 supply config is write-once until POR)"
  - "Under H743 headers the LDO branch compiles to PWR->CR3 |= PWR_CR3_LDOEN -- an OR that cannot clear the H755's SMPSEN (bit 2, which H743 headers name SCUEN with a write-1 convention)"
  - "POR default CR3=0x46 (SMPS feeds LDO) stays selected against floating SMPS pins on an LDO-wired board"
  - "HAL_PWREx_ConfigSupply is untrustworthy for the write: older HAL revisions' PWR_LDO_SUPPLY actively SETs bit 2 (re-selecting the wrong supply), and the pinned HAL's lock check reads bit 2 with inverted meaning on H755 silicon"
root_cause: wrong_api
resolution_type: code_fix
tags:
  - stm32h7
  - supply-config
  - ldo
  - smps
  - write-once
  - exitrun0mode
  - sibling-die
  - pre-main
---

# Own the first post-POR supply write when compiling for a sibling die

## Context

This firmware compiles as an STM32H743 (`board = nucleo_h743zi` — `platformio.ini:41` for `[env:board3]`, `:110` for `[env:board3_mule]`; PlatformIO has no H755 board def) but runs on STM32H755 silicon in the same LQFP-144: the NUCLEO-H755ZI-Q bench mule and the Board3 carrier. The H7 core-supply configuration in PWR_CR3 is **write-once until the next power-on reset**, and the first code to write it is not the sketch: it is `ExitRun0Mode()`, branch-linked from the reset handler before any C runtime exists (`bl ExitRun0Mode`, framework `startup_stm32h743xx.s:63-64`), compiled from whichever `USE_PWR_*_SUPPLY` macro the board variant defines.

The framework source (framework-arduinoststm32 4.21200.0, `~/.platformio/packages/framework-arduinoststm32/system/Drivers/CMSIS/Device/ST/STM32H7xx/Source/Templates/system_stm32h7xx.c:478-488`):

```c
#if defined(USE_PWR_LDO_SUPPLY)
  #if defined(SMPS)
    /* Exit Run* mode by disabling SMPS and enabling LDO */
    PWR->CR3 = (PWR->CR3 & ~PWR_CR3_SMPSEN) | PWR_CR3_LDOEN;
  #else
    /* Enable LDO mode */
    PWR->CR3 |= PWR_CR3_LDOEN;
  #endif /* SMPS */
  /* Wait till voltage level flag is set */
  while ((PWR->CSR1 & PWR_CSR1_ACTVOSRDY) == 0U)
  {}
```

An H743 build has no `SMPS` macro, so `USE_PWR_LDO_SUPPLY` compiles the `#else`: `PWR->CR3 |= PWR_CR3_LDOEN` — an OR that can never clear a bit. The H755 POR default is CR3 = **0x46**: SMPSEN (bit 2) and LDOEN (bit 1) both set — "SMPS feeds LDO" — plus a reserved bit that reads 1 (0x40). The OR writes 0x46 back and **consumes the write-once latch with the POR default**. If the board's wiring cannot validate that supply chain, ACTVOSRDY (PWR_CSR1 bit 13, `stm32h743xx.h:14196-14197`) never sets and the chip spins forever at the wait — pre-main, before any sketch code can run.

The escape hatch is in the same function: the direct-SMPS branch is `#elif defined(USE_PWR_DIRECT_SMPS_SUPPLY) && defined(SMPS)` (`system_stm32h7xx.c:499`). Under H743 headers `SMPS` does not exist, every branch after it is likewise `&& defined(SMPS)`-guarded (the one unguarded sibling, `USE_PWR_EXTERNAL_SOURCE_SUPPLY`, sits before it and is never defined here), and the trailing `#else` is comment-only — so defining `USE_PWR_DIRECT_SMPS_SUPPLY` compiles `ExitRun0Mode()` to an **empty function**, leaving the write-once latch unconsumed for the sketch. (Line numbers cite the Templates copy; the build actually compiles the byte-identical function in the framework package's own `system/STM32H7xx/` copy of system_stm32h7xx.c via SrcWrapper's include, where it sits at :455/:476.)

The deeper hazard is that the register *names* lie before the registers do. PWR_CR3 bit 2:

| Header | Name | Meaning |
|---|---|---|
| `stm32h743xx.h:14242-14244` | `PWR_CR3_SCUEN` | "Supply configuration update enable" — a write-once handshake/lock bit |
| `stm32h755xx.h:15046-15048` | `PWR_CR3_SMPSEN` | "SMPS Enable" — a live regulator control |

Same position, unrelated semantics, opposite write conventions. Any supply-config code written in H743 vocabulary and run on H755 silicon is manipulating a regulator it cannot name.

## Guidance

The pattern, proven live twice (mule 2026-08-12, PR kms254/Mustang-Dash-Test#44, merged; Board3 2026-08-19, PR #47):

**1. Repo variant header no-ops ExitRun0Mode.** `boards/variants/H755ZI_Q_MULE/variant_NUCLEO_H743ZI.h:417-421`:

```c
#if defined(DASH_MULE_H755Q) || defined(DASH_BOARD_BOARD3)
  #define USE_PWR_DIRECT_SMPS_SUPPLY
#else
  #define USE_PWR_LDO_SUPPLY
#endif
```

`USE_PWR_DIRECT_SMPS_SUPPLY` does **not** configure direct SMPS here — under H743 headers its only effect is the ExitRun0Mode no-op (the `&& defined(SMPS)` guard above). It is deliberately defined for *both* boards, including LDO-wired Board3, because its job is to keep the latch unconsumed, not to pick a supply. Both envs route through this variant: `board_build.variants_dir = boards/variants` / `board_build.variant = H755ZI_Q_MULE` (`platformio.ini:53-54`, `:115-116`).

**2. The sketch's `SystemClock_Config` makes the first post-POR supply write, with raw bits through H743 macro names, then waits on ACTVOSRDY before touching VOS or clocks.** STM32duino declares `SystemClock_Config` weak, so the sketch override wins at link time — but only step 1 makes it the *first* writer.

Board3 (real H755 carrier: VCORE is LDO-wired — VDDLDO fed, VLXSMPS/VFBSMPS floated per DS12923 strapping), `MustangDash/MustangDash.ino:269-272`, selected by `DASH_BOARD_BOARD3 && HSE_VALUE == 25000000UL` (`.ino:238`, defines at `platformio.ini:56-57`):

```c
MODIFY_REG(PWR->CR3,
           (PWR_CR3_SCUEN | PWR_CR3_LDOEN | PWR_CR3_BYPASS),
           PWR_CR3_LDOEN);
while (0U == (PWR->CSR1 & PWR_CSR1_ACTVOSRDY)) { }
```

Clears bit 2 (the die's SMPSEN, under its H743 alias `PWR_CR3_SCUEN`), sets bit 1: LDO-only, CR3 0x46 -> 0x42.

Mule (NUCLEO-H755ZI-Q: VCORE is SMPS-powered), `MustangDash/MustangDash.ino:343-346`, selected by `DASH_BOARD_BOARD3 && DASH_MULE_H755Q` (`.ino:320`, defines at `platformio.ini:118-119`): the same `MODIFY_REG`, value `PWR_CR3_SCUEN` — sets bit 2 (= SMPSEN on the die), clears LDOEN and BYPASS: direct SMPS.

Measured post-fix on Board3 (2026-08-19): CR3 0x46 -> 0x42, ACTVOSRDY 0 -> 1, 400 MHz boot, USB CDC live.

**3. Do not route the write through `HAL_PWREx_ConfigSupply()`.** Every name it is built from carries H743 semantics, and what it actually does floats with the HAL revision:

- In the framework pinned today, `PWR_LDO_SUPPLY` is plain `PWR_CR3_LDOEN` (`stm32h7xx_hal_pwr_ex.h:235`) and the no-SMPS `PWR_SUPPLY_CONFIG_MASK` is `(PWR_CR3_SCUEN | PWR_CR3_LDOEN | PWR_CR3_BYPASS)` (`:251`), so the `MODIFY_REG` at `stm32h7xx_hal_pwr_ex.c:347` happens to land the right LDO-only bits. But the function's lock detection is inverted on H755 silicon: a no-SMPS build reads bit 2 as `PWR_FLAG_SCUEN` (`stm32h7xx_hal_pwr.h:178`, flag macro at `:425`) and treats 0 as "config locked" (`stm32h7xx_hal_pwr_ex.c:325-336`). On the H755 that bit is SMPSEN — with a direct-SMPS config latched (bit 2 = 1) the HAL believes the latch is still open and returns HAL_OK for a write the hardware discarded; with LDO-only latched (bit 2 = 0) it takes the locked branch. It cannot even tell you whether it did anything.
- Earlier ST H7 HAL revisions used the write-1-to-SCUEN convention for `PWR_LDO_SUPPLY`; the sketch's own comment (`MustangDash/MustangDash.ino:259-268`) records the resulting SETs-bit-2 behavior as the live trap. A SCUEN-setting constant, written to H755 silicon, is literally `SMPSEN | LDOEN`: the correct-looking call actively re-selects the SMPS-feeds-LDO chain the fix exists to escape. (The pinned framework does not carry that constant — per this session's reading, its trap is the inverted lock check above, not the destructive write.)

A call whose meaning depends on which HAL the framework pins is not a supply-config strategy. Write the register yourself with the bit positions in front of you.

**4. Recovery rule.** Once a wrong supply config latches, reflashing alone **never** recovers the board — the fixed firmware's correct write hits a consumed latch. Recovery is fixed firmware resident **plus a true POR**. And "true POR" is stricter than pulling the power lead: an attached ST-LINK/SWD connection can back-power the rail and mask the POR, so unplug the debugger too.

**5. Diagnosis recipe** (repo-proven on both instances; same-night sibling triage in `docs/solutions/build-errors/an-unguarded-header-define-silently-stomps-a-build-flag.md`, the OTHER pre-main hang):

1. OpenOCD (PIO's own) halt; sample PC twice. Identical or 2 bytes apart = a spin (the ACTVOSRDY wait compiles to a 2-instruction loop).
2. `arm-none-eabi-addr2line -e firmware.elf <pc>` names the function. `ExitRun0Mode` + LR = `Reset_Handler` + empty stack = pre-main: no sketch-side fix can reach it.
3. Read the registers to name the state: PWR->CR3 (0x5802480C) and PWR->CSR1 (0x58024804). CR3 = 0x46 with CSR1 bit 13 clear (measured CSR1 = 0x4000 — ACTVOS = 01 in bits 15:14, ACTVOSRDY = 0) is exactly "POR-default SMPS-feeds-LDO latched, never validated".

## Why This Matters

- **The failure mode masquerades as a bricked board, and warm resets hide it for days.** The mule ran a full bench day on the factory demo's correct direct-SMPS latch — warm resets inherit whatever the last POR latched, and ExitRun0Mode's doomed OR write was silently discarded against the consumed latch. The first true power cycle presented a fresh latch, ExitRun0Mode consumed it with 0x46, and the board "died". A defect that survives every warm-reset session and kills the first cold boot is the worst kind for a car dash.
- **Reflashing — the universal fix — does nothing here**, which reads as hardware death until you know the latch rule.
- **The chip still runs while "bricked".** On Board3 the LDO fed VCORE through the hardwired VDDLDO regardless of the bad latch, so the ROM bootloader's USB DFU still enumerated — the bootloader never waits on ACTVOSRDY. "Bricks on our firmware but DFUs fine" is this signature, not dead silicon.
- **The through-line:** when you compile for a sibling die, the register names lie before the registers do. H743's `SCUEN` (lock handshake) and H755's `SMPSEN` (regulator enable) share bit 2 with opposite conventions, so every layer written in H743 vocabulary — startup, HAL, macros — manipulates the H755's regulator by accident. The only code that can be trusted with the write-once latch is code that writes raw bits it chose against the die's actual bit map and the board's actual VCORE wiring.

## When to Apply

- Any new PlatformIO env riding `nucleo_h743zi` onto H755 silicon: add its define to the variant guard (`variant_NUCLEO_H743ZI.h:417`) and give the sketch a supply block matching that board's VCORE wiring. The env is not done until both halves exist.
- Any edit to the repo variant header, either `SystemClock_Config` block, or anything else touching PWR_CR3.
- Any sibling-die compile generally (H743-family headers on H745/H747/H755/H757 silicon): audit every register the pre-main path writes for name drift, starting with PWR_CR3 bit 2.
- Triaging "board looks bricked after a power cycle but survived N warm-reset sessions", or "our firmware hangs but the DFU bootloader enumerates".

## Examples

**Mule (NUCLEO-H755ZI-Q, SMPS-powered VCORE, 2026-08-12, PR #44 merged).** Stock variant hard-defines `USE_PWR_LDO_SUPPLY`; ExitRun0Mode's OR latched the 0x46 POR default, which the -Q wiring cannot validate. Survived a whole bench day on warm resets inheriting the factory demo's direct-SMPS latch; first true power cycle hung pre-main at the ACTVOSRDY wait. Reflashing did not recover; fixed firmware + power cycle did. SWD triage (PC sampled twice, addr2line, empty stack) cracked it. Fix: the variant guard (PR #44), completing the sketch's pre-existing direct-SMPS write (`MODIFY_REG(..., PWR_CR3_SCUEN)`, `.ino:343-346`).

**Board3 (real H755 carrier, LDO-wired VCORE, SMPS pins floated, 2026-08-19, PR #47).** First power-on hung identically: PC pinned in ExitRun0Mode, LR = Reset_Handler, CR3 = 0x46, CSR1 = 0x4000 (ACTVOSRDY bit 13 = 0). The stock LDO branch is an OR that cannot clear SMPSEN, and the floated SMPS pins mean the default config can never validate. USB DFU still worked throughout (LDO fed VCORE via hardwired VDDLDO). Fix: extend the same variant guard to `DASH_BOARD_BOARD3` and make the sketch's LDO-only raw write the first post-POR supply write (`.ino:269-272`). Measured: CR3 0x46 -> 0x42, ACTVOSRDY 0 -> 1, 400 MHz, USB CDC live. Notably, an earlier `platformio.ini` comment had reasoned Board3 should "stay on the stock upstream variant on purpose" — that reasoning did not survive first contact (`platformio.ini:105-107`).

## Related

- `docs/solutions/build-errors/an-unguarded-header-define-silently-stomps-a-build-flag.md` — same night, same PR (#47), same SWD PC-sampling triage recipe: the OTHER pre-main boot hang (preprocessor `-D` stomp). Key triage contrast: the HSE hang does not care about POR vs warm reset; this one does (POR-only recovery).
- `docs/solutions/security-issues/fdcan-nonconforming-dlc-overflows-8-byte-rx-buffer.md` — same root-cause family (wrong_api on ST's vendor HAL for STM32H7): the HAL's actual behavior diverges from what its name promises, and trusting the abstraction is the defect.
- `docs/solutions/integration-issues/eve-panel-bringup-no-usb-enumeration-diagnosis.md` — symptom-family ancestor ("board looks dead / no USB"); this doc adds the register-level discriminator: a hung pre-main boot reads all-zero USB registers and mimics a bad cable, so check DSTS first.
- `docs/solutions/integration-issues/f767-spi-prescaler-quantization-27mhz-hard-wedge.md` — hang-family contrast: the 27 MHz wedge recovers with a reflash and no power cycle; the supply latch is the opposite. Which ritual clears a hang identifies its class.
- Pattern halves: PR kms254/Mustang-Dash-Test#44 (merged — mule/direct-SMPS half) and PR kms254/Mustang-Dash-Test#47 (Board3/LDO-only half, unmerged as of this writing).
