# Handoff: EVE Triple Dash — Mustang Race/Street Digital Cluster

## Overview
A three-screen digital instrument cluster for a track-built 1999 Ford Mustang (Gen 4 Coyote Aluminator swap, T56 Magnum close-ratio, 3.73 rear, 26″ tires, WR Blue Mica).

Target hardware:
- **MCU**: Teensy (4.x assumed) driving **Riverdi EVE displays** (Bridgetek EVE graphics controllers — BT81x family)
- **Displays**: one 7″ center (800×480) + two 5″ side panels (800×480), mounted in a single CNC bezel with physical turn-signal LEDs at the outer bosses
- **Data**: Ford Gen 4 Coyote control pack CAN, 2× ECUMasters PMU16 (custom CAN), RaceCapture (GPS speed, lap timing, IMU)
- **Mode switching**: TRACK vs STREET view is selected by a **CAN input** into the Teensy (physical switch → PMU → CAN broadcast, or direct digital input — TBD)

### Build phases
1. **Phase 1**: Center 7″ display only (both TRACK and STREET modes, alarm takeover, telltale dots, odometer)
2. **Phase 2**: Add left 5″ (engine) and right 5″ (timing/road) displays

## About the Design Files
The files in this bundle are **design references created in HTML** — an interactive prototype showing the intended look and behavior. They are NOT production code. The task is to **recreate these designs as EVE display lists / widgets on the Teensy** (e.g. via the Bridgetek EVE library, GD2/GDX, or a custom display-list generator). The HTML file runs in any browser and includes a live simulator plus a Tweaks panel (mode, units, accent, alarm test, manual RPM/MPH override) — use it as the visual source of truth.

## Fidelity
**High-fidelity.** Colors, typography, spacing, gauge geometry, and thresholds are final and should be matched as closely as the EVE rendering pipeline allows. Fonts will need conversion to EVE bitmap/custom fonts (see Typography).

## Screen Resolutions & Canvas
Each panel renders at **800×480** (native EVE resolution). The HTML mock draws the 7″ at 620×400 CSS px and the 5″ at 420×320 — **scale all dimensions in this document by ×1.29 (7″) or ×1.90 (5″ width) to native**, or better, treat all layout as proportional percentages of the panel.

## Design Tokens

### Colors
| Token | Hex | Use |
|---|---|---|
| Screen background | radial `#0e141b → #05070a` (7″), linear `#0b0f14 → #06080b` (5″) | panel fill (a flat `#080b0f` is acceptable on EVE) |
| Hairline / divider | `#151b22`, `#11171e`, `#131a22` | grid lattices, separators |
| Label gray | `#5f6b76` | small caps labels |
| Faint label gray | `#454f59`, `#49545f` | footer labels, compressed tick labels |
| Secondary value | `#c3ccd4`, `#8b97a3` | last-lap, odometer |
| Primary value white | `#f4f7f9` / `#e8edf2` | all live numerals |
| **Accent (gold)** | `#e8a33c` | RPM bar/arc fill, gauge arcs, P3 position |
| Green (ok/throttle) | `#3ddf77` | throttle bar, PMU on, delta negative |
| Amber (warn) | `#f2b13e` | warning thresholds, low fuel |
| Red (alert) | `#ff3b3b` (fills) / `#ff5252` (text) | redline, alarms, brake bar |
| Best lap purple | `#b79aff` | BEST lap time |
| Tach red-zone track | `#5c1616` | static red-zone arc segment |

### Typography
- **Numerals**: *Saira Condensed* (weights 600–800), always tabular. Convert to EVE custom font at 3–4 sizes (small ~20px, mid ~34px, large ~64px, hero ~150px at native scale).
- **Labels**: *Chakra Petch* (500–600), small caps style with wide letter-spacing (0.16em–0.44em).
- Google Fonts sources; both are OFL-licensed and safe to convert/embed.

## Screens / Views

