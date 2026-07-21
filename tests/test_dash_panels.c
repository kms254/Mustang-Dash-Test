/*
 * Invariant test: the triple-dash panel descriptor table (dash_panels.h)
 * pins the control pins, resolutions, and full display-timing rows for the
 * center (RVT70H) and two side (RVT50H) BT817 panels driven by one
 * Teensy 4.1 (plan U1 of docs/plans/2026-07-10-002-feat-side-panels-plan.md).
 *
 * Authority note: the CENTER row is checked against
 * libraries/FT800-FT813/src/EVE_config.h's EVE_RVT70H block, which is
 * bench-proven (REG_ID == 0x7C, first light confirmed on this exact panel --
 * see CLAUDE.md). The LEFT/RIGHT rows are checked against the library's
 * EVE_RVT50H block too, but that block is marked "untested" upstream, so
 * for the side panels the **Riverdi RVT50HQBNWN00 datasheet is the
 * tiebreaker authority**, not EVE_config.h. If bench bring-up of the side
 * panels forces a timing deviation from the library block, dash_panels.h
 * and this test move together, with the deviation documented in both.
 *
 * Runs on the host:
 *   gcc -std=c11 -I MustangDash tests/test_dash_panels.c -o /tmp/tp && /tmp/tp
 */

#include <stdio.h>
#include <stdint.h>

#include "dash_panels.h"

static int failures = 0;

