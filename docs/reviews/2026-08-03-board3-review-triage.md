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
10. **[CONFIRMED — order-form item, not a board defect] No component-free rail on
    any edge.** (dfm-fab) Measured courtyard-to-edge: **left −0.29 mm** (USBC1
    overhangs, as an edge connector must), **bottom 0.28 mm** (P2), **top and
    right 2.33 mm**. Three of four edges are populated, and nothing approaches
    the ~5 mm an assembly conveyor wants. At 250 × 50 mm expect JLC to add rails
    themselves; the risk is a quote-time hold, not a bad board. Recorded in
    `fab/ORDER.md` with the measured numbers so it is answered rather than
    discovered.

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
16. **[CONFIRMED — accepted for this revision, with a condition] F1's derated
    hold current has ~1.1× margin.** The reviewer's number was right. 1812L
    rerating, against a load built up from the rail rather than the guide's
    hand-wave (3 backlights ≤0.25 A = 0.75, buck input 0.59, telltales 0.16,
    two CAN transceivers 0.14, expanders 0.01 → **≈1.65 A worst case**):

    | ambient | 23 °C | 40 °C | 50 °C | 60 °C | 70 °C | 85 °C |
    |---|---|---|---|---|---|---|
    | hold | 3.00 A | 2.62 A | 2.43 A | 2.25 A | 2.00 A | 1.78 A |
    | vs 1.65 A | 1.82× | 1.59× | 1.47× | 1.36× | 1.21× | 1.08× |

    At 70 °C the 80%-of-hold rule for guaranteed non-trip gives 1.60 A, **under
    the 1.65 A load** — and 70 °C is the system's own ceiling anyway (the tact
    switches are −25/+70 °C).
    **Decision 2026-08-03 (Kevin): leave F1 as is.** The reasoning is not that
    the margin is fine — it is that this revision is the wrong place to spend it.
    - On the bench, where this board actually runs, ambient is 23–40 °C and the
      margin is **1.6–1.8×**. Ample.
    - **The 30 V rating is the forward-looking part.** Every same-footprint
      option above 3 A is **16 V max** (`C22374900` 4 A, `C7542974` 3.5 A), so
      raising hold current now means *dropping* voltage rating — backwards for a
      board whose successor takes automotive 12 V. Keeping 30 V in 1812 costs
      the amp; that is the trade, and it is the right way round here.
    - **The car version resizes F1 regardless.** At 12 V in, the same power is
      ~0.76 A, wanting a ~1.5 A part rather than 3 A.
    **What would reopen this:** running *this* board in a hot cabin. The 70 °C
    figure is not theoretical — if Board3 itself goes in the car for testing,
    a hot day with three backlights lit trips it, and the fix is `C22374900`
    (4 A, same land pattern, value + supplier metadata only, no GUI sync).
    *Also note for the 12 V design: 30 V is not enough for a real automotive
    input either. Load dump needs a proper front end (TVS + clamp); F1's rating
    must not be what carries it.* See the carrier plan's Outstanding Questions.
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

All five measured 2026-08-03.

