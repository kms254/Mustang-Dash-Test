#!/usr/bin/env python3
"""Build the boot-splash ASTC flash image and its firmware header.

Reads the vendored design export in assets/splash/, compresses every asset
to ASTC with the pinned astcenc binary (tools/get-astcenc.sh), lays the
results out as a single flash-image "pack", and emits:

  MustangDash/splash_flash.h   address table + the embedded provisioning pack

The firmware provisions the pack into the panel's QSPI flash once (compare
header, CMD_FLASHUPDATE on mismatch) and then renders every splash asset
directly from flash -- RAM_G is no longer used by the splash.

Pipeline (BRT_AN_033 BT81X Series Programming Guide, v2.8):
  * Backgrounds (3): full-res 1024x600. The gradient is reconstructed in
    float to kill 8-bit banding before compression: two passes of the
    edge-clamped box blur (radius 12) on float32 RGB, no ordered dither
    (Bayer noise fights block compression). Encoded ASTC 8x8.
  * All other assets: RGBA passed to the encoder as-is. Encoded ASTC 4x4.
  * Encoder: astcenc -cl <in> <out> {4x4|8x8} -thorough -j 1 -silent
    (fixed flags, single-threaded: output is deterministic for the pinned
    binary; re-running must produce a byte-identical header).
  * The 16-byte .astc container header is stripped; the raw blocks are then
    reordered into EVE's 2x2 tile order (guide section 6.1 "ASTC RAM
    Layout": tiles of 2x2 blocks stored TL,BL,BR,TR; an odd trailing column
    packs 1x2 top-then-bottom; an odd trailing row is linear). astcenc
    emits linear raster block order, so this swizzle is mandatory or the
    panel renders scrambled blocks.

Flash image layout:
  * BASE = flash address 4096. Sector 0 (0..4095) holds the vendor-
    programmed flashfast BLOB and is NEVER written.
  * 64-byte pack header at BASE: magic "MDSH", version, pack length,
    asset count, crc32 of everything after the header, zero padding.
  * Each asset 64-byte aligned (guide 6.2 "ASTC Flash Layout": ASTC bitmaps
    in flash must be 64-byte aligned; this also satisfies BITMAP_SOURCE's
    32-byte flash block addressing).
  * Total padded to a multiple of 4096 (CMD_FLASHUPDATE granularity,
    guide 5.82).

Run under WSL (Pillow + numpy, like tools/pony.py; astcenc via
tools/get-astcenc.sh first):
  wsl -- python3 tools/make_splash_flash.py
"""

import binascii
import io
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    import numpy as np
    from PIL import Image
except ImportError as exc:  # fail loudly with instructions
    sys.exit(
        f"ERROR: {exc}\n"
        "Pillow + numpy are required (same environment as tools/pony.py).\n"
        "Under WSL: python3 -m pip install pillow numpy"
    )

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets" / "splash"
OUT = ROOT / "MustangDash" / "splash_flash.h"
ASTCENC = ROOT / "tools" / ".astcenc" / "astcenc-sse2"

FLASH_BASE = 4096
HEADER_SIZE = 64
ASSET_ALIGN = 64  # guide 6.2: ASTC bitmaps in flash must be 64-byte aligned
SECTOR = 4096  # CMD_FLASHUPDATE granularity
MAGIC = b"MDSH"
VERSION = 1

BG_BLUR_RADIUS = 12  # full-res px per box pass; two passes

# (c_name, source file, astc block footprint, is_background)
# Grouping mirrors the firmware ThemeDesc tables: common assets first, then
# blue / red / checkered theme sets.
ASSET_LIST = [
    ("emblem", "emblem-200x200.png", "4x4", False),
    ("wordmark", "wordmark-mustang-700x80.png", "4x4", False),
    ("bars_chrome", "bars-chrome-240x45.png", "4x4", False),
    ("bg_blue", "bg-blue-1024x600.png", "4x4", True),  # quality trial 2026-07-21: fits RAM_G only while no street layer stages
    ("line_blue", "line-blue-340x40.png", "4x4", False),
    ("year_blue", "year-1965-blue.png", "4x4", False),
    ("bg_red", "bg-red-1024x600.png", "4x4", True),  # 4x4 trial 2026-07-21 (same budget rule as bg_blue)
    ("line_red", "line-red-340x40.png", "4x4", False),
    ("year_red", "year-1965-red.png", "4x4", False),
    ("bg_checkered", "bg-checkered-1024x600.png", "6x6", True),
    ("checker_block", "checker-block-240x52.png", "4x4", False),
    ("checker_line", "checker-line-300x14.png", "4x4", False),
    ("checker_strip", "checker-strip-1024x26.png", "4x4", False),
    ("year_checkered", "year-1965-checkered.png", "4x4", False),
]

