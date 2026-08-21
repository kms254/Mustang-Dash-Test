---
title: A clock constant is a request, not the operating point
date: 2026-08-21
category: conventions
module: spi-bus
problem_type: convention
component: development_workflow
severity: high
root_cause: missing_tooling
resolution_type: tooling_addition
applies_when:
  - walking a clock, baud or timer rate to a new operating point and about to record the result
  - a peripheral derives its rate by dividing a kernel clock the firmware never explicitly configures
  - two instances of one peripheral family sit on different kernel-clock sources (STM32H7 SPI1/2/3 on PLL1Q, SPI4/5 on APB2)
  - a soak is about to accept a hardware setting by the number that was requested rather than one read back
  - adding a diagnostic to settle a question -- baseline it at the unchanged setting first
symptoms:
  - A constant, the working notes and a bench acceptance record all cite a rate no instrument ever measured
  - The requested value is not a reachable point on the divider ladder, so it reads as a plausible neighbour of a number that is
  - An earlier note on different silicon already recorded an actual clock below the requested one, and nobody generalised it
related_components:
  - dash-panels
  - board3-carrier
  - stm32h7-hal
  - dash-serial
tags:
  - spi
  - clock-walk
  - stm32h7
  - prescaler
  - readback
  - measurement
  - bt817
  - board3
---

# A clock constant is a request, not the operating point

## Context

Board3 drives three BT817 panels from three independent SPI peripherals on one
STM32H755: center on SPI1, left on SPI2, right on SPI4
(`MustangDash/MustangDash.ino:164-169`). Every panel inits at a conservative
8 MHz *request* — which is itself **6.25 MHz attained** on all three, the bottom
rung of the very ladder this document is about, and a rate nothing has ever read
back either. Once all three inits are done the sketch raises each bus exactly once to a single constant,
`DASH_SPI_RUN_HZ` (`MustangDash/MustangDash.ino:943-944`).

That constant read `13500000UL` from 2026-07-21 (the commit accepting the F767
bench walk, `feat(f767): accept 13.5 MHz operating point from bench clock walk`)
until 2026-08-21. For that whole month the number 13.5 MHz was written down
three times as a *proven* operating point: in the constant's own comment, in
`CLAUDE.md`'s Hardware-truths SPI bullet ("now **13.5 MHz** … proven 2026-07-23
on TWO panels at fps 59, faults 0"), and in `docs/hardware/board3-bringup-card.md`
as the "13.5 MHz SPI clock re-walk" bring-up debt item.

On Board3 the panels were attaining **12.500 MHz**. Nothing in the tree had ever
read the rate back out of the peripheral, so nothing could say so.

The mechanism has two halves, and both are ordinary — which is the point.

**Half one: the driver rounds the request DOWN.** `SPISettings(hz, …)` reaches
`spi_init()` in the STM32 Arduino core, which walks a fixed prescaler ladder and
takes the first rung whose resulting rate is at or below the requested speed
(`~/.platformio/packages/framework-arduinoststm32/libraries/SPI/src/utility/spi_com.c:265-287`;
the `SPI_SPEED_CLOCK_DIV*_MHZ` macros are divisors, not megahertz —
`spi_com.h:46-53`). Prescalers are powers of two, 2 through 256, stored as the
exponent in the `CFG1.MBR` field, so attained = kernel >> (MBR + 1).

**Half two: the divider's source is not one clock.** `spi_com.c` picks the
kernel clock per instance: SPI1/2/3 resolve through `RCC_PERIPHCLK_SPI123`,
SPI4/5 through `RCC_PERIPHCLK_SPI45` (`spi_com.c:46-108`). This sketch's
`SystemClock_Config` configures exactly one peripheral mux — USB
(`MustangDash/MustangDash.ino:323-325`) — and never touches the SPI muxes, so
the H7 reset defaults stand, and the defaults are **asymmetric**: selector value
0 means `RCC_SPI123CLKSOURCE_PLL` (pll1_q) for SPI1/2/3 and
`RCC_SPI45CLKSOURCE_D2PCLK2` (APB2) for SPI4/5
(`stm32h7xx_hal_rcc_ex.h:1058,1117`). On Board3 that resolves to **PLL1Q at
200 MHz** (`PLLQ = 4` off an 800 MHz VCO, `MustangDash.ino:302`) for center and
left, and **APB2 at 100 MHz** (AHB 200 / `RCC_APB2_DIV2`,
`MustangDash.ino:315,317`) for right.

