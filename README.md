# Mustang-Dash-Test

First light for a **Riverdi 7" EVE4 display** driven by a **Teensy 4.1** over SPI.

| Item      | Value |
|-----------|-------|
| Display   | Riverdi **SM-RVT70HSBNWN00** — 7.0" **1024 × 600** IPS, **BT817** (EVE4), no touch |
| MCU       | Teensy 4.1 (IMXRT1062) |
| Library   | [RudolphRiedel/FT800-FT813](https://github.com/RudolphRiedel/FT800-FT813) (EmbeddedVideoEngine) v5.0.10, branch `5.x`, vendored in [`libraries/`](libraries/) |
| Profile   | `EVE_RVT70H` (1024×600, BT817, `EVE_GEN 4`) |

## Wiring

Shared ground. Panel logic VDD on Teensy **3.3 V**. Backlight on an external **5.0 V** supply.

| Signal   | Teensy 4.1 pin |
|----------|----------------|
| SCLK     | 13 (SPI0 SCK)  |
| MISO     | 12 (SPI0 MISO) |
| MOSI     | 11 (SPI0 MOSI) |
| CS       | **14**         |
| PD / RST | **17**         |
| INT      | not connected (polling only) |

## What the firmware does

[`MustangDash/MustangDash.ino`](MustangDash/MustangDash.ino):

1. Sets `CS`/`PDN` as outputs, brings up SPI0 at **8 MHz, mode 0, MSB-first**
   (BT817 requires ≤ 11 MHz until it is configured).
2. Runs `EVE_init()` and prints the result code over Serial.
3. Reads `REG_ID` (a healthy BT817 returns **0x7C**) so you can confirm the SPI
   link from the serial monitor.
4. Writes `REG_PWM_DUTY = 128` to turn the backlight on (~50%).
5. Draws one display list: dark background, **"HELLO MUSTANG"** centred in a
   large ROM font (font 31) with a smaller **"EVE4 first light"** line under it
   (font 27).
6. In `loop()` slowly pulses `REG_PWM_DUTY` between **20 and 128** (a ~1 s
   triangle sweep) so the render loop and software dimming are visibly working.

Serial is **115200** 8N1.

## Configuration (already applied in the vendored library)

- **Profile** — `EVE_config.h` defines `EVE_RVT70H` near the top of the file
  (active for both the library and the sketch, and works from the Arduino IDE
  without needing `-D` build flags). This is the *1024×600 BT817* Riverdi
  profile — note it is **not** `EVE_RiTFT70`, which is the 800×480 BT81x panel.
- **Pins** — `libraries/FT800-FT813/src/EVE_target/EVE_target_Arduino_Teensy4.h`
  defaults are set to `EVE_CS = 14` and `EVE_PDN = 17` (still overridable with
  `-D` build flags). The library auto-detects the Teensy 4.x target from the
  `ARDUINO_TEENSY41` compiler define and uses the standard `SPI` object.
- An Arduino `library.properties` was added so the Arduino IDE / arduino-cli
  pick up the `src/` layout (upstream ships only a PlatformIO `library.json`).

## Build & upload

### Arduino IDE (recommended — this is how you flash the board)

Copy `libraries/FT800-FT813` into your Arduino sketchbook `libraries/` folder
(or symlink it), open `MustangDash/MustangDash.ino`, choose **Tools → Board →
Teensy 4.1**, **USB Type: Serial**, then click **Upload** (Teensy Loader flashes
it automatically).

### arduino-cli

With a Teensy platform installed under `teensy:avr:teensy41`:

```bash
./scripts/compile.sh          # compiles to ./build/MustangDash.ino.hex
```

or directly:

```bash
arduino-cli compile -b teensy:avr:teensy41 --libraries ./libraries ./MustangDash
```

See [`BUILD.md`](BUILD.md) for how the compile was verified in this repo's
sandbox (where the normal PJRC downloads were unavailable).

## Expected result

- **Serial monitor (115200):** the banner, the pins/panel line,
  `Running EVE_init()... returned 0x00 (E_OK = 0x00)`,
  `REG_ID = 0x7C (expected 0x7C)`, then
  `EVE init OK: display list sent, backlight on. Pulsing now.`
- **Panel:** a near-black screen with white **"HELLO MUSTANG"** and a grey
  **"EVE4 first light"** below it; the backlight gently pulses.

### Troubleshooting (three most likely failure modes)

| Symptom on Serial | Likely cause |
|---|---|
| `EVE_init() did NOT return E_OK` | EVE never came out of reset — check `PDN` (pin 17), 3.3 V logic supply, and `CS` (pin 14). |
| `EVE_init()` OK but `REG_ID = 0x00`/`0xFF` (not `0x7C`) | SPI link problem — MISO/MOSI swapped, SCLK not on 13, wrong CS, or clock too fast. |
| Init OK and `REG_ID = 0x7C` but the panel stays dark | Backlight power (external 5 V) or `REG_PWM_DUTY` — the panel is alive but not lit. |
