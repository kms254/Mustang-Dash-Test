---
title: "Audit the fab's revision set before confirming placement"
date: 2026-08-11
category: developer-experience
module: board3-jlcpcb-order
problem_type: developer_experience
component: tooling
severity: high
applies_when:
  - "A fab's parts-placement confirmation presents an engineer-adjusted placement with a revised-items filter"
  - "Deciding whether an engineer's 'modified by our engineering team' flag means fixed, normalized, or broken -- a fix-flag is a finding, not an endorsement"
  - "Auditing placement orientation at scale -- verify the polarized subset, one part per polarity family, against a design-truth table generated from the board file"
  - "A fab share/DFM page is an SPA that returns an empty app shell to plain HTTP fetch"
symptoms:
  - "33 of 142 parts flagged 'modified by our engineering team', mixing the executed assembly remark (U2 rotated to the design's SE pin-1), library-model normalizations, and six of eight LEDs that were already correct"
  - "WebFetch on the jlcpcb.com/smt/dfm-result/share SPA returns the empty shell and reports 'No data / 0 components' -- which reads like a real answer and is not one"
  - "A stale automation Chrome held the chrome-devtools MCP's profile lock; recovery is killing filtered by the MCP's own profile path, never bare chrome.exe"
tags: [jlcpcb, parts-placement, paid-checkpoint, spa, browser-automation, revision-set, pin-1-orientation, board3]
---

# Audit the fab's revision set before confirming placement

## Context

Board3's assembly order SMT026081160502 (2026-08-11), at the third and final
human checkpoint before soldering: JLCPCB's parts-placement confirmation. The
page — an SPA behind a share link — showed the engineer-adjusted placement under
the banner "The image shows the parts placement adjusted by our engineer. Please
verify the orientation and position of the components," with 142 parts assembled,
0 unassembled, and a clean per-row DFM column. It arrived after the assembly
remark about U2 from the fab-viewer episode
([derive-the-fab-viewers-rule-before-trusting-its-outlier.md](derive-the-fab-viewers-rule-before-trusting-its-outlier.md)),
so one known question was already riding on it: did the engineer act on the
remark, and did anything else move while they were in there?

The checkpoint has three problems stacked on top of each other. The page is
invisible to naive fetchers — WebFetch returns a syntactically valid "No data,
0 components" that is actually the app shell, not the answer. The review is too
big to do exhaustively by eye — 142 parts, of which 33 carried the flag "This
item has been modified by our engineering team. Please review." And the flagged
set genuinely contains both signs: the fix we asked for, and "fixes" applied to
parts that were already correct. This session solved all three; the boards were
released to the line on the result. The order's checkpoint history lives in the
Board state block of [fab/ORDER.md](../../../fab/ORDER.md).

## Guidance

### 1. Read the SPA with browser automation, not a fetcher

WebFetch and plain HTTP against the share URL return the application shell:
"No data", 0 components. That is a loud non-answer that parses as data — the
same failure shape as a wrong-layer check, where the instrument reports cleanly
about the wrong thing. Treat any "empty" fab page from a fetcher as unread, not
as empty.

The chrome-devtools MCP (Playwright-equivalent) gets everything:

- `new_page` the share URL, then `wait_for` a designator string (any reference
  you know is on the board) so you block until the app has actually hydrated.
- The **accessibility snapshot yields the entire 142-row parts table as plain
  text** — designator, comment, footprint, LCSC number, side, and type per row.
  No scraping, no DOM spelunking; the a11y tree is the readable projection of
  the SPA.
- **Clicking a table row zooms the canvas viewer to that part**, and a viewport
  screenshot at that zoom is readable at pin-1-corner detail. That click-zoom
  is what makes a targeted audit cheap: one click, one screenshot, one verdict.

Recovery detail, paid for live: if the MCP reports "browser is already running
for .../chrome-devtools-mcp/chrome-profile", a stale automation Chrome holds the
profile lock. Kill chrome.exe processes **filtered by that profile path in their
command line** — never a bare chrome.exe kill, because the user's real browser
is a different process set and takes the collateral.

