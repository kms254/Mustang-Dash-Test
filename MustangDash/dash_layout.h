/*
 * dash_layout.h - the mock->panel scale system and shared gauge geometry.
 *
 * The dash was designed on a 620x400 mock canvas; the panel is 1024x600
 * (plan KTD9). This header owns ONLY the scale macros and the gauge
 * geometry fractions shared by every gauge; detailed per-element positions
 * belong to the renderer units.
 *
 * Which macro to use:
 *   DASH_LX(v) - x positions and horizontal rect extents (x1.6516, rounded)
 *   DASH_LY(v) - y positions and vertical rect extents   (x1.5,    rounded)
 *   DASH_LR(v) - radii, diameters, stroke widths, needle lengths: scale by
 *                the Y factor (1.5) ONLY, so circles stay circular instead
 *                of stretching into ellipses.
 *
 * All three are plain integer macros (round-half-up) so they are usable in
 * constant expressions / static initializers -- no lroundf at runtime.
 *
 * The two 5" side screens are a separate mock canvas: 420x320, scaled to
 * their own native 800x480 panel (assets/dash-design/README.md's "Screen
 * Resolutions & Canvas" section: "the HTML mock draws ... the 5" at
 * 420x320 -- scale all dimensions in this document by ... x1.90 (5" width)
 * to native"). Same three-macro shape, same round-half-up convention:
 *   DASH5_LX(v) - x positions/extents on a 5" panel (x800/420, rounded)
 *   DASH5_LY(v) - y positions/extents on a 5" panel (x1.5,     rounded)
 *   DASH5_LR(v) - radii/diameters/strokes on a 5" panel: Y factor (1.5)
 *                 ONLY, same rationale as DASH_LR.
 */

#ifndef DASH_LAYOUT_H
#define DASH_LAYOUT_H

#include <stdint.h>

/* panel size */
#define DASH_W 1024
#define DASH_H  600

/* mock canvas 620x400 -> panel 1024x600, integer rounding */
#define DASH_LX(v) ((int16_t)(((v) * 1024 + 310) / 620))
#define DASH_LY(v) ((int16_t)(((v) * 600 + 200) / 400))
#define DASH_LR(v) ((int16_t)(((v) * 3 + 1) / 2))

/* 5" side-panel size */
#define DASH5_W 800
#define DASH5_H 480

/* mock canvas 420x320 -> 5" panel 800x480, integer rounding */
#define DASH5_LX(v) ((int16_t)(((v) * 800 + 210) / 420))
#define DASH5_LY(v) ((int16_t)(((v) * 480 + 160) / 320))
#define DASH5_LR(v) ((int16_t)(((v) * 3 + 1) / 2))

/* Shared gauge geometry as fractions of the arc radius (design spec uses
 * viewBox radius 88): multiply by a gauge's scaled radius to place ticks,
 * tick labels, the needle tip, and the hub. */
#define DASH_GAUGE_TICK_OUT_FRAC (88.0f / 88.0f)
#define DASH_GAUGE_TICK_IN_FRAC  (80.0f / 88.0f)
#define DASH_GAUGE_LABEL_R_FRAC  (66.0f / 88.0f)
#define DASH_GAUGE_NEEDLE_R_FRAC (62.0f / 88.0f)
#define DASH_GAUGE_HUB_R_FRAC    ( 7.0f / 88.0f)

#endif /* DASH_LAYOUT_H */
