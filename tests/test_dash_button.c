/*
 * Invariant test: the trip/mode button gesture state machine (dash_button.h).
 *
 * Pins the U11 gesture split -- short press (< 1 s, fires on RELEASE) toggles
 * TRACK/STREET, long press (held >= 1 s, fires WHILE STILL HELD) resets the
 * trip -- on top of the debounce contract that a prior review finding put
 * there: a single EMI glitch on a car harness must never zero the trip, so
 * nothing fires until the raw level has been stable for > 30 ms, and exactly
 * one event fires per press.
 *
 * Runs on the host:
 *   gcc -std=c11 -I MustangDash tests/test_dash_button.c -o /tmp/tdb && /tmp/tdb
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "dash_button.h"

/* The timing contract is the feature. Drifting either number silently changes
 * a bench gesture, so pin both. */
_Static_assert(DASH_BTN_DEBOUNCE_MS == 30U, "debounce window must stay 30 ms (review finding)");
_Static_assert(DASH_BTN_LONG_MS == 1000U, "long-press threshold must stay 1000 ms");

static int failures = 0;

static void expect(int cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

/* Feed a held level from t_start (exclusive) through t_end (inclusive) in 1 ms
 * steps, recording every non-NONE event. Returns the number of events seen and
 * reports the first one plus the time it fired. */
static uint32_t run_until(DashButton *b, bool pressed, uint32_t t_start, uint32_t t_end,
                          DashBtnEvent *first, uint32_t *first_ms)
{
    uint32_t count = 0U;
    for (uint32_t t = t_start + 1U; t <= t_end; t++)
    {
        DashBtnEvent e = dash_button_step(b, pressed, t);
        if (e != DASH_BTN_EVENT_NONE)
        {
            if (count == 0U)
            {
                if (first != NULL) { *first = e; }
                if (first_ms != NULL) { *first_ms = t; }
            }
            count++;
        }
    }
    return count;
}

int main(void)
{
    DashButton b;
    DashBtnEvent ev;
    uint32_t at = 0U, n = 0U;

    /* ---- idle: an untouched button never fires ---- */
    dash_button_init(&b);
    ev = DASH_BTN_EVENT_NONE;
    n = run_until(&b, false, 0U, 5000U, &ev, &at);
    expect(n == 0U, "an idle button must never emit an event");

    /* ---- glitch rejection: a 5 ms blip is not a press ---- */
    dash_button_init(&b);
    (void)dash_button_step(&b, false, 1000U);
    n = run_until(&b, true, 1000U, 1005U, NULL, NULL);
    expect(n == 0U, "a 5 ms blip must not fire during the blip");
    n = run_until(&b, false, 1005U, 4000U, &ev, &at);
    expect(n == 0U, "a 5 ms glitch must never fire -- an EMI spike must not zero the trip");

    /* A 29 ms press is still inside the stability window: never promoted. */
    dash_button_init(&b);
    (void)dash_button_step(&b, false, 1000U);
    (void)run_until(&b, true, 1000U, 1029U, NULL, NULL);
    n = run_until(&b, false, 1029U, 4000U, NULL, NULL);
    expect(n == 0U, "a 29 ms press must not survive the 30 ms debounce window");

    /* ---- short press: fires on release, exactly once, as SHORT ---- */
    dash_button_init(&b);
    (void)dash_button_step(&b, false, 1000U);
    ev = DASH_BTN_EVENT_NONE;
    n = run_until(&b, true, 1000U, 1300U, &ev, &at);
    expect(n == 0U, "a short press must NOT fire while still held");
    n = run_until(&b, false, 1300U, 1600U, &ev, &at);
    expect(n == 1U, "a short press must fire exactly once");
    expect(ev == DASH_BTN_EVENT_SHORT, "a 300 ms press must report SHORT");
    expect(at > 1300U && at <= 1300U + DASH_BTN_DEBOUNCE_MS + 2U,
           "SHORT must fire just after the release is debounced");

    /* ---- long press: fires at the 1 s mark WHILE STILL HELD ---- */
    dash_button_init(&b);
    (void)dash_button_step(&b, false, 1000U);
    ev = DASH_BTN_EVENT_NONE;
    at = 0U;
    n = run_until(&b, true, 1000U, 3000U, &ev, &at);
    expect(n == 1U, "a long press must fire exactly once while held");
    expect(ev == DASH_BTN_EVENT_LONG, "a 2 s hold must report LONG");
    expect(at >= 1000U + DASH_BTN_LONG_MS && at <= 1000U + DASH_BTN_LONG_MS + 2U,
           "LONG must fire at the 1 s mark, not on release");

    /* ...and releasing after a LONG must NOT also emit a SHORT */
    n = run_until(&b, false, 3000U, 3500U, NULL, NULL);
    expect(n == 0U, "release after a LONG must not also emit a SHORT");

    /* ---- boundary: just under 1 s is SHORT, at 1 s is LONG ---- */
    dash_button_init(&b);
    (void)dash_button_step(&b, false, 0U);
    (void)run_until(&b, true, 0U, 999U, NULL, NULL);
    ev = DASH_BTN_EVENT_NONE;
    n = run_until(&b, false, 999U, 1400U, &ev, &at);
    expect(n == 1U && ev == DASH_BTN_EVENT_SHORT, "a 999 ms press must still be SHORT");

    /* the press edge lands at t=1, so 1000 ms of hold elapses at t=1001 */
    dash_button_init(&b);
    (void)dash_button_step(&b, false, 0U);
    ev = DASH_BTN_EVENT_NONE;
    n = run_until(&b, true, 0U, 1001U, &ev, &at);
    expect(n == 1U && ev == DASH_BTN_EVENT_LONG, "a press held to exactly 1000 ms must be LONG");

    /* ---- one fire per press: bouncy contact chatter mid-hold ---- */
    dash_button_init(&b);
    (void)dash_button_step(&b, false, 0U);
    /* settle a press, fire LONG, then let the contact chatter for 10 ms
     * (shorter than the debounce window) -- it must not re-arm a second fire */
    n = run_until(&b, true, 0U, 1500U, &ev, &at);
    expect(n == 1U && ev == DASH_BTN_EVENT_LONG, "hold must fire LONG once");
    n = run_until(&b, false, 1500U, 1508U, NULL, NULL); /* 8 ms bounce open */
    n += run_until(&b, true, 1508U, 3000U, NULL, NULL);
    expect(n == 0U, "an 8 ms bounce mid-hold must not re-fire");

    /* a genuine release then a genuine new short press does fire again */
    n = run_until(&b, false, 3000U, 3100U, NULL, NULL);
    expect(n == 0U, "release after LONG stays silent");
    (void)run_until(&b, true, 3100U, 3300U, NULL, NULL);
    ev = DASH_BTN_EVENT_NONE;
    n = run_until(&b, false, 3300U, 3500U, &ev, &at);
    expect(n == 1U && ev == DASH_BTN_EVENT_SHORT,
           "a new press after a completed gesture must fire again");

    /* ---- back-to-back short presses each toggle once ---- */
    dash_button_init(&b);
    (void)dash_button_step(&b, false, 0U);
    uint32_t shorts = 0U;
    uint32_t t = 0U;
    for (uint32_t i = 0U; i < 5U; i++)
    {
        for (uint32_t k = 0U; k < 200U; k++) /* 200 ms held */
        {
            t++;
            if (dash_button_step(&b, true, t) == DASH_BTN_EVENT_SHORT) { shorts++; }
        }
        for (uint32_t k = 0U; k < 200U; k++) /* 200 ms released */
        {
            t++;
            if (dash_button_step(&b, false, t) == DASH_BTN_EVENT_SHORT) { shorts++; }
        }
    }
    expect(shorts == 5U, "five short presses must produce exactly five SHORT events");

    /* ---- polarity is the caller's job: the state machine sees only bools ----
     * dash_button_step takes `pressed_now`, already normalized by the .ino's
     * DASH_SWITCH_TRIP_PRESSED comparison, so active-high (Nucleo B1) and
     * active-low (pull-up boards) share this identical logic. Nothing here
     * may mention a pin level. */

    /* ---- millis() wraparound must not strand the machine ----
     * A press straddling the 32-bit millis rollover: unsigned subtraction
     * keeps the elapsed math correct. */
    dash_button_init(&b);
    const uint32_t near_wrap = 0xFFFFFF00UL;
    (void)dash_button_step(&b, false, near_wrap);
    ev = DASH_BTN_EVENT_NONE;
    n = 0U;
    for (uint32_t i = 1U; i <= 2000U; i++)
    {
        DashBtnEvent e = dash_button_step(&b, true, near_wrap + i); /* wraps past 0 */
        if (e != DASH_BTN_EVENT_NONE) { ev = e; n++; }
    }
    expect(n == 1U && ev == DASH_BTN_EVENT_LONG,
           "a hold across the millis() rollover must still fire LONG exactly once");

    if (failures == 0)
    {
        printf("    OK (dash_button.h gesture state machine)\n");
        return 0;
    }
    fprintf(stderr, "%d assertion(s) failed\n", failures);
    return 1;
}
