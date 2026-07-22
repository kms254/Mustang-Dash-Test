---
title: "STM32 Migration - Plan"
date: 2026-07-21
artifact_contract: ce-unified-plan/v1
artifact_readiness: implementation-ready
execution: code
product_contract_source: ce-brainstorm
---

# STM32 Migration - Plan

## Goal Capsule

**Objective.** Replace the Teensy 4.1 + carrier PCB with a surface-mount
STM32H7-based dash controller: dedicated SPI per panel, plus new vehicle-side
I/O (telltales, switches, USB-C, dual CAN). This is a v2 dash controller, not
a bare MCU port.

**Product authority.** Kevin.

**Definition of done.** The STM32 board runs the full current dash at feature
parity with `main` — splash, all three panels, serial protocol, odometer
persistence — with the new hardware provisioned and proven at the hardware
level (CAN decode firmware explicitly staged out). Verification Contract
gates below define "proven."

**Product Contract preservation:** unchanged from the 2026-07-21 brainstorm.
Planning added the Planning Contract, Implementation Units, and Verification
Contract; one brainstorm-deferred item (odometer backend) is resolved as
KTD4.

---

## Product Contract

### Motivation (settled)

1. **Surface-mount robustness** — eliminating the Teensy-module-on-carrier.
   Primary driver.
2. **Precedent** — the Riverdi eval board (the panel vendor's own reference
   design) is STM32-based.
3. **Dedicated SPI per panel** — deletes the fan-out/combining logic entirely.
4. **Vehicle integration** — the dash becomes a real car device: live CAN
   data paths, physical warning lamps, physical controls.

### Decisions made (brainstorm)

- **Replace, not v2-alongside.** The Teensy carrier PCB is abandoned unfabbed.
- **Full replacement in one plan** — board design + firmware port together.
- **Dual/quad SPI lines routed to all three panels, unused by firmware.**
- **CAN staging: hardware now, decode later.** This plan proves buses routed
  and transceivers alive (loopback / visible traffic). CAN->DashState decode
  is a follow-on plan once real ECU/PMU hardware is on the bench.

### Hardware contract

- **3x dedicated SPI**, one per panel (center 7" RVT70H, left/right 5"
  RVT50H): point-to-point SCLK/MOSI/MISO/CS plus PD-RST per panel.
- **Both logic ICs deleted** (LVC244/LVC125). Point-to-point into a
  high-impedance CMOS receiver at 30 MHz is ordinary design; both sides
  3.3 V. **33 ohm series termination at each MCU SPI output.** Residual
  variable is FFC run length; mitigate with GPIO slew-rate config before
  reconsidering buffering.
- **8 warning telltale LED outputs** — independent discrete indicators, a
  hardware mirror of the existing alarm logic. PWM-dimmable as a group;
  lamp-test capability so a dead LED cannot hide a safety warning.
  **Driver (Kevin, 2026-07-21): SMD ULN2803A darlington array** — one
  SOP-18 between the GPIO bank and the 5 mm LEDs, lamps fed from the 5 V
  rail (sunlight-visible current off the MCU's budget; group PWM passes
  through the array unchanged). JLC: UMW clone C845537 $0.21 / 222k stock
  (extended) beats TI ULN2803ADWR C9683 $6.22 (preferred) even with the
  feeder fee. Series resistors sized per LED color at U2.
- **2-4 switch inputs**, debounced: trip/odometer reset (momentary) + spares.
- **USB-C connector**: device mode only — VCP carrying the existing 115200
  serial protocol, plus STM32 ROM DFU for field reflash. Not the power
  strategy; no host mode.
- **2x CAN** (FDCAN + transceivers), selectable termination: Bus 1 = engine
  ECU; Bus 2 = PMU/power distribution.
- **Odometer persistence** surviving power cycles with the existing
  CRC-record semantics (backend resolved in KTD4).
