# Board3 — order form answers

The JLCPCB quote form asks for things the gerbers do not state. Getting them
wrong causes a hold, a re-quote, or a board that arrives with parts missing.
This file sits beside the upload files because that is where you will be
standing when the form asks.

**Every value below was measured from the board on 2026-08-03, not copied from a
plan.** Where a fact is not in the board file at all, that is called out — those
are the ones nothing will catch for you.

## Fabrication

| Field | Answer | Why it is not obvious |
|---|---|---|
| **INPUT VOLTAGE** | **5 V ONLY at the barrel jack** | There is NO regulator between DC1 and the 5 V rail: DC1 -> F1 (PTC) -> Q2 (reverse-polarity FET) -> `/+5V`. U3 is the 5 V->3.3 V buck, downstream. A 12 V supply puts 12 V straight onto U8/U9, U11/U12 (6 V abs max), all eight LEDs and pins 17/18 of all three panel connectors. The board is silked `5V ONLY`. |
| Board size | **250 × 50 mm** | The routed outline is exactly 250.000 × 50.000; 250.254 × 50.254 is the bounding box **including the 0.254 mm Edge.Cuts line width** — harmless at quote, wrong if anyone computes panel utilisation from it. Not 230 × 50 — that figure circulated in session notes and review prompts and is wrong (U39). |
| Layers | **4** | |
| Thickness | **1.6 mm** | |
| Copper weight | **1 oz (35 µm) OUTER, 0.5 oz (17.5 µm) INNER** | Read this off the board, do not assume it is uniform. The stackup block gives F.Cu and B.Cu `(thickness 0.035)` and **In1.Cu and In2.Cu `(thickness 0.0175)`** — confirmed independently in the generated `.gbrjob`. This row said "1 oz, outer and inner" until 2026-08-04, which is wrong for half the stack: answering 1 oz inner moves JLC onto a different build with different dielectric thicknesses, which costs more **and** invalidates the 90 Ω USB pair geometry two rows down (computed against 0.2104 mm F.Cu→In1 prepreg). The design rules here (0.1016 mm min track/clearance) are JLC's standard limits; 2 oz would relax nothing and cost more. |
| Surface finish | **ENIG** | `(copper_finish "ENIG")` is in the board's stackup block. A real cost adder over HASL, so it will not be chosen for you. |
| Solder mask colour | **GREEN — required, not a preference** | The narrowest pad-to-pad mask dam is **0.180 mm** (U1.129 ↔ C6.2), with **42–47 dams under 0.20 mm** and **320+ under 0.25 mm** (two independent measurements agree on the narrowest and differ on the count by method; the earlier "51" here matched neither). The decisive case is intra-footprint, which none of those counts include: **U1's LQFP-144 adjacent-pin dam is 0.220 mm**, ×143 on that one part, plus U11/U12 at 0.212 mm and USBC1 at 0.200 mm. Green holds a 0.1 mm dam, so those pass with ~80 µm to spare. **JLC's other colours need 0.25 mm**, which every one of those — all 320+ — would violate — the mask would simply not print between those pads, next to a 0.5 mm-pitch QFP. Picking a colour for looks is a real defect here. |
| Minimum hole size | **0.20 mm** | Every via but one drills 0.25; the single BTN1 via at U1.93 is 0.5/0.2 and forces the lower tier. Declaring 0.25 is wrong. Stated as a rule rather than a count — the count moves with every routing change (U50 took it 222 → 234), and a stale number here is worse than none. |
| Via covering | **Tented** | The board sets `m_TentViasFront` and `m_TentViasBack` true. JLC's *plugged*-via service caps at 0.5 mm and ours are 0.6 mm, so tenting is the only option that matches. **Three vias are deliberately NOT tented** — the TP1 `/+5V` (126.251, 107.730), TP2 `/+3V3` (**121.314, 99.332** — moved by U59) and TP3 `/GND` (130.442, 110.508) probe points (U53), so you can confirm the rails on a board that looks dead. They carry an explicit not-tented override, so they survive the board-level "tented" answer; do not let anyone "fix" them back. **TP2 was at (135.508, 110.254) until 2026-08-04 and had to move**: it sat 0.0295 mm from C34's pad, below the 0.100 mm minimum mask width, so the two apertures merged into one opening in the exported gerber — a solder-theft path into an MCU rail decoupler. Both are `/+3V3`, and `solder_mask_bridge` only fires between *different* nets, so DRC could not see it. Check the mask dam, not just the DRC result, before untenting anything. |
| Impedance control | **Not ordered** | `(dielectric_constraints no)`. The USB pair is built to a 90 Ω target against the stackup below, but JLC will not verify it and is not being asked to. |
| Stackup | **JLC04161H-7628** | **This string appears nowhere in the `.kicad_pcb`** — only the numeric values match the template. Nothing binds the order to it, so it has to be selected by hand. |

