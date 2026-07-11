/*
 * dash_sim.h -- deterministic dash simulator behind the data interface
 * (plan KTD5, R12).
 *
 * Pure header: stdint/math only, no Arduino/EVE includes, host-testable
 * (see tests/test_dash_sim.c). Produces realistic-looking values in display
 * units (MPH, degF, psi, gallons, volts) with NO libc rand() and NO time():
 * everything derives from an internal LCG plus accumulated sim time, so two
 * sims stepped identically are bit-for-bit identical.
 *
 * Ownership rules (KTD6): every channel write goes through dash_ch_set()
 * and only when dash_ch_sim_owned() says the sim still owns that channel --
 * serial `set` (override) and `clear` (held-invalid) freeze a channel
 * against the sim. s->sim_frozen stops stepping entirely (no time accrual).
 *
 * The mock this replaces ticked at 60 ms; all rates below are scaled by
 * dt_ms so any step size integrates to the same per-second behavior.
 */

#ifndef DASH_SIM_H
#define DASH_SIM_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "dash_data.h"

/* ---- vehicle constants (do NOT include dash_math.h; SIM_-local) ---- */
/* MPH = rpm * tire_dia_in / (gear_ratio * final_drive * 336) */
#define SIM_TIRE_DIA_IN     26.0f
#define SIM_FINAL_DRIVE     3.73f
#define SIM_MPH_CONST       336.0f
static const float SIM_GEAR_RATIOS[6] = { 2.66f, 1.78f, 1.30f, 1.00f, 0.80f, 0.63f }; /* T56 */

/* ---- powertrain / driving-cycle tuning ----
 * TRACK is a lap-synced driving cycle: the lap position selects a segment
 * of the circuit profile below, speed chases that segment's target with
 * power-limited acceleration and race braking, and the gearbox follows the
 * wheel speed with hysteresis. RPM is always derived from speed THROUGH
 * the current gear, so the same road speed appears at very different rpm
 * depending on where on the lap (and in which gear) the car is -- no more
 * pegged 6200 rpm / 204 mph cruise. */
#define SIM_RPM_START         950.0f  /* rolling out of the pits */
#define SIM_RPM_IDLE          850.0f  /* rpm floor */
#define SIM_RPM_MAX           7600.0f /* brushes the 7600 shift point, under the 8000 redline */
#define SIM_UPSHIFT_RPM       7400.0f /* pull the next gear above this */
#define SIM_DOWNSHIFT_RPM     3300.0f /* consider a lower gear below this... */
#define SIM_DOWNSHIFT_MAX_RPM 6900.0f /* ...if it lands the revs below this */
#define SIM_ACCEL_BASE_MPHPS  18.0f   /* launch acceleration, mph per second */
#define SIM_ACCEL_FADE        0.062f  /* aero/power fade per mph of speed */
#define SIM_ACCEL_MIN_MPHPS   2.2f    /* top-end crawl toward vmax */
#define SIM_BRAKE_MPHPS       30.0f   /* race braking, mph shed per second */
#define SIM_SEG_JITTER_MPH    9u      /* per-visit target jitter: 0..8 - 4 mph */
#define SIM_SEG_COUNT         10u

/* ---- lap timing ---- */
#define SIM_LAP_ROLLOVER_MS 62000u

/* ---- temperatures / pressures / electrics ---- */
#define SIM_COLD_START_F    75.0f    /* ambient-ish warm-start value */
#define SIM_WARMUP_TAU_S    90.0f    /* exponential-approach time constant */
#define SIM_ECT_OPERATING_F 195.0f
#define SIM_OILT_OPERATING_F 220.0f
/* oilp = base + rpm/8000*40 + 2*sin: base 25 keeps normal running (any
 * rpm above ~800) clear of the 29 psi alarm threshold -- no flapping
 * alarms at cruise-idle dips -- and tops out ~67 psi at redline. Forcing
 * the alarm on the bench is `set oilp 25` / `alarm oilp`. */
#define SIM_OILP_BASE_PSI   25.0f
#define SIM_OILP_RPM_SPAN   40.0f
#define SIM_VOLTS_NOMINAL   13.6f

/* ---- fuel ---- */
#define SIM_FUEL_START_GAL  12.0f
#define SIM_FUEL_BURN_GPS   (0.02f / 60.0f) /* gal per second at track pace */
#define SIM_FUEL_STREET_DIV 40.0f           /* street burns 40x slower */