### CENTER 7″ — TRACK mode
- **Shift lights** (top, centered): 15 round LEDs, 18px dia (mock scale), 9px gap. Linear 0→8000 RPM (each LED = 533 RPM). Colors: LEDs 1–10 green `#3ddf77`, 11–13 amber `#f2b13e`, 14–15 red `#ff3b3b`. Above **7600 RPM all 15 flash red** at ~8 Hz. Off state: dark radial `#1a222b→#0d1218`. Hairline divider below.
- **GPS SPEED hero** (left ~2/3): label "GPS SPEED" (11px, letter-spacing .44em) → speed numeral (148px, weight 700) → "MPH" (14px, .34em).
- **RPM linear bar** under speed: label row "RPM" + live value (21px; white <7100, amber `#f2b13e` 7100–7600, red >7600). Bar: 8px tall pill, dark track `#0c1117`, fill = gold gradient (red >7600), **redline marker at 96%** (red 2px line, 50% opacity). Scale marks 0/2/4/6/8 beneath (9px; the "8" in muted red `#864b4b`).
- **Right column** (186px, hairline left border): LAP TIME (34px), DELTA value (24px, green negative / red positive) + **delta bar**: center-zero pill, fill extends left (ahead, green) or right (behind, red) up to ±50% for ±1.0s.
- Data: GPS speed & lap timing from RaceCapture; RPM from Coyote CAN.

### CENTER 7″ — STREET mode
- **Dual sweep gauges**, asymmetric: SPEED dial 340×305 (dominant), TACH dial 220×197. Both use the same gauge geometry (below).
- **Speedometer**: 0–200 MPH, **non-linear**: 0–140 occupies 82.35% of the 240° sweep (major ticks every 20 = 28.24° apart); 140–200 compressed (ticks every 20 = 14.12°). Tick labels 0–140 full size; 160/180/200 smaller + dimmer (`#49545f`). Gold arc fill from 0 to current speed; white needle (r=62 of 88 viewBox units); hub readout: speed (60px) + "MPH".
- **Tachometer**: 0–8000, linear, labels 0–8 (×1000); "8" in muted red. Static red-zone arc segment **7600→8000** (`#5c1616`). Arc fill gold, turns red >7600; hub: RPM value (38px, colored by state) + "RPM".
- **Warning telltale dots** (centered row under gauges, 26px gaps): FUEL (amber, <2.5 gal) · OIL (red, oil press <29 psi OR oil temp >248°F) · ECT (red, >217°F) · VOLTS (red, <12.0V). Dead-front: off = near-invisible dark dot `#161d25` + label `#39434d`; on = filled + glow + label colored. 9px dot + 9px label.
- **Odometer** (bottom center): "ODOMETER" (10px, .24em, `#454f59`) + value (20px, `#8b97a3`) + "MI".

### Gauge geometry (shared by all sweep gauges)
- SVG viewBox coordinates: center (120, 80), arc radius 88, stroke 9–10, round caps.
- Sweep: **240°**, from 210° (min, lower-left) clockwise to −30° (max, lower-right). Angle for fraction *f*: `a = 210° − f·240°`; point = `(120 + r·cos a, 80 − r·sin a)`.
- Ticks: r 80→88. Tick labels at r 66. Needle: from hub to r 62, white 3px, round cap, subtle glow. Hub: 7px radius disc `#0c1117` with `#39434d` ring.
- Value arc: same path as background arc, drawn with stroke-dash technique (arc length = 2π·88·(240/360) ≈ 368.6 units). On EVE, render as an arc/polyline sweep.
- Background arc track: `#141b23`.

### CENTER 7″ — ALARM TAKEOVER (both modes)
Full-screen overlay, **flashes** between bright red radial (`#d92020 → #700c0c`) and near-black red `#260606` at ~2.8 Hz. Content centered: "WARNING" (14px, .6em tracking, `#ffd9d9`) → alarm title (48px Chakra Petch 700) → live value (116px Saira 800) → limit line (15px, .28em, `#ffd9d9`).
| Alarm | Trigger | Limit text |
|---|---|---|
| OIL PRESSURE | < 29 psi (2.0 bar) | MINIMUM 29 PSI |
| OIL TEMP | > 248°F (120°C) | MAXIMUM 248°F |
| COOLANT TEMP | > 217°F (103°C) | MAXIMUM 217°F |
Priority: oil pressure > oil temp > coolant. Alarm should override BOTH modes and persist while condition holds.

### LEFT 5″ — ENGINE (Phase 2)
Header: "ENGINE" / "COYOTE CAN · 5″".
- **TRACK**: 4×2 hairline-divided grid (1px `#151b22` lattice on `#080b0f` cells). Top row (pit-now): OIL P (psi) · ECT (°F) · FUEL P (psi) · AFR L. Bottom row (trend): OIL T (°F) · IAT (°F) · VOLTS · AFR R. Cell: label (10px `#5f6b76`) top-left, value (31px Saira 600) + unit (10px) bottom. Warn/alert colors per thresholds below. Below grid: "PMU16 ×2 · OUTPUTS" strip — PUMP / FAN 1 / FAN 2 chips (green text+border when on with amps, gray "OFF", red FAULT state fills chip) + total amps at right.
- **STREET**: 2×2 mini sweep gauges (132×118): OIL P (0–100 psi) · ECT (100–260°F) · OIL T (100–300°F) · VOLTS (8–16V). Gold arc, red when out of range; hub value + label.

