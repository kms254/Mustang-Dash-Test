---
title: "A tool's finding can be a property of the tool, not of the artifact"
date: 2026-08-06
category: developer-experience
module: kicad-silk-mirror
problem_type: developer_experience
component: tooling
severity: high
applies_when:
  - "A measuring script reports a defect in an artifact and that report is about to become a blocker"
  - "Writing a fingerprint, dedup key, cache key, snapshot comparator or diff that decides whether two things are the same"
  - "Comparing geometry, floats, or any derived measurement for equality"
  - "A grouping, cluster, or bucket count lines up exactly with an obvious confounder"
  - "Converting coordinates between two frames with no reference implementation to check the convention against"
tags: [kicad, tooling, fingerprint, false-positive, tolerance, coordinate-transform, verification, board3]
---

# A tool's finding can be a property of the tool, not of the artifact

## Context

`tools/kicad_silk_mirror.py` mirrors edited board footprints back into their
`.pretty` libraries. A placed footprint is a full copy of its library
definition, so a board-only silkscreen fix disagrees with the library
(`lib_footprint_mismatch`) and is reverted by the next *Update Footprints from
Library*; a library-only fix changes nothing that gets fabricated. Both halves,
or neither — that is the tool's whole reason to exist
(`tools/kicad_silk_mirror.py:1-23`).

Because a library definition is shared, the tool mirrors only the **first**
instance of each library id, and it refuses to mirror a type whose placements
disagree with each other:

```
DIVERGENT -- instances of one type whose silk differs, NOT mirrored:
    <fpid>    11 instances, 4 distinct
  Pick the intended geometry by hand; a tool cannot know which is right.
```

That refusal is good design. It does not silently pick a winner. It names the
references in each group so a human can adjudicate. It is exactly the behaviour
you want from an instrument.

It reported **`C0805`: 11 instances, 4 distinct geometries**, and **`R0603`: 31
instances, 4 distinct**. The silkscreen clearance campaign was **reverted** on
that finding, and the blocker was written into the plan as unit U0
(`docs/plans/2026-08-05-001-fix-board3-silkscreen-clearance-plan.md:211`).

`C0805` has exactly **one** geometry. So does `R0603`. The divergence was
manufactured entirely inside `silk_signature()` — three independent bugs, each
sufficient on its own to block the mirror. Measured in isolation they are not
interchangeable: bug 1 alone diverges 8 types **including both figures quoted
above**, while bug 2 alone diverges 4 and bug 3 alone diverges 5, neither of
them touching `C0805` or `R0603`. The headline number came from bug 1; the
other two would have kept the mirror shut after it was fixed.

The plan's U0 is now headed **RETIRED**
(`docs/plans/2026-08-05-001-fix-board3-silkscreen-clearance-plan.md:170`), with
the superseded text preserved in a `<details>` block from line 208, because the
reasoning in it was sound given what it was told. The failure was upstream of
the reasoning. The tool fix and that plan edit landed in **PR #28** (merged); this
write-up is later.

## Guidance

Five rules, in the order they would have caught this.

### 1. Validate a coordinate transform against an artifact that already holds the answer

Do not reason about the sign convention. Find something that already contains
the transformed form and check that you reproduce it.

Bug 1 was that `silk_signature()` subtracted the footprint's **position but
never its orientation**. One library part placed at 0°/90°/180°/270°
fingerprinted as four geometries. The "4 distinct" was, literally, the count of
distinct rotations.

The fix needs a sign, and the sign was *tested*, not chosen. A library
`.kicad_mod` file **is** the footprint-local form, so it is the oracle:

- un-rotating by **+angle** reproduces the library's own coordinates **31/31 on
  `R0603` and 11/11 on `C0805`**, at all four rotations;
- un-rotating by **−angle** matches only the 0°/180° instances — **13/31** and
  **4/11**.

That asymmetry is what makes it a test rather than a preference. A convention
argued from first principles gives you a 50% coin flip that feels like
certainty; the oracle gives a result that could have come back either way and
didn't. `tools/kicad_silk_mirror.py:48-53` records the check and tells the next
reader to re-run it before trusting any change to that function.

Generalise: for any frame conversion — screen↔world, local↔global, UTC↔local,
row-major↔column-major, big↔little endian — locate a value whose transformed
form is already written down by someone else, and require an exact match on
enough cases to distinguish the candidate conventions.

