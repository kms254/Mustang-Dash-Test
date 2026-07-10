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

/* ---- powertrain tuning ---- */
#define SIM_TICK_MS         60.0f    /* the mock's tick; per-tick rates scale by dt/60 */
#define SIM_RPM_START       950.0f   /* rolling out of the pits */
#define SIM_RPM_SHIFT_AT    7900.0f  /* upshift point, gears 1..5 */
#define SIM_RPM_DROP_BASE   5200.0f  /* post-shift rpm = base + jitter(0..249) */
#define SIM_RPM_DROP_JITTER 250u
#define SIM_RPM_RAMP        125.0f   /* rpm gained per 60 ms tick, + jitter(0..80) */
#define SIM_RPM_RAMP_JITTER 81u
/* Top-gear clamp. The spec's 7900 redline in 6th would compute 260 MPH from
 * the T56 0.63 overdrive; the speed contract is [0, 210] MPH, so 6th holds
 * a sustained-straight 6200 rpm (~204 MPH) instead. */
#define SIM_RPM_TOP_CLAMP   6200.0f

/* ---- lap timing ---- */
#define SIM_LAP_ROLLOVER_MS 62000u

/* ---- temperatures / pressures / electrics ---- */
#define SIM_COLD_START_F    75.0f    /* ambient-ish warm-start value */
#define SIM_WARMUP_TAU_S    90.0f    /* exponential-approach time constant */
#define SIM_ECT_OPERATING_F 195.0f
#define SIM_OILT_OPERATING_F 220.0f
/* oilp = base + rpm/8000*45 + 2*sin: base 17.5 keeps it > 29 psi whenever
 * rpm > 2500 (no spurious low-oil alarms) and < 64 psi at redline */
#define SIM_OILP_BASE_PSI   17.5f
#define SIM_OILP_RPM_SPAN   45.0f
#define SIM_VOLTS_NOMINAL   13.6f

/* ---- fuel ---- */
#define SIM_FUEL_START_GAL  12.0f
#define SIM_FUEL_BURN_GPS   (0.02f / 60.0f) /* gal per second at track pace */
#define SIM_FUEL_STREET_DIV 40.0f           /* street burns 40x slower */

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

/* Gear 1, warm-start temps at ambient-ish cold values, full-ish tank. */
static inline void dash_sim_init(DashSimState *sim)
{
    sim->gear = 1;
    sim->t_s = 0.0f;
    sim->lcg = SIM_LCG_SEED;
    sim->lap_ms = 0u;
    sim->lap_count = 0u;
    sim->rpm = SIM_RPM_START;
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
    float ticks = (float) dt_ms / SIM_TICK_MS; /* mock ticks elapsed */
    sim->t_s += dt_s;
    float t = sim->t_s;

    float speed_mph;
    float fuel_burn_gps = SIM_FUEL_BURN_GPS;
    float delta_s = 0.0f;
    bool track = (s->mode == DASH_MODE_TRACK);

    if (track)
    {
        /* rpm climbs, banging up through the box; 6th holds the straight */
        sim->rpm += (SIM_RPM_RAMP + (float) dash_sim_jitter(sim, SIM_RPM_RAMP_JITTER)) * ticks;
        if (sim->rpm >= SIM_RPM_SHIFT_AT && sim->gear < 6)
        {
            sim->gear++;
            sim->rpm = SIM_RPM_DROP_BASE + (float) dash_sim_jitter(sim, SIM_RPM_DROP_JITTER);
        }
        if (sim->gear == 6 && sim->rpm > SIM_RPM_TOP_CLAMP)
        {
            sim->rpm = SIM_RPM_TOP_CLAMP;
        }
        speed_mph = dash_sim_speed_mph(sim->rpm, sim->gear);

        /* lap timer; first completed lap seeds best */
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

    /* publish -- only channels the sim still owns (KTD6) */
    if (dash_ch_sim_owned(s, DASH_CH_RPM))     { dash_ch_set(s, DASH_CH_RPM, sim->rpm); }
    if (dash_ch_sim_owned(s, DASH_CH_SPEED))   { dash_ch_set(s, DASH_CH_SPEED, speed_mph); }
    if (dash_ch_sim_owned(s, DASH_CH_ECT))     { dash_ch_set(s, DASH_CH_ECT, sim->ect_f); }
    if (dash_ch_sim_owned(s, DASH_CH_OILT))    { dash_ch_set(s, DASH_CH_OILT, sim->oilt_f); }
    if (dash_ch_sim_owned(s, DASH_CH_OILP))    { dash_ch_set(s, DASH_CH_OILP, oilp_psi); }
    if (dash_ch_sim_owned(s, DASH_CH_VOLTS))   { dash_ch_set(s, DASH_CH_VOLTS, volts); }
    if (dash_ch_sim_owned(s, DASH_CH_FUEL))    { dash_ch_set(s, DASH_CH_FUEL, sim->fuel_gal); }
    if (dash_ch_sim_owned(s, DASH_CH_AMBIENT)) { dash_ch_set(s, DASH_CH_AMBIENT, ambient_f); }

    if (track)
    {
        if (dash_ch_sim_owned(s, DASH_CH_DELTA)) { dash_ch_set(s, DASH_CH_DELTA, delta_s); }
        if (dash_ch_sim_owned(s, DASH_CH_LAP))   { dash_ch_set(s, DASH_CH_LAP, (float) sim->lap_ms); }
        if (sim->lap_count > 0u) /* before the first lap, LAST/BEST stay --:--.--- */
        {
            if (dash_ch_sim_owned(s, DASH_CH_LAST)) { dash_ch_set(s, DASH_CH_LAST, (float) sim->last_ms); }
            if (dash_ch_sim_owned(s, DASH_CH_BEST)) { dash_ch_set(s, DASH_CH_BEST, (float) sim->best_ms); }
        }
    }
}

#endif /* DASH_SIM_H */
