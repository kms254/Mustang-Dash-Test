---
title: A scaled-down preview is different evidence, not weaker evidence
date: 2026-08-22
category: design-patterns
module: splash-render
problem_type: design_pattern
component: development_workflow
severity: high
root_cause: missing_tooling
resolution_type: tooling_addition
applies_when:
  - an artifact will be judged by eye on target hardware - a rendered surface, a gradient, an animation, an LED colour - and previews or mock-ups are the only feedback so far
  - several candidates are in play and choosing between them currently costs one build-and-flash cycle each
  - the thing to be judged is transient on the device (a 2 s boot animation, a fault flash) and cannot be held still long enough to look at
  - a computed constant governs a perceptual effect - a falloff, a brightness, a spread - and has only been checked on paper
  - deciding whether bench-only scaffolding earns its place in firmware, or whether a locked design means it now comes out
symptoms:
  - A candidate that read as convincing in a scaled preview reads as something else entirely at 1:1 - engine-turned jeweling rendered as bubbles
  - Rejections arrive one per flash cycle, and each round produces another candidate rather than a decision
  - Each fix reveals an artefact the previous one was masking - block stepping, then colour banding, then dither blobs
  - A constant expressed against the wrong denominator looks correct on paper and is visibly wrong on the panels
  - The preview channel itself fails to render for the reviewer, so nothing has actually been looked at on the target
related_components:
  - MustangDash/splash_render.h
  - MustangDash/splash_config.h
  - MustangDash/dash_serial.h
  - MustangDash/MustangDash.ino
  - tools/make_material.py
tags:
  - splash
  - on-target-evaluation
  - bench-harness
  - perceptual-design
  - ram-g
  - astc
  - eve
  - scaffolding
---

# A scaled-down preview is different evidence, not weaker evidence

## Context

The splash background for the three-panel cluster took five surface candidates, three bitmap encodings, and one measured framebuffer property before it settled — and every one of those decisions was made wrong at least once from a preview image, then made right on glass. The work is on `feat/splash-metal-cluster-background`, **PR #52, open**, six commits from `origin/main`.

The thing being designed is a lit metallic field spanning three 1024×600 BT817 panels behind a 2000 ms boot animation. Nothing about it is verifiable by inspection: it is a *perceptual* artifact whose failure modes are contour banding, tiling repetition, block-boundary steps, dither mottling, and — in the worst case here — reading as an entirely different physical object than the one intended.