23. **[CONFIRMED — probe point half FIXED, indicator half open]** (dft) Two
    claims in one entry, and they resolved differently.
    **The bare-copper +5 V probe point now exists** — U53 untented a `/+5V` via
    at (126.251, 107.730) with a local ground 5.03 mm away, plus `/+3V3`. See
    (24).
    **No power-on indicator: true, and DECLINED 2026-08-04 (Kevin).** All eight
    LEDs are telltales — each sits between `/+5V` and a `/TTn_LED_K` cathode
    driven by the expanders, so **nothing lights without firmware**. A board with
    a dead MCU, a wrong option byte or a stalled I2C bus looks exactly like an
    unpowered one. The TP1–TP3 probe points now make that diagnosable with a
    meter, which is most of the value an indicator would have added.
    **Do not re-raise this as an oversight — it was costed and turned down.** The
    design was worked out in full and is recorded here so nobody repeats it:

    | | part | already on board as | R | current | output |
    |---|---|---|---|---|---|
    | +5V | Emerald green `C516142` | LED1/LED2 | 1 kΩ `C21190` | 2.0 mA | ~130 mcd |
    | +3V3 | Orange `C5246349` | LED5 | 1 kΩ `C21190` | 1.3 mA | ~117 mcd |

    Anode on the rail, cathode through the resistor to ground — the resistor
    replacing the AW9523B's current sink. **Zero new BOM lines**: all four part
    numbers are already fitted elsewhere, so it would have added 4 designators
    and nothing else.
    The colour choice is the non-obvious part and is worth keeping. Vf rules most
    of the range out on a 3.3 V rail — green, blue and white are 3.2–3.4 V and
    simply will not light. Of the three that fit (orange 2.2 V, red 2.4 V, yellow
    2.4 V), **orange is the only one bright enough to match the green**: at 1.8 cd
    against red's 270 mcd and yellow's 210 mcd, it is the sole low-Vf part that
    does not look dim beside a 1.3 cd emerald green.
    Cost of doing it: new components and new nets, i.e. the one change shape that
    requires **KTD12 — a GUI *Update PCB from Schematic*** — plus placement and
    routing. That, not the parts, is why it was declined.
24. **[PARTLY REFUTED, remainder FIXED] "Zero test points and 222/222 vias
    tented — no bare ground reference for a scope probe."** (dft)
    **The ground half is wrong.** `MP1`–`MP4` are **5.00 × 5.00 mm bare-copper
    pads on `/GND`** at the four board corners — mask open, no paste. That is a
    better ground target than any test point would have been. What they are not
    is *near* anything: the closest is ~100 mm from the centre-panel SPI, which
    defeats the short-ground-spring requirement.
    **The signal half was right, and mattered more than filed.** There were no
    signal probe points at all, and the **U9 read-integrity soak** — the
    project's own acceptance gate for every `DASH_SPI_RUN_HZ` increase — is
    written against `TP1=SCLK, TP2=MOSI, TP3=MISO, TP4=CS, TP5=GND`. Its "scope
    TP1/TP3, ground on TP5" branch could not be run as written. (Those names are
    Board2's, whose buffered topology Board3 dropped.)
    **Fixed in `tools/handroutes/u53-power-probe-points.json`, with no new
    parts.** Vias already existed on both rails with a `/GND` via between them,
    so opening the mask over three of them buys it — no footprints, no BOM line,
    no CPL row, nothing for parity to disagree with:

    | | net | position | to ground |
    |---|---|---|---|
    | TP1 +5V | `/+5V` | 126.251, 107.730 | 5.03 mm |
    | TP2 +3V3 | `/+3V3` | 135.508, 110.254 | 5.07 mm |
    | TP3 GND | `/GND` | 130.442, 110.508 | — |

    These two rails are what answers the bring-up question. `/+5V` sits
    downstream of the ideal-diode ORing, so it reads live whether the board is
    fed from the barrel jack or USB — "input power reached the board". `/+3V3`
    is the buck's output — "U3 is actually switching". Together they separate a
    dead supply from a dead regulator from a dead MCU, which is precisely the
    split you cannot make by looking at an unlit board.
    All three are clear of silk and none sits under a component courtyard, so
    **the board stays at 0 violations** at `--severity-all`. A 0.6 mm via is a
    small target — fine tip and a sprung ground lead, not a clip; MP1–MP4 remain
    for a bigger ground.
    *An earlier revision of this fix probed the centre-panel SPI bus instead,
    reasoning from the U9 clock-walk procedure's TP1–TP5. That target was
    inferred rather than asked for, and it was wrong — no test pads on the screen
    bus. Retargeted to the rails 2026-08-03. Note the consequence: the U9 soak's
    "scope TP1/TP3, ground on TP5" branch is still not executable as written on
    Board3, and that remains open.*
