/*
 * Invariant test: the generated splash flash-image address table
 * (MustangDash/splash_flash.h, emitted by tools/make_splash_flash.py) must
 * be internally consistent -- every asset inside the pack, aligned for
 * BITMAP_SOURCE flash addressing, non-overlapping, with strides matching
 * its ASTC block footprint -- and the embedded provisioning pack's header
 * bytes must agree with the #define'd constants the firmware compares
 * against on boot.
 *
 * Runs on the host (the header is self-contained: it carries #ifndef-guarded
 * EVE_ASTC_* defines and a PROGMEM no-op for non-Arduino builds):
 *   gcc -std=c11 -Wall -Wextra -Werror -I MustangDash \
 *       tests/test_splash_flash.c -o /tmp/tsf && /tmp/tsf
 */

#include <stdint.h>
#include <stdio.h>

#include "splash_flash.h"

static int failures = 0;

static void expect(int cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

/* square block edge for an ASTC format id (4x4, 6x6, 8x8 in use) */
static uint32_t astc_block(uint32_t fmt)
{
    if (fmt == EVE_ASTC_8X8) { return 8U; }
    if (fmt == EVE_ASTC_6X6) { return 6U; }
    return 4U;
}

/* bytes per block row for an ASTC format: ceil(w / block_w) * 16 */
static uint32_t astc_stride(uint32_t fmt, uint32_t w)
{
    uint32_t bw = astc_block(fmt);
    return ((w + bw - 1U) / bw) * 16U;
}

/* payload bytes: ceil(w/bw) * ceil(h/bh) * 16 */
static uint32_t astc_size(uint32_t fmt, uint32_t w, uint32_t h)
{
    uint32_t bw = astc_block(fmt);
    uint32_t bh = astc_block(fmt);
    return ((w + bw - 1U) / bw) * ((h + bh - 1U) / bh) * 16U;
}

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Independent CRC-32 (IEEE 802.3 / zlib / Python binascii.crc32): reflected,
 * poly 0xEDB88320, init 0xFFFFFFFF, final xor 0xFFFFFFFF. Bitwise, no table --
 * this exists specifically so a generator bug that CRCs the wrong byte range
 * (tools/make_splash_flash.py's build_pack() computes
 * binascii.crc32(bytes(body)) where body is every pack byte from offset 64
 * -- i.e. after the 64-byte header -- through the end of the sector-padded
 * pack) cannot stay self-consistent against a baked-in comparison value. */
static uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
        {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
            crc = (crc >> 1) ^ (0xEDB88320UL & mask);
        }
    }
    return crc ^ 0xFFFFFFFFUL;
}

