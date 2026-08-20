# Next-Rev Interconnect — Single External Connector + LED Daughterboard

Captured 2026-08-12 from a design discussion. Status: **direction set, nothing
committed to a schematic yet.** These are decisions-in-principle for the
revision after Board3 (the in-car packaging era), recorded so the reasoning
doesn't have to be re-derived. Part numbers below were verified in stock at
Mouser on 2026-08-12.

---

## 1. External interface: one AMPSEAL 16 8-way connector

The dash gets a **single automotive-grade external connection**: power in, two
CAN networks, nothing else. Chosen family: **TE AMPSEAL 16** (sealed, latched,
vibration-rated, PCB-mount header — the same family Haltech/Link/AEM ship on
real ECUs).

### Pin budget (8 positions)

| Pin | Signal | Notes |
|---|---|---|
| 1 | VIN (12 V) | ~1–2 A worst case (backlights via onboard buck); contacts rated 13 A |
| 2 | GND | |
| 3 | CAN1 H | keep each CAN pair on adjacent pins so the twisted pair lands naturally |
| 4 | CAN1 L | |
| 5 | CAN2 H | |
| 6 | CAN2 L | |
| 7 | IGN sense | switched-12V input so "key on" is a wire, not inferred from CAN traffic |
| 8 | spare | sealing plug until used |

(Pin assignment is illustrative — final numbering to be set against the header
drawing when the schematic is drawn. The load-bearing choices are: 8 positions,
CAN pairs adjacent, IGN sense wired, one true spare.)

### Parts (verified in stock 2026-08-12)

| Role | PN | Description | Price / stock |
|---|---|---|---|
| Board-side header | **TE 776280-1** | 8-pos right-angle PCB header **with panel gasket** | $9.71, 358 in stock (Mouser 571-776280-1) |
| Harness plug | **TE 776494-1** | 8-pos plug, 14–18 AWG, red / A-key | $4.84, 817 in stock |
| Socket contacts | **TE 776299-2** | HDSF size-16 socket, Ni, 16–18 AWG | $1.05 ea — buy ~20 (8 live + practice/re-pins) |

- 776279-1 is the same header **without** the gasket — do not substitute; the
  gasket is the enclosure seal (see §2).
- **OPEN ITEM — key match:** 776494-1 is A-key (red). The header's key letter is
  not in Mouser's description; confirm A-key from the customer drawing
  (ENG_CD_776279/776280) before ordering. Same-family-different-key is the
  classic AMPSEAL footgun.
- Harness: **18 AWG throughout** (inside both the contact range and the standard
  cavity-seal range; power is comfortable at 1–2 A). Twist the CAN pairs.
  Sealing plugs in unused cavities preserve the IP rating.
- Crimp tool: HDSF stamped-and-formed contacts; Waytek and WireCare stock
  economical AMPSEAL 16 crimpers — no need for TE's certified tool.

### Alternatives considered

- **Deutsch Autosport (AS)** — the named inspiration. Real motorsport quality,
  but ~$100–300 per side, its own crimp tooling, and it's panel-mount by
  nature: it mounts to the *enclosure* and needs an internal pigtail to the
  board (one more connection, though it fully decouples enclosure tolerance
  from the board). Still viable if the AS look becomes a goal in itself;
  architecture would be jam-nut AS on the wall + short internal pigtail.
- **Deutsch DTM** — equally legitimate; AMPSEAL 16 chosen for its PCB header
  with integrated panel gasket (one-piece solution, no pigtail).
- **3D-printing the board-side shell** — rejected. The $10 header is precision
  where it counts: molded seal surface (layer lines are leak paths and abrade
  the plug seal), latch ramp at specified retention force, and eight plated
  pins molded in at position with sealed pass-throughs. A printed shell still
  needs those pins placed to ±0.1 mm and retained — in practice glue joints,
  i.e. future intermittents, the exact failure class this bench keeps paying
  for. Also cabin temps clear PETG's glass transition. Buy the mated pair,
  print everything around it.

### Project-rule collision (flag now, not at fab time)

The TE parts are **not on LCSC**, so the header will be a hand-soldered part
outside the JLC assembly flow. Two consequences:

1. `kicad_fab.py` **silently drops** unsourced parts from the BOM — the header
   must be explicitly documented as manually-sourced, not discovered missing.
2. The board rule "every part carries a 3D model" still applies — TE publishes
   the STEP (download from te.com in a browser; the site 403s scripted
   fetches). Attach it to the footprint; it's also what drives the enclosure
   workflow below.

---

