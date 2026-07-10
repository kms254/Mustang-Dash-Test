# HANDOFF — Boot splash for 7" EVE4 dash display

## Task
Implement the 2-second animated boot splash described in `README.md` (same folder), then hold the final frame until the main dash UI takes over.

## Hardware context
- Display: 7" EVE4 module, BT817 controller, 1024×600, SPI interface
- Host MCU: **TODO — fill in (e.g. STM32F4, ESP32, Arduino Mega + shield)**
- EVE library: **TODO — fill in (e.g. Bridgetek EVE_HAL, Rudolph Riedel FT800-FT813, Matrix Orbital EVE lib)**
- Theme chosen: **TODO — blue / red / checkered**

## What's in this folder
- `README.md` — the full spec: asset layout table (final x/y positions), 2000 ms timeline with per-element start/end times, slide distances, easing formulas, and EVE-specific notes (`COLOR_A` for fades, `CMD_LOADIDENTITY`/`CMD_SCALE` for the emblem pop).
- 14 PNG assets at exact final pixel sizes. Transparent assets (emblem, bars, wordmark, lines, year) need an alpha-capable format (ARGB4 or COMPRESSED_RGBA_ASTC_8x8_KHR); backgrounds can be RGB565 or loaded as JPEG.

## Implementation notes
1. Convert assets with EVE Asset Builder and flash them to the module's QSPI flash (or load to RAM_G at boot via CMD_LOADIMAGE for the backgrounds).
2. Render loop: target 50–60 fps; each frame compute per-element progress `t = clamp((now_ms − start) / (end − start), 0, 1)`, apply the easing from README, rebuild the display list, swap.
3. Draw order per frame: background → bars/strips → emblem → wordmark → accent line → year.
4. After 2000 ms, keep drawing the final composition (or hand off to the dash UI's first screen).
5. Only the chosen theme's assets need to be flashed; the others can be ignored.

## Acceptance
- Matches the reference final frame (`../splash-<theme>.png`) pixel-close at 1024×600.
- Animation is smooth (no visible stepping), completes in 2.0 s, no flicker on load.
