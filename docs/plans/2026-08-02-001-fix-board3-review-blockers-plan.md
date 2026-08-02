---
artifact_contract: ce-unified-plan/v1
artifact_readiness: implementation-ready
execution: code
title: Close the 2026-08-02 six-lens review blockers and consolidate the BOM
date: 2026-08-02
branch: fix/board3-review-blockers
---

# Close the 2026-08-02 review blockers, then consolidate the BOM

## Goal Capsule

Board3 was one click from fabrication when a six-lens review returned **NO-GO**
on three blockers. The headline one is that the fix for the previous review's
CRITICAL finding is installed backwards and does nothing. This plan closes all
three, folds in the cheap findings that need the same GUI session, and takes the
BOM from 48 lines to 45 while permanently removing the defect class that broke
the JLCPCB uploader.

**Done means:** the board is orderable with every blocker closed, verified by
netlist and measurement rather than by inference, and the fab package regenerated
under KiCad's own interpreter.

## Why this plan exists

The 2026-07-31 pre-fab review returned NO-GO with 1 CRITICAL + 5 HIGH. Those were
fixed in PR #17 and the board reached DRC 0/0 with the fab package accepted by
JLCPCB's uploader. A confirmatory six-lens review then found that one of those
fixes was inverted, and that two problems had never been reviewed at all.

Five of six lenses independently found the diode defect, each from a different
angle. That convergence is why this plan is not optional.

## Verification Contract

Every unit states how it is proved. Three rules bind all of them, each paid for
in this repo:

1. **Read board facts through `pcbnew`, never regex on the `.kicad_pcb`.** Text
   parsing has produced at least four confidently wrong conclusions here.
2. **Verify by pin FUNCTION, not pin number.** This is the direct lesson of U31 —
   see KTD27.
3. **Run `tools/kicad_fab.py` under `C:/Program Files/KiCad/10.0/bin/python.exe`.**
   A bare `python` silently degrades the package and still reports success.

DRC is run in place, or with the FULL sidecar set (`.kicad_pro`, `.kicad_dru`,
`fp-lib-table`, both `.pretty` dirs). The `.kicad_dru` is load-bearing: without
it the board reports 36 violations, all inside its three declared scopes.

## Definition of Done

- `/U11_RSTN` and `/U12_RSTN` each carry a diode **anode**; `/+5V` carries both **cathodes**
- DRC 0 violations / 0 unconnected at `--severity-all`, airwires 0
- Power input path copper meets `docs/hardware/board3-pcb-layout-guide.md` §3
- `tools/kicad_lcsc.py check` PASS
- BOM/CPL symmetric; `kicad_fab.py` exits 0 with gerbers named `F_Cu.gbr` et al.
- BOM at 45 lines or fewer, with zero same-Comment/different-part-number pairs

---

## Sequencing

**U41 runs first and gates everything else.** The remaining units start from a
clean, merged `main` on a fresh branch — not from the current working tree.

    U41  ship what's already done  ──►  merged to main
                                          │
                                          ▼
                                   new branch: fix/board3-review-blockers
                                          │
             ┌────────────────────────────┼────────────────────────────┐
             ▼                            ▼                            ▼
       U35 (no GUI)              U31, U32, U34, U36              U33 (routing)
       metadata merges           one GUI sync covers all         power copper
             │                            │                            │
             └────────────────────────────┴────────────────────────────┘
                                          ▼
                              U37, U38, U39, U40 (cleanup)
                                          ▼
                              regenerate fab package, order

U31, U32, U34 and U36 all require the KTD12 GUI sync, so they batch into **one**
`Update PCB from Schematic` pass rather than four. U35 needs no sync at all and
can be done any time. U33 is independent copper work.

---

## Implementation Units

### U41. Ship what is already done, then start clean  **RUNS FIRST**

**Goal:** get the current working tree onto `main` so this plan's work starts
from a known state rather than layering onto uncommitted edits.

**Why first.** Two things are sitting uncommitted right now, and one of them is a
correction to a claim already made in a merged PR. Carrying them into the blocker
work would mix a documentation fix with a schematic fix in the same branch, and
the blocker work needs a clean diff — U31 in particular must show *only* the
diode change so the netlist proof is unambiguous.

**Current uncommitted state:**

