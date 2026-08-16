/*
 * dash_can_ford.h -- Ford control-pack CAN dialect decoder
 * (plan 2026-08-15-001 U3, KTD1-KTD6).
 *
 * Pure header: stdint/stdbool only, no HAL/Arduino includes, host-testable
 * (tests/test_dash_can_ford.c, plan U4). The firmware side (dash_can.h)
 * drains FDCAN1's FIFO and hands raw frames here; the golden vectors and the
 * PC emitter exercise the same entry points with no hardware.
 *
 * Bit positions, scaling, and sentinels are transcribed DIRECTLY from the
 * official Ford tables: IS_M-6017-M50HM (Gen 4X, 09/16/24) p.36 CAN Message
 * Definition; IS_M-6017-73M (7.3L) section 12.0 p.29 (byte-identical). They
 * are deliberately NOT read out of the DBC under assets/can/ -- the DBC and
 * this header must stay two independent transcriptions of the spec, so a
 * transcription slip has to reproduce in two languages before the
 * golden-frame tests go green (KTD8).
 *
 * Frame format (KTD1): four messages, 11-bit standard IDs, DLC 8, 500 kbps
 * classic CAN, Motorola/big-endian bit order. Ford numbers bits 0..63 with
 * bit 0 = the MSB of byte 0, so byte b carries bits 8b (0x80) .. 8b+7 (0x01).
 * CALLERS MUST PASS STANDARD-ID FRAMES ONLY: `id` is the 11-bit identifier
 * VALUE, so an extended frame whose 29-bit identifier happens to equal 0x270
 * is indistinguishable here -- the firmware drain filters on IdType before
 * calling in (U6), and host tests must not fabricate such frames as input.
 *
 * Ownership (KTD5): an accepted frame writes its channels via dash_ch_set
 * and sets their bits in DashState.can_owned, which locks the simulator out
 * (dash_ch_sim_owned). Serial stays supreme (R9): a channel under
 * `overridden|cleared` is never written and never claimed. Staleness (KTD6):
 * a message unseen for DASH_CAN_FORD_STALE_MS releases its claim on the
 * bench (sim reclaims, R8) or, in the car, keeps the claim latched while the
 * channels dead-front (R7) -- releasing there would let the still-running
 * sim re-validate the channel one loop later, putting sim fiction on car
 * glass. Only a fresh frame lifts the car latch.
 */

#ifndef DASH_CAN_FORD_H
#define DASH_CAN_FORD_H

#include <stdint.h>
#include <stdbool.h>

#include "dash_data.h"

/* ---- the 0x270-set (KTD1): IDs and broadcast rates ---- */
#define DASH_CAN_FORD_ID_ENGINE 0x270u /* 100 Hz: engine speed */
#define DASH_CAN_FORD_ID_AF     0x274u /* 50 Hz: AF0/AF1 wideband, fuel pressure */
#define DASH_CAN_FORD_ID_VSPD   0x275u /* 50 Hz: vehicle speed */
#define DASH_CAN_FORD_ID_TEMPS  0x278u /* 10 Hz: ECT/EOT, oil pressure, VBAT */

/* Staleness window (KTD6): >= 3x the slowest frame period (0x278 at 10 Hz),
 * comfortably sub-second per R7. A message is stale when now - last_seen
 * EXCEEDS this (strict >); uint32 wrap-safe via unsigned subtraction on the
 * same monotonic millis clock the rest of the dash runs on. */
#define DASH_CAN_FORD_STALE_MS 500u

/* Ford's own quality sentinels in the one-byte temperatures (KTD3): the ECU
 * reporting "degraded" / "faulted" instead of a reading. Never converted --
 * the channel dead-fronts, which is exactly the dash's invalid-renders-`--`
 * / no-alarm rule. Unsuffixed on purpose: they compare against an
 * int-promoted uint8_t. */
#define DASH_CAN_FORD_TEMP_DEGRADED 214
#define DASH_CAN_FORD_TEMP_FAULTED  215

/* Unit conversions -- decoder policy, not DBC content (KTD2): DashState
 * channels are degF/psi/mph by contract, Ford broadcasts degC/kPa/km/h. */
#define DASH_CAN_FORD_KPA_TO_PSI 0.145037738f
#define DASH_CAN_FORD_KMH_PER_MPH 1.60934f

