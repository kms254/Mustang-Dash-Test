/*
 * timing_render.h - EVE display-list renderers for the RIGHT 5" screen
 * (800x480): TRACK = "TIMING" (lap/position, LAST/BEST/PRED lap times,
 * throttle/brake percent bars, FUEL/LAPS/AMB grid, odometer footer) and
 * STREET = "ROAD" (fuel sweep gauge + TRIP A / RANGE / AMB / TIME row),
 * per assets/dash-design/README.md ("RIGHT 5in - TIMING / ROAD").
 *
 * NOT a pure header: everything here drives the EVE co-processor and reads
 * the sketch's file-scope state, so it is not host-testable and is not part
 * of tests/run-tests.sh. Include exactly once, from MustangDash.ino, after
 * dash_draw.h and dash_render.h (shared primitives, DF_* indices, COLOR_*)
 * and the glue prototypes (dash_font, g_dash, g_odo) that the code below
 * uses.
 *
 * Layout: the side screens' own mock canvas (420x320) scaled through
 * DASH5_LX/LY (positions, extents) and DASH5_LR (radii, strokes) per the
 * dash_layout.h convention. Invalid channels follow the dead-front rule
 * (R7): text renders "--", bar fills stay empty, the fuel needle parks at
 * rest -- never a false value.
 */
#ifndef TIMING_RENDER_H
#define TIMING_RENDER_H

/* The 9th font instance ("lap": Saira Condensed 42 px, glyphs
 * "0123456789:.-P") is index 8 in DASH_FONTS; the DF_ enum in dash_render.h
 * was deliberately not extended, so this header names the index itself. It
 * is also the ONLY Saira instance carrying 'P', so the POS readout ("P3")
 * renders with it too -- DF_MID has no 'P' glyph. */
#define TF_LAP 8

/* Font instances this screen references (dash_register_fonts bitmask):
 * registering only these keeps the unused instances' CMD_SETFONT2 words
 * out of the right panel's per-frame display list. */
#define TIMING_FONTS ((uint16_t)((1U << DF_LABEL) | (1U << DF_TINY) \
                               | (1U << DF_VAL) | (1U << DF_SMALL) \
                               | (1U << DF_MID) | (1U << TF_LAP)))

/* Header-title gray from the design mock (#9aa6b1) -- no shared COLOR_
 * token exists for it, so it stays local to this screen. */
#define TIMING_COLOR_TITLE 0x9AA6B1UL

/* Alpha pre-scaled by the crossfade; each render header carries its own
 * scoped copy of this one-liner (define/#undef pair) so the macro never
 * leaks across headers -- the splash SPLASH_A lesson. */
#define DA(a) ((uint8_t)(((uint16_t)(a) * (uint16_t)alpha) / 255U))

/* Header row, both modes: title left, "RACECAPTURE / 5"" source tag right.
 * (The mock's middle dot and double-prime are not in the Chakra charsets;
 * '/' and '"' are the closest available glyphs.) */
static void timing_header(const char *title, uint8_t alpha)
{
    dash_color(TIMING_COLOR_TITLE, alpha);
    EVE_cmd_text(DASH5_LX(26), DASH5_LY(20), dash_font(DF_LABEL), 0U, title);
    dash_color(COLOR_FAINT, alpha);
    EVE_cmd_text(DASH5_LX(394), DASH5_LY(24), dash_font(DF_TINY), EVE_OPT_RIGHTX,
                 "RACECAPTURE / 5\"");
}

/* A full-width horizontal hairline at mock y. */
static void timing_hairline(int16_t mock_y, uint8_t alpha)
{
    dash_color(COLOR_HAIRLINE, alpha);
    EVE_cmd_dl(DL_LINE_WIDTH | 8UL);
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH5_LX(26) * 16), (int16_t)(DASH5_LY(mock_y) * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH5_LX(394) * 16), (int16_t)(DASH5_LY(mock_y) * 16)));
    EVE_cmd_dl(DL_END);
}

