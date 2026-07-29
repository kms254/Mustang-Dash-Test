#!/usr/bin/env python3
"""Route a stripped Board3 headlessly, then refill zones -- the two are inseparable.

Wraps KiCadRoutingTools (cloned beside this repo; KICAD_ROUTING_TOOLS overrides).
The wrapper exists because four things must be true for the result to mean
anything, and the router does none of them on its own.

1. The board must carry REAL design rules before routing. KiCad's EasyEDA Pro
   importer writes stock defaults, and the router reads them from the sibling
   .kicad_pro. Left alone it routes to constraints Board3 never had.

2. The router must be told the geometry explicitly. Given no sibling project it
   falls back to its own defaults -- a 0.3 mm track width and a 0.1 mm clearance
   floor -- and will happily neck down to 0.0889 mm at fine-pitch pads, which is
   JLC ADVANCED process, not standard.

3. It rewrites the project's DRC settings to match whatever it produced, so DRC
   afterwards grades its own homework. --no-fix-drc-settings is always passed.

4. Zones MUST be refilled afterward. This is the big one: routing across a
   preserved copper pour leaves the pour stale, and KiCad then reports every
   crossing as a zero-clearance violation. Skipping the refill turned a real
   74-violation result into an apparent 974, of which 488 were phantom
   track-over-pour and via-over-pour overlaps. The refill is not a tidy-up step;
   without it the measurement is meaningless.

The net set comes from tools/kicad_netclass.json so the router and the strip
cannot disagree about what is held out.

  python tools/kicad_route.py kicad/routing/board3-stripped.kicad_pcb \\
      --out kicad/routing/board3-routed.kicad_pcb
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).parent
RULES = HERE / "kicad_rules.json"
NETCLASS = HERE / "kicad_netclass.json"
DEFAULT_TOOLS = Path.home() / "Code" / "KiCadRoutingTools"

EXIT_OK = 0
EXIT_ERROR = 1


def find_router() -> Path:
    root = Path(os.environ.get("KICAD_ROUTING_TOOLS", DEFAULT_TOOLS))
    route = root / "route.py"
    if not route.is_file():
        raise RuntimeError(
            f"KiCadRoutingTools not found at {root}. Clone "
            "https://github.com/drandyhaas/KiCadRoutingTools and run "
            "`python build_router.py`, or set KICAD_ROUTING_TOOLS."
        )
    return route


def apply_rules(board: Path, template_pro: Path) -> Path:
    """Write a sibling .kicad_pro carrying Board3's real rules, not the importer's."""
    spec = json.loads(RULES.read_text(encoding="utf-8"))
    pro = json.loads(template_pro.read_text(encoding="utf-8"))

    rules = pro.setdefault("board", {}).setdefault("design_settings", {}).setdefault("rules", {})
    rules.update(spec["rules_mm"])
    for cls in pro.setdefault("net_settings", {}).get("classes", []):
        if cls.get("name") == "Default":
            cls.update(spec["default_netclass_mm"])

    out = board.with_suffix(".kicad_pro")
    out.write_text(json.dumps(pro, indent=2), encoding="utf-8")
    return out


def excluded_net_args() -> list[str]:
    """Router --nets patterns: everything, minus every held-out net.

    Each name is emitted in BOTH the bare and the hierarchy-prefixed form.
    kicad_netclass.json records nets as EasyEDA named them (USB_DP), but a KiCad
    "Update PCB from Schematic" rewrites every board net to the schematic's
    hierarchical form (/USB_DP). Matching is literal, so a bare-only pattern
    silently stops excluding anything the moment that sync runs -- and the
    failure is invisible: the router reports success while having ripped up and
    re-solved the audited USB pair, the crystal loop and the QSPI bus, which are
    held out precisely because an autorouter degrades them. Emitting both forms
    keeps the exclusion correct on either side of that rename; an unmatched
    pattern is harmless.
    """
    spec = json.loads(NETCLASS.read_text(encoding="utf-8"))
    nets = sorted({n for e in spec["exclusions"] for n in e["nets"]})
    patterns = {f"!{n}" for n in nets} | {f"!/{n.lstrip('/')}" for n in nets}
    return ["*"] + sorted(patterns)


def airwires(board: Path) -> int:
    import pcbnew

    b = pcbnew.LoadBoard(str(board))
    conn = b.GetConnectivity()
    conn.RecalculateRatsnest()
    try:
        return conn.GetUnconnectedCount(True)
    except TypeError:
        return conn.GetUnconnectedCount()


def refill_zones(board: Path) -> int:
    """Refill every zone in place. See point 4 in the module docstring."""
    import pcbnew

    b = pcbnew.LoadBoard(str(board))
    filler = pcbnew.ZONE_FILLER(b)
    filler.Fill(b.Zones())
    pcbnew.SaveBoard(str(board), b)
    return len(b.Zones())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board", help="Stripped .kicad_pcb to route")
    ap.add_argument("--out", required=True, help="Where to write the routed board")
    ap.add_argument("--rules-from", help="Project file to use as the rules template "
                                        "(defaults to the board's own sibling)")
    args = ap.parse_args()

    src = Path(args.board)
    out = Path(args.out)
    if not src.is_file():
        print(f"no such board: {src}", file=sys.stderr)
        return EXIT_ERROR

    try:
        route_py = find_router()
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return EXIT_ERROR

    template = Path(args.rules_from) if args.rules_from else src.with_suffix(".kicad_pro")
    if not template.is_file():
        print(f"no rules template at {template} -- pass --rules-from", file=sys.stderr)
        return EXIT_ERROR

    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, out)
    pro = apply_rules(out, template)
    spec = json.loads(RULES.read_text(encoding="utf-8"))["default_netclass_mm"]
    print(f"rules applied  : {pro.name} (clearance {spec['clearance']}, "
          f"track {spec['track_width']}, via {spec['via_diameter']}/{spec['via_drill']})",
          flush=True)

    cmd = [
        sys.executable, str(route_py), str(out),
        "--nets", *excluded_net_args(),
        "--track-width", str(spec["track_width"]),
        "--via-size", str(spec["via_diameter"]),
        "--via-drill", str(spec["via_drill"]),
        "--no-fix-drc-settings",   # never let the router grade its own homework
        "--overwrite",
    ]
    # `out` already exists -- we copied the input there -- so its presence proves
    # nothing. Check the return code, and then check the board actually changed.
    # An earlier version tested only is_file() and reported success on a run that
    # had aborted for missing scipy, leaving the input copy untouched.
    before = airwires(out)
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        detail = (proc.stdout + proc.stderr).strip()
        print(f"router failed ({proc.returncode}):\n{detail[-800:]}", file=sys.stderr)
        return EXIT_ERROR

    after = airwires(out)
    if after >= before:
        print(f"router exited 0 but closed nothing ({before} -> {after} airwires); "
              "treating as failure", file=sys.stderr)
        return EXIT_ERROR
    print(f"routed         : {out.name}  ({before} -> {after} airwires)", flush=True)

    n = refill_zones(out)
    print(f"zones refilled : {n}", flush=True)
    print("\nRun tools/kicad_verify.py on the result -- routing is not verified "
          "by the router's own exit code.", flush=True)
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
