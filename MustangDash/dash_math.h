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
/* Oil temperature is judged on TRACK figures, not street ones. A 500+ whp NA
 * V8 on a road course lives at 250-280 F, which the old 235/248 pair called
 * amber and then alarm-worthy -- it would have left the gauge permanently
 * amber and fired the full-screen takeover every session. 270/290 is where
 * this car is genuinely in trouble. dash_sim.h's session lands at ~255 F, so
 * a normal session sits clear of amber with room to spare (owner decision,
 * follow-up to U8); the pair is consumed by dash_oil_temp_state, the OILT
 * telltale lamp, dash_alarm_classify's takeover, and the alarm banner's
 * threshold readout. */
#define DASH_OILT_AMBER_F   270.0f  /* amber above */
#define DASH_OILT_RED_F     290.0f  /* red above */
#define DASH_OILP_RED_PSI    29.0f  /* red below (no amber) */
#define DASH_VOLTS_RED_V     12.0f  /* red below (no amber) */
#define DASH_FUEL_AMBER_GAL   2.5f  /* amber below (amber only) */
#define DASH_FUELP_RED_PSI   43.0f  /* red below (no amber) */
#define DASH_IAT_AMBER_F    131.0f  /* amber above (amber only) */
#define DASH_AFR_AMBER        13.8f /* amber above -- lean under load (amber only) */

/* ---- derivation constants (README ~L103) ---- */
#define DASH_RANGE_MPG        16.0f  /* range estimate = gal * 16 mi/gal (street) */
/* laps-remaining = usable fuel / per-lap burn (track). The design doc states
 * the formula but not a figure.
 *
 * This figure and dash_sim.h's fuel depletion are now intended to AGREE, and
 * that is a change: the constant used to be deliberately decoupled, because the
 * simulator burned a demo-scaled 1.2 gal/hr that no realistic per-lap number
 * could have matched. U9 replaced that with a load-proportional burn measuring
 * 0.589 gal over an HPR lap, so the decoupling is retired -- this is the
 * measured figure, rounded. LAPS on the timing screen now counts down in step
 * with the laps the car actually drives (12 gal / 0.59 = ~20 laps), and if the
 * two ever drift apart again that is a defect rather than a design choice. */
#define DASH_LAP_BURN_GAL      0.59f

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

/* Clamp v into [lo, hi]. */
static inline float dash_clampf(float v, float lo, float hi)
{
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

/* Speedo needle fraction 0..1 with the mock's knee mapping: linear to
 * 0.8235 at 140 mph, then a shallower 0.0588-per-20-mph segment to 200. */
static inline float dash_speed_frac(float mph)
{
    float f;
    mph = dash_clampf(mph, 0.0f, (float)DASH_SPEED_MAX);
    if (mph <= (float)DASH_SPEED_KNEE)
    {
        f = (mph / (float)DASH_SPEED_KNEE) * 0.8235f;
    }
    else
    {
        f = 0.8235f + ((mph - (float)DASH_SPEED_KNEE) / 20.0f) * 0.0588f;
    }
    return dash_clampf(f, 0.0f, 1.0f);
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

static inline DashColorState dash_fuelp_state(float psi)
{
    return (psi < DASH_FUELP_RED_PSI) ? DASH_COLOR_RED : DASH_COLOR_NORMAL;
}

static inline DashColorState dash_iat_state(float f)
{
    return (f > DASH_IAT_AMBER_F) ? DASH_COLOR_AMBER : DASH_COLOR_NORMAL;
}

static inline DashColorState dash_afr_state(float afr)
{
    return (afr > DASH_AFR_AMBER) ? DASH_COLOR_AMBER : DASH_COLOR_NORMAL;
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

/* "--" when invalid, else the value at 0 or 1 decimals (house rounding).
 * The one numeric dead-front formatter for every plain cell readout on any
 * screen; deliberate exceptions keep their own snprintf (zero-padded LAP,
 * "P"-prefixed POS, floored LAPS, h:mm TIME). Rounding is half-away-from-
 * zero on both signs: the old (int)(v + 0.5f) idiom truncated negatives
 * toward zero (-10.6 -> "-10"), and AMB/IAT range below 0 F. */
static inline void dash_fmt_value(char *buf, size_t n, float v, uint8_t decimals, bool ok)
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
        snprintf(buf, n, "%d", (int)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f)));
    }
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

/* Session clock as MM:SS, never decimal minutes -- "12.4 MIN" is a number a
 * driver has to convert; "12:24" is one they read. Seconds truncate rather
 * than round, so the display never shows a second the session has not reached.
 * MM is not clamped to 59: the session legitimately runs past 20:00 while the
 * in-lap finishes (dash_sim.h U8), and 12 bytes holds any minute count the
 * uint32 ms can express. */
static inline void dash_fmt_mmss(uint32_t ms, bool valid, char out[12])
{
    if (!valid)
    {
        snprintf(out, 12, "--:--");
        return;
    }
    snprintf(out, 12, "%02u:%02u",
             (unsigned)(ms / 60000U),
             (unsigned)((ms / 1000U) % 60U));
}

