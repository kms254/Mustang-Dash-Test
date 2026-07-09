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

# Sync the committed platform definition into the sketchbook before building,
# so the tracked files in tools/teensy-avr-platform/ are the source of truth
# the compile actually exercises (not a stale manual copy). Skipped when the
# sketchbook has no offline platform dir (e.g. a real Teensyduino install).
SB="$(arduino-cli config get directories.user 2>/dev/null || true)"
PLATFORM_DIR="$SB/hardware/teensy/avr"
if [ -n "$SB" ] && [ -d "$PLATFORM_DIR/cores/teensy4" ]; then
    install -m 0644 tools/teensy-avr-platform/boards.txt "$PLATFORM_DIR/boards.txt"
    install -m 0644 tools/teensy-avr-platform/platform.txt "$PLATFORM_DIR/platform.txt"
    # The no-op ctags shim lives inside the platform (see platform.txt's
    # runtime.tools.ctags.path={runtime.platform.path}/tools-bin).
    install -m 0755 -D tools/teensy-avr-platform/ctags-shim.sh "$PLATFORM_DIR/tools-bin/ctags"
fi

arduino-cli compile \
    --clean \
    -b "$FQBN" \
    --libraries ./libraries \
    --output-dir "$OUT" \
    ./MustangDash

echo
echo "Built: $OUT/MustangDash.ino.hex"
