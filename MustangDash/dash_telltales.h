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

/* lamp bit positions, LSB first; the .ino maps bit -> physical pin */
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

static inline uint8_t dash_telltale_mask(const DashState *s)
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

#endif /* DASH_TELLTALES_H */
