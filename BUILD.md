# How the firmware is built

The firmware targets **STM32** and builds with **PlatformIO**. There is one
build path, and both the CLI and the VS Code buttons drive it.

```bash
./scripts/compile.sh                 # default env: nucleo_f767
./scripts/compile.sh h743            # the H755 carrier target
./scripts/compile.sh nucleo_f767 -v  # extra args pass through to pio
```

`scripts/compile.sh` is a thin wrapper around `pio run -e <env>`; it locates
PlatformIO in its own venv (`~/.platformio/penv`) and falls back to whatever
`pio` is on PATH. Override with `PIO=/path/to/pio` if yours lives elsewhere.

## Environments

| env | Board | Why it exists |
|---|---|---|
| `nucleo_f767` | NUCLEO-F767ZI | The three-panel mule. Cortex-M7 like the carrier, 2 MB flash, six SPI peripherals, onboard ST-LINK. Serial rides the ST-LINK VCP. **Default.** |
| `h743` | WeAct MiniSTM32H743VITX | Same silicon family as the custom carrier, so the devkit doubles as a firmware mule until the PCB exists. Serial on the USB-C VCP. |
| `riverdi_f469` | Riverdi F469 eval | Panel-integrated eval board, used for validating a panel independently of our wiring. |

PlatformIO resolves the toolchain and the STM32 Arduino core itself — nothing to
install by hand, no sketchbook to sync. `src_dir = MustangDash` keeps the `.ino`
layout; `lib_extra_dirs = libraries` uses the **vendored** EVE library rather
than the registry copy, because `src/EVE_config.h` selects the `EVE_RVT70H`
profile and `src/EVE_target/` carries our pins.

## Running the tests on Windows

`tests/run-tests.sh` compiles the host-side invariant tests with `gcc`, which
Git Bash does not provide. On Windows, run them through WSL:

```bash
wsl -- bash -lc "./tests/run-tests.sh"      # from the repo root
```

`wsl` inherits the Windows working directory as `/mnt/c/...`, so no `cd` is
needed. VS Code task **"Tests: invariant suite"** does exactly this, and runs
the script directly on macOS/Linux.

The trap worth knowing:

- **CRLF.** Git's `core.autocrlf=true` (Windows default) checks `.sh` files out
  with CRLF. Git Bash tolerates the trailing `\r` in a shebang; WSL's bash does
  not, and fails with ``/usr/bin/env: 'bash\r': No such file or directory``.
  `.gitattributes` pins `*.sh` to `eol=lf` to prevent this. Don't remove it —
  and note that writing a `.sh` file with a Windows text editor or a Python
  script that doesn't force `newline="\n"` reintroduces it.

## Flash budget

The splash asset pack dominates the image. As of 2026-08-05 the `nucleo_f767`
build sits at **1,819,568 B of 2,097,152** (86.8%), RAM 8,088 B of 524,288
(1.5%). The pack ships all three themes; gating it on `SPLASH_THEME` at build
time would reclaim a large fraction if flash gets tight.

## Notes

- The `.ino` declares every function before use. PlatformIO does its own
  prototype generation, so this is belt-and-braces — but it keeps the file
  readable top-to-bottom and costs nothing.
- Host tests never touch a board, a toolchain, or the network. They pin the
  display profile, the control pins, the backlight wave, the splash timeline,
  the dash math/sim/serial/odometer logic, the font format, the flash-pack
  layout, the button gestures and the telltale calibration. Run them after
  touching `EVE_config.h`, the STM32 target header, or any `MustangDash/*.h`
  pure header.
