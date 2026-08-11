# Grounding Validation (Phase 2.45)

Read this when Phase 2.45 runs. The doc just written becomes permanent, trusted knowledge — future agents will act on its claims without re-verifying them. This phase checks the claims against reality before they compound: a deterministic mechanical pass (bundled script) plus a semantic pass (one read-only validator subagent). Neither pass is a hard gate — every flag is adjudicated, because solution docs legitimately cite deleted paths and pre-fix states.

## Which tree is the ground truth

Two claim categories verify against different trees:

- **Code-behavior claims** (enum values, status semantics, limits, defaults) verify against the **local working tree** — they describe what this session's work produced and verified here.
- **Merge-state claims** ("fixed in #1608", "landed", "shipped") verify against **remote truth** — the checkout may predate a merge, so `gh pr view` (or the tracker equivalent) is primary and local git reachability is only the fallback. The script's `INFO: worktree is N commits behind …` line tells you how much to distrust the local tree for this category.

Before running the script, optionally run `git fetch --quiet` (best-effort — skip silently on failure or offline; the network is never a correctness dependency). When remote state cannot be checked at all, keep the claim, add an as-of qualifier ("as of this writing"), and record degraded verification in the run report.

## Step 1: Adjudicate the mechanical flags

The script reports flags; you decide each one. Three resolutions — **fix**, **annotate**, or **confirm intentional** — never an automatic rewrite and never an automatic pass:

| Flag | Likely meaning | Resolution |
|------|----------------|------------|
| path not found anywhere | Typo, or drafted from memory | Fix the citation or remove the claim |
| path missing here, exists at upstream | Stale checkout | Verify the claim against upstream; annotate if the doc implies the file is present locally |
| path deliberately gone (doc says removed/renamed) | Historical citation | Confirm the surrounding prose marks it as historical ("removed by this fix", "pre-fix state"); add that marker if absent |
| SHA does not resolve | Fabricated or from another repo | Replace with the PR number, or drop |
| SHA reachable from HEAD only | Local-only commit; SHA will change on rebase/squash merge | Replace with the PR number |
| SHA reachable from upstream only | Checkout predates the merge | Keep, with a temporal qualifier; verify the landed claim via `gh` |
| PR merged, but the change is absent from its commit list | Later work pushed to a merged PR's still-live head branch | Contradicted — cite the PR that actually carries the commit (often a later, still-open one), or state the change as unmerged. "Merged" plus "on the head branch" does not entail "in the PR" |
| SHA exists but unreachable | Rebased-away commit | Replace with the PR number |
| scaffold ("Learning 3", `{{…}}`) | Drafting-context leak | Always fix — rewrite as a real path or link |
| relative link unresolved | Wrong target | Fix the path |

If the script cannot be resolved on this platform, apply its checks manually at the same scope — scan the body for cited paths that don't exist, hex SHAs, `Learning(s) N` / `{{…}}` scaffold, and broken relative links — and note in the run output that the check was manual. Do not silently skip.

After any body edit from this step or Step 2, re-run the script until it reports clean or every remaining flag is confirmed intentional.

## Step 2: Semantic validator subagent (Full and headless; skipped in lightweight)

Dispatch **one generic read-only subagent** covering the written solution doc plus any `CONCEPTS.md` entries added or edited this run (Phase 2.4's entries are claims too — a glossary entry written from a session-level summary is exactly how wrong semantics enter the vocabulary). Use the same mid-tier model class as other reviewer subagents when the platform exposes one. Build its prompt from this template:

```
You are a grounding validator for documentation about to enter a permanent
knowledge store. You are read-only: never edit files. Inspect with Read,
Grep, Glob, git (non-mutating), and gh when available.

Inputs: the doc content below, the CONCEPTS.md entries below (if any), and
this staleness context: <INFO line from the mechanical script, or "none">.

Check every factual claim in four categories:

1. CODE-BEHAVIOR CLAIMS — assertions about how code behaves: enum values,
   status semantics, limits, defaults, ordering, state transitions. For
   each, locate the defining source in the current tree and quote the
   defining line(s) with file:line. Verdict: verified (with quote),
   contradicted (with the quote showing otherwise), or unverifiable
   (defining source not found).
   For every piece of evidence, also state what result would have
   appeared if the claim were FALSE. If the answer is "the same result",
   the evidence does not discriminate and the verdict is unverifiable,
   however true the claim may be — find evidence that can come out the
   other way. Evidence of absence (a grep returning 0, a count that
   stayed flat) is where this fails most often.

2. MERGE-STATE CLAIMS — assertions that a change landed ("fixed in",
   "merged", "shipped in", "resolved by #N"). The primary check has TWO
   halves and BOTH must pass:
     (a) did the PR land —
         gh pr view <n> --json state,mergedAt,baseRefName
     (b) is THIS change in it —
         gh pr view <n> --json commits --jq '.commits[].oid'
         and confirm the implementing commit is listed; or
         git merge-base --is-ancestor <commit> origin/<baseRefName>
   (a) ALONE IS INSUFFICIENT and passing it is not evidence for the claim:
   a merged PR's head branch keeps accepting commits, so "the PR is
   merged" and "the commit is on its head branch" can both be true of
   work the PR does not carry. Verdict: verified, contradicted (PR open;
   or merged but the change is absent from its commit list), or
   unverifiable (offline / no gh) — mark unverifiable as "degraded", do
   not guess.
   This check binds at REPORT time, not only here: a live "this PR now
   contains X" status sentence is the same claim and does the same damage
   uninspected (six commits were stranded off a merged PR's branch on the
   strength of one, 2026-08-08 — see the design-patterns doc's live-fire
   section). This validator only ever sees claims that reach a doc; the
   sentence that acts is usually spoken earlier.

3. INTERNAL COMPLETENESS — countable assertions ("six PRs", "three root
   causes", "all N consumers"). Count the substantiating items in the doc
   itself. Verdict: complete, or short (found M of N).

4. MEASURED-FIGURE CLAIMS — any number the doc presents as a current
   property of the artifact: a floor, a baseline, a violation count, a
   timing, a "typical" value. This includes figures the doc inherited
   from another doc, which is where they go stale unnoticed. Re-run the
   measurement now. Verdict: current (quote the command and its output),
   stale (give both numbers), or unverifiable. A figure that cannot be
   re-measured must be restated with the date it was taken rather than
   left in the present tense. Note that a gate justified by such a figure
   is making the same claim, so it expires with it.

Ignore session narrative ("we first tried X") — that describes the
conversation, not the tree. Ignore style.

Return a structured list, one entry per claim checked:
  claim (verbatim) | category | verdict | evidence (quote + file:line, or
  command output) | suggested edit (only for non-verified claims)
```

**Orchestrator handling of verdicts:**

- **contradicted** → fix the doc using the quoted evidence (the quote, not the conversation, is authoritative)
- **unverifiable** (behavior) → soften or attribute: "per this session's conclusion…" — or drop the claim
- **unverifiable/degraded** (merge-state) → keep with an as-of qualifier; record degraded verification in the report
- **short** (completeness) → complete the enumeration or restate the count to match what the doc substantiates
- **verified** → no change

## Reporting

Summarize the phase in one line of the run output (headless report's `Grounding:` line; interactive success output): flags adjudicated (fixed / annotated / confirmed), claims checked, claims softened or corrected, and `degraded — merge-state claims unverified offline` when applicable.
