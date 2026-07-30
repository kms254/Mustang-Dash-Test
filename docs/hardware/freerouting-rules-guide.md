# Freerouting rules guide — making the autorouter succeed on Board3-class boards

The complete contract for a successful headless freerouting run: what to
protect, what rules to stage, how to run, and what "done" means. The
greenfield experiment branch (`exp/freerouting-greenfield`) is the proving
ground; `tools/kicad_freeroute.py` is the executable half of this guide.

## The division of labour

Freerouting is a **bulldozer**: excellent at routing many unrouted nets on a
board with room, useless at surgical work on a routed board (its session
import wipes locked copper — see
`docs/solutions/tooling-decisions/freerouting-headless-integration-for-kicad.md`).
So the workflow is: strip everything an autorouter is allowed to own, protect
everything it isn't, give it real rules, and judge the result with the same
gates as hand routing.

## 1. The protect list — `tools/kicad_netclass.json`

Single source of truth, shared with `kicad_strip.py` and `kicad_measure.py`.
Never define a second list. Current groups and why they're held out:

| Group | Nets | Why the router must not touch it |
|---|---|---|
| usb_differential_main | USB_DP, USB_DM | 480 Mbps pair, 0.22% skew, zero vias — an autorouter can only degrade it |
| usb_differential_connector | USB_DP_CONN, USB_DM_CONN | Same pair, connector side |
| qspi | QSPI_* (7 nets) | Documented walk-back: an automated fix buried six escape vias inside U2's SMD pads |
| swd | SWCLK, SWDIO, SWO | Audited good; debug must work on first power-up |
| crystal | OSC_IN, OSC_OUT | Tight loop at the MCU; length/loop area matter more than completion |
| can_differential | CAN1_H/L, CAN2_H/L | Differential pairs, audited |

Physical placement is never at stake: freerouting cannot move footprints, so
connectors and every component stay put by construction.

## 2. The rules layer — staged into the experiment's `.kicad_pro`

KiCad's DSN exporter carries netclass widths/clearances/vias into the DSN, so
rules are staged in the **project file beside the stripped board copy**,
never in the shipping project. Staged classes:

| Class | Width | Clearance | Via | Assigned by pattern |
|---|---|---|---|---|
| Default | 0.254 | **0.13** | 0.6/0.3 | everything else |
| Power | 0.5 | 0.15 | 0.6/0.3 | `/+5V*`, `/+3V3`, `/VBUS`, `/GATE_BARREL` |
| GndStitch | 0.4 | 0.13 | 0.6/0.3 | `/GND` |

The load-bearing trick: **route-time clearance (0.13) sits above the DRC
gate's clearance (0.1016)**. Freerouting routes with ~28 µm of headroom, so
its output passes the real gate even where its geometry is sloppy at the
margin. Never route at exactly the gate value — every rounding error becomes
a violation.

Power nets get real widths because the strip deletes the original power
distribution tracks; without a Power class the router would rebuild 3 A
rails at signal width. Zones (GND ×3, +5V on In2) survive the strip and are
exported as DSN planes, so plane-connected pads don't get routed at all —
the classes govern only what genuinely needs new copper.

## 3. The run

```sh
# 1. Strip (census by default; --out required to actually write):
"C:/Program Files/KiCad/10.0/bin/python.exe" tools/kicad_strip.py \
    kicad/board3/<board>.kicad_pcb --out <scratch>/exp.kicad_pcb
# 2. Copy the .kicad_pro beside it and stage the classes above into
#    net_settings (classes + netclass_patterns).
# 3. Route — the wrapper locks all surviving copper (= the protect list),
#    fixes unnamed footprints, strips non-ASCII, runs 1.9.0:
"C:/Program Files/KiCad/10.0/bin/python.exe" tools/kicad_freeroute.py \
    <scratch>/exp.kicad_pcb --out <scratch>/exp-routed.kicad_pcb --passes 40
```

Version and trap details (2.2.4 recursion bug, modal-dialog hang, silent DSN
export failure on unnamed footprints) live in the solutions doc; the wrapper
absorbs all of them.

