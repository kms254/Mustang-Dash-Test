/*
 * dash_ttsweep.h -- the boot/bench telltale sweep timeline (pure, host-tested)
 *
 * A key-on lamp show: a chase across the carrier's four physical LED
 * columns, a bounce back, then a full-row hold -- which doubles as the
 * bulb check, since the hold lights all eight lamps at the calibrated
 * codes. Pure timeline logic: the caller owns the clock (millis) and the
 * lamp writes; this header only answers "which lamps at elapsed t".
 *
 * COLUMN ORDER IS BOARD GEOMETRY, NOT SILK NUMBERING. The eight telltales
 * sit in four two-lamp columns, west cluster then east (Board3 layout,
 * board-frame x: TT1/TT5 at 65 mm, TT2/TT7 at 75, TT3/TT6 at 225, TT4/TT8
 * at 234.5). A bit-order sweep would hop across the board; this one
 * physically travels left to right. Lamp bit l = silk TT(l+1), as
 * everywhere else.
 *
 * The sweep is an OVERLAY like DashState.tt_forced: callers OR it over the
 * live mask, so an alarm lamp can never blink off mid-show.
 */
#ifndef DASH_TTSWEEP_H
#define DASH_TTSWEEP_H

#include <stdint.h>
#include <stdbool.h>

/* Pace: doubled from the first bench look (90/450), which read as a
 * flicker rather than a sweep on real LEDs -- the chase was over before
 * the eye caught the direction. At 180 ms the travel is legible. */
#define DASH_TTSWEEP_STEP_MS 180U /* one column per step */
#define DASH_TTSWEEP_HOLD_MS 900U /* full-row hold (the bulb check) */
#define DASH_TTSWEEP_COLS 4U

/* column -> lamp bits: (TT1|TT5), (TT2|TT7), (TT3|TT6), (TT4|TT8).
 * Pairwise disjoint and OR-complete (0x11|0x42|0x24|0x88 == 0xFF) -- the
 * host test pins both, so a re-layout that moves an LED forces this table
 * to be re-derived rather than silently sweeping the wrong shape. */
static const uint8_t DASH_TTSWEEP_COL[DASH_TTSWEEP_COLS] = {
    (uint8_t) ((1U << 0) | (1U << 4)), /* TT1 + TT5 */
    (uint8_t) ((1U << 1) | (1U << 6)), /* TT2 + TT7 */
    (uint8_t) ((1U << 2) | (1U << 5)), /* TT3 + TT6 */
    (uint8_t) ((1U << 3) | (1U << 7)), /* TT4 + TT8 */
};

/* L->R (4 steps) then a bounce back R->L (3 steps: the far column is not
 * repeated), then the hold */
#define DASH_TTSWEEP_STEPS (2U * DASH_TTSWEEP_COLS - 1U)
#define DASH_TTSWEEP_TOTAL_MS \
    ((uint32_t) DASH_TTSWEEP_STEPS * DASH_TTSWEEP_STEP_MS + DASH_TTSWEEP_HOLD_MS)

static inline bool dash_ttsweep_done(uint32_t elapsed_ms)
{
    return elapsed_ms >= DASH_TTSWEEP_TOTAL_MS;
}

static inline uint8_t dash_ttsweep_mask(uint32_t elapsed_ms)
{
    if (dash_ttsweep_done(elapsed_ms))
    {
        return 0U; /* over: the caller's live mask stands alone again */
    }
    const uint32_t step = elapsed_ms / DASH_TTSWEEP_STEP_MS;
    if (step >= DASH_TTSWEEP_STEPS)
    {
        return 0xFFU; /* the full-row hold */
    }
    const uint32_t col = (step < DASH_TTSWEEP_COLS)
                             ? step
                             : (2U * DASH_TTSWEEP_COLS - 2U - step);
    return DASH_TTSWEEP_COL[col];
}

#endif /* DASH_TTSWEEP_H */