25. **[REFUTED] "The Tag-Connect -NL has no legs, so SWD must be hand-held."**
    (dft + simplicity) The board is not the constraint. H2's footprint carries
    **3 NPTH holes at 0.9906 mm** — the locating holes a *legged* TC2030-IDC
    cable drops into. `-NL` describes which **cable** you buy, not what the land
    pattern supports; buy the legged variant and it is hands-free. Nothing to fix.
26. **[FIXED — by moving parts, not by finding space]** P1/P2 carried no
    CANH/CANL pin identification. (dft) The connectors were identified by *bus*
    (`CAN1`/`CAN2`, `TERM1`/`TERM2`) but nothing said which screw is H. CAN is
    differential, so a swap does not work and does not announce itself — it just
    looks like a dead bus.
    **There was nowhere to print.** P1/P2 were boxed in on all four sides:
    | side | obstruction | gap |
    |---|---|---|
    | above | H1/H3 termination jumper | **0.61 mm** |
    | left | resistors | **0.94 mm** |
    | right | next part | **0.97 mm** |
    | below | board edge | **0.28 mm** |
    against the ~1.4 mm a 1.0 mm character needs. Inside the courtyard it would
    print under the terminal-block body — invisible once assembled, and worse
    than nothing because the board would *look* labelled.
    **Fixed in `tools/handroutes/u56-can-pin-silk.json` by making the space.**
    H1/H3 move up 1.0 mm, opening the band from 0.61 → **1.61 mm**. There was
    ample room: 9.94 mm above H1 before U2, 13.33 mm above H3 before C38.
    Two things came with it that footprint-moving alone would have missed:
    - **TERM1/TERM2 had to move too.** They sat 1.28 mm above the jumpers they
      label, so the new courtyard would have overlapped the text.
    - **Four stubs had to be re-laid.** H1/H3's pads are 2.00 mm square, so a
      1.0 mm move lands the old track ends *exactly on the new pad edge* — a
      connection too marginal to leave to a DRC opinion. Each segment ending on
      a jumper pad was re-added with that endpoint at the new pad centre.
    Labels are at each pad's own x, so each reads directly above the screw it
    names. Content verified by **function**: P1.1/P2.1 are `/CAN1_H`,`/CAN2_H`,
    P1.2/P2.2 are `/CAN1_L`,`/CAN2_L` — then checked again from the opposite
    direction, nearest-pad-to-each-label, 4/4 agree.
    Verified: DRC 0/0/0, exactly the 4 jumper nets changed (segment and via
    counts unchanged), BOM 41 / CPL 140 symmetric. No schematic change, so no
    KTD12 sync.
27. **[FIXED] The eight telltales carry no TT1–TT8 identification.** (dft)
    Fixed in `tools/handroutes/u55-telltale-silk.json`, 1.0 mm text on a 0.15 mm
    stroke to match U45's sixteen labels.
    **The mapping was the whole job, and it is not the designator number:**

    | LED1 | LED2 | LED3 | LED4 | LED5 | LED6 | LED7 | LED8 |
    |---|---|---|---|---|---|---|---|
    | TT1 | **TT4** | **TT2** | **TT3** | TT5 | TT6 | TT7 | TT8 |

    Three of eight are permuted. Labelling `LED2` as "TT2" would have been wrong
    on three lamps — which is exactly how U45 put `CENTER` on the `LEFT`
    connector and became the last review's only CRITICAL. Every label was
    derived from the net the cathode actually lands on, then **checked again
    afterwards from the opposite direction**: for each placed label, the nearest
    LED was found and its `/TTn_LED_K` net compared — 8/8 agree.
    The physical interleave is why the labels earn their keep: the left cluster
    carries TT1, TT2, TT5, TT7 and the right carries TT3, TT4, TT6, TT8, so
    nobody could infer the numbering from the layout.
    TT6 and TT8 sit to the left and right of their LEDs rather than below,
    because U12's courtyard (y 97.36–102.64) occupies the space under the right
    cluster's bottom row. Both remain unambiguous.
    *`kicad_handroute.py` gained an `add_silk` operation for this — it had none,
    which is why U45's labels did not come from it. Also `any_layer_id()`, since
    the existing resolver only walked the copper stack.*

