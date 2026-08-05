---
title: "A colon in one symbol name makes the entire library unloadable, and ERC calls it 'not found'"
date: 2026-08-04
category: integration-issues
module: kicad-symbols
problem_type: integration_issue
component: tooling
severity: high
symptoms:
  - "124 ERC `lib_symbol_issues` warnings saying the library 'was not found' at a path where the file plainly exists"
  - "GUI Change Symbol reports `*** symbol not found ***`, including for parts changing to the symbol they already use"
  - "KiCad's Symbol Chooser lists 24,335 items, all from stock libraries, with neither project library in the tree"
  - "Schematic renders, netlist, BOM and DRC (0 violations / 0 unconnected / 0 schematic parity) are all clean, so nothing points at the library"
root_cause: config_error
resolution_type: tooling_addition
related_components:
  - development_workflow
tags: [kicad, kicad-sym, symbol-library, lib-id, erc, sym-lib-table, easyeda-import, board3]
---

# A colon in one symbol name makes the entire library unloadable, and ERC calls it 'not found'

`kicad/board3/ProPrj_New-easyedapro.kicad_sym` held a symbol named
`CAPACITOR_THT:CP_RADIAL_D8.0MM_P2.50MM`. A colon is the library/symbol
separator in a KiCad LIB_ID (`Library:Symbol`), so KiCad refused to load that
library **entirely** — all 93 other symbols with it. It had been that way for
months, through every check this project runs.

## Problem

A single illegal character in one unused symbol name takes down an entire
`.kicad_sym` library. The failure is total (no symbol in the file is usable),
silent (the schematic keeps rendering), and mislabelled (the only diagnostic
says "not found" about a file that exists).

## Symptoms

In the order they are likely to reach you — which is the opposite of the order
of severity:

- **GUI `Change Symbol` fails on parts that are not being changed.** The
  attempt on SW1–SW4 reported `*** symbol not found ***`, *including for
  SW6/SW7, which were changing to the symbol they already used*. That is the
  observation that makes it unmistakably a library problem rather than a part
  problem: a symbol cannot simultaneously be in use on the sheet and not exist.
- **The Symbol Chooser is missing both project libraries.** Per this session's
  reading of the GUI, it listed 24,335 items, all stock — not re-derivable
  outside the running editor, so take it as corroboration, not measurement.
- **ERC carries a large, stable block of warnings reading like a stale path:**
  124 `lib_symbol_issues`, each *"The symbol library 'ProPrj_New-easyedapro'
  was not found at '…/ProPrj_New-easyedapro.kicad_sym'"*. The file is plainly
  at that path. **"Not found" here means "failed to load,"** and reading it
  literally sends you hunting a path problem that does not exist. Those 124 were
  present in every ERC baseline recorded up to the fix — though no baseline
  artefacts are kept in the tree, so that span rests on the fix commit's own
  account rather than on something re-derivable.
- **Everything that gates the work is green.** The sheet drew correctly, the
  exported netlist was correct, the BOM was correct, and DRC reported 0
  violations / 0 unconnected / 0 schematic parity throughout — before and after
  the fix alike.

The reason for that last line is the whole trap, and it is worth stating on its
own: **every symbol renders from the schematic's own `lib_symbols` cache, not
from the library.** Once a part is placed, the sheet no longer needs the library
for anything. So the library can rot completely without a single downstream
artefact changing. Nothing in this project's normal loop — render, netlist, BOM,
fab package, DRC — touches it.

**Why CI never caught it.** The ERC gate in
`.github/workflows/kicad-drc-erc.yml` runs `--severity-error`, and
`lib_symbol_issues` is a **warning**. It was invisible by construction, not by
accident. No amount of running that gate more often would ever have found it.

## What Didn't Work

Every hypothesis below looked plausible, and each one cost time. They are
recorded so nobody re-walks them. All the measurements are against the
pre-fix blob of `kicad/board3/ProPrj_New-easyedapro.kicad_sym`, re-derived for
this write-up.

| Hypothesis | Measurement | Verdict |
|---|---|---|
| **CRLF line endings** | The working-tree file was 28,179 CRLF and 0 bare LF (the stored blob is LF-normalised; `core.autocrlf=true` supplies the CRLF on checkout). Converting a copy to LF changed nothing. | Not the cause |
| **UTF-8 BOM** | Absent — first three bytes are not `EF BB BF`. | Not the cause |
| **Duplicate symbol names** | 94 symbols, 94 unique. | Not the cause |
| **Unbalanced parentheses** | Balanced; the file parses cleanly as s-expressions (the depth walk in `symbol_names()` completes and returns all 94). | Not the cause |
| **Broken or missing `sym-lib-table`** | `kicad/board3/sym-lib-table` is well-formed and `${KIPRJMOD}` resolves to a real file. | Not the cause |
| **A global-table nickname shadow** | The nickname `ProPrj_New-easyedapro` is not present in KiCad's global symbol table, so nothing was shadowing it. | Not the cause |

