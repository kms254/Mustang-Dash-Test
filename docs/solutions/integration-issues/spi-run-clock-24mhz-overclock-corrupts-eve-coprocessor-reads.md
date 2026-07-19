---
title: "Post-init SPI run clock at 24 MHz overclocks the bus: solid white screen, silent coprocessor read corruption"
date: 2026-07-10
category: integration-issues
module: display-bringup
problem_type: integration_issue
component: tooling
symptoms:
  - "panel showed solid white at DASH_SPI_RUN_HZ=24 MHz on real bench wiring, post-init SPI clock raise"
  - "EVE_init_flash() returned 0x01 (flash probe failed) so the boot splash was skipped"
  - "all 9 custom font inflates failed EVE_cmd_getptr() verification twice each and fell back to the ROM font"
  - "fps dropped from 60 to 25 while per-panel coprocessor fault counters stayed at 0, since corrupted REG_CMDB_SPACE reads never matched the fault signature in EVE_busy() and read as still busy instead"
  - "the serial protocol and EVE_init() itself kept working, since EVE_init ran at the safe 8 MHz before the clock was raised"
root_cause: config_error
resolution_type: config_change
severity: high
tags:
  - spi
  - bus-overclock
  - signal-integrity
  - bt817
  - eve4
  - dash-spi-run-hz
  - fault-detection
  - white-screen
last_refreshed: 2026-07-19
---

# Post-init SPI run clock at 24 MHz overclocks the bus: solid white screen, silent coprocessor read corruption

> **Scope note (2026-07-19):** the numeric conclusions here are **bench-loom
> era** — the 24 MHz failure and the 8 MHz operating point describe the
> jumper/FFC star harness, which the dash carrier PCB (buffered,
> source-terminated, point-to-point SPI) has since superseded. On the PCB the
> expected ceiling is 24–30 MHz, bounded by the BT817's 30 MHz rating — see
> `docs/solutions/architecture-patterns/dash-carrier-pcb-buffered-spi-topology-30mhz-clock-contract.md`
> for the new hardware→firmware contract and clock-walk procedure. **This doc
> remains authoritative** for the failure signature triple, the `EVE_busy()`
> fault-detection blind spot, the read-vs-write asymmetry, and the U9
> read-integrity soak acceptance rule — the clock-walk on the new board uses
> exactly that acceptance procedure.

## Problem

On `feat/side-panels`, the shared SPI bus rises once — after all three panels'
`EVE_init()` calls complete at the conservative init clock — to a single
post-init operating point, `DASH_SPI_RUN_HZ`
(`MustangDash/MustangDash.ino:137-145`, raise performed at
`MustangDash/MustangDash.ino:263-267`). The first candidate value, 24 MHz
(chosen because the BT817 panels are rated 30 MHz post-configuration per plan
KTD8), corrupted SPI reads on the actual bench wiring (jumper/FFC, not a
PCB) while largely leaving writes intact — and the firmware's only automatic
corruption detector, the coprocessor-fault check in `EVE_busy()`, never
tripped.

## Symptoms

First hardware run of the branch (center panel only, sides unwired, bus
raised to 24 MHz) produced a consistent triple:

- **Panel showed solid white** — the display list that reached the coprocessor
  was garbage, i.e. writes were accepted but rendered incorrectly.
- **Failed verification reads, with exact banner lines:**
  - `EVE_init_flash() returned 0x01 (E_OK=0x00), REG_FLASH_SIZE = ...` at
    `MustangDash/MustangDash.ino:277`, followed by
    `QSPI flash unavailable -> skipping splash (assets live in flash).` at
    `MustangDash/MustangDash.ino:303` — the flash probe (itself run at the
    raised clock, since it happens after the raise at
    `MustangDash/MustangDash.ino:263-267`, immediately followed by
    `EVE_init_flash()` at line 275) failed.
  - All 9 dash fonts: `Font %u inflate FAILED twice on panel %u -> ROM font 31
    there` (`MustangDash/MustangDash.ino:602-603`) — `load_dash_fonts()`
    (`MustangDash/MustangDash.ino:569-614`) issues `EVE_cmd_inflate()` then
    verifies via `EVE_cmd_getptr() == gend_expected` at line 586; both of the
    two allowed attempts per font failed that readback check, so every font
    instance fell back to the fixed ROM font.
