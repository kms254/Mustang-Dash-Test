---
title: "Hand-transcribe a spec multiple times, independently, and gate on agreement"
date: 2026-08-16
category: design-patterns
module: can-ford-dialect
problem_type: design_pattern
component: tooling
severity: high
applies_when:
  - "A specification exists only as a document machines cannot read (PDF tables, datasheet prose) with no machine-readable source to generate code from"
  - "Decode/encode logic must be hand-transcribed from that spec into firmware, tooling, or test code"
  - "A single transcription and a test written from that same transcription would agree with each other even if both are wrong"
  - "Building golden vectors or a round-trip test for a hand-authored protocol decoder"
  - "Deciding whether to consolidate repeated scale/offset/constant tables into one shared header versus keeping them duplicated"
tags: [can-bus, dbc, hand-transcription, redundant-verification, golden-vectors, spec-transcription, ford-dialect, cross-check]
related_components:
  - "assets/can/ford-control-pack-gen4.dbc"
  - "MustangDash/dash_can_ford.h"
  - "tools/make_can_golden.py"
  - "tests/test_dash_can_ford.c"
  - "tools/can_emit.py"
---

# Hand-transcribe a spec multiple times, independently, and gate on agreement

## Context

Ford publishes the Gen 4 control pack's CAN broadcast (the 0x270-set) only as
tables inside PDF instruction sheets — IS_M-6017-M50HM (Gen 4X, 09/16/24) p.36
"CAN Message Definition" and IS_M-6017-73M (7.3L, 02/21/23) s.12.0 p.29. There
is no machine-readable source: no vendor DBC, no header, nothing to parse. Every
consumer of the spec therefore starts with a human reading a PDF table and
typing bit positions, scales, and offsets into a file.

That creates a verification problem with no conventional answer: if you
hand-transcribe the tables once and then test the decoder against expectations
derived from that same transcription, the test proves only self-consistency.
A transposed bit span or a mistyped scale factor reproduces identically in the
code and in the test, and the suite goes green over a decoder that reads the
real bus wrong.

The stakes here are not cosmetic. The Ford dialect decoder claims 9 of the
dash's 26 channels (`DASH_CAN_FORD_CLAIMS`, `MustangDash/dash_can_ford.h:91-95`)
— including oil pressure (`DASH_CH_OILP`), oil temperature, and coolant
temperature (`DASH_CH_ECT`), the channels behind the dash's alarm takeover. A
transcription slip is a gauge that lies about oil pressure in a car on track.

The approach built in PR kms254/Mustang-Dash-Test#45 (open as of this writing;
merge pending): transcribe the tables multiple independent times, in different
notations, and let tests force the transcriptions to agree. A slip must then be
made identically in every notation to hide.

## Guidance

**The pattern: N independent restatements of the spec, in different notations,
with agreement enforced by tests.** Never let one transcription become the
source another is generated from, and never let a test's expected values come
from the code under test.

This repo carries four transcriptions of the Ford tables:

1. **The DBC** — `assets/can/ford-control-pack-gen4.dbc`. Declarative bit
   placement and scaling in DBC notation (e.g. `SG_ AF0 : 30|11@0+ (0.01,7)`
   at line 46). Its `CM_` comments carry provenance — line 63 names both
   official Ford documents, records the bit-numbering convention translation,
   and tags per-message confidence (`"Confidence: official (sibling tables
   agree byte-for-byte)"`, line 64; the M50D GWM set is explicitly marked
   OFFICIAL-SIBLING pending a sniff at first car contact). Provenance comments
   matter: they make each transcription auditable back to the page it came from.

2. **The golden-vector generator's Python constants** —
   `tools/make_can_golden.py`. It encodes frames *with* the DBC (via cantools,
   `scaling=False` so the generator owns every raw bit) but computes the
   expected decoded values from its own restatement of the spec
   (`afr_from_raw`, `degf_from_raw`, etc., lines 59-86). Its docstring states
   the rule (lines 12-18):

   > Expected values are computed HERE, from the spec constants restated below
   > (degF = degC*9/5+32, psi = kPa*0.145037738, mph = km/h / 1.60934;
   > rpm/AFR/volts pass through) -- never by calling or reimplementing the C
   > decoder's extraction (plan KTD8: a test that pins the decoder's own output
   > proves nothing). The DBC places bits, this file restates the scaling, and
   > MustangDash/dash_can_ford.h is a third transcription of the same tables --
   > a transcription slip must reproduce identically in two of the three to hide.

