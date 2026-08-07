---
title: "A tolerance that decides whether to act becomes the requirement"
date: 2026-08-06
category: developer-experience
module: kicad-silk-trim
problem_type: developer_experience
component: tooling
severity: medium
applies_when:
  - "A tool uses one threshold both to detect a violation and to decide how far to correct it"
  - "A safety margin, epsilon, or fuzz factor is added to a limit before comparing against it"
  - "A cleanup or codemod removes far more than expected and the excess looks like an inherent trade-off"
  - "Deciding between destructive and non-destructive fixes for a geometric or numeric constraint"
  - "Two requirements on the same object pull in opposite directions"
tags: [tooling, thresholds, tolerance, silkscreen, kicad, codemod, false-positive, board3]
---

# A tolerance that decides whether to act becomes the requirement

## Context

`tools/kicad_silk_trim.py` cuts silkscreen back from solder-mask apertures so the
board meets JLCPCB's 0.15 mm silk-to-pad clearance. It carries a small margin
(`MARGIN_MM = 0.02`, `tools/kicad_silk_trim.py:86`) for a specific and correct
reason: `GetEffectivePolygon()` returns a polygon *inscribed* in a round pad, so
every distance measured against it runs a few microns optimistic, and trimming
to exactly 0.15 mm left residual violations of ~3.8 µm.

The margin's job is to stop that approximation error from deciding a borderline
cut. But the tool applied it to a second question it was never meant to answer:

```python
# what it did: one threshold, two jobs
limit = (clearance + margin) + half_line_width
if distance_to_pad < limit:
    trim_or_delete(shape)          # <-- "should I act?" now asks for 0.17, not 0.15
```

The requirement was 0.15 mm. The tool enforced 0.17 mm.

**It deleted 137 compliant shapes.** Every `R0603` outline on the board — 62 of
them, all 31 placements — measures **0.1522 mm** from its own pads. That is
above the requirement and below requirement-plus-margin, so the tool removed all
of them. `C0805` lost 22 the same way, `<board-level>` all 24, `ERJP06F60R4V`
all 24.

## Guidance

### 1. Separate "does it violate?" from "how far do I correct it?"

They are different questions and they deserve different thresholds. The
requirement answers the first. The requirement plus a margin answers the second,
and only for objects the first question already condemned.

```python
# what it does now (tools/kicad_silk_trim.py:384-392, 401)
d_min = min_distance(shape, nearby_pads)
if (d_min - half_line_width) >= clearance:
    continue                       # compliant: leave it completely alone
# only now does the margin apply, to decide where the cut lands
```

A compliant object must come out of the tool unchanged — not "trimmed to a
slightly safer position". Anything else is the tool enforcing a standard nobody
agreed to, on a population nobody looked at.

### 2. When a cleanup removes more than expected, check which side of the line the extra was on

This is the part that makes the failure durable rather than obvious. The
over-deletion did not look like a bug. It looked like a hard engineering
trade-off, and it was written up as one — the plan recorded that a trial run
*"removes 28% of all silk line length, and takes `R0603` (31 placements) and
`ERJP06F60R4V` to zero silk"*, and reasoned, plausibly, that *"for chip passives
that is plausibly correct — many vendor libraries carry no silk on an 0603 at
all"*
(`docs/plans/2026-08-05-001-fix-board3-silkscreen-clearance-plan.md:85-90`).

Every sentence of that is a reasonable thing to say about parts that genuinely
cannot meet a requirement. None of it was true here, because those parts met the
requirement already. **A threshold sweeping up compliant objects produces
exactly the discussion you would have if the trade-off were real**, and the
discussion is about a population, so nobody checks an individual measurement.

The check is one line: print the distribution of what you are about to remove,
against the limit. `R0603` at 0.1522 against 0.15 answers it instantly.

### 3. Prefer the non-destructive lever, and look for one before accepting deletion

Clearance here is measured **edge-to-edge** against a *centreline* that the file
stores. So the gap is `centreline_distance − width/2`, and **narrowing a line
increases its clearance with no geometry change at all**. Nothing moves, nothing
is cut, the marking stays whole and legible.

That single lever fixed **50 shapes**, including all 24 board-level outlines
around C63/C64/C65 that the previous version deleted outright. Net effect across
the board: 150 trimmed / 157 removed became **50 narrowed / 88 trimmed / 22
removed**, and no footprint type lost all its silk.