Run the arithmetic the driver runs:

| request | center SPI1 (200 MHz) | left SPI2 (200 MHz) | right SPI4 (100 MHz) | attained |
|---|---|---|---|---|
| 13.5 MHz | /16 | /16 | /8 | **12.500 MHz** |
| 25.0 MHz | /8 | /8 | /4 | **25.000 MHz** |

13.5 MHz was never a point on Board3's ladder. The reachable set is
6.25 / 12.5 / 25 with **nothing between**, and 50 above — which is invisible
while you reason in terms of the requested number, because 13.5 and 12.5 look
like neighbours rather than like a wish and a fact.

The sharpest part is that the 13.5 was not sloppy when it was written. On the
F767 mule it was walked, soaked and genuinely attained: APB2 there is 108 MHz and
108/8 = 13.5 exactly, on the quantised F767 ladder 6.75 / 13.5 / 27 / 54. And the
F767 was **already a two-kernel board** — SPI1 off APB2 at 108 MHz, SPI2 off APB1
at 54 MHz, where 54/4 = 13.5 as well. A 2:1 power-of-two ratio hid the asymmetry
there for precisely the reason it hides it on Board3, so the precedent for this
whole finding sat in the tree a month early, in a one-line comment, correct and
unread. The number was a measured operating point on the
silicon it was measured on, and it silently became a **wrong label** the moment
the same firmware moved to a die with a different clock tree. An operating point
is a property of *constant × clock tree*, not of the constant.

## Guidance

**1. Treat any configured rate that hardware quantises as a request, and read
the achieved value back from the peripheral before you write it down.** This
applies to SPI prescalers, PWM dividers, UART baud (fractional or otherwise),
ADC sample clocks, I2C timing, and anything else where a driver accepts a
frequency in hertz and then picks a divider. The number you passed in is a wish.
The register field is the fact. Documentation, acceptance soaks, PR titles and
`CLAUDE.md` bullets must cite the fact.

**2. When the divider's source clock is not explicitly configured, "the bus
speed" may not be a single number at all.** Reset-default peripheral clock muxes
can differ per instance. Report the attained rate **per instance**, never once
for "the bus", and print the kernel clock alongside it so the reader can see
which tree each instance is hanging off.

**3. Baseline the instrument before you move the thing it measures.** This is
the step that actually produced the finding. The readback function and the
`diag` command were flashed **alone**, with `DASH_SPI_RUN_HZ` unchanged, before
any clock step. That run is what printed 12.5 against a tree that said 13.5. Had
the instrument and the clock change shipped in one flash, the first reading
would have had nothing to be compared against, the 12.5 would have been read as
"25 didn't take" or simply as the new normal, and a month-old documentation
error would have been overwritten rather than discovered. A measurement's first
job is to reproduce the state you believe you are already in.

**4. Make the diagnostic reachable at runtime, not only at boot.** Board3's boot
banner races CDC enumeration — the sketch waits 500 ms for `Serial` and then
prints regardless — so a boot-only line is a diagnostic you can only see by
winning a race. `dash_report_spi_clocks()` therefore prints in both places: at
the raise (`MustangDash.ino:952`) and on the `diag` serial command
(`MustangDash.ino:1807`).

**5. Gate the operating point on read integrity, not on frame rate.** fps and
the fault counter have already fooled this project once: at 24 MHz on the Teensy
loom, writes mostly survived while reads corrupted, and the symptom was a
degraded fps with `faults=0`. `diag` therefore takes 16 `REG_ID` reads per panel
and reports how many returned 0x7C (`MustangDash.ino:1820-1829`).

