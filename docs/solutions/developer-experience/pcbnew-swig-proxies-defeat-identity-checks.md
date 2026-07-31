---
title: pcbnew SWIG proxies defeat identity checks — `t is v` never matches across GetTracks() calls
date: 2026-07-30
category: developer-experience
module: kicad-scripting
problem_type: developer_experience
component: tooling
severity: medium
applies_when:
  - "A pcbnew script excludes an item from a collection with `is` or `id()`"
  - "A constraint or collision search inexplicably rejects every candidate"
  - "Any code that holds a board item across two GetTracks()/Footprints()/Pads() enumerations"
tags: [kicad, pcbnew, swig, python, identity, proxies, scripting]
---

# pcbnew SWIG proxies defeat identity checks — `t is v` never matches across GetTracks() calls

Every call to `board.GetTracks()` (and the other pcbnew collection accessors)
wraps the underlying C++ objects in **fresh Python proxy objects**. Two proxies
for the same via are different Python objects, so `t is v` is always false and
`id(t) == id(v)` is meaningless. An identity-based exclusion silently excludes
nothing.

## Context

During Board3's via respacing (PR kms254/Mustang-Dash-Test#15), a placement
search validated candidate positions against every via on the board, excluding
the via being moved with the obvious idiom:

```python
v = ...  # found in an earlier GetTracks() loop
for t in b.GetTracks():
    if t is v:          # never true -- t is a fresh proxy
        continue
    constraints.append(t)
```

The mover entered its own constraint list, so every candidate position within
the search radius violated the hole-to-hole rule **against the via itself**.
The search returned "no feasible spot" for three separate vias, twice, before
the cause surfaced. What made it hard to see: the failure is total but looks
spatial — "this pocket is just too tight" is a perfectly plausible verdict on a
dense board, so the broken exclusion masqueraded as a legitimate geometric
result.

The diagnosis that cracked it was a rejection census: tally *which* object
rejected each candidate. Every rejection traced to a via on the mover's own
net at distance ≈ search-radius — i.e., the mover itself.

## Guidance

Exclude and compare board items by **stable value**, not Python identity:

```python
# exclusion by coordinates -- the mover's position is unique on the board
for t in b.GetTracks():
    if t.GetClass() == "PCB_VIA":
        p = t.GetPosition()
        if abs(p.x - mover_pos.x) < 1000 and abs(p.y - mover_pos.y) < 1000:
            continue    # the mover, matched by value (nm units)
```

Any stable attribute works — position, or a captured unique property compared
by value. The rule: the moment an item reference crosses from one enumeration
to another, only value comparisons are trustworthy.

Corollaries, same mechanism:

- A "have I seen this item" set keyed on proxy objects or `id()` will treat
  every re-enumeration as all-new items.
- Holding a proxy from enumeration A and mutating it *does* affect the real
  object (the proxy forwards to the same C++ item) — mutation works across
  proxies; only identity does not. That asymmetry is why the bug survives
  spot-checks: the script visibly moves the right via while invisibly failing
  to exclude it.

When a constraint search rejects 100% of candidates, run a rejection census
before concluding the space is infeasible: print which item rejected each
candidate. A single item rejecting everything — especially one sharing the
mover's net — is the signature of a broken exclusion, not a tight pocket.

## Why This Matters

The failure mode returns a *plausible wrong answer*, not an error. "No legal
position exists" leads to escalating workarounds — moving neighbors, relaxing
rules, redesigning the pocket — all solving a problem that does not exist. On
Board3 it cost two full placement searches and nearly triggered an unnecessary
neighbor-shuffle before the census exposed it.

## When to Apply

- Any pcbnew script that excludes "the item being edited" from a collision,
  clearance, or constraint scan
- Deduplication or visited-set logic over board items
- Reviewing pcbnew code: treat `is`, `id()`, and object-keyed sets on board
  items as bugs unless the proxies provably come from the same enumeration

## Examples

The same search, before and after switching the exclusion to coordinates:

```text
identity exclusion : NO SPOT for /USB_DM_CONN, /SCLK_R_MCU, /GND  (all 128 candidates rejected)
value exclusion    : /USB_DM_CONN placed at r=0.08, /GND at r=0.25 -- same pockets
```

## Related

- [Call BuildConnectivity() before counting airwires](build-connectivity-before-counting-airwires.md) — same session, same theme: pcbnew measurements that silently describe the wrong board
- CLAUDE.md "pcbnew scripting lessons" — the SWIG graveyard rule (keep removed items' proxies alive until SaveBoard) and one-board-per-process are the other two members of this proxy-lifetime family
