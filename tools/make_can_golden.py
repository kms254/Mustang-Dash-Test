#!/usr/bin/env python3
r"""Generate tests/golden_can_ford.h -- golden vectors for the Ford CAN dialect.

Loads the repo's single source of truth for the Ford control-pack broadcast
(assets/can/ford-control-pack-gen4.dbc -- the 0x270-set, transcribed from the
official Ford Performance tables: IS_M-6017-M50HM (Gen 4X, 09/16/24) p.36
"CAN Message Definition" and IS_M-6017-73M (7.3L, 02/21/23) s.12.0 p.29,
byte-identical, performanceparts.ford.com/download/instructionsheets/),
ENCODES test frames with cantools, and emits a checked-in C header of frame
bytes plus expected post-conversion dash-channel values.

Expected values are computed HERE, from the spec constants restated below
(degF = degC*9/5+32, psi = kPa*0.145037738, mph = km/h / 1.60934;
rpm/AFR/volts pass through) -- never by calling or reimplementing the C
decoder's extraction (plan KTD8: a test that pins the decoder's own output
proves nothing). The DBC places bits, this file restates the scaling, and
MustangDash/dash_can_ford.h is a third transcription of the same tables --
a transcription slip must reproduce identically in two of the three to hide.

Deterministic: re-running must produce a byte-identical header (no
timestamps, no dict-order dependence, LF-only writes). On DBC edits,
regenerate; NEVER widen the emitted tolerance constants -- re-derive them
from the spec quantum instead (the re-derive-the-constant rule).

Run on the PIO penv python -- this box has NO system python (only the
Microsoft Store alias stub) and cantools lives in the penv:

  C:\Users\kevin\.platformio\penv\Scripts\python.exe tools/make_can_golden.py

Plan: docs/plans/2026-08-15-001-feat-can-telemetry-ford-dialect-plan.md (U2).
"""

import sys
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
OUT = ROOT / "tests" / "golden_can_ford.h"

# ---------------------------------------------------------------------------
# Spec constants (Ford tables + the DashState unit contract: mph/degF/psi).
# Expected values derive from THESE, never from the DBC object (KTD8).
# ---------------------------------------------------------------------------
KPA_TO_PSI = 0.145037738  # psi per kPa
KMH_PER_MPH = 1.60934     # km/h per mph (the PCM encodes VSPD as mph*1.60934)


def rpm_from_raw(raw):
    """ENGINE_SPEED: 1 rpm/bit, pass-through."""
    return float(raw)


def mph_from_raw(raw):
    """VSPD: 0.1 km/h per bit -> mph."""
    return (raw * 0.1) / KMH_PER_MPH


def degf_from_raw(raw):
    """ECT/EOT: degC = raw - 40 -> degF."""
    return (raw - 40.0) * 9.0 / 5.0 + 32.0


def psi_from_raw_kpa(raw):
    """EOP / FUEL_PRESSURE: 1 kPa per bit -> psi."""
    return raw * KPA_TO_PSI


def volts_from_raw(raw):
    """VBAT: 0.01 V/bit, pass-through."""
    return raw * 0.01


def afr_from_raw(raw):
    """AF0/AF1: A/F = raw*0.01 + 7, pass-through."""
    return raw * 0.01 + 7.0


# ECT/EOT sentinel raw bytes (KTD3). Factor 1, offset -40: physical 174 degC
# encodes raw 214 (degraded), physical 175 degC encodes raw 215 (faulted).
SENTINEL_DEGRADED = 214
SENTINEL_FAULTED = 215

