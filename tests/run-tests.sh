#!/usr/bin/env bash
# Host-side invariant tests -- no board, no arduino-cli, no network needed.
# These pin the things that must absolutely not change:
#   - the EVE display profile (EVE_RVT70H: 1024x600, BT817, EVE4)
#   - the hardware control pins (CS=10, PD/RST=8 on the STM32 target)
#   - the backlight triangle-wave bounds and period
#   - the boot-splash timeline (windows, easing, endpoints, overshoot bound)
#   - the trip/mode button gesture split + its 30 ms debounce
# The firmware build itself is verified separately by scripts/compile.sh.
set -euo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-gcc}"
LIB=libraries/FT800-FT813/src
PASS=0

echo "1/14 EVE profile invariants (EVE_config.h)"
"$CC" -std=c11 -Wall -Werror -fsyntax-only -I "$LIB" tests/test_eve_config.c
echo "    OK"; PASS=$((PASS + 1))

echo "2/14 pin invariants (EVE_target_Arduino_STM32_generic.h)"
"$CC" -std=c11 -Wall -Werror -fsyntax-only -DARDUINO=10819 -DARDUINO_ARCH_STM32 \
    -I tests/stubs -I "$LIB" tests/test_eve_pins.c
echo "    OK"; PASS=$((PASS + 1))

echo "3/14 backlight wave behavior (backlight_wave.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_backlight_wave.c -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "4/14 splash timeline behavior (splash_timeline.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_splash_timeline.c -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "5/14 dash math (dash_math.h, dash_layout.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_dash_math.c -lm -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "6/14 dash simulator (dash_sim.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_dash_sim.c -lm -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "7/14 dash serial protocol (dash_serial.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_dash_serial.c -lm -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "8/14 odometer record (dash_odo.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_dash_odo.c -lm -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "9/14 dash font invariants (dash_fonts.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_dash_fonts.c -lm -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "10/14 splash flash pack invariants (splash_flash.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_splash_flash.c -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "11/14 dash panel descriptors (dash_panels.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_dash_panels.c -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "12/14 telltale mask (dash_telltales.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_dash_telltales.c -lm -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "13/14 trip/mode button gestures (dash_button.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_dash_button.c -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "14/14 telltale brightness calibration (dash_calibration.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_dash_calibration.c -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo
echo "All $PASS/14 invariant tests passed."
