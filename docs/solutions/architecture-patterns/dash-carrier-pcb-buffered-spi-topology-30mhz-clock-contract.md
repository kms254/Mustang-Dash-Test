---
title: "Buffered shared-SPI topology and 30 MHz clock-walk contract for the dash carrier PCB"
date: 2026-07-19
problem_type: architecture_pattern
category: architecture-patterns
module: spi-bus
component: tooling
severity: high
applies_when:
  - "raising DASH_SPI_RUN_HZ (the SPI Operating Point) on the dash carrier PCB via the U9 read-integrity soak"
  - "reads fail before writes at a higher SPI clock — try the Teensy LPSPI delayed sample point before blaming signal integrity"
  - "revising the carrier PCB SPI signal path: source termination, LVC buffer/combiner, per-panel series resistors, FFC runs"
  - "probing the bus during a soak — TP1=SCLK_C, TP2=MOSI_C, TP3=MISO_NODE, TP4=CS_C, TP5=GND"
  - "needing more display bandwidth than 30 MHz single-wire SPI — that is BT817 dual/quad SPI (RiBus 9-16, currently NC), a v2 hardware change"
related_components:
  - "dash-panels"
  - "carrier-pcb"
  - "teensy-lpspi"
tags:
  - spi
  - bt817
  - teensy41
  - signal-integrity
  - carrier-pcb
  - source-termination
  - lpspi
  - clock-walk
---

## Context

The three-panel dash has so far run on a bench wiring loom (breadboards, jumper
wires, MTCELL FFC breakouts). That loom is what capped the shared SPI bus at
8 MHz: the first 24 MHz candidate failed read AND write integrity on the bench
(2026-07-10 — white screen, flash init 0x01, all font inflates failed, fps 25
with faults=0), fully documented in
`docs/solutions/integration-issues/spi-run-clock-24mhz-overclock-corrupts-eve-coprocessor-reads.md`.

The loom era produced three more hard lessons (session history): the Riverdi
Arduino demo's `SPI_CLOCK_DIV2` resolves to **12 MHz on a Teensy 4 — above the
BT817's ≤ 11 MHz pre-PLL init ceiling from the first byte** — and nearly
convicted a healthy panel before the true 8 MHz rebuild ran indefinitely;
two-panel operation on the star-of-stubs harness collapsed with fault storms
at 8 MHz *and* 4 MHz (clock speed only modulated the failure — the topology
was the disease); and a marginal bus corrupts *gradually*, showing
micro-glitches long before outright death.

A new dash carrier PCB (EasyEDA project "Board2") now replaces that loom for
the Teensy 4.1 + three Riverdi BT817 panels. Its SPI distribution network was
designed specifically to remove the loom's signal-integrity ceiling, and the
topology below is netlist-verified per this session's analysis. Future
firmware sessions need the hardware→firmware contract: what the board
guarantees, what the new clock ceiling is, which firmware knobs exist to reach
it, and how to walk the run clock up safely. One honesty note up front: as of
this writing the board is unfabricated — **no clock above 8 MHz has been
validated on any wiring, and "24–30 MHz on the PCB" is a projection from the
design analysis, not a measurement** (session history).

The firmware side of the contract is unchanged in shape: `DASH_SPI_RUN_HZ` in
`MustangDash/MustangDash.ino` is still the single post-init run-clock
constant, and its comment block still records the bench evidence
(`MustangDash/MustangDash.ino:141-149`). Note the current working tree has it
at a value explicitly marked as throwaway:

```c
static const uint32_t DASH_SPI_RUN_HZ = 2000000UL; /* TEMP bench diagnostic: 2 MHz street-wedge SI test -- do not commit */
```

(`MustangDash/MustangDash.ino:149`). Per the comment block above it, **8 MHz
is the committed, bench-verified operating point** on the loom; the 2 MHz
value is a temporary loom-era diagnostic that must not ship. On the PCB, the
expectation changes entirely — see below.

## Guidance

### The board topology (what the PCB guarantees — netlist-verified)

All of the following is per this session's netlist analysis of Board2:

