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
CRITICAL finding is installed backwards and does nothing.

This plan closes **every finding that can be closed before the board is made** —
not just the blockers. There is no spin 1 yet, so "defer to spin 2" would mean
deliberately fabricating known defects to save edits we are already making. Only
**two** findings are genuinely deferred, each blocked on an input that does not
exist: the board shrink (needs carrier CAD and measured FFC tail reach) and CAN
TVS (a production-board decision already recorded in KTD3).

**Done means:** the board is orderable with every closable finding closed,
verified by netlist and measurement rather than by inference, the BOM down from
48 lines to 44, and the fab package regenerated under KiCad's own interpreter.

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
- BOM at 44 lines, with zero same-Comment/different-part-number pairs
- `kicad-cli pcb export step` emits all 143 bodies (carrier CAD depends on it)
- No via centre inside any pad; no track-to-via edge gap below 0.12 mm
- Every finding from the 2026-08-02 review is closed, deferred with a stated
  blocking input, or declined with a stated reason — none silently dropped

---

## Sequencing

**U41 runs first and gates everything else.** The remaining units start from a
clean, merged `main` on a fresh branch — not from the current working tree.

    U41  ship what's already done  ──►  merged to main
                                          │
                                          ▼
                                   new branch: fix/board3-review-blockers
                                          │
      ┌───────────────────────────────────┼───────────────────────────────────┐
      ▼                                   ▼                                   ▼
  no GUI needed                  ONE GUI sync covers all              routing work
  U35  part-number merges        U31  diode reversal  ★               U33  power copper ★
  U38  netclass via_drill        U32  F1 resize       ★               U42  CAN termination
  U39  doc corrections           U34  C70/C71 → 1 µF                  U47  copper fixes
  U40  delete stale zip          U36  C67 → 0805
  U43  raise DRC guard rails     U46  footprint + metadata
                                 U48  R4 divider, VBUS OCP
      │                                   │                                   │
      └───────────────────────────────────┴───────────────────────────────────┘
                                          ▼
                            U45  silk: function labels, no refdes
                                 U49  SWO out to the debug header
                                 (last — U33/U42 move copper near connectors)
                                          ▼
                            U37  courtyards + un-ignore the check
                                          ▼
                    regenerate fab package → U44 order-form checklist → order

★ = the three fabrication blockers.

**The GUI sync is the expensive step, so batch it.** U31, U32, U34, U36, U46 and
U48 all need one `Update PCB from Schematic` pass — run it once, not six times.
That step is also the one nothing can verify automatically (KTD12) and the one
that silently set 16 DNP flags last time, so **re-run `bom_cpl_symmetry()` and a
DNP census immediately after it**.

U45 goes last on purpose: silk placed before U33 and U42 move copper near the
connectors would need redoing.

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

**U1 stock — risk reviewed and ACCEPTED (Kevin, 2026-08-02).** **U1
(STM32H755ZIT6, `C730212`) is at 37 units** and is ~64% of the per-board
component cost ($27.26 of ~$42.31); its only variant `C1343604` is at 0. This is
the pattern that hit U2, which went 2,163 → 4 units in seven days, and there is
no substitution available.

**Do not let this rush the plan.** The decision is that shipping a board with
three known blockers to beat a stock clock is the worse trade — a scrapped panel
costs more than a wait. If JLC's stock reaches zero before the work is done, the
fallback is to buy U1 elsewhere (DigiKey/Mouser carry it) and supply it as a
consigned part, or accept the lead time. Re-check stock at order time for
information, not as a gate.

**Unverified, flagged rather than guessed:** JLC publishes no maximum hole aspect
ratio. This board is 6.4:1 (1.6 mm / 0.25 mm) and 8.0:1 at BTN1. Their published
0.15 mm multilayer minimum with 1.6 mm listed as standard *implies* ~10.7:1 is
supported, but that is an inference. Confirm at order if certainty is wanted.

---

### U45. Silk carries function, not designators

**Goal:** stop printing reference designators, and label the things a person
actually has to identify with their hands.

**Current state**, measured through `pcbnew`: **8 of 146** designators print —
D10, D11, F1, U11, U12 and the three fiducials. The other 138 are hidden,
including all 64 capacitors, all 35 resistors, nine ICs, the eight telltales and
all six switches.

