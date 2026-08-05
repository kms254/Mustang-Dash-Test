---
title: "A large ERC count is a broken instrument, not a property of the design"
date: 2026-08-04
category: developer-experience
module: kicad-erc
problem_type: developer_experience
component: tooling
severity: high
applies_when:
  - "An inherited or imported design carries a violation count large enough that nobody reads the individual findings"
  - "Deciding whether a checker's output is inherent noise or a signal nobody has ever opened"
  - "Driving a four-figure ERC, lint, or analyzer count to zero without being allowed to change behaviour"
  - "Computing coordinates for a scripted schematic edit that the checker already reports"
  - "A quantity read out of a tool's report is physically absurd by two or three orders of magnitude"
symptoms:
  - "ERC reports 1009 violations while the netlist, BOM, DRC and host tests are all clean"
  - "A stable four-figure count is explained away as import noise and never opened"
  - "124 warnings saying an entire symbol library will not load sit unread inside the total for months"
  - "Wire lengths read out of the ERC report look like 15-25 micrometre fragments on a 2.54 mm grid"
root_cause: incomplete_setup
resolution_type: workflow_improvement
related_components:
  - development_workflow
tags: [kicad, erc, netlist-baseline, pin-types, pwr-flag, easyeda-import, board3, verification]
---

# A large ERC count is a broken instrument, not a property of the design

Board3's schematic reported **1009 ERC violations**. That number had been
treated as a property of the design — an imported board carries noise — and was
therefore never actually read. Inside it, 124 warnings were saying an entire
symbol library was unloadable, and they sat there for months.

It is now **0**, verified in place with kicad-cli 10.0.5 at `--severity-all`:

```
$ cd kicad/board3
$ kicad-cli sch erc --format json --severity-all --exit-code-violations \
    -o /tmp/erc.json "ProPrj_New Project_2026-07-15_23-14-34_2026-07-27.kicad_sch"
Found 0 violations
```

The report goes outside the project directory on purpose: `erc.json` is not
gitignored, so writing it beside the schematic leaves an untracked file in the
tree you are trying to prove is clean.

**Not one net moved to get there.** Exporting the netlist from the schematic as
it stood before the campaign and from the schematic as it stands now — both in
place — gives 228 nets on each side, no net present on one side only, and zero
membership changes on any shared net.

## Context

The count was a self-sealing excuse. Nobody reads 1009, so nobody discovered
that 124 of the items were `lib_symbol_issues` reporting that
`ProPrj_New-easyedapro.kicad_sym` would not load at all — a real, months-old
defect with a one-character cause (see
[the colon-in-symbol-name doc](../integration-issues/kicad-colon-in-symbol-name-makes-library-unloadable.md),
which owns that story). Clearing those took the report 1131 → 1009. This doc is
about all 1009 that remained, and more usefully about the method that removed
them.

The report's size was not the only thing hiding it. `.github/workflows/kicad-drc-erc.yml`
ran its ERC gate at `--severity-error` and compared per type against a base, so
a **warning** class could not fail the build no matter how large it grew. The
workflow's own comment records this: *"ERC did say so — 124 lib_symbol_issues —
but that check is a WARNING and the ERC gate below ran --severity-error at the
time. It was invisible by construction."* Two independent mechanisms had to fail
together: a report too big to read, and a gate scoped so it could not read that
part of it either. Both are now closed — see the gate rewrite under *Why This
Matters*.

## Guidance

### 1. Treat a large count as a broken instrument

The goal is not a smaller number. The goal is a report where **a new warning is
visible**. Until then every check you run is reporting into a void, and the cost
is paid the day something real appears in it. Budget the cleanup as instrument
repair, not as design work — because on a healthy design that is exactly what it
is.

### 2. Make the netlist the gate, at every step

Almost every ERC fix is metadata: pin electrical types, no-connect flags, label
positions, grid snapping. None of it should be able to change connectivity — so
prove it rather than assume it. Export `kicad-cli sch export netlist` before the
first edit and after every step, and compare **net membership** (net name →
`{(ref, pin)}` set), not bytes.

