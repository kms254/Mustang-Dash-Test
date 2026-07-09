#!/usr/bin/env bash
# Compile the MustangDash sketch for the Teensy 4.1.
#
# Requires arduino-cli with a Teensy platform installed under the FQBN
# teensy:avr:teensy41 (Teensyduino, or the minimal offline platform described
# in BUILD.md). The vendored EVE library in ./libraries is passed explicitly so
# you do not have to copy it into your Arduino sketchbook.
set -euo pipefail
cd "$(dirname "$0")/.."

FQBN="teensy:avr:teensy41"
OUT="./build"

arduino-cli compile \
    --clean \
    -b "$FQBN" \
    --libraries ./libraries \
    --output-dir "$OUT" \
    ./MustangDash

echo
echo "Built: $OUT/MustangDash.ino.hex"
