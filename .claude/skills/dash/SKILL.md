---
name: dash
description: Send a bench command to the running dash over serial. Use when the user wants to force a dash value or state - set RPM/speed/temps, switch TRACK/STREET mode, trigger or clear an alarm, set the odometer, freeze or resume the simulator, or read dash status. Examples - "/dash set-rpm 3500", "/dash mode street", "/dash alarm oilp", "/dash status".
---

# Dash Bench Control

Send one line of the dash's serial protocol to the connected Teensy 4.1 and
report the firmware's `ok …` / `err …` acknowledgement. The firmware prints
nothing over serial after boot except these acks, so the reply you read is
always the answer to the command you sent (single documented exception:
`flashwipe really` prints a warning line before its ack — see its row).

## Protocol

Line-based ASCII at 115200 8N1, newline-terminated. Commands (all
case-insensitive):

| Command | Effect |
|---|---|
| `set <channel> <value>` | Force a channel; sticky until cleared |
| `clear <channel>` | Mark a channel invalid (renders `--` / dead-front) |
| `mode track` / `mode street` | Switch the dash view instantly |
| `circuit hpr` / `circuit sweep` | TRACK's driving model: the real High Plains Raceway lap (default), or the range-sweep bench fixture — a 100 s ramp of speed 0 → 200 → 0 that walks all six gears and the tach's amber zone. Lap channels dead-front while sweeping, and the lap in progress is abandoned on either switch. TRACK-only: it has no effect until the mode is TRACK. |
| `alarm oilp\|oilt\|clt` | Force the matching alarm condition |
| `alarm off` | Release forced alarm channels back to the simulator |
| `odo set <miles>` | Reseed the persisted odometer |
| `sim on` / `sim off` | Resume pure simulation / freeze all values |
| `status` | One-line report (mode, fps, all channels, odometer; per-panel fields are comma triples center,left,right — `faults=0,0,0`, `retired=0,0,0`, `dl=t/s,t/s,t/s`, `eve=ok,ok,--`). `retired` counts frame-drain timeouts that killed a panel at runtime (it shows `eve=--` after); fps reads 0 when nothing renders — it never freezes at a stale value. |
| `help` | List commands |
| `flashwipe really` | **Destructive, minutes-long:** full-chip erase of the center panel's retired QSPI flash. The one two-line command: it first prints a `flashwipe: erasing…` warning, then blocks SILENTLY for minutes (dash frozen, no output) before the final `ok flashwipe` / `err flashwipe …` ack — extend the read timeout to at least 5 minutes and NEVER power-cycle while waiting. Bare `flashwipe` or any other argument errs without erasing. |

Channels: `rpm speed ect oilt oilp volts fuel delta lap last best ambient afr_l afr_r iat fuelp throttle brake lapn pos pred time pump fan1 fan2`.

## Steps

1. **Translate the skill arguments to a protocol line.**
   - Hyphenated shorthand: `set-rpm 3500` → `set rpm 3500` (any `set-<channel> <value>`).
   - Everything else passes through verbatim: `mode street`, `circuit sweep`,
     `alarm oilp`, `odo set 24318`, `sim on`, `clear ect`, `status`, `help`.
   - No arguments → send `status`.

2. **Send the line and read the ack.**
   - Windows (this bench): COM4 via `System.IO.Ports.SerialPort`, 115200 8N1,
     DTR/RTS enabled. Open, write the line + `"\n"`, read lines for up to 3 s,
     close. Example:

     ```powershell
     $p = New-Object System.IO.Ports.SerialPort 'COM4',115200,'None',8,'One'
     $p.DtrEnable = $true; $p.RtsEnable = $true; $p.ReadTimeout = 3000
     $p.Open(); $p.WriteLine('set rpm 3500')
     try { $p.ReadLine() } catch { 'NO-ACK' }
     $p.Close()
     ```

   - macOS/Linux: the Teensy enumerates as `/dev/tty.usbmodem*` /
     `/dev/ttyACM*`; write the line and read the ack with a short
     non-interactive tool (`python3 -c` with the `serial` module, or
     `stty 115200` + redirection with a `timeout`).

3. **Report the ack verbatim** (`ok set rpm 3500`, `err unknown channel`, or
   the `status` line). An `err …` reply is the firmware talking, not a bench
   failure — show it and explain briefly.

## Failure notes

- Port won't open: the Teensy isn't connected or another monitor holds COM4.
  This bench has a documented flaky-cable history — see
  docs/solutions/integration-issues/eve-panel-bringup-no-usb-enumeration-diagnosis.md.
- `NO-ACK`: the firmware predates the dash serial protocol (reflash), or the
  board is mid-boot — retry once after 3 s.
- To replay the boot splash instead, use the `/reboot-dash` skill.