**Decision: hide the remaining five and keep silk for inputs and buttons only.**
The board file is authoritative and `kicad/board3/renders/` carries current
top/bottom/angled images regenerated on every board commit, so a designator is
always seconds away. Printing 138 of them would have been a real layout job on a
dense 250 × 50 mm board with silk clearance only just armed at 0.15 mm (U28) —
whereas this direction makes silk *sparser*.

**Hide (5):** D10, D11, F1, U11, U12.
**Fiducials:** FID1–3 keep their marks; the designator text can go too.

**Label (16) — and label them by FUNCTION, not designator.** For precisely the
parts worth marking, the refdes is the least informative string available:

| Part | Print this, not this |
|---|---|
| FPC1 / FPC2 / FPC3 | **CENTER** / **LEFT** / **RIGHT** |
| P1 / P2 | **CAN1** / **CAN2** |
| H1 / H3 | **TERM1** / **TERM2** |
| H2 | **SWD** |
| DC1 | **12V** (or the confirmed supply rating) |
| USBC1 | **USB** |
| SW1–SW4 | **BTN1**–**BTN4** |
| SW6 / SW7 | **RESET** / **BOOT** |

Plugging the centre panel into the wrong FPC is a real bench mistake, and
`FPC2` does not protect against it. `RESET` vs `BOOT` matters more than `SW6` vs
`SW7` when a board is in a half-assembled state and someone is holding it.

**Files:** `kicad/board3/*.kicad_pcb` — reference-text visibility is a board-side
property of the placed footprint, so this needs **no schematic sync** (the U10
`Value` precedent). Functional labels are new `gr_text` items on `F.SilkS`.

**Verification:** DRC 0/0 with `silk_over_copper` and `silk_edge_clearance`
armed — this is the check that matters, since 16 new text items near connectors
is exactly where silk-over-pad appears. Text ≥1.0 mm height and ≥0.15 mm
thickness (JLC's minimums, see U43). Re-render and eyeball
`kicad/board3/renders/top.png`.

**Execution note:** do this **after** U33 and U42, both of which move copper near
connectors. Silk placed first would need redoing.

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

### U46. Cheap fixes that need no copper — do them while the files are open

Every item here is a metadata or footprint edit. None touches a net. They were
initially filed as "spin 2", which was wrong: there is no spin 1 yet, so
deferring them means **deliberately fabricating a board with known defects** to
save edits we are already making.

- **U4's exposed pad is still 100% paste.** The fourth EP part; U23 windowed the
  other three. Same edit, already understood. EP-to-pin gap is 1.200 mm so
  bridging is not credible, but there is no reason to ship the inconsistency.
- **LED3 has no printed polarity mark**, and **LED5's two silk cues contradict
  each other** — its centre arrow points at pad 1 (cathode) while its corner
  triangle sits at the pad 2 corner. The U23 fix covered LED4/6/7 and stopped.
  A reversed telltale just stays dark, but this is a rework/inspection trap.
- **LED3's value carries HTML-escape damage**:
  `HL-A-3528H343W-S1-13HL-HR3_SDCM_amp_lt_6_6000K-7000K`, where `_amp_lt_` is a
  surviving `&lt;`. Real MPN is `HL-A-3528H343W-S1-13HL-HR3(SDCM 6)(6000K-7000K)`.
  That 62-character string is what the Component Marking Layer carries.
- **`fab/bom.csv`'s `Part Class` column is stale** — 8 lines blank (including
  every part this campaign touched) and 1 wrong (`C98220` claims Basic, JLC says
  Extended). It is the column a human quotes from.
- **13 3D-model references point at `.wrl` where a `.step` sibling exists on
  disk** (F1, D10, D11, U11, U12, LED1, LED3–LED8). `kicad_lcsc.py check` passes
  because the file exists, but `kicad-cli pcb export step` ignores VRML, so an
  enclosure-fit export silently omits 13 bodies — and the carrier CAD is what
  gates the board-shrink decision.
- **Set footprint-type attributes.** 134 of 150 footprints carry none and
  `footprint_type_mismatch` is `ignore`, so exporting with `--smd-only` (the
  commonly-cited JLCPCB recipe) yields a **13-part CPL that still looks
  well-formed**. Set the attributes and un-ignore the check.
- **SW1–SW4 and SW6/SW7 use two land patterns for one part number**
  (`LS5.4` vs `LS5.0`). At most one is dimensionally right.

**Verification:** DRC 0/0; `kicad_lcsc.py check` PASS; `kicad-cli pcb export step`
produces a model with all 143 bodies.

---

### U47. Copper fixes to make while U33 and U42 already have the router open