Two more that look alarming in an EasyEDA-imported library and are **fine**:

- **Non-ASCII symbol names.** Eight names in that library contain non-ASCII
  characters, several of them Chinese — `1*2P卧式排针`, `1*7PIN排间距2.54MM`,
  `KF301-2P接线端子`, `MF1/4W-100KΩ±1% T52`. The library loads with all of them.
- **`/` in symbol names.** `TJA1051T/3/1J`, `17-21SUYC/TR8`,
  `MF1/4W-1KΩ±1%T52` — all fine.

**Only the colon is fatal.** That is the single-sentence takeaway; the rest of
the table exists so the sentence is believable.

One meta-lesson from the list: four of the six eliminated hypotheses were about
the *file as a file* (encoding, endings, syntax) and two were about the *tables*
that name it. None were about the *content of a name*, because a name is data,
not structure — right up until the name is also an identifier.

## Solution

Delete the offending symbol. It was used **zero times** in the schematic, so
removal costs nothing. The fix is on branch `fix/board3-review-section4`, carried
by **PR #25 (open, unmerged as of this writing)**. Note that PR #24 is merged off
the same branch and does *not* contain this commit — its commit list ends before
it. "The PR is merged" and "the commit is on its head branch" are both true here
and compose into a false claim; see
[verifying every part of a claim](../design-patterns/verifying-every-part-of-a-claim-does-not-verify-the-claim.md).

**Verify that by enumerating `(lib_id …)`, not by grepping for the name.** Of
149 `lib_id` references in the `.kicad_sch`, none names it. A grep for the
literal `CAPACITOR_THT:CP_RADIAL` also returns 0 — and is worthless, because
**KiCad escapes the colon as `{colon}`** when it writes the name into a file.
That grep returns 0 for a symbol that *is* placed, too. The escaping is also how
the name survives at all in a format whose identifiers split on `:`, which is
worth knowing before you go looking for it.

The same check turns up something the fix does not touch: the schematic still
carries a `lib_symbols` cache entry for the deleted symbol, written
`ProPrj_New-easyedapro:CAPACITOR_THT{colon}CP_RADIAL_D8.0MM_P2.50MM`. It is
inert — no instance references it — but it is a tidy demonstration of the split
described below: the sheet's copy of a symbol outlives the library's, and
neither one knows about the other.

### The diagnostic command

```bash
kicad-cli sym export svg --symbol <a-real-symbol-from-the-file> \
  -o /tmp/probe kicad/board3/ProPrj_New-easyedapro.kicad_sym
```

On a broken library this prints exactly one line:

```text
Unable to load library
```

Two things make this trap-laden, and the second is not optional:

**1. Read the stdout text, not `$?`.** Direct invocation against KiCad 10.0.5
gives distinct codes:

```text
good library, real symbol        -> "Plotting symbol 'R' unit 1 to ..."   exit 0
good library, nonexistent symbol -> "There is no symbol selected to save." exit 1
broken library, real symbol      -> "Unable to load library"               exit 2
```

But the exit code is easy to lose, and that is the reusable part:

```bash
kicad-cli sym export svg --symbol X -o /tmp/p broken.kicad_sym 2>&1 | cat
echo $?      # 0 -- this is cat's status, not kicad-cli's
```

`$?` after a pipeline is the **last** command's status. Any `| tee`, `| cat`,
`| head` in the probe silently converts a failure into a success unless
`set -o pipefail` is in force — which is exactly how this session first
concluded, wrongly, that the command "exits 0 either way." Matching the text is
robust against both that mistake and a future KiCad renumbering its exit codes.
`tools/kicad_libcheck.py` does that — it tests for the string `"Unable to load
library"` in combined stdout+stderr and ignores the return code entirely.

**2. Control against a known-good library.** Run the same command against
`C:/Program Files/KiCad/10.0/share/kicad/symbols/Device.kicad_sym --symbol R`
first. Without that control you cannot distinguish "this library is broken"
from "I got the invocation wrong," and the second is far more common. A probe
that has never succeeded proves nothing about the case where it fails.

### Result

- ERC **1131 → 1009**, with all 124 `lib_symbol_issues` cleared. The
  arithmetic is a net −122, not −124: two `lib_symbol_mismatch` (L1, U2)
  appeared in their place. Those are pre-existing cache-vs-library drift that
  KiCad could not detect before *because it could not read the library* —
  newly visible, not newly broken. (Later commits on the same PR took ERC from
  1009 to 0, so these two figures are historical to that point in the branch.)
- All **31** symbols the schematic actually draws from that library confirmed
  still present after the removal (31 unique `lib_id`s referenced, 31 found,
  0 missing).
