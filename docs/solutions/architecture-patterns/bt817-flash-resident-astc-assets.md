---
title: "Flash-resident ASTC assets on the BT817 (EVE4)"
date: 2026-07-10
problem_type: architecture_pattern
category: architecture-patterns
module: display-bringup
component: tooling
severity: medium
applies_when:
  - "RAM_G is contended by multiple simultaneous resident assets (fonts, backgrounds, crossfades) on a BT81x/EVE4 panel"
  - "the panel has factory-fitted QSPI NOR flash and static bitmaps can be shipped ASTC-compressed"
  - "deciding whether to provision static assets into panel flash vs decode-to-RAM_G at runtime"
  - "converting raster assets to ASTC for BT81x flash-resident rendering, including swizzle and alignment constraints"
  - "designing a one-time flash provisioning flow with CMD_FLASHUPDATE and a commit-record chunk"
tags:
  - bt817
  - eve4
  - astc
  - qspi-flash
  - ram_g
  - flash-provisioning
  - asset-pipeline
  - riverdi
---

# Flash-resident ASTC assets on the BT817 (EVE4)

## Context

The dash layout work created a RAM_G collision. The custom EVE bitmap fonts need ~273 KB of RAM_G from address 0 (`MustangDash/dash_fonts.h` budget footer — total 273,120 B), while the boot splash at the time occupied roughly 850 KB of the BT817's 1 MiB RAM_G as PNG bitmaps decoded via CMD_LOADIMAGE. Both had to be resident *simultaneously* during the splash-to-dash crossfade — the union problem already documented in the RAM_G budgeting pattern. The first plan draft gave up and substituted a fade-through-black so the two tenants never coexisted; Kevin rejected that and directed moving the splash assets into the panel's onboard QSPI flash instead, rendered directly from flash so the splash uses zero RAM_G. Shipped in PR #3 (branch `feat/dash-layout`, open as of this writing).

## Guidance

The pattern: compress static assets to ASTC offline, lay them out as a single versioned "pack", embed the pack in the MCU firmware, provision it into the panel's QSPI flash once (header-compare, CMD_FLASHUPDATE on mismatch), and render every asset directly from flash with `CMD_SETBITMAP(0x800000 | addr/32, …)`. RAM_G never sees the assets. Every constraint below was hard-won; each is grounded in the current tree.

### Offline pipeline (`tools/make_splash_flash.py`)

- **ASTC is the only format the BT81x renders directly from flash** — the whole design hangs on that (BT81x Programming Guide, BRT_AN_033 v2.8; the pipeline header cites the guide at `tools/make_splash_flash.py:14`).
- **The 2x2 tile swizzle is mandatory.** astcenc emits blocks in linear raster order; EVE wants tiles of 2x2 blocks stored TL, BL, BR, TR, with an odd trailing column packed 1x2 top-then-bottom and an odd trailing row linear (BRT_AN_033 v2.8 section 6.1 "ASTC RAM Layout"). `swizzle_blocks()` at `tools/make_splash_flash.py:181-208` implements exactly this; the docstring at lines 24-28 warns that skipping it makes "the panel render scrambled blocks".
- **64-byte alignment for every asset.** ASTC bitmaps in flash must be 64-byte aligned (BRT_AN_033 v2.8 section 6.2 "ASTC Flash Layout"); this also satisfies BITMAP_SOURCE's 32-byte flash block addressing (`tools/make_splash_flash.py:35-37` and line 71; padding applied in `build_pack()` at lines 253-255).
- **Block size by content class:** soft backgrounds get ASTC 8x8, detail/alpha elements get 4x4 — see the per-asset table at `tools/make_splash_flash.py:81-96` (backgrounds `"8x8"`, everything else `"4x4"`) and the pipeline notes at lines 15-19.
- **Float gradient reconstruction stays; Bayer dither goes.** The retired PNG pipeline dithered its downscaled backgrounds; the flash pipeline keeps the float32 two-pass edge-clamped box blur to kill 8-bit banding but deliberately drops ordered dither because "ordered noise fights ASTC's block encoder, and 8x8 blocks over a smooth float-reconstructed gradient band far less than the quantized source" (`smooth_background()` docstring, `tools/make_splash_flash.py:126-129`).
- **Determinism is a contract.** The encoder is invoked with fixed flags `-thorough -j 1 -silent` (`run_astcenc()` builds the command at `tools/make_splash_flash.py:140-144`) — single-threaded specifically so "output is deterministic for the pinned binary; re-running must produce a byte-identical header" (module docstring, `tools/make_splash_flash.py:21-22`).
- **Pack layout:** base = flash address 4096; sector 0 (bytes 0..4095) holds the vendor-programmed flashfast BLOB and is NEVER written (`tools/make_splash_flash.py:31-32` and line 69). A 64-byte header at base carries magic `"MDSH"`, version, pack length, asset count, and crc32 of everything after the header (`build_pack()` packing `<4sIIII` at lines 262-266); total is padded to a 4096-byte multiple, the CMD_FLASHUPDATE granularity (guide 5.82). Output is `MustangDash/splash_flash.h` — an address table plus the whole pack in Teensy PROGMEM (memory-mapped flash, so it costs no RAM; `tools/make_splash_flash.py:356-359`).

