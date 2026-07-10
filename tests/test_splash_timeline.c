/*
 * Invariant test: the boot-splash timeline must match the design spec in
 * assets/splash/README.md exactly -- element windows, easing endpoints,
 * slide distances, and the overshoot bound that sizes the emblem's draw
 * window. Pins the animation math so a future edit cannot silently drift
 * from the approved design (covers AE4's math half; R1).
 *
 * Runs on the host:
 *   gcc -std=c11 -I MustangDash tests/test_splash_timeline.c -o /tmp/st && /tmp/st
 */

#include <math.h>
#include <stdio.h>
#include <stdint.h>

#include "splash_timeline.h"

/* window constants must equal the spec's timeline table */
_Static_assert(SPLASH_DURATION_MS == 2000U, "splash duration must be 2000 ms");
_Static_assert(SPLASH_BARS_START == 200U && SPLASH_BARS_END == 840U,
               "bars window must be 200-840 ms");
_Static_assert(SPLASH_EMBLEM_START == 360U && SPLASH_EMBLEM_END == 1000U,
               "emblem window must be 360-1000 ms");
_Static_assert(SPLASH_WORD_START == 760U && SPLASH_WORD_END == 1360U,
               "wordmark/year window must be 760-1360 ms");
_Static_assert(SPLASH_LINE_START == 1040U && SPLASH_LINE_END == 1640U,
               "accent line window must be 1040-1640 ms");

/* slide endpoints must equal the spec's layout table */
_Static_assert(SPLASH_BAR_LEFT_X_FROM == -150 && SPLASH_BAR_LEFT_X_TO == 138,
               "left bar must slide -150 -> 138");
_Static_assert(SPLASH_BAR_RIGHT_X_FROM == 934 && SPLASH_BAR_RIGHT_X_TO == 646,
               "right bar must slide 934 -> 646");
_Static_assert(SPLASH_WORD_RISE_PX == 26, "wordmark/year must rise 26 px");

/* the layout table's absolute positions must agree with their centering:
 * the 200 px emblem at (412, 124) centers on the (512, 224) scale pivot,
 * and both accent-line variants center on x = 512 */
_Static_assert(SPLASH_EMBLEM_X + 200 / 2 == SPLASH_EMBLEM_CX,
               "emblem position must center on the x=512 pivot");
_Static_assert(SPLASH_EMBLEM_Y + 200 / 2 == SPLASH_EMBLEM_CY,
               "emblem position must center on the y=224 pivot");
_Static_assert(SPLASH_LINE_X == SPLASH_EMBLEM_CX - SPLASH_LINE_W / 2,
               "accent line canvas must center on x=512");
_Static_assert(SPLASH_CLINE_X == SPLASH_EMBLEM_CX - SPLASH_CLINE_W / 2,
               "checker line must center on x=512");

static int failures = 0;

