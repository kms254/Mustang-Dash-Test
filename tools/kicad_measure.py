#!/usr/bin/env python3
"""Measure routed nets, and diff two boards net by net. U6's instrument.

Two questions this answers, both of which U6 asks repeatedly and U7/U9 will ask
again:

  1. "What did this net actually become?" -- segments, vias, total length, and
     length per copper layer. A signal-integrity intent (single layer, symmetric
     vias, matched length) is only claimed once it has been measured, never
     because a route "looks right" in the editor.

  2. "Did anything else move?" -- --against BOARD prints every net whose segment
     count, via count, length or layer set differs. Hand-routing four groups on a
     107-net board is only safe if the other 103 are provably untouched, and an
     autorouter run that quietly rips a neighbouring net reports success either
     way (U5 learned this the expensive way: its exclusion patterns matched
     nothing for a whole session).

Groups come from tools/kicad_netclass.json, so the measurement and the record of
what is held out of autorouting cannot drift apart.

  python tools/kicad_measure.py board.kicad_pcb                 # flagged groups
  python tools/kicad_measure.py board.kicad_pcb --nets CAN1_H,CAN1_L
  python tools/kicad_measure.py board.kicad_pcb --all
  python tools/kicad_measure.py routed.kicad_pcb --against before.kicad_pcb

Needs KiCad's bundled interpreter for `pcbnew`; if the interpreter running this
cannot import it, the script re-executes itself under the one kicad_env finds,
so `python tools/kicad_measure.py ...` works from any shell.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent
NETCLASS = HERE / "kicad_netclass.json"
NM = 1_000_000


def _reexec_under_kicad_python() -> None:
    """Re-run this script under an interpreter that has pcbnew, once."""
    if os.environ.get("KICAD_MEASURE_REEXEC"):
        raise SystemExit(
            "pcbnew is not importable even under KiCad's own interpreter. "
            "Check the KiCad install, or set KICAD_CLI to a working one."
        )
    sys.path.insert(0, str(HERE))
    from kicad_env import KiCadNotFound, find_kicad_python

    try:
        interpreter = find_kicad_python()
    except KiCadNotFound as exc:
        raise SystemExit(str(exc))
    env = dict(os.environ, KICAD_MEASURE_REEXEC="1")
    raise SystemExit(subprocess.run([str(interpreter), *sys.argv], env=env).returncode)


try:
    import pcbnew
except ImportError:
    _reexec_under_kicad_python()


def measure(board) -> dict:
    """Per-net geometry, keyed by net name with the hierarchy prefix stripped.

    Nets are keyed bare (USB_DP, not /USB_DP) because a KiCad "Update PCB from
    Schematic" renames every net to its hierarchical form, and a measurement that
    changed identity across that sync would compare nothing to nothing.
    """
    stats: dict = {}
    for track in board.Tracks():
        name = track.GetNetname().lstrip("/")
        rec = stats.setdefault(name, {"segments": 0, "vias": 0, "length_mm": 0.0,
                                      "layers": {}})
        if track.Type() == pcbnew.PCB_VIA_T:
            rec["vias"] += 1
            continue
        length = track.GetLength() / NM
        layer = board.GetLayerName(track.GetLayer())
        rec["segments"] += 1
        rec["length_mm"] += length
        rec["layers"][layer] = rec["layers"].get(layer, 0.0) + length
    for rec in stats.values():
        rec["length_mm"] = round(rec["length_mm"], 3)
        rec["layers"] = {k: round(v, 3) for k, v in
                         sorted(rec["layers"].items(), key=lambda kv: -kv[1])}
    return stats


def report(stats: dict, names: list, title: str = "") -> None:
    if title:
        print(f"\n{title}")
    for name in names:
        rec = stats.get(name.lstrip("/"))
        if rec is None:
            print(f"  {name:<16} (no copper)")
            continue
        layers = "  ".join(f"{k} {v}" for k, v in rec["layers"].items())
        print(f"  {name:<16} {rec['length_mm']:8.3f} mm  vias={rec['vias']}  "
              f"segs={rec['segments']:<4} {layers}")


def group_summary(stats: dict, group: dict) -> None:
    """Print a group plus the two facts its intent is usually judged on."""
    nets = group["nets"]
    print(f"\n[{group['group']}]  status={group['status']}")
    report(stats, nets)
    vias = {n: stats.get(n.lstrip("/"), {}).get("vias") for n in nets}
    lengths = [stats[n.lstrip("/")]["length_mm"] for n in nets if n.lstrip("/") in stats]
    layer_sets = {n: tuple(sorted(stats.get(n.lstrip("/"), {}).get("layers", {})))
                  for n in nets}
    distinct_vias = sorted({v for v in vias.values() if v is not None})
    print(f"   via counts     : {vias}"
          f"{'   SYMMETRIC' if len(distinct_vias) <= 1 else '   ASYMMETRIC'}")
    print(f"   layer sets     : "
          f"{'identical' if len(set(layer_sets.values())) == 1 else layer_sets}")
    if len(lengths) > 1:
        print(f"   length spread  : {max(lengths) - min(lengths):.3f} mm "
              f"(min {min(lengths):.3f}, max {max(lengths):.3f})")


def diff(new: dict, old: dict) -> int:
    changed = []
    for name in sorted(set(new) | set(old)):
        a, b = old.get(name), new.get(name)
        if a != b:
            changed.append((name, a, b))
    if not changed:
        print("no net changed")
        return 0
    print(f"{len(changed)} net(s) changed:")
    for name, a, b in changed:
        def fmt(r):
            if r is None:
                return "absent"
            return (f"{r['length_mm']:.3f} mm, {r['vias']} vias, "
                    f"{r['segments']} segs, {list(r['layers'])}")
        print(f"  {name}\n      before: {fmt(a)}\n      after : {fmt(b)}")
    return len(changed)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board")
    ap.add_argument("--against", help="Baseline board to diff against")
    ap.add_argument("--nets", help="Comma-separated net names instead of the groups")
    ap.add_argument("--all", action="store_true", help="Every net with copper")
    ap.add_argument("--json", action="store_true", help="Emit the raw measurements")
    args = ap.parse_args()

    board = pcbnew.LoadBoard(args.board)
    stats = measure(board)

    if args.json:
        print(json.dumps(stats, indent=2, sort_keys=True))
        return 0

    if args.against:
        diff(stats, measure(pcbnew.LoadBoard(args.against)))
        return 0

    if args.all:
        report(stats, sorted(stats), title=f"{Path(args.board).name}: every net")
        return 0

    if args.nets:
        report(stats, [n.strip() for n in args.nets.split(",")],
               title=f"{Path(args.board).name}")
        return 0

    spec = json.loads(NETCLASS.read_text(encoding="utf-8"))
    print(f"{Path(args.board).name}: groups from {NETCLASS.name}")
    for group in spec["exclusions"]:
        group_summary(stats, group)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
