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
#define SIM_TIRE_DIA_IN     25.44f /* 315/30R18 on all four corners (plan R5) */
#define SIM_FINAL_DRIVE     3.73f
#define SIM_MPH_CONST       336.0f
static const float SIM_GEAR_RATIOS[6] = { 2.66f, 1.78f, 1.30f, 1.00f, 0.80f, 0.63f }; /* T56 */

/* ---- chassis / acceleration model (plan U2, R4/R5) ----
 * Acceleration is not authored, it is computed: traction-limited off the
 * corners, power-limited up top, opposed by drag --
 *   a = min(traction_g * g, whp * 550 / (mass * v)) - 0.5 * rho * CdA * v^2 / mass
 * All of these are tuning inputs rather than measurements. SIM_POWER_WHP is
 * the dyno figure measured in Denver, so no altitude derate is applied on
 * top of it (plan KTD5) -- the ~15% NA loss is already in the number. */
#define SIM_MASS_SLUG     90.1f    /* 2900 lb with driver / 32.174 */
#define SIM_POWER_WHP     511.0f   /* dyno-measured in Denver (KTD5) */
#define SIM_TRACTION_G    1.05f    /* rear-driven on 315 R-comps */
#define SIM_BRAKE_G       1.25f    /* square 315s, four-wheel braking */
#define SIM_LATERAL_G     1.30f    /* holds the car through a corner's arc (U10) */
#define SIM_CDA           9.5f     /* ft^2: Cd ~0.45 x ~21 ft^2; a 1965 body is not slippery */
#define SIM_AIR_RHO       0.00174f /* slug/ft^3 at ~5000 ft density altitude */
#define SIM_G_FPS2        32.174f
#define SIM_FPS_PER_MPH   1.46667f
#define SIM_HP_FT_LB_S    550.0f   /* 1 hp = 550 ft-lb/s */
#define SIM_V_FLOOR_FPS   1.0f     /* divide guard for the power term at rest */

/* ---- powertrain / driving-cycle tuning ----
 * TRACK drives a real lap of High Plains Raceway: lap POSITION is integrated
 * road speed (not a clock), the position selects a segment of the circuit
 * table below, and the car accelerates freely until lookahead braking for the
 * next corner's entry limit engages. The gearbox follows wheel speed with
 * hysteresis, and RPM is always derived from speed THROUGH the current gear,
 * so the same road speed appears at very different rpm depending on where on
 * the lap (and in which gear) the car is. Gear selection is emergent (R6):
 * nothing asks for a particular gear, and this circuit happens to want 5th on
 * the front straight. */
#define SIM_RPM_START         950.0f  /* rolling out of the pits */
#define SIM_RPM_IDLE          850.0f  /* rpm floor */
#define SIM_RPM_MAX           7600.0f /* brushes the 7600 shift point, under the 8000 redline */
#define SIM_UPSHIFT_RPM       7400.0f /* pull the next gear above this */
#define SIM_DOWNSHIFT_RPM     3300.0f /* consider a lower gear below this... */
#define SIM_DOWNSHIFT_MAX_RPM 6900.0f /* ...if it lands the revs below this */
/* Per-lap corner-limit jitter: an odd count, centred so the draw spans
 * +/-(N-1)/2 mph. Narrowed from 9 (+/-4) to 3 (+/-1) in U4, and this is a
 * consequence of the arc change rather than a free knob. Pinning a corner's
 * arc to track geometry instead of to the driver's speed made a corner a fixed
 * length of tarmac, so a given mph of jitter now buys several times more lap
 * TIME than it did when the arc shrank along with the speed. Measured over 35
 * flying laps at the default skill: +/-4 mph spreads laps by 5.7 s, +/-2 by
 * 3.1 s, +/-1 by 1.55 s. Only the last fits S2's 0.3-1.5 s band. */
#define SIM_SEG_JITTER_MPH    3u
#define SIM_SEG_JITTER_MID    ((float) ((SIM_SEG_JITTER_MPH - 1u) / 2u))
#define SIM_SEG_COUNT         16u

/* ---- driver skill (plan U4, R7) ----
 * ONE constant standing in for "moderate, not expert". It does three things:
 * scales the corner limits the driver is willing to carry, backs the brakes
 * off slightly from the tires' maximum, and widens the per-lap jitter
 * INVERSELY -- a less consistent driver is a less repeatable one.
 *
 * It applies ONLY to segments flagged is_corner_limit. Scaling segment 9 would
 * turn T12's flat-out kink into an enforced ceiling and lookahead braking would
 * then invent a braking event on the one section that must stay pinned --
 * corrupting the rpm sawtooth, invisibly, because a wrong sawtooth still looks
 * like a sawtooth.
 *
 * Held to 0.82..0.95 on purpose. At least four independent error sources feed
 * lap time -- apportioned segment lengths (KTD3), authored corner speeds, the
 * estimated CdA, and the power figure -- so a value outside that band is
 * evidence that one of THOSE is wrong, not licence to keep turning this one.
 * It is also no longer the dominant lever it was drafted as: per-corner turn
 * angle (below) moves lap time considerably harder. */
