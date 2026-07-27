#!/usr/bin/env python3
"""Strip routable copper from Board3 so both routers start from identical state.

Board3 arrives fully routed -- 0 airwires, 2435 segments. There is no routing
work left to measure, so the comparison only exists if we deliberately create
it. This removes copper from every net NOT listed in tools/kicad_netclass.json,
leaving the audited and flagged nets untouched.

The strip is the fairness pivot. Whatever it produces is the one starting state
KiCad's router and EasyEDA's autorouter are both measured from; if they begin
from different copper their completion numbers describe different problems and
the head-to-head is void. That is why the census matters as much as the strip:
U7 has to reproduce it by hand in EasyEDA.

Census is the default. Nothing is deleted unless --out names an output board,
and the input file is never modified either way.

  python tools/kicad_strip.py kicad/board3/<board>.kicad_pcb
  python tools/kicad_strip.py kicad/board3/<board>.kicad_pcb --out stripped.kicad_pcb
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import pcbnew

NETCLASS = Path(__file__).with_name("kicad_netclass.json")

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_VERIFY_FAILED = 2


def load_excluded_nets(path: Path) -> tuple[set[str], dict[str, str]]:
    """Return (net names held out of routing, net -> group label)."""
    data = json.loads(path.read_text(encoding="utf-8"))
    nets: set[str] = set()
    group_of: dict[str, str] = {}
    for entry in data["exclusions"]:
        for net in entry["nets"]:
            nets.add(net)
            group_of[net] = f"{entry['group']} [{entry['status']}]"
    if not nets:
        raise ValueError(f"{path} lists no excluded nets -- refusing to strip everything")
    return nets, group_of


def census(board) -> dict[str, tuple[int, int, int]]:
    """net -> (tracks, vias, pads). Pads are the invariant: copper goes, membership stays."""
    out: dict[str, list[int]] = {}
    for t in board.GetTracks():
        row = out.setdefault(t.GetNetname(), [0, 0, 0])
        if t.Type() == pcbnew.PCB_VIA_T:
            row[1] += 1
        else:
            row[0] += 1
    for fp in board.GetFootprints():
        for p in fp.Pads():
            out.setdefault(p.GetNetname(), [0, 0, 0])[2] += 1
    return {k: tuple(v) for k, v in out.items()}


def airwires(board) -> int:
    conn = board.GetConnectivity()
    conn.RecalculateRatsnest()
    try:
        return conn.GetUnconnectedCount(True)
    except TypeError:
        return conn.GetUnconnectedCount()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board", help="Path to the baseline .kicad_pcb")
    ap.add_argument("--out", help="Write the stripped board here. Omit for census only.")
    ap.add_argument("--netclass", default=str(NETCLASS))
    args = ap.parse_args()

    src = Path(args.board)
    if not src.is_file():
        print(f"no such board: {src}", file=sys.stderr)
        return EXIT_ERROR

    try:
        excluded, group_of = load_excluded_nets(Path(args.netclass))
    except (OSError, ValueError, KeyError) as exc:
        print(f"netclass unusable: {exc}", file=sys.stderr)
        return EXIT_ERROR

    board = pcbnew.LoadBoard(str(src))
    before = census(board)
    before_air = airwires(board)

    # Nets the netclass names but the board does not have mean a stale netclass,
    # which would silently under-protect. Say so rather than ignoring it.
    missing = sorted(n for n in excluded if n not in before)
    routable = sorted(n for n, (tr, _, _) in before.items() if n and n not in excluded and tr)

    strip_tracks = sum(before[n][0] for n in routable)
    strip_vias = sum(before[n][1] for n in routable)
    keep_tracks = sum(before[n][0] for n in excluded if n in before)
    keep_vias = sum(before[n][1] for n in excluded if n in before)

    print(f"board          : {src.name}", flush=True)
    print(f"airwires before: {before_air}", flush=True)
    print(f"excluded nets  : {len(excluded)}  ({keep_tracks} tracks, {keep_vias} vias kept)", flush=True)
    print(f"routable nets  : {len(routable)}  ({strip_tracks} tracks, {strip_vias} vias to strip)", flush=True)
    if missing:
        print(f"WARNING: netclass names {len(missing)} net(s) absent from this board: {missing}", flush=True)

    print("\n-- held out --", flush=True)
    for n in sorted(excluded):
        if n in before:
            tr, vi, pd = before[n]
            print(f"   {n:<16} tracks={tr:<4} vias={vi:<3} pads={pd:<3} {group_of[n]}", flush=True)

    print("\n-- to be stripped (top 15 by track count) --", flush=True)
    for n in sorted(routable, key=lambda k: -before[k][0])[:15]:
        tr, vi, pd = before[n]
        print(f"   {n:<16} tracks={tr:<4} vias={vi:<3} pads={pd}", flush=True)

    if not args.out:
        print("\nCensus only. Pass --out <file> to write the stripped board.", flush=True)
        return EXIT_OK

    # Collect first, delete second: mutating while iterating GetTracks() skips items.
    routable_set = set(routable)
    doomed = [t for t in board.GetTracks() if t.GetNetname() in routable_set]
    for t in doomed:
        board.Remove(t)

    out = Path(args.out)
    pcbnew.SaveBoard(str(out), board)
    print(f"\nwrote {out} ({len(doomed)} primitives removed)", flush=True)

    # SaveBoard can crash the interpreter during teardown AFTER writing, so the
    # written file is the evidence, not the exit code. Re-read it to verify.
    check = pcbnew.LoadBoard(str(out))
    after = census(check)
    after_air = airwires(check)

    problems = []
    for n in excluded:
        if n in before and before[n][:2] != after.get(n, (0, 0, 0))[:2]:
            problems.append(f"excluded net {n} changed: {before[n][:2]} -> {after.get(n, (0,0,0))[:2]}")
    for n in routable:
        if after.get(n, (0, 0, 0))[0]:
            problems.append(f"routable net {n} still has {after[n][0]} tracks")
    for n, row in before.items():
        if n and row[2] != after.get(n, (0, 0, 0))[2]:
            problems.append(f"pad membership changed on {n}: {row[2]} -> {after.get(n, (0,0,0))[2]}")

    print(f"airwires after : {after_air}  (was {before_air})", flush=True)
    if problems:
        print("\nVERIFICATION FAILED:", flush=True)
        for p in problems[:20]:
            print(f"   {p}", flush=True)
        return EXIT_VERIFY_FAILED
    print("verification   : OK -- excluded copper intact, routable copper gone, "
          "pad membership unchanged", flush=True)
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
