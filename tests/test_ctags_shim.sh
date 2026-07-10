#!/usr/bin/env bash
# Invariant test: the ctags no-op shim must stay a safe no-op.
#
# Contract (see docs/solutions/tooling-decisions/offline-teensy41-*.md):
#   - always exits 0 (a failing ctags aborts every arduino-cli build)
#   - with `-f <file>`, truncates/creates <file> EMPTY (empty tags = arduino-cli
#     inserts no auto-prototypes; a non-empty or missing file changes behavior)
#   - with `-f -` (stdout) or no -f at all, writes no file
#   - never touches anything else in the working directory
set -u
SHIM="$(cd "$(dirname "$0")/.." && pwd)/tools/teensy-avr-platform/ctags-shim.sh"
FAILURES=0

fail() { echo "FAIL: $1" >&2; FAILURES=$((FAILURES + 1)); }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

# 1. arduino-cli's usual shape: ctags <flags...> -f <out> <source>
sh "$SHIM" -u --language-force=c++ -f out.tags --c++-kinds=svpf sketch.cpp
[ $? -eq 0 ] || fail "exit nonzero for -f out.tags"
[ -f out.tags ] || fail "-f out.tags did not create the tags file"
[ -s out.tags ] && fail "-f out.tags produced a NON-empty tags file (would re-enable auto-prototypes)"

# 2. existing non-empty target must be truncated to empty
echo "stale content" > stale.tags
sh "$SHIM" -f stale.tags sketch.cpp
[ $? -eq 0 ] || fail "exit nonzero for existing target"
[ -s stale.tags ] && fail "existing tags file was not truncated to empty"

# 3. -f - (stdout) must not create a file named '-'
sh "$SHIM" -f - sketch.cpp
[ $? -eq 0 ] || fail "exit nonzero for -f -"
[ -e ./- ] && fail "created a file literally named '-'"

# 4. no -f at all: still exit 0, create nothing new
before="$(ls | sort)"
sh "$SHIM" --version
[ $? -eq 0 ] || fail "exit nonzero with no -f"
after="$(ls | sort)"
[ "$before" = "$after" ] && true || fail "created unexpected files with no -f"

if [ "$FAILURES" -eq 0 ]; then
    echo "OK: ctags shim honors its no-op contract"
    exit 0
fi
exit 1