static void expect(int cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

/* pin reserved -- {0..9, 22..27} (Teensy 4.1 SD/other-peripheral pins in
 * this project's wiring plan) or the SPI trio {11,12,13} (SCLK/MISO/MOSI,
 * shared by all three panels and never available as a CS/PD line). */
static int pin_reserved(uint8_t pin)
{
    if (pin <= 9U) { return 1; }
    if (pin >= 22U && pin <= 27U) { return 1; }
    if (pin == 11U || pin == 12U || pin == 13U) { return 1; }
    return 0;
}

int main(void)
{
    expect(DASH_PANEL_COUNT == 3, "there must be exactly three panel roles");
    expect(sizeof(DASH_PANELS) / sizeof(DASH_PANELS[0]) == 3U,
           "the descriptor table must have exactly three rows");

    const DashPanelDesc *center = &DASH_PANELS[DASH_PANEL_CENTER];
    const DashPanelDesc *left   = &DASH_PANELS[DASH_PANEL_LEFT];
    const DashPanelDesc *right  = &DASH_PANELS[DASH_PANEL_RIGHT];

    /* ---- 1. pins pinned ---- */
    expect(center->cs_pin == 14U, "center CS must stay pin 14");
    expect(center->pd_pin == 17U, "center PD must stay pin 17");
    expect(left->cs_pin == 15U, "left CS must stay pin 15");
    expect(left->pd_pin == 20U, "left PD must stay pin 20");
    expect(right->cs_pin == 16U, "right CS must stay pin 16");
    expect(right->pd_pin == 21U, "right PD must stay pin 21");

    {
        uint8_t pins[6] = {
            center->cs_pin, center->pd_pin,
            left->cs_pin,   left->pd_pin,
            right->cs_pin,  right->pd_pin,
        };
        for (int i = 0; i < 6; i++)
        {
            expect(!pin_reserved(pins[i]),
                   "no panel pin may fall in the reserved 0-9/22-27 or SPI 11-13 sets");
            for (int j = i + 1; j < 6; j++)
            {
                if (pins[i] == pins[j])
                {
                    fprintf(stderr, "FAIL: pins[%d] and pins[%d] collide (both %u)\n",
                            i, j, (unsigned)pins[i]);
                    failures++;
                }
            }
        }
    }

    /* ---- 2. timing rows ---- */

    /* center: EVE_config.h EVE_RVT70H block, bench-proven */
    expect(center->hcycle == 1344U, "center HCYCLE must match EVE_RVT70H (1344)");
    expect(center->hoffset == 160U, "center HOFFSET must match EVE_RVT70H (160)");
    expect(center->hsync0 == 0U, "center HSYNC0 must match EVE_RVT70H (0)");
    expect(center->hsync1 == 70U, "center HSYNC1 must match EVE_RVT70H (70)");
    expect(center->vcycle == 635U, "center VCYCLE must match EVE_RVT70H (635)");
    expect(center->voffset == 23U, "center VOFFSET must match EVE_RVT70H (23)");
    expect(center->vsync0 == 0U, "center VSYNC0 must match EVE_RVT70H (0)");
    expect(center->vsync1 == 10U, "center VSYNC1 must match EVE_RVT70H (10)");
    expect(center->swizzle == 0U, "center SWIZZLE must match EVE_RVT70H (0)");
    expect(center->pclkpol == 1U, "center PCLKPOL must match EVE_RVT70H (1)");
    expect(center->cspread == 0U, "center CSPREAD must match EVE_RVT70H (0)");
    expect(center->pclk_div == 0U, "center must use the pclk_freq form, not pclk_div");
    expect(center->pclk_freq == 0x0D12U,
           "center PCLK_FREQ must match EVE_RVT70H (0x0D12, 51 MHz EXTSYNC)");

    /* sides: EVE_config.h EVE_RVT50H block == Riverdi RVT50HQBNWN00 datasheet
     * tiebreaker (see file header note); left and right must be identical. */
    const DashPanelDesc *sides[2] = { left, right };
    const char *side_name[2] = { "left", "right" };
    for (int i = 0; i < 2; i++)
    {
        const DashPanelDesc *p = sides[i];
        char msg[128];

        snprintf(msg, sizeof msg, "%s HCYCLE must match RVT50HQBNWN00 (816)", side_name[i]);
        expect(p->hcycle == 816U, msg);
        snprintf(msg, sizeof msg, "%s HOFFSET must match RVT50HQBNWN00 (8)", side_name[i]);
        expect(p->hoffset == 8U, msg);
        snprintf(msg, sizeof msg, "%s HSYNC0 must match RVT50HQBNWN00 (0)", side_name[i]);
        expect(p->hsync0 == 0U, msg);
        snprintf(msg, sizeof msg, "%s HSYNC1 must match RVT50HQBNWN00 (4)", side_name[i]);
        expect(p->hsync1 == 4U, msg);
        snprintf(msg, sizeof msg, "%s VCYCLE must match RVT50HQBNWN00 (496)", side_name[i]);
        expect(p->vcycle == 496U, msg);
        snprintf(msg, sizeof msg, "%s VOFFSET must match RVT50HQBNWN00 (8)", side_name[i]);
        expect(p->voffset == 8U, msg);
        snprintf(msg, sizeof msg, "%s VSYNC0 must match RVT50HQBNWN00 (0)", side_name[i]);
        expect(p->vsync0 == 0U, msg);
        snprintf(msg, sizeof msg, "%s VSYNC1 must match RVT50HQBNWN00 (4)", side_name[i]);
        expect(p->vsync1 == 4U, msg);
        snprintf(msg, sizeof msg, "%s SWIZZLE must match RVT50HQBNWN00 (0)", side_name[i]);
        expect(p->swizzle == 0U, msg);
        snprintf(msg, sizeof msg, "%s PCLKPOL must match RVT50HQBNWN00 (1)", side_name[i]);
        expect(p->pclkpol == 1U, msg);
        snprintf(msg, sizeof msg, "%s CSPREAD must match RVT50HQBNWN00 (0)", side_name[i]);
        expect(p->cspread == 0U, msg);
        snprintf(msg, sizeof msg, "%s must use the pclk_div form, not pclk_freq", side_name[i]);
        expect(p->pclk_freq == 0U, msg);
        snprintf(msg, sizeof msg, "%s PCLK divider must match RVT50HQBNWN00 (3)", side_name[i]);
        expect(p->pclk_div == 3U, msg);
    }

    expect(left->hcycle == right->hcycle && left->hoffset == right->hoffset
           && left->hsync0 == right->hsync0 && left->hsync1 == right->hsync1
           && left->vcycle == right->vcycle && left->voffset == right->voffset
           && left->vsync0 == right->vsync0 && left->vsync1 == right->vsync1
           && left->swizzle == right->swizzle && left->pclkpol == right->pclkpol
           && left->cspread == right->cspread && left->pclk_div == right->pclk_div
           && left->pclk_freq == right->pclk_freq,
           "left and right timing rows must be identical (same panel model)");

    /* ---- 3. resolutions and roles ---- */
    expect(center->width == 1024U && center->height == 600U,
           "center resolution must stay 1024x600");
    expect(left->width == 800U && left->height == 480U,
           "left resolution must stay 800x480");
    expect(right->width == 800U && right->height == 480U,
           "right resolution must stay 800x480");
    expect(center->role == DASH_PANEL_CENTER, "center row's role field must be DASH_PANEL_CENTER");
    expect(left->role == DASH_PANEL_LEFT, "left row's role field must be DASH_PANEL_LEFT");
    expect(right->role == DASH_PANEL_RIGHT, "right row's role field must be DASH_PANEL_RIGHT");
    expect(center->role != left->role && left->role != right->role && center->role != right->role,
           "all three roles must be distinct");

    if (failures == 0)
    {
        printf("OK: panel pins, timings, resolutions, and roles hold for "
               "center/left/right\n");
        return 0;
    }
    return 1;
}
