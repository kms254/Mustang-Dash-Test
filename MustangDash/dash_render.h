/*
 * dash_render.h - EVE display-list renderers for the dash: TRACK / STREET
 * modes, alarm takeover, the shared gauge/bar drawing helpers, and the
 * frame / DL-measure wrappers. Pure code motion out of MustangDash.ino.
 *
 * NOT a pure header: everything here drives the EVE co-processor and reads
 * the sketch's file-scope state, so it is not host-testable and is not part
 * of tests/run-tests.sh. Include exactly once, from MustangDash.ino, after
 * the shared types and globals (g_dash, g_odo, g_fonts via dash_font(),
 * COLOR_*) and the glue prototypes (dash_font, dash_register_fonts,
 * eve_frame_begin/end) that the code below uses.
 */
#ifndef DASH_RENDER_H
#define DASH_RENDER_H

/* One sweep gauge (KTD2/KTD9): geometry derives from the design's shared
 * viewBox fractions (ticks r80-88, labels r66, needle r62, hub r7). */
struct GaugeSpec
{
    float cx, cy, r; /* panel px */
    int16_t stroke;  /* arc stroke, panel px */
};

struct Telltale
{
    const char *label;
    bool valid;
    bool lit;
    uint32_t color;
};

/* ---- dash rendering ----
 *
 * Everything below draws in plain (non-burst) command mode: at 8 MHz SPI a
 * full STREET frame is a few ms of transfer, comfortably over 30 fps, and
 * plain mode lets the crossfade mix splash + dash in one display list. The
 * _burst path (KTD8) stays available as an optimization if the measured fps
 * ever drops.
 *
 * Layout: mock coordinates (620x400) scaled through DASH_LX/LY (positions,
 * rect extents) and DASH_LR (radii, strokes, font-ish sizes) per KTD9.
 * Invalid channels follow the KTD4 convention: text renders "--", fills
 * render empty, LEDs dark, needles park at rest at dead-front opacity,
 * telltales show the distinct COLOR_NODATA state. */

/* font registry indices (order fixed by tools/make_dash_fonts.py) */
enum
{
    DF_HERO = 0, DF_BIG, DF_MID, DF_VAL, DF_SMALL, DF_TITLE, DF_LABEL, DF_TINY
};

/* COLOR_RGB + COLOR_A in one call; alpha pre-scaled by the crossfade. */
void dash_color(uint32_t rgb, uint8_t alpha)
{
    EVE_color_rgb(rgb);
    EVE_cmd_dl(COLOR_A(alpha));
}

/* Text color for an RPM-driven readout (shared by the TRACK value and the
 * STREET tach hub). */
static uint32_t dash_state_text_color(DashColorState rc)
{
    return (DASH_COLOR_RED == rc) ? COLOR_RED_TEXT :
           (DASH_COLOR_AMBER == rc) ? COLOR_AMBER : COLOR_VALUE;
}

#define DA(a) ((uint8_t)(((uint16_t)(a) * (uint16_t)alpha) / 255U))

/* A rounded-capsule bar: RECTS with LINE_WIDTH as the corner radius. */
static void draw_pill(int16_t x, int16_t y, int16_t w, int16_t h)
{
    const int16_t r = (int16_t)(h / 2);
    EVE_cmd_dl(DL_LINE_WIDTH | (uint32_t)(r * 16));
    EVE_cmd_dl(DL_BEGIN | EVE_RECTS);
    EVE_cmd_dl(VERTEX2F((int16_t)((x + r) * 16), (int16_t)((y + r) * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)((x + w - r) * 16), (int16_t)((y + h - r) * 16)));
    EVE_cmd_dl(DL_END);
}

/* Value arc as a LINE_STRIP polyline, one segment per ~3 degrees, endpoints
 * precomputed on the M7 (no CMD_ARC on BT817). frac0 < frac1 in gauge
 * fractions; stroke_px is the full stroke width. */
