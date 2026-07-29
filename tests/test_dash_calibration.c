/*
 * Invariant test: the telltale brightness calibration table
 * (dash_calibration.h) turns KTD13's measured mcd row into per-position
 * 8-bit dim codes for the AW9523B current DAC (I2C revision plan
 * 2026-07-28-001, U21 -- supersedes plan 003's U12 resistor derivation).
 *
 * Runs on the host:
 *   gcc -std=c11 -I MustangDash tests/test_dash_calibration.c -o /tmp/cal && /tmp/cal
 */

#include <stdio.h>
#include <string.h>

#include "dash_calibration.h"

static int failures = 0;

static void expect(int cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

int main(void)
{
    uint8_t codes[DASH_CAL_POSITIONS];

    /* 1. seed row is KTD13's, in TT order, white on the MIN flux bin.
     * Position index = TT number - 1 (the lamp-mask bit order):
     * TT1 LED1 green, TT2 LED3 white(min 2390), TT3 LED4 blue, TT4 LED2
     * green, TT5 LED5 orange, TT6 LED6 red, TT7 LED7 red, TT8 LED8 yellow */
    {
        expect(DASH_CAL_MCD[0] == 1300U, "TT1 green seeds 1300 mcd");
        expect(DASH_CAL_MCD[1] == 2390U, "TT2 white seeds the MIN bin 2390, not the 2700 typ");
        expect(DASH_CAL_MCD[2] == 350U,  "TT3 blue seeds 350 mcd");
        expect(DASH_CAL_MCD[3] == 1300U, "TT4 green seeds 1300 mcd");
        expect(DASH_CAL_MCD[4] == 1800U, "TT5 orange seeds 1800 mcd");
        expect(DASH_CAL_MCD[5] == 270U,  "TT6 red seeds 270 mcd");
        expect(DASH_CAL_MCD[6] == 270U,  "TT7 red seeds 270 mcd");
        expect(DASH_CAL_MCD[7] == 210U,  "TT8 yellow seeds 210 mcd");
    }

    /* 2. dimmest position pegs at maximum */
    {
        dash_cal_codes(codes);
        expect(codes[7] == 255U, "dimmest position (yellow) pegs at code 255");
    }

    /* 3. monotone: brighter LED -> lower-or-equal code, strictly lower for
     * strictly brighter parts */
    {
        dash_cal_codes(codes);
        for (uint8_t i = 0U; i < DASH_CAL_POSITIONS; i++)
        {
            for (uint8_t j = 0U; j < DASH_CAL_POSITIONS; j++)
            {
                if (DASH_CAL_MCD[i] > DASH_CAL_MCD[j])
                {
                    expect(codes[i] < codes[j],
                           "a brighter part must get a strictly lower code");
                }
            }
        }
    }

    /* 4. equal parts get equal codes (the two greens, the two reds) */
    {
        dash_cal_codes(codes);
        expect(codes[0] == codes[3], "green pair (TT1/TT4) matches");
        expect(codes[5] == codes[6], "red pair (TT6/TT7) matches");
    }

    /* 5. the seeded ratios are the mcd ratios, rounded to nearest */
    {
        dash_cal_codes(codes);
        for (uint8_t i = 0U; i < DASH_CAL_POSITIONS; i++)
        {
            const uint32_t mcd = DASH_CAL_MCD[i];
            const uint8_t want = (uint8_t)((255UL * 210U + mcd / 2U) / mcd);
            expect(codes[i] == want, "code equals round(255 * dimmest/mcd)");
        }
        expect(codes[0] == 41U, "green lands at 41 of 255");
        expect(codes[1] == 22U, "white lands at 22 of 255");
    }

    /* 6. a per-position trim override survives normalisation */
    {
        int16_t trim[DASH_CAL_POSITIONS] = { 0 };
        uint8_t trimmed[DASH_CAL_POSITIONS];
        trim[2] = 25;
        trim[5] = -30;
        dash_cal_codes_with_trim(trim, trimmed);
        dash_cal_codes(codes);
        expect(trimmed[2] == (uint8_t)(codes[2] + 25),
               "positive trim adds to the seeded code");
        expect(trimmed[5] == (uint8_t)(codes[5] - 30),
               "negative trim subtracts from the seeded code");
        expect(trimmed[0] == codes[0], "untrimmed positions are untouched");
    }

    /* 7. trim clamps instead of wrapping */
    {
        int16_t trim[DASH_CAL_POSITIONS] = { 0 };
        uint8_t trimmed[DASH_CAL_POSITIONS];
        trim[7] = 100;   /* yellow already pegs 255 */
        trim[1] = -300;  /* white would go negative */
        dash_cal_codes_with_trim(trim, trimmed);
        expect(trimmed[7] == 255U, "trim above the ceiling clamps at 255");
        expect(trimmed[1] == 0U, "trim below zero clamps at 0 (lamp off)");
    }

    /* 8. NULL trim means no trim */
    {
        uint8_t a[DASH_CAL_POSITIONS], b[DASH_CAL_POSITIONS];
        dash_cal_codes_with_trim((const int16_t *)0, a);
        dash_cal_codes(b);
        expect(0 == memcmp(a, b, sizeof a),
               "NULL trim and the default trim table agree while the table is all-zero");
    }

    if (failures)
    {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("dash_calibration: all invariants hold\n");
    return 0;
}
