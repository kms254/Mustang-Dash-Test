#!/usr/bin/env python3
r"""PC emitter: dash-sim channel lines in, Ford-dialect CAN frames out (U5).

Reads the can_sim_feed.c line protocol on stdin (key=value pairs in dash
units: mph / degF / psi / V / A/F), converts to the Ford control pack's
units, encodes the 0x270-set via cantools + the repo's source of truth
(assets/can/ford-control-pack-gen4.dbc), and transmits through the bench
CANable (candleLight / gs_usb, the proven 2026-08-14 stack) at spec rates:
0x270 every 10 ms, 0x274/0x275 every 20 ms, 0x278 every 100 ms.

Pacing uses ABSOLUTE next-due deadlines on time.perf_counter, never fixed
sleeps -- Windows timer granularity is ~15.6 ms, so waking late and
batching the sends that came due is expected and correct; achieved rates
are logged every 5 s to stderr so the batching stays observable.

SAFETY (plan U5): if stdin hits EOF the emitter exits; if no channel line
arrives for 500 ms it STOPS transmitting entirely and logs why -- a dead
feed must dead-front the glass, not freeze it fresh -- and resumes when
lines resume. A message is transmitted only while ALL its dash-sourced
channels are present in the latest feed line (a channel the sim holds
invalid is omitted from the line, which gates the frames that carry it).

Runs on the PIO penv python (plan KTD9) -- this box has NO system python,
and the CANable stack (python-can + gs_usb + libusb-package) plus cantools
live in the penv. Build the feed first, then pipe across the WSL/Windows
boundary:

  wsl -- bash -lc "gcc -std=c11 -Wall -Werror -I MustangDash tools/can_sim_feed.c -lm -o /tmp/can_sim_feed"
  wsl -- /tmp/can_sim_feed | C:\Users\kevin\.platformio\penv\Scripts\python.exe tools/can_emit.py

No hardware needed for verification (the transceivers are on reorder; the
emitter must be provably correct while the bus is still unbuildable):

  # unit gate: encode a fixed value set, byte-compare against the golden
  # nominal vectors (tests/golden_can_ford.h), PASS/FAIL per message:
  C:\Users\kevin\.platformio\penv\Scripts\python.exe tools/can_emit.py --dry-run --dry-run-fixed

  # live-stdin rehearsal: real feed in, frames printed instead of sent:
  wsl -- /tmp/can_sim_feed | C:\Users\kevin\.platformio\penv\Scripts\python.exe tools/can_emit.py --dry-run

Frame lines print to stdout as "0x270 8 0dac2d9202ee2af8"; status goes to
stderr. Exit 0 on clean shutdown (EOF / Ctrl-C / --dry-run N reached),
1 on a golden mismatch or a hard error.

Plan: docs/plans/2026-08-15-001-feat-can-telemetry-ford-dialect-plan.md
(U5, KTD9).
"""

import argparse
import math
import re
import sys
import threading
import time
import traceback
from pathlib import Path

try:
    import cantools
except ImportError as exc:  # fail loudly with instructions
    sys.exit(
        f"ERROR: {exc}\n"
        "cantools is required (the CAN bench stack lives in the PIO penv, "
        "not WSL).\n"
        r"Fix:  C:\Users\kevin\.platformio\penv\Scripts\python.exe "
        "-m pip install cantools"
    )

ROOT = Path(__file__).resolve().parent.parent
DBC = ROOT / "assets" / "can" / "ford-control-pack-gen4.dbc"
GOLDEN = ROOT / "tests" / "golden_can_ford.h"

# ---------------------------------------------------------------------------
# Spec constants (same values the golden generator restates; the DBC owns
# bit placement and scaling, THESE own the dash-unit -> Ford-unit hop).
# ---------------------------------------------------------------------------
KPA_TO_PSI = 0.145037738  # psi per kPa
KMH_PER_MPH = 1.60934     # km/h per mph (the PCM encodes VSPD as mph*1.60934)

BITRATE = 500000          # HS-CAN classic, 11-bit IDs (KTD1)

FRAME_IDS = (0x270, 0x274, 0x275, 0x278)

PERIOD_S = {0x270: 0.010, 0x274: 0.020, 0x275: 0.020, 0x278: 0.100}