- **fps sagged to 25 (target/normal 60) while `faults=0,0,0`** — the boot
  status ack reported zero coprocessor faults on all three panels despite the
  visibly broken frame rate and rendering.

Serial protocol itself remained fully functional throughout, and every
panel's `EVE_init()` + `REG_ID` read (`MustangDash/MustangDash.ino:255-259`)
came back healthy (`REG_ID 0x7C`) — because those all run before the raise,
at 8 MHz.

## What Didn't Work (diagnostic near-misses)

Nothing here was a "tried a fix, it failed" iteration — the branch went
straight from the first hardware boot to the isolation test below. The near
misses were interpretive, not procedural:

- **The fault counters gave false comfort.** `EVE_busy()`
  (`libraries/FT800-FT813/src/EVE_commands.c:466-509`) is the only place the
  firmware detects coprocessor corruption, and it does so by reading
  `REG_CMDB_SPACE` and checking one specific bit pattern:
  `(space & 3U) != 0U` at line 479. That signature means "the coprocessor
  itself reported a fault register error" — it says nothing about whether the
  16-bit value that was just read over a marginal bus is *itself* correct.
  Corrupted `REG_CMDB_SPACE` reads that happen to land on a value where
  `space & 3 == 0` don't match the fault signature at all; they fall through
  to the `else` branch (lines 489-502) and are read as ordinary "not full
  yet / still busy" states. `g_eve_faults` per panel
  (`MustangDash/MustangDash.ino:108`) therefore stayed `0,0,0` even though
  the bus was actively corrupting reads — the counter only detects a specific
  failure shape, not "the SPI link is unreliable."
- **fps alone was ambiguous.** A 60→25 fps sag is consistent with several
  benign causes (a slow frame's DL size, host-side timing jitter, a
  legitimately busy coprocessor). Nothing about "fps=25" on its own points at
  bus corruption; it only becomes diagnostic once cross-referenced with the
  read-verification failures (flash probe, font GETPTR) that happened in the
  same boot. This is exactly why the round's plan
  (`docs/plans/2026-07-10-002-feat-side-panels-plan.md`, U9) requires
  measuring the SPI operating point on two independent axes — fps *and* read
  integrity — rather than accepting a clock purely because the frame rate
  looks close to nominal.
- **A healthy `REG_ID` at init did not clear the bus.** Every panel's
  `EVE_init()` and its immediate `REG_ID` read
  (`MustangDash/MustangDash.ino:255`) run at the conservative init clock,
  strictly `<= 11 MHz` (bumped to 24 MHz only afterward, at
  `MustangDash/MustangDash.ino:263-267`). So "REG_ID == 0x7C for all three
  panels" only proves the wiring works at the init clock — it is not evidence
  about the run-rate clock at all, and reading it as a clean bill of health
  for the bus was the trap.

## Solution

**Isolation step:** re-ran the identical firmware with only
`DASH_SPI_RUN_HZ` changed to 8 MHz (i.e., never raising the clock past the
init rate) as a control test. Center-only bench regression at 8 MHz came back
fully healthy: `EVE_init_flash()` returned `E_OK` with CRC-current splash
assets, splash played 110 frames / 2000 ms, all 9 fonts inflated clean, DL
usage 409/648 words, fps=60, faults=`0,0,0`, and the odometer persisted
across reflashes. Because the only variable changed was the post-init clock,
this isolated the corruption to the SPI operating point itself rather than
firmware logic, panel wiring integrity in general, or the flash/font code
paths.

