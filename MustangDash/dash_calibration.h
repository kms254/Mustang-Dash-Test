/*
 * dash_calibration.h - per-position telltale brightness calibration
 * (I2C revision plan 2026-07-28-001, U21; supersedes plan 003's U12
 * resistor derivation, which never reached copper).
 *
 * The eight telltale positions carry six LED colours whose luminous
 * intensities span 210..2390 mcd at the same 20 mA test current (plan 003
 * KTD13, one vendor series so the figures are comparable).  With the
 * AW9523B's 256-step per-channel current DAC the matching problem becomes
 * data: each position gets an 8-bit dim code, seeded so every position
 * emits the dimmest part's intensity -- the dimmest (yellow, 210 mcd) pegs
 * at full code and everything brighter is scaled down by its mcd ratio.
 * Bench trim then moves individual positions without re-deriving anything.
 *
 * Host-testable (tests/test_dash_calibration.c): stdint only.  The .ino
 * owns the AW9523B register I/O and the ISEL current range; this header
 * owns only the code values.
 *
 * Position index = TT number - 1 (TT1..TT8), the same order the lamp-mask
 * bits drive.  Colour per position is KTD13's row as landed by U11:
 *   TT1 LED1 green 1300 | TT2 LED3 white 2390 (MIN flux bin per plan 003 --
 *   matching is worst-case) | TT3 LED4 blue 350 | TT4 LED2 green 1300 |
 *   TT5 LED5 orange 1800 (loose figure, different series) | TT6 LED6 red
 *   270 | TT7 LED7 red 270 | TT8 LED8 yellow 210
 */

#ifndef DASH_CALIBRATION_H
#define DASH_CALIBRATION_H

#include <stdint.h>

#define DASH_CAL_POSITIONS 8U

/* mcd at the shared 20 mA test current (KTD13 row; white = min bin) */
static const uint16_t DASH_CAL_MCD[DASH_CAL_POSITIONS] = {
    1300U, 2390U, 350U, 1300U, 1800U, 270U, 270U, 210U
};

/* bench trim: signed code offset added per position after normalisation.
 * All zero until the row is trimmed on glass; the trim is the authority
 * once measured, the seed is only the starting point. */
static const int16_t DASH_CAL_TRIM[DASH_CAL_POSITIONS] = {
    0, 0, 0, 0, 0, 0, 0, 0
};

/* Fill out[] with the 8-bit dim codes: dimmest position -> 255 before
 * trim, everything brighter scaled by (dimmest mcd / its mcd), rounded to
 * nearest.  trim may be NULL (no trim); the trimmed result clamps to
 * 0..255 rather than wrapping. */
static inline void dash_cal_codes_with_trim(const int16_t *trim,
                                            uint8_t out[DASH_CAL_POSITIONS])
{
    uint16_t dimmest = 0xFFFFU;
    for (uint8_t i = 0U; i < DASH_CAL_POSITIONS; i++)
    {
        if (DASH_CAL_MCD[i] < dimmest)
        {
            dimmest = DASH_CAL_MCD[i];
        }
    }
    for (uint8_t i = 0U; i < DASH_CAL_POSITIONS; i++)
    {
        const uint32_t mcd = DASH_CAL_MCD[i];
        int32_t code = (int32_t)((255UL * dimmest + (mcd / 2U)) / mcd);
        if (trim != (const int16_t *)0)
        {
            code += trim[i];
        }
        out[i] = (code < 0) ? 0U : ((code > 255) ? 255U : (uint8_t)code);
    }
}

static inline void dash_cal_codes(uint8_t out[DASH_CAL_POSITIONS])
{
    dash_cal_codes_with_trim(DASH_CAL_TRIM, out);
}

#endif /* DASH_CALIBRATION_H */
