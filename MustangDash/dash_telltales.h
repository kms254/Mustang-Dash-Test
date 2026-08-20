/*
 * dash_telltales.h - pure mapping from DashState to the 8-lamp warning
 * telltale mask (STM32 migration plan U6). The lamps are a hardware mirror
 * of the alarm/threshold logic the screens already render: same constants,
 * same validity gating, same engine-running rule for oil pressure.
 *
 * Host-testable (tests/test_dash_telltales.c): stdint + dash_data.h +
 * dash_math.h only. The .ino owns pin mapping, group dimming, and the boot
 * lamp test; this header owns only WHICH lamps are lit.
 *
 * Rules mirrored from dash_math.h, not re-derived:
 *   - a lamp lights only from a VALID channel -- a dead sender can neither
 *     light nor hide a warning at this layer; the boot lamp test is the
 *     bulb-check safety net for dead LEDs
 *   - oil and fuel pressure are gated on the engine running
 *     (rpm >= DASH_ENGINE_RUNNING_RPM), exactly like the full-screen alarm
 */

#ifndef DASH_TELLTALES_H
#define DASH_TELLTALES_H

#include <stdint.h>
#include "dash_data.h"
#include "dash_math.h"

/* WARNING CONDITIONS the firmware computes. These are NOT lamp positions --
 * see DASH_LAMP_TT below for where (if anywhere) each one lights. */
typedef enum {
    DASH_LAMP_OILP = 0, /* oil pressure red: < DASH_OILP_RED_PSI, engine running */
    DASH_LAMP_OILT,     /* oil temp red:    > DASH_OILT_RED_F */
    DASH_LAMP_CLT,      /* coolant red:     > DASH_ECT_RED_F */
    DASH_LAMP_VOLTS,    /* battery red:     < DASH_VOLTS_RED_V */
    DASH_LAMP_FUELP,    /* fuel press red:  < DASH_FUELP_RED_PSI, engine running */
    DASH_LAMP_FUEL,     /* fuel low amber:  < DASH_FUEL_AMBER_GAL */
    DASH_LAMP_AFR,      /* lean amber:      either bank > DASH_AFR_AMBER */
    DASH_LAMP_SHIFT,    /* shift: rpm in the flash zone */
    DASH_LAMP_COUNT
} DashLamp;

#define DASH_TELLTALE_ALL ((uint8_t) 0xFFU) /* boot lamp-test mask */

/* ---- condition -> physical telltale position -------------------------
 * Physical positions are the board's silk numbering: TT1..TT8 = bit 0..7.
 * The colours were verified by eye on Board3 (2026-08-19, every position
 * forced one at a time) and the assignments are the cluster legend in
 * docs/hardware/board3-telltale-legend.md:
 *
 *   TT1 green  L blinker     TT2 white  headlights    (CAN)
 *   TT3 blue   high beam     TT4 green  R blinker     (CAN)
 *   TT5 orange LOW FUEL      TT6 red    OIL PRESSURE  (firmware)
 *   TT7 red    parking brake TT8 yellow CEL/MIL       (CAN)
 *
 * Only two conditions own a lamp. That is the legend's doing, not an
 * oversight: six positions carry body/PCM signals that arrive over CAN,
 * so the firmware's remaining six warnings live on the screens, where
 * they already render. Coolant is the notable demotion and it is
 * deliberate -- an overheat raises the full-screen alarm takeover, which
 * is louder than a 3.5 mm LED, whereas a parking brake has no other way
 * to be shown.
 *
 * Until this table existed, condition bit l drove TT(l+1) one-to-one --
 * a placeholder nobody could check before the LEDs had hardware. It was
 * wrong in every row: the oil alarm lit TT1 (green) and low fuel lit TT6
 * (red) on the bench the night the board came up. */
#define DASH_TT_NONE 0xFFU /* condition has no lamp; screens only */

/* Physical lamp positions on the board (TT1..TT8). Deliberately NOT
 * DASH_LAMP_COUNT: that counts warning CONDITIONS, and the two are equal
 * only by coincidence today. Hardware tables (expander address, DIM
 * register, calibration code, GPIO pin) are indexed by POSITION and must
 * size from this. */
#define DASH_TT_COUNT 8U