/* THROTTLE / BRAKE percent bar: label left, live value right, 7 px pill
 * beneath (the center RPM-bar pattern: dark track, colored fill, and the
 * mock's soft fill glow as a slightly taller pill at DA(60)). Invalid
 * channel: "--" value, empty fill. */
static void timing_pct_bar(const char *label, int16_t label_y, int16_t bar_y,
                           uint8_t ch, uint32_t fill_col, uint32_t text_col,
                           uint8_t alpha)
{
    char buf[16];
    const bool ok = dash_ch_valid(&g_dash, ch);
    const float pct = dash_ch_get(&g_dash, ch);
    const int16_t bar_x = DASH5_LX(26);
    const int16_t bar_w = (int16_t)(DASH5_LX(394) - bar_x);
    const int16_t bar_h = DASH5_LR(7);

    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text(bar_x, DASH5_LY(label_y), dash_font(DF_TINY), 0U, label);
    dash_color(ok ? text_col : COLOR_VALUE, alpha);
    dash_fmt_value(buf, sizeof(buf), pct, 0U, ok);
    EVE_cmd_text(DASH5_LX(394), (int16_t)(DASH5_LY(label_y) - DASH5_LR(4)),
                 dash_font(DF_VAL), EVE_OPT_RIGHTX, buf);

    dash_color(COLOR_BAR_TRACK, alpha);
    draw_pill(bar_x, DASH5_LY(bar_y), bar_w, bar_h);
    if (ok && (pct > 0.0f))
    {
        const float frac = dash_clampf(pct / 100.0f, 0.0f, 1.0f);
        const int16_t fw = (int16_t)((float)bar_w * frac);
        const int16_t glow_h = (int16_t)(bar_h + DASH5_LR(4));
        if (fw > glow_h) /* glow pill is the tallest; gate on it */
        {
            dash_color(fill_col, DA(60)); /* the mock's fill glow */
            draw_pill(bar_x, (int16_t)(DASH5_LY(bar_y) - DASH5_LR(2)), fw, glow_h);
        }
        if (fw > bar_h)
        {
            dash_color(fill_col, alpha);
            draw_pill(bar_x, DASH5_LY(bar_y), fw, bar_h);
        }
    }
}

/* One bottom-grid cell: tiny label at the cell's left pad, DF_VAL value
 * right-aligned at the cell's right pad. */
static void timing_grid_cell(int16_t x0, int16_t x1, uint32_t val_col,
                             const char *label, const char *value, uint8_t alpha)
{
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text((int16_t)(DASH5_LX(x0) + DASH5_LX(9)), DASH5_LY(212),
                 dash_font(DF_TINY), 0U, label);
    dash_color(val_col, alpha);
    EVE_cmd_text((int16_t)(DASH5_LX(x1) - DASH5_LX(9)), DASH5_LY(204),
                 dash_font(DF_VAL), EVE_OPT_RIGHTX, value);
}

/* Odometer footer, mirroring the center screens' treatment: hairline, faint
 * "ODOMETER" label left, dash_odo_miles value + faint "MI" unit right. */
static void timing_odo_footer(uint8_t alpha)
{
    char buf[24];
    timing_hairline(254, alpha);
    dash_color(COLOR_FAINT, alpha);
    EVE_cmd_text(DASH5_LX(26), DASH5_LY(266), dash_font(DF_TINY), 0U, "ODOMETER");
    dash_color(COLOR_ODO, alpha);
    snprintf(buf, sizeof(buf), "%.1f", (double)dash_odo_miles(&g_odo));
    EVE_cmd_text(DASH5_LX(368), DASH5_LY(262), dash_font(DF_SMALL), EVE_OPT_RIGHTX, buf);
    dash_color(COLOR_FAINT, alpha);
    EVE_cmd_text(DASH5_LX(374), DASH5_LY(266), dash_font(DF_TINY), 0U, "MI");
}