### 2. Exact equality is the wrong comparison for measured or derived geometry

Compare at a tolerance the physical domain can actually hold.

Bug 2 was hashing exact integers. The trimmer computes cut points in **absolute
board coordinates** and rounds to integer nanometres, so one logical cut applied
to two placements of the same type lands up to **1 nm** apart. Measured on
`SOIC-8`: 26 shapes on each instance, worst matched-shape delta **1 nm**.

A nanometre is a millionth of a millimetre. No fabrication process has any
relationship to it. Exact hashing called that a divergence and refused to mirror
a type that is identical to any standard that exists.

The fix replaced hashing with a tolerant comparator, `_agree()`
(`tools/kicad_silk_mirror.py:133-148`), which walks paired entries and requires
shape kind and layer to match exactly while allowing width and every coordinate
to differ by up to `tol_nm`. `group_instances()`
(`tools/kicad_silk_mirror.py:101-123`) clusters placements through it. The
tolerance is a flag, defaulted to **1000 nm = 1 µm**
(`tools/kicad_silk_mirror.py:155-157`) — three orders of magnitude above the
observed noise and still two below anything a fab can resolve.

Pick the tolerance from the domain, not from the noise: it should be small
enough that a real difference cannot hide under it and large enough that
representation noise cannot escape it. If no such gap exists, you are comparing
the wrong quantity.

### 3. A fingerprint must cover the fields that actually carry the data for *every* shape kind it accepts

A silent per-kind gap is invisible, because the function still returns
something.

Bug 3: for polygons, the signature used `GetStart()` / `GetEnd()`. Those are not
a polygon's outline — that lives in `GetPolyShape()` — and they came back as
**raw board coordinates**, un-relativised. The result was "footprint-local"
pairwise deltas of **50–312 mm** on parts a few millimetres across (the
"coordinates" themselves landing 90–252 mm from the origin, depending on whether
you measure per-component or as a distance). Every type still
reporting divergent after bugs 1 and 2 were fixed was one carrying a polarity
band or a pin-1 marker, i.e. one carrying a polygon.

Nothing crashed. Nothing warned. The function returned a tuple of numbers, the
comparator compared them, and the answer was wrong for one shape kind out of
several. The fix branches on shape kind and walks the outline rings explicitly
(`tools/kicad_silk_mirror.py:82-95`), with `_points()`
(`tools/kicad_silk_mirror.py:126-130`) flattening each entry's coordinates so
one comparator handles both forms.

When a fingerprint function accepts a union type, enumerate the variants and ask
of each one: *does this branch read the field that carries the identity?* A
`default` branch that falls through to generic accessors is the shape of this
bug.

### 4. When a grouping matches a confounder exactly, suspect the grouping

`C0805`: 4 distinct geometries, and 4 distinct rotations. `R0603`: 4 and 4. That
coincidence was sitting in the report the whole time.

**A grouping that lines up perfectly with an obvious confounder is a bug until
proven otherwise.** Real divergence is ragged — you would expect 2, or 7, or one
outlier against ten identical parts. A count that reproduces a nuisance variable
exactly is the nuisance variable wearing the result's clothes.

Cheap test: cross-tabulate the reported groups against every confounder you can
name (rotation, layer, insertion order, file, locale, hash seed, timestamp). If
one of them is a bijection with your groups, you have found your bug, not your
finding.

### 5. Range-check your own intermediate values against what is physically possible

"Footprint-local" coordinates 90–252 mm from the origin on a 3.5 mm part are
outside the range the quantity can occupy. Not surprising — *impossible*. One assertion on
the intermediate — local coordinates must lie within the footprint's bounding
box, with slack — would have caught bug 3 the first time it ran, without anyone
needing to suspect polygons.

Intermediate values in a measuring tool are cheap to bound, because you almost
always know the physical envelope: a local offset is bounded by the part, a
percentage by 0–100, a duration by the run, a byte count by the file. Assert it.
The values that end up in a report get scrutinised; the ones feeding it do not.

## Why This Matters

**The tool's good behaviour is what gave the false finding its authority.**

