#!/usr/bin/env python3
"""Render the board to PNGs that are committed alongside the design.

The renders exist so a reviewer can see what changed without installing KiCad,
and so the README can show the board rather than describe it. They are build
output, not source -- `.kicad_pcb` is the truth and these are regenerated from
it, which is why the pre-commit hook refreshes them whenever the board changes.

Deliberately small (1200x750). These are binaries regenerated on every
PCB-touching commit, so every pixel is paid for permanently in history. 1200px
is enough to see placement and routing at a glance; anyone needing detail should
open the board.

    python tools/kicad_render.py            # refresh all three views
    python tools/kicad_render.py --check    # exit 2 if stale, write nothing
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from kicad_env import run_kicad_cli  # noqa: E402

REPO = Path(__file__).parent.parent
PROJECT = REPO / "kicad" / "board3"
OUT = PROJECT / "renders"

WIDTH, HEIGHT = "1200", "750"

# (filename, extra kicad-cli args). The angled view is the one worth looking at;
# top and bottom are the ones worth diffing.
VIEWS = (
    ("top.png", ["--side", "top"]),
    ("bottom.png", ["--side", "bottom"]),
    ("angled.png", ["--side", "top", "--rotate", "-25,0,20"]),
)

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_STALE = 2


def board() -> Path:
    boards = sorted(PROJECT.glob("*.kicad_pcb"))
    if not boards:
        raise SystemExit(f"no .kicad_pcb in {PROJECT}")
    return boards[0]


def render(pcb: Path, name: str, extra: list[str]) -> Path:
    target = OUT / name
    run_kicad_cli(
        "pcb", "render",
        "--quality", "high", "--perspective", "--background", "opaque",
        "-w", WIDTH, "-h", HEIGHT,
        *extra,
        "-o", str(target), str(pcb),
    )
    return target


def main() -> int:
    ap = argparse.ArgumentParser(description="Render Board3 to committed PNGs.")
    ap.add_argument("--check", action="store_true",
                    help="report whether renders are older than the board; write nothing")
    args = ap.parse_args()

    pcb = board()

    if args.check:
        stale = [n for n, _ in VIEWS
                 if not (OUT / n).exists() or (OUT / n).stat().st_mtime < pcb.stat().st_mtime]
        if stale:
            print(f"renders stale or missing: {', '.join(stale)}")
            print("  run: python tools/kicad_render.py")
            return EXIT_STALE
        print(f"renders current ({len(VIEWS)} views)")
        return EXIT_OK

    OUT.mkdir(parents=True, exist_ok=True)
    for name, extra in VIEWS:
        target = render(pcb, name, extra)
        print(f"  {target.relative_to(REPO)}  ({target.stat().st_size // 1024} KB)")
    print(f"rendered {len(VIEWS)} views from {pcb.name}")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