## 4. The gates — what "great" means

Run all three; any one alone can lie:

1. **Completion**: unrouted airwires = 0 (the wrapper prints the count after
   import).
2. **Legality**: `python tools/kicad_verify.py <routed> --baseline <shipped>`
   → NEW = 0. The SES-import path is trusted only on a *stripped* board;
   protected nets must additionally be diffed against their pre-route
   geometry, because a session that names a protected net wipes it — check
   before celebrating.
3. **Quality**: `python tools/kicad_measure.py <routed>` vs the same run on
   the shipped board — total length, via count, and identical per-group
   numbers for the protected nets. Completion with 3× the vias is not
   "great".

## 5. Iteration levers, in the order to reach for them

1. **Passes** (`--passes`): more rip-up attempts; cheap, try first.
2. **Clearance headroom**: if verify shows margin violations, raise Default
   clearance 0.13 → 0.15 and re-run rather than repairing output.
3. **Via cost / layer discipline**: 1.9.0's defaults alternate preferred
   direction per layer. If signal congestion collides with the In2 +5V zone,
   consider closing In2 to signals in the DSN.
4. **Protect-list edits**: if the router consistently ruins one net, move it
   to the protect list and hand-route it — the TT2 lesson: some pockets are
   displacement problems, not routing problems.
5. **Placement**: the one lever this pipeline cannot pull. If iteration
   plateaus with airwires stuck above zero, the fix is placement (e.g.,
   spreading U11's lamp pins across its package next spin), not more passes.

## 6. Seed-then-flood — the ordering that actually works

Four iterations on the greenfield experiment produced a clear pipeline
ordering (numbers: 247 airwires greenfield, Board3, 2026-07-29):

| Iteration | Approach | Result |
|---|---|---|
| 1 | Pure flood (protect list locked) | 226/247 routed in ~5 min, copper fully legal, 4 old edge violations *resolved* |
| 2 | Re-flood with everything locked to finish stragglers | 50-minute stall, nothing saved — locked boards give the router no room |
| 3 | Transplant proven copper *after* flooding | 87/693 donor items collide with flood copper — broken chains |
| 4 | **Seed proven copper first, then flood** | 240/247, clean annulars, small tail |

Rules that fell out:

- **Seed before flooding, never after.** Proven copper (GND stitching, known-hard
  nets like the telltale pockets, dense-fanout power spurs) goes into the
  *stripped* board, where it cannot collide, and gets locked. The flood routes
  the tractable majority around it. Transplanting into a routed board breaks
  proven chains at every collision.
- **The DSN plane trap:** zones export as planes, so freerouting believes every
  zone-net pad is already connected and will never add GND stitching — seeded
  GND vias/tracks are the only source of stitching. Without them, expect
  starved thermals and fill-island airwires the router cannot even see.
- **Carry exact via geometry when seeding.** Dump body width per layer
  (`GetWidth(layer)` in KiCad 10) as well as drill; rebuilding a 0.61/0.305 via
  as 0.6/0.305 shaved the annular ring below the 0.15 minimum and produced 32
  violations that looked like router output.
- **The flood has a non-deterministic tail.** Successive identical runs
  complete a different ~97% — a net routed in one run may be dropped in the
  next (long cross-board runs are the usual victims). Budget a finishing pass:
  re-flood the tail or hand-close it; do not chase determinism with more
  passes.
- **Locked-board re-floods never converge** — iteration 2's stall is
  structural, the same lesson as the TT2 pocket. If the flood leaves a net
  unrouted twice, it needs seeding or hand work, not retries.

## History

- 2026-07-29: guide created with the `exp/freerouting-greenfield` experiment.
  Strip removed 1,708 primitives → 247 airwires of greenfield; 20 protected
  nets (1,165 tracks, 19 vias) held byte-identical. Prior art: the July
  KiCad-evaluation comparison used the same protect list with
  KiCadRoutingTools; PR kms254/Mustang-Dash-Test#11 records why surgical
  autorouting was abandoned in favour of this division of labour.
