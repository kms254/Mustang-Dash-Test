---
title: "An airwire count cannot validate deleting copper on a poured net — the pour absorbs the evidence"
date: 2026-08-02
category: developer-experience
module: kicad/board3
problem_type: developer_experience
component: tooling
severity: high
applies_when:
  - "Deciding whether a piece of copper on a poured net (GND, a power plane) is redundant"
  - "Any trial-removal loop that gates on GetUnconnectedCount() or the ratsnest"
  - "Cleaning up dangling stubs, duplicate taps, or 'obviously covered' copper"
  - "Reviewing a copper deletion whose only stated evidence is 'airwires still zero'"
tags: [kicad, pcbnew, zones, pour, airwires, ratsnest, drc, starved-thermal, ground]
---

# An airwire count cannot validate deleting copper on a poured net — the pour absorbs the evidence

The ratsnest answers **"is this pad connected?"** A copper deletion asks
**"was that copper doing something?"** On an un-poured net those questions have
the same answer. On a poured net they do not: the pour steps in and carries the
connection the moment you delete what was carrying it. The airwire count stays
at zero and confirms nothing.

## Context

While closing out H2 (the Tag-Connect debug header, U49), a short `/GND` run
into `C6`'s ground pad looked like redundant copper — `C6.2` sits inside the
`/GND` pours on Top, Bottom and Inner1, so a dedicated tap appeared to duplicate
what three planes already did. It was removed, zones were refilled, and airwires
were counted: **still zero.** By the usual gate, the removal was safe.

It was not. That run was `C6`'s ground leg. With it gone the pad's only
remaining connection was the pour itself, through thermal relief — and the
`starved_thermal` DRC check, not the connectivity engine, is what reported it.
The copper was restored.

Note the ordering that makes this dangerous: **the airwire count was not wrong.**
`C6.2` genuinely was still connected. Zero was the truthful answer to the
question the ratsnest asks. It simply was not the question being asked.

## Guidance

**On a poured net, treat a zero airwire count as no evidence at all about a
deletion.** It cannot distinguish these three outcomes, and reports zero for two
of them:

| What actually happened | Airwires | Acceptable? |
|---|---|---|
| The copper really was redundant; the pour already carried it | 0 | yes |
| The copper *was* the connection; the pour silently took over | 0 | **no** — degraded to thermal relief |
| The copper was the connection and the pour does not reach | ≥1 | caught |

Only the third case is visible to the ratsnest, and it is the least likely to
occur — a pour big enough to look redundant is a pour big enough to absorb the
loss.

What to use instead, strongest first:

1. **Run a fill-aware DRC and read the whole census, not just `unconnected_items`.**
   `starved_thermal` is the check that catches the substitution, because it is
   the one that knows the difference between "attached" and "attached well
   enough". Use all severities so warnings are not filtered away:

   ```bash
   kicad-cli pcb drc BOARD.kicad_pcb --format json --severity-all -o report.json
   ```

2. **Ask what the pour is now carrying.** If a deletion moves a pad from a
   dedicated tap to thermal-relief spokes, that is a real electrical change —
   higher resistance, less current capacity, different reflow behaviour — even
   when it is DRC-legal. Decoupling capacitors are the worst place to accept it:
   a low-impedance path is the entire point of the part.

3. **Diff per-net geometry, so a deletion is defended by what it changed rather
   than by what it did not break.** `python tools/kicad_measure.py BOARD --against BEFORE`
   prints every net whose segment count, via count, length or layer set moved.

## Why This Matters

This failure mode is self-concealing in a way most are not. A guard that throws
false negatives gets caught by its own noise; this one returns the *correct*
number and lets a wrong conclusion be drawn from it. There is no error to
notice, no anomaly to investigate, and the copper is already gone.

It also targets ground specifically — the net most likely to be poured, most
likely to look redundant, and least likely to be re-examined, because "it's all
one plane anyway" is the intuition a pour is built on. That intuition is correct
about connectivity and wrong about current, impedance and thermal relief.

This board has already paid once for trusting a connectivity number over a
copper question: a stale-connectivity guard orphaned `C30` from a telltale's
`+5V` daisy chain while reporting "airwires 0" (see Related). That was a
*broken* measurement. This is a *correct* measurement answering a different
question — which is exactly why fixing the first did not prevent the second, and
why the two docs are companions rather than duplicates.

## When to Apply

- Any deletion of copper on `GND`, a power plane, or any net carrying a zone.
- Any automated dangle-cleanup or trial-removal loop over a poured board.
- Reviewing a diff justified by "airwires unchanged" — that sentence is
  load-bearing only for un-poured nets.
- Especially near **decoupling capacitor ground pads**, where the pour is
  densest and the electrical cost of substitution is highest.

## Examples

The reasoning that produced the bad deletion, and the reasoning that catches it:

```text
BAD:   C6.2 sits inside three GND pours, so this tap is duplicate copper.
       delete -> refill -> airwires 0 -> accept.
       (true premise, true measurement, wrong conclusion)

GOOD:  delete -> refill -> BuildConnectivity() -> airwires 0
       -> DRC --severity-all -> starved_thermal on C6.2 -> revert.
       The pour took over; "connected" had quietly become "connected
       through relief".
```

## Related

- [Call BuildConnectivity() before counting airwires](build-connectivity-before-counting-airwires.md)
  — the sibling, and a necessary companion rather than a substitute. That doc
  makes the airwire count *correct*; this one says a correct count still cannot
  answer the deletion question on a poured net. Apply both: rebuild
  connectivity, then decline to rely on the number anyway.
- [Refill zones before measuring a headlessly routed board](refill-zones-before-measuring-a-headlessly-routed-board.md)
  — the first layer of the same staleness family: measuring copper whose pours
  were never recomputed.
- [Stage project sidecars for headless DRC](stage-project-sidecars-for-headless-drc.md)
  — a DRC census is only usable if the run is not swamped by phantom library
  errors; required to make step 1 above trustworthy.
- `tools/kicad_handroute.py` — refills zones under the real rules after every
  edit, which is what makes a post-edit DRC meaningful in the first place.