## 6. DFM margins — thin but legal

All seven measured 2026-08-03. Two refuted, one turned into a live order-form
constraint.

28. **[CONFIRMED figure, "zero headroom" WRONG — but it constrains the order]**
    Min solder-mask dam is **0.1795 mm** exactly as claimed (U1.129 ↔ C6.2, a
    decoupling cap against the QFP), with 51 dams under 0.20 mm. But **0 dams
    are under 0.10 mm**, and JLCPCB's green mask holds a 0.1 mm dam — so there
    is ~80 µm of headroom, not zero.
    **The real consequence is colour.** JLC's non-green masks need **0.25 mm**
    dams, which all 51 of these would violate. **Green is now a requirement,
    not a preference**, and `fab/ORDER.md` says so.
29. **[REFUTED] "Smallest annular ring sits exactly on the process minimum."**
    Measured: **0.1500 mm** on the single 0.50/0.20 BTN1 via, **0.1750 mm** on
    every other via. JLC's via annular-ring minimum is ~0.13 mm. Not on the
    limit — 15% and 35% above it.
30. **[CONFIRMED, fixed] H2's NPTH holes are distinguished only inside a merged
    Excellon file.** Real: H2 has 3 NPTH pads (0.9906 mm) and `kicad_fab.py`
    exported one merged `.drl`, where non-plated holes are separated by a
    comment rather than by being in their own file. A fab that reads the
    geometry and skips the comment plates them, and H2's whole purpose is that
    pogo pins touch **bare** copper. Fixed by exporting separate PTH/NPTH files.
31. **[REFUTED] "Two 0.75 mm internal cutouts milled into the board profile."**
    There are none. Edge.Cuts holds exactly 8 items — 4 lines and 4 corner arcs
    — i.e. a 250 × 50 mm rectangle with R2.5 corners, and nothing else. No
    internal cutout of any size exists.
32. **[CONFIRMED, accepted] No local fiducials on any 0.5 mm-pitch part.** Three
    global fiducials (FID1/FID2/FID3) are spread across the board; the nearest
    to U1 is FID2 at 20.8 mm. Local fiducials would improve placement accuracy
    on a 250 mm panel, but JLC places 0.5 mm-pitch QFPs on global fiducials
    routinely and U1 sits mid-board rather than at a corner. Accepted; revisit
    only if a build shows placement drift.
33. **[CONFIRMED, and worse than reported] THT switches sit beside 0603
    passives.** The review said 1.10–1.30 mm; measured courtyard-to-courtyard it
    is **0.45 mm** (SW1→R28, SW2→R29, SW3→R30, SW4→R31) and 0.65/0.66 mm for
    SW6/SW7→R1. JLC hand-solders THT, so an iron tip works within half a
    millimetre of a 0603. Accepted — moving them means re-routing the button
    block — but it belongs in the assembly notes.
34. **[CONFIRMED, already deliberate] A DRC severity is set to `ignore`.**
    `footprint_symbol_field_mismatch`, documented in CLAUDE.md with its
    reasoning: Board3's supplier metadata lives on the *symbol* by design, so
    mirroring it onto footprints creates a second copy that drifts, and it
    produced 13 `lib_footprint_mismatch` violations when tried. The half that
    catches real defects, `footprint_symbol_mismatch`, stays armed. The lens is
    right that it deserves visibility — this entry is that visibility.

## 7. Simplification candidates