These need routing, which is exactly why they belong in this spin rather than the
next one — the alternative is opening the router twice.

- **Home-run the east `/+5V` feed.** R61 and D11 currently draw `/+5V`
  *through* the LED4 telltale daisy chain. Verified by real in-memory segment
  deletion: cutting either feed segment orphans `D11.2, LED4.2, R61.1`. One cold
  joint at LED4 pad 2 removes U12's reset pull-up, the internal 100 kΩ pull-down
  holds RSTN low, and **U12 sits in permanent reset — all four east telltales
  dark and the expander unresponsive on I2C.** The **west side already home-runs
  to the plane and does not have this**; make the east side match.
- **Relieve `/U12_RSTN`'s clearance.** It runs at **1.4 µm** of margin against
  two telltale vias (0.1030 mm edge gap vs a 0.1016 mm rule) — the tightest
  copper on the board, on a net added by the last campaign. An etch or
  registration excursion shorts reset to a telltale cathode, and U12 would then
  reset itself every time TT4 or TT8 illuminates.
- **Move the escape vias out of LED2/6/7/8's cathode pads.** Via centres sit
  *inside* pad 1 on four telltales. Tenting is irrelevant when the via is inside
  the pad's own mask aperture: solder wicks down the 0.25 mm barrel during
  reflow — up to ~22% of one pad's deposit — giving an asymmetric fillet and LED
  tilt on a part sitting behind a lens.
- **Give BTN1's via an annular ring with margin.** At 0.5/0.2 it has exactly
  **0.150 mm**, JLC's absolute minimum with zero headroom, and it is also the one
  via forcing the whole order into the 0.20 mm hole tier (U44). If neighbouring
  pads can be nudged to allow 0.6/0.25, both problems close together.

**Verification:** DRC 0/0 after `ZONE_FILLER.Fill()` then `BuildConnectivity()`;
re-run the segment-deletion test and confirm no LED cut orphans R61 or D11;
minimum track-to-via edge gap above 0.12 mm; no via centre inside any pad.

---

### U48. Two electrical fixes the review found outside the blocker set

- **R4 = 0 Ω puts unlimited 5 V injection into U1 pin 98 (PA9) on every
  power-up.** `/VBUS_SENSE` has exactly two pads: `R4.2` and `U1.98`. PA9 is 5 V
  tolerant, but ST's tolerance is conditional on VDD being present — and 3.3 V is
  *derived* from 5 V through U3, so during the buck's soft-start VBUS sits on PA9
  through 0 Ω while VDD is still rising. Current into the ESD clamp is limited
  only by the clamp itself (~10–50 Ω), i.e. **tens of mA against a ±5 mA limit,
  every single power-up**. Secondary: with no lower leg, PA9 floats when USB is
  unplugged and barrel-powered, so VBUS sensing reads indeterminate. Fix: make R4
  the top of a divider (100 k / 100 k). One added part.
- **`/VBUS` has no overcurrent protection.** The 2026-07-31 HIGH named *both*
  inputs; only the barrel got F1. Combined with U33's finding that `/VBUS` is
  10 mil copper, a board-side short downstream of the connector is cleared by the
  trace rather than by a protection device. Fit a PTC in the USB path as F1's
  counterpart.

**Verification:** `/VBUS_SENSE` carries R4.2, the new divider leg, and U1.98;
divider midpoint computes to ≤3.3 V at VBUS = 5.25 V. `/VBUS` has a series
protection element with exactly two pads on its input side.

---

### U49. Bring `/SWO` out to the debug header

**Goal:** give this board a diagnostic channel that does not collide with the
serial protocol.

**The gap.** `/SWO` is a **dangling net** — exactly one pad, `U1.130`, going
nowhere. H2 is a 5-pin header carrying `+3V3, SWDIO, SWCLK, NRST, GND`, so the
MCU's trace output has no destination and ITM is unavailable.

**Why this board needs it more than most.** The USB serial is not a debug
channel here, it is a *protocol* channel: `ok`/`err` acks are the only output
after boot, and both the `/dash` skill and the host tests depend on that
contract. So today there is **nowhere to print** — any `printf` added to diagnose
something breaks the contract the tooling relies on.

SWO is a separate one-way path out of the core carrying ITM (`printf` at
megabits), exception entry/exit trace, and PC sampling for profiling. ST-LINK
V2/V3 and J-Link all support it. On a board whose hard bring-up problems have
been timing and signal integrity — the 27 MHz wedge that dead-locked the
firmware in an unbounded busy-poll, the unresolved two-panel crosstalk, marginal
FFC contacts — exception trace and PC sampling are the right instruments, and
none of them cost a byte on the protocol port.

