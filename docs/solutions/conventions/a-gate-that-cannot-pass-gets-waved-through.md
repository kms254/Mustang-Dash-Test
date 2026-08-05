---
title: "A gate that cannot pass gets waved through"
date: 2026-07-27
category: conventions
module: kicad-verification
problem_type: convention
component: tooling
severity: high
applies_when:
  - "Setting a pass/fail bar on an inherited artifact that already violates it"
  - "Encoding a vendor's or platform's limits into an automated check"
  - "A check has been red for so long that nobody reads its output"
  - "Comparing a changed artifact against a baseline to attribute regressions"
tags:
  - gating
  - ci
  - kicad
  - drc
  - baseline
  - measurement
  - false-positive
---

# A gate that cannot pass gets waved through

## Context

Two separate defects in `tools/kicad_verify.py` and `tools/kicad_rules.json`
turned out to be the same defect.

**The unreachable absolute.** Board3 arrived from EasyEDA Pro carrying **41 DRC
violations** — courtyard overlaps and edge clearances that predate any routing
work and that no routing work removes. Measuring a routed board's raw total
against zero attributes all 41 to the router. `kicad_verify.py`'s docstring
records the consequence: it "is how a 33-violation result gets published as 74."

**The floor that was not a floor.** `tools/kicad_rules.json` carries
`"source": "https://jlcpcb.com/capabilities/pcb-capabilities"` at the top of the
file, and `rules_mm` sits directly under that stamp. It held
`min_through_hole_diameter: 0.3`. JLC's published drill range is **0.15–6.3 mm**.
0.3 mm is a price break, not a limit. The board carries exactly one drill below
it — the `U1.93` BTN1 via, deliberately shrunk to `0.5/0.2` because its pad
neighbours cap the diameter at 0.517 mm and 0.5/0.2 gives exactly the 0.15 mm
annular ring JLC does require. The rule duly failed the one via that had been
sized correctly.

Both produce a check that a correct board cannot pass. Commit `763aff9` fixed
both the same way.

## Guidance

**Apply the provenance test to every number in a capability block. If it is not
on the vendor's capability page, it is not a capability floor.** The test is
falsifiable and takes one lookup. It caught 0.3 mm immediately — the page says
0.15 mm, so the extra 0.15 mm came from somewhere else, and "somewhere else" was
a surcharge. A number whose provenance you cannot name is a preference wearing a
limit's clothes.

**Sort every rule into three kinds. The kind sets both the severity and the
cadence:**

| Kind | Example | Severity | Cadence |
|---|---|---|---|
| Capability — the fab physically cannot | drill < 0.15 mm, ring < 0.15 mm | fail the build | every change |
| Cost or preference — possible, priced or disliked | drill < 0.3 mm | advisory, never the exit code, with a recorded exception | every change |
| Property of the world — true of reality, not of the change | LCSC stock, part lifecycle | report, never gate | scheduled |

The third row is the one people misfile. `.github/workflows/kicad-stock.yml`
runs on `cron: "0 7 * * *"` with no `pull_request` trigger, and says why in its
own header: *"Stock is a property of the world rather than of the change under
review, so failing someone's PR because a capacitor sold out overnight punishes
the wrong person at the wrong moment."* A PR gate on part stock fails authors
for events they did not cause and cannot fix, which is the fastest possible
route to a check nobody reads.

**A demoted rule must stay visible and must carry a recorded reason.** Demotion
is not deletion. `cost_floors_mm` states the contract in the file itself: *"DRC
must not fail on these or the gate stops meaning anything ... Anything listed
here needs a recorded reason, the way U1.93's 0.2 mm drill has one."* The
advisory prints on every run above the pass/fail lines, so the 0.2 mm drill
stays a deliberate, re-examined choice rather than an unnoticed one.

**Measure both sides of a baseline through the same staged rules, or the delta
is arithmetic on incomparable numbers.** `kicad_verify.py` calls the same
`run_drc()` — which calls `_rules_project()` — for the board and for the
baseline. Rules come from `tools/kicad_rules.json`, never from the board's own
`.kicad_pro`: the EasyEDA importer writes KiCad's stock defaults there, and an
untouched Board3 measured against those reports **544 violations, 503 of them
clearance**, every one a false positive. A baseline measured against the
importer's defaults and a candidate measured against real rules produce a delta
that describes the rules, not the change.

**Gate the per-type delta, not the total.** A total that fell can conceal a
violation class that did not exist before. The comparison is per key:

```python
new = {k: counts[k] - base_counts.get(k, 0) for k in counts
       if counts[k] > base_counts.get(k, 0)}
...
return EXIT_NEW_VIOLATIONS if new else EXIT_CLEAN
```

Board3 today is **36 against a baseline of 41**. On the total alone that is
5 better and would pass while hiding any new class beneath the slack. Per type
it is `NEW = 0`, which is a real claim. The `carried over` line prints the
baseline classes that did not grow, so demotion never becomes concealment.

## Why This Matters

The failure mode is social, not technical. A red check that nobody can turn
green stops carrying information within about two runs: the first reader
investigates, the second learns the colour is constant, and from then on the
gate is scrolled past. It is now strictly worse than no gate, because it
occupies the slot where a working one would go and it makes the pipeline look
guarded.

