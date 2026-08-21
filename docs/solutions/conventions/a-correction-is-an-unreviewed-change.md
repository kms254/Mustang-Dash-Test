---
title: A correction is an unreviewed change
date: 2026-08-21
category: conventions
module: documentation-maintenance
problem_type: convention
component: documentation
severity: high
root_cause: missing_workflow_step
resolution_type: workflow_improvement
applies_when:
  - About to edit a sentence, blockquote or comment block specifically because it states a wrong fact
  - A correction is being applied as a wholesale rewrite of a block -- replacing text is a delete plus an insert, and reviewers only read the insert
  - Hands are already on a sentence for one reason while the other claims inside it go unchecked
  - file:line citations were written before a later edit changed the line count of the same file
  - The claim being corrected was copied from, or copied into, other files in the tree
symptoms:
  - A retraction blockquote added specifically to correct a doc is itself wrong two weeks later -- it asserted 13.5 MHz where the constant is 25, and 13.5 was never the attained rate on that board
  - An edit changes 'wiring' to 'loom' inside one sentence and leaves 'read AND write' standing, which the repo's own source doc contradicts in three separate places
  - A rewrite silently drops the strongest corroborating fact in the tree -- erasing the only copy in live source, with none in the knowledge store to fall back on -- and only git show HEAD recovers it
  - file:line citations written correctly go stale mid-session when lines are inserted above them; the claims validator strips the line number before checking, so it can only ever verify the path
  - One wrong claim lives in CLAUDE.md and in an architecture-patterns doc while the source doc both were copied from says the opposite
related_components:
  - development_workflow
  - tooling
tags:
  - correction-review
  - documentation-drift
  - adversarial-review
  - wholesale-rewrite
  - line-citation-drift
  - claim-propagation
  - knowledge-store
  - compound-engineering
---

# A correction is an unreviewed change

## Context

The 2026-08-21 Board3 SPI clock walk produced a large documentation-correction
pass: the run-clock constant moved to 25 MHz, a readback was added to print the
*attained* rate, and every place in the tree that quoted a stale rate had to be
brought forward. The clock finding itself is written up separately in
`docs/solutions/conventions/a-clock-constant-is-a-request-not-the-operating-point.md`;
it is the setting for this note, not its subject.

What this note is about is what the *correction pass itself* did. Six distinct
defects were introduced or nearly introduced by edits whose entire purpose was
to fix a wrong fact — five during the pass, one historical, showing the same
shape two weeks earlier. None were caught by tooling. All were caught by readers
taking the post-correction tree as the artifact under test. A seventh case — the
adversarial review of *this document*, which returned twelve more — is recorded
at the end.

The generalisation is uncomfortable and it is the durable part:

> **A correction is an unreviewed change. The edit that fixes a wrong fact is
> exactly as capable of introducing one — and it arrives wearing the authority
> of a fix, so it draws less scrutiny rather than more.**

All firmware and documentation changes described below are **uncommitted in the
working tree** as of writing. `git status --porcelain` lists `CLAUDE.md`,
`CONCEPTS.md`, `README.md`, `platformio.ini`, `MustangDash/MustangDash.ino`,
`MustangDash/dash_serial.h`, `docs/hardware/board3-bringup-card.md`, five
modified `docs/solutions/` files, and two still-untracked new documents — this
one and
`docs/solutions/conventions/a-clock-constant-is-a-request-not-the-operating-point.md`.
Historical claims below are therefore cited against `git show HEAD:<path>`
rather than against line numbers in files that are still moving.

## Guidance

Treat any edit whose stated purpose is "fix the wrong bit" as a change that has
not been reviewed, because it has not. Four habits follow, in the order they pay
off.

**1. Run the adversarial pass AFTER the correction pass, not before.** This is
the whole trick and it is purely an ordering property. A review that runs before
the corrections validates text that no longer exists by the time the work ships.
Every one of the five instances here was found by a reader who took the
post-correction tree as the artifact under test. A pre-correction review would
have found none of them, because none of them existed yet.

