---
title: "Verifying every part of a claim does not verify the claim"
date: 2026-08-05
category: design-patterns
module: grounding-validation
problem_type: design_pattern
component: tooling
severity: high
applies_when:
  - "Designing a validator, gate, or review question whose PASS will be read as evidence for a claim"
  - "A check verified several supporting facts and returned PASS on a compound claim"
  - "Restating a measured figure that another doc established at some earlier time"
  - "The evidence for a conclusion is an absence — a grep that returned 0, a count that stayed flat"
  - "A mechanical checker fires confidently on something outside the class it was built to judge"
symptoms:
  - "A validator returns VERIFIED on a false claim while every individual check it ran is correct"
  - "A differently-shaped question about the same claim catches in one pass what the first validator passed three times"
  - "A figure carried between docs was true when written, was never re-measured, and the gate it justifies outlives its premise"
  - "Evidence for a conclusion would look identical if the conclusion were false"
  - "A mechanical checker flags two fragments of a UUID as unresolvable commit SHAs"
resolution_type: workflow_improvement
related_components:
  - documentation
  - development_workflow
tags: [verification, grounding-validation, ce-compound, claim-entailment, stale-premise, evidence-design, false-negative, false-positive]
---

# Verifying every part of a claim does not verify the claim

## Context

`ce-compound`'s Phase 2.45 exists to stop wrong facts entering a permanent
knowledge store. It runs two passes over a freshly written doc: a mechanical
script (`.claude/skills/ce-compound/scripts/validate-doc-claims.py`) and one
read-only semantic validator subagent driven by the prompt template in
`.claude/skills/ce-compound/references/grounding-validation.md`.

In one session that pipeline blessed a false claim three times, in three
distinct shapes, and in the same session its mechanical half fired confidently
on an object that was never in its domain. **Every individual check was
correct.** No check was sloppy, no command was wrong, no output was misread. The
composition of correct checks was the defect.

Only the first shape is new to this repo. The other two are already documented
here from other directions, and are included because seeing all three together
is what identifies the family: one error surviving three different
transformations — decomposition, time, and discrimination — none of which more
rigour would have caught.

## Guidance

### 1. Check the predicate that carries the claim, not the conjuncts around it

A claim that decomposes cleanly is the dangerous kind, because each piece
verifies and the decomposition is never itself checked.

A draft asserted that the fix had landed in PR #24. (That sentence was corrected
before the commit, so it exists in no committed artifact — grepping for it finds
nothing. The phenomenon below is substantiated independently.) The validator
returned VERIFIED on two true facts:

- `gh pr view 24 --json state` → `MERGED` — true
- the fix commit is on that PR's head branch — true

Both true. The claim is false. The predicate that carries it is *"is the commit
**in** the PR"*, and that is neither of the two that were checked. The
discriminating command takes one line:

```bash
gh pr view 24 --json commits --jq '.commits[].oid'   # 21 commits, ending 0114404
```

**A merged PR's head branch keeps accepting commits.** `merged` and `on the head
branch` are jointly satisfiable by work the PR does not contain, which is
exactly what happened: 14 further commits landed on `fix/board3-review-section4`
starting nine minutes after PR #24 merged.

`grounding-validation.md` named exactly one primary check for this category —
`gh pr view <n> --json state,mergedAt,baseRefName`, three fields that cannot
answer the question. It now requires two halves, both of which must pass:

```text
2. MERGE-STATE CLAIMS — ... The primary check has TWO halves and BOTH
   must pass:
     (a) did the PR land —
         gh pr view <n> --json state,mergedAt,baseRefName
     (b) is THIS change in it —
         gh pr view <n> --json commits --jq '.commits[].oid'
         and confirm the implementing commit is listed; or
         git merge-base --is-ancestor <commit> origin/<baseRefName>
   (a) ALONE IS INSUFFICIENT and passing it is not evidence for the claim
