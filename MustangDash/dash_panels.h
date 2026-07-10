/*
 * dash_panels.h - static descriptor table for the three BT817 panels driven
 * by one Teensy 4.1 (center RVT70H + left/right RVT50H side panels, plan
 * U1 of docs/plans/2026-07-10-002-feat-side-panels-plan.md).
 *
 * Host-testable in tests/test_dash_panels.c before any wiring or EVE_init()
 * call touches real silicon -- the dash_math.h / splash_timeline.h pattern.
 * No Arduino or EVE library dependencies; stdint only, so this compiles on
 * host gcc as well as the Teensy toolchain.
 *
 * Values are transcribed from libraries/FT800-FT813/src/EVE_config.h:
 *   - CENTER timings: the EVE_RVT70H block (bench-proven; REG_ID == 0x7C,
 *     first light confirmed on this exact 1024x600 SM-RVT70HSBNWN00 panel).
 *   - LEFT/RIGHT timings: the EVE_RVT50H block, which upstream marks
 *     "untested". Until the side panels are bench-verified, the Riverdi
 *     RVT50HQBNWN00 datasheet is the tiebreaker authority for those two
 *     rows, NOT EVE_config.h -- if bring-up forces a timing deviation from
 *     the library's block, update dash_panels.h and tests/test_dash_panels.c
 *     together and document the deviation in both places.
 */

#ifndef DASH_PANELS_H
#define DASH_PANELS_H

#include <stdint.h>

typedef enum {
    DASH_PANEL_CENTER = 0,
    DASH_PANEL_LEFT,
    DASH_PANEL_RIGHT,
    DASH_PANEL_COUNT
} DashPanelRole;

/* Pixel clock is expressed one of two mutually exclusive ways, matching the
 * two forms EVE_config.h itself uses per profile:
 *   - pclk_div:  written to REG_PCLK directly (simple integer divider of the
 *                system clock). Nonzero means "use this field".
 *   - pclk_freq: written to REG_PCLK_FREQ, with REG_PCLK forced to 1 (EVE4's
 *                fractional-PLL path, used when a divider can't hit the
 *                panel's exact pixel rate). Nonzero means "use this field".
 * Exactly one of the two fields is nonzero per row; the other is 0 (unused).
 * Real divider and REG_PCLK_FREQ values are never 0, so 0 is an unambiguous
 * "not this form" sentinel -- no separate tag enum needed.
 */
typedef struct {
    DashPanelRole role;
    uint8_t  cs_pin;
    uint8_t  pd_pin;
    uint16_t width;
    uint16_t height;

    /* full REG_H.. / REG_V.. timing set, EVE_config.h naming */
    uint16_t hcycle;
    uint16_t hoffset;
    uint16_t hsync0;
    uint16_t hsync1;
    uint16_t vcycle;
    uint16_t voffset;
    uint16_t vsync0;
    uint16_t vsync1;
    uint8_t  swizzle;
    uint8_t  pclkpol;
    uint8_t  cspread;

    /* pixel-clock discriminant -- see comment above */
    uint8_t  pclk_div;   /* REG_PCLK divider form; 0 = unused */
    uint16_t pclk_freq;  /* REG_PCLK_FREQ form (REG_PCLK=1); 0 = unused */
} DashPanelDesc;

static const DashPanelDesc DASH_PANELS[DASH_PANEL_COUNT] = {
    /* CENTER: RVT70HSBNWN00 1024x600, EVE_config.h EVE_RVT70H block
     * (bench-proven, see CLAUDE.md "Verified state"). */
    {
        .role     = DASH_PANEL_CENTER,
        .cs_pin   = 14,
        .pd_pin   = 17,
        .width    = 1024,
        .height   = 600,
        .hcycle   = 1344,
        .hoffset  = 160,
        .hsync0   = 0,
        .hsync1   = 70,
        .vcycle   = 635,
        .voffset  = 23,
        .vsync0   = 0,
        .vsync1   = 10,
        .swizzle  = 0,
        .pclkpol  = 1,
        .cspread  = 0,
        .pclk_div = 0,
        .pclk_freq = 0x0D12, /* 51 MHz EXTSYNC form; REG_PCLK forced to 1 */
    },
    /* LEFT: RVT50HQBNWN00 800x480, EVE_config.h EVE_RVT50H block ("untested"
     * upstream -- Riverdi RVT50HQBNWN00 datasheet is the tiebreaker if bench
     * bring-up forces a deviation). */
    {
        .role     = DASH_PANEL_LEFT,
        .cs_pin   = 15,
        .pd_pin   = 20,
        .width    = 800,
        .height   = 480,
        .hcycle   = 816,
        .hoffset  = 8,
        .hsync0   = 0,
        .hsync1   = 4,
        .vcycle   = 496,
        .voffset  = 8,
        .vsync0   = 0,
        .vsync1   = 4,
        .swizzle  = 0,
        .pclkpol  = 1,
        .cspread  = 0,
        .pclk_div = 3,
        .pclk_freq = 0,
    },
    /* RIGHT: same panel model and timings as LEFT, distinct CS/PD only. */
    {
        .role     = DASH_PANEL_RIGHT,
        .cs_pin   = 16,
        .pd_pin   = 21,
        .width    = 800,
        .height   = 480,
        .hcycle   = 816,
        .hoffset  = 8,
        .hsync0   = 0,
        .hsync1   = 4,
        .vcycle   = 496,
        .voffset  = 8,
        .vsync0   = 0,
        .vsync1   = 4,
        .swizzle  = 0,
        .pclkpol  = 1,
        .cspread  = 0,
        .pclk_div = 3,
        .pclk_freq = 0,
    },
};

#endif /* DASH_PANELS_H */
