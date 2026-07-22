---
title: "Fitting multi-theme splash assets under EVE RAM_G: downscale, pack, verify"
date: 2026-07-09
category: design-patterns
module: display-bringup
problem_type: design_pattern
component: tooling
severity: medium
applies_when:
  - "budgeting assets that must be RAM_G-resident at render time (decoded bitmaps, uploaded fonts) on a BT81x/EVE panel"
  - "multiple decoded assets must be resident simultaneously (crossfade, layered UI) under a fixed RAM_G ceiling"
  - "supporting several selectable themes/asset sets that must not all occupy RAM_G at once"
  - "deciding whether a soft/gradient asset can be shipped downscaled and rendered via bitmap transform without visible loss"
  - "needing a build-time budget check plus a runtime verification that decoded totals match the plan"
last_updated: 2026-07-10
tags:
  - ram-g
  - memory-budget
  - bt817
  - eve4
  - asset-packing
  - bilinear-scaling
  - splash-screen
  - multi-theme
---

# Fitting multi-theme splash assets under EVE RAM_G: downscale, pack, verify

> **Scope note (2026-07-10):** the worked example below — the boot splash — no
> longer lives in RAM_G: PR #3 migrated the splash to ASTC assets in the
> panel's QSPI flash, rendered directly from flash (see
> `docs/solutions/architecture-patterns/bt817-flash-resident-astc-assets.md`).
> The budgeting technique itself is unchanged and still governs whatever must
> be RAM_G-resident — today, the dash's custom fonts, whose loader logs
> `RAM_G: fonts N bytes (headroom M)` in exactly this doc's pattern. Read the
> splash-specific numbers and load-order details below as a historical case
> study of the technique.

> **Scope note (2026-07-21):** counter-note to the above — the splash is back
> in RAM_G. The 2026-07-21 MCU-direct rewrite stages the embedded ASTC pack
> straight from MCU flash to RAM_G at boot (the panel's QSPI flash is no
> longer used at all); peak resident footprint is the custom fonts (~285 KB)
> plus the largest theme (~301 KB), roughly 586 KB of the 1 MiB budget. This
> budgeting technique governs that footprint again — see
> `docs/solutions/architecture-patterns/bt817-flash-resident-astc-assets.md`
> for the pack format, which now targets RAM_G instead of panel flash.

## Context

The BT817 renders bitmaps from RAM_G — 1 MiB, `#define EVE_RAM_G_SIZE ((uint32_t) 1024UL*1024UL)` (libraries/FT800-FT813/src/EVE.h:103) — with one exception: ASTC-compressed bitmaps in attached QSPI flash render directly from flash (the escape hatch the splash later took). Compressed PNG size is irrelevant to this budget: on-chip decode expands every asset to its full raster footprint (ARGB4 and RGB565 are both 2 bytes/pixel), so a single full-screen 1024x600 background costs 1,228,800 B — more than the entire chip.