MSG_NAMES = {
    0x270: "FORD_0270_ENGINE",
    0x274: "FORD_0274_FUEL_AFR",
    0x275: "FORD_0275_SPEED",
    0x278: "FORD_0278_TEMPS",
}

# Feed keys each message needs before it may transmit (per-message gate).
FRAME_KEYS = {
    0x270: ("rpm",),
    0x274: ("afr_l", "afr_r", "fuelp"),
    0x275: ("speed",),
    0x278: ("ect", "oilt", "oilp", "volts"),
}

FEED_DEAD_S = 0.5   # no line for this long -> stop transmitting
MAX_BATCH = 10      # cap catch-up sends per message per wake, then resync
RATE_LOG_S = 5.0    # achieved-rate log period

# Signals the dialect carries but the dash does not source (undecoded by the
# firmware). The live feed holds them at benign floors.
LIVE_FILLER = {
    "MAN_VAC": -105.0,   # gauge-vacuum floor (raw 0)
    "DI_PRESSURE": 0.0,  # PFI car
    "BOOST": 0.0,        # NA car
    "TOT": 25.0,         # degC, no trans temp on the bench
    "SHIFTER_POS": 0,
    "CODES_COUNT": 0,
    "GEAR": 0,
}

# --dry-run-fixed filler: the golden nominal vectors populate the undecoded
# neighbor signals for bit-masking realism (make_can_golden.py NOM values),
# so the byte-for-byte gate must fill them identically. ONLY the filler
# differs from live -- every dash-sourced conversion below is the same code
# path in both modes, which is exactly what the gate proves.
GOLDEN_FILLER = {
    "MAN_VAC": -30.0,       # golden raw 750
    "DI_PRESSURE": 11000.0, # golden raw 11000
    "BOOST": 0.0,
    "TOT": 85.0,            # golden raw 125
    "SHIFTER_POS": 4,
    "CODES_COUNT": 0,
    "GEAR": 3,
}

# --dry-run-fixed channel inputs, in the feed's dash units, chosen to land
# EXACTLY on the golden nominal_* raw values (tests/golden_can_ford.h):
# rpm 3500; AFR 12.6/12.1; FUELP/EOP 340 kPa expressed as the psi the feed
# would print; VSPD 119.4 km/h as mph; ECT 90 degC / EOT 80 degC as degF;
# VBAT 13.30 V.
FIXED_VALUES = {
    "rpm": 3500.0,
    "afr_l": 12.6,
    "afr_r": 12.1,
    "fuelp": 340.0 * KPA_TO_PSI,   # 49.31283092 psi
    "speed": 119.4 / KMH_PER_MPH,  # 74.19190481 mph
    "ect": 194.0,                  # degF = 90 degC
    "oilt": 176.0,                 # degF = 80 degC
    "oilp": 340.0 * KPA_TO_PSI,    # 49.31283092 psi
    "volts": 13.30,
}