EVE_FMT = {"4x4": "EVE_ASTC_4X4", "6x6": "EVE_ASTC_6X6", "8x8": "EVE_ASTC_8X8"}


def _box_blur_axis(arr, radius, axis):
    """Edge-clamped box blur along one axis via cumulative sums (float).

    Same kernel as the retired PNG pipeline (tools/make_splash_assets.py
    history); kept float-exact and fully deterministic.
    """
    n = arr.shape[axis]
    idx = np.clip(np.arange(-radius, n + radius), 0, n - 1)
    padded = np.take(arr, idx, axis=axis)
    csum = np.cumsum(padded, axis=axis, dtype=np.float64)
    zero_shape = list(csum.shape)
    zero_shape[axis] = 1
    csum = np.concatenate([np.zeros(zero_shape, csum.dtype), csum], axis=axis)
    k = 2 * radius + 1
    hi = np.take(csum, np.arange(k, k + n), axis=axis)
    lo = np.take(csum, np.arange(0, n), axis=axis)
    return ((hi - lo) / k).astype(np.float32)


def smooth_background(im):
    """Full-res float gradient reconstruction: 2x edge-clamped box blur.

    The source PNGs carry 8-bit plateau bands on their dark radial
    gradients; a ~50 px triangular kernel (two radius-12 box passes) is far
    wider than those plateaus and harmless on an already-soft vignette.
    No dither: ordered noise fights ASTC's block encoder, and 8x8 blocks
    over a smooth float-reconstructed gradient band far less than the
    quantized source. Returns opaque RGBA.
    """
    src = np.asarray(im.convert("RGB"), dtype=np.float32)
    for _ in range(2):
        src = _box_blur_axis(src, BG_BLUR_RADIUS, axis=0)
        src = _box_blur_axis(src, BG_BLUR_RADIUS, axis=1)
    rgb = np.clip(np.round(src), 0.0, 255.0).astype(np.uint8)
    h, w, _ = rgb.shape
    rgba = np.dstack([rgb, np.full((h, w, 1), 255, dtype=np.uint8)])
    return Image.fromarray(rgba, "RGBA")


def run_astcenc(png_path, astc_path, block):
    cmd = [
        str(ASTCENC), "-cl", str(png_path), str(astc_path), block,
        "-thorough", "-j", "1", "-silent",
    ]
    try:
        subprocess.run(cmd, check=True, capture_output=True, text=True)
    except FileNotFoundError:
        sys.exit(
            f"ERROR: astcenc not found at {ASTCENC}\n"
            "Fetch the pinned binary first:  wsl -- bash tools/get-astcenc.sh"
        )
    except subprocess.CalledProcessError as exc:
        sys.exit(f"ERROR: astcenc failed on {png_path}:\n{exc.stderr}")


