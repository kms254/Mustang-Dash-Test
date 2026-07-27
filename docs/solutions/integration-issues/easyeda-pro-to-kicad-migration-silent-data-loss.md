---
title: Migrating a board from EasyEDA Pro to KiCad loses data silently at four separate points
date: 2026-07-27
category: integration-issues
module: easyeda-workflow
problem_type: integration_issue
component: tooling
severity: high
symptoms:
  - "A freshly imported board reports hundreds of DRC clearance violations, all clustered just under a round number"
  - "Footprints exist on the imported board with zero pads, so components have geometry but no electrical existence"
  - "A KiCad importer terminates the process with 0xC0000409 instead of rejecting the file, and prints nothing"
  - "PCB_IO_MGR.Load succeeds and returns a board with 0 footprints and 0 tracks"
root_cause: wrong_api
resolution_type: workflow_improvement
tags: [easyeda, kicad, migration, import, design-rules, pcb, data-loss]
---

# Migrating a board from EasyEDA Pro to KiCad loses data silently at four separate points

## Problem

Getting Board3 (144 footprints, 107 nets, 4 layers) out of EasyEDA Pro v3 and
into KiCad 10.0.5 cost most of a session and six rejected formats. Every failure
mode was silent: no route announced that it had dropped something. Three of the
four produced a board that opened, looked correct, and was wrong.

## Symptoms

- **544 DRC violations on an untouched import**, 503 of them clearance, every
  actual between 0.117 mm and 0.197 mm — a suspiciously tight band just under
  0.2 mm. The same board measured against its real rules reports 41.
- **11 of 140 footprints with zero pads** after a PADS ASCII conversion: `DC1`
  (the barrel jack), `SW1`-`SW4`, and the `C1`/`C2`/`C12`/`C13`/`C14`/`C16`
  electrolytics. All 536 SMD pads survived; only through-hole was lost.
- **Hard crashes**, exit `0xC0000409` (stack buffer overrun), from
  `PCB_IO_MGR.Load` on wrong input — and because the crash takes stdout's buffer
  with it, the visible symptom is a hang with no output, not an error.
- **A silent empty board**: `EASYEDAPRO` accepted a `.eprj2` without complaint
  and returned 0 footprints, 0 tracks, 1 net.

## What Didn't Work

Six routes, in the order tried:

| Route | Result |
|---|---|
| `.eprj2` via `EASYEDA` (plugin 9) | hard crash |
| `.eprj2` via `EASYEDAPRO` (plugin 10) | silent empty board |
| Altium Designer export | hard crash — EasyEDA emits Protel ASCII, KiCad's Altium importer expects binary OLE/CFB (`D0 CF 11 E0`) |
| ODB++ export | hard crash on the archive, on its extracted job root, and on that root's parent |
| `.epro2` autosave backup | rejected, "Unable to find a valid schematic file". It is a zip, but holds an opaque `.epru` payload, not export structure |
| PADS ASCII export | **worked, and dropped every through-hole pad** |

The `.eprj2` is a SQLite database, not an archive. Its `boards`, `schematics`,
`components` and `documents` tables are all empty; the design lives in
`history_data.dataStr` as opaque encoded blobs. There is no direct-read path.

## Solution

**The working route is `File → Save as → "epro (V2 format)"`** in EasyEDA Pro,
then KiCad's `File → Import Non-KiCad Project → EasyEDA (JLCEDA) Pro Project`.

It is not in the Export menu. That menu offers only fabrication outputs (Gerber,
CPL, BOM, ODB++, IPC-D-356A) and foreign-EDA formats (Altium, PADS, DXF). The
format KiCad needs lives under **Save as**, as a *legacy V2* option, because
KiCad cannot read EasyEDA's current `epro2`/V3. The route is therefore to
deliberately save backwards one format version. Nothing signposts this.

That route is materially better than the PADS one:

| | PADS ASCII | epro V2 project |
|---|---|---|
| footprints | 140 | **144** |
| zero-pad footprints | **11** | **0** |
| nets | 94 | **107** |
| schematic | none | **1.15 MB `.kicad_sch`** |

**But the rules still do not come across.** The importer writes KiCad's stock
factory netclass — `clearance 0.2`, `track_width 0.2`, `via_diameter 0.6`,
`via_drill 0.3` — regardless of what the source design used. Those exact four
values are the tell. Apply the real rules before doing anything else; this repo
keeps them in [tools/kicad_rules.json](../../../tools/kicad_rules.json).

## Why This Works

Each failure is a different kind of translation gap, which is why no single
check catches them all:

- **Format-version mismatch.** KiCad implements the V2 `epro` reader. EasyEDA v3
  writes V3 (`epro2`) natively and only offers V2 as a legacy save.
- **Encoding mismatch.** EasyEDA's "Altium" export is Protel *ASCII*; KiCad's
  Altium importer parses the *binary* compound-document format. Feeding text to
  a binary struct parser is what produces the stack overrun rather than an error.
- **Feature gap in a lossy interchange.** PADS ASCII carried all 536 SMD pads and
  no through-hole pads. The format is not the design; it is a projection of it.
- **Rules are not geometry.** An importer's job is understood as translating
  objects. Design rules are project metadata, so they fall outside what it
  translates — and writing defaults is indistinguishable from translating badly.

The 0.117–0.197 mm band is the diagnostic signature of the last one. A board with
genuinely sloppy clearances shows a spread; a board drawn to a *looser* rule and
measured against a stricter one clusters immediately below the stricter value.

## Prevention

**Census pads per footprint after any conversion, never just count footprints.**
A footprint with zero pads is never legitimate:

```python
zero = [fp.GetReference() for fp in board.GetFootprints() if not len(list(fp.Pads()))]
assert not zero, f"conversion dropped pads on: {zero}"
```

The 11 missing components surfaced only because someone asked what the barrel
jack's designator was. A total-footprint check passes happily on a board whose
power input has no electrical existence.

**Treat imported design rules as absent until proven otherwise.** If a fresh
import shows hundreds of clearance violations clustered just below a round
number, suspect the rules before the board. Compare the netclass against KiCad's
factory defaults; if they match exactly, they were never translated.

**Pin one plugin id and validate the container before calling `Load`.** Never
iterate plugins looking for one that works — each wrong guess can take the
process down:

```python
head = path.open("rb").read(16)
if not head.startswith(b"!PADS-POWERPCB"):      # or D0CF11E0 for real Altium
    raise ValueError("refusing to Load: container is not the expected format")
board = pcbnew.PCB_IO_MGR.Load(pcbnew.PCB_IO_MGR.PADS, str(path))   # id pinned
if not board.GetFootprints() and not board.GetTracks():
    raise ValueError("empty board: conversion failed")               # never success
```

**Run every KiCad probe unbuffered (`python -u`).** A crashing importer discards
buffered stdout, so a run that printed six diagnostic lines before dying appears
to have printed nothing and reads as a hang. Unbuffered output is what turns
"it froze" into "it died on plugin 9".

**Point the project importer at an empty directory.** It treats its destination
as its own, and deleted a committed, unrelated `board3.kicad_pcb` that happened
to be sitting there.

## Related

- [Verifying EasyEDA design state by reading the .eprj2 project file](../developer-experience/easyeda-eprj2-agent-verification.md) — the same SQLite file, read for verification rather than migration
- [Authoring a full schematic through the EasyEDA MCP bridge](../developer-experience/easyeda-bridge-schematic-authoring-workflow.md) — the bridge's own limits, which motivated evaluating KiCad in the first place
