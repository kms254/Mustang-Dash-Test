/*
 * Invariant test: the deterministic dash simulator (dash_sim.h) must stay
 * inside physically plausible bounds, respect the data-interface ownership
 * rules (override / cleared / frozen, KTD6), keep gear+speed consistent with
 * the T56 ratios, cycle laps, and be bit-for-bit deterministic (plan U4,
 * KTD5, R12).
 *
 * Runs on the host:
 *   gcc -std=c11 -Wall -Wextra -Werror -I MustangDash tests/test_dash_sim.c \
 *       -lm -o /tmp/tds && /tmp/tds
 */

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "dash_data.h"
#include "dash_sim.h"

/* the drivetrain table must be the six T56 forward gears */
_Static_assert(sizeof(SIM_GEAR_RATIOS) / sizeof(SIM_GEAR_RATIOS[0]) == 6,
               "SIM_GEAR_RATIOS must have exactly 6 entries");
/* every channel id must fit the 16-bit valid/override/cleared masks */
_Static_assert(DASH_CH_COUNT <= 32, "channel ids must fit a uint32_t mask");

static int failures = 0;

static void expect(int cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

/* the test's own copy of the speed law the sim must obey */
static float expected_mph(float rpm, uint8_t gear)
{
    return rpm * 26.0f / (SIM_GEAR_RATIOS[gear - 1] * 3.73f * 336.0f);
}

int main(void)
{
    DashState s;
    DashSimState sim;

    dash_state_init(&s);
    dash_sim_init(&sim);
    expect(s.mode == DASH_MODE_TRACK, "boot mode must be TRACK");
    expect(sim.gear == 1, "sim must init in gear 1");

    /* ---- long-run bounds: 10 sim-minutes of TRACK at dt = 50 ms ---- */
    uint8_t prev_gear = sim.gear;
    float prev_rpm = 0.0f;
    float prev_fuel = 1e9f;
    uint32_t prev_lap_count = 0;
    uint32_t best_seen = 0;
    int best_records = 0;
    bool saw_top_gear = false;
    bool saw_downshift = false;
    float max_speed_seen = 0.0f;
    float min_speed_after_lap1 = 1e9f;
    float rpm_at_100_min = 1e9f; /* rpm observed near 100 mph -- the */
    float rpm_at_100_max = 0.0f; /* same-speed-different-gear spread */

    for (int i = 0; i < 12000; i++)
    {
        dash_sim_step(&sim, &s, 50u);

        expect(s.ch.rpm >= 0.0f && s.ch.rpm <= 8000.0f, "rpm must stay in [0, 8000]");
        expect(s.ch.speed_mph >= 0.0f && s.ch.speed_mph <= 210.0f,
               "speed must stay in [0, 210] mph");
        expect(s.ch.ect_f >= 40.0f && s.ch.ect_f <= 230.0f, "ECT must stay in [40, 230] F");
        expect(s.ch.oil_temp_f >= 40.0f && s.ch.oil_temp_f <= 260.0f,
               "oil temp must stay in [40, 260] F");
        expect(s.ch.oil_press_psi >= 5.0f && s.ch.oil_press_psi <= 75.0f,
               "oil pressure must stay in [5, 75] psi");
        expect(s.ch.volts >= 12.5f && s.ch.volts <= 14.5f, "volts must stay in [12.5, 14.5]");

        /* Phase-2 channels: always-published bounds (KTD5) */
        expect(s.ch.afr_l >= 11.0f && s.ch.afr_l <= 14.0f, "afr_l must stay in [11, 14]");
        expect(s.ch.afr_r >= 11.0f && s.ch.afr_r <= 14.0f, "afr_r must stay in [11, 14]");
        expect(s.ch.iat_f >= 50.0f && s.ch.iat_f <= 150.0f, "iat must stay in [50, 150] F");
        expect(s.ch.fuel_press_psi >= 42.9f && s.ch.fuel_press_psi <= 50.1f,
               "fuel pressure must stay in [43, 50] psi");
        expect(s.ch.pump_a >= 7.5f && s.ch.pump_a <= 8.5f, "pump amps must stay in [7.5, 8.5]");
        expect(s.ch.fan1_a >= 0.0f && s.ch.fan1_a <= 13.0f, "fan1 amps must stay in [0, 13]");
        expect(s.ch.fan2_a >= 0.0f && s.ch.fan2_a <= 12.0f, "fan2 amps must stay in [0, 12]");
        expect(s.ch.time_min < 1440u, "time must stay in [0, 1440) minutes");

        /* Phase-2 channels: TRACK-only bounds */
        expect(s.ch.throttle_pct >= 0.0f && s.ch.throttle_pct <= 100.0f,
               "throttle must stay in [0, 100] pct");
        expect(s.ch.brake_pct >= 0.0f && s.ch.brake_pct <= 100.0f, "brake must stay in [0, 100] pct");
        expect(s.ch.lap_n >= 1u, "lap number must be at least 1");
        expect(s.ch.pos >= 1u && s.ch.pos <= 99u, "pos must stay in [1, 99]");
        if (dash_ch_valid(&s, DASH_CH_PRED))
        {
            expect(s.ch.pred_ms <= 599999u, "pred must stay within the serial range");
        }

        expect(s.ch.fuel_gal >= 0.0f, "fuel must never go negative");
        expect(s.ch.fuel_gal <= prev_fuel + 1e-6f, "fuel must be monotonically non-increasing");
        prev_fuel = s.ch.fuel_gal;

        /* no spurious low-oil-pressure alarms once running hard */
        if (s.ch.rpm > 2500.0f)
        {
            expect(s.ch.oil_press_psi > 29.0f,
                   "oil pressure must stay above 29 psi when rpm > 2500");
        }

        /* gear/speed consistency at every 100th tick */
        if (i % 100 == 0)
        {
            expect(fabsf(s.ch.speed_mph - expected_mph(s.ch.rpm, sim.gear)) <= 2.0f,
                   "speed must match rpm * 26 / (ratio * 3.73 * 336) within 2 mph");
        }

        /* shift behavior: hysteretic box, one gear per tick, rpm moves the
         * physical direction (down on upshift, up on downshift) */
        expect(sim.gear >= 1 && sim.gear <= 6, "gear must stay in 1..6");
        expect((sim.gear >= prev_gear ? sim.gear - prev_gear : prev_gear - sim.gear) <= 1,
               "gear must change at most one step per tick");
        if (sim.gear > prev_gear)
        {
            expect(s.ch.rpm < prev_rpm, "an upshift must drop rpm at near-constant speed");
        }
        if (sim.gear < prev_gear)
        {
            saw_downshift = true;
            expect(s.ch.rpm > prev_rpm, "a downshift must raise rpm at near-constant speed");
        }
        if (sim.gear == 6) { saw_top_gear = true; }
        prev_gear = sim.gear;
        prev_rpm = s.ch.rpm;

        /* driving-cycle dynamics: collect the spread of speeds and the rpm
         * range observed near 100 mph (must span gears, not a fixed map) */
        if (s.ch.speed_mph > max_speed_seen) { max_speed_seen = s.ch.speed_mph; }
        if (sim.lap_count >= 1u && s.ch.speed_mph < min_speed_after_lap1)
        {
            min_speed_after_lap1 = s.ch.speed_mph;
        }
        if (s.ch.speed_mph >= 95.0f && s.ch.speed_mph <= 105.0f)
        {
            if (s.ch.rpm < rpm_at_100_min) { rpm_at_100_min = s.ch.rpm; }
            if (s.ch.rpm > rpm_at_100_max) { rpm_at_100_max = s.ch.rpm; }
        }

        /* lap rollovers: best only ever improves */
        if (sim.lap_count > prev_lap_count)
        {
            if (best_records > 0)
            {
                expect(s.ch.best_ms <= best_seen, "best lap must be non-increasing");
            }
            best_seen = s.ch.best_ms;
            best_records++;
            prev_lap_count = sim.lap_count;
        }
    }

    /* ---- lap cycle after 10 sim-minutes ---- */
    expect(sim.lap_count >= 8, "10 track minutes must complete at least 8 laps");
    expect(dash_ch_valid(&s, DASH_CH_LAP), "current lap channel must be valid");
    expect(dash_ch_valid(&s, DASH_CH_LAST), "last lap must be valid after a completed lap");
    expect(dash_ch_valid(&s, DASH_CH_BEST), "best lap must be valid after a completed lap");
    expect(s.ch.best_ms <= s.ch.last_ms, "best lap must be <= last lap");

    /* ---- driving-cycle dynamics over the 10 minutes ---- */
    expect(saw_top_gear, "the straights must reach 6th gear");
    expect(saw_downshift, "the corners must force downshifts");
    expect(max_speed_seen >= 170.0f, "the straights must exceed 170 mph");
    expect(max_speed_seen <= 205.0f, "top speed must stay under the 210 contract with margin");
    expect(min_speed_after_lap1 <= 85.0f, "the corners must fall below 85 mph");
    expect((rpm_at_100_max - rpm_at_100_min) >= 1200.0f,
           "~100 mph must occur at rpm at least 1200 apart (different gears, not a fixed speed->rpm map)");

    /* ---- alarm reachability: an override must survive sim steps ---- */
    dash_ch_set(&s, DASH_CH_OILP, 25.0f);
    s.overridden |= DASH_CH_BIT(DASH_CH_OILP);
    for (int i = 0; i < 10; i++)
    {
        dash_sim_step(&sim, &s, 50u);
    }
    expect(s.ch.oil_press_psi == 25.0f, "sim must not clobber an overridden channel");
    expect(dash_ch_valid(&s, DASH_CH_OILP), "overridden channel must stay valid");

    /* ---- validity discipline: cleared channels stay invalid ---- */
    s.cleared |= DASH_CH_BIT(DASH_CH_ECT);
    s.valid &= (uint32_t) ~DASH_CH_BIT(DASH_CH_ECT);
    for (int i = 0; i < 10; i++)
    {
        dash_sim_step(&sim, &s, 50u);
    }
    expect(!dash_ch_valid(&s, DASH_CH_ECT), "sim must not resurrect a cleared channel");

    /* ---- sim_frozen: no time accrual, no writes ---- */
    float rpm_snap = s.ch.rpm;
    float t_snap = sim.t_s;
    uint32_t lap_snap = sim.lap_ms;
    s.sim_frozen = true;
    for (int i = 0; i < 5; i++)
    {
        dash_sim_step(&sim, &s, 50u);
    }
    expect(s.ch.rpm == rpm_snap, "frozen sim must not change rpm");
    expect(sim.t_s == t_snap, "frozen sim must not accrue time");
    expect(sim.lap_ms == lap_snap, "frozen sim must not accrue lap time");
    s.sim_frozen = false;

    /* ---- determinism: two identical runs agree exactly ---- */
    DashState sa, sb;
    DashSimState ma, mb;
    dash_state_init(&sa);
    dash_state_init(&sb);
    dash_sim_init(&ma);
    dash_sim_init(&mb);
    for (int i = 0; i < 1000; i++)
    {
        dash_sim_step(&ma, &sa, 50u);
        dash_sim_step(&mb, &sb, 50u);
    }
    expect(sa.ch.rpm == sb.ch.rpm, "determinism: rpm must match exactly");
    expect(sa.ch.speed_mph == sb.ch.speed_mph, "determinism: speed must match exactly");
    expect(sa.ch.fuel_gal == sb.ch.fuel_gal, "determinism: fuel must match exactly");
    expect(sa.ch.afr_l == sb.ch.afr_l, "determinism: afr_l must match exactly");
    expect(sa.ch.iat_f == sb.ch.iat_f, "determinism: iat must match exactly");
    expect(sa.ch.fuel_press_psi == sb.ch.fuel_press_psi, "determinism: fuel pressure must match exactly");
    expect(sa.ch.throttle_pct == sb.ch.throttle_pct, "determinism: throttle must match exactly");
    expect(sa.ch.brake_pct == sb.ch.brake_pct, "determinism: brake must match exactly");
    expect(sa.ch.lap_n == sb.ch.lap_n, "determinism: lap number must match exactly");
    expect(sa.ch.pos == sb.ch.pos, "determinism: pos must match exactly");
    expect(sa.ch.pred_ms == sb.ch.pred_ms, "determinism: pred must match exactly");
    expect(sa.ch.time_min == sb.ch.time_min, "determinism: time must match exactly");
    expect(sa.ch.pump_a == sb.ch.pump_a, "determinism: pump amps must match exactly");
    expect(sa.ch.fan1_a == sb.ch.fan1_a, "determinism: fan1 amps must match exactly");
    expect(sa.ch.fan2_a == sb.ch.fan2_a, "determinism: fan2 amps must match exactly");

    /* ---- mask width: channel 24 (the last channel) and DASH_CH_ALL ---- */
    {
        DashState m;
        dash_state_init(&m);
        expect(DASH_CH_COUNT == 25, "must have exactly 25 channels");
        expect(DASH_CH_FAN2 == 24, "FAN2 must be channel id 24 (the last channel)");

        dash_ch_set(&m, DASH_CH_FAN2, 5.0f);
        m.overridden |= DASH_CH_BIT(DASH_CH_FAN2);
        expect(dash_ch_valid(&m, DASH_CH_FAN2), "channel 24's bit must set correctly in the valid mask");
        expect((m.overridden & DASH_CH_BIT(DASH_CH_FAN2)) != 0,
               "channel 24's bit must set correctly in the overridden mask");
        m.cleared |= DASH_CH_BIT(DASH_CH_FAN2);
        expect((m.cleared & DASH_CH_BIT(DASH_CH_FAN2)) != 0,
               "channel 24's bit must set correctly in the cleared mask");

        m.valid &= (uint32_t) ~DASH_CH_BIT(DASH_CH_FAN2);
        m.overridden &= (uint32_t) ~DASH_CH_BIT(DASH_CH_FAN2);
        m.cleared &= (uint32_t) ~DASH_CH_BIT(DASH_CH_FAN2);
        expect(!dash_ch_valid(&m, DASH_CH_FAN2), "channel 24's bit must clear correctly in the valid mask");
        expect((m.overridden & DASH_CH_BIT(DASH_CH_FAN2)) == 0,
               "channel 24's bit must clear correctly in the overridden mask");
        expect((m.cleared & DASH_CH_BIT(DASH_CH_FAN2)) == 0,
               "channel 24's bit must clear correctly in the cleared mask");

        /* DASH_CH_ALL must round-trip every id 0..24 and nothing beyond */
        for (uint8_t ch = 0; ch < DASH_CH_COUNT; ch++)
        {
            expect((DASH_CH_ALL & DASH_CH_BIT(ch)) != 0,
                   "DASH_CH_ALL must cover every channel id 0..24");
        }
        expect((DASH_CH_ALL & ~(uint32_t) ((1U << DASH_CH_COUNT) - 1U)) == 0,
               "DASH_CH_ALL must not set bits beyond DASH_CH_COUNT");
    }

    /* ---- STREET mode: cruise band, no lap timing ---- */
    DashState st;
    DashSimState mt;
    dash_state_init(&st);
    dash_sim_init(&mt);
    st.mode = DASH_MODE_STREET;
    for (int i = 0; i < 2400; i++) /* 2 sim-minutes at dt = 50 ms */
    {
        dash_sim_step(&mt, &st, 50u);
        expect(st.ch.speed_mph >= 50.0f && st.ch.speed_mph <= 70.0f,
               "street speed must cruise in [50, 70] mph");
        expect(st.ch.rpm >= 1900.0f && st.ch.rpm <= 3100.0f,
               "street rpm must stay in [1900, 3100]");
        expect(!dash_ch_valid(&st, DASH_CH_LAP), "street mode must never validate LAP");
        expect(!dash_ch_valid(&st, DASH_CH_LAST), "street mode must never validate LAST");
        expect(!dash_ch_valid(&st, DASH_CH_BEST), "street mode must never validate BEST");
        expect(!dash_ch_valid(&st, DASH_CH_LAPN), "street mode must never validate LAPN");
        expect(!dash_ch_valid(&st, DASH_CH_POS), "street mode must never validate POS");
        expect(!dash_ch_valid(&st, DASH_CH_PRED), "street mode must never validate PRED");
        expect(!dash_ch_valid(&st, DASH_CH_THROTTLE), "street mode must never validate THROTTLE");
        expect(!dash_ch_valid(&st, DASH_CH_BRAKE), "street mode must never validate BRAKE");
        expect(dash_ch_valid(&st, DASH_CH_AFR_L), "street mode must still validate AFR_L (engine sensor)");
        expect(dash_ch_valid(&st, DASH_CH_TIME), "street mode must still validate TIME");
        expect(st.ch.afr_l >= 11.0f && st.ch.afr_l <= 14.0f, "street afr_l must stay in [11, 14]");
    }
    expect(st.ch.fuel_gal >= 11.99f, "street fuel burn must be ~40x slower than track");

    if (failures == 0)
    {
        printf("OK: dash sim honors bounds, ownership, laps, freeze, and determinism\n");
        return 0;
    }
    return 1;
}
