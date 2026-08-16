/*
 * Invariant test: the Ford control-pack CAN dialect decoder
 * (dash_can_ford.h, plan 2026-08-15-001 U4) -- golden-frame fidelity (AE4),
 * sentinel dead-fronting, staleness in both bench and car semantics (AE2),
 * serial precedence (AE3), the can_owned ownership handshake with the
 * simulator, range edges, and the R11 sim round-trip: the real simulator's
 * output encoded to Ford frames by an INDEPENDENT test-side packer, decoded
 * by the real decoder, and compared channel-for-channel against direct drive.
 *
 * Runs on the host:
 *   gcc -std=c11 -Wall -Werror -I MustangDash -I tests \
 *       tests/test_dash_can_ford.c -lm -o /tmp/tdcf && /tmp/tdcf
 */

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "dash_data.h"
#include "dash_can_ford.h"
#include "dash_sim.h"
#include "golden_can_ford.h"

/* ---- constants that must never drift ---- */
_Static_assert(DASH_CAN_FORD_ID_ENGINE == 0x270u,
               "the engine-speed frame must stay at 0x270");
_Static_assert(DASH_CAN_FORD_ID_AF == 0x274u,
               "the A/F + fuel-pressure frame must stay at 0x274");
_Static_assert(DASH_CAN_FORD_ID_VSPD == 0x275u,
               "the vehicle-speed frame must stay at 0x275");
_Static_assert(DASH_CAN_FORD_ID_TEMPS == 0x278u,
               "the temps/pressure/volts frame must stay at 0x278");
_Static_assert(DASH_CH_COUNT == 26,
               "the Ford dialect maps 9 of exactly 26 channels (KTD2)");
_Static_assert(DASH_CAN_FORD_STALE_MS == 500u,
               "the staleness window is 500 ms: >= 3x the slowest (10 Hz) frame");
_Static_assert(DASH_CAN_FORD_TEMP_DEGRADED == 214
                   && DASH_CAN_FORD_TEMP_FAULTED == 215,
               "Ford's temperature quality sentinels are raw 214/215");
/* The golden header pins its own array sizes against these counts; pinning
 * the counts here means a regenerated header cannot silently shrink. */
_Static_assert(GOLDEN_CAN_VECTOR_COUNT == 21u,
               "the golden header must ship all 21 vectors");
_Static_assert(GOLDEN_CAN_EXPECT_COUNT == 44u,
               "the golden header must ship all 44 expectations");

/* The 9 CAN-covered channels (KTD2), transcribed HERE from the plan rather
 * than taken from the decoder's claim macros, so a claim-set slip in
 * dash_can_ford.h fails a test instead of being a shared assumption. */
#define FORD9_MASK (DASH_CH_BIT(DASH_CH_RPM)   | DASH_CH_BIT(DASH_CH_SPEED) | \
                    DASH_CH_BIT(DASH_CH_ECT)   | DASH_CH_BIT(DASH_CH_OILT)  | \
                    DASH_CH_BIT(DASH_CH_OILP)  | DASH_CH_BIT(DASH_CH_VOLTS) | \
                    DASH_CH_BIT(DASH_CH_AFR_L) | DASH_CH_BIT(DASH_CH_AFR_R) | \
                    DASH_CH_BIT(DASH_CH_FUELP))
_Static_assert(FORD9_MASK == DASH_CAN_FORD_CLAIMS,
               "the decoder's claim set must be exactly the 9 mapped channels");

static int failures = 0;

