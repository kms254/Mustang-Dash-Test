---
title: "VERTEX2F 15-bit signed coordinate wraparound at x >= 1024px on the RVT70H panel"
date: 2026-07-10
category: ui-bugs
module: display-bringup
problem_type: ui_bug
component: tooling
symptoms:
  - "Splash round: the checkered edge strip's wraparound copy at x=1299px wrapped to x=-749px mid-slide, drawing on the left side of the panel instead of sliding in from the right"
  - "Dash round: the alarm takeover's full-screen rect used VERTEX2F(EVE_HSIZE*16, ...) = 16384, one past the max representable 15-bit value, overflowing the field (caught pre-flash by code review, not on hardware)"
root_cause: logic_error
resolution_type: code_fix
severity: medium
tags:
  - eve
  - bt817
  - vertex2f
  - coordinate-overflow
  - wraparound
  - display-list
  - 1024px
---

# VERTEX2F 15-bit signed coordinate wraparound on the 1024-px panel

## Problem

`VERTEX2F` packs x and y as **15-bit signed fields in 1/16-pixel units**. The
encoder in the vendored library is a plain bitfield mask — it silently
truncates anything wider
(`libraries/FT800-FT813/src/EVE.h:1026-1031`):

```c
static inline uint32_t VERTEX2F(const int16_t xc0, const int16_t yc0)
{
    uint32_t const xc0v = ((((uint32_t) ((uint16_t) xc0)) & 0x7FFFUL) << 15U);
    uint32_t const yc0v = (((uint32_t) ((uint16_t) yc0)) & 0x7FFFUL);
    return (DL_VERTEX2F | xc0v | yc0v);
}
```

Signed 15 bits means the largest positive coordinate is **16383 sixteenths =
1023.94 px**. This panel (EVE_RVT70H) is exactly 1024 px wide —
`#define EVE_HSIZE ((uint32_t) 1024UL)` at
`libraries/FT800-FT813/src/EVE_config.h:909` — so the panel width itself is
unrepresentable: pixel-x 1024 encodes as `1024 * 16 = 16384`, which sets the
sign bit of the 15-bit field. The vertex lands in the **far-left negative
region (around -1024 px)**. There is no error and no clipping; the geometry
just draws in the wrong place. On this hardware the panel width sits exactly
one pixel past the last representable coordinate, so *any* x that merely
touches the right edge in "one-past-the-end" style is already wrapped.

Companion trap: `VERTEX2II` is **unsigned 9-bit** — max 511 px — so it cannot
address the right half of a 1024-px panel at all
(`libraries/FT800-FT813/src/EVE.h:1038-1041`):

```c
static inline uint32_t VERTEX2II(const uint16_t xc0, const uint16_t yc0, const uint8_t handle, const uint8_t cell)
{
    uint32_t const xc0v = ((((uint32_t) xc0) & 0x1FFUL) << 21U);
    uint32_t const yc0v = ((((uint32_t) yc0) & 0x1FFUL) << 12U);
```

All sketch drawing goes through the `x * 16` encode (e.g. `draw_flash_asset`
at `MustangDash/MustangDash.ino:696`:
`EVE_cmd_dl(VERTEX2F((int16_t)(x * 16), (int16_t)(y * 16)));`), so every
pixel-space x ≥ 1024 is a wrap waiting to happen.

## Symptoms