static void expect(int cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

static int nearf(float a, float b, float eps)
{
    return fabsf(a - b) <= eps;
}

int main(void)
{
    /* progress clamps outside its window */
    expect(splash_progress(0U, 200U, 840U) == 0.0f, "progress before window must be 0");
    expect(splash_progress(199U, 200U, 840U) == 0.0f, "progress at start-1 must be 0");
    expect(splash_progress(840U, 200U, 840U) == 1.0f, "progress at end must be 1");
    expect(splash_progress(5000U, 200U, 840U) == 1.0f, "progress after window must be 1");

    /* ease-out cubic: f(0)=0, f(0.5)=0.875, f(1)=1 */
    expect(nearf(splash_ease_out_cubic(0.0f), 0.0f, 1e-6f), "cubic f(0) must be 0");
    expect(nearf(splash_ease_out_cubic(0.5f), 0.875f, 1e-6f), "cubic f(0.5) must be 0.875");
    expect(nearf(splash_ease_out_cubic(1.0f), 1.0f, 1e-6f), "cubic f(1) must be 1");

    /* ease-out back: f(0)=0, f(1)=1, overshoots above 1 mid-window */
    expect(nearf(splash_ease_out_back(0.0f), 0.0f, 1e-5f), "back f(0) must be 0");
    expect(nearf(splash_ease_out_back(1.0f), 1.0f, 1e-5f), "back f(1) must be 1");
    float back_max = 0.0f;
    for (int i = 0; i <= 1000; i++)
    {
        float v = splash_ease_out_back((float)i / 1000.0f);
        if (v > back_max) { back_max = v; }
    }
    expect(back_max > 1.0f, "ease-out back must overshoot above 1.0");
    expect(back_max < 1.11f, "ease-out back overshoot must stay below 1.11");

    /* start states (now = 0): everything animated is hidden / at origin */
    expect(splash_bar_left_x(0U) == -150, "left bar must start at x = -150");
    expect(splash_bar_right_x(0U) == 934, "right bar must start at x = 934");
    expect(splash_bars_alpha(0U) == 0U, "bars must start invisible");
    expect(splash_emblem_alpha(0U) == 0U, "emblem must start invisible");
    expect(nearf(splash_emblem_scale(0U), 0.70f, 1e-4f), "emblem must start at scale 0.70");
    expect(splash_word_dy(0U) == 26, "wordmark must start 26 px below final");
    expect(splash_word_alpha(0U) == 0U, "wordmark must start invisible");
    expect(nearf(splash_line_reveal(0U), 0.0f, 1e-6f), "accent line must start unrevealed");
    expect(!splash_done(0U), "splash must not be done at t=0");

    /* final states (now = 2000, and any hold-phase instant): spec layout */
    for (uint32_t t = SPLASH_LINE_END; t <= SPLASH_DURATION_MS; t += 90U)
    {
        expect(splash_bar_left_x(t) == 138, "left bar must hold at x = 138");
        expect(splash_bar_right_x(t) == 646, "right bar must hold at x = 646");
        expect(splash_bars_alpha(t) == 255U, "bars must hold fully opaque");
        expect(splash_emblem_alpha(t) == 255U, "emblem must hold fully opaque");
        expect(nearf(splash_emblem_scale(t), 1.0f, 1e-4f), "emblem must hold at scale 1.0");
        expect(splash_word_dy(t) == 0, "wordmark must hold at final y");
        expect(splash_word_alpha(t) == 255U, "wordmark must hold fully opaque");
        expect(nearf(splash_line_reveal(t), 1.0f, 1e-6f), "accent line must hold fully revealed");
    }
    expect(splash_done(SPLASH_DURATION_MS), "splash must be done at t=2000");
    expect(!splash_done(SPLASH_DURATION_MS - 1U), "splash must not be done at t=1999");

    /* emblem overshoot bound: peak drawn size must fit the 220 px draw window */
    float scale_max = 0.0f;
    for (uint32_t t = 0U; t <= SPLASH_DURATION_MS; t++)
    {
        float s = splash_emblem_scale(t);
        if (s > scale_max) { scale_max = s; }
    }
    expect(scale_max > 1.0f, "emblem scale must overshoot above 1.0");
    expect(scale_max * 200.0f <= (float)SPLASH_EMBLEM_DRAW_PX,
           "peak emblem size must fit the SPLASH_EMBLEM_DRAW_PX window");

    /* bar motion must be monotonic (no jitter): left never moves right-to-left */
    int16_t prev = splash_bar_left_x(0U);
    for (uint32_t t = 1U; t <= SPLASH_BARS_END + 50U; t += 5U)
    {
        int16_t x = splash_bar_left_x(t);
        expect(x >= prev, "left bar x must be monotonically non-decreasing");
        prev = x;
    }

    if (failures == 0)
    {
        printf("OK: splash timeline matches the spec (windows, easing, endpoints, overshoot bound)\n");
        return 0;
    }
    return 1;
}