The general form: when a constraint is expressed on a derived quantity, list the
inputs to that derivation before assuming the object itself has to move. Width,
here, was an input nobody had considered a variable.

### 4. Two requirements on one object bound each other — check feasibility, don't optimise one

Narrowing is not free forever. The fab also has a **minimum printable line
width**, so a shape must satisfy both:

```
gap  = centreline_distance − width/2 ≥ clearance     (clearance)
width ≥ min_printable_width                          (printability)
```

which is satisfiable only when `centreline_distance ≥ clearance +
min_printable_width/2`. Above that line a shape is fixed by editing a number;
below it, the geometry has to change. `MIN_SILK_WIDTH_MM`
(`tools/kicad_silk_trim.py:93`) is the floor that keeps the tool from
"fixing" clearance by creating a width violation instead.

These two were treated as separate concerns for most of the campaign — one was
even scoped *out* of the plan as unrelated. They are not independent, and a tool
that optimises either one alone will trade a violation of one kind for a
violation of the other.

## Why This Matters

**The damage is invisible in the metric the tool reports.** "199 → 8 violations"
looks like a clean success whether the tool removed 22 shapes or 157. Clearance
improves monotonically as you delete markings; a silkscreen with no silk on it
scores perfectly. Any tool whose objective function is "reduce violations" will
happily reach zero by deleting the thing being measured, and the report will
congratulate it.

**The margin was correct and still caused this.** There was nothing wrong with
`MARGIN_MM` — it absorbs a real, measured approximation error. The defect was
letting one constant answer two questions. That makes this hard to catch by
reviewing the constant, which is where anyone would look.

**It generalises to anything with a fuzz factor that also gates the action:**

| Tool | The same mistake |
| --- | --- |
| Linter with an autofix tolerance | reformats code that already passed |
| Dead-code remover with a "probably unused" confidence | deletes referenced symbols just under the bar |
| Image/asset optimiser with a quality epsilon | re-encodes assets already inside budget |
| Migration or codemod with fuzzy matching | rewrites call sites that did not need it |
| Float comparison with an epsilon used for both `==` and clamping | clamps values that were already in range |

## When to Apply

- Whenever a threshold is written as `limit + margin` — ask which of the two
  questions each term belongs to.
- Before accepting that a cleanup's collateral is inherent: measure the removed
  population against the requirement, not against the tool's threshold.
- When a fix is destructive, before running it: is the constraint on a *derived*
  quantity with an input you could change instead?
- When two requirements constrain one object, derive the feasibility condition
  once and check objects against it, rather than fixing one requirement and
  discovering the other later.

## Examples

Board3, same board, same 0.15 mm requirement — only the tool's logic changed:

| | before | after |
| --- | ---: | ---: |
| narrowed (no geometry change) | 0 | **50** |
| trimmed | 150 | 88 |
| **removed entirely** | **157** | **22** |
| footprint types losing *all* silk | 4 | **0** |

The 22 remaining removals are the real cases — chip passives where silk between
the pads has nowhere to go (`ERJP06F60R4V` 8, `L0603` 4, `1N5819WS` 4, LED types
6). That is the number the original run's "28% of all silk line length" was
supposed to be about.

Landed in **PR #28** (merged), alongside the silkscreen campaign it unblocked.

## Related

- [a count at the report limit is not a measurement](a-count-at-the-report-limit-is-not-a-measurement.md)
  — the same campaign's other instrument failure. There the reported number was
  a ceiling; here the reported improvement was real but bought by destroying
  what was being measured. Both look like success from the report.
- [a tool's finding can be a property of the tool](a-tools-finding-can-be-a-property-of-the-tool.md)
  — the same tool family, one layer up: there the tool's *finding* was wrong,
  here its *action* was, and in both cases the output was plausible enough to be
  written into a plan.
- `tools/kicad_silk_trim.py` — `--scope`, `--margin` and `--min-width` are all
  flags now, and the module docstring records both design corrections.
- `docs/plans/2026-08-05-001-fix-board3-silkscreen-clearance-plan.md` — the
  *Key Decisions* section preserves the original per-class reasoning with a
  block-quote recording that its premise was false, rather than deleting it. The
  reasoning was sound; the input was wrong, and that is worth showing.
