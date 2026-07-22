#!/usr/bin/env python3
"""Remaster a splash background: kill gradient banding / halo rings.

The shipped backgrounds carry generation-era quantization halos (see
docs/solutions/ui-bugs/boot-splash-radial-gradient-banding-double-quantization.md).
This tool fits a smoothed radial color profile from the existing PNG
(median RGB per elliptical-radius bin around the luminance centroid), then
re-renders the same gradient in float64 and quantizes ONCE to 8-bit:
identical palette and composition, mathematically smooth falloff. No
dither -- ordered noise fights the ASTC block encoder downstream.

Usage (WSL, like the other asset tools):
  python3 tools/remaster_splash_bg.py blue [red] [checkered]

Overwrites assets/splash/bg-<theme>-1024x600.png (git holds the prior);
re-run tools/make_splash_flash.py afterwards to rebuild the pack.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
BINS = 512

def remaster(theme):
    path = ROOT / "assets" / "splash" / f"bg-{theme}-1024x600.png"
    img = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    h, w, _ = img.shape

    lum = img.sum(axis=2)
    ys, xs = np.mgrid[0:h, 0:w]
    wsum = lum.sum()
    cx = (xs * lum).sum() / wsum  # luminance centroid = gradient center
    cy = (ys * lum).sum() / wsum

    rx = max(cx, w - 1 - cx)
    ry = max(cy, h - 1 - cy)
    r = np.sqrt(((xs - cx) / rx) ** 2 + ((ys - cy) / ry) ** 2)
    r_norm = r / r.max()

    # median RGB per radius bin -> robust radial profile
    idx = np.minimum((r_norm * (BINS - 1)).astype(int), BINS - 1)
    prof = np.zeros((BINS, 3))
    for b in range(BINS):
        sel = img[idx == b]
        prof[b] = np.median(sel, axis=0) if len(sel) else prof[b - 1]

    # smooth the profile: two passes of a 9-tap moving average
    k = np.ones(9) / 9.0
    for _ in range(2):
        for c in range(3):
            prof[:, c] = np.convolve(np.pad(prof[:, c], 4, mode="edge"), k, mode="valid")

    # re-render continuously from the profile; single final quantization
    pos = r_norm * (BINS - 1)
    lo = np.minimum(pos.astype(int), BINS - 2)
    frac = (pos - lo)[..., None]
    out = prof[lo] * (1.0 - frac) + prof[lo + 1] * frac

    Image.fromarray(np.clip(out + 0.5, 0, 255).astype(np.uint8), "RGB").save(path)
    print(f"remastered {path.name}: center=({cx:.0f},{cy:.0f})")

if __name__ == "__main__":
    for t in (sys.argv[1:] or ["blue"]):
        remaster(t)