#define SIM_DRIVER_SKILL      0.86f

/* Brakes are derated only half as far as cornering: a moderate driver gets
 * closer to the tires' limit in a straight line -- one axis of grip and no
 * balance to manage -- than at an apex. */
static inline float dash_sim_brake_g(float driver_skill)
{
    return SIM_BRAKE_G * (0.5f * (1.0f + driver_skill));
}

/* ---- circuit selection (plan U7, R12, KTD7) ----
 * TRACK normally drives the HPR lap below. SWEEP is a bench FIXTURE, not a
 * second circuit: it exists because a real lap cannot reach the top of a
 * 200 mph speedo or ask for 6th gear, so replacing the old fictional cycle
 * outright would have silently deleted that coverage.
 *
 * It deliberately does NOT reuse the corner table or lookahead braking. A
 * designed ramp is a better fixture than an emergent one -- an invented second
 * circuit would only cover wherever that circuit happened to go, whereas a ramp
 * across the whole speed range visits every boundary by construction. What it
 * does share is the gearbox: rpm comes from road speed through the same
 * hysteretic box the lap uses, so the sweep exercises the real drivetrain math
 * rather than a parallel one.
 *
 * SWEEP is TRACK-side only. STREET's cruise is its own model and is unchanged
 * (R11); selecting SWEEP while in STREET does nothing until the mode returns to
 * TRACK. Lap timing is meaningless here, so LAP/LAST/BEST/DELTA/PRED (and the
 * rest of the lap-owned channels) are dead-fronted throughout. */
typedef enum {
    SIM_CIRCUIT_HPR = 0,   /* the default: a real lap of High Plains Raceway */
    SIM_CIRCUIT_SWEEP = 1, /* opt-in range fixture, `circuit sweep` */
} SimCircuit;

/* SIM_-local copy of dash_math.h's DASH_SPEED_MAX -- dash_sim.h does not
 * include dash_math.h. tests/test_dash_sim.c pins the two together, because a
 * sweep that stops short of the dial's top would quietly stop being a sweep. */
#define SIM_SWEEP_MAX_MPH   200.0f
/* Ramp rate, a bench-ergonomics number rather than a physical one: the whole
 * point is that a human can WATCH each range boundary go by. 4 mph/s puts a
 * full 0 -> 200 -> 0 cycle at 100 s, which is the binding constraint at the top
 * of the dial -- 6th gear only spans ~188-200 mph, so it is on screen for ~3 s
 * each way. Faster and top gear is a flicker; much slower and the 15 s spent
 * crawling through 1st tries the operator's patience. */
#define SIM_SWEEP_RATE_MPHPS 4.0f
#define SIM_SWEEP_PERIOD_S  (2.0f * SIM_SWEEP_MAX_MPH / SIM_SWEEP_RATE_MPHPS)

/* ---- lap geometry: High Plains Raceway, 2.55 mi (plan R1) ---- */
#define SIM_TRACK_LAP_FT    13464.0f

/* The circuit (plan U3). Distance-keyed, clockwise, start/finish at Turn 3's
 * exit so the lap opens with the full front straight -- which is why the
 * SEGMENT index is offset from the TURN number: segment 1 is T4 and T1 does
 * not appear until segment 13. Read the Segment column top to bottom and the
 * driving order is the PCA/Speed Secrets course guide's lap exactly.
 *
 * len_ft is apportioned, not surveyed (plan KTD3): only the lap total and the
 * 2838 ft front straight are published figures, so segment 0 must not flex --
 * the 2 ft reconciliation remainder lives in segment 15 (T3, 424 -> 426).
 * limit_mph is hand-authored from each corner's documented character (KTD2).
 *
 * A limit binds at its segment's entry boundary AND across the corner's ARC
 * beyond it (U10) -- the arc is derived from the limit and SIM_LATERAL_G, not
 * authored here. Past the arc the car accelerates freely until lookahead
 * braking for the NEXT entry limit engages. Both extremes are wrong: holding
 * the limit for all 700 ft of T4 has the car crawling, and releasing it at the
 * boundary gives the corner no duration at all (which ran the lap 34 s fast).
 *
 * is_corner_limit separates real constraints from annotations. Segment 0 (the
 * straight) and segment 9 (T12 "Ladder to Heaven", a flat-out uphill kink)
 * carry a speed for reference only: they are never braked for, and U4's
 * driver-skill scaling must skip them.
 *
 * turn_rad is how far the car's heading swings through the corner, and it is
 * the same category of authored track geometry as len_ft and limit_mph -- the
 * PCA/Speed Secrets character notes describe every corner, and T1 is published
 * at 160 deg, which anchors the rest. It matters far more than it looks: arc
 * length is radius x angle, so the angle column is the single strongest lever
 * on lap time in this model. A circuit-wide nominal angle claimed all fifteen
 * turns were the same corner, which T1 (a 160 deg hairpin) and T12 (a kink)
 * plainly are not. */