**2. Diff what a rewrite REMOVED, not what it added.** A wholesale block
replacement is a delete plus an insert, and reviewers — human and model alike —
judge the replacement against the *intent* of the rewrite. Nothing in a normal
diff view makes a dropped fact salient the way an added wrong one is. The cheap
mechanical version is `git show HEAD:<path>` against the working tree, read for
subtraction. Ask specifically: what did the old block assert that the new one no
longer does, and was that assertion load-bearing anywhere?

**3. Grep the whole tree for a retired claim; do not fix it where you noticed
it.** A claim gets copied far more often than it gets checked. When you retire a
number, a mechanism, or a failure description, the site where you found it is
one of an unknown number of sites.
`docs/solutions/conventions/fixing-the-instance-is-not-fixing-the-class.md` is
the hardware-side statement of the same rule; this is its prose form.

**4. Derive citations programmatically, from a settled tree.** `file:line`
references into a file you are also editing go stale silently and keep
resolving — to real lines, with different content. Do not hand-patch offsets;
regenerate the citations once the target file has stopped changing, by searching
for the quoted content rather than by doing arithmetic on line numbers.

Three corollaries worth stating explicitly:

- **A retraction has a shelf life like any other claim.** Being a retraction
  confers no immunity from going stale. Date it, and re-check it whenever the
  thing it corrects moves.
- **Your own context is a snapshot, and after you edit a file it is stale.** A
  reviewer on this round found that a briefing copy of `CLAUDE.md` and the file
  on disk carried *different* text for the same paragraph — the briefing copy
  still asserted a claim the disk version had already bounded and qualified. An
  agent, or a person working from notes or from a page read an hour ago, that
  quotes what it already holds will faithfully restate superseded text and never
  open the file that would contradict it. Re-read what you are about to quote,
  especially when you are the one who changed it.
- **A correction has blast radius.** Correcting a file invalidates pointers
  *into* that file — line numbers, quoted excerpts, "see the third paragraph"
  references — and those pointers may live in a document you are writing in the
  same breath.

## Why This Matters

The mechanism is four separate effects, and understanding them is what makes
this transferable to corrections that have nothing to do with clock rates.

**Authority inversion.** A correction is the output of someone who just looked
closely. It therefore *reads* as more trustworthy than the text around it, and
attracts less review than an ordinary edit would. That is exactly backwards: the
close look was aimed at one property, and everything outside that aim got no
look at all.

**The narrow lens.** A correction is made while checking one specific thing.
That focus is what makes the fix correct — and it is the same thing that makes
an adjacent error invisible. In instance 2 below, the editor's hands were
literally on the wrong sentence, changing four words of it, and the false clause
sitting beside those four words survived untouched.

**Rewrite is delete-plus-insert, and the delete is invisible.** Replacement is
the natural move when a block has two or three wrong claims in it, and it is the
move with the worst failure mode: an inserted wrong fact is visible in a diff, a
deleted right fact is not. Instance 3 destroyed the single best corroborating
fact in the repo for the very learning being written in the same session.

**Corrections invalidate pointers.** Nothing in this repo's toolchain flags
citation drift. The `ce-compound` mechanical claims validator explicitly strips
the line number before checking, in `normalize_path`:

```python
token = re.sub(r":\d+(-\d+)?$", "", token)  # strip `:line` / `:a-b` refs
```

(`~/.claude/skills/ce-compound/scripts/validate-doc-claims.py`)

It checks that a cited *path* exists. It cannot check that line 219 still means
what it meant, and a drifted citation is worse than a missing one, because it
still resolves.

## When to Apply

- Immediately after any pass that fixes a factual error in prose, comments, or
  documentation — schedule the adversarial read for *after* it, not before.