static void expect(int cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

static int nearf(float a, float b, float eps)
{
    float d = a - b;
    if (d < 0.0f) { d = -d; }
    return d <= eps;
}

/* Per-channel quantization tolerance, from the golden header's derived
 * constants (one raw quantum through the unit conversion; KTD8). */
static float golden_tol(uint8_t ch)
{
    switch (ch)
    {
        case DASH_CH_RPM:   return GOLDEN_CAN_TOL_RPM;
        case DASH_CH_SPEED: return GOLDEN_CAN_TOL_SPEED_MPH;
        case DASH_CH_ECT:   return GOLDEN_CAN_TOL_ECT_F;
        case DASH_CH_OILT:  return GOLDEN_CAN_TOL_OILT_F;
        case DASH_CH_OILP:  return GOLDEN_CAN_TOL_OILP_PSI;
        case DASH_CH_VOLTS: return GOLDEN_CAN_TOL_VOLTS;
        case DASH_CH_AFR_L: return GOLDEN_CAN_TOL_AFR;
        case DASH_CH_AFR_R: return GOLDEN_CAN_TOL_AFR;
        case DASH_CH_FUELP: return GOLDEN_CAN_TOL_FUELP_PSI;
        default:            return 0.0f; /* not a Ford channel: exact or bust */
    }
}

static const GoldenCanVector *find_vector(const char *name)
{
    for (unsigned i = 0u; i < GOLDEN_CAN_VECTOR_COUNT; i++)
    {
        if (strcmp(golden_can_vectors[i].name, name) == 0)
        {
            return &golden_can_vectors[i];
        }
    }
    return NULL;
}

static void decode_vec(DashCanFord *cf, const GoldenCanVector *v,
                       uint32_t now_ms, DashState *s)
{
    dash_can_ford_decode(cf, v->id, v->dlc, v->bytes, now_ms, s);
}

/* Decode the four nominal frames -- one full set of the dialect -- at t. */
static void decode_nominal_set(DashCanFord *cf, uint32_t now_ms, DashState *s)
{
    static const char *const nominals[4] = {
        "nominal_0270_rpm", "nominal_0274_afr_fuelp",
        "nominal_0275_speed", "nominal_0278_temps"
    };
    for (int i = 0; i < 4; i++)
    {
        const GoldenCanVector *v = find_vector(nominals[i]);
        expect(v != NULL, "every nominal golden vector must exist by name");
        if (v != NULL) { decode_vec(cf, v, now_ms, s); }
    }
}

/* ==== Covers R11: test-side ENCODE-ONLY Ford packer =====================
 *
 * A third independent transcription of the official Ford tables
 * (IS_M-6017-M50HM p.36 / IS_M-6017-73M s.12.0): it shares NO extraction
 * code with dash_can_ford.h and none of its constants -- the spec numbers
 * below are written from the table, so a transcription slip in the decoder
 * cannot be mirrored here and vanish.
 *
 * Table (Motorola; Ford numbers bits 0..63 with bit 0 = MSB of byte 0, so
 * bit n lives in byte n/8 at in-byte position 7 - n%8):
 *   0x270: ENGINE_SPEED   bits  2-15 (14-bit, 1 rpm/bit)
 *   0x274: AF0            bits 25-35 (11-bit, A/F = raw*0.01 + 7)
 *          AF1            bits 36-46 (11-bit, same encoding)
 *          FUEL_PRESSURE  bits 47-55 ( 9-bit, kPa)
 *   0x275: VSPD           bits  0-11 (12-bit, 0.1 km/h per bit)
 *   0x278: ECT            byte 0     (degC = raw - 40)
 *          EOT            byte 1     (degC = raw - 40)
 *          EOP            bits 24-33 (10-bit, kPa)
 *          VBAT           bits 48-58 (11-bit, 0.01 V/bit)
 */
#define RT_PSI_PER_KPA 0.145037738f
#define RT_KMH_PER_MPH 1.60934f

/* Write `value` MSB-first into Ford bit positions start..start+n_bits-1. */
static void ford_put_bits(uint8_t bytes[8], uint8_t start_bit, uint8_t n_bits,
                          uint16_t value)
{
    for (uint8_t i = 0u; i < n_bits; i++)
    {
        const uint8_t pos = (uint8_t) (start_bit + i);
        if (((value >> (n_bits - 1u - i)) & 1u) != 0u)
        {
            bytes[pos / 8u] |= (uint8_t) (1u << (7u - (pos % 8u)));
        }
    }
}

/* Round a raw field value to the nearest count, clamped to [0, raw_max]
 * (the field's own range: an out-of-range dash value saturates rather than
 * aliasing into a neighboring signal). !(> 0) also catches NaN. */
static uint16_t ford_clamp_raw(float raw, float raw_max)
{
    if (!(raw > 0.0f)) { return 0u; }
    if (raw > raw_max) { raw = raw_max; }
    return (uint16_t) lroundf(raw);
}

static void ford_pack_0270(uint8_t bytes[8], float rpm)
{
    memset(bytes, 0, 8u);
    ford_put_bits(bytes, 2u, 14u, ford_clamp_raw(rpm, 16383.0f));
}

static void ford_pack_0274(uint8_t bytes[8], float afr_l, float afr_r,
                           float fuelp_psi)
{
    memset(bytes, 0, 8u);
    ford_put_bits(bytes, 25u, 11u,
                  ford_clamp_raw((afr_l - 7.0f) * 100.0f, 2047.0f));
    ford_put_bits(bytes, 36u, 11u,
                  ford_clamp_raw((afr_r - 7.0f) * 100.0f, 2047.0f));
    ford_put_bits(bytes, 47u, 9u,
                  ford_clamp_raw(fuelp_psi / RT_PSI_PER_KPA, 511.0f));
}

static void ford_pack_0275(uint8_t bytes[8], float speed_mph)
{
    memset(bytes, 0, 8u);
    ford_put_bits(bytes, 0u, 12u,
                  ford_clamp_raw(speed_mph * RT_KMH_PER_MPH * 10.0f, 4095.0f));
}

static void ford_pack_0278(uint8_t bytes[8], float ect_f, float oilt_f,
                           float oilp_psi, float volts)
{
    memset(bytes, 0, 8u);
    ford_put_bits(bytes, 0u, 8u,
                  ford_clamp_raw((ect_f - 32.0f) * 5.0f / 9.0f + 40.0f, 255.0f));
    ford_put_bits(bytes, 8u, 8u,
                  ford_clamp_raw((oilt_f - 32.0f) * 5.0f / 9.0f + 40.0f, 255.0f));
    ford_put_bits(bytes, 24u, 10u,
                  ford_clamp_raw(oilp_psi / RT_PSI_PER_KPA, 1023.0f));
    ford_put_bits(bytes, 48u, 11u,
                  ford_clamp_raw(volts * 100.0f, 2047.0f));
}

int main(void)
{
    char msg[200];

    /* ==== Covers AE4: every golden vector decodes to its expectations ==== */
    {
        DashState s;
        DashCanFord cf;
        bool burst_active = false;

        for (unsigned vi = 0u; vi < GOLDEN_CAN_VECTOR_COUNT; vi++)
        {
            const GoldenCanVector *v = &golden_can_vectors[vi];
            const bool burst = (strncmp(v->name, "burst_", 6u) == 0);

            /* Fresh state per vector; the multi-frame burst shares one state
             * across its frames (that is what makes it a burst). */
            if (!burst || !burst_active)
            {
                dash_state_init(&s);
                dash_can_ford_init(&cf);
            }
            burst_active = burst;

            if (v->ext != 0u)
            {
                /* The decode API takes 11-bit standard identifier VALUES
                 * only: the firmware drain filters on IdType BEFORE calling
                 * in (U6), so this test mirrors that guard by never
                 * forwarding an extended frame -- treating ext=1 vectors as
                 * must-be-ignored is the test's own obligation. */
                snprintf(msg, sizeof msg,
                         "golden %s: an extended-ID vector must be a "
                         "must-ignore vector (n_expect 0)", v->name);
                expect(v->n_expect == 0u, msg);
                continue;
            }

            decode_vec(&cf, v, 0u, &s);

            if (v->n_expect == 0u)
            {
                /* Ignored frame into a fresh state: nothing may become
                 * valid or claimed. The stronger pre-populated check (state
                 * untouched, last-seen not refreshed) follows below. */
                snprintf(msg, sizeof msg,
                         "golden %s: an ignored frame must validate and "
                         "claim nothing from a fresh state", v->name);
                expect(s.valid == 0u && s.can_owned == 0u, msg);
                continue;
            }

            for (uint8_t k = 0u; k < v->n_expect; k++)
            {
                const GoldenCanExpect *e =
                    &golden_can_expects[v->first_expect + k];
                if (e->expect_valid != 0u)
                {
                    snprintf(msg, sizeof msg,
                             "golden %s: channel %u must be valid after the "
                             "decode", v->name, (unsigned) e->ch);
                    expect(dash_ch_valid(&s, e->ch), msg);
                    snprintf(msg, sizeof msg,
                             "golden %s: channel %u must decode to %g within "
                             "its quantization tolerance",
                             v->name, (unsigned) e->ch, (double) e->value);
                    expect(nearf(dash_ch_get(&s, e->ch), e->value,
                                 golden_tol(e->ch)), msg);
                }
                else
                {
                    snprintf(msg, sizeof msg,
                             "golden %s: channel %u must be INVALID "
                             "(sentinel dead-front)", v->name, (unsigned) e->ch);
                    expect(!dash_ch_valid(&s, e->ch), msg);
                }
                /* Accepted frames claim their channels either way: a
                 * sentinel is the ECU speaking for that sensor. */
                snprintf(msg, sizeof msg,
                         "golden %s: channel %u must be CAN-claimed after "
                         "the decode", v->name, (unsigned) e->ch);
                expect((s.can_owned & DASH_CH_BIT(e->ch)) != 0u, msg);
            }
        }
    }

    /* ---- ignored frames against a POPULATED state: valid/can_owned
     * unchanged, values untouched, and last-seen not refreshed (proven by a
     * subsequent expiry pass releasing every claim). ---- */
    {
        DashState s;
        DashCanFord cf;
        static const uint8_t ford9[9] = {
            DASH_CH_RPM, DASH_CH_SPEED, DASH_CH_ECT, DASH_CH_OILT,
            DASH_CH_OILP, DASH_CH_VOLTS, DASH_CH_AFR_L, DASH_CH_AFR_R,
            DASH_CH_FUELP
        };
        float snap_vals[9];

        dash_state_init(&s);
        dash_can_ford_init(&cf);
        decode_nominal_set(&cf, 0u, &s);
        expect(s.valid == FORD9_MASK && s.can_owned == FORD9_MASK,
               "one nominal set must validate and claim exactly the 9 mapped "
               "channels");
        for (int k = 0; k < 9; k++) { snap_vals[k] = dash_ch_get(&s, ford9[k]); }

        for (unsigned vi = 0u; vi < GOLDEN_CAN_VECTOR_COUNT; vi++)
        {
            const GoldenCanVector *v = &golden_can_vectors[vi];
            if (v->n_expect != 0u || v->ext != 0u) { continue; }
            decode_vec(&cf, v, 400u, &s);
            snprintf(msg, sizeof msg,
                     "golden %s: an ignored frame must leave the valid and "
                     "can_owned masks unchanged", v->name);
            expect(s.valid == FORD9_MASK && s.can_owned == FORD9_MASK, msg);
            for (int k = 0; k < 9; k++)
            {
                snprintf(msg, sizeof msg,
                         "golden %s: an ignored frame must leave channel %u's "
                         "value untouched", v->name, (unsigned) ford9[k]);
                expect(dash_ch_get(&s, ford9[k]) == snap_vals[k], msg);
            }
        }

        /* If any ignored frame had refreshed a message's last-seen at t=400,
         * that message would still be fresh at t=501 and its claim would
         * survive this pass. All claims date from t=0, so all must go. */
        dash_can_ford_expire(&cf, 501u, false, &s);
        expect(s.can_owned == 0u,
               "ignored frames must not refresh last-seen: every claim from "
               "t=0 must expire at t=501");
    }

    /* ==== sentinel guarded path: dead-front an ALREADY-VALID channel ==== */
    {
        DashState s;
        DashCanFord cf;
        const GoldenCanVector *nom = find_vector("nominal_0278_temps");
        const GoldenCanVector *sect = find_vector("sentinel_ect_degraded");
        const GoldenCanVector *seot = find_vector("sentinel_eot_faulted");
        expect(nom != NULL && sect != NULL && seot != NULL,
               "the 0x278 nominal and sentinel golden vectors must exist by name");

        dash_state_init(&s);
        dash_can_ford_init(&cf);
        decode_vec(&cf, nom, 0u, &s);
        expect(dash_ch_valid(&s, DASH_CH_ECT)
                   && (s.can_owned & DASH_CH_BIT(DASH_CH_ECT)) != 0u,
               "a valid ECT frame must validate and claim the channel before "
               "the sentinel arrives");

        /* ECT raw 214 (degraded): the guarded invalidation path, because
         * can_owned is already set and dash_ch_invalidate would no-op. */
        decode_vec(&cf, sect, 10u, &s);
        expect(!dash_ch_valid(&s, DASH_CH_ECT),
               "ECT raw 214 (degraded) must dead-front an already-valid ECT");
        expect((s.can_owned & DASH_CH_BIT(DASH_CH_ECT)) != 0u,
               "the ECT sentinel must keep the channel CAN-claimed (Dead "
               "state), not release it to the sim");
        expect(dash_ch_valid(&s, DASH_CH_OILT)
                   && nearf(dash_ch_get(&s, DASH_CH_OILT), 176.0f,
                            GOLDEN_CAN_TOL_OILT_F),
               "EOT in the same sentinel frame must stay valid at 176 degF");
        expect(!dash_ch_sim_owned(&s, DASH_CH_ECT),
               "a dead ECT must not be sim-owned: the sim cannot repaint a "
               "degraded sensor");
        if (dash_ch_sim_owned(&s, DASH_CH_ECT))
        {
            dash_ch_set(&s, DASH_CH_ECT, 190.0f); /* the sim's guarded idiom */
        }
        expect(!dash_ch_valid(&s, DASH_CH_ECT),
               "a guarded sim write must not land on the dead ECT channel");

        /* Symmetric: EOT raw 215 (faulted); ECT in that frame is healthy
         * again, which also exercises Dead -> CanOwned revalidation. */
        decode_vec(&cf, seot, 20u, &s);
        expect(!dash_ch_valid(&s, DASH_CH_OILT),
               "EOT raw 215 (faulted) must dead-front an already-valid OILT");
        expect((s.can_owned & DASH_CH_BIT(DASH_CH_OILT)) != 0u,
               "the EOT sentinel must keep OILT CAN-claimed (Dead state)");
        expect(dash_ch_valid(&s, DASH_CH_ECT)
                   && nearf(dash_ch_get(&s, DASH_CH_ECT), 194.0f,
                            GOLDEN_CAN_TOL_ECT_F),
               "a healthy ECT in the sentinel frame must revalidate the dead "
               "ECT at 194 degF (Dead -> CanOwned)");
        expect(!dash_ch_sim_owned(&s, DASH_CH_OILT),
               "a dead OILT must not be sim-owned");
        if (dash_ch_sim_owned(&s, DASH_CH_OILT))
        {
            dash_ch_set(&s, DASH_CH_OILT, 200.0f);
        }
        expect(!dash_ch_valid(&s, DASH_CH_OILT),
               "a guarded sim write must not land on the dead OILT channel");
    }

    /* ==== Covers AE2: staleness, bench and car semantics ==== */
    {
        DashState s;
        DashCanFord cf;

        /* bench semantics: release on expiry, sim reclaims (R8) */
        dash_state_init(&s);
        dash_can_ford_init(&cf);
        decode_nominal_set(&cf, 0u, &s);

        dash_can_ford_expire(&cf, 499u, false, &s);
        expect(s.can_owned == FORD9_MASK && s.valid == FORD9_MASK,
               "at 499 ms nothing may expire: the window is 500 ms strict-"
               "greater");
        dash_can_ford_expire(&cf, 500u, false, &s);
        expect(s.can_owned == FORD9_MASK && s.valid == FORD9_MASK,
               "at exactly 500 ms the window has not been EXCEEDED, so "
               "nothing may expire");

        dash_can_ford_expire(&cf, 501u, false, &s);
        expect(s.can_owned == 0u,
               "at 501 ms bench semantics must release every claim (R8)");
        expect(s.valid == FORD9_MASK,
               "bench expiry must leave the channels valid at their last "
               "values until the sim rewrites them");
        expect(nearf(dash_ch_get(&s, DASH_CH_RPM), 3500.0f,
                     GOLDEN_CAN_TOL_RPM),
               "the last CAN RPM value must survive a bench release");
        expect(dash_ch_sim_owned(&s, DASH_CH_RPM),
               "a bench-released channel must be sim-owned again");
        if (dash_ch_sim_owned(&s, DASH_CH_RPM))
        {
            dash_ch_set(&s, DASH_CH_RPM, 1234.0f); /* the sim's guarded idiom */
        }
        expect(nearf(dash_ch_get(&s, DASH_CH_RPM), 1234.0f, 0.001f),
               "the sim's write must land after a bench release");

        /* car semantics: dead-front with the claim latched (R7) */
        dash_state_init(&s);
        dash_can_ford_init(&cf);
        decode_nominal_set(&cf, 0u, &s);

        dash_can_ford_expire(&cf, 501u, true, &s);
        expect(s.valid == 0u,
               "at 501 ms car semantics must dead-front every CAN channel");
        expect(s.can_owned == FORD9_MASK,
               "car expiry must keep every claim LATCHED, holding the sim out");
        expect(!dash_ch_sim_owned(&s, DASH_CH_RPM),
               "the sim must stay locked out of a car-latched channel");
        if (dash_ch_sim_owned(&s, DASH_CH_RPM))
        {
            dash_ch_set(&s, DASH_CH_RPM, 1234.0f);
        }
        expect(!dash_ch_valid(&s, DASH_CH_RPM),
               "a guarded sim write must not land on a car-latched channel");

        /* Dead -> CanOwned: only a fresh frame lifts the car latch */
        {
            const GoldenCanVector *rpm = find_vector("nominal_0270_rpm");
            expect(rpm != NULL, "the nominal 0x270 vector must exist by name");
            if (rpm != NULL) { decode_vec(&cf, rpm, 600u, &s); }
        }
        expect(dash_ch_valid(&s, DASH_CH_RPM)
                   && nearf(dash_ch_get(&s, DASH_CH_RPM), 3500.0f,
                            GOLDEN_CAN_TOL_RPM),
               "a fresh frame must revalidate the dead RPM channel (Dead -> "
               "CanOwned)");
        dash_can_ford_expire(&cf, 601u, true, &s);
        expect(dash_ch_valid(&s, DASH_CH_RPM),
               "RPM must survive the next expiry pass: its message is fresh "
               "again");
        expect(!dash_ch_valid(&s, DASH_CH_SPEED),
               "SPEED must stay dead-fronted: 0x275 is still stale in car "
               "semantics");
    }

    /* ==== Covers AE3: serial precedence over CAN (R9) ==== */
    {
        DashState s;
        DashCanFord cf;
        const GoldenCanVector *rpm = find_vector("nominal_0270_rpm");
        expect(rpm != NULL, "the nominal 0x270 vector must exist by name");

        /* overridden blocks (mimic dash_serial_do_set_: write + valid +
         * overridden, sticky clear dropped) */
        dash_state_init(&s);
        dash_can_ford_init(&cf);
        dash_ch_set(&s, DASH_CH_RPM, 3200.0f);
        s.overridden |= DASH_CH_BIT(DASH_CH_RPM);
        s.cleared = (uint32_t) (s.cleared & ~DASH_CH_BIT(DASH_CH_RPM));
        if (rpm != NULL) { decode_vec(&cf, rpm, 0u, &s); }
        expect(nearf(dash_ch_get(&s, DASH_CH_RPM), 3200.0f, 0.001f),
               "a CAN frame must not overwrite a serial-overridden RPM");
        expect((s.can_owned & DASH_CH_BIT(DASH_CH_RPM)) == 0u,
               "the decoder must not claim a serial-overridden channel");

        /* cleared blocks too (mimic `clear rpm`: valid dropped, cleared set,
         * override dropped) */
        dash_state_init(&s);
        dash_can_ford_init(&cf);
        s.valid = (uint32_t) (s.valid & ~DASH_CH_BIT(DASH_CH_RPM));
        s.cleared |= DASH_CH_BIT(DASH_CH_RPM);
        s.overridden = (uint32_t) (s.overridden & ~DASH_CH_BIT(DASH_CH_RPM));
        if (rpm != NULL) { decode_vec(&cf, rpm, 0u, &s); }
        expect(!dash_ch_valid(&s, DASH_CH_RPM),
               "a CAN frame must not revalidate a serial-cleared channel");
        expect((s.can_owned & DASH_CH_BIT(DASH_CH_RPM)) == 0u,
               "the decoder must not claim a serial-cleared channel");

        /* `sim on` wipes the serial masks; a fresh frame then writes */
        s.overridden = 0u;
        s.cleared = 0u;
        if (rpm != NULL) { decode_vec(&cf, rpm, 10u, &s); }
        expect(dash_ch_valid(&s, DASH_CH_RPM)
                   && nearf(dash_ch_get(&s, DASH_CH_RPM), 3500.0f,
                            GOLDEN_CAN_TOL_RPM),
               "after `sim on` wipes the serial masks a fresh CAN frame must "
               "write RPM");
        expect((s.can_owned & DASH_CH_BIT(DASH_CH_RPM)) != 0u,
               "after `sim on` a fresh CAN frame must claim the channel again");
    }

    /* ==== `sim on` does not clear can_owned (only expiry manages it) ==== */
    {
        DashState s;
        DashCanFord cf;
        dash_state_init(&s);
        dash_can_ford_init(&cf);
        decode_nominal_set(&cf, 0u, &s);
        /* leave a stale serial override behind, then wipe as DASH_CMD_SIM
         * with sim_on does: overridden = 0, cleared = 0 -- and nothing else */
        s.overridden |= DASH_CH_BIT(DASH_CH_FUEL);
        s.overridden = 0u;
        s.cleared = 0u;
        expect(s.can_owned == FORD9_MASK,
               "`sim on` must not clear can_owned: only the decoder and its "
               "expiry pass manage it");
        expect(!dash_ch_sim_owned(&s, DASH_CH_RPM),
               "after `sim on` a CAN-fresh channel must remain locked against "
               "the sim");
    }

    /* ==== range edges: min/max are golden vectors above; explicit
     * float-overflow sanity on the maxima ==== */
    {
        DashState s;
        DashCanFord cf;
        static const char *const maxima[4] = {
            "max_0270", "max_0274", "max_0275", "max_0278"
        };
        dash_state_init(&s);
        dash_can_ford_init(&cf);
        for (int i = 0; i < 4; i++)
        {
            const GoldenCanVector *v = find_vector(maxima[i]);
            expect(v != NULL, "every max-edge golden vector must exist by name");
            if (v != NULL) { decode_vec(&cf, v, 0u, &s); }
        }
        expect(nearf(dash_ch_get(&s, DASH_CH_RPM), 16383.0f,
                     GOLDEN_CAN_TOL_RPM),
               "RPM raw max 16383 must decode to 16383 rpm without overflow");
        expect(nearf(dash_ch_get(&s, DASH_CH_OILP),
                     1023.0f * RT_PSI_PER_KPA, GOLDEN_CAN_TOL_OILP_PSI),
               "OILP raw max 1023 kPa must decode to ~148.4 psi within "
               "tolerance");
        expect(nearf(dash_ch_get(&s, DASH_CH_VOLTS), 20.47f,
                     GOLDEN_CAN_TOL_VOLTS),
               "VBAT raw max 2047 must decode to 20.47 V");
        for (uint8_t ch = 0u; ch < (uint8_t) DASH_CH_COUNT; ch++)
        {
            if ((FORD9_MASK & DASH_CH_BIT(ch)) == 0u) { continue; }
            snprintf(msg, sizeof msg,
                     "channel %u must decode to a finite value at the raw "
                     "maxima", (unsigned) ch);
            expect(isfinite(dash_ch_get(&s, ch)), msg);
        }
    }

    /* ==== Covers R11: sim -> encode -> decode -> DashState round-trip ====
     * The REAL simulator drives a reference DashState directly; each step
     * its 9 CAN-covered channels are converted back to Ford raw, packed by
     * the test-side encoder above, decoded by the REAL decoder into a second
     * DashState, and the two must agree within one raw quantum per channel.
     * Deterministic by construction: dash_sim_init seeds the LCG with its
     * fixed SIM_LCG_SEED, and no wall clock is involved. */
    {
        DashState ref, can;
        DashSimState sim;
        DashCanFord cf;
        static const uint8_t ford9[9] = {
            DASH_CH_RPM, DASH_CH_SPEED, DASH_CH_ECT, DASH_CH_OILT,
            DASH_CH_OILP, DASH_CH_VOLTS, DASH_CH_AFR_L, DASH_CH_AFR_R,
            DASH_CH_FUELP
        };
        /* one raw quantum through the conversion (the golden bounds) plus a
         * hair of float slack; derived once, never widened (KTD8) */
        const float rt_eps = 1e-4f;
        float max_dev[9] = { 0.0f };
        int compared[9] = { 0 };
        int mismatched[9] = { 0 };

        dash_state_init(&ref);
        dash_state_init(&can);
        dash_sim_init(&sim);
        dash_can_ford_init(&cf);

        for (int step = 0; step < 600; step++)
        {
            const uint32_t now = (uint32_t) (step + 1) * 16u;
            uint8_t b[8];

            dash_sim_step(&sim, &ref, 16u); /* direct drive: the reference */

            ford_pack_0270(b, dash_ch_get(&ref, DASH_CH_RPM));
            dash_can_ford_decode(&cf, 0x270u, 8u, b, now, &can);
            ford_pack_0274(b, dash_ch_get(&ref, DASH_CH_AFR_L),
                           dash_ch_get(&ref, DASH_CH_AFR_R),
                           dash_ch_get(&ref, DASH_CH_FUELP));
            dash_can_ford_decode(&cf, 0x274u, 8u, b, now, &can);
            ford_pack_0275(b, dash_ch_get(&ref, DASH_CH_SPEED));
            dash_can_ford_decode(&cf, 0x275u, 8u, b, now, &can);
            ford_pack_0278(b, dash_ch_get(&ref, DASH_CH_ECT),
                           dash_ch_get(&ref, DASH_CH_OILT),
                           dash_ch_get(&ref, DASH_CH_OILP),
                           dash_ch_get(&ref, DASH_CH_VOLTS));
            dash_can_ford_decode(&cf, 0x278u, 8u, b, now, &can);

            for (int k = 0; k < 9; k++)
            {
                const uint8_t ch = ford9[k];
                if (!dash_ch_valid(&ref, ch))
                {
                    continue; /* the sim has not published it yet */
                }
                compared[k]++;
                if (!dash_ch_valid(&can, ch))
                {
                    mismatched[k]++;
                    continue;
                }
                const float dev =
                    fabsf(dash_ch_get(&can, ch) - dash_ch_get(&ref, ch));
                if (dev > max_dev[k]) { max_dev[k] = dev; }
                if (dev > golden_tol(ch) + rt_eps) { mismatched[k]++; }
            }
        }

        for (int k = 0; k < 9; k++)
        {
            snprintf(msg, sizeof msg,
                     "round-trip: channel %u must be compared at least once "
                     "over 600 steps", (unsigned) ford9[k]);
            expect(compared[k] > 0, msg);
            snprintf(msg, sizeof msg,
                     "round-trip: channel %u must match direct drive within "
                     "one raw quantum on every step (max deviation %g)",
                     (unsigned) ford9[k], (double) max_dev[k]);
            expect(mismatched[k] == 0, msg);
        }
    }

    if (failures == 0)
    {
        printf("OK: ford dialect decodes, owns, expires, and round-trips the sim\n");
        return 0;
    }
    return 1;
}
