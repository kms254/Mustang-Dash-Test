"""Deterministic placement lint for Board3.

Three checks, each born from a confirmed defect (or defect class) of the first
JLCPCB order (2026-08) and calibrated against it per
docs/solutions/design-patterns/calibrate-an-automated-reviewer-on-a-confirmed-defect.md:

1. edgecuts-in-footprint -- a footprint carrying Edge.Cuts graphics is drawing
   holes (or cuts) in a language no drill file speaks. USBC1's locating pegs
   were filled Edge.Cuts polygons for the board's whole life: green in DRC,
   parity, and every sourcing audit, absent from both Excellon files, and only
   drilled because JLC's CAM engineer noticed (found via the production-file
   diff, fixed in PR #41). Holes belong in pads; board cuts belong at board
   level.

2. connector-mouth -- a wire-entry connector (screw terminal, barrel jack)
   placed against a board edge must have its entry face toward that edge. The
   entry face is the footprint body's DEEP side: the courtyard extends further
   from the pin row on the mouth side (the clamp cavity needs the depth --
   measured on the WJ500V: 5.8 vs 4.8 mm). P1/P2 shipped to the order flow
   180 degrees off, mouths at the LQFP; caught by eyes in JLC's 3D viewer,
   fixed in PR #36. The pads are electrically symmetric, so no electrical
   check can ever see this class.

3. drill-census -- exports the drill files fresh (kicad-cli, temp dir) and
   requires the hole census to match the board: every np_thru_hole pad, every
   thru_hole pad, and every via must land in an Excellon file, round drills as
   hits and oval drills as G85 slots. Catches the general class behind check 1:
   copper/pad intent that silently fails to reach the drill programs.

--self-test extracts the two historical defective board states from git
(pre-PR#36 at 1bfeef7 for the connector check, pre-PR#41 at 6a7e8ed for the
Edge.Cuts check) and asserts each check FIRES on them, then asserts the census
comparator detects an injected delta. A check that has never been seen to fire
is indistinguishable from a malformed one (the .kicad_dru lesson). If the blobs
are unreachable (shallow clone), the self-test SKIPS them visibly rather than
passing vacuously.

Run under an interpreter with pcbnew (KiCad's python on Windows; system python3
with the kicad package on CI). Exit 0 clean, 1 on findings or self-test failure.
"""

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import pcbnew

BOARD_REL = "kicad/board3/ProPrj_New Project_2026-07-15_23-14-34_2026-07-27.kicad_pcb"
PRE_PR36 = "1bfeef7"  # P1/P2 mouths still face the LQFP
PRE_PR41 = "6a7e8ed"  # USBC1 pegs still Edge.Cuts polygons

# Wire-entry connector classes: footprint-name substring -> human name.
# Only parts whose courtyard sits within EDGE_NEAR_MM of a board edge are
# judged; a mid-board connector has no mouth constraint.
WIRE_ENTRY_CLASSES = ("WJ500V", "DC-005")
EDGE_NEAR_MM = 6.0
NM = 1e6


def _findings(title, items):
    if items:
        print(f"FAIL {title}")
        for it in items:
            print(f"     {it}")
    else:
        print(f"ok   {title}")
    return list(items)


def check_edgecuts_in_footprints(board):
    bad = []
    for fp in board.GetFootprints():
        for item in fp.GraphicalItems():
            if isinstance(item, pcbnew.PCB_SHAPE) and item.GetLayer() == pcbnew.Edge_Cuts:
                bb = item.GetBoundingBox()
                bad.append(
                    f"{fp.GetReference()}: {item.ShowShape()} on Edge.Cuts at "
                    f"({bb.GetCenter().x / NM:.3f}, {bb.GetCenter().y / NM:.3f}) -- "
                    f"holes belong in pads, cuts at board level"
                )
    return _findings("edgecuts-in-footprint: no footprint draws on Edge.Cuts", bad)


def check_connector_mouths(board):
    edges = board.GetBoardEdgesBoundingBox()  # coarse direction only; the
    # centerline-vs-bbox distinction (see clip-test doc) is 0.127 mm here and
    # irrelevant at the millimetre scale this check operates on.
    ex1, ey1 = edges.GetLeft(), edges.GetTop()
    ex2, ey2 = edges.GetRight(), edges.GetBottom()
    bad = []
    for fp in board.GetFootprints():
        fpid = fp.GetFPID().GetLibItemName().wx_str()
        if not any(cls in fpid for cls in WIRE_ENTRY_CLASSES):
            continue
        cy = fp.GetCourtyard(pcbnew.F_CrtYd).BBox()
        if cy.GetWidth() == 0:
            cy = fp.GetBoundingBox()
        pads = list(fp.Pads())
        cx = sum(p.GetPosition().x for p in pads) / len(pads)
        cyy = sum(p.GetPosition().y for p in pads) / len(pads)
        # deep-side vector: pad centroid -> courtyard center
        dvx = cy.GetCenter().x - cx
        dvy = cy.GetCenter().y - cyy
        # nearest edge and its outward direction
        dists = {
            "west": (cy.GetLeft() - ex1, (-1, 0)),
            "east": (ex2 - cy.GetRight(), (1, 0)),
            "north": (cy.GetTop() - ey1, (0, -1)),
            "south": (ey2 - cy.GetBottom(), (0, 1)),
        }
        name, (dist, (ox, oy)) = min(dists.items(), key=lambda kv: kv[1][0])
        if dist / NM > EDGE_NEAR_MM:
            continue  # not edge-adjacent; no constraint
        dot = dvx * ox + dvy * oy
        if dot <= 0:
            bad.append(
                f"{fp.GetReference()} ({fpid}): wire-entry face points AWAY from the "
                f"{name} edge {dist / NM:.2f} mm away -- deep-side vector "
                f"({dvx / NM:+.2f}, {dvy / NM:+.2f}) mm"
            )
    return _findings("connector-mouth: wire-entry faces the nearest edge", bad)