35. **[REFUTED as stated; the optimisation behind it is real] "Four 60.4 Ω
    Extended parts with two now vestigial."** (simplicity) All four
    (R10/R14/R15/R24, `C2933247`, 60.4 Ω ±1%, Extended) are **in circuit**:
    R10+R14 in series make CAN1's 120.8 Ω, R15+R24 make CAN2's. **None is
    vestigial.** What is true is that U42's removal of the centre-tap caps left
    each pair electrically equivalent to one resistor, so 2×60.4 Ω could become
    1×120 Ω — 4 parts → 2.
    **Deliberately not doing it.** Collapsing the pair destroys the centre tap,
    which is the only thing that would let the split-termination cap come back if
    (19) is ever revisited. It also costs a topology change and therefore a KTD12
    GUI sync, to save two 0603s. Keeping the option is worth more.
36. **[FIXED] C41/C42 were Extended where a Basic part does the job.** They were
    `C527046` (YAGEO CC0603FRNPO9BN180, 18 pF ±1% NP0 0603, **Extended**). Now
    **`C1647`** — Samsung CL10C180JB8NNNC, 18 pF 50 V **C0G ±5%** 0603,
    **Basic**, 524k in stock at $0.0109. Extended BOM lines 26 → 25.
    Identical value, footprint and nets, so the board is untouched and there is
    no GUI sync (the U10 precedent).
    **The tolerance relaxation is the only real question, and it is fine by 45×.**
    ±1% → ±5% on an 18 pF pair moves the effective crystal load ±0.45 pF around
    CL = 12 pF. For a 25 MHz fundamental (C0 ≈ 3 pF, C1 ≈ 5 fF) that is
    **≈ ±5 ppm** of pull, against ±1 ppm for the ±1% part — so the total budget
    goes from about ±30 ppm (crystal ±10 initial, ±20 over temperature) to ±35.
    The tightest consumer on this board is CAN bit timing at roughly ±1580 ppm,
    and USB does not depend on HSE at all (DFU runs HSI48+CRS, AN2606 §52). The
    ±1% part was over-specified.
    `kicad_lcsc.py add C1647` brought the part in properly — symbol, footprint
    and STEP model in `JLCImport` — and the two instances' supplier fields were
    repointed to it.
    **One deliberate loose end.** C41/C42 still carry `lib_id`
    `ProPrj_New-easyedapro:CC0603FRNPO9BN180`; only their instance fields name
    the Samsung part. Fully repointing `lib_id` means hand-inserting a symbol
    definition into the schematic's 60k-line `lib_symbols` cache, which is
    s-expression surgery for no functional gain — the BOM is generated from
    instance properties and is correct. The on-disk library symbol was left
    describing the YAGEO part, because that is what it is. The residual hazard
    is that a GUI *Update Symbols from Library* would revert the fields; the
    canonical fix is a GUI *Change Symbol* on C41/C42, whenever the project is
    next open. Note the board already relies on this same instance-override
    pattern for SW1–SW4.
