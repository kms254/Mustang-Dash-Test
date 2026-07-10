/*
 * dash_odo.h - pure half of odometer persistence (unit U10, plan KTD7).
 *
 * Owns the EEPROM record layout, CRC, distance accumulation, and the write
 * cadence; the .ino supplies only the actual eeprom_read_block /
 * eeprom_write_block calls. No Arduino or EVE dependencies, so everything
 * here is host-testable (see tests/test_dash_odo.c) -- the splash_timeline.h
 * pattern. Distance math reuses dash_math.h's dash_odo_step so the tenths
 * convention (360000 mph*ms per tenth-mile) has exactly one definition.
 *
 * Record layout v2, serialized field-by-field (never rely on struct padding):
 *
 *   offset  0: magic       u32 LE  0x4F444F4D ("MODO")
 *   offset  4: version     u8      2
 *   offset  5: reserved    u8      0
 *   offset  6: odo_tenths  u32 LE  lifetime odometer, tenth-miles
 *   offset 10: trip_tenths u32 LE  trip meter, tenth-miles
 *   offset 14: seq         u32 LE  monotonic write counter
 *   offset 18: crc8                CRC-8/MAXIM over bytes 0..17
 *
 * Torn-write protection: the record is kept in TWO slots of the Teensy 4.1's
 * 4284-byte emulated EEPROM (DASH_ODO_SLOT_ADDR(0)=0, DASH_ODO_SLOT_ADDR(1)=32;
 * the 32-byte spacing keeps the slots in different emulation strides and
 * leaves room to grow). Each write bumps `seq` and lands in slot (seq % 2),
 * i.e. always the slot NOT holding the current record, so a power cut
 * mid-write (per-byte non-atomic flash writes, plus the emulation's
 * sector-compaction erase window) can only corrupt the slot being written --
 * the previous record survives and boot loads it via dash_odo_pick_load_slot
 * (higher valid seq wins; tie picks slot 0). Version-1 (15-byte, single-slot)
 * records fail decode; acceptable, v1 never shipped.
 *
 * Write cadence: one write per 0.1 mi crossing, no writes while stationary or
 * creeping below a tenth; `odo set` (reseed) writes immediately via
 * dirty_now.
 */

#ifndef DASH_ODO_H
#define DASH_ODO_H

#include <stdint.h>
#include <stdbool.h>

#include "dash_math.h" /* dash_odo_step / DASH_ODO_UMS_PER_TENTH */

#define DASH_ODO_MAGIC       0x4F444F4DU /* "MODO" when stored little-endian */
#define DASH_ODO_VERSION     2
#define DASH_ODO_RECORD_SIZE 19
#define DASH_ODO_SLOT_ADDR(i) ((i) * 32) /* slot 0 at 0, slot 1 at 32 */

/* Runtime odometer state. Only odo_tenths, trip_tenths, and seq persist; the
 * rest is in-RAM bookkeeping for accumulation and write scheduling. */
typedef struct {
    uint32_t odo_tenths;         /* lifetime distance, tenth-miles */
    uint32_t trip_tenths;        /* trip distance, tenth-miles */
    uint32_t seq;                /* monotonic write counter (persisted) */
    uint32_t rem_ums;            /* sub-tenth carry, mph*ms units */
    uint32_t tenths_since_write; /* tenths accumulated since last EEPROM write */
    bool dirty_now;              /* immediate-write demand (reseed / odo set) */
} DashOdo;

/* CRC-8/MAXIM (Dallas/1-Wire): poly 0x31 reflected -> 0x8C right-shift form,
 * init 0x00, no final xor. Check value: crc8("123456789") == 0xA1. Catches
 * every burst error of 8 bits or fewer, so any single corrupted record byte
 * fails decode. */
static inline uint8_t dash_odo_crc8(const uint8_t *p, uint32_t n)
{
    uint8_t crc = 0x00U;
    for (uint32_t i = 0U; i < n; i++)
    {
        crc ^= p[i];
        for (uint8_t bit = 0U; bit < 8U; bit++)
        {
            crc = (crc & 1U) ? (uint8_t)((crc >> 1) ^ 0x8CU)
                             : (uint8_t)(crc >> 1);
        }
    }
    return crc;
}

static inline void dash_odo_put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static inline uint32_t dash_odo_get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* Zero everything: the post-decode-failure baseline. */
static inline void dash_odo_init(DashOdo *o)
{
    o->odo_tenths = 0U;
    o->trip_tenths = 0U;
    o->seq = 0U;
    o->rem_ums = 0U;
    o->tenths_since_write = 0U;
    o->dirty_now = false;
}