**Files:** `kicad/board3/*.kicad_sch`, `kicad/board3/*.kicad_pcb`

**Approach:** H2 `PZ254V-11-05P` → the 6-pin variant of the same family (H1/H3
already use `PZ254V-11-02P`, so this is a family the board and the BOM already
carry). Route `U1.130` to the new pin. Suggested order — keep the existing five
in place and append SWO — so any cable already made stays valid.

**Space check, measured:** H2 sits at (130.44, 115.59) rotated 90°, bbox
3.17 × 13.33 mm, so growth is ±1.27 mm along y. Nearest neighbours are lateral,
not in the growth path — C4 and C35 are ~6.5 mm away in x, H3 is clear in x
entirely. Re-verify during execution rather than trusting this.

**Two honest caveats:**

1. A 6-pin 0.1″ header is **not a plug-in standard** — the conventional choices
   are the 10-pin Cortex Debug connector or a 20-pin. It works fine with flying
   leads, which is how this bench already connects, but it is not a socket
   anything ships with. If a standard connector is wanted, decide that now
   rather than after copper exists.
2. **No firmware enables ITM today.** This buys capability that still has to be
   built on. It is cheap to add now and impossible to add after fabrication,
   which is the whole argument.

Add the part with `python tools/kicad_lcsc.py add C<n>` once the 6-pin variant's
LCSC code is confirmed, then `kicad_lcsc.py models`.

**Verification:** `/SWO` carries exactly two pads — `U1.130` and the new H2 pin.
DRC 0/0. `kicad_lcsc.py check` PASS. Header pin order documented on the bring-up
card, since the silk (U45) will label it `SWD` rather than enumerate pins.

---

## U47 outcome — one closed, three assessed and left

**Closed: the east `/+5V` home-run.** R61.1 and D11.1 each now have their own via
into the Inner2 plane. Proven with the same segment-deletion test that found the
defect: both cuts that previously orphaned `D11.2, LED4.2, R61.1` now yield zero
airwires, while a control cut on LED8's feed still yields one.

**BTN1's annular ring — NOT IMPROVABLE, measured.** The DFM lens suggested
"bump the pad to 0.6 and keep 0.175", which assumed room that does not exist.
The nearest other-net edge is U1.92's pad at 0.360 mm, capping via diameter at
**0.517 mm**:

| option | ring | verdict |
|---|---|---|
| current 0.5 / 0.2 | 0.1500 | at JLC's minimum, forces the 0.20 mm drill tier |
| 0.517 / 0.25 | **0.1335** | **below the 0.15 mm minimum — not allowed** |
| 0.517 / 0.2 | 0.1585 | +0.0085 mm, spends clearance margin for nothing |

So the existing 0.5/0.2 is already correct and `kicad_rules.json` documents it as
a deliberate exception. **U44's "declare 0.20 mm minimum hole" stands** — the
tier cannot be dropped without moving U1's escape.

**`/U12_RSTN` clearance — left as-is.** Tightest point is a 0.36 mm In2 segment
at x=233.28 against the `/TT8_LED_K` via: gap 0.1030 mm against a 0.1016 rule,
**margin +0.0014 mm**. It passes. Nudging it requires moving the adjacent
segments that share its endpoints — a re-route of working copper for 1.4 µm.

**Telltale cathode escape vias — deferred.** LED2.1, LED6.1, LED7.1 and LED8.1
each have a via centre *inside* the pad, so solder wicks down the barrel during
reflow (up to ~22% of one pad's deposit, asymmetric fillet, LED tilt behind a
lens). Clear relocation spots exist ~1.6 mm out on all four — but each via is an
*escape*, carrying an F.Cu stub to the pad and a track away on B.Cu or In1, so
moving one is a coordinated two-layer re-route, four times, in the densest part
of the board. Deferred deliberately: it is a MEDIUM assembly-yield item, and
geometric edits introduced defects twice during this session's execution (the
pin-1 circles, the first widening pass). Worth doing with a clear head.

## Open findings — surfaced during execution, not yet closed

**U3 and U4 pin-1 marker circles are clipped by solder mask.** Surfaced by U43:
arming `solder_mask_min_width` (which had been *absent*, therefore zero) took
DRC 0 → 2, both `silk_over_copper`.

    U3 (TPS563201) circle overlaps its /GND pad 1  by 0.1030 mm  (40% of stroke width)
    U4 (CH224K)    circle overlaps its /GND EP     by 0.0174 mm