def log(msg):
    ts = time.strftime("%H:%M:%S")
    print(f"{ts} can_emit: " + msg, file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------
# CANable (gs_usb) -- opened ONLY outside --dry-run; the bench transceivers
# are on reorder and the adapter may be unplugged, so no test path may touch
# the bus.
# ---------------------------------------------------------------------------

def _install_libusb_backend():
    """Route pyusb at libusb-package's bundled libusb1 (proven on this
    bench, 2026-08-14): python-can's gs_usb interface calls usb.core.find
    with no backend, which on Windows finds no default libusb and fails, so
    default the backend argument in before importing can."""
    import libusb_package
    import usb.core

    backend = libusb_package.get_libusb1_backend()
    orig_find = usb.core.find

    def find_with_backend(*args, **kwargs):
        kwargs.setdefault("backend", backend)
        return orig_find(*args, **kwargs)

    usb.core.find = find_with_backend


def open_bus():
    """Import the TX stack (loudly) and open the CANable. Returns
    (can_module, bus)."""
    try:
        _install_libusb_backend()
        import can
    except ImportError as exc:
        sys.exit(
            f"ERROR: {exc}\n"
            "Live TX needs the proven CANable stack in the PIO penv.\n"
            r"Fix:  C:\Users\kevin\.platformio\penv\Scripts\python.exe "
            "-m pip install python-can gs_usb libusb-package"
        )
    bus = can.Bus(interface="gs_usb", channel=0, index=0, bitrate=BITRATE)
    return can, bus


# ---------------------------------------------------------------------------
# Encode: dash units -> Ford physical -> raw -> frame bytes.
# ---------------------------------------------------------------------------

def load_messages(db):
    """Map frame id -> cantools message; assert the 0x270-set is intact."""
    byname = {m.name: m for m in db.messages}
    msgs = {}
    for fid, name in MSG_NAMES.items():
        m = byname[name]
        assert m.frame_id == fid and m.length == 8, \
            f"{name}: DBC id/DLC drifted from the official table"
        msgs[fid] = m
    return msgs


def physical_for(fid, vals, filler):
    """Ford physical signal values for one message, from feed dash units.
    Note ECT/EOT: a physical 174/175 degC would encode the Ford sensor
    sentinels (raw 214/215) -- unreachable from the sim (tops out ~110
    degC) and deliberately not special-cased in a bench emitter."""
    if fid == 0x270:
        rpm = vals["rpm"]
        return {
            "ENGINE_SPEED": rpm,
            "ENGINE_SPEED_HZ": rpm / 60.0,
            "MAN_VAC": filler["MAN_VAC"],
            "DI_PRESSURE": filler["DI_PRESSURE"],
        }
    if fid == 0x274:
        return {
            "BOOST": filler["BOOST"],
            "AF0": vals["afr_l"],
            "AF1": vals["afr_r"],
            "FUEL_PRESSURE": vals["fuelp"] / KPA_TO_PSI,
        }
    if fid == 0x275:
        return {"VSPD": vals["speed"] * KMH_PER_MPH}
    if fid == 0x278:
        return {
            "ECT": (vals["ect"] - 32.0) * 5.0 / 9.0,
            "EOT": (vals["oilt"] - 32.0) * 5.0 / 9.0,
            "TOT": filler["TOT"],
            "EOP": vals["oilp"] / KPA_TO_PSI,
            "SHIFTER_POS": filler["SHIFTER_POS"],
            "CODES_COUNT": filler["CODES_COUNT"],
            "VBAT": vals["volts"],
            "GEAR": filler["GEAR"],
        }
    raise ValueError(f"unknown frame id 0x{fid:03X}")


def encode_message(msg, phys):
    """Clamp every physical value into the DBC range, quantize to raw with
    deterministic round-half-up (the emitter owns its rounding, not
    cantools'), and encode raw -- the same scaling=False call the golden
    generator uses, so the two sides quantize identically."""
    raw = {}
    for sig in msg.signals:
        v = float(phys[sig.name])
        lo = sig.minimum if sig.minimum is not None else sig.offset
        hi = (sig.maximum if sig.maximum is not None
              else sig.offset + sig.scale * ((1 << sig.length) - 1))
        v = min(max(v, lo), hi)
        r = int(math.floor((v - sig.offset) / sig.scale + 0.5))
        r = min(max(r, 0), (1 << sig.length) - 1)
        raw[sig.name] = r
    data = msg.encode(raw, scaling=False, padding=False)
    assert len(data) == 8, f"{msg.name}: encode length {len(data)}"
    return bytes(data)


# ---------------------------------------------------------------------------
# --dry-run-fixed: the unit's verification gate (plan U5 execution note).
# ---------------------------------------------------------------------------

def parse_golden_nominals():
    """Pull the four nominal_* vectors (id + 8 frame bytes) out of the
    generated golden header."""
    text = GOLDEN.read_text(encoding="ascii")
    pat = re.compile(
        r'\{\s*0x([0-9A-Fa-f]+)u,\s*(\d+)u,\s*(\d+)u,\s*'
        r'\{([^}]*)\},\s*(\d+)u,\s*(\d+)u,\s*"([^"]+)"',
        re.S,
    )
    out = {}
    for m in pat.finditer(text):
        name = m.group(7)
        if not name.startswith("nominal_"):
            continue
        fid = int(m.group(1), 16)
        ext = int(m.group(2))
        data = bytes(int(b, 16)
                     for b in re.findall(r"0x([0-9A-Fa-f]{2})u", m.group(4)))
        assert not ext and len(data) == 8, f"{name}: unexpected golden shape"
        out[fid] = (name, data)
    missing = [f"0x{fid:03X}" for fid in FRAME_IDS if fid not in out]
    assert not missing, f"golden header lacks nominal vectors for {missing}"
    return out


def run_dry_run_fixed(msgs):
    golden = parse_golden_nominals()
    failures = 0
    for fid in FRAME_IDS:
        data = encode_message(msgs[fid],
                              physical_for(fid, FIXED_VALUES, GOLDEN_FILLER))
        print(f"0x{fid:03X} {len(data)} {data.hex()}")
        name, want = golden[fid]
        if data == want:
            print(f"0x{fid:03X} PASS (matches golden {name})")
        else:
            failures += 1
            print(f"0x{fid:03X} FAIL: got {data.hex()} "
                  f"want {want.hex()} (golden {name})")
    if failures:
        log(f"{failures}/4 messages MISMATCH the golden nominal vectors")
        return 1
    log("all 4 messages match the golden nominal vectors byte-for-byte")
    return 0


# ---------------------------------------------------------------------------
# stdin reader thread: latest-values dict + last-line timestamp.
# ---------------------------------------------------------------------------

class Shared:
    def __init__(self):
        self.lock = threading.Lock()
        self.values = {}
        self.last_line = None  # perf_counter of the last parsed line
        self.eof = False
        self.reader_dead = False  # set on an unexpected reader-thread crash


def reader_loop(shared):
    stdin = sys.stdin
    try:
        # Deliver lines as they arrive (the feed is line-buffered for
        # exactly this); without it a stale burst arrives per buffer fill.
        stdin.reconfigure(line_buffering=True)
    except (AttributeError, ValueError):
        pass
    try:
        while True:
            line = stdin.readline()
            if line == "":
                with shared.lock:
                    shared.eof = True
                return
            vals = {}
            for tok in line.split():
                key, sep, txt = tok.partition("=")
                if not sep:
                    continue
                try:
                    vals[key] = float(txt)
                except ValueError:
                    continue
            if not vals:
                continue  # not a channel line; not a heartbeat either
            with shared.lock:
                # REPLACE wholesale: a key absent from the current line is a
                # channel the sim holds invalid, and must gate its message
                # rather than transmit its last value forever.
                shared.values = vals
                shared.last_line = time.perf_counter()
    except Exception:
        # Any unexpected crash here must not leave run_loop parked in the
        # feed-dead branch forever waiting for lines that will never come.
        log("reader thread crashed:\n" + traceback.format_exc())
        with shared.lock:
            shared.reader_dead = True


# ---------------------------------------------------------------------------
# Pacing loop (live TX and --dry-run share it; only the sink differs).
# ---------------------------------------------------------------------------

class BusDead(Exception):
    """Raised when a live bus.send() fails -- unwinds run_loop's pacing
    loop to the same shutdown/cleanup path KeyboardInterrupt uses."""


def run_loop(msgs, dry_run_limit, live):
    shared = Shared()
    threading.Thread(target=reader_loop, args=(shared,), daemon=True).start()

    can_mod, bus = (None, None)
    if live:
        can_mod, bus = open_bus()
        log(f"bus open: gs_usb channel 0 index 0 @ {BITRATE} bps")
    else:
        log("dry-run: frames print to stdout, nothing transmits")

    log("waiting for feed lines on stdin (feed-dead stop at "
        f"{int(FEED_DEAD_S * 1000)} ms of silence)...")

    next_due = {}
    sent = {fid: 0 for fid in FRAME_IDS}
    printed = 0
    running = False
    started_once = False
    gate_ok = {fid: None for fid in FRAME_IDS}  # None = never evaluated
    last_rate_log = time.perf_counter()
    rc = 0

    try:
        while True:
            now = time.perf_counter()
            with shared.lock:
                vals = shared.values
                last_line = shared.last_line
                eof = shared.eof
                reader_dead = shared.reader_dead

            if eof:
                log("feed EOF -- exiting")
                break

            if reader_dead:
                log("reader thread crashed -- exiting (no feed lines can "
                    "arrive; see traceback above) instead of hanging in "
                    "the feed-dead state forever")
                rc = 1
                break

            alive = (last_line is not None
                     and (now - last_line) <= FEED_DEAD_S)

            if running and not alive:
                log(f"feed dead (no line in {int(FEED_DEAD_S * 1000)} ms) "
                    "-- TX STOPPED (a dead feed must dead-front the glass)")
                running = False
            elif not running and alive:
                log("feed resumed -- TX resumed" if started_once
                    else "feed alive -- TX started")
                running = True
                started_once = True
                next_due = {fid: now for fid in FRAME_IDS}
                last_rate_log = now
                sent = {fid: 0 for fid in FRAME_IDS}

            if not running:
                time.sleep(0.02)
                continue

            # Per-message gate: every dash-sourced signal must be present.
            for fid in FRAME_IDS:
                ok = all(k in vals for k in FRAME_KEYS[fid])
                if ok != gate_ok[fid]:
                    if not ok:
                        missing = ",".join(k for k in FRAME_KEYS[fid]
                                           if k not in vals)
                        log(f"0x{fid:03X} gated: feed missing {missing}")
                    elif gate_ok[fid] is not None:
                        log(f"0x{fid:03X} ungated: feed carries "
                            "all its channels again")
                    gate_ok[fid] = ok

            soonest = min(next_due.values())
            if soonest > now:
                # Windows timer granularity ~15.6 ms: this oversleeps, and
                # the batch drain below makes up the difference.
                time.sleep(min(soonest - now, 0.05))
                continue

            for fid in FRAME_IDS:
                n = 0
                while next_due[fid] <= now and n < MAX_BATCH:
                    next_due[fid] += PERIOD_S[fid]
                    n += 1
                if next_due[fid] <= now:  # pathologically behind: resync
                    next_due[fid] = now + PERIOD_S[fid]
                if n == 0 or not gate_ok[fid]:
                    continue
                data = encode_message(msgs[fid],
                                      physical_for(fid, vals, LIVE_FILLER))
                for _ in range(n):
                    if bus is not None:
                        try:
                            bus.send(can_mod.Message(arbitration_id=fid,
                                                     is_extended_id=False,
                                                     data=data))
                        except can_mod.CanError as exc:
                            # A dead bus mid-soak must be loud, not silent:
                            # stop TX the same way the feed-dead path does
                            # and let the outer try/except exit nonzero
                            # after cleanup (bus.shutdown() in `finally`).
                            raise BusDead(str(exc)) from exc
                    else:
                        print(f"0x{fid:03X} {len(data)} {data.hex()}",
                              flush=True)
                        printed += 1
                    sent[fid] += 1
                if dry_run_limit and printed >= dry_run_limit:
                    log(f"dry-run limit reached ({dry_run_limit} frames) "
                        "-- exiting")
                    return 0

            if now - last_rate_log >= RATE_LOG_S:
                elapsed = now - last_rate_log
                parts = " ".join(f"0x{fid:03X} {sent[fid] / elapsed:.1f}/s"
                                 for fid in FRAME_IDS)
                log("rates: " + parts)
                sent = {fid: 0 for fid in FRAME_IDS}
                last_rate_log = now
    except BusDead as exc:
        log(f"bus.send failed: {exc} -- TX STOPPED (a dead bus mid-soak "
            "must be loud, not silent)")
        rc = 1
    except KeyboardInterrupt:
        log("interrupted -- shutting down")
    finally:
        if bus is not None:
            bus.shutdown()
    return rc


def main():
    ap = argparse.ArgumentParser(
        description="Ford-dialect CAN emitter: dash-sim feed lines on stdin "
                    "-> 0x270-set frames out the bench CANable (or stdout "
                    "with --dry-run).")
    ap.add_argument("--dry-run", nargs="?", type=int, const=0, default=None,
                    metavar="N",
                    help="encode + print frames instead of transmitting "
                         "(no CANable needed); optional N = stop after N "
                         "frames (0/omitted = until feed EOF)")
    ap.add_argument("--dry-run-fixed", action="store_true",
                    help="no stdin: encode the built-in golden-nominal "
                         "value set once and byte-compare against "
                         "tests/golden_can_ford.h (PASS/FAIL per message, "
                         "exit 1 on any mismatch)")
    args = ap.parse_args()

    db = cantools.database.load_file(DBC)
    msgs = load_messages(db)

    if args.dry_run_fixed:
        return run_dry_run_fixed(msgs)

    live = args.dry_run is None
    return run_loop(msgs, dry_run_limit=(args.dry_run or 0), live=live)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BrokenPipeError:
        # stdout consumer (head, a dying pager) went away: not an error.
        sys.exit(0)