- Whenever you replace a block rather than amend it: budget a subtraction diff.
- Whenever the claim being retired is more than a few weeks old, or plausible
  enough to have been quoted elsewhere — grep before you edit.
- Whenever a document you are writing cites `file:line` into a file that is also
  under edit in the same session. Regenerate at the end.
- Whenever you are about to write "this used to say X, and that was wrong" —
  date the retraction and expect to revisit it.

Not worth the ceremony for a typo fix, a rename with no factual content, or a
correction to a file nothing else points into.

## Examples

### 1. A retraction that itself went stale

`docs/solutions/integration-issues/spi-run-clock-24mhz-overclock-corrupts-eve-coprocessor-reads.md`
carried a blockquote added 2026-08-05 *specifically to correct* a code block the
doc had mislabelled "current tree". At HEAD it reads:

```
> **This doc labelled that block "current tree" until 2026-08-05, and it was
> not.** The constant is **13.5 MHz** (`MustangDash/MustangDash.ino`), set
> 2026-07-23 on the NUCLEO-F767ZI and proven on two panels at fps 59,
> faults 0. [...]
```

Two weeks later it is wrong twice over: the constant is 25 MHz, and 13.5 was
never the attained rate on the current board — Board3's H755 quantises the same
request to 12.5. The replacement blockquote in the working tree now says so in
its own opening sentence and labels itself accordingly: *"The correction written
then is now stale too — read this as an exhibit, not as a number."* A retraction
has a shelf life like any other claim.

### 2. A correction that fixed one word and preserved a false claim beside it

`CLAUDE.md` at HEAD:

```
24 MHz failed read AND write integrity on the Teensy wiring (2026-07-10: white
screen, flash init 0x01, all font inflates failed, fps 25 with faults=0)
```

That exact sentence was edited during the session — `wiring` → `loom`, the
surrounding paragraph restructured — and the words `read AND write` survived the
edit. They are false. The repo's own source doc, which both statements derive
from, says the opposite in **three separate places** at HEAD:

- *"while largely leaving writes intact"*
- *"writes were accepted but rendered incorrectly"*
- *"MOSI-side display-list writes landed well enough to produce a rendered (if
  garbage) frame"*

The narrow lens here was "is this failure attributed to the right hardware?" —
which it now is. The clause beside it got no attention because it was not what
the edit was about. Both halves are fixed in the working tree; the sentence now
reads *"failed read integrity on the **Teensy loom** (writes mostly survived,
reads did not)"*.

### 3. A wholesale rewrite that deleted the strongest supporting fact in the repo

Two claims in a long source-comment block above `DASH_SPI_RUN_HZ` in
`MustangDash/MustangDash.ino` were corrected by *replacing the block*. The
replacement silently dropped this line, present at HEAD:

```
6.75/13.5/27/54 (SPI1 /APB2 108MHz, SPI2 /APB1 54MHz; requests round DOWN).
```

That line recorded that the *older* F767 board was already a **two-kernel
board** — two SPI instances dividing two different source clocks, their
asymmetry hidden by a 2:1 power-of-two ratio. Which is the single best
corroborating precedent in the repo for the exact learning being written in the
same session, about Board3's H7 SPI kernel asymmetry. It was deleted by a
rewrite whose purpose was to make that block *more* accurate.

It came back only because an adversarial reviewer ran `git show HEAD` on the
file. It is now restored and generalised in the working tree —
`grep -n "APB1 54" MustangDash/MustangDash.ino` returns *"APB1 54 MHz, and 54/4
= 13.5 as well -- a 2:1 ratio that hid the"* — and propagated into
`docs/solutions/integration-issues/f767-spi-prescaler-quantization-27mhz-hard-wedge.md`,
which had no copy of it at HEAD at all.

Scope note, because it matters for how you look: the deletion did not erase the
fact from the repository. A 2026-07-22 plan document still carried it. What the
deletion erased was the only copy in the live firmware source — and
`docs/solutions/` had none at all, so after the rewrite the fact survived
nowhere a future session reasoning about SPI clock trees would actually look.

