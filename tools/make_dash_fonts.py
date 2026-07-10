#!/usr/bin/env python3
"""Generate the EVE (BT817) legacy bitmap-font header for the dash UI.

Rasterizes the vendored TTFs in assets/fonts/ with Pillow into legacy
bitmap-font instances (BT81x Programming Guide 5.4.1) and emits:

  MustangDash/dash_fonts.h          148-byte metric blocks + zlib glyph
                                    streams (EVE_cmd_inflate) + registry
  assets/fonts/preview-{name}.png   per-instance glyph strips for eyeballing

Format facts baked in here (verified against the programming guide):
the metric block is 128 advance-width bytes for codes 0..127, then u32 LE
format (17 = L4), line stride, cell width, cell height, and the glyph
pointer; gptr is emitted as 0 and patched by the firmware once it knows the
RAM_G load address. Glyph cells are fixed-size, contiguous from firstchar
(CMD_SETFONT2 indexes gptr + (c - firstchar)*stride*height), L4 packed two
pixels per byte with the LEFT pixel in the high nibble. Cell widths are kept
even so stride = cell_w/2 exactly. Two cells hold non-ASCII artwork so every
stored code stays < 128: F_VAL's '*' cell renders U+00B0 (degree sign) and
F_LABEL/F_TINY's ';' cell renders U+00B7 (middle dot). Label-style instances
bake wide tracking into their advance widths (F_TITLE +4, F_LABEL +3,
F_TINY +2 px).

Run under WSL (Pillow + numpy required, same as tools/make_splash_assets.py):
  wsl -- python3 tools/make_dash_fonts.py

The output is deterministic for a given Pillow/FreeType version; re-running
must produce byte-identical headers (the invariant the plan's verification
pins).
"""

import struct
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
FONTS = ROOT / "assets" / "fonts"
OUT = ROOT / "MustangDash"

# BITMAP_LAYOUT format code for 4-bit grayscale. The BT81x format table
# (and the vendored library's EVE.h:190) define L4 = 2; 17 is L2. A wrong
# value here renders every glyph through the wrong bit depth.
EVE_L4 = 2
METRIC_BLOCK_SIZE = 148

LABEL_SET = ' "/0123456789;ABCDEFGHIJKLMNOPQRSTUVWXYZ'

# name, ttf, px, glyph set (ASCII cell codes), handle, tracking px,
# artwork substitutions {cell char: rendered char}
INSTANCES = [
    ("hero", "SairaCondensed-Bold.ttf", 216, "0123456789-", 1, 0, {}),
    ("big", "SairaCondensed-Bold.ttf", 96, "0123456789:.-", 2, 0, {}),
    ("mid", "SairaCondensed-SemiBold.ttf", 52, "0123456789:.-+", 3, 0, {}),
    ("val", "SairaCondensed-SemiBold.ttf", 32, "0123456789:.-+*", 4, 0,
     {"*": "°"}),
    ("small", "SairaCondensed-SemiBold.ttf", 26, "0123456789.", 5, 0, {}),
    ("title", "ChakraPetch-Bold.ttf", 72,
     " ABCDEFGHIJKLMNOPQRSTUVWXYZ", 6, 4, {}),
    ("label", "ChakraPetch-SemiBold.ttf", 20, LABEL_SET, 7, 3,
     {";": "·"}),
    ("tiny", "ChakraPetch-Medium.ttf", 15, LABEL_SET, 8, 2,
     {";": "·"}),
]




def align4(n):
    return (n + 3) & ~3


