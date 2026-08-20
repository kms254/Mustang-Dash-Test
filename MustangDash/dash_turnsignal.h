/*
 * dash_turnsignal.h -- turn-signal stalk state + flasher (pure, host-tested)
 *
 * The dash has no turn-signal input of its own: in the car both blinkers
 * arrive as body signals over CAN, landing in DashState.tt_signals. On the
 * bench, BTN2 and BTN4 stand in for the stalk exactly as BTN1 stands in for
 * the CAN mode message -- an operator input, not invented data, which is why
 * this is allowed to write tt_signals while the simulator deliberately is
 * not (see docs/hardware/board3-telltale-legend.md).
 *
 * Stalk semantics: a press toggles its own side, and selecting one side
 * cancels the other -- you cannot signal both ways at once. The flasher
 * restarts lit on every activation, so a tap always produces a visible flash
 * rather than landing mid-dark-phase.
 *
 * Rate is 700 ms period at 50% duty = ~86 flashes/min, inside the 60-120
 * flashes/min band that FMVSS 108 / SAE J590 require of a real flasher.
 * The caller owns the clock and the lamp writes; this header only answers
 * "which blinker positions are lit at time t".
 */
#ifndef DASH_TURNSIGNAL_H
#define DASH_TURNSIGNAL_H

#include <stdint.h>
#include <stdbool.h>
#include "dash_telltales.h" /* DASH_TT_LEFT_BLINKER / DASH_TT_RIGHT_BLINKER */

#define DASH_TURN_PERIOD_MS 700U /* ~86 flashes/min */
#define DASH_TURN_ON_MS     350U /* 50% duty */

typedef enum {
    DASH_TURN_NONE = 0,
    DASH_TURN_LEFT,
    DASH_TURN_RIGHT
} DashTurnSide;

typedef struct {
    DashTurnSide side;
    uint32_t since_ms; /* activation instant; the flasher phase runs from here */
} DashTurn;

static inline void dash_turn_init(DashTurn *t)
{
    t->side = DASH_TURN_NONE;
    t->since_ms = 0U;
}

/* One press of the button for `side`. Same side again cancels; the other side
 * takes over. DASH_TURN_NONE is not a valid press and is ignored. */
static inline void dash_turn_press(DashTurn *t, DashTurnSide side, uint32_t now_ms)
{
    if (side == DASH_TURN_NONE)
    {
        return;
    }
    if (t->side == side)
    {
        t->side = DASH_TURN_NONE; /* cancel */
        return;
    }
    t->side = side;
    t->since_ms = now_ms; /* restart the flasher lit */
}

static inline void dash_turn_cancel(DashTurn *t)
{
    t->side = DASH_TURN_NONE;
}

/* True during the lit half of the flasher cycle. */
static inline bool dash_turn_phase_on(const DashTurn *t, uint32_t now_ms)
{
    if (t->side == DASH_TURN_NONE)
    {
        return false;
    }
    return ((uint32_t)(now_ms - t->since_ms) % DASH_TURN_PERIOD_MS) < DASH_TURN_ON_MS;
}

/* The blinker lamp positions lit right now, as a physical TT mask. Only ever
 * sets DASH_TT_LEFT_BLINKER or DASH_TT_RIGHT_BLINKER, so a caller can clear
 * exactly those two bits and OR this in without disturbing other signals. */
static inline uint8_t dash_turn_mask(const DashTurn *t, uint32_t now_ms)
{
    if (!dash_turn_phase_on(t, now_ms))
    {
        return 0U;
    }
    return (uint8_t) (1U << ((t->side == DASH_TURN_LEFT) ? DASH_TT_LEFT_BLINKER
                                                         : DASH_TT_RIGHT_BLINKER));
}

/* The two bits this module owns -- the caller's clear-mask. */
#define DASH_TURN_LAMP_BITS \
    ((uint8_t) ((1U << DASH_TT_LEFT_BLINKER) | (1U << DASH_TT_RIGHT_BLINKER)))

#endif /* DASH_TURNSIGNAL_H */