## Assembly

**Plain SMT assembly is not sufficient. The service must include through-hole /
hand-soldering**, or the board arrives with no power input, no CAN terminals and
no buttons.

**12 components have plated through-holes:**

    DC1                     power jack
    P1, P2                  CAN terminals
    H1, H3                  CAN termination jumpers
    SW1, SW2, SW3, SW4      buttons
    SW6, SW7                reset, boot
    USBC1                   mid-mount USB-C (through-hole legs)

Two exclusions worth stating, because both look like they belong on that list
and do not:

- **H2 is not on it.** The Tag-Connect TC2030-IDC-NL is a *cable*, not a fitted
  part — its three holes are NPTH alignment holes and its six pads are bare
  copper for pogo pins. Nothing is soldered. It is also `in_bom no` and
  `in_pos_files no`, so it appears in neither the BOM nor the CPL.
- **MP1–MP4 are not on it.** They are the four EasyEDA corner mounting pads —
  plated holes, but board features rather than components, and excluded from BOM
  and CPL by attribute.

## Known risk at order time

**U1 (STM32H755ZIT6, `C730212`) stock — RE-CHECKED 2026-08-03: 25 units.**
It was 37 on 2026-08-02. Down a third in a day, and this is the part that gates
the order: ~64% of per-board component cost ($27.26 of ~$42.31), Extended
(feeder fee), and its only variant `C1343604` is at 0.

**There is no drop-in substitute.** Checked 2026-08-03 against the same package
and subcategory: the single close match is `C730144` STM32H723ZET6 (3259 in
stock, $12.43) and it is **not a substitution** — single-core Cortex-M7 against
the H755's dual-core M7+M4, 512 KB flash against 2 MB, different I/O count.
Adopting it is a redesign, not a swap.

This is the pattern that hit U2, which went 2,163 → 4 units in seven days.
**Check stock immediately before ordering.** If it has gone, the order is blocked
regardless of everything above, and the decision becomes buy-elsewhere-and-consign
or wait.

**Panelisation.** No component-free rail is provided on any board edge and none
is designed in. Measured courtyard-to-edge clearance: **left −0.29 mm** (USBC1
deliberately overhangs, as an edge connector must), **bottom 0.28 mm** (P2),
**top and right 2.33 mm**. Assembly lines generally want ≥5 mm on two opposite
edges for the conveyor, so at 250 × 50 mm — long, narrow, and populated to three
of four edges — expect JLC to add rails themselves. Worth confirming at quote
rather than discovering it as a hold. Nothing in the board files declares an
intent either way.

**Drill files.** The package ships **separate PTH and NPTH Excellon files**, not
one merged file. That is deliberate: H2's three 0.9906 mm holes are non-plated,
and in a merged file the only thing marking them so is a comment. H2 is a
Tag-Connect land pattern whose pogo pins press on **bare copper** — plating those
holes is not a cosmetic error. Do not "helpfully" merge them when uploading.

## Board state

As of 2026-08-07, after the seven-lens pre-order review (all seven verdicts
ORDER) and the one copper change it produced:

- **`/VBUS` input trunk reinforced.** The full USB input current crossed a
  single 0.400 mm × 3.6 mm Top segment — measured by max-flow over the copper
  graph, closing triage item 50 — and the LM74700 ORing pair means a slightly
  higher USB source carries the whole load even with the barrel plugged in.
  A 1.0 mm parallel same-net conductor now overlaps it (merged, ~1.4 mm
  combined); max-flow went **0.400 → 0.650 mm-equivalent**, worst-case rise at
  the documented 1.65 A from **~+19 °C → ~+9 °C**, at parity with the barrel
  side's accepted 0.900 mm / +5 °C point. The min-cut now sits on the 0.650 mm
  vertical run to Q1 — so the layout guide's literal "≥60 mil input trunk" row
  is still not met on the USB path, **deliberately**: meeting it would re-lay
  that run plus both upstream branches, and +9 °C at worst case does not buy
  that risk. Recorded here so the rule row and the board stop disagreeing
  silently.
- **U2 (W25Q256 QSPI NOR) stays fitted, conditionally.** No firmware consumes
  it yet; the decision to keep it (vs DNP, ~$23/run) was made 2026-08-07 **on
  the condition that the JEDEC-ID smoke test on the bring-up card runs on the
  first board** — until it passes, a dead U2 is invisible.
- **Bench prerequisite:** the order has H1/H3 headers but **no 2.54 mm
  shunts**, and CAN bring-up needs H1 closed on day one. Shunts in the parts
  order or confirmed in the drawer before boards arrive.
