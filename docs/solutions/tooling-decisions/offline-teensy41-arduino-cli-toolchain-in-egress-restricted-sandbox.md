---
category: tooling-decisions
title: Offline Teensy 4.1 arduino-cli toolchain for egress-restricted sandboxes
module: build-tooling
date: 2026-07-09
problem_type: tooling_decision
component: tooling
severity: high
applies_when:
  - building teensy or arduino firmware in egress-restricted ci or sandboxes that block pjrc.com and downloads.arduino.cc
  - arduino-cli install script, github release downloads, or the pjrc board-manager index fail with proxy 403
  - arduino-cli .ino prototype generation breaks because stock ctags lacks the arduino-ctags fork's return types
symptoms:
  - "CONNECT to downloads.arduino.cc:443 and pjrc.com:443 rejected with 403 by the policy proxy"
  - "go install of arduino-cli fails because its go.mod contains replace directives"
  - "generated sketch .cpp contains a broken ` setup();` prototype when stock ctags replaces arduino-ctags"
resolution_type: environment_setup
related_components:
  - development_workflow
tags:
  - teensy41
  - arduino-cli
  - offline-build
  - egress-restricted
  - ctags
  - arm-none-eabi
  - toolchain
  - sandbox
---

# Offline Teensy 4.1 arduino-cli toolchain for egress-restricted sandboxes

## Context