int main(void)
{
    const uint32_t base = (uint32_t)SPLASH_FLASH_BASE;
    const uint32_t pack_size = (uint32_t)SPLASH_FLASH_PACK_SIZE;
    const uint32_t end = base + pack_size;
    const size_t count = sizeof(SPLASH_FLASH_ASSETS) / sizeof(SPLASH_FLASH_ASSETS[0]);
    char msg[160];

    /* pack-level invariants */
    expect(base == 4096UL, "image base must be flash address 4096 (sector 0 untouched)");
    expect((pack_size % 4096UL) == 0UL, "pack size must be a multiple of 4096 (CMD_FLASHUPDATE unit)");
    expect(SPLASH_FLASH_MAGIC == 0x4853444DUL, "magic constant must be LE 'MDSH'");
    expect(sizeof(splash_flash_pack) == SPLASH_FLASH_PACK_SIZE,
           "embedded pack array must be exactly PACK_SIZE bytes");
    expect(count == SPLASH_FA_COUNT, "asset table must have SPLASH_FA_COUNT rows");

    /* on-pack header bytes must agree with the #define'd constants */
    expect(splash_flash_pack[0] == 'M' && splash_flash_pack[1] == 'D' &&
           splash_flash_pack[2] == 'S' && splash_flash_pack[3] == 'H',
           "pack must start with the 'MDSH' magic bytes");
    expect(rd32le(&splash_flash_pack[0]) == SPLASH_FLASH_MAGIC,
           "pack magic u32 must equal SPLASH_FLASH_MAGIC");
    expect(rd32le(&splash_flash_pack[4]) == SPLASH_FLASH_VERSION,
           "pack version field must equal SPLASH_FLASH_VERSION");
    expect(rd32le(&splash_flash_pack[8]) == pack_size,
           "pack length field must equal SPLASH_FLASH_PACK_SIZE");
    expect(rd32le(&splash_flash_pack[12]) == (uint32_t)SPLASH_FA_COUNT,
           "pack asset-count field must equal SPLASH_FA_COUNT");
    expect(rd32le(&splash_flash_pack[16]) == SPLASH_FLASH_CRC,
           "pack CRC field must equal SPLASH_FLASH_CRC");

    /* Independent recomputation over the actual pack bytes, not just a second
     * copy of the value the generator baked from the same run. Per
     * tools/make_splash_flash.py build_pack(), the CRC covers every pack
     * byte after the 64-byte header through the end of the sector-padded
     * pack: pack[64 .. pack_size). */
    {
        uint32_t recomputed = crc32_ieee(&splash_flash_pack[64], pack_size - 64U);
        expect(recomputed == SPLASH_FLASH_CRC,
               "independently recomputed CRC-32 over pack[64..pack_size) must equal SPLASH_FLASH_CRC");
        expect(recomputed == rd32le(&splash_flash_pack[16]),
               "independently recomputed CRC-32 must equal the pack's on-disk CRC header field");
    }

    /* per-asset invariants: inside the pack, aligned, ascending, no overlap */
    uint32_t prev_end = base + 64U; /* assets start after the 64-byte header */
    for (size_t i = 0; i < count; i++)
    {
        const SplashFlashAsset *a = &SPLASH_FLASH_ASSETS[i];

        snprintf(msg, sizeof(msg), "asset %zu (%s): addr must be >= 4096", i, a->name);
        expect(a->addr >= 4096UL, msg);

        /* BITMAP_SOURCE flash addressing is 32-byte blocks; the guide's ASTC
         * flash layout section additionally requires 64-byte alignment */
        snprintf(msg, sizeof(msg), "asset %zu (%s): addr must be 64-byte aligned", i, a->name);
        expect((a->addr % 64UL) == 0UL, msg);

        snprintf(msg, sizeof(msg), "asset %zu (%s): must end inside the pack", i, a->name);
        expect(a->addr + a->size <= end, msg);

        snprintf(msg, sizeof(msg), "asset %zu (%s): must not overlap the previous asset", i, a->name);
        expect(a->addr >= prev_end, msg);
        prev_end = a->addr + a->size;

        snprintf(msg, sizeof(msg), "asset %zu (%s): fmt must be ASTC 4x4, 6x6 or 8x8", i, a->name);
        expect(a->fmt == EVE_ASTC_4X4 || a->fmt == EVE_ASTC_6X6 || a->fmt == EVE_ASTC_8X8, msg);

        snprintf(msg, sizeof(msg), "asset %zu (%s): size must match the block footprint", i, a->name);
        expect(a->size == astc_size(a->fmt, a->w, a->h), msg);

        snprintf(msg, sizeof(msg), "asset %zu (%s): stride must be ceil(w/bw)*16", i, a->name);
        expect(a->stride == astc_stride(a->fmt, a->w), msg);
    }

    /* the per-asset defines must mirror the table (spot-check the ones the
     * firmware theme table wires up by index) */
    expect(SPLASH_FLASH_ASSETS[SPLASH_FA_EMBLEM].addr == SPLASH_FA_EMBLEM_ADDR,
           "SPLASH_FA_EMBLEM_ADDR must match the table");
    expect(SPLASH_FLASH_ASSETS[SPLASH_FA_BG_BLUE].addr == SPLASH_FA_BG_BLUE_ADDR,
           "SPLASH_FA_BG_BLUE_ADDR must match the table");
    expect(SPLASH_FLASH_ASSETS[SPLASH_FA_BG_BLUE].fmt == EVE_ASTC_6X6,
           "backgrounds must be ASTC 6x6 (raised from 8x8 for gradient quality, 2026-07-21)");
    expect(SPLASH_FLASH_ASSETS[SPLASH_FA_EMBLEM].fmt == EVE_ASTC_4X4,
           "alpha elements must be ASTC 4x4");
    expect(SPLASH_FLASH_ASSETS[SPLASH_FA_BG_BLUE].w == 1024 &&
           SPLASH_FLASH_ASSETS[SPLASH_FA_BG_BLUE].h == 600,
           "backgrounds must be full-res 1024x600");

    if (failures == 0)
    {
        printf("test_splash_flash: OK (%zu assets, pack %lu bytes at %lu)\n",
               count, (unsigned long)pack_size, (unsigned long)base);
        return 0;
    }
    fprintf(stderr, "test_splash_flash: %d failure(s)\n", failures);
    return 1;
}