```

Worth noting what the old text got backwards: it ranked `gh` as primary and git
reachability as a *fallback for when `gh` is unavailable*. For this claim shape
the fallback was the discriminating check. A tool's convenience ranking is not a
ranking of evidential strength.

The Step 1 adjudication table gained the matching row:

| Flag | Likely meaning | Resolution |
|------|----------------|------------|
| PR merged, but the change is absent from its commit list | Later work pushed to a merged PR's still-live head branch | Contradicted — cite the PR that actually carries the commit (often a later, still-open one), or state the change as unmerged |

### 2. A measured figure is a claim with an expiry date

Two documents asserted, in substance, that a DRC floor was real and its CI
ratchet still earned its keep. The figure behind it came from
[a gate that cannot pass gets waved through](../conventions/a-gate-that-cannot-pass-gets-waved-through.md)
(*"a board whose measured floor was 36"*) and was true when written. Nobody
re-measured. When challenged, the measurements returned 0.

The validator never flagged it, and could not have: it verified **what the gate
did**, not **whether the gate's premise still held**. Those are different
questions and only one was asked.

**A ratchet is a claim about the artifact, and claims expire.** So is a floor, a
baseline, a benchmark, a "typical" count. The protocol gained a fourth claim
category:

```text
4. MEASURED-FIGURE CLAIMS — any number the doc presents as a current
   property of the artifact ... This includes figures the doc inherited
   from another doc, which is where they go stale unnoticed. Re-run the
   measurement now ... A figure that cannot be re-measured must be
   restated with the date it was taken rather than left in the present
   tense. Note that a gate justified by such a figure is making the same
   claim, so it expires with it.
```

This shape is otherwise well covered here — see
[re-derive the constant, not the threshold](../conventions/re-derive-the-constant-not-the-threshold.md)
for the same instinct inside a test suite.

### 3. Ask whether the evidence would look different if you were wrong

A doc proved a symbol was used **zero times** by grepping the `.kicad_sch` for
its literal name and getting 0 matches. The result was true. The test was
worthless: **KiCad escapes a colon in a symbol name as `{colon}` when it writes
the name into a file**, so that grep returns 0 for a symbol that *is* placed.

The evidence was identical under both outcomes. It could not lose.

The replacement enumerates every `(lib_id "…")` and tests membership. Same
conclusion, different epistemic standing — one of them could have come out
otherwise. The CODE-BEHAVIOR category now carries the question that exposes it:

> For every piece of evidence, also state what result would have appeared if the
> claim were FALSE. If the answer is "the same result", the evidence does not
> discriminate and the verdict is unverifiable, however true the claim may be.

The sharpest existing statement of this is
[a holdout list keyed outside the artifact it protects](../logic-errors/holdout-net-list-matched-nothing-after-hierarchical-rename.md):
*"A guard is only evidence if it can disagree with the thing it guards."* That
doc reaches it from a guard that shared names with the operation it guarded; this
one from evidence of absence. Evidence of absence is where it hides best.

### 4. The inverse: a matcher that fires outside its own domain

The same disease runs backwards. The mechanical checker repeatedly flagged a
KiCad object UUID as an unresolvable commit SHA. `SHA_RE` is
`\b[0-9a-f]{7,40}\b`; `-` is a non-word character, so `\b` fires at every UUID
segment boundary. The digit-and-letter guard below it was written to exclude
dates and decimal ids and has never heard of a UUID. The flag text then advised
replacing a schematic object identifier "with the PR number."

Fixed by matching UUIDs first and skipping any SHA candidate inside one:

```python
UUID_RE = re.compile(
    r"\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b", re.I
)
# in main(), before the SHA loop:
uuid_spans = [m.span() for m in UUID_RE.finditer(body)]
# inside the loop:
if any(a <= m.start() and m.end() <= b for a, b in uuid_spans):
    continue  # a segment of a UUID, not a commit
