/*
 * Invariant test: the odometer persistence record (dash_odo.h) must keep its
 * fixed 15-byte layout, its CRC-8/MAXIM integrity check, and the plan-KTD7
 * write cadence (one write per 0.1 mi crossing, none while stationary or
 * creeping, immediate after `odo set`). Pins the pure half of unit U10 so the
 * .ino's eeprom_read_block/eeprom_write_block glue stays trivially thin.
 *
 * Runs on the host:
 *   gcc -std=c11 -I MustangDash tests/test_dash_odo.c -lm -o /tmp/tdo && /tmp/tdo
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "dash_odo.h"

/* record layout must never drift: firmware in the field decodes old bytes */
_Static_assert(DASH_ODO_RECORD_SIZE == 15, "record must stay 15 bytes");
_Static_assert(DASH_ODO_EEPROM_ADDR == 0, "record must stay at EEPROM offset 0");
_Static_assert(DASH_ODO_MAGIC == 0x4F444F4DU, "magic must stay 'MODO' (LE)");
_Static_assert(DASH_ODO_VERSION == 1, "record version must stay 1");
_Static_assert(DASH_ODO_UMS_PER_TENTH == 360000U,
               "odometer must keep dash_math.h's 360000 mph*ms per tenth");

static int failures = 0;

