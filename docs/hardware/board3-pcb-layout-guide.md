# Board3 PCB Layout Guide (U8)

Companion to `board3-h755-pin-map.md` and the U8 unit in
`docs/plans/2026-07-24-001-feat-board3-h755-carrier-plan.md`.
Written 2026-07-26, before the layout pass.

## 0. Stackup — use 4 layers

A 144-pin LQFP with QUADSPI, USB, and two CAN buses is not a 2-layer board.
JLC 4-layer is cheap and buys the single most valuable thing here: an
uninterrupted ground plane directly under every fast signal.

| Layer | Use |
|---|---|
| L1 (top) | components + short signal escapes, all three SPI buses |
| L2 | **solid GND pour — never split, never route through it** |
| L3 | power pours: +5V region and +3V3 region |
| L4 (bottom) | secondary routing, remaining GND pour, stitched to L2 |

KTD6 says one continuous ground bonded with stitching vias. On 4 layers that
means L2 is the reference for everything, and L4's pour is stitched to it
every ~10 mm and around the board edge.

## 1. Placement order

Place in this order. Each step locks constraints for the next, so resist
jumping ahead.

**Step 1 — mechanical, edge-driven (these barely move afterward):**
1. Board outline + mounting holes
2. **FPC1 / FPC2 / FPC3** along the panel-facing edge, in physical
   left–center–right order matching the real cluster
3. Barrel jack DC1 and USB-C (USBC1) on the opposite/user edge
4. CAN terminals P1, P2 on an accessible edge
5. LED1–LED8 along a visible edge, evenly spaced, in telltale order
6. Buttons SW1–SW4 and NRST/BOOT0 (SW6/SW7) reachable
7. SWD header H2 + TC2030 land pattern where a probe can sit

**Step 2 — the MCU; orientation is the whole game:**
8. **U1** in the center. Rotate it so each SPI cluster faces its own panel:
   - PA5/PA6/PA7 (SPI1, center) toward FPC2
   - PB13/PB14/PB15 (SPI2, left) toward FPC1
   - PE2/PE5/PE6 (SPI4, right) toward FPC3

   Getting this right removes most of the routing difficulty. If a rotation
   makes two buses cross, try another before accepting it — and remember
   KTD1 lets you swap which SPI peripheral serves which panel if that routes
   cleaner. Firmware follows the board.

**Step 3 — anything that must hug a specific pin:**
9. **C3/C4/C5 (VCAP)** — one per VCAP pin (68/103/140), ≤5 mm, each with its
   own ground via. These are not generic decoupling; they stabilize the
   internal regulator.
10. **100 nF decoupling** — one per VDD pin, same side as the MCU, shortest
    possible loop pin→cap→via→L2. Place these before any routing.
11. **X1 + C41/C42** — as close to PH0/PH1 as physically possible, ground the
    two case pads locally, keep a ground guard around it, and route nothing
    under the crystal on any layer.
12. **L1 + C40 + C37** (VDDA filter) — bead first in the feed, caps at the pin.
13. **C46** at NRST, **R1** at BOOT0.
14. **C51/C52** at the buck (see §2), **C50** at the CH224K.

**Step 4 — blocks that follow their anchor:**
15. Buck U3 + L2 + C48/C49 + R3/R2 (§2)
16. Ideal-diode ORing: U5/Q1 and U6/Q2 between the two inputs and the +5V rail
17. NOR U2 near the PF6–PF9 / PB2 / PG6 cluster; FRAM U7 near PB10/PB11
18. CAN transceivers U8/U9 between the MCU's FDCAN pins and their terminals;
    termination network + jumper right at each transceiver
19. ULN2803 U10 between the MCU lamp pins (PD0–PD7) and the LED row
20. Bulk electrolytics near where current enters/leaves; test points last

## 2. The three circuits that punish sloppy layout

**Buck (TPS563201) — the hot loop.** The loop that matters is:
input cap → VIN pin → internal FET → SW pin → inductor → output cap → GND →
back to input cap. Keep that loop physically tiny.

- C51 (10 µF in) directly across VIN/GND pins, closer than anything else
- SW node copper **as small as possible** — it is the switching-noise
  radiator; wide enough for current, no larger
- C52 (VBST) tight between VBST and SW
- FB divider (R3/R2) away from the SW node; take the top of the divider from
  the **output cap terminal**, not the inductor pad, and route FB as a quiet
  trace hugging ground
- L2 inductor close to SW, output caps C48/C49 immediately after

**USB (D+/D−).** Route as a 90 Ω differential pair: short, tightly coupled,
matched, no stubs, no vias if avoidable, solid L2 ground the whole way.
D9 (USBLC6) goes **right at the connector**, before the pair runs anywhere —
ESD protection placed after the trace defeats the point. Compute the pair
geometry against JLC's actual stackup rather than guessing.

**CAN (H/L).** Differential pair from transceiver to terminal, kept together.
Split termination (60.4 Ω + 60.4 Ω + 4.7 nF) sits **at the transceiver**, with
the jumper in the leg. Keep the stub from the pair to the connector short.

## 3. Power distribution — sized, not guessed

The +5V rail feeds three backlights plus the telltales, and it is a
pass-through rail (no regulation between the jack and the panels).

| Path | Current | Copper |
|---|---|---|
| Input (jack/USB) → ideal diodes → +5V | ~1.5–2 A | pour or ≥60 mil |
| +5V → each FPC backlight pin | ~0.15–0.25 A | ≥20 mil |
| +3V3 → MCU/panels | ~0.8 A | pour or ≥30 mil |
| Signals | — | 6–8 mil |

KTD6 is explicit that the backlight return must ride the ground pour, not a
thin trace. Each FPC's ground pins get **multiple vias straight down to L2 at
the connector** — this is the exact failure that cost a bench night (the
shared-ground IR-drop learning in `docs/solutions/`); do not economize here.

The ideal-diode FETs (Q1/Q2) carry the full rail: wide copper on both source
and drain. The LM74700 senses the FET's own drop, so land the controller's
ANODE/CATHODE connections on the FET pads, not upstream.

## 4. Signal routing rules

- **SPI**: short, roughly length-matched per bus, over unbroken L2. The 33 Ω
  series resistors (R32–R37) go **at the MCU pin**, not near the connector —
  they damp the driver, so they must sit within a few mm of the output.
- **QUADSPI**: keep the six nets as a group, similar lengths, short. R9 (33 Ω)
  at the MCU clock pin.
- **Crystal**: shortest possible, guarded, nothing routed underneath.
- **No signal may cross a plane split.** With a solid L2 this is automatic —
  which is the reason for 4 layers.
- Keep the buck's SW node and inductor away from the crystal, the USB pair,
  and the QUADSPI group.

## 5. Verification before ordering (U9)

1. `tools/kicad_verify.py <board> --baseline <import>` reports NEW = 0 against
   JLC's rule set — the imported board carries 41 violations no layout work
   removes, so the bar is no new violations, not zero
2. Every FPC ground pin has ≥2 vias to L2
3. Visually trace the buck loop and confirm it is small
4. Confirm no trace crosses a plane gap
5. Silkscreen: every R/C/IC carries its value or model number on its own
   footprint body (standing project rule), plus polarity marks, pin-1 marks,
   connector labels, and the CAN termination jumper legend
6. Update `board3-h755-pin-map.md` to the **as-routed** table — after layout
   freeze the firmware pin constants are written from that document (KTD1)

## Related

- [Board3 pin map](board3-h755-pin-map.md)
- [Board3 carrier plan](../plans/2026-07-24-001-feat-board3-h755-carrier-plan.md)