```

Verified in both directions, which for a matcher means proving it still fires:
on a fixture carrying a bogus SHA, a real local-only SHA and a UUID, the bogus
one is flagged as unresolvable, the real one is flagged as local-only, and the
UUID is not counted at all — 2 SHAs checked, not 4.

### 5. What actually caught each — the finding

- Shape 1 was caught by **a second validator asked a differently-shaped
  question** ("does PR #24 carry this campaign?"), in one command.
- Shape 2 was caught by **a human who remembered the number should have
  changed**.
- Shape 3 was caught by **an adversarial pass that questioned the evidence
  rather than the conclusion**.

**These three attributions do not meet this doc's own standard, and neither does
"every individual check was correct."** No run report survives in the tree, so
the repository would look identical if any of them were false. They are recorded
from session memory, they are consistent with everything that *was* measured, and
they cannot come out the other way — which by §3 makes them unverifiable however
true. Stated plainly rather than quietly, because the alternative is a doc about
non-discriminating evidence resting on some. The measurable parts — the merge
state, the escaping, the matcher behaviour — are worked in Examples and do
discriminate. If this pipeline should durably support the claim, the fix is to
persist validator run reports, not to argue the point harder.

None was caught by more of the same checking. More rigour on "is PR #24 merged?"
converges harder on the wrong answer; the question had to change shape. The
cheapest way to buy a shape change is to put the shape-changing questions
*inside* the existing validator prompt, which is what items 1–3 above do. (Item
4 is not one of them: its remedy is a matcher fix in the mechanical script, not a
question in the prompt.) A genuinely separate second validator is worth its cost
on the narrow class of docs whose central claim is "this landed."

### 6. The edit that fixes this class is itself in the class

Applying item 2 added a fourth claim category to the validator prompt and left
the sentence above it reading *"Check every factual claim in three categories"*,
with the categories numbered 1, 2, 4, 3. A countable assertion invalidated by an
otherwise-correct edit — the exact defect this doc documents — introduced into
the artifact by the fix for it, and caught only because a validator was pointed
at the doc afterwards.

Both are corrected now. Keep the incident: it is the cheapest available evidence
that this class is not a lapse in care. The edit was deliberate, reviewed, and
made by someone holding the problem in mind, and the count still went stale — so
the countermeasure has to be a question somebody asks afterwards, not an
intention to be careful.

## Why This Matters

Phase 2.45's own framing is that the doc "becomes permanent, trusted knowledge —
future agents will act on its claims without re-verifying them." A false claim
that survives grounding validation is worse than an unvalidated one, because
VERIFIED is read as evidence.

Each of the three shapes produced a confident, specific, defensible wrong
answer, and each was defensible *because the sub-checks were right*. A reviewer
auditing the validator's work would have re-run the two `gh` commands, seen them
pass, and signed off. **The audit and the original share the failure, because
they share the question.** That is the property that makes this class expensive:
it is invisible to review that inherits the framing.

The staleness shape has the longest tail. A ratchet whose floor has been cleared
keeps passing while asserting nothing — it consumes CI minutes, occupies the slot
where a real gate would go, and reads as coverage. It had been in that state for
an unknown interval before anyone thought to re-measure.

## When to Apply

- Whenever a validator returns VERIFIED on a claim that decomposes into
  independently checkable parts — ask which single predicate carries the claim,
  and whether it is one of the parts that was checked
- Before believing any merge-state claim: check commit membership, not just PR
  state, and re-check when the head branch is still live
- Whenever a doc states a measured number in the present tense — floors,
  baselines, counts, timings, "typical" figures
- When designing a second verification pass: make it differently shaped, not
  more rigorous
- When a check reports nothing about something you believe is there, or reports
  something about an object it should not recognise — suspect the matcher's
  domain before the artifact
- Not for choosing what a calibration fixture should probe for; that is the
  neighbouring doc's subject (see Related)

## Examples

**Shape 1, worked.** All commands run 2026-08-05 on branch
`fix/board3-review-section4`:

```console
$ gh pr view 24 --json state,mergedAt,baseRefName,headRefName
{"baseRefName":"main","headRefName":"fix/board3-review-section4",
 "mergedAt":"2026-08-05T02:36:54Z","state":"MERGED"}

$ gh pr view 24 --json commits --jq '.commits | length'
21
$ gh pr view 24 --json commits --jq '.commits[-1].oid'
01144047d8b89dfe0f9c0bdb7e421630b259bb05

$ git merge-base --is-ancestor 1baff0a origin/main; echo "exit=$?"
exit=1

$ git branch -a --contains 1baff0a
* fix/board3-review-section4
  remotes/origin/fix/board3-review-section4