def build_instance(name, ttf, px, glyphs, handle, tracking, artwork):
    """Rasterize one font instance; return a dict of everything the header
    and preview emitters need."""
    font = ImageFont.truetype(str(FONTS / ttf), px)

    glyph = {}  # cell char -> (rendered char, advance px, ink bbox or None)
    x0 = y0 = 1 << 30
    x1 = y1 = -(1 << 30)
    for ch in glyphs:
        code = ord(ch)
        assert code < 128, "stored glyph code %d not ASCII in %s" % (code, name)
        art = artwork.get(ch, ch)
        adv = int(round(font.getlength(art))) + tracking
        assert 0 < adv < 256, "advance %d out of u8 range in %s" % (adv, name)
        bbox = font.getbbox(art)
        inked = (bbox is not None and bbox[2] > bbox[0] and bbox[3] > bbox[1])
        glyph[ch] = (art, adv, bbox if inked else None)
        if inked:
            x0 = min(x0, bbox[0])
            y0 = min(y0, bbox[1])
            x1 = max(x1, bbox[2])
            y1 = max(y1, bbox[3])
    assert x1 > x0 and y1 > y0, "no inked glyphs in %s" % name

    # Common cell box: union of all ink bboxes so baselines align across the
    # instance. Glyphs draw at their natural bearing (shift only rescues a
    # negative left bearing, uniformly) so the advance widths position them.
    shift = max(0, -x0)
    cell_w = x1 + shift
    if cell_w % 2:
        cell_w += 1  # keep stride = cell_w/2 exact for L4
    cell_h = y1 - y0
    stride = cell_w // 2

    first = min(ord(c) for c in glyphs)
    last = max(ord(c) for c in glyphs)

    nibble_cells = []  # one (cell_h, cell_w) uint8 array of 0..15 per cell
    for code in range(first, last + 1):
        ch = chr(code)
        if ch in glyph and glyph[ch][2] is not None:
            im = Image.new("L", (cell_w, cell_h), 0)
            ImageDraw.Draw(im).text((shift, -y0), glyph[ch][0],
                                    font=font, fill=255)
            arr = np.asarray(im, dtype=np.uint16)
            nib = ((arr * 15 + 127) // 255).astype(np.uint8)
        else:
            # in-range code with no glyph (or ink-free like space): blank cell
            nib = np.zeros((cell_h, cell_w), dtype=np.uint8)
        nibble_cells.append(nib)

    # L4 pack: two pixels per byte, LEFT pixel in the high nibble
    glyph_data = b"".join(
        ((nib[:, 0::2] << 4) | nib[:, 1::2]).astype(np.uint8).tobytes()
        for nib in nibble_cells
    )
    assert len(glyph_data) == stride * cell_h * (last - first + 1)

    widths = [0] * 128
    for ch in glyphs:
        widths[ord(ch)] = glyph[ch][1]
    metrics = bytes(widths) + struct.pack("<5I", EVE_L4, stride, cell_w, cell_h, 0)
    assert len(metrics) == METRIC_BLOCK_SIZE

    return {
        "name": name,
        "ttf": ttf,
        "px": px,
        "glyphs": glyphs,
        "handle": handle,
        "tracking": tracking,
        "first": first,
        "last": last,
        "cell_w": cell_w,
        "cell_h": cell_h,
        "stride": stride,
        "metrics": metrics,
        "glyph_data": glyph_data,
        "zdata": zlib.compress(glyph_data, 9),
        "nibble_cells": nibble_cells,
        "decoded": align4(METRIC_BLOCK_SIZE) + align4(len(glyph_data)),
    }


def emit_bytes(out, data):
    for i in range(0, len(data), 16):
        chunk = ", ".join("0x%02X" % b for b in data[i:i + 16])
        out.append("    %s," % chunk)


def emit_instance(out, inst):
    name = inst["name"]
    up = name.upper()
    glyph_bytes = len(inst["glyph_data"])
    zbytes = len(inst["zdata"])
    ncells = inst["last"] - inst["first"] + 1
    out.append(
        "/* F_%s: %s @ %d px + %d px tracking, glyphs \"%s\"," %
        (up, inst["ttf"], inst["px"], inst["tracking"],
         inst["glyphs"].replace("\\", "\\\\"))
    )
    out.append(
        " * cells '%c'(%d)..'%c'(%d) = %d, cell %dx%d px L4 stride %d," %
        (inst["first"], inst["first"], inst["last"], inst["last"], ncells,
         inst["cell_w"], inst["cell_h"], inst["stride"])
    )
    out.append(
        " * glyph data %d B -> zlib %d B; decoded RAM_G %d B */" %
        (glyph_bytes, zbytes, inst["decoded"])
    )
    out.append("#define DASH_FONT_%s_HANDLE %d" % (up, inst["handle"]))
    out.append("#define DASH_FONT_%s_FIRSTCHAR %d" % (up, inst["first"]))
    out.append("#define DASH_FONT_%s_CELL_W %d" % (up, inst["cell_w"]))
    out.append("#define DASH_FONT_%s_CELL_H %d" % (up, inst["cell_h"]))
    out.append("#define DASH_FONT_%s_STRIDE %d" % (up, inst["stride"]))
    out.append("#define DASH_FONT_%s_GLYPH_BYTES %dU" % (up, glyph_bytes))
    out.append("#define DASH_FONT_%s_ZBYTES %dU" % (up, zbytes))
    out.append("static const uint8_t dash_font_%s_metrics[148] PROGMEM = {"
               % name)
    emit_bytes(out, inst["metrics"])
    out.append("};")
    out.append("static const uint8_t dash_font_%s_glyphs_z[] PROGMEM = {"
               % name)
    emit_bytes(out, inst["zdata"])
    out.append("};")
    out.append("")


def write_header(instances):
    out = [
        "/* dash_fonts.h -- GENERATED by tools/make_dash_fonts.py; do not edit.",
        " * EVE (BT817) legacy bitmap fonts (guide 5.4.1) for the dash UI:",
        " * 148-byte metric blocks (gptr = 0, firmware patches the RAM_G",
        " * address at boot) plus zlib glyph streams for EVE_cmd_inflate.",
        " * Load with CMD_SETFONT2(handle, blockaddr, firstchar).",
        " * Source TTFs live in assets/fonts/ (OFL-licensed, vendored).",
        " */",
        "",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "#ifndef PROGMEM",
        "#define PROGMEM /* host builds have no flash attribute */",
        "#endif",
        "",
        "#ifndef DASH_FONT_DESC_DEFINED",
        "#define DASH_FONT_DESC_DEFINED",
        "typedef struct",
        "{",
        "    const uint8_t *metrics;  /* 148-byte legacy metric block, gptr = 0 */",
        "    const uint8_t *glyphs_z; /* zlib stream for EVE_cmd_inflate */",
        "    uint32_t zbytes;         /* compressed glyph stream size */",
        "    uint32_t glyph_bytes;    /* inflated glyph data size */",
        "    uint8_t handle;          /* bitmap handle for CMD_SETFONT2 */",
        "    uint8_t firstchar;       /* first stored character code */",
        "    uint16_t cell_w, cell_h, stride;",
        "} DashFontDesc;",
        "#endif",
        "",
    ]
    for inst in instances:
        emit_instance(out, inst)

    out.append("#define DASH_FONT_COUNT %d" % len(instances))
    out.append("static const DashFontDesc DASH_FONTS[DASH_FONT_COUNT] = {")
    for inst in instances:
        up = inst["name"].upper()
        out.append("    { dash_font_%s_metrics, dash_font_%s_glyphs_z,"
                   % (inst["name"], inst["name"]))
        out.append("      DASH_FONT_%s_ZBYTES, DASH_FONT_%s_GLYPH_BYTES,"
                   % (up, up))
        out.append("      DASH_FONT_%s_HANDLE, DASH_FONT_%s_FIRSTCHAR,"
                   % (up, up))
        out.append("      DASH_FONT_%s_CELL_W, DASH_FONT_%s_CELL_H, "
                   "DASH_FONT_%s_STRIDE }," % (up, up, up))
    out.append("};")
    out.append("")

    total = sum(inst["decoded"] for inst in instances)
    out.append("/* RAM_G budget (148 B metrics + inflated glyphs, "
               "4-byte aligned packing):")
    for inst in instances:
        out.append(" *   F_%-6s %7d B" % (inst["name"].upper(),
                                          inst["decoded"]))
    out.append(" *   total  %7d B */" % total)
    out.append("")
    with open(str(OUT / "dash_fonts.h"), "w", newline="\n") as fh:
        fh.write("\n".join(out))
    return total


def write_preview(inst):
    """White-on-black strip of the instance's glyphs, rebuilt from the same
    quantized L4 nibbles the chip will see (nibble * 17 -> 8-bit)."""
    gap = 4
    cells = [inst["nibble_cells"][ord(ch) - inst["first"]]
             for ch in inst["glyphs"]]
    w = sum(c.shape[1] for c in cells) + gap * (len(cells) + 1)
    strip = np.zeros((inst["cell_h"] + 2 * gap, w), dtype=np.uint8)
    x = gap
    for cell in cells:
        strip[gap:gap + inst["cell_h"], x:x + cell.shape[1]] = cell * 17
        x += cell.shape[1] + gap
    path = FONTS / ("preview-%s.png" % inst["name"])
    Image.fromarray(strip, "L").save(str(path), format="PNG", optimize=True)


def main():
    instances = [build_instance(*spec) for spec in INSTANCES]
    total = write_header(instances)
    for inst in instances:
        write_preview(inst)

    print("dash fonts (MustangDash/dash_fonts.h):")
    print("  name     px  hnd  range   cells  cell px    glyph B    zlib B   RAM_G B")
    for inst in instances:
        ncells = inst["last"] - inst["first"] + 1
        print("  F_%-6s %3d  %3d  %3d..%-3d  %4d  %4dx%-4d  %8d  %8d  %8d" % (
            inst["name"].upper(), inst["px"], inst["handle"],
            inst["first"], inst["last"], ncells,
            inst["cell_w"], inst["cell_h"],
            len(inst["glyph_data"]), len(inst["zdata"]), inst["decoded"]))
    print("  total decoded RAM_G: %d bytes" % total)
    print("  previews: assets/fonts/preview-{%s}.png"
          % ",".join(i["name"] for i in instances))


if __name__ == "__main__":
    main()
