---
title: One malformed constraint silently voids the entire .kicad_dru — kicad-cli says nothing and reports a clean-looking run
date: 2026-08-08
category: integration-issues
module: kicad-drc
problem_type: integration_issue
component: tooling
symptoms:
  - "Known-waived DRC findings reappear after a .kicad_dru edit (Board3: 26 courtyards_overlap + 10 copper_edge_clearance, the documented tight_courtyards_by_design / telltale_expander_cluster / usbc_slot_edge exceptions)"
  - "kicad-cli pcb drc exits 0 with 0 bytes on stderr while every custom rule in the file is dropped"
  - "A newly added rule reports zero findings at every threshold, indistinguishable from a clean board"
  - "Report JSON metadata (ignored_checks, included_severities) is byte-identical to a healthy run — nothing records that the rule file failed to load"
root_cause: config_error
resolution_type: config_change
severity: high
tags: [kicad, kicad-cli, drc, kicad-dru, custom-rules, silent-failure, min-resolved-spokes, fail-open]
---

# One malformed constraint silently voids the entire .kicad_dru — kicad-cli says nothing and reports a clean-looking run

## Problem

Writing a `min_resolved_spokes` rule in the sub-expression form that every other
constraint in Board3's `.kicad_dru` uses —
`(constraint min_resolved_spokes (min 4))` — does not produce a rule that fails
to fire, and does not produce an error. It invalidates the **entire rule file**.
Every scoped exception and every added check in `.kicad_dru` is dropped
wholesale, including rules that parsed fine *before* the malformed one, and
`kicad-cli pcb drc` completes with exit 0, empty stderr, and a normal-looking
report. The only visible consequence is that the board's verdict changes — and
only on the axes where the rule file changed the verdict in the first place.

Found during the 2026-08-07 DRC review session (session work, not yet a PR);
independently reproduced from scratch 2026-08-08 on a staged copy — every
number below is from that reproduction, not from the review's report.

## Symptoms

- 36 violations appear from nowhere on a board whose baseline is 0/0/0 at
  `--severity-all`: 26 `courtyards_overlap` + 10 `copper_edge_clearance` — the
  exact findings the file's waiver rules (`tight_courtyards_by_design`,
  `telltale_expander_cluster` from PR #14/#17, `usbc_slot_edge`) exist to
  suppress, all at severity `error`.
- The malformed rule itself contributes nothing: zero `starved_thermal`
  findings at any threshold, so the probe you just wrote looks like it found a
  clean board.
- No error anywhere: exit code 0, stderr 0 bytes, stdout the ordinary
  `Found N violations` / `Saved DRC Report` lines. It was not "buried in
  output"; kicad-cli 10.0.5 prints **nothing at all** about the parse failure.
- The report JSON is no help either: `ignored_checks`,
  `included_severities`, `kicad_version` are byte-identical between a healthy
  run and a dropped-file run. No field records rule-file status.

## What Didn't Work

- **The sub-expression form** `(constraint min_resolved_spokes (min 2))` —
  the natural way to write it, because all six pre-existing rules in Board3's
  `.kicad_dru` use `(constraint <type> (min …))` for their distance-valued
  constraints (`courtyard_clearance`, `hole_clearance`, `edge_clearance`,
  `silk_clearance`). `min_resolved_spokes` is count-valued and takes a bare
  integer; wrapping it in `(min …)` is a parse error, and KiCad's response to
  a parse error anywhere in the file is to load **no rules at all**.
- **Trusting exit code / stderr to gate the edit.** There is nothing to gate
  on. Deliberate garbage (`(rule broken (constraint` — unbalanced parens) was
  run as a control and produced a byte-for-byte identical failure signature:
  exit 0, empty stderr, same 36 reappeared findings. A plausible-looking
  near-miss and outright line noise are indistinguishable from the calling
  side.
- **Expecting rules before the error to survive.** The malformed rule was
  appended at the *end* of the file; the waiver rules at the top died anyway.
  It is whole-file invalidation, not parse-until-error.

## Solution

The working syntax, established empirically (KiCad 10.0.5, `kicad-cli pcb drc
--severity-all`, board + full sidecar set staged per
`docs/solutions/developer-experience/stage-project-sidecars-for-headless-drc.md`):

