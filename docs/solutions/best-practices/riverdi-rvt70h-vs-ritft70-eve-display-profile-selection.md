---
title: "Selecting the correct EVE display profile for Riverdi 7-inch panels (EVE_RVT70H vs EVE_RiTFT70)"
module: display-bringup
category: best-practices
date: 2026-07-09
problem_type: best_practice
component: tooling
severity: high
applies_when:
  - "configuring an EVE display profile in libraries/FT800-FT813/src/EVE_config.h"
  - "bringing up a Riverdi BT81x (EVE3/EVE4) display panel"
  - "matching a Riverdi RVT70xx model number to a library profile define"
symptoms:
  - "three Riverdi 7-inch profiles with confusingly similar names (EVE_RVT70, EVE_RiTFT70, EVE_RVT70H)"
  - "wrong profile still passes SPI init and REG_ID reads 0x7C, but panel output is garbled, scrolled, or blank"
root_cause: config_error
resolution_type: config_change
related_components:
  - development_workflow
tags:
  - eve4
  - bt817
  - riverdi
  - rvt70h
  - eve-config
  - display-profile
  - teensy41
  - ft800-ft813
---

# Selecting the correct EVE display profile for Riverdi 7-inch panels (EVE_RVT70H vs EVE_RiTFT70)

## Context

