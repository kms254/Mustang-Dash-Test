---
title: Migrating a board from EasyEDA Pro to KiCad loses data silently at six separate points
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
  - "A gerber package contains no file a fab would recognise as the board outline, because the outline shipped under another layer's name"
  - "A rotation-correction plugin matches an imported footprint by name and rotates a part that was already correct"
root_cause: wrong_api
resolution_type: workflow_improvement
tags: [easyeda, kicad, migration, import, design-rules, pcb, data-loss, gerbers, layer-names, rotation]
---

# Migrating a board from EasyEDA Pro to KiCad loses data silently at six separate points

## Problem

Getting Board3 (144 footprints, 107 nets, 4 layers) out of EasyEDA Pro v3 and
into KiCad 10.0.5 cost most of a session and six rejected formats. Every failure
mode was silent: no route announced that it had dropped something. Three of
those four produced a board that opened, looked correct, and was wrong.

Two further losses surfaced later the same day, when the imported board was
turned into a fab package — long after the work had stopped looking like an
import problem.

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
- **A board outline exported as `…-Multi-Layer.gbr`** — a filename that, in the
  vocabulary of the tool it came from, describes the opposite of an outline.
- **Footprints named `LQFP-144_…`, `SOIC-8_…`, `SOT-23-6_…` that are not drawn
  to the rotation datum those names imply.** Nothing on the board says so.

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

## Two More, Found When the Board Became a Fab Package

Neither of the last two is a dropped object, which is why neither showed up in a
pad census or a DRC run. In both, **a name survived the import while the meaning
behind it did not** — worse than a deletion, because nothing looks missing.

**The importer renames every layer, and one rename inverts its meaning.** KiCad's
canonical names are replaced with EasyEDA's in the `(layers …)` block of
[the board file](../../../kicad/board3/ProPrj_New%20Project_2026-07-15_23-14-34_2026-07-27.kicad_pcb),
here for the eleven layers a 4-layer order exports:

| KiCad canonical | name after import |
|---|---|
| `F.Cu` | Top Layer |
| `In1.Cu` | Inner1 |
| `In2.Cu` | Inner2 |
| `B.Cu` | Bottom Layer |
| `F.SilkS` | Top Silkscreen Layer |
| `B.SilkS` | Bottom Silkscreen Layer |
| `F.Mask` | Top Solder Mask Layer |
| `B.Mask` | Bottom Solder Mask Layer |
| `F.Paste` | Top Paste Mask Layer |
| `B.Paste` | Bottom Paste Mask Layer |
| **`Edge.Cuts`** | **Multi-Layer** |

Most are merely verbose, and the rest of the stackup follows the same pattern
(`F.Fab` → "Component Marking Layer", `Eco2.User` → "Mechanical Layer",
`User.1` → "Ratline Layer"). `Edge.Cuts` → `Multi-Layer` is different. KiCad
names each exported gerber after the layer's **user** name, so the board outline
ships as `…-Multi-Layer.gbr`. In Altium and EasyEDA vocabulary "multi-layer"
describes copper present on every layer — a pad property, not an outline. A fab
reading that filename learns the opposite of the truth, and the package looks
like it has no outline at all.

Fixed in `ead50be`. `canonicalise_gerber_names()` in
[tools/kicad_fab.py](../../../tools/kicad_fab.py) loads the board through
`pcbnew`, compares `GetLayerName(lid)` with `pcbnew.LayerName(lid)` for every
enabled layer, and renames each `…-<user name>.gbr` to the canonical name with
dots as underscores — so the outline becomes `…-Edge_Cuts.gbr`. The canonical name
is `pcbnew`'s, not the board file's token: `F.SilkS` in the table above comes out
as `F_Silkscreen`. `check_gerber_layers()` then reports any of the eleven required
names missing from the folder.

**Renaming the derived output is safe in a way renaming the board's layers is
not**, because layer names are referenced throughout the board file.

The rename needs KiCad's own interpreter. Without `pcbnew` importable the
function prints a note and leaves the EasyEDA names alone, so a package built
under stock Python still ships `-Multi-Layer.gbr`.

**Imported footprints are drawn to JLCPCB's rotation zero, not KiCad's.**
Measured 2026-07-27 by de-rotating pad 1 into each footprint's own zero:

| part | package | EasyEDA pin 1 | KiCad stock pin 1 | plugin correction | correct here |
|---|---|---|---|---|---|
| `U1` | LQFP-144 | south-west | north-west | +270 | **0** |
| `U8` | SOIC-8 | south-west | north-west | +270 | **0** |
| `D9` | SOT-23-6 | south-west | north-west | −90 | **0** |

Three package families, two different plugin constants, all resolving to zero
correction. EasyEDA/LCSC footprints are already drawn to JLC's own datum, which
is what you would hope from the ecosystem that assembles the board.

**The trap is that the name survived the datum change.** The `kicad-jlcpcb-tools`
correction table is calibrated against KiCad's *official* library and matches on
footprint **name** — and these are still named `LQFP-144_…`, `SOIC-8_…`,
`SOT-23-6_…`. Running its fabrication export against this project matches all
three rules and rotates a quarter turn that nothing needs: `U1` the STM32H755ZIT6,
`U8` and `U9` the TJA1051 CAN transceivers, `U7` the FRAM, and four SOT-23-6
parts. The tool reached for to prevent backwards parts is the one that would
produce them. The derivation is kept in `tools/kicad_fab.py`'s module docstring,
where anyone about to "fix" the uncorrected rotation will read it first.

`USBC1` cannot be compared this way at all: the two libraries number that
receptacle's pads differently (`A1` versus numeric), so there is no shared datum
and the comparison returns a meaningless 73°.

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

**Never let a derived artifact inherit an imported name.** Gerbers are named
after layers and netlists after nets, so every renamed object propagates into a
filename a stranger at a fab house has to interpret. Canonicalise on the way out;
do not rename the board's own layers to make the output right.

**Assume any correction table keyed on a name does not apply to imported parts.**
A name-matched rule encodes a claim about how the part was *drawn*, and an import
changes the drawing while preserving the name. Measure the datum before applying
the rule — for footprints that means locating pad 1 relative to the body centre
in the footprint's own zero, and comparing it against the library the rule was
calibrated on. Zero correction is a legitimate answer, and reaching it takes the
same measurement as any other.

## Related

- [Verifying EasyEDA design state by reading the .eprj2 project file](../developer-experience/easyeda-eprj2-agent-verification.md) — the same SQLite file, read for verification rather than migration
- [Authoring a full schematic through the EasyEDA MCP bridge](../developer-experience/easyeda-bridge-schematic-authoring-workflow.md) — the bridge's own limits, which motivated evaluating KiCad in the first place
- ["DRC clean and measured" is not "assemblable"](../conventions/drc-clean-and-measured-is-not-assemblable.md) — the fab-package pass that surfaced the last two losses, long after the board verified clean