typedef struct {
    float len_ft;         /* segment length */
    float end_ft;         /* cumulative distance at this segment's end */
    float limit_mph;      /* speed at this segment's ENTRY boundary */
    float turn_rad;       /* heading change through the corner; 0 for annotations */
    bool is_corner_limit; /* false = annotation only, never enforced */
} SimSeg;

#define SIM_DEG(d) ((d) * 0.0174532925f)

static const SimSeg SIM_SEGS[SIM_SEG_COUNT] = {
    /* len_ft     end_ft   limit  turn angle          is_corner_limit */
    { 2838.0f,  2838.0f,   0.0f, SIM_DEG(  0.0f), false }, /* front straight T3->T4: longest straight, official figure */
    {  700.0f,  3538.0f,  65.0f, SIM_DEG( 90.0f), true  }, /* T4  Biker's Berm -- primary braking zone, a square right off the straight */
    {  600.0f,  4138.0f,  75.0f, SIM_DEG( 80.0f), true  }, /* T5  Niagara -- blind left, apex past middle: an open bend */
    {  700.0f,  4838.0f,  55.0f, SIM_DEG(120.0f), true  }, /* T6  Danny's Lesson -- decreasing radius + very late apex: tightens well past 90 */
    { 1100.0f,  5938.0f, 105.0f, SIM_DEG( 75.0f), true  }, /* T7  High Plains Drifter -- fast sweeper: long by radius, not by angle */
    {  800.0f,  6738.0f,  70.0f, SIM_DEG( 90.0f), true  }, /* T8  -- uphill braking zone, a conventional right */
    { 1000.0f,  7738.0f,  85.0f, SIM_DEG(120.0f), true  }, /* T9A/9B To Hell on a Bobsled -- LINKED pair, so the angles sum */
    {  700.0f,  8438.0f,  80.0f, SIM_DEG( 60.0f), true  }, /* T10 -- a compression more than a corner: shallow direction change */
    {  800.0f,  9238.0f,  70.0f, SIM_DEG(110.0f), true  }, /* T11 -- "extremely late apex" means holding lock well past 90 */
    {  900.0f, 10138.0f, 125.0f, SIM_DEG(  0.0f), false }, /* T12 Ladder to Heaven -- flat-out uphill kink, NOT a limit and NOT an arc */
    {  600.0f, 10738.0f,  60.0f, SIM_DEG(100.0f), true  }, /* T13 Prairie Corkscrew entry -- hard direction change off the brakes */
    {  500.0f, 11238.0f,  65.0f, SIM_DEG( 90.0f), true  }, /* T14 -- right, 3/4 through the curb */
    {  500.0f, 11738.0f,  70.0f, SIM_DEG( 90.0f), true  }, /* T15 -- left, tracks out far right */
    {  800.0f, 12538.0f,  40.0f, SIM_DEG(160.0f), true  }, /* T1  -- 80 ft radius, 160 deg PUBLISHED, off-camber: the one sourced angle */
    {  500.0f, 13038.0f,  50.0f, SIM_DEG(100.0f), true  }, /* T2  -- very late apex, past 90 like T11 */
    {  426.0f, 13464.0f,  75.0f, SIM_DEG(120.0f), true  }, /* T3  -- DOUBLE apex: one long corner turning through two (+2 ft remainder) */
};

/* ---- lap delta trace (plan U5, R8) ----
 * DELTA is measured against the best lap the car actually drove, not authored.
 * Each lap is sampled into a distance-bucketed table of elapsed times, so the
 * reference can be read back at the car's exact position on track.
 *
 * TWO tables, not one, and this is the whole design: the current lap is being
 * written at the same moment the reference must stay readable, so a single
 * table would have the car compared against a half-overwritten mixture of two
 * laps. The current lap is committed over the reference only at a lap boundary,
 * and only when that lap is a new best.
 *
 * 128 buckets over 13464 ft is ~105 ft of track each, and the delta
 * INTERPOLATES within a bucket rather than stepping at its edge -- otherwise
 * the bar would jerk once per bucket, worst at corner speeds where a bucket
 * lasts nearly two seconds. The +1 slot is the lap's finish time, which is what
 * the last bucket interpolates toward.
 *
 * uint16_t centiseconds caps a stored lap at 655.35 s against a ~122 s driven
 * lap. Only a stalled `set speed 0` lap can reach the cap, and it saturates
 * rather than wrapping. Total cost 516 bytes.
 *
 * DELTA is deliberately NOT step-size invariant. A bucket's time is stamped on
 * the first step at or past its boundary, so 10 ms and 50 ms runs stamp times
 * up to one step apart -- and the underlying lap times already differ by up to
 * the 2% the suite allows. The step-size contract covers the integrators
 * (distance, lap time); DELTA is a display value derived from them, and no test
 * asserts it across step sizes. */