### 2. "Show Revised Items" is the engineer's diff — adjudicate it per-finding

The page's revised-items filter is exactly a diff: what the fab's engineer
changed against what you uploaded. On this order it selected 33 of 142 parts,
and the set decoded three ways, each needing different handling:

- **(a) Your own remark, executed.** U2 appeared on the list, and its render now
  shows pin 1 at the design's SE corner — the assembly remark from the viewer
  episode, acted on. This is the case you *want* on the list, and it is why the
  list cannot simply be dismissed.
- **(b) Library-model normalizations.** Harmless on non-polar passives, which
  cannot be hurt by rotation. The bulk of the 33.
- **(c) The risk case: engineer "fixes" applied to parts that were already
  correct.** Six of the eight LEDs were on the list. A normalization against a
  bad library datum would have broken correct polarity on six parts at once —
  silently, uniformly, and with a green DFM column.

A fab engineer's fix-flag is a finding like any other instrument output. Neither
trust nor distrust transfers wholesale across the set: the same 33-item list
contained the executed fix we asked for and six parts that were correct before
revision. Adjudicate each family on its own evidence.

### 3. Close the whole set by sampling its polarized subset against a design-truth table

Do not audit 33 items item-by-item, and do not audit orientation by eye against
memory. Two steps:

**Generate the design-truth compass table from the board file.** For each
polarized part, compute where pad 1 (or the cathode) sits relative to the
footprint center, in the board frame, and map it to N/S/E/W. The recipe is the
same pcbnew dump pattern the U2 investigation used: load the board, take the
pad's position minus the footprint's center, bucket the vector by compass
direction. (This session's dump script and the per-part screenshots live in
session scratch and are not committed — the recipe is the durable part, not the
artifact.)

**Then verify one representative per polarity family in the viewer.** The
justifying observation: under a library normalization, each polarity family is
either all-correct or all-wrong — the engineer's tooling applies one rule per
library entry, not per instance — so one representative per family is decisive.
Choose representatives to cover every distinct way the revision could break the
board:

- the remark target first (U2 — the one part with a known open question),
- one LED per cathode-direction family (not one LED total — the two families
  can fail independently),
- one of each flippable package family — SOIC, SOT-23-6, QFN — because packages
  with symmetric pad rows *can* be soldered 180° off and no electrical check
  (ERC, DRC, parity, netlist) can see it,
- one discrete diode, and the crystal.

Eight clicks closed all 33 flagged items. Every sampled family came back
correct — see the table under Examples — the checkpoint was confirmed, and the
boards were released to the line.

## Why This Matters

The placement confirmation is the last gate before soldering. Past it, an
orientation error is a batch of boards with a backwards flash chip or six
reversed LEDs, discoverable only at bring-up and fixable only with a rework
station. And the gate's page is doubly hostile: unreadable to naive fetchers
(the SPA shell reads as "no data" — an answer-shaped absence) and too big to
audit exhaustively by eye.

The family-sampling method converts an intractable-looking review into minutes
while covering every mechanism by which the revision could break the board. It
works because the unit of failure is the library entry, not the part instance:
sample the family, and the family's verdict is total.

The per-finding adjudication matters because the engineer's diff genuinely
carries both signs. Trusting it wholesale means six LEDs re-normalized against
whatever datum the engineer's library holds; reverting it wholesale means
undoing the U2 fix you explicitly asked for. Neither blanket policy survives
contact with this one 33-item list.

This closes the checkpoint trilogy, and the three lessons are one lesson at
three gates: the fab's instruments are instruments, to be calibrated before
believed. The 3D viewer was an **untrustworthy instrument** — its U2 model was a
stand-in that could neither confirm nor deny
([derive-the-fab-viewers-rule-before-trusting-its-outlier.md](derive-the-fab-viewers-rule-before-trusting-its-outlier.md)).
The production file was an **out-informing instrument** — JLC's CAM job knew
things about the board that the design files did not say
([diff-the-fabs-production-file-before-confirming-the-order.md](diff-the-fabs-production-file-before-confirming-the-order.md)).
The placement confirmation is **the engineer's diff as instrument** — real
findings and false fixes on the same list, separable only by checking each
family against design truth derived from the board file itself.

