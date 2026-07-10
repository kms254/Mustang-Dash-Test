# Claude Code Handoff — Mustang Dash, Three-Panel Pin Reference

Project: github.com/kms254/Mustang-Dash-Test
Purpose: complete, verified hardware truth for the three-panel bring-up so firmware
can be reviewed/extended against real pin assignments, not assumptions.

---

## Hardware summary

- Host: Teensy 4.1, bare (not the Copperhill triple-CAN board), bench-prototyped
  on breadboards, headers soldered to MTCELL FPC-20P 0.5mm breakouts.
- Displays: 3x Riverdi EVE4, BT817Q controller, RiBus 20-pin 0.5mm ZIF, no-touch:
  - Center: SM-RVT70HSBNWN00, 1024x600, 1000 nit
  - Left: SM-RVT50HQBNWN00, 800x480, 1000 nit
  - Right: SM-RVT50HQBNWN00, 800x480, 1000 nit
- SPI bus is SHARED (single SCK/MOSI/MISO) across all three panels. Each panel has
  its own dedicated CS and PD (power-down/reset) pin. This is the critical
  constraint: only one panel's CS may be asserted (low) at a time, or bus
  contention results. Verify the firmware's EVE instance-management explicitly
  guards against overlapping CS assertion across the three display contexts.
- Bench power: Teensy runs off USB (5V/3.3V via its own regulator) for logic;
  panel backlights run off a bench buck independently calibrated to 5.00V.
  Common ground ties Teensy, buck, and all three panel breakouts together.
- Library target: RudolphRiedel/FT800-FT813 (or current fork/successor), same
  family as the Skygauge/FT8XX Teensy-4.1-tuned fork referenced during bring-up.
