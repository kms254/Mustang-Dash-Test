---
title: "Read the running device before theorising from the code"
date: 2026-07-25
category: developer-experience
module: dash-serial
problem_type: developer_experience
component: development_workflow
severity: medium
applies_when:
  - "a bench report names a wrong-looking value on a live device that exposes a status query"
  - "code reading has produced two or more competing hypotheses and none has been tested"
  - "a reported number is out of range for what the code can produce"
  - "diagnosing on a board that recently changed MCU or USB identity"
symptoms:
  - "A reported value has no counterpart anywhere in the running state"
  - "The obvious code-derived explanation is unfalsifiable without touching the device"
tags:
  - diagnosis
  - serial-protocol
  - bench-workflow
  - dash-serial
  - observability
---

# Read the running device before theorising from the code

## Context

A bench report said the third panel showed "lap count 18 while I'm only 4.9 miles in". One HPR lap is 2.55 miles, so 4.9 miles is under two laps — and a 20-minute session caps the counter near 10 before `dash_sim_session_reset` zeroes it, so 18 was **out of range for anything the simulator could author**.

That looked like strong evidence, and it produced two confident hypotheses from code reading alone: a sticky serial override left on the `lapn` channel (plausible — `set lapn` exists and overrides are sticky), or a session reset failing to fire. Both were wrong.

One `status` read settled it in seconds: `lapn=2`, `odo=79.6`, `sim=on`, no overrides held. The lap counter was correct and always had been. The "18" was the **LAPS** cell — fuel range, `fuel_gal / DASH_LAP_BURN_GAL` — sitting one label away from a live lap *number* on the same screen, and it had tracked the tank faithfully the whole time (18 → 9 → 8 → 7 as fuel drained).

## Guidance

**When the system exposes a live status surface, read it before building a theory.** The order matters because code reading generates hypotheses cheaply and eliminates them expensively; a status read does the opposite.

1. **Sample twice, seconds apart.** One frame tells you the value; two tell you whether it is moving, frozen, or overridden — which is what actually discriminates between hypotheses.
2. **Suspect the label before the arithmetic.** A number that is out of range for the code often is not that number at all. Check which field the reporter is reading before assuming the value is wrong. Here the fix was renaming `LAPS` to `LAPS LEFT`, not repairing a counter.
3. **Re-confirm the device identity each session.** The `/dash` skill documents COM4 for the Teensy; the F767 bring-up board enumerates as an ST-Link Virtual COM Port on a different port entirely (COM6 on this bench). `[System.IO.Ports.SerialPort]::getportnames()` plus a `Win32_PnPEntity` name query resolves it in one step — cheaper than debugging a "dead" board that was only ever on the wrong port.
4. **Let the read kill your favourite hypothesis.** Both hypotheses here were specific, mechanism-backed, and wrong. That is the normal outcome, and it is the reason to read first.

## Why This Matters

Reasoning from code recovers what the program *can* do. A status read reports what it *is* doing, including state no source file records — which override is held, which session is running, what the odometer has accumulated across reboots. For a bug report about a running system, the second question is the one being asked.

The cost asymmetry is stark on this bench: the status read was one command with an immediate answer, while the sticky-override theory would have meant reading the serial protocol, the ownership bitmask, and the publish path — arriving at a mechanism that was real, present in the code, and not what was happening.

## When to Apply

- Any live-device or long-running-process report where a status or introspection query exists (`status`, a health endpoint, a metrics scrape, a debug dump).
- Before opening the source on any "impossible value" report.
- Less useful when the surface itself is suspect — if the reporting path is what you are debugging, its output is evidence about the reporter, not the system.

## Examples

The one-line read that ended the investigation, and the two fields that mattered:

```
ok status mode=track fps=59 ... lapn=2 ... fuel=5.88965 ... odo=79.6 trip=79.6 eve=ok,ok,ok
```

`lapn=2` killed both hypotheses at once; `fuel=5.88965` identified the real source of "18" (`5.89 / 0.59 ≈ 9` at that moment, having been 18 a third of a tank earlier). A second sample minutes later showed `fuel=4.64932` with the cell reading 8 — confirming it tracked the tank, not laps.

## Related

- [Two consumers of the same physics disagreeing in one frame](../logic-errors/pedal-display-derived-from-a-flag-while-the-model-had-the-value.md) — the real defect on the same panel, found by comparing two status fields
- PR #8 (merged 2026-07-25)