static void draw_arc(float cx, float cy, float r, float frac0, float frac1, int16_t stroke_px)
{
    const float sweep = frac1 - frac0;
    if (sweep <= 0.0f)
    {
        return;
    }
    int n = (int)(sweep * 80.0f) + 1; /* 80 segments per full 240 deg sweep */
    if (n < 2) { n = 2; }

    EVE_cmd_dl(DL_LINE_WIDTH | (uint32_t)(stroke_px * 8)); /* half-width in 1/16 px */
    EVE_cmd_dl(DL_BEGIN | EVE_LINE_STRIP);
    for (int i = 0; i <= n; i++)
    {
        float x;
        float y;
        dash_arc_point(cx, cy, r, frac0 + sweep * ((float)i / (float)n), &x, &y);
        EVE_cmd_dl(VERTEX2F((int16_t)(x * 16.0f), (int16_t)(y * 16.0f)));
    }
    EVE_cmd_dl(DL_END);
}

static void draw_gauge_chrome(const GaugeSpec *g, uint8_t alpha)
{
    dash_color(COLOR_ARC_TRACK, alpha);
    draw_arc(g->cx, g->cy, g->r, 0.0f, 1.0f, g->stroke);
}

static void draw_gauge_needle_hub(const GaugeSpec *g, float frac, bool valid, uint8_t alpha)
{
    const uint8_t na = valid ? alpha : DA(70); /* dead-front park when invalid */
    float nx;
    float ny;
    dash_arc_point(g->cx, g->cy, g->r * DASH_GAUGE_NEEDLE_R_FRAC,
                   valid ? frac : 0.0f, &nx, &ny);

    dash_color(COLOR_VALUE, na);
    EVE_cmd_dl(DL_LINE_WIDTH | (uint32_t)(DASH_LR(3) * 8));
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    EVE_cmd_dl(VERTEX2F((int16_t)(g->cx * 16.0f), (int16_t)(g->cy * 16.0f)));
    EVE_cmd_dl(VERTEX2F((int16_t)(nx * 16.0f), (int16_t)(ny * 16.0f)));
    EVE_cmd_dl(DL_END);

    /* hub disc + ring */
    const int16_t hub_r = (int16_t)(g->r * DASH_GAUGE_HUB_R_FRAC);
    dash_color(COLOR_HUB_RING, alpha);
    EVE_cmd_dl(DL_POINT_SIZE | (uint32_t)((hub_r + DASH_LR(2)) * 16));
    EVE_cmd_dl(DL_BEGIN | EVE_POINTS);
    EVE_cmd_dl(VERTEX2F((int16_t)(g->cx * 16.0f), (int16_t)(g->cy * 16.0f)));
    EVE_cmd_dl(DL_END);
    dash_color(COLOR_BAR_TRACK, alpha);
    EVE_cmd_dl(DL_POINT_SIZE | (uint32_t)(hub_r * 16));
    EVE_cmd_dl(DL_BEGIN | EVE_POINTS);
    EVE_cmd_dl(VERTEX2F((int16_t)(g->cx * 16.0f), (int16_t)(g->cy * 16.0f)));
    EVE_cmd_dl(DL_END);
}

static void draw_gauge_ticks(const GaugeSpec *g, const float *fracs, uint8_t count, uint8_t alpha)
{
    dash_color(COLOR_HUB_RING, alpha);
    EVE_cmd_dl(DL_LINE_WIDTH | (uint32_t)(DASH_LR(2) * 8));
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    for (uint8_t i = 0U; i < count; i++)
    {
        float x0, y0, x1, y1;
        dash_arc_point(g->cx, g->cy, g->r * DASH_GAUGE_TICK_IN_FRAC, fracs[i], &x0, &y0);
        dash_arc_point(g->cx, g->cy, g->r * DASH_GAUGE_TICK_OUT_FRAC, fracs[i], &x1, &y1);
        EVE_cmd_dl(VERTEX2F((int16_t)(x0 * 16.0f), (int16_t)(y0 * 16.0f)));
        EVE_cmd_dl(VERTEX2F((int16_t)(x1 * 16.0f), (int16_t)(y1 * 16.0f)));
    }
    EVE_cmd_dl(DL_END);
}

