---
title: "A holdout list keyed outside the artifact it protects unbinds on rename, and a guard sharing its names certifies the loss"
date: 2026-07-27
category: logic-errors
module: kicad-routing
problem_type: logic_error
component: tooling
severity: high
symptoms:
  - "`tools/kicad_strip.py` prints `excluded nets  : 20  (0 tracks, 0 vias kept)` and an empty `-- held out --` block, then exits 0"
  - "The two nets the holdout list exists to protect head the `-- to be stripped --` list at 448 and 437 tracks"
  - "A WARNING names all 20 held-out nets as absent from the board, and the run continues to completion anyway"
  - "1161 tracks and 19 vias of audited copper are deleted by a run whose last line reads `verification   : OK -- excluded copper intact`"
root_cause: logic_error
resolution_type: code_fix
related_components:
  - easyeda-workflow
  - kicad-verification
tags:
  - kicad
  - identifier-drift
  - holdout-list
  - silent-failure
  - verification
  - self-referential-guard
---

# A holdout list keyed outside the artifact it protects unbinds on rename

## Problem

**When a protection list keys on identifiers stored outside the artifact it
protects, an upstream rename silently unbinds it while the operation still
reports success — and a guard written against the same names as the operation it
guards will confirm that success.**

`tools/kicad_netclass.json` is the single source of truth for which Board3 nets
are held out of autorouting. It stores them bare: `"USB_DP"` at line 40, twenty
names in total, not one containing a `/`. The nets themselves live in the
`.kicad_pcb`, which is a different file under a different tool's control.

On 2026-07-27 at 16:38, commit `e721f36` ran "Update PCB from Schematic" with
*Re-link footprints to schematic symbols based on their reference designators*.
Its message records the consequence: *"That update reconciled net naming: the
board's nets lacked the hierarchical `/` prefix the schematic uses, so every via
and zone reconnected from GND to /GND, +3V3 to /+3V3 and so on."* The board today
contains `"/USB_DP"` 439 times and bare `"USB_DP"` zero times.

Nothing in the JSON changed, because nothing had to. The list did not break; it
stopped referring to anything.

Commit `9af4cf3` caught this in the router at 17:55 and fixed
`excluded_net_args()` in `tools/kicad_route.py`. It changed that file and the
board, and nothing else. **The same defect was live in `tools/kicad_strip.py`,
which at the time had one commit in its history — `98cf756`, 12:42, three hours
and fifty-six minutes before the rename that unbound it.** Line numbers below
are from that file as it stood then.

> **Fixed.** All three rules prescribed below are implemented in
> `tools/kicad_strip.py` today: `match_key()` normalises the hierarchical path,
> a hard refusal aborts the run when every held-out net would be destroyed, and
> the verification counts survivors instead of testing membership in the
> pre-state. The fix and this document arrived in the same commit, which is why
> the paragraph above originally read in the present tense — the doc was written
> from the pre-fix state and never revised once the fix landed beside it. The
> analysis below is kept as written because the reasoning, not the incident, is
> the reusable part.

## Symptoms

Running the census against the board **as it stood before the fix** reproduced
every step. The current tool cannot produce this run — the refusal at step 3
aborts first:

```text
excluded nets  : 20  (0 tracks, 0 vias kept)
routable nets  : 97  (2487 tracks, 159 vias to strip)
WARNING: netclass names 20 net(s) absent from this board: ['CAN1_H', ...]

-- held out --
                                     <- zero rows

-- to be stripped (top 15 by track count) --
   /USB_DM            tracks=448  vias=0   pads=2
   /USB_DP            tracks=437  vias=0   pads=2
   /GND               tracks=391  vias=34  pads=128
```

- **`0 tracks, 0 vias kept`** on a list whose whole purpose is keeping copper.
- **The `-- held out --` block prints nothing**, because every row is gated on
  the same failing lookup.
- **The audited USB pair is ranked first and second for deletion.** These are the
  nets the JSON describes as *"Textbook; an autorouter can only degrade it."*
- Nineteen of the twenty held-out nets are swept into `routable`, carrying
  **1161 tracks and 19 vias**. The twentieth is `SWO`, a single-pin net with no
  copper, which is why `9af4cf3` counts 19 rather than 20.

## What Didn't Work

- **Fixing the instance instead of sweeping the class.** `9af4cf3` diagnosed this
  exactly and wrote it down — `kicad_route.py:82-93` is an eleven-line docstring
  explaining that a bare-only pattern *"silently stops excluding anything the
  moment that sync runs."* It then repaired one of the three files that read the
  JSON. `tools/kicad_measure.py` was already immune for its own independent
  reason. `tools/kicad_strip.py` was never checked, and it is the destructive one.
- **Re-reading the written file to verify.** The author already distrusted exit
  codes and said so at `kicad_strip.py:140-141`: *"SaveBoard can crash the
  interpreter during teardown AFTER writing, so the written file is the evidence,
  not the exit code."* The re-read is real and the check runs. It is keyed on the
  same bare names as the deletion, so it cannot see what the deletion did.
- **Printing the discrepancy.** Line 100 computes `missing` — for this board,
  all 20 of 20 — and lines 112-113 print it as a non-fatal `WARNING`. The exact
  signal needed to stop was computed, formatted for a human, and stepped over.

## Solution

The chain, with line numbers, and then the three rules a fix must satisfy.

