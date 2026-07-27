---
title: Refill zones before measuring a headlessly routed board, or DRC reports order-of-magnitude phantom violations
date: 2026-07-27
category: developer-experience
module: kicad-routing
problem_type: developer_experience
component: tooling
severity: high
applies_when:
  - "Routing a KiCad board from a CLI or script rather than through the GUI"
  - "A routed board reports hundreds of zero-clearance violations against copper pours"
  - "Comparing autorouter completion or DRC results between tools or runs"
root_cause: missing_workflow_step
resolution_type: workflow_improvement
tags: [kicad, pcb, routing, drc, zones, headless, measurement]
---

# Refill zones before measuring a headlessly routed board

## Context

Board3 was routed headlessly by a CLI autorouter, then checked with
`kicad-cli pcb drc`. The result: **974 violations**, dominated by 512 clearance
errors of which 488 reported `actual 0.0000 mm` — copper apparently touching
copper on different nets, which would mean hundreds of shorts.

The real number was **74**.

The board had `GND`, `+3V3` and `+5V` copper pours that were deliberately
preserved through the strip. The router laid tracks and vias across them and
nothing refilled the pours afterward, so KiCad compared new copper against a
zone outline that had never been recomputed. Every crossing became a
zero-clearance violation.

The KiCad GUI refills zones as part of ordinary editing, so this failure mode is
invisible until routing moves to a script.

## Guidance

**Refill zones as part of the routing step, not as a follow-up.** The two are
inseparable — a routed board that has not been refilled is not in a measurable
state:

```python
import pcbnew

board = pcbnew.LoadBoard(path)
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
pcbnew.SaveBoard(path, board)
```

Wrapping the router is the reliable place for this. A wrapper that routes and
then returns without refilling hands the caller a board whose DRC output is
meaningless, and nothing about the output says so.

**Check what a zero-clearance violation is actually between before believing
it.** `actual 0.0000 mm` between two *nets* is a short. Between a track and a
zone it is almost always a stale fill:

```python
zero = [v for v in violations
        if v["type"] == "clearance" and "actual 0.0000" in v["description"]]
kinds = collections.Counter(
    tuple(sorted(i["description"].split(" [")[0] for i in v["items"]))
    for v in zero)
# {('Track', 'Zone'): 230, ('Via', 'Zone'): 258}  -> stale fill, not shorts
```

That one census turned a board that looked catastrophically broken into one
with 37 real new violations.

## Why This Matters

The error is not small and it is not random — it inflated the result by more
than an order of magnitude, in the direction of "this tool produces unusable
output". Had it gone unchecked it would have been the headline finding of a
tooling evaluation, and the conclusion drawn from it would have been the
opposite of the truth.

It is also silent in both directions. DRC does not warn that zones are stale;
the router does not warn that it left them that way. Nothing in the pipeline
reports a problem, and the number it produces looks precise.

## When to Apply

- Any KiCad routing, cleanup, or copper-modifying operation driven from a script
  or CLI rather than the GUI
- Before running DRC on any board whose copper changed programmatically
- Before comparing autorouter results across tools or runs — an unrefilled board
  on one side of a comparison invalidates it entirely

## Examples

Board3, same routed board, measured twice:

```text
before refill   974 violations   (clearance 512, hole_clearance 203, track_width 199)
after refill     74 violations   (courtyards 25, starved_thermal 23, edge 11, via geom 15)
```

The 488 phantom violations were `Track + Zone` (230) and `Via + Zone` (258).
None were shorts. The `clearance`, `hole_clearance` and `track_width` classes
vanished entirely — they had all been artifacts of comparing new copper against
an outline computed before that copper existed.

## Related

- [Migrating a board from EasyEDA Pro to KiCad loses data silently](../integration-issues/easyeda-pro-to-kicad-migration-silent-data-loss.md) — the other reason DRC numbers on an imported board cannot be taken at face value