static const uint8_t DASH_LAMP_TT[DASH_LAMP_COUNT] = {
    5U,           /* OILP  -> TT6 red    (right cluster, bottom-left) */
    DASH_TT_NONE, /* OILT  -> screens */
    DASH_TT_NONE, /* CLT   -> screens (alarm takeover covers an overheat) */
    DASH_TT_NONE, /* VOLTS -> screens */
    DASH_TT_NONE, /* FUELP -> screens */
    4U,           /* FUEL  -> TT5 orange (left cluster, bottom-left) */
    DASH_TT_NONE, /* AFR   -> screens */
    DASH_TT_NONE, /* SHIFT -> screens */
};

/* The warning conditions currently true, as condition bits (DashLamp).
 * Pure threshold/validity logic -- no notion of where anything lights. */
static inline uint8_t dash_telltale_conditions(const DashState *s)
{
    uint8_t mask = 0U;

    const bool engine_running = dash_ch_valid(s, DASH_CH_RPM)
                                && (s->ch.rpm >= DASH_ENGINE_RUNNING_RPM);

    if (engine_running && dash_ch_valid(s, DASH_CH_OILP)
        && (s->ch.oil_press_psi < DASH_OILP_RED_PSI))
    {
        mask |= (uint8_t) (1U << DASH_LAMP_OILP);
    }
    if (dash_ch_valid(s, DASH_CH_OILT) && (s->ch.oil_temp_f > DASH_OILT_RED_F))
    {
        mask |= (uint8_t) (1U << DASH_LAMP_OILT);
    }
    if (dash_ch_valid(s, DASH_CH_ECT) && (s->ch.ect_f > DASH_ECT_RED_F))
    {
        mask |= (uint8_t) (1U << DASH_LAMP_CLT);
    }
    if (dash_ch_valid(s, DASH_CH_VOLTS) && (s->ch.volts < DASH_VOLTS_RED_V))
    {
        mask |= (uint8_t) (1U << DASH_LAMP_VOLTS);
    }
    if (engine_running && dash_ch_valid(s, DASH_CH_FUELP)
        && (s->ch.fuel_press_psi < DASH_FUELP_RED_PSI))
    {
        mask |= (uint8_t) (1U << DASH_LAMP_FUELP);
    }
    if (dash_ch_valid(s, DASH_CH_FUEL) && (s->ch.fuel_gal < DASH_FUEL_AMBER_GAL))
    {
        mask |= (uint8_t) (1U << DASH_LAMP_FUEL);
    }
    if ((dash_ch_valid(s, DASH_CH_AFR_L) && (s->ch.afr_l > DASH_AFR_AMBER))
        || (dash_ch_valid(s, DASH_CH_AFR_R) && (s->ch.afr_r > DASH_AFR_AMBER)))
    {
        mask |= (uint8_t) (1U << DASH_LAMP_AFR);
    }
    if (dash_ch_valid(s, DASH_CH_RPM) && dash_shift_flash_zone(s->ch.rpm))
    {
        mask |= (uint8_t) (1U << DASH_LAMP_SHIFT);
    }

    return mask;
}

/* The lamps to light, as PHYSICAL positions (TT1..TT8 = bit 0..7) -- what
 * the caller writes to the expanders. Conditions are translated through
 * DASH_LAMP_TT, then the bench force overlay (serial `tt`) is OR'd in:
 * OR, never mask, so a forced lamp cannot hide a live warning and
 * releasing a force returns the natural state on the next frame. */
static inline uint8_t dash_telltale_mask(const DashState *s)
{
    const uint8_t cond = dash_telltale_conditions(s);
    uint8_t mask = 0U;

    for (uint8_t l = 0U; l < (uint8_t) DASH_LAMP_COUNT; l++)
    {
        if (0U != ((cond >> l) & 1U))
        {
            const uint8_t tt = DASH_LAMP_TT[l];
            if (tt != DASH_TT_NONE)
            {
                mask |= (uint8_t) (1U << tt);
            }
        }
    }

    /* CAN-sourced body/PCM lamps are already physical positions, so they
     * join directly -- no translation, and no condition may contest them:
     * the two sets are disjoint by the legend's construction. */
    mask |= s->tt_signals;

    mask |= s->tt_forced;

    return mask;
}

#endif /* DASH_TELLTALES_H */