The boot splash (merged in PR #2; asset storage later rearchitected in PR #3) made the constraint acute in two ways:

1. **Composition depth.** The final frame stacks a full-screen background, emblem, wordmark, side bars/blocks, accent line, year, and (checkered theme) two edge strips.
2. **Crossfade residency.** The splash-to-pony transition draws both compositions in the same display list per frame (`draw_splash_elements` calls `draw_splash_background` and the caller mixes in the pony via `DL_COLOR_A`), so the splash asset set *and* the 480x300 pony (MustangDash/pony_png.h:13-14, 288,000 B decoded) must be resident simultaneously. There is no "swap assets mid-transition" escape hatch.

Multiply by three build-selectable themes and the naive plan is dead on arrival. The pattern below is how the splash fits everything with verified headroom.

## Guidance

**1. Inventory decoded footprints at plan time.** Budget in decoded bytes (w x h x 2), never in PNG bytes. The asset generator makes this mechanical: `emit_asset` (tools/make_splash_assets.py:128) emits `#define` width/height/format per asset, and `write_header` (tools/make_splash_assets.py:148) appends a footer per generated header — `"/* decoded RAM_G total for this header: {total} bytes */"` (tools/make_splash_assets.py:165). The implementation plan carried a per-theme budget table with a worst-case total before any code was written (docs/plans/2026-07-09-001-feat-boot-splash-plan.md:148-162).

**2. Downscale the perceptually forgiving layer.** The background is a soft radial gradient, so it ships at half resolution — `BG_DOWNSCALE = (512, 300)` (tools/make_splash_assets.py:37), 307,200 B instead of an impossible 1,228,800 B — and is drawn 2x with bilinear filtering to fill the panel:

```c
EVE_cmd_dl(BITMAP_SIZE_H(EVE_HSIZE, EVE_VSIZE));
EVE_cmd_dl(BITMAP_SIZE(EVE_BILINEAR, EVE_BORDER, EVE_BORDER,
                       EVE_HSIZE & 0x1FFU, EVE_VSIZE & 0x1FFU));
EVE_cmd_dl(CMD_LOADIDENTITY);
EVE_cmd_scale(2L * 65536L, 2L * 65536L); /* 16.16 fixed point */
```

(MustangDash/MustangDash.ino:327-331, inside `draw_splash_background`). Soft gradients hide a 2x bilinear upscale; text, logos, and hard edges would not — those stay at native resolution. Two traps documented at the call site (MustangDash/MustangDash.ino:319-322): any draw window above 511 px overflows the 9/10-bit base fields, so the `BITMAP_SIZE_H`/`BITMAP_LAYOUT_H` companion words are required (shipped as active inline helpers, libraries/FT800-FT813/src/EVE.h:659-687), and `CMD_SETBITMAP` defaults to nearest filtering (per Bridgetek's BT81x programming guide), so the SIZE words must be re-emitted with `EVE_BILINEAR`. See docs/solutions/ui-bugs/boot-splash-radial-gradient-banding-double-quantization.md for keeping the downscaled gradient band-free.

**3. Pack sequentially, biggest first, headroom at the top.** `load_png_asset` assigns `dst->addr = g_next_addr`, decodes with `EVE_cmd_loadimage`, then advances by w x h x 2 rounded up to 4-byte alignment (MustangDash/MustangDash.ino:271-281). `load_splash_assets` resets `g_next_addr = 0` and loads in descending-size order — background (307,200), pony (288,000), wordmark (112,000), emblem (80,000), then the small chrome (MustangDash/MustangDash.ino:292-303) — so free space stays contiguous at the top of RAM_G. Why the top matters: the plan documents the assumption that the BT817's PNG decoder uses the top of RAM_G as scratch during `CMD_LOADIMAGE` (docs/plans/2026-07-09-001-feat-boot-splash-plan.md:166; restated at MustangDash/MustangDash.ino:109-112). That is a plan-documented assumption the decode order honors — it is not backed by a datasheet citation in this tree — but the failure mode it guards against (a later decode's scratch clobbering an earlier asset) is exactly why big-first packing is the safe default.

**4. Log the budget at runtime.** `load_splash_assets` closes with the live figure over serial (MustangDash/MustangDash.ino:305-307):

```c
Serial.printf("RAM_G: %lu bytes of assets loaded (headroom %lu)\r\n",
              (unsigned long)g_next_addr,
              (unsigned long)(EVE_RAM_G_SIZE - g_next_addr));
```

This turns the plan table into a checkable fact on every boot: if an asset regenerates larger, the drift shows up in the log before it shows up as corruption.

**5. Residency is per-theme; embedding is total.** Only the compiled theme's PNGs decode into RAM_G, but all three themes' byte arrays stay in the Teensy's flash via the `THEMES[3]` descriptor table (MustangDash/MustangDash.ino:78) indexed through `static volatile uint8_t g_theme` — volatile specifically so the compiler cannot fold the table access to one theme and let the linker garbage-collect the other two (comment at MustangDash/MustangDash.ino:104-107, plan requirement R5). RAM_G budgets the worst-case theme; flash budgets the sum.

This pattern generalizes the earlier single-asset on-chip PNG-decode approach (the since-retired pony logo) to a multi-asset, multi-theme working set.

## Why This Matters

Without step 1, the design phase produces compositions that physically cannot fit — the full-res background alone exceeds RAM_G, and that is only discoverable by arithmetic, never by looking at PNG file sizes (the 512x300 checkered background is a 30,052 B PNG that costs 307,200 B decoded — a 10x gap).

Without steps 3-4, overflow is *silent*. `EVE_cmd_loadimage` does not range-check your packing plan; blowing past the ceiling — or decoding into the region the decoder is assumed to use as scratch — corrupts already-loaded bitmaps with no error return, showing up later as garbage pixels that look like a rendering bug, not a memory bug.

The plan-time table plus the runtime log form a closed loop: the table predicts, the generator's per-header footer totals restate the prediction from real asset dimensions, and the serial line reports what actually happened on hardware. On this project the loop closed exactly — the bring-up serial capture during PR #2 work matched the arithmetic to the byte — which is what makes the *next* asset addition safe: any mismatch between the three numbers is drift, caught at boot rather than debugged off a corrupted screen.

## When to Apply

- Any EVE/BT81x target — or any display-list GPU with small dedicated bitmap memory — where the desired artwork, summed at decoded bytes/px, approaches or exceeds that memory.
- Whenever adding or resizing any asset in this project: re-run the budget (regenerate headers, check the footer totals, watch the boot-time `RAM_G:` serial line).
- Crossfades, transitions, or overlays that require two screens' asset sets resident in the same display list — budget the union, not the max.
- Multi-theme or multi-variant builds: budget RAM_G per variant (worst case), budget flash for the total, and make sure the linker cannot strip the inactive variants if they must stay selectable.

## Examples

Concrete numbers from this tree (all decoded sizes are w x h x 2 B):

- **The impossible baseline:** 1024x600 full-res background = 1,228,800 B vs `EVE_RAM_G_SIZE` = 1,048,576 B (libraries/FT800-FT813/src/EVE.h:103). Over budget before the second asset.
- **The downscale win:** background at 512x300 = 307,200 B — a 4x reduction bought with one bilinear 2x transform (MustangDash/MustangDash.ino:327-331).
- **Generator footer totals (in-tree):** common assets 213,600 B (MustangDash/splash_assets_common.h:3402), blue and red 345,600 B each (MustangDash/splash_assets_blue.h:1920, MustangDash/splash_assets_red.h:1669), checkered 405,008 B (MustangDash/splash_assets_checkered.h:2157). Note these are per-*header* totals; the per-theme *resident* set is background + pony + the subset each theme actually loads.
- **Per-theme resident totals:** blue/red = 847,200 B (headroom 201,376); checkered (worst case) = 885,008 B, headroom 163,568 B — about 156 KiB left at the top of RAM_G for the assumed decoder scratch. These follow arithmetically from the in-tree asset dimensions and the load list at MustangDash/MustangDash.ino:292-303, and the hardware serial log during PR #2 bring-up reported the same figures exactly.
- **Plan vs reality:** the plan's budget table predicted ~885 KB of 1,048 KB worst case and ~847 KB for blue/red (docs/plans/2026-07-09-001-feat-boot-splash-plan.md:148-162) — the shipped numbers landed on those predictions to the byte.

## Related

- [docs/solutions/ui-bugs/boot-splash-radial-gradient-banding-double-quantization.md](../ui-bugs/boot-splash-radial-gradient-banding-double-quantization.md) —
  the companion fix for the downscaled layer: same converter and 2x-bilinear
  render path, different concern (color banding vs the byte budget).
- [docs/solutions/best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md](../best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md) —
  defines the panel resolution and EVE generation the budget arithmetic is
  built against.
