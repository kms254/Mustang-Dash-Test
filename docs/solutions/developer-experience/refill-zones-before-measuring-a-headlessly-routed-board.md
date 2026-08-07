---
title: Refill zones before measuring a headlessly routed board, and fill them under the real rules
date: 2026-07-27
category: developer-experience
module: kicad-routing
problem_type: developer_experience
component: tooling
severity: high
applies_when:
  - "Routing a KiCad board from a CLI or script rather than through the GUI"
  - "A routed board reports hundreds of zero-clearance violations against copper pours"
  - "Refilling zones on a board whose design rules do not live in its own .kicad_pro"
  - "A copper edit produces thermal-relief violations you cannot account for"
  - "Comparing autorouter completion or DRC results between tools or runs"
root_cause: missing_workflow_step
resolution_type: workflow_improvement
tags: [kicad, pcb, routing, drc, zones, clearance, design-rules, headless, measurement]
---

# Refill zones before measuring a headlessly routed board

A routed board that has not been refilled is not in a measurable state — and a
refill performed under the wrong clearance is a copper edit you did not intend.
Both mistakes are silent, and both produce violations that look like the change
under test caused them.

## Context

Board3 was routed headlessly by a CLI autorouter, then checked with
`kicad-cli pcb drc`. The result: **974 violations**, dominated by 512 clearance
errors of which 488 reported `actual 0.0000 mm` — copper apparently touching
copper on different nets, which would mean hundreds of shorts.

The real number was **74**.

The board had `GND`, `+3V3` and `+5V` copper pours that were deliberately
preserved through the strip. The router laid tracks and vias across them and
nothing refilled the pours afterward, so KiCad compared new copper against a
zone outline that had never been recomputed. Every crossing became a
zero-clearance violation.

The KiCad GUI refills zones as part of ordinary editing, so this failure mode is
invisible until routing moves to a script.

**Refilling turned out to be only half the instruction.** A zone fill is
clearance-dependent, so its result depends on which design rules were loaded
when it ran — and KiCad loads those from the board's sibling `.kicad_pro`.
Board3's tracked project file carried the EasyEDA Pro importer's factory
defaults: netclass `Default` clearance **0.2 mm** against the board's real
**0.1016 mm**. A refill under it pulled every pour back an extra 0.0984 mm,
moved thermal spokes, and invented two `starved_thermal` violations that were
indistinguishable from damage caused by the copper edit being tested.

The trap is well hidden, because the obvious place to look says nothing is
wrong. The importer wrote `board.design_settings.rules.min_clearance` as `0.0`;
the 0.2 mm that governs the fill sits in `net_settings.classes`, a different
block.

## Guidance

**Refill zones as part of the routing step, not as a follow-up.** The two are
inseparable. Wrapping the router is the reliable place for this — a wrapper that
routes and then returns without refilling hands the caller a board whose DRC
output is meaningless, and nothing about the output says so.

**Fill against a staged project carrying the real rules, and never write the
tracked project file.** This is trap 1 in `tools/kicad_handroute.py`;
`refill_under_real_rules()` is the reference implementation:

```python
rules = json.loads(RULES.read_text(encoding="utf-8"))   # tools/kicad_rules.json
with tempfile.TemporaryDirectory() as tmp:
    staged = Path(tmp) / board_path.name
    shutil.copy2(board_path, staged)

    # Start from the real project so the stackup and layer names survive,
    # then override only the rule keys.
    project = json.loads(board_path.with_suffix(".kicad_pro").read_text())
    project["board"]["design_settings"]["rules"].update(rules["rules_mm"])
    for netclass in project["net_settings"]["classes"]:
        if netclass["name"] == "Default":
            netclass.update(rules["default_netclass_mm"])
    staged.with_suffix(".kicad_pro").write_text(json.dumps(project, indent=2))

    board = pcbnew.LoadBoard(str(staged))     # loaded under the staged rules
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    pcbnew.SaveBoard(str(board_path), board)  # written back to the real board
```

