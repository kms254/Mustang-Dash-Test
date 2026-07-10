/*
 * engine_render.h - EVE display-list renderer for the LEFT 5" ENGINE screen
 * (800x480): TRACK = 4x2 hairline telemetry grid + PMU16 outputs strip,
 * STREET = 2x2 mini sweep gauges, shared header row. Content authority is
 * assets/dash-design/README.md ("LEFT 5" - ENGINE") and the HTML mock's
 * 420x320 canvas, scaled through DASH5_LX/LY/LR (KTD4).
 *
 * NOT a pure header: everything here drives the EVE co-processor and reads
 * the sketch's file-scope state, so it is not host-testable and is not part
 * of tests/run-tests.sh. Include exactly once, from MustangDash.ino, after
 * dash_draw.h (GaugeSpec + primitives) and dash_render.h (the DF_* font
 * enum) and the glue prototypes (dash_font). Fonts are registered by the
 * frame dispatcher before these compositions run; nothing here registers
 * them. Invalid channels follow the dead-front convention (R7): "--" text,
 * empty arc fills, parked needles, COLOR_NODATA chips -- never stale data.
 *
 * Font-charset notes (tools/make_dash_fonts.py): DF_LABEL/DF_TINY carry
 * uppercase + digits only, with ';' rendering as the design's '\xb7'
 * separator and '"' as the inch mark; '-' and '.' exist only in the Saira
 * value instances, so every "--" placeholder renders in DF_VAL.
 */
#ifndef ENGINE_RENDER_H
#define ENGINE_RENDER_H

/* Alpha pre-scaled by the crossfade; each render header carries its own
 * scoped copy of this one-liner (define/#undef pair) so the macro never
 * leaks across headers -- the splash SPLASH_A lesson. */
#define DA(a) ((uint8_t)(((uint16_t)(a) * (uint16_t)alpha) / 255U))

/* ---- mock-px layout constants (420x320 canvas) ----
 * Content box from the mock's bezel padding (9 px) + inner padding
 * (15 px 16 px): x 25..395, y 24..296. */
#define ENG_X0 25
#define ENG_X1 395

/* TRACK grid lattice: 4 columns x 2 rows between header and PMU strip. */
static const int16_t ENG_GRID_X[5] = { 25, 118, 210, 302, 395 };
static const int16_t ENG_GRID_Y[3] = { 50, 142, 233 };

/* One TRACK grid cell: label top-left, value + unit bottom (KTD5 colors
 * via the dash_math classifier). AFR cells carry no unit (mock). */
struct EngineCell
{
    const char *label;
    const char *unit;
    uint8_t ch;                    /* DASH_CH_* id */
    uint8_t decimals;              /* 0 = int, 1 = one decimal */
    DashColorState (*state)(float);
};

/* Row-major per the design: pit-now row, then trend row. */
static const struct EngineCell ENG_CELLS[8] = {
    { "OIL P",  "PSI", DASH_CH_OILP,  0U, dash_oil_press_state },
    { "ECT",    "F",   DASH_CH_ECT,   0U, dash_ect_state },
    { "FUEL P", "PSI", DASH_CH_FUELP, 0U, dash_fuelp_state },
    { "AFR L",  "",    DASH_CH_AFR_L, 1U, dash_afr_state },
    { "OIL T",  "F",   DASH_CH_OILT,  0U, dash_oil_temp_state },
    { "IAT",    "F",   DASH_CH_IAT,   0U, dash_iat_state },
    { "VOLTS",  "V",   DASH_CH_VOLTS, 1U, dash_volts_state },
    { "AFR R",  "",    DASH_CH_AFR_R, 1U, dash_afr_state },
};

/* One STREET mini sweep gauge (mock 132x118 box, arc r 60.5 mock px). */
struct EngineMiniGauge
{
    const char *label;
    uint8_t ch;
    float vmin;
    float vmax;
    uint8_t decimals;
    DashColorState (*state)(float);
};

static const struct EngineMiniGauge ENG_GAUGES[4] = {
    { "OIL P ; PSI", DASH_CH_OILP,    0.0f, 100.0f, 0U, dash_oil_press_state },
    { "ECT ; F",     DASH_CH_ECT,   100.0f, 260.0f, 0U, dash_ect_state },
    { "OIL T ; F",   DASH_CH_OILT,  100.0f, 300.0f, 0U, dash_oil_temp_state },
    { "VOLTS",       DASH_CH_VOLTS,   8.0f,  16.0f, 1U, dash_volts_state },
};