/* ---- TRACK mode (U7): shift lights, GPS hero, RPM bar, lap/delta ---- */
void draw_track_mode(uint32_t now_ms, uint8_t alpha)
{
    char buf[24];
    const bool rpm_ok = dash_ch_valid(&g_dash, DASH_CH_RPM);
    const bool spd_ok = dash_ch_valid(&g_dash, DASH_CH_SPEED);
    const float rpm = g_dash.ch.rpm;

    /* shift lights: 15 LEDs, dia 18 mock, gap 9; all dark when rpm invalid */
    const bool flash_zone = rpm_ok && dash_shift_flash_zone(rpm);
    const bool flash_on = dash_flash_phase(now_ms, DASH_SHIFT_FLASH_HALF_MS);
    const uint8_t lit = rpm_ok ? dash_shift_led_count(rpm) : 0U;
    const int16_t led_r = (int16_t)(DASH_LR(18) / 2);
    for (uint8_t i = 0U; i < DASH_SHIFT_LED_COUNT; i++)
    {
        const int16_t cx = DASH_LX(121 + i * 27);
        const int16_t cy = DASH_LY(31);
        bool on = rpm_ok && ((i + 1U) <= lit);
        uint32_t col = (i < 10U) ? COLOR_GREEN : (i < 13U) ? COLOR_AMBER : COLOR_RED_FILL;
        if (flash_zone)
        {
            on = flash_on;
            col = COLOR_RED_FILL;
        }
        if (on)
        {
            /* glow: a larger soft point behind the lit LED */
            dash_color(col, DA(60));
            EVE_cmd_dl(DL_POINT_SIZE | (uint32_t)((led_r + DASH_LR(6)) * 16));
            EVE_cmd_dl(DL_BEGIN | EVE_POINTS);
            EVE_cmd_dl(VERTEX2F((int16_t)(cx * 16), (int16_t)(cy * 16)));
            EVE_cmd_dl(DL_END);
        }
        dash_color(on ? col : COLOR_DEAD_DOT, on ? alpha : DA(230));
        EVE_cmd_dl(DL_POINT_SIZE | (uint32_t)(led_r * 16));
        EVE_cmd_dl(DL_BEGIN | EVE_POINTS);
        EVE_cmd_dl(VERTEX2F((int16_t)(cx * 16), (int16_t)(cy * 16)));
        EVE_cmd_dl(DL_END);
    }

    /* hairline divider under the LED row */
    dash_color(COLOR_HAIRLINE, alpha);
    EVE_cmd_dl(DL_LINE_WIDTH | 8UL);
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH_LX(20) * 16), (int16_t)(DASH_LY(60) * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH_LX(600) * 16), (int16_t)(DASH_LY(60) * 16)));
    EVE_cmd_dl(DL_END);

    /* GPS SPEED hero, centered in the left column (mock center x 210) */
    const int16_t hx = DASH_LX(210);
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text(hx, DASH_LY(112), dash_font(DF_LABEL), EVE_OPT_CENTERX, "GPS SPEED");
    dash_color(COLOR_VALUE, alpha);
    if (spd_ok)
    {
        snprintf(buf, sizeof(buf), "%d", (int)(g_dash.ch.speed_mph + 0.5f));
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }
    EVE_cmd_text(hx, DASH_LY(185), dash_font(DF_HERO), EVE_OPT_CENTER, buf);
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text(hx, DASH_LY(250), dash_font(DF_LABEL), EVE_OPT_CENTERX, "MPH");

    /* RPM row: label, live value, capsule bar, redline marker, scale */
    const int16_t bar_x = DASH_LX(37);
    const int16_t bar_w = (int16_t)(DASH_LX(383) - bar_x);
    const int16_t bar_y = DASH_LY(285);
    const int16_t bar_h = DASH_LY(8);

    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text(bar_x, DASH_LY(262), dash_font(DF_LABEL), 0U, "RPM");
    const DashColorState rc = rpm_ok ? dash_rpm_color(rpm) : DASH_COLOR_NORMAL;
    dash_color(dash_state_text_color(rc), alpha);
    if (rpm_ok)
    {
        snprintf(buf, sizeof(buf), "%d", (int)(rpm + 0.5f));
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }
    EVE_cmd_text((int16_t)(bar_x + bar_w), DASH_LY(258), dash_font(DF_VAL), EVE_OPT_RIGHTX, buf);

    dash_color(COLOR_BAR_TRACK, alpha);
    draw_pill(bar_x, bar_y, bar_w, bar_h);
    if (rpm_ok && (rpm > 0.0f))
    {
        const float bfrac = (rpm >= (float)DASH_REDLINE_RPM) ? 1.0f : (rpm / (float)DASH_REDLINE_RPM);
        const int16_t fw = (int16_t)((float)bar_w * bfrac);
        if (fw > bar_h)
        {
            dash_color((DASH_COLOR_RED == rc) ? COLOR_RED_FILL : COLOR_ACCENT, alpha);
            draw_pill(bar_x, bar_y, fw, bar_h);
        }
    }
    /* redline marker at 96% of the bar, 2 px, half opacity */
    dash_color(COLOR_RED_FILL, DA(128));
    EVE_cmd_dl(DL_LINE_WIDTH | 16UL);
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    const int16_t red_x = (int16_t)(bar_x + (int16_t)((float)bar_w * 0.96f));
    EVE_cmd_dl(VERTEX2F((int16_t)(red_x * 16), (int16_t)(bar_y * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)(red_x * 16), (int16_t)((bar_y + bar_h) * 16)));
    EVE_cmd_dl(DL_END);

    for (uint8_t i = 0U; i < 5U; i++)
    {
        dash_color((4U == i) ? COLOR_MUTED_RED : COLOR_FAINT, alpha);
        const int16_t sx = (int16_t)(bar_x + (int32_t)bar_w * i / 4);
        snprintf(buf, sizeof(buf), "%u", (unsigned)(i * 2U));
        EVE_cmd_text(sx, DASH_LY(300), dash_font(DF_TINY), EVE_OPT_CENTERX, buf);
    }

    /* right column: hairline, LAP TIME, DELTA value + center-zero bar */
    dash_color(COLOR_HAIRLINE, alpha);
    EVE_cmd_dl(DL_LINE_WIDTH | 8UL);
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH_LX(412) * 16), (int16_t)(DASH_LY(100) * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH_LX(412) * 16), (int16_t)(DASH_LY(300) * 16)));
    EVE_cmd_dl(DL_END);

    const int16_t col_x = DASH_LX(428);
    const int16_t col_x1 = DASH_LX(582);
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text(col_x, DASH_LY(140), dash_font(DF_LABEL), 0U, "LAP TIME");
    dash_color(COLOR_VALUE, alpha);
    dash_fmt_lap(g_dash.ch.lap_ms, dash_ch_valid(&g_dash, DASH_CH_LAP), buf);
    EVE_cmd_text(col_x, DASH_LY(158), dash_font(DF_MID), 0U, buf);

    const bool delta_ok = dash_ch_valid(&g_dash, DASH_CH_DELTA);
    const float dfill = delta_ok ? dash_delta_fill(g_dash.ch.delta_s) : 0.0f;
    const uint32_t dcol = (dfill <= 0.0f) ? COLOR_GREEN : COLOR_RED_TEXT;
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text(col_x, DASH_LY(212), dash_font(DF_LABEL), 0U, "DELTA");
    dash_color(delta_ok ? dcol : COLOR_VALUE, alpha);
    if (delta_ok)
    {
        snprintf(buf, sizeof(buf), "%+.2f", (double)g_dash.ch.delta_s);
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }
    EVE_cmd_text(col_x1, DASH_LY(208), dash_font(DF_VAL), EVE_OPT_RIGHTX, buf);

    const int16_t db_x = col_x;
    const int16_t db_w = (int16_t)(col_x1 - col_x);
    const int16_t db_y = DASH_LY(232);
    const int16_t db_h = DASH_LY(8);
    const int16_t db_cx = (int16_t)(db_x + db_w / 2);
    dash_color(COLOR_BAR_TRACK, alpha);
    draw_pill(db_x, db_y, db_w, db_h);
    dash_color(COLOR_HUB_RING, alpha); /* center-zero marker */
    EVE_cmd_dl(DL_LINE_WIDTH | 8UL);
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    EVE_cmd_dl(VERTEX2F((int16_t)(db_cx * 16), (int16_t)(db_y * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)(db_cx * 16), (int16_t)((db_y + db_h) * 16)));
    EVE_cmd_dl(DL_END);
    if (delta_ok && (dfill != 0.0f))
    {
        const int16_t fw = (int16_t)((float)(db_w / 2) * ((dfill < 0.0f) ? -dfill : dfill));
        if (fw > 1)
        {
            dash_color(dcol, alpha);
            EVE_cmd_dl(DL_BEGIN | EVE_RECTS);
            EVE_cmd_dl(DL_LINE_WIDTH | 16UL);
            if (dfill < 0.0f) /* ahead: fill left of center */
            {
                EVE_cmd_dl(VERTEX2F((int16_t)((db_cx - fw) * 16), (int16_t)(db_y * 16)));
                EVE_cmd_dl(VERTEX2F((int16_t)(db_cx * 16), (int16_t)((db_y + db_h) * 16)));
            }
            else /* behind: fill right of center */
            {
                EVE_cmd_dl(VERTEX2F((int16_t)(db_cx * 16), (int16_t)(db_y * 16)));
                EVE_cmd_dl(VERTEX2F((int16_t)((db_cx + fw) * 16), (int16_t)((db_y + db_h) * 16)));
            }
            EVE_cmd_dl(DL_END);
        }
    }

    /* footer: odometer (label left, value right) */
    dash_color(COLOR_HAIRLINE, alpha);
    EVE_cmd_dl(DL_LINE_WIDTH | 8UL);
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH_LX(22) * 16), (int16_t)(DASH_LY(358) * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)(DASH_LX(598) * 16), (int16_t)(DASH_LY(358) * 16)));
    EVE_cmd_dl(DL_END);
    dash_color(COLOR_FAINT, alpha);
    EVE_cmd_text(DASH_LX(22), DASH_LY(368), dash_font(DF_TINY), 0U, "ODOMETER");
    dash_color(COLOR_ODO, alpha);
    snprintf(buf, sizeof(buf), "%.1f", (double)dash_odo_miles(&g_odo));
    EVE_cmd_text(DASH_LX(570), DASH_LY(365), dash_font(DF_SMALL), EVE_OPT_RIGHTX, buf);
    dash_color(COLOR_FAINT, alpha);
    EVE_cmd_text(DASH_LX(578), DASH_LY(368), dash_font(DF_TINY), 0U, "MI");
}

