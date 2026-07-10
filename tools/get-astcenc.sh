#!/usr/bin/env bash
# get-astcenc.sh - fetch the pinned astcenc release binary for the splash
# ASTC converter (tools/make_splash_flash.py).
#
# Downloads ARM's astc-encoder release zip, verifies its sha256, and extracts
# the sse2 variant (max compatibility; determinism does not depend on the
# SIMD variant, but the pinned binary is part of the reproducibility contract)
# into tools/.astcenc/ (gitignored).
#
# Pinned: astcenc 4.6.1 linux-x64.
#   4.6.1 is the newest 4.x release whose binaries still run on Ubuntu 20.04
#   (glibc 2.31); 4.7.0+ require GLIBC_2.34. This box's WSL is 20.04.
#
# Run under WSL:  wsl -- bash tools/get-astcenc.sh

set -euo pipefail

ASTCENC_VERSION="4.6.1"
ASTCENC_URL="https://github.com/ARM-software/astc-encoder/releases/download/4.6.1/astcenc-4.6.1-linux-x64.zip"
ASTCENC_ZIP_SHA256="e360aeabf3b5aeda6a7cfabddc49af8b204e28befa04ab8e8942c85620ba071a"
ASTCENC_BIN_SHA256="b5c5ad4a330125f75721ebec355c0c6327454983868893db3302d9e362806ffe"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST_DIR="$ROOT/tools/.astcenc"
DEST_BIN="$DEST_DIR/astcenc-sse2"
ZIP="$DEST_DIR/astcenc-$ASTCENC_VERSION-linux-x64.zip"

sha256_of() { sha256sum "$1" | cut -d' ' -f1; }

if [ -x "$DEST_BIN" ] && [ "$(sha256_of "$DEST_BIN")" = "$ASTCENC_BIN_SHA256" ]; then
    echo "astcenc $ASTCENC_VERSION already present: $DEST_BIN"
    exit 0
fi

mkdir -p "$DEST_DIR"

echo "Downloading astcenc $ASTCENC_VERSION ..."
if ! curl -fSL --retry 3 -o "$ZIP" "$ASTCENC_URL"; then
    echo "ERROR: download failed: $ASTCENC_URL" >&2
    echo "Fetch the zip manually, verify sha256 $ASTCENC_ZIP_SHA256," >&2
    echo "and place bin/astcenc-sse2 at $DEST_BIN" >&2
    exit 1
fi

got="$(sha256_of "$ZIP")"
if [ "$got" != "$ASTCENC_ZIP_SHA256" ]; then
    echo "ERROR: sha256 mismatch for $ZIP" >&2
    echo "  expected: $ASTCENC_ZIP_SHA256" >&2
    echo "  got:      $got" >&2
    rm -f "$ZIP"
    exit 1
fi

# Extract with python3's zipfile: WSL Ubuntu here has no `unzip`.
python3 - "$ZIP" "$DEST_DIR" <<'EOF'
import sys, zipfile
zip_path, dest = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(zip_path) as z:
    with z.open("bin/astcenc-sse2") as src, open(dest + "/astcenc-sse2", "wb") as out:
        out.write(src.read())
EOF
chmod +x "$DEST_BIN"

got="$(sha256_of "$DEST_BIN")"
if [ "$got" != "$ASTCENC_BIN_SHA256" ]; then
    echo "ERROR: sha256 mismatch for extracted $DEST_BIN" >&2
    echo "  expected: $ASTCENC_BIN_SHA256" >&2
    echo "  got:      $got" >&2
    exit 1
fi

rm -f "$ZIP"
"$DEST_BIN" -version | head -1
echo "OK: $DEST_BIN"
