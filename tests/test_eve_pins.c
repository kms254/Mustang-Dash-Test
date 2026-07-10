/*
 * Invariant test: the Teensy 4.1 control pins for the EVE display must stay
 * CS = 14 and PD/RST = 17 -- that is how the hardware is physically wired
 * (SCLK=13, MISO=12, MOSI=11 are the fixed SPI0 pins).
 *
 * Compiles the vendored Teensy 4 target header on the host with a minimal
 * Arduino.h stub (tests/stubs/):
 *   gcc -std=c11 -fsyntax-only -DARDUINO=10819 -DARDUINO_TEENSY41 \
 *       -I tests/stubs -I libraries/FT800-FT813/src tests/test_eve_pins.c
 */

#include "EVE_target/EVE_target_Arduino_Teensy4.h"

_Static_assert(EVE_CS == 14, "EVE chip-select must be Teensy pin 14 (hardware wiring)");
_Static_assert(EVE_PDN == 17, "EVE power-down/reset must be Teensy pin 17 (hardware wiring)");

int main(void) { return 0; }