/* Per-message index into DashCanFord's last-seen bookkeeping. */
enum {
    DASH_CAN_FORD_MSG_ENGINE = 0,
    DASH_CAN_FORD_MSG_AF,
    DASH_CAN_FORD_MSG_VSPD,
    DASH_CAN_FORD_MSG_TEMPS,
    DASH_CAN_FORD_MSG_COUNT
};

/* What each message claims (KTD5). Disjoint by construction -- and the
 * dialect-wide invariant recorded in dash_data.h holds ACROSS dialects too:
 * a channel may be claimed by at most one CAN dialect. */
#define DASH_CAN_FORD_CLAIM_ENGINE (DASH_CH_BIT(DASH_CH_RPM))
#define DASH_CAN_FORD_CLAIM_AF     (DASH_CH_BIT(DASH_CH_AFR_L) | \
                                    DASH_CH_BIT(DASH_CH_AFR_R) | \
                                    DASH_CH_BIT(DASH_CH_FUELP))
#define DASH_CAN_FORD_CLAIM_VSPD   (DASH_CH_BIT(DASH_CH_SPEED))
#define DASH_CAN_FORD_CLAIM_TEMPS  (DASH_CH_BIT(DASH_CH_ECT)  | \
                                    DASH_CH_BIT(DASH_CH_OILT) | \
                                    DASH_CH_BIT(DASH_CH_OILP) | \
                                    DASH_CH_BIT(DASH_CH_VOLTS))
/* The full Ford claim set: 9 of the 26 channels (KTD2). */
#define DASH_CAN_FORD_CLAIMS (DASH_CAN_FORD_CLAIM_ENGINE | \
                              DASH_CAN_FORD_CLAIM_AF     | \
                              DASH_CAN_FORD_CLAIM_VSPD   | \
                              DASH_CAN_FORD_CLAIM_TEMPS)

/* Decoder state: per-message freshness only. Channel values and ownership
 * live in DashState; this struct never duplicates them. `seen` exists
 * because last_seen_ms == 0 is a legal timestamp, not a "never": expiry must
 * ignore messages that have not arrived at all (nothing claimed, nothing to
 * dead-front). Zero-init is the boot contract ({0}, like dash_state_init). */
typedef struct {
    uint32_t last_seen_ms[DASH_CAN_FORD_MSG_COUNT];
    bool seen[DASH_CAN_FORD_MSG_COUNT];
} DashCanFord;

static inline void dash_can_ford_init(DashCanFord *cf)
{
    const DashCanFord zero = {0};
    *cf = zero;
}

/* Serial supremacy (R9): a channel under `set` or `clear` belongs to the
 * operator, and the decoder neither writes nor claims it. Deliberately NOT
 * dash_ch_sim_owned -- that predicate now includes can_owned, which is this
 * decoder's OWN mask and must not veto its writes. */
static inline bool dash_can_ford_serial_owned_(const DashState *s, uint8_t ch)
{
    return ((s->overridden | s->cleared) & DASH_CH_BIT(ch)) != 0U;
}

/* Decoder-owned invalidate: drop the valid bit, honoring ONLY the serial
 * masks. dash_ch_invalidate is the SIM-facing variant -- it early-returns
 * unless dash_ch_sim_owned, so on a CAN-owned channel it would silently
 * no-op, which is precisely the channel this decoder needs to dead-front
 * (sentinels, car-side expiry). The value itself is never disturbed --
 * only the valid bit moves, same contract as dash_ch_invalidate. */
static inline void dash_can_ford_invalidate(DashState *s, uint8_t ch)
{
    if (ch >= DASH_CH_COUNT) { return; }
    if (dash_can_ford_serial_owned_(s, ch)) { return; }
    s->valid = (uint32_t) (s->valid & ~DASH_CH_BIT(ch));
}

/* Guarded write (the dash_sim.h idiom, CAN flavor): value lands and the
 * claim is taken only when serial does not own the channel. */
static inline void dash_can_ford_write_(DashState *s, uint8_t ch, float v)
{
    if (dash_can_ford_serial_owned_(s, ch)) { return; }
    dash_ch_set(s, ch, v);
    s->can_owned |= DASH_CH_BIT(ch);
}

/* ECT/EOT share one encoding: a single byte, degC = raw - 40, with Ford's
 * quality sentinels at the top of the range (KTD3). A sentinel still CLAIMS
 * the channel -- the ECU is speaking for that sensor, and leaving it
 * sim-owned would put fiction over a faulted sensor -- but the value is
 * never converted: the channel dead-fronts instead. */