- Library symbol count 94 → 93. DRC unchanged at 0/0/0.

### The second, smaller gap found at the same time

`Board3.kicad_sym` was never registered in `sym-lib-table` while
`Board3:TC2030-IDC-NL` was in use on the sheet — the second ERC complaint,
*"The current configuration does not include the symbol library 'Board3'"*. It
is now the third entry in `kicad/board3/sym-lib-table`. Worth noting that this
is a *different* failure with an *almost identical* ERC message, which is part
of why the block of 124 was easy to write off as one known thing.

## Why This Works

A KiCad LIB_ID is `Nickname:Item`, split on the colon. A symbol name containing
a colon is therefore not a name KiCad can round-trip —
`CAPACITOR_THT:CP_RADIAL_D8.0MM_P2.50MM` inside library
`ProPrj_New-easyedapro` would have to be addressed as
`ProPrj_New-easyedapro:CAPACITOR_THT:CP_RADIAL_D8.0MM_P2.50MM`, which parses as
a symbol called `CAPACITOR_THT` with trailing garbage.

The load is **all-or-nothing**: KiCad rejects the file rather than skipping the
one bad entry. That is the measured behaviour and it is the part that makes the
blast radius absurd — one unused part disables 93 good ones.

And the reason it can survive months undetected is the **cache/library split**.
A `.kicad_sch` carries its own `lib_symbols` block: a full copy of every symbol
it uses, embedded in the sheet. Rendering, the netlist, the BOM and DRC all read
that copy. The library on disk is consulted only when you *place* a new part,
*change* a symbol, or *update from library* — i.e. only during authoring, never
during verification. So the entire verification surface of this project is
structurally incapable of seeing a dead library, and it will stay that way; the
check has to load the library on purpose.

## Prevention

`tools/kicad_libcheck.py` — run it against a KiCad project directory:

```bash
python tools/kicad_libcheck.py kicad/board3
```

Its second check loads the libraries instead of inferring their health from the
design, which is the only way to see this class — checks 4 and 5 do read the
schematic, for the different questions below. Five checks, all of them earned by
something that actually went wrong:

1. **Every library named in `sym-lib-table` / `fp-lib-table` resolves to a path
   that exists**, with `${KIPRJMOD}` expanded to the project directory.
2. **Every symbol library actually loads**, probed through `kicad-cli` with a
   real symbol name taken from the file, judged on the output text.
3. **No symbol or footprint name contains a colon.** This is the preventive
   half: check 2 tells you the library is dead, check 3 names the symbol that
   killed it instead of making you bisect a 442 KB file. The ban is surgical —
   the constant is `:` alone, because everything else the export emits (Chinese
   characters, `/`, `*`, spaces, `%`) was verified harmless.
4. **Every library nickname referenced by the schematic or the board is
   registered in the corresponding table** — the `Board3:TC2030-IDC-NL` gap
   above.
5. **Every instance's fields agree with the symbol its `lib_id` names**
   (watching `Supplier Part`, `Footprint`, `Manufacturer Part`, `Value`). Added
   in the same campaign for an adjacent reason: a GUI *Update Symbols from
   Library* rewrites instance fields from the library, so any divergence is a
   silent revert waiting to happen — and `--schematic-parity` only covers the
   footprint half, so a `Supplier Part` revert would ship to the BOM. 19
   instances diverged when this first ran, 12 of them on `Supplier Part` or
   `Footprint`, including F1 reverting from the 3 A PTC to the 200 mA one.

Current output on the tree (abridged — the run also prints the footprint-library
section and the results of checks 3 to 5; exit 0):

```text
symbol libraries declared   : 3
  ProPrj_New-easyedapro          93 symbols  loads
  JLCImport                      15 symbols  loads
  Board3                          2 symbols  loads
PASS -- every declared library loads, no illegal names, every referenced nickname registered
```

### The CI gate

Wired into `.github/workflows/kicad-drc-erc.yml` as the step **"Every declared
symbol library actually loads"**, placed before the DRC report step — so a dead
library fails the run before anything downstream gets a chance to be
misleadingly green. It is **absolute, not ratcheted**: unlike this workflow's
DRC and ERC gates, which compare against a baseline because the EasyEDA import
carries permanent noise, a library either loads or it does not. There is no
legitimate baseline for "broken."

It has to be its own step rather than a tightening of the ERC gate, for the
reason in the next section.

### Verified in both directions