## 2. How the header mounts (it doesn't bolt to the enclosure)

The header is fixed to the **PCB** (soldered pins; flange seats on the board).
The enclosure wall closes over the shroud nose, and the flange gasket is
compressed between the flange face and the **inside surface of the wall**.
Compression comes entirely from the board mounting geometry.

```
        outside │ enclosure wall │ inside
                │                │
   plug mates  ┌┴────────┐       │
   here ──────▶│  shroud  ├──────┤ ◀── gasket squeezed between
               └┬────────┘flange │     flange and inner wall face
                │     pins ──┐   │
                └────────────┼───┘
      ═══════════════════════╧═══════ PCB
                  ▲
        standoff height sets gasket squeeze
```

Design chain (Onshape):

1. **Cutout**: rectangular opening per the drawing's recommended panel opening.
   It only clears the nose — loose is fine; the gasket seals the perimeter,
   not the cutout edge. Position accuracy comes from the board bosses.
2. **Standoff height is the load-bearing dimension**: board-mounting plane to
   inner wall face = flange seating height − gasket squeeze (~20–30% of free
   gasket thickness, from the drawing). Too tall → no seal; too short → wall
   pre-loads the solder joints.
3. **Board screws near the connector.** Mating push goes flange-into-wall
   (free); unmating pull is resisted only by solder + board screws. Keep a
   boss within ~15–20 mm of the header.
4. **Assembly order before closing the rectangle**: a closed cutout in a solid
   wall forces nose-first board insertion. Keep the cutout fully in the base
   half of a clamshell (or split it across the seam) so the board drops in and
   the lid never touches the connector.
5. **OPEN ITEM**: check the drawing for flange screw holes into the PCB. If the
   header is solder-only, point 3 matters more, and a printed internal brace
   cradling the header body is a legitimate zero-cost reinforcement.

Modeling workflow: TE STEP → KiCad footprint → `kicad-cli pcb export step` →
import board STEP into Onshape as one component → derive the cutout from the
connector geometry **in place** (never from datasheet numbers alone), so a
board revision that moves the connector moves the cutout with it. Model the
mating plug + boot + harness bend radius too — the classic enclosure mistake is
a connector that's perfect until you try to plug it in inside the dash cavity.

---

## 3. LED telltale daughterboard: AWs stay home, board goes passive

Context: telltale LEDs may move to a daughterboard so they sit exactly behind
the bezel windows. Within the gauge cluster (inches of cable).

**Decision: both AW9523Bs stay on the main board; the daughterboard is purely
passive** — LEDs and copper, nothing else.

The reasoning, because the obvious version of it is backwards: the instinct is
"keep signal wires short," but the LED lines are the *robust* ones — the
AW9523B in LED mode is a constant-current sink, so those lines are DC and
current-regulated; length, resistance, and cable capacitance are irrelevant.
The fragile bus is the **I2C**, which on this design shares its trunk with the
FRAM (U7, the odometer). Keeping the AWs on the main board keeps I2C off the
harness entirely. A glitched telltale write self-heals on the next refresh; a
glitched odometer transaction does not.

What this buys:

- Zero schematic churn on the AW section — POR-safe port mapping, RSTN
  network, address straps, and the host-tested `dash_calibration.h` ISEL table
  all carry over unchanged.
