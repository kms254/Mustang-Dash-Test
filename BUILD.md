# How the compile was verified

The firmware in this repo was compiled clean for `teensy:avr:teensy41`. On a
normal workstation you would just install **Teensyduino** (which registers the
`teensy:avr` platform for arduino-cli / the Arduino IDE) and build. Most of this
file documents the toolchain that was assembled to verify the build in an
environment where the usual PJRC / Arduino downloads (`pjrc.com`,
`downloads.arduino.cc`) were blocked, in case it needs to be reproduced. If you
are on a normal machine with network access, read the next section and stop.

## Two build paths (both verified, byte-identical output)

| Path | Entry point | Use it for |
|------|-------------|------------|
| `arduino-cli` | `./scripts/compile.sh` | the reference build; what `tests/` and CI assume |
| PlatformIO | `platformio.ini` | the VS Code Build/Upload/Monitor buttons |

They are not redundant by accident — keep them in sync. PlatformIO resolves
`framework-arduinoteensy 1.162.0` and `toolchain-gccarmnoneeabi-teensy 15.2.1`,
which are the same core and compiler that Teensyduino 1.62.0 installs, so the
two paths produce identical section sizes. If they ever diverge, that is a real
signal, not noise.

Setup on a Windows workstation (verified 2026-07-08):

```bash
winget install --id ArduinoSA.CLI -e     # arduino-cli on PATH; shares the
                                         # Arduino15 data dir, so it inherits
                                         # the Teensy platform the IDE installed
```

Install Teensyduino through the Arduino IDE Board Manager (`teensy:avr`). For
PlatformIO, just open the folder in VS Code — the extension bootstraps its own
core from the **portable Python it ships predownloaded**, so no system Python is
required.

`platformio.ini` sets `src_dir = MustangDash` so the sketch folder is not moved,
and `lib_extra_dirs = libraries` so the vendored (locally modified) EVE library
is used instead of the registry copy. PlatformIO's `teensy41` board defines
`ARDUINO_TEENSY41`, which is what auto-selects `EVE_target_Arduino_Teensy4.h` —
verified, not assumed.

## Running the tests on Windows

`tests/run-tests.sh` compiles the host-side invariant tests with `gcc`, which
Git Bash does not provide. On Windows, run them through WSL:

```bash
wsl -- bash -lc "./tests/run-tests.sh"      # from the repo root
```

`wsl` inherits the Windows working directory as `/mnt/c/...`, so no `cd` is
needed. VS Code task **"Tests: invariant suite"** does exactly this, and runs
the script directly on macOS/Linux.

Two traps worth knowing:

- **CRLF.** Git's `core.autocrlf=true` (Windows default) checks the `.sh` files
  out with CRLF. Git Bash tolerates the trailing `\r` in a shebang; WSL's bash
  does not, and fails with ``/usr/bin/env: 'bash\r': No such file or directory``.
  `.gitattributes` pins `*.sh` to `eol=lf` to prevent this. Don't remove it.
- **PATH.** A shell started *before* `winget install ArduinoSA.CLI` won't see
  `arduino-cli`. Restart the terminal (or VS Code) after installing.

## Pieces used

| Piece | Normal source (blocked here) | Substitute used |
|-------|------------------------------|-----------------|
| `arduino-cli` | downloads.arduino.cc release | built from source: `git clone https://github.com/arduino/arduino-cli && go build` |
| ARM toolchain | Teensyduino `teensy-compile` | system `gcc-arm-none-eabi` 13.2.1 (`apt`); it ships the `thumb/v7e-m+dp/hard` multilib = Cortex-M7 hard-float |
| Teensy 4 core | Teensyduino package | `git clone https://github.com/PaulStoffregen/cores` → `teensy4/` |
| `SPI` library | Teensyduino bundled | `git clone https://github.com/PaulStoffregen/SPI` |
| `boards.txt` / `platform.txt` | Teensyduino package | hand-written minimal versions driving the system compiler (installed to the arduino-cli sketchbook `hardware/teensy/avr/`) |
| `ctags` (`.ino` prototype gen) | Arduino bundled `arduino-ctags` fork | not available offline; replaced with a **no-op shim** so arduino-cli inserts no auto-prototypes — the sketch therefore declares its own function prototypes |

## Notes / caveats

- This minimal platform **compiles and links** a correct Teensy 4.1 image
  (correct `-mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16`, the
  `imxrt1062_t41.ld` linker script, `-D__IMXRT1062__ -DARDUINO_TEENSY41
  -DF_CPU=600000000 -DUSB_SERIAL`). It does **not** wire up upload — there was
  no board attached and the PJRC loader tools were unavailable. Flash from the
  Arduino IDE (Teensy Loader) or `teensy_loader_cli` on the machine with the
  board.
- Because the `.ino` prototype generator is a no-op here, keep declaring
  function prototypes before use in the sketch (as `MustangDash.ino` does).
  Under a real Teensyduino install this is not required, but it is harmless.
- `arduino-cli` prints a few `Error initializing instance:` lines about missing
  package/library indexes and serial-discovery — those are because the offline
  setup has empty indexes; they do not affect compilation.

## Reproduce (offline)

```bash
# toolchain
apt-get install -y gcc-arm-none-eabi
git clone --branch v1.5.1 https://github.com/arduino/arduino-cli && (cd arduino-cli && go build -o /usr/local/bin/arduino-cli .)

# teensy platform into the sketchbook
arduino-cli config init
SB="$(arduino-cli config get directories.user)"   # e.g. ~/Arduino
mkdir -p "$SB/hardware/teensy/avr/cores" "$SB/hardware/teensy/avr/libraries"
# Pinned to the exact upstream snapshots the verified build used; drop the
# checkout lines to track upstream HEAD (at your own risk -- TEENSYDUINO=159
# in platform.txt describes this snapshot, not upstream HEAD).
git clone https://github.com/PaulStoffregen/cores && git -C cores checkout 7f107ee0a309f3813ed13f0d8f615497eca2ee49 && cp -r cores/teensy4 "$SB/hardware/teensy/avr/cores/teensy4"
git clone https://github.com/PaulStoffregen/SPI  && git -C SPI  checkout 7c83d0726746b652af37319e70cd3932c253ecae && cp -r SPI "$SB/hardware/teensy/avr/libraries/SPI"

# boards.txt / platform.txt / the ctags shim are ordinary tracked files at
# tools/teensy-avr-platform/ in this repo -- scripts/compile.sh installs them
# into "$SB/hardware/teensy/avr/" (shim as tools-bin/ctags) before every build,
# so no manual copy step is needed:
./scripts/compile.sh
```