- **Outbound (SCLK/MOSI):** each Teensy output has a 33 Ω source-termination
  resistor at the MCU pin (R39 = SCLK, R40 = MOSI), then feeds a
  **SN74LVC244ADWR** octal buffer (SOIC-20, ~3.9 ns tpd at 3.3 V). The buffer
  makes **three per-panel copies** of SCLK and MOSI; each copy gets its own
  33 Ω series termination (R32–R37) and drives a point-to-point run to that
  panel's Molex 5034802000 ZIF FFC connector (FPC1 = left, FPC2 = center,
  FPC3 = right). No more multi-drop bus: every panel sees a dedicated,
  source-terminated, buffered line.
- **Inbound (MISO):** each panel's MISO returns through a 100 kΩ pulldown into
  a **SN74LVC125ADR** quad tri-state buffer (SOIC-14, ~2.5 ns). Each gate's
  enable is wired to that panel's own CS — CS is active-low, so asserting a
  panel's CS opens exactly that panel's gate. The three outputs merge onto
  MISO_NODE (its own 100 kΩ pulldown), then 33 Ω (R38) into the Teensy MISO
  pin. The combiner replaces the loom's shared tri-state MISO party line —
  kept deliberately even though BT81x tri-states MISO natively, as an
  independent CS-gated isolation layer against a browning-out panel that
  fails to tri-state cleanly (session history).
- **CS safety:** CS_C/L/R each have a 10 kΩ pullup to +3V3 (R45–R47), so
  floating Teensy GPIO during reset/bootloader cannot phantom-select a panel
  or enable multiple combiner gates at once.
- **Connector pinout** is the standard RiBus 20-pin 0.5 mm mapping the
  firmware already assumes: pin 3 = SCLK, pin 4 = MISO, pin 5 = MOSI,
  pin 6 = CS, pin 8 = PD (`docs/hardware/three-panel-pin-reference.md:58-72`).

