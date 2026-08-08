# Board3 bring-up card

One page for the bench. Everything here was measured off the board/gerbers in
the 2026-08-07 pre-order review (`pcbnew` + gerber parse); coordinates are
board-frame mm. Print this before the boards arrive.

## Flashing — first connect

1. **ONE-TIME, before anything else** (H755 boots BOTH cores from the factory
   and the CM4 has no image):
   `STM32_Programmer_CLI -c port=SWD -ob BCM4=0`
   via ST-LINK on H2 (Tag-Connect TC2030, hand-held). Skipping this leaves the
   CM4 hard-faulting in the background — usually survivable, never desirable.
2. **Build + flash:** `./scripts/compile.sh board3`, then
   `pio run -e board3 -t upload` (ST-LINK on H2), **or** DFU with no debugger:
   hold BOOT + tap RESET, then `dfu-util` / STM32CubeProgrammer over USB.
3. The env targets the CM7 through the `nucleo_h743zi` variant (H743ZIT6 is
   the same LQFP-144 pin map; Board3's 25 MHz crystal is handled by the
   sketch's `SystemClock_Config` override — 400 MHz, USB on HSI48+CRS).
   Serial is **USB CDC** (enumerates as "MustangDash", 115200, `ok`/`err`
   protocol as on the bench).
4. **Boot banner self-checks:** panel init lines per panel, telltale expander
   ack (`west`/`east`), and `U2 QSPI JEDEC: EF 40 19 -- ok` — the U2
   keep-fitted condition, checked automatically every boot. `MISMATCH` or
   `FAILED` = U2/QSPI problem; the dash still runs.

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

- **U2 QSPI JEDEC-ID read — DONE, runs at every boot.** `board3_qspi_jedec_probe()`
  in the sketch prints the ID in the boot banner (see Flashing §4). The
  keep-fitted condition is satisfied the moment the first banner shows `ok`.
- **13.5 MHz SPI clock re-walk** per panel (see caveat above). Bench numbers
  came from failure on different topology and do not transfer.
- **CAN1 first bring-up needs H1 closed** — the order contains the headers but
  **no 2.54 mm shunts** (JLC doesn't supply them). Put shunts in the parts
  order or confirm drawer stock *before* the boards arrive.

## Prove the firmware BEFORE the boards ship — `[env:board3_mule]`

Target: the bench **NUCLEO-H755ZI-Q** — the **same STM32H755 die as U1**, with
every Board3 signal on the Zio headers. Same firmware as `board3` except the
clock/supply block: the -Q board is **SMPS-powered**, so the env's
`DASH_MULE_H755Q` selects a direct-SMPS clock config (the stock H743-variant
config selects LDO and *hangs SMPS silicon at the VOSRDY wait* — a board that
looks bricked; recover by flashing under reset). Being the real die, the mule
also **rehearses the one-time `BCM4=0` option-byte step** before Board3 needs
it, and proves USB-on-HSI48+CRS on the exact silicon.

**Bench trap:** the Nucleo's ST-LINK VCP is USART3 on **PD8/PD9** — the center
and left CS pins. Open the two VCP solder bridges (Nucleo-144 user manual)
before the panel session, or the ST-LINK's TX fights left CS.

Panels on the existing FFC breakouts at the *real* Board3 pins:

| Session | Wires | Proves |
|---|---|---|
| Panels + dash | FFC breakouts → PA5/6/7+PD8/PD11, PB13/14/15+PD9/PD12, PE2/5/6+PE3/PD13; BL on external 5 V | the whole render path on H7 silicon at Board3's pins |
| USB CDC | USB device connector | serial protocol + `/dash` skill over CDC (never bench-run on H7) |
| Telltales | AW9523B breakout ×2 on PB10/11 (strap 0x5B/0x5A) | the I2C lamp glue, which has **never had hardware** |
| Odometer | nothing | EEPROM emulation in H7 flash across power cycles |
| Button | momentary switch PC6→GND | the remapped gesture pin |
| QSPI | (optional) W25Q256 breakout PB2/PF6-9/PG6 | the JEDEC probe's ok path; unwired it proves the failure path boots on |

After a clean mule soak, first contact with Board3 tests only three things:
the 25 MHz clock block, the CM4 option byte, and real copper at 13.5 MHz.

## Known-tight spots (do not "discover" these)

- P1/P2 screw terminals sit 0.65 mm from the CAN transceivers (U8/U9) — care
  with the iron during the THT pass.
- U1×X1: crystal pad 0.243 mm from `/QSPI_IO1` — inspect after reflow.
- Zero reference designators print anywhere on the board (deliberate).
  Identification is the CPL + the functional silk labels (TT1–8, BTN1–4,
  RESET/BOOT, CAN1/2 + H/L, TERM1/2, USB, SWD, LEFT/CENTER/RIGHT, 5V ONLY).