## When to Apply

- Any fab parts-placement or DFM confirmation page, from any contract
  manufacturer — the banner text will differ; the shape (engineer-adjusted
  render + revised-items diff + confirm button) is the trigger.
- Any SPA that a fetcher reports as empty. "No data, 0 components" from
  WebFetch is the shell, not the page — a wrong-layer-check analog, a loud
  non-answer misreadable as data. Re-read it through browser automation before
  concluding anything from it.
- Any bulk "we adjusted your files" flag from a fab or CM — placement
  revisions, CAM edits, footprint substitutions. The set is a diff; adjudicate
  it per-finding, sampling by the equivalence class the adjustment tooling
  actually operates on (library entry, aperture, drill class).

## Examples

**The WebFetch-vs-browser contrast, same URL.** WebFetch: "No data", 0
components — a complete, well-formed, wrong answer. chrome-devtools MCP:
the full 142-row parts table out of the accessibility snapshot, plus a
click-zoomable canvas viewer readable at pin-1 detail.

**The 8-click audit table** (family → representative → design truth → observed
→ verdict):

| Family | Representative | Design truth (board frame) | Observed in viewer | Verdict |
|---|---|---|---|---|
| Remark target, WSON-8 | U2 | pin 1 SE | pin 1 SE | correct — remark executed |
| North-cathode LEDs (6) | LED1 | anode S | anode S | correct |
| South-cathode LEDs (2) | LED7 | anode N | anode N | correct |
| SOIC-8 | U7 | pin 1 NW | pin 1 NW | correct |
| SOT-23-6 | U5 | pin 1 NW | pin 1 NW | correct |
| QFN | U11 | pin 1 NW | pin 1 NW | correct |
| Discrete diode | D10 | anode W | anode W | correct |
| Crystal | X1 | pin 1 SW | pin 1 SW | correct |

Eight representatives, 33 flagged items closed, checkpoint confirmed, boards
released.

**The U2 before/after.** Fab-viewer episode: JLC's undersized stand-in model
rendered pin 1 at NE, where the design puts it SE — testimony inadmissible
either way. Assembly remark attached to the order: pin 1 must land on the SE
pad, per the Winbond datasheet. Placement confirmation: U2 on the revised-items
list, render showing pin 1 SE — the remark acted on, verified at the gate it
was written for.

Order context and checkpoint status: the Board state block of
[fab/ORDER.md](../../../fab/ORDER.md). Related PRs: #36 (the P1/P2 rotation the
viewer caught), #39 (the viewer-episode write-up), #40 and #41 (open as of this
writing).

## Related

- [derive the fab viewer's rule before trusting its outlier](derive-the-fab-viewers-rule-before-trusting-its-outlier.md)
  — trilogy doc 1, the same page one visit earlier: this checkpoint is the
  execution of its rung 5 close-out, and the written U2 remark surfaced here in
  the engineer's own revision set — the remark was honored.
- [diff the fab's production file before confirming the order](diff-the-fabs-production-file-before-confirming-the-order.md)
  — trilogy doc 2, the other paid checkpoint: the same adjudication frame
  (every fab-produced delta is adopt-or-challenge), applied there to a CAM zip
  and here to a revision set delivered through a UI filter.
- [verifying every part of a claim does not verify the claim](../design-patterns/verifying-every-part-of-a-claim-does-not-verify-the-claim.md)
  — why eight clicks entail 33 verdicts: within a polarity family the placements
  share one library footprint and one polarity convention, so a representative's
  evidence *would* look different if the family were wrong — the sampling is
  sound by its test, not by optimism.
- [calibrate an automated reviewer on a confirmed defect](../design-patterns/calibrate-an-automated-reviewer-on-a-confirmed-defect.md)
  — the design-truth compass table is this pattern's move: establish ground
  truth from the board file before reading any instrument's report.
- `fab/ORDER.md` board-state block — the operational record: this checkpoint
  marked DONE 2026-08-11 with the audit outcome; the only remaining watch is
  the possible X1 crystal-handling fee.
