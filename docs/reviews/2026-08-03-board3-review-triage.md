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
13. **[UNVERIFIED] `+3V3` is 100% 10 mil against the layout guide's ≥30 mil.**
14. **[UNVERIFIED] TJA1051 VIO has no bypass within 23 mm.**
15. **[UNVERIFIED] NRST filtering and pull-up are 87–89 mm from the pin.**
16. **[UNVERIFIED] F1's derated hold current has ~1.1× margin** against the
    project's own worst case.
17. **[UNVERIFIED] CAN termination resistors are 0603/100 mW** — a CANH-to-battery
    short in a 12 V vehicle exceeds that.
18. **[UNVERIFIED] `SCLK_R` runs 32 mm parallel to `MISO_R` at 0.389 mm on
    Inner2.**
19. **[UNVERIFIED] U42 gave up split termination's common-mode damping.** True by
    construction — it was the point of the change. Judge whether the trade holds.
20. **[UNVERIFIED] Post-fuse power path necks to 0.62 mm for 7.3 mm** against
    F1's 3 A hold. (drc-routing)
21. **[UNVERIFIED] This branch cut the board's tightest clearance to 72 nm of
    margin.** (drc-routing) Worth finding which edit did it.
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