/* ---- STREET mode (U8): dual sweep gauges, telltales, odometer ---- */

/* speedo tick fractions: every 20 MPH through the non-linear knee map */
static void street_speedo(uint8_t alpha)
{
    char buf[16];
    const GaugeSpec g = { (float)DASH_LX(193), (float)DASH_LY(186), (float)DASH_LR(156),
                          DASH_LR(16) };
    const bool ok = dash_ch_valid(&g_dash, DASH_CH_SPEED);
    const float mph = g_dash.ch.speed_mph;

    draw_gauge_chrome(&g, alpha);

    float tick_fracs[11];
    for (uint8_t i = 0U; i < 11U; i++)
    {
        tick_fracs[i] = dash_speed_frac((float)(i * 20U));
    }
    draw_gauge_ticks(&g, tick_fracs, 11U, alpha);

    /* tick labels at r66/88; 160/180/200 smaller + dimmer past the knee */
    for (uint8_t i = 0U; i < 11U; i++)
    {
        const uint16_t v = (uint16_t)(i * 20U);
        float lx;
        float ly;
        dash_arc_point(g.cx, g.cy, g.r * DASH_GAUGE_LABEL_R_FRAC, tick_fracs[i], &lx, &ly);
        const bool knee = (v > DASH_SPEED_KNEE);
        dash_color(knee ? COLOR_TICK_DIM : COLOR_LABEL, knee ? DA(200) : alpha);
        snprintf(buf, sizeof(buf), "%u", (unsigned)v);
        EVE_cmd_text((int16_t)lx, (int16_t)(ly - (float)DASH_LR(7)),
                     dash_font(DF_TINY), EVE_OPT_CENTER, buf);
    }

    if (ok && (mph > 0.5f))
    {
        dash_color(COLOR_ACCENT, alpha);
        draw_arc(g.cx, g.cy, g.r, 0.0f, dash_speed_frac(mph), g.stroke);
    }

    draw_gauge_needle_hub(&g, ok ? dash_speed_frac(mph) : 0.0f, ok, alpha);

    /* hub readout below center */
    dash_color(COLOR_VALUE, alpha);
    if (ok)
    {
        snprintf(buf, sizeof(buf), "%d", (int)(mph + 0.5f));
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }
    EVE_cmd_text((int16_t)g.cx, DASH_LY(255), dash_font(DF_BIG), EVE_OPT_CENTER, buf);
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text((int16_t)g.cx, DASH_LY(292), dash_font(DF_LABEL), EVE_OPT_CENTERX, "MPH");
}

