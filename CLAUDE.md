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
  odometer logic, font-format invariants, the splash flash-pack layout, the
  trip/mode button gestures, and the ctags-shim contract. Run them after
  touching `EVE_config.h`, the Teensy4 target header, any `MustangDash/*.h`
  pure header, or the platform files.
  Needs host `gcc`; Git Bash has none, so **on Windows run
  `wsl -- bash -lc "./tests/run-tests.sh"`** (or the VS Code task
  "Tests: invariant suite"). All 15/15 pass (2026-08-03).
- Boot splash: a 2000 ms animated splash (spec vendored in `assets/splash/`)
  plays at power-up, then crossfades directly into the dash. Splash assets are
  **ASTC bitmaps embedded in the firmware image** (`tools/make_splash_flash.py`,
  astcenc pinned by `tools/get-astcenc.sh`, WSL, emits `splash_flash.h`) and
  staged MCU flash -> RAM_G at boot with a 16-byte readback spot-check per
  asset; a failed check skips that asset for the session — no fallback. The
  panel's QSPI flash is NOT used by boot: the provision-then-stage path was
  deleted 2026-07-21 (direct-from-flash render hits a per-frame bandwidth
  ceiling above ~40 KB per asset, see
  `docs/solutions/architecture-patterns/bt817-flash-render-streaming-bandwidth-ceiling.md`,
  and the pack ships embedded either way). Theme stays build-time via
  `SPLASH_THEME` in `MustangDash/splash_config.h`.
- Dash: TRACK/STREET screens per the vendored design handoff
  (`assets/dash-design/`), all-procedural at ~60 fps with custom EVE bitmap
  fonts (`tools/make_dash_fonts.py` → `dash_fonts.h`, ~324 KB — RAM_G's only
  tenant). Data flows simulator → `DashState` channels (validity bitmask) →
  renderers; the serial protocol (115200; `ok`/`err` acks are the ONLY output
  after boot, with one documented exception: `flashwipe really` prints a
  pre-erase warning line) overrides any channel — the `/dash` skill wraps it. Odometer
  persists in Teensy EEPROM (CRC8 record). Alarm takeover preempts both
  modes; the oil-pressure alarm is gated on rpm ≥ 500 (engine running).