/* ---- TRACK: "TIMING" ---- */
void timing_track_screen(uint8_t alpha)
{
    char buf[24];
    char lap[16];

    timing_header("TIMING", alpha);

    /* LAP number (zero-padded like the mock) + gold position "P<n>" */
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text(DASH5_LX(26), DASH5_LY(50), dash_font(DF_LABEL), 0U, "LAP");
    dash_color(COLOR_VALUE, alpha);
    if (dash_ch_valid(&g_dash, DASH_CH_LAPN))
    {
        snprintf(buf, sizeof(buf), "%02u", (unsigned)g_dash.ch.lap_n);
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }
    EVE_cmd_text(DASH5_LX(64), DASH5_LY(42), dash_font(TF_LAP), 0U, buf);

    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text(DASH5_LX(344), DASH5_LY(54), dash_font(DF_TINY), EVE_OPT_RIGHTX, "POS");
    dash_color(COLOR_ACCENT, alpha);
    if (dash_ch_valid(&g_dash, DASH_CH_POS))
    {
        snprintf(buf, sizeof(buf), "P%u", (unsigned)g_dash.ch.pos);
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }
    EVE_cmd_text(DASH5_LX(394), DASH5_LY(42), dash_font(TF_LAP), EVE_OPT_RIGHTX, buf);

    /* LAST / BEST / PRED row, hairline-framed; BEST in purple, PRED colored
     * by the live delta sign (mock behavior) when the delta is valid */
    timing_hairline(74, alpha);
    timing_hairline(112, alpha);

    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text(DASH5_LX(26), DASH5_LY(80), dash_font(DF_TINY), 0U, "LAST");
    EVE_cmd_text(DASH5_LX(210), DASH5_LY(80), dash_font(DF_TINY), EVE_OPT_CENTERX, "BEST");
    EVE_cmd_text(DASH5_LX(394), DASH5_LY(80), dash_font(DF_TINY), EVE_OPT_RIGHTX, "PRED");

    dash_color(COLOR_VALUE_DIM, alpha);
    dash_fmt_lap(g_dash.ch.last_ms, dash_ch_valid(&g_dash, DASH_CH_LAST), lap);
    EVE_cmd_text(DASH5_LX(26), DASH5_LY(90), dash_font(DF_VAL), 0U, lap);
    dash_color(COLOR_BEST, alpha);
    dash_fmt_lap(g_dash.ch.best_ms, dash_ch_valid(&g_dash, DASH_CH_BEST), lap);
    EVE_cmd_text(DASH5_LX(210), DASH5_LY(90), dash_font(DF_VAL), EVE_OPT_CENTERX, lap);
    const bool delta_ok = dash_ch_valid(&g_dash, DASH_CH_DELTA);
    dash_color(!delta_ok ? COLOR_VALUE_DIM :
               (g_dash.ch.delta_s <= 0.0f) ? COLOR_GREEN : COLOR_RED_TEXT, alpha);
    dash_fmt_lap(g_dash.ch.pred_ms, dash_ch_valid(&g_dash, DASH_CH_PRED), lap);
    EVE_cmd_text(DASH5_LX(394), DASH5_LY(90), dash_font(DF_VAL), EVE_OPT_RIGHTX, lap);

    /* throttle / brake percent bars (values are bare numbers: no '%' glyph
     * in the Saira instances) */
    timing_pct_bar("THROTTLE", 126, 138, DASH_CH_THROTTLE, COLOR_GREEN,
                   COLOR_GREEN, alpha);
    timing_pct_bar("BRAKE", 156, 168, DASH_CH_BRAKE, COLOR_RED_FILL,
                   COLOR_RED_TEXT, alpha);

    /* bottom hairline grid: FUEL gal / LAPS remaining / AMB F */
    timing_hairline(194, alpha);
    timing_hairline(240, alpha);
    dash_color(COLOR_HAIRLINE, alpha);
    EVE_cmd_dl(DL_LINE_WIDTH | 8UL);
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH5_LX(149) * 16), (int16_t)(DASH5_LY(194) * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH5_LX(149) * 16), (int16_t)(DASH5_LY(240) * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH5_LX(271) * 16), (int16_t)(DASH5_LY(194) * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH5_LX(271) * 16), (int16_t)(DASH5_LY(240) * 16)));
    EVE_cmd_dl(DL_END);

    const bool fuel_ok = dash_ch_valid(&g_dash, DASH_CH_FUEL);
    dash_fmt_value(buf, sizeof(buf), g_dash.ch.fuel_gal, 1U, fuel_ok);
    timing_grid_cell(26, 122,
                     (fuel_ok && (DASH_COLOR_AMBER == dash_fuel_state(g_dash.ch.fuel_gal)))
                         ? COLOR_AMBER : COLOR_VALUE,
                     "FUEL", buf, alpha);
    dash_color(COLOR_FAINT, alpha); /* small GAL unit after the value */
    EVE_cmd_text(DASH5_LX(122), DASH5_LY(212), dash_font(DF_TINY), 0U, "GAL");

    float laps;
    if (dash_laps_remaining(g_dash.ch.fuel_gal, fuel_ok, &laps))
    {
        snprintf(buf, sizeof(buf), "%d", (int)laps); /* mock floors it */
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }
    timing_grid_cell(149, 271, COLOR_VALUE, "LAPS", buf, alpha);

    dash_fmt_value(buf, sizeof(buf), g_dash.ch.ambient_f, 0U,
                   dash_ch_valid(&g_dash, DASH_CH_AMBIENT));
    timing_grid_cell(271, 394, COLOR_VALUE, "AMB", buf, alpha);

    timing_odo_footer(alpha);
}