static void street_tach(uint8_t alpha)
{
    char buf[16];
    const GaugeSpec g = { (float)DASH_LX(487), (float)DASH_LY(182), (float)DASH_LR(101),
                          DASH_LR(10) };
    const bool ok = dash_ch_valid(&g_dash, DASH_CH_RPM);
    const float rpm = g_dash.ch.rpm;
    const float frac = ok ? (rpm / (float)DASH_REDLINE_RPM) : 0.0f;

    draw_gauge_chrome(&g, alpha);

    /* static red zone 7600..8000 */
    dash_color(COLOR_REDZONE, alpha);
    draw_arc(g.cx, g.cy, g.r,
             (float)DASH_SHIFT_RPM / (float)DASH_REDLINE_RPM, 1.0f, g.stroke);

    float tick_fracs[9];
    for (uint8_t i = 0U; i < 9U; i++)
    {
        tick_fracs[i] = (float)i / 8.0f;
    }
    draw_gauge_ticks(&g, tick_fracs, 9U, alpha);
    for (uint8_t i = 0U; i < 9U; i++)
    {
        float lx;
        float ly;
        dash_arc_point(g.cx, g.cy, g.r * DASH_GAUGE_LABEL_R_FRAC, tick_fracs[i], &lx, &ly);
        dash_color((8U == i) ? COLOR_MUTED_RED : COLOR_LABEL, alpha);
        snprintf(buf, sizeof(buf), "%u", (unsigned)i);
        EVE_cmd_text((int16_t)lx, (int16_t)(ly - (float)DASH_LR(7)),
                     dash_font(DF_TINY), EVE_OPT_CENTER, buf);
    }

    if (ok && (frac > 0.005f))
    {
        const DashColorState rc = dash_rpm_color(rpm);
        dash_color((DASH_COLOR_RED == rc) ? COLOR_RED_FILL : COLOR_ACCENT, alpha);
        draw_arc(g.cx, g.cy, g.r, 0.0f, (frac > 1.0f) ? 1.0f : frac, g.stroke);
    }

    draw_gauge_needle_hub(&g, (frac > 1.0f) ? 1.0f : frac, ok, alpha);

    const DashColorState rc = ok ? dash_rpm_color(rpm) : DASH_COLOR_NORMAL;
    dash_color(dash_state_text_color(rc), alpha);
    if (ok)
    {
        snprintf(buf, sizeof(buf), "%d", (int)(rpm + 0.5f));
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }
    EVE_cmd_text((int16_t)g.cx, DASH_LY(228), dash_font(DF_MID), EVE_OPT_CENTER, buf);
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text((int16_t)g.cx, DASH_LY(252), dash_font(DF_TINY), EVE_OPT_CENTERX, "RPM");
}

