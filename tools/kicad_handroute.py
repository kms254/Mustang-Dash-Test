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
    "move_footprints": [{"ref": "X1", "by": [0.5, 0.0]},
                        {"ref": "R33", "to": [136.0, 101.25], "rot": 180}],
    "delete":   [{"net": "CAN1_H", "bbox": [x1,y1,x2,y2], "layers": ["Bottom Layer"],
                  "vias": true}],
    "renet":    [{"from": "SCLK_L_MCU", "to": "SCLK_L", "bbox": [x1,y1,x2,y2],
                  "layers": ["Inner2"], "vias": true}],
    "add":      [{"net": "CAN1_H", "layer": "Top Layer", "width": 0.254,
                  "points": [[x,y], [x,y], ...]}],
    "add_vias": [{"net": "USB_DM_CONN", "at": [x,y], "diameter": 0.6, "drill": 0.3}],
    "untent_vias": [{"at": [x,y], "net": "SCLK_C", "label": "TP1"}],
    "add_silk":  [{"text": "TT1", "at": [x,y], "height": 1.0, "thickness": 0.15}],
    "replace_footprints": [{"ref": "SW1", "fpid": "lib:FOOTPRINT",
                            "libpath": "kicad/board3/lib.pretty"}],
    "add_keepouts": [{"ref": "H2", "at": [x,y], "radius": 0.85,
                      "layers": ["F.Cu","In1.Cu","In2.Cu","B.Cu"]}]
  }

Deletion semantics: a track is removed only when BOTH endpoints are inside bbox,
a via when its centre is. A segment that merely passes through survives --
deleting those silently orphans copper outside the region you meant to touch.
Omit "bbox" to match a whole net, "layers" to match any layer. "renet" selects
by the same rule, and exists because moving a series element to the other end of
a run renames most of that run rather than re-laying it: re-adding copper rounds
every endpoint through millimetres, so geometry that was DRC-clean moves. Renet
keeps the integer nanometres and changes only the net.

"move_footprints" takes "by" (relative) or "to" (absolute), plus an optional
"rot" in degrees. Placing a two-pad part in series ON its own escape needs the
part aligned with the trace, so orientation is part of the placement, not a
separate GUI step.

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

4. A POUR FILLS TO min_clearance, BUT AN NPTH HOLE IS JUDGED BY min_hole_clearance.
   Board3 sets those to 0.1016 and 0.2, so every zone that reaches a mounting or
   alignment hole lands in the 0.084 mm gap between them and reports
   hole_clearance -- four layers x every hole, with nothing wrong upstream to fix.
   "add_keepouts" is the fix: a rule area over the hole that only bans the pour.
   It must NOT ban vias. The keepout belongs to the FOOTPRINT, so it travels with
   the part and the library copy stays the source of truth.