/* ---- STREET: "ROAD" ---- */

/* FUEL sweep gauge (mock 190x170 box, shared 240-degree gauge geometry):
 * gold value arc 0..fuel/16 (amber below 2.5 gal), E/F end labels at the
 * gauge label radius, needle + hub, hub gal readout. Invalid fuel: no arc,
 * parked dead-front needle, "--" readout. */
static void road_fuel_gauge(uint8_t alpha)
{
    char buf[16];
    const GaugeSpec g = { (float)DASH5_LX(210), (float)DASH5_LY(150),
                          (float)DASH5_LR(88), DASH5_LR(10) };
    const bool ok = dash_ch_valid(&g_dash, DASH_CH_FUEL);
    const float gal = g_dash.ch.fuel_gal;
    /* README: 0-16 gal usable scale */
    const float frac = dash_clampf(ok ? (gal / 16.0f) : 0.0f, 0.0f, 1.0f);
    const bool amber = ok && (DASH_COLOR_AMBER == dash_fuel_state(gal));

    draw_gauge_chrome(&g, alpha);

    /* E / F end labels at the arc's label radius, fractions 0 and 1 */
    float lx;
    float ly;
    dash_color(COLOR_LABEL, alpha);
    dash_arc_point(g.cx, g.cy, g.r * DASH_GAUGE_LABEL_R_FRAC, 0.0f, &lx, &ly);
    EVE_cmd_text((int16_t)lx, (int16_t)ly, dash_font(DF_TINY), EVE_OPT_CENTER, "E");
    dash_arc_point(g.cx, g.cy, g.r * DASH_GAUGE_LABEL_R_FRAC, 1.0f, &lx, &ly);
    EVE_cmd_text((int16_t)lx, (int16_t)ly, dash_font(DF_TINY), EVE_OPT_CENTER, "F");

    if (ok && (frac > 0.005f))
    {
        dash_color(amber ? COLOR_AMBER : COLOR_ACCENT, alpha);
        draw_arc(g.cx, g.cy, g.r, 0.0f, frac, g.stroke);
    }

    draw_gauge_needle_hub(&g, frac, ok, alpha);

    /* hub readout: gallons, 1 decimal, amber when low */
    dash_color(amber ? COLOR_AMBER : COLOR_VALUE, alpha);
    dash_fmt_value(buf, sizeof(buf), gal, 1U, ok);
    EVE_cmd_text((int16_t)g.cx, (int16_t)(g.cy + (float)DASH5_LR(34)),
                 dash_font(DF_MID), EVE_OPT_CENTER, buf);
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text((int16_t)g.cx, (int16_t)(g.cy + (float)DASH5_LR(50)),
                 dash_font(DF_TINY), EVE_OPT_CENTERX, "GAL");
}

