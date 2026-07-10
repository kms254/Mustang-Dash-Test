---
title: "Boot splash gradient banding from stacked 8-bit-source and RGB565 quantization"
date: 2026-07-09
category: ui-bugs
module: display-bringup
problem_type: ui_bug
component: tooling
symptoms:
  - "visible banding/contour rings in the boot splash's dark radial gradient backgrounds on the physical panel"
  - "banding persisted after a first fix that Bayer-dithered the 8-bit source image onto the RGB565 grid (only fixed the 565 truncation, not the source's own 8-bit plateau steps)"
root_cause: logic_error
resolution_type: code_fix
severity: medium
last_updated: 2026-07-10
tags:
  - boot-splash
  - gradient-banding
  - rgb565
  - cmd-loadimage
  - ordered-dither
  - bt817
  - numpy
  - png
---

# Boot splash gradient banding from stacked 8-bit-source and RGB565 quantization

> **Scope note (2026-07-10):** this pipeline served the embedded-PNG RGB565
> splash backgrounds and was retired for that use when PR #3 moved the splash
> to ASTC assets in panel flash. The successor keeps the float-reconstruction
> stage but deliberately drops the Bayer dither (ordered noise fights ASTC's
> block encoder) — see `smooth_background()` in `tools/make_splash_flash.py`
> and `docs/solutions/architecture-patterns/bt817-flash-resident-astc-assets.md`.
> The two-quantizer analysis and the full pipeline below remain valid guidance
> for any future RGB565-stored gradient asset; the cited
> `smooth_dither_background()` implementation now lives in git history
> (removed when `tools/make_splash_assets.py` was trimmed to reference
> composites), so its file:line cites below are historical (as of PR #2).

## Problem

The boot splash's dark radial-gradient backgrounds showed visible banding (contour rings) on the physical RVT70H panel — reported after the first hardware run of the red theme as "the gradient doesn't look as smooth as I like." Root cause: **two stacked quantizers**, and fixing only one is the trap.

## Symptoms

- Wide concentric contour rings across the dark portion of the radial gradient, most visible on the red theme's crimson-to-black vignette; blue theme affected too.
- Rings visible only on the real panel at full brightness — the source PNGs look acceptable on a desktop monitor, where you aren't staring at a 1024x600 backlit panel showing mostly the darkest 30 levels.
- Firmware, load path, and asset dimensions all correct; `EVE_cmd_loadimage` decoded and drew the PNGs fine. This is purely a pixel-value problem.

## What Didn't Work

First attempt: ordered-Bayer dithering the 8-bit source image onto the RGB565 grid (pure PIL — per-channel Bayer threshold offsets plus a truncation LUT). The reasoning was that the BT817's `CMD_LOADIMAGE` decodes opaque PNGs to RGB565 by *truncating* each 8-bit channel (5 bits of red = 32 levels), so pre-dithering onto the 5/6/5 grid should hide the truncation steps.

It did — and the panel still banded. The chip's 8→5/6-bit truncation was now lossless, but the **source PNG's own 8-bit quantization** passed straight through: a dark, slow radial gradient plateaus visibly even at 8 bits/channel, and any downscale/re-encode faithfully preserves those plateaus. Dithering at the storage bit depth cannot reconstruct precision the source never carried. The dither faithfully reproduced an already-banded image.

## Solution

Reconstruct the gradient in float *first*, then dither onto the 565 grid. Implemented (as of PR #2 — see the scope note above; the code now lives in git history) in `tools/make_splash_assets.py` as `smooth_dither_background()` with helper `_box_blur_axis()`, the classic `BAYER8` 8x8 matrix, and `BG_BLUR_RADIUS = 6`. Landed as "fix(assets): band-free background gradients via float reconstruction + 565 dithering" in PR #2 (merged).

The essential pipeline (condensed from tools/make_splash_assets.py:97-112):

```python
# 1. float32 load + block-mean 2x downscale: 1024x600 -> 512x300
small = src.reshape(h, 2, w, 2, 3).mean(axis=(1, 3), ...)

# 2. two edge-clamped box-blur passes per axis (radius 6 at half-res
#    = ~25 px triangular kernel) -- spans the 8-bit plateau bands
for _ in range(2):
    small = _box_blur_axis(small, BG_BLUR_RADIUS, axis=0)
    small = _box_blur_axis(small, BG_BLUR_RADIUS, axis=1)

# 3. Bayer-dither the float image directly onto the 5/6/5 grid,
#    then store bit-replication 8-bit values
bayer = (np.asarray(BAYER8, np.float32) + 0.5) / 64.0
for ch, bits in enumerate((5, 6, 5)):
    levels = float((1 << bits) - 1)
    v = small[:, :, ch] / 255.0 * levels
    q = np.clip(np.floor(v + tiled_bayer), 0.0, levels)
    out[:, :, ch] = np.round(q * (255.0 / levels)).astype(np.uint8)
```

On the render side, `draw_splash_background()` (MustangDash/MustangDash.ino:323) draws the 512x300 bitmap at 2x with `EVE_BILINEAR` filtering — `BITMAP_SIZE_H` + `BITMAP_SIZE(EVE_BILINEAR, ...)` at MustangDash/MustangDash.ino:327-329 and a 2x 16.16 scale matrix at MustangDash/MustangDash.ino:331 — to fill 1024x600.

Dependency note: this added **numpy** to the WSL converter environment (`pip3 install --user numpy`; Pillow was already required, same as `tools/pony.py`).

Verified: user approved the smoothed gradients on the physical panel (red and blue themes; checkered rebuilt too); converter output byte-identical across re-runs (determinism gate); decoded RAM_G sizes unchanged (same dimensions and format — only pixel values changed); both firmware toolchains still byte-identical.

## Why This Works

**Quantizer 1 — the chip.** The BT817 decodes opaque PNGs to RGB565 by truncating each channel to 5/6/5 bits. 32 red levels across a slow dark gradient means each level occupies a wide ring. Pre-dithering onto the 565 grid fixes exactly this — but only this.

**Quantizer 2 — the source.** The source PNG is itself 8-bit. A dark radial gradient that spends hundreds of pixels crossing one 8-bit step already contains plateau bands; every later stage that operates at 8 bits (downscale, re-encode, LUT, dither) inherits them. The only way out is to *reconstruct* the underlying continuous gradient: work in float32, and blur with a kernel wider than the plateau bands. Two box passes at radius 6 on the half-res image ≈ a 25 px triangular kernel — wide enough to average across adjacent 8-bit plateaus and recover sub-8-bit precision, and *safe* here because the input is an already-soft vignette: there is no genuine high-frequency detail for the blur to destroy. (This is why the same trick is wrong for a background containing text or edges — mask those regions or skip them.)

**Bit-replication makes truncation exact.** The stored 8-bit bytes are `round(q * 255/levels)` — the values whose top 5 (or 6) bits are exactly `q` and whose lower bits replicate them. So the BT817's 8→5/6-bit truncation maps every stored pixel onto precisely the intended 565 level: the on-chip quantizer becomes a no-op, and the converter fully controls the final grid placement.

**Bilinear upscale turns ordered noise into smoothness.** The renderer draws the 512x300 asset at 2x with `EVE_BILINEAR` (MustangDash/MustangDash.ino:328). Each output pixel is a weighted average of up to four dithered source texels, which pushes the value back toward the true float gradient the Bayer pattern was encoding. The dither reads as smoothness, not grain — the ordered noise is effectively demodulated by the filter. (With `NEAREST` filtering the Bayer pattern would be visible as texture; per Bridgetek's BT81x programming guide — not verifiable in-tree, the vendored `EVE_cmd_setbitmap()` wrapper sets no filter field — `CMD_SETBITMAP` defaults to nearest, which is why the sketch re-emits the SIZE words with `EVE_BILINEAR`.)

## Prevention

- **Know your display's storage bit depth before authoring gradients.** RGB565 has 32/64/32 levels per channel; any slow gradient — especially a dark one, where the eye's contrast sensitivity is highest — *will* band without dithering. This is a property of the format, not a bug in the chip.
- **When a gradient bands, audit BOTH quantizers.** The storage-format truncation is the obvious one; the source asset's own bit depth is the silent one. If dithering onto the target grid doesn't fix it, the banding was already in the source — no amount of dithering at that depth can add back precision. Reconstruct in float (blur wider than the plateau spacing) or re-export the source at higher bit depth.
- **Blur-to-reconstruct is only safe on content that is already smooth.** A soft vignette loses nothing to a 25 px kernel; anything with real edges does. Gate the blur to background layers.
- **Store bit-replication values** so the target's truncation is exact — otherwise the chip re-quantizes your carefully dithered pixels and shifts them off-grid.
- **Pair ordered dither with a bilinear (or better) upscale** when the asset renders above its stored resolution; the filter averages the dither back toward the true value. Set the renderer's filter mode explicitly rather than trusting defaults — per Bridgetek's BT81x programming guide, `CMD_SETBITMAP` defaults to nearest filtering.
- **Keep the converter deterministic and re-run it as a gate.** Byte-identical output across re-runs, unchanged decoded RAM_G sizes, and byte-identical firmware from both toolchains caught that this change altered *only* pixel values.
- **This generalizes** to any lower-bit-depth render target showing soft gradients: RGB565/RGB555 TFT framebuffers, e-paper grayscale, LED matrices, GIF/palette exports. Same two-quantizer checklist, same float-reconstruct → dither-to-target-grid → exact-truncation pipeline.

## Related Issues

- [docs/solutions/best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md](../best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md) —
  the panel/profile this dithered asset renders on; same verify-against-the-
  hardware discipline.