- **External QSPI NOR storage (Kevin, 2026-07-21): Winbond W25Q01JV-class
  on the H755's QUADSPI.** Uses: CAN telemetry logging (M4 side; decoded
  dual-bus runs ~16 KB/s, so 32 MB = ~30 min, 128 MB = 2+ h) and
  optionally hosting the splash provisioning pack (frees ~660 KB of MCU
  flash). The whole W25Qxx family shares one command set, so capacity is
  pure economics decided at U1: JLC's 1 Gbit pricing is broker-tier
  (C5137036 $54 / C2962013 $27.6) while W25Q256/512JV are cheap and
  deep-stocked; the true W25Q01JV runs ~$6-8 at Digi-Key if the full
  128 MB earns its keep. Size to the longest logging session wanted.

---

## Planning Contract

### Key Technical Decisions

- **KTD1 — MCU: STM32H743VIT6, LQFP-100, 2 MB flash / 1 MB RAM.**
  JLC C114409 ($25.12, 140 stock) or the TR reel C5271084 ($10.91, 94 stock)
  — same part, prefer whichever is cheaper in stock at order time.
  **Named fallback: STM32H743IIT6 LQFP-176 (C89597, $12.13, 1257 stock)** —
  same silicon, deep stock, more I/O; larger footprint. Stock is shallow on
  the VIT6; re-run the availability check immediately before the JLC order
  and fall back without redesign anguish (pin work is a remap, not a
  re-architecture). Rationale: 2 MB flash is the binding constraint — the
  current build is ~770 KB, dominated by the 659 KB splash pack; 1 MB parts
  (VGT6, ZGT6, H723) fail headroom.
- **KTD2 — Firmware stack: Arduino (STM32duino core) via PlatformIO.**
  Keeps the `.ino`, Arduino APIs, and the existing PlatformIO workflow;
  maximum survival of `MustangDash/` code. The EVE library's Arduino path
  auto-selects via its generic STM32 support; verify during U4 and pin the
  target header explicitly if auto-selection misfires. FDCAN and USB CDC come
  from the STM32duino ecosystem (core CDC; FDCAN library) rather than bare
  HAL. Alternative (bare STM32Cube HAL) rejected: rewrites all glue for
  control the dash does not need.
- **KTD3 — CAN transceivers: 2x NXP TJA1051T/3 (JLC C38695, $0.59, 159k
  stock).** CAN FD capable (5 Mbps), automotive temp, and the `/3` variant's
  VIO pin runs I/O at 3.3 V while the transceiver runs from 5 V — no level
  shifting. Split termination (2x 60.4 ohm + cap) behind a jumper per bus.
- **KTD4 — Odometer backend: I2C FRAM, Infineon FM24CL64B (JLC C9829,
  $1.34, 24k stock).** Native 2.7-3.65 V, effectively infinite endurance, no
  wear-leveling code. Resolves the brainstorm's flash-emulation-vs-FRAM
  question: FRAM wins on code simplicity and write-cycle robustness for a
  value updated continuously while driving. The existing two-slot ping-pong
  CRC record layout ports as-is (see
  `docs/solutions/design-patterns/two-slot-pingpong-eeprom-record-torn-write-safety.md`).