The fab clips silk where it crosses a mask opening, so both pin-1 dots print
partially. **Warning severity — does not block fabrication.**

**A fix was attempted and reverted.** Recorded because the failure is more
useful than the attempt:

- The solve measured the circle against **the footprint's own pads**. That is the
  wrong model. Silk clipping is a *board-level* question, and arming
  `solder_mask_min_width` makes KiCad merge nearby mask openings into larger
  apertures and check the silk against those. Both circles were moved to a
  verified 0.15 mm clearance from every pad edge and **both warnings persisted
  unchanged**.
- Moving the library circle 0.004 mm and the instance 0.254 mm made them
  diverge, producing a `lib_footprint_mismatch` on U3 — the exact class
  documented in `docs/solutions/integration-issues/kicad-lib-footprint-mismatch-integer-nanometre-comparison.md`.
  A footprint edit must land in library *and* instance with identical numbers
  (KTD26), and "identical" means the same computed value, not the same intent.

**To close it properly**, someone needs a model of KiCad's merged-mask-aperture
geometry — the shape the check actually runs against — rather than pad
distances. Until then the rule stays armed and the two warnings stay visible,
per U43's own instruction: a failure the old rules were hiding is a real
finding, so do not lower the rule back.

## Genuinely deferred — and why

Unlike the list above, these cannot be closed in this spin. The distinction
matters: **"spin 2" is only a real bucket when something blocks the work now.**

- **Board shrink.** Gated on carrier CAD and measured FFC tail reach — inputs
  that do not exist yet. Worth ~$15–30 per 5-board run; not worth guessing at.
  U46's `.step` fix is a prerequisite, since the carrier CAD needs a complete
  3D export.
- **CAN bus TVS.** `/CAN1_H`, `/CAN1_L`, `/CAN2_H`, `/CAN2_L` reach the harness
  with no transient protection. Defensible and **already a recorded decision**:
  the TJA1051T/3 carries ±8 kV IEC 61000-4-2 and ±58 V bus-fault ratings, and
  KTD3 makes the car's 12 V front-end the production board's problem. This board
  is a bench article.
*(`/SWO` was listed here and has been promoted to U49 — the fix is a 5-pin →
6-pin header in a family the board already uses, plus one route, which is not a
blocking input at all.)*

## Declined, with reasons

Reviewed and rejected — recorded so they are not re-opened:

- **Removing the CH224K (U4).** Two 5.1 kΩ CC pull-downs would do the same job
  (−2 placements, −1 extended line, −1 exposed-pad reflow risk), and its 100 W
  capability is genuinely unusable here — no rail could accept 9 V. But it is
  *correctly and safely strapped* (CFG1 high forces 5 V regardless of CFG2/CFG3),
  and removing it means re-laying the entire power-entry corner, which is where
  U33's copper work already carries risk. Not worth compounding.
- **Removing the ten orphan capacitors** (C20–C28, C56) — a 7.62 mm grid in open
  board with no active device within 9.8 mm, residue of the deleted ULN2803
  block. Every pad has two track endpoints, so removal is a re-route of a working
  area to save ~$0.20 of parts. The `/+5V` daisy-chain hazard is exactly what
  makes this dangerous rather than easy.
- **Printing all 138 designators** — see U45. The board file plus
  `kicad/board3/renders/` make every designator recoverable in seconds; function
  labels on connectors and buttons are worth more than refdes on 64 capacitors.

## Order-time instructions, not board changes

- **Ask for an AOI check on U12 P0_6.** The U12↔LED6 pad gap is 0.200 mm with
  zero mask expansion, and LED6's pad carries ~15× the paste volume of the QFN
  pin beside it. A bridge there lands `/+5V` on P0_6 — which is **unconnected**,
  so the board works perfectly with the defect present, no functional test finds
  it, and 5 V is inside the part's 6 V pin rating so it will not fail fast.
- **Fit the H1/H3 jumpers** if U42 has not shipped, so the existing split
  termination is at least symmetric.

## Post-fab bench debt — Kevin initiates all bench operations

Brownout soak on the corrected RSTN network, backlight current measurement (the
number that would have sized F1 properly), SPI clock walk, I2C rise-time
confirmation against the computed 220–240 ns, and beeping DC1 pad 1 to the plug
tip before assuming the center-pin mapping.
