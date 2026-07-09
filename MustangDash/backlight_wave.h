/*
 * backlight_wave.h - pure triangle-wave stepper for the backlight demo.
 *
 * No Arduino or EVE dependencies so the logic is host-testable
 * (see tests/test_backlight_wave.c). Given the current duty and a step
 * (magnitude + direction), returns the next duty clamped to [lo, hi] and
 * flips the step's direction at either bound, preserving its magnitude.
 */

#ifndef BACKLIGHT_WAVE_H
#define BACKLIGHT_WAVE_H

#include <stdint.h>

static inline uint8_t bl_wave_next(uint8_t duty, int8_t *step, uint8_t lo, uint8_t hi)
{
    int next = (int)duty + (int)*step;
    int8_t mag = (int8_t)((*step < 0) ? -*step : *step);

    if (next >= (int)hi)
    {
        next = (int)hi;
        *step = (int8_t)-mag; /* bounce off the top: dim from here */
    }
    if (next <= (int)lo)
    {
        next = (int)lo;
        *step = mag; /* bounce off the bottom: brighten from here */
    }
    return (uint8_t)next;
}

#endif /* BACKLIGHT_WAVE_H */