# ---------------------------------------------------------------------------
# Tolerance families -- TWO, deliberately not one (tolerance independence).
#
# TIGHT (GOLDEN_CAN_TIGHT_*): for golden DECODE comparisons, where this
# generator's double math and the C decoder's float math run over the SAME
# raw integers -- the only legitimate divergence is float rounding, bounded
# well under 1e-4 in every channel's units at range max (worst case: SPEED,
# a few float ULP at 254 mph ~ 5e-5). 1e-3 keeps >= 10x headroom over that
# while sitting strictly below HALF a raw quantum on every channel (smallest
# half-quantum: 0.005, VOLTS/AFR), so a decoder systematically biased by one
# quantum -- the exact bias a one-quantum tolerance used to absorb -- FAILS.
# NEVER widen to make a test pass: a golden assertion failing at TIGHT is a
# real transcription divergence in one of the three transcriptions.
#
# QUANTUM (GOLDEN_CAN_TOL_*): for the R11 sim round-trip ONLY, where the
# test-side encoder quantizes the sim's float channels to raw counts before
# the decoder ever sees them -- ONE raw quantum (DBC scaling) through the
# unit conversion is inherent there, and only there. (name, value, comment)
# ---------------------------------------------------------------------------
TIGHT = 1e-3  # channel units; see the derivation above

TIGHT_TOLERANCES = [
    ("GOLDEN_CAN_TIGHT_RPM", TIGHT, "half-quantum 0.5 rpm"),
    ("GOLDEN_CAN_TIGHT_SPEED_MPH", TIGHT,
     f"half-quantum {0.05 / KMH_PER_MPH:.6g} mph"),
    ("GOLDEN_CAN_TIGHT_ECT_F", TIGHT, "half-quantum 0.9 degF"),
    ("GOLDEN_CAN_TIGHT_OILT_F", TIGHT, "half-quantum 0.9 degF"),
    ("GOLDEN_CAN_TIGHT_OILP_PSI", TIGHT,
     f"half-quantum {KPA_TO_PSI / 2:.6g} psi"),
    ("GOLDEN_CAN_TIGHT_VOLTS", TIGHT, "half-quantum 0.005 V"),
    ("GOLDEN_CAN_TIGHT_AFR", TIGHT, "half-quantum 0.005 A/F, both banks"),
    ("GOLDEN_CAN_TIGHT_FUELP_PSI", TIGHT,
     f"half-quantum {KPA_TO_PSI / 2:.6g} psi"),
]

TOLERANCES = [
    ("GOLDEN_CAN_TOL_RPM", 1.0, "1 rpm/bit, pass-through"),
    ("GOLDEN_CAN_TOL_SPEED_MPH", 0.1 / KMH_PER_MPH, "0.1 km/h quantum -> mph"),
    ("GOLDEN_CAN_TOL_ECT_F", 1.8, "1 degC quantum -> degF"),
    ("GOLDEN_CAN_TOL_OILT_F", 1.8, "1 degC quantum -> degF"),
    ("GOLDEN_CAN_TOL_OILP_PSI", KPA_TO_PSI, "1 kPa quantum -> psi"),
    ("GOLDEN_CAN_TOL_VOLTS", 0.01, "0.01 V/bit, pass-through"),
    ("GOLDEN_CAN_TOL_AFR", 0.01, "0.01 A/F per bit, both banks"),
    ("GOLDEN_CAN_TOL_FUELP_PSI", KPA_TO_PSI, "1 kPa quantum -> psi"),
]

# ---------------------------------------------------------------------------
# Vector set (plan U2): nominal per channel, min/max range edges, the 214/215
# sentinels, unknown-ID, short-DLC, the 29-bit-ID-value fall-through, and a
# realistic burst.
#
# Each entry: name, comment, then EITHER msg+raw (encoded via cantools with
# scaling=False -- raw signal values, so the generator, not float rounding,
# owns every bit) OR frame_id+raw_bytes for non-DBC frames. expects is a list
# of (DASH_CH_* identifier, expected dash value | None = must be INVALID).
# ---------------------------------------------------------------------------

# Shared 0x278 nominal raw values (the sentinel vectors override one byte
# each, so the not-invalidated signals in the same frame are provably live).
NOM_0278 = {
    "ECT": 130,          # 90 degC -> 194 degF
    "EOT": 120,          # 80 degC -> 176 degF
    "TOT": 125,          # 85 degC, undecoded (no dash channel)
    "EOP": 340,          # kPa -> psi
    "SHIFTER_POS": 4,    # undecoded
    "CODES_COUNT": 0,    # undecoded
    "VBAT": 1330,        # 13.30 V
    "GEAR": 3,           # undecoded
}

