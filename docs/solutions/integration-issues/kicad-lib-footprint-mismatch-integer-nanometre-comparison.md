---
title: "lib_footprint_mismatch is a real diff: KiCad compares footprints in integer nanometres, and the library reconciles toward the board"
date: 2026-08-01
category: integration-issues
module: kicad-footprints
problem_type: integration_issue
component: tooling
severity: medium
symptoms:
  - "DRC warning `lib_footprint_mismatch` on U2 (WSON-8) persisted through the whole Board3 pre-fab campaign and was written off as pre-existing/cosmetic"
  - "Board and library footprints look identical in the GUI; the differences only surface in a field-by-field pcbnew dump"
  - "Solder-paste aperture differs by 3 nanometres: 1216667 nm computed programmatically as (4.65-1.0)/3 vs 1216670 nm hand-typed to 5 decimals"
  - "Pad rotation bookkeeping differs (footprint-relative 180 on the board vs 0 in the library) on geometrically identical rectangles"
  - "`(zone_connect 2)` overrides exist on pads 4 and 9 in the board copy only, dating to the EasyEDA-to-KiCad import"
root_cause: config_error
resolution_type: config_change
related_components:
  - easyeda-workflow
tags: [kicad, drc, lib-footprint-mismatch, footprint-library, integer-nanometres, easyeda-import, pcbnew, board3]
---

# lib_footprint_mismatch is a diff, not noise — KiCad compares integer nanometres

## Problem

The Board3 pre-fab campaign closed every DRC item except one: a
`lib_footprint_mismatch` warning — "Footprint 'WSON-8_L8.0-W6.0-P1.27-TL-EP'
does not match copy in library 'ProPrj_New-easyedapro'" — that sat at
"1 violation" for the whole campaign and was repeatedly dismissed as
pre-existing and cosmetic. It *did* pre-exist the campaign (the EasyEDA import
created it), but the campaign then added **two more mismatch families on top
without noticing**, because the warning count never changes when you add new
differences to an already-mismatched footprint. The mechanism is KTD26's own
premise: KiCad embeds a copy of every footprint per placement, so any rule that
says "fix it in the library AND the instances" produces two edits that must be
byte-equivalent in ways that are easy to get subtly wrong — here, a windowed
exposed-pad paste aperture hand-typed as text into
`kicad/board3/ProPrj_New-easyedapro.pretty/WSON-8_L8.0-W6.0-P1.27-TL-EP.kicad_mod`
and computed programmatically via `pcbnew` into the U2 instance in
`kicad/board3/ProPrj_New Project_2026-07-15_23-14-34_2026-07-27.kicad_pcb`.

## Symptoms

- `kicad-cli pcb drc --severity-all` reports 1 `lib_footprint_mismatch`
  warning on U2's WSON-8 footprint, and the count never moves no matter what
  else on the board is fixed.
- Reading the two files side by side shows apertures that *look* identical —
  same nominal sizes, same offsets to the eye.
- The warning survives edits that were believed to make the copies match
  (adding the same apertures "identically" to both files).

## What Didn't Work

- **Treating the warning as a single pre-existing cosmetic artifact.** It was
  actually three independent difference families, two of them introduced by
  the campaign itself. A warning that stays at "1" is not one problem; it is
  an opaque boolean.
- **Casual side-by-side reading of the two files.** `1.21667` vs `1.216667`
  and a missing-vs-present rotation token do not register on a human diff of
  two 400-plus-line s-expression files, and pad-local properties like
  `zone_connect` hide in the noise.
- **(session history) The prior playbook never reached this warning class.**
  The repo's established treatment for inherited "cosmetic" DRC noise was
  scoped `.kicad_dru` exception rules (fix/board3-cosmetic-drc, 2026-07-30) —
  exemption with a recorded reason, not reconciliation of the underlying
  data — and the warning-inbox campaign's 146 → 0 counted violation classes,
  so this warning-severity mismatch survived a truthful "DRC-clean" bill. No
  prior session attempted to diff or explain it.

## Solution

First, make the warning explain itself: a field-by-field diff through
`pcbnew` — `FootprintLoad(libpath, name)` for the library copy,
`board.FindFootprintByReference("U2")` for the board copy — comparing per pad:
number, size, shape, `GetZoneConnection()`, orientation normalized by the
footprint's rotation, and per-layer membership, plus graphics counts per
layer, Value, attributes, and 3D models. That surfaced exactly three families:

1. **`(zone_connect 2)` on pads 4 and 9 only in the board copy** — the
   original, pre-campaign cause. The EasyEDA-to-KiCad import wrote pad-local
   zone-connect overrides into placed instances but not into the exported
   library footprint.
