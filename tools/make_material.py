#!/usr/bin/env python3
"""Generate the splash background material: a seamless engine-turned metal
tile plus the cluster-wide glow, sliced per panel.

WHY THIS IS GENERATED RATHER THAN AUTHORED
------------------------------------------
The background used to be one 1024x600 ASTC bitmap per theme -- 614,400 B of
RAM_G, which is 60% of a BT817's 1 MiB and left no room for the fonts once
they grew (see docs/solutions/conventions/, the 2026-08-21 clock round).
It also could not span three panels: three authored slices at that size do
not fit in MCU flash.

The fix is to split the image by SPATIAL FREQUENCY, because the two halves
want opposite things:

  high frequency (the machined weave)  -> must stay at NATIVE resolution;
                                          any upscale destroys it. But it
                                          REPEATS, so one small tile drawn
                                          with EVE_REPEAT covers any area.
  low  frequency (the lit glow)        -> is smooth by definition, so it
                                          survives being authored tiny and
                                          stretched with BILINEAR -- and
                                          comes out SMOOTHER than an ASTC
                                          bitmap, which quantises gradients
                                          into blocks.

Result: ~19 KB per panel instead of 614,400 B, at higher quality, and the
glow is computed across the whole cluster so it sweeps continuously over the
physical bezel gaps instead of repeating per panel.

GEOMETRY
--------
Panels are 1024x600 on a 7" diagonal, so the active area is ~153.4 mm wide
and one millimetre is ~6.68 px. PANEL_GAP_MM is the dead space between two
panels' ACTIVE AREAS (bezels plus the divider), measured on the real car.

Run under WSL (Pillow + numpy), then rebuild the pack:
    wsl -- python3 tools/make_material.py
    wsl -- python3 tools/make_splash_flash.py
"""
import math
import os

import numpy as np
from PIL import Image, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "splash")

# ---- cluster geometry -------------------------------------------------
PANEL_W, PANEL_H = 1024, 600
PANEL_DIAG_IN = 7.0
PANEL_GAP_MM = 15.0  # measured on the car, 2026-08-21: active area to active area

_W_MM = PANEL_DIAG_IN * 25.4 * PANEL_W / math.sqrt(PANEL_W ** 2 + PANEL_H ** 2)
PX_PER_MM = PANEL_W / _W_MM
PANEL_GAP_PX = int(round(PANEL_GAP_MM * PX_PER_MM))
CLUSTER_W = PANEL_W * 3 + PANEL_GAP_PX * 2

# ---- material ---------------------------------------------------------
TILE = 128  # engine-turned tile edge; 128 => 16,384 B at ASTC 4x4
SWIRL_SPACING = 32  # centre-to-centre of the machined discs
SWIRL_RADIUS = 26  # disc radius; > spacing/2 so discs overlap, as real jeweling does

# ---- glow -------------------------------------------------------------
GLOW_DIV = 2  # glow authored at 1/2 scale, stretched back with BILINEAR
GLOW_W, GLOW_H = PANEL_W // GLOW_DIV, PANEL_H // GLOW_DIV


def cluster_glow():
    """One glow field across the whole cluster, at 1/GLOW_DIV scale.

    The hotspot sits above centre on the CENTRE panel, so the side panels
    carry the outer falloff and the sweep reads continuously across the
    bezel gaps rather than repeating three times.
    """
    w, h = CLUSTER_W // GLOW_DIV, PANEL_H // GLOW_DIV
    y, x = np.mgrid[0:h, 0:w]
    hx, hy = w / 2.0, h * 0.10
    sx, sy = (PANEL_W / GLOW_DIV) * 0.115 * 1.9, h * 0.62
    g = 86.0 * np.exp(-(((x - hx) / sx) ** 2 + ((y - hy) / sy) ** 2))
    g += 16.0 * np.clip(1.0 - (y / float(h)) * 1.35, 0.0, None)
    return np.clip(g, 0, 255)


def _rgba(arr):
    """The pack encoder requires RGBA for every non-background asset."""
    if arr.ndim == 2:
        arr = np.dstack([arr, arr, arr])
    a = np.full(arr.shape[:2] + (1,), 255, dtype=np.uint8)
    return np.concatenate([arr.astype(np.uint8), a], axis=2)


def _unused_tint(mono):
    """World Rally Blue Mica (Subaru 02C) as the material colour.

    The glow layer ships as a colour bitmap rather than a mask so the hue
    lives in the asset instead of being re-derived in three places in the
    renderer. Screens emit where paint reflects, so the peak runs brighter
    and more saturated than the paint chip.
    """
    g = mono / 255.0
    out = np.zeros(mono.shape + (3,))
    unlit = (18.0 / 255.0 * 6.0, 76.0 / 255.0 * 6.0, 158.0 / 255.0 * 6.0)
    peak = (28.0, 108.0, 232.0)
    for i in range(3):
        out[:, :, i] = unlit[i] + peak[i] * g * 1.1
    return np.clip(out, 0, 255).astype(np.uint8)


def dither_tile(n=64, seed=7):
    """Output-resolution dither.

    REG_OUTBITS is 8 bits, so the FRAMEBUFFER quantises after every asset: a
    ~106-level ramp across 600 px steps every ~6 px and shows contour rings.
    No amount of source precision fixes that, because the quantisation happens
    downstream of the asset.

    This tile is drawn 1:1 with EVE_REPEAT and added at an amplitude of a few
    levels. Drawn 1:1 is the whole point -- an earlier attempt dithered the
    GLOW source instead, which is magnified on screen, so the dither cells
    became blobs and read as diagonal mottling.
    """
    rng = np.random.default_rng(seed)
    return rng.integers(0, 256, (n, n), dtype=np.uint8)


def main():
    # The glow ships as an 8-bit ALPHA MASK, not a colour image: the colour
    # comes from COLOR_RGB in the display list, so the ramp gets 256 levels
    # instead of RGB565's 32 blue levels. Normalise to the full 0..255 range
    # so none of that precision is wasted.
    glow = cluster_glow()
    glow = glow * (255.0 / max(1.0, glow.max()))
    im = Image.fromarray(np.clip(glow, 0, 255).astype(np.uint8), "L").filter(
        ImageFilter.GaussianBlur(1.2))
    names = ("left", "center", "right")
    step = (PANEL_W + PANEL_GAP_PX) // GLOW_DIV
    for i, nm in enumerate(names):
        x0 = i * step
        # RGB, not RGBA: this ships uncompressed as RGB565, no alpha channel
        im.crop((x0, 0, x0 + GLOW_W, GLOW_H)).save(
            os.path.join(OUT, "glow-%s-%dx%d.png" % (nm, GLOW_W, GLOW_H)))

    Image.fromarray(dither_tile(), "L").save(os.path.join(OUT, "dither-64x64.png"))

    per_panel = GLOW_W * GLOW_H + 64 * 64
    print("panel active width %.1f mm -> %.3f px/mm" % (_W_MM, PX_PER_MM))
    print("gap %.0f mm -> %d px; cluster %d px" % (PANEL_GAP_MM, PANEL_GAP_PX, CLUSTER_W))
    print("glow slice   %dx%d -> %6d B raw L8 (x3 panels)" % (GLOW_W, GLOW_H, GLOW_W * GLOW_H))
    print("dither tile  64x64   -> %6d B raw L8" % (64 * 64,))
    print("per panel    %6d B  (was 614400 for the authored background)" % per_panel)


if __name__ == "__main__":
    main()
