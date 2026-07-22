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
/* U7: the sweep fixture exists to cover the RENDERER's full range, so it is
 * checked against the renderer's own thresholds (speedo top, shift ladder, tach
 * zones) rather than against numbers this test re-authors. */
#include "dash_math.h"

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

/* U10: where a corner actually ENDS on track, MEASURED by driving it rather
 * than read back off the function under test. Inside the arc the cap holds the
 * car at that run's own (skill-scaled, jittered) limit, so the first point past
 * the segment start where speed climbs clear of that limit is the arc's exit.
 * Both inputs to the arc are track geometry, so this distance must come out the
 * same at any driver skill -- which is precisely what gives skill its leverage
 * on lap time: a slower driver spends LONGER in the same length of tarmac. */
static float observed_arc_ft(float skill, uint8_t seg)
{
    DashState s;
    DashSimState m;
    dash_state_init(&s);
    dash_sim_init(&m);
    dash_sim_set_skill(&m, skill);
    while (m.lap_count < 2u) { dash_sim_step(&m, &s, 10u); } /* onto a flying lap */
    const float seg_start = SIM_SEGS[seg].end_ft - SIM_SEGS[seg].len_ft;
    while (m.lap_dist_ft < seg_start) { dash_sim_step(&m, &s, 10u); }
    while (m.seg_idx == seg)
    {
        if (m.speed_mph > dash_sim_seg_limit_mph(&m, seg) * 1.02f)
        {
            return m.lap_dist_ft - seg_start;
        }
        dash_sim_step(&m, &s, 10u);
    }
    return -1.0f; /* never left the limit: the corner filled the whole segment */
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
        const float arc_default = dash_sim_corner_arc_ft(13u);
        expect(arc_default > 200.0f,
               "T1's 160 deg over its ~82 ft radius must produce a real arc");

        /* Measured by driving T1 at two different skills, not by calling the
         * pure arc function twice on the same literal segment index -- that
         * compares a function to itself and cannot fail. */
        const float arc_slow = observed_arc_ft(0.82f, 13u);
        const float arc_fast = observed_arc_ft(0.95f, 13u);
        expect(arc_slow > 0.0f && arc_fast > 0.0f,
               "T1's arc must be observable -- the car must leave the limit inside the segment");
        expect(fabsf(arc_slow - arc_fast) < 25.0f,
               "the DRIVEN corner arc must not move with driver skill");
        expect(fabsf(arc_slow - arc_default) < 40.0f
                   && fabsf(arc_fast - arc_default) < 40.0f,
               "the driven arc must match the geometry dash_sim_corner_arc_ft derives");
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

    /* ---- mask width: channel 25 (the last channel) and DASH_CH_ALL ---- */
    {
        DashState m;
        dash_state_init(&m);
        expect(DASH_CH_COUNT == 26, "must have exactly 26 channels");
        expect(DASH_CH_SESSION == 25, "SESSION must be channel id 25 (the last channel)");
        /* The DASH_CH_BIT macro is a uint32_t shift, so the enum has room for
         * six more ids and no more. Kept well clear on purpose. */
        expect(DASH_CH_COUNT <= 30, "leave headroom under the 32-channel mask limit");

        dash_ch_set(&m, DASH_CH_SESSION, 5.0f);
        m.overridden |= DASH_CH_BIT(DASH_CH_SESSION);
        expect(dash_ch_valid(&m, DASH_CH_SESSION), "channel 25's bit must set correctly in the valid mask");
        expect((m.overridden & DASH_CH_BIT(DASH_CH_SESSION)) != 0,
               "channel 25's bit must set correctly in the overridden mask");
        m.cleared |= DASH_CH_BIT(DASH_CH_SESSION);
        expect((m.cleared & DASH_CH_BIT(DASH_CH_SESSION)) != 0,
               "channel 25's bit must set correctly in the cleared mask");

        m.valid &= (uint32_t) ~DASH_CH_BIT(DASH_CH_SESSION);
        m.overridden &= (uint32_t) ~DASH_CH_BIT(DASH_CH_SESSION);
        m.cleared &= (uint32_t) ~DASH_CH_BIT(DASH_CH_SESSION);
        expect(!dash_ch_valid(&m, DASH_CH_SESSION), "channel 25's bit must clear correctly in the valid mask");
        expect((m.overridden & DASH_CH_BIT(DASH_CH_SESSION)) == 0,
               "channel 25's bit must clear correctly in the overridden mask");
        expect((m.cleared & DASH_CH_BIT(DASH_CH_SESSION)) == 0,
               "channel 25's bit must clear correctly in the cleared mask");

        /* DASH_CH_ALL must round-trip every id 0..25 and nothing beyond */
        for (uint8_t ch = 0; ch < DASH_CH_COUNT; ch++)
        {
            expect((DASH_CH_ALL & DASH_CH_BIT(ch)) != 0,
                   "DASH_CH_ALL must cover every channel id 0..25");
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
        expect(!dash_ch_valid(&st, DASH_CH_SESSION), "street mode must never validate SESSION");
        expect(dash_ch_valid(&st, DASH_CH_AFR_L), "street mode must still validate AFR_L (engine sensor)");
        expect(dash_ch_valid(&st, DASH_CH_TIME), "street mode must still validate TIME");
        expect(st.ch.afr_l >= 11.0f && st.ch.afr_l <= 14.0f, "street afr_l must stay in [11, 14]");
    }
    /* U9/R15/V5: this assertion used to read `>= 11.99f` -- street burned
     * 1.2/40 gal/hr, so two minutes moved the gauge by 0.001 gal. It could not
     * survive the load-proportional model and is UPDATED rather than deleted:
     * street still burns visibly less than track, it just burns a real amount.
     * 3 gal/hr over two minutes is 0.1 gal. */
    expect(st.ch.fuel_gal > 11.85f && st.ch.fuel_gal < 11.95f,
           "street must burn a real but modest ~0.1 gal in two minutes");

    /* ---- U7: the range-sweep fixture (R12, S4) ----
     * A real HPR lap tops out around 170 mph in 5th, so it cannot reach the top
     * of a 200 mph speedo or ask for 6th at all. The fixture is what preserves
     * that coverage, and it inherits U3's retired "must reach 6th gear"
     * assertion outright. */
    {
        DashState ws;
        DashSimState wm;

        /* the fixture must sweep the dial the RENDERER actually draws */
        expect(SIM_SWEEP_MAX_MPH == (float) DASH_SPEED_MAX,
               "the sweep's top speed must be the speedo's own DASH_SPEED_MAX");

        dash_state_init(&ws);
        dash_sim_init(&wm);
        expect(wm.circuit == SIM_CIRCUIT_HPR, "the default circuit must be the HPR lap");

        /* HPR is never left without an explicit command: 10 minutes of running
         * must not wander into the fixture on its own */
        for (int i = 0; i < 12000; i++)
        {
            dash_sim_step(&wm, &ws, 50u);
            if (wm.circuit != SIM_CIRCUIT_HPR) { break; }
        }
        expect(wm.circuit == SIM_CIRCUIT_HPR,
               "nothing but the serial selector may enter the sweep fixture");

        /* one full cycle of the fixture, instrumented */
        dash_state_init(&ws);
        dash_sim_init(&wm);
        dash_sim_set_circuit(&wm, SIM_CIRCUIT_SWEEP);

        float v_min = 1e9f, v_max = 0.0f;
        float rpm_min = 1e9f, rpm_max = 0.0f;
        uint8_t gears_seen = 0u;
        uint8_t ladder_max = 0u;
        int amber_ticks = 0;
        int red_ticks = 0;
        float gear_secs[7] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        bool lap_ch_stayed_invalid = true;

        const int cycle_steps = (int) (SIM_SWEEP_PERIOD_S / 0.05f); /* 100 s at 50 ms */
        for (int i = 0; i < cycle_steps; i++)
        {
            dash_sim_step(&wm, &ws, 50u);

            if (ws.ch.speed_mph < v_min) { v_min = ws.ch.speed_mph; }
            if (ws.ch.speed_mph > v_max) { v_max = ws.ch.speed_mph; }
            if (ws.ch.rpm < rpm_min) { rpm_min = ws.ch.rpm; }
            if (ws.ch.rpm > rpm_max) { rpm_max = ws.ch.rpm; }
            gears_seen = (uint8_t) (gears_seen | (uint8_t) (1u << (wm.gear - 1u)));
            gear_secs[wm.gear] += 0.05f;

            uint8_t leds = dash_shift_led_count(ws.ch.rpm);
            if (leds > ladder_max) { ladder_max = leds; }
            if (dash_rpm_color(ws.ch.rpm) == DASH_COLOR_AMBER) { amber_ticks++; }
            if (dash_rpm_color(ws.ch.rpm) == DASH_COLOR_RED) { red_ticks++; }

            /* rpm must remain the gearbox's own answer, not a second ramp */
            expect(fabsf(ws.ch.speed_mph - expected_mph(ws.ch.rpm, wm.gear)) <= 2.0f
                       || ws.ch.rpm <= SIM_RPM_IDLE + 0.01f,
                   "sweep rpm must come from speed through the real gearbox");

            if (dash_ch_valid(&ws, DASH_CH_LAP) || dash_ch_valid(&ws, DASH_CH_LAST)
                || dash_ch_valid(&ws, DASH_CH_BEST) || dash_ch_valid(&ws, DASH_CH_DELTA)
                || dash_ch_valid(&ws, DASH_CH_PRED))
            {
                lap_ch_stayed_invalid = false;
            }
        }

        /* the whole dial, both ends */
        expect(v_min <= 0.01f, "the sweep must reach a standstill at the bottom of the dial");
        expect(v_max >= SIM_SWEEP_MAX_MPH - 0.5f,
               "the sweep must reach the top of the 200 mph speedo");

        /* INHERITED from U3, which retired it when HPR could no longer satisfy
         * it: the fixture exists precisely so this assertion still has a home. */
        expect(gears_seen == 0x3Fu, "the sweep must visit all six gears");
        for (uint8_t g = 1u; g <= 6u; g++)
        {
            expect(gear_secs[g] >= 2.0f,
                   "every gear must be on screen long enough for a human to see it");
        }

        /* the tach's own range, measured with the renderer's thresholds */
        expect(rpm_min <= SIM_RPM_IDLE + 0.01f, "the sweep must drop the tach to idle");
        expect(rpm_max >= (float) DASH_AMBER_RPM,
               "the sweep must take the tach into its amber zone");
        expect(amber_ticks > 20, "the amber zone must be held long enough to read");

        /* The tach's RED zone needs rpm strictly above DASH_SHIFT_RPM and the
         * 15th shift LED needs DASH_REDLINE_RPM, neither of which the DRIVING
         * model can produce: it upshifts at SIM_UPSHIFT_RPM and limits at
         * SIM_RPM_MAX, both at or below the shift point, because a real shift
         * happens BEFORE the shift light goes red. That is correct for TRACK
         * and it is still pinned here -- but it left two of the four things
         * R12 says this fixture exists to exercise (redline, the shift-light
         * ladder) unreachable in every mode.
         *
         * SWEEP is therefore exempt from the DRIVING upshift point and holds
         * each gear to its own limiter, SIM_SWEEP_RPM_MAX. It is the same
         * class of exception as skipping the corner table and lookahead
         * braking (KTD7): a display exerciser, not a driving sim. The
         * exemption is to the SHIFT POINT only -- rpm still comes from road
         * speed through the same gearbox, checked step by step above. */
        expect(SIM_RPM_MAX <= (float) DASH_SHIFT_RPM,
               "the DRIVING rev limit must stay at or below the shift point -- "
               "which is exactly why SWEEP needs a limiter of its own");
        expect(SIM_SWEEP_RPM_MAX >= (float) DASH_REDLINE_RPM,
               "the sweep's limiter must reach the redline the 15th LED asks for");
        expect(rpm_max >= (float) DASH_REDLINE_RPM,
               "the sweep must take the tach all the way to the redline");
        expect(red_ticks > 20, "the red zone must be held long enough to read");
        expect(ladder_max == (uint8_t) DASH_SHIFT_LED_COUNT,
               "the sweep must light the whole 15-LED shift ladder");

        /* lap timing has no meaning here (dead-front convention) */
        expect(lap_ch_stayed_invalid,
               "LAP/LAST/BEST/DELTA/PRED must stay invalid throughout the sweep");
        expect(!dash_ch_valid(&ws, DASH_CH_LAPN), "LAPN must stay invalid in the sweep");
        expect(!dash_ch_valid(&ws, DASH_CH_POS), "POS must stay invalid in the sweep");
        expect(!dash_ch_valid(&ws, DASH_CH_SESSION),
               "SESSION must stay invalid in the sweep -- the session clock does not run");
        expect(wm.lap_count == 0u, "the sweep must never complete a lap");
        expect(wm.lap_ms == 0u, "the sweep must never run the lap clock");
        expect(wm.lap_dist_ft == 0.0f, "the sweep must never advance lap position");

        /* engine sensors keep running -- the fixture is not a freeze */
        expect(dash_ch_valid(&ws, DASH_CH_RPM) && dash_ch_valid(&ws, DASH_CH_SPEED),
               "the sweep must still publish rpm and speed");
        expect(dash_ch_valid(&ws, DASH_CH_AFR_L) && dash_ch_valid(&ws, DASH_CH_TIME),
               "the sweep must still publish the always-on engine channels");

        /* switching back resumes the lap simulation with no stale lap in flight */
        dash_sim_set_circuit(&wm, SIM_CIRCUIT_HPR);
        expect(wm.lap_dist_ft == 0.0f && wm.lap_ms == 0u,
               "returning to HPR must start a clean lap, not resume a half-driven one");
        uint32_t laps_before = wm.lap_count;
        for (int i = 0; i < 4000; i++) { dash_sim_step(&wm, &ws, 50u); }
        expect(wm.lap_count > laps_before, "HPR must complete laps again after the sweep");
        expect(dash_ch_valid(&ws, DASH_CH_LAP), "LAP must be published again under HPR");
        expect(wm.lap_dist_ft >= 0.0f && wm.lap_dist_ft < SIM_TRACK_LAP_FT,
               "lap position must be back inside the circuit after the sweep");

        /* determinism, and step-size independence of the ramp itself */
        {
            DashState wa, wb;
            DashSimState pa, pb;
            dash_state_init(&wa);
            dash_state_init(&wb);
            dash_sim_init(&pa);
            dash_sim_init(&pb);
            dash_sim_set_circuit(&pa, SIM_CIRCUIT_SWEEP);
            dash_sim_set_circuit(&pb, SIM_CIRCUIT_SWEEP);
            for (int i = 0; i < 1500; i++)
            {
                dash_sim_step(&pa, &wa, 50u);
                dash_sim_step(&pb, &wb, 50u);
            }
            expect(wa.ch.speed_mph == wb.ch.speed_mph, "determinism in SWEEP: speed");
            expect(wa.ch.rpm == wb.ch.rpm, "determinism in SWEEP: rpm");
            expect(pa.gear == pb.gear, "determinism in SWEEP: gear");
            expect(pa.sweep_t_s == pb.sweep_t_s, "determinism in SWEEP: ramp phase");
        }
        {
            DashState w10, w50;
            DashSimState p10, p50;
            dash_state_init(&w10);
            dash_state_init(&w50);
            dash_sim_init(&p10);
            dash_sim_init(&p50);
            dash_sim_set_circuit(&p10, SIM_CIRCUIT_SWEEP);
            dash_sim_set_circuit(&p50, SIM_CIRCUIT_SWEEP);
            for (int i = 0; i < 3000; i++) { dash_sim_step(&p10, &w10, 10u); } /* 30 s */
            for (int i = 0; i < 600; i++) { dash_sim_step(&p50, &w50, 50u); }  /* 30 s */
            expect(fabsf(w10.ch.speed_mph - w50.ch.speed_mph) < 1.0f,
                   "10 ms and 50 ms steps must ramp the sweep to the same speed");
            expect(p10.gear == p50.gear, "both step sizes must land in the same gear");
        }
    }

    /* ---- U7: leaving the lap simulation must DEAD-FRONT the lap channels ----
     * The STREET block above boots straight into STREET, so those channels were
     * never valid in the first place and it cannot see this. Switching modes
     * after laps have run can: nothing re-publishes LAP/LAST/BEST/DELTA in
     * STREET, so without an explicit invalidate they keep their last TRACK
     * values and the STREET screen shows a stale best-lap time forever. U11's
     * USER-button short press makes that one button press away. */
    {
        DashState sw;
        DashSimState swm;
        dash_state_init(&sw);
        dash_sim_init(&swm);
        while (swm.lap_count < 2u) { dash_sim_step(&swm, &sw, 50u); }
        expect(dash_ch_valid(&sw, DASH_CH_LAP), "TRACK must publish LAP before the switch");
        expect(dash_ch_valid(&sw, DASH_CH_BEST), "TRACK must publish BEST before the switch");

        sw.mode = DASH_MODE_STREET;
        dash_sim_step(&swm, &sw, 50u);
        expect(!dash_ch_valid(&sw, DASH_CH_LAP),
               "TRACK -> STREET must dead-front LAP, not strand its last value");
        expect(!dash_ch_valid(&sw, DASH_CH_LAST),
               "TRACK -> STREET must dead-front LAST");
        expect(!dash_ch_valid(&sw, DASH_CH_BEST),
               "TRACK -> STREET must dead-front BEST");
        expect(!dash_ch_valid(&sw, DASH_CH_DELTA),
               "TRACK -> STREET must dead-front DELTA");
        expect(!dash_ch_valid(&sw, DASH_CH_PRED),
               "TRACK -> STREET must dead-front PRED");
        expect(!dash_ch_valid(&sw, DASH_CH_LAPN),
               "TRACK -> STREET must dead-front LAPN");
        expect(!dash_ch_valid(&sw, DASH_CH_POS),
               "TRACK -> STREET must dead-front POS");
        expect(!dash_ch_valid(&sw, DASH_CH_THROTTLE),
               "TRACK -> STREET must dead-front THROTTLE");
        expect(!dash_ch_valid(&sw, DASH_CH_BRAKE),
               "TRACK -> STREET must dead-front BRAKE");
        expect(!dash_ch_valid(&sw, DASH_CH_SESSION),
               "TRACK -> STREET must dead-front SESSION: there is no session in STREET");

        /* ...and an operator's forced lap value is still the operator's: the
         * dead-fronting goes through dash_ch_invalidate's ownership guard. */
        dash_ch_set(&sw, DASH_CH_BEST, 90000.0f);
        sw.overridden |= DASH_CH_BIT(DASH_CH_BEST);
        dash_sim_step(&swm, &sw, 50u);
        expect(dash_ch_valid(&sw, DASH_CH_BEST) && sw.ch.best_ms == 90000u,
               "an overridden BEST must survive the STREET dead-fronting");

        /* switching back resumes lap publishing */
        sw.overridden = 0u;
        sw.mode = DASH_MODE_TRACK;
        dash_sim_step(&swm, &sw, 50u);
        expect(dash_ch_valid(&sw, DASH_CH_LAP),
               "STREET -> TRACK must publish LAP again");
    }

    /* ---- U8: the 20-minute session cycle and the split thermal model ----
     * One instrumented run through a full session and into the next, which is
     * where every claim in this unit actually lives: the rollover lands on a
     * lap boundary, the temperatures come back cold, and the two fluids move on
     * visibly different constants. */
    {
        DashState es;
        DashSimState em;
        dash_state_init(&es);
        dash_sim_init(&em);

        expect(em.session_ms == 0u && !em.session_pending,
               "a fresh sim must open a fresh session");
        expect(SIM_SESSION_MS == 1200000u, "a session must be 20 minutes");
        expect(SIM_OILT_TAU_S > 3.0f * SIM_ECT_TAU_S,
               "oil must warm on a far longer constant than coolant -- one shared "
               "constant is exactly what pinned both gauges by minute five");
        expect(SIM_OILT_TRACK_F > SIM_ECT_OPERATING_F,
               "oil's target must sit above coolant's regulated plateau");
        expect(SIM_OILT_TRACK_F > SIM_OILT_STREET_F,
               "a track session must run the oil hotter than a street cruise");

        float ect_at[26], oilt_at[26];
        for (int k = 0; k < 26; k++) { ect_at[k] = 0.0f; oilt_at[k] = 0.0f; }
        float oilt_peak = 0.0f; /* the hottest oil ever seen, reset included */
        int minute = 0;
        uint32_t reset_at_ms = 0u;      /* session_ms the instant before the reset */
        float dist_before_reset = -1.0f;
        float dist_after_reset = -1.0f;
        float ect_after_reset = -1.0f;
        float oilt_after_reset = -1.0f;
        uint32_t best_before_reset = 0u;
        uint32_t max_lap_count = 0u;
        uint32_t laps_in_session1 = 0u;
        uint32_t out_lap_ms_s1 = 0u;
        int resets = 0;
        bool delta_valid_on_new_outlap = false;

        /* 25 sim-minutes: comfortably past one rollover (20 min + the in-lap) */
        for (int i = 0; i < 30000; i++)
        {
            uint32_t sess_before = em.session_ms;
            float dist_pre = em.lap_dist_ft;
            uint32_t best_pre = em.best_ms;
            if (em.lap_count > max_lap_count) { max_lap_count = em.lap_count; }
            if (em.lap_count == 1u && out_lap_ms_s1 == 0u) { out_lap_ms_s1 = em.last_ms; }

            dash_sim_step(&em, &es, 50u);

            if (em.oilt_f > oilt_peak) { oilt_peak = em.oilt_f; }

            if (em.session_ms < sess_before) /* the session just rolled over */
            {
                resets++;
                if (resets == 1)
                {
                    reset_at_ms = sess_before;
                    dist_before_reset = dist_pre;
                    dist_after_reset = em.lap_dist_ft;
                    ect_after_reset = em.ect_f;
                    oilt_after_reset = em.oilt_f;
                    best_before_reset = best_pre;
                    laps_in_session1 = max_lap_count;
                }
                max_lap_count = 0u;
            }
            /* the new session's out-lap must be dead-fronted again: the old
             * session's reference trace is not a benchmark for this one */
            if (resets == 1 && em.lap_count == 0u && dash_ch_valid(&es, DASH_CH_DELTA))
            {
                delta_valid_on_new_outlap = true;
            }

            int m_now = (int) (em.t_s / 60.0f);
            if (m_now > minute && m_now <= 25)
            {
                minute = m_now;
                ect_at[m_now] = em.ect_f;
                oilt_at[m_now] = em.oilt_f;
            }
        }

        expect(resets >= 1, "a 25-minute run must roll the session over at least once");

        /* R14: the reset lands ON a lap boundary, never mid-lap. The car was
         * within a step of the finish line before it and back at the line after
         * -- which is the whole reason the reset is free of discontinuity. */
        expect(dist_before_reset > SIM_TRACK_LAP_FT - 400.0f,
               "the session reset must fire at a lap crossing, not mid-lap");
        expect(dist_after_reset == 0.0f,
               "lap distance must be exactly the start/finish line after a reset");

        /* the session runs AT LEAST its 20 minutes, and at most one lap longer */
        expect(reset_at_ms >= SIM_SESSION_MS,
               "a session must run its full 20 minutes before ending");
        expect(reset_at_ms <= SIM_SESSION_MS + 150000u,
               "a session must not overrun 20 minutes by more than one lap");
        expect(laps_in_session1 >= 8u,
               "a 20-minute session at ~2:02 laps must complete at least 8 laps");

        /* Temperatures come back cold and climb again. Sampled at the end of
         * the step that reset them, so each has already taken one step's worth
         * of warming back toward target -- hence "at cold start" rather than
         * "exactly SIM_COLD_START_F". */
        expect(ect_after_reset < SIM_COLD_START_F + 3.0f
                   && oilt_after_reset < SIM_COLD_START_F + 3.0f,
               "the session reset must return both temperatures to cold start");
        expect(ect_at[25] > 150.0f && oilt_at[25] > 100.0f,
               "both temperatures must climb again in the new session");

        /* the split constants, measured rather than asserted from the source:
         * coolant is DONE by minute 10 and moves under 2 F over the next ten,
         * while oil moves tens of degrees over the same window. */
        expect(fabsf(ect_at[5] - SIM_ECT_OPERATING_F) < 12.0f,
               "coolant must be within 12 F of its plateau by minute 5");
        expect(fabsf(ect_at[20] - ect_at[10]) < 2.0f,
               "coolant must be settled: under 2 F of movement from minute 10 to 20");
        expect((oilt_at[20] - oilt_at[10]) > 25.0f,
               "oil must still be climbing hard from minute 10 to 20, not settled");
        /* the S6 headline claim, pinned on its own: this fails the moment oil
         * is retuned to settle early, which is the failure mode this unit
         * exists to prevent. */
        expect((oilt_at[15] - oilt_at[10]) > 10.0f,
               "oil at minute 15 must be measurably hotter than at minute 10");
        /* 25 F, down from 30: raising the oil target to a track figure steepens
         * the early climb (same 600 s tau, a much bigger gap to close), so oil
         * is ~28 F behind coolant at minute 5 where it used to be ~49. The
         * CLAIM is unchanged and still has real margin -- oil lags coolant
         * early -- only the size of the lag moved, and it moved because the
         * calibration is more realistic, not because the model went soft. */
        expect(oilt_at[5] < ect_at[5] - 25.0f,
               "early in the session oil must lag well behind coolant");
        expect(oilt_at[20] > ect_at[20],
               "by the end of the session oil must run hotter than coolant");
        /* The number the gauge actually lands on. A 500+ whp car on a road
         * course runs 250-280 F oil, and the target is calibrated so a
         * 20-minute session arrives at the bottom of that band rather than at
         * a figure chosen to dodge the warning thresholds -- the thresholds
         * moved to 270/290 to meet it (owner decision, follow-up to U8). */
        expect(oilt_at[20] > 245.0f && oilt_at[20] < 265.0f,
               "a 20-minute session must end with the oil around 255 F, "
               "which is what this car actually runs on track");

        /* ...and it must still not cook itself into a permanent amber gauge.
         * Measured over the WHOLE run, not just the minute-20 sample: the
         * session's true peak is at the rollover, which lands up to one in-lap
         * PAST the 20-minute mark. */
        expect(oilt_at[20] < DASH_OILT_AMBER_F,
               "a normal session must not end with the oil gauge stuck in amber");
        expect(dash_oil_temp_state(oilt_peak) == DASH_COLOR_NORMAL,
               "the hottest oil a normal session ever reaches must stay out of "
               "amber -- a permanently amber gauge is a broken calibration");

        /* STREET is the other half of the calibration and it is NOT a scaled
         * copy of TRACK's: STREET cruises indefinitely, so its target is an
         * asymptote the gauge actually arrives at, while TRACK's is one the
         * session always ends before reaching. A street car does not run
         * 255 F oil, so the settled figure is checked directly. */
        {
            DashState cs;
            DashSimState cm;
            dash_state_init(&cs);
            dash_sim_init(&cm);
            cs.mode = DASH_MODE_STREET;
            for (int i = 0; i < 60000; i++) { dash_sim_step(&cm, &cs, 50u); }
            expect(cm.oilt_f > SIM_ECT_OPERATING_F && cm.oilt_f < 230.0f,
                   "a settled street cruise must run the oil just above coolant, "
                   "nowhere near a track session's 255 F");
            expect(dash_oil_temp_state(cm.oilt_f) == DASH_COLOR_NORMAL,
                   "a street cruise must never colour the oil gauge");
        }

        /* the out-lap rule is per SESSION, not per boot: best_ms is cleared, so
         * the new session's ~7 mph rollout cannot become the benchmark */
        expect(best_before_reset > 0u, "the first session must have set a best lap");
        {
            DashState bs;
            DashSimState bm;
            dash_state_init(&bs);
            dash_sim_init(&bm);
            while (bm.session_ms < SIM_SESSION_MS) { dash_sim_step(&bm, &bs, 50u); }
            while (bm.session_pending) { dash_sim_step(&bm, &bs, 50u); }
            expect(bm.best_ms == 0u && bm.lap_count == 0u,
                   "a session reset must clear the lap book, best_ms included");
            expect(!bm.ref_valid, "a session reset must clear the U5 delta reference");
            /* run the new session's out-lap and the flying lap after it */
            uint32_t out_lap = 0u;
            while (bm.lap_count < 1u) { dash_sim_step(&bm, &bs, 50u); }
            out_lap = bm.last_ms;
            expect(bm.best_ms == 0u,
                   "the new session's OUT-LAP must never be adopted as best_ms");
            while (bm.lap_count < 2u) { dash_sim_step(&bm, &bs, 50u); }
            expect(bm.best_ms > 0u && bm.best_ms < out_lap,
                   "the new session's first FLYING lap must set its best, and beat "
                   "the out-lap");
        }
        expect(!delta_valid_on_new_outlap,
               "DELTA must be dead-fronted again through the new session's out-lap");
        (void) out_lap_ms_s1;

        /* successive sessions must NOT be bit-identical: the LCG survives the
         * reset, so the new session draws fresh corner jitter rather than
         * replaying the last one exactly. */
        {
            DashState js;
            DashSimState jm;
            dash_state_init(&js);
            dash_sim_init(&jm);
            uint32_t s1_best, s2_best;
            float s1_jit13, s2_jit13;
            while (jm.lap_count < 3u) { dash_sim_step(&jm, &js, 50u); }
            s1_best = jm.best_ms;
            s1_jit13 = jm.seg_jitter_mph[13];
            while (jm.session_ms < SIM_SESSION_MS) { dash_sim_step(&jm, &js, 50u); }
            while (jm.session_pending) { dash_sim_step(&jm, &js, 50u); }
            while (jm.lap_count < 3u) { dash_sim_step(&jm, &js, 50u); }
            s2_best = jm.best_ms;
            s2_jit13 = jm.seg_jitter_mph[13];
            expect(s1_best != s2_best || s1_jit13 != s2_jit13,
                   "successive sessions must not be bit-identical replays: the "
                   "jitter LCG must survive the reset");
        }

        /* determinism must survive the session boundary too */
        {
            DashState da, db;
            DashSimState pa, pb;
            dash_state_init(&da);
            dash_state_init(&db);
            dash_sim_init(&pa);
            dash_sim_init(&pb);
            for (int i = 0; i < 27000; i++) /* 22.5 min: past one rollover */
            {
                dash_sim_step(&pa, &da, 50u);
                dash_sim_step(&pb, &db, 50u);
            }
            expect(pa.session_ms == pb.session_ms, "determinism across a session: clock");
            expect(pa.lap_dist_ft == pb.lap_dist_ft, "determinism across a session: distance");
            expect(pa.ect_f == pb.ect_f && pa.oilt_f == pb.oilt_f,
                   "determinism across a session: temperatures");
            expect(pa.fuel_gal == pb.fuel_gal, "determinism across a session: fuel");
        }

        /* the session clock is TRACK-only: STREET has no laps, so a
         * lap-gated rollover would have nothing to fire on */
        {
            DashState ts;
            DashSimState tm;
            dash_state_init(&ts);
            dash_sim_init(&tm);
            for (int i = 0; i < 600; i++) { dash_sim_step(&tm, &ts, 50u); } /* 30 s TRACK */
            uint32_t sess = tm.session_ms;
            expect(sess > 0u, "TRACK must advance the session clock");
            ts.mode = DASH_MODE_STREET;
            for (int i = 0; i < 2400; i++) { dash_sim_step(&tm, &ts, 50u); } /* 2 min */
            expect(tm.session_ms == sess, "time spent in STREET must not advance the session");
            /* nor may the SWEEP fixture, which has no laps either -- a clock
             * that ran there would strand the session pending-end until the
             * forced-reset bound fired in the middle of the ramp */
            ts.mode = DASH_MODE_TRACK;
            dash_sim_set_circuit(&tm, SIM_CIRCUIT_SWEEP);
            for (int i = 0; i < 2400; i++) { dash_sim_step(&tm, &ts, 50u); }
            expect(tm.session_ms == sess, "the SWEEP fixture must not advance the session");
        }

        /* the bounded exception: `set speed 0` stalls the lap forever, so the
         * pending-end must not wait forever with it */
        {
            DashState zs;
            DashSimState zm;
            dash_state_init(&zs);
            dash_sim_init(&zm);
            while (!zm.session_pending) { dash_sim_step(&zm, &zs, 50u); }
            dash_ch_set(&zs, DASH_CH_SPEED, 0.0f);
            zs.overridden |= DASH_CH_BIT(DASH_CH_SPEED);
            int steps = 0;
            while (zm.session_pending && steps < 20000) { dash_sim_step(&zm, &zs, 50u); steps++; }
            expect(!zm.session_pending,
                   "an overridden SPEED of 0 must not strand the session pending-end");
            expect((uint32_t) steps * 50u <= SIM_SESSION_PENDING_MAX_MS + 1000u,
                   "the forced reset must fire within one nominal lap plus margin");
            expect(zm.ect_f < SIM_COLD_START_F + 3.0f,
                   "the forced reset is still a real session reset");
        }

        /* the oil-pressure alarm's rpm >= 500 gate must survive the cold start:
         * the rollout is ~950 rpm, above the gate, and oil pressure at that rpm
         * must already be clear of the alarm threshold -- no spurious takeover
         * on the opening lap of every session. */
        {
            DashState as;
            DashSimState am;
            dash_state_init(&as);
            dash_sim_init(&am);
            for (int i = 0; i < 200; i++)
            {
                dash_sim_step(&am, &as, 50u);
                expect(as.ch.rpm >= DASH_ENGINE_RUNNING_RPM,
                       "the rollout must idle above the engine-running gate");
                expect(dash_alarm_classify(&as) != DASH_ALARM_OILP,
                       "a cold-start rollout must not raise the oil-pressure alarm");
            }
        }
    }

    /* ---- U9: load-proportional fuel burn (R15, S7) ---- */
    {
        /* one flying lap must cost roughly the calibrated figure. A band, not
         * an equality: burn is load-dependent, so no two laps agree exactly. */
        DashState fs;
        DashSimState fm;
        dash_state_init(&fs);
        dash_sim_init(&fm);
        while (fm.lap_count < 2u) { dash_sim_step(&fm, &fs, 50u); }
        float f0 = fm.fuel_gal;
        while (fm.lap_count < 3u) { dash_sim_step(&fm, &fs, 50u); }
        float lap_gal = f0 - fm.fuel_gal;
        expect(lap_gal > 0.45f && lap_gal < 0.75f,
               "a flying lap must cost roughly the calibrated 0.6 gal, not the "
               "0.04 gal the old flat rate burned");
        /* and the renderer's own constant must agree with it -- the deliberate
         * decoupling in dash_math.h is retired (U9) */
        expect(fabsf(lap_gal - DASH_LAP_BURN_GAL) < 0.06f,
               "DASH_LAP_BURN_GAL must now agree with the sim's measured per-lap "
               "burn: the two are no longer deliberately independent");

        /* driving harder burns faster. NOTE the honest form of this claim: the
         * per-lap TOTAL barely moves (a quicker lap is over sooner, so the
         * extra power is spent for less time -- the two nearly cancel, which is
         * a genuine and rather pleasing emergent result). What moves clearly is
         * the RATE, so that is what is pinned here, with the weak per-lap
         * direction checked alongside it. */
        {
            float gal_slow, gal_fast;
            uint32_t ms_slow, ms_fast;
            DashState a; DashSimState b;

            dash_state_init(&a); dash_sim_init(&b);
            while (b.lap_count < 2u) { dash_sim_step(&b, &a, 50u); }
            dash_sim_set_skill(&b, 0.80f);
            float g0 = b.fuel_gal;
            while (b.lap_count < 3u) { dash_sim_step(&b, &a, 50u); }
            gal_slow = g0 - b.fuel_gal;
            ms_slow = b.last_ms;

            dash_state_init(&a); dash_sim_init(&b);
            while (b.lap_count < 2u) { dash_sim_step(&b, &a, 50u); }
            dash_sim_set_skill(&b, 1.00f);
            g0 = b.fuel_gal;
            while (b.lap_count < 3u) { dash_sim_step(&b, &a, 50u); }
            gal_fast = g0 - b.fuel_gal;
            ms_fast = b.last_ms;

            expect(ms_fast < ms_slow, "the tidier lap must be the quicker one");
            expect((gal_fast / (float) ms_fast) > (gal_slow / (float) ms_slow) * 1.05f,
                   "a lap spent harder on the throttle must burn at a visibly "
                   "higher rate");
            expect(gal_fast > gal_slow,
                   "and cost more per lap, even if only slightly");
        }

        /* fuel does NOT reset with the session: it is the one thing that
         * carries across, so the tank and the thermal cycle run on different
         * periods and a cold-start out-lap regularly happens half empty */
        {
            DashState cs;
            DashSimState cm;
            dash_state_init(&cs);
            dash_sim_init(&cm);
            float fuel_pre = 0.0f, fuel_post = 0.0f;
            uint32_t sess_before;
            for (;;)
            {
                sess_before = cm.session_ms;
                fuel_pre = cm.fuel_gal;
                dash_sim_step(&cm, &cs, 50u);
                if (cm.session_ms < sess_before) { fuel_post = cm.fuel_gal; break; }
            }
            expect(fabsf(fuel_post - fuel_pre) < 0.01f,
                   "fuel must NOT reset on a session boundary");
            expect(fuel_pre > 5.0f && fuel_pre < 8.0f,
                   "a 20-minute session must burn roughly half a 12 gallon tank");

            /* ...and keeps depleting into the next session */
            for (int i = 0; i < 6000; i++) { dash_sim_step(&cm, &cs, 50u); } /* 5 min */
            expect(cm.fuel_gal < fuel_post - 1.0f,
                   "fuel must keep depleting into the next session");

            /* a tank is deliberately about two sessions, not one */
            float per_session = SIM_FUEL_START_GAL - fuel_pre;
            expect((SIM_FUEL_START_GAL / per_session) > 1.7f
                   && (SIM_FUEL_START_GAL / per_session) < 2.5f,
                   "a 12 gallon tank must span roughly two sessions");
        }

        /* refill on EMPTY, at a lap boundary, never mid-lap -- and therefore
         * never pinned at zero indefinitely */
        {
            DashState rs;
            DashSimState rm;
            dash_state_init(&rs);
            dash_sim_init(&rm);
            int refills = 0;
            int zero_run = 0, zero_run_max = 0;
            float prev = rm.fuel_gal;
            float prev_dist = rm.lap_dist_ft;
            for (int i = 0; i < 240000; i++) /* 200 sim-minutes, many tanks */
            {
                dash_sim_step(&rm, &rs, 50u);
                if (rm.fuel_gal > prev + 0.01f) /* the tank just filled */
                {
                    refills++;
                    /* the same step burns its own slice after the fill, so this
                     * is "a full tank", not a bit-exact SIM_FUEL_START_GAL */
                    expect(rm.fuel_gal > SIM_FUEL_START_GAL - 0.01f
                               && rm.fuel_gal <= SIM_FUEL_START_GAL,
                           "a refill must fill the tank to SIM_FUEL_START_GAL");
                    expect(prev_dist > SIM_TRACK_LAP_FT - 400.0f,
                           "a refill must happen at a lap boundary, never mid-lap");
                }
                if (rm.fuel_gal <= 0.0f) { zero_run++; if (zero_run > zero_run_max) { zero_run_max = zero_run; } }
                else { zero_run = 0; }
                prev = rm.fuel_gal;
                prev_dist = rm.lap_dist_ft;
            }
            expect(refills >= 3, "200 sim-minutes must run the tank dry and refill it several times");
            expect(rm.fuel_gal > 0.0f, "fuel must not be sitting at zero at the end of a long run");
            /* it DOES sit at zero -- from running dry mid-lap until the lap
             * ends -- but for a bounded stretch, not indefinitely */
            expect(zero_run_max * 50u < 150000u,
                   "a dry tank must be refilled within one lap, not left dead");
        }

        /* a frozen sim burns nothing */
        {
            DashState zs;
            DashSimState zm;
            dash_state_init(&zs);
            dash_sim_init(&zm);
            for (int i = 0; i < 200; i++) { dash_sim_step(&zm, &zs, 50u); }
            float held = zm.fuel_gal;
            zs.sim_frozen = true;
            for (int i = 0; i < 200; i++) { dash_sim_step(&zm, &zs, 50u); }
            expect(zm.fuel_gal == held, "a frozen sim must burn no fuel");
            zs.sim_frozen = false;
        }

        /* STREET burns visibly less per unit time than TRACK */
        {
            DashState ks, us;
            DashSimState km, um;
            dash_state_init(&ks);
            dash_state_init(&us);
            dash_sim_init(&km);
            dash_sim_init(&um);
            us.mode = DASH_MODE_STREET;
            for (int i = 0; i < 2400; i++) /* 2 sim-minutes each */
            {
                dash_sim_step(&km, &ks, 50u);
                dash_sim_step(&um, &us, 50u);
            }
            float track_burn = SIM_FUEL_START_GAL - km.fuel_gal;
            float street_burn = SIM_FUEL_START_GAL - um.fuel_gal;
            expect(street_burn > 0.05f, "street must burn a visible amount, not nothing");
            expect(street_burn * 3.0f < track_burn,
                   "street must burn several times less per unit time than track");
        }

        /* dash_laps_remaining must now agree with what the sim actually does --
         * the point of retiring the decoupling. Predicted from a full tank
         * against laps actually driven to empty, within a lap. */
        {
            DashState ls;
            DashSimState lm;
            dash_state_init(&ls);
            dash_sim_init(&lm);
            float predicted = 0.0f;
            expect(dash_laps_remaining(lm.fuel_gal, true, &predicted),
                   "laps-remaining must compute from a full tank");
            uint32_t laps = 0u;
            for (int i = 0; i < 60000 && lm.fuel_gal > 0.0f; i++)
            {
                uint32_t lc = lm.lap_count;
                uint32_t sess = lm.session_ms;
                dash_sim_step(&lm, &ls, 50u);
                /* a session reset zeroes lap_count, so count crossings, not the
                 * counter -- the tank spans more than one session by design */
                if (lm.lap_count > lc || lm.session_ms < sess) { laps++; }
            }
            expect(lm.fuel_gal <= 0.0f, "the tank must actually run dry inside 50 minutes");
            expect(fabsf((float) laps - predicted) <= 1.5f,
                   "dash_laps_remaining must agree with the sim's observed burn "
                   "to within about a lap");
        }

        /* determinism through the whole fuel path */
        {
            DashState fa, fb;
            DashSimState qa, qb;
            dash_state_init(&fa);
            dash_state_init(&fb);
            dash_sim_init(&qa);
            dash_sim_init(&qb);
            for (int i = 0; i < 5000; i++)
            {
                dash_sim_step(&qa, &fa, 50u);
                dash_sim_step(&qb, &fb, 50u);
            }
            expect(qa.fuel_gal == qb.fuel_gal, "determinism: fuel must match exactly");
        }
    }

    /* ---- the lap book must not outlive its session ----
     * dash_sim_session_reset zeroes lap_count / last_ms / best_ms, but
     * dash_ch_set is one-way, so without an explicit dead-front the dash keeps
     * showing the PREVIOUS session's LAST, BEST and PRED as live values right
     * through the new out-lap. DELTA already handled this correctly; these
     * three did not. The out-lap exclusion (U1) is a per-SESSION rule, so BEST
     * must stay dead-fronted for the out-lap AND the first flying lap. */
    {
        DashState rs;
        DashSimState rm;
        dash_state_init(&rs);
        dash_sim_init(&rm);
        uint32_t sess_before;
        for (;;)
        {
            sess_before = rm.session_ms;
            dash_sim_step(&rm, &rs, 50u);
            if (rm.session_ms < sess_before) { break; } /* the rollover step */
        }
        expect(rm.lap_count == 0u, "the rollover must leave the new session on its out-lap");
        expect(!dash_ch_valid(&rs, DASH_CH_LAST),
               "LAST must dead-front at a session rollover, not strand the old session's time");
        expect(!dash_ch_valid(&rs, DASH_CH_BEST),
               "BEST must dead-front at a session rollover");
        expect(!dash_ch_valid(&rs, DASH_CH_PRED),
               "PRED must dead-front at a session rollover");
        expect(!dash_ch_valid(&rs, DASH_CH_DELTA),
               "DELTA must dead-front at a session rollover (it already did)");

        /* LAST returns as soon as the out-lap completes; BEST must wait for a
         * flying lap, exactly as it does from a cold boot. */
        while (rm.lap_count < 1u) { dash_sim_step(&rm, &rs, 50u); }
        expect(dash_ch_valid(&rs, DASH_CH_LAST),
               "LAST must return once the new session's out-lap completes");
        expect(!dash_ch_valid(&rs, DASH_CH_BEST),
               "BEST must stay dead-fronted through the new session's out-lap");
        while (rm.lap_count < 2u) { dash_sim_step(&rm, &rs, 50u); }
        expect(dash_ch_valid(&rs, DASH_CH_BEST),
               "BEST must return once the new session has a flying lap");
    }

    /* ---- SESSION: the count-up session clock, published as a channel ----
     * The sim already ran session_ms to drive the 20-minute rollover; this
     * publishes it. Count-UP is the researched convention (no surveyed
     * platform ships a race countdown), and the only reset is the existing
     * rollover -- there is deliberately no manual/gesture reset until a real
     * session start arrives over CAN. */
    {
        DashState ss;
        DashSimState sm;
        dash_state_init(&ss);
        dash_sim_init(&sm);

        dash_sim_step(&sm, &ss, 50u);
        expect(dash_ch_valid(&ss, DASH_CH_SESSION),
               "SESSION must publish from the very first TRACK step");
        expect(ss.ch.session_ms == 50u, "SESSION must publish the elapsed session time in ms");

        /* it counts UP, monotonically, and only ever falls at a rollover */
        uint32_t prev_sess = ss.ch.session_ms;
        int rollovers = 0;
        uint32_t peak_sess = 0u;
        for (int i = 0; i < 30000; i++) /* 25 sim-minutes: past one full session */
        {
            dash_sim_step(&sm, &ss, 50u);
            expect(ss.ch.session_ms == sm.session_ms,
                   "SESSION must publish the simulator's own session clock, not a copy that drifts");
            if (ss.ch.session_ms < prev_sess)
            {
                rollovers++;
                expect(prev_sess >= SIM_SESSION_MS,
                       "the session clock may only restart at the 20-minute rollover");
            }
            else
            {
                expect(ss.ch.session_ms >= prev_sess,
                       "the session clock must count UP, never down");
            }
            if (ss.ch.session_ms > peak_sess) { peak_sess = ss.ch.session_ms; }
            prev_sess = ss.ch.session_ms;
        }
        expect(rollovers >= 1, "25 sim-minutes must cross at least one session rollover");
        /* U8: the flag drops at 20:00 but the session ends at the in-lap, so
         * the clock legitimately runs past 20:00 -- and never past the bounded
         * pending-end escape hatch. */
        expect(peak_sess > SIM_SESSION_MS,
               "the session must keep counting through the in-lap after the flag");
        expect(peak_sess < SIM_SESSION_MS + SIM_SESSION_PENDING_MAX_MS,
               "the session clock must stay inside the bounded pending-end window");

        /* an operator's forced value is still theirs (the ownership guard) */
        dash_ch_set(&ss, DASH_CH_SESSION, 123456.0f);
        ss.overridden |= DASH_CH_BIT(DASH_CH_SESSION);
        dash_sim_step(&sm, &ss, 50u);
        expect(ss.ch.session_ms == 123456u, "an overridden SESSION must survive a sim step");
    }

    /* ---- the taint of the lap that just ENDED must survive the crossing ----
     * sim->lap_tainted is cleared at the lap boundary, right after the best_ms
     * commit decision reads it, so by the time anything downstream renders a
     * frame the flag is already false. The lap-crossing delta flash needs to
     * know whether the lap it is about to compare was purely model-driven, so
     * the crossing stamps a sticky copy that survives until the NEXT crossing. */
    {
        DashState ts;
        DashSimState tm;
        dash_state_init(&ts);
        dash_sim_init(&tm);

        expect(!tm.last_lap_tainted, "no lap has ended yet, so the sticky taint must start clear");

        while (tm.lap_count < 2u) { dash_sim_step(&tm, &ts, 50u); }
        expect(!tm.last_lap_tainted,
               "laps the model drove itself must leave the sticky taint clear");

        /* force SPEED: this lap's distance is the operator's number, not the
         * model's, so its time is not evidence about the car (U6) */
        dash_ch_set(&ts, DASH_CH_SPEED, 200.0f);
        ts.overridden |= DASH_CH_BIT(DASH_CH_SPEED);
        uint32_t lc = tm.lap_count;
        while (tm.lap_count == lc) { dash_sim_step(&tm, &ts, 50u); }
        expect(tm.last_lap_tainted,
               "a lap driven at a forced speed must stamp the sticky taint at its crossing");
        expect(!tm.lap_tainted,
               "the in-progress flag must still clear at the crossing, as it always did");

        /* release the override: the NEXT lap is clean again, and the sticky
         * flag must follow it rather than latching for the rest of the run */
        ts.overridden = 0u;
        lc = tm.lap_count;
        while (tm.lap_count == lc) { dash_sim_step(&tm, &ts, 50u); }
        expect(!tm.last_lap_tainted,
               "the sticky taint must clear on the first clean lap after the override");

        /* a circuit switch taints the lap it starts, and that must be sticky too */
        dash_sim_set_circuit(&tm, SIM_CIRCUIT_HPR);
        lc = tm.lap_count;
        while (tm.lap_count == lc) { dash_sim_step(&tm, &ts, 50u); }
        expect(tm.last_lap_tainted,
               "a lap begun by a circuit switch must stamp the sticky taint too");
    }

    /* ---- a lap driven under a forced SPEED must not become BEST ----
     * U6 integrates lap distance from the PUBLISHED speed channel, so an
     * operator holding `set speed 200` drives a fabricated ~46 s lap. It is a
     * real thing they did, so LAST still shows it -- but adopting it as best_ms
     * AND as the U5 delta reference corrupts DELTA for the rest of the session
     * with no operator-reachable way to clear it (best_ms only ever decreases). */
    {
        DashState ts;
        DashSimState tm;
        dash_state_init(&ts);
        dash_sim_init(&tm);
        while (tm.lap_count < 3u) { dash_sim_step(&tm, &ts, 50u); }
        const uint32_t honest_best = tm.best_ms;
        expect(honest_best > 100000u, "three honest laps must have set a real best");

        /* the operator forces SPEED, exactly as `set speed 200` does */
        ts.overridden |= DASH_CH_BIT(DASH_CH_SPEED);
        dash_ch_set(&ts, DASH_CH_SPEED, 200.0f);
        const uint32_t before = tm.lap_count;
        while (tm.lap_count < before + 1u) { dash_sim_step(&tm, &ts, 50u); }

        expect(tm.last_ms < honest_best,
               "the forced-speed lap must genuinely be quicker (else this proves nothing)");
        expect(tm.best_ms == honest_best,
               "a lap driven under a forced SPEED must NOT be adopted as BEST");

        /* release the override; the next honest lap must read a sane DELTA
         * against the honest reference rather than tens of seconds behind */
        ts.overridden = (uint32_t) (ts.overridden & ~DASH_CH_BIT(DASH_CH_SPEED));
        const uint32_t after = tm.lap_count;
        while (tm.lap_count < after + 1u) { dash_sim_step(&tm, &ts, 50u); }
        for (int i = 0; i < 200; i++) { dash_sim_step(&tm, &ts, 50u); }
        expect(dash_ch_valid(&ts, DASH_CH_DELTA), "DELTA must still be published");
        expect(fabsf(dash_ch_get(&ts, DASH_CH_DELTA)) < 10.0f,
               "DELTA must stay sane -- the forced lap must not have become the reference");
        expect(tm.best_ms <= honest_best,
               "best must remain an honestly driven lap");
    }

    /* ---- a circuit switch must not hand a fabricated lap to BEST ----
     * dash_sim_set_circuit zeroes lap position and the lap clock but leaves
     * speed_mph, so a lap STARTED by `circuit hpr` opens at whatever the sweep
     * ramp (or the abandoned lap) left behind -- up to 200 mph in 6th. The
     * physics of carrying that speed is fine; committing the resulting lap time
     * is not. */
    {
        DashState cs;
        DashSimState cm;
        dash_state_init(&cs);
        dash_sim_init(&cm);
        while (cm.lap_count < 3u) { dash_sim_step(&cm, &cs, 50u); }
        const uint32_t honest_best = cm.best_ms;

        /* up the sweep ramp to the top of the dial, then back to the lap */
        dash_sim_set_circuit(&cm, SIM_CIRCUIT_SWEEP);
        while (cm.speed_mph < SIM_SWEEP_MAX_MPH - 1.0f) { dash_sim_step(&cm, &cs, 50u); }
        expect(cm.speed_mph > 190.0f, "the sweep must have reached the top of the dial");
        dash_sim_set_circuit(&cm, SIM_CIRCUIT_HPR);

        const uint32_t before = cm.lap_count;
        while (cm.lap_count < before + 1u) { dash_sim_step(&cm, &cs, 50u); }
        expect(cm.best_ms == honest_best,
               "a lap started by a circuit switch must NOT be adopted as BEST");

        /* the lap AFTER it is driven normally and is eligible again */
        const uint32_t after = cm.lap_count;
        while (cm.lap_count < after + 1u) { dash_sim_step(&cm, &cs, 50u); }
        expect(cm.best_ms > 100000u && cm.best_ms <= honest_best,
               "the next clean lap must be eligible for BEST again");
    }

    /* ---- FUEL must cycle on every driving path, not just TRACK ----
     * U9's refill sits at a lap boundary, which STREET and SWEEP never reach.
     * Its stated purpose -- that the gauge never sits dead at zero -- was
     * therefore unmet on two of the three paths, and U11's button puts STREET
     * one press away. */
    {
        DashState fs;
        DashSimState fm;
        dash_state_init(&fs);
        dash_sim_init(&fm);
        fs.mode = DASH_MODE_STREET;
        for (int i = 0; i < 396000; i++) { dash_sim_step(&fm, &fs, 50u); } /* 5.5 sim-hours */
        expect(fm.fuel_gal > 0.0f,
               "STREET must refill an empty tank -- the gauge must not strand at zero");
        expect(fm.fuel_gal <= SIM_FUEL_START_GAL,
               "a STREET refill must not overfill the tank");

        DashState ws;
        DashSimState wm;
        dash_state_init(&ws);
        dash_sim_init(&wm);
        dash_sim_set_circuit(&wm, SIM_CIRCUIT_SWEEP);
        for (int i = 0; i < 396000; i++) { dash_sim_step(&wm, &ws, 50u); }
        expect(wm.fuel_gal > 0.0f,
               "SWEEP must refill an empty tank too -- it has no lap boundary either");
    }

    /* ---- dash_sim_set_skill divides BY its argument ----
     * A zero (or NaN) skill fills seg_jitter_mph with Inf/NaN, which propagates
     * through dash_sim_seg_limit_mph into the sqrtf lookahead cap and the
     * grip-circle divide and corrupts speed and rpm for the rest of the run,
     * with no recovery. The public entry point must not be able to do that. */
    {
        const float poison[] = { 0.0f, -1.0f, -0.0f };
        for (unsigned p = 0; p < sizeof(poison) / sizeof(poison[0]); p++)
        {
            DashState ks;
            DashSimState km;
            dash_state_init(&ks);
            dash_sim_init(&km);
            dash_sim_set_skill(&km, poison[p]);
            expect(km.driver_skill > 0.0f,
                   "a non-positive driver skill must never be stored");
            for (uint8_t i = 0u; i < SIM_SEG_COUNT; i++)
            {
                expect(isfinite(km.seg_jitter_mph[i]),
                       "corner jitter must stay finite after a poison skill");
            }
            for (int i = 0; i < 4000; i++) { dash_sim_step(&km, &ks, 50u); }
            expect(isfinite(km.speed_mph) && isfinite(km.rpm),
                   "speed and rpm must survive a poison skill");
            expect(km.speed_mph >= 0.0f && km.speed_mph < SIM_SWEEP_MAX_MPH,
                   "speed must stay in range after a poison skill");
        }
    }

    if (failures == 0)
    {
        printf("OK: dash sim honors bounds, ownership, laps, freeze, and determinism\n");
        return 0;
    }
    return 1;
}
