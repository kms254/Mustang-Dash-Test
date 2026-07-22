/*
 * dash_button.h - pure gesture state machine for the trip/mode button (U11).
 *
 * One physical button, two gestures (the split the user chose):
 *
 *   short press  (< 1 s, fires on RELEASE) -> toggle TRACK / STREET
 *   long  press  (held >= 1 s, fires WHILE STILL HELD) -> trip reset
 *
 * Mode swap is the frequent bench action so it gets the cheap gesture; trip
 * reset -- which zeros a counter AND writes EEPROM -- moves behind the
 * deliberate one. The long press deliberately fires at the 1 s mark rather
 * than on release so the user sees the trip zero without having to let go;
 * the release that follows is then swallowed (see the `fired` latch) so one
 * press can never produce both events.
 *
 * Debounce contract, inherited from a prior review finding and preserved
 * verbatim: nothing is acted on until the raw level has been stable for
 * MORE than DASH_BTN_DEBOUNCE_MS (30 ms) -- a single EMI glitch on a car
 * harness must not zero the trip -- and exactly one event fires per press.
 * A bounce shorter than the window, in either direction, is invisible here.
 *
 * Polarity lives entirely in the caller. dash_button_step takes an already-
 * normalized `pressed_now` bool, so the Nucleo's active-HIGH USER button B1
 * (external pull-down, plain INPUT) and the active-LOW-on-internal-pull-up
 * boards share this identical logic -- see DASH_SWITCH_TRIP_PRESSED in the
 * .ino. Nothing in this file may ever mention a pin or a logic level.
 *
 * No Arduino or EVE dependencies, so it is host-testable
 * (see tests/test_dash_button.c) -- the dash_odo.h / splash_timeline.h pattern.
 *
 * All elapsed-time math is unsigned subtraction of uint32_t millis(), which
 * stays correct across the ~49.7-day rollover.
 */

#ifndef DASH_BUTTON_H
#define DASH_BUTTON_H

#include <stdint.h>
#include <stdbool.h>

/* Raw level must hold for MORE than this to be believed (review finding). */
#define DASH_BTN_DEBOUNCE_MS 30U
/* Hold at least this long, measured from the raw press edge, for a long press. */
#define DASH_BTN_LONG_MS 1000U

typedef enum {
    DASH_BTN_EVENT_NONE  = 0,
    DASH_BTN_EVENT_SHORT = 1, /* released before the long threshold */
    DASH_BTN_EVENT_LONG  = 2, /* still held, threshold reached */
} DashBtnEvent;

typedef struct {
    uint32_t edge_ms;  /* time of the last RAW level change (stability window) */
    uint32_t press_ms; /* raw edge that began the current debounced press */
    bool raw_down;     /* last raw sample */
    bool stable_down;  /* debounced level */
    bool fired;        /* an event already fired for this press (one per press) */
    bool primed;       /* the first sample has been taken (see dash_button_step) */
} DashButton;

static inline void dash_button_init(DashButton *b)
{
    b->edge_ms = 0U;
    b->press_ms = 0U;
    b->raw_down = false;
    b->stable_down = false;
    b->fired = false;
    b->primed = false; /* nothing is known about the line until it is first read */
}

/* Advance the machine one poll. `pressed_now` is the debounce-free, already
 * polarity-normalized reading; `now_ms` is millis(). Returns the event this
 * call produced, at most one. Call every loop iteration. */
static inline DashBtnEvent dash_button_step(DashButton *b, bool pressed_now, uint32_t now_ms)
{
    if (!b->primed)
    {
        /* The first poll is a MEASUREMENT of the line, never an edge. Seeding
         * raw_down = false in init instead made a level that was already
         * asserted at power-up -- a button held through boot, or a shorted
         * harness wire -- look like a fresh press at t=0, which fired LONG at
         * the 1 s mark and (since U11 put the trip reset on that gesture)
         * zeroed the trip odometer and wrote EEPROM silently on every boot.
         *
         * A level found already down is therefore adopted as ALREADY FIRED, so
         * it must be released once before any gesture can be produced. A line
         * found idle seeds exactly the state init used to assert, so nothing
         * about normal operation changes. */
        b->primed = true;
        b->edge_ms = now_ms;
        b->press_ms = now_ms;
        b->raw_down = pressed_now;
        b->stable_down = pressed_now;
        b->fired = pressed_now;
        return DASH_BTN_EVENT_NONE;
    }

    if (pressed_now != b->raw_down)
    {
        /* raw edge: restart the stability window, believe nothing yet */
        b->edge_ms = now_ms;
        b->raw_down = pressed_now;
        return DASH_BTN_EVENT_NONE;
    }

    if ((uint32_t)(now_ms - b->edge_ms) <= DASH_BTN_DEBOUNCE_MS)
    {
        return DASH_BTN_EVENT_NONE; /* still inside the stability window */
    }

    /* The raw level has been stable long enough to act on. */
    if (b->raw_down)
    {
        if (!b->stable_down)
        {
            /* press promoted. Time the hold from the RAW edge, not from the
             * moment the debounce cleared, so the 1 s the user feels is the
             * 1 s we measure. */
            b->stable_down = true;
            b->press_ms = b->edge_ms;
            b->fired = false;
        }
        if (!b->fired && (uint32_t)(now_ms - b->press_ms) >= DASH_BTN_LONG_MS)
        {
            b->fired = true; /* swallows the release below */
            return DASH_BTN_EVENT_LONG;
        }
    }
    else if (b->stable_down)
    {
        /* release promoted */
        const bool already_fired = b->fired;
        b->stable_down = false;
        b->fired = false; /* re-arm for the next press */
        if (!already_fired)
        {
            return DASH_BTN_EVENT_SHORT; /* held < DASH_BTN_LONG_MS */
        }
    }

    return DASH_BTN_EVENT_NONE;
}

#endif /* DASH_BUTTON_H */
