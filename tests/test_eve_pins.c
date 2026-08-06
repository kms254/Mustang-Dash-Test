/*
 * Invariant test: the EVE display control pins must stay CS = 10 and
 * PD/RST = 8 on the STM32 target -- that is how the NUCLEO-F767ZI mule is
 * physically wired, and the H755 carrier follows it.
 *
 * Compiles the vendored STM32 target header on the host with a minimal
 * Arduino.h stub (tests/stubs/):
 *   gcc -std=c11 -fsyntax-only -DARDUINO=10819 -DARDUINO_ARCH_STM32 \
 *       -I tests/stubs -I libraries/FT800-FT813/src tests/test_eve_pins.c
 *
 * These are the DEFAULT pins the header falls back to; both are guarded by
 * `#if !defined`, so a -D on the build line still overrides them. Pinning the
 * fallback is what catches an unconfigured build silently changing wiring.
 *
 */

#include "EVE_target/EVE_target_Arduino_STM32_generic.h"

_Static_assert(EVE_CS == 10, "EVE chip-select must be pin 10 (hardware wiring)");
_Static_assert(EVE_PDN == 8, "EVE power-down/reset must be pin 8 (hardware wiring)");

int main(void) { return 0; }