`load_excluded_nets()` (38-49) returns the JSON's names verbatim; line 45 is
`nets.add(net)` with no normalization. `census()` (52-64) keys on the board,
`t.GetNetname()` at line 56 and `p.GetNetname()` at line 63, so its keys are
hierarchical. Line 101 computes the set to destroy:

```python
routable = sorted(n for n, (tr, _, _) in before.items() if n and n not in excluded and tr)
```

`before` holds `/USB_DP`; `excluded` holds `USB_DP`; `n not in excluded` is true
for all 97. Lines 131-134 then delete every track whose net is in that set.

The verification at 147-149 is where success gets certified:

```python
for n in excluded:
    if n in before and before[n][:2] != after.get(n, (0, 0, 0))[:2]:
        problems.append(...)
```

`n in before` is false for all 20, so the loop body never executes and `problems`
stays empty. The next loop (150-152) flags any *routable* net that still has
tracks — the held-out nets now have none, so it **affirmatively reports the
deletion as correct**. Lines 163-164 print `verification   : OK -- excluded
copper intact, routable copper gone, pad membership unchanged` and return
`EXIT_OK`.

**Rule 1 — normalize, or match both forms.** Two shipped patterns in this repo do
this, at opposite ends. `kicad_measure.py:79` normalizes on read, and its
docstring at 71-76 says why: *"a measurement that changed identity across that
sync would compare nothing to nothing."*

```python
name = track.GetNetname().lstrip("/")
```

`kicad_route.py:97` cannot normalize, because it emits patterns to a foreign
tool, so it emits both:

```python
patterns = {f"!{n}" for n in nets} | {f"!/{n.lstrip('/')}" for n in nets}
```

**Rule 2 — zero matches is an error, never a silent no-op.** A holdout list that
matches nothing is never a valid state. `load_excluded_nets()` already refuses an
*empty* list at line 48 — *"refusing to strip everything"* — which is the same
judgement applied one step too early. A list of twenty that binds to zero is
identical in effect and currently returns 0.

**Rule 3 — prove the held-out nets are byte-identical after the run.** Not that
the tool exited 0, and not that a name lookup failed to fire. Track count and via
count per net, measured before and after, compared. `9af4cf3` is the model:
*"Proven rather than assumed: all 19 held-out nets have byte-identical track
count, via count and length before and after. /USB_DP still 437 tracks /
118.739 mm, /OSC_OUT still 8 / 2 vias / 11.171 mm."* An independent census of the
current board returns 437 tracks for `/USB_DP` and 8 tracks with 2 vias for
`/OSC_OUT`, so that proof still holds and is cheap to re-run.

## Why This Works

The defect is not the missing `lstrip("/")`. It is that the operation and its
verifier were written from the same mental model, so they share a failure mode
and their agreement carries no information. A guard is only evidence if it can
disagree with the thing it guards, and this one is constructed so it cannot: both
resolve nets through `excluded`, both come up empty, and emptiness reads as
"nothing wrong" to a loop that only appends on mismatch.

Rules 1 and 2 attack the binding; rule 3 attacks the shared model. Rule 3 is the
one that survives the next unforeseen rename, because a before/after copper
census never mentions a name from the JSON at all. It asks the board what
happened to it.

The blast radius argues the same way. `tools/kicad_strip.py` was the fairness
pivot for the KiCad-versus-EasyEDA comparison — its own docstring says *"if they
begin from different copper their completion numbers describe different problems
and the head-to-head is void."* That comparison is retired (EasyEDA was dropped
2026-07-31; `kicad/board3` is the design, not a copy), so the stakes it names are
historical.

**The half that has not expired is the one that matters.** The tool still runs
manually, still appears in no CI workflow, and still has no host test — so its
self-verification remains the entire safety net, which is exactly why that net
must not be woven from the same thread. A guard's independence matters most
precisely where nothing else is watching, and losing the original justification
does not restore the missing coverage.

## Prevention

- **Treat any identifier stored outside the artifact it names as a binding that
  can come undone, and make the binding assert itself.** Config files, net lists,
  allowlists, denylists, fixture keys, test selectors. The list does not go stale
  visibly — it keeps looking correct while pointing nowhere.
- **A filter that matches nothing must fail loudly.** Count the matches and
  compare against the expected count. "Excluded 0 of 20" is an error; only
  "excluded 20 of 20" is a no-op worth continuing from.
- **Never verify an operation through the same lookup the operation used.**
  Ask whether the guard could report a problem if the operation were maximally
  wrong. Here it could not, for any input.
- **Verify effects on the artifact, not the absence of raised errors.** Count
  what you promised to preserve, before and after. This is more work than reading
  an exit code and it is the only check that would have caught a rename nobody
  anticipated.
- **When a fix is written for one call site, grep for the others in the same
  commit.** `grep -ln kicad_netclass tools/*.py` returns three files. `9af4cf3`
  fixed one, and the destructive one was not it.

## Related

- [Migrating a board from EasyEDA Pro to KiCad loses data silently at six separate points](../integration-issues/easyeda-pro-to-kicad-migration-silent-data-loss.md) — the migration whose re-link step performed the rename
- [A gate that cannot pass gets waved through](../conventions/a-gate-that-cannot-pass-gets-waved-through.md) — the other way a check stops carrying information
- [Refill zones before measuring a headlessly routed board](../developer-experience/refill-zones-before-measuring-a-headlessly-routed-board.md) — a KiCad measurement that arrives precise and wrong