/* ---- Phase-2 channels (plan KTD5): engine + PMU sensors, always published
 * (both modes, like ECT/OILT/OILP/VOLTS above); lap-timing channels
 * (lapn/pos/pred/throttle/brake) are TRACK-only, mirroring the existing
 * DELTA/LAP/LAST/BEST gating. ---- */
#define SIM_REDLINE_RPM      8000.0f /* SIM_-local copy; dash_sim.h does not include dash_math.h */
#define SIM_AFR_BASE          13.5f  /* leaner at idle */
#define SIM_AFR_LOAD_SPAN      2.0f  /* richer (lower AFR) toward redline */
#define SIM_IAT_HEATSOAK_F    25.0f  /* intake runs hotter than ambient */
#define SIM_FUELP_BASE_PSI    46.5f
#define SIM_FUELP_WANDER_PSI   3.5f
#define SIM_PUMP_BASE_A        8.0f
#define SIM_FAN_ON_A           12.0f
#define SIM_FAN1_PERIOD_S      20.0f
#define SIM_FAN1_ON_S          12.0f
#define SIM_FAN2_PERIOD_S      25.0f
#define SIM_FAN2_ON_S          10.0f
#define SIM_FAN2_PHASE_S       10.0f
#define SIM_MIN_PER_DAY      1440.0f

/* ---- deterministic jitter ---- */
#define SIM_LCG_SEED        0x4D757354u /* "MusT" */

typedef struct {
    uint8_t gear;       /* 1..6 */
    float t_s;          /* accumulated sim time, seconds */
    uint32_t lcg;       /* deterministic jitter state */
    uint32_t lap_ms;    /* current lap accumulator */
    uint32_t lap_count; /* completed laps */
    /* internal continuous state */
    float rpm;
    float speed_mph;    /* driving-cycle integrator state */
    uint8_t seg_idx;    /* current circuit segment (0xFF = force refresh) */
    float seg_target;   /* jittered target speed for the current segment */
    float ect_f;
    float oilt_f;
    float fuel_gal;
    uint32_t last_ms;
    uint32_t best_ms;
} DashSimState;

/* lcg = lcg*1664525 + 1013904223; jitter = (lcg>>16) % range */
static inline uint32_t dash_sim_jitter(DashSimState *sim, uint32_t range)
{
    sim->lcg = sim->lcg * 1664525u + 1013904223u;
    return (sim->lcg >> 16) % range;
}

static inline float dash_sim_speed_mph(float rpm, uint8_t gear)
{
    return rpm * SIM_TIRE_DIA_IN
           / (SIM_GEAR_RATIOS[gear - 1] * SIM_FINAL_DRIVE * SIM_MPH_CONST);
}

/* Inverse of dash_sim_speed_mph: engine rpm for a road speed in a gear. */
static inline float dash_sim_rpm_from_speed(float mph, uint8_t gear)
{
    return mph * SIM_GEAR_RATIOS[gear - 1] * SIM_FINAL_DRIVE * SIM_MPH_CONST
           / SIM_TIRE_DIA_IN;
}

/* Gear 1, warm-start temps at ambient-ish cold values, full-ish tank. */
static inline void dash_sim_init(DashSimState *sim)
{
    sim->gear = 1;
    sim->t_s = 0.0f;
    sim->lcg = SIM_LCG_SEED;
    sim->lap_ms = 0u;
    sim->lap_count = 0u;
    sim->rpm = SIM_RPM_START;
    sim->speed_mph = dash_sim_speed_mph(SIM_RPM_START, 1); /* ~7 mph, rolling out */
    sim->seg_idx = 0xFFu; /* force a target refresh on the first step */
    sim->seg_target = 0.0f;
    sim->ect_f = SIM_COLD_START_F;
    sim->oilt_f = SIM_COLD_START_F;
    sim->fuel_gal = SIM_FUEL_START_GAL;
    sim->last_ms = 0u;
    sim->best_ms = 0u;
}