```

The commit *"one colon made the whole symbol library unloadable"* does not
appear in PR #24's commit list at all. The last commit PR #24 carries is dated
`2026-08-04T20:31:55-06:00`; the merge completed at `20:36:54-06:00`; the first
of the 14 campaign commits is dated `20:45:31-06:00`. Every one was pushed to
that branch *after* the PR merged. They are carried by **PR #25**, open against
`main` from the same head branch and unmerged as of this writing.

> The bare SHAs above appear as command arguments, not as citations. The
> mechanical script flags the local-only one (`1baff0a`) as reachable-from-HEAD-
> only and advises citing the PR number, which is correct; `0114404` is
> reachable from `origin/main` and draws no flag at all. The merge-state claims
> in this doc cite PR #24 and PR #25.

**Shape 2, worked.** The premise was a 36-violation floor:

```console
$ kicad-cli pcb drc --severity-all --schematic-parity "<the .kicad_pcb>"
Found 0 violations
Found 0 unconnected items
Found 0 schematic parity issues

$ python tools/kicad_verify.py "<the .kicad_pcb>"
violations   : 0
unconnected  : 0
```

Note the flag: a bare `--severity-all` run reports only violations and
unconnected items. The third number requires `--schematic-parity`, which is off
by default — a checker's default scope is itself a claim about what "clean"
covers.

**Shape 3, worked.**

```console
$ grep -c "CAPACITOR_THT:CP_RADIAL" "<the .kicad_sch>"          # the worthless test
0
$ grep -n "CAPACITOR_THT{colon}CP_RADIAL" "<the .kicad_sch>"    # the same name, as written
3933:  (symbol "ProPrj_New-easyedapro:CAPACITOR_THT{colon}CP_RADIAL_D8.0MM_P2.50MM"
4036:    (symbol "CAPACITOR_THT{colon}CP_RADIAL_D8.0MM_P2.50MM_1_0"
```

Line 3933 is the fully-qualified library-ID form — exactly the string shape a
`(lib_id …)` carries — so a literal-name grep is blind by construction, not by
accident. The discriminating test enumerates instead: 149 `lib_id` references,
44 distinct, none containing `CAPACITOR`. The conclusion held; only the second
test could have told you otherwise.

**The shape recurs within hours.** The docs corrected in shape 1's aftermath
were careful — they say *"PR #24 is merged but ends before these commits"* — and
then added *"and no open PR yet carries them."* PR #25 was opened three minutes
after that commit was authored. **The correction was falsified faster than the
sentence it corrected**, by shape 2 acting on shape 1's fix. The durable form is
an as-of qualifier plus the PR number, re-checked at publication — not a negative
existential about the state of the world.

## Related

- [Calibrate an automated reviewer on a confirmed defect](calibrate-an-automated-reviewer-on-a-confirmed-defect.md)
  — **the nearest neighbour, and the boundary is worth stating.** That doc is
  about the *fixture*: whether the defect a gate probes for is real, still
  present, and matched by concept rather than name collision. This doc is about
  the *claim*: whether a set of individually correct checks composes into the
  assertion being made. They meet at its `VBUS` matcher, which accepted a finding
  that merely shared a name with the defect — shape 3 reached from the tool's
  side. Use that doc when deciding what a check should probe for; use this one
  when deciding whether a passing check licenses the conclusion drawn from it
- [A guard is only evidence if it can disagree with the thing it guards](../logic-errors/holdout-net-list-matched-nothing-after-hierarchical-rename.md)
  — the strongest existing statement of shape 3, reached from a self-referential
  guard rather than from evidence of absence
- [A gate that cannot pass gets waved through](../conventions/a-gate-that-cannot-pass-gets-waved-through.md)
  — the source of the 36-violation floor that shape 2 restated, and now the
  record of its closure. "A ratchet is a claim about the artifact, and claims
  expire" is that doc's formulation
- [Re-derive the constant, not the threshold](../conventions/re-derive-the-constant-not-the-threshold.md)
  — shape 2 inside a test suite: when a measured number stops matching, re-derive
  it rather than widening the band around it
- [A large ERC count is a broken instrument](../developer-experience/a-large-erc-count-is-a-broken-instrument.md)
  — the campaign whose merge state shape 1 got wrong, and the doc whose quoted
  UUID tripped the mechanical checker
- [A colon in one symbol name makes the entire library unloadable](../integration-issues/kicad-colon-in-symbol-name-makes-library-unloadable.md)
  — shape 3's subject; its Solution section now carries the corrected
  enumerate-don't-grep reasoning
