---
title: "An identity mapping between two namespaces is the absence of a mapping"
date: 2026-08-20
category: design-patterns
module: dash-telltales
problem_type: design_pattern
component: tooling
severity: high
applies_when:
  - "two enums, tables, or index spaces have the same cardinality and code walks one with the other's index"
  - "a lookup table would be written as `x[i] = i` (or `bit l -> thing l+1`) and therefore is not written at all"
  - "the correctness of an index correspondence cannot be checked without hardware, a vendor response, or some other absent thing"
  - "adding a value to an enum whose length another module's array depends on"
tags:
  - index-aliasing
  - lookup-table
  - enum
  - placeholder
  - firmware
  - telltales
  - type-safety
  - untestable-defaults
---

# An identity mapping between two namespaces is the absence of a mapping

## Context

Board3's eight telltale lamps and the firmware's eight warning conditions
are unrelated namespaces that happened to have the same cardinality. The
firmware bridged them with the obvious thing: condition bit `l` drove
lamp `TT(l+1)`. Nobody wrote that down as a decision, because it does not
look like one — an identity mapping looks like the natural order of
things rather than a claim about the world.

It was wrong in all eight rows, and it could not have been discovered
before the LEDs existed. Both sides were internally consistent, every
host test passed, and the physical meaning of "lamp 3" lived only in a
schematic and a bezel drawing. The defect surfaced within minutes of the
LEDs first lighting: a forced oil-pressure alarm lit the **green left
blinker**, and a real low-fuel condition lit **a red**. On a car, the
first of those means an engine-destroying fault announces itself as a
turn signal.

## Guidance

**When two index spaces are independent, write the mapping down even when
it is currently the identity — and give each space its own count.**

Three concrete moves, all visible in `MustangDash/dash_telltales.h`:

1. **Name the two spaces separately.** `DASH_LAMP_COUNT`
   (`dash_telltales.h:37`) counts warning *conditions*; `DASH_TT_COUNT`
   (`:72`) counts physical lamp *positions*. Both are 8, and the comment
   at the definition says so explicitly — the point is that they are
   equal by coincidence, not by construction.

2. **Make the correspondence a table, not an assumption.**
   `DASH_LAMP_TT[]` (`:74`) maps each condition to a position or to
   `DASH_TT_NONE`. Writing it forced the question "where does coolant
   light?" to be answered out loud — the answer turned out to be
   "nowhere, deliberately."

3. **Separate the layers that were conflated.**
   `dash_telltale_conditions()` (`:87`) computes what is wrong;
   `dash_telltale_mask()` (`:138`) decides which lamps light. The
   original single function did both and could not distinguish them,
   which is why no test could catch the error.

Then **pin the assumptions that remain** with assertions rather than
comments. Where two position-indexed tables must agree,
`MustangDash.ino:506` says so:

```c
static_assert(DASH_CAL_POSITIONS == DASH_TT_COUNT,
              "calibration table and lamp positions must be the same length");
```

And where the mapping itself must hold, test the *specific* wrong answer,
not just the right one. `tests/test_dash_telltales.c` asserts that low
oil pressure lights TT6 **and explicitly does not light TT1** — naming
the exact defect the bench found, so a future edit that reintroduces it
fails loudly rather than passing a vaguer assertion.

## Why This Matters

**An identity mapping is invisible to review.** Reviewers see
`lamp_set(l, mask >> l & 1)` and read it as plumbing, not as an
assertion that two independent orderings agree. There is nothing to
disagree with, which is precisely the problem: the claim is never stated,
so it is never examined.

**Equal cardinality is a trap that gets worse silently.** While the two
counts match, every array indexed by the wrong space still works. The day
someone adds a ninth warning condition, the hardware tables — sized from
the condition count — silently grow a garbage row, or the loop walks one
element past a table that did not grow. The failure appears far from the
edit that caused it, in code nobody touched.

**"Untestable" is not the same as "fine."** The mapping's correctness
lived outside the software entirely: in a schematic, a parts list, and a
bezel. A placeholder that cannot be checked is not a default — it is an
unexploded defect with a plausible-looking implementation, and it will
keep passing every test until the missing half of the world shows up.

**The general shape:** whenever you find yourself *not* writing a lookup
table because it would be `x[i] = i`, that is the moment to write it.
The table costs a few lines; its absence costs the ability to be wrong
out loud.

## When to Apply

- Two enums, tables, or index spaces with equal cardinality, where one is
  used to index the other. Especially when one side is defined by
  hardware, a spec, or another team.
- Any correspondence whose truth lives outside the code — a pin map, a
  connector's pin order, a CAN signal layout, a UI element order, a
  database column order.
- Before shipping a mapping that no test can currently verify: either
  find a way to assert it, or record in a comment what would have to be
  true and how it will eventually be checked.
- When adding a member to an enum, grep for arrays sized by its count and
  confirm each one is indexed by *that* space.

## Examples

**Before** — the mapping exists only in the shape of the loop, and is
wrong in every row:

```c
/* condition bit l drives TT(l+1) -- never stated as a decision */
for (uint8_t l = 0U; l < DASH_LAMP_COUNT; l++) {
    dash_lamp_set(l, 0U != ((mask >> l) & 1U));
}
```

**After** — the correspondence is data, and the two spaces are counted
separately (`dash_telltales.h:74`):

```c
#define DASH_TT_NONE 0xFFU /* condition has no lamp; screens only */

static const uint8_t DASH_LAMP_TT[DASH_LAMP_COUNT] = {
    5U,           /* OILP  -> TT6 red    (right cluster, bottom-left) */
    DASH_TT_NONE, /* OILT  -> screens */
    DASH_TT_NONE, /* CLT   -> screens (alarm takeover covers an overheat) */
    DASH_TT_NONE, /* VOLTS -> screens */
    DASH_TT_NONE, /* FUELP -> screens */
    4U,           /* FUEL  -> TT5 orange (left cluster, bottom-left) */
    DASH_TT_NONE, /* AFR   -> screens */
    DASH_TT_NONE, /* SHIFT -> screens */
};
```

Writing the table did more than fix the indices. Six of eight rows turned
out to be `DASH_TT_NONE` — the board's lamps mostly carry body signals
(blinkers, headlights, high beam) that the firmware does not compute at
all. The identity mapping had been quietly asserting that eight warning
conditions and eight cluster lamps were the same eight things. They
overlap in two.

Shipped in PR kms254/Mustang-Dash-Test#48 (open as of this writing);
the resulting cluster legend is
`docs/hardware/board3-telltale-legend.md`.

## Related

- `docs/solutions/build-errors/an-unguarded-header-define-silently-stomps-a-build-flag.md`
  — the same week, the same family: a value that looked settled
  (`-D HSE_VALUE`) was silently replaced, and the compiler's warning went
  unread. There the claim was overwritten; here it was never made.
- `docs/solutions/design-patterns/verifying-every-part-of-a-claim-does-not-verify-the-claim.md`
  — companion failure of evidence: each half checked out, the whole was
  still wrong.
- `docs/solutions/conventions/a-gate-that-cannot-pass-gets-waved-through.md`
  — a check that cannot fail stops being a check; an assertion that is
  never stated was never a check at all.
