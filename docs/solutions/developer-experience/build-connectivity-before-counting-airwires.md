---
title: Call BuildConnectivity() before counting airwires — GetConnectivity() after a refill lies in both directions
date: 2026-07-30
category: developer-experience
module: kicad-scripting
problem_type: developer_experience
component: tooling
severity: high
applies_when:
  - "A pcbnew script gates a copper edit on GetUnconnectedCount() after a zone refill"
  - "A guarded removal loop reports load-bearing verdicts that make no physical sense"
  - "A board saved by a guarded script turns out to have airwires the guard said were zero"
  - "Any trial-mutation pattern: edit, refill, count, accept-or-revert"
tags: [kicad, pcbnew, connectivity, airwires, zone-refill, guard, scripting]
---

# Call BuildConnectivity() before counting airwires — GetConnectivity() after a refill lies in both directions

`board.GetConnectivity()` returns the existing connectivity object, and after a
`ZONE_FILLER(...).Fill(...)` that object can still describe the pre-fill board.
`RecalculateRatsnest()` on a stale object does not rescue it. A guard built on
that count fails silently in both directions: it accepts edits that break the
net, and it vetoes edits that are legal. `board.BuildConnectivity()` between the
fill and the count is what makes the number mean something.

## Context

Board3's warning-inbox cleanup (PR kms254/Mustang-Dash-Test#15) removed dangling
copper with a trial-removal guard: remove the item, refill zones, count
airwires, keep the removal only if the count stayed zero. The guard read:

```python
b.Remove(item)
filler.Fill(b.Zones())
conn = b.GetConnectivity()
conn.RecalculateRatsnest()
if conn.GetUnconnectedCount(True) == 0:   # stale -- measured the wrong board
    ...
```

Both failure directions occurred in one session:

- **False accept.** A tail-trim was accepted at "airwires 0" and saved. The
  saved board actually had one unconnected item — the trim had orphaned a
  telltale LED's +5V anode together with its daisy-chain neighbor. The orphan
  surfaced only later, as an inexplicable DRC `unconnected_items` entry, and
  needed a diagnosis session plus a repair bridge.
- **False block.** With that one real airwire baked into the board, the `== 0`
  guard then rejected *every* subsequent legal removal — eight dangles in a row
  reported "load-bearing" although each removal was harmless. The loop looked
  finished ("nothing more can be removed safely") while eight fixable warnings
  remained.

The second failure is the insidious one: a guard that always fails looks like a
conservative guard doing its job, not like a broken measurement.

## Guidance

Rebuild connectivity explicitly after every fill, before every count:

```python
b.Remove(item)
filler.Fill(b.Zones())
b.BuildConnectivity()                     # <- the load-bearing line
conn = b.GetConnectivity()
conn.RecalculateRatsnest()
if conn.GetUnconnectedCount(True) == 0:
    keep()
else:
    revert()
```

Apply it uniformly — the accept path, the revert path, and any baseline
measurement taken at script start. A baseline captured without the rebuild can
disagree with every later measurement taken with it.

### Evidence status, re-measured 2026-08-05: could not reproduce

Two things were found on that date and they point in opposite directions, so
both are recorded rather than one being allowed to settle it.

**The rule was not being followed anywhere.** A repo-wide search returned **zero**
calls to `BuildConnectivity()` — in `kicad_handroute`, `kicad_freeroute`,
`kicad_route`, `kicad_shove` or `kicad_strip`, all of which count airwires and
one of which gates every board edit. The rule had lived in `CLAUDE.md` and in
this doc for six days without reaching a single line of code. That gap is now
closed.

**And the failure would not reproduce.** Two attempts on Board3 with KiCad
10.0.5, both measuring with and without the call:

- fresh board per measurement, 0 / 5 / 25 / 100 `/GND` tracks removed — identical
  counts in all four;
- the documented pattern faithfully: **one** board object, a 40-iteration
  trial-removal loop of remove → fill → count — **zero disagreements**, sequences
  identical element for element.

So on this board and this KiCad, the call changes nothing measurable. It was
added anyway: the incidents above were real, cost a diagnosis session and a
repair bridge, and the call is free. But **do not cite it as load-bearing
without re-measuring** — and if you are debugging a wrong airwire count, this is
now a weak suspect rather than a strong one. Either KiCad's behaviour changed
between the incident and now, or the original diagnosis attributed the two
failures to the wrong cause. That question is open.

The first attempt above is worth its own note: it reloaded the board before each
measurement, which by construction cannot show accumulated staleness, and it
returned a clean "no difference" that looked like a result. The reproduction
only became meaningful once it matched the *loop* the incidents actually
happened in.

Two supporting rules from the same session:

- **Measure the baseline first.** An absolute `== 0` guard assumes the board
  starts at zero. If a prior bug left a real airwire behind, the guard blocks
  everything and reports it as physics. When a whole class of edits starts
  failing the guard, re-measure the board's own baseline before believing any
  verdict.
- **The guard's verdicts are only as good as its refresh discipline.** The
  same trial-mutation loop had run dozens of correct accepts before the one
  false accept — staleness is intermittent, which is exactly why it survives
  casual testing.

## Why This Matters

The false accept writes a broken board to disk while reporting success — on
this board it disconnected a telltale LED's supply, which would have shipped as
a dead lamp had DRC not been re-run afterward. The false block wastes the
opposite way: legal work is refused, loops "converge" early, and the operator
concludes the remaining defects are unfixable. Neither direction produces an
error message. One `BuildConnectivity()` call per iteration is the entire cost
of eliminating both.

## When to Apply

- Every pcbnew script that counts airwires after modifying copper or fills —
  guards, gates, verifiers, baseline measurements
- Any accept-or-revert trial loop over board mutations
- When a previously reliable connectivity guard starts rejecting everything:
  suspect a baked-in airwire from an earlier stale read, and re-measure the
  board's baseline with a rebuilt connectivity object

## Examples

Same board, same edit queue, one line different:

```text
without BuildConnectivity(): round reports 8/8 dangles "load-bearing", 0 removed
with    BuildConnectivity(): same 8 dangles all remove cleanly, airwires 0
```

## Related

- [Refill zones before measuring a headlessly routed board](refill-zones-before-measuring-a-headlessly-routed-board.md) — the sibling staleness: copper recomputed but never refilled, and fills run under the wrong rules. This doc adds the third layer: the connectivity object itself.
- [pcbnew SWIG proxies defeat identity checks](pcbnew-swig-proxies-defeat-identity-checks.md) — found in the same session; the other way a pcbnew script silently measures the wrong thing
