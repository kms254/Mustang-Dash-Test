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

/* U4: best flying-lap time with the driver retuned to `skill`. dash_sim_set_skill
 * rescales the jitter table in place rather than redrawing it, so two runs at
 * different skills walk the same LCG stream and differ ONLY by the driver. */
static uint32_t best_lap_at_skill(float skill)
{
    DashState s;
    DashSimState m;
    dash_state_init(&s);
    dash_sim_init(&m);
    dash_sim_set_skill(&m, skill);
    while (m.lap_count < 4u) { dash_sim_step(&m, &s, 50u); }
    return m.best_ms;
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

        /* U4: per-corner turn angle. Every real corner turns through some
         * angle; the two annotations turn through none. */
        for (uint8_t i = 0; i < SIM_SEG_COUNT; i++)
        {
            if (SIM_SEGS[i].is_corner_limit)
            {
                expect(SIM_SEGS[i].turn_rad > 0.3f && SIM_SEGS[i].turn_rad < 3.2f,
                       "every corner must turn through a plausible angle (17-183 deg)");
            }
            else
            {
                expect(SIM_SEGS[i].turn_rad == 0.0f,
                       "annotation segments (0 and 9) must turn through no angle");
            }
        }
        expect(fabsf(SIM_SEGS[13].turn_rad - SIM_DEG(160.0f)) < 0.001f,
               "segment 13 is T1, documented by the track at 160 degrees");
        expect(SIM_SEGS[13].turn_rad > SIM_SEGS[4].turn_rad,
               "T1 (160 deg hairpin) must turn through more angle than T7 (fast sweeper)");
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
    /* U10 corner-duration instrumentation. Deliberately knows nothing about
     * how the arc is computed -- it measures only what a corner should LOOK
     * like from outside: distance held near the limit, and the speed at a
     * fixed depth into the corner. */
    float seg9_max_mph = 0.0f;       /* T12 is flat out, must stay unaffected */
    float prev_dist_ft = 0.0f;
    float near_limit_ft = 0.0f;      /* this lap, distance held near a limit */
    float near_limit_ft_min = 1e9f;  /* worst flying lap */
    int corner_probes = 0;           /* 80 ft into a corner, still at the limit? */
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

        /* U5: once a reference lap exists DELTA is a real measurement, so it
         * must stay finite and inside a sane band -- a broken trace shows up
         * here as a wild or non-finite number rather than a plausible one. */
        if (dash_ch_valid(&s, DASH_CH_DELTA))
        {
            expect(isfinite(s.ch.delta_s) && fabsf(s.ch.delta_s) <= 5.0f,
                   "delta must stay finite and within +/-5 s");
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
                float lim = dash_sim_seg_limit_mph(&sim, si);
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
        if (sim.seg_idx == 9u && s.ch.speed_mph > seg9_max_mph)
        {
            seg9_max_mph = s.ch.speed_mph;
        }
        /* U4: T12 is a flat-out kink. The car must be pinned across its entry
         * -- the only lift allowed inside segment 9 is the lookahead braking
         * for T13, which lives at the segment's far end. */
        if (sim.seg_idx == 9u
            && sim.lap_dist_ft < (SIM_SEGS[9].end_ft - SIM_SEGS[9].len_ft + 100.0f))
        {
            expect(s.ch.throttle_pct == 100.0f,
                   "the car must be at full throttle entering T12, never braked "
                   "for its annotation speed");
        }

        /* ---- U10: corners must have DURATION ---- */
        {
            uint8_t si = sim.seg_idx;
            float lim = dash_sim_seg_limit_mph(&sim, si);

            if (sim.lap_dist_ft < prev_dist_ft) /* lap rolled over */
            {
                if (sim.lap_count >= 2u && near_limit_ft < near_limit_ft_min)
                {
                    near_limit_ft_min = near_limit_ft;
                }
                near_limit_ft = 0.0f;
            }
            else if (SIM_SEGS[si].is_corner_limit
                     && fabsf(s.ch.speed_mph - lim) <= 5.0f)
            {
                near_limit_ft += sim.lap_dist_ft - prev_dist_ft;
            }

            /* 80 ft into every corner the car must STILL be at its limit --
             * this is what "the corner has an arc" means, and it is the one
             * thing entry-boundary-only limiting cannot produce. 80 ft is
             * comfortably inside even T1's arc at its most negative jitter. */
            if (SIM_SEGS[si].is_corner_limit && sim.lap_count >= 1u)
            {
                float probe = (SIM_SEGS[si].end_ft - SIM_SEGS[si].len_ft) + 80.0f;
                if (prev_dist_ft < probe && sim.lap_dist_ft >= probe)
                {
                    expect(s.ch.speed_mph <= lim + 3.0f,
                           "80 ft into a corner the car must still be at its limit, "
                           "not already accelerating away from it");
                    corner_probes++;
                }
            }
            prev_dist_ft = sim.lap_dist_ft;
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
    /* T1's target is the authored 40 mph pulled down by the driver constant,
     * plus this lap's jitter -- so the band tracks SIM_DRIVER_SKILL rather
     * than pinning a number that only holds at one skill value. */
    {
        float t1 = SIM_SEGS[13].limit_mph * SIM_DRIVER_SKILL;
        expect(min_speed_after_lap1 <= t1 + 6.0f && min_speed_after_lap1 >= t1 - 6.0f,
               "the slowest point on the lap must be near T1's skill-scaled target");
    }
    expect(min_speed_seg == 12u || min_speed_seg == 13u,
           "the slowest point on the lap must be at T1 (segment 13) or its entry");
    expect(corner_entries >= 40, "every corner on every lap must be checked");
    expect(brake_zones >= 20, "most corners must be preceded by a real braking zone");
    expect((rpm_at_80_max - rpm_at_80_min) >= 1200.0f,
           "~80 mph must occur at rpm at least 1200 apart (different gears, not a fixed speed->rpm map)");

    /* ---- U10: corner duration from lateral grip ---- */
    expect(corner_probes >= 40, "every corner on every lap must be probed for duration");
    expect(near_limit_ft_min >= 2500.0f,
           "a lap must hold a corner limit over thousands of feet of arc, "
           "not touch each limit for a single instant");
    /* T12 carries a 125 mph annotation. If it were ever treated as a limit the
     * car would be capped there -- or at 125 * SIM_DRIVER_SKILL once U4 lands,
     * ~108 mph. Running clean past the annotation is the decisive evidence that
     * it is neither arced nor scaled, and unlike a fixed mph threshold it does
     * not silently re-pin itself to one skill value. */
    expect(seg9_max_mph > SIM_SEGS[9].limit_mph + 5.0f,
           "segment 9 (T12) is flat out: the car must run past its annotation "
           "speed, never be arced or skill-scaled down to it");
    expect(s.ch.best_ms > 95000u && s.ch.best_ms < 150000u,
           "corner duration must move lap time substantially off the 1:28 "
           "entry-boundary-only baseline, toward the real-world band");

    /* ---- U4: the driver skill constant ---- */

    /* the constant must stay inside its guardrail. Outside 0.82-0.95 it has
     * stopped meaning "moderate driver" and started absorbing errors in the
     * apportioned lengths, the authored speeds, CdA, or the power figure. */
    expect(SIM_DRIVER_SKILL >= 0.82f && SIM_DRIVER_SKILL <= 0.95f,
           "the calibrated SIM_DRIVER_SKILL must sit within 0.82-0.95");

    /* wide sanity bound on the calibrated default: catches genuine model
     * breakage without pinning the sim to an unverified target */
    expect(s.ch.best_ms > 100000u && s.ch.best_ms < 150000u,
           "the default lap must land in a 1:40-2:30 sanity band");

    /* a better driver is faster, at every step of the constant */
    {
        const float skills[6] = { 0.82f, 0.86f, 0.90f, 0.94f, 0.97f, 1.00f };
        uint32_t prev = 0xFFFFFFFFu;
        for (int i = 0; i < 6; i++)
        {
            uint32_t best = best_lap_at_skill(skills[i]);
            expect(best < prev, "raising SIM_DRIVER_SKILL toward 1.0 must reduce lap time");
            prev = best;
        }
    }

    /* skill scales real corner limits and NOTHING else. Segments 0 and 9 carry
     * annotation speeds: scaling those would put an enforced ceiling on the
     * front straight and on T12's flat-out kink, and lookahead braking would
     * then invent a braking zone on both. */
    {
        DashState u4s;
        DashSimState u4m;
        dash_state_init(&u4s);
        dash_sim_init(&u4m);
        for (float k = 0.82f; k <= 1.001f; k += 0.06f)
        {
            dash_sim_set_skill(&u4m, k);
            expect(dash_sim_seg_limit_mph(&u4m, 0u) == SIM_SEGS[0].limit_mph,
                   "segment 0 (front straight) must never be scaled by driver skill");
            expect(dash_sim_seg_limit_mph(&u4m, 9u) == SIM_SEGS[9].limit_mph,
                   "segment 9 (T12) must never be scaled by driver skill");
            expect(dash_sim_corner_arc_ft(9u) == 0.0f,
                   "segment 9 (T12) must never be given a corner arc");
            expect(fabsf(dash_sim_seg_limit_mph(&u4m, 13u)
                         - (SIM_SEGS[13].limit_mph * k + u4m.seg_jitter_mph[13])) < 0.01f,
                   "a real corner limit must be scaled by driver skill");
        }
    }

    /* a corner's ARC is track geometry, so it must not move with the driver --
     * a slower driver spends LONGER in the same corner, which is exactly why
     * skill has leverage on lap time at all. */
    {
        DashState u4s;
        DashSimState u4m;
        dash_state_init(&u4s);
        dash_sim_init(&u4m);
        float arc_default = dash_sim_corner_arc_ft(13u);
        dash_sim_set_skill(&u4m, 1.0f);
        expect(dash_sim_corner_arc_ft(13u) == arc_default,
               "corner arc length must not change with driver skill");
        expect(arc_default > 200.0f,
               "T1's 160 deg over its ~82 ft radius must produce a real arc");
    }

    /* consecutive laps must differ visibly, and not by zero */
    {
        float min_l = 1e9f, max_l = 0.0f;
        for (int i = 1; i < lap_times_n; i++)
        {
            if (i > 1)
            {
                expect(lap_times_ms[i] != lap_times_ms[i - 1],
                       "consecutive flying laps must not be identical");
            }
            float sec = (float) lap_times_ms[i] / 1000.0f;
            if (sec < min_l) { min_l = sec; }
            if (sec > max_l) { max_l = sec; }
        }
        expect((max_l - min_l) >= 0.3f && (max_l - min_l) <= 1.5f,
               "flying laps must vary by roughly 0.3-1.5 s");
    }

    /* determinism holds at any constant value, not just the default */
    {
        DashState ka, kb;
        DashSimState pa, pb;
        dash_state_init(&ka);
        dash_state_init(&kb);
        dash_sim_init(&pa);
        dash_sim_init(&pb);
        dash_sim_set_skill(&pa, 0.83f);
        dash_sim_set_skill(&pb, 0.83f);
        for (int i = 0; i < 3000; i++)
        {
            dash_sim_step(&pa, &ka, 50u);
            dash_sim_step(&pb, &kb, 50u);
        }
        expect(pa.lap_dist_ft == pb.lap_dist_ft, "determinism at a retuned skill: distance");
        expect(pa.last_ms == pb.last_ms, "determinism at a retuned skill: lap time");
        expect(ka.ch.rpm == kb.ch.rpm, "determinism at a retuned skill: rpm");
    }

    /* ---- U5 prerequisite: dash_ch_invalidate's ownership contract ----
     * The sim needs a way to put a channel BACK to invalid (no reference lap
     * yet, and later the non-lap fixture modes). It must obey exactly the same
     * ownership rule as writing does: what the operator forced is the
     * operator's, not the sim's. */
    {
        DashState iv;
        dash_state_init(&iv);

        dash_ch_set(&iv, DASH_CH_DELTA, 0.5f);
        expect(dash_ch_valid(&iv, DASH_CH_DELTA), "a sim-owned channel starts valid");
        dash_ch_invalidate(&iv, DASH_CH_DELTA);
        expect(!dash_ch_valid(&iv, DASH_CH_DELTA),
               "invalidate must dead-front a sim-owned channel");

        /* an overridden channel belongs to the operator: untouched */
        dash_ch_set(&iv, DASH_CH_DELTA, 0.5f);
        iv.overridden |= DASH_CH_BIT(DASH_CH_DELTA);
        dash_ch_invalidate(&iv, DASH_CH_DELTA);
        expect(dash_ch_valid(&iv, DASH_CH_DELTA),
               "invalidate must never dead-front an overridden channel");
        expect(iv.ch.delta_s == 0.5f, "invalidate must never disturb a channel's value");

        /* a serially cleared channel is already invalid and must stay so --
         * and the sticky cleared bit must survive, or `sim on` would be the
         * only way back and `clear` would stop being sticky */
        iv.overridden = (uint32_t) (iv.overridden & ~DASH_CH_BIT(DASH_CH_DELTA));
        iv.cleared |= DASH_CH_BIT(DASH_CH_DELTA);
        iv.valid = (uint32_t) (iv.valid & ~DASH_CH_BIT(DASH_CH_DELTA));
        dash_ch_invalidate(&iv, DASH_CH_DELTA);
        expect(!dash_ch_valid(&iv, DASH_CH_DELTA), "a cleared channel stays invalid");
        expect((iv.cleared & DASH_CH_BIT(DASH_CH_DELTA)) != 0,
               "invalidate must not disturb the sticky cleared bit");

        /* out-of-range ids are a no-op, matching dash_ch_set */
        iv.valid = DASH_CH_ALL;
        iv.overridden = 0U;
        iv.cleared = 0U;
        dash_ch_invalidate(&iv, (uint8_t) DASH_CH_COUNT);
        expect(iv.valid == DASH_CH_ALL, "invalidate must ignore an out-of-range channel id");
    }

    /* ---- U5: real lap delta from a best-lap trace ---- */
    {
        DashState d5s;
        DashSimState d5m;

        /* bucket indexing must be safe at both lap boundaries */
        expect(dash_sim_trace_bucket(0.0f) == 0u, "distance 0 must land in bucket 0");
        expect(dash_sim_trace_bucket(-1.0f) == 0u,
               "a negative distance must clamp to bucket 0");
        expect(dash_sim_trace_bucket(SIM_TRACK_LAP_FT) == (uint8_t) (SIM_TRACE_BUCKETS - 1u),
               "distance of exactly one lap must clamp to the last bucket");
        expect(dash_sim_trace_bucket(SIM_TRACK_LAP_FT * 2.0f)
               == (uint8_t) (SIM_TRACE_BUCKETS - 1u),
               "distance past a lap must clamp to the last bucket");
        for (float d = 0.0f; d <= SIM_TRACK_LAP_FT; d += 37.0f)
        {
            expect(dash_sim_trace_bucket(d) < SIM_TRACE_BUCKETS,
                   "every lap position must index a real bucket");
        }

        dash_state_init(&d5s);
        dash_sim_init(&d5m);
        expect(!d5m.ref_valid, "a fresh sim must have no delta reference");

        /* before any reference lap DELTA is dead-fronted (`--`), never a
         * fabricated zero -- matching LAST/BEST's treatment (U1) */
        while (d5m.lap_count < 1u)
        {
            dash_sim_step(&d5m, &d5s, 50u);
            expect(!dash_ch_valid(&d5s, DASH_CH_DELTA),
                   "DELTA must stay invalid before any lap completes");
        }
        expect(!d5m.ref_valid, "the out-lap must never become the delta reference");
        expect(!dash_ch_valid(&d5s, DASH_CH_DELTA),
               "DELTA must still be invalid after only the out-lap");

        while (d5m.lap_count < 2u) { dash_sim_step(&d5m, &d5s, 50u); }
        expect(d5m.ref_valid, "the first flying lap must commit a delta reference");
        dash_sim_step(&d5m, &d5s, 50u);
        expect(dash_ch_valid(&d5s, DASH_CH_DELTA),
               "DELTA must become valid once a reference lap exists");

        expect(d5m.ref_trace_cs[0] == 0u, "the reference trace must start at t=0");
        for (uint16_t b = 1u; b < SIM_TRACE_SLOTS; b++)
        {
            expect(d5m.ref_trace_cs[b] >= d5m.ref_trace_cs[b - 1u],
                   "the reference trace must be monotonic in distance");
        }
        expect(fabsf((float) d5m.ref_trace_cs[SIM_TRACE_BUCKETS] * 10.0f
                     - (float) d5m.best_ms) < 15.0f,
               "the reference trace's finish stamp must be the reference lap's time");

        /* the sim must not dead-front an operator's DELTA on its way past */
        {
            DashState os;
            DashSimState om;
            dash_state_init(&os);
            dash_sim_init(&om);
            dash_ch_set(&os, DASH_CH_DELTA, 0.75f);
            os.overridden |= DASH_CH_BIT(DASH_CH_DELTA);
            for (int i = 0; i < 200; i++) { dash_sim_step(&om, &os, 50u); }
            expect(dash_ch_valid(&os, DASH_CH_DELTA) && os.ch.delta_s == 0.75f,
                   "an overridden DELTA must survive the sim's dead-fronting");
        }

        /* sign: a lap slower than the reference runs positive, a faster one
         * negative. Driver skill is the only lever that moves lap time without
         * disturbing the LCG stream, so it is what "drive slower" means here.
         * Both are sampled on the step BEFORE the line, since the crossing
         * resets the lap clock and takes the delta back to ~0. */
        {
            DashState ss, fs;
            DashSimState sm, fm;
            dash_state_init(&ss);
            dash_sim_init(&sm);
            while (sm.lap_count < 2u) { dash_sim_step(&sm, &ss, 50u); }
            fs = ss;
            fm = sm;
            dash_sim_set_skill(&sm, 0.80f); /* scruffier lap */
            dash_sim_set_skill(&fm, 1.00f); /* tidier lap */
            float slow_last = 0.0f, fast_last = 0.0f;
            while (sm.lap_count < 3u) { slow_last = ss.ch.delta_s; dash_sim_step(&sm, &ss, 50u); }
            while (fm.lap_count < 3u) { fast_last = fs.ch.delta_s; dash_sim_step(&fm, &fs, 50u); }
            expect(slow_last > 0.05f,
                   "a lap slower than the reference must run a positive delta");
            expect(fast_last < -0.05f,
                   "a lap faster than the reference must run a negative delta");
        }

        /* a new best lap replaces the reference trace with its own */
        {
            DashState ns;
            DashSimState nm;
            uint16_t ref_before[SIM_TRACE_SLOTS];
            int changed = 0;
            dash_state_init(&ns);
            dash_sim_init(&nm);
            while (nm.lap_count < 2u) { dash_sim_step(&nm, &ns, 50u); }
            for (uint16_t b = 0u; b < SIM_TRACE_SLOTS; b++)
            {
                ref_before[b] = nm.ref_trace_cs[b];
            }
            uint32_t best_before = nm.best_ms;
            dash_sim_set_skill(&nm, 1.00f); /* guarantees the next lap is a new best */
            while (nm.lap_count < 3u) { dash_sim_step(&nm, &ns, 50u); }
            expect(nm.best_ms < best_before, "the tidier lap must set a new best");
            for (uint16_t b = 0u; b < SIM_TRACE_SLOTS; b++)
            {
                if (nm.ref_trace_cs[b] != ref_before[b]) { changed++; }
            }
            expect(changed > 0, "a new best lap must replace the delta reference trace");
            expect(fabsf((float) nm.ref_trace_cs[SIM_TRACE_BUCKETS] * 10.0f
                         - (float) nm.best_ms) < 15.0f,
                   "the committed reference must be the new best lap's own trace");
        }

        /* a slower lap must NOT replace the reference */
        {
            DashState rs;
            DashSimState rm;
            dash_state_init(&rs);
            dash_sim_init(&rm);
            while (rm.lap_count < 2u) { dash_sim_step(&rm, &rs, 50u); }
            uint16_t finish_before = rm.ref_trace_cs[SIM_TRACE_BUCKETS];
            dash_sim_set_skill(&rm, 0.70f); /* comfortably slower than the reference */
            while (rm.lap_count < 3u) { dash_sim_step(&rm, &rs, 50u); }
            expect(rm.ref_trace_cs[SIM_TRACE_BUCKETS] == finish_before,
                   "a lap slower than the best must leave the reference trace alone");
        }

        /* determinism through the delta path */
        {
            DashState da, db;
            DashSimState pa, pb;
            dash_state_init(&da);
            dash_state_init(&db);
            dash_sim_init(&pa);
            dash_sim_init(&pb);
            while (pa.lap_count < 3u) { dash_sim_step(&pa, &da, 50u); }
            while (pb.lap_count < 3u) { dash_sim_step(&pb, &db, 50u); }
            expect(da.ch.delta_s == db.ch.delta_s, "determinism: delta must match exactly");
            int trace_diff = 0;
            for (uint16_t b = 0u; b < SIM_TRACE_SLOTS; b++)
            {
                if (pa.ref_trace_cs[b] != pb.ref_trace_cs[b]) { trace_diff++; }
            }
            expect(trace_diff == 0, "determinism: the reference trace must match exactly");
        }
    }

    /* ---- U6: lap position integrates the PUBLISHED speed (R9, KTD6) ---- */
    {
        DashState u6s;
        DashSimState u6m;

        /* sim-owned: distance advances by exactly the speed the dash is
         * showing. When the sim owns SPEED that is its own number, so normal
         * operation is unchanged -- this pins the two together. */
        dash_state_init(&u6s);
        dash_sim_init(&u6m);
        for (int i = 0; i < 400; i++)
        {
            float shown = u6s.ch.speed_mph;
            float before = u6m.lap_dist_ft;
            dash_sim_step(&u6m, &u6s, 50u);
            if (i > 0 && u6m.lap_dist_ft >= before)
            {
                expect(fabsf((u6m.lap_dist_ft - before)
                             - shown * SIM_FPS_PER_MPH * 0.05f) < 0.05f,
                       "lap distance must advance at exactly the displayed speed");
            }
        }

        /* overridden low: `set speed 45` genuinely crawls the circuit */
        dash_state_init(&u6s);
        dash_sim_init(&u6m);
        for (int i = 0; i < 200; i++) { dash_sim_step(&u6m, &u6s, 50u); }
        dash_ch_set(&u6s, DASH_CH_SPEED, 45.0f);
        u6s.overridden |= DASH_CH_BIT(DASH_CH_SPEED);
        float d0 = u6m.lap_dist_ft;
        uint32_t lapc0 = u6m.lap_count;
        for (int i = 0; i < 200; i++) { dash_sim_step(&u6m, &u6s, 50u); } /* 10 s */
        expect(u6m.lap_count == lapc0, "10 s at 45 mph must not complete a lap");
        expect(fabsf((u6m.lap_dist_ft - d0) - 45.0f * SIM_FPS_PER_MPH * 10.0f) < 1.0f,
               "an overridden SPEED must drive lap position at the commanded speed");

        /* overridden to zero: track position freezes, the lap clock does not */
        dash_ch_set(&u6s, DASH_CH_SPEED, 0.0f);
        float d1 = u6m.lap_dist_ft;
        uint32_t lap0 = u6m.lap_ms;
        for (int i = 0; i < 200; i++) { dash_sim_step(&u6m, &u6s, 50u); }
        expect(u6m.lap_dist_ft == d1, "SPEED overridden to 0 must freeze lap position");
        expect(u6m.lap_ms == lap0 + 10000u, "the lap timer must keep running at speed 0");

        /* cleared: there is no speed to integrate, so fall back to the sim's
         * own -- the lap continues rather than freezing forever */
        u6s.overridden = (uint32_t) (u6s.overridden & ~DASH_CH_BIT(DASH_CH_SPEED));
        u6s.cleared |= DASH_CH_BIT(DASH_CH_SPEED);
        u6s.valid = (uint32_t) (u6s.valid & ~DASH_CH_BIT(DASH_CH_SPEED));
        float d2 = u6m.lap_dist_ft;
        for (int i = 0; i < 200; i++) { dash_sim_step(&u6m, &u6s, 50u); }
        expect(!dash_ch_valid(&u6s, DASH_CH_SPEED), "a cleared SPEED must stay invalid");
        expect(u6m.lap_dist_ft > d2 + 100.0f,
               "a cleared SPEED must fall back to the sim's own speed, not freeze the lap");

        /* determinism with an override in place */
        {
            DashState oa, ob;
            DashSimState qa, qb;
            dash_state_init(&oa);
            dash_state_init(&ob);
            dash_sim_init(&qa);
            dash_sim_init(&qb);
            dash_ch_set(&oa, DASH_CH_SPEED, 45.0f);
            dash_ch_set(&ob, DASH_CH_SPEED, 45.0f);
            oa.overridden |= DASH_CH_BIT(DASH_CH_SPEED);
            ob.overridden |= DASH_CH_BIT(DASH_CH_SPEED);
            for (int i = 0; i < 2000; i++)
            {
                dash_sim_step(&qa, &oa, 50u);
                dash_sim_step(&qb, &ob, 50u);
            }
            expect(qa.lap_dist_ft == qb.lap_dist_ft,
                   "determinism under an overridden SPEED: distance");
            expect(qa.lap_ms == qb.lap_ms, "determinism under an overridden SPEED: lap time");
        }
    }

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

    /* ---- U10: step-size independence THROUGH the corners ----
     * 60 s of lap 1 barely leaves the front straight, so the check above
     * says little about corner behavior. A completed flying lap integrates
     * every arc, so its time is the real step-size invariant. */
    {
        DashState s10, s50;
        DashSimState m10, m50;
        dash_state_init(&s10);
        dash_state_init(&s50);
        dash_sim_init(&m10);
        dash_sim_init(&m50);
        while (m10.lap_count < 2u) { dash_sim_step(&m10, &s10, 10u); }
        while (m50.lap_count < 2u) { dash_sim_step(&m50, &s50, 50u); }
        expect(m10.last_ms > 0u && m50.last_ms > 0u, "both step sizes must time a flying lap");
        float rel = fabsf((float) m10.last_ms - (float) m50.last_ms) / (float) m10.last_ms;
        expect(rel < 0.02f,
               "10 ms and 50 ms steps must produce the same flying-lap time within 2%");
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
