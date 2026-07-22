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

/* the test's own copy of the speed law the sim must obey (315/30R18, R5) */
static float expected_mph(float rpm, uint8_t gear)
{
    return rpm * 25.44f / (SIM_GEAR_RATIOS[gear - 1] * 3.73f * 336.0f);
}

int main(void)
{
    DashState s;
    DashSimState sim;

    dash_state_init(&s);
    dash_sim_init(&sim);
    expect(s.mode == DASH_MODE_TRACK, "boot mode must be TRACK");
    expect(sim.gear == 1, "sim must init in gear 1");
    expect(sim.lap_dist_ft == 0.0f, "sim must init at the start/finish line");
    expect(sim.speed_mph > 5.0f && sim.speed_mph < 10.0f,
           "sim must init on the pit-lane rollout, not rolling at speed");

    /* ---- U3: the High Plains Raceway table ---- */
    {
        float sum = 0.0f;
        for (uint8_t i = 0; i < SIM_SEG_COUNT; i++)
        {
            sum += SIM_SEGS[i].len_ft;
            expect(SIM_SEGS[i].len_ft > 0.0f, "every HPR segment must have positive length");
            expect(fabsf(SIM_SEGS[i].end_ft - sum) < 0.01f,
                   "each HPR segment's end_ft must be the running sum of len_ft");
        }
        expect(fabsf(sum - SIM_TRACK_LAP_FT) < 0.01f,
               "the HPR segment lengths must sum to exactly SIM_TRACK_LAP_FT");
        expect(SIM_SEG_COUNT == 16u, "the HPR table must have 16 segments");
        expect(fabsf(SIM_SEGS[0].len_ft - 2838.0f) < 0.01f,
               "segment 0 is the published 2838 ft front straight and must not flex");
        expect(!SIM_SEGS[0].is_corner_limit, "segment 0 (front straight) is not a corner limit");
        expect(!SIM_SEGS[9].is_corner_limit,
               "segment 9 (T12 Ladder to Heaven) is flat out, not a corner limit");
        for (uint8_t i = 0; i < SIM_SEG_COUNT; i++)
        {
            if (i != 0u && i != 9u)
            {
                expect(SIM_SEGS[i].is_corner_limit, "every other HPR segment is a corner limit");
            }
        }
        expect(fabsf(SIM_SEGS[13].limit_mph - 40.0f) < 0.01f,
               "segment 13 is T1, the tightest corner on the track");
    }

    /* ---- long-run bounds: 10 sim-minutes of TRACK at dt = 50 ms ---- */
    uint8_t prev_gear = sim.gear;
    float prev_rpm = 0.0f;
    float prev_fuel = 1e9f;
    uint32_t prev_lap_count = 0;
    uint32_t best_seen = 0;
    int best_records = 0;
    bool saw_downshift = false;
    float max_speed_seen = 0.0f;
    float min_speed_after_lap1 = 1e9f;
    uint8_t min_speed_seg = 0xFFu;
    float rpm_at_80_min = 1e9f; /* rpm observed near 80 mph -- the */
    float rpm_at_80_max = 0.0f; /* same-speed-different-gear spread */

    /* U1/U3 circuit instrumentation */
    float seg0_peak_mph = 0.0f;      /* front-straight peak */
    uint8_t prev_si = 0xFFu;
    float seg_max_mph = 0.0f;        /* peak speed inside the segment just left */
    bool seg_saw_lift = false;       /* throttle hit 0 inside that segment */
    int corner_entries = 0;
    int brake_zones = 0;
    uint32_t lap_times_ms[8];
    int lap_times_n = 0;

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
                   "speed must match rpm * 25.44 / (ratio * 3.73 * 336) within 2 mph");
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
        prev_gear = sim.gear;
        prev_rpm = s.ch.rpm;

        /* driving-cycle dynamics: collect the spread of speeds and the rpm
         * range observed near 80 mph (must span gears, not a fixed map) */
        if (s.ch.speed_mph > max_speed_seen) { max_speed_seen = s.ch.speed_mph; }
        if (sim.lap_count >= 1u && s.ch.speed_mph < min_speed_after_lap1)
        {
            min_speed_after_lap1 = s.ch.speed_mph;
            min_speed_seg = sim.seg_idx;
        }
        if (s.ch.speed_mph >= 75.0f && s.ch.speed_mph <= 85.0f)
        {
            if (s.ch.rpm < rpm_at_80_min) { rpm_at_80_min = s.ch.rpm; }
            if (s.ch.rpm > rpm_at_80_max) { rpm_at_80_max = s.ch.rpm; }
        }

        /* ---- U3: lookahead braking, checked at every segment boundary ----
         * a segment's limit constrains its ENTRY only, so the invariant is
         * "at or below the limit at the boundary", and the lift that got the
         * car there must have happened in the PREVIOUS segment. */
        if (sim.seg_idx != prev_si)
        {
            uint8_t si = sim.seg_idx;
            if ((prev_si != 0xFFu) && SIM_SEGS[si].is_corner_limit)
            {
                float lim = SIM_SEGS[si].limit_mph + sim.seg_jitter_mph[si];
                expect(s.ch.speed_mph <= lim + 2.0f,
                       "the car must enter every corner at or below that corner's limit");
                corner_entries++;
                if (seg_max_mph > lim + 1.0f) /* it actually had to slow down */
                {
                    expect(seg_saw_lift,
                           "braking must begin before the corner, not inside it");
                    brake_zones++;
                }
            }
            seg_max_mph = 0.0f;
            seg_saw_lift = false;
            prev_si = si;
        }
        if (s.ch.speed_mph > seg_max_mph) { seg_max_mph = s.ch.speed_mph; }
        if (s.ch.throttle_pct == 0.0f) { seg_saw_lift = true; }
        if (sim.seg_idx == 0u && s.ch.speed_mph > seg0_peak_mph)
        {
            seg0_peak_mph = s.ch.speed_mph;
        }

        expect(sim.lap_dist_ft >= 0.0f && sim.lap_dist_ft < SIM_TRACK_LAP_FT,
               "lap distance must stay inside [0, SIM_TRACK_LAP_FT)");

        /* lap rollovers: best only ever improves */
        if (sim.lap_count > prev_lap_count)
        {
            expect(sim.lap_count == prev_lap_count + 1u,
                   "lap count must advance exactly one per lap-distance crossing");
            if (lap_times_n < 8) { lap_times_ms[lap_times_n++] = sim.last_ms; }
            if (dash_ch_valid(&s, DASH_CH_BEST))
            {
                if (best_records > 0)
                {
                    expect(s.ch.best_ms <= best_seen, "best lap must be non-increasing");
                }
                best_seen = s.ch.best_ms;
                best_records++;
            }
            prev_lap_count = sim.lap_count;
        }
    }

    /* ---- lap cycle after 10 sim-minutes (replaces the 62 s-lap bound) ---- */
    expect(sim.lap_count >= 4u && sim.lap_count <= 6u,
           "10 track minutes must complete 4-6 HPR laps");
    expect(dash_ch_valid(&s, DASH_CH_LAP), "current lap channel must be valid");
    expect(dash_ch_valid(&s, DASH_CH_LAST), "last lap must be valid after a completed lap");
    expect(dash_ch_valid(&s, DASH_CH_BEST), "best lap must be valid after a completed lap");
    expect(s.ch.best_ms <= s.ch.last_ms, "best lap must be <= last lap");

    /* lap 1 is the out-lap: slower than every flying lap, and excluded from best */
    expect(lap_times_n >= 4, "at least four laps must be timed in 10 minutes");
    for (int i = 1; i < lap_times_n; i++)
    {
        expect(lap_times_ms[0] > lap_times_ms[i],
               "lap 1 (the out-lap) must be slower than every flying lap");
        expect(s.ch.best_ms != lap_times_ms[0],
               "the out-lap must never become the best lap");
    }

    /* ---- driving-cycle dynamics over the 10 minutes ---- */
    expect(saw_downshift, "the corners must force downshifts");
    expect(max_speed_seen <= 205.0f, "top speed must stay under the 210 contract with margin");
    expect(seg0_peak_mph >= 140.0f && seg0_peak_mph <= 185.0f,
           "front-straight peak must land in the 140-185 mph sanity band");
    expect(min_speed_after_lap1 <= 46.0f && min_speed_after_lap1 >= 33.0f,
           "the slowest point on the lap must be near T1's 40 mph target");
    expect(min_speed_seg == 12u || min_speed_seg == 13u,
           "the slowest point on the lap must be at T1 (segment 13) or its entry");
    expect(corner_entries >= 40, "every corner on every lap must be checked");
    expect(brake_zones >= 20, "most corners must be preceded by a real braking zone");
    expect((rpm_at_80_max - rpm_at_80_min) >= 1200.0f,
           "~80 mph must occur at rpm at least 1200 apart (different gears, not a fixed speed->rpm map)");

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
    float dist_snap = sim.lap_dist_ft;
    s.sim_frozen = true;
    for (int i = 0; i < 5; i++)
    {
        dash_sim_step(&sim, &s, 50u);
    }
    expect(s.ch.rpm == rpm_snap, "frozen sim must not change rpm");
    expect(sim.t_s == t_snap, "frozen sim must not accrue time");
    expect(sim.lap_ms == lap_snap, "frozen sim must not accrue lap time");
    expect(sim.lap_dist_ft == dist_snap, "frozen sim must not accrue lap distance");
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
    expect(ma.lap_dist_ft == mb.lap_dist_ft, "determinism: lap distance must match exactly");
    expect(ma.lap_ms == mb.lap_ms, "determinism: lap time must match exactly");

    /* ---- step-size independence: 10 ms and 50 ms integrate the same lap ----
     * (dash_sim.h's standing contract: all rates scale by dt_ms) */
    {
        DashState s10, s50;
        DashSimState m10, m50;
        dash_state_init(&s10);
        dash_state_init(&s50);
        dash_sim_init(&m10);
        dash_sim_init(&m50);
        for (int i = 0; i < 6000; i++) { dash_sim_step(&m10, &s10, 10u); } /* 60 s */
        for (int i = 0; i < 1200; i++) { dash_sim_step(&m50, &s50, 50u); } /* 60 s */
        expect(fabsf(m10.t_s - m50.t_s) < 0.05f, "both step sizes must accrue 60 s");
        expect(fabsf(m10.lap_dist_ft - m50.lap_dist_ft) < 0.02f * SIM_TRACK_LAP_FT,
               "10 ms and 50 ms steps must cover the same lap distance within 2%");
    }

    /* ---- U2: the traction / power / drag acceleration model ---- */
    {
        DashState u2s;
        DashSimState u2m;

        /* off the line the car is tire-limited, not power-limited */
        dash_state_init(&u2s);
        dash_sim_init(&u2m);
        float v0 = u2m.speed_mph;
        dash_sim_step(&u2m, &u2s, 10u);
        float a0 = (u2m.speed_mph - v0) / 0.01f;
        expect(fabsf(a0 - (SIM_TRACTION_G * SIM_G_FPS2 / SIM_FPS_PER_MPH)) < 0.5f,
               "acceleration from rest must be traction-limited, not power-limited");

        /* the power term divides by speed: a standing start must stay finite */
        dash_state_init(&u2s);
        dash_sim_init(&u2m);
        u2m.speed_mph = 0.0f;
        dash_sim_step(&u2m, &u2s, 10u);
        expect(isfinite(u2m.speed_mph) && u2m.speed_mph > 0.0f,
               "zero speed must not divide by zero, and the car must pull away");

        /* up the front straight (no corner within braking range): full
         * throttle always accelerates, and does so less the faster it goes */
        float prev_a = 1e9f;
        for (float v = 70.0f; v <= 160.0f; v += 10.0f)
        {
            dash_state_init(&u2s);
            dash_sim_init(&u2m);
            u2m.speed_mph = v;
            u2m.lap_dist_ft = 100.0f;
            dash_sim_step(&u2m, &u2s, 10u);
            float a = (u2m.speed_mph - v) / 0.01f;
            expect(a > 0.0f, "full throttle must never decelerate below terminal speed");
            expect(a < prev_a, "acceleration must fall monotonically with speed (power-limited)");
            prev_a = a;
        }
    }

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