This is what makes the work safe enough to do in bulk. Two steps were expected to
add nodes — no-connect flags and PWR_FLAGs — so for those the permitted diff was
declared in advance, in the script itself: *"the netlist gains an
unconnected-(...) entry per flagged pin. That is the intended and only permitted
difference — any change to a real net means revert."*

**In the event neither step changed the netlist at all**, which is why the
headline diff above is exactly zero rather than "zero apart from the expected
additions." The `unconnected-` pseudo-nets were already there — 135 before the
flags and 135 after, because the netlist names an unconnected pin whether or not
a flag asserts the intent — and PWR_FLAG symbols do not reach the netlist at all.
Declaring an allowance you turn out not to need is the cheap direction to be
wrong in; the reverse would have hidden a real change inside an expected one.

### 3. ERC's JSON is authoritative about position — do not recompute it

Every violation item carries a `uuid` that maps exactly to the object in the
file, and a `pos`. Use both. Reconstructing pin world-coordinates independently
(rotating library pin offsets into place through the symbol's `(at x y rot)`)
was tried and, per this session's measurement, disagreed with ERC on about 2% of
endpoints — enough to put a flag on the wrong pin, where it either does nothing
or, worse, marks a pin that *is* connected.

**The `pos` is in mm/100, and the report's own header does not say so.** The JSON
declares `"coordinate_units": "mm"` while the values are hundredths of a
millimetre:

```json
{ "description": "Symbol R60 Pin 1 [1, Input, Line]",
  "pos": { "x": 0.5492, "y": 3.72 },
  "uuid": "dace5284-5350-4435-96bf-124a13550743" }
```

R60 pin 1 is at **54.92, 372.0 mm**. The consumer code just multiplies:

```python
xy = (round(p["x"] * 100, 3), round(p["y"] * 100, 3))
```

This is worth stating loudly because getting it wrong produces a conclusion that
*sounds* like a discovery. Read at face value, the wires in this schematic came
out as tens-of-micrometre fragments — ERC prints `length 0.0152 mm` and
`length 0.0254 mm` — which would mean a corrupt file, and briefly did get
reported that way. They are ordinary wires: 467 of them, median length exactly
2.54 mm, and 458 of the 467 exactly 2.54.

The tell that settles it costs nothing: take two adjacent pins on the same part
and check the pitch **along the axis they are stacked on**. U1 pins 9 and 10 have
identical `x`, and their `y` values `1.0668` and `1.0922` differ by `0.0254` —
one 2.54 mm pitch, exactly 100× the reported delta. (Check the wrong axis and you
get a delta of 0, which looks like a different bug.) When a quantity is absurd by
orders of magnitude, suspect your units before the data.

### 4. When a computed answer is uncertain, ask ERC

Ten of the dangling wires could not be resolved by computing which end was free.
Rather than guess: move the label to one end, re-run ERC, and whatever is still
flagged had the *other* end free. It converged in two passes. A label sitting on
a pin is electrically identical to a label sitting on the wire that reaches it,
**so a wrong pass costs nothing** — which is what makes the iteration safe rather
than merely clever. Prefer a cheap two-pass oracle over an expensive one-pass
derivation whenever the wrong answer is free to undo.

### 5. Indentation is not an anchor in KiCad files

This campaign produced five separate wrong answers from whitespace-anchored
regexes. The EasyEDA export indents with tabs, `kicad_lcsc.py` with two spaces,
and a cache entry copied from one into the other lands at `"\t\t  (symbol"` —
two tabs *and* two spaces. The three most instructive:

| Pattern | What it did |
| --- | --- |
| `^\t\(symbol` | Reported a whole library as empty |
| `\(pin \w+ \w+\s*\n` | Dropped 9 symbols — both AW9523B expanders and every LED — because the importer writes the pin on one line and the exporter does not |
| `\n\t\(symbol\n` | Matched nothing at all in a CRLF file read with `newline=""` |

Match **structure**: find the opening token, walk to the balanced close, test
depth. Every script in this campaign carried the same `block_end()` helper and
an indentation-agnostic anchor:

```python
pat = r'\r?\n[ \t]+\(symbol "'      # any leading whitespace
```

Related and sharper: a bare `\(xy ...\)` pattern **also matches the polyline
graphics inside symbol definitions**, whose coordinates are symbol-local.
Editing those corrupts the artwork of every part on the sheet. Scope edits to
top-level `(wire ...)` blocks explicitly.

### 6. Run ERC in place — never on a copy

`kicad-cli sch erc` resolves the library tables through the `.kicad_pro` **whose
name matches the schematic's**. A schematic staged out as
`_erc_baseline.kicad_sch` loads no project at all and invents phantom
violations. Copying into the project *directory* is not enough; the sidecar has
to match by name. To baseline an edit, put the HEAD version back at the real
path, measure, then restore. (Sibling trap for the board side:
[headless DRC judges the board plus its sidecar files](stage-project-sidecars-for-headless-drc.md).)

A related subtlety, found while producing the numbers in this doc: **an ERC count
is a function of the schematic *and* its libraries.** Rolling the schematic back
while leaving corrected libraries in place gives a baseline that is close but not
equal to the historical one. State which of the two you rolled back, or the
comparison is not the one you think it is.

## Why This Matters

**Noise is not free, it is deferred.** The 124 unloadable-library warnings were
correct, specific, and present in every run for months. Nothing about them was
hard except being seen. The one-time cost of getting a report to zero buys every
future run the ability to mean something.

**"Unconnected" and "meant to be unconnected" are different claims.** 100 of the
violations were pins with no connection and no flag saying that was intended —
60 spare STM32H755 GPIO, 27 unused panel-connector pins, 12 tact-switch second
poles, one barrel-jack sleeve. A no-connect flag is the difference between a
schematic somebody checked and one that merely has not been, and it is what makes
a future *genuinely* missing connection visible instead of hiding among a hundred
others. 35 flags already existed, so somebody had started this and stopped.

**Some findings are latent defects even when nothing is wrong today.** Two
classes here were fragility rather than fault. The 16 both-ends-dangling stubs
were ghosts of deleted two-pin parts — including where an earlier unit removed
the CAN common-mode capacitors — and actively misled: a reader seeing `CAN1_CT`
and `GND` stubs side by side would reasonably think something connects there. The
11 off-grid endpoints connected correctly by exact coordinate coincidence, so any
later drag or snap would silently break them — and for six of the eight symbols
the thing that breaks is the AW9523B reset network that must hold ≥335 µs.

**A ratcheted gate cannot retire a debt; it can only stop it growing.** The ERC
gate was `--severity-error` with a per-type delta, justified by "a known ERC debt
(unflagged unused pins)". That rationale — an imported schematic carries warning
noise no edit can clear — was believed when it was written and is now falsified:
every one of the 1009 cleared. Reaching zero is what made the gate rewritable,
because **zero is the only count a gate can assert without arguing about a
baseline.**

So the gate was rewritten as part of this work: absolute, and at
`--severity-all`. The severity is the load-bearing half. At `--severity-error`
it could not have reported the one genuine defect in the pile no matter how bad
that got — `lib_symbol_issues` is a warning class. Verified in both directions
before landing: 0 violations at `--severity-all` on the current schematic, and a
synthetic report containing a warning-severity `lib_symbol_issues` fails the step.

**Then the same question was put to the DRC gate, and it gave the same answer.**
That gate was also a ratchet, on the same shape of premise — the import left 41
error-severity violations, measured at a floor of 36. Measured now through the
project's real staged rules, the board reports **0 violations and 0 unconnected
at `--severity-all`**. So that ratchet went too, and with it the two baseline
checkouts and the baseline picker that fed both gates: three CI steps that
cloned the repo twice a run to compute a comparison nothing consumed any more.

Getting there needed one prerequisite worth naming, because it is why the gate
had been errors-only and defensibly so. The verifier staged the board with its
rules but *not* the library tables, and without those an all-severities run is
swamped by phantom "library not found" findings that also **mask** the real
mismatches underneath. Staging `fp-lib-table` and the `.pretty` dirs is what
makes `--severity-all` mean anything. A severity filter covering for a staging
gap is a reasonable local decision that quietly becomes a blind spot — fix the
staging, then drop the filter, in that order.

## When to Apply

- Before dismissing any checker count as "import noise" — read a sample of each
  class first; class *shape* tells you more than the total.
- When adding a CI gate on a checker: decide explicitly which severities it
  reads. A gate scoped to errors makes every warning class permanently invisible.
- Whenever you are about to compute an object's coordinates that the tool already
  reports. Use the tool's answer, and sanity-check its units against a known
  pitch before trusting a single number.
- Whenever a bulk edit must be provably non-functional — pick the artifact that
  *defines* the function (here, the netlist) and diff it at every step.

## Examples

### The 1009, by class — measured, not recalled

Re-running ERC over the pre-campaign schematic in place gives:

```
  469  unconnected_wire_endpoint
  372  pin_to_pin
  100  pin_not_connected
   55  pin_not_driven
   11  endpoint_off_grid
    1  lib_symbol_mismatch          (2 at the time — see note)
  severity: {'error': 155, 'warning': 853}
```

Note on the reconciliation: this measurement pairs the **old schematic** with
**current libraries**, and one of the two original `lib_symbol_mismatch` items
was text drift on the library side that has since been corrected. 1007 + 2 =
1009.

**`power_pin_not_driven` is deliberately absent from that list.** Those six were
not part of the 1009 — they *appeared* once the pin types were correct, because
only then could ERC tell that `/+5V`, `/+3V3`, `/GND`, `/VDDA`, `/CH224_VDD` and
`/CH224_VSNS` carry `power_in` pins and no recognised source. That is true rather
than a defect: `/+5V` arrives through the ideal-diode FETs and `/+3V3` leaves the
buck through L1, both passive by construction, and `/GND` has no source at all.
PWR_FLAG is the assertion that the rail *is* driven. **Expect a correct fix to
surface new classes; check the shape of the rise, not the number.**

### Fixing each class

| Class | n | What it was | Fix |
| --- | ---: | --- | --- |
| `unconnected_wire_endpoint` | 469 | One idiom, no exceptions: every net label sat at its wire's **midpoint**, so the far end touched nothing | Move each label onto the free end (same wire, so the net cannot change) |
| `pin_to_pin` | 372 | The import typed every pin `unspecified` or `input`; ERC cannot reason about a connection when neither end declares what it is | Retype from the pin's own name and the part's datasheet role |
| `pin_not_connected` | 100 | Genuinely unused pins, unflagged | `no_connect` at coordinates taken from ERC |
| `pin_not_driven` | 55 | — | Fell out for free with the pin types |
| `endpoint_off_grid` | 11 | 8 symbol pins + 3 wire ends. Seven of the symbols sit at typed round coordinates (60, 95, 130, 170 × 372, 388), none a multiple of 1.27 mm; the eighth (LED2) is on grid in x and off in y | Snap symbol and everything sharing a pin coordinate by the same delta |
| `power_pin_not_driven` | 6 | Emergent; rails passive by construction | PWR_FLAG on six rails |

Independent corroboration of the pin-typing step, from the schematic's own
`lib_symbols` cache:

```
BASELINE : {'unspecified': 353, 'input': 46}                         total 399
CURRENT  : {'passive': 156, 'bidirectional': 126, 'power_in': 53,
            'input': 27, 'unspecified': 32, 'output': 3,
            'open_collector': 2, 'power_out': 1}                     total 400
```

Two electrical types became eight. The `+1` is PWR_FLAG's single `power_out`
pin — the mechanism by which the six rails became driven. The 32 pins still
`unspecified` are symbols the rule table does not cover: the classifier returns
`None` for those and they are left alone, which is the right default.

### The rule table is the deliverable, not the script

The pin-typing rules are entirely name-driven and refuse to guess:

```python
# Whole-symbol families passive by construction: two-terminal parts, connectors,
# mechanical switches, ESD arrays, discrete FETs. A connector pin has no drive
# direction, and calling one an input or output would be a claim the part does
# not make.
PASSIVE_SYMBOLS = re.compile(r"^(0603WAF|CC0603|...|TC2030|ERJP06)")

IC_RULES = {
    "STM32H755ZIT6": [
        (r"^(VSS|VSSA|VSSSMPS)$", "power_in"),
        (r"^VCAP$", "passive"),             # external stabiliser cap, not a supply
        (r"^NRST$", "bidirectional"),       # open-drain: driven inside and out
        (r"^P[A-K]\d+", "bidirectional"),
    ],
    "AW9523BTQR": [(r"^INTN$", "open_collector"), ...],
    ...
}
```

Anything unlisted falls through to `passive` — **the type that asserts nothing
about direction**. That is the whole discipline: pin type is metadata that cannot
move a net, but it can encode a *wrong claim*, and a wrong claim in a checker's
input is worse than the noise you were trying to remove.

One easily-missed mechanic: the retype must be applied to the **library symbol
and the schematic's `lib_symbols` cache**. ERC reads the cache, and a divergence
between the two is its own warning class.

### `uuid` as a free set operation

The 16 both-ends-dangling stubs were found with no geometry at all — just by
counting how many times each wire's uuid appears in the report:

```python
cnt = collections.Counter()
for sh in erc["sheets"]:
    for v in sh["violations"]:
        if v["type"] == "unconnected_wire_endpoint":
            for it in v["items"]:
                cnt[it["uuid"]] += 1
ghosts = [u for u, n in cnt.items() if n == 2]
```

Measured: **469 flags over 453 distinct wires — 437 flagged once, 16 twice.**
Against 467 wires on the sheet, that means 97% of every wire in the schematic had
a free end. The script then hard-asserts `len(ghosts) == 16` and, before deleting
anything, checks each stub carries exactly one label and that the label's net
name still exists elsewhere in the netlist. Cheap assertions on a derived count
are how a bulk edit stays reviewable.

### Counts that reconcile

Every object-level change accounts for itself against the baseline schematic:

```
              baseline   current   delta
no_connect          35       135    +100   the 100 flags
wires              467       451     -16   the 16 ghost stubs
labels             509       499     -10   -16 ghost labels, +6 PWR_FLAG labels
PWR_FLAG             0         7      +7   1 cached definition + 6 instances
```

PWR_FLAG's symbol went into the project's own `Board3.kicad_sym` rather than
KiCad's global `power` library, so the schematic still resolves from the project
alone — which is also what the library-load check verifies.

## Related

- [A colon in one symbol name makes the entire library unloadable](../integration-issues/kicad-colon-in-symbol-name-makes-library-unloadable.md)
  — owns the 124 `lib_symbol_issues` half of the report (ERC 1131 → 1009). The
  two failures are complements worth holding together: there a gate's **severity
  filter** made a class unreadable; here the report's **volume** did. Either one
  alone is enough to hide a finding indefinitely.
- [A gate that cannot pass gets waved through](../conventions/a-gate-that-cannot-pass-gets-waved-through.md)
  — the same social failure in its third form. That doc's deciding question ("is
  zero a state this artifact could actually occupy?") now answers *yes* for ERC,
  which is the argument for making that gate absolute.
- [Headless DRC judges the board plus its sidecar files](stage-project-sidecars-for-headless-drc.md)
  — the board-side form of the staged-copy trap. Note this campaign is a large
  counterexample to that doc's heuristic *"uniform mass findings with one
  repeated message are configuration, not design"*: 469 + 372 uniform findings
  here were entirely real. The discriminator that survives both cases is whether
  the message names a **resource the checker could not resolve** (configuration)
  or a **property of objects it read fine** (design).
- The campaign lives on branch `fix/board3-review-section4`, carried by **PR #25
  (open, unmerged as of this writing)**. PR #24 is merged off the same branch and
  does *not* contain these commits — its list ends before them, so "merged" and
  "on the head branch" are both true of it while "landed in #24" is false; see
  [verifying every part of a claim](../design-patterns/verifying-every-part-of-a-claim-does-not-verify-the-claim.md).
  Five commits for six classes, each with its own netlist check; `pin_not_driven`
  never got one because it fell out with the pin types, and the last commit
  bundles the final two classes.
- `.github/workflows/kicad-drc-erc.yml` — now four absolute gates and no
  ratchet: ERC at `--severity-all`, DRC through the staged rules, library-load,
  and schematic parity. The comments on the ERC and DRC steps record why each
  stopped being a delta, and where the deleted baseline machinery went.

**Provenance note.** The edits were made by single-purpose scripts written for
this campaign, which live in session scratch and are **not tracked in the repo**.
They are cited here as method, and their rules are reproduced above so the method
survives them. The durable artifacts are the schematic, the netlist-diff
discipline, and this doc.
