# Board3 bring-up card

One page for the bench. Everything here was measured off the board/gerbers in
the 2026-08-07 pre-order review (`pcbnew` + gerber parse); coordinates are
board-frame mm. Print this before the boards arrive.

## Test points — three identical bare 0.6 mm ENIG dots, NO silk labels

| TP | Net | Location | Identify by continuity |
|---|---|---|---|
| TP1 | `/+5V`  | (126.251, 107.730) | beeps to FPC pin 17/18 (backlight +5V) |
| TP2 | `/+3V3` | (121.314, 99.332)  | beeps to FPC pin 1 (panel 3.3 V) |
| TP3 | `/GND`  | (130.442, 110.508) | beeps to MP1–MP4 corner pads |

## The zero-tools MCU test (do this before suspecting anything else)

Hold **BOOT** (SW7) + tap **RESET** (SW6) → STM32 AN2606 system bootloader →
**DFU device enumerates on USB**. No firmware, no debugger. Enumerates = MCU,
power, and the whole USB path are good; the problem is firmware or downstream.

## Dead-board walk (DMM + scope, no desoldering at any step)

1. Power from **barrel, 5 V ONLY** (silk says so; 12 V kills U8/U9/U11/U12 and
   the LEDs). Ground reference: TP3 or any MP corner pad.
2. **TP1 = 0 V?** Walk the input: DC1.1 (`/DC1_IN`, THT pin) → F1.2
   (`/+5V_BARREL`, 1812 pad) → Q2 pins 1–3 vs 5–8 (SOP-8). Unpowered
   diode-mode across Q2: body diode ~0.3 V = FET fine, controller (U6) suspect;
   no diode reading = wrong/reversed FET.
3. **TP1 ok, TP2 = 0?** Ohms TP2↔TP3 unpowered. ~0 Ω = shorted 3V3 rail —
   **unplug panels one at a time** (splits board-short from panel/FFC-short;
   the test the original FFC incident never had). Open → scope `/BUCK_SW` at
   L2's pad; check R2/R3 feedback divider.
4. **Rails ok, no serial?** BOOT+RESET → DFU enumerate? Yes → firmware problem.
   No → **swap the USB cable first** (the flaky-cable incident), then SWD via
   H2. Core alive → USB path (D9's SOT-23-6 pads); core dead → `/NRST`
   (probe at SW6, R49, C46, or H2.3).
5. **MCU up, glass dark?** Center panel's whole SPI is one 0603 row at
   y≈92.99 beside FPC2: R32.2=`/SCLK_C` · R35.2=`/MOSI_C` · R41.1=`/MISO_C` ·
   R45.1=`/CS_C` · R11.1=`/PD_C`, ground at R41.2. **No panel attached**:
   CS/PD read 3.3 V (pull-ups), MISO reads low (100 k pull-down) — so "MCU
   never ran" vs "ran and hung" is a DMM question. Verify +5V/+3V3 continuity
   from TP1/TP2 to each FPC tail *before* risking a panel. Backlight end of an
   FFC: pins 19/20 beep to GND.
6. **Telltales dark?** I2C tap at R5/R8 (4.7 k pull-up pads); scan expects
   **0x5B (U11 west)** and **0x5A (U12 east)**. A hung expander recovers by
   holding its RSTN low at the C70/C71 pads — no bodge needed.

## Left/right panel caveat (do not trust a launch-end waveform)

Only **MISO/CS/PD** have connector-end probe resistors on left/right (left
R42/R46/R12, GND at R42.2; right R43/R47/R13, GND at R43.2). **SCLK/MOSI series
resistors sit at the MCU end** (U50 moved them — correctly), so the *receiver*
end of those 80–90 mm lines is only the 0.5 mm FPC solder tails. For the
13.5 MHz clock re-walk on this copper (required before trusting the bench
number — topology changed), probe left/right SCLK at the FPC tail, not at
R33/R36.

## Bring-up debt — run these on the first good board

- **U2 QSPI JEDEC-ID read** (~20 lines: QUADSPI init, command 0x9F, expect
  `EF 40 19` for W25Q256JV — confirm against the datasheet). U2 has no other
  consumer in the firmware; this read is the entire return on fitting it, and
  the decision to keep it fitted (2026-08-07) was made on the condition this
  test exists. Until it passes, a dead U2 is invisible.
- **13.5 MHz SPI clock re-walk** per panel (see caveat above). Bench numbers
  came from failure on different topology and do not transfer.
- **CAN1 first bring-up needs H1 closed** — the order contains the headers but
  **no 2.54 mm shunts** (JLC doesn't supply them). Put shunts in the parts
  order or confirm drawer stock *before* the boards arrive.

## Known-tight spots (do not "discover" these)

- P1/P2 screw terminals sit 0.65 mm from the CAN transceivers (U8/U9) — care
  with the iron during the THT pass.
- U1×X1: crystal pad 0.243 mm from `/QSPI_IO1` — inspect after reflow.
- Zero reference designators print anywhere on the board (deliberate).
  Identification is the CPL + the functional silk labels (TT1–8, BTN1–4,
  RESET/BOOT, CAN1/2 + H/L, TERM1/2, USB, SWD, LEFT/CENTER/RIGHT, 5V ONLY).