Three properties make it correct. The project is a *copy* of the real one with
rule keys overridden, so nothing else about the board's setup is invented. The
board is loaded from the staged path so the staged project is the sibling KiCad
actually reads. The result is saved back to the real board path, so the copper
moves and the project file does not. `tools/kicad_verify.py` stages the same way
in `_rules_project()` before every DRC run.

**Check what a zero-clearance violation is actually between before believing
it.** `actual 0.0000 mm` between two *nets* is a short. Between a track and a
zone it is almost always a stale fill:

```python
zero = [v for v in violations
        if v["type"] == "clearance" and "actual 0.0000" in v["description"]]
kinds = collections.Counter(
    tuple(sorted(i["description"].split(" [")[0] for i in v["items"]))
    for v in zero)
# {('Track', 'Zone'): 230, ('Via', 'Zone'): 258}  -> stale fill, not shorts
```

That one census turned a board that looked catastrophically broken into one
with 37 real new violations.

### The project file was later synced, and the rule did not change

Commit `763aff9` wrote the real rules into
`kicad/board3/ProPrj_New Project_2026-07-15_23-14-34_2026-07-27.kicad_pro`, so a
GUI refill no longer silently changes copper. That is a convenience, not a
transfer of authority. Restate the rule precisely:

> **Design rules come from `tools/kicad_rules.json`, never from the
> `.kicad_pro`. The project file is a synced copy of it, not a second source.**

**That claim has since been falsified in one direction, and the failure is worth
more than the rule.** On 2026-08-02, `d74143a` raised five DRC guard rails to
JLC's published limits — including `min_hole_clearance` from 0.1016 to 0.2 —
by editing the `.kicad_pro` and the `.kicad_dru`, and never touching
`tools/kicad_rules.json`. Since the verifier does
`rules.update(spec["rules_mm"])` on a staged copy, the staged run *overwrites*
the raised value with the stale one. For that key the project file was current
and the rules file was stale: the copy outranked its own source.

So the sync is directional in intent and bidirectional in practice. The rule
survives — one file must be authoritative — but it needs its enforcement clause:
**a change to a design rule is not done until it appears in
`tools/kicad_rules.json`**, because that is the only copy the gate reads.

Nothing enforces the sync. KiCad rewrites
the project file on ordinary GUI activity: commit `1941588` exists only because
opening Board Setup materialised three zero-valued placeholder rows in the
pre-defined size lists. A GUI session is exactly when the rules could quietly go
back, which is why every tool still stages rather than trusting, and why that
commit re-checked `min_clearance` 0.1016, `min_via_annular_width` 0.15,
`min_hole_to_hole` 0.5 and the `Default` netclass at 0.1016 / 0.254 rather than
assuming they had survived.

## Why This Matters

The first error is not small and it is not random — it inflated the result by
more than an order of magnitude, in the direction of "this tool produces
unusable output". Had it gone unchecked it would have been the headline finding
of a tooling evaluation, and the conclusion drawn from it would have been the
opposite of the truth.

The second error is smaller and worse. Two invented `starved_thermal`
violations do not look like a broken measurement; they look like a plausible
consequence of the edit you just made to nearby copper. An order-of-magnitude
error invites suspicion. A two-violation error gets attributed and fixed, and
the fix is applied to a defect that does not exist.

Both are silent in both directions. DRC does not warn that zones are stale, or
that they were filled under different rules from the ones it is now grading
against. The router does not warn that it left them either way. Nothing in the
pipeline reports a problem, and the number it produces looks precise.

## When to Apply

- Any KiCad routing, cleanup, or copper-modifying operation driven from a script
  or CLI rather than the GUI
- Before running DRC on any board whose copper changed programmatically
- Whenever a fill runs on a board whose authoritative rules live outside its
  `.kicad_pro` — the fill must be given those rules explicitly