- No resistors on the daughterboard (the AW's current control replaces them).
- Trivially diagnosable failures: an LED that doesn't light is a continuity
  check, not an I2C forensics session.

### Daughterboard connector

- ~10–12 pins: **GND, +5V, TT1–TT8 in lamp-bit order** (so nobody consults a
  table with a meter in hand), plus a second GND (covers a marginal contact)
  and 1–2 spare sinks routed to unpopulated LED footprints (a ninth telltale
  later = solder one LED + firmware, not a new board). Each AW has 8 unused
  ports.
- **Gold contacts, not tin** — mA-level sinks are dry-circuit territory; tin +
  vibration = fretting corrosion = the telltale that flickers years later.
- Keyed/shrouded or obvious-orientation — a reversed connector puts +5V on a
  sink line (AW pins are 6 V-rated, so it likely survives, but don't find out).
- **Cable type**: board-to-board header + standoffs if the geometry allows
  (nothing to fatigue) > FFC (fine for one-time installation with name-brand
  cable; the bench FFC failures were insertion-cycle wear + cheap tin cable,
  not the medium) > discrete-wire latched crimp (JST GH/PA) if the cable
  crosses a gap that flexes during assembly or service. If FFC: back-flip ZIF,
  gold, contact-side orientation arrow on the silk.
- Anchor any cable near both ends so vibration flexes mid-span, not at the
  terminations.

### Optics (the daughterboard's real job)

- LED placement from the **bezel drawing**, not a tidy grid.
- Light-bleed walls or a gasket between adjacent telltale windows — the
  classic cluster defect, and it's mechanical, not electrical.
- Matte black soldermask.

### Escalation path (only if the board ever leaves the cluster)

Tens of cm through the car, or growing LED count → move the expanders to the
daughterboard and bridge I2C properly: PCA9517A buffer at the cable boundary
minimum, PCA9615 (differential I2C) if genuinely remote — the non-negotiable
part is isolating the FRAM's bus segment from cable capacitance and EMI.
Truly remote indicator panel → its own CAN node (RaceCapture already
establishes CAN as the vehicle bus). Both are overkill inside the cluster.

---

## 4. Programming: Tag-Connect only — USB is dropped (decided 2026-08-19)

**Decision, made the night Board3 hit first light: the next rev carries no USB
at all.** Programming, debug, and recovery are SWD via the TC2030 pads, full
stop. This was mooted 2026-08-12 and confirmed after a full bench session on
the real Tag-Connect flow (BCM4 option-byte write, two full flashes, live SWD
PC-sampling that diagnosed both boot hangs) proved it comfortable — Kevin has a
retaining clip, so even long sessions are hands-free.

What this deletes from the board (the whole USB input path): USBC1, the
USB-side ideal diode (LM74700 + AO4406A), the USBLC6 ESD part, the CH224 PD
sink and its dividers, and the VBUS sense chain. Note §1 already moves the
power input to 12 V VIN through the AMPSEAL, so the *barrel* 5 V input dies on
this rev too — the dual-5V-input ORing architecture collapses to one path:
VIN → buck. Two ideal-diode controllers and change, gone.

**Accepted losses, recorded once, eyes open:**

- **USB DFU field reflash.** On Board3, BOOT+RESET → DFU was the zero-tools
  "is the MCU/power/USB path alive" diagnostic AND the no-debugger reflash
  path. On this rev, both jobs need the ST-LINK + Tag-Connect. Consequence
  for placement: **the TC2030 pads must be reachable with the dash installed**
  (or pulling the cluster to reflash is the accepted cost — decide when the
  enclosure exists, but decide on purpose).
- **USB CDC serial console.** The `ok`/`err` protocol, `/dash` skill,
  `status`, `odo set`, forced alarms — tonight's whole bench session rode CDC.
  This needs a new home before the schematic freezes. Candidates, roughly in
  order of appeal: **RTT over the same SWD link** (bidirectional, zero extra
  pins, works through the Tag-Connect already attached; OpenOCD supports it),
  a 3-pin UART header (bench-only, near-zero cost), or a CAN console (car-
  native, but makes the bench depend on a CAN adapter). The BOOT/RESET
  buttons' *reflash* role goes away with DFU, but RESET stays useful.

  **GATE (decided 2026-08-19): the RTT console shim must be implemented and
  bench-proven — on Board3, where CDC still exists as the fallback — before
  any v2 schematic deletes USB.** Board3 is the risk-free proving ground:
  same die, same debugger, and if RTT disappoints, USB is still there. The
  in-car operating model this enables: car 12 V powers the board always;
  the tag (pads reachable with the dash installed) carries programming AND
  the console; the programmer never powers the board; a flash interrupted
  by power loss is a reflash, not a brick.
- The boot-banner-races-CDC-enumeration problem (500 ms wait, bit us at first
  light) becomes moot or changes shape with the console choice — RTT and UART
  don't enumerate.

Firmware consequence: the next-rev env drops `USBCON`/`USBD_USE_CDC`, and
`Serial` maps to whatever the console decision lands on.

---

## 5. Bench FFC restock (already ordered 2026-08-12)

For the RiBus panel connections (20-pin, 0.5 mm):

- **Molex Premo-Flex 15020-0219** — 0.5 mm, 20 ckt, gold (AU), 203 mm,
  Type A (same-side contacts — verified against the working cable), qty 5.
- The retired cables were **tin** — three strikes: dry-circuit oxide at SPI
  signal currents (the left panel's 0x68 failure signature), soft plating worn
  out by bench insertion cycles, and tin-on-gold mixed-metal mating into the
  panels' gold-flash ZIFs (accelerated fretting). **Toss them, don't bin
  them** — a backup that reintroduces the failure mode isn't a backup.
- Cables are consumables: a suspect one gets swapped and binned, not debugged.
  Mark the known-good set.
