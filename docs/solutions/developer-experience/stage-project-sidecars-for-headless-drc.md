---
title: Headless DRC judges the board plus its sidecar files — stage all of them or get phantom findings
date: 2026-07-30
category: developer-experience
module: kicad-drc
problem_type: developer_experience
component: tooling
severity: medium
applies_when:
  - "Running kicad-cli pcb drc on a board copied away from its project directory"
  - "CLI and GUI DRC report different counts for the same board"
  - "A --severity-all audit reports mass lib_footprint_issues on an imported board"
  - "Building any tool that stages a board into a scratch directory before checking it"
tags: [kicad, kicad-cli, drc, fp-lib-table, libraries, staging, headless, phantom-findings]
---

# Headless DRC judges the board plus its sidecar files — stage all of them or get phantom findings

`kicad-cli pcb drc` resolves the board's project context from **sibling files in
the board's directory**. Copy a `.kicad_pcb` into a scratch directory and check
it there, and every sidecar you did not copy silently changes the verdict. The
findings this produces are indistinguishable from real board defects until the
counts refuse to reconcile with the GUI.

## Context

Board3's warning-inbox campaign (PR kms254/Mustang-Dash-Test#15) worked on a
scratch copy of the board, staged with its `.kicad_pro` and `.kicad_dru` — the
two sidecars the repo's tooling already knew about. A `--severity-all` run then
reported **120 `lib_footprint_issues`**, all one message: *"The current
configuration does not include the footprint library 'ProPrj_New-easyedapro'"*.

The GUI, checking the real project, showed **31 library mismatches** for the
same board. The 120 were phantoms: the project's `fp-lib-table` and its
`.pretty` directories exist in the repo, but were not staged beside the copy,
so the CLI could not resolve any footprint's library and flagged every
footprint carrying that library prefix. Staging `fp-lib-table` plus the two
`.pretty` directories collapsed 120 to the 37 genuine instance-vs-library
mismatches (31 pre-existing plus 6 the session's own silk edits had created) —
which could then actually be fixed.

The phantom class also *hid* the real class: with the library unresolvable, the
CLI reported "not found" instead of comparing footprints at all, so the real
mismatches were invisible until staging was fixed.

## Guidance

The complete sidecar set a staged board needs for a faithful DRC:

| Sidecar | Governs | Missing it produces |
|---|---|---|
| `<board>.kicad_pro` | design rules, netclasses | phantom clearance violations (stock defaults) |
| `<board>.kicad_dru` | scoped custom rule exceptions | re-flagged violations the exceptions retire |
| `fp-lib-table` | footprint library resolution | mass `lib_footprint_issues` "not found", masking real mismatches |
| referenced `.pretty` dirs | the library content itself | same as above |

> **Same fingerprint, opposite cause (2026-08-08):** the `.kicad_dru` row's
> re-flagged violations also appear when the file is *present but malformed* —
> one bad constraint silently voids the whole file (exit 0, no stderr), so
> "confirm the file was staged" does not rule this out. Distinguish with the
> behavioral probe: append a deliberately-firing rule to a staged copy and
> require nonzero findings (the GUI rule editor's syntax check may also catch
> it at edit time — unverified). See
> [one bad constraint silently voids every custom rule](../integration-issues/kicad-dru-one-bad-constraint-silently-voids-every-custom-rule.md).

Rules of thumb:

- **Stage by project, not by file.** When copying a board for checking, copy
  the whole sidecar set. The set above is what Board3 needs; treat any new
  sidecar KiCad grows the same way.
- **A CLI/GUI count mismatch is a staging symptom first.** Before treating
  either number as the truth, ask what context each judge had. The GUI always
  has the full project; the CLI has whatever was staged.
- **Uniform mass findings with one repeated message are configuration, not
  design.** 120 identical "library not configured" lines mean one missing
  file, not 120 board defects.

In this repo, `tools/kicad_verify.py` stages `.kicad_pro` (with the real rules
from `tools/kicad_rules.json`) and `.kicad_dru`. It does not stage
`fp-lib-table` — harmless for its error-severity gate, since library issues
are warnings, but any tool extending it to `--severity-all` audits must add
the library sidecars or inherit the phantom class.

## Why This Matters

Phantom findings at this scale poison an audit twice: they inflate the workload
(120 items that no board edit can ever fix), and they mask the real findings
underneath (the genuine mismatches only appear once the library resolves). A
session that "fixes" toward the phantom count chases an unreachable zero; one
that dismisses the class as CLI noise throws away the 37 real items inside it.

## When to Apply

- Any tool or script that copies a `.kicad_pcb` before checking, rendering, or
  measuring it
- Reconciling GUI DRC counts with CLI DRC counts on the same board
- Auditing warning-severity classes (`--severity-all`) on imported boards,
  where library provenance is most likely to be nonstandard

## Examples

Same board, same command, staging the library sidecars:

```text
without fp-lib-table + .pretty : lib_footprint_issues 120  (all "library not found")
with    fp-lib-table + .pretty : lib_footprint_mismatch 37 (real, fixable, fixed)
```

What the *genuine* mismatches actually are — and why one of them (U2) outlived
this campaign at a count of "1" until it was diffed field-by-field — is
anatomized in the integration-issues doc linked below: instance-only
`zone_connect` overrides from the EasyEDA import, integer-nanometre precision
differences, and rotation bookkeeping, reconciled toward the board.

## Related

- [A large ERC count is a broken instrument](a-large-erc-count-is-a-broken-instrument.md) — **the largest counterexample to this doc's heuristic** that *"uniform mass findings with one repeated message are configuration, not design"*: 469 `unconnected_wire_endpoint` and 372 `pin_to_pin`, each a single repeated message, and every one of them real. The discriminator that survives both cases: configuration when the message names a **resource the checker could not resolve**, design when it names a **property of objects it read fine**. It also carries the ERC form of this doc's staging trap, where the sidecar has to match the schematic by *filename*
- [A colon in one symbol name makes the entire library unloadable](../integration-issues/kicad-colon-in-symbol-name-makes-library-unloadable.md) — **the same "library not found" message from the opposite cause.** Here the checker was misconfigured and the library was fine; there the checker was right and the library was genuinely dead. Both produce a uniform mass of identical warnings, so this doc's heuristic *"uniform mass findings with one repeated message are configuration, not design"* has a real counterexample. The discriminator is whether the message survives a correctly-staged run
- [lib_footprint_mismatch is a real diff](../integration-issues/kicad-lib-footprint-mismatch-integer-nanometre-comparison.md) — the genuine half of this warning class: what the real mismatches are made of once the phantoms are staged away, and the reconcile-toward-the-board fix
- [Refill zones before measuring a headlessly routed board](refill-zones-before-measuring-a-headlessly-routed-board.md) — the rules-staging half of the same principle: the `.kicad_pro` sidecar decides what the fill and the DRC judge against
- [Call BuildConnectivity() before counting airwires](build-connectivity-before-counting-airwires.md) — same campaign; the in-memory version of measuring against the wrong context