### Encoder pinning (`tools/get-astcenc.sh`)

- Pinned to **astcenc 4.6.1 linux-x64** with sha256 verification of both the release zip and the extracted binary (`tools/get-astcenc.sh:18-21`, checks at lines 45-52 and 64-70).
- The version choice is a glibc constraint: "4.6.1 is the newest 4.x release whose binaries still run on Ubuntu 20.04 (glibc 2.31); 4.7.0+ require GLIBC_2.34. This box's WSL is 20.04" (`tools/get-astcenc.sh:11-12`).
- The sse2 variant is chosen for max compatibility; determinism doesn't depend on the SIMD variant, "but the pinned binary is part of the reproducibility contract" (`tools/get-astcenc.sh:5-8`). The `-thorough -j 1` determinism flags live in `make_splash_flash.py`, not in this fetch script.

### Firmware side (`MustangDash/MustangDash.ino`)

- **Probe first, never depend on flash.** Boot calls `EVE_init_flash()` then reads `REG_FLASH_SIZE` (capacity in MB); "A failure here is logged, never fatal -- the dash must not depend on flash" (`MustangDash/MustangDash.ino:268-275`). If the probe failed, the splash is skipped entirely and the dash proceeds (`MustangDash/MustangDash.ino:294-306`). On the bench the Riverdi RVT70H reports 64 MB.
- **Header compare = cheap idempotence.** `splash_header_current()` does one `EVE_cmd_flashread()` of the 64-byte header into RAM_G scratch (CMD_FLASHREAD wants src 64-byte aligned, dest 4-byte aligned, length a multiple of 4 — guide 5.80) and compares magic/version/length/count/crc against the compile-time constants baked into `splash_flash.h` (`MustangDash/MustangDash.ino:615-629`).
- **Provisioning in 32 KB CMD_FLASHUPDATE chunks** staged through a RAM_G scratch buffer near the top of RAM_G, far above the fonts (`SCRATCH_HDR = 0xFF000`, `SCRATCH_BUF = 0xF7000`, `PROVISION_CHUNK = 0x8000` at `MustangDash/MustangDash.ino:150-156`). CMD_FLASHUPDATE runs fine in the full-speed state `EVE_init_flash()` left the chip in, and only erases+writes 4 KB sectors whose content differs (guide 5.82 / Table 5).
- **The header chunk is written LAST — it is the commit record.** Quoting `MustangDash/MustangDash.ino:653-658`:

  > The 64-byte header lives in chunk 0, and splash_header_current() only ever reads the header back -- so the header chunk is the COMMIT RECORD and must be written LAST. Writing it first would let a power-cut provisioning run (header ok, later chunks never written) pass verification on every subsequent boot and render garbage assets forever (review finding: torn provisioning).

  The loop writes chunks 1..N first, then chunk 0 last; a re-read of the header verifies.
- **Clear the bitmap cache after any flash write.** `EVE_cmd_clearcache()` is called right after provisioning "so no stale blocks render (guide 5.89; the library helper swaps in the empty display lists CMD_CLEARCACHE requires first)" (`MustangDash/MustangDash.ino:673-676`).
- **Drawing is three display-list lines.** `draw_flash_asset()` (`MustangDash/MustangDash.ino:692-698`) passes `0x800000UL | (addr / 32UL)` — bit 23 flags a flash source, and the address is in 32-byte blocks. CMD_SETBITMAP accepts extended (>17) formats like `EVE_ASTC_8X8` and **emits BITMAP_EXT_FORMAT plus the LAYOUT(_H)/SIZE(_H) words itself** (BRT_AN_033 v2.8 section 5.75), so no manual BITMAP_EXT_FORMAT is needed. Filter comes out NEAREST, which is exact for the 1:1 draws used here.

## Why This Matters