NOM_0278_EXPECTS = [
    ("DASH_CH_ECT", degf_from_raw(130)),
    ("DASH_CH_OILT", degf_from_raw(120)),
    ("DASH_CH_OILP", psi_from_raw_kpa(340)),
    ("DASH_CH_VOLTS", volts_from_raw(1330)),
]

VECTORS = [
    # --- nominal, one realistic frame per message -------------------------
    dict(
        name="nominal_0270_rpm",
        comment="nominal 0x270: 3500 rpm, raw 3500 -> bytes 0-1 = 0x0D 0xAC "
                "(the known-good cross-check). ENGINE_SPEED_HZ = rpm/60 (raw "
                "5833), MAN_VAC = -30 kPa (raw 750), DI_PRESSURE = 11000 kPa: "
                "populated for bit-masking realism, all undecoded.",
        msg="FORD_0270_ENGINE",
        raw={"ENGINE_SPEED": 3500, "ENGINE_SPEED_HZ": 5833,
             "MAN_VAC": 750, "DI_PRESSURE": 11000},
        expects=[("DASH_CH_RPM", rpm_from_raw(3500))],
    ),
    dict(
        name="nominal_0274_afr_fuelp",
        comment="nominal 0x274: AF0 raw 560 -> 12.6, AF1 raw 510 -> 12.1 "
                "(power mixture), FUEL_PRESSURE 340 kPa, BOOST 0 (NA car, "
                "undecoded).",
        msg="FORD_0274_FUEL_AFR",
        raw={"BOOST": 0, "AF0": 560, "AF1": 510, "FUEL_PRESSURE": 340},
        expects=[("DASH_CH_AFR_L", afr_from_raw(560)),
                 ("DASH_CH_AFR_R", afr_from_raw(510)),
                 ("DASH_CH_FUELP", psi_from_raw_kpa(340))],
    ),
    dict(
        name="nominal_0275_speed",
        comment="nominal 0x275: VSPD raw 1194 = 119.4 km/h -> ~74.2 mph.",
        msg="FORD_0275_SPEED",
        raw={"VSPD": 1194},
        expects=[("DASH_CH_SPEED", mph_from_raw(1194))],
    ),
    dict(
        name="nominal_0278_temps",
        comment="nominal 0x278: ECT 90 degC -> 194 degF, EOT 80 degC -> "
                "176 degF, EOP 340 kPa -> psi, VBAT 13.30 V; TOT/SHIFTER/"
                "CODES/GEAR populated but undecoded.",
        msg="FORD_0278_TEMPS",
        raw=dict(NOM_0278),
        expects=list(NOM_0278_EXPECTS),
    ),
    # --- min range edges: all-zero raw ------------------------------------
    dict(
        name="min_0270",
        comment="min edge 0x270: all-zero raw. RPM 0; MAN_VAC raw 0 would be "
                "-105 kPa but is undecoded, just 0 raw here.",
        msg="FORD_0270_ENGINE",
        raw={"ENGINE_SPEED": 0, "ENGINE_SPEED_HZ": 0,
             "MAN_VAC": 0, "DI_PRESSURE": 0},
        expects=[("DASH_CH_RPM", rpm_from_raw(0))],
    ),
    dict(
        name="min_0274",
        comment="min edge 0x274: all-zero raw. AFR floor is the +7 offset "
                "(raw 0 -> 7.0 A/F, not zero); FUELP 0 kPa -> 0 psi.",
        msg="FORD_0274_FUEL_AFR",
        raw={"BOOST": 0, "AF0": 0, "AF1": 0, "FUEL_PRESSURE": 0},
        expects=[("DASH_CH_AFR_L", afr_from_raw(0)),
                 ("DASH_CH_AFR_R", afr_from_raw(0)),
                 ("DASH_CH_FUELP", psi_from_raw_kpa(0))],
    ),
    dict(
        name="min_0275",
        comment="min edge 0x275: VSPD raw 0 -> 0 mph.",
        msg="FORD_0275_SPEED",
        raw={"VSPD": 0},
        expects=[("DASH_CH_SPEED", mph_from_raw(0))],
    ),
    dict(
        name="min_0278",
        comment="min edge 0x278: all-zero raw. ECT/EOT raw 0 = -40 degC = "
                "-40 degF (valid, NOT a sentinel); EOP 0 psi; VBAT 0.00 V.",
        msg="FORD_0278_TEMPS",
        raw={"ECT": 0, "EOT": 0, "TOT": 0, "EOP": 0, "SHIFTER_POS": 0,
             "CODES_COUNT": 0, "VBAT": 0, "GEAR": 0},
        expects=[("DASH_CH_ECT", degf_from_raw(0)),
                 ("DASH_CH_OILT", degf_from_raw(0)),
                 ("DASH_CH_OILP", psi_from_raw_kpa(0)),
                 ("DASH_CH_VOLTS", volts_from_raw(0))],
    ),
    # --- max range edges: every signal at raw max (adjacent-bit armor) ----
    dict(
        name="max_0270",
        comment="max edge 0x270: every signal at raw max -- RPM 16383 plus "
                "saturated undecoded neighbors, so a decoder mask slip reads "
                "garbage instead of zeros.",
        msg="FORD_0270_ENGINE",
        raw={"ENGINE_SPEED": 16383, "ENGINE_SPEED_HZ": 16383,
             "MAN_VAC": 4095, "DI_PRESSURE": 32767},
        expects=[("DASH_CH_RPM", rpm_from_raw(16383))],
    ),
    dict(
        name="max_0274",
        comment="max edge 0x274: AF0/AF1 raw 2047 -> 27.47 A/F, "
                "FUEL_PRESSURE 511 kPa, BOOST raw max 511 (undecoded).",
        msg="FORD_0274_FUEL_AFR",
        raw={"BOOST": 511, "AF0": 2047, "AF1": 2047, "FUEL_PRESSURE": 511},
        expects=[("DASH_CH_AFR_L", afr_from_raw(2047)),
                 ("DASH_CH_AFR_R", afr_from_raw(2047)),
                 ("DASH_CH_FUELP", psi_from_raw_kpa(511))],
    ),
    dict(
        name="max_0275",
        comment="max edge 0x275: VSPD raw 4095 = 409.5 km/h -> ~254.5 mph.",
        msg="FORD_0275_SPEED",
        raw={"VSPD": 4095},
        expects=[("DASH_CH_SPEED", mph_from_raw(4095))],
    ),
    dict(
        name="max_0278",
        comment="max edge 0x278: ECT/EOT raw 255 = 215 degC = 419 degF -- "
                "ABOVE the 214/215 sentinel bytes, must convert, not "
                "invalidate (KTD3: only raw 214/215 dead-front). EOP raw "
                "1023 kPa, VBAT raw 2047 -> 20.47 V, others at raw max.",
        msg="FORD_0278_TEMPS",
        raw={"ECT": 255, "EOT": 255, "TOT": 255, "EOP": 1023,
             "SHIFTER_POS": 15, "CODES_COUNT": 255, "VBAT": 2047, "GEAR": 15},
        expects=[("DASH_CH_ECT", degf_from_raw(255)),
                 ("DASH_CH_OILT", degf_from_raw(255)),
                 ("DASH_CH_OILP", psi_from_raw_kpa(1023)),
                 ("DASH_CH_VOLTS", volts_from_raw(2047))],
    ),
    # --- sentinels (KTD3): one channel dead-fronts, frame-mates stay live -
    dict(
        name="sentinel_ect_degraded",
        comment="sentinel: ECT raw 214 (byte 0 = 0xD6, physical 174 degC = "
                "'sensor degraded') -> DASH_CH_ECT must be INVALID, never "
                "converted; EOT/EOP/VBAT in the SAME frame stay valid.",
        msg="FORD_0278_TEMPS",
        raw={**NOM_0278, "ECT": SENTINEL_DEGRADED},
        expects=[("DASH_CH_ECT", None),
                 ("DASH_CH_OILT", degf_from_raw(120)),
                 ("DASH_CH_OILP", psi_from_raw_kpa(340)),
                 ("DASH_CH_VOLTS", volts_from_raw(1330))],
    ),
    dict(
        name="sentinel_eot_faulted",
        comment="sentinel: EOT raw 215 (byte 1 = 0xD7, physical 175 degC = "
                "'sensor faulted') -> DASH_CH_OILT must be INVALID; ECT/EOP/"
                "VBAT in the SAME frame stay valid.",
        msg="FORD_0278_TEMPS",
        raw={**NOM_0278, "EOT": SENTINEL_FAULTED},
        expects=[("DASH_CH_ECT", degf_from_raw(130)),
                 ("DASH_CH_OILT", None),
                 ("DASH_CH_OILP", psi_from_raw_kpa(340)),
                 ("DASH_CH_VOLTS", volts_from_raw(1330))],
    ),
    # --- frames the decoder must ignore without touching state ------------
    dict(
        name="unknown_id_0x123",
        comment="unknown ID 0x123: not in the dialect, must be ignored "
                "(state untouched).",
        frame_id=0x123,
        raw_bytes=[0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88],
        expects=[],
    ),
    dict(
        name="short_dlc_0270",
        comment="short DLC: 0x270 with dlc 4 (first 4 bytes of a 6000 rpm "
                "frame), must be ignored -- a truncated frame proves "
                "nothing about any signal.",
        msg="FORD_0270_ENGINE",
        raw={"ENGINE_SPEED": 6000, "ENGINE_SPEED_HZ": 10000,
             "MAN_VAC": 1050, "DI_PRESSURE": 9000},
        dlc=4,
        expects=[],
    ),
    dict(
        name="ext_29bit_fallthrough",
        comment="29-bit-only frame whose identifier VALUE is 0x270 (7250 "
                "rpm payload). The decoder takes no ID-type input, so ext "
                "REJECTION lives in the firmware drain's standard-only "
                "guard (U6), reachable only on the live-bus checklist. "
                "What the host suite tests here: fed as its full 29-bit "
                "identifier value, the frame falls through the ID switch "
                "like any unknown ID. It must never be fed as the "
                "truncated 11-bit value -- the decoder would consume it.",
        msg="FORD_0270_ENGINE",
        raw={"ENGINE_SPEED": 7250, "ENGINE_SPEED_HZ": 12083,
             "MAN_VAC": 1050, "DI_PRESSURE": 12000},
        ext=1,
        expects=[],
    ),
    # --- realistic burst: all four frames, one consistent instant ---------
    # Corner exit at HPR: 4800 rpm, 3rd gear, ~75 mph, near-WOT, warm car.
    dict(
        name="burst_0270",
        comment="burst 1/4 (corner exit, consistent instant): 4800 rpm; "
                "HZ = 80.00 (raw 8000); MAN_VAC -2 kPa near-WOT (raw 1030); "
                "DI 12000 kPa.",
        msg="FORD_0270_ENGINE",
        raw={"ENGINE_SPEED": 4800, "ENGINE_SPEED_HZ": 8000,
             "MAN_VAC": 1030, "DI_PRESSURE": 12000},
        expects=[("DASH_CH_RPM", rpm_from_raw(4800))],
    ),
    dict(
        name="burst_0274",
        comment="burst 2/4: AF0 12.5 / AF1 12.4 under load, fuel 380 kPa.",
        msg="FORD_0274_FUEL_AFR",
        raw={"BOOST": 0, "AF0": 550, "AF1": 540, "FUEL_PRESSURE": 380},
        expects=[("DASH_CH_AFR_L", afr_from_raw(550)),
                 ("DASH_CH_AFR_R", afr_from_raw(540)),
                 ("DASH_CH_FUELP", psi_from_raw_kpa(380))],
    ),
    dict(
        name="burst_0275",
        comment="burst 3/4: 120.6 km/h (raw 1206) -> ~74.9 mph, consistent "
                "with 4800 rpm in 3rd (3.73 final, 315/30R18).",
        msg="FORD_0275_SPEED",
        raw={"VSPD": 1206},
        expects=[("DASH_CH_SPEED", mph_from_raw(1206))],
    ),
    dict(
        name="burst_0278",
        comment="burst 4/4: ECT 95 degC -> 203 degF, EOT 105 degC -> "
                "221 degF (oil hotter than coolant on track), EOP 450 kPa, "
                "VBAT 13.80 V, 3rd gear.",
        msg="FORD_0278_TEMPS",
        raw={"ECT": 135, "EOT": 145, "TOT": 130, "EOP": 450,
             "SHIFTER_POS": 4, "CODES_COUNT": 0, "VBAT": 1380, "GEAR": 3},
        expects=[("DASH_CH_ECT", degf_from_raw(135)),
                 ("DASH_CH_OILT", degf_from_raw(145)),
                 ("DASH_CH_OILP", psi_from_raw_kpa(450)),
                 ("DASH_CH_VOLTS", volts_from_raw(1380))],
    ),
]


