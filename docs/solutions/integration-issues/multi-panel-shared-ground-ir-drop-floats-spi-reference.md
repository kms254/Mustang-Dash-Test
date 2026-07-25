---
title: Multi-panel SPI — a marginal shared ground floats the reference under summed current (works with one panel, fails with two)
date: 2026-07-24
category: integration-issues
module: stm32-migration
problem_type: integration_issue
component: tooling
symptoms:
  - "Each panel renders perfectly ALONE, but connect two and one (or both) fails -- white screen, black screen, or garbage; adding a second panel breaks a previously-working first one"
  - "/dash status shows faults climbing, retired incrementing (frame-drain read timeouts), creg=0x00 (MISO reads all-zero), eve=-- on the failing panel"
  - "A one-vs-two threshold: works with one panel's ground current, fails the moment a second panel's return current joins the same path"
  - "A clock walk appears to prove 13.5 MHz is 'too fast' (panels retire) -- but 13.5 runs two panels clean once the ground is solid"
root_cause: config_error
resolution_type: config_change
severity: high
tags: [spi, grounding, ground-bounce, ir-drop, bt817, multi-panel, stm32, f767, signal-integrity]
---

# Multi-panel SPI — a marginal shared ground floats the reference under summed current

## Problem

Bringing up two Riverdi EVE4 (BT817) panels on the NUCLEO-F767ZI (center 7"
on SPI1, left 5" on SPI2), each panel rendered flawlessly *on its own*, but the
moment both were connected one or both failed — white screens, black screens,
climbing `faults`, and `retired` panels. Hours went into blaming the SPI clock,
a "dead" panel, and the buck supply. The actual cause was the **common ground**:
it was routed through a high-resistance path (a breadboard power rail, then a
single thin MCU-GND jumper), and the *summed* return current of two panels
developed enough IR-drop to float the panels' ground relative to the MCU's SPI
drivers, corrupting every SPI read.

## Symptoms

- Panel A works alone; panel B works alone; connect both and one fails (white/
  black/garbage). Adding the second panel breaks the first.
- `/dash status`: `faults` climbing, `retired` incrementing, `creg=id:0x00`
  (MISO reading all-zero), `eve=--` on the failing panel. `retired` counts
  frame-drain read timeouts that kill a panel at runtime (`MustangDash.ino:211`).
- **A one-vs-two threshold** — the tell. It holds with one panel's ground
  return (~0.17 A) and breaks when a second panel's return (~0.35 A total)
  joins the same marginal path.
- White screen specifically = the panel's logic ground is floating relative to
  the MCU (no valid SPI reference) → BT817 never initializes.

## What Didn't Work

Several dead ends, each a mis-attribution of a grounding problem to something
else — all reframed once the ground was fixed:

- **Blaming the SPI run clock.** Walked 13.5 → 6.75 → 3.375 MHz. 6.75 rendered
  but the center faulted (short DL → black); 3.375 built a full fault-free DL
  but was too slow and retired at boot. "The clock can't thread the needle" —
  because it was never the clock. **13.5 MHz runs two panels clean once grounds
  are solid.** (Reframes the earlier clock-walk notes:
  [f767-spi-prescaler-quantization-27mhz-hard-wedge.md](f767-spi-prescaler-quantization-27mhz-hard-wedge.md),
  [spi-run-clock-24mhz-overclock-corrupts-eve-coprocessor-reads.md](spi-run-clock-24mhz-overclock-corrupts-eve-coprocessor-reads.md).)
- **Concluding a BT817 was dead.** A panel read `REG_ID 0x68` (not `0x7C`) on
  the *center's* known-good slot + a brand-new FFC — looked like damaged
  silicon. It came up clean on the Riverdi eval board: `0x68` was a **marginal
  FFC/read contact**, not a dead chip. (`0x68` = real-but-wrong byte = corrupted
  read; `0xFF` = open line; `0x00` = MISO low.)
- **Blaming the buck supply / a "bad 5-inch-to-buck wire."** The *same cable*
  worked when moved to the MCU's 3.3 V + GND, and the buck powered the other
  panel fine. The variable was the ground *path*, not the buck or the wire.
- **Star-wiring power direct from the buck** (bypassing a breadboard power bus).
  Improved but didn't fix it — because the offending path was the *ground*
  return, not the positive rail.

## Solution

Route **all panel grounds to one solid, low-resistance ground bus** — a screw
terminal block, bus bar, or WAGO lever-nut, **never a breadboard rail** — bonded
to the MCU (Nucleo) GND with a heavy, short (or doubled) wire. With the ground
solid, two panels render clean at the full **13.5 MHz** run clock
(`fps=59, faults=0,0,0, eve=ok,ok`).

Two interim proofs confirmed the mechanism before the bus was in hand:

- **Doubling the MCU-GND wires** (parallel conductors = lower resistance)
  restored two-panel operation immediately.
- **Feeding a panel from the MCU's own 3.3 V + GND** (the same reference as the
  SPI drivers) rendered it clean at `faults=0` — bypassing the shared marginal
  ground entirely.

Sizing note: the ground bus carries the *summed* logic return (~0.17 A/panel,
~0.5 A for three); a 2 A buck is fine on current — the failure is always
resistance, never capacity.

## Why This Works

The BT817 frame-drain polls its command-FIFO registers over SPI every frame;
those reads are referenced to the MCU's ground. Ground offset is
`V = I_total × R_path`. Through a breadboard rail or a thin jumper, `R_path` is
small but non-zero, so:

- **One panel** (~0.17 A) → a tolerable offset → reads land → renders.
- **Two panels** (~0.35 A through the *same* path) → **double the offset** →
  the SPI reference shifts enough that reads sample wrong bits → the firmware
  either detects a (phantom) coprocessor fault (`faults`) or the drain never
  sees completion and times out (`retired`) → the panel is marked dead →
  white/black glass.

Lowering `R_path` (a solid bus, or more parallel copper) drops the offset below
the corruption threshold **regardless of panel count**. The clock was a red
herring: slower clocks changed the *failure mode* (fault vs. retire) but never
removed it, because the corruption source was the ground reference, not edge
rate.

## Prevention

- **Never carry power or ground for multiple panels through a breadboard rail.**
  Spring contacts are high-resistance; the summed current IR-drops. Breadboards
  for signals only — anything over a few tens of mA wants solid, clamped/soldered
  contact.
- **One solid ground bus** (terminal block / bus bar / WAGO), with a **heavy,
  short bond to MCU GND**. Scale headroom with panel count.
- **Never drive SPI into a panel whose ground isn't bonded to the MCU** — a
  floating ground gives a white screen (and stresses the BT817 I/O).
- **Change one variable at a time when a panel misbehaves:**
  1. *Isolate the panel* — drop it on the vendor eval board (clean connector) to
     prove the silicon before condemning it.
  2. *Isolate power/ground* — feed one panel from the MCU's own rail to remove
     the shared-supply variable.
- **`/dash status` read-diagnostic cheat-sheet (F767):** `creg id` — `0x7C`
  healthy, `0xFF` open/floating read line, `0x00` MISO stuck low, other-wrong
  (e.g. `0x68`) marginal read contact; `faults` = read corruption at clock speed;
  `retired` = frame-drain read timeouts (panel gave up, then shows `eve=--`).
- **Signature to recognize:** "works alone, fails together" with a one-vs-two
  (or two-vs-three) threshold = a shared marginal ground/power path saturating
  under summed load — not the panels, not the clock, not the firmware.