- **The entire 1 MiB RAM_G is freed for the dash fonts.** The splash is no longer a RAM_G tenant at all — "RAM_G is untouched by the splash -- the dash fonts below are its only tenant" (`MustangDash/MustangDash.ino:63-66`).
- **The direct splash-to-dash crossfade came back.** The fade-through-black compromise died: "splash draws from flash, dash text from RAM_G fonts, so both are resident with no memory contention" (`MustangDash/MustangDash.ino:1471-1473`).
- **Full-resolution 1024x600 backgrounds for the first time.** The retired PNG pipeline downscaled backgrounds to 512x300 and had the firmware upscale with a 2x bilinear bitmap transform purely to fit RAM_G; the flash pipeline encodes native 1024x600 and draws 1:1, "no scale transform needed anymore" (`MustangDash/MustangDash.ino:700-701`).
- **Boot stopped re-decoding PNGs every power-up.** Provisioning is one-time. Bench-observed: the first boot wrote the 644 KB pack (SPLASH_FLASH_PACK_SIZE = 659,456 B), the second boot no-oped on the CRC match ("Splash flash assets current"). Rendering from flash sustained 109 frames over the 2000 ms splash (this session's bench log; the firmware's printed target is >= 100).
- **Cost:** the Teensy binary's `data` section grew from ~206 KB (the retired embedded-PNG splash build) to ~691 KB (PR #3) — the ~659 KB pack replaces ~198 KB of PNG headers, plus the dash's zlib font streams ride along. Cheap on a Teensy 4.1's 8 MB, and PROGMEM is memory-mapped so it costs no RAM.

## When to Apply

- BT81x-family (BT815/BT816/BT817/BT818) projects where RAM_G is contended — fonts, live bitmaps, and large static art that must coexist on screen. Move the *static* side to panel flash.
- Static assets only: the pack is provisioned once and compared by header/CRC, not streamed. NOT for content that changes at runtime — anything dynamic still belongs in RAM_G.
- **Probe-first discipline (the bench gate):** before building any of this pipeline, run `EVE_init_flash()` + `REG_FLASH_SIZE` on the real hardware and confirm the module actually has attached flash. Riverdi EVE4 modules ship the QSPI flash chip with the flashfast BLOB factory-programmed in sector 0 — which is why sector 0 must never be written and why `EVE_init_flash()` can jump straight to full-speed mode.
- Keep the firmware's flash-failure path honest: the splash is optional, the dash never depends on flash (`MustangDash/MustangDash.ino:294-306`).

## Examples

**Before (PNG-in-RAM_G, retired pipeline):** background PNG embedded in Teensy PROGMEM → `CMD_LOADIMAGE` decode into RAM_G every boot → ~307 KB of RAM_G for *one* background at HALF resolution (512x300, drawn with a 2x bilinear bitmap transform because full-res would not fit alongside everything else).

**After (ASTC-in-panel-flash):** the same background is native 1024x600 ASTC 8x8, exactly 153,600 bytes in the panel's QSPI flash (`MustangDash/splash_flash.h` — `SPLASH_FA_BG_BLUE_ADDR 111680`, `SPLASH_FA_BG_BLUE_SIZE 153600`), zero bytes of RAM_G, no boot-time decode. Half the bytes, four times the pixels.

**The draw call** (`MustangDash/MustangDash.ino:692-698`):

```c
void draw_flash_asset(const SplashFlashAsset *a, int16_t x, int16_t y)
{
    EVE_cmd_setbitmap(0x800000UL | (a->addr / 32UL), (uint16_t)a->fmt, a->w, a->h);
    EVE_cmd_dl(DL_BEGIN | EVE_BITMAPS);
    EVE_cmd_dl(VERTEX2F((int16_t)(x * 16), (int16_t)(y * 16)));
    EVE_cmd_dl(DL_END);
}
```

That is the whole runtime cost of a flash-resident asset: one CMD_SETBITMAP with the flash bit, one vertex.

## Related

- `docs/solutions/design-patterns/eve-ram-g-budgeting-multi-theme-splash-assets.md` — the RAM_G budgeting pattern this architecture supersedes *for the splash*; the budgeting technique itself still applies to whatever remains RAM_G-resident (today: the dash fonts), but its splash worked example and per-theme resident totals describe the retired architecture.
- `docs/solutions/ui-bugs/boot-splash-radial-gradient-banding-double-quantization.md` — the RGB565 dither pipeline that no longer applies to the splash background (ASTC quantizes by block, not by 5/6/5 truncation); still valid as general RGB565 dithering guidance.
- `docs/solutions/design-patterns/eve-logo-onchip-png-decode-skeleton-silhouette.md` — the on-chip PNG-decode-into-RAM_G pattern that was the project's previous default asset path; its subject (the pony screen) was removed by PR #3.
- `docs/solutions/best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md` — the BT817/EVE4 profile grounding that the ASTC + CMD_FLASHUPDATE feature set depends on.
