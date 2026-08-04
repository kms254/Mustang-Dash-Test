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
| Board size | **250 × 50 mm** | Measured 250.254 × 50.254 over Edge.Cuts. Not 230 × 50 — that figure circulated in session notes and review prompts and is wrong (U39). |
| Layers | **4** | |
| Thickness | **1.6 mm** | |
| Copper weight | **1 oz (35 um), outer and inner** | The order form asks, and it changes both price and minimum trace/space. The design rules here (0.1016 mm min track/clearance) are JLC's standard 1 oz limits; 2 oz would relax nothing and cost more. |
| Surface finish | **ENIG** | `(copper_finish "ENIG")` is in the board's stackup block. A real cost adder over HASL, so it will not be chosen for you. |
| Solder mask colour | **GREEN — required, not a preference** | The narrowest mask dam on the board is **0.1795 mm** (U1.129 ↔ C6.2), and **51 dams sit under 0.20 mm**. Green holds a 0.1 mm dam, so those pass with ~80 µm to spare. **JLC's other colours need 0.25 mm**, which every one of those 51 would violate — the mask would simply not print between those pads, next to a 0.5 mm-pitch QFP. Picking a colour for looks is a real defect here. |
| Minimum hole size | **0.20 mm** | Every via but one drills 0.25; the single BTN1 via at U1.93 is 0.5/0.2 and forces the lower tier. Declaring 0.25 is wrong. Stated as a rule rather than a count — the count moves with every routing change (U50 took it 222 → 234), and a stale number here is worse than none. |
| Via covering | **Tented** | The board sets `m_TentViasFront` and `m_TentViasBack` true. JLC's *plugged*-via service caps at 0.5 mm and ours are 0.6 mm, so tenting is the only option that matches. |
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

As of 2026-08-03, after U50:

- DRC **0 errors, 0 warnings, 0 unconnected, 0 schematic parity** (run in place,
  `--severity-all --schematic-parity`)
- BOM 42 lines / CPL 140 rows, symmetric
- Every via-in-pad remaining is deliberate (U4's exposed-pad thermal vias, Q2's
  plane stitches, U1.93's documented exception)

**Re-upload the gerbers if you are ordering after 2026-08-03.** U50 moved the
four panel-SPI series resistors (R33/R34/R36/R37) to their MCU pins and re-cut
their nets, so any zip built before that describes different copper. The CPL
changed too — those four parts moved and their rotations went 90° → 180°/0°.
`gerbers-jlcpcb.zip` is a build artefact and is not tracked; regenerate with
`"C:/Program Files/KiCad/10.0/bin/python.exe" tools/kicad_fab.py kicad/board3`.

**No open electrical items.** U42 closed 2026-08-03: the CAN termination
mid-point capacitors (C57/C58) are deleted, so each bus is now a plain
120.8 Ω end termination selected by its jumper. With H1/H3 **open** the
resistors float — there is no longer a permanent RC load on `CAN_L`. Fit the
jumpers only if this board is at an end of the CAN bus.