This session's sandbox routed all HTTPS through an organization policy proxy that rejected CONNECT to `pjrc.com:443` and `downloads.arduino.cc:443` with 403 (policy: do not retry or route around). That blocked every normal path to a Teensy build: the arduino-cli install script's binary download, GitHub release assets for arduino-cli, the PJRC board-manager index (`package_teensy_index.json`), and its toolchain/tools payloads — i.e., Teensyduino was uninstallable. What still worked: plain `git clone` of public GitHub repos, raw.githubusercontent.com, `apt`, and Go module downloads (proxy.golang.org was on the proxy's no-proxy list). The goal was to verify that `MustangDash/MustangDash.ino` compiles for `teensy:avr:teensy41`; the assembled substitute toolchain is documented in `BUILD.md` and preserved in-tree under `tools/teensy-avr-platform/` (pieces table: `BUILD.md:12-19`).

## Guidance

Five pieces replace the normal Teensyduino install (reproduction script: `BUILD.md:39-54`):

1. **arduino-cli from source, via `go build` — not `go install`.** In this session, `go install github.com/arduino/arduino-cli@latest` failed with "The go.mod file ... contains one or more replace directives": `go install` refuses modules that use `replace`, and arduino-cli's go.mod does. The working path is `git clone https://github.com/arduino/arduino-cli` then `go build -o /usr/local/bin/arduino-cli .` inside the clone (`BUILD.md:14`, `BUILD.md:42`); this session additionally pinned the clone with `--depth 1 --branch v1.5.1`, and Go auto-fetched the newer toolchain it needed through proxy.golang.org.

2. **ARM toolchain from apt.** `apt-get install -y gcc-arm-none-eabi` (13.2.1 in this session; `BUILD.md:15`, `BUILD.md:41`). The critical verification is that the distro toolchain ships the Cortex-M7 double-precision hard-float multilib — see Examples for the two commands. If `thumb/v7e-m+dp/hard` is present, no PJRC toolchain is needed to link IMXRT1062 binaries.

3. **Teensy core + SPI via git clone.** `PaulStoffregen/cores` provides the full `teensy4/` core including the `imxrt1062_t41.ld` linker script; `PaulStoffregen/SPI` provides the SPI library, which the core repo does not ship (`BUILD.md:16-17`, `BUILD.md:48-49`). Neither repo contains `boards.txt`/`platform.txt` — those exist only inside the Teensyduino package, so they must be written by hand (piece 4). Copy `cores/teensy4` to the sketchbook's `hardware/teensy/avr/cores/teensy4` and SPI to `hardware/teensy/avr/libraries/SPI` so the FQBN resolves as exactly `teensy:avr:teensy41`.

4. **Hand-written minimal `boards.txt`/`platform.txt`**, preserved in-repo at `tools/teensy-avr-platform/`. Key content:
   - `compiler.path=/usr/bin/` drives the system compilers (`tools/teensy-avr-platform/platform.txt:7`).
   - CPU flags `-mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16` (`platform.txt:22`).
   - Defines `-DF_CPU={build.fcpu} -D{build.usbtype} ... -D__IMXRT1062__ -DTEENSYDUINO=159 ... -DARDUINO_{build.board}` (`platform.txt:24`), with board values `fcpu=600000000`, `usbtype=USB_SERIAL`, `board=TEENSY41` (`tools/teensy-avr-platform/boards.txt:12-15`).
   - Link with `--specs=nano.specs -T{build.core.path}/imxrt1062_t41.ld` (`boards.txt:16`) and size ceilings 8,126,464 B flash / 524,288 B RAM1 (`boards.txt:18-19`).
   - Both `recipe.preproc.includes` and `recipe.preproc.macros` are required (`platform.txt:44-47`) — in this session, omitting `recipe.preproc.macros` failed the build with "open .../sketch_merged.cpp: no such file or directory".
   - Teensy-specific size regexes so the report counts `.text.itcm`/`.bss.dma`/`.bss.extram` correctly (`platform.txt:69-70`).

5. **The ctags no-op shim (the least obvious step).** arduino-cli's `.ino` prototype generation invokes `{runtime.tools.ctags.path}/ctags` (path wired at `platform.txt:9`). Arduino normally bundles a fork, **arduino-ctags**, which emits function *return types*; stock universal-ctags and stock exuberant-ctags both omit them, so arduino-cli inserts a broken prototype — literally ` setup();` with no return type — into the generated `.cpp`. Since arduino-ctags is only distributed via the blocked index, the fix is a shim that writes an *empty* tags file, making arduino-cli insert no auto-prototypes at all: `tools/teensy-avr-platform/ctags-shim.sh` (rationale comment at lines 2-5; the `: > "$out"` empty-file write at line 11). Consequence: sketches must declare their own prototypes before use — `MustangDash/MustangDash.ino:23-25` carries the explanatory note and lines 33-34 the forward declarations. Sub-gotcha from this session: the shim was first written *through* a symlink pointing at `/usr/bin/ctags-exuberant`, which overwrote the symlink target; remove the symlink before installing the shim. Finally, empty `package_index.json`/`library_index.json` in the arduino-cli data dir silence most index noise; the remaining "Error initializing instance" lines are harmless offline (`BUILD.md:33-35`).

## Why This Matters

Without this recipe, no CI or sandbox verification of Teensy firmware is possible behind an egress policy that blocks pjrc.com and downloads.arduino.cc — the entire official distribution chain (Teensyduino, arduino-cli binaries, the board-manager toolchain) is unreachable. The ctags trap deserves special weight: the failure it produces — "error: expected constructor, destructor, or type conversion before ';'" on a generated ` setup();` line — looks exactly like a sketch bug and gives no hint that the real cause is a ctags variant mismatch inside arduino-cli's prototype generator. Scope limits: upload is deliberately NOT covered — no board was attached and the PJRC loader tools come from the blocked host; flash from a machine with Teensy Loader or `teensy_loader_cli` (`BUILD.md:26-29`). On a normal workstation, installing Teensyduino replaces this entire recipe (`BUILD.md:3-8`).

## When to Apply

- Egress-restricted CI or sandboxes that need to compile Teensy firmware, or any Arduino platform distributed only through a board-manager package index that the network policy blocks.
- Any arduino-cli setup where the bundled arduino-ctags fork is unavailable and `.ino` prototype generation must be neutralized (the shim + explicit-prototypes pattern applies independently of Teensy).
- The environment must still allow git clone of GitHub, apt, and the Go module proxy — those are the recipe's only inputs.

## Examples

**Verify the apt toolchain can target Teensy 4.1 (Cortex-M7 hard-float):**

```bash
arm-none-eabi-gcc -print-multi-lib | grep 'v7e-m+dp/hard'
# expected: thumb/v7e-m+dp/hard;@mthumb@march=armv7e-m+dp@mfloat-abi=hard

arm-none-eabi-gcc -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-d16 -mthumb -print-file-name=libc.a
# expected: a path inside .../thumb/v7e-m+dp/hard/ (not just "libc.a" echoed back)
```

If the second command echoes back `libc.a` unresolved, the multilib is missing and linking will fail.

**Broken-prototype failure signature (stock ctags) vs fixed state (shim):** with stock universal/exuberant ctags, the generated sketch `.cpp` gains a return-type-less ` setup();` line and compilation fails with "error: expected constructor, destructor, or type conversion before ';'" (as observed this session). With the no-op shim in place at `{runtime.tools.ctags.path}/ctags` (`tools/teensy-avr-platform/platform.txt:9`), the tags file is empty, arduino-cli inserts nothing, and the sketch supplies its own prototypes — `MustangDash/MustangDash.ino:33-34`:

```c
void draw_first_light(void);
void set_backlight(uint8_t duty);
```

**Final compile** — via the wrapper `scripts/compile.sh:14-19`:

```bash
arduino-cli compile --clean -b teensy:avr:teensy41 --libraries ./libraries --output-dir ./build ./MustangDash
```

As observed this session: exit 0, no warnings, size report 31,740 bytes flash (0% of 8,126,464) and 37,888 bytes RAM (7% of 524,288), producing `build/MustangDash.ino.hex`.

## Related

- [BUILD.md](../../../BUILD.md) — the canonical step-by-step procedure this doc explains the rationale for; keep the two consistent.
- [tools/teensy-avr-platform/](../../../tools/teensy-avr-platform/) — the actual hand-written platform files (`boards.txt`, `platform.txt`, `ctags-shim.sh`).
- [scripts/compile.sh](../../../scripts/compile.sh) — the build entry point readers will actually run.
- [CLAUDE.md](../../../CLAUDE.md) — "Build environment (this cloud sandbox only)" section, the brief form of this learning.
- [docs/solutions/best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md](../best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md) — the other learning from the same bring-up session (low overlap: display configuration, not build tooling).
- GitHub issue search skipped for this learning (no `gh` CLI in the sandbox; no meaningful issue history).
