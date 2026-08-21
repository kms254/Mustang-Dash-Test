---
title: "A saturated instrument: a count at the tool's reporting limit is not a count"
date: 2026-08-06
category: developer-experience
module: kicad-drc
problem_type: developer_experience
component: tooling
severity: high
applies_when:
  - "A checker's count is about to become the size of a problem, a plan's scope, or a before/after headline"
  - "Two different violation types report the same suspiciously round total in one run"
  - "A count does not move after the rule that produces it is made materially stricter"
  - "Comparing a before number and an after number that were produced under different rule strengths"
  - "Deciding whether a CI gate should be absolute or ratcheted against a measured floor"
symptoms:
  - "silk_over_copper and silk_overlap both report exactly 199 on the same board"
  - "Doubling and then multiplying the clearance rule 6.7x leaves the reported count unchanged"
  - "A commit message claims 199 -> 8 where the 199 and the 8 are not the same kind of quantity"
  - "A report cap propagates into CLAUDE.md, a plan's Goal Capsule, and a tool docstring as a measurement"
root_cause: wrong_api
resolution_type: workflow_improvement
related_components:
  - development_workflow
  - documentation
tags: [kicad, drc, report-limit, saturation, measurement, silkscreen, board3, verification]
---

# A saturated instrument: a count at the tool's reporting limit is not a count

`kicad-cli pcb drc` stops counting a violation type at **199**. Board3's
silkscreen campaign was scoped, planned, documented and headlined against a
number that was that ceiling rather than a quantity.

The ceiling is measured, not theorised: it is what the silk codes do. **The
mechanism behind it is not established** -- see section 2, where the number that
was supposed to prove the limit's shape turned out not to be a count either.

The failure has a general shape, and the shape is the transferable part: **a
count that has hit its tool's reporting ceiling reads the same no matter what
you point it at.** It is not a wrong number. It is *not a number* — and from the
outside it is indistinguishable from a real measurement. It has units, it has
digits, it sits in a JSON field called `violations`, and it will happily become
the denominator of a before/after claim.

