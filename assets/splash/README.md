# Mustang Splash — EVE4 (BT817) firmware animation spec

Target: 7" EVE4, 1024×600. Duration: **2000 ms**, then hold final frame.
Convert PNGs with EVE Asset Builder (ARGB4/ASTC for transparent assets, RGB565/JPEG for backgrounds).

## Final layout (top-left positions, px)

| Asset | File | Final position | Size |
|---|---|---|---|
| Background | `bg-{theme}-1024x600.png` | 0, 0 | 1024×600 |
| Left bars | `bars-chrome-240x45.png` | 138, 202 | 240×45 |
| Right bars | `bars-chrome-240x45.png` | 646, 202 | 240×45 |
| Emblem | `emblem-200x200.png` | 412, 124 (center 512, 224) | 200×200 |
| Wordmark | `wordmark-mustang-700x80.png` | 162, 358 (center x 512) | 700×80 |
| Accent line | `line-{theme}-340x40.png` | 342, 420 (line itself is 300×3 centered in the canvas) | 340×40 |
| Year | `year-1965-{theme}.png` | 412, 456 | 200×28 |

Checkered theme replaces:
- bars → `checker-block-240x52.png` at 138/646, y 198
- accent line → `checker-line-300x14.png` at 362, 440
- plus `checker-strip-1024x26.png` at y 0 (and y 574, flipped or offset by 13 px for alternating start)

## Timeline (ms, within 0–2000)

All easing: **ease-out cubic** — `f(t) = 1 − (1−t)³` — unless noted.

1. **Background** — visible from 0 ms (static).
2. **Bars / checker blocks & strips** — 200 → 840 ms.
   Left: slide X from −150 → 138. Right: slide X from 934 → 646.
   Opacity 0 → 1 over the same window. Top strip slides in with left bar timing, bottom strip with right.
3. **Emblem** — 360 → 1000 ms.
   Scale 0.70 → 1.00 about its center (512, 224) with slight overshoot: use
   `f(t) = 1 + 2.7·(t−1)³ + 1.7·(t−1)²` (ease-out back). Opacity 0 → 1.
   (EVE: `CMD_LOADIDENTITY` + `CMD_TRANSLATE`/`CMD_SCALE` around bitmap center.)
4. **Wordmark + year** — 760 → 1360 ms.
   Slide Y from +26 px below final to final. Opacity 0 → 1 (use `COLOR_A`).
5. **Accent line** — 1040 → 1640 ms.
   Scale X 0 → 1 about center x 512 (or reveal with a scissor rect expanding from center).
6. **1640 → 2000 ms** — hold; splash complete.

Per-element progress: `t = clamp((now − start) / (end − start), 0, 1)`, then apply easing.

## Opacity on EVE
Set `COLOR_A(alpha)` before drawing each bitmap (0–255). Restore to 255 after.

## Static fallback
`../splash-blue.png`, `../splash-red.png`, `../splash-checkered.png` are complete 1024×600 final frames if you want a no-animation boot image.