- **KTD5 — Power: automotive 12 V input -> 5 V buck -> 3.3 V.** 12 V battery
  input with reverse-polarity, load-dump/transient clamping, and input
  filtering per automotive practice; a buck to 5 V (backlights + CAN
  transceivers), an LDO or second buck 5->3.3 V (MCU + panels' logic).
  Exact regulator parts selected in U1 via JLC search; budget the 5 V rail
  for three panel backlights plus transceivers (measure backlight draw on
  the bench — U1 input).
- **KTD6 — Panel timing profiles carry over untouched.** `EVE_RVT70H` and
  `EVE_RVT50H` in `libraries/FT800-FT813/src/EVE_config.h` are bench- and
  eval-board-validated; the migration does not touch panel timing.

### Sources & Research

- JLC parts search (2026-07-21, live stock): MCU/transceiver/FRAM figures in
  KTD1/3/4.
- Institutional: `docs/solutions/architecture-patterns/dash-carrier-pcb-buffered-spi-topology-30mhz-clock-contract.md`
  (30 MHz BT817 ceiling, why point-to-point deletes the buffers),
  `docs/solutions/architecture-patterns/bt817-flash-render-streaming-bandwidth-ceiling.md`
  (staging architecture the firmware keeps), two-slot EEPROM pattern doc
  (KTD4).
- Brainstorm grounding (2026-07-21): flash-size constraint, EVE STM32
  targets present in the vendored library, per-panel eval-board validation
  path.

---

## High-Level Technical Design

```mermaid
graph LR
    subgraph Vehicle
        BAT[12V battery] --> PROT[protection:\nrev-pol + transient]
        ECU[ECU CAN] --- T1[TJA1051T/3]
        PMU[PMU CAN] --- T2[TJA1051T/3]
    end
    PROT --> BUCK5[5V buck] --> REG33[3.3V reg]
    BUCK5 --> BL[backlights x3]
    BUCK5 --> T1 & T2
    subgraph Board
        MCU[STM32H743VIT6\nLQFP-100]
        FRAM[FM24CL64B\nI2C FRAM]
        USB[USB-C\nVCP + DFU]
        TT[8 telltales]
        SW[2-4 switches]
    end
    REG33 --> MCU
    T1 -->|FDCAN1| MCU
    T2 -->|FDCAN2| MCU
    MCU ---|I2C| FRAM
    MCU --- USB
    MCU --> TT
    SW --> MCU
    MCU ==>|SPI1 +33R| P1[center 7\" RVT70H]
    MCU ==>|SPI2 +33R| P2[left 5\" RVT50H]
    MCU ==>|SPI3 +33R| P3[right 5\" RVT50H]
    MCU -.->|quad lines, dark| P1 & P2 & P3
```

Firmware shape: the pure headers (`dash_*.h`, `splash_*.h` logic, fonts,
odometer record) are MCU-agnostic and move unchanged; the `.ino` glue is
rewritten around three SPI instances instead of one shared bus + CS mux; the
EVE library's per-panel descriptor (`EVE_select_panel`) likely simplifies to
one context per SPI peripheral.

---

## Implementation Units

### Phase A — Hardware design

### U1. Power architecture and rail design

**Goal:** A complete, reviewed power schematic block: 12 V automotive input
protection, 5 V buck, 3.3 V rail, backlight distribution.
**Requirements:** Hardware contract (power); KTD5.
**Dependencies:** none.
**Files:** EasyEDA project (schematic sheet: power); `docs/hardware/` notes
if a decision needs recording.
**Approach:** Measure real backlight current on the bench first (three
panels, full brightness) — it sizes the buck. Select regulator + protection
parts via JLC search with basic-library preference. Automotive input:
reverse-polarity FET or diode, TVS for load dump, bulk + ferrite input
filter. Panels' logic 3.3 V comes from the board; backlight 5 V rail
switched/fused per panel.
**Test scenarios:** Test expectation: none — design unit; verification is
review + U8 bench measurements (rail voltages under full load, ripple at
the panel connectors).
**Verification:** Power block schematic complete with every part carrying an
LCSC number and value silkscreen per the standing rule; rail budget table
(worst-case current per rail) written into the schematic notes.

### U2. Full schematic capture

**Goal:** Complete board schematic in EasyEDA Pro: MCU core, 3x panel SPI,
I/O, CAN, FRAM, USB-C.
**Requirements:** entire Hardware contract; KTD1-KTD6.
**Dependencies:** U1.
**Files:** EasyEDA project (new board, superseding the tracked Teensy
carrier project).
**Approach:** MCU core per ST hardware checklist: VCAP caps, per-pin
decoupling, 25 MHz crystal (or HSI+PLL — decide against USB clock accuracy
needs; USB wants a crystal), BOOT0 strap, SWD header, NRST. Three SPI
peripherals mapped to panel connectors (FFC, down-side contact per bench
notes) with 33 ohm series resistors at the MCU, PD-RST + CS per panel, quad
data lines routed to RiBus 9-16 left unconnected at the MCU end of firmware
concern but wired to GPIO-capable pins. USB-C: CC pull-downs (5.1k) for
device mode, ESD array on D+/D-. CAN: TJA1051T/3 x2, VIO to 3.3 V, split
termination behind jumpers. Telltales: 8x GPIO -> transistor/driver +
resistor, group PWM enable for dimming, wired so firmware can lamp-test.
Switches: 4x inputs, RC debounce + pull-ups. FRAM on I2C with pull-ups.
**Patterns to follow:** per-panel pin map discipline of
`docs/hardware/three-panel-pin-reference.md`; silkscreen value labels on
every R/C/IC (standing rule).
**Test scenarios:** Test expectation: none — design unit; U8 is the proof.
**Verification:** ERC clean; every part has LCSC number; pin map table
exported into `docs/hardware/` as the successor of the three-panel pin
reference.

### U3. PCB layout and fabrication order

**Goal:** Routed board, JLC fab + assembly order placed.
**Requirements:** Hardware contract; KTD1 fallback rule.
**Dependencies:** U2.
**Files:** EasyEDA project (PCB); order artifacts.
**Approach:** SPI runs length-matched loosely per panel (not across panels),
ground pours, CAN pairs routed differentially to their connectors, USB D+/D-
as a 90 ohm pair. Autorouter/copper-pour via the EasyEDA app UI directly
(bridge APIs unreachable — known gap). **Re-run the KTD1 stock check
immediately before ordering; fall back to IIT6 if VIT6 stock is gone.**
**Test scenarios:** Test expectation: none — DRC + JLC DFM checks are the
gate.
**Verification:** DRC clean; JLC DFM passes; order confirmed with assembly
for all extended parts.

### Phase B — Firmware port (parallel with fab lead time)

### U4. PlatformIO STM32 environment + pure-header port

**Goal:** The sketch compiles for the H743 target with all pure headers
unchanged; host invariant suite still green.
**Requirements:** KTD2; firmware port scope.
**Dependencies:** none (parallel with Phase A).
**Files:** `platformio.ini` (new env), `MustangDash/MustangDash.ino`
(conditional target glue), `tests/run-tests.sh` (unchanged — verify).
**Approach:** Add an `env:h743` (STM32duino core, board variant for the
chosen part). Gate Teensy-specific includes (`EEPROM.h`, IMXRT bits) behind
target defines. Verify the EVE library selects a working STM32 target from
the STM32duino define set — pin explicitly via build flag if auto-selection
misfires. Keep the Teensy env intact until U8 passes (the bench keeps
working during the port).
**Execution note:** run the host suite first and after every structural
move — it is the regression net for everything MCU-agnostic.
**Test scenarios:** host suite 11/11 (unchanged behavior); `pio run -e h743`
builds clean; flash usage reported < 2 MB with the full splash pack.
**Verification:** both envs build; host tests green.

### U5. EVE multi-panel port to dedicated SPI

**Goal:** The vendored library drives three panels via three SPI
peripherals instead of one shared bus.
**Requirements:** Hardware contract (dedicated SPI); KTD2, KTD6.
**Dependencies:** U4.
**Files:** `libraries/FT800-FT813/src/EVE_target/` (STM32 Arduino path),
`MustangDash/dash_panels.h`, `.ino` panel glue.
**Approach:** Extend the existing panel-descriptor pattern
(`EVE_select_panel`) so each descriptor carries its SPI instance
(`SPI`/`SPI2`/`SPI3` objects in Arduino terms) rather than only CS/PD pins.
Selection becomes trivial (no bus contention); the DMA-in-flight guard
logic simplifies or drops. Keep the descriptor API shape so `.ino` call
sites survive.
**Technical design (directional):** descriptor gains `SPIClass *bus`; the
target header's transmit primitives take the active descriptor's bus
instead of the global. Init stays at 8 MHz per panel then raises per panel
independently — per-panel clocks are now independently tunable.
**Test scenarios:** host suite still green (descriptor struct changes are
host-visible via `test_dash_panels.c` — extend it: three distinct bus
handles, selection returns the right handle, init-clock/run-clock fields
per panel).
**Verification:** builds for h743; panel test extended and green.

### U6. Peripheral firmware: USB VCP, FRAM odometer, telltales, switches

**Goal:** The new I/O works: serial protocol over USB CDC, odometer on
FRAM, telltales mirroring alarms, switches debounced.
**Requirements:** Hardware contract (USB-C, odometer, telltales, switches);
KTD4.
**Dependencies:** U4.
**Files:** `.ino` glue; `MustangDash/dash_odometer.h` (backend seam);
new `MustangDash/dash_telltales.h` (pure logic where possible);
`tests/` additions.
**Approach:** Serial: `Serial` maps to USB CDC on the STM32duino core —
the 115200 rate becomes nominal; protocol code unchanged. Odometer: keep
the two-slot ping-pong CRC record exactly; swap the storage primitive from
EEPROM API to FRAM I2C read/write behind a small backend interface.
Telltales: a pure mapping table alarm-state -> lamp mask (host-testable),
plus boot lamp-test (all on ~500 ms). Switches: debounce in the main loop,
trip-reset wired to the odometer's existing trip logic.
**Test scenarios:** odometer record round-trip + torn-write cases against a
mock FRAM backend (reuse existing odometer tests, swap the backend);
telltale mask: each alarm sets its lamp, oil-pressure lamp obeys the
rpm >= 500 gate, lamp-test mask = all-on; switch debounce: bounce train
yields one edge, held press yields one reset event.
**Verification:** new tests green in the host suite; `status` over USB CDC
acks on bench hardware (U8 completes this).

### U7. CAN bring-up to loopback

**Goal:** Both FDCAN peripherals initialized, external-loopback frames
verified — proving silicon, transceivers, and connectors.
**Requirements:** Hardware contract (2x CAN); CAN staging decision.
**Dependencies:** U4 (env), U3 (hardware, for the on-board half).
**Files:** `.ino` diag hooks; `MustangDash/dash_can.h` (init only).
**Approach:** STM32duino FDCAN library; classic CAN 500 kbps default (both
target devices speak classic; FD stays available). A `cantest` serial
command sends on bus 1, expects on bus 2 with the two buses wire-jumpered —
one command proves both transceivers, both connectors, and termination.
Decode explicitly out of scope.
**Test scenarios:** Test expectation: bench-only — `cantest` round-trip ok
across jumpered buses; silent-bus (no jumper) reports timeout, not hang.
**Verification:** `cantest` acks `ok` on the assembled board.

### Phase C — Bring-up

### U8. Board bring-up and parity validation

**Goal:** The assembled board runs the dash at parity; Definition of Done
met.
**Requirements:** all; Verification Contract.
**Dependencies:** U3, U5, U6, U7.
**Files:** `docs/solutions/` entries for anything hard-won (via
`/ce-compound`); `CLAUDE.md` hardware-truths update.
**Approach:** Staged: (1) power rails before any silicon stress — voltages,
ripple, backlight rail under load; (2) MCU alive — SWD, DFU enumerates,
VCP echoes; (3) panels one at a time — each panel was pre-validated on the
eval board, so `REG_ID != 0x7C` here indicts the board, not the panel;
(4) full dash — splash from provisioned flash, 60 fps, serial walk, odometer
power-cycle x10, telltale lamp-test, switch reset, `cantest`.
**Execution note:** follow the bench hazards already documented (FFC
down-side contact, beep BLGND continuity before applying backlight power).
**Test scenarios:** the Verification Contract below is this unit's checklist.
**Verification:** Verification Contract gates all pass.

---

## Verification Contract

- Host invariant suite green (all tests, including new odometer-backend,
  telltale, and panel-descriptor cases).
- `pio run -e h743` clean; flash < 2 MB including splash pack.
- Bench gates on the assembled board:
  - rails in spec under full backlight load
  - DFU enumerates; VCP carries the serial protocol (`status` acks)
  - `REG_ID == 0x7C` on all three panels via their dedicated buses
  - splash plays from RAM_G, staged from the firmware-embedded pack at boot (MCU-direct architecture, 2026-07-21 — panel flash unused)
  - dash renders at 60 fps (center), sides at their target rate
  - odometer value survives 10 power cycles on FRAM
  - telltale lamp-test at boot; oil-pressure lamp gated on rpm >= 500
  - trip-reset switch resets trip, odometer total unaffected
  - `cantest` round-trip across jumpered buses
- Per-panel fallback available throughout: any panel failing on the board
  re-validates on the eval board rig to split board-vs-panel in minutes.

## Definition of Done

All Verification Contract gates pass on the assembled board. CAN decode,
enclosure/mounting, and Teensy-carrier retirement cleanup are follow-on
work.

---

## Scope Boundaries

### Deferred to Follow-Up Work
- CAN message decode -> DashState (needs real ECU/PMU on the bench;
  protocols/DBCs pinned then).
- **Teensy strip-out (AFTER U8 passes, not before):** once the STM32 board
  is proven at parity, remove the Teensy path -- the `env:teensy41` build,
  `ARDUINO_TEENSY41` gates in the .ino, Teensy-specific comments, the
  Teensy pin story in `CLAUDE.md`'s hardware truths, and Teensy-era build
  docs. Until then the dual-target build IS the working bench. Scope
  guard: `docs/solutions/` history stays -- those learnings are records,
  not live instructions; the strip targets live code, config, and
  instruction files only.
- `MustangDash.ino` uncommitted bench diagnostics: revert or fold before
  U4 branches from a clean tree.
- Retire/archive the tracked EasyEDA Teensy-carrier project once the new
  board exists.

### Outside this plan
- Enclosure, mounting, loom/connector selection for the car install.
- Dual/quad SPI *firmware* (copper only, by decision).

## Open Questions (execution-time)

- **KTD1 RETARGET DECISION (Kevin, 2026-07-21): the carrier targets the
  dual-core STM32H755 — Cortex-M7 owns display/EVE, Cortex-M4 owns both
  CAN networks (ECU + PMU buses, dual FDCAN as already contracted).**
  This supersedes the H743VIT6 selection and the F767 alternative
  considered earlier the same day. Consequences accepted with the decision:
  - **KTD2 breaks.** STM32duino does not support dual-core H7; the M4 CAN
    core is a bare CubeHAL project by necessity and the M7 side likely
    follows. The Arduino glue path validated across the four PlatformIO
    envs does NOT carry to the H755 target. The pure headers (dash logic,
    odometer, telltales, serial protocol) are portable C and survive.
  - **PlatformIO remains the build driver (verified 2026-07-21):**
    `framework = stm32cube` instead of `arduino`; PIO ships
    `nucleo_h745zi_q` (the H755's crypto-less twin die), and a custom
    board JSON can name the H755 exactly (the riverdi_f469 pattern).
    Dual core = two envs (CM7 + CM4 images). Crucially the EVE library
    ships a NATIVE HAL H7 target -- EVE_target_STM32H7.h, selected by
    plain `#if defined(STM32H7)`, no Arduino -- so the display driver
    carries to bare-metal for free. Known patch: the per-panel bus
    routing added today lives in the Arduino wrapper; the HAL target
    needs its equivalent (per-panel SPI_HandleTypeDef resolver, same
    pattern as eve_spi()).
  - **Adoption strategy: M7-only first.** Boot with the M4 parked (option
    bytes), making the H755 behave like an H743 — single-core firmware
    lands first, dual-core (shared-SRAM + HSEM inter-core CAN handoff)
    comes as a later increment, never a big-bang.
  - **Mule: NUCLEO-H755ZI-Q.** Note the -Q boards are SMPS-powered: the
    power-supply config in clock init differs from LDO boards, a classic
    hang-at-boot trap for generic configs.
  - **Exact part pinned (Kevin, 2026-07-21): STM32H755ZIT6, LQFP-144 —
    JLC C730212, $27.25, stock 43 at pin time.** Stock is shallow:
    re-check immediately before the U3 order (the same discipline KTD1
    already required for the H743).
  - **Carrier power design: prefer LDO mode** (pins strapped per
    datasheet, no SMPS inductor) — full 480 MHz available, firmware
    init identical in shape to H743-class code, one less silent-hang
    trap at bring-up. SMPS only if a U1 thermal calc demands it. The
    NUCLEO-H755ZI-Q mule is SMPS-configured, so its board init needs
    the SMPS supply-config line -- a known one-liner, not a porting
    signal for the carrier.
  - **Verify at U1/U2, do not assume:** whether H743/H753 LQFP144
    pinouts are close enough to share the carrier footprint (would
    allow a single-core fallback fit given the shallow H755 stock).
  - The single-core H743/H753 remains the documented fallback if dual-core
    costs prove out worse than expected — the M7-only-first strategy keeps
    that door open.
- Crystal vs HSI for USB clocking — resolve in U2 (crystal is the safe
  default; confirm footprint).
- Exact regulator parts (U1 owns, via JLC search with measured backlight
  current as input).
- FFC run lengths in the car (Deferred; affects nothing until install).
