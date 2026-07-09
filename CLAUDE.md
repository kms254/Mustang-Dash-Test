# CLAUDE.md — working notes for Mustang-Dash-Test

Durable, hard-won context for this repo so future sessions start ahead.
Update this file whenever a task uncovers something non-obvious.

## What this project is

First-light / bring-up firmware for a **Riverdi SM-RVT70HSBNWN00** (7" 1024×600
IPS, **BT817 / EVE4**, no touch) on a **Teensy 4.1**, using the
RudolphRiedel **FT800-FT813** (EmbeddedVideoEngine) library, vendored in
`libraries/FT800-FT813`.

- Sketch: `MustangDash/MustangDash.ino`
- Build: `./scripts/compile.sh` → `teensy:avr:teensy41`, `--libraries ./libraries`

## Hardware truths (don't re-derive)

- Pins: SCLK=13, MISO=12, MOSI=11, **CS=14**, **PD/RST=17**. INT not wired → poll.
- Panel logic on 3.3 V, backlight on external 5 V, shared ground.
- SPI: mode 0, MSB-first, **≤ 11 MHz during EVE_init()** (we use 8 MHz and stay there).

## Library gotchas (verified against the headers)

- **Profile is `EVE_RVT70H`**, NOT `EVE_RiTFT70`. `EVE_RVT70H` = 1024×600 BT817
  (EVE_GEN 4); `EVE_RiTFT70` = 800×480 BT81x. Set in `src/EVE_config.h` right
  after the include guard (works in the Arduino IDE without `-D` flags).
- Pins live in `src/EVE_target/EVE_target_Arduino_Teensy4.h` (guarded
  `#if !defined`, so `-D EVE_CS=` / `-D EVE_PDN=` still override). Target is
  auto-selected from the `ARDUINO_TEENSY41` compiler define.
- Upstream ships only a PlatformIO `library.json`. We added `library.properties`
  so the Arduino IDE / arduino-cli use the `src/` layout. Keep it.
- API names actually used (confirmed in `EVE_commands.h` / `EVE.h`):
  `EVE_init()` (returns `E_OK`==0), `EVE_memRead8(REG_ID)` (BT817 → **0x7C**),
  `EVE_memWrite8(REG_PWM_DUTY, ...)`, `EVE_cmd_dl`, `EVE_color_rgb`,
  `EVE_cmd_text(x, y, font, EVE_OPT_CENTER, "...")`, `EVE_execute_cmd()`.
  Font 31 is the largest built-in ROM font. Read headers before adding calls.
- `EVE_DMA` is enabled for Teensy 4, but plain (non-`_burst`) display-list calls
  transfer directly over SPI — fine for first light.

## Build environment (this cloud sandbox only)

The web sandbox **blocks `pjrc.com` and `downloads.arduino.cc`** (egress 403), so
the normal Teensyduino install is impossible here. A minimal offline
`teensy:avr:teensy41` platform was assembled — see `BUILD.md` and
`tools/teensy-avr-platform/`. Key points if you need to rebuild it:

- `arduino-cli`: `go build` from the github clone (a plain `go install` fails on
  its `replace` directives).
- ARM toolchain: system `gcc-arm-none-eabi` (`apt`) — it *does* have the
  `thumb/v7e-m+dp/hard` multilib (Cortex-M7 hard-float), so it links Teensy 4.1.
- Core + SPI: `git clone` PaulStoffregen/cores (`teensy4/`) and PaulStoffregen/SPI.
- `.ino` prototype generation uses a **no-op `ctags` shim** (the arduino-ctags
  fork isn't downloadable offline). Consequence: **declare function prototypes
  in `.ino` files before use** — stock exuberant/universal ctags don't emit the
  return type, so real auto-prototyping is unavailable here.
- The `Error initializing instance:` index/discovery lines from arduino-cli are
  harmless offline noise.

On a normal workstation, just install Teensyduino and ignore all of the above.

## Verified state

`arduino-cli compile -b teensy:avr:teensy41 --libraries ./libraries ./MustangDash`
succeeds clean: 31,740 B flash (0%), 37,888 B RAM (7%), no warnings.
Upload was NOT exercised here (no board attached); flash via Arduino IDE.

## Knowledge store

- `docs/solutions/` — documented solutions to past problems (best practices,
  bugs, workflow patterns), organized by category with YAML frontmatter
  (`module`, `tags`, `problem_type`). Relevant when implementing or debugging
  in documented areas.
- `CONCEPTS.md` — shared domain vocabulary (entities, named processes, status
  concepts). Relevant when orienting to the codebase.
