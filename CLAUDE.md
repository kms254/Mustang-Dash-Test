# CLAUDE.md — working notes for Mustang-Dash-Test

Durable, hard-won context for this repo so future sessions start ahead.
Update this file whenever a task uncovers something non-obvious.

## What this project is

First-light / bring-up firmware for a **Riverdi SM-RVT70HSBNWN00** (7" 1024×600
IPS, **BT817 / EVE4**, no touch) on a **Teensy 4.1**, using the
RudolphRiedel **FT800-FT813** (EmbeddedVideoEngine) library, vendored in
`libraries/FT800-FT813`.

- Sketch: `MustangDash/MustangDash.ino` (setup/loop/glue) — the renderers live
  in `MustangDash/dash_render.h` (dash) and `MustangDash/splash_render.h`
  (splash), single-TU headers included only by the `.ino`; they are EVE-bound,
  **not** pure headers, and not host-tested
- Build: `./scripts/compile.sh` → `teensy:avr:teensy41`, `--libraries ./libraries`
  (also syncs `tools/teensy-avr-platform/` into the sketchbook first — the
  tracked files are the source of truth; the sync self-skips on a real
  Teensyduino install, so it is safe to run anywhere)
- Build (VS Code): `platformio.ini` drives the PlatformIO Build/Upload/Monitor
  buttons. Parallel path to `compile.sh`, not a replacement — see BUILD.md.
- Tests: `./tests/run-tests.sh` — host-side invariant tests pinning the display
  profile, wiring pins, backlight wave, splash timeline, dash math/sim/serial/
  odometer logic, font-format invariants, the splash flash-pack layout, and
  the ctags-shim contract. Run them after touching `EVE_config.h`, the Teensy4
  target header, any `MustangDash/*.h` pure header, or the platform files.
  Needs host `gcc`; Git Bash has none, so **on Windows run
  `wsl -- bash -lc "./tests/run-tests.sh"`** (or the VS Code task
  "Tests: invariant suite"). All 11/11 pass.
- Boot splash: a 2000 ms animated splash (spec vendored in `assets/splash/`)
  plays at power-up, then crossfades directly into the dash. Splash assets are
  **ASTC bitmaps in the panel's 64 MB QSPI flash**, rendered direct from flash
  (zero RAM_G): `tools/make_splash_flash.py` (astcenc pinned by
  `tools/get-astcenc.sh`, WSL) emits `splash_flash.h`; the firmware provisions
  the panel flash once at boot when the pack CRC differs — sector 0 (the
  vendor flashfast blob) is never written. Theme stays build-time via
  `SPLASH_THEME` in `MustangDash/splash_config.h`.
- Dash: TRACK/STREET screens per the vendored design handoff
  (`assets/dash-design/`), all-procedural at ~60 fps with custom EVE bitmap
  fonts (`tools/make_dash_fonts.py` → `dash_fonts.h`, ~273 KB — RAM_G's only
  tenant). Data flows simulator → `DashState` channels (validity bitmask) →
  renderers; the serial protocol (115200; `ok`/`err` acks are the ONLY output
  after boot) overrides any channel — the `/dash` skill wraps it. Odometer
  persists in Teensy EEPROM (CRC8 record). Alarm takeover preempts both
  modes; the oil-pressure alarm is gated on rpm ≥ 500 (engine running).

## Hardware truths (don't re-derive)

- Bus pins: SCLK=13, MISO=12, MOSI=11, shared by all three panels. Per-panel
  CS / PD-RST: **center 14/17, left 15/20, right 16/21** (`dash_panels.h`).
  INT not wired → poll.
- Panel logic on 3.3 V, backlight on external 5 V, shared ground.
- SPI: mode 0, MSB-first, **≤ 11 MHz during every panel's EVE_init()** (we init
  at 8 MHz), then one bus-wide "raise" to `DASH_SPI_RUN_HZ` — **8 MHz, bench-
  verified**. 24 MHz failed read AND write integrity on this wiring
  (2026-07-10: white screen, flash init 0x01, all font inflates failed,
  fps 25 with faults=0). Walk it up only via U9's read-integrity soak.

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

## Build environment (Kevin's Windows workstation)

Nothing above applies here: network is open, and real Teensyduino is installed.

- Teensyduino via Arduino IDE Board Manager: `teensy:avr` **1.62.0**,
  `teensy-compile` 15.2.1. Sketchbook (`Documents/Arduino`) is empty, which is
  fine — `compile.sh` passes `--libraries ./libraries` explicitly.