`kicad_silk_mirror` refuses to guess. It reports divergence rather than
picking a winner, it names the references in each group, and it says outright
that "a tool cannot know which is right"
(`tools/kicad_silk_mirror.py:214-220`). That is correct design and it is why the
report was believed. A tool that fails loudly and declines to paper over
ambiguity earns trust — and trust is exactly what converts its output into a
decision without a second look.

So the correctness of a fail-loud tool matters *more* than that of a
best-effort one, not less. A tool that shrugs gets checked. A tool with the
integrity to stop the line gets obeyed. When it is wrong, it is wrong with the
full weight of its own good manners behind it — and the cost here was a whole
work campaign reverted and a blocker written into a plan.

The false report also survived review, because the story it told was *plausible*:
"instances of one type genuinely have different neighbours" is a true sentence
about PCB silkscreen, and it is a real reason placements of one footprint could
legitimately diverge. A plausible mechanism is not evidence that the mechanism
fired. Review caught neither the 4-equals-4 coincidence nor the 312 mm "local"
offset, both of which were in the data.

With all three bugs fixed, **all 30 mirrorable types mirror cleanly**, zero
divergent, and the board holds DRC 0/0 with `lib_footprint_mismatch` 0.
Verified at the current tree (both transcripts trimmed to the lines that carry
the claim — the mirror also prints its library dir, and the verify its board
name and a drill cost-floor advisory):

```
$ "C:/Program Files/KiCad/10.0/bin/python.exe" tools/kicad_silk_mirror.py \
    "kicad/board3/ProPrj_New Project_2026-07-15_23-14-34_2026-07-27.kicad_pcb"
types mirrored   : 0
types unchanged  : 30  (library already matches, not rewritten)
types skipped    : 5   (no .pretty beside the board)

$ "C:/Program Files/KiCad/10.0/bin/python.exe" tools/kicad_verify.py <board>
violations   : 0
unconnected  : 0
```