def cf(v):
    """Format a float as a deterministic C float literal."""
    s = f"{float(v):.9g}"
    if not any(c in s for c in ".eE"):
        s += ".0"
    return s + "f"


def wrap_comment(text, indent, width=76):
    """Wrap text into C comment lines at the given indent."""
    import textwrap
    body = textwrap.wrap(text, width=width - len(indent) - 3)
    lines = [f"{indent}/* {body[0]}"]
    lines += [f"{indent} * {ln}" for ln in body[1:]]
    lines[-1] += " */"
    return lines


def encode_vectors(db):
    """Fill id/dlc/bytes on every vector; return them checked."""
    msgs = {m.name: m for m in db.messages}
    # Sanity: the four ids and DLCs match the official table.
    expect_ids = {"FORD_0270_ENGINE": 0x270, "FORD_0274_FUEL_AFR": 0x274,
                  "FORD_0275_SPEED": 0x275, "FORD_0278_TEMPS": 0x278}
    for name, fid in expect_ids.items():
        m = msgs[name]
        assert m.frame_id == fid and m.length == 8, \
            f"{name}: DBC id/DLC drifted from the official table"

    for v in VECTORS:
        v.setdefault("ext", 0)
        v.setdefault("dlc", 8)
        if "msg" in v:
            m = msgs[v["msg"]]
            data = m.encode(v["raw"], scaling=False, padding=False)
            assert len(data) == 8, f"{v['name']}: encode length {len(data)}"
            v["id"] = m.frame_id
            b = bytearray(8)
            b[: v["dlc"]] = data[: v["dlc"]]  # short-DLC: truncate, pad 0
            v["bytes"] = bytes(b)
        else:
            v["id"] = v["frame_id"]
            v["bytes"] = bytes(v["raw_bytes"])
            assert len(v["bytes"]) == 8

    byname = {v["name"]: v for v in VECTORS}
    # Known-good cross-checks (independent of cantools' own claims):
    # 3500 rpm in Ford bits 2-15 must land 0x0D 0xAC in bytes 0-1.
    assert byname["nominal_0270_rpm"]["bytes"][:2] == b"\x0d\xac", \
        "RPM 3500 cross-check failed: ENGINE_SPEED bit placement drifted"
    # Sentinel bytes must sit exactly where the decoder will look.
    assert byname["sentinel_ect_degraded"]["bytes"][0] == SENTINEL_DEGRADED
    assert byname["sentinel_ect_degraded"]["bytes"][1] == NOM_0278["EOT"]
    assert byname["sentinel_eot_faulted"]["bytes"][1] == SENTINEL_FAULTED
    assert byname["sentinel_eot_faulted"]["bytes"][0] == NOM_0278["ECT"]
    return VECTORS