During bring-up of the Riverdi SM-RVT70HSBNWN00 (7.0" 1024x600 IPS, BT817 / EVE4, no touch) on a Teensy 4.1 with the vendored RudolphRiedel FT800-FT813 library (v5.0.10, `libraries/FT800-FT813`), the display profile had to be selected in `EVE_config.h`. The library ships three Riverdi 7-inch profiles (`EVE_RVT70`, `EVE_RiTFT70`, `EVE_RVT70H`); the two whose names are most easily confused for this panel are `EVE_RiTFT70` and `EVE_RVT70H` — and the task description itself suggested "EVE_RiTFT70 or the closest current name". `EVE_RiTFT70` reads like "Riverdi TFT 7.0" but is actually the 800x480 EVE3 (BT815/BT816) profile; following the hint would have configured 800x480 timings on a 1024x600 panel. The correct profile for this panel is `EVE_RVT70H`.

## Guidance

Pick the profile by matching the Riverdi model number against the comment above each config block in `libraries/FT800-FT813/src/EVE_config.h` — never by how the profile name reads.

Model-number decoding rule: the two letters after the size digits encode the series.

- `RVT70HS...` — the "H" series: 1024x600, BT817, EVE4. Library profile suffix `H` (`EVE_RVT70H`), whose block comment is "RVT70HSBxxxxx 1024x600 7.0\" Riverdi, various options, BT817" (libraries/FT800-FT813/src/EVE_config.h:907).
- `RVT70xQB...` — 800x480, BT815/BT816 (EVE3): `EVE_RiTFT70`, whose block comment is "RVT70xQBxxxxx 800x480 7.0\" Riverdi, various options, BT815/BT816" (libraries/FT800-FT813/src/EVE_config.h:750).
- `RVT70xQF...` (e.g. RVT70UQFNWC0x) — 800x480, FT812/FT813 (EVE2): a third profile, `EVE_RVT70` (libraries/FT800-FT813/src/EVE_config.h:698).

The commented-out master list of profiles near the top of the file is grouped by chipset ("BT817 / BT818" at libraries/FT800-FT813/src/EVE_config.h:112, "BT815 / BT816" at :125, "FT812 / F813" at :152), so a profile's section immediately tells you its EVE generation. Cross-check the chip printed on the panel's datasheet against the section before trusting a name.

To enable the profile, define it in `EVE_config.h` right after the include guard (this project does so at libraries/FT800-FT813/src/EVE_config.h:101):

```c
#ifndef EVE_CONFIG_H
#define EVE_CONFIG_H

#define EVE_RVT70H   /* RVT70HSBxxxxx 1024x600 7.0" Riverdi, BT817 (EVE4) */
```

Placing the define in the header (rather than a `-D` build flag) makes it active for both the library translation units and the sketch, and works identically from the Arduino IDE and arduino-cli.

Adjacent configuration that goes with the profile:

- Pins are set in `libraries/FT800-FT813/src/EVE_target/EVE_target_Arduino_Teensy4.h`: `EVE_CS 14` and `EVE_PDN 17`, each guarded by `#if !defined` (libraries/FT800-FT813/src/EVE_target/EVE_target_Arduino_Teensy4.h:61-67), so `-D EVE_CS=` / `-D EVE_PDN=` build flags still override.
- SPI must stay at or below 11 MHz until EVE init completes (BT817 requirement); the sketch uses 8 MHz, mode 0, MSB-first (MustangDash/MustangDash.ino:72-76).

## Why This Matters

Choosing the wrong profile is a silent misconfiguration. SPI init still succeeds and REG_ID still reads 0x7C on a healthy BT81x regardless of which profile is selected — REG_ID health does not validate the profile. But HSIZE/VSIZE and the sync timings are wrong (`EVE_RiTFT70` sets `Resolution_800x480`, expanding to `EVE_HSIZE 800` / `EVE_VSIZE 480` at libraries/FT800-FT813/src/EVE_config.h:1380-1382, with `EVE_GEN 3` at :755), so the panel shows a garbled, scrolled, or dark image while nothing on the serial output flags a problem. The wrong profile also selects the wrong EVE generation, so BT817-specific features such as `EVE_PCLK_FREQ` are never configured.

Note: in this environment the fix was verified by reading the library source and a clean teensy41 compile (the configured `EVE_HSIZE`/`EVE_VSIZE` flow into the sketch); the hardware render was not exercised here as no board was attached.

## When to Apply

- Configuring any EVE2/EVE3/EVE4 display profile in the FT800-FT813 library — the profile fully determines resolution, sync timings, and `EVE_GEN`.
- Bringing up any Riverdi panel: decode the model number (`RVT<size><series>...`) and match it to the block comment, not the profile name.
- Whenever a task description, forum post, or AI suggestion names a specific profile define: verify the suggested name against the config block's comment, `EVE_HSIZE`/`EVE_VSIZE`, and `EVE_GEN` before enabling it.

## Examples

Wrong (what the task hint suggested) — `EVE_RiTFT70` is an 800x480 EVE3 profile:

```c
/* RVT70xQBxxxxx 800x480 7.0" Riverdi, various options, BT815/BT816 */  /* EVE_config.h:750 */
#if defined (EVE_RiTFT70)                                               /* EVE_config.h:752 */
#define Resolution_800x480                                              /* EVE_config.h:753 -> HSIZE 800, VSIZE 480 (EVE_config.h:1380-1382) */
...
#define EVE_GEN 3                                                       /* EVE_config.h:760 */
```

Right (matches SM-RVT70HSBNWN00) — `EVE_RVT70H` is the 1024x600 EVE4 profile:

```c
/* tested with RVT70HSBNWC00-B */                                       /* EVE_config.h:906 */
/* RVT70HSBxxxxx 1024x600 7.0" Riverdi, various options, BT817 */       /* EVE_config.h:907 */
#if defined (EVE_RVT70H)                                                /* EVE_config.h:908 */
#define EVE_HSIZE ((uint32_t) 1024UL)                                   /* EVE_config.h:909 */
#define EVE_VSIZE ((uint32_t) 600UL)                                    /* EVE_config.h:910 */
...
#define EVE_PCLK_FREQ ((uint32_t) 0x0D12UL) /* -> 51MHz */              /* EVE_config.h:920 */
#define EVE_GEN 4                                                       /* EVE_config.h:925 */
#if !defined (EVE_BACKLIGHT_FREQ)
#define EVE_BACKLIGHT_FREQ ((uint32_t) 4000UL) /* 4kHz per Riverdi */   /* EVE_config.h:927 */
#endif
```

(All EVE_config.h citations are relative to `libraries/FT800-FT813/src/`.)

## Related

- [CLAUDE.md](../../../CLAUDE.md) — "Library gotchas" section states the one-line rule (EVE_RVT70H, not EVE_RiTFT70); this doc carries the fuller why/symptoms/diagnosis narrative.
- [README.md](../../../README.md) — documents the panel, the profile choice, and a serial-symptom troubleshooting table for init/REG_ID failures.
- GitHub issue search skipped for this learning (no `gh` CLI in the build sandbox; two-commit-old repo with no issue history).
