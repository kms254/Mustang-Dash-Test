---
name: reboot-dash
description: Reboot the Teensy 4.1 dash so the boot splash replays. Use when the user wants to restart the dash, replay/show the splash, or capture the boot serial banner. Optional argument "capture" also reads the boot diagnostics from the serial port.
---

# Reboot the Dash

Reboot the connected Teensy 4.1 so the currently-flashed firmware restarts —
the boot splash plays again (~2 s animation + crossfade), then the pony screen
returns. Does not rebuild or reflash anything.

## Steps

1. **Resolve the reboot tool.** It ships with PlatformIO's Teensy package:
   - Windows: `%USERPROFILE%\.platformio\packages\tool-teensy\teensy_reboot.exe`
   - macOS/Linux: `~/.platformio/packages/tool-teensy/teensy_reboot`

   If the file is missing, PlatformIO's Teensy toolchain isn't installed on
   this machine — say so and suggest `pio run -t upload` once (it installs the
   package) rather than hunting for alternatives.

2. **Run it.** Discard its output; it exits quickly once the board reboots.
   Windows example:

   ```powershell
   & "$env:USERPROFILE\.platformio\packages\tool-teensy\teensy_reboot.exe" | Out-Null
   ```

3. **Report** that the splash is playing. Nothing else to do for a plain
   reboot.

4. **Only if the arguments ask for serial/banner/diagnostics capture**
   (e.g. `capture`): after the reboot, retry-open the board's serial port for
   up to 12 s and read for ~8 s, then print the captured banner (per-panel
   pins, per-panel `EVE_init`/`REG_ID`, per-panel `RAM_G` font load, DL usage,
   boot timing).
   - Windows: COM4 at 115200 8N1 with DTR/RTS enabled, via
     `System.IO.Ports.SerialPort` (the bench workflow documented in
     CLAUDE.md — there is no interactive monitor in this harness).
   - macOS/Linux: the Teensy enumerates as `/dev/tty.usbmodem*` /
     `/dev/ttyACM*`; read it with a short non-interactive tool (e.g.
     `python3 -c` with the `serial` module, or `timeout 8 cat` after `stty
     115200`).

   Healthy boot prints one line per panel of the form
   `Panel CENTER: EVE_init 0x00 (E_OK=0x00), REG_ID 0x7C (want 0x7C)` (then
   `Panel LEFT:` and `Panel RIGHT:`), one `RAM_G panel N: fonts ... bytes`
   line per healthy panel, a `DL usage (track/street of 2048): ...` line, and
   two boot-timing lines (`Boot: <n> ms to splash start`, `Boot: dash live at
   <n> ms`). A dead side panel shows a non-0x00 init / non-0x7C REG_ID on its
   own line while the others stay healthy. If the port never opens, the
   Teensy likely isn't connected over USB — report that instead of retrying
   forever.

## Failure notes

- No Teensy found / reboot tool reports nothing: check the USB cable (this
  bench has a documented flaky-cable history — see
  docs/solutions/integration-issues/eve-panel-bringup-no-usb-enumeration-diagnosis.md).
- Splash plays but screen stays dark: backlight 5 V supply, see the same doc.