### RIGHT 5″ — TIMING / ROAD (Phase 2)
Header: "TIMING" (track) / "ROAD" (street), "RACECAPTURE · 5″".
- **TRACK**: LAP number (42px) + POS (gold "P3" 30px) row; LAST / BEST (purple) / PRED lap times row (20px, hairline-framed); THROTTLE (green) and BRAKE (red) percent bars (7px pills); bottom hairline grid: FUEL gal · LAPS remaining · AMB °F; odometer footer row.
- **STREET**: FUEL sweep gauge (190×170, 0–16 gal, "E"/"F" end labels, gold arc, amber <2.5 gal) with hub gal readout; bottom row: TRIP A · RANGE · AMB · TIME (17px values). No odometer here (it lives on center screen in street).

## Interactions & Behavior
- **Mode switch**: CAN input selects TRACK/STREET. All three screens switch together. Debounce and animate-free (instant swap is fine).
- **Shift-light flash**: >7600 RPM, all LEDs toggle red at ~8 Hz. RPM bar/arc/readout turn red at the same threshold; amber pre-warning at 7100.
- **Alarm takeover**: see above; flash ~2.8 Hz. Must preempt everything on the center screen.
- **Delta bar**: green fills left of center when ahead of best, red fills right when behind; ±1.0 s full scale.
- **Needles/arcs**: update at telemetry rate (aim ≥ 30 fps on EVE; the mock uses ~16 Hz ticks with light smoothing).
- **Odometer/trip**: integrate GPS or wheel speed; persist odometer in EEPROM/FRAM.

## Vehicle Constants (for scaling & derived values)
- Redline **8000 RPM**, shift light **7600 RPM**
- T56 Magnum close-ratio: 2.66 / 1.78 / 1.30 / 1.00 / 0.80 / 0.63; final drive **3.73**; tire dia **26″**
- MPH = (RPM × 26) / (ratio × 3.73 × 336) → gear tops @8000: 63 / 93 / 128 / 166 / 208 / 264
- Speedo scale 0–200 MPH (non-linear knee at 140)
- Fuel tank modeled at 16 gal usable (E/F gauge scale); range estimate = gal × 16 mi/gal (street), laps-remaining = usable ÷ per-lap burn (track)

## Warning thresholds (all panels)
| Channel | Amber | Red |
|---|---|---|
| Oil pressure | — | < 29 psi |
| ECT | > 210°F | > 217°F |
| Oil temp | > 235°F | > 248°F |
| Fuel pressure | — | < 43 psi |
| Volts | — | < 12.0 |
| IAT | > 131°F | — |
| AFR (per bank) | > 13.8 (lean under load) | — |
| Fuel level | < 2.5 gal | — |

## State Management (firmware sketch)
- `mode` (TRACK/STREET) ← CAN input
- Live channels: rpm, gpsSpeed, ect, oilT, oilP, fuelP, iat, batt, afrL, afrR, fuelLevel, throttle, brake, lap/lastLap/bestLap/predLap/position (RaceCapture), pmu output states+amps
- Derived: alarmActive(+which), shiftLightCount/flash, deltaSeconds, rangeMi, lapsRemaining, odometer/trip (persisted)
- Missing-data behavior: show `--` and never render a stale alarm.

## Assets
- No raster assets. All graphics are vector/procedural (arcs, pills, LEDs). Fonts: Saira Condensed + Chakra Petch (Google Fonts, OFL) — convert to EVE fonts.
- Physical bezel provides turn-signal LEDs (and future telltale LEDs: high beam, brake, CEL, charge, low fuel) — not rendered on screens.

## Files
- `Eve Race Dash.dc.html` — the interactive design reference (open in a browser; requires `support.js` alongside). Tweaks panel: mode, units, accent, alarm test, manual RPM/MPH.
- `support.js` — runtime for the HTML prototype (not part of the design).
- `renderings/01-track-mode.png`, `renderings/02-street-mode.png` — final renders, gold accent.