## Why This Matters

- **An acceptance soak inherits the label you gave it.** The 2026-07-23
  two-panel soak was real evidence; it was evidence for 12.5 MHz on that
  hardware, filed under 13.5. Everything downstream — the CLAUDE.md bullet, the
  bring-up card's debt item, any future "we know 13.5 works, try 27 next"
  reasoning — inherited the wrong number. The data was never wrong; only the
  name on it was, and a name is what the next session actually reads.
- **The coarse ladder was unreadable from the request side.** Believing the bus
  ran at 13.5 made 27 MHz look like "one doubling away" — which is exactly the
  step that hard-wedged the F767. Knowing it runs 12.5 makes the real question
  visible: the only rungs are 25 and 50, and 50 is 167% of the BT817 slave's
  30 MHz rating.
- **The harmlessness of the asymmetric defaults is conditional, and nothing in
  the code enforces the condition.** Center/left at 200 MHz and right at 100 MHz
  agree on the attained rate for every request in **[781.25 kHz, 100 MHz)**
  *because 200:100 is itself a power of two*: the low ladder is the high one
  shifted a rung, so they share every rung except the high ladder's top
  (100 MHz) and the low ladder's bottom (0.390625 MHz) — and those two
  exceptions are exactly the two regions where they diverge. The bound matters
  here only as discipline: this is a document about stated absolutes getting
  inherited, so its own absolute has to carry its limits.
  Break that ratio and one constant yields two different bus speeds: point
  SPI123 at a 150 MHz PLL2P while SPI45 stays on APB2 100 MHz, and a 25 MHz
  request attains 18.75 MHz on center/left and 25.0 MHz on right. That is a
  three-panels-two-clocks bug with no compile error, no runtime error, and no
  symptom except one panel being slower than its siblings. The readback is what
  makes it a visible line of output rather than a mystery.
- **This repo had already written the rule, and prose did not hold it.**
  [`f767-spi-prescaler-quantization-27mhz-hard-wedge.md`](../integration-issues/f767-spi-prescaler-quantization-27mhz-hard-wedge.md)
  was written on 2026-07-21 — the same day the 13.5 constant landed — and its
  prevention section already says, in as many words, *record the attained
  value, never the requested one*. It was correct, it was findable, it was in
  the right category, and it was violated for the next month by the people who
  wrote it. Nothing executed it. That is the generalisation worth carrying out
  of this round: a prevention rule expressed only as prose is itself a request.
  Rules that survive are the ones something runs — a printed line, a test, a
  gate — and the work of compounding a learning is not finished when the rule is
  written, only when it is mechanized. This document is separate from that one
  for the same reason: a rule and the proof of its insufficiency should not be
  the same file.
- **It is a recurrence, not a first sighting.** The F767 first-light record
  already carried "actual SPI clock is 6.75 MHz (8 MHz request rounds down)"
  (auto memory [claude], `f767-first-light-2026-07-21`). The class of error had
  been observed, correctly, once — and then not generalised into a rule or an
  instrument, so it was available to happen again on the next board. Observing a
  trap and building the instrument that catches it are different acts, and only
  the second one compounds.

## When to Apply

- Before writing any clock, baud or sample rate into documentation, a commit
  message, a bring-up card or a design note as an accepted operating point.
- At the start of any clock walk — flash the readback first, unchanged, and
  confirm it reproduces the rate you believe you are already running.
- When the same firmware moves to different silicon, a different variant, or a
  different `SystemClock_Config`: every quantised rate in the tree must be
  re-measured, because the constant did not change and its meaning did.
- When one peripheral family is spread across multiple instances (three SPIs
  here) and the kernel-clock muxes are left at reset defaults.
- When a bench result and a later result disagree by an amount that looks like a
  small percentage — 13.5 versus 12.5 is 7.4%, comfortably inside the range
  where a discrepancy gets rationalised as measurement noise instead of
  investigated as a different rung.

