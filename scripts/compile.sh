#!/usr/bin/env bash
# Compile the MustangDash firmware for the STM32 target.
#
# The primary target is the NUCLEO-F767ZI three-panel mule; the H755 carrier
# (env h743) builds from the same source. Both are PlatformIO environments in
# platformio.ini, and PlatformIO resolves the toolchain and the STM32 Arduino
# core itself -- nothing to install by hand, no sketchbook to sync.
#
#   ./scripts/compile.sh                 # default env (nucleo_f767)
#   ./scripts/compile.sh h743            # the carrier target
#   ./scripts/compile.sh nucleo_f767 -v  # extra args pass through to pio
#
set -euo pipefail
cd "$(dirname "$0")/.."

ENV="${1:-nucleo_f767}"
[ $# -gt 0 ] && shift

# PlatformIO lives in its own venv on this workstation (bootstrapped from the
# portable Python the VS Code extension ships -- there is no system Python).
PIO="${PIO:-}"
if [ -z "$PIO" ]; then
    for candidate in \
        "$HOME/.platformio/penv/Scripts/pio.exe" \
        "$HOME/.platformio/penv/bin/pio" \
        "$(command -v pio 2>/dev/null || true)"
    do
        if [ -n "$candidate" ] && [ -x "$candidate" ]; then PIO="$candidate"; break; fi
    done
fi
if [ -z "$PIO" ]; then
    echo "pio not found. Set PIO=/path/to/pio, or install PlatformIO." >&2
    exit 1
fi

"$PIO" run -e "$ENV" "$@"

echo
echo "Built env '$ENV' -> .pio/build/$ENV/firmware.bin"
