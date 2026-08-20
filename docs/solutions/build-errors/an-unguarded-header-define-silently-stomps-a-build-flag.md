---
title: "An unguarded header #define silently stomps a -D build flag and compiles out the clock override"
date: 2026-08-19
category: build-errors
module: firmware-build
problem_type: build_error
component: tooling
severity: critical
symptoms:
  - "Board hangs at first power-on inside HAL_RCC_OscConfig waiting for HSERDY -- no USB enumeration, board looks dead"
  - "Sketch's Board3 SystemClock_Config override silently compiled out: its #if gate on HSE_VALUE == 25000000UL evaluated false"
  - "RCC_CR readback over SWD 0x5C025: HSEON=1, HSERDY=0, HSEBYP=1 -- bypass asserted against a real 25 MHz crystal, so the oscillator amp is disabled"
  - "Compiler printed warning: \"HSE_VALUE\" redefined in every build log since the env was created -- nobody read it"
  - "Even a surviving boot would run SystemCoreClock skewed 3.125x (8 MHz assumed vs 25 MHz real)"
root_cause: config_error
resolution_type: code_fix
tags:
  - platformio
  - build-flags
  - preprocessor
  - hse
  - stm32
  - silent-failure
  - compiler-warnings
  - variant-header
---

# An unguarded header #define silently stomps a -D build flag and compiles out the clock override

## Problem

Board3's first power-on looked dead — flashed clean, then nothing: no USB enumeration, no serial, no sign of life. The cause was a preprocessor stomp: the Arduino-STM32 variant header redefined the build's `-D HSE_VALUE=25000000UL` back to `8000000` in every translation unit, which silently compiled out the sketch's Board3 clock override and left the stock 8 MHz HSE-BYPASS clock config to hang forever against a real 25 MHz crystal.

## Symptoms

- **No USB CDC enumeration after flash.** Upload succeeds, the board enumerates nothing. A brand-new board presenting exactly like dead hardware.
- **PC pinned pre-`setup()`.** SWD sampling with PlatformIO's own OpenOCD (halt, `reg pc` twice — same address both times) plus `arm-none-eabi-addr2line` against `firmware.elf` resolved the spin to `SystemClock_Config` → `HAL_RCC_OscConfig`. This is the same PC-sample + addr2line + register-readback recipe that had diagnosed a *different* pre-main hang (the H755 supply-config latch) minutes earlier the same night — it is the repo's proven boot-hang triage.
- **RCC_CR readback: `0x5C025`** — HSEON=1 (bit 16), HSERDY=0 (bit 17), **HSEBYP=1** (bit 18). Bypass mode named the failure precisely: HSEBYP turns the oscillator amplifier off and expects an external clock on OSC_IN. Board3 has a crystal there, not a clock, so the crystal can never start, HSERDY never sets, and `HAL_RCC_OscConfig` blocks on it forever.
- **The warning was in every build log the env ever produced:** `variant_NUCLEO_H743ZI.h:368: warning: "HSE_VALUE" redefined` (line 368 in the pre-fix header, where the bare `#define` sat). It printed on every successful-looking build since `[env:board3]` was created. Nobody read it.

## What Didn't Work

- **Replug, power-cycle, reflash.** The hang is deterministic and pre-`main()`-adjacent (first thing the Arduino core runs before `setup()`); no power-state ritual touches it. Worth ruling out fast here only because the *other* hang that night (the write-once supply latch) genuinely did care about POR vs. warm reset — this one does not.
- **Assuming the `-D` flag was authoritative.** `platformio.ini:57` plainly sets `-D HSE_VALUE=25000000UL` in `[env:board3]` (`platformio.ini:39`), and the sketch's gate plainly tests it — so the flag "obviously" reached the code. It did not. The variant header's bare `#define` won in every TU that includes it, which via `Arduino.h` → `pins_arduino.h` is effectively all of them.
- **Trusting a warning-bearing build because it linked.** This is the honest beat: the `"HSE_VALUE" redefined` warning was visible for weeks, in green builds, and was scrolled past every time. The repo has already paid for this failure mode once — the ERC-1009 lesson (`docs/solutions/developer-experience/a-large-erc-count-is-a-broken-instrument.md`): output nobody reads is an instrument that has stopped instrumenting. A build log with tolerated warnings is the same broken instrument at smaller scale — the one line that mattered had no way to stand out because *no* line was being read.

