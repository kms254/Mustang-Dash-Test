---
title: Search a package by both its JEDEC name and its metric dimensions, because LCSC indexes whatever string the vendor filed
date: 2026-07-28
category: developer-experience
module: part-sourcing
problem_type: developer_experience
component: tooling
severity: high
applies_when:
  - "Searching LCSC/JLCPCB by package name for a part, colour, or value"
  - "About to conclude that a part is scarce, single-sourced, or unavailable"
  - "A sourcing stop condition is about to halt work on a stock number"
  - "A package migration is being considered on availability grounds"
root_cause: missing_workflow_step
resolution_type: workflow_improvement
tags: [lcsc, jlcpcb, part-sourcing, bom, pcbparts, package-naming, board3]
---

# Search a package by both its JEDEC name and its metric dimensions

## Context

Board3's telltale row is moving from 0805 to PLCC-2 (plan
`docs/plans/2026-07-27-003-feat-telltale-driver-and-rail-decoupling-plan.md`,
U11). Searching LCSC for `package = "PLCC-2"` returned **98 parts** — OSRAM,
Broadcom, Vishay, Lite-On, every one `extended`. The best-stocked orange/amber
candidate held **79 units**, single-sourced. That trips the plan's stop
condition ("stop and report ... if a part cannot be sourced with complete LCSC
metadata"), and the fallback being weighed was migrating the whole row to
PLCC-4.

The same physical package searched as `SMD3528-2P` / `3528` returns
**194 parts**:

```text
orange/amber   best stock     79  ->  14,654   (HONGLITRONIC HL-A-3528S22OC)
blue           best stock    100  ->  45,998
result count                   98 ->     194
```

Same 3.5 × 2.8 mm two-pad body, same reel, interchangeable at the schematic
level. (**Not** necessarily the same land pattern — see "a package name is not a
land pattern" below.) LCSC's `package`
field is **manufacturer-indexed, not normalized** — whatever string the vendor
filed is what gets indexed. Tier-1 Western vendors file the JEDEC name; tier-2
Asian vendors (XINGLIGHT, HONGLITRONIC, TOGIALED, Everlight, Amicc, Gui guang)
file the metric dimensions of the identical part. A query for one **never**
returns the other, and nothing in the result set indicates you saw a slice.

Board3's own BOM already carries the split in plain sight: `LED4`/`LED5` are
XINGLIGHT `XL-2012UBC` / `XL-2012UOC` — **2012 is the metric spelling of
0805** — sitting in a footprint named `LED0805-RD`.

## Guidance

**Search every package at least two ways — JEDEC name and metric dimensions —
before drawing any conclusion about availability.** A low stock count from a
single-spelling search is *unverified*. It is not a sourcing conclusion, it is
not evidence for a package change, and it must not fire a stop condition.

The `pcbparts` `jlc_search` `packages` array is OR-matched, so both spellings
cost one call, not two:

```python
jlc_search(packages=["PLCC-2", "SMD3528-2P", "3528"], ...)
```

Pairs worth knowing. The rule matters more than the table — when a package is
not listed here, assume it has a second spelling and search for one:

| JEDEC / imperial | Metric / alternate | Confirmed |
|---|---|---|
| PLCC-2 | `SMD3528-2P`, `3528` | yes — 98 vs 194 parts, 2026-07-28 |
| SOIC-18 | `SOP-18` | yes — both live in this repo, below |
| PLCC-4 | `3528-4P` | expected, unverified |
| SOT-23 | `SOT-23-3` | expected, unverified |
| 0805 / 0603 / 0402 / 1206 | `2012` / `1608` / `1005` / `3216` | standard imperial↔metric; LCSC usually carries both |

SOIC/SOP is live in this repo right now. The ULN2803A being replaced sits in
`SOP-18_L11.4-W7.6-P1.27-LS10.6-BL`; its replacement TBD62083AFWG sits in
`SOIC-18_L11.6-W7.5-P1.27-LS10.3-BL`. Same 18-pin 1.27 mm gullwing part, two
package strings — a `SOIC-18` filter would not have found the incumbent, and a
`SOP-18` filter would not have found the replacement.

Passives bite less: LCSC carries both spellings for most chip resistors and
capacitors. "Less" is not "never", and the second array element is free.

Two things to check once the *real* candidate set is visible:

- **`library_type` is a cost axis.** `basic`/`preferred` parts ship without
  JLC's per-part feeder fee; every `extended` part adds one. Here it did not
  discriminate — all 194 PLCC-2/3528 candidates are `extended` — but across a
  row of eight distinct parts it usually does, and it is the reason to look at
  the whole set rather than the top row.
- **Operating temperature is where the tiers actually differ, and it does not
  favour tier-1 by default.** XINGLIGHT ran −20…+80 °C; HONGLITRONIC and
  Everlight ran −40…+85 °C; one HONGLITRONIC part (HVO-3528CPXA, 2,000 units)
  ran −40…+100 °C. Board3's *currently fitted* XINGLIGHT LEDs already have the
  −20 °C floor, so the tier-2 3528 parts are a thermal **upgrade** over what is
  on the board — not a compromise accepted for stock. Check the number; do not
  infer it from the brand.
- **`has_easyeda_footprint` decides whether you can actually use the part**, and
  no search surfaces it. JLC's *assembly* catalogue and EasyEDA's *CAD* library
  are different sets: a part can be stocked, cheap and correct and still have no
  symbol or footprint to import, at which point `kicad_lcsc.py add` fails with a
  bare `No component found`. Three of the six LEDs chosen on 2026-07-28 —
  `HVR-3528CPXA` (23,232 in stock), `HVB-3528CPX` (18,234), `HVCW-3528CPX` —
  died here *after* being selected. Only `jlc_get_part` exposes the field
  (`has_easyeda_footprint`, `easyeda_symbol_uuid`, `easyeda_footprint_uuid`);
  check it on the shortlist before committing to a part, not after.

## A package name is not a land pattern

Having found the parts, do not assume one footprint serves them. EasyEDA draws
each manufacturer's land pattern from that manufacturer's datasheet, so one
package string covers many geometries. The three HONGLITRONIC `SMD3528-2P` LEDs
imported on 2026-07-28 — same vendor, same series, same package string:

```text
HVO-3528CPXA   pads at ±1.35 mm    1.0 × 2.6
HVT-3528CPX    pads at ±1.524 mm   1.5 × 2.6
HVY-3528CPX    pads at ±1.47 mm    1.3 × 2.3
```

0.35 mm of pad-centre spread and 0.5 mm of pad width, within one series. For a
two-pad part that is tombstoning and open-joint territory, so borrowing a
sibling's footprint to work around a missing CAD model is not a shortcut — it is
a guess at pad geometry on a board going to assembly. Draw it from the datasheet
or pick a part that imports.

## Why This Matters

The failure is silent and it *inverts* decisions rather than degrading them.
Nothing about a coherent 98-row result of recognisable brand names says "this
is half the catalogue". The search succeeded, the parts were real, the stock
numbers were true — and the conclusion drawn from them ("orange PLCC-2 is
unsourceable, migrate to PLCC-4") was wrong by two orders of magnitude on the
number that decided it.

Stop conditions amplify this. A plan that says "stop if a part cannot be
sourced" delegates its authority to whatever the last search returned; a
single-spelling search therefore does not just mislead a human, it halts
autonomous work with a clean, confident-looking report. The guard has to sit
*upstream* of the stop condition — in how the search is issued — because by the
time the stop condition fires, the evidence looks complete.

Cost asymmetry: one extra string in one `packages` array, against a package
migration plus a footprint addition plus a full series-resistor re-derivation,
all triggered by a number that was off by 14,575 units.

## When to Apply

- Before any search result is allowed to become a sourcing conclusion — scarce,
  single-sourced, unavailable, "only one vendor makes this"
- Before a stop condition fires on availability, or a package is changed on
  availability grounds
- When a result set is **uniformly one region's vendors**. That is the
  signature: 98 hits, all Western tier-1, no Chinese manufacturer anywhere in a
  catalogue that is mostly Chinese manufacturers
- When the MPN itself encodes a size (`XL-2012UBC`, `HL-A-3528S22OC`) — the
  vendor named the part in the spelling it filed under, and that spelling is
  your second query
- Less useful for parts searched by exact MPN or LCSC code; this is a
  *package-filtered* search trap

## Examples

The whole learning in four numbers, one physical package, 2026-07-28:

```text
packages=["PLCC-2"]                     98 parts   orange best stock     79
packages=["SMD3528-2P", "3528"]        194 parts   orange best stock 14,654
                                                   blue   best stock    100 -> 45,998
```

Both queries were correct. Both returned genuine, in-stock, footprint-compatible
parts. Only the union is a catalogue.

## Related

- [EasyEDA part search shows zero stock on a well-stocked JLC basic part](../integration-issues/easyeda-jlc-ghost-listing-zero-stock.md)
  — the sibling failure. That one is the search surface lying about *stock*;
  this one is the search surface lying about *breadth*. Both end in an
  unnecessary hunt for a replacement part.
- `docs/plans/2026-07-27-003-feat-telltale-driver-and-rail-decoupling-plan.md`
  — U11 (telltales to PLCC-2) and the stop condition this nearly fired.
- `kicad/README.md` — every placed part must carry LCSC metadata, which is why
  the search step is load-bearing rather than advisory.