/* ---- lap-crossing delta override (MoTeC's shipped pattern) ----
 *
 * On crossing start/finish the LAP TIME readout is OVERWRITTEN for a few
 * seconds with the just-completed lap's delta versus the PREVIOUS lap, then
 * reverts to the running lap clock. This is not an invented flash zone: the
 * MoTeC C127 manual (p.47) describes exactly this -- a numeric field showing a
 * channel value plus "override" values that "display each time their value is
 * updated... shown for a programmable period of time".
 *
 * The reference is the PREVIOUS lap, not the best. AiM ships "Previous lap" as
 * a selectable reference, Bosch's at-the-line delta computes against the last
 * laptime, and Ferrari describes the 296 GT3 dash as showing "current and
 * previous lap". Best-lap comparison already lives in the DELTA bar below it,
 * so referencing best here would say the same thing twice.
 *
 * All of the decision logic lives in this header rather than in the renderer,
 * because every rule in it is a behavior claim that must be testable without a
 * display: the out-lap suppression, the vs-previous arithmetic, the new-best
 * detection, the hold, and the taint gate. dash_render.h only picks a color and
 * draws the string this produces. */

/* The hold is a HAZARD, not a free parameter. AiM's published guidance is that
 * beyond roughly 7-10 s the hold starts covering the next lap's predictive
 * data -- the driver is already braking for T1 while the dash is still talking
 * about the lap before. 4 s is long enough to read at speed and short enough to
 * be gone before the first braking zone. Do not raise it. */
#define DASH_LAP_FLASH_MS 4000U

/* The first lap that may flash, 1-indexed. Lap 1 is the standing-start out-lap
 * and lap 2 vs lap 1 would read as a fake ~18 s improvement; the first honest
 * comparison is flying lap vs flying lap. This is the SAME out-lap exclusion
 * best_ms and the U5 delta reference already apply in dash_sim.h -- one lap
 * later, because this rule needs two representative laps rather than one. */
#define DASH_LAP_FLASH_MIN_LAP 3U

/* Half-period of the new-best alternation. ~2 Hz gives eight beats across the
 * 4 s hold: enough to register as an event, slow enough that the number is
 * never a blur. The value alternates COLOR rather than blinking on and off, so
 * it stays legible in every phase. */
#define DASH_LAP_FLASH_BLINK_HALF_MS 250U

typedef enum {
    DASH_LAPFLASH_NONE = 0, /* no override: draw the running lap clock */
    DASH_LAPFLASH_QUICKER,  /* quicker than the previous lap */
    DASH_LAPFLASH_SLOWER,   /* slower than the previous lap */
    DASH_LAPFLASH_BEST,     /* ...and it is a new best lap */
} DashLapFlashKind;

typedef struct {
    uint32_t lap_n;        /* LAPN as of the last update; 0 = nothing seen yet */
    uint32_t prev_last_ms; /* the lap completed BEFORE the one now in LAST */
    uint32_t prev_best_ms; /* BEST as of the last update */
    bool prev_best_ok;     /* ...and whether it was valid */
    bool prev_tainted;     /* the lap in prev_last_ms was not purely model-driven */
    uint32_t start_ms;     /* when the current override began */
    float delta_s;         /* completed lap minus the lap before it */
    DashLapFlashKind kind; /* NONE when no override is armed */
} DashLapFlash;

static inline void dash_lap_flash_reset(DashLapFlash *f)
{
    const DashLapFlash zero = {0, 0U, 0U, false, false, 0U, 0.0f, DASH_LAPFLASH_NONE};
    *f = zero;
}

static inline DashLapFlashKind dash_lap_flash_kind(const DashLapFlash *f)
{
    return f->kind;
}

/* True on the "highlight" phase of the new-best alternation. Defined ahead of
 * dash_lap_flash_text because that function calls it. */
static inline bool dash_lap_flash_blink(const DashLapFlash *f, uint32_t now_ms)
{
    return dash_flash_phase((uint32_t)(now_ms - f->start_ms),
                            (uint16_t)DASH_LAP_FLASH_BLINK_HALF_MS);
}

/* The override's text: signed, two decimals, seconds.
 *
 * On a new best the word BEST! alternates with the delta on the same 2 Hz
 * phase the colour uses, so the driver gets the event AND the number inside
 * one 4 s hold. DF_MID carries only "0123456789:.-+!BEST"
 * (tools/make_dash_fonts.py) -- those five letters were added for exactly
 * this, and no other word is renderable in this font. Anything else would
 * print blank cells. */
static inline void dash_lap_flash_text(const DashLapFlash *f, uint32_t now_ms,
                                       char out[16])
{
    if ((DASH_LAPFLASH_BEST == f->kind) && dash_lap_flash_blink(f, now_ms))
    {
        snprintf(out, 16, "BEST!");
    }
    else
    {
        snprintf(out, 16, "%+.2f", (double)f->delta_s);
    }
}

