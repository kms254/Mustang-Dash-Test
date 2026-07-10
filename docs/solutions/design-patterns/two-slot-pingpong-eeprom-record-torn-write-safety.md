---
title: "Two-Slot Ping-Pong EEPROM Records for Torn-Write-Safe Persistence"
date: 2026-07-10
problem_type: design_pattern
category: design-patterns
component: database
module: persistence
severity: high
applies_when:
  - "persisting counters that must survive power loss on wear-leveled flash-emulated EEPROM"
  - "any single-record CRC-validate-or-zero design"
  - "automotive/embedded 12V-rail contexts where power can drop mid-write"
  - "Teensy 4.1 EEPROM emulation writes (per-byte, non-atomic, multi-sector)"
  - "designing lifetime/odometer-style monotonic counters in embedded firmware"
tags:
  - teensy
  - eeprom
  - torn-write
  - ping-pong
  - crc
  - odometer
  - persistence
  - power-loss
---

# Two-slot ping-pong records for torn-write-safe EEPROM persistence on Teensy 4.1

## Context

The dash odometer persists lifetime miles to the Teensy 4.1's emulated EEPROM, written on every 0.1-mile crossing while driving — in a car, where the 12 V rail dies without warning. The original design was a single record at a fixed address, CRC-validated, with zero-init on validation failure. Two independent code reviewers flagged that design P1, and a validator CONFIRMED the finding by reading the actual emulation source.

Per the review validator's reading of `cores/teensy4/eeprom.c` (Teensyduino 1.62 — the file lives in the workstation's Arduino15 packages, not in this repo):

- The emulation writes **per-byte, non-atomically**. There is no transactional record write.
- Addresses stripe across flash sectors as `addr >> 2 % FLASH_SECTORS`, so a 15–19 byte record at low addresses **spans multiple 4K flash sectors** — one logical "write" is scattered across several physical erase units.
- Unchanged bytes are **skipped**, so a given write touches an unpredictable, scattered subset of the record's bytes rather than a contiguous run.
- The sector-compaction path has an **erase-then-rewrite window**: if power dies inside it, live bytes can be wiped outright, not just left half-updated.

Net effect: a power cut during (or even near) a write can leave the record CRC-invalid in arbitrary ways. With single-slot + zero-init-on-invalid, the failure mode is that the **lifetime odometer resets to 0** — the CRC correctly detects the corruption and then the recovery policy destroys the data anyway.

The fix shipped in PR #3 (branch `feat/dash-layout`): a two-slot ping-pong record scheme in `MustangDash/dash_odo.h`, with thin EEPROM glue in `MustangDash/MustangDash.ino`. No hardware power-cut test was performed; the torn-write behavior is proven by host-side tests (`tests/test_dash_odo.c`) plus the validator's reading of the emulation source — that is the exact evidence basis.

## Guidance

### The record: fixed layout, serialized field-by-field, CRC-8 over everything

Define an explicit byte layout — never `memcpy` a struct — with magic, version, payload, a monotonic sequence counter, and a trailing CRC. From `MustangDash/dash_odo.h:11-19`:

```
 * Record layout v2, serialized field-by-field (never rely on struct padding):
 *
 *   offset  0: magic       u32 LE  0x4F444F4D ("MODO")
 *   offset  4: version     u8      2
 *   offset  5: reserved    u8      0
 *   offset  6: odo_tenths  u32 LE  lifetime odometer, tenth-miles
 *   offset 10: trip_tenths u32 LE  trip meter, tenth-miles
 *   offset 14: seq         u32 LE  monotonic write counter
 *   offset 18: crc8                CRC-8/MAXIM over bytes 0..17
```

Backed by constants at `dash_odo.h:45-48`:

```c
#define DASH_ODO_MAGIC       0x4F444F4DU /* "MODO" when stored little-endian */
#define DASH_ODO_VERSION     2
#define DASH_ODO_RECORD_SIZE 19
#define DASH_ODO_SLOT_ADDR(i) ((i) * 32) /* slot 0 at 0, slot 1 at 32 */
```

The CRC choice is CRC-8/MAXIM (Dallas/1-Wire), implemented in right-shift 0x8C form at `dash_odo.h:65-78`. The rationale is in the comment (`dash_odo.h:61-64`): it "catches every burst error of 8 bits or fewer, so any single corrupted record byte fails decode" — a good fit for a 19-byte record where the dominant failure is a handful of torn bytes, at one byte of overhead.

### Two slots, 32 bytes apart

The record lives in TWO slots of the 4284-byte emulated EEPROM. The design comment at `dash_odo.h:21-30` states both the mechanism and the threat model:

```
 * Torn-write protection: the record is kept in TWO slots of the Teensy 4.1's
 * 4284-byte emulated EEPROM (DASH_ODO_SLOT_ADDR(0)=0, DASH_ODO_SLOT_ADDR(1)=32;
 * the 32-byte spacing keeps the slots in different emulation strides and
 * leaves room to grow). Each write bumps `seq` and lands in slot (seq % 2),
 * i.e. always the slot NOT holding the current record, so a power cut
 * mid-write (per-byte non-atomic flash writes, plus the emulation's
 * sector-compaction erase window) can only corrupt the slot being written --
 * the previous record survives and boot loads it via dash_odo_pick_load_slot
 * (higher valid seq wins; tie picks slot 0).
```

The 32-byte spacing serves two purposes: it keeps the slots in different emulation strides (relevant given the `addr >> 2` striping the validator observed), and it leaves 13 spare bytes per slot so a future v3 record can grow without moving slot 1.

### Boot: decode both, highest valid seq wins

`dash_odo_pick_load_slot` (`dash_odo.h:145-161`) is the entire arbitration policy:

```c
static inline uint8_t dash_odo_pick_load_slot(const uint8_t rec0[DASH_ODO_RECORD_SIZE],
                                              const uint8_t rec1[DASH_ODO_RECORD_SIZE],
                                              DashOdo *out)
{
    DashOdo o0, o1;
    bool ok0 = dash_odo_decode(&o0, rec0);
    bool ok1 = dash_odo_decode(&o1, rec1);
    if (ok0 && ok1)
    {
        if (o1.seq > o0.seq) { *out = o1; return 1U; }
        *out = o0;
        return 0U;
    }
    if (ok0) { *out = o0; return 0U; }
    if (ok1) { *out = o1; return 1U; }
    return 0xFFU;
}
```

Both slots decode (magic + version + CRC, `dash_odo_decode` at `dash_odo.h:126-138`); the higher valid `seq` wins; a lone valid slot wins by default; equal seq — unreachable via the normal write path — deterministically picks slot 0 (`dash_odo.h:140-144`). Only when **neither** slot is valid does it return 0xFF and leave `*out` untouched, and only then does the caller zero-init. Zero-init is now reserved for genuinely fresh EEPROM (or a torn very first write), not for any single corruption event.

### Write: bump seq, land in the *other* slot

The slot is derived from seq parity, so alternation is a consequence of the counter rather than separately tracked state. `dash_odo.h:163-181`:

```c
/* The slot the NEXT write must land in: seq+1's parity, i.e. always the slot
 * NOT holding the current record, so a torn write can never destroy the only
 * good copy. */
static inline uint8_t dash_odo_next_write_slot(const DashOdo *o)
{
    return (uint8_t)((o->seq + 1U) & 1U);
}

/* One-call write preparation for the .ino glue: bump seq, serialize, and
 * report which slot (see DASH_ODO_SLOT_ADDR) the record must be written to.
 * The seq bump is intentionally pre-encode so the counter stored in EEPROM
 * always matches the slot's parity. */
static inline void dash_odo_encode_next(DashOdo *o, uint8_t out[DASH_ODO_RECORD_SIZE],
                                        uint8_t *slot)
{
    o->seq += 1U;
    dash_odo_encode(o, out);
    *slot = (uint8_t)(o->seq & 1U);
}
```

The pre-encode seq bump matters: the counter stored inside the record always matches the parity of the slot it occupies, so slot occupancy is self-describing and recovery after a tear naturally retries into the torn slot (its seq is stale/invalid, so the parity math points back at it).

### The .ino glue stays trivially thin

All EEPROM API contact is confined to two functions in `MustangDash/MustangDash.ino` (the toolchain header avr/eeprom.h - angle-include - at `MustangDash.ino:42` with the note "Teensy 4.1 wear-leveled EEPROM emulation (4284 B)"). Load, `MustangDash.ino:497-511`, including the review-finding comment:

```c
void odo_eeprom_load(void)
{
    /* Two-slot ping-pong (review finding): a torn write -- power loss
     * mid-write on the 12V rail -- corrupts at most the slot being
     * written; the other slot still holds the previous odometer, so a
     * tear costs 0.1 mi, never the lifetime count. */
    uint8_t rec0[DASH_ODO_RECORD_SIZE];
    uint8_t rec1[DASH_ODO_RECORD_SIZE];
    eeprom_read_block(rec0, (const void *)DASH_ODO_SLOT_ADDR(0), DASH_ODO_RECORD_SIZE);
    eeprom_read_block(rec1, (const void *)DASH_ODO_SLOT_ADDR(1), DASH_ODO_RECORD_SIZE);
    if (dash_odo_pick_load_slot(rec0, rec1, &g_odo) == 0xFFU)
    {
        dash_odo_init(&g_odo); /* blank or corrupt EEPROM: clean zero start */
    }
}
```

Write, `MustangDash.ino:513-519`:

```c
void odo_eeprom_write(void)
{
    uint8_t rec[DASH_ODO_RECORD_SIZE];
    uint8_t slot;
    dash_odo_encode_next(&g_odo, rec, &slot); /* bumps seq, alternates slots */
    eeprom_write_block(rec, (void *)DASH_ODO_SLOT_ADDR(slot), DASH_ODO_RECORD_SIZE);
}
```

Everything above these two functions is pure C with no Arduino dependencies (`dash_odo.h:2-9`), which is exactly what makes the torn-write scenarios host-testable.

### Write cadence: one write per tenth, none while parked

Wear and tear-exposure are bounded by writing only when there is something new worth persisting. `dash_odo_should_write` (`dash_odo.h:196-203`) fires only when a 0.1 mi boundary was crossed since the last write, or when `dirty_now` demands an immediate write:

```c
static inline bool dash_odo_should_write(DashOdo *o)
{
    return o->tenths_since_write > 0U || o->dirty_now;
}
```

`dash_odo_mark_written` (`dash_odo.h:205-210`) clears both after the physical write; `dash_odo_reseed` (`dash_odo.h:212-219`) — the `odo set <miles>` bench command — sets `dirty_now` so a manual reseed persists immediately rather than waiting for the next tenth. The main loop wiring is `MustangDash.ino:359-367` (advance → should_write → write → mark_written), and the reseed path at `MustangDash.ino:570-572` writes immediately. Stationary or creeping below a tenth never writes (`dash_odo.h:196-199`).

## Why This Matters

**The failure-cost transformation is the whole point.** Both designs detect a torn write — CRC-8 catches it either way. The difference is what detection costs:

- Single-slot + zero-init: a torn write costs the **entire odometer history**. A 24,318-mile car reads 0.0 after one unlucky ignition-off.
- Two-slot ping-pong: a torn write costs **one 0.1-mile interval** — the pending tenth that was being persisted. The previous record, in the slot that was not being touched, survives and loads at boot.

CRC-validate-then-zero-init alone is a trap worth naming: it *feels* safe because corruption is detected, but it pairs detection with total data loss. Detection without a surviving copy just converts silent corruption into deterministic erasure. The second slot is what turns the CRC from a data-destruction trigger into a data-recovery selector.

**Wear stays a non-issue.** At highway speed (~70 mph) the cadence produces ~700 writes/hour — one 19-byte record per 0.1 mi — and zero while parked or idling in place. The Teensy 4.1 emulation wear-levels those writes across its flash region (4284-byte logical capacity, `MustangDash.ino:42`), and per the validator's reading it additionally skips unchanged bytes, so actual flash stress per record write is a handful of bytes. Doubling the write sites from one slot to two alternating slots does not double wear per write — each write still writes one record; it just alternates where. The cadence, not the slot count, is the wear control.

**The evidence basis is honest.** No hardware power-cut test was performed. Confidence comes from (a) the validator's confirmed reading of the emulation's non-atomic per-byte writes, sector striping, and compaction erase window, which establishes that torn/partial records are a real reachable state, and (b) host tests that prove the recovery logic handles every such state correctly (see Examples).

## When to Apply

Use two-slot ping-pong (or a generalization of it) when all of these hold:

- The value is a **persistent counter or accumulator on flash-emulated EEPROM**: odometers, hour meters, energy/kWh counters, cycle counts, event totals.
- **Power loss is unscheduled** — automotive 12 V, battery packs, wall-wart devices with no shutdown handshake.
- The **history is expensive or impossible to reconstruct**: nobody can re-derive lifetime miles after a reset to zero.
- The underlying storage gives **no atomic multi-byte write** (true of the Teensy 4.1 emulation per the validator's reading, and of most flash-emulated EEPROM).

Skip it when:

- The value is **cheap to lose** — a UI preference, a last-selected screen, brightness. Zero-init on corruption is fine there; the user just re-picks it.
- The value is **rewritten continuously from an external source of truth** — e.g. a setting mirrored from a phone app or a CAN bus master. The next sync repairs any loss.
- The storage layer already provides transactional/journaled writes (e.g. a proper filesystem with atomic rename, or an FRAM part with genuinely atomic writes) — then the redundancy belongs at that layer, not re-implemented above it.

Portable ingredients if you re-apply this elsewhere: fixed field-by-field serialization (no struct padding), magic + version + CRC gate, monotonic seq with slot = seq parity, slot spacing that respects the storage's physical striping and leaves growth room, decode-both-pick-highest-valid-seq at boot, and a write cadence tied to meaningful change rather than time.

## Examples

All in `tests/test_dash_odo.c`, which runs on the host (`gcc -std=c11 -I MustangDash tests/test_dash_odo.c -lm`; on this Windows box, via WSL per `tests/run-tests.sh`).

**The torn-write recovery test** — the scenario the whole design exists for, labeled "THE POINT" in the source (`tests/test_dash_odo.c:150-179`). A valid slot-1 record at seq 5 holding 24,318.0 lifetime miles; a seq-6 write to slot 0 is torn mid-record (power dies after byte 9, the tail reads as erased 0xFF); the loader must take the survivor, losing only the pending tenth:

```c
    /* torn-write recovery (THE POINT): a power cut mid-write corrupts only
     * the slot being written; the previous slot must survive intact */
    {
        DashOdo a, out;
        uint8_t good[DASH_ODO_RECORD_SIZE], torn[DASH_ODO_RECORD_SIZE];
        dash_odo_init(&a);
        a.odo_tenths = 243180U; /* 24,318.0 mi lifetime */
        a.trip_tenths = 1426U;
        a.seq = 5U;
        dash_odo_encode(&a, good); /* seq 5 lives in slot 1 (5 % 2) */

        /* the torn seq-6 write: first half of the new record made it to
         * slot 0, the tail is stale garbage -> CRC fails */
        DashOdo b = a;
        b.odo_tenths = 243181U;
        uint8_t next_slot;
        dash_odo_encode_next(&b, torn, &next_slot);
        expect(next_slot == 0U, "seq 6 must target slot 0, away from the good record");
        memset(&torn[9], 0xFF, DASH_ODO_RECORD_SIZE - 9); /* power died here */

        dash_odo_init(&out);
        expect(dash_odo_pick_load_slot(torn, good, &out) == 1U,
               "torn slot 0 must lose to intact slot 1");
        expect(out.odo_tenths == 243180U,
               "lifetime odometer must survive a torn write (24318.0 mi)");
        expect(out.trip_tenths == 1426U, "trip must survive a torn write");
        expect(out.seq == 5U, "seq must reload from the surviving record");
        expect(dash_odo_next_write_slot(&out) == 0U,
               "the retry must overwrite the torn slot, not the survivor");
    }
```

Note the final assertion: after recovery, the *next* write targets the torn slot again — the survivor is never the next write target, so the system self-heals without a special repair path.

**The corruption sweep** (`tests/test_dash_odo.c:69-87`): every possible single-byte error — all 19 positions × all 255 non-zero xor masks, 4,845 corrupted records — must fail decode. This is what licenses the "any single corrupted byte is caught" claim behind the CRC-8/MAXIM choice:

```c
    for (int pos = 0; pos < DASH_ODO_RECORD_SIZE; pos++)
    {
        for (int x = 1; x <= 255; x++)
        {
            uint8_t bad[DASH_ODO_RECORD_SIZE];
            DashOdo scratch;
            memcpy(bad, rec, sizeof bad);
            bad[pos] ^= (uint8_t)x;
            dash_odo_init(&scratch);
            if (dash_odo_decode(&scratch, bad))
            {
                fprintf(stderr, "FAIL: single-byte error at %d (xor 0x%02X) not caught\n",
                        pos, x);
                failures++;
            }
        }
    }
```

**The equal-seq tiebreak pin** (`tests/test_dash_odo.c:213-230`): equal seq in both slots cannot happen through `dash_odo_encode_next`, but the loader's behavior is pinned anyway so recovery is deterministic rather than accidental:

```c
    /* equal-seq tiebreak (pathological, can't happen via encode_next):
     * deterministic -> slot 0 wins */
    {
        ...
        a.seq = 9U;
        dash_odo_encode(&a, rec0);
        ...
        b.seq = 9U;
        dash_odo_encode(&b, rec1);
        dash_odo_init(&out);
        expect(dash_odo_pick_load_slot(rec0, rec1, &out) == 0U,
               "equal seq must deterministically pick slot 0");
        expect(out.odo_tenths == 111U, "equal-seq tiebreak must load slot 0's fields");
    }
```

The suite also pins the layout itself with `_Static_assert`s (`tests/test_dash_odo.c:20-26`: record stays 19 bytes, slots stay at offsets 0 and 32, magic stays "MODO", version stays 2) — because firmware already in the field must keep decoding old bytes — plus the both-slots-invalid → 0xFF → zero-init path (`tests/test_dash_odo.c:181-194`) and the write cadence (`tests/test_dash_odo.c:274-293`: no write while creeping below a tenth, a write on each crossing, never while stationary).

## Related

- `docs/solutions/architecture-patterns/bt817-flash-resident-astc-assets.md` — the sibling torn-write defense from the same review round, on a different storage medium with a different mechanism: the panel's QSPI flash holds a single copy and relies on write ORDERING (the CRC header chunk as commit record, written last), while this pattern holds two copies and relies on slot REDUNDANCY (sequence counter + CRC picks the newest valid). Together they are this codebase's two answers to non-atomic persistent writes under power loss.