/* mini-gauge cell centers (mock px): 2x2 in the content area below the
 * header; gauge center sits 65 mock px below the 118-px box top (viewBox
 * center (120,80) of "24 -14 192 172" at the 132/192 display scale). */
static const int16_t ENG_GAUGE_CX[2] = { 118, 302 };
static const int16_t ENG_GAUGE_CY[2] = { 117, 240 };

/* "--" when invalid, else the value at 0 or 1 decimals (house rounding). */
static void engine_fmt_value(char *buf, size_t n, float v, uint8_t decimals, bool ok)
{
    if (!ok)
    {
        snprintf(buf, n, "--");
    }
    else if (0U != decimals)
    {
        snprintf(buf, n, "%.1f", (double)v);
    }
    else
    {
        snprintf(buf, n, "%d", (int)(v + 0.5f));
    }
}

/* Rounded rect with an explicit corner radius (the PMU chips' 6 mock px;
 * draw_pill hardcodes r = h/2, too round for a chip). */
static void engine_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r)
{
    EVE_cmd_dl(DL_LINE_WIDTH | (uint32_t)(r * 16));
    EVE_cmd_dl(DL_BEGIN | EVE_RECTS);
    EVE_cmd_dl(VERTEX2F((int16_t)((x + r) * 16), (int16_t)((y + r) * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)((x + w - r) * 16), (int16_t)((y + h - r) * 16)));
    EVE_cmd_dl(DL_END);
}

/* Header row, both modes: "ENGINE" + source tag ("COYOTE CAN ; 5"" renders
 * as the design's "COYOTE CAN <middot> 5<inch>" through the label charset). */
static void engine_header(uint8_t alpha)
{
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text(DASH5_LX(ENG_X0), DASH5_LY(22), dash_font(DF_LABEL), 0U, "ENGINE");
    dash_color(COLOR_FAINT, alpha);
    EVE_cmd_text(DASH5_LX(ENG_X1), DASH5_LY(25), dash_font(DF_TINY),
                 EVE_OPT_RIGHTX, "COYOTE CAN ; 5\"");
}

/* ---- TRACK: 4x2 hairline grid ---- */
static void engine_track_grid(uint8_t alpha)
{
    char buf[16];

    /* hairline lattice: border + internal dividers, 1 px */
    dash_color(COLOR_HAIRLINE, alpha);
    EVE_cmd_dl(DL_LINE_WIDTH | 8UL);
    EVE_cmd_dl(DL_BEGIN | EVE_LINES);
    for (uint8_t i = 0U; i < 5U; i++)
    {
        EVE_cmd_dl(VERTEX2F((int16_t)(DASH5_LX(ENG_GRID_X[i]) * 16),
                            (int16_t)(DASH5_LY(ENG_GRID_Y[0]) * 16)));
        EVE_cmd_dl(VERTEX2F((int16_t)(DASH5_LX(ENG_GRID_X[i]) * 16),
                            (int16_t)(DASH5_LY(ENG_GRID_Y[2]) * 16)));
    }
    for (uint8_t j = 0U; j < 3U; j++)
    {
        EVE_cmd_dl(VERTEX2F((int16_t)(DASH5_LX(ENG_GRID_X[0]) * 16),
                            (int16_t)(DASH5_LY(ENG_GRID_Y[j]) * 16)));
        EVE_cmd_dl(VERTEX2F((int16_t)(DASH5_LX(ENG_GRID_X[4]) * 16),
                            (int16_t)(DASH5_LY(ENG_GRID_Y[j]) * 16)));
    }
    EVE_cmd_dl(DL_END);

    for (uint8_t i = 0U; i < 8U; i++)
    {
        const struct EngineCell *c = &ENG_CELLS[i];
        const int16_t x0 = ENG_GRID_X[i % 4U];
        const int16_t x1 = ENG_GRID_X[(i % 4U) + 1U];
        const int16_t y0 = ENG_GRID_Y[i / 4U];
        const int16_t y1 = ENG_GRID_Y[(i / 4U) + 1U];
        const bool ok = dash_ch_valid(&g_dash, c->ch);
        const float v = dash_ch_get(&g_dash, c->ch);

        dash_color(COLOR_LABEL, alpha);
        EVE_cmd_text(DASH5_LX(x0 + 10), DASH5_LY(y0 + 9),
                     dash_font(DF_TINY), 0U, c->label);

        /* value color from the channel's classifier; invalid = plain "--" */
        const DashColorState rc = ok ? c->state(v) : DASH_COLOR_NORMAL;
        dash_color(dash_state_text_color(rc), alpha);
        engine_fmt_value(buf, sizeof(buf), v, c->decimals, ok);
        EVE_cmd_text(DASH5_LX(x0 + 10), DASH5_LY(y1 - 31),
                     dash_font(DF_VAL), 0U, buf);

        if ('\0' != c->unit[0])
        {
            dash_color(COLOR_FAINT, alpha);
            EVE_cmd_text(DASH5_LX(x1 - 8), DASH5_LY(y1 - 19),
                         dash_font(DF_TINY), EVE_OPT_RIGHTX, c->unit);
        }
    }
}