3. **The firmware decoder** — `MustangDash/dash_can_ford.h`, hand-written C
   bit extraction. Its header comment (lines 10-16) makes the independence
   deliberate: bit positions and scaling "are transcribed DIRECTLY from the
   official Ford tables … They are deliberately NOT read out of the DBC under
   assets/can/ -- the DBC and this header must stay two independent
   transcriptions of the spec." Example extraction (lines 190-192):

   ```c
   /* ENGINE_SPEED, bits 2-15 (14-bit, 1 rpm/bit, 0-16383). */
   const uint16_t rpm = (uint16_t) (((bytes[0] & 0x3Fu) << 8) | bytes[1]);
   ```

   Each signal cites its official bit span in a comment, so the C shifts can be
   audited against the PDF without consulting any other transcription.

4. **The test-side packer** — the `ford_pack_*` functions in
   `tests/test_dash_can_ford.c` (lines 226-263), a fourth hand transcription
   written as the *inverse* (encode) direction. The file's header comment
   (line 7) calls it "an INDEPENDENT test-side packer." Same-file miniature of
   the pattern: `FORD9_MASK` (lines 52-58) re-transcribes the 9-channel claim
   set from the plan rather than reusing the decoder's macros, so a claim-set
   slip fails a `_Static_assert` instead of being a shared assumption.

Three gates force agreement:

- **Golden decode** (`tests/test_dash_can_ford.c`, the AE4 block from line
  269): the checked-in `tests/golden_can_ford.h` vectors — DBC-encoded bytes
  plus Python-computed expectations — are fed to the real C decoder. This is
  transcription 1 encoding, transcription 2 predicting, transcription 3
  decoding: three-way agreement per vector, 21 vectors, 44 expectations.
- **R11 sim round-trip** (`tests/test_dash_can_ford.c:714-816`): the real
  simulator drives a reference `DashState`; each step, its 9 CAN channels are
  packed by transcription 4 and decoded by transcription 3 into a second
  `DashState`, compared channel-for-channel over 600 steps.
- **Emitter byte-match** (`tools/can_emit.py`): the bench impostor's
  `--dry-run --dry-run-fixed` mode encodes a fixed value set and byte-compares
  against the golden nominal vectors (`run_dry_run_fixed`, lines 297-314),
  exiting 1 on any mismatch — so the PC emitter that plays the Ford PCM on the
  bench is pinned to the same bytes the decoder is tested against.

**The tolerance nuance — do not use one tolerance family for two different
comparisons.** Agreement checks between two implementations of the *same math
over the same raw integers* must use tight, float-rounding-scale tolerances;
round-trips that pass through *wire quantization* need quantum-scale bounds.
Conflating them lets a one-quantum systematic bias pass the golden test — a
real review finding on this PR, fixed in the same PR by splitting the families:

- `GOLDEN_CAN_TIGHT_*` (`tests/golden_can_ford.h:32-39`): 0.001 in channel
  units, for golden decode only. The derivation comment (lines 24-31): float
  rounding between the generator's double math and the decoder's float math is
  "< 1e-4 in every channel's units at range max. 0.001 keeps >= 10x headroom
  over that while sitting strictly below HALF a raw quantum on every channel,
  so a decoder systematically biased by one quantum FAILS."
- `GOLDEN_CAN_TOL_*` (`tests/golden_can_ford.h:48-55`): one raw quantum per
  channel (e.g. 1.8 °F for the 1 °C temperature quantum, 0.145 psi for 1 kPa),
  for the R11 round-trip only, where the packer quantizes floats to raw counts
  before the decoder ever sees them — one quantum of error is inherent there,
  and only there.

Both families carry a "NEVER widen to make a test pass" rule
(`tools/make_can_golden.py:95-112`, `tests/test_dash_can_ford.c:78-92`):
re-derive from the spec quantum instead. A golden assertion failing at TIGHT
is a real transcription divergence in one of the transcriptions, not noise.

## Why This Matters

The probability argument: one transcription hides any error it contains; N
independent transcriptions in different notations hide only errors made
*identically N times by different processes*. A bit-span transposition typed
into a DBC sawtooth start bit, a Python arithmetic function, a C shift-and-mask
expression, and a C inverse-encode loop would have to land on the same wrong
answer in four unrelated notations before the gates go green. Errors that
plausibly repeat (misreading the PDF itself the same way twice) shrink the
guarantee but don't void it — the notations force different *kinds* of mistakes,
and the encode/decode inversion in particular makes an off-by-one asymmetric.

It also keeps the bench honest over time: `tools/can_emit.py` (the frame
producer) and `dash_can_ford.h` (the consumer) remain two independent
consumers of one spec, pinned to each other only through the golden bytes. Had
the emitter been generated from the decoder (or vice versa), the pair could
drift together — a self-consistent loop that plays and decodes frames no real
Ford PCM sends, with every test green.

## When to Apply

- Hand-transcription is unavoidable: PDF-only vendor specs, printed register
  maps, datasheet tables, protocol definitions with no machine-readable
  release. The CAN dialect here is the archetype; a peripheral's register map
  transcribed into both firmware defines and a test-side model is the same
  shape.
