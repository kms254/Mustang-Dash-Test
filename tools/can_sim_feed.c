/* can_sim_feed.c -- bench feed: the dash's REAL simulator, streamed as text.
 *
 * Steps dash_sim.h (the same host-tested code the firmware runs -- not a
 * reimplementation, plan R3) in real time on the HPR lap and prints one
 * line per 16 ms step to stdout with the 9 CAN-covered channels in dash
 * units (mph / degF / psi / V / A/F):
 *
 *   rpm=3500.0 speed=74.1919 ect=194.00 oilt=176.00 oilp=49.313
 *   volts=13.30 afr_l=12.60 afr_r=12.10 fuelp=49.313
 *
 * key=value pairs, parser-friendly; a channel INVALID in DashState is
 * simply omitted from the line. stdout is line-buffered (block-buffered
 * pipes deliver stale bursts); pacing uses absolute deadlines on
 * CLOCK_MONOTONIC so cadence drift does not accumulate -- this is a bench
 * feed, wall time is correct (unlike the tests, which own their clock).
 *
 * Build + run under WSL (the host-test toolchain -- Windows has no C
 * compiler):
 *
 *   wsl -- bash -lc "gcc -std=c11 -Wall -Werror -I MustangDash tools/can_sim_feed.c -lm -o /tmp/can_sim_feed && /tmp/can_sim_feed"
 *
 * Bench use pipes it across the WSL/Windows boundary into the emitter
 * (plan KTD9 -- the CANable stack lives on the PIO penv python):
 *
 *   wsl -- /tmp/can_sim_feed | C:\Users\kevin\.platformio\penv\Scripts\python.exe tools/can_emit.py
 *
 * Exits cleanly on SIGINT/SIGTERM and on a broken pipe (the emitter
 * dying must not spew write errors -- SIGPIPE is ignored and the stream
 * error flag ends the loop instead).
 *
 * Plan: docs/plans/2026-08-15-001-feat-can-telemetry-ford-dialect-plan.md (U5).
 */

#define _POSIX_C_SOURCE 200809L /* clock_gettime/clock_nanosleep under -std=c11 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dash_data.h"
#include "dash_sim.h"

#define FEED_STEP_MS 16u /* nominal loop cadence, ~60 Hz like the firmware */

/* A stall (suspend, debugger, starved VM) leaves the absolute deadline far
 * in the past; beyond this, resync to now instead of machine-gunning
 * catch-up lines. Steady-state jitter never comes near it. */
#define FEED_RESYNC_MS 1000L

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void) sig;
    g_stop = 1;
}

static void ts_add_ms(struct timespec *ts, long ms)
{
    ts->tv_sec += ms / 1000L;
    ts->tv_nsec += (ms % 1000L) * 1000000L;
    if (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec += 1;
    }
}

/* a - b, in milliseconds */
static long ts_diff_ms(const struct timespec *a, const struct timespec *b)
{
    return (long) (a->tv_sec - b->tv_sec) * 1000L
           + (long) ((a->tv_nsec - b->tv_nsec) / 1000000L);
}

/* Print the 9 CAN-covered channels (plan KTD2) that are currently VALID,
 * in dash units, space-separated key=value, one line. Returns false once
 * the stream has errored (broken pipe: the emitter died). */
static bool feed_print_line(const DashState *s)
{
    const char *sep = "";

    if (dash_ch_valid(s, DASH_CH_RPM))   { printf("%srpm=%.1f",   sep, (double) s->ch.rpm);            sep = " "; }
    if (dash_ch_valid(s, DASH_CH_SPEED)) { printf("%sspeed=%.4f", sep, (double) s->ch.speed_mph);      sep = " "; }
    if (dash_ch_valid(s, DASH_CH_ECT))   { printf("%sect=%.2f",   sep, (double) s->ch.ect_f);          sep = " "; }
    if (dash_ch_valid(s, DASH_CH_OILT))  { printf("%soilt=%.2f",  sep, (double) s->ch.oil_temp_f);     sep = " "; }
    if (dash_ch_valid(s, DASH_CH_OILP))  { printf("%soilp=%.3f",  sep, (double) s->ch.oil_press_psi);  sep = " "; }
    if (dash_ch_valid(s, DASH_CH_VOLTS)) { printf("%svolts=%.2f", sep, (double) s->ch.volts);          sep = " "; }
    if (dash_ch_valid(s, DASH_CH_AFR_L)) { printf("%safr_l=%.2f", sep, (double) s->ch.afr_l);          sep = " "; }
    if (dash_ch_valid(s, DASH_CH_AFR_R)) { printf("%safr_r=%.2f", sep, (double) s->ch.afr_r);          sep = " "; }
    if (dash_ch_valid(s, DASH_CH_FUELP)) { printf("%sfuelp=%.3f", sep, (double) s->ch.fuel_press_psi); sep = " "; }

    if (sep[0] != '\0')
    {
        printf("\n"); /* line-buffered: this is the flush */
    }
    return !ferror(stdout);
}

int main(void)
{
    /* FIRST, before anything can print: a block-buffered pipe would sit on
     * ~4 KB of lines and deliver them to the emitter in stale bursts. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal; /* no SA_RESTART: the sleep must wake */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN); /* broken pipe -> ferror(stdout), clean exit */

    DashState s;
    DashSimState sim;
    dash_state_init(&s); /* TRACK mode default */
    dash_sim_init(&sim); /* HPR is the sim's default circuit (R12) */

    fprintf(stderr,
            "can_sim_feed: real dash_sim, HPR lap, %u ms steps -- Ctrl-C to stop\n",
            (unsigned) FEED_STEP_MS);

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!g_stop)
    {
        /* Absolute-deadline arithmetic: each tick advances the PREVIOUS
         * deadline, so per-sleep overshoot never accumulates as drift. */
        ts_add_ms(&next, (long) FEED_STEP_MS);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (ts_diff_ms(&now, &next) > FEED_RESYNC_MS)
        {
            next = now; /* stalled hard: pick the cadence back up from here */
        }

        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
        {
            if (g_stop) { break; }
        }
        if (g_stop) { break; }

        dash_sim_step(&sim, &s, FEED_STEP_MS);
        if (!feed_print_line(&s))
        {
            break; /* emitter side of the pipe is gone; nothing to say it to */
        }
    }

    fprintf(stderr, "can_sim_feed: stopped\n");
    return 0;
}