/* Serialize the persisted fields into the fixed 19-byte v2 record. For an
 * actual EEPROM write use dash_odo_encode_next instead -- it bumps seq and
 * picks the ping-pong slot; raw encode is for tests and re-serialization. */
static inline void dash_odo_encode(const DashOdo *o, uint8_t out[DASH_ODO_RECORD_SIZE])
{
    dash_odo_put_u32le(&out[0], DASH_ODO_MAGIC);
    out[4] = (uint8_t)DASH_ODO_VERSION;
    out[5] = 0U; /* reserved */
    dash_odo_put_u32le(&out[6], o->odo_tenths);
    dash_odo_put_u32le(&out[10], o->trip_tenths);
    dash_odo_put_u32le(&out[14], o->seq);
    out[18] = dash_odo_crc8(out, 18U);
}

/* Parse a record read from EEPROM. Returns false -- leaving *o untouched --
 * on magic, version, or crc mismatch (blank/corrupt/torn EEPROM); the caller
 * keeps its dash_odo_init() zeros in that case. Version 2 only: v1 records
 * fail here (v1 never shipped). On success the runtime bookkeeping fields
 * start zeroed. */
static inline bool dash_odo_decode(DashOdo *o, const uint8_t in[DASH_ODO_RECORD_SIZE])
{
    if (dash_odo_get_u32le(&in[0]) != DASH_ODO_MAGIC) { return false; }
    if (in[4] != (uint8_t)DASH_ODO_VERSION) { return false; }
    if (in[18] != dash_odo_crc8(in, 18U)) { return false; }
    o->odo_tenths = dash_odo_get_u32le(&in[6]);
    o->trip_tenths = dash_odo_get_u32le(&in[10]);
    o->seq = dash_odo_get_u32le(&in[14]);
    o->rem_ums = 0U;
    o->tenths_since_write = 0U;
    o->dirty_now = false;
    return true;
}

/* Boot-time slot arbitration: decode both slots' raw bytes; the higher valid
 * seq wins (equal seq -- unreachable via encode_next -- deterministically
 * picks slot 0). Loads the winner into *out and returns its slot index, or
 * 0xFF with *out untouched when neither slot is valid (fresh EEPROM, or a
 * torn very first write): the caller keeps its dash_odo_init() zeros. */
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

/* Integrate one timestep. Delegates to dash_math.h's dash_odo_step: the
 * sub-tenth distance stays in integer mph*ms units (integer-exact per step,
 * so N small steps equal one big step) and whole tenths are extracted at
 * DASH_ODO_UMS_PER_TENTH. Returns the tenths added this call (usually 0). */
static inline uint32_t dash_odo_advance(DashOdo *o, float mph, uint32_t dt_ms)
{
    uint32_t tenths = dash_odo_step(mph, dt_ms, &o->rem_ums);
    o->odo_tenths += tenths;
    o->trip_tenths += tenths;
    o->tenths_since_write += tenths;
    return tenths;
}

/* Write cadence: persist when a tenth-mile boundary was crossed since the
 * last write, or when a reseed demands an immediate write. Never true while
 * stationary or creeping below a tenth -- EEPROM wear stays bounded at one
 * write per 0.1 mi. */
static inline bool dash_odo_should_write(DashOdo *o)
{
    return o->tenths_since_write > 0U || o->dirty_now;
}

/* Call after the EEPROM write actually happened. */
static inline void dash_odo_mark_written(DashOdo *o)
{
    o->tenths_since_write = 0U;
    o->dirty_now = false;
}

/* `odo set <miles>`: reseed the lifetime odometer (nearest tenth), preserve
 * the trip, drop the sub-tenth carry, and demand an immediate write. */
static inline void dash_odo_reseed(DashOdo *o, float miles)
{
    o->odo_tenths = (uint32_t)(miles * 10.0f + 0.5f);
    o->rem_ums = 0U;
    o->dirty_now = true;
}

/* Convenience readouts in display units. */
static inline float dash_odo_miles(const DashOdo *o)
{
    return (float)o->odo_tenths / 10.0f;
}

static inline float dash_trip_miles(const DashOdo *o)
{
    return (float)o->trip_tenths / 10.0f;
}

#endif /* DASH_ODO_H */
