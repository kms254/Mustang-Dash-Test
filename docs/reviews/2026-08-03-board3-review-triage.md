# Board3 seven-lens review — deduplicated worklist (2026-08-03)

Source: seven-lens adversarial review of `fix/board3-review-blockers` (PR #21),
15 agents, 0 errors. **52 raw findings → 44 distinct** after collapsing the same
issue reported by multiple lenses.

## Read this before using the list

**52 is not a defect count.** Only CRITICAL/HIGH were deduped and verified —
8 of 14 went to an adversarial verifier, and **6 of 6 HIGHs tested were refuted**
with reproduced measurements. On the only sample tested, roughly three quarters
did not survive contact with the board.

So: **§1 is proven, §2–§7 are claims.** Anything marked UNVERIFIED has not been
measured by a second party, and on this board unverified findings have a poor
record. Verify before acting.

**Coverage gap, stated plainly:** verification was capped at 8 findings, so 6
serious ones were never checked — including the CRITICAL. That is how it nearly
shipped.

Status key: `[FIXED]` `[CONFIRMED]` (survived refutation) `[REFUTED]`
`[UNVERIFIED]`. Origin lens in parentheses.

---

## 1. Done

1. **[FIXED] FPC silk labels CENTER/LEFT were on the wrong connectors.** (dft)
   CRITICAL. FPC1 carries `/SCLK_L` but read `CENTER`; FPC2 carries `/SCLK_C` but
   read `LEFT`. Would have caused the exact bench error U45 exists to prevent.
   Root cause was U45's own table in the plan, which was wrong; corrected in
   place. Fixed and re-verified against the netlist in `66a87b4`.

---

## 2. Fab package and docs — CLOSED 2026-08-03

All seven verified against the regenerated package. Five were real and are fixed;
one was already settled; one was wrong.

2. **[FIXED] The gerber zip predated the board** and contained none of U45's
   16 silk labels. (fab-readiness) Regenerated under KiCad's own interpreter.
   Note this recurs by design — the zip is a gitignored build artefact, so it is
   stale the moment copper moves. U50 restaled it; regenerated again.
3. **[FIXED] `.gbrjob` referenced 11 filenames not in the zip.**
   (fab-readiness + dfm-fab — independent convergence, the signal that made the
   diode CRITICAL credible last time.) Real: `kicad_fab.py` wrote the `.gbrjob`
   *before* canonicalising the layer names, so every reference pointed at a
   pre-rename filename. Fixed at source; the job file is now rewritten after the
   rename. Audited: 11 `FilesAttributes`, **0 missing from the zip**.
4. **[FIXED] `fab/ORDER.md` omitted copper weight.** (dfm-fab) Now states 1 oz
   (35 µm) outer and inner, with the reason it is not free to change.
5. **[SETTLED — 12] `ORDER.md` said 13 THT components; the lens counted 12.**
   (dfm-fab) The lens was right. `ORDER.md` lists 12 and names the two exclusions
   that look like they belong and do not (H2 is a cable, MP1–MP4 are board
   features), so the disagreement cannot recur silently.
6. **[REFUTED] "H2 has two silk lines at 0.120 mm, below the 0.15 minimum."**
   (dfm-fab) Measured in both the library footprint and the board instance
   (KTD26 — they can disagree): **both lines are 0.1500 mm**, exactly at the
   minimum, not 0.120. The figure was wrong. Consistent with DRC, which reports
   0 violations at `--severity-all`.
   *Adjacent, checked because the sweep surfaced it:* 25 silk shapes on the board
   report width 0.0000 mm — D10/D11, LED1–LED8, P1/P2, U11/U12. All 25 are
   **filled polygons** (pin-1 and polarity markers), where the outline width is
   never plotted. Not a defect, recorded so the next sweep does not re-raise it.
7. **[FIXED] `kicad/README.md` documented 7 bodiless exemptions** (now 8) and a
   stale 3D-model directory. (fab-readiness) Reads 8 with the breakdown, and the
   model path matches `EASYEDA_MODELS/`.
8. **[FIXED] Drill map shipped inside the order archive with no declared
   function.** (fab-readiness) True: `-drl_map.gbr` was the one file in the zip
   neither declared in the `.gbrjob` nor readable as fab input — the machine
   drills from the Excellon `.drl`. Now excluded from the upload zip and still
   generated into `fab/gerbers/` for humans. **Every file in the archive now has
   a declared function**: 11 declared gerbers, the job file, the drill.

## 3. Procurement / order form

9. **[UNVERIFIED] U1 is at 25 units**, variant at 0. `ORDER.md` records 37 as of
   2026-08-02 — it is falling. (fab-readiness) **Check before ordering; this
   blocks the order regardless of the board.**
10. **[UNVERIFIED] No component-free rail on any edge; panelisation for assembly
    unaddressed.** (dfm-fab)

## 4. Board electrical — real engineering decisions, none blocking DRC

11. **[FIXED] Panel SPI series termination was at the connector, ~90 mm from the
    driver.** (adversarial-ee) Verifier reproduced the geometry
    (93.2 / 88.8 / 90.0 / 91.5 mm) and confirmed it contradicts
    `docs/hardware/board3-pcb-layout-guide.md:120-122`, but downgraded severity.
    Re-measured independently and every figure reproduced to 0.01 mm. Two things
    the review missed: **R32/R35 (centre) were already compliant** at 10.21 and
    6.51 mm — FPC2 sits beside U1 — so only four of six were wrong; and the same
    "at the MCU pin" requirement is stated twice more in the carrier plan, so
    this was a layout deviation from an explicit spec, not a judgement call.
    Fixed in `tools/handroutes/u50-spi-series-termination-at-mcu.json`: driver
    stubs are now **2.45 / 5.75 / 4.80 / 6.08 mm**. No schematic change — the
    resistors stay between the same two nets — so no GUI sync (KTD12) was needed.
12. **[REFUTED → NOT-A-DEFECT] "U48 shipped half"** — `/VBUS` has no overcurrent
    protection and R4 has no divider lower leg. (adversarial-ee) Both
    *observations* are true; neither is a defect.
    - **PA9 is 5 V tolerant.** The STM32H7 pin table gives PA9 I/O structure
      `FT_u` (`FT` = 5 V tolerant I/O; `_u` = supplied by VDD33USB), additional
      function `OTG_FS_VBUS`. VDD33USB is connected — U1 pin 91 → `/+3V3`.
      Absolute max for FT pins is `Min(VDD,VDDA,VDD33USB,VBAT) + 4.0` = **7.3 V**
      against 5.0 V applied.
    - **Injection current is zero, not merely "within limits".** `IINJ(PIN)` is
      **−5/+0 mA** for FT pins, footnoted *"positive injection is not possible on
      these I/Os"* — an FT pin has no clamp diode to VDD, which is what makes it
      5 V tolerant. ST specifies FT_u leakage out to `VIN ≤ 5.5 V` (≤5 µA, i.e.
      ≤50 mV across R4), so this is a characterised operating condition.
    - R4 is therefore a belt-and-braces series limiter, exactly as U48 intended.
      Had PA9 been non-FT it would still have covered it: (5.0−3.6)/10k =
      **140 µA against 5 mA**, 36× margin.
    - `/VBUS` is a *negotiated* rail — R6/R7 feed a CH224K PD trigger (U4) whose
      CFG1 is tied high, selecting **5 V**. Even a hypothetical 20 V contract
      gives 1.64 mA through R4, still inside ±5 mA.
    - No OCP on `/VBUS` is true (F1 is only in the barrel path) and is normal for
      a USB-C **sink**: the source provides OCP by spec. Optional hardening, not
      a fix.
All nine measured 2026-08-03. **Every geometric figure in this section
reproduced** — unlike §8, these lenses were reading the board correctly. What
they were less reliable about is what the numbers mean.

13. **[CONFIRMED — violates the guide, inside the thermal target] `+3V3` is
    100% 10 mil against the layout guide's ≥30 mil.** True: **no `+3V3` pour
    exists** (the only pours are `/GND` ×3 and `/+5V` on Inner2), 216 of 221
    segments are 0.254 mm, and the widest conductor anywhere on the net is
    0.300 mm — so whatever the topology, the trunk is ≤12 mil. But at the
    guide's own 0.8 A that is **~8 °C rise**, inside the 10 °C design target,
    with 1.11× current margin. Rule violated, physics fine. 21 of those
    segments are on inner layers, where the same width is only 0.44 A.
    *(A graph walk was attempted to identify the trunk and produced nonsense —
    1 of 74 sink pads resolved as reachable, inflating every downstream count.
    The bound above is topology-free and does not depend on it.)*
14. **[CONFIRMED] TJA1051 VIO has no bypass within 23 mm.** Measured: U8.5 →
    nearest `/+3V3` cap **23.19 mm**, U9.5 → **26.95 mm**. The contrast is the
    tell — VCC on the same parts is decoupled at **4.71 mm** (C59) and
    **3.86 mm** (C60), so VIO was missed, not traded away. NXP asks for 100 nF
    at both. Fixing means two new parts, a schematic edit and a KTD12 GUI sync
    — the expensive shape of change, for a modest EMI/RXD-integrity gain.
15. **[FIXED, as far as the crystal allows] NRST filtering was 87–89 mm from the
    pin.** Measured from U1.27: **C46 88.97 mm**, R49 86.68 mm, SW6 81.54 mm.
    Violates layout guide §3 item 13 ("C46 at NRST") *and* ST's own recommended
    NRST protection. It matters because the H7's internal filter only rejects
    pulses under ~300 ns; a longer glitch on an 89 mm antenna in a car resets the
    dash.
    **C46 cannot reach the pin, and that is a placement conflict, not an
    oversight.** NRST is pin 27, between OSC_IN (25), OSC_OUT (26) and the
    crystal — and **X1's courtyard overlaps U1's** (161.748 vs U1's 162.095), so
    there is no channel east of pin 27 at all. The nearest free 0603 pocket is
    7.20 mm and reaching it means crossing the crystal courtyard. §3's own
    priority order ranks the crystal and VDDA filter *above* C46, so the layout
    followed the ranking and C46 lost.
    Moving X1 was costed and rejected: X1.1 is already **2.047 mm** from OSC_IN,
    and any displacement that frees pin 27 makes that worse while pushing X1
    into C9/C45/C8. Kevin's call, 2026-08-03: **leave X1 in the ideal location.**
    Fixed in `tools/handroutes/u52-nrst-filter-to-mcu.json`: C46 moves to the
    first clear spot outside U1's footprint, **88.97 → 29.89 mm** straight
    (35.1 mm routed), using the existing escape via so the board gains none.
    **R49 deliberately stays** — a 10 kΩ pull-up is a DC hold, not a filter; at
    DC it pulls the net high wherever it sits, and the transient path belongs to
    the capacitor.
    *Separate finding, not in the review:* `/OSC_OUT` routes **11.17 mm** to a
    pin 4.18 mm away, because XI and XO are diagonally opposite on this 4-pad
    package so only one can face U1. Rotating X1 just swaps which net is long.
    Fixing it needs a footprint with adjacent XI/XO — an open part decision.
16. **[NOT VERIFIED — needs the derating curve] F1's derated hold current has
    ~1.1× margin.** F1 is `1812L300_30GR`, 3 A hold at 23 °C, against a stated
    1.5–2 A worst case. The *direction* is real: PTC hold current derates
    steeply with ambient and an under-dash environment is not 23 °C. But the
    specific 1.1× was not reproduced — that needs the Littelfuse derating
    table, which was not fetched. Settled neither way.
17. **[CONFIRMED] CAN termination resistors are 0603/100 mW.** All four
    (R10/R14/R15/R24, 60.4 Ω) are `R0603`. Termination is R10+R14 in series
    (120.8 Ω) across CANH–CANL with the jumper in the CANH leg. A CANH-to-12 V
    short drives ~99 mA through the pair — **~1.19 W total, ~0.6 W each, about
    6× a 0603's rating**. Real for a vehicle, but only when the H1/H3 jumper is
    fitted, i.e. only when this board is a bus end.
18. **[CONFIRMED exactly] `SCLK_R` runs 32 mm parallel to `MISO_R` at 0.389 mm
    on Inner2.** Measured **32.74 mm at 0.389 mm edge-to-edge** — exact. A
    second corridor of **54.73 mm at 0.557 mm** also exists, so if anything the
    finding understates it. Aggressor and victim are the same bus, and SPI
    samples MISO on the SCLK edge, which is when crosstalk peaks. Unchanged by
    U50, which re-laid `/SCLK_R` on its original line.
19. **[TRUE BY CONSTRUCTION] U42 gave up split termination's common-mode
    damping.** Verified in the netlist: `/CAN1_CT` and `/CAN2_CT` carry only the
    two resistors — no capacitor to ground. Each pair is now electrically one
    120.8 Ω differential terminator with a floating centre tap: correct
    differential termination, no common-mode damping. It was the point of the
    change; the trade is a judgement, not a defect.
20. **[FIXED] Post-fuse power path necks to 0.62 mm for 7.3 mm.** (drc-routing)
    Geometry reproduced exactly, and a trunk walk confirmed it load-bearing
    (blocking it disconnects Q2's sources from F1 — the whole rail). Widened in
    `tools/handroutes/u51-plus5v-barrel-neck.json`: 1.524 mm for the first
    3.00 mm, 0.900 mm for the rest. **15 °C → 8 °C at the guide's 2 A.** It
    cannot be 60 mil throughout: Q2's gate pad is 0.596 mm from the centreline,
    capping a uniform width at 0.989 mm.
21. **[CONFIRMED exactly — informational] The board's tightest clearance is
    72 nm of margin.** (drc-routing) Swept every track/via pair on every layer:
    minimum gap **0.101672 mm** against the 0.1016 rule = **+72 nm**, between
    `/+5V_BARREL` and `/GATE_BARREL` near Q2. It passes, and it is margin
    against *our own* rule — JLCPCB's capability is ~0.0889 mm, so real fab
    margin is ~12.8 µm. Not a defect; worth knowing that pair has no room left
    for a future edit. U51 widened a **different** `/+5V_BARREL` segment.
22. **[REFUTED → LOW] Barrel input has no OVP and silk says only "DC IN".**
    (adversarial-ee + dft, same issue twice.) Verifier kept the observation and
    demolished the severity. **Still worth marking the voltage** — it was left
    off deliberately because nothing in the repo confirms the rating. *Confirm it.*

## 5. Testability

23. **[UNVERIFIED] No power-on indicator and no bare-copper +5 V probe point.**
    (dft) A powered healthy board looks identical to a dead one.
24. **[UNVERIFIED] Zero test points and 222/222 vias tented** — no bare ground
    reference for a scope probe. (dft)
25. **[UNVERIFIED ×2 lenses] The Tag-Connect -NL has no legs**, so SWD must be
    hand-held for a whole session; U49 removed the previous header. (dft +
    simplicity.) A real ergonomics regression — weigh against the space it saved.
26. **[UNVERIFIED] P1/P2 carry no CANH/CANL pin identification.** (dft)
27. **[UNVERIFIED] The eight telltales carry no TT1–TT8 identification.** (dft)

## 6. DFM margins — thin but legal

28. **[UNVERIFIED] Solder mask webs have zero headroom** (min dam 0.1795 mm).
29. **[UNVERIFIED] Smallest annular ring sits exactly on the process minimum.**
30. **[UNVERIFIED] H2's NPTH holes are marked non-plated only by an Excellon
    comment.**
31. **[UNVERIFIED] Two 0.75 mm internal cutouts** milled into the board profile.
32. **[UNVERIFIED] No local fiducials on any 0.5 mm-pitch part** on a 250 mm board.
33. **[UNVERIFIED] Six through-hole tact switches have 0603 passives 1.10–1.30 mm
    away.** (dfa)
34. **[UNVERIFIED] A DRC severity was set to `ignore` in the same branch that
    armed the guard rails.** (drc-routing) That is
    `footprint_symbol_field_mismatch`, deliberate and documented in CLAUDE.md —
    but the lens is right that it deserves visibility.

## 7. Simplification candidates

35. **[UNVERIFIED] CAN termination is still four 60.4 Ω Extended parts with two
    now vestigial.** (simplicity) This is the U42 consolidation deliberately not
    done — the defect is closed, the cost optimisation is not.
36. **[UNVERIFIED] C41/C42 are Extended ±1% 18 pF where a Basic+preferred part is
    identical.** (simplicity)
37. **[UNVERIFIED] SW1–SW4 and SW6/SW7 are one part on two land patterns.**

## 8. Refuted — do not act on these

38. **[REFUTED → NOT-A-DEFECT] U34's 1 µF RSTN caps ate the AW9523B 5 ms budget.**
    Circuit analysis reproduced exactly; the *impact* claim was wrong, and the
    impact was what made it HIGH.
39. **[REFUTED → NOT-A-DEFECT] Eight telltales are six part numbers in one
    package.** Every sub-claim reproduced; premise and conclusion both wrong.
40. **[REFUTED → LOW] Six undocumented via-in-pad instances remain.** Geometry
    reproduced by exhaustive circle-vs-polygon audit at 4096 segments, then the
    classification, mechanism and severity all disproved.
41. **[REFUTED → LOW] The three deliberate via-in-pad groups are not
    assembly-safe.** Same treatment.
42. **[REFUTED → LOW] No reference designator reaches the assembler.** Found ~98%
    pre-existing and deliberate, and mischaracterised on three counts.
43. **[UNVERIFIED, overlaps 40/41] Eight vias sit in pasted, mask-opened pads.**
    (dfm-fab) Same theme as two findings *refuted* on measurement — treat with
    corresponding scepticism.
44. **[UNVERIFIED, overlaps 42] No refdes printed and no fab-package map.** (dft)

---

## What the board got right

Worth recording, because a list of 44 reads worse than the board is. An
independent nanometre-level sweep — segment/segment, point/segment, polygonised
pads, vias as circles on every layer they span — found **0 clearance violations
across 2760 tracks, 222 vias and 773 pad outlines**, 0 hole-to-hole under
0.5 mm, and 0 hole-to-copper under 0.2 mm. The DRC 0/0/0/0 claim survived, and
the delta against the merge base is 0 → 0 *against a stricter rule set*.

D10/D11 — the previous review's CRITICAL — verified correct three independent
ways by pin **function**. All four exposed pads are IPC-7093 compliant. All eight
LED polarities verified by function and by silk. The fiducials are textbook.
`kicad_lcsc.py check` and `duplicates` both pass, and a regenerated BOM/CPL is
byte-identical to the tracked files.

## Suggested order of work

1. ~~§2 (2)–(8)~~ — **closed 2026-08-03.** Five fixed, one already settled, one
   refuted (H2's silk is 0.15 mm, not 0.120).
2. §3 (9) — check U1 stock; it can block the order outright
3. ~~Decide (11), (12), (22)~~ — **all three closed.** (22) fixed 2026-08-03
   (`5V ONLY`); (12) refuted on the datasheet, no copper; (11) fixed 2026-08-03
   by moving R33/R34/R36/R37 to their MCU pins.
4. Everything else: verify before acting, given the 6-of-6 refutation rate

**Running score on this list: of the 4 serious items actually measured against
the board, 3 were refuted and 1 held.** Items 11 and 12 came from the same lens
(adversarial-ee) and the same unit (U48/U8 layout); one was a real deviation from
an explicit written spec, the other dissolved on one line of a datasheet. The
preamble's warning holds — but note it cuts both ways: (11) was *downgraded* by
its verifier and was nonetheless the one worth fixing.
