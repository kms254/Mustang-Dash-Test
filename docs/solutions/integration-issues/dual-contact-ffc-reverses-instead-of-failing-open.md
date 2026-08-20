---
title: "A dual-contact FFC connector reverses a flipped cable instead of failing open"
date: 2026-08-20
category: integration-issues
module: display-bringup
problem_type: integration_issue
component: development_workflow
symptoms:
  - "Board completely dead the moment the FFC is seated: no telltales, no boot sweep, no USB enumeration"
  - "Removing the FFC restores normal boot instantly, with no damage"
  - "Rail collapses because the cable presents its conductors mirrored -- +3V3 lands on the panel's ground"
  - "Visual inspection cannot settle it: both faces of a dual-contact slot carry contact fingers"
root_cause: incomplete_setup
resolution_type: environment_setup
severity: high
related_components:
  - tooling
tags:
  - ffc
  - zif
  - dual-contact
  - cable-orientation
  - panel-bringup
  - board3
  - rail-short
  - bench-workaround
---

# A dual-contact FFC connector reverses a flipped cable instead of failing open

## Problem

Seating a panel FFC into Board3 killed the board outright — no LEDs, no USB —
and unplugging it restored normal boot. The cable was undamaged and correctly
made. It was inserted with the wrong face up, and because the connector takes
contacts on **both** faces, that did not produce an open circuit: it produced a
**mirrored pinout**, putting `/+3V3` onto the panel's ground.

## Symptoms

- **Dead on connect.** With the FFC seated the board did nothing: no telltales,
  no boot sweep, no CDC enumeration. `[System.IO.Ports.SerialPort]::GetPortNames()`
  showed the dash port gone entirely.
- **Instantly fine when unplugged.** Removing the FFC and re-powering booted
  normally — which localises the fault to the cable or the load in one step.
- **No damage, either time.** The rail collapsed and recovered; nothing was
  degraded by the event.
- **Nothing to see.** The cable end was clean, fully inserted, and latched.

## What Didn't Work

- **Reasoning about the contact side visually.** This is the trap specific to
  the part. On a single-contact ZIF you can look into the slot, see which face
  carries the gold fingers, and orient the cable to match. A **dual-contact**
  connector has fingers on *both* faces, so "which side are the contacts on"
  has no discriminating answer. The eye cannot resolve it; only a meter can.
- **Reasoning about the fold.** The cable has to be folded 180° to reach a
  connector that opens toward the board interior, and it is genuinely easy to
  talk yourself into either answer about which face ends up down after the
  fold. Working it out geometrically is slower and less reliable than measuring.
- **Suspecting the power budget first.** A plausible competing hypothesis —
  the panel's backlight browning out a laptop USB-C port — has the same headline
  symptom. It was wrong here, and the FFC-removal test separated them in one
  move without needing to resolve it.

## Solution

**Flip the cable end-for-end about its long axis** (the other face up) and
reseat. That reverses the conductor order back to correct, and the board came
up immediately — first glass at 60 fps, and all three panels at `eve=ok,ok,ok`
the same day.

**Before applying power, prove it with an ohmmeter:**

1. Board **unpowered**, FFC seated and latched, **panel attached at the far
   end**.
2. Measure **TP2 (`/+3V3`) ↔ TP3 (`/GND`)** and **TP1 (`/+5V`) ↔ TP3**.
3. **Near 0 Ω → reversed.** Pull the cable, flip it, measure again.
4. Not a short (may read low and climb as the panel's input caps charge) →
   orientation is good, apply power.

**The panel must be attached for this test.** A reversed cable with nothing on
the far end shorts nothing — the mirrored conductors are only tied together
*through the panel*. Testing the cable alone gives a clean reading on a cable
that will kill the board.

For a positive check rather than absence-of-short, buzz **TP2** to the panel's
3.3 V pin: continuity means conductor 1 really is landing on pad 1.

## Why This Works

Rotating an FFC 180° about its long axis swaps which face is up **and reverses
the left-to-right conductor order** — conductor 1 ends up where conductor 20
was. On a single-contact connector the wrong face simply doesn't touch, so the
failure is an open circuit and a puzzling dead panel. On a dual-contact part
("Double-Sided Contacts, Top and Bottom Entry" in LCSC's data; Molex markets it
as "Dual contact terminal — offers space savings") the contacts meet the cable
either way, so the wrong face **connects, mirrored**.

Board3's FPC pinout makes that immediately destructive: pad 1 is `/+3V3`,
pad 2 `/GND`, pads 17/18 `/+5V`, pads 19/20 `/GND`. Mirrored, the board's 3V3
meets the panel's ground conductor and vice versa — a hard short across the
regulator, which current-limits and drops the rail before the MCU can run.
Hence dead-on-connect, alive-on-disconnect, and no damage.

The feature framing is what makes this non-obvious: dual contact exists to
improve contact reliability and let a cable of either type be used. Nothing in
the datasheet presents it as a hazard, and it converts a benign, obvious
failure mode into a silent, destructive-looking one.

## Prevention

1. **Ohm the rails through the cable before every first power-up**, with the
   panel attached, per the procedure above. It is 30 seconds and it is
   definitive where inspection is not. This is now in
   `docs/hardware/board3-bringup-card.md`.
2. **Treat "dual contact" / "top and bottom entry" as a warning label** when
   selecting or reworking an FFC connector. It changes the wrong-orientation
   failure from open-circuit to reversed, and it removes visual inspection as a
   check. If a design can tolerate a single-contact part, the failure mode is
   strictly friendlier.
3. **Ask what regime a bench workaround enters that the design never
   contemplated.** This is the general lesson, and it is worth more than the
   connector detail. The fold existed only to work around a placement defect
   (the connectors open toward the board interior — see
   `docs/solutions/conventions/fixing-the-instance-is-not-fixing-the-class.md`),
   and the fold is what inverted the contact face. **A workaround for a
   mechanical problem introduced an electrical failure mode the original design
   could never have had.** Other members of that family worth checking for:
   reversed conductors, unmated shields, cables under compression, connectors
   mated at an angle, and strain relief defeated by a detour.
4. **When a board dies on connect, remove the cable first.** It is the cheapest
   bisect available — one action separates "board or firmware" from "cable or
   load," and this bench has now had that answer come back "cable" twice.

## Related Issues

- `docs/solutions/integration-issues/eve-panel-bringup-no-usb-enumeration-diagnosis.md`
  — the same headline symptom (panel connect kills the board and USB
  enumeration) from a different cause: a physically damaged FFC shorting VDD to
  GND. Together these are the two known ways an FFC takes the board down, and
  the FFC-removal bisect is the shared first move.
- `docs/solutions/conventions/fixing-the-instance-is-not-fixing-the-class.md`
  — the placement defect that forced the fold. This failure is downstream of
  that one.
- `docs/hardware/board3-bringup-card.md` — the ohm-test procedure, in the
  panels row of the bring-up debt list.
- `docs/hardware/board3-telltale-legend.md` — the FPC pinout and the bench
  facts for these connectors.
- Cable hygiene, same family: this project retired a set of tin FFCs for
  dry-circuit oxide and tin-on-gold fretting (the left panel's `0x68`
  signature). "Suspect the cable" is an earned reflex here.
