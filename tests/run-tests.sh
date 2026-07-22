#!/usr/bin/env bash
# Host-side invariant tests -- no board, no arduino-cli, no network needed.
# These pin the things that must absolutely not change:
#   - the EVE display profile (EVE_RVT70H: 1024x600, BT817, EVE4)
#   - the hardware control pins (CS=14, PD/RST=17)
#   - the backlight triangle-wave bounds and period
#   - the boot-splash timeline (windows, easing, endpoints, overshoot bound)
#   - the ctags shim's no-op contract
# The firmware build itself is verified separately by scripts/compile.sh.
set -euo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-gcc}"
LIB=libraries/FT800-FT813/src
PASS=0

echo "1/13 EVE profile invariants (EVE_config.h)"
"$CC" -std=c11 -Wall -Werror -fsyntax-only -I "$LIB" tests/test_eve_config.c
echo "    OK"; PASS=$((PASS + 1))

echo "2/13 pin invariants (EVE_target_Arduino_Teensy4.h)"
"$CC" -std=c11 -Wall -Werror -fsyntax-only -DARDUINO=10819 -DARDUINO_TEENSY41 \
    -I tests/stubs -I "$LIB" tests/test_eve_pins.c
echo "    OK"; PASS=$((PASS + 1))

echo "3/13 backlight wave behavior (backlight_wave.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_backlight_wave.c -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "4/13 splash timeline behavior (splash_timeline.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_splash_timeline.c -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "5/13 ctags shim no-op contract"
bash tests/test_ctags_shim.sh
PASS=$((PASS + 1))

echo "6/13 dash math (dash_math.h, dash_layout.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_dash_math.c -lm -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "7/13 dash simulator (dash_sim.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_dash_sim.c -lm -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "8/13 dash serial protocol (dash_serial.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_dash_serial.c -lm -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "9/13 odometer record (dash_odo.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_dash_odo.c -lm -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "10/13 dash font invariants (dash_fonts.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_dash_fonts.c -lm -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "11/13 splash flash pack invariants (splash_flash.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_splash_flash.c -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "12/13 dash panel descriptors (dash_panels.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_dash_panels.c -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "13/13 telltale mask (dash_telltales.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_dash_telltales.c -lm -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo
echo "All $PASS/13 invariant tests passed."
