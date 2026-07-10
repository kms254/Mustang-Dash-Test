/*
@file    EVE_target_Arduino_Teensy4.h
@brief   target specific includes, definitions and functions
@version 5.0
@date    2023-06-24
@author  Rudolph Riedel

@section LICENSE

MIT License

Copyright (c) 2016-2023 Rudolph Riedel

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the Software
is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

@section History

5.0
- extracted from EVE_target.h
- basic maintenance: checked for violations of white space and indent rules
- split up the optional default defines to allow to only change what needs
    changing thru the build-environment
- changed #include "EVE_cpp_wrapper.h" to #include "../EVE_cpp_wrapper.h"
- added ARDUINO_TEENSY40 to the Teensy 4 target

*/

#ifndef EVE_TARGET_ARDUINO_TEENSY4_H
#define EVE_TARGET_ARDUINO_TEENSY4_H

#if defined (ARDUINO)

#include <stdint.h>
#include <stddef.h> /* NULL, used by the multi-panel pin resolvers below */
#include <Arduino.h>
#include "../EVE_cpp_wrapper.h"

#ifdef __cplusplus
extern "C"
{
#endif

#if defined (ARDUINO_TEENSY41) || defined (ARDUINO_TEENSY40)

/* you may define these in your build-environment to use different settings */
/* Mustang Dash / Teensy 4.1 wiring: CS = pin 14, PD/RST = pin 17 */
#if !defined (EVE_CS)
#define EVE_CS 14
#endif

#if !defined (EVE_PDN)
#define EVE_PDN 17
#endif
/* you may define these in your build-environment to use different settings */

/* multi-panel support: drive more than one panel on the same SPI bus with */
/* individual CS / PD lines and individual display parameters */
#define EVE_MULTI_PANEL

#define EVE_PANEL_SLOTS 4U /* number of fault-state slots, one per panel */

typedef struct EVE_panel
{
    uint8_t cs_pin;     /* chip-select pin for this panel */
    uint8_t pdn_pin;    /* power-down / reset pin for this panel */
    uint8_t slot;       /* fault-state slot, 0 to (EVE_PANEL_SLOTS - 1) */
    uint8_t pclk;       /* pixel-clock divider, only used when pclk_freq is 0 */
    uint16_t pclk_freq; /* value for REG_PCLK_FREQ, 0 = use the pclk divider instead */
    uint16_t hsize;     /* active display width */
    uint16_t vsize;     /* active display height */
    uint16_t hcycle;    /* total number of clocks per line, incl front/back porch */
    uint16_t hoffset;   /* start of active line */
    uint16_t hsync0;    /* start of horizontal sync pulse */
    uint16_t hsync1;    /* end of horizontal sync pulse */
    uint16_t vcycle;    /* total number of lines per screen, including pre/post */
    uint16_t voffset;   /* start of active screen */
    uint16_t vsync0;    /* start of vertical sync pulse */
    uint16_t vsync1;    /* end of vertical sync pulse */
    uint8_t swizzle;    /* FT8xx output to LCD - pin order */
    uint8_t pclkpol;    /* LCD data is clocked in on this PCLK edge */
    uint8_t cspread;    /* helps with noise, when set to 1 fewer signals are changed simultaneously */
} EVE_panel_t;

extern const EVE_panel_t *EVE_active_panel; /* selected panel, NULL = compile-time configuration */

#define EVE_DMA

#if defined (EVE_DMA)
extern uint32_t EVE_dma_buffer[1025U];
extern volatile uint16_t EVE_dma_buffer_index;
extern volatile uint8_t EVE_dma_busy;

void EVE_init_dma(void);
void EVE_start_dma_transfer(void);
#endif

#define DELAY_MS(ms) delay(ms)

/* multi-panel support: resolve the pins of the selected panel, */
/* fall back to the compile-time defaults when no panel is selected */
static inline uint8_t EVE_cs_pin(void)
{
    return ((NULL == EVE_active_panel) ? ((uint8_t) EVE_CS) : EVE_active_panel->cs_pin);
}

static inline uint8_t EVE_pdn_pin(void)
{
    return ((NULL == EVE_active_panel) ? ((uint8_t) EVE_PDN) : EVE_active_panel->pdn_pin);
}

static inline void EVE_pdn_set(void)
{
    digitalWrite(EVE_pdn_pin(), LOW); /* go into power-down */
}

static inline void EVE_pdn_clear(void)
{
    digitalWrite(EVE_pdn_pin(), HIGH); /* power up */
}

static inline void EVE_cs_set(void)
{
    digitalWrite(EVE_cs_pin(), LOW); /* make EVE listen */
}

static inline void EVE_cs_clear(void)
{
    digitalWrite(EVE_cs_pin(), HIGH); /* tell EVE to stop listen */
}

static inline void spi_transmit(uint8_t data)
{
    wrapper_spi_transmit(data);
}

static inline void spi_transmit_32(uint32_t data)
{
    spi_transmit((uint8_t)(data & 0x000000ffUL));
    spi_transmit((uint8_t)(data >> 8U));
    spi_transmit((uint8_t)(data >> 16U));
    spi_transmit((uint8_t)(data >> 24U));
}

/* spi_transmit_burst() is only used for cmd-FIFO commands */
/* so it *always* has to transfer 4 bytes */
static inline void spi_transmit_burst(uint32_t data)
{
#if defined (EVE_DMA)
    EVE_dma_buffer[EVE_dma_buffer_index++] = data;
#else
    spi_transmit_32(data);
#endif
}

static inline uint8_t spi_receive(uint8_t data)
{
    return (wrapper_spi_receive(data));
}

static inline uint8_t fetch_flash_byte(const uint8_t *p_data)
{
    return (*p_data);
}

#endif /* Teensy41 */

#ifdef __cplusplus
}
#endif

#endif /* Arduino */

#endif /* EVE_TARGET_ARDUINO_TEENSY4_H */
