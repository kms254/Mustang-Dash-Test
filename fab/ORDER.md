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
| Board size | **250 × 50 mm** | Measured 250.254 × 50.254 over Edge.Cuts. Not 230 × 50 — that figure circulated in session notes and review prompts and is wrong (U39). |
| Layers | **4** | |
| Thickness | **1.6 mm** | |
| Copper weight | **1 oz (35 um), outer and inner** | The order form asks, and it changes both price and minimum trace/space. The design rules here (0.1016 mm min track/clearance) are JLC's standard 1 oz limits; 2 oz would relax nothing and cost more. |
| Surface finish | **ENIG** | `(copper_finish "ENIG")` is in the board's stackup block. A real cost adder over HASL, so it will not be chosen for you. |
| Minimum hole size | **0.20 mm** | 221 vias drill 0.25, but the single BTN1 via at U1.93 is 0.5/0.2 and forces the lower tier. Declaring 0.25 is wrong. |
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
is designed in. At 250 × 50 mm this is a long, narrow board; if JLC's assembly
line requires rails they add them themselves, but it is worth confirming at quote
rather than discovering it as a hold. Nothing in the board files declares an
intent either way.

## Board state

As of 2026-08-03, on branch `fix/board3-review-blockers`:

- DRC **0 errors, 0 warnings, 0 unconnected, 0 schematic parity**
- BOM 42 lines / CPL 140 rows, symmetric
- Every via-in-pad remaining is deliberate (U4's exposed-pad thermal vias, Q2's
  plane stitches, U1.93's documented exception)

**No open electrical items.** U42 closed 2026-08-03: the CAN termination
mid-point capacitors (C57/C58) are deleted, so each bus is now a plain
120.8 Ω end termination selected by its jumper. With H1/H3 **open** the
resistors float — there is no longer a permanent RC load on `CAN_L`. Fit the
jumpers only if this board is at an end of the CAN bus.