def _board_census(board):
    census = {"npth_hits": 0, "npth_slots": 0, "pth_hits": 0, "pth_slots": 0}
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            attr = pad.GetAttribute()
            if attr not in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH):
                continue
            d = pad.GetDrillSize()
            if d.x == 0:
                continue
            kind = "npth" if attr == pcbnew.PAD_ATTRIB_NPTH else "pth"
            census[f"{kind}_hits" if d.x == d.y else f"{kind}_slots"] += 1
    for t in board.GetTracks():
        if t.Type() == pcbnew.PCB_VIA_T:
            census["pth_hits"] += 1
    return census


def _excellon_census(drl_path):
    hits = slots = 0
    if drl_path.exists():
        for line in drl_path.read_text().splitlines():
            s = line.strip()
            if re.match(r"^X-?[\d.]+Y-?[\d.]+$", s):
                hits += 1
            elif s.startswith("X") and "G85" in s:
                slots += 1
    return hits, slots


def _kicad_cli():
    cli = shutil.which("kicad-cli") or shutil.which("kicad-cli.exe")
    if cli:
        return cli
    win = Path("C:/Program Files/KiCad/10.0/bin/kicad-cli.exe")
    return str(win) if win.exists() else None


def check_drill_census(board, board_path, inject_delta=0):
    cli = _kicad_cli()
    if cli is None:
        print("skip drill-census: kicad-cli not found")
        return []
    with tempfile.TemporaryDirectory() as td:
        subprocess.run(
            [cli, "pcb", "export", "drill", "--format", "excellon",
             "--drill-origin", "absolute", "--excellon-units", "mm",
             "--excellon-separate-th", "-o", td + "/", str(board_path)],
            check=True, capture_output=True)
        stem = Path(board_path).stem
        npth = _excellon_census(Path(td) / f"{stem}-NPTH.drl")
        pth = _excellon_census(Path(td) / f"{stem}-PTH.drl")
    want = _board_census(board)
    want["npth_hits"] += inject_delta
    got = {"npth_hits": npth[0], "npth_slots": npth[1],
           "pth_hits": pth[0], "pth_slots": pth[1]}
    bad = []
    for k in want:
        if want[k] != got[k]:
            bad.append(f"{k}: board expects {want[k]}, Excellon carries {got[k]}")
    return _findings(
        f"drill-census: board {want} == exported {got}"
        if not bad else "drill-census", bad)


def _git_blob(repo, rev, rel, dest):
    r = subprocess.run(["git", "-C", str(repo), "cat-file", "-e", f"{rev}:{rel}"],
                       capture_output=True)
    if r.returncode != 0:
        return False
    data = subprocess.run(["git", "-C", str(repo), "show", f"{rev}:{rel}"],
                          check=True, capture_output=True).stdout
    dest.write_bytes(data)
    return True


def self_test(repo):
    failures = 0
    with tempfile.TemporaryDirectory() as td:
        pre36 = Path(td) / "pre36.kicad_pcb"
        if _git_blob(repo, PRE_PR36, BOARD_REL, pre36):
            found = check_connector_mouths(pcbnew.LoadBoard(str(pre36)))
            refs = {f.split()[0] for f in found}
            if {"P1", "P2"} <= refs:
                print(f"self-test: connector-mouth FIRES on {PRE_PR36} (P1, P2) -- calibrated")
            else:
                print(f"SELF-TEST FAIL: connector-mouth did not fire on {PRE_PR36} (got {refs})")
                failures += 1
        else:
            print(f"self-test SKIP: {PRE_PR36} unreachable (shallow clone?) -- connector-mouth uncalibrated this run")

        pre41 = Path(td) / "pre41.kicad_pcb"
        if _git_blob(repo, PRE_PR41, BOARD_REL, pre41):
            found = check_edgecuts_in_footprints(pcbnew.LoadBoard(str(pre41)))
            if len(found) == 2 and all(f.startswith("USBC1") for f in found):
                print(f"self-test: edgecuts-in-footprint FIRES on {PRE_PR41} (USBC1 x2) -- calibrated")
            else:
                print(f"SELF-TEST FAIL: edgecuts check on {PRE_PR41} returned {len(found)} findings")
                failures += 1
        else:
            print(f"self-test SKIP: {PRE_PR41} unreachable (shallow clone?) -- edgecuts uncalibrated this run")

        live = repo / BOARD_REL
        board = pcbnew.LoadBoard(str(live))
        if not check_drill_census(board, live, inject_delta=1):
            print("SELF-TEST FAIL: census comparator ignored an injected +1 delta")
            failures += 1
        else:
            print("self-test: drill-census comparator detects an injected delta -- calibrated")
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board", nargs="?", default=None)
    ap.add_argument("--self-test", action="store_true",
                    help="prove each check fires on its historical confirmed defect")
    args = ap.parse_args()

    repo = Path(__file__).resolve().parent.parent
    board_path = Path(args.board) if args.board else repo / BOARD_REL

    failures = 0
    if args.self_test:
        failures += self_test(repo)
        print()

    board = pcbnew.LoadBoard(str(board_path))
    findings = []
    findings += check_edgecuts_in_footprints(board)
    findings += check_connector_mouths(board)
    findings += check_drill_census(board, board_path)

    print()
    print(f"placement lint: {len(findings)} finding(s), self-test failures: {failures}")
    sys.exit(1 if (findings or failures) else 0)


if __name__ == "__main__":
    main()
