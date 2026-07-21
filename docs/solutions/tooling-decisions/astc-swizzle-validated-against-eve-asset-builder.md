---
title: "Validating our ASTC block swizzle against Bridgetek's EVE Asset Builder"
date: 2026-07-20
category: tooling-decisions
module: splash-asset-pipeline
problem_type: tooling_decision
component: tooling
severity: medium
applies_when:
  - "a hand-rolled implementation of a vendor binary-format spec has only indirect evidence (it renders correctly) that it is byte-correct"
  - "a vendor ships a GUI tool whose CLI backend can be run headless as an independent oracle"
  - "deciding whether to replace a pinned, portable pipeline step with a vendor tool that produces identical output"
  - "reading EVE Asset Builder output (.raw, sidecar .json) and mapping its fields onto BT81x flash-layout requirements"
tags:
  - astc
  - bt817
  - eve4
  - eve-asset-builder
  - asset-pipeline
  - cross-validation
  - tooling-portability
---

# Validating our ASTC block swizzle against Bridgetek's EVE Asset Builder

## Context

`tools/make_splash_flash.py` encodes every splash asset with a pinned `astcenc`, strips the 16-byte `.astc` container header, and reorders the raw blocks from linear raster order into EVE's 2x2 tile order. That reordering — `swizzle_blocks()` at `tools/make_splash_flash.py:181-208` — was implemented **from the PDF spec alone** (BRT_AN_033 BT81X Programming Guide section 6.1 "ASTC RAM Layout": tiles of 2x2 blocks stored TL, BL, BR, TR; an odd trailing column packs 1x2 top-then-bottom; an odd trailing row is linear; the pipeline restates this at `tools/make_splash_flash.py:23-28`).

Until now the only evidence the swizzle was right was negative and indirect: *the panel does not render scrambled blocks*. That rules out a grossly wrong ordering, but it does not rule out a subtly wrong one — an odd-column or odd-row edge case that happens not to bite on the asset dimensions we ship. A hand-rolled binary-format transform with no independent oracle is exactly the kind of code that silently works until the day an asset has an odd block count.

An oracle turned out to already be on this workstation. EVE Screen Editor 5.3.0 bundles Bridgetek's own EVE Asset Builder as a headless CLI.

## Guidance

**When a vendor ships a GUI asset tool, look for its CLI backend and use it as a differential oracle against your own implementation of the same spec — then decide adoption separately from validation.**

### Finding and running the oracle

ESE 5.3.0 ships EAB as a Nuitka-compiled executable, with its own `astcenc.exe` beside it:

```
C:\Users\Public\Documents\EVE Screen Editor\modules\EabQmlPlugin\nuitka\eab_tools.exe
C:\Users\Public\Documents\EVE Screen Editor\modules\EabQmlPlugin\nuitka\astcenc.exe
```

`eab_tools.exe` shells out to `astcenc.exe` **by bare name**, so that directory must be on `PATH` or the run dies with `'astcenc.exe' is not recognized`. The invocation form is the one ESE's own "Command Line" tab surfaces:

```
eab_tools.exe pre82x img_cvt --input <in> --output-dir <dir> --output-name <name> \
  --format ASTC_4x4 --astc-effort thorough --astc-params "-j 1"
```

### The differential test

Run both pipelines on the same tracked asset and compare bytes. We used `assets/splash/emblem-200x200.png` (200x200 → 50x50 ASTC 4x4 blocks → 40,000 B).

- **EAB:** the command above → `emblem_ASTC_4x4.raw`, 40,000 bytes.
- **Ours:** `astcenc-sse2 -cl <src> <out> 4x4 -thorough -j 1 -silent` (the flags `run_astcenc()` uses at `tools/make_splash_flash.py:140-144`), strip the 16-byte header, then `swizzle_blocks(linear, 50, 50)` imported directly from the module.

```
EAB == ours_linear   : False
EAB == ours_swizzled : True
same block multiset  : True
```

**EAB already applies the EVE 2x2 tile swizzle, and our implementation reproduces it byte-for-byte.**

Confirmed a second way on 2026-07-21: the pack `make_splash_flash.py` emits was written into a panel's QSPI flash from EVE Screen Editor and rendered from flash addresses on real BT817 silicon, producing a correct splash. A wrong swizzle renders as scrambled blocks, so a correct render is independent hardware evidence for the same claim — byte equality against EAB, and visual correctness on the device, from two unrelated toolchains.

The third line is what makes the comparison sound, and it should not be skipped. Comparing two independently-configured encoders risks a false *negative* (different encoder output, so the ordering test is meaningless) or a coincidental false *positive*. Confirming the two outputs are permutations of the same 2,500 blocks proves both binaries encoded identically, so the only remaining difference under test is block order — which is precisely the claim.

### Secondary findings about EAB output

1. **EAB's `.raw` carries no container header.** Confirmed by arithmetic on two assets: 1200x760 at ASTC 4x4 = 300x190 blocks x 16 B = 912,000 bytes exactly, and 200x200 = 50x50 x 16 B = 40,000 bytes exactly. Pure block data, identical in shape to our header-stripped output — no adjustment needed when comparing.
2. **The `stride: 64` field in EAB's sidecar `.json` is a constant, not a line stride.** It read 64 for both a 1200-wide and a 200-wide image. It corresponds to the **64-byte flash alignment requirement** (guide section 6.2), which the pack layout already honors (`tools/make_splash_flash.py:35-37`). Do not read it as a per-image value and do not derive a `BITMAP_LAYOUT` linestride from it — a 64-byte linestride is wrong for every asset we ship.
3. **EAB also emits a `.converted.png`** — its own decode of the compressed result. That is a genuinely useful QA artifact: it shows compression damage without flashing the panel. Our pipeline has no equivalent. Noted as a possible future improvement, not a gap that blocks anything.

