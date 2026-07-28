#!/usr/bin/env python3
"""Apply a declarative copper edit to a .kicad_pcb, then refill zones correctly.

KTD5 says the flagged signal-integrity nets are hand-routed, never autorouted.
This is what "by hand" means here: the route is written down as coordinates,
reviewed as text, applied atomically, and measured afterwards with
tools/kicad_measure.py. Nothing is dragged in the editor, so nothing is
unreproducible.

  python tools/kicad_handroute.py board.kicad_pcb edit.json [--dry-run]

An edit is a single spec, or {"steps": [spec, spec, ...]} applied in order:

  {
    "move_footprints": [{"ref": "X1", "by": [0.5, 0.0]}],
    "delete":   [{"net": "CAN1_H", "bbox": [x1,y1,x2,y2], "layers": ["Bottom Layer"],
                  "vias": true}],
    "add":      [{"net": "CAN1_H", "layer": "Top Layer", "width": 0.254,
                  "points": [[x,y], [x,y], ...]}],
    "add_vias": [{"net": "USB_DM_CONN", "at": [x,y], "diameter": 0.6, "drill": 0.3}]
  }

Deletion semantics: a track is removed only when BOTH endpoints are inside bbox,
a via when its centre is. A segment that merely passes through survives --
deleting those silently orphans copper outside the region you meant to touch.
Omit "bbox" to match a whole net, "layers" to match any layer.

Three traps this tool exists to absorb
--------------------------------------
1. THE REFILL MUST USE THE REAL RULES. Zone fills are clearance-dependent, and
   the tracked .kicad_pro still carries the importer's factory defaults
   (clearance 0.2 mm against the board's real 0.1016 -- see KTD2). Filling under
   those pulls every pour back an extra 0.1 mm, which moves thermal spokes and
   invents starved_thermal violations that look like your edit caused them. So
   the fill happens against a staged project carrying tools/kicad_rules.json,
   and the tracked project file is never written.

2. A THROUGH VIA IS CHECKED AGAINST ALL FOUR LAYERS. Board3 routes ~1600 mm of
   signal through Inner1 and Inner2, so "the top and bottom are clear here" says
   nothing about whether a via can land there. Check the inner layers before
   placing one: a via at (37.100,93.918) shorted an Inner1 VBUS run during U6.

3. board.GetTracks() RE-WRAPS THE LIVE CONTAINER. Call it again after a Remove()
   and you get a SwigPyObject that will not iterate. Snapshot once, up front.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).parent
RULES = HERE / "kicad_rules.json"
NM = 1_000_000

try:
    import pcbnew
except ImportError:  # depends on which interpreter invoked us
    # Re-run under KiCad's own interpreter, exactly once, so this works from any
    # shell -- same contract as tools/kicad_measure.py.
    if os.environ.get("KICAD_HANDROUTE_REEXEC"):
        raise SystemExit("pcbnew is not importable even under KiCad's interpreter.")
    sys.path.insert(0, str(HERE))
    from kicad_env import KiCadNotFound, find_kicad_python

    try:
        _interpreter = find_kicad_python()
    except KiCadNotFound as exc:
        raise SystemExit(str(exc))
    raise SystemExit(subprocess.run(
        [str(_interpreter), *sys.argv],
        env=dict(os.environ, KICAD_HANDROUTE_REEXEC="1")).returncode)


def to_nm(value: float) -> int:
    return int(round(value * NM))


def layer_id(board, name: str) -> int:
    for lid in board.GetEnabledLayers().CuStack():
        if name in (board.GetLayerName(lid), pcbnew.LayerName(lid)):
            return lid
    raise SystemExit(f"unknown copper layer: {name!r}")


def apply_step(board, spec: dict, tracks: list, dry: bool):
    netmap = {net.GetNetname().lstrip("/"): net
              for net in board.GetNetsByNetcode().values()}
    removed = added = 0

    for rule in spec.get("delete", []):
        net = rule["net"].lstrip("/")
        bbox = rule.get("bbox")
        layers = rule.get("layers")
        for track in tracks:
            if track.GetNetname().lstrip("/") != net:
                continue
            is_via = track.Type() == pcbnew.PCB_VIA_T
            if is_via and not rule.get("vias", True):
                continue
            if layers and not is_via and board.GetLayerName(track.GetLayer()) not in layers:
                continue
            start, end = track.GetStart(), track.GetEnd()
            if bbox:
                x1, y1, x2, y2 = bbox

                def inside(point) -> bool:
                    return x1 <= point.x / NM <= x2 and y1 <= point.y / NM <= y2

                if not inside(start):
                    continue
                if not is_via and not inside(end):
                    continue
            kind = "via" if is_via else board.GetLayerName(track.GetLayer())
            print(f"  - {net:<13} {kind:<13} ({start.x/NM:8.3f},{start.y/NM:8.3f})->"
                  f"({end.x/NM:8.3f},{end.y/NM:8.3f})")
            if not dry:
                board.Remove(track)
            removed += 1

    for rule in spec.get("move_footprints", []):
        ref = rule["ref"]
        dx, dy = rule["by"]
        for footprint in board.GetFootprints():
            if footprint.GetReference() != ref:
                continue
            pos = footprint.GetPosition()
            moved = pcbnew.VECTOR2I(pos.x + to_nm(dx), pos.y + to_nm(dy))
            if not dry:
                footprint.SetPosition(moved)
            print(f"  ~ moved {ref} by ({dx},{dy}) -> "
                  f"({moved.x/NM:.3f},{moved.y/NM:.3f})")
            break
        else:
            raise SystemExit(f"no footprint {ref!r} on the board")

    for rule in spec.get("add", []):
        net = netmap[rule["net"].lstrip("/")]
        lid = layer_id(board, rule["layer"])
        width = to_nm(rule.get("width", 0.254))
        points = rule["points"]
        for (x1, y1), (x2, y2) in zip(points, points[1:]):
            segment = pcbnew.PCB_TRACK(board)
            segment.SetStart(pcbnew.VECTOR2I(to_nm(x1), to_nm(y1)))
            segment.SetEnd(pcbnew.VECTOR2I(to_nm(x2), to_nm(y2)))
            segment.SetWidth(width)
            segment.SetLayer(lid)
            segment.SetNet(net)
            board.Add(segment)
            print(f"  + {rule['net']:<13} {rule['layer']:<13} "
                  f"({x1:8.3f},{y1:8.3f})->({x2:8.3f},{y2:8.3f})")
            added += 1

    for rule in spec.get("add_vias", []):
        via = pcbnew.PCB_VIA(board)
        x, y = rule["at"]
        via.SetPosition(pcbnew.VECTOR2I(to_nm(x), to_nm(y)))
        via.SetWidth(to_nm(rule.get("diameter", 0.6)))
        via.SetDrill(to_nm(rule.get("drill", 0.3)))
        via.SetNet(netmap[rule["net"].lstrip("/")])
        via.SetLayerPair(layer_id(board, rule.get("top", "Top Layer")),
                         layer_id(board, rule.get("bottom", "Bottom Layer")))
        # A via sitting in a thermal pad must be tented, or reflow wicks solder
        # down the barrel and leaves a void under the pad.
        if rule.get("tented"):
            via.SetFrontTentingMode(pcbnew.TENTING_MODE_TENTED)
            via.SetBackTentingMode(pcbnew.TENTING_MODE_TENTED)
        board.Add(via)
        print(f"  + {rule['net']:<13} via           ({x:8.3f},{y:8.3f})"
              f"{'  tented' if rule.get('tented') else ''}")
        added += 1

    for rule in spec.get("add_footprints", []):
        footprint = pcbnew.FootprintLoad(rule["lib"], rule["name"])
        if footprint is None:
            raise SystemExit(f"footprint {rule['name']!r} not in {rule['lib']!r}")
        x, y = rule["at"]
        board.Add(footprint)
        footprint.SetPosition(pcbnew.VECTOR2I(to_nm(x), to_nm(y)))
        footprint.SetReference(rule["ref"])
        # board_only keeps "Update PCB from Schematic" from deleting a part that
        # has no symbol; the other two keep it out of the BOM and the CPL.
        footprint.SetAttributes(pcbnew.FP_SMD | pcbnew.FP_BOARD_ONLY
                                | pcbnew.FP_EXCLUDE_FROM_BOM
                                | pcbnew.FP_EXCLUDE_FROM_POS_FILES)
        print(f"  + {rule['ref']:<13} {rule['name']:<24} ({x:8.3f},{y:8.3f})")
        added += 1

    return removed, added


def refill_under_real_rules(board_path: Path) -> int:
    """Refill every zone with tools/kicad_rules.json in force. See trap 1."""
    rules = json.loads(RULES.read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        staged = work / board_path.name
        shutil.copy2(board_path, staged)

        sibling = board_path.with_suffix(".kicad_pro")
        project = json.loads(sibling.read_text(encoding="utf-8")) if sibling.is_file() else {}
        settings = project.setdefault("board", {}).setdefault("design_settings", {})
        settings.setdefault("rules", {}).update(rules["rules_mm"])
        classes = project.setdefault("net_settings", {}).setdefault("classes", [])
        if not any(c.get("name") == "Default" for c in classes):
            classes.append({"name": "Default"})
        for netclass in classes:
            if netclass.get("name") == "Default":
                netclass.update(rules["default_netclass_mm"])
        staged.with_suffix(".kicad_pro").write_text(json.dumps(project, indent=2),
                                                    encoding="utf-8")

        board = pcbnew.LoadBoard(str(staged))
        pcbnew.ZONE_FILLER(board).Fill(board.Zones())
        pcbnew.SaveBoard(str(board_path), board)
        connectivity = board.GetConnectivity()
        connectivity.RecalculateRatsnest()
        try:
            return connectivity.GetUnconnectedCount(True)
        except TypeError:
            return connectivity.GetUnconnectedCount()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("board")
    parser.add_argument("edit", help="JSON edit spec, or a {'steps': [...]} log")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print what would change without writing the board")
    args = parser.parse_args()

    board_path = Path(args.board)
    document = json.loads(Path(args.edit).read_text(encoding="utf-8"))
    steps = document.get("steps", [document])

    board = pcbnew.LoadBoard(str(board_path))
    # Snapshot before the first Remove(). See trap 3.
    tracks = list(board.Tracks())

    removed = added = 0
    for spec in steps:
        if len(steps) > 1:
            print(f"[{spec.get('step', '?')}]")
        step_removed, step_added = apply_step(board, spec, tracks, args.dry_run)
        removed += step_removed
        added += step_added

    print(f"removed {removed}, added {added}")
    if args.dry_run:
        print("dry run -- board not written")
        return 0

    pcbnew.SaveBoard(str(board_path), board)
    unconnected = refill_under_real_rules(board_path)
    print(f"saved {board_path.name}  (zones refilled under real rules, "
          f"airwires now {unconnected})")
    print("Now measure it: python tools/kicad_measure.py <board> --against <before>")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
