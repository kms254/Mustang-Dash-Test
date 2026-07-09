#!/usr/bin/env bash
# Host-side invariant tests -- no board, no arduino-cli, no network needed.
# These pin the things that must absolutely not change:
#   - the EVE display profile (EVE_RVT70H: 1024x600, BT817, EVE4)
#   - the hardware control pins (CS=14, PD/RST=17)
#   - the backlight triangle-wave bounds and period
#   - the ctags shim's no-op contract
# The firmware build itself is verified separately by scripts/compile.sh.
set -euo pipefail
cd "$(dirname "$0")/.."

CC="${CC:-gcc}"
LIB=libraries/FT800-FT813/src
PASS=0

echo "1/4 EVE profile invariants (EVE_config.h)"
"$CC" -std=c11 -Wall -Werror -fsyntax-only -I "$LIB" tests/test_eve_config.c
echo "    OK"; PASS=$((PASS + 1))

echo "2/4 pin invariants (EVE_target_Arduino_Teensy4.h)"
"$CC" -std=c11 -Wall -Werror -fsyntax-only -DARDUINO=10819 -DARDUINO_TEENSY41 \
    -I tests/stubs -I "$LIB" tests/test_eve_pins.c
echo "    OK"; PASS=$((PASS + 1))

echo "3/4 backlight wave behavior (backlight_wave.h)"
BIN="$(mktemp)"
"$CC" -std=c11 -Wall -Werror -I MustangDash tests/test_backlight_wave.c -o "$BIN"
"$BIN"; rm -f "$BIN"
PASS=$((PASS + 1))

echo "4/4 ctags shim no-op contract"
bash tests/test_ctags_shim.sh
PASS=$((PASS + 1))

echo
echo "All $PASS/4 invariant tests passed."