- TRACK simulation (2026-07-22): a **physically-modelled lap of High Plains
  Raceway**, not a driving cycle. Lap position integrates road speed over a
  16-segment distance-keyed table; acceleration is traction/power/drag from the
  real spec (511 whp measured in Denver, 2900 lb, 315/30R18 square, T56 Magnum,
  3.73); corners carry an arc derived from their limit speed and lateral grip,
  with per-corner turn angles. **Lap time is an output, not an input** — 1:59.7
  at `SIM_DRIVER_SKILL` 0.86 (was 2:01.6 before U12's corner exits), front
  straight ~170 mph, peak 5th gear. Non-obvious traps, all found the hard way:
  corner arc radius must come from
  the *authored* limit, not the skill-scaled one (otherwise a slower driver gets
  a shorter corner and skill loses its leverage), and segments flagged
  `is_corner_limit == false` (the front straight, T12) must be exempt from both
  skill scaling and lookahead braking.
  **U12 (2026-07-25) added the corner EXIT, and it took two mechanisms, not
  one.** A corner used to be a constant-radius arc that just *ended*, so lateral
  demand went from everything to nothing between two steps and the car took full
  thrust instantly — visible on glass as a throttle that slammed open. Fixing it
  needs (1) the arc to **unwind** past `SIM_CORNER_APEX_FRAC` (0.85; lateral
  tapers to zero and the speed ceiling rises as `v_lim/sqrt(lat)`, so the arc's
  end is continuous) *and* (2) a **rate-limited pedal**
  (`SIM_THROTTLE_ROLLON_S` 0.7 s, `DashSimState.throttle_frac`, free on the way
  down). (1) alone is not enough: `sqrt(1 - used^2)` is near-vertical as load
  comes off, so the pedal still hit full travel in ~50 ms. **The apex fraction
  is calibrated against lap time** — unwinding from mid-arc (0.5) hands the car
  half of every corner and ran 1:52.4. Thrust comes from the slewed pedal, never
  a display filter: the original defect was pedal channels describing a car the
  physics wasn't driving. Consequence to expect: any test probing U2's
  traction/power model must set `throttle_frac = 1.0f` first, or it measures the
  roll-on and calls it the car's traction limit. Runs 20-minute **sessions** that end on
  the first lap completion past the mark — never mid-lap. Coolant and oil use
  separate time constants (120 s / 600 s) so oil is still climbing at minute 15;
  fuel burns ~0.62 gal/lap (`DASH_LAP_BURN_GAL` 0.63, rounded pessimistic since
  it backs the LAPS LEFT promise) and deliberately does *not* reset with the session,
  so tank and thermal cycles run on different periods. `circuit hpr|sweep`
  selects a range-sweep fixture that exercises the full speedo dial, all six
  gears, and the tach's red zone — none of which a real HPR lap reaches.
  Plan: `docs/plans/2026-07-22-001-feat-hpr-lap-simulation-plan.md`.
- USER button (`PC13` on the Nucleo, pin 24 on Teensy): **short press toggles
  TRACK/STREET, hold ≥1 s resets the trip.** Gesture logic is a pure header
  (`dash_button.h`), host-tested. The 30 ms debounce is load-bearing — a single
  EMI glitch on a car harness must not zero the trip. In the real car the mode
  switch will come from a **CAN message**; the button is a bench stand-in, and
  serial, button, and CAN all converge on one `s->mode` assignment.
- CAN upstream is an **Autosport Labs RaceCapture** (broadcasts CAN, runs Lua,
  configured from the phone app). Two things are meant to ride it, neither built
  yet (`dash_can.h` is still FDCAN-loopback-only, so nothing receives):
  **session length**, set in the paddock — this is the *only* thing blocking a
  countdown session timer, since HPDE sessions vary (20/25/30 min) and the dash
  has no input device left (both button gestures are taken); and **real lap
  timing**, which RaceCapture does natively via GPS. `LAP`/`LAST`/`BEST`/`DELTA`
  are simulated today only because there is no other source — with RaceCapture
  on the bus they become measured, and the simulator becomes bench-only.
  Unconfirmed: whether one value can be changed *trackside* from the phone
  (live control vs. a config push). Check the real unit before designing on it.
- **The simulator drives `DashState` directly — it does NOT emit fake CAN.**
  `dash_sim_step()` calls `dash_ch_set()`; renderers read the struct. That is
  the documented seam (`dash_data.h`: "Producers — `dash_sim.h` now, CAN
  decoders later — fill `DashState.ch`"), so sim and CAN decoders are peers,
  and adding CAN is not a refactor. **The catch:** the sim therefore gives the
  decode path zero exercise, so a 20-minute clean bench run proves nothing
  about it — the seam we will actually ship on is the one thing untested.
  Do NOT fix this by routing the sim through CAN in the render loop (encode +
  decode every frame at 60 fps, no runtime benefit). Fix it with a **host
  test**: take sim output, encode to CAN frames, run it back through the real
  decoder, assert the resulting `DashState` matches. Cheap, once frames exist.

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

The dash-era `data` is the embedded splash pack (~1.66 MB of ASTC assets,
all three themes; blue/red backgrounds at 4x4, checkered at 6x6) plus the
zlib font glyphs (F767 image ~1.81 MB of 2 MB, 86.4% -- gating the pack on
SPLASH_THEME at build time would reclaim ~888 KB if flash gets tight);
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
**F767 first light (2026-07-21): NUCLEO-F767ZI + center 7" CONFIRMED.**
MCU-direct splash (embedded pack -> RAM_G) played on glass, crossfade, 60 fps
sim dash, REG_ID 0x7C, faults=0, pwm=128 -- all on the single ST-LINK USB
cable (power Plan A held at full backlight duty; bench buck not needed).
F767 SPI clocks are prescaler-quantized (APB2=108 MHz, powers of two:
6.75/13.5/27/54 -- requests round DOWN). Clock walk result (2026-07-21, on
long low-quality jumpers): 6.75 MHz clean; **13.5 MHz ACCEPTED** (3-min
STREET soak, fps=60 every sample, faults=0, REG_ID stable x15, splash
staging clean, eyes-on in both modes); **27 MHz HARD-WEDGED the firmware**
-- serial fully dead, loop stuck in the unbounded EVE_execute_cmd busy-poll
on corrupted reads; recovered by ST-LINK reflash, no power-cycle needed.
13.5 MHz is the F767 operating point; re-walk on carrier copper. Wiring per
docs/hardware/nucleo-f767-center-panel-wiring.md (breakout BL pads bridged
17-18 and 19-20, single tails to CN8-9/CN8-11). Serial mode switch, forced
alarm (green LED + takeover, both modes), and alarm-off all verified live.
Panel-flash state (2026-07-21): the center panel's QSPI flash holds an
obsolete EVE Screen Editor image — a 2026-07-20 ESE session loaded generated
map/bin files to address 0, so sector 0 is ESE-provenance, not factory.
Firmware no longer reads or writes panel flash. A guarded `flashwipe` serial
command exists for the full-chip erase, but as of 2026-07-21 the center
panel's flash NO LONGER ATTACHES (EVE_init_flash -> 0x06 DETACHED, warm and
cold boots alike, post-ESE-session) — the erase cannot run, and equally the
stale image cannot be read by anything, which retires the hygiene concern.
Diagnosis needs the eval board (does ESE direct-USB still detect the chip?);
only matters if panel flash is ever wanted again (the carrier uses external
NOR instead). flashwipe's ack is diagnostic: it reports init code + status
and refuses to fake success (first bench run exposed that CMD_FLASHERASE on
a detached flash no-ops below the fault latch: a 0-second "ok").
Bring-up hazards actually hit on this bench (in symptom order): a damaged FFC
end shorting pins 1-2 (VDD-GND -> Teensy won't enumerate on USB), and a flaky
Teensy micro-USB cable that perfectly mimicked a dead board. Bench rules that
came out of it: pins 19-20 (BLGND) beep to pin 2 (GND) — use that continuity
to positively identify the backlight end (17-20) before applying 5 V; the FFC
is down-side contact at the panel; the panel survives being driven with no
5 V on BLVDD (renders, just dark).

## KiCad is the design; EasyEDA is retired (2026-07-31)

Kevin moved off EasyEDA. `kicad/board3/` is the authoritative schematic and
board — not a downstream copy — so edit it directly and do not mirror anything
into `EasyEDA/`, which stays only as history. Anything that was *blocked* on the
EasyEDA bridge is now **retired**, not deferred: that includes the 2026-07-31
pre-fab review's caveat that its electrical findings covered "the KiCad copy
only", and its recommended ERC/power-tree re-run through the bridge. That copy
is the design, so those findings stand on their own.

This changes nothing about KTD12 below. The GUI-only schematic→board sync is a
constraint of KiCad's own eeschema/pcbnew seam; retiring EasyEDA automates none
of it. Measured 2026-08-01 while trying: `pcbnew` *can* add footprints, create
nets and assign pads (that part works), but the nearest clear pocket for the new
RSTN passives is **13.5 mm from U11 pin 23 and 16.25 mm from U12 pin 23** — there
is no room beside either IC, so the placement judgement and the route out of the
QFN escape want the interactive router, not a search algorithm.

## KiCad parts rule (Board3)

**Every part placed on a KiCad board must be an LCSC part with matching supplier
metadata and a 3D model.** Add them with `python tools/kicad_lcsc.py add C<n>` —
never by dragging a generic symbol from the stock libraries. `check` audits and
exits non-zero on any gap; `models` backfills missing 3D models. See
`kicad/README.md`.

Why it is a build dependency, not a preference: `tools/kicad_fab.py` builds the
JLCPCB BOM from the schematic's supplier fields, so an un-sourced part does not
warn — it silently vanishes from the BOM.

**Searching for the part is itself a trap.** LCSC indexes `package` as the
vendor filed it, so a JEDEC-name query hides the metric-name half of the
catalogue (`PLCC-2` 98 parts vs `3528` 194; orange stock 79 vs 14,654). Always
search a package **both ways** before calling a part scarce —
`docs/solutions/developer-experience/search-a-package-by-both-its-jedec-and-metric-names.md`.

Field-name traps, both load-bearing and both already paid for:

- The LCSC code lives in **`Supplier Part`**. The JLCImport plugin writes it to a
  property named `LCSC`, so `kicad_lcsc.py add` remaps it; skip the remap and the
  part looks sourced on screen while being invisible to the BOM.
- **`LCSC Part Name` is not the part number** — it holds descriptive text, often
  Chinese. Mapping the obvious-looking field yields a BOM of garbage.
- JLCImport opens files without an explicit encoding, so on a Windows cp1252
  console any part with a `℃` or a Chinese character dies with a
  UnicodeEncodeError. `kicad_lcsc.py` relaunches itself under `-X utf8` to dodge
  this — via `subprocess`, **not** `os.execv`, which on Windows does not replace
  the process and loses the child's output and exit code.

3D models are `kicad/board3/EASYEDA_MODELS/*.step`, tracked (~53 MB), referenced
as `${KIPRJMOD}/EASYEDA_MODELS/…`. The EasyEDA import wrote those refs but never
the files, so the 3D view was empty until they were backfilled 2026-07-27.
JLCImport's computed model transform agrees with the EasyEDA-authored one
(checked on SOT-23-6: both `rotate 0 0 -180`), which is why the files could be
dropped in under the existing names without rewriting 30 footprints and 140
board entries.

### Schematic → board sync is GUI-only. Nothing automates it. (verified 2026-07-28)

Editing a symbol as text is the easy half; getting it onto the board is the part
with no CLI. All three candidate routes were tested on Board3 and all three fail:

- **`kicad-cli pcb`** offers only `drc/export/import/render/upgrade`, and
  `import` means *foreign format* (Eagle/Altium), not netlist.
- **The `pcbnew` Python API exposes nothing** — no `netlist`, `updater`,
  `synchronise` or `fromsch` symbol at all. `BOARD_NETLIST_UPDATER` is C++
  internal and was never SWIG-wrapped.
- **The KiCad MCP's `sync_schematic_to_board` reports success and CORRUPTS the
  board.** Re-tested 2026-08-02 on the fully-routed Board3; worse than the
  earlier "does nothing" reading. It returned `success: true, 0 footprints
  added, 93 nets added, 55 pads assigned` — and:
  - **It creates duplicate nets differing only by a leading slash.** The board
    carries `/GND`, `/+5V`, `/DC1_IN`; it added `GND`, `+5V`, `DC1_IN`. Net
    count 229 → 245, with **16 names existing in both forms**.
  - **It then reassigns pads onto the new names**, orphaning them from the
    copper attached to the old ones: **airwires 0 → 30**.
  - **It does not do the thing you wanted anyway** — F1's footprint change
    (`BSMD1812-200-30V` → `1812L300_30GR`) did not happen; 0 footprints added.

  Recoverable only because the tree was committed; `git restore` put the md5
  back exactly. **Never run it on a placed-and-routed board.** KTD12 stands:
  the sync is GUI-only.

  What works instead, and is how every schematic change in the 2026-08-02
  campaign reached the board: **apply both sides by hand.** For a rotation,
  rotate the symbol *and* the footprint and swap the pad nets (U31). For a
  value or supplier change on an unchanged land pattern, edit the schematic
  properties and, where the board's `Value` prints, set it with `fp.SetValue()`
  (U10/U24/U32). Verify through the exported netlist by pin **function**, never
  pin number (KTD27).

**A fourth route exists and is not a netlist sync: `pcbnew` can replace footprints
directly.** `FootprintLoad`, `board.Add`/`Remove`, `SetPosition`/
`SetOrientationDegrees`, `board.FindNet` and `pad.SetNet` are all exposed and all
work — U11 swapped all eight telltales this way, preserving position, rotation
and field layers, assigning every pad from the netlist. **It still had to be
reverted:** DRC went 194 → 286 → 258 → 231 across three passes, never reaching
baseline. The blocker is **routing topology, not geometry**: a collision-free
placement for all eight exists within 1.5 mm of home (found by searching
rotation × offset against `GetEffectiveShape(F_Cu).Collide`), but **`/+5V`
daisy-chains *through* the telltale pads** — deleting a stale stub at an LED pad
orphans `C30` further down the chain. Re-routing here means re-establishing a
chain, not drawing independent stubs, which wants a real router or the GUI. So:
the API can place footprints; do not use it to dodge the GUI step on an area
whose net topology runs through the parts you are replacing. Recipe and the
per-part offsets are in the U11 section of the telltale plan.

**Telltale/button architecture (I2C revision, 2026-07-29, plan 2026-07-28-001):**
the eight telltales are driven by two AW9523B expanders on I2C2 (trunk taps
forward from the FRAM U7): west U11 at 0x5B (AD both +5V — every port
POR-safe), east U12 at 0x5A (AD1 +5V/AD0 GND — LEDs on POR-safe P1_4–P1_7).
Both VCC=+5V (POR "high" = anode rail = LEDs off; pins are 6 V-rated, I2C
VIH is a fixed 1.4 V so the 3.3 V trunk is legal); RSTN strapped +5V
(internal pull-DOWN); 5 ms post-POR before I2C; ID reg 0x10 = 0x23. RSTN datasheet facts
(verified 2026-08-02): VIH fixed 1.4 V (not VCC-ratiometric), internal
pull-down 100 kΩ typ, built-in ≤10 µs glitch filter; the 10k/100n/Schottky
network holds reset ≥335 µs and settles at 4.55 V — 3.2x/3.3x margins.
Brightness matching is the host-tested calibration table
(`dash_calibration.h`, ISEL ×2/4 range) — the resistor derivation (plan 003
U12) was superseded unfabbed. **Buttons stayed on MCU GPIO (KTD20)** — no
firmware consumer, CAN is the real input; R28–R31 are 1 kΩ series parts.
PD0–PD7 are freed. Lamp bit l drives TT(l+1); DIM regs 0x2C–0x2F on both ICs.

**pcbnew scripting lessons (KiCad 10, all bisected the hard way, 2026-07-29):**
- After `board.Remove(item)`, keep the Python proxy alive (a named list)
  until `SaveBoard` — letting removed items be GC'd frees the C++ objects
  and corrupts the SWIG session (unrelated proxies turn into raw
  SwigPyObjects). The "memory leak … no destructor" warnings are this
  safety working, not a problem.
- One board per process: a second `LoadBoard` invalidates the first
  board's wrappers.
- `PCB_VIA.GetWidth()` needs a layer argument now (asserts without).
- Do NOT trust `GetEffectiveShape().Collide()` for clearance work: it
  under-covers segment midpoints between sampled probe points and
  misreports via shapes on inner layers. Exact point-segment /
  segment-segment math from raw endpoints converges; the shape oracle
  does not. `tools/kicad_verify.py` (staged rules) is the only judge.
- KiCadRoutingTools' route.py: `--rip-existing-nets` enables coordinated
  reroutes; it necks below process minimums at fine-pitch pads (repair by
  widening in place — 0.254 fits ON a 0.28 QFN pad) and its micro-vias
  (0.15/0.18 drill) need replacing. Route order matters: route the most
  boxed-in pad FIRST so fanouts nest (the last of three nets sharing one
  escape mouth always loses — a QFN mouth between a pad column and a wall
  fits exactly two 0.254 tracks).
- Call `board.BuildConnectivity()` after every `ZONE_FILLER.Fill()` and
  before every `GetUnconnectedCount()` — `GetConnectivity()` alone can
  read pre-fill state and lies in BOTH directions (accepted a trim that
  orphaned LED7's +5V; then vetoed eight legal removals against the
  baked-in airwire). Details:
  `docs/solutions/developer-experience/build-connectivity-before-counting-airwires.md`.
- `t is v` NEVER matches across two `GetTracks()` enumerations — every
  call mints fresh SWIG proxies, so identity-based self-exclusion lets an
  item collide with itself ("no feasible spot" that looks like tight
  geometry). Exclude by coordinates/value. Mutation through any proxy
  still works; only identity breaks.
- **The staged-copy trap has an ERC form, and it is nastier because the
  filename is the hidden dependency.** `kicad-cli sch erc` resolves the
  footprint library table through the `.kicad_pro` **whose name matches the
  schematic's**. A baseline copied out as `_erc_baseline.kicad_sch` therefore
  loads no project at all and reports **141 phantom `footprint_link_issues`**
  on Board3 — making an ERC-neutral edit look like it removed 154 violations.
  Copying into the project *directory* is not enough; the sidecar has to match
  by name. To baseline a schematic edit, put the HEAD version back at the real
  path, run ERC, then restore your copy — anything else compares a project to
  a non-project.
- Headless DRC on a staged board copy needs the FULL sidecar set:
  `.kicad_pro`, `.kicad_dru`, **`fp-lib-table` + the `.pretty` dirs** —
  without the last two, kicad-cli reports mass `lib_footprint_issues`
  "library not found" phantoms (120 on Board3) that also mask the real
  mismatches. `tools/kicad_verify.py` stages only pro+dru (fine for its
  error gate; not for `--severity-all` audits).

**Autorouting Board3 (2026-07-29, all paid for):** `tools/kicad_freeroute.py`
wraps freerouting headless (portable JRE + jars in `C:\Users\kevin\Tools`).
Facts it encodes: freerouting **2.2.4 infinite-loops** in
`PolylineTrace.combine()` on this board's DSN — use **1.9.0**; KiCad's DSN
export **returns False silently** for footprints with empty reference
designators (Board3's four EasyEDA corner pads) — synthesize refs; the DSN
reader pops a **modal warning dialog on any non-ASCII byte** (Ω in resistor
values) even in batch mode and `-dct` does not dismiss it — strip to ASCII;
KiCad's **SES import wipes and rebuilds every net named in the session**, so
locked copper is lost wholesale (276 airwires) — session import is unusable
for surgical work on a routed board. Bottom line: with all existing copper
locked (KiCad does export locks as `fix` wires, all 2,801 honored),
freerouting **could not route TT2 at all** — the mouth needed copper moved,
and the final TT2/TT5/TT7 restack was laid by hand from complete window
dumps with exact clearance math (track-track 0.3556, track-via 0.5286,
via-via 0.7016 center-to-center at 0.254 mm/0.1016 mm rules).

**Moving a two-pad part off its site is not free: something is already routed
through the gap it leaves.** U50 (2026-08-03) moved the four panel-SPI series
resistors R33/R34/R36/R37 ~90 mm to their MCU pins, per the layout guide §4 and
the carrier plan. Each vacated 0603 needs a *bridge* joining the two nets its
pads used to join — and on Board3 all four sites had another net running through
that 1.507 mm gap (`/GND` weaves along the whole connector-side resistor row at
y~93.73; `/MISO_L` crosses R36's). A straight top bridge produced exactly four
`tracks_crossing` errors, and top detours are boxed in, so the bridges drop to
**Bottom** (the guide's secondary-routing layer — never Inner1, the GND
reference). Two of them then could not keep the vacated pad centres, because a
through via is judged on all four layers: `/TT2_LED_K` runs on Bottom 0.057 mm
away, and a `/MISO_R` Inner2 diagonal 0.161 mm. And one *legal* via still pinched
R42's `/GND` pad to a single thermal spoke — `starved_thermal`, invisible to the
airwire count, which stayed 0 throughout. Budget three DRC iterations for a part
relocation, not one.

Two consequences worth carrying: `tools/kicad_handroute.py` now has **`renet`**
(reassign copper between nets without re-laying it, so a split run keeps its
integer nanometres) and **rotation / absolute placement** in `move_footprints` —
older plan text saying it "has no rotate op" describes the tool as it was. And a
pure relocation needs **no schematic edit and therefore no KTD12 GUI sync**: the
part stays between the same two nets, so only its footprint moves.

**Never window-filter board dumps by an object's reference point.** Ask whether
the SHAPE intersects the window, never whether some representative point is
inside it — and the test differs per object type: **tracks** by segment clip
(Liang-Barsky), **pads and footprints** by bounding-box overlap
(`GetBoundingBox()`, never `GetPosition()`), **vias** by centre±radius on every
layer they span. Both halves have been paid for: endpoint containment hid the
full-width `VBUS_SENSE` Inner1 river (y95.014, x54–110) twice in one session and
cost three routing iterations (2026-07-29); centre containment then hid C6's GND
pad (body at 151.294,123.079, pad reaching to x151.744) and `/SWO` was routed
straight through it (2026-08-02). The second happened *because* the first
write-up drew the line at "segments" and explicitly excused pads — so when you
generalise a rule of this shape, test every object type before exempting any.
Same family as the rule below.

**`--schematic-parity` is OFF by default, and it is the only check that compares
a footprint to its SYMBOL.** Plain DRC compares each footprint against its own
*library* copy, which passes happily while the board disagrees with the
schematic. That blind spot shipped four real defects, each a board marked with a
part an earlier unit had already replaced: F1 kept `BSMD1812-200-30V` after U32
fitted the 3 A PTC, C70/C71 still said `100nF` after U34 took them to 1 µF, R4
still said `0Ω` after U48 made it 10 kΩ, and LED3 carried U46's mangled MPN. All
are fixed and the gate is now in CI (`kicad-drc-erc.yml`), absolute rather than
ratcheted. Two related traps: an FPID written without its library nickname
(`C0603` rather than `ProPrj_New-easyedapro:C0603`) reads as a mismatch *and*
stops KiCad resolving the library at all, so `lib_footprint_mismatch` silently
tests nothing; and `pcbnew.FootprintLoad()` returns exactly such a bare FPID, so
set it explicitly after any scripted footprint swap.

**`footprint_symbol_field_mismatch` is deliberately set to `ignore`.** KiCad wants
every symbol field mirrored onto the footprint. Mirroring them was tried
(2026-08-03) and reverted: Board3's supplier metadata lives on the *symbol* by
design — `kicad_fab.py` builds the BOM from schematic fields — so copying it onto
footprints creates a second copy that drifts, and it immediately produced 13
`lib_footprint_mismatch` violations because the library copies carry no such
fields. The exemption covers the field check only; `footprint_symbol_mismatch`
(footprint and value) stays armed, and that is the half that catches real
defects.

**Read board facts through `pcbnew`, never regex.** Three separate wrong
conclusions in one session came from parsing `.kicad_pcb` as text: "the board has
no tracks" (the file writes `(segment` followed by a newline, so a `\(segment `
pattern matches nothing of 2,487), "no track lands on any telltale pad", and "all
six copper zones are netless" (they are `/GND` ×3 filled, `/+5V` ×1, plus two
Inner2 keepout rule areas that are netless by definition). Each looked like a
real finding and each was a parser bug.

So KTD12 holds: **Kevin runs Update PCB from Schematic in the GUI**, with
re-link-by-reference ticked, and KiCad must be **restarted first** after any
out-of-editor schematic edit — it syncs from eeschema's cache, not the file, and
otherwise silently reports no changes.

**But check whether you need the sync at all.** It reconciles *footprints and
nets*. A pure part swap onto an identical land pattern has neither, and the only
stale artefact is the board footprint's own `Value` — which is visible on the
Component Marking Layer, so leaving it means fabricating a board marked with the
part that is not fitted. `fp.SetValue(...)` + `board.Save()` closes that with no
sync and no geometry change (U10, 2026-07-28: DRC 36/0 unchanged, nets 216 and
footprints 148 unchanged). A *footprint* change — 0805 → PLCC-2 — genuinely
needs the GUI step, and new parts still go in via `kicad_lcsc.py add C<n>`
(supplier metadata) and `kicad_lcsc.py models` (STEP backfill), never a
hand-placed symbol.

Related trap, same day: **`kicad_fab.py` must be launched under KiCad's own
interpreter** (`C:/Program Files/KiCad/10.0/bin/python.exe`). It has no
self-reexec, and under a bare `python` it silently skips gerber renaming and the
rotation audit — the run still exits reporting a written fab package, just a
degraded one whose layers are named `Top Layer.gbr` instead of `F_Cu.gbr`.

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
