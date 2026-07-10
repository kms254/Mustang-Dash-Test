---
title: "Backlight PWM at the library's 4 kHz default whines audibly"
date: 2026-07-09
category: integration-issues
module: display-bringup
problem_type: integration_issue
component: tooling
symptoms:
  - "audible high-pitched whine from the panel's backlight driver, tracking the backlight PWM duty as it changes"
root_cause: config_error
resolution_type: config_change
severity: medium
tags:
  - backlight
  - pwm
  - reg-pwm-hz
  - rvt70h
  - bt817
  - riverdi
  - audible-noise
---

# Backlight PWM at the library's 4 kHz default whines audibly

## Problem

With the backlight pulsing between duty 20 and 128, the panel emitted an
audible whine that tracked the brightness sweep. The FT800-FT813 library's
`EVE_RVT70H` profile sets the backlight PWM frequency to 4 kHz — inside the
most sensitive band of human hearing — so the backlight driver's passives
sing along with the switching.

## Symptoms

- A high-pitched whine from the display module, loudest at mid-range
  `REG_PWM_DUTY` values, changing pitch/level as the duty sweeps.

## What Didn't Work

- Nothing else was tried; the frequency was identified as the likely cause
  directly, because 4 kHz is audible by construction and the panel datasheet
  disagrees with the library default.

## Solution

Override the profile's default with the maximum the register supports, in the
project block of `EVE_config.h` (the profile's own definition is guarded by
`#if !defined`, so an earlier define wins — libraries/FT800-FT813/src/EVE_config.h:927):

```c
/* libraries/FT800-FT813/src/EVE_config.h:106 */
#define EVE_BACKLIGHT_FREQ ((uint32_t) 10000UL)
```

The invariant test pins the value so it cannot silently regress
(tests/test_eve_config.c:34). Additionally, running the backlight *steady at
duty 128 (100%)* removes switching edges entirely — a fully-on PWM is DC —
which is why the steady-backlight firmware is silent even independent of the
frequency change.

## Why This Works

- This panel's own datasheet (RVT70HSBNWN00 V1.1A, section 10.4) recommends a
  backlight PWM frequency of 10–100 kHz; the BT817's `REG_PWM_HZ` register
  tops out at 10,000 Hz, so 10 kHz is the closest legal value — and it sits
  at the edge of adult hearing instead of the middle of it.
- The library's 4 kHz default ("as recommended by Riverdi") was calibrated
  for older Riverdi modules with different LED drivers; per this session's
  reading of the library changelog, Riverdi FT81x/BT815/816 modules were even
  moved to 250 Hz for a CAT4238 driver whose PWM range is 100 Hz–2 kHz.
  Defaults encode *some* panel's constraints — not necessarily yours.

## Prevention

- When adopting a display profile, check the panel's *own* datasheet
  electrical/backlight section against the profile's `EVE_BACKLIGHT_FREQ`
  (and any other `#if !defined`-guarded defaults) instead of assuming the
  library default matches the module revision.
- The invariant suite (`tests/run-tests.sh`) asserts the 10 kHz value with the
  rationale in the assert message, so a future library re-vendor that drops
  the override fails the test instead of reintroducing the whine.

## Related Issues

- [docs/solutions/best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md](../best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md) —
  same lesson generalized: verify profile constants against the physical panel,
  not the profile's name or defaults.