| File | What it is | Action |
|---|---|---|
| `CLAUDE.md` | AW9523B RSTN datasheet facts pulled 2026-08-02 (V_IH 1.4 V fixed, 100 kΩ internal pull-down, ≤10 µs glitch filter, the 10k/100n network's 335 µs / 4.55 V figures) | **commit** |
| `docs/plans/2026-08-02-001-...-plan.md` | this plan | **commit** |
| `EasyEDA/New Project_...eprj2` | Kevin's, binary, predates the session | **leave unstaged** |

Note: the CLAUDE.md edit was previously described as riding with PR #19. It did
not — #19 merged without it. This unit is where it actually lands.

**Approach:**

1. Branch off updated `origin/main` (PR #19 is merged, so `main` already carries
   the DNP fix, the BOM/CPL symmetry gate, and the ASCII/footprint hardening).
2. Commit the two files. Do **not** stage the `.eprj2`.
3. Open the PR via `/ce-commit-push-pr`. The description should state plainly
   that the plan documents a **NO-GO** verdict with three fabrication blockers —
   this PR ships the diagnosis, not the fix.
4. **Wait for merge.** Do not begin U31 on this branch.
5. After merge, `git fetch origin main` and create `fix/board3-review-blockers`
   from it. All remaining units land there.

**Verification:**
- `git status` shows only the `.eprj2` modified
- The PR contains exactly two files
- `fix/board3-review-blockers` branches from a `main` that includes this plan
- `tools/kicad_lcsc.py check` still PASS and DRC still 0/0 on the fresh branch —
  a sanity check that nothing was disturbed by the branch dance

**Execution note:** this unit ships documentation only. No board file, no
schematic, no tooling changes. If the diff contains anything else, stop.

---

### U31. Reverse D10/D11 — the RSTN networks are currently inert  **BLOCKER**

**Goal:** make the Schottky discharge diodes do what their commit message says.

**The defect.** Both diodes sit anode-on-`/+5V`, cathode-on-RSTN — the opposite
of intent. Confirmed five ways: symbol pin names (`K`=1, `A`=2 in
`JLCImport.kicad_sym`), symbol geometry, the exported schematic netlist
(`pinfunction "K_1"` inside `/U11_RSTN`), the `pcbnew` pad-net map, and the
footprint silk band sitting at the pad-1 end.

**Consequence, quantified.** The diode is forward-biased during the rail ramp, so
C70/C71 charge through it rather than through R60/R61. RSTN crosses its fixed
1.4 V V_IH at VCC ≈ 1.5–1.6 V — *below* V_POR (1.8 V typ). **Reset assertion
duration is exactly zero at every ramp rate.** The brownout half fails in the
same direction: reverse-biased on collapse, so the cap holds RSTN high and
discharges only through R60 (τ ≈ 909 µs). This is worse than fitting no diode,
which would at least deliver ~335 µs of power-up hold.

**Files:** `kicad/board3/*.kicad_sch`, `kicad/board3/*.kicad_pcb`

**Approach — zero copper movement.** Both footprints are pad-symmetric about
their origin, measured exact to **0 nm** on both axes with identical pad sizes
(pads at rel ±1.172 mm, 1.000 × 0.700 mm). So:

1. In the schematic, swap the connections: pin 1 (K) → `/+5V`, pin 2 (A) → the
   RSTN net, on both D10 and D11.
2. In the board, rotate D10 and D11 **180°**.
3. Pad 1 (now `/+5V`) lands exactly where the `/+5V` copper already terminates;
   pad 2 (now RSTN) lands where the RSTN copper is. No track is edited.

Doing only step 1 leaves the CPL orienting the old way. Doing only step 2 shorts
`/U11_RSTN` to `/+5V` — pad numbers carry the nets, so rotation alone moves the
wrong net onto the wrong copper. **Both steps, or neither.**

**Verification:**
- Export the netlist and assert `A_*` on `/U11_RSTN` and `/U12_RSTN`, `K_*` on `/+5V`
- `pcbnew`: `D10.1 == /+5V`, `D10.2 == /U11_RSTN` (same for D11)
- DRC unchanged at 0/0, airwires still 0
- Regenerate the fab package and read the rotation audit for D10/D11

**Execution note:** verify by pin **function**. Asserting pin-number membership
is exactly the check that let this through.

---

### U32. Resize F1 — the plan's own stop condition has already fired  **BLOCKER**

**Goal:** an overcurrent element that does not nuisance-trip in normal operation.

**The defect.** F1 is C960026, 2 A hold / 4 A trip, derating to **~1.4 A hold at
60 °C** by the previous plan's own note. `docs/hardware/board3-pcb-layout-guide.md`
§3 states the input path carries **1.5–2 A**. Two independent load tallies during
review returned 1.74 A and 1.90 A typical. The previous plan's stop condition —
*"if steady-state exceeds ~1.3 A, the part moves up a size before order"* — is
satisfied by numbers already in the repo, with no bench measurement needed.

The 400 mA "logic side" figure that sized the original part omits the MCU, both
CAN transceivers, and the telltales.

**Consequence:** a PPTC at 1.36–1.64× hold latches off in seconds to minutes. The
dash goes dark mid-session and stays dark until DC1 is unplugged and the part
cools. Before tripping, self-heating raises its resistance and compounds U33.

**Files:** `kicad/board3/*.kicad_sch`

**Approach:** swap to **C48985875** (LUTE 1812L300/30GR) — 3 A hold / 5 A trip,
30 V, **same 1812 land pattern**, 10 mΩ initial vs 20 mΩ (halving the series drop
U33 addresses), 47.5 k stock, $0.168. Add with
`python tools/kicad_lcsc.py add C48985875`, then `kicad_lcsc.py models`.

**Verification:** `/DC1_IN` still has exactly two pads (`DC1.1`, `F1.1`) — the
part must stay in series, not bridged. `kicad_lcsc.py check` PASS. Board
footprint `Value` reads the new part so the Component Marking Layer stays honest.

---

### U33. Widen the power input path — 10 mil against a documented 60 mil  **BLOCKER**

**Goal:** input copper that meets the project's own layout guide.

**The defect.** `docs/hardware/board3-pcb-layout-guide.md:104` specifies:

    | Input (jack/USB) → ideal diodes → +5V | ~1.5–2 A | pour or ≥60 mil |

Measured through `pcbnew`:

| Net | Length | Width | Zone |
|---|---|---|---|
| `/DC1_IN` | 9.7 mm | **100% at 0.254 mm** | none |
| `/+5V_BARREL` | 34.3 mm | **100% at 0.254 mm** | none |
| `/VBUS` | 62.9 mm | **100% at 0.254 mm** | none |

0.254 mm is 10 mil — **six times narrower than the stated minimum**, on the net
where all board current sums. `/DC1_IN` is copper laid during the previous
campaign at the net-class default, which extended the undersized path rather than
fixing it.

Additionally: Q2's drain has **zero `/+5V` vias within 4 mm**; the nearest is
16.15 mm away, so current crosses 16 mm of 10 mil top-layer copper before
reaching the Inner2 plane.

**Consequence:** IPC-2221 gives 0.254 mm 1 oz external ≈ 1.0–1.2 A at 20 °C rise.
At 1.9–2.29 A that is a **56–86 °C rise** — 116–146 °C in a 60 °C cabin, at or
above JLC standard FR-4 Tg. Series resistance ~145–175 mΩ costs 0.28–0.40 V
before the rail reaches the plane. U3 (TPS563201) has V_IN(min) = 4.5 V; a 5 V
−5% adapter at peak load lands at **~4.35 V, below UVLO** — the 3.3 V rail
collapses. Backlight boost converters are constant-power loads, so they draw more
as the rail sags: positive feedback.

**Files:** `kicad/board3/*.kicad_pcb`

**Approach:** widen `/DC1_IN` and `/+5V_BARREL` to ≥1.5 mm or pour them; add a
via field (≥6 vias at 0.6/0.25) at Q2's drain into the Inner2 `/+5V` plane. Give
`/VBUS` the same treatment where routable. This is the only unit needing genuine
routing work.

**Verification:** re-run the width census; no segment on those three nets below
1.5 mm unless poured. DRC 0/0 after `ZONE_FILLER.Fill()` **then**
`board.BuildConnectivity()` — in that order. `GetConnectivity()` alone reads
pre-fill state and lies in both directions.

---

### U34. C70/C71 100 nF → 1 µF — sized for a slow rail ramp

**Goal:** reset hold that survives the actual ramp rate, which has never been
measured.

Even correctly oriented (U31), 100 nF meets the ~100 µs hold requirement only for
rail ramps faster than ~4 ms:

| +5V ramp | 100 nF | 1 µF |
|---|---|---|
| 1 ms | 405 µs | 3383 µs |
| 5 ms | **73 µs** | 3649 µs |
| 10 ms | **0 µs** | 4047 µs |
| 50 ms | **0 µs** | 730 µs |

The ramp here is set by the adapter, the LM74700 gate slew, and ~76 µF of bulk.
1 µF fits the same 0603 land and covers ramps to 50 ms. All values sit far above
the part's 10 µs RSTN glitch filter, so the filter is never the binding constraint.

**Files:** `kicad/board3/*.kicad_sch`. Same land pattern — no re-route.

**Verification:** schematic and board `Value` agree; `kicad_lcsc.py check` PASS.

---

### U35. BOM consolidation, zero-copper tier — 48 → 46 lines

**Goal:** collapse duplicate part numbers that exist only because parts arrived
in different EasyEDA import batches.

Two pairs are electrically identical and same-package, differing only in
manufacturer — and in each case one side is already **Basic** while the other is
**Extended**:

| Used by | Currently | Replace with | Why |
|---|---|---|---|
| C17–C28 (12) | **C1591** Samsung, 100 nF 50 V X7R 0603, *Extended* | **C14663** YAGEO, identical spec, **Basic** | already fitted on 31 other positions |
| R11–R13, R45–R47 (6) | **C98220** YAGEO, 10 k ±1% 100 mW 0603, *Extended* | **C25804** UNI-ROYAL, identical spec, **Basic** | already on R1/R2/R7/R49/R60/R61; 4× cheaper |

**Saves:** 2 BOM lines, 2 extended-part feeder fees (~$6), 2 feeder positions.
Also removes the last same-Comment/different-part-number pair on the 100 nF line —
the exact family that broke the JLCPCB uploader (KTD28).

**Files:** `kicad/board3/*.kicad_sch` — property edits only (`Supplier Part`,
`Manufacturer Part`, `Manufacturer`, `JLCPCB Part Class`, `Description`) on 18
symbols. `Value` strings are already identical, so board footprint Values are
already correct: **no GUI sync, no footprint change, no net change.** This is the
U10/U24 precedent.

**Verification:** gerbers and CPL byte-identical before/after; only
`bom-jlcpcb.csv` moves. `kicad_lcsc.py check` PASS.

---

### U36. BOM consolidation, package tier — C67 0603 → 0805, 46 → 45 lines

**Goal:** make the panel bulk caps one part, and kill the uploader collision at
its root rather than working around it.

There is one bulk cap per panel connector: **C66** 22 µF/0805 at FPC1, **C67**
10 µF/0603 at FPC2, **C68** 22 µF/0805 at FPC3. C67 is the odd one out, and its
0603 land is **inherited from the EasyEDA import, not chosen**. It is also the
board's only 10 V-rated capacitor — the most heavily DC-bias-derated part here,
delivering roughly half its label at 3.3 V, so its effective capacitance is nearer
5 µF against its siblings' ~17 µF.

Changing it to **C45783** (22 µF 0805) makes FPC2 match FPC1/FPC3, removes a BOM
line, and permanently deletes the `10uF` Comment collision that
`disambiguate_comments()` currently works around.

**Files:** `kicad/board3/*.kicad_sch`, `kicad/board3/*.kicad_pcb`

**Approach:** footprint change 0603 → 0805 plus a local re-route at FPC2. Needs
the KTD12 GUI sync — which U31/U32/U34 already require, so it rides along.

**Do NOT** take the tempting zero-copper shortcut of DNP-ing C67: that deletes the
**center** panel's local bulk, and the center panel is the primary one.

**Verification:** `/+3V3` bulk unchanged or improved at FPC2; DRC 0/0; BOM shows
no `10uF` line; `bom_cpl_symmetry()` clean.

#### Package consolidation beyond this point is NOT recommended

The full census, by placement count:

    54  C0603      35  R0603      10  C0805      (everything else is 1-4)

The remaining 0805 population is 7× 22 µF bulk (C29, C30, C31, C48, C49, C66,
C68) and 2× 4.7 µF (C38, C39). Moving bulk to 0603 costs voltage rating and
DC-bias headroom for **no BOM-line saving** — they are already one line each.
Moving the 4.7 µF pair saves no line either.

The optimization target is **distinct (value, package, part) tuples**, not a
uniform package size. Before: four values carry duplicates (`100nF` ×2 parts,
`10kΩ` ×2 parts, `10uF` ×2 packages, the tact switch ×2 footprints). After U35 +
U36: only the tact switch remains, and it is one part number on two land
patterns — a tidiness item, not a cost item (see spin-2 list).

---

### U37. Courtyards on D10, D11, F1 — and stop ignoring the check

**Goal:** close the blind spot that hid the AW9523B overlap until 2026-07-31.

`missing_courtyard` is set to `ignore` in the `.kicad_pro`. Setting it to `error`
on a staged copy yields exactly 7 violations: **D10, D11, F1** plus the 4 unnamed
EasyEDA corner pads. Three of the seven new parts walked through the same hole the
last fix left open — all three came from `JLCImport.pretty`, none of which carries
an `F.CrtYd` item.

**No collision today** — measured by hand with exact polygon math: D11→C71
1.285 mm, D10→C70 3.045 mm, F1→SW2 2.205 mm (pad-to-courtyard). This is a
false-pass risk, not a present defect.

**Files:** `kicad/board3/JLCImport.pretty/1N5819WS.kicad_mod`,
`.../BSMD1812-200-30V.kicad_mod`, `kicad/board3/*.kicad_pro`, plus the placed
instances (KTD26: library-only fixes change nothing fabricated).

**Verification:** `missing_courtyard` at `error` yields only the 4 corner pads.

---

### U38. Netclass `via_drill` 0.3 → 0.25

Every via on the board is 0.25 mm drill (0.175 mm ring), but the Default netclass
still carries `via_drill: 0.30`. The next via drawn in the GUI arrives at 0.6/0.3
= **0.150 mm ring**, silently landing at JLC's absolute minimum and undoing
`f621c3b` at that location. Directly relevant because U31, U33 and U36 all involve
GUI work.

**Files:** `kicad/board3/*.kicad_pro`. **Verification:** place a test via, confirm
0.25 mm, delete it.

---

### U39. Correct four wrong numbers that are in merged documentation

Each was stated as fact and is not:

| Claim | Truth | Where it is wrong |
|---|---|---|
| Board is 230 × 50 mm | **250 × 50 mm** (Edge.Cuts measured) | session notes, review prompts |
| 198 vias | **205** (204 @ 0.6/0.25 + 1 @ 0.5/0.2) | PR #17, `kicad_rules.json` comment |
| "150-violation baseline" | **Not reproducible.** True delta is 0 → 0. Trajectory: `5e047a7` 0/0 → `3c9f8c1` 12/14 → `d08f29c` 0/9 → `262dc65` 0/0. The nearest real figure is the 146-*warning* inbox at `25193d2`, which predates the 7-part work. | PR #17 (merged), PR #19 |
| 0.25 mm drills incur a surcharge | **No surcharge applies.** JLC's rule is conditioned on via *diameter* < 0.45 mm; ours are 0.60 and 0.50. | `tools/kicad_rules.json` `cost_floors_mm`, PR #17 |

The last one matters most: `kicad_rules.json` models a flat drill floor, which is
more pessimistic than JLC's actual pricing, and **that wrong model drove a real
design decision** (the 0.7/0.35 → 0.6/0.25 change).

**Files:** `tools/kicad_rules.json`, `CLAUDE.md`, PR descriptions.

---

### U40. Delete the stale gerber decoy

`fab/board3-gerbers.zip` (Jul 27, gitignored) is the output of a `kicad_fab.py`
run under a bare `python`: no gerber renaming, so the board outline ships as
**`Multi-Layer`** — which in Altium/EasyEDA vocabulary means copper on every
layer, i.e. a fab reading that filename learns the opposite of the truth. It is
also seven parts stale, it sits next to the correct `fab/gerbers-jlcpcb.zip`, and
it has the more authoritative-sounding name. One `rm`.

---

### U42. Simplify CAN termination to one resistor per bus, jumper-selectable

**Goal:** replace the four-part split network with a two-part one, and make
termination an independent yes/no choice **per CAN bus**.

**What is there now**, verified through `pcbnew` — both buses identical:

    CAN1_H ──[ H1 ]── /CAN1_TERM ──[ R10 60.4Ω ]──┬── /CAN1_CT
                                                   │
                                              [ C57 4.7nF ]
                                                   │
                                                  GND
                                                   │
    CAN1_L ─────────────────[ R14 60.4Ω ]──────────┘

Split termination is the right *topology* — two halves with a mid-point cap damp
common-mode without loading differential. The defect is **which leg the jumper
interrupts**. H1 sits only in the `CAN1_H` branch, so with the jumper open:

- `CAN1_H` → open at H1, carries nothing
- `CAN1_L` → **still** 60.4 Ω + 4.7 nF to ground, permanently

That is an unbalanced AC load on one leg of a differential pair — τ = 284 ns
against a 1 µs bit time at 1 Mbps. Exactly what split termination exists to
avoid. `tools/kicad_rules.json` records the intended default as *un-terminated*
("the bus is terminated at its ends by the vehicle harness"), so the un-jumpered
state is the one that is wrong.

**Approach — series resistor behind the jumper:**

    CAN_H ──[ H1 pin1 ][ H1 pin2 ]──[ R 120Ω ]── CAN_L

Jumper **fitted** → a single 120 Ω across the pair, the standard CAN end
termination. Jumper **open** → the resistor is left dangling from one line with
its far end floating: a ~1 pF stub with no path to anywhere, instead of today's
permanent RC to ground. Each bus keeps its own header, so **CAN1 and CAN2 are
selected independently** — fit one, both, or neither.

**Parts, per bus:** `H1` + `R10` + `R14` + `C57` (4) → `H1` + one 120 Ω (2).

| | before | after |
|---|---|---|
| placements | 8 (both buses) | 4 |
| `C2933247` 60.4 Ω ×4, **Extended** | 1 line | removed |
| `C53987` 4.7 nF ×2, Basic (used *only* by C57/C58) | 1 line | removed |
| `C22787` 120 Ω 0603, **Basic + preferred** (no feeder fee), 3.08 M stock, $0.008 | — | 1 line |

Net **−1 BOM line and −1 extended-part fee**, on top of U35/U36. Running total:
48 → 46 (U35) → 45 (U36) → **44** (U42).

Power check: at 2 V differential, 120 Ω dissipates 33 mW against the 0603 part's
100 mW rating — 3× margin, and 0603 matches the board's dominant package.

**Files:** `kicad/board3/*.kicad_sch`, `kicad/board3/*.kicad_pcb`

**Approach note:** this needs a re-route of both CAN connector areas, so it is
the second-largest routing job in the plan after U33. Add the part with
`python tools/kicad_lcsc.py add C22787`, then `kicad_lcsc.py models`.

**Verification:** `/CAN1_H` and `/CAN1_L` each carry exactly the transceiver pin,
the connector pin, and one termination element; no net reaches `/GND` through a
termination part; DRC 0/0. Repeat for CAN2.

**Interim measure until this ships:** on the current board, **fit the H1/H3
jumpers**. That makes the existing network the symmetric split termination it was
drawn as, at the cost of 120 Ω of extra termination on a short bench bus — benign
at 1 Mbps. Put it on the bring-up card.

---

### U43. Raise four DRC guard rails that sit below the fab's actual limits

**Goal:** stop the rule set from being able to certify an unbuildable board.

Each of these is currently *satisfied by the geometry* — so this is not a defect
list, it is a list of checks that would not catch one. Measured against JLC's
live capability table:

| Rule | Configured | JLC actual | Board's real value |
|---|---|---|---|
| `m_SolderMaskMinWidth` | **0.0 mm** (disabled) | 0.10 mm green / 0.13 black-white | 0.1795 mm ✓ |
| `min_text_height` | 0.8 mm | **1.0 mm** | 1.0 / 1.27 mm ✓ |
| `min_text_thickness` | 0.08 mm | **0.15 mm** | 0.15 mm ✓ (exactly at limit) |
| `min_hole_clearance` | 0.1016 mm | 0.20 via→track, 0.28 PTH→track, 0.30 inner PTH→cu | 0.2730 / 0.4330 / 0.3021 ✓ |

Plus the `.kicad_dru` scopes edge clearance **below** the fab limit:

    (rule usbc_slot_edge
      (condition "A.intersectsArea('usbc_slots')")
      (constraint edge_clearance (min 0.1mm)))

0.1 mm is half of JLC's 0.20 mm copper-to-routed-slot minimum. The board sits at
0.2417 mm so it passes anyway — but that rule would certify a board that cannot
be built. Raise it to 0.20 mm and it still passes, while meaning something.

Because `solder_mask_bridge` is severity *error* while its width parameter is
0.0, it only ever catches actual aperture overlaps, never a sub-minimum dam.

**Files:** `kicad/board3/*.kicad_pro`, `kicad/board3/*.kicad_dru`

**Verification:** after raising all five, DRC still 0/0. If anything now fails,
that is a real finding the old rules were hiding — treat it as such, do not lower
the rule back.

**Related, and deliberately NOT changed here:** three features pass with
essentially no margin and should be watched rather than adjusted, since fixing
them costs re-routing:

- BTN1 via annular ring **0.1500 mm** — JLC's absolute minimum, recommended 0.20
- USBC1.1/2/3 hole → Inner2 `/+5V` zone **0.3021 mm** vs a 0.30 mm limit (2.1 µm)
- USBC1 pads → routed retention slot **0.2417 mm** vs 0.20 mm

---

### U44. Order-form facts that are not in any file

**Goal:** the quote form asks for things the gerbers do not state, and getting
them wrong causes a hold, a re-quote, or unfitted parts.

| Field | Answer | Why it is not obvious |
|---|---|---|
| Board size | **250 × 50 mm** | not 230 × 50 (U39) |
| Minimum hole size | **0.20 mm** | 204 vias are 0.25, but the single BTN1 via at 0.20 forces the lower tier. Declaring 0.25 is wrong. |
| Via covering | **Tented** | the board sets `m_TentViasFront/Back = True`; JLC's plugged-via service caps at 0.5 mm and ours are 0.6 mm, so tenting is what must be selected |
| Surface finish | **ENIG** | the stackup block specifies it and it is a real cost adder over HASL |
| Stackup | **JLC04161H-7628** | the literal string appears **nowhere** in the `.kicad_pcb` — only the numeric values match, so nothing binds the order to that template |
| Impedance control | **not ordered** | `dielectric_constraints no`. The USB pair is built to a 90 Ω target against the written stackup, but JLC will not verify it |
| Assembly service | **must include through-hole / hand-solder** | **12 of 143 designators are THT**: DC1, H1, H2, H3, P1, P2, SW1–SW4, SW6, SW7. Plain SMT assembly leaves the board with no power jack, no CAN terminals and no buttons. |

**Also time-sensitive, not a file fix:** **U1 (STM32H755ZIT6, `C730212`) is at 37
units** and is ~64% of the per-board component cost ($27.26 of ~$42.31). Its only
variant `C1343604` is at **0**. This is the identical pattern that hit U2, which
went 2,163 → 4 units in seven days. There is no substitution available — the
mitigation is order timing. Re-check stock immediately before placing the order,
and treat a drop toward zero as a reason to place it that day.

**Unverified, flagged rather than guessed:** JLC publishes no maximum hole aspect
ratio. This board is 6.4:1 (1.6 mm / 0.25 mm) and 8.0:1 at BTN1. Their published
0.15 mm multilayer minimum with 1.6 mm listed as standard *implies* ~10.7:1 is
supported, but that is an inference. Confirm at order if certainty is wanted.

---

## Key Technical Decisions

**KTD27. Verify by pin FUNCTION, never pin number.** U31's defect survived a
verification gate that asserted `/U11_RSTN` carries `IC.23 + R.2 + C.1 + D.1`.
Pin 1 *is* the cathode, so the assertion passed while the circuit was inverted —
and the plan's own test scenario had named that exact failure ("a reversed diode
turns the fix into a permanent RSTN clamp"). A membership check over pin numbers
cannot see orientation. Any polarised two-terminal part must be verified through
`pinfunction` from the exported netlist, or through the symbol's pin *names*.

**KTD28. The BOM's optimization target is distinct (value, package, part) tuples.**
Not a uniform package size. Duplicate tuples cost a BOM line and a feeder fee, and
same-Comment/different-part pairs are the family that broke the JLCPCB uploader on
2026-08-02. Consolidating package sizes for their own sake trades voltage rating
and DC-bias headroom for nothing.

**KTD29. A vendor upload verdict is a single-attempt observation.** During the
2026-08-02 order attempt, a byte-identical file pair failed and then passed on
immediate retry. Every earlier "fail" in that bisect had been recorded once and
treated as deterministic. Bisects against remote services need retries in the
protocol, or the bisect lies. This is why the `10uF` Comment collision remains
*plausible but unproven* rather than confirmed.

**KTD30. `.kicad_dru` is load-bearing and must be staged for any DRC.** Without it
the board reports 36 violations, all inside its three declared scopes (26
`courtyards_overlap`, 10 `copper_edge_clearance` at the USBC1 slots). No
over-broad suppression — verified by removing the `telltale_expander_cluster` rule
and getting exactly 1 violation back.

---

## Deferred to spin 2

Recorded so they are not lost, and explicitly **not** in scope here:

- **R4 = 0 Ω on `/VBUS_SENSE`** puts unlimited 5 V injection into U1 pin 98 during
  the buck's soft-start, while VDD is still rising. ST's limit is ±5 mA; the clamp
  sees tens of mA on every power-up. Also floats when USB is unplugged. One-part
  fix: make R4 the top of a 100 k/100 k divider.
- **`/VBUS` has no overcurrent protection.** The 2026-07-31 HIGH named *both*
  inputs; only the barrel got F1.
- **`/U12_RSTN` runs at 1.4 µm of clearance margin** against two telltale vias
  (0.1030 mm edge gap vs a 0.1016 mm rule) — the tightest copper on the board.
- **R61/D11 draw `/+5V` through the LED4 daisy chain.** One cold joint at LED4
  pad 2 orphans U12's reset pull-up, and the internal pull-down holds RSTN low:
  U12 in permanent reset, all four east telltales dark. The **west side home-runs
  to the plane and does not have this.** Home-run the east side too.
- **`fab/bom.csv`'s `Part Class` column is stale.** 8 lines blank — including
  every part this campaign touched (D10/D11, F1, U2) — and 1 wrong (`C98220`
  says Basic, JLC says Extended). Not an upload risk (`Part Class` is absent
  from `bom-jlcpcb.csv`) but it is the column a human quotes from, so a budget
  built from it understates the extended-part fees.
- **13 board 3D-model references point at `.wrl`, not `.step`** (F1, D10, D11,
  U11, U12, LED1, LED3–LED8) — every one has a `.step` sibling already on disk.
  `kicad_lcsc.py check` passes because the referenced file exists, but
  `kicad-cli pcb export step` ignores VRML, so an enclosure-fit STEP export
  silently omits those 13 bodies. Mechanical only, and a quiet drift from
  `kicad/README.md`'s stated `.step` convention.
- **LED3's value carries HTML-escape damage** — `HL-A-3528H343W-S1-13HL-HR3_SDCM_amp_lt_6_6000K-7000K`,
  where `_amp_lt_` is a surviving `&lt;`. Real MPN is
  `HL-A-3528H343W-S1-13HL-HR3(SDCM 6)(6000K-7000K)`. Cosmetic for ordering — the
  LCSC code picks the part — but that 62-character string is what the Component
  Marking Layer carries.
- **`--smd-only` is a CPL landmine.** 134 of 150 footprints carry no
  footprint-type attribute and `footprint_type_mismatch` is `ignore`. The shipped
  CPL is correct, but exporting with `--smd-only` (the commonly-cited JLCPCB
  recipe) yields a **13-part** CPL that still looks well-formed.
- **Ask for an AOI check on U12 P0_6.** The U12↔LED6 pad gap is 0.200 mm with
  zero mask expansion, and LED6's pad carries ~15× the paste volume of the QFN
  pin beside it. A bridge there lands `/+5V` on P0_6 — which is *unconnected*, so
  the board works perfectly with the defect present and no functional test finds
  it. +5 V is inside the part's 6 V pin rating, so it will not fail fast either.
- **CAN bus pins have no TVS.** `/CAN1_H`, `/CAN1_L`, `/CAN2_H`, `/CAN2_L` go
  straight to the harness. Defensible — the TJA1051T/3 carries ±8 kV IEC 61000-4-2
  and ±58 V bus-fault ratings, and KTD3 explicitly makes the car's 12 V front-end
  the production board's problem. Recorded so it is not lost when this moves into
  the car.
- **`/SWO` connects to exactly one pad (U1.130) and goes nowhere** — H2 is a
  5-pin SWD header with no SWO pin, so ITM trace is unavailable. Costs no parts.
- **24 of 32 expander ports are unused** — U11 and U12 each drive only P1_4–P1_7.
  The useful reading is positive: this board already has **24 spare POR-safe I/O
  on an existing bus**, so any spin-2 feature (backlight enable, more telltales,
  an encoder) belongs there rather than on a new IC or a new MCU GPIO.
- **CH224K (U4) is a 100 W PD controller hard-strapped to 5 V.** Safe as wired,
  but two 5.1 kΩ CC pull-downs do the same job: −2 placements, −1 extended line,
  −1 exposed-pad reflow risk.
- **Ten capacitors (C20–C28, C56) sit in empty board space** on a 7.62 mm grid with
  no active device within 9.8 mm — residue of the deleted ULN2803 block. Not
  removable without re-routing: every pad has two track endpoints.
- **LED3 has no printed polarity mark; LED5's two silk cues contradict each other.**
- **SW1–SW4 vs SW6/SW7 use two land patterns for one part number** (LS5.4 vs LS5.0).
- **U4's exposed pad is still 100% paste** — the fourth EP part; U23's windowing
  covered three.
- **LED2/6/7/8 have an unfilled 0.25 mm via inside the cathode pad's mask opening**
  — solder wicking, up to ~22% of one pad's deposit, asymmetric fillet.
- **Board shrink.** Gated on carrier CAD and measured FFC tail reach, not on the
  PCB editor. Worth ~$15–30 per 5-board run; not worth re-opening a verified
  layout for.

## Post-fab bench debt — Kevin initiates all bench operations

Brownout soak on the corrected RSTN network, backlight current measurement (the
number that would have sized F1 properly), SPI clock walk, I2C rise-time
confirmation against the computed 220–240 ns, and beeping DC1 pad 1 to the plug
tip before assuming the center-pin mapping.