static inline void dash_can_ford_temp_(DashState *s, uint8_t ch, uint8_t raw)
{
    if (dash_can_ford_serial_owned_(s, ch)) { return; }
    s->can_owned |= DASH_CH_BIT(ch);
    if ((raw == DASH_CAN_FORD_TEMP_DEGRADED) || (raw == DASH_CAN_FORD_TEMP_FAULTED))
    {
        dash_can_ford_invalidate(s, ch);
        return;
    }
    const float deg_c = (float) raw - 40.0f;
    dash_ch_set(s, ch, deg_c * 9.0f / 5.0f + 32.0f);
}

/* Decode one raw frame into DashState.
 *
 * `id` is the 11-bit standard identifier VALUE (see the header comment:
 * extended-frame filtering is the caller's job); `bytes` is the payload,
 * of which the dialect reads exactly 8 bytes. Returns true only when the
 * frame was CONSUMED: one of the four dialect IDs with dlc >= 8. Sentinel
 * payloads count as consumed -- a degraded sensor is still a live bus, and
 * a known frame always refreshes last-seen. Unknown IDs and runt frames
 * (dlc < 8) return false without touching state -- neither the freshness
 * bookkeeping nor DashState moves -- so a caller keying an accept counter
 * on the return measures decode health, not filter traffic (KTD11).
 *
 * Bit extraction below follows Ford's numbering (bit 0 = MSB of byte 0);
 * each signal cites its official bit span. Signals the table carries but
 * the dash has no channel for (ENGINE_SPEED_HZ, MAN_VAC, DI_PRESSURE,
 * BOOST, TOT, SHIFTER, CODES, GEAR) are deliberately not decoded (R6). */
static inline bool dash_can_ford_decode(DashCanFord *cf, uint32_t id,
                                        uint8_t dlc, const uint8_t bytes[8],
                                        uint32_t now_ms, DashState *s)
{
    if (dlc < 8u) { return false; } /* runt: not this dialect's frame */

    switch (id)
    {
        case DASH_CAN_FORD_ID_ENGINE:
        {
            cf->last_seen_ms[DASH_CAN_FORD_MSG_ENGINE] = now_ms;
            cf->seen[DASH_CAN_FORD_MSG_ENGINE] = true;
            /* ENGINE_SPEED, bits 2-15 (14-bit, 1 rpm/bit, 0-16383). */
            const uint16_t rpm = (uint16_t) (((bytes[0] & 0x3Fu) << 8) | bytes[1]);
            dash_can_ford_write_(s, DASH_CH_RPM, (float) rpm);
            break;
        }

        case DASH_CAN_FORD_ID_AF:
        {
            cf->last_seen_ms[DASH_CAN_FORD_MSG_AF] = now_ms;
            cf->seen[DASH_CAN_FORD_MSG_AF] = true;
            /* AF0, bits 25-35 (11-bit); A/F = raw * 0.01 + 7 -- already
             * gasoline AFR, passed through (KTD2 assumption). */
            const uint16_t af0 = (uint16_t) (((bytes[3] & 0x7Fu) << 4) | (bytes[4] >> 4));
            dash_can_ford_write_(s, DASH_CH_AFR_L, (float) af0 * 0.01f + 7.0f);
            /* AF1, bits 36-46 (11-bit), same encoding. */
            const uint16_t af1 = (uint16_t) (((bytes[4] & 0x0Fu) << 7) | (bytes[5] >> 1));
            dash_can_ford_write_(s, DASH_CH_AFR_R, (float) af1 * 0.01f + 7.0f);
            /* FUEL_PRESSURE, bits 47-55 (9-bit, kPa) -> psi. */
            const uint16_t fp_kpa = (uint16_t) (((bytes[5] & 0x01u) << 8) | bytes[6]);
            dash_can_ford_write_(s, DASH_CH_FUELP,
                                 (float) fp_kpa * DASH_CAN_FORD_KPA_TO_PSI);
            break;
        }

        case DASH_CAN_FORD_ID_VSPD:
        {
            cf->last_seen_ms[DASH_CAN_FORD_MSG_VSPD] = now_ms;
            cf->seen[DASH_CAN_FORD_MSG_VSPD] = true;
            /* VSPD, bits 0-11 (12-bit, 0.1 km/h per bit) -> mph. */
            const uint16_t vspd = (uint16_t) ((bytes[0] << 4) | (bytes[1] >> 4));
            dash_can_ford_write_(s, DASH_CH_SPEED,
                                 ((float) vspd * 0.1f) / DASH_CAN_FORD_KMH_PER_MPH);
            break;
        }

        case DASH_CAN_FORD_ID_TEMPS:
        {
            cf->last_seen_ms[DASH_CAN_FORD_MSG_TEMPS] = now_ms;
            cf->seen[DASH_CAN_FORD_MSG_TEMPS] = true;
            /* ECT byte 0, EOT byte 1: degC = raw - 40 -> degF, with the
             * 214/215 sentinels dead-fronting instead of converting. */
            dash_can_ford_temp_(s, DASH_CH_ECT, bytes[0]);
            dash_can_ford_temp_(s, DASH_CH_OILT, bytes[1]);
            /* EOP, bits 24-33 (10-bit, kPa 0-1023) -> psi. */
            const uint16_t eop_kpa = (uint16_t) ((bytes[3] << 2) | (bytes[4] >> 6));
            dash_can_ford_write_(s, DASH_CH_OILP,
                                 (float) eop_kpa * DASH_CAN_FORD_KPA_TO_PSI);
            /* VBAT, bits 48-58 (11-bit, 0.01 V/bit). */
            const uint16_t vbat = (uint16_t) ((bytes[6] << 3) | (bytes[7] >> 5));
            dash_can_ford_write_(s, DASH_CH_VOLTS, (float) vbat * 0.01f);
            break;
        }

        default:
            return false; /* unknown id: not ours, no state touched */
    }
    return true; /* one of the four dialect IDs: consumed (sentinels included) */
}

