---
title: "BT817 flash render-streaming has a per-frame bandwidth ceiling"
date: 2026-07-21
category: architecture-patterns
module: display-bringup
problem_type: architecture_pattern
component: tooling
severity: medium
applies_when:
  - "deciding whether a static bitmap should render direct from panel QSPI flash or be staged into RAM_G first"
  - "a flash-resident bitmap renders with correct content but horizontal line dropouts or tearing"
  - "REG_FLASH_STATUS already reads 3 (full speed) and flash-sourced rendering still tears"
  - "sizing a full-screen background or other large ASTC asset for a BT81x/EVE4 panel"
tags:
  - bt817
  - eve4
  - astc
  - qspi-flash
  - ram_g
  - flash-streaming
  - bandwidth
---

# BT817 flash render-streaming has a per-frame bandwidth ceiling

> **Scope note (2026-07-21):** the flash-source fallback this doc discusses
> was removed by the 2026-07-21 MCU-direct splash rewrite — the splash now
> stages the embedded pack MCU-flash -> RAM_G at boot with no flash-render
> path left at all. The bandwidth-ceiling finding itself stands unchanged;
> it's the reason rendering is RAM_G-only now, not merely the preferred path.

## Context

The boot splash originally rendered its ASTC assets direct from the panel's
QSPI flash, using zero RAM_G. On 2026-07-10 that was changed to stage assets
flash -> RAM_G at boot, with the flash source kept as an automatic fallback.
The comment block at `MustangDash/splash_render.h:19-31` records the reason as
bench forensics on one center module "whose flash *render-streaming* path was
broken" while its control path (`CMD_FLASHREAD`/`CMD_FLASHUPDATE`, CRC) stayed
healthy — i.e. the failure was attributed to a fault in that specific module.

That attribution was incomplete. Testing on 2026-07-20/21 established that the
tearing is a **per-frame bandwidth ceiling that every module has**, not a
defect, and that it persists with the flash controller confirmed in full-speed
mode. Vendor documentation and Bridgetek support both describe this limit
directly (see Guidance). Staging is therefore the correct architecture, not a
workaround — the firmware independently arrived at Bridgetek's own recommended
pattern.

The evidence was gathered through EVE Screen Editor 5.3 driving the panels over
the Riverdi eval board's FTDI bridge — a toolchain entirely independent of the
Teensy firmware, so it corroborates rather than re-runs the original finding.

## Guidance

**Do not render large ASTC bitmaps direct from panel flash. Stage them into
RAM_G at boot.** Reserve flash-direct rendering for small assets.

The observed threshold on this hardware sits between **40 KB and 56 KB per
asset**. Below it, flash-direct rendering is clean; above it, artifacts appear
and worsen with size.

This matches vendor guidance closely. Direct-from-flash ASTC rendering is a
documented, first-class BT81x feature — BRT_AN_033 v2.8 §2.9 describes the
graphics engine fetching assets straight from flash, and §6 markets it as
displaying images "without consuming valuable RAM_G space" — but Bridgetek
qualifies it as bandwidth-limited against the real-time frame budget. Their
support staff put the comfortable size at **"a few tens of KBytes,"** which
lands on the same threshold measured here, and their recommendation for
anything larger is to load into RAM_G.

The vendor's own diagnosis of this failure mode appears on BRT Community topic
485 ("Display ASTC images directly from BT817 flash", Dec 2023 – Jan 2024),
where two users report the identical symptom shape — correct content, streaks
and corruption, worsening as more or larger ASTC assets are added, clean when
copied to RAM_G first. Bridgetek staff there: "a large image or one which is of
greater quality (e.g. ASTC 4x4) or multiple images may take longer than the
time available... it is suggested to [use RAM_G] as the access time will be
very low." Rudolph Riedel — author of the `FT800-FT813` library vendored in
this repo — adds: "I would rather put as much as possible in RAM_G before
displaying directly from FLASH."

**`REG_UNDERRUN` is the diagnostic.** Bridgetek staff recommend polling it in a
loop after displaying, to confirm the engine is running out of frame time
rather than hitting a rendering fault.

Two partial mitigations exist, neither a substitute for staging:

- **System clock.** The BT817 default is **60 MHz**, not 72; BRT_AN_033 states
  "72MHz is recommended for optimal performance," set via the `CLKSEL` host
  command (`EVE_CLKSEL`, `0x61`, `EVE.h:171`) with `REG_FREQUENCY`
  (`0x0030200C`, `EVE.h:522`) updated manually to match. Riedel notes 72 MHz
  "also increases the clock for the external SPI flash," so it raises flash
  headroom specifically. 72 MHz is the top of the documented range — there is
  no supported path above it. This is separate from the host SPI bus clock.
- **`REG_ADAPTIVE_FRAMERATE`** — see the test-setup caveat under Examples.

Critically, **`REG_FLASH_STATUS == 3` (full speed) does not lift this ceiling.**
Confirming full-speed mode is worth doing to rule it out, but it is not a fix.
Issuing `CMD_FLASHFAST` when the controller is already at 3 changes nothing.

Useful registers and their definitions in the vendored library:

| Symbol | Address | Defined at |
|---|---|---|
| `REG_FLASH_STATUS` | `0x003025F0` | `libraries/FT800-FT813/src/EVE.h:1150` |
| `REG_FLASH_SIZE` | `0x00309024` | `libraries/FT800-FT813/src/EVE.h:1151` |
| `CMD_FLASHFAST` | `0xFFFFFF4A` | `libraries/FT800-FT813/src/EVE.h:1128` |

`REG_FLASH_STATUS` values are enumerated at
`libraries/FT800-FT813/src/EVE_commands.h:171-174`: `0` INIT, `1` DETACHED,
`2` BASIC, `3` FULL.

`CMD_FLASHFAST` takes a result address and writes `0` on success or an error
code. The meanings below are quoted from EVE Screen Editor's own command
reference. The library defines five matching `EVE_FAIL_FLASHFAST_*` constants
at `libraries/FT800-FT813/src/EVE_commands.h:161-165` — the pairing here is
**inferred from ordinal position**, not verified against vendor documentation,
and the first row is the weakest (the vendor calls `0xe001` "not attached"
while the library names its first constant `NOT_SUPPORTED`):

| Vendor code | Library constant | Meaning |
|---|---|---|
| `0xe001` | `EVE_FAIL_FLASHFAST_NOT_SUPPORTED` | flash not attached |
| `0xe002` | `EVE_FAIL_FLASHFAST_NO_HEADER_DETECTED` | no header in sector 0 |
| `0xe003` | `EVE_FAIL_FLASHFAST_SECTOR0_FAILED` | sector 0 integrity check failed |
| `0xe004` | `EVE_FAIL_FLASHFAST_BLOB_MISMATCH` | device/blob mismatch |
| `0xe005` | `EVE_FAIL_FLASHFAST_SPEED_TEST` | failed full-speed test |

`EVE_cmd_flashfast()` (declared at `EVE_commands.h:241`) returns these, so a
single `Serial.printf` of its return value is the cheapest diagnosis path on
the firmware side.

## Why This Matters

Flash-direct rendering is attractive because it costs zero RAM_G, and RAM_G on
this project is heavily contended — the dash fonts alone occupy ~273 KB. It is
tempting to treat the staging step as a defensive workaround that could be
removed once a "good" module is fitted. It cannot: both modules tested fail at
the real workload, so removing staging would reintroduce visible tearing on the
shipped splash.

Equally, the failure is easy to misdiagnose. The artifact is *correct content
with horizontal line dropouts* — not scrambled blocks. That distinction matters:
scrambled blocks indicate wrong addresses, bad alignment, or a missing 2x2 ASTC
swizzle, whereas correct-but-torn content indicates the data is right and the
delivery rate is not. Chasing SPI clock, display timings, or the ASTC pipeline
after seeing tearing is wasted effort; all three were eliminated here.

## When to Apply

- Any static bitmap larger than ~40 KB destined for a BT81x/EVE4 panel
- Full-screen backgrounds in particular, which are the worst case
- When flash-sourced content tears but small flash-sourced assets render clean
- Before attributing flash render artifacts to a defective panel module

## Examples

Measurements, both modules with `REG_FLASH_STATUS = 3` and `REG_FLASH_SIZE = 64`:

**7" RVT70H (1024x600)** — asset sizes from `MustangDash/splash_flash.h`:

| Asset | Size | Flash-direct result |
|---|---|---|
| `bg-blue-1024x600` (ASTC 8x8) | 153,600 B | heavy tearing |
| `wordmark-mustang-700x80` | 56,000 B | one band |
| `emblem-200x200` | 40,000 B | clean |
| `line-blue-340x40` | 13,600 B | clean |
| `bars-chrome-240x45` | 11,520 B | clean |
| `year-1965-blue` | 5,600 B | clean |

Staging the background and wordmark into RAM_G while leaving the four small
assets on flash renders clean — the same hybrid the firmware performs.

**5" RVT50H (800x480)**, custom ESE device profile built from
`libraries/FT800-FT813/src/EVE_config.h:802-823`:

| Load | Result |
|---|---|
| background x1 | clean |
| background x3 | torn |
| full splash asset set (~280 KB/frame) | torn |

Both modules fail at the full splash asset set, which is the shipping workload.

### Scoping — what this evidence does not establish

The 5" tolerated more load before tearing than the 7" did, but the comparison
is confounded: an 800x480 panel clips a 1024x600 background, so it demanded
roughly 62% of the reads. **Do not read a part-to-part quality difference into
these numbers.**

### Test-setup caveat — adaptive framerate was disabled

`REG_ADAPTIVE_FRAMERATE` (`0x0030257C`, `libraries/FT800-FT813/src/EVE.h:1148`)
**defaults to 1 (enabled)** on BT817 per the BT817/8 datasheet register map and
BRT_AN_033 v2.8. When enabled, the graphics engine suspends PCLK if the next
scan-out line is not ready, stretching the frame instead of scanning out a torn
one.

The ESE device profile used for these measurements had it set to **0**, so
every result above was taken with that protection *off*, not at hardware
defaults. The measurements are still valid as a relative comparison, but the
absolute thresholds may be pessimistic versus a default-configured chip.

This has not been re-tested with it enabled. Note that vendor sources do not
present it as a flash-specific remedy, and the forum case below tried frame-rate
and `REG_AH_HCYCLE_MAX` timing tweaks which "helped... but failed on a more
thorough test" — so it is unlikely to be a complete fix.

### Why no published throughput number

A 153,600 B asset at 60 fps is only ~9.2 MB/s, which intuitively should not
trouble a quad-rate flash interface. Bridgetek does not publish a bandwidth
figure for the flash-direct path; they characterize the failure qualitatively
instead, as the graphics engine failing to retrieve an asset within the
real-time frame budget. `REG_UNDERRUN` is the register their support staff
point at for detecting it. The absolute ceiling therefore has no documented
number to check against — only the empirical thresholds above and the vendor's
"few tens of KBytes" rule of thumb.

## Related

- `docs/solutions/architecture-patterns/bt817-flash-resident-astc-assets.md` —
  the flash-resident asset pipeline this ceiling constrains. Note its
  "Probe-first discipline" bullet frames reaching full-speed mode as sufficient
  for the pipeline; this doc narrows that claim.
- `docs/solutions/design-patterns/eve-ram-g-budgeting-multi-theme-splash-assets.md` —
  the RAM_G budget that staging consumes
- `docs/solutions/integration-issues/spi-run-clock-24mhz-overclock-corrupts-eve-coprocessor-reads.md` —
  a different failure mode with a superficially similar presentation; that one
  is transfer corruption, this one is render bandwidth
- `docs/solutions/tooling-decisions/astc-swizzle-validated-against-eve-asset-builder.md` —
  the swizzle correctness this session also confirmed on hardware

### External sources

- BRT_AN_033 "BT81X Series Programming Guide" v2.8 — §2.9 Flash Interface,
  §6 ASTC, `BITMAP_SOURCE` and `REG_ADAPTIVE_FRAMERATE` definitions
- BT817/8 Datasheet v1.2 — register map, `CLKSEL` host command table,
  §4.3 adaptive HSYNC / adaptive framerate
- BRT Community topic 485, "Display ASTC images directly from BT817 flash"
  (Dec 2023 – Jan 2024) — <http://www.brtcommunity.com/index.php?topic=485.0>
  — independent reports of this symptom, with Bridgetek staff and Rudolph
  Riedel diagnosing it as a flash real-time bandwidth limit