Reproduced first-hand on the merged tree (KiCad 10, board staged with its real
rules and sidecars, `--severity-all`, varying only the `min` on the
`jlc_silk_to_pad` rule in the board's `.kicad_dru` under `kicad/board3/`):

```
[silk_to_pad min=0.15mm]  total=0
[silk_to_pad min=0.30mm]  silk_over_copper=199  silk_overlap=199
[silk_to_pad min=0.60mm]  silk_over_copper=199  silk_overlap=199
[silk_to_pad min=1.00mm]  silk_over_copper=199  silk_overlap=199
```

The rule gets 3.3× stricter across those three rows and the number does not
move by one.

## Context

The campaign began from a real and separate discovery, banked the same week:
**KiCad runs no silkscreen test at all unless a `silk_clearance` rule exists in
the `.kicad_dru`.** Board3's `.kicad_pro` carried `min_silk_clearance: 0.15`
with `silk_over_copper` / `silk_overlap` / `silk_edge_clearance` all armed at
`warning`, all of it inert; DRC reported 0 at `--severity-all` while JLCPCB's
DFM returned 81 red silkscreen errors on the same board.

Adding the missing rule was the fix. The board then reported **199
`silk_over_copper` + 199 `silk_overlap`** — and *that* is the number that got
written down. It went into `CLAUDE.md` as the size of the problem, into the
Goal Capsule of
`docs/plans/2026-08-05-001-fix-board3-silkscreen-clearance-plan.md`, into the
docstring of `tools/kicad_silk_trim.py`, and finally into a commit subject on
this branch claiming **"199 → 8"**.

That claim compares a saturated number against an unsaturated one. The two
sides are not the same kind of quantity, so the ratio between them means
nothing at all. The honest figures, taken directly off the geometry with
`pcbnew` instead of from the report, are **233 shape×pad pairs over 169
shapes**, reduced to **6** by the fix — six cases documented as scoped
exceptions in the `.kicad_dru` rather than muted.

The board carries **586 silk `PCB_SHAPE` objects before the fix** (24
board-level + 562 footprint-owned) and **590 after** (24 + 566, the trim having
split some shapes into pieces), plus 28 silk texts. Those are two board states,
not two readings of one board -- worth stating precisely, because presenting
them as a discrepancy would be the same incommensurability this doc is about.
At a 1.00 mm clearance requirement nearly every one of them must be violating.
The report still says 199.

## Guidance

### 1. The detection rule is one command: make the test stricter and re-run

**A real count rises. A saturated one does not.**

That is the whole test. It costs one extra run of a tool you were already
running, it needs no knowledge of the tool's internals, and it would have caught
this before the number reached a plan's Goal Capsule.

Concretely, for anything with a threshold: double it. For anything without one,
widen the scope — more files, more severities, a looser filter. Then look at the
delta, not the total. Here, going from 0.30 mm to 1.00 mm should have added
hundreds of findings and added zero.

Apply the test **before** a count is quoted anywhere durable — a plan, a commit
message, a doc, a dashboard — not after somebody questions it. The cost is
asymmetric: one run beforehand, versus a scoping decision, a tool, and three
documents built on a constant.

### 2. Do not infer "unsaturated" from another number in the same report

This is the trap that makes the failure survive a casual sanity check. A single
run can contain one type far above 199 and two types pinned exactly to it.
Measured on the merged tree, in **one report**, with a deliberately absurd
`(constraint clearance (min 3mm))` alongside the silk rule at 0.60 mm:

```
clearance=514   silk_over_copper=199   silk_overlap=199
```

The obvious reading -- "the tool can clearly print more than 199, so my 199 must
be real" -- is invalid. But the *interesting* part is why, and it took the
grounding review of this very doc to find it.

**514 is not a count either.** Applying section 1 to it: tightening the probe
from 3 mm to 5 mm to 8 mm gives **514, 514, 513** -- it does not rise. Four
identical runs give **514, 514, 513, 514** -- it is not even deterministic,
which no real census is. And a genuine 3 mm all-net clearance sweep on a board
with roughly 2,400 tracks would run to thousands, not hundreds. So 514 is
another ceiling, reached raggedly -- consistent with a multithreaded checker
whose workers each consult a shared error budget -- and not a measurement.

An earlier revision of this doc presented 514 as the proof that the limit is
"per error code, not per report". That conclusion may still be right, but **514
was never evidence for it**: the argument used an unexamined number to establish
that another number was unexamined. The honest statement is narrower -- whatever
bounds these counts is not applied uniformly across a report, because two types
stop at 199 while a third stops somewhere near 514. Why, is unknown.

The rule that survives is the one that never needed a theory of the mechanism:
**the only evidence that a specific count is unsaturated is that it moved when
the rule tightened.** No number in the same report licenses a conclusion about
any other -- including one that looks big enough to be safe.

### 3. Suspect the shape of the number, not just its size

Two independent violation types reporting **the identical total** is close to
conclusive on its own. Independent geometric properties of a real board do not
agree to the digit. Round or repeated totals — 100, 199, 200, 250, 500, 1000 —
deserve the stricter-rule test before anything else.

### 4. When the count matters, measure the geometry, not the report

A checker's report is an *interface*, subject to pagination, truncation,
deduplication and severity filtering. The board file is the *data*. Where a
number is load-bearing — scoping work, sizing a fix, proving a delta — derive it
from the objects:

```python
# what the report cannot truncate: pairs measured directly off the board
pairs = [(shape, pad) for shape in silk_shapes(board)
                       for pad in pads_near(shape)
                       if gap(shape, pad) < 0.15]
```

That is where **233 pairs over 169 shapes** came from, and it is a *different
unit* from what the report counts — the report counts violations, one per
offending pair, but only up to 199 of each type. Naming the unit is half the
discipline: "233 shape×pad pairs over 169 shapes" cannot be silently compared
against "8 violations".

### 5. A gate at zero is immune — which is an argument for absolute gates

**Zero cannot saturate.** Any gate whose pass condition is "the count is 0" is
unaffected by every failure mode in this doc: a truncated report and an honest
one agree exactly at zero, and they agree nowhere else.

A **ratcheted** gate — "no worse than the measured baseline" — is the opposite.
It is defined entirely in terms of a count, so it inherits the count's
saturation. A ratchet pinned at a saturated floor cannot detect a regression of
any size below the cap, and it will report "no new violations" forever.

This is the third independent argument this repo has accumulated for keeping its
CI gates absolute rather than ratcheted (`.github/workflows/kicad-drc-erc.yml`
runs four absolute design-rule gates -- library-load, schematic parity, DRC and
ERC -- plus a fab BOM/CPL symmetry check that also fails the build, and consumes
no baseline). The first was that a
gate which can never pass gets waved through; the second was that a ratchet can
stop a debt growing but can never retire it. This is the third: **a ratchet's
threshold is a measurement, and measurements can be fake.**

### 6. Correct the number everywhere it propagated, not just where you found it

A saturated figure spreads because it is useful — it is the one quantity anyone
has. Correcting it in the plan and in `CLAUDE.md` is not the same as correcting
it.

Writing this doc found five more copies that the earlier correction missed, all
in files nobody thought of as carrying a measurement: `tools/kicad_silk_trim.py`
opened its module docstring with *"199 silk objects sit 0.031 to 0.1495 mm
away"*, its `poly_points()` docstring described the tool reporting no work to do
*"on a board with 199 violations"*, and the plan still had the figure in three
body sections after its Goal Capsule had been corrected. Every one of those
sentences is about a real defect, and every one carried the cap forward as if it
were a census. All are fixed in the same change as this doc.

The grounding review of this doc then found **two more** the grep had still
missed: the sentence *"unconditioned 18 + 199, scoped to pads 9 + 0"*, which
exists verbatim in `CLAUDE.md` and in the board's `.kicad_dru`, and which had
been annotated in the plan but nowhere else. Three passes, and the figure was
still propagating. That is the honest measure of how far a useful number
travels once it exists.

**When you retract a number, grep for it.** (Generalised beyond numbers, to any
retired claim, in
[a correction is an unreviewed change](../conventions/a-correction-is-an-unreviewed-change.md) —
which also records that a retraction is itself an unreviewed edit with a shelf
life.) A retraction that lands only where
the number was first written leaves the copies that will be read next.

## Why This Matters

**The number set the scope of the work, and it was wrong in the direction that
looks harmless.** 199 understates 233 by only 15%, so nothing about it felt
absurd; it simply capped what could ever be discovered. Every "199 → N" claim on
this branch was arithmetic between incommensurable quantities, and the one that
reached a commit subject read as a clean 96% reduction.

**It survived because it was produced by fixing something real.** The rule that
made the count appear was a genuine repair — the check had been inert for
months. A number that arrives at the moment of a correct fix carries that fix's
credibility, and gets re-derived by nobody.

**It is the sibling of a failure this repo banked days earlier, and the pair is
the useful unit.** *A setting is not a check* covers a test that never ran:
three `silk_*` severities and a `min_silk_clearance` in the `.kicad_pro`, all
inert, DRC reporting 0 against the fab's 81. This doc covers a test that runs
and truncates. Same outcome both times — **a plausible number that nobody
re-derives** — from opposite causes. Between them they cover the whole surface:
a check can lie by not existing, and it can lie by not finishing.

Hence the rule this repo now applies to both: **probe any check you believe is
running by asserting something the board must fail.** The `.kicad_dru` records
this in the rule's own comment — *"Verified to FIRE, not merely to pass: at
0.60mm this same rule reports 199 of each"* — and that probe is simultaneously
the proof the rule works and the demonstration of its ceiling. One run answers
both questions.

**This is not a KiCad problem.** Every one of these truncates or paginates while
looking authoritative:

| Tool | The ceiling |
| --- | --- |
| Linters / compilers | `--max-warnings`, "too many errors, stopping", per-file caps |
| Test runners | `--maxfail`, fail-fast, summary truncation |
| CI annotations | GitHub caps annotations per run; the rest exist only in the log |
| Log tails / `head`-limited search | The tool's own `head_limit` default, not the match count |
| API list endpoints | Page size, and a `total` that is sometimes the page |
| DFM / EDA vendor reports | Per-category display caps in the web UI |

In every case the same one-command test applies: **make the query stricter or
wider, and check that the number moves.**

## When to Apply

- **Before** a checker count enters a plan, a commit message, a doc, a metric or
  a review claim — not when it is challenged.
- Whenever two independent categories in one report share a total, or a total is
  suspiciously round.
- Whenever a before/after pair was produced under different rule strengths,
  different severities, or different tool versions. Confirm both sides are the
  same kind of quantity before dividing them.
- When choosing between an absolute gate and a ratchet: if the floor is a count
  from a report, prefer absolute.
- When a fix "reduces" a count by a large factor and the reduction was easier
  than expected.

## Examples

### The sweep that exposed it

Varying only the clearance value on one unchanged board (the pre-fix board,
measured 2026-08-05):

| `silk_clearance (min …)` | `silk_over_copper` | Reading |
| --- | ---: | --- |
| 0.10 mm | **62** | a real count — below the limit |
| 0.15 mm | 199 | at the limit |
| 0.20 mm | 199 | |
| 0.30 mm | 199 | |
| 0.40 mm | 199 | |
| 0.60 mm | 199 | |
| 1.00 mm | 199 | ~590 silk shapes exist; nearly all must be violating |

The 0.10 mm row is the one that makes the rest interpretable: it is the only
value that produced a number the tool was willing to finish counting. **A sweep
is worth more than a probe** -- one stricter run tells you a count is suspect;
a sweep that includes a value *below* the ceiling tells you where the ceiling is.

That 62 needs its exact scope, which the table does not carry: the silk rule
**unconditioned**, the documented-exceptions rule removed, and the board's own
tracked `.kicad_pro` rather than the staged rule set. With the pad-scoped rule
that actually ships, the same board reads **37**. All the readings are below the
ceiling and all move with the rule, so the point holds -- but a figure quoted
without its recipe is not reproducible, which is a smaller version of the same
failure.

### Reproducing it on the fixed board

The cap is a property of the tool, not of the broken state, so it reproduces on
the repaired board. Re-run 2026-08-06 against the merged tree, staging the board
with `tools/kicad_rules.json` plus its `.kicad_dru`, `fp-lib-table`,
`sym-lib-table` and `.pretty` directories (per
[stage-project-sidecars-for-headless-drc](stage-project-sidecars-for-headless-drc.md)),
at `--severity-all`:

```
min=0.15mm   total=0                                    # the shipped rule; board is clean
min=0.30mm   silk_over_copper=199  silk_overlap=199
min=0.60mm   silk_over_copper=199  silk_overlap=199
min=1.00mm   silk_over_copper=199  silk_overlap=199
```

Note the first row. **The gate that ships is at zero, and zero is the one
reading the cap cannot corrupt** — §5 in practice.

### The non-uniformity probe

One report, silk rule at 0.60 mm, plus `(rule probe_wide_clearance (constraint
clearance (min 3mm)))`:

```
total=912   clearance=514   silk_over_copper=199   silk_overlap=199
```

514 and 199 in the same JSON, so whatever bounds these counts is not applied to
the report as a whole. **But do not read 514 as a count** -- see section 2: it
does not rise when the probe tightens (514 / 514 / 513 at 3 / 5 / 8 mm) and it
is not reproducible run to run (514, 514, 513, 514). It is a second ceiling,
reached raggedly. Two ceilings in one report is still enough to show that a
conclusion about one type does not transfer to another, which is all this
example is for.

### The honest figures, and the units they are in

| Quantity | Value | Source |
| --- | --- | --- |
| Reported violations, rule at 0.15 mm | 199 + 199 | the report — **saturated** |
| Silk↔pad pairs under 0.15 mm | **233** | geometry, via `pcbnew` |
| Distinct shapes involved | **169** | geometry |
| Silk `PCB_SHAPE` objects on the board | 586 / 590 | `CLAUDE.md` / re-count at merged tree |
| Pairs after the fix | **6** | geometry; six documented `.kicad_dru` exceptions |
| JLCPCB DFM silkscreen-to-pad reds | 50 → 5 | the fab's own check |

The last row is the useful cross-check: an **independent instrument**, with its
own unrelated ceiling, moving in the same direction by a similar factor. When a
count matters, a second tool that measures the same physical thing is worth more
than any amount of re-reading the first one.

## Related

- [A large ERC count is a broken instrument, not a property of the design](a-large-erc-count-is-a-broken-instrument.md)
  — the nearest sibling, and the complement. There the report was *too large to
  read*, so a real finding hid inside 1009 true violations; here the report is
  *too small to be true*, so findings never appeared at all. Both end at the same
  place: **get the count to zero, because zero is the only reading a gate can
  assert without arguing about a baseline** — and, as it turns out, the only one
  a report limit cannot forge.
- [A gate that cannot pass gets waved through](../conventions/a-gate-that-cannot-pass-gets-waved-through.md)
  — the social half. A saturated count makes a ratcheted gate permanently
  green instead of permanently red; opposite symptom, same end state of a gate
  that has stopped carrying information.
- [Verifying every part of a claim does not verify the claim](../design-patterns/verifying-every-part-of-a-claim-does-not-verify-the-claim.md)
  — "199 → 8" is exactly this: both numbers were real outputs of real runs, and
  the sentence joining them was still false.
- [Headless DRC judges the board plus its sidecar files](stage-project-sidecars-for-headless-drc.md)
  — required to reproduce any of the runs above; without the library tables and
  `.pretty` dirs an all-severities run is mostly phantoms.
- `CLAUDE.md` — the *"KiCad runs NO silkscreen test unless a `silk_clearance`
  rule exists"* section, immediately followed by the corrected account of the
  199. The two paragraphs are deliberately adjacent.
- The board's `.kicad_dru` under `kicad/board3/` — `jlc_silk_to_pad` and
  `silk_to_pad_documented_exceptions`, each carrying the reasoning for its scope
  and the proof that it fires.
- `docs/plans/2026-08-05-001-fix-board3-silkscreen-clearance-plan.md` — the plan
  whose Goal Capsule now opens by retracting its own original scope figure.
- `.github/workflows/kicad-drc-erc.yml` — four absolute gates, no ratchet, no
  baseline. §5 is why that choice keeps paying.
- The work landed in **PR #28**
  (`https://github.com/kms254/Mustang-Dash-Test/pull/28`), which also carries the
  silkscreen fix itself and three other instruments that turned out not to be
  measuring what they appeared to.

**Postscript, from writing this doc.** Establishing that PR #28 had landed went
wrong the same way once: `git merge-base --is-ancestor` returned a confident
"not merged" from a stale local `origin/main` that had not been fetched. Same
family, different mechanism — the tool answered correctly about the input it
had, and the input was not the thing anyone meant to ask about. The general form
covers both: **an instrument's output is a claim about its input, not about the
world**, and the cheap defence is a second, independent reading (here, the API's
own `state` field) before the answer is used.
