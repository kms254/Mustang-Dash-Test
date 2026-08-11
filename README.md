# Mustang-Dash-Test

Firmware **and carrier hardware** for a **Riverdi EVE4 instrument cluster** —
a 7" centre panel with two 5" side panels, each on its **own dedicated SPI
peripheral**, driven by an **STM32H755**.

| Item      | Value |
|-----------|-------|
| Displays  | Centre: Riverdi **SM-RVT70HSBNWN00** — 7.0" **1024 × 600** IPS, **BT817** (EVE4), no touch. Sides: RVT50H. |
| MCU       | **STM32H755ZIT6** on the in-repo carrier (**Board3**, ordered from JLCPCB 2026-08). Bench mules: NUCLEO-H755ZI-Q (same die) and NUCLEO-F767ZI (proven three-panel rig). |
| Library   | [RudolphRiedel/FT800-FT813](https://github.com/RudolphRiedel/FT800-FT813) (EmbeddedVideoEngine) v5.0.10, branch `5.x`, vendored in [`libraries/`](libraries/) with multi-panel / per-panel-SPI-bus support |
| Profile   | `EVE_RVT70H` (1024×600, BT817, `EVE_GEN 4`) |

## Repo layout

| Path | What it is |
|---|---|
| [`MustangDash/`](MustangDash/) | The firmware sketch plus pure, host-tested headers (sim, serial, odometer, button, telltales) |
| [`kicad/board3/`](kicad/board3/) | The carrier board — schematic, routed PCB, footprint libraries. DRC/ERC 0 at `--severity-all`; reviewed pre-order by seven independent lenses |
| [`fab/`](fab/) | The JLCPCB package: gerbers/BOM/CPL plus [`ORDER.md`](fab/ORDER.md), the order-form contract every figure of which re-derives from the tree |
| [`tools/`](tools/) | The KiCad pipeline: `kicad_lcsc.py` (sourcing), `kicad_fab.py` (fab package), `kicad_verify.py` (the DRC gate), silk/routing/verification tooling |
| [`docs/hardware/`](docs/hardware/) | [Bring-up card](docs/hardware/board3-bringup-card.md) (print before the boards arrive) and the [as-built H755 pin map](docs/hardware/board3-h755-pin-map.md) |
| [`docs/solutions/`](docs/solutions/) + [`CONCEPTS.md`](CONCEPTS.md) | Documented learnings (YAML frontmatter: `module`, `tags`, `problem_type`) and the shared domain vocabulary |

## Wiring

Shared ground, panel logic on **3.3 V**, backlight on an external **5.0 V**
supply (a dev board's USB rail cannot feed three backlights).

**Each panel has its own SPI peripheral** — on Board3: centre SPI1
(PA5/6/7, CS PD8, PD PD11), left SPI2 (PB13/14/15, CS PD9, PD PD12), right
SPI4 (PE2/5/6, CS PE3, PD PD13) — plus per-panel CS and PD/RST. Panel timings
live in [`MustangDash/dash_panels.h`](MustangDash/dash_panels.h); per-target
pins in the sketch's board blocks and the
[pin map](docs/hardware/board3-h755-pin-map.md). `INT` is not wired — the
firmware polls.

**SPI is mode 0, MSB-first, ≤ 11 MHz during every panel's `EVE_init()`**, then
each bus rises once to the run clock. 13.5 MHz is the proven operating point on
the F767 bench (27 MHz hard-wedges the firmware rather than degrading
gracefully) — but clock numbers come from failure, not derivation, and **do not
transfer across copper**: re-walk the clock on each new board, per the bring-up
card.

## What the firmware does

A 2000 ms animated boot splash (ASTC assets embedded in the MCU image and
staged to RAM_G with per-asset readback checks) crossfades into the dash, which
renders TRACK and STREET screens procedurally at ~60 fps with custom EVE bitmap
fonts.

Data flows simulator → `DashState` channels → renderers; CAN decoders will fill
the same channels later. The serial protocol (115200, USB-CDC on Board3;
`ok`/`err` acks) overrides any channel; the odometer persists across power
cycles. A short press on the gesture button (BTN1 on Board3, the USER button on
the mules) toggles TRACK/STREET; a ≥1 s hold resets the trip. Telltales drive
eight LEDs through two AW9523B expanders on I2C2; the boot banner self-checks
the panels, the expanders, and the QSPI NOR's JEDEC ID.

## Build

See [`BUILD.md`](BUILD.md) for the environments and the flash budget.

```bash
./scripts/compile.sh                    # PlatformIO, default env nucleo_f767
./scripts/compile.sh board3             # the carrier (25 MHz crystal clock tree)
./scripts/compile.sh board3_mule        # same firmware on the NUCLEO-H755ZI-Q
wsl -- bash -lc "./tests/run-tests.sh"  # host invariant tests, no board needed
```

**H755 targets, one-time first connect:** the factory chip boots both cores and
the CM4 has no image — run `STM32_Programmer_CLI -c port=SWD -ob BCM4=0` once.
The mule env is SMPS-aware (flashing plain `board3` onto the -Q Nucleo hangs at
the VOSRDY wait); details and the session matrix are on the
[bring-up card](docs/hardware/board3-bringup-card.md).

The host tests pin what must not silently change: the `EVE_RVT70H` profile, the
control pins, the backlight sweep, the splash timeline, the dash
math/sim/serial/odometer logic, the font format, the flash-pack layout, the
button gestures and the telltale calibration.

## Configuration (already applied in the vendored library)

- **Profile** — `EVE_config.h` defines `EVE_RVT70H` near the top of the file,
  so it applies without `-D` flags. Note it is **not** `EVE_RiTFT70`, which is
  the 800×480 BT81x panel — picking that one initialises fine and renders
  garbage. See [the profile-selection
  learning](docs/solutions/best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md).
- **Multi-panel** — the STM32 target header carries `EVE_MULTI_PANEL` and a
  per-panel descriptor (`EVE_panel_t`) with an optional per-panel `SPIClass*`
  bus; the sketch selects the active panel at runtime.
- An Arduino `library.properties` was added so the `src/` layout is picked up
  (upstream ships only a PlatformIO `library.json`).

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Board looks completely dead | Hold **BOOT** + tap **RESET** → a DFU device on USB proves MCU, power and the USB path with zero tools. Then walk the [bring-up card](docs/hardware/board3-bringup-card.md); on this bench a "dead board" has historically been a cable or connector, not silicon. |
| `EVE_init() did NOT return E_OK` | EVE never came out of reset — check PD/RST, the 3.3 V logic supply, and CS. |
| Init OK but `REG_ID = 0x00`/`0xFF` (not `0x7C`) | SPI link — MISO/MOSI swapped, wrong CS, or the clock too fast. `0x00` and `0xFF` distinguish a floating line from a stuck one. |
| Init OK and `REG_ID = 0x7C` but the panel stays dark | Backlight power or `REG_PWM_DUTY`. The panel is alive but unlit — it renders fine with no 5 V on BLVDD, just invisibly. |
| Init OK, backlight lit, image garbled or wrong geometry | Wrong display profile in `EVE_config.h`. |

More hard-won detail — FFC orientation, identifying the backlight end by
continuity, why instruments that answer confidently deserve suspicion — is in
[`docs/solutions/`](docs/solutions/), organised by category with searchable
frontmatter.