### 4. Citation drift inside one session

A subagent wrote a set of `file:line` citations into the new conventions doc,
every one correct when written and independently verified. The orchestrator then
applied corrections to that same `.ino`, inserting lines above them. Every
citation at or past the insertion point silently became wrong — still resolving
to real lines, now with different content.

Two live specimens of the drift mechanic, measurable right now:

- The sentence from instance 2 sits at **line 34** in
  `git show HEAD:docs/solutions/architecture-patterns/dash-carrier-pcb-buffered-spi-topology-30mhz-clock-contract.md`
  and at **line 58** of the working-tree copy — 24 lines apart, one session.
  (Line 58 now carries the *corrected* form: repairing instance 5 changed what
  the drifted line says as well as where it is.)
- *"MOSI-side display-list writes landed well enough"* is at line **219** at HEAD
  of the 24 MHz doc and line **236** in the working tree.

The citations in the conventions doc were repaired by regenerating them against
the settled tree rather than by patching offsets, and all eleven
`MustangDash.ino:<N>` references now resolve to their intended content.
Spot-checked: `164-169` lands on the three `SPIClass` bus objects, `943-944` on
the `beginTransaction` raise, `302` on `PLLQ = 4U`, `775` on
`dash_report_spi_clocks`, `1807` on its `diag` call site.

### 5. One wrong claim, two sites, and a source that refuted it

The `read AND write` error of instance 2 was not one edit's problem. At HEAD it
sat in **two** places — `CLAUDE.md` and
`docs/solutions/architecture-patterns/dash-carrier-pcb-buffered-spi-topology-30mhz-clock-contract.md`
(*"the first 24 MHz candidate failed read AND write integrity on the bench"*) —
while the **third** document, the source doc both were derived from, said the
opposite three times over. Two independent copies, neither re-derived from the
source, both wrong in the same way. Fixing it where it was noticed would have
left the other one standing.

`grep -rn "read AND write" .` over the working tree now returns only this
document's own quotations of it; the claim is gone from every site that was
asserting it. That exception is not a footnote — an earlier draft of this
sentence claimed the grep returned nothing, which was true when it was written
and was falsified by writing it down. The blast-radius corollary, self-applied.

### 6. And once more, inside the pass that produced this document

The instances above were already written up when a cross-referencing pass found
another, made twenty minutes earlier by the same correction pass — this time in
`CONCEPTS.md`.

The `Saturated Count` entry ended with a concrete case. At HEAD:

```
... indicts the fix's datum rather than the checker's ceiling (the 2026-08-10
board-edge case — twelve silk violations that two successive clips never
touched, because both measured to a bounding box half an edge-stroke outside
the manufactured cut).
```

The project's own vocabulary rules forbid dates and current-config counts inside
glossary entries, so the parenthetical was scrubbed. The scrub was right about
the date and the count — and it also took **the mechanism**, which was never in
violation: *measured to a bounding box … outside the manufactured cut* is the
part that tells a reader what the failure actually looks like. What replaced it
was a generic clause that could not be checked against anything.

The narrow lens was "remove the date and the count". Everything else in the
sentence fell outside it. The entry now keeps the mechanism and omits the date
and the count, which is what the rule actually required:

```
... indicts the fix's datum rather than the checker's ceiling — the classic
shape being a clearance fix measured to a shape's bounding box while the
checker measures to the manufactured outline, half a line-width inside it ...
```

This instance is worth more than the others, because it happened *during the
pass that was documenting the phenomenon*, by an author who had just written the
rule, and it was still missed. That is the strongest available evidence that the
countermeasure cannot be attentiveness. It has to be a second reader with the
diff.

### 7. And in the review of this document

This note was validated the way it recommends: an adversarial reader, with the
tree, running after the writing. It returned twelve defects.