/* Drive the override. Call once per frame with the current millis().
 *
 * `last_lap_tainted` is the simulator's sticky flag for the lap that just
 * ENDED (dash_sim.h's last_lap_tainted). A lap driven at a forced SPEED, or one
 * begun by a circuit switch, has a real elapsed time over a fabricated
 * distance -- dash_sim.h already refuses to let such a lap become best_ms or
 * the delta reference, and the same reasoning applies here twice over: the
 * tainted lap must not flash a fabricated gain, AND the lap after it must not
 * flash a fabricated loss against that same time. So the taint is carried
 * forward one lap alongside the time it belongs to. */
static inline void dash_lap_flash_update(DashLapFlash *f, const DashState *s,
                                         uint32_t now_ms, bool last_lap_tainted)
{
    /* Expire first, in wrap-safe unsigned arithmetic. */
    if ((f->kind != DASH_LAPFLASH_NONE)
        && ((uint32_t)(now_ms - f->start_ms) >= DASH_LAP_FLASH_MS))
    {
        f->kind = DASH_LAPFLASH_NONE;
    }

    /* Off the lap path entirely -- STREET, or the SWEEP fixture, where LAPN is
     * dead-fronted. There is no lap to compare, and the lap book the next TRACK
     * lap would be measured against is stale the moment lap timing stops, so
     * the whole state goes rather than just the override. */
    if (!dash_ch_valid(s, DASH_CH_LAPN))
    {
        dash_lap_flash_reset(f);
        return;
    }

    const uint32_t lap_n = s->ch.lap_n;
    const bool last_ok = dash_ch_valid(s, DASH_CH_LAST);
    const uint32_t last_ms = last_ok ? s->ch.last_ms : 0U;
    const bool best_ok = dash_ch_valid(s, DASH_CH_BEST);
    const uint32_t best_ms = best_ok ? s->ch.best_ms : 0U;

    if ((f->lap_n != 0U) && (lap_n > f->lap_n))
    {
        /* A lap just completed: the lap NUMBER advancing is the crossing, and
         * LAST/BEST already hold the new lap's figures by the time this runs. */
        const uint32_t done_n = lap_n - 1U; /* the lap that finished, 1-indexed */
        if (last_ok
            && (done_n >= DASH_LAP_FLASH_MIN_LAP)
            && (f->prev_last_ms > 0U)
            && !last_lap_tainted && !f->prev_tainted)
        {
            f->delta_s = ((float)last_ms - (float)f->prev_last_ms) * 0.001f;
            /* A new best is BEST having MOVED to this lap's time -- not merely
             * matching it, which an equalled lap would also do. */
            const bool new_best = best_ok && (best_ms == last_ms)
                                  && (!f->prev_best_ok || (best_ms < f->prev_best_ms));
            f->kind = new_best
                          ? DASH_LAPFLASH_BEST
                          : ((f->delta_s < 0.0f) ? DASH_LAPFLASH_QUICKER
                                                 : DASH_LAPFLASH_SLOWER);
            f->start_ms = now_ms;
        }
        f->prev_last_ms = last_ms;
        f->prev_tainted = last_lap_tainted;
    }
    else if (lap_n < f->lap_n)
    {
        /* LAPN went backwards: the 20-minute session rollover restarted the lap
         * book. No comparison may cross that boundary -- the new session opens
         * on another cold out-lap, and its lap 1 is not a successor to the old
         * session's last lap. */
        f->prev_last_ms = 0U;
        f->prev_tainted = false;
        f->kind = DASH_LAPFLASH_NONE;
    }
    else if (f->lap_n == 0U)
    {
        /* First sight of the lap path: adopt LAST as history without flashing
         * it. Nothing was witnessed crossing the line. */
        f->prev_last_ms = last_ms;
        f->prev_tainted = last_lap_tainted;
    }

    f->lap_n = lap_n;
    f->prev_best_ok = best_ok;
    f->prev_best_ms = best_ms;
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

/* ---- fuel derivations (README ~L103, KTD5) ----
 * Both return false ("not computable") and leave *out at 0 when the fuel
 * channel is invalid -- the dead-front convention (R7): callers must check
 * the return value before rendering, same discipline as dash_fmt_lap's
 * explicit `valid` parameter. */

/* range estimate = gal * 16 mi/gal (street). */
static inline bool dash_range_mi(float fuel_gal, bool fuel_valid, float *out_mi)
{
    if (!fuel_valid)
    {
        *out_mi = 0.0f;
        return false;
    }
    *out_mi = fuel_gal * DASH_RANGE_MPG;
    return true;
}

/* laps-remaining = usable fuel / per-lap burn (track). */
static inline bool dash_laps_remaining(float fuel_gal, bool fuel_valid, float *out_laps)
{
    if (!fuel_valid)
    {
        *out_laps = 0.0f;
        return false;
    }
    *out_laps = fuel_gal / DASH_LAP_BURN_GAL;
    return true;
}

#endif /* DASH_MATH_H */
