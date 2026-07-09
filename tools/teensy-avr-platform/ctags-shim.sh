#!/bin/sh
# No-op ctags shim for offline arduino-cli setup.
# Arduino .ino auto-prototype generation needs the arduino-ctags fork (emits
# function return types); it is not available offline. Emitting empty tags makes
# arduino-cli insert NO auto-prototypes, so sketches declare their own prototypes.
out=""; prev=""
for arg in "$@"; do
  if [ "$prev" = "-f" ]; then out="$arg"; fi
  prev="$arg"
done
if [ -n "$out" ] && [ "$out" != "-" ]; then : > "$out"; fi
exit 0