```
# BROKEN — silently voids the whole .kicad_dru:
(rule probe (constraint min_resolved_spokes (min 2)))

# CORRECT — bare count, no (min …) wrapper:
(rule probe (constraint min_resolved_spokes 2))
```

And the proof the correct form actually *fires* rather than merely parses —
the threshold ladder on Board3, whose `.kicad_pro` board setup carries
`min_resolved_spokes: 2` with `starved_thermal` at severity `error`:

| rule value | starved_thermal findings | waived findings |
|---|---|---|
| bare `2` | 0 (matches board setup — board is clean at 2 spokes) | still suppressed |
| bare `3` | 12 | still suppressed |
| bare `4` | 47 | still suppressed |
| `(min 2)` sub-expression | 0 — rule never armed | **36 reappear** |

A rule that reports 0 because it is malformed and a rule that reports 0
because the board is clean are indistinguishable from the passing side; the
ladder is what separates them.

## Evidence — condensed reproduction transcript

Staged in scratch: `.kicad_pcb` + `.kicad_pro` + `.kicad_dru` + `fp-lib-table`
+ `sym-lib-table` + `Board3.pretty` + `JLCImport.pretty` +
`ProPrj_New-easyedapro.pretty` (the full sidecar set — anything less produces
its own phantom findings and would have confounded the measurement). Board:
`kicad/board3/ProPrj_New Project_2026-07-15_23-14-34_2026-07-27.kicad_pcb`.
Each variant restores the pristine `.kicad_dru`, appends one rule, and runs:

```
kicad-cli pcb drc --severity-all --format json -o report.json <board>
```

| variant | appended to `.kicad_dru` | exit | stderr | result |
|---|---|---|---|---|
| a | nothing (baseline) | 0 | empty | 0 violations, 0 unconnected, 0 parity |
| b | `(rule probe (constraint min_resolved_spokes (min 2)))` | 0 | empty | **36**: courtyards_overlap 26 + copper_edge_clearance 10, all severity `error`; 0 starved_thermal |
| c | `(rule probe (constraint min_resolved_spokes 2))` | 0 | empty | 0 violations — file parsed, waivers held |
| c′ | same, bare `3` / bare `4` | 0 | empty | 12 / 47 starved_thermal — rule demonstrably fires |
| d | `(rule broken (constraint` | 0 | empty | **36** — identical set to (b) |

Spot-checked items in variant (b) confirm these are the documented waived
findings, not new defects: `courtyards_overlap` on C44/U1 (the U1 decoupling
ring, waived by `tight_courtyards_by_design`); `copper_edge_clearance` on
USBC1's routed retention slots (waived by `usbc_slot_edge`, "board setup
constraints edge clearance 0.5000 mm; actual 0.4542 mm").

## Why This Works

KiCad's custom-rule loader is transactional and fail-open: a parse error
anywhere in `.kicad_dru` aborts loading the whole file, and DRC then runs with
**no custom rules** — board-setup constraints and netclass rules only, which is
why the `copper_edge_clearance` messages in variant (b) still cite the
`.kicad_pro`'s 0.5 mm figure. The `.kicad_pro` is unaffected; only the
`.kicad_dru` drops. In the GUI's rule editor a syntax checker exists to catch
this at edit time (unverified here — this reproduction is headless-only); in
`kicad-cli` 10.0.5 the degradation is completely silent on every channel a
script could observe: exit code, stderr, stdout, and the report JSON's
metadata.

The mistake itself is a natural one. The sub-expression `(min …)` is the
*correct* syntax for every distance-valued constraint, and all six rules that
were already in Board3's file use it. `min_resolved_spokes` is count-valued
and takes a bare integer. One wrong guess about which family a constraint
belongs to costs not one dead rule but the entire file.

**The fingerprint, and its blind spot.** A dropped `.kicad_dru` announces
itself only through the *delta between the file's intent and no file at all*:

- **Waiver rules** announce their own loss loudly — the findings they suppress
  reappear (here, 36 at severity `error`, so even an error-gated CI run
  catches it on this board).
