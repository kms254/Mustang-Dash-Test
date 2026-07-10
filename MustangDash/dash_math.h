/*
 * dash_math.h - pure gauge/threshold/format math for the dash renderer.
 *
 * Every number a gauge, shift light, telltale, or alarm banner shows is
 * computed here so it is host-testable (see tests/test_dash_math.c) before
 * any rendering exists -- the splash_timeline.h pattern. No Arduino or EVE
 * dependencies; only dash_data.h (channel ids / DashState) and libc.
 *
 * Units are display units throughout: mph, degF, psi, volts, gallons,
 * seconds (lap delta), milliseconds (lap times).
 */

#ifndef DASH_MATH_H
#define DASH_MATH_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>

#include "dash_data.h"

/* ---- vehicle / spec constants ---- */
#define DASH_REDLINE_RPM      8000
#define DASH_SHIFT_RPM        7600  /* red / flash strictly above this */
#define DASH_AMBER_RPM        7100
#define DASH_SHIFT_LED_COUNT    15
#define DASH_GAUGE_SWEEP_DEG   240
#define DASH_GAUGE_START_DEG   210  /* frac 0 at 210 deg, frac 1 at -30 deg */
#define DASH_SPEED_MAX         200
#define DASH_SPEED_KNEE        140

/* flash half-periods (square wave, see dash_flash_phase) */
#define DASH_SHIFT_FLASH_HALF_MS  62U  /* ~8 Hz */
#define DASH_ALARM_FLASH_HALF_MS 179U  /* ~2.8 Hz */

/* The oil-pressure ALARM is meaningless with the engine off -- every real
 * cluster suppresses it below cranking speed, or key-on before start would
 * be a permanent takeover. The OIL telltale dot stays ungated (design
 * spec); only the full-screen alarm requires a running engine. */
#define DASH_ENGINE_RUNNING_RPM 500.0f

/* ---- warning thresholds (README spec, display units) ---- */
#define DASH_ECT_AMBER_F    210.0f  /* amber above */
#define DASH_ECT_RED_F      217.0f  /* red above */
#define DASH_OILT_AMBER_F   235.0f  /* amber above */
#define DASH_OILT_RED_F     248.0f  /* red above */
#define DASH_OILP_RED_PSI    29.0f  /* red below (no amber) */
#define DASH_VOLTS_RED_V     12.0f  /* red below (no amber) */
#define DASH_FUEL_AMBER_GAL   2.5f  /* amber below (amber only) */

#define DASH_DEG_TO_RAD (3.14159265358979323846f / 180.0f)

typedef enum {
    DASH_COLOR_NORMAL,
    DASH_COLOR_AMBER,
    DASH_COLOR_RED,
} DashColorState;

typedef enum {
    DASH_ALARM_NONE = 0,
    DASH_ALARM_OILP,
    DASH_ALARM_OILT,
    DASH_ALARM_CLT,
} DashAlarm;

/* Speedo needle fraction 0..1 with the mock's knee mapping: linear to
 * 0.8235 at 140 mph, then a shallower 0.0588-per-20-mph segment to 200. */
static inline float dash_speed_frac(float mph)
{
    float f;
    if (mph < 0.0f) { mph = 0.0f; }
    if (mph > (float)DASH_SPEED_MAX) { mph = (float)DASH_SPEED_MAX; }
    if (mph <= (float)DASH_SPEED_KNEE)
    {
        f = (mph / (float)DASH_SPEED_KNEE) * 0.8235f;
    }
    else
    {
        f = 0.8235f + ((mph - (float)DASH_SPEED_KNEE) / 20.0f) * 0.0588f;
    }
    if (f < 0.0f) { f = 0.0f; }
    if (f > 1.0f) { f = 1.0f; }
    return f;
}

/* Needle angle in radians: 210 deg at frac 0, sweeping 240 deg clockwise
 * to -30 deg at frac 1 (standard math angles, x right / y up). */
static inline float dash_gauge_angle_rad(float frac)
{
    return ((float)DASH_GAUGE_START_DEG - frac * (float)DASH_GAUGE_SWEEP_DEG)
           * DASH_DEG_TO_RAD;
}

/* Point on a gauge arc in screen coordinates (y down):
 * (cx + r*cos a, cy - r*sin a), matching the design's needle math. */
static inline void dash_arc_point(float cx, float cy, float r, float frac,
                                  float *x, float *y)
{
    float a = dash_gauge_angle_rad(frac);
    *x = cx + r * cosf(a);
    *y = cy - r * sinf(a);
}

/* Shift ladder: LED i (1-based) lights when rpm >= i * 8000/15. */
static inline uint8_t dash_shift_led_count(float rpm)
{
    uint8_t n = 0U;
    for (uint8_t i = 1U; i <= DASH_SHIFT_LED_COUNT; i++)
    {
        if (rpm >= (float)i * ((float)DASH_REDLINE_RPM / (float)DASH_SHIFT_LED_COUNT))
        {
            n = i;
        }
    }
    return n;
}

/* Whole ladder flashes strictly above the shift point. */
static inline bool dash_shift_flash_zone(float rpm)
{
    return rpm > (float)DASH_SHIFT_RPM;
}

/* Square wave: true for the first half_period_ms of every full period.
 * Shift lights use DASH_SHIFT_FLASH_HALF_MS, alarms DASH_ALARM_FLASH_HALF_MS. */
