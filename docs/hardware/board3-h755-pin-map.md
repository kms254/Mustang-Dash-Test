# Board3 STM32H755ZIT6 Pin Map (U1 — definitive reference)

Part: STM32H755ZIT6, LQFP-144 (JLC C730212). Pin numbers verified against DS12923 Rev 3 Table 8/9 (STM32H745xI/G — identical pinout per the datasheet's compatibility statement), extracted 2026-07-25. The schematic (U2–U7) and the firmware's Board3 pin table both follow this document; any change lands here first.

**Status: DRAFT until U8 layout freeze.** Priority is hardware, not software (plan KTD1): GPIO-class assignments (CS, PD-reset, lamps, button) and the SPI-peripheral↔panel pairings may be reassigned during layout wherever routing benefits — firmware pin constants are rewritten from the final table. Only the AF constraints and dedicated pins below are immovable.

## Panel SPI buses (point-to-point, 33 Ω series at MCU, 10 kΩ CS pull-up + 100 kΩ MISO pull-down per bus)

| Net | Port | LQFP-144 pin | AF |
|---|---|---|---|
| Center SCLK (SPI1_SCK) | PA5 | 44 | AF5 |
| Center MISO (SPI1_MISO) | PA6 | 45 | AF5 |
| Center MOSI (SPI1_MOSI) | PA7 | 46 | AF5 |
| Center CS | PD8 | 76 | GPIO |
| Center PD-reset | PD11 | 81 | GPIO |
| Left SCLK (SPI2_SCK) | PB13 | 73 | AF5 |
| Left MISO (SPI2_MISO) | PB14 | 74 | AF5 |
| Left MOSI (SPI2_MOSI) | PB15 | 75 | AF5 |
| Left CS | PD9 | 77 | GPIO |
| Left PD-reset | PD12 | 82 | GPIO |
| Right SCLK (SPI4_SCK) | PE2 | 1 | AF5 |
| Right MISO (SPI4_MISO) | PE5 | 4 | AF5 |
| Right MOSI (SPI4_MOSI) | PE6 | 5 | AF5 |
| Right CS | PE3 | 2 | GPIO — **moved from PD10 at layout (U18, 2026-07-28)**: PD10 exits the west edge and crossed the entire package to reach FPC3; PE3 is free, full-speed, and sits inside the right-panel SPI escape group on the east edge. PD10 is now unassigned. |
| Right PD-reset | PD13 | 83 | GPIO |

Rules: no `_C` dual-pad pins on any SPI net (PC2_C pin 30 exists but is analog-only — unused); no pull-down on PB15 (a low PB15 blocks all non-USB ROM-bootloader interfaces, AN2606 §52).

## QUADSPI NOR (W25Q512JVEIQ, C7389628)

| Net | Port | LQFP-144 pin | AF |
|---|---|---|---|
| CLK | PB2 | 51 | QUADSPI_CLK |
| NCS | PG6 | 86 | AF10 BK1_NCS |
| IO0 | PF8 | 22 | BK1_IO0 |
| IO1 | PF9 | 23 | BK1_IO1 |
| IO2 | PF7 | 21 | BK1_IO2 |
| IO3 | PF6 | 20 | AF9 BK1_IO3 |

IO3 on PF6 (verified bonded with the AF) keeps PD13 as the right panel's reset. The generic-carrier pin table ran verbatim until U18 (2026-07-28) moved Right CS to PE3 — one pin-constant edit, sanctioned by the DRAFT clause above and recorded in the firmware-edits list below. PF6–PF9 form a contiguous cluster (pins 20–23) for tight layout. ES0445 note for firmware: memory-mapped read of the region's last byte returns 0x00 and can stall AXI — pad `FSIZE`.

## Peripherals

| Net | Port | LQFP-144 pin | AF |
|---|---|---|---|
| FDCAN1 RX / TX (CAN A) | PB8 / PB9 | 136 / 137 | AF9 |
| FDCAN2 RX / TX (CAN B) | PB5 / PB6 | 132 / 133 | AF9 |
| I2C2 SCL / SDA (FRAM 0x50, AW9523B west 0x5B, AW9523B east 0x5A) | PB10 / PB11 | 66 / 67 | AF4 — trunk taps forward from the FRAM (U7) per the I2C revision plan (2026-07-28-001); ~200 mm with four device loads, well inside 400 pF at 400 kHz |
| USB OTG_FS DM / DP | PA11 / PA12 | 100 / 101 | AF10 |
| USB VBUS sense | PA9 | 98 | additional fn OTG_FS_VBUS — wired to VBUS via removable link (Nucleo SB21 analog) |
| SWDIO / SWCLK | PA13 / PA14 | 102 / 107 | AF0 |
| SWO (test point only; header stays 5-pin) | PB3 | 130 | AF0 |
| ~~Lamps (ULN2803 inputs)~~ **superseded 2026-07-29** | ~~PD0–PD7~~ **unassigned** | 112–117, 120, 121 | The telltales left the MCU in the I2C revision (plan 2026-07-28-001): two AW9523B expanders on I2C2 drive them. West IC **U11** at 0x5B (AD1=AD0=+5V — every port POR-safe), east IC **U12** at 0x5A (AD1=+5V, AD0=GND — LEDs on its POR-safe P1_4–P1_7). Both ICs run **VCC=+5V** (power-on "high" equals the anode rail, LEDs hard off; all pins rated 6 V abs max, I2C VIH fixed 1.4 V so the 3.3 V trunk is legal). RSTN strapped to +5V (internal 100 kΩ pull-DOWN). INT unused — `dash_button.h` polls; the AW9523B's anti-jitter stacks harmlessly. Firmware: ISEL ×2/4 range (~18.5 mA full-scale), DIM registers 0x2C–0x2F both ICs, 5 ms post-POR wait, ID reg 0x10 reads 0x23. PD0–PD7 are free for future use. |
| Mode/trip button (active-LOW, internal pull-up, pressed→GND) | PC13 | 9 | GPIO |
| HSE crystal 25 MHz | PH0 / PH1 | 25 / 26 | OSC_IN / OSC_OUT |
| NRST (button + 100 nF) | NRST | 27 | — |
| BOOT0 (10 kΩ pull-down + button to 3V3) | BOOT0 | 135 | — |
| VCAP — 3 pins, 2.2 µF each, not tied together | VCAP | 68, 103, 140 | verify count visually in the PDF (text extraction shows three rows; expected for the dual-core part's domains) |

Power strapping (DS12923 §3.5.1): VDDSMPS ties to VDD (hard sequencing rule); VSSSMPS → GND; VLXSMPS/VFBSMPS floating (community-confirmed for H745/755 — re-verify at U2 against the current datasheet rev).

## Enumerated firmware edits (complete list)

1. ~~FRAM I2C: the sketch's `Wire.begin()` uses the variant default (collides with FDCAN1 on PB8/PB9) — add the Wire pin selection to PB11/PB10 (SDA/SCL) in the Board3 build glue.~~ **Closed 2026-07-29 (U21):** `dash_lamps_init()` sets `Wire.setSDA(PB11); Wire.setSCL(PB10)` before any `Wire.begin()`; the FRAM inherits the corrected pins.
2. Right CS is **PE3**, not PD10 (`DASH_CS_PINS[right]` in the sketch — applied 2026-07-28 with U18; see the panel-SPI table note).
3. **Applied 2026-07-29 (U21):** the carrier branch drives lamps through the two AW9523Bs (addresses/registers per the Lamps row above); `DASH_LAMP_PINS` no longer exists on the carrier. Teensy and F767 branches unchanged.

Plus whatever pin reassignments U8's layout earns (recorded here at freeze). The generic-carrier branch's tables apply verbatim *as drafted*; the final firmware pin table is written from this doc after layout. Board-glue items (clock/PWR LDO flag, build env, CM4 park, `DASH_SPI_RUN_HZ` walk values) are tracked in the plan's Dependencies.

## U1 part selections

| Role | Part | LCSC | Notes |
|---|---|---|---|
| 5 V → 3.3 V buck | TPS563201DDCR (TI, 3 A sync, SOT-23-6) | C116592 | 103k stock @ $0.07 (2026-07-25); replaces the K7803 THT module |
| HSE crystal | 25 MHz ±20 ppm, 3225 SMD, load caps per part CL | pick at U2 | DFU is crystal-free (AN2606 §52) — value serves the app PLL only (25→480 MHz: /5 ×192 /2) |
| CAN connectors | 5.08 mm 2-pin screw terminal per bus | pick at U6 | exact LCSC chosen from stock at capture time |

## H743 single-core fallback (stock hedge) — status: NOT yet cleared

The H743ZIT6 shares the LQFP-144 land pattern, but pins 14–17 differ (SMPS pins on the H755 vs other functions on the H743), so the strap nets (VDDSMPS→VDD etc.) may conflict with what the H743 puts on those physical pins. A pin-by-pin diff of positions 14–17 (and any other divergent rows) is required before the fallback can be relied on. Do this before the U10 order decision if JLC stock looks thin.
