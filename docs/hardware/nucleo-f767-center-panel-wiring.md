# NUCLEO-F767ZI — Center 7" Panel Setup & Wiring Guide

Bench guide for first light on the NUCLEO-F767ZI driving the center RVT70H
(7" 1024x600, BT817) over SPI1. Executes U3 of
`docs/plans/2026-07-21-002-feat-f767-first-light-plan.md`; the sides come
later (appendix). Board authority for anything not covered here:
`docs/hardware/datasheets/um1974-nucleo-144-mb1137.pdf` (UM1974 — many board
variants; read only NUCLEO-F767ZI rows/columns).

## 1. Jumper check (before anything else)

Factory defaults — verify, don't move (UM1974 §3 out-of-box config):

| Jumper | Position | Meaning |
|---|---|---|
| JP3 | pins 3-4 (U5V) | powered from ST-LINK USB |
| JP1 | OFF | 300 mA declared at USB enumeration |
| JP5 (IDD) | ON | MCU power measurement bridge closed |
| CN4 (x2) | both ON | ST-LINK programs the on-board MCU |

Nothing moves tonight unless power Plan C fires (below). CN4 comes off only
when using the CN6 SWD connector to program an external target — not tonight.

## 2. Workstation + board setup (U2 gates, no panel attached)

1. Install the ST-LINK USB driver: **STM32CubeProgrammer** preferred (bundles
   driver + JRE + firmware updater), or bare STSW-LINK009. No separate VCP
   driver needed on Windows 11.
2. Plug the Nucleo into a **root USB port** (no hub).
3. Run the ST-LINK firmware update once (CubeProgrammer → Firmware upgrade).
   **Skippable** if the updater won't launch — proceed to upload and come
   back only if OpenOCD misbehaves.
4. `pio run -e nucleo_f767 -t upload` (default `upload_protocol = stlink`).
5. Open the VCP COM port at 115200. Expect the boot banner; `status` must ack
   `ok` with **no panel attached** (missing panels retire; dash runs headless).

Gate: do not wire the panel until step 5 passes.

## 3. Wiring table (power off, USB unplugged)

Breakout pad N = RiBus pin N (MTCELL FPC-20P, verified by continuity on the
center breakout — re-verify pad-frame vs pad 20 on any breakout not
previously bench-tested). FFC contacts face **down** at the panel end.

| Signal | MCU pin | Zio label | Connector pin | Breakout pad | Wire |
|---|---|---|---|---|---|
| SCLK | PA5 | D13 | CN7 pin 10 | 3 | |
| MISO | PA6 | D12 | CN7 pin 12 | 4 | |
| MOSI | PB5 | D22 | CN7 pin 13 | 5 | |
| CS | PF13 | D7 | CN10 pin 2 | 6 | |
| PD/RST | PF14 | D4 | CN10 pin 8 | 8 | |
| Panel VDD 3.3 V | — | +3V3 | CN8 pin 7 | 1 | |
| Panel GND | — | GND | CN7 pin 8 (or CN8 pin 11) | 2 | |
| INT | — | — | not connected (we poll) | 7 | — |
| BL+ (backlight 5 V) | — | per power plan below | — | 17 **and** 18 | doubled |
| BL− (backlight return) | — | per power plan below | — | 19 **and** 20 | doubled |

**Zio diagram reading trap (cost us a double-take at the bench):** ST's Zio
pinout diagram prints TWO label columns per side — ST name + Arduino name
both describing the SAME pin. Power pins repeat the name ("+5V +5V"),
which reads as if the adjacent even pin were also +5V. It is not: CN8 has
exactly one +5V (pin 9); pins 10/12 are GPIOs **PC12/PD2 — and PD2 is
telltale lamp 5 in this build**, so landing backlight return there would
fight a driven output. Doubling: BL+ x2 joined into CN8-9; BL− split
across CN8-11 and CN8-13 (both GND).

Pin facts verified 2026-07-21 against UM1974 + STM32duino
`variant_NUCLEO_F767ZI`. All five signal pins are free of on-board functions
(Ethernet RMII, LEDs, USB, VCP all elsewhere). PB5 at D22 is a direct
connection — no solder-bridge work (SB121/SB122 only affect D11, unused).

**Before any power:**

- Beep pads **19-20 (BL−) to pad 2 (GND)** — continuity proves which FFC end
  is the backlight end (the RiBus ties BLGND to logic GND internally).
- If this breakout is untested: beep panel frame to pad 20 (should beep) and
  pad 1 (should stay silent).
- Visually check the FFC ends for damage (a damaged end shorting pins 1-2 =
  VDD-GND short; symptom: board won't enumerate on USB).

## 4. Backlight power — tiered plan (fastest first)

**Plan A — single cable, try first:** BL+ (pads 17-18) → Nucleo **+5V, CN8
pin 9**; BL− (pads 19-20) → **GND, CN8 pin 11**. Everything runs off the one
ST-LINK USB cable.

- The USB/5V budget is 500 mA total (regulator U6 limit; red **LD5** = over
  500 mA). Boot pushes the backlight to **full duty at the crossfade** — that
  exact moment is where Plan A fails if it's going to.
- Failure signatures: LD5 lights, or the board browns out — possibly as a
  **repeating boot loop** (reset → splash → crossfade → reset) with the VCP
  COM port dropping and re-enumerating each cycle. Non-destructive.

**Plan B — on any Plan A failure:** move **only** BL+/BL− to the bench buck
at 5.00 V; tie buck GND to a Nucleo GND (common ground). Logic stays USB-fed.
This is the Teensy rig's proven arrangement, and it removes the backlight
from the board's budget entirely.

**Plan C — only if USB power is flaky for the board itself** (resets with the
backlight already on Plan B): 7-12 V into **VIN, CN8 pin 15**, move **JP3 to
pins 5-6**, JP1 stays OFF. **Mandatory order: external supply ON first, USB
plugged after** (UM1974 §6.4.3). USB stays connected for ST-LINK + VCP.
(E5V exists but its pin is on the unpopulated morpho CN11 — soldering
required; not tonight's path.)

## 5. Power-up and first-light gates (U4)

In order; stop at the first failing gate:

1. Power up. VCP enumerates, boot banner prints.
2. `REG_ID` reads `0x7C` (in the boot diagnostics).
3. Splash plays **from RAM_G** — staging summary line clean, no miscompare
   lines — then crossfades into the dash; gauges animate from the simulator;
   `status` acks. **Eyes on the panel:** the picture must match the known
   dash look (serial health cannot see render corruption).
4. Clock walk (optional tonight): note that the F767's SPI1 can only produce
   **6.75 / 13.5 / 27 / 54 MHz** (216 MHz core, APB2 108 MHz, power-of-two
   prescalers) — the firmware's "8 MHz" request actually runs at **6.75 MHz**
   (safe: EVE_init needs < 11 MHz). The only raises are 13.5 then 27 MHz;
   accept a step only on clean staging spot-checks + `REG_ID` + font-verify
   reads **and** an eyes-on-panel check — never frame rate. Record the
   attained (not requested) frequency.

## 6. Panel-flash wipe (after first light only — U5)

When first light has passed and the `flashwipe` command is flashed: run it
with its confirmation argument. **Expect minutes of total silence** (64 MB
chip erase: typically 2-3+ min; the dash freezes and serial says nothing).
**Do not power-cycle during the wipe** — `ok` is the completion signal. A
power cycle afterward must boot identically (boot never touches panel flash).

## Appendix — side panels (three-panel night, not tonight)

From the committed pin plan in `MustangDash/MustangDash.ino` (Zio positions
for these pins not yet verified against UM1974 — verify before wiring):

| Panel | SPI | MOSI | MISO | SCLK | CS | PD |
|---|---|---|---|---|---|---|
| Left 5" | SPI2 | PB15 | PC2 | PB10 | PE9 | PE13 |
| Right 5" | SPI4 | PE6 | PE5 | PE2 | PE11 | PF15 |
