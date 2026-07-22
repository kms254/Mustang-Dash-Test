---
module: dash-rendering
date: 2026-07-22
problem_type: tooling_decision
component: tooling
severity: medium
applies_when:
  - Adding characters to an existing EVE bitmap font instance
  - Choosing between a font glyph and a pre-rendered bitmap for a short piece of UI text
  - Estimating RAM_G or flash impact of a font change before making it
tags:
  - eve
  - fonts
  - ram-g
  - asset-budget
  - cost-estimation
---

# Costing a character addition to an EVE fixed-cell font

## Context

The dash needed to render the word `BEST!` in its large number font, which carried
only `0123456789:.-+` — no letters. That raised two questions with non-obvious
answers: what does adding characters actually cost, and is a pre-rendered bitmap
cheaper than extending the font?

Both estimates made during the work were wrong, in the same direction, by roughly
8x. The cost model below is what they were missing.

## Guidance

**Cost is driven by two multiplied factors, and the second one is easy to miss.**

**1. The contiguous codepoint span, not the glyph count.** EVE legacy fonts store a
cell for every codepoint between the lowest and highest character in the set — the
generator takes `min(ord)` to `max(ord)`. Adding five letters to a numeric font does
not add five cells:

| Glyph set | Span | Cells |
|---|---|---|
| `0123456789:.-+` | `+`(43)..`:`(58) | 16 |
| `…+BEST` | `!`(33)..`T`(84) | 52 |
| `…+` and full A-Z | `!`(33)..`Z`(90) | 58 |

Adding `BEST` costs 36 blank cells to get 4 letters. Note the corollary: **once you
have paid the jump to the letter range, the rest of the alphabet is nearly free in
cell count** — `T`(84) to `Z`(90) is 6 more cells.

**2. Every cell resizes to fit the largest glyph.** This is the factor both estimates
omitted. Cells are a fixed size across the instance, so introducing a wide character
widens *every* cell — including all the digits that were already there.

Measured on this font (`MustangDash/dash_fonts.h:647-649`):

| | Cells | Cell size | RAM_G |
|---|---|---|---|
| digits only | 16 | 26x36 | 7,636 B |
| + `BEST` | 52 | 26x36 | 24,484 B |
| + full A-Z | 58 | **38x42** | 46,432 B |

The last row is the trap. Cell count rose only 12%, but `W` forced the cell from
26x36 to 38x42 and RAM_G nearly doubled. **Estimating cells alone predicted +2.8 KB;
actual was +22 KB.**

**Flash barely moves.** Blank cells compress to almost nothing — this instance's
46,284 B of glyph data is 4,627 B as a zlib stream. The cost lands on RAM_G, which
holds the *inflated* data.

**Choosing font vs. pre-rendered bitmap.** A bitmap is smaller in bytes, so the naive
answer is usually wrong. Prefer the font unless RAM_G is genuinely scarce, because
the bitmap costs things that do not show up in a size comparison:

- **A bitmap handle**, which is a scarcer resource than RAM_G here — the platform has
  32, and fonts, widget scratch, and ROM fonts already claim nearly all of them.
- **A separate asset pipeline**, staging path, and readback check, none of which a
  font glyph needs.
- **It cannot compose.** A bitmap word cannot sit inline with a number in the same
  font at the same baseline; you end up positioning a bitmap against text metrics
  by hand.
- **It buys exactly one word**, where the font change makes any word renderable.

## Why This Matters

The cost is small in absolute terms here — RAM_G ended at 323,548 B against roughly
a megabyte available, with fonts as its only tenant. But the *estimate* was wrong by
8x twice in a row, and an 8x error against a tighter budget is how a change gets
reverted after it has already been built and wired up.

The reason both estimates missed is worth naming: cell count is the visible variable,
so it is what gets counted. Cell *size* is a property of the whole instance that
silently changes when a single wide character enters, which makes it easy to treat as
constant when it is not.

## When to Apply

Any change to an EVE bitmap font instance's glyph set, and any "should this be text or
an image" decision on this dash. Also worth applying whenever a fixed-cell or
fixed-stride format is involved — the "one wide member resizes every slot" behaviour
is a property of the format family, not of this particular tool.

## Examples

Estimating before the change, correctly:

```
new span   = max(ord) - min(ord) + 1          # not the count of added glyphs
new cell   = bounding box of the WIDEST and TALLEST glyph in the new set
new RAM_G  ~= new span * new cell_w * new cell_h / 2   # L4 = 4 bits per pixel
```

For the A-Z addition above: `58 * 38 * 42 / 2 = 46,284 B` of glyph data, which is
what the generator reported. Running that arithmetic *before* the change would have
produced the right number the first time.

If the answer is uncomfortably large, the cheapest lever is usually the span rather
than the set — a glyph far from the existing range (a low-ASCII symbol, say) can cost
more in blank cells than the characters you actually wanted.