Every claim that was *hard* to get right survived — each `git show HEAD`
comparison, all four drifted line numbers, the validator source quote, both
`CONCEPTS.md` quotations, all eleven firmware citations, the sibling section
title, all three inbound links. What failed was of one kind, and it is the kind
this document is about:

- The intro said **five** instances; the headings ran to six. Instance 6 had
  been appended and the count above it never revisited.
- **"Two corollaries"** introduced three, because one was inserted later.
- The `git status` manifest listed three changed files where there were nine,
  having gone stale the moment instance 6 added another.
- The scope note claimed the rewrite erased "the only copy anywhere in
  `docs/solutions/`". There was never a copy there — a claim contradicted four
  lines earlier by this same document, and propagated into its frontmatter, so
  the wrong claim occupied two sites exactly as instance 5 describes.
- Instance 4's specimen said "the false claim sits at line 58". Repairing
  instance 5 had replaced that line with its corrected form.
- And the verification instruction in instance 5 — `grep` returns nothing — was
  true when written and false by the time it was read, because writing it down
  populated the thing it searched for.

Three of those are the failure mode recorded at §6 of the nearest-neighbour
document: *a countable assertion invalidated by an otherwise-correct edit*.
Reproduced here, in the document that cites it, by an author who had just
finished writing that reproducing it was the risk.

That is the strongest evidence in this file, and it is why the last line of the
Guidance section is the operative one. Attentiveness had every advantage
available here — subject-matter priming, a written rule, a fresh example — and
it did not work. The second reader with the diff did.

## Related

**The nearest neighbour — read the distinction, do not fold them.**
- [Verifying every part of a claim does not verify the claim](../design-patterns/verifying-every-part-of-a-claim-does-not-verify-the-claim.md)
  — its section 6, "The edit that fixes this class is itself in the class",
  states this thesis in a paragraph and already names the first countermeasure.
  That doc's problem is *verification granularity* (a compound claim false while
  every conjunct verifies); this one's is *attention allocation* (a class of
  change under-reviewed because of what it is called). They occur independently:
  a correction can introduce a single-clause falsehood that any granularity of
  checking would catch, if anyone checked. **But state the distinction between
  the documents, not against that section**: §6 and §7 are themselves prior
  recordings of *this* doc's class, so instance 6 below is the third such case
  in the repo rather than the first.
- [Fixing the instance is not fixing the class](fixing-the-instance-is-not-fixing-the-class.md)
  — the sibling, and the split matters: that doc is about a fix that is
  *correct where applied but under-scoped*; this one is about a fix that is
  *wrong where it landed*. Neither doc's remedy catches the other's instances.
  They compose only on instance 5.

**Ancestors of individual countermeasures.**
- [A count at the report limit is not a measurement](../developer-experience/a-count-at-the-report-limit-is-not-a-measurement.md)
  — the narrow ancestor of countermeasure 3: it states "when you retract a
  number, grep for it", scoped to numbers. This generalises it to any retired
  claim.
- [Hand-transcribe a spec multiple times and gate on agreement](../design-patterns/hand-transcribe-a-spec-multiple-times-and-gate-on-agreement.md)
  — why countermeasure 1's reader has to be *independent* rather than merely
  second.
- [Calibrate an automated reviewer on a confirmed defect](../design-patterns/calibrate-an-automated-reviewer-on-a-confirmed-defect.md)
  — the fixture form of the same blind spot: a checker that cannot notice its
  own repair.

**Exhibits.**
- [24 MHz overclock corrupts EVE coprocessor reads](../integration-issues/spi-run-clock-24mhz-overclock-corrupts-eve-coprocessor-reads.md)
  — instance 1, the blockquote that was wrong twice. Its current text labels
  itself an exhibit rather than a source of numbers.
- [A clock constant is a request, not the operating point](a-clock-constant-is-a-request-not-the-operating-point.md)
  — the finding these corrections were serving, and the round this came out of.
