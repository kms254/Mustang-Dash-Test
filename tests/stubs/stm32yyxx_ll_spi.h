/*
 * Minimal stm32yyxx_ll_spi.h stub for host-compiling
 * EVE_target_Arduino_STM32_generic.h in tests/test_eve_pins.c.
 *
 * The real header ships with the STM32 Arduino core and pulls in the whole
 * device tree. The pin invariant only needs the target header to PARSE, so
 * this declares just the LL SPI surface that header's inline functions
 * reference -- nothing here is called, and the test is -fsyntax-only.
 *
 * Keep this in step with the target header: if a future EVE library revision
 * uses another LL_SPI_* call, the pin test stops compiling. That is the
 * intended behaviour -- it fails loudly rather than silently skipping, which
 * is the whole point of pinning the pins in the first place.
 */

#pragma once

#include <stdint.h>

typedef struct {
    volatile uint32_t placeholder;
} SPI_TypeDef;

static inline uint32_t LL_SPI_IsActiveFlag_TXP(SPI_TypeDef *spi)   { (void) spi; return 1U; }
static inline uint32_t LL_SPI_IsActiveFlag_TXE(SPI_TypeDef *spi)   { (void) spi; return 1U; }
static inline uint32_t LL_SPI_IsActiveFlag_RXP(SPI_TypeDef *spi)   { (void) spi; return 1U; }
static inline uint32_t LL_SPI_IsActiveFlag_RXNE(SPI_TypeDef *spi)  { (void) spi; return 1U; }
static inline void     LL_SPI_TransmitData8(SPI_TypeDef *spi, uint8_t d) { (void) spi; (void) d; }
static inline uint8_t  LL_SPI_ReceiveData8(SPI_TypeDef *spi)       { (void) spi; return 0U; }