"""

from __future__ import annotations

import argparse
import json
import math
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


# Removed board items must outlive SaveBoard. Letting a removed SWIG proxy be
# garbage-collected frees the underlying C++ object and corrupts the session --
# unrelated proxies turn into raw SwigPyObjects. apply_step's locals die before
# main() saves, so this deliberately leaks at module scope for the process
# lifetime. The "memory leak ... no destructor" warnings are this working.
_KEEPALIVE: list = []


def any_layer_id(board, name: str) -> int:
    """Resolve ANY layer by name, not just copper.

    layer_id() walks CuStack(), which is right for tracks and vias and useless
    for silkscreen. Board3 renames its layers ("Top Silkscreen Layer", not
    "F.SilkS"), so both the board's own name and KiCad's canonical name are
    accepted -- and layers are always compared by ID afterwards, never by name.
    """
    for lid in board.GetEnabledLayers().Seq():
        if name in (board.GetLayerName(lid), pcbnew.LayerName(lid)):
            return lid
    raise SystemExit(f"unknown layer: {name!r}")


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

    for rule in spec.get("renet", []):
        # Reassign copper from one net to another WITHOUT re-laying it. Splitting a
        # series element off the far end of a run means most of that run simply
        # changes name; deleting and re-adding it would round every endpoint
        # through millimetres and move geometry that is already DRC-clean.
        # Selection semantics are the same as "delete": both endpoints inside bbox.
        src = rule["from"].lstrip("/")
        dst = netmap[rule["to"].lstrip("/")]
        bbox = rule.get("bbox")
        layers = rule.get("layers")
        for track in tracks:
            if track.GetNetname().lstrip("/") != src:
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
            print(f"  > {src} -> {rule['to'].lstrip('/'):<13} {kind:<13} "
                  f"({start.x/NM:8.3f},{start.y/NM:8.3f})->"
                  f"({end.x/NM:8.3f},{end.y/NM:8.3f})")
            if not dry:
                track.SetNet(dst)

    for rule in spec.get("move_silk", []):
        # Move a BOARD-level silk text (not a footprint field). Matched on its
        # exact string, with an optional "at" to disambiguate when the same
        # legend appears twice -- TERM1/TERM2 are distinct, but CAN1/CAN2 style
        # pairs are one edit away from being ambiguous, so the guard is cheap.
        want = rule["text"]
        near = rule.get("at")
        dx, dy = rule["by"]
        hits = []
        for d in board.GetDrawings():
            if not hasattr(d, "GetText") or d.GetText() != want:
                continue
            if near is not None:
                p = d.GetPosition()
                if abs(p.x - to_nm(near[0])) > to_nm(0.5) or \
                   abs(p.y - to_nm(near[1])) > to_nm(0.5):
                    continue
            hits.append(d)
        if len(hits) != 1:
            raise SystemExit(
                f"move_silk {want!r}: matched {len(hits)} items, expected exactly 1 "
                f"-- add or correct 'at' to disambiguate")
        pos = hits[0].GetPosition()
        moved = pcbnew.VECTOR2I(pos.x + to_nm(dx), pos.y + to_nm(dy))
        if not dry:
            hits[0].SetPosition(moved)
        print(f"  ~ silk {want!r} by ({dx},{dy}) -> "
              f"({moved.x/NM:.3f},{moved.y/NM:.3f})")

    for rule in spec.get("add_silk", []):
        # Board-level silkscreen text. Defaults match the 16 labels U45 placed
        # (1.0 mm high, 0.15 mm stroke) so the board reads as one hand -- and
        # 0.15 mm is also the process minimum, so do not go below it.
        text = pcbnew.PCB_TEXT(board)
        x, y = rule["at"]
        text.SetText(rule["text"])
        text.SetPosition(pcbnew.VECTOR2I(to_nm(x), to_nm(y)))
        text.SetLayer(any_layer_id(board, rule.get("layer", "Top Silkscreen Layer")))
        text.SetTextHeight(to_nm(rule.get("height", 1.0)))
        text.SetTextWidth(to_nm(rule.get("width", 1.0)))
        text.SetTextThickness(to_nm(rule.get("thickness", 0.15)))
        text.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_CENTER)
        text.SetVertJustify(pcbnew.GR_TEXT_V_ALIGN_CENTER)
        if rule.get("angle"):
            text.SetTextAngleDegrees(rule["angle"])
        if rule.get("mirror"):
            text.SetMirrored(True)
        board.Add(text)
        print(f"  T {rule['text']:<10} {rule.get('layer','Top Silkscreen Layer'):<21} "
              f"({x:8.3f},{y:8.3f})")
        added += 1

    for rule in spec.get("replace_footprints", []):
        # Swap a footprint for a different land pattern, keeping everything that
        # is NOT the land pattern: position, rotation, side, reference, value,
        # attributes, and the net on every pad matched BY PAD NUMBER (KTD27 --
        # never by order). pcbnew.FootprintLoad returns a bare FPID with no
        # library nickname, which reads as a lib_footprint_mismatch and stops
        # KiCad resolving the library at all, so the full FPID is set explicitly.
        ref = rule["ref"]
        lib, name = rule["fpid"].split(":", 1)
        old = next((f for f in board.GetFootprints() if f.GetReference() == ref), None)
        if old is None:
            raise SystemExit(f"no footprint {ref!r} on the board")
        nets = {p.GetNumber(): p.GetNet() for p in old.Pads()}
        new = pcbnew.FootprintLoad(rule["libpath"], name)
        if new is None:
            raise SystemExit(f"could not load {name!r} from {rule['libpath']!r}")
        new.SetReference(old.GetReference())
        new.SetValue(old.GetValue())
        new.SetPosition(old.GetPosition())
        new.SetOrientation(old.GetOrientation())
        new.SetLayer(old.GetLayer())
        new.SetAttributes(old.GetAttributes())
        new.SetFPID(pcbnew.LIB_ID(lib, name))
        missing = [n for n in nets if n not in {p.GetNumber() for p in new.Pads()}]
        if missing:
            raise SystemExit(f"{ref}: new footprint has no pad(s) {missing} -- refusing")
        for pad in new.Pads():
            if pad.GetNumber() in nets:
                pad.SetNet(nets[pad.GetNumber()])
        if not dry:
            board.Remove(old)
            board.Add(new)
            _KEEPALIVE.append(old)
        print(f"  * {ref}: {old.GetFPIDAsString()} -> {rule['fpid']}")
        for pad in sorted(new.Pads(), key=lambda p: p.GetNumber()):
            o = next((p for p in old.Pads() if p.GetNumber() == pad.GetNumber()), None)
            if o is None:
                continue
            print(f"      pad{pad.GetNumber()} {o.GetPosition().x/NM:8.3f} -> "
                  f"{pad.GetPosition().x/NM:8.3f} mm   net {pad.GetNetname()}")

    for rule in spec.get("untent_vias", []):
        # Open the solder mask over a via so it becomes a probe point. The board
        # default tents every via front and back, which is right for 229 of them
        # and wrong for the handful you need to put a scope on. Selection is by
        # exact position because a via has no name to address it by.
        x, y = rule["at"]
        want = (to_nm(x), to_nm(y))
        tol = to_nm(rule.get("tolerance", 0.01))
        for track in tracks:
            if track.Type() != pcbnew.PCB_VIA_T:
                continue
            pos = track.GetPosition()
            if abs(pos.x - want[0]) > tol or abs(pos.y - want[1]) > tol:
                continue
            if rule.get("net") and track.GetNetname().lstrip("/") != rule["net"].lstrip("/"):
                raise SystemExit(
                    f"via at ({x},{y}) is on {track.GetNetname()!r}, "
                    f"not {rule['net']!r} -- refusing to untent the wrong one")
            if not dry:
                track.SetFrontTentingMode(pcbnew.TENTING_MODE_NOT_TENTED)
                track.SetBackTentingMode(pcbnew.TENTING_MODE_NOT_TENTED)
            print(f"  o untented {track.GetNetname():<13} via at ({x},{y})"
                  f"{'  ' + rule['label'] if rule.get('label') else ''}")
            break
        else:
            raise SystemExit(f"no via within {rule.get('tolerance', 0.01)} mm of ({x},{y})")

    for rule in spec.get("move_footprints", []):
        ref = rule["ref"]
        for footprint in board.GetFootprints():
            if footprint.GetReference() != ref:
                continue
            pos = footprint.GetPosition()
            if "to" in rule:
                x, y = rule["to"]
                moved = pcbnew.VECTOR2I(to_nm(x), to_nm(y))
                how = f"to ({x},{y})"
            else:
                dx, dy = rule["by"]
                moved = pcbnew.VECTOR2I(pos.x + to_nm(dx), pos.y + to_nm(dy))
                how = f"by ({dx},{dy})"
            if not dry:
                footprint.SetPosition(moved)
                if "rot" in rule:
                    footprint.SetOrientationDegrees(rule["rot"])
            if "rot" in rule:
                how += f" rot={rule['rot']}"
            print(f"  ~ moved {ref} {how} -> ({moved.x/NM:.3f},{moved.y/NM:.3f})")
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

    for rule in spec.get("add_keepouts", []):
        # A rule area parented to the footprint, so it moves with the part and a
        # future placement of the same library footprint inherits it. Geometry is
        # ABSOLUTE -- KiCad writes footprint zone outlines in board coordinates,
        # which is why these read the same as the pads beside them.
        ref = rule["ref"]
        for footprint in board.GetFootprints():
            if footprint.GetReference() == ref:
                break
        else:
            raise SystemExit(f"no footprint {ref!r} on the board")

        if "points" in rule:
            points = rule["points"]
        else:
            # Regular octagon: inradius = radius * cos(22.5 deg) = 0.9239 * radius
            # is what actually holds the pour back, so size against THAT, not the
            # circumradius the number looks like.
            cx, cy = rule["at"]
            r = rule["radius"]
            points = [(cx + r * math.cos(math.radians(22.5 + 45 * k)),
                       cy + r * math.sin(math.radians(22.5 + 45 * k)))
                      for k in range(8)]

        zone = pcbnew.ZONE(footprint)
        zone.SetIsRuleArea(True)
        # The file format calls this "copperpour"; the API calls it ZoneFills.
        zone.SetDoNotAllowZoneFills(True)
        zone.SetDoNotAllowVias(not rule.get("allow_vias", True))
        zone.SetDoNotAllowTracks(not rule.get("allow_tracks", True))
        zone.SetDoNotAllowPads(not rule.get("allow_pads", True))
        zone.SetDoNotAllowFootprints(rule.get("ban_footprints", True))
        layers = pcbnew.LSET()
        for name in rule.get("layers", ["F.Cu", "In1.Cu", "In2.Cu", "B.Cu"]):
            layers.AddLayer(layer_id(board, name))
        zone.SetLayerSet(layers)
        outline = zone.Outline()
        outline.NewOutline()
        for x, y in points:
            outline.Append(to_nm(x), to_nm(y))
        if not dry:
            footprint.Add(zone)
        span = ",".join(rule.get("layers", ["F.Cu", "In1.Cu", "In2.Cu", "B.Cu"]))
        print(f"  + {ref:<13} keepout       ({points[0][0]:8.3f},{points[0][1]:8.3f})"
              f" +{len(points)-1} pts  [{span}]  pour banned, vias "
              f"{'banned' if zone.GetDoNotAllowVias() else 'legal'}")
        added += 1

    for rule in spec.get("move_zones", []):
        dx, dy = rule["by"]
        hits = 0
        for zone in board.Zones():
            if zone.GetIsRuleArea() != rule.get("rule_area", False):
                continue
            if rule.get("net") and zone.GetNetname().lstrip("/") != rule["net"].lstrip("/"):
                continue
            layers = [board.GetLayerName(l) for l in zone.GetLayerSet().CuStack()]
            if rule.get("layer") and rule["layer"] not in layers:
                continue
            before = zone.GetBoundingBox()
            if not dry:
                zone.Move(pcbnew.VECTOR2I(to_nm(dx), to_nm(dy)))
            print(f"  ~ zone {zone.GetNetname()} on {','.join(layers)} moved by "
                  f"({dx},{dy}) from ({before.GetLeft()/NM:.2f},{before.GetTop()/NM:.2f})")
            hits += 1
        if not hits:
            raise SystemExit(f"move_zones matched nothing: {rule}")
        added += hits

    for rule in spec.get("set_pad_nets", []):
        # Assigning a net to a board-only pad. Board3's mounting holes are
        # footprints with no schematic symbol, so "Update PCB from Schematic"
        # leaves them alone -- verified, it did -- but that also means nothing
        # upstream will ever assert this net for them. It lives here or nowhere.
        target_net = netmap[rule["net"].lstrip("/")]
        hits = 0
        for footprint in board.GetFootprints():
            if rule.get("fpid") and rule["fpid"] not in footprint.GetFPID().GetUniStringLibId():
                continue
            if rule.get("ref") and footprint.GetReference() != rule["ref"]:
                continue
            for pad in footprint.Pads():
                if rule.get("pad") and pad.GetNumber() != rule["pad"]:
                    continue
                pos = pad.GetPosition()
                print(f"  ~ {footprint.GetFPID().GetUniStringLibId()} pad "
                      f"{pad.GetNumber()} at ({pos.x/NM:.3f},{pos.y/NM:.3f}): "
                      f"{pad.GetNetname()!r} -> {rule['net']}")
                if not dry:
                    pad.SetNet(target_net)
                hits += 1
        if not hits:
            raise SystemExit(f"set_pad_nets matched nothing: {rule}")
        added += hits

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