def parse_astc(path, want_w, want_h, block):
    """Strip the 16-byte .astc header; validate footprint and payload size."""
    data = path.read_bytes()
    if len(data) < 16 or data[:4] != b"\x13\xab\xa1\x5c":
        sys.exit(f"ERROR: {path} is not a .astc file")
    bx, by, bz = data[4], data[5], data[6]
    dim_x = int.from_bytes(data[7:10], "little")
    dim_y = int.from_bytes(data[10:13], "little")
    dim_z = int.from_bytes(data[13:16], "little")
    want_bx, want_by = (int(v) for v in block.split("x"))
    if (bx, by, bz) != (want_bx, want_by, 1):
        sys.exit(f"ERROR: {path}: block {bx}x{by}x{bz}, wanted {block}x1")
    if (dim_x, dim_y, dim_z) != (want_w, want_h, 1):
        sys.exit(f"ERROR: {path}: dims {dim_x}x{dim_y}x{dim_z}, wanted {want_w}x{want_h}x1")
    payload = data[16:]
    bcols = -(-want_w // want_bx)
    brows = -(-want_h // want_by)
    if len(payload) != bcols * brows * 16:
        sys.exit(
            f"ERROR: {path}: payload {len(payload)} B, "
            f"expected {bcols}x{brows} blocks = {bcols * brows * 16} B"
        )
    return payload, bcols, brows


def swizzle_blocks(payload, bcols, brows):
    """Reorder linear raster blocks into EVE's 2x2 tile order.

    Guide section 6.1: blocks group into 2x2 tiles stored
        0 3
        1 2
    (memory order TL, BL, BR, TR). An odd trailing column packs as 1x2
    (top, bottom); an odd trailing row is stored linearly.
    """
    blk = [payload[i:i + 16] for i in range(0, len(payload), 16)]

    def b(r, c):
        return blk[r * bcols + c]

    out = []
    r = 0
    while r + 1 < brows:
        c = 0
        while c + 1 < bcols:
            out += [b(r, c), b(r + 1, c), b(r + 1, c + 1), b(r, c + 1)]
            c += 2
        if bcols % 2:
            out += [b(r, bcols - 1), b(r + 1, bcols - 1)]
        r += 2
    if brows % 2:
        out += [b(brows - 1, c) for c in range(bcols)]
    assert len(out) == len(blk)
    return b"".join(out)


def build_assets(tmp):
    """Encode every asset; return list of dicts with payload + metadata."""
    if not ASTCENC.is_file():
        sys.exit(
            f"ERROR: astcenc not found at {ASTCENC}\n"
            "Fetch the pinned binary first:  wsl -- bash tools/get-astcenc.sh"
        )
    built = []
    for name, src, block, is_bg in ASSET_LIST:
        src_path = ASSETS / src
        with Image.open(src_path) as im:
            w, h = im.size
            if is_bg:
                in_path = tmp / f"{name}.png"
                smooth_background(im).save(in_path, format="PNG")
            else:
                if im.mode != "RGBA":
                    sys.exit(f"ERROR: {src}: expected RGBA, got {im.mode}")
                in_path = src_path
        astc_path = tmp / f"{name}.astc"
        run_astcenc(in_path, astc_path, block)
        payload, bcols, brows = parse_astc(astc_path, w, h, block)
        payload = swizzle_blocks(payload, bcols, brows)
        built.append({
            "name": name,
            "src": src,
            "block": block,
            "fmt": EVE_FMT[block],
            "w": w,
            "h": h,
            "stride": bcols * 16,  # bytes per block row = ceil(w/bw)*16
            "payload": payload,
        })
        print(f"  {name}: {w}x{h} {block} -> {len(payload)} B")
    return built


def build_pack(assets):
    """Lay out header + aligned assets; pad to a sector multiple."""
    body = bytearray()
    offset = HEADER_SIZE
    for a in assets:
        pad = -offset % ASSET_ALIGN
        body += b"\x00" * pad
        offset += pad
        a["addr"] = FLASH_BASE + offset
        body += a["payload"]
        offset += len(a["payload"])
    total = HEADER_SIZE + len(body)
    body += b"\x00" * (-total % SECTOR)
    pack_size = HEADER_SIZE + len(body)
    crc = binascii.crc32(bytes(body)) & 0xFFFFFFFF
    header = struct.pack(
        "<4sIIII", MAGIC, VERSION, pack_size, len(assets), crc
    )
    header += b"\x00" * (HEADER_SIZE - len(header))
    pack = bytes(header) + bytes(body)
    assert len(pack) == pack_size and pack_size % SECTOR == 0
    return pack, crc


def emit_header(assets, pack, crc):
    magic_u32 = struct.unpack("<I", MAGIC)[0]
    out = [
        "/* splash_flash.h -- GENERATED by tools/make_splash_flash.py; do not edit.",
        " * Boot-splash ASTC assets: flash-image address table + provisioning pack.",
        " * Source PNGs live in assets/splash/ (vendored design export).",
        " *",
        " * The pack lives in the panel's QSPI flash at SPLASH_FLASH_BASE; sector 0",
        " * (flash 0..4095, vendor flashfast BLOB) is never written. Assets are",
        " * ASTC (backgrounds 8x8, alpha elements 4x4), blocks already in EVE's",
        " * 2x2 tile order, each asset 64-byte aligned for direct rendering via",
        " * BITMAP_SOURCE flash addressing (addr / 32).",
        " */",
        "",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "/* self-contained for host tests: no-op PROGMEM and the EVE ASTC",
        " * format ids (guarded; EVE.h's definitions win when included first) */",
        "#ifndef PROGMEM",
        "#define PROGMEM",
        "#endif",
        "#ifndef EVE_ASTC_4X4",
        "#define EVE_ASTC_4X4 ((uint32_t) 37808UL)",
        "#endif",
        "#ifndef EVE_ASTC_6X6",
        "#define EVE_ASTC_6X6 ((uint32_t) 37812UL)",
        "#endif",
        "#ifndef EVE_ASTC_8X8",
        "#define EVE_ASTC_8X8 ((uint32_t) 37815UL)",
        "#endif",
        "",
        "#define SPLASH_FLASH_BASE 4096UL",
        f"#define SPLASH_FLASH_VERSION {VERSION}UL",
        f"#define SPLASH_FLASH_MAGIC 0x{magic_u32:08X}UL /* LE \"MDSH\" */",
        f"#define SPLASH_FLASH_PACK_SIZE {len(pack)}UL",
        f"#define SPLASH_FLASH_CRC 0x{crc:08X}UL /* crc32 of pack bytes 64..end */",
        "",
    ]
    for a in assets:
        up = a["name"].upper()
        out += [
            f"/* {a['name']}: {a['w']}x{a['h']} {a['block']} from {a['src']} */",
            f"#define SPLASH_FA_{up}_ADDR {a['addr']}UL",
            f"#define SPLASH_FA_{up}_W {a['w']}U",
            f"#define SPLASH_FA_{up}_H {a['h']}U",
            f"#define SPLASH_FA_{up}_FMT {a['fmt']}",
            f"#define SPLASH_FA_{up}_STRIDE {a['stride']}U",
            f"#define SPLASH_FA_{up}_SIZE {len(a['payload'])}UL",
            "",
        ]
    out += [
        "typedef struct",
        "{",
        "    uint32_t addr;   /* absolute flash byte address */",
        "    uint32_t size;   /* ASTC payload bytes */",
        "    uint32_t fmt;    /* EVE_ASTC_4X4 or EVE_ASTC_8X8 */",
        "    uint16_t w;      /* pixels */",
        "    uint16_t h;      /* pixels */",
        "    uint16_t stride; /* bytes per block row = ceil(w/bw)*16 */",
        "    const char *name;",
        "} SplashFlashAsset;",
        "",
        "/* table indices; grouping mirrors the firmware theme tables:",
        " * common (emblem/wordmark/bars), then blue, red, checkered sets */",
        "enum",
        "{",
    ]
    for i, a in enumerate(assets):
        out.append(f"    SPLASH_FA_{a['name'].upper()} = {i},")
    out += [
        "    SPLASH_FA_COUNT",
        "};",
        "",
        "static const SplashFlashAsset SPLASH_FLASH_ASSETS[SPLASH_FA_COUNT] = {",
    ]
    for a in assets:
        up = a["name"].upper()
        out.append(
            f"    {{ SPLASH_FA_{up}_ADDR, SPLASH_FA_{up}_SIZE, SPLASH_FA_{up}_FMT,"
            f" SPLASH_FA_{up}_W, SPLASH_FA_{up}_H, SPLASH_FA_{up}_STRIDE,"
            f" \"{a['name']}\" }},"
        )
    out += [
        "};",
        "",
        "/* The entire flash image (header + assets + padding), embedded for",
        " * one-time provisioning via CMD_FLASHUPDATE. Teensy 4.1 PROGMEM is",
        " * memory-mapped flash, so this costs no RAM. */",
        f"static const uint8_t splash_flash_pack[{len(pack)}UL] PROGMEM = {{",
    ]
    lines = []
    for i in range(0, len(pack), 16):
        chunk = ", ".join(f"0x{b:02X}" for b in pack[i:i + 16])
        lines.append(f"    {chunk},")
    out += lines
    out += ["};", ""]

    out.append("/* flash image map:")
    out.append(f" *   {'asset':<16} {'addr':>8} {'size':>8}  fmt")
    for a in assets:
        out.append(
            f" *   {a['name']:<16} {a['addr']:>8} {len(a['payload']):>8}  "
            f"{a['block']} ({a['w']}x{a['h']})"
        )
    total_payload = sum(len(a["payload"]) for a in assets)
    out.append(f" *   assets total {total_payload} B; pack {len(pack)} B")
    out.append(f" *   (base {FLASH_BASE}, 64 B header, 64 B asset alignment,")
    out.append(" *    padded to a 4096 B sector multiple) */")
    out.append("")
    with open(OUT, "w", newline="\n") as fh:
        fh.write("\n".join(out))
    print(f"wrote {OUT.name}: pack {len(pack)} B, crc 0x{crc:08X}")


def main():
    print("encoding ASTC assets:")
    with tempfile.TemporaryDirectory() as tmpdir:
        assets = build_assets(Path(tmpdir))
    pack, crc = build_pack(assets)
    emit_header(assets, pack, crc)


if __name__ == "__main__":
    main()