## Solution

Guard the variant's default so a command-line definition wins. The fix lives in the repo-owned variant copy — `boards/variants/H755ZI_Q_MULE/variant_NUCLEO_H743ZI.h` — which both `board3` and `board3_mule` already ride via `board_build.variants_dir`/`board_build.variant` (`platformio.ini:53-54`, `115-116`).

Before (stock STM32duino header, pre-fix line 368 — unguarded against the command line):

```c
// HSE default value is 25MHz in HAL
// HSE_BYPASS is 8MHz
#ifndef HSE_BYPASS_NOT_USED
  #define HSE_VALUE             8000000
#endif
```

After (`boards/variants/H755ZI_Q_MULE/variant_NUCLEO_H743ZI.h:376-378`):

```c
#if !defined(HSE_VALUE) && !defined(HSE_BYPASS_NOT_USED)
  #define HSE_VALUE             8000000
#endif
```

The guard composes with upstream's existing `HSE_BYPASS_NOT_USED` knob rather than replacing it — three cases, all correct:

1. **Env defines `HSE_VALUE`** (Board3's `-D HSE_VALUE=25000000UL`): the header defines nothing; the env's value survives into every TU. The sketch's gate at `MustangDash/MustangDash.ino:238` — `#if defined(DASH_BOARD_BOARD3) && defined(HSE_VALUE) && (HSE_VALUE == 25000000UL)` — goes true, and the 25 MHz clock override compiles in.
2. **Env defines `HSE_BYPASS_NOT_USED`** (upstream's opt-out for a Nucleo with a real crystal fitted): unchanged from stock — the header defines nothing and the HAL's own 25 MHz default applies.
3. **Env defines neither** (the H755 mule on a real NUCLEO-H755ZI-Q, 8 MHz ST-LINK MCO): the stock 8000000 default lands exactly as before. The mule env is behaviorally untouched.

Verified on the bench post-fix (PR kms254/Mustang-Dash-Test#47, unmerged as of this writing): Board3 boots at 400 MHz SYSCLK from the 25 MHz crystal (25 /M5 × N160 /P2, per the override at `MustangDash/MustangDash.ino:249-250`), USB CDC enumerates, full dash firmware live.

## Why This Works

The mechanism is pure preprocessor ordering. `-D` definitions are processed before any line of any file, so when the compiler reaches the variant header, `HSE_VALUE` is already `25000000UL`. A later bare `#define HSE_VALUE 8000000` with a different body is a *redefinition*: GCC warns (`"HSE_VALUE" redefined` — the warning that was printing all along) and the **new value wins** from that line onward. Because the variant header is pulled in near the top of every TU (through `Arduino.h` → `pins_arduino.h`), every downstream consumer saw 8000000, never 25000000:

- **Consequence (a), the hang:** the sketch's equality gate evaluated `8000000 == 25000000UL` → false, so the entire Board3 `SystemClock_Config` override compiled out — *silently*, because a false `#if` is not an error, it is a feature. STM32duino declares `SystemClock_Config` weak (noted at `MustangDash/MustangDash.ino:246-248`), so with no strong override in the link, the variant's weak default linked instead: 8 MHz HSE **bypass**, written for the Nucleo's ST-LINK MCO. On Board3's real crystal that is unstartable by construction — HSEBYP disables the amp — hence HSERDY never, `HAL_RCC_OscConfig` forever, RCC_CR `0x5C025`.
- **Consequence (b), the sleeper:** the HAL computes `SystemCoreClock` from `HSE_VALUE` when the PLL runs from HSE, so even a boot that somehow succeeded would have carried clock arithmetic off by 25/8 = **3.125x** — every HAL timeout, every baud-rate derivation, wrong by the same factor.

The `#if !defined(HSE_VALUE)` guard inverts the precedence from *last definition wins* to *first definition wins*, which is the correct polarity for a default: a default that can overwrite an explicit setting is not a default, it is a trap with a warning attached.

## Prevention

1. **Grep every build log for `redefined` — mechanically, not by eyeball.** One line does it:

   ```
   pio run 2>&1 | grep -i redefined
   ```

   Empty output is the pass condition. Any hit on a load-bearing `-D` flag is this bug wearing a different macro name. This is cheap enough to belong in CI or in `scripts/compile.sh` itself.

2. **A `-D` consumed by an `#if` equality gate deserves a compile-time proof.** The gate at `MustangDash.ino:238` is *designed* to compile the override out on non-Board3 envs — so its silence is ambiguous: "mule build, working as intended" and "Board3 build, clock config stomped" look identical. Make the intended-impossible combination an error:

   ```c
   #if defined(DASH_BOARD_BOARD3) && defined(HSE_VALUE) && (HSE_VALUE == 25000000UL)
   extern "C" void SystemClock_Config(void) { /* ... 25 MHz crystal tree ... */ }
   #define DASH_BOARD3_CLOCK_OVERRIDE_COMPILED 1
   #endif

   #if defined(DASH_BOARD_BOARD3) && !defined(DASH_BOARD3_CLOCK_OVERRIDE_COMPILED)
   #error "DASH_BOARD_BOARD3 is set but the 25 MHz clock override compiled out -- HSE_VALUE was stomped (grep the build log for 'redefined')"
   #endif
   ```

   With that in place, this bug is a red build with a message naming its own diagnosis, instead of a dead board and an SWD session.

3. **Repo-owned variant copies are the sanctioned place to fix vendor headers.** The precedent already exists in this exact file: the H755 supply guard (`variant_NUCLEO_H743ZI.h:417-421`, banked after the 2026-08-12 mule hang) lives in the same repo copy, wired in by `board_build.variants_dir = boards/variants` (`platformio.ini:53-54`). The PlatformIO-managed framework package is not editable in place (it is regenerated on clean installs), and an upstreamed fix helps a future toolchain, not this board. When a vendor header is wrong, copy it under `boards/variants/`, fix it there with a dated comment explaining the delta from stock, and point the env at it — that is now the pattern's second use, which is what makes it a pattern.

## Related Issues

- `docs/solutions/architecture-patterns/own-the-first-supply-write-when-compiling-for-a-sibling-die.md` — the OTHER pre-main hang from the same first-light night (and the same PR), diagnosed with the same SWD recipe; also the canonical write-up of the H755 supply guard this doc's Prevention #3 cites.
- `docs/solutions/developer-experience/a-large-erc-count-is-a-broken-instrument.md` — same unread-warning failure mode: the decisive signal was emitted in every run and read by nobody because it lived inside routine output.
- `docs/solutions/integration-issues/kicad-dru-one-bad-constraint-silently-voids-every-custom-rule.md` — silent-void family: one bad input silently disables an entire configured behavior while the run looks clean.
- `docs/solutions/developer-experience/a-count-at-the-report-limit-is-not-a-measurement.md` — instrument-misreading family: a tool output taken at face value is not the quantity you think it is until verified.
- `docs/solutions/conventions/a-gate-that-cannot-pass-gets-waved-through.md` — a check whose output is never read stops being a check; the redefinition warning was a permanently-yellow gate.
- `docs/solutions/integration-issues/f767-spi-prescaler-quantization-27mhz-hard-wedge.md` — same "the value you requested is not the value you got" shape on STM32 clocks.
- Fix PR: kms254/Mustang-Dash-Test#47.