- When a copper edit produces thermal-relief or clearance violations near, but
  not on, the thing you changed: refill under the real rules and re-measure
  before diagnosing
- Before comparing autorouter results across tools or runs — an unrefilled board,
  or one filled under different rules, invalidates the comparison entirely

## Examples

Board3, same routed board, measured twice:

```text
before refill   974 violations   (clearance 512, hole_clearance 203, track_width >=199)
after refill     74 violations   (courtyards 25, starved_thermal 23, edge 11, via geom 15)
```

`track_width` lands on exactly 199, which is `kicad-cli`'s per-error-code report
limit — so that class was probably clipped and the 974 is a lower bound. Its two
neighbours exceed 199, and the limit is *not* uniform across error codes, which
makes coincidence less likely rather than more. The argument here is unaffected:
the point is that those classes vanished after the refill, not their magnitudes.
See [a count at the report limit is not a measurement](a-count-at-the-report-limit-is-not-a-measurement.md).

The 488 phantom violations were `Track + Zone` (230) and `Via + Zone` (258).
None were shorts. The `clearance`, `hole_clearance` and `track_width` classes
vanished entirely — they had all been artifacts of comparing new copper against
an outline computed before that copper existed.

The rules the importer substituted, and what the board was actually drawn to
(`tools/kicad_rules.json`, JLCPCB 4-layer standard):

```text
                       imported     real
netclass clearance      0.2        0.1016   <- governs the zone fill
netclass track_width    0.2        0.254
min_clearance           0.0        0.1016
min_track_width         0.2        0.1016
min_via_diameter        0.5        0.45
min_via_annular_width   0.1        0.15
min_hole_to_hole        0.25       0.5
```

Measured against the imported column an untouched Board3 reports 544
violations, 503 of them clearance, all sitting in a 0.117–0.197 mm band just
under a 0.2 mm rule the board was never drawn to. Against the real column the
same board reported 41 at the time — which became the baseline every later DRC
delta was taken from.

**That baseline is retired.** Board3 now measures 0 violations and 0 unconnected
under the same real rules, and both CI design gates went absolute on 2026-08-05:
the ratchet, its two baseline checkouts and the picker were deleted, because a
floor that has been cleared makes a ratchet pass while asserting nothing. The
544-vs-41 contrast above is still the right illustration of *why* the rules file
exists — it is the measurement that proved the imported rules were fiction — but
41 is a historical figure, not a bar anything is judged against.

## Related

- [Migrating a board from EasyEDA Pro to KiCad loses data silently](../integration-issues/easyeda-pro-to-kicad-migration-silent-data-loss.md) — why the rules had to be reconstructed into `tools/kicad_rules.json` in the first place
- [Search every copper layer before placing a via](search-every-copper-layer-before-placing-a-via.md) — the same staged-rules requirement, applied to via placement instead of zone fills
- [A gate that cannot pass gets waved through](../conventions/a-gate-that-cannot-pass-gets-waved-through.md) — what else goes wrong when the rule file and the checker disagree

**The rest of the family.** This doc was the first of a set that has since grown
to eight, and several of them cite it while it cited none of them. All are the
same shape — *the measurement was not measuring what you thought* — and each
names a different way to be fooled:

- [Call BuildConnectivity() before counting airwires](build-connectivity-before-counting-airwires.md) — the in-memory version of this doc's own trap, and one this doc's reference implementation does not yet apply
- [Headless DRC judges the board plus its sidecar files](stage-project-sidecars-for-headless-drc.md) — the same staging requirement for the *library* tables rather than the rules
- [An airwire count cannot validate copper changes on a poured net](airwire-counts-cannot-validate-deleting-copper-on-a-poured-net.md) — a correct number answering a different question
- [Window-filter board geometry by shape intersection](clip-test-board-window-queries.md) — the obstacle map itself being incomplete
- [pcbnew SWIG proxies defeat identity checks](pcbnew-swig-proxies-defeat-identity-checks.md) — the object you compared is not the object you meant