def emit(vectors):
    n_expects = sum(len(v["expects"]) for v in vectors)
    out = []
    w = out.append
    w("/* golden_can_ford.h -- GENERATED by tools/make_can_golden.py; "
      "do not edit.")
    w(" * Golden CAN vectors for the Ford control-pack dialect "
      "(the 0x270-set).")
    w(" *")
    w(" * Frames were ENCODED with cantools from the repo's source of truth,")
    w(" * assets/can/ford-control-pack-gen4.dbc, itself transcribed from the")
    w(" * official Ford Performance tables: IS_M-6017-M50HM (Gen 4X, "
      "09/16/24)")
    w(" * p.36 \"CAN Message Definition\" and IS_M-6017-73M (7.3L, 02/21/23)")
    w(" * s.12.0 p.29 (byte-identical tables),")
    w(" * performanceparts.ford.com/download/instructionsheets/.")
    w(" *")
    w(" * Expected channel values were computed inside the generator from "
      "the")
    w(" * spec constants (degF = degC*9/5+32, psi = kPa*0.145037738,")
    w(" * mph = km/h / 1.60934; rpm/AFR/volts pass through) -- never from "
      "the")
    w(" * C decoder, so the DBC encode and the dash_can_ford.h decode stay")
    w(" * independent transcriptions of the same tables (plan KTD8).")
    w(" */")
    w("")
    w("#pragma once")
    w("")
    w("#include <stdint.h>")
    w("")
    w("#include \"dash_data.h\" /* DASH_CH_* ids; tests compile with "
      "-I MustangDash */")
    w("")
    w("/* TIGHT decode tolerances -- golden decode comparisons ONLY. The")
    w(" * generator's double math and the decoder's float math run over the")
    w(" * SAME raw integers, so only float rounding may differ (< 1e-4 in")
    w(" * every channel's units at range max). 0.001 keeps >= 10x headroom")
    w(" * over that while sitting strictly below HALF a raw quantum on every")
    w(" * channel, so a decoder systematically biased by one quantum FAILS.")
    w(" * NEVER widen to make a test pass: a golden assertion failing at")
    w(" * TIGHT is a real transcription divergence. */")
    for name, val, comment in TIGHT_TOLERANCES:
        w(f"#define {name} {cf(val)} /* {comment} */")
    w("")
    w("/* QUANTUM round-trip tolerances -- the R11 sim round-trip ONLY: its")
    w(" * test-side encoder quantizes the sim's float channels to raw counts")
    w(" * before the decoder sees them, so one raw quantum (DBC scaling)")
    w(" * through the unit conversion is inherent there, and only there.")
    w(" * Golden decode comparisons must use the TIGHT family above. Derived")
    w(" * once from the spec -- NEVER widen; re-derive on any scaling change")
    w(" * (KTD8). */")
    for name, val, comment in TOLERANCES:
        w(f"#define {name} {cf(val)} /* {comment} */")
    w("")
    w("/* Expected post-conversion outcome of decoding one vector.")
    w(" * expect_valid 1: the channel must be valid and match value within "
      "its")
    w(" * GOLDEN_CAN_TIGHT_* bound. expect_valid 0: the channel must be "
      "INVALID")
    w(" * after this frame (sentinel dead-front); value is 0 and unused. */")
    w("typedef struct {")
    w("    uint8_t ch;           /* DASH_CH_* id (dash_data.h) */")
    w("    uint8_t expect_valid;")
    w("    float value;          /* dash units: rpm/mph/degF/psi/V/AFR */")
    w("} GoldenCanExpect;")
    w("")
    w("/* One raw frame plus its expectation slice in golden_can_expects[].")
    w(" * n_expect 0 = the frame must be IGNORED (DashState untouched). */")
    w("typedef struct {")
    w("    uint16_t id;          /* CAN identifier value */")
    w("    uint8_t ext;          /* 1 = frame exists ONLY as 29-bit. The")
    w("                             decoder has no ID-type input, so hosts")
    w("                             feed its FULL 29-bit identifier VALUE")
    w("                             (falls through the ID switch); IdType")
    w("                             rejection itself is the firmware drain's")
    w("                             (U6), live-bus checklist only. */")
    w("    uint8_t dlc;")
    w("    uint8_t bytes[8];     /* dlc bytes meaningful; rest zero */")
    w("    uint8_t first_expect; /* index into golden_can_expects[] */")
    w("    uint8_t n_expect;")
    w("    const char *name;     /* for test diagnostics */")
    w("} GoldenCanVector;")
    w("")
    w(f"#define GOLDEN_CAN_VECTOR_COUNT {len(vectors)}u")
    w(f"#define GOLDEN_CAN_EXPECT_COUNT {n_expects}u")
    w("")

    # --- expects array ----------------------------------------------------
    w("static const GoldenCanExpect golden_can_expects[] = {")
    idx = 0
    for v in vectors:
        v["first"] = idx
        if not v["expects"]:
            continue
        lo, hi = idx, idx + len(v["expects"]) - 1
        w(f"    /* [{lo}..{hi}] {v['name']} */")
        for ch, val in v["expects"]:
            if val is None:
                w(f"    {{ {ch}, 0u, 0.0f }},")
            else:
                w(f"    {{ {ch}, 1u, {cf(val)} }},")
        idx += len(v["expects"])
    w("};")
    w("")

    # --- vectors array ------------------------------------------------------
    w("static const GoldenCanVector golden_can_vectors[] = {")
    for i, v in enumerate(vectors):
        out.extend(wrap_comment(f"[{i}] {v['comment']}", "    "))
        byte_lit = ", ".join(f"0x{b:02X}u" for b in v["bytes"])
        w(f"    {{ 0x{v['id']:03X}u, {v['ext']}u, {v['dlc']}u,")
        w(f"      {{ {byte_lit} }},")
        w(f"      {v['first']}u, {len(v['expects'])}u, \"{v['name']}\" }},")
    w("};")
    w("")
    w("_Static_assert(sizeof golden_can_vectors / sizeof golden_can_vectors[0]")
    w("                   == GOLDEN_CAN_VECTOR_COUNT,")
    w("               \"vector count drifted\");")
    w("_Static_assert(sizeof golden_can_expects / sizeof golden_can_expects[0]")
    w("                   == GOLDEN_CAN_EXPECT_COUNT,")
    w("               \"expect count drifted\");")
    w("")

    # --- trailing summary table --------------------------------------------
    w("/* Vector summary:")
    for i, v in enumerate(vectors):
        idtxt = f"0x{v['id']:03X}" + (" EXT" if v["ext"] else "")
        hexbytes = " ".join(f"{b:02x}" for b in v["bytes"])
        w(f" * [{i:2d}] {v['name']:<22} {idtxt:<9} dlc={v['dlc']}  "
          f"{hexbytes}")
        if v["expects"]:
            parts = []
            for ch, val in v["expects"]:
                short = ch.replace("DASH_CH_", "")
                parts.append(f"{short}=INVALID" if val is None
                             else f"{short}={float(val):.6g}")
            w(f" *       -> {' '.join(parts)}")
        else:
            w(" *       -> (must be ignored; DashState untouched)")
    w(" */")
    return "\n".join(out) + "\n"


def main():
    db = cantools.database.load_file(DBC)
    vectors = encode_vectors(db)
    text = emit(vectors)
    text.encode("ascii")  # the header must stay pure ASCII
    with open(OUT, "w", newline="\n", encoding="ascii") as f:
        f.write(text)
    n_expects = sum(len(v["expects"]) for v in vectors)
    print(f"  {len(vectors)} vectors, {n_expects} expectations "
          f"-> {OUT.relative_to(ROOT)} ({len(text)} B)")


if __name__ == "__main__":
    main()