/* telltale row: dead-front dots; lit = filled + glow + colored label; a
 * channel with NO data renders the distinct muted state (KTD4/R8 note) */
static void street_telltales(uint8_t alpha)
{
    const bool oilp_ok = dash_ch_valid(&g_dash, DASH_CH_OILP);
    const bool oilt_ok = dash_ch_valid(&g_dash, DASH_CH_OILT);
    const struct Telltale tt[4] = {
        { "FUEL", dash_ch_valid(&g_dash, DASH_CH_FUEL),
          dash_ch_valid(&g_dash, DASH_CH_FUEL) &&
              (DASH_COLOR_AMBER == dash_fuel_state(g_dash.ch.fuel_gal)),
          COLOR_AMBER },
        { "OIL", oilp_ok || oilt_ok,
          dash_telltale_oil(g_dash.ch.oil_press_psi, oilp_ok, g_dash.ch.oil_temp_f, oilt_ok),
          COLOR_RED_FILL },
        { "ECT", dash_ch_valid(&g_dash, DASH_CH_ECT),
          dash_ch_valid(&g_dash, DASH_CH_ECT) &&
              (DASH_COLOR_RED == dash_ect_state(g_dash.ch.ect_f)),
          COLOR_RED_FILL },
        { "VOLTS", dash_ch_valid(&g_dash, DASH_CH_VOLTS),
          dash_ch_valid(&g_dash, DASH_CH_VOLTS) &&
              (DASH_COLOR_RED == dash_volts_state(g_dash.ch.volts)),
          COLOR_RED_FILL },
    };

    /* four groups (dot + label) centered with 26 px mock gaps */
    const int16_t cy = DASH_LY(346);
    const int16_t dot_r = (int16_t)(DASH_LR(9) / 2);
    int16_t x = DASH_LX(214); /* row roughly centered for 4 groups */
    for (uint8_t i = 0U; i < 4U; i++)
    {
        uint32_t dot_col = COLOR_DEAD_DOT;
        uint32_t lab_col = COLOR_HUB_RING;
        uint8_t a = DA(230);
        if (!tt[i].valid)
        {
            dot_col = COLOR_NODATA; /* lost sensor is never "confirmed OK" */
            lab_col = COLOR_NODATA;
        }
        else if (tt[i].lit)
        {
            dot_col = tt[i].color;
            lab_col = tt[i].color;
            a = alpha;
            dash_color(dot_col, DA(60)); /* glow */
            EVE_cmd_dl(DL_POINT_SIZE | (uint32_t)((dot_r + DASH_LR(5)) * 16));
            EVE_cmd_dl(DL_BEGIN | EVE_POINTS);
            EVE_cmd_dl(VERTEX2F((int16_t)(x * 16), (int16_t)(cy * 16)));
            EVE_cmd_dl(DL_END);
        }
        dash_color(dot_col, a);
        EVE_cmd_dl(DL_POINT_SIZE | (uint32_t)(dot_r * 16));
        EVE_cmd_dl(DL_BEGIN | EVE_POINTS);
        EVE_cmd_dl(VERTEX2F((int16_t)(x * 16), (int16_t)(cy * 16)));
        EVE_cmd_dl(DL_END);
        dash_color(lab_col, a);
        EVE_cmd_text((int16_t)(x + DASH_LX(10)), (int16_t)(cy - DASH_LR(7)),
                     dash_font(DF_TINY), 0U, tt[i].label);
        x = (int16_t)(x + DASH_LX(48));
    }
}