Both incidents here were expensive in attention before they were caught. The
importer-default rules survived four implementation units, over which every DRC
run measured annular rings against a 0.1 mm default the board was never designed
to — and a 0.10 mm ring introduced during routing went unreported the entire
time.

**That instance is now closed, and how it closed is the point.** This doc
previously recorded a live one: `.github/workflows/kicad-drc-erc.yml` ran
`kicad-cli pcb drc` with `--exit-code-violations` and no baseline, enforcing a
zero-violation bar on a board whose measured floor was 36 — unreachable by
construction. It was not fixed by relaxing the bar. The raw `kicad-cli` call was
demoted to an informational report that writes `drc.json` and gates nothing, and
the gate moved to `tools/kicad_verify.py --baseline … --contract count`, which
fails on a per-type increase from the floor.

The choice is per check, not per pipeline, and the deciding question is whether
zero is a state the artifact could actually occupy.

**On this repo the answer eventually became "yes" everywhere, and the ratchets
were retired.** For a period the workflow ran both kinds side by side — ratcheted
DRC and ERC deltas against a baseline, beside absolute gates for schematic parity
and library loading — which is still the shape to copy when a floor is genuinely
real. But the floor here was not permanent: ERC went 1009 → 0 and DRC to 0 at all
severities under the project's own staged rules, so both deltas became absolute
and the baseline checkouts they fed were deleted as dead weight. The durable
lesson is the middle step, not the end state: **a ratchet is justified by a claim
about the artifact, and claims expire.** Re-measure the floor before renewing the
ratchet, or you will still be gating against a defect somebody already fixed.

**A resolved instance, in warning form.** The thesis applies to standing
warnings as well as red gates: U2's `lib_footprint_mismatch` sat at "1" through
an entire pre-fab campaign, waved through as "pre-existing/cosmetic" — while
two new divergence families accumulated invisibly under the unchanging count,
because a per-footprint warning is a boolean that cannot register additions.
It cleared only when someone produced the field-level diff instead of accepting
the label; see
[lib_footprint_mismatch is a real diff](../integration-issues/kicad-lib-footprint-mismatch-integer-nanometre-comparison.md).

## When to Apply

- Before adding any pass/fail check to an artifact you inherited rather than
  authored — measure its floor first, then gate the delta from that floor.
- When transcribing a vendor's, platform's, or standard's limits into a config:
  run the provenance test on each number one at a time.
- When a check has been red for more than one review cycle. That is the symptom;
  treat it as a bar that needs moving, not as work that needs finishing.
- When a rule you want to keep is not a hard limit — demote it to an advisory
  with a recorded exception rather than deleting it or leaving it failing.

## Examples

The split, in `tools/kicad_rules.json`. The capability block carries only what
the vendor page states; the price break lives elsewhere:

```json
"rules_mm": {
  "min_clearance": 0.1016,
  "min_via_diameter": 0.45,
  "min_through_hole_diameter": 0.2,
  "min_via_annular_width": 0.15
},
"cost_floors_mm": {
  "$comment": "Not capability limits -- price breaks. DRC must not fail on these
   or the gate stops meaning anything ...",
  "through_hole_diameter": 0.3
}
```

The advisory that replaced the failure, `tools/kicad_verify.py:81`. Its
docstring names the mechanism directly:

```python
def cost_floor_advisories(board: Path) -> list:
    """Report geometry that is manufacturable but priced above the cheap tier.

    These must never fail the gate -- a gate that is permanently red gets waved
    through, which is how the review harness's calibration rotted. ...
    """
```

It returns a list of strings that `main()` prints as `advisory :` lines. It is
not consulted by any return statement. That is the whole point: the exit code is
`EXIT_CLEAN`, `EXIT_ERROR`, or `EXIT_NEW_VIOLATIONS`, and a cost floor cannot
reach any of them.

## Related

- [A colon in one symbol name makes the entire library unloadable](../integration-issues/kicad-colon-in-symbol-name-makes-library-unloadable.md) — the inverse polarity of this doc's thesis: a gate that is permanently *green* and structurally unable to report the class it was meant to catch, because the finding was a warning and the gate filtered to errors. Also the absolute gate that closed this doc's live instance
- [A large ERC count is a broken instrument](../developer-experience/a-large-erc-count-is-a-broken-instrument.md) — **this doc's deciding question applied to the ERC gate, answering the other way.** That gate was ratcheted on the premise that an imported schematic carries warning noise no edit can clear; all 1009 violations were then cleared without one net moving, so zero *is* a state this artifact can occupy. The gate has since been rewritten absolute at `--severity-all`, and the DRC gate followed for the same reason once its floor was re-measured at 0. Worth carrying: a ratchet justified by inherited noise is a claim about the artifact, and claims expire — re-test the premise before renewing the ratchet
- [Refill zones before measuring a headlessly routed board](../developer-experience/refill-zones-before-measuring-a-headlessly-routed-board.md) — the other way a DRC number arrives precise and wrong
- [Calibrate an automated reviewer on a confirmed defect](../design-patterns/calibrate-an-automated-reviewer-on-a-confirmed-defect.md) — the calibration rot this doc's advisory docstring refers to
- [Migrating a board from EasyEDA Pro to KiCad loses data silently](../integration-issues/easyeda-pro-to-kicad-migration-silent-data-loss.md) — where the 41-violation baseline comes from
