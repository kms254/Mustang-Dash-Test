#!/usr/bin/env python3
"""Check JLCPCB stock and lifecycle for every LCSC part on the board.

Deliberately NOT a pull-request gate. Stock is a property of the world, not of
the change under review, so failing someone's PR because a capacitor went out of
stock overnight punishes the wrong person at the wrong moment. This runs on a
schedule: the useful signal is "a part you depend on became unavailable", and
the useful time to learn it is before you order, not during review.

Self-contained by necessity. `kicad_lcsc.py` reaches JLC through the locally
installed KiCad plugin, which does not exist on a CI runner, so the one API call
this needs is reimplemented here against the same endpoint.

    python tools/kicad_stock.py                  # report every part
    python tools/kicad_stock.py --min-stock 500  # stricter floor
    python tools/kicad_stock.py --quiet          # only problems

Exit 2 if any part is unavailable, delisted, or could not be queried.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from kicad_lcsc import find_schematic, read_parts  # noqa: E402

REPO = Path(__file__).parent.parent
DEFAULT_PROJECT = REPO / "kicad" / "board3"

SEARCH_API = "https://jlcpcb.com/api/overseas-pcb-order/v1/shoppingCart/smtGood/selectSmtComponentList"
HEADERS = {
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
    "Accept": "application/json",
    "Content-Type": "application/json",
    "Origin": "https://jlcpcb.com",
    "Referer": "https://jlcpcb.com/parts",
}

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_PROBLEM = 2


def lookup(code: str, timeout: int = 20) -> dict | None:
    """Return the JLC record whose componentCode matches *code* exactly.

    The endpoint is a keyword search, so it will happily return near misses.
    Matching componentCode exactly is what makes the answer trustworthy -- a
    fuzzy hit reporting healthy stock for a different part is worse than no
    answer at all.
    """
    body = json.dumps({"keyword": code, "currentPage": 1, "pageSize": 25}).encode()
    req = urllib.request.Request(SEARCH_API, data=body, headers=HEADERS)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = json.loads(resp.read().decode("utf-8"))

    page = (raw.get("data") or {}).get("componentPageInfo") or {}
    for item in page.get("list") or []:
        if (item.get("componentCode") or "").upper() == code.upper():
            prices = item.get("componentPrices") or []
            return {
                "lcsc": item.get("componentCode", ""),
                "model": item.get("componentModelEn", ""),
                "stock": item.get("stockCount", 0) or 0,
                "type": "Basic" if item.get("componentLibraryType") == "base" else "Extended",
                "price": prices[0].get("productPrice") if prices else None,
            }
    return None


def annotate(level: str, msg: str) -> None:
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print(f"::{level}::{msg}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Check JLCPCB stock for every LCSC part on the board.")
    ap.add_argument("-p", "--project", default=str(DEFAULT_PROJECT))
    ap.add_argument("--min-stock", type=int, default=200,
                    help="floor below which a part is called low (default: 200)")
    ap.add_argument("--quiet", action="store_true", help="print only parts with a problem")
    ap.add_argument("--delay", type=float, default=0.4,
                    help="seconds between requests, to stay polite (default: 0.4)")
    args = ap.parse_args()

    parts = read_parts(find_schematic(Path(args.project).resolve()))

    # Quantity matters: 24 of a part with 100 in stock is a different problem
    # from 1 of a part with 100 in stock.
    need: dict[str, list[str]] = {}
    values: dict[str, str] = {}
    for p in parts:
        code = (p.get("Supplier Part") or "").strip()
        if code:
            need.setdefault(code, []).append(p.get("Reference", "?"))
            values.setdefault(code, (p.get("Value") or "").strip())

    print(f"checking {len(need)} distinct LCSC parts (floor {args.min_stock})\n")

    unavailable: list[str] = []
    low: list[str] = []
    delisted: list[str] = []
    errors: list[str] = []

    items = sorted(need.items())
    for i, (code, refs) in enumerate(items):
        qty = len(refs)
        try:
            rec = lookup(code)
        except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError,
                json.JSONDecodeError, OSError) as exc:
            errors.append(code)
            print(f"  {code:12s} QUERY FAILED  {type(exc).__name__}: {exc}")
            continue
        finally:
            if i < len(items) - 1:
                time.sleep(args.delay)

        if rec is None:
            delisted.append(code)
            print(f"  {code:12s} NOT IN JLC LIBRARY  ({values.get(code, '')}, x{qty})")
            annotate("error", f"{code} ({values.get(code, '')}, x{qty}) is no longer in the "
                              f"JLCPCB library - it cannot be assembled")
            continue

        stock = rec["stock"]
        flag = ""
        if stock <= 0:
            unavailable.append(code)
            flag = "OUT OF STOCK"
            annotate("error", f"{code} {rec['model']} (x{qty}) is out of stock at JLCPCB")
        elif stock < max(args.min_stock, qty * 10):
            low.append(code)
            flag = f"LOW (need x{qty})"
            annotate("warning", f"{code} {rec['model']} has only {stock} in stock (need x{qty})")

        if flag or not args.quiet:
            print(f"  {code:12s} {stock:>8,} {rec['type']:9s} {rec['model'][:28]:28s} x{qty:<3d} {flag}")

    print()
    print(f"out of stock : {len(unavailable)}")
    print(f"low stock    : {len(low)}")
    print(f"delisted     : {len(delisted)}")
    if errors:
        print(f"query failed : {len(errors)}  ({', '.join(errors)})")
        print("  a failed query is not a healthy part -- treat it as unknown, not fine")

    problems = unavailable or delisted or errors
    print(f"\n{'PROBLEMS FOUND' if problems else 'all parts available'}")
    return EXIT_PROBLEM if problems else EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
