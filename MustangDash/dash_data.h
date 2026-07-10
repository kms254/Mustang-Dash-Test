/* dash_data.h -- the dash's single data interface (KTD4).
 *
 * Pure header: stdint/stdbool only, no Arduino/EVE includes, host-testable.
 * Producers (dash_sim.h now, CAN decoders later) fill DashState.ch and set
 * validity bits; renderers read only this struct. An invalid channel renders
 * `--` (text) or the dead-front convention (graphics) and can never assert
 * an alarm (R11).
 *
 * Override semantics (KTD6):
 *  - `set <ch> <v>`  -> value written, valid bit set, override bit set;
 *                       the simulator must not touch overridden channels.
 *  - `clear <ch>`    -> valid bit dropped, cleared bit set; stays invalid
 *                       across sim steps until `sim on` or a new `set`.
 *  - `sim on`        -> override and cleared masks wiped; sim owns all
 *                       channels again. `sim off` freezes stepping entirely.
 */

#ifndef DASH_DATA_H
#define DASH_DATA_H

#include <stdint.h>
#include <stdbool.h>

/* Channel ids -- one bit each in the valid/override/cleared masks.
 * Phase-1 set only (plan KTD4): channels with no current renderer or alarm
 * consumer are deliberately absent. */
enum {
    DASH_CH_RPM = 0,
    DASH_CH_SPEED,   /* MPH */
    DASH_CH_ECT,     /* coolant, degF */
    DASH_CH_OILT,    /* oil temperature, degF */
    DASH_CH_OILP,    /* oil pressure, psi */
    DASH_CH_VOLTS,
    DASH_CH_FUEL,    /* gallons */
    DASH_CH_DELTA,   /* lap delta, seconds, negative = ahead */
    DASH_CH_LAP,     /* current lap time, ms */
    DASH_CH_LAST,    /* last lap, ms */
    DASH_CH_BEST,    /* best lap, ms */
    DASH_CH_AMBIENT, /* degF */
    DASH_CH_COUNT
};

#define DASH_CH_BIT(ch) ((uint16_t) (1U << (ch)))
#define DASH_CH_ALL ((uint16_t) ((1U << DASH_CH_COUNT) - 1U))

typedef enum {
    DASH_MODE_TRACK = 0,
    DASH_MODE_STREET = 1,
} DashMode;

typedef struct {
    float rpm;
    float speed_mph;
    float ect_f;
    float oil_temp_f;
    float oil_press_psi;
    float volts;
    float fuel_gal;
    float delta_s;
    uint32_t lap_ms;
    uint32_t last_ms;
    uint32_t best_ms;
    float ambient_f;
} DashChannels;

typedef struct {
    DashChannels ch;
    uint16_t valid;      /* DASH_CH_BIT() per channel */
    uint16_t overridden; /* serial `set` freezes these against the sim */
    uint16_t cleared;    /* serial `clear` holds these invalid */
    DashMode mode;
    bool sim_frozen;     /* `sim off`: hold current values, stop stepping */
} DashState;

static inline bool dash_ch_valid(const DashState *s, uint8_t ch)
{
    return (s->valid & DASH_CH_BIT(ch)) != 0U;
}

/* True when the simulator may write this channel this step. */
static inline bool dash_ch_sim_owned(const DashState *s, uint8_t ch)
{
    return ((s->overridden | s->cleared) & DASH_CH_BIT(ch)) == 0U;
}

static inline void dash_ch_set(DashState *s, uint8_t ch, float v)
{
    switch (ch) {
        case DASH_CH_RPM: s->ch.rpm = v; break;
        case DASH_CH_SPEED: s->ch.speed_mph = v; break;
        case DASH_CH_ECT: s->ch.ect_f = v; break;
        case DASH_CH_OILT: s->ch.oil_temp_f = v; break;
        case DASH_CH_OILP: s->ch.oil_press_psi = v; break;
        case DASH_CH_VOLTS: s->ch.volts = v; break;
        case DASH_CH_FUEL: s->ch.fuel_gal = v; break;
        case DASH_CH_DELTA: s->ch.delta_s = v; break;
        case DASH_CH_LAP: s->ch.lap_ms = (uint32_t) v; break;
        case DASH_CH_LAST: s->ch.last_ms = (uint32_t) v; break;
        case DASH_CH_BEST: s->ch.best_ms = (uint32_t) v; break;
        case DASH_CH_AMBIENT: s->ch.ambient_f = v; break;
        default: return;
    }
    s->valid |= DASH_CH_BIT(ch);
}

static inline float dash_ch_get(const DashState *s, uint8_t ch)
{
    switch (ch) {
        case DASH_CH_RPM: return s->ch.rpm;
        case DASH_CH_SPEED: return s->ch.speed_mph;
        case DASH_CH_ECT: return s->ch.ect_f;
        case DASH_CH_OILT: return s->ch.oil_temp_f;
        case DASH_CH_OILP: return s->ch.oil_press_psi;
        case DASH_CH_VOLTS: return s->ch.volts;
        case DASH_CH_FUEL: return s->ch.fuel_gal;
        case DASH_CH_DELTA: return s->ch.delta_s;
        case DASH_CH_LAP: return (float) s->ch.lap_ms;
        case DASH_CH_LAST: return (float) s->ch.last_ms;
        case DASH_CH_BEST: return (float) s->ch.best_ms;
        case DASH_CH_AMBIENT: return s->ch.ambient_f;
        default: return 0.0f;
    }
}

/* Boot state: everything invalid, sim running, TRACK default (KTD3). */
static inline void dash_state_init(DashState *s)
{
    const DashState zero = {0};
    *s = zero;
    s->mode = DASH_MODE_TRACK;
}

#endif /* DASH_DATA_H */