37. **[CONFIRMED — and mis-filed as "simplification"; it is a fit risk]** SW1–SW4
    and SW6/SW7 are the same part (`C5340169`, HX TS4538CJ) on **two different
    land patterns**, and the difference is not cosmetic:

    | | footprint | pad X offsets | lead span |
    |---|---|---|---|
    | SW1–SW4 | `…-LS5.4` | ±2.700 mm | 5.4 mm |
    | SW6, SW7 | `…-LS5.0` | ±2.500 mm | 5.0 mm |

    Both agree on the 3.00 mm pitch (pads at y ±1.500) and both use 1.0 mm
    drills. They disagree by **0.4 mm on lead span, 0.2 mm per side** — and the
    part has exactly one true lead span, so **one of these two has its holes in
    the wrong place**. With a ~0.8 mm lead in a 1.0 mm hole there is only ~0.1 mm
    of radial slack, so the wrong one needs the leads splayed or pinched to seat.
    These are hand-soldered, so it will probably go in — and probably not sit
    flat.
    **SETTLED FROM THE VENDOR DRAWING, and SW1–SW4 were the wrong ones.** The HX
    TS4538CJ datasheet's recommended PCB pattern is dimensioned **5 mm × 3 mm,
    holes ø1** — the LS5.0 footprint exactly. SW6/SW7 were right all along.
    Reading it needed a rasteriser: the drawing is vector, the only embedded
    image on the page is the vendor logo, and the callouts do not survive text
    extraction. `pip install pymupdf` renders it; poppler needs a sudo password.
    **It is a fit defect, not a margin.** The same drawing specifies the leads as
    **4-ø0.95 ±0.1 into ø1.0 holes** — 0.025 mm of radial slack at nominal and
    *none* at the +0.1 lead tolerance. There is nothing to absorb 0.2 mm per side
    with, so SW1–SW4 would not have seated without splaying the leads. (My
    earlier estimate of "probably still seats" assumed a 0.5–0.6 mm lead and was
    wrong by a factor of two.)
    **Root cause found:** SW1–SW4 were placed from a *different library symbol* —
    `4.5X4.5X4.5WATERPROOF TACT SWITCH DIP 260G`, whose 5.4 mm land pattern is
    presumably right for that part — and the value and supplier fields were then
    overridden to the TS4538CJ without the footprint following.
    **Fixed in `tools/handroutes/u54-button-land-pattern.json`.** All six buttons
    now measure 5.000 × 3.000 mm. Position, rotation, reference, value and
    attributes preserved; nets carried across by pad number. The schematic's
    Footprint field was updated in the same change — a board-only edit would have
    left `--schematic-parity` disagreeing, which is the only check that compares
    a footprint to its symbol.
    **Three things came free with it:**
    - **The BOM lost a line, 42 → 41.** The same LCSC part was being ordered as
      two lines (4 + 2) purely because of the footprint split. This is what
      `kicad_lcsc.py duplicates` was hinting at with "41 distinct LCSC codes, 42
      value+footprint keys"; it now reads 41 and 41.
    - **Item 33 relaxed.** Both pads move inward, so SW→R28–R31 courtyard
      clearance goes **0.45 → 0.65 mm**.
    - The `hanxia` / `hanxia(韩下)` manufacturer split that the merge exposed was
      normalised to the ASCII spelling, removing a cp1252 encoding hazard.
    Verified: DRC 0/0/0, ERC unchanged at 1127, nets unchanged, BOM/CPL
    symmetric at 41/140.
    *Also noted: `C5340169` is Extended with **6,046 in stock** and the board
    uses six — worth a glance before ordering.*

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
43. **[REFUTED — the scepticism was warranted] "Eight vias sit in pasted,
    mask-opened pads."** (dfm-fab) The count is right; the risk is not. Measured:
    8 vias land inside a top pad — **U4 ×5, Q2 ×2, U1 ×1** — and they split into
    two harmless groups.
    - **U4 ×5** have `paste = False`. They are the CH224K's exposed-pad thermal
      vias, and with no paste aperture over them there is nothing to wick.
    - **Q2 ×2 and U1 ×1** do have paste, which is the case that would matter —
      and all three are **tented**: U1.93 explicitly (`front/back = 1`), Q2.6 and
      Q2.7 by inheriting the board default, which is `m_TentViasFront/Back =
      True`. A tented barrel cannot wick.
    Exactly the set CLAUDE.md already documents as deliberate (U4's thermal vias,
    Q2's plane stitches, U1.93's exception). Consistent with 40/41 being refuted.
44. **[CONFIRMED as fact, accepted — consistent with 42] No refdes printed.**
    (dft) Measured: **0 of 148** footprints have a visible silk reference, though
    all 148 have their refdes *assigned* to the top silk layer — they are placed
    and hidden, not missing. That is what makes it deliberate rather than an
    oversight, which is what (42) concluded.
    Not an assembly risk: the assembler places from the CPL, which is symmetric
    at 140 rows. It is a bench-rework annoyance — you cannot identify R33 on the
    board by eye. Turning them on is not free either: 148 designators on a dense
    250 × 50 mm board would collide with pads and each other, and the board
    already carries the 16 *functional* silk labels from U45, which is the
    information a human actually needs. Accepted.

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
