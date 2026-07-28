---
title: Luminous intensity is not comparable across parts, so match inside one series or not at all
date: 2026-07-28
category: conventions
module: part-selection
problem_type: convention
component: tooling
severity: high
applies_when:
  - "Choosing indicator LEDs from a catalogue by brightness"
  - "Matching a row of LEDs for even perceived brightness"
  - "Deriving series resistors from a photometric target"
  - "Comparing candidates across vendors, packages, or viewing angles"
tags: [led, optoelectronics, photometry, part-selection, lcsc, jlcpcb, telltales, board3]
---

# Luminous intensity is not comparable across parts

## Context

Board3's eight telltales are moving to a 3528 package (plan
`docs/plans/2026-07-27-003-feat-telltale-driver-and-rail-decoupling-plan.md`,
U11), and U12 re-derives their series resistors for **matched perceived
brightness**. Selecting parts meant reading luminous intensity off LCSC.

Yellow candidates, all nominally the same 3.5 x 2.8 mm two-pad package:

```text
  85 mcd   Everlight    67-21UYC            20 mA   120 deg
 119 mcd   Everlight    67-21/Y2C           10 mA   120 deg
 210 mcd   HONGLITRONIC HL-A-3528S31YC      20 mA   120 deg
 260 mcd   Gui guang    GL3528UY01          20 mA   120 deg
 400 mcd   XINGLIGHT    XL-3528UYC          20 mA   120 deg
 900 mcd   TOGIALED     TJ-S3528...C9Y      20 mA   120 deg
 1.4 cd    HONGLITRONIC HVY-3528CPX         20 mA   120 deg
  11 cd    HONGLITRONIC GT-3528U41YC        50 mA    30 deg
```

A 16x spread at the same current and beam angle, 130x including the last row.
That reads as enormous product differentiation. Most of it is not.

## Guidance

**Treat a millicandela figure as meaningful only against another figure from the
same series, at the same test current, at the same viewing angle.** Four
independent effects break comparison, and they compound:

1. **mcd is on-axis intensity, not flux.** The candela is lumens per steradian,
   so a narrow part concentrates comparable total light into a smaller cone and
   reports a larger number. The 11 cd part above is 30 deg; against a 120 deg
   part it covers roughly 1/16 the solid angle, so its headline figure says
   almost nothing about which looks brighter behind a diffusing mask. **This is
   the trap that inverts choices** — filtering a catalogue by "brightest" sorts
   by narrowest beam.
2. **Test current varies and is easy to miss.** 10, 20 and 50 mA all appear
   above, often only inside the free-text description rather than a field.
3. **Bin range within one part number.** Everlight's 3528 blue is quoted
   `72 mcd~180 mcd` — 2.5x, for a single orderable part. That spread is wider
   than the matching precision a brightness-matching exercise is chasing.
4. **Lens type.** Water clear, diffused, and white-lens variants of the same die
   report different on-axis intensity.

**The corollary that is good news:** the candela is *defined* against the CIE
V(lambda) photopic curve, so a millicandela figure already encodes the eye's
555 nm sensitivity peak. Matching mcd across colours **is** matching perceived
brightness — there is no additional photopic weighting to apply. Equal *current*
across colours is the wrong target; equal mcd is the right one, and it needs no
correction factor.

Practical rule: pick one vendor series and hold test current and beam angle
constant across the whole row. Effects 1, 2 and 4 vanish, leaving only binning,
and the relative numbers become trustworthy even when the absolute ones are not.

## Why This Matters

The failure is quiet. Every figure is real, published, and correct for the
condition it was measured at; nothing in a results table announces that the rows
are measured differently. A catalogue sorted by intensity looks like a
brightness ranking and is closer to a beam-angle ranking.

It also sets a hard floor on what "matched" can mean. With a 2.5x bin inside one
part number, no resistor derivation can hold a row closer than roughly +/-30%.
Specifying tighter is chasing precision the datasheets do not carry, and the
right response is either per-channel PWM trim at runtime or accepting the
tolerance — not more decimal places in the arithmetic.

For Board3 this decided the part set: holding HONGLITRONIC's HL-A series at
20 mA and 120 deg across **five of the six** colours is what made the intensity
figures usable at all, and it is why a 350 mcd single-value blue was chosen over
a `72-180 mcd` one that was otherwise equivalent.

**Orange is the exception, and the exception is the more useful half of the
lesson.** There is no HL-A 3528 orange at LCSC, so that position takes
`HVO-3528CPXA` (`C5246349`) from a different series -- and it arrives with a
white diffusing lens where the other five are water clear. Its 1.8 cd is
therefore not comparable to the five HL-A figures in the way those are
comparable to each other, and no arithmetic fixes that. The practical rule when
a row cannot be held in one series: **match within the series, then treat the
outlier as a bench-trim position** rather than pretending its catalogue number
sits on the same axis.

A second gap worth expecting: the white (`C516299`) publishes **no luminous
intensity at all** -- the field is `-`, with a 6500 K colour temperature in its
place. White LEDs are routinely specified by luminous flux or chromaticity bin
instead, so a row containing white will usually have one position whose figure
has to come from the datasheet's bin table or from the bench, not the catalogue.

## When to Apply

- Selecting indicator LEDs by brightness from any distributor catalogue
- Matching a row of telltales, status LEDs, or backlights for even appearance
- Deriving series resistors from a target luminous output
- Any time a candidate looks dramatically brighter than its peers — check the
  viewing angle and test current before believing it
- Not relevant when a single LED's absolute output is the only requirement and
  no comparison is being made

## Examples

The whole learning in two rows from the table above:

```text
 900 mcd   TOGIALED     TJ-S3528...C9Y      20 mA   120 deg
  11 cd    HONGLITRONIC GT-3528U41YC        50 mA    30 deg
```

The second looks 12x brighter. It is measured at 2.5x the current, into roughly
1/16 the solid angle. Behind an icon mask the first is the better part, and a
sort by intensity puts it last.

## Related

- [Search a package by both its JEDEC name and its metric dimensions](../developer-experience/search-a-package-by-both-its-jedec-and-metric-names.md)
  — the sibling from the same part-selection pass. That one is the search
  surface returning a slice; this one is the returned data not meaning what it
  appears to.
- [DRC clean and measured is not assemblable](drc-clean-and-measured-is-not-assemblable.md)
  — same shape: a number that passes inspection while answering a narrower
  question than the one being asked.
- `docs/plans/2026-07-27-003-feat-telltale-driver-and-rail-decoupling-plan.md`
  — KTD9 and U12, the perceived-brightness derivation this constrains.
