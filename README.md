# Mustang-Dash-Test

Firmware for a **Riverdi EVE4 instrument cluster** — a 7" centre panel with two
5" side panels on a shared SPI bus, driven by an **STM32**.

| Item      | Value |
|-----------|-------|
| Display   | Riverdi **SM-RVT70HSBNWN00** — 7.0" **1024 × 600** IPS, **BT817** (EVE4), no touch. Side panels are RVT50H. |
| MCU       | STM32 — NUCLEO-F767ZI three-panel mule; STM32H755 on the custom carrier |
| Library   | [RudolphRiedel/FT800-FT813](https://github.com/RudolphRiedel/FT800-FT813) (EmbeddedVideoEngine) v5.0.10, branch `5.x`, vendored in [`libraries/`](libraries/) |
| Profile   | `EVE_RVT70H` (1024×600, BT817, `EVE_GEN 4`) |

## Wiring

Shared ground, panel logic on **3.3 V**, backlight on an external **5.0 V**
supply (a dev board's USB rail cannot feed three backlights).

SCLK, MISO and MOSI are shared by all three panels; each panel gets its own CS
and PD/RST. The per-panel pin table lives in
[`MustangDash/dash_panels.h`](MustangDash/dash_panels.h). `INT` is not wired —
the firmware polls.

**SPI is mode 0, MSB-first, ≤ 11 MHz during every panel's `EVE_init()`**, then
the bus rises once to the run clock. On the F767 the clock is
prescaler-quantized (6.75 / 13.5 / 27 / 54 MHz, requests round *down*); 13.5 MHz
is the proven operating point on two panels, and 27 MHz hard-wedges the
firmware rather than degrading gracefully.

## What the firmware does

A 2000 ms animated boot splash (ASTC assets embedded in the MCU image and staged
to RAM_G) crossfades directly into the dash, which renders TRACK and STREET
screens procedurally at ~60 fps with custom EVE bitmap fonts.

Data flows simulator → `DashState` channels → renderers. The serial protocol
(115200 8N1) overrides any channel; the odometer persists across power cycles.
A short press on the USER button toggles TRACK/STREET, a ≥1 s hold resets the
trip.

## Build

See [`BUILD.md`](BUILD.md) for the environments and the flash budget.

```bash
./scripts/compile.sh                    # PlatformIO, default env nucleo_f767
wsl -- bash -lc "./tests/run-tests.sh"  # host invariant tests, no board needed
```

The tests pin what must not silently change: the `EVE_RVT70H` profile, the
control pins, the backlight sweep, the splash timeline, the dash
math/sim/serial/odometer logic, the font format, the flash-pack layout, the
button gestures and the telltale calibration.

## Configuration (already applied in the vendored library)

- **Profile** — `EVE_config.h` defines `EVE_RVT70H` near the top of the file,
  so it applies without `-D` flags. Note it is **not** `EVE_RiTFT70`, which is
  the 800×480 BT81x panel — picking that one initialises fine and renders
  garbage. See [the profile-selection
  learning](docs/solutions/best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md).
- **Pins** — the STM32 target header carries the defaults (`EVE_CS`, `EVE_PDN`),
  still overridable with `-D`. Per-panel CS/PD come from `dash_panels.h` at
  runtime.
- An Arduino `library.properties` was added so the `src/` layout is picked up
  (upstream ships only a PlatformIO `library.json`).

## Troubleshooting

| Symptom on Serial | Likely cause |
|---|---|
| `EVE_init() did NOT return E_OK` | EVE never came out of reset — check PD/RST, the 3.3 V logic supply, and CS. |
| Init OK but `REG_ID = 0x00`/`0xFF` (not `0x7C`) | SPI link — MISO/MOSI swapped, wrong CS, or the clock too fast. `0x00` and `0xFF` distinguish a floating line from a stuck one. |
| Init OK and `REG_ID = 0x7C` but the panel stays dark | Backlight power or `REG_PWM_DUTY`. The panel is alive but unlit — it renders fine with no 5 V on BLVDD, just invisibly. |
| Init OK, backlight lit, image garbled or wrong geometry | Wrong display profile in `EVE_config.h`. |

More hard-won bring-up detail — FFC orientation, identifying the backlight end
by continuity, and why a dead-looking board is usually a cable — is in
[`docs/solutions/`](docs/solutions/).
