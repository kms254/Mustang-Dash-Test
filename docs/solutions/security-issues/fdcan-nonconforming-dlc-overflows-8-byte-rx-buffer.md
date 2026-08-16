---
title: "An 8-byte CAN RX buffer is a stack smash: ST's FDCAN HAL copies DLCtoBytes[raw DLC], up to 64 bytes, regardless of frame format"
date: 2026-08-16
category: security-issues
module: can-rx
problem_type: security_issue
component: tooling
severity: critical
symptoms:
  - "ST's HAL copies DLCtoBytes[rx.DataLength] bytes into the RX destination buffer, and that table runs 0,1,2,...,8,12,16,20,24,32,48,64 regardless of classic vs FD frame format (framework-arduinoststm32 stm32h7xx_hal_fdcan.c line 245 table, line 3093 copy loop)"
  - "FDCAN RX drain in MustangDash/dash_can.h declared all three HAL RX destination buffers as uint8_t data[8], sized to the classic-CAN logical DLC maximum rather than the HAL's actual copy width"
  - "A classic CAN frame legally carries wire DLC codes 9-15 (all of which mean '8 data bytes'), so a single malformed or nonconforming frame from a real bus drives the HAL to copy up to 64 bytes into the 8-byte buffer -- a stack overflow of up to 56 bytes"
  - "Never manifested at runtime -- caught by two independent reviewers during code review of the new CAN RX path (branch feat/can-ford-dialect, PR #45) before any live-bus session"
root_cause: wrong_api
resolution_type: code_fix
related_components:
  - "dash_can.h"
  - "dash_can_ford.h"
  - "fdcan-hal-rx-drain"
tags: [fdcan, can-bus, buffer-overflow, stack-overflow, vendor-hal, stm32h7, dlc, code-review]
---

# An 8-byte CAN RX buffer is a stack smash: ST's FDCAN HAL copies DLCtoBytes[raw DLC], up to 64 bytes, regardless of frame format

## Problem

The new CAN RX drain (`dash_can_rx_drain()` in `MustangDash/dash_can.h`) originally passed an 8-byte stack buffer to `HAL_FDCAN_GetRxMessage()`, sized from the protocol truth that a classic CAN frame carries at most 8 data bytes. But ST's FDCAN HAL sizes its copy from the raw DLC *code*, not the classic-CAN byte count: `HAL_FDCAN_GetRxMessage` copies `DLCtoBytes[DataLength]` bytes into the caller's buffer, and that table runs `{0..8, 12, 16, 20, 24, 32, 48, 64}` with no check of the frame's FD flag. A classic frame legally carries DLC codes 9–15 (wire meaning: 8 data bytes), so a single nonconforming-but-legal frame from any node on the bus overflows the caller's stack frame by up to 56 bytes. Fixed in PR kms254/Mustang-Dash-Test#45 (branch `feat/can-ford-dialect`; open as of this writing).

## Symptoms

**None at runtime — and honestly so.** The defect was caught by two independent code reviewers (a correctness pass and an adversarial pass, converging on the same line) reviewing the new RX drain, in the window between the build landing and the first live-bus session: the CAN transceivers were not even installed yet, so no frame had ever traversed this path on hardware.

What the symptom row *would* have been, on the first frame with DLC 9–15 on a real car bus:

- Silent corruption of up to 56 bytes of stack above `data[8]` — the RX header, loop locals, saved registers, return address — with nothing logged and no fault at the moment of the write.
- Most likely presentation: a hard fault or a wedged main loop at some later, unrelated point in the frame-drain path, on a bus the bench never produces (the bench impostor `tools/can_emit.py` sends well-formed frames; a car harness does not promise that).
- Worst presentation: no crash at all — corrupted locals decoding into plausible-looking gauge values.

## What Didn't Work