## Examples

**The readback, on Board3** (`MustangDash/MustangDash.ino:775-809`). It asks the
`SPIClass` which peripheral it actually got, resolves the kernel clock through
the same HAL oracle the driver used to pick the prescaler, then reads the
prescaler exponent straight out of the peripheral:

```c
SPI_TypeDef *inst = DASH_SPI_BUSES[b]->getHandle()->Instance;
if (inst == SPI1 || inst == SPI2 || inst == SPI3)
    ker = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI123);
else if (inst == SPI4 || inst == SPI5)
    ker = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI45);
else
    ker = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI6);

const uint32_t mbr = (inst->CFG1 & SPI_CFG1_MBR_Msk) >> SPI_CFG1_MBR_Pos;
const uint32_t div = 1UL << (mbr + 1UL);
```

Output, per panel, at boot and on `diag`:

```
SPI raised to 25.00 MHz requested (prescaler rounds down; read-integrity soak gates the attained operating point)
  SPI center kernel 200000000 Hz / 8 = 25.000 MHz attained
  SPI left   kernel 200000000 Hz / 8 = 25.000 MHz attained
  SPI right  kernel 100000000 Hz / 4 = 25.000 MHz attained
```

Note the deliberate wording of the request line: it says *requested*, and it
says the prescaler rounds down. A line that prints a request must say so, or it
becomes the next wrong label.

**Known gap in the instrument.** The readback is compiled only where
`EVE_PANEL_HAS_BUS && SPI_CFG1_MBR_Pos` are both defined
(`MustangDash.ino:777`); otherwise it prints `SPI attained rate: not reported on
this target` (`MustangDash.ino:807`). `SPI_CFG1_MBR_Pos` is an SPI-v2 (H7) field
and is absent from the F767 headers, so the F767 mule build takes the fallback
branch — the board where this class of error was first seen is currently the one
board that cannot report it. On F7 the equivalent field is `CR1.BR` — but F7 **also**
lacks `RCC_PERIPHCLK_SPI123`, so the function's inner fallback reports every
instance's kernel as PCLK2 (108 MHz), including SPI2, which actually runs from
PCLK1 at 54 MHz. A `CR1.BR`-only extension would print the left panel at 27 MHz
while it runs 13.5 — the same error class, rebuilt inside the instrument meant to
prevent it. The extension is two changes, not one: the field, *and* per-instance
PCLK1/PCLK2 selection.

**What the 25 MHz step then bought, measured on real copper 2026-08-21.**
Baseline first at the unchanged setting: three panels, fps 60, `faults=0,0,0`,
REG_ID 16/16 on all three, attained 12.5 MHz. Then the constant moved to 25 MHz
and two 20-minute soaks ran, STREET and TRACK, with `diag` and `status` sampled
once a minute, 20 samples each. 16 reads × 3 panels × 20 samples × 2 legs =
**1,920 REG_ID reads, zero misses**. `faults=0,0,0` and `retired=0,0,0`
throughout, `eve=ok,ok,ok`, no drift in either leg. fps was 60 in TRACK and 57 in
STREET; the cluster's STREET display lists total about 33% more than TRACK
(`dl` 647/689/359 versus 434/434/408 — larger on center and left, slightly
*smaller* on right, so the honest figure is the total and not the leading two)
and the reading was steady rather than decaying, so it reads as render
cost rather than link trouble — explicitly **not proven**, because no 12.5 MHz
STREET sample exists to difference it against, and that gap is recorded rather
than papered over.

The walk stops at 25: the BT817 QSPI slave is rated 30 MHz, putting 25 at 83% of
spec, and the next prescaler-reachable rung is 50 at 167%. Reaching 30 would
require moving an SPI kernel source off the reset defaults — which is also the change that
can break the power-of-two ratio currently masking the asymmetry — a *matched*
move scaling both sources by the same power of two would preserve it — so it has
to be done for all three instances together and re-measured per panel.