- The transcribed data feeds something whose wrongness is expensive or
  invisible — safety-relevant telemetry, calibration constants, anything where
  the failure mode is "plausible wrong numbers" rather than a crash.

**Anti-boundary — when a machine-readable source exists, generate; do not
duplicate.** If one authoritative artifact can *generate* every consumer
(codegen from a real vendor DBC, from a SVD file, from an IDL), generation is
strictly better: duplication then carries all of the maintenance cost and none
of the verification value, because the "transcriptions" share one origin and
one failure mode. This pattern is exclusively for the case where every copy
must pass through human hands anyway.

**Never "clean up" the duplication into a shared constants header.** The
repeated `0.145037738` / `1.60934` / `raw*0.01+7` across four files will look
like a DRY violation to any future refactor pass. Centralizing them deletes
the property the tests depend on: once the decoder and the golden generator
read one constant, a wrong value in it verifies itself. The comments in each
file (`dash_can_ford.h:10-16`, `make_can_golden.py:12-18`,
`test_dash_can_ford.c:49-51`) exist to warn that refactor off.

**Honest maintenance cost:** a spec revision must be applied N times (four
here), and each application re-audited against the new document. That is the
price of the verification property, paid deliberately. The gates make a missed
application loud — an update applied to three of four transcriptions fails the
golden or round-trip tests rather than shipping.

## Examples

One constant traced through all four notations — the wideband A/F encoding,
A/F = raw × 0.01 + 7:

1. **DBC** — `assets/can/ford-control-pack-gen4.dbc:46`:
   `SG_ AF0 : 30|11@0+ (0.01,7) [7|27.47] "A/F" DASH`
   (scale/offset pair `(0.01,7)`; restated in prose at line 65:
   `A/F = raw*0.01+7 (PCM encodes ((lambda*stoich)-7)*100)`).
2. **Golden generator, Python** — `tools/make_can_golden.py:84-86`:
   ```python
   def afr_from_raw(raw):
       """AF0/AF1: A/F = raw*0.01 + 7, pass-through."""
       return raw * 0.01 + 7.0
   ```
3. **Firmware decoder, C** — `MustangDash/dash_can_ford.h:200-203`:
   ```c
   /* AF0, bits 25-35 (11-bit); A/F = raw * 0.01 + 7 -- already
    * gasoline AFR, passed through (KTD2 assumption). */
   const uint16_t af0 = (uint16_t) (((bytes[3] & 0x7Fu) << 4) | (bytes[4] >> 4));
   dash_can_ford_write_(s, DASH_CH_AFR_L, (float) af0 * 0.01f + 7.0f);
   ```
4. **Test-side packer, C inverse** — `tests/test_dash_can_ford.c:236-237`:
   ```c
   ford_put_bits(bytes, 25u, 11u,
                 ford_clamp_raw((afr_l - 7.0f) * 100.0f, 2047.0f));
   ```

The golden decode gate checks 1→2→3 agreement (raw 560 must decode to 12.6 A/F
within `GOLDEN_CAN_TIGHT_AFR` = 0.001, half-quantum 0.005); the R11 round-trip
checks 4→3 inversion within `GOLDEN_CAN_TOL_AFR` = 0.01 (one quantum); and
`can_emit.py --dry-run-fixed` byte-matches the emitter's encode of 12.6/12.1
A/F against the golden `nominal_0274_afr_fuelp` bytes. A slip in the `+7`
offset, the `0.01` scale, or the bits-25-35 span in any single file fails at
least one gate.

## Related

- PR kms254/Mustang-Dash-Test#45 (Ford CAN dialect; open, merge pending)
- Plan: `docs/plans/2026-08-15-001-feat-can-telemetry-ford-dialect-plan.md`
  (KTD8 is the independence rule these files cite)
- `tests/golden_can_ford.h` — the generated agreement artifact (header comment
  lines 1-16 restates the whole provenance chain)
- CLAUDE.md "the Ford dialect decoder" section — the seam this feature plugs
  into (sim → `DashState` ← CAN, one producer contract)
- Sibling from the same PR, different defect class:
  [fdcan-nonconforming-dlc-overflows-8-byte-rx-buffer](../security-issues/fdcan-nonconforming-dlc-overflows-8-byte-rx-buffer.md)
  (RX-path memory safety; shares files, not topic).
- The other side of this doc's boundary:
  [astc-swizzle-validated-against-eve-asset-builder](../tooling-decisions/astc-swizzle-validated-against-eve-asset-builder.md)
  — same PDF-only-spec problem shape, solved with a one-shot external vendor
  oracle instead of permanently-maintained transcriptions. Use that shape when
  an authoritative external implementation exists to diff against once; use
  this doc's shape when agreement must be re-proven on every test run.