- **Sizing the buffer from the protocol instead of from the callee.** The original rationale — "classic CAN payload is max 8 bytes" — is true of the wire and false of the HAL. The receiving side's contract is not "how many data bytes can a classic frame carry" but "how many bytes will `HAL_FDCAN_GetRxMessage` write," and that answer lives in the HAL source, not the CAN spec:

  `stm32h7xx_hal_fdcan.c:245`:

  ```c
  static const uint8_t DLCtoBytes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
  ```

  and the copy loop in `HAL_FDCAN_GetRxMessage`, `stm32h7xx_hal_fdcan.c:3092-3096`:

  ```c
  /* Retrieve Rx payload */
  pData = (uint8_t *)RxAddress;
  for (ByteCounter = 0; ByteCounter < DLCtoBytes[pRxHeader->DataLength]; ByteCounter++)
  {
      pRxData[ByteCounter] = pData[ByteCounter];
  }
  ```

  `pRxHeader->DataLength` is the raw 4-bit DLC field straight out of message RAM (`stm32h7xx_hal_fdcan.c:3074`); nothing between it and the table indexing consults `FDFormat`.

- **Reviewing the drain logic in isolation.** The drain's own code reads correctly against its assumptions — the bug is invisible until a reviewer opens the vendor HAL and reads the copy loop. Both reviewers found it only by reading `stm32h7xx_hal_fdcan.c` itself, not by any analysis of `dash_can.h` alone. A structural review of the caller can never surface a callee whose write length exceeds the caller's mental model of it.

## Solution

Two independent halves, both in `MustangDash/dash_can.h`: size every RX buffer for the HAL's worst case, and clamp the DLC before handing it to the decoders (whose `dlc` parameter is a byte count by contract — the typedef at `dash_can.h:41-43`, with the contract language spelled out at the clamp, `dash_can.h:277-280`).

Before (the drain as first written):

```c
FDCAN_RxHeaderTypeDef rx;
uint8_t data[8] = {0};   /* classic CAN: max 8 data bytes */
if (HAL_OK != HAL_FDCAN_GetRxMessage(&g_can1, FDCAN_RX_FIFO0, &rx, data))
```

After — the drain, `dash_can.h:263-281`:

```c
FDCAN_RxHeaderTypeDef rx;
/* 64 bytes, not 8: a classic frame legally carries DLC codes 9-15,
 * and HAL_FDCAN_GetRxMessage copies DLCtoBytes[raw DLC code] bytes
 * -- a table that runs to 64 regardless of frame format -- so an
 * 8-byte buffer is a stack smash waiting on one malformed frame. */
uint8_t data[64] = {0};
if (HAL_OK != HAL_FDCAN_GetRxMessage(&g_can1, FDCAN_RX_FIFO0, &rx, data))
{
    break;
}
...
/* Classic-CAN DLC codes 0-8 ARE the byte count in this HAL
 * (FDCAN_DLC_BYTES_8 == 8U); codes 9-15 mean 8 data bytes on the
 * wire, so clamp before the decoder -- its dlc contract is a byte
 * count, and 9-15 must read as a full frame, not a magic length. */
const uint8_t dlc = (rx.DataLength <= 8U) ? (uint8_t) rx.DataLength : 8U;
```

Every other call site that receives through this HAL got the same 64-byte buffer, each carrying a pointer back to the drain's comment. At the current tree there are four such sites in `dash_can.h`:

- the drain itself (`dash_can.h:268`),
- the boot-time flush `dash_can_rx_flush()` (`dash_can.h:319`),
- the `cantest` stale-frame drop loop (`dash_can.h:344`),
- the `cantest` receive poll (`dash_can.h:373`),

the latter three annotated `/* 64, not 8: see the drain -- the HAL copies up to 64 bytes */`. (The drain and both cantest sites were widened from 8 bytes; the boot flush was authored in that same review-fix commit, born already at 64.)

The test half is the DLC-15 acceptance case in `tests/test_dash_can_ford.c:415-444`: it feeds the golden `nominal_0270_rpm` vector's bytes to the real decoder at `dlc=15` and requires it to be consumed with a bit-exact result versus `dlc=8`:

```c
expect(dash_can_ford_decode(&cf15, rpm->id, 15u, rpm->bytes,
                            0u, &s15),
       "dlc=15 must be consumed: acceptance is dlc >= 8, not "
       "dlc == 8");
expect(s15.valid == s8.valid && s15.can_owned == s8.can_owned,
       "a dlc=15 decode must produce the same valid/can_owned "
       "masks as dlc=8");
expect(dash_ch_get(&s15, DASH_CH_RPM)
           == dash_ch_get(&s8, DASH_CH_RPM),
       "a dlc=15 decode must read bytes 0-7 identically to "
       "dlc=8 (bit-exact RPM)");
```

This pins the `dlc >= 8` acceptance rule the drain's clamp depends on: a DLC 9–15 frame is a full 8-byte frame, not a runt and not a magic length.

## Why This Works

**DLC is a code, not a count.** The 4-bit DLC field has 16 values; classic CAN defines only 0–8 as literal byte counts and specifies that 9–15 mean "8 data bytes" on the wire, while CAN FD reuses the same codes for 12/16/20/24/32/48/64. The peripheral stores the raw code in message RAM, and **the HAL is format-agnostic by design** — classic and FD frames share one receive path, so `HAL_FDCAN_GetRxMessage` translates the code through the one table that is correct for FD (`stm32h7xx_hal_fdcan.c:245`) and copies that many bytes (`:3093`) without asking whether this frame was classic. That is a reasonable HAL design; the caller-side consequence is that the buffer contract is "64 bytes, always," even on a bus that will only ever run classic CAN. A 64-byte buffer makes the caller correct against the callee's actual write behavior; the clamp then restores classic-CAN semantics (9–15 → 8) at the seam before the pure decoders, so their byte-count contract stays intact.

## Prevention

- **Size buffers from the callee's write behavior, not from the protocol.** Before passing a buffer to any vendor HAL, read the vendor's copy loop and size for its worst case. The datasheet describes the wire; the HAL source describes the write. On this repo that specifically means: any new `HAL_FDCAN_GetRxMessage` call site takes `uint8_t[64]`, no exceptions — the comment at `dash_can.h:264-267` is the canonical statement, and the other sites point at it.
- **Keep the DLC-15 test pattern for every future dialect.** Any decoder registered at the `dash_can_register_dialect()` seam inherits the drain's clamp, and its test suite should include the same acceptance case: feed a golden frame's bytes at `dlc=15`, require consumption and a bit-exact decode versus `dlc=8` (`tests/test_dash_can_ford.c:415-444` is the template).
- **The drain clamps so downstream stays pure.** The clamp at `dash_can.h:281` means the host-tested pure decoders (`dash_can_ford.h` and successors) keep a simple 8-byte-count contract and never need to know DLC codes exist — the one impure, hardware-facing file absorbs the HAL's semantics. Preserve that split; do not push raw `DataLength` values past the drain.
- **Adversarial review of a new hardware seam pays before first power-on.** This bug was caught between build and first live-bus session precisely because two independent reviewers (correctness + adversarial) were pointed at a brand-new RX path and read the vendor source underneath it. A path that has "never failed on the bench" has usually just never been fed the input that fails it.

## Related Issues

- PR kms254/Mustang-Dash-Test#45 (`feat/can-ford-dialect`) — the fix and the DLC-15 test; open as of this writing.
- `docs/plans/2026-08-15-001-feat-can-telemetry-ford-dialect-plan.md` — the Ford CAN dialect round that introduced the RX drain this bug rode in on.
- `MustangDash/dash_can_ford.h` / `tests/test_dash_can_ford.c` — the pure decoder and its golden-vector suite, whose `dlc >= 8` acceptance rule is the contract the drain's clamp upholds.
- Same family, different instruments — this repo's recurring "the library's actual behavior diverges from what its interface suggests" lesson: [a-tools-finding-can-be-a-property-of-the-tool](../developer-experience/a-tools-finding-can-be-a-property-of-the-tool.md), [a-count-at-the-report-limit-is-not-a-measurement](../developer-experience/a-count-at-the-report-limit-is-not-a-measurement.md), [eve-cmd-setbitmap-clobbers-current-font-handle](../ui-bugs/eve-cmd-setbitmap-clobbers-current-font-handle.md). This is the first instance where the divergence was memory-unsafe rather than merely misleading.