void timing_street_screen(uint8_t alpha)
{
    char buf[24];

    timing_header("ROAD", alpha);
    road_fuel_gauge(alpha);

    /* bottom row: TRIP A / RANGE / AMB / TIME. Units the Saira value fonts
     * cannot spell ride in the Chakra labels ("RANGE MI"); the mock's
     * degree sign has no glyph, so AMB is the bare integer. */
    timing_hairline(266, alpha);

    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text(DASH5_LX(26), DASH5_LY(274), dash_font(DF_TINY), 0U, "TRIP A");
    EVE_cmd_text(DASH5_LX(163), DASH5_LY(274), dash_font(DF_TINY), EVE_OPT_CENTERX, "RANGE MI");
    EVE_cmd_text(DASH5_LX(257), DASH5_LY(274), dash_font(DF_TINY), EVE_OPT_CENTERX, "AMB");
    EVE_cmd_text(DASH5_LX(394), DASH5_LY(274), dash_font(DF_TINY), EVE_OPT_RIGHTX, "TIME");

    dash_color(COLOR_VALUE_DIM, alpha);
    snprintf(buf, sizeof(buf), "%.1f", (double)dash_trip_miles(&g_odo));
    EVE_cmd_text(DASH5_LX(26), DASH5_LY(284), dash_font(DF_VAL), 0U, buf);

    float range;
    const bool range_ok = dash_range_mi(g_dash.ch.fuel_gal,
                                        dash_ch_valid(&g_dash, DASH_CH_FUEL), &range);
    dash_fmt_value(buf, sizeof(buf), range, 0U, range_ok);
    EVE_cmd_text(DASH5_LX(163), DASH5_LY(284), dash_font(DF_VAL), EVE_OPT_CENTERX, buf);

    dash_fmt_value(buf, sizeof(buf), g_dash.ch.ambient_f, 0U,
                   dash_ch_valid(&g_dash, DASH_CH_AMBIENT));
    EVE_cmd_text(DASH5_LX(257), DASH5_LY(284), dash_font(DF_VAL), EVE_OPT_CENTERX, buf);

    /* TIME: 12-hour h:mm (the mock's clock convention) + tiny AM/PM */
    if (dash_ch_valid(&g_dash, DASH_CH_TIME))
    {
        const uint32_t m = g_dash.ch.time_min % 1440U;
        uint32_t h12 = (m / 60U) % 12U;
        if (0U == h12) { h12 = 12U; }
        snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)h12, (unsigned)(m % 60U));
        EVE_cmd_text(DASH5_LX(374), DASH5_LY(284), dash_font(DF_VAL), EVE_OPT_RIGHTX, buf);
        dash_color(COLOR_LABEL, alpha);
        EVE_cmd_text(DASH5_LX(378), DASH5_LY(290), dash_font(DF_TINY), 0U,
                     ((m / 60U) < 12U) ? "AM" : "PM");
    }
    else
    {
        EVE_cmd_text(DASH5_LX(394), DASH5_LY(284), dash_font(DF_VAL), EVE_OPT_RIGHTX, "--");
    }
}

/* DA is this header's private alpha-scaling helper; end its scope here so it
 * cannot silently capture an `alpha` parameter in later includes (mirrors
 * dash_render.h's convention). */
#undef DA

#endif /* TIMING_RENDER_H */