**Counter-example worth keeping.** Board3 runs clean at 25 MHz where the older
Teensy loom failed read integrity at 24, and where 27 MHz hard-wedged the F767 on
jumpers. That is a separate lesson — bench operating points do not transfer
across topology — already carried in `CLAUDE.md`. It is mentioned here only
because it is the reason the *acceptance* evidence had to be re-gathered on
Board3 rather than inherited, which is what forced the readback question in the
first place.

## See also

**The direct predecessor.**
- [F767 SPI prescaler quantization / 27 MHz hard wedge](../integration-issues/f767-spi-prescaler-quantization-27mhz-hard-wedge.md)
  — states the rule this document is the failure of. Its F767 numbers
  (6.75 / 13.5 / 27 / 54 off APB2 108 MHz) are **correct and must not be
  "corrected" to 12.5** — that ladder is real on that die. What did not survive
  is the sufficiency of a prose-only prevention rule, and its "derive the ladder
  from the bus clock" advice, which assumes there is one bus clock.

**Numbers this measurement makes stale.**
- [24 MHz overclock corrupts EVE coprocessor reads](../integration-issues/spi-run-clock-24mhz-overclock-corrupts-eve-coprocessor-reads.md)
  — carries a 2026-08-05 *self-correction* block asserting "the constant is
  13.5 MHz". That block was itself added to fix a stale number and is now stale
  twice over. It is the cleanest available exhibit for why a retraction has to
  be grepped for, not just written where the error was noticed.
- [Dash carrier buffered-SPI topology / 30 MHz clock contract](../architecture-patterns/dash-carrier-pcb-buffered-spi-topology-30mhz-clock-contract.md)
  — its clock figure is explicitly a projection; Board3's two-soak acceptance
  discharges it at 25 MHz.

**Same family — a number that is not the measurement you think it is.**
- [A count at the report limit is not a measurement](../developer-experience/a-count-at-the-report-limit-is-not-a-measurement.md)
  — the sibling failure, and worth reading against this one: that doc's own
  detection test ("make the check stricter and watch the number move") would
  have **passed** this bug. Moving the constant 13.5 → 25 moved the reported
  rate 12.5 → 25.0, obediently, while it was wrong the whole time. A number that
  responds correctly to your inputs can still never have been measured.
- [Verifying every part of a claim does not verify the claim](../design-patterns/verifying-every-part-of-a-claim-does-not-verify-the-claim.md)
  — "soak-accepted at 13.5 MHz" was true in every part and false as a sentence.
- [An unguarded header define silently stomps a build flag](../build-errors/an-unguarded-header-define-silently-stomps-a-build-flag.md)
  — the build-time twin of configured-versus-effective, on this same clock tree.

**Method.**
- [Query the running device before theorising](../developer-experience/query-the-running-device-before-theorising.md)
  — `diag` extends the status surface that convention is about.
- [Own the first supply write when compiling for a sibling die](../architecture-patterns/own-the-first-supply-write-when-compiling-for-a-sibling-die.md)
  — register-level truth over framework abstraction, same board, same campaign.
- [Re-derive the constant, not the threshold](re-derive-the-constant-not-the-threshold.md)
  — the standing convention on not inheriting pinned numbers.

**Prior art.** [The three-panel F767 bring-up plan](../../plans/2026-07-22-002-feat-three-panel-f767-bringup-plan.md)
argued a month before the evidence existed that clock trees differ per bus and
numbers do not transfer between them. The thesis was already in the repo; what
was missing was the instrument.

**What the correction pass itself cost.** Bringing every stale rate in the tree
forward introduced or nearly introduced four fresh defects, including deleting
the repo's strongest corroborating fact for this very finding. Written up in
[a correction is an unreviewed change](a-correction-is-an-unreviewed-change.md).

**Bench record.** [Board3 bring-up card](../../hardware/board3-bringup-card.md)
carries the walk as a discharged debt item with the acceptance evidence.