#define SIM_TRACE_BUCKETS   128u
#define SIM_TRACE_SLOTS     (SIM_TRACE_BUCKETS + 1u) /* +1: the finish stamp */
#define SIM_TRACE_BUCKET_FT (SIM_TRACK_LAP_FT / (float) SIM_TRACE_BUCKETS)
#define SIM_TRACE_CS_MAX    65535u

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
    float lap_dist_ft;  /* lap position: integrated road speed, not a clock (U1) */
    float driver_skill; /* SIM_DRIVER_SKILL, held in state so it is retunable (U4) */
    SimCircuit circuit; /* HPR lap or the SWEEP range fixture (U7) */
    float sweep_t_s;    /* SWEEP ramp phase; accrues only while sweeping */
    uint8_t seg_idx;    /* current circuit segment */
    float seg_jitter_mph[SIM_SEG_COUNT]; /* per-lap corner-limit offsets */
    float ect_f;
    float oilt_f;
    float fuel_gal;
    uint32_t last_ms;
    uint32_t best_ms;
    /* U5 lap delta: the lap being driven, and the best lap to measure it
     * against. Elapsed centiseconds at each distance bucket; slot
     * SIM_TRACE_BUCKETS is the finish stamp. */
    uint16_t lap_trace_cs[SIM_TRACE_SLOTS];
    uint16_t ref_trace_cs[SIM_TRACE_SLOTS];
    uint16_t trace_next;  /* next unstamped bucket, 0..SIM_TRACE_SLOTS */
    bool ref_valid;       /* a reference lap exists; DELTA is dead-front until it does */
} DashSimState;

/* Which distance bucket a lap position falls in. Clamped at BOTH ends: the
 * rollover invariant keeps lap_dist_ft in [0, SIM_TRACK_LAP_FT), but the
 * stamp below is called before the rollover subtraction, so a distance at or
 * past the lap length must land on the last bucket rather than off the end of
 * the array. The !(> 0) form also catches NaN. */
static inline uint8_t dash_sim_trace_bucket(float dist_ft)
{
    if (!(dist_ft > 0.0f)) { return 0u; }
    const float b = dist_ft / SIM_TRACE_BUCKET_FT;
    if (b >= (float) (SIM_TRACE_BUCKETS - 1u))
    {
        return (uint8_t) (SIM_TRACE_BUCKETS - 1u);
    }
    return (uint8_t) b;
}

/* Stamp the current lap clock into every bucket up to `upto` that has not been
 * stamped yet. Buckets are filled forward-only, so a bucket records the time at
 * which the car FIRST reached it -- and a step long enough to skip buckets
 * fills the skipped ones rather than leaving holes the delta would read as
 * zero. Saturates rather than wrapping at the uint16 cap. */
static inline void dash_sim_trace_stamp(DashSimState *sim, uint16_t upto)
{
    uint32_t cs = sim->lap_ms / 10u;
    if (cs > SIM_TRACE_CS_MAX) { cs = SIM_TRACE_CS_MAX; }
    while ((sim->trace_next <= upto) && (sim->trace_next < SIM_TRACE_SLOTS))
    {
        sim->lap_trace_cs[sim->trace_next] = (uint16_t) cs;
        sim->trace_next++;
    }
}

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

/* Hysteretic gearbox: upshift above SIM_UPSHIFT_RPM; downshift when revs sag
 * AND the lower gear lands safely under the shift point. RPM always derives
 * from wheel speed through the selected gear, so the same road speed reads very
 * different rpm depending on which gear the box happens to be holding.
 *
 * Factored out for U7: the sweep fixture drives its own speed but runs it
 * through THIS box rather than a parallel path, which is what makes the fixture
 * evidence about the real drivetrain math instead of about itself. */
static inline void dash_sim_gearbox(DashSimState *sim)
{
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
}

/* Every channel the lap simulation owns, put back to invalid.
 *
 * dash_ch_set only ever RAISES the valid bit, so a channel published in TRACK
 * stays valid for as long as the dash runs unless something takes it back --
 * and nothing re-publishes LAP/LAST/BEST/DELTA off the lap path, so they simply
 * freeze at their last TRACK values. On the STREET screen that reads as a live
 * best-lap time from a session that ended minutes ago. It is reachable with one
 * press of the USER button (U11), so it is dead-fronted on EVERY non-lap path:
 * STREET, and the SWEEP fixture where lap timing has no meaning at all.
 *
 * dash_ch_invalidate carries the ownership guard, so a `set best ...` the
 * operator forced is left exactly where they put it. */
static inline void dash_sim_dead_front_lap(DashState *s)
{
    dash_ch_invalidate(s, DASH_CH_LAP);
    dash_ch_invalidate(s, DASH_CH_LAST);
    dash_ch_invalidate(s, DASH_CH_BEST);
    dash_ch_invalidate(s, DASH_CH_DELTA);
    dash_ch_invalidate(s, DASH_CH_PRED);
    dash_ch_invalidate(s, DASH_CH_LAPN);
    dash_ch_invalidate(s, DASH_CH_POS);
    dash_ch_invalidate(s, DASH_CH_THROTTLE);
    dash_ch_invalidate(s, DASH_CH_BRAKE);
}

