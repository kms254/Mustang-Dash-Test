---
title: CMD_SETBITMAP after font registration clobbers the current font handle
date: 2026-07-21
category: ui-bugs
module: dash-rendering
problem_type: ui_bug
component: tooling
symptoms:
  - "Rectangular patches of scrambled colored blocks (ASTC-noise look) exactly where one font's glyphs should render; text drawn with other fonts renders fine"
  - "Serial health is perfect while the screen is wrong -- fps=60, faults=0, REG_ID stable; another render-corruption class invisible to serial checks"
root_cause: wrong_api
resolution_type: code_fix
severity: high
tags: [eve, bt817, cmd-setbitmap, bitmap-handle, fonts, ram-g, dash-render]
---

# CMD_SETBITMAP after font registration clobbers the current font handle

## Problem

Adding a full-screen RAM_G bitmap as a base layer under the STREET dash
(2026-07-21 carbon-backdrop trial on the F767 rig) scrambled parts of the
dash: blocky colored garbage rendered where certain UI elements should be,
while gauges, needles, and most numerals drew normally.

## Symptoms

- Scrambled ASTC-noise rectangles at the screen positions of one font's
  glyphs; everything else correct (bench photo diagnosis).
- All serial health signals clean (`fps=60`, `faults=0,0,0`, `REG_ID 0x7C`)
  -- serial acks cannot see render corruption, same blind-spot family as
  the L4/L2 font-format bug
  (`docs/solutions/ui-bugs/eve-font-format-l4-l2-confusion-serial-verification-blind-spot.md`).

## What Didn't Work

Nothing else was tried -- the photo made the mechanism readable directly:
only elements using one specific font were garbled, which pointed at a
per-handle problem rather than a staging, swizzle, or clock problem (all
of which garble *the bitmap*, not *someone else's glyphs*).

## Solution

`CMD_SETBITMAP` configures whatever bitmap handle is **currently
selected** -- it does not allocate one. The dash frame registers its fonts
first (`dash_register_fonts`), which leaves the current handle parked on
the last font registered; issuing `CMD_SETBITMAP` there re-pointed that
font's handle at the background bitmap.

Fix: select a scratch handle above the font set before the draw, restore
handle 0 after:

```c
EVE_cmd_dl(BITMAP_HANDLE(15UL));          /* scratch, above all dash fonts */
EVE_cmd_setbitmap(src, fmt, w, h);
EVE_cmd_dl(DL_BEGIN | EVE_BITMAPS);
EVE_cmd_dl(VERTEX2F(0, 0));
EVE_cmd_dl(DL_END);
EVE_cmd_dl(BITMAP_HANDLE(0UL));           /* restore the default handle */
```

The trial layer was later retired (STREET returned to flat black by
choice), but the lesson is pinned as a NOTE comment at the STREET branch
of `draw_dash_content` in `MustangDash/dash_render.h` for the next
full-screen bitmap.

## Why This Works

EVE bitmap state (source, layout, size, format) lives per-handle, and
`CMD_SETBITMAP` is sugar that writes that state to the *current* handle.
Text rendering assumes font handles keep the configuration
`dash_register_fonts` gave them for the whole frame. Any mid-frame
`CMD_SETBITMAP` therefore silently retargets whichever handle happens to
be selected. The splash never hits this because every splash element
re-runs `CMD_SETBITMAP` immediately before its own vertices each frame
(`MustangDash/splash_render.h`), so no draw depends on handle state
surviving from earlier in the frame.

## Prevention

- Any bitmap draw inside dash frame content (after font registration) must
  explicitly select a handle that no font uses, and restore handle 0
  afterwards -- never issue `CMD_SETBITMAP` on whatever handle is current.
- Diagnostic signature to remember: corruption confined to one font's
  glyphs with clean serial health = a handle got reconfigured mid-frame;
  corruption of the bitmap itself = staging/swizzle/clock problem.
- Eyes-on-panel remains a mandatory acceptance signal for any render-path
  change; no serial check catches this class.

## Related Issues

- `docs/solutions/ui-bugs/eve-font-format-l4-l2-confusion-serial-verification-blind-spot.md`
  -- the serial-verification blind spot this bug re-confirmed.
- `docs/solutions/architecture-patterns/bt817-flash-render-streaming-bandwidth-ceiling.md`
  -- why full-screen bitmaps render from RAM_G in the first place.
