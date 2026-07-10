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
 * Record layout, serialized field-by-field (never rely on struct padding):
 *
 *   offset  0: magic       u32 LE  0x4F444F4D ("MODO")
 *   offset  4: version     u8      1
 *   offset  5: reserved    u8      0
 *   offset  6: odo_tenths  u32 LE  lifetime odometer, tenth-miles
 *   offset 10: trip_tenths u32 LE  trip meter, tenth-miles
 *   offset 14: crc8                CRC-8/MAXIM over bytes 0..13
 *
 * The record lives at a fixed offset in the Teensy 4.1's 4284-byte emulated
 * EEPROM. Write cadence: one write per 0.1 mi crossing, no writes while
 * stationary or creeping below a tenth; `odo set` (reseed) writes
 * immediately via dirty_now.
 */

#ifndef DASH_ODO_H
#define DASH_ODO_H

#include <stdint.h>
#include <stdbool.h>

#include "dash_math.h" /* dash_odo_step / DASH_ODO_UMS_PER_TENTH */

#define DASH_ODO_MAGIC       0x4F444F4DU /* "MODO" when stored little-endian */
#define DASH_ODO_VERSION     1
#define DASH_ODO_RECORD_SIZE 15
#define DASH_ODO_EEPROM_ADDR 0

/* Runtime odometer state. Only odo_tenths and trip_tenths persist; the rest
 * is in-RAM bookkeeping for accumulation and write scheduling. */
typedef struct {
    uint32_t odo_tenths;         /* lifetime distance, tenth-miles */
    uint32_t trip_tenths;        /* trip distance, tenth-miles */
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
    o->rem_ums = 0U;
    o->tenths_since_write = 0U;
    o->dirty_now = false;
}

/* Serialize the persisted fields into the fixed 15-byte record. */
static inline void dash_odo_encode(const DashOdo *o, uint8_t out[DASH_ODO_RECORD_SIZE])
{
    dash_odo_put_u32le(&out[0], DASH_ODO_MAGIC);
    out[4] = (uint8_t)DASH_ODO_VERSION;
    out[5] = 0U; /* reserved */
    dash_odo_put_u32le(&out[6], o->odo_tenths);
    dash_odo_put_u32le(&out[10], o->trip_tenths);
    out[14] = dash_odo_crc8(out, 14U);
}

/* Parse a record read from EEPROM. Returns false -- leaving *o untouched --
 * on magic, version, or crc mismatch (blank/corrupt EEPROM); the caller keeps
 * its dash_odo_init() zeros in that case. On success the runtime bookkeeping
 * fields start zeroed. */
static inline bool dash_odo_decode(DashOdo *o, const uint8_t in[DASH_ODO_RECORD_SIZE])
{
    if (dash_odo_get_u32le(&in[0]) != DASH_ODO_MAGIC) { return false; }
    if (in[4] != (uint8_t)DASH_ODO_VERSION) { return false; }
    if (in[14] != dash_odo_crc8(in, 14U)) { return false; }
    o->odo_tenths = dash_odo_get_u32le(&in[6]);
    o->trip_tenths = dash_odo_get_u32le(&in[10]);
    o->rem_ums = 0U;
    o->tenths_since_write = 0U;
    o->dirty_now = false;
    return true;
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
