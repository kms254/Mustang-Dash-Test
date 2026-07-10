/*
 * Invariant test: every gauge/threshold/format computation in dash_math.h
 * and the mock->panel scale system in dash_layout.h must match the dash
 * plan's U3 spec exactly -- speed knee mapping, gauge sweep geometry, shift
 * light ladder + flash phases, per-channel color thresholds, valid-gated
 * alarm classification (R11), lap-time formatting, and drift-free odometer
 * tenths integration. Pins the math so a renderer edit cannot silently
 * change behavior.
 *
 * Runs on the host:
 *   gcc -std=c11 -Wall -Wextra -Werror -I MustangDash \
 *       tests/test_dash_math.c -lm -o /tmp/tdm && /tmp/tdm
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "dash_math.h"
#include "dash_layout.h"

/* spec pins -- these numbers come from the vehicle/README spec table */
_Static_assert(DASH_GAUGE_SWEEP_DEG == 240, "gauge sweep must be 240 deg");
_Static_assert(DASH_SPEED_KNEE == 140, "speed knee must be 140 mph");
_Static_assert(DASH_REDLINE_RPM == 8000, "redline must be 8000 rpm");
_Static_assert(DASH_SHIFT_RPM == 7600, "shift point must be 7600 rpm");
_Static_assert(DASH_SHIFT_LED_COUNT == 15, "shift ladder must be 15 LEDs");

static int failures = 0;
static int checks = 0;