A check that has never failed is not a check. This one was run against the tree
as it stands (**PASS**) *and* against the library one commit earlier with the
colon symbol restored (**exit 1**, naming both the symptom — "cannot be loaded
by KiCad" — and the cause — "symbol … contains ':'"). Re-confirmed for this
write-up: the pre-fix blob has 94 symbols with exactly one colon in a name; the
current file has 93 and none.

### The general lesson: a severity filter is a blind spot, not just a filter

This is the part that generalises past KiCad. The ERC gate ran
`--severity-error`; the finding was a warning. The check was running, passing,
and *structurally unable* to report the defect — for months, on every push.

The failure mode is worse than having no check, because a green check is taken
as evidence. And the usual instinct is exactly wrong: the workflow's own comment
explains that errors-only exists because *"a check that cries wolf is a check
people learn to merge past"* — the imported board was held to carry warning
noise that no edit could clear. That is why the answer here is neither "raise
the ERC gate to warnings" nor "leave it": it is a **separate, absolute gate for
the specific class that can be judged absolutely.**

**The premise has since been falsified, which strengthens rather than weakens
that conclusion.** The remaining 1009 ERC violations were driven to zero without
a single net moving — see
[a large ERC count is a broken instrument](../developer-experience/a-large-erc-count-is-a-broken-instrument.md),
which is what the "1009 → 0" note above points at. The noise was not inherent;
it was unread. The ERC gate has since been rewritten absolute at
`--severity-all`, so the class described here — a warning nobody's gate could
see — now fails the build directly, and the separate library-load check is a
second line rather than the only one.

When auditing any gate, ask what it is filtering *out*, and whether the class
you most fear lives on the far side of that filter. Applied here that question
has a name: which of this project's defects would produce only a warning?

One implementation note in the script worth surfacing, because it is a generic
trap: `symbol_names()` walks s-expression **depth** rather than matching leading
whitespace, because this project's two symbol libraries disagree about
indentation — the EasyEDA export uses tabs, `kicad_lcsc` uses two spaces. An
`^\t\(symbol` pattern reports `JLCImport` as empty, and a bare `\(symbol`
pattern counts the nested `NAME_1_0` body sub-symbols as real parts. Depth 1 is
the only thing that means "a symbol in this library." This is the same family as
the repo's standing rule to read board facts through `pcbnew` rather than
regex — a text pattern over a structured format is a guess about formatting, and
formatting is exactly what two different exporters will not agree on.

## Related Issues

**Tell this apart from its look-alike first.**
[Headless DRC judges the board plus its sidecar files](../developer-experience/stage-project-sidecars-for-headless-drc.md)
documents the *same* "library not found" message from the opposite cause: there
the checker was misconfigured (unstaged `fp-lib-table`) and the library was
fine; here the checker was right and the library was dead. Both produce a mass
of identical warnings that mask the real comparison underneath. The
discriminator is whether the message survives a correctly-staged run — and that
doc's heuristic *"uniform mass findings with one repeated message are
configuration, not design"* now has this page as its counterexample.

**Upstream provenance.**
[The EasyEDA Pro → KiCad migration lost data silently](easyeda-pro-to-kicad-migration-silent-data-loss.md)
enumerates the other places this import produced something structurally invalid
that no downstream check looks at. This is another member of that set, found
much later.
[lib_footprint_mismatch and the integer-nanometre comparison](kicad-lib-footprint-mismatch-integer-nanometre-comparison.md)
is the footprint-side sibling — a real library-vs-board divergence hiding inside
a warning class — and its prevention rule ("a warning that never clears gets
waved through") is the nearest existing statement of the severity lesson above.

**Same shape, different domain.**
[The holdout net list matched nothing after a hierarchical rename](../logic-errors/holdout-net-list-matched-nothing-after-hierarchical-rename.md)
is the closest structural match in the repo: an identifier-namespace separator
silently invalidating a match, with the failure presenting as absence rather
than as an error.

**On gates and what "clean" means.**
[A gate that cannot pass gets waved through](../conventions/a-gate-that-cannot-pass-gets-waved-through.md)
is the opposite polarity of the same social failure — that doc covers a gate
always red and therefore ignored; this one a gate always green and therefore
trusted.
[DRC-clean and measured is not assemblable](../conventions/drc-clean-and-measured-is-not-assemblable.md)
is this repo's canonical "clean does not mean correct" doc, and this is a fourth
category for its table: not rules, intent, or manufacturability, but *the
verification surface never reading the artefact at all*.

**Method.**
[Calibrate an automated reviewer on a confirmed defect](../design-patterns/calibrate-an-automated-reviewer-on-a-confirmed-defect.md)
is the principle behind two steps of this diagnosis — running the probe against
a stock library to prove the invocation works, and re-running the finished check
against the restored bug to prove it fires.
[Window-filter board geometry by shape intersection](../developer-experience/clip-test-board-window-queries.md)
is the other instance of a documented rule being correct in its example and
wrong at its boundary; here the boundary was "which severities can this gate
see."

Also in `CLAUDE.md`: "Read board facts through `pcbnew`, never regex" (same
family — a query that looks exhaustive but has a blind spot), and the KTD12
GUI-sync constraint, which is what put someone in *Change Symbol* in the first
place.