static inline void dash_sim_step(DashSimState *sim, DashState *s, uint32_t dt_ms)
{
    if (s->sim_frozen)
    {
        return; /* `sim off`: no time accrual, no writes */
    }

    float dt_s = (float) dt_ms * 0.001f;
    sim->t_s += dt_s;
    float t = sim->t_s;

    float speed_mph;
    float fuel_burn_gps = SIM_FUEL_BURN_GPS;
    float delta_s = 0.0f;
    bool track = (s->mode == DASH_MODE_TRACK);

    float track_throttle_pct = 0.0f;
    float track_brake_pct = 0.0f;

    if (track)
    {
        /* lap timer first -- the driving cycle below is keyed off lap
         * position, so every lap drives the same circuit (deterministic
         * braking points), with per-visit target jitter for variety */
        sim->lap_ms += dt_ms;
        if (sim->lap_ms > SIM_LAP_ROLLOVER_MS)
        {
            sim->last_ms = sim->lap_ms;
            if (sim->lap_count == 0u || sim->last_ms < sim->best_ms)
            {
                sim->best_ms = sim->last_ms;
            }
            sim->lap_ms = 0u;
            sim->lap_count++;
        }

        /* the circuit: 10 lap-position segments (fraction-of-lap end,
         * target mph) -- front straight, hairpin, chute, sweepers, back
         * straight, esses, final complex onto the straight */
        static const struct { float frac_end; float target_mph; } SIM_SEGS[SIM_SEG_COUNT] = {
            { 0.19f, 196.0f }, /* front straight (long enough to pull 6th) */
            { 0.27f,  64.0f }, /* T1 hairpin */
            { 0.36f, 124.0f }, /* chute */
            { 0.42f,  86.0f }, /* T3 */
            { 0.56f, 178.0f }, /* back straight */
            { 0.63f,  94.0f }, /* T5 */
            { 0.72f, 140.0f }, /* esses */
            { 0.79f,  70.0f }, /* T7 tight */
            { 0.90f, 156.0f }, /* kink run */
            { 1.01f, 112.0f }, /* final complex onto the straight */
        };
        const float lap_frac = (float) sim->lap_ms / (float) SIM_LAP_ROLLOVER_MS;
        uint8_t si = 0u;
        while ((si < (SIM_SEG_COUNT - 1u)) && (lap_frac > SIM_SEGS[si].frac_end))
        {
            si++;
        }
        if (si != sim->seg_idx)
        {
            sim->seg_idx = si;
            sim->seg_target = SIM_SEGS[si].target_mph
                              + (float) dash_sim_jitter(sim, SIM_SEG_JITTER_MPH) - 4.0f;
        }

        /* chase the segment target: power-limited accel, race braking */
        float accel = SIM_ACCEL_BASE_MPHPS - SIM_ACCEL_FADE * sim->speed_mph;
        if (accel < SIM_ACCEL_MIN_MPHPS)
        {
            accel = SIM_ACCEL_MIN_MPHPS;
        }
        if (sim->speed_mph < (sim->seg_target - 0.5f))
        {
            sim->speed_mph += accel * dt_s;
            if (sim->speed_mph > sim->seg_target) { sim->speed_mph = sim->seg_target; }
            track_throttle_pct = 62.0f + 38.0f * (accel / SIM_ACCEL_BASE_MPHPS);
            track_brake_pct = 0.0f;
        }
        else if (sim->speed_mph > (sim->seg_target + 0.5f))
        {
            sim->speed_mph -= SIM_BRAKE_MPHPS * dt_s;
            if (sim->speed_mph < sim->seg_target) { sim->speed_mph = sim->seg_target; }
            track_throttle_pct = 0.0f;
            track_brake_pct = 82.0f + 8.0f * sinf(t * 3.0f);
        }
        else /* holding a corner / terminal-velocity crawl */
        {
            track_throttle_pct = 24.0f + (sim->rpm / SIM_RPM_MAX) * 22.0f;
            track_brake_pct = 0.0f;
        }

        /* hysteretic gearbox: upshift above SIM_UPSHIFT_RPM; downshift when
         * revs sag AND the lower gear lands safely under the shift point.
         * RPM always derives from wheel speed through the selected gear, so
         * the same road speed reads very different rpm by lap position. */
        const float rpm_now = dash_sim_rpm_from_speed(sim->speed_mph, sim->gear);
        if ((rpm_now > SIM_UPSHIFT_RPM) && (sim->gear < 6u))
        {
            sim->gear++;
        }
        else if ((sim->gear > 1u) && (rpm_now < SIM_DOWNSHIFT_RPM)
                 && (dash_sim_rpm_from_speed(sim->speed_mph, (uint8_t) (sim->gear - 1u))
                     < SIM_DOWNSHIFT_MAX_RPM))
        {
            sim->gear--;
        }
        sim->rpm = dash_sim_rpm_from_speed(sim->speed_mph, sim->gear);
        if (sim->rpm < SIM_RPM_IDLE) { sim->rpm = SIM_RPM_IDLE; }
        if (sim->rpm > SIM_RPM_MAX) { sim->rpm = SIM_RPM_MAX; }
        speed_mph = sim->speed_mph;

        /* lap delta: the mock's two-sine blend, range about +/-0.88 s */
        delta_s = 0.55f * sinf(t * 0.8f) + 0.25f * sinf(t * 0.27f) - 0.08f;
    }
    else
    {
        /* STREET: pinned 6th, gentle cruise; lap values freeze entirely */
        sim->gear = 6;
        sim->rpm = 2500.0f + 450.0f * sinf(t * 0.5f) + (float) dash_sim_jitter(sim, 61u);
        speed_mph = 60.0f + 8.0f * sinf(t * 0.22f);
        fuel_burn_gps = SIM_FUEL_BURN_GPS / SIM_FUEL_STREET_DIV;
    }

    /* temps warm toward operating on a ~90 s time constant, then breathe */
    float warm_k = 1.0f - expf(-dt_s / SIM_WARMUP_TAU_S);
    float ect_target = SIM_ECT_OPERATING_F + 3.0f * sinf(t * 0.15f);
    float oilt_target = SIM_OILT_OPERATING_F + 7.0f * sinf(t * 0.12f + 1.0f);
    sim->ect_f += (ect_target - sim->ect_f) * warm_k;
    sim->oilt_f += (oilt_target - sim->oilt_f) * warm_k;

    float oilp_psi = SIM_OILP_BASE_PSI + (sim->rpm / 8000.0f) * SIM_OILP_RPM_SPAN
                     + 2.0f * sinf(t * 0.9f);
    float volts = SIM_VOLTS_NOMINAL + 0.3f * sinf(t * 0.3f);
    float ambient_f = 75.0f + 2.0f * sinf(t * 0.04f);

    sim->fuel_gal -= fuel_burn_gps * dt_s;
    if (sim->fuel_gal < 0.0f)
    {
        sim->fuel_gal = 0.0f;
    }

    /* AFR: tracks rpm load -- richer (lower AFR) toward redline, small
     * bank-to-bank variation; deterministic wander via sinf(t). */
    float afr_load = SIM_AFR_BASE - (sim->rpm / SIM_REDLINE_RPM) * SIM_AFR_LOAD_SPAN;
    float afr_l = afr_load + 0.3f * sinf(t * 0.7f);
    float afr_r = afr_load - 0.3f * sinf(t * 0.7f + 0.5f);

    /* IAT: ambient plus under-hood heat soak. */
    float iat_f = ambient_f + SIM_IAT_HEATSOAK_F + 8.0f * sinf(t * 0.09f);

    /* fuel pressure: steady with small wander, per README ~43-50 psi. */
    float fuelp_psi = SIM_FUELP_BASE_PSI + SIM_FUELP_WANDER_PSI * sinf(t * 0.15f);

    /* PMU outputs: pump steady, fans duty-cycle on a temperature-ish cadence
     * (independent periods so they are not always in lockstep). */
    float pump_a = SIM_PUMP_BASE_A + 0.3f * sinf(t * 0.2f);
    float fan1_phase = fmodf(t, SIM_FAN1_PERIOD_S);
    float fan1_a = (fan1_phase < SIM_FAN1_ON_S) ? SIM_FAN_ON_A + 0.5f * sinf(t * 1.3f) : 0.0f;
    float fan2_phase = fmodf(t + SIM_FAN2_PHASE_S, SIM_FAN2_PERIOD_S);
    float fan2_a = (fan2_phase < SIM_FAN2_ON_S) ? (SIM_FAN_ON_A - 1.0f) + 0.5f * sinf(t * 1.1f) : 0.0f;

    /* time of day: ~1 minute of clock per minute of sim time, from 00:00. */
    float time_min = fmodf(t / 60.0f, SIM_MIN_PER_DAY);

    /* publish -- only channels the sim still owns (KTD6) */
    if (dash_ch_sim_owned(s, DASH_CH_RPM))     { dash_ch_set(s, DASH_CH_RPM, sim->rpm); }
    if (dash_ch_sim_owned(s, DASH_CH_SPEED))   { dash_ch_set(s, DASH_CH_SPEED, speed_mph); }
    if (dash_ch_sim_owned(s, DASH_CH_ECT))     { dash_ch_set(s, DASH_CH_ECT, sim->ect_f); }
    if (dash_ch_sim_owned(s, DASH_CH_OILT))    { dash_ch_set(s, DASH_CH_OILT, sim->oilt_f); }
    if (dash_ch_sim_owned(s, DASH_CH_OILP))    { dash_ch_set(s, DASH_CH_OILP, oilp_psi); }
    if (dash_ch_sim_owned(s, DASH_CH_VOLTS))   { dash_ch_set(s, DASH_CH_VOLTS, volts); }
    if (dash_ch_sim_owned(s, DASH_CH_FUEL))    { dash_ch_set(s, DASH_CH_FUEL, sim->fuel_gal); }
    if (dash_ch_sim_owned(s, DASH_CH_AMBIENT)) { dash_ch_set(s, DASH_CH_AMBIENT, ambient_f); }

    /* Phase-2 engine/PMU channels -- always published (both modes), like
     * ECT/OILT/OILP/VOLTS above. */
    if (dash_ch_sim_owned(s, DASH_CH_AFR_L))  { dash_ch_set(s, DASH_CH_AFR_L, afr_l); }
    if (dash_ch_sim_owned(s, DASH_CH_AFR_R))  { dash_ch_set(s, DASH_CH_AFR_R, afr_r); }
    if (dash_ch_sim_owned(s, DASH_CH_IAT))    { dash_ch_set(s, DASH_CH_IAT, iat_f); }
    if (dash_ch_sim_owned(s, DASH_CH_FUELP))  { dash_ch_set(s, DASH_CH_FUELP, fuelp_psi); }
    if (dash_ch_sim_owned(s, DASH_CH_PUMP))   { dash_ch_set(s, DASH_CH_PUMP, pump_a); }
    if (dash_ch_sim_owned(s, DASH_CH_FAN1))   { dash_ch_set(s, DASH_CH_FAN1, fan1_a); }
    if (dash_ch_sim_owned(s, DASH_CH_FAN2))   { dash_ch_set(s, DASH_CH_FAN2, fan2_a); }
    if (dash_ch_sim_owned(s, DASH_CH_TIME))   { dash_ch_set(s, DASH_CH_TIME, time_min); }

    if (track)
    {
        if (dash_ch_sim_owned(s, DASH_CH_DELTA)) { dash_ch_set(s, DASH_CH_DELTA, delta_s); }
        if (dash_ch_sim_owned(s, DASH_CH_LAP))   { dash_ch_set(s, DASH_CH_LAP, (float) sim->lap_ms); }
        if (sim->lap_count > 0u) /* before the first lap, LAST/BEST stay --:--.--- */
        {
            if (dash_ch_sim_owned(s, DASH_CH_LAST)) { dash_ch_set(s, DASH_CH_LAST, (float) sim->last_ms); }
            if (dash_ch_sim_owned(s, DASH_CH_BEST)) { dash_ch_set(s, DASH_CH_BEST, (float) sim->best_ms); }

            /* predicted lap: near the last lap, +/-200 ms deterministic jitter */
            {
                int32_t jitter = (int32_t) dash_sim_jitter(sim, 400u) - 200;
                int64_t pred = (int64_t) sim->last_ms + jitter;
                if (pred < 0) { pred = 0; }
                if (dash_ch_sim_owned(s, DASH_CH_PRED)) { dash_ch_set(s, DASH_CH_PRED, (float) pred); }
            }
        }

        /* lap number (1-indexed, current lap) and race position -- position
         * is a small integer that changes occasionally (every other lap),
         * not every tick. */
        if (dash_ch_sim_owned(s, DASH_CH_LAPN)) { dash_ch_set(s, DASH_CH_LAPN, (float) (sim->lap_count + 1u)); }
        if (dash_ch_sim_owned(s, DASH_CH_POS))
        {
            uint32_t pos = 1u + ((sim->lap_count / 2u) % 5u);
            dash_ch_set(s, DASH_CH_POS, (float) pos);
        }

        /* throttle/brake straight from the driving cycle's chase state:
         * full throttle down the straights, hard brake into the corners,
         * part throttle holding a corner. */
        if (dash_ch_sim_owned(s, DASH_CH_THROTTLE)) { dash_ch_set(s, DASH_CH_THROTTLE, track_throttle_pct); }
        if (dash_ch_sim_owned(s, DASH_CH_BRAKE))    { dash_ch_set(s, DASH_CH_BRAKE, track_brake_pct); }
    }
}

#endif /* DASH_SIM_H */