static void expect(int cond, const char *msg)
{
    checks++;
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

static int nearf(float a, float b, float eps)
{
    return fabsf(a - b) <= eps;
}

int main(void)
{
    /* ---- speed fraction: knee mapping from the mock ---- */
    expect(dash_speed_frac(0.0f) == 0.0f, "speed frac at 0 mph must be 0");
    expect(nearf(dash_speed_frac(140.0f), 0.8235f, 1e-4f),
           "speed frac at the 140 mph knee must be 0.8235");
    expect(nearf(dash_speed_frac(160.0f), 0.8823f, 1e-4f),
           "speed frac at 160 mph must be 0.8823");
    expect(dash_speed_frac(200.0f) >= 0.999f && dash_speed_frac(200.0f) <= 1.0f,
           "speed frac at 200 mph must be in [0.999, 1]");
    for (int v = 0; v < 200; v++)
    {
        expect(dash_speed_frac((float)(v + 1)) > dash_speed_frac((float)v) - 1e-6f,
               "speed frac must be monotone over 0..200 mph");
    }
    expect(dash_speed_frac(250.0f) <= 1.0f, "speed frac must clamp above 200 mph");

    /* ---- gauge angle / arc point geometry ---- */
    expect(nearf(dash_gauge_angle_rad(0.0f), 210.0f * 3.14159265f / 180.0f, 1e-4f),
           "gauge angle at frac 0 must be 210 deg");
    expect(nearf(dash_gauge_angle_rad(1.0f), -30.0f * 3.14159265f / 180.0f, 1e-4f),
           "gauge angle at frac 1 must be -30 deg");
    {
        float x = 0.0f, y = 0.0f;
        dash_arc_point(120.0f, 80.0f, 88.0f, 0.0f, &x, &y);
        expect(nearf(x, 120.0f + 88.0f * cosf(dash_gauge_angle_rad(0.0f)), 1e-3f),
               "arc point x must be cx + r*cos(a)");
        expect(nearf(y, 80.0f - 88.0f * sinf(dash_gauge_angle_rad(0.0f)), 1e-3f),
               "arc point y must be cy - r*sin(a) (screen y down)");
        expect(nearf(y, 124.0f, 1e-3f),
               "arc point at frac 0 (210 deg) must land below center");
    }

    /* ---- shift LED ladder + flash ---- */
    expect(dash_shift_led_count(0.0f) == 0U, "0 rpm must light 0 shift LEDs");
    expect(dash_shift_led_count(533.34f) == 1U, "533.34 rpm must light LED 1");
    expect(dash_shift_led_count(532.0f) == 0U, "532 rpm must light 0 LEDs");
    expect(dash_shift_led_count(8000.0f) == 15U, "8000 rpm must light all 15 LEDs");
    expect(dash_shift_led_count(4000.0f) == 7U, "4000 rpm must light 7 LEDs (7.5 floors)");
    expect(!dash_shift_flash_zone(7600.0f), "flash zone must be off at exactly 7600");
    expect(dash_shift_flash_zone(7601.0f), "flash zone must be on above 7600");
    expect(dash_flash_phase(0U, 62U) == true, "flash phase must start on at t=0");
    expect(dash_flash_phase(62U, 62U) == false, "flash phase must be off at t=62");
    expect(dash_flash_phase(124U, 62U) == true, "flash phase must be on again at t=124");
    expect(DASH_SHIFT_FLASH_HALF_MS == 62U, "shift flash half period must be 62 ms");
    expect(DASH_ALARM_FLASH_HALF_MS == 179U, "alarm flash half period must be 179 ms");

    /* ---- rpm color bands (AE2): red strictly above 7600 ---- */
    expect(dash_rpm_color(7300.0f) == DASH_COLOR_AMBER, "7300 rpm must be amber");
    expect(dash_rpm_color(7599.0f) == DASH_COLOR_AMBER, "7599 rpm must be amber");
    expect(dash_rpm_color(7099.0f) == DASH_COLOR_NORMAL, "7099 rpm must be normal");
    expect(dash_rpm_color(7600.0f) == DASH_COLOR_AMBER, "7600 rpm itself must be amber");
    expect(dash_rpm_color(7601.0f) == DASH_COLOR_RED, "7601 rpm must be red");

    /* ---- per-channel thresholds at limit-eps / limit / limit+eps ---- */
    expect(dash_ect_state(217.1f) == DASH_COLOR_RED, "ECT 217.1 must be red");
    expect(dash_ect_state(217.0f) == DASH_COLOR_AMBER, "ECT 217.0 must be amber");
    expect(dash_ect_state(210.0f) == DASH_COLOR_NORMAL, "ECT 210.0 must be normal");
    expect(dash_ect_state(210.1f) == DASH_COLOR_AMBER, "ECT 210.1 must be amber");
    expect(dash_oil_press_state(29.1f) == DASH_COLOR_NORMAL, "oil press 29.1 must be normal");
    expect(dash_oil_press_state(29.0f) == DASH_COLOR_NORMAL, "oil press 29.0 must be normal (<29 rule)");
    expect(dash_oil_press_state(28.9f) == DASH_COLOR_RED, "oil press 28.9 must be red");
    expect(dash_volts_state(12.0f) == DASH_COLOR_NORMAL, "12.0 V must be normal");
    expect(dash_volts_state(11.9f) == DASH_COLOR_RED, "11.9 V must be red");
    expect(dash_fuel_state(2.5f) == DASH_COLOR_NORMAL, "2.5 gal must be normal");
    expect(dash_fuel_state(2.4f) == DASH_COLOR_AMBER, "2.4 gal must be amber");
    expect(dash_oil_temp_state(235.1f) == DASH_COLOR_AMBER, "oil temp 235.1 must be amber");
    expect(dash_oil_temp_state(248.1f) == DASH_COLOR_RED, "oil temp 248.1 must be red");

    /* ---- oil telltale: invalid channels never trigger (R11) ---- */
    expect(dash_telltale_oil(25.0f, true, 200.0f, true), "valid low oil pressure must trip telltale");
    expect(!dash_telltale_oil(25.0f, false, 200.0f, true), "invalid oil pressure must not trip telltale");
    expect(dash_telltale_oil(60.0f, true, 260.0f, true), "valid hot oil must trip telltale");
    expect(!dash_telltale_oil(25.0f, false, 260.0f, false), "all-invalid must never trip telltale");

    /* ---- alarm classification: valid-gated, oilp > oilt > clt priority ---- */
    {
        DashState s;
        dash_state_init(&s);
        expect(dash_alarm_classify(&s) == DASH_ALARM_NONE, "no valid channels must classify NONE");

        dash_ch_set(&s, DASH_CH_OILP, 25.0f);
        dash_ch_set(&s, DASH_CH_ECT, 220.0f);
        expect(dash_alarm_classify(&s) == DASH_ALARM_CLT,
               "low oil pressure with no rpm (engine off) must not alarm OILP");

        dash_ch_set(&s, DASH_CH_RPM, 300.0f);
        expect(dash_alarm_classify(&s) == DASH_ALARM_CLT,
               "low oil pressure below cranking rpm must not alarm OILP");

        dash_ch_set(&s, DASH_CH_RPM, 3000.0f);
        expect(dash_alarm_classify(&s) == DASH_ALARM_OILP,
               "low oil pressure with the engine running must outrank hot coolant");

        dash_ch_set(&s, DASH_CH_OILP, 60.0f);
        expect(dash_alarm_classify(&s) == DASH_ALARM_CLT,
               "healthy oil pressure + hot coolant must classify CLT");

        dash_state_init(&s);
        dash_ch_set(&s, DASH_CH_OILT, 260.0f); /* oilp never set -> invalid */
        expect(dash_alarm_classify(&s) == DASH_ALARM_OILT,
               "hot oil with oil pressure invalid must classify OILT");

        dash_state_init(&s);
        s.ch.oil_press_psi = 25.0f; /* value present but valid bit NOT set */
        dash_ch_set(&s, DASH_CH_RPM, 3000.0f);
        dash_ch_set(&s, DASH_CH_ECT, 220.0f);
        expect(dash_alarm_classify(&s) == DASH_ALARM_CLT,
               "invalid oil pressure value must not assert OILP (R11)");
    }

    /* ---- delta bar fill (AE3): signed clamp to [-1, +1] ---- */
    expect(nearf(dash_delta_fill(-0.4f), -0.4f, 1e-6f), "delta -0.4 s must fill -0.4");
    expect(dash_delta_fill(-3.0f) == -1.0f, "delta -3 s must clamp to -1");
    expect(dash_delta_fill(2.5f) == 1.0f, "delta +2.5 s must clamp to +1");

    /* ---- lap time formatting ---- */
    {
        char buf[16];
        dash_fmt_lap(0U, true, buf);
        expect(strcmp(buf, "0:00.000") == 0, "lap 0 ms must format 0:00.000");
        dash_fmt_lap(58120U, true, buf);
        expect(strcmp(buf, "0:58.120") == 0, "lap 58120 ms must format 0:58.120");
        dash_fmt_lap(62000U, true, buf);
        expect(strcmp(buf, "1:02.000") == 0, "lap 62000 ms must format 1:02.000");
        dash_fmt_lap(62000U, false, buf);
        expect(strcmp(buf, "--:--.---") == 0, "invalid lap must format --:--.---");
    }

    /* ---- odometer tenths integration: exact, drift-free carry ---- */
    {
        uint32_t rem_big = 0U;
        uint32_t tenths_big = dash_odo_step(60.0f, 6000U, &rem_big);
        expect(tenths_big == 1U, "60 mph for 6000 ms must be exactly 1 tenth");

        uint32_t rem_small = 0U;
        uint32_t tenths_small = 0U;
        for (int i = 0; i < 60; i++)
        {
            tenths_small += dash_odo_step(60.0f, 100U, &rem_small);
        }
        expect(tenths_small == 1U, "60 x 100 ms steps at 60 mph must also total 1 tenth");
        expect(rem_small == rem_big, "small-step remainder must equal single-step remainder (no drift)");

        uint32_t rem_zero = 0U;
        expect(dash_odo_step(0.0f, 1000U, &rem_zero) == 0U, "0 mph must add 0 tenths");
        expect(rem_zero == 0U, "0 mph must leave the remainder untouched");
    }

    /* ---- layout scale macros: 620x400 mock -> 1024x600 panel ---- */
    expect(DASH_LX(620) == 1024, "LX(620) must be 1024 (full width)");
    expect(DASH_LY(400) == 600, "LY(400) must be 600 (full height)");
    expect(DASH_LX(0) == 0, "LX(0) must be 0");
    expect(DASH_LR(88) == 132, "LR(88) must be 132 (radius scales by 1.5)");
    expect(DASH_LX(310) == 512, "LX(310) must be 512 (mock center -> panel center)");
    expect(DASH_W == 1024 && DASH_H == 600, "panel size must be 1024x600");

    if (failures == 0)
    {
        printf("OK: dash math matches the U3 spec (%d checks: knee map, gauge geometry, "
               "shift ladder, thresholds, alarms, lap format, odometer)\n", checks);
        return 0;
    }
    return 1;
}