/* This corner's limit: the authored speed pulled down to what the driver will
 * actually carry, plus the current lap's jitter. Annotations (segments 0 and
 * 9) are returned untouched -- they are never enforced, and scaling them would
 * manufacture a braking zone on a flat-out section (U4). */
static inline float dash_sim_seg_limit_mph(const DashSimState *sim, uint8_t seg)
{
    if (!SIM_SEGS[seg].is_corner_limit)
    {
        return SIM_SEGS[seg].limit_mph;
    }
    return SIM_SEGS[seg].limit_mph * sim->driver_skill + sim->seg_jitter_mph[seg];
}

/* U10: how far a corner LASTS. A corner is not a point on the track -- the
 * car is pinned by lateral grip for an arc past the entry boundary, and that
 * arc is what actually costs the lap time. It is derived, not authored:
 *
 *     r   = v_authored^2 / (SIM_LATERAL_G * g)   radius implied by the corner
 *     arc = r * turn_rad                         geometry of the corner
 *
 * so a slow corner is geometrically tight and short, a fast sweeper is long.
 * Sanity check on the one corner HPR publishes: T1's 40 mph limit implies an
 * 82 ft radius against a documented 80 ft, and its 160 deg is published too.
 *
 * BOTH inputs are track geometry, so the arc is DRIVER-INDEPENDENT -- radius
 * comes from the authored limit, not the skill-scaled or jittered one. That is
 * deliberate and it is what gives SIM_DRIVER_SKILL any leverage: a corner is a
 * fixed length of tarmac, so a slower driver spends LONGER in it. Deriving the
 * radius from the driver's speed instead made the arc shrink exactly as fast
 * as the car slowed (arc ~ v^2, so arc time ~ v), which is why skill was worth
 * barely 6 s across its whole range before this unit.
 *
 * Clamped to the segment: a short segment cannot contain more arc than it has
 * (T3's double apex wants 605 ft of its 426 ft, so it corners throughout). */
static inline float dash_sim_corner_arc_ft(uint8_t seg)
{
    if (!SIM_SEGS[seg].is_corner_limit)
    {
        return 0.0f; /* annotations (segment 0, segment 9) turn nothing */
    }
    const float v = SIM_SEGS[seg].limit_mph * SIM_FPS_PER_MPH;
    const float arc = (v * v / (SIM_LATERAL_G * SIM_G_FPS2)) * SIM_SEGS[seg].turn_rad;
    return (arc > SIM_SEGS[seg].len_ft) ? SIM_SEGS[seg].len_ft : arc;
}

/* Redraw every corner's limit jitter. Once per lap rather than per visit, so
 * lookahead braking sees one stable target all the way down to the boundary
 * (a target that moved under the braking curve would chatter the throttle).
 * Non-corner segments keep 0 -- their speeds are annotations, not limits.
 *
 * Magnitude scales INVERSELY with driver skill (U4): the worse the driver, the
 * less repeatable, so the wider the lap-to-lap spread. The LCG is drawn the
 * same number of times whatever the skill, so determinism is unaffected. */
static inline void dash_sim_reseed_jitter(DashSimState *sim)
{
    for (uint8_t i = 0u; i < SIM_SEG_COUNT; i++)
    {
        sim->seg_jitter_mph[i] =
            SIM_SEGS[i].is_corner_limit
            ? (((float) dash_sim_jitter(sim, SIM_SEG_JITTER_MPH) - SIM_SEG_JITTER_MID)
               / sim->driver_skill)
            : 0.0f;
    }
}

/* Retune the driver without disturbing the LCG. The current lap's jitter is
 * rescaled in place rather than redrawn, so two sims run at different skills
 * walk the same random stream and differ ONLY by the driver -- which is what
 * makes "a better driver is faster" a testable claim rather than a coin flip
 * against +/-4 mph of corner-limit noise. */
static inline void dash_sim_set_skill(DashSimState *sim, float skill)
{
    const float rescale = sim->driver_skill / skill;
    for (uint8_t i = 0u; i < SIM_SEG_COUNT; i++)
    {
        sim->seg_jitter_mph[i] *= rescale;
    }
    sim->driver_skill = skill;
}

/* Select the circuit (U7). Always restarts it, even when re-selecting the one
 * already running -- `circuit sweep` twice is a bench operator asking to see
 * the sweep from the bottom of the dial again, which is the only thing it could
 * reasonably mean.
 *
 * The lap in progress is abandoned rather than resumed: the car leaves the lap
 * simulation at some arbitrary point on track and comes back at some arbitrary
 * speed, so continuing to time that lap would commit a fabricated lap time to
 * LAST -- and possibly to BEST, where it would poison DELTA's reference for the
 * rest of the session. Completed laps (lap_count, last_ms, best_ms, the
 * reference trace) are history and are deliberately kept.
 *
 * The LCG is NOT touched, so a sweep detour does not rewind the jitter stream
 * into a bit-identical replay of the laps already driven. */
