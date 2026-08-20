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
#include "dash_ttsweep.h"
#include "dash_turnsignal.h"

/* silk TT number -> bit position (TT1 = bit 0), so the assertions below
 * read in the same numbering as the board and the legend doc */
#define TT(n) ((uint8_t) ((n) - 1U))

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
        expect(dash_telltale_conditions(&s) == 0U,
               "no valid channel may light any lamp");
    }

    /* 2. oil pressure obeys the engine-running gate exactly like the alarm */
    {
        DashState s = fresh();
        dash_ch_set(&s, DASH_CH_OILP, DASH_OILP_RED_PSI - 1.0f);
        expect(dash_telltale_conditions(&s) == 0U,
               "low oil pressure with no rpm channel must not light OILP");
        dash_ch_set(&s, DASH_CH_RPM, DASH_ENGINE_RUNNING_RPM - 1.0f);
        expect((dash_telltale_conditions(&s) & (1U << DASH_LAMP_OILP)) == 0U,
               "low oil pressure below running rpm must not light OILP");
        dash_ch_set(&s, DASH_CH_RPM, DASH_ENGINE_RUNNING_RPM);
        expect((dash_telltale_conditions(&s) & (1U << DASH_LAMP_OILP)) != 0U,
               "low oil pressure at running rpm must light OILP");
        dash_ch_set(&s, DASH_CH_OILP, DASH_OILP_RED_PSI + 1.0f);
        expect((dash_telltale_conditions(&s) & (1U << DASH_LAMP_OILP)) == 0U,
               "healthy oil pressure must clear OILP");
    }

    /* 3. each threshold lamp lights from its own constant */
    {
        DashState s = fresh();
        dash_ch_set(&s, DASH_CH_OILT, DASH_OILT_RED_F + 1.0f);
        dash_ch_set(&s, DASH_CH_ECT, DASH_ECT_RED_F + 1.0f);
        dash_ch_set(&s, DASH_CH_VOLTS, DASH_VOLTS_RED_V - 0.5f);
        dash_ch_set(&s, DASH_CH_FUEL, DASH_FUEL_AMBER_GAL - 0.5f);
        const uint8_t m = dash_telltale_conditions(&s);
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
        expect(dash_telltale_conditions(&s) == 0U,
               "low fuel pressure with engine off must not light FUELP");
        dash_ch_set(&s, DASH_CH_RPM, DASH_ENGINE_RUNNING_RPM);
        expect((dash_telltale_conditions(&s) & (1U << DASH_LAMP_FUELP)) != 0U,
               "low fuel pressure with engine running must light FUELP");
    }

    /* 5. AFR lean lights from either bank independently */
    {
        DashState s = fresh();
        dash_ch_set(&s, DASH_CH_AFR_L, DASH_AFR_AMBER + 0.3f);
        expect((dash_telltale_conditions(&s) & (1U << DASH_LAMP_AFR)) != 0U,
               "lean left bank must light AFR");
        DashState s2 = fresh();
        dash_ch_set(&s2, DASH_CH_AFR_R, DASH_AFR_AMBER + 0.3f);
        expect((dash_telltale_conditions(&s2) & (1U << DASH_LAMP_AFR)) != 0U,
               "lean right bank must light AFR");
    }

    /* 6. shift lamp follows the flash zone */
    {
        DashState s = fresh();
        dash_ch_set(&s, DASH_CH_RPM, 8000.0f); /* well into the flash zone */
        expect((dash_telltale_conditions(&s) & (1U << DASH_LAMP_SHIFT)) != 0U,
               "flash-zone rpm must light SHIFT");
        dash_ch_set(&s, DASH_CH_RPM, 2000.0f);
        expect((dash_telltale_conditions(&s) & (1U << DASH_LAMP_SHIFT)) == 0U,
               "cruise rpm must clear SHIFT");
    }

    /* 7. the lamp-test mask covers every lamp */
    expect(DASH_TELLTALE_ALL == 0xFFU && DASH_LAMP_COUNT == 8,
           "lamp test must exercise all 8 lamps");

    /* 7b. condition -> physical position map (the cluster legend in code,
     * verified against Board3's LEDs by eye 2026-08-19). Positions are
     * silk TT1..TT8 = bit 0..7. */
    {
        /* the two conditions that own a lamp */
        expect(DASH_LAMP_TT[DASH_LAMP_OILP] == TT(6),
               "oil pressure lights TT6, the right cluster's red");
        expect(DASH_LAMP_TT[DASH_LAMP_FUEL] == TT(5),
               "low fuel lights TT5, the left cluster's orange");

        /* every other condition is screens-only: the remaining positions
         * carry CAN body/PCM signals (blinkers, headlights, high beam,
         * parking brake, CEL) that the firmware does not compute */
        expect(DASH_LAMP_TT[DASH_LAMP_OILT] == DASH_TT_NONE &&
                   DASH_LAMP_TT[DASH_LAMP_CLT] == DASH_TT_NONE &&
                   DASH_LAMP_TT[DASH_LAMP_VOLTS] == DASH_TT_NONE &&
                   DASH_LAMP_TT[DASH_LAMP_FUELP] == DASH_TT_NONE &&
                   DASH_LAMP_TT[DASH_LAMP_AFR] == DASH_TT_NONE &&
                   DASH_LAMP_TT[DASH_LAMP_SHIFT] == DASH_TT_NONE,
               "the other six conditions render on the screens, not a lamp");

        /* no two conditions may share a position, and every mapped
         * position must be a real lamp -- a legend edit that collides or
         * runs off the end is the failure this pins */
        uint8_t seen = 0U;
        for (uint8_t l = 0U; l < (uint8_t) DASH_LAMP_COUNT; l++)
        {
            const uint8_t tt = DASH_LAMP_TT[l];
            if (tt == DASH_TT_NONE) { continue; }
            expect(tt < 8U, "a mapped position must be TT1..TT8");
            expect(((seen >> tt) & 1U) == 0U,
                   "two conditions must not drive one lamp");
            seen |= (uint8_t) (1U << tt);
        }
    }

    /* 8. physical mask: conditions translate, forces OR in, and neither
     *    can suppress the other */
    {
        DashState s = fresh();

        /* a warning with no lamp stays off the board entirely */
        dash_ch_set(&s, DASH_CH_VOLTS, DASH_VOLTS_RED_V - 0.5f);
        expect((dash_telltale_conditions(&s) & (1U << DASH_LAMP_VOLTS)) != 0U &&
                   dash_telltale_mask(&s) == 0U,
               "a screens-only warning lights no lamp");

        /* force is a physical position, independent of any condition */
        s.tt_forced = (uint8_t) (1U << TT(3));
        expect(dash_telltale_mask(&s) == (1U << TT(3)),
               "a forced lamp lights with no matching condition");
        s.tt_forced = 0U;

        /* oil pressure lands on TT6, not on its condition index (TT1) --
         * the bug this remap fixes: the bench saw the oil alarm light the
         * green left-blinker lamp */
        dash_ch_set(&s, DASH_CH_RPM, DASH_ENGINE_RUNNING_RPM + 500.0f);
        dash_ch_set(&s, DASH_CH_OILP, DASH_OILP_RED_PSI - 1.0f);
        expect((dash_telltale_mask(&s) & (1U << TT(6))) != 0U,
               "low oil pressure lights TT6");
        expect((dash_telltale_mask(&s) & (1U << TT(1))) == 0U,
               "low oil pressure must NOT light TT1, the left blinker");

        /* releasing an unrelated force leaves the live lamp alone */
        s.tt_forced = (uint8_t) (1U << TT(2));
        expect((dash_telltale_mask(&s) & (1U << TT(6))) != 0U,
               "an unrelated force must not disturb a live lamp");
        s.tt_forced = 0U;
        expect((dash_telltale_mask(&s) & (1U << TT(6))) != 0U,
               "clearing forces must not touch a naturally-lit lamp");

        /* force + condition on ONE lamp: releasing the force cannot blank
         * it, because the overlay ORs rather than masks */
        s.tt_forced = (uint8_t) (1U << TT(6));
        expect((dash_telltale_mask(&s) & (1U << TT(6))) != 0U,
               "force + warning on one lamp is still lit");
        s.tt_forced = 0U;
        expect((dash_telltale_mask(&s) & (1U << TT(6))) != 0U,
               "releasing the force cannot blank a live warning lamp");
    }

    /* 8b. the CAN body/PCM seam: physical positions, joined not translated,
     *     and independent of both the conditions and the bench forces */
    {
        DashState s = fresh();

        /* an unwired bench leaves every CAN position dark */
        expect(dash_telltale_mask(&s) == 0U,
               "no producer means no body lamp lights");

        /* body signals are already positions -- TT1 is the left blinker,
         * NOT condition bit 0 (oil pressure) */
        s.tt_signals = (uint8_t) (1U << TT(1));
        expect(dash_telltale_mask(&s) == (1U << TT(1)),
               "a CAN body signal lights its own position directly");
        expect(dash_telltale_conditions(&s) == 0U,
               "a body signal must not appear as a warning condition");

        /* conditions and body signals coexist without contesting: the
         * legend gives them disjoint positions */
        dash_ch_set(&s, DASH_CH_RPM, DASH_ENGINE_RUNNING_RPM + 500.0f);
        dash_ch_set(&s, DASH_CH_OILP, DASH_OILP_RED_PSI - 1.0f);
        expect(dash_telltale_mask(&s) ==
                   (uint8_t) ((1U << TT(1)) | (1U << TT(6))),
               "blinker and oil-pressure lamps light together, untangled");

        /* a released body signal cannot blank a live warning, and a
         * released force cannot blank a live body signal */
        s.tt_signals = 0U;
        expect(dash_telltale_mask(&s) == (1U << TT(6)),
               "dropping a body signal leaves the warning lamp alone");
        s.tt_signals = (uint8_t) (1U << TT(3));
        s.tt_forced = (uint8_t) (1U << TT(3));
        s.tt_forced = 0U;
        expect((dash_telltale_mask(&s) & (1U << TT(3))) != 0U,
               "releasing a force cannot blank a live body signal");
    }

    /* 9. the sweep timeline (dash_ttsweep.h): geometry table + bulb check */
    {
        /* columns are pairwise disjoint and OR-complete: every lamp swept
         * exactly once per pass, none forgotten by a re-layout */
        uint8_t all = 0U;
        int popcount = 0;
        for (uint8_t c = 0U; c < DASH_TTSWEEP_COLS; c++)
        {
            for (uint8_t b = 0U; b < 8U; b++)
            {
                if ((DASH_TTSWEEP_COL[c] >> b) & 1U)
                {
                    expect(((all >> b) & 1U) == 0U,
                           "sweep columns must not share a lamp");
                    popcount++;
                }
            }
            all |= DASH_TTSWEEP_COL[c];
        }
        expect(all == 0xFFU && popcount == 8,
               "sweep columns must cover all 8 lamps exactly once");

        /* timeline shape: L->R, bounce, hold, done */
        expect(dash_ttsweep_mask(0U) == DASH_TTSWEEP_COL[0],
               "sweep starts at the west column");
        expect(dash_ttsweep_mask(3U * DASH_TTSWEEP_STEP_MS) == DASH_TTSWEEP_COL[3],
               "step 3 is the east column");
        expect(dash_ttsweep_mask(4U * DASH_TTSWEEP_STEP_MS) == DASH_TTSWEEP_COL[2],
               "the bounce comes back without repeating the far column");
        expect(dash_ttsweep_mask(6U * DASH_TTSWEEP_STEP_MS) == DASH_TTSWEEP_COL[0],
               "the bounce ends back at the west column");
        expect(dash_ttsweep_mask(7U * DASH_TTSWEEP_STEP_MS) == 0xFFU,
               "after the chase, the full-row hold (the bulb check)");
        expect(dash_ttsweep_mask(DASH_TTSWEEP_TOTAL_MS - 1U) == 0xFFU,
               "the hold lasts to the final millisecond");
        expect(dash_ttsweep_done(DASH_TTSWEEP_TOTAL_MS) &&
                   dash_ttsweep_mask(DASH_TTSWEEP_TOTAL_MS) == 0U,
               "at TOTAL the sweep is done and asserts nothing");
        expect(!dash_ttsweep_done(0U),
               "the sweep is not born finished");
    }

    /* 10. turn-signal stalk + flasher (dash_turnsignal.h) */
    {
        DashTurn t;
        dash_turn_init(&t);

        /* idle: nothing lit, ever */
        expect(dash_turn_mask(&t, 0U) == 0U && dash_turn_mask(&t, 12345U) == 0U,
               "an idle stalk lights no blinker at any time");

        /* a press starts the flasher LIT -- a tap must always be visible */
        dash_turn_press(&t, DASH_TURN_LEFT, 1000U);
        expect(dash_turn_mask(&t, 1000U) == (1U << DASH_TT_LEFT_BLINKER),
               "left press lights TT1 immediately");
        expect(dash_turn_mask(&t, 1000U + DASH_TURN_ON_MS - 1U) != 0U,
               "lit through the end of the on-phase");
        expect(dash_turn_mask(&t, 1000U + DASH_TURN_ON_MS) == 0U,
               "dark at the start of the off-phase");
        expect(dash_turn_mask(&t, 1000U + DASH_TURN_PERIOD_MS) ==
                   (1U << DASH_TT_LEFT_BLINKER),
               "lit again one full period later");

        /* left never lights the right lamp */
        expect((dash_turn_mask(&t, 1000U) & (1U << DASH_TT_RIGHT_BLINKER)) == 0U,
               "left signalling must not light TT4");

        /* the other side takes over and restarts the phase lit */
        dash_turn_press(&t, DASH_TURN_RIGHT, 1500U);
        expect(t.side == DASH_TURN_RIGHT,
               "pressing the other side takes over");
        expect(dash_turn_mask(&t, 1500U) == (1U << DASH_TT_RIGHT_BLINKER),
               "right press lights TT4 immediately, phase restarted");
        expect((dash_turn_mask(&t, 1500U) & (1U << DASH_TT_LEFT_BLINKER)) == 0U,
               "taking over cancels the left lamp -- never both at once");

        /* same side again cancels */
        dash_turn_press(&t, DASH_TURN_RIGHT, 2000U);
        expect(t.side == DASH_TURN_NONE && dash_turn_mask(&t, 2000U) == 0U,
               "pressing the active side cancels it");

        /* the module owns exactly two bits, and they are the blinkers */
        expect(DASH_TURN_LAMP_BITS ==
                   ((1U << DASH_TT_LEFT_BLINKER) | (1U << DASH_TT_RIGHT_BLINKER)),
               "the clear-mask is exactly the two blinker positions");
        dash_turn_press(&t, DASH_TURN_LEFT, 3000U);
        for (uint32_t dt = 0U; dt < 3U * DASH_TURN_PERIOD_MS; dt += 17U)
        {
            expect((dash_turn_mask(&t, 3000U + dt) & ~DASH_TURN_LAMP_BITS) == 0U,
                   "the flasher may never set a bit outside its own two");
        }

        /* flash rate stays inside the FMVSS 108 / SAE J590 60-120 per minute
         * band -- the constants are the spec claim, so pin them */
        expect(DASH_TURN_PERIOD_MS >= 500U && DASH_TURN_PERIOD_MS <= 1000U,
               "flash period must be 60-120 flashes per minute");
        expect(DASH_TURN_ON_MS > 0U && DASH_TURN_ON_MS < DASH_TURN_PERIOD_MS,
               "duty cycle must be a real fraction of the period");
    }

    if (failures == 0)
    {
        printf("OK: telltale mask mirrors alarm/threshold logic with validity "
               "and engine-running gating\n");
        return 0;
    }
    return 1;
}