void draw_street_mode(uint32_t now_ms, uint8_t alpha)
{
    (void)now_ms;
    char buf[24];

    street_speedo(alpha);
    street_tach(alpha);
    street_telltales(alpha);

    /* odometer, bottom center */
    dash_color(COLOR_FAINT, alpha);
    EVE_cmd_text(DASH_LX(258), DASH_LY(368), dash_font(DF_TINY), 0U, "ODOMETER");
    dash_color(COLOR_ODO, alpha);
    snprintf(buf, sizeof(buf), "%.1f", (double)dash_odo_miles(&g_odo));
    EVE_cmd_text(DASH_LX(310), DASH_LY(378), dash_font(DF_SMALL), EVE_OPT_CENTER, buf);
    dash_color(COLOR_FAINT, alpha);
    EVE_cmd_text(DASH_LX(352), DASH_LY(368), dash_font(DF_TINY), 0U, "MI");
}

/* ---- alarm takeover (U9): preempts both modes at ~2.8 Hz ---- */
void draw_alarm_takeover(DashAlarm alarm, uint32_t now_ms, uint8_t alpha)
{
    char buf[24];
    const bool bright = dash_flash_phase(now_ms, DASH_ALARM_FLASH_HALF_MS);

    if (bright)
    {
        /* vertical gradient approximation of the spec's red radial */
        EVE_cmd_dl(COLOR_A(alpha));
        EVE_cmd_gradient(0, 0, 0xD92020UL, 0, (int16_t)EVE_VSIZE, 0x700C0CUL);
    }
    else
    {
        dash_color(0x260606UL, alpha);
        EVE_cmd_dl(DL_LINE_WIDTH | 16UL);
        EVE_cmd_dl(DL_BEGIN | EVE_RECTS);
        EVE_cmd_dl(VERTEX2F(0, 0));
        /* bottom-right corner: EVE_HSIZE*16 = 16384 overflows VERTEX2F's
         * signed 15-bit field (the splash-round wraparound lesson), so the
         * rect ends at the last representable 1/16 px inside the panel */
        EVE_cmd_dl(VERTEX2F((int16_t)(EVE_HSIZE * 16 - 1), (int16_t)(EVE_VSIZE * 16)));
        EVE_cmd_dl(DL_END);
    }

    const int16_t cx = (int16_t)(EVE_HSIZE / 2U);
    const char *title = (DASH_ALARM_OILP == alarm) ? "OIL PRESSURE" :
                        (DASH_ALARM_OILT == alarm) ? "OIL TEMP" : "COOLANT TEMP";
    /* limit text derives from the same constants dash_alarm_classify()
     * compares against, so a threshold tune can never leave the banner
     * showing a stale number */
    char limit[24];
    if (DASH_ALARM_OILP == alarm)
    {
        snprintf(limit, sizeof(limit), "MINIMUM %d PSI", (int)DASH_OILP_RED_PSI);
    }
    else
    {
        snprintf(limit, sizeof(limit), "MAXIMUM %d F",
                 (DASH_ALARM_OILT == alarm) ? (int)DASH_OILT_RED_F : (int)DASH_ECT_RED_F);
    }
    const float live = (DASH_ALARM_OILP == alarm) ? g_dash.ch.oil_press_psi :
                       (DASH_ALARM_OILT == alarm) ? g_dash.ch.oil_temp_f : g_dash.ch.ect_f;

    dash_color(COLOR_ALARM_TXT, alpha);
    EVE_cmd_text(cx, DASH_LY(120), dash_font(DF_LABEL), EVE_OPT_CENTER, "WARNING");
    dash_color(COLOR_VALUE, alpha);
    EVE_cmd_text(cx, DASH_LY(165), dash_font(DF_TITLE), EVE_OPT_CENTER, title);
    snprintf(buf, sizeof(buf), "%d", (int)(live + 0.5f));
    EVE_cmd_text(cx, DASH_LY(245), dash_font(DF_HERO), EVE_OPT_CENTER, buf);
    dash_color(COLOR_ALARM_TXT, alpha);
    EVE_cmd_text(cx, DASH_LY(330), dash_font(DF_LABEL), EVE_OPT_CENTER, limit);
}

