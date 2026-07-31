#!/usr/bin/env python3
"""Route a KiCad board's unconnected nets with freerouting, headless.

Run under KiCad's own interpreter (needs pcbnew):

    "C:/Program Files/KiCad/10.0/bin/python.exe" tools/kicad_freeroute.py \
        BOARD.kicad_pcb --out ROUTED.kicad_pcb

Pipeline: lock all existing copper (in-memory) -> export Specctra DSN ->
run the freerouting JAR headless -> import the .ses session -> restore
lock state and any synthesized refs -> refill zones -> save to --out.

Locking is the scalpel: KiCad exports locked tracks/vias as `fix`-type
wires, which freerouting must not rip up, so only genuinely unrouted
nets get routed. Pass --no-lock to let freerouting rip up and optimize
the whole board (bulldozer mode).

Two board-specific traps this wrapper absorbs:
- Footprints with an empty reference (EasyEDA-import corner pads on
  Board3) make ExportSpecctraDSN return False with no message. They get
  synthetic XNOREF<n> refs for the export and are restored before save.
- ExportSpecctraDSN/ImportSpecctraSES swallow exceptions into a bool,
  so every step is checked and failure aborts before anything is saved.

This tool does not judge clearances. Gate the output with:

    python tools/kicad_verify.py OUT.kicad_pcb --baseline BASELINE.kicad_pcb

Exit codes: 0 = routed and saved, 1 = usage/setup error,
2 = freerouting or import failed (nothing saved).
"""

import argparse
import os
import subprocess
import sys
import tempfile

# 1.9.0 is the default on purpose: 2.2.4 (freerouting.jar here) hits an
# infinite recursion in PolylineTrace.combine() on this board's DSN
# (stacked same-net duplicate vias are the suspected trigger; 1.9.0
# logs "Multiple vias skipped" and carries on). Newer JREs run it fine.
DEFAULT_JAR = r"C:\Users\kevin\Tools\freerouting\freerouting-1.9.0.jar"
DEFAULT_JAVA_DIR = r"C:\Users\kevin\Tools\jre25-extract"

EXIT_OK = 0
EXIT_USAGE = 1
EXIT_FAILED = 2


def find_java(explicit):
    if explicit:
        return explicit
    env = os.environ.get("FREEROUTING_JAVA")
    if env:
        return env
    # Portable JRE installed 2026-07-29 (no system Java on this box).
    if os.path.isdir(DEFAULT_JAVA_DIR):
        for d in sorted(os.listdir(DEFAULT_JAVA_DIR)):
            cand = os.path.join(DEFAULT_JAVA_DIR, d, "bin", "java.exe")
            if os.path.isfile(cand):
                return cand
    return "java"  # PATH fallback


def find_jar(explicit):
    if explicit:
        return explicit
    return os.environ.get("FREEROUTING_JAR", DEFAULT_JAR)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board")
    ap.add_argument("--out", help="output board path (default: <board>-freerouted.kicad_pcb)")
    ap.add_argument("--in-place", action="store_true", help="overwrite the input board")
    ap.add_argument("--passes", type=int, default=20, help="freerouting max passes (-mp)")
    ap.add_argument("--no-lock", action="store_true",
                    help="do NOT lock existing copper; freerouting may rip up the whole board")
    ap.add_argument("--jar", help="freerouting jar (or FREEROUTING_JAR env)")
    ap.add_argument("--java", help="java executable (or FREEROUTING_JAVA env)")
    ap.add_argument("--timeout", type=int, default=1800, help="freerouting timeout, seconds")
    args = ap.parse_args()

    try:
        import pcbnew
    except ImportError:
        print("pcbnew not importable -- run under KiCad's python "
              r'("C:/Program Files/KiCad/10.0/bin/python.exe")')
        return EXIT_USAGE

    java = find_java(args.java)
    jar = find_jar(args.jar)
    if not os.path.isfile(jar):
        print("freerouting jar not found:", jar)
        return EXIT_USAGE
    if not os.path.isfile(args.board):
        print("board not found:", args.board)
        return EXIT_USAGE
    if args.in_place:
        out = args.board
    else:
        out = args.out or (os.path.splitext(args.board)[0] + "-freerouted.kicad_pcb")

    board = pcbnew.LoadBoard(args.board)

    # DSN export refuses footprints without a reference designator.
    noref = []
    for fp in board.GetFootprints():
        if not fp.GetReference().strip():
            noref.append(fp)
            fp.SetReference("XNOREF%d" % len(noref))
    if noref:
        print("synthesized refs for %d unnamed footprint(s) (export only)" % len(noref))

    pre_locked = sum(1 for t in board.GetTracks() if t.IsLocked())
    if not args.no_lock:
        for t in board.GetTracks():
            t.SetLocked(True)
        print("locked %d existing tracks/vias as fixed" % len(board.GetTracks()))

    tmpdir = tempfile.mkdtemp(prefix="freeroute-")
    dsn = os.path.join(tmpdir, "board.dsn")
    ses = os.path.join(tmpdir, "board.ses")
    if not (pcbnew.ExportSpecctraDSN(board, dsn) and os.path.isfile(dsn)):
        print("DSN export failed (exporter swallows the reason; check for "
              "unnamed footprints, exotic pad shapes)")
        return EXIT_FAILED

    # Freerouting's DSN reader pops a MODAL warning dialog on any non-ASCII
    # byte (each ohm-sign in a resistor value) and parks forever waiting for
    # a click -- even in batch mode, even with -dct. Strip them up front.
    with open(dsn, "rb") as f:
        raw = f.read()
    clean = bytes(c if c < 0x80 else ord("_") for c in raw)
    if clean != raw:
        with open(dsn, "wb") as f:
            f.write(clean)
        print("sanitized %d non-ASCII byte(s) out of the DSN" % sum(
            1 for c in raw if c >= 0x80))
    print("DSN exported:", dsn, os.path.getsize(dsn), "bytes")

    cmd = [java, "-jar", jar, "-de", dsn, "-do", ses, "-mp", str(args.passes)]
    print("running:", " ".join(cmd))
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        print("freerouting timed out after %ds -- nothing saved" % args.timeout)
        return EXIT_FAILED
    tail = (proc.stdout or "") + (proc.stderr or "")
    for line in tail.strip().splitlines()[-12:]:
        print("  fr:", line)
    if not os.path.isfile(ses):
        print("freerouting produced no session file (exit %s) -- nothing saved" % proc.returncode)
        return EXIT_FAILED

    if not pcbnew.ImportSpecctraSES(board, ses):
        print("SES import failed -- nothing saved")
        return EXIT_FAILED

    # Restore what the export borrowed: lock state and empty refs.
    if not args.no_lock and pre_locked == 0:
        for t in board.GetTracks():
            t.SetLocked(False)
    elif not args.no_lock:
        print("note: board had %d pre-locked items; lock state left as-is" % pre_locked)
    for fp in noref:
        fp.SetReference("")

    filler = pcbnew.ZONE_FILLER(board)
    filler.Fill(board.Zones())
    pcbnew.SaveBoard(out, board)

    conn = board.GetConnectivity()
    conn.RecalculateRatsnest()
    try:
        unrouted = conn.GetUnconnectedCount(True)
    except TypeError:
        unrouted = conn.GetUnconnectedCount()
    print("saved:", out)
    print("unrouted airwires after import:", unrouted)
    print("now gate it: python tools/kicad_verify.py \"%s\" --baseline <pre-change board>" % out)
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