- Panel 1 (center 7") is bench-verified working: HELLO MUSTANG rendered, SPI
  confirmed alive, REG_PWM_DUTY software backlight dimming confirmed via a
  pulsing loop. Panels 2 and 3 are being wired now using the same method.

---

## Teensy 4.1 pin assignment (all three panels, final)

| Function | Teensy pin | Notes |
|---|---|---|
| SPI SCK (shared, all 3 panels) | 13 | Hardware SPI |
| SPI MOSI (shared, all 3 panels) | 11 | Hardware SPI |
| SPI MISO (shared, all 3 panels) | 12 | Hardware SPI |
| CS — Center 7" | 14 | Unique per panel |
| CS — Left 5" | 15 | Unique per panel |
| CS — Right 5" | 16 | Unique per panel |
| PD/RST — Center 7" | 17 | Active low |
| PD/RST — Left 5" | 20 | Active low (18/19 skipped — see below) |
| PD/RST — Right 5" | 21 | Active low |
| I2C SDA0/SCL0 (reserved, unwired) | 18, 19 | Deliberately left free: the Teensy 4.1's primary I2C pair, and the last default-I2C pins available once CAN (0/1/22/23), telltales (2–9), and buttons (24–27) are counted. Future tenant: e.g. an ambient-light sensor for the unified dash dimmer. |
| CAN1 TX (Ford bus, not yet wired) | 22 | Stage 4 |
| CAN1 RX (Ford bus, not yet wired) | 23 | Stage 4 |
| CAN2 TX (chassis bus, not yet wired) | 1 | Stage 4 |
| CAN2 RX (chassis bus, not yet wired) | 0 | Stage 4 |
| Telltale 1-8 (not yet wired) | 2,3,4,5,6,7,8,9 | Stage 5, PWM-capable |
| Buttons 1-4 (not yet wired) | 24,25,26,27 | Stage 5.5, INPUT_PULLUP |
| VIN | — | NOT connected on bench (USB-powered). Will take board 5V on final PCB. |
| 3.3V out | — | Feeds all 3 panels' VDD logic on bench. Watch for regulator strain with 3 panels loaded (~0.52A combined typ per datasheet); if flicker/reset/glitch appears during soak, move panel VDD to a dedicated 3.3V source instead of the Teensy's onboard regulator. |

---

## RiBus 20-pin signal definition (identical pinout, all panel sizes)

| RiBus pin | Signal | Wired to |
|---|---|---|
| 1 | VDD 3.3V logic | Teensy 3.3V pin |
| 2 | GND | Common ground |
| 3 | SCLK | Teensy 13 |
| 4 | MISO | Teensy 12 |
| 5 | MOSI | Teensy 11 |
| 6 | CS | Teensy 14 / 15 / 16 (per panel) |
| 7 | INT | Not connected (poll instead) |
| 8 | RST/PD (active low) | Teensy 17 / 20 / 21 (per panel) |
| 9-16 | GPIO/QSPI/audio | Not connected (single-SPI mode, no audio) |
| 17, 18 | BLVDD backlight 5V | Bench buck 5.00V out (doubled, heavy jumpers) |
| 19, 20 | BLGND backlight return | Bench buck GND (doubled, heavy jumpers) |

---

## Breakout pad mapping (MTCELL FPC-20P 0.5mm, "straight" case)

Confirmed by continuity test (panel frame to breakout pad 20 beeps; pad 1 stays
silent). This board's silkscreen pad numbers equal the RiBus pin numbers
directly (pad N = RiBus N). Verify this same test on the breakouts for panels 2
and 3 individually before trusting the mapping — do not assume it carries over
from unit to unit without re-testing.

| Breakout pad | RiBus pin | Teensy / supply |
|---|---|---|
| 1 | VDD | Teensy 3.3V |
| 2 | GND | Ground rail |
| 3 | SCLK | Teensy 13 |
| 4 | MISO | Teensy 12 |
| 5 | MOSI | Teensy 11 |
| 6 | CS | Teensy 14/15/16 |
| 7 | INT | NC |
| 8 | PD | Teensy 17/20/21 |
| 9-16 | NC | — |
| 17 | BL+ | Buck 5V |
| 18 | BL+ | Buck 5V |
| 19 | BL- | Buck GND |
| 20 | BL- | Buck GND |

---

## Panel timing/config profiles needed in firmware

| Panel | EVE_config profile | Resolution | HCYCLE/HOFFSET/VCYCLE/VOFFSET |
|---|---|---|---|
| Center | RVT70 / EVE_RiTFT70-equivalent | 1024x600 | HCYCLE 1344, HOFFSET 160, VCYCLE 635, VOFFSET 23 (per Riverdi datasheet; confirm against current library's built-in profile if present) |
| Left | RVT50-equivalent | 800x480 | Standard EVE4 5" 800x480 values (confirm against library's built-in profile) |
| Right | RVT50-equivalent | 800x480 | Same as Left |

Each panel needs its own EVE device context/instance in firmware (own CS, own
PD, own resolution config, own RAM_G — these do NOT share memory, each BT817 is
independent silicon with its own 1 MiB RAM_G and its own onboard QSPI flash).

---

## RAM_G / flash architecture decision (CENTER PANEL ONLY — see boot sequence below)

- Each panel has an onboard QSPI flash chip (confirmed populated on the
  RVT70HSBNWN00 / RVT50HQBNWN00 family per Riverdi datasheets).
- **Only the CENTER 7" panel plays splash animations at boot.** Left and right
  5" panels have NO splash content and do not need flash programming for boot
  assets. Do not run the flash-programming step on the eval board for the two
  5" panels — only the center panel's flash gets the 3x ~2 second animation
  assets written to it.
- Center panel: splash/boot animations (3x ~2 second animations) must be
  STREAMED FROM FLASH, not held resident in RAM_G. Do not attempt to preload
  full animation frame sets into RAM_G — 1 MiB will not hold multiple
  animations plus fonts simultaneously.
- All three panels: dash fonts (gauge numerals, labels) should be preloaded
  into RAM_G at boot and left resident for the life of the session (via the
  library's pre-upload functions, e.g. lv_draw_eve_pre_upload_font_range or the
  native EVE equivalent CMD_LOADIMAGE/CMD_INFLATE at boot). This applies to all
  three panels equally, including left/right which have no splash content.
- Because fonts stay resident in RAM_G on the center panel while animations
  stream from flash, the center panel's boot handoff CAN crossfade from splash
  to live dash with no black frame. The originally-logged "R17 deviation"
  (forced black frame between splash and dash due to assumed RAM_G conflict) is
  NOT necessary and should be removed/reverted if implemented — confirm current
  repo state against this.
- Flash must be programmed once for the physical center panel (not per
  firmware build) via the Riverdi SM-STM32 eval board + EVE Screen Editor /
  flash utility, using the Direct USB (FTDI bridge) path, with the 6-position
  RiBus master-select jumper block set to Direct USB (NOT STM32) during
  programming.

## Boot sequence across all three panels (synchronized)

1. Power-up: all three panels init (own CS/PD, own resolution config), fonts
   preloaded into RAM_G on all three. Left/right hold at black/idle — no
   content drawn yet.
2. Center panel plays its 3x splash animations, streamed from its flash.
   Left/right remain black/idle during this entire sequence.
3. When the center panel's animation sequence completes, fire a single shared
   "boot complete" event/flag in firmware.
4. On that event, ALL THREE panels fade in together into their live dash
   content (center's gauges, left's gauges, right's gauges) at the same time.
   Left/right have no crossfade-from-splash to manage since they had no splash
   — they simply fade in from black on the shared trigger.
5. Confirm in the repo that this synchronization is a single shared
   state/flag/timer, not three independently-timed fades that could drift
   apart and look uncoordinated.

---

## Backlight dimming — SINGLE UNIFIED CONTROL ACROSS ALL THREE PANELS

**Brightness is one setting for the whole cluster, not three independent ones.**
There is no per-panel brightness control anywhere in the UI, firmware API, or
CAN interface. One value in, all three panels move together, always.

- No PWM hardware on the board/bench setup. Dimming is done entirely by writing
  REG_PWM_DUTY (0-128) over SPI to each panel's own BT817 individually — this
  is a per-chip register, there is no shared hardware brightness line — but the
  VALUE written must be the same on all three every time it changes.
- REG_PWM_HZ should be set once at init per panel (10-100kHz range for both the
  7" and 5" sizes per Riverdi datasheets) and never touched again.
- Architect this as ONE firmware-level brightness variable/state (e.g. a single
  `dash_brightness` 0-128 value) that a single setter function writes to
  REG_PWM_DUTY on all three panel contexts in the same call. Do not implement
  three separate brightness variables or three separate setter paths, even if
  they happen to be driven by the same source today — that invites drift later
  (one panel's write failing silently while the others succeed, a future dev
  adding a per-panel override, etc.).
- Telltale LED PWM (Stage 5, physical LEDs, separate from the screens) should
  also read from this same single brightness value once implemented, so the
  whole physical cluster — three screens plus telltales — dims as one unit for
  day/night, driven eventually by one CAN "dash dimmer" signal.
- Confirm in the repo: search for REG_PWM_DUTY writes and verify there is
  exactly one code path that sets brightness, called once per panel per update
  with an identical value, not three independent brightness states that could
  diverge.

---

## Known-good reference points for debugging

- REG_ID should read 0x7C on a healthy, correctly-clocked BT817 — check this
  first if any panel is silent on SPI.
- If MISO shows no response, check SCLK and MOSI continuity FIRST — a bench
  bring-up bug on the center panel traced to cold solder joints on SCLK (pad 3)
  and MOSI (pad 5) on the breakout header, which produced exactly this symptom
  (init fails silently, no register reads succeed) despite MISO itself being
  wired correctly. Do not assume MISO is the fault just because it's silent.
- SPI clock: keep at or below ~11 MHz during EVE_init(); library should raise
  to a higher rate (up to 30 MHz rated on these panels) for normal draw
  operations after successful init. Confirm current repo code actually does
  this speed step-up post-init rather than staying conservative throughout.

---

## What to check in the repo against this document

1. Pin definitions (EVE_target.h or equivalent) match the Teensy pin table above
   exactly for all three panel contexts — no swapped CS/PD pairs.
2. Three independent EVE device contexts exist, each with correct resolution
   config, and CS assertion is mutually exclusive across all three at any given
   moment (no code path that could assert two CS lines simultaneously).
3. RAM_G usage stays within budget: center panel needs fonts-resident +
   one-animation-frame-at-a-time streaming from flash; left/right panels only
   need fonts-resident (no animation content at all). Flag any code that tries
   to hold multiple full animation frame sets in RAM_G at once, or that
   attempts to program/stream splash content to the left or right panels.
3b. Confirm boot synchronization is a single shared flag/event firing all three
    fade-ins together when the center panel's splash sequence ends — not three
    independently timed sequences.
4. Backlight init sets REG_PWM_HZ once and modulates only REG_PWM_DUTY
   thereafter, per panel — and confirm all three panels are always written
   the SAME REG_PWM_DUTY value from a single shared brightness state, never
   three independent values.
5. SPI speed is conservative during init and raised after, per panel.
6. No hardcoded assumption that all three panels share one resolution/timing
   profile — center is 1024x600, both sides are 800x480.
