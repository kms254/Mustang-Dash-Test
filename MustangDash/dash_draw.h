/*
 * dash_draw.h - shared EVE drawing primitives for every dash screen: the
 * gauge spec type, color/alpha helpers, capsule bars, polyline arcs, and
 * the sweep-gauge chrome/needle/tick painters. Pure code motion out of
 * dash_render.h so the side-screen renderers can compose from the same
 * primitives as the center.
 *
 * NOT a pure header: everything here drives the EVE co-processor, so it is
 * not host-testable and is not part of tests/run-tests.sh. Include exactly
 * once, from MustangDash.ino, BEFORE dash_render.h and the side render
 * headers, and after the COLOR_* constants and dash_layout.h they use.
 */
#ifndef DASH_DRAW_H
#define DASH_DRAW_H

/* One sweep gauge (KTD2/KTD9): geometry derives from the design's shared
 * viewBox fractions (ticks r80-88, labels r66, needle r62, hub r7). */
struct GaugeSpec
{
    float cx, cy, r; /* panel px */
    int16_t stroke;  /* arc stroke, panel px */
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

/* Alpha pre-scaled by the crossfade; each render header carries its own
 * scoped copy of this one-liner (define/#undef pair) so the macro never
 * leaks across headers -- the splash SPLASH_A lesson. */
#define DA(a) ((uint8_t)(((uint16_t)(a) * (uint16_t)alpha) / 255U))

/* Rounded rect: RECTS with LINE_WIDTH as an explicit corner radius. */
static void draw_round_rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r)
{
    EVE_cmd_dl(DL_LINE_WIDTH | (uint32_t)(r * 16));
    EVE_cmd_dl(DL_BEGIN | EVE_RECTS);
    EVE_cmd_dl(VERTEX2F((int16_t)((x + r) * 16), (int16_t)((y + r) * 16)));
    EVE_cmd_dl(VERTEX2F((int16_t)((x + w - r) * 16), (int16_t)((y + h - r) * 16)));
    EVE_cmd_dl(DL_END);
}

/* A rounded-capsule bar: the full-round special case (r = h/2). */
static void draw_pill(int16_t x, int16_t y, int16_t w, int16_t h)
{
    draw_round_rect(x, y, w, h, (int16_t)(h / 2));
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

#undef DA /* scoped to this header; render headers define their own copy */

#endif /* DASH_DRAW_H */
