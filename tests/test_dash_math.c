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
    /* Oil temp thresholds are TRACK thresholds, not street ones: a 500+ whp
     * car on a road course runs 250-280 F, so 255 (what dash_sim.h's session
     * lands on) must read normal, amber starts at 270 and red at 290. */
    expect(dash_oil_temp_state(255.0f) == DASH_COLOR_NORMAL,
           "oil temp 255 -- a normal track session -- must NOT be amber");
    expect(dash_oil_temp_state(270.0f) == DASH_COLOR_NORMAL, "oil temp 270.0 must be normal");
    expect(dash_oil_temp_state(270.1f) == DASH_COLOR_AMBER, "oil temp 270.1 must be amber");
    expect(dash_oil_temp_state(290.0f) == DASH_COLOR_AMBER, "oil temp 290.0 must be amber");
    expect(dash_oil_temp_state(290.1f) == DASH_COLOR_RED, "oil temp 290.1 must be red");

    /* ---- Phase-2 thresholds (KTD5): fuel pressure red, IAT/AFR amber ---- */
    expect(dash_fuelp_state(42.9f) == DASH_COLOR_RED, "fuel pressure 42.9 must be red");
    expect(dash_fuelp_state(43.0f) == DASH_COLOR_NORMAL, "fuel pressure 43.0 must be normal (<43 rule)");
    expect(dash_fuelp_state(43.1f) == DASH_COLOR_NORMAL, "fuel pressure 43.1 must be normal");
    expect(dash_iat_state(131.0f) == DASH_COLOR_NORMAL, "IAT 131.0 must be normal");
    expect(dash_iat_state(131.1f) == DASH_COLOR_AMBER, "IAT 131.1 must be amber");
    expect(dash_iat_state(132.0f) == DASH_COLOR_AMBER, "IAT 132.0 must be amber");
    expect(dash_afr_state(13.8f) == DASH_COLOR_NORMAL, "AFR 13.8 must be normal (>13.8 rule)");
    expect(dash_afr_state(13.9f) == DASH_COLOR_AMBER, "AFR 13.9 must be amber");

    /* ---- oil telltale: invalid channels never trigger (R11) ---- */
    expect(dash_telltale_oil(25.0f, true, 200.0f, true), "valid low oil pressure must trip telltale");
    expect(!dash_telltale_oil(25.0f, false, 200.0f, true), "invalid oil pressure must not trip telltale");
    expect(dash_telltale_oil(60.0f, true, DASH_OILT_RED_F + 1.0f, true),
           "valid hot oil must trip telltale");
    expect(!dash_telltale_oil(60.0f, true, 255.0f, true),
           "a normal track session's 255 F oil must NOT trip the telltale");
    expect(!dash_telltale_oil(25.0f, false, DASH_OILT_RED_F + 1.0f, false),
           "all-invalid must never trip telltale");

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
        dash_ch_set(&s, DASH_CH_OILT, DASH_OILT_RED_F + 1.0f); /* oilp never set -> invalid */
        expect(dash_alarm_classify(&s) == DASH_ALARM_OILT,
               "hot oil with oil pressure invalid must classify OILT");

        /* The oil-temp thresholds moved up to track figures (270/290), which
         * makes it possible to raise them until the alarm is unreachable in
         * practice. Both ends are pinned: a genuinely cooked engine still
         * takes the screen, and a normal session's 255 F never does. */
        dash_state_init(&s);
        dash_ch_set(&s, DASH_CH_OILT, 255.0f);
        expect(dash_alarm_classify(&s) == DASH_ALARM_NONE,
               "a normal track session's oil temp must never raise an alarm");
        dash_ch_set(&s, DASH_CH_OILT, DASH_OILT_RED_F + 10.0f);
        expect(dash_alarm_classify(&s) == DASH_ALARM_OILT,
               "genuinely overheated oil must still take the screen");

        /* the oil-PRESSURE alarm's engine-running gate is independent of the
         * oil-TEMPERATURE thresholds: hot oil must not need rpm to alarm */
        dash_state_init(&s);
        dash_ch_set(&s, DASH_CH_OILT, DASH_OILT_RED_F + 1.0f);
        expect(!dash_ch_valid(&s, DASH_CH_RPM), "rpm must be invalid for this case");
        expect(dash_alarm_classify(&s) == DASH_ALARM_OILT,
               "the OILT alarm must not inherit the OILP engine-running gate");

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

    /* ---- session clock formatting: MM:SS, never decimal minutes ---- */
    {
        char buf[12];
        dash_fmt_mmss(0U, true, buf);
        expect(strcmp(buf, "00:00") == 0, "session 0 ms must format 00:00");
        dash_fmt_mmss(59999U, true, buf);
        expect(strcmp(buf, "00:59") == 0, "session 59999 ms must truncate to 00:59");
        dash_fmt_mmss(60000U, true, buf);
        expect(strcmp(buf, "01:00") == 0, "session 60000 ms must format 01:00");
        dash_fmt_mmss(1200000U, true, buf);
        expect(strcmp(buf, "20:00") == 0, "the 20-minute session mark must format 20:00");
        /* the session runs PAST 20:00 until the in-lap finishes (U8) */
        dash_fmt_mmss(1342500U, true, buf);
        expect(strcmp(buf, "22:22") == 0, "a session past the flag must keep counting up");
        dash_fmt_mmss(60000U, false, buf);
        expect(strcmp(buf, "--:--") == 0, "invalid session must format --:--");
    }

    /* ---- lap-crossing delta flash (MoTeC override pattern) ----
     * The state machine is driven purely off published channels plus the
     * simulator's sticky taint flag, so every rule below is testable without
     * a display: out-lap suppression, vs-PREVIOUS (not vs-best) reference,
     * the new-best treatment, the 4 s hold, and the taint gate. */
    {
        DashState s;
        DashLapFlash f;
        char buf[16];

        /* helper-free: drive the channels by hand so the test states the
         * contract rather than re-deriving it from dash_sim.h */
        dash_state_init(&s);
        dash_lap_flash_reset(&f);

        /* boot: lap 1 in progress, nothing completed */
        dash_ch_set(&s, DASH_CH_LAPN, 1.0f);
        dash_lap_flash_update(&f, &s, 1000U, false);
        expect(dash_lap_flash_kind(&f) == DASH_LAPFLASH_NONE,
               "no lap has completed, so nothing may override the lap clock");

        /* lap 1 completes (the out-lap): suppressed */
        dash_ch_set(&s, DASH_CH_LAPN, 2.0f);
        dash_ch_set(&s, DASH_CH_LAST, 140000.0f);
        dash_lap_flash_update(&f, &s, 2000U, false);
        expect(dash_lap_flash_kind(&f) == DASH_LAPFLASH_NONE,
               "the out-lap must never flash: there is no earlier lap to compare it to");

        /* lap 2 completes: the first flying lap, but lap 2 vs the out-lap is
         * the fake ~18 s improvement U1 excludes everywhere else */
        dash_ch_set(&s, DASH_CH_LAPN, 3.0f);
        dash_ch_set(&s, DASH_CH_LAST, 122000.0f);
        dash_ch_set(&s, DASH_CH_BEST, 122000.0f);
        dash_lap_flash_update(&f, &s, 3000U, false);
        expect(dash_lap_flash_kind(&f) == DASH_LAPFLASH_NONE,
               "lap 2 vs the out-lap must be suppressed, not shown as an 18 s gain");

        /* lap 3 completes, quicker than lap 2 AND a new best */
        dash_ch_set(&s, DASH_CH_LAPN, 4.0f);
        dash_ch_set(&s, DASH_CH_LAST, 121580.0f);
        dash_ch_set(&s, DASH_CH_BEST, 121580.0f);
        dash_lap_flash_update(&f, &s, 100000U, false);
        expect(dash_lap_flash_kind(&f) == DASH_LAPFLASH_BEST,
               "a new best must get the dedicated best treatment, not the plain green");
        /* a new best ALTERNATES the word with the number on the blink phase,
         * so the driver gets the event and the delta inside one hold. Both
         * phases are pinned: BEST! is only renderable because DF_MID carries
         * A-Z (tools/make_dash_fonts.py), and the number must still appear. */
        dash_lap_flash_text(&f, f.start_ms, buf);
        expect(strcmp(buf, "BEST!") == 0,
               "a new best must show BEST! on the highlight phase");
        dash_lap_flash_text(&f, f.start_ms + DASH_LAP_FLASH_BLINK_HALF_MS, buf);
        expect(strcmp(buf, "-0.42") == 0,
               "the flash must read the signed 2-decimal delta vs the PREVIOUS lap");

        /* the hold is 4 s: still up just before, gone at the boundary */
        dash_lap_flash_update(&f, &s, 100000U + DASH_LAP_FLASH_MS - 1U, false);
        expect(dash_lap_flash_kind(&f) == DASH_LAPFLASH_BEST,
               "the override must still be up 1 ms before the hold expires");
        dash_lap_flash_update(&f, &s, 100000U + DASH_LAP_FLASH_MS, false);
        expect(dash_lap_flash_kind(&f) == DASH_LAPFLASH_NONE,
               "the override must revert to the running lap clock after the hold");
        expect(DASH_LAP_FLASH_MS <= 7000U,
               "the hold must stay under AiM's ~7 s point, past which it hides the next lap");

        /* lap 4 completes, slower than lap 3 -- red, and NOT a best */
        dash_ch_set(&s, DASH_CH_LAPN, 5.0f);
        dash_ch_set(&s, DASH_CH_LAST, 122730.0f);
        dash_lap_flash_update(&f, &s, 200000U, false);
        expect(dash_lap_flash_kind(&f) == DASH_LAPFLASH_SLOWER,
               "a slower lap must flash slower, even though BEST did not move");
        dash_lap_flash_text(&f, f.start_ms, buf);
        expect(strcmp(buf, "+1.15") == 0, "a slower lap must read a signed positive delta");

        /* lap 5 quicker than lap 4 but still off the best: plain green, and
         * this is the case that proves the reference is PREVIOUS, not best */
        dash_ch_set(&s, DASH_CH_LAPN, 6.0f);
        dash_ch_set(&s, DASH_CH_LAST, 122000.0f);
        dash_lap_flash_update(&f, &s, 300000U, false);
        expect(dash_lap_flash_kind(&f) == DASH_LAPFLASH_QUICKER,
               "quicker than the previous lap but off the best must flash plain quicker");
        dash_lap_flash_text(&f, f.start_ms, buf);
        expect(strcmp(buf, "-0.73") == 0,
               "the delta must be against the previous lap, not against best");

        /* a TAINTED lap must not flash at all... */
        dash_ch_set(&s, DASH_CH_LAPN, 7.0f);
        dash_ch_set(&s, DASH_CH_LAST, 46000.0f);
        dash_lap_flash_update(&f, &s, 400000U, true);
        expect(dash_lap_flash_kind(&f) == DASH_LAPFLASH_NONE,
               "a lap driven at a forced speed must never flash a fabricated gain");

        /* ...and the lap AFTER it must not either: the reference is that same
         * fabricated time, so it would flash an equally fabricated loss */
        dash_ch_set(&s, DASH_CH_LAPN, 8.0f);
        dash_ch_set(&s, DASH_CH_LAST, 122100.0f);
        dash_lap_flash_update(&f, &s, 500000U, false);
        expect(dash_lap_flash_kind(&f) == DASH_LAPFLASH_NONE,
               "the lap after a tainted one must not flash against the tainted time");

        /* the lap after THAT is honest again */
        dash_ch_set(&s, DASH_CH_LAPN, 9.0f);
        dash_ch_set(&s, DASH_CH_LAST, 122400.0f);
        dash_lap_flash_update(&f, &s, 600000U, false);
        expect(dash_lap_flash_kind(&f) == DASH_LAPFLASH_SLOWER,
               "the flash must recover on the first clean pair after a tainted lap");

        /* the 20-minute session rollover restarts the lap book: LAPN goes
         * backwards and no comparison may cross the boundary */
        dash_ch_set(&s, DASH_CH_LAPN, 1.0f);
        dash_ch_invalidate(&s, DASH_CH_LAST);
        dash_ch_invalidate(&s, DASH_CH_BEST);
        dash_lap_flash_update(&f, &s, 700000U, false);
        expect(dash_lap_flash_kind(&f) == DASH_LAPFLASH_NONE,
               "a session rollover must clear the override");
        dash_ch_set(&s, DASH_CH_LAPN, 2.0f);
        dash_ch_set(&s, DASH_CH_LAST, 141000.0f);
        dash_lap_flash_update(&f, &s, 701000U, false);
        expect(dash_lap_flash_kind(&f) == DASH_LAPFLASH_NONE,
               "the new session's out-lap must be suppressed like the first one");

        /* STREET / SWEEP: LAPN is dead-fronted, so there is no lap to flash --
         * and an override already up must drop the moment the mode changes,
         * not linger for the rest of its hold on a screen with no laps. */
        dash_ch_set(&s, DASH_CH_LAPN, 3.0f);
        dash_ch_set(&s, DASH_CH_LAST, 122000.0f);
        dash_lap_flash_update(&f, &s, 702000U, false);
        dash_ch_set(&s, DASH_CH_LAPN, 4.0f);
        dash_ch_set(&s, DASH_CH_LAST, 122500.0f);
        dash_lap_flash_update(&f, &s, 703000U, false);
        expect(dash_lap_flash_kind(&f) == DASH_LAPFLASH_SLOWER,
               "lap 3 of a NEW session must flash, on the new session's own lap count");
        dash_ch_invalidate(&s, DASH_CH_LAPN);
        dash_lap_flash_update(&f, &s, 703100U, false);
        expect(dash_lap_flash_kind(&f) == DASH_LAPFLASH_NONE,
               "leaving the lap path must drop the override immediately");
    }

    /* ---- shared value formatter (dash_fmt_value) ---- */
    {
        char buf[16];
        dash_fmt_value(buf, sizeof(buf), 42.7f, 0U, false);
        expect(strcmp(buf, "--") == 0, "invalid value must format --");
        dash_fmt_value(buf, sizeof(buf), 42.44f, 1U, true);
        expect(strcmp(buf, "42.4") == 0, "decimals=1 must format one decimal");
        dash_fmt_value(buf, sizeof(buf), 42.5f, 0U, true);
        expect(strcmp(buf, "43") == 0, "decimals=0 must round half up (42.5 -> 43)");
        dash_fmt_value(buf, sizeof(buf), 42.4f, 0U, true);
        expect(strcmp(buf, "42") == 0, "decimals=0 must round down (42.4 -> 42)");
        dash_fmt_value(buf, sizeof(buf), -10.6f, 0U, true);
        expect(strcmp(buf, "-11") == 0,
               "negative must round half away from zero (-10.6 -> -11)");
        dash_fmt_value(buf, sizeof(buf), -0.4f, 0U, true);
        expect(strcmp(buf, "0") == 0, "-0.4 must round to 0");
        dash_fmt_value(buf, sizeof(buf), -40.0f, 0U, true);
        expect(strcmp(buf, "-40") == 0, "AMB floor -40 must format -40");
    }

    /* ---- shared clamp (dash_clampf) ---- */
    {
        expect(dash_clampf(-0.5f, 0.0f, 1.0f) == 0.0f, "clamp below lo must return lo");
        expect(dash_clampf(1.5f, 0.0f, 1.0f) == 1.0f, "clamp above hi must return hi");
        expect(dash_clampf(0.25f, 0.0f, 1.0f) == 0.25f, "clamp inside range must pass through");
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

    /* ---- fuel derivations (README ~L103, KTD5): range = gal*16, laps at
     * the design's per-lap burn; both signal "not computable" via a bool
     * return + zeroed out-param when fuel is invalid ---- */
    {
        float out = -1.0f;

        expect(dash_range_mi(5.0f, true, &out) == true, "range_mi must compute when fuel valid");
        expect(nearf(out, 80.0f, 1e-4f), "range_mi(5 gal) must be exactly 5*16 = 80 mi");
        expect(dash_range_mi(0.0f, true, &out) == true, "range_mi must compute at 0 gal");
        expect(nearf(out, 0.0f, 1e-6f), "range_mi(0 gal) must be 0 mi");
        out = -1.0f;
        expect(dash_range_mi(5.0f, false, &out) == false, "range_mi must be not-computable when fuel invalid");
        expect(out == 0.0f, "range_mi must zero the out-param when not computable");

        out = -1.0f;
        expect(dash_laps_remaining(DASH_LAP_BURN_GAL * 3.0f, true, &out) == true,
               "laps_remaining must compute when fuel valid");
        expect(nearf(out, 3.0f, 1e-4f), "laps_remaining must be fuel / DASH_LAP_BURN_GAL");
        out = -1.0f;
        expect(dash_laps_remaining(12.0f, false, &out) == false,
               "laps_remaining must be not-computable when fuel invalid");
        expect(out == 0.0f, "laps_remaining must zero the out-param when not computable");
    }

    /* ---- layout scale macros: 620x400 mock -> 1024x600 panel ---- */
    expect(DASH_LX(620) == 1024, "LX(620) must be 1024 (full width)");
    expect(DASH_LY(400) == 600, "LY(400) must be 600 (full height)");
    expect(DASH_LX(0) == 0, "LX(0) must be 0");
    expect(DASH_LR(88) == 132, "LR(88) must be 132 (radius scales by 1.5)");
    expect(DASH_LX(310) == 512, "LX(310) must be 512 (mock center -> panel center)");
    expect(DASH_W == 1024 && DASH_H == 600, "panel size must be 1024x600");

    /* ---- 5" side-panel layout scale macros: 420x320 mock -> 800x480 panel,
     * same round-half-up convention as the center macros above ---- */
    expect(DASH5_LX(420) == 800, "DASH5_LX(420) must be 800 (full width)");
    expect(DASH5_LY(320) == 480, "DASH5_LY(320) must be 480 (full height)");
    expect(DASH5_LX(0) == 0, "DASH5_LX(0) must be 0");
    expect(DASH5_LY(0) == 0, "DASH5_LY(0) must be 0");
    /* half-pixel rounding: 3*1.5 = 4.5 -> round-half-up gives 5, same
     * convention DASH_LY(3) uses ((3*600+200)/400 = 5)) */
    expect(DASH5_LY(3) == 5, "DASH5_LY(3) must round 4.5 up to 5 (half-pixel, center convention)");
    /* 1*1.5 = 1.5 -> round-half-up gives 2 */
    expect(DASH5_LR(1) == 2, "DASH5_LR(1) must round 1.5 up to 2 (half-pixel, center convention)");
    expect(DASH5_LR(118) == 177, "DASH5_LR(118) must be 177 (radius scales by 1.5)");
    expect(DASH5_W == 800 && DASH5_H == 480, "5\" panel size must be 800x480");

    if (failures == 0)
    {
        printf("OK: dash math matches the U3 spec (%d checks: knee map, gauge geometry, "
               "shift ladder, thresholds, alarms, lap format, odometer)\n", checks);
        return 0;
    }
    return 1;
}