/* One dash composition: fonts registered, then alarm-or-mode content. The
 * alarm classifier reads valid channels only, so a missing sensor can never
 * assert (or hold) a takeover (R10/R11). */
void draw_dash_content(uint32_t now_ms, uint8_t alpha)
{
    dash_register_fonts();

    const DashAlarm alarm = dash_alarm_classify(&g_dash);
    if (DASH_ALARM_NONE != alarm)
    {
        draw_alarm_takeover(alarm, now_ms, alpha);
    }
    else if (DASH_MODE_STREET == g_dash.mode)
    {
        draw_street_mode(now_ms, alpha);
    }
    else
    {
        draw_track_mode(now_ms, alpha);
    }
    EVE_cmd_dl(COLOR_A(255U));
}

/* Build one un-swapped frame for a mode and read back its display-list
 * usage (REG_CMD_DL, bytes -> words). Boot-time diagnostic only (KTD8);
 * the DL is discarded by the next CMD_DLSTART. */
uint16_t measure_mode_dl(DashMode mode)
{
    const DashMode saved = g_dash.mode;
    g_dash.mode = mode;
    EVE_cmd_dl(CMD_DLSTART);
    EVE_cmd_dl(DL_CLEAR_COLOR_RGB | COLOR_BG);
    EVE_cmd_dl(DL_CLEAR | CLR_COL | CLR_STN | CLR_TAG);
    draw_dash_content(0UL, 255U);
    EVE_execute_cmd();
    const uint32_t dl_bytes = EVE_memRead32(REG_CMD_DL);
    g_dash.mode = saved;
    return (uint16_t)(dl_bytes / 4UL);
}

/* The standing display: one full dash frame, swapped in. */
void dash_frame(uint32_t now_ms)
{
    eve_frame_begin(COLOR_BG);
    draw_dash_content(now_ms, 255U);
    eve_frame_end();
}

/* DA is this header's private alpha-scaling helper; end its scope here so it
 * cannot silently capture an `alpha` parameter in splash_render.h or the
 * .ino below (review finding; mirrors splash_render.h's SPLASH_A pattern). */
#undef DA

#endif /* DASH_RENDER_H */