### The adoption decision: validate, do not switch

**Do not change `make_splash_flash.py` to shell out to `eab_tools`, despite byte-identical output.** Three reasons, in order of weight:

- **Portability regression.** `eab_tools.exe` is Windows-only and requires an ESE install. Our pipeline uses a pinned Linux `astcenc` fetched by `tools/get-astcenc.sh` and runs under WSL (`wsl -- python3 tools/make_splash_flash.py`, per `tools/make_splash_flash.py:41-43`). Switching would make splash builds depend on a GUI application being installed on the build host — the opposite direction from "fix things in the repo, not per-machine."
- **It replaces one step out of many.** EAB covers only image conversion. The float background blur, the flash pack layout, 64-byte alignment, the CRC, and the pack header are all ours and would remain ours.
- **It demonstrated the exact fragility the pinned binary avoids.** It could not run at all until `astcenc.exe` was manually placed on `PATH`. `tools/get-astcenc.sh` exists to pin a known binary with sha256 verification precisely so the pipeline never depends on ambient environment state.

The value delivered here is **confidence, not code**. The swizzle is now known-correct against the vendor's own implementation rather than assumed-correct from a PDF.

## Why This Matters

- **It converts indirect evidence into direct evidence.** "The panel doesn't render scrambled blocks" and "our bytes equal Bridgetek's bytes" are very different confidence levels. The second one covers the odd-column and odd-row edge cases that our current asset dimensions never exercise — the failure mode that would otherwise appear the first time someone adds an asset with an odd block count.
- **It removes the swizzle from the suspect list during future debugging.** When a flash-resident asset renders wrong, `swizzle_blocks()` can now be ruled out by citation instead of re-derived from the spec.
- **It's a repeatable technique, not a one-off.** Vendor GUI tools frequently wrap a scriptable backend. When a project hand-implements a vendor binary format, checking whether the vendor's own tool can be driven headless is a cheap way to get an independent oracle. The differential test costs minutes; re-deriving a spec by hand costs hours and yields less certainty.
- **Validation and adoption are separate decisions.** An external tool producing identical output is evidence your implementation is right — it is not automatically an argument to depend on that tool. Portability, environment fragility, and scope of coverage decide adoption; byte equality only decides correctness.

## When to Apply

- **Before trusting any hand-rolled implementation of a vendor binary layout** — block orderings, tile swizzles, header packings, alignment rules — where the only current evidence is "it looks right on the device."
- **When a vendor GUI tool is installed anyway.** Check for a bundled CLI (ESE surfaces the exact command in a "Command Line" tab). If one exists, an oracle is already available at no cost.
- **Always confirm the comparison is sound before drawing a conclusion from it.** With two independent encoders, verify the outputs are permutations of the same block set. Without that check, a byte mismatch is uninterpretable and a byte match is luck.
- **Not as a reason to adopt.** Run the adoption question through the project's actual constraints — here, cross-platform reproducibility beat vendor-canonical output that we already match exactly.

## Examples

**The comparison harness** (run under WSL; source paths absolute so `make_splash_flash` imports cleanly):

```python
import sys, subprocess, pathlib
sys.path.insert(0, "tools")
from make_splash_flash import swizzle_blocks, ASTCENC   # ASTCENC = tools/.astcenc/astcenc-sse2

src = pathlib.Path("assets/splash/emblem-200x200.png")
out = pathlib.Path("/tmp/ours.astc")
subprocess.run([str(ASTCENC), "-cl", str(src), str(out),
                "4x4", "-thorough", "-j", "1", "-silent"], check=True)

ours_linear = out.read_bytes()[16:]          # strip the .astc container header
ours_swz    = swizzle_blocks(ours_linear, 50, 50)
eab         = pathlib.Path("/tmp/eab/emblem_ASTC_4x4.raw").read_bytes()

blocks = lambda d: sorted(d[i:i+16] for i in range(0, len(d), 16))
print("EAB == ours_linear  :", eab == ours_linear)
print("EAB == ours_swizzled:", eab == ours_swz)
print("same block multiset :", blocks(eab) == blocks(ours_swz))   # soundness gate
```

**What each result line would have meant:**

| Result | Interpretation |
|---|---|
| swizzled match + multiset match | Our swizzle is correct (**observed**) |
| linear match + multiset match | EAB does *not* swizzle; the firmware or our pipeline would need re-examination |
| neither match, multiset match | Same encoder, but our tile ordering is wrong — a real bug |
| multiset mismatch | Encoders differ; the ordering test says nothing. Fix the flags and re-run |

That last row is why the multiset check is part of the harness rather than an afterthought.

## Related

- `docs/solutions/architecture-patterns/bt817-flash-resident-astc-assets.md` — the flash-resident ASTC architecture this pipeline serves; its "the 2x2 tile swizzle is mandatory" bullet is the claim this doc independently confirms, and its encoder-pinning section is the constraint that decided against adopting `eab_tools`.
- `docs/solutions/design-patterns/eve-ram-g-budgeting-multi-theme-splash-assets.md` — the RAM_G budgeting pressure that motivated moving splash assets to flash in the first place.
- `docs/solutions/best-practices/riverdi-rvt70h-vs-ritft70-eve-display-profile-selection.md` — the BT817/EVE4 profile grounding the ASTC flash feature set depends on.
