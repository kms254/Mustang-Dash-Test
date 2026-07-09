/*
 * Invariant test: the backlight triangle wave must stay within [BL_MIN, BL_MAX]
 * (20..128 -- REG_PWM_DUTY's useful range tops out at 128 = 100%), must
 * actually reach both bounds, and must have the documented ~2.2 s period
 * (108 steps of 2 at 20 ms per step for a full min->max->min sweep).
 *
 * Runs on the host:
 *   gcc -std=c11 -I MustangDash tests/test_backlight_wave.c -o /tmp/bl && /tmp/bl
 */

#include <stdio.h>
#include <stdlib.h>

#include "backlight_wave.h"

#define BL_MIN 20U
#define BL_MAX 128U

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
    /* mirror the sketch's initial state: full brightness, dimming down */
    uint8_t duty = BL_MAX;
    int8_t step = -2;

    int seen_min = 0;
    int seen_max = 0;
    int iterations_to_return_to_max = -1;

    for (int i = 1; i <= 1000; i++)
    {
        duty = bl_wave_next(duty, &step, BL_MIN, BL_MAX);

        expect(duty >= BL_MIN && duty <= BL_MAX, "duty escaped [BL_MIN, BL_MAX]");
        if (duty == BL_MIN) { seen_min = 1; }
        if (duty == BL_MAX)
        {
            seen_max = 1;
            if (iterations_to_return_to_max < 0) { iterations_to_return_to_max = i; }
        }
    }

    expect(seen_min, "wave never reached BL_MIN (20)");
    expect(seen_max, "wave never returned to BL_MAX (128)");

    /* full sweep: (128-20)/2 = 54 steps down + 54 steps up = 108 iterations,
     * which at 20 ms per loop() pass is the documented ~2.2 s period */
    expect(iterations_to_return_to_max == 108,
           "full min->max->min sweep is not 108 iterations (~2.2 s at 20 ms/step)");

    /* step magnitude must be preserved across bounces */
    expect(step == 2 || step == -2, "step magnitude drifted from 2");

    if (failures == 0)
    {
        printf("OK: backlight wave bounded [20,128], hits both bounds, 108-step period\n");
        return 0;
    }
    return 1;
}
