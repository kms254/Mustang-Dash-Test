---
title: Freerouting headless integration for KiCad — version choice and the four traps
date: 2026-07-29
category: tooling-decisions
module: kicad/board3
problem_type: tooling_decision
component: tooling
severity: medium
applies_when:
  - "Autorouting or batch-routing a KiCad board with freerouting from a script"
  - "Exporting Specctra DSN or importing SES sessions via pcbnew"
  - "A freerouting batch run sits at ~0 CPU with no output (it is a hidden modal dialog, not a hang)"
tags: [freerouting, autorouting, kicad, dsn, ses, pcbnew, headless]
---

# Freerouting headless integration for KiCad — version choice and the four traps

> **Status: valid, but not the entry point for Board3.** This recipe is for a
> from-scratch or bulk route. Every board change since 2026-07-31 has gone
> through `tools/kicad_handroute.py` and a committed spec in
> `tools/handroutes/`, and the board is now at 0 airwires and 0 violations —
> there are no unrouted nets for an autorouter to take. That is this doc's own
> conclusion arriving on schedule, not a contradiction of it: it already ends by
> saying locked-copper autorouting cannot route a net whose pocket requires
> existing copper to move, and that the next move is displacement by hand. Read
> on for the tooling contract; start routing work at `tools/handroutes/`.

## Context

Closing Board3's last airwire (TT2, PR kms254/Mustang-Dash-Test#11) meant driving
freerouting without its GUI: export the board to Specctra DSN, run the JAR
headless, import the resulting SES. Every stage failed at least once, each
failure looked like a different problem (a hang, a silent no-op, a data-loss
import), and none of the causes was in the error output. The working recipe is
codified in `tools/kicad_freeroute.py`; this doc records why each part of that
recipe exists.

## Guidance

Use **freerouting 1.9.0, not 2.2.4** — 2.2.4 enters infinite recursion in
`PolylineTrace.combine()` while ingesting this board's DSN (log shows the same
stack frame repeating forever; suspected trigger was stacked same-net duplicate
copper, which 1.9.0 instead skips with a "Multiple vias skipped" warning).
Newer JREs run the old JAR fine. On this workstation the runtime lives at
`C:\Users\kevin\Tools\jre25-extract\` and the JARs at
`C:\Users\kevin\Tools\freerouting\` (no system Java exists).

Before and after the JAR, four traps:

1. **DSN export fails silently on unnamed footprints.**
   `pcbnew.ExportSpecctraDSN(board, path)` returns `False` with no message if
   any footprint has an empty reference designator (Board3's four
   EasyEDA-imported corner mounting pads). Synthesize refs (`XNOREF1`…) before
   export and restore the empty refs before saving anything. The same refs
   must be present again at SES import or the importer errors.

2. **The DSN reader pops a modal dialog on any non-ASCII byte — even in
   batch mode.** Ω signs in resistor values produced a "DSN file reader"
   warning window that parked two runs at ~0 CPU, indistinguishable from a
   hang unless you look at the desktop. `-dct` does not dismiss it. Strip the
   DSN to ASCII after export (replace bytes ≥ 0x80).

3. **KiCad exports locked tracks/vias as `(type fix)` wires and freerouting
   honors them** — verified: all 2,801 locks survived. This is the scalpel
   for "route only the unrouted nets."

4. **`pcbnew.ImportSpecctraSES` wipes and rebuilds every net named in the
   session**, and freerouting's session only contains wiring it owns — so
   locked copper on session-named nets is deleted wholesale. Importing a
   session from a locked-copper run took this board from 1 airwire to 276.
   Session import is only safe when freerouting was allowed to own the whole
   board; it is unusable for surgical work on an already-routed board.

The conceptual bottom line: **locked-copper autorouting cannot route a net
whose pocket requires existing copper to move.** Freerouting with full lock
semantics honored still could not route TT2, because the fix was displacing
two neighbouring nets — a shove/restack, which no amount of routing effort
within fixed walls can express. When an autorouter fails on a boxed-in net,
the next move is displacement (by hand with exact clearance math, or a shove
router), not a different autorouter.

## Why This Matters

Each trap wastes hours precisely because the failure is silent or misleading:
the export "succeeds" (returns False, writes nothing), the batch run "hangs"
(a dialog nobody can see), the import "works" (and quietly deletes 276
connections). The recipe in `tools/kicad_freeroute.py` absorbs all four, so
future routing automation starts from a working baseline instead of
rediscovering them.

## When to Apply

- Any scripted freerouting run against a KiCad board — use
  `tools/kicad_freeroute.py` rather than calling the JAR directly.
- Any `ExportSpecctraDSN` call on a board with imported (EasyEDA/Altium)
  footprints — check for empty reference designators first.
- Judging a stalled freerouting process: check CPU and the desktop for a
  dialog before assuming it is computing.
- Deciding whether to re-run an autorouter after a failure on one net: if the
  net's escape is capacity-limited by neighbouring copper, no router with
  locks will succeed — plan a displacement instead.

## Examples

```sh
# The whole pipeline, traps absorbed:
"C:/Program Files/KiCad/10.0/bin/python.exe" tools/kicad_freeroute.py \
    BOARD.kicad_pcb --out ROUTED.kicad_pcb
# then gate the result — the wrapper does not judge clearances:
python tools/kicad_verify.py ROUTED.kicad_pcb --baseline BASELINE.kicad_pcb
```

The 2.2.4 failure signature, for recognition (this exact frame repeating
endlessly in stdout/stderr):

```
at app.freerouting.board.PolylineTrace.combine(PolylineTrace.java:180)
at app.freerouting.board.PolylineTrace.combine(PolylineTrace.java:180)
...
```

## Related

- `tools/kicad_freeroute.py` — the codified recipe (module docstring repeats the traps)
- `tools/kicad_handroute.py` and `tools/handroutes/` — where routing work on this board actually starts now: surgical edits from a committed spec, with the reasoning recorded in each spec's `$why` block
- `tools/kicad_verify.py` — the gate to check a route against. Both CI design gates went absolute on 2026-08-05, and the verifier now stages the library sidecars and judges at `--severity-all`; `--baseline` survives for interactive attribution
- `docs/solutions/developer-experience/clip-test-board-window-queries.md` — the query bug that shaped the same routing session
- `docs/solutions/developer-experience/refill-zones-before-measuring-a-headlessly-routed-board.md` — zone hygiene for the same headless pipeline
- CLAUDE.md "Autorouting Board3" block — compressed version of this doc
