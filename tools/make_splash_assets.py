#!/usr/bin/env python3
"""Generate the boot-splash reference composites.

Reads the vendored design export in assets/splash/ and emits:

  assets/splash/reference/splash-{theme}.png   full-res final-frame composites

These are the AE4 eyeballing references only. The firmware assets are built
by tools/make_splash_flash.py, which compresses the same PNGs to ASTC and
lays them out as a flash image for the panel's QSPI flash (the old PROGMEM
PNG headers this script used to emit are retired).

Run under WSL (Pillow required, same as tools/pony.py):
  wsl -- python3 tools/make_splash_assets.py

The output is deterministic for a given Pillow version; re-running must
produce byte-identical composites.
"""

from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets" / "splash"
REF = ASSETS / "reference"

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
    print("reference composites:")
    for theme in ("blue", "red", "checkered"):
        write_reference(theme)


if __name__ == "__main__":
    main()