The five candidates were built and rejected on the real cluster: engine-turned jeweling at two scales, brushed, a full-panel non-repeating anodised field, and the original carbon re-derived (the commit "fix(splash): the background has no texture, and the gradient took three encodings", PR #52). Every one of them had looked acceptable, or at least arguable, as a PNG on a monitor.

Two mechanisms made the on-glass loop cheap enough to actually run, both temporary bench serial commands:

- **`mat on|off`** held the splash background on every panel indefinitely, with no dash drawn on top, so a candidate could be looked at for as long as it took. Its own comment said why it existed: *"The background is a judgement call that a scaled-down preview gets wrong -- one candidate read as plausible texture in a mock-up and as bubbles on glass -- and the splash is only 2 s long."* (`git show <the cleanup commit>^:MustangDash/dash_serial.h`, the `DASH_CMD_MAT` enum entry.)
- **`splash`** replays the whole boot animation on demand — same reasoning one level up, since the animation crossfades away after 2 s and a power cycle per look is worse.

`mat` was introduced alongside the textureless background and **deliberately deleted** by the commit *"refactor(splash): remove the background-comparison scaffolding"* (PR #52 — 22 files changed, 40 insertions, 238 deletions) once the design locked. It does not exist at HEAD, which is the point: find it with `git log --oneline -S 'DASH_CMD_MAT' -- MustangDash/`, which returns both the commit that added it and the one that removed it, then read the pre-cleanup revision of `MustangDash/dash_serial.h` and `MustangDash/MustangDash.ino`. (`--diff-filter=D` does *not* work here: it selects commits where the FILE was deleted, and both files still exist, so it returns empty with exit 0 — a recipe that fails silently is worse than no recipe.) `splash` stayed and is live in `MustangDash/dash_serial.h`.

## Guidance

### 1. Treat a downscaled preview as a different measurement, not a smaller one

Downsampling is a low-pass filter. The failure modes that decide whether a rendered surface is acceptable at 1:1 are *precisely the high-frequency structure a downscale destroys*: block boundaries, dither cells, tiling seams, quantisation contours, and the fine texture that distinguishes "machined" from "noise". A preview does not under-report those; it removes them and reports the remainder confidently.

The sharpest instance here inverted the meaning of the image entirely. Engine-turned jeweling read as plausible machined aluminium as a PNG and as **bubbles** on the panel. The cause was found only afterwards and only because someone was looking at it full-size: each disc carried a radial brightness falloff *and* an angular sheen gradient — which is exactly how you shade a sphere. Flattening the brightness, crisping the edges and letting the scratches dominate produced a legible machined motif, which was then rejected for a different reason: a 128 px tile repeats **40 times** across a 1024×600 panel (8 across × 5 down), and no amount of tile tuning fixes repetition.

### 2. Build the re-run harness before you build the third candidate

The economics are what make this a rule rather than a preference. Without a harness, each look costs a rebuild, a flash, a boot, and a 2000 ms window before the animation crossfades away. That is a per-look cost high enough to silently discourage looking — which is how a bad choice survives an inspection that technically happened.

The harness does not remove the rebuild — a new candidate still means regenerating the pack and reflashing. What it removes is the **2000 ms window**, and that is the cost that actually stops people looking: an artifact you can study for a minute gets judged, one that vanishes after two seconds gets guessed at. RAM_G headroom is what made even that affordable; the centre panel sat at 469,312 B of 1,048,576 before the L8 glow landed and 608,064 B after, so the background layer was never the constraint.

**A caution about this very claim, which is the sharpest thing in this document.** A live variant of the harness *did* stage several candidates at once and switch between them over serial, and it is what made comparing five surfaces practical. It was never committed — it lived in the working tree and was collapsed away before the first commit — so **no revision in the repository contains it** (`git log --all -S 'SPLASH_MATERIAL_COUNT' -- MustangDash/` returns nothing). The commit message in PR #52 nevertheless asserts that staging every candidate turned five flash cycles into one, and the first draft of this document repeated it, because both were written from memory of the session rather than from the artifact. Scaffolding you delete without committing cannot be cited afterwards; if the harness taught you something, the commit is the only place that survives. **The harness is nearly free whenever the target has headroom, and the cost of not building it is paid every single time you look.**

The same argument scales up a level: `splash` exists because a 2 s animation that plays once at power-up cannot be judged, and a power cycle per look is worse than a rebuild per look was. the commit "feat(splash): the glow reaches across the whole cluster" (PR #52) states the general form directly: *"if a thing can only be judged on hardware, it needs to be re-runnable on hardware."*

### 3. Measure the output stage before blaming the asset

Three encodings were tried for one smooth gradient, and each one exposed a cause the previous one had masked. The reasoning is preserved verbatim in `encode_l8()` in `tools/make_splash_flash.py`:

```
      ASTC   -- a BLOCK codec. A gradient is exactly what it handles worst,
                and this asset is magnified on screen, so every block
                boundary became a visible step.
      RGB565 -- no blocks, but only 32 blue levels, which bands a dark blue
                ramp on its own. Dithering the SOURCE does not fix that,
                because the source is magnified: the dither cells become
                blobs and read as diagonal mottling. Dither only works at
                output resolution.
      L8     -- 8 bits of ALPHA (256 levels) with the colour supplied by
                COLOR_RGB. The ramp is 8x finer than RGB565's blue, there is
                nothing to block-quantise, EVE interpolates the alpha in
                hardware when magnifying, and it is half the size. No dither
                needed anywhere.
```

Even a perfect asset still banded, because the **last quantiser is the framebuffer** and nobody had checked it. `REG_OUTBITS` reads 0 (8 bits) and `REG_DITHER` reads 1 on all three panels — and critically, those were *measured*, read back live through the `diag` command rather than assumed:

```c
Serial.printf("  panel %-6s REG_OUTBITS 0x%03X REG_DITHER %u\r\n",
              kNames[p],
              (unsigned)EVE_memRead16(REG_OUTBITS),
              (unsigned)EVE_memRead8(REG_DITHER));
```
(the `diag` handler in `MustangDash/MustangDash.ino`. The comment above it records why: the vendored library configures those registers only for an unrelated board, *"so they sit at reset defaults nobody had ever checked."*)

An 8-bit ramp across 600 px still contours regardless of source precision. The fix is a 64×64 noise tile drawn **1:1 with `EVE_REPEAT` and `EVE_NEAREST`** at an amplitude of ~3 output levels (`SPLASH_DITHER_A 3U`, `SPLASH_DITHER_A` in `MustangDash/splash_render.h`, applied at line 321). **Dither belongs at output resolution.** Dithering the magnified source is the failure this replaced — the same 1:1-versus-magnified distinction as the metal tile, arrived at independently.

### 4. Express a perceptual constant in the units of the thing being perceived

A constant can compute correctly and still be wrong on glass, and the framing of the constant is often why. The glow hotspot's horizontal sigma was ~7% of cluster width — a defensible number that put the entire falloff inside the centre panel and left the sides lit only by the vertical term, reading as two dim screens flanking a bright one.

It is now `GLOW_SPREAD_X = 0.34` (`GLOW_SPREAD_X` in `tools/make_material.py`), and the more durable half of that change is the *denominator*. The old value was buried as `0.115 * 1.9` **against panel width**; the design object is one light source seen across three screens, so panel width was the wrong unit to think in. The comment now says so explicitly, and records the boundary of the design space too: past roughly 0.6 the cluster stops reading as lit-from-centre and becomes uniformly blue — a legitimate but different design, worth choosing rather than drifting into.

### 5. Budget the scaffolding's removal as part of the work, and verify the removal on the same instrument

`mat` was removed the moment the design locked, along with `tools/make_splash_assets.py` (deleted by this work), `assets/splash/reference/` (deleted by this work), 8.1 MB of retired art, and the dead engine-turned generator code. That is correct: the tool held five candidates and there is now one background, and `splash` covers looking at the result.

The removal introduced its own defect, and it is instructive that **the compiler could not see it**. A regex that removed the `mat` parser ran from a comment to the next blank line, and the `splash` parser had been inserted directly after that comment — so it went too. The `DASH_CMD_SPLASH` enum entry survived, so the build was clean, and the failure appeared only at runtime on the board as `err unknown command`. It was fixed by re-verifying against the **live command surface** rather than the build log. (A fresh instance of `docs/solutions/conventions/a-correction-is-an-unreviewed-change.md` — the pattern is documented there.)

### 6. A cheap loop is not a substitute for looking up what already works

This is the limit that keeps the rest honest. The harness made iteration nearly
free, and five candidates were iterated. What actually settled the question was
neither the sixth candidate nor a better preview: it was fifteen minutes of
reading what shipping premium clusters do, which returned a one-sided answer —
they are plain dark backgrounds, and a patterned cluster background is the
aftermarket tell. The commit says it plainly: *"one search would have saved five
rounds"* (the commit "fix(splash): the background has no texture, and the gradient took three encodings", PR #52).

So the ordering is: **establish what the answer probably is, then build the
harness to choose among the survivors.** A cheap loop makes a wide search
affordable, which is exactly what makes it tempting to substitute for a narrow
one. The harness answers *which of these*, never *should it be any of these*.

The repo also already held prior art nobody found. `docs/solutions/ui-bugs/boot-splash-radial-gradient-banding-double-quantization.md`
(2026-07-10) documented banding on this same splash gradient — and its
prevention bullet prescribes *"pair ordered dither with a bilinear (or better)
upscale … the filter averages the dither back toward the true value."* That is
precisely what was tried here, and on glass it produced visible diagonal
mottling. The ancestor validated the technique at a **2x** upscale with an 8x8
Bayer cell — the same geometry the shipping glow uses — so the prescription is
not wrong as measured. What broke it was ratio: the glow was authored at 1/16
scale at the time, and at 16x one dither cell spans well over a hundred screen
pixels, which bilinear cannot demodulate because it averages source texels, not
screen ones. The advice holds at the ratio it was measured at and inverts well
above it, which is why that doc is flagged for refresh rather than cited as
support.

## Why This Matters

The general shape is a mismatch between where a judgement is *made* and where the artifact is *consumed*. Any intermediate representation — a downscale, a mock-up, a screenshot, a host-side render — is a lossy channel, and for perceptual work the loss is concentrated exactly in the band that carries the decision. That is why "the preview looked fine" and "it is wrong on the panel" are not in tension: both are accurate reports about different signals.

On this branch the preview channel eventually failed *completely* — the candidate images did not render for the reviewer at all — which is what forced the decision onto the panel. In hindsight that should have been the first move rather than the fallback, and it would have saved most of five rounds.

There is a second, quieter cost. When looking is expensive, looking gets skipped, and every skipped look is a decision made on the cheap channel by default. A harness does not just make verification possible; it changes which verification is the path of least resistance. `mat` and `splash` cost a few dozen lines each and one afternoon's RAM_G headroom, against five reflash cycles per comparison round.

Finally, the framebuffer measurement is the reason the whole chain terminated. Three asset-side fixes each improved the gradient and none of them could have finished the job, because the last quantiser was downstream of every asset. **A pipeline is only as precise as its output stage, and the output stage is the one nobody wrote.**

## When to Apply

- **Before building the harness at all.** Spend fifteen minutes on what shipping products already do. The harness answers *which of these*; it never answers *should it be any of these*, and a cheap loop is exactly what makes substituting the first question for the second tempting.
- **Any artifact judged perceptually on target hardware** — display surfaces, animation timing, LED colour and brightness, backlight curves, audio. If the acceptance criterion is "does it look/sound right", the target is the only valid instrument.
- **When the target has spare storage for more than one candidate.** Stage them all; the switching cost drops to a command and the comparison becomes side-by-side rather than serial-with-memory.
- **When an artifact is magnified, tiled, or requantised between authoring and display.** Every one of those transforms has a characteristic artifact that a preview at a different scale cannot show.
- **When a computed constant governs something perceived.** Check its denominator against the object actually being perceived, and check the ends of its useful range so the chosen value is a choice.
- **When removing scaffolding.** Verify against the running system's live surface, not the build. Deletions that leave a declaration behind compile clean.

Do **not** reach for this when the acceptance criterion is numeric and host-checkable — geometry, checksums, timing budgets, byte layouts. Those belong in the host test suite, which is faster and does not need a board. The splash pack's structural invariants (L8 stride, uncompressed size, power-of-two dither tile, no asset full-panel) are host-tested for exactly that reason; only the *appearance* required glass.

## Examples

**The bench hold that made the loop affordable** (removed by the scaffolding-cleanup commit in PR #52; recover it with `git log -p -S 'g_bg_hold' -- MustangDash/MustangDash.ino`):

```c
if (g_bg_hold)
{
    /* bench hold (`mat`): the splash background on every panel, with
     * no dash on top, so a candidate material can be judged for
     * longer than the 2 s splash allows */
    const ThemeDesc *theme = &THEMES[g_theme];
    if (dash_select_panel(DASH_PANEL_CENTER))
    {
        eve_frame_begin(0x000000UL);
        EVE_color_rgb(0xFFFFFFUL);
        draw_splash_background(theme, DASH_PANEL_CENTER, 255U);
        eve_frame_end();
    }
    dash_sides_frame(0U, theme, 255U, 0x000000UL);
}
```

Note what it does *not* do: it does not render a candidate to a file, and it does not shorten the splash. It removes the time limit entirely, on every panel at once, at the real resolution.

**Output-resolution dither, drawn 1:1** (the dither block in `draw_splash_background()`, `MustangDash/splash_render.h`) — the `EVE_NEAREST, EVE_REPEAT, EVE_REPEAT` triple is the whole point; nothing about this tile may be magnified:

```c
EVE_cmd_dl(COLOR_A((uint8_t)(((uint16_t)alpha * SPLASH_DITHER_A) / 255U)));
EVE_cmd_dl(BLEND_FUNC(EVE_SRC_ALPHA, EVE_ONE));
EVE_cmd_setbitmap(dith_src, (uint16_t)dith->fmt, dith->w, dith->h);
EVE_cmd_dl(BITMAP_SIZE(EVE_NEAREST, EVE_REPEAT, EVE_REPEAT, ...));
```

The generator states the causal chain (`tools/make_material.py`, `dither_tile()`): *"REG_OUTBITS is 8 bits, so the FRAMEBUFFER quantises after every asset: a ~106-level ramp across 600 px steps every ~6 px and shows contour rings. No amount of source precision fixes that, because the quantisation happens downstream of the asset."*

**The constant re-expressed in cluster units** (the `GLOW_SPREAD_X` block in `tools/make_material.py`):

```python
# Horizontal spread of the hotspot, as a fraction of the WHOLE CLUSTER width --
# not of one panel, because the point is a single light source seen across
# three screens.
GLOW_SPREAD_X = 0.34
```

**The negative result is also a result.** After five candidates, what settled it was prior art rather than a sixth generation: shipping premium clusters use plain dark backgrounds, and a patterned cluster background is the aftermarket tell. That reasoning is now load-bearing enough to live in the renderer itself (the layer comment above `draw_splash_background()`), so the next person does not re-derive five textures to reach the same place. Real carbon went on the bezel, which is where the car has it.

---

## Related

**The nearest neighbour — read the split, do not fold them.**
- [Query the running device before theorising](../developer-experience/query-the-running-device-before-theorising.md)
  — the *diagnosis* half of the same instinct: read the status surface the
  device already exposes instead of reasoning about it. This is the *judgement*
  half, where no field answers the question because the question is perceptual,
  so the surface has to be built. That doc's own When-to-Apply excludes the
  case where the surface itself is under test, which is exactly this one.

**The prior instance — this is a pattern, not a one-off.**
- [An identity mapping is the absence of a mapping](an-identity-mapping-is-the-absence-of-a-mapping.md)
  — two days earlier, a lamp mapping no test could verify was closed by a bench
  command that forced each physical position so it could be judged by eye. Same
  countermeasure, different subsystem, and the reason this is written as a
  repeatable shape rather than a splash anecdote.

**Contradicted or narrowed by this round.**
- [Boot-splash radial gradient banding: double quantization](../ui-bugs/boot-splash-radial-gradient-banding-double-quantization.md)
  — the ancestor. Its two-quantizer model is missing the one downstream of every
  asset: the framebuffer's own output depth, which no asset-side choice can
  reach. Its source-dither prescription also holds only at modest magnification
  — it was validated at 2x and inverts well above that. Flagged for refresh.
- [EVE RAM_G budgeting for multi-theme splash assets](eve-ram-g-budgeting-multi-theme-splash-assets.md)
  — defined "perceptually forgiving" on the wrong axis: magnification forgives
  resolution loss and *amplifies* encoding artefacts. Its multi-theme premise is
  also gone.
- [BT817 flash-resident ASTC assets](../architecture-patterns/bt817-flash-resident-astc-assets.md)
  — its codec recommendation needs the qualifier that ASTC is a block codec and
  therefore wrong storage for a magnified smooth ramp.

**Same reflex, other subjects.**
- [A clock constant is a request, not the operating point](../conventions/a-clock-constant-is-a-request-not-the-operating-point.md)
  — measure-don't-assume applied to a bus clock; here it was applied to the
  framebuffer's own output depth.
- [A correction is an unreviewed change](../conventions/a-correction-is-an-unreviewed-change.md)
  — the deleted-parser incident is a fresh instance, and the reason the removal
  was closed against the live command surface rather than the build log.
- [Verifying every part of a claim does not verify the claim](verifying-every-part-of-a-claim-does-not-verify-the-claim.md)
  — every preview render was correct; *"the preview looks good, therefore the
  panel looks good"* was the defect.
- [Continuity and slew are two separate fixes](continuity-and-slew-are-two-separate-fixes.md)
  — the structural match for a chain of fixes each of which measured as done and
  unmasked the next cause.
