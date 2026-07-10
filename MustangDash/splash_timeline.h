/*
 * splash_timeline.h - pure animation math for the 2000 ms boot splash.
 *
 * Implements the timeline from assets/splash/README.md: element windows,
 * ease-out cubic, ease-out back (spec coefficients 2.7 / 1.7), and the
 * per-element position / alpha / scale interpolation. No Arduino or EVE
 * dependencies so every number here is host-testable
 * (see tests/test_splash_timeline.c) -- the backlight_wave.h pattern.
 *
 * The renderer (MustangDash.ino) consumes these helpers once per frame with
 * now_ms = milliseconds since splash start.
 */

#ifndef SPLASH_TIMELINE_H
#define SPLASH_TIMELINE_H

#include <stdint.h>

/* ---- timeline windows (ms), from the spec ---- */
#define SPLASH_DURATION_MS   2000U
#define SPLASH_BARS_START     200U
#define SPLASH_BARS_END       840U
#define SPLASH_EMBLEM_START   360U
#define SPLASH_EMBLEM_END    1000U
#define SPLASH_WORD_START     760U
#define SPLASH_WORD_END      1360U
#define SPLASH_LINE_START    1040U
#define SPLASH_LINE_END      1640U
/* 1640..2000 is the hold; every element is at its final state there. */

/* ---- final layout (top-left px), from the spec's layout table ---- */
#define SPLASH_BAR_LEFT_X_FROM  (-150)
#define SPLASH_BAR_LEFT_X_TO      138
#define SPLASH_BAR_RIGHT_X_FROM   934
#define SPLASH_BAR_RIGHT_X_TO     646
#define SPLASH_BAR_Y              202
#define SPLASH_EMBLEM_X           412
#define SPLASH_EMBLEM_Y           124
#define SPLASH_EMBLEM_CX          512 /* scale pivot */
#define SPLASH_EMBLEM_CY          224
#define SPLASH_WORD_X             162
#define SPLASH_WORD_Y             358
#define SPLASH_WORD_RISE_PX        26 /* slides up from final+26 to final */
#define SPLASH_LINE_X             342
#define SPLASH_LINE_Y             420
#define SPLASH_LINE_W             340 /* full asset canvas width */
#define SPLASH_YEAR_X             412
#define SPLASH_YEAR_Y             456
/* checkered-theme variants */
#define SPLASH_CBLOCK_Y           198
#define SPLASH_CLINE_X            362
#define SPLASH_CLINE_Y            440
#define SPLASH_CLINE_W            300
#define SPLASH_CSTRIP_TOP_Y         0
#define SPLASH_CSTRIP_BOT_Y       574
#define SPLASH_CSTRIP_ALT_OFFSET   13 /* bottom strip x offset, alternating start */

/* Emblem scale runs 0.70 -> 1.00 with ease-out-back overshoot. The easing
 * peaks at ~1.0998, so the scale peaks at ~1.030 and the drawn emblem never
 * exceeds 206 px; a 220 px draw window is overshoot-safe (test-pinned). */
#define SPLASH_EMBLEM_SCALE_FROM 0.70f
#define SPLASH_EMBLEM_SCALE_TO   1.00f
#define SPLASH_EMBLEM_DRAW_PX     220

/* Linear progress through [start_ms, end_ms], clamped to [0, 1]. */
static inline float splash_progress(uint32_t now_ms, uint32_t start_ms, uint32_t end_ms)
{
    if (now_ms <= start_ms) { return 0.0f; }
    if (now_ms >= end_ms)   { return 1.0f; }
    return (float)(now_ms - start_ms) / (float)(end_ms - start_ms);
}

/* f(t) = 1 - (1-t)^3 */
static inline float splash_ease_out_cubic(float t)
{
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

/* f(t) = 1 + 2.7*(t-1)^3 + 1.7*(t-1)^2  (spec coefficients; f(0)=0, f(1)=1,
 * overshoots to ~1.0998 at t ~= 0.58) */
static inline float splash_ease_out_back(float t)
{
    float u = t - 1.0f;
    return 1.0f + 2.7f * u * u * u + 1.7f * u * u;
}

/* Eased progress -> 0..255 alpha for COLOR_A. */
static inline uint8_t splash_alpha_from(float eased)
{
    if (eased <= 0.0f) { return 0U; }
    if (eased >= 1.0f) { return 255U; }
    return (uint8_t)(eased * 255.0f + 0.5f);
}

/* Linear interpolation helper on an eased 0..1 value. */
static inline float splash_lerp(float from, float to, float eased)
{
    return from + (to - from) * eased;
}

/* ---- per-element state at now_ms ---- */

static inline float splash_bars_eased(uint32_t now_ms)
{
    return splash_ease_out_cubic(splash_progress(now_ms, SPLASH_BARS_START, SPLASH_BARS_END));
}

static inline int16_t splash_bar_left_x(uint32_t now_ms)
{
    float x = splash_lerp((float)SPLASH_BAR_LEFT_X_FROM, (float)SPLASH_BAR_LEFT_X_TO,
                          splash_bars_eased(now_ms));
    return (int16_t)(x + (x >= 0.0f ? 0.5f : -0.5f));
}

static inline int16_t splash_bar_right_x(uint32_t now_ms)
{
    float x = splash_lerp((float)SPLASH_BAR_RIGHT_X_FROM, (float)SPLASH_BAR_RIGHT_X_TO,
                          splash_bars_eased(now_ms));
    return (int16_t)(x + 0.5f);
}

static inline uint8_t splash_bars_alpha(uint32_t now_ms)
{
    return splash_alpha_from(splash_bars_eased(now_ms));
}

static inline float splash_emblem_scale(uint32_t now_ms)
{
    float eased = splash_ease_out_back(splash_progress(now_ms, SPLASH_EMBLEM_START, SPLASH_EMBLEM_END));
    return splash_lerp(SPLASH_EMBLEM_SCALE_FROM, SPLASH_EMBLEM_SCALE_TO, eased);
}

static inline uint8_t splash_emblem_alpha(uint32_t now_ms)
{
    return splash_alpha_from(splash_ease_out_cubic(
        splash_progress(now_ms, SPLASH_EMBLEM_START, SPLASH_EMBLEM_END)));
}

/* Wordmark + year rise together: offset below final position, +26 px -> 0. */
static inline int16_t splash_word_dy(uint32_t now_ms)
{
    float eased = splash_ease_out_cubic(splash_progress(now_ms, SPLASH_WORD_START, SPLASH_WORD_END));
    return (int16_t)(splash_lerp((float)SPLASH_WORD_RISE_PX, 0.0f, eased) + 0.5f);
}

static inline uint8_t splash_word_alpha(uint32_t now_ms)
{
    return splash_alpha_from(splash_ease_out_cubic(
        splash_progress(now_ms, SPLASH_WORD_START, SPLASH_WORD_END)));
}

/* Accent line reveal, 0..1 of its full width, expanding about center. */
static inline float splash_line_reveal(uint32_t now_ms)
{
    return splash_ease_out_cubic(splash_progress(now_ms, SPLASH_LINE_START, SPLASH_LINE_END));
}

/* Splash complete (hold finished)? */
static inline int splash_done(uint32_t now_ms)
{
    return now_ms >= SPLASH_DURATION_MS;
}

#endif /* SPLASH_TIMELINE_H */