(0 mirrored / 30 unchanged is the *post-apply* steady state; the apply run in
PR #28 wrote 17 and left 13 already matching — 30 either way.)

## When to Apply

- **Before a tool's finding becomes a blocker.** If a report is about to revert
  work, cancel a plan, or open a bug against an artifact, spend one pass
  attacking the *instrument* first. The asymmetry is brutal: checking the tool
  costs an hour; acting on a false finding cost a campaign.
- **Whenever you write a "these two things differ" report.** This shape is not
  specific to PCB footprints. It covers deduplication keys, cache keys,
  snapshot-test comparators, checksum and diff tools, content-addressed storage,
  memoization, database uniqueness constraints, and any equality check on
  derived values. The three bugs, and the signal that would have caught all of
  them, port directly:
  - *unnormalised inputs* (bug 1) → a cache key that includes an irrelevant
    dimension, so identical work misses every time; a snapshot test that fails
    on key order, timestamps, or locale;
  - *exact equality on derived values* (bug 2) → float comparison in a
    numerical test, a checksum over a serialisation that carries incidental
    whitespace, a dedup key over a value that round-trips lossily;
  - *per-variant gaps* (bug 3) → a hash function that ignores one field of a
    tagged union, so two distinguishable records collide, or two identical ones
    do not;
  - *confounded groupings* (rule 4, the detection signal rather than a fourth
    bug — here a symptom of bug 1) → cluster counts that equal the number of
    input shards, worker threads, or source files.
- **On any coordinate, unit, or frame conversion** — before the transform is
  used, not after its output looks reasonable.
- **When a review passes on a plausible-sounding finding.** Plausibility is what
  makes a wrong answer expensive. Ask what evidence would distinguish the
  plausible story from the bug, and go look for it.

## Examples

**The report that stopped the campaign, and what was actually true:**

| Reported | Actual |
|---|---|
| `C0805` — 11 instances, 4 distinct geometries | one geometry, placed at 0°/90°/180°/270° |
| `R0603` — 31 instances, 4 distinct geometries | one geometry, four rotations |
| 9 types divergent, mirror blocked (as reported 2026-08-05) | 0 types divergent, all 30 mirror |

**Un-rotation, validated instead of argued** — the library file is the oracle:

| candidate | `R0603` match | `C0805` match |
|---|---|---|
| un-rotate by **+angle** | **31/31** | **11/11** |
| un-rotate by −angle | 13/31 (0°/180° only) | 4/11 (0°/180° only) |

**Exact vs. tolerant comparison**, measured on `SOIC-8`: 26 silk shapes per
instance, worst matched-shape delta **1 nm**. Exact integer hashing → divergent.
Comparison at 1 µm → identical. Both statements are about the same two objects.

**Two further defects surfaced only while confirming the fix** — both are the
same family as the finding itself, an operation editing more of the artifact
than it advertises:

- `pcbnew.FootprintSave()` writes whatever the clone carries, so mirroring wrote
  `Reference "R12"` into `R0603.kicad_mod`. Every future placement of that part
  would have arrived pre-named R12. A library definition is a **template** and
  must not inherit the designator of whichever placement happened to be cloned.
  Fixed by `clone.SetReference("REF**")` before saving
  (`tools/kicad_silk_mirror.py:199-205`). Compare U57, where a footprint swap
  silently changed a field's *visibility*.
- It rewrote all 30 types whether or not any had changed, churning the uuid of
  every property in the 13 untouched `.kicad_mod` files and burying the real
  diff. It now loads the library copy, compares it through the same tolerant
  `_agree()`, and skips a match
  (`tools/kicad_silk_mirror.py:183-192`), reporting `types unchanged` separately.
  **A write with no semantic change is not free** — it costs the reviewer the
  ability to see the change that mattered.

## Related

- `docs/plans/2026-08-05-001-fix-board3-silkscreen-clearance-plan.md` — U0,
  RETIRED at line 170 with the superseded original preserved from line 209.
- `tools/kicad_silk_mirror.py` — `silk_signature()` (line 37),
  `group_instances()` (101), `_points()` (126), `_agree()` (133). Each docstring
  records the false reading it was written to prevent.
- `tools/kicad_silk_trim.py` — the trimmer whose absolute-coordinate cut points
  produced the 1 nm spread.
- PR **#28** (merged) — the fix, plus the silkscreen campaign it unblocked.
- [a large ERC count is a broken instrument](a-large-erc-count-is-a-broken-instrument.md)
  — the same lesson from the other direction: a checker's *count* treated as a
  property of the design when it was a property of the checker.
- [a count at the report limit is not a measurement](a-count-at-the-report-limit-is-not-a-measurement.md)
  — a reported number that describes the reporter's limit rather than the
  artifact.
- [clip-test board window queries](clip-test-board-window-queries.md) — a
  measurement that omitted objects by construction, per object type.
- [build connectivity before counting airwires](build-connectivity-before-counting-airwires.md)
  — an oracle that lied in both directions until it was refreshed before being
  read.
- [an integer-nanometre comparison hides real mismatches](../integration-issues/kicad-lib-footprint-mismatch-integer-nanometre-comparison.md)
  — the closest sibling by *mechanism* rather than by theme. There a count stays
  flat while real differences accumulate; here a count inflates while no real
  difference exists. Both are a comparison that answers a question nobody meant
  to ask, and both are fixed the same way: compare at a tolerance the physical
  domain can actually hold, and derive the number from the data rather than
  accepting the comparator's verdict.
- [verifying every part of a claim does not verify the claim](../design-patterns/verifying-every-part-of-a-claim-does-not-verify-the-claim.md)
  — the divergence report was assembled from three true readings (the position
  *was* subtracted, the integers *did* differ, the polygon fields *were* read)
  and composed into a false conclusion.
- [calibrate an automated reviewer on a confirmed defect](../design-patterns/calibrate-an-automated-reviewer-on-a-confirmed-defect.md)
  — the general remedy this case wanted: a comparator is a reviewer, and an
  uncalibrated one is trusted exactly as far as its last unchallenged output.
- CLAUDE.md, "KiCad runs NO silkscreen test unless a `silk_clearance` rule
  exists" — probe any check you believe is running by asserting something it
  must fail. The present doc is the converse: probe any finding you believe,
  by attacking the instrument that produced it.
- [derive the fab viewer's rule before trusting its outlier](derive-the-fab-viewers-rule-before-trusting-its-outlier.md)
  — the same attack run against an instrument that cannot be fixed: a
  consistency sweep localizes a library fault, a pad-pitch scale check
  disqualifies the model, and the ladder ends at *can neither confirm nor
  deny* — closed at ground truth plus a written assembly remark rather than a
  tool fix. The same viewer returned a TRUE finding (P1/P2, PR #36) in the
  same session: the verdict attaches to the finding, not the tool.