**Fix:** `DASH_SPI_RUN_HZ` was set to the bench-verified value, with the
failure evidence recorded directly in the comment above it, in commit
`bf993d9` on PR
[kms254/Mustang-Dash-Test#5](https://github.com/kms254/Mustang-Dash-Test/pull/5)
(unmerged as of 2026-07-10).

Before (pre-fix):
```c
/* Post-init SPI operating point (R11/KTD8). All three EVE_init()s run at the
 * conservative 8 MHz; the bus then rises to this once. 24 MHz is the first
 * candidate (panels rated 30 MHz post-config); U9's bench read-integrity
 * soak validates or tunes it -- fps alone never accepts an operating point. */
static const uint32_t DASH_SPI_RUN_HZ = 24000000UL;
```

After (current tree, `MustangDash/MustangDash.ino:137-145`):
```c
/* Post-init SPI operating point (R11/KTD8). All three EVE_init()s run at the
 * conservative 8 MHz; the bus then rises to this once. 24 MHz was the first
 * candidate (panels rated 30 MHz post-config) but FAILED read integrity on
 * the actual bench (2026-07-10): flash init returned 0x01, all 9 font
 * inflates failed GETPTR verification, and corrupted REG_CMDB_SPACE reads
 * dragged fps to 25 with faults=0 -- writes mostly survived, reads did not.
 * 8 MHz is the verified operating point until the U9 soak walks it up --
 * fps alone never accepts an operating point. */
static const uint32_t DASH_SPI_RUN_HZ = 8000000UL;
```

Banner excerpts, corrupt (24 MHz) vs. healthy (8 MHz):

- Corrupt: `EVE_init_flash() returned 0x01 ...` → `QSPI flash unavailable ->
  skipping splash (assets live in flash).`; `Font N inflate FAILED twice on
  panel 0 -> ROM font 31 there` (all 9 fonts); status ack `fps=25 ...
  faults=0,0,0`.
- Healthy: `EVE_init_flash()` returns `E_OK` with CRC-current assets; splash
  runs 110 frames / 2000 ms; every font inflate verified clean; DL usage
  409/648; `fps=60`; `faults=0,0,0`; odometer intact across reflashes.

## Why This Works

- **Init always succeeds because it never touches the risky clock.** All
  three panels' `EVE_init()` calls, and the `REG_ID` sanity read right after
  each, run at the fixed conservative init rate — required to be `<= 11 MHz`
  per the datasheet and this repo's hardware truths — and the bus-wide raise
  to `DASH_SPI_RUN_HZ` happens strictly afterward
  (`MustangDash/MustangDash.ino:263-267`), via `SPI.endTransaction()` /
  `SPI.beginTransaction()` with the new `SPISettings`. Nothing about the
  run-rate clock can break panel bring-up itself.
- **The run clock is bounded by bench wiring, not chip rating.** The BT817 is
  rated for 30 MHz post-configuration, and 24 MHz is comfortably under that —
  but the panels are connected over jumper/FFC bench wiring on this rig, not
  a PCB trace. Signal integrity at a given clock is a property of the
  physical link (trace/wire length, connector quality, grounding), not the
  silicon's rated ceiling, so "under the chip's rated max" was never a
  sufficient condition for this wiring.
- **Writes survived while reads failed because the two directions are not
  symmetric here.** MOSI-side display-list writes landed well enough to
  produce a rendered (if garbage) frame — the coprocessor executed *something*
  — while MISO-side reads (flash status, `EVE_cmd_getptr()` verification,
  `REG_CMDB_SPACE`) came back corrupted. This is consistent with the MISO
  path being the more marginal leg of this particular bench harness at
  24 MHz; the practical upshot is that a "renders" panel is not proof of a
  clean bus, because render-time correctness only exercises MOSI, while the
  system's only automatic corruption check depends on a MISO read.
- **The fault-signature blind spot in `EVE_busy()`.** The single detector for
  bus corruption is `(REG_CMDB_SPACE & 3) != 0`
  (`libraries/FT800-FT813/src/EVE_commands.c:479`) — a narrow, specific bit
  pattern meant to catch a genuine coprocessor-reported fault. A corrupted
  16-bit read that happens not to land on that pattern is silently
  reinterpreted as an ordinary FIFO-space value, which is why `g_eve_faults`
  read `0,0,0` throughout — the detector was never designed to catch "the
  read itself is wrong," only "the coprocessor told us it's wrong."

## Prevention

- **Acceptance rule: read-integrity soak, not fps.** Per
  `docs/plans/2026-07-10-002-feat-side-panels-plan.md` (U9 and the Definition
  of Done), no SPI operating-point raise may be accepted on frame rate alone.
  The bar is a per-panel tight read-soak of `REG_ID` (must read `0x7C` every
  time) and `REG_CMDB_SPACE` (zero `(space & 3) != 0` hits) over
  `>= 1,000,000` reads, plus a 30-minute soak ending with the per-panel fault
  counters at zero — any spurious fault at speed disqualifies the operating
  point regardless of measured fps.
- **Recognizable signature triple for future diagnosis:** solid/garbage
  render + a read-verification failure with an explicit non-`E_OK` return
  code (flash init, font `GETPTR`, or similar) + fps sag with
  `faults=0,0,0` on the status ack. Any two of these three showing up
  together on a clock change should be read as "SPI read corruption," not as
  three unrelated bugs — `faults=0` is not exculpatory once a verification
  read has already failed in the same boot.
- **Treat operating-point raises as bench-gated one-line constants, with the
  evidence in the comment.** `DASH_SPI_RUN_HZ` is a single constant and the
  comment directly above it is required to carry the dated bench result that
  justifies the current value — this is what let the 24→8 MHz regression be
  understood at a glance from the diff alone, and it's the pattern to keep
  for the next candidate the U9 soak clears.
- **The raise currently happens before, not after, everything that must
  verify reads — that's a known, accepted tradeoff, not a design flaw to fix
  silently.** `EVE_init_flash()` (`MustangDash/MustangDash.ino:275`) and
  `load_dash_fonts()` for every panel (`MustangDash/MustangDash.ino:309-315`)
  both run *after* the bus-wide raise, i.e. at `DASH_SPI_RUN_HZ`, by design
  (KTD8 wants the 3x bandwidth for flash/font upload, not just steady-state
  rendering). That means the chosen operating point must be read-clean, not
  merely render-clean, because flash provisioning and font loading are
  read-verification-heavy and will fail first and loudest at an unsafe clock
  — as they did here. Any future reorder (raising the clock only after
  flash/font upload) would change this tradeoff and should be treated as a
  deliberate architecture decision, not a quick fix.

## Related Issues

- `docs/solutions/architecture-patterns/dash-carrier-pcb-buffered-spi-topology-30mhz-clock-contract.md`
  — the successor for the *ceiling*: the carrier PCB's buffered topology,
  read-timing budget, LPSPI delayed-sampling knob, and the clock-walk that
  applies this doc's soak rule to walk the operating point up from 8 MHz.
- `docs/solutions/integration-issues/eve-panel-bringup-no-usb-enumeration-diagnosis.md`
  — same bench discipline (single-variable isolation against a known-good
  control state) and the doc whose "REG_ID 0x7C: the SPI link is good" ladder
  this learning scopes: REG_ID health at the init clock says nothing about
  the post-init run clock.
- `docs/solutions/ui-bugs/eve-font-format-l4-l2-confusion-serial-verification-blind-spot.md`
  — same "match the verification channel to the failure mode" rule, opposite
  outcome on the same check: in the L4/L2 bug the font GETPTR verification
  *passed* despite corruption (byte counts are format-agnostic); here it
  correctly *failed* and was the check that caught the problem.
- `docs/solutions/architecture-patterns/bt817-flash-resident-astc-assets.md`
  — the authoritative description of the `EVE_init_flash()` probe-first
  discipline whose `0x01` failure was this bug's loudest symptom.
- `docs/solutions/best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md`
  — the canonical pre-init SPI facts (≤ 11 MHz until init completes); this
  doc covers the post-init operating point those facts don't.