/* ---- TRACK: "PMU16 x2 - OUTPUTS" strip (KTD5) ----
 * amps == 0 -> gray OFF chip; amps > 0 -> green chip with the amp figure;
 * channel invalid -> dead-front COLOR_NODATA chip. FAULT is deferred to the
 * CAN round (Scope Boundaries) and deliberately not rendered here. */
static void engine_pmu_strip(uint8_t alpha)
{
    static const struct { const char *label; uint8_t ch; } chips[3] = {
        { "PUMP",  DASH_CH_PUMP },
        { "FAN 1", DASH_CH_FAN1 },
        { "FAN 2", DASH_CH_FAN2 },
    };
    char buf[16];
    const int16_t chip_w = 119;
    const int16_t chip_h = 37;
    const int16_t chip_y = 258;

    /* strip label row */
    dash_color(COLOR_FAINT, alpha);
    EVE_cmd_text(DASH5_LX(ENG_X0), DASH5_LY(244), dash_font(DF_TINY), 0U,
                 "PMU16 X2 ; OUTPUTS");

    /* total amps: sum of the VALID output channels only; "--" when none */
    float total = 0.0f;
    uint8_t n_valid = 0U;
    for (uint8_t i = 0U; i < 3U; i++)
    {
        if (dash_ch_valid(&g_dash, chips[i].ch))
        {
            total += dash_ch_get(&g_dash, chips[i].ch);
            n_valid++;
        }
    }
    dash_color(COLOR_FAINT, alpha);
    if (n_valid > 0U)
    {
        snprintf(buf, sizeof(buf), "%.1f", (double)total);
        EVE_cmd_text(DASH5_LX(352), DASH5_LY(240), dash_font(DF_SMALL),
                     EVE_OPT_RIGHTX, buf);
    }
    else
    {
        EVE_cmd_text(DASH5_LX(352), DASH5_LY(238), dash_font(DF_VAL),
                     EVE_OPT_RIGHTX, "--");
    }
    EVE_cmd_text(DASH5_LX(355), DASH5_LY(244), dash_font(DF_TINY), 0U, "A TOTAL");

    for (uint8_t i = 0U; i < 3U; i++)
    {
        const int16_t cx0 = (int16_t)(ENG_X0 + i * (chip_w + 6));
        const int16_t ccx = (int16_t)(cx0 + chip_w / 2);
        const bool ok = dash_ch_valid(&g_dash, chips[i].ch);
        const float amps = dash_ch_get(&g_dash, chips[i].ch);
        const bool on = ok && (amps > 0.0f);

        /* chip border: filled rounded rect in the state color, then the
         * interior punched back to the panel background */
        uint32_t edge = ok ? (on ? COLOR_GREEN : COLOR_HUB_RING) : COLOR_NODATA;
        dash_color(edge, on ? DA(110) : DA(180)); /* mock border sits dim */
        engine_round_rect(DASH5_LX(cx0), DASH5_LY(chip_y),
                          DASH5_LX(chip_w), DASH5_LY(chip_h), DASH5_LR(6));
        dash_color(COLOR_BG, 255U); /* opaque punch-out, crossfade-safe */
        engine_round_rect((int16_t)(DASH5_LX(cx0) + 2), (int16_t)(DASH5_LY(chip_y) + 2),
                          (int16_t)(DASH5_LX(chip_w) - 4), (int16_t)(DASH5_LY(chip_h) - 4),
                          (int16_t)(DASH5_LR(6) - 2));

        dash_color(edge, on ? DA(190) : alpha); /* label at .75 opacity when lit */
        EVE_cmd_text(DASH5_LX(ccx), DASH5_LY(chip_y + 5),
                     dash_font(DF_TINY), EVE_OPT_CENTERX, chips[i].label);

        if (!ok)
        {
            dash_color(COLOR_NODATA, alpha);
            EVE_cmd_text(DASH5_LX(ccx), DASH5_LY(chip_y + 13),
                         dash_font(DF_VAL), EVE_OPT_CENTERX, "--");
        }
        else if (on)
        {
            /* "8.6A": digits (DF_SMALL) right-aligned to a join point, the
             * "A" (DF_TINY, the only letter-bearing rung) after it */
            dash_color(COLOR_GREEN, alpha);
            snprintf(buf, sizeof(buf), "%.1f", (double)amps);
            EVE_cmd_text((int16_t)(DASH5_LX(ccx) + DASH5_LX(6)), DASH5_LY(chip_y + 15),
                         dash_font(DF_SMALL), EVE_OPT_RIGHTX, buf);
            EVE_cmd_text((int16_t)(DASH5_LX(ccx) + DASH5_LX(7)), DASH5_LY(chip_y + 22),
                         dash_font(DF_TINY), 0U, "A");
        }
        else
        {
            dash_color(COLOR_HUB_RING, alpha);
            EVE_cmd_text(DASH5_LX(ccx), DASH5_LY(chip_y + 16),
                         dash_font(DF_LABEL), EVE_OPT_CENTERX, "OFF");
        }
    }
}