2. **Three nanometres.** The board apertures were computed as
   `(4.65 - 1.0) / 3` and rounded to 6 decimals → `1.216667` mm
   (1,216,667 nm). The library apertures were hand-typed at 5 decimals →
   `1.21667` mm (1,216,670 nm). KiCad's comparator works in integer
   nanometres, so these are simply different numbers.
3. **Rotation bookkeeping.** The board apertures were added programmatically
   with absolute angle 0 on a footprint placed at 180°, making them
   footprint-relative 180; the library had no angle (0). Identical geometry
   for rectangles — different numbers to the comparator.

The fix reconciled the **library toward the board** — the board is the design
and the only artifact that gets fabricated; the library copy is reference
material. Per this session's conclusion this direction also matches the repo's
larger state: EasyEDA was retired 2026-07-31 and `kicad/board3` is
authoritative, so the placed instance is the ground truth to preserve.

The zone_connect diff, before/after in the `.kicad_mod` (board copy was
already correct):

```
; before (library pad 4 — no override)          ; after (matches board)
(pad "4" smd rect                               (pad "4" smd rect
    (at -4 1.905 270)                               (at -4 1.905 270)
    (size 0.7 1)                                    (size 0.7 1)
    (layers "F.Cu" "F.Mask" "F.Paste")              (layers "F.Cu" "F.Mask" "F.Paste")
    ...                                             (zone_connect 2)
```

Now present in both files: `.kicad_mod` lines 375 (pad 4) and 406 (pad 9);
`.kicad_pcb` lines 18482 (pad 4) and 18528 (pad 9).

The 3 nm + rotation diff, before/after for one library aperture line:

```
; before: hand-typed 5 decimals, no angle  ->  1216670 nm, relative 0
(pad "" smd rect (at -1.71667 -1.9) (size 1.21667 1.4) (layers "F.Paste") ...)

; after: board's 6-decimal values, explicit 180  ->  1216667 nm, relative 180
(pad "" smd rect (at -1.716667 -1.9 180) (size 1.216667 1.4) (layers "F.Paste") ...)
```

The reconciled library apertures are
`kicad/board3/ProPrj_New-easyedapro.pretty/WSON-8_L8.0-W6.0-P1.27-TL-EP.kicad_mod`
lines 409–417; the corresponding board-instance apertures are
`kicad/board3/ProPrj_New Project_2026-07-15_23-14-34_2026-07-27.kicad_pcb`
lines 18385–18447 (the U2 footprint starts at line 18025, `(at 174.09 107.206
180)` — which is why the board file can omit the pad angle token: absolute 0
on a 180° footprint *is* relative 180).

Two execution traps hit during the edit itself:

- **The `.kicad_mod` is CRLF** (verified with `file`: "ASCII text, with CRLF
  line terminators"), so multi-line text anchors for Edit-style replacements
  must be CRLF-aware or they silently fail to match.
- A careless find-replace dropped a trailing space and produced malformed
  `(at -1.716667-1.9)` — two tokens fused into one — which needed a repair
  pass. S-expression files do not forgive lost whitespace.

Verification: `kicad-cli pcb drc --severity-all` → **0 violations, 0
unconnected** (down from 1 warning). Committed as `1618b88` — "fix(kicad):
reconcile the WSON-8 library copy -- DRC 0/0, inbox zero" — which, as of this
writing, sits on the local branch `fix/board3-prefab-blockers`.

## Why This Works

KiCad's footprint comparator does not compare "the design intent"; it compares
stored fields, with all lengths held as integer nanometres and pad-local
overrides (`zone_connect`, thermal settings) treated as first-class data. So:

- `1.21667` mm and `1.216667` mm differ by 3 nm and therefore differ, period.
  Decimal-precision *habits* (hand-typing 5 decimals vs a script printing 6)
  create real mismatches even when both values came from the same formula.
- A rotation of 0 vs 180 on a rectangular pad is geometrically identical but
  numerically distinct, and the comparator sees numbers.
- An importer that writes `zone_connect` into instances but not the library
  export creates a mismatch that no amount of aperture-fixing will clear.

Reconciling library → board works because only the board is fabricated: gerber
and paste output come from the embedded instance copies. Editing the library
to match the board changes nothing about what gets manufactured, closes the
warning, and removes the re-link regression risk (KTD26's other half: an
instance-only fix is silently reverted the next time the footprint is updated
from the library).

## Prevention

- **One code path, two targets.** When a rule (KTD26) demands the same edit in
  the library file and the placed instances, generate both from the same
  script — the same floats, the same rounding, the same angle bookkeeping.
  Never hand-type a rounded value in one place and compute it in another; that
  is two independent sources of truth pretending to be one.

  > **This is necessary, not sufficient — and on its own it points the wrong
  > way.** One code path removes the *authoring* discrepancy; it does not make
  > exact comparison correct. A single script that computes cut points in
  > absolute board coordinates still lands the same edit 1 nm apart on two
  > placements of one footprint, because the coordinates differ before the
  > rounding does. A comparator demanding exact equality then reports a
  > divergence that does not exist, and a later campaign was reverted on exactly
  > that reading. Pair this rule with a **tolerance the physical domain can
  > hold** — a nanometre is a millionth of a millimetre and no fab has any
  > relationship to it. See
  > [a tool's finding can be a property of the tool](../developer-experience/a-tools-finding-can-be-a-property-of-the-tool.md).
- **Diff immediately after any dual edit.** If both copies were touched by
  different means anyway, run a field-by-field comparison before calling the
  work done:

  ```python
  # kicad's python.exe: "C:/Program Files/KiCad/10.0/bin/python.exe"
  import pcbnew

  board = pcbnew.LoadBoard(r"...\ProPrj_New Project_2026-07-15_23-14-34_2026-07-27.kicad_pcb")
  bfp = board.FindFootprintByReference("U2")
  lfp = pcbnew.FootprintLoad(r"...\ProPrj_New-easyedapro.pretty",
                             "WSON-8_L8.0-W6.0-P1.27-TL-EP")

  def pad_key(p, base_angle):
      return (p.GetNumber(),
              tuple(p.GetSize()),                      # integer nm — exact
              p.GetShape(),
              p.GetZoneConnection(),                   # pad-local overrides count
              (p.GetOrientationDegrees() - base_angle) % 360,
              tuple(sorted(l for l in p.GetLayerSet().Seq())))

  b = sorted(pad_key(p, bfp.GetOrientationDegrees()) for p in bfp.Pads())
  l = sorted(pad_key(p, 0) for p in lfp.Pads())
  for x in b:
      if x not in l: print("board-only:", x)
  for x in l:
      if x not in b: print("lib-only:  ", x)
  ```

  Normalizing pad orientation by the footprint's own rotation is the step
  that makes board (absolute angles) and library (relative angles) comparable.
- **Treat integer nanometres as the unit of truth.** "The same number" at
  different decimal precision is not the same number. When a dimension is
  derived (`(4.65 - 1.0) / 3`), let one script derive it everywhere.
- **Remember pad-local overrides are part of the comparison.** Importers write
  them asymmetrically; `zone_connect`, thermal gaps, and paste margins can all
  make two visually identical footprints mismatch.
- **A warning that never clears gets waved through — don't let it.**
  `lib_footprint_mismatch` is a diffable, explainable signal; because it is a
  boolean per footprint, its count stays flat while new differences pile up
  underneath it. This is the sibling of the repo's existing convention doc:
  a gate that cannot pass gets waved through, and so does a warning nobody
  expects to change. Either drive it to zero or produce the field-level diff
  that proves exactly what it contains — "pre-existing" is not a diagnosis.
  Scoped `.kicad_dru` exemption is for geometry that is *correct by intent*;
  a library/instance mismatch is data drift and should be reconciled, not
  exempted. (session history)
- **CRLF and whitespace discipline in `.kicad_mod` edits.** These files are
  CRLF in this repo; multi-line anchors must match that, and every token
  boundary matters (`(at -1.716667-1.9)` is what a dropped trailing space
  looks like).

## Related Issues

- [stage-project-sidecars-for-headless-drc](../developer-experience/stage-project-sidecars-for-headless-drc.md) —
  the *phantom* half of this warning class: missing staged sidecars fabricate
  `lib_footprint_mismatch` findings; this doc explains the *genuine* ones.
- [a-gate-that-cannot-pass-gets-waved-through](../conventions/a-gate-that-cannot-pass-gets-waved-through.md) —
  the adjacent failure mode this doc extends from gates to warnings.
- [easyeda-pro-to-kicad-migration-silent-data-loss](./easyeda-pro-to-kicad-migration-silent-data-loss.md) —
  upstream cause: the import that wrote pad-local overrides into instances but
  not the exported library (a further divergence mode beyond its six).
- KTD26 (Board3 pre-fab campaign rule,
  `docs/plans/2026-07-31-001-fix-board3-prefab-blockers-plan.md`): footprint
  fixes must land in both the library and the placed instances — the rule this
  doc's failure mode grows out of.
- `docs/reviews/2026-07-31-board3-prefab-review.html` — the pre-fab review
  whose baseline already carried this warning. (session history)
- CLAUDE.md's "read board facts through `pcbnew`, never regex" rule — a 3 nm
  size asymmetry is exactly the class of fact text-reading misjudges.
  (session history)
- Commit `1618b88` on `fix/board3-prefab-blockers` (local as of this
  writing): "fix(kicad): reconcile the WSON-8 library copy -- DRC 0/0, inbox
  zero".