static inline void dash_sim_set_circuit(DashSimState *sim, SimCircuit circuit)
{
    sim->circuit = circuit;
    sim->sweep_t_s = 0.0f;   /* the sweep always starts at the bottom of the dial */
    sim->lap_dist_ft = 0.0f; /* back to start/finish: no half-driven lap to resume */
    sim->lap_ms = 0u;
    sim->trace_next = 0u;
    dash_sim_trace_stamp(sim, 0u); /* the new lap starts at t=0 by definition */
}

/* Gear 1, warm-start temps at ambient-ish cold values, full-ish tank.
 * The ~7 mph rollout is deliberate: it makes lap 1 the session out-lap, which
 * is why lap 1 is excluded from best_ms below (U1). */
static inline void dash_sim_init(DashSimState *sim)
{
    sim->gear = 1;
    sim->t_s = 0.0f;
    sim->lcg = SIM_LCG_SEED;
    sim->lap_ms = 0u;
    sim->lap_count = 0u;
    sim->rpm = SIM_RPM_START;
    sim->speed_mph = dash_sim_speed_mph(SIM_RPM_START, 1); /* ~7 mph, rolling out */
    sim->lap_dist_ft = 0.0f;
    sim->driver_skill = SIM_DRIVER_SKILL; /* must precede the first jitter draw */
    sim->circuit = SIM_CIRCUIT_HPR; /* HPR is the default; SWEEP is opt-in (R12) */
    sim->sweep_t_s = 0.0f;
    sim->seg_idx = 0u;
    dash_sim_reseed_jitter(sim);
    sim->ect_f = SIM_COLD_START_F;
    sim->oilt_f = SIM_COLD_START_F;
    sim->fuel_gal = SIM_FUEL_START_GAL;
    sim->last_ms = 0u;
    sim->best_ms = 0u;
    for (uint16_t b = 0u; b < SIM_TRACE_SLOTS; b++)
    {
        sim->lap_trace_cs[b] = 0u;
        sim->ref_trace_cs[b] = 0u;
    }
    sim->trace_next = 0u;
    sim->ref_valid = false; /* no lap driven yet: DELTA stays dead-fronted */
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
    bool delta_valid = false; /* U5: no reference lap yet -> DELTA dead-fronts */
    bool track = (s->mode == DASH_MODE_TRACK);
    /* Three driving paths, not two (U7): the HPR lap, the SWEEP range fixture,
     * and STREET's cruise. Only the first has laps to time. */
    bool lap_active = track && (sim->circuit == SIM_CIRCUIT_HPR);

    float track_throttle_pct = 0.0f;
    float track_brake_pct = 0.0f;

    if (lap_active)
    {
        /* lap TIMER (no longer the lap position -- see below) */
        sim->lap_ms += dt_ms;

        /* U1: lap position is integrated road speed. A lap closes when the
         * car crosses SIM_TRACK_LAP_FT, so lap time is an OUTPUT. Nothing but
         * lap_dist_ft and lap_ms reset, so laps 2+ cross start/finish carrying
         * their speed -- and with S/F at T3's exit that is a corner-exit speed
         * feeding the front straight, i.e. flying laps by construction. */
        /* U6/R9: integrate the PUBLISHED speed, not the sim's private one. In
         * normal operation these are the same float -- the channel holds what
         * the sim wrote at the end of the previous step -- so nothing changes.
         * When an operator forces SPEED, the car genuinely travels at the
         * commanded speed, and the speedo and the lap agree instead of the
         * dash showing 45 mph while the sim laps at 150. A cleared SPEED is
         * invalid and there is no number to integrate, so fall back to the
         * sim's own rather than freezing the lap forever. */
        float dist_mph = dash_ch_valid(s, DASH_CH_SPEED)
                         ? dash_ch_get(s, DASH_CH_SPEED)
                         : sim->speed_mph;
        if (dist_mph < 0.0f) { dist_mph = 0.0f; } /* a forced negative must not reverse the lap */
        sim->lap_dist_ft += dist_mph * SIM_FPS_PER_MPH * dt_s;

        /* U5: record when this lap reached where it is. Stamped before the
         * rollover subtraction, so the bucket clamp is what keeps the final
         * partial bucket in range. */
        dash_sim_trace_stamp(sim, dash_sim_trace_bucket(sim->lap_dist_ft));

        if (sim->lap_dist_ft >= SIM_TRACK_LAP_FT)
        {
            sim->lap_dist_ft -= SIM_TRACK_LAP_FT;
            sim->last_ms = sim->lap_ms;
            dash_sim_trace_stamp(sim, (uint16_t) SIM_TRACE_BUCKETS); /* finish stamp */
            /* lap 1 is the out-lap: it rolls out of the pits at ~7 mph, so it
             * is several seconds slow and must never set the benchmark. It
             * still populates LAST and still displays -- it is a real lap, just
             * not a representative one. lap_count here is laps completed
             * BEFORE this one, so 1 is the first flying lap to finish. */
            if ((sim->lap_count == 1u)
                || ((sim->lap_count > 1u) && (sim->last_ms < sim->best_ms)))
            {
                sim->best_ms = sim->last_ms;
                /* U5: the best lap is also the delta REFERENCE, so it commits
                 * on exactly the same condition -- which is what keeps the
                 * out-lap out of it. Referencing a ~7 mph rollout would read
                 * every flying lap as tens of seconds ahead. */
                for (uint16_t b = 0u; b < SIM_TRACE_SLOTS; b++)
                {
                    sim->ref_trace_cs[b] = sim->lap_trace_cs[b];
                }
                sim->ref_valid = true;
            }
            sim->lap_ms = 0u;
            sim->trace_next = 0u;
            dash_sim_trace_stamp(sim, 0u); /* the new lap starts at t=0 by definition */
            sim->lap_count++;
            dash_sim_reseed_jitter(sim);
        }

        /* locate the current segment by distance (U3) */
        uint8_t si = 0u;
        while ((si < (SIM_SEG_COUNT - 1u)) && (sim->lap_dist_ft >= SIM_SEGS[si].end_ft))
        {
            si++;
        }
        sim->seg_idx = si;

        /* U3 lookahead braking (KTD4). Rather than braking AT a corner, walk
         * the rest of the lap and ask what speed each upcoming corner entry
         * still permits right now:
         *     v_allow = sqrt(v_limit^2 + 2 * a_brake * distance_to_it)
         * The lowest such figure is the cap. Riding the cap down IS the
         * braking zone, and it lands the car exactly on the limit at the
         * boundary -- which is what makes the braking points, and therefore
         * the tach sawtooth, come out in the right place. */
        const float a_brake = dash_sim_brake_g(sim->driver_skill) * SIM_G_FPS2;
        float v_fps = sim->speed_mph * SIM_FPS_PER_MPH;
        float cap_fps = 1.0e6f; /* no corner in range -> no cap */
        /* measured from where the car will be at the END of this step, not
         * where it is now: capping against the current position lets it
         * arrive a step's worth of braking (~2 mph) above the limit. This
         * converges to the same continuous solution at any dt. */
        float d_ahead = SIM_SEGS[si].end_ft - sim->lap_dist_ft - v_fps * dt_s;
        if (d_ahead < 0.0f) { d_ahead = 0.0f; }
        for (uint8_t k = 1u; k <= SIM_SEG_COUNT; k++)
        {
            const uint8_t j = (uint8_t) ((si + k) % SIM_SEG_COUNT);
            if (SIM_SEGS[j].is_corner_limit) /* annotations are never braked for */
            {
                const float vt = dash_sim_seg_limit_mph(sim, j) * SIM_FPS_PER_MPH;
                const float allow = sqrtf(vt * vt + 2.0f * a_brake * d_ahead);
                if (allow < cap_fps) { cap_fps = allow; }
            }
            d_ahead += SIM_SEGS[j].len_ft;
        }

        /* U10: corner DURATION. Inside a corner's arc the car is spending its
         * grip on turning, so two things change. The limit becomes a ceiling
         * for the whole arc rather than a single boundary instant, and the
         * longitudinal budget is what the grip circle leaves over:
         *
         *     used  = a_lat / a_lat_max = (v / v_limit)^2
         *     scale = sqrt(1 - used^2)
         *
         * At the limit nothing is left and the car holds speed; below it the
         * driver can already feed throttle in, which is what a real exit looks
         * like. Braking is deliberately NOT scaled -- the lookahead solution
         * above assumes full a_brake, and derating one without the other would
         * overshoot every corner entry. */
        float grip_scale = 1.0f;
        if (SIM_SEGS[si].is_corner_limit)
        {
            const float lim_mph = dash_sim_seg_limit_mph(sim, si);
            const float seg_start_ft = SIM_SEGS[si].end_ft - SIM_SEGS[si].len_ft;
            if ((sim->lap_dist_ft - seg_start_ft) < dash_sim_corner_arc_ft(si))
            {
                const float v_lim = lim_mph * SIM_FPS_PER_MPH;
                float used = (v_fps * v_fps) / (v_lim * v_lim);
                if (used > 1.0f) { used = 1.0f; }
                grip_scale = sqrtf(1.0f - used * used);
                if (cap_fps > v_lim) { cap_fps = v_lim; }
            }
        }

        bool braking = false;
        if (v_fps > cap_fps)
        {
            v_fps -= a_brake * dt_s;
            if (v_fps < cap_fps) { v_fps = cap_fps; }
            braking = true;
        }
        else
        {
            /* U2: traction-limited off the corners, power-limited up top,
             * both opposed by drag. v_guard keeps the power term finite at
             * the standing-start rollout (v_fps is never actually 0 given
             * dash_sim_init's seed, but the divide is guarded anyway). */
            const float v_guard = (v_fps < SIM_V_FLOOR_FPS) ? SIM_V_FLOOR_FPS : v_fps;
            const float a_traction = SIM_TRACTION_G * SIM_G_FPS2;
            const float a_power = (SIM_POWER_WHP * SIM_HP_FT_LB_S)
                                  / (SIM_MASS_SLUG * v_guard);
            const float a_drag = 0.5f * SIM_AIR_RHO * SIM_CDA * v_guard * v_guard
                                 / SIM_MASS_SLUG;
            const float a_prop = (a_power < a_traction) ? a_power : a_traction;
            /* drag is never scaled: it does not compete for tire grip */
            const float a_net = a_prop * grip_scale - a_drag;
            v_fps += a_net * dt_s;
            if (v_fps > cap_fps) { v_fps = cap_fps; braking = true; }
        }
        if (v_fps < 0.0f) { v_fps = 0.0f; }
        sim->speed_mph = v_fps / SIM_FPS_PER_MPH;

        /* the model has exactly two states: on the brakes, or on the throttle */
        track_throttle_pct = braking ? 0.0f : 100.0f;
        track_brake_pct = braking ? (82.0f + 8.0f * sinf(t * 3.0f)) : 0.0f;

        dash_sim_gearbox(sim);
        speed_mph = sim->speed_mph;

        /* U5/R8: lap delta against the best lap actually driven. The reference
         * table holds elapsed time at each bucket boundary, so interpolating
         * between the two that bracket the car gives the reference's time AT
         * this spot on the track; the delta is what the clock reads now minus
         * that. Positive = behind the reference. Before any reference exists
         * there is nothing to subtract, and DELTA is dead-fronted below rather
         * than publishing a fabricated zero. */
        if (sim->ref_valid)
        {
            const uint8_t b = dash_sim_trace_bucket(sim->lap_dist_ft);
            float frac = (sim->lap_dist_ft - (float) b * SIM_TRACE_BUCKET_FT)
                         / SIM_TRACE_BUCKET_FT;
            if (frac < 0.0f) { frac = 0.0f; }
            if (frac > 1.0f) { frac = 1.0f; } /* the clamped last bucket runs long */
            const float t0 = (float) sim->ref_trace_cs[b] * 0.01f;
            const float t1 = (float) sim->ref_trace_cs[b + 1u] * 0.01f;
            delta_s = ((float) sim->lap_ms * 0.001f) - (t0 + (t1 - t0) * frac);
            delta_valid = true;
        }
    }
    else if (track)
    {
        /* SWEEP (U7, R12/S4): a deterministic triangle across the whole
         * speedo, 0 -> SIM_SWEEP_MAX_MPH -> 0, with rpm falling out of the same
         * gearbox the lap uses. No corner table, no lookahead braking, no
         * physics -- the acceleration model is exactly what a coverage fixture
         * must NOT depend on, since what the car can do is the thing that
         * limits where a real lap goes.
         *
         * The phase is read BEFORE the accumulator advances, so the fixture's
         * first published step is a true 0 mph -- the bottom of the dial is a
         * range boundary too, and it should be swept rather than skipped. */
        const float phase = fmodf(sim->sweep_t_s, SIM_SWEEP_PERIOD_S);
        float v = phase * SIM_SWEEP_RATE_MPHPS;
        if (v > SIM_SWEEP_MAX_MPH) { v = 2.0f * SIM_SWEEP_MAX_MPH - v; } /* the way back down */
        if (v < 0.0f) { v = 0.0f; }
        sim->sweep_t_s += dt_s;
        sim->speed_mph = v;
        dash_sim_gearbox(sim);
        speed_mph = sim->speed_mph;
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

    if (lap_active)
    {
        if (delta_valid)
        {
            if (dash_ch_sim_owned(s, DASH_CH_DELTA)) { dash_ch_set(s, DASH_CH_DELTA, delta_s); }
        }
        else
        {
            dash_ch_invalidate(s, DASH_CH_DELTA); /* `--`, not a fabricated 0.00 */
        }
        if (dash_ch_sim_owned(s, DASH_CH_LAP))   { dash_ch_set(s, DASH_CH_LAP, (float) sim->lap_ms); }
        if (sim->lap_count > 0u) /* before the first lap, LAST/BEST stay --:--.--- */
        {
            if (dash_ch_sim_owned(s, DASH_CH_LAST)) { dash_ch_set(s, DASH_CH_LAST, (float) sim->last_ms); }
            /* BEST needs a flying lap: after the out-lap alone there is no
             * representative time, so it stays dead-fronted rather than
             * publishing the out-lap or a fabricated zero (U1). */
            if ((sim->lap_count > 1u) && dash_ch_sim_owned(s, DASH_CH_BEST))
            {
                dash_ch_set(s, DASH_CH_BEST, (float) sim->best_ms);
            }

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
    else
    {
        /* Off the lap path -- STREET, or the SWEEP fixture. Nothing here
         * re-publishes these, so they must be actively taken back to `--`
         * rather than left holding the last lap TRACK ever ran. */
        dash_sim_dead_front_lap(s);
    }
}

#endif /* DASH_SIM_H */
