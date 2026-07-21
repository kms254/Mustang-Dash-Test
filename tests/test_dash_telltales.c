/*
 * Invariant test: the 8-lamp telltale mask (dash_telltales.h) mirrors the
 * dash's alarm/threshold logic -- same constants, same validity gating,
 * same engine-running rule -- so the physical lamps on the STM32 carrier
 * can never disagree with what the screens show (migration plan U6).
 *
 * Runs on the host:
 *   gcc -std=c11 -I MustangDash tests/test_dash_telltales.c -lm -o /tmp/tt && /tmp/tt
 */

#include <stdio.h>
#include <string.h>

#include "dash_telltales.h"

static int failures = 0;

static void expect(int cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

static DashState fresh(void)
{
    DashState s;
    memset(&s, 0, sizeof s); /* all channels invalid */
    return s;
}

int main(void)
{
    /* 1. all channels invalid -> no lamp can light (dead sender rule) */
    {
        DashState s = fresh();
        expect(dash_telltale_mask(&s) == 0U,
               "no valid channel may light any lamp");
    }

    /* 2. oil pressure obeys the engine-running gate exactly like the alarm */
    {
        DashState s = fresh();
        dash_ch_set(&s, DASH_CH_OILP, DASH_OILP_RED_PSI - 1.0f);
        expect(dash_telltale_mask(&s) == 0U,
               "low oil pressure with no rpm channel must not light OILP");
        dash_ch_set(&s, DASH_CH_RPM, DASH_ENGINE_RUNNING_RPM - 1.0f);
        expect((dash_telltale_mask(&s) & (1U << DASH_LAMP_OILP)) == 0U,
               "low oil pressure below running rpm must not light OILP");
        dash_ch_set(&s, DASH_CH_RPM, DASH_ENGINE_RUNNING_RPM);
        expect((dash_telltale_mask(&s) & (1U << DASH_LAMP_OILP)) != 0U,
               "low oil pressure at running rpm must light OILP");
        dash_ch_set(&s, DASH_CH_OILP, DASH_OILP_RED_PSI + 1.0f);
        expect((dash_telltale_mask(&s) & (1U << DASH_LAMP_OILP)) == 0U,
               "healthy oil pressure must clear OILP");
    }

    /* 3. each threshold lamp lights from its own constant */
    {
        DashState s = fresh();
        dash_ch_set(&s, DASH_CH_OILT, DASH_OILT_RED_F + 1.0f);
        dash_ch_set(&s, DASH_CH_ECT, DASH_ECT_RED_F + 1.0f);
        dash_ch_set(&s, DASH_CH_VOLTS, DASH_VOLTS_RED_V - 0.5f);
        dash_ch_set(&s, DASH_CH_FUEL, DASH_FUEL_AMBER_GAL - 0.5f);
        const uint8_t m = dash_telltale_mask(&s);
        expect((m & (1U << DASH_LAMP_OILT)) != 0U, "hot oil must light OILT");
        expect((m & (1U << DASH_LAMP_CLT)) != 0U, "hot coolant must light CLT");
        expect((m & (1U << DASH_LAMP_VOLTS)) != 0U, "low volts must light VOLTS");
        expect((m & (1U << DASH_LAMP_FUEL)) != 0U, "low fuel must light FUEL");
        expect((m & (1U << DASH_LAMP_OILP)) == 0U,
               "OILP stays dark: channel invalid and engine not running");
    }

    /* 4. fuel pressure is engine-gated (pump prime at key-on is not a fault) */
    {
        DashState s = fresh();
        dash_ch_set(&s, DASH_CH_FUELP, DASH_FUELP_RED_PSI - 5.0f);
        expect(dash_telltale_mask(&s) == 0U,
               "low fuel pressure with engine off must not light FUELP");
        dash_ch_set(&s, DASH_CH_RPM, DASH_ENGINE_RUNNING_RPM);
        expect((dash_telltale_mask(&s) & (1U << DASH_LAMP_FUELP)) != 0U,
               "low fuel pressure with engine running must light FUELP");
    }

    /* 5. AFR lean lights from either bank independently */
    {
        DashState s = fresh();
        dash_ch_set(&s, DASH_CH_AFR_L, DASH_AFR_AMBER + 0.3f);
        expect((dash_telltale_mask(&s) & (1U << DASH_LAMP_AFR)) != 0U,
               "lean left bank must light AFR");
        DashState s2 = fresh();
        dash_ch_set(&s2, DASH_CH_AFR_R, DASH_AFR_AMBER + 0.3f);
        expect((dash_telltale_mask(&s2) & (1U << DASH_LAMP_AFR)) != 0U,
               "lean right bank must light AFR");
    }

    /* 6. shift lamp follows the flash zone */
    {
        DashState s = fresh();
        dash_ch_set(&s, DASH_CH_RPM, 8000.0f); /* well into the flash zone */
        expect((dash_telltale_mask(&s) & (1U << DASH_LAMP_SHIFT)) != 0U,
               "flash-zone rpm must light SHIFT");
        dash_ch_set(&s, DASH_CH_RPM, 2000.0f);
        expect((dash_telltale_mask(&s) & (1U << DASH_LAMP_SHIFT)) == 0U,
               "cruise rpm must clear SHIFT");
    }

    /* 7. the lamp-test mask covers every lamp */
    expect(DASH_TELLTALE_ALL == 0xFFU && DASH_LAMP_COUNT == 8,
           "lamp test must exercise all 8 lamps");

    if (failures == 0)
    {
        printf("OK: telltale mask mirrors alarm/threshold logic with validity "
               "and engine-running gating\n");
        return 0;
    }
    return 1;
}