- `arduino-cli` **1.5.1** on PATH (`winget install ArduinoSA.CLI`), at
  `C:\Program Files\Arduino CLI\`. It defaults to the same `%LOCALAPPDATA%\Arduino15`
  data dir the IDE uses, so it inherits the Teensy platform for free.
- Arduino IDE also bundles its own `arduino-cli` at
  `…/Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe`.
  Works, but lives inside the IDE install tree — prefer the PATH one.
- PlatformIO core lives at `~/.platformio/penv`. It was bootstrapped from the
  **portable Python the VS Code extension ships predownloaded**
  (`assets/predownloaded/python-portable-windows_amd64-*.tar.gz`) — there is no
  system Python on this box, only the Microsoft Store alias stub, so do not
  reach for `python -m pip`.
- PlatformIO's `teensy41` board defines `ARDUINO_TEENSY41` (verified via
  `pio run -t envdump`), so the EVE target header auto-selects correctly. It
  resolves `framework-arduinoteensy 1.162.0` + `toolchain-…-teensy 15.2.1`,
  matching Teensyduino 1.62.0 — hence byte-identical output to `compile.sh`.
- PIO's `.ino` → `.cpp` conversion does its own prototype generation, so the
  sketch's explicit prototypes are redundant here but harmless. Keep them: the
  offline arduino-cli path still depends on them.
- **CRLF vs WSL.** `core.autocrlf=true` is set globally, so `.sh` files check out
  with CRLF. Git Bash silently tolerates the `\r`; WSL's bash does not
  (``/usr/bin/env: 'bash\r'``). `.gitattributes` pins `*.sh` to `eol=lf`.
  If a shell script suddenly fails only under WSL, suspect line endings first.
- WSL2 `Ubuntu` (20.04, gcc 9.4.0) is installed and is the default distro; it is
  how the host-side tests run on this box. `wsl` inherits the Windows cwd.
- Windows has **no host C compiler at all** (no gcc/clang/cl/MSYS2/MinGW).
  Don't write tooling that assumes one; use WSL.

## Verified state

On the **offline sandbox** platform, `arduino-cli compile -b teensy:avr:teensy41
--libraries ./libraries ./MustangDash` succeeded clean: 53,244 B flash (0%),
59,232 B RAM (11%), no warnings (full-newlib link; the earlier nano.specs build
measured 31,740 / 37,888).

On the **Windows workstation with real Teensyduino 1.62.0** (2026-07-08), both
`./scripts/compile.sh` and `pio run` succeed clean and agree exactly:

```
FLASH: code:42192, data:7448, headers:8724   (pre-splash, 2026-07-08)
FLASH: code:45584, data:205700, headers:8808 (embedded-PNG splash era, 2026-07-09)
FLASH: code:~70300, data:~690900, headers:~8900 (dash + flash-splash, 2026-07-09)
RAM1:  variables:~17000, code:~67700
RAM2:  variables:12416
```

The dash-era `data` (~675 KB) is the embedded panel-flash provisioning pack
(~644 KB of ASTC splash assets, all three themes) plus the zlib font glyphs;
the older ~206 KB embedded-PNG figures describe a deleted architecture and are
kept only as history. Do not expect the sandbox's numbers to match — different
toolchain, different libc. The two *workstation* paths agreeing byte-for-byte
is the invariant worth watching (re-confirmed for the dash build).

**Hardware-verified (2026-07-09): FIRST LIGHT CONFIRMED** (original bring-up;
the HELLO MUSTANG / pony screens it referenced are since replaced by the dash).
Upload via `pio run -t upload` (Teensy Loader) works; `teensy_reboot.exe` + a
raw COM4 read (115200) captures the boot banner without an interactive monitor.
On the real panel: `EVE_init()` returned `E_OK`, **`REG_ID == 0x7C` observed**,
rendering at 8 MHz SPI, backlight under `REG_PWM_DUTY` control. Dash-era bench
facts (2026-07-09): 64 MB QSPI flash detected on the panel, one-time splash
provisioning + CRC no-op reboot path verified, 60 fps sustained, serial AE walk
acked, odometer persistence across power cycles verified.
Bring-up hazards actually hit on this bench (in symptom order): a damaged FFC
end shorting pins 1-2 (VDD-GND -> Teensy won't enumerate on USB), and a flaky
Teensy micro-USB cable that perfectly mimicked a dead board. Bench rules that
came out of it: pins 19-20 (BLGND) beep to pin 2 (GND) — use that continuity
to positively identify the backlight end (17-20) before applying 5 V; the FFC
is down-side contact at the panel; the panel survives being driven with no
5 V on BLVDD (renders, just dark).

## Knowledge store

- `docs/solutions/` — documented solutions to past problems (best practices,
  bugs, workflow patterns), organized by category with YAML frontmatter
  (`module`, `tags`, `problem_type`). Relevant when implementing or debugging
  in documented areas.
- `CONCEPTS.md` — shared domain vocabulary (entities, named processes, status
  concepts). Relevant when orienting to the codebase.

## CE workflow (Every compound-engineering skills, .claude/skills/)

The loop: **Scope → Plan → Build → Review → Ship → Learn.**

1. `/ce-brainstorm` — fuzzy idea → requirements
2. `/ce-plan` — requirements → implementation plan (docs/plans/)
3. `/ce-work` — execute the plan
4. `/ce-code-review` — before every PR
5. `/ce-commit` / `/ce-commit-push-pr` — commits ALWAYS go through these
   skills, never hand-rolled `git commit` (`/ce-resolve-pr-feedback` for
   review comments)
6. `/ce-compound` — bank hard-won learnings into docs/solutions/

`/lfg` chains 2-5. Situational: `/ce-debug` (bugs), `/ce-simplify-code`
(cleanup), `/ce-pov` (adopt-or-not verdicts), `/ce-compound-refresh` (stale
learnings), `/ce-worktree` (isolated experiments). Step 6 feeds step 1 —
that's the compounding.