- **Added checks announce nothing.** Board3's silk rules
  (`jlc_silk_to_pad`, PR #28) live in this same file. Baseline silk findings:
  0 (rule armed, the documented D10/D11/R9 exceptions hold). Dropped-file silk
  findings: 0 (no silk check runs at all — KiCad runs no silkscreen test
  without a `silk_clearance` rule in `.kicad_dru`). Same number, opposite
  meanings, measured in this very transcript. Likewise
  `pth_hole_to_track_jlc` (PR #27), which by design catches nothing today.
  Of the six rules in Board3's file, only the three board-setup waiver rules
  produce any signal when the file dies (`silk_to_pad_documented_exceptions`
  is waiver-shaped too, but it waives an in-file rule, so it dies with it —
  signal-less).
- **Consequently: a board whose `.kicad_dru` only adds checks gets no signal
  at all.** DRC reports 0, CI stays green, and every custom check has silently
  stopped running. Board3 is *lucky* to have error-severity waivers acting as
  an accidental canary. Do not generalize "the reappearing waivers will warn
  us" to boards that have none.

## Prevention

- **Fire-both-ways, now extended to the file level.** The discipline the silk
  campaign (PR #28) already carved into the `.kicad_dru` comments — "Verified
  to FIRE, not merely to pass" — applies to every rule-file edit, in both
  directions: after any `.kicad_dru` change, (1) show a rule FIRING (tighten
  its threshold or point it at a known-dirty case and watch the count move),
  and (2) show the waivers still SUPPRESSING (the baseline violation set is
  unchanged). Step (2) is what catches whole-file invalidation; step (1) is
  what catches a rule that parsed but never armed.
- **Baseline violation-set compare after any rule-file edit.** Cheap and
  mechanical: run DRC before and after, diff the violation counts by type. An
  edit that was supposed to add one probe rule and instead changed
  `courtyards_overlap` from 0 to 26 is a dropped file, full stop. This is the
  same instrument-check reflex as "the netlist diff is the gate" for schematic
  edits.
- **Treat a probe that reports 0 as unproven until you have seen it report
  nonzero.** The min 4 → 47, 3 → 12, 2 → 0 ladder costs three CLI runs and
  converts "no findings" from a hope into a measurement.
- **Do not build tooling that trusts exit code or stderr to validate a
  `.kicad_dru`.** There is nothing there to trust — measured: exit 0, 0 bytes.
  If a lint step is wanted, the working check is behavioral: append a
  deliberately-firing rule to a staged copy and require nonzero findings.

## Related Issues

- `docs/solutions/integration-issues/kicad-colon-in-symbol-name-makes-library-unloadable.md`
  — the same failure shape one layer over: one malformed *name* voids an entire
  symbol library while everything still renders, and "not found" means *failed
  to load*, not missing. Both are KiCad loaders that fail closed on the file
  and fail open on the workflow.
- `docs/solutions/developer-experience/stage-project-sidecars-for-headless-drc.md`
  (campaign: PR #15) — the complementary trap: there the sidecar was *absent*
  and the phantom findings were the noise; here the sidecar is *present but
  unparseable* and the reappearing findings are the signal. Same lesson from
  both sides: headless DRC's verdict is a function of inputs it never
  itemizes.
- The `.kicad_dru`'s own `jlc_silk_to_pad` comment block (PR #28) — the origin
  of the fire-both-ways discipline this doc extends: "A rule that reports 0
  because it is malformed and one that reports 0 because the board is clean
  are indistinguishable from the passing side."
- CLAUDE.md's silkscreen entry — "a value in the project file is not evidence
  the test exists"; this doc is the rule-file-level instance: a rule in
  `.kicad_dru` is not evidence the rule is running.

**Transferability.** This is the general failure mode of any system that
parses a config file permissively and degrades a parse failure to "no rules"
instead of an error: linter configs that fall back to defaults, CI policy
files that skip unparseable sections, firewall rule sets that load empty on
syntax errors. The portable tests are the same two: after editing any
rules-as-config file, prove one rule fires, and prove the previous verdict on
a known case is unchanged. A green run proves nothing about a rule set whose
loader fails open.
