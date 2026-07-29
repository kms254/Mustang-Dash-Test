---
title: Authoring a full schematic through the EasyEDA MCP bridge — division of labor, guard repairs, and pin-truth discipline
date: 2026-07-25
category: developer-experience
module: easyeda-bridge
problem_type: developer_experience
component: tooling
severity: high
applies_when:
  - "An agent is wiring or editing an EasyEDA Pro schematic through the easyeda-mcp-pro bridge"
  - "connect_pins_by_net returns a coincides-with-existing-wire refusal"
  - "A component move must not silently detach or steal wires"
  - "Wiring any polarized or multi-pole part (LEDs, diodes, tact switches, crystals) from a library symbol"
tags: [easyeda-bridge, schematic-authoring, net-wiring, pin-polarity, erc, workflow]
---

# Authoring a full schematic through the EasyEDA MCP bridge — division of labor, guard repairs, and pin-truth discipline

## Context

Board3's entire schematic phase (MCU power tree, three SPI buses, USB-C + PD +
ideal-diode power entry, NOR + FRAM, dual CAN, SMD telltales — ~160 components)
was authored in one session through the bridge, 2026-07-25. The workflow below
is what actually survived contact; each rule was learned from a live failure.

## Guidance

**Division of labor.** `schematic.placeComponent` times out after 15 s without
landing anything — even with server and extension version-matched (verified by
component-count readback before and after every attempt). Treat placement as
broken: Kevin places parts in the app (library search by LCSC code, or
copy-paste of an existing same-device part, which clones LCSC/footprint/BOM
fields), then the agent reads them back with `schematic_components` and does
ALL wiring by `primitiveId` via `connect_pins_by_net`. Deletes, moves,
no-connect markers, ERC, and save all work through the bridge.

**The coincident-point guard, and the repair pattern.** EasyEDA Pro auto-merges
primitives that share a coordinate, so the bridge refuses a connect whose pin
point coincides with a wire on a different net — that refusal is a prevented
short, not an error to work around. It fires constantly when parts are placed
in tight rows (pin span ~40 canvas units, stub length 10; anything under ~60
units of spacing collides). The repair is always the same three steps:

1. Delete the part's own freshly created stubs (from `created_primitive_ids`).
2. Move the part clear with `schematic_modify_primitive` (x/y).
3. Reconnect every affected pin, including the neighbor pin the guard refused.

Ask for ≥60 canvas units between parts at placement time to avoid the loop.

**Never trust a move's wire handling — verify nets after every move.**
`modify_primitive` claims "component moves keep connected wires attached," but
in practice followed wires sometimes do not move (leaving a live-net stub
sitting exactly on a *different* part's pin — a latent short the netlist then
shows as a wrong-net membership), and the follower can grab a *neighbor's* wire
whose endpoint merely touched the moved part's old pin. After any move, re-read
`schematic_nets` (or `verify_write`) and check the affected nets' exact
membership before wiring on.

**Read pin truth before wiring anything polarized or multi-pole.**
`schematic_component_pins` returns real pin names — use them, never the
designator convention. Live catches from one session: 0805 LED symbols disagree
on polarity (some put A on pin 1, others on pin 2 — half of eight LEDs were
reversed); 4-pin tact switches pair pins 1-2 and 3-4 as the SAME internal pole
(wire diagonally, 1↔4, which is correct under every pairing convention); 4-pad
crystals carry the electrodes on pins 1/3 with 2/4 as case ground. When the
symbol graphic is the only source (pin names like "A"/"B"), capture the canvas
region and look at the drawn internal structure.

**ERC after every unit, not at the end.** `erc_run`'s inferred floating-pin
census is precise enough to catch a single missed supply pin (it caught a
FRAM VSS left floating). Error count 0 with an explainable floating list is
the per-unit exit gate; warnings from future-unit pins are expected and fine.

**Keep the delete list complete.** Deleting a component does not delete its
wire stubs. Every replacement (LED swap, buffer removal, connector change)
must delete component + all its stubs in one call, or the orphaned stubs keep
their net tags and later collide with new wiring as phantom-net obstacles.

## Why This Matters

The netlist is copper: a silently merged +3V3/GND or a reversed LED survives
ERC-by-eyeball and becomes a board respin. The guard + verify-after-move +
pin-truth discipline caught, in one session: two would-be power shorts, eight
potential LED polarity errors, a permanently-pressed button wiring carried
over from a previous board, and one floating supply pin. The workflow also
keeps the human in the loop exactly where the tooling is weak (placement)
and the agent where it is strong (systematic netlist work + verification).

## When to Apply

- Any bridge-driven schematic wiring session on this project (Board3 layout
  fixes, future board spins)
- Whenever `connect_pins_by_net` reports a failure containing "coincides with
  an existing wire" — apply the three-step repair, never force
- Before wiring any part whose symbol was not authored this session

## Examples

Guard refusal and repair, as seen live:

```text
Refusing to connect pin "2" on <C48> to net "GND": point (1050, 1305)
coincides with an existing wire on net "+3V3" ...
```

→ delete C49's two fresh stubs → move C49 from (1080,1305) to (1080,1355) →
reconnect C49.1→+3V3, C49.2→GND, then retry C48.2→GND: clean.

Pin-truth check before wiring a "simple" LED:

```text
LED4 (XL-2012UBC): pin 1 = K, pin 2 = A   ← reversed vs LED1 (pin 1 = A)
```

## Related

- [Verifying EasyEDA design state by reading the .eprj2 project file](easyeda-eprj2-agent-verification.md)
- [EasyEDA/JLC ghost listing with zero stock](../integration-issues/easyeda-jlc-ghost-listing-zero-stock.md)
