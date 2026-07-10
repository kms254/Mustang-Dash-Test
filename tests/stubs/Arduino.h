/*
 * Minimal Arduino.h stub for host-compiling EVE_target_Arduino_Teensy4.h in
 * tests/test_eve_pins.c. Only the symbols that header's static inline
 * functions reference. Not a general-purpose stub.
 */

#ifndef ARDUINO_STUB_H
#define ARDUINO_STUB_H

#include <stdint.h>

#define LOW 0
#define HIGH 1

#ifdef __cplusplus
extern "C" {
#endif

void digitalWrite(uint8_t pin, uint8_t val);
void delay(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* ARDUINO_STUB_H */
