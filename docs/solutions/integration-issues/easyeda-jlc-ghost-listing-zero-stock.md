---
title: EasyEDA part search shows zero stock on a well-stocked JLC basic part
date: 2026-07-25
category: integration-issues
module: easyeda-workflow
problem_type: integration_issue
component: tooling
symptoms:
  - "A canonical JLC basic part (e.g. C25804, 10k 0603, ~5.7M stock) shows LCSC Stock:0 / JLCPCB Stock:0 in EasyEDA's part search"
  - "Search results contain parts that look unrelated to the query (different package, through-hole, different value)"
  - "Sorting by stock floats zero-stock rows to the top, reinforcing the impression the part is gone"
root_cause: wrong_api
resolution_type: workflow_improvement
severity: medium
tags: [easyeda, jlcpcb, lcsc, bom, part-sourcing, ghost-listing]
---

# EasyEDA part search shows zero stock on a well-stocked JLC basic part

## Problem

While placing the BOOT0 pull-down for Board3 (2026-07-25), searching EasyEDA for `C25804` (UNI-ROYAL 0603WAF1002T5E, 10 kΩ 0603 — a canonical JLC basic part) appeared to show zero stock, nearly triggering an unnecessary hunt for an alternative part.

## Symptoms

- "Zero stock" in the app on a part that the live JLC API showed with 5,699,528 units.
- The visible results were actually `C2585804` (a through-hole metal-film resistor) and `C2580804` (a KOA 2010 chip) — not `C25804` at all.

## What Didn't Work

- Trusting the in-app search result at face value and looking for a replacement part — the replacement search was solving a problem that didn't exist.

## Solution

Two independent traps compound here; check both before concluding a part is out of stock:

1. **Substring-matched part codes.** EasyEDA's search matches the query as a *substring* of the LCSC code, so `25804` matches `C2585804` and `C2580804`. Combined with a stock-ascending sort, unrelated zero-stock parts dominate the first page. Fix: search the **exact MPN** (`0603WAF1002T5E`) with a package filter, or read the `LCSC Part#` field of each row and match it exactly.
2. **Ghost listings.** The JLC catalog carries duplicate rows for the same MPN under manufacturer "JLCPCB Assembly" with `C9900…` codes (here: `C9900298159`, `C9900021713`) — placeholder entries with permanent zero stock that shadow the genuine manufacturer listing. Fix: never accept a `C9900…` code or a "JLCPCB Assembly" manufacturer row; pick the row whose manufacturer is the real one (UNI-ROYAL, TI, Samsung…).

Fastest bypass when the part already exists in the design: copy a placed instance of the same part on the schematic instead of searching at all — identical BOM row, no search dialog.

Verification path from an agent session: the pcbparts `jlc_stock_check` live API listed all three rows side-by-side (genuine `C25804` at 5.7 M stock, two `C9900…` ghosts at 0), which is what exposed the trap.

## Why This Works

The in-app "zero stock" was never about the part — it was the search surface returning *different parts* (substring matches) and *ghost duplicates* (assembly-placeholder rows). Matching on exact LCSC code or exact MPN + real manufacturer selects the row that JLC actually stocks and assembles from.

## Prevention

- BOM rule (already Board3 plan R16): pin every part by the **genuine manufacturer's LCSC code** in the part attributes; treat any `C9900…` code in a BOM export as a defect.
- When the app claims zero stock on a supposedly-basic part, suspect this trap *first* — confirm against a live stock check (pcbparts `jlc_stock_check`, or the LCSC website by exact code) before hunting alternatives.
- Prefer copying already-placed instances of common passives over re-searching.

## Related Issues

- Board3 sourcing snapshot and R16 (BOM pins parts by LCSC code): docs/plans/2026-07-24-001-feat-board3-h755-carrier-plan.md
