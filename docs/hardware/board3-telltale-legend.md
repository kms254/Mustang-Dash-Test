# Board3 telltale legend

What each of the eight cluster lamps means, what colour it physically is,
and where its signal comes from. Decided 2026-08-19/20 at the bench, with
every colour **verified by eye** on the assembled board — each position
forced one at a time over the serial `tt` command, the night Board3 first
lit its LEDs. The design colours and the stuffed parts agree in all eight
positions.

This file is the authority. `MustangDash/dash_telltales.h` encodes the
firmware half of it in `DASH_LAMP_TT[]`, and
`tests/test_dash_telltales.c` pins that table.

## The cluster

The eight lamps sit as two 2×2 blocks, west (left) and east (right).
Colours fell into rows without anyone planning it, and the legend follows
that grain: **the top row is body signals, the bottom row is warnings.**

|            | left cluster            |                          | right cluster           |                          |
|------------|-------------------------|--------------------------|-------------------------|--------------------------|
| **top — body**     | **TT1** green<br>Left blinker | **TT2** white<br>Headlights on | **TT3** blue<br>High beam | **TT4** green<br>Right blinker |
| **bottom — warn**  | **TT5** orange<br>Low fuel | **TT7** red<br>Parking brake / brake fault | **TT6** red<br>Oil pressure | **TT8** yellow<br>CEL / MIL |

The greens land at the outer edges of each cluster — the correct place
for blinkers — which is what first suggested reading the layout as rows
rather than as silk order.

## Full table

| Net | LED | LCSC | Colour | Cluster / position | Expander | DIM reg | Meaning | Source |
|-----|-----|------|--------|--------------------|----------|---------|---------|--------|
| TT1 | LED1 | C516142 | green | left, top-left | west U11 @ 0x5B | 0x2C | Left blinker | CAN |
| TT2 | LED3 | C516299 | white 6000–7000 K | left, top-right | west U11 @ 0x5B | 0x2D | Headlights on | CAN |
| TT3 | LED4 | C516143 | blue | right, top-left | east U12 @ 0x5A | 0x2D | High beam | CAN |
| TT4 | LED2 | C516142 | green | right, top-right | east U12 @ 0x5A | 0x2C | Right blinker | CAN |
| TT5 | LED5 | C5246349 | orange | left, bottom-left | west U11 @ 0x5B | 0x2E | **Low fuel** | firmware |
| TT6 | LED6 | C516141 | red | right, bottom-left | east U12 @ 0x5A | 0x2E | **Oil pressure** | firmware |
| TT7 | LED7 | C516141 | red | left, bottom-right | west U11 @ 0x5B | 0x2F | Parking brake / brake fault | CAN |
| TT8 | LED8 | C516140 | yellow | right, bottom-right | east U12 @ 0x5A | 0x2F | CEL / MIL | CAN |

Silk numbering is 1-based (TT1…TT8); firmware position bits are 0-based
(TT1 = bit 0). The serial `tt` command speaks silk numbers.

## Why these assignments

Colour follows the ISO 2575 / FMVSS grammar, which is not decoration —
it is what makes a cluster readable at a glance by anyone who has driven
another car:

- **Red = stop now, damage or danger.** Both reds are spent on genuine
  stop-driving faults: oil pressure (TT6) and the brake system (TT7).
  The brake warning is red by regulation (FMVSS 105) and in production
  serves double duty — parking brake engaged *and* brake fluid low /
  hydraulic failure. Those are both switch-to-ground signals and wire-OR
  onto one lamp.
- **Amber = caution, service soon.** Low fuel (TT5, orange) and CEL/MIL
  (TT8, yellow). The MIL is *required* to be amber or yellow by EPA/CARB,
  which is why it moved off a red position during this decision.
- **Green = a system is active and normal** — the blinkers.
- **Blue = high beam**, effectively universal.
- **White** has no standard meaning, which makes it the right home for
  headlights-on: an indicator, not a warning.

### Coolant temperature has no lamp, deliberately

An overheat raises the dash's **full-screen alarm takeover**, which
preempts both TRACK and STREET and is far louder than a 3.5 mm LED. The
lamp would have been redundant with a stronger mechanism. A parking
brake, by contrast, has no other way to reach the driver — so the red
went to the signal that had nowhere else to go, not the one that was
already covered.

The same reasoning demotes oil temperature, volts, fuel pressure, lean
AFR, and the shift indicator: all render on the screens today, and none
is an instant stop-driving event that the screens cannot convey.

### Everything but two lamps waits on CAN

Six of eight positions carry signals the firmware does not compute:

- **Five body signals** — both blinkers, headlights, high beam, parking
  brake — have no PCM source. They come from **RaceCapture** inputs
  (switch-to-ground → Lua → CAN frame). Doing the parking brake this way
  rather than as a discrete input is what keeps the dash a pure CAN
  consumer with **zero GPIO wiring**, and preserves the next revision's
  single spare connector pin.
  **Open item:** confirm the real RaceCapture unit has five spare inputs
  before committing the harness — this has not been checked against
  hardware.
- **CEL/MIL** comes from the Ford control pack PCM over the existing
  0x270-set bus, and belongs to a future dialect round.

So on the bench today only **TT5** and **TT6** can light from the
simulator. That is the legend working as intended, not a gap.

**The seam is built and waiting.** `DashState.tt_signals` is an 8-bit
mask of *physical positions* that `dash_telltale_mask()` joins directly —
no translation, since body signals are positions rather than warning
conditions, and the legend gives the two sets disjoint lamps. A dialect
decoder writes it and owns its own staleness, exactly as `can_owned`
channels work; the telltale layer never ages a bit out. No producer
exists yet, so an unwired bench simply shows those positions dark.

The simulator deliberately does **not** drive it. Faking body signals
would make the CAN path look proven when it is not — the same reason the
simulator writes `DashState` rather than emitting fake CAN frames. For
bench inspection, force the lamps with `tt <n> on` instead, which is
unmistakably an override.

## What this replaced

Until 2026-08-20 the firmware drove condition bit *l* onto TT(*l*+1), a
one-to-one placeholder written before the LEDs had hardware and therefore
never checkable. It was wrong in every row, and the bench proved it twice
in one evening: a forced oil-pressure alarm lit **TT1, the green left
blinker**, and a real low-fuel condition after a long simulator run lit
**TT6, a red**. Both now land where this table says.

## Bench tools

- `tt <1-8|all> on|off` — force any position; a force-ON overlay that is
  OR'd over the live mask, so it can never suppress a real warning, and
  `off` releases the force rather than blanking the lamp.
- `tt sweep` — replay the boot sweep (a column chase across the board's
  four physical LED columns, then a full-row hold that doubles as the
  bulb check).

Forcing one lamp at a time is how this legend was built; it is also the
fastest way to re-verify a board after assembly.
