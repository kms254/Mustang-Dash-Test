#!/usr/bin/env python3
"""Generate the boot-splash firmware headers and reference composites.

Reads the vendored design export in assets/splash/ and emits:

  MustangDash/splash_assets_common.h     emblem, wordmark, chrome bars
  MustangDash/splash_assets_blue.h       blue background (downscaled), line, year
  MustangDash/splash_assets_red.h        red background (downscaled), line, year
  MustangDash/splash_assets_checkered.h  checkered bg, blocks, line, strip, year
  assets/splash/reference/splash-{theme}.png   full-res final-frame composites

Backgrounds are downscaled 1024x600 -> 512x300 and re-encoded as opaque PNG:
the BT817's RAM_G (1 MiB) cannot hold a full-screen 16 bpp bitmap, so the
firmware draws the half-res background with a 2x bitmap transform (bilinear).
All other assets pass through byte-identical. Each asset becomes a PROGMEM
PNG byte array plus W/H/FMT defines; the decoded RAM_G footprint is W*H*2
bytes for both ARGB4 (alpha PNGs) and RGB565 (opaque PNGs).

Run under WSL (Pillow + numpy required; Pillow same as tools/pony.py):
  wsl -- python3 tools/make_splash_assets.py

The output is deterministic for a given Pillow version; re-running must
produce byte-identical headers (the invariant the plan's verification pins).
"""

import io
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets" / "splash"
OUT = ROOT / "MustangDash"
REF = ASSETS / "reference"

BG_DOWNSCALE = (512, 300)

# Final-frame layout from assets/splash/README.md (top-left positions).
EMBLEM_POS = (412, 124)
WORDMARK_POS = (162, 358)
BARS_LEFT = (138, 202)
BARS_RIGHT = (646, 202)
LINE_POS = (342, 420)
YEAR_POS = (412, 456)
CHECKER_BLOCK_LEFT = (138, 198)
CHECKER_BLOCK_RIGHT = (646, 198)
CHECKER_LINE_POS = (362, 440)
CHECKER_STRIP_TOP_Y = 0
CHECKER_STRIP_BOTTOM_Y = 574


# 8x8 Bayer matrix (values 0..63) for ordered dithering.
BAYER8 = (
    (0, 32, 8, 40, 2, 34, 10, 42),
    (48, 16, 56, 24, 50, 18, 58, 26),
    (12, 44, 4, 36, 14, 46, 6, 38),
    (60, 28, 52, 20, 62, 30, 54, 22),
    (3, 35, 11, 43, 1, 33, 9, 41),
    (51, 19, 59, 27, 49, 17, 57, 25),
    (15, 47, 7, 39, 13, 45, 5, 37),
    (63, 31, 55, 23, 61, 29, 53, 21),
)

BG_BLUR_RADIUS = 6  # half-res px per box pass; two passes = ~25 px triangle


def _box_blur_axis(arr, radius, axis):
    """Edge-clamped box blur along one axis via cumulative sums (float)."""
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


