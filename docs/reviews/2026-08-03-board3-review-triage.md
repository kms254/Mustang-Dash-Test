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

## 2. Fab package and docs — cheap, mostly self-inflicted, do before ordering

2. **[CONFIRMED] The gerber zip predates the board** and contains none of U45's
   16 silk labels. (fab-readiness) `fab/ORDER.md` points at it as the upload
   file. **Just regenerate under KiCad's own interpreter.**
3. **[UNVERIFIED ×2 lenses] `.gbrjob` references 11 filenames not in the zip.**
   (fab-readiness + dfm-fab — independent convergence, the signal that made the
   diode CRITICAL credible last time.) Likely fixed by (2).
4. **[UNVERIFIED] `fab/ORDER.md` omits copper weight** — a real order-form field
   that changes price and capability. (dfm-fab)
5. **[UNVERIFIED] `ORDER.md` says 13 THT components; the lens counted 12.**
   (dfm-fab) One of us is wrong — I measured 13, excluding H2 and MP1–MP4.
6. **[UNVERIFIED] H2 has two silk lines at 0.120 mm, below the 0.15 minimum.**
   (dfm-fab) In the Tag-Connect footprint this branch added.
7. **[UNVERIFIED] `kicad/README.md` still documents 7 bodiless exemptions** (now
   8) and a stale 3D-model directory. (fab-readiness)
8. **[UNVERIFIED] Drill map ships inside the order archive with no declared
   function.** (fab-readiness)

## 3. Procurement / order form

9. **[UNVERIFIED] U1 is at 25 units**, variant at 0. `ORDER.md` records 37 as of
   2026-08-02 — it is falling. (fab-readiness) **Check before ordering; this
   blocks the order regardless of the board.**
10. **[UNVERIFIED] No component-free rail on any edge; panelisation for assembly
    unaddressed.** (dfm-fab)

## 4. Board electrical — real engineering decisions, none blocking DRC

11. **[CONFIRMED → MEDIUM] Panel SPI series termination is at the connector,
    ~90 mm from the driver.** (adversarial-ee) Verifier reproduced the geometry
    (93.2 / 88.8 / 90.0 / 91.5 mm) and confirmed it contradicts
    `docs/hardware/board3-pcb-layout-guide.md:120-122`, but downgraded severity.
    Source-series termination only works at the driver. **Fixing means moving
    four resistors and re-routing.**
12. **[UNVERIFIED] U48 shipped half** — `/VBUS` still has no overcurrent
    protection and R4 has no divider lower leg. (adversarial-ee)
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

1. §2 (2)–(8) — bounded, mostly self-inflicted, no board risk
2. §3 (9) — check U1 stock; it can block the order outright
3. Decide (11), (12), (22) — the three with real electrical weight
4. Everything else: verify before acting, given the 6-of-6 refutation rate