/* Which channels a message index claims. Switch, not a file-scope table, so
 * a TU that includes this header without calling anything stays warning-free
 * under -Wall -Wextra -Werror. */
static inline uint32_t dash_can_ford_claim_(uint8_t msg)
{
    switch (msg)
    {
        case DASH_CAN_FORD_MSG_ENGINE: return DASH_CAN_FORD_CLAIM_ENGINE;
        case DASH_CAN_FORD_MSG_AF:     return DASH_CAN_FORD_CLAIM_AF;
        case DASH_CAN_FORD_MSG_VSPD:   return DASH_CAN_FORD_CLAIM_VSPD;
        case DASH_CAN_FORD_MSG_TEMPS:  return DASH_CAN_FORD_CLAIM_TEMPS;
        default: return 0U;
    }
}

/* Expiry pass (KTD6) -- run every loop, after the FIFO drain and before
 * dash_sim_step. Idempotent and cheap when nothing changed: four iterations
 * of mask arithmetic.
 *
 * For each message stale past DASH_CAN_FORD_STALE_MS:
 *  - bench (car_semantics == false): release its can_owned bits, so the sim
 *    reclaims the channels on its next step (R8). `seen` drops with it --
 *    the message is back to never-arrived until a fresh frame reclaims.
 *  - car (car_semantics == true): can_owned stays LATCHED and the claimed
 *    channels dead-front through the decoder-owned invalidate (R7). The sim
 *    stays locked out; only a fresh frame carrying the channel revalidates.
 *    `seen` deliberately stays true so the pass keeps re-asserting the
 *    dead-front -- e.g. `sim on` wipes the serial masks a stale `set` left
 *    behind, and the next pass must put the channel back to `--` rather
 *    than let the stamped value stand as live data.
 *
 * Car-side invalidation targets claim & can_owned, never the whole claim
 * set: a channel serial-owned for the message's entire life was never
 * CAN-claimed, and dead-fronting it would fight the sim for a channel this
 * dialect does not hold. */
static inline void dash_can_ford_expire(DashCanFord *cf, uint32_t now_ms,
                                        bool car_semantics, DashState *s)
{
    for (uint8_t m = 0u; m < (uint8_t) DASH_CAN_FORD_MSG_COUNT; m++)
    {
        if (!cf->seen[m]) { continue; }
        if ((uint32_t) (now_ms - cf->last_seen_ms[m]) <= DASH_CAN_FORD_STALE_MS)
        {
            continue; /* fresh */
        }
        const uint32_t claim = dash_can_ford_claim_(m);
        if (car_semantics)
        {
            const uint32_t held = claim & s->can_owned;
            for (uint8_t ch = 0u; ch < (uint8_t) DASH_CH_COUNT; ch++)
            {
                if ((held & DASH_CH_BIT(ch)) != 0U)
                {
                    dash_can_ford_invalidate(s, ch);
                }
            }
        }
        else
        {
            s->can_owned = (uint32_t) (s->can_owned & ~claim);
            cf->seen[m] = false;
        }
    }
}

#endif /* DASH_CAN_FORD_H */