static void expect(int cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

int main(void)
{
    /* CRC-8/MAXIM check value: crc8("123456789") == 0xA1 */
    expect(dash_odo_crc8((const uint8_t *)"123456789", 9U) == 0xA1U,
           "crc8 must be CRC-8/MAXIM (check value 0xA1)");

    /* encode -> decode round trip preserves the persisted fields */
    DashOdo src, dst;
    uint8_t rec[DASH_ODO_RECORD_SIZE];
    dash_odo_init(&src);
    src.odo_tenths = 243180U;  /* 24,318.0 mi */
    src.trip_tenths = 1426U;   /*    142.6 mi */
    dash_odo_encode(&src, rec);
    expect(rec[0] == 0x4DU && rec[1] == 0x4FU && rec[2] == 0x44U && rec[3] == 0x4FU,
           "magic must serialize as 'MODO' little-endian");
    expect(rec[4] == 1U, "version byte must be 1");
    expect(rec[5] == 0U, "reserved byte must be 0");
    expect(rec[14] == dash_odo_crc8(rec, 14U), "crc byte must cover bytes 0..13");
    dash_odo_init(&dst);
    expect(dash_odo_decode(&dst, rec), "decode of a fresh encode must succeed");
    expect(dst.odo_tenths == 243180U, "decode must recover odo_tenths");
    expect(dst.trip_tenths == 1426U, "decode must recover trip_tenths");
    expect(dst.rem_ums == 0U && dst.tenths_since_write == 0U && !dst.dirty_now,
           "decode must leave runtime state zeroed");

    /* corruption: every possible single-byte error, at every position, must
     * be caught (magic/version checks or CRC-8, which detects all <=8-bit
     * bursts) */
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

    /* magic / version mismatch must fail even with a recomputed (valid) crc */
    {
        uint8_t bad[DASH_ODO_RECORD_SIZE];
        DashOdo scratch;
        memcpy(bad, rec, sizeof bad);
        bad[0] = 0x00U;                       /* break magic */
        bad[14] = dash_odo_crc8(bad, 14U);    /* ...but keep crc valid */
        dash_odo_init(&scratch);
        expect(!dash_odo_decode(&scratch, bad), "bad magic must fail even with valid crc");
        memcpy(bad, rec, sizeof bad);
        bad[4] = 2U;                          /* unknown version */
        bad[14] = dash_odo_crc8(bad, 14U);
        dash_odo_init(&scratch);
        expect(!dash_odo_decode(&scratch, bad), "unknown version must fail even with valid crc");
    }

    /* blank-EEPROM simulation: erased flash reads 0xFF everywhere */
    {
        uint8_t blank[DASH_ODO_RECORD_SIZE];
        DashOdo scratch;
        memset(blank, 0xFF, sizeof blank);
        dash_odo_init(&scratch);
        expect(!dash_odo_decode(&scratch, blank), "15 bytes of 0xFF must decode false");
        expect(scratch.odo_tenths == 0U && scratch.trip_tenths == 0U,
               "failed decode must leave the caller's zeroed state intact");
    }

    /* AE7: exactly 0.1 mi (60 mph for 6000 ms) advances one tenth */
    {
        DashOdo o, back;
        uint8_t buf[DASH_ODO_RECORD_SIZE];
        dash_odo_init(&o);
        expect(dash_odo_advance(&o, 60.0f, 6000U) == 1U,
               "60 mph for 6000 ms must return exactly 1 tenth");
        expect(o.odo_tenths == 1U, "odo must read 1 tenth after 0.1 mi");
        expect(o.trip_tenths == 1U, "trip must read 1 tenth after 0.1 mi");
        expect(o.rem_ums == 0U, "exact 0.1 mi must leave no remainder");
        dash_odo_encode(&o, buf);
        dash_odo_init(&back);
        expect(dash_odo_decode(&back, buf) && back.odo_tenths == 1U && back.trip_tenths == 1U,
               "advanced odometer must survive an encode/decode round trip");

        /* odo set: reseed keeps trip, zeroes remainder, forces a write */
        dash_odo_reseed(&o, 12000.0f);
        expect(o.odo_tenths == 120000U, "reseed(12000.0) must set odo to 120000 tenths");
        expect(o.trip_tenths == 1U, "reseed must preserve the trip");
        expect(o.rem_ums == 0U, "reseed must zero the remainder");
        expect(o.dirty_now, "reseed must set dirty_now");
        expect(dash_odo_should_write(&o), "reseed must demand an immediate write");
    }

    /* write cadence: one write per tenth crossing, never while creeping or
     * stationary */
    {
        DashOdo o;
        dash_odo_init(&o);
        expect(!dash_odo_should_write(&o), "fresh state must not want a write");
        expect(dash_odo_advance(&o, 30.0f, 1000U) == 0U, "sub-tenth creep must add 0 tenths");
        expect(!dash_odo_should_write(&o), "creeping below a tenth must not write");
        expect(o.rem_ums == 30000U, "creep distance must be carried in the remainder");
        /* 30 mph needs 12000 ms per tenth; 11000 ms more crosses it */
        expect(dash_odo_advance(&o, 30.0f, 11000U) == 1U, "crossing the tenth must add 1");
        expect(dash_odo_should_write(&o), "crossing a tenth must demand a write");
        dash_odo_mark_written(&o);
        expect(!dash_odo_should_write(&o), "mark_written must clear the write demand");
        expect(o.tenths_since_write == 0U, "mark_written must zero tenths_since_write");
        expect(dash_odo_advance(&o, 0.0f, 60000U) == 0U, "stationary must add 0 tenths");
        expect(!dash_odo_should_write(&o), "stationary must never write");
        expect(o.odo_tenths == 1U && o.trip_tenths == 1U,
               "cadence sequence must total exactly 1 tenth");
    }

    /* no drift: 600 x 10 ms steps at 60 mph == one 6000 ms step */
    {
        DashOdo small, big;
        uint32_t small_tenths = 0U;
        dash_odo_init(&small);
        dash_odo_init(&big);
        for (int i = 0; i < 600; i++)
        {
            small_tenths += dash_odo_advance(&small, 60.0f, 10U);
        }
        uint32_t big_tenths = dash_odo_advance(&big, 60.0f, 6000U);
        expect(small_tenths == big_tenths, "600x10ms and 1x6000ms must yield equal tenths");
        expect(small.odo_tenths == big.odo_tenths, "split steps must not drift the odometer");
        expect(small.rem_ums == big.rem_ums, "split steps must not drift the remainder");
    }

    /* convenience readouts */
    {
        DashOdo o;
        dash_odo_init(&o);
        o.odo_tenths = 243180U;
        o.trip_tenths = 1426U;
        expect(dash_odo_miles(&o) > 24317.99f && dash_odo_miles(&o) < 24318.01f,
               "dash_odo_miles must read 24318.0");
        expect(dash_trip_miles(&o) > 142.59f && dash_trip_miles(&o) < 142.61f,
               "dash_trip_miles must read 142.6");
    }

    if (failures == 0)
    {
        printf("OK: odometer record layout, crc, cadence, and drift invariants hold\n");
        return 0;
    }
    return 1;
}