def smooth_dither_background(im, downscale):
    """Downscale a background and dither it onto the RGB565 grid, band-free.

    Two quantizers band these dark radial gradients: the source PNG's own
    8-bit steps, and the BT817's PNG decode truncating each channel to 5/6
    bits. So the pipeline reconstructs the gradient in float first --
    block-mean 2x downscale, then two edge-clamped box-blur passes (a ~25 px
    triangular kernel at half-res, far wider than the source's 8-bit plateau
    bands and harmless on an already-soft vignette) -- and Bayer-dithers the
    float image straight onto the 5/6/5 grid. Stored bytes use bit-replication
    values, so the chip's 8->5/6-bit truncation is exact and the renderer's
    2x bilinear upscale averages the ordered noise back toward the true
    gradient. Fully deterministic.
    """
    w, h = downscale
    src = np.asarray(im.convert("RGB"), dtype=np.float32)[: h * 2, : w * 2]
    small = src.reshape(h, 2, w, 2, 3).mean(axis=(1, 3), dtype=np.float64).astype(np.float32)
    for _ in range(2):
        small = _box_blur_axis(small, BG_BLUR_RADIUS, axis=0)
        small = _box_blur_axis(small, BG_BLUR_RADIUS, axis=1)

    bayer = (np.asarray(BAYER8, dtype=np.float32) + 0.5) / 64.0
    tiled = np.tile(bayer, (h // 8 + 1, w // 8 + 1))[:h, :w]
    out = np.empty((h, w, 3), dtype=np.uint8)
    for ch, bits in enumerate((5, 6, 5)):
        levels = float((1 << bits) - 1)
        v = small[:, :, ch] / 255.0 * levels
        q = np.clip(np.floor(v + tiled), 0.0, levels)
        out[:, :, ch] = np.round(q * (255.0 / levels)).astype(np.uint8)
    return Image.fromarray(out, "RGB")


def load_asset(filename, downscale=None):
    """Return (png_bytes, width, height, eve_format) for one asset."""
    data = (ASSETS / filename).read_bytes()
    with Image.open(io.BytesIO(data)) as im:
        if downscale is not None:
            small = smooth_dither_background(im, downscale)
            buf = io.BytesIO()
            small.save(buf, format="PNG", optimize=True)
            return buf.getvalue(), small.width, small.height, "RGB565"
        fmt = "ARGB4" if im.mode in ("RGBA", "LA", "PA") else "RGB565"
        return data, im.width, im.height, fmt


def emit_asset(out, name, data, width, height, fmt):
    ram = width * height * 2
    up = name.upper()
    out.append(
        f"/* {name}: {width}x{height}, PNG {len(data)} B, "
        f"decodes to EVE_{fmt} = {ram} B in RAM_G */"
    )
    out.append(f"#define SPLASH_{up}_W {width}")
    out.append(f"#define SPLASH_{up}_H {height}")
    out.append(f"#define SPLASH_{up}_FMT EVE_{fmt}")
    out.append(f"static const uint8_t splash_{name}_png[] PROGMEM = {{")
    for i in range(0, len(data), 16):
        chunk = ", ".join(f"0x{b:02X}" for b in data[i : i + 16])
        out.append(f"    {chunk},")
    out.append("};")
    out.append(f"#define SPLASH_{up}_PNG_SIZE {len(data)}U")
    out.append("")
    return ram


def write_header(filename, title, assets):
    """assets: list of (c_name, source_filename, downscale)."""
    out = [
        f"/* {filename} -- GENERATED by tools/make_splash_assets.py; do not edit.",
        f" * {title}",
        " * Source PNGs live in assets/splash/ (vendored design export).",
        " */",
        "",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
    ]
    total = 0
    for c_name, src, downscale in assets:
        data, w, h, fmt = load_asset(src, downscale)
        total += emit_asset(out, c_name, data, w, h, fmt)
    out.append(f"/* decoded RAM_G total for this header: {total} bytes */")
    out.append("")
    with open(OUT / filename, "w", newline="\n") as fh:
        fh.write("\n".join(out))
    print(f"  {filename}: {total} B decoded")


def paste(base, filename, pos, flip=False):
    with Image.open(ASSETS / filename) as im:
        overlay = im.convert("RGBA")
        if flip:
            overlay = overlay.transpose(Image.FLIP_LEFT_RIGHT)
        base.alpha_composite(overlay, pos)


def write_reference(theme):
    """Composite the spec's final frame at full resolution for AE4 eyeballing."""
    with Image.open(ASSETS / f"bg-{theme}-1024x600.png") as bg_im:
        frame = bg_im.convert("RGBA")
    if theme == "checkered":
        paste(frame, "checker-strip-1024x26.png", (0, CHECKER_STRIP_TOP_Y))
        paste(frame, "checker-strip-1024x26.png", (0, CHECKER_STRIP_BOTTOM_Y), flip=True)
        paste(frame, "checker-block-240x52.png", CHECKER_BLOCK_LEFT)
        paste(frame, "checker-block-240x52.png", CHECKER_BLOCK_RIGHT)
        paste(frame, "emblem-200x200.png", EMBLEM_POS)
        paste(frame, "wordmark-mustang-700x80.png", WORDMARK_POS)
        paste(frame, "checker-line-300x14.png", CHECKER_LINE_POS)
        paste(frame, "year-1965-checkered.png", YEAR_POS)
    else:
        paste(frame, "bars-chrome-240x45.png", BARS_LEFT)
        paste(frame, "bars-chrome-240x45.png", BARS_RIGHT)
        paste(frame, "emblem-200x200.png", EMBLEM_POS)
        paste(frame, "wordmark-mustang-700x80.png", WORDMARK_POS)
        paste(frame, f"line-{theme}-340x40.png", LINE_POS)
        paste(frame, f"year-1965-{theme}.png", YEAR_POS)
    REF.mkdir(parents=True, exist_ok=True)
    frame.convert("RGB").save(REF / f"splash-{theme}.png")
    print(f"  reference/splash-{theme}.png")


def main():
    print("headers:")
    write_header(
        "splash_assets_common.h",
        "Theme-independent splash assets: emblem, wordmark, chrome bars.",
        [
            ("emblem", "emblem-200x200.png", None),
            ("wordmark", "wordmark-mustang-700x80.png", None),
            ("bars_chrome", "bars-chrome-240x45.png", None),
        ],
    )
    write_header(
        "splash_assets_blue.h",
        "Blue theme: background (downscaled 2x), accent line, year.",
        [
            ("bg_blue", "bg-blue-1024x600.png", BG_DOWNSCALE),
            ("line_blue", "line-blue-340x40.png", None),
            ("year_blue", "year-1965-blue.png", None),
        ],
    )
    write_header(
        "splash_assets_red.h",
        "Red theme: background (downscaled 2x), accent line, year.",
        [
            ("bg_red", "bg-red-1024x600.png", BG_DOWNSCALE),
            ("line_red", "line-red-340x40.png", None),
            ("year_red", "year-1965-red.png", None),
        ],
    )
    write_header(
        "splash_assets_checkered.h",
        "Checkered theme: background (downscaled 2x), blocks, line, strip, year.",
        [
            ("bg_checkered", "bg-checkered-1024x600.png", BG_DOWNSCALE),
            ("checker_block", "checker-block-240x52.png", None),
            ("checker_line", "checker-line-300x14.png", None),
            ("checker_strip", "checker-strip-1024x26.png", None),
            ("year_checkered", "year-1965-checkered.png", None),
        ],
    )
    print("reference composites:")
    for theme in ("blue", "red", "checkered"):
        write_reference(theme)


if __name__ == "__main__":
    main()