- Bring-up procedure, TP map and debt list: `docs/hardware/board3-bringup-card.md`.

As of 2026-08-05, after the silkscreen campaign:

- DRC **0 errors, 0 warnings, 0 unconnected, 0 schematic parity** (run in place,
  `--severity-all --schematic-parity`) — and now **with a silkscreen rule that
  actually runs**. `min_silk_clearance` in the `.kicad_pro` plus three armed
  `silk_*` severities were enforcing nothing; the check exists only once a
  `silk_clearance` rule is in the `.kicad_dru`. Silk-to-pad went **233
  violations → 6**, all six scoped exceptions carrying their measured worst
  (D10/D11's cathode bands, R9 against U1.52)
- BOM 41 lines / CPL 142 rows, symmetric — the tool is the authority on these
  numbers, not this file; it read 42 here from 2026-08-03 while the committed
  BOM was already 41
- Every via-in-pad remaining is deliberate (U4's exposed-pad thermal vias, Q2's
  plane stitches, U1.93's documented exception)
- Narrowest solder-mask dam **0.180 mm**, narrowest different-net copper
  clearance **0.101672 mm** (pre-existing, `/+5V_BARREL` ↔ `/GATE_BARREL`), and
  no untented via closer than **0.565 mm** to a pad aperture. DRC alone does not
  cover the first or third of those — see the Via covering row.

**Re-upload the gerbers AND the CPL if you are ordering after 2026-08-10.**
P1/P2 (the CAN screw terminals) were rotated 180° so the wire mouths face the
bottom board edge — caught in JLCPCB's 3D placement viewer during the first
order, where both JLC's model and KiCad's showed the mouths facing into the
board. The change is **electrically null** (the pins sit on the rotation axis,
so each hole keeps its net — `/CANx_H` stays on the west hole; copper is
untouched, airwires 0, netlist diff is exactly the four pin-membership swaps)
but it changes two shipped artifacts: the **CPL** (P1/P2 rotation 0 → 180, which
is what tells the hand-solder crew which way the body faces) and
**`F_Silkscreen.gbr`** (the body outline flipped with the part and is clipped
0.15 mm short of the board edge). A pre-2026-08-10 CPL orders boards whose CAN
wires point at the LQFP. Do not "fix" this by rotating in JLC's own editor —
their placement UI has no rotate control (verified 2026-08-10).

**Re-upload the gerbers if you are ordering after 2026-08-07.** The `/VBUS`
reinforcement is a **copper change** (one new F.Cu conductor plus the refilled
Top GND pour around it), so any zip built before 2026-08-07 describes a board
with the old 0.400 mm neck. BOM/CPL are untouched by it. The earlier
re-upload rules below still apply to older zips.

**Re-upload the gerbers if you are ordering after 2026-08-05.** The silkscreen
campaign rewrote `F_Silkscreen.gbr` across 160 shapes — 50 narrowed, 88 trimmed,
22 removed — so any zip built before 2026-08-05 prints markings this board no
longer draws. **Copper, drills, mask, paste and the BOM/CPL are untouched by
it** (150 footprints, 229 nets, 2780 tracks, 238 vias, 660 pads, identical), so
this one is a silkscreen-only re-upload. Everything below still applies to any
zip older than 2026-08-04.

U50 moved the
four panel-SPI series resistors (R33/R34/R36/R37) to their MCU pins and re-cut
their nets, so any zip built before that describes different copper. The CPL
changed too — those four parts moved and their rotations went 90° → 180°/0°.
U57 then took the four CAN termination resistors (R10/R14/R15/R24) from 0603 to
0805 — **a different LCSC part, C3959530, so the BOM changed as well as the
copper** — moved R15 0.70 mm, and put two vias and a short Bottom-layer run
into `/CAN2_TX`. U58 added **two more parts** (C72/C73, TJA1051 VIO bypass):
no new BOM line — they are the 100 nF Basic part already fitted 41× — but the
**CPL grew from 140 to 142 rows**, so an assembly order placed against an older
CPL will leave both unfitted.
`gerbers-jlcpcb.zip` is a build artefact and is not tracked; regenerate with
`"C:/Program Files/KiCad/10.0/bin/python.exe" tools/kicad_fab.py kicad/board3`.

**No open electrical items.** U42 closed 2026-08-03: the CAN termination
mid-point capacitors (C57/C58) are deleted, so each bus is now a plain
120.8 Ω end termination selected by its jumper. With H1/H3 **open** the
resistors float — there is no longer a permanent RC load on `CAN_L`. Fit the
jumpers only if this board is at an end of the CAN bus.