**Instance 1 — splash checkered strip (fixed in PR #2, merged).** The bottom
edge strip slides in and is drawn twice to cover the right-edge sliver. Per
the splash-round review, the wraparound copy's computed x reached ~1299 px
mid-slide → 20784 sixteenths → wrapped to roughly **-749 px**, drawing a strip
fragment sliding across the wrong (left) edge of the screen. Visible artifact;
caught by adversarial code review plus the validator.

**Instance 2 — alarm takeover rect (fixed in PR #3, open as of this
writing).** The alarm's dark full-screen rectangle used the natural
"one-past-the-end" bottom-right corner `EVE_HSIZE * 16 = 16384` → sign bit set
→ corner wraps negative and the "full-screen" rect would have
collapsed/mangled instead of covering the panel. Caught by the orchestrator
pre-flash — self-noticed during implementation because the splash lesson was
fresh.

## What Didn't Work

- **Waiting for anything to fail loudly.** Nothing does. The encode is a
  silent bitfield mask (`& 0x7FFFUL`), so out-of-range values are truncated,
  not rejected.
- **Relying on the C type system.** The `int16_t` parameter means a computed
  16384 is already implementation-defined in the cast *before* the mask ever
  runs — there is no well-defined place to detect it after the call.
- **Relying on the host-side invariant tests.** They pin the display profile
  and animation math headers; the overflow happens in the display-list
  *encoding* inside the sketch's draw path, which the host tests never
  execute. Both instances got past compilation and the test suite untouched.

## Solution

**Fix 1 — gate the animated wraparound copy so it is only emitted while
on-screen** (`MustangDash/MustangDash.ino:772-780`):

```c
            /* wraparound copy for the right-edge sliver -- drawn only once it
             * starts on-screen: past EVE_HSIZE the x*16 value exceeds
             * VERTEX2F's signed 15-bit field and would wrap to the left side
             * of the panel mid-slide, defeating the slide-in */
            const int16_t wrap_x = (int16_t)(bot_x + (int16_t)strip->w);
            if (wrap_x < (int16_t)EVE_HSIZE)
            {
                draw_flash_asset(strip, wrap_x, SPLASH_CSTRIP_BOT_Y);
            }
```

**Fix 2 — clamp the full-screen rect's right edge to the last representable
1/16 px** (`MustangDash/MustangDash.ino:1356-1360`):

```c
        EVE_cmd_dl(VERTEX2F(0, 0));
        /* bottom-right corner: EVE_HSIZE*16 = 16384 overflows VERTEX2F's
         * signed 15-bit field (the splash-round wraparound lesson), so the
         * rect ends at the last representable 1/16 px inside the panel */
        EVE_cmd_dl(VERTEX2F((int16_t)(EVE_HSIZE * 16 - 1), (int16_t)(EVE_VSIZE * 16)));
```

(y is safe unclamped: `EVE_VSIZE` is 600 at
`libraries/FT800-FT813/src/EVE_config.h:910`, so `600 * 16 = 9600` fits
comfortably in 15 signed bits.)

## Why This Works

- The splash gate keeps every emitted pixel-x in `[0, 1023]`, so every
  `x * 16` stays ≤ 16368 — inside the positive range of the 15-bit signed
  field. Off-screen pieces are simply not emitted, which is also what the
  slide-in animation visually wants.
- The rect corner at `EVE_HSIZE * 16 - 1 = 16383` sixteenths is the **last
  representable 1/16-px inside the panel** — 1023.9375 px. That is
  sub-pixel-identical to 1024 on screen (the rasterizer fills to the panel
  edge), fully representable, and cannot wrap. Treating `EVE_HSIZE * 16` as
  one-past-the-end and subtracting 1 converts an unencodable coordinate into
  an exact visual equivalent.

## Prevention

1. **Gate or clamp before the `*16` encode.** Any computed or animated x can
   exceed the panel; the check must happen in pixel space, before conversion.
   Treat `EVE_HSIZE * 16` as one-past-the-end: use `EVE_HSIZE * 16 - 1` for
   the right/bottom edges of full-screen geometry, and an
   `if (x < EVE_HSIZE)` gate for pieces that may slide off-screen.
2. **VERTEX2II is never an alternative on panels wider than 511 px.** Its
   x/y fields are unsigned 9-bit (`& 0x1FFUL`, `EVE.h:1040-1041`). On the
   1024-px RVT70H, VERTEX2F is the only vertex opcode that can reach the
   right half of the screen.
3. **When geometry must legitimately extend past an edge** (slide-in
   animations, oversized bitmaps), don't emit raw out-of-range vertices.
   Either gate per-piece as the splash does, or move the origin with the
   17-bit translate registers and keep the vertex itself small —
   `libraries/FT800-FT813/src/EVE.h:1062-1065`:

   ```c
   static inline uint32_t VERTEX_TRANSLATE_X(const int32_t xco)
   {
       return (DL_VERTEX_TRANSLATE_X | (((uint32_t) xco) & 0x1FFFFUL));
   }
   ```

   (paired with `VERTEX_FORMAT` at `EVE.h:1052` to control VERTEX2F
   precision).
4. **Twice-bitten note.** This trap recurred within two working days, across
   two different features (splash animation, then dash alarm), produced by
   the same authorship pipeline. The pattern to internalize is mechanical:
   **every VERTEX2F emission near a screen edge gets either a clamp with a
   comment or an on-screen gate** — the two shipped fixes above are the
   templates. If a new draw call computes x from animation state or uses
   `EVE_HSIZE` in a corner, it is in scope for this rule by default.

## Related

- `docs/solutions/design-patterns/eve-logo-onchip-png-decode-skeleton-silhouette.md` — carries the prior, partial mention of this territory ("VERTEX2F coordinates are in 1/16-pixel units — multiply by 16") without the signed-15-bit ceiling that makes the unit conversion a trap on a 1024-px panel; this doc supplies the missing half.
- `docs/solutions/ui-bugs/eve-font-format-l4-l2-confusion-serial-verification-blind-spot.md` — same bug family: a silently-wrong value encoded into an EVE structure, nothing faults, serial verification cannot see it, only the panel (or an independent reviewer) reveals it.
- `docs/solutions/design-patterns/eve-ram-g-budgeting-multi-theme-splash-assets.md` — sibling field-width trap at the same 511-px boundary family: BITMAP_SIZE's 9/10-bit base fields need the _H companion words on wide draws, just as VERTEX2II's 9-bit fields disqualify it entirely here.
- `docs/solutions/architecture-patterns/bt817-flash-resident-astc-assets.md` — `draw_flash_asset()`'s `VERTEX2F(x*16, y*16)` is the code site most likely to meet this overflow when drawing 1024-px-wide flash assets; its callers gate coordinates per this doc's rule.