/* ---- STREET: one 2x2 mini sweep gauge cell ---- */
static void engine_mini_gauge(const struct EngineMiniGauge *mg,
                              int16_t cx_mock, int16_t cy_mock, uint8_t alpha)
{
    char buf[16];
    const GaugeSpec g = { (float)DASH5_LX(cx_mock), (float)DASH5_LY(cy_mock),
                          (float)DASH5_LR(60), DASH5_LR(7) };
    const bool ok = dash_ch_valid(&g_dash, mg->ch);
    const float v = dash_ch_get(&g_dash, mg->ch);

    float frac = ok ? (v - mg->vmin) / (mg->vmax - mg->vmin) : 0.0f;
    if (frac < 0.0f) { frac = 0.0f; }
    if (frac > 1.0f) { frac = 1.0f; }
    const DashColorState rc = ok ? mg->state(v) : DASH_COLOR_NORMAL;

    draw_gauge_chrome(&g, alpha);
    if (ok && (frac > 0.005f))
    {
        dash_color((DASH_COLOR_RED == rc) ? COLOR_RED_FILL : COLOR_ACCENT, alpha);
        draw_arc(g.cx, g.cy, g.r, 0.0f, frac, g.stroke);
    }
    draw_gauge_needle_hub(&g, frac, ok, alpha);

    /* hub readout: value below center in the open 120-degree gap, label
     * under it (DF_VAL: mock's 19 px value = 28.5 native, the closest rung) */
    dash_color(dash_state_text_color(rc), alpha);
    engine_fmt_value(buf, sizeof(buf), v, mg->decimals, ok);
    EVE_cmd_text((int16_t)g.cx, DASH5_LY(cy_mock + 24),
                 dash_font(DF_VAL), EVE_OPT_CENTER, buf);
    dash_color(COLOR_LABEL, alpha);
    EVE_cmd_text((int16_t)g.cx, DASH5_LY(cy_mock + 33),
                 dash_font(DF_TINY), EVE_OPT_CENTERX, mg->label);
}

/* ---- public entry points (prototyped and dispatched from the .ino) ---- */

void engine_track_screen(uint8_t alpha)
{
    engine_header(alpha);
    engine_track_grid(alpha);
    engine_pmu_strip(alpha);
}

void engine_street_screen(uint8_t alpha)
{
    engine_header(alpha);
    for (uint8_t i = 0U; i < 4U; i++)
    {
        engine_mini_gauge(&ENG_GAUGES[i], ENG_GAUGE_CX[i % 2U],
                          ENG_GAUGE_CY[i / 2U], alpha);
    }
}

/* DA is this header's private alpha-scaling helper; end its scope here so
 * it cannot silently capture an `alpha` parameter in timing_render.h or the
 * .ino below (same discipline as dash_render.h / dash_draw.h). */
#undef DA

#endif /* ENGINE_RENDER_H */