static inline bool dash_flash_phase(uint32_t ms, uint16_t half_period_ms)
{
    if (half_period_ms == 0U) { return true; }
    return (ms / half_period_ms) % 2U == 0U;
}

/* Tach color: red strictly above the shift point, amber from 7100 up. */
static inline DashColorState dash_rpm_color(float rpm)
{
    if (rpm > (float)DASH_SHIFT_RPM) { return DASH_COLOR_RED; }
    if (rpm >= (float)DASH_AMBER_RPM) { return DASH_COLOR_AMBER; }
    return DASH_COLOR_NORMAL;
}

/* ---- per-channel threshold classifiers ---- */

static inline DashColorState dash_ect_state(float f)
{
    if (f > DASH_ECT_RED_F) { return DASH_COLOR_RED; }
    if (f > DASH_ECT_AMBER_F) { return DASH_COLOR_AMBER; }
    return DASH_COLOR_NORMAL;
}

static inline DashColorState dash_oil_temp_state(float f)
{
    if (f > DASH_OILT_RED_F) { return DASH_COLOR_RED; }
    if (f > DASH_OILT_AMBER_F) { return DASH_COLOR_AMBER; }
    return DASH_COLOR_NORMAL;
}

static inline DashColorState dash_oil_press_state(float psi)
{
    return (psi < DASH_OILP_RED_PSI) ? DASH_COLOR_RED : DASH_COLOR_NORMAL;
}

static inline DashColorState dash_volts_state(float v)
{
    return (v < DASH_VOLTS_RED_V) ? DASH_COLOR_RED : DASH_COLOR_NORMAL;
}

static inline DashColorState dash_fuel_state(float gal)
{
    return (gal < DASH_FUEL_AMBER_GAL) ? DASH_COLOR_AMBER : DASH_COLOR_NORMAL;
}

/* Oil telltale: low pressure or overheated oil -- but an invalid channel
 * can never assert it (R11). Delegates to the per-channel classifiers so
 * each threshold has exactly one definition. */
static inline bool dash_telltale_oil(float oil_press_psi, bool oilp_valid,
                                     float oil_temp_f, bool oilt_valid)
{
    return (oilp_valid && (DASH_COLOR_RED == dash_oil_press_state(oil_press_psi)))
        || (oilt_valid && (DASH_COLOR_RED == dash_oil_temp_state(oil_temp_f)));
}

/* Highest-priority active alarm, checking ONLY valid channels (R11):
 * oil pressure > oil temperature > coolant. */
static inline DashAlarm dash_alarm_classify(const DashState *s)
{
    const bool engine_running = dash_ch_valid(s, DASH_CH_RPM)
        && (s->ch.rpm >= DASH_ENGINE_RUNNING_RPM);
    if (engine_running
        && dash_ch_valid(s, DASH_CH_OILP) && s->ch.oil_press_psi < DASH_OILP_RED_PSI)
    {
        return DASH_ALARM_OILP;
    }
    if (dash_ch_valid(s, DASH_CH_OILT) && s->ch.oil_temp_f > DASH_OILT_RED_F)
    {
        return DASH_ALARM_OILT;
    }
    if (dash_ch_valid(s, DASH_CH_ECT) && s->ch.ect_f > DASH_ECT_RED_F)
    {
        return DASH_ALARM_CLT;
    }
    return DASH_ALARM_NONE;
}

/* Lap-delta bar fill as a signed fraction of the half-width, clamped to
 * [-1, +1]; negative = ahead of best = fills left / green. */
static inline float dash_delta_fill(float delta_s)
{
    if (delta_s < -1.0f) { return -1.0f; }
    if (delta_s > 1.0f) { return 1.0f; }
    return delta_s;
}

/* "m:ss.mmm" (minutes unpadded), or "--:--.---" when the time is invalid. */
static inline void dash_fmt_lap(uint32_t ms, bool valid, char out[16])
{
    if (!valid)
    {
        snprintf(out, 16, "--:--.---");
        return;
    }
    snprintf(out, 16, "%u:%02u.%03u",
             (unsigned)(ms / 60000U),
             (unsigned)((ms / 1000U) % 60U),
             (unsigned)(ms % 1000U));
}

/* Odometer integration with integer remainder carry. Distance this step is
 * mph * dt_ms / 3.6 micro-miles (mph * dt_ms / 3600000 miles), and one
 * tenth-mile increment is 100000 micro-miles. Returns the number of whole
 * tenths to add to the stored odometer -- usually 0.
 *
 * Drift note (test-pinned): truncating micro-miles per step loses up to a
 * micro-mile every call (60 mph at 10 Hz is 1666.67 -> 1666), so N small
 * steps would undercount one big step. The carry therefore stays in raw
 * mph*ms units -- integer-exact per step -- and tenths are extracted at
 * 3.6 * 100000 = 360000 mph*ms per tenth. */
#define DASH_ODO_UMS_PER_TENTH 360000U /* mph*ms per tenth mile */
static inline uint32_t dash_odo_step(float mph, uint32_t dt_ms, uint32_t *remainder_ums)
{
    uint32_t tenths;
    if (mph < 0.0f) { mph = 0.0f; }
    *remainder_ums += (uint32_t)(mph * (float)dt_ms + 0.5f);
    tenths = *remainder_ums / DASH_ODO_UMS_PER_TENTH;
    *remainder_ums -= tenths * DASH_ODO_UMS_PER_TENTH;
    return tenths;
}

#endif /* DASH_MATH_H */