The prior buffered prototype plan used 74HC DIP buffers, which added roughly
55 ns to the read round-trip and would have capped reads at ~12–16 MHz on
their own (session history / this session's analysis). The LVC parts cut the
buffer contribution to ~7 ns total, so reads and writes now share the same
ceiling.

### The new ceiling: 30 MHz, set by the BT817, not the wiring

The BT817 datasheet maximum for single-wire SPI is **30 MHz** — the same
"panels rated 30 MHz post-config" figure the repo's pin reference already
records (`docs/hardware/three-panel-pin-reference.md:204-207`); the oft-cited
40 MHz applies only to QSPI reads, which the FT800-FT813 library does not use
(session history). On the loom, wiring failed long before silicon; on the
PCB, per this session's analysis, the silicon rating is the binding limit.
Expected good outcome on the board: **24–30 MHz** (projection — to be proven
by the walk).

The init-clock rule is unchanged and non-negotiable: every panel's
`EVE_init()` must run at ≤ 11 MHz (this firmware inits at 8 MHz), and the bus
rises once afterward to `DASH_SPI_RUN_HZ`
(`MustangDash/MustangDash.ino:273-276`). Never express the init or run clock
via `SPI_CLOCK_DIV2`-style divisors — on a Teensy 4 that macro resolves to
12 MHz, silently violating the init ceiling (session history).

### Read-timing budget at 30 MHz (why reads are the tight direction)

At 30 MHz the bit period is ~33 ns. Per this session's analysis, the one-way
path delay is ≈10 ns: ~6.4 ns of buffer tpd (LVC244 out, LVC125 back), plus
board traces and ~200 mm of FFC at ≈1.2 ns each way — and on top of that sits
the BT817's own clock-to-MISO-valid delay. Sampling MISO on the opposite SCLK
edge leaves a ~16 ns budget: workable, but tight. That asymmetry is why a
clock that writes cleanly can still fail reads — and on this board, a
reads-fail-first signature means **loop latency, not signal integrity**.

### Firmware knobs, in order of application

**1. LPSPI delayed sample point.** The i.MX RT1062's LPSPI has a delayed-
sample capability (sample MISO half a cycle later / on the delayed point),
which directly absorbs round-trip loop delay on reads without touching signal
edges. Teensyduino's SPI library doesn't expose it as an API, so it's a
post-`SPI.begin()` register tweak. Illustrative sketch — **verify field names
and the correct LPSPI instance against the i.MX RT1062 reference manual
before use**:

```c
/* ILLUSTRATIVE ONLY -- verify against i.MX RT1062 RM (LPSPI chapter).
 * Teensy 4.1 pins 11/12/13 are LPSPI4. CFGR1[SAMPLE] moves MISO sampling
 * to the delayed point; module must be disabled while changing CFGR1. */
LPSPI4_CR &= ~LPSPI_CR_MEN;            /* disable module */
LPSPI4_CFGR1 |= LPSPI_CFGR1_SAMPLE;    /* delayed sample point */
LPSPI4_CR |= LPSPI_CR_MEN;             /* re-enable */
```

Apply this first when reads fail at a clock where writes are clean — that is
the loop-latency signature this knob exists for.

**2. IMXRT pad drive/slew tuning.** The SW_PAD_CTL registers for the SCLK and
MOSI pads expose DSE (drive strength), SPEED, and SRE (slew rate) fields.
With 33 Ω source terminations already on the board, softer or harder edges
are a trim, not a rescue — use them to clean up overshoot/ringing observed on
the scope, not to chase a failing clock. Illustrative sketch, same caveat:

```c
/* ILLUSTRATIVE ONLY -- verify pad register names/fields against the RM.
 * Example: retune the SCLK pad (Teensy 13 = AD_B0_03 on T4.1 -- confirm!).
 * DSE(6) medium drive, SPEED(2), fast slew. */
IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_B0_03 =
    IOMUXC_PAD_DSE(6) | IOMUXC_PAD_SPEED(2) | IOMUXC_PAD_SRE;
```

### The clock-walk procedure (unchanged discipline, new expectations)

1. Leave init at 8 MHz. Never touch the ≤ 11 MHz init rule.
2. Raise `DASH_SPI_RUN_HZ` one step at a time (e.g. 12 → 16 → 20 → 24 →
   30 MHz).
3. At each step, run the **U9 read-integrity soak** — per-panel REG_ID and
   REG_CMDB_SPACE read soak plus the 30-minute fault-counter-clean run, as
   defined in the existing SPI operating-point learning. **fps alone never
   accepts an operating point** — the 24 MHz failure ran at fps 25 with
   faults=0. The acceptance gate must exercise the same path and property the
   failure breaks: read-back/byte-compare integrity under sustained rendering
   load, per panel (session history — the verification-channel rule).
4. If reads fail before writes at some clock: enable LPSPI delayed sampling
   (knob 1) and re-run the soak at the same clock before backing off. On this
   board that failure shape is loop latency, not SI.
5. Record the accepted value and the dated soak evidence in the
   `DASH_SPI_RUN_HZ` comment block, per the established convention
   (`MustangDash/MustangDash.ino:141-148`).

### Beyond 30 MHz: quad, not overclock

Do not push past 30 MHz single-wire — that's the datasheet ceiling. The BT817
supports dual/quad SPI; the QSPI data lines are RiBus pins 9–16, which are
**deliberately NC on this board** (as on the loom,
`docs/hardware/three-panel-pin-reference.md:70`). Quad at 30 MHz is 4×
throughput and is the v2 board path, not a firmware experiment on Board2.

### Debug hardware on the board

Test points, per this session's analysis: TP1 = SCLK_C, TP2 = MOSI_C,
TP3 = MISO_NODE, TP4 = CS_C, TP5 = GND. **TP5 is a dedicated scope ground —
use it with a short ground spring, not a long ground lead**, or the ringing
on screen will be the probe's inductance, not the board's.

## Why This Matters

- **The 8 MHz cap is a loom fact, not a system fact.** The committed operating
  point and its failure evidence (`MustangDash/MustangDash.ino:141-149`, and
  the 24 MHz learning doc) describe the old wiring. A future session that
  reads "24 MHz failed" and concludes the PCB should stay at 8 MHz would be
  leaving 3–4× SPI bandwidth on the table; one that jumps straight to 30 MHz
  without the soak would repeat the original mistake in the other direction.
- **Failure signatures now decode differently.** On the loom, read corruption
  meant marginal wiring — back the clock off. On the PCB, reads failing while
  writes pass points at round-trip loop latency, and the correct first move
  is the delayed-sample knob, not a clock retreat. Knowing which regime the
  hardware is in changes the entire diagnosis tree.
- **The CS pullups and per-panel MISO gating close two loom-era hazard
  classes** — phantom panel selection from floating GPIO during
  reset/bootloader, and MISO bus contention — that previously had to be
  reasoned about in firmware. Firmware can now assume exactly one combiner
  gate opens per CS assertion.
- **Throughput is the dash's scaling axis.** Fonts, flash provisioning, and
  three-panel display lists all ride the one shared bus; 24–30 MHz vs 8 MHz
  is the difference between headroom and rationing as side-panel content
  grows.

## When to Apply

- Bringing firmware up on the Board2 carrier PCB for the first time
- Walking `DASH_SPI_RUN_HZ` up from 8 MHz after moving off the bench loom
- Diagnosing SPI read failures, font-inflate GETPTR failures, or flash-probe
  failures at a raised clock on the PCB
- Deciding whether a clock failure means "back off" (write failures / scope
  shows real SI trouble) or "delay the sample point" (reads fail first)
- Scoping bandwidth work: anything needing more than 30 MHz single-wire is a
  v2 quad-SPI board conversation, not a clock bump
- Probing the board with a scope (use the TP1–TP5 test points and the TP5
  ground spring)

## Examples

### Clock-walk decision table (symptom → action)

| Symptom at candidate clock | Reading | Action |
|---|---|---|
| Soak clean: REG_ID 0x7C every read, zero `(space & 3)` hits, 30-min faults 0,0,0 | Operating point good | Accept; record clock + dated evidence in the `DASH_SPI_RUN_HZ` comment; optionally continue walking |
| Reads fail (REG_ID misreads, font GETPTR, flash init ≠ E_OK) but rendering/writes look clean | Loop latency — round trip exceeds the sample window | Enable LPSPI delayed sample point; re-soak at the **same** clock |
| Reads still fail with delayed sampling enabled | Genuine margin problem at this clock | Scope SCLK/MISO at TP1/TP3 (ground on TP5); if edges are clean, back off one step and accept the previous clock |
| Writes also fail (garbage render, white screen) | Signal integrity, not latency — delayed sampling cannot help | Back off; probe TP1/TP2 for overshoot/ringing; consider pad DSE/SRE trim only with scope evidence |
| One panel fails, others pass at the same clock | Per-panel leg issue (FFC seating, connector, that panel's termination) | Inspect that panel's FFC/ZIF; the topology is point-to-point, so a single-leg fault is physical, not bus-wide |
| fps sags but soak is clean | Not a bus problem | Do not touch the clock; profile the frame (DL size, host timing) — fps alone neither accepts nor rejects an operating point |
| Clean at 30 MHz, want more throughput | Single-wire ceiling reached | Stop. Quad SPI on RiBus pins 9–16 is the v2 board path |

### The constant to touch, and how

One line changes per step, evidence in the comment — same pattern that made
the 24→8 MHz regression legible from the diff alone:

```c
/* Post-init SPI operating point. Board2 carrier PCB (LVC-buffered
 * point-to-point SPI): U9 soak passed at NN MHz on YYYY-MM-DD
 * (REG_ID/CMDB_SPACE soak clean, 30-min faults 0,0,0, delayed-sample=ON/OFF).
 * BT817 single-wire ceiling is 30 MHz -- beyond that is quad SPI, v2 board. */
static const uint32_t DASH_SPI_RUN_HZ = NN000000UL;
```

And before any of this: replace the current tree's
`2000000UL /* TEMP bench diagnostic ... do not commit */` value
(`MustangDash/MustangDash.ino:149`) with the verified 8 MHz baseline as the
walk's starting point.

## Related

- `docs/solutions/integration-issues/spi-run-clock-24mhz-overclock-corrupts-eve-coprocessor-reads.md`
  — the loom-era failure this board obsoletes, and the authoritative
  definition of the U9 read-integrity soak and the "fps alone never accepts
  an operating point" rule. This doc updates the physics; that doc still owns
  the acceptance procedure.
- `docs/hardware/three-panel-pin-reference.md` — RiBus 20-pin pinout, the
  ≤ 11 MHz init constraint, and the 30 MHz panel rating.
- `docs/solutions/best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md`
  — pre-init SPI facts (≤ 11 MHz until init completes).
- `docs/solutions/integration-issues/eve-panel-bringup-no-usb-enumeration-diagnosis.md`
  — bench isolation discipline that still applies when a single panel leg
  fails on the PCB.
- `docs/solutions/architecture-patterns/bt817-flash-resident-astc-assets.md`
  — the flash probe runs at the run clock and is the loudest canary for a bad
  operating point.
